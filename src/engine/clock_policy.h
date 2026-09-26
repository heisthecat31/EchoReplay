#pragma once

// W2-D. The replay clock's arithmetic, with no engine pointer, no Windows and no facts header in
// sight, so tests/clock drives all of it on a machine with no game.
//
// ---------------------------------------------------------------------------------------------
// ★★★ WHY HALF OF THIS FILE IS CLASSIFICATION AND NOT ARITHMETIC.
//
// Everything this module reports is derived from TWO SAMPLES OF A SOURCE THAT CAN RESET, REGRESS
// OR JUMP -- and this packet owns the clock, so every seek, reverse step and gap crossing is a
// DELIBERATE discontinuity in the very quantity the rest of the system differences.
//
//   ★ A discontinuity in the instrument looks exactly like a large reading from the thing being
//     measured. `now - prev` cannot tell them apart, because unsigned subtraction has already
//     collapsed "went backwards" onto "went forward by 1.8e19" before anyone can look.
//
// So every difference in this module is CLASSIFIED FIRST, in signed form, against a bound derived
// from what the engine can physically produce -- and a sample that fails is REJECTED AND COUNTED,
// never clamped. ★★ Saturating is not a fix: a clamped stamp from the future reads as zero until
// the source climbs back to it, and if it never does, forever. The counters are the fix, because
// they make the rejection visible instead of turning it into a plausible number.
//
// The same discipline, one level up: a meter fed only good samples will happily report a STALE
// value forever once every sample starts failing. RateMeter therefore reports UNKNOWN after a run
// of rejections rather than the last good answer, and the run length is on the status line.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>
#include <string.h>

#include "engine/clock.h"
#include "engine/session_watch.h"
#include "playback/core/transport.h"

