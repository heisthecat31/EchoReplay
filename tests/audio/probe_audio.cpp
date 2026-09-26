// W2-H's in-game probe.
//
// ==============================================================================================
// WHAT IT ESTABLISHES, AND IT IS THE HALF NO OFFLINE CHECK CAN REACH
// ==============================================================================================
//
//  1. THE S10.4.2 SINK PROOFS, in vivo. AUDIO.SINK_PTR and AUDIO.SINK_VOICE_OFF are the only two
//     non-exported numbers in the whole audio design, and every dependent feature currently prints
//     "depends on unverified fact AUDIO.SINK_VOICE_OFF". This runs both proofs against real memory:
//     sink+0x50 or +0x58 equals the exported GetWwiseXAudio2Interface(), and sink+0x68 reports a
//     sane GetVoiceDetails and a GetFrequencyRatio of exactly 1.0f.
//
//  2. TIER A: does SetFrequencyRatio actually move the audio clock. W1-G found the sink is fed on
//     every pass whether or not anything is audible, so this is measurable in the menu. Two
//     independent readings, in two clock domains -- GetBufferTick and the voice's own SamplesPlayed
//     -- because one cannot tell "the ratio did not take" from "the machine was starved".
//     ★ It emits TIER-A-CONFIRMED and never O4-PASS. S12.4.1 makes that separation mechanical, and
//     it is written here rather than in the report so it cannot be applied retroactively.
//
//  3. THE MEASURED A/V OFFSET. The 85 ms figure is 8 x 512 / 48000 at one device's settings. This
//     reports the runtime samplesPerFrame, samplesPerSecond and queue depth, so S10.4.1a's constant
//     becomes a reading.
//
//  4. THE STATE-MODEL CENSUS, which is what the pin block's field list rests on. How many state
//     groups the memo holds, how many emitters exist, how many per-emitter switches, how many
//     voices are sounding and on how many objects. ★ EVERY ONE IS REPORTED AS A NUMBER, not as a
//     boolean: "the memo has 14 groups" and "the memo could not be read" are different answers, and
//     a pass/fail would collapse them.
//
// ==============================================================================================
// WHAT IT DOES NOT ESTABLISH
// ==============================================================================================
//
// Nothing about whether a reconcile fixes real music, whether a re-armed loop sounds right, or
// whether replayed VOIP is audible. Those need a loaded replay, which needs W2-B and W3-C. This
// probe asserts nothing that depends on a replay being in progress, so its result cannot be an
// accident of timing.
//
// ★ IT NEVER LEAVES THE RATIO CHANGED. The Tier-A measurement restores 1.0f in the same function
// that set it, on every exit path including the failure ones. A probe that left somebody's game
// pitched down would be a worse outcome than not measuring at all.
//
// ==============================================================================================
// THE LAUNCH PROFILE, AND WHY THE OFFLINE-BIT LOCK IS THIS PROBE'S BUSINESS
// ==============================================================================================
//
// Wwise comes up during boot and the census needs a loaded scene, so this cannot be an attach-time
// probe -- and a launch that outlives the attach window reaches login. The only profile that may do
// that is offline-local, whose four loopback hosts and in-client match host are the whole reason it
// is allowed to. That profile's safety rests on ONE bit, which a menu script node can clear, so the
// harness refuses it unless a probe declares the offline-bit-lock guard.
//
// So this probe installs it -- bootstrap::Attach reaches offlinelock::Apply() -- and then REPORTS
// whether it took, and fails when it did not. A declaration the harness cannot verify is a promise;
// a declaration the probe checks is a fact.

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "checkledger.h"

#include "audio/akbind.h"
#include "audio/audio_facts.h"
#include "audio/audiostate_init.h"
#include "audio/memprobe.h"
#include "audio/ratepolicy.h"
#include "audio/sinkvoice.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/offlinelock.h"
#include "engine/audiostate.h"
#include "generated/facts_generated.h"

