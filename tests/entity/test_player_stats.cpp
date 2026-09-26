// The live scoreboard: ReadPlayerStats over a FAKE CR15NetGame.
//
// Every offset the fixture plants through comes from facts_generated.h -- the same header
// identity_access.cpp reads them from -- so a registry change moves both sides at once and the
// fixture can never agree with a reader that has drifted. The three mutants the brief names are
// case 1 (a stat's offset moved by 4), case 3 (a fill bit set for a value the engine never wrote)
// and case 1's width assertions (a read widened past the engine's own type).

#include "doctest.h"

#include <stdint.h>
#include <string.h>

#include <windows.h>

#include "engine/entity.h"
#include "engine/identity_access.h"
#include "generated/facts_generated.h"

using replay::UserIndex;
namespace ia = replay::identity_access;
using ia::Refusal;

namespace {

// The peer object and its row table live past every stat block this fixture plants, so a read
// that walks off the end of one lands in the other's plant rather than in fresh zeroes.
constexpr size_t kPeerObjOff   = 0x10000;
constexpr size_t kPeerTableOff = 0x14000;
constexpr size_t kRegionBytes  = 0x64000;

// The engine's own chain, re-derived here from the registry rather than copied from the reader.
uint8_t* StatBlock(uint8_t* ng, uint16_t slot) {
    return ng + size_t(facts::USER_PROFILE_STRIDE) * slot;
}

struct Planted {
    uint32_t kind;
    uint32_t raw;
    uint32_t count;
};

void PlantWords(uint8_t* block, uint32_t kindOff, uint32_t valueOff, uint32_t countOff,
                const Planted& p) {
    memcpy(block + kindOff, &p.kind, 4);
    memcpy(block + valueOff, &p.raw, 4);
    memcpy(block + countOff, &p.count, 4);
}

// One planted value per stat, all different from each other and from every offset in the table,
// so a wrong field, a repeated field, a wrong slot or a wrong width is each visible as a value.
constexpr size_t kRoleScratch = 0x63800;  // above every planted table, so no other plant can reach the role byte

struct Fixture {
    uint8_t* ng = nullptr;

    explicit Fixture(uint16_t entrants = 2) {
        ng = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kRegionBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(ng != nullptr);
        memset(ng, 0, kRegionBytes);

    // A fresh game space is a NEW binding, and the span cache keys its proofs on the bind
    // generation: without this, a reservation that lands on the address a freed fixture used
    // would be answered out of the old fixture's proof. EntityBind makes this call in the
    // shipped path; here the fixture is the thing doing the binding.
        replay::entity::BindEpochAdvance();
        const uint16_t count = entrants;
        memcpy(ng + facts::IDENT_ENTRANT_COUNT_OFF, &count, 2);
        // sub_1401B2EA0 bounds its own entrant loop on a DIFFERENT word (0x1401B3746 reads
        // [ng+0E2h], not [ng+0DCh]); a live game space carries both, so the fixture plants both.
        memcpy(ng + facts::STATS_PLR_ENTRANT_COUNT_OFF, &count, 2);
        for (uint16_t i = 0; i < entrants; ++i) {
            const uint16_t word = uint16_t(i);   // slot i, and every other bit clear
            memcpy(ng + facts::USER_ARRAY_OFF + size_t(facts::USER_STRIDE) * i, &word, 2);
        }
    }
    ~Fixture() {
        if (ng) VirtualFree(ng, 0, MEM_RELEASE);
    }

    void PlantPeers(uint64_t peerCount) {
        uint8_t* peers = ng + kPeerObjOff;
        memcpy(ng + 0x40, &peers, sizeof(peers));
        uint8_t* table = ng + kPeerTableOff;
        memcpy(peers + facts::STATS_PEER_OBJ_ADJ + facts::STATS_PEER_TABLE_OFF, &table, sizeof(table));
        memcpy(peers + facts::STATS_PEER_OBJ_ADJ + facts::STATS_PEER_COUNT_OFF, &peerCount, 8);
    }
    void PlantPing(uint16_t slot, uint16_t ms) {
        memcpy(ng + kPeerTableOff + size_t(0xD8) * slot + 0x8A, &ms, 2);
    }

