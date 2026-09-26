// The goal feed: decode, the forward-crossing edge, the level restore and the
// identity rewrite.
//
// NO REAL GOAL CAPTURE EXISTS. The only file in hand carries zero SR15NetReliablePostScoreUpdate
// rows, so the decode half runs against 28-byte rows minted here and written through the REAL
// container writer, read back through the REAL reader and the REAL tap table. What is synthesized
// is the CONTENT; every byte of framing is the shipped path's.
//
// The feed half runs against fake handlers that record what they were handed and a fake netGame
// whose only real content is the engine's own slot table and a gameplay pointer -- because those
// two are what the identity rewrite and the fill call are derived from.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "bootstrap/livesafety.h"
#include "capture/chapterbuilder.h"
#include "capture/manifestbuild.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/demotypes.h"
#include "foundation/gametype.h"
#include "playback/core/goalfeed.h"
#include "playback/core/goalfeed_facts.h"
#include "playback/core/statsfeed_facts.h"

using namespace replay;
using namespace replay::capture;
using namespace replay::playback;
using replay::playback::goalfeed::Goal;
using replay::playback::goalfeed::RecordedGoals;
using replay::playback::goalfeed::ScanCensus;

namespace {

namespace gf = replay::goalfeed_facts;
namespace sf = replay::statsfeed_facts;

// ---- what a handler call was handed ------------------------------------------------------------
// The body is COPIED, so a later call cannot make an earlier assertion pass.
struct Call {
    enum Kind { kFill, kReceive, kSfx, kRaise } kind = kFill;
    uint8_t body[goalfeed::kBodyBytes] = {};
    uint32_t sfxUserId = 0;
    uint64_t symbol = 0;
    const void* target = nullptr;
};

std::vector<Call> g_calls;
bool g_raise_fill = false;

void Record(Call::Kind k, const void* msg, const void* target) {
    const uint64_t* m = static_cast<const uint64_t*>(msg);
    Call c;
    c.kind = k;
    c.target = target;
    memcpy(c.body, reinterpret_cast<const void*>(uintptr_t(m[2])), goalfeed::kBodyBytes);
    g_calls.push_back(c);
}

void FakeFill(void* gameplay, const void* msg) {
    if (g_raise_fill) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    Record(Call::kFill, msg, gameplay);
}
void FakeReceive(void* netGame, const void* msg) { Record(Call::kReceive, msg, netGame); }
void FakeSfx(void* netGame, uint32_t id) {
    Call c;
    c.kind = Call::kSfx;
    c.sfxUserId = id;
    c.target = netGame;
    g_calls.push_back(c);
}

// The raise sink also witnesses what the feed did NOT do: a raise that carried a score symbol, or
// any write at all into the rules block, would have to appear here or in the bytes below.
void FakeRaise(void* netGame, uint64_t symbol) {
    Call c;
    c.kind = Call::kRaise;
    c.symbol = symbol;
    c.target = netGame;
    g_calls.push_back(c);
}

const goalfeed::Handlers kFake{&FakeFill, &FakeReceive, &FakeSfx, &FakeRaise};

// ---- the fake netGame --------------------------------------------------------------------------
// Two real fields: the slot table the rewrite reads, and the gameplay POINTER the fill call is made
// through. The pointer is a LOAD in the engine, and a feed that added the offset instead would pass
// this test only if the test made the same mistake -- so the fake stores a pointer to a separate
// object and the fill assertions check which object it landed in.
struct FakeGameplay {
    uint8_t bytes[256] = {};
};

// Wide enough for every field the feed reads off a netGame -- the slot table, the gameplay pointer
// and the role word the P-B0 probe looks at -- because a fake that is merely wide enough for the
// slot table smashes the stack the moment the gameplay pointer is written into it.
inline constexpr size_t kFakeNetGameBytes = 16u * 1024u;
static_assert(kFakeNetGameBytes > sf::kSlotTableOff + 4u * sf::kSlotTableMax, "slot table");
static_assert(kFakeNetGameBytes > gf::kGameplayPtrOff + sizeof(void*), "gameplay pointer");
static_assert(kFakeNetGameBytes > gf::kRoleWordPtrOff + sizeof(void*), "role word pointer");

struct FakeNetGame {
    uint8_t bytes[kFakeNetGameBytes] = {};
    FakeGameplay gameplay;

