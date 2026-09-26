// W1-D's ATTACH-TIME probe.
//
// ★ WHY THIS FILE HAS NO REPLAY_MODULE, AND WHY THE WORK IS IN A STATIC INITIALISER.
//
// Sweep 4 spent two launches on this probe and collected nothing. The cause was not the probe's
// checks and not the `attach` declaration: it was `coexist::ClaimInstance`. W1-J puts several probe
// DLLs into `plugins\` for one launch, every one of them embeds the same W1-C bootstrap, and that
// bootstrap refuses to run a SECOND copy of the plugin in one process -- keyed on the resolved log
// path plus the pid, precisely so two copies cannot both claim a CNSProvider slot. It is correct, it
// is protecting the thing this packet owns, and it is not going to change.
//
//     19:31:13.392 [boot]    dll=...\plugins\probe_tickattach.dll
//     19:31:13.392 [boot]    2 module(s) self-registered before DllMain ran
//     19:31:13.392 [coexist] another module IN THIS PROCESS has already claimed ...echoreplay.log
//                            ... refusing, so the two do not both claim a provider slot
//
// `Attach()` then returns false, the worker never starts, `modreg::InitAll` is never called, and a
// verdict written from a module's Init is never written at all. Whichever probe DLL the loader reaches
// first wins the claim and files; every other one is silently inert.
//
// ★ So the verdict is written from a STATIC INITIALISER, which the log line above proves runs before
// DllMain and therefore before the claim is even attempted. That makes the completion point real
// rather than declared: this probe files whether or not the bootstrap decides to run, and it files
// whether it passed, failed, or could not look.
//
// Everything it asserts needs only the loader and two mapped images -- no engine state, no bootstrap,
// no buildguard, no log. Under the loader lock we may read memory and write a file, which is exactly
// and only what this does: no LoadLibrary, no thread, no wait.
//
// ★ WHAT IT ESTABLISHES. The slot claim's safety rests on the engine only ever resolving `Update` and
// `RadPluginShutdown` against our module. Offline that is a census of all 21 slot references in four
// functions. Here it is tested the way the engine tests it: the real GetProcAddress, against the real
// loaded image, for all eleven CNSProvider names, each of which must come back null. The design's
// canary only fires after a wrong lookup has already happened; this asks first.
//
// ★ WHAT IT DOES NOT ESTABLISH. Nothing about the tick. One tick per rendered frame, the delta, and
// which slot is free all need CR15Game::Initialize to have run and the frame loop to be turning.
// Those are W1-D-match.probe.json and wait for a profile that reaches a frame loop.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "generated/facts_generated.h"

namespace {

const char* const kForbidden[] = {
    "AllowGuests", "InitGlobals", "ProviderID", "Social", "UserProviderID", "Users",
    "MicStart", "MicStop", "MicDestroy", "VoipDestroyEncoder", "VoipDestroyDecoder",
};
constexpr size_t kForbiddenCount = sizeof kForbidden / sizeof kForbidden[0];

size_t g_checked = 0;
size_t g_failed = 0;
char   g_detail[3072];

void Note(const char* text) { strncat_s(g_detail, sizeof g_detail, text, _TRUNCATE); }

void Check(bool ok, const char* what) {
    ++g_checked;
    if (!ok) {
        ++g_failed;
        char one[220];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%sFAILED: %s", g_failed > 1 ? "; " : "", what);
        Note(one);
    }
}

// The registry records addresses against the base the binary declares. buildguard::MappedAddress is
// the tree's one rebasing point, but it belongs to a bootstrap that may have refused to run, so this
// probe derives the same thing from two facts and bounds-checks it against the mapped image.
const uint8_t* MappedGameAddress(uint64_t recordedVa, size_t bytes) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return nullptr;
    }
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    const uint64_t recordedBase = facts::GAME_SINGLETON_VA - facts::GAME_SINGLETON_RVA;
    if (recordedVa < recordedBase) {
        return nullptr;
    }
    const uint64_t rva = recordedVa - recordedBase;
    if (rva + bytes > nt->OptionalHeader.SizeOfImage) {
        return nullptr;
    }
    return base + rva;
}