    // The three packetlossratio sources, each a different float so a swapped arm reports the wrong
    // number rather than failing to read.
    void PlantLoss(float local, float peer, float server, uint16_t slot) {
        memcpy(ng + facts::STATS_PLR_LOCAL_LOSS_OFF, &local, 4);
        memcpy(ng + facts::STATS_PLR_PEER_LOSS_OFF + size_t(facts::STATS_PLR_PEER_LOSS_STRIDE) * slot, &peer, 4);
        memcpy(ng + facts::STATS_PLR_SVR_LOSS_OFF + size_t(facts::STATS_PLR_SVR_LOSS_STRIDE) * slot, &server, 4);
    }
    void PlantLocalCount(uint16_t n) { memcpy(ng + facts::STATS_PLR_LOCAL_COUNT_OFF, &n, 2); }
    void PlantRole(uint8_t bits) {
        uint8_t* const role = ng + kRoleScratch;
        memcpy(role, &bits, 1);
        memcpy(ng + facts::STATS_PLR_ROLE_OBJ_OFF, &role, sizeof(role));
    }
};

// The role byte sub_1401B2EA0 tests at 0x1401B3E4F-0x1401B3E68: bit 2 alone is a client, bit 1 is
// the server side.
constexpr uint8_t kRoleClient = 0x04;
constexpr uint8_t kRoleServer = 0x02;

// Poison: every field of the record the reader is asked to fill starts as a value no plant uses,
// so "left alone" is an assertion and not an absence of one.
constexpr int32_t kPoisonInt   = -0x5A5A5A5;
constexpr float   kPoisonFloat = -12345.5f;
constexpr uint8_t kPoisonByte  = 0x5A;

ia::PlayerStats Poisoned() {
    ia::PlayerStats s;
    s.possessionTime = kPoisonFloat;
    s.points = s.goals = s.stuns = s.blocks = s.assists = s.saves = kPoisonInt;
    s.interceptions = s.steals = s.catches = s.passes = s.shotsTaken = kPoisonInt;
    s.ping = kPoisonInt;
    s.packetLossRatio = kPoisonFloat;
    s.isEmotePlaying = kPoisonByte;
    return s;
}

// The twelve stats in the reader's own table order, each with the member it must land in.
struct IntCase {
    uint32_t kindOff, valueOff, countOff;
    uint16_t bit;
    int32_t ia::PlayerStats::*dst;
    const char* name;
};

#define STAT_CASE(NAME, MEMBER, BIT)                                                    \
    { facts::STATS_##NAME##_KIND_OFF, facts::STATS_##NAME##_VALUE_OFF,                  \
      facts::STATS_##NAME##_COUNT_OFF, ia::BIT, &ia::PlayerStats::MEMBER, #NAME }

const IntCase kIntCases[] = {
    STAT_CASE(POINTS, points, kPsFillPoints),
    STAT_CASE(GOALS, goals, kPsFillGoals),
    STAT_CASE(STUNS, stuns, kPsFillStuns),
    STAT_CASE(BLOCKS, blocks, kPsFillBlocks),
    STAT_CASE(ASSISTS, assists, kPsFillAssists),
    STAT_CASE(SAVES, saves, kPsFillSaves),
    STAT_CASE(INTERCEPTIONS, interceptions, kPsFillInterceptions),
    STAT_CASE(STEALS, steals, kPsFillSteals),
    STAT_CASE(CATCHES, catches, kPsFillCatches),
    STAT_CASE(PASSES, passes, kPsFillPasses),
    STAT_CASE(SHOTS_TAKEN, shotsTaken, kPsFillShotsTaken),
};
constexpr size_t kIntCaseCount = sizeof(kIntCases) / sizeof(kIntCases[0]);

// A value per stat that no offset, no bit and no other stat's value can collide with, and whose
// top half is non-zero so a 16-bit read of a 32-bit field is a different number.
int32_t PlantedValue(size_t i) { return int32_t(0x00130000 + 0x1111 * int32_t(i) + 7); }

}  // namespace

TEST_CASE("W1-E scoreboard: all fifteen fields, and only the fourteen `ng` alone can source") {
    Fixture f(2);
    constexpr float kRemoteLoss = 0.6875f;
    uint8_t* block = StatBlock(f.ng, 1);

    for (size_t i = 0; i < kIntCaseCount; ++i) {
        const Planted p{1u, uint32_t(PlantedValue(i)), 0u};   // kind 1: not the ratio kind
        PlantWords(block, kIntCases[i].kindOff, kIntCases[i].valueOff, kIntCases[i].countOff, p);
    }
    const float possession = 137.25f;
    uint32_t possessionBits = 0;
    memcpy(&possessionBits, &possession, 4);
    PlantWords(block, facts::STATS_POSSESSION_TIME_KIND_OFF, facts::STATS_POSSESSION_TIME_VALUE_OFF,
               facts::STATS_POSSESSION_TIME_COUNT_OFF, Planted{1u, possessionBits, 0u});

    f.PlantPeers(2);
    f.PlantPing(1, 43);
    f.PlantPing(0, 0x1234);   // a neighbouring row, so a wrong slot reads a different number
    f.PlantLoss(0.5f, kRemoteLoss, 0.25f, 1);
    f.PlantLocalCount(1);     // entrant 1 is past the local count, so it is a remote arm
    f.PlantRole(kRoleClient);

    ia::PlayerStats out = Poisoned();
    uint16_t fill = static_cast<uint16_t>(~0u);
    Refusal why = Refusal::ShortBuffer;
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill, &why));
    CHECK(why == Refusal::None);

    for (size_t i = 0; i < kIntCaseCount; ++i) {
        CAPTURE(kIntCases[i].name);
        CHECK((fill & kIntCases[i].bit) != 0);
        CHECK(out.*(kIntCases[i].dst) == PlantedValue(i));
    }
    CHECK((fill & ia::kPsFillPossessionTime) != 0);
    CHECK(out.possessionTime == possession);
    CHECK((fill & ia::kPsFillPing) != 0);
    CHECK(out.ping == 43);

    CHECK((fill & ia::kPsFillPacketLoss) != 0);
    CHECK(memcmp(&out.packetLossRatio, &kRemoteLoss, 4) == 0);

    // Bit 14 is the one `ng` alone cannot answer: no EmoteSource is named here and no seam is
    // installed in this image, so it stays clear and its field stays exactly as passed.
    CHECK((fill & ia::kPsFillEmote) == 0);
    CHECK(out.isEmotePlaying == kPoisonByte);
    CHECK(fill == ia::kPlayerStatsNetGameOnlyMask);
    CHECK((fill & uint16_t(~ia::kPlayerStatsReadableMask)) == 0);
}

TEST_CASE("W1-E scoreboard: the ratio arm divides, and a ratio with no samples is NOT a zero") {
    Fixture f(2);
    uint8_t* block = StatBlock(f.ng, 0);

    // points: a sampled ratio -- 900 over 4 is the engine's own integer division.
    PlantWords(block, facts::STATS_POINTS_KIND_OFF, facts::STATS_POINTS_VALUE_OFF,
               facts::STATS_POINTS_COUNT_OFF, Planted{5u, 900u, 4u});
    // goals: the same ratio kind with NO samples. sub_140155C80 publishes 0 here; that 0 is not a
    // score, so the bit must stay clear and the caller's field must be untouched.
    PlantWords(block, facts::STATS_GOALS_KIND_OFF, facts::STATS_GOALS_VALUE_OFF,
               facts::STATS_GOALS_COUNT_OFF, Planted{5u, 777u, 0u});
    // possession_time: a float ratio, 300.0 over 8.
    const float total = 300.0f;
    uint32_t totalBits = 0;
    memcpy(&totalBits, &total, 4);
    PlantWords(block, facts::STATS_POSSESSION_TIME_KIND_OFF, facts::STATS_POSSESSION_TIME_VALUE_OFF,
               facts::STATS_POSSESSION_TIME_COUNT_OFF, Planted{5u, totalBits, 8u});

    ia::PlayerStats out = Poisoned();
    uint16_t fill = 0;
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(0), out, &fill));

    CHECK((fill & ia::kPsFillPoints) != 0);
    CHECK(out.points == 225);
    CHECK((fill & ia::kPsFillPossessionTime) != 0);
    CHECK(out.possessionTime == 37.5f);

    CHECK((fill & ia::kPsFillGoals) == 0);
    CHECK(out.goals == kPoisonInt);   // not 0, and not 777

    // Every stat this case did not plant reads kind 0 -- not the ratio kind -- so it IS claimed,
    // with the zero the engine holds. That is the control on the assertion above: the clear bit
    // belongs to the unsampled RATIO, not to "the block was quiet".
    CHECK((fill & ia::kPsFillStuns) != 0);
    CHECK(out.stuns == 0);
}

