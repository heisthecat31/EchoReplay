// The round trip: real records through the real container, read back by the real player, into a
// stand-in for VOIP.RECEIVE.
//
// ★ THIS IS THE ARM THAT CAUGHT THE BLOCK PREFIX. A stream block is
// [u32 exact bit length][packed records]; a BitReader built on StreamView::records directly starts
// four bytes early and the first decode refuses. Nothing in the resolution or injection arms
// touches the container, so without this the player's reader would have been unexercised -- and it
// would have failed on the first real file with a plausible "no voice in this recording".
//
// ★ AND THE ONLY THING SUBSTITUTED IS THE ENGINE ENTRY POINT. SetEntryPointForTest replaces a
// mapped VOIP.RECEIVE, which an offline machine cannot have, and nothing else: the chapter read,
// the span arithmetic, the rate gate, the resolution walk, the gate inspection and InjectVoip
// itself are the shipping code. A dry-run mode would have left the branch that ships untested.

#include <string.h>

#include <string>
#include <vector>

#include "audio/audio_facts.h"
#include "audio/memprobe.h"
#include "audio/voip/voip_facts.h"
#include "audio/voip/voipplay.h"
#include "audio/voipformat.h"
#include "bootstrap/livesafety.h"
#include "capture/capture_types.h"
#include "capture/streamcodec.h"
#include "doctest.h"
#include "fakeengine.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demofile.h"

using namespace replay;
using namespace replay::audio;
using namespace replay::audio::voip;
using w3c::FakeEngine;
using w3c::FakeReceiveVoip;
using w3c::g_call;
using w3c::RecordedCall;

namespace {

struct Frame {
    uint64_t tUs;
    uint32_t speaker;
    uint16_t seq;
    std::vector<uint8_t> payload;
};

// Framed exactly the way chapterbuilder frames it: EncodeRawRecord per record, then the u32 bit
// count in front. Written here rather than reusing the builder because the builder is W2-A's whole
// worker; the two must agree, and this is the file that says what "agree" means.
std::vector<uint8_t> BuildVoipBlock(const std::vector<Frame>& frames) {
    std::vector<uint8_t> packed(64 * 1024, 0);
    bitio::BitWriter w(packed.data(), packed.size());
    std::vector<uint8_t> body;
    for (const Frame& f : frames) {
        body.assign(sizeof(capture::VoipRec) + f.payload.size(), 0);
        capture::VoipRec r{};
        r.tUs = f.tUs;
        r.speakerSlot = f.speaker;
        r.frameCtr = f.seq;
        r.bodyBytes = static_cast<uint16_t>(f.payload.size());
        memcpy(body.data(), &r, sizeof r);
        memcpy(body.data() + sizeof r, f.payload.data(), f.payload.size());
        REQUIRE(capture::EncodeRawRecord(w, body.data(), static_cast<uint16_t>(body.size())));
    }
    const uint32_t bits = static_cast<uint32_t>(w.BitsWritten());
    const uint32_t packedBytes = (bits + 7u) / 8u;
    std::vector<uint8_t> block(capture::kBlockPrefixBytes + packedBytes, 0);
    memcpy(block.data(), &bits, sizeof bits);
    memcpy(block.data() + capture::kBlockPrefixBytes, packed.data(), packedBytes);
    return block;
}

std::vector<Frame> MakeFrames(uint32_t speaker, uint64_t t0Us, uint32_t n) {
    std::vector<Frame> out;
    for (uint32_t i = 0; i < n; ++i) {
        Frame f;
        f.tUs = t0Us + uint64_t(i) * 20000u;   // fifty packets a second
        f.speaker = speaker;
        f.seq = static_cast<uint16_t>(i & kSeqMask);
        f.payload.assign(32 + (i % 8), static_cast<uint8_t>(0x40 + i));
        out.push_back(f);
    }
    return out;
}

// A file that exists only to be a clock: five frames a second, so a test can walk the playhead
// across tens of seconds without ever running out of chapter.
std::vector<Frame> MakeClockFrames(uint32_t speaker, uint64_t t0Us, uint32_t n) {
    std::vector<Frame> out;
    for (uint32_t i = 0; i < n; ++i) {
        Frame f;
        f.tUs = t0Us + uint64_t(i) * 200000u;
        f.speaker = speaker;
        f.seq = static_cast<uint16_t>(i & kSeqMask);
        f.payload.assign(8, static_cast<uint8_t>(0x5A));
        out.push_back(f);
    }
    return out;
}

bool WriteVoipDemo(const wchar_t* path, const std::vector<Frame>& frames, uint64_t t0Us) {
    Manifest m{};
    m.formatMajor = 1;
    m.t0Us = t0Us;
    m.keyframeIntervalUs = 2000000;
    DemoWriter w;
    if (!w.Open(path, m)) {
        return false;
    }
    const std::vector<uint8_t> block = BuildVoipBlock(frames);
    w.BeginChapter(kSectionVoip, t0Us);
    w.WriteStreamBlock(kStreamVoip, capture::kSchemaVoip, block.data(), block.size(),
                       static_cast<uint32_t>(frames.size()), static_cast<uint32_t>(frames.size()));
    w.EndChapter(frames.back().tUs + 1);
    w.Close();
    return w.Stats().bytesWritten != 0;
}

void MakeEngineWithTalker(FakeEngine& e, uint16_t slot, uint16_t generation) {
    e.Build(1);
    e.SetSlotGeneration(slot, generation);
    e.Bind(0, 4, 0x0007, MakeUserId(slot, generation));
    e.InstallLookup();
}


// The bind-epoch source the two epoch arms move by hand. The product reads this through the same
// function pointer voip_module installs the live IEntity behind, so what is substituted is the
// SOURCE of the term and not the key's use of it.
BindEpochTerm g_epochTerm;

BindEpochTerm TestBindEpoch() { return g_epochTerm; }

BindEpochTerm BoundEpoch(uint64_t generation) {
    BindEpochTerm t;
    t.state = BindEpochTerm::State::Bound;
    t.value = generation;
    return t;
}

// ---- the engine's own Connect/Disconnect, and the pointer they are handed --------------------
//
// ★ THE ORACLE FOR A WRITE THROUGH A FREED RECEIVER IS THE POINTER Disconnect IS HANDED. Counting
// releases cannot see it: releasing through the wrong component system and releasing through the
// right one both count one. So the seam records `cs`, and a test that has declared a receiver gone
// poisons its bytes and registers it here -- any call carrying it sets `touchedFreed`, which is a
// fact about the run rather than the absence of one.
// A user id no test ever mints, and deliberately NOT 0xFFFFFFFF: voipsite.cpp:79 says an unset
// talker slot is an ABSENCE rather than a licence to assume that value, so a fixture spelling it
// that way would look like the guess the product refuses to make.
uint32_t g_noUser = 0xA5A5A5A5u;

struct BindSeam {
    int   connects = 0;
    int   disconnects = 0;
    void* lastCs = nullptr;
    bool  touchedFreed = false;
};
BindSeam g_seam;
FakeEngine* g_seamTarget = nullptr;
const void* g_freedCs = nullptr;

uint16_t DenseOfPool(FakeEngine& e, uint16_t poolIndex) {
    for (uint16_t d = 0; d < FakeEngine::kInstances; ++d) {
        if (e.denseMap[d] == poolIndex) {
            return d;
        }
    }
    return 0;
}

void __fastcall SeamConnect(void* cs, uint16_t poolIndex, uint32_t userId) {
    ++g_seam.connects;
    g_seam.lastCs = cs;
    if (g_freedCs != nullptr && cs == g_freedCs) {
        g_seam.touchedFreed = true;
    }
    if (g_seamTarget == nullptr) {
        return;
    }
    // What the engine's Connect does to the field BindTalker reads back.
    g_seamTarget->Poke(g_seamTarget->ci,
                       g_seamTarget->CiOff(DenseOfPool(*g_seamTarget, poolIndex),
                                           voip_facts::kCiUserIdOff),
                       userId);
}

void __fastcall SeamDisconnect(void* cs, uint16_t poolIndex) {
    ++g_seam.disconnects;
    g_seam.lastCs = cs;
    if (g_freedCs != nullptr && cs == g_freedCs) {
        g_seam.touchedFreed = true;
    }
    if (g_seamTarget == nullptr) {
        return;
    }
    g_seamTarget->Poke(g_seamTarget->ci,
                       g_seamTarget->CiOff(DenseOfPool(*g_seamTarget, poolIndex),
                                           voip_facts::kCiUserIdOff),
                       g_noUser);
}

void ArmSeam(FakeEngine& e) {
    g_seam = BindSeam{};
    g_seamTarget = &e;
    g_freedCs = nullptr;
    SetBindCallsForTest(&SeamConnect, &SeamDisconnect, &g_noUser);
}

void DisarmSeam() {
    g_seamTarget = nullptr;
    g_freedCs = nullptr;
    SetBindCallsForTest(nullptr, nullptr, nullptr);
}

// One live receiver instance, FREE -- so the player has to bind it through the engine's own Connect
// and ends up OWNING a pool slot. MakeEngineWithTalker's instance is already bound, which answers
// AlreadyBound and can never produce an owned slot, so the abandon path has no fixture there at all.
void MakeEngineWithFreeInstance(FakeEngine& e, uint16_t slot, uint16_t generation) {
    e.Build(1);
    e.SetSlotGeneration(slot, generation);
    // Pool slot 2 for dense 0, so a pool index is never accidentally equal to its dense index.
    e.denseMap[0] = 2;
    e.pool[2] = 0u | (0x50u << 16);
    e.Poke(e.ci, e.CiOff(0, voip_facts::kCiUserIdOff), g_noUser);
    e.Poke(e.ci, e.CiOff(0, voip_facts::kCiDecoderStateOff), 0u);
    e.InstallLookup();
}

// Every case below drives a real bind, and TryBind now asks livesafety (BACKLOG-1 O3). A test
// binary never installs the offline-bit lock, so without this pin they would all be measuring the
// gate instead of the container walk. The refusal arm is its own case, and restores this state.
const struct PinContained {
    PinContained() {
        replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    }
} g_pin_contained;

}  // namespace

