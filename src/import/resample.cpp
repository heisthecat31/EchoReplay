#include "import/resample.h"

#include <math.h>

#include <ratio>

namespace replay::import {
namespace {

// A jump shorter than this is never a teleport, whatever the speed: repaired stamps can be 1 ms apart,
// and a few centimetres over 1 ms is noise, not a respawn.
constexpr float kTeleportMinJumpM = 1.5f;

float Dist(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

float MonotoneTangent(float left, float right) {
    if (left * right <= 0.0f) return 0.0f;
    const float limit = 3.0f * fminf(fabsf(left), fabsf(right));
    const float m = 0.5f * (left + right);
    return fabsf(m) > limit ? copysignf(limit, m) : m;
}

void Slerp(const float* a, const float* b, float s, float* out) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float wa = 1.0f - s, wb = s;
    if (d < 0.9995f) {
        d = fmaxf(-1.0f, fminf(1.0f, d));
        const float theta = acosf(d);
        const float sinTheta = sinf(theta);
        wa = sinf((1.0f - s) * theta) / sinTheta;
        wb = sinf(s * theta) / sinTheta;
    }
    float n = 0.0f;
    for (int i = 0; i < 4; ++i) {
        out[i] = wa * a[i] + wb * b[i];
        n += out[i] * out[i];
    }
    n = sqrtf(n);
    for (int i = 0; i < 4; ++i) out[i] /= n;
}

// World-frame angular velocity taking a to b over dtS: b = r * a, r = b * conj(a).
void AngularVelocity(const float* a, const float* b, float dtS, float* out) {
    const float rx = b[3] * -a[0] + b[0] * a[3] + b[1] * -a[2] - b[2] * -a[1];
    const float ry = b[3] * -a[1] - b[0] * -a[2] + b[1] * a[3] + b[2] * -a[0];
    const float rz = b[3] * -a[2] + b[0] * -a[1] - b[1] * -a[0] + b[2] * a[3];
    float rw = b[3] * a[3] + b[0] * a[0] + b[1] * a[1] + b[2] * a[2];
    const float sign = rw < 0.0f ? -1.0f : 1.0f;
    rw *= sign;
    const float sinHalf = sqrtf(rx * rx + ry * ry + rz * rz);
    if (sinHalf < 1e-7f || dtS <= 0.0f) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    const float angle = 2.0f * atan2f(sinHalf, rw);
    const float k = sign * angle / (sinHalf * dtS);
    out[0] = rx * k;
    out[1] = ry * k;
    out[2] = rz * k;
}

}  // namespace

void TrackAdd(Track& t, uint32_t frame, int64_t tUs, const float pos[3], const float q[4], uint32_t sample) {
    t.frame.push_back(frame);
    t.tUs.push_back(tUs);
    t.pos.insert(t.pos.end(), pos, pos + 3);
    t.ori.insert(t.ori.end(), q, q + 4);
    t.sample.push_back(sample);
}

void TrackFinish(Track& t, float maxSpeedMps) {
    const size_t n = t.frame.size();
    t.joinsNext.assign(n, 0);
    t.tangent.assign(n * 3, 0.0f);
    t.cursor = 0;
    t.teleports = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        const int64_t dt = t.tUs[i + 1] - t.tUs[i];
        if (t.frame[i + 1] != t.frame[i] + 1 || dt <= 0 || dt > kGapUs) continue;
        const float jump = Dist(&t.pos[i * 3], &t.pos[(i + 1) * 3]);
        if (jump > kTeleportMinJumpM && jump > maxSpeedMps * float(dt) * 1e-6f) {
            ++t.teleports;
            continue;
        }
        t.joinsNext[i] = 1;
        const float* q0 = &t.ori[i * 4];
        float* q1 = &t.ori[(i + 1) * 4];
        if (q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3] < 0.0f) {
            for (int a = 0; a < 4; ++a) q1[a] = -q1[a];
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const bool hasLeft = i > 0 && t.joinsNext[i - 1];
        const bool hasRight = t.joinsNext[i] != 0;
        for (int a = 0; a < 3; ++a) {
            float left = 0.0f, right = 0.0f;
            if (hasLeft) left = (t.pos[i * 3 + a] - t.pos[(i - 1) * 3 + a]) / (float(t.tUs[i] - t.tUs[i - 1]) * 1e-6f);
            if (hasRight) right = (t.pos[(i + 1) * 3 + a] - t.pos[i * 3 + a]) / (float(t.tUs[i + 1] - t.tUs[i]) * 1e-6f);
            float m = 0.0f;
            if (hasLeft && hasRight) m = MonotoneTangent(left, right);
            else if (hasLeft) m = left;
            else if (hasRight) m = right;
            t.tangent[i * 3 + a] = m;
        }
    }
}

