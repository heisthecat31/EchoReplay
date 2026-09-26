// The component-event feed: the decode, the (script, event) allow-list, the
// forward-only edge, and the handle validation that stands between a recorded handle and the wrong
// actor on this host.
//
// The capture in hand carries 5,429 of these rows and the allow-list admits four, so the decode
// half runs against 24-byte rows minted here and written through the REAL container writer, read
// back through the REAL reader and the REAL tap table. What is synthesized is the CONTENT; every
// byte of framing is the shipped path's.
//
// The feed half runs against a fake CR15NetGame::ComponentEventCB and a fake CR15NetIdCS::Actor
// that implements the engine's own three-part check, because index / generation / live is exactly
// what the recorded handles have to survive on a host that is not the recording server.

#include "doctest.h"

#include <windows.h>

#include <stdint.h>
#include <string.h>

#include <vector>

#include "bootstrap/livesafety.h"
#include "capture/chapterbuilder.h"
#include "capture/manifestbuild.h"
#include "engine/scriptgraph_facts.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/demotypes.h"
#include "playback/core/componentfeed.h"
#include "playback/core/componentfeed_facts.h"

using namespace replay;
using namespace replay::capture;
using namespace replay::playback;
using replay::playback::componentfeed::RecordedEvents;
using replay::playback::componentfeed::Row;
using replay::playback::componentfeed::ScanCensus;

