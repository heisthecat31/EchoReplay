#include "audio/audiorate.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include "audio/sinkvoice.h"

namespace replay {
namespace audio {
namespace {

// ★ THE ONE TEST SEAM IN THIS FILE, AND WHY IT IS HERE RATHER THAN IN sinkvoice.cpp.
// What can be wrong in this class is SEQUENCING -- when the sink is probed, what a failed probe is
// allowed to conclude, and which rate is commanded once it comes back. None of that can be driven
// on a machine with no game, because both sink operations read echovr.exe's own memory. Routing
// the two calls through pointers lets tests/audio supply a chain whose appearance and disappearance
// it controls, which is the only way the pause-then-play path is checkable without a launch. The
// memory-reading code itself is untouched: the shipped functions are the defaults.
size_t (*g_probeSinks)(const AkExports&, SinkChain*) = &ProbeSinks;
size_t (*g_applyRatio)(const AkExports&, float, int32_t*, size_t*) = &ApplyFrequencyRatio;
// Through g_probeSinks rather than RestoreFrequencyRatio's own probe, so a test that supplied a
// chain gets the same chain on the restore path as on the command path. A restore that went to the
// real ProbeSinks would read echovr.exe's memory from a fixture that has none.
size_t ShippedRestore(const AkExports& ak, int32_t* firstError, bool forget) {
    if (firstError != nullptr) {
        *firstError = 0;
    }
    SinkChain chain;
    if (g_probeSinks(ak, &chain) == 0) {
        return 0;
    }
    return RestoreChainRatios(chain, SinkRatioLedger(), firstError, nullptr, forget);
}

size_t (*g_restoreRatio)(const AkExports&, int32_t*, bool) = &ShippedRestore;

// A one-line channel from this module to the log. It runs under the playback tick and holds no
// ILog; RateDriver drains this every tick. One slot, because a second notice before the drain is a
// later reading of the same situation, and the count says the drain was behind.
char g_notice[256] = {0};
uint32_t g_noticesSuperseded = 0;

class AudioRateBinding final : public IAudioRate {
public:
    bool Init(const AkExports& ak, const AudioRateConfig& cfg) {
        ak_ = ak;
        cfg_ = cfg;
        ResetBias();
        lock_.Reset();
        suspended_ = false;
        deviceDirty_.store(false);
        plan_ = RatePlan{};
        request_ = RateCommand{};

        if (ak_.GetAudioSettings == nullptr) {
            SetDiagnostic("Wwise exports did not bind: GetAudioSettings is absent");
            available_ = false;
            return false;
        }
        // GetAudioSettings doubles as the "is Wwise up" predicate: it returns AK_NotInitialized
        // before init and fills both numbers after. One call supplies the precondition and the
        // scale, which is why it is the right source rather than merely a better constant.
        if (ak_.GetAudioSettings(&settings_) != kAkSuccess || settings_.samplesPerFrame == 0 ||
            settings_.samplesPerSecond == 0) {
            SetDiagnostic("Wwise is not initialised (GetAudioSettings refused)");
            available_ = false;
            return false;
        }

        // ★ A FAILED ARM AT BIND TIME IS NORMAL, NOT A VERDICT. CAkSinkXAudio2::Init is what writes
        // AUDIO.XAUDIO2_VER_SELECTOR and AUDIO.SINK_LIST_PTR (sinkvoice.h S198), and it has not run
        // when this module binds -- the selector still reads 0, which is UnknownXAudio2Version. Tick
        // re-arms until the sink exists, so this returning false means "not yet", never "never".
        return Arm();
    }

    // The whole of the probe half, in one place, because it has three callers that used to carry
    // three copies: the bind, a device change, and the per-frame re-arm.
    bool Arm() {
        SinkChain chain;
        sinkCount_ = g_probeSinks(ak_, &chain);
        if (sinkCount_ == 0) {
            // With the operand, not just the word. A refusal that names a comparison and hides
            // what it compared is why the last six launches each re-derived this module from
            // source instead of reading one line.
            if (chain.firstRefusal == SinkVerdict::RatioOutOfDomain) {
                char detail[192];
                snprintf(detail, sizeof(detail), "%s (it read %.6f)",
                         SinkVerdictName(chain.firstRefusal),
                         static_cast<double>(chain.firstRefusalRatio));
                SetDiagnostic(detail);
            } else {
                SetDiagnostic(SinkVerdictName(chain.firstRefusal));
            }
            available_ = false;
            return false;
        }
        if (ak_.GetAudioSettings != nullptr) {
            ak_.GetAudioSettings(&settings_);
        }
        // ★ A SINK THAT COMES BACK OWES THE INTENT RECORDED WHILE IT WAS GONE (AUDIO-FIX-3 B).
        // `request_` holds what the viewer asked for whether or not there was a sink to ask; what
        // was missing is a place that DELIVERS it on every path that re-arms. ReArm replans for
        // itself, but Wakeup and the device-status callback arm through here too, and after those
        // the module sat available at 1.0x with a rate of 0.10x recorded and nothing to make it
        // retry -- a whole viewing at the wrong pitch, fixed only by touching the speed again.
        if (!available_) {
            deliverOnArm_ = true;
        }
        available_ = true;
        diagnostic_[0] = '\0';
        return true;
    }

    // ---- B.9 -----------------------------------------------------------------------------------

    bool Available() const override { return available_; }

    float MaxRate() const override {
        if (!available_) {
            return 0.0f;
        }
        return cfg_.rateHookInstalled ? cfg_.installedMaxRate : 1.0f;
    }

