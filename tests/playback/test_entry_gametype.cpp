// The gametype a capture is entered with.
//
// Every capture written before the recorder kept the whole gametype qword holds the hash's low
// dword or 0xFFFFFFFF, neither of which the engine recognizes. These cases pin each resolution rule
// on its own, the recorder's late fill, and the read-back out of a real written .echodemo:
// ChapterBuilder -> the container -> DemoReader -> OwnerStateGametype.

#include <doctest.h>

#include <windows.h>

#include <string.h>

#include <string>
#include <vector>

#include "capture/chapterbuilder.h"
#include "capture/gametypeid.h"
#include "capture/manifestbuild.h"
#include "foundation/demofile/demofile.h"
#include "foundation/gametype.h"
#include "playback/core/entry_gametype.h"

using namespace replay;
using namespace replay::capture;
namespace gt = replay::gametype;

namespace {

// Read out of the engine (RE-CLOCK-SCOREBOARD-PHASE's name table), not out of SymbolHash.
constexpr uint64_t kArena = 0xCB60A4DE7E1CAF73ull;
constexpr uint64_t kArenaPrivate = 0x09990965F4DB8C03ull;
constexpr uint64_t kCombat = 0x3D5C3976578A321Aull;
constexpr uint64_t kCombatPrivate = 0x33BBF6842DF97A3Full;
constexpr uint64_t kPracticeAi = 0x888AC0CFD102345Full;
constexpr uint64_t kSocial = 0x042D9CF9CFDDCF76ull;
constexpr uint64_t kArenaLevel = 0x576ED3F8428EBC4Bull;  // mpl_arena_a

// The owner's real capture: what its manifest says, and what its owner-state event says.
constexpr uint64_t kRealManifestValue = 0x00000000FFFFFFFFull;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
const char kRealOwnerState[] =
    "{\"gametype\":-3791849610740453517, \"starttime\":1789716229, "
    "\"level\":6300205991959903307, \"ingame\":true}";

uint64_t Low(uint64_t id) { return id & 0xFFFFFFFFull; }  // facts-ok: PHYS.ID_DEAD a u32 mask over a packed id; PHYS.ID_DEAD is PC_PRESENT and GOAL.USERID_UNSET is unmerged, so neither emits a constant

}  // namespace

TEST_CASE("CSymbol64 reproduces the engine's own name hashes") {
    CHECK(gt::SymbolHash("echo_arena") == kArena);
    CHECK(gt::SymbolHash("echo_arena_private") == kArenaPrivate);
    CHECK(gt::SymbolHash("echo_combat") == kCombat);
    CHECK(gt::SymbolHash("echo_combat_private") == kCombatPrivate);
    CHECK(gt::SymbolHash("echo_arena_practice_ai") == kPracticeAi);
    CHECK(gt::SymbolHash("social_2.0") == kSocial);
    CHECK(gt::SymbolHash("mpl_arena_a") == kArenaLevel);
    // Lower-cased before hashing, as the engine does.
    CHECK(gt::SymbolHash("ECHO_Arena") == kArena);
    CHECK(gt::SymbolHash("echo_arenb") != kArena);
}

TEST_CASE("no two known gametypes share a low dword, so the 32-bit match is never a guess") {
    for (const gt::Known& a : gt::kKnown) {
        CHECK(a.id == gt::SymbolHash(a.name));
        CHECK(gt::IsFull(a.id));
        CHECK(gt::MatchLowDword(uint32_t(a.id)) == a.id);
        CHECK(std::string(gt::NameOf(a.id)) == a.name);
    }
    CHECK(gt::NameOf(0x1234567800000000ull) == nullptr);
}

TEST_CASE("rule 1: a whole 64-bit recorded gametype passes through, known or not") {
    gt::Resolved r = gt::Resolve(kCombat, kArena, kArena);
    CHECK(r.id == kCombat);
    CHECK(r.rule == gt::Rule::Manifest64);

    // Not in the table, still a whole qword: the engine's word is not second-guessed.
    r = gt::Resolve(0xABCDEF0123456789ull, kArena, kArena);
    CHECK(r.id == 0xABCDEF0123456789ull);
    CHECK(r.rule == gt::Rule::Manifest64);
}