namespace {

namespace cef = replay::componentfeed_facts;

// The capture's own values, from RE-LAUNCH-TUBES §3.1 and RE-COMPONENT-EVENT-ENTRY §3.5.
constexpr uint64_t kTubeScript = 0x6aea99eeb2bab6c4ull;
constexpr uint64_t kLaunchSym = 0x6e0a199402b8e9afull;   // nevt_launch_sequence
constexpr uint64_t kReturnSym = 0x627e045b095b618full;   // nevt_return_sequence
// The DENY that shares the tube's script id: its listener forces a grab release and re-sends to
// self-and-all-peers, which is the reason the key is the PAIR.
constexpr uint64_t kDenySym = 0x7e0e1c8017a3e6beull;
constexpr uint64_t kGametypeScript = replay::scriptgraph_facts::kArenaScriptHash;

constexpr uint16_t kTubeA = 0x0023;
constexpr uint16_t kTubeB = 0x001e;

std::vector<uint8_t> MakeBody(uint64_t sym, uint64_t script, uint16_t handle, uint16_t flags,
                              uint32_t param) {
    std::vector<uint8_t> b(componentfeed::kBodyBytes, 0);
    memcpy(b.data() + cef::kOffSymbol, &sym, sizeof sym);
    memcpy(b.data() + cef::kOffScriptId, &script, sizeof script);
    memcpy(b.data() + cef::kOffHandle, &handle, sizeof handle);
    memcpy(b.data() + cef::kOffFlags, &flags, sizeof flags);
    memcpy(b.data() + cef::kOffParam, &param, sizeof param);
    return b;
}

// ---- what the engine was handed ----------------------------------------------------------------
struct Call {
    uint8_t body[componentfeed::kBodyBytes] = {};
    uint64_t peer = 0;
    const void* target = nullptr;
};

std::vector<Call> g_calls;
bool g_raise_entry = false;

char FakeEntry(void* netGame, void* smsg) {
    if (g_raise_entry) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    const uint64_t* m = static_cast<const uint64_t*>(smsg);
    Call c;
    c.target = netGame;
    c.peer = m[0];
    memcpy(c.body, reinterpret_cast<const void*>(uintptr_t(m[2])), componentfeed::kBodyBytes);
    g_calls.push_back(c);
    return 1;
}

// The engine's own three-part check, standing in for sub_140CA5BA0: index < count, generation
// match, live bit. Everything the recorded handles have to survive.
struct FakePool {
    uint16_t count = 0x40;
    uint16_t gen[0x40] = {};
    bool live[0x40] = {};
    FakePool() {
        for (bool& b : live) b = true;
    }
};
FakePool g_pool;
uint64_t g_self_peer = 0xDEADBEEF12345678ull;

void* FakeResolve(void* out16, uint16_t handle) {
    uint64_t* o = static_cast<uint64_t*>(out16);
    const uint16_t idx = handle & cef::kHandleIndexMask;
    const uint16_t gen = uint16_t(handle >> cef::kHandleGenShift);
    if (idx < g_pool.count && idx < 0x40 && g_pool.gen[idx] == gen && g_pool.live[idx]) {
        o[0] = 0x5350414345000000ull | idx;   // a space symbol that differs per slot
        o[1] = 0x0000000000000000ull | idx;   // actorRef, low 16 bits != 0xFFFF
    } else {
        o[0] = cef::kInvalidActorSpace;
        o[1] = cef::kInvalidActorRef;
    }
    return out16;
}

const componentfeed::Handlers kFake{&FakeEntry, &FakeResolve, &g_self_peer, nullptr};

// A netGame the probe line can read a level-space pointer out of without faulting.
struct FakeNetGame {
    uint8_t bytes[16u * 1024u] = {};
    void* Ptr() { return bytes; }
};

// ---- the file ----------------------------------------------------------------------------------
struct TempDemo {
    wchar_t path[512];
    TempDemo() {
        wchar_t dir[400];  // facts-ok: AUDIO.LISTENER_COUNT_OFF a temp-directory buffer length, not the listener-count offset
        const DWORD n = GetTempPathW(static_cast<DWORD>(sizeof(dir) / sizeof(dir[0])), dir);
        if (!n) { dir[0] = L'.'; dir[1] = 0; }
        _snwprintf_s(path, 512, _TRUNCATE, L"%lscevt1_%lu_%llu.echodemo", dir,
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

constexpr uint64_t kT0 = 20'000'000ull;
constexpr uint64_t kChapterUs = 2'000'000ull;

struct SynthTimes {
    uint64_t t0 = 0;
    uint64_t end = 0;
};

// Two tubes' worth of launch/return, one DENY row on the same script id, and a wall of gametype
// rows -- the shape of the real capture, three orders of magnitude smaller.
SynthTimes WriteEventDemo(const wchar_t* path) {
    SynthTimes t{};
    t.t0 = kT0;
    t.end = t.t0 + 8 * kChapterUs;

    ManifestBuilder mb;
    mb.Begin(t.t0, 0, 0x40);
    const uint8_t md5[16] = {0xce, 0x77, 0x01, 0x02};
    mb.SetBuild(md5, 0x746562656C696768ull, 0x0102030405060708ull);
    mb.SetLevel(6300205991959903307ull, 0xF00Dull, 1780177787ull);
    mb.SetKeyframeIntervalUs(kChapterUs);
    mb.SetUuid(9, 9);
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

    const uint16_t tap = TapTypeIndex("SR15NetComponentEvent");
    REQUIRE(tap != kTapTypeIndexNone);

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(t.t0);

    // The arrival axis is the recorder's wall clock, deliberately offset from the capture axis so a
    // feed that mistook one for the other could not pass.
    const uint64_t arrivalBase = 900'000'000ull;
    const uint64_t dt = 20000;
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

        // The gametype wall: 99.8% of the real capture's rows, and every one of them refused.
        if (f % 4 == 0) {
            FeedEventBody(b, arrival, tap,
                          MakeBody(0xa4ef01205f5062a1ull, kGametypeScript, 0x002f, 0x0005,
                                   0xFFFFFFFFu));  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
        }

        const uint64_t intoChapter = (st - t.t0) % kChapterUs;
        const uint64_t chapter = (st - t.t0) / kChapterUs;
        const bool at = intoChapter >= kChapterUs / 2 && intoChapter < kChapterUs / 2 + dt;
        if (!at) continue;
        if (chapter == 1) {
            // The launch trigger the allow-list refuses, and the light-rig launch it admits -- same
            // recorded script id, one row apart.
            FeedEventBody(b, arrival, tap, MakeBody(kDenySym, kTubeScript, kTubeA, 0x000d, 0));
            // Relay bit SET on an allow-listed row, which the capture's own rows do not carry: the
            // masking has to be proved on a row that would otherwise re-broadcast.
            FeedEventBody(b, arrival + 1, tap, MakeBody(kLaunchSym, kTubeScript, kTubeA, 0x000d, 4));
        }
        if (chapter == 2) {
            FeedEventBody(b, arrival, tap, MakeBody(kReturnSym, kTubeScript, kTubeA, 0x0005, 4));
        }
        if (chapter == 4) {
            FeedEventBody(b, arrival, tap, MakeBody(kLaunchSym, kTubeScript, kTubeB, 0x0005, 4));
        }
        if (chapter == 5) {
            FeedEventBody(b, arrival, tap, MakeBody(kReturnSym, kTubeScript, kTubeB, 0x0005, 4));
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
        g_raise_entry = false;
        g_pool = FakePool{};
        componentfeed::InjectHandlersForTest(&kFake);
        componentfeed::InjectScriptIdResolverForTest(nullptr, nullptr);
        componentfeed::PlaybackRecordedEvents().Reset();
        componentfeed::ResetCensusForTest();
        componentfeed::SetPlayheadOriginUs(0);
        componentfeed::Arm(true);
        // The real offline lock is not held in a test process, so the verdict is OVERRIDDEN rather
        // than published, exactly as the goal feed's fixture does it.
        livesafety::SetForTest(false, true);
    }
    ~FeedFixture() {
        livesafety::ResetForTest();
        componentfeed::Arm(false);
        componentfeed::InjectScriptIdResolverForTest(nullptr, nullptr);
        componentfeed::InjectHandlersForTest(nullptr);
        g_calls.clear();
    }
};

// One launch and its return on tube A, at known playhead times, in the process store the shipped
// feed reads.
void PairInStore(uint16_t handle = kTubeA, uint16_t launchFlags = 0x000d) {
    RecordedEvents& s = componentfeed::PlaybackRecordedEvents();
    s.Reset();
    ScanCensus c;
    const std::vector<uint8_t> launch = MakeBody(kLaunchSym, kTubeScript, handle, launchFlags, 4);
    const std::vector<uint8_t> ret = MakeBody(kReturnSym, kTubeScript, handle, 0x0005, 4);
    REQUIRE(s.Offer(1'000'000, launch.data(), uint32_t(launch.size()), c));
    REQUIRE(s.Offer(5'000'000, ret.data(), uint32_t(ret.size()), c));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The allow-list
// ---------------------------------------------------------------------------------------------

TEST_CASE("the allow-list key is the PAIR: one script id, one admitted symbol, one refused") {
    CHECK(componentfeed::Classify(kTubeScript, kLaunchSym).allowed);
    CHECK(componentfeed::Classify(kTubeScript, kReturnSym).allowed);
    CHECK(componentfeed::Classify(kTubeScript, kReturnSym).resets);
    CHECK_FALSE(componentfeed::Classify(kTubeScript, kLaunchSym).resets);

    // Same recorded script id, and its listener re-sends to self-and-all-peers.
    CHECK_FALSE(componentfeed::Classify(kTubeScript, kDenySym).allowed);
    // Same symbol, a script id the list does not name.
    CHECK_FALSE(componentfeed::Classify(kGametypeScript, kLaunchSym).allowed);
    // The headline DENYs.
    CHECK_FALSE(componentfeed::Classify(kGametypeScript, 0xa4ef01205f5062a1ull).allowed);
    CHECK_FALSE(componentfeed::Classify(0xb9e136910995a482ull, kLaunchSym).allowed);
    CHECK_FALSE(componentfeed::Classify(0xFFFFFFFFFFFFFFFFull, kLaunchSym).allowed);  // facts-ok: LOBBY.JOIN_FREE_SENTINEL an all-ones u64 "none" local to this code, not the lobby join sentinel
}

TEST_CASE("The announcer's goal calls are admitted, its possession and win calls are not") {
    constexpr uint64_t kAnnouncer = 0x949c090e2472d440ull;
    // Goal calls recorded in the owner's capture, each subscribed only by a1991b50876efaa1.
    CHECK(componentfeed::Classify(kAnnouncer, 0x6cfb6cd15bd19ca7ull).allowed);  // "3 points never looked so pretty!"
    CHECK(componentfeed::Classify(kAnnouncer, 0x47a24a8203460acdull).allowed);  // "2 points on the board!"
    CHECK(componentfeed::Classify(kAnnouncer, 0x588766ed78a8935cull).allowed);  // "Goal!"
    CHECK(componentfeed::Classify(kAnnouncer, 0x6cfb6cd15bd190a7ull).allowed);  // "He eyes it. He tries it. He buys it."
    CHECK(componentfeed::Classify(kAnnouncer, 0x452fcfebcf5cf578ull).allowed);
    CHECK(componentfeed::Classify(kAnnouncer, 0x7b5cc8d9d65ac704ull).allowed);
    CHECK(componentfeed::Classify(kAnnouncer, 0xc24ac30553900ddaull).allowed);
    CHECK_FALSE(componentfeed::Classify(kAnnouncer, 0x588766ed78a8935cull).resets);
    // Same script, a possession call: the host's own tracker already voices these.
    CHECK_FALSE(componentfeed::Classify(kAnnouncer, 0x4cf8c8adf6bb7b58ull).allowed);  // "Interception!"
    // Same script, a subscription whose thread raises fx_blue_wins into a profile-writing listener.
    CHECK_FALSE(componentfeed::Classify(kAnnouncer, 0xb3f09125076d2741ull).allowed);
    // Goal calls whose thread reaches a SENDEVENTGLOBALNODE only through a body-less pass-through
    // node, so a pseudocode edge walk missed them.
    for (uint64_t raisesFxWins : {0x1f527e2935a6b9dfull, 0x4685e374e793a327ull, 0x782f472b900ed4cbull,
                                  0x92b31a84252e8bbdull, 0xb3f09025076d2741ull, 0xb3f09625076d2741ull,
                                  0xc24ca52ce46181ffull, 0xcecb2e2dd5cb00bbull}) {
        CAPTURE(raisesFxWins);
        CHECK_FALSE(componentfeed::Classify(kAnnouncer, raisesFxWins).allowed);
    }
    // A goal-call symbol under any other script id is still refused: the key is the pair.
    CHECK_FALSE(componentfeed::Classify(kTubeScript, 0x588766ed78a8935cull).allowed);
}

// ---------------------------------------------------------------------------------------------
// The decode, through the real writer and reader
// ---------------------------------------------------------------------------------------------

TEST_CASE("a written .echodemo's component rows decode through the real container and the list") {
    TempDemo tmp;
    const SynthTimes t = WriteEventDemo(tmp.path);

    DemoReader reader;
    REQUIRE(reader.Open(tmp.path));

    RecordedEvents s;
    ScanCensus c;
    const uint32_t stored = componentfeed::ScanRecordedEvents(reader, s, c);

    // Four allow-listed rows out of everything written -- the real capture's own proportion.
    CHECK(stored == 4);
    CHECK(s.Count() == 4);
    CHECK(c.rows == 4);
    CHECK(c.tap != 0xFFFF);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(c.tap_chapters > 0);
    CHECK(c.denied_rows > 0);
    CHECK(c.component_rows == c.rows + c.denied_rows);

    // The refusals are named per pair, which is what a later widening has to be argued from.
    bool sawDenyPair = false, sawGametypePair = false, sawAllowPair = false;
    for (uint32_t i = 0; i < c.pair_count; ++i) {
        if (c.pairs[i].script_id == kTubeScript && c.pairs[i].symbol == kDenySym) {
            sawDenyPair = true;
            CHECK_FALSE(c.pairs[i].allowed);
        }
        if (c.pairs[i].script_id == kGametypeScript) {
            sawGametypePair = true;
            CHECK_FALSE(c.pairs[i].allowed);
            CHECK(c.pairs[i].rows > 4);
        }
        if (c.pairs[i].script_id == kTubeScript && c.pairs[i].symbol == kLaunchSym) {
            sawAllowPair = true;
            CHECK(c.pairs[i].allowed);
        }
    }
    CHECK(sawDenyPair);
    CHECK(sawGametypePair);
    CHECK(sawAllowPair);
    CHECK(c.pair_overflow == 0);

    // In order, on the PLAYHEAD axis, and inside the chapters they were written into -- not on the
    // arrival axis, which is 880 s away from it.
    CHECK(s.At(0).symbol == kLaunchSym);
    CHECK(s.At(0).handle == kTubeA);
    CHECK(s.At(1).symbol == kReturnSym);
    CHECK(s.At(2).handle == kTubeB);
    CHECK(s.At(3).symbol == kReturnSym);
    for (uint32_t i = 0; i + 1 < s.Count(); ++i) CHECK(s.At(i).t_us <= s.At(i + 1).t_us);
    CHECK(s.At(0).t_us > t.t0);
    CHECK(s.At(3).t_us < t.end);
    CHECK(c.placed_rows == 4);

    // The span question the report has to answer: how many of them a viewer can actually reach.
    CHECK(s.CountInSpan(int64_t(t.t0), int64_t(t.end)) == 4);
    CHECK(s.CountInSpan(int64_t(s.At(2).t_us), int64_t(t.end)) == 2);

    // The bytes are the file's, unedited in the store.
    CHECK(s.At(0).flags == 0x000d);
    CHECK((s.At(0).body[cef::kOffFlags] & cef::kRelayBit) != 0);
}

TEST_CASE("a launch with no return, and a return with no launch, are dropped at the scan") {
    RecordedEvents s;
    ScanCensus c;
    const std::vector<uint8_t> launch = MakeBody(kLaunchSym, kTubeScript, kTubeA, 0x0005, 4);
    const std::vector<uint8_t> orphanReturn = MakeBody(kReturnSym, kTubeScript, kTubeB, 0x0005, 4);
    REQUIRE(s.Offer(1'000'000, launch.data(), uint32_t(launch.size()), c));
    REQUIRE(s.Offer(2'000'000, orphanReturn.data(), uint32_t(orphanReturn.size()), c));
    CHECK(c.rows == 2);
    s.DropUnpairedRows(c);
    // The rig walks a segment list; half a sequence strands it lit and parented.
    CHECK(s.Count() == 0);
    CHECK(c.unpaired_rows == 2);
    CHECK(c.rows == 0);
}

// ---------------------------------------------------------------------------------------------
// The transport rules
// ---------------------------------------------------------------------------------------------

TEST_CASE("the component feed fires forward, once, and only forward") {
    FeedFixture fx;
    FakeNetGame ng;
    PairInStore();

    SUBCASE("a forward crossing fires each row once") {
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.size() == 1);
        componentfeed::Sweep(ng.Ptr(), 3'000'000, false);
        CHECK(g_calls.size() == 1);   // nothing new to cross
        componentfeed::Sweep(ng.Ptr(), 6'000'000, false);
        CHECK(g_calls.size() == 2);
        CHECK(componentfeed::Census().fires == 2);
    }

    SUBCASE("the first sweep of a load re-bases instead of sweeping the span") {
        componentfeed::Sweep(ng.Ptr(), 9'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().fires == 0);
    }

    SUBCASE("a SEEK across both rows fires nothing -- no catch-up burst") {
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::NoteSeek();
        componentfeed::Sweep(ng.Ptr(), 9'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().seeks_suppressed == 1);
        CHECK(componentfeed::Census().fires == 0);
    }

    SUBCASE("reverse fires nothing and consumes nothing") {
        componentfeed::Sweep(ng.Ptr(), 9'000'000, false);
        componentfeed::Sweep(ng.Ptr(), 4'000'000, false);
        componentfeed::Sweep(ng.Ptr(), 500'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().reverse_skipped == 2);
        // ...and the crossing set is recomputed from the playhead on the next forward motion.
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.size() == 1);
    }

    SUBCASE("the pre-roll hold re-bases and fires nothing") {
        componentfeed::Sweep(ng.Ptr(), 0, true);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, true);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().holds == 2);
        // And the first real forward tick after the hold does not replay the held span.
        componentfeed::Sweep(ng.Ptr(), 2'100'000, false);
        CHECK(g_calls.empty());
    }

    SUBCASE("a pause on top of a row cannot fire it twice") {
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 1'000'000, false);
        CHECK(g_calls.size() == 1);
        for (int i = 0; i < 5; ++i) componentfeed::Sweep(ng.Ptr(), 1'000'000, false);
        CHECK(g_calls.size() == 1);
    }

    SUBCASE("a return whose launch this pass never fired is skipped, not guessed at") {
        // Land between the two rows without crossing the launch, then run forward over the return.
        componentfeed::Sweep(ng.Ptr(), 3'000'000, false);
        componentfeed::Sweep(ng.Ptr(), 6'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().unpaired_skipped == 1);
    }
}

TEST_CASE("the relay bit is cleared in the bytes the engine is handed, and nothing else is") {
    FeedFixture fx;
    FakeNetGame ng;
    PairInStore(kTubeA, /*launchFlags=*/0x000d);

    componentfeed::Sweep(ng.Ptr(), 0, false);
    componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
    REQUIRE(g_calls.size() == 1);

    const Call& c = g_calls[0];
    CHECK((c.body[cef::kOffFlags] & cef::kRelayBit) == 0);
    // The space selector, the other three bits of that byte, survives: masking must not widen.
    CHECK((c.body[cef::kOffFlags] & cef::kSpaceSelectorMask) == cef::kSpaceSelectorAny);
    uint64_t sym = 0, script = 0;
    uint16_t handle = 0;
    uint32_t param = 0;
    memcpy(&sym, c.body + cef::kOffSymbol, sizeof sym);
    memcpy(&script, c.body + cef::kOffScriptId, sizeof script);
    memcpy(&handle, c.body + cef::kOffHandle, sizeof handle);
    memcpy(&param, c.body + cef::kOffParam, sizeof param);
    CHECK(sym == kLaunchSym);
    CHECK(script == kTubeScript);
    CHECK(handle == kTubeA);
    CHECK(param == 4u);
    // The sender is the engine's own local peer id, LOADED, so the handler stamps the event as
    // locally originated rather than as some peer's.
    CHECK(c.peer == g_self_peer);
    CHECK(c.target == ng.Ptr());

    // And the store still holds the file's bytes: the mask is made on a copy.
    CHECK((componentfeed::PlaybackRecordedEvents().At(0).body[cef::kOffFlags] & cef::kRelayBit) != 0);
}

// ---------------------------------------------------------------------------------------------
// Handle validation
// ---------------------------------------------------------------------------------------------

TEST_CASE("a recorded handle that does not resolve on THIS host refuses the raise") {
    FeedFixture fx;
    FakeNetGame ng;
    PairInStore();

    SUBCASE("index at or past the pool's count") {
        g_pool.count = kTubeA & cef::kHandleIndexMask;   // one short of the recorded index
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().refused_handle == 1);
    }

    SUBCASE("the slot was recycled, so its generation no longer matches the recording's") {
        g_pool.gen[kTubeA & cef::kHandleIndexMask] = 1;   // recorded generation is 0
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().refused_handle == 1);
    }

    SUBCASE("the slot is not live") {
        g_pool.live[kTubeA & cef::kHandleIndexMask] = false;
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().refused_handle == 1);
    }