namespace replay {
namespace clock {

// What Status().refusal reads once Release() has run. Released is the reason only when there was
// a take to release, or when no reason had been recorded yet; an enabled module that never got the
// clock keeps the reason it never did. Stamping Released over NoGlobalClock / SiteMismatch /
// BuildGuard / NotACClock / NoFloor erased the one diagnosis the NOT-RUN population carried, and
// made "released" and "never taken" read the same from the status line.
inline ClockRefusal RefusalAfterRelease(bool wasHeld, ClockRefusal before) {
    if (wasHeld || before == ClockRefusal::None) {
        return ClockRefusal::Released;
    }
    return before;
}

// ------------------------------------------------------------------------------------------------
// ★★★ WHEN THE ENGINE'S CLOCK IS OURS TO WRITE, AND WHY THAT IS A DECISION RATHER THAN A DEFAULT.
//
// `playback::Init` publishes `ReplayServices::transport` unconditionally, before any file is
// loaded, and a transport with no file reports `TransportState::Pause`. A tick that mirrors
// "whatever the transport says" therefore commands the engine's OWN world clock to stop during
// live play, on the first frame our provider ever receives -- which is what launch 5 did: it took
// two CClocks at 03:19:41.932, set CLOCK.PAUSE_BIT, and the game log stopped 234 seconds short of
// a level that never loaded. The renderer kept presenting, because nothing in the graphics path is
// ours; every sim-time state machine, level load included, did not.
//
// ★ THE PREDICATE IS `DurationUs() > 0`, AND IT IS THE ONE W2-H's RateDriver ALREADY ENGAGES ON
// (S9.10). A file that is loaded has a length; "no file" answers 0. Two modules deciding "is
// playback engaged" from two different facts is a pair that can disagree, and the disagreement
// would read as the world clock pausing while the audio sink keeps running at 1.0x.
enum class ClockGate : uint8_t {
    Drive,       // a replay is loaded: take the clock if we do not hold it, and write it
    Idle,        // no replay: take nothing, write nothing, and say so once
    Disengage,   // the replay went away while we held the clock: put the registers back
    Stopped,     // Release() has run. Terminal for the process
};

// `released` outranks everything: the latch is what stopped the next frame from re-taking a clock
// that had just been handed back. Disengage does NOT set it -- an unload is not a release, and a
// second file loaded in the same session must be able to drive the clock again.
inline ClockGate GateFor(bool haveTransport, uint64_t durationUs, bool held, bool released) {
    if (released) {
        return ClockGate::Stopped;
    }
    if (haveTransport && durationUs > 0) {
        return ClockGate::Drive;
    }
    return held ? ClockGate::Disengage : ClockGate::Idle;
}

// An ended session (engine/session_watch.h) reads as nothing to drive, so a held clock takes the Disengage arm and an
// unheld one stays Idle however long the file is.
inline ClockGate GateForSession(bool haveTransport, uint64_t durationUs, bool held, bool released,
                                bool sessionEnded) {
    return GateFor(haveTransport, sessionEnded ? 0 : durationUs, held, released);
}

struct RegisterLayout {
    uint32_t timescaleOff;
    uint32_t minElapsedOff;
    uint32_t flagsOff;
    uint64_t ourFlagBits;
};

struct SavedRegisters {
    uint32_t timescaleBits;
    uint64_t minElapsed;
};

using ClockAliveFn = bool (*)(const void* clock);

// Puts one clock's registers back. The pointer was verified when the clock was taken, which says
// nothing about now: `alive` is asked immediately before the first store, and a clock that fails
// it is left alone. Our two flag bits are cleared rather than the snapshotted word restored,
// because the engine read-modify-writes that qword every frame.
inline bool RestoreRegisters(void* clock, const RegisterLayout& at, const SavedRegisters& saved,
                             ClockAliveFn alive) {
    if (clock == nullptr || alive == nullptr || !alive(clock)) {
        return false;
    }
    uint8_t* p = static_cast<uint8_t*>(clock);
    memcpy(p + at.timescaleOff, &saved.timescaleBits, sizeof saved.timescaleBits);
    memcpy(p + at.minElapsedOff, &saved.minElapsed, sizeof saved.minElapsed);
    uint64_t flags = 0;
    memcpy(&flags, p + at.flagsOff, sizeof flags);
    flags &= ~at.ourFlagBits;
    memcpy(p + at.flagsOff, &flags, sizeof flags);
    return true;
}

// How a pair of samples of a monotonic engine counter relate. Asked BEFORE the difference is taken.
enum class SampleKind : uint8_t {
    First,        // no predecessor; not an error and not a measurement
    Normal,
    Stalled,      // equal. A REAL reading: this is what pause looks like
    Regressed,    // the counter went backwards -- the instrument reset. Not a small reading
    Implausible,  // forward by more than the engine can produce in the frames that elapsed
};

const char* SampleKindName(SampleKind);

// True for the two kinds that carry a usable delta.
inline bool Usable(SampleKind k) { return k == SampleKind::Normal || k == SampleKind::Stalled; }

struct SimBounds {
    // CClock+0x60, read from the live clock by the caller. Each heartbeat clamps the scaled delta
    // to it, so it is the per-beat ceiling and therefore the plausibility bound.
    //
    // ★ The default is 0, and 0 means NO BOUND HAS BEEN ESTABLISHED -- under which ClassifySim
    // calls every non-trivial sample Implausible. That is deliberate: a caller who forgets to
    // supply it gets a visibly dead meter and a rejection count, not a bound made up in this header
    // that would silently admit or exclude the wrong samples.
    uint64_t maxElapsedUs = 0;
    // Upper bound on heartbeats per rendered frame (W1-D S3.1: beat 1, plus 2 and 3 after us).
    uint32_t maxBeatsPerFrame = 3;
    // Past this many frames between two of our ticks the difference is not a frame delta whatever
    // the counter says, and computing a bound from it would only overflow into a vacuous pass.
    uint32_t maxFramesSpanned = 4096;
};

// CClock+0x28 sampled at two of our ticks, `framesElapsed` rendered frames apart. `deltaOut` is
// written only for Normal and Stalled; it is 0 otherwise, and 0 from this function NEVER means
// "the clock did not advance" on its own -- the kind says which of the four things happened.
SampleKind ClassifySim(uint64_t prev, uint64_t now, uint64_t framesElapsed, const SimBounds& b,
                       uint64_t& deltaOut);

// QueryPerformanceCounter sampled at two of our ticks. QPC is documented monotonic and 64-bit, so
// Regressed and Implausible are both "cannot happen" -- which is exactly why they are checked: a
// measurement whose failure mode is unconstructible is not a measurement, and this one is shared
// with the sim classifier so a defect in either shows up in both.
SampleKind ClassifyWall(uint64_t prevTicks, uint64_t nowTicks, uint64_t freqHz, uint64_t maxSpanUs,
                        uint64_t& deltaUsOut);

// ---------------------------------------------------------------------------------------------
// The achieved rate: scaled sim time the engine actually advanced, over real time, both measured
// across the SAME interval (our tick to our tick) so a skipped frame cancels out of the ratio.
//
// Two decaying accumulators divided at the end, rather than a decaying average OF THE RATIO: the
// ratio of two small noisy numbers is itself noisy in a way that no amount of smoothing removes,
// while the ratio of two smoothed sums is the windowed rate and is what we want to report.
// ---------------------------------------------------------------------------------------------
class RateMeter {
public:
    // A rejection run longer than this makes the meter report unknown rather than stale.
    static const uint32_t kStaleAfter = 30;

    // `decay` in (0,1): the weight kept from the previous frame. 0.9 is about a 10-frame window.
    void Reset(double decay = 0.9);

    // Only Normal and Stalled samples are fed; anything else must go to Reject().
    void Feed(uint64_t simDeltaUs, uint64_t wallDeltaUs);
    void Reject();

