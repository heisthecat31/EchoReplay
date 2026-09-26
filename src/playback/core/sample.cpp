#include "playback/core/sample.h"

#include "playback/core/sample_internal.h"

#include <math.h>
#include <string.h>

namespace replay {
namespace playback {

using namespace sample_internal;

void LerpVec3(const float a[3], const float b[3], float t, float out[3]) {
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = a[1] + (b[1] - a[1]) * t;
    out[2] = a[2] + (b[2] - a[2]) * t;
}

void SlerpQuat(const float a[4], const float b[4], float t, float out[4]) {
    t = Clamp01(t);
    float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    double dot = (double)a[0] * bx + (double)a[1] * by + (double)a[2] * bz + (double)a[3] * bw;

    // Shortest path. q and -q are the same rotation, so a negative dot means the raw interpolation
    // would take the long way round -- visible as a body spinning the wrong way through a sample
    // boundary, which is exactly the artefact a replay must not invent.
    if (dot < 0.0) {
        bx = -bx; by = -by; bz = -bz; bw = -bw;
        dot = -dot;
    }
    if (dot > 1.0) dot = 1.0;

    float s0, s1;
    if (dot > 0.9995) {
        // Nearly parallel: sin(theta) underflows and the slerp formula loses all precision. A
        // normalised lerp is within float noise of the slerp here and is numerically stable.
        s0 = 1.0f - t;
        s1 = t;
    } else {
        const double theta = acos(dot);
        const double sin_theta = sin(theta);
        s0 = (float)(sin((1.0 - (double)t) * theta) / sin_theta);
        s1 = (float)(sin((double)t * theta) / sin_theta);
    }

    float x = a[0] * s0 + bx * s1;
    float y = a[1] * s0 + by * s1;
    float z = a[2] * s0 + bz * s1;
    float w = a[3] * s0 + bw * s1;

    const double len = sqrt((double)x * x + (double)y * y + (double)z * z + (double)w * w);
    if (len > 1e-12) {
        const float inv = (float)(1.0 / len);
        x *= inv; y *= inv; z *= inv; w *= inv;
    } else {
        // Degenerate input. Return the older sample rather than a zero quaternion, which downstream
        // would turn into a NaN transform on the body.
        x = a[0]; y = a[1]; z = a[2]; w = a[3];
    }
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}

static void NegateVec3(float v[3]) {
    v[0] = -v[0];
    v[1] = -v[1];
    v[2] = -v[2];
}


// True when an axis overshot its own chord and took the line instead of the curve.
static bool HermiteVec3(const float p0[3], const float p1[3], const float v0[3], const float v1[3],
                        float dtSeconds, float u, float out[3]) {
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    const float h10 = u3 - 2.0f * u2 + u;
    const float h01 = -2.0f * u3 + 3.0f * u2;
    const float h11 = u3 - u2;
    bool clamped = false;
    for (int k = 0; k < 3; ++k) {
        const float line = p0[k] + (p1[k] - p0[k]) * u;
        const float curve = h00 * p0[k] + h10 * (v0[k] * dtSeconds) + h01 * p1[k] +
                            h11 * (v1[k] * dtSeconds);
        const float off = curve - line;
        // An overshoot this large is a disagreement between the positions and the velocities, not
        // a curve: two samples 22 ms apart are never half a metre off their own chord.
        const bool overshot = off > kMaxHermiteDeviation || off < -kMaxHermiteDeviation;
        out[k] = overshot ? line : curve;
        clamped = clamped || overshot;
    }
    return clamped;
}

bool SampleBodyAt(const BodyState* frames, const uint64_t* times, size_t n,
                  SeriesCursor& cursor, int64_t t_us, BodyState& out, bool reversed, bool hermite,
                  HermiteTally* tally) {
    const size_t i = cursor.Resolve(times, n, t_us);
    if (i == kNoRecord) {
        return false;   // E-P4: not yet present. Absent, never frozen at its first pose.
    }
    const BodyState& a = frames[i];
    if (i + 1 >= n) {
        out = a;        // at or past the newest sample: hold it. We never extrapolate.
        if (reversed) {
            NegateVec3(out.vel);
            NegateVec3(out.angVel);
        }
        return true;
    }
    const BodyState& b = frames[i + 1];
    float u = Alpha(times[i], times[i + 1], t_us);
    // Hold `a` until `b`'s own instant, then take `b` whole. The orientation rides the same switch:
    // an identity quaternion blended into a spinning disc is the rotational half of the same slide.
    const bool teleport = IsTeleport(a.com, b.com, times[i], times[i + 1]);
    if (teleport) {
        u = u < 1.0f ? 0.0f : 1.0f;
    }

    const int64_t bracketUs = (int64_t)times[i + 1] - (int64_t)times[i];
    // Each guard is read ONCE and both the decision and the reason come off that reading: a second
    // set of conditions written for the counter would be free to disagree with the interpolant it
    // claims to describe.
    const bool usable_bracket = bracketUs > 0 && bracketUs <= kMaxHermiteBracketUs;
    const bool usable_tangents = UsableTangent(a.vel) && UsableTangent(b.vel);
    if (hermite && !teleport && usable_bracket && usable_tangents) {
        const bool clamped = HermiteVec3(a.com, b.com, a.vel, b.vel,
                                         (float)((double)bracketUs * 1e-6), u, out.com);
        if (tally != nullptr) {
            ++(clamped ? tally->clamp : tally->cubic);
        }
    } else {
        LerpVec3(a.com, b.com, u, out.com);
        // With the curve off the line is what was asked for, so it is not a fallback and has no
        // reason. Only a refused bracket is counted against one.
        if (hermite && tally != nullptr) {
            ++(teleport ? tally->teleport : !usable_bracket ? tally->gap : tally->velocity);
        }
    }
    LerpVec3(a.vel, b.vel, u, out.vel);
    LerpVec3(a.angVel, b.angVel, u, out.angVel);
    LerpVec3(a.lastHeldCom, b.lastHeldCom, u, out.lastHeldCom);
    SlerpQuat(a.ori, b.ori, u, out.ori);
    out.bits11 = a.bits11;   // ★ HELD, not blended. The older sample's word, as the engine does.
    out.owner_slot = a.owner_slot;   // and possession is a level, so it steps at its own sample
    if (reversed) {
        NegateVec3(out.vel);
        NegateVec3(out.angVel);
    }
    return true;
}

bool SamplePlayerAt(const PlayerState* frames, const uint64_t* times, size_t n,
                    SeriesCursor& cursor, int64_t t_us, PlayerState& out, bool reversed) {
    const size_t i = cursor.Resolve(times, n, t_us);
    if (i == kNoRecord) {
        return false;
    }
    const PlayerState& a = frames[i];
    if (i + 1 >= n) {
        out = a;
        if (reversed) {
            NegateVec3(out.refVel);
        }
        return true;
    }
    const PlayerState& b = frames[i + 1];
    const float u = Alpha(times[i], times[i + 1], t_us);

    SlerpQuat(a.hmdOri, b.hmdOri, u, out.hmdOri);
    SlerpQuat(a.lHandOri, b.lHandOri, u, out.lHandOri);
    SlerpQuat(a.rHandOri, b.rHandOri, u, out.rHandOri);
    LerpVec3(a.lHandOff, b.lHandOff, u, out.lHandOff);
    LerpVec3(a.rHandOff, b.rHandOff, u, out.rHandOff);
    LerpVec3(a.refVel, b.refVel, u, out.refVel);
    out.bendBack = a.bendBack + (b.bendBack - a.bendBack) * u;
    out.bendFwd = a.bendFwd + (b.bendFwd - a.bendFwd) * u;
    out.restPoseHeight = a.restPoseHeight + (b.restPoseHeight - a.restPoseHeight) * u;
    out.trigL = a.trigL + (b.trigL - a.trigL) * u;
    out.trigR = a.trigR + (b.trigR - a.trigR) * u;

    // ★ The discrete four. holdingontouserid in particular MUST hold: a blended grab id is not a
    // value the client ever had, and N-2's sentinel idiom depends on the word changing only when the
    // recording says it changed.
    out.bits28 = a.bits28;
    out.holdL = a.holdL;
    out.holdR = a.holdR;
    out.anchor = a.anchor;
    if (reversed) {
        NegateVec3(out.refVel);
    }
    return true;
}

}  // namespace playback
}  // namespace replay
