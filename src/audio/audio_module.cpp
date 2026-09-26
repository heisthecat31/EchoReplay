// P-4 `audio`. Binds the AK exports, publishes IAudioState and IAudioRate into ReplayServices,
// registers the three category-N primitives, and ticks the closed loop.
//
// ★ EVERY OUTCOME IS ANNOUNCED, INCLUDING THE GOOD ONES. S10.4.5 makes the degradation ladder a
// designed path rather than an error case, and its second property is that failure is never silent:
// a replay that quietly desyncs its audio reads as a fidelity bug, which is exactly what the label
// exists to prevent. So the startup line says what bound, what refused, and why -- and it says it
// once, at load, when the operator can still choose a different configuration.

#include "audio/akbind.h"
#include "audio/audiopins.h"
#include "audio/audiorate.h"
#include "audio/ratedrive.h"
#include "audio/audioresidue.h"
#include "audio/audiopinsource.h"
#include "audio/audiostate_init.h"
#include "audio/audio_facts.h"
#include "audio/foleymix.h"
#include "audio/voicelifetime.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/services.h"
#include "bootstrap/tickcost.h"
#include "engine/audiorate.h"
#include "engine/audiostate.h"
#include "engine/tick.h"
#include "playback/core/playback.h"

#include <windows.h>

