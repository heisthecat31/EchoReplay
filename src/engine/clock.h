#pragma once

// B.5 IReplayClock -- the replay clock. Owned by W2-D; it is what makes the transport's decisions
// change how fast the world runs.
//
// ---------------------------------------------------------------------------------------------
// ★★★ B.5's SIGNATURES ARE UNCHANGED. ITS MECHANISM NOTE IS RETIRED, AND SAYING SO IS THE POINT.
//
// B.5 declares `SetStepUs(uint64_t) // clk[0x90]; 1 us for pause, never 0`. The register named in
// that comment is CLOCK.EXTSTEP_OFF, and the fact registry now records
// CLOCK.EXTSTEP_PERFRAME_WRITE_VA: TICK.UPDATE_CLOCK REWRITES +0x90 every frame on the fixed-step
// path (0x140127A04) and CLEARS CLOCK.EXTSTEP_ENABLE_BIT every frame on the other one, so an
// externally written step does not survive a single frame either way.
//
// ★ The METHOD survives that intact, because its meaning was never the register: "advance the sim
// by exactly this many microseconds this frame". The mechanism that delivers it is CLOCK.PAUSE_BIT
// plus the self-clearing CLOCK.STEPONCE_BIT with CLOCK.STEPONCE_OFF (+0x98) as the delta -- and
// +0x98 is NOT rewritten per frame. "1 us for pause, never 0" is still exactly right, and so is
// every other signature. Nothing written against B.5 needs editing to compile against this header.
//
// ★★ THE APPENDED METHODS, and W1-D's precedent for appending. Rate and pause are not expressible
// through B.5 as frozen: the mechanism is a float multiplier and a flag bit, not a step. W1-D
// appended `frameDeltaUs` to B.3 on the same grounds -- no consumer of the contract existed yet,
// and the alternative was shipping a field whose documented meaning was false. ReplayServices::clock
// has no consumer today either. The appended methods are at the END of the class so the vtable
// layout of the frozen prefix is untouched.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

namespace replay {

struct ReplayServices;

// Why the clock is not driving the engine, when it is not. "It fell back" is not a diagnosis, so
// every refusal has its own value -- the same discipline W1-D used for ClaimResult.
enum class ClockRefusal : uint8_t {
    None = 0,
    Disabled,          // transport.clock_writes is false
    NoModule,          // GetModuleHandleW(nullptr) failed
    BuildGuard,        // the build guard did not pass, so no offset in facts/ means anything here
    SiteMismatch,      // TICK.HEARTBEAT_BEFORE or TICK.UPDATE_CLOCK is not the bytes we measured
    NoGlobalClock,     // CLOCK.GLOBAL_PTR is still null -- the engine has not constructed it
    NotACClock,        // the object at CLOCK.GLOBAL_PTR is not stepped by TICK.HEARTBEAT_BEFORE
    NoFloor,           // the 1 us floor could not be written, so pause would produce +inf
    // ★ Release() has run on a clock this module HELD. TERMINAL for the process, and reported rather
    // than silent: the take gate in the tick is keyed on `held` alone, so a release that did not
    // latch is undone by the very next tick -- which is exactly what launch 10 measured
    // (CLOCK.FLAGS_OFF bit 1 back on and our floor still installed one frame after Release). An
    // inert clock that says why is the whole reason this enum exists.
    //
    // A module that never held the clock KEEPS the reason it never did; Release() on it sets
    // ClockStatus::released and leaves this alone, so "released" and "never took it" cannot read
    // the same.
    Released,
    // ★ No replay is loaded, so the engine's clock is the engine's. The transport is published
    // before any file is opened and a fileless transport reports Pause, so mirroring it during live
    // play stops the world -- see ClockGate in clock_policy.h. This is the ordinary state of a
    // session that has not opened a replay yet, not a fault, and it clears when a file loads.
    NotEngaged,
};

const char* ClockRefusalName(ClockRefusal);

// Everything the module knows about what it did, for the panel, the log and the tests. Read-only,
// and cheap: it is a copy of counters the tick already maintains.
struct ClockStatus {
    ClockRefusal refusal;
    bool     engaged;            // a replay with a duration is loaded, so the clock is ours to write
    bool     held;               // we have taken the clock and are writing it
    bool     everHeld;           // a take succeeded at some point in this process
    bool     released;           // Release() has run: the latch, whether or not a take preceded it
    bool     paused;             // CLOCK.PAUSE_BIT is set by us
    float    commandedRate;      // what the transport asked for, signed
    float    writtenTimescale;   // what is actually in CLOCK.TIMESCALE_OFF, always positive
    bool     cappedByCeiling;    // the request was above what the engine can deliver