    void SetRate(float r) override {
        request_.rate = r;
        request_.paused = false;
        RateCommand cmd = request_;
        cmd.available = available_;
        cmd.maxRate = MaxRate();
        plan_ = PlanRate(cmd);
        // ★ A RATE CHANGE BYPASSES BOTH BACKOFFS. The 4 s re-arm ceiling and the reassert throttle
        // exist so a dead sink is not walked at frame rate; neither may hold the ONE transition the
        // viewer is listening to. The streak goes with them, so the retries that follow a rate
        // change are the 4 Hz the transient behind a wakeup needs rather than the ceiling a
        // long-dead sink earned.
        reArmNextMs_ = 0;
        reassertNextMs_ = 0;
        armFailures_ = 0;
        armFailStreak_ = 0;
        // A new rate is a new measurement for the loop, including one it gave up on at the old rate.
        loopDisengaged_ = false;
        railSinceMs_ = 0;
        floorClamped_ = false;

        if (plan_.suspend) {
            Suspend(true);
            LatchLabel(plan_);
            return;
        }
        // Before the availability gate, not after it. A wakeup is what puts sound back in the
        // room, and gating it on a flag that a suspended sink necessarily clears is how a single
        // pause used to mute the session for good.
        if (suspended_) {
            Wakeup();
            // ★ AND THE PLAN IS REBUILT ON WHAT THE WAKEUP LEFT BEHIND (AUDIO-FIX-3 B). `cmd`
            // above carries the availability as it was BEFORE Wakeup's own Arm(), and a wakeup
            // moves that flag in both directions: Wwise rebuilds CAkSinkXAudio2 asynchronously
            // behind WakeupFromSuspend, so a module that was available before the pause is usually
            // NOT available one statement later, and one that was unavailable can come back here.
            // Planning on the stale flag commands RateMode::Unavailable's ratio 1.0 under a label
            // latched from the live flag -- "audio: slowed with picture" over a mix still at 1x,
            // which is the paused-set arm of 2026-09-21.
            cmd.available = available_;
            cmd.maxRate = MaxRate();
            plan_ = PlanRate(cmd);
        }
        if (!available_) {
            // Nothing to command yet. Tick re-arms, replans from `request_` and commands then --
            // which is why the rate the user asked for is kept rather than the plan built without
            // a sink, whose ratio is the 1.0 of RateMode::Unavailable.
            LatchLabel();
            return;
        }
        // The trim is reset on every commanded change: an integrator carrying error from the
        // previous rate would fight the new one for a second, which is audible as a pitch drift
        // exactly when the user has just changed speed and is listening for it.
        lock_.Reset();
        CommandPlanned();
        LatchLabel();
    }

    // ★ ONE MODE PER RATE, DECIDED AFTER THE RATE HAS BEEN ACTED ON. The label used to be derived
    // from `available_` on every call, so a single-frame probe miss -- and the unthrottled re-arm
    // produced those by the dozen -- flipped the panel between "slowed with picture" and "rate
    // control unavailable" about a rate that had not changed. It is latched at the END of SetRate
    // because Wakeup() re-probes and Command() can be refused, either of which unlatches the sink
    // one statement after PlanRate saw it (L3-RUN-13 F4).
    void LatchLabel() {
        RateCommand cmd = request_;
        cmd.available = available_;
        cmd.maxRate = MaxRate();
        LatchLabel(PlanRate(cmd));
    }

    void LatchLabel(const RatePlan& decided) {
        labelPlan_ = decided;
        // What the sinks DID, not what the plan asked for. A ratio the device clamped up to its own
        // floor is still a rate the viewer can hear, and RateLabel already has the true sentence for
        // it; printing "slowed with picture" over a clamped voice is the label lying about the mix.
        if (floorClamped_ && labelPlan_.mode == RateMode::Slowed) {
            labelPlan_.pitchIsHonest = false;
        }
        labelUpgraded_ = false;
    }

    void Suspend(bool renderAnyway) override {
        // Not gated on `available_`. Suspend/Wakeup are Wwise exports; SetFrequencyRatio is the
        // sink's COM vtable. Gating a matched pair on a flag that can change between its halves
        // leaks a suspension -- and a pause that cannot mute is a worse product than one whose
        // pitch control refused.
        request_.paused = true;
        if (ak_.Suspend == nullptr || suspended_) {
            return;
        }
        // renderAnyway = true keeps Wwise processing while it stops outputting, which is what a
        // seek needs: StopAll, the state reconcile and the RNG pins are all commands, and a sound
        // engine that had stopped ticking would swallow them until the wakeup.
        if (ak_.Suspend(renderAnyway) == kAkSuccess) {
            suspended_ = true;
        }
    }

    void Wakeup() override {
        if (ak_.WakeupFromSuspend == nullptr || !suspended_) {
            return;
        }
        if (ak_.WakeupFromSuspend() != kAkSuccess) {
            return;
        }
        suspended_ = false;
        request_.paused = false;
        // ★ THE SINK IS REBUILT BEHIND THIS CALL, NOT INSIDE IT. Wwise recreates CAkSinkXAudio2 on
        // its own thread, so a probe issued here usually finds the list head still zero. That is a
        // transient and Tick's re-arm owns it. Commanding the ratio into the gap is what used to
        // latch `available_ = false` for the rest of the session -- the sink refused because it did
        // not exist yet, and nothing but a device-status event could ever clear the verdict.
        if (Arm() && !plan_.suspend) {
            // The ratio does not survive a device rebuild, and a wakeup is the cheapest place to
            // re-assert it that cannot be forgotten.
            CommandPlanned();
        }
    }

    double AudioSeconds() const override {
        // Not gated on `available_`: GetBufferTick is Wwise's own frame counter and keeps counting
        // whether or not the sink's ratio can be commanded. Returning 0 here while a measurement
        // window was open turned "the ratio was refused" into "the audio clock went backwards".
        if (ak_.GetBufferTick == nullptr || settings_.samplesPerSecond == 0) {
            return 0.0;
        }
        // ★ The denominator is READ, never written down. 93.75 is 48000/512 -- one device's
        // settings. On a 44.1 kHz endpoint the true figure is 86.13, and an oracle carrying the
        // constant would have reported every rate about 9% high and been "fixed" until it matched
        // the wrong number.
        const double frames = static_cast<double>(ak_.GetBufferTick());
        return frames * settings_.samplesPerFrame / settings_.samplesPerSecond;
    }

    const char* Diagnostic() const override { return diagnostic_; }

    int64_t DisplayBiasSceneUs() const override { return bias_.OffsetSceneUs(); }

    // The plan LATCHED for the rate the transport is running -- not a fresh read of `available_`.
    // SetRate decides it, DowngradeMode is the only other writer, and neither runs per frame, so
    // this string is constant for the lifetime of a rate.
    const char* RateLabel() const override {
        return replay::audio::RateLabel(labelPlan_, cfg_.rateHookRequested);
    }

