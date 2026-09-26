// W1-E's ATTACH-TIME probe. Everything here is answered before the engine has initialised, so it runs
// on `bare` today rather than waiting for offline session entry.
//
// ★ WHY THIS IS NOT A CONSOLATION PRIZE.
//
// HP1's whole safety argument is ONE BRANCH. IsLocallyControlled refuses any owner whose low word is
// >= 0x10 at `cmp cx,10h / jnb`, before it touches memory, and PeerTime refuses at the identical
// constant. That is why a synthetic owner cannot resolve to a local user and cannot read out of bounds
// -- not "was not observed to", cannot. Every in-match probe W1-E has queued rests on it.
//
// Those bytes were verified in echovr.exe ON DISK. That proves nothing about what is in memory:
// W1-C's first real sweep was REFUSED because rlay.dll had already detoured a guarded site, and a
// detour lands in the mapped image, not in the file. So this probe re-reads the two bound sites, and
// the prologues of every function W1-E either calls or reasons about, out of the image the loader
// actually mapped.
//
// If a bound is patched, the synthetic owner is unsafe and W1-E's match probes must not run. Finding
// that out costs one attach-time read; finding it out afterwards costs the whole sweep's credibility.
//
// ★ WHAT IT DOES NOT ESTABLISH: anything requiring a component pool. IsLocallyControlled returning
// false, UpdateTargetState taking the transform, a pose rendering exactly, and the local pawn's
// accessors varying all need a loaded match and are queued separately as W1-E-match.probe.json. This
// probe deliberately asserts nothing that depends on engine state, so its verdict cannot be an
// accident of timing.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "bootstrap/buildguard.h"
#include "bootstrap/log.h"
#include "bootstrap/services.h"
#include "generated/facts_generated.h"

namespace {

const char kReplayModuleName[] = "w1e-attach";

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

// One recorded byte window against the mapped image. `label` names the site in the failure text,
// because "a prologue differed" is not a diagnosis.
void CheckSite(uint64_t va, const uint8_t* want, size_t n, const char* label) {
    const void* p = replay::buildguard::MappedAddress(va);
    char what[192];
    _snprintf_s(what, sizeof what, _TRUNCATE, "%s resolves inside the mapped image", label);
    Check(p != nullptr, what);
    if (p == nullptr) {
        return;
    }
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
    if (char* dot = strrchr(stem, '.')) {
        *dot = '\0';
    }
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", root, stem);
    CreateDirectoryA(dir, nullptr);

    char probeId[128] = "W1-E-attach";
    char ids[512] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    if (const char* found = strstr(ids, "probe_entityattach=")) {
        const char* value = found + strlen("probe_entityattach=");
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
    replay::log::Direct(g_failed == 0 ? replay::LogLevel::Info : replay::LogLevel::Error,
                        kReplayModuleName, "%s (%llu checked): %s", g_failed == 0 ? "PASS" : "FAIL",
                        (unsigned long long)g_checked, g_detail);
}

bool Init(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&Init), &self);

    // ---- 1. The two bound sites HP1 rests on. These are the point of the probe. -----------------
    CheckSite(facts::ENTITY_HP1_BOUND_SITE, facts::ENTITY_HP1_BOUND_SITE_BYTES,
              sizeof facts::ENTITY_HP1_BOUND_SITE_BYTES,
              "ENTITY_HP1_BOUND_SITE (cmp cx,10h in IsLocallyControlled)");
    CheckSite(facts::NETGAME_PEERTIME_BOUND_SITE, facts::NETGAME_PEERTIME_BOUND_SITE_BYTES,
              sizeof facts::NETGAME_PEERTIME_BOUND_SITE_BYTES,
              "NETGAME_PEERTIME_BOUND_SITE (cmp dx,10h in PeerTime)");

    // ---- 2. Known-answer control, on the site that matters most. --------------------------------
    // Without it, a memcmp that always agreed would have passed both checks above and the probe would
    // report a green it had not earned.
    {
        const void* p = replay::buildguard::MappedAddress(facts::ENTITY_HP1_BOUND_SITE);
        Check(p == nullptr || memcmp(static_cast<const uint8_t*>(p) + 1,
                                     facts::ENTITY_HP1_BOUND_SITE_BYTES,
                                     sizeof facts::ENTITY_HP1_BOUND_SITE_BYTES) != 0,
              "the same comparison one byte along refuses (known-answer control)");
    }

