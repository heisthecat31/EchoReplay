// SMOOTH-1: motion smoothing for slow-motion replay (DESIGN-SMOOTHING.md S8.1, S8.2).
//
// Synthetic cases pin the filter's properties; the real-capture case measures the owner's symptom (the
// frame-to-frame velocity step at 0.1x) on recorded sessions and runs when REPLAY_SMOOTH_CAPTURES names
// one or more .echodemo files (';'-separated).

#include "doctest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "foundation/demofile/demofile.h"
#include "playback/core/sample.h"
#include "playback/core/smooth.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"
#include "smoothing_fixture.h"

using replay::BodyState;
using replay::PlayerState;
using replay::playback::BodyKnot;
using replay::playback::BodyTrack;
using replay::playback::BreakRule;
using replay::playback::KnotKind;
using replay::playback::PlayerKnot;
using replay::playback::PlayerTrack;
using replay::playback::SeriesCursor;
using replay::playback::SmoothMode;
using replay::playback::SmoothParams;
using replay::playback::SmoothTally;
using replay::playback::Window;
using replay::playback::WindowCaps;

namespace sm = replay::playback;

namespace {

using namespace smoothing_fixture;

constexpr sm::TrackEdges kWhole{true, true, 0, 0};

void FillSmoothWindowWhole(Window& w, size_t n, double noise, uint64_t seed) {
    FillSmoothWindow(w, n, noise, seed);
    w.SetFileEdges(true, true);
}

sm::SmoothFrame Frame(float weight, SmoothTally* tally) {
    sm::SmoothFrame f;
    f.weight = weight;
    f.tally = tally;
    return f;
}

bool SampleBodySmooth(Window& w, size_t k, int64_t t, BodyState& out, SmoothTally* tally = nullptr) {
    memset(&out, 0, sizeof out);
    BodyTrack& tr = w.BodyAt(k);
    return sm::SampleBodySmoothAt(w, tr, tr.cursor, t, out, false, true, tally);
}

bool SamplePlayerSmooth(Window& w, size_t k, int64_t t, PlayerState& out, SmoothTally* tally = nullptr) {
    memset(&out, 0, sizeof out);
    PlayerTrack& tr = w.PlayerAt(k);
    return sm::SamplePlayerSmoothAt(w, tr, tr.cursor, t, out, false, tally);
}

}  // namespace

TEST_CASE("smoothing T9: mode table, parse and strength clamp") {
    const double rates[] = {-4, -1, -0.999, -0.5, 0, 0.1, 0.998, 1, 4};
    const bool slowmo[] = {false, false, false, true, true, true, true, false, false};
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        CHECK(sm::SmoothWanted(SmoothMode::On, rates[i]));
        CHECK_FALSE(sm::SmoothWanted(SmoothMode::Off, rates[i]));
        CHECK(sm::SmoothWanted(SmoothMode::SlowMo, rates[i]) == slowmo[i]);
    }
    SmoothMode m = SmoothMode::On;
    CHECK(sm::ParseSmoothMode("off", &m));
    CHECK(m == SmoothMode::Off);
    CHECK(sm::ParseSmoothMode("slowmo", &m));
    CHECK(m == SmoothMode::SlowMo);
    CHECK(sm::ParseSmoothMode("on", &m));
    CHECK(m == SmoothMode::On);
    CHECK_FALSE(sm::ParseSmoothMode("", &m));
    CHECK_FALSE(sm::ParseSmoothMode("ON ", &m));
    CHECK_FALSE(sm::ParseSmoothMode("slow", &m));
    CHECK_FALSE(sm::ParseSmoothMode(nullptr, &m));
    CHECK(sm::ParamsForStrength(0).strength == 1);
    CHECK(sm::ParamsForStrength(9).strength == 3);
    CHECK(sm::ParamsForStrength(2).sigma_s == 0.030f);
    CHECK(sm::ParamsForStrength(1).radius_s == doctest::Approx(0.045f));
    CHECK(sm::ParamsForStrength(3).radius_s == doctest::Approx(0.125f));
}

TEST_CASE("smoothing T2: quadratic motion is reproduced exactly, edges included") {
    const size_t n = 60;
    const std::vector<uint64_t> t = JitteredTimes(n, 3000.0, 5);
    std::vector<BodyState> f(n);
    const double a[3] = {1.0, -2.0, 0.5}, b[3] = {3.0, 0.5, -1.0}, c[3] = {-4.0, 2.5, 1.5};
    for (size_t i = 0; i < n; ++i) {
        const double ts = (double)(t[i] - kT0) * 1e-6;
        memset(&f[i], 0, sizeof f[i]);
        for (int k = 0; k < 3; ++k) {
            f[i].com[k] = (float)(a[k] + b[k] * ts + c[k] * ts * ts);
            f[i].vel[k] = (float)(b[k] + 2.0 * c[k] * ts);
        }
        f[i].ori[3] = 1.0f;
    }
    for (uint8_t strength = 1; strength <= 3; ++strength) {
        const SmoothParams p = sm::ParamsForStrength(strength);
        double worstPos = 0, worstVel = 0;
        for (size_t j = 0; j < n; ++j) {
            BodyKnot k;
            const KnotKind kind = sm::FitBodyKnot(f.data(), t.data(), n, j, kWhole, p, k);
            CHECK((kind == KnotKind::Fit2 || kind == KnotKind::Pinned));
            const double ts = (double)(t[j] - kT0) * 1e-6;
            for (int d = 0; d < 3; ++d) {
                worstPos = std::max(worstPos, fabs(k.com[d] - (a[d] + b[d] * ts + c[d] * ts * ts)));
                worstVel = std::max(worstVel, fabs(k.vel[d] - (b[d] + 2.0 * c[d] * ts)));
            }
        }
        MESSAGE("strength " << int(strength) << ": worst knot error " << worstPos << " m, "
                            << worstVel << " m/s");
        CHECK(worstPos < 1e-4);
        CHECK(worstVel < 1e-3 * 5);   // float storage of the samples: ~5e-7 m over ~30 ms support
    }
}

