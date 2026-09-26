#include "playback/core/reload_cycle.h"

namespace replay {
namespace reloadcycle {

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::Idle:         return "idle";
        case Phase::AwaitLeave:   return "await-leave";
        case Phase::AwaitRequest: return "await-request";
        case Phase::AwaitExit:    return "await-exit";
        case Phase::AwaitEntry:   return "await-entry";
        case Phase::Done:         return "done";
    }
    return "?";
}

const char* OutcomeName(Outcome o) {
    switch (o) {
        case Outcome::None:           return "none";
        case Outcome::Cycled:         return "cycled";
        case Outcome::LeaveStuck:     return "leave-stuck";
        case Outcome::RequestRefused: return "request-refused";
        case Outcome::NoExit:         return "no-exit";
        case Outcome::SessionFailed:  return "session-failed";
        case Outcome::EntryTimedOut:  return "entry-timed-out";
        case Outcome::Cancelled:      return "cancelled";
    }
    return "?";
}

void Machine::Arm(uint64_t nowMs, bool entryAlreadyRequested) {
    outcome_ = Outcome::None;
    finishedEdge_ = false;
    armedMs_ = nowMs;
    exitMs_ = 0;
    leftLevel_ = false;
    Enter(entryAlreadyRequested ? Phase::AwaitExit : Phase::AwaitLeave, nowMs);
}

void Machine::Cancel() {
    if (Holds()) Finish(Outcome::Cancelled);
}

void Machine::Enter(Phase p, uint64_t nowMs) {
    phase_ = p;
    phaseMs_ = nowMs;
}

void Machine::Finish(Outcome o) {
    phase_ = Phase::Done;
    outcome_ = o;
    finishedEdge_ = true;
}

Action Machine::Step(const Observation& o) {
    lastState_ = o.netGameState;
    const uint64_t inPhase = o.nowMs - phaseMs_;
    switch (phase_) {
        case Phase::AwaitLeave:
            if (!o.leavePending) {
                Enter(Phase::AwaitRequest, o.nowMs);
                return Action::Request;
            }
            if (inPhase >= limits_.leaveMs) Finish(Outcome::LeaveStuck);
            return Action::None;
        case Phase::AwaitRequest:
            if (inPhase >= limits_.requestMs) {
                Finish(Outcome::RequestRefused);
                return Action::None;
            }
            return Action::Request;
        case Phase::AwaitExit:
            if (o.netGameState != kStateInGame) {
                exitMs_ = o.nowMs;
                leftLevel_ = true;
                Enter(Phase::AwaitEntry, o.nowMs);
                return Action::None;
            }
            if (inPhase >= limits_.exitMs) Finish(Outcome::NoExit);
            return Action::None;
        case Phase::AwaitEntry:
            if (o.netGameState == kStateInGame) {
                Finish(Outcome::Cycled);
            } else if (o.netGameState < 0) {
                Finish(Outcome::SessionFailed);
            } else if (inPhase >= limits_.entryMs) {
                Finish(Outcome::EntryTimedOut);
            }
            return Action::None;
        case Phase::Idle:
        case Phase::Done:
            return Action::None;
    }
    return Action::None;
}

void Machine::Answer(RequestAnswer answer, uint64_t nowMs) {
    if (phase_ != Phase::AwaitRequest) return;
    switch (answer) {
        case RequestAnswer::Accepted: Enter(Phase::AwaitExit, nowMs); break;
        case RequestAnswer::NotYet:   break;
        case RequestAnswer::Refused:  Finish(Outcome::RequestRefused); break;
    }
}

}  // namespace reloadcycle
}  // namespace replay