    // ---- 3. The functions E-4 CALLS through a pointer. A detour here is executed by us. ---------
    static const Site kCalled[] = {
        {facts::CS_LOOKUP_BY_SYMBOL, facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
         sizeof facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE, "CS_LOOKUP_BY_SYMBOL"},
        {facts::ENTITY_IS_LOCALLY_CONTROLLED, facts::ENTITY_IS_LOCALLY_CONTROLLED_PROLOGUE,
         sizeof facts::ENTITY_IS_LOCALLY_CONTROLLED_PROLOGUE, "ENTITY_IS_LOCALLY_CONTROLLED"},
        {facts::ENTITY_LIST_UNLINK, facts::ENTITY_LIST_UNLINK_PROLOGUE,
         sizeof facts::ENTITY_LIST_UNLINK_PROLOGUE, "ENTITY_LIST_UNLINK"},
        {facts::ENTITY_HISTORY_RECYCLE, facts::ENTITY_HISTORY_RECYCLE_PROLOGUE,
         sizeof facts::ENTITY_HISTORY_RECYCLE_PROLOGUE, "ENTITY_HISTORY_RECYCLE"},
        {facts::ENTITY_EMIT_CHANGED_DELEGATES, facts::ENTITY_EMIT_CHANGED_DELEGATES_PROLOGUE,
         sizeof facts::ENTITY_EMIT_CHANGED_DELEGATES_PROLOGUE, "ENTITY_EMIT_CHANGED_DELEGATES"},
    };
    for (const Site& s : kCalled) {
        CheckSite(s.va, s.want, s.n, s.label);
    }

    // ---- 4. The functions W1-E REASONS ABOUT but never calls. -----------------------------------
    // A detour on one of these does not corrupt our writes; it invalidates the analysis that says our
    // writes are enough, which is the more expensive failure because nothing else would report it.
    static const Site kReasoned[] = {
        {facts::ENTITY_UPDATE_TARGET_STATE, facts::ENTITY_UPDATE_TARGET_STATE_PROLOGUE,
         sizeof facts::ENTITY_UPDATE_TARGET_STATE_PROLOGUE, "ENTITY_UPDATE_TARGET_STATE"},
        {facts::NET_UPDATE_INTERP, facts::NET_UPDATE_INTERP_PROLOGUE,
         sizeof facts::NET_UPDATE_INTERP_PROLOGUE, "NET_UPDATE_INTERP"},
        {facts::ENTITY_PLAYER_STATE_CB, facts::ENTITY_PLAYER_STATE_CB_PROLOGUE,
         sizeof facts::ENTITY_PLAYER_STATE_CB_PROLOGUE, "ENTITY_PLAYER_STATE_CB"},
        {facts::ENTITY_SEQ_COMPARE, facts::ENTITY_SEQ_COMPARE_PROLOGUE,
         sizeof facts::ENTITY_SEQ_COMPARE_PROLOGUE, "ENTITY_SEQ_COMPARE"},
        {facts::ENTITY_SET_OWNER_REFUSED, facts::ENTITY_SET_OWNER_REFUSED_PROLOGUE,
         sizeof facts::ENTITY_SET_OWNER_REFUSED_PROLOGUE, "ENTITY_SET_OWNER_REFUSED"},
        {facts::ENTITY_INTERPOLATE_PHYS_STATE, facts::ENTITY_INTERPOLATE_PHYS_STATE_PROLOGUE,
         sizeof facts::ENTITY_INTERPOLATE_PHYS_STATE_PROLOGUE, "ENTITY_INTERPOLATE_PHYS_STATE"},
        {facts::ENTITY_WARP_SEQ_ADVANCE, facts::ENTITY_WARP_SEQ_ADVANCE_PROLOGUE,
         sizeof facts::ENTITY_WARP_SEQ_ADVANCE_PROLOGUE, "ENTITY_WARP_SEQ_ADVANCE"},
        {facts::NETGAME_PEER_TIME_BY_USER, facts::NETGAME_PEER_TIME_BY_USER_PROLOGUE,
         sizeof facts::NETGAME_PEER_TIME_BY_USER_PROLOGUE, "NETGAME_PEER_TIME_BY_USER"},
    };
    for (const Site& s : kReasoned) {
        CheckSite(s.va, s.want, s.n, s.label);
    }

    char note[512];
    _snprintf_s(note, sizeof note, _TRUNCATE, "%s%llu sites read from the mapped image; rlay=%s",
                g_failed ? " | " : "", (unsigned long long)g_checked,
                GetModuleHandleA("rlay.dll") ? "present" : "absent");
    Note(note);
    Note(" | ESTABLISHES: the two 16-slot bounds HP1's safety rests on, and the prologues of the five "
         "functions E-4 calls and the eight it reasons about, as they stand in memory rather than on "
         "disk. DOES NOT ESTABLISH: anything needing a component pool -- those are W1-E-match.");

    WriteVerdict(self);
    return true;
}

void Shutdown() {}

}  // namespace

REPLAY_MODULE("w1e-attach", 20, Init, nullptr, Shutdown);
