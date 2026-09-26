// What DllMain does, and what the worker thread does instead.
//
// The split is forced by the loader contract (R11 S0.2): dbgcore.dll LoadLibraryA's us from inside its
// own DllMain, at WinMain statement 2, so our DLL_PROCESS_ATTACH runs with the Windows loader lock held.
// Under that lock we may write a file and create a thread; we may not wait, join, LoadLibrary, or suspend
// another thread. Everything with any weight is therefore on the worker, and DllMain returns immediately.
#pragma once

#include <stdint.h>

#include "bootstrap/services.h"

namespace replay {
namespace bootstrap {

// DLL_PROCESS_ATTACH. Writes the attach line first, claims the instance, pins the module, initialises
// MinHook and starts the worker. Returns false only when this instance must not run at all - a rename
// into the loader's slot, or a second copy of us in the same process.
bool Attach(void* selfModule);

// DLL_PROCESS_DETACH. `processTerminating` is DllMain's lpReserved != nullptr.
void Detach(bool processTerminating);

// The services locator. Modules publish their own interface into it during init, in `order` sequence, and
// read the ones they need. A pointer is null until its owning packet has registered (B.12), which is why
// REPLAY_REQUIRE exists.
ReplayServices* Services();

// True once the worker has finished its startup sequence. The build guard's verdict is separate: a run
// can be ready and still have refused the build, in which case no module was initialised.
bool Ready();

const char* BuildId();

// Whole-token switch match over a command line: `-offline` is present in `-offline -windowed` and is
// NOT present in `-offlinefoo` or `-x-offline`. Pure, and exposed because it decides whether the
// network blocker arms - a substring match here would arm for a switch that is not there, and the two
// outcomes need to be testable without a game.
bool HasSwitch(const wchar_t* commandLine, const wchar_t* flag);

// ★★ Whether a -offline on the command line means anything in THIS process.
//
// -offline is absent from stock echovr.exe - verified, as neither ASCII nor UTF-16 - and exists only
// because rlay.dll byte-patches it in. Without rlay the engine never parses it, the session goes online
// anyway, and arming on the switch would cut a live match off from its server.
//
// ★ Judged BEFORE arming rather than corrected after. The obvious remedy - arm on the hint, release it
// when the session state disagrees - deadlocks: arming during the menu phase blocks the login and
// matchmaking traffic that PRODUCES the state, so the observation that would release the arm can only
// happen if the arm is not there. This predicate has no such circularity; neither input needs a socket.
//
// Pure, so both answers are constructible in a test without a game.
bool OfflineHintIsCredible(const wchar_t* commandLine, bool rlayLoaded);

// What kind of session is this? Every value is a HINT and every one is releasable; see the long comment
// in bootstrap.cpp. Four outcomes became five when W2-G took -replayviewer off the engine command line.
//
// RefusedStaleSwitch is not a session kind, it is a REFUSAL: a launcher that still passes -replayviewer
// has handed the engine a switch nothing registers, and ApplyCommandLine stops at the first of those -
// so -offline, -level, -region and the four host pins on that same line were never applied. The one
// thing that session is NOT is offline. It must not read as Replay (the switch no longer carries that
// meaning) and it must not read as LiveOnlineMatch (which is the only classification that DISARMS the
// blocker, and this is the launch least entitled to that).
enum class SessionMode { Replay, Offline, LiveOnlineMatch, RefusedStaleSwitch, Unknown };

const char* SessionModeName(SessionMode mode);

// Pure, and exposed for the same reason HasSwitch and OfflineHintIsCredible are: all five outcomes have
// to be constructible in a test without a game. `openRequestPresent` is whether ECHOREPLAY_OPEN is set
// to something non-empty - the caller does the reading, and nothing here parses the path.
SessionMode ClassifySession(const wchar_t* commandLine, bool openRequestPresent, bool rlayLoaded);

// The one session all three containment gates must leave alone: a genuine live online match. True ONLY
// when the classification is LiveOnlineMatch AND no -offline switch is on the command line at all.
//
// ★ Why not just `session == LiveOnlineMatch`. The offline-bit lock and the loopback-only bind rewrite
// have no release mechanism the way the egress blocker does, so their decision is final at startup. A
// bare -offline that is merely not yet credible - rlay.dll loading after us, the alphabetical load-order
// race - classifies as LiveOnlineMatch on the first look but may be a genuinely offline session. That is
// AMBIGUOUS, and ambiguity must fall to the CONTAINED side: a genuine live match carries no -offline, so
// requiring its absence keeps the exemption to exactly the sessions that are unambiguously online.
//
// A replay (ECHOREPLAY_OPEN set) and an offline session (credible -offline) are not LiveOnlineMatch and
// so are never exempt; nor are a refused stale switch or an unreadable command line. Pure, so both
// answers are constructible in a test without a game.
bool IsLiveOnlineExempt(SessionMode session, bool offlineSwitchSeen);

// What `playback.host_netvar_hook` resolves to: "hook" or "off". The setter detours exist to protect
// netvars a replay is driving, so they are asked for only where that can happen -- a process that is
// not live-online exempt, holds the offline lock, and has a gamestate flag on -- and never when the
// file says off. Pure, so every refusal is constructible in a test.
const char* ResolveHostNetvarHook(const char* requested, bool liveOnlineExempt, bool offlineLocked,
                                  bool gamestateDriven);

// What `playback.host_score_hook` resolves to, by the same rule: the OnScoreCB detour exists only
// where a replay can drive the disc -- contained, offline-locked, playback.disc_drive on.
const char* ResolveHostScoreHook(const char* requested, bool liveOnlineExempt, bool offlineLocked,
                                 bool discDriven);

// The name of W2-G's launch carrier as THIS library spells it. Exposed for one reason: src/control
// depends on this library, so src/bootstrap cannot include control/launchargs.h to get the constant,
// and two independent spellings of one name drift silently - the drift here would make Replay
// unclassifiable again, which is the defect this accessor exists to keep from recurring.
// tests/bootstrap compares the two.
const wchar_t* OpenEnvVarName();

// Is ECHOREPLAY_OPEN set to something non-empty? Exposed because this is where the classification can
// go wrong quietly: an empty value must read as absent, and the two Win32 spellings of this question do
// not agree about that. Reads the process environment, so a test drives it by setting the variable.
bool OpenRequestPresent();

// GetTickCount64() as of the first statement of DLL_PROCESS_ATTACH. The egress blocker's armedAtMs is
// measured from here, because HARNESS.md 2.3 asks for the window in which the blocker is NOT yet
// protecting anything - which starts when we attach, not when Arm() happens to be called.
uint64_t AttachTickMs();

// The drain interval a config file is allowed to ask for, clamped. Pure and exposed so the boundary is
// tested rather than trusted: 0 spins a core for the process lifetime and a negative value casts to
// INFINITE, so the ring is never drained and every module's log line is invisible until exit.
int64_t ClampDrainIntervalMs(int64_t requested);

}  // namespace bootstrap
}  // namespace replay
