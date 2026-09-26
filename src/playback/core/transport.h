#pragma once

// W2-B. Appendix B.8's IReplayTransport, and the S7.3.2 accumulator behind it.
//
// ★ B.8 is FROZEN and four packets compile against it -- W2-E, W2-F, W2-G, W2-H. It is declared here,
// in `namespace replay`, per B.12's rule over the whole appendix (the round-9 correction that moved all
// twelve code blocks; W1-E's headers followed it, and so does this one). A global IReplayTransport in a
// DLL sharing a process with the engine and with rlay.dll collides at link time in somebody else's
// translation unit.
//
// ---------------------------------------------------------------------------------------------
// ★ THE ACCUMULATOR IS ADOPTED UNCHANGED IN ITS ARITHMETIC from the design -- including
// `acc -= sgn * d` (the unsigned form makes reverse ACCELERATE, measured 5.6x runaway), the two
// playheads, CATCHUP_LIMIT, and the refusal to clamp wall_dt. That loop survived an independent 30-cell
// rebuild by the transport auditor and I have no grounds to touch it.
//
// ★ THE FIDELITY PROPERTY IS HELD BY REPRESENTATION, NOT BY ARITHMETIC. The solver playhead is stored
// as an INDEX into the recorded boundary table plus a sub-tick remainder, so "every engine step is a
// whole recorded tick of exactly its recorded dt" is a property of the type rather than a result the
// loop has to keep achieving. S7.3.2's own mutation 3 -- step by `acc` directly -- is not expressible
// against this representation, which is the difference between a defect you test for and one you
// cannot write.
//
// ★ ONE REFINEMENT TO S7.5.2, STATED BECAUSE IT DEVIATES FROM A MEASURED DECISION. S7.5.2 specifies
// micro-seek as "set solver_playhead to the target and acc = 0", against a measured 4.17% rate error
// when acc is left INTACT. Zeroing acc is right about the failure it was measured against -- the
// surplus must not be consumed twice -- but discarding the SUB-TICK remainder every micro-seek is a
// per-frame loss, and above the ceiling K micro-seek runs on ~95% of frames, so that loss becomes a
// systematic rate error in the other direction. This implementation lands the solver on the recorded
// boundary at or below the target and carries the remainder, which is strictly less than one tick and
// therefore can never be re-consumed as a whole tick. Both failure modes are cells in the gate:
// tests/playback asserts no runaway AND no systematic loss, and the intact-acc mutant goes red.
// ---------------------------------------------------------------------------------------------

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "playback/core/discontinuity.h"

