#pragma once

// HOST-HOLD policy. Which of the two host keys netvarhud should be holding this tick, as a pure
// function of the load state, the config and the host's observed phase -- so the rule is testable
// without an engine, and the module's tick is one call.
//
// The two keys are not the same kind of write and do not get the same rule:
//
//  * `round_time` is the netint the arena script SEEDS its script-local round clock from at each
//    round_start. It freezes nothing by itself, so it is held from the load onwards -- including
//    through the pre-roll, which is the one moment that matters: the host's first round_start
//    happens during it, and a hold that waited for main_loop would miss the only seed of the round
//    it is trying to outlast.
//
//  * `pause` freezes EVERY countdown the script has, not just the round clock. A host that is in a
//    countdown-driven phase when that lands never leaves it. So it is armed only while the host is
//    observed in main_loop, released after a short grace once it is observed anywhere else (which
//    lets the host walk its own countdowns back to main_loop), and re-armed on re-entry.
//
// Borrow()/Return() lift both holds for a caller that needs the host's own values to mean something
// for a moment, and put them back. Refcounted, safe from any thread, and watchdogged: a borrow that
// is never returned restores the holds by itself rather than leaving the host running.

#include <stdint.h>

namespace replay {
namespace playback {
namespace hosthold {

// How long the pause hold survives the host being seen outside main_loop before it is released.
inline constexpr uint64_t kOutsideMainLoopGraceUs = 2000000;
// A borrow nobody returns is a bug in the borrower, not a reason to leave the host unheld.
inline constexpr uint64_t kBorrowWatchdogUs = 5000000;

struct Policy {
    bool        loaded = false;       // a replay is loaded, and this process may write the engine
    bool        round_time_on = false;   // playback.host_hold
    bool        pause_on = false;        // playback.host_hold_pause
    bool        playing = false;         // the host's game_state matched main_loop
    const char* phase_name = "not published";
};

struct Want {
    bool round_time = false;
    bool pause = false;
};

// One tick. `wall_us` is the elapsed real time since the last call.
Want Step(const Policy& p, uint64_t wall_us);

// Lift both holds until the matching Return. Any thread.
void Borrow();
void Return();
uint32_t BorrowDepth();

// The phase the pause hold was last armed or released at, for the module's log line.
const char* LastPauseEdgePhase();

void ResetForTest();

}  // namespace hosthold
}  // namespace playback
}  // namespace replay
