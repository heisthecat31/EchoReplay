// The HUD netvar writer, driven end to end over synthetic engine memory: a Window
// carrying recorded netvar tracks, the playhead resolve, the publish, and the stage-3 sweep calling
// stand-in setters that write the fake store the way the engine's typed SetValue<T> does (low dword
// for a float or an int, whole qword for a symbol), and stand-in getters that read it back the way
// the engine's do -- a missing row is 0 / 0.0f / ~0, never an error.
//
// The sweep is handed no store pointer by anything here: the stand-ins reach the fake store through
// g_sess, as the engine's accessors reach theirs through the pool. So a case that makes the
// record's store pointer unreadable is the 2026-09-18 launch, and the writer must still run.

#include "doctest.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

#include "bootstrap/log.h"
#include "fake_netvar_session.h"
#include "playback/core/hosthold.h"
#include "playback/core/netvarguard.h"
#include "playback/core/netvarhud.h"
#include "playback/core/window.h"

using namespace replay;
using namespace replay::playback;
using fake_netvar::FakeSession;
namespace guard = replay::playback::netvarguard;

namespace {

constexpr uint64_t kGameTimer = 0xB2996AD484F1D1E8ull;
constexpr uint64_t kCountdown = 0x3E766A9AFE6C97B0ull;
constexpr uint64_t kGameState = 0xB2996AD399FDC0FFull;
constexpr uint64_t kBlueScore = 0x1C97052A21E09F11ull;
constexpr uint64_t kRedScore  = 0x2ED07B1C8C27548Dull;
constexpr uint64_t kOtherVar  = 0x02FE0AA4F2637309ull;   // paused_state: recorded, not a HUD key
constexpr uint64_t kFrisbeeOwner = 0x1B10BDC962B47FCCull;   // game_frisbee_ownerid: the decoder writes it
constexpr uint64_t kStateA    = 0x97D0A383527C012Full;
constexpr uint64_t kStateB    = 0xAB08D245B707B56Bull;
constexpr uint64_t kClockTag  = 0x1CFull << 32;          // DATA-REAL-CAPTURE S5.2

uint64_t RawF(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    return kClockTag | bits;
}
float LowF(uint64_t raw) {
    const uint32_t bits = uint32_t(raw);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

struct SetterCall {
    char     kind;
    void*    target;
    uint32_t handle;
    uint64_t symbol;
    uint64_t value;
    float    lifetime;
};

FakeSession*            g_sess = nullptr;
std::vector<SetterCall> g_calls;

void WriteLowDword(uint64_t symbol, uint32_t low) {
    uint64_t raw = 0;
    g_sess->globalStore.Get(symbol, raw);
    g_sess->globalStore.Set(symbol, (raw & 0xFFFFFFFF00000000ull) | low);
}
void __fastcall FakeSetReal(void* netGame, uint64_t symbol, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    g_calls.push_back({'r', netGame, 0, symbol, bits, 0.0f});
    WriteLowDword(symbol, bits);
}
void __fastcall FakeSetSymbol(void* netGame, uint64_t symbol, uint64_t value, float lifetime) {
    g_calls.push_back({'s', netGame, 0, symbol, value, lifetime});
    g_sess->globalStore.Set(symbol, value);
}
void __fastcall FakeSetInt(void* netVarCs, uint32_t handle, uint64_t symbol, uint32_t value) {
    g_calls.push_back({'i', netVarCs, handle, symbol, value, 0.0f});
    WriteLowDword(symbol, value);
}
void __fastcall FaultingSetReal(void*, uint64_t, float) {
    volatile int* p = nullptr;
    *p = 1;
}

// What a non-authority host's setters do: return, having written nothing.
void __fastcall RefusingSetReal(void* netGame, uint64_t symbol, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    g_calls.push_back({'r', netGame, 0, symbol, bits, 0.0f});
}

struct GetterState {
    size_t reads = 0;
    bool   fault = false;
};
GetterState g_getters;

uint64_t Held(void* netGame, uint64_t symbol, uint64_t missing) {
    ++g_getters.reads;
    CHECK(netGame == g_sess->NetGame());
    if (g_getters.fault) {
        volatile int* p = nullptr;
        *p = 1;
    }
    uint64_t raw = 0;
    return g_sess->globalStore.Get(symbol, raw) ? raw : missing;
}
float FakeGetReal(void* netGame, uint64_t symbol) { return LowF(Held(netGame, symbol, 0)); }
int32_t FakeGetInt(void* netGame, uint64_t symbol) { return int32_t(uint32_t(Held(netGame, symbol, 0))); }
uint64_t* FakeGetSymbol(uint64_t* result, void* netGame, uint64_t symbol) {
    *result = Held(netGame, symbol, ~0ull);
    return result;
}

// The phase flags' pair. The engine's flag setter takes a byte and its getter answers a bool, and
// the store keeps whatever byte was written -- so a writer that compared flags by VALUE instead of
// by truth would see a recorded 2 differ from the held 1 on every sweep.
void __fastcall FakeSetFlag(void* netVarCs, uint32_t handle, uint64_t symbol, uint8_t value) {
    g_calls.push_back({'f', netVarCs, handle, symbol, value, 0.0f});
    WriteLowDword(symbol, value);
}
bool FakeGetBool(void* netGame, uint64_t symbol) { return Held(netGame, symbol, 0) != 0; }

HudNetVarSetters FakeSetters() {
    HudNetVarSetters s;
    s.getReal = &FakeGetReal;
    s.getInt = &FakeGetInt;
    s.getSymbol = &FakeGetSymbol;
    s.getBool = &FakeGetBool;
    s.setReal = &FakeSetReal;
    s.setSymbol = &FakeSetSymbol;
    s.setInt = &FakeSetInt;
    s.setFlag = &FakeSetFlag;
    return s;
}

// The two phase keys, taken from the table rather than spelled again, with their names asserted:
// they were APPENDED at 7 and 8 and a reordering must fail here and not silently test red_score.
constexpr size_t kSuddenDeathIndex = 7;
constexpr size_t kMatchOverIndex   = 8;

size_t CallsFor(uint64_t symbol) {
    size_t n = 0;
    for (const SetterCall& c : g_calls) n += (c.symbol == symbol);
    return n;
}

// The recorded tracks. game_timer runs down to 0 and a round reset jumps it to 120; countdown_timer
// and red_score have NO track in this capture.
void BuildTracks(Window& w, uint16_t slot = FakeSession::kGlobalSlot) {
    const KeyedRecord recs[] = {
        {1000, kGameTimer, RawF(100.0f)}, {1000, kGameState, kStateA}, {1000, kOtherVar, 0x11},  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        {2000, kGameTimer, RawF(99.0f)},  {2500, kBlueScore, kClockTag | 2u},
        {3000, kGameTimer, RawF(0.0f)},   {3500, kGameState, kStateB},
        {4000, kGameTimer, RawF(120.0f)}, {5000, kGameTimer, RawF(119.0f)},
    };
    REQUIRE(w.NetVars().Build(nullptr, 0, recs, sizeof recs / sizeof recs[0]));
    for (uint64_t k : {kGameTimer, kGameState, kBlueScore, kOtherVar}) w.NoteNetVarSlot(k, slot);
}

struct Fixture {
    FakeSession sess;
    Window w;
    uint64_t now_ms = 10'000;

    Fixture() {
        HudNetVarsResetForTest();
        guard::ResetForTest();
        g_sess = &sess;
        g_calls.clear();
        g_getters = GetterState{};
        BuildTracks(w);
        // The live host's own values, which playback has to displace.
        sess.globalStore.Set(kGameTimer, RawF(300.0f));
        sess.globalStore.Set(kCountdown, RawF(5.0f));
        sess.globalStore.Set(kGameState, 0x1111);
        sess.globalStore.Set(kBlueScore, kClockTag | 9u);
        sess.globalStore.Set(kRedScore, kClockTag | 9u);
        sess.SetRulesClock(42.0f);
    }
    ~Fixture() {
        HudNetVarsResetForTest();
        guard::ResetForTest();
        g_sess = nullptr;
    }

    void Arm(bool clock = true, bool score = true, bool contained = true, bool host_filter = false) {
        HudNetVarsArm(contained, clock, score, host_filter, FakeSetters());
    }
    // The setter detours standing in front of the same stand-in setters the writer calls, as the
    // armed trampolines stand in front of the engine's.
    void InstallGuard() {
        guard::Setters originals;
        originals.setInt = &FakeSetInt;
        originals.setFlag = &FakeSetFlag;
        originals.setRealNetGame = &FakeSetReal;
        originals.setSymbolNetGame = &FakeSetSymbol;
        guard::SetOriginalsForTest(originals);
    }
    // The arena script's publish block, entering through the detours like any engine caller.
    void HostScriptPublishes(float timer) {
        guard::DetoursForTest().setRealNetGame(sess.NetGame(), kGameTimer, timer);
        guard::DetoursForTest().setSymbolNetGame(sess.NetGame(), kGameState, 0x1111, INFINITY);
        guard::DetoursForTest().setRealNetGame(sess.NetGame(), kOtherVar, 7.0f);
    }
    // One frame: the tick resolves at T and publishes, then the stage-3 tail sweeps.
    void Frame(int64_t t_us) {
        HudNetVarFrame hud;
        ResolveHudNetVars(w, t_us, hud);
        HudNetVarsPublish(hud);
        now_ms += 16;
        HudNetVarsSweep(sess.NetGame(), now_ms);
    }
    float Timer() {
        uint64_t raw = 0;
        REQUIRE(sess.globalStore.Get(kGameTimer, raw));
        return LowF(raw);
    }
    uint64_t Raw(uint64_t key) {
        uint64_t raw = 0;
        REQUIRE(sess.globalStore.Get(key, raw));
        return raw;
    }
};

// Everything log::Direct wrote while this lives.
struct LogCapture {
    std::wstring path;

    LogCapture() {
        wchar_t dir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, dir);
        path = std::wstring(dir) + L"netvarhud_selftest_" + std::to_wstring(GetCurrentProcessId()) +
               L".log";
        DeleteFileW(path.c_str());
        log::RedirectForTest(path.c_str());
        log::SetLevel(LogLevel::Info);
    }
    ~LogCapture() {
        log::EndRedirectForTest();
        DeleteFileW(path.c_str());
    }
    std::string Text() const {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }
    static size_t Count(const std::string& text, const std::string& needle) {
        size_t n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    }
};

}  // namespace

