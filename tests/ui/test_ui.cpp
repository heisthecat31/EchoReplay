// W2-F. The offline half of the UI gate: the timeline arithmetic, which has no facade call and no
// engine read in it and therefore runs on a machine with no game.
//
// WHAT THIS COVERS, stated because a GATE-RESULT number with no scope is a claim: cell layout, the
// click round trip, endpoint reachability, the fine window, marker classification, label layout,
// the fast-forward ceiling and time formatting. It covers NO facade call, NO band-3 registration
// and NO drawing -- those need a launch and are parked in runs/queue/payload.json as
// w2f-band3-registration-lands and w2f-band3-callback-thread. This gate being green is not the
// panel working.

#include "doctest.h"

#include <stdint.h>
#include <string.h>

#include "generated/facts_generated.h"
#include "ui/timeline.h"

using namespace replay::ui;

namespace {

Span Whole(int64_t durationUs) {
    Span s;
    s.beginUs = 0;
    s.endUs = durationUs;
    return s;
}

// Durations chosen so the integer division lands differently: a round number, a prime-ish one that
// never divides evenly, a two-hour capture, and the degenerate zero.
const int64_t kDurations[] = {0, 1, 999, 1000000, 60000000, 599999999, 7200000000ll};

}  // namespace

TEST_CASE("CellCount stays inside its bounds whatever it is handed") {
    CHECK(CellCount(0.0f, 6.0f) == 1);
    CHECK(CellCount(-100.0f, 6.0f) == 1);
    CHECK(CellCount(700.0f, 0.0f) == 1);
    CHECK(CellCount(700.0f, -6.0f) == 1);
    CHECK(CellCount(5.0f, 6.0f) == 1);         // narrower than one cell is still one cell
    CHECK(CellCount(700.0f, 6.0f) == 116);
    CHECK(CellCount(1.0e9f, 1.0f) == kMaxCells);
    // The value the panel actually uses, pinned so a change to either constant is visible here.
    CHECK(CellCount(690.0f, 6.0f) == 115);
}

TEST_CASE("P1: clicking cell i puts the playhead in cell i") {
    // SCOPED, because outside this scope P1 is not merely unmet but unmeetable: when a strip has
    // more cells than the span has microseconds, two cells share an edge and nothing can tell them
    // apart. The degenerate case gets its own weaker assertion below rather than being dropped.
    size_t exercised = 0;
    for (int64_t dur : kDurations) {
        const Span s = Whole(dur);
        for (uint32_t cells = 1; cells <= kMaxCells; ++cells) {
            if (dur < static_cast<int64_t>(cells)) continue;
            for (uint32_t i = 0; i < cells; ++i) {
                const int64_t t = SeekTargetForCell(s, cells, i);
                CHECK(t >= s.beginUs);
                CHECK(t <= s.endUs);
                CHECK(CellOfUs(s, cells, t) == i);
                ++exercised;
            }
        }
    }
    // A scoped sweep that skipped everything would pass silently. It does not.
    CHECK(exercised > 100000);
}

TEST_CASE("with more cells than microseconds the round trip still lands on the same instant") {
    // The honest weaker property. Two cells that share an edge ARE the same instant, so the index
    // may differ while the TIME must not -- and the time is what the user sees.
    const Span s = Whole(50);
    for (uint32_t cells = 60; cells <= 256; ++cells) {
        for (uint32_t i = 0; i < cells; ++i) {
            const int64_t t = SeekTargetForCell(s, cells, i);
            const uint32_t landed = CellOfUs(s, cells, t);
            CHECK(landed < cells);
            CHECK(CellEdgeUs(s, cells, landed) == CellEdgeUs(s, cells, i));
        }
    }
}

TEST_CASE("P2: both ends of the capture are reachable from the strip") {
    for (int64_t dur : kDurations) {
        const Span s = Whole(dur);
        for (uint32_t cells = 2; cells <= kMaxCells; ++cells) {
            CHECK(SeekTargetForCell(s, cells, 0) == s.beginUs);
            CHECK(SeekTargetForCell(s, cells, cells - 1) == s.endUs);
        }
        // A one-cell strip is degenerate: its only cell seeks to the start, not the end, because
        // "the only click available jumps to the end of the match" is not a sane default.
        CHECK(SeekTargetForCell(s, 1, 0) == s.beginUs);
    }
}

