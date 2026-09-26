#pragma once

// The S6.4 per-tick cost histogram, the degradation ladder and the kill switch.
//
// Measured with RDTSC on the tick thread, converted to microseconds on the worker: QueryPerformanceCounter
// is cheap but is still a call through a shared page, and S6.4 asks for zero syscalls on the tick. The
// TSC-to-microsecond ratio is calibrated once, off the tick, against QPC.
//
// ★ THE LADDER MAY NOT SHED A REQUIRED STREAM. S6.4 RULE 0 is explicit, and the version this design
// shipped violated it: ASSEEN is required by ruling Q1 and S_ORACLE by ruling Q-B, so shedding them to
// meet SC4 would make a budget gate satisfiable by deleting a required feature. Only two rungs exist
// here, both genuinely optional load: the oracle's CADENCE (never to zero) and the keyframe interval
// (within SC5's seek budget). If the budget cannot be met with every required stream on, SC4 fails and
// says so -- which is a result, not a failure of this class.
//
// Every rung writes a DEGRADED record into the file naming what changed and why, so a file is honest
// about its own fidelity rather than quietly different from the next one.

#include <stdint.h>

namespace replay {
namespace capture {

// Powers-of-two-ish buckets over the range S6.4 cares about: the median target is 20 us, the p99 is
// 50 us and the hard fail is 200 us, so the resolution is finest where the budget lives.
//
// ★ AND OVER THE RANGE THAT WAS ACTUALLY MEASURED. L2 reported "median 5000 us ... max 4506 us",
// which reads as an impossible median-above-max and made the instrument look like the defect: above
// 2000 us the ladder went 2000 -> 5000 -> 20000, so every real cost between 2 ms and 5 ms rendered as
// the bucket CEILING 5000. A histogram that cannot express the number it killed a capture on cannot
// be argued with. The rungs from 1000 us up are finer now, and every quantile is reported as the
// half-open INTERVAL it really is (see medianLoUs) rather than as a ceiling wearing a point value.
constexpr uint32_t kCostBuckets = 39;

struct CostReport {
    uint64_t samples;
    // Each quantile is a BUCKET, so it is reported as the half-open interval (loUs, Us] that the
    // measurement actually established. Reporting only the ceiling is what produced L2's
    // "median 5000 us ... max 4506 us".
    uint32_t medianUs;
    uint32_t medianLoUs;
    uint32_t p99Us;
    uint32_t p99LoUs;
    uint32_t p999Us;
    uint32_t maxUs;
    uint64_t buckets[kCostBuckets];
    uint32_t bucketUpperUs[kCostBuckets];
    // The rolling window S6.4 rung 4 acts on: p99 over the last 512 ticks.
    uint32_t windowP99Us;
};

class CostMeter {
public:
    void Init();

    // Tick thread. Returns the raw TSC; hand it back to Stop. Allocation-free, lock-free, syscall-free.
    uint64_t Start() const;
    void Stop(uint64_t startTsc);

    void Report(CostReport& out) const;

    // ---- the ladder ----------------------------------------------------------------------------
    enum class Rung : uint8_t {
        Nominal = 0,
        OracleCadenceReduced,   // rung 1: the oracle samples less often, never zero
        KeyframeIntervalRaised, // rung 2: within SC5's seek budget
        Killed,                 // p99.9 over 2x the hard fail for three windows: stop, flag, close clean
    };

    // Called on the worker after each window. Returns true when the rung CHANGED, so the caller writes
    // exactly one DEGRADED record per transition rather than one per window.
    bool Evaluate(Rung& newRung);