    // The closed loop. `rateKnown` false means UNKNOWN, never "zero" and never a stale value.
    bool     rateKnown;
    double   achievedRate;       // scaled sim advance / real time, measured over the same interval
    bool     framePeriodKnown;
    double   framePeriodUs;
    double   ceiling;            // maxElapsedUs / framePeriodUs; 0 when the period is unknown

    uint64_t maxElapsedUs;       // as read from the live clock
    bool     maxElapsedLive;     // false means the CLOCK.MAXELAPSED_US constant was substituted
    uint64_t minElapsedUsAtTake; // ★ CLOCK.MINELAPSED_OFF as we found it. Expected 0
    uint64_t floorUs;            // what we wrote over it

    // How many CClocks we found, verified and are writing. Three exist; each is checked separately.
    uint32_t clocksFound;
    uint32_t clocksWritten;

    // The discontinuity ledger. Every one of these is a difference this module refused to trust,
    // and a non-zero value is a finding rather than noise.
    uint64_t simRegressions;     // CClock+0x28 went backwards: the engine reset a clock
    uint64_t simImplausible;     // forward by more than the engine can produce in one frame
    uint64_t wallRejected;       // QPC regressed, stalled or spanned an absurd interval
    uint64_t skippedFrameSamples;// our tick did not run on the previous frame; not a frame delta
    uint64_t rateSamples;
    uint32_t rateRejectRun;      // consecutive rejections; past 30 the rate reads UNKNOWN

    uint64_t stepsArmed;         // CLOCK.STEPONCE_BIT set
    uint64_t stepsRefusedBusy;   // an arm arrived before the previous one had been consumed
    uint64_t movesReverse;       // a backwards playhead move: the engine cannot step backwards
    uint64_t movesJump;          // a seek, not a step

    uint64_t burstsEntered;
    uint64_t burstsLeaked;       // a burst still open at the next tick; closed for the caller
    uint64_t idleTicks;          // ticks that returned because no replay was loaded
    uint64_t disengages;         // replays unloaded under us: the clocks were handed back each time
    uint64_t unsteppedReports;   // objects offered to AddClock that TICK.HEARTBEAT_BEFORE does not step
    uint64_t unsteppedSuppressed;// of those, the repeats that were counted here instead of logged
    uint64_t watchdogPets;
    bool     watchdogBound;      // the deadlock monitor was found and its Tick prologue verified
};

class IReplayClock {
public:
    // ---- B.5, frozen. Signatures unchanged; see the header comment for SetStepUs's mechanism. ----

    // Advance the sim by exactly `stepUs` microseconds on the next heartbeat. NEVER 0.
    virtual void     SetStepUs(uint64_t stepUs) = 0;
    // SIGNED. Reverse into t0 underflowed the unsigned form to 1.8e19 and the loop then read it as
    // FORWARD. Clamped to [0, duration].
    virtual void     SetPlayhead(int64_t tUs) = 0;
    virtual int64_t  Playhead() const = 0;
    virtual void     EnterBurst() = 0;
    virtual void     PetWatchdog() = 0;
    virtual void     ExitBurst() = 0;
    virtual uint64_t LastEngineDeltaUs() const = 0;

    // ---- Appended by W2-D. See the header comment for why, and for W1-D's precedent. ----

    // Signed: negative is reverse. The MAGNITUDE reaches CLOCK.TIMESCALE_OFF; the sign is the
    // transport's business, because the engine's integrators only run forward (S7.5.5).
    virtual void     SetRate(float rate) = 0;
    virtual void     SetPaused(bool paused) = 0;
    // The file's length, for SetPlayhead's clamp. 0 means unknown and disables the upper clamp.
    virtual void     SetDurationUs(uint64_t durationUs) = 0;
    // Put every field back the way we found it and stop writing. Idempotent; also run at shutdown.
    // TERMINAL: the module does not re-take for the rest of the process. Status().released reads
    // true, and Status().refusal reads ClockRefusal::Released if there was a take to release --
    // otherwise it keeps naming why there was not. An inert clock is never a silent one.
    virtual void     Release() = 0;
    virtual ClockStatus Status() const = 0;

    // ★ PRE-ROLL. Run the engine's clock forward at `rate` while the TRANSPORT stays paused, so the
    // offline host's 45 s pre-match script elapses before the first second the viewer watches. For
    // as long as it is active the transport mirror stops imposing the transport's pause and rate;
    // `false` hands both back, and the very next mirror re-imposes them. Defaulted, not pure: the
    // doubles that implement this interface have no engine clock behind them.
    virtual void     SetPreRoll(bool active, float rate) { (void)active; (void)rate; }

protected:
    // Static storage duration, never deleted. Mirrors what W1-C did for ILog and W1-D for ITick.
    ~IReplayClock() = default;
};

namespace clock {

// The process-wide instance, registered into ReplayServices by this module. Never null once the
// module has initialised; Status().refusal says whether it is actually driving anything.
IReplayClock* Get();

}  // namespace clock
}  // namespace replay