TEST_CASE("smoothing T3: output is a pure function of the playhead") {
    Window w;
    FillSmoothWindowWhole(w, 301, 0.004, 21);
    w.SetSmoothing(sm::ParamsForStrength(2));
    const uint64_t* bt = w.BodyTimes(w.BodyAt(0));
    const int64_t lo = (int64_t)bt[0], hi = (int64_t)bt[w.BodyAt(0).count - 1];
    const int kN = 2000;
    std::vector<int64_t> probes(kN);
    for (int k = 0; k < kN; ++k) probes[k] = lo + (hi - lo) * k / kN;
    std::vector<BodyState> refB(kN);
    std::vector<PlayerState> refP(kN);
    for (int k = 0; k < kN; ++k) {
        REQUIRE(SampleBodySmooth(w, 0, probes[k], refB[k]));
        REQUIRE(SamplePlayerSmooth(w, 0, probes[k], refP[k]));
    }
    size_t mismatches = 0;
    auto compare = [&](int k) {
        BodyState b;
        PlayerState p;
        SampleBodySmooth(w, 0, probes[k], b);
        SamplePlayerSmooth(w, 0, probes[k], p);
        if (memcmp(&b, &refB[k], sizeof b) != 0 || memcmp(&p, &refP[k], sizeof p) != 0) ++mismatches;
    };
    for (int k = kN - 1; k >= 0; --k) compare(k);   // reverse play
    Lcg r(3);
    for (int k = 0; k < kN; ++k) {                    // random seeks, memo dropped between some
        if (k % 7 == 0) {
            w.SetSmoothing(sm::ParamsForStrength(3));
            w.SetSmoothing(sm::ParamsForStrength(2));
        }
        compare((int)(r.Next() % kN));
    }
    CHECK(mismatches == 0);

    // `reversed` flips the velocities and nothing else.
    BodyTrack& tr = w.BodyAt(0);
    BodyState fwd, rev;
    memset(&fwd, 0, sizeof fwd);
    memset(&rev, 0, sizeof rev);
    sm::SampleBodySmoothAt(w, tr, tr.cursor, probes[777], fwd, false, true, nullptr);
    sm::SampleBodySmoothAt(w, tr, tr.cursor, probes[777], rev, true, true, nullptr);
    for (int c = 0; c < 3; ++c) {
        CHECK(rev.vel[c] == -fwd.vel[c]);
        CHECK(rev.angVel[c] == -fwd.angVel[c]);
        CHECK(rev.com[c] == fwd.com[c]);
    }
}

TEST_CASE("smoothing T5: no knot is fitted across a discontinuity") {
    const size_t n = 160;
    std::vector<uint64_t> t = JitteredTimes(n, 2000.0, 8);
    for (size_t i = 120; i < n; ++i) t[i] += 100000;   // a gap after sample 119
    std::vector<BodyState> f(n);
    double x = 0.0, vx = 6.0;
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) x += vx * (double)(t[i] - t[i - 1]) * 1e-6;
        if (i == 30) vx = -vx;                          // bounce: velocity reversal at sample 30
        memset(&f[i], 0, sizeof f[i]);
        f[i].com[0] = (float)x;
        f[i].com[1] = (float)(0.5 * x);
        f[i].vel[0] = (float)vx;
        f[i].vel[1] = (float)(0.5 * vx);
        f[i].ori[3] = 1.0f;
        f[i].owner_slot = i >= 60 ? 3u : UINT32_MAX;   // caught at sample 60
    }
    for (size_t i = 90; i < n; ++i) f[i].com[2] = 40.0f;   // teleport between 89 and 90

    CHECK(sm::BodyBreakRule(f.data(), t.data(), n, 29) != BreakRule::None);
    CHECK(sm::BodyBreakRule(f.data(), t.data(), n, 59) == BreakRule::Possession);
    CHECK(sm::BodyBreakRule(f.data(), t.data(), n, 89) == BreakRule::Teleport);
    CHECK(sm::BodyBreakRule(f.data(), t.data(), n, 119) == BreakRule::Gap);
    CHECK(sm::BodyBreakRule(f.data(), t.data(), n, 45) == BreakRule::None);

    const SmoothParams p = sm::ParamsForStrength(3);
    size_t spanned = 0, pinnedNotRaw = 0;
    double worstFlight = 0;
    for (size_t j = 0; j < n; ++j) {
        size_t lo = 0, hi = 0;
        sm::BodyKnotSupport(f.data(), t.data(), n, j, p, &lo, &hi);
        for (size_t i = lo; i < hi; ++i) {
            if (sm::BodyBreakAfter(f.data(), t.data(), n, i)) ++spanned;
        }
        BodyKnot k;
        const KnotKind kind = sm::FitBodyKnot(f.data(), t.data(), n, j, kWhole, p, k);
        if (kind == KnotKind::Pinned && memcmp(k.com, f[j].com, sizeof k.com) != 0) ++pinnedNotRaw;
        if (kind == KnotKind::Fit2) {
            for (int c = 0; c < 3; ++c) worstFlight = std::max(worstFlight, (double)fabs(k.com[c] - f[j].com[c]));
        }
    }
    CHECK(spanned == 0);
    CHECK(pinnedNotRaw == 0);
    CHECK(worstFlight < 1e-4);   // straight flight between breaks is reproduced

    // Player: a grab edge and a tracking glitch.
    std::vector<PlayerState> pf(n);
    Lcg r(4);
    for (size_t i = 0; i < n; ++i) {
        pf[i] = PlayerAt((double)(t[i] - kT0) * 1e-6, 0.0, r);
        if (i >= 40) pf[i].holdL = 5;
        if (i >= 70) pf[i].rHandOff[0] += 0.8f;
    }
    CHECK(sm::PlayerBreakRule(pf.data(), t.data(), n, 39) == BreakRule::Grab);
    CHECK(sm::PlayerBreakRule(pf.data(), t.data(), n, 69) == BreakRule::HandJump);
    CHECK(sm::PlayerBreakRule(pf.data(), t.data(), n, 50) == BreakRule::None);
    PlayerKnot pk;
    CHECK(sm::FitPlayerKnot(pf.data(), t.data(), n, 40, kWhole, p, pk) == KnotKind::Pinned);
    CHECK(memcmp(pk.vec, pf[40].lHandOff, 3 * sizeof(float)) == 0);
    CHECK(sm::FitPlayerKnot(pf.data(), t.data(), n, 69, kWhole, p, pk) == KnotKind::Pinned);
    CHECK(memcmp(pk.vec + 3, pf[69].rHandOff, 3 * sizeof(float)) == 0);
}