namespace replay {
namespace {

const char* const kReplayModuleName = "audio";

ReplayServices* g_services = nullptr;
audio::AkExports g_ak;
audio::AudioResidueConfig g_residueCfg;
bool g_engineCallsEnabled = false;
audio::RateDriver g_rateDriver;
audio::FoleyMixHold g_foleyMix(audio::FoleyMix::Engine);

// The wall clock the loop and the voice tracker run on. QPC through the tick's own frame delta
// rather than a second timer: two clocks measuring the same frame is how a drift nobody can
// attribute gets introduced.
double g_sceneSeconds = 0.0;

// REPLAY_MODULE's tick, not an ITick::RegisterEarly callback. engine/tick.cpp already drives
// modreg::TickAll from its own early registration, so registering a second time would tick this
// module twice a frame and advance the closed loop's clock at double rate -- which reads as the
// audio ratio drifting for no reason anybody could find.
// ★★★ WHETHER THE SEEK-RESIDUE BATTERY WAS EVER ARMED, ASKED BY THE PACKET THAT SUPPLIES IT.
//
// ArmAudioResidue is the one call that puts N-5a, N-17a, N-16 and N-17b into the battery, and it is
// made by the playback core because the battery does not exist when this module initialises. If it
// is never made, nothing anywhere fails: the battery still reports thirteen rows, two of them
// PLACEHOLDERS that name this packet as the owner of the audio ones, and a backwards seek then
// leaves the Wwise memo stale for the rest of the session -- music and atmosphere wrong, no error,
// no log line, every test green. That is the exact defect this packet exists to prevent, and it was
// shipped unreachable once already.
//
// So it is asked, once, from the side that can tell. "N-5a" is registered by nothing but
// RegisterAudioResidue -- the core's placeholders are "N-16" and "N-17" -- so a null diagnostic for
// it is a positive answer and not an inference.
//
// ★ The question can only be asked once the core has a battery, and `consulted` is the witness for
// that: before playback::Init runs, an absent row means "too early", not "never armed", and the two
// must not share an answer.
//
// ★★ W3-A's residue-check module (order 310) reports the same MISSING CALL and says "audio::
// ArmAudioResidue is in the same state" -- but that half of its line is prose, not a check: it reads
// its own armed flag and asserts ours by association. The two seams are separate calls and can
// diverge, so this one asks about W2-H's rows specifically. Two lines when both are missing is a
// cheaper failure than one line that is wrong about which packet is dead.
void ReportResidueArming() {
    static bool answered = false;
    if (answered || g_services == nullptr || g_services->log == nullptr) {
        return;
    }
    const playback::ResidueStatus residue = playback::ResidueSnapshot();
    if (!residue.consulted) {
        return;   // the core has not initialised; there is nothing yet to be absent from
    }
    answered = true;
    if (playback::ResidueDiagnostic("N-5a") != nullptr) {
        g_services->log->Write(LogLevel::Info, kReplayModuleName,
                               "seek-residue battery armed: the audio primitives are registered "
                               "among %u row(s)",
                               static_cast<unsigned>(residue.registered));
        return;
    }
    g_services->log->Write(
        LogLevel::Error, kReplayModuleName,
        "THE AUDIO SEEK RESIDUE IS NOT ARMED. The battery holds %u row(s) and none of them is "
        "N-5a, so audio::ArmAudioResidue was never called and N-5a/N-16/N-17a/N-17b are not "
        "registered. A backwards seek will NOT reconcile the Wwise state memo, will not re-arm "
        "sustained loops and will not restore the container-history pins: music and atmosphere "
        "stay wrong for the rest of the session, silently. The call belongs in playback::Init "
        "once g.battery exists (src/playback/core/playback_module.cpp, W2-B).",
        static_cast<unsigned>(residue.registered));
}

uint32_t g_soundPlayerArmAttempts = 0;
audio::SoundPlayerProbe g_soundPlayerProbe = nullptr;
double g_nextArmAtS = 0.0;
bool g_lateArmLogged = false;

// InitAudioState is a one-shot at module init and the game has not created CSoundPlayer by then.
// Retried about once a second until it takes, because everything downstream -- seek reconcile, the
// residue rows, the capture's keyframe pins -- reads AudioState() and gets null for the life of the
// process otherwise.
void RetrySoundPlayerBind() {
#if defined(W2H_MUTATE_NO_LATE_ARM)
    return;   // mutation control: the one-shot resolve, which is the L2-RUN-13 defect itself
#else
    if (audio::AudioState() != nullptr || g_sceneSeconds < g_nextArmAtS) {
        return;
    }
    g_nextArmAtS = g_sceneSeconds + 1.0;
    ++g_soundPlayerArmAttempts;
    // 0 means "resolve the real one", which is every shipping call: the probe is null unless a test
    // set it, and a test's fake appears on the tick it chooses rather than on the first one.
    const uintptr_t probed = g_soundPlayerProbe != nullptr ? g_soundPlayerProbe() : 0;
    if (!audio::InitAudioState(g_ak, probed)) {
        return;
    }
    if (g_services != nullptr) {
        g_services->audioState = audio::AudioState();
        if (!g_lateArmLogged && g_services->log != nullptr) {
            g_lateArmLogged = true;
            g_services->log->Write(LogLevel::Info, kReplayModuleName,
                                   "CSoundPlayer resolved %.1f s after init (not at init): state "
                                   "reconcile and keyframe audio pins armed from here",
                                   g_sceneSeconds);
        }
    }
#endif
}

// Real elapsed time between ticks, which is not the engine's frame delta: the clock module scales
// that by the playback rate, and a loop that compares a wall-paced audio clock against it needs the
// unscaled number. Zero on the first tick, which RateLock reads as "no window" and skips.
double WallDtSeconds() {
    static LARGE_INTEGER freq = {};
    static LARGE_INTEGER last = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG prev = last.QuadPart;
    last = now;
    if (prev == 0 || freq.QuadPart == 0 || now.QuadPart <= prev) {
        return 0.0;
    }
    return static_cast<double>(now.QuadPart - prev) / static_cast<double>(freq.QuadPart);
}

void AudioTick(const TickInfo& info) {
    if (!g_engineCallsEnabled) {
        return;
    }
    // A ROOT of its own: the engine enters this module on its own tick, so none of it was inside
    // the playback tick's scope and none of it reached the `cost:` line.
    const tickcost::Scope cost_root(tickcost::kRootAudio);
    ReportResidueArming();
    // Bound per tick, never at init: playback (order 300) publishes the transport after this module
    // (45) has initialised, so a pointer taken here at Init would be null for the whole session.
    // ★ RECORDER-2 part A / L2. The rate driver writes the engine's audio sink rate and takes the
    // seek mute; in a live match that is the owner's own game audio. Gated at the caller so the
    // driver itself stays a pure, test-drivable object.
    tickcost::Scope cost_rate(tickcost::kAudioRate);
    if (livesafety::Allowed(livesafety::Feature::AudioRate)) {
        g_rateDriver.Drive(g_services->transport, g_services->audioRate, audio::AudioSeekMuteSeam(),
                           g_services->log);
    }
    cost_rate.Close();
    const double dt = static_cast<double>(info.frameDeltaUs) * 1e-6;
    g_sceneSeconds += dt;
    RetrySoundPlayerBind();
    // ★ TWO DIFFERENT SECONDS. g_sceneSeconds is SCALED sim time; the rate lock compares it against
    // Wwise's buffer clock, which runs in WALL time, so handing the scaled dt as the wall dt made
    // the loop gain wrong by the rate itself on top of comparing unlike clocks.
    // ★ LIVE-SAFETY M1. The closed-loop lock commands SetFrequencyRatio on the SAME sink voices the
    // driver above writes, so it needs the SAME gate -- it was outside it, and with no demo loaded
    // the loop's own guard does not fire (the default RatePlan is Normal/1.0/no-suspend and
    // audio.rate_control defaults on), so it walked the live game's audio to the +/-5% rail.
    // Loaded is part of the condition, not an optimisation: with no replay there is nothing for a
    // lock between the transport and Wwise to correct, so it must write nothing at all.
    const bool replayLoaded =
        g_services->transport != nullptr && g_services->transport->DurationUs() > 0;
    if (replayLoaded && livesafety::Allowed(livesafety::Feature::AudioRate)) {
        const tickcost::Scope cost_tick(tickcost::kAudioRate);
        audio::TickAudioRate(g_sceneSeconds, WallDtSeconds());
    }
    // The foley-mix state (foleymix.h). Loaded is part of the condition: with no replay the state
    // belongs to whatever the engine is doing for the person at the keyboard.
    if (replayLoaded && livesafety::Allowed(livesafety::Feature::FoleyMix)) {
        const audio::FoleyMixOutcome outcome = g_foleyMix.Tick(audio::AudioState(), GetTickCount64());
        if (outcome == audio::FoleyMixOutcome::Corrected && g_services->log != nullptr) {
            g_services->log->Write(LogLevel::Info, kReplayModuleName,
                                   "foley mix: Wwise state group %u answered %u, set to %u "
                                   "(audio.foley_mix=%s; correction %u)",
                                   audio::kFoleyMixStateGroup, g_foleyMix.Report().lastSeen,
                                   g_foleyMix.WantedState(), audio::FoleyMixName(g_foleyMix.Mix()),
                                   g_foleyMix.Report().corrections);
        }
    } else {
        g_foleyMix.Reset();
    }
    // The loop-versus-one-shot discriminator needs a sample rate faster than the keyframe rate, and
    // the tick is the only thing in this design that runs at frame rate for free.
    {
        const tickcost::Scope cost_voices(tickcost::kAudioVoices);
        audio::AudioTrackVoices(static_cast<uint32_t>(info.simTimeUs / 1000));
    }
    // The keyframe pin set is maintained HERE, a bounded slice per tick, and not inside the
    // keyframe sample: L2-RUN-14 F37 measured the whole-table walk at 159 ms on one game-thread
    // tick, twice a second, and it cost the session 13.6% of its ticks.
    const tickcost::Scope cost_pins(tickcost::kAudioPins);
    audio::AudioPinsTick();
}

bool AudioInit(ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);
    g_services = services;
    ILog* log = services->log;
    const IConfig* cfg = services->config;

