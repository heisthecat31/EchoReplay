// The wiring arm for DIAG-WIRING gap 4. The shipping audio module TU is compiled in and run the
// way the DLL runs it -- modreg::InitAll, then modreg::TickAll -- against a fake transport and a
// counting IAudioRate substituted into the services table AFTER Init, because the tick must bind
// to services every frame and not once at Init (playback publishes the transport at order 300,
// long after this module's Init at 45). A tick that never reads services->transport, or that
// binds it at Init, drives nothing here and the arm goes red.

#include <doctest.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "audio/audio_facts.h"
#include "audio/audiorate.h"
#include "audio/audiopins.h"
#include "audio/audiopinsource.h"
#include "audio/audiostate_init.h"
#include "audio/audioresidue.h"
#include "audio/voicelifetime.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/modreg.h"
#include "bootstrap/services.h"
#include "engine/audiorate.h"
#include "engine/tick.h"
#include "playback/core/playback.h"
#include "playback/core/transport.h"

using namespace replay;

// W2-B's module TU is not in this process, and these two live there. The answer supplied is the
// one the real module gives before it has initialised -- "not consulted" -- which keeps the audio
// tick's residue-arming report on its too-early branch rather than filing a false MISSING CALL.
namespace replay {
namespace playback {
ResidueStatus ResidueSnapshot() { return ResidueStatus{}; }
const char* ResidueDiagnostic(const char*) { return nullptr; }
}  // namespace playback
}  // namespace replay

namespace {

class NullLog final : public ILog {
public:
    uint32_t lines = 0;
    std::vector<std::string> text;
    void Write(LogLevel, const char*, const char* fmt, ...) override {
        ++lines;
        char line[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof line, fmt, ap);
        va_end(ap);
        text.emplace_back(line);
    }
    bool Enabled(LogLevel) const override { return false; }
    void DumpLedger() override {}
};

// hookmgr cannot install anything in this process -- there is no echovr.exe mapped -- so the arm
// is driven through voicelifetime's own test seam. What is under test is the module's CALL, its
// arguments and its ORDER, none of which a unit test of ArmVoiceLifetime can see.
struct ArmObservation {
    int creates = 0;
    int arms = 0;
    uint64_t pcVa = 0;
    size_t prologueLen = 0;
    bool prologueGiven = false;
    std::string factId;
    std::string configKey;
    // Taken at the moment the arm ran: AudioRate() is null until InitAudioRate publishes, and
    // every sink probe this module makes is behind that publication.
    bool beforeRateInit = false;
};
ArmObservation g_arm;

int64_t FakeTermOriginal(void*, void*) { return 0; }

HookStatus FakeHookCreate(const char* factId, uint64_t pcVa, const uint8_t* prologue,
                          size_t prologueLen, void*, void** trampoline, const char* configKey) {
    ++g_arm.creates;
    g_arm.pcVa = pcVa;
    g_arm.prologueLen = prologueLen;
    g_arm.prologueGiven = prologue != nullptr;
    g_arm.factId = factId != nullptr ? factId : "";
    g_arm.configKey = configKey != nullptr ? configKey : "";
    g_arm.beforeRateInit = replay::audio::AudioRate() == nullptr;
    *trampoline = reinterpret_cast<void*>(&FakeTermOriginal);
    return HookStatus::Created;
}

HookStatus FakeHookArm(const char*) {
    ++g_arm.arms;
    return HookStatus::Armed;
}

class DefaultConfig final : public IConfig {
public:
    bool GetBool(const char*, bool dflt) const override { return dflt; }
    int64_t GetInt(const char*, int64_t dflt) const override { return dflt; }
    double GetDouble(const char*, double dflt) const override { return dflt; }
    const char* GetString(const char*, const char* dflt) const override { return dflt; }
    bool Has(const char*) const override { return false; }
};

class NullTick final : public ITick {
public:
    bool RegisterEarly(TickFn, void*) override { return true; }
    bool RegisterLate(TickFn, void*) override { return true; }
    const char* Mode() const override { return "none"; }
};

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

    bool Available() const override { return true; }
    float MaxRate() const override { return 1.0f; }
    void SetRate(float r) override { setRates.push_back(r); }
    void Suspend(bool) override { ++suspends; }
    void Wakeup() override { ++wakeups; }
    double AudioSeconds() const override { return 0.0; }
    const char* Diagnostic() const override { return ""; }
    int64_t DisplayBiasSceneUs() const override { return 0; }
    const char* RateLabel() const override { return "label"; }
};