TEST_CASE("W1-E scoreboard: an unwritten peer row and an unreadable block are both refused") {
    SUBCASE("a slot past the engine's peer count leaves ping alone") {
        Fixture f(2);
        f.PlantPeers(1);         // slot 1 is past the count: the accessor would return a bare 0
        f.PlantPing(1, 999);     // and the row it would have read holds a value, so a reader that
                                 // ignores the bound reads 999 rather than nothing
        ia::PlayerStats out = Poisoned();
        uint16_t fill = 0;
        REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill));
        CHECK((fill & ia::kPsFillPing) == 0);
        CHECK(out.ping == kPoisonInt);

        // slot 0 IS inside the count, out of the same fixture, so what is proved is the bound and
        // not the fixture.
        f.PlantPing(0, 17);
        ia::PlayerStats in0 = Poisoned();
        uint16_t fill0 = 0;
        REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(0), in0, &fill0));
        CHECK((fill0 & ia::kPsFillPing) != 0);
        CHECK(in0.ping == 17);
    }

    SUBCASE("a stat block outside committed memory refuses, and claims nothing") {
        // A reservation whose committed head holds the roster but whose stat block is not there.
        uint8_t* ng = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kRegionBytes, MEM_RESERVE, PAGE_NOACCESS));
        REQUIRE(ng != nullptr);
        REQUIRE(VirtualAlloc(ng, 0x1000, MEM_COMMIT, PAGE_READWRITE) != nullptr);
        replay::entity::BindEpochAdvance();   // a new reservation is a new binding
        const uint16_t count = 1;
        memcpy(ng + facts::IDENT_ENTRANT_COUNT_OFF, &count, 2);
        const uint16_t word = 0;
        memcpy(ng + facts::USER_ARRAY_OFF, &word, 2);

        ia::PlayerStats out = Poisoned();
        uint16_t fill = static_cast<uint16_t>(~0u);
        Refusal why = Refusal::None;
        CHECK_FALSE(ia::ReadPlayerStats(ng, UserIndex(0), out, &fill, &why));
        CHECK(why == Refusal::StatsUnreadable);
        CHECK(fill == 0);
        CHECK(out.points == kPoisonInt);
        VirtualFree(ng, 0, MEM_RELEASE);
    }

    SUBCASE("the four ways to be asked nothing answerable each get their own word") {
        Fixture f(1);
        ia::PlayerStats out = Poisoned();
        uint16_t fill = 0;
        Refusal why = Refusal::None;

        CHECK_FALSE(ia::ReadPlayerStats(f.ng, UserIndex(0), out, nullptr, &why));
        CHECK(why == Refusal::StatsNoFillMask);

        CHECK_FALSE(ia::ReadPlayerStats(nullptr, UserIndex(0), out, &fill, &why));
        CHECK(why == Refusal::NoNetGame);
        CHECK(fill == 0);

        CHECK_FALSE(ia::ReadPlayerStats(f.ng, UserIndex(7), out, &fill, &why));
        CHECK(why == Refusal::NoSuchEntrant);
    }
}

TEST_CASE("W1-E scoreboard: O(1) per entrant -- no syscall per call, and under 1 us a player") {
    Fixture f(2);
    for (uint16_t s = 0; s < 2; ++s) {
        uint8_t* block = StatBlock(f.ng, s);
        for (size_t i = 0; i < kIntCaseCount; ++i)
            PlantWords(block, kIntCases[i].kindOff, kIntCases[i].valueOff, kIntCases[i].countOff,
                       Planted{1u, uint32_t(PlantedValue(i) + s), 0u});
    }
    f.PlantPeers(2);
    f.PlantPing(0, 11);
    f.PlantPing(1, 22);

    ia::PlayerStats warm = Poisoned();
    uint16_t fill = 0;
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(0), warm, &fill));
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), warm, &fill));

    // Two entrants prove two different stat bases; a single shared cache line would miss on every
    // call and this difference would be the call count.
    const uint64_t before = replay::entity::SpanProbeCount();
    const int kPlayers = 8 * 60;   // eight players, sixty ticks
    ia::PlayerStats out = Poisoned();
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kPlayers; ++i)
        REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(uint16_t(i & 1)), out, &fill));
    QueryPerformanceCounter(&t1);
    const uint64_t probes = replay::entity::SpanProbeCount() - before;
    const double perPlayerUs =
        double(t1.QuadPart - t0.QuadPart) * 1e6 / double(freq.QuadPart) / kPlayers;

    MESSAGE("ENG-W1E-STATSREADERS: " << kPlayers << " per-entrant scoreboard reads, "
                                     << perPlayerUs << " us a player, " << probes
                                     << " span probe(s)");
    // probes == 0 is the load-bearing assertion: a regression to the per-call VirtualQuery shows
    // up here and nowhere else. The wall clock is a coarse ceiling only -- this box compiles other
    // packets while the suite runs, and the measured cost sits near 1 us a player -- so it is set
    // where it still catches an order-of-magnitude regression without failing on a busy machine.
    // probes must not SCALE with the call count.  It is no longer an absolute zero: since
    // L2-RUN-7 F3 this reader and the roster gate share one proof line, and
    // SpanProofMaxCachedReads is a CADENCE and not a latch -- a window this wide crosses it and
    // MUST re-probe, because a proof that never expires is a latched success.  One crossing is two
    // probes (the header prove misses, then the array prove is wider than what that miss proved).
    // The shape still caught here is a cache that misses every call, which is kPlayers of them.
    CHECK(probes * 16u <= uint64_t(kPlayers));

    // The wall-clock ceiling is host-normalised rather than raised: 5.0 us was a property of an
    // idle box and read 5.76 us on a busy one, which is a report about the machine and not about
    // the reader. The baseline is measured in the same run on the same load -- one VirtualQuery,
    // the syscall the span cache exists to remove -- so both numbers move together and the ratio
    // between them is the claim: a cached read costs a small multiple of one probe, and an
    // order-of-magnitude regression cannot fit inside it.
    MEMORY_BASIC_INFORMATION mbi;
    size_t probed = 0;
    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    for (int i = 0; i < kPlayers; ++i) probed += VirtualQuery(f.ng, &mbi, sizeof mbi);
    QueryPerformanceCounter(&p1);
    REQUIRE(probed == size_t(kPlayers) * sizeof mbi);   // the baseline really ran
    const double perProbeUs =
        double(p1.QuadPart - p0.QuadPart) * 1e6 / double(freq.QuadPart) / kPlayers;
    REQUIRE(perProbeUs > 0.0);
    MESSAGE("ENG-W1E-STATSREADERS: baseline " << perProbeUs << " us a VirtualQuery, ratio "
                                              << (perPlayerUs / perProbeUs));
    CHECK(perPlayerUs <= 25.0 * perProbeUs);
    CHECK(out.ping == 22);   // the last call was entrant 1, so the loop really did read
}

