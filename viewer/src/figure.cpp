#include "figure.h"

#include <algorithm>

namespace erv {
namespace {

constexpr uint32_t kXray = 1u << 1;   // GizmoWireFlag kWireXray: also drawn at 0.35 alpha where hidden
constexpr float kPi = 3.14159265f;

V3 Axis(int a) {
    switch (a) {
        case 0: return {1, 0, 0};
        case 1: return {-1, 0, 0};
        case 2: return {0, 0, 1};
        case 3: return {0, 0, -1};
        case 4: return {0, 1, 0};
        default: return {0, -1, 0};
    }
}

void Seg(std::vector<WireVert>& o, V3 a, V3 b, uint32_t c, uint32_t f, float thick, V3 side, V3 up) {
    AddLine(o, a, b, c, f);
    if (thick <= 0.f) return;
    const float w = 0.006f * thick;
    AddLine(o, a + side * w, b + side * w, c, f);
    AddLine(o, a - side * w, b - side * w, c, f);
    if (thick > 1.5f) {
        AddLine(o, a + up * w, b + up * w, c, f);
        AddLine(o, a - up * w, b - up * w, c, f);
    }
}

void Ring(std::vector<WireVert>& o, V3 c, V3 u, V3 v, float r, uint32_t col, uint32_t f, int n = 20) {
    for (int i = 0; i < n; ++i) {
        const float a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
        AddLine(o, c + u * (r * cosf(a0)) + v * (r * sinf(a0)), c + u * (r * cosf(a1)) + v * (r * sinf(a1)), col, f);
    }
}

void Box(std::vector<WireVert>& o, V3 c, V3 x, V3 y, V3 z, V3 h, uint32_t col, uint32_t f) {
    V3 p[8];
    for (int i = 0; i < 8; ++i)
        p[i] = c + x * ((i & 1 ? 1 : -1) * h.x) + y * ((i & 2 ? 1 : -1) * h.y) + z * ((i & 4 ? 1 : -1) * h.z);
    const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& k : e) AddLine(o, p[k[0]], p[k[1]], col, f);
}

// Two-bone IK: root -> joint -> end, the joint bent towards `pole`.
V3 Elbow(V3 root, V3 end, float l1, float l2, V3 pole) {
    V3 d = end - root;
    float len = Len(d);
    const V3 dir = Norm(d);
    len = std::clamp(len, 0.05f, (l1 + l2) * 0.999f);
    const float a = (l1 * l1 - l2 * l2 + len * len) / (2 * len);
    const float h = sqrtf(std::max(0.f, l1 * l1 - a * a));
    V3 bend = pole - dir * Dot(pole, dir);
    if (Len(bend) < 1e-4f) bend = Cross(dir, V3{1, 0, 0});
    return root + dir * a + Norm(bend) * h;
}

uint32_t Dim(uint32_t c, float k) {
    const uint32_t r = uint32_t((c & 0xFF) * k), g = uint32_t(((c >> 8) & 0xFF) * k), b = uint32_t(((c >> 16) & 0xFF) * k);
    return (c & 0xFF000000u) | (std::min(r, 255u)) | (std::min(g, 255u) << 8) | (std::min(b, 255u) << 16);
}

}  // namespace

void AddLine(std::vector<WireVert>& out, V3 a, V3 b, uint32_t colour, uint32_t flags) {
    out.push_back({{a.x, a.y, a.z}, colour, flags});
    out.push_back({{b.x, b.y, b.z}, colour, flags});
}

void HeadAxes(const Q4& q, int fa, V3& fwd, V3& up, V3& right) {
    fwd = Norm(Rotate(q, Axis(fa)));
    // up is the head's own +Y unless forward is on Y, then +Z
    const V3 upLocal = (fa >= 4) ? V3{0, 0, 1} : V3{0, 1, 0};
    up = Norm(Rotate(q, upLocal));
    right = Norm(Cross(up, fwd));
}

