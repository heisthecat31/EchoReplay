#pragma once

// W2-I.  O3 -- the skeleton oracle.  The primary numeric proof of pose fidelity.
//
// What it compares: the fully solved skeleton per player, captured live and re-sampled at matched
// SOURCE timestamps during replay.  Nothing in the capture path authors bone data; the bones are the
// output of the engine's IK, blender, spring and helmet solvers, so agreement means the whole solver
// stack consumed the same inputs in the same order with the same dt.
//
// --------------------------------------------------------------------------------------------
// FOUR DECISIONS THAT ARE NOT OBVIOUS, EACH WITH THE DEFECT CLASS IT ANSWERS
//
// 1. ★ THE BONE COUNT IS DERIVED, NEVER 23.  S5.4.7 and S12.3 both write "23 bones/player,
//    bone_o[92], bone_t[69]".  The builder does not.  sub_140165D60 emits
//    `user_bones[%u]|bone_o[%llu]` in a loop whose trip count is a RUNTIME call
//    (ORACLE.BONES_COUNT_CALL_SITE), advancing the orientation index by 4 and the translation index
//    by 3 per bone.  An oracle asserting 92 would go red on a correct response from a skeleton with
//    any other bone count -- the S12.4 defect exactly, where 48000/512 was written into an oracle as
//    though it were a constant of the engine.  So O3 reads boneCount out of the data and asserts the
//    RELATION 4N/3N, which is the part the binary actually guarantees.
//
// 2. ★ MATCHING IS BY SOURCE TIME, AND NEVER BY INDEX.  Comparing live[i] against replay[i] would
//    pass on a stream that is uniformly one frame late -- which is one of S12.3's own mutations.
//    Unmatched instants on either side are reported, not dropped.
//
// 3. ★ THE NO-SKEW ASSERTION IS OVER TWO INDEPENDENT READS, NOT ONE COPIED STAMP.  If E-7 stamped
//    once and wrote that stamp into both snapshots, "both snapshots carry the identical sim_time_us"
//    would be a tautology that no mutation could reach.  SkeletonInstant therefore carries the clock
//    read taken BEFORE the /session builder and the one taken AFTER the /player_bones builder, plus
//    the frame index at each -- two domains, the CClock accumulator and CncaGame's frame counter,
//    neither derived from the other.  A tick boundary between the two builders moves both.
//
// 4. ★ SOURCE TIME IS CROSS-CHECKED AGAINST SIM TIME.  W1-D found the engine's clock is beaten three
//    times per frame and that `scaledDeltaUs` is beat 1 alone; a producer that integrates it
//    under-reports elapsed time.  A stream whose source-time span disagrees with its own sim-time
//    span by more than the configured fraction is refused as ClockDrift -- the producer measured with
//    the wrong instrument, and every fidelity number computed from it is void rather than red.
// --------------------------------------------------------------------------------------------

#include "verify/verify.h"

namespace replay {
namespace verify {

// One player's solved skeleton at one instant.  `boneO` is 4*boneCount floats (x,y,z,w per bone, in
// the builder's own emission order) and `boneT` is 3*boneCount.  The oracle does not own either.
struct BoneFrame {
    uint32_t     playerId;
    uint32_t     boneCount;
    const float* boneO;
    const float* boneT;
};

// One tick's worth of oracle tap output.  The two clock readings bracket the two builder calls.
struct SkeletonInstant {
    uint64_t          sourceTimeUs;      // the matching key; never wall clock
    uint64_t          simTimeBeforeUs;   // CClock+0x28 read before the /session builder ran
    uint64_t          simTimeAfterUs;    // ... and again after the /player_bones builder returned
    uint32_t          frameIndexBefore;  // CncaGame+0x830 at the same two points: a second domain
    uint32_t          frameIndexAfter;
    uint32_t          playerCount;
    const BoneFrame*  players;
};

struct SkeletonStream {
    SourceTag              tag;
    const SkeletonInstant* instants;
    uint32_t               count;
};

struct O3Config {
    float posToleranceM;      // SC2: 0.5 mm, the engine's own 1 mm position quantisation floor
    float oriToleranceDeg;    // SC2: 0.1 deg, ~ the 10-bit-per-component quaternion floor
    float minPassRatio;       // SC2: 0.95 of sampled instants
    // The two clocks inside one recording, compared as a dimensionless ratio.  Nothing device
    // specific enters here: it is source-time span against the engine's own sim-time span.
    float maxClockDriftFrac;
};
O3Config O3Defaults();

struct O3Outlier {
    float    value;         // metres, or degrees
    uint32_t player;
    uint32_t bone;
    uint64_t sourceTimeUs;
    bool     seen;
};

struct O3Result {
    Verdict      verdict;
    NotRunReason reason;

    uint32_t matchedInstants;
    uint32_t liveOnlyInstants;
    uint32_t replayOnlyInstants;
    uint32_t passedInstants;      // every bone inside both tolerances
    uint32_t skewInstants;        // the clock or the frame index moved across the two builders

    uint64_t comparedBones;       // THE DENOMINATOR.  Pass is unreachable while this is zero.
    uint64_t posViolations;
    uint64_t oriViolations;

    O3Outlier maxPos;
    O3Outlier maxOri;

    // Reported, never asserted.  A replay that is the identical computation SHOULD reproduce bones
    // bit for bit, so 100% here is the goal and not a fault -- but it is also what an accidentally
    // aliased comparison looks like, so the number is always in the record beside the aliasing and
    // SourceTag refusals that are the actual enforcement.
    uint64_t bitIdenticalBones;

    char detail[192];
};

O3Result O3Compare(const SkeletonStream& live, const SkeletonStream& replay, const O3Config&);

// The geodesic angle between two unit quaternions in degrees, treating q and -q as one rotation.
// Exposed because the sensitivity ladder measures with the same function the gate judges with; two
// implementations of "how far apart are these" is how a floor stops describing the gate.
float QuatAngleDeg(const float a[4], const float b[4]);

}  // namespace verify
}  // namespace replay
