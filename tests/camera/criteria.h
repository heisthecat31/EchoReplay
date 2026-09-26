#pragma once

// The arithmetic behind W1-H's probe criteria, factored out of probe_camera.cpp so that
// tests/camera can drive every one of them offline with BOTH a construction that passes and a
// construction that fails.
//
// ★ That is the whole reason this header exists. The audit rejected an assertion --
// `gateReasserts == 0` after the first frame -- that could never pass, against a counter that is
// cumulative and is reset only in Init(). The defect was not in the engine or in the probe's
// plumbing; it was in a criterion nobody had tried to satisfy. A criterion whose passing input and
// whose failing input have both been constructed cannot have that defect, and constructing them
// needs the arithmetic to be reachable without a running engine.
//
// No Windows, no engine, no floating-point library beyond <cmath>: this compiles into the offline
// test unchanged.

#include <stdint.h>

#include <cmath>

namespace replay {
namespace camera_criteria {

// Three outcomes, in the order the harness aggregates them: FAIL if anything evaluable failed,
// else NOT-RUN if anything was undetermined, else PASS.
enum class Outcome : uint8_t { Ok, Failed, Undetermined };

// The two arms are considered to agree on the viewport aspect within this much. The commit computes
// 2*atan(tan(fovY/2) * w/h) in float, so recovering the aspect back out costs a few ulp at each
// end; this is three orders of magnitude below the 4:3-versus-16:9 gap the number has to resolve.
inline constexpr float kAspectTolerance = 1e-3f;

// An aspect this close to 1.0 cannot be told apart from "no conversion happened at all", so a
// square viewport makes the FOV discriminator undecidable rather than failing.
inline constexpr float kSquareAspectBand = 5e-3f;

// P0 sits far above the arena; the baseline camera must be at least this far from it, or the later
// equalities prove nothing.
inline constexpr float kBaselineSeparationM = 50.0f;

inline bool SameVec3(const float a[3], const float b[3]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

inline bool SameVec4(const float a[4], const float b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

inline float Distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// aspect = tan(nodeFovH / 2) / tan(fovY / 2), inverting the conversion CAM.VIEW_COMMIT performs.
// Returns a negative number when the inputs cannot carry an aspect at all, so a caller reports
// COULD-NOT-DETERMINE rather than a wrong ratio.
inline float RecoverAspect(float nodeFovH, float fovY) {
    const float kPi = 3.14159265f;
    if (!(fovY > 0.0f) || !(fovY < kPi) || !(nodeFovH > 0.0f) || !(nodeFovH < kPi)) {
        return -1.0f;
    }
    const float denom = std::tan(fovY * 0.5f);
    if (!(denom > 1e-6f)) {
        return -1.0f;
    }
    return std::tan(nodeFovH * 0.5f) / denom;
}

// ★ THE CORRECTED GATE CRITERION. `Stats::gateReasserts` counts once per view per frame whenever
// viewstate+0x9C was found armed, and is reset only in Init(), so it is cumulative and
// monotonically non-decreasing. "== 0 after the first frame" is therefore unsatisfiable: every
// freshly constructed view-state has +0x9C == 1, so the first SetPose alone drives it to viewCount.
//
// What is checkable is the pair, and each half names its own failing input:
//
//   RISE  the first write must find the gate armed on at least one and at most `viewCount` views.
//         FAILS against a counter that cannot move -- a wrong offset, a field that is always zero,
//         an instrument that would have read zero forever while the assertion above called that a
//         pass.
//   FLAT  across the rest of the arm the counter must not move at all.
//         FAILS against an engine that re-arms the integrator, which is exactly what
//         CAM.ENTER_FREECAM does when it replaces every view-state object.
inline Outcome GateReassertOutcome(uint64_t before, uint64_t afterFirstWrite, uint64_t atArmEnd,
                                   uint32_t viewCount, bool armCompleted) {
    if (viewCount == 0 || !armCompleted || afterFirstWrite < before || atArmEnd < afterFirstWrite) {
        return Outcome::Undetermined;
    }
    const uint64_t rise = afterFirstWrite - before;
    const bool riseOk = rise >= 1 && rise <= viewCount;
    const bool flatOk = atArmEnd == afterFirstWrite;
    return (riseOk && flatOk) ? Outcome::Ok : Outcome::Failed;
}

// ★ The same shape for ci+0x274. Arm C clears the flag after every SetPose, so the next SetPose
// must find it clear and re-set it: the counter has to climb by roughly one per frame of that arm.
// Arm D leaves it alone, so it must then go flat but for the single re-set at the boundary.
//
// This is what turns a flat reading in arm A from an untested zero into a measurement: a counter
// that stayed flat through arm C as well would not be measuring anything, and its silence in arm A
// would mean nothing.
inline Outcome FreeCamInstrumentOutcome(uint64_t afterFirstWrite, uint64_t atCiArmEnd,
                                        uint64_t atEnd, uint32_t ciArmFrames, bool ciArmCompleted,
                                        bool recoveryCompleted) {
    if (!ciArmCompleted || !recoveryCompleted || ciArmFrames < 3 || atCiArmEnd < afterFirstWrite ||
        atEnd < atCiArmEnd) {
        return Outcome::Undetermined;
    }
    const uint64_t ciRise = atCiArmEnd - afterFirstWrite;
    const uint64_t recoveryRise = atEnd - atCiArmEnd;
    return (ciRise >= ciArmFrames - 2 && recoveryRise <= 1) ? Outcome::Ok : Outcome::Failed;
}

// ★ WHICH ARM DROVE CCameraNode[0], which is the question the whole packet turns on.
//
// On the view-state arm (ci+0x274 != 0) the commit converts our vertical FOV into a horizontal one
// with the live viewport, so the aspect recovered from node+0x98 is the viewport's and is the SAME
// at 55 degrees and at 90. On the CI arm (ci+0x274 == 0) CAM.SPECTATOR_UPDATE writes node+0x98
// straight out of ci+0x50 with no conversion at all, so the recovered aspect collapses to exactly
// 1.0 at every FOV. The two arms are each other's control and neither reading could pass in the
// other's place.
//
// A genuinely square viewport makes the two indistinguishable. That is undecidable, not a failure.
inline Outcome ViewStateArmOutcome(float aspectAtFovA, float aspectAtFovB) {
    if (!(aspectAtFovA > 0.0f) || !(aspectAtFovB > 0.0f)) {
        return Outcome::Undetermined;
    }
    if (std::fabs(aspectAtFovA - 1.0f) < kSquareAspectBand) {
        return Outcome::Undetermined;
    }
    return std::fabs(aspectAtFovA - aspectAtFovB) < kAspectTolerance ? Outcome::Ok
                                                                     : Outcome::Failed;
}

inline Outcome CiArmOutcome(float aspectOnCiArm, float aspectOnViewStateArm) {
    if (!(aspectOnCiArm > 0.0f) || !(aspectOnViewStateArm > 0.0f)) {
        return Outcome::Undetermined;
    }
    // If the viewport really is square there is nothing to tell apart.
    if (std::fabs(aspectOnViewStateArm - 1.0f) < kSquareAspectBand) {
        return Outcome::Undetermined;
    }
    return std::fabs(aspectOnCiArm - 1.0f) < kAspectTolerance ? Outcome::Ok : Outcome::Failed;
}

inline Outcome BaselineControlOutcome(bool baselineValid, const float baselineNodePos[3],
                                      const float p0[3]) {
    if (!baselineValid) {
        return Outcome::Undetermined;
    }
    return Distance(baselineNodePos, p0) > kBaselineSeparationM ? Outcome::Ok : Outcome::Failed;
}

// The harness's own aggregation rule, pushed down to the run: FAIL if anything evaluable failed,
// else NOT-RUN if anything was undetermined, else PASS.
inline const char* AggregateVerdict(const Outcome* outcomes, size_t count) {
    bool failed = false;
    bool unknown = false;
    for (size_t i = 0; i < count; ++i) {
        if (outcomes[i] == Outcome::Failed) {
            failed = true;
        } else if (outcomes[i] == Outcome::Undetermined) {
            unknown = true;
        }
    }
    return failed ? "FAIL" : (unknown ? "NOT-RUN" : "PASS");
}

inline const char* OutcomeName(Outcome o) {
    switch (o) {
        case Outcome::Ok:           return "yes";
        case Outcome::Failed:       return "NO";
        case Outcome::Undetermined: return "COULD-NOT-DETERMINE";
    }
    return "COULD-NOT-DETERMINE";
}

}  // namespace camera_criteria
}  // namespace replay
