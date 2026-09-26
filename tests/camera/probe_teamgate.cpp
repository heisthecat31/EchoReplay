// W2-E. The spectator-camera enable gate, measured rather than reasoned about.
//
// THE QUESTION. CR15NetGame::Update enables the entire shipping spectator camera rig by itself,
// every frame, when three things hold (CAM.SPECTATOR_ENABLE_CALL_SITE):
//
//     (netgameFlags & 4) == 0  &&  CAM.SPECTATOR_TEAM_GATE(netGame)  &&  *(u32*)netGame == 9
//     then:  if (CAM.WEARING_HEADSET()) DisableSystem(gs) else EnableSystem(gs)
//
// If that comes true on its own under -spectatorstream, W2-E ships no enable code at all. If it
// does not, W2-E writes one word. This probe decides which.
//
// ---------------------------------------------------------------------------------------------
// THE INDEXING NOTE, AND IT CORRECTS THE BRIEF RATHER THAN FOLLOWING IT.
//
// netGame+0x178 IS a per-entrant array of USER.STRIDE (592) bytes -- IDENT.PROFILE_JSON_OFF and
// IDENT.USER_ACTOR_GUID_OFF are both addressed as 592*index off the same base, so the warning is
// right about the array. But CAM.SPECTATOR_TEAM_GATE DOES NOT INDEX IT. Its own bytes are
//
//     0F B7 81 78 01 00 00      movzx eax, word ptr [rcx+178h]
//
// ModRM 0x81 is mod=2, rm=1, no SIB: base rcx, disp32, and NO index register. The gate reads
// ENTRANT 0, hard-coded. So "index it to the local user's slot" would measure a word the gate
// never looks at, and the honest read is BOTH: entrant 0, because that is what decides the gate,
// and the whole table, because that is what tells us whether the local user IS entrant 0.
//
// That distinction is the difference between "write the local user's team word" (which would not
// enable anything if the local user is not entrant 0) and "write entrant 0's".
//
// So this probe does not report a team id. It reports the table, and it CALLS THE GATE and reports
// what the gate itself returned -- the observable, not the path.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/offlinelock.h"
#include "bootstrap/tableguard.h"
#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"
#include "generated/facts_generated.h"
#include "probe_teamgate_outcome.h"

namespace {

using namespace replay::camera_facts;
namespace probe = replay::probe_teamgate;

using TeamGateFn = bool(__fastcall*)(void* netGame);

// The W2-E drop has since merged, so these three come from facts_generated.h; the short names are
// local spellings of the generated constants, not transcriptions of their values.
const uint64_t kTeamGateVA      = facts::CAM_SPECTATOR_TEAM_GATE;
const uint64_t kSystemEnabledVA = facts::CAM_SPECTATOR_SYSTEM_ENABLED;
const uint64_t kEnabledGlobalVA = facts::CAM_SPECTATOR_ENABLED_GLOBAL;
const uint64_t kHeadsetByteVA   = 0x1420C7D1Aull;
// The architect's spectator-fixedclock-word, which this probe now ANSWERS rather than
// duplicating: a NON-ZERO value here substitutes a fixed clock for the wall-clock read that
// DESIGN S8.1.2 rests on, reintroducing the pause-time freeze the bespoke camera was deleted
// to avoid. Read TWICE -- at attach and again after the match -- because "nobody writes it"
// is the claim under test, and a single reading cannot see a write that happens in between.
const uint64_t kFixedClockFlagVA  = 0x142099038ull;
const uint64_t kFixedClockValueVA = 0x14209CB00ull;
const uint64_t kHeadsetDwordVA  = 0x1420C7784ull;

const uint8_t kTeamGatePrologue[20] = {
    0x66, 0x83, 0xB9, 0xDE, 0x00, 0x00, 0x00, 0x00, 0x75, 0x03,
    0x33, 0xC0, 0xC3, 0x0F, 0xB7, 0x81, 0x78, 0x01, 0x00, 0x00};
const uint8_t kSystemEnabledPrologue[20] = {
    0x8B, 0x05, 0x4E, 0x61, 0x37, 0x01, 0xC3, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x48, 0x8B, 0x81, 0xD0};

// GAME.TEAM_OFF was PC_PRESENT and withheld because its evidence named an address its bytes were
// not at, so the row had no site. ENG-SPECTATOR-VIEWER corrected the address, anchored it and the
// verdict re-derived to PC_DECISIVE, so the value comes from the registry now and the hatch that
// stood here has gone with the verdict it named.
const uint32_t kGameTeamOff = facts::GAME_TEAM_OFF;

// The gate's own first test, at netGame+0xDE. NOT registered, and NOT the same field as
// IDENT.ENTRANT_COUNT_OFF (0xDC) or IDENT.NONREPLAY_COUNT_OFF (0xE2). Its meaning is UNKNOWN and
// this probe reports it raw rather than naming it.
const uint32_t kGateFirstTestOff = 0xDEu;
const uint32_t kUserWordOff      = facts::USER_ARRAY_OFF;
const uint32_t kMaxEntrantsDumped = 16u;

struct Entrant {
    uint16_t raw;
    uint16_t team;
    uint16_t slot;
    uint16_t teamSlot;
};

struct Census {
    // ★ TWO DIFFERENT QUESTIONS, and they must not share an answer -- buildguard.h's own split.
    // "Is this echovr.exe at all?" gates whether this probe can mean anything: outside the game
    // every site mismatches and scoring that FAIL is the W1-C failure mode, a guard refusal that
    // reads exactly like a defect. "Did this site drift?" is a real finding, but only once the
    // build question has been answered yes.
    bool     buildOk         = false;
    bool     sitesVerified   = false;
    bool     reachedMatch    = false;
    bool     gateCalled      = false;
    bool     gateResult      = false;
    bool     ciFound         = false;
    // What the SHIPPING resolve said, kept verbatim so the row's word is derived and not decided
    // here, and so a reader can tell a could-not-ask from a failure without parsing prose.
    probe::CsSite csSite = probe::CsSite::Unreadable;
    replay::camera::Refusal csRefusal = replay::camera::Refusal::NoNetGame;
    uint64_t levelSpace        = 0;
    uint64_t csFromLookup      = 0;
    uint64_t csFromCsSpace     = 0;
    uint32_t spaceTableVerdict = 0;

