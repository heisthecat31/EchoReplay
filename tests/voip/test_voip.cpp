// W3-C's offline half: the receiver resolution, the capture tap, and the injection decision, all
// driven against a SYNTHETIC component system laid out at the offsets the W3-C fact drop
// decoded out of echovr.exe. No game, no sound card, no launch.
//
// ★ WHY A SYNTHETIC ENGINE RATHER THAN A MOCK OF OUR OWN CODE. The thing that can be wrong here is
// the WALK -- which field holds the count, which map goes which way, which dword is the join key --
// and a mock built from the same beliefs as the code proves only that the beliefs are consistent.
// So the fixture builds memory at the offsets and the REAL walk runs over it, and every arm names
// both an input that makes the check pass and one that makes it fail.

#include <string.h>

#include <string>
#include <vector>

#include "audio/audio_facts.h"
#include "audio/memprobe.h"
#include "audio/voip/voip_facts.h"
#include "audio/voip/voipcapture.h"
#include "audio/voip/voipplay.h"
#include "audio/voip/voipsite.h"
#include "audio/voipformat.h"
#include "capture/capture_types.h"
#include "capture/streamcodec.h"
#include "doctest.h"
#include "fakeengine.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demofile.h"
#include "generated/facts_generated.h"

using namespace replay;
using namespace replay::audio;
using namespace replay::audio::voip;
using w3c::FakeEngine;
using w3c::FakeReceiveVoip;
using w3c::g_call;
using w3c::RecordedCall;

namespace w3c {

RecordedCall g_call;

void FakeReceiveVoip(void* receiver, uint32_t handle, const void* header, const void* payload,
                     uint32_t length) {
    ++g_call.calls;
    g_call.receiver = receiver;
    g_call.handle = handle;
    memcpy(g_call.header, header, sizeof g_call.header);
    g_call.length = length;
    g_call.payload.assign(static_cast<const uint8_t*>(payload),
                          static_cast<const uint8_t*>(payload) + length);
}

int g_lookupCalls = 0;

void* __fastcall FakeComponentLookup(void* gameSpace, uint64_t symbol) {
    ++g_lookupCalls;
    const uint8_t* slot = static_cast<const uint8_t*>(gameSpace) + FakeEngine::kFixtureCsTableOff;
    if (gameSpace == nullptr || !replay::audio::Readable(slot, sizeof(void*))) {
        return nullptr;
    }
    void* raw = nullptr;
    memcpy(&raw, slot, sizeof raw);
    if (!replay::audio::Readable(raw, sizeof(FakeEngine::FakeCsTable))) {
        return nullptr;
    }
    const FakeEngine::FakeCsTable* t = static_cast<const FakeEngine::FakeCsTable*>(raw);
    for (size_t i = 0; i < t->count && i < 2; ++i) {
        if (t->symbols[i] == symbol) {
            return t->systems[i];
        }
    }
    return nullptr;
}

void ClearLookup() {
    replay::audio::voip::SetComponentLookupForTest(nullptr);
}

void FakeEngine::InstallLookup() {
    replay::audio::voip::SetComponentLookupForTest(&FakeComponentLookup);
}

}  // namespace w3c

namespace {

struct SinkTrap {
    int calls = 0;
    bool accept = true;
    uint32_t speaker = 0;
    uint16_t seq = 0;
    std::vector<uint8_t> payload;
};

bool TrapSink(uint32_t speaker, uint16_t seq, const void* payload, uint32_t bytes, void* ctx) {
    auto* t = static_cast<SinkTrap*>(ctx);
    ++t->calls;
    t->speaker = speaker;
    t->seq = seq;
    t->payload.assign(static_cast<const uint8_t*>(payload),
                      static_cast<const uint8_t*>(payload) + bytes);
    return t->accept;
}

uint64_t TotalDispositions(const VoipCaptureStats& s) {
    return s.stored + s.droppedNoSink + s.droppedSinkRefused + s.droppedOversize +
           s.droppedNoPayload + s.droppedNoHeader;
}

}  // namespace

