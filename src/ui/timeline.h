#pragma once

// W2-F. The timeline's arithmetic, with no facade call in it.
//
// ---------------------------------------------------------------------------------------------
// * WHY THE SCRUB BAR IS A STRIP OF BUTTONS AND NOT A SLIDER, stated here because it is the first
// question anyone will ask of this file.
//
// echovr.exe does not contain a slider. The NRadGui facade declares SliderReal, DragFloat,
// Selectable, BeginChild and IsMouseHoveringRect in libr15, and the PC build linked none of them --
// the linker kept only what the game's own four debug windows and its dialogue UI call. The
// evidence is in the W2-F fact drop: each facade function carries its Begin/End balance token
// as a string literal, and echovr.exe holds six of them. There is no mouse-position query either,
// so a drag handle cannot be implemented at all through this facade.
//
// What IS there is Button(label, size) returning "was clicked", and SameLine(absoluteX, spacing).
// Those two compose into a click-to-seek strip: N contiguous fixed-width buttons across the panel,
// each seeking to its own point, with round markers and the playhead distinguished by cell HEIGHT
// (Button takes an explicit size) rather than by colour -- because PushStyleColor's first argument
// is an EGuiColors enum whose values this packet has not derived, and calling it with a guessed
// index would colour something arbitrary.
//
// * THE COARSE STRIP ALONE IS NOT ENOUGH, AND THAT IS WHY THERE ARE TWO. A 120-cell strip over a
// ten-minute match is 5 s per cell. So the panel draws a second strip over a window around the
// playhead (default +-10 s, a config key), which is sub-second per cell, and an exact InputReal
// seconds box beside them. Coarse to get there, fine to land.
//
// * THE TWO PROPERTIES THIS FILE EXISTS TO HOLD, and they are NOT the same property.
//
//   P1, round trip: CellOfUs(SeekTargetForCell(i)) == i. Clicking cell i must put the playhead in
//   cell i, or the handle appears to jump somewhere other than where you clicked.
//   P2, endpoint reachability: SeekTargetForCell(0) == beginUs and SeekTargetForCell(cells-1) ==
//   endUs, for cells >= 2. Every instant of the capture must be reachable from the strip.
//
// ★ ONE GRID, NOT TWO, AND THE FIRST ATTEMPT HERE HAD TWO. CellEdgeUs is the only definition of
// where a cell starts; CellOfUs INVERTS IT BY BINARY SEARCH rather than by the algebraic inverse
// (t - begin) * cells / dur. Those two are not the same function: both truncate and the
// truncations do not cancel. With dur = 10 and cells = 4 the edges are 0, 2, 5, 7 while the closed
// form maps 2 back to cell 0, so a click on cell 1 puts the playhead in cell 0. The sweep in
// tests/ui found that on the first run, over 68,800 assertions; it is recorded here because the
// closed form is what anyone would write, and it looks right.
//
// SeekTargetForCell is then the cell's own left edge -- which is inside the cell by construction,
// so P1 cannot fail -- with one exception: the LAST cell seeks to endUs, which is what buys P2.
//
// ★ P1's REAL SCOPE, stated rather than assumed. When a strip has more cells than the span has
// microseconds, two cells share an edge and no function can tell them apart; P1 is then not merely
// unmet, it is unmeetable. It therefore holds exactly when the edges are strictly increasing,
// dur >= cells, and tests/ui asserts that case and the degenerate one separately -- in the
// degenerate case the weaker true statement is that the round trip lands on the same INSTANT, even
// if on a different index.
//
// * WHICH PROPERTY THE RED ARM DISCRIMINATES, stated exactly, because getting this wrong is how a
// test comes to be believed for the wrong reason. SeekTargetForCellSingleDenominator -- every
// cell, including the last, seeking to its own left edge, which is how this comes out if you only
// think about drawing the strip -- PASSES P1 completely. What it fails is P2: its last cell seeks
// to endUs - duration/cells, so the final cell's worth of the capture cannot be reached by any
// click. That is invisible in a screenshot and surfaces months later as "the scrubber will not go
// to the end". tests/ui asserts the mutant PASSES P1 and FAILS P2 -- both halves, because a red
// arm that failed everything would not tell us which property the sweep is measuring.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace ui {

