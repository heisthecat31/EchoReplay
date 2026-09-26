#pragma once
// DESIGN-HOST-SYNC P4. Where the recording says the disc's "untouched" shell is up.
//
// In a real match the disc controller sends forceSetFXState_Red/Blue at every round restart, the FX
// graph spawns the shell, and the first grab's OnNetTouchGrab arm clears it. So the shell is a level
// of the playhead: up from a restart R[k] until the first recorded grab G[k] after it. R[0] is the
// capture start, whose shell is the one the host itself spawned at load (Blue, LIVE-TRUTH R5).
//
// R[k], k >= 1, is the first recorded `game_state` edge into pre_launch after goal k and before goal
// k+1 -- on 2026-09-24_12-59-23 that is 19.5-19.7 s after each of the first six goals, and the match
// winner has none. G[k] comes from the bound disc's possession edges over the whole file (the
// load-time track scan) and is filled in once the disc track is known; until it is, the span
// answers "not known" and nothing is delivered for it.

#include <stdint.h>

namespace replay::shellplan {

enum class Colour : uint8_t { Blue, Red };
const char* ColourName(Colour c);

// 69ae322e @ 0x180006750: lastscoreteam == 0 sends Red, anything else Blue. The recorded scoring
// team stands in for the host's stale lastscoreteam.
inline Colour ColourOfScoringTeam(uint16_t team) { return team == 0 ? Colour::Red : Colour::Blue; }

struct GoalIn {
    int64_t t_us = 0;
    uint16_t team = 0;
};
// One recorded game_state edge; `pre_launch` is true for pre_launch_enter / pre_launch_update.
struct PhaseIn {
    int64_t t_us = 0;
    bool pre_launch = false;
};

// One change of the bound disc's recorded owned/unowned state; edges are given in time order.
struct HolderEdgeIn {
    int64_t t_us = 0;
    bool owned = false;
};

inline constexpr uint32_t kMaxSpans = 256;

struct Span {
    int64_t r_us = 0;
    Colour colour = Colour::Blue;
    int32_t goal = -1;          // the goal index this restart follows; -1 for R[0]
    bool grab_known = false;
    bool grab_none = false;     // the span ends with no recorded grab in it
    int64_t g_us = 0;
};

struct Want {
    bool known = false;   // false: the span's first grab has not been scanned yet
    int32_t k = -1;       // -1: before the capture start
    bool show = false;
    Colour colour = Colour::Blue;
};

class Plan {
public:
    // Goals and phase edges in time order, on the playhead's axis.
    void Build(int64_t captureStartUs, const GoalIn* goals, uint32_t nGoals, const PhaseIn* phases,
               uint32_t nPhases);
    void Clear() { count_ = 0; }

    uint32_t Count() const { return count_; }
    const Span& At(uint32_t k) const { return spans_[k]; }
    // The end of span k: the next restart, or INT64_MAX for the last.
    int64_t SpanEndUs(uint32_t k) const;

    void SetGrab(uint32_t k, int64_t gUs);
    void SetNoGrab(uint32_t k);
    // Every span's first grab from the disc's whole-file possession edges: the first owned edge
    // after R[k] and before the next restart. A disc still owned at R[k] is grabbed at R[k]; the
    // disc is unowned before its first edge.
    void FillGrabs(const HolderEdgeIn* edges, uint32_t n);
    void ClearGrabs();
    // The first span whose grab is still unscanned, or Count().
    uint32_t FirstUnscanned() const;

    Want WantAt(int64_t tUs) const;

private:
    Span spans_[kMaxSpans];
    uint32_t count_ = 0;
};

}  // namespace replay::shellplan