TEST_CASE("the receiver is found in the game space at +0x2B08, never through the CR15Game at +0x8") {
    // ARCH-10 S1.1: netGame+0x8 is the CR15Game back-pointer, so the lookup this code used to do
    // through it could not have found the receiver system in any session. The rig plants a
    // different, non-null object there, so an accessor put back on +0x8 fails the way it failed
    // live instead of tripping over a null.
    FakeEngine e;
    e.Build(1);
    e.InstallLookup();

    void* atEight = nullptr;
    memcpy(&atEight, e.netGame.data() + facts::IDENT_NETGAME_GAMESPACE_OFF, sizeof atEight);
    REQUIRE(atEight == e.gameDecoy.data());
    REQUIRE(atEight != static_cast<void*>(e.gameSpace.data()));

    VoipSite site;
    Resolve why = Resolve::NoNetGame;
    REQUIRE(ResolveVoipSite(e.netGame.data(), &site, &why));
    CHECK(why == Resolve::Ok);
    CHECK(site.cs == e.cs.data());
    CHECK(site.netGame == e.netGame.data());

    SUBCASE("★ the mirror of the belief this fixes: a space only at +0x8 is no space at all") {
        FakeEngine m;
        m.Build(1);
        m.InstallLookup();
        m.Poke(m.netGame, facts::NETGAME_CS_SPACE_OFF, static_cast<void*>(nullptr));
        m.Poke(m.netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, m.gameSpace.data());

        VoipSite s;
        Resolve w = Resolve::Ok;
        CHECK_FALSE(ResolveVoipSite(m.netGame.data(), &s, &w));
        CHECK(w == Resolve::NoGameSpace);   // an accessor on +0x8 resolves this rig instead
        CHECK_FALSE(s.Valid());
        e.InstallLookup();   // m is about to go away; leave the seam pointing at a live rig
    }

    SUBCASE("a field that is not mapped is unknown, and says so separately") {
        // 0x1000 is inside the 64 KB no-access region every Windows process reserves, so the read
        // is refused rather than faulted -- and the answer must not be "there is no space".
        VoipSite s;
        Resolve w = Resolve::Ok;
        CHECK_FALSE(ResolveVoipSite(reinterpret_cast<void*>(0x1000), &s, &w));
        CHECK(w == Resolve::GameSpaceUnreadable);
        CHECK(strcmp(ResolveText(w), ResolveText(Resolve::NoGameSpace)) != 0);
        CHECK_FALSE(s.Valid());
    }

    w3c::ClearLookup();
}

TEST_CASE("the walk finds the instance the engine's own dispatcher would have chosen") {
    FakeEngine e;
    e.Build(3);
    e.SetSlotGeneration(2, 0x0007);
    e.Bind(0, 5, 0x00AA, MakeUserId(9, 0x0001));
    e.Bind(1, 3, 0x00BB, MakeUserId(2, 0x0007));   // slot 2, generation 7
    e.Bind(2, 6, 0x00CC, MakeUserId(4, 0x0002));

    uint32_t userId = 0;
    Resolve why = Resolve::Ok;
    REQUIRE(UserIdOf(e.netGame.data(), 2, &userId, &why));
    CHECK(userId == MakeUserId(2, 0x0007));

    uint32_t handle = 0;
    const uint8_t* ci = nullptr;
    REQUIRE(HandleOf(e.Site(), userId, &handle, &ci, &why));
    CHECK(why == Resolve::Ok);
    // pool slot 3 in the low word, the pool entry's generation in the high -- which is what
    // sub_140D8A630 assembles and what ReceiveVoip's low-word-only lookup consumes.
    CHECK((handle & 0xFFFFu) == 3u);
    CHECK((handle >> 16) == 0x00BBu);
    CHECK(ci == e.Ci(1));

    SUBCASE("a user nobody is connected as is refused, not approximated") {
        uint32_t h = 0;
        const uint8_t* c = nullptr;
        CHECK_FALSE(HandleOf(e.Site(), MakeUserId(11, 3), &h, &c, &why));
        CHECK(why == Resolve::NoSuchTalker);
        CHECK(h == 0u);
    }

    SUBCASE("an instance past the live count is not searched") {
        // The row is still in memory and still matches; only the count says it is not live.
        e.Build(1);
        uint32_t h = 0;
        const uint8_t* c = nullptr;
        CHECK_FALSE(HandleOf(e.Site(), userId, &h, &c, &why));
        CHECK(why == Resolve::NoSuchTalker);
    }

    SUBCASE("★ the round trip catches a pool table that is not ours") {
        // Exactly one byte of belief changed: the pool entry no longer points back at the dense
        // index that reached it. Without the reverse hop this produces a plausible handle whose low
        // word indexes a live instance -- somebody else's.
        e.pool[3] = (e.pool[3] & 0xFFFF0000u) | 2u;
        uint32_t h = 0;
        const uint8_t* c = nullptr;
        CHECK_FALSE(HandleOf(e.Site(), userId, &h, &c, &why));
        CHECK(why == Resolve::TableDisagree);
    }
}

