// W2-H's offline arm. Everything here runs on a machine with no game and no sound card.
//
// Each case names the input that makes it pass and the input that makes it fail; where the failing
// input is a mutation of the subject rather than of the data, the mutation is written out and run,
// because a check nobody has seen go red is a check nobody has tested.

#include <doctest.h>

#include <string.h>

#include <chrono>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <limits>
#include <vector>

#include "checkledger.h"

#include "audio/akbind.h"
#include "audio/audio_facts.h"
#include "audio/audiopins.h"
#include "audio/audiopinsource.h"
#include "audio/audiostate_init.h"
#include "audio/audiorate.h"
#include "audio/audioresidue.h"
#include "audio/audiostate_init.h"
#include "audio/foleymix.h"
#include "bootstrap/config.h"
#include "audio/ratedrive.h"
#include "audio/ratepolicy.h"
#include "audio/sinkvoice.h"
#include "audio/voicelifetime.h"
#include "audio/voipformat.h"
#include "bootstrap/services.h"
#include "engine/audiostate.h"
#include "playback/core/transport.h"

using namespace replay;
using namespace replay::audio;

namespace {
// RegisterAudioResidue takes a getter now, because CSoundPlayer resolves after the arm. These
// cases are not about that: they pin one state for the life of the case and hand over a getter
// that returns it.
IAudioState* g_fixedResidueState = nullptr;
IAudioState* FixedResidueState() { return g_fixedResidueState; }
AudioStateGetter BindResidueState(IAudioState* s) {
    g_fixedResidueState = s;
    return &FixedResidueState;
}
}  // namespace

// ==============================================================================================
// The rate ladder (S10.4.4 / S10.4.5)
// ==============================================================================================

TEST_CASE("the rate ladder serves every row of the S10.4.4 policy table") {
    SUBCASE("1.0x is the shipped ratio and reports itself as normal") {
        RateCommand cmd;
        cmd.rate = 1.0f;
        const RatePlan plan = PlanRate(cmd);
        CHECK(plan.mode == RateMode::Normal);
        CHECK(plan.ratio == 1.0f);
        CHECK(plan.pitchIsHonest);
        CHECK_FALSE(plan.suspend);
    }

    SUBCASE("the whole slow-motion range is honest, and is unaffected by the missing hook") {
        for (float r : {0.5f, 0.25f, 0.1f, 0.05f, 0.01f}) {
            RateCommand cmd;
            cmd.rate = r;
            cmd.maxRate = 1.0f;   // the Level-1X hook is NOT installed
            const RatePlan plan = PlanRate(cmd);
            CHECK(plan.mode == RateMode::Slowed);
            CHECK(plan.ratio == doctest::Approx(r));
            CHECK(plan.pitchIsHonest);
        }
    }

    SUBCASE("below XAudio2's own minimum the plan clamps AND says it clamped") {
        RateCommand cmd;
        cmd.rate = 0.0001f;    // under kXAudio2MinFreqRatio, where the call would be refused
        const RatePlan plan = PlanRate(cmd);
        CHECK(plan.ratio == kXAudio2MinFreqRatio);
        CHECK_FALSE(plan.pitchIsHonest);
    }

    SUBCASE("pause suspends; it never commands a zero ratio") {
        RateCommand cmd;
        cmd.paused = true;
        const RatePlan plan = PlanRate(cmd);
        CHECK(plan.mode == RateMode::Suspended);
        CHECK(plan.suspend);
        CHECK(plan.ratio == 1.0f);
    }

    SUBCASE("reverse rides the magnitude") {
        RateCommand cmd;
        cmd.rate = -0.5f;
        const RatePlan plan = PlanRate(cmd);
        // OWNER-WATCH-FIX: the sink has no backwards mix, so reverse is silence rather than the
        // forward mix at |r| it used to play under a backward picture.
        CHECK(plan.mode == RateMode::Muted);
        CHECK(plan.suspend);
        CHECK(std::string(RateLabel(plan, true)) == "audio: muted while the picture runs backward");
    }

    SUBCASE("above 1.0x without the hook clamps to 1.0 and says so, rather than commanding a "
            "ratio XAudio2 would silently clamp for us") {
        RateCommand cmd;
        cmd.rate = 4.0f;
        cmd.maxRate = 1.0f;
        const RatePlan plan = PlanRate(cmd);
        CHECK(plan.mode == RateMode::ClampedFast);
        CHECK(plan.ratio == 1.0f);
        CHECK_FALSE(plan.pitchIsHonest);
        CHECK(std::string(RateLabel(plan, true)).find("rate hook unavailable") != std::string::npos);
        CHECK(std::string(RateLabel(plan, false)).find("hook disabled") != std::string::npos);
    }

    SUBCASE("above 1.0x WITH the hook is honest up to the installed maximum") {
        RateCommand cmd;
        cmd.rate = 4.0f;
        cmd.maxRate = 4.0f;
        CHECK(PlanRate(cmd).mode == RateMode::Accelerated);
        CHECK(PlanRate(cmd).ratio == doctest::Approx(4.0f));

        cmd.rate = 8.0f;   // past the installed ceiling
        CHECK(PlanRate(cmd).ratio == doctest::Approx(4.0f));
        CHECK_FALSE(PlanRate(cmd).pitchIsHonest);
    }

    SUBCASE("??? the property ruling Q-A demands: refusing the hook costs ONLY pitch above 1.0x") {
        // The passing input is the whole transport range with maxRate 1.0; the failing input would
        // be any row below 1.0 whose plan differed from the same row with the hook installed.
        for (float r : {0.001f, 0.1f, 0.5f, 1.0f}) {
            RateCommand without;
            without.rate = r;
            without.maxRate = 1.0f;
            RateCommand with;
            with.rate = r;
            with.maxRate = 4.0f;
            CHECK(PlanRate(without).ratio == PlanRate(with).ratio);
            CHECK(PlanRate(without).mode == PlanRate(with).mode);
        }
    }

    SUBCASE("when the sink proofs refused, audio still plays at 1.0x and nothing suspends") {
        RateCommand cmd;
        cmd.rate = 0.25f;
        cmd.available = false;
        const RatePlan plan = PlanRate(cmd);
        CHECK(plan.mode == RateMode::Unavailable);
        CHECK(plan.ratio == 1.0f);
        CHECK_FALSE(plan.suspend);
    }
}

// ==============================================================================================
// S10.4.1a -- the A/V offset, which no rate oracle can see
// ==============================================================================================

TEST_CASE("the A/V bias is measured in scene time and latches to a constant") {
    SUBCASE("48 kHz, 512-sample frames, 8 buffers queued is the 85 ms the design quotes") {
        AvBias bias;
        for (uint32_t i = 0; i < AvBias::kWarmupSamples; ++i) {
            bias.Observe(8, 512, 48000);
        }
        REQUIRE(bias.Latched());
        CHECK(bias.OffsetSceneUs() == 85333);
    }

    SUBCASE("??? and it MOVES with the device, which is the whole reason it is not a constant") {
        // The failing input for a hardcoded 85 ms: a 44.1 kHz endpoint. Same buffer count, and the
        // correct answer is 11% larger.
        AvBias bias;
        for (uint32_t i = 0; i < AvBias::kWarmupSamples; ++i) {
            bias.Observe(8, 512, 44100);
        }
        REQUIRE(bias.Latched());
        CHECK(bias.OffsetSceneUs() == 92879);
        CHECK(bias.OffsetSceneUs() > 85333);
    }

    SUBCASE("the maximum over the warm-up is what latches, so a filling voice does not jitter it") {
        AvBias bias;
        const uint32_t depths[] = {0, 1, 3, 8, 7, 8, 6, 8};
        for (uint32_t i = 0; i < AvBias::kWarmupSamples; ++i) {
            bias.Observe(depths[i % 8], 512, 48000);
        }
        CHECK(bias.OffsetSceneUs() == 85333);
    }

    SUBCASE("a voice that was never fed does NOT latch a zero bias") {
        // Zero across the warm-up is the absence of a measurement, not a measurement of zero, and
        // applying it as a correction would be right only by accident.
        AvBias bias;
        for (uint32_t i = 0; i < AvBias::kWarmupSamples * 2; ++i) {
            bias.Observe(0, 512, 48000);
        }
        CHECK_FALSE(bias.Latched());
        CHECK(bias.OffsetSceneUs() == 0);
    }

    SUBCASE("an implausible depth is refused rather than applied") {
        AvBias bias;
        for (uint32_t i = 0; i < AvBias::kWarmupSamples; ++i) {
            bias.Observe(4000, 512, 48000);   // 42 seconds of buffer: a misread, not a device
        }
        CHECK_FALSE(bias.Latched());
    }

    SUBCASE("a device change invalidates it") {
        AvBias bias;
        for (uint32_t i = 0; i < AvBias::kWarmupSamples; ++i) {
            bias.Observe(8, 512, 48000);
        }
        REQUIRE(bias.Latched());
        bias.Reset();
        CHECK_FALSE(bias.Latched());
        CHECK(bias.OffsetSceneUs() == 0);
    }
}

// ==============================================================================================
// S12.4 -- two oracles, because one cannot tell the two failure modes apart
// ==============================================================================================

TEST_CASE("the O4 verdict distinguishes a broken mechanism from a starved machine") {
    const double tol = 0.02;

    SUBCASE("both clocks track the commanded rate") {
        RateSample s;
        s.settingsValid = true;
        s.wwiseRatio = 0.25;
        s.deviceRatio = 0.25;
        CHECK(JudgeRate(s, 0.25, tol) == RateVerdict::Pass);
    }

    SUBCASE("the device did not consume at rate: the ratio did not take, and that is a FAIL") {
        RateSample s;
        s.settingsValid = true;
        s.wwiseRatio = 1.0;
        s.deviceRatio = 1.0;
        CHECK(JudgeRate(s, 0.25, tol) == RateVerdict::FailMechanism);
    }

    SUBCASE("??? the device tracked but Wwise lagged: starved upstream, and that is NOT-RUN") {
        // This is the case a single-oracle gate would have reported as a broken mechanism, sending
        // somebody to fix working code.
        RateSample s;
        s.settingsValid = true;
        s.deviceRatio = 0.25;
        s.wwiseRatio = 0.17;
        CHECK(JudgeRate(s, 0.25, tol) == RateVerdict::NotRunStarved);
    }

    SUBCASE("Wwise not up is NOT-RUN, never a pass") {
        RateSample s;
        s.settingsValid = false;
        CHECK(JudgeRate(s, 1.0, tol) == RateVerdict::NotRunInvalid);
    }
}

TEST_CASE("the closed loop trims towards the scene clock and stays bounded") {
    RateLock lock;
    // Priming records the offset between two clocks with different origins rather than fighting it.
    CHECK(lock.Update(0.5f, 100.0, 0.0, 0.01f) == 0.5f);
    CHECK(lock.Trim() == 0.0f);

    SUBCASE("audio running ahead of the scene clock trims the ratio down") {
        for (int i = 0; i < 50; ++i) {
            lock.Update(0.5f, 100.0 + 0.02 * i + 0.01, 0.02 * i, 0.02);
        }
        CHECK(lock.Trim() < 0.0f);
    }

    SUBCASE("the trim is bounded, so a starved machine cannot drive it anywhere") {
        for (int i = 0; i < 5000; ++i) {
            lock.Update(0.5f, 100.0 + 10.0, 0.0, 0.02);
        }
        CHECK(lock.Trim() >= -RateLock::kMaxTrim);
        CHECK(lock.Trim() <= RateLock::kMaxTrim);
    }
}

// ==============================================================================================
// KB_AUDIO_PINS
// ==============================================================================================

namespace {

AudioPins MakePins() {
    AudioPins p;
    p.stateCount = 3;
    for (uint32_t i = 0; i < p.stateCount; ++i) {
        p.states[i] = {0x1000u + i, 0x2000u + i};  // facts-ok: PHYS.CI_PAUSED_BIT a test fixture value this case compares against, not an engine position
    }
    p.switchCount = 2;
    p.switches[0] = {7, 0x30u, 0x40u};
    p.switches[1] = {9, 0x31u, 0x41u};
    p.loopCount = 2;
    p.loops[0] = {7, 0xABCDu, 1234, 9000};
    p.loops[1] = {9, 0xBEEFu, -1, 500};
    p.seed = 0xDEADBEEFu;
    p.seedValid = true;
    p.historyBytes = 40;
    for (uint32_t i = 0; i < p.historyBytes; ++i) {
        p.history[i] = static_cast<uint8_t>(i * 7);
    }
    p.totals = {3, 3, 900, 2, 2, 2, 41, 0};
    return p;
}

}  // namespace

