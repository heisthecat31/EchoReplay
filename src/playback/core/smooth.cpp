#include "playback/core/smooth.h"

#include <math.h>
#include <string.h>

#include "playback/core/sample.h"
#include "playback/core/sample_internal.h"
#include "playback/core/window.h"

namespace replay {
namespace playback {

using namespace sample_internal;

namespace {

// At most this many neighbours per side, whatever the cadence: it caps a fit's cost on a 90 Hz stream.
constexpr size_t kKmax = 8;
constexpr float kVelocityJumpMps = 5.0f;     // bounce, punch, deflection: never smoothed through
constexpr float kHandJumpM = 0.5f;           // a hand offset moving this far in one sample is a glitch
// Chordal regression of quaternions is accurate for small spreads only: neighbours more than 0.6 rad
// from the centre sample are left out of its fit. |dot| = cos(angle / 2).
constexpr double kQuatSpreadCos = 0.95533648912560598;   // cos(0.3)
constexpr float kMaxQuatBracketRad = 1.0f;   // wider than this, the log-space cubic is not trusted

constexpr uint32_t kMaxSupport = 2 * kKmax + 1;

bool Finite3(const float v[3]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

bool GapAfter(const uint64_t* t, size_t i) {
    const int64_t dt = (int64_t)t[i + 1] - (int64_t)t[i];
    return dt <= 0 || dt > kMaxHermiteBracketUs;
}

bool DistanceOver(const float a[3], const float b[3], float limit) {
    const double dx = (double)b[0] - a[0], dy = (double)b[1] - a[1], dz = (double)b[2] - a[2];
    return !(dx * dx + dy * dy + dz * dz <= (double)limit * limit);
}

// ---- quaternion helpers, (x, y, z, w) --------------------------------------------------------------

void QMul(const double a[4], const double b[4], double out[4]) {
    const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

void QConj(const double q[4], double out[4]) {
    out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
}

void QLoad(const float q[4], double out[4]) {
    out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; out[3] = q[3];
}

// Rotation vector (axis * angle, rad) of a unit quaternion taken on its w >= 0 hemisphere.
void QLog(const double q[4], double r[3]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    if (w < 0.0) { x = -x; y = -y; z = -z; w = -w; }
    const double s = sqrt(x * x + y * y + z * z);
    // 2 atan2(s, w) / s, with its limit 2 / w as s -> 0.
    const double k = s > 1e-9 ? 2.0 * atan2(s, w) / s : 2.0 / (w > 1e-12 ? w : 1.0);
    r[0] = x * k; r[1] = y * k; r[2] = z * k;
}

void QExp(const double r[3], double q[4]) {
    const double angle = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    const double half = 0.5 * angle;
    const double k = angle > 1e-9 ? sin(half) / angle : 0.5 - angle * angle / 48.0;
    q[0] = r[0] * k; q[1] = r[1] * k; q[2] = r[2] * k; q[3] = cos(half);
}

// v' = q v q*, for a unit q.
void QRotate(const double q[4], const double v[3], double out[3]) {
    const double p[4] = {v[0], v[1], v[2], 0.0};
    double qc[4], t[4], r[4];
    QConj(q, qc);
    QMul(q, p, t);
    QMul(t, qc, r);
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

// Body-local angular rate that carries qa to qb over dt: Log(qa* qb) / dt.
void ChordRate(const float qa[4], const float qb[4], double dtSeconds, float rate[3]) {
    double a[4], b[4], ac[4], d[4], r[3];
    QLoad(qa, a);
    QLoad(qb, b);
    QConj(a, ac);
    QMul(ac, b, d);
    QLog(d, r);
    for (int c = 0; c < 3; ++c) rate[c] = dtSeconds > 0.0 ? (float)(r[c] / dtSeconds) : 0.0f;
}

// ---- the fit ---------------------------------------------------------------------------------------

struct Support {
    size_t lo = 0, hi = 0;
    bool left_edge = false, right_edge = false;   // j is the first/last sample of its segment
};

template <typename BreakFn>
Support FindSupport(const uint64_t* t, size_t n, size_t j, const SmoothParams& p, BreakFn brk) {
    Support s;
    s.lo = s.hi = j;
    s.left_edge = j == 0 || brk(j - 1);
    s.right_edge = j + 1 >= n || brk(j);
    // A segment edge's fit is one-sided: twice the reach keeps three samples in it at the lowest strength.
    const double reach = (s.left_edge || s.right_edge ? 2.0 : 1.0) * (double)p.radius_s * 1e6;
    const int64_t radius_us = (int64_t)reach;
    while (s.lo > 0 && j - s.lo < kKmax && (int64_t)(t[j] - t[s.lo - 1]) <= radius_us &&
           !brk(s.lo - 1)) {
        --s.lo;
    }
    while (s.hi + 1 < n && s.hi - j < kKmax && (int64_t)(t[s.hi + 1] - t[j]) <= radius_us &&
           !brk(s.hi)) {
        ++s.hi;
    }
    return s;
}

// The local-regression equivalent kernel: value = sum l_i x_i, derivative = sum d_i x_i. Returns the
// polynomial degree used (2 or 1), or 0 when fewer than two weighted samples pin a line.
int SolveKernel(const double* tau, const double* w, size_t cnt, int maxDegree, double* l, double* d) {
    double m[5] = {0, 0, 0, 0, 0};
    size_t effective = 0;
    for (size_t i = 0; i < cnt; ++i) {
        if (w[i] <= 0.0) continue;
        ++effective;
        const double t1 = tau[i], t2 = t1 * t1;
        m[0] += w[i];
        m[1] += w[i] * t1;
        m[2] += w[i] * t2;
        m[3] += w[i] * t2 * t1;
        m[4] += w[i] * t2 * t2;
    }
    if (maxDegree >= 2 && effective >= 3) {
        const double c00 = m[2] * m[4] - m[3] * m[3];
        const double c01 = m[2] * m[3] - m[1] * m[4];
        const double c02 = m[1] * m[3] - m[2] * m[2];
        const double c11 = m[0] * m[4] - m[2] * m[2];
        const double c12 = m[1] * m[2] - m[0] * m[3];
        const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
        const double scale = m[0] * m[2] * m[4];
        if (scale > 0.0 && fabs(det) > 1e-9 * scale) {
            const double inv = 1.0 / det;
            for (size_t i = 0; i < cnt; ++i) {
                const double t1 = tau[i], t2 = t1 * t1;
                l[i] = w[i] * (c00 + c01 * t1 + c02 * t2) * inv;
                d[i] = w[i] * (c01 + c11 * t1 + c12 * t2) * inv;
            }
            return 2;
        }
    }
    if (effective >= 2) {
        const double det = m[0] * m[2] - m[1] * m[1];
        if (m[0] * m[2] > 0.0 && det > 1e-9 * m[0] * m[2]) {
            const double inv = 1.0 / det;
            for (size_t i = 0; i < cnt; ++i) {
                l[i] = w[i] * (m[2] - m[1] * tau[i]) * inv;
                d[i] = w[i] * (m[0] * tau[i] - m[1]) * inv;
            }
            return 1;
        }
    }
    return 0;
}

struct Kernel {
    size_t lo = 0, cnt = 0, center = 0;   // center = j - lo
    double tau[kMaxSupport];
    double w[kMaxSupport];
    double l[kMaxSupport];
    double d[kMaxSupport];
    int degree = 0;
};

// Weights and the base kernel for the support. At a segment edge the fit is one-sided; the caller pins
// the value to the raw sample there and keeps only the fitted slope.
template <typename RepeatFn>
void BuildKernel(const uint64_t* t, size_t j, const Support& s, const SmoothParams& p,
                 RepeatFn isRepeat, Kernel& k) {
    k.lo = s.lo;
    k.cnt = s.hi - s.lo + 1;
    k.center = j - s.lo;
    const double inv2s2 = 1.0 / (2.0 * (double)p.sigma_s * (double)p.sigma_s);
    for (size_t q = 0; q < k.cnt; ++q) {
        const size_t i = s.lo + q;
        const double tau = (double)((int64_t)t[i] - (int64_t)t[j]) * 1e-6;
        k.tau[q] = tau;
        k.w[q] = isRepeat(i) ? 0.0 : exp(-tau * tau * inv2s2);
    }
    k.degree = SolveKernel(k.tau, k.w, k.cnt, 2, k.l, k.d);
}

// value/derivative of `dims` float channels at `stride` floats apart in each sample record.
bool ApplyKernel(const Kernel& k, const float* base, size_t recordFloats, size_t offset, int dims,
                 float* value, float* deriv) {
    for (int c = 0; c < dims; ++c) {
        double v = 0.0, dv = 0.0;
        for (size_t q = 0; q < k.cnt; ++q) {
            const double x = base[(k.lo + q) * recordFloats + offset + c];
            v += k.l[q] * x;
            dv += k.d[q] * x;
        }
        if (!isfinite(v) || !isfinite(dv)) return false;
        value[c] = (float)v;
        deriv[c] = (float)dv;
    }
    return true;
}

// Chordal regression of one quaternion channel, rate returned body-local.
bool FitQuat(const Kernel& base, const float* rec, size_t recordFloats, size_t offset, bool pinned,
             float q[4], float rate[3]) {
    const float* qj = rec + (base.lo + base.center) * recordFloats + offset;
    double aligned[kMaxSupport][4];
    double w[kMaxSupport];
    bool dropped = false;
    for (size_t i = 0; i < base.cnt; ++i) {
        const float* qi = rec + (base.lo + i) * recordFloats + offset;
        double dot = 0.0;
        for (int c = 0; c < 4; ++c) dot += (double)qi[c] * qj[c];
        const double sign = dot < 0.0 ? -1.0 : 1.0;
        for (int c = 0; c < 4; ++c) aligned[i][c] = sign * qi[c];
        w[i] = base.w[i];
        // A channel can repeat on its own (a head that updates less often than the hands): a stale
        // orientation carries no new information, exactly as a whole stale sample does.
        const bool stale = i > 0 && memcmp(qi, qi - recordFloats, 4 * sizeof(float)) == 0;
        if (fabs(dot) < kQuatSpreadCos || stale) {
            w[i] = 0.0;
            dropped = dropped || base.w[i] > 0.0;
        }
    }
    const double* l = base.l;
    const double* d = base.d;
    double lq[kMaxSupport], dq[kMaxSupport];
    if (dropped) {
        if (SolveKernel(base.tau, w, base.cnt, 2, lq, dq) == 0) return false;
        l = lq;
        d = dq;
    }
    double c0[4] = {0, 0, 0, 0}, c1[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < base.cnt; ++i) {
        for (int c = 0; c < 4; ++c) {
            c0[c] += l[i] * aligned[i][c];
            c1[c] += d[i] * aligned[i][c];
        }
    }
    const double len = sqrt(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2] + c0[3] * c0[3]);
    if (!(len > 1e-6) || !isfinite(len)) return false;
    double qk[4];
    if (pinned) {
        QLoad(qj, qk);
    } else {
        for (int c = 0; c < 4; ++c) qk[c] = c0[c] / len;
    }
    double dq4[4];
    double along = 0.0;
    for (int c = 0; c < 4; ++c) {
        dq4[c] = c1[c] / len;
        along += dq4[c] * qk[c];
    }
    for (int c = 0; c < 4; ++c) dq4[c] -= along * qk[c];
    double qc[4], omega[4];
    QConj(qk, qc);
    QMul(qc, dq4, omega);
    for (int c = 0; c < 3; ++c) {
        rate[c] = (float)(2.0 * omega[c]);
        if (!isfinite(rate[c])) return false;
    }
    for (int c = 0; c < 4; ++c) q[c] = (float)qk[c];
    return true;
}

// Chord slope of a float channel inside the segment [s.lo', s.hi'] that holds j: centred when both
// neighbours are in the segment, else one-sided, else zero.
void RawChord(const float* rec, size_t recordFloats, size_t offset, int dims, const uint64_t* t,
              size_t j, bool hasPrev, bool hasNext, float* deriv) {
    const size_t a = hasPrev ? j - 1 : j;
    const size_t b = hasNext ? j + 1 : j;
    const double dt = (double)((int64_t)t[b] - (int64_t)t[a]) * 1e-6;
    for (int c = 0; c < dims; ++c) {
        deriv[c] = dt > 0.0 ? (float)(((double)rec[b * recordFloats + offset + c] -
                                       rec[a * recordFloats + offset + c]) / dt)
                            : 0.0f;
    }
}

void RawQuatChord(const float* rec, size_t recordFloats, size_t offset, const uint64_t* t, size_t j,
                  bool hasPrev, bool hasNext, float rate[3]) {
    const size_t a = hasPrev ? j - 1 : j;
    const size_t b = hasNext ? j + 1 : j;
    ChordRate(rec + a * recordFloats + offset, rec + b * recordFloats + offset,
              (double)((int64_t)t[b] - (int64_t)t[a]) * 1e-6, rate);
}

constexpr size_t kBodyFloats = sizeof(BodyState) / sizeof(float);
constexpr size_t kPlayerFloats = sizeof(PlayerState) / sizeof(float);
static_assert(sizeof(BodyState) % sizeof(float) == 0, "BodyState is read as a float record");
static_assert(sizeof(PlayerState) % sizeof(float) == 0, "PlayerState is read as a float record");
constexpr size_t kBodyCom = offsetof(BodyState, com) / sizeof(float);
constexpr size_t kBodyOri = offsetof(BodyState, ori) / sizeof(float);
constexpr size_t kPlayerQuat[3] = {offsetof(PlayerState, hmdOri) / sizeof(float),
                                   offsetof(PlayerState, lHandOri) / sizeof(float),
                                   offsetof(PlayerState, rHandOri) / sizeof(float)};
// lHandOff, rHandOff, then the three posture scalars: contiguous in PlayerState.
constexpr size_t kPlayerVec = offsetof(PlayerState, lHandOff) / sizeof(float);
constexpr size_t kPlayerPosture = offsetof(PlayerState, bendBack) / sizeof(float);
static_assert(offsetof(PlayerState, rHandOff) == offsetof(PlayerState, lHandOff) + 12, "layout");
static_assert(offsetof(PlayerState, restPoseHeight) == offsetof(PlayerState, bendBack) + 8, "layout");

// A knot reads its support [lo, hi] and one sample either side of it (the walk's stopping test and the
// stale-repeat test). It is fitted identically in every window that holds all of those; two samples of
// margin, clear of the window's outer edge by a whole gap, keep a jitter-placed record from the next
// chapter out of the question. A real edge (file edge, birth, death) has nothing beyond it anywhere.
bool SupportComplete(const uint64_t* t, size_t n, const Support& s, TrackEdges e) {
    const bool left = e.begin_real || (s.lo >= 2 && (int64_t)t[s.lo - 2] >= e.safe_begin_us);
    const bool right = e.end_real || (s.hi + 2 < n && (int64_t)t[s.hi + 2] <= e.safe_end_us);
    return left && right;
}

bool BodyRepeat(const BodyState* f, const uint64_t* t, size_t n, size_t i) {
    return i > 0 && memcmp(f[i].com, f[i - 1].com, sizeof f[i].com) == 0 &&
           !BodyBreakAfter(f, t, n, i - 1);
}

bool PlayerRepeat(const PlayerState* f, const uint64_t* t, size_t n, size_t i) {
    return i > 0 && memcmp(f[i].lHandOff, f[i - 1].lHandOff, sizeof f[i].lHandOff) == 0 &&
           memcmp(f[i].rHandOff, f[i - 1].rHandOff, sizeof f[i].rHandOff) == 0 &&
           !PlayerBreakAfter(f, t, n, i - 1);
}

}  // namespace

// ---- settings --------------------------------------------------------------------------------------

SmoothParams ParamsForStrength(uint8_t strength) {
    if (strength < kSmoothStrengthMin) strength = kSmoothStrengthMin;
    if (strength > kSmoothStrengthMax) strength = kSmoothStrengthMax;
    static const float kSigma[3] = {0.018f, 0.030f, 0.050f};
    SmoothParams p;
    p.strength = strength;
    p.sigma_s = kSigma[strength - 1];
    p.radius_s = 2.5f * p.sigma_s;
    return p;
}

bool ParseSmoothMode(const char* s, SmoothMode* out) {
    if (s == nullptr) return false;
    if (strcmp(s, "off") == 0) { *out = SmoothMode::Off; return true; }
    if (strcmp(s, "slowmo") == 0) { *out = SmoothMode::SlowMo; return true; }
    if (strcmp(s, "on") == 0) { *out = SmoothMode::On; return true; }
    return false;
}

const char* SmoothModeName(SmoothMode m) {
    switch (m) {
    case SmoothMode::Off: return "off";
    case SmoothMode::SlowMo: return "slow-mo";
    case SmoothMode::On: return "always";
    }
    return "off";
}

const char* SmoothStrengthName(uint8_t strength) {
    return strength <= 1 ? "low" : strength == 2 ? "med" : "high";
}

bool SmoothWanted(SmoothMode m, double signedRate) {
    switch (m) {
    case SmoothMode::On: return true;
    case SmoothMode::SlowMo: return fabs(signedRate) < 0.999;
    case SmoothMode::Off: return false;
    }
    return false;
}

float StepSmoothWeight(float current, float target, int64_t wall_us, bool snap) {
    if (snap) return target;
    const float step = float(wall_us > 0 ? wall_us : 0) / 200000.0f;
    if (current < target) return current + step < target ? current + step : target;
    if (current > target) return current - step > target ? current - step : target;
    return current;
}

// ---- breaks ----------------------------------------------------------------------------------------

BreakRule BodyBreakRule(const BodyState* f, const uint64_t* t, size_t n, size_t i) {
    if (i + 1 >= n) return BreakRule::None;
    const BodyState& a = f[i];
    const BodyState& b = f[i + 1];
    if (GapAfter(t, i)) return BreakRule::Gap;
    if (IsTeleport(a.com, b.com, t[i], t[i + 1])) return BreakRule::Teleport;
    if (a.owner_slot != b.owner_slot) return BreakRule::Possession;
    if (a.bits11 != b.bits11) return BreakRule::StateWord;
    if (!UsableTangent(a.vel) || !UsableTangent(b.vel) || !Finite3(a.com) || !Finite3(b.com)) {
        return BreakRule::BadSample;
    }
    if (DistanceOver(a.vel, b.vel, kVelocityJumpMps)) return BreakRule::VelocityJump;
    return BreakRule::None;
}

BreakRule PlayerBreakRule(const PlayerState* f, const uint64_t* t, size_t n, size_t i) {
    if (i + 1 >= n) return BreakRule::None;
    const PlayerState& a = f[i];
    const PlayerState& b = f[i + 1];
    if (GapAfter(t, i)) return BreakRule::Gap;
    if (a.holdL != b.holdL || a.holdR != b.holdR || a.anchor != b.anchor) return BreakRule::Grab;
    if (!Finite3(a.lHandOff) || !Finite3(b.lHandOff) || !Finite3(a.rHandOff) || !Finite3(b.rHandOff)) {
        return BreakRule::BadSample;
    }
    if (DistanceOver(a.lHandOff, b.lHandOff, kHandJumpM) ||
        DistanceOver(a.rHandOff, b.rHandOff, kHandJumpM)) {
        return BreakRule::HandJump;
    }
    return BreakRule::None;
}

bool BodyBreakAfter(const BodyState* f, const uint64_t* t, size_t n, size_t i) {
    return BodyBreakRule(f, t, n, i) != BreakRule::None;
}

bool PlayerBreakAfter(const PlayerState* f, const uint64_t* t, size_t n, size_t i) {
    return PlayerBreakRule(f, t, n, i) != BreakRule::None;
}

void BodyKnotSupport(const BodyState* f, const uint64_t* t, size_t n, size_t j, const SmoothParams& p,
                     size_t* lo, size_t* hi) {
    const Support s = FindSupport(t, n, j, p, [&](size_t i) { return BodyBreakAfter(f, t, n, i); });
    *lo = s.lo;
    *hi = s.hi;
}

// ---- knots -----------------------------------------------------------------------------------------

// noinline and one call site each: every knot comes from one instruction sequence, so a knot fitted in
// one window, direction or frame is bitwise the knot fitted in any other.
__declspec(noinline) KnotKind FitBodyKnot(const BodyState* f, const uint64_t* t, size_t n, size_t j,
                                          TrackEdges e, const SmoothParams& p, BodyKnot& out) {
    const Support s = FindSupport(t, n, j, p, [&](size_t i) { return BodyBreakAfter(f, t, n, i); });
    out.complete = SupportComplete(t, n, s, e);
    const bool pinned = s.left_edge || s.right_edge;
    const BodyState& raw = f[j];
    const float* rec = reinterpret_cast<const float*>(f);

    Kernel k;
    BuildKernel(t, j, s, p, [&](size_t i) { return BodyRepeat(f, t, n, i); }, k);

    const bool hasPrev = !s.left_edge, hasNext = !s.right_edge;
    KnotKind kind = pinned ? KnotKind::Pinned : k.degree == 2 ? KnotKind::Fit2 : KnotKind::Fit1;
    if (k.degree == 0 || !ApplyKernel(k, rec, kBodyFloats, kBodyCom, 3, out.com, out.vel)) {
        kind = KnotKind::Raw;
        memcpy(out.com, raw.com, sizeof out.com);
        if (UsableTangent(raw.vel)) {
            memcpy(out.vel, raw.vel, sizeof out.vel);
        } else {
            RawChord(rec, kBodyFloats, kBodyCom, 3, t, j, hasPrev, hasNext, out.vel);
        }
    } else if (pinned) {
        memcpy(out.com, raw.com, sizeof out.com);
    }
    // Each orientation falls back on its own: a body that does not turn has nothing to fit, and that
    // must not cost its position the fit it does have.
    if (k.degree == 0 || !FitQuat(k, rec, kBodyFloats, kBodyOri, pinned, out.ori, out.oriRate)) {
        memcpy(out.ori, raw.ori, sizeof out.ori);
        RawQuatChord(rec, kBodyFloats, kBodyOri, t, j, hasPrev, hasNext, out.oriRate);
    }
    out.kind = kind;
    return kind;
}

__declspec(noinline) KnotKind FitPlayerKnot(const PlayerState* f, const uint64_t* t, size_t n,
                                            size_t j, TrackEdges e, const SmoothParams& p,
                                            PlayerKnot& out) {
    const Support s = FindSupport(t, n, j, p, [&](size_t i) { return PlayerBreakAfter(f, t, n, i); });
    out.complete = SupportComplete(t, n, s, e);
    const bool pinned = s.left_edge || s.right_edge;
    const float* rec = reinterpret_cast<const float*>(f);
    const float* raw = rec + j * kPlayerFloats;

    Kernel k;
    BuildKernel(t, j, s, p, [&](size_t i) { return PlayerRepeat(f, t, n, i); }, k);

    const bool hasPrev = !s.left_edge, hasNext = !s.right_edge;
    KnotKind kind = pinned ? KnotKind::Pinned : k.degree == 2 ? KnotKind::Fit2 : KnotKind::Fit1;
    if (k.degree == 0 || !ApplyKernel(k, rec, kPlayerFloats, kPlayerVec, 6, out.vec, out.vecRate) ||
        !ApplyKernel(k, rec, kPlayerFloats, kPlayerPosture, 3, out.vec + 6, out.vecRate + 6)) {
        kind = KnotKind::Raw;
        memcpy(out.vec, raw + kPlayerVec, 6 * sizeof(float));
        memcpy(out.vec + 6, raw + kPlayerPosture, 3 * sizeof(float));
        RawChord(rec, kPlayerFloats, kPlayerVec, 6, t, j, hasPrev, hasNext, out.vecRate);
        RawChord(rec, kPlayerFloats, kPlayerPosture, 3, t, j, hasPrev, hasNext, out.vecRate + 6);
    } else if (pinned) {
        memcpy(out.vec, raw + kPlayerVec, 6 * sizeof(float));
        memcpy(out.vec + 6, raw + kPlayerPosture, 3 * sizeof(float));
    }
    for (int q = 0; q < 3; ++q) {
        if (k.degree == 0 ||
            !FitQuat(k, rec, kPlayerFloats, kPlayerQuat[q], pinned, out.quat[q], out.quatRate[q])) {
            memcpy(out.quat[q], raw + kPlayerQuat[q], 4 * sizeof(float));
            RawQuatChord(rec, kPlayerFloats, kPlayerQuat[q], t, j, hasPrev, hasNext, out.quatRate[q]);
        }
    }
    out.kind = kind;
    return kind;
}

TrackEdges EdgesOf(const Window& w, const uint64_t* times, size_t n) {
    TrackEdges e;
    e.safe_begin_us = (int64_t)w.OuterBeginUs() + kMaxHermiteBracketUs;
    e.safe_end_us = (int64_t)w.OuterEndUs() - kMaxHermiteBracketUs;
    if (n == 0) return e;
    e.begin_real = w.BeginsFile() || (int64_t)times[0] >= e.safe_begin_us;
    e.end_real = w.EndsFile() || (int64_t)times[n - 1] <= e.safe_end_us;
    return e;
}

namespace {

// The memo slot for knot j, fitting it on a miss. `keep` is the index the caller still needs, so the
// two ends of one bracket never evict each other.
template <typename Knot, typename FitFn>
const Knot& MemoKnot(KnotMemo<Knot>& memo, uint32_t gen, size_t j, size_t keep, SmoothTally* tally,
                     FitFn fit) {
    for (int s = 0; s < 2; ++s) {
        if (memo.gen[s] == gen && memo.index[s] == j) return memo.knot[s];
    }
    int slot = 0;
    if (memo.gen[0] == gen && memo.index[0] == keep) slot = 1;
    const KnotKind kind = fit(memo.knot[slot]);
    memo.gen[slot] = gen;
    memo.index[slot] = j;
    if (tally != nullptr) {
        ++tally->fits;
        if (kind == KnotKind::Fit1 || kind == KnotKind::Raw) ++tally->degenerate;
    }
    return memo.knot[slot];
}

// Cubic Hermite with its time derivative. An axis whose curve leaves its own chord by more than the
// 1:1 sampler allows takes the line, exactly as HermiteVec3 does.
void HermiteWithRate(const float* p0, const float* p1, const float* v0, const float* v1, int dims,
                     float dt, float u, bool clampToChord, float* out, float* rate) {
    const float u2 = u * u, u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f, h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2, h11 = u3 - u2;
    const float g00 = 6.0f * u2 - 6.0f * u, g10 = 3.0f * u2 - 4.0f * u + 1.0f;
    const float g01 = -g00, g11 = 3.0f * u2 - 2.0f * u;
    for (int c = 0; c < dims; ++c) {
        const float curve = h00 * p0[c] + h10 * (v0[c] * dt) + h01 * p1[c] + h11 * (v1[c] * dt);
        const float line = p0[c] + (p1[c] - p0[c]) * u;
        const float off = curve - line;
        if (clampToChord && (off > kMaxHermiteDeviation || off < -kMaxHermiteDeviation)) {
            out[c] = line;
            if (rate != nullptr) rate[c] = (p1[c] - p0[c]) / dt;
        } else {
            out[c] = curve;
            if (rate != nullptr) {
                rate[c] = (g00 * p0[c] + g10 * (v0[c] * dt) + g01 * p1[c] + g11 * (v1[c] * dt)) / dt;
            }
        }
    }
}

// q(u) = qa Exp(H(u)), H the Hermite in the tangent space at qa from 0 to Log(qa* qb). Falls back to a
// slerp between the knots when the bracket turns more than 1 rad.
bool QuatHermite(const float qa[4], const float wa[3], const float qbIn[4], const float wb[3],
                 float dt, float u, float out[4]) {
    double a[4], b[4];
    QLoad(qa, a);
    QLoad(qbIn, b);
    double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0) {
        for (int c = 0; c < 4; ++c) b[c] = -b[c];
    }
    double ac[4], D[4], r[3];
    QConj(a, ac);
    QMul(ac, b, D);
    QLog(D, r);
    const double angle = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!(angle <= kMaxQuatBracketRad)) {
        SlerpQuat(qa, qbIn, u, out);
        return true;
    }
    const double wbd[3] = {wb[0], wb[1], wb[2]};
    double wbInA[3];
    QRotate(D, wbd, wbInA);
    const double uu = u, u2 = uu * uu, u3 = u2 * uu;
    const double h10 = u3 - 2.0 * u2 + uu, h01 = -2.0 * u3 + 3.0 * u2, h11 = u3 - u2;
    double H[3];
    for (int c = 0; c < 3; ++c) {
        H[c] = h10 * (double)wa[c] * dt + h01 * r[c] + h11 * wbInA[c] * dt;
    }
    double e[4], q[4];
    QExp(H, e);
    QMul(a, e, q);
    const double len = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int c = 0; c < 4; ++c) out[c] = (float)(q[c] / len);
    return false;
}

void Negate3(float v[3]) {
    v[0] = -v[0];
    v[1] = -v[1];
    v[2] = -v[2];
}

}  // namespace

bool SampleBodySmoothAt(Window& w, BodyTrack& tr, SeriesCursor& cursor, int64_t t_us, BodyState& out,
                        bool reversed, bool hermite, SmoothTally* tally) {
    const BodyState* f = w.BodyFrames(tr);
    const uint64_t* t = w.BodyTimes(tr);
    const size_t n = tr.count;
    const size_t i = cursor.Resolve(t, n, t_us);
    if (i == kNoRecord) {
        return false;
    }
    auto raw = [&]() {
        SeriesCursor resolved = cursor;
        return SampleBodyAt(f, t, n, resolved, t_us, out, reversed, hermite, nullptr);
    };
    if (i + 1 >= n) {
        return raw();
    }
    if (BodyBreakAfter(f, t, n, i)) {
        if (tally != nullptr) ++tally->held;
        return raw();
    }
    const TrackEdges e = EdgesOf(w, t, n);
    const SmoothParams& p = w.Smoothing();
    const uint32_t gen = w.SmoothGen();
    auto fit = [&](size_t j) {
        return [&, j](BodyKnot& k) { return FitBodyKnot(f, t, n, j, e, p, k); };
    };
    const BodyKnot ka = MemoKnot(tr.smooth_memo, gen, i, i + 1, tally, fit(i));
    const BodyKnot& kb = MemoKnot(tr.smooth_memo, gen, i + 1, i, tally, fit(i + 1));
    if (!ka.complete || !kb.complete) {
        if (tally != nullptr) ++tally->edge_raw;
        return raw();
    }

    const BodyState& a = f[i];
    const BodyState& b = f[i + 1];
    const float u = Alpha(t[i], t[i + 1], t_us);
    const float dt = (float)((double)((int64_t)t[i + 1] - (int64_t)t[i]) * 1e-6);
    HermiteWithRate(ka.com, kb.com, ka.vel, kb.vel, 3, dt, u, true, out.com, out.vel);
    if (tr.claim_owner) {
        SlerpQuat(a.ori, b.ori, u, out.ori);   // a disc's spin is not what the owner sees as jitter
    } else if (QuatHermite(ka.ori, ka.oriRate, kb.ori, kb.oriRate, dt, u, out.ori) && tally != nullptr) {
        ++tally->quat_slerp;
    }
    LerpVec3(a.angVel, b.angVel, u, out.angVel);
    LerpVec3(a.lastHeldCom, b.lastHeldCom, u, out.lastHeldCom);
    out.bits11 = a.bits11;
    out.owner_slot = a.owner_slot;
    if (reversed) {
        Negate3(out.vel);
        Negate3(out.angVel);
    }
    if (tally != nullptr) ++tally->brackets;
    return true;
}

bool SamplePlayerSmoothAt(Window& w, PlayerTrack& tr, SeriesCursor& cursor, int64_t t_us,
                          PlayerState& out, bool reversed, SmoothTally* tally) {
    const PlayerState* f = w.PlayerFrames(tr);
    const uint64_t* t = w.PlayerTimes(tr);
    const size_t n = tr.count;
    const size_t i = cursor.Resolve(t, n, t_us);
    if (i == kNoRecord) {
        return false;
    }
    auto raw = [&]() {
        SeriesCursor resolved = cursor;
        return SamplePlayerAt(f, t, n, resolved, t_us, out, reversed);
    };
    if (i + 1 >= n) {
        return raw();
    }
    if (PlayerBreakAfter(f, t, n, i)) {
        if (tally != nullptr) ++tally->held;
        return raw();
    }
    const TrackEdges e = EdgesOf(w, t, n);
    const SmoothParams& p = w.Smoothing();
    const uint32_t gen = w.SmoothGen();
    auto fit = [&](size_t j) {
        return [&, j](PlayerKnot& k) { return FitPlayerKnot(f, t, n, j, e, p, k); };
    };
    const PlayerKnot ka = MemoKnot(tr.smooth_memo, gen, i, i + 1, tally, fit(i));
    const PlayerKnot& kb = MemoKnot(tr.smooth_memo, gen, i + 1, i, tally, fit(i + 1));
    if (!ka.complete || !kb.complete) {
        if (tally != nullptr) ++tally->edge_raw;
        return raw();
    }

    const PlayerState& a = f[i];
    const PlayerState& b = f[i + 1];
    const float u = Alpha(t[i], t[i + 1], t_us);
    const float dt = (float)((double)((int64_t)t[i + 1] - (int64_t)t[i]) * 1e-6);
    float* quats[3] = {out.hmdOri, out.lHandOri, out.rHandOri};
    for (int q = 0; q < 3; ++q) {
        if (QuatHermite(ka.quat[q], ka.quatRate[q], kb.quat[q], kb.quatRate[q], dt, u, quats[q]) &&
            tally != nullptr) {
            ++tally->quat_slerp;
        }
    }
    float vec[9];
    HermiteWithRate(ka.vec, kb.vec, ka.vecRate, kb.vecRate, 6, dt, u, true, vec, nullptr);
    HermiteWithRate(ka.vec + 6, kb.vec + 6, ka.vecRate + 6, kb.vecRate + 6, 3, dt, u, false, vec + 6,
                    nullptr);
    memcpy(out.lHandOff, vec, sizeof out.lHandOff);
    memcpy(out.rHandOff, vec + 3, sizeof out.rHandOff);
    out.bendBack = vec[6];
    out.bendFwd = vec[7];
    out.restPoseHeight = vec[8];
    LerpVec3(a.refVel, b.refVel, u, out.refVel);
    out.trigL = a.trigL + (b.trigL - a.trigL) * u;
    out.trigR = a.trigR + (b.trigR - a.trigR) * u;
    out.bits28 = a.bits28;
    out.holdL = a.holdL;
    out.holdR = a.holdR;
    out.anchor = a.anchor;
    if (reversed) {
        Negate3(out.refVel);
    }
    if (tally != nullptr) ++tally->brackets;
    return true;
}

void BlendBody(const BodyState& raw, const BodyState& sm, float wSmooth, BodyState& out) {
    LerpVec3(raw.com, sm.com, wSmooth, out.com);
    LerpVec3(raw.vel, sm.vel, wSmooth, out.vel);
    LerpVec3(raw.angVel, sm.angVel, wSmooth, out.angVel);
    LerpVec3(raw.lastHeldCom, sm.lastHeldCom, wSmooth, out.lastHeldCom);
    SlerpQuat(raw.ori, sm.ori, wSmooth, out.ori);
    out.bits11 = raw.bits11;
    out.owner_slot = raw.owner_slot;
}

void BlendPlayer(const PlayerState& raw, const PlayerState& sm, float wSmooth, PlayerState& out) {
    SlerpQuat(raw.hmdOri, sm.hmdOri, wSmooth, out.hmdOri);
    SlerpQuat(raw.lHandOri, sm.lHandOri, wSmooth, out.lHandOri);
    SlerpQuat(raw.rHandOri, sm.rHandOri, wSmooth, out.rHandOri);
    LerpVec3(raw.lHandOff, sm.lHandOff, wSmooth, out.lHandOff);
    LerpVec3(raw.rHandOff, sm.rHandOff, wSmooth, out.rHandOff);
    LerpVec3(raw.refVel, sm.refVel, wSmooth, out.refVel);
    out.bendBack = raw.bendBack + (sm.bendBack - raw.bendBack) * wSmooth;
    out.bendFwd = raw.bendFwd + (sm.bendFwd - raw.bendFwd) * wSmooth;
    out.restPoseHeight = raw.restPoseHeight + (sm.restPoseHeight - raw.restPoseHeight) * wSmooth;
    out.trigL = raw.trigL;
    out.trigR = raw.trigR;
    out.bits28 = raw.bits28;
    out.holdL = raw.holdL;
    out.holdR = raw.holdR;
    out.anchor = raw.anchor;
}

}  // namespace playback
}  // namespace replay
