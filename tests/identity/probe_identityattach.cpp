// W2-C's ATTACH-TIME probe.  Everything here is answered before the engine has initialised,
// so it runs on `bare` / `bare-norlay` / `bare-level` today rather than waiting for `flat`.
//
// ★ WHAT IT IS FOR, and why the on-disk check is not enough.
//
// tests\identity\sitecheck.py reads every one of this packet's sites out of echovr.exe ON DISK
// and cross-checks it against the constants the shipped code compiles against.  That says
// nothing about what is in MEMORY: W1-C's first real sweep was refused because rlay.dll had
// already detoured a guarded site, and a detour lands in the mapped image, not in the file.
//
// The cosmetic path calls nine engine functions.  If any of them is detoured, this packet
// applies a loadout through somebody else's code and reports success -- which is precisely the
// silent-wrong-avatar failure the whole packet exists to remove.  Finding that out costs one
// attach-time read.
//
// ★ WHAT IT DOES NOT ESTABLISH: anything needing a CR15NetGame.  The slot tables, the profile
// CJson, the live loadout table and the customisation records are all engine state, and every
// claim about them is queued as W2-C-match.probe.json, which is NOT in the queue while `flat`
// is refused -- a probe that cannot be planned makes the whole sweep unplannable for everyone.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "bootstrap/buildguard.h"
#include "bootstrap/log.h"
#include "bootstrap/services.h"
#include "engine/identity_access.h"
#include "engine/identity_access_facts.h"
#include "generated/facts_generated.h"

namespace {

// L1: does a recorded VA, dereferenced WITHOUT rebasing, fault in this process?
//
// Everything else in this probe reads through buildguard::MappedAddress, which is why it could
// report every site green while the shipped module read addresses that are not in the process
// at all -- the control was on the correct side of the defect.  This is the one read that is
// deliberately on the WRONG side, so the artifact records the answer rather than assuming it.
// Its own function because __try may not share a frame with unwindable C++ objects.
bool LiteralDerefFaults(uint64_t va, const uint8_t* want, size_t n, int* cmpOut) {
    __try {
        *cmpOut = memcmp(reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), want, n);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

const char kReplayModuleName[] = "w2c-attach";

size_t g_checked = 0;
size_t g_failed = 0;
char g_detail[3072];

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

void CheckSite(uint64_t va, const uint8_t* want, size_t n, const char* label) {
    const void* p = replay::buildguard::MappedAddress(va);
    char what[192];
    _snprintf_s(what, sizeof what, _TRUNCATE, "%s resolves inside the mapped image", label);
    Check(p != nullptr, what);
    if (p == nullptr) return;
    _snprintf_s(what, sizeof what, _TRUNCATE, "%s: mapped bytes are the recorded ones (no detour)",
                label);
    Check(memcmp(p, want, n) == 0, what);
}

struct Site {
    uint64_t va;
    const uint8_t* want;
    size_t n;
    const char* label;
};

void WriteVerdict(HMODULE self) {
    char root[MAX_PATH] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);
    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(self, dll, sizeof dll);
    const char* leaf = strrchr(dll, '\\');
    leaf = leaf != nullptr ? leaf + 1 : dll;
    char stem[MAX_PATH] = {0};
    strncpy_s(stem, sizeof stem, leaf, _TRUNCATE);
    if (char* dot = strrchr(stem, '.')) *dot = '\0';
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, stem);
    CreateDirectoryA(dir, nullptr);

    char probeId[128] = "W2-C-attach";
    char ids[512] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    if (const char* found = strstr(ids, "probe_identityattach=")) {
        const char* value = found + strlen("probe_identityattach=");
        size_t n = strcspn(value, ";");
        if (n >= sizeof probeId) n = sizeof probeId - 1;
        memcpy(probeId, value, n);
        probeId[n] = '\0';
    }

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2] = {0};
    for (size_t i = 0, o = 0; cwd[i] != '\0' && o + 2 < sizeof cwdEscaped; ++i) {
        if (cwd[i] == '\\') cwdEscaped[o++] = '\\';
        cwdEscaped[o++] = cwd[i];
    }

    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", dir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "wb") == 0 && fh != nullptr) {
        fprintf(fh,
                "{ \"probe\": \"%s\", \"verdict\": \"%s\", \"checked\": %llu, \"cwd\": \"%s\",\n"
                "  \"detail\": \"%s\" }\n",
                probeId, g_failed == 0 ? "PASS" : "FAIL", (unsigned long long)g_checked,
                cwdEscaped, g_detail);
        fclose(fh);
    }
    replay::log::Direct(g_failed == 0 ? replay::LogLevel::Info : replay::LogLevel::Error,
                        kReplayModuleName, "%s (%llu checked): %s", g_failed == 0 ? "PASS" : "FAIL",
                        (unsigned long long)g_checked, g_detail);
}

