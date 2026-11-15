#pragma once
// A player's body from what the recording carries: head position and rotation, both hands. The game
// builds its body from the same three with its own IK; this is our version of it (Echo-like proportions),
// drawn as line segments the renderer depth-tests against the level.

#include <stdint.h>

#include <vector>

#include "replay.h"

namespace erv {

struct WireVert {
    float pos[3];
    uint32_t rgba;   // 0xAABBGGRR
    uint32_t flags;
};

struct FigureStyle {
    uint32_t colour = 0xFFFFFFFF;
    uint32_t accent = 0xFFFFFFFF;
    bool xray = false;        // visible through walls (dimmed)
    bool body = true;         // torso, arms and legs; off = head and hands only
    float thickness = 1.f;    // 0 thin, 1 normal, 2 bold
    int forward_axis = 2;     // which head axis looks forward: 0 +X .. 5 -Z (see HeadAxes)
};

// Head basis from the recorded rotation, with the forward axis the replay uses.
void HeadAxes(const Q4& q, int forward_axis, V3& fwd, V3& up, V3& right);
// Picks the forward axis for a replay: the one that points from the head towards the hands most often.
int GuessForwardAxis(const Replay&);

void AddFigure(std::vector<WireVert>& out, const PlayerPose&, const FigureStyle&);
void AddDisc(std::vector<WireVert>& out, V3 pos, Q4 ori, uint32_t colour, bool xray);
void AddTrail(std::vector<WireVert>& out, const std::vector<V3>& pts, uint32_t colour, bool xray);
void AddLine(std::vector<WireVert>& out, V3 a, V3 b, uint32_t colour, uint32_t flags);

}  // namespace erv