TEST_CASE("★ the recorded talker key is not the live one, and re-deriving is not a no-op") {
    // Session A: slot 2 generation 7 sits in pool slot 3. The capture records the handle it saw.
    FakeEngine a;
    a.Build(2);
    a.SetSlotGeneration(2, 0x0007);
    a.Bind(0, 3, 0x00BB, MakeUserId(2, 0x0007));
    a.Bind(1, 4, 0x00CC, MakeUserId(5, 0x0001));
    uint32_t recordedHandle = 0;
    const uint8_t* ci = nullptr;
    Resolve why = Resolve::Ok;
    REQUIRE(HandleOf(a.Site(), MakeUserId(2, 0x0007), &recordedHandle, &ci, &why));

    // Session B: the same player is back in slot 2 with a new generation, and pool slot 3 -- the one
    // the recording names -- now belongs to somebody else entirely.
    FakeEngine b;
    b.Build(2);
    b.SetSlotGeneration(2, 0x0011);
    b.Bind(0, 3, 0x00DD, MakeUserId(8, 0x0004));   // a DIFFERENT player in the recorded slot
    b.Bind(1, 7, 0x00EE, MakeUserId(2, 0x0011));   // our player, elsewhere

    uint32_t liveId = 0;
    REQUIRE(UserIdOf(b.netGame.data(), 2, &liveId, &why));
    uint32_t liveHandle = 0;
    REQUIRE(HandleOf(b.Site(), liveId, &liveHandle, &ci, &why));

    // If anybody ever "optimises" injection to replay the recorded key, this is the assertion that
    // goes red: the recorded handle resolves, silently, to the wrong player.
    CHECK((recordedHandle & 0xFFFFu) == 3u);
    CHECK((liveHandle & 0xFFFFu) == 7u);
    CHECK(recordedHandle != liveHandle);
    CHECK(ci == b.Ci(1));
}

TEST_CASE("a user slot with nobody in it is an absence, not slot zero") {
    FakeEngine e;
    e.Build(1);
    Resolve why = Resolve::Ok;
    uint32_t id = 0xDEADBEEF;

    CHECK_FALSE(UserIdOf(e.netGame.data(), voip_facts::kUserSlotTableMax, &id, &why));
    CHECK(why == Resolve::SlotOutOfRange);
    CHECK(id == 0u);

    // Build() fills every generation with 0xFFFF, which is the engine's own "nobody".
    CHECK_FALSE(UserIdOf(e.netGame.data(), 3, &id, &why));
    CHECK(why == Resolve::NoGeneration);

    CHECK_FALSE(UserIdOf(nullptr, 3, &id, &why));
    CHECK(why == Resolve::NoNetGame);

    e.SetSlotGeneration(3, 0x0042);
    REQUIRE(UserIdOf(e.netGame.data(), 3, &id, &why));
    CHECK(id == MakeUserId(3, 0x0042));
}

TEST_CASE("every gate ReceiveVoip checks is read rather than assumed") {
    FakeEngine e;
    e.Build(1);
    e.SetSlotGeneration(1, 5);
    e.Bind(0, 2, 0x0099, MakeUserId(1, 5));
    const uint8_t* ci = e.Ci(0);
    void* provider = &e;   // any non-null

    CHECK(InspectGates(e.Site(), ci, provider) == Gate::Open);
    CHECK(InspectGates(e.Site(), ci, nullptr) == Gate::NoProvider);

    SUBCASE("gate 2: the netGame sub-object") {
        e.Poke(e.netGame, voip_facts::kNetGameSubOff, static_cast<void*>(nullptr));
        CHECK(InspectGates(e.Site(), ci, provider) == Gate::NoNetGameSub);
    }
    SUBCASE("gate 3: a decoder state that is set with a talker that is not") {
        e.Poke(e.ci, e.CiOff(0, voip_facts::kCiTalkerBOff), voip_facts::kTalkerUnset);
        CHECK(InspectGates(e.Site(), ci, provider) == Gate::NotConnected);
    }
    SUBCASE("gate 3's other arm: a zero decoder state skips the talker checks entirely") {
        e.Poke(e.ci, e.CiOff(0, voip_facts::kCiTalkerAOff), voip_facts::kTalkerUnset);
        e.Poke(e.ci, e.CiOff(0, voip_facts::kCiTalkerBOff), voip_facts::kTalkerUnset);
        e.Poke(e.ci, e.CiOff(0, voip_facts::kCiDecoderStateOff), 0u);
        CHECK(InspectGates(e.Site(), ci, provider) == Gate::Open);
    }
    SUBCASE("a null CI is unknown, never open") {
        CHECK(InspectGates(e.Site(), nullptr, provider) == Gate::Unreadable);
    }
}

