#include "audio/audioresidue.h"

#include <new>
#include <string.h>

#include "audio/audiorate.h"
#include "audio/ratedrive.h"
#include "audio/audiostate_init.h"

namespace replay {
namespace audio {
namespace {

using playback::Availability;
using playback::Cause;
using playback::Discontinuity;

// One copy, because ReconcileResidue::Diagnostic() returns it per call now rather than holding a
// string chosen at registration.
const char kNullStateDiagnostic[] = "IAudioState is null: CSoundPlayer was not resolved";

AudioPins g_pins;
bool g_pinsValid = false;
uint32_t g_pinsGeneration = 0;
// How many holders currently forbid waking the sink. Two can stand at once -- the per-jump
// bracket N-17a takes below, and an outer drag (E-A5) spanning many jumps -- and neither one's
// release may drop the other's, which a single flag cannot express.
uint32_t g_muteHolds = 0;
AudioResidueStats g_stats;
// The ONE live config, seeded by RegisterAudioResidue from the module's config keys and read by the
// rows on every Fire.
AudioResidueConfig g_cfg;

// Shared state the three primitives coordinate through. They are separate battery rows because
// S10.3 orders them around the reconcile, not because they are independent.
struct Bracket {
    IAudioRate* rate = nullptr;
    bool suspendedByUs = false;
    bool holdsMute = false;   // this bracket owns one of g_muteHolds and has not handed it back
};
Bracket g_bracket;

void WakeIfWeSuspended() {
    if (g_bracket.suspendedByUs && g_bracket.rate != nullptr) {
        g_bracket.rate->Wakeup();
        g_bracket.suspendedByUs = false;
    }
}

// The destructive half runs on a real jump, never on the stream of jumps a held scrub or a
// fast-forward micro-seek produces.
bool IsFullJump(const Discontinuity& d) {
    return (d.cause == Cause::Seek || d.cause == Cause::GapCross) && d.needs_preroll;
}

// ★ N-5a -- the audio path's accumulators, invalidated. S7.5.5 names N-5 as "accumulators compute
// clockTime - base and go negative", and the audio subsystem has two of exactly that shape:
//
//   the closed-loop rate lock, which integrates (audioSeconds - sceneSeconds) minus a base taken
//   once, and whose only corrective lever is DETUNING THE MIX -- so carrying it across a jump makes
//   a seek audibly change pitch for a reason no listener could attribute;
//
//   the loop tracker, which measures how long a voice has been continuously sounding. Its window
//   ends at the jump, and after a BACKWARD one its two stamps straddle the discontinuity, so the
//   duration is not merely wrong but meaningless.
//
// It runs on EVERY cause, including ReverseTick and MicroSeek, because those are exactly the ones
// that fire at frame rate and would otherwise integrate a jump per frame. It is cheap: two stores.
class ClockRebaseResidue final : public playback::IResidue {
public:
    explicit ClockRebaseResidue(IAudioRate* rate) : rate_(rate) {}

    const char* Id() const override { return "N-5a"; }
    // Always Ready. It invalidates our own caches; it calls nothing that can be absent, so there is
    // no bind it could be missing. A row that reported Unmapped here would be skipped, and the
    // caches it exists to clear are precisely the ones nothing else clears.
    Availability Avail() const override { return Availability::Ready; }
    const char* Diagnostic() const override { return ""; }
    bool RunsOn(Cause) const override { return true; }

    void Fire(const Discontinuity&) override {
        MarkAudioClockDiscontinuity();
        AudioForgetVoices();
        ++g_stats.clockRebases;
        (void)rate_;
    }

private:
    IAudioRate* rate_ = nullptr;
};

class ReconcileResidue final : public playback::IResidue {
public:
    explicit ReconcileResidue(AudioStateGetter get) : get_(get) {}

    const char* Id() const override { return "N-16"; }
    // * Both of these ASK, every time the battery calls them. Snapshotting either at construction
    // is what left this row Unmapped for the life of a session whose CSoundPlayer resolved one
    // second in (L2-RUN-13): the skipped set is chosen per fire, so the row un-skips itself.
    Availability Avail() const override {
        return State() != nullptr ? Availability::Ready : Availability::Unmapped;
    }
    const char* Diagnostic() const override {
        return State() != nullptr ? "" : kNullStateDiagnostic;
    }

    bool RunsOn(Cause c) const override {
        // Every cause. A reverse tick crosses the same state boundaries a backwards seek does, and
        // a chapter epoch roll can bring a new keyframe's targets with it.
        (void)c;
        return true;
    }