namespace replay {

// B.8, plus one value B.8 could not express. ★★★ ABSENCE IS NOT A VALUE (S9.2): with only the five
// verbatim values, a transport holding no file had to answer `Pause` -- and `Pause` is a COMMAND.
// W2-D's clock mirrored that command onto the engine's own world clock on the first game tick of
// launch 1 (runs/20260902-071934Z/launch-01), stopped every sim-time state machine in the process,
// and the level never loaded. The five verbatim values keep their order and their meaning; NotLoaded
// is appended, so nothing written against B.8 changes its layout.
//
// ★ A consumer that only asks "is it paused" still gets a correct answer -- NotLoaded is not Pause,
// so nothing pauses off it. A consumer that asks "is it running" must ask whether a replay is
// loaded first: `DurationUs() > 0` is that fact, and it is the one W2-D's clock and W2-H's
// RateDriver both engage on.
enum class TransportState { Pause, Play, Scrub, Seek, Step, NotLoaded };

// ★★★ WHICH END OF THE RECORDING THE PLAYHEAD IS SITTING ON, and why it is not `held_for_coverage`.
// The resident table running out is a WAIT: the next instants exist and nobody has decoded them yet.
// The FILE running out is a STATE: there is nothing further to decode, ever, and a replay that keeps
// reporting "the decoder is behind" at the end of the recording reports a finished playback as a
// stalled one, one hold per frame, forever. The two are answered by different counters
// (EndClamps/CoverageHolds) and by different words in the log because a viewer has to act on them
// differently: one is waited out, the other is the end.
//
// ★★ A SEPARATE ANSWER RATHER THAN A SIXTH TransportState, because every existing reader of that
// enum switches over it exhaustively and a new word would fall through all of them into "paused"
// or worse into the unnamed-state arm. An end of the recording IS a pause; it is a pause with a
// reason, and the reason is published beside it.
enum class EndState : uint8_t { None = 0, AtStart, AtEnd };

// ★★★ B.8 AMENDMENT: `SeekRound(int roundIndex)` was ambiguous, and W2-F was right to refuse it.
//
// `roundIndex` could mean a POSITION in Manifest::rounds or the round NUMBER in RoundMarker::index.
// Those diverge the moment a capture starts mid-match or carries a non-round marker kind -- and a
// wrong answer here does not fault. It lands on a different round, looks plausible, and gets blamed
// on seek.
//
// ★★ RULING: it is the round NUMBER. That is what a user means by "go to round 3", and it is the
// reading that survives a capture starting mid-match -- where rounds[0].index == 2 and a position of
// 0 would silently answer a different question.
//
// And the ambiguity is made UNREPRESENTABLE rather than documented: a distinct type, the same
// discipline UserIndex/UserSlot and PhysHandle/PlayerHandle already use in this codebase, so passing
// an array position where a round number belongs is a compile error instead of a wrong jump.
struct RoundNumber {
    uint16_t v;
    explicit RoundNumber(uint16_t x) : v(x) {}
};

// What LastStepTicks() answers when the transport does not count its steps at all. It is NOT 0:
// 0 is a COUNTED refusal (a step that ran into an end of the tick table), and a caller that read
// an uncounting double as a refusal would refuse every step made through it. No tick count can
// reach INT_MIN, so the two answers stay distinguishable.
inline constexpr int kStepTicksUncounted = INT_MIN;

// ★★★ WHY THE PICTURE IS HELD WHEN NOTHING IS WRONG. A load whose disc is not bound yet runs the
// offline host's 45 s pre-match script forward on the engine clock with the playhead pinned, so the
// disc is driven from the first second the owner watches. State() reads `Pause` throughout, which
// is what the transport is -- this says WHY, and it is the one thing that tells a viewer waiting on
// purpose from a viewer that hung. `progress` is 0..1 over the wall budget; `hostRemainingS` is an
// ESTIMATE of the host's own countdown (the script's copy is not readable), not a measurement.
struct PrepareStatus {
    bool   preparing     = false;
    bool   playRequested = false;   // Play was pressed during the wait and is remembered
    double elapsedS      = 0.0;
    double boundS        = 0.0;
    double progress      = 0.0;
    double hostRemainingS = 0.0;
};

// DESIGN-UI-2 S5.3. What a drag looks like from outside the transport. All fields are meaningful
// only while `active`.
struct ScrubStats {
    bool     active = false;        // State() == TransportState::Scrub
    int64_t  pendingUs = 0;         // newest target handed in
    int64_t  appliedUs = 0;         // newest target actually applied to the world
    uint32_t skipped = 0;           // targets coalesced away since BeginScrub
    bool     lastRefused = false;   // the last step's window build refused; the playhead did not
                                    // move, so a drawn playhead must snap back to PlayheadUs()
};

class IReplayTransport {
public:
    virtual bool Load(const wchar_t* path) = 0;
    virtual void Unload() = 0;
    virtual void Play(float rate) = 0;      // negative = reverse
    virtual void Pause() = 0;
    virtual void Step(int frames) = 0;
    // The SIGNED recorded ticks the last Step actually consumed -- negative backwards, 0 when it
    // ran into either end of the table and moved nothing. Defaulted for the reason
    // camera_control::CommandPose is (W2-R's and W2-F's doubles implement this interface with no
    // tick table behind them), and defaulted to `uncounted` rather than to a count, because a
    // double that never counted has not refused anything.
    //
    // L3-RUN-2 F4 answered `ok` on a step that moved nothing; reading the playhead either side of
    // Step() catches that, but has a blind spot this count does not: a recorded tick table is
    // NON-decreasing, so a stalled source records the same boundary twice and a step that
    // genuinely consumed a tick can leave the playhead on the microsecond it started from.
    virtual int LastStepTicks() const { return kStepTicksUncounted; }
    virtual void Seek(int64_t tUs) = 0;     // F-4: signed, as B.5 already is
    // Returns FALSE when the file carries no round with that number -- refused, never clamped to a
    // neighbour. Jumping to a plausible wrong round is precisely the failure this signature exists
    // to prevent, and a silent clamp would reintroduce it behind a correct-looking type.
    virtual bool SeekRound(RoundNumber) = 0;
    virtual void SetMode(bool asSeen) = 0;  // false = ground truth (default)
    virtual TransportState State() const = 0;
    // Which end of the recording the playhead is parked on, if either. State() reports Pause at
    // both ends -- that is what they are -- and this says why, so a viewer sitting at the end of a
    // file can be told the replay finished rather than left looking at a frozen picture. Defaulted
    // for the doubles that have no file behind them.
    virtual EndState Ended() const { return EndState::None; }
    virtual float Rate() const = 0;
    virtual int64_t PlayheadUs() const = 0; // F-4: reverse into t0 underflows a u64
    virtual uint64_t DurationUs() const = 0;
    // Changes on every successful Load. Load unloads and reloads inside one call, so a module that
    // only watches DurationUs cannot tell a second file from the first still being open. Defaulted
    // for the doubles that load nothing.
    virtual uint32_t LoadSerial() const { return 0; }