TEST_CASE("recorded voice reaches the engine's own entry point, in order, once each") {
    const wchar_t* path = L"w3c_voip_roundtrip.echodemo";
    const uint64_t t0 = 5000000;
    const std::vector<Frame> frames = MakeFrames(MakeUserId(6, 0x0003), t0, 25);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithTalker(e, 6, 0x0003);

    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    CHECK(player.HasVoice());

    // Frame 0 lands at t0 exactly. The seating span ends BEFORE it, so seating the cursor delivers
    // nothing and every frame is attributable to the loop below.
    int64_t at = static_cast<int64_t>(t0) - 2;
    size_t injected = player.Advance(at + 1, 1.0f, e.netGame.data());
    CHECK(injected == 0);
    at += 1;
    for (int i = 0; i < 30; ++i) {
        injected += player.Advance(at + 20000, 1.0f, e.netGame.data());
        at += 20000;
    }

    CHECK(injected == frames.size());
    CHECK(g_call.calls == static_cast<int>(frames.size()));
    CHECK(player.Stats().injected == frames.size());
    CHECK(player.Stats().recordsRead == frames.size());
    CHECK(player.Stats().recordsMalformed == 0);
    CHECK(player.Stats().chaptersLoaded == 1);
    CHECK(player.Stats().discontinuities == 0);
    // ★ Every block decoded is verified against the encoder's declared bit count. A player that
    // decoded blocks and verified none would report zero here while still looking like it worked.
    CHECK(player.Stats().blocksVerified == 1);
    CHECK(player.Stats().blocksBitMismatch == 0);

    // The last call carries the last frame, verbatim, with its own sequence in the stub.
    const Frame& last = frames.back();
    CHECK(g_call.length == last.payload.size());
    REQUIRE(g_call.payload.size() == last.payload.size());
    CHECK(memcmp(g_call.payload.data(), last.payload.data(), last.payload.size()) == 0);
    uint16_t seq = 0;
    memcpy(&seq, g_call.header + audio_facts::kVoipRecvSeqOff, sizeof seq);
    CHECK(seq == last.seq);
    // The handle came from the live tables: pool slot 4, generation 7.
    CHECK((g_call.handle & 0xFFFFu) == 4u);
    CHECK((g_call.handle >> 16) == 0x0007u);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    w3c::ClearLookup();
}

TEST_CASE("voice is real time or it is not delivered") {
    const wchar_t* path = L"w3c_voip_rate.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeFrames(MakeUserId(6, 0x0003), t0, 10);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithTalker(e, 6, 0x0003);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
    at += 1;

    SUBCASE("half speed feeds a codec at the wrong rate, so it feeds it nothing") {
        for (int i = 0; i < 12; ++i) {
            player.Advance(at + 20000, 0.5f, e.netGame.data());
            at += 20000;
        }
        CHECK(g_call.calls == 0);
        CHECK(player.Stats().injected == 0);
        CHECK(player.Stats().skippedRate == frames.size());
        // ★ The cursor still moved. Holding the frames back and releasing them when the rate
        // returned would inject a second of speech into one frame.
        CHECK(player.Stats().recordsRead == frames.size());
    }

    SUBCASE("a paused playhead delivers nothing, and a reversing one is a discontinuity") {
        // Paused: the playhead did not move, which is its own counter.
        CHECK(player.Advance(at, 1.0f, e.netGame.data()) == 0);
        CHECK(player.Stats().skippedNoStep == 1);
        CHECK(player.Stats().discontinuities == 0);

        // Reversing: the playhead moved BACKWARDS, which is a jump however small. It drops the
        // cursor and the cached receiver rather than being counted as a zero-length step.
        CHECK(player.Advance(at - 50000, -1.0f, e.netGame.data()) == 0);
        CHECK(player.Stats().discontinuities == 1);
        CHECK(g_call.calls == 0);
    }

    SUBCASE("a forward jump nobody announced is still a jump, and does not dump the backlog") {
        // Well past the contiguous-span threshold, so it is a seek rather than a hitch. The whole
        // file is 200 ms of speech; a span that covered it would hand the engine every frame at
        // once, which is what the threshold exists to prevent.
        const int64_t jumpTo = static_cast<int64_t>(t0) + 4000000;
        player.Advance(jumpTo, 1.0f, e.netGame.data());
        CHECK(player.Stats().discontinuities == 1);
        CHECK(g_call.calls == 0);
    }

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    w3c::ClearLookup();
}