TEST_CASE("rule 2: a recorded low dword becomes the whole hash, and combat stays combat") {
    // Owner state and fallback both say ARENA; the low dword must still win.
    gt::Resolved r = gt::Resolve(Low(kCombat), kArena, kArena);
    CHECK(r.id == kCombat);
    CHECK(r.rule == gt::Rule::LowDword);

    r = gt::Resolve(Low(kCombatPrivate), kArena, kArena);
    CHECK(r.id == kCombatPrivate);

    r = gt::Resolve(Low(kArena), kCombat, kCombat);
    CHECK(r.id == kArena);
    CHECK(r.rule == gt::Rule::LowDword);

    r = gt::Resolve(Low(kSocial), 0, kArena);
    CHECK(r.id == kSocial);
}

TEST_CASE("rule 3: the real capture's 0xFFFFFFFF is recovered from its own owner-state event") {
    uint64_t stated = 0;
    REQUIRE(gt::ParseOwnerStateGametype(kRealOwnerState, sizeof(kRealOwnerState) - 1, &stated));
    CHECK(stated == kArena);

    // The fallback is COMBAT here so that the event, not the fallback, is what answered.
    gt::Resolved r = gt::Resolve(kRealManifestValue, stated, kCombat);
    CHECK(r.id == kArena);
    CHECK(r.rule == gt::Rule::OwnerState);

    // All-ones at the new read width, and a file that recorded nothing, recover the same way.
    r = gt::Resolve(~0ull, kCombat, kArena);
    CHECK(r.id == kCombat);
    CHECK(r.rule == gt::Rule::OwnerState);
    r = gt::Resolve(0, kCombat, kArena);
    CHECK(r.id == kCombat);
    CHECK(r.rule == gt::Rule::OwnerState);

    // An owner state that is itself not a whole gametype settles nothing.
    r = gt::Resolve(kRealManifestValue, 0x7E1CAF73ull, kCombat);
    CHECK(r.rule == gt::Rule::Fallback);
}

TEST_CASE("rule 4: with nothing else, the fallback name; with no fallback, the value is left alone") {
    gt::Resolved r = gt::Resolve(kRealManifestValue, 0, kArena);
    CHECK(r.id == kArena);
    CHECK(r.rule == gt::Rule::Fallback);

    // An unknown 32-bit value is not a low-dword match.
    r = gt::Resolve(0x12345678ull, 0, kPracticeAi);
    CHECK(r.id == kPracticeAi);
    CHECK(r.rule == gt::Rule::Fallback);

    r = gt::Resolve(kRealManifestValue, 0, 0);
    CHECK(r.id == kRealManifestValue);
    CHECK(r.rule == gt::Rule::Unresolved);

    // A capture that recorded no gametype at all never asked for one: the command line's stands.
    r = gt::Resolve(0, 0, kArena);
    CHECK(r.id == 0);
    CHECK(r.rule == gt::Rule::Unresolved);
}

TEST_CASE("each rule has its own name for the log line") {
    const gt::Rule rules[] = {gt::Rule::Manifest64, gt::Rule::LowDword, gt::Rule::OwnerState,
                              gt::Rule::Fallback, gt::Rule::Unresolved};
    for (size_t i = 0; i < 5; ++i) {
        for (size_t k = i + 1; k < 5; ++k) {
            CHECK(std::string(gt::RuleName(rules[i])) != gt::RuleName(rules[k]));
        }
    }
    CHECK(std::string(gt::RuleName(gt::Rule::LowDword)) == "low-dword-match");
    CHECK(std::string(gt::RuleName(gt::Rule::OwnerState)) == "owner-state-event");
}

TEST_CASE("the events are only read when the recorded value settles nothing") {
    CHECK_FALSE(gt::NeedsOwnerState(kCombat));
    CHECK_FALSE(gt::NeedsOwnerState(Low(kCombat)));
    CHECK(gt::NeedsOwnerState(kRealManifestValue));
    CHECK(gt::NeedsOwnerState(~0ull));
    CHECK(gt::NeedsOwnerState(0));
}

