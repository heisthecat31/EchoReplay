#pragma once

// The rate ladder, the A/V bias and the O4 verdict, as pure functions.
//
// Nothing here touches the engine, so all three are driven end to end by tests/audio on a machine
// with no game. That is deliberate: the parts of S10.4 that can be wrong in an interesting way are
// the POLICY (what ratio should a commanded rate produce, and what does the user get told when it
// cannot), the OFFSET (S10.4.1a, which no rate oracle can see), and the DISCRIMINATION (S12.4's
// two oracles, whose whole purpose is to tell a broken mechanism from a busy machine). None of
// those needs a sound card to be checked, and a design that could only check them in vivo would
// have checked them once.

#include <stdint.h>

namespace replay {
namespace audio {

// What the transport asked for, and what the sink can actually do about it.
struct RateCommand {
    float rate = 1.0f;        // the transport's rate; negative means reverse
    bool paused = false;
    bool available = true;    // false when the S10.4.2 proofs refused
    float maxRate = 1.0f;     // 1.0 without the Level-1X MaxFrequencyRatio hook
};

// How rate is served. Exactly one of these is true of any plan, which is what lets
// IReplayTransport::ActiveModeLabel carry a single honest string.
enum class RateMode : uint8_t {
    Normal = 0,        // ratio 1.0, bit-identical to ordinary play
    Slowed,            // 0 < r < 1: full mix at r speed and r pitch
    Accelerated,       // r > 1 and the installed MaxFrequencyRatio allows it
    ClampedFast,       // r > 1 but MaxFrequencyRatio is 1.0: 1.0x pitch, and the UI says so
    Suspended,         // paused, or a seek mute: device held open, no glitch on resume
    Unavailable,       // the proofs refused; audio plays at 1.0x whatever the transport does
    Muted,             // the picture runs backward and the sink can only play forward
    Count
};
const char* RateModeName(RateMode);

struct RatePlan {
    RateMode mode = RateMode::Normal;
    float ratio = 1.0f;       // what to hand SetFrequencyRatio; always inside XAudio2's own clamp
    bool suspend = false;     // Suspend(true) rather than a ratio
    bool pitchIsHonest = true;  // false when the ratio no longer matches the transport rate
    bool reversed = false;      // the transport runs backward; the sink can only play forward
};

// Reverse rides |r|: the design E-A7. Below XAudio2's own minimum the call would be refused rather
// than scaled, so the floor is real and the plan reports it by clamping rather than by failing.
RatePlan PlanRate(const RateCommand&);

// The label S10.4.5 requires. Never empty, and never silent about a degradation: "FF audio: 1.0x
// pitch (rate hook unavailable)" is a sentence a user can act on, where a missing label is a
// fidelity bug nobody attributes.
const char* RateLabel(const RatePlan&, bool rateHookEnabledInConfig);

// ---- S10.4.1a: the A/V offset, measured in SCENE time -----------------------------------------
//
// ★ THE CORRECTION THIS EXISTS FOR. The sink's buffering is flat in scene time -- a fixed number of
// audio frames -- so it is 85 ms of CONTENT at every rate. In WALL time it is 85/r ms, which is
// 853 ms at 0.1x: audio nearly a second behind picture in the mode the owner asked for by name.
// Because the scene-time figure is the constant one, the correction is a constant subtraction from
// the DISPLAY playhead and needs no per-rate table.
//
// ★ AND IT IS MEASURED. 85 ms is 8 x 512 / 48000 at one device's settings; it moves with sample
// rate and buffer count. samplesPerFrame and samplesPerSecond come from GetAudioSettings and the
// queue depth from XAUDIO2_VOICE_STATE, both at run time. A hardcoded 85 would reintroduce exactly
// the class of defect S12.4 found one layer down.
class AvBias {
public:
    // Warm-up length. The queue depth swings while the voice fills, and a bias that tracked the
    // swing would make the picture jitter -- so the maximum over the warm-up is taken and LATCHED,
    // which is what "the bias is a constant" means operationally.
    static constexpr uint32_t kWarmupSamples = 32;
    // ★ THE BUDGET IS WALL TIME, THE WARM-UP IS A SAMPLE COUNT. kWarmupSamples observations latch a
    // bias in that many ticks whatever the frame rate, which is correct. The budget around it must
    // not be a tick count: four warm-up lengths was 4.27 s at the fixed 30 Hz step the viewer used
    // to be locked to and would silently become 0.9 s at 144 fps. Stated as the seconds it was.
    static constexpr uint64_t kBiasWarmupBudgetMs = 4267;
    static constexpr uint64_t kBiasWarmupRetryMs = 1067;
    // Anything past this is not a buffer depth, it is a misread, and it is refused rather than
    // applied: half a second of video lag would be worse than the defect it corrects.
    static constexpr int64_t kMaxPlausibleUs = 500000;

    void Observe(uint32_t buffersQueued, uint32_t samplesPerFrame, uint32_t samplesPerSecond);
    void Reset();   // a device change invalidates the measurement (E-A1)

