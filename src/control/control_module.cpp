// W2-G. The control module: the console window, the ECHOREPLAY_OPEN launch request, and the tick
// that drains the command queue. One translation unit, as B.10 requires; src/plugin/dllmain.cpp is
// not edited.
//
// ORDER 80, after ui (70), so the replay::ui seam is up before a launch request is replayed into
// it. ★ IT IS NOT "so the transport is up", which is what this said and it was wrong: the
// transport is registered by playback at order 300, a long way after this module initialises. The
// behaviour was never wrong, because ServiceLaunchRequest waits per tick for the transport to
// appear -- but the reasoning was, and a reason that does not hold is how the wait gets deleted by
// somebody tidying up.
//
// * THE ENVIRONMENT IS READ HERE, ON THE WORKER, NOT AT ATTACH -- the same place, and the same
// tick, that used to read the command line. The block is inherited at CreateProcess and nothing of
// ours or the engine's rewrites this name, so the answer at attach and the answer here are the
// same; reading it here keeps the validation's file I/O out from under the loader lock. It is
// snapshotted once and never re-read.
//
// * WHY IT IS NO LONGER A SWITCH: AUDIT_W1-F_R4 F2. An unregistered switch makes ApplyCommandLine
// return before ANY handler runs, so -replayviewer voided -offline and all four host pins and the
// launch went online. launchargs.h carries the trace. There is no command-line fallback.
//
// * THE LAUNCH REQUEST IS A REQUEST, NOT AN ENTRY SEQUENCE. This module establishes no session
// state of its own. It waits until the transport has registered, then calls replay::ui::LoadPath
// (or LoadMostRecent), which consults ISessionEntry the way DESIGN 8.4.3 requires -- and SE-B's
// stock call sets the offline bit itself. Inventing an entry sequence here would be a second,
// divergent answer to a question W1-F already owns.

#include <stdio.h>
#include <windows.h>

#include "bootstrap/log.h"
#include "bootstrap/services.h"
#include "control/command.h"
#include "control/console.h"
#include "control/dispatch.h"
#include "bootstrap/livesafety.h"
#include "control/http.h"
#include "control/launchargs.h"
#include "control/launchgate.h"
#include "engine/session.h"
#include "engine/tick.h"
#include "ui/uiapi.h"

