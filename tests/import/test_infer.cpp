#include "doctest.h"

#include <string.h>

#include <algorithm>
#include <functional>
#include <string>

#include "fixture.h"
#include "import/inferred.h"

using namespace replay::import;
using import_test::kStep30Hz;
using import_test::MatchBuilder;

namespace {

constexpr uint64_t kBlueUser = 11, kOrangeUser = 22, kOrange2User = 33, kBlue2User = 44;

// Two blue and two orange players at 30 Hz; `each(frame, builder, rows)` may add fields per frame.
struct Four {
    uint16_t blue, orange, orange2, blue2;
};

MatchBuilder Arena(int frames, const std::function<void(int, MatchBuilder&, const Four&)>& each,
                   SourceKind kind = SourceKind::EchoReplay) {
    MatchBuilder b(kind);
    const Four rows{b.Identity(kBlueUser, "blue", kTeamBlue), b.Identity(kOrangeUser, "orange", kTeamOrange),
                    b.Identity(kOrange2User, "orange2", kTeamOrange), b.Identity(kBlue2User, "blue2", kTeamBlue)};
    for (int i = 0; i < frames; ++i) {
        b.Frame(i * kStep30Hz);
        b.Player(rows.blue, 0, 0.0f, 0.0f);
        b.Player(rows.orange, 1, 10.0f, 10.0f);
        b.Player(rows.orange2, 2, -10.0f, 10.0f);
        b.Player(rows.blue2, 3, 10.0f, -10.0f);
        each(i, b, rows);
    }
    return b;
}

// The sample of file-slot-agnostic reader row `row` in the latest frame.
SrcPlayer& Last(MatchBuilder& b, uint16_t row) {
    const SrcFrame& f = b.match.frames.back();
    for (uint32_t i = f.firstPlayer; i < f.firstPlayer + f.playerCount; ++i) {
        if (b.match.players[i].rosterIndex == row) return b.match.players[i];
    }
    FAIL("row not in frame");
    return b.match.players[0];
}

struct Inferred {
    InferredTracks tracks;
    ConvertReport report{};
};

Inferred RunInfer(MatchBuilder& b, const InferFacts& facts = kInferFacts) {
    Inferred r;
    REQUIRE(Normalise(b.match, ConvertOptions{}, r.report).ok);
    REQUIRE(Infer(b.match, facts, r.tracks, r.report).ok);
    return r;
}

uint16_t SlotOf(const SourceMatch& m, uint64_t user) {
    for (uint16_t s = 0; s < m.roster.size(); ++s) {
        if (m.roster[s].userId == user) return s;
    }
    return kNoSlot;
}

const SampleInference& At(const SourceMatch& m, const InferredTracks& t, int frame, uint64_t user) {
    const SrcFrame& f = m.frames[frame];
    for (uint32_t i = f.firstPlayer; i < f.firstPlayer + f.playerCount; ++i) {
        if (m.players[i].userId == user) return t.samples[i];
    }
    FAIL("user not in frame");
    return t.samples[0];
}

InferFacts WithRule(InferRule rule) {
    InferFacts facts = kInferFacts;
    facts.rules[rule].enabled = true;
    return facts;
}

}  // namespace

TEST_CASE("I-6 stun: rising edge, span duration, puncher from the stats delta") {
    MatchBuilder b = Arena(150, [](int i, MatchBuilder& m, const Four& r) {
        Last(m, r.blue).stunned = i >= 60 && i < 90;
        Last(m, r.orange).stats[kStatStuns] = i >= 62 ? 1 : 0;  // credited 2 frames later
    });
    Inferred r = RunInfer(b);
    REQUIRE(r.tracks.stuns.size() == 1);
    const StunRow& s = r.tracks.stuns[0];
    CHECK(s.tUs == b.match.frames[60].tUs);
    CHECK(s.victimSlot == SlotOf(b.match, kBlueUser));
    CHECK(s.puncherSlot == SlotOf(b.match, kOrangeUser));
    CHECK(s.durationS == doctest::Approx(30 * kStep30Hz * 1e-6).epsilon(1e-4));
    CHECK(s.flags == 0);
    CHECK(r.report.inferredRows[kRuleStun] == 1);
}