    uint32_t netGameState    = 0;
    uint16_t gateFirstTest   = 0;   // netGame+0xDE, the gate's own precondition
    uint16_t entrantCount    = 0;   // IDENT.ENTRANT_COUNT_OFF
    uint16_t nonReplayCount  = 0;   // IDENT.NONREPLAY_COUNT_OFF
    uint16_t lobbyTeam       = 0;   // GAME.TEAM_OFF

    uint32_t enabledGlobal   = 0;   // CAM.SPECTATOR_ENABLED_GLOBAL, read only after VerifySite
    uint32_t fixedClockAtAttach = 0;   // spectator-fixedclock-word, reading 1 of 2
    uint32_t fixedClockAtMatch  = 0;   // ... and reading 2 of 2
    uint64_t fixedClockValue    = 0;   // what would be substituted if the flag is non-zero
    bool     fixedClockRead     = false;
    // spectator-global-1420D3AE4 asks for the module base and the absolute address actually
    // read alongside the value, so a legitimate zero can be told apart from a read of the wrong
    // address. Claiming that read without reporting these would be answering half of it.
    uint64_t moduleBase         = 0;
    uint64_t enabledGlobalAddr  = 0;
    uint8_t  headsetByte     = 0;
    uint32_t headsetDword    = 0;

    int32_t  ciMode          = -1;  // SPECTATOR.CI_MODE_OFF
    int32_t  ciFreeCamActive = -1;  // SPECTATOR.CI_FREECAM_ACTIVE_OFF
    uint16_t csInstances     = 0;
    uint16_t csLocalIndex    = 0;

    // The guard this probe DECLARES, verified rather than promised. See WriteVerdict.
    bool     guardHeld       = false;
    char     lockState[48]   = {0};

    uint32_t entrantsDumped  = 0;
    char     cmdline[1024]   = {0};
    Entrant  entrants[kMaxEntrantsDumped]{};