TEST_CASE("the pin block round-trips and refuses rather than guessing") {
    const AudioPins in = MakePins();
    std::vector<uint8_t> buf(MaxPinBytes());
    const size_t n = EncodePins(in, buf.data(), buf.size());
    REQUIRE(n > 0);

    SUBCASE("every section comes back byte-identical") {
        AudioPins out;
        PinDecodeReport rep;
        REQUIRE(DecodePins(buf.data(), n, &out, &rep) == PinDecode::Ok);
        CHECK(rep.unknownSections == 0);
        CHECK(out.stateCount == in.stateCount);
        CHECK(memcmp(out.states, in.states, in.stateCount * sizeof(AudioStatePair)) == 0);
        CHECK(out.switchCount == in.switchCount);
        CHECK(memcmp(out.switches, in.switches, in.switchCount * sizeof(AudioSwitchTriple)) == 0);
        CHECK(out.loopCount == in.loopCount);
        CHECK(memcmp(out.loops, in.loops, in.loopCount * sizeof(AudioLoopPin)) == 0);
        CHECK(out.seed == in.seed);
        CHECK(out.seedValid);
        CHECK(out.historyBytes == in.historyBytes);
        CHECK(memcmp(out.history, in.history, in.historyBytes) == 0);
        // ??? The totals survive, and they are what makes a capped capture detectable: 900 switches
        // seen against 2 stored is a file whose seek will restore a subset, and echodemo verify can
        // say so because the number is on disk.
        CHECK(out.totals.switchesSeen == 900);
        CHECK(out.totals.switchesStored == 2);
    }

    SUBCASE("a buffer one byte short produces nothing, never a short block") {
        std::vector<uint8_t> tight(n - 1);
        CHECK(EncodePins(in, tight.data(), tight.size()) == 0);
    }

    SUBCASE("a wrong version is refused by version, not misparsed into plausible values") {
        std::vector<uint8_t> bad(buf.begin(), buf.begin() + n);
        bad[0] = 0xFF;
        AudioPins out;
        CHECK(DecodePins(bad.data(), bad.size(), &out, nullptr) == PinDecode::UnsupportedVersion);
    }

    SUBCASE("a ragged section length is refused") {
        std::vector<uint8_t> bad(buf.begin(), buf.begin() + n);
        // The first section header sits right after the 4-byte block header; its length is at +2.
        uint32_t len = 0;
        memcpy(&len, bad.data() + 4 + 2, sizeof len);
        len -= 1;
        memcpy(bad.data() + 4 + 2, &len, sizeof len);
        AudioPins out;
        CHECK(DecodePins(bad.data(), bad.size(), &out, nullptr) == PinDecode::RaggedSection);
    }

    SUBCASE("a truncated block is refused where it ran out, not read past") {
        AudioPins out;
        CHECK(DecodePins(buf.data(), n - 4, &out, nullptr) == PinDecode::SectionOverrunsBlock);
    }

    SUBCASE("??? an unknown section is skipped by length AND COUNTED") {
        // The mutation: append a section with a tag no build understands. A reader that dropped it
        // silently could not tell a file that carries nothing from one it cannot read.
        std::vector<uint8_t> grown(buf.begin(), buf.begin() + n);
        const uint8_t body[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        const uint16_t tag = 999;
        const uint32_t len = sizeof body;
        grown.insert(grown.end(), reinterpret_cast<const uint8_t*>(&tag),
                     reinterpret_cast<const uint8_t*>(&tag) + 2);
        grown.insert(grown.end(), reinterpret_cast<const uint8_t*>(&len),
                     reinterpret_cast<const uint8_t*>(&len) + 4);
        grown.insert(grown.end(), body, body + sizeof body);
        uint16_t count = 0;
        memcpy(&count, grown.data() + 2, sizeof count);
        count += 1;
        memcpy(grown.data() + 2, &count, sizeof count);

        AudioPins out;
        PinDecodeReport rep;
        REQUIRE(DecodePins(grown.data(), grown.size(), &out, &rep) == PinDecode::Ok);
        CHECK(rep.unknownSections == 1);
        CHECK(rep.unknownBytes == sizeof body);
        CHECK(out.stateCount == in.stateCount);   // the known sections still decoded
    }
}

// ==============================================================================================
// S_VOIP
// ==============================================================================================

TEST_CASE("the replay header stub puts the sequence exactly where ReceiveVoip reads it") {
    const VoipHeaderStub stub = MakeHeaderStub(0x1234);
    uint16_t at4 = 0;
    memcpy(&at4, stub.bytes + 4, sizeof at4);
    CHECK(at4 == (0x1234 & kSeqMask));
    // Everything else is zero: if this build's reading of the header layout is ever wrong, the
    // callee reads a zero rather than a stack value that happens to look plausible.
    for (size_t i = 0; i < sizeof stub.bytes; ++i) {
        if (i == 4 || i == 5) {
            continue;
        }
        CHECK(stub.bytes[i] == 0);
    }
}

namespace {

struct InjectSpy {
    static void* receiver;
    static uint32_t talkerKey;
    static uint16_t seqSeen;
    static uint32_t length;
    static uint32_t calls;

    static void Receive(void* r, uint32_t key, const void* header, const void*, uint32_t len) {
        receiver = r;
        talkerKey = key;
        memcpy(&seqSeen, static_cast<const uint8_t*>(header) + 4, sizeof seqSeen);
        length = len;
        ++calls;
    }
};
void* InjectSpy::receiver = nullptr;
uint32_t InjectSpy::talkerKey = 0;
uint16_t InjectSpy::seqSeen = 0;
uint32_t InjectSpy::length = 0;
uint32_t InjectSpy::calls = 0;

}  // namespace

TEST_CASE("injection hands the engine the frame and never partially injects") {
    InjectSpy::calls = 0;
    int receiverObject = 0;
    VoipInjector inj;
    inj.receiveVoip = &InjectSpy::Receive;
    inj.receiver = &receiverObject;

    uint8_t payload[64] = {};
    CHECK(InjectVoip(inj, 0x00070003u, 700, payload, sizeof payload));
    CHECK(InjectSpy::calls == 1);
    CHECK(InjectSpy::receiver == &receiverObject);
    CHECK(InjectSpy::talkerKey == 0x00070003u);
    CHECK(InjectSpy::seqSeen == 700);
    CHECK(InjectSpy::length == sizeof payload);

    SUBCASE("an unready injector does nothing rather than calling through a null") {
        VoipInjector empty;
        CHECK_FALSE(InjectVoip(empty, 1, 1, payload, sizeof payload));
        CHECK(InjectSpy::calls == 1);
    }

    SUBCASE("an implausible length is refused before the call") {
        CHECK_FALSE(InjectVoip(inj, 1, 1, payload, static_cast<uint16_t>(kMaxPayload + 1)));
        CHECK(InjectSpy::calls == 1);
    }
}

// ==============================================================================================
// IAudioState against a synthetic CSoundPlayer -- the memo walk and the N-16 repair
// ==============================================================================================

namespace {

// A CSoundPlayer laid out the way the shipping one is, in ordinary heap memory. Only the four
// fields this design reads exist; everything else is zero, which is the point -- a field we never
// touch cannot be wrong, and a field we do touch is here at the offset the binary decodes to.
struct FakeSoundPlayer {
    static constexpr size_t kSize = 0x400;
    static constexpr size_t kMemoOff = 0x338;   // SOUND.MEMO_CACHE_OFF
    static constexpr size_t kCountOff = 0x30;   // SOUND.MEMO_COUNT_OFF, relative to the container
    static constexpr size_t kEmitterMapOff = 0x60;
    static constexpr size_t kEmitterMapCountOff = 0x90;
    static constexpr size_t kEmitterSwitchMemoOff = 0x78;

    std::vector<uint8_t> block;
    std::vector<uint64_t> stateEntries;     // {u32 group, u32 value} packed
    std::vector<uint8_t> emitterMap;        // 16-byte {u32 id, pad, u64 ptr}
    std::vector<uint8_t> emitterA;          // one emitter record
    std::vector<uint64_t> switchEntries;

    FakeSoundPlayer() : block(kSize, 0), emitterA(0x100, 0) {}

    uintptr_t Base() { return reinterpret_cast<uintptr_t>(block.data()); }

    void SetStates(std::vector<std::pair<uint32_t, uint32_t>> pairs) {
        stateEntries.clear();
        for (auto& p : pairs) {
            stateEntries.push_back(static_cast<uint64_t>(p.first) |
                                   (static_cast<uint64_t>(p.second) << 32));
        }
        Poke(kMemoOff, reinterpret_cast<uintptr_t>(stateEntries.data()));
        Poke(kMemoOff + kCountOff, static_cast<uint64_t>(stateEntries.size()));
    }

    // One emitter with a switch memo, so the per-object half is exercised too.
    void SetEmitter(uint32_t gameObj, std::vector<std::pair<uint32_t, uint32_t>> pairs) {
        switchEntries.clear();
        for (auto& p : pairs) {
            switchEntries.push_back(static_cast<uint64_t>(p.first) |
                                    (static_cast<uint64_t>(p.second) << 32));
        }
        const uintptr_t data = reinterpret_cast<uintptr_t>(switchEntries.data());
        const uint64_t count = switchEntries.size();
        memcpy(emitterA.data() + kEmitterSwitchMemoOff, &data, sizeof data);
        memcpy(emitterA.data() + kEmitterSwitchMemoOff + kCountOff, &count, sizeof count);

        emitterMap.assign(16, 0);
        const uintptr_t emitterPtr = reinterpret_cast<uintptr_t>(emitterA.data());
        memcpy(emitterMap.data(), &gameObj, sizeof gameObj);
        memcpy(emitterMap.data() + 8, &emitterPtr, sizeof emitterPtr);
        Poke(kEmitterMapOff, reinterpret_cast<uintptr_t>(emitterMap.data()));
        Poke(kEmitterMapCountOff, uint64_t{1});
    }

    uint32_t CachedValue(uint32_t group) const {
        for (uint64_t e : stateEntries) {
            if (static_cast<uint32_t>(e) == group) {
                return static_cast<uint32_t>(e >> 32);
            }
        }
        return 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    }

    uint32_t CachedSwitch(uint32_t group) const {
        for (uint64_t e : switchEntries) {
            if (static_cast<uint32_t>(e) == group) {
                return static_cast<uint32_t>(e >> 32);
            }
        }
        return 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    }

private:
    template <class T>
    void Poke(size_t off, T v) {
        memcpy(block.data() + off, &v, sizeof v);
    }
};

// The Wwise side, driven by tables so a test can make truth and cache disagree deliberately.
std::vector<std::pair<uint32_t, uint32_t>> g_wwiseStates;
std::vector<std::pair<uint32_t, uint32_t>> g_wwiseSwitches;
uint32_t g_setStateCalls = 0;
uint32_t g_setSwitchCalls = 0;

int32_t FakeGetState(uint32_t group, uint32_t* out) {
    for (auto& p : g_wwiseStates) {
        if (p.first == group) {
            *out = p.second;
            return kAkSuccess;
        }
    }
    return kAkNotInitialized;
}
int32_t FakeSetState(uint32_t group, uint32_t value) {
    ++g_setStateCalls;
    for (auto& p : g_wwiseStates) {
        if (p.first == group) {
            p.second = value;
            return kAkSuccess;
        }
    }
    g_wwiseStates.push_back({group, value});
    return kAkSuccess;
}
int32_t FakeGetSwitch(uint32_t group, uint64_t, uint32_t* out) {
    for (auto& p : g_wwiseSwitches) {
        if (p.first == group) {
            *out = p.second;
            return kAkSuccess;
        }
    }
    return kAkNotInitialized;
}
int32_t FakeSetSwitch(uint32_t group, uint32_t value, uint64_t) {
    ++g_setSwitchCalls;
    for (auto& p : g_wwiseSwitches) {
        if (p.first == group) {
            p.second = value;
            return kAkSuccess;
        }
    }
    g_wwiseSwitches.push_back({group, value});
    return kAkSuccess;
}

// A single sounding voice on whatever game object is asked about, so the loop tracker has
// something to track. Playing id 42, event 0xF00D.
int32_t FakeGetPlayingIDs(uint64_t, uint32_t* count, uint32_t* out) {
    if (out == nullptr) {
        *count = 1;
        return kAkSuccess;
    }
    if (*count >= 1) {
        out[0] = 42;
        *count = 1;
    }
    return kAkSuccess;
}
uint32_t FakeGetEventId(uint32_t playingId) { return playingId == 42 ? 0xF00Du : 0u; }

AkExports FakeExports() {
    AkExports ak{};
    ak.GetState = &FakeGetState;
    ak.SetState = &FakeSetState;
    ak.GetSwitch = &FakeGetSwitch;
    ak.SetSwitch = &FakeSetSwitch;
    return ak;
}

}  // namespace

TEST_CASE("the state binding walks the memo the way the engine does") {
    ResetAudioStateForTest();
    g_wwiseStates.clear();
    g_wwiseSwitches.clear();
    g_setStateCalls = 0;
    g_setSwitchCalls = 0;

    FakeSoundPlayer sp;
    sp.SetStates({{0x11u, 0xAAu}, {0x22u, 0xBBu}, {0x33u, 0xCCu}});
    sp.SetEmitter(7, {{0x77u, 0x99u}});
    REQUIRE(InitAudioState(FakeExports(), sp.Base()));
    IAudioState* state = AudioState();
    REQUIRE(state != nullptr);

    SUBCASE("the group list comes from the memo and the VALUES come from Wwise") {
        // The distinction is the whole of N-16: the memo is what CAN be wrong.
        g_wwiseStates = {{0x11u, 0xAAu}, {0x22u, 0x99u}, {0x33u, 0xCCu}};
        AudioStatePair out[8];
        uint32_t total = 0;
        const size_t n = state->SnapshotStates(out, 8, &total);
        REQUIRE(n == 3);
        CHECK(total == 3);
        CHECK(out[1].group == 0x22u);
        CHECK(out[1].value == 0x99u);   // Wwise's value, not the memo's 0xBB
    }

    SUBCASE("a cap reports the total it saw alongside the count it wrote") {
        AudioStatePair out[2];
        uint32_t total = 0;
        CHECK(state->SnapshotStates(out, 2, &total) == 2);
        CHECK(total == 3);
    }

    SUBCASE("the per-emitter switch memo is walked too") {
        g_wwiseSwitches = {{0x77u, 0x99u}};
        AudioSwitchTriple out[8];
        uint32_t total = 0;
        REQUIRE(state->SnapshotSwitches(out, 8, &total) == 1);
        CHECK(total == 1);
        CHECK(out[0].gameObj == 7u);
        CHECK(out[0].group == 0x77u);
        CHECK(out[0].value == 0x99u);
    }
}

TEST_CASE("??? N-16: the reconcile drives Wwise to the target AND corrects the cache") {
    ResetAudioStateForTest();
    g_wwiseStates.clear();
    g_wwiseSwitches.clear();
    g_setStateCalls = 0;

    FakeSoundPlayer sp;
    sp.SetStates({{0x11u, 0xAAu}, {0x22u, 0xBBu}});
    REQUIRE(InitAudioState(FakeExports(), sp.Base()));
    IAudioState* state = AudioState();

    SUBCASE("Wwise disagrees with the target: it is driven, and the cache follows") {
        g_wwiseStates = {{0x11u, 0xAAu}, {0x22u, 0xBBu}};
        const AudioStatePair want[] = {{0x11u, 0xEEu}};
        state->SetTargets(want, 1, nullptr, 0);
        state->Reconcile();

        Symbol64 truth = 0;
        REQUIRE(state->GetStateTruth(0x11u, truth));
        CHECK(truth == 0xEEu);
        CHECK(sp.CachedValue(0x11u) == 0xEEu);
        CHECK(state->LastReconcile().statesCorrectedInWwise == 1);
    }

    SUBCASE("??? Wwise ALREADY agrees but the cache does not -- the case that makes a backwards seek "
            "permanently wrong, and the one a target-only reconcile would miss") {
        // The engine's memo says 0xBB for group 0x22. Wwise actually holds 0x77. Nothing in the
        // replay's target set mentions 0x22. Leave it and the next time the GAME sets 0x22 to 0xBB
        // its own setter short-circuits and Wwise stays at 0x77 for the rest of the session.
        g_wwiseStates = {{0x11u, 0xAAu}, {0x22u, 0x77u}};
        state->SetTargets(nullptr, 0, nullptr, 0);
        state->Reconcile();

        CHECK(sp.CachedValue(0x22u) == 0x77u);
        CHECK(state->LastReconcile().memoEntriesCorrected == 1);
    }

    SUBCASE("the mutation that must go red: skip the cache write and the memo stays stale") {
        // Written out rather than described. This is what the code would do without the
        // WriteMemoValue call, and the assertion above is what catches it.
        g_wwiseStates = {{0x11u, 0xAAu}, {0x22u, 0x77u}};
        const uint32_t before = sp.CachedValue(0x22u);
        CHECK(before == 0xBBu);
        // ... with no Reconcile() call at all, the cache is untouched:
        CHECK(sp.CachedValue(0x22u) == 0xBBu);
    }

    SUBCASE("SetState reaches Wwise with the real value and no intermediate one") {
        // The design's sentinel would show up here as two SetState calls, the first with a value
        // nobody asked for. One call, with the target, is the whole point of not using it.
        g_wwiseStates = {{0x11u, 0xAAu}};
        g_setStateCalls = 0;
        CHECK(state->SetState(0x11u, 0x55u));
        CHECK(g_setStateCalls == 1);
        Symbol64 truth = 0;
        state->GetStateTruth(0x11u, truth);
        CHECK(truth == 0x55u);
    }

    SUBCASE("an id wider than 32 bits is refused, never truncated into a state nobody asked for") {
        CHECK_FALSE(state->SetState(0x1'0000'0011ull, 0x55u));
    }
}

// ==============================================================================================
// The residue battery
// ==============================================================================================

namespace {

class NullAudioState final : public IAudioState {
public:
    uint32_t reconciles = 0;
    uint32_t pins = 0;
    bool SetState(Symbol64, Symbol64) override { return true; }
    bool SetSwitch(Symbol64, Symbol64, uint64_t) override { return true; }
    bool GetStateTruth(Symbol64, Symbol64&) const override { return false; }
    bool GetSwitchTruth(Symbol64, uint64_t, Symbol64&) const override { return false; }
    void Reconcile() override { ++reconciles; }
    void PinRandom(uint32_t, const Blob&) override { ++pins; }
    bool ReadRandomPins(uint32_t&, Blob&) const override { return false; }
    size_t SnapshotStates(AudioStatePair*, size_t, uint32_t*) const override { return 0; }
    size_t SnapshotSwitches(AudioSwitchTriple*, size_t, uint32_t*) const override { return 0; }
    size_t SnapshotLoops(AudioLoopPin*, size_t, uint32_t*) const override { return 0; }
    void SetTargets(const AudioStatePair*, size_t, const AudioSwitchTriple*, size_t) override {}
    const ReconcileReport& LastReconcile() const override { return report_; }

private:
    ReconcileReport report_{};
};

class SpyRate final : public IAudioRate {
public:
    uint32_t suspends = 0, wakeups = 0;
    bool Available() const override { return true; }
    float MaxRate() const override { return 1.0f; }
    void SetRate(float) override {}
    void Suspend(bool) override { ++suspends; }
    void Wakeup() override { ++wakeups; }
    double AudioSeconds() const override { return 0.0; }
    const char* Diagnostic() const override { return ""; }
    int64_t DisplayBiasSceneUs() const override { return 0; }
    const char* RateLabel() const override { return ""; }
};

uint32_t g_stopAlls = 0;
uint32_t g_posts = 0;
uint32_t g_seeks = 0;
void FakeStopAll(uint64_t) { ++g_stopAlls; }
int32_t FakeRenderAudio(bool) { return kAkSuccess; }
uint32_t FakePostEvent(uint32_t, uint64_t, uint32_t, void (*)(int32_t, void*), void*, uint32_t,
                       void*, uint32_t) {
    ++g_posts;
    return 1;
}
// AK_INVALID_PLAYING_ID for one nominated event: the event is not loaded, or the game object is no
// longer registered after the jump. Wwise reports that by returning 0, not by failing.
uint32_t g_refusePostEvent = 0;
uint32_t FakePostEventRefusing(uint32_t eventId, uint64_t, uint32_t, void (*)(int32_t, void*),
                               void*, uint32_t, void*, uint32_t) {
    ++g_posts;
    return eventId == g_refusePostEvent ? 0u : 1u;
}
int32_t FakeSeekOnEvent(uint32_t, uint64_t, int32_t, bool, uint32_t) {
    ++g_seeks;
    return kAkSuccess;
}

playback::Discontinuity Jump(playback::Cause cause, bool preroll) {
    playback::Discontinuity d;
    d.cause = cause;
    d.needs_preroll = preroll;
    return d;
}

}  // namespace

TEST_CASE("the audio residue is reachable only from the discontinuity flag, and runs on the right "
          "causes") {
    ResetAudioResidueForTest();
    g_stopAlls = g_posts = g_seeks = 0;

    NullAudioState state;
    SpyRate rate;
    AkExports ak{};
    ak.StopAll = &FakeStopAll;
    ak.RenderAudio = &FakeRenderAudio;
    ak.PostEvent = &FakePostEvent;
    ak.SeekOnEventMs = &FakeSeekOnEvent;

    AudioResidueConfig cfg;

    playback::ResidueBattery battery;
    REQUIRE(RegisterAudioResidue(battery, BindResidueState(&state), &rate, cfg) == 4);
    REQUIRE(battery.Count() == 4);
    CHECK(battery.UnavailableCount() == 0);

    SUBCASE("S10.3's order, with the accumulator rebase ahead of it") {
        CHECK(std::string(battery.At(0)->Id()) == "N-5a");
        CHECK(std::string(battery.At(1)->Id()) == "N-17a");
        CHECK(std::string(battery.At(2)->Id()) == "N-16");
        CHECK(std::string(battery.At(3)->Id()) == "N-17b");
    }

    SUBCASE("??? a micro-seek does NOT stop everything -- above the ceiling that happens every frame") {
        battery.Fire(Jump(playback::Cause::MicroSeek, false));
        CHECK(g_stopAlls == 0);
        CHECK(rate.suspends == 0);
    }

    SUBCASE("??? a reverse tick does NOT stop everything either, and at 90 Hz that matters") {
        battery.Fire(Jump(playback::Cause::ReverseTick, false));
        CHECK(g_stopAlls == 0);
    }

    SUBCASE("a full seek mutes, reconciles, restores the pins and unmutes -- and stops nothing") {
        AudioPins pins;
        pins.loopCount = 3;
        pins.loops[0] = {7, 0x1111u, 500, 9000};
        pins.loops[1] = {7, 0x2222u, 100, 250};
        pins.loops[2] = {7, 0x3333u, -1, 9000};
        SetAudioPins(pins);

        battery.Fire(Jump(playback::Cause::Seek, true));
        CHECK(rate.suspends == 1);
        CHECK(rate.wakeups == 1);
        CHECK(state.reconciles == 1);
        CHECK(state.pins == 1);
        CHECK(g_stopAlls == 0);
        CHECK(g_posts == 0);
        CHECK(g_seeks == 0);
    }

    SUBCASE("a held scrub mutes and reconciles, and restores no pins until release") {
        for (int i = 0; i < 10; ++i) {
            battery.Fire(Jump(playback::Cause::Seek, false));
        }
        CHECK(g_stopAlls == 0);
        CHECK(g_posts == 0);
        CHECK(state.pins == 0);
        CHECK(state.reconciles == 10);
    }

    SUBCASE("??? the reconcile is cheap on a reverse tick until the target set actually changes") {
        for (int i = 0; i < 100; ++i) {
            battery.Fire(Jump(playback::Cause::ReverseTick, false));
        }
        CHECK(state.reconciles == 1);   // the first one; the other 99 had nothing new to apply

        AudioPins pins;
        SetAudioPins(pins);             // the window resolved a new keyframe
        battery.Fire(Jump(playback::Cause::ReverseTick, false));
        CHECK(state.reconciles == 2);
    }

    SUBCASE("a drag holds the mute across many jumps and releases it once") {
        HoldAudioSeekMute(true);
        for (int i = 0; i < 5; ++i) {
            battery.Fire(Jump(playback::Cause::Seek, false));
        }
        CHECK(rate.suspends == 1);
        CHECK(rate.wakeups == 0);
        HoldAudioSeekMute(false);
        CHECK(rate.wakeups == 1);
    }
}

// FIX-AUDIO-B. Live carries one engine-posted loop on every player for the whole match; the old
// N-17a StopAll killed all eight at a replay's first seek and nothing could re-post them. The
// invariant: whatever the transport does, the engine's voices after it are the voices before it.
namespace voiceset {
std::set<std::pair<uint64_t, uint32_t>> g_voices;   // (game object, event)
void StopAll(uint64_t obj) {
    for (auto it = g_voices.begin(); it != g_voices.end();) {
        it = (obj == kAkInvalidGameObject || it->first == obj) ? g_voices.erase(it) : std::next(it);
    }
}
uint32_t Post(uint32_t eventId, uint64_t obj, uint32_t, void (*)(int32_t, void*), void*, uint32_t,
              void*, uint32_t) {
    g_voices.insert({obj, eventId});
    return 1;
}
}  // namespace voiceset

TEST_CASE("FIX-AUDIO-B: no transport action changes the set of engine-owned loops") {
    ResetAudioResidueForTest();
    NullAudioState state;
    SpyRate rate;
    playback::ResidueBattery battery;
    REQUIRE(RegisterAudioResidue(battery, BindResidueState(&state), &rate, AudioResidueConfig{}) == 4);

    constexpr uint32_t kPerPawnLoop = 4076609996u;
    constexpr uint32_t kLevelAmbient = 2378928745u;
    voiceset::g_voices.clear();
    for (uint64_t pawn = 963; pawn <= 970; ++pawn) voiceset::g_voices.insert({pawn, kPerPawnLoop});
    voiceset::g_voices.insert({1024, kLevelAmbient});
    const auto before = voiceset::g_voices;

    AudioPins pins;
    pins.loopCount = 1;
    pins.loops[0] = {kPerPawnLoop, 963, -1, 130000};   // what the file says, with no phase
    SetAudioPins(pins);

    const playback::Cause causes[] = {playback::Cause::Seek, playback::Cause::GapCross,
                                      playback::Cause::MicroSeek, playback::Cause::ReverseTick,
                                      playback::Cause::ChapterEpochRoll};
    for (playback::Cause c : causes) {
        battery.Fire(Jump(c, true));
        battery.Fire(Jump(c, false));
        CHECK(voiceset::g_voices == before);
    }
    HoldAudioSeekMute(true);   // a drag spanning jumps, released at the end
    for (int i = 0; i < 5; ++i) battery.Fire(Jump(playback::Cause::Seek, false));
    battery.Fire(Jump(playback::Cause::Seek, true));
    HoldAudioSeekMute(false);
    CHECK(voiceset::g_voices == before);
    CHECK(rate.suspends == rate.wakeups);   // every mute taken was released
}

TEST_CASE("an unavailable primitive registers and reports, rather than vanishing") {
    ResetAudioResidueForTest();
    NullAudioState state;
    SpyRate rate;
    AkExports empty{};   // nothing bound: the -noaudio shape

    playback::ResidueBattery battery;
    REQUIRE(RegisterAudioResidue(battery, BindResidueState(&state), nullptr, AudioResidueConfig{}) == 4);
    // ??? ONE, not two. N-17b is deliberately always Ready: it owns the release of the mute N-17a
    // takes, and a row that could be SKIPPED while the mute stood would leave the replay silent.
    // Its degraded half -- the loop re-arm -- reports through a counter and a diagnostic instead,
    // because an availability enum cannot express "half of this works".
    CHECK(battery.UnavailableCount() == 1);   // N-17a alone: StopAll and RenderAudio did not bind
    const char* ids[4] = {};
    REQUIRE(battery.UnavailableIds(ids, 4) == 1);
    REQUIRE(ids[0] != nullptr);
    CHECK(std::string(ids[0]) == "N-17a");
    // "It is unavailable" is not a diagnosis; every not-Ready row has to say why.
    for (size_t i = 0; i < battery.Count(); ++i) {
        if (battery.At(i)->Avail() != playback::Availability::Ready) {
            CHECK(strlen(battery.At(i)->Diagnostic()) > 0);
        }
    }
}

// ==============================================================================================
// The XAudio2 vtable, which is the one layout this packet cannot get from a struct declaration
// alone
// ==============================================================================================

TEST_CASE("the source-voice vtable places SetFrequencyRatio where the design says it is") {
    // The static_asserts in sinkvoice.h are the real check and they run at compile time; this case
    // exists so the relationship is also visible in a test report, and so that a build which
    // somehow relaxed them still fails here.
    CHECK(offsetof(XAudio2SourceVoiceVtbl, DestroyVoice) == 0x90);
    CHECK(offsetof(XAudio2SourceVoiceVtbl, SetFrequencyRatio) == 0xD0);
    CHECK(offsetof(XAudio2SourceVoiceVtbl, GetState) == 0xC8);
    CHECK(sizeof(XAudio2SourceVoiceVtbl) == 29 * sizeof(void*));
}

// ==============================================================================================
// A6 -- inherited from W1-G, which could only range-check the selector
// ==============================================================================================
//
// AUDIO.XAUDIO2_VER_SELECTOR decides how XAUDIO2_VOICE_DETAILS is read, and it is the one struct
// whose shape moved between 2.7 and 2.8. A selector of 3 with XAudio2_7 mapped is IN RANGE and
// still wrong: the channel count is read out of the ActiveFlags slot and the sample rate out of the
// channel count, and both land inside the plausibility window often enough to pass. So the whole
// selector x mapped-set space is driven here, including the combinations a real process will not
// produce -- those are the ones a wrong table would hide in.
TEST_CASE("A6: the version selector is checked against the runtime that is actually mapped") {
    SUBCASE("agreement, disagreement and 'nothing mapped' are three answers, not two") {
        CHECK(ClassifyXAudio2Module(3, kXAudio2Mapped29) == XAudio2ModuleCheck::Agrees);
        CHECK(ClassifyXAudio2Module(2, kXAudio2Mapped28) == XAudio2ModuleCheck::Agrees);
        CHECK(ClassifyXAudio2Module(1, kXAudio2Mapped27) == XAudio2ModuleCheck::Agrees);

        // The silent-wrongness case, and the reason this arm exists.
        CHECK(ClassifyXAudio2Module(3, kXAudio2Mapped27) == XAudio2ModuleCheck::Mismatch);
        CHECK(ClassifyXAudio2Module(1, kXAudio2Mapped29) == XAudio2ModuleCheck::Mismatch);
        CHECK(ClassifyXAudio2Module(2, kXAudio2Mapped27 | kXAudio2Mapped29) ==
              XAudio2ModuleCheck::Mismatch);

        // Not agreement and not disagreement. It must not read as either.
        CHECK(ClassifyXAudio2Module(3, 0) == XAudio2ModuleCheck::NoneMapped);
        CHECK(ClassifyXAudio2Module(1, 0) == XAudio2ModuleCheck::NoneMapped);

        // An out-of-range selector belongs to UnknownXAudio2Version; this must not claim it.
        CHECK(ClassifyXAudio2Module(0, kXAudio2Mapped29) == XAudio2ModuleCheck::NotChecked);
        CHECK(ClassifyXAudio2Module(4, kXAudio2Mapped29) == XAudio2ModuleCheck::NotChecked);
    }

    SUBCASE("the whole 3 x 8 space, so no combination is decided by accident") {
        size_t agrees = 0;
        size_t mismatch = 0;
        size_t none = 0;
        for (uint32_t sel = 1; sel <= 3; ++sel) {
            for (uint32_t mask = 0; mask < 8; ++mask) {
                const XAudio2ModuleCheck c = ClassifyXAudio2Module(sel, mask);
                const bool selectorsOwnBit = (mask & (1u << (sel - 1))) != 0;
                if (mask == 0) {
                    CHECK(c == XAudio2ModuleCheck::NoneMapped);
                    ++none;
                } else if (selectorsOwnBit) {
                    CHECK(c == XAudio2ModuleCheck::Agrees);
                    ++agrees;
                } else {
                    CHECK(c == XAudio2ModuleCheck::Mismatch);
                    ++mismatch;
                }
            }
        }
        // ??? Every outcome is reached. A sweep of a space that only ever produced one answer would
        // agree with a classifier that returns a constant.
        CHECK(agrees == 12);
        CHECK(mismatch == 9);
        CHECK(none == 3);
    }

    SUBCASE("each state has a distinct name, so a report cannot collapse two of them") {
        CHECK(strcmp(XAudio2ModuleCheckName(XAudio2ModuleCheck::Agrees),
                     XAudio2ModuleCheckName(XAudio2ModuleCheck::NoneMapped)) != 0);
        CHECK(strcmp(XAudio2ModuleCheckName(XAudio2ModuleCheck::Mismatch),
                     XAudio2ModuleCheckName(XAudio2ModuleCheck::NoneMapped)) != 0);
        CHECK(strstr(SinkVerdictName(SinkVerdict::XAudio2ModuleMismatch), "not the one mapped") !=
              nullptr);
        CHECK(strcmp(SinkVerdictName(SinkVerdict::XAudio2ModuleMismatch),
                     SinkVerdictName(SinkVerdict::UnknownXAudio2Version)) != 0);
    }
}

// ==============================================================================================
// The sink wait -- and the row marked "launch 13" is why this arm exists
// ==============================================================================================
//
// On 2026-08-30 the in-game probe filed FAIL with four checks against a ceiling of eight, and two
// of those four were one condition counted twice. Its artifact records exactly what it saw:
// "0 proven, 0 rejected, first refusal: the XAudio2 version selector is not 1, 2 or 3". Both of the
// globals that refusal reads live in the zero-initialised tail of .data, beyond SizeOfRawData, so 0
// there means CAkSinkXAudio2::Init had not run -- there was no sink to prove, and a sink proof that
// was never performed did not fail. The classifier is driven below with those recorded inputs, and
// must call it could-not-ask.
TEST_CASE("the sink wait separates 'it was not there' from 'it is there and it disagrees'") {
    SUBCASE("launch 13's recorded inputs, replayed") {
        CHECK(ClassifySinkWait(0, SinkVerdict::UnknownXAudio2Version) ==
              SinkWaitOutcome::NeverAppeared);
        // The same run one refusal later, had the selector been in range: an empty list head is
        // -noaudio or a device that is not up, and neither is a failed proof either.
        CHECK(ClassifySinkWait(0, SinkVerdict::NoSinkList) == SinkWaitOutcome::NeverAppeared);
    }

    SUBCASE("a sink that exists and disagrees is a real failure and must not be softened") {
        CHECK(ClassifySinkWait(0, SinkVerdict::IfaceMismatch) == SinkWaitOutcome::Refused);
        CHECK(ClassifySinkWait(0, SinkVerdict::VoiceUnreadable) == SinkWaitOutcome::Refused);
        CHECK(ClassifySinkWait(0, SinkVerdict::XAudio2ModuleMismatch) == SinkWaitOutcome::Refused);
        CHECK(ClassifySinkWait(0, SinkVerdict::DetailsImplausible) == SinkWaitOutcome::Refused);
        CHECK(ClassifySinkWait(0, SinkVerdict::RatioOutOfDomain) == SinkWaitOutcome::Refused);
    }

    SUBCASE("one proven sink outranks any refusal, because a chain can hold both") {
        CHECK(ClassifySinkWait(1, SinkVerdict::VoiceUnreadable) == SinkWaitOutcome::Proven);
        CHECK(ClassifySinkWait(2, SinkVerdict::Ok) == SinkWaitOutcome::Proven);
    }

    // ??? Every verdict is classified, and the count is asserted so a NEW SinkVerdict cannot inherit
    // an arm by default. Adding one reddens this line, which is the only place that forces someone
    // to decide whether it means "not there" or "there and wrong".
    SUBCASE("the whole verdict space, with no default arm") {
        CHECK(static_cast<size_t>(SinkVerdict::Count) == 12);
        size_t never = 0;
        size_t refused = 0;
        for (size_t v = 0; v < static_cast<size_t>(SinkVerdict::Count); ++v) {
            const SinkWaitOutcome o = ClassifySinkWait(0, static_cast<SinkVerdict>(v));
            CHECK(o != SinkWaitOutcome::Proven);   // nothing proven cannot classify as proven
            if (o == SinkWaitOutcome::NeverAppeared) {
                ++never;
            } else {
                ++refused;
            }
        }
        CHECK(never == 4);     // Ok, NoImage, NoSinkList, UnknownXAudio2Version
        CHECK(refused == 8);
    }

    // ??? The reason WORDS are an interface: W2-R's rows print them verbatim as the reason a replay
    // has no rate control. A set with a hole in it publishes "unknown" for a real cause, and a set
    // with two identical strings makes two causes indistinguishable in the only place anyone reads
    // them. Both are checked over the whole enum rather than over the ones someone remembered.
    SUBCASE("the reason words are a closed set, published verbatim") {
        for (size_t v = 0; v < static_cast<size_t>(SinkVerdict::Count); ++v) {
            const char* name = SinkVerdictName(static_cast<SinkVerdict>(v));
            REQUIRE(name != nullptr);
            CAPTURE(std::string(name));
            CHECK(strcmp(name, "unknown") != 0);
            CHECK(strlen(name) > 0);
            for (size_t w = v + 1; w < static_cast<size_t>(SinkVerdict::Count); ++w) {
                CHECK(strcmp(name, SinkVerdictName(static_cast<SinkVerdict>(w))) != 0);
            }
        }
    }

    SUBCASE("each outcome has a distinct name, so a report cannot collapse two of them") {
        CHECK(strcmp(SinkWaitOutcomeName(SinkWaitOutcome::NeverAppeared),
                     SinkWaitOutcomeName(SinkWaitOutcome::Refused)) != 0);
        CHECK(strcmp(SinkWaitOutcomeName(SinkWaitOutcome::Proven),
                     SinkWaitOutcomeName(SinkWaitOutcome::Refused)) != 0);
    }
}

// ==============================================================================================
// LAUNCH 13, REPLAYED THROUGH THE PROBE'S OWN LEDGER
// ==============================================================================================
//
// -- S9.18: run the new check against the artifact it was written for. These are the recorded
// inputs of that run -- 27 of 27 AK names bound, the sound engine initialised, the sink wait ending
// on "the XAudio2 version selector is not 1, 2 or 3", and the offline-bit lock held with sites 2 of
// 2. The rules below are the ones probe_audio.cpp scores by, included from the same header rather
// than restated here, so this cannot pass while the probe does something else.
//
// The run filed FAIL with checked=4. It must now file NOT-RUN with 2 asked and 6 named as unasked.
TEST_CASE("launch 13's recorded run, scored by the ledger the probe uses") {
    using namespace replay::audio::ledger;

    Ledger l;
    l.Check(kCheckExportsBound, true);   // "AK exports: 27 of 27 resolved by name"

    const SinkWaitOutcome outcome = ClassifySinkWait(0, SinkVerdict::UnknownXAudio2Version);
    REQUIRE(outcome == SinkWaitOutcome::NeverAppeared);
    const bool couldNotAsk = true;
    SkipSinkDependents(l, "no sink object appeared inside the deadline");

    l.Check(kCheckOfflineLock, true);    // "offline-bit lock: locked ... sites 2 of 2"

    SUBCASE("the verdict is NOT-RUN, and the two checks it did ask both passed") {
        CHECK(strcmp(Verdict(l, couldNotAsk), "NOT-RUN") == 0);
        CHECK(l.Failed() == 0);
        CHECK(l.Asked() == 2);
    }

    SUBCASE("the six checks it could not ask are counted and named") {
        CHECK(l.NotAsked() == 6);
        CHECK(l.Asked() + l.NotAsked() == kCheckCeiling);
        CHECK(kCheckCeiling == 8);

        char unasked[3072];
        RenderUnasked(l, false, unasked, sizeof unasked);
        CHECK(strstr(unasked, "rate-0.5x") != nullptr);
        CHECK(strstr(unasked, "rate-0.25x") != nullptr);
        CHECK(strstr(unasked, "rate-0.1x") != nullptr);
        CHECK(strstr(unasked, "rate-baseline") != nullptr);
        CHECK(strstr(unasked, "a6-module-agrees") != nullptr);
        CHECK(strstr(unasked, "sink-proofs") != nullptr);
        // Every one carries a reason: an accepted exclusion, not a forgotten check.
        CHECK(strstr(unasked, "NO REASON WAS RECORDED") == nullptr);
    }

    SUBCASE("no token: the ladder did not run, so nothing here is Tier-A confirmed") {
        CHECK(strcmp(Token(l, true), "none") == 0);
        CHECK(strcmp(Token(l, false), "none") == 0);
    }
}

// The ledger must be able to say all three things, or NOT-RUN is only a softer word for FAIL.
TEST_CASE("the ledger's three verdicts, each reachable and each distinct") {
    using namespace replay::audio::ledger;

    SUBCASE("a sink that exists and disagrees is a FAIL, and NOT-RUN must not absorb it") {
        Ledger l;
        l.Check(kCheckExportsBound, true);
        REQUIRE(ClassifySinkWait(0, SinkVerdict::IfaceMismatch) == SinkWaitOutcome::Refused);
        l.Check(kCheckSinkProofs, false);
        CHECK(strcmp(Verdict(l, false), "FAIL") == 0);
        // And an asked-and-failed check outranks a could-not-ask: a launch whose offline-bit lock
        // did not hold is a real finding whatever the audio engine did.
        CHECK(strcmp(Verdict(l, true), "FAIL") == 0);
    }

    SUBCASE("all eight asked and none failed is PASS, and only that is Tier-A confirmed") {
        Ledger l;
        for (int i = 0; i < kCheckCeiling; ++i) {
            l.Check(static_cast<CheckId>(i), true);
        }
        CHECK(l.Asked() == kCheckCeiling);
        CHECK(l.NotAsked() == 0);
        CHECK(strcmp(Verdict(l, false), "PASS") == 0);
        CHECK(strcmp(Token(l, true), "TIER-A-CONFIRMED") == 0);
        // The same eight, measured somewhere other than the declared point, confirm nothing.
        CHECK(strcmp(Token(l, false), "none") == 0);
    }

    SUBCASE("??? PASS means every check was asked: seven held and one unasked is NOT-RUN") {
        Ledger l;
        for (int i = 0; i < kCheckCeiling - 1; ++i) {
            l.Check(static_cast<CheckId>(i), true);
        }
        REQUIRE(l.Failed() == 0);
        REQUIRE(l.NotAsked() == 1);
        // couldNotAsk false is the state the provisional record used to reach, and it said PASS.
        CHECK(strcmp(Verdict(l, false), "NOT-RUN") == 0);
    }

    SUBCASE("a check nobody scored and nobody excluded renders as forgotten, not as accepted") {
        Ledger l;
        l.Check(kCheckExportsBound, true);
        l.Skip(kCheckSinkProofs, "no sink appeared");
        char unasked[3072];
        RenderUnasked(l, false, unasked, sizeof unasked);
        CHECK(strstr(unasked, "no sink appeared") != nullptr);
        // The other six were neither asked nor excluded. That is the state that used to be invisible.
        CHECK(strstr(unasked, "NO REASON WAS RECORDED") != nullptr);
        CHECK(l.Asked() + l.NotAsked() == kCheckCeiling);
    }

    SUBCASE("the first reason wins, so a blanket exclusion cannot bury the specific one") {
        Ledger l;
        l.Skip(kCheckModuleAgrees, "the selector was out of range");
        SkipSinkDependents(l, "no sink object appeared inside the deadline");
        char unasked[3072];
        RenderUnasked(l, false, unasked, sizeof unasked);
        CHECK(strstr(unasked, "the selector was out of range") != nullptr);
    }
}

// ??? THE RECORD f870d62 LEFT BEHIND. Its provisional record, written from inside the sink wait,
// said "verdict": "PASS" with seven checks rendered as forgotten, and the packet's own gate went red
// on it. The state is replayed here through the writer the probe uses, so the shape of every record
// is proven by the code that produces it rather than by a fixture somebody typed.
TEST_CASE("a provisional record is not a verdict, and says which checks it has not reached") {
    using namespace replay::audio::ledger;

    Ledger l;
    l.Check(kCheckExportsBound, true);   // the state inside the sink wait: the sound engine is up
    Record r;
    r.probeId = "W2-H-audio";
    r.wwiseUpMs = 4100.0;
    r.sinkWaitMs = 9250.0;
    r.sinkPolls = 21;
    r.detail = "inside the sink wait";
    static char out[12288];

    SUBCASE("mid-run: the in-progress word, every unreached check pending, no token, truncation undecided") {
        r.provisional = true;
        FormatRecord(l, r, out, sizeof out);
        CHECK(strstr(out, "\"verdict\": \"IN-PROGRESS\"") != nullptr);
        CHECK(strstr(out, "\"verdict\": \"PASS\"") == nullptr);
        CHECK(strstr(out, "\"provisional\": true") != nullptr);
        CHECK(strstr(out, "\"truncated\": null") != nullptr);
        CHECK(strstr(out, "\"token\": \"none\"") != nullptr);
        CHECK(strstr(out, "\"checkCeiling\": 8, \"asked\": 1, \"notAsked\": 7") != nullptr);
        CHECK(strstr(out, kForgottenReason) == nullptr);
        size_t pending = 0;
        for (const char* p = strstr(out, kPendingReason); p != nullptr;
             p = strstr(p + 1, kPendingReason)) {
            ++pending;
        }
        CHECK(pending == 7);
    }

    SUBCASE("the same state filed as final is a NOT-RUN with seven checks forgotten, not seven pending") {
        r.provisional = false;
        FormatRecord(l, r, out, sizeof out);
        CHECK(strstr(out, "\"verdict\": \"NOT-RUN\"") != nullptr);
        CHECK(strstr(out, "\"provisional\": false") != nullptr);
        CHECK(strstr(out, kPendingReason) == nullptr);
        CHECK(strstr(out, kForgottenReason) != nullptr);
    }

    SUBCASE("the in-progress word is none of the three final words") {
        CHECK(strcmp(kInProgressVerdict, "PASS") != 0);
        CHECK(strcmp(kInProgressVerdict, "FAIL") != 0);
        CHECK(strcmp(kInProgressVerdict, "NOT-RUN") != 0);
    }
}

TEST_CASE("the declared completion point gates PASS and the token") {
    using namespace replay::audio::ledger;

    Ledger l;
    for (int i = 0; i < kCheckCeiling; ++i) {
        l.Check(static_cast<CheckId>(i), true);
    }
    Record r;
    r.probeId = "W2-H-audio";
    r.detail = "all eight held";
    static char out[12288];

    SUBCASE("eight held at a live match: PASS, TIER-A-CONFIRMED, not truncated") {
        r.completionReached = true;
        r.matchSeenMs = 61000.0;
        FormatRecord(l, r, out, sizeof out);
        CHECK(strstr(out, "\"verdict\": \"PASS\"") != nullptr);
        CHECK(strstr(out, "\"token\": \"TIER-A-CONFIRMED\"") != nullptr);
        CHECK(strstr(out, "\"completesAt\": \"match\", \"completionReached\": true, \"truncated\": false, \"matchSeenMs\": 61000") != nullptr);
    }

    SUBCASE("??? eight held but no match inside the deadline: NOT-RUN, no token, truncated -- never PASS") {
        r.completionReached = false;
        FormatRecord(l, r, out, sizeof out);
        CHECK(strstr(out, "\"verdict\": \"NOT-RUN\"") != nullptr);
        CHECK(strstr(out, "\"verdict\": \"PASS\"") == nullptr);
        CHECK(strstr(out, "\"token\": \"none\"") != nullptr);
        CHECK(strstr(out, "\"completionReached\": false, \"truncated\": true, \"matchSeenMs\": null") != nullptr);
    }

    SUBCASE("an asked-and-failed check still outranks a missed completion point") {
        l.Check(kCheckOfflineLock, false);
        r.completionReached = false;
        FormatRecord(l, r, out, sizeof out);
        CHECK(strstr(out, "\"verdict\": \"FAIL\"") != nullptr);
    }
}

TEST_CASE("the check names are one enumeration: as many as the enum, and no two alike") {
    using namespace replay::audio::ledger;
    CHECK(sizeof(kCheckNames) / sizeof(kCheckNames[0]) == kCheckCeiling);
    for (int i = 0; i < kCheckCeiling; ++i) {
        CHECK(kCheckNames[i][0] != '\0');
        for (int j = i + 1; j < kCheckCeiling; ++j) {
            CHECK(strcmp(kCheckNames[i], kCheckNames[j]) != 0);
        }
    }
}

// ==============================================================================================
// The bindings, driven with a table-driven resolver so both arms run with no game present
// ==============================================================================================

namespace {

std::vector<std::string> g_asked;
const char* g_withhold = nullptr;

void* TableResolver(const char* name, void*) {
    g_asked.push_back(name);
    if (g_withhold != nullptr && strstr(name, g_withhold) != nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000));
}

}  // namespace

