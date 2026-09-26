// The stun feed: decode, the forward-crossing edge, the level restore, the double
// stun and the identity rewrite.
//
// A REAL STUN CAPTURE EXISTS, and it is the owner's own: eight SR15StunPlayerMsg rows with victim,
// stunner, duration and a stamp. The decode half is driven against that file when it is on the
// machine, against the census RE-STUNS.md S1.8 pins, and against synthetic rows written through the
// REAL container writer and read back through the REAL reader and the REAL tap table otherwise.
//
// The feed half runs against a fake component system built at the engine's own offsets -- vtable
// slot, handle map, element pool -- so the resolution this feed performs (actor -> SComponentID ->
// 72-byte record) is exercised rather than stubbed, and the delegate wrappers are fakes that record
// what they were handed.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

#include <vector>

#include "bootstrap/livesafety.h"
#include "capture/chapterbuilder.h"
#include "capture/manifestbuild.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/demotypes.h"
#include "generated/facts_generated.h"
#include "playback/core/stunfeed.h"
#include "playback/core/stunfeed_facts.h"
#include "../local_inputs.h"

using namespace replay;
using namespace replay::capture;
using namespace replay::playback;
using replay::playback::stunfeed::RecordedStuns;
using replay::playback::stunfeed::ScanCensus;
using replay::playback::stunfeed::Stun;

namespace {

namespace sf = replay::stunfeed_facts;

// ---- what a delegate call was handed -----------------------------------------------------------
struct Call {
    enum Kind { kHit, kStun } kind = kHit;
    void* cs = nullptr;
    uint32_t component = 0;
};
std::vector<Call> g_calls;
bool g_raise_on_stun = false;

void FakeOnHit(void* cs, uint32_t id) { g_calls.push_back(Call{Call::kHit, cs, id}); }
void FakeOnStun(void* cs, uint32_t id) {
    if (g_raise_on_stun) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    g_calls.push_back(Call{Call::kStun, cs, id});
}

// ---- the fake engine ---------------------------------------------------------------------------
// The actor guid a recorded slot maps to is `slot + 1` in the low qword, and the fake resolver
// turns that into component handle `slot` -- except for kActorWithNoComponent, which answers the
// engine's own 0xFFFF and must refuse rather than write anywhere.
constexpr uint64_t kActorWithNoComponent = 900;
constexpr uint16_t kComponentGeneration = 3;

void FakeFind(void* /*cs*/, void* out, uint64_t lo, uint64_t /*hi*/) {
    uint32_t id = sf::kNoComponent;
    if (lo >= 1 && lo <= 8) id = uint32_t(lo - 1) | (uint32_t(kComponentGeneration) << 16);
    memcpy(out, &id, sizeof id);
}

struct FakeCs {
    static constexpr uint32_t kElements = 8;
    uint8_t raw[0x200] = {};
    void* vtbl[64] = {};
    uint32_t map[32] = {};
    uint8_t pool[kElements * sf::kRecordStride] = {};