// A half-open span of replay time, in microseconds on the file's own axis. beginUs <= endUs.
struct Span {
    int64_t beginUs = 0;
    int64_t endUs = 0;

    int64_t DurationUs() const { return endUs > beginUs ? endUs - beginUs : 0; }
    bool Empty() const { return endUs <= beginUs; }
};

// A strip never grows past this, whatever width the panel is given. 256 buttons is already more
// facade calls per frame than the whole rest of the panel; past it the cells are sub-pixel and the
// fine strip is the right tool anyway.
inline constexpr uint32_t kMaxCells = 256;

// How many cells fit. Always >= 1, so a strip is never zero-width and no caller divides by zero.
uint32_t CellCount(float widthPx, float cellPx);

// The left edge of cell i, for drawing. i is clamped to [0, cells]; CellEdgeUs(cells) == endUs.
int64_t CellEdgeUs(const Span&, uint32_t cells, uint32_t i);

// Which cell contains t. Clamped into [0, cells-1] at both ends, so a playhead outside the span
// still marks the nearest cell rather than vanishing.
uint32_t CellOfUs(const Span&, uint32_t cells, int64_t tUs);

// Where clicking cell i seeks to. Cell 0 is exactly beginUs and cell cells-1 is exactly endUs.
int64_t SeekTargetForCell(const Span&, uint32_t cells, uint32_t i);

// * THE RED ARM, compiled into the shipping tree on purpose. The same function written with one
// denominator -- the way it comes out if you only think about drawing and not about clicking. It
// is correct for every cell EXCEPT that the last (endUs - duration/cells) of the file cannot be
// reached by any click, which is invisible in a screenshot and shows up as "the scrubber will not
// go to the end". tests/ui asserts it FAILS the round trip the real one passes; if it ever stopped
// failing, the sweep would have lost its discriminating power and the test says so.
int64_t SeekTargetForCellSingleDenominator(const Span&, uint32_t cells, uint32_t i);

// The window the fine strip covers. Keeps a CONSTANT width and slides against the ends rather than
// shrinking, so the seconds-per-cell a user has learned does not change as they approach t=0.
Span FineSpan(int64_t playheadUs, int64_t durationUs, int64_t windowUs);

// What a cell is, which the panel turns into a height. Deliberately not a colour -- see the header
// -- EXCEPT through CellColour below, which UI-2 added when PushButtonDisabled turned out to take
// its colour as four floats and to leave the button clickable (panel.cpp's greyed browser rows).
//
// ★ Elapsed is APPENDED, not inserted. These values are compared by number in tests/ui and drawn as
// heights by the panel; renumbering Marker or Playhead would move a tick's height, not its meaning.
enum class CellClass : uint8_t {
    Normal = 0,
    Marker,            // a round boundary falls in this cell
    Playhead,
    MarkerPlayhead,    // both, and the caller must be able to see that it is both
    Elapsed,           // UI-2: behind the playhead. The bar's only use of colour to carry progress
};

// UI-2 S3.1: the bar's cell width. The panel's strips keep their own 6 px.
inline constexpr float kBarCellPx = 3.0f;

// Which tick family a capture marker belongs to, which is what decides its colour and whether
// `ui.bar_markers` draws it at all. Everything the owner called clutter -- joins, leaves, team
// changes, pauses -- maps to None and is not drawn.
enum class MarkerFamily : uint8_t { None = 0, Round, Goal, Bookmark };

MarkerFamily FamilyOfKind(uint16_t kind);

// Straight RGBA, so this file still links into a test with no facade in it. gui::Vec4 has the same
// four floats and the bar converts at the call.
struct Rgba {
    float r, g, b, a;
};

