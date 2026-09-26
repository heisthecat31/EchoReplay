#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "import/hypotheses.h"
#include "import/inferred.h"
#include "import/timeline.h"

namespace replay::import {
namespace {

// bits28 (DESIGN-FORMATS §3.3). The L/R split of the thruster and punch pairs follows the table's
// order and, unlike geo attach (I-1), has no native measurement behind it yet.
constexpr uint32_t kBitPunchLeft = 0x2;
constexpr uint32_t kBitPunchRight = 0x4;
constexpr uint32_t kBitMovingAttached = 0x80;
constexpr uint32_t kBitBoost = 0x100;
constexpr uint32_t kBitBrake = 0x200;
constexpr uint32_t kBitThrustLeft = 0x400;
constexpr uint32_t kBitThrustRight = 0x800;

constexpr int64_t kGoalPairWindowUs = 2000000;  // Spark's last_score changes 0-1 frame before the points
constexpr float kMinDirectionSpeed = 0.5f;      // m/s; slower velocities have no reliable direction
constexpr float kMaxStunS = 30.0f;              // SR15StunPlayerMsg duration range (0, 30]

using V3 = std::array<float, 3>;

V3 Load(const float v[3]) { return {v[0], v[1], v[2]}; }
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Scale(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Length(const V3& a) { return sqrtf(Dot(a, a)); }

bool TurnWithin(const V3& before, const V3& after, float maxDeg) {
    const float lb = Length(before), la = Length(after);
    if (lb < kMinDirectionSpeed || la < kMinDirectionSpeed) return false;
    return Dot(before, after) / (lb * la) > cosf(maxDeg * 3.14159265f / 180.0f);
}

bool Opposes(uint8_t a, uint8_t b) {
    return a != b && a != kTeamSpectator && b != kTeamSpectator;
}

struct Sample {
    uint32_t player, frame;
};

class Inference {
public:
    Inference(const SourceMatch& match, const InferFacts& facts, InferredTracks& out, ConvertReport& report)
        : m_(match), facts_(facts), out_(out), report_(report) {}

    void Run() {
        out_.samples.assign(m_.players.size(), SampleInference{0, kNoSlot, kNoSlot, 0.0f, 0.0f});
        out_.discOwner.assign(m_.frames.size(), kNoSlot);
        BuildTracks();
        BuildUserSlots();
        DirectReadings();
        for (const std::vector<Sample>& track : tracks_) Kinematics(track);
        CountOnsets();
        if (Enabled(kRuleStun)) Stuns();
        Goals();
        Stats();
        TeamStats();
        DiscOwners();
    }

private:
    bool Enabled(InferRule rule) const { return facts_.rules[rule].enabled; }
    const SrcPlayer& P(const Sample& s) const { return m_.players[s.player]; }
    const SrcFrame& F(const Sample& s) const { return m_.frames[s.frame]; }
    int64_t T(const Sample& s) const { return F(s).tUs; }

    // Present in the very next frame with no GAP between: the two samples bracket one source step.
    bool Linked(const Sample& a, const Sample& b) const {
        return b.frame == a.frame + 1 && T(b) - T(a) <= kGapUs;
    }

    void BuildTracks() {
        tracks_.assign(m_.roster.size(), {});
        for (uint32_t f = 0; f < m_.frames.size(); ++f) {
            const SrcFrame& frame = m_.frames[f];
            for (uint32_t i = frame.firstPlayer; i < frame.firstPlayer + frame.playerCount; ++i) {
                tracks_[m_.players[i].rosterIndex].push_back({i, f});
            }
        }
    }

    void BuildUserSlots() {
        for (uint16_t slot = 0; slot < m_.roster.size(); ++slot) {
            if (m_.roster[slot].userId != kNoUser) userSlots_.push_back({m_.roster[slot].userId, slot});
        }
        std::sort(userSlots_.begin(), userSlots_.end());
    }

    uint16_t SlotOfUser(uint64_t userId) const {
        auto it = std::lower_bound(userSlots_.begin(), userSlots_.end(), std::make_pair(userId, uint16_t{0}));
        return it != userSlots_.end() && it->first == userId ? it->second : kNoSlot;
    }

    // I-1..I-5: direct readings of per-frame state.
    void DirectReadings() {
        std::array<uint16_t, 256> slotOfApi;
        slotOfApi.fill(kNoSlot);
        const uint32_t idle = Enabled(kRuleBaseline) ? facts_.idleWord.value : 0;
        const uint32_t geoL = facts_.geoBitLeft.value, geoR = facts_.geoBitRight.value;
        for (const SrcFrame& frame : m_.frames) {
            const uint32_t end = frame.firstPlayer + frame.playerCount;
            for (uint32_t i = frame.firstPlayer; i < end; ++i) slotOfApi[m_.players[i].apiSlot] = m_.players[i].rosterIndex;
            for (uint32_t i = frame.firstPlayer; i < end; ++i) {
                const SrcPlayer& p = m_.players[i];
                SampleInference& s = out_.samples[i];
                s.bits28 |= idle;
                if (Enabled(kRuleGeoAttach)) {
                    if (p.holdL == Hold::Geo) s.bits28 |= geoL;
                    if (p.holdR == Hold::Geo) s.bits28 |= geoR;
                }
                const bool attached = p.holdL == Hold::Geo || p.holdR == Hold::Geo;
                if (Enabled(kRuleMovingAttached) && attached && Length(Load(p.vel)) > facts_.movingAttachedMinSpeed) {
                    s.bits28 |= kBitMovingAttached;
                }
                if (Enabled(kRuleHeldPlayer)) {
                    if (p.holdL == Hold::Player && p.holdLSlot != kNotInFrame) s.holdLSlot = slotOfApi[p.holdLSlot];
                    if (p.holdR == Hold::Player && p.holdRSlot != kNotInFrame) s.holdRSlot = slotOfApi[p.holdRSlot];
                }
                if (Enabled(kRuleGrip)) {
                    s.trigL = p.holdL != Hold::None ? 1.0f : 0.0f;
                    s.trigR = p.holdR != Hold::None ? 1.0f : 0.0f;
                }
            }
            for (uint32_t i = frame.firstPlayer; i < end; ++i) slotOfApi[m_.players[i].apiSlot] = kNoSlot;
        }
    }

    // I-7..I-10 over runs of linked, unpaused samples. Acceleration is the raw per-step difference:
    // a centred smoothing window spreads a one-step impact over neighbouring steps, which would let a
    // collision pass the "sustained for N frames" tests that separate it from a brake.
    void Kinematics(const std::vector<Sample>& track) {
        const bool any = Enabled(kRuleBoost) || Enabled(kRuleBrake) || Enabled(kRuleThrusters) || Enabled(kRulePunch);
        if (!any) return;
        size_t begin = 0;
        while (begin < track.size()) {
            if (F(track[begin]).pause != PauseState::Unpaused) {
                ++begin;
                continue;
            }
            size_t end = begin + 1;
            while (end < track.size() && F(track[end]).pause == PauseState::Unpaused &&
                   Linked(track[end - 1], track[end])) {
                ++end;
            }
            KinematicRun(&track[begin], end - begin);
            begin = end;
        }
    }

    void KinematicRun(const Sample* run, size_t n) {
        if (n < 2) return;
        t_.resize(n);
        v_.resize(n);
        a_.resize(n);
        excludedBefore_.assign(n + 1, 0);
        for (size_t i = 0; i < n; ++i) {
            const SrcPlayer& p = P(run[i]);
            t_[i] = static_cast<double>(T(run[i])) * 1e-6;
            v_[i] = Load(p.vel);
            a_[i] = i == 0 ? V3{0, 0, 0} : Scale(Sub(v_[i], v_[i - 1]), static_cast<float>(1.0 / (t_[i] - t_[i - 1])));
            // Contact forces dominate while stunned or holding geometry.
            const bool excluded = p.stunned || p.holdL == Hold::Geo || p.holdR == Hold::Geo;
            excludedBefore_[i + 1] = excludedBefore_[i] + (excluded ? 1 : 0);
        }
        if (Enabled(kRuleBoost)) Boost(run, n);
        if (Enabled(kRuleBrake)) Brake(run, n);
        if (Enabled(kRuleThrusters)) {
            Thrusters(run, n, true);
            Thrusters(run, n, false);
        }
        if (Enabled(kRulePunch)) {
            Punch(run, n, true);
            Punch(run, n, false);
        }
    }

    bool AnyExcluded(size_t first, size_t last) const { return excludedBefore_[last + 1] != excludedBefore_[first]; }

    void SetBit(const Sample* run, size_t first, size_t last, uint32_t bit) {
        for (size_t i = first; i <= last; ++i) out_.samples[run[i].player].bits28 |= bit;
    }

    // cond_[i] describes the step ending at sample i; runs of at least minSteps set `bit` on their samples.
    template <class Accept>
    void MarkRuns(const Sample* run, size_t n, size_t minSteps, uint32_t bit, Accept accept) {
        size_t i = 1;
        while (i < n) {
            if (!cond_[i]) {
                ++i;
                continue;
            }
            size_t last = i;
            while (last + 1 < n && cond_[last + 1]) ++last;
            if (last - i + 1 >= minSteps && accept(i, last)) SetBit(run, i, last, bit);
            i = last + 1;
        }
    }

    // I-7: a forward-aligned speed gain within the window, with a real acceleration peak and the
    // direction of travel kept (a wall bounce reverses it).
    void Boost(const Sample* run, size_t n) {
        cond_.assign(n, 0);
        const double window = facts_.boostWindowS;
        size_t j = 0;
        for (size_t i = 1; i < n; ++i) {
            while (t_[i] - t_[j] > window) ++j;
            if (j == i || AnyExcluded(j, i)) continue;
            V3 forward;
            // The full head frame (H3), not hmdOri: that one is yaw-only (H2), and boosts go up and down too.
            QuatAxis(BodyQuat(P(run[i]), kEmitFacts), 2, forward.data());
            if (Dot(Sub(v_[i], v_[j]), forward) < facts_.boostMinGainMps) continue;
            float peak = 0.0f;
            for (size_t k = j + 1; k <= i; ++k) peak = std::max(peak, Length(a_[k]));
            if (peak < facts_.boostMinPeakAccel) continue;
            const bool fromRest = Length(v_[j]) < kMinDirectionSpeed;
            if (!fromRest && !TurnWithin(v_[j], v_[i], facts_.boostMaxTurnDeg)) continue;
            cond_[i] = 1;
        }
        const bool spanMeasured = facts_.boostSpanS.status == FactStatus::kMeasured && facts_.boostSpanS.value > 0.0f;
        for (size_t i = 1; i < n; ++i) {
            if (!cond_[i]) continue;
            SetBit(run, i, i, kBitBoost);
            if (!spanMeasured || cond_[i - 1]) continue;
            for (size_t k = i + 1; k < n && t_[k] - t_[i] <= facts_.boostSpanS.value; ++k) SetBit(run, k, k, kBitBoost);
        }
    }

    // I-8: deceleration along the travel direction for several consecutive steps, ending slower but
    // still moving the same way. A collision is one step; a bounce turns the velocity round.
    void Brake(const Sample* run, size_t n) {
        cond_.assign(n, 0);
        for (size_t i = 1; i < n; ++i) {
            const float speed = Length(v_[i - 1]);
            if (speed < kMinDirectionSpeed || AnyExcluded(i - 1, i)) continue;
            cond_[i] = Dot(a_[i], Scale(v_[i - 1], 1.0f / speed)) <= facts_.brakeMaxAccel;
        }
        MarkRuns(run, n, facts_.brakeMinFrames, kBitBrake, [&](size_t first, size_t last) {
            const V3& before = v_[first - 1];
            const V3& after = v_[last];
            return Length(after) < facts_.brakeMaxSpeedRatio * Length(before) &&
                   TurnWithin(before, after, facts_.brakeMaxTurnDeg);
        });
    }

    // I-9: moderate sustained acceleration along the thruster axis of that hand.
    void Thrusters(const Sample* run, size_t n, bool left) {
        if (facts_.thrusterAxis.status != FactStatus::kMeasured) return;  // no direction to test against
        const Vec3f& axis = facts_.thrusterAxis.value;
        const V3 local{axis.x, axis.y, axis.z};
        if (Length(local) == 0.0f) return;
        cond_.assign(n, 0);
        for (size_t i = 1; i < n; ++i) {
            const float accel = Length(a_[i]);
            if (accel < facts_.thrustMinAccel || accel > facts_.thrustMaxAccel || AnyExcluded(i - 1, i)) continue;
            const SrcPlayer& p = P(run[i]);
            const float* q = HandQuat(left ? p.lhand : p.rhand, kEmitFacts);
            V3 world{0, 0, 0}, column;
            for (int c = 0; c < 3; ++c) {
                QuatAxis(q, c, column.data());
                for (int r = 0; r < 3; ++r) world[r] += column[r] * local[c];
            }
            cond_[i] = Dot(a_[i], world) / (accel * Length(world)) >= facts_.thrustMinCos;
        }
        MarkRuns(run, n, facts_.thrustMinFrames, left ? kBitThrustLeft : kBitThrustRight,
                 [](size_t, size_t) { return true; });
    }

    // I-10: the hand moving fast away from the head, for several consecutive steps.
    void Punch(const Sample* run, size_t n, bool left) {
        cond_.assign(n, 0);
        for (size_t i = 1; i < n; ++i) {
            if (AnyExcluded(i - 1, i)) continue;
            const SrcPlayer& now = P(run[i]);
            const SrcPlayer& before = P(run[i - 1]);
            const V3 offNow = Sub(Load((left ? now.lhand : now.rhand).pos), Load(now.head.pos));
            const V3 offBefore = Sub(Load((left ? before.lhand : before.rhand).pos), Load(before.head.pos));
            const V3 step = Sub(offNow, offBefore);
            const float speed = Length(step) / static_cast<float>(t_[i] - t_[i - 1]);
            cond_[i] = speed >= facts_.punchMinHandSpeed && Dot(step, offNow) > 0.0f;
        }
        MarkRuns(run, n, facts_.punchMinFrames, left ? kBitPunchLeft : kBitPunchRight,
                 [](size_t, size_t) { return true; });
    }

    // inferredRows counts onsets: a rule's output switching on within one player's linked samples.
    void CountOnsets() {
        struct BitRule {
            InferRule rule;
            uint32_t mask;
        };
        const BitRule bitRules[] = {
            {kRuleGeoAttach, facts_.geoBitLeft.value | facts_.geoBitRight.value},
            {kRuleMovingAttached, kBitMovingAttached},
            {kRuleBaseline, facts_.idleWord.value},
            {kRuleBoost, kBitBoost},
            {kRuleBrake, kBitBrake},
            {kRuleThrusters, kBitThrustLeft | kBitThrustRight},
            {kRulePunch, kBitPunchLeft | kBitPunchRight},
        };
        for (const std::vector<Sample>& track : tracks_) {
            for (size_t k = 0; k < track.size(); ++k) {
                const SampleInference& now = out_.samples[track[k].player];
                const bool linked = k > 0 && Linked(track[k - 1], track[k]);
                const SampleInference* before = linked ? &out_.samples[track[k - 1].player] : nullptr;
                for (const BitRule& r : bitRules) {
                    const bool was = before != nullptr && (before->bits28 & r.mask) != 0;
                    if ((now.bits28 & r.mask) != 0 && !was) ++report_.inferredRows[r.rule];
                }
                const bool held = now.holdLSlot != kNoSlot || now.holdRSlot != kNoSlot;
                const bool wasHeld = before != nullptr && (before->holdLSlot != kNoSlot || before->holdRSlot != kNoSlot);
                if (held && !wasHeld) ++report_.inferredRows[kRuleHeldPlayer];
                const bool grip = now.trigL > 0.0f || now.trigR > 0.0f;
                const bool wasGrip = before != nullptr && (before->trigL > 0.0f || before->trigR > 0.0f);
                if (grip && !wasGrip) ++report_.inferredRows[kRuleGrip];
            }
        }
    }

    float MedianStepS() const {
        std::vector<int64_t> steps;
        for (size_t f = 1; f < m_.frames.size(); ++f) steps.push_back(m_.frames[f].tUs - m_.frames[f - 1].tUs);
        if (steps.empty()) return 0.0f;
        std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());
        return static_cast<float>(steps[steps.size() / 2]) * 1e-6f;
    }

    const SrcPlayer* InFrame(uint32_t frame, uint16_t slot) const {
        const SrcFrame& f = m_.frames[frame];
        for (uint32_t i = f.firstPlayer; i < f.firstPlayer + f.playerCount; ++i) {
            if (m_.players[i].rosterIndex == slot) return &m_.players[i];
        }
        return nullptr;
    }

    static float HandToHead(const SrcPlayer& attacker, const float head[3]) {
        const V3 target = Load(head);
        return std::min(Length(Sub(Load(attacker.lhand.pos), target)), Length(Sub(Load(attacker.rhand.pos), target)));
    }

    // I-6: one stun row per rising edge of `stunned`, stamped at the first stunned sample. An edge needs
    // an earlier sample of the same player; a player first seen already stunned has no observed onset.
    void Stuns() {
        struct Credit {
            int64_t tUs;
            uint16_t slot;
            bool operator<(const Credit& o) const { return tUs != o.tUs ? tUs < o.tUs : slot < o.slot; }
        };
        std::vector<Credit> credits;
        for (uint16_t slot = 0; slot < tracks_.size(); ++slot) {
            const std::vector<Sample>& track = tracks_[slot];
            for (size_t k = 1; k < track.size(); ++k) {
                if (P(track[k]).stats[kStatStuns] > P(track[k - 1]).stats[kStatStuns]) credits.push_back({T(track[k]), slot});
            }
        }
        for (const SrcEvent& e : m_.events) {
            if (e.kind != SrcEventKind::StunCredited) continue;
            const uint16_t slot = SlotOfUser(e.a);
            if (slot != kNoSlot) credits.push_back({e.tUs, slot});
        }
        std::sort(credits.begin(), credits.end());

        const float floorS = std::max(MedianStepS(), 1e-3f);
        const int64_t windowUs = static_cast<int64_t>(facts_.stunStatWindowS * 1e6f);
        for (uint16_t slot = 0; slot < tracks_.size(); ++slot) {
            const std::vector<Sample>& track = tracks_[slot];
            for (size_t k = 1; k < track.size(); ++k) {
                const SrcPlayer& victim = P(track[k]);
                if (!victim.stunned || P(track[k - 1]).stunned || victim.team == kTeamSpectator) continue;

                StunRow row{T(track[k]), slot, kNoSlot, 0.0f, 0};
                int64_t activeUs = 0;
                bool ended = false;
                for (size_t j = k; j + 1 < track.size() && Linked(track[j], track[j + 1]); ++j) {
                    if (F(track[j]).pause == PauseState::Unpaused) activeUs += T(track[j + 1]) - T(track[j]);
                    if (!P(track[j + 1]).stunned) {
                        ended = true;
                        break;
                    }
                }
                float duration = static_cast<float>(activeUs) * 1e-6f;
                if (!ended) {
                    row.flags |= kStunDurationTruncated;
                    const Fact<float>& median = facts_.stunDurationMedianS;
                    if (median.status == FactStatus::kMeasured && median.value > 0.0f) duration = median.value;
                }
                row.durationS = std::min(std::max(duration, floorS), kMaxStunS);
                row.puncherSlot = Puncher(track[k].frame, victim, credits, windowUs, row.flags);
                out_.stuns.push_back(row);
                ++report_.inferredRows[kRuleStun];
                if (row.flags & kStunPuncherGuessed) ++report_.stunPuncherGuessed;
            }
        }
        std::stable_sort(out_.stuns.begin(), out_.stuns.end(),
                         [](const StunRow& a, const StunRow& b) { return a.tUs < b.tUs; });
    }

    template <class Credits>
    uint16_t Puncher(uint32_t frame, const SrcPlayer& victim, const Credits& credits, int64_t windowUs,
                     uint8_t& flags) const {
        const int64_t t = m_.frames[frame].tUs;
        uint16_t best = kNoSlot;
        float bestDistance = INFINITY;
        auto consider = [&](uint16_t slot, float distance) {
            if (best == kNoSlot || distance < bestDistance || (distance == bestDistance && slot < best)) {
                best = slot;
                bestDistance = distance;
            }
        };

        for (auto it = std::lower_bound(credits.begin(), credits.end(), typename Credits::value_type{t - windowUs, 0});
             it != credits.end() && it->tUs <= t + windowUs; ++it) {
            const SrcPlayer* attacker = InFrame(frame, it->slot);
            const uint8_t team = attacker != nullptr ? attacker->team : m_.roster[it->slot].team;
            if (!Opposes(team, victim.team)) continue;
            consider(it->slot, attacker != nullptr ? HandToHead(*attacker, victim.head.pos) : INFINITY);
        }
        if (best != kNoSlot) return best;

        const SrcFrame& f = m_.frames[frame];
        for (uint32_t i = f.firstPlayer; i < f.firstPlayer + f.playerCount; ++i) {
            const SrcPlayer& p = m_.players[i];
            if (Opposes(p.team, victim.team)) consider(p.rosterIndex, HandToHead(p, victim.head.pos));
        }
        if (best == kNoSlot || bestDistance > facts_.stunNearRadiusM) flags |= kStunPuncherGuessed;
        return best;
    }

    // Goals come from points edges; the credited team is the one whose points rose, so an own goal
    // credits the other team. The nearest GoalScored within the window supplies scorer and details.
    void Goals() {
        std::vector<uint32_t> goalEvents;
        for (uint32_t e = 0; e < m_.events.size(); ++e) {
            if (m_.events[e].kind == SrcEventKind::GoalScored) goalEvents.push_back(e);
        }
        std::vector<bool> used(goalEvents.size(), false);

        for (size_t f = 1; f < m_.frames.size(); ++f) {
            const SrcFrame& now = m_.frames[f];
            const SrcFrame& before = m_.frames[f - 1];
            const uint16_t rose[2] = {
                static_cast<uint16_t>(now.bluePoints > before.bluePoints ? now.bluePoints - before.bluePoints : 0),
                static_cast<uint16_t>(now.orangePoints > before.orangePoints ? now.orangePoints - before.orangePoints : 0),
            };
            for (uint8_t team = kTeamBlue; team <= kTeamOrange; ++team) {
                if (rose[team] == 0) continue;
                GoalRow row{};
                row.tUs = now.tUs;
                row.team = team;
                row.points = static_cast<uint8_t>(std::min<uint16_t>(rose[team], UINT8_MAX));
                row.scorerSlot = row.assistSlot = kNoSlot;
                size_t match = goalEvents.size();
                int64_t nearest = kGoalPairWindowUs + 1;
                for (size_t g = 0; g < goalEvents.size(); ++g) {
                    const int64_t distance = llabs(m_.events[goalEvents[g]].tUs - now.tUs);
                    if (!used[g] && distance < nearest) {
                        match = g;
                        nearest = distance;
                    }
                }
                if (match == goalEvents.size()) {
                    row.flags |= kGoalFromEdgeOnly;
                } else {
                    used[match] = true;
                    FillFromEvent(m_.events[goalEvents[match]], row);
                }
                out_.goals.push_back(row);
            }
        }

        // A tape's own GoalScored stands without a points edge; Spark's last_score is a snapshot that
        // is already non-empty on the first line, so an unpaired one there is an old goal.
        if (m_.kind == SourceKind::Tape) {
            for (size_t g = 0; g < goalEvents.size(); ++g) {
                if (used[g]) continue;
                const SrcEvent& e = m_.events[goalEvents[g]];
                GoalRow row{};
                row.tUs = e.tUs;
                row.team = static_cast<uint8_t>(e.c & 0xFF);
                row.points = static_cast<uint8_t>(e.c >> 8);
                FillFromEvent(e, row);
                out_.goals.push_back(row);
            }
        }
        std::stable_sort(out_.goals.begin(), out_.goals.end(),
                         [](const GoalRow& a, const GoalRow& b) { return a.tUs < b.tUs; });
    }

    void FillFromEvent(const SrcEvent& e, GoalRow& row) const {
        static_assert(sizeof row.goalTypeText == sizeof e.text);
        memcpy(row.goalTypeText, e.text, sizeof row.goalTypeText);
        row.goalTypeText[sizeof row.goalTypeText - 1] = '\0';
        row.scorerSlot = SlotOfUser(e.a);
        row.assistSlot = SlotOfUser(e.b);
        row.speedMps = e.f0;
        row.distanceM = e.f1;
        if (row.scorerSlot == kNoSlot) row.flags |= kGoalScorerUnresolved;
    }

    void Stats() {
        std::vector<const SrcPlayer*> last(m_.roster.size(), nullptr);
        for (const SrcFrame& frame : m_.frames) {
            for (uint32_t i = frame.firstPlayer; i < frame.firstPlayer + frame.playerCount; ++i) {
                const SrcPlayer& p = m_.players[i];
                const SrcPlayer*& previous = last[p.rosterIndex];
                if (previous == nullptr || memcmp(previous->stats, p.stats, sizeof p.stats) != 0) {
                    StatRow row{frame.tUs, p.rosterIndex, {}, p.possessionTime};
                    memcpy(row.stats, p.stats, sizeof row.stats);
                    out_.stats.push_back(row);
                }
                previous = &p;
            }
        }
    }

    // Spark carries team totals; a tape does not, so they are summed from the players present.
    void TeamStats() {
        TeamStatRow last[2] = {};
        bool any[2] = {false, false};
        for (const SrcFrame& frame : m_.frames) {
            uint32_t sums[2][kStatCount] = {};
            float possession[2] = {0.0f, 0.0f};
            if (!frame.hasTeamStats) {
                for (uint32_t i = frame.firstPlayer; i < frame.firstPlayer + frame.playerCount; ++i) {
                    const SrcPlayer& p = m_.players[i];
                    if (p.team > kTeamOrange) continue;
                    for (int s = 0; s < kStatCount; ++s) sums[p.team][s] += p.stats[s];
                    possession[p.team] += p.possessionTime;
                }
            }
            for (uint8_t team = kTeamBlue; team <= kTeamOrange; ++team) {
                TeamStatRow row{frame.tUs, team, {}, 0.0f};
                for (int s = 0; s < kStatCount; ++s) {
                    row.stats[s] = frame.hasTeamStats ? frame.teamStats[team][s]
                                                      : static_cast<uint16_t>(std::min<uint32_t>(sums[team][s], UINT16_MAX));
                }
                row.possessionTime = frame.hasTeamStats ? frame.teamPossessionTime[team] : possession[team];
                if (any[team] && memcmp(last[team].stats, row.stats, sizeof row.stats) == 0) continue;
                out_.teamStats.push_back(row);
                last[team] = row;
                any[team] = true;
            }
        }
    }

    void DiscOwners() {
        uint16_t previous = kNoSlot;
        for (size_t f = 0; f < m_.frames.size(); ++f) {
            previous = DiscOwnerSlot(m_, m_.frames[f], kEmitFacts.discOwner.value, previous);
            out_.discOwner[f] = previous;
        }
    }

    const SourceMatch& m_;
    const InferFacts& facts_;
    InferredTracks& out_;
    ConvertReport& report_;
    std::vector<std::vector<Sample>> tracks_;
    std::vector<std::pair<uint64_t, uint16_t>> userSlots_;
    std::vector<double> t_;
    std::vector<V3> v_, a_;
    std::vector<uint32_t> excludedBefore_;
    std::vector<uint8_t> cond_;
};

}  // namespace

Result Infer(const SourceMatch& match, const InferFacts& facts, InferredTracks& out, ConvertReport& report) {
    if (match.mapSymbol == 0) return Refuse(ResultCode::BadArguments, "Infer needs a normalised match");
    for (const SrcPlayer& p : match.players) {
        if (p.rosterIndex >= match.roster.size()) {
            return Refuse(ResultCode::BadArguments, "a player sample names file slot %u of %zu", p.rosterIndex,
                          match.roster.size());
        }
    }
    out = InferredTracks{};
    Inference(match, facts, out, report).Run();
    return Success();
}

}  // namespace replay::import