bool Init(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&Init), &self);

    using namespace replay::identity_facts;

    // ---- 0. The slide, and L1: the un-rebased dereference. --------------------------------
    // Recorded first because every other line here depends on it.  echovr.exe is DYNAMICBASE +
    // HIGH_ENTROPY_VA, so `slide` is expected to be large and non-zero; if it is zero this run
    // happened to load at the preferred base and every check below is blind to C-1's class.
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const long long slide =
            static_cast<long long>(base) - static_cast<long long>(facts::PE_IMAGE_BASE);
        int cmp = 0;
        const bool faulted = LiteralDerefFaults(facts::IDENT_APPLY_LOADOUT_TO_ACTOR,
                                                facts::IDENT_APPLY_LOADOUT_TO_ACTOR_PROLOGUE, 20,
                                                &cmp);
        char one[320];
        _snprintf_s(one, sizeof one, _TRUNCATE,
                    "L1: main image base 0x%llX, slide 0x%llX; the literal 0x%llX %s",
                    (unsigned long long)base, (unsigned long long)slide,
                    (unsigned long long)facts::IDENT_APPLY_LOADOUT_TO_ACTOR,
                    faulted ? "FAULTED when dereferenced un-rebased"
                            : (cmp == 0 ? "happened to hold the recorded bytes"
                                        : "was readable but held other bytes"));
        Note(one);
        Note(" | ");
        // Not a Check(): whether the literal faults is an OBSERVATION about this run, and
        // failing the probe on it would report a defect in echovr.exe rather than in us.
    }

    // ---- 0b. The shipped module's own table, not a parallel copy of it. --------------------
    // The audit's point: this probe rebased while identity_access.cpp did not, so the control
    // could never see the defect.  Walking the module's OWN resolved sites removes that gap --
    // if the module stops rebasing, these go null or wrong here.
    {
        Check(replay::identity_access::BuildMatches(),
              "identity_access::BuildMatches() accepts every site it will call");
        const size_t n = replay::identity_access::EngineSiteCount();
        Check(n == 14, "identity_access publishes all fourteen of its engine sites");
        for (size_t i = 0; i < n; ++i) {
            const void* p = replay::identity_access::EngineSiteResolved(i);
            const uint64_t va = replay::identity_access::EngineSiteVa(i);
            char what[220];
            _snprintf_s(what, sizeof what, _TRUNCATE,
                        "identity_access resolved %s through the mapped image",
                        replay::identity_access::EngineSiteName(i));
            Check(p != nullptr && p == replay::buildguard::MappedAddress(va), what);
        }
        if (!replay::identity_access::BuildMatches()) {
            char one[320];
            _snprintf_s(one, sizeof one, _TRUNCATE, "BuildMismatchDetail: %s | ",
                        replay::identity_access::BuildMismatchDetail());
            Note(one);
        }
    }

    // ---- 1. Known-answer control, FIRST. --------------------------------------------------
    // A memcmp that always agreed would pass every check below and report a green it had not
    // earned.  This one deliberately expects a byte string the site cannot hold, so a zero
    // here means the comparison is inert and the whole verdict is void.
    {
        uint8_t wrong[20];
        memcpy(wrong, kOVERRIDE_ALIGNMENTPrologue, sizeof wrong);
        wrong[0] = static_cast<uint8_t>(~wrong[0]);
        const void* p = replay::buildguard::MappedAddress(kOVERRIDE_ALIGNMENT);
        Check(p != nullptr && memcmp(p, wrong, sizeof wrong) != 0,
              "CONTROL: a deliberately wrong prologue does NOT match (the comparison is live)");
    }

    // ---- 2. The nine entry points the cosmetic path CALLS. --------------------------------
    // A detour on any of these means this packet dresses a player through somebody else's
    // code and cannot tell.  These are the rows identity_access::BuildMatches() checks at
    // run time; the probe checks them before a match is ever loaded.
    static const Site kCalled[] = {
        {facts::IDENT_APPLY_LOADOUT_TO_ACTOR, facts::IDENT_APPLY_LOADOUT_TO_ACTOR_PROLOGUE,
         sizeof facts::IDENT_APPLY_LOADOUT_TO_ACTOR_PROLOGUE, "IDENT_APPLY_LOADOUT_TO_ACTOR"},
        {facts::IDENT_CUSTOMISATION_ALIGNMENT, facts::IDENT_CUSTOMISATION_ALIGNMENT_PROLOGUE,
         sizeof facts::IDENT_CUSTOMISATION_ALIGNMENT_PROLOGUE, "IDENT_CUSTOMISATION_ALIGNMENT"},
        {kACTOR_COMPONENT_REF, kACTOR_COMPONENT_REFPrologue,
         sizeof kACTOR_COMPONENT_REFPrologue, "IDENT_ACTOR_COMPONENT_REF"},
        {kOVERRIDE_ALIGNMENT, kOVERRIDE_ALIGNMENTPrologue,
         sizeof kOVERRIDE_ALIGNMENTPrologue, "IDENT_OVERRIDE_ALIGNMENT"},
        {kAPPLY_ALIGNMENT_CHANGE, kAPPLY_ALIGNMENT_CHANGEPrologue,
         sizeof kAPPLY_ALIGNMENT_CHANGEPrologue, "IDENT_APPLY_ALIGNMENT_CHANGE"},
        {kAPPLY_ALIGNMENT_ALL_USERS, kAPPLY_ALIGNMENT_ALL_USERSPrologue,
         sizeof kAPPLY_ALIGNMENT_ALL_USERSPrologue, "IDENT_APPLY_ALIGNMENT_ALL_USERS"},
        {kJSON_IS_EMPTY, kJSON_IS_EMPTYPrologue,
         sizeof kJSON_IS_EMPTYPrologue, "IDENT_JSON_IS_EMPTY"},
        {kJSON_SET_STRING, kJSON_SET_STRINGPrologue,
         sizeof kJSON_SET_STRINGPrologue, "IDENT_JSON_SET_STRING"},
        {kJSON_SET_INT, kJSON_SET_INTPrologue,
         sizeof kJSON_SET_INTPrologue, "IDENT_JSON_SET_INT"},
        {kJSON_GET_STRING, kJSON_GET_STRINGPrologue,
         sizeof kJSON_GET_STRINGPrologue, "IDENT_JSON_GET_STRING"},
    };
    for (const Site& s : kCalled) CheckSite(s.va, s.want, s.n, s.label);

    // ---- 3. The functions this packet REASONS ABOUT but never calls. -----------------------
    // A detour here does not corrupt our writes; it invalidates the reading that says our
    // writes are enough -- the more expensive failure, because nothing else would report it.
    // ApplyLoadoutItems is the body carrying breaker 1's early return; ApplyCurrentLoadout is
    // what OverrideAlignment tail-calls into; UserDisplayName is what decides whether the
    // recorded name is printed at all; NetGameAlignment is the team-colour derivation.
    static const Site kReasoned[] = {
        {facts::IDENT_APPLY_LOADOUT_ITEMS, facts::IDENT_APPLY_LOADOUT_ITEMS_PROLOGUE,
         sizeof facts::IDENT_APPLY_LOADOUT_ITEMS_PROLOGUE, "IDENT_APPLY_LOADOUT_ITEMS"},
        {facts::IDENT_LOAD_AND_APPLY_ITEM, facts::IDENT_LOAD_AND_APPLY_ITEM_PROLOGUE,
         sizeof facts::IDENT_LOAD_AND_APPLY_ITEM_PROLOGUE, "IDENT_LOAD_AND_APPLY_ITEM"},
        {facts::IDENT_NETGAME_ALIGNMENT, facts::IDENT_NETGAME_ALIGNMENT_PROLOGUE,
         sizeof facts::IDENT_NETGAME_ALIGNMENT_PROLOGUE, "IDENT_NETGAME_ALIGNMENT"},
        {facts::IDENT_SET_LOADOUT_NUMBER, facts::IDENT_SET_LOADOUT_NUMBER_PROLOGUE,
         sizeof facts::IDENT_SET_LOADOUT_NUMBER_PROLOGUE, "IDENT_SET_LOADOUT_NUMBER"},
        {facts::IDENT_GEN_BOT_PROFILE, facts::IDENT_GEN_BOT_PROFILE_PROLOGUE,
         sizeof facts::IDENT_GEN_BOT_PROFILE_PROLOGUE, "IDENT_GEN_BOT_PROFILE"},
        {facts::IDENT_GEN_BOT_LOADOUT, facts::IDENT_GEN_BOT_LOADOUT_PROLOGUE,
         sizeof facts::IDENT_GEN_BOT_LOADOUT_PROLOGUE, "IDENT_GEN_BOT_LOADOUT"},
        {kAPPLY_CURRENT_LOADOUT, kAPPLY_CURRENT_LOADOUTPrologue,
         sizeof kAPPLY_CURRENT_LOADOUTPrologue, "IDENT_APPLY_CURRENT_LOADOUT"},
        {kUSER_DISPLAY_NAME, kUSER_DISPLAY_NAMEPrologue,
         sizeof kUSER_DISPLAY_NAMEPrologue, "IDENT_USER_DISPLAY_NAME"},
        {kLOADOUT_ASSIGN, kLOADOUT_ASSIGNPrologue,
         sizeof kLOADOUT_ASSIGNPrologue, "IDENT_LOADOUT_ASSIGN"},
    };
    for (const Site& s : kReasoned) CheckSite(s.va, s.want, s.n, s.label);

    // ---- 4. The two byte-shape windows. ---------------------------------------------------
    // These are not entry points.  They are the evidence that the 48-byte customisation slot
    // stride and the two ActorComponentRef field positions -- neither of which any single
    // instruction encodes -- are still what identity_access.cpp's hand-declared structs assume.
    static const Site kShapes[] = {
        {kCUST_SLOT_WALK_BYTESVa, kCUST_SLOT_WALK_BYTES, sizeof kCUST_SLOT_WALK_BYTES,
         "IDENT_CUST_SLOT_WALK_BYTES (the 3<<4 slot stride)"},
        {kACTOR_REF_CS_BYTESVa, kACTOR_REF_CS_BYTES, sizeof kACTOR_REF_CS_BYTES,
         "IDENT_ACTOR_REF_CS_BYTES (the component-system field at +0x40)"},
        {kACTOR_REF_HANDLE_BYTESVa, kACTOR_REF_HANDLE_BYTES, sizeof kACTOR_REF_HANDLE_BYTES,
         "IDENT_ACTOR_REF_HANDLE_BYTES (the handle at +0x48)"},
    };
    for (const Site& s : kShapes) CheckSite(s.va, s.want, s.n, s.label);

    char note[640];
    _snprintf_s(note, sizeof note, _TRUNCATE,
                "%s%llu checks over 22 sites read from the mapped image; rlay=%s",
                g_failed ? " | " : "", (unsigned long long)g_checked,
                GetModuleHandleA("rlay.dll") ? "present" : "absent");
    Note(note);
    Note(" | ESTABLISHES: the ten entry points W2-C calls, the nine it reasons about and the "
         "three byte-shape windows its hand-declared structs rest on, as they stand IN MEMORY "
         "rather than on disk, plus a known-answer control proving the comparison is live. "
         "DOES NOT ESTABLISH: anything needing a CR15NetGame -- the slot tables, the profile "
         "CJson, the live loadout table and the customisation records are all W2-C-match.");

    WriteVerdict(self);
    return true;
}

void Shutdown() {}

}  // namespace

REPLAY_MODULE("w2c-attach", 20, Init, nullptr, Shutdown);
