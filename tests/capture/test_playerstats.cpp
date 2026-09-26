// The live scoreboard's producer half (ARCH-21 MAJOR-B2). Two properties, and both of them are the
// ones that would have shipped broken:
//
//   1. THE GATE. ReadPlayerStats costs ~1 us per entrant. Sixteen entrants on every tick is the exact
//      shape of L2-RUN-5 F1 -- the roster's 1247 us tick that killed a capture at 50 s. The assertion
//      here is a COUNT of engine reads over 1489 ticks, not a "it ran" flag: only a count can tell a
//      gate that gates from a gate that is decoration.
//
//   2. THE MASK. The record carries the reader's fill unchanged, because the absence sentinels are
//      the format's and there is one copy of them. A capture that widened the mask would hand
//      PlayerStatsToDisk a claim on values nobody sourced, and the file would read as a scoreboard
//      of zeroes that was never played.

#include "doctest.h"

#include "capture/playerstats.h"

#include "capture/roster.h"
#include "foundation/demofile/format.h"

#include <vector>

using namespace replay;
using namespace replay::capture;

namespace {

// The engine read, counted. "The sweep ran" is not the question -- "how many times did it touch the
// engine" is, so every Read is tallied whether it succeeds or refuses.
class CountingStatsSource : public IPlayerStatsSource {
public:
    mutable uint32_t reads = 0;
    bool usable = true;
    uint16_t answerFill = uint16_t(kPlayerStatsFillAll);
    identity_access::PlayerStats answer = {};

    bool Usable() const override { return usable; }

    bool Read(uint16_t userIndex, identity_access::PlayerStats& out, uint16_t& fill,
              uint8_t& reason, const char*& why) const override {
        (void)userIndex;
        (void)reason;
        (void)why;
        ++reads;
        // Only the claimed fields are written. An unclaimed field is left exactly as the caller
        // handed it over, which is what the record must then carry.
        if (answerFill & kPsFillPoints) out.points = answer.points;
        if (answerFill & kPsFillGoals) out.goals = answer.goals;
        if (answerFill & kPsFillPing) out.ping = answer.ping;
        if (answerFill & kPsFillPacketLoss) out.packetLossRatio = answer.packetLossRatio;
        if (answerFill & kPsFillEmote) out.isEmotePlaying = answer.isEmotePlaying;
        fill = answerFill;
        return true;
    }
};

struct Collector {
    std::vector<PlayerStatsRec> recs;
};

void CollectRec(void* ctx, const PlayerStatsRec& rec) {
    static_cast<Collector*>(ctx)->recs.push_back(rec);
}

// n active rows, userIndex == row index, so a refusal or a swap is visible in the record.
void FillRoster(RosterTable& t, uint16_t n) {
    for (uint16_t i = 0; i < n; ++i) {
        bool appended = false;
        RosterRow* r = t.FindOrAppend(0x1000ull + i, i, 0, appended);
        REQUIRE(r != nullptr);
        REQUIRE(appended);
        r->active = 1;
        r->userIndex = i;
    }
}

}  // namespace

TEST_CASE("1489 ticks do not buy 1489 sweeps") {
    CountingStatsSource src;
    RosterTable roster;
    FillRoster(roster, 16);

    PlayerStatsStore store = {};
    Collector sink;

    // A signal that never moves: the only arm left is the cadence, which is the arm the count below
    // is a statement about.
    const uint64_t kSignal = StatsSignalOf(3, 2, 1, 1, 0, 4, 1, 16);
    const uint64_t kStepUs = 11111;   // ~90 Hz
    const uint32_t kTicks = 1489;     // L2-RUN-5's own capture length

    for (uint32_t i = 0; i < kTicks; ++i) {
        const uint64_t tUs = uint64_t(i) * kStepUs;
        StatsSweepReport rep;
        SweepPlayerStats(&src, roster, store, tUs, tUs, kSignal, kStatsRescanIntervalUs,
                         /*keyframeUs=*/0, /*force=*/i == 0, CollectRec, &sink, rep);
    }

    // One forced sweep at the open, then one per cadence period across the span. Derived from the
    // span and the interval, never from a transcribed sweep count.
    const uint64_t elapsedUs = uint64_t(kTicks - 1) * kStepUs;
    const uint64_t expectedSweeps = 1 + elapsedUs / kStatsRescanIntervalUs;
    CHECK(src.reads == 16 * expectedSweeps);
    CHECK(src.reads < 16 * uint64_t(kTicks));

    // Nothing moved after the first sweep, so the first sweep is the only one that wrote.
    CHECK(sink.recs.size() == 16);
    for (const PlayerStatsRec& r : sink.recs) {
        CHECK(r.keyframe == 1);
        CHECK(r.sourceTimeUs == 0);
    }
}