// The colour of one cell. `family` is meaningful only for the two marker classes; the caller
// resolves an overlap (goal > round > bookmark) before it asks.
Rgba CellColour(CellClass, MarkerFamily family);

// One marker on the axis. `index` is the round number the label shows.
struct Marker {
    int64_t tUs;
    uint16_t index;
    uint16_t kind;   // replay::capture::MarkKind; 0 is no kind, which draws as unknown
};

// Fills `out[0..cells)` with each cell's class. Markers outside the span are skipped, never
// clamped onto the end cell -- a round that is not in this window must not draw a tick that says
// it is. Returns how many markers actually landed inside.
uint32_t ClassifyCells(const Span&, uint32_t cells, const Marker* markers, size_t markerCount,
                       int64_t playheadUs, bool playheadInSpan, CellClass* out);

// ClassifyCells, plus: every plain cell before the playhead's cell becomes Elapsed. Marker and
// playhead cells keep their own class, because a tick behind the playhead is still a tick.
uint32_t ClassifyCellsElapsed(const Span&, uint32_t cells, const Marker* markers, size_t markerCount,
                              int64_t playheadUs, bool playheadInSpan, CellClass* out);

// A label to draw above the strip: `x` is pixels from the strip's left edge.
struct MarkerLabel {
    float x;
    uint16_t index;
    uint16_t kind;
};

// Lays out round labels along `widthPx`, dropping any that would overlap one already placed --
// overlapping labels render as unreadable mush and hide the very markers they exist to announce.
// Returns how many were placed; `dropped` receives how many were suppressed for lack of room, so
// the panel can say "3 more" instead of silently showing fewer rounds than the file has.
size_t LayoutMarkerLabels(const Span&, float widthPx, float minSpacingPx, const Marker* markers,
                          size_t markerCount, MarkerLabel* out, size_t outCap, size_t* dropped);

// ---- WHAT KIND OF MARKER THIS IS, ON THE SCREEN (W2-F PANEL4) --------------------------------
// The strip used to print "R" plus RoundMarker::index for every marker of every kind. On a solo
// -offline host the engine's round counter never leaves 0, so twelve markers of six kinds all read
// R0: the data was right and the label threw the kind away. These three give the kind a symbol.

// The glyph for one kind -- ASCII, at most two characters, because the panel counts the characters
// it hands the facade and a multi-byte glyph would make that count a lie. A kind this table does
// not know gets its own symbol; it never falls through into a neighbour's.
const char* RoundGlyph(uint16_t kind);

// The label for one marker: its glyph, the round number for the two kinds that carry one, and
// "xN" when N consecutive markers said the same thing. Only ROUND_BEGIN and ROUND_END carry a
// round number at their instant -- printing "G0" would be a claim the file does not make.
// Returns the characters written, never more than cap-1.
size_t FormatRoundLabel(uint16_t kind, uint16_t index, uint32_t runLength, char* out, size_t cap);

// One button on the rounds strip. A run of markers with the same kind AND index is one entry.
struct RoundEntry {
    char label[16];
    size_t sourceIndex;   // the FIRST marker of the run: what the button seeks to and ids on
    uint16_t index;
    uint32_t runLength;
};

// Collapses runs, fills at most outCap entries, and reports through `clamped` how many collapsed
// entries did not fit -- a strip that silently showed fewer rounds than the file has would say the
// round is not there. Returns how many entries were written.
size_t BuildRoundStrip(const Marker* markers, size_t markerCount, RoundEntry* out, size_t outCap,
                       size_t* clamped);

// The engine's fast-forward ceiling: CLOCK.MAX_ELAPSED / the raw frame period. Returns 0 when the
// frame period is not known yet, which the caller must render as "unknown" -- NOT as 4.0. The
// ceiling is 4.0 only at 60 Hz, and a UI that hardcodes it lies on a 90 Hz machine.
float RateCeiling(uint32_t maxElapsedUs, uint64_t rawFramePeriodUs);