TEST_CASE("the owner-state parser takes the signed decimal the engine prints, and refuses the rest") {
    uint64_t v = 0;
    const auto parse = [&v](const char* s) {
        v = 0;
        return gt::ParseOwnerStateGametype(s, strlen(s), &v);
    };
    CHECK(parse("{\"gametype\":4421472114608583194}"));
    CHECK(v == kCombat);
    CHECK(parse("{\"level\":1,\"gametype\" : -9223372036854775808}"));
    CHECK(v == 0x8000000000000000ull);
    CHECK(parse("{\"gametype\":18446744073709551615}"));
    CHECK(v == ~0ull);

    CHECK_FALSE(parse("{\"gametype\":18446744073709551616}"));  // one past a qword
    CHECK_FALSE(parse("{\"gametype\":\"echo_arena\"}"));
    CHECK_FALSE(parse("{\"gametype\":-}"));
    CHECK_FALSE(parse("{\"gametypes\":5}"));
    CHECK_FALSE(parse("{\"level\":6300205991959903307}"));
    CHECK_FALSE(parse(""));
    // Bounded by the byte count, not by a terminator the body does not carry.
    CHECK_FALSE(gt::ParseOwnerStateGametype(kRealOwnerState, 11, &v));
    CHECK_FALSE(gt::ParseOwnerStateGametype(nullptr, 0, &v));
}

namespace {

struct TempDemo {
    wchar_t path[MAX_PATH];
    explicit TempDemo(const wchar_t* tag) {
        wchar_t dir[MAX_PATH];
        GetTempPathW(MAX_PATH, dir);
        swprintf_s(path, L"%lsshippath-%ls-%lu.echodemo", dir, tag, GetCurrentProcessId());
    }
    ~TempDemo() { DeleteFileW(path); }
};

template <class T>
void Feed(ChapterBuilder& b, uint16_t kind, const T& payload) {
    uint8_t buf[sizeof(RecHeader) + sizeof(T)];
    RecHeader h{};
    h.kind = kind;
    h.bytes = uint16_t(sizeof(T));
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &payload, sizeof payload);
    b.Consume(buf, uint32_t(sizeof buf));
}

void FeedEvent(ChapterBuilder& b, uint64_t stampUs, uint16_t typeIndex, const std::string& body) {
    // The engine's body is the JSON and its terminator.
    const size_t bodyBytes = body.size() + 1;
    std::vector<uint8_t> buf(sizeof(RecHeader) + sizeof(EventRec) + bodyBytes);
    RecHeader h{};
    h.kind = kRecEvent;
    h.bytes = uint16_t(sizeof(EventRec) + bodyBytes);
    EventRec r{};
    r.stampUs = stampUs;
    r.typeIndex = typeIndex;
    r.bodyBytes = uint16_t(bodyBytes);
    memcpy(buf.data(), &h, sizeof h);
    memcpy(buf.data() + sizeof h, &r, sizeof r);
    memcpy(buf.data() + sizeof h + sizeof r, body.c_str(), bodyBytes);
    b.Consume(buf.data(), uint32_t(buf.size()));
}

enum class OwnerStateRows { None, Real };

// A capture the way the recorder writes one: `recordedGametype` is what the engine field read at
// open, and the close applies the recorder's own late-fill rule exactly as capture.cpp does.
void WriteCapture(const wchar_t* path, uint64_t recordedGametype, OwnerStateRows rows,
                  bool applyLateFill) {
    const uint64_t t0 = 5000000;
    ManifestBuilder mb;
    mb.Begin(t0, 0, 0x40);
    mb.SetLevel(kArenaLevel, recordedGametype, 0);
    mb.SetKeyframeIntervalUs(2000000);
    mb.SetUuid(1, 2);
    REQUIRE(mb.Refusal() == nullptr);

    DemoWriter writer;
    REQUIRE(writer.Open(path, mb.View()));

    BuilderConfig cfg;
    cfg.keyframeIntervalUs = 2000000;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;

    const uint16_t ownerState = TapTypeIndex("SNSLobbyOwnerState");
    const uint16_t entrantState = TapTypeIndex("SNSLobbyEntrantStatev2");
    REQUIRE(ownerState != kTapTypeIndexNone);
    REQUIRE(entrantState != kTapTypeIndexNone);

    ChapterBuilder b;
    REQUIRE(b.Open(&writer, mb.View(), cfg));
    b.EmitMatchBegin(t0);
    const uint64_t dt = 11111;
    for (uint32_t f = 0; f < 300; ++f) {  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
        const uint64_t st = t0 + f * dt;
        TickRec tr{};
        tr.sourceTimeUs = st;
        tr.simTimeUs = 1000000 + f * dt;
        tr.frameIndex = f + 1;
        tr.frameDeltaUs = uint32_t(dt);
        tr.scaledDeltaUs = uint32_t(dt);
        Feed(b, kRecTick, tr);
        if (f == 2) {
            // A DIFFERENT tap row carrying a gametype member: only the owner-state row may answer.
            FeedEvent(b, st, entrantState, "{\"gametype\":4421472114608583194}");
        }
        if (f == 3 && rows == OwnerStateRows::Real) FeedEvent(b, st, ownerState, kRealOwnerState);
    }
    b.EmitMatchEnd(t0 + 300 * dt);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
    b.FlushChapter();
    b.Close();
    if (applyLateFill) {
        const uint64_t late = LateGametype(mb.View().gametypeId, b.OwnerStateGametype());
        if (late != 0) REQUIRE(writer.SetGametypeId(late));
    }
    writer.Close();
}