int32_t TrackLocate(Track& t, const TickFrame& tick) {
    const uint32_t n = static_cast<uint32_t>(t.frame.size());
    while (t.cursor < n && t.frame[t.cursor] < tick.older) ++t.cursor;
    return t.cursor < n && t.frame[t.cursor] == tick.older ? static_cast<int32_t>(t.cursor) : -1;
}

void TrackEval(const Track& t, uint32_t i, const TickFrame& tick, TrackSample& out) {
    const float* p0 = &t.pos[i * 3];
    const float* q0 = &t.ori[i * 4];
    const bool inSegment = t.joinsNext[i] && t.frame[i + 1] == tick.newer && tick.alpha > 0.0f;
    if (!inSegment) {
        for (int a = 0; a < 3; ++a) {
            out.pos[a] = p0[a];
            // Exactly on a point that continues, the curve's own tangent; held anywhere else.
            out.vel[a] = (t.joinsNext[i] && tick.alpha == 0.0f) ? t.tangent[i * 3 + a] : 0.0f;
            out.angVel[a] = 0.0f;
        }
        for (int a = 0; a < 4; ++a) out.ori[a] = q0[a];
        if (t.joinsNext[i] && tick.alpha == 0.0f) {
            AngularVelocity(q0, &t.ori[(i + 1) * 4], float(t.tUs[i + 1] - t.tUs[i]) * 1e-6f, out.angVel);
        }
        return;
    }
    const float* p1 = &t.pos[(i + 1) * 3];
    const float* q1 = &t.ori[(i + 1) * 4];
    const float* m0 = &t.tangent[i * 3];
    const float* m1 = &t.tangent[(i + 1) * 3];
    const float dt = float(t.tUs[i + 1] - t.tUs[i]) * 1e-6f;
    const float s = tick.alpha, s2 = s * s, s3 = s2 * s;
    const float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
    const float d00 = 6 * s2 - 6 * s, d10 = 3 * s2 - 4 * s + 1, d01 = -6 * s2 + 6 * s, d11 = 3 * s2 - 2 * s;
    for (int a = 0; a < 3; ++a) {
        out.pos[a] = h00 * p0[a] + h10 * dt * m0[a] + h01 * p1[a] + h11 * dt * m1[a];
        out.vel[a] = (d00 * p0[a] + d01 * p1[a]) / dt + d10 * m0[a] + d11 * m1[a];
    }
    Slerp(q0, q1, s, out.ori);
    AngularVelocity(q0, q1, dt, out.angVel);
}

Result BuildTickGrid(const SourceMatch& match, uint32_t rateHz, std::vector<TickFrame>& out) {
    out.clear();
    // At most one tick per millisecond.
    if (rateHz == 0 || rateHz > std::milli::den) {
        return Refuse(ResultCode::BadArguments, "tick rate %u Hz is out of range", rateHz);
    }
    const size_t n = match.frames.size();
    if (n == 0) return Refuse(ResultCode::TooShort, "the match has no frames");
    const int64_t t0 = match.frames[0].tUs;
    const int64_t tEnd = match.frames[n - 1].tUs;
    out.reserve(static_cast<size_t>((tEnd - t0) / (1000000 / rateHz) + 2));
    uint32_t older = 0;
    bool gapPending = false;
    for (uint64_t k = 0;; ++k) {
        const int64_t t = t0 + static_cast<int64_t>((k * 1000000ull + rateHz / 2) / rateHz);
        if (t > tEnd) break;
        while (older + 1 < n && match.frames[older + 1].tUs <= t) ++older;
        const int64_t span = match.frames[older + 1 < n ? older + 1 : older].tUs - match.frames[older].tUs;
        if (span > kGapUs && t != match.frames[older].tUs) {
            gapPending = true;
            continue;
        }
        TickFrame tick{};
        tick.tUs = t;
        tick.older = older;
        tick.newer = older + 1 < n ? older + 1 : older;
        tick.alpha = span > 0 ? float(double(t - match.frames[older].tUs) / double(span)) : 0.0f;
        tick.gapBefore = gapPending;
        gapPending = false;
        out.push_back(tick);
    }
    return Success();
}

}  // namespace replay::import