// ★★★ THE ANTI-VACUITY ARM. The discontinuity guard shipped unreachable once: its only caller
// mirrored the value it compared against, so the comparison was `x != x` forever. A test that
// asserted "no discontinuity was reported" would have passed just as well against that dead code as
// against working code -- which is precisely how it survived.
//
// So this arm does NOT assert a counter and stop. It drives a real backwards seek and requires the
// OBSERVABLE CONSEQUENCE: the frames played before the seek are delivered AGAIN afterwards. With
// the guard dead, EnsureChapter returns early (same chapter), the reseat is gated on an already
// non-zero cursor, the held record's stamp is past the rewound playhead, and NOTHING is delivered.
// The counter check and the effect check therefore fail together, and tests/voip/check.ps1 builds a
// mutant with the guard neutered and requires exactly that.
TEST_CASE("a backwards seek re-seats the cursor, and the guard is observed doing it") {
    const wchar_t* path = L"w3c_voip_seekback.echodemo";
    const uint64_t t0 = 3000000;
    const std::vector<Frame> frames = MakeFrames(MakeUserId(6, 0x0003), t0, 25);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithTalker(e, 6, 0x0003);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t start = static_cast<int64_t>(t0) - 2;
    int64_t at = start;
    REQUIRE(player.Advance(at, 1.0f, e.netGame.data()) == 0);   // establishes the span
    for (int i = 0; i < 10; ++i) {
        at += 20000;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    const int deliveredBefore = g_call.calls;
    REQUIRE(deliveredBefore >= 10);
    CHECK(player.Stats().discontinuities == 0);

    // Seek back to the beginning. This is the first thing anyone does to a replay.
    at = start;
    player.Advance(at, 1.0f, e.netGame.data());
    CHECK(player.Stats().discontinuities == 1);

    // ★ The effect, which a counter alone would not have caught.
    for (int i = 0; i < 10; ++i) {
        at += 20000;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    CHECK(g_call.calls >= deliveredBefore + 10);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    w3c::ClearLookup();
}

TEST_CASE("a talker the live session does not have is refused, per frame, with a reason") {
    const wchar_t* path = L"w3c_voip_absent.echodemo";
    const uint64_t t0 = 2000000;
    const std::vector<Frame> frames = MakeFrames(MakeUserId(9, 0x0002), t0, 5);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithTalker(e, 6, 0x0003);   // a different player entirely
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 8; ++i) {
        player.Advance(at + 20000, 1.0f, e.netGame.data());
        at += 20000;
    }
    CHECK(g_call.calls == 0);
    CHECK(player.Stats().refusedNoTalker == frames.size());
    CHECK(player.Stats().lastResolve == Resolve::NoGeneration);
    char line[192];
    CHECK(std::string(player.StatusLine(line, sizeof line)).find("no live receiver") !=
          std::string::npos);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    w3c::ClearLookup();
}

TEST_CASE("a recording with no voice section is a state, not a failure") {
    const wchar_t* path = L"w3c_voip_stripped.echodemo";
    Manifest m{};
    m.formatMajor = 1;
    m.t0Us = 100;
    DemoWriter w;
    REQUIRE(w.Open(path, m));
    uint8_t rec[16] = {};
    w.BeginChapter(kSectionCore, 100);
    w.WriteStreamBlock(kStreamTick, capture::kSchemaTick, rec, sizeof rec, 1, 1);
    w.EndChapter(200);
    w.Close();

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    CHECK_FALSE(player.HasVoice());
    char line[192];
    CHECK(std::string(player.StatusLine(line, sizeof line)).find("no voice section") !=
          std::string::npos);
}

// ★ THE RESOLUTION IS AN ENGINE CALL, AND THE UNRESOLVED CASE IS THE ONE THAT USED TO PAY IT EVERY
// FRAME. The cache short-circuited only after SUCCESS, so a session that had not produced a
// receiver yet -- exactly the loading window -- re-entered CS.LOOKUP_BY_SYMBOL on every tick, and
// that call sorts the gamespace's system table in place on the calling thread (ARCH-10 S7). This
// arm counts the calls AT THE SEAM, because no counter inside the player can tell a cached answer
// apart from a thousand fresh ones.
TEST_CASE("an unresolved receiver is looked up once per space, never once per frame") {
    const wchar_t* path = L"w3c_voip_resolve_throttle.echodemo";
    const uint64_t t0 = 1000000;
    // Forty-four seconds of file, so the playhead can cross the retry cadence with chapter to spare.
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    // The netGame's space slot is aimed at the CR15Game decoy, whose +0x420 is the render-budget
    // word and not a table: the call is made, the engine is entered, and it answers null. A refusal
    // is the expensive case -- the table is sorted before it can answer at all -- so it is the case
    // a per-frame retry punishes hardest.
    FakeEngine e;
    FakeEngine other;
    e.Build(1);
    other.Build(1);
    e.InstallLookup();
    e.Poke(e.netGame, facts::NETGAME_CS_SPACE_OFF, e.gameDecoy.data());

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    w3c::g_lookupCalls = 0;

    // Sixty frames a second: the rate the module's tick actually calls Advance at.
    const int64_t kTickUs = 16667;
    int64_t at = static_cast<int64_t>(t0);
    REQUIRE(player.Advance(at, 1.0f, e.netGame.data()) == 0);   // establishes the span

    // A thousand ticks at sixty frames a second -- a whole loading window -- for one engine call.
    for (int i = 0; i < 1000; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    CHECK(w3c::g_lookupCalls == 1);
    CHECK(player.Stats().resolveAttempts == 1);
    // Every frame still refuses, and still says WHY: the throttle suppresses the repetition of the
    // engine call, not the answer.
    CHECK(player.Stats().refusedNoSite == 1000);
    CHECK(player.Stats().lastResolve == Resolve::NoVoipCs);
    char line[192];
    CHECK(std::string(player.StatusLine(line, sizeof line)).find("receiver did not resolve") !=
          std::string::npos);

    // A different game space is a different question, so it is asked -- exactly once more.
    e.Poke(e.netGame, facts::NETGAME_CS_SPACE_OFF, other.gameDecoy.data());
    at += kTickUs;
    player.Advance(at, 1.0f, e.netGame.data());
    CHECK(w3c::g_lookupCalls == 2);
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    CHECK(w3c::g_lookupCalls == 2);
    CHECK(player.Stats().resolveAttempts == 2);

    // ★ AND THE THROTTLE IS NOT A GIVE-UP. A component system that registers late moves neither the
    // netGame nor the space, so only the cadence can find it: one more call after the retry span,
    // and one only. Stepped under the forward-jump threshold so that no step is a seek.
    for (int i = 0; i < 55; ++i) {
        at += 400000;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    CHECK(w3c::g_lookupCalls == 3);
    CHECK(player.Stats().discontinuities == 0);

    w3c::ClearLookup();
}

// ★ MAPPED-HOLDING-NULL AND UNMAPPED ARE DIFFERENT EPOCHS, and the difference is load-bearing. The
// space pointer reads null in BOTH states, so the readability term is the only thing telling them
// apart: an epoch key without it would see one epoch where there are three, and a session whose
// netGame page becomes readable would wait out the whole floor before anyone asked again. The flip
// is made with the page protection the product's own probe reads (VirtualQuery), not with a fixture
// flag, so what is under test is the real refusal and not a stand-in for it.
TEST_CASE("a space field that becomes unmapped, and mapped again, is re-asked exactly once each") {
    const wchar_t* path = L"w3c_voip_readable_flip.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    e.Build(1);
    e.InstallLookup();

    // The netGame needs pages of its own: a vector's belong to the allocator and to whatever else
    // shares them, and un-mapping one of those would take the heap with it.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t pageBytes = si.dwPageSize;
    const size_t bytes = facts::NETGAME_CS_SPACE_OFF + 2 * pageBytes;
    uint8_t* netGame =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(netGame != nullptr);
    memcpy(netGame, e.netGame.data(), e.netGame.size());
    uint8_t* const spacePage = netGame + (facts::NETGAME_CS_SPACE_OFF & ~(pageBytes - 1));
    void* const noSpace = nullptr;
    memcpy(netGame + facts::NETGAME_CS_SPACE_OFF, &noSpace, sizeof noSpace);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    w3c::g_lookupCalls = 0;

    const int64_t kTickUs = 16667;
    int64_t at = static_cast<int64_t>(t0);
    REQUIRE(player.Advance(at, 1.0f, netGame) == 0);   // establishes the span
    auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) {
            at += kTickUs;
            player.Advance(at, 1.0f, netGame);
        }
    };

    ticks(200);
    CHECK(player.Stats().resolveAttempts == 1);
    CHECK(player.Stats().lastResolve == Resolve::NoGameSpace);

    DWORD wasProtected = 0;
    REQUIRE(VirtualProtect(spacePage, pageBytes, PAGE_NOACCESS, &wasProtected) != 0);
    ticks(200);
    CHECK(player.Stats().resolveAttempts == 2);
    CHECK(player.Stats().lastResolve == Resolve::GameSpaceUnreadable);

    REQUIRE(VirtualProtect(spacePage, pageBytes, PAGE_READWRITE, &wasProtected) != 0);
    ticks(200);
    CHECK(player.Stats().resolveAttempts == 3);
    CHECK(player.Stats().lastResolve == Resolve::NoGameSpace);

    // The engine was never entered: there was never a space to look a system up in, which is also
    // why the lookup counter cannot be this arm's oracle and resolveAttempts is.
    CHECK(w3c::g_lookupCalls == 0);
    // Six hundred ticks is ten seconds of playhead, well under the floor, so all three attempts are
    // the epoch key moving rather than the cadence expiring.
    const int64_t floorUs = 20 * 1000000;
    CHECK(at - static_cast<int64_t>(t0) < floorUs);

    VirtualFree(netGame, 0, MEM_RELEASE);
    w3c::ClearLookup();
}

// * THE ONE CHANGE THE POINTER TERMS CANNOT SEE. A game space torn down and rebuilt AT THE SAME
// ADDRESS under the same netGame reads identically in {netGame, space, readable} -- and a heap
// allocator handing back the block it just freed makes that ordinary rather than unlikely. The held
// VoipSite would then keep injecting replayed voice through the FREED image's receiver, which is a
// write through a dangling component system on every voice frame for the rest of the session.
//
// The rebuild here is the real thing rather than a flag: the space keeps its address and its table
// pointer is re-aimed at a second engine's system table, so the fixture's lookup -- which reads
// whatever the object carries at CS.TABLE_OFF -- answers the NEW receiver. The only thing that
// moves with it is the bind epoch, which is what entity_module advances when the space's
// system-table digest changes (entity.h:310-317).
//
// The oracle is the pointer VOIP.RECEIVE is HANDED, recorded at the seam: a player serving the old
// site is indistinguishable from one that re-resolved in every frame counter, and differs here by
// which component system a stranger's voice is injected through.
TEST_CASE("a game space rebuilt at the same address is a new epoch, and the old receiver is dropped") {
    const wchar_t* path = L"w3c_voip_same_address_rebuild.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    // Two complete engines. `before` owns the netGame and the space the player ever sees; `after`
    // exists only to own a second receiver system with its own pools, so that "the old one" and
    // "the new one" are two distinct addresses and not one object mutated in place.
    FakeEngine before;
    FakeEngine after;
    MakeEngineWithTalker(before, 6, 0x0003);
    after.Build(1);
    after.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));
    REQUIRE(before.cs.data() != after.cs.data());

    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);

    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &before);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    w3c::g_lookupCalls = 0;

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, before.netGame.data()) == 0);   // establishes the span
    at += 1;
    auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) {
            at += kTickUs;
            player.Advance(at, 1.0f, before.netGame.data());
        }
    };

    ticks(100);
    CHECK(w3c::g_lookupCalls == 1);
    CHECK(player.Stats().resolveAttempts == 1);
    CHECK(player.Stats().sitesDroppedOnEpoch == 0);
    REQUIRE(g_call.calls > 0);
    CHECK(g_call.receiver == before.cs.data());
    const int callsBefore = g_call.calls;

    // The rebuild. The space keeps its address; its table now leads to the other receiver, and the
    // engine's bind epoch moves the way EntityBind moves it -- by one, on that edge and no other.
    before.Poke(before.gameSpace, uint32_t(FakeEngine::kFixtureCsTableOff), &after.table);
    g_epochTerm = BoundEpoch(8);

    ticks(100);
    CHECK(player.Stats().sitesDroppedOnEpoch == 1);
    CHECK(player.Stats().resolveAttempts == 2);
    CHECK(w3c::g_lookupCalls == 2);
    // * THE OLD SITE WAS NEVER SERVED AGAIN. Every call after the rebuild carries the new receiver,
    // and there WERE calls after the rebuild -- an arm that only checked the last one would pass on
    // a player that served the freed system ninety-nine times and then re-resolved.
    CHECK(g_call.calls > callsBefore);
    CHECK(g_call.receiver == after.cs.data());
    // No seek: OnDiscontinuity would have dropped the site for reasons that have nothing to do with
    // the epoch, and the whole span sits far below the retry floor, so neither could have done it.
    CHECK(player.Stats().discontinuities == 0);
    CHECK(at - static_cast<int64_t>(t0) < 20 * 1000000);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    w3c::ClearLookup();
}