    SUBCASE("a resolved handle raises") {
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.size() == 1);
        CHECK(componentfeed::Census().refused_handle == 0);
    }
}

namespace {
uint64_t g_target_script = kTubeScript;
bool FakeScriptIdOfActor(void* /*ctx*/, uint64_t /*space*/, uint64_t /*ref*/, uint64_t& out) {
    out = g_target_script;
    return true;
}
bool RefusingScriptIdOfActor(void*, uint64_t, uint64_t, uint64_t&) { return false; }
}  // namespace

TEST_CASE("a target whose script id disagrees with the recorded one refuses the raise") {
    FeedFixture fx;
    FakeNetGame ng;
    PairInStore();
    componentfeed::InjectScriptIdResolverForTest(&FakeScriptIdOfActor, nullptr);

    SUBCASE("agreement raises") {
        g_target_script = kTubeScript;
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.size() == 1);
        CHECK(componentfeed::Census().refused_script_id == 0);
    }

    SUBCASE("a base-index shift that landed on another script refuses") {
        g_target_script = kGametypeScript;
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().refused_script_id == 1);
    }

    SUBCASE("a resolver that cannot answer refuses too -- unknown is not agreement") {
        componentfeed::InjectScriptIdResolverForTest(&RefusingScriptIdOfActor, nullptr);
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().refused_script_id == 1);
    }
    g_target_script = kTubeScript;
}