namespace replay {
namespace {

// Everything this module drives lives in replay::control; REPLAY_MODULE has to be instantiated from
// a namespace that can see ::replay, so the module sits one level up and pulls the names in.
using namespace ::replay::control;

const char* const kReplayModuleName = "control";

ReplayServices* g_services = nullptr;
OpenRequest     g_launch;

enum class LaunchState : uint8_t {
    NotRequested,
    Waiting,
    Done,
    GaveUp,
    // The value was set and could not be used. Distinct from GaveUp, which is a load that was
    // attempted and turned down, and from NotRequested, which is nobody asking for anything.
    Refused,
};
LaunchState g_launchState = LaunchState::NotRequested;
bool        g_httpWanted = false;
bool        g_httpReported = false;
void*       g_lastNetGame = nullptr;
uint64_t    g_waitStartedMs = 0;
bool        g_waitClockStarted = false;
uint64_t    g_launchSeq = 0;
uint64_t    g_launchRetryAtMs = 0;
uint32_t    g_launchAttempts = 0;
// Every refusal this request produced, so `diag` reports a number rather than the owner having to
// find a log line. A launch asked for a capture and did not get one is a fact the surface must be
// able to state.
uint32_t    g_launchRefusals = 0;
// The state the last refusal was collected at, and whether one is outstanding. L3 F1: a load the
// engine's own precondition turned down is retried when the ENGINE moves, not when a clock ticks --
// re-asking the same question of the same state can only get the same answer.
bool        g_refusalPending = false;
int32_t     g_refusedAtState = 0;
// The last reason the gate gave for waiting, so the log carries each transition once instead of
// once per frame. The gate returns string literals, so the pointer identifies the reason.
const char* g_waitNote = nullptr;

// How long to wait for a transport before reporting that the launch request cannot be served. Long
// enough to cover a cold level load on a slow disk, short enough that the user gets an answer in
// the same session rather than a console that silently never does anything.
constexpr uint64_t kLaunchWaitLimitMs = 120ull * 1000ull;

// A load that is refused because the session is not ready yet is worth trying again; one that is
// refused because the file is not there is not, and neither answer is distinguishable from the
// other at submission time. So it retries on a fixed cadence inside the same overall limit, and
// says how many times it tried when it gives up.
constexpr uint64_t kLaunchRetryMs = 3000;
constexpr uint32_t kLaunchMaxAttempts = 8;

bool InSweep() {
    // Same probe the bootstrap uses to decide egress mode. Duplicated as one expression rather than
    // widening another packet's header for it.
    return GetEnvironmentVariableA("REPLAY_PROBE_ROOT", nullptr, 0) != 0;
}

void StartConsole(const IConfig* config) {
    const console::Mode mode = console::ModeFromString(config->GetString("control.console", "auto"));
    if (!console::ShouldStart(mode, InSweep())) {
        dispatch::SetConsoleState(false, InSweep() ? "not started: this is a sweep"
                                                  : "not started: control.console is off");
        log::Direct(LogLevel::Info, kReplayModuleName, "console not started (%s)",
                    InSweep() ? "sweep" : "config");
        return;
    }
    if (console::Start()) {
        dispatch::SetConsoleState(true, console::Diagnostic());
        log::Direct(LogLevel::Info, kReplayModuleName, "console up: %s", console::Diagnostic());
    } else {
        // Not fatal. The panel and the launch switch are unaffected by a console that did not come
        // up, and taking the module down would remove them too.
        dispatch::SetConsoleState(false, console::Diagnostic());
        log::Direct(LogLevel::Warn, kReplayModuleName, "console did not start: %s",
                    console::Diagnostic());
    }
}

// AUDIT_W1-F_R4 F2 again, from the other end: if a stale launcher still puts our old switch on the
// line, the engine has ALREADY thrown the whole line away by the time this runs -- -offline, -level
// and all four host pins with it. Nothing here can undo that; what it can do is make it the loudest
// line in the log instead of an invisible online launch.
void WarnOnLegacySwitch() {
    if (!CommandLineMentionsReplayViewer(::GetCommandLineW())) {
        return;
    }
    log::Direct(LogLevel::Warn, kReplayModuleName, "%s", kLegacySwitchWarning);
}

// Three answers, not two. "absent" for a value that was set and refused would report the launch the
// owner asked for as a launch he never asked for.
const char* LaunchStateWord() {
    switch (g_launchState) {
        case LaunchState::NotRequested: return "absent";
        case LaunchState::Refused:      return "refused";
        default:                        return "requested";
    }
}

void ReadOpenEnv() {
    WarnOnLegacySwitch();

    if (!ReadOpenRequest(&g_launch)) {
        g_launchState = LaunchState::NotRequested;
        dispatch::SetLaunchState(false, 0, "not set");
        return;
    }
    if (g_launch.refusal != OpenRefusal::None) {
        // Named, never guessed at. Opening some other capture because this one could not be opened
        // is the worst available outcome for a launch that named a file.
        ++g_launchRefusals;
        g_launchState = LaunchState::Refused;
        char note[160];
        snprintf(note, sizeof note, "refused: %s", OpenRefusalText(g_launch.refusal));
        dispatch::SetLaunchState(true, g_launchRefusals, note);
        log::Direct(LogLevel::Warn, kReplayModuleName, "%ls refused: %s (%ls)", kOpenEnvVar,
                    OpenRefusalText(g_launch.refusal),
                    g_launch.path[0] != 0 ? g_launch.path : L"the value did not fit");
        return;
    }

    g_launchState = LaunchState::Waiting;
    if (g_launch.hasPath) {
        dispatch::SetLaunchState(true, g_launchRefusals, "waiting for a transport");
        log::Direct(LogLevel::Info, kReplayModuleName, "%ls %ls", kOpenEnvVar, g_launch.path);
    } else {
        dispatch::SetLaunchState(true, g_launchRefusals, "waiting for a transport (most recent)");
        log::Direct(LogLevel::Info, kReplayModuleName,
                    "%ls names no file: entering from the most recent capture", kOpenEnvVar);
    }
}

// Fires the launch request exactly once, when the services it needs have registered. Everything it
// then does goes through the same dispatcher the console uses, so an ECHOREPLAY_OPEN launch and a
// typed `load` cannot diverge.
void OpenBrowserInstead() {
    Command browse;
    browse.id = CommandId::List;
    dispatch::Submit(browse);
}

// ★ BACKLOG-1. The recorder surface's Record/Stop buttons, issued as command TEXT through the
// parser and the queue the console types into -- so the button, the console and the HTTP route all
// get the same session gate, the same "already recording" refusal and the same status line. The
// UI never calls the recorder itself.
//
// The direction is control -> ui, like ui::SetBookmarks: nothing under src/ui includes this module.
// Tick thread, like every other replay::ui call. The answer is whether the command was ACCEPTED --
// a queued command's outcome is collected under its sequence number, which a button has no use for.
bool SubmitUiCommand(const wchar_t* line) {
    if (line == nullptr) return false;
    const ParseResult parsed = Parse(line);
    if (!parsed.ok) {
        log::Direct(LogLevel::Warn, kReplayModuleName, "ui command refused: %ls -- %s", line,
                    parsed.error);
        return false;
    }
    const dispatch::Result r = dispatch::Submit(parsed.cmd);
    log::Direct(r.ok ? LogLevel::Info : LogLevel::Warn, kReplayModuleName, "ui command `%ls`: %s",
                line, r.text);
    return r.ok;
}

void SubmitLaunchLoad(uint64_t nowMs) {
    Command cmd;
    cmd.id = CommandId::Load;
    if (g_launch.hasPath) {
        for (size_t i = 0; i < kMaxPathChars; ++i) {
            cmd.text[i] = g_launch.path[i];
            if (g_launch.path[i] == 0) {
                break;
            }
        }
        cmd.text[kMaxPathChars - 1] = 0;
    } else {
        // ★ A FLAG, NOT AN EMPTY STRING. "Enter from the most recent capture" is DESIGN 8.4.3's
        // no-path behaviour, carried by ECHOREPLAY_OPEN=* and said explicitly here, because
        // inferring it from an empty text field is what made console `load ""` open a capture the
        // owner had not named -- and an empty environment value cannot be told from an unset one.
        cmd.loadMostRecent = true;
    }
    const dispatch::Result r = dispatch::Submit(cmd);
    g_launchSeq = r.seq;
    g_launchRetryAtMs = nowMs + kLaunchRetryMs;
    ++g_launchAttempts;
    log::Direct(r.ok ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
                "open request submitted (attempt %u): %s", g_launchAttempts, r.text);
}

// ============================================================================================
// THE WAIT IS MEASURED ON THE WALL CLOCK, NOT ON THE ENGINE'S SIM TIME.
//
// This used to difference two samples of TickInfo::simTimeUs, with a comment calling it "monotonic
// and a sound elapsed measure". It is monotonic while it runs -- but tick.cpp reads it as ZERO
// whenever g.clock is still null, and the project's own AuditFrame keeps a simRegressions counter
// for exactly this source. So the first control tick with the clock unresolved recorded a start of
// 0, and the tick where the clock resolved supplied the engine's whole accumulated sim time: after
// a two-minute sit in the menu the subtraction exceeded the limit immediately and the launch
// request was abandoned after a handful of frames. On a genuine regression the unsigned
// subtraction underflowed to ~1.8e19 and gave up instantly.
//
// GetTickCount64 needs no clock to be resolved, cannot regress, and is the unit the message
// already quotes to the owner. It is not derived from anything the engine can reset.
// ============================================================================================
void ServiceLaunchRequest(const TickInfo&) {
    if (g_launchState != LaunchState::Waiting) {
        return;
    }
    const uint64_t nowMs = GetTickCount64();
    if (!g_waitClockStarted) {
        g_waitStartedMs = nowMs;
        g_waitClockStarted = true;
    }
    const bool outOfTime = (nowMs - g_waitStartedMs) > kLaunchWaitLimitMs;

    // Read the session service, not the log. ISessionEntry::State() casts the engine's raw value,
    // which is wider than NetGameState and negative on a failure, so it is compared as an int.
    const bool    sessionPresent = g_services->session != nullptr;
    const int32_t netState =
        sessionPresent ? static_cast<int32_t>(g_services->session->State()) : 0;

    // ★ THE OUTCOME IS COLLECTED, NOT ASSUMED. The request used to be a one-shot fired on the
    // first tick at which the transport existed -- which is tick 1, during login, long before a
    // session exists to enter -- and if the load was refused the explanation went to a panel that
    // was not drawing yet and was never retried. Now the sequence number comes back, the result is
    // read when the tick produces it, and a refusal is tried again inside the same limit.
    if (g_launchSeq != 0) {
        dispatch::Result done;
        if (dispatch::TakeResult(g_launchSeq, &done)) {
            if (done.ok) {
                g_launchState = LaunchState::Done;
                dispatch::SetLaunchState(true, g_launchRefusals, done.text);
                log::Direct(LogLevel::Info, kReplayModuleName, "ECHOREPLAY_OPEN: %s", done.text);
                if (!g_launch.hasPath) {
                    OpenBrowserInstead();
                }
                return;
            }
            ++g_launchRefusals;
            g_refusalPending = true;
            g_refusedAtState = netState;
            dispatch::SetLaunchState(true, g_launchRefusals, done.text);
            log::Direct(LogLevel::Warn, kReplayModuleName,
                        "ECHOREPLAY_OPEN attempt %u refused: %s", g_launchAttempts, done.text);
            g_launchSeq = 0;
        } else if (nowMs >= g_launchRetryAtMs) {
            // The answer was displaced from the result ring by later commands before we read it.
            // Treated as "unknown", which means retry rather than assume either outcome.
            g_launchSeq = 0;
        } else {
            return;   // still pending
        }
    }

    LaunchGateInput gate;
    gate.transportPresent = g_services->transport != nullptr;
    gate.sessionPresent   = sessionPresent;
    gate.netGameState     = netState;
    gate.attempts         = g_launchAttempts;
    gate.maxAttempts      = kLaunchMaxAttempts;
    gate.refusalPending   = g_refusalPending;
    gate.refusedAtState   = g_refusedAtState;
    gate.retryDue         = nowMs >= g_launchRetryAtMs;
    gate.outOfTime        = outOfTime;

    const char* why = "no reason recorded";
    switch (DecideLaunchStep(gate, &why)) {
        case LaunchStep::Wait:
            if (g_waitNote != why) {
                g_waitNote = why;
                dispatch::SetLaunchState(true, g_launchRefusals, why);
                log::Direct(LogLevel::Info, kReplayModuleName,
                            "ECHOREPLAY_OPEN waiting (NetGame state %ld): %s",
                            static_cast<long>(netState), why);
            }
            return;

        case LaunchStep::GiveUp:
            g_launchState = LaunchState::GaveUp;
            dispatch::SetLaunchState(true, g_launchRefusals, why);
            log::Direct(LogLevel::Error, kReplayModuleName,
                        "ECHOREPLAY_OPEN gave up after %u attempt(s) over %llu s at NetGame state "
                        "%ld: %s. Opening the browser instead so the capture can be chosen by hand.",
                        g_launchAttempts,
                        static_cast<unsigned long long>((nowMs - g_waitStartedMs) / 1000ull),
                        static_cast<long>(netState), why);
            OpenBrowserInstead();
            return;

        case LaunchStep::Submit:
            g_refusalPending = false;
            SubmitLaunchLoad(nowMs);
            return;
    }
}

void ControlTick(const TickInfo& info) {
    // Kept for ControlShutdown, which has no TickInfo and must not hand routes back through a
    // listener that belongs to a session the engine has already replaced.
    g_lastNetGame = info.netGame;
    // The listener is embedded in CR15NetGame, which does not exist before state 9, so registration
    // is attempted from the tick rather than from init. Start() is a no-op once it has succeeded
    // on this netGame and while netGame is null, so this costs two loads per frame in the steady
    // state.
    //
    // ★ REGISTERED ON *THIS* NETGAME, not registered at all. A cross-level load runs
    // SESSION.CREATE_OFFLINE_MATCH, which builds a new CR15NetGame: the routes stay on the old
    // object's listener and the engine answers 404 for our URIs while a bool still says we are up
    // (DIAG-LEVEL-MISMATCH-404). Every other per-session binding in this plugin re-validates per
    // tick; this is that validation.
    if (g_httpWanted && info.netGame != nullptr && !http::RegisteredOn(info.netGame)) {
        if (http::Running()) {
            // Said out loud and said to the panel: the worst part of this defect was that the
            // status line kept claiming a surface that was gone.
            http::Invalidate("the engine rebuilt the session");
            dispatch::SetHttpState(false, http::Diagnostic());
            g_httpReported = false;
        }
        if (http::Start(info.netGame)) {
            dispatch::SetHttpState(true, http::Diagnostic());
            log::Direct(LogLevel::Info, kReplayModuleName, "http up: %s", http::Diagnostic());
        } else if (!g_httpReported) {
            // Reported once, not every frame: the listener may simply not be bound yet, and a log
            // line per frame would bury everything else.
            g_httpReported = true;
            dispatch::SetHttpState(false, http::Diagnostic());
            log::Direct(LogLevel::Info, kReplayModuleName, "http not up yet: %s",
                        http::Diagnostic());
        }
    }
    ServiceLaunchRequest(info);
    dispatch::Tick();
}

bool ControlInit(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);
    g_services = services;