TEST_CASE("Bind fills every member it has a name for, and names the first gap when it cannot") {
    size_t count = 0;
    const AkSymbolName* table = SymbolTable(&count);
    REQUIRE(count > 20);

    SUBCASE("all present") {
        g_asked.clear();
        g_withhold = nullptr;
        AkExports ak{};
        BindReport rep;
        CHECK(Bind(&ak, &rep, &TableResolver, nullptr));
        CHECK(rep.resolved == rep.requested);
        CHECK(rep.firstMissing == nullptr);
        CHECK(g_asked.size() == count);
        CHECK(ak.GetBufferTick != nullptr);
    }

    SUBCASE("one missing is reported by member name, and everything else still binds") {
        g_asked.clear();
        g_withhold = "GetBufferTick";
        AkExports ak{};
        BindReport rep;
        CHECK_FALSE(Bind(&ak, &rep, &TableResolver, nullptr));
        CHECK(rep.resolved == rep.requested - 1);
        REQUIRE(rep.firstMissing != nullptr);
        CHECK(std::string(rep.firstMissing) == "GetBufferTick");
        CHECK(ak.GetBufferTick == nullptr);
        CHECK(ak.GetAudioSettings != nullptr);
    }

    SUBCASE("every advertised name is a plausible decorated AK symbol") {
        for (size_t i = 0; i < count; ++i) {
            REQUIRE(table[i].decorated != nullptr);
            const std::string s = table[i].decorated;
            CHECK(s[0] == '?');
            CHECK(s.find("@AK@@") != std::string::npos);
        }
    }
}

// ==============================================================================================
// THE SEEK-RESIDUE AUDIT: state the audio path carries that a jump must invalidate
// ==============================================================================================
//
// Asked to look for anything in the audio path that assumes a state the residue does not restore,
// three things were carried across a discontinuity with nothing invalidating them. All three are
// audible rather than merely measurable, which is why they belong to this subsystem.
//
// INV-P is the rule they break: state genuinely carried across frames is a CACHE of
// f(file, playhead), and on a discontinuity a cache is invalidated, never updated.

TEST_CASE("F2: a partially bound Wwise must not take a seek mute it cannot release") {
    // ??? THE BUG. N-17a's readiness is StopAll+RenderAudio; N-17b's is PostEvent+SeekOnEvent. They
    // are different conditions over the same bind, so they can DIVERGE -- and ResidueBattery::Fire
    // skips a non-Ready row. Mute taken by a row that is Ready, release owned by a row that is not,
    // and the replay is silent for the rest of the session after its first seek. Nothing reports it
    // as a failure: the battery counts a skip, and silence is not an error anywhere.
    ResetAudioResidueForTest();
    g_stopAlls = g_posts = g_seeks = 0;

    NullAudioState state;
    SpyRate rate;
    AkExports partial{};
    partial.StopAll = &FakeStopAll;
    partial.RenderAudio = &FakeRenderAudio;
    // PostEvent and SeekOnEventMs deliberately absent -- a real shape, since they are separate
    // exports and a stripped or future build can carry one set and not the other.

    playback::ResidueBattery battery;
    REQUIRE(RegisterAudioResidue(battery, BindResidueState(&state), &rate, AudioResidueConfig{}) == 4);

    battery.Fire(Jump(playback::Cause::Seek, true));
    CHECK(rate.suspends == rate.wakeups);   // every mute taken is a mute released
    CHECK(rate.wakeups > 0);

    SUBCASE("and a second seek does not stack a mute either") {
        battery.Fire(Jump(playback::Cause::Seek, true));
        CHECK(rate.suspends == rate.wakeups);
    }
}

TEST_CASE("F1: the audio clock lock must not integrate across a discontinuity") {
    // ??? THE BUG. RateLock integrates (audioSeconds - sceneSeconds) - errorBase_, where errorBase_
    // is taken once at prime time. That is exactly the S7.5.5 N-5 shape: an accumulator computing
    // clockTime - base. Nothing re-primed it on a jump, so after one the loop is correcting a
    // difference that a seek created rather than one the device drifted -- and it corrects it by
    // detuning the mix, which is the audible failure mode.
    RateLock lock;
    lock.Update(1.0f, 100.0, 0.0, 0.02);          // prime: base = 100
    for (int i = 0; i < 100; ++i) {
        lock.Update(1.0f, 100.0 + 0.02 * i + 0.5, 0.02 * i, 0.02);   // half a second of error
    }
    REQUIRE(lock.Trim() < -0.001f);               // the loop has wound up, correctly so far

    lock.Reprime();
    CHECK(lock.Trim() == 0.0f);                   // the correction is abandoned, not carried
    // And the next sample re-takes the base rather than measuring against the old one, so a jump
    // of any size produces no correction at all.
    CHECK(lock.Update(1.0f, 5000.0, 0.0, 0.02) == 1.0f);
    CHECK(lock.Trim() == 0.0f);
}

TEST_CASE("F3: sustainedMs cannot underflow when the clock runs backwards") {
    // ??? THE BUG. sustainedMs was lastSeenMs - firstSeenMs on uint32, fed from the engine sim clock
    // -- which the transport drives and which DECREASES under reverse playback and a backward seek.
    // One underflow and a voice that has been sounding for 3 ms reports about 4.29e9 ms, which is
    // past every plausible loop threshold, so every one-shot in the file re-fires on the next seek.
    // That is precisely the machine-gunning S10.3 forbids, arrived at from the opposite direction.
    ResetAudioStateForTest();
    g_wwiseStates.clear();

    FakeSoundPlayer sp;
    sp.SetStates({{0x11u, 0xAAu}});
    sp.SetEmitter(7, {});
    AkExports ak = FakeExports();
    ak.GetPlayingIDsFromGameObject = &FakeGetPlayingIDs;
    ak.GetEventIDFromPlayingID = &FakeGetEventId;
    REQUIRE(InitAudioState(ak, sp.Base()));
    IAudioState* state = AudioState();

    AudioTrackVoices(5000);      // the voice is first seen at t = 5 s
    AudioTrackVoices(3000);      // ... and the playhead has since gone BACKWARDS

    AudioLoopPin loops[8];
    uint32_t total = 0;
    const size_t n = state->SnapshotLoops(loops, 8, &total);
    REQUIRE(n == 1);
    CHECK(loops[0].sustainedMs < 1000u);   // a duration, not an underflow

    SUBCASE("and a discontinuity drops the tracker, because it is a cache of a time window") {
        ResetAudioResidueForTest();
        SpyRate rate;
        playback::ResidueBattery battery;
        REQUIRE(RegisterAudioResidue(battery, BindResidueState(state), &rate, AudioResidueConfig{}) == 4);
        battery.Fire(Jump(playback::Cause::ReverseTick, false));

        uint32_t after = 0;
        CHECK(state->SnapshotLoops(loops, 8, &after) == 0);
        CHECK(after == 0);
    }
}

// ==============================================================================================
// THE REVERSE-PLAYBACK SWEEP: unsigned and derived time arithmetic that a backward clock breaks
// ==============================================================================================
//
// F3 was an unsigned subtraction that underflowed when the clock ran backwards. Reverse is a
// direction almost nothing here has been tested against, so this is the rest of the sweep over the
// audio path. Two more, and the first is the one that matters: it makes the two-oracle design of
// S12.4 report the exact misdiagnosis it exists to prevent.
//
// The frame delta itself is NOT one of them -- W1-D's AuditFrame already refuses a regressed sim
// time, sets the delta to zero and counts it. That was checked rather than assumed.

TEST_CASE("F4: a counter that reset is not a measurement, and must not read as a broken mechanism") {
    // ??? THE BUG. SampleOracle takes `samples - lastSamples_` over XAUDIO2_VOICE_STATE::SamplesPlayed
    // summed across sinks, as a u64 subtraction. SamplesPlayed is PER VOICE and restarts at zero
    // when the voice is destroyed and recreated -- which is exactly what a device change does
    // (E-A1), and what any ReplaceOutput does. The subtraction underflows to about 1.8e19, the
    // device ratio comes out astronomically large, and JudgeRate reports FailMechanism.
    //
    // ??? THAT IS THE MISDIAGNOSIS THE TWO ORACLES EXIST TO PREVENT, arrived at from underneath: the
    // whole point of reading XAudio2's own domain was to tell "the ratio did not take" apart from a
    // machine problem, and here a DEVICE CHANGE is reported as the mechanism being broken. Someone
    // would be sent to fix working code by the instrument built to stop that happening.
    SUBCASE("StepForward refuses a source that went backwards") {
        double delta = 0.0;
        CHECK(StepForward(1000.0, 1500.0, &delta));
        CHECK(delta == doctest::Approx(500.0));

        CHECK_FALSE(StepForward(1500.0, 0.0, &delta));       // the voice was recreated
        CHECK_FALSE(StepForward(4294967000.0, 100.0, &delta));  // GetBufferTick wrapped its u32
        CHECK(StepForward(1000.0, 1000.0, &delta));          // a stalled counter is a real reading
        CHECK(delta == 0.0);
    }

    SUBCASE("a regressed source is NOT-RUN, never FAIL") {
        RateSample s;
        s.settingsValid = true;
        s.sourceRegressed = true;
        // Without the flag these numbers are an open-and-shut mechanism failure, which is the trap:
        // the values look exactly like a ratio that did not take.
        s.wwiseRatio = 1.0;
        s.deviceRatio = 1.0;
        CHECK(JudgeRate(s, 0.25, 0.02) == RateVerdict::NotRunCounterReset);

        s.sourceRegressed = false;
        CHECK(JudgeRate(s, 0.25, 0.02) == RateVerdict::FailMechanism);
    }

    SUBCASE("and it does not swallow a real failure -- the flag only fires when a counter moved back") {
        RateSample s;
        s.settingsValid = true;
        s.sourceRegressed = false;
        s.wwiseRatio = 0.25;
        s.deviceRatio = 0.25;
        CHECK(JudgeRate(s, 0.25, 0.02) == RateVerdict::Pass);
    }
}

TEST_CASE("F5: the loop tracker rebases when the clock regresses, rather than freezing a voice") {
    // ??? THE BUG, and it is the half of F3 that saturation alone does not fix. Saturating the
    // subtraction stops the 4.29e9 report, but firstSeenMs is still a stamp from the FUTURE: the
    // voice reads as zero-sustained for as long as it takes the clock to climb back past it, and if
    // it never does, forever. A loop that should be re-armed on every seek is silently never
    // re-armed, which is the opposite failure and just as inaudible to a test that only checks for
    // the overflow.
    //
    // N-5a drops the tracker on a discontinuity and covers the replay case. This covers the case
    // N-5a cannot see: the engine's own clock regressing with no discontinuity raised -- which
    // W1-D's AuditFrame counts as simRegressions, so it is a thing that happens.
    ResetAudioStateForTest();
    g_wwiseStates.clear();

    FakeSoundPlayer sp;
    sp.SetStates({{0x11u, 0xAAu}});
    sp.SetEmitter(7, {});
    AkExports ak = FakeExports();
    ak.GetPlayingIDsFromGameObject = &FakeGetPlayingIDs;
    ak.GetEventIDFromPlayingID = &FakeGetEventId;
    REQUIRE(InitAudioState(ak, sp.Base()));
    IAudioState* state = AudioState();

    AudioTrackVoices(5000);
    AudioTrackVoices(3000);    // regressed: firstSeenMs is now in the future
    AudioTrackVoices(3500);    // and the clock resumes forward from there

    AudioLoopPin loops[8];
    uint32_t total = 0;
    REQUIRE(state->SnapshotLoops(loops, 8, &total) == 1);
    // Rebased at 3000, so 500 ms of observation -- not 0, which is what a frozen future stamp gives.
    CHECK(loops[0].sustainedMs == 500u);
}

// ==============================================================================================
// The transport -> IAudioRate link (S10.4.4): read every tick, commanded on change
// ==============================================================================================

namespace {

class FakeTransport final : public IReplayTransport {
public:
    TransportState state = TransportState::Play;
    float rate = 1.0f;
    uint64_t duration = 60000000;

    bool Load(const wchar_t*) override { return true; }
    void Unload() override { duration = 0; }
    void Play(float r) override { rate = r; state = TransportState::Play; }
    void Pause() override { state = TransportState::Pause; }
    void Step(int) override {}
    void Seek(int64_t) override {}
    bool SeekRound(RoundNumber) override { return false; }
    void SetMode(bool) override {}
    TransportState State() const override { return state; }
    float Rate() const override { return rate; }
    int64_t PlayheadUs() const override { return 0; }
    uint64_t DurationUs() const override { return duration; }
    const char* ActiveModeLabel() const override { return "fake"; }
};

class CountingRate final : public IAudioRate {
public:
    std::vector<float> setRates;
    uint32_t suspends = 0, wakeups = 0;
    bool available = true;
    float maxRate = 1.0f;

    bool Available() const override { return available; }
    float MaxRate() const override { return available ? maxRate : 0.0f; }
    void SetRate(float r) override { setRates.push_back(r); }
    void Suspend(bool) override { ++suspends; }
    void Wakeup() override { ++wakeups; }
    double AudioSeconds() const override { return 0.0; }
    const char* Diagnostic() const override { return ""; }
    int64_t DisplayBiasSceneUs() const override { return 0; }
    const char* RateLabel() const override { return "label"; }
};

class FakeSeekMute final : public audio::ISeekMute {
public:
    bool held = false;
    uint32_t unloadReleases = 0;
    bool Held() const override { return held; }
    void ReleaseOnUnload() override {
        ++unloadReleases;
        held = false;
    }
};

class CountingLog final : public ILog {
public:
    uint32_t warns = 0, lines = 0;
    void Write(LogLevel level, const char*, const char*, ...) override {
        ++lines;
        if (level == LogLevel::Warn) {
            ++warns;
        }
    }
    bool Enabled(LogLevel) const override { return true; }
    void DumpLedger() override {}
};

// A sink that runs one driver tick at the instant it is suspended. N-17a takes the mute and N-17b
// hands it back inside a SINGLE battery pass, so from outside that pass there is no window at all --
// and a hold nothing can observe is a hold nothing can prove was taken. This is exactly the window
// a multi-tick pre-roll hold presents to the driver on every frame it stands.
class TickingRate final : public IAudioRate {
public:
    RateDriver* driver = nullptr;
    const IReplayTransport* transport = nullptr;
    std::vector<float> setRates;
    uint32_t suspends = 0, wakeups = 0;
    bool inTick = false;

    bool Available() const override { return true; }
    float MaxRate() const override { return 1.0f; }
    void SetRate(float r) override { setRates.push_back(r); }
    void Suspend(bool) override {
        ++suspends;
        if (driver != nullptr && !inTick) {
            inTick = true;
            driver->Drive(transport, this, AudioSeekMuteSeam(), nullptr);
            inTick = false;
        }
    }
    void Wakeup() override { ++wakeups; }
    double AudioSeconds() const override { return 0.0; }
    const char* Diagnostic() const override { return ""; }
    int64_t DisplayBiasSceneUs() const override { return 0; }
    const char* RateLabel() const override { return "label"; }
};

}  // namespace

TEST_CASE("the rate driver commands the sink once per transport change, never per tick") {
    FakeTransport t;
    CountingRate a;
    RateDriver d;

    t.Play(0.5f);
    for (int i = 0; i < 5; ++i) {
        d.Drive(&t, &a, nullptr, nullptr);
    }
    REQUIRE(a.setRates.size() == 1);
    CHECK(a.setRates[0] == 0.5f);
    CHECK(d.Stats().commands == 1);
    CHECK(a.suspends == 0);

    SUBCASE("pause is one Suspend, and play again is one SetRate at the same rate") {
        t.Pause();
        for (int i = 0; i < 5; ++i) {
            d.Drive(&t, &a, nullptr, nullptr);
        }
        CHECK(a.suspends == 1);
        CHECK(a.setRates.size() == 1);
        t.Play(0.5f);
        for (int i = 0; i < 5; ++i) {
            d.Drive(&t, &a, nullptr, nullptr);
        }
        CHECK(a.suspends == 1);
        REQUIRE(a.setRates.size() == 2);
        CHECK(a.setRates[1] == 0.5f);
        CHECK(d.Stats().commands == 3);
        CHECK(d.Stats().suspends == 1);
    }

    SUBCASE("a rate above MaxRate is commanded AND counted as a clamp; reverse and slow are not") {
        t.Play(2.0f);
        d.Drive(&t, &a, nullptr, nullptr);
        d.Drive(&t, &a, nullptr, nullptr);
        REQUIRE(a.setRates.size() == 2);
        CHECK(a.setRates[1] == 2.0f);
        CHECK(d.Stats().clamps == 1);
        t.Play(-0.5f);
        d.Drive(&t, &a, nullptr, nullptr);
        CHECK(a.setRates.back() == -0.5f);
        CHECK(d.Stats().clamps == 1);
        t.Play(1.0f);
        d.Drive(&t, &a, nullptr, nullptr);
        CHECK(d.Stats().clamps == 1);
        CHECK(d.Stats().commands == 4);
    }

    SUBCASE("an unavailable sink takes the command for its label but is not a clamp") {
        a.available = false;
        t.Play(3.0f);
        d.Drive(&t, &a, nullptr, nullptr);
        CHECK(a.setRates.size() == 2);
        CHECK(d.Stats().clamps == 0);
    }

    SUBCASE("a change during a held seek mute waits, and lands when the hold lets go") {
        FakeSeekMute mute;
        mute.held = true;
        t.Play(0.25f);
        d.Drive(&t, &a, &mute, nullptr);
        d.Drive(&t, &a, &mute, nullptr);
        CHECK(a.setRates.size() == 1);
        CHECK(d.Stats().deferred == 2);
        mute.held = false;
        d.Drive(&t, &a, &mute, nullptr);
        REQUIRE(a.setRates.size() == 2);
        CHECK(a.setRates[1] == 0.25f);
        d.Drive(&t, &a, &mute, nullptr);
        CHECK(a.setRates.size() == 2);
        CHECK(mute.unloadReleases == 0);   // a hold is released by its owner, never by the driver
    }

    SUBCASE("a non-finite rate is refused, logged once per transition, and counted while it lasts") {
        CountingLog log;
        t.Play(std::numeric_limits<float>::quiet_NaN());
        for (int i = 0; i < 5; ++i) {
            d.Drive(&t, &a, nullptr, &log);
        }
        CHECK(a.setRates.size() == 1);   // nothing is commanded off a NaN
        CHECK(d.Stats().nonFinite == 5);
        CHECK(log.warns == 1);           // one line for the transition, not one per tick
        t.Play(0.75f);
        d.Drive(&t, &a, nullptr, &log);
        REQUIRE(a.setRates.size() == 2);
        CHECK(a.setRates[1] == 0.75f);   // the refusal held nothing back once the rate was real
        t.Play(std::numeric_limits<float>::infinity());
        d.Drive(&t, &a, nullptr, &log);
        CHECK(a.setRates.size() == 2);
        CHECK(d.Stats().nonFinite == 6);
        CHECK(log.warns == 2);           // it went finite and back: that is a new transition
    }

    SUBCASE("a rate chosen while paused does not re-suspend an already suspended sink") {
        t.Pause();
        d.Drive(&t, &a, nullptr, nullptr);
        CHECK(a.suspends == 1);
        t.rate = 0.25f;   // the scrub speed the user picked without leaving pause
        for (int i = 0; i < 4; ++i) {
            d.Drive(&t, &a, nullptr, nullptr);
        }
        CHECK(a.suspends == 1);
        CHECK(d.Stats().suspends == 1);
        CHECK(d.Stats().commands == 2);   // the first SetRate and the one Suspend
        t.Play(0.25f);
        d.Drive(&t, &a, nullptr, nullptr);
        REQUIRE(a.setRates.size() == 2);
        CHECK(a.setRates[1] == 0.25f);    // and that is the rate it resumes at
    }

    SUBCASE("unloading hands the sink back at 1.0x exactly once, and a menu is never driven") {
        t.Pause();
        d.Drive(&t, &a, nullptr, nullptr);
        CHECK(a.suspends == 1);
        t.Unload();   // duration 0 and State() still Pause: the menu shape
        for (int i = 0; i < 5; ++i) {
            d.Drive(&t, &a, nullptr, nullptr);
        }
        REQUIRE(a.setRates.size() == 2);
        CHECK(a.setRates[1] == 1.0f);
        CHECK(a.suspends == 1);
        CHECK(d.Stats().releases == 1);
        for (int i = 0; i < 5; ++i) {
            d.Drive(nullptr, &a, nullptr, nullptr);
        }
        CHECK(a.setRates.size() == 2);
        CHECK(d.Stats().releases == 1);
    }
}

TEST_CASE("the rate driver does nothing before playback publishes a transport, or with no sink") {
    FakeTransport t;
    CountingRate a;
    RateDriver d;
    t.Play(0.5f);
    d.Drive(nullptr, &a, nullptr, nullptr);
    d.Drive(&t, nullptr, nullptr, nullptr);
    CHECK(a.setRates.empty());
    CHECK(a.suspends == 0);
    CHECK(d.Stats().commands == 0);

    t.duration = 0;   // loaded nothing: Pause is what State() says with no file, and it is not a command
    t.Pause();
    d.Drive(&t, &a, nullptr, nullptr);
    CHECK(a.suspends == 0);
    CHECK(d.Stats().commands == 0);
}

// ==============================================================================================
// F2 and F3: the driver's hold has a producer on the SHIPPING path, and an unload
// during a held mute does not strand the sink suspended
// ==============================================================================================

TEST_CASE("??? F2: N-17a's mute IS the hold the rate driver reads, taken on the shipping path") {
    // Before this, HoldAudioSeekMute had no caller outside the tests: AudioSeekMuteHeld() was false
    // for the whole life of a session, so the deferral below -- unit-green -- was dead code, and a
    // SetRate arriving inside the bracket woke a sink the bracket was holding silent.
    ResetAudioResidueForTest();
    g_stopAlls = g_posts = g_seeks = 0;

    NullAudioState state;
    AkExports ak{};
    ak.StopAll = &FakeStopAll;
    ak.RenderAudio = &FakeRenderAudio;
    ak.PostEvent = &FakePostEvent;
    ak.SeekOnEventMs = &FakeSeekOnEvent;

    FakeTransport t;
    RateDriver d;
    TickingRate a;
    a.driver = &d;
    a.transport = &t;

    playback::ResidueBattery battery;
    REQUIRE(RegisterAudioResidue(battery, BindResidueState(&state), &a, AudioResidueConfig{}) == 4);
    REQUIRE_FALSE(AudioSeekMuteHeld());

    t.Play(1.0f);
    d.Drive(&t, &a, AudioSeekMuteSeam(), nullptr);
    REQUIRE(a.setRates.size() == 1);

    // The ordinary scrub: the seek lands and the transport is playing at a new rate, which is the
    // change the driver would command the moment it saw it.
    t.Play(0.5f);
    battery.Fire(Jump(playback::Cause::Seek, true));
    CHECK(a.suspends == 1);
    CHECK(a.setRates.size() == 1);   // the tick that landed inside the bracket commanded nothing
    CHECK(d.Stats().deferred == 1);
    CHECK(a.wakeups == 1);           // and N-17b, not the driver, is what let go
    CHECK_FALSE(AudioSeekMuteHeld());

    d.Drive(&t, &a, AudioSeekMuteSeam(), nullptr);
    REQUIRE(a.setRates.size() == 2);
    CHECK(a.setRates[1] == 0.5f);
}

TEST_CASE("??? F3: an unload during a held seek mute drops the hold instead of leaving a suspended sink") {
    ResetAudioResidueForTest();
    g_stopAlls = g_posts = g_seeks = 0;

    NullAudioState state;
    AkExports ak{};
    ak.StopAll = &FakeStopAll;
    ak.RenderAudio = &FakeRenderAudio;
    ak.PostEvent = &FakePostEvent;
    ak.SeekOnEventMs = &FakeSeekOnEvent;

    FakeTransport t;
    RateDriver d;
    CountingRate a;

    playback::ResidueBattery battery;
    REQUIRE(RegisterAudioResidue(battery, BindResidueState(&state), &a, AudioResidueConfig{}) == 4);

    t.Play(1.0f);
    d.Drive(&t, &a, AudioSeekMuteSeam(), nullptr);
    REQUIRE(a.setRates.size() == 1);

    SUBCASE("unload arrives first: the hold dies with the replay and the sink comes back awake") {
        HoldAudioSeekMute(true);                            // the drag, held across the jump
        battery.Fire(Jump(playback::Cause::Seek, true));    // N-17a mutes; N-17b cannot release
        REQUIRE(a.suspends == 1);
        REQUIRE(a.wakeups == 0);
        REQUIRE(AudioSeekMuteHeld());

        t.Unload();
        d.Drive(&t, &a, AudioSeekMuteSeam(), nullptr);
        CHECK(a.wakeups == 1);
        REQUIRE(a.setRates.size() == 2);
        CHECK(a.setRates[1] == 1.0f);
        CHECK(d.Stats().unloadMutesDropped == 1);
        CHECK(d.Stats().releases == 1);
        CHECK_FALSE(AudioSeekMuteHeld());

        HoldAudioSeekMute(false);   // the drag's own release, now that nothing is held
        CHECK(a.wakeups == 1);
        CHECK(a.suspends == 1);
    }

    SUBCASE("release arrives first: the ordinary unload is untouched") {
        HoldAudioSeekMute(true);
        battery.Fire(Jump(playback::Cause::Seek, true));
        REQUIRE(a.suspends == 1);
        HoldAudioSeekMute(false);
        CHECK(a.wakeups == 1);
        CHECK_FALSE(AudioSeekMuteHeld());

        t.Unload();
        d.Drive(&t, &a, AudioSeekMuteSeam(), nullptr);
        REQUIRE(a.setRates.size() == 2);
        CHECK(a.setRates[1] == 1.0f);
        CHECK(d.Stats().unloadMutesDropped == 0);
        CHECK(d.Stats().releases == 1);
        CHECK(a.wakeups == 1);
    }
}