TEST_CASE("the same handle resolving to a different actor mid-load refuses the second half") {
    FeedFixture fx;
    FakeNetGame ng;
    PairInStore();

    componentfeed::Sweep(ng.Ptr(), 0, false);
    componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
    REQUIRE(g_calls.size() == 1);
    // The pool recycled the slot to a different actor without changing the generation the engine
    // checks -- the one case the engine's own three-part test cannot see.
    g_pool.gen[kTubeA & cef::kHandleIndexMask] = 0;
    const uint16_t idx = kTubeA & cef::kHandleIndexMask;
    g_pool.live[idx] = true;
    struct Swap {
        static void* Resolve(void* out16, uint16_t handle) {
            uint64_t* o = static_cast<uint64_t*>(out16);
            o[0] = 0x5350414345000000ull | (handle & 0xFFFu);
            o[1] = 0xABCDull;   // a different actorRef than FakeResolve gave
            return out16;
        }
    };
    const componentfeed::Handlers swapped{&FakeEntry, &Swap::Resolve, &g_self_peer, nullptr};
    componentfeed::InjectHandlersForTest(&swapped);
    componentfeed::Sweep(ng.Ptr(), 6'000'000, false);
    CHECK(g_calls.size() == 1);
    CHECK(componentfeed::Census().refused_identity == 1);
    componentfeed::InjectHandlersForTest(&kFake);
}

