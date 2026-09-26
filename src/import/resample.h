#pragma once

// Source-rate tracks resampled onto the output tick grid (DESIGN-FORMATS §7.4). Internal to Emit.
//
// Positions use a monotone cubic Hermite (Fritsch-Carlson tangents from finite differences), so a
// curve never overshoots its source points: a pause, whose frames repeat one position, stays put
// instead of wobbling. The velocity written with each tick is the derivative of that same curve, so
// the viewer's own Hermite between ticks reproduces it. A track breaks at source gaps, at absences and
// at teleports (respawns, goal resets); nothing interpolates or differentiates across a break.

#include <stdint.h>
#include <vector>

#include "import/emit.h"
#include "import/sourcemodel.h"
#include "import/timeline.h"

namespace replay::import {

constexpr float kPlayerTeleportMps = 40.0f;    // no player moves this fast; a jump this fast is a respawn
constexpr float kDiscTeleportMps = 80.0f;      // twice any throw; a jump this fast is a reset to centre

// One entity's presence over the source frames. Point i sits at frames[frame[i]].
struct Track {
    std::vector<uint32_t> frame;
    std::vector<int64_t> tUs;
    std::vector<float> pos;         // 3 per point
    std::vector<float> tangent;     // 3 per point, m/s
    std::vector<float> ori;         // 4 per point, hemisphere-aligned to the previous point in a segment
    std::vector<uint8_t> joinsNext; // point i and i + 1 are one continuous segment
    std::vector<uint32_t> sample;   // caller's index for point i (SourceMatch::players index, or frame)
    uint32_t cursor = 0;            // advanced by Locate; ticks are visited in order
    uint32_t teleports = 0;
};

void TrackAdd(Track& t, uint32_t frame, int64_t tUs, const float pos[3], const float q[4], uint32_t sample);

// Marks breaks (source gap, absence, teleport above maxSpeedMps) and fills the tangents.
void TrackFinish(Track& t, float maxSpeedMps);

// The point at the tick's older frame, or -1 when the entity is absent there. Ticks must be visited in
// increasing order.
int32_t TrackLocate(Track& t, const TickFrame& tick);

struct TrackSample {
    float pos[3], vel[3];
    float ori[4], angVel[3];
};

// Evaluates at the tick. Inside a segment: Hermite position, its derivative, slerp and the segment's
// angular velocity. At a break or the last point: that point's pose, held, with zero velocities.
void TrackEval(const Track& t, uint32_t point, const TickFrame& tick, TrackSample& out);

}  // namespace replay::import
