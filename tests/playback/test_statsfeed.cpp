// The stat feed: decode, level semantics, the header rewrite and the release.
//
// The decode half runs against the REAL capture through the REAL container reader -- a second
// decoder written here would be the defect the format exists to stop. `.echodemo` is gitignored,
// so a machine without the file gets a named skip, never a quiet pass.
//
// The feed half runs against a FAKE pair of handlers and a fake netGame whose only real content is
// the engine's own slot table, because that table is what the header rewrite is derived from. The
// fake records what it was handed, so "byte-exact payload" is an assertion and not a hope.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "bootstrap/livesafety.h"
#include "foundation/demofile/demofile.h"
#include "playback/core/statsfeed.h"
#include "playback/core/statsfeed_facts.h"
#include "playback/core/windowbuild.h"

using namespace replay;
using namespace replay::playback;
using replay::playback::statsfeed::MappedPlayer;
using replay::playback::statsfeed::Payload;
using replay::playback::statsfeed::RecordedStats;
using replay::playback::statsfeed::ScanCensus;

namespace {

namespace sf = replay::statsfeed_facts;

// What a handler call was handed. The payload is COPIED, so a later call cannot make an earlier
// assertion pass.
struct Call {
    bool team = false;
    uint32_t netUserId = 0;
    uint16_t teamId = 0;
    std::string blob;
    uint32_t blobLen = 0;
};

std::vector<Call> g_calls;
bool g_raise = false;

void FakePlayer(void* /*netGame*/, const void* msg) {
    const uint64_t* m = static_cast<const uint64_t*>(msg);
    if (g_raise) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    Call c;
    c.netUserId = *reinterpret_cast<const uint32_t*>(uintptr_t(m[2]));
    c.blobLen = uint32_t(m[4]);
    c.blob.assign(reinterpret_cast<const char*>(uintptr_t(m[3])), c.blobLen);
    g_calls.push_back(c);
}

void FakeTeam(void* /*netGame*/, const void* msg) {
    const uint64_t* m = static_cast<const uint64_t*>(msg);
    Call c;
    c.team = true;
    c.teamId = *reinterpret_cast<const uint16_t*>(uintptr_t(m[2]));
    c.blobLen = uint32_t(m[4]);
    c.blob.assign(reinterpret_cast<const char*>(uintptr_t(m[3])), c.blobLen);
    g_calls.push_back(c);
}

const statsfeed::Handlers kFake{&FakePlayer, &FakeTeam};

// A netGame that is nothing but the slot table the gate reads. Wide enough for the whole table so
// a read past the last element is still inside the object.
struct FakeNetGame {
    uint8_t bytes[sf::kSlotTableOff + 4u * sf::kSlotTableMax + 64] = {};