// ---------------------------------------------------------------------------------------------
// The gates
// ---------------------------------------------------------------------------------------------

TEST_CASE("the component feed reaches the engine only when contained and armed") {
    FeedFixture fx;
    FakeNetGame ng;
    PairInStore();

    SUBCASE("a session that is not contained offline playback raises nothing") {
        livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 6'000'000, false);
        CHECK(g_calls.empty());
        CHECK(componentfeed::Census().fires == 0);
    }

    SUBCASE("playback.component_events off raises nothing") {
        componentfeed::Arm(false);
        componentfeed::Sweep(ng.Ptr(), 0, false);
        componentfeed::Sweep(ng.Ptr(), 6'000'000, false);
        CHECK(g_calls.empty());
    }

    SUBCASE("a handler that faults turns the feed off for the session rather than trying again") {
        componentfeed::Sweep(ng.Ptr(), 0, false);
        g_raise_entry = true;
        componentfeed::Sweep(ng.Ptr(), 2'000'000, false);
        CHECK(componentfeed::Census().faulted == 1);
        CHECK_FALSE(componentfeed::Armed());
        g_raise_entry = false;
        componentfeed::Sweep(ng.Ptr(), 6'000'000, false);
        CHECK(g_calls.empty());
    }
}

// =================================================================================================
// THE REAL CAPTURE. RE-LAUNCH-TUBES §Q3 counted 5,429 SR15NetComponentEvent rows in it and
// RE-COMPONENT-EVENT-ENTRY §3.5 admits four of them; this is that claim, decoded by the shipped
// path rather than by the Python mirror the RE used. The capture is NOT in the repo (.gitignore
// keeps recorded sessions out of history), so the case looks in the two places it can be and says
// so when it is in neither, rather than passing quietly.
// =================================================================================================