    // ★★★ THE CAPTURE'S OWN SAMPLE SPACING -- how far apart THIS FILE's recorded ticks are, in
    // microseconds. It is NOT the render frame period: that is how fast this machine draws, and the
    // two are independent (a 30 Hz capture plays on a 90 Hz machine). A consumer that judges a seek
    // landing against the render period calls a correct nearest-tick landing a miss on every machine
    // that draws faster than the file was recorded -- which is what src/control/dispatch.cpp did
    // until this member existed, because the interface published no cadence to ask instead.
    //
    // Defaulted false for the reason LastStepTicks is defaulted (W2-R's and W2-F's doubles have no
    // file behind them), and false whenever nothing is loaded or the loaded capture carries no
    // measurable cadence. `*outUs` is UNTOUCHED on false: an out-parameter written on a refusal is
    // how a caller that ignores the bool comes to hold a number nobody measured.
    virtual bool TickSpacingUs(int64_t* outUs) const { (void)outUs; return false; }

    // ★★★ WHICH RECORDED TICK THE PLAYHEAD IS SITTING IN. `*outTickUs` is the boundary of the
    // loaded window's tick table AT OR BEFORE `playheadUs`, on the same playback-relative axis
    // PlayheadUs() answers on (INV-AXIS) -- so a caller compares the two directly and never holds a
    // conversion of its own.
    //
    // It reads the SAME table StepTicks lands on, so the two cannot disagree about where a boundary
    // is. That is why this is an accessor here rather than a tick table reassembled by whoever needs
    // to ask: tests/replay's `w2r-step-moves-by-ticks` could not ask its boundary clause at all while
    // the interface published no accessor, and scored `boundary-not-published` in its place.
    //
    // False with `*outTickUs` UNTOUCHED when nothing is loaded, when the table holds no boundary, or
    // when the playhead is BEFORE the first recorded one -- a reverse step past the front of the table
    // is a real state (PlayheadUs is deliberately unclamped there) and no boundary is at or before it.
    // Clamping to the first would answer a question nobody asked. Defaulted false for the reason
    // TickSpacingUs is (W2-R's and W2-F's doubles have no file behind them).
    virtual bool NearestTickUs(int64_t playheadUs, int64_t* outTickUs) const {
        (void)playheadUs;
        (void)outTickUs;
        return false;
    }

    // ★★★ WHERE THE RECORDED CONTENT ACTUALLY IS, on the same playhead axis PlayheadUs() answers
    // on (INV-AXIS). It is NOT [0, DurationUs()): the axis is anchored at the manifest's t0, and a
    // capture whose first core chapter begins eight hundred seconds after t0 has eight hundred
    // seconds of NOTHING in front of it. A timeline drawn over the duration therefore draws a file
    // that is 95% dead air, which is exactly what the owner saw -- "super zoomed out... shows way
    // more than is actually recorded and I cant actually seek to that part".
    //
    // `*firstUs`/`*lastUs` are UNTOUCHED on false, for the reason TickSpacingUs leaves its
    // out-parameter alone: a caller that ignores the bool must not come to hold a span nobody
    // measured. False when nothing is loaded or the capture carries no core chapter with content.
    virtual bool PlayableSpanUs(int64_t* firstUs, int64_t* lastUs) const {
        (void)firstUs;
        (void)lastUs;
        return false;
    }