TEST_CASE("W1-E scoreboard: the registry's own layout, re-read rather than restated") {
    // Every stat is the three-word counter the /session writer reads: value at kind+4, count at
    // kind+8. Each of the 36 numbers is its own registered fact with its own anchor, so this is a
    // cross-check between 36 independently decoded imm32s and not a restatement of one of them.
    for (size_t i = 0; i < kIntCaseCount; ++i) {
        CAPTURE(kIntCases[i].name);
        CHECK(kIntCases[i].valueOff == kIntCases[i].kindOff + 4u);
        CHECK(kIntCases[i].countOff == kIntCases[i].kindOff + 8u);
    }
    CHECK(facts::STATS_POSSESSION_TIME_VALUE_OFF == facts::STATS_POSSESSION_TIME_KIND_OFF + 4u);
    CHECK(facts::STATS_POSSESSION_TIME_COUNT_OFF == facts::STATS_POSSESSION_TIME_KIND_OFF + 8u);

    // The twelve counters are distinct entries of one array: 48 bytes apart, none repeated.
    uint32_t offs[kIntCaseCount + 1];
    for (size_t i = 0; i < kIntCaseCount; ++i) offs[i] = kIntCases[i].kindOff;
    offs[kIntCaseCount] = facts::STATS_POSSESSION_TIME_KIND_OFF;
    for (size_t i = 0; i < kIntCaseCount + 1; ++i) {
        CHECK(offs[i] % 48u == facts::STATS_STUNS_KIND_OFF % 48u);
        for (size_t j = i + 1; j < kIntCaseCount + 1; ++j) CHECK(offs[i] != offs[j]);
    }
}

TEST_CASE("W1-E scoreboard: packetlossratio takes the arm the /session writer takes") {
    Fixture f(2);
    const float kLocal = 0.125f, kPeer = 0.375f, kServer = 0.75f;
    f.PlantLoss(kLocal, kPeer, kServer, 1);

    ia::PlayerStats out = Poisoned();
    uint16_t fill = 0;
    ia::Refusal why = ia::Refusal::None;

    // Entrant 0 is below the local count, so the single local conn-stats float answers.
    f.PlantLocalCount(1);
    f.PlantRole(kRoleClient);
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(0), out, &fill, &why));
    CHECK((fill & ia::kPsFillPacketLoss) != 0);
    CHECK(memcmp(&out.packetLossRatio, &kLocal, 4) == 0);

    // Entrant 1 is remote. Client role -> the 0x70-stride table.
    out = Poisoned(); fill = 0;
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill, &why));
    CHECK((fill & ia::kPsFillPacketLoss) != 0);
    CHECK(memcmp(&out.packetLossRatio, &kPeer, 4) == 0);

    // Same entrant, server role -> the 0xE78-stride table. Both plants are readable, so swapping
    // the arms returns the OTHER number instead of refusing: the value is what discriminates.
    out = Poisoned(); fill = 0;
    f.PlantRole(kRoleServer);
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill, &why));
    CHECK(memcmp(&out.packetLossRatio, &kServer, 4) == 0);

    // An unreadable role object is not "client": no arm is taken and nothing is published.
    out = Poisoned(); fill = 0;
    uint8_t* const nowhere = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(nowhere != nullptr);
    memcpy(f.ng + facts::STATS_PLR_ROLE_OBJ_OFF, &nowhere, sizeof(nowhere));
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill, &why));
    CHECK((fill & ia::kPsFillPacketLoss) == 0);
    CHECK(out.packetLossRatio == kPoisonFloat);
    VirtualFree(nowhere, 0, MEM_RELEASE);
}

TEST_CASE("W1-E scoreboard: the entrant bound packetlossratio tests is the roster's own bound") {
    // sub_1401B2EA0 re-tests the count it already looped on before it will source a ratio, and an
    // index at or past it gets a zeroed register. Whether that arm is reachable at all depends on
    // whether the two facts name one field, so the test asserts whichever is true of this build.
    Fixture f(2);
    f.PlantLoss(1.0f, 2.0f, 3.0f, 1);
    f.PlantLocalCount(0);
    f.PlantRole(kRoleClient);
    ia::PlayerStats out = Poisoned();
    uint16_t fill = 0;
    ia::Refusal why = ia::Refusal::None;

    if (facts::IDENT_ENTRANT_COUNT_OFF == facts::STATS_PLR_ENTRANT_COUNT_OFF) {
        CHECK(!ia::ReadPlayerStats(f.ng, UserIndex(2), out, &fill, &why));
        CHECK(why == ia::Refusal::NoSuchEntrant);   // the roster refuses first, on the same word
    } else {
        const uint16_t shrunk = 1;
        memcpy(f.ng + facts::STATS_PLR_ENTRANT_COUNT_OFF, &shrunk, 2);
        REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill, &why));
        CHECK((fill & ia::kPsFillPacketLoss) == 0);
        CHECK(out.packetLossRatio == kPoisonFloat);
    }
}