// ---- the availability state machine (L3-RUN/-2/-3 F8) ------------------------------------------
//
// Three launches reported `w2h-audio-slope-at-half-rate` as could-not-ask with "the sink is
// unavailable". The cause is entirely offline and entirely sequencing: Wwise tears CAkSinkXAudio2
// down on Suspend and rebuilds it BEHIND WakeupFromSuspend rather than inside it, so the probe
// issued in the wakeup finds no sink -- and that transient used to be recorded as a permanent
// verdict, with `Wakeup` itself gated on the flag the verdict had just cleared. One pause muted the
// session for the rest of its life. These cases drive that exact sequence against a sink whose
// appearance the test controls.
namespace {

struct FakeSink {
    int suspends = 0;
    int wakeups = 0;
    bool sinkBuilt = true;      // false models "CAkSinkXAudio2::Init has not run yet"
    uint32_t tick = 0;
    int probes = 0;
    std::vector<float> applied;
};
FakeSink g_fake;

int32_t FakeSettings(replay::audio::AkAudioSettings* s) {
    s->samplesPerFrame = 512;
    s->samplesPerSecond = 48000;
    return replay::audio::kAkSuccess;
}
uint32_t FakeTick() { return g_fake.tick; }
int32_t FakeSuspend(bool) {
    ++g_fake.suspends;
    g_fake.sinkBuilt = false;   // this is what Wwise does, and it is why the old code latched
    return replay::audio::kAkSuccess;
}
void (*g_deviceCallback)(void*, uint32_t, uint32_t, int32_t, int32_t) = nullptr;
int32_t FakeRegisterDeviceStatus(void (*cb)(void*, uint32_t, uint32_t, int32_t, int32_t)) {
    g_deviceCallback = cb;
    return replay::audio::kAkSuccess;
}
int32_t FakeWakeup() {
    ++g_fake.wakeups;
    return replay::audio::kAkSuccess;   // the sink comes back later, on Wwise's own thread
}
// Defined below, once FakeVoiceState exists. FakeProbe's chain has to carry a LIVE voice: the
// tick path reads through the proven voice now, and a stub sink with a null vtable is not a
// simplification of a sink, it is a torn-down one -- which would make every availability case here
// measure the teardown path instead of the one it was written for.
void FillProvenSink(replay::audio::ProvenSink* out);

size_t FakeProbe(const replay::audio::AkExports&, replay::audio::SinkChain* out) {
    *out = replay::audio::SinkChain{};
    ++g_fake.probes;
    if (!g_fake.sinkBuilt) {
        out->firstRefusal = replay::audio::SinkVerdict::NoSinkList;
        return 0;
    }
    out->count = 1;
    FillProvenSink(&out->sinks[0]);
    return 1;
}
size_t FakeApply(const replay::audio::AkExports&, float ratio, int32_t* firstError,
                 size_t* voicesGone) {
    if (firstError != nullptr) {
        *firstError = g_fake.sinkBuilt ? 0 : -1;
    }
    if (voicesGone != nullptr) {
        *voicesGone = 0;   // this fake refuses, it never loses a voice; the gone path has its own
    }
    if (!g_fake.sinkBuilt) {
        return 0;
    }
    g_fake.applied.push_back(ratio);
    return 1;
}

// ??? `closedLoop` IS A PARAMETER NOW, and O-1 is the reason. Every case here built the module with
// the loop off, so the one gate that decides whether the loop may run at all -- the rate -- had no
// case that could fail, and a mutant deleting it survived. Defaulted off so every existing case
// measures exactly what it measured before.
replay::IAudioRate* ArmFakeSink(bool sinkBuiltAtBind, bool closedLoop = false) {
    using namespace replay::audio;
    ResetAudioRateForTest();
    g_fake = FakeSink{};
    g_fake.sinkBuilt = sinkBuiltAtBind;
    g_deviceCallback = nullptr;
    SetSinkAccessForTest(&FakeProbe, &FakeApply);
    AkExports ak{};
    ak.GetAudioSettings = &FakeSettings;
    ak.GetBufferTick = &FakeTick;
    ak.Suspend = &FakeSuspend;
    ak.WakeupFromSuspend = &FakeWakeup;
    ak.RegisterAudioDeviceStatusCallback = &FakeRegisterDeviceStatus;
    AudioRateConfig cfg;
    cfg.closedLoop = closedLoop;
    InitAudioRate(ak, cfg);
    return AudioRate();
}

void ReleaseFakeSink() {
    replay::audio::SetSinkAccessForTest(nullptr, nullptr);
    replay::audio::ResetAudioRateForTest();
}

}  // namespace

TEST_CASE("a pause does not latch the sink unavailable, and 0.5x lands when it comes back") {
    replay::IAudioRate* rate = ArmFakeSink(true);
    REQUIRE(rate != nullptr);
    REQUIRE(rate->Available());

    rate->Suspend(true);
    CHECK(g_fake.suspends == 1);
    // The suspension is deliberate, so the missing sink is the correct state of the world and the
    // re-arm must leave it alone. A re-arm that fought it would wake the engine the user paused.
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    CHECK(g_fake.wakeups == 0);

    // Wwise raises a device-status event when it tears the sink down, and the re-probe behind that
    // event finds nothing -- which is how the module comes to be BOTH suspended and unavailable at
    // the moment the transport asks to play again. That is the state the launch was in, and it is
    // the state in which a wakeup gated on `available_` can never run.
    REQUIRE(g_deviceCallback != nullptr);
    g_deviceCallback(nullptr, 0, 0, 0, 0);
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    CHECK_FALSE(rate->Available());
    CHECK(g_fake.wakeups == 0);            // a device event must not undo a deliberate pause

    rate->SetRate(0.5f);
    CHECK(g_fake.wakeups == 1);            // gated on `available_`, this used to be unreachable
    CHECK_FALSE(rate->Available());        // the sink is still being rebuilt: a transient
    CHECK(g_fake.applied.empty());

    g_fake.sinkBuilt = true;               // Wwise finishes rebuilding it
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    CHECK(rate->Available());
    REQUIRE(g_fake.applied.size() == 1);
    // 0.5, not 1.0. The plan built while the sink was missing is RateMode::Unavailable, whose ratio
    // is 1.0; replaying it would hold the sink at full pitch under a half-speed picture and read as
    // a working mechanism to anything that only checked that a ratio was commanded.
    CHECK(g_fake.applied[0] == 0.5f);
    CHECK(std::string(rate->RateLabel()) == "audio: slowed with picture");
    ReleaseFakeSink();
}

TEST_CASE("a bind before the sink is built arms itself, with no device-status event") {
    replay::IAudioRate* rate = ArmFakeSink(false);
    REQUIRE(rate != nullptr);
    REQUIRE_FALSE(rate->Available());
    CHECK(std::string(rate->Diagnostic()).length() > 0);

    rate->SetRate(0.5f);
    CHECK(g_fake.applied.empty());
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    CHECK_FALSE(rate->Available());        // still not built: nothing invents a sink

    // OWNER-WATCH-FIX: the retry is throttled to 4 Hz of WALL time. The next tick is 17 ms later,
    // inside the window, so nothing is re-probed -- and that is the point: unthrottled, this path
    // ran a full ProbeSinks with page proofs on the game thread every frame while degraded.
    g_fake.sinkBuilt = true;
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    CHECK_FALSE(rate->Available());
    for (int i = 0; i < 20 && !rate->Available(); ++i) {
        replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    }
    CHECK(rate->Available());
    REQUIRE(g_fake.applied.size() == 1);
    CHECK(g_fake.applied[0] == 0.5f);
    ReleaseFakeSink();
}

TEST_CASE("the audio clock is readable while the ratio is not commandable") {
    replay::IAudioRate* rate = ArmFakeSink(true);
    g_fake.tick = 9375;                    // 100 s at 48000/512
    const double whileAvailable = rate->AudioSeconds();
    CHECK(whileAvailable == doctest::Approx(100.0));

    rate->Suspend(true);                   // sink gone, Wwise still counting frames
    REQUIRE_FALSE(g_fake.sinkBuilt);
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    // Zero here is what turned "the ratio was refused" into "the audio clock ran backwards", which
    // is why W2-R's slope arm measured nothing to report even in the launch where it did enter.
    CHECK(rate->AudioSeconds() == doctest::Approx(100.0));
    ReleaseFakeSink();
}

// ==============================================================================================
// ??? THE RATE COMPOSES WITH THE ENGINE'S OWN PITCH -- and the proof that used to forbid that
//
// Five launches printed a slope that was never scored, and L3-RUN-4's second launch showed the
// shape: `transport rate 0.50x: audio: slowed with picture`, then `audio rate control unavailable`
// on every rate after it. The sink proof required GetFrequencyRatio to read exactly 1.0f, and the
// command overwrote that field with the rate -- so the FIRST successful command made the voice
// unrecognisable to its own proof, permanently. These cases drive both halves against a voice
// whose ratio the test owns.

namespace {

struct FakeVoiceState {
    replay::audio::XAudio2SourceVoice voice{};   // must be first: `self` is this address
    float ratio = 1.0f;
    float maxRatio = 1024.0f;   // XAudio2 clamps the ask to the sink's MaxFrequencyRatio
    // Added to the field after every read, so a test can make something OTHER than this module
    // the last writer -- which is the state the ratio proof used to refuse outright.
    float driftPerRead = 0.0f;
    // ??? WHAT XAUDIO2 ACTUALLY DOES, and the whole of the ratchet. SetFrequencyRatio is applied on
    // the device's own processing pass, so a GetFrequencyRatio issued in the same breath returns
    // the ratio IN FORCE -- the previous one. The owner's log prints four rows of it
    // (`wrote 0.0500, voice reads 0.1000`). One stale read per Set: a read later in the session
    // sees the new value, which is why the defect needed a cycle to show.
    bool staleReadBack = false;
    float prevRatio = 1.0f;
    bool pendingStaleRead = false;
    int32_t refuseWith = 0;
    int setCalls = 0;
    uint32_t buffersQueued = 0;
    uint64_t samplesPlayed = 0;
    int getStateCalls = 0;
};

int32_t __stdcall FakeVoiceSetRatio(void* self, float ratio, uint32_t) {
    auto* f = reinterpret_cast<FakeVoiceState*>(self);
    ++f->setCalls;
    if (f->refuseWith < 0) {
        return f->refuseWith;
    }
    f->prevRatio = f->ratio;
    f->ratio = ratio > f->maxRatio ? f->maxRatio : ratio;
    f->pendingStaleRead = f->staleReadBack;
    return 0;
}

void __stdcall FakeVoiceGetRatio(void* self, float* out) {
    auto* f = reinterpret_cast<FakeVoiceState*>(self);
    if (f->pendingStaleRead) {
        f->pendingStaleRead = false;
        *out = f->prevRatio;
        return;
    }
    *out = f->ratio;
    f->ratio += f->driftPerRead;
}

void __stdcall FakeVoiceGetState(void* self, replay::audio::XAudio2VoiceState* out, uint32_t) {
    auto* f = reinterpret_cast<FakeVoiceState*>(self);
    ++f->getStateCalls;
    out->buffersQueued = f->buffersQueued;
    out->samplesPlayed = f->samplesPlayed;
}

replay::audio::XAudio2SourceVoiceVtbl& FakeVoiceVtbl() {
    static replay::audio::XAudio2SourceVoiceVtbl vtbl = [] {
        replay::audio::XAudio2SourceVoiceVtbl v{};
        v.SetFrequencyRatio = &FakeVoiceSetRatio;
        v.GetFrequencyRatio = &FakeVoiceGetRatio;
        v.GetState = &FakeVoiceGetState;
        return v;
    }();
    return vtbl;
}

// A SECOND vtable, identical in behaviour and different in address: what a voice rebuilt at the
// same heap address looks like. Behaviourally identical on purpose -- a test that told the two
// apart by their effects would pass on a difference the shipped code never sees.
replay::audio::XAudio2SourceVoiceVtbl& RebuiltVoiceVtbl() {
    static replay::audio::XAudio2SourceVoiceVtbl vtbl = FakeVoiceVtbl();
    return vtbl;
}

// A chain of `count` voices, each starting at the ratio the ENGINE gave it.
struct FakeChain {
    FakeVoiceState voices[replay::audio::SinkChain::kMax];
    replay::audio::SinkChain chain;

    void Build(std::vector<float> engineRatios) {
        chain = replay::audio::SinkChain{};
        for (size_t i = 0; i < engineRatios.size(); ++i) {
            voices[i] = FakeVoiceState{};
            voices[i].voice.vtbl = &FakeVoiceVtbl();
            voices[i].ratio = engineRatios[i];
            chain.sinks[i].sink = 0x1000 + i;
            chain.sinks[i].voice = &voices[i].voice;
            // What ProveSink records, and the fixture has to record it too: a chain built below the
            // builder with this field left null would make LiveVtbl refuse every voice, and every
            // case below would then pass by never reaching the code it is about.
            chain.sinks[i].vtbl = voices[i].voice.vtbl;
            // And the generation, for the same reason and from the same authority: ProveSink
            // stamps SinkGeneration(sink) on every sink it proves, so a chain built below the
            // builder that left this at 0 would have every VoiceCall refuse it -- every case below
            // would then pass by never reaching the code it is about. PER SINK: a Term on one
            // address does not move another's, which is what T1 is about.
            chain.sinks[i].generation = replay::audio::SinkGeneration(chain.sinks[i].sink);
            chain.sinks[i].inputChannels = 2;
            chain.sinks[i].inputSampleRate = 48000;
            chain.sinks[i].frequencyRatio = engineRatios[i];
            ++chain.count;
        }
    }

    // What a fresh probe would report: the CURRENT reading, not the one the last probe saw.
    void Reprobe() {
        for (size_t i = 0; i < chain.count; ++i) {
            chain.sinks[i].frequencyRatio = voices[i].ratio;
        }
    }
};

// The voice behind FakeProbe. Queue depth 0, so the availability cases keep the bias unlatched
// and measure only what they are about; the warm-up case owns the depth that latches.
FakeChain g_availChain;
void FillProvenSink(replay::audio::ProvenSink* out) {
    if (g_availChain.chain.count == 0) {
        g_availChain.Build({1.0f});
    }
    *out = g_availChain.chain.sinks[0];
}

// The warm-up case drives the module's own tick, so its sink has to arrive through the probe seam
// rather than as a chain handed to a function.
FakeChain g_warmChain;
size_t g_warmProbes = 0;
size_t WarmupProbe(const replay::audio::AkExports&, replay::audio::SinkChain* out) {
    ++g_warmProbes;
    *out = g_warmChain.chain;
    return g_warmChain.chain.count;
}

}  // namespace

// =================================================================================================
// OWNER-WATCH-FIX Q4. "at slower playback speeds theres a weird hitching, and in the menu it
// alternates between audio control unavailable, and audio slowed with picture. And its perfectly in
// sync with the hitching." The label was a live mirror of one bool, re-read every frame.
// =================================================================================================

TEST_CASE("at a slow rate the label is constant and the sink is written once per rate change") {
    replay::IAudioRate* rate = ArmFakeSink(true);
    REQUIRE(rate->Available());

    rate->SetRate(0.25f);
    const std::string latched = rate->RateLabel();
    CHECK(latched == "audio: slowed with picture");
    const size_t writesAfterRateChange = g_fake.applied.size();
    CHECK(writesAfterRateChange == 1);

    // Ten simulated seconds at 90 fps. The sink is made to vanish and come back on alternate
    // frames, which is exactly the transient that used to flip the panel string: each miss set
    // available_ false and the next tick's unthrottled ReArm set it true again.
    size_t flips = 0;
    for (int i = 0; i < 900; ++i) {
        g_fake.sinkBuilt = (i % 2) == 0;
        replay::audio::TickAudioRate(i * 0.25 / 90.0, 1.0 / 90.0);
        if (std::string(rate->RateLabel()) != latched) ++flips;
    }
    g_fake.sinkBuilt = true;

    CHECK(flips == 0);
    CHECK(std::string(rate->RateLabel()) == latched);
    // And the loop did not turn into a rate controller: at 0.25x the audio clock runs in wall time
    // and the scene clock in scaled time, so their difference grows forever and the trim used to
    // rail, writing the sink on every one of those 900 frames.
    CHECK(g_fake.applied.size() <= writesAfterRateChange + 1);

    // The control for the two negatives above: a REAL rate change must still move both.
    rate->SetRate(0.5f);
    CHECK(g_fake.applied.size() > writesAfterRateChange);
    const size_t afterHalf = g_fake.applied.size();

    // And 1.0x does NOT go through the apply seam: it is an absolute restore of the engine's own
    // baseline, and with nothing of ours outstanding it writes nothing at all. "At 1.0x, with
    // nothing pinned, src/audio writes nothing" is a rule this case is the executable statement of.
    rate->SetRate(1.0f);
    CHECK(std::string(rate->RateLabel()) != latched);
    CHECK(g_fake.applied.size() == afterHalf);
    ReleaseFakeSink();
}

TEST_CASE("a sink that never comes back stops being probed at frame rate, and says so once") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmFakeSink(false);
    REQUIRE_FALSE(rate->Available());
    g_fake.probes = 0;

    // Sixty simulated seconds of a module that cannot arm. Unthrottled this was a sink walk with a
    // VirtualQuery page proof per node on EVERY frame; at 4 Hz flat it is still 240 walks a minute
    // on the game thread of a session that is already hitching.
    const int ticks = 60 * 90;
    for (int i = 0; i < ticks; ++i) {
        TickAudioRate(i / 90.0, 1.0 / 90.0);
    }
    // The ceiling is 4 s, so a minute cannot hold more than the handful before the backoff plus
    // fifteen after it. Derived as a bound, not a count: the point is that it is not per frame.
    CHECK(g_fake.probes > 0);
    CHECK(g_fake.probes < 40);
    CHECK(g_fake.probes < ticks / 100);

    // And the degraded state is not silent any more: 2 m 26 s of it with no line at all is what
    // made the owner's choppy burst un-attributable.
    char notice[256] = {0};
    CHECK(DrainAudioRateNotice(notice, sizeof notice, nullptr));
    CHECK(std::string(notice).find("degraded") != std::string::npos);

    // The control: the sink coming back is still picked up, and reported. The backoff has reached
    // its ceiling, so this must run long enough to cross one of those periods.
    g_fake.sinkBuilt = true;
    for (int i = 0; i < 90 * 5; ++i) {
        TickAudioRate((ticks + i) / 90.0, 1.0 / 90.0);
    }
    CHECK(rate->Available());
    CHECK(DrainAudioRateNotice(notice, sizeof notice, nullptr));
    CHECK(std::string(notice).find("recovered") != std::string::npos);
    ReleaseFakeSink();
}

TEST_CASE("a device change mid-playback does not un-latch a measured A/V bias") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmFakeSink(true);
    REQUIRE(rate->Available());
    g_warmChain.Build({1.0f});
    g_warmChain.voices[0].buffersQueued = 8;   // a depth that latches
    SetSinkAccessForTest(&WarmupProbe, &FakeApply);

    for (int i = 0; i < 200; ++i) {
        TickAudioRate(i / 90.0, 1.0 / 90.0);
    }
    const int64_t latched = rate->DisplayBiasSceneUs();
    REQUIRE(latched > 0);   // the premise: without a latched bias there is nothing to drop

    // ??? THE HITCH. DisplayBiasSceneUs is subtracted from the display playhead every frame, so
    // dropping it to zero jumps the presented instant by the whole bias and jumps back when it
    // re-latches -- ten frames of motion at 0.25x, once a second.
    REQUIRE(g_deviceCallback != nullptr);
    g_deviceCallback(nullptr, 0, 0, 0, 0);
    TickAudioRate(200 / 90.0, 1.0 / 90.0);
    CHECK(rate->DisplayBiasSceneUs() == latched);

    for (int i = 0; i < 200; ++i) {
        TickAudioRate((201 + i) / 90.0, 1.0 / 90.0);
    }
    CHECK(rate->DisplayBiasSceneUs() == latched);
    ReleaseFakeSink();
}

TEST_CASE("a bias warm-up that never latches stops polling the voice on every tick") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmFakeSink(true);
    REQUIRE(rate->Available());
    g_warmChain.Build({1.0f});
    // The depth that can never latch: AvBias::Observe throws the count away when the whole warm-up
    // read zero, so this sink warms up forever. That is the state L3 #8 was in for 4.4 seconds.
    g_warmChain.voices[0].buffersQueued = 0;
    g_warmProbes = 0;
    SetSinkAccessForTest(&WarmupProbe, &FakeApply);

    const int ticks = 2000;
    for (int i = 0; i < ticks; ++i) {
        TickAudioRate(i / 90.0, 1.0 / 90.0);
    }
    CHECK(rate->DisplayBiasSceneUs() == 0);     // it never latched, which is this case's premise

    // The bound, derived the way the code derives it rather than copied as a number: the budget,
    // plus one attempt per warm-up length for the rest of the run.
    // Derived from the same two published constants the module reads, in the tick count 90 fps
    // turns them into. A budget expressed in ticks would have shrunk to 1.4 s at this frame rate.
    const size_t budget = static_cast<size_t>(AvBias::kBiasWarmupBudgetMs * 90 / 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    const size_t backoff =
        static_cast<size_t>(ticks * 1000.0 / 90.0 / AvBias::kBiasWarmupRetryMs) + 2;
    // The drift reconciler probes the same seam on its own cadence, and this case counts probes
    // rather than warm-ups. Derived from its published period for the same reason as the line
    // above: a hard number here would fail the day either cadence moved.
    const size_t driftProbes =
        static_cast<size_t>(ticks * 1000.0 / 90.0 / kRatioDriftCheckMs) + 2;
    CHECK(g_warmProbes >= budget);              // the budget is spent, not skipped
    CHECK(g_warmProbes <= budget + backoff + driftProbes);
    CHECK(static_cast<int>(g_warmProbes) < ticks / 4);
    ReleaseFakeSink();
}

// ---- the L3 #8 crash: a voice torn down between the proof and the use -------------------------

TEST_CASE("a voice torn down after the proof is skipped, not called, and comes back on its return") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    VoiceRatioLedger ledger;

    // Wwise zeroes the vtable field on teardown; the chain still holds the pointer we proved. This
    // is the exact state the dump caught: `mov rax,[rcx]` reads 0 and the next instruction calls
    // through 0xC8 off it.
    fc.voices[0].voice.vtbl = nullptr;
    size_t gone = 99;
    int32_t err = 1;
    CHECK(ApplyRatioToChain(fc.chain, 0.5f, ledger, &err, &gone) == 0);
    CHECK(gone == 1);
    CHECK(err == 0);                        // nothing refused a command, because none went out
    CHECK(fc.voices[0].setCalls == 0);
    CHECK(fc.voices[0].ratio == 1.0f);
    CHECK(ledger.Count() == 0);             // and no half-written record of a command never sent

    uint64_t samples = 1;
    uint32_t queued = 1;
    gone = 0;
    CHECK(ReadChainSamplesPlayed(fc.chain, &samples, &queued, &gone) == false);
    CHECK(gone == 1);
    CHECK(fc.voices[0].getStateCalls == 0);
    CHECK(samples == 1);                    // untouched: a sum over no live voice is not a reading
    CHECK(queued == 1);

    CHECK(RestoreChainRatios(fc.chain, ledger, nullptr, &gone) == 0);
    CHECK(gone == 0);                       // never commanded, so there is nothing of ours to undo

    // And the voice coming back is re-proved, not permanently written off.
    fc.voices[0].voice.vtbl = &FakeVoiceVtbl();
    fc.voices[0].buffersQueued = 3;
    CHECK(ApplyRatioToChain(fc.chain, 0.5f, ledger, &err, &gone) == 1);
    CHECK(gone == 0);
    CHECK(fc.voices[0].ratio == 0.5f);
    CHECK(ReadChainSamplesPlayed(fc.chain, &samples, &queued, &gone) == true);
    CHECK(queued == 3);
}

TEST_CASE("a voice rebuilt at the same address is refused, not commanded through") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr, nullptr) == 1);
    REQUIRE(fc.voices[0].setCalls == 1);

    // Same address, non-null vtable, DIFFERENT object. A null check alone waves this through, and
    // what it waves through is a call into whatever Wwise built there with our ledger's baseline.
    fc.voices[0].voice.vtbl = &RebuiltVoiceVtbl();
    REQUIRE(fc.voices[0].voice.vtbl != nullptr);
    size_t gone = 0;
    fc.Reprobe();
    CHECK(ApplyRatioToChain(fc.chain, 1.0f, ledger, nullptr, &gone) == 0);
    CHECK(gone == 1);
    CHECK(fc.voices[0].setCalls == 1);      // no second command reached the rebuilt object
    CHECK(ReadChainSamplesPlayed(fc.chain, nullptr, nullptr, &gone) == false);
    CHECK(fc.voices[0].getStateCalls == 0);

    // The restore leaves it alone too -- putting "our" baseline into somebody else's voice is the
    // same defect wearing a tidier name.
    CHECK(RestoreChainRatios(fc.chain, ledger, nullptr, &gone) == 0);
    CHECK(gone == 1);
    CHECK(fc.voices[0].setCalls == 1);
}

// ---- AUDIO-FIX-1: the stale read-back ratchet -------------------------------------------------
//
// The owner's session: 0.1000 -> 0.0500 -> 0.0250 -> 0.00625, a baseline halving on every apply
// until `baseline * 0.10` fell under the device floor and the module reported "SetFrequencyRatio
// was refused by every sink" about a call it never made. Every case below is RED against the code
// that shipped, because every one of them turns on the read-back being a pass behind.

TEST_CASE("a slow-mo cycle returns to the engine's own ratio when the read-back is a pass behind") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].staleReadBack = true;
    VoiceRatioLedger ledger;

    for (int cycle = 0; cycle < 5; ++cycle) {
        REQUIRE(ApplyRatioToChain(fc.chain, 0.1f, ledger, nullptr) == 1);
        CHECK(fc.voices[0].ratio == doctest::Approx(0.1f));
        const VoiceRatio* slowed = ledger.Find(&fc.voices[0].voice);
        REQUIRE(slowed != nullptr);
        // The number the old code moved. It is the engine's, it was read once, and nothing this
        // module does may ever rewrite it.
        CHECK(slowed->baseline == doctest::Approx(1.0f));

        // Both doors back to 1.0x, alternately: the absolute restore CommandPlanned takes, and a
        // plain apply at rate 1.0, which is the door the closed loop and every re-arm used. The
        // second one is the owner's sentence -- it wrote `baseline * 1.0` with `baseline` already
        // re-derived from our own 0.1, so 1.00x on the transport left the voice at 0.1.
        if ((cycle % 2) == 0) {
            REQUIRE(RestoreChainRatios(fc.chain, ledger, nullptr, nullptr, /*forget=*/false) == 1);
        } else {
            REQUIRE(ApplyRatioToChain(fc.chain, 1.0f, ledger, nullptr) == 1);
        }
        CHECK(fc.voices[0].ratio == doctest::Approx(1.0f));
    }
}

TEST_CASE("rapid successive rate changes do not compound through the stale read-back") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].staleReadBack = true;
    VoiceRatioLedger ledger;

    // Four commands with no processing pass between them: every `live` read is one command behind,
    // which is the state a user dragging the speed control produces.
    for (float rate : {0.5f, 0.25f, 0.1f, 0.5f}) {
        REQUIRE(ApplyRatioToChain(fc.chain, rate, ledger, nullptr) == 1);
        CHECK(fc.voices[0].ratio == doctest::Approx(rate));
        CHECK(ledger.Find(&fc.voices[0].voice)->baseline == doctest::Approx(1.0f));
    }
    REQUIRE(ApplyRatioToChain(fc.chain, 1.0f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(1.0f));
}

TEST_CASE("a product under the device floor is clamped and reported, not refused") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].staleReadBack = true;
    VoiceRatioLedger ledger;

    int32_t err = 0;
    size_t clamped = 99;
    // Applied, not skipped. Skipping is what latched `available_ = false` and printed a refusal for
    // a call that was never made. 1.0 x 0.0005 is under XAudio2's own 1/1024 floor.
    CHECK(ApplyRatioToChain(fc.chain, 0.0005f, ledger, &err, nullptr, &clamped) == 1);
    CHECK(clamped == 1);
    CHECK(err == kRatioBelowDeviceMinimum);
    CHECK(fc.voices[0].ratio == doctest::Approx(kXAudio2MinFreqRatio));

    // And the clamp is not a state: the same voice at 1.0x goes back to the engine's own ratio.
    REQUIRE(RestoreChainRatios(fc.chain, ledger, nullptr, nullptr, /*forget=*/false) == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(1.0f));
}