    // Ledger row 0.11. With it off nothing below runs, which is what "individually removable" has to
    // mean for a row whose absence must not take the rest of playback down with it.
    g_engineCallsEnabled = cfg->GetBool("audio.engine_calls", true);
    if (!g_engineCallsEnabled) {
        log->Write(LogLevel::Info, kReplayModuleName,
                   "audio.engine_calls is off: no audio rate, seek or determinism control");
        return true;
    }

    audio::BindReport bind;
    const bool allBound = audio::Bind(&g_ak, &bind);
    log->Write(allBound ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
               "AK exports: %u of %u resolved by name%s%s", bind.resolved, bind.requested,
               bind.firstMissing != nullptr ? ", first missing: " : "",
               bind.firstMissing != nullptr ? bind.firstMissing : "");

    // ★ THE CUTOFF A/B'S OTHER ARM. With this off, every consumer below binds an AkExports whose
    // writing entry points are null and the sink ratio is never commanded, so src/audio is strictly
    // read-only for the whole session: the census still runs, the pitch simply is not applied. It
    // is the only way to ask "is the missing cutoff something WE write" and get an answer, because
    // what would be changing is inside Wwise's bank data where no code review can reach.
    const bool writesEnabled = cfg->GetBool("audio.writes_enabled", true);
    audio::SetAudioWritesEnabled(writesEnabled);
    if (!writesEnabled) {
        g_ak = audio::ReadOnlyAkExports(g_ak);
        log->Write(LogLevel::Warn, kReplayModuleName,
                   "audio.writes_enabled is OFF: this session writes NOTHING to Wwise or to the "
                   "engine's audio memory -- no state, no switch, no memo, no post, no stop, no "
                   "seek, no suspend, no sink pitch. Slow-motion audio will not be pitched.");
    }