    // False when nothing usable has arrived, or when the rejection run is long enough that the
    // last good answer can no longer be called live. A stale number reported as current is the
    // failure this exists to prevent.
    bool Achieved(double& rateOut) const;
    // The smoothed real-time frame period, for the fast-forward ceiling. Same staleness rule.
    bool FramePeriodUs(double& usOut) const;

    uint64_t Samples() const { return samples_; }
    uint64_t Rejects() const { return rejects_; }
    uint32_t RejectRun() const { return rejectRun_; }

private:
    double decay_ = 0.9;
    double simAcc_ = 0.0;
    double wallAcc_ = 0.0;
    double countAcc_ = 0.0;   // the same decay applied to a count of 1, so wallAcc_/countAcc_ is
                              // the smoothed frame period rather than a sum that grows with rate
    uint64_t samples_ = 0;
    uint64_t rejects_ = 0;
    uint32_t rejectRun_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Rate, and the ceiling above which asking for more silently gets you nothing.
// ---------------------------------------------------------------------------------------------

// maxElapsedUs / framePeriodUs. 66666/16667 = 4.0x at 60 Hz, 66666/11111 = 6.0x at 90 Hz -- the
// numbers the design quotes, which is what checks this formula rather than a comment claiming it.
// Returns 0 when the frame period is not yet known: NOT 4.0, which is only true at 60 Hz.
double Ceiling(uint64_t maxElapsedUs, double framePeriodUs);

struct TimescalePlan {
    float value = 1.0f;      // what to write to CClock+0x38
    bool  cappedByCeiling = false;
    bool  cappedByFloor = false;
};

// ★★★ THE MAGNITUDE, NEVER THE SIGNED RATE. CClock+0x38 must never go negative: the heartbeat
// converts the scaled product with the unsigned cvttss2si idiom at 0x1400CF695, so a negative
// product reads as a huge u64, loses the `cmovnb` against the minimum and is then pinned to
// CLOCK.MAXELAPSED_US by the `cmovb` against the maximum. A reverse rate written signed would make
// the engine take the LARGEST forward step it can, every frame -- silently, and looking like a
// transport bug. Reverse moves the playhead backwards; the engine's own integrators only ever run
// forward, which the design already states as the honest cost of reverse.
//
// ★★ And never zero: a zero scale drives the scaled delta to zero, hence CLOCK.RECIP_OFF to +inf.
// `ceiling` of 0 means "not known yet" and applies no cap.
TimescalePlan PlanTimescale(double rate, double ceiling);

// ---------------------------------------------------------------------------------------------
// The playhead. Signed throughout, because reverse into t0 underflows an unsigned form to 1.8e19
// and the loop then reads it as FORWARD (B.5's own note).
// ---------------------------------------------------------------------------------------------
int64_t ClampPlayhead(int64_t tUs, uint64_t durationUs);

// What a change in the transport's playhead means, while the transport is paused. Frame-step is
// inferred from it rather than from a transient state enum, and the inference is only safe because
// the alternatives are named and excluded instead of falling through a default.
enum class MoveKind : uint8_t {
    None,          // the playhead did not move
    ForwardStep,   // a forward move small enough to hand the engine as one step-once delta
    Reverse,       // backwards: the engine cannot step backwards, so no step is armed
    Jump,          // further than the engine can step in one beat -- a seek, not a step
};

const char* MoveKindName(MoveKind);

MoveKind ClassifyPlayheadMove(int64_t prevUs, int64_t nowUs, uint64_t maxStepUs, uint64_t& stepOut);

// ---------------------------------------------------------------------------------------------
// ★ WHICH TRANSPORT STATES HOLD THE ENGINE. The engine world must not free-run whenever the
// playhead is being MOVED rather than played: its net-physics time base would march on real time
// while the node stamps teleport, and a body only re-latches on a beat we then never arm. A drag
// is the case this got wrong -- BeginScrub pauses the transport but keeps the pre-drag signed rate,
// so Scrub is the ONLY place the hold is visible (DIAG-SCRUB-BODIES).
// ---------------------------------------------------------------------------------------------
bool EngineHeld(TransportState state, float rate);

// The beat a playhead move earns while the engine is held. Forward gets its own size; reverse and
// jump get the floor -- the engine has no backwards dt and no step that large, but it still needs
// ONE beat to consume the history window the install just replaced whole. A floor-sized delta is
// "republish state, do not integrate": nothing can drift in 1 us.
uint64_t StepForMove(MoveKind mk, uint64_t moveStepUs, uint64_t floorUs);

// The step-once arm, decided. The every-other-frame guard is real -- there are two or three beats
// per frame, so a bit left set across frames steps twice -- but a step it refuses is CARRIED, not
// dropped: at 30-45 drag steps a second, dropping loses half of them.
struct StepArmPlan {
    uint64_t armUs;   // 0: nothing is armed this frame
    bool refused;     // a pending step did not arm (the stepsRefusedBusy population, unchanged)
    bool carry;       // ... and the refusal was transient, so the request must survive the frame
};

StepArmPlan PlanStepArm(bool stepEnabled, bool wantPaused, uint64_t pendingUs, bool armedLastFrame,
                        uint64_t maxUs);

// ---------------------------------------------------------------------------------------------
// The floor install for ONE clock: write it, read it back, and on a mismatch put the snapshot back
// before saying no. A clock whose minimum does not read as the floor must not receive the pause
// bit -- the engine's scaled delta would be 0 and CLOCK.RECIP_OFF 1.0f/0.0f = +inf -- so "did the
// write stick" is asked of every clock the module drives, the late arrivals included, and not only
// of the first one found. `Field` supplies Store(uint64_t) and Load(); the offline suite hands in
// one that refuses writes, which is the only way to watch this refuse.
// ---------------------------------------------------------------------------------------------
template <typename Field>
bool InstallFloor(Field& minElapsed, uint64_t floorUs, uint64_t snapshotUs, uint64_t& readBackOut) {
    minElapsed.Store(floorUs);
    readBackOut = minElapsed.Load();
    if (readBackOut == floorUs) {
        return true;
    }
    minElapsed.Store(snapshotUs);
    return false;
}

// ---------------------------------------------------------------------------------------------
// AN UNSTEPPED CLOCK IS A FACT WORTH SAYING ONCE, NOT 33 000 TIMES.
//
// `TopUpClocks` re-offers every clock it has not yet accepted on EVERY tick, for the life of the
// process, because a CClock the engine builds late must still be picked up. When one of those
// objects is not stepped by TICK.HEARTBEAT_BEFORE, AddClock refuses it -- correctly -- and said so
// once per tick: launch 5's `echoreplay.log` holds ~33 000 identical `third at 0x... is not
// stepped` lines, 90% of the file. A log that is 90% one line is a log nobody reads, so the
// volume destroys the very diagnosis the warning exists to deliver.
//
// THE FACT STAYS, THE VOLUME GOES. A report is LOGGED on every transition -- a name seen for the
// first time, a name now pointing at a different object, or a name accepted since it last refused
// -- and COUNTED otherwise. Both counts go on the status line, so "we refused this 33 000 times"
// is still answerable; it just is not retold 33 000 times.
//
// Keyed per NAME, not on "the last thing we refused". Two unstepped clocks alternating within one
// tick would defeat a single-slot latch and restore the whole volume, which is exactly the failure
// this replaces. The key is copied and compared with strcmp: the same literal appears at two call
// sites and nothing guarantees the linker pools them.
// ---------------------------------------------------------------------------------------------
class UnsteppedReporter {
public:
    // True when this refusal should be written to the log. Every call counts, logged or not.
    bool ShouldLog(const char* name, const void* obj) {
        ++reports_;
        Slot* s = SlotFor(name);
        if (s == nullptr) {
            return true;   // more names than slots: never silence a clock we cannot track
        }
        if (s->armed && s->obj == obj) {
            ++suppressed_;
            return false;
        }
        s->armed = true;
        s->obj = obj;
        return true;
    }