int GuessForwardAxis(const Replay& r) {
    double score[6] = {};
    for (const Part& part : r.parts) {
        for (const PlayerTrack& p : part.players) {
            const size_t step = std::max<size_t>(1, p.pose.size() / 400);
            for (size_t i = 0; i < p.pose.size(); i += step) {
                const PoseKey& k = p.pose[i];
                const V3 mid = Norm((k.l_off + k.r_off) * 0.5f);
                for (int a = 0; a < 6; ++a) {
                    V3 f = Rotate(k.head_q, Axis(a));
                    f.y = 0;
                    V3 m = mid;
                    m.y = 0;
                    score[a] += Dot(Norm(f), Norm(m));
                }
            }
        }
    }
    int best = 2;
    for (int a = 0; a < 4; ++a)   // forward is horizontal-ish: only X / Z candidates
        if (score[a] > score[best]) best = a;
    return best;
}

void AddFigure(std::vector<WireVert>& o, const PlayerPose& p, const FigureStyle& s) {
    const uint32_t f = s.xray ? kXray : 0;
    const uint32_t col = p.stunned ? 0xFF40E0FFu : s.colour;   // stunned: yellow
    V3 fwd, up, right;
    HeadAxes(p.head_q, s.forward_axis, fwd, up, right);
    const V3 worldUp{0, 1, 0};
    // the body hangs under the head, facing where the head faces (yaw only), leaning with the pitch a little
    V3 bodyFwd = fwd;
    bodyFwd.y = 0;
    bodyFwd = Len(bodyFwd) > 0.1f ? Norm(bodyFwd) : Norm(Cross(right, worldUp));
    const V3 bodyRight = Norm(Cross(worldUp, bodyFwd));
    const float th = s.thickness;

    // helmet: three rings and a visor
    const float hr = 0.135f;
    Ring(o, p.head, right, up, hr, col, f);
    Ring(o, p.head, fwd, up, hr, col, f);
    Ring(o, p.head, right, fwd, hr, col, f);
    const V3 visor = p.head + fwd * (hr * 0.85f);
    Ring(o, visor, right, up, hr * 0.55f, s.accent, f, 16);
    if (p.speaking) {   // sound rings beside the helmet
        Ring(o, p.head + right * 0.2f, fwd, up, 0.05f, 0xFF60FF60u, f, 10);
        Ring(o, p.head - right * 0.2f, fwd, up, 0.05f, 0xFF60FF60u, f, 10);
    }

    if (s.body) {
        const V3 neck = p.head - up * 0.16f;
        const V3 chest = neck - worldUp * 0.12f + bodyFwd * 0.02f;
        const V3 pelvis = chest - worldUp * 0.42f - bodyFwd * 0.03f;
        const V3 lSh = chest + bodyRight * -0.2f + worldUp * 0.04f, rSh = chest + bodyRight * 0.2f + worldUp * 0.04f;
        Seg(o, p.head - up * hr, neck, col, f, th, bodyRight, bodyFwd);
        Seg(o, neck, chest, col, f, th, bodyRight, bodyFwd);
        Seg(o, lSh, rSh, col, f, th, bodyFwd, worldUp);
        Seg(o, chest, pelvis, col, f, th, bodyRight, bodyFwd);
        // chest plate
        Box(o, chest - worldUp * 0.12f + bodyFwd * 0.03f, bodyRight, worldUp, bodyFwd, {0.17f, 0.14f, 0.09f}, Dim(col, 0.8f), f);
        // arms
        const V3 pole = (worldUp * -1.f) + bodyFwd * -0.4f;
        const V3 lEl = Elbow(lSh, p.lhand, 0.29f, 0.28f, pole - bodyRight * 0.3f);
        const V3 rEl = Elbow(rSh, p.rhand, 0.29f, 0.28f, pole + bodyRight * 0.3f);
        Seg(o, lSh, lEl, col, f, th, bodyFwd, worldUp);
        Seg(o, lEl, p.lhand, col, f, th, bodyFwd, worldUp);
        Seg(o, rSh, rEl, col, f, th, bodyFwd, worldUp);
        Seg(o, rEl, p.rhand, col, f, th, bodyFwd, worldUp);
        // legs: they trail in zero g, knees a little forward, feet back against the motion
        V3 drift = p.vel * -0.04f;
        if (Len(drift) > 0.25f) drift = Norm(drift) * 0.25f;
        const V3 lHip = pelvis - bodyRight * 0.1f, rHip = pelvis + bodyRight * 0.1f;
        const V3 lKnee = lHip - worldUp * 0.4f + bodyFwd * 0.12f + drift * 0.5f;
        const V3 rKnee = rHip - worldUp * 0.4f + bodyFwd * 0.08f + drift * 0.5f;
        const V3 lFoot = lKnee - worldUp * 0.38f - bodyFwd * 0.1f + drift;
        const V3 rFoot = rKnee - worldUp * 0.38f - bodyFwd * 0.14f + drift;
        Seg(o, lHip, rHip, col, f, th, bodyFwd, worldUp);
        Seg(o, lHip, lKnee, col, f, th, bodyRight, bodyFwd);
        Seg(o, lKnee, lFoot, col, f, th, bodyRight, bodyFwd);
        Seg(o, rHip, rKnee, col, f, th, bodyRight, bodyFwd);
        Seg(o, rKnee, rFoot, col, f, th, bodyRight, bodyFwd);
        Seg(o, lFoot, lFoot + bodyFwd * 0.12f, col, f, th, bodyRight, worldUp);
        Seg(o, rFoot, rFoot + bodyFwd * 0.12f, col, f, th, bodyRight, worldUp);
    }

    // hands: a box each, smaller when the grip is closed, with the pointing axis
    auto hand = [&](V3 at, Q4 q, float trig) {
        const V3 hx = Rotate(q, {1, 0, 0}), hy = Rotate(q, {0, 1, 0}), hz = Rotate(q, {0, 0, 1});
        const float k = 1.f - 0.35f * std::clamp(trig, 0.f, 1.f);
        Box(o, at, hx, hy, hz, V3{0.045f, 0.025f, 0.06f} * k, s.accent, f);
        AddLine(o, at, at + hz * 0.1f, Dim(s.accent, 0.7f), f);
    };
    hand(p.lhand, p.l_q, p.trig_l);
    hand(p.rhand, p.r_q, p.trig_r);
}