    char     failedSite[64]  = {0};
};

struct State {
    HMODULE           self     = nullptr;
    std::atomic<bool> bootstrapOk{false};
    Census            census;
};
State g;

template <typename T>
T Load(uintptr_t p) {
    T v{};
    memcpy(&v, reinterpret_cast<const void*>(p), sizeof(T));
    return v;
}

void JsonEscape(char* out, size_t cap, const char* in) {
    size_t o = 0;
    for (size_t i = 0; in[i] != 0 && o + 2 < cap; ++i) {
        const char c = in[i];
        if (c == '"' || c == 0x5C) {
            out[o++] = 0x5C;
        }
        out[o++] = c;
    }
    out[o] = 0;
}

void DllStem(char* out, size_t cap) {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(g.self, path, sizeof path);
    const char* base = strrchr(path, 0x5C);
    base = base ? base + 1 : path;
    strncpy_s(out, cap, base, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) {
        *dot = 0;
    }
}

void ProbeDir(char* out, size_t cap) {
    char root[MAX_PATH] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);
    char stem[MAX_PATH];
    DllStem(stem, sizeof stem);
    _snprintf_s(out, cap, _TRUNCATE, "%s%c%s", root, static_cast<char>(0x5C), stem);
    CreateDirectoryA(out, nullptr);
}

// "W2-E-unknown" is the fallback, never a real id: a done.json whose `probe` does not match the
// plan reads as NOT-RUN, which is the correct meaning of "this DLL could not tell which probe it
// was". Same rule W1-H's probe applies.
void ProbeId(char* out, size_t cap) {
    strncpy_s(out, cap, "W2-E-unknown", _TRUNCATE);
    char ids[1024] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    char stem[MAX_PATH];
    DllStem(stem, sizeof stem);
    for (char* entry = ids; *entry != 0;) {
        char* end = strchr(entry, ';');
        if (end != nullptr) {
            *end = 0;
        }
        if (char* eq = strchr(entry, '=')) {
            *eq = 0;
            if (_stricmp(entry, stem) == 0) {
                strncpy_s(out, cap, eq + 1, _TRUNCATE);
                return;
            }
        }
        if (end == nullptr) {
            break;
        }
        entry = end + 1;
    }
}

void OpenArtifact(FILE** fh, const char* name) {
    char dir[MAX_PATH];
    ProbeDir(dir, sizeof dir);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s%c%s", dir, static_cast<char>(0x5C), name);
    if (fopen_s(fh, path, "wb") != 0) {
        *fh = nullptr;
    }
}

// ★ Written ONCE by the static initialiser and never touched again, so "the DLL loaded and filed
// before it waited for anything" stays observable in EVERY run -- including the successful ones,
// where done.json has already been overwritten by the real verdict. Without it that contract item
// is only checkable by winning a race against the worker, which offline is unwinnable: with a
// wrong build the worker completes in well under a millisecond. It also separates "the probe never
// loaded" from "the probe loaded and measured", which done.json alone cannot do.
void WriteAttached(const char* id) {
    FILE* fh = nullptr;
    OpenArtifact(&fh, "attached.json");
    if (fh != nullptr) {
        fprintf(fh,
                "{\"probe\":\"%s\",\"event\":\"static-initialiser-ran\",\"beforeWorker\":true,"
                "\"tickMs\":%llu}\n",
                id, static_cast<unsigned long long>(GetTickCount64()));
        fclose(fh);
    }
}

void WriteProvisional(const char* id) {
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    JsonEscape(cwdEscaped, sizeof cwdEscaped, cwd);
    FILE* fh = nullptr;
    OpenArtifact(&fh, "done.json");
    if (fh != nullptr) {
        fprintf(fh,
                "{\"probe\":\"%s\",\"verdict\":\"NOT-RUN\",\"checked\":0,\"provisional\":true,"
                "\"cwd\":\"%s\",\"detail\":\"attached and waiting for a match; this file is "
                "provisional and is overwritten by the real verdict. If you are reading it, the "
                "process was killed first.\"}\n",
                id, cwdEscaped);
        fclose(fh);
    }
}

uintptr_t Game() {
    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (module == 0) {
        return 0;
    }
    return Load<uintptr_t>(module + facts::GAME_SINGLETON_RVA);
}

// Every site this probe calls or reads, identified by its BYTES after the rebase. W1-C lost three
// launches to a raw-address read whose guard refused and reported false, which reads exactly like
// a cold engine -- so a refusal here is recorded as its own outcome with the site named, never as
// a zero.
bool VerifySites(Census* c) {
    struct Site {
        const char*    id;
        uint64_t       va;
        const uint8_t* bytes;
    };
    const Site kSites[] = {
        {"CAM.SPECTATOR_TEAM_GATE", kTeamGateVA, kTeamGatePrologue},
        {"CAM.SPECTATOR_SYSTEM_ENABLED", kSystemEnabledVA, kSystemEnabledPrologue},
    };
    for (const Site& s : kSites) {
        if (!replay::buildguard::VerifySite(s.id, s.va, s.bytes, 20)) {
            strncpy_s(c->failedSite, sizeof c->failedSite, s.id, _TRUNCATE);
            return false;
        }
    }
    return true;
}

