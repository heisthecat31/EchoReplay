#pragma once

// W2-D. The verdict logic for the three release checks in probe_clock, in one place so the OFFLINE
// suite can run it against the numbers launch 10 actually recorded.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS IS A HEADER AND NOT THREE `if`s IN THE PROBE.
//
// The probe needs a running engine, so a check that lives only inside it can never be shown to FAIL
// against the artifact it was written for -- it is asserted to work and then queued. These three
// take their inputs as plain integers, so tests/clock/test_clock.cpp feeds them the readings in
// runs/20260830-022305Z/launch-10/probes/probe_clock/done.json (CLOCK.FLAGS_OFF 0x2 after Release,
// CLOCK.MINELAPSED_OFF 1 against 0 at take) and asserts they report FAILED on them. That is S9.18:
// the check is proven against the defect, not against the fix.
//
// ★ AND THE DETAIL SAYS WHAT WAS SEEN. The version these replace emitted the SAME sentence -- "our
// two CLOCK.FLAGS_OFF bits are clear after Release()" -- whether the outcome was ok or failed, so
// the sweep's failure text asserted the opposite of what had happened and the reader had to know
// the code to read the result. A detail that does not change with the reading is decoration.
//
// ★ AND A CLOCK NOBODY TOOK IS NOT A RESTORED CLOCK. A module whose Take() refused leaves the
// engine's registers exactly as a perfect release would -- our bits clear, the engine's own
// minimum in place, held=false -- so without asking "was there ever a take" all three checks read
// ok for a run in which nothing was written. That reading is NOT-RUN, a fourth outcome, because
// "could not read the register" and "there was nothing to read for" are different answers.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>
#include <stdio.h>

#include "engine/clock.h"
#include "engine/clock_facts.h"

