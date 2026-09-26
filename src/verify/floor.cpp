#include "verify/floor.h"

#include "foundation/bitio/bitio.h"
#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace replay {
namespace verify {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool NameEq(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

const bitio::WireField* FindField(const bitio::WireField* table, size_t n, const char* name) {
    for (size_t i = 0; i < n; ++i) {
        if (NameEq(table[i].name, name)) {
            return &table[i];
        }
    }
    return nullptr;
}

void Normalise4(float q[4]) {
    double n = sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
    if (n <= 0.0) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    for (int i = 0; i < 4; ++i) {
        q[i] = float(double(q[i]) / n);
    }
}

// Hamilton product, rotation-of-a-rotation: out = delta * q.
void Compose(const float delta[4], const float q[4], float out[4]) {
    const double dx = delta[0], dy = delta[1], dz = delta[2], dw = delta[3];
    const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    out[0] = float(dw * qx + dx * qw + dy * qz - dz * qy);
    out[1] = float(dw * qy - dx * qz + dy * qw + dz * qx);
    out[2] = float(dw * qz + dx * qy - dy * qx + dz * qw);
    out[3] = float(dw * qw - dx * qx - dy * qy - dz * qz);
    Normalise4(out);
}

void AxisAngle(const float axis[3], double deg, float out[4]) {
    double n = sqrt(double(axis[0]) * axis[0] + double(axis[1]) * axis[1] + double(axis[2]) * axis[2]);
    if (n <= 0.0) {
        n = 1.0;
    }
    const double half = deg * kPi / 360.0;
    const double s = sin(half) / n;
    out[0] = float(axis[0] * s);
    out[1] = float(axis[1] * s);
    out[2] = float(axis[2] * s);
    out[3] = float(cos(half));
}

// ★ The ENGINE's quantiser, not our storage codec's entry point, and the distinction is the whole
// measurement.  bitio::BitWriter::PackQuat10 REFUSES a quaternion that is not already on the wire
// grid -- W1-A's losslessness rule, that a value we sampled ourselves must never be squeezed into
// quat10 -- so feeding it a perturbed orientation returns a refusal rather than a rounded value.
// That refusal is correct and is exactly why it is the wrong instrument here: the question this
// floor asks is what the ENGINE's own BitPackQuat does to a perturbation of a given size, and
// QuantQuat/DequantQuat are W1-A's mirrors of the two functions at 0x141527C40 and 0x141527350,
// re-derived instruction for instruction from the binary.  Measuring through the storage guard
// instead would have reported "the codec refuses" as though it were "the gate cannot see it".
bool RoundTripQuat(const bitio::FieldSpec& spec, const float in[4], float out[4]) {
    bitio::Quat10 packed;
    if (bitio::QuantQuat(in, spec.rawBits, packed) != bitio::WireStatus::Ok) {
        return false;
    }
    return bitio::DequantQuat(packed, spec.rawBits, out) == bitio::WireStatus::Ok;
}

bool RoundTripReal(const bitio::FieldSpec& spec, float in, float* out) {
    uint64_t index = 0;
    if (bitio::QuantReal(in, spec.min, spec.max, spec.step, index) != bitio::WireStatus::Ok) {
        return false;
    }
    *out = bitio::DequantReal(index, spec.min, spec.step);
    return true;
}

bool QuatBitsDiffer(const float a[4], const float b[4]) {
    return memcmp(a, b, 4 * sizeof(float)) != 0;
}

}  // namespace

OriFloorResult MeasureOrientationFloor(const float refQuat[4], const float axis[3], const char* field,
                                       const O3Config& cfg) {
    OriFloorResult r;
    memset(&r, 0, sizeof(r));
    r.gateToleranceDeg = cfg.oriToleranceDeg;

    const bitio::WireField* wf =
        FindField(bitio::kPlayerStateWire, bitio::kPlayerStateWireCount, field);
    if (wf == nullptr || wf->cls != bitio::FieldClass::WireQuat) {
        snprintf(r.refusal, sizeof(r.refusal),
                 "no WireQuat field named '%s' in bitio::kPlayerStateWire; the floor refuses rather "
                 "than measuring some other field",
                 field);
        return r;
    }
    const bitio::FieldSpec spec = bitio::SpecOf(*wf, 0);
    if (bitio::FieldRefusal(spec) != nullptr) {
        snprintf(r.refusal, sizeof(r.refusal), "the codec refuses '%s': %s", field,
                 bitio::FieldRefusal(spec));
        return r;
    }

    float ref[4] = {refQuat[0], refQuat[1], refQuat[2], refQuat[3]};
    Normalise4(ref);

    float decRef[4];
    if (!RoundTripQuat(spec, ref, decRef)) {
        snprintf(r.refusal, sizeof(r.refusal), "the reference orientation did not survive the codec");
        return r;
    }

    // The positive control for every "no difference" below: the unperturbed pair, through the same
    // path, with the same metric.  It must read exactly zero, and the rungs prove the same metric
    // can read non-zero.  Without it, a metric stuck at 0 would report a perfect floor.
    r.baselineDeg = QuatAngleDeg(decRef, decRef);
    if (r.baselineDeg != 0.0f) {
        snprintf(r.refusal, sizeof(r.refusal),
                 "the unperturbed baseline read %.9g deg, not 0; the metric is not measuring what it "
                 "is being asked to",
                 double(r.baselineDeg));
        return r;
    }

    // The step, bisected rather than transcribed.  Lower bound: a perturbation that the codec maps
    // back onto the reference bit for bit.  Upper bound: one that does not.
    double lo = 0.0, hi = 4.0;
    float probe[4], dq[4], dec[4];
    AxisAngle(axis, hi, dq);
    Compose(dq, ref, probe);
    if (!RoundTripQuat(spec, probe, dec) || !QuatBitsDiffer(dec, decRef)) {
        snprintf(r.refusal, sizeof(r.refusal),
                 "a %.1f deg perturbation did not move the encoded quaternion; the sweep has no upper "
                 "bound and cannot report a step",
                 hi);
        return r;
    }
    for (int it = 0; it < 60; ++it) {
        const double mid = 0.5 * (lo + hi);
        AxisAngle(axis, mid, dq);
        Compose(dq, ref, probe);
        if (RoundTripQuat(spec, probe, dec) && QuatBitsDiffer(dec, decRef)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    r.measuredStepDeg = float(hi);

    static const float kLadder[] = {0.05f, 0.1f, 0.2f, 0.5f, 1.0f};
    r.rungCount = uint32_t(sizeof(kLadder) / sizeof(kLadder[0]));
    for (uint32_t i = 0; i < r.rungCount; ++i) {
        AxisAngle(axis, double(kLadder[i]), dq);
        Compose(dq, ref, probe);
        FloorRung& rung = r.rungs[i];
        rung.commandedDeg = kLadder[i];
        if (!RoundTripQuat(spec, probe, dec)) {
            rung.observedDeg = -1.0f;
            rung.gateRed = false;
        } else {
            rung.observedDeg = QuatAngleDeg(decRef, dec);
            rung.gateRed = rung.observedDeg > cfg.oriToleranceDeg;
        }
        if (rung.gateRed) {
            if (r.firstRedDeg == 0.0f) {
                r.firstRedDeg = rung.commandedDeg;
            }
        } else {
            ++r.missedRungs;
        }
    }

    r.valid = true;
    return r;
}

PosFloorResult MeasurePositionFloor(const float refPos[3], const char* field, const O3Config& cfg) {
    PosFloorResult r;
    memset(&r, 0, sizeof(r));
    r.gateToleranceM = cfg.posToleranceM;

    const bitio::WireField* wf = FindField(bitio::kSimStateWire, bitio::kSimStateWireCount, field);
    if (wf == nullptr || wf->cls != bitio::FieldClass::WireQ) {
        snprintf(r.refusal, sizeof(r.refusal), "no WireQ field named '%s' in bitio::kSimStateWire",
                 field);
        return r;
    }
    const bitio::FieldSpec spec = bitio::SpecOf(*wf, 0);
    if (bitio::FieldRefusal(spec) != nullptr) {
        snprintf(r.refusal, sizeof(r.refusal), "the codec refuses '%s': %s", field,
                 bitio::FieldRefusal(spec));
        return r;
    }

    float decRef = 0.0f;
    if (!RoundTripReal(spec, refPos[0], &decRef)) {
        snprintf(r.refusal, sizeof(r.refusal), "the reference position did not survive the codec");
        return r;
    }

    double lo = 0.0, hi = 1.0;
    float dec = 0.0f;
    if (!RoundTripReal(spec, refPos[0] + float(hi), &dec) || dec == decRef) {
        snprintf(r.refusal, sizeof(r.refusal),
                 "a %.3f m displacement did not move the encoded value; no upper bound", hi);
        return r;
    }
    for (int it = 0; it < 60; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (RoundTripReal(spec, refPos[0] + float(mid), &dec) && dec != decRef) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    r.measuredStepM = float(hi);
    r.gateBelowWireStep = r.measuredStepM > cfg.posToleranceM;
    r.valid = true;
    return r;
}

}  // namespace verify
}  // namespace replay