TEST_CASE("smoothing T6: centred -- no lag, no visible amplitude loss") {
    Window w;
    const size_t n = 401;
    const std::vector<uint64_t> t = JitteredTimes(n, 3000.0, 31);
    Lcg r(77);
    const double freq = 2.0, amp = 0.05, wv = 2.0 * 3.14159265358979 * freq;
    for (size_t i = 0; i < n; ++i) {
        const double ts = (double)(t[i] - kT0) * 1e-6;
        PlayerState p = PlayerAt(ts, 0.0, r);
        p.lHandOff[0] = (float)(amp * sin(wv * ts) + r.Uniform(-0.005, 0.005));
        w.AddPlayerSample(0, t[i], p);
    }
    w.SetCoverage(kT0, t[n - 1] + 1, kT0, t[n - 1] + 1);
    w.Finalize();
    w.SetFileEdges(true, true);
    w.SetSmoothing(sm::ParamsForStrength(2));

    // 1 ms steps over the middle of the track.
    const int64_t from = (int64_t)t[40], to = (int64_t)t[n - 40];
    std::vector<double> vs, pos;
    PlayerState prev;
    REQUIRE(SamplePlayerSmooth(w, 0, from - kMsUs, prev));
    for (int64_t ts = from; ts < to; ts += kMsUs) {
        PlayerState cur;
        REQUIRE(SamplePlayerSmooth(w, 0, ts, cur));
        vs.push_back((cur.lHandOff[0] - prev.lHandOff[0]) / 1e-3);
        pos.push_back(cur.lHandOff[0]);
        prev = cur;
    }
    // Phase of the smoothed velocity against the clean one: v_s ~ A cos(w (t - d)), so projecting onto
    // cos and sin gives tan(w d). A 1 ms grid search cannot resolve this; the projection can.
    double pc = 0, ps = 0;
    for (size_t i = 0; i < vs.size(); ++i) {
        const double tm = ((double)(from + (int64_t)i * kMsUs - (int64_t)kT0) - 500.0) * 1e-6;
        pc += vs[i] * cos(wv * tm);
        ps += vs[i] * sin(wv * tm);
    }
    const double lagMs = atan2(ps, pc) / wv * 1e3;
    // Amplitude: least-squares projection of the smoothed position onto the clean sinusoid.
    double num = 0, den = 0;
    for (size_t i = 0; i < pos.size(); ++i) {
        const double tm = ((double)(from + (int64_t)i * kMsUs - (int64_t)kT0)) * 1e-6;
        const double clean = amp * sin(wv * tm);
        num += pos[i] * clean;
        den += clean * clean;
    }
    MESSAGE("lag " << lagMs << " ms, amplitude ratio " << num / den);
    CHECK(fabs(lagMs) < 1.0);
    CHECK(num / den > 0.97);
}

TEST_CASE("smoothing T7: C1 at every knot; the 1:1 path shows the step the owner sees") {
    Window w;
    FillSmoothWindowWhole(w, 200, 0.003, 41);
    w.SetSmoothing(sm::ParamsForStrength(2));
    PlayerTrack& tr = w.PlayerAt(0);
    const uint64_t* t = w.PlayerTimes(tr);
    const double h = 200e-6;
    auto hand = [&](int64_t ts, bool smooth, float out[3]) {
        PlayerState p;
        memset(&p, 0, sizeof p);
        if (smooth) {
            sm::SamplePlayerSmoothAt(w, tr, tr.cursor, ts, p, false, nullptr);
        } else {
            sm::SamplePlayerAt(w.PlayerFrames(tr), t, tr.count, tr.cursor, ts, p, false);
        }
        memcpy(out, p.lHandOff, 3 * sizeof(float));
    };
    auto worstStep = [&](bool smooth) {
        double worst = 0;
        for (size_t j = 20; j + 20 < tr.count; ++j) {
            float a[3], b[3], c[3];
            const int64_t tj = (int64_t)t[j];
            hand(tj - (int64_t)(h * 1e6), smooth, a);
            hand(tj, smooth, b);
            hand(tj + (int64_t)(h * 1e6), smooth, c);
            double step = 0, speed = 0;
            for (int k = 0; k < 3; ++k) {
                const double vl = (b[k] - a[k]) / h, vr = (c[k] - b[k]) / h;
                step += (vr - vl) * (vr - vl);
                speed += vl * vl;
            }
            worst = std::max(worst, sqrt(step) / std::max(sqrt(speed), 0.05));
        }
        return worst;
    };
    const double smoothStep = worstStep(true), rawStep = worstStep(false);
    MESSAGE("worst relative velocity step at a knot: smooth " << smoothStep << ", raw " << rawStep);
    CHECK(rawStep > 0.10);    // control: the 1:1 path's corner is visible to this probe
    // A 200 us finite difference carries O(h * accel) and float rounding of ~1e-7 m / 2e-4 s.
    CHECK(smoothStep < 0.03);
}

