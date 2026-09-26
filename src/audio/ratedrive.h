#pragma once

// The transport -> IAudioRate link of S10.4.4. Nothing else commands the audio rate: the transport is
// the single source of truth for pause and rate, and this driver mirrors it into Wwise every tick
// the same way engine/clock.cpp mirrors it into the clock. Read every tick, commanded on change.

#include <stddef.h>
#include <stdint.h>

namespace replay {

class IAudioRate;
class ILog;
class IReplayTransport;

namespace audio {

struct SinkRatioWrite;

struct RateDriveStats {
    uint32_t commands = 0;   // SetRate or Suspend calls actually issued
    uint32_t suspends = 0;
    uint32_t clamps = 0;     // |r| above MaxRate(): the sink stays at 1.0x pitch and the log says so
    uint32_t releases = 0;   // the replay went away: audio handed back at 1.0x
    uint32_t deferred = 0;   // a change that waited behind a held seek mute
    uint32_t nonFinite = 0;  // ticks whose Rate() was NaN or infinite: nothing was commanded off it
    uint32_t unloadMutesDropped = 0;   // unloads that arrived while a seek mute was still held
    // Sink-ratio writes that repeated the line already printed for that voice. One line per
    // transition per voice, and this is how many ticks that rule swallowed.
    uint32_t ratioLinesRepeated = 0;
    // Lines the rate module asked for through its notice channel: a drift correction, or the closed
    // loop giving up at its rail.
    uint32_t notices = 0;
};

// The seek mute as the driver sees it. audioresidue.cpp owns the fact and implements this: the
// driver READS the hold and REPORTS the unload, and never sets the hold itself, so there is exactly
// one place that can decide the sink is being held.
class ISeekMute {
public:
    virtual ~ISeekMute() = default;
    // Any holder standing: while this is true the sink belongs to the pre-roll, not to the driver.
    virtual bool Held() const = 0;
    // The replay went away, so every hold taken for it dies with it and the sink comes back awake.
    virtual void ReleaseOnUnload() = 0;
};

class RateDriver {
public:
    // `transport` may be null (playback not initialised yet) and may become null again; `audio`
    // null means nothing is driven; `seekMute` null means nothing is ever held. A change that
    // arrives during a held mute waits, because waking the sink is exactly what the hold forbids.
    void Drive(const IReplayTransport* transport, IAudioRate* audio, ISeekMute* seekMute, ILog* log);

    const RateDriveStats& Stats() const { return stats_; }
    void Reset();

private:
    // Drains the per-sink ratio writes the command just made and prints one line for each voice
    // whose numbers are not the ones already reported for it.
    void ReportSinkRatioWrites(ILog* log);
    void ReportSinkAdoptions(ILog* log);
    // Drains the rate module's one-line notice channel, every tick, above the no-change early-out.
    void ReportRateNotice(ILog* log);
    // False when this write says exactly what the last line for that sink said; adopts it
    // otherwise. A sink there is no slot for is always reported rather than silenced.
    bool AdoptSinkRatioReport(const SinkRatioWrite& write);

    bool engaged_ = false;   // a replay with a duration is loaded and the sink follows it
    bool paused_ = false;
    bool badRate_ = false;   // the last Rate() was not finite: one line per transition, not per tick
    float rate_ = 1.0f;
    RateDriveStats stats_;

    // What the last sink-ratio line said, per sink voice. Fixed capacity, checked against the sink
    // chain's own bound where it is used; keyed by sink id, so a chain of several voices cannot
    // report one of them and call it the chain.
    static constexpr size_t kMaxSinks = 8;
    struct ReportedRatio {
        uintptr_t sink = 0;
        float rate = 0.0f;
        float baseline = 0.0f;
        float written = 0.0f;
        float readBack = 0.0f;
        bool valid = false;
    };
    ReportedRatio reported_[kMaxSinks];
};

}  // namespace audio
}  // namespace replay
