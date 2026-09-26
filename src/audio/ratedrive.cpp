#include "audio/ratedrive.h"

#include <cmath>
#include <stdio.h>

#include "audio/audiorate.h"
#include "audio/sinkvoice.h"
#include "bootstrap/services.h"
#include "engine/audiorate.h"
#include "playback/core/transport.h"

namespace replay {
namespace audio {

namespace {

const char* const kModule = "audio";

void Log(ILog* log, LogLevel level, const char* fmt, float rate, const char* label) {
    if (log != nullptr) {
        log->Write(level, kModule, fmt, static_cast<double>(rate), label);
    }
}

// The label, and -- when there is one -- the refusal that produced it. RateLabel only ever says
// that a rate is unavailable; which proof refused lives in Diagnostic, and a line that carries
// the word without the reason is why six launches each re-derived this module from source.
void DescribeAudio(IAudioRate* audio, char* out, size_t size) {
    const char* why = audio->Available() ? nullptr : audio->Diagnostic();
    if (why == nullptr || why[0] == '\0') {
        snprintf(out, size, "%s", audio->RateLabel());
        return;
    }
    snprintf(out, size, "%s -- %s", audio->RateLabel(), why);
}

}  // namespace

void RateDriver::Drive(const IReplayTransport* transport, IAudioRate* audio, ISeekMute* seekMute,
                       ILog* log) {
    if (audio == nullptr) {
        return;
    }
    const bool held = seekMute != nullptr && seekMute->Held();
    // A loaded replay has a duration; "no file" reports 0 and State() reports Pause, and treating
    // that Pause as a command would silence the menu the moment this module ticked.
    const bool engaged = transport != nullptr && transport->DurationUs() > 0;
    if (!engaged) {
        if (engaged_) {
            engaged_ = false;
            paused_ = false;
            badRate_ = false;
            rate_ = 1.0f;
            if (held) {
                // The hold was taken for a replay that no longer exists. Deferring the hand-back
                // here would strand the sink suspended for the rest of the session -- nothing
                // fires a discontinuity once there is no replay to seek in, so no release would
                // ever come -- so the owner drops the session's holds and the sink comes back.
                seekMute->ReleaseOnUnload();
                ++stats_.unloadMutesDropped;
            }
            audio->SetRate(1.0f);   // 1.0x, and wakes the sink if anything still had it suspended
            ++stats_.commands;
            ++stats_.releases;
            Log(log, LogLevel::Info,
                held ? "replay unloaded during a held seek mute: the hold is dropped and audio is "
                       "handed back at %.2fx (%s)"
                     : "replay unloaded: audio handed back at %.2fx (%s)",
                1.0f, audio->RateLabel());
            ReportSinkRatioWrites(log);
        }
        return;
    }

    // Drained on EVERY tick, above the no-change early-out: the two things the rate module reports
    // through this channel -- a voice found off the ratio we put it at, and the closed loop giving
    // up at its rail -- both happen while the transport is not changing, which is the one path the
    // edge-driven half of this function never reaches.
    ReportRateNotice(log);

    const bool paused = transport->State() == TransportState::Pause;
    const float rate = transport->Rate();
    if (engaged_ && paused == paused_ && rate == rate_) {
        return;
    }
    if (held) {
        ++stats_.deferred;   // the hold owns the sink until the pre-roll lets go; re-read next tick
        return;
    }
    if (!paused && !std::isfinite(rate)) {
        // Commanding it would push a garbage pitch into Wwise, and `rate != rate_` stays true
        // forever for a NaN, so an unguarded driver re-commands and re-logs on every single tick.
        // The sink keeps the last rate it was given; the count says how long that lasted.
        ++stats_.nonFinite;
        if (!badRate_) {
            badRate_ = true;
            Log(log, LogLevel::Warn,
                "transport rate is not a finite number: audio holds its last command at %.2fx (%s)",
                rate_, audio->RateLabel());
        }
        return;
    }
    badRate_ = false;

    const bool alreadySuspended = engaged_ && paused_;
    engaged_ = true;
    paused_ = paused;
    rate_ = rate;

    if (paused) {
        if (alreadySuspended) {
            // Already suspended: re-issuing Suspend changes nothing at the sink and inflates a
            // counter that is read as launch evidence. The new rate is what it resumes at, and the
            // audio module is not told until then -- a suspended engine has no sink to command and
            // SetRate's first act is a wakeup. The only thing missing was the sentence: the owner
            // set 0.10x while paused, saw `audio rate control unavailable` on the resume tick, and
            // had no way to tell which of the two moments the module was talking about.
            Log(log, LogLevel::Info,
                "transport rate %.2fx while paused: audio holds its suspension and takes this rate "
                "on resume (%s)",
                rate, audio->RateLabel());
            return;
        }
        audio->Suspend(true);
        ++stats_.commands;
        ++stats_.suspends;
        char detail[256];
        DescribeAudio(audio, detail, sizeof(detail));
        Log(log, LogLevel::Info, "transport paused: audio suspended at %.2fx (%s)", rate, detail);
        return;
    }
    audio->SetRate(rate);
    ++stats_.commands;
    const float speed = rate < 0.0f ? -rate : rate;
    if (audio->Available() && speed > audio->MaxRate()) {
        ++stats_.clamps;
        // ★ THE LABEL MAY NOT CLAIM 1.0x WHILE A VOICE IS OFF IT. On 2026-09-20 this sentence sat
        // above a write of 0.0373. It is now two sentences: why the pitch cannot follow the
        // transport (the voices are created with MaxFrequencyRatio = 1.0, so they have no headroom
        // at all), and whether the restore to that 1.0 has actually landed on every voice yet.
        const size_t outstanding = OutstandingVoices(SinkRatioLedger(), 1.0f);
        if (outstanding == 0) {
            Log(log, LogLevel::Warn,
                "transport rate %.2fx is above what the sink can pitch -- the game creates its sink "
                "voices with MaxFrequencyRatio 1.0, so audio stays at 1.0x and is desynced from the "
                "picture (%s)",
                rate, audio->RateLabel());
        } else if (log != nullptr) {
            log->Write(LogLevel::Warn, kModule,
                       "transport rate %.2fx is above what the sink can pitch -- audio is being "
                       "put back to 1.0x and %zu voice(s) are not there yet (%s)",
                       static_cast<double>(rate), outstanding, audio->RateLabel());
        }
        // The clamp still commanded 1.0x at the sinks, and a record left undrained here would be
        // printed against the NEXT rate change as though that change had written it.
        ReportSinkAdoptions(log);
        ReportSinkRatioWrites(log);
        return;
    }
    char detail[256];
    DescribeAudio(audio, detail, sizeof(detail));
    Log(log, LogLevel::Info, "transport rate %.2fx: %s", rate, detail);
    ReportSinkAdoptions(log);
    ReportSinkRatioWrites(log);
}

// Its own line per voice, not appended to the rate line above: the label on that line is parsed by
// name elsewhere. The numbers that actually reached the sinks are what no log carried -- "audio:
// 1.0x, as the game mixed it" read identically whether the voice went to 1.0 or stayed at 0.25,
// which is why the stuck slow-mo took a launch and a source re-derivation to find.
void RateDriver::ReportSinkRatioWrites(ILog* log) {
    static_assert(kMaxSinks == SinkChain::kMax, "one report slot per sink the chain can hold");
    SinkRatioWrite writes[kMaxSinks];
    SinkRatioWriteCounts counts;
    // Drained even with no log to print to: records left standing would be reported against a
    // later command as if they had just been written.
    const size_t n = DrainSinkRatioWrites(writes, kMaxSinks, &counts);
    if (log == nullptr) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const SinkRatioWrite& w = writes[i];
        if (!AdoptSinkRatioReport(w)) {
            ++stats_.ratioLinesRepeated;
            continue;
        }
        log->Write(LogLevel::Info, kModule,
                   "sink ratio: sink %p rate %.4f x baseline %.4f -> wrote %.4f, voice reads %.4f",
                   reinterpret_cast<void*>(w.sink), static_cast<double>(w.rate),
                   static_cast<double>(w.baseline), static_cast<double>(w.written),
                   static_cast<double>(w.readBack));
    }
    if (counts.superseded != 0 || counts.dropped != 0) {
        log->Write(LogLevel::Info, kModule,
                   "sink ratio: %u write(s) superseded and %u dropped before this report -- the "
                   "sinks were commanded more often than the rate changed",
                   counts.superseded, counts.dropped);
    }
    char census[256];
    FormatSinkRatioCensus(census, sizeof census, SinkRatioCensusNow(SinkRatioLedger(), rate_));
    log->Write(LogLevel::Info, kModule, "%s", census);
}

// One line per voice the ledger has met, and it answers a question no earlier log could: WHICH
// outputs we are pitching. If a census comes back clean and the owner still hears slow audio, the
// remaining hypothesis is that the audible device is not among these, and this is the line that
// settles it.
void RateDriver::ReportSinkAdoptions(ILog* log) {
    SinkAdoptNotice notices[kMaxAdoptNotices];
    const size_t n = DrainSinkAdoptNotices(notices, kMaxAdoptNotices);
    if (log == nullptr) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const SinkAdoptNotice& a = notices[i];
        log->Write(LogLevel::Info, kModule,
                   "sink adopted: sink %p voice %p output index %u, %u ch @ %u Hz, baseline %.4f "
                   "(the ratio the engine creates this voice with), max ratio %.4f",
                   reinterpret_cast<void*>(a.sink), a.voice, a.outputIndex, a.channels,
                   a.sampleRate, static_cast<double>(kShippedFrequencyRatio),
                   static_cast<double>(a.maxRatio));
    }
}