    // The ledger, not a fresh COM read: this is called from a frame that has already run late, and
    // probing the sink chain there would charge the report to the hitch it is describing. Voice 0's
    // baseline/live pair is the one that names a ratchet.
    void DescribeState(char* out, size_t cap) const override {
        if (out == nullptr || cap == 0) {
            return;
        }
        const VoiceRatioLedger& ledger = SinkRatioLedger();
        const VoiceRatio* v0 = ledger.Count() > 0 ? &ledger.At(0) : nullptr;
        snprintf(out, cap,
                 "avail=%d mode=%u plan=%.4f last=%.4f susp=%d floor=%d loop=%s sinks=%zu "
                 "ledger=%zu baseline0=%.4f cmd0=%.4f read0=%.4f rearmfail=%u",
                 available_ ? 1 : 0, unsigned(plan_.mode), static_cast<double>(plan_.ratio),
                 static_cast<double>(lastCommanded_), suspended_ ? 1 : 0, floorClamped_ ? 1 : 0,
                 loopDisengaged_ ? "off" : "on", sinkCount_, ledger.Count(),
                 static_cast<double>(v0 != nullptr ? v0->baseline : 0.0f),
                 static_cast<double>(v0 != nullptr ? v0->commanded : 0.0f),
                 static_cast<double>(v0 != nullptr ? v0->observed : 0.0f), armFailures_);
    }

    // ---- the per-frame half ----------------------------------------------------------------------

    void MarkDeviceChanged() { deviceDirty_.store(true); }

    void Tick(double sceneSeconds, double dtWall) {
        // Both of these run BEFORE the early-out, not after it. Coming back from unavailable is
        // the one transition a check gated on being available could never take, and it is the
        // transition the whole module depends on: nothing else re-arms the sink.
        if (deviceDirty_.exchange(false)) {
            OnDeviceChanged();
        }
        nowMs_ += dtWall > 0.0 ? static_cast<uint64_t>(dtWall * 1000.0 + 0.5) : 0;
        if (!available_) {
            ReArm();
        }
        if (!available_) {
            return;
        }
        if (deliverOnArm_) {
            deliverOnArm_ = false;
            DeliverRecordedIntent();
        }

        // Feed the A/V bias until it latches. The queue depth swings while the voice fills, so the
        // maximum over the warm-up is what gets held -- a bias that tracked the swing would make
        // the picture jitter, and S10.4.1a's whole claim is that the correction is a constant.
        //
        // * AND THE WARM-UP IS BOUNDED. A sink that never reads cleanly never latches, so this used
        // to re-enter the read on every tick for the rest of the session -- hundreds of calls
        // through an object Wwise is free to destroy at any moment, which is what turned the L3 #8
        // race into a certainty. Past the budget the poll backs off to one attempt per warm-up
        // length: a sink that comes good after a long rebuild is still picked up, and one that
        // never will is no longer touched at frame rate.
        if (!bias_.Latched()) {
            if (BiasWarmupDue()) {
                uint64_t samples = 0;
                uint32_t queued = 0;
                size_t gone = 0;
                if (ReadChainSamples(&samples, &queued, &gone)) {
                    bias_.Observe(queued, settings_.samplesPerFrame, settings_.samplesPerSecond);
                } else if (gone > 0) {
                    ReportVoicesGone(gone);
                }
            }
        }

        // ★ THE LOOP ONLY RUNS AT 1.0x. AudioSeconds() is Wwise's buffer clock, which advances in
        // WALL time; sceneSeconds is scaled sim time. At any other rate their difference is not
        // drift -- it grows at (1-r) per second forever, the trim walks to its +/-5% rail, and
        // `corrected` differs from the last command on EVERY frame, so the sink is written at frame
        // rate. At 1.0x the two clocks measure the same seconds and the loop measures real drift.
        // ★ RECONCILING, NOT EDGE-DRIVEN. The module used to command only when the transport
        // changed, so any command that was dropped -- refused inside a wakeup, skipped while a seek
        // mute held the sink -- lasted until the user next touched the speed. These two assertions
        // run every tick and cost a few float compares plus one sink probe every two seconds.
        ReassertIntent();
        VerifyAgainstSinks();

        if (suspended_ || !LoopOwnsRatio() || plan_.suspend || plan_.mode != RateMode::Normal) {
            return;
        }
        RunClosedLoop(sceneSeconds, dtWall);
    }

    // ★ A ONE-SIDED CONTROLLER IS NOT A CONTROLLER (PITCH-FIX-2 S5). The loop trims the commanded
    // ratio by up to +/-5% to pull Wwise's buffer clock back onto the scene clock. Every voice it
    // can reach was created with MaxFrequencyRatio = 1.0 and at RateMode::Normal the target IS 1.0,
    // so the entire POSITIVE half of the trim is clamped away by XAudio2 before it reaches the
    // device -- silently, and then read back as drift, which pushed the trim further until it
    // railed. What is left is a loop that can only ever make the audio slower, with nothing but the
    // clamp to bring it back: the 0.29-0.31 cluster that outlived five minutes and six sinks in the
    // owner's log, and all eight `trim sat at its 5% rail` disengages.
    //
    // So it is gated on the voices being able to serve a correction in BOTH directions. Today no
    // Wwise sink voice can, so the loop never commands; the A/V correction that does work -- AvBias,
    // which moves the picture rather than the pitch -- is untouched. The machinery stays because
    // the gate is a property of the voice and not of the design: a voice with real headroom, which
    // the rate hook would create, gets its loop back with no further decision.
    bool LoopOwnsRatio() const {
        if (!cfg_.closedLoop || loopDisengaged_) {
            return false;
        }
        const VoiceRatioLedger& ledger = SinkRatioLedger();
        for (size_t i = 0; i < ledger.Count(); ++i) {
            if (ledger.At(i).maxRatio > kShippedFrequencyRatio &&
                !NearlyEqualRatio(ledger.At(i).maxRatio, kShippedFrequencyRatio)) {
                return true;
            }
        }
        return false;
    }