TEST_CASE("I-6 stun: two credited opponents -> the one whose hand is nearer the victim's head") {
    MatchBuilder b = Arena(150, [](int i, MatchBuilder& m, const Four& r) {
        Last(m, r.blue).stunned = i >= 60 && i < 90;
        Last(m, r.orange).stats[kStatStuns] = i >= 60 ? 1 : 0;
        Last(m, r.orange2).stats[kStatStuns] = i >= 61 ? 1 : 0;
        if (i == 60) Last(m, r.orange2).rhand.pos[0] = 0.4f, Last(m, r.orange2).rhand.pos[2] = 0.2f;
        Last(m, r.blue2).stats[kStatStuns] = i >= 60 ? 1 : 0;  // a teammate is never the puncher,
        if (i == 60) Last(m, r.blue2).lhand.pos[0] = 0.1f, Last(m, r.blue2).lhand.pos[2] = 0.1f;  // however close
    });
    Inferred r = RunInfer(b);
    REQUIRE(r.tracks.stuns.size() == 1);
    CHECK(r.tracks.stuns[0].puncherSlot == SlotOf(b.match, kOrange2User));
}

TEST_CASE("I-6 stun: no credit -> nearest opponent in range; nobody in range -> guessed") {
    MatchBuilder near = Arena(150, [](int i, MatchBuilder& m, const Four& r) {
        Last(m, r.blue).stunned = i >= 60 && i < 90;
        if (i == 60) {
            SrcPlayer& o2 = Last(m, r.orange2);
            o2.lhand.pos[0] = 1.0f, o2.lhand.pos[2] = 1.0f;
        }
    });
    Inferred r = RunInfer(near);
    REQUIRE(r.tracks.stuns.size() == 1);
    CHECK(r.tracks.stuns[0].puncherSlot == SlotOf(near.match, kOrange2User));
    CHECK((r.tracks.stuns[0].flags & kStunPuncherGuessed) == 0);

    MatchBuilder far = Arena(150, [](int i, MatchBuilder& m, const Four& r) {
        Last(m, r.blue).stunned = i >= 60 && i < 90;
        Last(m, r.orange2).lhand.pos[0] = -30.0f;
        Last(m, r.orange2).rhand.pos[0] = -30.0f;
    });
    r = RunInfer(far);
    REQUIRE(r.tracks.stuns.size() == 1);
    CHECK(r.tracks.stuns[0].puncherSlot == SlotOf(far.match, kOrangeUser));
    CHECK((r.tracks.stuns[0].flags & kStunPuncherGuessed) != 0);
    CHECK(r.report.stunPuncherGuessed == 1);
}

TEST_CASE("I-6 stun: a tape StunCredited event names a candidate like a stats delta") {
    MatchBuilder b = Arena(150, [](int i, MatchBuilder& m, const Four& r) {
        Last(m, r.blue).stunned = i >= 60 && i < 90;
        if (i == 65) m.Event(SrcEventKind::StunCredited).a = kOrange2User;
    }, SourceKind::Tape);
    Inferred r = RunInfer(b);
    REQUIRE(r.tracks.stuns.size() == 1);
    CHECK(r.tracks.stuns[0].puncherSlot == SlotOf(b.match, kOrange2User));
    CHECK((r.tracks.stuns[0].flags & kStunPuncherGuessed) == 0);
}