// modreg::InitAll is once per process, so every case shares one initialisation, exactly as
// tests/playback/test_playback_arm.cpp does.
struct Env {
    NullLog log;
    DefaultConfig config;
    NullTick tick;
    ReplayServices services{};
    FakeTransport transport;
    CountingRate rate;
    size_t failures = 0;
    ArmObservation arm;
    bool audioRegistered = false;
    IAudioRate* published = nullptr;

    Env() {
        // The tick's sink commands are gated on livesafety::Feature::AudioRate, and this binary
        // never installs the offline-bit lock, so the un-overridden verdict is LockNotHeld and
        // every command below would be inert. These cases are about the WIRING in the process the
        // wiring exists for: a contained offline replay. The inert half is livesafety's own suite.
        replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
        services.log = &log;
        services.config = &config;
        services.tick = &tick;
        audio::SetHookAccessForTest(&FakeHookCreate, &FakeHookArm);
        for (size_t i = 0; i < modreg::Count(); ++i) {
            if (strcmp(modreg::At(i)->name, "audio") == 0) {
                audioRegistered = true;
            }
        }
        modreg::InitAll(&services);
        arm = g_arm;
        audio::SetHookAccessForTest(nullptr, nullptr);
        failures = modreg::InitFailures();
        published = services.audioRate;
        // The substitution: the module's own binding cannot reach Wwise in this process, so the
        // tick's commands are observed on a stand-in placed where the module publishes its own.
        services.audioRate = &rate;
        services.transport = &transport;
    }

    void Ticks(int n) {
        TickInfo info{};
        info.frameDeltaUs = 11111;
        info.frameIndex = 1;
        for (int i = 0; i < n; ++i) {
            info.simTimeUs += info.frameDeltaUs;
            ++info.frameIndex;
            modreg::TickAll(info);
        }
    }
};

Env& TheEnv() {
    static Env env;
    return env;
}

}  // namespace

TEST_CASE("the audio module registered, initialised, and published an IAudioRate") {
    Env& e = TheEnv();
    REQUIRE(e.audioRegistered);
    CHECK(e.failures == 0);
    // Published even though Wwise is absent here: Available() carries the answer, not a null.
    CHECK(e.published != nullptr);
}

TEST_CASE("★ row 2.6 is armed by the shipping module -- once, before any sink is probed") {
    Env& e = TheEnv();
    REQUIRE(e.audioRegistered);

    // Once. Not zero (L3 #10: ArmVoiceLifetime had no non-test caller at all) and not per-frame.
    CHECK(e.arm.creates == 1);
    CHECK(e.arm.arms == 1);
    CHECK(e.arm.factId == "AUDIO.SINK_TERM");
    CHECK(e.arm.configKey == "audio.sink_term_hook");
    CHECK(e.arm.pcVa == replay::audio_facts::kSinkTermVa);
    CHECK(e.arm.prologueLen == replay::audio_facts::kSinkTermPrologueLen);
    CHECK(e.arm.prologueGiven);

    // The ordering, both ends: null when the arm ran, non-null after Init returned. Without the
    // second half the first is satisfied by a module that never publishes anything at all.
    CHECK(e.arm.beforeRateInit);
    CHECK(e.published != nullptr);

    CHECK(audio::VoiceLifetimeState() == audio::LifetimeArm::Armed);
    // The whole point of arming: the ledger stops answering could-not-ask. Unavailable is the
    // honest answer here -- Wwise is not in this process -- and it is a DIFFERENT answer.
    CHECK(audio::SinkAvailabilityState() != audio::SinkAvailability::CouldNotAsk);

    // The site is never transcribed into the expectation either.
    char expected[96];
    snprintf(expected, sizeof expected,
             "voice lifetime hook (AUDIO.SINK_TERM, ledger row 2.6): armed at 0x%llx",
             static_cast<unsigned long long>(replay::audio_facts::kSinkTermVa));
    bool armedLine = false;
    bool refusalLine = false;
    for (const std::string& s : e.log.text) {
        if (s == expected) {
            armedLine = true;
        }
        if (s.find("ledger row 2.6): REFUSED") != std::string::npos) {
            refusalLine = true;
        }
    }
    CHECK(armedLine);
    CHECK_FALSE(refusalLine);
}