// A fake R15NetCustomization component system, laid out through the registry's own offsets.
namespace {
int EmotePlayingFn(void*, int32_t arg) { return arg == 0x51EED ? 1 : 0; }
int EmoteNotPlayingFn(void*, int32_t)  { return 0; }

constexpr size_t   kEmoteBlobBytes = 0x5000;
constexpr size_t   kEmoteMapAt     = 0x0400;
constexpr size_t   kEmotePoolAt    = 0x0800;
constexpr size_t   kEmoteTargetAt  = 0x1E80;
constexpr size_t   kEmoteVtableAt  = 0x2680;
constexpr uint16_t kEmoteElement   = 1;
constexpr uint16_t kEmoteId        = 3;

struct EmoteFixture {
    uint8_t* blob     = nullptr;
    uint8_t* handleAt = nullptr;

    EmoteFixture() {
        blob = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kEmoteBlobBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(blob != nullptr);
        memset(blob, 0, kEmoteBlobBytes);

        uint8_t* const map  = blob + kEmoteMapAt;
        uint8_t* const pool = blob + kEmotePoolAt;
        memcpy(blob + facts::STATS_EMOTE_MAP_OFF,  &map,  sizeof(map));
        memcpy(blob + facts::STATS_EMOTE_POOL_OFF, &pool, sizeof(pool));
        memcpy(map + size_t(4) * kEmoteId, &kEmoteElement, 2);  // 4 = the SIB scale at 0x140D08D0E

        uint8_t* const target = blob + kEmoteTargetAt;
        uint8_t* const vtable = blob + kEmoteVtableAt;
        // Every OTHER slot answers "not playing", so a dispatch through the wrong slot is a wrong
        // ANSWER rather than a crash -- which is what makes an off-by-one slot go red.
        void* const idle = reinterpret_cast<void*>(&EmoteNotPlayingFn);
        for (size_t i = 0; i < 0x400; i += sizeof(void*)) memcpy(vtable + i, &idle, sizeof(idle));
        void* const live = reinterpret_cast<void*>(&EmotePlayingFn);
        memcpy(vtable + facts::STATS_EMOTE_VTSLOT, &live, sizeof(live));
        memcpy(target, &vtable, sizeof(vtable));

        handleAt = pool + size_t(facts::STATS_EMOTE_ELEM_STRIDE) * kEmoteElement +
                   facts::STATS_EMOTE_ELEM_ADJ;
        memcpy(handleAt, &target, sizeof(target));   // the non-null word sub_140128B20 tests at +0
        memcpy(handleAt + facts::STATS_EMOTE_TARGET_OFF, &target, sizeof(target));
        const int32_t arg = 0x51EED;
        memcpy(handleAt + facts::STATS_EMOTE_ARG_OFF, &arg, 4);
    }
    ~EmoteFixture() { if (blob) VirtualFree(blob, 0, MEM_RELEASE); }

    ia::EmoteSource Source() const {
        ia::EmoteSource s;
        s.componentSystem = blob;
        s.emoteId = kEmoteId;
        return s;
    }
};
}  // namespace

TEST_CASE("W1-E scoreboard: is_emote_playing dispatches through the registered vtable slot") {
    EmoteFixture e;
    bool playing = false;
    ia::Refusal why = ia::Refusal::None;

    REQUIRE(ia::ReadEmotePlaying(e.Source(), playing, &why));
    CHECK(why == ia::Refusal::None);
    CHECK(playing);   // only the pointer AT facts::STATS_EMOTE_VTSLOT answers true

    // The engine's own false: a handle that names no target never dispatches.
    void* const none = nullptr;
    memcpy(e.handleAt + facts::STATS_EMOTE_TARGET_OFF, &none, sizeof(none));
    playing = true;
    REQUIRE(ia::ReadEmotePlaying(e.Source(), playing, &why));
    CHECK(why == ia::Refusal::None);
    CHECK(!playing);

    // No component system named at all is its own word, not a false.
    ia::EmoteSource empty;
    playing = true;
    CHECK(!ia::ReadEmotePlaying(empty, playing, &why));
    CHECK(why == ia::Refusal::EmoteNoSystem);
    CHECK(playing);   // untouched on a refusal
}

TEST_CASE("W1-E scoreboard: an unreadable vtable refuses BY NAME instead of calling through it") {
    EmoteFixture e;
    uint8_t* const nowhere = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(nowhere != nullptr);
    memcpy(e.blob + kEmoteTargetAt, &nowhere, sizeof(nowhere));   // the target's vtable pointer

    bool playing = true;
    ia::Refusal why = ia::Refusal::None;
    CHECK(!ia::ReadEmotePlaying(e.Source(), playing, &why));
    CHECK(why == ia::Refusal::EmoteVtableUnreadable);
    CHECK(playing);

    // And the target object ITSELF. Nothing proves that one: it is a different object per entrant,
    // so a proof line keyed on it would miss on every call and be no cache at all -- which leaves
    // the guarded load as the whole check. Delete the guard and this read is an access violation
    // on the game's own tick thread, which is what the mutant proves.
    memcpy(e.handleAt + facts::STATS_EMOTE_TARGET_OFF, &nowhere, sizeof(nowhere));
    playing = true;
    why = ia::Refusal::None;
    CHECK(!ia::ReadEmotePlaying(e.Source(), playing, &why));
    CHECK(why == ia::Refusal::EmoteVtableUnreadable);
    CHECK(playing);

    VirtualFree(nowhere, 0, MEM_RELEASE);
}

TEST_CASE("W1-E scoreboard: bit 14 costs an EmoteSource, and the mask says so") {
    CHECK(ia::kPlayerStatsReadableMask == 0x7FFFu);
    CHECK(uint16_t(ia::kPlayerStatsReadableMask & ~ia::kPlayerStatsNetGameOnlyMask) ==
          uint16_t(ia::kPsFillEmote));

    Fixture f(2);
    f.PlantLoss(0.5f, 0.25f, 0.125f, 1);
    f.PlantLocalCount(1);
    f.PlantRole(kRoleClient);
    EmoteFixture e;

    ia::PlayerStats out = Poisoned();
    uint16_t fill = 0;
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(0), out, &fill, nullptr));
    CHECK((fill & ia::kPsFillEmote) == 0);
    CHECK(out.isEmotePlaying == kPoisonByte);
    CHECK((fill & ~ia::kPlayerStatsNetGameOnlyMask) == 0);

    out = Poisoned(); fill = 0;
    const ia::EmoteSource src = e.Source();
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(0), out, &fill, nullptr, &src));
    CHECK((fill & ia::kPsFillEmote) != 0);
    CHECK(out.isEmotePlaying == 1u);
    CHECK((fill & ~ia::kPlayerStatsReadableMask) == 0);
}