    bool Latched() const { return latched_; }
    // Scene microseconds to subtract from the DISPLAY playhead. Zero until latched, and zero is the
    // correct answer then: no correction is better than a wrong one.
    int64_t OffsetSceneUs() const { return latched_ ? offsetUs_ : 0; }
    uint32_t SamplesObserved() const { return observed_; }

private:
    int64_t offsetUs_ = 0;
    uint32_t observed_ = 0;
    bool latched_ = false;
};

// ---- the closed loop (S10.4.3 use 2) ----------------------------------------------------------
//
// Open-loop rate control is an assumption. Wwise's own frame counter says what the audio clock
// actually did, and a slow proportional correction keeps it tracking the replay's scene clock so
// the A/V offset stays at its constant instead of accumulating device drift and LEngine hitches.
//
// Slow on purpose: the correction is a fraction of the error per second, so a hitch is absorbed
// over a second rather than heard as a pitch step. It is also bounded, because an unbounded
// integrator chasing a starved machine would command a ratio the mechanism cannot hold and turn a
// performance problem into an audible one.
class RateLock {
public:
    static constexpr float kMaxTrim = 0.05f;    // +/- 5% of the commanded ratio, and no more
    static constexpr float kGainPerSecond = 0.25f;

    void Reset();

    // ★ THE N-5 SUBSCRIPTION, AND THIS CLASS IS EXACTLY THE SHAPE S7.5.5 NAMES. It integrates
    // `(audioSeconds - sceneSeconds) - errorBase_`, where the base is taken once -- an accumulator
    // computing clockTime minus a base, which is the definition of an N-5 primitive. Carried across
    // a playhead jump it corrects a difference the SEEK created rather than one the device drifted,
    // and the only lever it has is detuning the mix. So a discontinuity abandons the correction and
    // re-takes the base: on a jump a cache is invalidated, never updated (INV-P).
    //
    // Abandoning rather than scaling is deliberate. There is no correct trim to carry over -- the
    // quantity being corrected did not merely change size, it stopped being the same quantity.
    void Reprime();

    // `audioSeconds` is GetBufferTick scaled by the RUNTIME samplesPerFrame/samplesPerSecond;
    // `sceneSeconds` is the replay clock. Returns the ratio to command, trimmed.
    float Update(float commandedRatio, double audioSeconds, double sceneSeconds, double dtWall);
    float Trim() const { return trim_; }

private:
    float trim_ = 0.0f;
    double lastError_ = 0.0;
    bool primed_ = false;
    double errorBase_ = 0.0;
};

// ---- S12.4: two oracles, because one cannot tell the two failure modes apart -------------------
enum class RateVerdict : uint8_t {
    Pass = 0,
    FailMechanism,      // the DEVICE did not consume at the commanded rate: the ratio did not take
    NotRunStarved,      // the device tracked but Wwise did not: the machine is starved, not broken
    NotRunInvalid,      // GetAudioSettings failed, so Wwise is not up and there is nothing to judge
    NotRunCounterReset, // a counter went BACKWARDS between samples, so neither number is a rate
    Count
};
const char* RateVerdictName(RateVerdict);

// Both measurements are RELATIVE TO THE MEASURED 1.0x BASELINE, which cancels every device-specific
// term including ones nobody enumerated -- notably the 48000/512 that a round-3 spec hardcoded and
// which would have failed every rate on a 44.1 kHz endpoint.
// ★ A COUNTER THAT CAN RESET IS NOT A CLOCK, and this is the guard that says so. Both sources
// here go backwards under conditions that are not failures: XAUDIO2_VOICE_STATE::SamplesPlayed is
// PER VOICE and restarts at zero when the voice is destroyed and recreated, which is what a device
// change does (E-A1) and what any ReplaceOutput does; and GetBufferTick is a u32 that wraps roughly
// every 12.7 hours at 512/48000. An unsigned subtraction across either produces a vast delta, and a
// vast delta reads as "the ratio did not take" -- so the instrument built to distinguish a broken
// mechanism from a machine problem would report a DEVICE CHANGE as the mechanism being broken.
//
// `delta` is untouched and false is returned when `current` is behind `previous`. Equal is a valid
// forward step of zero: a stalled counter is a real reading and a genuine finding.
bool StepForward(double previous, double current, double* delta);

struct RateSample {
    double wwiseRatio = 0.0;    // (dTick/dWall at r) / (dTick/dWall at 1.0)
    double deviceRatio = 0.0;   // (dSamplesPlayed/dWall at r) / (same at 1.0)
    bool settingsValid = false;
    // Set when either source moved backwards between the two samples. The ratios above are then
    // meaningless rather than bad, and the verdict has to say so instead of judging them.
    bool sourceRegressed = false;
};
RateVerdict JudgeRate(const RateSample&, double commanded, double tolerance);

}  // namespace audio
}  // namespace replay
