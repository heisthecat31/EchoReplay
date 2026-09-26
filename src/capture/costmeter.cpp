#include "capture/costmeter.h"

#include <windows.h>

#include <intrin.h>
#include <string.h>

namespace replay {
namespace capture {
namespace {

// Bucket upper bounds in microseconds. Dense from 1 to 64 because that is where the median and p99
// budgets sit; coarse above 256 because anything up there is already a hard fail and only its
// magnitude matters.
constexpr uint32_t kUpper[kCostBuckets] = {
    1,    2,    3,    4,    5,    6,    8,     10,    12,    15,    20,    25,   30,
    40,   50,   64,   80,   100,  125,  150,   200,   250,   300,   400,   500,  750,
    1000, 1500, 2000, 2500, 3000, 4000, 5000,  7500,  10000, 20000, 50000, 100000,
    0xFFFFFFFFu};

static_assert(sizeof(kUpper) / sizeof(kUpper[0]) == kCostBuckets,
              "the bucket table and kCostBuckets disagree");

// The exclusive lower bound of a bucket. Bucket 0 starts at zero; every other bucket starts where its
// predecessor ended. Quantiles are reported as (Lower(i), kUpper[i]] because that -- and not the
// ceiling alone -- is what a histogram measured.
uint32_t Lower(uint32_t i) { return i == 0 ? 0u : kUpper[i - 1]; }

}  // namespace

uint32_t CostMeter::BucketOf(uint32_t us) {
    for (uint32_t i = 0; i < kCostBuckets; ++i) {
        if (us <= kUpper[i]) return i;
    }
    return kCostBuckets - 1;
}

void CostMeter::Init() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);

    // Calibrate TSC against QPC over a short spin. This runs at capture open, off the tick.
    LARGE_INTEGER a{}, b{};
    QueryPerformanceCounter(&a);
    const uint64_t t0 = __rdtsc();
    for (;;) {
        QueryPerformanceCounter(&b);
        if (b.QuadPart - a.QuadPart >= f.QuadPart / 200) {   // ~5 ms
            break;
        }
    }
    const uint64_t t1 = __rdtsc();
    const double seconds = double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
    tscPerUs_ = seconds > 0.0 ? double(t1 - t0) / (seconds * 1e6) : 0.0;
    if (!(tscPerUs_ > 0.0)) {
        // A machine whose TSC did not move is a measurement we do not have. Zero here makes every
        // sample land in bucket 0 and Report say samples=0, which reads as NOT-RUN rather than as a
        // flattering set of very fast ticks.
        tscPerUs_ = 0.0;
    }
    memset(buckets_, 0, sizeof buckets_);
    memset(windowBuckets_, 0, sizeof windowBuckets_);
    samples_ = 0;
    maxUs_ = 0;
    windowCount_ = 0;
    windowP99Us_ = 0;
    windowMaxUs_ = 0;
    lastWindow_ = WindowStat{};
    rung_ = Rung::Nominal;
    overBudgetWindows_ = 0;
    hardFailWindows_ = 0;
    oracleDivisor_ = 1;
}

uint64_t CostMeter::Start() const { return __rdtsc(); }

void CostMeter::Stop(uint64_t startTsc) {
    if (tscPerUs_ <= 0.0) {
        return;
    }
    const uint64_t now = __rdtsc();
    if (now <= startTsc) {
        return;   // a migration between cores with unsynchronised TSCs; not a measurement
    }
    AddSampleUs(static_cast<uint32_t>(double(now - startTsc) / tscPerUs_));
}

void CostMeter::AddSampleUs(uint32_t us) {
    const uint32_t b = BucketOf(us);
    ++buckets_[b];
    ++windowBuckets_[b];
    ++samples_;
    ++windowCount_;
    windowSumUs_ += us;
    if (us > maxUs_) {
        maxUs_ = us;
    }
    if (us > windowMaxUs_) {
        windowMaxUs_ = us;
    }
}

void CostMeter::AddUnchargedUs(uint32_t us) {
    unchargedSumUs_ += us;
    ++unchargedCount_;
    if (us > unchargedWindowMaxUs_) {
        unchargedWindowMaxUs_ = us;
    }
}

namespace {

// Returns the BUCKET INDEX holding the p-th sample, so a caller can report both of its bounds. An
// empty histogram has no bucket and says so with kCostBuckets.
uint32_t PercentileBucket(const uint64_t* buckets, uint64_t total, double p) {
    if (total == 0) {
        return kCostBuckets;
    }
    const uint64_t want = static_cast<uint64_t>(double(total) * p);
    uint64_t seen = 0;
    for (uint32_t i = 0; i < kCostBuckets; ++i) {
        seen += buckets[i];
        if (seen > want) {
            return i;
        }
    }
    return kCostBuckets - 1;
}

uint32_t Percentile(const uint64_t* buckets, uint64_t total, double p) {
    const uint32_t b = PercentileBucket(buckets, total, p);
    return b < kCostBuckets ? kUpper[b] : 0u;
}

uint32_t PercentileLower(const uint64_t* buckets, uint64_t total, double p) {
    const uint32_t b = PercentileBucket(buckets, total, p);
    return b < kCostBuckets ? Lower(b) : 0u;
}

}  // namespace