TEST_CASE("smoothing T8: degenerate inputs never produce NaN") {
    const SmoothParams p = sm::ParamsForStrength(2);
    uint64_t t1[1] = {kT0};
    BodyState one;
    memset(&one, 0, sizeof one);
    one.ori[3] = 1.0f;
    BodyKnot k;
    CHECK(sm::FitBodyKnot(&one, t1, 1, 0, kWhole, p, k) == KnotKind::Raw);
    CHECK(memcmp(k.com, one.com, sizeof k.com) == 0);

    const size_t n = 30;
    std::vector<uint64_t> t = JitteredTimes(n, 0.0, 1);
    std::vector<BodyState> same(n, one);
    for (auto& s : same) { s.com[0] = 2.5f; s.com[1] = -1.0f; }
    for (size_t j = 0; j < n; ++j) {
        sm::FitBodyKnot(same.data(), t.data(), n, j, kWhole, p, k);
        CHECK(k.com[0] == 2.5f);
        CHECK(k.com[1] == -1.0f);
    }

    std::vector<BodyState> nan(n, one);
    for (size_t i = 0; i < n; ++i) nan[i].com[0] = (float)i * 0.1f;
    nan[15].com[1] = NAN;
    std::vector<uint64_t> dup = t;
    dup[20] = dup[19];                                 // a dt == 0 pair
    for (size_t j = 0; j < n; ++j) {
        sm::FitBodyKnot(nan.data(), dup.data(), n, j, kWhole, p, k);
        const bool finite = isfinite(k.com[0]) && isfinite(k.com[1]) && isfinite(k.com[2]);
        CHECK((finite || j == 15));
    }
    CHECK(sm::BodyBreakRule(nan.data(), dup.data(), n, 19) == BreakRule::Gap);

    // A two-sample segment between breaks: the knot keeps its raw value and takes the chord's slope.
    std::vector<BodyState> seg(n, one);
    for (size_t i = 0; i < n; ++i) seg[i].com[0] = (float)i * 0.1f;
    for (size_t i = 11; i < n; ++i) seg[i].owner_slot = 1;
    for (size_t i = 13; i < n; ++i) seg[i].owner_slot = 2;
    CHECK(sm::FitBodyKnot(seg.data(), t.data(), n, 11, kWhole, p, k) == KnotKind::Pinned);
    CHECK(k.com[0] == seg[11].com[0]);
    const double chord = (seg[12].com[0] - seg[11].com[0]) / ((double)(t[12] - t[11]) * 1e-6);
    CHECK(k.vel[0] == doctest::Approx(chord).epsilon(1e-4));
}

// ---- real captures ----------------------------------------------------------------------------------

namespace {

// The owner's symptom, measured where it happens: the change of velocity across a recorded sample.
// At every knot j whose two brackets are both drawn smooth, v- and v+ are 1 ms chords either side of
// t_j (0.1x at ~90 fps advances ~1.1 ms of source per frame), and |v+ - v-| is recorded for both paths.
struct KnotSteps {
    std::vector<double> raw, smooth, fidelity, along;
    static double Pct(std::vector<double> v, double q) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[(size_t)(q * (v.size() - 1))];
    }
    static double Rms(const std::vector<double>& v) {
        double s = 0;
        for (double x : v) s += x * x;
        return v.empty() ? 0 : sqrt(s / v.size());
    }
    double MedianRatio() const { return Pct(raw, 0.5) > 0 ? Pct(smooth, 0.5) / Pct(raw, 0.5) : 0; }
    double RmsRatio() const { return Rms(raw) > 0 ? Rms(smooth) / Rms(raw) : 0; }
    void Report(const char* name, const char* unit, double scale) const {
        printf("  %-9s step p50 %.3f -> %.3f (x%.2f)  p90 %.3f -> %.3f  rms %.3f -> %.3f (x%.2f) %s"
               "  | knot off the raw path p50 %.1f p99 %.1f mm, off the raw sample p99 %.1f mm (%zu knots)\n",
               name, Pct(raw, 0.5) * scale, Pct(smooth, 0.5) * scale, MedianRatio(), Pct(raw, 0.9) * scale,
               Pct(smooth, 0.9) * scale, Rms(raw) * scale, Rms(smooth) * scale, RmsRatio(), unit,
               Pct(fidelity, 0.5) * 1e3, Pct(fidelity, 0.99) * 1e3, Pct(along, 0.99) * 1e3, raw.size());
    }
};