// The 2026-09-18 launches. The pool's slot map names the handle's record, the typed accessors work,
// and the one thing the writer used to demand -- a store pointer it could read itself -- is absent.
TEST_CASE("netvarhud: the writer runs on a valid route whose store is NOT readable") {
    Fixture f;
    f.sess.SetStoreOf(FakeSession::kGlobalRecord, reinterpret_cast<void*>(0xDEAD0000DEAD0000ull));
    const NetVarSite site = LocateNetVarSite(f.sess.NetGame());
    REQUIRE(site.RouteOk());
    REQUIRE_FALSE(site.StoreOk());

    LogCapture captured;
    f.Arm();
    f.Frame(2600);

    CHECK(HudNetVarsStats().route_invalid == 0);
    CHECK(f.Timer() == 99.0f);
    CHECK(f.Raw(kGameState) == kStateA);
    CHECK(uint32_t(f.Raw(kBlueScore)) == 2u);
    CHECK(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));
    CHECK(HudNetVarsApplying());

    const std::string text = captured.Text();
    CHECK(LogCapture::Count(text, "hud netvars: route valid at netgame state 9") == 1);
    CHECK(text.find("store DEAD0000DEAD0000 [the record's store pointer is null or unreadable]") !=
          std::string::npos);
    CHECK(text.find("role word unread") != std::string::npos);
}

TEST_CASE("netvarhud: the route-valid line carries the role word the setters' gate tests") {
    Fixture f;
    uint64_t roleWord = 0x42;   // bit 1: authority
    FakeSession::Put(f.sess.netGame, facts::NETGAME_ROLEFLAGS_OFF, static_cast<void*>(&roleWord));
    {
        LogCapture captured;
        f.Arm();
        f.Frame(2600);
        CHECK(captured.Text().find("role word 0x42 = authority;") != std::string::npos);
    }
    roleWord = 0x04;   // a client
    {
        LogCapture captured;
        f.Arm();
        f.Frame(2600);
        CHECK(captured.Text().find("role word 0x4 = NOT authority") != std::string::npos);
    }
}

TEST_CASE("netvarhud: each route refusal is logged once, with its own reason and raw values") {
    Fixture f;
    LogCapture captured;
    f.Arm();

    f.sess.slotMap[FakeSession::kGlobalSlot][0] = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    f.Frame(2600);
    f.Frame(2600);
    f.sess.slotMap[FakeSession::kGlobalSlot][0] = FakeSession::kGlobalRecord;
    f.sess.slotMap[FakeSession::kGlobalSlot][1] = 9;   // the slot was recycled
    f.Frame(2600);
    f.Frame(2600);
    CHECK(g_calls.empty());
    CHECK(g_getters.reads == 0);
    CHECK(HudNetVarsStats().route_invalid == 4);
    CHECK_FALSE(HudNetVarsApplying());

    f.sess.slotMap[FakeSession::kGlobalSlot][1] = FakeSession::kGlobalGeneration;
    f.Frame(2600);
    CHECK(f.Timer() == 99.0f);
    f.sess.SetRecordCount(0);
    f.Frame(2600);

    const std::string text = captured.Text();
    CHECK(LogCapture::Count(text, "hud netvars: route not valid at netgame state 9") == 2);
    CHECK(LogCapture::Count(text, "the slot map holds 0xFFFF for the slot") == 1);
    CHECK(LogCapture::Count(text, "index=65535") == 1);
    CHECK(LogCapture::Count(text, "the handle's slot was recycled") == 1);
    CHECK(LogCapture::Count(text, "index=2 generation=9") == 1);
    CHECK(LogCapture::Count(text, "hud netvars: route lost at netgame state 9") == 1);
    CHECK(LogCapture::Count(text, "the slot's record index is not below the record count") == 1);
    CHECK(LogCapture::Count(text, "count=0") == 1);
    CHECK(text.find("cannot be walked yet") == std::string::npos);
}