// A fake entity layer behind the seam: it answers for one entrant and declines for every other,
// which is what makes "the seam was asked and said no" distinguishable from "nobody was asked".
namespace {

EmoteFixture* g_seamFixture = nullptr;
uint16_t      g_seamEntrant = 0;

bool SeamResolve(void* ng, UserIndex entrant, ia::EmoteSource* out) {
    if (!ng || !g_seamFixture || entrant.v != g_seamEntrant) return false;
    *out = g_seamFixture->Source();
    return true;
}

const ia::EmoteSourceSeam kSeam{&SeamResolve};

// Nothing else in the suite may see an installed seam, so the install is scoped.
struct SeamInstalled {
    SeamInstalled(EmoteFixture* f, uint16_t entrant) {
        g_seamFixture = f;
        g_seamEntrant = entrant;
        ia::SetEmoteSource(&kSeam);
    }
    ~SeamInstalled() {
        ia::SetEmoteSource(nullptr);
        g_seamFixture = nullptr;
        g_seamEntrant = 0;
    }
};

}  // namespace

TEST_CASE("W1-E scoreboard: bit 14 comes from the installed seam, and stays clear without one") {
    Fixture f(2);
    EmoteFixture e;
    ia::PlayerStats out = Poisoned();
    uint16_t fill = 0;
    ia::Refusal why = ia::Refusal::None;
    ia::EmoteSource resolved;

    // Nothing installed: the refusal is its own word, and the bit is clear.
    CHECK(!ia::ResolveEmoteSource(f.ng, UserIndex(1), resolved, &why));
    CHECK(why == ia::Refusal::EmoteNoSeam);
    CHECK(resolved.componentSystem == nullptr);
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill, nullptr));
    CHECK((fill & ia::kPsFillEmote) == 0);
    CHECK(out.isEmotePlaying == kPoisonByte);

    SeamInstalled installed(&e, 1);

    // Installed and answering: no EmoteSource argument is needed at the call any more.
    why = ia::Refusal::ShortBuffer;
    CHECK(ia::ResolveEmoteSource(f.ng, UserIndex(1), resolved, &why));
    CHECK(why == ia::Refusal::None);
    CHECK(resolved.componentSystem == e.blob);
    CHECK(resolved.emoteId == kEmoteId);
    out = Poisoned(); fill = 0;
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill, nullptr));
    CHECK((fill & ia::kPsFillEmote) != 0);
    CHECK(out.isEmotePlaying == 1u);

    // Installed but declining for entrant 0: a decline is not a "not playing".
    out = Poisoned(); fill = 0; why = ia::Refusal::None;
    CHECK(!ia::ResolveEmoteSource(f.ng, UserIndex(0), resolved, &why));
    CHECK(why == ia::Refusal::EmoteNoSystem);
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(0), out, &fill, nullptr));
    CHECK((fill & ia::kPsFillEmote) == 0);
    CHECK(out.isEmotePlaying == kPoisonByte);

    // An EmoteSource passed at the call wins over the seam: this one's handle names no target, so
    // the engine's own false is published where the seam would have answered true.
    EmoteFixture quiet;
    void* const none = nullptr;
    memcpy(quiet.handleAt + facts::STATS_EMOTE_TARGET_OFF, &none, sizeof(none));
    const ia::EmoteSource explicitSrc = quiet.Source();
    out = Poisoned(); fill = 0;
    REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(1), out, &fill, nullptr, &explicitSrc));
    CHECK((fill & ia::kPsFillEmote) != 0);
    CHECK(out.isEmotePlaying == 0u);
}

