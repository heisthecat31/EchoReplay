#include "engine/clock_policy.h"

#include <math.h>

namespace replay {
namespace clock {

const char* SampleKindName(SampleKind k) {
    switch (k) {
        case SampleKind::First: return "first";
        case SampleKind::Normal: return "normal";
        case SampleKind::Stalled: return "stalled";
        case SampleKind::Regressed: return "regressed";
        case SampleKind::Implausible: return "implausible";
        default: return "?";
    }
}

const char* MoveKindName(MoveKind k) {
    switch (k) {
        case MoveKind::None: return "none";
        case MoveKind::ForwardStep: return "forward-step";
        case MoveKind::Reverse: return "reverse";
        case MoveKind::Jump: return "jump";
        default: return "?";
    }
}

SampleKind ClassifySim(uint64_t prev, uint64_t now, uint64_t framesElapsed, const SimBounds& b,
                       uint64_t& deltaOut) {
    deltaOut = 0;

    // ★ THE ORDER IS THE POINT, and it is W2-B's rule applied to a second instrument. "Did the
    // source go backwards" is answered FIRST, in signed form. Asking afterwards returns the wrong
    // verdict on the way to the right one, because the unsigned difference has already turned a
    // regression into a number that is indistinguishable from a huge forward jump.
    if (now < prev) {
        return SampleKind::Regressed;
    }
    if (now == prev) {
        return SampleKind::Stalled;
    }
    // A bound computed from an absurd frame span is not a bound; it is an arithmetic that overflows
    // into a vacuous pass. Refuse before computing rather than after.
    if (framesElapsed > b.maxFramesSpanned || b.maxElapsedUs == 0 || b.maxBeatsPerFrame == 0) {
        return SampleKind::Implausible;
    }
    const uint64_t frames = framesElapsed ? framesElapsed : 1;
    const uint64_t bound = frames * b.maxBeatsPerFrame * b.maxElapsedUs;
    const uint64_t d = now - prev;
    if (d > bound) {
        return SampleKind::Implausible;
    }
    deltaOut = d;
    return SampleKind::Normal;
}

SampleKind ClassifyWall(uint64_t prevTicks, uint64_t nowTicks, uint64_t freqHz, uint64_t maxSpanUs,
                        uint64_t& deltaUsOut) {
    deltaUsOut = 0;
    if (freqHz == 0) {
        return SampleKind::Implausible;
    }
    if (nowTicks < prevTicks) {
        return SampleKind::Regressed;
    }
    if (nowTicks == prevTicks) {
        return SampleKind::Stalled;
    }
    // Split the conversion so the multiply never overflows: `ticks * 1000000` overflows a u64 after
    // about three weeks at a 10 MHz QPC, and a wrapped duration is a small plausible number.
    const uint64_t d = nowTicks - prevTicks;
    const uint64_t us = (d / freqHz) * 1000000ull + ((d % freqHz) * 1000000ull) / freqHz;
    if (maxSpanUs != 0 && us > maxSpanUs) {
        return SampleKind::Implausible;
    }
    deltaUsOut = us;
    return SampleKind::Normal;
}

void RateMeter::Reset(double decay) {
    if (!(decay > 0.0) || !(decay < 1.0)) {
        decay = 0.9;
    }
    decay_ = decay;
    simAcc_ = wallAcc_ = countAcc_ = 0.0;
    samples_ = rejects_ = 0;
    rejectRun_ = 0;
}

void RateMeter::Feed(uint64_t simDeltaUs, uint64_t wallDeltaUs) {
    simAcc_ = simAcc_ * decay_ + static_cast<double>(simDeltaUs);
    wallAcc_ = wallAcc_ * decay_ + static_cast<double>(wallDeltaUs);
    countAcc_ = countAcc_ * decay_ + 1.0;
    ++samples_;
    rejectRun_ = 0;
}

void RateMeter::Reject() {
    ++rejects_;
    // Saturate the RUN rather than let it wrap: this counter only ever gates a comparison against
    // kStaleAfter, and a wrap would silently un-stale a meter that has been dead for four billion
    // frames. Saturation is wrong for a measurement and right for a latch, and this is a latch.
    if (rejectRun_ != 0xFFFFFFFFu) {
        ++rejectRun_;
    }
}

bool RateMeter::Achieved(double& rateOut) const {
    if (samples_ == 0 || rejectRun_ >= kStaleAfter || !(wallAcc_ > 0.0)) {
        return false;
    }
    rateOut = simAcc_ / wallAcc_;
    return true;
}

bool RateMeter::FramePeriodUs(double& usOut) const {
    if (samples_ == 0 || rejectRun_ >= kStaleAfter || !(countAcc_ > 0.0) || !(wallAcc_ > 0.0)) {
        return false;
    }
    usOut = wallAcc_ / countAcc_;
    return true;
}

double Ceiling(uint64_t maxElapsedUs, double framePeriodUs) {
    if (maxElapsedUs == 0 || !(framePeriodUs > 0.0)) {
        return 0.0;
    }
    return static_cast<double>(maxElapsedUs) / framePeriodUs;
}

TimescalePlan PlanTimescale(double rate, double ceiling) {
    TimescalePlan p;

    // NaN reaches the float store as NaN and the heartbeat multiplies by it, so every comparison
    // downstream fails silently and the scaled delta becomes whatever cvttss2si makes of a NaN.
    // Every ordered comparison below is false for a NaN, so it would fall through untouched -- this
    // is the branch that stops it, and it has to be first.
    if (!(rate == rate)) {
        p.value = 1.0f;
        p.cappedByFloor = true;
        return p;
    }

    double m = fabs(rate);

    // Never zero: the heartbeat divides 1.0f by the scaled delta expressed in seconds, so a scale
    // that rounds the delta to zero puts +inf into CLOCK.RECIP_OFF and every consumer of it. The
    // 1 us floor this module writes to CLOCK.MINELAPSED_OFF already makes that unrepresentable;
    // this is the second, independent line, because one guard on a +inf is not enough guards.
    const double kMinTimescale = 1e-6;
    if (!(m > kMinTimescale)) {
        m = kMinTimescale;
        p.cappedByFloor = true;
    }

    // Above the ceiling the engine stops accelerating without saying so, so writing the requested
    // number would make the register disagree with the behaviour. Write what is achievable and let
    // the status report the gap -- silent degradation is the one thing the design forbids here.
    if (ceiling > 0.0 && m > ceiling) {
        m = ceiling;
        p.cappedByCeiling = true;
    }

    p.value = static_cast<float>(m);
    return p;
}

int64_t ClampPlayhead(int64_t tUs, uint64_t durationUs) {
    if (tUs < 0) {
        return 0;
    }
    const uint64_t cap = durationUs > 0x7FFFFFFFFFFFFFFFull ? 0x7FFFFFFFFFFFFFFFull : durationUs;
    return static_cast<uint64_t>(tUs) > cap ? static_cast<int64_t>(cap) : tUs;
}

MoveKind ClassifyPlayheadMove(int64_t prevUs, int64_t nowUs, uint64_t maxStepUs, uint64_t& stepOut) {
    stepOut = 0;
    if (nowUs == prevUs) {
        return MoveKind::None;
    }
    // Signed first, again. The three outcomes have three different remedies -- arm a step, arm
    // nothing, arm nothing -- and an unsigned difference would merge two of them into the third.
    if (nowUs < prevUs) {
        return MoveKind::Reverse;
    }
    const uint64_t d = static_cast<uint64_t>(nowUs) - static_cast<uint64_t>(prevUs);
    if (maxStepUs == 0 || d > maxStepUs) {
        return MoveKind::Jump;
    }
    stepOut = d;
    return MoveKind::ForwardStep;
}

bool EngineHeld(TransportState state, float rate) {
    // Enumerated, not defaulted: the defect this replaces was a state the list had never heard of,
    // and a `default` would swallow the next one the same way.
    switch (state) {
        case TransportState::Pause:
        case TransportState::Scrub:
        case TransportState::Seek:
        case TransportState::Step:
            return true;
        case TransportState::Play:
        case TransportState::NotLoaded:
            // Reverse is engine-paused by design: the engine has no backwards dt, so every
            // dt-driven integrator would run away from the pose we just published.
            return rate < 0.0f;
    }
    return rate < 0.0f;
}

uint64_t StepForMove(MoveKind mk, uint64_t moveStepUs, uint64_t floorUs) {
    switch (mk) {
        case MoveKind::ForwardStep:
            return moveStepUs;
        case MoveKind::Reverse:
        case MoveKind::Jump:
            return floorUs;
        case MoveKind::None:
            return 0;
    }
    return 0;
}

StepArmPlan PlanStepArm(bool stepEnabled, bool wantPaused, uint64_t pendingUs, bool armedLastFrame,
                        uint64_t maxUs) {
    StepArmPlan p;
    p.armUs = 0;
    p.refused = false;
    p.carry = false;
    if (pendingUs == 0) {
        return p;
    }
    if (!stepEnabled || !wantPaused || armedLastFrame) {
        p.refused = true;
        // Only the busy guard is transient. A step held against a clock that is not stepping, or
        // against an engine we are not pausing, would never become arm-able and would sit in the
        // slot forever, arming itself at whatever unrelated moment the conditions next allowed.
        p.carry = stepEnabled && wantPaused && armedLastFrame;
        return p;
    }
    p.armUs = (maxUs != 0 && pendingUs > maxUs) ? maxUs : pendingUs;
    return p;
}

}  // namespace clock
}  // namespace replay