void Collect(Census* c, uintptr_t netGame) {
    const uintptr_t game = Game();

    // Whether this launch even asked for -spectatorstream decides how to read a lobbyTeam
    // that is not 2, so the command line is part of the measurement, not context.
    strncpy_s(c->cmdline, sizeof c->cmdline, GetCommandLineA(), _TRUNCATE);

    c->netGameState   = Load<uint32_t>(netGame);
    c->gateFirstTest  = Load<uint16_t>(netGame + kGateFirstTestOff);
    c->entrantCount   = Load<uint16_t>(netGame + facts::IDENT_ENTRANT_COUNT_OFF);
    c->nonReplayCount = Load<uint16_t>(netGame + facts::IDENT_NONREPLAY_COUNT_OFF);
    if (game != 0) {
        c->lobbyTeam = Load<uint16_t>(game + kGameTeamOff);
    }

    // The whole entrant table, INDEXED by USER.STRIDE. Entry 0 is the one the gate reads.
    const uint32_t n = c->entrantCount < kMaxEntrantsDumped ? c->entrantCount : kMaxEntrantsDumped;
    c->entrantsDumped = n;
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t w =
            Load<uint16_t>(netGame + kUserWordOff + static_cast<uintptr_t>(i) * facts::USER_STRIDE);
        c->entrants[i].raw      = w;
        c->entrants[i].team     = static_cast<uint16_t>((w >> 10) & 7);
        c->entrants[i].slot     = static_cast<uint16_t>(w & 0x1F);
        c->entrants[i].teamSlot = static_cast<uint16_t>((w >> 5) & 0x1F);
    }

    // The gate itself. Calling it is what makes this a measurement rather than a re-derivation of
    // arithmetic I already read out of the binary -- if my decode of the team test is wrong, this
    // disagrees with the table and the disagreement IS the finding.
    const auto gate = reinterpret_cast<TeamGateFn>(
        const_cast<void*>(replay::buildguard::MappedAddress(kTeamGateVA)));
    if (gate != nullptr) {
        c->gateResult = gate(reinterpret_cast<void*>(netGame));
        c->gateCalled = true;
    }

    c->moduleBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    const void* enabledAt = replay::buildguard::MappedAddress(kEnabledGlobalVA);
    c->enabledGlobalAddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(enabledAt));
    c->enabledGlobal = *reinterpret_cast<const uint32_t*>(enabledAt);
    // spectator-fixedclock-word, reading 2 of 2: after the match, so a write between the
    // two readings is visible rather than assumed absent.
    c->fixedClockAtMatch = *reinterpret_cast<const uint32_t*>(
        replay::buildguard::MappedAddress(kFixedClockFlagVA));
    c->fixedClockValue = *reinterpret_cast<const uint64_t*>(
        replay::buildguard::MappedAddress(kFixedClockValueVA));
    c->headsetByte = *reinterpret_cast<const uint8_t*>(
        replay::buildguard::MappedAddress(kHeadsetByteVA));
    c->headsetDword = *reinterpret_cast<const uint32_t*>(
        replay::buildguard::MappedAddress(kHeadsetDwordVA));

    // The spectator CI, which closes W1-H section 9 items 2 and 5.
    //
    // ★ ASKED, not re-derived. camera::detail::ResolveSpectatorCs is the routine Create() itself
    // calls, and it is the ONLY holder of this policy: netGame+0x8 is the space, netGame+0x2B10 is
    // the spectator CS itself, both slots are probed for readability BEFORE they are loaded, the
    // space's component-system table is shown indexable before an engine routine touches it, and
    // the lookup's answer must EQUAL the +0x2B10 slot. This probe carried a second copy of that
    // rule; it had already lost the two readability probes, and it collapsed five distinguishable
    // refusals into one word this file then filed as "failed". The copy is
    // gone. What is left here is what only the probe can do: guard the lookup site, and read the
    // instance the resolve names.
    const void* siteAddr = replay::buildguard::MappedAddress(facts::CS_LOOKUP_BY_SYMBOL);
    if (siteAddr == nullptr ||
        !replay::Readable(siteAddr, sizeof(facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE))) {
        c->csSite = probe::CsSite::Unreadable;
        return;
    }
    if (!replay::buildguard::VerifySite("CS.LOOKUP_BY_SYMBOL", facts::CS_LOOKUP_BY_SYMBOL,
                                        facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
                                        sizeof(facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE))) {
        c->csSite = probe::CsSite::Mismatch;
        return;
    }
    c->csSite = probe::CsSite::Verified;

    const replay::camera::detail::Resolution res = replay::camera::detail::ResolveSpectatorCs(
        reinterpret_cast<void*>(netGame),
        reinterpret_cast<replay::camera::detail::LookupComponentSystemFn>(
            const_cast<void*>(siteAddr)));
    c->csRefusal          = res.refusal;
    c->levelSpace         = res.levelSpace;
    c->csFromLookup       = res.csFromLookup;
    c->csFromCsSpace      = res.csFromCsSpace;
    c->spaceTableVerdict  = static_cast<uint32_t>(res.spaceTable.verdict);
    if (res.refusal != replay::camera::Refusal::None) {
        return;
    }

    const uintptr_t cs = static_cast<uintptr_t>(res.csFromLookup);
    c->csInstances  = Load<uint16_t>(cs + kCsInstanceCountOff);
    c->csLocalIndex = Load<uint16_t>(cs + kCsLocalIndexOff);
    const uintptr_t base =
        (c->csLocalIndex < c->csInstances) ? Load<uintptr_t>(cs + kCsInstanceBaseOff) : 0;
    if (base == 0) {
        // The resolve found the component system; the instance behind its local index is what is
        // missing. That is an answer, and the enum already has the word for it.
        c->csRefusal = replay::camera::Refusal::NoLocalInstance;
        return;
    }
    const uintptr_t ci = base + static_cast<uintptr_t>(c->csLocalIndex) * kCiStride;
    c->ciMode          = Load<int32_t>(ci + kCiModeOff);
    c->ciFreeCamActive = Load<int32_t>(ci + kCiFreeCamActiveOff);
    c->ciFound         = true;
}