// * A "NOT YET" ANSWER MUST NOT BE HELD FOR THE SESSION. A component system that registers late
// moves neither the netGame, nor the space, nor the bind epoch -- entity_module advances the epoch
// on a REBIND, not on a system appearing inside a space it is already bound to -- so the key cannot
// see the answer change. Forward playback produces no seek either, and a seek is the only other
// thing that drops the throttle. Without the floor cadence the receiver that appears one second
// after the space does is never found, and the replay plays out silent behind a status line that
// was right once and has been stale ever since (AUD-F3D F3D-1, next door in W3-A).
//
// So this arm asserts the RECOVERY rather than the throttle: voice reaches the engine after the
// late registration, with no seek anywhere in it.
TEST_CASE("a receiver that registers late is found by the cadence, with no seek") {
    const wchar_t* path = L"w3c_voip_late_registration.echodemo";
    const uint64_t t0 = 1000000;
    // Five frames a second for 44 s: enough chapter to walk past the 20 s floor twice.
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithTalker(e, 6, 0x0003);
    // The space is up and its table is real; the receiver has simply not registered into it yet.
    // That is NoVoipCs -- the engine was entered, sorted its table and answered "not here" -- and it
    // is the outcome a session produces for the whole of its loading window.
    const uint64_t realSymbol = e.table.symbols[0];
    e.table.symbols[0] = 0;

    g_epochTerm = BoundEpoch(3);
    SetBindEpochSource(&TestBindEpoch);

    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    w3c::g_lookupCalls = 0;

    int64_t at = static_cast<int64_t>(t0);
    REQUIRE(player.Advance(at, 1.0f, e.netGame.data()) == 0);   // establishes the span
    // Stepped at 400 ms -- under kMaxContiguousSpanUs, so no step is ever classified as a jump.
    auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) {
            at += 400000;
            player.Advance(at, 1.0f, e.netGame.data());
        }
    };

    ticks(25);   // 10 s of playhead: inside the floor, so exactly one engine call
    CHECK(w3c::g_lookupCalls == 1);
    CHECK(player.Stats().lastResolve == Resolve::NoVoipCs);
    CHECK(g_call.calls == 0);

    // The system registers. Nothing in the key moves: same netGame, same space, same epoch.
    e.table.symbols[0] = realSymbol;

    // Two more floors' worth of playhead. The cadence is the only thing that can find it.
    ticks(100);
    CHECK(player.Stats().lastResolve == Resolve::Ok);
    CHECK(g_call.calls > 0);
    CHECK(g_call.receiver == e.cs.data());
    // * It recovered without a jump, which is the point: a replay played forward never produces one.
    CHECK(player.Stats().discontinuities == 0);
    // And the recovery cost ONE more engine call, not one per frame: the cadence is a floor, not a
    // licence to re-enter the sort now that the answer has changed.
    CHECK(w3c::g_lookupCalls == 2);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    w3c::ClearLookup();
}


// =================================================================================================
// The owned-slot abandon: it had no control at all (F3F-1), it ran only on the 1.0x
// delivery path (F3F-4), and the bind epoch alone cannot see the rebuild it was added for (F3F-3).
// =================================================================================================

// ★ THE SLOT PATH, NOT THE SITE PATH. `sitesDroppedOnEpoch` says we stopped SERVING a receiver;
// `abandonedOnSessionChange` says we let go of pool slots the engine's Connect gave us. They are
// different state with different consequences, and only the first had an arm (AUD-F3F F3F-1): the
// epoch term of SpaceReplaced could be deleted outright and the whole suite stayed green.
//
// The rule this arm pins: when the space is replaced under a binding, the slots are DROPPED and
// never Disconnected, because Disconnect would be a write through the freed receiver -- so
// BACKLOG-1 O3. TryBind now asks livesafety before it claims: VOIP.CONNECT builds a decoder in the
// LIVE receiver's pool, whose other entries are real people in the owner's public match, and the
// lookup on the way there sorts the gamespace table in place. Both arms of the refusal, then the
// contained control -- without which this would pass against a player that never binds at all.
//
// The release side is deliberately NOT gated: owned_ can only hold what this claim put there, so
// an uncontained process has nothing to give back and the balance is preserved by construction.
TEST_CASE("BACKLOG-1 O3: an uncontained process claims no talker slot") {
    const wchar_t* path = L"w3c_voip_owned_abandon.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine live;
    MakeEngineWithFreeInstance(live, 6, 0x0003);
    ArmSeam(live);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &live);

    const bool arms[2][2] = {{true, false}, {false, false}};
    for (const auto& arm : arms) {
        replay::livesafety::SetForTest(arm[0], arm[1]);
        VoipPlayer player;
        REQUIRE(player.Load(path, nullptr, nullptr));
        int64_t at = static_cast<int64_t>(t0) - 1;
        for (int i = 0; i < 101; ++i) {
            player.Advance(at, 1.0f, live.netGame.data());
            at += 20000;
        }
        CHECK(player.Stats().bound == 0);
        CHECK(player.Stats().claimRefusedUncontained > 0);
        CHECK(g_seam.connects == 0);
        CHECK(player.Stats().released == 0);
        player.Unload(live.netGame.data());
        CHECK(g_seam.disconnects == 0);   // nothing was claimed, so nothing is given back
    }

    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    VoipPlayer control;
    REQUIRE(control.Load(path, nullptr, nullptr));
    int64_t at = static_cast<int64_t>(t0) - 1;
    for (int i = 0; i < 101; ++i) {
        control.Advance(at, 1.0f, live.netGame.data());
        at += 20000;
    }
    CHECK(control.Stats().bound == 1);
    CHECK(g_seam.connects == 1);
    CHECK(control.Stats().claimRefusedUncontained == 0);
    control.Unload(live.netGame.data());
}

// `released` and `releaseFailed` must both stay at zero while `abandonedOnSessionChange` moves.
TEST_CASE("owned pool slots are abandoned, never disconnected, when the space is replaced") {
    const wchar_t* path = L"w3c_voip_owned_abandon.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine before;
    FakeEngine after;
    MakeEngineWithFreeInstance(before, 6, 0x0003);
    after.Build(1);
    after.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));
    REQUIRE(before.cs.data() != after.cs.data());

    ArmSeam(before);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &before);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, before.netGame.data()) == 0);   // establishes the span
    at += 1;
    auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) {
            at += kTickUs;
            player.Advance(at, 1.0f, before.netGame.data());
        }
    };

    ticks(100);
    // The binding is real: the engine's Connect was called once and the player recorded the slot.
    REQUIRE(player.Stats().bound == 1);
    REQUIRE(g_seam.connects == 1);
    CHECK(g_seam.lastCs == before.cs.data());
    // And a hundred ticks with nothing moving did NOT abandon it. This half is the arm that keeps
    // the next one honest: a SpaceReplaced that answered true unconditionally would pass every
    // assertion below and fail here.
    CHECK(player.Stats().abandonedOnSessionChange == 0);

    // The rebuild, modelled the way entity_module sees one: the table block stays where it is and
    // its CONTENTS change -- which is exactly what moves the `entries` digest and with it the bind
    // epoch. The table BASE is deliberately left alone, so this arm turns on the epoch term and on
    // nothing else; the base is the subject of the last arm in this file.
    before.table.systems[0] = after.cs.data();
    before.table.systems[1] = after.cs.data();
    g_epochTerm = BoundEpoch(8);

    ticks(100);
    CHECK(player.Stats().abandonedOnSessionChange == 1);
    CHECK(player.Stats().released == 0);
    CHECK(player.Stats().releaseFailed == 0);
    CHECK(g_seam.disconnects == 0);
    // No seek did it, and the whole span sits far under the retry floor.
    CHECK(player.Stats().discontinuities == 0);
    CHECK(at - static_cast<int64_t>(t0) < 20 * 1000000);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ THE NEGATIVE THE COMMENT DEMANDS. SpaceReplaced's own text says a space field that goes unmapped