TEST_CASE("★ gap 4: the tick reads the transport and drives the sink, once per change") {
    Env& e = TheEnv();
    e.rate.setRates.clear();
    e.rate.suspends = 0;

    e.transport.Play(0.5f);
    e.Ticks(4);
    REQUIRE(e.rate.setRates.size() == 1);
    CHECK(e.rate.setRates[0] == 0.5f);
    CHECK(e.rate.suspends == 0);

    e.transport.Pause();
    e.Ticks(4);
    CHECK(e.rate.suspends == 1);
    CHECK(e.rate.setRates.size() == 1);

    e.transport.Play(0.5f);
    e.Ticks(4);
    CHECK(e.rate.suspends == 1);
    REQUIRE(e.rate.setRates.size() == 2);
    CHECK(e.rate.setRates[1] == 0.5f);

    e.transport.Play(0.25f);
    e.Ticks(1);
    REQUIRE(e.rate.setRates.size() == 3);
    CHECK(e.rate.setRates[2] == 0.25f);
}

TEST_CASE("the transport is bound per tick: taking it away stops the commands, giving it back resumes") {
    Env& e = TheEnv();
    e.rate.setRates.clear();
    e.transport.Play(0.5f);
    e.Ticks(1);

    e.services.transport = nullptr;
    e.Ticks(3);
    // Handed back at 1.0x once when the transport vanished, then nothing.
    REQUIRE(e.rate.setRates.size() >= 1);
    CHECK(e.rate.setRates.back() == 1.0f);
    const size_t after = e.rate.setRates.size();
    e.Ticks(3);
    CHECK(e.rate.setRates.size() == after);

    e.services.transport = &e.transport;
    e.transport.Play(0.75f);
    e.Ticks(2);
    REQUIRE(e.rate.setRates.size() == after + 1);
    CHECK(e.rate.setRates.back() == 0.75f);
}

TEST_CASE("★ the tick reads the LIVE seek-mute seam: a held mute defers the command, the release lands it") {
    // audio_selftest proves the driver defers and that N-17a takes the hold. Only this arm can show
    // that the shipping tick asks the real seam rather than a constant: a module handing the driver
    // a hard-coded "not held" passes every unit test in the suite and fails here.
    Env& e = TheEnv();
    e.transport.Play(1.0f);
    e.Ticks(1);
    const size_t before = e.rate.setRates.size();

    audio::HoldAudioSeekMute(true);
    e.transport.Play(0.5f);
    e.Ticks(3);
    CHECK(e.rate.setRates.size() == before);
    CHECK(e.rate.wakeups == 0);

    audio::HoldAudioSeekMute(false);
    e.Ticks(1);
    REQUIRE(e.rate.setRates.size() == before + 1);
    CHECK(e.rate.setRates.back() == 0.5f);
}

TEST_CASE("★ the shipping tick retries the CSoundPlayer resolve that init alone never gets") {
    // L2-RUN-13: unresolved at module init -- the game had not created CSoundPlayer yet -- and
    // InitAudioState was a one-shot, so AudioState() was null for the whole session and every
    // keyframe of a four-minute capture carried an empty pin block. There is no CSoundPlayer in
    // this process either, so what is observable here is that the module keeps asking, and that it
    // does not report a success it did not get.
    Env& e = TheEnv();
    e.Ticks(1);
    const uint32_t before = audio::AudioStateArmAttempts();
    CHECK(before >= 1u);           // the tick asks; module init's one-shot is not what did this
    e.Ticks(120);                  // 11.111 ms a tick: 1.33 s of scene time, past one cadence
    const uint32_t after = audio::AudioStateArmAttempts();
    CHECK(after > before);
    CHECK(after - before <= 2u);   // retried on a cadence, not on every frame
    CHECK(audio::AudioState() == nullptr);
    CHECK(e.services.audioState == nullptr);
}

// ==============================================================================================
// The late CSoundPlayer resolve reaching the SEEK RESIDUE, not just the capture's pins
// ==============================================================================================