// checked counts what was actually EVALUATED. Undetermined checks are counted and named, never
// scored as passes -- reporting an undetermined read as failing is a false accusation and as
// passing a false all-clear, so it is its own outcome.
struct Check {
    const char* name;
    const char* outcome;   // "ok" | "failed" | "undetermined"
    const char* detail;
};

// ★★ THE GUARD THIS PROBE DECLARES, VERIFIED RATHER THAN PROMISED.
//
// The descriptor declares guards: ["offline-bit-lock"], and an earlier version justified that by
// reading bootstrap.cpp top to bottom: offlinelock::Apply() at :340 precedes coexist::ClaimInstance
// at :401, therefore the lock installs even when the claim is refused. THE LINE NUMBERS ARE RIGHT
// AND THE CONCLUSION WAS BACKWARDS -- source order is not execution order. Following the call graph
// instead:
//
//     Attach(:391) -> log::Attach -> coexist::ClaimInstance(:401)
//                       refused -> RETURN FALSE (:403), and the worker is never created
//                     ... pin, DisableThreadLibraryCalls, hookmgr::Initialize, attachhook::RunAll
//                     CreateThread(WorkerMain) (:429)
//                       WorkerMain(:199) ... offlinelock::Apply() (:340)
//
// Apply() lives INSIDE WorkerMain, which is created at the END of Attach. A probe that loses the
// instance claim returns at :403 and installs NO lock. So the guarantee is the opposite of what was
// claimed, and "declared present but absent" is the worst shape a guard can take -- the launch
// proceeds believing it is protected.
//
// A justification is a promise; this is the mechanism. offlinelock::Current() reads the instrument
// out of SHARED MEMORY precisely so a module that did not install the hook still reports what the
// hook observed, so this is a true statement about the PROCESS, not about this module. If the
// process is not protected, the probe FAILS -- the declaration cannot outlive the thing it declares.
void CollectGuard(Census* c) {
    const replay::offlinelock::LockState st = replay::offlinelock::Current().state;
    strncpy_s(c->lockState, sizeof c->lockState, replay::offlinelock::LockStateName(st), _TRUNCATE);
    // AlreadyLockedInProcess is a SUCCESS: every probe DLL links this library, MinHook refuses the
    // duplicate installs, and a module reporting that is describing a process that IS locked.
    c->guardHeld = (st == replay::offlinelock::LockState::Locked ||
                    st == replay::offlinelock::LockState::AlreadyLockedInProcess);
}