double Dist3(const float a[3], const float b[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

// Distance from a knot to the raw polyline around its sample: the shape error. The distance to the raw
// sample itself also carries the along-path shift that undoes stamp jitter, which is the point.
template <typename Get>
double OffPath(const float k[3], size_t j, size_t n, Get sample) {
    double best = 1e30;
    const size_t lo = j >= 2 ? j - 2 : 0, hi = j + 2 < n ? j + 2 : n - 1;
    for (size_t i = lo; i < hi; ++i) {
        const float* a = sample(i);
        const float* b = sample(i + 1);
        double ab[3], ak[3], len2 = 0, dot = 0;
        for (int c = 0; c < 3; ++c) {
            ab[c] = (double)b[c] - a[c];
            ak[c] = (double)k[c] - a[c];
            len2 += ab[c] * ab[c];
            dot += ab[c] * ak[c];
        }
        const double u = len2 > 0 ? std::min(1.0, std::max(0.0, dot / len2)) : 0.0;
        double d2 = 0;
        for (int c = 0; c < 3; ++c) d2 += (ak[c] - u * ab[c]) * (ak[c] - u * ab[c]);
        best = std::min(best, sqrt(d2));
    }
    return best;
}

double VelocityStep(const float m[3], const float c[3], const float p[3]) {
    double s = 0;
    for (int k = 0; k < 3; ++k) {
        const double d = ((double)p[k] - c[k]) / 1e-3 - ((double)c[k] - m[k]) / 1e-3;
        s += d * d;
    }
    return sqrt(s);
}

// The head's forward axis, so its step is the angular-rate step in rad/s.
void Forward(const float q[4], float out[3]) {
    out[0] = 2 * (q[0] * q[2] + q[3] * q[1]);
    out[1] = 2 * (q[1] * q[2] - q[3] * q[0]);
    out[2] = 1 - 2 * (q[0] * q[0] + q[1] * q[1]);
}

struct CaptureReport {
    KnotSteps com, lh, rh, hmd;
    size_t breaks[(size_t)BreakRule::kCount] = {};
    size_t bodyPairs = 0, playerPairs = 0, bodyRepeats = 0, playerRepeats = 0;
    double dtSum = 0, dtSq = 0;
    size_t dtN = 0;
    size_t kinds[5] = {};
    SmoothTally tally;
    size_t seamCompared = 0, seamMismatch = 0, seamRawDiffers = 0;
    double fitNsBody = 0, fitNsPlayer = 0;
    size_t fitBodyN = 0, fitPlayerN = 0;
    size_t hmdRepeats = 0, edgeExamples = 0;
};

// The window the module holds while the playhead is in `center`'s span.
bool Build(replay::DemoReader& reader, sm::TrackScan& scan, sm::NetVarPrimer& primer, size_t center,
           Window& w) {
    sm::BuildStats st;
    if (!sm::BuildWindowRange(reader, center, w, st, &scan.clocks, &primer)) return false;
    w.SetSmoothing(sm::ParamsForStrength(2));
    return true;
}

// Normal flow: the playhead sweeps the centre chapter at 1 ms steps, as 0.1x does. Every smooth sample
// is counted in the tally, so `edge_raw` is what a playthrough would log.
void MeasureWindow(Window& w, uint64_t from, uint64_t to, CaptureReport& rep) {
    for (size_t b = 0; b < w.BodyCount(); ++b) {
        BodyTrack& tr = w.BodyAt(b);
        const BodyState* f = w.BodyFrames(tr);
        const uint64_t* t = w.BodyTimes(tr);
        const size_t n = tr.count;
        for (size_t i = 0; i + 1 < n; ++i) {
            if (t[i] < from || t[i] >= to) continue;
            ++rep.bodyPairs;
            ++rep.breaks[(size_t)sm::BodyBreakRule(f, t, n, i)];
            if (i > 0 && memcmp(f[i].com, f[i - 1].com, sizeof f[i].com) == 0) ++rep.bodyRepeats;
        }
        for (uint64_t ts = from; ts < to; ts += kMsUs) {
            BodyState s;
            const uint32_t before = rep.tally.edge_raw;
            sm::SampleBodySmoothAt(w, tr, tr.cursor, (int64_t)ts, s, false, true, &rep.tally);
            if (rep.tally.edge_raw != before && rep.edgeExamples < 6) {
                ++rep.edgeExamples;
                const sm::TrackEdges e = sm::EdgesOf(w, t, n);
                SeriesCursor c;
                const size_t i = c.Resolve(t, n, (int64_t)ts);
                size_t lo = 0, hi = 0;
                sm::BodyKnotSupport(f, t, n, i, w.Smoothing(), &lo, &hi);
                printf("  edge_raw body %llx: i=%zu n=%zu lo=%zu hi=%zu t-outer_begin=%lld ms outer_end-t=%lld ms t0-ob=%lld tn-oe=%lld begin_real=%d end_real=%d\n",
                       (unsigned long long)tr.id.value, i, n, lo, hi, (long long)((int64_t)ts - (int64_t)w.OuterBeginUs()) / kMsUs,
                       (long long)((int64_t)w.OuterEndUs() - (int64_t)ts) / kMsUs,
                       (long long)((int64_t)t[0] - (int64_t)w.OuterBeginUs()) / kMsUs,
                       (long long)((int64_t)t[n - 1] - (int64_t)w.OuterEndUs()) / kMsUs, e.begin_real, e.end_real);
            }
        }
        const sm::TrackEdges e = sm::EdgesOf(w, t, n);
        SeriesCursor rc;
        for (size_t j = 1; j + 1 < n; ++j) {
            if (t[j] < from || t[j] >= to) continue;
            BodyKnot k;
            const auto c0 = std::chrono::steady_clock::now();
            const KnotKind kind = sm::FitBodyKnot(f, t, n, j, e, w.Smoothing(), k);
            rep.fitNsBody += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - c0).count();
            ++rep.fitBodyN;
            ++rep.kinds[(size_t)kind];
            if (!k.complete || sm::BodyBreakAfter(f, t, n, j - 1) || sm::BodyBreakAfter(f, t, n, j)) continue;
            if (memcmp(f[j].com, f[j - 1].com, sizeof f[j].com) == 0) continue;   // a repeat is not a pose
            if (kind == KnotKind::Fit2) {
                rep.com.fidelity.push_back(OffPath(k.com, j, n, [&](size_t i) { return f[i].com; }));
                rep.com.along.push_back(Dist3(k.com, f[j].com));
            }
            BodyState r[3], s[3];
            for (int q = 0; q < 3; ++q) {
                const int64_t at = (int64_t)t[j] + (q - 1) * kMsUs;
                memset(&r[q], 0, sizeof r[q]);
                sm::SampleBodyAt(f, t, n, rc, at, r[q], false, true, nullptr);
                sm::SampleBodySmoothAt(w, tr, tr.cursor, at, s[q], false, true, nullptr);
            }
            rep.com.raw.push_back(VelocityStep(r[0].com, r[1].com, r[2].com));
            rep.com.smooth.push_back(VelocityStep(s[0].com, s[1].com, s[2].com));
        }
    }
    for (size_t pi = 0; pi < w.PlayerCount(); ++pi) {
        PlayerTrack& tr = w.PlayerAt(pi);
        const PlayerState* f = w.PlayerFrames(tr);
        const uint64_t* t = w.PlayerTimes(tr);
        const size_t n = tr.count;
        for (size_t i = 0; i + 1 < n; ++i) {
            if (t[i] < from || t[i] >= to) continue;
            ++rep.playerPairs;
            if (i > 0 && memcmp(f[i].hmdOri, f[i - 1].hmdOri, 16) == 0) ++rep.hmdRepeats;
            ++rep.breaks[(size_t)sm::PlayerBreakRule(f, t, n, i)];
            if (i > 0 && memcmp(f[i].lHandOff, f[i - 1].lHandOff, 12) == 0 &&
                memcmp(f[i].hmdOri, f[i - 1].hmdOri, 16) == 0) {
                ++rep.playerRepeats;
            }
            const double dt = (double)(t[i + 1] - t[i]) * 1e-3;
            if (dt > 0 && dt < 66.667) {
                rep.dtSum += dt;
                rep.dtSq += dt * dt;
                ++rep.dtN;
            }
        }
        for (uint64_t ts = from; ts < to; ts += kMsUs) {
            PlayerState s;
            const uint32_t before = rep.tally.edge_raw;
            sm::SamplePlayerSmoothAt(w, tr, tr.cursor, (int64_t)ts, s, false, &rep.tally);
            if (rep.tally.edge_raw != before && rep.edgeExamples < 6) {
                ++rep.edgeExamples;
                const sm::TrackEdges e = sm::EdgesOf(w, t, n);
                SeriesCursor c;
                const size_t i = c.Resolve(t, n, (int64_t)ts);
                printf("  edge_raw player slot %u: i=%zu n=%zu t-outer_begin=%lld ms outer_end-t=%lld ms begin_real=%d end_real=%d files=%d/%d\n",
                       unsigned(tr.user_slot), i, n, (long long)((int64_t)ts - (int64_t)w.OuterBeginUs()) / kMsUs,
                       (long long)((int64_t)w.OuterEndUs() - (int64_t)ts) / kMsUs, e.begin_real, e.end_real,
                       w.BeginsFile(), w.EndsFile());
            }
        }
        const sm::TrackEdges e = sm::EdgesOf(w, t, n);
        SeriesCursor rc;
        for (size_t j = 1; j + 1 < n; ++j) {
            if (t[j] < from || t[j] >= to) continue;
            PlayerKnot k;
            const auto c0 = std::chrono::steady_clock::now();
            const KnotKind kind = sm::FitPlayerKnot(f, t, n, j, e, w.Smoothing(), k);
            rep.fitNsPlayer += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - c0).count();
            ++rep.fitPlayerN;
            ++rep.kinds[(size_t)kind];
            if (!k.complete || sm::PlayerBreakAfter(f, t, n, j - 1) || sm::PlayerBreakAfter(f, t, n, j)) continue;
            if (memcmp(f[j].lHandOff, f[j - 1].lHandOff, 12) == 0) continue;
            if (kind == KnotKind::Fit2) {
                rep.lh.fidelity.push_back(OffPath(k.vec, j, n, [&](size_t i) { return f[i].lHandOff; }));
                rep.rh.fidelity.push_back(OffPath(k.vec + 3, j, n, [&](size_t i) { return f[i].rHandOff; }));
                rep.lh.along.push_back(Dist3(k.vec, f[j].lHandOff));
                rep.rh.along.push_back(Dist3(k.vec + 3, f[j].rHandOff));
            }
            PlayerState r[3], s[3];
            for (int q = 0; q < 3; ++q) {
                const int64_t at = (int64_t)t[j] + (q - 1) * kMsUs;
                memset(&r[q], 0, sizeof r[q]);
                sm::SamplePlayerAt(f, t, n, rc, at, r[q], false);
                sm::SamplePlayerSmoothAt(w, tr, tr.cursor, at, s[q], false, nullptr);
            }
            rep.lh.raw.push_back(VelocityStep(r[0].lHandOff, r[1].lHandOff, r[2].lHandOff));
            rep.lh.smooth.push_back(VelocityStep(s[0].lHandOff, s[1].lHandOff, s[2].lHandOff));
            rep.rh.raw.push_back(VelocityStep(r[0].rHandOff, r[1].rHandOff, r[2].rHandOff));
            rep.rh.smooth.push_back(VelocityStep(s[0].rHandOff, s[1].rHandOff, s[2].rHandOff));
            float fr[3][3], fs[3][3];
            for (int q = 0; q < 3; ++q) {
                Forward(r[q].hmdOri, fr[q]);
                Forward(s[q].hmdOri, fs[q]);
            }
            rep.hmd.raw.push_back(VelocityStep(fr[0], fr[1], fr[2]));
            rep.hmd.smooth.push_back(VelocityStep(fs[0], fs[1], fs[2]));
        }
    }
}