// Whether a requested playback rate can actually be delivered. The policy lives here rather than in
// the panel so it is testable without a game, and so the console and the panel refuse the same
// rates for the same reasons out of one function instead of two that drift.
enum class RateVerdict : uint8_t {
    Ok = 0,
    ZeroRefused,    // S7.3.3: the clock reciprocates the scaled delta, so zero yields +inf
    AboveCeiling,   // past it the engine stops accelerating and does not report it
};

// ★ AN UNKNOWN CEILING REFUSES NOTHING. When `ceiling` is 0 the frame period has not been measured,
// so we do not know what is achievable; refusing on a number we do not have would block a rate that
// may work perfectly. The caller says "ceiling unknown" rather than implying a limit -- and never
// substitutes 4.0, which is the ceiling at 60 Hz alone.
// A rate as this build STATES it: two decimals, the only precision the panel, the console and
// every refusal sentence ever print. CheckRate compares these numbers rather than the raw ratio,
// so a refusal's own sentence is true as written. Sign is preserved; magnitude is what rounds.
double ReportedRate(double rate);

RateVerdict CheckRate(double rate, float ceiling);

// The rates the transport OFFERS, derived from the ceiling instead of listed.
//
// * A BUTTON THE CLAMP WILL REFUSE IS A LIE THE UI TELLS ITSELF. The panel drew a fixed ladder
// ending in 4x on a machine whose measured ceiling is 2.00x, directly above the sentence stating
// that ceiling (L3-RUN-10 F4). Deriving the set here means the row cannot contradict the sentence:
// every rate returned satisfies CheckRate(rate, ceiling) == Ok.
//
// A known ceiling is itself always offered, rounded to the two decimals the sentence states, so the
// fastest playback the machine can actually do is reachable even when it is not a round number. An
// unknown ceiling (<= 0) refuses nothing, so the whole ladder is offered -- the same answer
// CheckRate gives.
//
// Writes at most `cap` rates and returns how many; kOfferedRateCap holds the ladder plus a ceiling.
constexpr size_t kOfferedRateCap = 8;
size_t OfferedRates(float ceiling, float* out, size_t cap);

// Whether a set of "things that are unavailable" can be believed to be complete.
//
// * AN EMPTY UNAVAILABLE-LIST READS AS "EVERYTHING IS AVAILABLE", AND THAT IS THE TRAP. A source
// that has not been consulted yet, a registry nothing has registered into, and a registry where
// genuinely nothing is degraded all produce a count of zero -- and only the last one means the
// replay is telling the whole truth. Rendering the first two as a clean bill of health is a false
// all-clear on the exact surface that exists to prevent false all-clears.
//
// So the count alone is never the input. `sourceConsulted` must come from the caller having
// actually reached the registry, and when it is false the answer is Unknown WHATEVER the count --
// which makes "0 means fine" unrepresentable rather than merely discouraged. tests/ui asserts the
// zero-count-not-consulted cell specifically, because that is the one a natural implementation
// gets wrong.
enum class Availability3 : uint8_t {
    Unknown = 0,        // nobody asked, or there was nothing to ask
    AllAvailable,       // asked, and the answer was genuinely none
    SomeUnavailable,    // asked, and there are this many
};

Availability3 ClassifyAvailability(bool sourceConsulted, size_t unavailableCount);
const char* Availability3Name(Availability3);

// Index of the first marker labelled `roundNumber`, or kNoMarker. First match wins: a capture may
// carry two markers with the same label (a round restarted after a reset), and the earlier one is
// what "go to round 2" means.
static const size_t kNoMarker = (size_t)-1;
size_t FindRoundByNumber(const Marker* markers, size_t count, int64_t roundNumber);

// "1:23.456", "-0:02.500", "1:05:00.000". Always NUL-terminates. Signed, because a reverse
// playhead legitimately runs below t0 and B.8 types PlayheadUs as int64 for exactly that reason.
void FormatTime(int64_t tUs, char* out, size_t cap);

}  // namespace ui
}  // namespace replay