    // ★ DERIVED FROM THE ONE ACCESSOR ABOVE, AND NOT VIRTUAL. Two implementations of "is this a
    // boundary" is how a timeline comes to disagree with the thing it scrubs; a transport that
    // publishes its boundaries has already answered this, and one that does not answers false here
    // because it answered false there. Nothing overrides it, so nothing can drift from it -- and
    // being non-virtual it adds no vtable slot to frozen B.8.
    // Defaulted for the reason TickSpacingUs is: a double with no load behind it is never
    // preparing, and a caller that does not know about the pre-roll reads exactly what it read
    // before.
    virtual PrepareStatus Preparing() const { return PrepareStatus(); }

    bool IsOnTickBoundary(int64_t playheadUs) const {
        int64_t tick = 0;
        return NearestTickUs(playheadUs, &tick) && tick == playheadUs;
    }

    // ★★★ INV-AXIS. EVERY TIME CROSSING THIS INTERFACE IS MICROSECONDS SINCE THE START OF
    // PLAYBACK. Absolute source time exists only INSIDE the transport. The full statement, and why
    // this axis and not the other one, is in playback/core/playback.h.
    //
    // Seek(0) is the first frame, Seek(DurationUs()) the last, PlayheadUs() is always in
    // [0, DurationUs()], and fraction * DurationUs() is directly seekable. There is no origin
    // accessor here on purpose: publishing one would put a second way to compute a position beside
    // the only correct one, and four call sites each holding their own conversion is how the axes
    // came to disagree in the first place.
    virtual const char* ActiveModeLabel() const = 0;   // never silent degradation

    // ★ RATE WITHOUT RESUMING. Play() is "start running, at this rate"; this is "run at this rate
    // WHEN you run". Without it every speed and direction control had to call Play(), and a rate
    // change while paused therefore unpaused -- the owner's complaint. Defaulted so the existing
    // test doubles keep compiling and no vtable slot above it moves.
    virtual void SetRate(float rate) { (void)rate; }

    // ---- DESIGN-UI-2 S5.3, the scrub API. Microseconds on INV-AXIS, like every other time here.
    //
    // The whole of "smooth dragging" is that the caller owns the target and this owns at most one
    // of them: ScrubTo overwrites a single pending slot, and the tick applies whatever is in it
    // once per frame. Older targets are DISCARDED, never queued -- a queue replays every
    // intermediate position, which is the lag itself.

    // Enter Scrub. Records the pre-drag play/pause state and signed rate, pauses, mutes audio.
    // Idempotent: a second BeginScrub takes no second audio hold and does not re-record the
    // resume state. Never changes rate or direction.
    virtual void BeginScrub() {}

    // Overwrite the single pending target, clamped to [0, DurationUs()]. Cheap; safe to call every
    // frame. No-op unless a scrub is active. Applied at most once per tick.
    virtual void ScrubTo(int64_t us) { (void)us; }

    // Full seek to `us` (or to the last target if us < 0), run the deferred bookkeeping, re-centre
    // the window, unmute, then restore the pre-drag play/pause at the pre-drag rate and direction.
    // Idempotent; safe when no scrub is active.
    virtual void EndScrub(int64_t us) { (void)us; }

    virtual ScrubStats Scrub() const { return ScrubStats(); }

    // Motion smoothing: mode 0 off, 1 slow-mo only, 2 always; strength 1..3. Defaulted and appended
    // for the same reason SetRate is: the test doubles keep compiling and no vtable slot moves.
    // MotionSmoothing() packs strength << 8 | mode; 0 means this transport has no smoothing.
    virtual void SetMotionSmoothing(uint8_t mode, uint8_t strength) { (void)mode; (void)strength; }
    virtual uint16_t MotionSmoothing() const { return 0; }