// The seam: the playhead leaves window `a` (centre c) for `b` (centre c+1) at the end of chapter c, and
// comes back across the same instant in reverse. Within a second of it, both windows must draw the same
// pose bit for bit wherever both hold complete knots.
void CompareWindows(Window& a, Window& b, uint64_t from, uint64_t to, CaptureReport& rep) {
    for (size_t i = 0; i < a.BodyCount(); ++i) {
        BodyTrack& ta = a.BodyAt(i);
        for (size_t k = 0; k < b.BodyCount(); ++k) {
            BodyTrack& tb = b.BodyAt(k);
            if (tb.id.value != ta.id.value || tb.id.tag != ta.id.tag || tb.id.fill != ta.id.fill) continue;
            const uint64_t* at = a.BodyTimes(ta);
            const uint64_t* btm = b.BodyTimes(tb);
            size_t jb = 0;
            for (size_t ja = 0; ja + 1 < ta.count; ++ja) {
                if (at[ja] < from || at[ja] >= to) continue;
                while (jb < tb.count && btm[jb] < at[ja]) ++jb;
                if (jb + 1 >= tb.count || btm[jb] != at[ja]) continue;
                if (memcmp(&a.BodyFrames(ta)[ja], &b.BodyFrames(tb)[jb], sizeof(BodyState)) != 0) {
                    ++rep.seamRawDiffers;
                    continue;
                }
                const int64_t mid = (int64_t)(at[ja] + (at[ja + 1] - at[ja]) / 2);
                BodyState sa, sb;
                memset(&sa, 0, sizeof sa);
                memset(&sb, 0, sizeof sb);
                SmoothTally tla, tlb;
                sm::SampleBodySmoothAt(a, ta, ta.cursor, mid, sa, false, true, &tla);
                sm::SampleBodySmoothAt(b, tb, tb.cursor, mid, sb, false, true, &tlb);
                if (tla.edge_raw != 0 || tlb.edge_raw != 0) continue;
                ++rep.seamCompared;
                if (memcmp(&sa, &sb, sizeof sa) != 0) ++rep.seamMismatch;
            }
            break;
        }
    }
    for (size_t i = 0; i < a.PlayerCount(); ++i) {
        PlayerTrack& ta = a.PlayerAt(i);
        for (size_t k = 0; k < b.PlayerCount(); ++k) {
            PlayerTrack& tb = b.PlayerAt(k);
            if (tb.user_slot != ta.user_slot || tb.generation != ta.generation) continue;
            const uint64_t* at = a.PlayerTimes(ta);
            const uint64_t* btm = b.PlayerTimes(tb);
            size_t jb = 0;
            for (size_t ja = 0; ja + 1 < ta.count; ++ja) {
                if (at[ja] < from || at[ja] >= to) continue;
                while (jb < tb.count && btm[jb] < at[ja]) ++jb;
                if (jb + 1 >= tb.count || btm[jb] != at[ja]) continue;
                if (memcmp(&a.PlayerFrames(ta)[ja], &b.PlayerFrames(tb)[jb], sizeof(PlayerState)) != 0) {
                    ++rep.seamRawDiffers;
                    continue;
                }
                const int64_t mid = (int64_t)(at[ja] + (at[ja + 1] - at[ja]) / 2);
                PlayerState sa, sb;
                memset(&sa, 0, sizeof sa);
                memset(&sb, 0, sizeof sb);
                SmoothTally tla, tlb;
                sm::SamplePlayerSmoothAt(a, ta, ta.cursor, mid, sa, false, &tla);
                sm::SamplePlayerSmoothAt(b, tb, tb.cursor, mid, sb, false, &tlb);
                if (tla.edge_raw != 0 || tlb.edge_raw != 0) continue;
                ++rep.seamCompared;
                if (memcmp(&sa, &sb, sizeof sa) != 0) ++rep.seamMismatch;
            }
            break;
        }
    }
}

}  // namespace

