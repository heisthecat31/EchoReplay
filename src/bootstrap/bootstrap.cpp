#include "bootstrap/bootstrap.h"

#include <windows.h>

#include <cstring>

#include "bootstrap/attachhook.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/coexist.h"
#include "bootstrap/config.h"
#include "bootstrap/egress.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/inbound.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/mainprof.h"
#include "bootstrap/modreg.h"
#include "bootstrap/offlinelock.h"
#include "bootstrap/paths.h"
#include "bootstrap/sc7.h"
#include "generated/facts_generated.h"

namespace replay {
namespace bootstrap {
namespace {

#ifndef REPLAY_BUILD_ID
#define REPLAY_BUILD_ID "W1-C"
#endif

const char kBuildId[] = REPLAY_BUILD_ID " " __DATE__ " " __TIME__;

// Named rather than written as a bare 0 in the argument list: the fact registry's no-transcription gate
// reads a literal in an argument position as a possible offset, and it is right to.
constexpr DWORD kDefaultStackSize = 0;

// The drain interval a config file is allowed to ask for. Ten milliseconds is far below any useful
// setting and still not a spin; a minute is far above any useful setting and still drains.
constexpr int64_t kMinDrainMs = 10;
constexpr int64_t kMaxDrainMs = 60000;
constexpr DWORD kRunImmediately = 0;
// SC7's second look. The first runs the moment every module has initialised; a detour installed later -
// by us on a lazy path, or by anything else sharing the process - is invisible to it. Thirty seconds is
// past the game's own startup and inside the shortest sweep window (L9 is 60 s), so both passes land.
constexpr uint64_t kLateSc7Ms = 30000;

struct State {
    uint64_t       attachTickMs;
    ReplayServices services;
    HANDLE         worker;
    HANDLE         stop;
    HMODULE        self;
    volatile LONG  ready;
};

State g_state;

void LogEnvironment() {
    char dll[kPathMax];
    char cwd[kPathMax];
    char exe[kPathMax];
    // %ls in a narrow format string would be a locale conversion; asking Windows for the ANSI forms is
    // both cheaper and lossless enough for a diagnostic.
    GetModuleFileNameA(g_state.self, dll, static_cast<DWORD>(sizeof dll));
    GetCurrentDirectoryA(static_cast<DWORD>(sizeof cwd), cwd);
    GetModuleFileNameA(nullptr, exe, static_cast<DWORD>(sizeof exe));

    log::Direct(LogLevel::Info, "boot", "build=%s pid=%lu", kBuildId, GetCurrentProcessId());
    log::Direct(LogLevel::Info, "boot", "dll=%s", dll);
    log::Direct(LogLevel::Info, "boot", "exe=%s", exe);
    // The plugins scan root is the process CWD, not the exe directory. A wrong CWD is the difference
    // between "loaded from somewhere unexpected" and "never loaded", and those look identical otherwise.
    log::Direct(LogLevel::Info, "boot", "cwd=%s", cwd);
    // Static self-registration has already happened by now: the CRT runs every REPLAY_MODULE's registrar
    // before it calls DllMain. Printing the count here rather than after the build guard means a refused
    // build still tells you whether the registry mechanism itself is alive.
    log::Direct(LogLevel::Info, "boot", "%llu module(s) self-registered before DllMain ran",
                static_cast<unsigned long long>(modreg::Count()));
}

// S4.6 rule 5: "Any feature depending on a U fact logs `depends on unverified fact <id>` at startup."
// Printing the whole list once at startup is the version of that rule this packet can implement: a
// per-feature attribution needs the features, which are Wave 2.
void LogUnverifiedFacts() {
    if constexpr (facts::UNVERIFIED_FACT_COUNT == 0) {
        return;
    }
    ILog* out = log::Interface();
    out->Write(LogLevel::Warn, "facts",
               "%llu of %llu registry facts are unverified on PC; any feature that reaches one is "
               "working from a number nobody re-derived",
               static_cast<unsigned long long>(facts::UNVERIFIED_FACT_COUNT),
               static_cast<unsigned long long>(facts::FACT_COUNT));
    // S4.6 rule 5 says these are LOUD. At Trace they are filtered out at the default level, so the rule
    // was implemented into invisibility - the summary appeared and not one of the ids it counted.
    for (size_t i = 0; i < facts::UNVERIFIED_FACT_COUNT; ++i) {
        out->Write(LogLevel::Info, "facts", "depends on unverified fact %s",
                   facts::UNVERIFIED_FACT_IDS[i]);
    }
}

// A WHOLE-TOKEN match, not a substring. `-offline` inside `-offlinefoo` is a different switch, and a
// blocker that armed on it would be arming for a reason that is not there. A token ends at whitespace,
// a quote, or one of the separators the engine's parser accepts for `switch=value`.
bool IsTokenBoundary(wchar_t c) {
    return c == 0 || c == L' ' || c == L'\t' || c == L'"' || c == L'=' || c == L':';
}

bool HasSwitchImpl(const wchar_t* cmd, const wchar_t* flag) {
    for (const wchar_t* p = cmd; *p != 0; ++p) {
        if (p != cmd && !IsTokenBoundary(p[-1])) {
            continue;   // mid-token: not the start of a switch
        }
        size_t i = 0;
        while (flag[i] != 0 && p[i] != 0 && (p[i] | 0x20) == (flag[i] | 0x20)) {
            ++i;
        }
        if (flag[i] == 0 && IsTokenBoundary(p[i])) {
            return true;
        }
    }
    return false;
}

// ★ Which kind of session is this? Read from the command line the game will actually parse - W1-F's
// GetCommandLine hook is installed during Attach, so by the time the worker runs this reflects any
// injected arguments rather than the raw ones.
//
// ★★★ EVERY ONE OF THESE IS A HINT, and the distinction cost a ruling to get right. A switch on the
// command line is evidence of what the user ASKED FOR, never of what the engine did with it - and
// -offline is not even parsed by stock echovr.exe. It exists because rlay.dll patches it in, so
// "plugin loaded, -offline passed, rlay.dll absent" is a session that goes ONLINE while the switch says
// otherwise. Verified: neither -offline nor -replayviewer occurs in echovr.exe as ASCII or UTF-16.
//
// So these arm on the strict side and are RELEASABLE. What settles it is SessionFlags bit 0x40, whose
// only writers W1-F owns; once that is readable the state decides and any hint it contradicts is given
// up through egress::Release.
// The name of W2-G's launch carrier. Spelled here rather than included from control/launchargs.h,
// because src/control depends on this library and reaching back up would close the loop; the same
// reason that header spells ".echodemo" itself rather than including the UI's. Kept in step by a test
// rather than by hand: tests/bootstrap/test_session_mode.cpp includes both and refuses a drift.
constexpr wchar_t kOpenEnvVar[] = L"ECHOREPLAY_OPEN";

SessionMode DetectSessionMode(bool* offlineSwitchSeen, bool* rlayLoaded) {
    const wchar_t* cmd = GetCommandLineW();
    *rlayLoaded = coexist::IsModuleLoaded("rlay.dll");
    *offlineSwitchSeen = cmd != nullptr && HasSwitchImpl(cmd, L"-offline");
    return ClassifySession(cmd, OpenRequestPresent(), *rlayLoaded);
}

// ★★★ THE LOAD-ORDER RE-CHECK, and it exists because the first look FAILS OPEN.
//
// Plugins load alphabetically, so echoreplay.dll is reached before rlay.dll. In the ordinary case we
// therefore look for rlay before it exists, disbelieve a -offline that is in fact credible, classify a
// genuinely offline session as a live online match, and leave it UNGUARDED. That is the wrong direction
// on the one guard standing between this project and a repeat of its production-server incident.
//
// ★ It cannot deadlock, which is what separates it from release-on-session-state: rlay's presence needs
// no socket, so the observation is not downstream of the traffic the arm would block.
//
// Bounded, because a plugin that has not appeared by the time the loader has finished its pass is not
// going to. The cost while pending is one GetModuleHandleA per drain tick.
constexpr uint64_t kRlayRecheckWindowMs = 30000;

struct EgressRecheck {
    bool     pending;
    uint64_t deadlineMs;
};

EgressRecheck g_egressRecheck;

void RecheckLateRlay() {
    if (!g_egressRecheck.pending) {
        return;
    }
    if (coexist::IsModuleLoaded("rlay.dll")) {
        g_egressRecheck.pending = false;
        // Still a HINT - rlay is loaded and the switch is present, which is credible but not observed
        // session state - so it stays releasable.
        const egress::ArmState state = egress::Arm(egress::ArmTrigger::OfflineHint);
        log::Direct(LogLevel::Warn, "boot",
                    "rlay.dll appeared after the first look: -offline is credible after all, so the "
                    "network blocker is arming now (%s). The window between attach and this point was "
                    "unguarded",
                    egress::ArmStateName(state));
        return;
    }
    if (GetTickCount64() >= g_egressRecheck.deadlineMs) {
        g_egressRecheck.pending = false;
        log::Direct(LogLevel::Info, "boot",
                    "rlay.dll did not appear within %llu ms of the arming decision, so -offline stays "
                    "inert and this remains a live online match. Not armed, and that is the correct "
                    "outcome rather than a missed one",
                    static_cast<unsigned long long>(kRlayRecheckWindowMs));
    }
}

DWORD WINAPI WorkerMain(LPVOID) {
    // ★ FIRST, before config::Load and before the 35 MB md5. Orchestrator ruling
    // 2026-08-28, and the reason is that this is the one gap in this component with no downstream
    // remedy: ConnectEx is obtainable ONLY through WSAIoctl, so refusing that lookup closes it
    // completely - but a pointer fetched before we arm is already in a register and cannot be revoked.
    // Every other uncovered path is something we can hook later; that one we cannot.
    //
    // config is already available: config::LoadForAttach() read the file at attach time, read-only, so
    // the ledger key means something this early. Still on the worker, so the loader-lock discipline is
    // untouched - this reorders worker steps and moves nothing inline.
    // ★★★ ONE CONDITION: do not arm ONLY when this is a live online match being captured. Arm otherwise.
    //
    // The invariant is P3b - this plugin originates no outbound traffic of its own. The sole reason a
    // capture session is exempt is that in a LIVE ONLINE match the client's traffic is the SUBJECT of
    // the recording. An -offline session has no live match, so that justification is absent and the
    // blocker arms; a sweep and an explicit config key still override, and an unreadable command line
    // takes the strict side.
    //
    // Three earlier versions were wrong. `inSweep || GetBool(key, false)` armed only for the harness, so
    // a shipped viewer phoned out freely. Defaulting to armed then broke the owner's primary use case.
    // Keying on -replayviewer read the wrong axis: `offline-local` does not carry it, and that is the
    // profile which produced 82 refused destinations - so the session that most needed the blocker was
    // the one classified as not needing it.
    const bool inSweep = GetEnvironmentVariableA("REPLAY_PROBE_ROOT", nullptr, 0) != 0;
    const IConfig* cfg = config::Interface();
    const char* mode = cfg->GetString("bootstrap.block_egress", "auto");
    const bool forcedOn = mode != nullptr && std::strcmp(mode, "on") == 0;
    const bool forcedOff = mode != nullptr && std::strcmp(mode, "off") == 0;
    bool offlineSwitchSeen = false;
    bool rlayLoaded = false;
    const SessionMode session = DetectSessionMode(&offlineSwitchSeen, &rlayLoaded);

    bool blockEgress = true;
    egress::ArmTrigger trigger = egress::ArmTrigger::ModeUnknown;
    if (inSweep) {
        blockEgress = true;
        trigger = egress::ArmTrigger::SweepOverride;
    } else if (forcedOff) {
        blockEgress = false;
        trigger = egress::ArmTrigger::DisabledByConfig;
    } else if (session == SessionMode::LiveOnlineMatch) {
        // ★ the one exemption -- AND IT IS ABOVE `on`. `on` is a request
        // made before the session existed; a live online match is what the session turned out to
        // be, and in it the blocker contains nothing and only cuts the player off the host.
        blockEgress = false;
        trigger = egress::ArmTrigger::LiveOnlineCapture;
        if (forcedOn) {
            log::Direct(LogLevel::Warn, "boot",
                        "bootstrap.block_egress=on is IGNORED in this session: it is a live online "
                        "match, and arming the egress blocker would cut this client off the host. "
                        "The blocker stays disarmed");
        }
    } else if (forcedOn) {
        blockEgress = true;
        trigger = egress::ArmTrigger::ConfigForcedOn;
    } else if (session == SessionMode::Replay) {
        blockEgress = true;
        trigger = egress::ArmTrigger::ReplayHint;      // asked for, not yet entered
    } else if (session == SessionMode::Offline) {
        blockEgress = true;
        trigger = egress::ArmTrigger::OfflineHint;     // ★ asked for, and the engine may ignore it
    } else if (session == SessionMode::RefusedStaleSwitch) {
        // Spelled out rather than left to fall through the default below, so the one classification
        // that must never reach the exemption cannot acquire a different answer by accident later.
        blockEgress = true;
        trigger = egress::ArmTrigger::ModeUnknown;
        log::Direct(LogLevel::Warn, "boot",
                    "-replayviewer is on the engine command line and no build registers that switch. "
                    "ApplyCommandLine stops at the first unregistered switch, so -offline, -level, "
                    "-region and the host pins on this line were IGNORED and this session is not "
                    "offline. The launch path is the %ls environment variable (W2-G). Classified as "
                    "%s, and the blocker is armed.",
                    kOpenEnvVar, SessionModeName(session));
    } else {
        blockEgress = true;
        trigger = egress::ArmTrigger::ModeUnknown;
    }

    if (blockEgress) {
        egress::Arm(trigger);
        log::Direct(trigger == egress::ArmTrigger::ModeUnknown ? LogLevel::Warn : LogLevel::Info,
                    "boot", "the network blocker is armed (%s); -offline %s, rlay.dll %s",
                    egress::ArmTriggerName(trigger),
                    offlineSwitchSeen ? "present" : "absent",
                    rlayLoaded ? "loaded" : "NOT loaded");
    } else {
        egress::RecordNotArmed(trigger);
        if (trigger == egress::ArmTrigger::DisabledByConfig) {
            // Loud, and at Error. Somebody has switched off the one mechanism enforcing the owner's
            // binding no-server constraint, and a run that does that must say so unmissably.
            log::Direct(LogLevel::Error, "boot",
                        "bootstrap.block_egress is 'off' and this is not a sweep: the network blocker "
                        "is NOT armed and this process can reach a server. Set it to 'auto' to let the "
                        "session mode decide");
        } else {
            // Expected and correct, so Info: this is a LIVE ONLINE match and its traffic is the subject
            // of the recording. Said anyway, because "not armed" must never be a silence.
            // ★ The two inputs are logged even though the answer is "no". A -offline that was seen
            // but not believed is the one case where this decision could be wrong through a load-order
            // race - rlay loading after we looked - and that is only diagnosable if both facts are
            // recorded, not just the conclusion drawn from them.
            if (offlineSwitchSeen && !rlayLoaded) {
                // ★ We disbelieved a switch that may simply have been read too early. Look again.
                g_egressRecheck.pending = true;
                g_egressRecheck.deadlineMs = GetTickCount64() + kRlayRecheckWindowMs;
            }
            log::Direct(LogLevel::Info, "boot",
                        "the network blocker is NOT armed (%s): this is a live online match and its "
                        "traffic is what the capture is OF. -offline %s, rlay.dll %s%s",
                        egress::ArmTriggerName(trigger),
                        offlineSwitchSeen ? "present" : "absent",
                        rlayLoaded ? "loaded" : "NOT loaded",
                        (offlineSwitchSeen && !rlayLoaded)
                            ? " - the switch is inert without rlay, so it was not believed"
                            : "");
        }
    }

    // First, because everything below wants to say something and the log is the only channel we have.
    config::Load();
    log::Drain();

    coexist::Report report{};
    if (config::Interface()->GetBool("bootstrap.log_coexistence", true)) {
        coexist::Survey(&report);
    }

    const BuildVerdict verdict = buildguard::Check();
    if (verdict != BuildVerdict::Accept) {
        // Nothing is installed and no module is initialised. The plugin is inert, the log says exactly
        // why, and the game is left exactly as it was found.
        // ★ THE UNWIND, now that the blocker arms BEFORE the guard has a verdict.
        //
        // hookmgr's own hooks are removed: we recognise no build, so we install nothing into the game.
        // The EGRESS blocker is deliberately LEFT ARMED. Its purpose does not depend on our recognising
        // the executable - the process is still running and could still reach a server, and a sweep
        // that refused the build still wants the network closed. Tearing it down here would undo the
        // one protection that is still meaningful.
        //
        // That is why hookmgr::Shutdown() no longer calls MH_Uninitialize: that removes EVERY hook in
        // the process, including the blocker's, from a path whose whole message is "we touched nothing".
        hookmgr::Shutdown();
        if (blockEgress && egress::Current().state == egress::ArmState::Armed) {
            log::Direct(LogLevel::Info, "boot",
                        "the egress blocker stays armed: it does not depend on recognising this build");
        }
        log::Shutdown();
        InterlockedExchange(&g_state.ready, 1);
        return 0;
    }

    // ★ Drained between phases, not just at the top. DumpLedger is ~48 ring lines and the
    // unverified-fact list is 140-odd more; at log.level=trace that is most of the ring's 256 slots
    // before the loop's first drain 250 ms later, and the overflow is only reported at process exit.
    // ★ The offline-bit lock, and it goes HERE for two reasons. It needs the build guard to have
    // accepted, because it scans this executable's .text and hooks what it finds there; and it must be
    // live before any menu can reach the session teardown, which is far later than this. Its Arm-time
    // cost is a single pass over .text plus one MinHook freeze, both measured and reported.
    //
    // The return is CONSUMED. A lock that cannot be established is the one condition under which a sweep
    // is running without the network safety property it believes it has, so it is logged at Error and
    // carried into the probe report rather than being left for somebody to notice.
    //
    // ★★★ NOT APPLIED FOR A GENUINE LIVE ONLINE MATCH. The offline-bit lock, and the loopback-only bind
    // rewrite it gates below, are OFFLINE/REPLAY containment - not egress. In a live online match the
    // offline bit is never set, so the lock has nothing to hold, and the client's own sockets must bind
    // real interfaces to reach the live host: a loopback rewrite there cuts the match off exactly as the
    // egress blocker would (the network-unreachable failure this exemption fixes). The signal is the SAME
    // positive classification that exempts the egress blocker, tightened - IsLiveOnlineExempt requires the
    // absence of any -offline switch, so a bare -offline not yet credible through a late rlay is contained
    // rather than exempt. Replay, offline, an unreadable command line and a refused stale switch all keep
    // the lock, and with it the loopback rewrite. Ambiguity falls to the contained side.
    offlinelock::LockState lockState = offlinelock::LockState::NotAttempted;
    if (IsLiveOnlineExempt(session, offlineSwitchSeen)) {
        log::Direct(LogLevel::Info, "boot",
                    "live online match (%s, -offline %s): the offline-bit lock is NOT applied and the "
                    "loopback-only bind rewrite stays disarmed, so the client's own sockets reach the "
                    "live host. Its traffic is the subject of the capture",
                    SessionModeName(session), offlineSwitchSeen ? "present" : "absent");
    } else {
        lockState = offlinelock::Apply();
        if (lockState != offlinelock::LockState::Locked) {
            log::Direct(LogLevel::Error, "boot",
                        "the offline bit lock is %s: the offline mode bit can still be cleared by the "
                        "session teardown, and a sweep on this launch is NOT protected by it",
                        offlinelock::LockStateName(lockState));
        }
    }
    log::Drain();

    // ★ The INBOUND half of constraint L2, and it goes immediately after the lock because the lock IS
    // its arming predicate (ARCH-18 S5, ledger row 1X.4). Not the presence of -offline: an unregistered
    // switch earlier on the engine's command line voids the whole line silently, so the args record
    // what was asked for and the lock records what the engine did. AlreadyLockedInProcess counts as
    // Locked - it means another module of ours won the install race, not that the bit is unprotected,
    // and grading it as not-offline would leave the very session this rule exists for uncontained.
    const bool offlineLocked = lockState == offlinelock::LockState::Locked ||
                               lockState == offlinelock::LockState::AlreadyLockedInProcess;

    // ★ The same two facts, published once for every engine-WRITING playback path in the plugin
    // (RECORDER-2 part A). Before this line the answer is "not decided", which refuses -- so a path
    // that somehow ran earlier than boot cannot write either.
    livesafety::PublishBootFacts(IsLiveOnlineExempt(session, offlineSwitchSeen), offlineLocked);
    livesafety::LogStartupBlock();
    log::Drain();
    //
    // ★ RequestedString, never GetString. This key is a tri-state whose `auto` has to be RESOLVED
    // before anything reads it: an ordinary read closes the publish window, and SetEffectiveString then
    // refuses, leaving row 1X.4 Unresolved on every SC7 pass forever - a missing denominator reported
    // in place of a missing publisher. So we take the request, decide, and publish what auto became.
    const char* loopbackValue = config::RequestedString("net.loopback_only");
    const inbound::Mode loopbackMode = inbound::ParseMode(loopbackValue);
    const char* loopbackSource =
        loopbackMode == inbound::Mode::Auto        ? "auto, resolved from the offline lock"
        : loopbackMode == inbound::Mode::Unrecognised
            ? "an unrecognised net.loopback_only value, treated as auto"
            : "an explicit net.loopback_only value";
    if (loopbackMode == inbound::Mode::Unrecognised) {
        log::Direct(LogLevel::Warn, "boot",
                    "net.loopback_only is '%s', which is none of auto/on/off; treating it as auto. An "
                    "unreadable override is not an override, and silently taking it as 'off' would "
                    "leave an offline session reachable",
                    loopbackValue != nullptr ? loopbackValue : "");
    }
    const bool liveOnlineExempt = IsLiveOnlineExempt(session, offlineSwitchSeen);
    const inbound::Decision loopback = inbound::Decide(loopbackMode, offlineLocked, liveOnlineExempt);
    if (loopback.trigger == inbound::ArmTrigger::LiveOnlineRefused) {
        // Loud, and it names the key: this is the exact setting that broke
        // the join on 2026-09-18, and an operator who left it on has to be told it was ignored
        // rather than discover it from a match that would not connect.
        log::Direct(LogLevel::Warn, "boot",
                    "net.loopback_only=on is IGNORED in this session: it is a live online match, "
                    "and rewriting the client's binds to loopback would stop it reaching the host. "
                    "The bind rewrite stays disarmed");
    }
    // What `auto` became, published before anything can read the key. This is the row's resolution:
    // every later reader is served the mode that is actually running, and SC7 has a denominator.
    config::SetEffectiveString("net.loopback_only", loopback.arm ? "on" : "off");
    if (loopback.arm) {
        inbound::Arm(loopback.trigger, loopbackMode, loopbackSource);
    } else {
        inbound::RecordNotArmed(loopback.trigger, loopbackMode, loopbackSource);
    }
    log::Drain();

    // Row 2.8's resolution, from the same classification the three containment gates above used.
    // Before DumpLedger and InitAll: both read the key, and a read closes the publish window.
    config::SetEffectiveString(
        "playback.host_netvar_hook",
        ResolveHostNetvarHook(config::RequestedString("playback.host_netvar_hook"),
                              IsLiveOnlineExempt(session, offlineSwitchSeen), offlineLocked,
                              cfg->GetBool("playback.gamestate_clock", false) ||
                                  cfg->GetBool("playback.gamestate_score", false)));
    // Row 2.9, same classification, same window.
    config::SetEffectiveString(
        "playback.host_score_hook",
        ResolveHostScoreHook(config::RequestedString("playback.host_score_hook"),
                             IsLiveOnlineExempt(session, offlineSwitchSeen), offlineLocked,
                             cfg->GetBool("playback.disc_drive", false)));

    log::Interface()->DumpLedger();
    log::Drain();
    LogUnverifiedFacts();
    log::Drain();

    g_state.services.config = config::Interface();
    g_state.services.log = log::Interface();
    const size_t live = modreg::InitAll(&g_state.services);
    log::Drain();
    log::Direct(LogLevel::Info, "boot", "%llu of %llu module(s) initialised; %llu hook(s) created, %llu armed",
                static_cast<unsigned long long>(live),
                static_cast<unsigned long long>(modreg::Count()),
                static_cast<unsigned long long>(hookmgr::Count()),
                static_cast<unsigned long long>(hookmgr::ArmedCount()));

    InterlockedExchange(&g_state.ready, 1);

    // After the modules, because the frame thread it samples is reported by one of them, and before
    // SC7, whose pass reads ~10 MB of .text and would otherwise be the first thing a profile showed.
    mainprof::Start();

    // ★ SC7's runtime half, which has been NOT-RUN on every launch this project has ever made. Here
    // because this is the first moment the ledger is complete: every module has registered and armed
    // whatever its row declares, so the declaration and the bytes can be compared against each other.
    // On the worker, never on the game tick - it reads ~10 MB of .text from disk.
    sc7::RunPass("post-init");
    log::Drain();

    // Clamped, because both ends are harmful and both are one keystroke away in a hand-edited file:
    // 0 spins a core for the process lifetime inside a VR title, and a negative value casts to INFINITE
    // so the ring is never drained and every module's log line is invisible until exit.
    const int64_t requested = config::Interface()->GetInt("log.drain_interval_ms", 250);
    const int64_t clamped = ClampDrainIntervalMs(requested);
    if (clamped != requested) {
        log::Direct(LogLevel::Warn, "config",
                    "log.drain_interval_ms is %lld, which is outside [%lld, %lld]; using %lld",
                    static_cast<long long>(requested), static_cast<long long>(kMinDrainMs),
                    static_cast<long long>(kMaxDrainMs), static_cast<long long>(clamped));
    }
    const DWORD interval = static_cast<DWORD>(clamped);
    bool lateSc7Done = false;
    for (;;) {
        const DWORD rc = WaitForSingleObject(g_state.stop, interval);
        RecheckLateRlay();
        // L2-RUN-3 F7: the teardown report never survives the harness's kill, so the bind counters
        // have to reach the log on a clock while the process is still alive. No-op unless armed, and
        // it is placed before the drain so the line leaves the ring in the same pass.
        inbound::MaybeLogHeartbeat(GetTickCount64());
        if (!lateSc7Done && GetTickCount64() - g_state.attachTickMs >= kLateSc7Ms) {
            lateSc7Done = true;
            sc7::RunPass("late-tick");
        }
        log::Drain();
        if (rc != WAIT_TIMEOUT) {
            break;
        }
    }
    return 0;
}

}  // namespace

bool Attach(void* selfModule) {
    g_state.attachTickMs = GetTickCount64();
    g_state.self = static_cast<HMODULE>(selfModule);

    // ★ The first statement. EchoLoader discards LoadLibraryA's return and logs nothing, so this line is
    // the only evidence that we exist at all - and it has to be on disk before anything that could fault.
    if (!log::Attach(selfModule)) {
        return false;
    }
    LogEnvironment();

    coexist::Report report{};
    if (!coexist::ClaimInstance(&report)) {
        return false;
    }

    // Pin. EchoLoader discards our HMODULE and never calls FreeLibrary, but W1-D's slot claim hands our
    // module handle to the engine for the process lifetime, and a module that can be unmapped while the
    // engine holds a pointer into it is a crash waiting for someone to run FreeLibrary in a console.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&Attach), &pinned);

    // No DLL_THREAD_ATTACH callbacks: the engine makes plenty of threads and we have nothing to do on any
    // of them. This must come before CreateThread or the new thread's attach notification races it.
    DisableThreadLibraryCalls(g_state.self);

    // MH_Initialize is HeapCreate plus one VirtualAlloc: no DLL load, no thread touched, safe here.
    // Arming a hook is not, and nothing is armed from this function.
    hookmgr::Initialize();

    // ★ Inline, before the worker exists, because some work cannot wait for it. Ledger row 1X.1 must
    // beat the game's own GetCommandLineA call, which W1-F measured 46 bytes of straight-line code after
    // the call that reaches us - a worker thread loses that race by construction. Read the config first,
    // read-only, so each hook's ledger key is the user's answer rather than its declared default.
    config::LoadForAttach();
    attachhook::RunAll();

    g_state.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_state.worker = CreateThread(nullptr, kDefaultStackSize, WorkerMain, nullptr, kRunImmediately,
                                  nullptr);
    if (g_state.worker == nullptr) {
        log::Direct(LogLevel::Error, "boot", "CreateThread failed (%lu); this plugin will do nothing",
                    GetLastError());
        return false;
    }
    CloseHandle(g_state.worker);
    g_state.worker = nullptr;
    // From here a Direct line has somewhere to land other than the calling thread's stack frame.
    log::SetDeferred(true);
    // Not joined and not waited on: the loader lock is held and the worker will outlive this call.
    return true;
}

void Detach(bool processTerminating) {
    // The teardown half of ARCH-18 S6.5. L2 #2 recorded the hook ARMED and its counters nowhere
    // (L2-RUN-2 F4), because Current() had no caller in a shipping build: a counter that only exists
    // inside a process that has since exited measures nothing. This is the last moment a launch can
    // say what the bind detour actually saw, so it runs on both paths and before the log is shut down.
    inbound::LogReport("teardown");
    if (processTerminating) {
        // Every other thread has been terminated, so the ring can be flushed safely -- and that is all.
        // The engine tore itself down before ExitProcess: a module shutdown here joins dead threads
        // under the loader lock and calls through pointers into freed engine objects (launch 07:37,
        // the clock's restore, unhandled 0xC0000005 2 s after LogOut). The OS reclaims the rest; an
        // open capture was closed earlier, from RadPluginShutdown via modreg::RunPreExit.
        log::Shutdown();
        return;
    }
    // We pinned ourselves, so a FreeLibrary should never reach here. If one does, say so and do nothing
    // else: joining the worker under the loader lock is the deadlock this whole design avoids.
    log::Direct(LogLevel::Warn, "boot",
                "FreeLibrary on a pinned module; leaving the worker running rather than joining it under "
                "the loader lock");
    if (g_state.stop != nullptr) {
        SetEvent(g_state.stop);
    }
}

const wchar_t* OpenEnvVarName() { return kOpenEnvVar; }

// Set to something, and not to nothing.
//
// The buffer form is not a detail. GetEnvironmentVariableW returns the character count EXCLUDING the
// terminator when the value fits the buffer, so an empty value and an absent one are both 0 - which is
// what "present and non-empty" needs, and the same read control/launchargs.cpp makes. The (nullptr, 0)
// form answers 1 for an empty value, because it reports the size a buffer would need; written that way
// this classified `ECHOREPLAY_OPEN=` as a launch request. Measured, not assumed - see
// tests/bootstrap/test_session_mode.cpp, which pins both forms.
//
// Nothing here is parsed. Whether the value names an openable capture is W2-G's question, asked later
// and answered with its own refusal reasons; a value that names nothing openable is still a launch that
// meant playback, and must not read as a live online match.
bool OpenRequestPresent() {
    wchar_t probe[2] = {};
    return GetEnvironmentVariableW(kOpenEnvVar, probe, static_cast<DWORD>(_countof(probe))) != 0;
}


const char* SessionModeName(SessionMode mode) {
    switch (mode) {
        case SessionMode::Replay:             return "replay";
        case SessionMode::Offline:            return "offline";
        case SessionMode::LiveOnlineMatch:    return "live online match";
        case SessionMode::RefusedStaleSwitch: return "refused: stale -replayviewer on the engine line";
        case SessionMode::Unknown:            return "unknown";
    }
    return "unknown";
}

SessionMode ClassifySession(const wchar_t* commandLine, bool openRequestPresent, bool rlayLoaded) {
    // ★ FIRST, and before anything else on the line is believed. An unregistered switch makes
    // ApplyCommandLine stop, so every switch on that line - -offline included - was discarded by the
    // engine. Reading -offline off a line the engine threw away would be believing a hint that was
    // never delivered, and the answer it produces (Offline) is the one that says "this session has no
    // live match". It has one.
    if (commandLine != nullptr && HasSwitchImpl(commandLine, L"-replayviewer")) {
        return SessionMode::RefusedStaleSwitch;
    }
    // The launch request is now W2-G's environment variable. It is OUR carrier and we are the ones who
    // honour it, so its presence really does mean a playback is intended - which is what -replayviewer
    // used to mean here, and what it can never mean again.
    if (openRequestPresent) {
        return SessionMode::Replay;
    }
    if (commandLine == nullptr || commandLine[0] == 0) {
        return SessionMode::Unknown;   // no reading, so no claim: the caller arms
    }
    // -offline is rlay's switch, and means nothing without rlay.
    if (OfflineHintIsCredible(commandLine, rlayLoaded)) {
        return SessionMode::Offline;
    }
    // Either no -offline at all, or a -offline the engine will ignore. Both are a live online match,
    // and this is the branch that stops the plugin blocking the owner's own game.
    return SessionMode::LiveOnlineMatch;
}

bool IsLiveOnlineExempt(SessionMode session, bool offlineSwitchSeen) {
    return session == SessionMode::LiveOnlineMatch && !offlineSwitchSeen;
}

const char* ResolveHostNetvarHook(const char* requested, bool liveOnlineExempt, bool offlineLocked,
                                  bool gamestateDriven) {
    const bool refusedByFile = requested != nullptr && std::strcmp(requested, "off") == 0;
    return !refusedByFile && !liveOnlineExempt && offlineLocked && gamestateDriven ? "hook" : "off";
}

const char* ResolveHostScoreHook(const char* requested, bool liveOnlineExempt, bool offlineLocked,
                                 bool discDriven) {
    return ResolveHostNetvarHook(requested, liveOnlineExempt, offlineLocked, discDriven);
}

int64_t ClampDrainIntervalMs(int64_t requested) {
    if (requested < kMinDrainMs) {
        return kMinDrainMs;
    }
    return requested > kMaxDrainMs ? kMaxDrainMs : requested;
}

ReplayServices* Services() { return &g_state.services; }

bool HasSwitch(const wchar_t* commandLine, const wchar_t* flag) {
    if (commandLine == nullptr || flag == nullptr || flag[0] == 0) {
        return false;
    }
    return HasSwitchImpl(commandLine, flag);
}

bool OfflineHintIsCredible(const wchar_t* commandLine, bool rlayLoaded) {
    return rlayLoaded && HasSwitch(commandLine, L"-offline");
}

bool Ready() { return g_state.ready != 0; }

const char* BuildId() { return kBuildId; }

uint64_t AttachTickMs() { return g_state.attachTickMs; }

}  // namespace bootstrap
}  // namespace replay