// and comes back is the SAME space and must not cost us the slots -- abandoning there leaves them
// Connect'd in a live session with nobody to release them. Without this arm, "abandon on any key
// move" would pass every other assertion in this file.
TEST_CASE("a space field that flaps unmapped does not cost us the slots we own") {
    const wchar_t* path = L"w3c_voip_flap_keeps_slots.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithFreeInstance(e, 6, 0x0003);

    // The netGame needs pages of its own, for the reason the readable-flip arm above gives.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t pageBytes = si.dwPageSize;
    const size_t bytes = facts::NETGAME_CS_SPACE_OFF + 2 * pageBytes;
    uint8_t* netGame =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(netGame != nullptr);
    memcpy(netGame, e.netGame.data(), e.netGame.size());
    uint8_t* const spacePage = netGame + (facts::NETGAME_CS_SPACE_OFF & ~(pageBytes - 1));

    ArmSeam(e);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, netGame) == 0);
    at += 1;
    auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) {
            at += kTickUs;
            player.Advance(at, 1.0f, netGame);
        }
    };

    ticks(50);
    REQUIRE(player.Stats().bound == 1);

    DWORD wasProtected = 0;
    REQUIRE(VirtualProtect(spacePage, pageBytes, PAGE_NOACCESS, &wasProtected) != 0);
    ticks(50);
    REQUIRE(VirtualProtect(spacePage, pageBytes, PAGE_READWRITE, &wasProtected) != 0);
    ticks(50);

    // The site was dropped and re-resolved across the flap -- that is the cheap mistake and it is
    // supposed to happen -- but the SLOTS were kept, and nothing was Disconnected.
    CHECK(player.Stats().abandonedOnSessionChange == 0);
    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().sitesDroppedOnEpoch > 0);

    // And teardown through the live space still releases them, which is what keeping them was for.
    player.Unload(netGame);
    CHECK(player.Stats().abandonedOnSessionChange == 0);
    CHECK(g_seam.disconnects == 1);
    CHECK(g_seam.lastCs == e.cs.data());
    CHECK(player.Stats().released == 1);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    VirtualFree(netGame, 0, MEM_RELEASE);
    w3c::ClearLookup();
}

// ★ RATE GATES WHAT WE DELIVER; IT MUST NOT GATE WHAT WE NOTICE (AUD-F3F F3F-4). The abandon used to
// live inside `if (realTime)`, which sits below the `toUs <= fromUs` return -- so a space replaced
// while the transport was paused was never observed, and the slots went on pointing into a receiver
// that had gone until a 1.0x frame happened to arrive. The engine keeps ticking while paused, and
// voip_module's VoipTick calls Advance on every one of those ticks with no rate gate of its own.
//
// `skippedNoStep` is asserted deliberately: it is the proof that these ticks really did take the
// paused early return, so an arm that observed the space only after it would have no cover here.
TEST_CASE("a space replaced while the transport is paused is seen on the tick, not at 1.0x") {
    const wchar_t* path = L"w3c_voip_paused_replace.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine before;
    FakeEngine after;
    MakeEngineWithFreeInstance(before, 6, 0x0003);
    after.Build(1);
    after.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));

    ArmSeam(before);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &before);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, before.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, before.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);
    const uint32_t skippedBefore = player.Stats().skippedNoStep;

    // Paused. The playhead stops dead and the rate is not 1.0x; the tick does not. The rebuild is
    // the contents-change one again, so the epoch term is what has to see it.
    before.table.systems[0] = after.cs.data();
    before.table.systems[1] = after.cs.data();
    g_epochTerm = BoundEpoch(8);
    for (int i = 0; i < 20; ++i) {
        CHECK(player.Advance(at, 0.0f, before.netGame.data()) == 0);
    }

    CHECK(player.Stats().skippedNoStep >= skippedBefore + 20);   // the early return really was taken
    CHECK(player.Stats().abandonedOnSessionChange == 1);
    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().discontinuities == 0);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ AND THE TEARDOWN THAT NEVER GOES THROUGH A TICK AT ALL (AUD-F3F F3F-4). SetVoipDemoPath(nullptr)
// -- the browser picking another capture -- calls Unload() straight from the caller, and VoipShutdown
// does the same. So there is a window with no Advance in it, and ReleaseOwned's only guard was
// `boundSite_.Valid()`, which is a non-null pointer test that a freed receiver passes.
//
// No tick happens here between the replacement and the Unload, deliberately: this arm can only be
// saved by the check inside ReleaseOwned, so it is that check's control and not the one above's.
TEST_CASE("Unload after a replacement never Disconnects through the receiver that has gone") {
    const wchar_t* path = L"w3c_voip_unload_after_replace.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine before;
    FakeEngine after;
    MakeEngineWithFreeInstance(before, 6, 0x0003);
    after.Build(1);
    after.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));

    ArmSeam(before);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &before);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, before.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, before.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);

    // The space is replaced and the old receiver's image is freed -- poisoned rather than returned
    // to the allocator, so that a call through it is detectable instead of merely undefined.
    before.table.systems[0] = after.cs.data();
    before.table.systems[1] = after.cs.data();
    memset(before.cs.data(), 0xDD, before.cs.size());
    g_freedCs = before.cs.data();

    // ★ THE netGame HANDED TO Unload IS THE ONE THIS SCENARIO'S SESSION ACTUALLY HOLDS (AUD-F3K
    // F3K-1). The replacement above is three edits INSIDE `before`'s own table block: the space
    // pointer at NETGAME.CS_SPACE_OFF does not move, the block base does not move, and the bind
    // epoch is deliberately not moved either -- it is a snapshot entity_module::Tick maintains and
    // this arm's premise is that no tick happens, so a fixture that hand-moved it would be supplying
    // the one term the scenario forbids. EVERY term of the key therefore reads identical, the
    // netGame included: this is entity.h:243's third case, "the same block carrying a rebuilt system
    // table", and no term of the key can tell it apart. The previous revision of this arm handed
    // Unload a second, unrelated engine's netGame -- which moves the key's first term, which its own
    // fixture never produced, and which certified a guard that was inert on the case staged here.
    //
    // What saves it is the identity re-ask: the lookup is re-run on this live space and answers
    // after.cs, which is not the receiver we bound, so the slots are dropped unwritten.
    player.Unload(before.netGame.data());

    CHECK(g_seam.touchedFreed == false);
    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().abandonedOnSessionChange == 1);
    // Not one term of the key moved, so this counter is the whole evidence: it moves only where the
    // re-ask itself decided, and it is zero on every arm the key can already see.
    CHECK(player.Stats().abandonedOnIdentity == 1);
    CHECK(player.Stats().released == 0);
    CHECK(player.Stats().releaseFailed == 0);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ THE OTHER SIDE OF THAT COIN, and the arm that stops the re-ask from being a blanket refusal.
// The table is rebuilt in place exactly as above -- contents changed, the receiver moved to a
// different slot behind a system that was not there before -- and the lookup hands back the SAME
// receiver. Nothing was freed, so the slots must be given back. A guard that abandoned on any
// rebuild, or that re-asked and ignored the answer, passes every assertion in the arm above and
// fails here: it would leave up to kMaxOwned slots Connected in a LIVE session with nobody left to
// release them, which is the leak ReleaseOwned exists to prevent.
TEST_CASE("a rebuilt table that answers the same receiver still releases the slots we own") {
    const wchar_t* path = L"w3c_voip_rebuild_same_receiver.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine before;
    FakeEngine after;
    MakeEngineWithFreeInstance(before, 6, 0x0003);
    after.Build(1);
    REQUIRE(before.cs.data() != after.cs.data());

    ArmSeam(before);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &before);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, before.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, before.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);

    // The receiver we bound is still in this table, second now, behind a stranger. A cached ANSWER
    // is stale here while the receiver itself is not -- which is the case the re-ask has to resolve
    // by comparing rather than by refusing.
    const uint64_t kStrangerCsSymbol = 0x1111222233334444ull;
    before.table.count = 2;
    before.table.symbols[0] = kStrangerCsSymbol;
    before.table.systems[0] = after.cs.data();
    before.table.symbols[1] = voip_facts::kReceiverCsSymbol;
    before.table.systems[1] = before.cs.data();
    // A Disconnect through the STRANGER is the mistake this arm hunts, so that is what is marked.
    g_freedCs = after.cs.data();

    player.Unload(before.netGame.data());

    CHECK(g_seam.touchedFreed == false);
    CHECK(player.Stats().abandonedOnSessionChange == 0);
    CHECK(player.Stats().abandonedOnIdentity == 0);
    CHECK(g_seam.disconnects == 1);
    CHECK(g_seam.lastCs == before.cs.data());
    CHECK(player.Stats().released == 1);
    CHECK(player.Stats().releaseFailed == 0);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ AND THE TABLE THE RE-ASK MUST NOT ENTER. Everything here is intact -- the receiver is still the