TEST_CASE("a voice that appears mid slow-mo takes the engine's baseline, not the chain's rate") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].staleReadBack = true;
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.1f, ledger, nullptr) == 1);

    // Wwise builds a second sink voice while the replay is still at 0.10x. Its ratio is 1.0 --
    // the engine's -- and 0.10x composes with THAT, not with the first voice's commanded value.
    FakeVoiceState& fresh = fc.voices[1];
    fresh = FakeVoiceState{};
    fresh.voice.vtbl = &FakeVoiceVtbl();
    fresh.staleReadBack = true;
    fc.chain.sinks[1].sink = 0x2000;
    fc.chain.sinks[1].voice = &fresh.voice;
    fc.chain.sinks[1].vtbl = fresh.voice.vtbl;
    fc.chain.sinks[1].generation = fc.chain.sinks[0].generation;
    fc.chain.count = 2;

    REQUIRE(ApplyRatioToChain(fc.chain, 0.1f, ledger, nullptr) == 2);
    CHECK(fresh.ratio == doctest::Approx(0.1f));
    CHECK(fc.voices[0].ratio == doctest::Approx(0.1f));   // not 0.01: it did not compound
    CHECK(ledger.Find(&fresh.voice)->baseline == doctest::Approx(1.0f));
    CHECK(ledger.Find(&fc.voices[0].voice)->baseline == doctest::Approx(1.0f));

    REQUIRE(RestoreChainRatios(fc.chain, ledger, nullptr, nullptr, /*forget=*/false) == 2);
    CHECK(fc.voices[0].ratio == doctest::Approx(1.0f));
    CHECK(fresh.ratio == doctest::Approx(1.0f));
}

// ??? THE INVARIANT REPLACES THIS CASE'S OLD CONTRACT, DELIBERATELY. It used to assert that a value
// a second writer left in the field became the new baseline. That was the design, and the design
// was wrong: a CAkSinkXAudio2 source voice is created with MaxFrequencyRatio = 1.0f and nothing in
// the image ever calls SetFrequencyRatio on it, so the only writer that has ever moved one of these
// fields is US, and "compose with whatever is in there" composed with our own residue. 0.1000 ->
// 0.0500 -> 0.0125, 2026-09-20.
TEST_CASE("a reading that is nobody's command is a finding, never a new baseline") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].staleReadBack = true;
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);

    // Somebody else moves the field to a value that is neither of our commands.
    fc.voices[0].ratio = 0.8f;
    fc.voices[0].pendingStaleRead = false;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);
    CHECK(ledger.Find(&fc.voices[0].voice)->baseline == doctest::Approx(1.0f));
    CHECK(fc.voices[0].ratio == doctest::Approx(0.5f));   // 1.0 x 0.5, not 0.8 x 0.5

    // And it is still visible as what it was: the drift check needs the same wrong value twice,
    // because one sample of it is the one-pass-late read.
    fc.voices[0].ratio = 0.8f;
    CHECK(CheckChainDrift(fc.chain, ledger, 0.5f, nullptr) == 0);
    fc.voices[0].ratio = 0.8f;
    CHECK(CheckChainDrift(fc.chain, ledger, 0.5f, nullptr) == 1);
}

// ---- PITCH-FIX-2 T1..T7: the owner's 2026-09-20 session, line by line -------------------------
//
// Each case is keyed to a named line of that log and each is RED against HEAD. The common defect
// they surround: the baseline was LEARNED from a read-back, and the ledger row that would have
// prevented the re-learn was retired by a generation counter that any of the eight sinks could move.

TEST_CASE("T1 baseline_survives_a_foreign_sinks_teardown") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.1f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(0.1f));

    // A DIFFERENT sink is torn down -- one of the other seven, at an address this chain never held.
    // It bumps the process counter, and before S1 that retired this live voice's ledger row.
    TermDetourBody(0x9999, nullptr, nullptr, nullptr);
    fc.Reprobe();
    fc.chain.sinks[0].generation = SinkGeneration(fc.chain.sinks[0].sink);

    REQUIRE(ApplyRatioToChain(fc.chain, 0.1f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(0.1f));   // 22:26:31 wrote 0.0125 here
    REQUIRE(ledger.Find(&fc.voices[0].voice) != nullptr);
    CHECK(ledger.Find(&fc.voices[0].voice)->baseline == doctest::Approx(1.0f));

    // And this sink's OWN teardown still retires it -- the guard is narrowed, not removed.
    TermDetourBody(fc.chain.sinks[0].sink, nullptr, nullptr, nullptr);
    fc.chain.sinks[0].generation = SinkGeneration(fc.chain.sinks[0].sink);
    ledger.RetainOnly(fc.chain);
    CHECK(ledger.Find(&fc.voices[0].voice) == nullptr);
}

TEST_CASE("T2 first_sight_while_slowed_takes_1_0_not_the_reading") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({0.2942f});   // what the field holds: OUR value, from another sink, minutes earlier
    VoiceRatioLedger ledger;

    REQUIRE(ApplyRatioToChain(fc.chain, 0.95f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(0.95f));   // 22:31:48 wrote 0.2795 here
    CHECK(ledger.Find(&fc.voices[0].voice)->baseline == doctest::Approx(1.0f));
}

TEST_CASE("T3 read_back_is_one_pass_behind") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].staleReadBack = true;
    VoiceRatioLedger ledger;

    for (float rate : {1.0f, 0.25f, 0.5f, 1.0f}) {
        REQUIRE(ApplyRatioToChain(fc.chain, rate, ledger, nullptr) == 1);
        CHECK(ledger.Find(&fc.voices[0].voice)->baseline == doctest::Approx(1.0f));
        CHECK(fc.voices[0].ratio == doctest::Approx(rate));
        // Every one of these was a phantom `expected 0.2500 found 1.0000` and a CommandPlanned().
        CHECK(CheckChainDrift(fc.chain, ledger, rate, nullptr) == 0);
    }
}

TEST_CASE("T4 max_frequency_ratio_of_one_is_not_drift") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].maxRatio = 1.0f;   // what every CAkSinkXAudio2 voice in this game is created with
    VoiceRatioLedger ledger;

    REQUIRE(ApplyRatioToChain(fc.chain, 1.05f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio <= 1.0f);              // we never ask for what it cannot hold
    CHECK(fc.voices[0].ratio == doctest::Approx(1.0f));
    CHECK(CheckChainDrift(fc.chain, ledger, 1.05f, nullptr) == 0);
    CHECK(OutstandingVoices(ledger, 1.05f) == 0);   // or the intent never counts as delivered

    const SinkRatioCensus census = SinkRatioCensusNow(ledger, 1.05f);
    CHECK(census.clampedAtMax >= 1);
    CHECK(census.relearned == 0);
}

TEST_CASE("T5 pointer_churn_does_not_multiply_baselines") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
    VoiceRatioLedger ledger;
    DrainSinkAdoptNotices(nullptr, 0);

    const float rates[] = {1.0f, 0.1f, 0.25f, 2.0f};
    for (int pass = 0; pass < 100; ++pass) {
        // The sink OBJECTS move every probe -- 56 distinct addresses in twelve minutes -- while
        // the voices behind them do not.
        for (size_t i = 0; i < fc.chain.count; ++i) {
            fc.chain.sinks[i].sink = 0x40000 + static_cast<uintptr_t>(pass) * 0x100 + i;
            fc.chain.sinks[i].generation = SinkGeneration(fc.chain.sinks[i].sink);
        }
        fc.Reprobe();
        ApplyRatioToChain(fc.chain, rates[pass % 4], ledger, nullptr);
    }

    const SinkRatioCensus census = SinkRatioCensusNow(ledger, 1.0f);
    CHECK(census.voices == 8);
    CHECK(census.distinctBaselines == 1);
    CHECK(census.baselines[0] == doctest::Approx(1.0f));
    CHECK(census.relearned == 0);
    SinkAdoptNotice notices[kMaxAdoptNotices];
    CHECK(DrainSinkAdoptNotices(notices, kMaxAdoptNotices) == 8);   // one Adopt per voice, ever
}

TEST_CASE("T6 ff_and_1x_restore_absolutely") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.1f, ledger, nullptr) == 1);

    // 22:26:34, exactly: the voice is sitting at a value of ours, and the plan says 1.0x because
    // 2.00x is above what the sink can pitch. `baseline * 1.0` wrote 0.0373 under that label.
    VoiceRatio* row = ledger.FindMutable(&fc.voices[0].voice);
    REQUIRE(row != nullptr);
    fc.voices[0].ratio = 0.0373f;
    row->observed = 0.0373f;
    fc.Reprobe();

    REQUIRE(ApplyRatioToChain(fc.chain, 1.0f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == 1.0f);   // bit-for-bit, not an approximation of a product
    CHECK(OutstandingVoices(ledger, 1.0f) == 0);

    // And the restore path writes the same absolute number.
    fc.voices[0].ratio = 0.0373f;
    row->observed = 0.0373f;
    REQUIRE(RestoreChainRatios(fc.chain, ledger, nullptr, nullptr, /*forget=*/false) == 1);
    CHECK(fc.voices[0].ratio == 1.0f);

    // Confirmed at 1.0 by the field itself: from here nothing is written at all.
    const int calls = fc.voices[0].setCalls;
    REQUIRE(ApplyRatioToChain(fc.chain, 1.0f, ledger, nullptr) == 1);
    REQUIRE(RestoreChainRatios(fc.chain, ledger, nullptr, nullptr, /*forget=*/false) == 1);
    CHECK(fc.voices[0].setCalls == calls);
}

TEST_CASE("T7 one_baseline_per_session (the census test)") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f, 1.0f, 1.0f, 1.0f});
    VoiceRatioLedger ledger;
    for (size_t i = 0; i < fc.chain.count; ++i) {
        fc.voices[i].staleReadBack = true;
        fc.voices[i].maxRatio = 1.0f;
    }

    // The owner's 48 transport-rate lines, in the order and the proportion he drove them, with a
    // sink torn down between every third one.
    const float script[] = {1.0f,  0.25f, 0.5f,  1.0f,  0.1f,  0.5f,  0.25f, 1.0f,
                            1.05f, 1.0f,  0.1f,  1.0f,  0.95f, 1.0f,  0.1f,  0.25f};
    const size_t foreignBefore = SinkRatioCensusNow(ledger, 1.0f).foreignWriters;
    for (int round = 0; round < 3; ++round) {
        for (size_t s = 0; s < sizeof script / sizeof script[0]; ++s) {
            if (s % 3 == 0) {
                // A sink dies. Its own row goes; the other three must not notice.
                const size_t victim = (s / 3) % fc.chain.count;
                TermDetourBody(fc.chain.sinks[victim].sink, nullptr, nullptr, nullptr);
                // Term destroys the voice, and what Wwise builds next is created at 1.0 -- that is
                // the fact the whole fix rests on, so the fixture has to obey it or the test would
                // be measuring a voice the engine never produces.
                fc.voices[victim].ratio = 1.0f;
                fc.voices[victim].prevRatio = 1.0f;
                fc.voices[victim].pendingStaleRead = false;
                for (size_t i = 0; i < fc.chain.count; ++i) {
                    fc.chain.sinks[i].generation = SinkGeneration(fc.chain.sinks[i].sink);
                }
            }
            fc.Reprobe();
            ApplyRatioToChain(fc.chain, script[s], ledger, nullptr);
            // Nothing in this fixture writes the field but us, so a drift finding here is a real
            // one and the case has to say what it saw: a bare "foreign-writer 1" at the end names
            // neither the voice nor the number.
            SinkRatioDrift worst{};
            if (CheckChainDrift(fc.chain, ledger, script[s], &worst) != 0) {
                MESSAGE("drift at round " << round << " step " << s << " rate " << script[s]
                                          << ": sink " << worst.sink << " expected " << worst.expected
                                          << " found " << worst.live);
            }
        }
    }

    const SinkRatioCensus census = SinkRatioCensusNow(ledger, 1.0f);
    CHECK(census.distinctBaselines == 1);
    CHECK(census.baselines[0] == doctest::Approx(1.0f));
    CHECK(census.relearned == 0);
    // Against the count this case started with, not against zero: the foreign-writer tally is a
    // process-wide cumulative counter and the cases above it legitimately raise it.
    CHECK(census.foreignWriters == foreignBefore);

    // And the line itself, in the shape DIAG-PITCH-2 S6.5 asked for.
    char line[256];
    REQUIRE(FormatSinkRatioCensus(line, sizeof line, census) > 0);
    CHECK(std::string(line).find("baselines {1.0000 x4}") != std::string::npos);
    CHECK(std::string(line).find("relearned 0") != std::string::npos);
}

TEST_CASE("audio.writes_enabled off makes the sink path strictly read-only") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    VoiceRatioLedger ledger;

    SetAudioWritesEnabled(false);
    CHECK(ApplyRatioToChain(fc.chain, 0.25f, ledger, nullptr) == 0);
    CHECK(fc.voices[0].setCalls == 0);
    CHECK(fc.voices[0].ratio == 1.0f);
    CHECK(ledger.Count() == 0);
    CHECK(RestoreChainRatios(fc.chain, ledger, nullptr) == 0);
    CHECK(fc.voices[0].setCalls == 0);

    // And the reads that the census depends on are untouched by the switch: a read-only build has
    // to still be able to measure, or the A/B has no instrument on its OFF arm.
    CHECK(CheckChainDrift(fc.chain, ledger, 1.0f, nullptr) == 0);

    SetAudioWritesEnabled(true);
    CHECK(ApplyRatioToChain(fc.chain, 0.25f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(0.25f));

    // The export view is the other half, and it is the half no future write can escape: the
    // writers are null, every reader is kept.
    AkExports all;
    all.SetState = reinterpret_cast<int32_t (*)(AkUniqueID, AkUniqueID)>(&FakeVoiceGetRatio);
    all.SetSwitch = reinterpret_cast<int32_t (*)(AkUniqueID, AkUniqueID, AkGameObjectID)>(&FakeVoiceGetRatio);
    all.StopAll = reinterpret_cast<void (*)(AkGameObjectID)>(&FakeVoiceGetRatio);
    all.SeekOnEventMs = reinterpret_cast<int32_t (*)(AkUniqueID, AkGameObjectID, AkTimeMs, bool, AkPlayingID)>(&FakeVoiceGetRatio);
    all.Suspend = reinterpret_cast<int32_t (*)(bool)>(&FakeVoiceGetRatio);
    all.SetRandomSeed = reinterpret_cast<void (*)(uint32_t)>(&FakeVoiceGetRatio);
    all.GetState = reinterpret_cast<int32_t (*)(AkUniqueID, AkUniqueID*)>(&FakeVoiceGetRatio);
    all.GetSwitch = reinterpret_cast<int32_t (*)(AkUniqueID, AkGameObjectID, AkUniqueID*)>(&FakeVoiceGetRatio);
    all.GetAudioSettings = reinterpret_cast<int32_t (*)(AkAudioSettings*)>(&FakeVoiceGetRatio);
    const AkExports ro = ReadOnlyAkExports(all);
    CHECK(ro.SetState == nullptr);
    CHECK(ro.SetSwitch == nullptr);
    CHECK(ro.StopAll == nullptr);
    CHECK(ro.SeekOnEventMs == nullptr);
    CHECK(ro.Suspend == nullptr);
    CHECK(ro.SetRandomSeed == nullptr);
    CHECK(ro.GetState != nullptr);
    CHECK(ro.GetSwitch != nullptr);
    CHECK(ro.GetAudioSettings != nullptr);
}

TEST_CASE("??? AUDIO-FIX-2: the runtime state-writes switch stops the writes and keeps the reads") {
    using namespace replay::audio;
    ResetAudioStateForTest();
    g_wwiseStates.clear();
    g_wwiseSwitches.clear();
    g_setStateCalls = 0;
    g_setSwitchCalls = 0;
    SetAudioWritesEnabled(true);
    SetStateWritesEnabled(true);

    FakeSoundPlayer sp;
    sp.SetStates({{0x11u, 0xAAu}});
    sp.SetEmitter(7, {{0x77u, 0x99u}});
    REQUIRE(InitAudioState(FakeExports(), sp.Base()));
    IAudioState* state = AudioState();
    REQUIRE(state != nullptr);
    g_wwiseStates = {{0x11u, 0xAAu}};
    g_wwiseSwitches = {{0x77u, 0x99u}};

    SUBCASE("off: no Wwise write is issued and the memo keeps the engine's own value") {
        SetStateWritesEnabled(false);
        CHECK(!state->SetState(0x11u, 0xEEu));
        CHECK(!state->SetSwitch(0x77u, 0xEEu, 7u));
        CHECK(g_setStateCalls == 0);
        CHECK(g_setSwitchCalls == 0);
        // The memo is untouched, which is the point: with it corrected behind Wwise's back the
        // ENGINE's own next SetSwitch for that group returns early, so the OFF arm of the A/B has
        // to leave it exactly as the game left it.
        AudioSwitchTriple sw[4];
        REQUIRE(state->SnapshotSwitches(sw, 4, nullptr) == 1);
        CHECK(sw[0].group == 0x77u);

        // ...and the census reads that the OFF arm is measured with still answer. An arm with no
        // instrument is not an arm.
        AudioStatePair st[4];
        uint32_t total = 0;
        CHECK(state->SnapshotStates(st, 4, &total) == 1);
        CHECK(total == 1);
    }

    SUBCASE("on again: the same two calls go through") {
        SetStateWritesEnabled(false);
        CHECK(!state->SetState(0x11u, 0xEEu));
        SetStateWritesEnabled(true);
        CHECK(state->SetState(0x11u, 0xEEu));
        CHECK(g_setStateCalls == 1);
    }

    SUBCASE("the init key is the outer term, so the verb cannot talk it back on") {
        SetAudioWritesEnabled(false);
        SetStateWritesEnabled(true);
        CHECK(!StateWritesEnabled());
        CHECK(!state->SetState(0x11u, 0xEEu));
        CHECK(g_setStateCalls == 0);
    }

    SetAudioWritesEnabled(true);
    SetStateWritesEnabled(true);
    ResetAudioStateForTest();
}

TEST_CASE("drift is measured against what we commanded, and only for voices we commanded") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f, 1.0f});
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 2);

    SinkRatioDrift worst;
    CHECK(CheckChainDrift(fc.chain, ledger, 0.5f, &worst) == 0);

    // Somebody else moved one voice. That is the reading the reconciler exists to print -- once it
    // has seen the same wrong value twice, because in this title one sample of it is the lag.
    fc.voices[1].ratio = 0.2f;
    CHECK(CheckChainDrift(fc.chain, ledger, 0.5f, &worst) == 0);
    fc.voices[1].ratio = 0.2f;
    CHECK(CheckChainDrift(fc.chain, ledger, 0.5f, &worst) == 1);
    CHECK(worst.sink == fc.chain.sinks[1].sink);
    CHECK(worst.expected == doctest::Approx(0.5f));
    CHECK(worst.live == doctest::Approx(0.2f));

    // A voice with no record of ours is not judged: its ratio is the engine's business.
    VoiceRatioLedger empty;
    CHECK(CheckChainDrift(fc.chain, empty, 0.5f, &worst) == 0);
}

TEST_CASE("the ratio that went out is recorded; a teardown mid-sequence records nothing") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr, nullptr) == 1);
    const VoiceRatio* e = ledger.Find(&fc.voices[0].voice);
    REQUIRE(e != nullptr);
    CHECK(e->baseline == 1.0f);
    CHECK(e->observed == 0.5f);

    // A voice that refuses the command must not leave a record saying we PITCHED it. The row
    // itself is taken before the call now -- it holds a constant, so there is nothing in it a
    // failed call could make untrue -- and it must still read as a voice we have not moved.
    fc.Build({1.0f});
    VoiceRatioLedger fresh;
    fc.voices[0].refuseWith = -2005401450;   // XAUDIO2_E_INVALID_CALL
    int32_t err = 0;
    CHECK(ApplyRatioToChain(fc.chain, 0.5f, fresh, &err, nullptr) == 0);
    CHECK(err == -2005401450);
    const VoiceRatio* r = fresh.Find(&fc.voices[0].voice);
    REQUIRE(r != nullptr);
    CHECK(r->commanded == doctest::Approx(r->baseline));
    CHECK(OutstandingVoices(fresh, 0.5f) == 1);   // and the 0.5x is still owed to this voice
}

// ??? THE PREMISE OF THIS CASE IS GONE, AND THAT IS THE FIX. It used to assert that a voice reading
// 1.2 was a voice the engine had pitched to 1.2, and that our rate composed with it. No
// CAkSinkXAudio2 voice is ever pitched by the game -- it is created with MaxFrequencyRatio 1.0 and
// no call site moves it -- so a reading of 1.2 is not the engine's ratio, it is a reading, and
// composing with readings is the ratchet. The case now pins the opposite.
TEST_CASE("a reading other than the shipped ratio does not become the thing we multiply") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.2f});
    VoiceRatioLedger ledger;
    int32_t err = 1;

    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, &err) == 1);
    CHECK(err == 0);
    CHECK(fc.voices[0].ratio == 0.5f);   // 1.0 x 0.5, never 1.2 x 0.5

    fc.Reprobe();
    REQUIRE(ApplyRatioToChain(fc.chain, 1.0f, ledger, &err) == 1);
    CHECK(fc.voices[0].ratio == 1.0f);   // EXACTLY, not approximately
}

TEST_CASE("a commanded ratio does not make the voice fail its own proof") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    VoiceRatioLedger ledger;

    // Before any command the voice reads the shipped value and is unknown to the ledger.
    CHECK(ledger.Find(&fc.voices[0].voice) == nullptr);
    CHECK(CheckVoiceRatioDomain(1.0f) == SinkVerdict::Ok);

    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);
    REQUIRE(fc.voices[0].ratio == 0.5f);

    // ??? THE LATCH, RETROACTIVELY. The old rule was `ratio != 1.0f -> refuse`, and this is the
    // reading it would have refused -- the one WE just wrote. Asserting the inequality pins that
    // the case is exercising the state the old proof could not survive, not merely a passing one.
    CHECK(fc.voices[0].ratio != kShippedFrequencyRatio);
    const VoiceRatio* tracked = ledger.Find(&fc.voices[0].voice);
    REQUIRE(tracked != nullptr);
    CHECK(tracked->baseline == 1.0f);
    CHECK(tracked->observed == 0.5f);
    CHECK(CheckVoiceRatioDomain(fc.voices[0].ratio) == SinkVerdict::Ok);

    // A second rate lands too -- which the launches never once reached.
    fc.Reprobe();
    REQUIRE(ApplyRatioToChain(fc.chain, 0.25f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == 0.25f);
}

TEST_CASE("release puts every voice back to the engine's ratio and forgets it") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f, 1.0f});
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 2);
    REQUIRE(fc.voices[0].ratio == 0.5f);
    REQUIRE(fc.voices[1].ratio == 0.5f);
    REQUIRE(ledger.Count() == 2);

    fc.Reprobe();
    int32_t err = 1;
    CHECK(RestoreChainRatios(fc.chain, ledger, &err) == 2);
    CHECK(err == 0);
    CHECK(fc.voices[0].ratio == 1.0f);
    CHECK(fc.voices[1].ratio == 1.0f);
    CHECK(ledger.Count() == 0);
}

// ??? THE REPORT WAS ONE GLOBAL SLOT HOLDING THE FIRST VOICE A COMMAND REACHED, and the restore
// wrote 1.0 over that slot. A chain of two voices with different engine ratios therefore reported
// ONE baseline for both; and handing one voice back erased the other voice's write from the log
// while that voice was still pitched, so `sink ratio: ... voice reads 1.2000` was printed over a
// sink running at 0.4. Which voice a line is about is the whole content of the line.
TEST_CASE("every sink voice reports its own write, and a restore touches only its own") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.2f, 0.8f});
    VoiceRatioLedger ledger;
    SinkRatioWrite w[SinkChain::kMax];
    SinkRatioWriteCounts counts;
    DrainSinkRatioWrites(w, SinkChain::kMax);   // the table is process-wide: start from empty

    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 2);
    REQUIRE(DrainSinkRatioWrites(w, SinkChain::kMax, &counts) == 2);
    CHECK(counts.superseded == 0);
    CHECK(counts.dropped == 0);
    CHECK(w[0].sink == fc.chain.sinks[0].sink);
    CHECK(w[0].rate == 0.5f);
    CHECK(w[0].baseline == doctest::Approx(1.0f));
    CHECK(w[0].written == doctest::Approx(0.5f));
    CHECK(w[0].readBack == doctest::Approx(fc.voices[0].ratio));
    CHECK(w[1].sink == fc.chain.sinks[1].sink);
    CHECK(w[1].sink != w[0].sink);
    CHECK(w[1].rate == 0.5f);
    CHECK(w[1].baseline == doctest::Approx(1.0f));
    CHECK(w[1].written == doctest::Approx(0.5f));
    CHECK(w[1].readBack == doctest::Approx(fc.voices[1].ratio));

    // Hand back ONLY the first, without a drain in between: the second voice is still at 0.5 and
    // its record has to keep saying so. (No re-apply here: both voices are settled at 0.5 by their
    // own read-back, so a second apply would correctly write nothing and stage nothing.)
    fc.Reprobe();
    SinkChain one = fc.chain;
    one.count = 1;
    REQUIRE(RestoreChainRatios(one, ledger, nullptr, nullptr, /*forget=*/false) == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(1.0f));
    CHECK(fc.voices[1].ratio == doctest::Approx(0.5f));

    REQUIRE(DrainSinkRatioWrites(w, SinkChain::kMax, &counts) == 1);
    CHECK(counts.superseded == 0);
    CHECK(w[0].sink == fc.chain.sinks[0].sink);
    CHECK(w[0].rate == 1.0f);
    CHECK(w[0].baseline == doctest::Approx(1.0f));
    CHECK(w[0].readBack == doctest::Approx(1.0f));
}

// The emitter half. The records are only an instrument if something prints them, and the driver is
// where "one line per rate change per sink voice" is either true or not.
TEST_CASE("the driver prints one sink ratio line per voice, and never the same line twice") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f, 1.0f});
    VoiceRatioLedger ledger;
    SinkRatioWrite staged[SinkChain::kMax];
    DrainSinkRatioWrites(staged, SinkChain::kMax);
    // The adopt table is process-wide for the same reason the write table is, so every earlier case
    // in this binary has left rows in it. Counting lines without emptying it first counts theirs.
    SinkAdoptNotice stagedAdopts[SinkChain::kMax];
    while (DrainSinkAdoptNotices(stagedAdopts, SinkChain::kMax) != 0) {
    }

    FakeTransport t;
    CountingRate a;
    RateDriver d;
    CountingLog log;

    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 2);
    t.Play(0.5f);
    d.Drive(&t, &a, nullptr, &log);
    // The transport rate line, one line for each voice, one adopt line for each voice, and the
    // census that closes every report.
    CHECK(log.lines == 6);
    CHECK(d.Stats().ratioLinesRepeated == 0);

    // The same two writes again: a re-command at a rate that did not move, which is what a device
    // rebuild produces. Neither voice has anything new to say, and neither is adopted twice.
    fc.Reprobe();
    for (size_t i = 0; i < fc.chain.count; ++i) {
        // Undo the settle, so the apply really does command again: a voice whose field no longer
        // reads what we left in it is exactly the case a re-command exists for.
        fc.voices[i].ratio = 1.0f;
        ledger.FindMutable(&fc.voices[i].voice)->observed = 1.0f;
    }
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 2);
    t.Play(0.25f);
    d.Drive(&t, &a, nullptr, &log);
    CHECK(log.lines == 8);   // the rate line and the census
    CHECK(d.Stats().ratioLinesRepeated == 2);
}

TEST_CASE("a voice this module never commanded is left alone by the release") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({0.8f});
    VoiceRatioLedger ledger;
    CHECK(RestoreChainRatios(fc.chain, ledger, nullptr) == 0);
    CHECK(fc.voices[0].ratio == 0.8f);
    CHECK(fc.voices[0].setCalls == 0);
}

TEST_CASE("a clamped command is recorded as what the voice took, not as what was asked") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].maxRatio = 1.0f;    // CreateSourceVoice asks for MaxFrequencyRatio = 1.0f
    VoiceRatioLedger ledger;

    REQUIRE(ApplyRatioToChain(fc.chain, 1.5f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == 1.0f);          // XAudio2 clamped it
    const VoiceRatio* tracked = ledger.Find(&fc.voices[0].voice);
    REQUIRE(tracked != nullptr);
    // Recording the ask (1.5f) would refuse this voice on the next probe for a difference the
    // clamp caused -- the same latch in a second costume.
    CHECK(tracked->observed == 1.0f);
    CHECK(CheckVoiceRatioDomain(fc.voices[0].ratio) == SinkVerdict::Ok);
}

TEST_CASE("a voice that refuses the command is counted, not recorded as commanded") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].refuseWith = -1;   // E_INVALIDARG
    VoiceRatioLedger ledger;
    int32_t err = 0;
    CHECK(ApplyRatioToChain(fc.chain, 0.5f, ledger, &err) == 0);
    CHECK(err == -1);
    CHECK(fc.voices[0].ratio == 1.0f);
}