namespace {

using namespace replay;
using namespace replay::audio;
using namespace replay::audio::ledger;

char g_detail[6144];
bool g_wwiseSeen = false;
bool g_couldNotAsk = false;
volatile LONG g_bootstrapOk = 0;

// The ledger lives in checkledger.h so that audio_selftest can replay a recorded run through the
// same rules this probe scores by. Its header explains why a counter was not enough.
ledger::Ledger g_ledger;

void Note(const char* text) { strncat_s(g_detail, sizeof g_detail, text, _TRUNCATE); }

void Notef(const char* fmt, ...) {
    char one[900];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(one, sizeof one, _TRUNCATE, fmt, args);
    va_end(args);
    Note(one);
}

void Check(ledger::CheckId id, bool ok, const char* what) {
    g_ledger.Check(id, ok);
    if (!ok) {
        Notef(" | FAILED: %s", what);
    }
}

void Skip(ledger::CheckId id, const char* reason) { g_ledger.Skip(id, reason); }

// ---- the measurement --------------------------------------------------------------------------

double NowSeconds() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

struct Reading {
    double wall = 0.0;
    uint32_t tick = 0;
    uint64_t samples = 0;
    uint32_t queued = 0;
    bool ok = false;
};

Reading Sample(const AkExports& ak) {
    Reading r;
    if (ak.GetBufferTick == nullptr) {
        return r;
    }
    r.wall = NowSeconds();
    r.tick = ak.GetBufferTick();
    r.ok = ReadSamplesPlayed(ak, &r.samples, &r.queued);
    return r;
}

// Measures the frames-per-second and samples-per-second the sink actually produced over `seconds`,
// at whatever ratio is currently commanded. The caller does the ratio; this only reads.
struct Rates {
    double wwiseFps = 0.0;
    double deviceSps = 0.0;
    bool ok = false;
};

Rates MeasureFor(const AkExports& ak, double seconds) {
    const Reading a = Sample(ak);
    if (!a.ok) {
        return Rates{};
    }
    Sleep(static_cast<DWORD>(seconds * 1000.0));
    const Reading b = Sample(ak);
    if (!b.ok) {
        return Rates{};
    }
    const double dt = b.wall - a.wall;
    if (dt <= 0.0) {
        return Rates{};
    }
    Rates out;
    out.wwiseFps = static_cast<double>(b.tick - a.tick) / dt;
    out.deviceSps = static_cast<double>(b.samples - a.samples) / dt;
    out.ok = true;
    return out;
}

void RunTierA(const AkExports& ak, const AkAudioSettings& settings) {
    const CheckId rungs[] = {kCheckRate050, kCheckRate025, kCheckRate010};

    const Rates base = MeasureFor(ak, 2.0);
    Check(kCheckRateBaseline, base.ok, "the 1.0x baseline could be measured from both clock domains");
    if (!base.ok) {
        for (CheckId id : rungs) {
            Skip(id, "the 1.0x baseline could not be measured, and a rung is a ratio against it");
        }
        return;
    }
    Notef(" | baseline at 1.0x: wwise %.2f frames/s, device %.0f samples/s (nominal %.2f, %u)",
          base.wwiseFps, base.deviceSps,
          static_cast<double>(settings.samplesPerSecond) / settings.samplesPerFrame,
          settings.samplesPerSecond);

    const float rates[] = {0.5f, 0.25f, 0.1f};
    for (size_t i = 0; i < 3; ++i) {
        const float r = rates[i];
        int32_t firstError = 0;
        const size_t applied = ApplyFrequencyRatio(ak, r, &firstError);
        if (applied == 0) {
            Notef(" | %.2fx: SetFrequencyRatio was refused by every sink (hr 0x%08X)",
                  static_cast<double>(r), static_cast<unsigned>(firstError));
            g_ledger.Check(rungs[i], false);
            continue;
        }
        const Rates at = MeasureFor(ak, 2.0);
        // Restore before judging, so a failing branch cannot skip it.
        ApplyFrequencyRatio(ak, 1.0f, nullptr);

        RateSample sample;
        sample.settingsValid = true;
        if (at.ok && base.wwiseFps > 0.0 && base.deviceSps > 0.0) {
            sample.wwiseRatio = at.wwiseFps / base.wwiseFps;
            sample.deviceRatio = at.deviceSps / base.deviceSps;
        }
        const RateVerdict verdict = JudgeRate(sample, r, 0.05);
        Notef(" | %.2fx: wwise %.3f, device %.3f -> %s", static_cast<double>(r), sample.wwiseRatio,
              sample.deviceRatio, RateVerdictName(verdict));
        g_ledger.Check(rungs[i], verdict != RateVerdict::FailMechanism);
    }
    // Belt and braces. Nobody's game is left pitched down by a measurement.
    ApplyFrequencyRatio(ak, 1.0f, nullptr);
}

void RunCensus(const AkExports& ak) {
    if (!InitAudioState(ak)) {
        Note(" | CSoundPlayer could not be resolved through SOUND.SYSTEMS_PTR_VA + "
             "SOUND.PLAYER_SLOT_OFF: the state census was NOT taken (this is also what -noaudio "
             "looks like, so it is reported rather than failed)");
        return;
    }
    IAudioState* state = AudioState();
    if (state == nullptr) {
        return;
    }

    AudioStatePair states[128];
    uint32_t stateTotal = 0;
    const size_t nStates = state->SnapshotStates(states, 128, &stateTotal);
    Notef(" | state memo: %u group(s) total, %zu read", stateTotal, nStates);
    for (size_t i = 0; i < nStates && i < 24; ++i) {
        Notef("%s0x%08X=0x%08X", i == 0 ? " [" : " ", states[i].group, states[i].value);
    }
    if (nStates != 0) {
        Note("]");
    }

    AudioSwitchTriple switches[512];
    uint32_t switchTotal = 0;
    const size_t nSwitches = state->SnapshotSwitches(switches, 512, &switchTotal);
    Notef(" | per-emitter switches: %u total, %zu read", switchTotal, nSwitches);

    // Two samples a second apart, so the loop tracker has something to measure. A single sample
    // cannot distinguish a loop from a one-shot, which is the whole reason sustainedMs exists.
    AudioTrackVoices(0);
    Sleep(1000);
    AudioTrackVoices(1000);
    AudioLoopPin loops[256];
    uint32_t loopTotal = 0;
    const size_t nLoops = state->SnapshotLoops(loops, 256, &loopTotal);
    Notef(" | sounding voices after 1 s: %u total, %zu read", loopTotal, nLoops);
    for (size_t i = 0; i < nLoops && i < 16; ++i) {
        Notef("%sobj=%u evt=0x%08X pos=%dms sustained=%ums", i == 0 ? " [" : " ", loops[i].gameObj,
              loops[i].eventId, loops[i].positionMs, loops[i].sustainedMs);
    }
    if (nLoops != 0) {
        Note("]");
    }

    uint32_t seed = 0;
    Blob history{};
    const bool pins = state->ReadRandomPins(seed, history);
    Notef(" | container history: %u byte(s), seed readback %s", history.n,
          pins ? "valid" : "write-only in this build (expected)");
}

// SOUND.DISABLE_FLAG, read raw and reported raw. Nonzero and CSoundPlayer::SetState and ::SetSwitch
// both return SUCCESS without calling Wwise, so a reconcile against it corrects nothing and reports
// that every write took -- a check that passes vacuously. Reading it here is what makes "the
// reconcile did nothing" attributable to the flag rather than to the reconcile.
//
// Read directly rather than through ReconcileReport, because getting it from there would mean
// calling Reconcile(), and a census probe that writes engine state is measuring itself.
void ReportSoundDisableFlag() {
    const void* p = buildguard::MappedAddress(audio_facts::kSoundDisableFlagVa);
    uint32_t value = 0;
    if (p == nullptr) {
        Note(" | SOUND.DISABLE_FLAG: NOT READ, the preferred-base VA could not be rebased against "
             "the running module");
        return;
    }
    if (!ReadAt(reinterpret_cast<uintptr_t>(p), &value)) {
        Notef(" | SOUND.DISABLE_FLAG: NOT READ, %p is not committed readable memory", p);
        return;
    }
    Notef(" | SOUND.DISABLE_FLAG at %p: raw 0x%08X (%s)", p, value,
          value == 0 ? "clear, so the engine setters reach Wwise"
                     : "SET, so every SetState/SetSwitch would return SUCCESS without calling Wwise");
}

// ★ THE GUARD THIS DESCRIPTOR DECLARES, CHECKED RATHER THAN PROMISED.
//
// The descriptor declares guards: ["offline-bit-lock"], and the harness treats that declaration as a
// precondition it has no way to verify -- it refuses a match-loading launch without one and then
// trusts it. So the probe that made the claim reports whether the claim came true, and FAILS when it
// did not: a launch that ran on -offline without the lock is a launch whose network safety property
// was a configuration rather than an invariant, and that is a finding about the sweep even when every
// audio number in it is correct.
//
// Locked and AlreadyLockedInProcess are BOTH held: the second is what a module that lost the install
// race reports about a process that is locked by someone else. Everything else is not held.
void ReportOfflineLock() {
    // Apply() runs on the bootstrap worker, after the build guard, so it is not resolved the instant
    // DllMain returns. Bounded, and only while the answer can still change: with no worker there is
    // nothing to wait for and the wait would be 30 s of nothing.
    const double deadline = NowSeconds() + 30.0;
    while (InterlockedCompareExchange(&g_bootstrapOk, 0, 0) != 0 &&
           offlinelock::Current().state == offlinelock::LockState::NotAttempted &&
           NowSeconds() < deadline) {
        Sleep(250);
    }

    const offlinelock::Report& lock = offlinelock::Current();
    const bool held = lock.state == offlinelock::LockState::Locked ||
                      lock.state == offlinelock::LockState::AlreadyLockedInProcess;
    Notef(" | offline-bit lock: %s (bootstrap::Attach %s, installed here %s, sites %u of %u, "
          "calls %llu, clears prevented %llu, exercise %s)",
          offlinelock::LockStateName(lock.state),
          InterlockedCompareExchange(&g_bootstrapOk, 0, 0) != 0 ? "accepted" : "REFUSED",
          lock.installedHere ? "yes" : "no", lock.sitesRecorded, lock.sitesExpected,
          static_cast<unsigned long long>(lock.calls),
          static_cast<unsigned long long>(lock.clearsPrevented),
          offlinelock::ExerciseName(offlinelock::Exercised()));
    Check(kCheckOfflineLock, held,
          "the offline-bit lock this descriptor declares is actually held in this process");
}

void WriteVerdict(HMODULE self);
void WriteProvisional(HMODULE self);

// ★ THE PROBE TIMES ITSELF. Every figure below is measured from Worker's own first tick -- DllMain
// creates this thread and returns, so that is within a millisecond of load -- and none of it comes
// from the harness, which times the process rather than the observation. An instrument that cannot
// report its own window cannot tell "the sink never appeared" from "I stopped watching".
double g_workerStart = 0.0;
double g_wwiseUpMs = -1.0;
double g_sinkWaitMs = 0.0;
double g_selectorFirstMs = -1.0;
double g_sinkListFirstMs = -1.0;
uint32_t g_sinkPolls = 0;
bool g_matchSeen = false;
double g_matchSeenMs = -1.0;

double SinceStartMs() { return (NowSeconds() - g_workerStart) * 1000.0; }

// The declared completion point, read the way probe_clock and W1-E's in-match probe read it:
// CR15Game with a live CR15NetGame is what "a match" means to every probe in this sweep, so the
// fact is shared rather than each probe inventing one. Two pointer reads, both guarded.
bool MatchLive() {
    const void* slot = buildguard::MappedAddress(facts::GAME_SINGLETON_VA);
    uint64_t game = 0;
    if (slot == nullptr || !ReadAt(reinterpret_cast<uintptr_t>(slot), &game) || game == 0) {
        return false;
    }
    uint64_t netGame = 0;
    return ReadAt(static_cast<uintptr_t>(game) + facts::GAME_NETGAME_PTR_OFF, &netGame) &&
           netGame != 0;
}

// Samples at least once even after the deadline, so a launch that spent its budget on the sink still
// records whether a match was up when it filed.
void WaitForMatch(HMODULE self, double deadline) {
    for (;;) {
        if (MatchLive()) {
            g_matchSeen = true;
            g_matchSeenMs = SinceStartMs();
            return;
        }
        if (NowSeconds() >= deadline) {
            return;
        }
        WriteProvisional(self);
        Sleep(250);
    }
}

// "never" and "0 ms" are different answers and -1 is not a time.
const char* MsText(double ms, char* buf, size_t n) {
    if (ms < 0.0) {
        strncpy_s(buf, n, "never", _TRUNCATE);
    } else {
        _snprintf_s(buf, n, _TRUNCATE, "%.0f ms", ms);
    }
    return buf;
}

DWORD WINAPI Worker(LPVOID param) {
    HMODULE self = static_cast<HMODULE>(param);
    g_workerStart = NowSeconds();

    AkExports ak{};
    BindReport bind;
    const bool allBound = Bind(&ak, &bind);
    Notef("AK exports: %u of %u resolved by name", bind.resolved, bind.requested);
    Check(kCheckExportsBound, allBound,
          "every decorated AK name akbind.cpp asks for resolved in the mapped image");

    // ★ THREE WAITS, ONE DEADLINE, AND THE THIRD IS THE ONE THE DESCRIPTOR PROMISED.
    //
    // GetAudioSettings answers as soon as AK::SoundEngine::Init has built an audio manager, and what
    // it returns is the ENGINE's frame and rate settings, which exist with no output device at all:
    // the 512 / 48000 a run reports before the device is up are Wwise's own defaults. Every
    // measurement below needs CAkSinkXAudio2, which is constructed later and is what writes both
    // AUDIO.XAUDIO2_VER_SELECTOR and AUDIO.SINK_LIST_PTR -- so asking once, the instant the settings
    // answer, measures this probe's timing and not the engine's sink.
    //
    // The settings wait therefore keeps only its true meaning ("the sound engine is initialised"),
    // a second wait polls ProbeSinks to the SAME deadline, and a third waits for the live
    // CR15NetGame that "completesAt: match" declares -- because the final record ends the launch
    // (the harness kills on probes-complete), and a probe that files before the point it declared
    // cuts its own launch short and measures the menu. The declared budget is unchanged: 120 s of
    // waiting in total, however it divides between the three.
    AkAudioSettings settings{};
    const double deadline = g_workerStart + 120.0;
    while (NowSeconds() < deadline) {
        if (ak.GetAudioSettings != nullptr && ak.GetAudioSettings(&settings) == kAkSuccess &&
            settings.samplesPerFrame != 0 && settings.samplesPerSecond != 0) {
            g_wwiseSeen = true;
            g_wwiseUpMs = SinceStartMs();
            break;
        }
        WriteProvisional(self);
        Sleep(250);
    }
    if (!g_wwiseSeen) {
        g_couldNotAsk = true;
        Note(" | the sound engine never reported initialised inside 120 s: NOTHING below was "
             "measured");
        ledger::SkipSinkDependents(g_ledger,
                                   "the sound engine never reported initialised, so no sink "
                                   "could exist to ask");
        // Still reported. The lock is a property of the launch, not of the audio engine, and a
        // launch that ran unprotected is the same finding whether or not Wwise came up.
        WaitForMatch(self, deadline);
        ReportOfflineLock();
        WriteVerdict(self);
        return 0;
    }
    Notef(" | sound engine initialised at %.0f ms: GetAudioSettings reports %u samples/frame, "
          "%u samples/s (engine settings, which exist before any output device)",
          g_wwiseUpMs, settings.samplesPerFrame, settings.samplesPerSecond);

    SinkChain chain;
    size_t proven = 0;
    for (;;) {
        proven = ProbeSinks(ak, &chain);
        ++g_sinkPolls;
        g_sinkWaitMs = SinceStartMs();
        if (chain.versionSelector != 0 && g_selectorFirstMs < 0.0) {
            g_selectorFirstMs = g_sinkWaitMs;
        }
        if (chain.sinkListHead != 0 && g_sinkListFirstMs < 0.0) {
            g_sinkListFirstMs = g_sinkWaitMs;
        }
        // A positive refusal deliberately does NOT end the wait: NoVoice and IfaceMismatch are both
        // states a chain passes through while a device is being built, so only a refusal that
        // survives to the deadline is evidence about the design.
        if (proven > 0 || NowSeconds() >= deadline) {
            break;
        }
        WriteProvisional(self);
        Sleep(250);
    }

    WaitForMatch(self, deadline);

    const SinkWaitOutcome outcome = ClassifySinkWait(proven, chain.firstRefusal);
    char selectorAt[32];
    char listAt[32];
    Notef(" | sinks: %zu proven, %zu rejected after %u poll(s) ending at %.0f ms -> %s (last "
          "refusal %s)",
          proven, chain.rejected, g_sinkPolls, g_sinkWaitMs, SinkWaitOutcomeName(outcome),
          SinkVerdictName(chain.firstRefusal));
    Notef(" | SINK_LIST_PTR head 0x%llX, first non-null at %s; XAUDIO2_VER_SELECTOR %u, first "
          "non-zero at %s",
          static_cast<unsigned long long>(chain.sinkListHead), MsText(g_sinkListFirstMs, listAt,
          sizeof listAt), chain.versionSelector, MsText(g_selectorFirstMs, selectorAt,
          sizeof selectorAt));
    if (g_matchSeen) {
        Notef(" | match: live CR15NetGame at %.0f ms, so what follows is the match-time reading the "
              "descriptor declares",
              g_matchSeenMs);
    } else {
        Note(" | match: NO live CR15NetGame inside the deadline. The declared completion point was "
             "not reached, so what follows was measured wherever the game was, not at a match, and "
             "the verdict says so");
    }

    // A6, inherited from W1-G and reported as three states rather than two. The selector decides how
    // GetVoiceDetails is laid out, so a selector that disagrees with the mapped runtime reads the
    // channel count out of the ActiveFlags slot -- plausible numbers, wrong ones. NoneMapped fails
    // this arm too: "no XAudio2 is mapped under its own name" is not a confirmation.
    //
    // ★ But NotChecked is not a fourth kind of disagreement, it is the absence of the question, and
    // scoring it as a failure counted UnknownXAudio2Version twice -- once under its own name and
    // once here. And a mask that was never populated is not an empty mask: printing "[]" for a
    // loader nobody asked reads as the measurement "no XAudio2 runtime is mapped".
    if (!chain.mappedQueried) {
        Notef(" | XAUDIO2_VER_SELECTOR %u vs mapped runtimes: NOT QUERIED, the probe refused at %s "
              "before it reached the loader",
              chain.versionSelector, SinkVerdictName(chain.firstRefusal));
    } else {
        Notef(" | XAUDIO2_VER_SELECTOR %u vs mapped runtimes [%s%s%s]: %s", chain.versionSelector,
              (chain.mappedXAudio2 & kXAudio2Mapped27) ? "XAudio2_7 " : "",
              (chain.mappedXAudio2 & kXAudio2Mapped28) ? "XAudio2_8 " : "",
              (chain.mappedXAudio2 & kXAudio2Mapped29)
                  ? "XAudio2_9"
                  : (chain.mappedXAudio2 == 0 ? "none of the three under its own name" : ""),
              XAudio2ModuleCheckName(chain.moduleCheck));
    }
    if (chain.moduleCheck == XAudio2ModuleCheck::NotChecked) {
        Skip(kCheckModuleAgrees,
             "the selector was out of range, so the loader was never held against it: "
             "UnknownXAudio2Version owns that verdict");
    } else {
        Check(kCheckModuleAgrees, chain.moduleCheck == XAudio2ModuleCheck::Agrees,
              "AUDIO.XAUDIO2_VER_SELECTOR names the XAudio2 runtime this process actually mapped");
    }

    // ★ THREE OUTCOMES, WHERE THERE WAS ONE CHECK. A sink that was never constructed has not failed
    // a sink proof, and a run that reports it as one spends its evidence arguing with itself.
    switch (outcome) {
        case SinkWaitOutcome::Proven:
            Check(kCheckSinkProofs, true,
                  "at least one sink passed both S10.4.2 proofs against real memory");
            break;
        case SinkWaitOutcome::Refused:
            Check(kCheckSinkProofs, false,
                  "a sink object exists and no entry passed both S10.4.2 proofs against real "
                  "memory");
            break;
        case SinkWaitOutcome::NeverAppeared:
            g_couldNotAsk = true;
            ledger::SkipSinkDependents(
                g_ledger,
                "no sink object appeared inside the deadline, so there was nothing to prove: "
                "CAkSinkXAudio2::Init writes both the selector and the list head, and both were "
                "still at their .data zero-init value");
            break;
    }

    if (outcome == SinkWaitOutcome::Proven) {
        // The measured A/V offset, which is what S10.4.1a's constant actually is on this device.
        AvBias bias;
        for (uint32_t i = 0; i < AvBias::kWarmupSamples; ++i) {
            uint64_t samples = 0;
            uint32_t queued = 0;
            if (ReadSamplesPlayed(ak, &samples, &queued)) {
                bias.Observe(queued, settings.samplesPerFrame, settings.samplesPerSecond);
            }
            Sleep(20);
        }
        Notef(" | measured A/V offset: %lld us of SCENE time (%s); at 0.1x that is %lld us of WALL "
              "time",
              static_cast<long long>(bias.OffsetSceneUs()),
              bias.Latched() ? "latched" : "NOT latched, so no bias would be applied",
              static_cast<long long>(bias.OffsetSceneUs()) * 10);

        Notef(" | voice 0: %u channel(s) at %u Hz, ratio %.4f", chain.sinks[0].inputChannels,
              chain.sinks[0].inputSampleRate,
              static_cast<double>(chain.sinks[0].frequencyRatio));

        RunTierA(ak, settings);
    } else {
        const CheckId ladder[] = {kCheckRateBaseline, kCheckRate050, kCheckRate025, kCheckRate010};
        for (CheckId id : ladder) {
            Skip(id, "the rate ladder commands a proven sink, and none was proven");
        }
    }

    RunCensus(ak);
    ReportSoundDisableFlag();
    ReportOfflineLock();

    Note(" | ESTABLISHES: the two S10.4.2 proofs against real memory, that the sink responds to "
         "SetFrequencyRatio in both clock domains (TIER-A-CONFIRMED, NOT O4-PASS -- S12.4.1), the "
         "device's actual buffering in scene time, and a census of the live audio state the pin "
         "block is designed around. DOES NOT ESTABLISH: that a reconcile fixes real music, that a "
         "re-armed loop sounds right, or anything about replayed VOIP -- all three need a loaded "
         "replay and wait for W2-B and W3-C.");
    WriteVerdict(self);
    return 0;
}

// The artifact's directory and the id the sweep gave this probe. Both records need them, and
// neither is written outside a sweep: no REPLAY_PROBE_ROOT means nothing goes into somebody's
// install.
bool ProbeArtifact(HMODULE self, char* dir, size_t dirN, char* probeId, size_t idN) {
    char root[MAX_PATH] = {0};
    const DWORD rootLen = GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);
    if (rootLen == 0 || rootLen >= sizeof root) {
        return false;
    }

    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(self, dll, sizeof dll);
    const char* leaf = strrchr(dll, '\\');
    leaf = leaf != nullptr ? leaf + 1 : dll;
    char stem[MAX_PATH] = {0};
    strncpy_s(stem, sizeof stem, leaf, _TRUNCATE);
    if (char* dot = strrchr(stem, '.')) {
        *dot = '\0';
    }