TEST_CASE("netvarhud: the getter read-back decides each write -- one call when it differs, none when it holds") {
    Fixture f;
    f.Arm();
    f.Frame(2600);
    const size_t first = g_calls.size();
    REQUIRE(first == 3);   // game_timer, game_state, blue_score: all differed from the host's values
    // One read to decide and one to confirm, per written key.
    CHECK(g_getters.reads == 6);

    f.Frame(2600);
    CHECK(g_calls.size() == first);
    CHECK(g_getters.reads == 9);
    CHECK(HudNetVarsStats().unchanged == 3);

    // The host moves ONE key: the getter sees it, and only that key is written again.
    f.sess.globalStore.Set(kBlueScore, kClockTag | 7u);
    f.Frame(2600);
    REQUIRE(g_calls.size() == first + 1);
    CHECK(g_calls.back().symbol == kBlueScore);
    CHECK(uint32_t(f.Raw(kBlueScore)) == 2u);
}

// PERF-3. The sweep runs about four times a rendered frame and each engine getter spins on the
// record's lock, so "one getter per key per sweep" was 2.3 ms of every frame for an answer that had
// not changed. With the host-netvar filter on -- the shipping default, which swallows the host's
// writes to a key we own -- a key whose recorded value has not moved is answered from the memo, and
// the engine is asked again once a second.
TEST_CASE("netvarhud: with the host filter on, a key that has not moved costs no engine getter") {
    Fixture f;
    f.Arm(true, true, true, /*host_filter=*/true);
    f.Frame(2600);
    const size_t writes = g_calls.size();
    const uint32_t reads_after_first = g_getters.reads;
    REQUIRE(writes == 3);

    // Sixty more sweeps inside the verification window: the timer is re-read every sweep (its
    // recorded value moves, and its read-back is the overwritten= measurement) and nothing else is.
    for (int i = 0; i < 60; ++i) f.Frame(2600);
    CHECK(g_calls.size() == writes);
    CHECK(g_getters.reads == reads_after_first + 60);
    CHECK(HudNetVarsStats().memoed == 120);   // game_state and blue_score, 60 sweeps each

    // A seek drops the memos: the next sweep asks the engine again for every key.
    HudNetVarsInvalidate(true);
    const uint32_t before = g_getters.reads;
    f.Frame(2600);
    CHECK(g_getters.reads > before + 1);
}

TEST_CASE("netvarhud: with the host filter OFF the read-back still runs on every sweep") {
    Fixture f;
    f.Arm(true, true, true, /*host_filter=*/false);
    f.Frame(2600);
    const size_t writes = g_calls.size();
    f.Frame(2600);
    CHECK(HudNetVarsStats().memoed == 0);

    // The host can reach the record when its writes are not swallowed, and the getter is the only
    // thing that would notice. It must still notice on the very next sweep.
    f.sess.globalStore.Set(kBlueScore, kClockTag | 7u);
    f.Frame(2600);
    REQUIRE(g_calls.size() == writes + 1);
    CHECK(g_calls.back().symbol == kBlueScore);
}

TEST_CASE("netvarhud: a setter whose write does not read back is named once and that key stops") {
    Fixture f;
    LogCapture captured;
    HudNetVarSetters s = FakeSetters();
    s.setReal = &RefusingSetReal;
    HudNetVarsArm(true, true, true, false, s);
    for (int i = 0; i < 5; ++i) f.Frame(2600);

    CHECK(CallsFor(kGameTimer) == 1);
    CHECK(f.Timer() == 300.0f);
    CHECK(HudNetVarsStats().setter_refused == 1);
    CHECK(HudNetVarsStats().faulted == 0);
    CHECK_FALSE(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));
    // The other kinds' setters landed and keep being driven.
    CHECK(f.Raw(kGameState) == kStateA);
    CHECK(HudNetVarsOwns(kGameState, FakeSession::kGlobalSlot));
    CHECK(HudNetVarsApplying());

    const std::string text = captured.Text();
    CHECK(LogCapture::Count(text, "hud netvar game_timer: the setter was REFUSED -- wrote 99.000, "
                                  "the getter reads back 300.000") == 1);
    CHECK(LogCapture::Count(text, "REFUSED") == 1);

    // A new load asks again.
    HudNetVarsArm(true, true, true, false, FakeSetters());
    f.Frame(2600);
    CHECK(f.Timer() == 99.0f);
}

TEST_CASE("netvarhud: a kind whose getter did not bind is not written and stays with the decoder") {
    Fixture f;
    HudNetVarSetters s = FakeSetters();
    s.getReal = nullptr;
    HudNetVarsArm(true, true, true, false, s);
    f.Frame(2600);
    CHECK(CallsFor(kGameTimer) == 0);
    CHECK(f.Timer() == 300.0f);
    CHECK_FALSE(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));
    CHECK(f.Raw(kGameState) == kStateA);
    CHECK(HudNetVarsOwns(kGameState, FakeSession::kGlobalSlot));
}

TEST_CASE("netvarhud: a getter that raises is contained and turns the writer off") {
    Fixture f;
    f.Arm();
    g_getters.fault = true;
    f.Frame(2600);
    f.Frame(2700);
    CHECK(g_calls.empty());
    CHECK(HudNetVarsStats().faulted == 1);
    CHECK(HudNetVarsStats().sweeps == 1);
}

TEST_CASE("netvarhud: a tracked key is written through its typed setter with the value at T") {
    Fixture f;
    f.Arm();
    f.Frame(2600);

    REQUIRE(kHudVars[kHudGameTimerIndex].symbol == kGameTimer);   // the measured key
    CHECK(f.Timer() == 99.0f);
    CHECK(f.Raw(kGameState) == kStateA);
    CHECK(uint32_t(f.Raw(kBlueScore)) == 2u);

    REQUIRE(g_calls.size() == 3);
    for (const SetterCall& c : g_calls) {
        if (c.symbol == kGameTimer) {
            CHECK(c.kind == 'r');
            CHECK(c.target == f.sess.NetGame());
        } else if (c.symbol == kGameState) {
            CHECK(c.kind == 's');
            CHECK(c.target == f.sess.NetGame());
            CHECK(isinf(c.lifetime));   // what the engine's own callers pass
        } else {
            REQUIRE(c.symbol == kBlueScore);
            CHECK(c.kind == 'i');
            // SetInt is the handle form: the validated component and the WHOLE cached handle.
            CHECK(c.target == static_cast<void*>(f.sess.cs));
            CHECK(c.handle == (uint32_t(FakeSession::kGlobalSlot) | (5u << 16)));
        }
    }
}

TEST_CASE("netvarhud: forward, a backward seek, and pause -- the value is a function of T alone") {
    Fixture f;
    f.Arm();

    f.Frame(1500);
    CHECK(f.Timer() == 100.0f);
    f.Frame(2100);
    CHECK(f.Timer() == 99.0f);
    f.Frame(5200);
    CHECK(f.Timer() == 119.0f);
    CHECK(f.Raw(kGameState) == kStateB);

    f.w.InvalidateCursors();   // a seek, as the module does on any discontinuity
    f.Frame(1200);
    CHECK(f.Timer() == 100.0f);
    CHECK(f.Raw(kGameState) == kStateA);

    // Reverse play: no discontinuity is fired, T just decreases.
    f.Frame(5100);
    f.Frame(4500);
    CHECK(f.Timer() == 120.0f);
    f.Frame(3100);
    CHECK(f.Timer() == 0.0f);

    // Paused: the same T, frame after frame. The store already holds the value, so no setter runs.
    const size_t before = g_calls.size();
    const uint32_t unchangedBefore = HudNetVarsStats().unchanged;
    for (int i = 0; i < 5; ++i) f.Frame(3100);
    CHECK(g_calls.size() == before);
    CHECK(HudNetVarsStats().unchanged > unchangedBefore);
    CHECK(f.Timer() == 0.0f);
}

