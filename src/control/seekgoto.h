#pragma once

// W2-G, ARCH-17 S8. The two decisions behind `seek +<t>` / `seek -<t>` and `goto next` / `goto
// prev`, written as pure functions for the same reason loadgate.h and launchgate.h are:
// `tests/control` links `replay_control_core`, which does NOT carry dispatch.cpp, so a decision
// left inside a dispatcher arm is a decision the offline suite cannot sweep. The arms stay thin.

#include <stddef.h>
#include <stdint.h>

#include "playback/core/transport.h"

namespace replay {
namespace control {

// THREE ANSWERS, NOT TWO. "landed where you asked", "landed short because the file ends there" and
// "this capture has no range to move inside" are three different things, and a bool would make the
// last two indistinguishable from the first. S8.1 rules that a relative seek CLAMPS -- `-10s` at
// t=3s means the start, exactly as the panel's nudge buttons behave -- but a clamp the reply does
// not name is a silent discontinuity, so the caller is told which of the three happened and says
// so in the sentence.
enum class RelativeSeek {
    Landed,   // playhead + delta was already inside [0, durationUs]
    Clamped,  // it was outside, and *landUs is the end of the file it ran into
    NoRange,  // durationUs <= 0: nothing is known about where the file ends. REFUSE, never clamp
              // everything to 0 -- "the file is 0 long" and "seek to the start" are not the same
              // command, and the second one is spelled `seek 0`.
};

inline const char* RelativeSeekName(RelativeSeek r) {
    switch (r) {
        case RelativeSeek::Landed:  return "landed";
        case RelativeSeek::Clamped: return "clamped";
        case RelativeSeek::NoRange: return "no-range";
    }
    return "unknown";
}

// Saturating, because the sum is the one place this arithmetic can wrap: `whole` carries a signed
// microsecond delta bounded by the parser's kMaxSeconds, but `playheadUs` is whatever the transport
// holds, and a wrapped sum would clamp to the WRONG end of the file -- a `seek +10` that lands at 0.
inline int64_t SaturatingAddUs(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b) {
        return INT64_MAX;
    }
    if (b < 0 && a < INT64_MIN - b) {
        return INT64_MIN;
    }
    return a + b;
}

// `landUs` is written on every answer except NoRange, so a caller that prints it cannot print an
// uninitialised value after a refusal it forgot to check.
inline RelativeSeek ResolveRelativeSeekUs(int64_t playheadUs, int64_t deltaUs, int64_t durationUs,
                                          int64_t* landUs) {
    if (durationUs <= 0) {
        return RelativeSeek::NoRange;
    }
    const int64_t want = SaturatingAddUs(playheadUs, deltaUs);
    int64_t land = want;
    if (land < 0) {
        land = 0;
    } else if (land > durationUs) {
        land = durationUs;
    }
    if (landUs != nullptr) {
        *landUs = land;
    }
    // A playhead already outside the file (which the transport can report while a load settles)
    // clamps too, and saying so is the honest answer: the landing is not where the arithmetic said.
    return land == want ? RelativeSeek::Landed : RelativeSeek::Clamped;
}

// `seek <pct>` is the same absolute seek as `seek <us>` once the percentage is a time, and the
// percentage is the only part of it that is a decision -- so it is here, where the offline suite
// can sweep it, and not inside an arm. The arm used to hand the fraction to
// ui::SeekFraction, which returns true whatever the transport did, so a percentage seek could not
// tell a landing from a no-op.
enum class PercentSeek {
    Resolved,
    NoRange,  // durationUs <= 0. Half of a span nobody knows is not 0 -- the command that means
              // the start of the file is spelled `seek 0`, and answering it here would move the
              // playhead somewhere the operator did not ask for.
};

inline PercentSeek ResolvePercentSeekUs(double percent, int64_t durationUs, int64_t* landUs) {
    // NaN compares false against every bound, so it would fall through the clamps below and cast
    // to an unspecified time. It has no landing, and that is the answer it gets.
    if (durationUs <= 0 || !(percent == percent)) {
        return PercentSeek::NoRange;
    }
    if (percent < 0.0) {
        percent = 0.0;
    } else if (percent > 100.0) {
        percent = 100.0;
    }
    int64_t land = static_cast<int64_t>(percent / 100.0 * static_cast<double>(durationUs));
    // The product is a double and the duration can exceed what one holds exactly, so the clamp is
    // re-applied on the integer: 100% must be the end of the file and never one microsecond past.
    if (land < 0) {
        land = 0;
    } else if (land > durationUs) {
        land = durationUs;
    }
    if (landUs != nullptr) {
        *landUs = land;
    }
    return PercentSeek::Resolved;
}

// Again three answers. S8.2: `goto next` at the last bookmark REFUSES rather than wrapping, and
// "there are no bookmarks" must not borrow that refusal's words -- one is answered by `mark`, the
// other by moving the playhead.
enum class MarkerStep {
    Found,
    NoMarkers,
    AtEdge,
};

inline const char* MarkerStepName(MarkerStep s) {
    switch (s) {
        case MarkerStep::Found:     return "found";
        case MarkerStep::NoMarkers: return "no-markers";
        case MarkerStep::AtEdge:    return "at-edge";
    }
    return "unknown";
}

// STRICTLY greater / less, so two `goto next` in a row advance twice instead of sticking on the
// marker they just landed on (S8.2).
//
// O(n) over n <= notes::kMaxBookmarks (256), deliberately, and NOT a binary search: notes.cpp
// appends each bookmark at count_ in the order `mark` was typed and never sorts, so the array is
// unordered and a neighbour-by-index step would land on whichever marker happened to be recorded
// next -- the defect the unsorted fixture in tests/control pins. The nearest-above / nearest-below
// scan is correct on unordered input; sorting first to binary-search 256 int64s would cost more
// than the scan it saves, once, per keystroke.
inline MarkerStep StepMarkerUs(const int64_t* tUs, size_t count, int64_t nowUs, bool forward,
                               int64_t* landUs) {
    if (tUs == nullptr || count == 0) {
        return MarkerStep::NoMarkers;
    }
    bool have = false;
    int64_t best = 0;
    for (size_t i = 0; i < count; ++i) {
        const int64_t t = tUs[i];
        if (forward) {
            if (t <= nowUs) {
                continue;
            }
            if (!have || t < best) {
                best = t;
                have = true;
            }
        } else {
            if (t >= nowUs) {
                continue;
            }
            if (!have || t > best) {
                best = t;
                have = true;
            }
        }
    }
    if (!have) {
        return MarkerStep::AtEdge;
    }
    if (landUs != nullptr) {
        *landUs = best;
    }
    return MarkerStep::Found;
}

// ARE WE WHERE WE SAID WE ARE? Three answers, not two (L3-RUN-3 F1). Every arm that names a target
// time must read the playhead back after the seek and classify what it finds: the absolute arm has
// done this since 6e18393 and the relative, bookmark and marker-step arms did not -- they echoed
// their own arithmetic, so `seek +10` answered "to 12133312 us" having landed on 2255533 us and
// `goto next` answered "bookmark at 2133312" having landed on 2255533.
enum class SeekLanding {
    OnTarget,    // the playhead is exactly where the arm asked for
    NearestTick, // it is within the tolerance below: a sampled capture has no tick at every
                 // microsecond, and landing on the neighbouring one is correct behaviour
    OffTarget,   // it is somewhere else entirely. REFUSE BY NAME: an arm that printed its target
                 // here would be making a false statement about where the playhead is.
};

inline const char* SeekLandingName(SeekLanding s) {
    switch (s) {
        case SeekLanding::OnTarget:    return "on-target";
        case SeekLanding::NearestTick: return "nearest-tick";
        case SeekLanding::OffTarget:   return "landed-off-target";
    }
    return "unknown";
}

// THE TOLERANCE IS AN INPUT, NOT A CONSTANT. It used to be a compiled-in one-30-Hz-
// frame 33334 us, chosen because IReplayTransport publishes no tick interval (transport.h:85-108)
// -- but a landing classified against a number nobody measured is a verdict about a machine that
// may not exist, and the same 35 ms miss is a correct neighbouring tick on one cadence and a real
// miss on another. The caller passes the interval published by the one place that MEASURES a
// cadence (ui::TickIntervalUs), and when nothing has been measured the row is COULD-NOT-ASK in the
// caller -- never on-target, and never a guess made here. The misses F1 recorded are 9.9 s and
// 122 ms, outside any cadence this system runs at.
//
// A tolerance of zero or less means only an exact landing counts. That is the honest reading of an
// interval of zero, and it is not the same thing as declining to answer.
inline SeekLanding ClassifySeekLandingUs(int64_t targetUs, int64_t landedUs, int64_t toleranceUs) {
    if (landedUs == targetUs) {
        return SeekLanding::OnTarget;
    }
    // The difference is the one place this can wrap -- `-INT64_MIN` is undefined and a negated
    // target would refuse for the wrong reason -- so the magnitude is taken in the unsigned domain,
    // where the subtraction is exact for every pair.
    const uint64_t landedBits = static_cast<uint64_t>(landedUs);
    const uint64_t targetBits = static_cast<uint64_t>(targetUs);
    const uint64_t away = landedUs > targetUs ? landedBits - targetBits : targetBits - landedBits;
    if (toleranceUs <= 0) {
        return SeekLanding::OffTarget;
    }
    return away <= static_cast<uint64_t>(toleranceUs) ? SeekLanding::NearestTick
                                                      : SeekLanding::OffTarget;
}

// ★★★ WHOSE CADENCE. The tolerance is ONE TICK OF THE FILE, asked of the transport that loaded it.
//
// It used to be `ui::TickIntervalUs` -- the RENDER frame period, the only cadence this process
// measured before IReplayTransport::TickSpacingUs existed. Those are two different quantities that
// happen to share a unit. A 30 Hz capture (33333 us between samples) on a 90 Hz machine (11111 us
// between frames) put every correct nearest-tick landing 16000 us from its target outside a
// tolerance of 11111, so `seek`, `goto` and the marker steps reported `landed-off-target` for
// behaviour that was exactly right. The render period keeps its job -- panel.cpp's rate ceiling,
// which IS a question about this machine -- and stops being consulted about the file.
enum class SeekTolerance {
    FromCapture,  // the loaded file published its sample spacing
    Unknown,      // nothing loaded, or a capture with no measurable cadence
};

inline SeekTolerance SeekToleranceUs(const IReplayTransport* t, int64_t* outUs) {
    int64_t spacingUs = 0;
    if (t == nullptr || !t->TickSpacingUs(&spacingUs) || spacingUs <= 0) {
        return SeekTolerance::Unknown;
    }
    if (outUs != nullptr) {
        *outUs = spacingUs;
    }
    return SeekTolerance::FromCapture;
}

// The whole landing decision, here rather than in the dispatcher arm for this file's stated reason:
// tests/control cannot link dispatch.cpp, so a decision left there is one no test can execute.
// Returns nullptr when the landing is acceptable, otherwise the sentence the arm reports -- which
// is the fourth answer SeekLanding cannot hold when the capture publishes no cadence. Never a
// verdict off a number nobody measured.
inline const char* SeekLandingRefusalFor(const IReplayTransport* t, int64_t targetUs,
                                         int64_t landedUs, SeekLanding* landing) {
    int64_t toleranceUs = 0;
    if (SeekToleranceUs(t, &toleranceUs) != SeekTolerance::FromCapture) {
        return "could-not-ask: no capture is loaded, or this one publishes no tick spacing, so "
               "there is no sample interval to classify the landing against";
    }
    const SeekLanding s = ClassifySeekLandingUs(targetUs, landedUs, toleranceUs);
    if (landing != nullptr) {
        *landing = s;
    }
    return s == SeekLanding::OffTarget ? SeekLandingName(s) : nullptr;
}

}  // namespace control
}  // namespace replay