    _snprintf_s(dir, dirN, _TRUNCATE, "%s\\%s", root, stem);
    CreateDirectoryA(dir, nullptr);

    strncpy_s(probeId, idN, "W2-H-audio", _TRUNCATE);
    char ids[512] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    if (const char* found = strstr(ids, "probe_audio=")) {
        const char* value = found + strlen("probe_audio=");
        size_t n = strcspn(value, ";");
        if (n >= idN) {
            n = idN - 1;
        }
        memcpy(probeId, value, n);
        probeId[n] = '\0';
    }
    return true;
}

// One writer for both records, and the record itself is checkledger.h's FormatRecord: the verdict,
// the token, the unasked list and the in-progress word all come from the header the offline suite
// and audio_ledgerdump share, so nothing about the artifact's meaning is decided here.
//
// ★ IT LANDS ATOMICALLY. The harness polls done.json while the game runs -- its completion
// predicate parses it -- and a reader that catches the file mid-write gets a parse error. On the
// provisional record that costs nothing; on the final one it would be a lost verdict. So every
// record is written beside the target and renamed over it.
void WriteRecord(HMODULE self, bool provisional) {
    char dir[MAX_PATH];
    char probeId[128];
    if (!ProbeArtifact(self, dir, sizeof dir, probeId, sizeof probeId)) {
        return;
    }

    ledger::Record rec;
    rec.probeId = probeId;
    rec.provisional = provisional;
    rec.couldNotAsk = g_couldNotAsk || !g_wwiseSeen;
    rec.completionReached = g_matchSeen;
    rec.wwiseUpMs = g_wwiseUpMs;
    rec.sinkWaitMs = g_sinkWaitMs;
    rec.sinkPolls = g_sinkPolls;
    rec.selectorFirstMs = g_selectorFirstMs;
    rec.sinkListFirstMs = g_sinkListFirstMs;
    rec.matchSeenMs = g_matchSeenMs;
    rec.detail = g_detail;
    // Only the worker thread writes records, so one buffer serves every call.
    static char record[12288];
    ledger::FormatRecord(g_ledger, rec, record, sizeof record);

    char tmp[MAX_PATH];
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", dir);
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s\\done.json.writing", dir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, tmp, "wb") != 0 || fh == nullptr) {
        return;
    }
    fputs(record, fh);
    fclose(fh);

    // The rename can lose a race with the harness's own poll, which opens done.json to parse it and
    // does not share delete. Retried rather than dropped: on the final record a lost rename is a
    // lost verdict, and a second of retries costs nothing against a 240 s launch.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
            return;
        }
        Sleep(50);
    }
    // Last resort. A torn record is worse than an atomic one and better than none: the harness
    // treats an unparseable done.json as "not a verdict yet" and reads it again.
    if (CopyFileA(tmp, path, FALSE)) {
        DeleteFileA(tmp);
    }
}