TEST_CASE("netvarhud: a round-reset discontinuity is a step, never interpolated") {
    Fixture f;
    f.Arm();
    // Samples 0.0 @3000 and 120.0 @4000. Anything between them is the EARLIER sample.
    for (int64_t t : {3001, 3500, 3999}) {
        f.Frame(t);
        CHECK(f.Timer() == 0.0f);
    }
    f.Frame(4000);
    CHECK(f.Timer() == 120.0f);
}

TEST_CASE("netvarhud: a key with no track is never written") {
    Fixture f;
    f.Arm();
    for (int64_t t : {500, 1500, 2600, 5200}) f.Frame(t);

    CHECK(CallsFor(kCountdown) == 0);
    CHECK(CallsFor(kRedScore) == 0);
    CHECK(f.Raw(kCountdown) == RawF(5.0f));
    CHECK(uint32_t(f.Raw(kRedScore)) == 9u);
    CHECK(CallsFor(kOtherVar) == 0);   // recorded, but not a HUD key: the decoder path owns it
}

TEST_CASE("netvarhud: before a key's first row the first row's value is written, not the host's") {
    Fixture f;
    f.Arm();
    f.Frame(500);    // game_timer and game_state open at 1000, blue_score at 2500
    CHECK(f.Timer() == 100.0f);
    CHECK(f.Raw(kGameState) == kStateA);
    CHECK(uint32_t(f.Raw(kBlueScore)) == 2u);

    f.Frame(2100);   // and from its first row on, the value at T as before
    CHECK(f.Timer() == 99.0f);
}

TEST_CASE("netvarhud: a window with no row for a key keeps asserting the last value") {
    Fixture f;
    f.Arm();
    f.Frame(2100);
    REQUIRE(f.Timer() == 99.0f);

    // The seek that went quiet for 7 s: the new window carries nothing for the HUD keys.
    Window empty;
    const KeyedRecord other[] = {{9000, kOtherVar, 0x22}};
    REQUIRE(empty.NetVars().Build(nullptr, 0, other, 1));
    empty.NoteNetVarSlot(kOtherVar, FakeSession::kGlobalSlot);
    const size_t before = CallsFor(kGameTimer);
    for (int i = 0; i < 3; ++i) {
        f.sess.globalStore.Set(kGameTimer, RawF(300.0f));   // the host takes its chance
        HudNetVarFrame hud;
        ResolveHudNetVars(empty, 9500, hud);
        CHECK_FALSE(hud.v[kHudGameTimerIndex].due);
        HudNetVarsPublish(hud);
        HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);
        CHECK(f.Timer() == 99.0f);
    }
    CHECK(CallsFor(kGameTimer) == before + 3);
    CHECK(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));

    // Held, and still write-only-when-different.
    HudNetVarFrame hud;
    ResolveHudNetVars(empty, 9500, hud);
    HudNetVarsPublish(hud);
    HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);
    CHECK(CallsFor(kGameTimer) == before + 3);

    // A new load starts from nothing held.
    f.Arm();
    f.sess.globalStore.Set(kGameTimer, RawF(300.0f));
    ResolveHudNetVars(empty, 9500, hud);
    HudNetVarsPublish(hud);
    HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);
    CHECK(f.Timer() == 300.0f);
}

TEST_CASE("netvarhud: nothing runs with no replay loaded, uncontained, or with both flags off") {
    Fixture f;
    HudNetVarFrame hud;
    ResolveHudNetVars(f.w, 2600, hud);
    HudNetVarsPublish(hud);

    HudNetVarsSweep(f.sess.NetGame(), 1);   // never armed: no replay loaded
    f.Arm(true, true, /*contained=*/false);
    HudNetVarsPublish(hud);
    HudNetVarsSweep(f.sess.NetGame(), 2);
    f.Arm(false, false, true);
    HudNetVarsPublish(hud);
    HudNetVarsSweep(f.sess.NetGame(), 3);

    CHECK(g_calls.empty());
    CHECK(HudNetVarsStats().sweeps == 0);
    CHECK(f.Timer() == 300.0f);

    // Armed, then unloaded: the published frame is dropped with the arm.
    f.Arm();
    f.Frame(2600);
    REQUIRE(f.Timer() == 99.0f);
    HudNetVarsDisarm();
    f.sess.globalStore.Set(kGameTimer, RawF(300.0f));
    HudNetVarsSweep(f.sess.NetGame(), 4);
    CHECK(f.Timer() == 300.0f);
}

TEST_CASE("netvarhud: each flag gates its own group") {
    Fixture f;
    f.Arm(/*clock=*/true, /*score=*/false);
    f.Frame(2600);
    CHECK(f.Timer() == 99.0f);
    CHECK(CallsFor(kBlueScore) == 0);
    // An off group's keys are withheld from the decoder path too: the flag means nobody drives them.
    CHECK(HudNetVarsOwns(kBlueScore, FakeSession::kGlobalSlot));
    CHECK(HudNetVarsOwns(kBlueScore, FakeSession::kOtherSlot));
    CHECK_FALSE(HudNetVarsOwns(kOtherVar, FakeSession::kGlobalSlot));

    Fixture g;
    g.Arm(/*clock=*/false, /*score=*/true);
    g.Frame(2600);
    CHECK(g.Timer() == 300.0f);
    CHECK(uint32_t(g.Raw(kBlueScore)) == 2u);
}

TEST_CASE("netvarhud: no write while the route does not validate, and writes resume when it does") {
    Fixture f;
    f.Arm();
    f.sess.SetLevelSpace(f.sess.nextLevelSpace);   // the netGame left the component's space
    f.Frame(2600);
    f.Frame(2700);
    CHECK(g_calls.empty());
    CHECK(HudNetVarsStats().route_invalid == 2);
    CHECK_FALSE(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));   // decoder keeps it meanwhile

    f.sess.SetComponentSpace(f.sess.nextLevelSpace);
    f.Frame(2800);
    CHECK(f.Timer() == 99.0f);
    CHECK(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));
}

TEST_CASE("netvarhud: a key recorded in another slot is refused -- (slot, symbol), not symbol") {
    Fixture f;
    Window other;
    BuildTracks(other, FakeSession::kOtherSlot);
    f.Arm();
    HudNetVarFrame hud;
    ResolveHudNetVars(other, 2600, hud);
    HudNetVarsPublish(hud);
    HudNetVarsSweep(f.sess.NetGame(), 1);

    CHECK(g_calls.empty());
    CHECK(HudNetVarsStats().slot_refused == 3);
    CHECK(f.Timer() == 300.0f);
    CHECK_FALSE(HudNetVarsOwns(kGameTimer, FakeSession::kOtherSlot));
}

