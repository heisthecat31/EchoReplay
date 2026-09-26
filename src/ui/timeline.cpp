#include <math.h>
#include <stdio.h>
#include "capture/markers.h"
#include "ui/timeline.h"

namespace replay {
namespace ui {
namespace {

int64_t ClampI64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

uint32_t CellCount(float widthPx, float cellPx) {
    if (!(widthPx > 0.0f) || !(cellPx > 0.0f)) return 1;
    const float n = widthPx / cellPx;
    if (!(n >= 1.0f)) return 1;
    if (n >= static_cast<float>(kMaxCells)) return kMaxCells;
    return static_cast<uint32_t>(n);
}

int64_t CellEdgeUs(const Span& s, uint32_t cells, uint32_t i) {
    if (cells == 0) return s.beginUs;
    if (i > cells) i = cells;
    const int64_t dur = s.DurationUs();
    if (dur == 0) return s.beginUs;
    return s.beginUs + static_cast<int64_t>(i) * dur / static_cast<int64_t>(cells);
}

uint32_t CellOfUs(const Span& s, uint32_t cells, int64_t tUs) {
    if (cells <= 1) return 0;
    if (s.DurationUs() == 0) return 0;
    if (tUs <= s.beginUs) return 0;
    if (tUs >= s.endUs) return cells - 1;

    // ★ THE INVERSE OF CellEdgeUs, COMPUTED FROM CellEdgeUs -- not a second formula that ought to
    // agree with it. The obvious closed form, (t - begin) * cells / dur, is the algebraic inverse
    // and is WRONG at the boundaries, because both it and CellEdgeUs truncate and the two
    // truncations do not cancel: with dur = 10 and cells = 4 the edges are 0, 2, 5, 7, but the
    // closed form maps 2 to cell 0 -- so a click on cell 1 would land the playhead in cell 0. Eight
    // compares over an at-most-256-cell strip is not a cost worth trading a wrong answer for, and
    // it makes the round trip hold BY CONSTRUCTION rather than by arithmetic luck.
    uint32_t lo = 0;
    uint32_t hi = cells - 1;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1) / 2;
        if (CellEdgeUs(s, cells, mid) <= tUs) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

int64_t SeekTargetForCell(const Span& s, uint32_t cells, uint32_t i) {
    // A one-cell strip is degenerate and the rule below would send its only cell to the END of the
    // capture, which is a surprising thing for the only click available to do. It seeks to the
    // start, like every other cell 0.
    if (cells <= 1) return s.beginUs;
    if (i >= cells - 1) return s.endUs;   // the last cell reaches the end of the capture exactly
    return CellEdgeUs(s, cells, i);
}

int64_t SeekTargetForCellSingleDenominator(const Span& s, uint32_t cells, uint32_t i) {
    return CellEdgeUs(s, cells, i);
}

Span FineSpan(int64_t playheadUs, int64_t durationUs, int64_t windowUs) {
    Span s;
    if (durationUs < 0) durationUs = 0;
    if (windowUs <= 0 || durationUs <= windowUs) {
        s.beginUs = 0;
        s.endUs = durationUs;
        return s;
    }
    const int64_t clampedHead = ClampI64(playheadUs, 0, durationUs);
    int64_t begin = clampedHead - windowUs / 2;
    begin = ClampI64(begin, 0, durationUs - windowUs);
    s.beginUs = begin;
    s.endUs = begin + windowUs;
    return s;
}

uint32_t ClassifyCells(const Span& s, uint32_t cells, const Marker* markers, size_t markerCount,
                       int64_t playheadUs, bool playheadInSpan, CellClass* out) {
    if (out == nullptr || cells == 0) return 0;
    for (uint32_t i = 0; i < cells; ++i) out[i] = CellClass::Normal;

    uint32_t landed = 0;
    for (size_t m = 0; m < markerCount; ++m) {
        const int64_t t = markers[m].tUs;
        // Strictly inside, and the end is inclusive so a round boundary exactly at the end of the
        // window still shows. A marker outside is skipped, never clamped onto an end cell: a tick
        // that says "a round starts here" when it does not is worse than no tick.
        if (t < s.beginUs || t > s.endUs) continue;
        out[CellOfUs(s, cells, t)] = CellClass::Marker;
        ++landed;
    }

    if (playheadInSpan && playheadUs >= s.beginUs && playheadUs <= s.endUs) {
        CellClass& c = out[CellOfUs(s, cells, playheadUs)];
        c = (c == CellClass::Marker) ? CellClass::MarkerPlayhead : CellClass::Playhead;
    }
    return landed;
}

size_t LayoutMarkerLabels(const Span& s, float widthPx, float minSpacingPx, const Marker* markers,
                          size_t markerCount, MarkerLabel* out, size_t outCap, size_t* dropped) {
    size_t placed = 0;
    size_t skipped = 0;
    if (dropped != nullptr) *dropped = 0;
    if (out == nullptr || outCap == 0 || !(widthPx > 0.0f)) {
        if (dropped != nullptr) *dropped = markerCount;
        return 0;
    }
    const int64_t dur = s.DurationUs();
    float lastX = 0.0f;
    bool haveLast = false;

    for (size_t m = 0; m < markerCount; ++m) {
        const int64_t t = markers[m].tUs;
        if (t < s.beginUs || t > s.endUs) continue;   // not in this window: not this strip's to show
        const float frac = dur > 0 ? static_cast<float>(static_cast<double>(t - s.beginUs) /
                                                        static_cast<double>(dur))
                                   : 0.0f;
        const float x = frac * widthPx;
        if (haveLast && (x - lastX) < minSpacingPx) {
            ++skipped;
            continue;
        }
        if (placed >= outCap) {
            ++skipped;
            continue;
        }
        out[placed].x = x;
        out[placed].index = markers[m].index;
        out[placed].kind = markers[m].kind;
        ++placed;
        lastX = x;
        haveLast = true;
    }
    if (dropped != nullptr) *dropped = skipped;
    return placed;
}

const char* RoundGlyph(uint16_t kind) {
    using namespace replay::capture;
    switch (kind) {
        case kMarkMatchBegin: return "MB";
        case kMarkMatchEnd: return "ME";
        case kMarkRoundBegin: return "RB";
        case kMarkRoundEnd: return "RE";
        case kMarkLaunchRelease: return "LR";
        case kMarkGoal: return "G";
        case kMarkSuddenDeath: return "SD";
        case kMarkOvertime: return "OT";
        case kMarkMatchOver: return "MO";
        case kMarkPauseBegin: return "PB";
        case kMarkPauseEnd: return "PE";
        case kMarkJoin: return "J";
        case kMarkLeave: return "L";
        case kMarkTeamChange: return "TC";
        case kMarkBookmark: return "BM";
        case kMarkPhase: return "PH";
    }
    return "?";
}

size_t FormatRoundLabel(uint16_t kind, uint16_t index, uint32_t runLength, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    const bool carriesRound =
        kind == replay::capture::kMarkRoundBegin || kind == replay::capture::kMarkRoundEnd;
    int n;
    if (carriesRound && runLength > 1) {
        n = snprintf(out, cap, "%s%ux%u", RoundGlyph(kind), static_cast<unsigned>(index),
                     static_cast<unsigned>(runLength));
    } else if (carriesRound) {
        n = snprintf(out, cap, "%s%u", RoundGlyph(kind), static_cast<unsigned>(index));
    } else if (runLength > 1) {
        n = snprintf(out, cap, "%sx%u", RoundGlyph(kind), static_cast<unsigned>(runLength));
    } else {
        n = snprintf(out, cap, "%s", RoundGlyph(kind));
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n) < cap - 1 ? static_cast<size_t>(n) : cap - 1;
}

size_t BuildRoundStrip(const Marker* markers, size_t markerCount, RoundEntry* out, size_t outCap,
                       size_t* clamped) {
    if (clamped != nullptr) *clamped = 0;
    if (markers == nullptr || out == nullptr || outCap == 0) {
        if (clamped != nullptr) *clamped = markerCount;
        return 0;
    }
    size_t written = 0;
    size_t dropped = 0;
    size_t m = 0;
    while (m < markerCount) {
        size_t run = 1;
        while (m + run < markerCount && markers[m + run].kind == markers[m].kind &&
               markers[m + run].index == markers[m].index) {
            ++run;
        }
        if (written < outCap) {
            RoundEntry& e = out[written];
            FormatRoundLabel(markers[m].kind, markers[m].index, static_cast<uint32_t>(run),
                             e.label, sizeof e.label);
            e.sourceIndex = m;
            e.index = markers[m].index;
            e.runLength = static_cast<uint32_t>(run);
            ++written;
        } else {
            ++dropped;
        }
        m += run;
    }
    if (clamped != nullptr) *clamped = dropped;
    return written;
}

float RateCeiling(uint32_t maxElapsedUs, uint64_t rawFramePeriodUs) {
    if (rawFramePeriodUs == 0 || maxElapsedUs == 0) return 0.0f;
    return static_cast<float>(maxElapsedUs) / static_cast<float>(rawFramePeriodUs);
}

double ReportedRate(double rate) {
    const double magnitude = rate < 0.0 ? -rate : rate;
    const double stated = floor(magnitude * 100.0 + 0.5) / 100.0;
    return rate < 0.0 ? -stated : stated;
}

RateVerdict CheckRate(double rate, float ceiling) {
    if (rate == 0.0) return RateVerdict::ZeroRefused;
    // The comparison is made on the numbers we STATE, not on the raw ratio. 66666 us / 33334 us per
    // frame is 1.99994, and refusing 2.0 against it printed "2.00x is above this machine's
    // fast-forward ceiling of 2.00x": the only fast-forward rate this build advertises, unplayable,
    // refused by a sentence that contradicts itself (L3-RUN-9 F2). The ceiling is still derived
    // exactly as before; only the comparison's precision changes, and it can admit at most 0.005
    // over the ratio -- the direction the frame-period measurement is already biased away from.
    const double magnitude = ReportedRate(rate < 0.0 ? -rate : rate);
    if (ceiling > 0.0f && magnitude > ReportedRate(static_cast<double>(ceiling))) {
        return RateVerdict::AboveCeiling;
    }
    return RateVerdict::Ok;
}

size_t OfferedRates(float ceiling, float* out, size_t cap) {
    // The round numbers a user reaches for. It is a starting set, not the answer: what survives is
    // whatever this ceiling admits.
    static const float kLadder[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    size_t n = 0;
    for (size_t i = 0; i < sizeof kLadder / sizeof kLadder[0] && n < cap; ++i) {
        if (CheckRate(static_cast<double>(kLadder[i]), ceiling) == RateVerdict::Ok) out[n++] = kLadder[i];
    }
    if (ceiling <= 0.0f) return n;
    const float stated = static_cast<float>(ReportedRate(static_cast<double>(ceiling)));
    for (size_t i = 0; i < n; ++i) {
        // Compared as the numbers we state, exactly as CheckRate compares them: a ceiling of
        // 1.99994 states 2.00x and must not add a second button beside the ladder's 2x.
        if (ReportedRate(static_cast<double>(out[i])) == ReportedRate(static_cast<double>(stated))) {
            return n;
        }
    }
    if (n < cap) out[n++] = stated;
    return n;
}

Availability3 ClassifyAvailability(bool sourceConsulted, size_t unavailableCount) {
    if (!sourceConsulted) return Availability3::Unknown;
    return unavailableCount == 0 ? Availability3::AllAvailable : Availability3::SomeUnavailable;
}

const char* Availability3Name(Availability3 a) {
    switch (a) {
        case Availability3::Unknown: return "Unknown";
        case Availability3::AllAvailable: return "AllAvailable";
        case Availability3::SomeUnavailable: return "SomeUnavailable";
    }
    return "<invalid>";
}

size_t FindRoundByNumber(const Marker* markers, size_t count, int64_t roundNumber) {
    if (markers == nullptr) return kNoMarker;
    for (size_t i = 0; i < count; ++i) {
        if (static_cast<int64_t>(markers[i].index) == roundNumber) return i;
    }
    return kNoMarker;
}

void FormatTime(int64_t tUs, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return;
    const bool neg = tUs < 0;
    // Negate in unsigned so INT64_MIN does not overflow on the way to its own magnitude.
    uint64_t mag = neg ? (~static_cast<uint64_t>(tUs) + 1u) : static_cast<uint64_t>(tUs);

    const uint64_t ms = (mag / 1000u) % 1000u;
    const uint64_t totalSec = mag / 1000000u;
    const uint64_t sec = totalSec % 60u;
    const uint64_t min = (totalSec / 60u) % 60u;
    const uint64_t hour = totalSec / 3600u;

    char buf[40];
    size_t n = 0;
    if (neg) buf[n++] = '-';

    auto put = [&](uint64_t v, int digits) {
        char d[20];
        int k = 0;
        do {
            d[k++] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        } while (v != 0 && k < 20);
        while (k < digits) d[k++] = '0';
        while (k > 0 && n + 1 < sizeof buf) buf[n++] = d[--k];
    };

    if (hour > 0) {
        put(hour, 1);
        buf[n++] = ':';
        put(min, 2);
    } else {
        put(min, 1);
    }
    buf[n++] = ':';
    put(sec, 2);
    buf[n++] = '.';
    put(ms, 3);
    buf[n] = '\0';

    size_t i = 0;
    for (; i + 1 < cap && i < n; ++i) out[i] = buf[i];
    out[i] = '\0';
}

// ---- UI-2: the bar's three tick families and their colours ------------------------------------

MarkerFamily FamilyOfKind(uint16_t kind) {
    using namespace replay::capture;
    switch (kind) {
        case kMarkMatchBegin:
        case kMarkMatchEnd:
        case kMarkRoundBegin:
        case kMarkRoundEnd:
        case kMarkMatchOver:
            return MarkerFamily::Round;
        case kMarkGoal:
        case kMarkLaunchRelease:
        case kMarkSuddenDeath:
        case kMarkOvertime:
            return MarkerFamily::Goal;
        case kMarkBookmark:
            return MarkerFamily::Bookmark;
    }
    // Joins, leaves, team changes, pauses and phases. They are in the capture and they are what the
    // owner called clutter; the debug panel still lists every one of them.
    return MarkerFamily::None;
}

Rgba CellColour(CellClass cls, MarkerFamily family) {
    switch (cls) {
        case CellClass::Marker:
        case CellClass::MarkerPlayhead:
            switch (family) {
                case MarkerFamily::Goal: return Rgba{1.00f, 0.82f, 0.40f, 1.00f};
                case MarkerFamily::Bookmark: return Rgba{0.49f, 0.88f, 0.66f, 1.00f};
                case MarkerFamily::Round:
                case MarkerFamily::None: break;
            }
            return Rgba{0.44f, 0.70f, 1.00f, 1.00f};
        case CellClass::Playhead: return Rgba{1.00f, 1.00f, 1.00f, 1.00f};
        case CellClass::Elapsed: return Rgba{1.00f, 0.54f, 0.24f, 1.00f};
        case CellClass::Normal: break;
    }
    return Rgba{1.00f, 1.00f, 1.00f, 0.18f};
}

uint32_t ClassifyCellsElapsed(const Span& span, uint32_t cells, const Marker* markers,
                              size_t markerCount, int64_t playheadUs, bool playheadInSpan,
                              CellClass* out) {
    const uint32_t landed =
        ClassifyCells(span, cells, markers, markerCount, playheadUs, playheadInSpan, out);
    if (!playheadInSpan || cells == 0) return landed;
    // CellOfUs clamps, so a playhead outside the span colours the whole track or none of it --
    // which is what "before the playhead" means at either end.
    const uint32_t head = CellOfUs(span, cells, playheadUs);
    for (uint32_t i = 0; i < head && i < cells; ++i) {
        if (out[i] == CellClass::Normal) out[i] = CellClass::Elapsed;
    }
    return landed;
}

}  // namespace ui
}  // namespace replay