TEST_CASE("the ratio proof accepts what a voice can hold and refuses what it cannot") {
    using namespace replay::audio;

    SUBCASE("first sight: anything inside XAudio2's own range is the engine's business") {
        CHECK(CheckVoiceRatioDomain(1.0f) == SinkVerdict::Ok);
        CHECK(CheckVoiceRatioDomain(1.2f) == SinkVerdict::Ok);
        CHECK(CheckVoiceRatioDomain(kXAudio2MinFreqRatio) == SinkVerdict::Ok);
        CHECK(CheckVoiceRatioDomain(kXAudio2MaxFreqRatio) == SinkVerdict::Ok);
    }

    SUBCASE("outside it, the float came from the wrong field") {
        CHECK(CheckVoiceRatioDomain(0.0f) == SinkVerdict::RatioOutOfDomain);
        CHECK(CheckVoiceRatioDomain(-1.0f) == SinkVerdict::RatioOutOfDomain);
        CHECK(CheckVoiceRatioDomain(kXAudio2MinFreqRatio / 2.0f) ==
              SinkVerdict::RatioOutOfDomain);
        CHECK(CheckVoiceRatioDomain(2048.0f) == SinkVerdict::RatioOutOfDomain);
        CHECK(CheckVoiceRatioDomain(std::numeric_limits<float>::quiet_NaN()) ==
              SinkVerdict::RatioOutOfDomain);
        CHECK(CheckVoiceRatioDomain(std::numeric_limits<float>::infinity()) ==
              SinkVerdict::RatioOutOfDomain);
    }

    SUBCASE("a value inside the domain is admitted whoever wrote it") {
        // The ledger is not consulted at all. A reading that is neither our last observation nor
        // any baseline we hold is the engine's ratio, and refusing it is what latched the module
        // unavailable for the rest of the session.
        CHECK(CheckVoiceRatioDomain(0.6f) == SinkVerdict::Ok);
        CHECK(CheckVoiceRatioDomain(0.9f) == SinkVerdict::Ok);
        CHECK(CheckVoiceRatioDomain(1.0f) == SinkVerdict::Ok);
    }
}

// ??? WHOEVER WROTE THE FIELD, THE VALUE IN IT IS THE ENGINE'S RATIO NOW.
//
// The proof used to demand that GetFrequencyRatio read back either our last observation or a
// baseline the ledger had snapshotted. RetainOnly drops a voice on any transient refusal, so the
// next Adopt snapshots OUR OWN last command as the "engine" baseline -- and a voice the engine then
// rebuilds reads 1.0f, which matches neither. That is the whole latch: one refusal, and every
// re-arm re-reads the same voice and refuses it again.

TEST_CASE("a voice found at a ratio nobody commanded is still commanded from 1.0") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.2f});                    // a value on first sight that is nobody's command
    fc.voices[0].driftPerRead = 0.01f;   // something else is driving the field
    VoiceRatioLedger ledger;

    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);
    const VoiceRatio* e = ledger.Find(&fc.voices[0].voice);
    REQUIRE(e != nullptr);
    // 1.2 was read by nobody and adopted by nobody: the ask is 1.0 x 0.5, not 1.2 x 0.5.
    CHECK(e->baseline == kShippedFrequencyRatio);
    CHECK(e->observed == doctest::Approx(0.5f));
    CHECK(fc.voices[0].ratio == doctest::Approx(0.51f));   // our 0.5 plus the other writer's step

    // No Reprobe: the apply reads nothing to decide the target, so a chain carrying a stale probe
    // reading cannot be what it commands against -- and neither can a fresh one.
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);
    e = ledger.Find(&fc.voices[0].voice);
    REQUIRE(e != nullptr);
    CHECK(e->baseline == kShippedFrequencyRatio);
    // The ledger says this voice is at 0.5 and its own read-back said so too, so the second command
    // at the same rate issues no call at all.
    CHECK(fc.voices[0].setCalls == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(0.51f));
    // And the sink still proves: the domain is the layout claim, and 0.61 is inside it.
    CHECK(CheckVoiceRatioDomain(0.61f) == SinkVerdict::Ok);
}

// ??? THIS CASE USED TO ASSERT THE DEFECT. It required `poisoned->baseline == 0.5f` after a prune,
// i.e. it pinned as correct the state in which the ledger calls our own slowed value the engine's
// ratio -- and from there every "1.0x" writes 0.5 for the rest of the session. That is the owner's
// "it never comes out of slowmo audio mode", asserted green.
TEST_CASE("a voice pruned by a transient and returned keeps its baseline; a rebuilt one re-bases") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);
    REQUIRE(fc.voices[0].ratio == 0.5f);

    // The transient: NoVoice or SinkUnreadable while a device rebuild is in flight. The voice is
    // not in the proven chain -- but it was not torn down either, and its generation says so.
    SinkChain none;
    ledger.RetainOnly(none);
    CHECK(ledger.Count() == 1);
    REQUIRE(ledger.Find(&fc.voices[0].voice) != nullptr);
    CHECK(ledger.Find(&fc.voices[0].voice)->present == false);

    // It comes back at the same address still carrying OUR 0.5, and 1.0x puts the engine's ratio
    // back rather than re-asserting the 0.5 as if the engine had chosen it.
    fc.Reprobe();
    REQUIRE(ApplyRatioToChain(fc.chain, 1.0f, ledger, nullptr) == 1);
    const VoiceRatio* kept = ledger.Find(&fc.voices[0].voice);
    REQUIRE(kept != nullptr);
    CHECK(kept->baseline == 1.0f);
    CHECK(kept->baseline == kShippedFrequencyRatio);
    CHECK(fc.voices[0].ratio == 1.0f);

    // And a value that is NOT ours is a FINDING, not a new baseline. It is inside the domain, so
    // nothing refuses it; it takes two samples of the same wrong value to be judged, because one is
    // the one-pass-late read; and the command that follows is still 1.0 x rate.
    fc.voices[0].ratio = 1.3f;
    fc.Reprobe();
    CHECK(CheckVoiceRatioDomain(fc.voices[0].ratio) == SinkVerdict::Ok);
    CHECK(CheckChainDrift(fc.chain, ledger, 1.0f, nullptr) == 0);
    CHECK(CheckChainDrift(fc.chain, ledger, 1.0f, nullptr) == 1);

    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);
    CHECK(fc.voices[0].ratio == doctest::Approx(0.5f));
    const VoiceRatio* rebased = ledger.Find(&fc.voices[0].voice);
    REQUIRE(rebased != nullptr);
    CHECK(rebased->baseline == kShippedFrequencyRatio);
}

TEST_CASE("a read-back that differs in the last bits is still our own value") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].driftPerRead = 1.0e-6f;   // storage rounding, not a second writer
    VoiceRatioLedger ledger;

    for (int i = 0; i < 3; ++i) {
        REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);
    }
    // Bit-exact matching would call each read-back a foreign write and re-baseline onto it, so
    // three commands at 0.5x would land at 0.125x.
    CHECK(fc.voices[0].ratio == doctest::Approx(0.5f));
    const VoiceRatio* e = ledger.Find(&fc.voices[0].voice);
    REQUIRE(e != nullptr);
    CHECK(e->baseline == 1.0f);
}

TEST_CASE("the ratio tolerance is relative and refuses a real difference") {
    using namespace replay::audio;
    CHECK(NearlyEqualRatio(0.5f, 0.5f));
    CHECK(NearlyEqualRatio(0.5f, 0.5f + 1.0e-6f));
    CHECK(NearlyEqualRatio(64.0f, 64.0f + 1.0e-3f));   // scaled by magnitude
    CHECK_FALSE(NearlyEqualRatio(0.5f, 0.51f));
    CHECK_FALSE(NearlyEqualRatio(1.0f, 0.5f));
}

TEST_CASE("the ledger holds only voices the current chain still has") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f, 1.0f});
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 2);
    REQUIRE(ledger.Count() == 2);

    const void* survivor = &fc.voices[1].voice;
    SinkChain shrunk{};
    shrunk.sinks[0] = fc.chain.sinks[1];
    shrunk.count = 1;
    ledger.RetainOnly(shrunk);
    CHECK(ledger.Count() == 2);                         // absent, not erased
    CHECK(ledger.Find(survivor)->present == true);
    REQUIRE(ledger.Find(&fc.voices[0].voice) != nullptr);
    CHECK(ledger.Find(&fc.voices[0].voice)->present == false);

    // A voice rebuilt at the same address IS erased, and only its generation says so.
    SinkChain rebuilt{};
    rebuilt.sinks[0] = fc.chain.sinks[0];
    rebuilt.sinks[0].generation += 1;
    rebuilt.count = 1;
    ledger.RetainOnly(rebuilt);
    CHECK(ledger.Find(&fc.voices[0].voice) == nullptr);
    CHECK(ledger.Count() == 1);

    // And the table cannot grow past a legal chain even across many rebuilds.
    for (int round = 0; round < 4; ++round) {
        fc.Build({1.0f, 1.0f, 1.0f});
        REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 3);
        CHECK(ledger.Count() == 3);
    }
}

// ==============================================================================================
// ??? EVERY SEQUENCE THAT ENDS AT 1.0x LEAVES THE VOICE AT THE ENGINE'S OWN RATIO.
//
// The owner: "if I go to .25x then back to 1, it never comes out of like slowmo audio mode." The
// module commanded 1.0x, latched RateMode::Normal and logged "as the game mixed it" over a sink
// sitting at 0.25, because `Command(1.0f)` writes `baseline * 1.0` -- it re-asserts a belief, it
// does not restore a value. Nothing offline could see it: every existing case asserts the label,
// or the command COUNT while slowed, and none asserts the ratio after a return to 1.0.
//
// These drive the whole module through IAudioRate against voices whose ratio the test owns, and
// every one of them ends on the absolute value.
// ==============================================================================================

namespace {

FakeChain g_absChain;
int g_absDropSink = -1;      // a sink the next probe refuses for a transient, as NoVoice does

size_t AbsProbe(const replay::audio::AkExports&, replay::audio::SinkChain* out) {
    using namespace replay::audio;
    *out = SinkChain{};
    if (!g_fake.sinkBuilt) {
        out->firstRefusal = SinkVerdict::NoSinkList;
        return 0;
    }
    g_absChain.Reprobe();
    for (size_t i = 0; i < g_absChain.chain.count; ++i) {
        if (static_cast<int>(i) == g_absDropSink) {
            ++out->rejected;
            out->firstRefusal = SinkVerdict::NoVoice;
            continue;
        }
        out->sinks[out->count++] = g_absChain.chain.sinks[i];
    }
    return out->count;
}

size_t AbsApply(const replay::audio::AkExports& ak, float rate, int32_t* firstError,
                size_t* voicesGone) {
    using namespace replay::audio;
    SinkChain chain;
    if (AbsProbe(ak, &chain) == 0) {
        if (firstError != nullptr) {
            *firstError = -1;
        }
        return 0;
    }
    return ApplyRatioToChain(chain, rate, SinkRatioLedger(), firstError, voicesGone);
}

replay::IAudioRate* ArmAbs(std::vector<float> engineRatios, bool rateHook = true) {
    using namespace replay::audio;
    ResetAudioRateForTest();
    g_fake = FakeSink{};
    g_deviceCallback = nullptr;
    g_absDropSink = -1;
    g_absChain.Build(std::move(engineRatios));
    SetSinkAccessForTest(&AbsProbe, &AbsApply);
    AkExports ak{};
    ak.GetAudioSettings = &FakeSettings;
    ak.GetBufferTick = &FakeTick;
    ak.Suspend = &FakeSuspend;
    ak.WakeupFromSuspend = &FakeWakeup;
    ak.RegisterAudioDeviceStatusCallback = &FakeRegisterDeviceStatus;
    AudioRateConfig cfg;
    cfg.rateHookInstalled = rateHook;
    cfg.installedMaxRate = 4.0f;
    InitAudioRate(ak, cfg);
    return AudioRate();
}

// The sink comes back on Wwise's own thread, behind the wakeup; the 4 Hz re-arm is what finds it.
void PumpUntilAvailable(replay::IAudioRate* rate) {
    g_fake.sinkBuilt = true;
    for (int i = 0; i < 40 && !rate->Available(); ++i) {
        replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    }
}

// One sink of the chain flaps and comes back at the same address. The device event is what makes
// the module re-command over the SHORTENED chain -- which is the whole trigger: an apply that runs
// while a voice is missing is the one that used to erase its baseline.
void FlapSink(int index) {
    g_absDropSink = index;
    if (g_deviceCallback != nullptr) {
        g_deviceCallback(nullptr, 0, 0, 0, 0);
    }
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    g_absDropSink = -1;
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
}

}  // namespace

TEST_CASE("0.25x and back to 1x leaves the voice at the engine's ratio, not at 0.25") {
    replay::IAudioRate* rate = ArmAbs({1.0f});
    REQUIRE(rate->Available());

    rate->SetRate(0.25f);
    CHECK(g_absChain.voices[0].ratio == doctest::Approx(0.25f));
    rate->SetRate(1.0f);
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    CHECK(std::string(rate->RateLabel()) == "audio: 1.0x, as the game mixed it");
    ReleaseFakeSink();
}

TEST_CASE("two slow steps compose from the shipped 1.0 once each, and 1x restores it") {
    replay::IAudioRate* rate = ArmAbs({1.0f});
    REQUIRE(rate->Available());

    rate->SetRate(0.5f);
    CHECK(g_absChain.voices[0].ratio == doctest::Approx(0.5f));
    rate->SetRate(0.25f);
    // 0.25, not 0.125: the second rate multiplies the ratio the engine CREATED the voice with,
    // which is 1.0 for every CAkSinkXAudio2 voice, and never our own 0.5.
    CHECK(g_absChain.voices[0].ratio == doctest::Approx(0.25f));
    rate->SetRate(1.0f);
    CHECK(g_absChain.voices[0].ratio == 1.0f);   // bit-exact: an absolute restore, not 1.0 * 1.0
    ReleaseFakeSink();
}

TEST_CASE("a fast rate with no rate hook holds the shipped ratio at every step") {
    replay::IAudioRate* rate = ArmAbs({1.0f}, /*rateHook=*/false);
    REQUIRE(rate->Available());
    CHECK(rate->MaxRate() == 1.0f);

    rate->SetRate(2.0f);
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    rate->SetRate(1.0f);
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    ReleaseFakeSink();
}

TEST_CASE("a pause spliced into the slow-mo does not compound the rate it resumes at") {
    replay::IAudioRate* rate = ArmAbs({1.0f});
    rate->SetRate(0.25f);
    REQUIRE(g_absChain.voices[0].ratio == doctest::Approx(0.25f));

    rate->Suspend(true);
    PumpUntilAvailable(rate);
    rate->SetRate(0.25f);
    PumpUntilAvailable(rate);
    // 0.25, not 0.0625. The wakeup re-asserts the rate against a baseline it did not re-derive.
    CHECK(g_absChain.voices[0].ratio == doctest::Approx(0.25f));

    rate->Suspend(true);
    PumpUntilAvailable(rate);
    rate->SetRate(1.0f);
    PumpUntilAvailable(rate);
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    ReleaseFakeSink();
}

TEST_CASE("reverse and back to forward ends at the engine's ratio and not suspended") {
    replay::IAudioRate* rate = ArmAbs({1.0f});
    rate->SetRate(0.25f);
    rate->SetRate(-1.0f);
    PumpUntilAvailable(rate);
    rate->SetRate(1.0f);
    PumpUntilAvailable(rate);
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    CHECK(rate->Available());
    ReleaseFakeSink();
}

TEST_CASE("a transient probe refusal on one sink of two cannot poison either baseline") {
    // NoVoice / SinkUnreadable / VoiceGone all reach the ledger the same way: the sink is missing
    // from a proven chain that still has others in it, so the apply runs and the absent voice's
    // record used to be erased. This is the owner's defect, reproduced from the trigger up.
    for (int dropped = 0; dropped < 2; ++dropped) {
        replay::IAudioRate* rate = ArmAbs({1.0f, 1.0f});
        rate->SetRate(0.25f);
        REQUIRE(g_absChain.voices[0].ratio == doctest::Approx(0.25f));
        REQUIRE(g_absChain.voices[1].ratio == doctest::Approx(0.25f));

        FlapSink(dropped);                       // the flap, while slowed

        rate->SetRate(1.0f);
        CHECK(g_absChain.voices[0].ratio == 1.0f);
        CHECK(g_absChain.voices[1].ratio == 1.0f);
        ReleaseFakeSink();
    }

    // A one-sink chain too: ApplyFrequencyRatio returns early on an empty probe, so the only way
    // this one loses its entry is the probe coming back empty and then full again.
    replay::IAudioRate* rate = ArmAbs({1.0f});
    rate->SetRate(0.25f);
    FlapSink(0);
    PumpUntilAvailable(rate);
    rate->SetRate(1.0f);
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    ReleaseFakeSink();
}

TEST_CASE("a device change while slowed does not re-base the baseline onto our own rate") {
    replay::IAudioRate* rate = ArmAbs({1.0f});
    rate->SetRate(0.25f);
    REQUIRE(g_absChain.voices[0].ratio == doctest::Approx(0.25f));

    REQUIRE(g_deviceCallback != nullptr);
    g_deviceCallback(nullptr, 0, 0, 0, 0);
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    CHECK(g_absChain.voices[0].ratio == doctest::Approx(0.25f));

    rate->SetRate(1.0f);
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    ReleaseFakeSink();
}

// ==============================================================================================
// ??? AN INTENT THAT REACHED NO SINK IS NOT A DELIVERED INTENT.
//
// The owner: "audio still seems to not come back into 1x normal pitch when coming out of .1x for a
// while. Sometimes it works sometimes it doesnt." The transient is a ProbeSinks that misses across
// the 0.1x -> 1x transition. The module recorded the ask anyway, so the reconciler's guard
// (`lastCommanded_ == want`) was satisfied by a write nothing received, and the only thing left
// that could put the mix back was the 2 s drift sweep -- when it was running at all.
//
// These two drive the whole module through IAudioRate and assert the RECOVERY LATENCY, which is
// the half the drift sweep cannot fake: within one reassert period of the sinks reappearing.
// ==============================================================================================

// 250 ms of reassert period at the 60 Hz this rig ticks at, with a frame of slack. Well inside the
// 2 s drift sweep, which is what makes these cases about the reassert and not about it.
constexpr int kTicksInOneReassertPeriod = 16;

TEST_CASE("A return to 1x that reached no sink is retried until it does") {
    replay::IAudioRate* rate = ArmAbs({1.0f, 1.0f});
    REQUIRE(rate->Available());
    rate->SetRate(0.1f);
    REQUIRE(g_absChain.voices[0].ratio == doctest::Approx(0.1f));
    REQUIRE(g_absChain.voices[1].ratio == doctest::Approx(0.1f));

    // The transient: every probe misses across the transition, exactly as a sink list mid-rebuild
    // does. Nothing can be written, and the module must not believe otherwise.
    g_fake.sinkBuilt = false;
    rate->SetRate(1.0f);
    for (int i = 0; i < 30; ++i) {
        replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    }
    CHECK(g_absChain.voices[0].ratio == doctest::Approx(0.1f));
    CHECK(g_absChain.voices[1].ratio == doctest::Approx(0.1f));

    // The sinks come back. One reassert period later the mix is at the engine's own ratio, bit for
    // bit -- and not two seconds later, when the drift sweep would have found it.
    g_fake.sinkBuilt = true;
    for (int i = 0; i < kTicksInOneReassertPeriod; ++i) {
        replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    }
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    CHECK(g_absChain.voices[1].ratio == 1.0f);
    ReleaseFakeSink();
}

TEST_CASE("A restore that reached one sink of two stays outstanding for the other") {
    replay::IAudioRate* rate = ArmAbs({1.0f, 1.0f});
    rate->SetRate(0.1f);
    REQUIRE(g_absChain.voices[1].ratio == doctest::Approx(0.1f));

    // Sink 1 is missing from the proven chain for the whole transition: the restore reaches sink 0
    // and returns a nonzero count, which is what used to read as "delivered".
    g_absDropSink = 1;
    rate->SetRate(1.0f);
    CHECK(g_absChain.voices[0].ratio == 1.0f);
    CHECK(g_absChain.voices[1].ratio == doctest::Approx(0.1f));

    g_absDropSink = -1;
    for (int i = 0; i < kTicksInOneReassertPeriod; ++i) {
        replay::audio::TickAudioRate(0.0, 1.0 / 60.0);
    }
    CHECK(g_absChain.voices[1].ratio == 1.0f);
    ReleaseFakeSink();
}

// ??? The release puts back what it can reach and REMEMBERS the rest. A suspended engine
// has no voice to write, so the old unconditional Reset() threw away the only record of what a
// voice still holding `baseline * 0.1` was supposed to hold -- and nothing in the process could
// name it afterwards.
TEST_CASE("A release that restored nothing keeps the baselines it could not put back") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmAbs({1.0f});
    rate->SetRate(0.25f);
    REQUIRE(g_absChain.voices[0].ratio == doctest::Approx(0.25f));
    REQUIRE(SinkRatioLedger().Count() == 1);

    rate->Suspend(true);          // no sink to command: the restore inside Release writes nothing
    ShutdownAudioRate();
    CHECK(g_absChain.voices[0].ratio == doctest::Approx(0.25f));   // still ours, still pitched
    REQUIRE(SinkRatioLedger().Count() == 1);
    CHECK(SinkRatioLedger().At(0).baseline == kShippedFrequencyRatio);
    ReleaseFakeSink();
}

// ??? The 2 s verify walks the sink list and reads one ratio per voice ON THE FRAME
// THREAD. It is inside tickcost::kAudioRate (audio_module.cpp), it is bounded by SinkChain::kMax,
// and this says what a full-width pass costs with no game in the room.
TEST_CASE("The drift verify is bounded and its per-pass cost is reported") {
    using namespace replay::audio;
    std::vector<float> ratios(SinkChain::kMax, 1.0f);
    replay::IAudioRate* rate = ArmAbs(ratios);
    REQUIRE(rate->Available());
    rate->SetRate(0.5f);
    REQUIRE(SinkRatioLedger().Count() == SinkChain::kMax);

    // Two seconds of tick per pass is what arms the verify; 40 passes is enough to time.
    const int kPasses = 40;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPasses; ++i) {
        TickAudioRate(0.0, 2.0);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double usPerPass =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / kPasses;
    MESSAGE("F9: verify pass over " << SinkChain::kMax << " fake voices: " << usPerPass
                                    << " us per pass");
    // Not a benchmark rail: a pass that costs a frame is the defect, and a fake voice is cheaper
    // than a real COM call by a margin no threshold here can model.
    CHECK(usPerPass < 1000.0);
    CHECK(g_absChain.voices[0].ratio == doctest::Approx(0.5f));
    ReleaseFakeSink();
}

TEST_CASE("the sink write is reported with the sink, the rate, the baseline and the read-back") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmAbs({1.0f});
    SinkRatioWrite w[SinkChain::kMax];
    DrainSinkRatioWrites(w, SinkChain::kMax);   // the table is process-wide: start from empty

    rate->SetRate(0.5f);
    REQUIRE(DrainSinkRatioWrites(w, SinkChain::kMax) == 1);
    CHECK(w[0].rate == 0.5f);
    CHECK(w[0].baseline == kShippedFrequencyRatio);
    CHECK(w[0].written == doctest::Approx(0.5f));
    CHECK(w[0].readBack == doctest::Approx(0.5f));

    rate->SetRate(1.0f);
    REQUIRE(DrainSinkRatioWrites(w, SinkChain::kMax) == 1);
    CHECK(w[0].rate == 1.0f);
    CHECK(w[0].readBack == 1.0f);   // the number the log prints IS the number in the voice
    CHECK(w[0].readBack == g_absChain.voices[0].ratio);
    ReleaseFakeSink();
}

// ??? THE IDEMPOTENCE CLAUSE AS AN EXECUTABLE STATEMENT. A fix that only closed the one sequence the
// owner happened to run would leave the next packet free to reintroduce this from another
// direction; the property is that the ORDER cannot matter, so it is stated over every order.
TEST_CASE("every sequence of at most five transport actions ending at 1.0x restores the original") {
    using namespace replay::audio;
    enum Step { kSlowQuarter, kSlowHalf, kFast, kPauseResume, kReverse, kTransient, kDeviceChange,
                kStepCount };

    // Both voices are created at the shipped 1.0, because every CAkSinkXAudio2 source voice is.
    // The property is unchanged by that: whatever the walk did, 1.0x ends bit-exactly back there.
    const float kEngine0 = kShippedFrequencyRatio;
    const float kEngine1 = kShippedFrequencyRatio;
    int checked = 0;
    for (int length = 1; length <= 5; ++length) {
        long total = 1;
        for (int i = 0; i < length; ++i) {
            total *= kStepCount;
        }
        // 7^4 = 2401 sequences is already every order that matters; the two longest levels are
        // walked at a fixed stride rather than exhaustively so the case stays a second, not a
        // minute. A stride, not a sample: it is the same set on every machine and every run.
        const long stride = length >= 4 ? 11 : 1;   // coprime with 7, so no step position is fixed
        for (long seq = 0; seq < total; seq += stride) {
            replay::IAudioRate* rate = ArmAbs({kEngine0, kEngine1});
            long code = seq;
            for (int i = 0; i < length; ++i) {
                switch (static_cast<Step>(code % kStepCount)) {
                    case kSlowQuarter: rate->SetRate(0.25f); break;
                    case kSlowHalf:    rate->SetRate(0.5f);  break;
                    case kFast:        rate->SetRate(2.0f);  break;
                    case kPauseResume:
                        rate->Suspend(true);
                        PumpUntilAvailable(rate);
                        break;
                    case kReverse:
                        rate->SetRate(-1.0f);
                        PumpUntilAvailable(rate);
                        break;
                    case kTransient:
                        FlapSink(static_cast<int>(seq % 2));
                        break;
                    case kDeviceChange:
                        if (g_deviceCallback != nullptr) {
                            g_deviceCallback(nullptr, 0, 0, 0, 0);
                        }
                        TickAudioRate(0.0, 1.0 / 60.0);
                        break;
                    case kStepCount: break;
                }
                code /= kStepCount;
            }
            rate->SetRate(1.0f);
            PumpUntilAvailable(rate);
            REQUIRE(g_absChain.voices[0].ratio == kEngine0);
            REQUIRE(g_absChain.voices[1].ratio == kEngine1);
            ++checked;
            ReleaseFakeSink();
        }
    }
    CHECK(checked > 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
}

// ==============================================================================================
// W2-H-LIFETIME-3 -- ledger row 2.6, the voice lifetime signal (ARCH-22 S4.3, T1..T8)
//
// The crash these cases are about is L3 #9, and it is NOT L3 #8. #8 read the voice's vtable field
// back as 0 and faulted at `call [rax+0C8h]`, which LiveVtbl's null test already covers. #9's
// vtable is VALID -- IXAudio2SourceVoice's vtable lives in xaudio2_9.dll's .rdata and survives
// DestroyVoice -- so the identity test passes and the call goes INTO XAudio2 on a freed object.
// The fake below reproduces that and nothing weaker: the voice is "freed" without its memory being
// cleared and without its vtable pointer being touched.
// ==============================================================================================

#include <stdlib.h>

#include <atomic>
#include <new>
#include <chrono>
#include <thread>

#include "audio/voicelifetime.h"

namespace lifetime_t {

struct LifeVoice {
    replay::audio::XAudio2SourceVoice voice{};   // must be first: `self` is this address
    float ratio = 1.0f;
    float maxRatio = 1024.0f;
    bool destroyed = false;
    int callsWhileDestroyed = 0;
    uint64_t samplesPlayed = 0;
    uint32_t buffersQueued = 1;
};

// The freed voice, called. Under REPLAY_T1_TRAP it takes the access violation for real, which is
// how the RED before this packet was recorded; by default it counts the call and returns, because a
// test process that dies has stopped proving anything about the other seven cases.
void NoteUseAfterFree(LifeVoice* v) {
    ++v->callsWhileDestroyed;
    if (getenv("REPLAY_T1_TRAP") != nullptr) {
        *static_cast<volatile float*>(nullptr) = 1.0f;
    }
}

void __stdcall LifeGetRatio(void* self, float* out) {
    auto* v = reinterpret_cast<LifeVoice*>(self);
    if (v->destroyed) {
        NoteUseAfterFree(v);
        return;
    }
    *out = v->ratio;
}

int32_t __stdcall LifeSetRatio(void* self, float ratio, uint32_t) {
    auto* v = reinterpret_cast<LifeVoice*>(self);
    if (v->destroyed) {
        NoteUseAfterFree(v);
        return 0;
    }
    v->ratio = ratio > v->maxRatio ? v->maxRatio : ratio;
    return 0;
}

void __stdcall LifeGetState(void* self, replay::audio::XAudio2VoiceState* out, uint32_t) {
    auto* v = reinterpret_cast<LifeVoice*>(self);
    if (v->destroyed) {
        NoteUseAfterFree(v);
        return;
    }
    out->samplesPlayed = v->samplesPlayed;
    out->buffersQueued = v->buffersQueued;
}

replay::audio::XAudio2SourceVoiceVtbl& LifeVtblTable() {
    static replay::audio::XAudio2SourceVoiceVtbl v = [] {
        replay::audio::XAudio2SourceVoiceVtbl t{};
        t.SetFrequencyRatio = &LifeSetRatio;
        t.GetFrequencyRatio = &LifeGetRatio;
        t.GetState = &LifeGetState;
        return t;
    }();
    return v;
}

// The sink address is the voice's own address here. Only its identity matters: it is what the
// detour marks and what the reader publishes.
replay::audio::ProvenSink Prove(LifeVoice* v) {
    replay::audio::ProvenSink p;
    p.sink = reinterpret_cast<uintptr_t>(v);
    p.voice = &v->voice;
    p.vtbl = v->voice.vtbl;
    p.inputChannels = 2;
    p.inputSampleRate = 48000;
    p.frequencyRatio = v->ratio;
    p.generation = replay::audio::SinkGeneration(p.sink);
    return p;
}

LifeVoice* Init(LifeVoice* v) {
    v->voice.vtbl = &LifeVtblTable();
    return v;
}

// The "original" the detour wraps: what CAkSinkXAudio2::Term does to the voice, minus the memory
// clear the engine also does not do.
int64_t __fastcall DestroyOriginal(void* a1, void*) {
    reinterpret_cast<LifeVoice*>(a1)->destroyed = true;
    return 1;
}
int64_t __fastcall NoopOriginal(void*, void*) { return 1; }

std::atomic<bool> g_originalRan{false};
int64_t __fastcall FlaggingOriginal(void*, void*) {
    g_originalRan.store(true, std::memory_order_seq_cst);
    return 1;
}

// A reader that holds its VoiceCall open until it is told to leave. T5 and T6 need presence with a
// duration; every other case needs none.
struct HeldReader {
    std::atomic<bool> inside{false};
    std::atomic<bool> leave{false};
    std::atomic<bool> ok{false};
    std::thread th;

    void Start(const replay::audio::ProvenSink& proven) {
        th = std::thread([this, proven] {
            replay::audio::VoiceCall call(proven);
            ok.store(call.Ok(), std::memory_order_seq_cst);
            inside.store(true, std::memory_order_seq_cst);
            while (!leave.load(std::memory_order_seq_cst)) {
                std::this_thread::yield();
            }
        });
        while (!inside.load(std::memory_order_seq_cst)) {
            std::this_thread::yield();
        }
    }
    void Stop() {
        leave.store(true, std::memory_order_seq_cst);
        th.join();
    }
};

int64_t ElapsedUs(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                 t0)
        .count();
}

}  // namespace lifetime_t