TEST_CASE("the fill mask the reader answered is the mask on the record") {
    CountingStatsSource src;
    // The two bits this build cannot source, cleared -- derived from the whole mask rather than
    // written out, so renumbering a bit cannot leave a stale literal behind.
    const uint16_t kAnswered = uint16_t(kPlayerStatsFillAll & ~(kPsFillPacketLoss | kPsFillEmote));
    src.answerFill = kAnswered;
    src.answer.points = 12;
    src.answer.goals = 3;
    src.answer.ping = 47;
    src.answer.packetLossRatio = 0.25f;   // claimed by nobody: must never reach the record
    src.answer.isEmotePlaying = 1;

    RosterTable roster;
    FillRoster(roster, 1);
    roster.Mutable(0).userIndex = 7;
    PlayerStatsStore store = {};
    Collector sink;

    StatsSweepReport rep;
    REQUIRE(SweepPlayerStats(&src, roster, store, 5000, 6000, 0, kStatsRescanIntervalUs, 0, true,
                             CollectRec, &sink, rep));
    REQUIRE(sink.recs.size() == 1);
    const PlayerStatsRec& rec = sink.recs[0];

    CHECK(rec.fill == kAnswered);
    CHECK(rec.userIndex == 7);
    CHECK(rec.v.points == 12);
    // The unclaimed pair is the caller's zero-initialised value, not the source's -- the sweep
    // copies what was answered for and fabricates nothing.
    CHECK(rec.v.packetLossRatio == 0.0f);
    CHECK(rec.v.isEmotePlaying == 0);

    // ...and the mask that reaches the format is what makes the absence sentinels appear. Widening
    // it anywhere upstream publishes those two zeroes as a measurement.
    PlayerStatsRecord out = {};
    out.xplatformId = rec.xplatformId;
    out.engineTimeUs = rec.engineTimeUs;
    out.captureTimeUs = rec.sourceTimeUs;
    out.points = rec.v.points;
    out.goals = rec.v.goals;
    out.ping = rec.v.ping;
    out.packetLossRatio = rec.v.packetLossRatio;
    out.isEmotePlaying = rec.v.isEmotePlaying;
    out.fill = rec.fill;
    out.userIndex = rec.userIndex;
    out.keyframe = rec.keyframe;

    PlayerStatsRecordDisk disk = {};
    PlayerStatsToDisk(out, disk);
    CHECK(disk.packetLossRatio == kPlayerStatFloatNone);
    CHECK(disk.isEmotePlaying == kPlayerStatEmoteNone);
    CHECK(disk.points == 12);
}

TEST_CASE("a bit going out is a change even when every value stands still") {
    identity_access::PlayerStats a = {};
    a.points = 5;
    CHECK_FALSE(StatsChanged(a, kPsFillPoints, a, kPsFillPoints));
    CHECK(StatsChanged(a, uint16_t(kPsFillPoints | kPsFillGoals), a, kPsFillPoints));
    CHECK(StatsChanged(a, kPsFillPoints, a, uint16_t(kPsFillPoints | kPsFillGoals)));
}