TEST_CASE("smoothing S8.2: real captures -- jitter, fidelity, breaks, window-edge continuity, cost") {
    const char* list = getenv("REPLAY_SMOOTH_CAPTURES");
    if (list == nullptr || *list == '\0') {
        MESSAGE("SKIPPED: set REPLAY_SMOOTH_CAPTURES to one or more .echodemo paths (';'-separated)");
        return;
    }
    std::vector<std::string> paths;
    for (const char* p = list; *p;) {
        const char* e = strchr(p, ';');
        paths.emplace_back(p, e ? (size_t)(e - p) : strlen(p));
        if (!e) break;
        p = e + 1;
    }
    for (const std::string& path : paths) {
        replay::DemoReader reader;
        std::wstring wpath(path.begin(), path.end());
        REQUIRE_MESSAGE(reader.Open(wpath.c_str()), path);
        static sm::TrackScan scan;
        static sm::NetVarPrimer primer;
        scan = sm::TrackScan();
        primer = sm::NetVarPrimer();
        const char* why = "unstated";
        REQUIRE(sm::ScanTrackSources(reader, scan, &why));
        sm::ScanNetVarPrimer(reader, primer);
        const WindowCaps caps = sm::CapsForScan(WindowCaps(), scan);

        static Window w(caps), next(caps);
        REQUIRE(w.Reserve(caps));
        REQUIRE(next.Reserve(caps));
        CaptureReport rep;
        std::vector<size_t> cores;
        for (size_t c = 0; c < reader.ChapterCount(); ++c) {
            if (reader.ChapterAt(c).section == replay::kSectionCore) cores.push_back(c);
        }
        for (size_t k = 0; k < cores.size(); ++k) {
            const replay::ChapterKey ck = reader.ChapterAt(cores[k]);
            REQUIRE(Build(reader, scan, primer, cores[k], w));
            MeasureWindow(w, ck.tBeginUs, ck.tEndUs, rep);
            if (k + 1 < cores.size()) {
                REQUIRE(Build(reader, scan, primer, cores[k + 1], next));
                CompareWindows(w, next, ck.tEndUs - 1000000, ck.tEndUs + 1000000, rep);
            }
        }
        reader.Close();

        const char* ruleName[] = {"none", "gap", "teleport", "possession", "state-word", "bad-sample",
                                  "vel-jump", "grab", "hand-jump"};
        const size_t dtN = std::max<size_t>(rep.dtN, 1);
        const double dtMean = rep.dtSum / dtN;
        printf("\n[smooth-capture] %s\n", path.c_str());
        printf("  pairs: body %zu player %zu; stale repeats body %.2f%% player %.2f%%; player stamp interval %.2f ms sd %.2f ms\n",
               rep.bodyPairs, rep.playerPairs, 100.0 * rep.bodyRepeats / std::max<size_t>(rep.bodyPairs, 1),
               100.0 * rep.playerRepeats / std::max<size_t>(rep.playerPairs, 1), dtMean,
               sqrt(std::max(0.0, rep.dtSq / dtN - dtMean * dtMean)));
        printf("  hmd orientation repeats its previous sample in %.1f%% of player samples\n",
               100.0 * rep.hmdRepeats / std::max<size_t>(rep.playerPairs, 1));
        printf("  breaks:");
        for (size_t r = 1; r < (size_t)BreakRule::kCount; ++r) printf(" %s=%zu", ruleName[r], rep.breaks[r]);
        printf("\n  knots: fit2=%zu fit1=%zu raw=%zu pinned=%zu\n", rep.kinds[1], rep.kinds[2], rep.kinds[3], rep.kinds[4]);
        printf("  0.1x sweep tally: brackets=%u held=%u edge_raw=%u fits=%u degenerate=%u quat_slerp=%u\n",
               rep.tally.brackets, rep.tally.held, rep.tally.edge_raw, rep.tally.fits, rep.tally.degenerate,
               rep.tally.quat_slerp);
        rep.com.Report("body com", "m/s", 1.0);
        rep.lh.Report("l hand", "m/s", 1.0);
        rep.rh.Report("r hand", "m/s", 1.0);
        rep.hmd.Report("hmd", "rad/s", 1.0);
        printf("  window seam (+-1 s): %zu brackets compared, %zu differ; %zu raw samples differ between the windows\n",
               rep.seamCompared, rep.seamMismatch, rep.seamRawDiffers);
        printf("  fit cost: body %.0f ns/knot (%zu), player %.0f ns/knot (%zu)\n",
               rep.fitNsBody / std::max<size_t>(rep.fitBodyN, 1), rep.fitBodyN,
               rep.fitNsPlayer / std::max<size_t>(rep.fitPlayerN, 1), rep.fitPlayerN);

        CHECK(rep.tally.edge_raw == 0);
        CHECK(rep.seamMismatch == 0);
        CHECK(rep.seamCompared > 0);
        CHECK(rep.lh.MedianRatio() <= 0.4);
        CHECK(rep.rh.MedianRatio() <= 0.4);
        // The recorded head orientation repeats its previous sample in over 90% of player samples in
        // these captures, so its median step is 0 on both paths and a ratio gate would pass vacuously.
        // What can be asserted is that smoothing never makes the head rougher.
        CHECK(rep.hmd.RmsRatio() <= 1.0);
        CHECK(rep.com.MedianRatio() <= 0.6);
        CHECK(KnotSteps::Pct(rep.com.fidelity, 0.99) <= 0.015);
        CHECK(KnotSteps::Pct(rep.lh.fidelity, 0.99) <= 0.025);
        CHECK(KnotSteps::Pct(rep.rh.fidelity, 0.99) <= 0.025);
    }
}