void RateDriver::ReportRateNotice(ILog* log) {
    char line[256];
    uint32_t superseded = 0;
    if (!DrainAudioRateNotice(line, sizeof line, &superseded)) {
        return;
    }
    ++stats_.notices;
    if (log == nullptr) {
        return;
    }
    if (superseded == 0) {
        log->Write(LogLevel::Warn, kModule, "%s", line);
        return;
    }
    log->Write(LogLevel::Warn, kModule, "%s (%u earlier notice(s) superseded)", line, superseded);
}

bool RateDriver::AdoptSinkRatioReport(const SinkRatioWrite& write) {
    ReportedRatio* spare = nullptr;
    for (ReportedRatio& r : reported_) {
        if (!r.valid) {
            if (spare == nullptr) {
                spare = &r;
            }
            continue;
        }
        if (r.sink != write.sink) {
            continue;
        }
        // The module's own tolerance, not equality: a read-back that differs in the last bits is
        // the same value, and printing it again would be a line about float storage.
        if (NearlyEqualRatio(r.rate, write.rate) && NearlyEqualRatio(r.baseline, write.baseline) &&
            NearlyEqualRatio(r.written, write.written) &&
            NearlyEqualRatio(r.readBack, write.readBack)) {
            return false;
        }
        r = ReportedRatio{write.sink, write.rate, write.baseline, write.written, write.readBack,
                          true};
        return true;
    }
    if (spare != nullptr) {
        *spare = ReportedRatio{write.sink, write.rate, write.baseline, write.written,
                               write.readBack, true};
    }
    return true;
}

void RateDriver::Reset() {
    engaged_ = false;
    paused_ = false;
    badRate_ = false;
    rate_ = 1.0f;
    stats_ = RateDriveStats{};
    for (ReportedRatio& r : reported_) {
        r = ReportedRatio{};
    }
}

}  // namespace audio
}  // namespace replay