    FakeCs() {
        void* v = vtbl;
        memcpy(raw, &v, sizeof v);
        vtbl[facts::LOCAL_CS_VTSLOT_FIND_COMPONENT / sizeof(void*)] =
            reinterpret_cast<void*>(&FakeFind);
        void* m = map;
        memcpy(raw + sf::kCsHandleMapOff, &m, sizeof m);
        void* p = pool;
        memcpy(raw + sf::kCsPoolBaseOff, &p, sizeof p);
        // Element index == handle, so a record can be named by the slot that owns it.
        for (uint32_t i = 0; i < kElements; ++i) map[i] = i;
    }
    void* Ptr() { return raw; }
    uint8_t* Rec(uint32_t handle) { return pool + handle * sf::kRecordStride; }
    uint32_t Flags(uint32_t handle) {
        uint32_t v = 0;
        memcpy(&v, Rec(handle) + sf::kFlagsOff, sizeof v);
        return v;
    }
    float Elapsed(uint32_t handle) {
        float v = 0.0f;
        memcpy(&v, Rec(handle) + sf::kElapsedOff, sizeof v);
        return v;
    }
    float Length(uint32_t handle) {
        float v = 0.0f;
        memcpy(&v, Rec(handle) + sf::kLengthOff, sizeof v);
        return v;
    }
};

struct FakeNetGame {
    uint8_t raw[0x3000] = {};
    uint8_t space[64] = {};
    FakeNetGame() {
        void* s = space;
        memcpy(raw + facts::NETGAME_CS_SPACE_OFF, &s, sizeof s);
    }
    void* Ptr() { return raw; }
};

FakeCs* g_cs = nullptr;
void* g_lookup_space = nullptr;
uint64_t g_lookup_symbol = 0;

void* FakeLookup(void* space, uint64_t symbol) {
    g_lookup_space = space;
    g_lookup_symbol = symbol;
    return g_cs ? g_cs->Ptr() : nullptr;
}

constexpr uint64_t kFakeSymbol = 0x0123456789ABCDEFull;
const stunfeed::Handlers kFake{&FakeOnHit, &FakeOnStun, &FakeLookup, &FakeFind, kFakeSymbol};

// ---- the roster ---------------------------------------------------------------------------------
// Recorded slot 7 is on nobody's field; recorded slot 6 maps to a user whose actor carries no
// punchable component.
constexpr uint16_t kUnmappedRecSlot = 7;
constexpr uint16_t kNoComponentRecSlot = 6;

bool MapUser(void* /*ctx*/, uint16_t recSlot, stunfeed::LiveUser& out) {
    if (recSlot == kUnmappedRecSlot || recSlot > 7) return false;
    out.net_user_id = uint32_t(recSlot + 10u) | (4u << 16);
    out.actor_lo = recSlot == kNoComponentRecSlot ? kActorWithNoComponent : uint64_t(recSlot) + 1u;
    out.actor_hi = 0x99;
    return true;
}

// ---- the store ----------------------------------------------------------------------------------
std::vector<uint8_t> MakeBody(uint16_t victim, uint16_t puncher, float seconds, uint32_t flag) {
    std::vector<uint8_t> b(sf::kBodyBytes, 0);
    const uint32_t v = uint32_t(victim);  // generation 0, as the recording carries it
    const uint32_t p = uint32_t(puncher);
    memcpy(b.data() + sf::kOffVictim, &v, sizeof v);
    memcpy(b.data() + sf::kOffPuncher, &p, sizeof p);
    memcpy(b.data() + sf::kOffDuration, &seconds, sizeof seconds);
    memcpy(b.data() + sf::kOffFlag, &flag, sizeof flag);
    return b;
}

// Rows whose placement is the identity: one chapter whose span equals its arrival span, so the
// stamps the cases assert against are the stamps they were offered with.
struct StoreBuilder {
    ScanCensus census;
    RecordedStuns& s;
    uint64_t last = 0;
    explicit StoreBuilder(RecordedStuns& store) : s(store) {
        s.Reset();
        s.BeginChapter();
    }
    void Add(uint64_t tUs, uint16_t victim, uint16_t puncher, float seconds = 2.0f,
             uint32_t flag = 0) {
        const std::vector<uint8_t> b = MakeBody(victim, puncher, seconds, flag);
        REQUIRE(s.Offer(tUs, b.data(), uint32_t(b.size()), census));
        if (tUs > last) last = tUs;
    }
    void Done() { s.EndChapter(0, last + 1'000'000, 0, last + 1'000'000, census); }
};

// ---- the file -----------------------------------------------------------------------------------
struct TempDemo {
    wchar_t path[512];
    TempDemo() {
        wchar_t dir[400]; // facts-ok: AUDIO.LISTENER_COUNT_OFF a path-buffer length, not that offset
        const DWORD n = GetTempPathW(400, dir); // facts-ok: AUDIO.LISTENER_COUNT_OFF a path-buffer length, not that offset
        if (!n) {
            dir[0] = L'.';
            dir[1] = 0;
        }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lsstun1_%lu_%llu.echodemo", dir,
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

// Two stuns 41 ms apart on ONE victim, in the same chapter: the capture's own hardest case, written
// through the shipped writer so the arrival interpolation has to preserve the gap.
void WriteStunDemo(const wchar_t* path) {
    ManifestBuilder mb;
    mb.Begin(kT0, 0, 0x40);
    const uint8_t md5[16] = {0xca, 0xa0, 0x5e, 0xe3};
    mb.SetBuild(md5, 0x73747561736D7367ull, 0x0102030405060708ull);
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

    const uint16_t stunTap = TapTypeIndex("SR15StunPlayerMsg");
    REQUIRE(stunTap != kTapTypeIndexNone);
    const uint16_t otherTap = TapTypeIndex("SR15NetComponentEvent");
    REQUIRE(otherTap != kTapTypeIndexNone);

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(kT0);

    // The arrival axis is the recorder's wall clock, deliberately offset from the capture axis so a
    // feed that mistook one for the other could not pass.
    const uint64_t arrivalBase = 900'000'000ull;
    const uint64_t dt = 10'000;
    const uint64_t end = kT0 + 4 * kChapterUs;
    const std::vector<uint8_t> filler(24, 0x11);

    for (uint32_t f = 0; f * dt <= end - kT0; ++f) {
        const uint64_t st = kT0 + f * dt;
        const uint64_t arrival = arrivalBase + f * dt;
        TickRec tr{};
        tr.sourceTimeUs = st;
        tr.simTimeUs = 1'000'000 + f * dt;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = uint32_t(dt);
        tr.scaledDeltaUs = uint32_t(dt);
        Feed(b, kRecTick, tr);
        if (f % 4 == 0) FeedEventBody(b, arrival, otherTap, filler);

        const uint64_t into = (st - kT0) % kChapterUs;
        const uint64_t chapter = (st - kT0) / kChapterUs;
        if (chapter == 1 && into == kChapterUs / 2) {
            FeedEventBody(b, arrival, stunTap, MakeBody(5, 7, 2.0f, 0));
            FeedEventBody(b, arrival + 41'000, stunTap, MakeBody(5, 3, 2.0f, 0));
        }
        if (chapter == 2 && into == kChapterUs / 4) {
            FeedEventBody(b, arrival, stunTap, MakeBody(0, 3, 2.0f, 1));
            // A duration the engine's timer could not be running: refused at decode, not stored.
            FeedEventBody(b, arrival + 1000, stunTap, MakeBody(1, 2, 0.0f, 0));  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF a 1000 ms gap in this fixture's timeline, not a vtable slot
        }
    }
    b.EmitMatchEnd(end);
    b.FlushChapter();
    b.Close();
    writer.Close();
}

// ---- the fixture --------------------------------------------------------------------------------
struct FeedFixture {
    FakeCs cs;
    FakeNetGame ng;
    FeedFixture() {
        g_calls.clear();
        g_raise_on_stun = false;
        g_cs = &cs;
        g_lookup_space = nullptr;
        g_lookup_symbol = 0;
        stunfeed::InjectHandlersForTest(&kFake);
        stunfeed::PlaybackRecordedStuns().Reset();
        stunfeed::ResetCensusForTest();
        stunfeed::SetPlayheadOriginUs(0);
        stunfeed::Arm(true);
        // The real offline lock is not held in a test process, so the verdict is OVERRIDDEN rather
        // than published, exactly as the goal feed's fixture does it.
        livesafety::SetForTest(false, true);
    }
    ~FeedFixture() {
        livesafety::ResetForTest();
        stunfeed::Arm(false);
        stunfeed::InjectHandlersForTest(nullptr);
        stunfeed::PlaybackRecordedStuns().Reset();
        g_cs = nullptr;
        g_calls.clear();
    }
    void Sweep(int64_t tUs, bool hold = false) {
        stunfeed::Sweep(ng.Ptr(), tUs, hold, &MapUser, nullptr);
    }
};

}  // namespace

// =================================================================================================
// Decode
// =================================================================================================

TEST_CASE("a written .echodemo's stun rows decode through the real container and keep their gaps") {
    TempDemo tmp;
    WriteStunDemo(tmp.path);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));