    // What the last complete window actually measured. The kill rung used to reach its verdict and
    // report nothing -- L5 re-run #4 killed a live match and left no number anywhere in the log, the
    // sentinel or the file -- so the measurement the ladder acted on is now readable and the kill
    // line quotes it.
    struct WindowStat {
        uint32_t medianUs = 0;
        uint32_t medianLoUs = 0;   // the median bucket is (medianLoUs, medianUs]
        uint32_t p99Us = 0;
        uint32_t p99LoUs = 0;
        uint32_t maxUs = 0;
        uint64_t windows = 0;        // complete windows evaluated this launch
        uint32_t overKillWindows = 0;  // consecutive windows whose median passed kKillMedianUs
        // The same count for the tail rung, kept separate because which rung fired is the whole
        // diagnosis: one says the typical tick got expensive, the other says a few ticks stalled.
        uint32_t overKillTailWindows = 0;
        // Whether the tail rung was ASKED at all this window. p99 over the charged population is a
        // tail only while that population is big enough to have one (see Evaluate); when it is not
        // the rung is NOT-RUN and the counter above is neither a pass nor a fail. A two-state
        // publication could not say that, and a reader would take the zero for health.
        bool tailMeasured = false;
        // Windows this launch the tail rung could not be asked on, so the absence of a kill has a
        // stated reason rather than an implied one.
        uint32_t tailNotRunWindows = 0;
        // L3-RUN-13 F3. The same answers as COUNTS over the launch. Everything above is the LAST
        // window, and on a run the capture SURVIVES there is no last window worth reading: the
        // rung's verdict was computed and discarded, so a close could not say whether the tail rung
        // ever ran, let alone that it read over its line and the amount term is what kept the file
        // open. `tailKillWindows` counts windows meeting the WHOLE kill condition; kKillWindows
        // CONSECUTIVE of those close the file, so a non-zero here beside a live capture is the
        // amount term doing exactly what it was added for.
        uint32_t tailMeasuredWindows = 0;
        uint32_t tailOverBudgetWindows = 0;
        uint32_t tailKillWindows = 0;
        // The always-on half of the same window: work the plugin does whether or not a capture is
        // open, measured and reported but NOT charged (see AddUnchargedUs).
        uint32_t unchargedMeanUs = 0;
        uint32_t unchargedMaxUs = 0;
        // How many ticks of this window put a sample in the CHARGED histogram -- the denominator
        // every quantile above was taken over. It is not kWindowTicks: since L2-RUN-8 F12 a tick
        // with no capture open adds nothing here, so this reads 0 for a window the plugin spent
        // idle and a test can assert that rather than infer it from a median that happens to be
        // small.
        uint32_t chargedSamples = 0;
        // What an open capture cost the frame ON AVERAGE over this window, and the second half of
        // the tail rung's kill condition (kKillTailMeanUs). Published so a tail that read over its
        // line and did not kill is a stated number rather than a silence.
        uint32_t chargedMeanUs = 0;
        // Whether the tail read over kKillTailUs this window. Separate from overKillTailWindows,
        // which counts only windows that met the WHOLE kill condition: a window can carry a spike
        // above the tail line and still cost the frame nothing, and the log has to say which.
        bool tailOverBudget = false;
    };
    WindowStat LastWindow() const { return lastWindow_; }

    // The ingestion half of Stop(), without the clock. Stop() measures an interval and calls this;
    // a caller that already has a duration -- the worker's cost-window injection, AUDIT_M7 F4 --
    // calls it directly, so both reach the meter through exactly one accumulation path.
    void AddSampleUs(uint32_t us);

    // ★ THE NUMERATOR (L2-RUN-2 F1). Tick work that happens whether or not a capture is open is
    // recorded here instead of AddSampleUs, so it appears in the diagnostic and never in the
    // budget. Charging it is what killed L2: `gate` cost 1685-1941 us per tick for four minutes
    // after the capture was already dead, so the kill fired on a number no capture could lower and
    // no amount of making capture cheaper could ever have cleared it.
    //
    // The kill line stays the CONSTANT kKillMedianUs rather than becoming a measured no-capture
    // baseline. A baseline derived at runtime rises with whatever always-on work we ship, so a
    // future defect of exactly this shape would raise its own kill line and pass -- a guard that
    // adapts to the thing it exists to catch. A constant budget over a numerator that contains only
    // capture-attributable work states the design's claim directly: an open capture may cost the
    // frame 200 us typical, and what the plugin costs when idle is a different number with a
    // different owner, printed beside it.
    //
    // Called exactly ONCE PER TICK, which is also what lets a window close while no capture is
    // open: Evaluate ends a window on either counter reaching kWindowTicks, so the always-on
    // floor is still reported during an idle stretch instead of accumulating unbounded into the
    // first window after the next capture opens.
    void AddUnchargedUs(uint32_t us);
    Rung CurrentRung() const { return rung_; }