    // What the plan asks for against what was last commanded. At RateMode::Normal the intent is the
    // engine's own baseline, which CommandPlanned puts back absolutely -- never `baseline * 1.0`,
    // which only re-asserts whatever the ledger currently believes.
    void ReassertIntent() {
        if (suspended_ || plan_.suspend || !available_) {
            return;
        }
        const float want = PlannedRatio();
        if (NearlyEqualRatio(lastCommanded_, want)) {
            return;
        }
        // At 1.0x with the loop running, the trim owns the commanded value and a difference from
        // 1.0 is the correction working, not a dropped command. With the loop gated off -- which is
        // every voice this title has -- the guard must NOT fire, or the one intent the viewer is
        // listening for is the one that never retries.
        if (plan_.mode == RateMode::Normal && LoopOwnsRatio()) {
            return;
        }
        // ★ THE CHECK IS PER TICK, THE RETRY IS NOT. Everything above is float compares, so a
        // delivered intent costs nothing; the retry below re-probes the sink list, which is the
        // VirtualQuery walk per node the re-arm poll is throttled for. 4 Hz is two orders of
        // magnitude inside the window a rate is measured over, and SetRate clears this deadline so
        // the first retry after a rate change is on the very next tick.
        if (nowMs_ < reassertNextMs_) {
            return;
        }
        reassertNextMs_ = nowMs_ + kReassertRetryMs;
        CommandPlanned();
    }

    // The plan the sinks are supposed to be holding right now. NOT `lastCommanded_`, which is the
    // last ask that reached EVERY voice: when a rate change reaches part of a chain -- one sink
    // mid-rebuild, one voice destroyed under the call -- that stays at the old rate, and judging
    // the voices against it scores the ones that took the new rate as the broken ones.
    // `expected 1.0001 found 0.1000`, 0.77 s after our own correct 0.1000 write, four times in the
    // 2026-09-21 session, every one of them a report that the slow-mo was the fault.
    // With the loop engaged the trim is the intent, and that is the one case where the last
    // commanded value is the current one.
    float IntendedRatio() const {
        return LoopOwnsRatio() && plan_.mode == RateMode::Normal ? lastCommanded_ : PlannedRatio();
    }

    // A value we have just started asking for says nothing about the voice yet: the device applies
    // on its own processing pass and a read taken inside that pass returns the ask before it. A
    // RETRY of a value already outstanding does not restart the window -- the device has had the
    // whole period to apply that one, and a 4 Hz reassert of a stuck voice would otherwise hold the
    // drift check off for as long as the voice stayed stuck, which is exactly when it is needed.
    void NoteIntentIssued(float ratio) {
        if (NearlyEqualRatio(ratio, lastIssued_)) {
            return;
        }
        lastIssued_ = ratio;
        issuedAtMs_ = nowMs_;
    }

    // The voices themselves, twice a second at most. This is the line that would have named the
    // ratchet on its first occurrence instead of on the third launch: it prints what the voice
    // reads next to what the ledger says we put there.
    void VerifyAgainstSinks() {
        if (!available_ || suspended_ || plan_.suspend || nowMs_ < driftNextMs_) {
            return;
        }
        if (nowMs_ < issuedAtMs_ + kRatioReadLagMs) {
            return;   // inside the device's own pass: every reading is the previous ask
        }
        driftNextMs_ = nowMs_ + kDriftCheckPeriodMs;
        SinkChain chain;
        if (g_probeSinks(ak_, &chain) == 0) {
            return;
        }
        const float intent = IntendedRatio();
        SinkRatioDrift worst;
        const size_t drifted = CheckChainDrift(chain, SinkRatioLedger(), intent, &worst);
        if (drifted == 0) {
            return;
        }
        // Corrected every check, said at most every ten seconds. A drift this module cannot fix --
        // a voice whose MaxFrequencyRatio clamps every write, say -- is a real finding and a
        // standing one, and a standing finding repeated every two seconds is a log flood.
        if (nowMs_ >= driftSaidNextMs_) {
            driftSaidNextMs_ = nowMs_ + kDriftSayEveryMs;
            // The census rides the verify line it belongs to. A drift report that named a gap
            // without saying what the module believes the baselines are is the line the owner's
            // log carried nine times and nobody could act on.
            char census[256];
            FormatSinkRatioCensus(census, sizeof census,
                                  SinkRatioCensusNow(SinkRatioLedger(), intent));
            Notice("audio drift corrected: %zu voice(s) off, worst sink %p expected %.4f found "
                   "%.4f -- %s",
                   drifted, reinterpret_cast<void*>(worst.sink),
                   static_cast<double>(worst.expected), static_cast<double>(worst.live), census);
        }
        CommandPlanned();
    }

    // ★ THE LOOP MUST NOT BE THE THING THAT DETUNES THE MIX. At 1.0x it corrects real device drift.
    // When its trim sits at the +/-5% rail it has stopped measuring drift -- the owner's log caught
    // a voice held at 0.2795/0.2942 = 0.9500, the rail exactly, for twenty seconds with the panel
    // reading "1.0x, as the game mixed it" -- and every frame it writes is detune the viewer hears.
    // Three seconds at the rail and the loop is done for this rate; the baselines go back exactly.
    void RunClosedLoop(double sceneSeconds, double dtWall) {
        if (loopDisengaged_) {
            return;
        }
        const float corrected = lock_.Update(plan_.ratio, AudioSeconds(), sceneSeconds, dtWall);
        const float trim = lock_.Trim();
        const float mag = trim < 0.0f ? -trim : trim;
        if (mag < RateLock::kMaxTrim * 0.999f) {
            railSinceMs_ = 0;
        } else {
            if (railSinceMs_ == 0) {
                railSinceMs_ = nowMs_;
            }
            if (nowMs_ - railSinceMs_ >= kRailGiveUpMs) {
                loopDisengaged_ = true;
                lock_.Reset();
                CommandPlanned();
                Notice("audio drift loop disengaged: the trim sat at its %.0f%% rail for %llu ms, "
                       "which is not device drift; the sinks are back at the pitch the game mixed",
                       static_cast<double>(RateLock::kMaxTrim * 100.0f),
                       static_cast<unsigned long long>(nowMs_ - railSinceMs_));
                railSinceMs_ = 0;
                return;
            }
        }
        if (corrected != lastCommanded_) {
            Command(corrected);
        }
    }