// The red arm, and what it is red FOR.
TEST_CASE("the single-denominator mutant passes P1 and fails P2") {
    size_t p1Cases = 0;
    size_t p2Failures = 0;
    for (int64_t dur : kDurations) {
        const Span s = Whole(dur);
        for (uint32_t cells = 2; cells <= kMaxCells; ++cells) {
            if (dur < static_cast<int64_t>(cells)) continue;   // outside P1's scope for both forms
            for (uint32_t i = 0; i < cells; ++i) {
                const int64_t t = SeekTargetForCellSingleDenominator(s, cells, i);
                // The mutant really does satisfy the round trip. That is exactly why P1 alone
                // would have shipped it, and why P2 has to exist as a separate property.
                CHECK(CellOfUs(s, cells, t) == i);
                ++p1Cases;
            }
            if (SeekTargetForCellSingleDenominator(s, cells, cells - 1) < s.endUs) ++p2Failures;
        }
    }
    // Both halves. If the mutant ever stopped passing P1, or stopped failing P2, the pair would
    // have lost its discriminating power and this test would be measuring nothing.
    CHECK(p1Cases > 100000);
    CHECK(p2Failures > 1000);
}

TEST_CASE("FineSpan keeps a constant width and slides against the ends") {
    const int64_t dur = 600000000;   // ten minutes
    const int64_t win = 10000000;    // ten seconds

    const Span atStart = FineSpan(0, dur, win);
    CHECK(atStart.beginUs == 0);
    CHECK(atStart.DurationUs() == win);

    const Span atEnd = FineSpan(dur, dur, win);
    CHECK(atEnd.endUs == dur);
    CHECK(atEnd.DurationUs() == win);

    const Span middle = FineSpan(dur / 2, dur, win);
    CHECK(middle.DurationUs() == win);
    CHECK(middle.beginUs == dur / 2 - win / 2);

    // A playhead outside the file is clamped, not allowed to drag the window off the axis.
    CHECK(FineSpan(-5000000, dur, win).beginUs == 0);
    CHECK(FineSpan(dur * 2, dur, win).endUs == dur);

    // Shorter than the window, or no window: the whole file, never a span that runs past the end.
    CHECK(FineSpan(0, 3000000, win).endUs == 3000000);
    CHECK(FineSpan(0, dur, 0).endUs == dur);
    CHECK(FineSpan(0, dur, -1).endUs == dur);
}

TEST_CASE("a marker outside the span places no tick at all") {
    const Span s = {10000000, 20000000};
    const Marker outside[] = {{5000000, 1}, {25000000, 2}};
    CellClass cells[16];
    const uint32_t landed = ClassifyCells(s, 16, outside, 2, 0, false, cells);
    CHECK(landed == 0);
    // ★ The vacuity trap this exists to catch: clamping an out-of-range marker onto an end cell
    // would draw a tick that says "a round starts here" when no round does, and the count alone
    // would not notice because it would still be 2. So the CELLS are checked, not just the count.
    for (int i = 0; i < 16; ++i) CHECK(cells[i] == CellClass::Normal);
}

TEST_CASE("a marker inside lands in the cell CellOfUs names, and coexists with the playhead") {
    const Span s = Whole(1000000);
    CellClass cells[10];
    const Marker m[] = {{300000, 1}};

    uint32_t landed = ClassifyCells(s, 10, m, 1, 0, false, cells);
    CHECK(landed == 1);
    CHECK(cells[CellOfUs(s, 10, 300000)] == CellClass::Marker);

    // Playhead in a different cell: two distinct classes, neither masking the other.
    landed = ClassifyCells(s, 10, m, 1, 800000, true, cells);
    CHECK(cells[CellOfUs(s, 10, 300000)] == CellClass::Marker);
    CHECK(cells[CellOfUs(s, 10, 800000)] == CellClass::Playhead);

    // Playhead ON the marker: a fourth value, because rendering it as either one alone would hide
    // the other and the user would lose a round mark exactly when they scrubbed onto it.
    landed = ClassifyCells(s, 10, m, 1, 300000, true, cells);
    CHECK(cells[CellOfUs(s, 10, 300000)] == CellClass::MarkerPlayhead);
}

