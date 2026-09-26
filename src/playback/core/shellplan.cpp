#include "playback/core/shellplan.h"

#include <stdint.h>

namespace replay::shellplan {

const char* ColourName(Colour c) { return c == Colour::Red ? "Red" : "Blue"; }

void Plan::Build(int64_t captureStartUs, const GoalIn* goals, uint32_t nGoals,
                 const PhaseIn* phases, uint32_t nPhases) {
    count_ = 0;
    Span& first = spans_[count_++];
    first = Span();
    first.r_us = captureStartUs;
    first.colour = Colour::Blue;

    uint32_t p = 0;
    for (uint32_t g = 0; g < nGoals && count_ < kMaxSpans; ++g) {
        const int64_t after = goals[g].t_us;
        const int64_t before = g + 1 < nGoals ? goals[g + 1].t_us : INT64_MAX;
        while (p < nPhases && phases[p].t_us <= after) ++p;
        uint32_t q = p;
        while (q < nPhases && phases[q].t_us < before && !phases[q].pre_launch) ++q;
        if (q >= nPhases || phases[q].t_us >= before) continue;   // no restart: the match winner
        Span& s = spans_[count_++];
        s = Span();
        s.r_us = phases[q].t_us;
        s.colour = ColourOfScoringTeam(goals[g].team);
        s.goal = static_cast<int32_t>(g);
        p = q;
    }
}

int64_t Plan::SpanEndUs(uint32_t k) const {
    return k + 1 < count_ ? spans_[k + 1].r_us : INT64_MAX;
}

void Plan::SetGrab(uint32_t k, int64_t gUs) {
    if (k >= count_) return;
    spans_[k].grab_known = true;
    spans_[k].grab_none = false;
    spans_[k].g_us = gUs;
}

void Plan::SetNoGrab(uint32_t k) {
    if (k >= count_) return;
    spans_[k].grab_known = true;
    spans_[k].grab_none = true;
}

void Plan::FillGrabs(const HolderEdgeIn* edges, uint32_t n) {
    uint32_t e = 0;
    bool owned = false;
    for (uint32_t k = 0; k < count_; ++k) {
        const int64_t r = spans_[k].r_us;
        const int64_t end = SpanEndUs(k);
        while (e < n && edges[e].t_us <= r) owned = edges[e++].owned;
        if (owned) {
            SetGrab(k, r);
            continue;
        }
        uint32_t f = e;
        while (f < n && edges[f].t_us < end && !edges[f].owned) ++f;
        if (f < n && edges[f].t_us < end) SetGrab(k, edges[f].t_us);
        else SetNoGrab(k);
    }
}

void Plan::ClearGrabs() {
    for (uint32_t k = 0; k < count_; ++k) {
        spans_[k].grab_known = false;
        spans_[k].grab_none = false;
        spans_[k].g_us = 0;
    }
}

uint32_t Plan::FirstUnscanned() const {
    for (uint32_t k = 0; k < count_; ++k) {
        if (!spans_[k].grab_known) return k;
    }
    return count_;
}

Want Plan::WantAt(int64_t tUs) const {
    Want w;
    if (count_ == 0 || tUs < spans_[0].r_us) {
        w.known = true;
        return w;
    }
    uint32_t lo = 0, hi = count_;   // the last span whose r_us <= tUs
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (spans_[mid].r_us <= tUs) lo = mid;
        else hi = mid;
    }
    const Span& s = spans_[lo];
    w.k = static_cast<int32_t>(lo);
    w.colour = s.colour;
    w.known = s.grab_known;
    w.show = s.grab_known && (s.grab_none || tUs < s.g_us);
    return w;
}

}  // namespace replay::shellplan