namespace replay {
namespace clockcheck {

// The four outcomes a check can carry. UNDETERMINED is "could not ask"; NOT-RUN is "there was
// nothing to ask about". Neither is a pass and neither is a failure, and the two-state form of
// this probe would have had to spell both as one of the others.
enum class Outcome { Undetermined, Ok, Failed, NotRun };

// ★ THE DECLARED COMPLETION POINT. W2-D.probe.json declares completesAt "match" and a 300 s budget.
// On the 20260830 sweep this probe finished its own phase machine on the MENU, wrote done.json at
// 16.1 s -- which is what ends the launch -- and the sweep's headline clock result was therefore
// measured on a launch that never reached level load. Nothing in the verdict said so.
//
// A probe answering earlier than it declared has not answered the question it was queued for. It is
// not a failure of the engine and it is not a pass: it is a TRUNCATED run, and the word for that
// here is undetermined. The scored readings keep their own outcomes and are not touched.
inline Outcome CompletionReached(bool matchObserved, uint32_t matchTick, uint32_t ticks,
                                 char* detail, size_t bytes) {
    if (matchObserved) {
        _snprintf_s(detail, bytes, _TRUNCATE,
                    "a CR15NetGame was live at GAME.PTR_VA+0x%X on tick %u of %u, so the readings "
                    "below were taken on the match this probe declares it completes at",
                    facts::GAME_NETGAME_PTR_OFF, matchTick, ticks);
        return Outcome::Ok;
    }
    _snprintf_s(detail, bytes, _TRUNCATE,
                "TRUNCATED: the descriptor declares completesAt \"match\", and across %u ticks "
                "CR15Game+0x%X was never non-null -- no match was ever entered. Every reading in "
                "this file was taken before the declared completion point, on whatever the client "
                "was showing, and none of them is evidence about a running match. Writing done.json "
                "is what ends the launch, so this run stopped itself short of its own budget.",
                ticks, facts::GAME_NETGAME_PTR_OFF);
    return Outcome::Undetermined;
}

inline const char* OutcomeName(Outcome o) {
    switch (o) {
        case Outcome::Ok: return "ok";
        case Outcome::Failed: return "failed";
        case Outcome::NotRun: return "not-run";
        default: return "undetermined";
    }
}

// Everything read AFTER IReplayClock::Release() returned, plus the pre-release facts the checks
// need. Filled by the probe from engine memory; filled by the offline suite from the sweep.
struct ReleaseReading {
    bool     everHeldRead;          // Status() was read at some point, so everHeld means something
    bool     everHeld;              // the module held the clock at some point before Release()
    bool     released;              // Release() was actually reached
    bool     registersRead;         // the post-release read of the clock object landed
    uint64_t flags;                 // CLOCK.FLAGS_OFF
    uint64_t minElapsedUs;          // CLOCK.MINELAPSED_OFF
    bool     minElapsedAtTakeRead;  // the module reported what was there before we wrote
    uint64_t minElapsedAtTakeUs;    // and that value
    bool     statusRead;            // Status() was read after Release()
    ClockRefusal refusal;           // what it said
    bool     held;                  // whether the module still holds the clock
    bool     latched;               // and whether it says Release() has run (Status().released)
};

// ★ THE ENGAGED PRECONDITION, SHARED BY ALL THREE. "Restored" is a claim about a clock that was
// taken; for one that never was, every register reads as restored and the module reads as stopped,
// which is how three green verdicts came to be possible for a run that wrote nothing. Returns true
// when the check may go on to read its registers; otherwise the outcome and detail are filled.
inline bool Engaged(const ReleaseReading& r, char* detail, size_t cap, Outcome& out) {
    if (!r.everHeldRead) {
        snprintf(detail, cap, "whether the module ever held the clock was never read, so there is "
                              "no way to say whether there was anything to restore");
        out = Outcome::Undetermined;
        return false;
    }
    if (!r.everHeld) {
        snprintf(detail, cap,
                 "NOT RUN: the module never held the clock (refusal=%u), so nothing was written "
                 "and there is nothing to have restored; see clock-took for why",
                 (unsigned)r.refusal);
        out = Outcome::NotRun;
        return false;
    }
    if (!r.released) {
        snprintf(detail, cap, "Release() was not reached");
        out = Outcome::Undetermined;
        return false;
    }
    return true;
}

// ★ OUR TWO BITS, AND ONLY OURS. TICK.UPDATE_CLOCK read-modify-writes this same qword every frame
// (it clears bits 3 and 4 and sets bit 0), so a whole-word comparison against a snapshot would go
// red on the engine's own housekeeping. The claim is exactly "the two bits the transport owns are
// clear", and the near-miss control in the offline suite sets every OTHER bit to prove it.
inline Outcome FlagsRestored(const ReleaseReading& r, char* detail, size_t cap) {
    Outcome gate;
    if (!Engaged(r, detail, cap, gate)) {
        return gate;
    }
    if (!r.registersRead) {
        snprintf(detail, cap, "Release() ran but CLOCK.FLAGS_OFF could not be read back");
        return Outcome::Undetermined;
    }
    const uint64_t ours = r.flags & (clock_facts::kPauseBit | clock_facts::kStepOnceBit);
    if (ours != 0) {
        snprintf(detail, cap,
                 "CLOCK.FLAGS_OFF is 0x%llX after Release(): %s%s%s still set, so the module is "
                 "still driving the engine's clock after being told to stop",
                 (unsigned long long)r.flags,
                 (ours & clock_facts::kPauseBit) ? "PAUSE bit 1" : "",
                 (ours == (clock_facts::kPauseBit | clock_facts::kStepOnceBit)) ? " and " : "",
                 (ours & clock_facts::kStepOnceBit) ? "STEPONCE bit 2" : "");
        return Outcome::Failed;
    }
    snprintf(detail, cap,
             "CLOCK.FLAGS_OFF is 0x%llX after Release(): both bits we own (1 and 2) are clear, and "
             "the engine's other bits are left as the engine left them",
             (unsigned long long)r.flags);
    return Outcome::Ok;
}

// ★ THE FLOOR IS THE HALF THAT CANNOT BE CLEARED BY ACCIDENT. Nothing in the engine writes
// CLOCK.MINELAPSED_OFF -- that is what min-elapsed-unwritten-by-anyone-else establishes -- so if the
// 1 us floor is still there after a Release() that stores the snapshot back, the only thing that can
// have written it is US, taking the clock again on the following tick.
inline Outcome FloorRestored(const ReleaseReading& r, char* detail, size_t cap) {
    Outcome gate;
    if (!Engaged(r, detail, cap, gate)) {
        return gate;
    }
    if (!r.registersRead || !r.minElapsedAtTakeRead) {
        snprintf(detail, cap,
                 "Release() ran but %s could not be read, so there is nothing to compare",
                 !r.registersRead ? "CLOCK.MINELAPSED_OFF" : "the value at take");
        return Outcome::Undetermined;
    }
    // Equal-valued fields are untestable: if the engine's own minimum were already the floor we
    // write, a restore and a leak would produce the same number and this check would pass for both.
    if (r.minElapsedAtTakeUs == clock_facts::kFloorUs) {
        snprintf(detail, cap,
                 "CLOCK.MINELAPSED_OFF was already %llu at take -- the value we write -- so a "
                 "restore and a leaked floor are indistinguishable here",
                 (unsigned long long)r.minElapsedAtTakeUs);
        return Outcome::Undetermined;
    }
    if (r.minElapsedUs != r.minElapsedAtTakeUs) {
        snprintf(detail, cap,
                 "CLOCK.MINELAPSED_OFF is %llu after Release() and was %llu at take%s",
                 (unsigned long long)r.minElapsedUs, (unsigned long long)r.minElapsedAtTakeUs,
                 r.minElapsedUs == clock_facts::kFloorUs
                     ? ": OUR FLOOR IS STILL INSTALLED, which only a take after the release can do"
                     : ": the field is neither ours nor the engine's original");
        return Outcome::Failed;
    }
    snprintf(detail, cap,
             "CLOCK.MINELAPSED_OFF is back to the %llu it held at take, so the 1 us floor was "
             "removed and not re-installed by a later take",
             (unsigned long long)r.minElapsedAtTakeUs);
    return Outcome::Ok;
}

// ★ THE REGISTERS ARE THE EFFECT; THIS IS THE CAUSE. Bits that read clear one frame after Release()
// are also what a module that has not re-taken YET looks like, so the module's own account of
// itself is asked too: released means not held, the latch set, and a refusal that names the
// release -- three fields for one fact, so a module that sets one and not the others is seen.
inline Outcome StoppedAfterRelease(const ReleaseReading& r, char* detail, size_t cap) {
    Outcome gate;
    if (!Engaged(r, detail, cap, gate)) {
        return gate;
    }
    if (!r.statusRead) {
        snprintf(detail, cap, "Status() was not read after Release()");
        return Outcome::Undetermined;
    }
    if (r.held || !r.latched || r.refusal != ClockRefusal::Released) {
        snprintf(detail, cap,
                 "after Release() the module reports held=%s released=%s refusal=%u: %s",
                 r.held ? "true" : "false", r.latched ? "true" : "false", (unsigned)r.refusal,
                 r.held ? "it has taken the clock again rather than staying released"
                        : "the latch and the refusal do not both say released, so the next tick "
                          "may take the clock again");
        return Outcome::Failed;
    }
    snprintf(detail, cap,
             "after Release() the module reports held=false released=true refusal=released, so "
             "the take gate is latched and no later tick re-writes the fields put back above");
    return Outcome::Ok;
}

}  // namespace clockcheck
}  // namespace replay
