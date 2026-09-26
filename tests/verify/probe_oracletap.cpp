// W2-I's ATTACH-TIME probe.
//
// ★ WHAT IT ESTABLISHES, AND IT IS DELIBERATELY NARROW. E-7 will call into two functions inside the
// game's own image. Everything this packet knows about those five addresses -- the route table, the
// handler bodies, the rel32 call sites, the 20-byte prologues -- was read out of echovr.exe ON DISK.
// A file is not a process: the loader relocates, another plugin may have detoured a site before us,
// and a mod that patches the /session handler would leave the disk bytes untouched. This probe asks
// the only question a launch can answer today, at the only cost an attach-time launch has: are the
// five oracle sites, IN THE IMAGE THE LOADER MAPPED, still the bytes the registry recorded?
//
// ★ WHAT IT DOES NOT ESTABLISH. Nothing about O2, O3 or the tap's output. Those need a loaded match
// (HARNESS.md S3a names them explicitly as waiting for W1-F), and this probe asserts nothing that
// depends on engine state, so its result cannot be an accident of timing.
//
// ★ WHY A STATIC INITIALISER. W1-J puts several probe DLLs into plugins\ for one launch and W1-C's
// bootstrap refuses to run a second copy of the plugin in one process. Whichever probe wins the
// claim files a verdict; every other one would be silently inert and report NOTHING, which is
// indistinguishable from never having loaded. The verdict is therefore written from a static
// initialiser, which runs before DllMain and before any instance claim is attempted. Everything
// asserted here needs only the loader and one mapped image.
//
// ★ THE SITE TABLE IS THE SHIPPED ONE. The addresses come from oracletap::SiteFactId /
// SiteAddress, not from a list retyped here. A probe with its own copy of the table would keep
// passing after the tap started calling somewhere else.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "engine/oracletap.h"
#include "generated/facts_generated.h"

namespace {

size_t g_checked = 0;
size_t g_failed = 0;
char   g_detail[3072];

void Note(const char* text) {
    strncat_s(g_detail, sizeof g_detail, text, _TRUNCATE);
}

void Check(bool ok, const char* what) {
    ++g_checked;
    if (!ok) {
        ++g_failed;
        char one[240];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%sFAILED: %s", g_failed > 1 ? "; " : "", what);
        Note(one);
    }
}

// The registry records addresses against the base the binary declares; the loader may map it
// elsewhere. buildguard::MappedAddress is the tree's one rebasing point, but it belongs to a
// bootstrap that may have refused to run, so this derives the same thing from two facts and
// bounds-checks it against the mapped image.
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

struct SitePin {
    const char*    factId;
    uint64_t       va;
    const uint8_t* prologue;
};

// The prologue that belongs to each address. The addresses themselves come from the shipped table;
// this pairs each with its recorded bytes, and a site the tap adds without a pin here is REPORTED as
// unpinned rather than silently skipped -- a table that grew past its checks is the shape that lets
// a new call site arrive unguarded.
const SitePin kPins[] = {
    {"ORACLE.SESSION_BUILDER", facts::ORACLE_SESSION_BUILDER, facts::ORACLE_SESSION_BUILDER_PROLOGUE},
    {"ORACLE.SESSION_BUILDER_ALT", facts::ORACLE_SESSION_BUILDER_ALT,
     facts::ORACLE_SESSION_BUILDER_ALT_PROLOGUE},
    {"ORACLE.BONES_BUILDER", facts::ORACLE_BONES_BUILDER, facts::ORACLE_BONES_BUILDER_PROLOGUE},
    {"ORACLE.SESSION_ROUTE_HANDLER", facts::ORACLE_SESSION_ROUTE_HANDLER,
     facts::ORACLE_SESSION_ROUTE_HANDLER_PROLOGUE},
    {"ORACLE.BONES_ROUTE_HANDLER", facts::ORACLE_BONES_ROUTE_HANDLER,
     facts::ORACLE_BONES_ROUTE_HANDLER_PROLOGUE},
};
constexpr size_t kPinCount = sizeof kPins / sizeof kPins[0];

const SitePin* FindPin(uint64_t va) {
    for (size_t i = 0; i < kPinCount; ++i) {
        if (kPins[i].va == va) {
            return &kPins[i];
        }
    }
    return nullptr;
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

    // No REPLAY_PROBE_ROOT means this is not a sweep at all -- someone launched the game with this
    // DLL installed. Write nothing rather than scattering files into the install.
    if (rootLen == 0 || rootLen >= sizeof root) {
        return;
    }

    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, stem);
    CreateDirectoryA(dir, nullptr);