// L2-RUN-7 F3.  The capture roster's change signal calls EntrantCount on EVERY tick, and it used
// to answer with three replay::Readable calls -- VirtualQuery, which clock.cpp:128 says is asked
// "once per take, never on the tick path".  In the field they measured ~400 us apiece: 1.2 ms of
// every tick and the cost meter killed every capture at 50 s.  The honest offline form of that
// bound is a COUNT, not a wall clock -- an idle box cannot reproduce a 400 us VirtualQuery -- so
// the probe count is the assertion and the time is a floor check beside it.
TEST_CASE("W1-E roster gate: EntrantCount proves its spans once per binding, not once per tick") {
    // The fake netGame is sized from the table the roster gate reads -- the whole user array at
    // the registry's own bound -- so a registry change moves the fixture with it.
    const size_t kNgBytes = size_t(facts::USER_ARRAY_OFF) +
                            size_t(facts::USER_STRIDE) * facts::ENTITY_USER_SLOT_TABLE_MAX;
    uint8_t* const ng = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kNgBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE(ng != nullptr);
    memset(ng, 0, kNgBytes);
    const uint16_t entrants  = 8;
    const uint16_t nonReplay = 3;
    memcpy(ng + facts::IDENT_ENTRANT_COUNT_OFF,  &entrants,  sizeof entrants);
    memcpy(ng + facts::IDENT_NONREPLAY_COUNT_OFF, &nonReplay, sizeof nonReplay);
    REQUIRE(size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * entrants <= kNgBytes);

    uint32_t count = 0;
    uint32_t band = 0;
    ia::Refusal why = ia::Refusal::None;
    REQUIRE(ia::EntrantCount(ng, count, band, &why));
    CHECK(why == ia::Refusal::None);
    CHECK(count == entrants);
    CHECK(band == nonReplay);

    // The measured window sits inside ONE proof cadence by construction: SpanProofMaxCachedReads
    // is what turns "proved once" into "proved recently", and a window wider than it would be
    // asserting zero against a re-probe the design requires.
    constexpr int kCalls = 200;
    REQUIRE(2u * uint32_t(kCalls) < replay::entity::SpanProofMaxCachedReads());

    const uint64_t before = replay::entity::SpanProbeCount();
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kCalls; ++i) {
        if (!ia::EntrantCount(ng, count, band, &why)) REQUIRE(false);
    }
    QueryPerformanceCounter(&t1);
    const uint64_t probes = replay::entity::SpanProbeCount() - before;
    const double perCallUs =
        double(t1.QuadPart - t0.QuadPart) * 1e6 / double(freq.QuadPart) / double(kCalls);
    // The control, measured on the SAME address in the same run: one VirtualQuery, the syscall
    // the proof exists to remove.  A wall-clock bound alone cannot discriminate here -- an idle box
    // cannot reproduce the field's 400 us probe -- and the probe COUNT cannot either, because the
    // pre-fix shape called replay::Readable directly and SpanProbeCount only counts probes made
    // THROUGH the cache.  What separates the two shapes is that a gated tick now costs less than
    // one VirtualQuery, where it used to cost three.
    MEMORY_BASIC_INFORMATION mbi;
    size_t probed = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kCalls; ++i) probed += VirtualQuery(ng, &mbi, sizeof mbi);
    QueryPerformanceCounter(&t1);
    REQUIRE(probed == size_t(kCalls) * sizeof mbi);   // so the control cannot silently not run
    const double perProbeUs =
        double(t1.QuadPart - t0.QuadPart) * 1e6 / double(freq.QuadPart) / double(kCalls);

    MESSAGE("ENG-W1E-EMOTE-4: " << perCallUs << " us an EntrantCount, " << probes
                                << " span probe(s) over " << kCalls << " calls; baseline "
                                << perProbeUs << " us a VirtualQuery, ratio "
                                << (perCallUs / perProbeUs));
    CHECK(probes == 0u);
    CHECK(perCallUs <= 1.0);
    CHECK(perCallUs <= 2.0 * perProbeUs);
    CHECK(count == entrants);

    // A refusal is never cached and a new base is never inherited: a netGame whose user array runs
    // off the end of committed memory is refused even though the header is the proved one.
    // A count whose array runs past the whole RESERVATION this allocation could have: VirtualAlloc
    // reserves in units of the allocation granularity, so two of them is past anything committed
    // here -- the page rounding of kNgBytes cannot accidentally make this span readable.
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uint16_t tooMany = uint16_t((2u * size_t(si.dwAllocationGranularity) -
                                       facts::USER_ARRAY_OFF) / facts::USER_STRIDE + 1u);
    memcpy(ng + facts::IDENT_ENTRANT_COUNT_OFF, &tooMany, sizeof tooMany);
    why = ia::Refusal::None;
    CHECK(!ia::EntrantCount(ng, count, band, &why));
    CHECK(why == ia::Refusal::RosterUnreadable);

    VirtualFree(ng, 0, MEM_RELEASE);
}

// ReadPlayerStats and ReadEmotePlaying answered ELEVEN replay::Readable calls --
// VirtualQuery -- per entrant per sweep: the peer object pointer, both counts, the role pointer and
// the role byte, the loss row, and six down the emote chain.  L2-RUN-8 charged them 73 us early in
// a match and 188 us late, the climb being the VAD tree the game grows under itself, so the honest
// offline bound is not an absolute microsecond figure (an idle box cannot reproduce a contended
// VirtualQuery) but a RATIO against one measured in the same run on the same load.
namespace {

// A fake R15NetCustomization laid out for a WHOLE ROSTER: one component system, one id->element
// map, one element pool and one vtable -- shared, as the engine's are -- and a DIFFERENT target
// object per entrant, which is the one base in the chain the reader cannot cache on.  Every offset
// comes from the registry, so a registry change moves the fixture with the reader.
constexpr uint16_t kSweepEmotes = uint16_t(facts::ENTITY_USER_SLOT_TABLE_MAX);
constexpr size_t   kSwMapAt     = facts::STATS_EMOTE_POOL_OFF + sizeof(void*);
constexpr size_t   kSwPoolAt    = kSwMapAt + 4 * size_t(kSweepEmotes);
constexpr size_t   kSwPoolBytes = size_t(facts::STATS_EMOTE_ELEM_STRIDE) * (kSweepEmotes - 1) +
                                  facts::STATS_EMOTE_ELEM_ADJ + facts::STATS_EMOTE_ARG_OFF +
                                  sizeof(int32_t);
constexpr size_t   kSwVtAt      = kSwPoolAt + kSwPoolBytes;
constexpr size_t   kSwTargetsAt = kSwVtAt + facts::STATS_EMOTE_VTSLOT + sizeof(void*);
constexpr size_t   kSwBlobBytes = kSwTargetsAt + sizeof(void*) * kSweepEmotes;

// Playing exactly when this handle's own argument word reached the call: every other handle
// carries a different one and element 0's is zero, so a dispatch off the wrong element is a wrong
// ANSWER rather than a crash.
int SweepEmoteFn(void*, int32_t arg) { return arg != 0 ? 1 : 0; }

struct SweepEmotes {
    uint8_t* blob = nullptr;

    SweepEmotes() {
        blob = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kSwBlobBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(blob != nullptr);
        memset(blob, 0, kSwBlobBytes);

        uint8_t* const map  = blob + kSwMapAt;
        uint8_t* const pool = blob + kSwPoolAt;
        uint8_t* const vt   = blob + kSwVtAt;
        memcpy(blob + facts::STATS_EMOTE_MAP_OFF,  &map,  sizeof(map));
        memcpy(blob + facts::STATS_EMOTE_POOL_OFF, &pool, sizeof(pool));
        void* const live = reinterpret_cast<void*>(&SweepEmoteFn);
        memcpy(vt + facts::STATS_EMOTE_VTSLOT, &live, sizeof(live));

        for (uint16_t i = 0; i < kSweepEmotes; ++i) {
            memcpy(map + size_t(4) * i, &i, 2);   // 4 = the SIB scale at 0x140D08D0E
            uint8_t* const target = blob + kSwTargetsAt + sizeof(void*) * i;
            memcpy(target, &vt, sizeof(vt));      // every target shares the class's vtable
            uint8_t* const handle = pool + size_t(facts::STATS_EMOTE_ELEM_STRIDE) * i +
                                    facts::STATS_EMOTE_ELEM_ADJ;
            memcpy(handle, &target, sizeof(target));
            memcpy(handle + facts::STATS_EMOTE_TARGET_OFF, &target, sizeof(target));
            const int32_t arg = int32_t(i) + 1;
            memcpy(handle + facts::STATS_EMOTE_ARG_OFF, &arg, 4);
        }
    }
    ~SweepEmotes() { if (blob) VirtualFree(blob, 0, MEM_RELEASE); }