    // The user slot the entrant replaying recorded slot `recordedSlot` holds in THIS session. The
    // two differ whenever the viewer holds a slot of its own. False, with `*outLiveSlot` untouched,
    // until that player has been seated. Defaulted and appended for the reason SetRate is.
    virtual bool LiveUserSlot(uint16_t recordedSlot, uint16_t* outLiveSlot) const {
        (void)recordedSlot;
        (void)outLiveSlot;
        return false;
    }

protected:
    ~IReplayTransport() = default;
};

namespace playback {

// The recorded tick boundaries, in source microseconds, non-decreasing. Tick i spans
// [At(i), At(i+1)) and its recorded dt is At(i+1) - At(i). Borrowed; the window owns the storage.
class TickTable {
public:
    void Set(const uint64_t* boundaries, size_t n) { t_ = boundaries; n_ = n; }
    size_t Count() const { return n_; }
    uint64_t At(size_t i) const { return t_[i]; }
    bool Empty() const { return n_ < 2; }
    uint64_t FirstUs() const { return n_ ? t_[0] : 0; }
    uint64_t LastUs() const { return n_ ? t_[n_ - 1] : 0; }
    // Largest i with At(i) <= t, or 0. Used only on a discontinuity; steady state moves by +-1.
    size_t IndexAt(int64_t t) const;

private:
    const uint64_t* t_ = nullptr;
    size_t n_ = 0;
};

// ★★★ HOW THE STEP BETWEEN TWO RECORDED BOUNDARIES IS CLASSIFIED, AND WHY IT IS AN ENUM.
//
// `At(i+1) - At(i)` is a difference of two samples of a source that can jump, stall or go backwards,
// and unsigned subtraction turns two of those three into the SAME enormous number. A regressed table
// and a five-second capture gap both arrive as ~1.8e19 -- so the transport would report a file defect
// as a gap, treat it as one, and nothing would ever say the table was out of order.
//
// ★★ A discontinuity in the instrument looks exactly like a large reading from the thing being
// measured. The only way to tell them apart is to ask BEFORE doing the arithmetic, in signed form, and
// to keep the DIAGNOSIS separate from the REMEDY: a regression and a gap are both crossed the same
// way, and they are not the same finding.
enum class StepKind : uint8_t {
    Normal = 0,
    ZeroDt,      // ★ EQUAL IS A VALID READING. Duplicate stamps are legal in the file
    Gap,         // longer than the engine can step; crossed, never commanded
    Regressed,   // the table went backwards: a file defect, not a transport event
};

const char* StepKindName(StepKind);

// Signed on purpose. `now` and `prev` are boundary samples; the sign of their difference is the whole
// question, and it is the thing unsigned arithmetic destroys before anyone can look at it.
StepKind ClassifyStep(uint64_t prev, uint64_t now, uint64_t max_step_us, uint64_t& dt_out);

struct TransportTuning {
    // ★★★ THE LARGEST STEP THAT MEANS ANYTHING, AND WHY A "WHOLE RECORDED TICK" IS NOT ENOUGH.
    //
    // The fidelity property -- every engine step is a whole recorded tick -- is a property of the
    // REPRESENTATION, and this is where that stops being the same as safe. A recorded tick can be five
    // seconds long: a capture gap, a DROPPED run, a stalled writer. Commanding a 5,000,000 us step
    // does not advance the solver by five seconds; CClock clamps the scaled delta to
    // CLOCK.MAXELAPSED_US (66,666), so physics takes ONE 66 ms step and the other 4.93 seconds are
    // discarded inside the engine, silently, with the playhead insisting it moved.
    //
    // So a tick longer than the clamp is not a step at all. It is a GAP, and the transport crosses it
    // the way E-P3b says: micro-seek to the far edge, raise Cause::GapCross so the residue battery
    // fires, and never hand the number to the clock.
    uint64_t max_step_us = 66666;   // CLOCK.MAXELAPSED_US; a config key, never invented here

    // ★★★ THE LONGEST REAL INTERVAL ONE FRAME MAY SPEND, AND WHY THE SURPLUS IS DROPPED.
    //
    // The accumulator used to take wall_dt whole, on the reasoning that discarding it "silently
    // discarded scene time". For a recorder that is right; for a VIEWER it is not. A hitch -- a
    // window rebuild, a shader compile, the debugger -- is time the watcher spent looking at a
    // frozen picture, and paying it back afterwards makes the replay sprint through the seconds he
    // was waiting to see. What a viewer wants after a hitch is to carry on from where he was.
    //
    // So a frame advances by at most `rate * max_wall_dt_us`, and the excess is DROPPED and COUNTED
    // (WallClamps/WallDroppedUs) rather than banked.
    //
    // ★★ THE QUANTITY IS THE RENDER INTERVAL, NOT THE CAPTURE'S TICK. 100 ms made every frame of a
    // machine below 10 fps a clamped frame: 120 ms frames advanced 100 ms each and the replay ran at
    // 0.833x against wall clock for as long as the machine stayed slow, reported in exactly the form
    // a one-off stall takes. 250 ms puts the floor at 4 fps -- below any rate this viewer is usable
    // at -- while a real hitch (a window rebuild, a shader compile, the debugger) is still longer
    // than that and still drops. A machine that is simply slow is now told apart from a hitch by the
    // pacing line's per-second clamp count, not by this constant.
    uint64_t max_wall_dt_us = 250000;