TEST_CASE("the capture tap resolves the speaker and counts every disposition") {
    FakeEngine e;
    e.Build(2);
    e.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));
    e.Bind(1, 1, 0x0008, MakeUserId(7, 0x0004));

    uint8_t header[16] = {};
    const uint16_t wireSeq = 0x0BCD;   // more than ten bits on purpose
    memcpy(header + audio_facts::kVoipRecvSeqOff, &wireSeq, sizeof wireSeq);
    std::vector<uint8_t> payload(96);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i * 7 + 1);
    }

    SinkTrap trap;
    ResetForTest();
    SetVoipSink(&TrapSink, &trap);

    // The handle the engine's own dispatcher would have assembled: pool slot 1 in the low word and
    // that entry's generation in the high. The tap checks the two agree even though VOIP.RECEIVE
    // does not, because a disagreement means the pool is not shaped the way we believe.
    const uint32_t handle = 1u | (0x0008u << 16);
    CHECK(TapForTest(e.cs.data(), handle, header, payload.data(), 96) == TapVerdict::Stored);
    CHECK(trap.calls == 1);
    CHECK(trap.speaker == MakeUserId(7, 0x0004));
    CHECK(trap.seq == (wireSeq & kSeqMask));
    REQUIRE(trap.payload.size() == payload.size());
    CHECK(memcmp(trap.payload.data(), payload.data(), payload.size()) == 0);
    CHECK(LatchedCs() == e.cs.data());
    CHECK(LatchedHandle() == handle);

    SUBCASE("a length past the bound is refused whole and its size is remembered") {
        CHECK(TapForTest(e.cs.data(), handle, header, payload.data(), kMaxPayload + 1) ==
              TapVerdict::Oversize);
        const VoipCaptureStats s = CaptureStats();
        CHECK(s.droppedOversize == 1);
        CHECK(s.maxLengthRefused == kMaxPayload + 1);
        CHECK(s.stored == 1);   // the good frame above; a refusal does not un-store it
    }

    SUBCASE("a sink that refuses is a drop with its own name") {
        trap.accept = false;
        CHECK(TapForTest(e.cs.data(), handle, header, payload.data(), 96) ==
              TapVerdict::SinkRefused);
        CHECK(CaptureStats().droppedSinkRefused == 1);
    }

    SUBCASE("no sink at all is counted, not silence") {
        SetVoipSink(nullptr, nullptr);
        CHECK(TapForTest(e.cs.data(), handle, header, payload.data(), 96) == TapVerdict::NoSink);
        CHECK(CaptureStats().droppedNoSink == 1);
    }

    SUBCASE("an unreadable header refuses rather than storing sequence zero") {
        CHECK(TapForTest(e.cs.data(), handle, nullptr, payload.data(), 96) == TapVerdict::NoHeader);
        CHECK(CaptureStats().droppedNoHeader == 1);
        CHECK(trap.calls == 1);   // still only the good frame
    }

    SUBCASE("a handle whose generation the pool does not agree with is not attributed") {
        // Pool slot 1 is real; the generation is not the one that entry carries. Without the
        // agreement check this stores a plausible and WRONG speaker, and no later reader could
        // tell -- the frame would be attributed to whoever holds that slot, forever.
        const uint32_t stale = 1u | (0xBEEFu << 16);
        CHECK(TapForTest(e.cs.data(), stale, header, payload.data(), 96) ==
              TapVerdict::StoredUnresolved);
        CHECK(trap.speaker == kUnresolvedSpeaker);
        CHECK(CaptureStats().unresolvedSpeaker == 1);
    }

    SUBCASE("★ observed is the sum of the dispositions, so nothing can be lost unaccounted") {
        trap.accept = false;
        TapForTest(e.cs.data(), handle, header, payload.data(), 96);
        trap.accept = true;
        TapForTest(e.cs.data(), handle, header, payload.data(), kMaxPayload + 1);
        TapForTest(e.cs.data(), handle, nullptr, payload.data(), 96);
        SetVoipSink(nullptr, nullptr);
        TapForTest(e.cs.data(), handle, header, payload.data(), 96);
        const VoipCaptureStats s = CaptureStats();
        CHECK(s.observed == 5);
        CHECK(TotalDispositions(s) == s.observed);
    }

    SetVoipSink(nullptr, nullptr);
}

