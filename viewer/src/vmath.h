#pragma once
#include <math.h>

namespace erv {

struct V3 {
    float x = 0, y = 0, z = 0;
};
struct Q4 {
    float x = 0, y = 0, z = 0, w = 1;
};

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return sqrtf(Dot(a, a)); }
inline V3 Norm(V3 a) {
    const float l = Len(a);
    return l > 1e-6f ? a * (1.f / l) : V3{0, 0, 1};
}
inline V3 Lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

inline V3 Rotate(Q4 q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    const V3 t = Cross(u, v) * 2.f;
    return v + t * q.w + Cross(u, t);
}
inline Q4 Slerp(Q4 a, Q4 b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) b = {-b.x, -b.y, -b.z, -b.w}, d = -d;
    if (d > 0.9995f) {
        Q4 r{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        const float l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
        return {r.x / l, r.y / l, r.z / l, r.w / l};
    }
    const float th = acosf(d), s = sinf(th);
    const float wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

// a then b applied to a vector is Mul(b, a): Rotate(Mul(b, a), v) == Rotate(b, Rotate(a, v)).
inline Q4 Mul(Q4 a, Q4 b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q4 Conj(Q4 q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Q4 QNorm(Q4 q) {
    const float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-8f ? Q4{q.x / l, q.y / l, q.z / l, q.w / l} : Q4{};
}
inline Q4 AxisAngle(V3 axis, float a) {
    const V3 n = Norm(axis);
    const float s = sinf(a * 0.5f);
    return {n.x * s, n.y * s, n.z * s, cosf(a * 0.5f)};
}
// The shortest rotation taking direction a to direction b.
inline Q4 FromTo(V3 a, V3 b) {
    a = Norm(a), b = Norm(b);
    const float d = Dot(a, b);
    if (d < -0.9999f) {
        V3 ax = Cross(V3{1, 0, 0}, a);
        if (Len(ax) < 1e-3f) ax = Cross(V3{0, 1, 0}, a);
        return AxisAngle(ax, 3.14159265f);
    }
    const V3 c = Cross(a, b);
    return QNorm({c.x, c.y, c.z, 1.f + d});
}
// The rotation whose columns are x, y, z (an orthonormal right-handed basis).
inline Q4 FromBasis(V3 x, V3 y, V3 z) {
    const float tr = x.x + y.y + z.z;
    Q4 q;
    if (tr > 0) {
        const float s = sqrtf(tr + 1.f) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = sqrtf(1.f + x.x - y.y - z.z) * 2;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = sqrtf(1.f + y.y - x.x - z.z) * 2;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = sqrtf(1.f + z.z - x.x - y.y) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return QNorm(q);
}

}  // namespace erv
