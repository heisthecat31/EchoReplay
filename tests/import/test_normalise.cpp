#include "doctest.h"

#include <string.h>

#include "fixture.h"
#include "import/inferred.h"
#include "import/timeline.h"

using namespace replay::import;
using import_test::kStep30Hz;
using import_test::MatchBuilder;

namespace {

// One player at 30 Hz for `frames` frames, stamps from `stamp(i)`.
template <class Stamp>
MatchBuilder Steady(int frames, Stamp stamp) {
    MatchBuilder b;
    const uint16_t row = b.Identity(100, "solo", kTeamBlue);
    for (int i = 0; i < frames; ++i) {
        b.Frame(stamp(i));
        b.Player(row, 0);
    }
    return b;
}

Result Run(MatchBuilder& b, ConvertReport& report, int32_t session = -1) {
    ConvertOptions options;
    options.session = session;
    return Normalise(b.match, options, report);
}

}  // namespace

TEST_CASE("Normalise: a few backward stamps are repaired in place, file order kept") {
    MatchBuilder b = Steady(290, [](int i) { return i * kStep30Hz; });
    b.match.frames[100].tUs = b.match.frames[99].tUs - 156000;  // the tape sample's worst step
    b.match.frames[200].tUs = b.match.frames[199].tUs;
    ConvertReport report{};
    REQUIRE(Run(b, report).ok);
    CHECK(report.repairedStamps == 2);
    CHECK_FALSE(report.retimedWhole);
    CHECK(b.match.frames[100].tUs == b.match.frames[99].tUs + kRepairStepUs);
    for (size_t i = 1; i < b.match.frames.size(); ++i) CHECK(b.match.frames[i].tUs > b.match.frames[i - 1].tUs);
}

TEST_CASE("Normalise: jitter on more than 1% of frames re-times on the nominal rate but keeps a real gap") {
    const int64_t step15Hz = 1000000 / 15;
    MatchBuilder b = Steady(290, [&](int i) { return i * step15Hz + (i % 3 == 1 ? 90000 : 0); });
    b.match.nominalHz = 15;
    for (int i = 150; i < 290; ++i) b.match.frames[i].tUs += 2000000;  // a 2 s hole mid-file
    ConvertReport report{};
    REQUIRE(Run(b, report).ok);
    CHECK(report.retimedWhole);
    CHECK(b.match.frames[1].tUs - b.match.frames[0].tUs == step15Hz);
    CHECK(b.match.frames[150].tUs - b.match.frames[149].tUs > 2000000);
    CHECK(report.gaps == 1);
    CHECK(report.requiresAllowGaps);
}

TEST_CASE("Normalise: isolated lead-in and tail frames are dropped; a paused stretch is not a gap") {
    MatchBuilder b = Steady(200, [](int i) { return 9000000 + i * kStep30Hz; });
    b.match.frames[0].tUs = 0;                                        // 10 s before the rest
    b.match.frames[199].tUs = b.match.frames[198].tUs + 6000000;      // 6 s after
    for (int i = 50; i < 120; ++i) b.match.frames[i].pause = PauseState::Paused;
    ConvertReport report{};
    REQUIRE(Run(b, report).ok);
    CHECK(report.leadInDropped == 1);
    CHECK(report.tailDropped == 1);
    CHECK(report.framesKept == 198);
    CHECK(report.gaps == 0);
    CHECK_FALSE(report.requiresAllowGaps);
    CHECK(b.match.roster[0].firstSeenUs == b.match.frames.front().tUs);
    CHECK(b.match.roster[0].lastSeenUs == b.match.frames.back().tUs);
}

TEST_CASE("Normalise: sessions split; the longest is the default and --session picks another") {
    auto build = [] {
        MatchBuilder b;
        b.match.sessions.push_back(SrcSession{"session-1"});
        const uint16_t first = b.Identity(1, "first", kTeamBlue);
        const uint16_t second = b.Identity(2, "second", kTeamOrange);
        for (int i = 0; i < 100; ++i) {
            b.Frame(i * kStep30Hz, 0);
            b.Player(first, 0);
        }
        for (int i = 100; i < 390; ++i) {
            b.Frame(i * kStep30Hz, 1);
            b.Player(second, 0);
            b.Event(SrcEventKind::DiscThrown).a = 2;
        }
        return b;
    };
    MatchBuilder b = build();
    ConvertReport report{};
    REQUIRE(Run(b, report).ok);
    CHECK(report.sessionsFound == 2);
    CHECK(report.sessionChosen == 1);
    CHECK(b.match.frames.size() == 290);
    REQUIRE(b.match.roster.size() == 1);
    CHECK(b.match.roster[0].userId == 2);
    CHECK(b.match.players[0].rosterIndex == 0);
    CHECK(b.match.events.size() == 290);
    CHECK(b.match.frames[5].firstEvent == 5);
    CHECK(b.match.events[5].tUs == b.match.frames[5].tUs);

    MatchBuilder other = build();
    ConvertReport otherReport{};
    REQUIRE(Run(other, otherReport, 0).ok);
    CHECK(other.match.roster[0].userId == 1);

    MatchBuilder bad = build();
    ConvertReport badReport{};
    CHECK(Run(bad, badReport, 2).code == ResultCode::BadArguments);
}

