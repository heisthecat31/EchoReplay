#include "bootstrap/tickcost.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

#include "bootstrap/log.h"
#include "bootstrap/pageprobe.h"

namespace replay {
namespace tickcost {
namespace {

struct State {
    std::atomic<int64_t>  ticks[kPhaseCount];
    std::atomic<uint32_t> frames;
    std::atomic<uint32_t> players;
    std::atomic<int64_t>  freq;
};

// Namespace scope rather than a function-local static: the first Scope is constructed on the game's
// frame thread, and a magic static would take a guard lock there.
State g_state;

int64_t Frequency() {
    int64_t f = g_state.freq.load(std::memory_order_relaxed);
    if (f == 0) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart;
        g_state.freq.store(f, std::memory_order_relaxed);
    }
    return f;
}

double MeanUs(int64_t ticks, int64_t freq, uint32_t frames) {
    if (ticks <= 0 || freq <= 0 || frames == 0) {
        return 0.0;
    }
    return (double(ticks) * 1000000.0) / (double(freq) * double(frames));
}

}  // namespace

void Add(Phase p, int64_t ticks) {
    if (p < kPhaseCount && ticks > 0) {
        g_state.ticks[p].fetch_add(ticks, std::memory_order_relaxed);
    }
}

Scope::Scope(Phase p) : start_(0), phase_(p) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    start_ = t.QuadPart;
}