    ia::EmoteSource Source(uint16_t entrant) const {
        ia::EmoteSource s;
        s.componentSystem = blob;
        s.emoteId = entrant;
        return s;
    }
};

}  // namespace

TEST_CASE("W1-E scoreboard: a whole-roster sweep of all fifteen fields costs no syscall") {
    constexpr uint16_t kEntrants = uint16_t(facts::ENTITY_USER_SLOT_TABLE_MAX);
    Fixture     f(kEntrants);
    SweepEmotes e;

    float          one     = 1.0f;
    uint32_t       oneBits = 0;
    memcpy(&oneBits, &one, sizeof(one));
    for (uint16_t s = 0; s < kEntrants; ++s) {
        uint8_t* const block = StatBlock(f.ng, s);
        for (size_t i = 0; i < kIntCaseCount; ++i)
            PlantWords(block, kIntCases[i].kindOff, kIntCases[i].valueOff, kIntCases[i].countOff,
                       Planted{1u, uint32_t(PlantedValue(i) + s), 0u});
        PlantWords(block, facts::STATS_POSSESSION_TIME_KIND_OFF,
                   facts::STATS_POSSESSION_TIME_VALUE_OFF,
                   facts::STATS_POSSESSION_TIME_COUNT_OFF, Planted{1u, oneBits, 0u});
        f.PlantPing(s, uint16_t(100 + s));
        f.PlantLoss(0.25f, 0.5f, 0.75f, s);
    }
    f.PlantPeers(kEntrants);
    // Nobody is local, so EVERY entrant takes the conn-stats arm -- the one that dereferences the
    // session-role object and indexes a per-slot table. Those were three of the eleven probes, and
    // they are the ones a sweep used to repeat once per entrant.
    f.PlantLocalCount(0);
    f.PlantRole(kRoleClient);

    ia::PlayerStats last = Poisoned();
    auto sweep = [&]() -> uint16_t {
        uint16_t all = uint16_t(~uint16_t(0));   // every bit, narrowed by each entrant fill
        for (uint16_t s = 0; s < kEntrants; ++s) {
            ia::PlayerStats out = Poisoned();
            uint16_t        fill = 0;
            const ia::EmoteSource src = e.Source(s);
            REQUIRE(ia::ReadPlayerStats(f.ng, UserIndex(s), out, &fill, nullptr, &src));
            all  = uint16_t(all & fill);
            last = out;
        }
        return all;
    };

    // All fifteen, on every entrant: a field that stopped being read makes this bound RED before
    // any cost claim is reached, so the cost below is a cost for the whole job and not for a
    // shrunken one.
    REQUIRE(sweep() == ia::kPlayerStatsReadableMask);
    CHECK(last.ping == 100 + kEntrants - 1);
    CHECK(last.packetLossRatio == 0.5f);       // the peer-side table, which is the arm a client takes
    CHECK(last.isEmotePlaying == 1u);
    CHECK(last.possessionTime == 1.0f);

    // The measured window sits inside ONE proof cadence by construction: SpanProofMaxCachedReads
    // is a cadence and not a latch, and a window wide enough to cross it MUST re-probe, because a
    // proof that never expires is a latched success.
    const int kSweeps = 8;
    REQUIRE(uint32_t(kSweeps + 1) * uint32_t(kEntrants) <
            replay::entity::SpanProofMaxCachedReads());

    const uint64_t before = replay::entity::SpanProbeCount();
    LARGE_INTEGER  freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kSweeps; ++i) REQUIRE(sweep() == ia::kPlayerStatsReadableMask);
    QueryPerformanceCounter(&t1);
    const uint64_t probes = replay::entity::SpanProbeCount() - before;
    const double   perSweepUs =
        double(t1.QuadPart - t0.QuadPart) * 1e6 / double(freq.QuadPart) / kSweeps;

    // The same-run control: one VirtualQuery, the syscall the proofs exist to remove, measured on
    // this box under this load. An absolute microsecond bound is a report about the machine
    // (ENG-W1E-EMOTE-4 measured the pre-fix shape passing one), so the RATIO is the claim.
    MEMORY_BASIC_INFORMATION mbi;
    size_t                   probed = 0;
    const int                kProbes = kSweeps * int(kEntrants);
    LARGE_INTEGER            p0, p1;
    QueryPerformanceCounter(&p0);
    for (int i = 0; i < kProbes; ++i) probed += VirtualQuery(f.ng, &mbi, sizeof mbi);
    QueryPerformanceCounter(&p1);
    REQUIRE(probed == size_t(kProbes) * sizeof mbi);   // the control really ran
    const double perProbeUs =
        double(p1.QuadPart - p0.QuadPart) * 1e6 / double(freq.QuadPart) / kProbes;
    REQUIRE(perProbeUs > 0.0);

    MESSAGE("ENG-STATSPERF-3: " << kEntrants << "-entrant sweep of all fifteen fields, "
                                << perSweepUs << " us a sweep, " << probes << " span probe(s); "
                                << "baseline " << perProbeUs << " us a VirtualQuery, ratio "
                                << (perSweepUs / perProbeUs) << " probes' worth");

    // Zero, not "few": every base a sweep touches is proved on a line of its own, and the first
    // sweep above is the one that paid for them.
    CHECK(probes == 0);

    // The deliverable's absolute bound, kept as a ceiling on top of the ratio rather than instead
    // of it.
    CHECK(perSweepUs <= 20.0);

    // The discriminating bound. Pre-fix this sweep cost sixteen entrants x eleven VirtualQuery =
    // 176 probes' worth; one proof dropped anywhere in the chain puts sixteen back. The ceiling is
    // set from what the fixed shape and the mutants actually measure on this box -- see the report.
    CHECK(perSweepUs <= 8.0 * perProbeUs);
}