TEST_CASE("I-6 stun edges: pause, gap, player leave, first seen stunned") {
    // Paused frames inside the span do not count toward the duration.
    MatchBuilder paused = Arena(200, [](int i, MatchBuilder& m, const Four& r) {
        Last(m, r.blue).stunned = i >= 60 && i < 120;
        if (i >= 70 && i < 100) m.match.frames.back().pause = PauseState::Paused;
    });
    Inferred r = RunInfer(paused);
    REQUIRE(r.tracks.stuns.size() == 1);
    CHECK(r.tracks.stuns[0].durationS == doctest::Approx(30 * kStep30Hz * 1e-6).epsilon(1e-4));

    // The victim leaves mid-stun: the row stands, marked truncated at the last present sample.
    auto buildLeaver = [] {
        MatchBuilder m;
        const uint16_t victim = m.Identity(kBlueUser, "blue", kTeamBlue);
        const uint16_t enemy = m.Identity(kOrangeUser, "orange", kTeamOrange);
        for (int i = 0; i < 150; ++i) {
            m.Frame(i * kStep30Hz);
            if (i < 80) m.Player(victim, 0).stunned = i >= 60;
            m.Player(enemy, 1, 1.0f, 0.0f);
        }
        return m;
    };
    // Unmeasured, the observed truncated span stands; measured, the native median replaces it.
    InferFacts unmeasured = kInferFacts;
    unmeasured.stunDurationMedianS.status = FactStatus::kUnmeasured;
    MatchBuilder leaver = buildLeaver();
    r = RunInfer(leaver, unmeasured);
    REQUIRE(r.tracks.stuns.size() == 1);
    CHECK((r.tracks.stuns[0].flags & kStunDurationTruncated) != 0);
    CHECK(r.tracks.stuns[0].durationS == doctest::Approx(19 * kStep30Hz * 1e-6).epsilon(1e-4));
    MatchBuilder leaverAgain = buildLeaver();
    r = RunInfer(leaverAgain);
    REQUIRE(r.tracks.stuns.size() == 1);
    CHECK((r.tracks.stuns[0].flags & kStunDurationTruncated) != 0);
    CHECK(r.tracks.stuns[0].durationS == doctest::Approx(kInferFacts.stunDurationMedianS.value));

    // An onset across a > 250 ms hole still counts, stamped at the later frame; a player whose first
    // sample is already stunned has no observed onset.
    MatchBuilder gap;
    const uint16_t a = gap.Identity(kBlueUser, "blue", kTeamBlue);
    const uint16_t late = gap.Identity(kOrangeUser, "orange", kTeamOrange);
    for (int i = 0; i < 150; ++i) {
        const int64_t t = i * kStep30Hz + (i >= 60 ? 1000000 : 0);
        gap.Frame(t);
        gap.Player(a, 0).stunned = i >= 60 && i < 70;
        if (i >= 100) gap.Player(late, 1).stunned = i < 110;
    }
    r = RunInfer(gap);
    REQUIRE(r.tracks.stuns.size() == 1);
    CHECK(r.tracks.stuns[0].tUs == gap.match.frames[60].tUs);
    CHECK(r.report.gaps == 1);
}