TEST_CASE("T1: a voice destroyed through Term is never called again -- the L3 #9 shape") {
    using namespace replay::audio;
    using namespace lifetime_t;
    ResetVoiceLifetimeForTest();

    LifeVoice v;
    Init(&v);
    SinkChain chain;
    chain.sinks[0] = Prove(&v);
    chain.count = 1;

    // Term runs: DestroyVoice, and the field and the sink list keep publishing the voice as valid.
    TermDetourBody(chain.sinks[0].sink, &DestroyOriginal, &v, nullptr);
    REQUIRE(v.destroyed);

    // ??? THE STATE THE OLD GUARD PASSES. Nothing readable changed: the memory is mapped, the vtable
    // pointer is intact and identical to the one that was proven. LiveVtbl -- the whole of the
    // guard before this packet -- accepts it, and the call it would have let through is the
    // 0xC0000005. This CHECK is not a requirement on the shipped design; it is the statement that
    // T1 has something left to catch.
    CHECK(LiveVtbl(chain.sinks[0]) != nullptr);

    VoiceRatioLedger ledger;
    int32_t err = 0;
    size_t gone = 0;
    const size_t applied = ApplyRatioToChain(chain, 2.0f, ledger, &err, &gone);
    CHECK(applied == 0);
    CHECK(gone == 1);
    CHECK(v.callsWhileDestroyed == 0);

    uint64_t samples = 0;
    uint32_t queued = 0;
    size_t goneRead = 0;
    CHECK_FALSE(ReadChainSamplesPlayed(chain, &samples, &queued, &goneRead));
    CHECK(goneRead == 1);
    CHECK(v.callsWhileDestroyed == 0);
    CHECK(VoiceLifetimeStatsSnapshot().refusals >= 2);
}

TEST_CASE("T2: the mark and the generation are published BEFORE the original and cleared after") {
    using namespace replay::audio;
    using namespace lifetime_t;
    ResetVoiceLifetimeForTest();

    LifeVoice v;
    Init(&v);
    ProvenSink proven = Prove(&v);
    const uint64_t before = VoiceGeneration();

    struct Seen {
        uint64_t generation = 0;
        bool refusedUnderMark = false;
    };
    static Seen seen;
    static ProvenSink* observed = nullptr;
    observed = &proven;
    seen = Seen{};

    auto observing = [](void*, void*) -> int64_t {
        seen.generation = VoiceGeneration();
        // Same sink, generation re-read so ONLY the dying mark can refuse it.
        ProvenSink fresh = *observed;
        fresh.generation = VoiceGeneration();
        VoiceCall call(fresh);
        seen.refusedUnderMark = !call.Ok();
        return 1;
    };

    TermDetourBody(proven.sink, observing, &v, nullptr);
    CHECK(seen.generation == before + 1);      // bumped before the original ran
    CHECK(seen.refusedUnderMark);              // and the mark was up while it ran

    ProvenSink after = Prove(&v);              // a fresh proof, taken after Term returned
    VoiceCall call(after);
    CHECK(call.Ok());                          // the mark was cleared on the way out
    CHECK(VoiceLifetimeStatsSnapshot().termEntries == 1);
}

TEST_CASE("T3: both arms of the Dekker handshake fire") {
    using namespace replay::audio;
    using namespace lifetime_t;
    ResetVoiceLifetimeForTest();

    LifeVoice v;
    Init(&v);
    const ProvenSink base = Prove(&v);

    constexpr int kIters = 100000;
    std::atomic<int> refused{0};
    std::atomic<bool> stop{false};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            ProvenSink p = base;
            p.generation = VoiceGeneration();
            VoiceCall call(p);
            if (!call.Ok()) {
                refused.fetch_add(1, std::memory_order_relaxed);
            }
            // Presence with a duration: without it the two threads interleave so rarely that a
            // green run would mean "we did not look", not "it holds".
            for (volatile int i = 0; i < 40; ++i) {
            }
        }
    });

    for (int i = 0; i < kIters; ++i) {
        TermDetourBody(base.sink, &NoopOriginal, &v, nullptr);
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    const VoiceLifetimeStats st = VoiceLifetimeStatsSnapshot();
    MESSAGE("T3 counts: reader refusals=" << refused.load() << " producer waits=" << st.drainWaits
                                          << " timeouts=" << st.drainTimeouts
                                          << " term entries=" << st.termEntries);
    CHECK(refused.load() > 0);        // the mark won a race
    CHECK(st.drainWaits > 0);         // and the reader won one
    CHECK(st.drainTimeouts == 0);     // nobody had to be abandoned at this duration
}

TEST_CASE("T4: a different sink recycled at the same address is refused, and only the generation "
          "does it") {
    using namespace replay::audio;
    using namespace lifetime_t;
    ResetVoiceLifetimeForTest();

    // One storage, two objects: the ABA sub_14139A640 produces when it frees the real sink and a
    // rebuild puts a new one at the same address.
    alignas(LifeVoice) unsigned char storage[sizeof(LifeVoice)];
    LifeVoice* first = Init(new (storage) LifeVoice());
    const ProvenSink stale = Prove(first);

    TermDetourBody(stale.sink, &NoopOriginal, first, nullptr);
    first->~LifeVoice();
    LifeVoice* second = Init(new (storage) LifeVoice());
    TermDetourBody(reinterpret_cast<uintptr_t>(second), &NoopOriginal, second, nullptr);

    // Same address, same vtable, behaviourally identical -- everything the old guard compares.
    REQUIRE(reinterpret_cast<uintptr_t>(second) == stale.sink);
    REQUIRE(second->voice.vtbl == stale.vtbl);
    CHECK(LiveVtbl(stale) != nullptr);          // again: the old guard passes this

    VoiceCall refused(stale);
    CHECK_FALSE(refused.Ok());

    // CONTROL. The same object, the same address, the same vtable -- with the generation the
    // proof would have had. It is accepted, so T4's refusal is attributable to the generation word
    // and to nothing else that happens to differ.
    ProvenSink current = stale;
    current.generation = VoiceGeneration();
    VoiceCall accepted(current);
    CHECK(accepted.Ok());
    second->~LifeVoice();
}

TEST_CASE("T5: the spin is bounded, the original runs anyway, and the give-up is counted") {
    using namespace replay::audio;
    using namespace lifetime_t;
    ResetVoiceLifetimeForTest();

    LifeVoice v;
    Init(&v);
    const ProvenSink proven = Prove(&v);

    HeldReader reader;
    reader.Start(proven);
    REQUIRE(reader.ok.load());          // it really is inside a call

    g_originalRan.store(false, std::memory_order_seq_cst);
    const auto t0 = std::chrono::steady_clock::now();
    TermDetourBody(proven.sink, &FlaggingOriginal, &v, nullptr);
    const int64_t elapsed = ElapsedUs(t0);
    reader.Stop();

    const VoiceLifetimeStats st = VoiceLifetimeStatsSnapshot();
    MESSAGE("T5: waited " << elapsed << "us for a budget of " << kTermDrainBudgetUs
                          << "us, timeouts=" << st.drainTimeouts);
    CHECK(g_originalRan.load());        // the audio thread is never left hanging
    CHECK(st.drainWaits == 1);
    CHECK(st.drainTimeouts == 1);
    CHECK(elapsed >= static_cast<int64_t>(kTermDrainBudgetUs) / 2);
    CHECK(elapsed < 40 * static_cast<int64_t>(kTermDrainBudgetUs));
}

TEST_CASE("T6: a Term reached on the reader's own thread does not wait for the reader") {
    using namespace replay::audio;
    using namespace lifetime_t;
    ResetVoiceLifetimeForTest();

    LifeVoice v;
    Init(&v);
    const ProvenSink proven = Prove(&v);

    g_originalRan.store(false, std::memory_order_seq_cst);
    VoiceCall call(proven);
    REQUIRE(call.Ok());
    // This is row 0.19's shape: ReplaceOutput tears the device down on OUR thread, from inside a
    // use. A spin that counted the caller would wait the whole budget here, once per hidden seek.
    const auto t0 = std::chrono::steady_clock::now();
    TermDetourBody(proven.sink, &FlaggingOriginal, &v, nullptr);
    const int64_t elapsed = ElapsedUs(t0);

    CHECK(g_originalRan.load());
    CHECK(VoiceLifetimeStatsSnapshot().drainWaits == 0);
    // A floor a real 2 ms spin cannot fit under.
    CHECK(elapsed < static_cast<int64_t>(kTermDrainBudgetUs) / 4);
}

namespace lifetime_t {

struct FakeHook {
    const char* factId = nullptr;
    uint64_t pcVa = 0;
    const uint8_t* prologue = nullptr;
    size_t prologueLen = 0;
    const char* configKey = nullptr;
    replay::HookStatus status = replay::HookStatus::Created;
    int creates = 0;
};
FakeHook g_hook;

replay::HookStatus FakeCreate(const char* factId, uint64_t pcVa, const uint8_t* prologue,
                              size_t prologueLen, void*, void** trampoline, const char* configKey) {
    ++g_hook.creates;
    g_hook.factId = factId;
    g_hook.pcVa = pcVa;
    g_hook.prologue = prologue;
    g_hook.prologueLen = prologueLen;
    g_hook.configKey = configKey;
    if (trampoline != nullptr) {
        *trampoline = reinterpret_cast<void*>(&NoopOriginal);
    }
    return g_hook.status;
}
replay::HookStatus FakeArm(const char*) {
    return g_hook.status == replay::HookStatus::Created ? replay::HookStatus::Armed
                                                        : g_hook.status;
}

}  // namespace lifetime_t

TEST_CASE("T7: a refused install is could-not-ask, not `sink gone` and not a rate") {
    using namespace replay::audio;
    using namespace lifetime_t;
    ResetVoiceLifetimeForTest();
    ResetAudioRateForTest();
    SetHookAccessForTest(&FakeCreate, &FakeArm);

    SUBCASE("the site is registered from the fact row, address and guard bytes both") {
        g_hook = FakeHook{};
        g_hook.status = replay::HookStatus::Created;
        CHECK(ArmVoiceLifetime() == LifetimeArm::Armed);
        CHECK(g_hook.pcVa == facts::AUDIO_SINK_TERM);
        CHECK(g_hook.prologueLen == sizeof facts::AUDIO_SINK_TERM_PROLOGUE);
        REQUIRE(g_hook.prologue != nullptr);
        // The guard bytes reach hookmgr unchanged: an install with no prologue, or with fewer bytes
        // than the row records, is a detour nobody checked the site for.
        CHECK(memcmp(g_hook.prologue, facts::AUDIO_SINK_TERM_PROLOGUE,
                     sizeof facts::AUDIO_SINK_TERM_PROLOGUE) == 0);
        CHECK(g_hook.prologueLen == 20);
        CHECK(std::string(g_hook.configKey) == "audio.sink_term_hook");
        // CONTROL for the two subcases below: with the same rate binding up and row 2.6 armed,
        // the line is not could-not-ask. So could-not-ask is caused by the refusal and not by the
        // fixture being unable to answer anything at all.
        replay::IAudioRate* rate = ArmFakeSink(true);
        SetVoiceLifetimeArm(LifetimeArm::Armed);
        REQUIRE(rate->Available());
        CHECK(SinkAvailabilityState() == SinkAvailability::Available);
        CHECK(std::string(SinkAvailabilityLine()).find("could-not-ask") == std::string::npos);
        ReleaseFakeSink();
    }

    SUBCASE("a prologue mismatch is a third state in the ledger line") {
        g_hook = FakeHook{};
        g_hook.status = replay::HookStatus::RefusedByPrologue;
        const LifetimeArm arm = ArmVoiceLifetime();
        CHECK(arm == LifetimeArm::PrologueMismatch);
        SetVoiceLifetimeArm(arm);
        CHECK(SinkAvailabilityState() == SinkAvailability::CouldNotAsk);
        const std::string line = SinkAvailabilityLine();
        CHECK(line.find("could-not-ask") != std::string::npos);
        CHECK(line.find("sink gone") == std::string::npos);
        CHECK(line.find("2.00x") == std::string::npos);
        CHECK(line.find("audio.sink_term_hook") != std::string::npos);
    }

    SUBCASE("the config key off is its own state, distinguishable from a failure") {
        g_hook = FakeHook{};
        g_hook.status = replay::HookStatus::RefusedByConfig;
        const LifetimeArm arm = ArmVoiceLifetime();
        CHECK(arm == LifetimeArm::RefusedByConfig);
        SetVoiceLifetimeArm(arm);
        CHECK(SinkAvailabilityState() == SinkAvailability::CouldNotAsk);
        // The state is the same third state; WHY it is could-not-ask is the other channel, and
        // the two refusals must not collapse into one word.
        CHECK(std::string(LifetimeArmName(arm)).find("is off") != std::string::npos);
        CHECK(std::string(LifetimeArmName(arm)) !=
              std::string(LifetimeArmName(LifetimeArm::PrologueMismatch)));
    }

    SetHookAccessForTest(nullptr, nullptr);
    ResetVoiceLifetimeForTest();
    ResetAudioRateForTest();
}

TEST_CASE("T8: the clamp is still read back off the voice, so 2.0x cannot be reported as 2.00x") {
    using namespace replay::audio;
    using namespace lifetime_t;
    ResetVoiceLifetimeForTest();

    LifeVoice v;
    Init(&v);
    v.maxRatio = 1.0f;      // what CreateSourceVoice asked for; the clamp is silent and returns S_OK
    SinkChain chain;
    chain.sinks[0] = Prove(&v);
    chain.count = 1;

    VoiceRatioLedger ledger;
    int32_t err = 0;
    const size_t applied = ApplyRatioToChain(chain, 2.0f, ledger, &err, nullptr);
    REQUIRE(applied == 1);
    const VoiceRatio* entry = ledger.Find(&v.voice);
    REQUIRE(entry != nullptr);
    // The ledger holds what the voice TOOK. Holding the ask would print 2.00x while the voice runs
    // at 1.0x, which is the one failure S10.4.5 calls worse than no rate control at all.
    CHECK(entry->observed == doctest::Approx(1.0f));
    CHECK(v.ratio == doctest::Approx(1.0f));
}

TEST_CASE("with no audio state bound the block is still written, and it says why it is empty") {
    // L2-RUN-13: CSoundPlayer was never resolved, EncodeCurrentAudioPins returned 0, the writer
    // wrote no block, and the file could not be told apart from one recorded before block 16
    // existed. There is no CSoundPlayer in this process either, which is exactly the case.
    replay::audio::ResetAudioStateForTest();
    replay::audio::ResetAudioPinLedgerForTest();
    REQUIRE(replay::audio::AudioState() == nullptr);

    std::vector<uint8_t> buf(replay::audio::MaxPinBytes());
    replay::audio::PinSource why = replay::audio::PinSource::Bound;
    const size_t n = replay::audio::EncodeCurrentAudioPins(buf.data(), buf.size(), &why);
    REQUIRE(n != 0);
    CHECK(why == replay::audio::PinSource::NoAudioState);

    replay::audio::AudioPins out;
    replay::audio::PinDecodeReport rep;
    REQUIRE(replay::audio::DecodePins(buf.data(), n, &out, &rep) == replay::audio::PinDecode::Ok);
    CHECK(rep.unknownSections == 0);
    CHECK(out.source == replay::audio::PinSource::NoAudioState);
    CHECK(out.stateCount == 0);
    CHECK(out.switchCount == 0);
    CHECK(out.loopCount == 0);
    CHECK(out.seedValid == false);
    CHECK(std::string(replay::audio::PinSourceName(out.source)) == "no audio state");
}

TEST_CASE("a pin set read from a bound state is NOT the same bytes as one with no state") {
    // The two empties have to differ on disk, or the reason field is decoration.
    replay::audio::AudioPins bound;
    bound.source = replay::audio::PinSource::Bound;
    replay::audio::AudioPins unbound;
    unbound.source = replay::audio::PinSource::NoAudioState;
    std::vector<uint8_t> a(replay::audio::MaxPinBytes()), b(replay::audio::MaxPinBytes());
    const size_t na = replay::audio::EncodePins(bound, a.data(), a.size());
    const size_t nb = replay::audio::EncodePins(unbound, b.data(), b.size());
    REQUIRE(na == nb);
    REQUIRE(na != 0);
    CHECK(memcmp(a.data(), b.data(), na) != 0);

    replay::audio::AudioPins out;
    REQUIRE(replay::audio::DecodePins(a.data(), na, &out, nullptr) == replay::audio::PinDecode::Ok);
    CHECK(out.source == replay::audio::PinSource::Bound);
}

// ==============================================================================================
// W2-H / L2-RUN-14 F37: the pin set is a LEDGER, and the keyframe pays only the serialisation
// ==============================================================================================

namespace {

// A CSoundPlayer with an arbitrary number of emitters, each carrying a switch memo. FakeSoundPlayer
// above has exactly one, which is enough to prove the walk is correct and no use at all for showing
// what it costs -- the term that stalled RUN-14 is the emitter count.
struct FakeSoundPlayerN {
    // Taken from the registry the shipping walk reads, not transcribed beside it: a fixture with
    // its own copy of an offset certifies the code against the fixture rather than the binary.
    static constexpr size_t kSize = 1024;
    static constexpr size_t kMemoOff = replay::audio_facts::kMemoCacheOff;
    static constexpr size_t kCountOff = replay::audio_facts::kMemoCountOff;
    static constexpr size_t kEmitterMapOff = replay::audio_facts::kEmitterMapOff;
    static constexpr size_t kEmitterMapCountOff = replay::audio_facts::kEmitterMapCountOff;
    static constexpr size_t kEmitterSwitchMemoOff = replay::audio_facts::kEmitterSwitchMemoOff;
    static constexpr size_t kEmitterSize = 256;
    static constexpr size_t kMapStride = replay::audio_facts::kEmitterMapStride;

    std::vector<uint8_t> block;
    std::vector<uint64_t> stateEntries;
    std::vector<uint8_t> emitterMap;
    std::vector<std::vector<uint8_t>> emitters;
    std::vector<std::vector<uint64_t>> switchEntries;

    static constexpr uint32_t kStateGroup = 17u;
    static constexpr uint32_t kSwitchGroup0 = 4096u;

    FakeSoundPlayerN(uint32_t emitterCount, uint32_t groupsPerEmitter) : block(kSize, 0) {
        stateEntries.push_back(kStateGroup | (uint64_t{170u} << 32));
        Poke(kMemoOff, reinterpret_cast<uintptr_t>(stateEntries.data()));
        Poke(kMemoOff + kCountOff, static_cast<uint64_t>(stateEntries.size()));

        emitters.resize(emitterCount);
        switchEntries.resize(emitterCount);
        emitterMap.assign(static_cast<size_t>(emitterCount) * kMapStride, 0);
        for (uint32_t e = 0; e < emitterCount; ++e) {
            emitters[e].assign(kEmitterSize, 0);
            for (uint32_t g = 0; g < groupsPerEmitter; ++g) {
                switchEntries[e].push_back(static_cast<uint64_t>(kSwitchGroup0 + g) |
                                           (static_cast<uint64_t>(32u + g) << 32));
            }
            const uintptr_t data = reinterpret_cast<uintptr_t>(switchEntries[e].data());
            const uint64_t count = switchEntries[e].size();
            memcpy(emitters[e].data() + kEmitterSwitchMemoOff, &data, sizeof data);
            memcpy(emitters[e].data() + kEmitterSwitchMemoOff + kCountOff, &count, sizeof count);
            const uint32_t gameObj = e + 1;
            const uintptr_t emitterPtr = reinterpret_cast<uintptr_t>(emitters[e].data());
            memcpy(emitterMap.data() + static_cast<size_t>(e) * kMapStride, &gameObj, sizeof gameObj);
            memcpy(emitterMap.data() + static_cast<size_t>(e) * kMapStride + 8, &emitterPtr,
                   sizeof emitterPtr);
        }
        Poke(kEmitterMapOff, reinterpret_cast<uintptr_t>(emitterMap.data()));
        Poke(kEmitterMapCountOff, static_cast<uint64_t>(emitterCount));
    }

    uintptr_t Base() { return reinterpret_cast<uintptr_t>(block.data()); }

private:
    template <class T>
    void Poke(size_t off, T v) {
        memcpy(block.data() + off, &v, sizeof v);
    }
};

using replay::audio::AudioPinLedger;
using replay::audio::AudioPinsTick;
using replay::audio::EncodeCurrentAudioPins;
using replay::audio::ResetAudioPinLedgerForTest;
using replay::audio::SetPinLedgerSoundPlayerForTest;

// Drives the ledger far enough that it has published `cycles` complete pin sets, so a measurement
// is taken against a converged set and not a half-built one.
void RunCycles(uint32_t cycles) {
    const uint64_t want = AudioPinLedger().cycles + cycles;
    for (uint32_t i = 0; i < 5000 && AudioPinLedger().cycles < want; ++i) {
        AudioPinsTick();
    }
}

uint64_t NanosOver(uint32_t iters, void (*body)()) {
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < iters; ++i) {
        body();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return ns == 0 ? 1u : ns;
}

replay::IAudioState* g_perfState = nullptr;
std::vector<uint8_t> g_perfBuf;

// The control: exactly what EncodeCurrentAudioPins did before this change, and what the mutant puts
// back. Its cost has to GROW with the emitter count, or the measurement is not sensitive enough to
// say anything about the subject being flat.
void PerfWalk() {
    replay::audio::AudioPins pins;
    pins.stateCount = static_cast<uint32_t>(g_perfState->SnapshotStates(
        pins.states, replay::audio::AudioPins::kMaxStates, &pins.totals.statesSeen));
    pins.switchCount = static_cast<uint32_t>(g_perfState->SnapshotSwitches(
        pins.switches, replay::audio::AudioPins::kMaxSwitches, &pins.totals.switchesSeen));
    pins.loopCount = static_cast<uint32_t>(g_perfState->SnapshotLoops(
        pins.loops, replay::audio::AudioPins::kMaxLoops, &pins.totals.loopsSeen));
    replay::audio::EncodePins(pins, g_perfBuf.data(), g_perfBuf.size());
}

void PerfEncode() {
    replay::audio::PinSource why = replay::audio::PinSource::Unstated;
    EncodeCurrentAudioPins(g_perfBuf.data(), g_perfBuf.size(), &why);
}

struct PerfPoint {
    uint64_t walkNs;
    uint64_t encodeNs;
};

PerfPoint MeasureAt(uint32_t emitters) {
    static FakeSoundPlayerN* sp = nullptr;
    delete sp;
    sp = new FakeSoundPlayerN(emitters, 2);
    replay::audio::ResetAudioStateForTest();
    ResetAudioPinLedgerForTest();
    g_wwiseStates.clear();
    g_wwiseSwitches.clear();
    REQUIRE(InitAudioState(FakeExports(), sp->Base()));
    SetPinLedgerSoundPlayerForTest(sp->Base());
    g_perfState = replay::audio::AudioState();
    REQUIRE(g_perfState != nullptr);
    RunCycles(3);

    constexpr uint32_t kIters = 200;
    PerfWalk();   // warm both paths so first-touch page faults are not in the sample
    PerfEncode();
    PerfPoint p;
    p.walkNs = NanosOver(kIters, &PerfWalk) / kIters;
    p.encodeNs = NanosOver(kIters, &PerfEncode) / kIters;
    return p;
}

}  // namespace

TEST_CASE("F37: the keyframe cost is flat in the emitter count, and the walk it replaced is not") {
    // L2-RUN-14 measured the `audiopins` tick phase at 6589-7247 us MEAN and 159358 us MAX on the
    // game thread, twice a second, and the session lost 13.6% of its ticks to it. The cost is the
    // whole-table walk -- a VirtualQuery pair and a Wwise query per emitter per switch group --
    // paid from scratch inside the keyframe sample.
    //
    // Same-run ratios, never a wall-clock threshold: this machine's absolute numbers mean nothing
    // on another one, and the CONTROL is what makes the subject's flatness a result rather than a
    // measurement too coarse to see anything. Note what the control understates: the Wwise queries
    // are stubs here, so the walk's real cost on a live host is strictly worse than measured, while
    // the ledger's is not -- it makes at most kTruthPerTick of them a tick whatever they cost.
    g_perfBuf.assign(replay::audio::MaxPinBytes(), 0);

    const PerfPoint one = MeasureAt(1);
    const PerfPoint many = MeasureAt(64);
    const PerfPoint most = MeasureAt(512);

    MESSAGE("emitters=1   walk " << one.walkNs << " ns   encode " << one.encodeNs << " ns");
    MESSAGE("emitters=64  walk " << many.walkNs << " ns   encode " << many.encodeNs << " ns");
    MESSAGE("emitters=512 walk " << most.walkNs << " ns   encode " << most.encodeNs << " ns");

    // The control, which is also the proof the instrument can see the thing: the walk is O(n).
    CHECK(most.walkNs > many.walkNs);
    CHECK(most.walkNs >= one.walkNs * 8);

    // The subject: the keyframe now serialises a struct that is already built. What is left is a
    // memcpy of the sections, which grows with the SET, not with the table walked to find it --
    // 512 emitters carry 512 stored switch rows against 2, so a small factor is expected and a
    // proportional one is the walk having crept back in.
    CHECK(most.encodeNs <= one.encodeNs * 4 + 2000);
    CHECK(most.encodeNs * 8 <= most.walkNs);
}

TEST_CASE("F37: no single tick walks more than its slice of the emitter table") {
    // The bound is what makes the cost payable at all: spreading the same walk over ticks is only
    // a fix if no tick can take the whole of it. Counted rather than timed, so it cannot go green
    // on a fast machine.
    FakeSoundPlayerN sp(512, 2);
    replay::audio::ResetAudioStateForTest();
    ResetAudioPinLedgerForTest();
    g_wwiseStates.clear();
    g_wwiseSwitches.clear();
    REQUIRE(InitAudioState(FakeExports(), sp.Base()));
    SetPinLedgerSoundPlayerForTest(sp.Base());
    REQUIRE(replay::audio::AudioState() != nullptr);

    RunCycles(3);
    const replay::audio::PinLedgerStats st = AudioPinLedger();
    MESSAGE("cycles " << st.cycles << " ticks " << st.ticks << " emitterSteps " << st.emitterSteps
                      << " maxEmittersInATick " << st.maxEmittersInATick << " maxTruthInATick "
                      << st.maxTruthInATick << " truthQueries " << st.truthQueries);
    CHECK(st.cycles >= 3u);
    CHECK(st.maxEmittersInATick <= 16u);   // kEmittersPerTick
    // kTruthPerTick is 8 and the tick stops at the emitter boundary after the budget is gone, so
    // the overrun a tick can carry is one emitter's group count -- two here.
    CHECK(st.maxTruthInATick <= 8u + 2u);
    CHECK(st.emittersWalked == 512u);      // and the whole table is still walked, once per cycle
    CHECK(st.lastCycleTicks >= 512u / 16u);

    // The reuse is why the Wwise term is not the walk's, merely spread: a table that is not
    // changing costs the revalidation sweep and nothing else.
    const uint64_t before = AudioPinLedger().truthQueries;
    RunCycles(1);
    const uint64_t spent = AudioPinLedger().truthQueries - before;
    MESSAGE("truth queries over one steady cycle: " << spent);
    CHECK(spent <= 16u);

    ResetAudioPinLedgerForTest();
    replay::audio::ResetAudioStateForTest();
}