TEST_CASE("netvarhud: the measurement counts the frames a host script overwrote game_timer") {
    Fixture f;
    f.Arm();
    f.now_ms = 50'000;
    f.Frame(2100);   // first write: nothing to compare against yet

    // 10 frames paused at T. On 4 of them "the script" republishes its own countdown after us.
    for (int i = 0; i < 10; ++i) {
        if (i % 3 == 0) f.sess.globalStore.Set(kGameTimer, RawF(250.0f - float(i)));
        f.Frame(2100);
    }
    char line[192] = {};
    CHECK_FALSE(HudNetVarsLastMeasureLine(line, sizeof line));   // < 1 s so far

    f.now_ms += 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    f.Frame(2100);
    REQUIRE(HudNetVarsLastMeasureLine(line, sizeof line));
    CHECK(std::string(line) ==
          "hud netvar game_timer: wrote=99.000 found_next_frame=99.000 overwritten=4/11 frames "
          "writes=5 (last 1 s)");

    // An undisturbed second reads 0 overwritten and no writes at all.
    for (int i = 0; i < 5; ++i) f.Frame(2100);
    f.now_ms += 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    f.Frame(2100);
    REQUIRE(HudNetVarsLastMeasureLine(line, sizeof line));
    CHECK(std::string(line) ==
          "hud netvar game_timer: wrote=99.000 found_next_frame=99.000 overwritten=0/6 frames "
          "writes=0 (last 1 s)");
}

TEST_CASE("netvarhud: a new load starts the measurement fresh") {
    Fixture f;
    f.Arm();
    f.now_ms = 70'000;
    f.Frame(2100);

    // Unload, the level moves the value on, load again: that is not the script overwriting us.
    HudNetVarsDisarm();
    f.sess.globalStore.Set(kGameTimer, RawF(300.0f));
    f.Arm();
    f.Frame(2100);
    f.now_ms += 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    f.Frame(2100);

    char line[192] = {};
    REQUIRE(HudNetVarsLastMeasureLine(line, sizeof line));
    CHECK(std::string(line) ==
          "hud netvar game_timer: wrote=99.000 found_next_frame=99.000 overwritten=0/1 frames "
          "writes=1 (last 1 s)");
}

TEST_CASE("netvarhud: a setter that raises is contained and turns the writer off") {
    Fixture f;
    HudNetVarSetters s = FakeSetters();
    s.setReal = &FaultingSetReal;
    HudNetVarsArm(true, true, true, false, s);
    f.Frame(2600);
    f.Frame(2700);

    CHECK(HudNetVarsStats().faulted == 1);
    CHECK(HudNetVarsStats().sweeps == 1);
    CHECK_FALSE(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));
}

TEST_CASE("netvarhud: rules+0xBC is never written, and its writer no longer exists in the source") {
    Fixture f;
    f.Arm();
    for (int64_t t : {1500, 2600, 3100, 4100, 5200}) f.Frame(t);
    REQUIRE(HudNetVarsStats().writes > 0);
    CHECK(f.sess.RulesClock() == 42.0f);

    // The retired surface, spelled in halves so this file never matches its own scan.
    const std::string dead[] = {std::string("WriteGameState") + "ClockOn",
                                std::string("ArmGameState") + "ClockWriteback",
                                std::string("PublishGameState") + "Clock",
                                std::string("kMatch") + "ClockOff"};
    const char* files[] = {"/src/engine/identity_access.cpp", "/src/engine/identity_access.h",
                           "/src/playback/core/playback_module.cpp"};
    for (const char* rel : files) {
        std::ifstream in(std::string(REPLAY_SOURCE_DIR) + rel, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), rel);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string text = buf.str();
        REQUIRE(text.size() > 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        for (const std::string& name : dead) {
            CHECK_MESSAGE(text.find(name) == std::string::npos, rel << " still names " << name);
        }
    }
}

// ---- HOST-NV: the host's own writers against the writer ---------------------------------------

TEST_CASE("netvarhud: with the filter on, the script's republish is swallowed and overwritten reads 0") {
    Fixture f;
    LogCapture captured;
    f.InstallGuard();
    f.Arm(true, true, true, /*host_filter=*/true);
    f.now_ms = 50'000;
    f.Frame(2100);
    CHECK(guard::CurrentMode() == guard::Mode::Filter);

    for (int i = 0; i < 10; ++i) {
        f.HostScriptPublishes(250.0f - float(i));
        CHECK(f.Timer() == 99.0f);              // what the HUD reads between the two passes
        CHECK(f.Raw(kGameState) == kStateA);
        f.Frame(2100);
    }
    uint64_t other = 0;
    REQUIRE(f.sess.globalStore.Get(kOtherVar, other));
    CHECK(LowF(other) == 7.0f);                 // the script's own netvar still lands

    f.now_ms += 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    f.Frame(2100);
    char line[512] = {};
    REQUIRE(HudNetVarsLastMeasureLine(line, sizeof line));
    CHECK(std::string(line) ==
          "hud netvar game_timer: wrote=99.000 found_next_frame=99.000 overwritten=0/11 frames "
          "writes=1 (last 1 s)");
    REQUIRE(HudNetVarsLastHostWriteLine(line, sizeof line));
    CHECK(std::string(line).find(
              "hud netvar host writes (filter): game_timer swallowed=10 passed=0; game_state "
              "swallowed=10 passed=0; blue_score swallowed=0 passed=0; slot 1 |") == 0);
    // PERF-3's counters ride the same line. The memo is what makes the sweep affordable, so a run
    // with the filter on must show it answering.
    CHECK(std::string(line).find("memoed=0 ") == std::string::npos);
    CHECK(LogCapture::Count(captured.Text(), "hud netvar host writes (filter):") == 1);
}

TEST_CASE("netvarhud: with the filter key off the same writes pass and are counted") {
    Fixture f;
    f.InstallGuard();
    f.Arm();
    f.now_ms = 50'000;
    f.Frame(2100);
    CHECK(guard::CurrentMode() == guard::Mode::Observe);

    for (int i = 0; i < 4; ++i) {
        f.HostScriptPublishes(250.0f);
        CHECK(f.Timer() == 250.0f);
        f.Frame(2100);
    }
    f.now_ms += 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    f.Frame(2100);
    char line[512] = {};
    REQUIRE(HudNetVarsLastMeasureLine(line, sizeof line));
    CHECK(std::string(line).find("overwritten=4/5 frames") != std::string::npos);
    REQUIRE(HudNetVarsLastHostWriteLine(line, sizeof line));
    CHECK(std::string(line).find(
              "hud netvar host writes (observe): game_timer swallowed=0 passed=4; game_state "
              "swallowed=0 passed=4; blue_score swallowed=0 passed=0; slot 1 |") == 0);
    // With the filter off the host can reach the record, so nothing may be answered from a memo.
    CHECK(std::string(line).find("memoed=0 (last 1 s)") != std::string::npos);
}

TEST_CASE("netvarhud: the filter is pass-through with no replay loaded, outside a match, and after unload") {
    Fixture f;
    f.InstallGuard();

    f.Frame(2100);   // never armed: no replay
    CHECK(guard::CurrentMode() == guard::Mode::PassThrough);
    f.HostScriptPublishes(250.0f);
    CHECK(f.Timer() == 250.0f);

    f.Arm(true, true, true, true);
    f.Frame(2100);
    REQUIRE(guard::CurrentMode() == guard::Mode::Filter);

    HudNetVarsIdle();   // the stage-3 tail while the session is not a running match
    CHECK(guard::CurrentMode() == guard::Mode::PassThrough);
    f.HostScriptPublishes(251.0f);
    CHECK(f.Timer() == 251.0f);

    f.Frame(2100);
    REQUIRE(guard::CurrentMode() == guard::Mode::Filter);
    f.sess.SetLevelSpace(f.sess.nextLevelSpace);   // the route stops validating: a level change
    f.Frame(2100);
    CHECK(guard::CurrentMode() == guard::Mode::PassThrough);
    f.sess.SetComponentSpace(f.sess.nextLevelSpace);

    f.Frame(2100);
    REQUIRE(guard::CurrentMode() == guard::Mode::Filter);
    HudNetVarsDisarm();
    CHECK(guard::CurrentMode() == guard::Mode::PassThrough);
    f.Frame(2100);   // and a sweep after the unload does not raise it again
    CHECK(guard::CurrentMode() == guard::Mode::PassThrough);
}