    // S7.3.2. Past this the loop stops trying to catch up by stepping and micro-seeks instead. With
    // the clamp above, only a HIGH RATE can reach it: at 8x a clamped 100 ms frame asks for 800 ms
    // and four substeps cannot pay it, which is fast-forward working as asked, not a hitch.
    double catchup_limit_us = 2.0e6;
    // S7.5.3's K -- a bound on the STEPPING WORK one frame may do, not on how far it may advance.
    //
    // ★★★ K IS IN TICKS AND THE COMMENT ABOVE IS IN MILLISECONDS, AND THAT IS THE BUG PERF-2 FOUND.
    // "Four substeps cannot pay 800 ms" assumes the 33 ms table the tests use: K*33 = 133 ms, so at
    // 1x only a real stall reaches it. The owner's captures are recorded at the headset's rate, ~11
    // ms a tick, where K*11 = 44 ms -- so at 1x EVERY frame slower than 22 fps saturated, micro-
    // seeked, and paid for it with a full category-N residue battery (~100 ms at eight players),
    // which made the next frame slower still. The loop latched at one micro-seek per frame and the
    // owner's 4v4 sat at 6-10 fps for the rest of the session.
    //
    // The body is two array reads and a subtract, so 64 of them is under a microsecond: K bounds a
    // pathological table walk, and `catchup_limit_us` above is what actually decides that catching
    // up by stepping has stopped being sensible. At 1x, 64 ticks of any capture cadence outruns the
    // 250 ms wall clamp, so 1x no longer micro-seeks at all; at 8x a clamped frame asks for 2 s, K
    // pays ~0.7 s of it and the remainder reaches catchup_limit_us -- fast-forward working as asked,
    // which is the behaviour the line above describes and the one the tests assert.
    uint32_t max_substeps = 64;
};

// What one frame of transport decided. The caller writes `commanded_step_us` to the clock and
// publishes at `display_playhead_us`.
struct StepResult {
    int64_t display_playhead_us = 0;   // solver + remainder; continuous, every frame
    int64_t solver_playhead_us = 0;    // lands only on recorded boundaries
    uint32_t steps_consumed = 0;
    // ★ NEVER ZERO. S7.3.3: a zero delta makes clock+0x40 = 1/0 = +inf and UpdateTargetState divides
    // by it. 1 us when no whole tick is affordable, which is also how pause is expressed.
    uint64_t commanded_step_us = 1;
    bool micro_seek = false;
    bool saturated = false;            // the loop wanted more than max_substeps
    // The resident table ran out in the direction of travel and the playhead HELD at its edge
    // instead of being jumped past it. Coverage lagging the playhead is a reason to wait, never a
    // reason to skip the span nobody has decoded yet.
    bool held_for_coverage = false;
    // The playhead is on the first or last sample of the RECORDING and the transport paused itself
    // there. Set on the frame that arrives and on every frame it stays, so a UI that only looks at
    // this frame's result still shows "ended".
    EndState end_state = EndState::None;
    // Real microseconds this frame was over `max_wall_dt_us` and did not spend. Non-zero means a
    // hitch, and the playhead deliberately did not catch up on it.
    uint64_t wall_dropped_us = 0;
    // ★ A recorded tick longer than max_step_us was crossed rather than stepped. Non-zero means the
    // file has a gap here, and Cause::GapCross was raised for it.
    bool crossed_gap = false;
};

class Transport {
public:
    void Reset(const TickTable& ticks, const TransportTuning& tuning);

    // The transport's own state. Rate is signed; 0 means paused and is expressed as a 1 us step, never
    // as a zero step and never as timescale 0 (S7.3.3, and the HeartbeatBefore arithmetic in
    // W2-B.md F3a is why the floor makes that safe).
    void SetRate(double rate);
    double Rate() const { return rate_; }
    void SetPaused(bool p) { paused_ = p; }
    bool Paused() const { return paused_; }

