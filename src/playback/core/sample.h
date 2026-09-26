#pragma once

// W2-B. Sampling the recorded stream at the playhead: our own lerp, slerp and hold-last.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY WE INTERPOLATE AND THE ENGINE DOES NOT, which is F5 and is the reason this file exists.
//
// The design A3 clause 2 says the engine's interpolator makes reverse and off-tick seek "free by
// construction", and S7.2.2 says UpdateTargetState owns the transform "with the engine's own
// lerp/slerp intact". Read against ENTITY.UPDATE_TARGET_STATE those sentences are false in OUR
// environment: four IS_HOST tests gate the interpolate and extrapolate branches, offline we are
// unavoidably the host, and what reaches the body is the head history node VERBATIM.
//
// So the interpolation is ours. That is not a compromise -- the player channel already did it (M1,
// "our own lerp/slerp over the recorded stream", because the authority arm short-circuits
// UpdateInterpolatedPlayerStates), and doing it for bodies too makes the two channels ONE mechanism
// instead of two. It is also direction-agnostic and off-tick by construction, which is what the
// owner's "fast forward and back" needs and what an engine interpolator driven by a peer clock is not.
//
// ★ THE DISCRETE FIELDS ARE HELD, NEVER BLENDED, and that is not a simplification. R6 S3.3 records
// that the engine copies the `bits` dword rather than blending it, and ENTITY.INTERPOLATE_PHYS_STATE
// does the same: its lerp arm copies the OLDER node's first 16 bytes verbatim and interpolates only
// the vectors after them. "The action word at time t is the last one recorded at or before t" is both
// what the engine does and what a replay needs -- a half-punch is not a state the client ever held.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "engine/entity.h"
#include "playback/core/resolve.h"

namespace replay {
namespace playback {

// Shortest-path quaternion slerp, normalised, with the small-angle case falling back to a normalised
// lerp. `t` is clamped to [0,1] by the caller's construction but clamped again here, because a sample
// time outside the bracket is a bug that must not become an extrapolated pose.
void SlerpQuat(const float a[4], const float b[4], float t, float out[4]);

void LerpVec3(const float a[3], const float b[3], float t, float out[3]);

// Which interpolant each bracket got, and -- when it was the straight line -- which guard refused
// the curve. Plain counters the caller owns and keeps: a sample costs one increment, and a reason
// is only recorded while the curve is armed, because with it off the line is what was asked for.
struct HermiteTally {
    uint32_t cubic = 0;      // interpolated by the curve
    uint32_t teleport = 0;   // the bracket is a jump: position is held across it, never blended
    uint32_t gap = 0;        // longer than three sample periods (or non-positive): a hole, not a bracket
    uint32_t velocity = 0;   // a tangent is not a velocity: NaN, or faster than anything moves
    uint32_t clamp = 0;      // the curve left its own chord, so at least one axis took the line
    uint32_t Lerped() const { return teleport + gap + velocity + clamp; }
};

// Sample a body at `t`. `times` must be non-decreasing. The cursor is the caller's, so a steady-state
// frame costs a compare and a lerp; it is invalidated by the caller on a discontinuity, never here.
//
// Returns false when there is no sample at or before `t` -- the caller must then NOT publish, because
// an entity that had not yet joined must be absent rather than frozen at its first pose (E-P4).
// ★ `reversed` NEGATES the sampled velocities and nothing else. A recorded velocity is d(pose)/dt
// along the recording; play the recording backwards and the body is genuinely moving the other way,
// so handing the engine the forward vector points every velocity consumer 180 degrees wrong.
// Negated, never zeroed: a zero would lose the attached-velocity edge the engine reads out of the
// player's bits28, and "not moving" is a different claim from "moving backwards".
// `hermite` interpolates POSITION with a cubic through the recorded velocities instead of a
// straight line, which is what takes the corner off the motion between two 45 Hz samples.
// Orientation keeps its slerp. Defaulted off so every caller that has not asked for it -- and
// every case written against the line -- keeps the interpolant it was written against.
// `tally`, when given, counts this bracket against the interpolant it got.
bool SampleBodyAt(const BodyState* frames, const uint64_t* times, size_t n,
                  SeriesCursor& cursor, int64_t t_us, BodyState& out, bool reversed = false,
                  bool hermite = false, HermiteTally* tally = nullptr);

// Same for a player. Continuous fields blend; bits28, holdL, holdR and anchor hold last.
bool SamplePlayerAt(const PlayerState* frames, const uint64_t* times, size_t n,
                    SeriesCursor& cursor, int64_t t_us, PlayerState& out, bool reversed = false);

}  // namespace playback
}  // namespace replay