TEST_CASE("netvarhud: only the keys the writer is holding are owned -- a flagged-off group and a refused slot stay the host's") {
    Fixture f;
    f.InstallGuard();
    f.Arm(/*clock=*/true, /*score=*/false, true, /*host_filter=*/true);
    f.Frame(2600);
    guard::DetoursForTest().setInt(f.sess.cs, FakeSession::kGlobalSlot | (uint32_t{FakeSession::kGlobalGeneration} << 16),
                                   kBlueScore, 5);
    CHECK(uint32_t(f.Raw(kBlueScore)) == 5u);   // score group off: nobody here writes it
    guard::DetoursForTest().setRealNetGame(f.sess.NetGame(), kGameTimer, 250.0f);
    CHECK(f.Timer() == 99.0f);

    // Recorded in a slot that is not the global handle's: the writer refuses, so the host keeps it.
    Fixture g;
    g.InstallGuard();
    Window elsewhere;
    BuildTracks(elsewhere, FakeSession::kGlobalSlot + 1);
    g.Arm(true, true, true, true);
    HudNetVarFrame hud;
    ResolveHudNetVars(elsewhere, 2100, hud);
    HudNetVarsPublish(hud);
    HudNetVarsSweep(g.sess.NetGame(), g.now_ms += 16);
    CHECK(HudNetVarsStats().slot_refused > 0);
    guard::DetoursForTest().setRealNetGame(g.sess.NetGame(), kGameTimer, 250.0f);
    CHECK(g.Timer() == 250.0f);
}

TEST_CASE("netvarhud: the frisbee owner is owned while the decoder drives it, and only then") {
    Fixture f;
    f.InstallGuard();
    const KeyedRecord recs[] = {{1000, kGameTimer, RawF(100.0f)}, {1000, kFrisbeeOwner, 4}};  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    Window w;
    REQUIRE(w.NetVars().Build(nullptr, 0, recs, 2));
    w.NoteNetVarSlot(kGameTimer, FakeSession::kGlobalSlot);
    w.NoteNetVarSlot(kFrisbeeOwner, FakeSession::kGlobalSlot);
    f.Arm(true, true, true, true);

    auto frame = [&](bool decoder_bound) {
        HudNetVarFrame hud;
        ResolveHudNetVars(w, 1500, hud);
        REQUIRE(hud.frisbee_owner.due);
        hud.frisbee_owner.due = decoder_bound;
        HudNetVarsPublish(hud);
        HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);
    };
    f.sess.globalStore.Set(kFrisbeeOwner, 4);   // what the decoder left

    frame(true);
    guard::DetoursForTest().setSymbolNetGame(f.sess.NetGame(), kFrisbeeOwner, 0xFFFFFFFFull, INFINITY);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    CHECK(f.Raw(kFrisbeeOwner) == 4);
    CHECK(CallsFor(kFrisbeeOwner) == 0);   // the writer itself never writes it

    frame(false);
    guard::DetoursForTest().setSymbolNetGame(f.sess.NetGame(), kFrisbeeOwner, 0xFFFFFFFFull, INFINITY);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    CHECK(f.Raw(kFrisbeeOwner) == 0xFFFFFFFFull);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

    // Recorded in a component that is not the global handle's: the decoder writes it THERE, so the
    // host's write to the global one is not ours to drop.
    Window elsewhere;
    REQUIRE(elsewhere.NetVars().Build(nullptr, 0, recs, 2));
    elsewhere.NoteNetVarSlot(kGameTimer, FakeSession::kGlobalSlot);
    elsewhere.NoteNetVarSlot(kFrisbeeOwner, FakeSession::kGlobalSlot + 1);
    HudNetVarFrame hud;
    ResolveHudNetVars(elsewhere, 1500, hud);
    REQUIRE(hud.frisbee_owner.due);
    HudNetVarsPublish(hud);
    HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);
    guard::DetoursForTest().setSymbolNetGame(f.sess.NetGame(), kFrisbeeOwner, 7, INFINITY);
    CHECK(f.Raw(kFrisbeeOwner) == 7);
}

TEST_CASE("netvarhud: a key whose setter is refused goes back to the host") {
    Fixture f;
    f.InstallGuard();
    HudNetVarSetters s = FakeSetters();
    s.setReal = &RefusingSetReal;
    HudNetVarsArm(true, true, true, true, s);
    f.Frame(2100);   // refused here
    f.Frame(2100);
    guard::DetoursForTest().setRealNetGame(f.sess.NetGame(), kGameTimer, 250.0f);
    CHECK(f.Timer() == 250.0f);
    guard::DetoursForTest().setSymbolNetGame(f.sess.NetGame(), kGameState, 0x1111, INFINITY);
    CHECK(f.Raw(kGameState) == kStateA);   // the keys that do land stay ours
}

TEST_CASE("netvarhud: the phase flags are published through setFlag and compared by TRUTH") {
    REQUIRE(strcmp(kHudVars[kSuddenDeathIndex].name, "sudden_death_active") == 0);
    REQUIRE(strcmp(kHudVars[kMatchOverIndex].name, "match_is_over") == 0);
    REQUIRE(kHudVars[kSuddenDeathIndex].kind == HudVarKind::Flag);
    REQUIRE(kHudVars[kMatchOverIndex].kind == HudVarKind::Flag);
    const uint64_t sd = kHudVars[kSuddenDeathIndex].symbol;
    const uint64_t mo = kHudVars[kMatchOverIndex].symbol;

    Fixture f;
    // A recorder writes the engine's byte, and the engine's byte for a set flag is not always 1.
    const KeyedRecord recs[] = {{1000, kGameTimer, RawF(100.0f)}, {1000, sd, 2}, {1000, mo, 0},  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
                                {3000, sd, 0}, {3000, mo, 1}};
    Window w;
    REQUIRE(w.NetVars().Build(nullptr, 0, recs, sizeof recs / sizeof recs[0]));
    for (uint64_t k : {kGameTimer, sd, mo}) w.NoteNetVarSlot(k, FakeSession::kGlobalSlot);
    f.sess.globalStore.Set(sd, 0);
    f.sess.globalStore.Set(mo, 0);
    f.Arm();

    auto frame = [&](int64_t t) {
        HudNetVarFrame hud;
        ResolveHudNetVars(w, t, hud);
        HudNetVarsPublish(hud);
        HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);
    };

    frame(2000);   // the load's forced sweep asserts every due key, equal or not
    CHECK(CallsFor(sd) == 1);
    CHECK(g_calls.back().kind == 'f');           // through setFlag, not setInt
    CHECK(f.Raw(sd) != 0);                       // set
    CHECK(f.Raw(mo) == 0);
    const size_t sd_asserted = CallsFor(sd), mo_asserted = CallsFor(mo);
    CHECK(HudNetVarsOwns(sd, FakeSession::kGlobalSlot));

    // The row still says 2 and the engine now holds 1. Same state, so neither key is rewritten --
    // this is the assertion a value comparison fails, once per sweep, for the rest of the file.
    frame(2500);
    frame(2900);
    CHECK(CallsFor(sd) == sd_asserted);
    CHECK(CallsFor(mo) == mo_asserted);

    frame(3500);
    CHECK(CallsFor(sd) == sd_asserted + 1);
    CHECK(f.Raw(sd) == 0);
    CHECK(CallsFor(mo) == mo_asserted + 1);
    CHECK(f.Raw(mo) != 0);
}