// Our own export directory, read out of the image the loader mapped rather than the file on disk.
size_t MappedExportCount(HMODULE self, bool* sawUpdate, bool* sawShutdown) {
    *sawUpdate = false;
    *sawShutdown = false;
    if (self == nullptr) {
        return 0;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(self);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return 0;
    }
    const IMAGE_EXPORT_DIRECTORY* exp =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const uint32_t* names = reinterpret_cast<const uint32_t*>(base + exp->AddressOfNames);
    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (strcmp(name, "Update") == 0) {
            *sawUpdate = true;
        } else if (strcmp(name, "RadPluginShutdown") == 0) {
            *sawShutdown = true;
        }
    }
    return exp->NumberOfNames;
}

void WriteVerdict(HMODULE self) {
    char root[MAX_PATH] = {0};
    const DWORD rootLen = GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);

    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(self, dll, sizeof dll);
    const char* leaf = strrchr(dll, '\\');
    leaf = leaf != nullptr ? leaf + 1 : dll;
    char stem[MAX_PATH] = {0};
    strncpy_s(stem, sizeof stem, leaf, _TRUNCATE);
    if (char* dot = strrchr(stem, '.')) {
        *dot = '\0';
    }

    // No REPLAY_PROBE_ROOT means we are not in a sweep at all -- a developer double-clicked the game
    // with this DLL installed. Write nothing rather than scattering files into the install.
    if (rootLen == 0 || rootLen >= sizeof root) {
        return;
    }

    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, stem);
    CreateDirectoryA(dir, nullptr);

    char probeId[128] = "W1-D-attach";
    char ids[512] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    if (const char* found = strstr(ids, "probe_tickattach=")) {
        const char* value = found + strlen("probe_tickattach=");
        size_t n = strcspn(value, ";");
        if (n >= sizeof probeId) {
            n = sizeof probeId - 1;
        }
        memcpy(probeId, value, n);
        probeId[n] = '\0';
    }

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2] = {0};
    for (size_t i = 0, o = 0; cwd[i] != '\0' && o + 2 < sizeof cwdEscaped; ++i) {
        if (cwd[i] == '\\') {
            cwdEscaped[o++] = '\\';
        }
        cwdEscaped[o++] = cwd[i];
    }

    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", dir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "wb") == 0 && fh != nullptr) {
        fprintf(fh,
                "{ \"probe\": \"%s\", \"verdict\": \"%s\", \"checked\": %llu, \"cwd\": \"%s\",\n"
                "  \"detail\": \"%s\" }\n",
                probeId, g_failed == 0 ? "PASS" : "FAIL", (unsigned long long)g_checked, cwdEscaped,
                g_detail);
        fclose(fh);
    }
}

// Runs from a static initialiser, before DllMain and before any instance claim is attempted.
struct AttachProbe {
    AttachProbe();
};