// one we bound and the lookup would answer it -- but the block's flags bit 0 says the engine's
// lookup SORTS it in place before searching, which is an engine write on our thread into a block
// that a teardown is exactly the wrong moment to touch (ARCH-10 S7, tableguard SortPending). So the
// table is judged BEFORE the engine is entered, the lookup is never made, and what could not be
// verified is not written. Delete the CheckSpaceTable call and this arm releases instead.
// ★ AND SortPending IS THE ONE REFUSAL THAT MEANS "NOT YET" (AUD-F3L F3L-2). Bit 0 is set because a
// component system registered since the last lookup and the next lookup by anyone clears it. The
// abandon it used to trigger was PERMANENT on that transient fact, and nothing reclaimed the slots:
// BindTalker answers AlreadyBound for one we let go of and TryBind never records it, so up to
// kMaxOwned receiver instances stayed Connected for the life of the gamespace and a real player met
// NoFreeInstance. The claim is parked instead and redeemed on a later tick.
// (Name deliberately free of commas: doctest's -tc= filter is comma-separated and cannot select a
// case whose name contains one -- AUD-F3L section 1.)
TEST_CASE("a teardown meeting a sort-pending table is parked and released on a later tick") {
    const wchar_t* path = L"w3c_voip_unload_sort_pending.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithFreeInstance(e, 6, 0x0003);

    ArmSeam(e);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);

    // The only change, and it moves no term of the key: the key reads the block's BASE, not its
    // flags.
    e.SetTableSortPending(true);

    player.Unload(e.netGame.data());

    // Nothing was written and nothing was thrown away: the claim is held.
    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().released == 0);
    CHECK(player.Stats().releaseFailed == 0);
    CHECK(player.Stats().releaseDeferred == 1);
    CHECK(player.Stats().abandonedOnSessionChange == 0);
    CHECK(player.Stats().abandonedOnIdentity == 0);
    CHECK(player.Stats().abandonedOnSortPending == 0);

    // A retry while the bit is STILL set costs one attempt and writes nothing -- the arm that stops
    // "defer" from becoming "release anyway one tick later".
    player.RetryPendingRelease(e.netGame.data());
    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().releaseRetries == 1);
    CHECK(player.Stats().released == 0);

    // The next lookup anyone makes clears bit 0. Now the slot comes back, through the receiver we
    // bound and the pool index it issued.
    e.SetTableSortPending(false);
    player.RetryPendingRelease(e.netGame.data());
    CHECK(g_seam.disconnects == 1);
    CHECK(g_seam.lastCs == e.cs.data());
    CHECK(g_seam.touchedFreed == false);
    CHECK(player.Stats().released == 1);
    CHECK(player.Stats().releaseFailed == 0);
    CHECK(player.Stats().abandonedOnSessionChange == 0);

    // And the claim is gone: a second retry is a no-op rather than a second Disconnect.
    player.RetryPendingRelease(e.netGame.data());
    CHECK(g_seam.disconnects == 1);
    CHECK(player.Stats().releaseRetries == 2);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ THE ARM WHERE A REFUSAL IS THE DECIDING TERM (AUD-F3K F3K-2). The netGame is the SAME pointer
// every Advance was handed, so SpaceReplaced's first term is silent; the space slot is unmapped, so
// its other two terms are left unread and it returns false. The tick path's rule for that state is
// the opposite one and stays right: a space field that goes unmapped and comes back is the same
// space and must not cost us the slots (the flap arm above). A TEARDOWN is where the two part
// company -- it is about to WRITE through a cached receiver pointer and cannot read the space that
// would confirm it -- and only the identity re-ask can say so. Holding a claim across a flap and
// refusing to write on an unverifiable one are one rule seen from both ends: the claim is cheap, the
// write is not.
TEST_CASE("Unload through the bound netGame whose space has gone unmapped abandons") {
    const wchar_t* path = L"w3c_voip_unload_space_unmapped.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithFreeInstance(e, 6, 0x0003);

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t pageBytes = si.dwPageSize;
    const size_t bytes = facts::NETGAME_CS_SPACE_OFF + 2 * pageBytes;
    uint8_t* netGame =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(netGame != nullptr);
    memcpy(netGame, e.netGame.data(), e.netGame.size());
    uint8_t* const spacePage = netGame + (facts::NETGAME_CS_SPACE_OFF & ~(pageBytes - 1));

    ArmSeam(e);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, netGame) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, netGame);
    }
    REQUIRE(player.Stats().bound == 1);
    REQUIRE(player.Stats().abandonedOnSessionChange == 0);

    DWORD wasProtected = 0;
    REQUIRE(VirtualProtect(spacePage, pageBytes, PAGE_NOACCESS, &wasProtected) != 0);
    player.Unload(netGame);
    REQUIRE(VirtualProtect(spacePage, pageBytes, PAGE_READWRITE, &wasProtected) != 0);

    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().released == 0);
    CHECK(player.Stats().releaseFailed == 0);
    CHECK(player.Stats().abandonedOnSessionChange == 1);
    CHECK(player.Stats().abandonedOnIdentity == 1);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    VirtualFree(netGame, 0, MEM_RELEASE);
    w3c::ClearLookup();
}

// The other order, and the cheap one: a tick DOES land between the replacement and the Unload, so
// ObserveSpace has already dropped the claim by the time the teardown runs. The bind epoch moves
// here legitimately -- a tick is what maintains it -- and the point of the arm is that the Unload
// afterwards is a no-op: not a second abandon, and not a write through the receiver that has gone.
TEST_CASE("a replacement observed by a tick leaves Unload nothing to release") {
    const wchar_t* path = L"w3c_voip_tick_then_unload.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine before;
    FakeEngine after;
    MakeEngineWithFreeInstance(before, 6, 0x0003);
    after.Build(1);
    after.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));

    ArmSeam(before);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &before);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, before.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, before.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);

    before.table.systems[0] = after.cs.data();
    before.table.systems[1] = after.cs.data();
    g_epochTerm = BoundEpoch(8);
    memset(before.cs.data(), 0xDD, before.cs.size());
    g_freedCs = before.cs.data();

    // One tick, carrying the netGame voip_module would now be handing Advance.
    at += kTickUs;
    player.Advance(at, 1.0f, after.netGame.data());
    CHECK(player.Stats().abandonedOnSessionChange == 1);
    CHECK(g_seam.disconnects == 0);

    player.Unload(after.netGame.data());

    CHECK(g_seam.touchedFreed == false);
    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().abandonedOnSessionChange == 1);   // counted once, at the tick
    CHECK(player.Stats().released == 0);
    CHECK(player.Stats().releaseFailed == 0);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ THE netGame TERM'S ISOLATOR, and that alone (AUD-F3K F3K-2). The session hands back a
// DIFFERENT netGame pointer whose space slot is also not mapped. Delete `bound.netGame != now.netGame`
// from SpaceReplaced and every other arm here still passes while this one writes through the
// receiver that has gone -- which is what the arm is for. What it does NOT establish is what a
// refusal means: the probe over the space slot runs and refuses, but the netGame term has already
// decided the verdict above it, so that refusal never reaches the answer. The arm that puts a
// refusal in the deciding seat is the same-pointer one above, where the netGame is unmoved and the
// unreadable space is the only thing left that can abandon.
TEST_CASE("Unload with a live netGame that cannot be read abandons instead of releasing") {
    const wchar_t* path = L"w3c_voip_unload_unreadable_live.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine before;
    FakeEngine after;
    MakeEngineWithFreeInstance(before, 6, 0x0003);
    after.Build(1);
    after.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));

    ArmSeam(before);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &before);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, before.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, before.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);

    before.table.systems[0] = after.cs.data();
    before.table.systems[1] = after.cs.data();
    memset(before.cs.data(), 0xDD, before.cs.size());
    g_freedCs = before.cs.data();

    // Reserved and never committed, so the probe over NETGAME.CS_SPACE_OFF refuses rather than
    // faulting -- and it is wide enough to REACH that offset, which a smaller rig would not be.
    void* unreadable =
        VirtualAlloc(nullptr, FakeEngine::kNetGameSize, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(unreadable != nullptr);
    player.Unload(unreadable);
    VirtualFree(unreadable, 0, MEM_RELEASE);

    CHECK(g_seam.touchedFreed == false);
    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().abandonedOnSessionChange == 1);
    CHECK(player.Stats().released == 0);
    CHECK(player.Stats().releaseFailed == 0);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ THE REBUILD THE BIND EPOCH CANNOT SEE (AUD-F3F F3F-3). entity_module advances the epoch only when