TEST_CASE("Normalise: file slots follow first appearance by userId, not the reused API playerid") {
    MatchBuilder b;
    const uint16_t late = b.Identity(900, "late", kTeamOrange);  // reader row 0, appears second
    const uint16_t early = b.Identity(500, "early", kTeamBlue);
    const uint16_t spectator = b.Identity(700, "watcher", kTeamSpectator);
    const uint16_t earlyAgain = b.Identity(500, "early", kTeamBlue);  // a second reader row, same person
    for (int i = 0; i < 100; ++i) {
        b.Frame(i * kStep30Hz);
        if (i < 50) {
            b.Player(early, 3);
        } else {
            b.Player(late, 3);  // same playerid, another person
            b.Player(i < 75 ? early : earlyAgain, 5);  // the first person, renumbered
        }
        b.Player(spectator, 7);
    }
    ConvertReport report{};
    REQUIRE(Run(b, report).ok);
    REQUIRE(b.match.roster.size() == 3);
    CHECK(b.match.roster[0].userId == 500);
    CHECK(b.match.roster[1].userId == 700);
    CHECK(b.match.roster[2].userId == 900);
    CHECK(b.match.roster[1].team == kTeamSpectator);
    CHECK(b.match.roster[2].firstSeenUs == b.match.frames[50].tUs);
    CHECK(b.match.roster[0].lastSeenUs == b.match.frames[99].tUs);
    const SrcFrame& f = b.match.frames[60];
    CHECK(b.match.players[f.firstPlayer].rosterIndex == 2);
    CHECK(b.match.players[f.firstPlayer + 1].rosterIndex == 0);
    CHECK(b.match.players[b.match.frames[80].firstPlayer + 1].rosterIndex == 0);
}

TEST_CASE("Normalise: an empty game_status holds the previous phase") {
    MatchBuilder b = Steady(100, [](int i) { return i * kStep30Hz; });
    b.match.frames[0].status = GameStatus::Empty;
    b.match.frames[10].status = GameStatus::RoundStart;
    for (int i = 11; i < 100; ++i) b.match.frames[i].status = i % 2 ? GameStatus::Empty : GameStatus::Playing;
    ConvertReport report{};
    REQUIRE(Run(b, report).ok);
    CHECK(b.match.frames[0].status == GameStatus::Empty);
    CHECK(b.match.frames[11].status == GameStatus::RoundStart);
    CHECK(b.match.frames[13].status == GameStatus::Playing);
}

TEST_CASE("Normalise refuses by name") {
    ConvertReport report{};

    MatchBuilder combat = Steady(100, [](int i) { return i * kStep30Hz; });
    snprintf(combat.match.mapName, sizeof combat.match.mapName, "%s", "mpl_combat_dyson");
    Result r = Run(combat, report);
    CHECK(r.code == ResultCode::UnsupportedMap);
    CHECK(strstr(r.message, "mpl_combat_dyson") != nullptr);

    MatchBuilder brief = Steady(50, [](int i) { return i * kStep30Hz; });
    CHECK(Run(brief, report).code == ResultCode::TooShort);

    MatchBuilder mirrored = Steady(2000, [](int i) { return i * kStep30Hz; });
    mirrored.match.stats.basisTrips[kPartLHand] = 3;
    mirrored.match.stats.basisFirstTripFrame[kPartLHand] = 417;
    r = Run(mirrored, report);
    CHECK(r.code == ResultCode::Basis);
    CHECK(strstr(r.message, "lhand") != nullptr);
    CHECK(strstr(r.message, "417") != nullptr);

    MatchBuilder tolerated = Steady(2000, [](int i) { return i * kStep30Hz; });
    tolerated.match.stats.basisTrips[kPartHead] = 2;  // exactly 0.1% passes
    CHECK(Run(tolerated, report).ok);

    MatchBuilder crowd;
    crowd.Frame(0);
    crowd.Frame(3000000);
    for (uint64_t u = 1; u <= 65; ++u) crowd.Player(crowd.Identity(u, "p", kTeamBlue), static_cast<uint8_t>(u));
    CHECK(Run(crowd, report).code == ResultCode::Roster);

    MatchBuilder garbled = Steady(100, [](int i) { return i * kStep30Hz; });
    garbled.match.stats.lines = 100;
    garbled.match.stats.badLines = 2;
    CHECK(Run(garbled, report).code == ResultCode::Corrupt);
}
