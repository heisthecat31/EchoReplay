#pragma once

// The B.9 implementation's wiring, kept out of engine/audiorate.h for the same reason
// audiostate_init.h is kept out of engine/audiostate.h: a contract header that drags in its
// implementation can stop compiling for reasons unrelated to the contract.

#include <stdint.h>

#include "audio/akbind.h"
#include "audio/ratepolicy.h"
#include "audio/sinkvoice.h"
#include "audio/voicelifetime.h"
#include "engine/audiorate.h"

namespace replay {
namespace audio {

struct AudioRateConfig {
    // ★ TWO BOOLEANS, NOT ONE, AND THE DIFFERENCE IS THE WHOLE OF S10.4.5's SECOND AND THIRD ROWS.
    // `requested` is what config key audio.max_rate_hook says; `installed` is whether the Level-1X
    // hook on IXAudio2::CreateSourceVoice actually went in. Collapsing them into one flag would make
    // "the operator turned it off" and "it failed to install" the same state, and the design
    // requires the UI to distinguish them -- a silently-degraded replay reads as a fidelity bug.
    //
    // `installed` is FALSE in this build. The hook is the one rung of S10.4 whose behaviour cannot
    // be established without launching the game, and an unverified COM vtable hook inside somebody's
    // running game is not a trade this project makes. MaxRate() is therefore 1.0, pause and the
    // entire slow-motion range are unaffected, and the label says "rate hook unavailable".
    bool rateHookRequested = true;
    bool rateHookInstalled = false;
    float installedMaxRate = 4.0f;   // what MaxRate() reports once the hook IS installed
    bool closedLoop = true;          // the S10.4.3 PI lock
};

// `ak` must already be bound. Returns false when the S10.4.2 proofs refused, which is a supported
// state: playback continues, audio plays at 1.0x, and Diagnostic() names the reason.
bool InitAudioRate(const AkExports& ak, const AudioRateConfig&);

// Once per frame, from the module tick. Samples the audio clock for the closed loop, feeds the A/V
// bias its warm-up observations, and re-applies the ratio if the device changed under us.
void TickAudioRate(double sceneSeconds, double dtWall);

// Abandons the closed loop's accumulated correction and re-takes its base. Called from the audio
// path's N-5a residue row on every discontinuity: the lock integrates a difference between two
// clocks, and a playhead jump changes that difference by the size of the jump -- which the loop
// would then "correct" by detuning the mix. There is no correct trim to carry across a jump,
// because the quantity being corrected stopped being the same quantity.
void MarkAudioClockDiscontinuity();

// The O4 instrument. Fills a RateSample from the two independent clock domains -- Wwise's frame
// counter and XAudio2's own samplesPlayed -- so a starved machine is distinguishable from a
// mechanism that did not take. Returns false when Wwise is not up, which is O4's NOT-RUN.
bool SampleRateOracle(double dtWall, RateSample* out);

// How often the rate module verifies the sinks against its own ledger: it probes the chain and
// compares each voice's live ratio with `baseline * lastCommanded`. Published because it is a
// probe cadence, and a test that bounded probe counts without it would be counting two mechanisms
// and attributing both to one.
constexpr uint64_t kRatioDriftCheckMs = 2000;

// How long after the module starts asking for a NEW ratio a reading of the voices says nothing
// about them. The device applies on its own processing pass -- ~11 ms at this title's 48000/512 --
// and this title's read-back is observably one pass behind, so a check that lands inside the window
// scores the previous ask as the current voice. Twenty passes of slack, and still an order of
// magnitude inside kRatioDriftCheckMs, so a voice that really is stuck is judged on the next check
// rather than a check later. A retry of a value already outstanding does not restart it.
constexpr uint64_t kRatioReadLagMs = 250;

// One line the rate module wants said, drained by whoever holds an ILog. The module runs under the
// playback tick with no log of its own, and the two things it now has to report -- a voice found
// off the ratio we put it at, and the closed loop giving up at its rail -- are exactly the lines
// whose absence cost three launches. `superseded` counts notices a slow drain overwrote.
bool DrainAudioRateNotice(char* out, size_t cap, uint32_t* superseded = nullptr);

// Restores every source voice this module commanded to the ratio the engine had, and stops
// answering. Called from the audio module's shutdown: the rate lives in XAudio2 and would outlive
// us otherwise, leaving the game pitched at whatever the replay stopped on.
void ShutdownAudioRate();

// ---- S10.4.5's sink-availability line, and it has THREE states ------------------------------
//
// ARCH-22 S3.4. Row 2.6 is what makes "the voice is still there" answerable; with the detour
// refused there is no teardown announcement on the unannounced paths (Suspend, the stall watchdog),
// so an unavailable sink and a sink nobody could ask about are different findings and a two-state
// gate cannot express the difference. `could-not-ask` is a recorded capability reduction -- never a
// `sink gone`, which reads as a clean skip, and never a confident rate.
enum class SinkAvailability : uint8_t { Available, Unavailable, CouldNotAsk };
SinkAvailability SinkAvailabilityState();
const char* SinkAvailabilityLine();

// Told by the audio module once ArmVoiceLifetime has a verdict. Before it is told, the state is
// could-not-ask, because that is what it is.
void SetVoiceLifetimeArm(LifetimeArm arm);

void ResetAudioRateForTest();

// Replaces the two sink operations -- the probe and the ratio command -- for tests/audio. Either
// argument null restores the shipped one. See the comment on g_probeSinks in audiorate.cpp for why
// the seam is needed at all: the availability state machine is sequencing that no offline check
// could otherwise reach, and it is the part of S10.4 that a launch has now twice failed to measure.
void SetSinkAccessForTest(size_t (*probe)(const AkExports&, SinkChain*),
                          size_t (*apply)(const AkExports&, float, int32_t*, size_t*),
                          size_t (*restore)(const AkExports&, int32_t*, bool) = nullptr);

}  // namespace audio
}  // namespace replay