uint64_t ManifestGametypeOf(const wchar_t* path) {
    DemoReader reader;
    REQUIRE(reader.Open(path));
    return reader.GetManifest().gametypeId;
}

}  // namespace

TEST_CASE("an OLD capture still loads, and its gametype is read back out of its own event") {
    TempDemo demo(L"old");
    // Exactly the owner's file: 0xFFFFFFFF in the manifest, the truth on S_EVENT, no late fill.
    WriteCapture(demo.path, kRealManifestValue, OwnerStateRows::Real, /*applyLateFill=*/false);

    DemoReader reader;
    REQUIRE(reader.Open(demo.path));
    CHECK(reader.GetManifest().gametypeId == kRealManifestValue);
    CHECK(reader.GetManifest().levelId == kArenaLevel);
    CHECK(reader.ChapterCount() > 0);

    // The entrant-state row two ticks earlier states COMBAT; answering that would be reading the
    // wrong tap row.
    CHECK(playback::OwnerStateGametype(reader) == kArena);
    CHECK(playback::OwnerStateGametypeOfFile(demo.path) == kArena);

    const gt::Resolved r =
        gt::Resolve(reader.GetManifest().gametypeId, playback::OwnerStateGametype(reader), kCombat);
    CHECK(r.id == kArena);
    CHECK(r.rule == gt::Rule::OwnerState);
}

TEST_CASE("a capture with no owner-state row, or no file at all, answers 0 rather than a guess") {
    TempDemo demo(L"none");
    WriteCapture(demo.path, kRealManifestValue, OwnerStateRows::None, false);
    CHECK(playback::OwnerStateGametypeOfFile(demo.path) == 0);
    CHECK(playback::OwnerStateGametypeOfFile(L"Z:\\no\\such\\capture.echodemo") == 0);
    CHECK(playback::OwnerStateGametypeOfFile(nullptr) == 0);
}

TEST_CASE("the recorder files the whole gametype: read at open, or filled from the lobby at close") {
    {
        TempDemo demo(L"full");
        WriteCapture(demo.path, kCombat, OwnerStateRows::Real, true);
        // The engine's whole word stands even though the owner state says arena.
        CHECK(ManifestGametypeOf(demo.path) == kCombat);
    }
    {
        TempDemo demo(L"late");
        WriteCapture(demo.path, ~0ull, OwnerStateRows::Real, true);
        // THE MUTANT: drop DemoWriter::SetGametypeId's store (SHIP_MUTATE_NO_LATE_GAMETYPE) and the
        // trailer manifest keeps the all-ones read at open.
        CHECK(ManifestGametypeOf(demo.path) == kArena);
        CHECK(gt::Resolve(ManifestGametypeOf(demo.path), 0, 0).rule == gt::Rule::Manifest64);
    }
    {
        TempDemo demo(L"unfilled");
        WriteCapture(demo.path, ~0ull, OwnerStateRows::None, true);
        CHECK(ManifestGametypeOf(demo.path) == ~0ull);
    }
}

// The owner's real spectated capture, when this machine has it: manifest 0xFFFFFFFF, and an
// owner-state event the engine itself wrote. Machine-local, so absent it says so rather than pass.
TEST_CASE("the owner's real capture states echo_arena in its own owner-state event") {
    wchar_t path[MAX_PATH];
    const DWORD n = ExpandEnvironmentStringsW(
        L"%USERPROFILE%\\Documents\\EchoReplay\\"
        L"2026-09-18_03-33-45_576ED3F8428EBC4B_00000000FFFFFFFF.echodemo",
        path, MAX_PATH);
    REQUIRE(n != 0);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        MESSAGE("SKIPPED BY NAME: the real capture is not on this machine");
        return;
    }
    CHECK(ManifestGametypeOf(path) == kRealManifestValue);
    CHECK(playback::OwnerStateGametypeOfFile(path) == kArena);
}
