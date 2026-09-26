#pragma once

// The bracket arithmetic sample.cpp and smooth.cpp share: one definition, so the smoothed path and
// the 1:1 path can never disagree about what a teleport, a gap or a usable tangent is.

#include <stdint.h>

namespace replay {
namespace playback {
namespace sample_internal {

inline float Clamp01(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}

// The blend factor for a query inside [t0, t1]. A zero-width bracket returns 0 rather than dividing:
// duplicate stamps are legal in the file and must resolve to the older sample, not to a NaN pose.
// ★★ THE UNDERFLOW THIS USED TO SATURATE THROUGH. `(uint64_t)t - t0` with t below t0 wraps to
// ~1.8e19, the division gives an enormous ratio, and Clamp01 turns it into 1.0 -- so a broken
// precondition silently returned "fully at the NEWER sample", which is the furthest possible wrong
// answer and the one hardest to see. Clamping the result is not a guard; it is what hides the fault.
//
// The continuity is checked instead, in signed form, before any arithmetic: t below the bracket means
// the caller's cursor and this call disagree, and the honest answer is the OLDER sample.
inline float Alpha(uint64_t t0, uint64_t t1, int64_t t) {
    if (t1 <= t0) return 0.0f;
    if (t < 0 || (uint64_t)t <= t0) return 0.0f;
    if ((uint64_t)t >= t1) return 1.0f;
    const double span = (double)(t1 - t0);
    const double into = (double)((uint64_t)t - t0);
    return Clamp01((float)(into / span));
}

// ★★★ THE SPEED NO RECORDED MOTION REACHES, AND WHY BLENDING PAST IT IS A DEFECT.
//
// A round reset moves the disc from (-5.108, 7.322, 27.268) to the face-off point in 26 ms -- 28.9 m,
// ~1100 m/s -- and carries the players 140 m with it. The client that recorded this received an
// absolute position and SNAPPED; interpolating the pair instead draws a body sliding the length of
// the arena over one sample period, which is a motion nobody ever saw.
//
// 150 m/s is the ceiling because the capture's fastest disc is 32.3 m/s and a boosted player is
// slower still: there is a factor of four between the quickest real motion and the threshold, and
// another factor of seven between the threshold and the slowest teleport. Nothing legitimate is
// near it from either side.
constexpr float kTeleportSpeedMps = 150.0f;

// Squared, so the hot path pays one multiply-and-compare and no sqrt. Time-symmetric by
// construction -- it reads a distance and an interval, neither of which has a direction -- so a
// rewind snaps at exactly the instant a forward play does.
inline bool IsTeleport(const float a[3], const float b[3], uint64_t t0, uint64_t t1) {
    if (t1 <= t0) {
        return false;
    }
    const double dx = (double)b[0] - a[0];
    const double dy = (double)b[1] - a[1];
    const double dz = (double)b[2] - a[2];
    const double reach = (double)kTeleportSpeedMps * (double)(t1 - t0) * 1e-6;
    return dx * dx + dy * dy + dz * dz > reach * reach;
}

// ★ THE RECORDING IS 45 Hz AND THE PICTURE IS NOT. A straight line between two samples 22 ms apart
// makes a body that is really on a curve look like it is on a sequence of short straight segments,
// and the corner at every sample is what reads as stutter at slow speeds. The recorded velocities
// are already decoded beside the positions, which makes the curve free: a cubic Hermite through
// both samples with the recorded velocities as tangents.
//
// It is SYMMETRIC IN TIME by construction -- the tangents are the stored forward velocities and the
// bracket is the stored bracket, neither of which depends on which way the playhead is moving, so
// the same instant evaluates to the same position forwards and backwards. (`reversed` negates the
// OUTPUT velocity for the engine; it must not reach the interpolant.)
//
// Every guard below falls back to the straight line rather than to a guess: the curve is only
// better than the line when the tangents describe the same motion the endpoints do.
constexpr float kMaxHermiteSpeed = 150.0f;        // m/s; above this the sample is not a velocity
constexpr float kMaxHermiteDeviation = 0.5f;      // m from the line, before the curve is refused
constexpr int64_t kMaxHermiteBracketUs = 66667;   // 3 sample periods at 45 Hz: beyond it, a gap

inline bool UsableTangent(const float v[3]) {
    for (int k = 0; k < 3; ++k) {
        const float s = v[k] < 0.0f ? -v[k] : v[k];
        if (!(s <= kMaxHermiteSpeed)) {   // written positively so a NaN is refused, not admitted
            return false;
        }
    }
    return true;
}

}  // namespace sample_internal
}  // namespace playback
}  // namespace replay