TEST_CASE("smoothing perf: fit and per-frame cost against the design's budget (S7)") {
    // S7 estimates 0.5 us per body fit, 1.2 us per player fit, 0.15 / 0.4 us per unchanged-bracket
    // frame. The gate is 4x those, so a noisy machine does not fail it but a pathological regression does.
    Window w;
    FillSmoothWindowWhole(w, 2000, 0.003, 51);
    w.SetSmoothing(sm::ParamsForStrength(2));
    BodyTrack& bt = w.BodyAt(0);
    PlayerTrack& pt = w.PlayerAt(0);
    const BodyState* bf = w.BodyFrames(bt);
    const PlayerState* pf = w.PlayerFrames(pt);
    const uint64_t* t = w.BodyTimes(bt);
    const SmoothParams p = w.Smoothing();
    using Clock = std::chrono::steady_clock;
    volatile float sink = 0;

    double bodyFitNs = 1e30, playerFitNs = 1e30, bodyFrameNs = 1e30, playerFrameNs = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        auto c0 = Clock::now();
        for (size_t j = 20; j + 20 < bt.count; ++j) {
            BodyKnot k;
            sm::FitBodyKnot(bf, t, bt.count, j, kWhole, p, k);
            sink = sink + k.com[0];
        }
        bodyFitNs = std::min(bodyFitNs, std::chrono::duration<double, std::nano>(Clock::now() - c0).count() / (bt.count - 40));
        c0 = Clock::now();
        for (size_t j = 20; j + 20 < pt.count; ++j) {
            PlayerKnot k;
            sm::FitPlayerKnot(pf, w.PlayerTimes(pt), pt.count, j, kWhole, p, k);
            sink = sink + k.vec[0];
        }
        playerFitNs = std::min(playerFitNs, std::chrono::duration<double, std::nano>(Clock::now() - c0).count() / (pt.count - 40));

        // 0.1x at 90 fps: ~1.1 ms of source per frame, so most frames stay inside one bracket.
        const int64_t from = (int64_t)t[50];
        const int frames = 20000;
        c0 = Clock::now();
        for (int f = 0; f < frames; ++f) {
            BodyState s;
            sm::SampleBodySmoothAt(w, bt, bt.cursor, from + f * 1100, s, false, true, nullptr);
            sink = sink + s.com[0];
        }
        bodyFrameNs = std::min(bodyFrameNs, std::chrono::duration<double, std::nano>(Clock::now() - c0).count() / frames);
        c0 = Clock::now();
        for (int f = 0; f < frames; ++f) {
            PlayerState s;
            sm::SamplePlayerSmoothAt(w, pt, pt.cursor, from + f * 1100, s, false, nullptr);
            sink = sink + s.lHandOff[0];
        }
        playerFrameNs = std::min(playerFrameNs, std::chrono::duration<double, std::nano>(Clock::now() - c0).count() / frames);
    }
    printf("[smooth-perf] fit: body %.0f ns, player %.0f ns | 0.1x frame (fits amortised): body %.0f ns, player %.0f ns\n",
           bodyFitNs, playerFitNs, bodyFrameNs, playerFrameNs);
    // 8 players and ~12 bodies at 0.1x: the per-frame total the S7 gate (mean <= OFF + 10 us) is about.
    printf("[smooth-perf] 8 players + 12 bodies at 0.1x: %.1f us per frame\n", (8 * playerFrameNs + 12 * bodyFrameNs) / 1000.0);
    CHECK(bodyFitNs < 4 * 500.0);
    CHECK(playerFitNs < 4 * 1200.0);
    CHECK((8 * playerFrameNs + 12 * bodyFrameNs) / 1000.0 < 10.0);
}