    // ★★★ THE RESIDENT TABLE MOVED UNDER THE PLAYHEAD. The window refills its tick storage IN
    // PLACE on every rebuild, so the pointer this transport borrowed stayed valid while the boundaries
    // behind it became a different three chapters -- and the COUNT stayed the one taken at load. Then
    // `index_` named a row of a table that no longer existed: playback crossed chapter seams by
    // accident and no seek could reach past the load-time window (L3-RUN-3 F1).
    //
    // ★★ THE PLAYHEAD IS PRESERVED BY TIME, NEVER BY INDEX. Boundary i of the old window and
    // boundary i of the new one are different instants, so keeping the index would teleport the
    // playhead by however far the window rolled, on a rebuild the user never asked for. This is not a
    // jump -- the position in time does not change -- so it raises no discontinuity.
    //
    // ★★★ AND THE TIME IT PRESERVES IS THE CACHED ONE, NOT At(index_) + acc_ (PLAYBACK-FIX-3 A).
    // The caller REFILLS THE STORAGE FIRST and hands the table over afterwards, so by the time this
    // runs `At(index_)` already reads the NEW window's boundaries at the OLD window's index -- and
    // "preserving" that reading moved the playhead forward by exactly the span the window rolled off:
    // one chapter, every rebuild, 2.02 s and 4.01 s on the capture that reported it. That is the
    // whole of the sprint. `display_us_` is written by every mutator BEFORE any table can be swapped,
    // so it is the one reading a refill cannot corrupt.
    void Retable(const TickTable& ticks);

    // WHETHER THE RESIDENT TABLE IS THE FILE'S OWN EDGE -- the caller's fact, because it owns the
    // chapters and this class only ever sees one table.  It decides whether running out of table
    // in the direction of travel is the END OF THE RECORDING (clamp, self-pause, EndState) or
    // coverage lagging the playhead (hold, and wait for the rebuild).  The span test cannot answer
    // it at the front: the manifest origin can precede the first recorded tick, and reverse play
    // reaching that tick then holds forever with EndClamps at zero.
    void SetTableEdges(bool atFileStart, bool atFileEnd) {
        table_at_file_start_ = atFileStart;
        table_at_file_end_ = atFileEnd;
    }

    // ★★★ THE FILE'S OWN SPAN, WHICH THE TABLE CANNOT SEE. The table is three chapters; the end of
    // it is not the end of the recording, and only the caller knows which is which. Without that
    // distinction the accumulator's surplus at index 0 of a resident window cannot be told from the
    // surplus at the first frame of the FILE -- and discarding it in the first case stalls reverse at
    // every chapter seam, while keeping it in the second runs the display playhead off the front of
    // the recording (L3-RUN-3 F5 measured -20880 us on a file whose first tick is 0).
    //
    // Unset means unknown, and an unknown span clamps nothing: a transport nobody told where the file
    // ends must not invent an answer.
    void SetFileSpan(int64_t first_us, int64_t last_us);
    bool FileSpanKnown() const { return span_set_; }

    // A jump. Does not apply state -- it moves the playhead and records that the next frame crosses a
    // discontinuity. That is the whole of W2-B.md S6.1.
    void SeekTo(int64_t t_us);
    // Consume exactly n recorded ticks, ignoring the accumulator. Negative steps backwards.
    // Returns the SIGNED number of ticks actually consumed: 0 at either end of the table, and
    // short of |n| when the table ran out partway. This is the only source for that number --
    // the playhead cannot be differenced for it across a repeated boundary.
    int StepTicks(int n);

    // The recorded boundary at or below `t_us`, in ABSOLUTE source microseconds. It is read out of
    // the table THIS transport steps over, so a caller can never be told a boundary StepTicks would
    // not land on. False with `*out_us` untouched when the table holds nothing, or when `t_us` is
    // before the first recorded sample -- there is no boundary at or before it, and the clamp
    // SnapToTarget applies is a seek behaviour, not an answer to this question.
    //
    // O(log n): TickTable::IndexAt's binary search, the same one SnapToTarget uses.
    bool TickAtOrBelow(int64_t t_us, int64_t* out_us) const;

    StepResult Advance(uint64_t wall_dt_us);

    int64_t DisplayPlayheadUs() const;
    int64_t SolverPlayheadUs() const;
    double AccumulatorUs() const { return acc_us_; }

    // True exactly once per discontinuity, consumed by the caller's frame loop. The CAUSE travels with
    // it: without that, Cause::GapCross was declared and produced nowhere, so the residue that exists
    // to absorb a gap never fired on one.
    bool TakeDiscontinuity(int64_t& from_us, int64_t& to_us, Cause& cause);