    void Put(uint16_t slot, uint16_t index, uint16_t gen) {
        uint8_t* at = bytes + sf::kSlotTableOff + 4u * slot;
        memcpy(at, &index, sizeof index);
        memcpy(at + 2, &gen, sizeof gen);
    }
    void Empty() {
        const uint16_t none = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        for (uint16_t s = 0; s < sf::kSlotTableMax; ++s) Put(s, none, none);
    }
    void* Ptr() { return bytes; }
};

// The feed is process-global, like every other playback store. Each case starts from a known one.
struct FeedFixture {
    FeedFixture() {
        g_calls.clear();
        g_raise = false;
        statsfeed::InjectHandlersForTest(&kFake);
        statsfeed::PlaybackRecordedStats().Reset();
        statsfeed::ResetCensusForTest();
        statsfeed::Arm(true);
        // The real offline lock is not held in a test process, so the verdict is OVERRIDDEN rather
        // than published: PublishBootFacts sets only the exemption half and Classify would still
        // read LockNotHeld off the live lock.
        livesafety::SetForTest(false, true);
    }
    ~FeedFixture() {
        livesafety::ResetForTest();
        statsfeed::Arm(false);
        statsfeed::InjectHandlersForTest(nullptr);
        g_calls.clear();
    }
};

const char kJsonA[] = "{\"Saves\":{\"op\":\"add\",\"val\":9}}";
const char kJsonB[] = "{\"Saves\":{\"op\":\"add\",\"val\":12}}";

void OfferPlayerAt(RecordedStats& s, uint64_t tUs, uint16_t slot, const char* json,
                   ScanCensus& c) {
    REQUIRE(s.OfferPlayer(tUs, slot, json, uint32_t(strlen(json)), c));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------------------------

TEST_CASE("the newest row at or before the playhead is what a read answers, in both directions") {
    RecordedStats s;
    ScanCensus c;
    OfferPlayerAt(s, 1'000'000, 3, kJsonA, c);
    OfferPlayerAt(s, 5'000'000, 3, kJsonB, c);

    Payload p;
    uint32_t row = 0;
    CHECK_FALSE(s.PlayerAt(3, 999'999, p, row));
    REQUIRE(s.PlayerAt(3, 1'000'000, p, row));
    CHECK(std::string(reinterpret_cast<const char*>(p.bytes), p.len) == kJsonA);
    REQUIRE(s.PlayerAt(3, 4'999'999, p, row));
    CHECK(std::string(reinterpret_cast<const char*>(p.bytes), p.len) == kJsonA);
    REQUIRE(s.PlayerAt(3, 9'000'000, p, row));
    CHECK(std::string(reinterpret_cast<const char*>(p.bytes), p.len) == kJsonB);
    // Back again: the answer is a search, so a reverse pass gets the same values with no undo.
    REQUIRE(s.PlayerAt(3, 2'000'000, p, row));
    CHECK(std::string(reinterpret_cast<const char*>(p.bytes), p.len) == kJsonA);
    // A slot the file never mentioned has no answer at any time, and does not borrow slot 3's.
    CHECK_FALSE(s.PlayerAt(4, 9'000'000, p, row));
}

TEST_CASE("a chapter's rows are spread inside it, per key, and stay strictly inside") {
    RecordedStats s;
    ScanCensus c;
    s.BeginChapter();
    OfferPlayerAt(s, 1'000'000, 0, kJsonA, c);
    OfferPlayerAt(s, 1'000'000, 0, kJsonB, c);
    OfferPlayerAt(s, 1'000'000, 1, kJsonA, c);
    s.EndChapter(1'000'000, 3'000'000, c);
    CHECK(c.spread_rows == 3);

    Payload p;
    uint32_t r0 = 0, r1 = 0, r2 = 0;
    REQUIRE(s.PlayerAt(0, 3'000'000, p, r0));
    // Slot 0's two rows land at thirds of the chapter; slot 1's single row at its half. Both
    // denominators are per key, and every stamp is strictly inside [begin, end).
    CHECK(s.RowTimeUs(0) > 1'000'000);
    CHECK(s.RowTimeUs(1) > s.RowTimeUs(0));
    CHECK(s.RowTimeUs(1) < 3'000'000);
    CHECK(s.RowTimeUs(2) == 2'000'000);
    REQUIRE(s.PlayerAt(0, s.RowTimeUs(1) - 1, p, r1));
    CHECK(std::string(reinterpret_cast<const char*>(p.bytes), p.len) == kJsonA);
    REQUIRE(s.PlayerAt(0, s.RowTimeUs(1), p, r2));
    CHECK(std::string(reinterpret_cast<const char*>(p.bytes), p.len) == kJsonB);
}

TEST_CASE("a row past the engine's own slot bound is dropped by name, never stored") {
    RecordedStats s;
    ScanCensus c;
    CHECK_FALSE(s.OfferPlayer(1, statsfeed::kMaxSlots, kJsonA, uint32_t(strlen(kJsonA)), c));
    CHECK_FALSE(s.OfferTeam(1, 2, kJsonA, 20, c));
    CHECK(c.dropped_rows == 2);
    REQUIRE(c.first_drop != nullptr);
    CHECK(strstr(c.first_drop, "slot") != nullptr);
    CHECK(s.RowCount() == 0);
}

// ---------------------------------------------------------------------------------------------
// The feed
// ---------------------------------------------------------------------------------------------

TEST_CASE_FIXTURE(FeedFixture, "the header is rewritten to the live slot and generation") {
    FakeNetGame ng;
    ng.Empty();
    ng.Put(/*slot*/ 6, /*index*/ 21, /*gen*/ 0x0003);
    uint32_t id = 0;
    REQUIRE(statsfeed::LiveNetUserId(ng.Ptr(), 21, id));
    CHECK((id & 0xFFFF) == 6);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    CHECK((id >> 16) == 3);
    // A user index the table does not hold is a REFUSAL, not slot 0: a guessed slot writes another
    // player's record.
    CHECK_FALSE(statsfeed::LiveNetUserId(ng.Ptr(), 22, id));
    // And the sentinel element is never matched, however it is asked for.
    CHECK_FALSE(statsfeed::LiveNetUserId(ng.Ptr(), 0xFFFF, id));  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
}

TEST_CASE_FIXTURE(FeedFixture, "a mapped player's newest row reaches the handler byte for byte") {
    ScanCensus c;
    RecordedStats& s = statsfeed::PlaybackRecordedStats();
    OfferPlayerAt(s, 1'000'000, 2, kJsonA, c);
    OfferPlayerAt(s, 5'000'000, 2, kJsonB, c);

    FakeNetGame ng;
    ng.Empty();
    ng.Put(9, 4, 0x0007);
    const MappedPlayer mapped[] = {{2, 4}};

    statsfeed::Sweep(ng.Ptr(), 6'000'000, mapped, 1, 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(g_calls.size() >= 1);
    CHECK_FALSE(g_calls[0].team);
    CHECK(g_calls[0].netUserId == (9u | (7u << 16)));
    CHECK(g_calls[0].blob == kJsonB);
    CHECK(g_calls[0].blobLen == uint32_t(strlen(kJsonB)));
    CHECK(statsfeed::Census().player_writes == 1);
}

TEST_CASE_FIXTURE(FeedFixture, "an unmapped recorded slot is never fed") {
    ScanCensus c;
    OfferPlayerAt(statsfeed::PlaybackRecordedStats(), 1'000'000, 2, kJsonA, c);
    FakeNetGame ng;
    ng.Empty();
    ng.Put(9, 4, 0x0007);
    // The roster mapping resolved slot 2 to user index 5, which this engine's table does not hold.
    const MappedPlayer mapped[] = {{2, 5}};
    statsfeed::Sweep(ng.Ptr(), 6'000'000, mapped, 1, 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(statsfeed::Census().player_writes == 0);
    CHECK(statsfeed::Census().no_live_slot == 1);
    for (const Call& call : g_calls) CHECK(call.team);   // the teams still run
}

TEST_CASE_FIXTURE(FeedFixture,
                  "before a player's first row the engine is given the fresh-entrant baseline") {
    ScanCensus c;
    OfferPlayerAt(statsfeed::PlaybackRecordedStats(), 5'000'000, 2, kJsonA, c);
    FakeNetGame ng;
    ng.Empty();
    ng.Put(1, 1, 0x0002);
    const MappedPlayer mapped[] = {{2, 1}};

    statsfeed::Sweep(ng.Ptr(), 1'000'000, mapped, 1, 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(g_calls.size() >= 1);
    CHECK(g_calls[0].blob == "{}");
    CHECK(statsfeed::Census().zero_writes >= 1);
    CHECK(statsfeed::Census().player_writes == 0);
}

TEST_CASE_FIXTURE(FeedFixture, "an unchanged row is not re-fed until the cadence comes round") {
    ScanCensus c;
    OfferPlayerAt(statsfeed::PlaybackRecordedStats(), 1'000'000, 2, kJsonA, c);
    FakeNetGame ng;
    ng.Empty();
    ng.Put(0, 0, 0x0001);
    const MappedPlayer mapped[] = {{2, 0}};

    statsfeed::Sweep(ng.Ptr(), 2'000'000, mapped, 1, 10'000);
    const size_t first = g_calls.size();
    REQUIRE(first >= 1);
    statsfeed::Sweep(ng.Ptr(), 2'100'000, mapped, 1, 10'100);
    CHECK(g_calls.size() == first);   // same row, inside the second: nothing written

    // A second later the feed re-applies whatever the host has been accruing on top.
    statsfeed::Sweep(ng.Ptr(), 2'200'000, mapped, 1, 11'100);
    CHECK(g_calls.size() > first);
    CHECK(statsfeed::Census().reapplies == 1);

    // And a discontinuity re-applies immediately, whatever the clock says.
    const size_t before = g_calls.size();
    statsfeed::Invalidate();
    statsfeed::Sweep(ng.Ptr(), 2'300'000, mapped, 1, 11'150);
    CHECK(g_calls.size() > before);
}

// The entrant a recorded slot maps to is not stable across a load: LEAVE tears the old one down
// and the next load admits a fresh one, in a different engine slot and behind a bumped generation,
// with a ZEROED stat record. Keyed on the row alone the feed reads "same row, already applied" and
// leaves that record blank until the re-apply second comes round -- a full second of a HUD that
// says the player has done nothing.
TEST_CASE_FIXTURE(FeedFixture, "a re-admitted entrant is written again, inside the cadence second") {
    ScanCensus c;
    OfferPlayerAt(statsfeed::PlaybackRecordedStats(), 1'000'000, 2, kJsonA, c);
    FakeNetGame ng;
    ng.Empty();
    ng.Put(0, 0, 0);                       // engine slot 0 holds live index 0, generation 0
    const MappedPlayer mapped[] = {{2, 0}};

    statsfeed::Sweep(ng.Ptr(), 2'000'000, mapped, 1, 10'000);
    const size_t first = g_calls.size();
    REQUIRE(first >= 1);
    CHECK(g_calls[0].netUserId == 0u);     // slot 0 | gen 0

    // Same row, same recorded slot, same millisecond -- but the live entrant behind it is new.
    ng.Empty();
    ng.Put(5, 0, 1);                       // re-admitted: engine slot 5, generation 1
    statsfeed::Sweep(ng.Ptr(), 2'050'000, mapped, 1, 10'050);
    REQUIRE(g_calls.size() == first + 1);
    CHECK(g_calls.back().netUserId == (5u | (1u << 16)));
    CHECK(std::string(g_calls.back().blob) == kJsonA);

    // ...and once written, that entrant is not written again until the cadence says so.
    statsfeed::Sweep(ng.Ptr(), 2'060'000, mapped, 1, 10'060);
    CHECK(g_calls.size() == first + 1);
}

TEST_CASE_FIXTURE(FeedFixture, "a backward seek across a row feeds the older row again") {
    ScanCensus c;
    RecordedStats& s = statsfeed::PlaybackRecordedStats();
    OfferPlayerAt(s, 1'000'000, 2, kJsonA, c);
    OfferPlayerAt(s, 5'000'000, 2, kJsonB, c);
    FakeNetGame ng;
    ng.Empty();
    ng.Put(0, 0, 0x0001);
    const MappedPlayer mapped[] = {{2, 0}};

    statsfeed::Sweep(ng.Ptr(), 6'000'000, mapped, 1, 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(g_calls.size() >= 1);
    CHECK(std::string(g_calls[0].blob) == kJsonB);
    g_calls.clear();
    statsfeed::Invalidate();
    statsfeed::Sweep(ng.Ptr(), 2'000'000, mapped, 1, 1050);
    REQUIRE(g_calls.size() >= 1);
    CHECK(g_calls[0].blob == kJsonA);
}

TEST_CASE_FIXTURE(FeedFixture, "nothing runs when the feed is not armed, and nothing when uncontained") {
    ScanCensus c;
    OfferPlayerAt(statsfeed::PlaybackRecordedStats(), 1'000'000, 2, kJsonA, c);
    FakeNetGame ng;
    ng.Empty();
    ng.Put(0, 0, 0x0001);
    const MappedPlayer mapped[] = {{2, 0}};

    statsfeed::Arm(false);
    statsfeed::Sweep(ng.Ptr(), 2'000'000, mapped, 1, 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(g_calls.empty());
    REQUIRE(statsfeed::Census().refusal != nullptr);
    CHECK(strcmp(statsfeed::Census().refusal, "not armed") == 0);

    statsfeed::Arm(true);
    livesafety::SetForTest(true, true);   // a live online session
    statsfeed::Sweep(ng.Ptr(), 2'000'000, mapped, 1, 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(g_calls.empty());
    REQUIRE(statsfeed::Census().refusal != nullptr);
    CHECK(strcmp(statsfeed::Census().refusal, "containment") == 0);
    livesafety::SetForTest(false, true);
}

TEST_CASE_FIXTURE(FeedFixture, "the release zeroes every mapped player and both teams") {
    ScanCensus c;
    OfferPlayerAt(statsfeed::PlaybackRecordedStats(), 1'000'000, 2, kJsonA, c);
    FakeNetGame ng;
    ng.Empty();
    ng.Put(3, 8, 0x0004);
    const MappedPlayer mapped[] = {{2, 8}};

    statsfeed::RequestRelease(mapped, 1);
    CHECK(statsfeed::ReleasePending());
    statsfeed::ServiceRelease(ng.Ptr());
    CHECK_FALSE(statsfeed::ReleasePending());

    REQUIRE(g_calls.size() == 3);
    CHECK_FALSE(g_calls[0].team);
    CHECK(g_calls[0].netUserId == (3u | (4u << 16)));
    CHECK(g_calls[0].blob == "{}");
    CHECK(g_calls[1].team);
    CHECK(g_calls[1].teamId == 0);
    // Below one 20-byte row: the applier's zeroing loop runs and its apply loop does not.
    CHECK(g_calls[1].blobLen < 20);
    CHECK(g_calls[2].team);
    CHECK(g_calls[2].teamId == 1);

    // A serviced request must not survive itself -- it would be re-run against the NEXT file.
    g_calls.clear();
    statsfeed::ServiceRelease(ng.Ptr());
    CHECK(g_calls.empty());
}

TEST_CASE_FIXTURE(FeedFixture, "a raising handler turns the feed off instead of raising again") {
    ScanCensus c;
    OfferPlayerAt(statsfeed::PlaybackRecordedStats(), 1'000'000, 2, kJsonA, c);
    FakeNetGame ng;
    ng.Empty();
    ng.Put(0, 0, 0x0001);
    const MappedPlayer mapped[] = {{2, 0}};

    g_raise = true;
    statsfeed::Sweep(ng.Ptr(), 2'000'000, mapped, 1, 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(statsfeed::Census().faulted == 1);
    CHECK_FALSE(statsfeed::Armed());
    g_raise = false;
    statsfeed::Sweep(ng.Ptr(), 2'000'000, mapped, 1, 2000);
    CHECK(g_calls.empty());
}

// ---------------------------------------------------------------------------------------------
// The real capture, through the real container reader
// ---------------------------------------------------------------------------------------------
namespace {

bool RealCapturePath(wchar_t* out, size_t cap) {
    const DWORD n = ExpandEnvironmentStringsW(
        L"%USERPROFILE%\\Documents\\EchoReplay\\"
        L"2026-09-18_03-33-45_576ED3F8428EBC4B_00000000FFFFFFFF.echodemo",
        out, DWORD(cap));
    return n != 0 && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

TEST_CASE("the owner's real capture decodes its stat rows through the shipped reader") {
    wchar_t path[MAX_PATH];
    if (!RealCapturePath(path, MAX_PATH)) {
        MESSAGE("SKIPPED BY NAME: the real capture is not on this machine");
        return;
    }
    DemoReader reader;
    REQUIRE(reader.Open(path));
    RecordedStats store;
    ScanCensus c;
    const uint32_t stored = ScanRecordedStats(reader, store, c);

    // THE CONTROL BEFORE THE NUMBER. A zero row count has three causes and they are three
    // different repairs: the file carries no stat row, its tap table never named the two rows, or
    // its event stream did not decode. The first two are asserted here, so the third cannot read
    // as the first.
    CHECK(c.tap_chapters > 0);
    CHECK(c.player_tap != 0xFFFF);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(c.team_tap != 0xFFFF);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(c.event_rows > 0);

    printf("[stats] real capture: %u event row(s), %u tap chapter(s), taps %u/%u, "
           "%u player row(s), %u team row(s), %u refused, %u dropped, %u spread\n",
           c.event_rows, c.tap_chapters, unsigned(c.player_tap), unsigned(c.team_tap),
           c.player_rows, c.team_rows, c.refused_rows, c.dropped_rows, c.spread_rows);
    CHECK(stored == c.player_rows + c.team_rows);
    CHECK(c.player_rows == 50);
    CHECK(c.team_rows == 20);
    CHECK(c.refused_rows == 0);
    CHECK(c.dropped_rows == 0);

    // Every stored payload is the file's own bytes: the player rows are JSON objects and the team
    // rows are a whole number of 20-byte rows.
    for (uint16_t i = 0; i < store.PlayerSlotCount(); ++i) {
        const uint16_t slot = store.PlayerSlotAt(i);
        Payload p;
        uint32_t row = 0;
        REQUIRE(store.PlayerAt(slot, INT64_MAX, p, row));
        REQUIRE(p.len >= 2);
        CHECK(p.bytes[0] == '{');
        CHECK(p.bytes[p.len - 1] == '}');
    }
    for (uint16_t t = 0; t < statsfeed::kMaxTeams; ++t) {
        if (store.TeamRowCount(t) == 0) continue;
        Payload p;
        uint32_t row = 0;
        REQUIRE(store.TeamAt(t, INT64_MAX, p, row));
        CHECK(p.len % 20 == 0);
    }
}

// THE TABLE A LAUNCH COMPARES THE HUD AGAINST. Content-relative, so it is the same three instants
// however the file's origin moves. Printed rather than asserted: the values are the file's, and an
// assertion on them would pin this test to one capture.
TEST_CASE("expected stat values at +10 s, +30 s and +45 s of the owner's real capture") {
    wchar_t path[MAX_PATH];
    if (!RealCapturePath(path, MAX_PATH)) {
        MESSAGE("SKIPPED BY NAME: the real capture is not on this machine");
        return;
    }
    DemoReader reader;
    REQUIRE(reader.Open(path));
    RecordedStats store;
    ScanCensus c;
    REQUIRE(ScanRecordedStats(reader, store, c) > 0);

    // ★ THE ORIGIN IS THE ONE `seek @N` COUNTS FROM -- the first recorded TICK -- and not the
    // first stat row. Indexed from the first row this table sat 20.9 s ahead of every reading a
    // launch can take with `seek`, which LAUNCH-STATS2 read as the feed running 21 s late.
    TrackScan scan;
    const char* why = nullptr;
    REQUIRE(ScanTrackSources(reader, scan, &why));
    REQUIRE(scan.have_tick_span);
    const int64_t origin = scan.first_tick_us;

    uint64_t firstRow = UINT64_MAX;
    for (uint32_t r = 0; r < store.RowCount(); ++r) {
        const uint64_t t = store.RowTimeUs(r);
        if (t < firstRow) firstRow = t;
    }
    REQUIRE(firstRow != UINT64_MAX);
    // Both on the playhead axis, which is the file axis minus the manifest origin.
    const int64_t t0 = (int64_t)reader.GetManifest().t0Us;
    printf("[stats] content origin (first tick) playhead %lld us; first stat row at playhead "
           "%lld us, i.e. +%.3f s of content\n",
           (long long)(origin - t0), (long long)((int64_t)firstRow - t0),
           double((int64_t)firstRow - origin) / 1e6);

    const int64_t offsets[] = {10, 30, 45};
    for (int64_t secs : offsets) {
        const int64_t at = origin + secs * 1'000'000;
        for (uint16_t i = 0; i < store.PlayerSlotCount(); ++i) {
            const uint16_t slot = store.PlayerSlotAt(i);
            Payload p;
            uint32_t row = 0;
            if (!store.PlayerAt(slot, at, p, row)) {
                printf("[stats] +%llds slot %u: (no row yet -- the HUD must read all zeroes)\n",
                       (long long)secs, unsigned(slot));
                continue;
            }
            printf("[stats] +%llds slot %u: %.*s\n", (long long)secs, unsigned(slot), int(p.len),
                   reinterpret_cast<const char*>(p.bytes));
        }
        for (uint16_t t = 0; t < statsfeed::kMaxTeams; ++t) {
            Payload p;
            uint32_t row = 0;
            if (!store.TeamAt(t, at, p, row)) continue;
            printf("[stats] +%llds team %u: %u row(s) of 20 B\n", (long long)secs, unsigned(t),
                   unsigned(p.len / 20));
        }
    }
}