void WriteVerdict(const char* id) {
    const Census& c = g.census;

    Check checks[9];
    uint32_t n = 0;

    checks[n++] = {"offline-bit-lock-held", c.guardHeld ? "ok" : "failed",
                   c.guardHeld ? c.lockState
                               : "DECLARED guards:[offline-bit-lock] but the process is "
                                 "NOT protected -- see lockState and bootstrapAttached"};
    // spectator-fixedclock-word, ANSWERED here rather than duplicated as a second parked read.
    // Zero is the expected reading; non-zero substitutes a fixed clock for the wall-clock read
    // DESIGN S8.1.2 rests on and REOPENS it, so it fails loudly rather than being reported as a
    // number somebody has to notice.
    checks[n++] = {"fixedclock-flag-zero",
                   !c.fixedClockRead ? "undetermined"
                       : ((c.fixedClockAtAttach == 0 && c.fixedClockAtMatch == 0) ? "ok" : "failed"),
                   !c.fixedClockRead
                       ? "not read: the build was not confirmed, so the RVA was not dereferenced"
                       : ((c.fixedClockAtAttach == 0 && c.fixedClockAtMatch == 0)
                              ? "zero at attach and after the match: the stock free-cam integrator "
                                "is on wall time and DESIGN S8.1.2 holds"
                              : "NON-ZERO: a fixed clock is substituted for QueryPerformanceCounter, "
                                "which reopens S8.1.2 -- the stock camera would freeze at pause")};
    checks[n++] = {"build-is-echovr", c.buildOk ? "ok" : "undetermined",
                   c.buildOk ? "the mapped main module is the measured echovr.exe"
                             : "not the expected echovr.exe build -- every reading below is void, "
                               "and this is NOT a defect report"};
    checks[n++] = {"sites-verify",
                   !c.buildOk ? "undetermined" : (c.sitesVerified ? "ok" : "failed"),
                   c.sitesVerified ? "every guarded site holds its registry bytes after rebase"
                                   : (c.buildOk ? c.failedSite : "not evaluated: wrong build")};
    checks[n++] = {"reached-match", c.reachedMatch ? "ok" : "undetermined",
                   c.reachedMatch ? "netGame present and state == 9"
                                  : "no netGame at state 9 before the timeout"};
    checks[n++] = {"gate-called", !c.reachedMatch ? "undetermined" : (c.gateCalled ? "ok" : "failed"),
                   "CAM.SPECTATOR_TEAM_GATE invoked on the live netGame"};
    checks[n++] = {"entrant-table-read",
                   !c.reachedMatch ? "undetermined" : (c.entrantsDumped > 0 ? "ok" : "failed"),
                   "at least one entrant word read at USER.STRIDE spacing"};
    const probe::Outcome csOutcome =
        probe::SpectatorCsOutcome(c.reachedMatch, c.csSite, c.csRefusal);
    checks[n++] = {"spectator-cs", csOutcome.word, csOutcome.detail};
    // The ANSWER, and it is deliberately not a pass/fail: either value is a valid finding and
    // scoring it would turn an observation into a verdict.
    //
    // ★ The detail must never assert a finding that was not observed. An earlier version said
    // "the gate does not pass" whenever reachedMatch was false -- i.e. it reported a measurement
    // it had never taken, as the negative result. That is the false all-clear this outcome exists
    // to prevent, and the offline run is what caught it.
    const char* answer;
    if (!c.reachedMatch) {
        answer = "NOT OBSERVED: no match was reached, so the gate was never evaluated";
    } else if (c.gateResult) {
        answer = "OBSERVED: the gate passes on its own -- W2-E ships no enable code";
    } else {
        answer = "OBSERVED: the gate does not pass -- W2-E must write entrant 0's team word";
    }
    checks[n++] = {"enable-condition-holds", !c.reachedMatch ? "undetermined" : "ok", answer};

    uint32_t failed = 0, undetermined = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (strcmp(checks[i].outcome, "failed") == 0) ++failed;
        if (strcmp(checks[i].outcome, "undetermined") == 0) ++undetermined;
    }
    // NOT-RUN is the harness's word for could-not-ask, and probe::VerdictWord is where that rule
    // lives so a test can drive it. The count of questions that could not be asked travels in
    // `undetermined`, which the harness reads and marks the row qualified with, and the reason
    // stays in each check's detail. `checked` is the ASKED count -- rows that could not be asked
    // are not in it, so a run that asked nothing files checked=0, which the harness reads as
    // "asserted nothing" on its own.
    const uint32_t asked = n - undetermined;
    const char* verdict = probe::VerdictWord(failed, undetermined);

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    JsonEscape(cwdEscaped, sizeof cwdEscaped, cwd);

    FILE* fh = nullptr;
    OpenArtifact(&fh, "done.json");
    if (fh == nullptr) {
        return;
    }
    fprintf(fh,
            "{\"probe\":\"%s\",\"verdict\":\"%s\",\"checked\":%u,\"undetermined\":%u,"
            "\"provisional\":false,"
            "\"cwd\":\"%s\",\"bootstrapAttached\":%s,\n",
            id, verdict, asked, undetermined, cwdEscaped,
            g.bootstrapOk.load(std::memory_order_acquire) ? "true" : "false");

    fprintf(fh, " \"checks\":[");
    for (uint32_t i = 0; i < n; ++i) {
        char d[256];
        JsonEscape(d, sizeof d, checks[i].detail ? checks[i].detail : "");
        fprintf(fh, "%s{\"name\":\"%s\",\"outcome\":\"%s\",\"detail\":\"%s\"}",
                i ? "," : "", checks[i].name, checks[i].outcome, d);
    }
    fprintf(fh, "],\n");

    fprintf(fh,
            " \"reads\":{\"netGameState\":%u,\"gateFirstTest_0xDE\":%u,"
            "\"entrantCount_0xDC\":%u,\"nonReplayCount_0xE2\":%u,\"lobbyTeam_game0x8560\":%u,"
            "\"spectatorEnabledGlobal\":%u,\"headsetByte\":%u,\"headsetDword\":%u,"
            "\"ciMode_0x3C\":%d,\"ciFreeCamActive_0x274\":%d,"
            "\"csInstances\":%u,\"csLocalIndex\":%u,\"gateCalled\":%s,\"gateResult\":%s,"
            "\"lockState\":\"%s\",\"guardHeld\":%s,"
            "\"fixedClockAtAttach\":%u,\"fixedClockAtMatch\":%u,"
            "\"fixedClockValue\":%llu,\"fixedClockRead\":%s,"
            "\"moduleBase\":\"0x%llX\",\"enabledGlobalAddr\":\"0x%llX\","
            "\"enabledGlobalHex\":\"0x%X\",\"netGameStateHex\":\"0x%X\","
            "\"ciModeHex\":\"0x%X\"},\n",
            c.netGameState, c.gateFirstTest, c.entrantCount, c.nonReplayCount, c.lobbyTeam,
            c.enabledGlobal, c.headsetByte, c.headsetDword, c.ciMode, c.ciFreeCamActive,
            c.csInstances, c.csLocalIndex, c.gateCalled ? "true" : "false",
            c.gateResult ? "true" : "false", c.lockState,
            c.guardHeld ? "true" : "false",
            c.fixedClockAtAttach, c.fixedClockAtMatch,
            static_cast<unsigned long long>(c.fixedClockValue),
            c.fixedClockRead ? "true" : "false",
            static_cast<unsigned long long>(c.moduleBase),
            static_cast<unsigned long long>(c.enabledGlobalAddr),
            c.enabledGlobal, c.netGameState, static_cast<unsigned>(c.ciMode));

    // The resolve, as the SHIPPING routine reported it. The row above renders one of three words;
    // this is the reason behind it, unabridged, so a reader never has to infer which of the five
    // refusals produced an "undetermined".
    fprintf(fh,
            " \"spectatorCs\":{\"site\":\"%s\",\"refusal\":\"%s\","
            "\"levelSpace\":\"0x%llX\",\"csFromLookup\":\"0x%llX\","
            "\"csFromCsSpace\":\"0x%llX\",\"spaceTableVerdict\":%u,\"outcome\":\"%s\"},\n",
            probe::CsSiteName(c.csSite), replay::camera::RefusalName(c.csRefusal),
            static_cast<unsigned long long>(c.levelSpace),
            static_cast<unsigned long long>(c.csFromLookup),
            static_cast<unsigned long long>(c.csFromCsSpace),
            c.spaceTableVerdict,
            probe::SpectatorCsOutcome(c.reachedMatch, c.csSite, c.csRefusal).word);

    // The table, indexed. Entry 0 is the ONLY one CAM.SPECTATOR_TEAM_GATE reads.
    char cmdEscaped[2048];
    JsonEscape(cmdEscaped, sizeof cmdEscaped, c.cmdline);
    fprintf(fh, " \"cmdline\":\"%s\",\n", cmdEscaped);

    fprintf(fh, " \"entrants\":[");
    for (uint32_t i = 0; i < c.entrantsDumped; ++i) {
        fprintf(fh,
                "%s{\"index\":%u,\"raw\":%u,\"team\":%u,\"slot\":%u,\"teamSlot\":%u,"
                "\"isGateSubject\":%s}",
                i ? "," : "", i, c.entrants[i].raw, c.entrants[i].team, c.entrants[i].slot,
                c.entrants[i].teamSlot, i == 0 ? "true" : "false");
    }
    fprintf(fh, "],\n");

    fprintf(fh,
            " \"note\":\"CAM.SPECTATOR_TEAM_GATE reads netGame+0x178 with ModRM 0x81 -- base rcx, "
            "disp32, NO index register -- so it samples ENTRANT 0 only. The entrants array is "
            "dumped at USER.STRIDE(592) spacing so that whether the local user IS entrant 0 is "
            "answerable from this artifact. gateResult is the gate's own return value, called on "
            "the live netGame, not a re-derivation of its arithmetic.\"}\n");
    fclose(fh);
}