TEST_CASE("I-1/I-2/I-5 geo attach per arm, moving-attached only when enabled, idle word; a disabled rule adds nothing") {
    MatchBuilder b = Arena(100, [](int i, MatchBuilder& m, const Four& r) {
        SrcPlayer& p = Last(m, r.blue);
        if (i >= 10 && i < 20) p.holdL = Hold::Geo;
        if (i >= 30 && i < 40) p.holdR = Hold::Geo, p.vel[2] = 0.5f;
    });
    Inferred r = RunInfer(b);
    const uint32_t idle = kInferFacts.idleWord.value;
    CHECK(At(b.match, r.tracks, 5, kBlueUser).bits28 == idle);
    CHECK(At(b.match, r.tracks, 15, kBlueUser).bits28 == (idle | kInferFacts.geoBitLeft.value));
    CHECK(At(b.match, r.tracks, 35, kBlueUser).bits28 == (idle | kInferFacts.geoBitRight.value));
    CHECK(At(b.match, r.tracks, 15, kBlueUser).trigL == 1.0f);
    CHECK(At(b.match, r.tracks, 15, kBlueUser).trigR == 0.0f);
    CHECK(r.report.inferredRows[kRuleGeoAttach] == 2);
    CHECK(r.report.inferredRows[kRuleMovingAttached] == 0);
    CHECK(r.report.inferredRows[kRuleBaseline] == 4);

    InferFacts moving = kInferFacts;
    moving.rules[kRuleMovingAttached].enabled = true;
    r = RunInfer(b, moving);
    CHECK(At(b.match, r.tracks, 35, kBlueUser).bits28 == (idle | kInferFacts.geoBitRight.value | 0x80u));
    CHECK(At(b.match, r.tracks, 15, kBlueUser).bits28 == (idle | kInferFacts.geoBitLeft.value));
    CHECK(r.report.inferredRows[kRuleMovingAttached] == 1);

    InferFacts off = kInferFacts;
    off.rules[kRuleGeoAttach].enabled = false;
    off.rules[kRuleBaseline].enabled = false;
    MatchBuilder again = Arena(100, [](int i, MatchBuilder& m, const Four& r) {
        if (i >= 10 && i < 20) Last(m, r.blue).holdL = Hold::Geo;
    });
    r = RunInfer(again, off);
    CHECK(At(again.match, r.tracks, 15, kBlueUser).bits28 == 0);
}

TEST_CASE("I-3/I-4 a held player resolves through that frame's playerid, which is reused") {
    MatchBuilder b;
    const uint16_t holder = b.Identity(kBlueUser, "holder", kTeamBlue);
    const uint16_t first = b.Identity(kOrangeUser, "first", kTeamOrange);
    const uint16_t second = b.Identity(kOrange2User, "second", kTeamOrange);
    for (int i = 0; i < 100; ++i) {
        b.Frame(i * kStep30Hz);
        SrcPlayer& h = b.Player(holder, 0);
        h.holdR = Hold::Player;
        h.holdRSlot = 4;
        b.Player(i < 50 ? first : second, 4);  // playerid 4 changes hands at frame 50
    }
    Inferred r = RunInfer(b);
    CHECK(At(b.match, r.tracks, 10, kBlueUser).holdRSlot == SlotOf(b.match, kOrangeUser));
    CHECK(At(b.match, r.tracks, 60, kBlueUser).holdRSlot == SlotOf(b.match, kOrange2User));
    CHECK(At(b.match, r.tracks, 60, kBlueUser).holdLSlot == kNoSlot);
    CHECK(At(b.match, r.tracks, 60, kBlueUser).trigR == 1.0f);
}

TEST_CASE("goals: points edge, point-team rule for own goals, edge-only, and Spark's opening snapshot") {
    MatchBuilder b = Arena(290, [](int i, MatchBuilder& m, const Four&) {
        SrcFrame& f = m.match.frames.back();
        if (i == 0) {  // last_score left over from before the recording
            SrcEvent& old = m.Event(SrcEventKind::GoalScored);
            old.a = kOrangeUser, old.c = kTeamOrange | 2 << 8;
            snprintf(old.text, sizeof old.text, "%s", "INSIDE SHOT");
        }
        f.bluePoints = i >= 50 ? 2 : 0;
        f.orangePoints = i >= 150 ? 3 : 0;
        f.orangePoints += i >= 250 ? 2 : 0;
        if (i == 49) {  // Spark changes last_score one frame before the points
            SrcEvent& e = m.Event(SrcEventKind::GoalScored);
            e.a = kOrangeUser, e.b = kOrange2User, e.c = kTeamOrange | 2 << 8, e.f0 = 12.5f, e.f1 = 8.0f;
            snprintf(e.text, sizeof e.text, "%s", "INSIDE SHOT");
        }
        if (i == 150) {
            SrcEvent& e = m.Event(SrcEventKind::GoalScored);
            e.a = 999;  // resolved by nobody
            e.c = kTeamOrange | 3 << 8;
        }
    });
    Inferred r = RunInfer(b);
    REQUIRE(r.tracks.goals.size() == 3);
    const GoalRow& own = r.tracks.goals[0];
    CHECK(own.tUs == b.match.frames[50].tUs);
    CHECK(own.team == kTeamBlue);  // orange scored into their own goal: blue's points rose
    CHECK(own.points == 2);
    CHECK(own.scorerSlot == SlotOf(b.match, kOrangeUser));
    CHECK(own.assistSlot == SlotOf(b.match, kOrange2User));
    CHECK(own.speedMps == 12.5f);
    CHECK(std::string(own.goalTypeText) == "INSIDE SHOT");
    CHECK(own.flags == 0);
    CHECK(r.tracks.goals[1].team == kTeamOrange);
    CHECK(r.tracks.goals[1].points == 3);
    CHECK(r.tracks.goals[1].flags == kGoalScorerUnresolved);
    CHECK(r.tracks.goals[2].flags == kGoalFromEdgeOnly);
    CHECK(r.tracks.goals[2].scorerSlot == kNoSlot);
}