    // IAudioState publishes even when CSoundPlayer could not be reached, because AudioState()
    // returns null in that case and every consumer already checks its ReplayServices pointers.
    if (audio::InitAudioState(g_ak)) {
        services->audioState = audio::AudioState();
        log->Write(LogLevel::Info, kReplayModuleName, "CSoundPlayer resolved; state reconcile armed");
    } else {
        log->Write(LogLevel::Warn, kReplayModuleName,
                   "CSoundPlayer not resolved: audio state will not be reconciled on a seek "
                   "(this is the expected state under -noaudio)");
    }

    // Ledger row 2.6, armed HERE: at module init, before anything probes a sink, because the
    // detour is what makes calling through a proven sink safe and a probe that ran first would
    // have run unguarded. hookmgr owns the audio.sink_term_hook key, so the arm reads it, not us.
    // The verdict is printed exactly as the arm returned it -- the module never supplies a word
    // the arm did not.
    const audio::LifetimeArm lifetimeArm = audio::ArmVoiceLifetime();
    audio::SetVoiceLifetimeArm(lifetimeArm);
    if (lifetimeArm == audio::LifetimeArm::Armed) {
        log->Write(LogLevel::Info, kReplayModuleName,
                   "voice lifetime hook (AUDIO.SINK_TERM, ledger row 2.6): armed at 0x%llx",
                   static_cast<unsigned long long>(audio_facts::kSinkTermVa));
    } else {
        log->Write(LogLevel::Warn, kReplayModuleName,
                   "voice lifetime hook (AUDIO.SINK_TERM, ledger row 2.6): REFUSED: %s",
                   audio::LifetimeArmName(lifetimeArm));
    }

    audio::AudioRateConfig rateCfg;
    rateCfg.rateHookRequested = cfg->GetBool("audio.max_rate_hook", true);
    // ★ Requested is not installed. This build ships no Level-1X hook, so MaxRate() is 1.0 whatever
    // the key says, and the label distinguishes "you turned it off" from "it is not available".
    rateCfg.rateHookInstalled = false;
    rateCfg.closedLoop = cfg->GetBool("audio.rate_control", true);

    const bool rateOk = audio::InitAudioRate(g_ak, rateCfg);
    services->audioRate = audio::AudioRate();
    // Three states, printed once at load where the operator can still choose another configuration.
    log->Write(audio::SinkAvailabilityState() == audio::SinkAvailability::Available
                   ? LogLevel::Info
                   : LogLevel::Warn,
               kReplayModuleName, "%s (row 2.6: %s)", audio::SinkAvailabilityLine(),
               audio::LifetimeArmName(audio::VoiceLifetimeState()));
    if (rateOk) {
        log->Write(LogLevel::Info, kReplayModuleName,
                   "rate control available; max rate %.1fx (%s)",
                   static_cast<double>(services->audioRate->MaxRate()),
                   rateCfg.rateHookRequested ? "rate hook requested but not installed in this build"
                                             : "rate hook disabled by config");
    } else if (services->audioRate == nullptr) {
        log->Write(LogLevel::Warn, kReplayModuleName,
                   "audio rate control unavailable: no instance");
    } else {
        // Not yet, not never. CAkSinkXAudio2::Init has not run when this module binds, so the
        // version selector still reads 0 and there is no sink to prove; Tick re-arms every frame.
        // Logging that as a warning made an ordinary startup read as a refused proof.
        log->Write(LogLevel::Info, kReplayModuleName,
                   "audio rate control not armed at bind time (re-armed each frame): %s",
                   services->audioRate->Diagnostic());
    }