    void Fire(const Discontinuity& d) override {
        IAudioState* const state = State();
        if (state == nullptr) {
            return;
        }
        // ★ The exact trigger, not a rate limit. On a reverse tick or a micro-seek the reconcile is
        // only meaningful if the resolved target changed; the pins generation says whether it did,
        // and comparing it is what keeps a 90 Hz reverse from issuing 90 Wwise queries a second per
        // state group for no reason.
        const bool cheapCause = d.cause == Cause::ReverseTick || d.cause == Cause::MicroSeek ||
                                d.cause == Cause::ChapterEpochRoll;
        if (cheapCause && g_pinsGeneration == firedGeneration_) {
            return;
        }
        firedGeneration_ = g_pinsGeneration;
        if (!g_cfg.reconcileState) {
            return;
        }

        if (g_pinsValid) {
            state->SetTargets(g_pins.states, g_pins.stateCount, g_pins.switches, g_pins.switchCount);
        }
        state->Reconcile();
        ++g_stats.reconciles;
    }

private:
    IAudioState* State() const { return get_ != nullptr ? get_() : nullptr; }

    AudioStateGetter get_ = nullptr;
    uint32_t firedGeneration_ = 0xFFFFFFFFu;
};

// S10.3 step 1: the mute. Step 2 was StopAll(kAkInvalidGameObject) and it is gone on purpose.
// Every voice on a pawn emitter is the engine's: the blanket stop killed the per-pawn ambience loop
// the engine posts once per pawn and never re-posts, so every replay lost it at its first seek while
// live carries it on all eight players for the whole match. Nothing after a jump needs a stop: the
// pose feed hands the engine the action-word edges between the pre-jump word and the target's, so
// the engine ends its own action loops, and one-shot tails end at their authored length.
class MuteResidue final : public playback::IResidue {
public:
    explicit MuteResidue(IAudioRate* rate) : rate_(rate) {}

    const char* Id() const override { return "N-17a"; }
    Availability Avail() const override {
        return rate_ != nullptr ? Availability::Ready : Availability::Unmapped;
    }
    const char* Diagnostic() const override {
        return rate_ != nullptr ? "" : "no audio rate bound: a seek has no sink to mute";
    }
    bool RunsOn(Cause c) const override { return c == Cause::Seek || c == Cause::GapCross; }

    void Fire(const Discontinuity&) override {
        if (!g_cfg.muteOnSeek || rate_ == nullptr || g_bracket.suspendedByUs) {
            return;
        }
        // Suspend(true): Wwise keeps PROCESSING while it stops OUTPUTTING, so the reconcile and
        // the pin restore that follow are consumed rather than queued behind a stopped engine,
        // and every voice keeps running through the bracket.
        //
        // The hold is taken BEFORE the suspend, not after: the rate driver reads
        // AudioSeekMuteHeld() every tick and will not command a sink somebody is holding, and a
        // hold published after the action it protects is not a hold.
        g_bracket.rate = rate_;
        g_bracket.suspendedByUs = true;
        g_bracket.holdsMute = true;
        HoldAudioSeekMute(true);
        rate_->Suspend(true);
        ++g_stats.mutes;
    }

private:
    IAudioRate* rate_ = nullptr;
};

// S10.3 steps 4 and 6: the RNG pins and the unmute. Step 5, the loop re-arm, went with the StopAll:
// with no stop there is nothing to put back, and a re-post would stack a second copy on a voice
// that never stopped.
class UnmuteResidue final : public playback::IResidue {
public:
    explicit UnmuteResidue(AudioStateGetter get) : get_(get) {}

    const char* Id() const override { return "N-17b"; }
    // ★ ALWAYS READY. This row releases the mute N-17a took, and ResidueBattery::Fire SKIPS a
    // non-Ready row: a mute taken by a Ready row and released by a skipped one leaves the replay
    // silent for the rest of the session after its first seek, with nothing reporting a failure.
    Availability Avail() const override { return Availability::Ready; }
    const char* Diagnostic() const override { return ""; }
    bool RunsOn(Cause c) const override { return c == Cause::Seek || c == Cause::GapCross; }

    void Fire(const Discontinuity& d) override {
        if (IsFullJump(d) && g_pinsValid) {
            RestorePins();
        }
        // Unconditional, including on a jump that did nothing: a release that depended on the same
        // predicate as the mute would leave the replay silent the first time the two disagreed.
        if (g_bracket.holdsMute) {
            g_bracket.holdsMute = false;
            HoldAudioSeekMute(false);   // wakes the sink only if no outer drag still holds it
        }
    }

private:
    void RestorePins() {
        IAudioState* const state = get_ != nullptr ? get_() : nullptr;
        if (state == nullptr) {
            return;
        }
        Blob history{g_pins.historyBytes != 0 ? g_pins.history : nullptr, g_pins.historyBytes};
        state->PinRandom(g_pins.seed, history);
        ++g_stats.pinsRestored;
    }

