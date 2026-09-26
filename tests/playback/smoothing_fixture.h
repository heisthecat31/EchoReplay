#pragma once

// Deterministic synthetic tracks for the smoothing cases: one body and one player on a jittered 45 Hz
// grid. Kept free of smoothing types so test_smoothing_off.cpp compiles against the tree before them.

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "playback/core/window.h"

namespace smoothing_fixture {

using replay::BodyState;
using replay::PlayerState;
using replay::playback::Window;

inline constexpr uint64_t kT0 = 1000000;
inline constexpr double kPeriodUs = 22222.0;   // 45 Hz
inline constexpr int64_t kMsUs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000, one millisecond in microseconds, not the CR15Game vtable slot

struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    uint32_t Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return (uint32_t)(s >> 33);
    }
    double Uniform(double lo, double hi) { return lo + (hi - lo) * (Next() / 2147483648.0); }
};

inline replay::playback::BodyIdentity BodyId(uint64_t v) {
    replay::playback::BodyIdentity id;
    id.value = v;
    return id;
}

inline std::vector<uint64_t> JitteredTimes(size_t n, double jitterUs, uint64_t seed) {
    Lcg r(seed);
    std::vector<uint64_t> t(n);
    for (size_t i = 0; i < n; ++i) {
        t[i] = kT0 + (uint64_t)llround(i * kPeriodUs + r.Uniform(-jitterUs, jitterUs));
    }
    return t;
}

inline void QuatFromAxisAngle(double ax, double ay, double az, double angle, float q[4]) {
    const double len = sqrt(ax * ax + ay * ay + az * az);
    const double s = sin(angle * 0.5) / len;
    q[0] = (float)(ax * s); q[1] = (float)(ay * s); q[2] = (float)(az * s); q[3] = (float)cos(angle * 0.5);
}

inline BodyState BodyAt(double ts, double noise, Lcg& r) {
    BodyState s;
    memset(&s, 0, sizeof s);
    const double w = 2.0 * 3.14159265358979 * 0.7;
    s.com[0] = (float)(3.0 * sin(w * ts) + r.Uniform(-noise, noise));
    s.com[1] = (float)(1.5 * cos(w * ts) + r.Uniform(-noise, noise));
    s.com[2] = (float)(0.8 * ts + r.Uniform(-noise, noise));
    s.vel[0] = (float)(3.0 * w * cos(w * ts));
    s.vel[1] = (float)(-1.5 * w * sin(w * ts));
    s.vel[2] = 0.8f;
    QuatFromAxisAngle(0.2, 1.0, 0.1, 1.3 * ts + r.Uniform(-noise, noise), s.ori);
    s.bits11 = 7;
    s.owner_slot = UINT32_MAX;
    return s;
}

inline PlayerState PlayerAt(double ts, double noise, Lcg& r) {
    PlayerState p;
    memset(&p, 0, sizeof p);
    const double w = 2.0 * 3.14159265358979 * 1.1;
    QuatFromAxisAngle(0.0, 1.0, 0.0, 0.6 * sin(w * ts) + r.Uniform(-noise, noise), p.hmdOri);
    QuatFromAxisAngle(1.0, 0.2, 0.0, 0.9 * ts + r.Uniform(-noise, noise), p.lHandOri);
    QuatFromAxisAngle(0.0, 0.3, 1.0, -0.7 * ts + r.Uniform(-noise, noise), p.rHandOri);
    for (int c = 0; c < 3; ++c) {
        p.lHandOff[c] = (float)(0.3 * sin(w * ts + c) + r.Uniform(-noise, noise));
        p.rHandOff[c] = (float)(0.25 * cos(w * ts * 0.8 + c) + r.Uniform(-noise, noise));
    }
    p.bendBack = (float)(0.2 * sin(w * ts));
    p.bendFwd = (float)(0.1 * cos(w * ts));
    p.restPoseHeight = 1.6f;
    p.trigL = 0.5f;
    p.holdL = p.holdR = p.anchor = UINT32_MAX;
    return p;
}

// A finalized window with one body and one player on a jittered 45 Hz grid.
inline void FillSmoothWindow(Window& w, size_t n, double noise, uint64_t seed) {
    const std::vector<uint64_t> t = JitteredTimes(n, 3000.0, seed);
    Lcg r(seed * 7 + 1);
    for (size_t i = 0; i < n; ++i) {
        const double ts = (double)(t[i] - kT0) * 1e-6;
        w.AddBodySample(BodyId(0x2001), t[i], BodyAt(ts, noise, r));
        w.AddPlayerSample(0, t[i], PlayerAt(ts, noise, r));
    }
    w.SetCoverage(kT0, t[n - 1] + 1, kT0, t[n - 1] + 1);
    w.Finalize();
}

inline uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}


}  // namespace smoothing_fixture