    dispatch::Init(services);
    // Ledger row 0.16. Refusable like every other row; when it is off the routes are never
    // registered and the console and the panel are unaffected.
    g_httpWanted = services->config->GetBool("control.http_routes", true);
    // The routes are the engine's listener plus an unauthenticated POST, so
    // outside contained offline playback they are off unless the owner says otherwise. Pushed into
    // http rather than tested here, because the per-tick re-registration starts there too.
    const bool liveRoutes = services->config->GetBool("control.http_routes_live", false);
    http::SetLiveAllowed(liveRoutes);
    const bool routesPossible = livesafety::HttpRoutesAllowed(liveRoutes);
    dispatch::SetHttpState(false, !g_httpWanted ? "disabled by control.http_routes"
                                  : routesPossible
                                      ? "waiting for the engine's listener"
                                      : "inert: not contained offline playback "
                                        "(control.http_routes_live)");
    StartConsole(services->config);
    ui::SetCommandSink(&SubmitUiCommand);
    ReadOpenEnv();

    log::Direct(LogLevel::Info, kReplayModuleName,
                "control ready: %zu commands, console %s, ECHOREPLAY_OPEN %s", SpecCount(),
                console::Running() ? "up" : "down", LaunchStateWord());
    return true;
}

void ControlShutdown() {
    // The console first: its thread calls into dispatch, so it has to be stopped before the state
    // it reads goes away.
    console::Stop();
    dispatch::SetConsoleState(false, "stopped");
    // Before dispatch::Shutdown for the reason the console is stopped first: the sink runs a
    // command, and a surface that still held it would be submitting into a dead queue.
    ui::SetCommandSink(nullptr);
    // Before dispatch::Shutdown, because a registered route points at a handler in this DLL and the
    // listener outlives us on a FreeLibrary unload.
    http::Stop(g_lastNetGame);
    dispatch::SetHttpState(false, "stopped");
    dispatch::Shutdown();
    g_services = nullptr;
    g_launchState = LaunchState::NotRequested;
    g_waitClockStarted = false;
    g_launchSeq = 0;
    g_launchAttempts = 0;
    g_launchRefusals = 0;
    dispatch::SetLaunchState(false, 0, "stopped");
}

REPLAY_MODULE("control", 80, ControlInit, ControlTick, ControlShutdown);

}  // namespace
}  // namespace replay