    RecordedStuns s;
    ScanCensus c;
    const uint32_t stored = stunfeed::ScanRecordedStuns(reader, s, c);

    CHECK(stored == 3);
    CHECK(s.Count() == 3);
    CHECK(c.tap != 0xFFFF);  // facts-ok: NETVAR.SLOT_NONE the feed's own all-ones u16 "none"
    CHECK(c.tap_chapters > 0);
    CHECK(c.refused_rows == 1);  // the zero-duration row

    SUBCASE("the fields survive the round trip") {
        CHECK(s.At(0).victim == 5u);
        CHECK(s.At(0).puncher == 7u);
        CHECK(s.At(0).duration == doctest::Approx(2.0f));
        CHECK_FALSE(s.At(0).blocked);
        CHECK(s.At(1).victim == 5u);
        CHECK(s.At(1).puncher == 3u);
        CHECK(s.At(2).victim == 0u);
        CHECK(s.At(2).blocked);
    }

    SUBCASE("the 41 ms double stun is still 41 ms apart after the arrival placement") {
        const int64_t gap = int64_t(s.At(1).t_us) - int64_t(s.At(0).t_us);
        CHECK(gap > 30'000);
        CHECK(gap < 55'000);
    }

    SUBCASE("one victim, two rows, and the newest at a playhead is the one that restarted him") {
        CHECK(s.VictimCount() == 2);
        const int32_t inFirst = s.LastForVictimAtOrBefore(5, int64_t(s.At(0).t_us) + 10'000);
        CHECK(inFirst == 0);
        const int32_t inSecond = s.LastForVictimAtOrBefore(5, int64_t(s.At(1).t_us) + 10'000);
        CHECK(inSecond == 1);
    }
}

#define STUN_WIDEN_(x) L##x
#define STUN_WIDEN(x) STUN_WIDEN_(x)
#define STUN_CAPTURE "2026-09-19_21-11-01_576ED3F8428EBC4B_FFFFFFFFFFFFFFFF.echodemo"

TEST_CASE("the owner's own capture decodes to the eight stuns the census pins") {
    const std::wstring candidates[] = {
        STUN_WIDEN(REPLAY_PLAYBACK_FIXTURE_DIR) L"/" STUN_WIDEN(STUN_CAPTURE),
        replay_test::CaptureFileW(STUN_WIDEN(STUN_CAPTURE)),
    };
    DemoReader reader;
    bool opened = false;
    for (const std::wstring& p : candidates) {
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && reader.Open(p.c_str())) {
            opened = true;
            break;
        }
    }
    if (!opened) {
        MESSAGE("SKIPPED BY NAME: the 2026-09-19 capture is on neither path");
        return;
    }

    RecordedStuns s;
    ScanCensus c;
    const uint32_t stored = stunfeed::ScanRecordedStuns(reader, s, c);

    // RE-STUNS.md S1.8, and it is a complete set: the recorded stat counters move exactly eight
    // times and no more.
    REQUIRE(stored == 8);
    CHECK(c.refused_rows == 0);
    CHECK(c.dropped_rows == 0);

    struct Row {
        double contentS;
        uint16_t victim;
        uint16_t puncher;
        bool blocked;
    };
    const Row want[] = {
        {43.089, 0, 3, false}, {45.592, 0, 3, true},  {49.229, 4, 1, false},
        {61.778, 2, 0, false}, {61.857, 7, 6, false}, {70.956, 5, 7, false},
        {70.997, 5, 3, false}, {73.753, 3, 5, false},
    };
    // The content window opens at capture second 3862.614 (RE-STUNS.md S1.0).
    const double kContentOriginS = 3862.614;
    for (uint32_t i = 0; i < 8; ++i) {
        const Stun& got = s.At(i);
        CHECK(got.victim == want[i].victim);
        CHECK(got.puncher == want[i].puncher);
        CHECK(got.blocked == want[i].blocked);
        CHECK(got.duration == doctest::Approx(2.0f));
        // The stamp is the tap's ARRIVAL axis interpolated into its chapter, which RE-STUNS.md S3(b)
        // bounds at about 20 ms on this file.  A tenth of a second is the loosest bound that still
        // separates every row in the census from every other one.
        const double err = double(got.t_us) / 1e6 - kContentOriginS - want[i].contentS;
        CHECK(err < 0.10);
        CHECK(err > -0.10);
    }

    SUBCASE("the two pairs the counters could never have separated are still separated") {
        const int64_t near79 = int64_t(s.At(4).t_us) - int64_t(s.At(3).t_us);
        CHECK(near79 > 55'000);
        CHECK(near79 < 105'000);
        const int64_t near41 = int64_t(s.At(6).t_us) - int64_t(s.At(5).t_us);
        CHECK(near41 > 20'000);
        CHECK(near41 < 65'000);
    }

    SUBCASE("six distinct victims, and garlicyummy is the one stunned twice") {
        CHECK(s.VictimCount() == 6);
    }
}

// =================================================================================================
// The edge
// =================================================================================================

TEST_CASE("a forward crossing writes the record then raises OnHit and OnStun once") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, /*victim=*/2, /*puncher=*/3);
    sb.Done();
    memset(fx.cs.Rec(2), 0xAA, sf::kRecordStride);

    fx.Sweep(900'000);  // the first sweep re-bases; nothing has been crossed yet
    CHECK(g_calls.empty());
    CHECK(stunfeed::Census().fires == 0);

    fx.Sweep(1'100'000);
    REQUIRE(g_calls.size() == 2);
    CHECK(stunfeed::Census().fires == 1);

    SUBCASE("OnHit comes first, and both carry the same live component id") {
        CHECK(g_calls[0].kind == Call::kHit);
        CHECK(g_calls[1].kind == Call::kStun);
        CHECK(g_calls[0].component == g_calls[1].component);
        CHECK((g_calls[0].component & 0xFFFFu) == 2u);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed component id, not the engine no-slot field
        CHECK((g_calls[0].component >> 16) == kComponentGeneration);
        CHECK(g_calls[0].cs == fx.cs.Ptr());
    }

    SUBCASE("the component system was found by the symbol read out of the image") {
        CHECK(g_lookup_symbol == kFakeSymbol);
        CHECK(g_lookup_space == static_cast<void*>(fx.ng.space));
    }

    SUBCASE("the three fields the engine's own handler writes, and nothing else") {
        CHECK((fx.cs.Flags(2) & sf::kFlagStunned) != 0);
        CHECK((fx.cs.Flags(2) & sf::kFlagStunPulse) != 0);
        CHECK((fx.cs.Flags(2) & sf::kFlagInvulnerable) == 0);
        CHECK(fx.cs.Elapsed(2) == doctest::Approx(0.0f));
        CHECK(fx.cs.Length(2) == doctest::Approx(2.0f));
        // Every other byte of the 72 is still the 0xAA it was filled with: this feed writes three
        // fields, and a stat counter is not one of them.
        const uint8_t* rec = fx.cs.Rec(2);
        for (uint32_t i = 0; i < sf::kRecordStride; ++i) {
            const bool owned = (i >= sf::kFlagsOff && i < sf::kFlagsOff + 4) ||
                               (i >= sf::kElapsedOff && i < sf::kElapsedOff + 4) ||
                               (i >= sf::kLengthOff && i < sf::kLengthOff + 4);
            if (!owned) CHECK(rec[i] == 0xAA);
        }
    }

    SUBCASE("staying on the row does not fire it again") {
        fx.Sweep(1'200'000);
        fx.Sweep(1'300'000);
        CHECK(g_calls.size() == 2);
        CHECK(stunfeed::Census().fires == 1);
    }
}

TEST_CASE("the 41 ms double stun fires twice and the second restarts the timer") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, 5, 7);
    sb.Add(1'041'000, 5, 3);
    sb.Done();

    fx.Sweep(900'000);
    fx.Sweep(1'010'000);
    REQUIRE(g_calls.size() == 2);
    CHECK(fx.cs.Elapsed(5) == doctest::Approx(0.0f));

    // Let the engine's own timer be somewhere into the first stun, then cross the second.
    float advanced = 0.031f;
    memcpy(fx.cs.Rec(5) + sf::kElapsedOff, &advanced, sizeof advanced);
    fx.Sweep(1'050'000);

    CHECK(stunfeed::Census().fires == 2);
    REQUIRE(g_calls.size() == 4);
    CHECK(g_calls[2].kind == Call::kHit);
    CHECK(g_calls[3].kind == Call::kStun);
    CHECK(fx.cs.Elapsed(5) == doctest::Approx(0.0f));
}

TEST_CASE("nothing fires on a seek on a reverse step on the pre-roll or with the feed off") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, 2, 3);
    sb.Done();

    SUBCASE("a forward seek across the row consumes it") {
        fx.Sweep(500'000);
        stunfeed::NoteSeek();
        fx.Sweep(1'500'000);
        CHECK(g_calls.empty());
        CHECK(stunfeed::Census().fires == 0);
        CHECK(stunfeed::Census().seeks_suppressed == 1);
    }

    SUBCASE("a backward step raises nothing") {
        fx.Sweep(1'500'000);
        fx.Sweep(1'400'000);
        CHECK(g_calls.empty());
        CHECK(stunfeed::Census().fires == 0);
        CHECK(stunfeed::Census().reverse_levels == 1);
    }

    SUBCASE("the pre-roll's own wind raises nothing and writes nothing") {
        fx.Sweep(500'000, /*hold=*/true);
        fx.Sweep(1'500'000, /*hold=*/true);
        CHECK(g_calls.empty());
        CHECK(stunfeed::Census().holds == 2);
        CHECK(stunfeed::Census().level_writes == 0);
    }

    SUBCASE("the key being off is the whole gate") {
        stunfeed::Arm(false);
        fx.Sweep(500'000);
        fx.Sweep(1'500'000);
        CHECK(g_calls.empty());
        CHECK(stunfeed::Census().fires == 0);
    }

    SUBCASE("containment refuses before anything is resolved") {
        livesafety::SetForTest(false, false);
        fx.Sweep(500'000);
        fx.Sweep(1'500'000);
        CHECK(g_calls.empty());
        CHECK(stunfeed::Census().fires == 0);
    }
}

// =================================================================================================
// The level
// =================================================================================================

TEST_CASE("a seek into a stun span restores bit1 and the remaining duration without raising") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, 2, 3);
    sb.Done();

    fx.Sweep(500'000);
    stunfeed::NoteSeek();
    fx.Sweep(1'750'000);  // 0.75 s into a 2.0 s stun

    CHECK(g_calls.empty());
    CHECK(stunfeed::Census().level_writes >= 1);
    CHECK((fx.cs.Flags(2) & sf::kFlagStunned) != 0);
    // bit0 is a one-frame pulse the engine's own Update clears; a level restore is not an edge.
    CHECK((fx.cs.Flags(2) & sf::kFlagStunPulse) == 0);
    CHECK((fx.cs.Flags(2) & sf::kFlagInvulnerable) == 0);
    CHECK(fx.cs.Elapsed(2) == doctest::Approx(0.75f));
    CHECK(fx.cs.Length(2) == doctest::Approx(2.0f));
}

TEST_CASE("a seek out of a stun span clears the stunned bit and leaves invulnerability alone") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, 2, 3);
    sb.Done();

    fx.Sweep(900'000);
    fx.Sweep(1'100'000);  // fire
    REQUIRE(stunfeed::Census().fires == 1);
    // The engine's own Update would have set the post-stun invulnerability by the time the span
    // ends; the feed must not take it away, because how long it lasts is not in the recording.
    uint32_t flags = sf::kFlagStunned | sf::kFlagInvulnerable;
    memcpy(fx.cs.Rec(2) + sf::kFlagsOff, &flags, sizeof flags);

    stunfeed::NoteSeek();
    fx.Sweep(5'000'000);
    CHECK((fx.cs.Flags(2) & sf::kFlagStunned) == 0);
    CHECK((fx.cs.Flags(2) & sf::kFlagInvulnerable) != 0);
    CHECK(stunfeed::Census().level_clears == 1);

    SUBCASE("a second seek to the same place has nothing left to write") {
        const uint32_t before = stunfeed::Census().level_writes;
        stunfeed::NoteSeek();
        fx.Sweep(5'000'000);
        CHECK(stunfeed::Census().level_writes == before);
    }
}

TEST_CASE("the level answers from the NEWEST row on that victim so a double stun holds him longer") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, 5, 7);
    sb.Add(1'041'000, 5, 3);
    sb.Done();

    stunfeed::NoteSeek();
    fx.Sweep(2'900'000);  // past the FIRST stun's end, inside the SECOND's
    CHECK(g_calls.empty());
    CHECK((fx.cs.Flags(5) & sf::kFlagStunned) != 0);
    CHECK(fx.cs.Elapsed(5) == doctest::Approx(1.859f));
}

// =================================================================================================
// Refusals
// =================================================================================================

TEST_CASE("a recorded user who is not on the field is a named refusal and never a guessed slot") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, kUnmappedRecSlot, 3);
    sb.Done();

    fx.Sweep(900'000);
    fx.Sweep(1'100'000);
    CHECK(g_calls.empty());
    CHECK(stunfeed::Census().fires == 0);
    CHECK(stunfeed::Census().unmapped_victim == 1);
}

TEST_CASE("a victim whose actor carries no punchable component refuses rather than writing") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, kNoComponentRecSlot, 3);
    sb.Done();