TEST_CASE("goals: a tape GoalScored with no points edge still stands") {
    MatchBuilder b = Arena(120, [](int i, MatchBuilder& m, const Four&) {
        if (i == 60) {
            SrcEvent& e = m.Event(SrcEventKind::GoalScored);
            e.a = kBlueUser, e.c = kTeamBlue | 2 << 8;
        }
    }, SourceKind::Tape);
    Inferred r = RunInfer(b);
    REQUIRE(r.tracks.goals.size() == 1);
    CHECK(r.tracks.goals[0].team == kTeamBlue);
    CHECK(r.tracks.goals[0].points == 2);
    CHECK(r.tracks.goals[0].scorerSlot == SlotOf(b.match, kBlueUser));
}

TEST_CASE("stat rows on change only; tape team totals are summed from players") {
    MatchBuilder b = Arena(100, [](int i, MatchBuilder& m, const Four& r) {
        Last(m, r.blue).stats[kStatSaves] = i >= 40 ? 1 : 0;
        Last(m, r.blue).possessionTime = i * 0.1f;
        Last(m, r.blue2).stats[kStatSaves] = i >= 70 ? 2 : 0;
    }, SourceKind::Tape);
    Inferred r = RunInfer(b);
    CHECK(r.tracks.stats.size() == 4 + 2);  // one opening row per player, then two changes
    CHECK(r.tracks.stats[4].tUs == b.match.frames[40].tUs);
    CHECK(r.tracks.stats[4].possessionTime == doctest::Approx(4.0f));
    REQUIRE(r.tracks.teamStats.size() == 2 + 2);
    CHECK(r.tracks.teamStats[3].team == kTeamBlue);
    CHECK(r.tracks.teamStats[3].stats[kStatSaves] == 3);
}

TEST_CASE("disc owner follows possession (H5 measured): kept through the throw, none once it is lost") {
    MatchBuilder b = Arena(100, [](int i, MatchBuilder& m, const Four& r) {
        if (i < 30) Last(m, r.orange).holdR = Hold::Disc;
        Last(m, r.orange).possession = i < 60;
    });
    Inferred r = RunInfer(b);
    CHECK(r.tracks.discOwner[10] == SlotOf(b.match, kOrangeUser));
    CHECK(r.tracks.discOwner[40] == SlotOf(b.match, kOrangeUser));
    CHECK(r.tracks.discOwner[70] == kNoSlot);
}