    char probeId[128] = "W2-I-sites";
    char ids[512] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    if (const char* found = strstr(ids, "probe_oracletap=")) {
        const char* value = found + strlen("probe_oracletap=");
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

struct AttachProbe {
    AttachProbe();
};

AttachProbe::AttachProbe() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&WriteVerdict), &self);

    const size_t siteCount = replay::oracletap::SiteCount();
    Check(siteCount == kPinCount, "every site in the shipped tap table has a prologue pin here");

    size_t matched = 0;
    for (size_t i = 0; i < siteCount; ++i) {
        const uint64_t va = replay::oracletap::SiteAddress(i);
        const char* id = replay::oracletap::SiteFactId(i);
        const SitePin* pin = FindPin(va);

        char what[192];
        _snprintf_s(what, sizeof what, _TRUNCATE, "%s (0x%llX) has a recorded prologue",
                    id != nullptr ? id : "?", (unsigned long long)va);
        Check(pin != nullptr, what);
        if (pin == nullptr) {
            continue;
        }

        const uint8_t* site = MappedGameAddress(va, 21);
        _snprintf_s(what, sizeof what, _TRUNCATE, "%s resolves inside the mapped game image",
                    pin->factId);
        Check(site != nullptr, what);
        if (site == nullptr) {
            continue;
        }

        _snprintf_s(what, sizeof what, _TRUNCATE,
                    "the mapped bytes at %s are the recorded prologue (no foreign detour)",
                    pin->factId);
        const bool ok = memcmp(site, pin->prologue, 20) == 0;
        Check(ok, what);
        if (ok) {
            ++matched;
        }

        // The known-answer control. The same comparison one byte along MUST fail; without it a
        // memcmp against a zeroed expectation, or against a site that reads as all-zero, would look
        // exactly like a clean match. A negative result nobody constructed is not a control.
        _snprintf_s(what, sizeof what, _TRUNCATE,
                    "the same comparison one byte along at %s refuses (known-answer control)",
                    pin->factId);
        Check(memcmp(site + 1, pin->prologue, 20) != 0, what);
    }

    // Evidence, not assertions. rlay.dll's load state is recorded because the prologue results only
    // mean what they say alongside it.
    const bool rlay = GetModuleHandleA("rlay.dll") != nullptr;
    char note[900];
    _snprintf_s(note, sizeof note, _TRUNCATE,
                "%sverdict written from a static initialiser, before DllMain and before any instance "
                "claim. %llu of %llu oracle site(s) carry their recorded prologue in the MAPPED "
                "image. rlay=%s",
                g_failed ? " | " : "", (unsigned long long)matched, (unsigned long long)siteCount,
                rlay ? "loaded" : "not-loaded");
    Note(note);
    Note(" | ESTABLISHES: the five addresses E-7 depends on are, in the image the loader mapped and "
         "under this rlay.dll condition, the functions the registry recorded from the file on disk -- "
         "each with a one-byte-offset known-answer control. DOES NOT ESTABLISH: the builders' "
         "PROTOTYPES (integration item V-I-W2I1), that either builder can be called, or anything at "
         "all about O2 or O3, which need a loaded match and wait for W1-F.");

    WriteVerdict(self);
}

const AttachProbe g_attachProbe;

}  // namespace