    // Re-probes and re-applies. E-A1: a device change rebuilds the sink list, and the ratio does
    // not survive it. This is also the only correct response to a probe that now finds zero sinks:
    // report, do not latch a stale "available".
    //
    // ★ Called from Tick, never from the device-status callback itself. That callback runs on
    // Wwise's own thread, and re-entering ReplaceOutput's aftermath with COM calls from inside it
    // is a race we would win most of the time. The callback sets a flag; the game thread acts.
    void OnDeviceChanged() {
        // ★ THE LATCHED BIAS SURVIVES. DisplayBiasSceneUs is subtracted from the display playhead
        // every frame, so dropping a latched ~85 ms to zero jumps the presented instant by the whole
        // bias and jumps back when it re-latches -- 2.5 frames of motion at 1.0x, ten at 0.25x, and
        // that lurch IS the hitch the owner saw. Only a bias that never latched is re-measured.
        if (!bias_.Latched()) {
            ResetBias();
        }
        lock_.Reset();
        loopDisengaged_ = false;
        railSinceMs_ = 0;
        if (Arm() && !plan_.suspend) {
            CommandPlanned();
        }
    }

    // The one path back from unavailable, run from Tick while the module is degraded and never
    // while it is working. A device-status event used to be the only way out, so an arm that failed
    // because the sink had not been built yet (a bind before CAkSinkXAudio2::Init, or the gap
    // behind a wakeup) stayed failed for the whole session.
    //
    // Deliberately unthrottled. It costs a handful of module and page queries per frame IN THE
    // DEGRADED STATE ONLY, and the alternative is worse than the cost: a rate is measured over the
    // window that begins the moment it is commanded, so a re-arm one poll interval late is a slope
    // that reads the average of two rates and fails a mechanism that works.
    void ReArm() {
        if (suspended_ || ak_.GetAudioSettings == nullptr) {
            return;   // no sink is the CORRECT state of a suspended engine, not a fault to retry
        }
        // ★ THROTTLED, at a frame-rate-independent 4 Hz. Unthrottled it ran a full ProbeSinks with
        // VirtualQuery page proofs on the game thread EVERY frame while degraded, and each success
        // re-planned and re-commanded, which is the other half of the flap. 250 ms is two orders of
        // magnitude inside the window a rate is measured over, so a re-arm is never late enough to
        // matter to the measurement the old comment here was protecting.
        if (nowMs_ < reArmNextMs_) {
            return;
        }
        reArmNextMs_ = nowMs_ + ReArmPeriodMs();
        if (!Arm()) {
            ++armFailStreak_;
            // Only a RUN of failures may change what the panel says, and then only downwards. One
            // frame's probe miss is not a mode.
            if (armFailures_ < kArmFailuresBeforeDowngrade) {
                ++armFailures_;
                if (armFailures_ == kArmFailuresBeforeDowngrade) {
                    DowngradeMode();
                    // ★ AND IT SAYS SO, ONCE. This state used to be entirely silent: the owner's
                    // log holds 2 m 26 s of a degraded module with not one [audio] line in it,
                    // which is why the choppy burst could only be correlated, never attributed.
                    Notice("audio rate control degraded: %s -- retrying, backing off to %llu ms",
                           diagnostic_[0] != '\0' ? diagnostic_ : "no sink",
                           (unsigned long long)kReArmCeilingMs);
                }
            }
            return;
        }
        if (armFailStreak_ >= kArmFailuresBeforeDowngrade) {
            Notice("audio rate control recovered after %u failed sink probe(s)", armFailStreak_);
        }
        armFailStreak_ = 0;
        armFailures_ = 0;
        // The replan and the command are NOT here. Arm() has set the delivery flag, and Tick runs
        // DeliverRecordedIntent one statement later -- in this same tick, so nothing is slower.
        // Two paths that each replanned and commanded for themselves is how a recovery wrote the
        // sink twice; and Wakeup and the device-status callback, which arm through Arm() without
        // coming through here, had no such path at all.
    }

    // ★ AND IT BACKS OFF. 4 Hz is right for the transient behind a wakeup; a sink that has refused
    // twenty times running is not going to be there on the next frame, and every probe walks the
    // sink list with a VirtualQuery page proof per node, on the game thread, inside whatever hitch
    // is already running. Doubling from 250 ms to a 4 s ceiling costs a genuinely-returning sink at
    // most one ceiling period of silence and costs a genuinely-dead one nothing at all.
    uint64_t ReArmPeriodMs() const {
        uint64_t period = kReArmPeriodMs;
        for (uint32_t i = kArmFailuresBeforeDowngrade;
             i < armFailStreak_ && period < kReArmCeilingMs; ++i) {
            period *= 2;
        }
        return period > kReArmCeilingMs ? kReArmCeilingMs : period;
    }

    // The one way the latched label may change between rate changes, and it only ever goes
    // downwards: a sink that has refused kArmFailuresBeforeDowngrade times running is not going to
    // serve this rate, and saying so once beats saying it every other frame.
    void DowngradeMode() {
        if (labelPlan_.mode == RateMode::Unavailable) {
            return;
        }
        RatePlan degraded;
        degraded.mode = RateMode::Unavailable;
        degraded.reversed = labelPlan_.reversed;
        labelPlan_ = degraded;
    }

    void RebaseClock() { lock_.Reprime(); }

    // Giving the sink back. The game keeps playing after the replay module unloads and it should
    // keep playing at its own pitch, so the last thing this module does is put every voice it
    // commanded back to the ratio the engine had. Nothing else will: the ratio lives in XAudio2,
    // not in us, and a voice left at 0.5x outlives the module that set it.
    void Release() {
        if (!suspended_) {
            int32_t firstError = 0;
            RestoreFrequencyRatio(ak_, &firstError);
        }
        // ★ AND THE RECORDS OF THE VOICES IT COULD NOT REACH SURVIVE IT. RestoreChainRatios retires
        // what it put back; clearing the rest here destroyed the only thing in the process that
        // knew what a voice still holding `baseline * 0.1` was supposed to hold -- on the suspended
        // path, where nothing was restored at all, that was every voice. A record of a voice Wwise
        // genuinely rebuilt is retired by RetainOnly on the generation change, not by this.
        available_ = false;
        reassertNextMs_ = 0;
    }