    // This name was accepted. Re-arm it, so a later refusal of the same object is a transition and
    // is said out loud rather than folded into a count from before it worked.
    void Accepted(const char* name) {
        if (Slot* s = SlotFor(name)) {
            s->armed = false;
            s->obj = nullptr;
        }
    }

    uint64_t Reports() const { return reports_; }
    uint64_t Suppressed() const { return suppressed_; }

private:
    // The engine builds three (global, third, game); the fourth slot is headroom, and running out
    // of slots degrades to logging everything rather than to silence.
    static const uint32_t kSlots = 4;
    static const size_t kKeyMax = 16;

    struct Slot {
        char        key[kKeyMax] = {};
        const void* obj = nullptr;
        bool        armed = false;
    };

    Slot* SlotFor(const char* name) {
        if (name == nullptr) {
            return nullptr;
        }
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (slots_[i].key[0] != 0 && strcmp(slots_[i].key, name) == 0) {
                return &slots_[i];
            }
        }
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (slots_[i].key[0] == 0) {
                size_t n = 0;
                while (name[n] != 0 && n + 1 < kKeyMax) {
                    slots_[i].key[n] = name[n];
                    ++n;
                }
                slots_[i].key[n] = 0;
                return &slots_[i];
            }
        }
        return nullptr;
    }

    Slot     slots_[kSlots];
    uint64_t reports_ = 0;
    uint64_t suppressed_ = 0;
};

}  // namespace clock
}  // namespace replay