    FakeNetGame() {
        Empty();
        void* p = &gameplay;
        memcpy(bytes + gf::kGameplayPtrOff, &p, sizeof p);
    }
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

static_assert(gf::kGameplayPtrOff > sf::kSlotTableOff + 4u * sf::kSlotTableMax,
              "the fake would be writing the gameplay pointer over the slot table");

// ---- the recorded -> live mapping the module supplies ------------------------------------------
struct MapTable {
    // recorded slot -> live user index; 0xFFFF means "not on the field".
    uint16_t to_index[16];
    MapTable() {
        for (uint16_t i = 0; i < 16; ++i) to_index[i] = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
};

struct MapCtx {
    MapTable table;
    FakeNetGame* ng = nullptr;
};

bool MapUser(void* ctx, uint16_t recSlot, uint32_t& out) {
    MapCtx* m = static_cast<MapCtx*>(ctx);
    if (recSlot >= 16 || m->table.to_index[recSlot] == 0xFFFF) return false;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const uint16_t want = m->table.to_index[recSlot];
    for (uint16_t slot = 0; slot < sf::kSlotTableMax; ++slot) {
        uint16_t index = 0, gen = 0;
        const uint8_t* at = m->ng->bytes + sf::kSlotTableOff + 4u * slot;
        memcpy(&index, at, sizeof index);
        memcpy(&gen, at + 2, sizeof gen);
        if (index != want || index == 0xFFFF || gen == 0xFFFF) continue;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
        out = uint32_t(slot) | (uint32_t(gen) << 16);
        return true;
    }
    return false;
}

// ---- minting a 28-byte body --------------------------------------------------------------------
std::vector<uint8_t> MakeBody(uint16_t team, uint16_t points, uint16_t goalType, uint32_t scorer,
                              uint32_t assist, float speed, float distance, float held) {
    std::vector<uint8_t> b(goalfeed::kBodyBytes, 0);
    memcpy(b.data() + gf::kOffTeam, &team, sizeof team);
    memcpy(b.data() + gf::kOffPoints, &points, sizeof points);
    memcpy(b.data() + gf::kOffGoalType, &goalType, sizeof goalType);
    memcpy(b.data() + gf::kOffScorer, &scorer, sizeof scorer);
    memcpy(b.data() + gf::kOffAssist, &assist, sizeof assist);
    memcpy(b.data() + gf::kOffSpeed, &speed, sizeof speed);
    memcpy(b.data() + gf::kOffDistance, &distance, sizeof distance);
    memcpy(b.data() + gf::kOffPossession, &held, sizeof held);
    // The pad at +14 is deliberately NOT zero: it is the cheapest witness that the feed carries the
    // recording's bytes through rather than rebuilding a body from decoded fields.
    const uint16_t pad = 0xBEEF;
    memcpy(b.data() + 14, &pad, sizeof pad);
    return b;
}

uint32_t RecId(uint16_t slot, uint16_t gen) { return uint32_t(slot) | (uint32_t(gen) << 16); }

uint32_t BodyU32(const uint8_t* b, uint32_t off) {
    uint32_t v = 0;
    memcpy(&v, b + off, sizeof v);
    return v;
}

// ---- the file ----------------------------------------------------------------------------------
struct TempDemo {
    wchar_t path[512];
    TempDemo() {
        wchar_t dir[400];  // facts-ok: AUDIO.LISTENER_COUNT_OFF a temp-path buffer length, not the listener count offset
        const DWORD n = GetTempPathW(400, dir);  // facts-ok: AUDIO.LISTENER_COUNT_OFF a temp-path buffer length, not the listener count offset
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsgoal1_%lu_%llu.echodemo", dir,
                     (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    }
    ~TempDemo() { DeleteFileW(path); }
};

template <class T>
void Feed(ChapterBuilder& b, uint16_t kind, const T& payload) {
    uint8_t buf[sizeof(RecHeader) + sizeof(T)];
    RecHeader h{};
    h.kind = kind;
    h.bytes = uint16_t(sizeof(T));
    h.producer = 0;
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &payload, sizeof payload);
    b.Consume(buf, uint32_t(sizeof buf));
}

void FeedEventBody(ChapterBuilder& b, uint64_t stampUs, uint16_t typeIndex,
                   const std::vector<uint8_t>& body) {
    std::vector<uint8_t> buf(sizeof(RecHeader) + sizeof(EventRec) + body.size());
    RecHeader h{};
    h.kind = kRecEvent;
    h.bytes = uint16_t(sizeof(EventRec) + body.size());
    h.producer = 0;
    EventRec r{};
    r.stampUs = stampUs;
    r.dst = 0;
    r.typeIndex = typeIndex;
    r.bodyBytes = uint16_t(body.size());
    memcpy(buf.data(), &h, sizeof h);
    memcpy(buf.data() + sizeof h, &r, sizeof r);
    memcpy(buf.data() + sizeof h + sizeof r, body.data(), body.size());
    b.Consume(buf.data(), uint32_t(buf.size()));
}

constexpr uint64_t kT0 = 20'999'790ull;
constexpr uint64_t kChapterUs = 2'000'000ull;

// Two goals, in the second and the fourth chapter, each surrounded by ordinary event rows so the
// arrival window the placement interpolates against is a real one.
struct SynthTimes {
    uint64_t t0, end;
    uint64_t goalArrivalA, goalArrivalB;   // the wall stamp the row was tapped at
    uint64_t goalSourceA, goalSourceB;     // the capture-axis instant it really happened at
};

SynthTimes WriteGoalDemo(const wchar_t* path, uint16_t badTypeRows) {
    SynthTimes t{};
    t.t0 = kT0;
    t.end = t.t0 + 8 * kChapterUs;

    ManifestBuilder mb;
    mb.Begin(t.t0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x676F6C646D617374ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(kChapterUs);
    mb.SetUuid(7, 7);
    PlayerRecord p{};
    p.xplatformId = 0x1000;
    p.userSlot = 3;
    p.userIndex = 3;
    mb.AddPlayer(p);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = kChapterUs;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    const uint16_t goalTap = TapTypeIndex("SR15NetReliablePostScoreUpdate");
    REQUIRE(goalTap != kTapTypeIndexNone);
    const uint16_t otherTap = TapTypeIndex("SR15NetComponentEvent");
    REQUIRE(otherTap != kTapTypeIndexNone);

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(t.t0);

    // The arrival axis is the recorder's wall clock, deliberately offset from the capture axis so a
    // feed that mistook one for the other could not pass.
    const uint64_t arrivalBase = 900'000'000ull;
    const uint64_t dt = 20000;
    const std::vector<uint8_t> filler(24, 0x11);

    t.goalArrivalA = 0;
    t.goalArrivalB = 0;
    for (uint32_t f = 0; f * dt <= t.end - t.t0; ++f) {
        const uint64_t st = t.t0 + f * dt;
        const uint64_t arrival = arrivalBase + f * dt;
        TickRec tr{};
        tr.sourceTimeUs = st;
        tr.simTimeUs = 1'000'000 + f * dt;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = uint32_t(dt);
        tr.scaledDeltaUs = uint32_t(dt);
        Feed(b, kRecTick, tr);

        // An ordinary event every few ticks: this is what widens each chapter's arrival window.
        if (f % 4 == 0) FeedEventBody(b, arrival, otherTap, filler);

        const uint64_t intoChapter = (st - t.t0) % kChapterUs;
        const uint64_t chapter = (st - t.t0) / kChapterUs;
        if (chapter == 1 && intoChapter >= kChapterUs / 2 && intoChapter < kChapterUs / 2 + dt) {
            t.goalArrivalA = arrival;
            t.goalSourceA = st;
            FeedEventBody(b, arrival, goalTap,
                          MakeBody(0, 2, 3, RecId(3, 1), RecId(5, 1), 12.5f, 30.25f, 4.5f));
        }
        if (chapter == 3 && intoChapter >= kChapterUs / 4 && intoChapter < kChapterUs / 4 + dt) {
            t.goalArrivalB = arrival;
            t.goalSourceB = st;
            FeedEventBody(b, arrival, goalTap,
                          MakeBody(1, 3, 6, RecId(5, 1), 0xFFFFFFFFu, 21.0f, 9.5f, 1.25f));  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
            for (uint16_t i = 0; i < badTypeRows; ++i) {
                FeedEventBody(b, arrival, goalTap,
                              MakeBody(0, 2, 99, RecId(3, 1), RecId(5, 1), 1.0f, 1.0f, 1.0f));
            }
        }
    }
    b.EmitMatchEnd(t.end);
    b.FlushChapter();
    b.Close();
    writer.Close();
    return t;
}

// ---- the fixture -------------------------------------------------------------------------------
struct FeedFixture {
    FeedFixture() {
        g_calls.clear();
        g_raise_fill = false;
        goalfeed::InjectHandlersForTest(&kFake);
        goalfeed::PlaybackRecordedGoals().Reset();
        goalfeed::ResetCensusForTest();
        goalfeed::SetHornEnabled(false);
        // Both optional elements are off here though the shipped default is on, so every case that
        // counts handler calls counts the ones it is about.  The explosion's own cases turn it on.
        goalfeed::SetExplosionEnabled(false);
        goalfeed::SetPlayheadOriginUs(0);
        goalfeed::Arm(true);
        // The real offline lock is not held in a test process, so the verdict is OVERRIDDEN rather
        // than published, exactly as the stat feed's fixture does it.
        livesafety::SetForTest(false, true);
    }
    ~FeedFixture() {
        livesafety::ResetForTest();
        goalfeed::Arm(false);
        goalfeed::SetHornEnabled(false);
        goalfeed::SetExplosionEnabled(false);
        goalfeed::InjectHandlersForTest(nullptr);
        g_calls.clear();
    }
};

// Two goals in the process store, at known playhead times, with a mapping for both.
struct Scene {
    FakeNetGame ng;
    MapCtx ctx;
    Scene() {
        ctx.ng = &ng;
        // Recorded slot 3 is live slot 9 generation 4; recorded slot 5 is live slot 2 generation 7.
        ng.Put(9, 11, 4);
        ng.Put(2, 12, 7);
        ctx.table.to_index[3] = 11;
        ctx.table.to_index[5] = 12;
    }
    void Sweep(int64_t tUs, bool hold = false) {
        goalfeed::Sweep(ng.Ptr(), tUs, hold, &MapUser, &ctx);
    }
};

void OfferAt(RecordedGoals& s, uint64_t tUs, uint16_t team, uint16_t points, uint16_t type,
             uint32_t scorer, uint32_t assist) {
    ScanCensus c;
    const std::vector<uint8_t> body = MakeBody(team, points, type, scorer, assist, 1.0f, 2.0f, 3.0f);
    REQUIRE(s.Offer(tUs, body.data(), uint32_t(body.size()), c));
}

// The store the shipped feed reads, filled directly for the feed cases.
void TwoGoalsInStore() {
    RecordedGoals& s = goalfeed::PlaybackRecordedGoals();
    s.Reset();
    OfferAt(s, 1'000'000, 0, 2, 3, RecId(3, 1), RecId(5, 1));
    OfferAt(s, 5'000'000, 1, 3, 6, RecId(5, 1), 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    // Offer stores on the arrival stamp; with no chapter to place them in, that stamp IS the time.
}

size_t CountOf(Call::Kind k) {
    size_t n = 0;
    for (const Call& c : g_calls) {
        if (c.kind == k) ++n;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The store and the decode
// ---------------------------------------------------------------------------------------------

TEST_CASE("a written .echodemo's goal rows decode through the real container and land in order") {
    TempDemo tmp;
    const SynthTimes t = WriteGoalDemo(tmp.path, /*badTypeRows=*/2);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));

    RecordedGoals s;
    ScanCensus c;
    const uint32_t stored = goalfeed::ScanRecordedGoals(reader, s, c);

    CHECK(stored == 2);
    CHECK(c.rows == 2);
    CHECK(s.Count() == 2);
    // The two rows with a goal type the banner has no arm for are refused by name, not stored and
    // silently skipped: a count of 4 here would mean the engine gets shown something we invented.
    CHECK(c.refused_rows == 2);
    CHECK(c.dropped_rows == 0);
    CHECK(c.tap_chapters > 0);
    CHECK(c.tap != 0xFFFF);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(c.event_rows > 2);

    SUBCASE("every field comes back, and the untouched pad proves the bytes are the file's") {
        const Goal& a = s.At(0);
        CHECK(a.team == 0);
        CHECK(a.points == 2);
        CHECK(a.goal_type == 3);
        CHECK(a.scorer == RecId(3, 1));
        CHECK(a.assist == RecId(5, 1));
        CHECK(a.speed == doctest::Approx(12.5f));
        CHECK(a.distance == doctest::Approx(30.25f));
        CHECK(a.possession_s == doctest::Approx(4.5f));
        uint16_t pad = 0;
        memcpy(&pad, a.body + 14, sizeof pad);
        CHECK(pad == 0xBEEF);

        const Goal& b = s.At(1);
        CHECK(b.team == 1);
        CHECK(b.points == 3);
        CHECK(b.goal_type == 6);
        CHECK(b.scorer == RecId(5, 1));
        CHECK(b.assist == 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    }

    SUBCASE("each row lands near the instant it happened, on the playhead's axis") {
        const uint64_t a = s.At(0).t_us;
        const uint64_t b = s.At(1).t_us;
        CHECK(a < b);
        // The row carries the tap's ARRIVAL stamp, which is an epoch away from the capture axis.
        // What brings it back is where that stamp falls among the arrival stamps of the chapter's
        // other events, and the accuracy that buys is what this bound measures: a banner within a
        // tenth of a second of the shot is a banner the owner reads as on time. An even spread
        // inside the chapter -- the stat feed's placement -- would miss by up to a second.
        const int64_t errA = int64_t(a) - int64_t(t.goalSourceA);
        const int64_t errB = int64_t(b) - int64_t(t.goalSourceB);
        CHECK(errA > -100'000);
        CHECK(errA < 100'000);
        CHECK(errB > -100'000);
        CHECK(errB < 100'000);
        CHECK(a > t.t0);
        CHECK(b < t.end);
        CHECK(c.placed_rows == 2);
    }

    SUBCASE("the search answers the newest row at or before the playhead, in both directions") {
        const uint64_t a = s.At(0).t_us;
        const uint64_t b = s.At(1).t_us;
        CHECK(s.IndexAtOrBefore(int64_t(a) - 1) == -1);
        CHECK(s.IndexAtOrBefore(int64_t(a)) == 0);
        CHECK(s.IndexAtOrBefore(int64_t(b) - 1) == 0);
        CHECK(s.IndexAtOrBefore(int64_t(b)) == 1);
        CHECK(s.IndexAtOrBefore(int64_t(b) + 100'000'000) == 1);
        CHECK(s.FirstAfter(int64_t(a) - 1) == 0);
        CHECK(s.FirstAfter(int64_t(a)) == 1);
        CHECK(s.FirstAfter(int64_t(b)) == 2);
    }
}

TEST_CASE("a body shorter than the engine's own 28 is refused, never padded") {
    RecordedGoals s;
    ScanCensus c;
    const std::vector<uint8_t> body = MakeBody(0, 2, 1, RecId(3, 1), RecId(5, 1), 1, 2, 3);
    CHECK_FALSE(s.Offer(1000, body.data(), goalfeed::kBodyBytes - 1, c));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK_FALSE(s.Offer(1000, nullptr, goalfeed::kBodyBytes, c));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(c.refused_rows == 2);
    CHECK(s.Count() == 0);
}

// ---------------------------------------------------------------------------------------------
// The edge
// ---------------------------------------------------------------------------------------------

TEST_CASE("a forward crossing fires both handlers once, in the client's order, with live ids") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();

    sc.Sweep(900'000);          // the first sweep re-bases; nothing has been crossed yet
    CHECK(g_calls.empty());
    CHECK(goalfeed::Census().fires == 0);

    sc.Sweep(1'100'000);        // crosses goal 0
    REQUIRE(g_calls.size() == 2);
    CHECK(goalfeed::Census().fires == 1);

    SUBCASE("the fill comes first and lands in the gameplay object, the raise second in netGame") {
        CHECK(g_calls[0].kind == Call::kFill);
        CHECK(g_calls[0].target == static_cast<const void*>(&sc.ng.gameplay));
        CHECK(g_calls[1].kind == Call::kReceive);
        CHECK(g_calls[1].target == sc.ng.Ptr());
    }

    SUBCASE("both handlers see the SAME bytes, with both ids rewritten to this session's lobby") {
        CHECK(memcmp(g_calls[0].body, g_calls[1].body, goalfeed::kBodyBytes) == 0);
        // recorded slot 3 -> live slot 9 generation 4; recorded slot 5 -> live slot 2 generation 7.
        CHECK(BodyU32(g_calls[0].body, gf::kOffScorer) == (9u | (4u << 16)));
        CHECK(BodyU32(g_calls[0].body, gf::kOffAssist) == (2u | (7u << 16)));
    }

    SUBCASE("the other twenty bytes are the recording's, pad included") {
        uint16_t pad = 0;
        memcpy(&pad, g_calls[0].body + 14, sizeof pad);
        CHECK(pad == 0xBEEF);
        uint16_t team = 0xFF, points = 0, type = 0;
        memcpy(&team, g_calls[0].body + gf::kOffTeam, sizeof team);
        memcpy(&points, g_calls[0].body + gf::kOffPoints, sizeof points);
        memcpy(&type, g_calls[0].body + gf::kOffGoalType, sizeof type);
        CHECK(team == 0);
        CHECK(points == 2);
        CHECK(type == 3);
    }

    SUBCASE("sitting on the row, pausing and resuming on top of it does not fire it again") {
        const size_t after = g_calls.size();
        sc.Sweep(1'100'000);
        sc.Sweep(1'100'000);
        sc.Sweep(1'100'001);
        CHECK(goalfeed::Census().fires == 1);
        CHECK(g_calls.size() == after);
    }

    SUBCASE("the second goal fires on its own crossing and not before") {
        sc.Sweep(4'999'999);
        CHECK(goalfeed::Census().fires == 1);
        sc.Sweep(5'000'000);
        CHECK(goalfeed::Census().fires == 2);
        CHECK(CountOf(Call::kReceive) == 2);
    }
}

TEST_CASE("one tick that crosses two rows fires both, in time order") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(0);
    sc.Sweep(6'000'000);
    CHECK(goalfeed::Census().fires == 2);
    REQUIRE(g_calls.size() == 4);
    uint16_t firstTeam = 0xFF, secondTeam = 0xFF;
    memcpy(&firstTeam, g_calls[0].body + gf::kOffTeam, sizeof firstTeam);
    memcpy(&secondTeam, g_calls[2].body + gf::kOffTeam, sizeof secondTeam);
    CHECK(firstTeam == 0);
    CHECK(secondTeam == 1);
}

TEST_CASE("reverse never fires, and never un-fires") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(6'000'000);   // re-base above both rows
    goalfeed::ResetCensusForTest();
    g_calls.clear();
    sc.Sweep(4'000'000);
    sc.Sweep(2'000'000);
    sc.Sweep(500'000);
    CHECK(goalfeed::Census().fires == 0);
    CHECK(CountOf(Call::kReceive) == 0);
}

TEST_CASE("a forward SEEK across a goal restores the banner data and raises nothing") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(500'000);
    g_calls.clear();
    goalfeed::ResetCensusForTest();

    goalfeed::NoteSeek();
    sc.Sweep(3'000'000);   // the seek destination is past goal 0

    CHECK(goalfeed::Census().fires == 0);
    CHECK(goalfeed::Census().seeks_suppressed == 1);
    CHECK(CountOf(Call::kReceive) == 0);
    // The LEVEL is still owed: the banner's expression nodes read the gameplay fields, so after a
    // seek past a goal those fields must hold that goal.
    REQUIRE(CountOf(Call::kFill) == 1);
    CHECK(goalfeed::Census().level_writes == 1);
    CHECK(g_calls[0].target == static_cast<const void*>(&sc.ng.gameplay));
    uint16_t type = 0;
    memcpy(&type, g_calls[0].body + gf::kOffGoalType, sizeof type);
    CHECK(type == 3);

    SUBCASE("and the level is not re-written every tick once it is right") {
        const size_t after = g_calls.size();
        sc.Sweep(3'100'000);
        sc.Sweep(3'200'000);
        CHECK(g_calls.size() == after);
    }
}

TEST_CASE("a backward seek then forward play fires the same goal again") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(500'000);
    sc.Sweep(1'500'000);
    REQUIRE(goalfeed::Census().fires == 1);

    goalfeed::NoteSeek();
    sc.Sweep(500'000);            // back before the goal: no fire, and no level to restore
    CHECK(goalfeed::Census().fires == 1);

    sc.Sweep(1'500'000);          // and forward over it again
    CHECK(goalfeed::Census().fires == 2);
    CHECK(CountOf(Call::kReceive) == 2);
}

TEST_CASE("with no row at or before the playhead the engine's own fields are left alone") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    goalfeed::NoteSeek();
    sc.Sweep(100'000);
    CHECK(g_calls.empty());
    CHECK(goalfeed::Census().level_writes == 0);
}

TEST_CASE("the pre-roll hold crosses rows without firing, and play afterwards is not retroactive") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(500'000, /*hold=*/true);
    sc.Sweep(1'500'000, /*hold=*/true);
    sc.Sweep(2'500'000, /*hold=*/true);
    CHECK(goalfeed::Census().fires == 0);
    CHECK(goalfeed::Census().holds == 3);
    // The level is still restored while held: the banner data is a function of the playhead.
    CHECK(goalfeed::Census().level_writes == 1);
    sc.Sweep(2'600'000);
    CHECK(goalfeed::Census().fires == 0);
}

// ---------------------------------------------------------------------------------------------
// Identity, the horn and the refusals
// ---------------------------------------------------------------------------------------------

TEST_CASE("an unmapped scorer is written as the engine's own nobody, not as a guessed slot") {
    FeedFixture fx;
    Scene sc;
    sc.ctx.table.to_index[3] = 0xFFFF;   // the scorer left, or never mapped  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    TwoGoalsInStore();
    sc.Sweep(500'000);
    sc.Sweep(1'500'000);
    REQUIRE(goalfeed::Census().fires == 1);
    CHECK(BodyU32(g_calls[0].body, gf::kOffScorer) == gf::kNoUserId);
    CHECK(BodyU32(g_calls[0].body, gf::kOffAssist) == (2u | (7u << 16)));
    CHECK(goalfeed::Census().unmapped_scorer == 1);
}

TEST_CASE("a goal recorded with no assist keeps the engine's own unset id, not our nobody") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(4'000'000);        // re-base, and the level restore for goal 0
    g_calls.clear();
    sc.Sweep(5'500'000);        // goal 1, whose recorded assist is the engine's unset value
    REQUIRE(goalfeed::Census().fires == 1);
    // Rewriting it would be us answering a question the recording already answered, and the answer
    // the engine gives to its own unset id is not the one it gives to an unresolvable user.
    CHECK(BodyU32(g_calls[0].body, gf::kOffAssist) == gf::kUnsetUserId);
    CHECK(goalfeed::Census().unmapped_assist == 0);
}

TEST_CASE("a mapping whose live index is not in the slot table refuses rather than guessing") {
    FeedFixture fx;
    Scene sc;
    sc.ng.Empty();                      // mapped, but nobody is on the field
    TwoGoalsInStore();
    sc.Sweep(500'000);
    sc.Sweep(1'500'000);
    REQUIRE(goalfeed::Census().fires == 1);
    CHECK(BodyU32(g_calls[0].body, gf::kOffScorer) == gf::kNoUserId);
    CHECK(BodyU32(g_calls[0].body, gf::kOffAssist) == gf::kNoUserId);
}

TEST_CASE("the horn is off by default, on when asked, and never for a scorer with no live id") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(500'000);
    sc.Sweep(1'500'000);
    CHECK(CountOf(Call::kSfx) == 0);

    goalfeed::SetHornEnabled(true);
    sc.Sweep(5'500'000);                // goal 1: recorded slot 5, which IS mapped
    CHECK(CountOf(Call::kSfx) == 1);
    CHECK(goalfeed::Census().horn_calls == 1);

    goalfeed::Forget();
    sc.ng.Empty();
    g_calls.clear();
    sc.Sweep(500'000);
    sc.Sweep(1'500'000);
    CHECK(CountOf(Call::kSfx) == 0);
}

TEST_CASE("a disarmed feed and an uncontained process both write nothing") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();

    SUBCASE("playback.goal_events off") {
        goalfeed::Arm(false);
        sc.Sweep(500'000);
        sc.Sweep(1'500'000);
        CHECK(g_calls.empty());
        CHECK(std::string(goalfeed::Census().refusal ? goalfeed::Census().refusal : "") ==
              "not armed");
    }

    SUBCASE("a live online session") {
        livesafety::SetForTest(true, true);
        sc.Sweep(500'000);
        sc.Sweep(1'500'000);
        CHECK(g_calls.empty());
        CHECK(std::string(goalfeed::Census().refusal ? goalfeed::Census().refusal : "") ==
              "containment");
    }

    SUBCASE("a netGame with no gameplay object yet") {
        memset(sc.ng.bytes + gf::kGameplayPtrOff, 0, sizeof(void*));
        sc.Sweep(500'000);
        sc.Sweep(1'500'000);
        CHECK(g_calls.empty());
        CHECK(std::string(goalfeed::Census().refusal ? goalfeed::Census().refusal : "") ==
              "no gameplay object");
    }
}

TEST_CASE("a handler that raises turns the feed off for the session rather than trying again") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(500'000);
    g_raise_fill = true;
    sc.Sweep(1'500'000);
    CHECK(goalfeed::Census().faulted == 1);
    CHECK_FALSE(goalfeed::Armed());
    g_raise_fill = false;
    sc.Sweep(5'500'000);
    CHECK(goalfeed::Census().fires == 0);
}

// GOAL-W1 PART 1. A host score that reached the engine has overwritten the gameplay block's
// scorer, team and point fields with the HOST'S -- its own detector credits 3 points where the
// recording says 2. The next level restore puts the recording back, and it must do that with the
// FILL alone: a second raise here would be a banner at a moment nothing happened.
TEST_CASE("a host score that passed the gate forces one recorded re-fill and no raise") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(500'000);
    sc.Sweep(1'500'000);          // crosses goal 0: fill + receive
    REQUIRE(goalfeed::Census().fires == 1);
    g_calls.clear();

    sc.Sweep(1'600'000);          // steady state: the answer has not changed, so nothing is written
    REQUIRE(g_calls.empty());

    goalfeed::NotePassSinceLastSweep();
    sc.Sweep(1'700'000);
    REQUIRE(CountOf(Call::kFill) == 1);
    CHECK(CountOf(Call::kReceive) == 0);
    CHECK(CountOf(Call::kSfx) == 0);
    CHECK(goalfeed::Census().fires == 1);
    CHECK(goalfeed::Census().pass_reapplies == 1);
    CHECK(g_calls[0].target == static_cast<const void*>(&sc.ng.gameplay));

    SUBCASE("and the flag is consumed, not latched") {
        g_calls.clear();
        sc.Sweep(1'800'000);
        CHECK(g_calls.empty());
        CHECK(goalfeed::Census().pass_reapplies == 1);
    }
}

TEST_CASE("a pass noted before any row is reached writes nothing there is no recording for") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    sc.Sweep(100'000);
    g_calls.clear();
    goalfeed::ResetCensusForTest();

    goalfeed::NotePassSinceLastSweep();
    sc.Sweep(200'000);            // still before goal 0: there is no recorded level to restore
    CHECK(g_calls.empty());
    CHECK(goalfeed::Census().pass_reapplies == 0);
}

// ---------------------------------------------------------------------------------------------
// The goal explosion
// ---------------------------------------------------------------------------------------------

namespace {

// Everything the sink must never see: the two score raises whose only listener is the match state
// machine, and the relay it emits.  A raise carrying one of these would be the host pass the design
// refused, arriving through this feed.
constexpr uint64_t kScoreBlue = gametype::SymbolHash("nevt_increment_score_blue");
constexpr uint64_t kScoreRed = gametype::SymbolHash("nevt_increment_score_red");
constexpr uint64_t kGameScore = gametype::SymbolHash("nevt_game_score");

// One goal raises THREE events: the rig's material flash, the side-agnostic FX send, and the FX
// event for the scoring side.  This counts goals, not raises, so the callers that assert "one
// explosion per goal" still read as they did.
constexpr size_t kRaisesPerGoal = 3;

size_t ExplosionRaises() {
    size_t n = 0;
    for (const Call& c : g_calls) {
        if (c.kind != Call::kRaise) continue;
        CHECK(c.symbol != kScoreBlue);
        CHECK(c.symbol != kScoreRed);
        CHECK(c.symbol != kGameScore);
        CHECK((c.symbol == gf::kExplosionSymbol || c.symbol == gf::kScoreFxSendSymbol ||
               c.symbol == gf::kScoreFxBlueSymbol || c.symbol == gf::kScoreFxRedSymbol));
        ++n;
    }
    CHECK(n % kRaisesPerGoal == 0);
    return n / kRaisesPerGoal;
}

}  // namespace

TEST_CASE("the explosion symbol is the one the rig listens for, derived and not transcribed") {
    CHECK(gf::kExplosionSymbol == gametype::SymbolHash("evt_glb_scored_matanimation"));
    // The three symbols the feed must never raise are different values, so the assertions in
    // ExplosionRaises() are testing something.
    CHECK(gf::kExplosionSymbol != kScoreBlue);
    CHECK(gf::kExplosionSymbol != kScoreRed);
    CHECK(gf::kExplosionSymbol != kGameScore);
}

TEST_CASE("a forward crossing raises the rig's event once, after the banner, on the netGame") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    goalfeed::SetExplosionEnabled(true);

    sc.Sweep(900'000);
    CHECK(g_calls.empty());

    sc.Sweep(1'100'000);
    REQUIRE(g_calls.size() == 2 + kRaisesPerGoal);
    CHECK(g_calls[0].kind == Call::kFill);
    CHECK(g_calls[1].kind == Call::kReceive);
    for (size_t i = 2; i < g_calls.size(); ++i) {
        CHECK(g_calls[i].kind == Call::kRaise);
        CHECK(g_calls[i].target == sc.ng.Ptr());
    }
    CHECK(ExplosionRaises() == 1);
    CHECK(goalfeed::Census().explosion_raises == 1);

    SUBCASE("and sitting on the row afterwards does not raise it again") {
        sc.Sweep(1'200'000);
        sc.Sweep(2'000'000);
        CHECK(ExplosionRaises() == 1);
        CHECK(goalfeed::Census().explosion_raises == 1);
    }

    SUBCASE("the second goal is its own crossing") {
        sc.Sweep(5'100'000);
        CHECK(ExplosionRaises() == 2);
        CHECK(goalfeed::Census().explosion_raises == 2);
    }

    SUBCASE("one tick over both rows raises once per row") {
        goalfeed::Forget();
        g_calls.clear();
        goalfeed::ResetCensusForTest();
        sc.Sweep(500'000);
        sc.Sweep(6'000'000);
        CHECK(ExplosionRaises() == 2);
    }
}

namespace {

// The fx rig's colour flag as its graph keeps it: fx_blue/red_score_event latch it, the send's burst
// reads it, and the deferred entry runs them in raise order. allocate_graph leaves it red.
std::vector<uint64_t> BurstColours() {
    std::vector<uint64_t> colours;
    uint64_t flag = gf::kScoreFxRedSymbol;
    for (const Call& c : g_calls) {
        if (c.kind != Call::kRaise) continue;
        if (c.symbol == gf::kScoreFxBlueSymbol || c.symbol == gf::kScoreFxRedSymbol) flag = c.symbol;
        if (c.symbol == gf::kScoreFxSendSymbol) colours.push_back(flag);
    }
    return colours;
}

}  // namespace

TEST_CASE("each explosion wears the colour of the team the point went to, own goals included") {
    FeedFixture fx;
    Scene sc;
    RecordedGoals& s = goalfeed::PlaybackRecordedGoals();
    s.Reset();
    OfferAt(s, 1'000'000, 0, 2, 3, RecId(3, 1), RecId(5, 1));   // blue scores
    OfferAt(s, 3'000'000, 1, 2, 3, RecId(5, 1), 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    // An orange player into orange's own goal: the row names blue, the team that got the point.
    OfferAt(s, 5'000'000, 0, 1, 10, RecId(5, 1), 0xFFFFFFFFu);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    goalfeed::SetExplosionEnabled(true);

    sc.Sweep(500'000);
    sc.Sweep(2'000'000);
    sc.Sweep(4'000'000);
    sc.Sweep(6'000'000);
    CHECK(goalfeed::Census().explosion_raises == 3);
    const std::vector<uint64_t> expected = {gf::kScoreFxBlueSymbol, gf::kScoreFxRedSymbol,
                                            gf::kScoreFxBlueSymbol};
    CHECK(BurstColours() == expected);
}

TEST_CASE("nothing the user did not play through raises the explosion") {
    FeedFixture fx;
    Scene sc;
    TwoGoalsInStore();
    goalfeed::SetExplosionEnabled(true);

    SUBCASE("a forward seek across a goal") {
        sc.Sweep(500'000);
        g_calls.clear();
        goalfeed::NoteSeek();
        sc.Sweep(3'000'000);
        CHECK(CountOf(Call::kFill) == 1);   // the level is still restored
        CHECK(ExplosionRaises() == 0);
        CHECK(goalfeed::Census().explosion_raises == 0);
    }

    SUBCASE("playing backwards over both rows") {
        sc.Sweep(6'000'000);
        g_calls.clear();
        sc.Sweep(4'000'000);
        sc.Sweep(500'000);
        CHECK(ExplosionRaises() == 0);
    }

    SUBCASE("the pre-roll's fast wind, which owns the clock") {
        sc.Sweep(500'000, /*hold=*/true);
        sc.Sweep(6'000'000, /*hold=*/true);
        CHECK(ExplosionRaises() == 0);
        CHECK(goalfeed::Census().holds == 2);
    }

    SUBCASE("playback.goal_explosion off") {
        goalfeed::SetExplosionEnabled(false);
        sc.Sweep(500'000);
        sc.Sweep(1'500'000);
        CHECK(CountOf(Call::kReceive) == 1);   // the rest of the goal still happened
        CHECK(ExplosionRaises() == 0);
    }

    SUBCASE("a session that is not contained") {
        livesafety::SetForTest(true, true);
        sc.Sweep(500'000);
        sc.Sweep(1'500'000);
        CHECK(g_calls.empty());
    }

    SUBCASE("a host score that forces a level re-fill") {
        sc.Sweep(1'500'000);       // re-base past goal 0 without crossing it
        g_calls.clear();
        goalfeed::ResetCensusForTest();
        goalfeed::NotePassSinceLastSweep();
        sc.Sweep(1'600'000);
        CHECK(CountOf(Call::kFill) == 1);
        CHECK(ExplosionRaises() == 0);
    }
}

TEST_CASE("rows written by the real writer and read back raise one explosion each, in time order") {
    FeedFixture fx;
    Scene sc;
    TempDemo tmp;
    const SynthTimes t = WriteGoalDemo(tmp.path, /*badTypeRows=*/0);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));
    RecordedGoals& s = goalfeed::PlaybackRecordedGoals();
    s.Reset();
    ScanCensus c;
    REQUIRE(goalfeed::ScanRecordedGoals(reader, s, c) == 2);
    const int64_t a = int64_t(s.At(0).t_us);
    const int64_t b = int64_t(s.At(1).t_us);

    goalfeed::SetExplosionEnabled(true);
    sc.Sweep(int64_t(t.t0));
    CHECK(ExplosionRaises() == 0);
    sc.Sweep(a + 1);
    CHECK(ExplosionRaises() == 1);
    sc.Sweep(b - 1);
    CHECK(ExplosionRaises() == 1);
    sc.Sweep(b + 1);
    CHECK(ExplosionRaises() == 2);
    CHECK(goalfeed::Census().explosion_raises == 2);
}