    fx.Sweep(900'000);
    fx.Sweep(1'100'000);
    CHECK(g_calls.empty());
    CHECK(stunfeed::Census().fires == 0);
    CHECK(stunfeed::Census().refused_handle == 1);
}

TEST_CASE("an unmapped puncher is counted and does not stop the victim's stun") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, 2, kUnmappedRecSlot);
    sb.Done();

    fx.Sweep(900'000);
    fx.Sweep(1'100'000);
    // Neither delegate carries the stunner, so a stunner nobody can name changes nothing the
    // engine is told.
    CHECK(g_calls.size() == 2);
    CHECK(stunfeed::Census().fires == 1);
    CHECK(stunfeed::Census().unmapped_puncher == 1);
}

TEST_CASE("a delegate that raises turns the feed off for the session rather than retrying it") {
    FeedFixture fx;
    StoreBuilder sb(stunfeed::PlaybackRecordedStuns());
    sb.Add(1'000'000, 2, 3);
    sb.Add(2'000'000, 4, 3);
    sb.Done();

    g_raise_on_stun = true;
    fx.Sweep(900'000);
    fx.Sweep(1'100'000);
    CHECK(stunfeed::Census().faulted == 1);
    CHECK_FALSE(stunfeed::Armed());

    g_raise_on_stun = false;
    fx.Sweep(2'100'000);
    CHECK(stunfeed::Census().fires == 0);
}