TEST_CASE("F37: the ledger's block is byte-identical to the block the walk produced") {
    // The cost fix is only a fix if the file is the same. Anything the walk put in the block and
    // the ledger does not is fidelity lost to performance, which is the trade this project does
    // not make.
    FakeSoundPlayerN sp(8, 3);
    replay::audio::ResetAudioStateForTest();
    ResetAudioPinLedgerForTest();
    g_wwiseStates = {{FakeSoundPlayerN::kStateGroup, 48879u}};
    g_wwiseSwitches = {{FakeSoundPlayerN::kSwitchGroup0, 119u},
                       {FakeSoundPlayerN::kSwitchGroup0 + 1, 120u},
                       {FakeSoundPlayerN::kSwitchGroup0 + 2, 121u}};
    REQUIRE(InitAudioState(FakeExports(), sp.Base()));
    SetPinLedgerSoundPlayerForTest(sp.Base());
    replay::IAudioState* state = replay::audio::AudioState();
    REQUIRE(state != nullptr);
    RunCycles(6);

    replay::audio::AudioPins walked;
    walked.source = replay::audio::PinSource::Bound;
    walked.stateCount = static_cast<uint32_t>(state->SnapshotStates(
        walked.states, replay::audio::AudioPins::kMaxStates, &walked.totals.statesSeen));
    walked.switchCount = static_cast<uint32_t>(state->SnapshotSwitches(
        walked.switches, replay::audio::AudioPins::kMaxSwitches, &walked.totals.switchesSeen));
    walked.loopCount = static_cast<uint32_t>(state->SnapshotLoops(
        walked.loops, replay::audio::AudioPins::kMaxLoops, &walked.totals.loopsSeen));
    walked.totals.statesStored = walked.stateCount;
    walked.totals.switchesStored = walked.switchCount;
    walked.totals.loopsStored = walked.loopCount;
    walked.totals.targetsDropped = replay::audio::AudioTargetsDropped();
    // The one field the walk never filled: IAudioState reports a total but not the width it
    // walked, so AUDIOPINS S5 left emittersWalked at 0. The ledger owns its own cursor and knows.
    // The expected value is the fixture's emitter count, not the ledger's own answer.
    walked.totals.emittersWalked = 8u;
    CHECK(AudioPinLedger().emittersWalked == 8u);

    std::vector<uint8_t> fromWalk(replay::audio::MaxPinBytes());
    std::vector<uint8_t> fromLedger(replay::audio::MaxPinBytes());
    const size_t nw = replay::audio::EncodePins(walked, fromWalk.data(), fromWalk.size());
    replay::audio::PinSource why = replay::audio::PinSource::Unstated;
    const size_t nl = EncodeCurrentAudioPins(fromLedger.data(), fromLedger.size(), &why);
    REQUIRE(nw != 0);
    REQUIRE(nl == nw);
    CHECK(why == replay::audio::PinSource::Bound);
    CHECK(memcmp(fromWalk.data(), fromLedger.data(), nw) == 0);

    // And the values are the ones Wwise gave, not the memo's cache -- N-16's whole point, and the
    // thing a ledger reusing a stale row would quietly lose.
    replay::audio::AudioPins out;
    replay::audio::PinDecodeReport rep;
    REQUIRE(replay::audio::DecodePins(fromLedger.data(), nl, &out, &rep) ==
            replay::audio::PinDecode::Ok);
    CHECK(rep.unknownSections == 0);
    REQUIRE(out.stateCount == 1u);
    CHECK(out.states[0].value == 48879u);
    REQUIRE(out.switchCount == 24u);
    CHECK(out.switches[0].value == 119u);
    CHECK(out.switches[1].value == 120u);

    ResetAudioPinLedgerForTest();
    replay::audio::ResetAudioStateForTest();
}

// ---- O-1: the label and the mode are LATCHED, and the closed loop only runs at 1.0x ------------
//
// The owner's complaint was the panel alternating between "audio rate control unavailable" and
// "audio: slowed with picture" at a rate he had not touched, in time with the hitching. Both
// halves of the answer -- RateLabel() reading the latched plan instead of `available_`, and the PI
// loop refusing to run at anything but 1.0x -- shipped with no case that could fail them, because
// the fixture built the module with the loop off and nothing could drive availability false.
// These run the module through real availability flaps with the loop ON.

namespace {

// One second of 60 fps frames. `outageFrames` of them have no sink: a FLAP, not an outage --
// ReArm polls at 4 Hz and three consecutive misses legitimately downgrade the label, so a flap
// this test may assert nothing changed for has to stay inside one poll period.
void RunSecond(double& scene, float ratio, int outageFrames) {
    for (int f = 0; f < 60; ++f) {
        g_fake.sinkBuilt = (f >= outageFrames);
        scene += (1.0 / 60.0) * (double)ratio;
        replay::audio::TickAudioRate(scene, 1.0 / 60.0);
    }
}

}  // namespace

TEST_CASE("O-1: the rate label and mode stay latched across availability flaps") {
    for (float ratio : {0.25f, 0.5f}) {
        replay::IAudioRate* rate = ArmFakeSink(true, /*closedLoop=*/true);
        REQUIRE(rate != nullptr);
        REQUIRE(rate->Available());

        rate->SetRate(ratio);
        const char* const latched = rate->RateLabel();
        REQUIRE(latched != nullptr);
        CHECK(std::string(latched) == "audio: slowed with picture");

        // Ten seconds. The sink goes on the second of every pair, which is one failed re-arm per
        // outage -- short of kArmFailuresBeforeDowngrade, so nothing may legitimately change.
        //
        // The unlatch is driven by the device-status event, which is what actually unlatches a sink.
        // It used to be driven by accident: the sink went missing, the phantom drift re-commanded
        // over the gap, and the refused command dropped availability. PITCH-FIX-2 removes that
        // churn, so the flap this case needs has to be asked for.
        double scene = 0.0;
        size_t flaps = 0;
        REQUIRE(g_deviceCallback != nullptr);
        for (int s = 0; s < 10; ++s) {
            g_fake.sinkBuilt = false;
            g_deviceCallback(nullptr, 0, 0, 0, 0);
            replay::audio::TickAudioRate(scene, 1.0 / 60.0);
            if (!rate->Available()) ++flaps;
            CHECK(std::string(rate->RateLabel()) == std::string(latched));
            RunSecond(scene, ratio, /*outageFrames=*/10);
            CHECK(std::string(rate->RateLabel()) == std::string(latched));
        }
        // The case is vacuous unless availability really moved: this is what ArmFakeSink could not
        // do before, and it is what M2 needs in order to die.
        CHECK(flaps > 0);
        CHECK(std::string(rate->RateLabel()) == "audio: slowed with picture");
        ReleaseFakeSink();
    }
}

TEST_CASE("O-1: the closed loop does not command the sink at a slowed rate") {
    replay::IAudioRate* rate = ArmFakeSink(true, /*closedLoop=*/true);
    REQUIRE(rate != nullptr);

    // ??? THE POSITIVE CONTROL IS THE GATE (PITCH-FIX-2 S5). The loop trims the commanded ratio in
    // BOTH directions, so it only runs for a voice that can serve an upward trim. A CAkSinkXAudio2
    // voice is created with MaxFrequencyRatio = 1.0 and cannot, which is why the shipped title
    // never sees this loop command anything; a voice with real headroom -- what the Level-1X rate
    // hook would create -- is what makes the loop runnable, and what keeps the assertion below from
    // being vacuous for the reason S5 exists rather than in spite of it.
    FakeVoiceState headroom{};
    headroom.voice.vtbl = &FakeVoiceVtbl();
    replay::audio::ProvenSink seeded{};
    seeded.sink = 0x7000;
    seeded.voice = &headroom.voice;
    seeded.vtbl = headroom.voice.vtbl;
    seeded.generation = replay::audio::SinkGeneration(seeded.sink);
    REQUIRE(replay::audio::SinkRatioLedger().Adopt(seeded) != nullptr);
    replay::audio::SinkRatioLedger().FindMutable(&headroom.voice)->maxRatio = 2.0f;
    // This fixture commands through the apply seam, which never touches the real ledger, so the
    // seeded row has to be told what the seam delivered. Left un-told it reads as a voice with our
    // write permanently outstanding, and the 4 Hz reassert retry -- not the loop -- would be what
    // the counts below measured.
    const auto delivered = [&headroom](float r) {
        replay::audio::SinkRatioLedger().Observe(&headroom.voice, r);
    };

    // 1.0x first, to prove the loop is armed and this fixture can drive it at all: without this a
    // mutant that disabled the loop outright would pass the assertion below.
    rate->SetRate(1.0f);
    delivered(1.0f);
    double scene = 0.0;
    const size_t beforeNormal = g_fake.applied.size();
    for (int s = 0; s < 3; ++s) RunSecond(scene, 1.0f, /*outageFrames=*/0);
    const size_t loopCommands = g_fake.applied.size() - beforeNormal;
    // Without this the case is vacuous: a mutant that disabled the loop outright would pass the
    // assertion below by doing nothing at all. Measured 24 over 180 frames -- the trim moves in
    // steps, so this is well under one command per frame even where the loop IS allowed to run.
    CHECK(loopCommands > 10);

    rate->SetRate(0.25f);
    delivered(0.25f);
    const size_t beforeSlow = g_fake.applied.size();
    for (int s = 0; s < 5; ++s) RunSecond(scene, 0.25f, /*outageFrames=*/0);
    // Five seconds -- 300 frames. Measured 10 with the gate in place -- not zero, because this
    // fake's chain reports voices gone, which drops availability and the 4 Hz re-arm re-commands
    // the planned ratio. THE BOUND IS THE MEASURED DIFFERENCE, not a round number: taking the rate
    // gate out of the closed loop moves this fixture to 20, not to the ~300 a free-running
    // per-frame rewrite would give, because the fake's own outages keep re-planning the ratio the
    // loop is chasing. A bound of 60 admitted that mutant; 15 does not.
    const size_t slowCommands = g_fake.applied.size() - beforeSlow;
    MESSAGE("commands at 1.0x: " << loopCommands << ", at 0.25x: " << slowCommands);
    CHECK(slowCommands < 15);
    CHECK(std::string(rate->RateLabel()) == "audio: slowed with picture");
    ReleaseFakeSink();
}

// =================================================================================================
// AUDIO-FIX-3. RUN-VERIFY-2 measured the loudspeaker across three 1x -> 0.1x -> 1x cycles: one was
// really slowed, two were not moved at all, and the two that failed each carried
// `audio drift corrected: expected 1.0001 found 0.1000` within a second of the rate change. The
// number the corrector calls `expected` is the last ask that reached EVERY voice; the number it
// calls a fault is our own fresh slow-mo write.
// =================================================================================================

namespace {

// The chain the drift cases drive. Unlike FakeApply, this one goes through the shipping per-voice
// path, so the ledger the drift check reads is built by the code under test rather than by the
// fixture -- a fake that recorded the ask would make every one of these cases pass on its own
// bookkeeping.
FakeChain g_driftChain;

size_t DriftProbe(const replay::audio::AkExports&, replay::audio::SinkChain* out) {
    ++g_fake.probes;
    g_driftChain.Reprobe();
    *out = g_driftChain.chain;
    return g_driftChain.chain.count;
}

size_t DriftApply(const replay::audio::AkExports&, float ratio, int32_t* firstError, size_t* gone) {
    return replay::audio::ApplyRatioToChain(g_driftChain.chain, ratio,
                                            replay::audio::SinkRatioLedger(), firstError, gone);
}

// Everything the module said over one drive, joined. The notice channel holds one line at a time
// and supersedes the rest, so a case that drained once would be asserting against whichever line
// happened to be last.
std::string DriveAndCollect(int frames, double dt) {
    std::string said;
    char line[512];
    uint32_t superseded = 0;
    for (int i = 0; i < frames; ++i) {
        replay::audio::TickAudioRate(0.0, dt);
        while (replay::audio::DrainAudioRateNotice(line, sizeof line, &superseded)) {
            said += line;
            said += "\n";
        }
    }
    return said;
}

}  // namespace

TEST_CASE("a rate change that reached part of the chain is not drift in the part that took it") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmFakeSink(true);
    REQUIRE(rate->Available());

    // Two sinks, and the second refuses the write -- a sink mid-rebuild, the state the session's
    // nine distinct sink pointers in six minutes kept passing through. The first takes 0.1x.
    g_driftChain.Build({1.0f, 1.0f});
    g_driftChain.voices[0].staleReadBack = true;
    g_driftChain.voices[1].staleReadBack = true;
    g_driftChain.voices[1].refuseWith = -1;   // any HRESULT failure: the fake tests the sign
    SetSinkAccessForTest(&DriftProbe, &DriftApply);

    rate->SetRate(0.1f);
    CHECK(g_driftChain.voices[0].ratio == doctest::Approx(0.1f));
    CHECK(g_driftChain.voices[1].ratio == doctest::Approx(1.0f));
    // The ask did not reach every voice, so it is not delivered -- which is exactly the state in
    // which the last delivered command stays behind the transport.
    CHECK(OutstandingVoices(SinkRatioLedger(), 0.1f) == 1);

    const std::string said = DriveAndCollect(240, 1.0 / 60.0);   // 4 s: two drift checks
    MESSAGE(said);

    // The voice that took the rate is still at the rate, and was never named as the fault. The
    // correction re-commanded `expected` over it, which is the owner's "it goes slow-mo and then
    // comes back on its own".
    CHECK(g_driftChain.voices[0].ratio == doctest::Approx(0.1f));
    CHECK(said.find("expected 1.0000 found 0.1000") == std::string::npos);
    // And the voice that refused IS reported, against the rate the transport is actually at.
    CHECK(said.find("expected 0.1000 found 1.0000") != std::string::npos);
    ReleaseFakeSink();
}

// ??? THIS CASE IS NOT RED AGAINST THE PRE-FIX LOGIC, AND THAT IS THE FINDING ABOUT IT. The read-lag
// window in VerifyAgainstSinks stops a check from RUNNING inside kRatioReadLagMs of a rate change;
// the streak reset in RecordCommand independently stops the reading that check would take from
// being adjudicated. Every sequence this fixture can produce is caught by the streak first, so the
// window has no independent killer here -- it is defence in depth over a second mechanism, kept
// because the two fail differently (the streak protects the verdict, the window protects the probe,
// the `observed` write and the CommandPlanned that follows a verdict). What this case does hold to
// is the direction that WOULD be a regression: the window must not swallow a real finding.
TEST_CASE("the read-lag window delays a drift verdict and does not silence one") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmFakeSink(true);
    g_driftChain.Build({1.0f, 1.0f});
    g_driftChain.voices[0].staleReadBack = true;
    g_driftChain.voices[1].staleReadBack = true;
    g_driftChain.voices[1].refuseWith = -1;
    SetSinkAccessForTest(&DriftProbe, &DriftApply);

    // A drift check falls due every kRatioDriftCheckMs, so one is always about to happen when the
    // user touches the speed. 200 ms of ticks is inside kRatioReadLagMs of the change.
    DriveAndCollect(240, 1.0 / 60.0);
    rate->SetRate(0.1f);
    const std::string said = DriveAndCollect(12, 1.0 / 60.0);
    CHECK(said.find("drift corrected") == std::string::npos);
    // ...and the window is not a silencer: the refusing voice is still found once it has passed.
    const std::string later = DriveAndCollect(240, 1.0 / 60.0);
    CHECK(later.find("drift corrected") != std::string::npos);
    ReleaseFakeSink();
}

TEST_CASE("a rate change invalidates a drift verdict that was one sample in") {
    using namespace replay::audio;
    FakeChain fc;
    fc.Build({1.0f});
    fc.voices[0].staleReadBack = true;
    VoiceRatioLedger ledger;
    REQUIRE(ApplyRatioToChain(fc.chain, 0.5f, ledger, nullptr) == 1);

    // A value that is nobody's command, one sample in: forgiven once, because one sample is the lag.
    fc.voices[0].ratio = 0.9f;
    fc.voices[0].pendingStaleRead = false;
    CHECK(CheckChainDrift(fc.chain, ledger, 0.5f, nullptr) == 0);
    REQUIRE(ledger.Find(&fc.voices[0].voice)->driftStreak == 1);

    // The user changes speed before the second sample. The streak was taken at the old rate, about
    // a question nobody is asking any more; counting it is a drift line on the first check after
    // every rate change, which is what the 0.77 s lines in the 2026-09-21 log are.
    REQUIRE(ApplyRatioToChain(fc.chain, 0.25f, ledger, nullptr) == 1);
    CHECK(ledger.Find(&fc.voices[0].voice)->driftStreak == 0);

    fc.voices[0].ratio = 0.9f;
    fc.voices[0].pendingStaleRead = false;
    CHECK(CheckChainDrift(fc.chain, ledger, 0.25f, nullptr) == 0);   // one sample is still the lag
    CHECK(CheckChainDrift(fc.chain, ledger, 0.25f, nullptr) == 1);   // two of it is a writer
}

TEST_CASE("the session's rapid-fire cycle does not re-learn a baseline off our own ratio") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmFakeSink(true);
    g_driftChain.Build({1.0f});
    g_driftChain.voices[0].staleReadBack = true;
    SetSinkAccessForTest(&DriftProbe, &DriftApply);

    // 01:38:40 -> 01:38:43, the operator's own four commands about 1.1 s apart. Under the learned
    // baseline this walked 1.0000 -> 0.0312 -> 0.0010, and the restore to 1.0x wrote 0.0010.
    for (float r : {0.5f, 0.25f, 0.1f, 1.0f}) {
        rate->SetRate(r);
        DriveAndCollect(66, 1.0 / 60.0);
    }
    const SinkRatioCensus census = SinkRatioCensusNow(SinkRatioLedger(), 1.0f);
    CHECK(census.relearned == 0);
    CHECK(census.distinctBaselines == 1);
    CHECK(census.baselines[0] == kShippedFrequencyRatio);
    CHECK(g_driftChain.voices[0].ratio == 1.0f);   // bit-exact, not approximately the engine's pitch
    ReleaseFakeSink();
}

TEST_CASE("a rate recorded while there was no sink is delivered by whichever path re-arms") {
    using namespace replay::audio;
    replay::IAudioRate* rate = ArmFakeSink(true);
    REQUIRE(rate->Available());

    // The paused-set arm of 2026-09-21: the transport is paused, so the engine is suspended and
    // Wwise has taken the sink down with it. The speed is changed, and the resume tick is the first
    // thing that reaches the audio module.
    rate->Suspend(true);
    rate->SetRate(0.1f);
    CHECK(g_fake.wakeups == 1);
    CHECK_FALSE(rate->Available());        // Wwise rebuilds CAkSinkXAudio2 on its own thread
    CHECK(g_fake.applied.empty());
    CHECK(std::string(rate->RateLabel()) == "audio rate control unavailable");

    // It comes back through the device-status callback rather than through the re-arm poll -- the
    // path that used to arm the sink and deliver nothing, leaving the module available at 1.0x with
    // 0.10x recorded and a label reading "unavailable" for the rest of the viewing.
    g_fake.sinkBuilt = true;
    REQUIRE(g_deviceCallback != nullptr);
    g_deviceCallback(nullptr, 0, 0, 0, 0);
    replay::audio::TickAudioRate(0.0, 1.0 / 60.0);

    CHECK(rate->Available());
    REQUIRE(g_fake.applied.size() == 1);
    CHECK(g_fake.applied[0] == 0.1f);
    CHECK(std::string(rate->RateLabel()) == "audio: slowed with picture");
    ReleaseFakeSink();
}

TEST_CASE("no tick pays for a whole emitter's switch groups, however many it has") {
    // The 2026-09-21 hitch recorder fired twelve times, and its dominant signature is 21-32 ms
    // inside the third audio phase -- AudioPinsTick. This is the one term in that phase with no
    // bound: the tick used to stop only on an emitter boundary, so a single emitter's group count
    // -- up to kMaxPlausibleMemoCount -- was paid in one frame, each row a Wwise query that takes
    // the sound engine's global lock. Counted, not timed, so it cannot go green on a fast machine.
    constexpr uint32_t kEmitters = 3;
    constexpr uint32_t kGroupsPerEmitter = 400;
    FakeSoundPlayerN sp(kEmitters, kGroupsPerEmitter);
    replay::audio::ResetAudioStateForTest();
    ResetAudioPinLedgerForTest();
    g_wwiseStates.clear();
    g_wwiseSwitches.clear();
    REQUIRE(InitAudioState(FakeExports(), sp.Base()));
    SetPinLedgerSoundPlayerForTest(sp.Base());
    REQUIRE(replay::audio::AudioState() != nullptr);

    RunCycles(2);
    const replay::audio::PinLedgerStats st = AudioPinLedger();
    MESSAGE("cycles " << st.cycles << " ticks " << st.ticks << " maxTruthInATick "
                      << st.maxTruthInATick << " truthQueries " << st.truthQueries << " deferrals "
                      << st.deferrals);
    CHECK(st.cycles >= 2u);
    // The bound is the module's own per-tick query budget -- the MESSAGE above prints what it
    // measured. Stated against the fixture's group count rather than against that number, because
    // what this case is about is that ONE EMITTER's groups are no longer a tick's bill.
    CHECK(st.maxTruthInATick < kGroupsPerEmitter);
    // And the set is still complete: a bound that dropped rows would be a different bug.
    CHECK(st.emittersWalked == kEmitters);
    replay::audio::AudioPins pins;
    REQUIRE(replay::audio::GetAudioPins(&pins));
    CHECK(pins.totals.switchesSeen == kEmitters * kGroupsPerEmitter);
    // 1200 rows against a 512-row block: the ones past the block are seen and not stored, which is
    // the pre-existing bound and is what the totals row exists to make visible.
    CHECK(pins.switchCount == replay::audio::AudioPins::kMaxSwitches);
}

// AUDIO-CUTOFF-FIX. The foley-mix state hold (audio/foleymix.h). The fake below answers GetState
// with whatever Wwise "holds", records every SetState, and can be told to refuse either call.
namespace {

class FoleyStateFake final : public IAudioState {
public:
    uint32_t held = 0;
    bool getFails = false;
    bool setFails = false;
    std::vector<std::pair<uint32_t, uint32_t>> sets;

    bool SetState(Symbol64 group, Symbol64 value) override {
        if (setFails) return false;
        sets.push_back({static_cast<uint32_t>(group), static_cast<uint32_t>(value)});
        held = static_cast<uint32_t>(value);
        return true;
    }
    bool SetSwitch(Symbol64, Symbol64, uint64_t) override { return true; }
    bool GetStateTruth(Symbol64 group, Symbol64& value) const override {
        if (getFails || group != kFoleyMixStateGroup) return false;
        value = held;
        return true;
    }
    bool GetSwitchTruth(Symbol64, uint64_t, Symbol64&) const override { return false; }
    void Reconcile() override {}
    void PinRandom(uint32_t, const Blob&) override {}
    bool ReadRandomPins(uint32_t&, Blob&) const override { return false; }
    size_t SnapshotStates(AudioStatePair*, size_t, uint32_t*) const override { return 0; }
    size_t SnapshotSwitches(AudioSwitchTriple*, size_t, uint32_t*) const override { return 0; }
    size_t SnapshotLoops(AudioLoopPin*, size_t, uint32_t*) const override { return 0; }
    void SetTargets(const AudioStatePair*, size_t, const AudioSwitchTriple*, size_t) override {}
    const ReconcileReport& LastReconcile() const override { return report_; }

private:
    ReconcileReport report_{};
};

}  // namespace

TEST_CASE("AUDIO-CUTOFF-FIX: the headset hold sets the in-headset foley state once and re-asserts a flip") {
    FoleyStateFake wwise;
    wwise.held = kFoleyMixBroadcastState;   // what a replay session was measured to play
    FoleyMixHold hold(FoleyMix::Headset, kFoleyMixPollMs);

    // First tick asserts at once; the fake now holds "false".
    CHECK(hold.Tick(&wwise, 5) == FoleyMixOutcome::Corrected);
    REQUIRE(wwise.sets.size() == 1);
    CHECK(wwise.sets[0].first == kFoleyMixStateGroup);
    CHECK(wwise.sets[0].second == kFoleyMixHeadsetState);
    CHECK(hold.Report().lastSeen == kFoleyMixBroadcastState);

    // Inside the interval nothing is even read; at the interval a matching answer is Held.
    CHECK(hold.Tick(&wwise, 500) == FoleyMixOutcome::Idle);
    CHECK(hold.Tick(&wwise, 1005) == FoleyMixOutcome::Held);
    CHECK(wwise.sets.size() == 1);

    // The engine's own enable path flips it back; the next poll corrects it again.
    wwise.held = kFoleyMixBroadcastState;
    CHECK(hold.Tick(&wwise, 2005) == FoleyMixOutcome::Corrected);
    CHECK(wwise.sets.size() == 2);
    CHECK(hold.Report().corrections == 2);
    CHECK(hold.Report().polls == 3);
    CHECK(hold.Report().refusals == 0);
}

TEST_CASE("AUDIO-CUTOFF-FIX: engine mode never reads or writes; broadcast mode wants the other child") {
    FoleyStateFake wwise;
    wwise.held = kFoleyMixHeadsetState;
    FoleyMixHold engine(FoleyMix::Engine, kFoleyMixPollMs);
    CHECK(engine.Tick(&wwise, 5) == FoleyMixOutcome::Idle);
    CHECK(engine.Report().polls == 0);
    CHECK(wwise.sets.empty());

    FoleyMixHold broadcast(FoleyMix::Broadcast, kFoleyMixPollMs);
    CHECK(broadcast.WantedState() == kFoleyMixBroadcastState);
    CHECK(broadcast.Tick(&wwise, 5) == FoleyMixOutcome::Corrected);
    CHECK(wwise.held == kFoleyMixBroadcastState);
}

TEST_CASE("AUDIO-CUTOFF-FIX: refusals are counted, never retried inside the interval, and Reset re-asserts") {
    FoleyStateFake wwise;
    wwise.held = kFoleyMixBroadcastState;
    FoleyMixHold hold(FoleyMix::Headset, kFoleyMixPollMs);

    CHECK(hold.Tick(nullptr, 5) == FoleyMixOutcome::Refused);   // CSoundPlayer never resolved
    wwise.getFails = true;
    CHECK(hold.Tick(&wwise, 1005) == FoleyMixOutcome::Refused);
    wwise.getFails = false;
    wwise.setFails = true;
    CHECK(hold.Tick(&wwise, 2005) == FoleyMixOutcome::Refused);   // writes disabled downstream
    CHECK(hold.Report().refusals == 3);
    CHECK(wwise.sets.empty());

    wwise.setFails = false;
    CHECK(hold.Tick(&wwise, 2100) == FoleyMixOutcome::Idle);      // still inside the interval
    hold.Reset();                                                  // a demo (re)load
    CHECK(hold.Tick(&wwise, 2100) == FoleyMixOutcome::Corrected);
    CHECK(wwise.held == kFoleyMixHeadsetState);
}

TEST_CASE("AUDIO-CUTOFF-FIX: the config word is parsed exactly and a typo is refused") {
    FoleyMix mix = FoleyMix::Broadcast;
    CHECK(ParseFoleyMix("engine", &mix));
    CHECK(mix == FoleyMix::Engine);
    CHECK(ParseFoleyMix("headset", &mix));
    CHECK(mix == FoleyMix::Headset);
    CHECK(ParseFoleyMix("broadcast", &mix));
    CHECK(mix == FoleyMix::Broadcast);
    CHECK_FALSE(ParseFoleyMix("Headset", &mix));
    CHECK_FALSE(ParseFoleyMix("", &mix));
    CHECK_FALSE(ParseFoleyMix(nullptr, &mix));
    CHECK(mix == FoleyMix::Broadcast);   // untouched by the refusals
    CHECK(std::string(FoleyMixName(FoleyMix::Headset)) == "headset");
}

// The row used to be read without being declared in config_keys.inc, and an undeclared key answers
// the caller's default whatever the file says: config.json could never change the mix.
TEST_CASE("audio.foley_mix in config.json reaches the foley-mix hold") {
    struct Case {
        const char* file;
        FoleyMix    want;
        bool        parsed;
    };
    const Case cases[] = {
        {"{}", FoleyMix::Headset, true},
        {R"({"audio.foley_mix": "broadcast"})", FoleyMix::Broadcast, true},
        {R"({"audio": {"foley_mix": "engine"}})", FoleyMix::Engine, true},
        {R"({"audio.foley_mix": "loud"})", FoleyMix::Engine, false},
    };
    for (const Case& c : cases) {
        CAPTURE(c.file);
        replay::config::ResetForTest();
        REQUIRE(replay::config::LoadFromTextForTest(c.file, std::strlen(c.file)));
        FoleyMix mix = FoleyMix::Broadcast;
        const char* word = nullptr;
        CHECK(FoleyMixFromConfig(replay::config::Interface(), &mix, &word) == c.parsed);
        CHECK(mix == c.want);
        REQUIRE(word != nullptr);
        CHECK(FoleyMixHold(mix, kFoleyMixPollMs).Mix() == c.want);
    }
    replay::config::ResetForTest();
}