void AddDisc(std::vector<WireVert>& o, V3 pos, Q4 ori, uint32_t colour, bool xray) {
    const uint32_t f = xray ? kXray : 0;
    const V3 a = Rotate(ori, {1, 0, 0}), b = Rotate(ori, {0, 0, 1}), n = Rotate(ori, {0, 1, 0});
    const uint32_t dim = Dim(colour, 0.6f);
    // rim: a rounded edge, three rings across its thickness
    const float R = 0.15f;
    Ring(o, pos, a, b, R, colour, f, 40);
    Ring(o, pos + n * 0.014f, a, b, R - 0.006f, colour, f, 40);
    Ring(o, pos - n * 0.014f, a, b, R - 0.006f, colour, f, 40);
    // the domed faces: a lip ring and a raised inner ring each side, the hub at the centre
    for (float side : {1.f, -1.f}) {
        Ring(o, pos + n * (0.018f * side), a, b, R - 0.03f, dim, f, 36);
        Ring(o, pos + n * (0.022f * side), a, b, 0.075f, colour, f, 28);
        Ring(o, pos + n * (0.024f * side), a, b, 0.03f, colour, f, 16);
    }
    // spokes from hub to rim, and the edge ticks that read the spin
    for (int i = 0; i < 12; ++i) {
        const float an = 2 * kPi * i / 12;
        const V3 d = a * cosf(an) + b * sinf(an);
        AddLine(o, pos + d * 0.03f + n * 0.024f, pos + d * (R - 0.03f) + n * 0.018f, dim, f);
        AddLine(o, pos + d * 0.03f - n * 0.024f, pos + d * (R - 0.03f) - n * 0.018f, dim, f);
        AddLine(o, pos + d * (R - 0.006f) + n * 0.014f, pos + d * (R - 0.006f) - n * 0.014f, colour, f);
    }
    Ring(o, pos, a, b, R + 0.012f, (colour & 0x00FFFFFFu) | 0x60000000u, f, 40);   // glow
}

void AddTrail(std::vector<WireVert>& o, const std::vector<V3>& pts, uint32_t colour, bool xray) {
    for (size_t i = 1; i < pts.size(); ++i) {
        const float k = float(i) / float(pts.size());
        AddLine(o, pts[i - 1], pts[i], (colour & 0x00FFFFFFu) | (uint32_t(40 + 200 * k) << 24), xray ? kXray : 0);
    }
}

}  // namespace erv
