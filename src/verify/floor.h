#pragma once

// W2-I.  The measured sensitivity floor.
//
// "Every oracle publishes a measured sensitivity floor including a defect it is shown to MISS.  An
// oracle with no known blind spot has not been characterised."
//
// S12.3 names the experiment: perturb one hand orientation by 0.05, 0.1, 0.2, 0.5 and 1.0 degrees and
// record at which magnitude the gate first goes red, with the DESIGN PREDICTING that 0.05 deg is
// missed because it is below the wire quantisation step -- and stating that a run where nothing is
// missed means the gate is mis-scaled and is itself a finding.
//
// --------------------------------------------------------------------------------------------
// ★ NOTHING HERE IS A WRITTEN-DOWN CONSTANT.
//
// The quantisation step is MEASURED, at the reference orientation actually supplied, by bisecting for
// the smallest perturbation whose decoded quaternion differs bitwise from the decoded reference.  The
// S12.4 defect -- 48000/512 written into an oracle as though it were a constant of the engine, so that
// a green mechanism failed a red oracle on ordinary hardware -- is the same shape: a floor of "about
// 0.1 to 0.16 degrees" transcribed from prose would be a number nobody could falsify, and the
// smallest representable rotation is not one number anyway.  It depends on where on the sphere the
// reference sits and which way the perturbation goes, which is exactly why it is swept.
//
// The wire parameters come from bitio::kPlayerStateWire and kSimStateWire BY FIELD NAME.  No range,
// step or width is spelled here.  A field that is renamed or removed makes the measurement REFUSE
// (NoBaseline) rather than quietly measure the wrong thing.
//
// ★ WHAT THIS FLOOR IS, AND WHAT IT IS NOT.  It measures the gate's threshold against the transport:
// perturbation -> wire codec -> comparator -> SC2 tolerance.  It does NOT include the engine's solver
// stack, which stands between a perturbed hand orientation and the bones O3 actually compares.  The
// solver can only amplify or attenuate, and which it does is unmeasured until the T3 sweep.  So the
// number below is a floor on the WIRE-AND-COMPARATOR path and is reported as such; O3's in-vivo floor
// is a separate measurement that cannot be taken offline, and claiming this one is that one would be
// the "probe validated my assumption, not reality" failure.
// --------------------------------------------------------------------------------------------

#include "verify/o3.h"

namespace replay {
namespace verify {

struct FloorRung {
    float commandedDeg;   // the perturbation asked for
    float observedDeg;    // what survived the wire codec, measured with the gate's own metric
    bool  gateRed;        // observedDeg > O3Config::oriToleranceDeg
};

struct OriFloorResult {
    bool  valid;
    char  refusal[128];        // non-empty iff !valid

    float measuredStepDeg;     // the smallest perturbation the wire can represent, here, MEASURED
    float baselineDeg;         // the unperturbed pair through the same path: must be exactly 0
    float gateToleranceDeg;    // echoed so the record carries what it was judged against

    uint32_t  rungCount;
    FloorRung rungs[8];

    float    firstRedDeg;      // the smallest commanded perturbation the gate saw; 0 if none
    uint32_t missedRungs;      // commanded perturbations the gate did NOT see
};

// `refQuat` is the reference orientation (need not be normalised); `axis` the rotation axis of the
// perturbation (need not be normalised).  `field` names a WireQuat field in bitio::kPlayerStateWire.
OriFloorResult MeasureOrientationFloor(const float refQuat[4], const float axis[3],
                                       const char* field, const O3Config&);

struct PosFloorResult {
    bool  valid;
    char  refusal[128];
    float measuredStepM;       // smallest displacement the wire can represent, MEASURED
    float gateToleranceM;
    bool  gateBelowWireStep;   // true when the tolerance is finer than anything the wire can carry
};

// `field` names a WireQ field in bitio::kSimStateWire (com, lastheldcom, v, w).
PosFloorResult MeasurePositionFloor(const float refPos[3], const char* field, const O3Config&);

}  // namespace verify
}  // namespace replay
