// The two scoring decisions AUDIT_F3E F3E-2 rejected, lifted out of probe_ui.cpp so they can be
// red offline. probe_ui.cpp is a launch-only DLL: nothing offline can reach the code that decides
// whether a window is a FAIL or a could-not-ask, and a decision no test can red is a decision the
// next launch discovers. These two functions are the decision; probe_ui.cpp calls them and does
// nothing else with the condition.
//
// DESIGN S8.5.4a consequence 3: "A launch that ends before any draw is could-not-ask, never FAIL.
// `marshalledDraws == 0` on a window that never reached a drawable frame is a censored
// measurement." And S9 rule 1: a two-state gate cannot carry asked-and-failed apart from
// could-not-ask. (Since AUD-ARCH F-B the counter that answers "was there a drawable frame" is
// ownerDraws; marshalledDraws narrowed to the band-3-consumed subset. See DrawWindowSinceEnable.)
#pragma once

#include <stdint.h>

#include "ui/csregister.h"

namespace probeui {

// ★★ THE POPULATION BOTH RULES BELOW ARE SCORED OVER (AUDIT_F3I F3I-3). ARCH-13 s2.3 states its
// coverage rule over `drawsAfter / ownerTicksAfter` -- values ACROSS A STAMP -- and `ownerTicks`
// in the Report is a LIFETIME total that starts at the module's first tick, above every early
// return in OnTick. Scored on the lifetime totals, a launch that spent 600 ticks with the panel
// closed and then drew on all 3400 of the rest files as 599 missing frames against a panel that
// drew on every frame it was allowed to. So the stamp is the panel's own enable, csregister
// publishes both counters at it, and everything below is scored on the DIFFERENCE.
//
// `valid == false` is a could-not-ask, never a FAIL, and it has exactly two causes: the panel was
// never opened (nothing was ever asked of the draw), or a counter went BACKWARDS across the stamp,
// which is ARCH-13 s2.3 item 4 -- the module was reset under the observer.
struct DrawWindow {
    uint64_t draws = 0;
    uint64_t ownerTicks = 0;
    bool valid = false;
    bool everOpened = false;
};

inline DrawWindow DrawWindowSinceEnable(const replay::ui::csregister::Report& r) {
    DrawWindow w;
    w.everOpened = r.enableStamps != 0;
    if (!w.everOpened) return w;
    // ownerDraws, not marshalledDraws (AUD-ARCH F-B): the coverage rule asks whether the panel
    // was on screen on every frame, and that is every draw at any site. marshalledDraws is the
    // band-3-consumed SUBSET and is S8.5.4a leg 3, a different question with a different answer.
    if (r.ownerDraws < r.ownerDrawsAtEnable) return w;
    if (r.ownerTicks < r.ownerTicksAtEnable) return w;
    w.draws = r.ownerDraws - r.ownerDrawsAtEnable;
    w.ownerTicks = r.ownerTicks - r.ownerTicksAtEnable;
    w.valid = true;
    return w;
}

// ★ ARCH-13 844d21b s1.3: THE CENSORING PREDICATE IS OWNER TICKS, AND ONLY OWNER TICKS. Band-3
// entries censor nothing -- under the render-cadence rule a window with owner ticks and no entries
// is a window the panel should have drawn on every one of, so zero draws there is an ANSWER.
// `ownerTicks == 0` is the drain never being reached, and `marshalledDraws == 0` is then a lower
// bound (">= 0"), not a verdict. The second tick is ARCH-13 s2.3's one-tick slack, and it is not
// slop: the enable can be stamped part-way through an owner tick whose own drain has already run,
// so the first tick that can draw is the second one of the window.
inline bool DrawWindowIsCensored(uint64_t marshalledDraws, uint64_t ownerTicks) {
    return marshalledDraws == 0 && ownerTicks < 2;
}

// ★ ARCH-13 s2.3 item 1, the COVERAGE rule: `drawsAfter >= ownerTicksAfter - 1`. Existence is not
// coverage -- one draw in four thousand ticks is exactly the flickering panel the ruling forbids,
// and it used to be a pass. Written as `draws + 1 >= ticks` so it cannot underflow at ticks == 0.
// BOTH arguments are since-stamp differences: see DrawWindowSinceEnable.
inline bool DrawCoverageHolds(uint64_t marshalledDraws, uint64_t ownerTicks) {
    return marshalledDraws + 1 >= ownerTicks;
}

// May the sampler stop waiting on this status? Only for a state that is already a determination.
// `Unconfirmed` (never dispatched) and `Invoked` (dispatched, never drawn) are the two states the
// draw check SCORES, and their only remedy is more time: breaking on them shortened the 150 s
// window to about ten seconds and then recorded the short window as a verdict, which is the defect
// F3E-2 names. `CallbackSilent` and `MarshalStale` are different -- both mean the callback fired
// AND the panel drew and then something stopped, so the answer is already in hand.
inline bool SamplerMayBreakOn(replay::ui::csregister::Status status, uint64_t marshalledDraws) {
    using replay::ui::csregister::Status;
    if (marshalledDraws == 0) return false;
    return status == Status::CallbackSilent || status == Status::MarshalStale;
}

}  // namespace probeui