    void ResetForTest() {
        SinkRatioLedger().Reset();
        ak_ = AkExports{};
        cfg_ = AudioRateConfig{};
        settings_ = AkAudioSettings{};
        ResetBias();
        lock_.Reset();
        plan_ = RatePlan{};
        labelPlan_ = RatePlan{};
        labelUpgraded_ = false;
        nowMs_ = 0;
        reArmNextMs_ = 0;
        reassertNextMs_ = 0;
        armFailures_ = 0;
        armFailStreak_ = 0;
        biasWarmupStartMs_ = 0;
        biasRetryNextMs_ = 0;
        request_ = RateCommand{};
        lastCommanded_ = 1.0f;
        lastIssued_ = kShippedFrequencyRatio;
        issuedAtMs_ = 0;
        deliverOnArm_ = false;
        driftNextMs_ = 0;
        driftSaidNextMs_ = 0;
        sinkCount_ = 0;
        available_ = false;
        suspended_ = false;
        deviceDirty_.store(false);
        diagnostic_[0] = 0;
        lastTick_ = 0.0;
        lastSamples_ = 0;
        oraclePrimed_ = false;
    }

    bool SampleOracle(double dtWall, RateSample* out) {
        if (out == nullptr) {
            return false;
        }
        *out = RateSample{};
        AkAudioSettings now{};
        if (ak_.GetAudioSettings == nullptr || ak_.GetAudioSettings(&now) != kAkSuccess ||
            now.samplesPerSecond == 0) {
            return false;
        }
        out->settingsValid = true;

        const double tick = ak_.GetBufferTick != nullptr ? ak_.GetBufferTick() : 0.0;
        uint64_t samples = 0;
        if (!ReadChainSamples(&samples, nullptr, nullptr)) {
            return false;
        }
        if (oraclePrimed_ && dtWall > 0.0) {
            // Neither source is a clock. SamplesPlayed restarts when a voice is recreated and
            // GetBufferTick is a u32 that wraps; a raw subtraction across either is a vast number
            // that reads as "the ratio did not take". Refuse the sample instead of judging it.
            const double nowSamples = static_cast<double>(samples);
            double dTick = 0.0;
            double dSamples = 0.0;
            if (!StepForward(lastTick_, tick, &dTick) ||
                !StepForward(lastSamples_, nowSamples, &dSamples)) {
                out->sourceRegressed = true;
            } else {
                const double framesPerSecond =
                    static_cast<double>(now.samplesPerSecond) / now.samplesPerFrame;
                out->wwiseRatio = dTick / dtWall / framesPerSecond;
                out->deviceRatio = dSamples / dtWall / now.samplesPerSecond;
            }
        }
        lastTick_ = tick;
        lastSamples_ = static_cast<double>(samples);
        oraclePrimed_ = true;
        return true;
    }

private:
    // ★ 1.0x IS AN ABSOLUTE RESTORE, NOT A MULTIPLY BY ONE. `Command(1.0f)` writes
    // `baseline * 1.0` -- it re-asserts whatever the ledger currently calls the baseline and puts
    // nothing back. When a transient probe refusal had re-based that belief onto our own slowed
    // value, every later "1.0x" wrote 0.25 with RateMode::Normal latched and a log line saying the
    // ratio took. Restoring writes the engine's own float, bit for bit, so the sequence that got
    // there cannot matter.
    // ★ AN INTENT IS DELIVERED WHEN IT REACHED A SINK, AND NOT WHEN IT WAS MERELY ISSUED. This
    // used to record the ask itself, so a write that reached ZERO voices -- a ProbeSinks that
    // missed inside a device change, a sink list mid-rebuild -- satisfied ReassertIntent's guard
    // and was never retried. That is the owner's "sometimes it comes back to 1x and sometimes it
    // doesn't": the mix stays at 0.1x with the panel reading 1.0x until the speed is touched again.
    // Per voice, because a chain where one sink took the write and another refused is still
    // outstanding for the one that refused.
    void MarkDelivered(float ratio, bool reached) {
        if (reached && OutstandingVoices(SinkRatioLedger(), ratio) == 0) {
            lastCommanded_ = ratio;
        }
    }

    // What this plan wants the voices to hold. RateMode::Normal carries ratio 1.0 like the rest,
    // but it is the one mode whose 1.0 means "the pitch the engine chose" rather than a number to
    // compose, so the two are kept apart at the one place that has to tell them apart.
    float PlannedRatio() const {
        return plan_.mode == RateMode::Normal ? 1.0f : plan_.ratio;
    }

    void CommandPlanned() {
        // ★ AND FAST-FORWARD RESTORES ABSOLUTELY TOO (PITCH-FIX-2 S6). Above what the sink can
        // pitch, PlanRate sets ratio 1.0 with mode ClampedFast -- and that went down the composing
        // path, which wrote `baseline * 1.0`. On 2026-09-20 that was 0.0373 under a panel reading
        // "audio stays at 1.0x". Any plan whose intended pitch is 1.0 is a restore, whatever the
        // mode is called; only a plan that really wants a different pitch composes.
        if (NearlyEqualRatio(PlannedRatio(), 1.0f)) {
            // ★ AND 1.0x WITH NOTHING OUTSTANDING WRITES NOTHING AT ALL. RestoreBaselines puts back
            // what we pitched; when we have pitched nothing there is nothing to put back, and
            // commanding `baseline * 1.0` would be this plugin writing a ratio into the game's own
            // live mix for no reason. The cutoff review's rule: at 1x, with nothing pinned,
            // src/audio writes nothing.
            // A restore that put nothing back because there was nothing of ours to put back is
            // delivered; one that put nothing back because the probe found no sink is not.
            NoteIntentIssued(1.0f);
            MarkDelivered(1.0f, RestoreBaselines() || SinkRatioLedger().Count() == 0);
            return;
        }
        Command(plan_.ratio);
    }

    // ★ THE RECORDED INTENT, DELIVERED THE MOMENT THERE IS SOMETHING TO DELIVER IT TO
    // (AUDIO-FIX-3 B). Replanned from the transport's own last request, never replayed from
    // `plan_`: a plan built while the module was unavailable carries RateMode::Unavailable's
    // ratio 1.0, and commanding that is how a 0.10x survived the round trip as silence at 1x.
    void DeliverRecordedIntent() {
        RateCommand cmd = request_;
        cmd.available = true;
        cmd.maxRate = MaxRate();
        const RatePlan planned = PlanRate(cmd);
        if (planned.suspend) {
            Suspend(true);
            LatchLabel(planned);
            return;
        }
        // The single UPGRADE this design allows: the rate was latched while there was no sink, and
        // now there is one. Once per rate -- after that the label holds whatever it holds. Not
        // LatchLabel(), which clears that latch and would let the label walk once per re-arm.
        const bool mayUpgrade = !labelUpgraded_ && labelPlan_.mode == RateMode::Unavailable;
        plan_ = planned;
        // Deliberately NOT lock_.Reset(): resetting restarts the trim ramp, and a ramp that
        // restarts on every recovery is the loop that railed.
        CommandPlanned();
        if (!mayUpgrade) {
            return;
        }
        labelPlan_ = plan_;
        labelUpgraded_ = true;
        if (!NearlyEqualRatio(PlannedRatio(), kShippedFrequencyRatio)) {
            Notice("audio rate control available again: the %.2fx recorded while there was no sink "
                   "is applied now -- %s",
                   static_cast<double>(request_.rate), RateLabel());
        }
    }