namespace {

// One blue player whose velocity is v(i) m/s along z and whose forward is +z.
MatchBuilder Moving(int frames, const std::function<float(int)>& v) {
    return Arena(frames, [&](int i, MatchBuilder& m, const Four& r) { Last(m, r.blue).vel[2] = v(i); });
}

bool BitAnywhere(const MatchBuilder& b, const Inferred& r, uint32_t bit) {
    for (size_t i = 0; i < b.match.players.size(); ++i) {
        if (b.match.players[i].userId == kBlueUser && (r.tracks.samples[i].bits28 & bit)) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("I-7 boost fires on a forward speed gain, not on a wall bounce, and stays off by default") {
    auto boost = [](int i) { return i < 60 ? 2.0f : i < 64 ? 2.0f + (i - 59) : 6.0f; };
    MatchBuilder def = Moving(120, boost);
    CHECK_FALSE(BitAnywhere(def, RunInfer(def), 0x100));
    MatchBuilder on = Moving(120, boost);
    CHECK(BitAnywhere(on, RunInfer(on, WithRule(kRuleBoost)), 0x100));

    MatchBuilder bounce = Moving(120, [](int i) { return i < 60 ? -5.0f : 3.0f; });
    CHECK_FALSE(BitAnywhere(bounce, RunInfer(bounce, WithRule(kRuleBoost)), 0x100));

    // A one-step jump is a boost; the same jump across a > 250 ms hole is not an acceleration at all.
    auto jump = [](int i) { return i < 62 ? 2.0f : 6.0f; };
    MatchBuilder step = Moving(120, jump);
    CHECK(BitAnywhere(step, RunInfer(step, WithRule(kRuleBoost)), 0x100));
    MatchBuilder gap = Moving(120, jump);
    for (size_t f = 62; f < gap.match.frames.size(); ++f) gap.match.frames[f].tUs += 400000;
    CHECK_FALSE(BitAnywhere(gap, RunInfer(gap, WithRule(kRuleBoost)), 0x100));
}

TEST_CASE("I-8 brake fires on sustained deceleration, not on a collision stop") {
    MatchBuilder brake = Moving(120, [](int i) { return i < 40 ? 8.0f : i < 52 ? 8.0f - 0.3f * (i - 39) : 4.4f; });
    CHECK(BitAnywhere(brake, RunInfer(brake, WithRule(kRuleBrake)), 0x200));

    MatchBuilder wall = Moving(120, [](int i) { return i < 40 ? 8.0f : 0.0f; });
    CHECK_FALSE(BitAnywhere(wall, RunInfer(wall, WithRule(kRuleBrake)), 0x200));
    MatchBuilder glance = Moving(120, [](int i) { return i < 40 ? 8.0f : 1.0f; });
    CHECK_FALSE(BitAnywhere(glance, RunInfer(glance, WithRule(kRuleBrake)), 0x200));
}

TEST_CASE("I-9 thrusters need a measured axis; I-10 punch needs a fast outward hand") {
    auto thrust = [](int i) { return i < 40 ? 0.0f : (i - 40) * 2.0f * kStep30Hz * 1e-6f; };
    InferFacts noAxis = WithRule(kRuleThrusters);
    noAxis.thrusterAxis.status = FactStatus::kUnmeasured;
    MatchBuilder unmeasured = Moving(120, thrust);
    CHECK_FALSE(BitAnywhere(unmeasured, RunInfer(unmeasured, noAxis), 0xC00));
    const InferFacts axis = WithRule(kRuleThrusters);  // P1: hand-local +z
    MatchBuilder measured = Moving(120, thrust);
    CHECK(BitAnywhere(measured, RunInfer(measured, axis), 0x400));
    CHECK(BitAnywhere(measured, RunInfer(measured, axis), 0x800));

    // Seven sustained steps, but a > 250 ms hole splits them into two runs of three.
    MatchBuilder split = Arena(120, [](int i, MatchBuilder& m, const Four& r) {
        auto stamp = [](int k) { return k * kStep30Hz + (k >= 44 ? 400000 : 0); };
        m.match.frames.back().tUs = stamp(i);
        const int64_t accelerated = stamp(std::min(i, 47)) - stamp(40);
        Last(m, r.blue).vel[2] = i < 40 ? 0.0f : 2.0f * static_cast<float>(accelerated) * 1e-6f;
    });
    CHECK_FALSE(BitAnywhere(split, RunInfer(split, axis), 0x400));

    MatchBuilder punch = Arena(120, [](int i, MatchBuilder& m, const Four& r) {
        if (i >= 50 && i < 54) Last(m, r.blue).rhand.pos[2] = 0.2f * (i - 49);
        if (i >= 54) Last(m, r.blue).rhand.pos[2] = 0.8f;
    });
    Inferred r = RunInfer(punch, WithRule(kRulePunch));
    CHECK(BitAnywhere(punch, r, 0x4));
    CHECK_FALSE(BitAnywhere(punch, r, 0x2));
    CHECK(r.report.inferredRows[kRulePunch] == 1);
}

TEST_CASE("Infer is deterministic and refuses a match Normalise has not seen") {
    auto build = [] {
        return Arena(200, [](int i, MatchBuilder& m, const Four& r) {
            Last(m, r.blue).stunned = i >= 60 && i < 90;
            Last(m, r.orange).vel[2] = i < 100 ? 2.0f : 7.0f;
            if (i % 40 < 10) Last(m, r.orange2).holdL = Hold::Geo;
            m.match.frames.back().bluePoints = i >= 150 ? 2 : 0;
        });
    };
    InferFacts all = kInferFacts;
    for (RuleGate& g : all.rules) g.enabled = true;
    all.thrusterAxis.value = {0.0f, 0.0f, 1.0f};
    MatchBuilder a = build(), b = build();
    Inferred ra = RunInfer(a, all), rb = RunInfer(b, all);
    REQUIRE(ra.tracks.samples.size() == rb.tracks.samples.size());
    CHECK(memcmp(ra.tracks.samples.data(), rb.tracks.samples.data(),
                 ra.tracks.samples.size() * sizeof(SampleInference)) == 0);
    REQUIRE(ra.tracks.stuns.size() == 1);
    // Field by field: StunRow has trailing padding that a byte compare would read.
    const StunRow& sa = ra.tracks.stuns[0];
    const StunRow& sb = rb.tracks.stuns[0];
    CHECK((sa.tUs == sb.tUs && sa.victimSlot == sb.victimSlot && sa.puncherSlot == sb.puncherSlot &&
           sa.durationS == sb.durationS && sa.flags == sb.flags));
    CHECK(ra.tracks.goals.size() == 1);
    CHECK(memcmp(ra.report.inferredRows, rb.report.inferredRows, sizeof ra.report.inferredRows) == 0);

    MatchBuilder raw = build();
    InferredTracks tracks;
    ConvertReport report{};
    CHECK_FALSE(Infer(raw.match, kInferFacts, tracks, report).ok);
}

TEST_CASE("scale: a 666 s ten-player match at 30 Hz through Normalise and Infer" * doctest::skip()) {
    MatchBuilder b;
    uint16_t rows[10];
    for (int p = 0; p < 10; ++p) rows[p] = b.Identity(2000 + p, "p", p < 5 ? kTeamBlue : kTeamOrange);
    for (int i = 0; i < 20000; ++i) {
        b.Frame(i * kStep30Hz);
        for (int p = 0; p < 10; ++p) {
            SrcPlayer& s = b.Player(rows[p], static_cast<uint8_t>(p), static_cast<float>(p), 0.0f);
            s.vel[2] = static_cast<float>((i + p * 37) % 90) * 0.1f;
            s.stunned = (i + p * 101) % 390 < 40;
            s.holdL = (i + p) % 290 < 30 ? Hold::Geo : Hold::None;
            s.stats[kStatStuns] = static_cast<uint16_t>((i + p * 53) / 390);
        }
        b.match.frames.back().bluePoints = static_cast<uint16_t>(i / 2000);
    }
    InferFacts all = kInferFacts;
    for (RuleGate& g : all.rules) g.enabled = true;
    all.thrusterAxis.value = {0.0f, 0.0f, 1.0f};
    Inferred r = RunInfer(b, all);
    CHECK(r.tracks.samples.size() == 200000);
    CHECK(r.tracks.stuns.size() > 390);
}