TEST_CASE("netvarhud: an old file with no phase rows leaves both flag keys with the host") {
    const uint64_t sd = kHudVars[kSuddenDeathIndex].symbol;
    const uint64_t mo = kHudVars[kMatchOverIndex].symbol;
    Fixture f;                                   // BuildTracks has no flag rows: a pre-flag capture
    f.InstallGuard();
    f.Arm(true, true, true, /*host_filter=*/true);
    f.sess.globalStore.Set(sd, 0);
    f.sess.globalStore.Set(mo, 0);
    f.Frame(2600);
    CHECK(CallsFor(sd) == 0);
    CHECK(CallsFor(mo) == 0);

    // Un-owned means the host still drives them: a key this file has nothing to say about must not
    // be taken away from the only writer there is.
    const uint32_t handle = FakeSession::kGlobalSlot | (uint32_t{FakeSession::kGlobalGeneration} << 16);
    guard::DetoursForTest().setFlag(f.sess.cs, handle, sd, 1);
    guard::DetoursForTest().setFlag(f.sess.cs, handle, mo, 1);
    CHECK(f.Raw(sd) == 1);
    CHECK(f.Raw(mo) == 1);
}

TEST_CASE("netvarhud: every HUD key and the frisbee owner are held at once -- the owned table fits") {
    Fixture f;
    f.InstallGuard();
    Window w;
    std::vector<KeyedRecord> recs;
    for (size_t i = 0; i < kHudVarCount; ++i) {
        const uint64_t v = kHudVars[i].kind == HudVarKind::Real ? uint64_t(RawF(7.0f))
                         : kHudVars[i].kind == HudVarKind::Symbol ? kStateA
                         : 1u;
        recs.push_back({1000, kHudVars[i].symbol, v});  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    }
    recs.push_back({1000, kFrisbeeOwner, 4});  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(w.NetVars().Build(nullptr, 0, recs.data(), recs.size()));
    for (const KeyedRecord& r : recs) w.NoteNetVarSlot(r.key, FakeSession::kGlobalSlot);
    f.Arm(true, true, true, /*host_filter=*/true);
    f.sess.globalStore.Set(kFrisbeeOwner, 4);

    HudNetVarFrame hud;
    ResolveHudNetVars(w, 2000, hud);
    HudNetVarsPublish(hud);
    HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);

    // Nine HUD keys plus the frisbee owner. One entry short and the LAST key
    // handed over is silently dropped from the guard's table -- invisible in every per-key test,
    // because every other key still swallows.
    const uint32_t handle = FakeSession::kGlobalSlot | (uint32_t{FakeSession::kGlobalGeneration} << 16);
    const uint64_t mo = kHudVars[kMatchOverIndex].symbol;
    const uint64_t held_mo = f.Raw(mo);
    guard::DetoursForTest().setFlag(f.sess.cs, handle, mo, uint8_t(held_mo == 0 ? 1 : 0));
    CHECK(f.Raw(mo) == held_mo);
    guard::DetoursForTest().setSymbolNetGame(f.sess.NetGame(), kFrisbeeOwner, 0xFFFFFFFFull, INFINITY);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    CHECK(f.Raw(kFrisbeeOwner) == 4);
    guard::DetoursForTest().setRealNetGame(f.sess.NetGame(), kGameTimer, 250.0f);
    CHECK(f.Timer() == 7.0f);
}

TEST_CASE("netvarhud: Disarm hands every owned key back -- ownership does not cross a reload") {
    Fixture f;
    f.InstallGuard();
    f.Arm(true, true, true, /*host_filter=*/true);
    f.Frame(2600);
    REQUIRE(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));
    REQUIRE(HudNetVarsOwns(kGameState, FakeSession::kGlobalSlot));

    HudNetVarsDisarm();
    CHECK_FALSE(HudNetVarsOwns(kGameTimer, FakeSession::kGlobalSlot));

    // THE `--:--` BUG, in its own shape: the NEXT load arms on a route that is already valid, so
    // its first sweep runs before a single cell has been published. With the previous file's
    // ownership left standing, that empty sweep re-owns all seven keys with no value of its own and
    // the guard goes on swallowing the host's game_state for the rest of the session -- the
    // scoreboard clock is gated on game_state, so it never draws again.
    f.Arm(true, true, true, /*host_filter=*/true);
    HudNetVarsInvalidate(false);      // armed, nothing asserted yet: the state that sweep runs in
    HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);

    guard::DetoursForTest().setRealNetGame(f.sess.NetGame(), kGameTimer, 250.0f);
    CHECK(f.Timer() == 250.0f);
    guard::DetoursForTest().setSymbolNetGame(f.sess.NetGame(), kGameState, 0x1111, INFINITY);
    CHECK(f.Raw(kGameState) == 0x1111);
}

// ---- HOST-HOLD -------------------------------------------------------------------------------
//
// The two keys are the HOST's, not the recording's: they are never resolved from the window, they
// are held at a constant while a replay is loaded, and they are given back at the value the engine
// held before -- including on the sweep AFTER the writer has been disarmed, which is the one that
// decides whether the host's round clock ever restarts.

namespace {
constexpr uint64_t kRoundTimeKey = 0x5B748A2B4792DD2Dull;
constexpr uint64_t kPausedKey    = 0x02FE0AA4F2637309ull;
constexpr uint64_t kPausedValue  = 0x41D2D73C0F30021Aull;
}  // namespace

TEST_CASE("hosthold: the round-time seed is taken, owned, and given back at the engine's value") {
    Fixture f;
    f.InstallGuard();
    f.sess.globalStore.Set(kRoundTimeKey, 300u);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    f.Arm(true, true, true, /*host_filter=*/true);

    HudNetVarsSetHostHold(true, false);
    f.Frame(2600);
    CHECK(f.Raw(kRoundTimeKey) == 3600u);
    CHECK(HudNetVarsHostHoldHeld(0));
    CHECK_FALSE(HudNetVarsHostHoldHeld(1));

    // Owned: the host's own write to the same key is swallowed, so it cannot put 300 back.
    const uint32_t handle =
        FakeSession::kGlobalSlot | (uint32_t{FakeSession::kGlobalGeneration} << 16);
    guard::DetoursForTest().setInt(f.sess.cs, handle, kRoundTimeKey, 300u);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    CHECK(f.Raw(kRoundTimeKey) == 3600u);

    // Unload. The writer is disarmed, but the key is still the host's to get back.
    HudNetVarsSetHostHold(false, false);
    HudNetVarsDisarm();
    HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);
    CHECK(f.Raw(kRoundTimeKey) == 300u);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    CHECK_FALSE(HudNetVarsHostHoldHeld(0));
    // ...and released: the host's write lands again.
    guard::DetoursForTest().setInt(f.sess.cs, handle, kRoundTimeKey, 180u);
    CHECK(f.Raw(kRoundTimeKey) == 180u);
}