    // The calibration the tick's own phase breakdown converts with, so there is one TSC-to-microsecond
    // ratio in the module rather than a second one measured somewhere else. Zero means the machine's
    // TSC did not move and no cost figure from this meter is a measurement.
    double TscPerUs() const { return tscPerUs_; }

    // The current settings the ladder controls. Both start at the design's shipping values.
    uint32_t OracleDivisor() const { return oracleDivisor_; }
    uint32_t KeyframeIntervalUs() const { return keyframeIntervalUs_; }
    void SetKeyframeIntervalUs(uint32_t us) { keyframeIntervalUs_ = us; }

    // S6.4's budget numbers, in one place so a gate reads them rather than restating them.
    // Public because a window is the unit, and the hard-fail budget the magnitude, that anything
    // driving this meter has to express itself in.
    static constexpr uint32_t kWindowTicks = 512;
    static constexpr uint32_t kP999HardFailUs = 200;

    static constexpr uint32_t kMedianBudgetUs = 20;
    static constexpr uint32_t kP99BudgetUs = 50;

    // ★ THE KILL RUNG READS THE WINDOW MEDIAN, NOT A TAIL QUANTILE, AND THIS IS WHY.
    //
    // A 512-sample window cannot express a 99.9th percentile. Percentile() returns the first bucket
    // whose cumulative count passes floor(n*p); at n=512, p=0.999 that is floor(511.488)=511, so it
    // returns the bucket holding the 512th sample -- the window MAXIMUM, every time. The rung that
    // read it was therefore "the single worst tick in this window passed 400 us", three windows
    // running, and the sample is a wall-clock RDTSC bracket that includes any OS preemption of the
    // tick thread. One descheduled tick per 512 is not a cost we spent; it is normal Windows.
    //
    // L5 re-run #4 (2026-09-03) is that arithmetic in the field: a healthy 32 s solo match was
    // killed at the third window boundary -- the earliest instant the ladder can reach Killed --
    // while the manifest it wrote still carries keyframe_us=2000000, so rung 2 never fired, so at
    // most ONE window in the whole run had p99 over the 50 us budget. Tail catastrophic, body inside
    // budget: exactly one outlier per window.
    //
    // The median is what the kill exists to bound -- "our tick work is now typically this
    // expensive" -- and a handful of outliers cannot move it. 200 us is 10x the S6.4 median budget;
    // at 30-90 Hz that is a capture eating 0.6-1.8% of wall time as its TYPICAL cost, which is a
    // runaway by any reading. The margin over the run that was killed is at least 4x: its p99 stayed
    // inside 50 us, so its median did too.
    static constexpr uint32_t kKillMedianUs = 10 * kMedianBudgetUs;

    // The quantile the tail rung reads, as a percent, in one place: the rung compares against it and
    // Evaluate's expressibility test solves for it, so the two cannot drift apart.
    static constexpr uint32_t kTailQuantilePct = 99;