DWORD WINAPI WorkerMain(LPVOID) {
    Census& c = g.census;

    // The build question first. If this is not the echovr.exe the registry was measured against,
    // nothing below can mean anything and every check is UNDETERMINED -- not failed.
    c.buildOk = replay::buildguard::Check() == replay::BuildVerdict::Accept;
    c.sitesVerified = c.buildOk && VerifySites(&c);

    // spectator-fixedclock-word, reading 1 of 2: at attach, before any match exists. Taken
    // only once the build is confirmed, because MappedAddress on the wrong image reads
    // whatever happens to live at that RVA and a plausible number is worse than no number.
    if (c.buildOk) {
        c.fixedClockAtAttach = *reinterpret_cast<const uint32_t*>(
            replay::buildguard::MappedAddress(kFixedClockFlagVA));
        c.fixedClockRead = true;
    }
    if (c.sitesVerified) {
        // Up to 240 s for the harness to reach a loaded match; the descriptor's timeout is what
        // actually bounds the launch, and this loop just has to not outlive it. Overridable so the
        // offline contract test can exercise the no-match path in a second instead of four minutes
        // -- it changes only how long we wait, never what is measured or how it is scored.
        uint64_t waitMs = 240000ull;
        char override[32] = {0};
        if (GetEnvironmentVariableA("REPLAY_PROBE_DEADLINE_MS", override, sizeof override) != 0) {
            const long long v = _atoi64(override);
            if (v > 0) {
                waitMs = static_cast<uint64_t>(v);
            }
        }
        const uint64_t deadline = GetTickCount64() + waitMs;
        while (GetTickCount64() < deadline) {
            const uintptr_t game = Game();
            if (game != 0) {
                const uintptr_t netGame = Load<uintptr_t>(game + facts::GAME_NETGAME_PTR_OFF);
                if (netGame != 0 && Load<uint32_t>(netGame) == facts::SESSION_STATE_INGAME) {
                    c.reachedMatch = true;
                    Collect(&c, netGame);
                    break;
                }
            }
            Sleep(250);
        }
    }

    CollectGuard(&c);

    char id[128];
    ProbeId(id, sizeof id);
    WriteVerdict(id);
    return 0;
}

// Filed from a STATIC INITIALISER, with no bootstrap dependency: in a multi-probe launch the
// instance claim refuses every module after the alphabetically-first, and five probes once filed
// nothing for exactly that reason. Nothing below waits on bootstrap::Attach.
struct AttachFiler {
    AttachFiler() {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&WriteProvisional), &g.self);
        char id[128];
        ProbeId(id, sizeof id);
        WriteAttached(id);
        WriteProvisional(id);
        if (HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr)) {
            CloseHandle(worker);
        }
    }
};
AttachFiler g_filer;

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (g.self == nullptr) {
                g.self = module;
            }
            // Reported, not obeyed: the worker is already measuring, and a refused instance claim
            // is a normal outcome in a multi-probe launch rather than a reason to disappear.
            g.bootstrapOk.store(replay::bootstrap::Attach(module), std::memory_order_release);
            return TRUE;
        case DLL_PROCESS_DETACH:
            replay::bootstrap::Detach(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