#include "playback/core/windowbuild.h"
#include "../local_inputs.h"

#define CEVT_WIDEN_(x) L##x
#define CEVT_WIDEN(x) CEVT_WIDEN_(x)
#define CEVT_CAPTURE "2026-09-18_03-33-45_576ED3F8428EBC4B_00000000FFFFFFFF.echodemo"

TEST_CASE("the owner's capture decodes to the RE's row count and the expected allow-listed subset") {
    const std::wstring candidates[] = {
        CEVT_WIDEN(REPLAY_PLAYBACK_FIXTURE_DIR) L"/" CEVT_WIDEN(CEVT_CAPTURE),
        replay_test::CaptureFileW(CEVT_WIDEN(CEVT_CAPTURE)),
    };
    DemoReader reader;
    bool opened = false;
    for (const std::wstring& p : candidates) {
        if (!p.empty() && reader.Open(p.c_str())) { opened = true; break; }
    }
    if (!opened) {
        MESSAGE("SKIPPED: the 2026-09-18 capture is in neither the fixture dir nor the captures folder "
                "(REPLAY_CAPTURE_DIR, else Documents\\EchoReplay)");
        return;
    }

    RecordedEvents s;
    ScanCensus c;
    componentfeed::ScanRecordedEvents(reader, s, c);

    // Every row the tap carries, decoded -- the number the RE's own decode produced.
    CHECK(c.component_rows == 5429u);
    // Four of them, and they are the tube light rig's two launch/return pairs.
    CHECK(s.Count() == 4u);
    CHECK(c.unpaired_rows == 0u);
    for (uint32_t i = 0; i < s.Count(); ++i) {
        CHECK(s.At(i).script_id == kTubeScript);
        CHECK((s.At(i).symbol == kLaunchSym || s.At(i).symbol == kReturnSym));
        CHECK((s.At(i).handle == kTubeA || s.At(i).handle == kTubeB));
    }

    // The three DENY script ids account for the rest, and the gametype's wall is the bulk of it.
    uint32_t gametype = 0;
    for (uint32_t i = 0; i < c.pair_count; ++i) {
        if (c.pairs[i].script_id == kGametypeScript) gametype += c.pairs[i].rows;
        if (c.pairs[i].script_id == kTubeScript && c.pairs[i].symbol == kDenySym) {
            CHECK_FALSE(c.pairs[i].allowed);
        }
    }
    CHECK(gametype > 2000u);

    // WHERE THEY LAND. The playable span is the tick span, not the chapter table's -- this capture
    // is `partial_start` and its first core chapter begins 823 s before its first tick.
    TrackScan scan;
    REQUIRE(ScanTrackSources(reader, scan));
    REQUIRE(scan.have_tick_span);
    const uint32_t inSpan = s.CountInSpan(scan.first_tick_us, scan.last_tick_us);
    MESSAGE("playable span [" << scan.first_tick_us << ".." << scan.last_tick_us
                              << "] us; rows at " << s.At(0).t_us << ", " << s.At(1).t_us << ", "
                              << s.At(2).t_us << ", " << s.At(3).t_us << "; in span " << inSpan);
    // ALL FOUR, and that is the answer to the question the packet asked. The RE's 840.83/841.00
    // reading is on the tap's ARRIVAL axis, which this capture's `partial_start` shape puts ~21 s
    // behind the playhead axis; placed inside their chapters the rows land at 861.90 / 863.58 s
    // (tube 0x23) and 868.07 / 869.75 s (tube 0x1e), i.e. 18.0 / 19.7 / 24.2 / 25.9 s into the
    // 48.5 s a viewer can reach.
    CHECK(inSpan == 4u);
    // And the placement is not merely inside the span, it keeps the recorded SHAPE: the RE measured
    // 1.65 s and 1.66 s between each tube's launch and its return, on the arrival axis.
    const int64_t gapA = int64_t(s.At(1).t_us) - int64_t(s.At(0).t_us);
    const int64_t gapB = int64_t(s.At(3).t_us) - int64_t(s.At(2).t_us);
    CHECK(gapA > 1'500'000);
    CHECK(gapA < 1'800'000);
    CHECK(gapB > 1'500'000);
    CHECK(gapB < 1'800'000);
}