    // AND THE TAIL RUNG BESIDE IT, because the median rung above is blind to a stall that misses
    // half the window. L2 #15 measured 159 ms on one tick in fifteen: 6.7 % of the window, which
    // moves no median at all and is four orders of magnitude over the budget on the ticks it hits.
    //
    // It reads p99, not the p99.9 the kill switch used to. The design states the tail budget as
    // \"same, p99.9 | -- | > 200 us\" with the kill at \"p99.9 exceeds 2x the hard fail for three
    // consecutive windows\"; over 512 samples p99.9 is not expressible -- floor(512*0.999)=511 makes
    // Percentile() return the window MAXIMUM every time, which is the arithmetic that killed a
    // healthy match in L5 re-run #4. p99 over 512 samples IS expressible: it is the 507th sample, so
    // one descheduled tick per window (0.2 %) cannot reach it and a 1-in-15 stall (6.7 %) cannot
    // avoid it. The magnitude stays the design's own hard-fail number, 200 us, and the rung stays
    // AND IT IS ASKED ONLY OVER A POPULATION THAT CAN EXPRESS IT (L3-RUN-11 F5). Every quantile here
    // is taken over the CHARGED sample count, which Evaluate documents may be smaller than the
    // window: the window closes on 512 ticks OR 512 charged samples, whichever comes first, so a
    // launch whose capture is open for only part of the tick stream closes windows on UNCHARGED
    // ticks with the charged population truncated. Over a truncated population p99 is not a tail --
    // at 100 charged samples or fewer the 99th percentile IS the maximum -- and the rung silently
    // becomes "the single worst charged tick passed 200 us", which is the same p99.9-over-512
    // arithmetic this header condemns, reached through foreign ticks instead of through the
    // quantile. That is what closed the always-on capture of a PLAYBACK session at window 4 with a
    // 15-20 us median. See Evaluate for the integer solve and for why an unaskable window is
    // NOT-RUN rather than a pass.
    //
    // three consecutive windows, so what changed is the quantile the budget is read at and nothing
    // else. The median rung is untouched and either rung alone reaches Killed.
    static constexpr uint32_t kKillTailUs = kP999HardFailUs;

    // AND THE AMOUNT BESIDE THE HEIGHT (L3-RUN-12 F4). A quantile states how HIGH the tail is and
    // nothing about what it COST. S6.4's subject is "capture must never degrade a live match", and
    // what a window of capture costs a frame is its mean; a capture with an amortised, change-gated
    // pass -- the roster rescan runs only on the ticks the engine's entrant count moves -- puts that
    // pass above the p99 index whenever it lands on more than 1 % of the window, however cheap the
    // window is as a whole. RUN-12 is that measurement: median 15-20 us, charged mean 51 us, p99 in
    // (750, 1000] us, max 1278 us, over a FULL 512 charged samples -- askable, asked, and the file
    // was closed on a capture costing 0.5 % of a 90 Hz frame.
    //
    // So the tail term carries the amount too, at the magnitude the median rung already kills on:
    // a tail may close a file only when the window's charged work AVERAGES what the median rung
    // would kill for. Derived from kKillMedianUs rather than restated -- one kill magnitude, two
    // ways of reading the window. L2 #15's 1-in-15 stall of 159 ms averages 10.6 ms per tick and
    // still kills; a handful of sub-millisecond spikes on a 17 us body averages 26-34 us and no
    // longer does.
    static constexpr uint32_t kKillTailMeanUs = kKillMedianUs;
    static constexpr uint32_t kKillWindows = 3;

    static constexpr uint32_t kSeekBudgetKeyframeMaxUs = 4000000;  // SC5: 2 s doubled, still O(1) seek

private:
    static uint32_t BucketOf(uint32_t us);

    double tscPerUs_ = 0.0;
    uint64_t buckets_[kCostBuckets] = {};
    uint64_t samples_ = 0;
    uint32_t maxUs_ = 0;

    // The rolling window, kept as a second histogram so no per-sample array is needed and the tick
    // stays allocation-free.
    uint64_t windowBuckets_[kCostBuckets] = {};
    uint32_t windowCount_ = 0;
    uint32_t windowP99Us_ = 0;

    uint32_t windowMaxUs_ = 0;
    uint64_t windowSumUs_ = 0;
    // The uncharged window, kept alongside the charged one and snapshotted by the same Evaluate.
    uint32_t tailNotRunWindows_ = 0;
    uint64_t unchargedSumUs_ = 0;
    uint32_t unchargedCount_ = 0;
    uint32_t unchargedWindowMaxUs_ = 0;
    WindowStat lastWindow_{};

    Rung rung_ = Rung::Nominal;
    uint32_t overBudgetWindows_ = 0;
    uint32_t hardFailWindows_ = 0;
    uint32_t tailFailWindows_ = 0;
    uint32_t tailMeasuredWindows_ = 0;
    uint32_t tailOverBudgetWindows_ = 0;
    uint32_t tailKillWindows_ = 0;
    uint32_t oracleDivisor_ = 1;
    uint32_t keyframeIntervalUs_ = 2000000;   // S5.5: 2.0 s of source time
};

}  // namespace capture
}  // namespace replay