    uint64_t MicroSeeks() const { return micro_seeks_; }
    uint64_t StepsTotal() const { return steps_total_; }
    // ★★ REPLACES NonRecordedSteps(), WHICH COULD NEVER FIRE. That counter asked "was the commanded
    // step a recorded dt", and the step is READ OUT OF the recorded table one line earlier -- so it was
    // a guard whose red was unconstructible, which this project has shipped three of. This one asks a
    // question the data can answer NO to: how many recorded ticks were too long for the engine to
    // step. A file with a gap makes it non-zero, which is a constructible red.
    uint64_t GapsCrossed() const { return gaps_crossed_; }
    // ★★ COUNTED SEPARATELY FROM GapsCrossed, because they are the same remedy and different
    // findings. A gap is the file honestly reporting that time passed with nothing recorded; a
    // regression is the tick table being out of order, which is a defect in the file or in the way it
    // was assembled. Folding them would make a corrupt table indistinguishable from a stalled capture.
    uint64_t TableRegressions() const { return table_regressions_; }
    // Duplicate stamps crossed without commanding a step. Legal in the file, and a real reading.
    uint64_t ZeroDtSteps() const { return zero_dt_steps_; }
    // Frames whose leftover accumulator would have shown a playhead the file does not contain, and
    // was dropped to the edge instead. Counted rather than silent: it is wall time the replay could
    // not spend, and a non-zero count away from either end of a file would be a defect in the clamp.
    uint64_t EndClamps() const { return end_clamps_; }
    // The largest step this transport ever commanded. Must never exceed max_step_us; a test asserts it
    // over a table that contains a five-second tick.
    uint64_t LargestStepUs() const { return largest_step_us_; }
    // Frames whose real interval was longer than max_wall_dt_us, and the real time they dropped.
    // A viewer that hitches shows a non-zero count and a playhead that did NOT sprint afterwards.
    uint64_t WallClamps() const { return wall_clamps_; }
    uint64_t WallDroppedUs() const { return wall_dropped_us_; }
    // Frames the playhead held at the edge of the resident table because there was nothing decoded
    // to step onto. A hold is the correct answer to a window that has not caught up yet. It stops at
    // the ends of the FILE, which are a state and not a wait -- see EndState.
    uint64_t CoverageHolds() const { return coverage_holds_; }
    // Which end of the recording the playhead is parked on, if either. Cleared the moment it moves.
    EndState Ended() const { return end_state_; }
    // Rebuilds whose new table did not cover the held time, so the clamp moved the published
    // playhead. Each one raised a discontinuity; a non-zero count away from a seek is a window that
    // was built around the wrong instant.
    uint64_t RetableJumps() const { return retable_jumps_; }

private:
    void SnapToTarget(int64_t target_us);
    // Drops the part of the accumulator that would show a playhead outside the file's own span.
    void ClampDisplayToFileSpan();
    // The same, against the RESIDENT table: a playhead past either edge of it has no samples behind
    // it, so it waits there rather than running into a span the window has not built.
    void ClampDisplayToTable();
    int64_t ComputeDisplayUs() const;
    void NoteDisplay() { display_us_ = ComputeDisplayUs(); }

    TickTable ticks_;
    TransportTuning tuning_;
    size_t index_ = 0;         // solver playhead == ticks_.At(index_)
    double acc_us_ = 0.0;      // signed sub-tick + catch-up remainder
    double rate_ = 1.0;
    bool paused_ = true;

    bool disc_pending_ = false;
    int64_t disc_from_ = 0, disc_to_ = 0;
    Cause disc_cause_ = Cause::Seek;

    uint64_t micro_seeks_ = 0, steps_total_ = 0;
    uint64_t gaps_crossed_ = 0, largest_step_us_ = 0;
    uint64_t table_regressions_ = 0, zero_dt_steps_ = 0;
    uint64_t end_clamps_ = 0;
    uint64_t wall_clamps_ = 0, wall_dropped_us_ = 0, coverage_holds_ = 0;
    uint64_t retable_jumps_ = 0;
    bool table_at_file_start_ = false, table_at_file_end_ = false;
    EndState end_state_ = EndState::None;

    // The published playhead, in the units everything else on this interface speaks. Held as a
    // VALUE rather than recomputed from the table, because the table is borrowed storage somebody
    // else refills -- see Retable.
    int64_t display_us_ = 0;

    bool span_set_ = false;
    int64_t span_first_us_ = 0, span_last_us_ = 0;
};

}  // namespace playback
}  // namespace replay