    // False when nothing of ours is outstanding -- an untracked voice has nothing to put back, and
    // `Command` owns the availability accounting for that case exactly as it always has.
    bool RestoreBaselines() {
        if (suspended_) {
            return false;
        }
        int32_t firstError = 0;
        return g_restoreRatio(ak_, &firstError, /*forget=*/false) != 0;
    }

    void Command(float ratio) {
        if (suspended_) {
            return;   // there is no sink to command, and its absence is not evidence about one
        }
        NoteIntentIssued(ratio);
        int32_t firstError = 0;
        size_t gone = 0;
        const size_t applied = g_applyRatio(ak_, ratio, &firstError, &gone);
        MarkDelivered(ratio, applied > 0);
        // Not a refusal: the product fell under the device's floor and every voice was clamped up
        // to it. The rate still plays, as slowly as this device can, and the label says which.
        floorClamped_ = applied > 0 && firstError == kRatioBelowDeviceMinimum;
        if (applied == 0) {
            // A voice that went away and a device that said no are different findings, and the
            // rows print this reason verbatim: reporting a refusal that never happened is what
            // sends the next reader looking for an HRESULT nobody returned.
            if (gone > 0) {
                ReportVoicesGone(gone);
            } else {
                available_ = false;
                SetDiagnostic("SetFrequencyRatio was refused by every sink");
            }
        }
    }

    // The voice stopped being the one that was proven, under a use. Unlatching is what makes the
    // next tick re-probe instead of commanding a dead object, and the word is the verdict set's
    // own -- W2-R's rows print these strings.
    void ReportVoicesGone(size_t gone) {
        char detail[192];
        snprintf(detail, sizeof detail, "%s (%zu of the proven voices)",
                 SinkVerdictName(SinkVerdict::VoiceGone), gone);
        SetDiagnostic(detail);
        available_ = false;
    }

    // Through the probe seam, not through ReadSamplesPlayed's own probe: the availability state
    // machine is the part of S10.4 that only a launch has ever exercised, and a read that reached
    // past the seam could not be driven on a machine with no game.
    bool ReadChainSamples(uint64_t* samples, uint32_t* queued, size_t* gone) {
        SinkChain chain;
        if (g_probeSinks(ak_, &chain) == 0) {
            return false;
        }
        return ReadChainSamplesPlayed(chain, samples, queued, gone);
    }

    // * A DERIVED BOUND IN WALL TIME, NOT IN TICKS. The warm-up needs AvBias::kWarmupSamples
    // observations and consumes at most one per tick, so a warm-up that is progressing latches in
    // that many ticks whatever the frame rate. The BUDGET is the part that must not be a tick count:
    // 128 ticks was 4.3 s at the fixed 30 Hz step and would be 0.9 s at 144, quietly giving a slow
    // sink a third of the time to come good. Four warm-up lengths at 30 Hz, stated as the seconds
    // they were, leaves room for three complete restarts -- AvBias::Observe throws the count away
    // when the whole warm-up read a zero queue depth -- after which the sink is not warming up
    // slowly, it is not warming up.
    // Past the budget the read still happens, once per warm-up length. Stopping outright would mean
    // a sink that comes back after a long rebuild never gets a bias at all.
    bool BiasWarmupDue() {
        if (nowMs_ < biasWarmupStartMs_ + AvBias::kBiasWarmupBudgetMs) {
            return true;
        }
        if (nowMs_ < biasRetryNextMs_) {
            return false;
        }
        biasRetryNextMs_ = nowMs_ + AvBias::kBiasWarmupRetryMs;
        return true;
    }

    void ResetBias() {
        bias_.Reset();
        biasWarmupStartMs_ = nowMs_;
        biasRetryNextMs_ = nowMs_;
    }

    void Notice(const char* fmt, ...) {
        if (g_notice[0] != '\0') {
            ++g_noticesSuperseded;
        }
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g_notice, sizeof g_notice, fmt, ap);
        va_end(ap);
    }

    void SetDiagnostic(const char* why) {
        if (why == nullptr) {
            diagnostic_[0] = '\0';
            return;
        }
        snprintf(diagnostic_, sizeof diagnostic_, "%s", why);
    }