    g_residueCfg.reconcileState = cfg->GetBool("audio.state_reconcile", true);
    g_residueCfg.muteOnSeek = cfg->GetBool("audio.seek_mute", true);

    const char* mixText = nullptr;
    audio::FoleyMix mix = audio::FoleyMix::Engine;
    if (!audio::FoleyMixFromConfig(cfg, &mix, &mixText)) {
        log->Write(LogLevel::Warn, kReplayModuleName,
                   "audio.foley_mix=\"%s\" is not engine|headset|broadcast: the foley-mix state is "
                   "left to the engine", mixText != nullptr ? mixText : "");
    }
    g_foleyMix = audio::FoleyMixHold(mix);
    if (mix == audio::FoleyMix::Engine) {
        log->Write(LogLevel::Info, kReplayModuleName,
                   "foley mix: audio.foley_mix=engine, Wwise state group %u is not held",
                   audio::kFoleyMixStateGroup);
    } else {
        log->Write(LogLevel::Info, kReplayModuleName,
                   "foley mix: audio.foley_mix=%s, Wwise state group %u held at %u while a replay "
                   "is loaded (%s)",
                   audio::FoleyMixName(mix), audio::kFoleyMixStateGroup, g_foleyMix.WantedState(),
                   mix == audio::FoleyMix::Headset
                       ? "the in-headset child: thrusters silent past 10 m, grabs 7 m, beds 15 m"
                       : "the broadcast child: 40 m curves floored at -19 dB");
    }

    return true;
}

void AudioShutdown() {
    // Printed unconditionally, including all-zero: "the drain never waited" and "nobody counted"
    // are different findings, and kTermDrainBudgetUs can only be judged against drainTimeouts.
    if (g_services != nullptr && g_services->log != nullptr) {
        const audio::VoiceLifetimeStats vl = audio::VoiceLifetimeStatsSnapshot();
        g_services->log->Write(LogLevel::Info, kReplayModuleName,
                               "voice lifetime (row 2.6, %s): %llu term entries, %llu drain waits, "
                               "%llu drain timeouts, %llu refusals, %llu no reader slot",
                               audio::LifetimeArmName(audio::VoiceLifetimeState()),
                               static_cast<unsigned long long>(vl.termEntries),
                               static_cast<unsigned long long>(vl.drainWaits),
                               static_cast<unsigned long long>(vl.drainTimeouts),
                               static_cast<unsigned long long>(vl.refusals),
                               static_cast<unsigned long long>(vl.noReaderSlot));
    }
    audio::ShutdownAudioRate();
    if (g_services != nullptr) {
        g_services->audioState = nullptr;
        g_services->audioRate = nullptr;
        g_services = nullptr;
    }
    g_engineCallsEnabled = false;
    g_rateDriver.Reset();
}

// Order 45: after identity (40) so a log line about audio follows the roster, and before playback,
// which asks IAudioRate for MaxRate before it decides what the transport can offer.
REPLAY_MODULE("audio", 45, AudioInit, AudioTick, AudioShutdown);

}  // namespace

namespace audio {

// The seam the playback core calls once it owns a battery. Kept here rather than in AudioInit
// because the battery belongs to W2-B and does not exist at module-init time.
void SetSoundPlayerProbeForTest(SoundPlayerProbe probe) { g_soundPlayerProbe = probe; }

size_t ArmAudioResidue(playback::ResidueBattery& battery) {
    // * &AudioState, not AudioState(). Arming happens when a demo loads, which can fall either
    // side of the late CSoundPlayer resolve; passing the value froze whichever answer that race
    // happened to give, and on every host so far that answer was null.
    return RegisterAudioResidue(battery, &AudioState, AudioRate(), g_residueCfg);
}

uint32_t AudioStateArmAttempts() { return g_soundPlayerArmAttempts; }

}  // namespace audio
}  // namespace replay