TEST_CASE("hosthold: an unload with no Set beforehand still gives the key back") {
    Fixture f;
    f.InstallGuard();
    f.sess.globalStore.Set(kRoundTimeKey, 300u);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    f.Arm(true, true, true, true);
    HudNetVarsSetHostHold(true, true);
    f.Frame(2600);
    REQUIRE(f.Raw(kRoundTimeKey) == 3600u);
    REQUIRE(f.Raw(kPausedKey) == kPausedValue);

    HudNetVarsDisarm();   // the module's unload order, without the tick that clears `want`
    HudNetVarsSweep(f.sess.NetGame(), f.now_ms += 16);
    CHECK(f.Raw(kRoundTimeKey) == 300u);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    CHECK_FALSE(HudNetVarsHostHoldHeld(0));
    CHECK_FALSE(HudNetVarsHostHoldHeld(1));
}

TEST_CASE("hosthold: the pause key is only written when it is asked for, and wins over the record") {
    Fixture f;
    f.InstallGuard();
    f.sess.globalStore.Set(kPausedKey, 0x1234ull);
    f.Arm(true, true, true, true);

    HudNetVarsSetHostHold(true, false);
    f.Frame(2600);
    CHECK(f.Raw(kPausedKey) == 0x1234ull);   // off by default: nothing touches it

    HudNetVarsSetHostHold(true, true);
    f.Frame(2700);
    CHECK(f.Raw(kPausedKey) == kPausedValue);
    CHECK(HudNetVarsHostHoldHeld(1));
    // Precedence: a recorded value for this key arrives through the decoder, which asks first.
    const uint32_t handle =
        FakeSession::kGlobalSlot | (uint32_t{FakeSession::kGlobalGeneration} << 16);
    guard::DetoursForTest().setSymbol(f.sess.cs, handle, kPausedKey, 0x1234ull, INFINITY);
    CHECK(f.Raw(kPausedKey) == kPausedValue);

    HudNetVarsSetHostHold(true, false);
    f.Frame(2800);
    CHECK(f.Raw(kPausedKey) == 0x1234ull);   // released back to what the engine held
    CHECK_FALSE(HudNetVarsHostHoldHeld(1));
}

TEST_CASE("hosthold: nothing is held when the writer is not armed for a contained process") {
    Fixture f;
    f.InstallGuard();
    f.sess.globalStore.Set(kRoundTimeKey, 300u);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    f.Arm(true, true, /*contained=*/false, true);
    HudNetVarsSetHostHold(true, true);
    f.Frame(2600);
    CHECK(f.Raw(kRoundTimeKey) == 300u);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    CHECK_FALSE(HudNetVarsHostHoldHeld(0));
    CHECK_FALSE(HudNetVarsHostHoldHeld(1));
}

TEST_CASE("hosthold: the guard's owned table still covers every key at once") {
    Fixture f;
    f.InstallGuard();
    f.sess.globalStore.Set(kRoundTimeKey, 300u);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    f.sess.globalStore.Set(kPausedKey, 0x1234ull);
    f.Arm(true, true, true, true);
    HudNetVarsSetHostHold(true, true);
    f.Frame(2000);

    // Nine HUD keys, the frisbee owner and the two hold keys. One position short and the LAST key
    // handed over is silently dropped from the guard's table -- invisible in every per-key test,
    // because every other key still swallows.
    const uint32_t handle =
        FakeSession::kGlobalSlot | (uint32_t{FakeSession::kGlobalGeneration} << 16);
    guard::DetoursForTest().setSymbol(f.sess.cs, handle, kPausedKey, 0x9999ull, INFINITY);
    CHECK(f.Raw(kPausedKey) == kPausedValue);
    guard::DetoursForTest().setInt(f.sess.cs, handle, kRoundTimeKey, 5u);
    CHECK(f.Raw(kRoundTimeKey) == 3600u);
    const float timer = f.Timer();
    guard::DetoursForTest().setRealNetGame(f.sess.NetGame(), kGameTimer, 250.0f);
    CHECK(f.Timer() == timer);   // the HUD keys still swallow with the hold positions in the table
}

// ---- the hold POLICY, with no engine at all ---------------------------------------------------

TEST_CASE("hosthold: round-time holds from the load, pause waits for main_loop") {
    hosthold::ResetForTest();
    hosthold::Policy p;
    p.loaded = true;
    p.round_time_on = true;
    p.pause_on = true;
    p.phase_name = "pre_match_enter";

    // Pre-roll: the host is nowhere near main_loop, and the round-time seed is exactly what has to
    // be held NOW -- its only read is the round_start that happens during the pre-roll.
    hosthold::Want w = hosthold::Step(p, 16000);
    CHECK(w.round_time);
    CHECK_FALSE(w.pause);

    p.playing = true;
    p.phase_name = "main_loop_enter";
    w = hosthold::Step(p, 16000);
    CHECK(w.round_time);
    CHECK(w.pause);

    // Out of main_loop: held through the grace, released after it, re-armed on re-entry.
    p.playing = false;
    p.phase_name = "transition_to_launch_enter";
    w = hosthold::Step(p, hosthold::kOutsideMainLoopGraceUs / 2);
    CHECK(w.pause);
    w = hosthold::Step(p, hosthold::kOutsideMainLoopGraceUs);
    CHECK_FALSE(w.pause);
    CHECK(w.round_time);
    p.playing = true;
    p.phase_name = "main_loop_update";
    w = hosthold::Step(p, 16000);
    CHECK(w.pause);

    p.loaded = false;
    w = hosthold::Step(p, 16000);
    CHECK_FALSE(w.round_time);
    CHECK_FALSE(w.pause);
}

TEST_CASE("hosthold: a borrow lifts both holds, and a borrow nobody returns is watchdogged") {
    hosthold::ResetForTest();
    hosthold::Policy p;
    p.loaded = true;
    p.round_time_on = true;
    p.pause_on = true;
    p.playing = true;
    p.phase_name = "main_loop_enter";
    REQUIRE(hosthold::Step(p, 16000).pause);

    hosthold::Borrow();
    hosthold::Borrow();
    CHECK(hosthold::BorrowDepth() == 2);
    hosthold::Want w = hosthold::Step(p, 16000);
    CHECK_FALSE(w.round_time);
    CHECK_FALSE(w.pause);
    hosthold::Return();
    w = hosthold::Step(p, 16000);
    CHECK_FALSE(w.round_time);   // still one borrow outstanding
    hosthold::Return();
    w = hosthold::Step(p, 16000);
    CHECK(w.round_time);
    CHECK(w.pause);
    // Return past zero cannot wrap the count into a permanent lift.
    hosthold::Return();
    CHECK(hosthold::BorrowDepth() == 0);

    hosthold::Borrow();
    CHECK_FALSE(hosthold::Step(p, 16000).pause);
    hosthold::Step(p, hosthold::kBorrowWatchdogUs);
    CHECK(hosthold::BorrowDepth() == 0);
    CHECK(hosthold::Step(p, 16000).pause);
    hosthold::ResetForTest();
}