namespace {

// A CSoundPlayer with a state memo and nothing else, at the offsets the binary decodes to. The
// full walk is audio_selftest's subject; what this arm needs is a resolve that SUCCEEDS, and it
// must succeed later than the arm, which is the whole point.
struct WireSoundPlayer {
    static constexpr size_t kSize = 0x400;
    static constexpr size_t kMemoOff = audio_facts::kMemoCacheOff;
    static constexpr size_t kCountOff = audio_facts::kMemoCountOff;

    std::vector<uint8_t> block;
    std::vector<uint64_t> entries;

    WireSoundPlayer() : block(kSize, 0) {}
    uintptr_t Base() { return reinterpret_cast<uintptr_t>(block.data()); }

    void SetState(uint32_t group, uint32_t value) {
        entries.assign(1, static_cast<uint64_t>(group) | (static_cast<uint64_t>(value) << 32));
        const uintptr_t data = reinterpret_cast<uintptr_t>(entries.data());
        const uint64_t count = entries.size();
        memcpy(block.data() + kMemoOff, &data, sizeof data);
        memcpy(block.data() + kMemoOff + kCountOff, &count, sizeof count);
    }
};

WireSoundPlayer* g_lateSoundPlayer = nullptr;
uint32_t g_probeCalls = 0;

// Absent for the first two retries, then there -- the game brings Wwise up when it brings it up.
uintptr_t LateSoundPlayer() {
    ++g_probeCalls;
    return g_probeCalls >= 3u && g_lateSoundPlayer != nullptr ? g_lateSoundPlayer->Base() : 0;
}

playback::IResidue* RowById(playback::ResidueBattery& b, const char* id) {
    for (size_t i = 0; i < b.Count(); ++i) {
        if (strcmp(b.At(i)->Id(), id) == 0) {
            return b.At(i);
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("★ the seek residue binds to the audio state the LATE resolve produced") {
    // ENG-PINS-LIVE's open finding. The retry landed the late CSoundPlayer in services->audioState
    // and in the capture's keyframe pins, and the residue rows STILL held the null they were armed
    // with: N-16 stayed Unmapped, the battery skipped it, and a backwards seek never reconciled the
    // Wwise memo for the rest of the session -- with the log saying the resolve had succeeded.
    Env& e = TheEnv();
    audio::ResetAudioResidueForTest();
    audio::ResetAudioStateForTest();

    WireSoundPlayer sp;
    sp.SetState(0x11u, 0xAAu);
    g_lateSoundPlayer = &sp;
    g_probeCalls = 0;
    audio::SetSoundPlayerProbeForTest(&LateSoundPlayer);

    // ★ ARMED FIRST, exactly as the shipping order does it: playback arms when a demo loads, and
    // on every host so far that is before the game has created CSoundPlayer.
    playback::ResidueBattery battery;
    REQUIRE(audio::ArmAudioResidue(battery) == 4);
    REQUIRE(audio::AudioState() == nullptr);
    CHECK_FALSE(audio::AudioResidueStateBound());

    playback::IResidue* n16 = RowById(battery, "N-16");
    REQUIRE(n16 != nullptr);
    CHECK(n16->Avail() == playback::Availability::Unmapped);
    CHECK(strlen(n16->Diagnostic()) > 0);

    for (int i = 0; i < 600 && audio::AudioState() == nullptr; ++i) {
        e.Ticks(1);
    }
    REQUIRE(audio::AudioState() != nullptr);          // the retry took, late
    CHECK(e.services.audioState == audio::AudioState());

    // The rows armed before the resolve can see it, because they ask rather than remember.
    CHECK(audio::AudioResidueStateBound());
    CHECK(n16->Avail() == playback::Availability::Ready);
    CHECK(strcmp(n16->Diagnostic(), "") == 0);

    // ...and a keyframe's pins reach a live state through the battery. Fire() SKIPS a row that is
    // not Ready, so a captured null costs the reconcile twice over.
    audio::AudioPins pins;
    pins.seed = 0x1234u;
    pins.seedValid = true;
    audio::SetAudioPins(pins);

    const uint32_t reconcilesBefore = audio::AudioResidueStatsRef().reconciles;
    const uint32_t pinsBefore = audio::AudioResidueStatsRef().pinsRestored;
    playback::Discontinuity d;
    d.cause = playback::Cause::Seek;
    d.needs_preroll = true;
    battery.Fire(d);
    CHECK(audio::AudioResidueStatsRef().reconciles == reconcilesBefore + 1);
    CHECK(audio::AudioResidueStatsRef().pinsRestored == pinsBefore + 1);

    // The fake dies with this scope; nothing may keep pointing at it.
    audio::SetSoundPlayerProbeForTest(nullptr);
    g_lateSoundPlayer = nullptr;
    audio::ResetAudioStateForTest();
    audio::ResetAudioResidueForTest();
    e.services.audioState = nullptr;
}

TEST_CASE("* the pin ledger is stepped by the shipping tick, once per tick and not on a cadence") {
    // L2-RUN-14 F37: the pin set used to be gathered inside the keyframe sample and cost the game
    // thread up to 159 ms a call. It is now maintained a slice at a time on the module tick, and
    // only this arm can show the shipping module actually steps it -- a ledger nothing drives
    // publishes the empty set forever and every unit test of it still passes.
    Env& e = TheEnv();
    e.Ticks(1);
    const uint64_t before = audio::AudioPinLedger().ticks;
    e.Ticks(30);
    const uint64_t after = audio::AudioPinLedger().ticks;
    CHECK(after - before == 30u);

    // There is no CSoundPlayer in this process, so the honest published set is the empty one that
    // says so -- and the keyframe still gets a block.
    std::vector<uint8_t> buf(audio::MaxPinBytes());
    audio::PinSource why = audio::PinSource::Bound;
    const size_t n = audio::EncodeCurrentAudioPins(buf.data(), buf.size(), &why);
    CHECK(n != 0);
    CHECK(why == audio::PinSource::NoAudioState);
}

// ==============================================================================================
// L3-RUN-13 F4: with the sink gone, the shipped ledger line says unavailable -- never a mix.
//
// The label used to be read off `plan_`, which PlanRate builds one statement before SetRate calls
// Wakeup(), and Wakeup re-probes. A sink torn down behind a pause therefore printed
// `transport rate -1.00x: audio: 1.0x, as the game mixed it` while the same object's Diagnostic()
// said there was no Wwise XAudio2 sink. Driven through the shipping module -- modreg::InitAll
// published this binding, modreg::TickAll drives its RateDriver -- with only the Wwise exports and
// the sink probe substituted, because neither exists in this process.

namespace {

struct WireVoice {
    audio::XAudio2SourceVoice voice{};   // first: `self` is this address
    float ratio = 1.0f;
    uint64_t samples = 0;
};
WireVoice g_wireVoice;
bool g_wireSinkBuilt = true;
uint32_t g_wireSuspends = 0;
uint32_t g_wireWakeups = 0;

int32_t __stdcall WireSetRatio(void* self, float ratio, uint32_t) {
    reinterpret_cast<WireVoice*>(self)->ratio = ratio;
    return 0;
}
void __stdcall WireGetRatio(void* self, float* out) {
    *out = reinterpret_cast<WireVoice*>(self)->ratio;
}
void __stdcall WireGetState(void* self, audio::XAudio2VoiceState* out, uint32_t) {
    WireVoice* v = reinterpret_cast<WireVoice*>(self);
    v->samples += 512;
    out->buffersQueued = 4;
    out->samplesPlayed = v->samples;
}

audio::XAudio2SourceVoiceVtbl& WireVtbl() {
    static audio::XAudio2SourceVoiceVtbl vtbl = [] {
        audio::XAudio2SourceVoiceVtbl v{};
        v.SetFrequencyRatio = &WireSetRatio;
        v.GetFrequencyRatio = &WireGetRatio;
        v.GetState = &WireGetState;
        return v;
    }();
    return vtbl;
}

size_t WireProbe(const audio::AkExports&, audio::SinkChain* out) {
    *out = audio::SinkChain{};
    if (!g_wireSinkBuilt) {
        out->firstRefusal = audio::SinkVerdict::NoSinkList;
        return 0;
    }
    g_wireVoice.voice.vtbl = &WireVtbl();
    out->sinks[0].sink = 0x1000;
    out->sinks[0].voice = &g_wireVoice.voice;
    // The fields ProveSink stamps. A chain built below the builder that left the vtable or the
    // generation at zero would make every VoiceCall refuse the voice, and this case would then
    // pass by never reaching the availability it is about.
    out->sinks[0].vtbl = g_wireVoice.voice.vtbl;
    out->sinks[0].generation = audio::SinkGeneration(out->sinks[0].sink);
    out->sinks[0].inputChannels = 2;
    out->sinks[0].inputSampleRate = 48000;
    out->sinks[0].frequencyRatio = g_wireVoice.ratio;
    out->count = 1;
    return 1;
}

size_t WireApply(const audio::AkExports&, float ratio, int32_t* firstError, size_t* voicesGone) {
    if (firstError != nullptr) {
        *firstError = g_wireSinkBuilt ? 0 : -1;
    }
    if (voicesGone != nullptr) {
        *voicesGone = 0;
    }
    if (!g_wireSinkBuilt) {
        return 0;
    }
    g_wireVoice.ratio = ratio;
    return 1;
}

const std::string* FindLine(const std::vector<std::string>& text, const char* prefix) {
    for (size_t i = text.size(); i > 0; --i) {
        if (text[i - 1].compare(0, strlen(prefix), prefix) == 0) {
            return &text[i - 1];
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("* F4: a sink lost behind a pause makes the shipped ledger say unavailable, not a mix") {
    Env& e = TheEnv();
    REQUIRE(e.published != nullptr);
    // The object under test is the one modreg::InitAll published, not a private instance.
    REQUIRE(audio::AudioRate() == e.published);

    IAudioRate* const stand_in = e.services.audioRate;
    e.services.audioRate = e.published;

    g_wireSinkBuilt = true;
    g_wireVoice = WireVoice{};
    audio::SetSinkAccessForTest(&WireProbe, &WireApply);
    audio::AkExports ak{};
    ak.GetAudioSettings = [](auto* s) -> int32_t {
        s->samplesPerFrame = 512;
        s->samplesPerSecond = 48000;
        return audio::kAkSuccess;
    };
    ak.Suspend = [](bool) -> int32_t {
        ++g_wireSuspends;
        return audio::kAkSuccess;
    };
    ak.WakeupFromSuspend = []() -> int32_t {
        ++g_wireWakeups;
        return audio::kAkSuccess;
    };
    audio::AudioRateConfig cfg;
    cfg.closedLoop = false;   // the PI lock has its own cases; this one is about the label
    REQUIRE(audio::InitAudioRate(ak, cfg));
    REQUIRE(e.published->Available());

    e.transport.Play(1.0f);
    e.Ticks(2);
    e.transport.Pause();
    e.Ticks(2);
    REQUIRE(g_wireSuspends == 1);
    // Wwise tears the sink down behind the suspension. The re-arm deliberately leaves a suspended
    // engine alone, so `available_` is still true when the next rate is planned -- which is the
    // whole shape of the defect.
    g_wireSinkBuilt = false;
    REQUIRE(e.published->Available());

    // OWNER-WATCH-FIX: reverse now MUTES rather than waking the sink to play a forward mix, so the
    // rate that takes this path back out of the suspension is a forward one. The defect shape is
    // unchanged: the wakeup re-probes, finds the sink Wwise tore down, and must say so.
    e.log.text.clear();
    e.transport.Play(0.5f);
    e.Ticks(2);

    const std::string* line = FindLine(e.log.text, "transport rate 0.50x: ");
    REQUIRE(line != nullptr);
    REQUIRE(g_wireWakeups == 1);
    CHECK_FALSE(e.published->Available());

    // AUD-FINAL ruling (a): the word is `unavailable`, and the cause is the sink verdict's own
    // sentence rather than a second copy of it written down here.
    CHECK(line->find("audio rate control unavailable") != std::string::npos);
    CHECK(line->find(audio::SinkVerdictName(audio::SinkVerdict::NoSinkList)) != std::string::npos);
    CHECK(line->find("as the game mixed it") == std::string::npos);
    CHECK(line->find("slowed with picture") == std::string::npos);

    // L3-RUN-24 clause (5): the line named the refusal and said nothing about the direction, so a
    // reversed transport under a dead sink read exactly like a forward one. The word is looked for
    // in the LABEL, past the `-1.00x` the driver prints, and it is a literal rather than
    // RateLabel's own output -- an expectation read from the arm under test cannot fail with it.
    // Now reverse, on the sink that is already known dead: PlanRate answers unavailable BEFORE it
    // answers direction, so this is the one arm where a reversed label still reaches the panel.
    e.log.text.clear();
    e.transport.Play(-1.0f);
    e.Ticks(2);
    const std::string* rev = FindLine(e.log.text, "transport rate -1.00x: ");
    REQUIRE(rev != nullptr);
    const std::string reverseLabel = rev->substr(strlen("transport rate -1.00x: "));
    CHECK(reverseLabel.find("reverse") != std::string::npos);
    CHECK(reverseLabel.find("audio rate control unavailable") != std::string::npos);

    // The control that stops a blanket prefix from passing as an answer: same dead sink, forward
    // transport, the refusal sentence still there and the direction word gone.
    e.log.text.clear();
    e.transport.Play(1.0f);
    e.Ticks(2);
    const std::string* forward = FindLine(e.log.text, "transport rate 1.00x: ");
    REQUIRE(forward != nullptr);
    CHECK_FALSE(e.published->Available());
    const std::string forwardLabel = forward->substr(strlen("transport rate 1.00x: "));
    CHECK(forwardLabel.find("audio rate control unavailable") != std::string::npos);
    CHECK(forwardLabel.find("reverse") == std::string::npos);

    audio::ResetAudioRateForTest();
    audio::SetSinkAccessForTest(nullptr, nullptr);
    e.services.audioRate = stand_in;
}

// L3-RUN-14 F4: the same ledger line with the sink PRESENT. Both runs logged
// `transport rate -1.00x: audio: 1.0x, as the game mixed it`, and the two clamp lines a second
// earlier prove the sink was live at the time -- RateDriver only emits those when Available() is
// true. So the remaining half of F4 is not an availability question at all: PlanRate rides |r|,
// the sink plays the mix FORWARD, and the label claimed the game had mixed it that way.

TEST_CASE("* F4: reverse with a live sink says the mix runs forward, never that the game mixed it") {
    Env& e = TheEnv();
    REQUIRE(e.published != nullptr);

    IAudioRate* const stand_in = e.services.audioRate;
    e.services.audioRate = e.published;

    g_wireSinkBuilt = true;
    g_wireVoice = WireVoice{};
    audio::SetSinkAccessForTest(&WireProbe, &WireApply);
    audio::AkExports ak{};
    ak.GetAudioSettings = [](auto* s) -> int32_t {
        s->samplesPerFrame = 512;
        s->samplesPerSecond = 48000;
        return audio::kAkSuccess;
    };
    ak.Suspend = [](bool) -> int32_t {
        ++g_wireSuspends;
        return audio::kAkSuccess;
    };
    ak.WakeupFromSuspend = []() -> int32_t {
        ++g_wireWakeups;
        return audio::kAkSuccess;
    };
    audio::AudioRateConfig cfg;
    cfg.closedLoop = false;
    REQUIRE(audio::InitAudioRate(ak, cfg));
    // Re-published into the same singleton the case above reset: the object under test is still
    // the one modreg::InitAll gave the services table, not a private instance.
    REQUIRE(audio::AudioRate() == e.published);
    REQUIRE(e.published->Available());

    e.transport.Play(1.0f);
    e.Ticks(2);
    e.log.text.clear();
    e.transport.Play(-1.0f);
    e.Ticks(2);

    const std::string* reverse = FindLine(e.log.text, "transport rate -1.00x: ");
    REQUIRE(reverse != nullptr);
    // The state the defect needs. If the sink had gone the line would read `unavailable` for the
    // reason the case above pins, and this one would pass without ever reaching the reverse arm.
    REQUIRE(e.published->Available());

    // OWNER-WATCH-FIX: the answer to "a mix nobody made" is not a better sentence about it, it is
    // not playing it. The literal is written out here rather than read from RateLabel, so an
    // expectation taken from the arm under test cannot fail with it.
    CHECK(reverse->find("audio: muted while the picture runs backward") != std::string::npos);
    CHECK(reverse->find("as the game mixed it") == std::string::npos);

    // The control for that negative: the same fixture, one rate later, must still be able to print
    // the sentence. Without it a broken FindLine or a renamed string would pass the CHECK above.
    e.log.text.clear();
    e.transport.Play(1.0f);
    e.Ticks(2);
    const std::string* forward = FindLine(e.log.text, "transport rate 1.00x: ");
    REQUIRE(forward != nullptr);
    CHECK(forward->find("as the game mixed it") != std::string::npos);

    audio::ResetAudioRateForTest();
    audio::SetSinkAccessForTest(nullptr, nullptr);
    e.services.audioRate = stand_in;
}
