#include "import/basis.h"

#include <math.h>

namespace replay::import {
namespace {

constexpr double kMinLength = 1e-6;
constexpr double kMirrorMinDot = 0.9;  // §5.2 step 2

struct V3 {
    double x, y, z;
};

V3 Load(const float v[3]) { return {v[0], v[1], v[2]}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Scale(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }

bool Normalize(V3& v) {
    const double length = sqrt(Dot(v, v));
    if (!(length > kMinLength)) return false;  // also rejects NaN
    v = Scale(v, 1.0 / length);
    return true;
}

}  // namespace

BasisVerdict BasisToQuat(const float forward[3], const float up[3], const float left[3], float q[4]) {
    V3 f = Load(forward);
    V3 apiLeft = Load(left);
    if (!Normalize(f) || !Normalize(apiLeft)) return BasisVerdict::Degenerate;
    V3 u = Load(up);
    u = Sub(u, Scale(f, Dot(u, f)));
    if (!Normalize(u)) return BasisVerdict::Degenerate;
    const V3 l = Cross(u, f);  // unit: u and f are orthonormal
    if (Dot(l, apiLeft) < kMirrorMinDot) return BasisVerdict::Mirrored;

    // Rotation matrix with columns [l | u | f]; m[row][col].
    const double m00 = l.x, m01 = u.x, m02 = f.x;
    const double m10 = l.y, m11 = u.y, m12 = f.y;
    const double m20 = l.z, m21 = u.z, m22 = f.z;
    const double trace = m00 + m11 + m22;
    double x, y, z, w;
    if (trace >= m00 && trace >= m11 && trace >= m22) {
        const double s = 2.0 * sqrt(1.0 + trace);
        w = 0.25 * s;
        x = (m21 - m12) / s;
        y = (m02 - m20) / s;
        z = (m10 - m01) / s;
    } else if (m00 >= m11 && m00 >= m22) {
        const double s = 2.0 * sqrt(1.0 + m00 - m11 - m22);
        w = (m21 - m12) / s;
        x = 0.25 * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 >= m22) {
        const double s = 2.0 * sqrt(1.0 + m11 - m00 - m22);
        w = (m02 - m20) / s;
        x = (m01 + m10) / s;
        y = 0.25 * s;
        z = (m12 + m21) / s;
    } else {
        const double s = 2.0 * sqrt(1.0 + m22 - m00 - m11);
        w = (m10 - m01) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25 * s;
    }
    const double sign = w < 0.0 ? -1.0 : 1.0;  // one canonical sign keeps output deterministic
    q[0] = static_cast<float>(x * sign);
    q[1] = static_cast<float>(y * sign);
    q[2] = static_cast<float>(z * sign);
    q[3] = static_cast<float>(w * sign);
    return BasisVerdict::Ok;
}

void AlignHemisphere(const float previous[4], float q[4]) {
    const float dot = previous[0] * q[0] + previous[1] * q[1] + previous[2] * q[2] + previous[3] * q[3];
    if (dot < 0.0f) {
        for (int i = 0; i < 4; ++i) q[i] = -q[i];
    }
}

}  // namespace replay::import