void WriteVerdict(HMODULE self) { WriteRecord(self, false); }

// ★ SO THAT A SHORT LIFE STILL ANSWERS THE QUESTION. Two launches in the last sweep exited on their
// own at ~13 s on this profile. A probe whose only output is its final verdict has nothing to show
// for a game that died mid-wait, and "the sink did not appear within 13 s" is a real answer worth
// keeping. The harness's completion predicate reads past a provisional record on purpose, so this
// does not end the launch the way a verdict does -- and the record carries the in-progress word
// rather than a verdict, so it could not end one even if that predicate stopped looking.
void WriteProvisional(HMODULE self) {
    static double lastMs = -1.0e9;
    const double now = SinceStartMs();
    if (now - lastMs < 2000.0) {
        return;
    }
    lastMs = now;
    WriteRecord(self, true);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(self);
            // ★ First, because this is what arms the offline-bit lock: bootstrap::Attach starts the
            // worker that runs offlinelock::Apply() once the build guard accepts, and this probe runs
            // on a profile that loads a match. Reported, not obeyed -- a refused instance claim is a
            // normal outcome in a shared plugins folder and is not a reason to stop measuring; the
            // verdict says which it was.
            InterlockedExchange(&g_bootstrapOk, replay::bootstrap::Attach(self) ? 1 : 0);
            // A worker, not DllMain: this probe sleeps for seconds at a time waiting for Wwise and
            // measuring rates, and doing that under the loader lock would deadlock the game.
            HANDLE t = CreateThread(nullptr, 0, &Worker, self, 0, nullptr);
            if (t != nullptr) {
                CloseHandle(t);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            replay::bootstrap::Detach(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