TEST_CASE("marker labels that would overlap are dropped and counted") {
    const Span s = Whole(1000000);
    Marker m[8];
    for (int i = 0; i < 8; ++i) {
        m[i].tUs = 500000 + i * 1000;   // eight markers within 7 ms: a few pixels apart at most
        m[i].index = static_cast<uint16_t>(i + 1);
    }
    MarkerLabel out[8];
    size_t dropped = 0;
    const size_t placed = LayoutMarkerLabels(s, 690.0f, 26.0f, m, 8, out, 8, &dropped);
    CHECK(placed == 1);
    CHECK(dropped == 7);
    CHECK(placed + dropped == 8);   // nothing is lost without being counted

    // The control: with no minimum spacing nothing is dropped, which is what proves the drop above
    // came from the spacing rule and not from the markers being unreachable in the first place.
    size_t dropped2 = 0;
    const size_t placed2 = LayoutMarkerLabels(s, 690.0f, 0.0f, m, 8, out, 8, &dropped2);
    CHECK(placed2 == 8);
    CHECK(dropped2 == 0);

    // Output capacity is respected and the overflow is counted, not written past.
    MarkerLabel small[2];
    size_t dropped3 = 0;
    const size_t placed3 = LayoutMarkerLabels(s, 690.0f, 0.0f, m, 8, small, 2, &dropped3);
    CHECK(placed3 == 2);
    CHECK(dropped3 == 6);
}

TEST_CASE("the fast-forward ceiling reports unknown rather than guessing 4x") {
    // ★ The whole point of the function. 4.0 is the ceiling at 60 Hz ONLY; a UI that hardcodes it
    // lies on a 90 Hz machine, and a UI that returns 4.0 when it has measured nothing lies always.
    CHECK(RateCeiling(66666, 0) == 0.0f);
    CHECK(RateCeiling(0, 16666) == 0.0f);
    CHECK(RateCeiling(66666, 16666) == doctest::Approx(4.0).epsilon(0.001));
    CHECK(RateCeiling(66666, 13888) == doctest::Approx(4.8).epsilon(0.001));   // 72 Hz
    CHECK(RateCeiling(66666, 11111) == doctest::Approx(6.0).epsilon(0.001));   // 90 Hz
}

TEST_CASE("the offered rates are derived from the ceiling, and the ceiling is one of them") {
    float r[kOfferedRateCap];

    // This machine (L3-RUN-10): the sentence says 2.00x, so 4x must not be on offer and 2x must.
    size_t n = OfferedRates(2.0f, r, kOfferedRateCap);
    REQUIRE(n == 5);
    CHECK(r[4] == doctest::Approx(2.0));
    for (size_t i = 0; i < n; ++i) CHECK(CheckRate(r[i], 2.0f) == RateVerdict::Ok);

    // A ceiling that is not a round number is still reachable: the row offers it by name.
    n = OfferedRates(1.73f, r, kOfferedRateCap);
    REQUIRE(n == 5);
    CHECK(r[4] == doctest::Approx(1.73));
    CHECK(CheckRate(r[4], 1.73f) == RateVerdict::Ok);

    // Exactly a ladder rate: offered once, not twice.
    n = OfferedRates(4.0f, r, kOfferedRateCap);
    CHECK(n == 6);
    // A 30 Hz machine's ceiling is 1.99994, stated 2.00x. CheckRate admits 2x against it, so the
    // row must not grow a second 2.00x button beside the ladder's.
    n = OfferedRates(1.99994f, r, kOfferedRateCap);
    CHECK(n == 5);

    // Unknown ceiling refuses nothing, so nothing is withheld either.
    n = OfferedRates(0.0f, r, kOfferedRateCap);
    CHECK(n == 6);
    CHECK(r[5] == doctest::Approx(4.0));

    // A ceiling below every round number still leaves the machine something to press.
    n = OfferedRates(0.07f, r, kOfferedRateCap);
    REQUIRE(n == 1);
    CHECK(r[0] == doctest::Approx(0.07));

    // ★ THE ARM, swept rather than sampled: an offered rate the clamp refuses is the defect.
    for (float ceiling = 0.05f; ceiling <= 8.0f; ceiling += 0.01f) {
        n = OfferedRates(ceiling, r, kOfferedRateCap);
        CHECK(n > 0);
        bool ceilingOffered = false;
        for (size_t i = 0; i < n; ++i) {
            CHECK(CheckRate(r[i], ceiling) == RateVerdict::Ok);
            if (ReportedRate(r[i]) == ReportedRate(static_cast<double>(ceiling))) ceilingOffered = true;
        }
        CHECK(ceilingOffered);
    }
}