// its FNV-1a digest of the system table's contents differs, so a space rebuilt at the same address
// whose table reconstructs identically -- same count, same symbols in the same order, same system
// addresses -- moves the epoch by nothing at all. The epoch is held FIXED across the rebuild here,
// which is that case exactly, and the term that has to catch it is the table block's base pointer:
// the block is reallocated by the rebuild even where its contents digest the same.
TEST_CASE("a rebuild that leaves the bind epoch standing is still caught, by the table block") {
    const wchar_t* path = L"w3c_voip_identical_digest_rebuild.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine before;
    FakeEngine after;
    MakeEngineWithFreeInstance(before, 6, 0x0003);
    after.Build(1);
    after.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));
    REQUIRE(before.cs.data() != after.cs.data());

    ArmSeam(before);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &before);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    w3c::g_lookupCalls = 0;

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, before.netGame.data()) == 0);
    at += 1;
    auto ticks = [&](int n) {
        for (int i = 0; i < n; ++i) {
            at += kTickUs;
            player.Advance(at, 1.0f, before.netGame.data());
        }
    };

    ticks(100);
    REQUIRE(player.Stats().bound == 1);
    REQUIRE(g_call.calls > 0);
    CHECK(g_call.receiver == before.cs.data());
    const int callsBefore = g_call.calls;

    // The rebuild. Same netGame, same space address, and the bind epoch DELIBERATELY UNCHANGED --
    // this is the digest-identical reconstruction, so every term the previous commit had reads the
    // same across it.
    before.Poke(before.gameSpace, uint32_t(FakeEngine::kFixtureCsTableOff), &after.table);

    ticks(100);
    CHECK(player.Stats().abandonedOnSessionChange == 1);
    CHECK(player.Stats().sitesDroppedOnEpoch == 1);
    CHECK(g_seam.disconnects == 0);
    CHECK(g_call.calls > callsBefore);
    CHECK(g_call.receiver == after.cs.data());
    CHECK(player.Stats().discontinuities == 0);
    CHECK(at - static_cast<int64_t>(t0) < 20 * 1000000);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ THE RECEIVER SURVIVED AND THE SLOT DID NOT (AUD-F3L F3L-1). Nothing here is freed, nothing is
// rebuilt and every pointer term agrees: SameReceiverStillLive answers YES, because the receiver
// really is the one we bound. What changed is one level down -- the engine handed our pool slot to
// somebody else, which a player leaving and another joining does without touching the system
// object at all. The cached poolIndex is then an address into a stranger, and voipsite.h:20-26 says
// the engine validates none of it: Disconnect would succeed into whoever holds that slot now and
// kick a LIVE talker out of voice in the owner's real match. Move the pre-check back after the
// write -- which is exactly where it used to sit, as a post-hoc read-back -- and this arm reds.
TEST_CASE("a pool slot that changed hands is not disconnected even though the receiver is ours") {
    const wchar_t* path = L"w3c_voip_pool_churned.echodemo";
    const uint64_t t0 = 1000000;
    const uint32_t ourId = MakeUserId(6, 0x0003);
    const std::vector<Frame> frames = MakeClockFrames(ourId, t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithFreeInstance(e, 6, 0x0003);

    ArmSeam(e);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);

    // The slot we own, addressed the way the release will address it.
    uint32_t handle = 0;
    const uint8_t* ci = nullptr;
    Resolve why = Resolve::Ok;
    REQUIRE(HandleOf(e.Site(), ourId, &handle, &ci, &why));
    const uint16_t ourPool = static_cast<uint16_t>(handle);

    // The churn: that instance now carries a different live player. This is the ONLY change, and no
    // term of the space key and no pointer inside the receiver moves with it.
    const uint32_t stranger = MakeUserId(9, 0x0011);
    e.Poke(e.ci, e.CiOff(DenseOfPool(e, ourPool), voip_facts::kCiUserIdOff), stranger);

    player.Unload(e.netGame.data());

    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().abandonedOnPool == 1);
    CHECK(player.Stats().released == 0);
    // Not a leak and not a failure: the write was refused, so releaseFailed must stay clear or the
    // one disposition that says a live talker was SPARED is buried in the one that says we leaked.
    CHECK(player.Stats().releaseFailed == 0);
    CHECK(player.Stats().abandonedOnSessionChange == 0);

    // And the stranger still holds the slot, which is the fact the counter is about.
    uint32_t stillTheirs = 0;
    REQUIRE(HandleOf(e.Site(), stranger, &handle, &ci, &why));
    REQUIRE(ci != nullptr);
    memcpy(&stillTheirs, ci + voip_facts::kCiUserIdOff, sizeof(stillTheirs));
    CHECK(stillTheirs == stranger);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ THE DEFERRAL IS BOUNDED, AND THE ABANDON IS NAMED (AUD-F3L F3L-2). A table that never settles
// must not turn a teardown into a per-frame engine lookup, so the retry has a budget; when it runs
// out the claim is abandoned under its OWN counter, because "the table was busy for 160ms" and "the
// receiver has been freed" are different facts and one number would hide the recoverable one.
TEST_CASE("a sort-pending table that never settles abandons under its own name") {
    const wchar_t* path = L"w3c_voip_sort_pending_forever.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    FakeEngine e;
    MakeEngineWithFreeInstance(e, 6, 0x0003);

    ArmSeam(e);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));

    const int64_t kTickUs = 20000;
    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);

    e.SetTableSortPending(true);
    player.Unload(e.netGame.data());
    REQUIRE(player.Stats().releaseDeferred == 1);

    for (int i = 0; i < 32; ++i) {
        player.RetryPendingRelease(e.netGame.data());
    }

    // Bounded: the retry stopped costing attempts long before the 32 ticks ran out.
    CHECK(player.Stats().releaseRetries == 8);
    CHECK(player.Stats().abandonedOnSortPending == 1);
    CHECK(player.Stats().abandonedOnSessionChange == 1);
    CHECK(player.Stats().abandonedOnIdentity == 0);
    CHECK(g_seam.disconnects == 0);
    CHECK(g_seam.touchedFreed == false);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

// ★ THE TEARDOWN THAT ENTERS THE ENGINE NOWHERE (AUD-F3L F3L-3). VoipShutdown is reached only from
// modreg::ShutdownAll(), which bootstrap.cpp:463-468 calls only inside `if (processTerminating)`:
// the loader lock at DLL_PROCESS_DETACH, every other thread already killed wherever it stood. A
// lookup sorts a gamespace table in place and Disconnect writes into a receiver whose owning
// threads are dead -- and the release buys nothing, because nothing will read that pool again. The
// oracle is counted at the seam rather than asserted about the code: g_lookupCalls and disconnects.
TEST_CASE("the terminating teardown makes no engine call while the ordinary one still releases") {
    const wchar_t* path = L"w3c_voip_terminating_teardown.echodemo";
    const uint64_t t0 = 1000000;
    const std::vector<Frame> frames = MakeClockFrames(MakeUserId(6, 0x0003), t0, 220);
    REQUIRE(WriteVoipDemo(path, frames, t0));

    const int64_t kTickUs = 20000;

    SUBCASE("terminating: no lookup, no Disconnect, and the claim is counted as dropped") {
        FakeEngine e;
        MakeEngineWithFreeInstance(e, 6, 0x0003);
        ArmSeam(e);
        g_epochTerm = BoundEpoch(7);
        SetBindEpochSource(&TestBindEpoch);
        g_call = RecordedCall{};
        VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

        VoipPlayer player;
        REQUIRE(player.Load(path, nullptr, nullptr));
        int64_t at = static_cast<int64_t>(t0) - 2;
        REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
        at += 1;
        for (int i = 0; i < 100; ++i) {
            at += kTickUs;
            player.Advance(at, 1.0f, e.netGame.data());
        }
        REQUIRE(player.Stats().bound == 1);

        const int lookupsBefore = w3c::g_lookupCalls;
        player.UnloadAtTermination();

        CHECK(w3c::g_lookupCalls == lookupsBefore);
        CHECK(g_seam.disconnects == 0);
        CHECK(g_seam.touchedFreed == false);
        CHECK(player.Stats().abandonedAtTermination == 1);
        CHECK(player.Stats().released == 0);
        CHECK(player.Stats().releaseFailed == 0);

        VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
        SetBindEpochSource(nullptr);
        DisarmSeam();
        w3c::ClearLookup();
    }

    SUBCASE("not terminating: the same state re-asks and gives the slot back") {
        FakeEngine e;
        MakeEngineWithFreeInstance(e, 6, 0x0003);
        ArmSeam(e);
        g_epochTerm = BoundEpoch(7);
        SetBindEpochSource(&TestBindEpoch);
        g_call = RecordedCall{};
        VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

        VoipPlayer player;
        REQUIRE(player.Load(path, nullptr, nullptr));
        int64_t at = static_cast<int64_t>(t0) - 2;
        REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
        at += 1;
        for (int i = 0; i < 100; ++i) {
            at += kTickUs;
            player.Advance(at, 1.0f, e.netGame.data());
        }
        REQUIRE(player.Stats().bound == 1);

        const int lookupsBefore = w3c::g_lookupCalls;
        player.Unload(e.netGame.data());

        // The discriminating half: the two paths differ by exactly one lookup and one Disconnect,
        // so a terminating path that quietly kept the re-ask would be indistinguishable without it.
        CHECK(w3c::g_lookupCalls == lookupsBefore + 1);
        CHECK(g_seam.disconnects == 1);
        CHECK(g_seam.lastCs == e.cs.data());
        CHECK(player.Stats().released == 1);
        CHECK(player.Stats().abandonedAtTermination == 0);

        VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
        SetBindEpochSource(nullptr);
        DisarmSeam();
        w3c::ClearLookup();
    }
}