    AudioStateGetter get_ = nullptr;
};

ClockRebaseResidue* g_rebase = nullptr;
ReconcileResidue* g_reconcile = nullptr;
MuteResidue* g_mute = nullptr;
UnmuteResidue* g_unmute = nullptr;

// The getter the rows were armed with, so a caller can ask whether the state is reachable now
// without holding a row of its own.
AudioStateGetter g_stateGetter = nullptr;

#if defined(W2H_MUTATE_CAPTURED_STATE)
// Mutation control: the L2-RUN-13 defect itself -- the state resolved ONCE at registration and
// held for the session. Everything below is unchanged; only the moment of the read moves.
IAudioState* g_mutantCaptured = nullptr;
IAudioState* MutantCapturedState() { return g_mutantCaptured; }
#endif

// Static storage: the battery holds borrowed pointers and never deletes through them, and a module
// that allocated its residue rows would be the one allocation on a path that must not allocate.
alignas(ClockRebaseResidue) uint8_t g_rebaseStore[sizeof(ClockRebaseResidue)];
alignas(ReconcileResidue) uint8_t g_reconcileStore[sizeof(ReconcileResidue)];
alignas(MuteResidue) uint8_t g_muteStore[sizeof(MuteResidue)];
alignas(UnmuteResidue) uint8_t g_unmuteStore[sizeof(UnmuteResidue)];

}  // namespace

size_t RegisterAudioResidue(playback::ResidueBattery& battery, AudioStateGetter get,
                            IAudioRate* rate, const AudioResidueConfig& cfg) {
#if defined(W2H_MUTATE_CAPTURED_STATE)
    g_mutantCaptured = get != nullptr ? get() : nullptr;
    get = &MutantCapturedState;
#endif
    g_stateGetter = get;
    g_rebase = new (g_rebaseStore) ClockRebaseResidue(rate);
    g_reconcile = new (g_reconcileStore) ReconcileResidue(get);
    g_cfg = cfg;
    g_mute = new (g_muteStore) MuteResidue(rate);
    g_unmute = new (g_unmuteStore) UnmuteResidue(get);

    // S10.3's order, with N-5a ahead of it: the accumulators are rebased before anything reads
    // them, and it is the only row that runs on the causes S10.3 never contemplated (a reverse tick
    // and a micro-seek are discontinuities too, and they arrive at frame rate).
    size_t n = 0;
    n += battery.Register(g_rebase) ? 1u : 0u;
    n += battery.Register(g_mute) ? 1u : 0u;
    n += battery.Register(g_reconcile) ? 1u : 0u;
    n += battery.Register(g_unmute) ? 1u : 0u;
    return n;
}

void SetAudioPins(const AudioPins& pins) {
    g_pins = pins;
    g_pinsValid = true;
    ++g_pinsGeneration;
}

bool AudioSeekMuteHeld() { return g_muteHolds != 0; }

void HoldAudioSeekMute(bool held) {
    if (held) {
        ++g_muteHolds;
        return;
    }
    if (g_muteHolds == 0 || --g_muteHolds != 0) {
        return;   // never taken, or another holder still stands
    }
    WakeIfWeSuspended();
}

namespace {

// The one seam the rate driver sees. It READS the hold and REPORTS the unload; it cannot set the
// hold, because this file is the only owner of that fact.
class SeekMuteSeam final : public ISeekMute {
public:
    bool Held() const override { return g_muteHolds != 0; }
    void ReleaseOnUnload() override {
        // Holds belong to the session that took them. Deferring the hand-back instead would
        // strand the sink suspended for the rest of the process: nothing fires a discontinuity
        // once there is no replay to seek in, so no release would ever come.
        g_muteHolds = 0;
        g_bracket.holdsMute = false;
        WakeIfWeSuspended();
    }
};

SeekMuteSeam g_seam;

}  // namespace

ISeekMute* AudioSeekMuteSeam() { return &g_seam; }

const AudioResidueStats& AudioResidueStatsRef() { return g_stats; }

const AudioPins* AudioPinsForTest() { return g_pinsValid ? &g_pins : nullptr; }

bool AudioResidueStateBound() { return g_stateGetter != nullptr && g_stateGetter() != nullptr; }

void ResetAudioResidueForTest() {
    g_stateGetter = nullptr;
    g_pins = AudioPins{};
    g_pinsValid = false;
    g_pinsGeneration = 0;
    g_muteHolds = 0;
    g_cfg = AudioResidueConfig{};
    g_stats = AudioResidueStats{};
    g_bracket = Bracket{};
    g_rebase = nullptr;
    g_reconcile = nullptr;
    g_mute = nullptr;
    g_unmute = nullptr;
}

}  // namespace audio
}  // namespace replay
