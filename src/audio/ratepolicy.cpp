#include "audio/ratepolicy.h"

#include "audio/sinkvoice.h"

namespace replay {
namespace audio {
namespace {

float Abs(float v) { return v < 0.0f ? -v : v; }
double AbsD(double v) { return v < 0.0 ? -v : v; }

}  // namespace

const char* RateModeName(RateMode m) {
    switch (m) {
        case RateMode::Normal: return "normal";
        case RateMode::Slowed: return "slowed";
        case RateMode::Accelerated: return "accelerated";
        case RateMode::ClampedFast: return "clamped-fast";
        case RateMode::Suspended: return "suspended";
        case RateMode::Unavailable: return "unavailable";
        case RateMode::Muted: return "muted";
        default: return "unknown";
    }
}

RatePlan PlanRate(const RateCommand& cmd) {
    RatePlan plan;
    // Direction is settled BEFORE availability, not after it. A reversed transport under a dead
    // sink is still reversed, and a label that reports only the refusal reads as if the picture
    // were running forward (L3-RUN-24 clause 5).
    plan.reversed = cmd.rate < 0.0f;

    if (!cmd.available) {
        // Everything else in the product is unaffected; audio simply plays at 1.0x. Refusing to
        // suspend here matters: a build whose proofs failed must still make sound.
        plan.mode = RateMode::Unavailable;
        plan.ratio = 1.0f;
        plan.pitchIsHonest = Abs(cmd.rate - 1.0f) < 1e-6f && !cmd.paused;
        return plan;
    }

    if (cmd.paused || cmd.rate == 0.0f) {
        // Suspend, not ratio zero. Two reasons and both are load-bearing: XAudio2 refuses a ratio
        // below its own minimum rather than scaling to it, and the design's clock rule is that a
        // scaled delta must never reach zero because the engine derives a reciprocal from it.
        plan.mode = RateMode::Suspended;
        plan.suspend = true;
        plan.ratio = 1.0f;
        return plan;
    }

    // ★ REVERSE IS SILENCE. The sink has no backwards mix, so every earlier answer here played the
    // recording FORWARD under a backward picture -- a mix nobody made, at the pitch of a moment the
    // viewer is not looking at. Muting says the one true thing, once, and suspending is the same
    // mechanism pause already uses, so leaving reverse wakes it with no glitch.
    if (plan.reversed) {
        plan.mode = RateMode::Muted;
        plan.suspend = true;
        plan.ratio = 1.0f;
        return plan;
    }

    const float speed = Abs(cmd.rate);

    if (speed <= 1.0f) {
        plan.ratio = speed < kXAudio2MinFreqRatio ? kXAudio2MinFreqRatio : speed;
        plan.mode = plan.ratio == 1.0f ? RateMode::Normal : RateMode::Slowed;
        // A clamp at the floor is a real, if tiny, dishonesty and it is reported as one rather
        // than rounded away.
        plan.pitchIsHonest = plan.ratio == speed;
        return plan;
    }

    // Above 1.0x. XAudio2 clamps SetFrequencyRatio to MaxFrequencyRatio SILENTLY, so a ratio the
    // installed maximum cannot hold must not be commanded and then reported as if it had taken.
    const float ceiling = cmd.maxRate < 1.0f ? 1.0f : cmd.maxRate;
    if (speed <= ceiling) {
        plan.ratio = speed;
        plan.mode = RateMode::Accelerated;
        return plan;
    }
    plan.ratio = ceiling;
    plan.mode = ceiling == 1.0f ? RateMode::ClampedFast : RateMode::Accelerated;
    plan.pitchIsHonest = false;
    return plan;
}

const char* RateLabel(const RatePlan& plan, bool rateHookEnabledInConfig) {
    // Total over the enum, with no default arm: RateMode::Normal used to fall through one and
    // return the empty string, which printed `transport paused: audio suspended at 1.00x ()` --
    // a line that says nothing and reads like a missing diagnostic.
    //
    // ★ Only two modes can carry a direction now: Unavailable, which keeps the sign because a line
    // carrying only the refusal read as a forward picture, and Muted, whose whole sentence IS the
    // direction. Every other playing mode is forward by construction, because PlanRate mutes reverse
    // before it reaches them. It used to ride |r| instead, and at -1.00x the ledger said
    // "audio: 1.0x, as the game mixed it" (L3-RUN-13 and RUN-14 F4) about a mix nobody made.
    switch (plan.mode) {
        case RateMode::Normal:
            return "audio: 1.0x, as the game mixed it";
        case RateMode::Unavailable:
            // Two facts, and the reader needs both: a line carrying only the refusal was read as a
            // forward picture (L3-RUN-24 clause 5). AUD-FINAL (a)'s sentence stays whole, and the
            // cause DescribeAudio appends still lands after it.
            return plan.reversed ? "reverse: audio rate control unavailable"
                                 : "audio rate control unavailable";
        case RateMode::ClampedFast:
            return rateHookEnabledInConfig ? "FF audio: 1.0x pitch (rate hook unavailable)"
                                           : "FF audio: 1.0x pitch (hook disabled)";
        case RateMode::Suspended:
            return "audio suspended";
        case RateMode::Slowed:
            return plan.pitchIsHonest ? "audio: slowed with picture"
                                      : "audio: at the minimum ratio the device accepts";
        case RateMode::Accelerated:
            return "audio: sped with picture";
        case RateMode::Muted:
            // The one sentence reverse gets, and it does not change while reverse lasts.
            return "audio: muted while the picture runs backward";
        case RateMode::Count:
            break;
    }
    return "audio: rate mode not classified";
}

// ---- AvBias -----------------------------------------------------------------------------------

void AvBias::Reset() {
    offsetUs_ = 0;
    observed_ = 0;
    latched_ = false;
}

void AvBias::Observe(uint32_t buffersQueued, uint32_t samplesPerFrame, uint32_t samplesPerSecond) {
    if (latched_ || samplesPerFrame == 0 || samplesPerSecond == 0) {
        return;
    }
    const int64_t queuedSamples = static_cast<int64_t>(buffersQueued) * samplesPerFrame;
    const int64_t us = (queuedSamples * 1000000) / samplesPerSecond;
    if (us > offsetUs_ && us <= kMaxPlausibleUs) {
        offsetUs_ = us;
    }
    if (++observed_ >= kWarmupSamples) {
        latched_ = true;
        // A depth of zero across the whole warm-up means the voice was never fed, which is not a
        // measurement of "no latency" -- it is the absence of a measurement, and applying it as a
        // zero bias would be right by accident.
        if (offsetUs_ <= 0) {
            latched_ = false;
            observed_ = 0;
        }
    }
}

// ---- RateLock ---------------------------------------------------------------------------------

void RateLock::Reset() {
    trim_ = 0.0f;
    lastError_ = 0.0;
    primed_ = false;
    errorBase_ = 0.0;
}

void RateLock::Reprime() {
    // Reset() also clears lastError_ and is what a rate CHANGE wants. This is narrower on purpose:
    // the two callers mean different things, and collapsing them would hide which one ran.
    trim_ = 0.0f;
    primed_ = false;
    errorBase_ = 0.0;
}

float RateLock::Update(float commandedRatio, double audioSeconds, double sceneSeconds,
                       double dtWall) {
    if (dtWall <= 0.0 || commandedRatio <= 0.0f) {
        return commandedRatio;
    }
    if (!primed_) {
        // The two clocks have different origins and we do not care what they are; only their
        // relative drift matters. Priming records the offset instead of trying to remove it.
        errorBase_ = audioSeconds - sceneSeconds;
        primed_ = true;
        lastError_ = 0.0;
        return commandedRatio;
    }

    const double error = (audioSeconds - sceneSeconds) - errorBase_;
    lastError_ = error;

    // Audio ahead of the scene clock means the sink is consuming too fast, so the trim is negative.
    double delta = -error * kGainPerSecond * dtWall;
    trim_ = static_cast<float>(trim_ + delta);
    if (trim_ > kMaxTrim) {
        trim_ = kMaxTrim;
    } else if (trim_ < -kMaxTrim) {
        trim_ = -kMaxTrim;
    }

    float corrected = commandedRatio * (1.0f + trim_);
    if (corrected < kXAudio2MinFreqRatio) {
        corrected = kXAudio2MinFreqRatio;
    }
    return corrected;
}

// ---- the O4 verdict ---------------------------------------------------------------------------

const char* RateVerdictName(RateVerdict v) {
    switch (v) {
        case RateVerdict::Pass: return "PASS";
        case RateVerdict::FailMechanism: return "FAIL (the ratio did not take)";
        case RateVerdict::NotRunStarved: return "NOT-RUN (the machine was starved, not the mechanism)";
        case RateVerdict::NotRunInvalid: return "NOT-RUN (Wwise is not up)";
        case RateVerdict::NotRunCounterReset:
            return "NOT-RUN (a counter reset between samples -- a device change, not a failure)";
        default: return "unknown";
    }
}

bool StepForward(double previous, double current, double* delta) {
    if (current < previous) {
        return false;
    }
    if (delta != nullptr) {
        *delta = current - previous;
    }
    return true;
}

RateVerdict JudgeRate(const RateSample& s, double commanded, double tolerance) {
    if (!s.settingsValid) {
        return RateVerdict::NotRunInvalid;
    }
    // Before the discrimination, not after it: a regressed counter produces numbers that look
    // exactly like a mechanism failure, so judging first and checking second would return the wrong
    // verdict on the way to the right one.
    if (s.sourceRegressed) {
        return RateVerdict::NotRunCounterReset;
    }
    const bool deviceTracks = AbsD(s.deviceRatio - commanded) <= tolerance;
    const bool wwiseTracks = AbsD(s.wwiseRatio - commanded) <= tolerance;

    // The discrimination, and it is the whole reason there are two readings. If XAudio2 itself did
    // not consume at the commanded rate, the ratio never took and that is a mechanism failure. If
    // the device consumed correctly but Wwise's frame counter lagged, the sound engine was starved
    // upstream -- a machine problem, and reporting it as a broken mechanism would send someone to
    // "fix" working code.
    if (!deviceTracks) {
        return RateVerdict::FailMechanism;
    }
    if (!wwiseTracks) {
        return RateVerdict::NotRunStarved;
    }
    return RateVerdict::Pass;
}

}  // namespace audio
}  // namespace replay