AttachProbe::AttachProbe() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&WriteVerdict), &self);

    // The two names the engine WILL ask for. GetProcAddress is literally what CNSProvider::Update and
    // CNSProvider::Destroy call, so this is the engine's own question asked early.
    Check(self != nullptr && GetProcAddress(self, "Update") != nullptr,
          "GetProcAddress(self, Update) resolves");
    Check(self != nullptr && GetProcAddress(self, "RadPluginShutdown") != nullptr,
          "GetProcAddress(self, RadPluginShutdown) resolves");

    // The eleven it MIGHT ask for must not. A no-op returning 0 would be indistinguishable from a valid
    // provider id; null keeps a mistaken lookup loud (design S2 F8b).
    for (size_t i = 0; i < kForbiddenCount; ++i) {
        char what[128];
        _snprintf_s(what, sizeof what, _TRUNCATE, "GetProcAddress(self, %s) is null", kForbidden[i]);
        Check(self != nullptr && GetProcAddress(self, kForbidden[i]) == nullptr, what);
    }

    bool sawUpdate = false;
    bool sawShutdown = false;
    const size_t exports = MappedExportCount(self, &sawUpdate, &sawShutdown);
    Check(exports == 2, "the mapped export table holds exactly two names");
    Check(sawUpdate && sawShutdown, "the mapped export table holds Update and RadPluginShutdown");

    // Our Level 1 fallback site, read from the image the loader mapped. Run on `bare` (rlay.dll
    // present) and `bare-norlay` (moved aside), identical and correct bytes in both is the in-vivo
    // form of W1-C's empty-intersection finding, taken at the one address we would ever patch.
    const size_t prologueLen = sizeof facts::NETGAME_UPDATE_PROLOGUE;
    const uint8_t* site = MappedGameAddress(facts::NETGAME_UPDATE, prologueLen + 1);
    Check(site != nullptr, "NETGAME_UPDATE resolves inside the mapped game image");
    Check(site != nullptr && memcmp(site, facts::NETGAME_UPDATE_PROLOGUE, prologueLen) == 0,
          "the mapped bytes at NETGAME_UPDATE are the recorded prologue (no foreign detour)");
    Check(site == nullptr || memcmp(site + 1, facts::NETGAME_UPDATE_PROLOGUE, prologueLen) != 0,
          "the same comparison one byte along refuses (known-answer control)");

    Check(MappedGameAddress(facts::GAME_SINGLETON_VA, sizeof(void*)) != nullptr,
          "GAME_SINGLETON_VA resolves inside the mapped game image");
    Check(MappedGameAddress(facts::CLOCK_GLOBAL_PTR, sizeof(void*)) != nullptr,
          "CLOCK_GLOBAL_PTR resolves inside the mapped game image");

    // Evidence, not assertions. rlay.dll's load state is recorded because the prologue result above
    // only means what it says alongside it; the singleton state is recorded because how early
    // CR15Game::Initialize runs is unmeasured, and it is a race, so it must not gate a verdict.
    const bool rlay = GetModuleHandleA("rlay.dll") != nullptr;
    const uint8_t* cell = MappedGameAddress(facts::GAME_SINGLETON_VA, sizeof(void*));
    const uint8_t* game =
        cell != nullptr ? *reinterpret_cast<const uint8_t* const volatile*>(cell) : nullptr;

    char note[700];
    if (game == nullptr) {
        _snprintf_s(note, sizeof note, _TRUNCATE,
                    "%sverdict written from a static initialiser, before DllMain and before any "
                    "instance claim. CR15Game singleton still null at this point, as expected; slot "
                    "state is not observable here. rlay=%s exports=%llu",
                    g_failed ? " | " : "", rlay ? "loaded" : "not-loaded",
                    (unsigned long long)exports);
    } else {
        _snprintf_s(note, sizeof note, _TRUNCATE,
                    "%sverdict written from a static initialiser, before DllMain. CR15Game already at "
                    "%p: A=%p B=%p C=%p. rlay=%s exports=%llu",
                    g_failed ? " | " : "", static_cast<const void*>(game),
                    *reinterpret_cast<void* const volatile*>(game + facts::GAME_PROVIDER_SLOT_A),
                    *reinterpret_cast<void* const volatile*>(game + facts::GAME_PROVIDER_SLOT_B),
                    *reinterpret_cast<void* const volatile*>(game + facts::GAME_PROVIDER_SLOT_C),
                    rlay ? "loaded" : "not-loaded", (unsigned long long)exports);
    }
    Note(note);
    Note(" | ESTABLISHES: every name the engine can resolve against our module, asked with the engine's "
         "own GetProcAddress against the real loaded image; and our Level 1 fallback site's bytes under "
         "this rlay.dll condition, with a known-answer control. DOES NOT ESTABLISH: anything about the "
         "tick -- one tick per frame, the delta, and which slot is free all need a running frame loop "
         "and are queued separately as W1-D-match.");

    WriteVerdict(self);
}

const AttachProbe g_attachProbe;

}  // namespace