    AkExports ak_{};
    AudioRateConfig cfg_{};
    AkAudioSettings settings_{};
    AvBias bias_;
    // When the current warm-up began, in the same wall milliseconds the re-arm throttle counts. The
    // budget is re-armed only by a reset, so a sink that flaps cannot buy itself a fresh budget
    // every device event it causes.
    uint64_t biasWarmupStartMs_ = 0;
    uint64_t biasRetryNextMs_ = 0;
    RateLock lock_;
    RatePlan plan_{};
    // ★ WHAT THE PANEL SAYS. Decided at a rate change and changed at most once more per rate, so a
    // probe that misses for a frame cannot rewrite the sentence a viewer is reading.
    RatePlan labelPlan_{};
    bool labelUpgraded_ = false;
    // Wall milliseconds accumulated from the tick's own dt -- never a frame count. The re-arm
    // cadence has to be the same 4 Hz at 30 fps and at 144.
    uint64_t nowMs_ = 0;
    uint64_t reArmNextMs_ = 0;
    uint32_t armFailures_ = 0;
    // Uncapped, unlike armFailures_, because it drives the backoff rather than the panel string.
    uint32_t armFailStreak_ = 0;
    // The undelivered-intent retry, at the same 4 Hz and for the same reason.
    uint64_t reassertNextMs_ = 0;
    static constexpr uint64_t kReassertRetryMs = 250;
    static constexpr uint64_t kReArmPeriodMs = 250;
    static constexpr uint64_t kReArmCeilingMs = 4000;
    static constexpr uint32_t kArmFailuresBeforeDowngrade = 3;
    // What the transport last asked for, kept apart from the plan because a plan made while the
    // sink was missing does not remember the rate it could not serve.
    RateCommand request_{};
    float lastCommanded_ = 1.0f;
    // Every voice the last command reached was clamped up to the device's own floor. Not a failure
    // -- the rate plays, at the slowest pitch this device has -- and the label has to say so.
    bool floorClamped_ = false;
    // The drift reconciler and the rail gate. Both are wall-millisecond deadlines off the tick's own
    // dt, for the same reason the re-arm throttle is: a frame count would change cadence with fps.
    uint64_t driftNextMs_ = 0;
    uint64_t driftSaidNextMs_ = 0;
    // The last value this module started asking the voices for, and when. A drift check inside
    // kRatioReadLagMs of a CHANGE is reading the device's previous pass, not the voice.
    float lastIssued_ = kShippedFrequencyRatio;
    uint64_t issuedAtMs_ = 0;
    bool deliverOnArm_ = false;
    uint64_t railSinceMs_ = 0;
    bool loopDisengaged_ = false;
    static constexpr uint64_t kDriftCheckPeriodMs = kRatioDriftCheckMs;
    static constexpr uint64_t kDriftSayEveryMs = 10000;
    static constexpr uint64_t kRailGiveUpMs = 3000;
    size_t sinkCount_ = 0;
    bool available_ = false;
    // Set on Wwise's device-status thread, consumed on the game thread. Relaxed is enough:
    // the only thing it orders is "re-probe soon", and a re-probe one frame late is correct.
    std::atomic<bool> deviceDirty_{false};
    bool suspended_ = false;
    char diagnostic_[192] = {0};

    double lastTick_ = 0.0;
    // A double, not a u64, so the regression check is one comparison in one domain. SamplesPlayed
    // stays exact in a double past any session length: 2^53 samples is about 5900 years at 48 kHz.
    double lastSamples_ = 0.0;
    bool oraclePrimed_ = false;
};

AudioRateBinding g_rate;
bool g_ready = false;

// AK_AudioDeviceEvent_Removal / _Initialization both arrive here. The event code is not inspected:
// every one of them means the sink list may have been rebuilt, and re-probing is cheap while
// guessing which events matter is a list that goes stale.
void __cdecl OnAudioDeviceStatus(void*, uint32_t, uint32_t, int32_t, int32_t) {
    g_rate.MarkDeviceChanged();
}

}  // namespace

IAudioRate* AudioRate() { return g_ready ? &g_rate : nullptr; }

bool DrainAudioRateNotice(char* out, size_t cap, uint32_t* superseded) {
    if (g_notice[0] == '\0') {
        return false;
    }
    if (out != nullptr && cap > 0) {
        snprintf(out, cap, "%s", g_notice);
    }
    if (superseded != nullptr) {
        *superseded = g_noticesSuperseded;
    }
    g_notice[0] = '\0';
    g_noticesSuperseded = 0;
    return true;
}

bool InitAudioRate(const AkExports& ak, const AudioRateConfig& cfg) {
    // ★ Ready even when the proofs refuse. IAudioRate is how every consumer ASKS whether audio rate
    // control exists, so withholding the pointer would turn a reported degradation into a null that
    // nobody can explain. Available() and Diagnostic() carry the answer.
    const bool ok = g_rate.Init(ak, cfg);
    if (ak.RegisterAudioDeviceStatusCallback != nullptr) {
        ak.RegisterAudioDeviceStatusCallback(&OnAudioDeviceStatus);
    }
    g_ready = true;
    return ok;
}

void TickAudioRate(double sceneSeconds, double dtWall) {
    if (g_ready) {
        g_rate.Tick(sceneSeconds, dtWall);
    }
}

void MarkAudioClockDiscontinuity() {
    if (g_ready) {
        g_rate.RebaseClock();
    }
}

bool SampleRateOracle(double dtWall, RateSample* out) {
    return g_ready && g_rate.SampleOracle(dtWall, out);
}

void ShutdownAudioRate() {
    if (!g_ready) {
        return;
    }
    g_rate.Release();
    g_ready = false;
}

namespace {
LifetimeArm g_lifetimeArm = LifetimeArm::NeverAttempted;
}  // namespace

void SetVoiceLifetimeArm(LifetimeArm arm) { g_lifetimeArm = arm; }

SinkAvailability SinkAvailabilityState() {
    if (g_lifetimeArm != LifetimeArm::Armed) {
        return SinkAvailability::CouldNotAsk;
    }
    if (!g_ready) {
        return SinkAvailability::CouldNotAsk;
    }
    return g_rate.Available() ? SinkAvailability::Available : SinkAvailability::Unavailable;
}

// One fixed sentence per state, and no formatting: the WHY of a could-not-ask is
// LifetimeArmName(VoiceLifetimeState()), which the module logs on its own line. Two channels for
// two facts -- what state we are in, and what put us there -- rather than one string that has to
// be parsed to recover either.
const char* SinkAvailabilityLine() {
    switch (SinkAvailabilityState()) {
        case SinkAvailability::Available:
            return "sink availability: available";
        case SinkAvailability::Unavailable:
            return g_ready ? g_rate.Diagnostic() : "audio rate control was never initialised";
        case SinkAvailability::CouldNotAsk:
            break;
    }
    return "sink availability: could-not-ask -- ledger row 2.6 (audio.sink_term_hook) is not armed, "
           "so nothing announces a voice teardown on the paths the stock device-status callback "
           "misses, and this module makes no claim about the sink and reports no rate from it";
}

void ResetAudioRateForTest() {
    g_rate.ResetForTest();
    g_ready = false;
    g_lifetimeArm = LifetimeArm::NeverAttempted;
}

void SetSinkAccessForTest(size_t (*probe)(const AkExports&, SinkChain*),
                          size_t (*apply)(const AkExports&, float, int32_t*, size_t*),
                          size_t (*restore)(const AkExports&, int32_t*, bool)) {
    g_probeSinks = probe != nullptr ? probe : &ProbeSinks;
    g_applyRatio = apply != nullptr ? apply : &ApplyFrequencyRatio;
    g_restoreRatio = restore != nullptr ? restore : &ShippedRestore;
}

}  // namespace audio
}  // namespace replay