void Scope::Close() {
    if (start_ == 0) {
        return;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    Add(phase_, t.QuadPart - start_);
    start_ = 0;
}

Scope::~Scope() { Close(); }

namespace {

// The previous frame's charge per phase, and the running totals it was differenced against. Game
// thread only -- NoteFrame is the tick's own first statement -- so plain values, no atomics.
int64_t g_frameMark[kPhaseCount] = {0};
int64_t g_frameCost[kPhaseCount] = {0};

// Log-ish buckets over the wall delta in milliseconds: 0.5 ms up to 16 ms, then doubling to
// 2 seconds. 40 buckets covers 240 fps and a one-second stall in the same table, and the bucket a
// frame lands in is a shift, not a search.
constexpr size_t kDtBuckets = 40;
constexpr double kDtBucketMs = 0.5;
uint32_t g_dtHist[kDtBuckets] = {0};
uint32_t g_dtFrames = 0;
double g_dtMax = 0.0;

size_t DtBucket(double dtMs) {
    if (dtMs <= 0.0) {
        return 0;
    }
    const double ratio = dtMs / kDtBucketMs;
    size_t i = 0;
    double edge = 1.0;
    while (i + 1 < kDtBuckets && ratio > edge) {
        edge = i < 31 ? edge * 1.25 : edge * 2.0;
        ++i;
    }
    return i;
}

double BucketUpperMs(size_t i) {
    double edge = 1.0;
    for (size_t k = 0; k < i; ++k) {
        edge = k < 31 ? edge * 1.25 : edge * 2.0;
    }
    return edge * kDtBucketMs;
}

double Percentile(uint32_t want) {
    uint32_t seen = 0;
    for (size_t i = 0; i < kDtBuckets; ++i) {
        seen += g_dtHist[i];
        if (seen >= want) {
            return BucketUpperMs(i);
        }
    }
    return g_dtMax;
}

// The hitch line's rate limit, counted in the tick's own milliseconds rather than in frames: a
// budget in frames would be ten times tighter at 240 fps than at 24.
double   g_hitchSecondMs = 0.0;
double   g_hitchMinuteMs = 0.0;
uint32_t g_hitchThisSecond = 0;
uint32_t g_hitchThisMinute = 0;
uint32_t g_hitchSuppressed = 0;

}  // namespace

void NoteFrame(uint32_t recordedPlayers) {
    g_state.frames.fetch_add(1, std::memory_order_relaxed);
    g_state.players.store(recordedPlayers, std::memory_order_relaxed);
    for (uint8_t i = 0; i < kPhaseCount; ++i) {
        const int64_t now = g_state.ticks[i].load(std::memory_order_relaxed);
        g_frameCost[i] = now - g_frameMark[i];
        g_frameMark[i] = now;
    }
}

void NoteFrameDt(double dtMs) {
    ++g_dtFrames;
    ++g_dtHist[DtBucket(dtMs)];
    if (dtMs > g_dtMax) {
        g_dtMax = dtMs;
    }
}

DtSummary TakeDtSummary() {
    DtSummary s;
    s.frames = g_dtFrames;
    if (g_dtFrames != 0) {
        s.p50 = Percentile((g_dtFrames * 50 + 99) / 100);
        s.p95 = Percentile((g_dtFrames * 95 + 99) / 100);
        s.p99 = Percentile((g_dtFrames * 99 + 99) / 100);
        s.max = g_dtMax;
    }
    for (size_t i = 0; i < kDtBuckets; ++i) {
        g_dtHist[i] = 0;
    }
    g_dtFrames = 0;
    g_dtMax = 0.0;
    return s;
}

bool HitchDue(double dtMs, uint32_t clampsThisSecond, uint32_t* suppressed) {
    g_hitchSecondMs += dtMs;
    g_hitchMinuteMs += dtMs;
    if (g_hitchSecondMs >= 1000.0) {
        g_hitchSecondMs = 0.0;
        g_hitchThisSecond = 0;
    }
    if (g_hitchMinuteMs >= 60000.0) {
        g_hitchMinuteMs = 0.0;
        g_hitchThisMinute = 0;
    }
    if (dtMs <= kHitchDtMs && clampsThisSecond <= kHitchClamps) {
        return false;
    }
    if (g_hitchThisSecond >= 8 || g_hitchThisMinute >= 40) {
        ++g_hitchSuppressed;
        return false;
    }
    ++g_hitchThisSecond;
    ++g_hitchThisMinute;
    if (suppressed != nullptr) {
        *suppressed = g_hitchSuppressed;
    }
    g_hitchSuppressed = 0;
    return true;
}

bool FormatFrameCost(char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    const int64_t freq = Frequency();
    double us[kPhaseCount];
    for (uint8_t i = 0; i < kPhaseCount; ++i) {
        us[i] = MeanUs(g_frameCost[i], freq, 1);
    }
    const int n = std::snprintf(
        out, cap,
        "tick=%.0f sweep=%.0f ui=%.0f audio=%.0f/%.0f/%.0f apply=%.0f jump=%.0f window=%.0f "
        "prov=%.0f drive=%.0f",
        us[kRootTick], us[kRootSweep], us[kRootUi], us[kAudioRate], us[kAudioVoices], us[kAudioPins],
        us[kApply], us[kJump], us[kWindow], us[kProvision], us[kDrive]);
    return n > 0;
}

uint64_t PhaseUsForTest(Phase p) {
    if (p >= kPhaseCount) {
        return 0;
    }
    const int64_t freq = Frequency();
    const int64_t ticks = g_state.ticks[p].load(std::memory_order_relaxed);
    if (ticks <= 0 || freq <= 0) {
        return 0;
    }
    return uint64_t((double(ticks) * 1000000.0) / double(freq));
}

void ResetForTest() {
    for (uint8_t i = 0; i < kPhaseCount; ++i) {
        g_state.ticks[i].store(0, std::memory_order_relaxed);
    }
    g_state.frames.store(0, std::memory_order_relaxed);
    g_state.players.store(0, std::memory_order_relaxed);
}

bool FormatAndReset(char* out, size_t cap) {
    const uint32_t frames = g_state.frames.exchange(0, std::memory_order_relaxed);
    int64_t raw[kPhaseCount];
    for (uint8_t i = 0; i < kPhaseCount; ++i) {
        raw[i] = g_state.ticks[i].exchange(0, std::memory_order_relaxed);
        // The per-frame mark is differenced against these same counters, so it has to be zeroed
        // with them. Left standing, the first frame of every second differenced a near-zero total
        // against last second's and charged that frame nothing at all -- on the one frame after a
        // second boundary, which is exactly where a hitch report lands.
        g_frameMark[i] = 0;
    }
    const uint64_t logTicks = log::CostTicksTake();
    if (frames == 0 || out == nullptr || cap == 0) {
        return false;
    }

    const int64_t freq = Frequency();
    double us[kPhaseCount];
    for (uint8_t i = 0; i < kPhaseCount; ++i) {
        us[i] = MeanUs(raw[i], freq, frames);
    }
    // The stage sweep runs several times per rendered frame and the UI draw once; dividing all three
    // roots by the same frame count is what makes `total` the plugin's cost per frame rather than per
    // entry.
    // ★ AND THE AUDIO MODULE IS A FOURTH ROOT. The engine enters it on its own tick, outside the
    // playback tick entirely, so its cost was not in `total` and had no field of its own -- which is
    // why the 22-second choppy burst at 0.10x could be correlated with the audio module's degraded
    // re-arm loop and never charged to it.
    const double total = us[kRootTick] + us[kRootSweep] + us[kRootUi] + us[kRootAudio];
    const double logUs = MeanUs(int64_t(logTicks), freq, frames);
    const double named =
        us[kApply] + us[kHud] + us[kStats] + us[kGoals] + us[kStuns] + us[kDress] + us[kJump] +
        us[kEvents] + us[kWindow] + us[kNetVar] + us[kSweepTail] + us[kSeat] + us[kProvision] +
        us[kDrive] + us[kRootUi] + us[kAudioRate] + us[kAudioVoices] + us[kAudioPins] + logUs;
    const double other = total > named ? total - named : 0.0;
    // Page-probe faults since this line was last formatted, which the pacing log does once a
    // second. A nonzero count on steady frames means some caller probes an unmapped address every
    // frame and pays a kernel round trip for it -- a performance defect in its own right.
    static uint64_t s_faultsLast = 0;
    const uint64_t faultsNow = ::replay::ProbeFaultCount();
    const uint64_t faults = faultsNow - s_faultsLast;
    s_faultsLast = faultsNow;

    const int n = std::snprintf(out, cap,
                                "cost: total=%.0f us/frame (apply=%.0f, hud=%.0f, "
                                "stats=%.0f, goals=%.0f, stuns=%.0f, dress=%.0f, jump=%.0f, "
                                "events=%.0f, window=%.0f, netvar=%.0f, sweep=%.0f, "
                                "hudsite=%.0f, hudholds=%.0f, seat=%.0f, "
                                "prov=%.0f, drive=%.0f, ui=%.0f, audio=%.0f/%.0f/%.0f, "
                                "log=%.0f, other=%.0f) frames=%u players=%u pfault=%llu/s",
                                total, us[kApply], us[kHud], us[kStats],
                                us[kGoals], us[kStuns], us[kDress], us[kJump], us[kEvents],
                                us[kWindow], us[kNetVar], us[kSweepTail], us[kHudSite],
                                us[kHudHolds], us[kSeat], us[kProvision],
                                us[kDrive],
                                us[kRootUi], us[kAudioRate], us[kAudioVoices], us[kAudioPins],
                                logUs, other, unsigned(frames),
                                unsigned(g_state.players.load(std::memory_order_relaxed)),
                                (unsigned long long)faults);
    return n > 0;
}

}  // namespace tickcost
}  // namespace replay