TEST_CASE("an unknown ceiling refuses no rate, and zero is refused always") {
    // ★ The trap this exists to stop. With no measured frame period the ceiling is 0, and a policy
    // that treated 0 as "the limit is zero" would refuse EVERY rate; one that substituted 4.0 would
    // refuse legitimate rates on a 90 Hz machine. Neither: unknown means nothing is refused.
    CHECK(CheckRate(1.0, 0.0f) == RateVerdict::Ok);
    CHECK(CheckRate(8.0, 0.0f) == RateVerdict::Ok);
    CHECK(CheckRate(-8.0, 0.0f) == RateVerdict::Ok);

    // Zero is refused whatever the ceiling, known or not: the clock reciprocates the scaled delta.
    CHECK(CheckRate(0.0, 0.0f) == RateVerdict::ZeroRefused);
    CHECK(CheckRate(0.0, 4.0f) == RateVerdict::ZeroRefused);

    // A known ceiling refuses above it, in BOTH directions -- reverse consumes the same budget.
    CHECK(CheckRate(4.0, 4.0f) == RateVerdict::Ok);          // exactly at the ceiling is achievable
    // Above the ceiling AS STATED. 4.001 and 4.0 are one rate to every reader of this
    // build, and a refusal cannot say one of them is above the other (L3-RUN-9 F2).
    CHECK(CheckRate(4.01, 4.0f) == RateVerdict::AboveCeiling);
    CHECK(CheckRate(-4.01, 4.0f) == RateVerdict::AboveCeiling);
    CHECK(CheckRate(4.001, 4.0f) == RateVerdict::Ok);
    CHECK(CheckRate(-1.0, 4.0f) == RateVerdict::Ok);

    // The six rates the panel offers, against the three ceilings the design names. This is the
    // table that decides which buttons grey out, so it is pinned rather than left to inspection.
    const float rates[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    for (float r : rates) {
        CHECK(CheckRate(r, 6.0f) == RateVerdict::Ok);         // 90 Hz: everything we offer fits
        CHECK(CheckRate(r, 0.0f) == RateVerdict::Ok);         // unknown: nothing refused
    }
    CHECK(CheckRate(4.0f, 4.8f) == RateVerdict::Ok);           // 72 Hz
    CHECK(CheckRate(4.0f, 3.5f) == RateVerdict::AboveCeiling); // a slower machine greys 4x
}

// L3-RUN-9 F2. At 30 Hz the ceiling is 66666 / 33334 = 1.99994, which this build states -- in the
// panel, in `info`, and in the refusal itself -- as 2.00x. Refusing 2.00x against it made the
// advertised maximum unplayable and printed a sentence that contradicted itself.
TEST_CASE("the ceiling this build STATES is playable, and a refusal names a bigger number") {
    const float ceiling = RateCeiling(facts::CLOCK_MAXELAPSED_US, 33334);
    REQUIRE(ceiling > 0.0f);
    REQUIRE(ceiling < 2.0f);                      // the raw ratio really is below the stated one
    CHECK(ReportedRate(ceiling) == doctest::Approx(2.00));

    CHECK(CheckRate(2.0, ceiling) == RateVerdict::Ok);
    CHECK(CheckRate(-2.0, ceiling) == RateVerdict::Ok);
    CHECK(CheckRate(2.01, ceiling) == RateVerdict::AboveCeiling);
    CHECK(CheckRate(-2.01, ceiling) == RateVerdict::AboveCeiling);

    // The property the sentence rests on, over the whole band a user can ask for: nothing is ever
    // refused for being above a number it is printed as equal to.
    for (int hundredths = 1; hundredths <= 800; ++hundredths) {
        const double r = hundredths / 100.0;
        if (CheckRate(r, ceiling) == RateVerdict::AboveCeiling) {
            CHECK(ReportedRate(r) > ReportedRate(ceiling));
        }
    }
}

TEST_CASE("an unconsulted source is Unknown, never a clean bill of health") {
    // ★ THE CELL A NATURAL IMPLEMENTATION GETS WRONG, and the reason this function takes two
    // arguments instead of one. A count of zero from a source nobody asked is NOT "everything is
    // available" -- it is "we do not know", and rendering it as the former is a false all-clear on
    // the one surface whose whole job is to prevent false all-clears.
    CHECK(ClassifyAvailability(false, 0) == Availability3::Unknown);
    CHECK(ClassifyAvailability(false, 7) == Availability3::Unknown);   // count is irrelevant

    // Consulted and genuinely empty is the only way to earn AllAvailable.
    CHECK(ClassifyAvailability(true, 0) == Availability3::AllAvailable);
    CHECK(ClassifyAvailability(true, 1) == Availability3::SomeUnavailable);
    CHECK(ClassifyAvailability(true, 19) == Availability3::SomeUnavailable);

    // Every state names itself; "unavailable" with no name is not a diagnosis.
    CHECK(strcmp(Availability3Name(Availability3::Unknown), "Unknown") == 0);
    CHECK(strcmp(Availability3Name(Availability3::AllAvailable), "AllAvailable") == 0);
    CHECK(strcmp(Availability3Name(Availability3::SomeUnavailable), "SomeUnavailable") == 0);

    // ★ The discriminating pair, stated as one assertion: the two zero-count inputs must NOT
    // collapse to the same answer. If they ever did, this whole function would be a rename of
    // `count == 0` and the trap would be back.
    CHECK(ClassifyAvailability(false, 0) != ClassifyAvailability(true, 0));
}

TEST_CASE("a round lookup by label reports absence rather than picking a neighbour") {
    Marker m[4];
    const uint16_t labels[] = {1, 2, 2, 5};   // note the duplicate: a round restarted after a reset
    for (int i = 0; i < 4; ++i) {
        m[i].tUs = (i + 1) * 1000000;
        m[i].index = labels[i];
    }

    CHECK(FindRoundByNumber(m, 4, 1) == 0);
    CHECK(FindRoundByNumber(m, 4, 5) == 3);
    // First match wins, so "go to round 2" reaches the earlier one.
    CHECK(FindRoundByNumber(m, 4, 2) == 1);

    // ★ A round that is not there must be kNoMarker, never a nearest neighbour and never 0 --
    // returning 0 would silently jump to round 1 for every mistyped number.
    CHECK(FindRoundByNumber(m, 4, 3) == kNoMarker);
    CHECK(FindRoundByNumber(m, 4, 0) == kNoMarker);
    CHECK(FindRoundByNumber(m, 4, 99) == kNoMarker);
    CHECK(FindRoundByNumber(m, 4, -1) == kNoMarker);

    // An empty or absent table is a miss, not a crash and not a match.
    CHECK(FindRoundByNumber(m, 0, 1) == kNoMarker);
    CHECK(FindRoundByNumber(nullptr, 4, 1) == kNoMarker);
}

TEST_CASE("FormatTime survives its own edges") {
    char b[32];

    FormatTime(0, b, sizeof b);
    CHECK(strcmp(b, "0:00.000") == 0);

    FormatTime(83456000, b, sizeof b);
    CHECK(strcmp(b, "1:23.456") == 0);

    FormatTime(-2500000, b, sizeof b);
    CHECK(strcmp(b, "-0:02.500") == 0);

    FormatTime(3900000000ll, b, sizeof b);
    CHECK(strcmp(b, "1:05:00.000") == 0);

    // INT64_MIN has no positive counterpart; negating it in a signed type is undefined behaviour,
    // so the implementation negates in unsigned. If it ever stops doing that this is where it dies.
    FormatTime(INT64_MIN, b, sizeof b);
    CHECK(b[0] == '-');
    CHECK(strlen(b) > 1);

    // A cap smaller than the text truncates and still terminates; it never writes past the buffer.
    char tiny[4];
    memset(tiny, 0x7F, sizeof tiny);
    FormatTime(83456000, tiny, sizeof tiny);
    CHECK(strlen(tiny) == 3);
    CHECK(tiny[3] == '\0');

    // A zero cap must write nothing at all rather than terminating a buffer it was told not to use.
    char guard[2] = {0x7F, 0x7F};
    FormatTime(1, guard, 0);
    CHECK(guard[0] == 0x7F);
}

TEST_CASE("CellEdgeUs is monotonic and closes exactly on the end") {
    for (int64_t dur : kDurations) {
        const Span s = Whole(dur);
        static const uint32_t kCellCounts[] = {1u, 2u, 7u, 115u, 256u};
        for (uint32_t cells : kCellCounts) {
            int64_t prev = s.beginUs;
            for (uint32_t i = 0; i <= cells; ++i) {
                const int64_t e = CellEdgeUs(s, cells, i);
                CHECK(e >= prev);
                prev = e;
            }
            CHECK(CellEdgeUs(s, cells, cells) == s.endUs);
            // Past the end clamps rather than running off the axis.
            CHECK(CellEdgeUs(s, cells, cells + 99) == s.endUs);
        }
    }
}

TEST_CASE("CellOfUs clamps rather than dropping a playhead off either end") {
    const Span s = {1000, 2000};
    CHECK(CellOfUs(s, 10, 0) == 0);
    CHECK(CellOfUs(s, 10, 1000) == 0);
    CHECK(CellOfUs(s, 10, 1999) == 9);
    CHECK(CellOfUs(s, 10, 2000) == 9);
    CHECK(CellOfUs(s, 10, 999999) == 9);
    // A zero-width span has exactly one answer and must not divide by its own duration.
    const Span empty = {5, 5};
    CHECK(CellOfUs(empty, 10, 5) == 0);
    CHECK(CellOfUs(empty, 10, 99) == 0);
}
