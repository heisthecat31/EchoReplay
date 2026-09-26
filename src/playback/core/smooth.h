#pragma once

// Motion smoothing for slow-motion replay (DESIGN-SMOOTHING.md).
//
// The 1:1 samplers in sample.h draw each bracket between two raw 45 Hz samples, so velocity steps at
// every sample -- invisible at 1x, a visible change of speed per sample at 0.1x. This file replaces the
// raw samples with KNOTS: a centred, Gaussian-weighted local quadratic fit over a few neighbours gives
// each sample a denoised value and a derivative that agrees with it, and a C1 cubic is drawn between
// consecutive knots. Centred means no lag; quadratic means constant velocity and constant acceleration
// (a disc in free flight) pass through unchanged; breaks keep every discontinuity sharp.
//
// Everything here is a pure function of the raw samples and the playhead. Knots are memoised per track
// (two slots, the current bracket's ends), which only saves work: a hit and a miss return the same bits.

#include <stddef.h>
#include <stdint.h>

#include "engine/entity.h"

namespace replay {
namespace playback {

class Window;
class SeriesCursor;
struct BodyTrack;
struct PlayerTrack;

enum class SmoothMode : uint8_t { Off = 0, SlowMo = 1, On = 2 };
constexpr uint8_t kSmoothStrengthMin = 1, kSmoothStrengthMax = 3, kSmoothStrengthDefault = 2;

struct SmoothParams {
    float sigma_s = 0.030f;
    float radius_s = 0.075f;     // 2.5 sigma
    uint8_t strength = kSmoothStrengthDefault;
};
SmoothParams ParamsForStrength(uint8_t strength);            // clamps into [1,3]
bool ParseSmoothMode(const char* s, SmoothMode* out);        // "off" | "slowmo" | "on"
const char* SmoothModeName(SmoothMode m);                    // "off" | "slow-mo" | "always"
const char* SmoothStrengthName(uint8_t strength);            // "low" | "med" | "high"
bool SmoothWanted(SmoothMode m, double signedRate);          // SlowMo: fabs(rate) < 0.999
// The mode crossfade: `current` moves toward `target` by wall_us / 200 ms, or lands on it when `snap`.
float StepSmoothWeight(float current, float target, int64_t wall_us, bool snap);

// Pinned: the first or last sample of a segment keeps its raw value; only its slope is fitted.
enum class KnotKind : uint8_t { None = 0, Fit2 = 1, Fit1 = 2, Raw = 3, Pinned = 4 };

struct BodyKnot {
    float com[3], vel[3];        // smoothed position and its derivative (m/s, world)
    float ori[4], oriRate[3];    // smoothed orientation and its body-local rate (rad/s)
    KnotKind kind;
    bool complete;               // every window holding this sample fits this knot bit for bit
};

struct PlayerKnot {
    float quat[3][4];            // hmdOri, lHandOri, rHandOri
    float quatRate[3][3];        // body-local rad/s
    float vec[9];                // lHandOff, rHandOff, bendBack, bendFwd, restPoseHeight
    float vecRate[9];
    KnotKind kind;
    bool complete;
};

// Two slots are enough: a bracket needs knots j and j+1, and advancing one sample reuses one of them.
template <typename Knot>
struct KnotMemo {
    Knot knot[2];
    uint32_t gen[2] = {0, 0};    // 0 = empty; Window::SmoothGen() is never 0
    size_t index[2] = {0, 0};
};
using BodyKnotMemo = KnotMemo<BodyKnot>;
using PlayerKnotMemo = KnotMemo<PlayerKnot>;

struct SmoothTally {             // caller-owned, like HermiteTally
    uint32_t fits = 0;           // knots computed (memo misses)
    uint32_t degenerate = 0;     // knots that fell back to Fit1 or Raw
    uint32_t edge_raw = 0;       // brackets drawn raw because a knot's support crossed an artificial window edge
    uint32_t brackets = 0;       // brackets drawn by the smooth interpolant
    uint32_t held = 0;           // brackets drawn raw because they cross a break (teleport, gap, grab...)
    uint32_t quat_slerp = 0;     // quaternion brackets wider than 1 rad, slerped between knots
};

struct TrackEdges {
    bool begin_real = false;     // nothing precedes the first sample in ANY window
    bool end_real = false;
    int64_t safe_begin_us = 0;   // samples at or after this are held by every window that holds them
    int64_t safe_end_us = 0;
};

// Why the pair (i, i+1) is a discontinuity; the first rule that fires, in the order listed.
enum class BreakRule : uint8_t {
    None = 0, Gap, Teleport, Possession, StateWord, BadSample, VelocityJump, Grab, HandJump,
    kCount
};
BreakRule BodyBreakRule(const BodyState* f, const uint64_t* t, size_t n, size_t i);
BreakRule PlayerBreakRule(const PlayerState* f, const uint64_t* t, size_t n, size_t i);

// Pure. `i` indexes the track's arrays; true = the pair (i, i+1) is a discontinuity.
bool BodyBreakAfter(const BodyState* f, const uint64_t* t, size_t n, size_t i);
bool PlayerBreakAfter(const PlayerState* f, const uint64_t* t, size_t n, size_t i);

// Pure knot fits over the track's raw arrays. `e` only decides `out.complete`.
KnotKind FitBodyKnot(const BodyState* f, const uint64_t* t, size_t n, size_t j, TrackEdges e,
                     const SmoothParams& p, BodyKnot& out);
KnotKind FitPlayerKnot(const PlayerState* f, const uint64_t* t, size_t n, size_t j, TrackEdges e,
                       const SmoothParams& p, PlayerKnot& out);

// The [lo, hi] sample range a knot's fit reads; a test seam for "never spans a break".
void BodyKnotSupport(const BodyState* f, const uint64_t* t, size_t n, size_t j, const SmoothParams& p,
                     size_t* lo, size_t* hi);

TrackEdges EdgesOf(const Window& w, const uint64_t* times, size_t n);

// Same return contract as SampleBodyAt/SamplePlayerAt. A bracket that crosses a break, or whose knots
// sit in the edge ring, is exactly the 1:1 sampler's output (`hermite` is passed through for that).
// `cursor` is the caller's, as for the 1:1 samplers; the knot memo is the track's.
bool SampleBodySmoothAt(Window& w, BodyTrack& tr, SeriesCursor& cursor, int64_t t_us, BodyState& out,
                        bool reversed, bool hermite, SmoothTally* tally);
bool SamplePlayerSmoothAt(Window& w, PlayerTrack& tr, SeriesCursor& cursor, int64_t t_us,
                          PlayerState& out, bool reversed, SmoothTally* tally);

// Continuous fields blend by `wSmooth`; discrete fields come from `raw`.
void BlendBody(const BodyState& raw, const BodyState& sm, float wSmooth, BodyState& out);
void BlendPlayer(const PlayerState& raw, const PlayerState& sm, float wSmooth, PlayerState& out);

// What ApplyOptions carries. nullptr or weight == 0 -> the untouched 1:1 path.
struct SmoothFrame {
    float weight = 0.0f;         // 0 raw, 1 smooth, between = the mode-change crossfade
    SmoothTally* tally = nullptr;
};

}  // namespace playback
}  // namespace replay