TEST_CASE("injection hands the engine the arguments its own caller would have") {
    FakeEngine e;
    e.Build(2);
    e.SetSlotGeneration(6, 0x0003);
    e.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));
    e.Bind(1, 1, 0x0008, MakeUserId(7, 0x0004));

    capture::VoipRec rec{};
    rec.tUs = 1000;
    rec.speakerSlot = MakeUserId(6, 0x00FF);   // a RECORDED generation, deliberately stale
    rec.frameCtr = 0x0155;
    rec.bodyBytes = 40;
    std::vector<uint8_t> payload(40);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(0xA0 + i);
    }

    g_call = RecordedCall{};
    const VoipPlayer::InjectOutcome out = VoipPlayer::InjectOneForTest(
        e.Site(), &e, reinterpret_cast<void*>(&FakeReceiveVoip), rec, payload.data());

    REQUIRE(out.injected);
    CHECK(out.resolve == Resolve::Ok);
    CHECK(out.gate == Gate::Open);
    CHECK(g_call.calls == 1);
    CHECK(g_call.receiver == e.cs.data());
    // ★ The handle came from the LIVE tables. The record's stale 0x00FF generation reached nothing.
    CHECK((g_call.handle & 0xFFFFu) == 4u);
    CHECK((g_call.handle >> 16) == 0x0007u);
    CHECK(g_call.length == 40u);
    CHECK(memcmp(g_call.payload.data(), payload.data(), payload.size()) == 0);

    SUBCASE("the header stub carries the sequence at +4 and nothing anywhere else") {
        uint16_t seq = 0;
        memcpy(&seq, g_call.header + audio_facts::kVoipRecvSeqOff, sizeof seq);
        CHECK(seq == (rec.frameCtr & kSeqMask));
        for (size_t i = 0; i < sizeof g_call.header; ++i) {
            if (i == audio_facts::kVoipRecvSeqOff || i == audio_facts::kVoipRecvSeqOff + 1) {
                continue;
            }
            CHECK(g_call.header[i] == 0);
        }
    }
}

TEST_CASE("injection refuses rather than guessing, and calls nothing when it does") {
    FakeEngine e;
    e.Build(1);
    e.SetSlotGeneration(6, 0x0003);
    e.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));

    capture::VoipRec rec{};
    rec.speakerSlot = MakeUserId(6, 0x0003);
    rec.frameCtr = 3;
    rec.bodyBytes = 8;
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    SUBCASE("an unresolved speaker in the file never becomes a slot") {
        rec.speakerSlot = kUnresolvedSpeaker;
        g_call = RecordedCall{};
        const auto out = VoipPlayer::InjectOneForTest(e.Site(), &e,
                                                      reinterpret_cast<void*>(&FakeReceiveVoip),
                                                      rec, payload);
        CHECK_FALSE(out.injected);
        CHECK(out.resolve == Resolve::NoSuchTalker);
        CHECK(g_call.calls == 0);
    }

    SUBCASE("a closed gate stops the call before it is made") {
        e.Poke(e.ci, e.CiOff(0, voip_facts::kCiTalkerAOff), voip_facts::kTalkerUnset);
        g_call = RecordedCall{};
        const auto out = VoipPlayer::InjectOneForTest(e.Site(), &e,
                                                      reinterpret_cast<void*>(&FakeReceiveVoip),
                                                      rec, payload);
        CHECK_FALSE(out.injected);
        CHECK(out.gate == Gate::NotConnected);
        CHECK(g_call.calls == 0);
    }

    SUBCASE("a null provider is gate one, and the engine would have returned at once") {
        g_call = RecordedCall{};
        const auto out = VoipPlayer::InjectOneForTest(e.Site(), nullptr,
                                                      reinterpret_cast<void*>(&FakeReceiveVoip),
                                                      rec, payload);
        CHECK_FALSE(out.injected);
        CHECK(out.gate == Gate::NoProvider);
        CHECK(g_call.calls == 0);
    }

    SUBCASE("the player's slot emptied since the recording") {
        FakeEngine empty;
        empty.Build(1);
        empty.Bind(0, 4, 0x0007, MakeUserId(6, 0x0003));
        // No SetSlotGeneration: every slot's generation is the engine's own 0xFFFF "nobody".
        g_call = RecordedCall{};
        const auto out = VoipPlayer::InjectOneForTest(empty.Site(), &empty,
                                                      reinterpret_cast<void*>(&FakeReceiveVoip),
                                                      rec, payload);
        CHECK_FALSE(out.injected);
        CHECK(out.resolve == Resolve::NoGeneration);
        CHECK(g_call.calls == 0);
    }
}