// ★ A PARKED RELEASE MUST NOT DISCONNECT THE INSTANCE THE NEW CAPTURE IS SPEAKING THROUGH
// (AUD-F3M F3M-1). Parking leaves the instance CONNECTED under the live user id, so the browser
// switching captures inside a sort-pending window hands capture B the very instance capture A
// parked: BindTalker answers AlreadyBound, B binds nothing of its own, and the retry -- whose every
// term still agrees, because the receiver, the slot and the talker really are unchanged -- then
// disconnects the voice B is routing. Separate arrays keep the two claims from aliasing in memory;
// only moving OWNERSHIP keeps them from naming one instance. Delete the ReclaimFromPending call and
// the first CHECK after the retry reds.
TEST_CASE("a parked release whose instance the new capture took over is dropped not disconnected") {
    const wchar_t* pathA = L"w3c_voip_rebind_parked_a.echodemo";
    const wchar_t* pathB = L"w3c_voip_rebind_parked_b.echodemo";
    const uint64_t t0 = 1000000;
    const uint32_t ourId = MakeUserId(6, 0x0003);
    const std::vector<Frame> frames = MakeClockFrames(ourId, t0, 220);
    REQUIRE(WriteVoipDemo(pathA, frames, t0));
    REQUIRE(WriteVoipDemo(pathB, frames, t0));

    FakeEngine e;
    MakeEngineWithFreeInstance(e, 6, 0x0003);

    ArmSeam(e);
    g_epochTerm = BoundEpoch(7);
    SetBindEpochSource(&TestBindEpoch);
    g_call = RecordedCall{};
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    const int64_t kTickUs = 20000;
    VoipPlayer player;
    REQUIRE(player.Load(pathA, nullptr, nullptr));

    int64_t at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    REQUIRE(player.Stats().bound == 1);

    // The browser picks another capture inside the window: Load's implicit Unload can neither
    // release nor abandon, so it parks -- and the new capture speaks for the same talker.
    e.SetTableSortPending(true);
    REQUIRE(player.Load(pathB, nullptr, e.netGame.data()));
    CHECK(player.Stats().releaseDeferred == 1);
    CHECK(g_seam.disconnects == 0);

    at = static_cast<int64_t>(t0) - 2;
    REQUIRE(player.Advance(at + 1, 1.0f, e.netGame.data()) == 0);
    at += 1;
    for (int i = 0; i < 100; ++i) {
        at += kTickUs;
        player.Advance(at, 1.0f, e.netGame.data());
    }
    // B bound NOTHING of its own and did not even ask: a parked instance is still CONNECTED, so
    // HandleOf resolves it and TryBind is never reached. That is the whole hazard -- without these
    // two lines the case would be about two different instances and could not fail.
    CHECK(player.Stats().bound == 1);
    CHECK(player.Stats().alreadyBound == 0);
    CHECK(player.Stats().injected > 0);

    // The window closes and the retry fires. The instance B is speaking through is untouched and
    // the parked copy is gone.
    e.SetTableSortPending(false);
    player.RetryPendingRelease(e.netGame.data());
    CHECK(g_seam.disconnects == 0);
    CHECK(player.Stats().abandonedOnRebind == 1);
    CHECK(player.Stats().released == 0);
    CHECK(player.Stats().releaseFailed == 0);
    CHECK(player.Stats().abandonedOnSessionChange == 0);
    CHECK(player.Stats().abandonedOnSortPending == 0);

    // ★ AND THE SLOT IS NOT LEAKED, which is what makes this a MOVE and not a second way to drop a
    // claim: B's own teardown gives the instance back, through the receiver we bound and the pool
    // index it issued. A fix that merely forgot the parked entry would leave this at zero.
    player.Unload(e.netGame.data());
    CHECK(g_seam.disconnects == 1);
    CHECK(g_seam.lastCs == e.cs.data());
    CHECK(g_seam.touchedFreed == false);
    CHECK(player.Stats().released == 1);
    CHECK(player.Stats().releaseFailed == 0);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    SetBindEpochSource(nullptr);
    DisarmSeam();
    w3c::ClearLookup();
}

namespace {

bool ShiftByViewer(uint16_t recordedSlot, uint16_t* liveSlot, void*) {
    *liveSlot = static_cast<uint16_t>(recordedSlot + 1);
    return true;
}

bool NobodySeated(uint16_t, uint16_t*, void*) { return false; }

// Two live talkers, at slots 2 and 3, on pool entries 4 and 5.
void MakeEngineWithTwoTalkers(FakeEngine& e) {
    e.Build(2);
    e.SetSlotGeneration(2, 0x0011);
    e.Bind(0, 4, 0x0007, MakeUserId(2, 0x0011));
    e.SetSlotGeneration(3, 0x0022);
    e.Bind(1, 5, 0x0009, MakeUserId(3, 0x0022));
    e.InstallLookup();
}

size_t PlayAll(VoipPlayer& player, FakeEngine& e, uint64_t t0, int ticks, int64_t stepUs) {
    int64_t at = static_cast<int64_t>(t0) - 1;
    size_t injected = player.Advance(at, 1.0f, e.netGame.data());
    for (int i = 0; i < ticks; ++i) {
        at += stepUs;
        injected += player.Advance(at, 1.0f, e.netGame.data());
    }
    return injected;
}

}  // namespace

TEST_CASE("a recorded speaker speaks through the slot its entrant holds in this session") {
    const wchar_t* path = L"w3c_voip_seated.echodemo";
    const uint64_t t0 = 3000000;
    const std::vector<Frame> frames = MakeFrames(MakeUserId(2, 0x0001), t0, 10);
    REQUIRE(WriteVoipDemo(path, frames, t0));
    FakeEngine e;
    MakeEngineWithTwoTalkers(e);
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);

    SUBCASE("mapped: recorded slot 2 is live slot 3") {
        g_call = RecordedCall{};
        VoipPlayer player;
        player.SetLiveSlotMap(&ShiftByViewer, nullptr);
        REQUIRE(player.Load(path, nullptr, nullptr));
        CHECK(PlayAll(player, e, t0, 12, 20000) == frames.size());
        CHECK(g_call.calls == static_cast<int>(frames.size()));
        CHECK(g_call.handle == (5u | (0x0009u << 16)));
        CHECK(player.Stats().refusedNotSeated == 0);
    }
    SUBCASE("unmapped: the recorded slot is taken as the live one, which is the other player") {
        g_call = RecordedCall{};
        VoipPlayer player;
        REQUIRE(player.Load(path, nullptr, nullptr));
        CHECK(PlayAll(player, e, t0, 12, 20000) == frames.size());
        CHECK(g_call.handle == (4u | (0x0007u << 16)));
    }
    SUBCASE("not seated yet: refused and counted, never sent to whoever holds the recorded slot") {
        g_call = RecordedCall{};
        VoipPlayer player;
        player.SetLiveSlotMap(&NobodySeated, nullptr);
        REQUIRE(player.Load(path, nullptr, nullptr));
        CHECK(PlayAll(player, e, t0, 12, 20000) == 0);
        CHECK(g_call.calls == 0);
        CHECK(player.Stats().refusedNotSeated == frames.size());
        char line[192];
        CHECK(std::string(player.StatusLine(line, sizeof line)).find("seated") != std::string::npos);
    }

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    w3c::ClearLookup();
}

TEST_CASE("a tick that crosses a voice chapter boundary delivers the end of the chapter it leaves") {
    const wchar_t* path = L"w3c_voip_boundary.echodemo";
    const uint64_t t0 = 4000000;
    // Frames every 20 ms; the second chapter begins 5 ms after the first one's last frame, so a
    // 22 ms tick straddles the boundary with a frame of the first chapter still undelivered.
    const std::vector<Frame> frames = MakeFrames(MakeUserId(3, 0x0022), t0, 20);
    const std::vector<Frame> first(frames.begin(), frames.begin() + 10);
    const std::vector<Frame> second(frames.begin() + 10, frames.end());
    Manifest m{};
    m.formatMajor = 1;
    m.t0Us = t0;
    m.keyframeIntervalUs = 2000000;
    {
        DemoWriter w;
        REQUIRE(w.Open(path, m));
        for (const std::vector<Frame>* part : {&first, &second}) {
            const std::vector<uint8_t> block = BuildVoipBlock(*part);
            w.BeginChapter(kSectionVoip, part->front().tUs - 5000);
            w.WriteStreamBlock(kStreamVoip, capture::kSchemaVoip, block.data(), block.size(),
                               static_cast<uint32_t>(part->size()),
                               static_cast<uint32_t>(part->size()));
            w.EndChapter(part->back().tUs);
        }
        w.Close();
    }
    FakeEngine e;
    MakeEngineWithTwoTalkers(e);
    VoipPlayer::SetEntryPointForTest(reinterpret_cast<void*>(&FakeReceiveVoip), &e);
    g_call = RecordedCall{};

    VoipPlayer player;
    REQUIRE(player.Load(path, nullptr, nullptr));
    REQUIRE(player.HasVoice());
    CHECK(PlayAll(player, e, t0, 25, 22222) == frames.size());
    CHECK(g_call.calls == static_cast<int>(frames.size()));
    CHECK(player.Stats().chaptersLoaded == 2);
    CHECK(player.Stats().discontinuities == 0);

    VoipPlayer::SetEntryPointForTest(nullptr, nullptr);
    w3c::ClearLookup();
}
