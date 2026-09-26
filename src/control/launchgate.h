#pragma once

// W2-G. "May the ECHOREPLAY_OPEN load be submitted YET?", as a pure function, so tests/control can
// sweep a recorded NetGame sequence without a game.
//
// ★ WHY THIS EXISTS: L3 F1, the run that had no world. ServiceLaunchRequest submitted `load` as
// soon as `g_services->transport` was non-null -- a plugin-side condition that says nothing about
// the engine. On L3 that was NetGame state 1 (loading root), 1.7 s BEFORE the engine attempted
// login. The load reached ui::LoadPath, whose MustEnter arm called ISessionEntry::RequestEntry,
// and the stock entry call tore the half-built session down: the engine then reported
// "One or more prerequisites are missing" and sat in state -94 (login failed) for the remaining
// 4.5 minutes, retrying a dead loopback 25 times.
//
// So the transport being registered is necessary and NOT sufficient. The engine must also have
// reached a state at which entering a session is a thing it can survive -- NetGameState's own
// 7 (loading level) / 8 (ready for game) / 9 (in game). Below that we WAIT.
//
// ★ A WAIT IS THE THIRD ANSWER, and the reason this is a gate and not a retry. "Submit" and
// "give up" are the only two answers the old code had, so "not yet" had to be spelled as one of
// them and it was spelled as "submit". Wait / Submit / GiveUp are three, and the request is never
// dropped: every path ends at Submit or at GiveUp, and GiveUp carries the reason it gave up on.
//
// ★ AND THE REFUSAL RETRY IS KEYED ON THE STATE, NOT ON A CLOCK. W1-F's precondition in
// RequestEntry refuses a load that arrives too early. Retrying that on a timer asks the same
// question of the same state and gets the same answer; the thing that can change the answer is
// the engine changing state, so that is what the retry waits for. The wall clock is still the
// backstop, because a state that never changes must not wait forever.

#include <stdint.h>

namespace replay {
namespace control {

// The first NetGame state at which a session entry is survivable. NetGameState names 7, 8 and 9;
// this is its lowest member and is compared as the raw int, because ISessionEntry::State() casts
// the engine's raw value -- which is wider, and negative on failure -- straight into that enum.
inline constexpr int32_t kLoadReadyState = 7;

enum class LaunchStep : uint8_t {
    Wait,    // conditions are not met; do nothing this tick and ask again on the next one
    Submit,  // submit the load now
    GiveUp,  // bounded out: open the browser instead, naming the reason
};

struct LaunchGateInput {
    bool     transportPresent = false;
    bool     sessionPresent   = false;
    // ISessionEntry::State() as a raw int. 0 when there is no session service to ask -- which is
    // why `sessionPresent` is its own field: "the engine is at state 0" and "nobody has told us
    // what state the engine is at" are different facts and must not share a representation.
    int32_t  netGameState     = 0;
    uint32_t attempts         = 0;
    uint32_t maxAttempts      = 0;
    // The last submitted load came back refused, and the state it was refused at. Until the
    // engine leaves that state, asking again cannot produce a different answer.
    bool     refusalPending   = false;
    int32_t  refusedAtState   = 0;
    // The wall-clock retry gap has elapsed. Only consulted for a retry that is NOT a refusal --
    // an answer that fell out of the result ring before it was read, where the outcome is
    // genuinely unknown and a state change is not the thing being waited for.
    bool     retryDue         = false;
    bool     outOfTime        = false;
};

// `why` is always written, on every answer, because the caller logs it and the GiveUp arm puts it
// in front of the owner. A GiveUp whose message says "gave up" and not what it gave up on is the
// refusal that sends someone to the source.
inline LaunchStep DecideLaunchStep(const LaunchGateInput& in, const char** why) {
    const char* reason = nullptr;
    LaunchStep blocked = in.outOfTime ? LaunchStep::GiveUp : LaunchStep::Wait;

    if (!in.transportPresent) {
        reason = "no replay transport has registered yet";
    } else if (!in.sessionPresent) {
        reason = "the session service has not registered, so the engine's state cannot be read";
    } else if (in.netGameState < kLoadReadyState) {
        // The one L3 F1 fixes. Loading here tears down a session the engine is still building.
        reason = "NetGame has not reached level load (state 7); entering a session before the "
                 "engine has logged in wedges it in login failed";
    } else if (in.attempts >= in.maxAttempts) {
        reason = "the load was refused on every attempt";
        blocked = LaunchStep::GiveUp;
    } else if (in.refusalPending && in.netGameState == in.refusedAtState) {
        reason = "the load was refused and NetGame has not changed state since, so asking again "
                 "would ask the same question of the same state";
    } else if (!in.refusalPending && in.attempts != 0 && !in.retryDue) {
        reason = "the previous attempt's answer was lost; waiting out the retry gap";
    } else if (in.outOfTime) {
        reason = "the wait limit elapsed before the load could be submitted";
        blocked = LaunchStep::GiveUp;
    } else {
        if (why != nullptr) {
            *why = "the transport is registered and NetGame is at or past level load";
        }
        return LaunchStep::Submit;
    }

    if (why != nullptr) {
        *why = reason;
    }
    return blocked;
}

}  // namespace control
}  // namespace replay