void CostMeter::Report(CostReport& out) const {
    out.samples = samples_;
    out.medianUs = Percentile(buckets_, samples_, 0.50);
    out.medianLoUs = PercentileLower(buckets_, samples_, 0.50);
    out.p99Us = Percentile(buckets_, samples_, 0.99);
    out.p99LoUs = PercentileLower(buckets_, samples_, 0.99);
    out.p999Us = Percentile(buckets_, samples_, 0.999);
    out.maxUs = maxUs_;
    out.windowP99Us = windowP99Us_;
    for (uint32_t i = 0; i < kCostBuckets; ++i) {
        out.buckets[i] = buckets_[i];
        out.bucketUpperUs[i] = kUpper[i];
    }
}

bool CostMeter::Evaluate(Rung& newRung) {
    // EITHER counter closes the window (L2-RUN-8 F12). It used to be the charged one alone, which
    // was the same number until a tick with no capture open stopped contributing a charged sample:
    // after that a window could only close while a capture was open, so an idle stretch published
    // no uncharged mean at all and its ticks all landed in the first window after the next open.
    // unchargedCount_ is one per tick, so this closes on 512 ticks or 512 charged samples,
    // whichever comes first, and every quantile below is taken over windowCount_ -- the charged
    // half's own count, which may now be smaller than the window.
    if (windowCount_ < kWindowTicks && unchargedCount_ < kWindowTicks) {
        newRung = rung_;
        return false;
    }
    const uint32_t charged = windowCount_;
    const uint32_t chargedMean =
        charged ? static_cast<uint32_t>(windowSumUs_ / charged) : 0u;
    const uint32_t median = Percentile(windowBuckets_, windowCount_, 0.50);
    const uint32_t medianLo = PercentileLower(windowBuckets_, windowCount_, 0.50);
    const uint32_t p99 = Percentile(windowBuckets_, windowCount_, 0.99);
    const uint32_t p99Lo = PercentileLower(windowBuckets_, windowCount_, 0.99);
    windowP99Us_ = p99;
    memset(windowBuckets_, 0, sizeof windowBuckets_);
    windowCount_ = 0;
    windowSumUs_ = 0;

    const Rung before = rung_;

    // The kill reads the median. See kKillMedianUs in the header for why the tail cannot be read
    // here and what reading it cost in L5 re-run #4.
    //
    // ★ AND IT READS THE MEDIAN BUCKET'S LOWER BOUND, NOT ITS CEILING. A quantile off a histogram is
    // an interval; comparing the ceiling asserts a cost the samples do not establish, and would kill a
    // capture whose true median sat anywhere inside the bucket. medianLo >= kKillMedianUs says exactly
    // "more than half of this window PROVABLY cost more than the kill line", because a sample in the
    // bucket (medianLo, median] is strictly above medianLo.
    if (medianLo >= kKillMedianUs) {
        if (++hardFailWindows_ >= kKillWindows) {
            rung_ = Rung::Killed;
        }
    } else {
        hardFailWindows_ = 0;
    }

    // The tail rung, beside the median one and on the same three-window latch. p99Lo, not p99,
    // for the reason the median rung takes medianLo: a quantile off a histogram is an interval,
    // and comparing the bucket ceiling asserts a cost the samples do not establish.
    //
    // AND IT IS ASKED ONLY WHERE p99 IS A TAIL (L3-RUN-11 F5). Every quantile above is taken over
    // `charged`, which the window-close rule at the top of this function allows to be far smaller
    // than kWindowTicks: an uncharged tick closes the window too, so a launch whose capture is open
    // on part of the tick stream -- a playback session, where the plugin's other work runs on the
    // same tick -- reaches Evaluate with a truncated charged population. PercentileLower returns the
    // sample at index floor(charged * kTailQuantilePct / 100), and that index is strictly below the
    // last sample only while
    //
    //     floor(charged * 99 / 100) < charged - 1        i.e. charged > 100
    //
    // Below it the "tail" IS the window maximum, and the rung means "the single worst charged tick
    // passed 200 us" -- the L5 re-run #4 arithmetic, arrived at through foreign ticks shortening the
    // population rather than through the quantile. Solved in integers rather than compared against a
    // literal 101, so kTailQuantilePct stays the only place the quantile is stated.
    //
    // AN UNASKABLE WINDOW IS NOT-RUN, NOT A PASS. S6.4's measurement protocol rules on exactly this
    // -- "A measurement taken while the machine was contended is NOT-RUN with a reason, never FAIL"
    // -- and a pass would be the mirror defect: it would clear a latch that genuinely failing
    // windows had set, so a capture could stall its way past the rung by interleaving windows it
    // never gave the rung enough samples to judge. The latch is HELD and the window is counted, so
    // the stop line can say the rung was never asked instead of leaving its zero to read as health.
    //
    // AND A TAIL CLOSES A FILE ONLY WHERE IT COSTS THE FRAME (L3-RUN-12 F4). The guard above asks
    // whether p99 is a tail at all; this asks whether the tail it read is a runaway. p99 over 512
    // charged samples is the 507th tick, so any pass that runs on more than 1 % of ticks sits above
    // it -- and capture HAS one: ScanRosterTick is behind an entrant-count gate, so it does nothing
    // on most ticks and a full walk with profile reads on the few where the engine's roster moves.
    // RUN-12 closed a playback session's own capture on exactly that (roster 20.3/840 us mean/max,
    // stats 10.8/407) with a 15-20 us median and a 51 us charged mean, over a FULL window: the rung
    // was asked, and what it lacked was the amount. kKillTailMeanUs is the median rung's own kill
    // line read over the window's mean, so a 1-in-15 stall (10.6 ms a tick) still kills and a
    // handful of sub-millisecond spikes on a cheap body no longer does. See the header.
    const bool tailMeasurable =
        charged > 1u && (charged * kTailQuantilePct) / 100u < charged - 1u;
    const bool tailOverBudget = tailMeasurable && p99Lo >= kKillTailUs;
    // L3-RUN-13 F3. The launch-scoped tally beside the per-window verdict, so a close
    // that SURVIVED states what the rung did rather than saying nothing at all.
    if (tailMeasurable) {
        ++tailMeasuredWindows_;
    }
    if (tailOverBudget) {
        ++tailOverBudgetWindows_;
    }
    if (!tailMeasurable) {
        ++tailNotRunWindows_;
    } else if (tailOverBudget && chargedMean >= kKillTailMeanUs) {
        ++tailKillWindows_;
        if (++tailFailWindows_ >= kKillWindows) {
            rung_ = Rung::Killed;
        }
    } else {
        tailFailWindows_ = 0;
    }

    lastWindow_.medianUs = median;
    lastWindow_.medianLoUs = medianLo;
    lastWindow_.p99Us = p99;
    lastWindow_.p99LoUs = p99Lo;
    lastWindow_.maxUs = windowMaxUs_;
    lastWindow_.chargedSamples = charged;
    ++lastWindow_.windows;
    lastWindow_.overKillWindows = hardFailWindows_;
    lastWindow_.overKillTailWindows = tailFailWindows_;
    lastWindow_.tailMeasured = tailMeasurable;
    lastWindow_.tailMeasuredWindows = tailMeasuredWindows_;
    lastWindow_.tailOverBudgetWindows = tailOverBudgetWindows_;
    lastWindow_.tailKillWindows = tailKillWindows_;
    lastWindow_.tailNotRunWindows = tailNotRunWindows_;
    lastWindow_.chargedMeanUs = chargedMean;
    lastWindow_.tailOverBudget = tailOverBudget;
    windowMaxUs_ = 0;
    lastWindow_.unchargedMeanUs =
        unchargedCount_ ? static_cast<uint32_t>(unchargedSumUs_ / unchargedCount_) : 0u;
    lastWindow_.unchargedMaxUs = unchargedWindowMaxUs_;
    unchargedSumUs_ = 0;
    unchargedCount_ = 0;
    unchargedWindowMaxUs_ = 0;

    if (rung_ != Rung::Killed) {
        if (p99 > kP99BudgetUs) {
            ++overBudgetWindows_;
            if (rung_ == Rung::Nominal) {
                // Rung 1: sample the oracle less often. Never to zero -- ruling Q-B requires the
                // stream, and a ladder that can delete a required stream is a budget gate satisfiable
                // by removing the product.
                oracleDivisor_ = 4;
                rung_ = Rung::OracleCadenceReduced;
            } else if (rung_ == Rung::OracleCadenceReduced) {
                // Rung 2: lengthen the keyframe interval, bounded so seek stays inside SC5.
                const uint32_t next = keyframeIntervalUs_ * 2u;
                keyframeIntervalUs_ = next > kSeekBudgetKeyframeMaxUs ? kSeekBudgetKeyframeMaxUs : next;
                rung_ = Rung::KeyframeIntervalRaised;
            }
            // There is deliberately no rung 3. The next thing to shed would be a required stream.
        } else {
            overBudgetWindows_ = 0;
        }
    }

    newRung = rung_;
    return rung_ != before;
}

}  // namespace capture
}  // namespace replay
