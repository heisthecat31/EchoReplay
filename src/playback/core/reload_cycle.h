#pragma once

// RELOAD-6. Opening a replay while one is playing re-enters the arena level through the engine's
// own offline-match entry (SESSION.CREATE_OFFLINE_MATCH, the body rlay's -offline boot runs at
// launch), after the previous replay's entrants have left. The entry unloads every game space in
// CR15Game's list and builds a fresh one, which is the only thing that destroys the previous
// session's attached nametag/heraldry children and orphaned script graphs
// (diagnoses DIAG-RELOAD-NAMES and ENG-RELOAD-6).
//
// This is the sequence as a pure machine: the module feeds it observations each tick and performs
// the one action it asks for. Every failure releases the hold, so the reload always goes on -- in
// place, as before RELOAD-6, when the level could not be re-entered.

#include <stdint.h>

namespace replay {
namespace reloadcycle {

enum class Phase : uint8_t {
    Idle,
    AwaitLeave,    // the previous replay's entrants are still leaving the engine
    AwaitRequest,  // ask the session to re-enter the level; retried while the answer is "not yet"
    AwaitExit,     // accepted: waiting for NetGame to leave "in game"
    AwaitEntry,    // left the level: waiting for "in game" again, in the new space
    Done,
};

enum class Outcome : uint8_t {
    None,              // not finished (or never armed)
    Cycled,            // the level was left and entered again
    LeaveStuck,        // the entrants never left; reloaded in place
    RequestRefused,    // the session would not re-enter; reloaded in place
    NoExit,            // accepted but the level was never left; reloaded in place
    SessionFailed,     // the session fell into a failure state after leaving the level
    EntryTimedOut,     // left the level and did not come back inside the bound
    Cancelled,         // the replay was unloaded mid-cycle
};

const char* PhaseName(Phase p);
const char* OutcomeName(Outcome o);

enum class RequestAnswer : uint8_t { Accepted, NotYet, Refused };

enum class Action : uint8_t { None, Request };

struct Limits {
    uint64_t leaveMs = 6000;     // after the module's own 3 s leave bound has already given up
    uint64_t requestMs = 5000;   // "not yet" answers before giving up
    uint64_t exitMs = 5000;      // accepted to "left the level"; the stock call leaves on its next update
    uint64_t entryMs = 60000;    // left to "in game" again; ~4-8 s measured, a boot's worth of slack
};

struct Observation {
    uint64_t nowMs;
    bool     leavePending;   // identity::RosterLeavePending()
    int32_t  netGameState;   // raw NetGame state word
};

inline constexpr int32_t kStateInGame = 9;

class Machine {
public:
    explicit Machine(Limits limits = Limits()) : limits_(limits) {}

    // entryAlreadyRequested: the load funnel has already asked for a level change (a capture on a
    // different level), which is the same cycle -- the machine then only waits it out.
    void Arm(uint64_t nowMs, bool entryAlreadyRequested);
    void Cancel();

    Action Step(const Observation& o);
    void Answer(RequestAnswer answer, uint64_t nowMs);

    // True while admissions and engine writes for the new replay must wait.
    bool Holds() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
    Phase phase() const { return phase_; }
    Outcome outcome() const { return outcome_; }
    bool FinishedNow() { const bool f = finishedEdge_; finishedEdge_ = false; return f; }
    uint64_t ElapsedMs(uint64_t nowMs) const { return nowMs - armedMs_; }
    uint64_t ArmedAtMs() const { return armedMs_; }
    bool LeftLevel() const { return leftLevel_; }
    uint64_t ExitAtMs() const { return exitMs_; }
    int32_t LastState() const { return lastState_; }

private:
    void Enter(Phase p, uint64_t nowMs);
    void Finish(Outcome o);

    Limits   limits_;
    Phase    phase_ = Phase::Idle;
    Outcome  outcome_ = Outcome::None;
    uint64_t armedMs_ = 0;
    uint64_t phaseMs_ = 0;
    uint64_t exitMs_ = 0;
    int32_t  lastState_ = 0;
    bool     finishedEdge_ = false;
    bool     leftLevel_ = false;
};

}  // namespace reloadcycle
}  // namespace replay
