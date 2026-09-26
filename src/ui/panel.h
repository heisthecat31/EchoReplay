#pragma once

// W2-F. The panel: what gets drawn, once per frame, from the band-3 component-system callback.
//
// Everything here runs on the engine main thread inside an open GUI frame. csregister guarantees
// both -- it refuses to call Draw() at all when the callback lands on a worker -- so this file
// never checks a thread and never takes a lock.

#include <stdint.h>

#include "bootstrap/services.h"
#include "ui/demoindex.h"
#include "ui/inframe.h"
#include "ui/legacyimport.h"

namespace replay {
class IReplayCameraControl;
}

namespace replay {
namespace ui {
namespace panel {

// ---- WHAT ONE FRAME OF THIS PANEL IS ALLOWED TO HAND THE ENGINE'S ImGui -----------------------
//
// The facade has no scrolling child window, no clipper and no way to ask ImGui what is visible, so
// the panel decides that itself: it draws ONE PAGE of browser rows -- the rows that fit the window
// it pins -- and cuts every drawn line to the width that window can show. Both numbers are here
// rather than in panel.cpp because the offline bench scores the facade traffic one frame produces,
// and a bench with its own copy of the page size cannot see the page size change.
//
// Every widget the panel issues costs the engine an ItemAdd, an ID hash and a CalcTextSize over
// the whole label -- ImGui measures text it then clips -- so rows nobody can see and characters
// past the right edge are paid for at full price and read as nothing.
constexpr float kPanelHeightPx = 430.0f;
constexpr float kContentWidthPx = 690.0f;
// ImGui's default 13px font at its default FramePadding and ItemSpacing: one text line plus one
// framed row is about this tall. It sizes the page, so it is written down once.
constexpr float kLinePitchPx = 21.0f;
// The default font's advance for the characters these lines are made of.
constexpr float kGlyphAdvancePx = 7.0f;
// Title bar, the header line and its two buttons, the separators, the pager and the status line.
constexpr float kBrowserChromePx = 120.0f;
// A row is its button and its verdict line; a faulted row adds a third that most rows do not have.
constexpr size_t kBrowserLinesPerRow = 2;

constexpr size_t kBrowserRowsPerPage = static_cast<size_t>(
    (kPanelHeightPx - kBrowserChromePx) / (kLinePitchPx * kBrowserLinesPerRow));
constexpr size_t kRowCharBudget = static_cast<size_t>(kContentWidthPx / kGlyphAdvancePx);
// Every line the pinned window has room for. It is the ceiling on a frame: a draw that hands ImGui
// more text than this is measuring characters that are off the bottom or off the right of a window
// the panel itself sized, and paying full price for them.
constexpr size_t kLinesOnScreen = static_cast<size_t>(kPanelHeightPx / kLinePitchPx);

// ---- THE STREAM ONE BROWSER FRAME IS ALLOWED TO HAND THE ENGINE ------------------------------
//
// L5 #11 measured imguiNanos = 208 500 of a 209 100 ns draw: 99.7% of the frame is the engine's
// own NRadGui issuing the widgets we name, and nothing on our side of the facade is left in it.
// The only lever left is the STREAM -- how many widgets and how many characters a frame asks for.
// Both numbers are declared here, derived from the layout rather than from a run, so a draw that
// grows an item breaks a compile-time budget instead of a launch.
//
// The layout, exactly: a two-line header block (the listing/pager line and the status line, one
// Text between them), Rescan, Close, one Button per row on the page, < Prev and Next >.
constexpr size_t kBrowserHeaderLines = 2;
constexpr size_t kBrowserChromeItems = 6;   // header block, Rescan, Close, < Prev, Next >,
                                            // playable-only toggle
// The five chrome button labels as ImGui measures them: everything from "##" on is an ID.
constexpr size_t kBrowserChromeToggleChars = 18;   // "playable only: OFF", the wider of the two
constexpr size_t kBrowserChromeButtonChars =
    23 + kBrowserChromeToggleChars;                // Rescan + Close + < Prev + Next > + the toggle
// ---- THE ROUNDS STRIP (W2-F PANEL4) ---------------------------------------------------------
// At most this many dedup'd buttons, then one tail stating how many did not fit. Published
// because the bench asserts the page's stream against it and a budget only one side knows is a
// budget nothing can be measured against.
constexpr size_t kMaxRoundButtons = 12;
// One "rounds:" text, the buttons, and at most one tail.
constexpr size_t kRoundStripItemBudget = 1 + kMaxRoundButtons + 1;
// "rounds:" is 7. The widest label is 10: a two-character glyph, the five digits a uint16 round
// number can reach on the two kinds that carry one, and "x" plus a run of at most the 64 markers
// the panel holds. The tail is at most "...+52".
constexpr size_t kRoundStripGlyphBudget = 7 + kMaxRoundButtons * 10 + 6;

// ---- THE BAR'S ROW (UI-2 P2 S10) --------------------------------------------------------------
// Two configurations, budgeted separately because they behave differently: the owner's default
// draws the bar alone and the agents' (`ui.debug_panel=true`) draws both in the same frame. The
// bar's track is 3 px cells across at most kBarMaxWidthPx, which the timeline's own kMaxCells (256)
// caps; the rest is one glyph row, the control row and the menu when it is open.
constexpr float kBarMaxWidthPx = 1536.0f;
constexpr size_t kBarTrackCells = 256;
constexpr size_t kBarMarkerLabels = 12;   // LayoutMarkerLabels drops the rest
constexpr size_t kBarControlItems = 22;   // name, =, time, state, hint, 8 transport, 5 speeds,
                                          // Open, Unload, and three of slack for the rows that
                                          // only appear when the window is wide enough
constexpr size_t kBarMenuItems = 16;      // open, unload, up to 6 rates, 6 toggles, 2 width
constexpr size_t kBarItemBudget =
    1 + kBarMarkerLabels + kBarTrackCells + kBarControlItems + kBarMenuItems;
// Cells are "##b<n>": everything from "##" on is an ID and is not measured. What IS measured is the
// glyph row, the control labels, the time readout and the menu's rows.
constexpr size_t kBarGlyphBudget = kBarMarkerLabels * 6 + kBarControlItems * 12 +
                                   kBarMenuItems * 20 + 64;

constexpr size_t kBrowserItemBudget = kBrowserRowsPerPage + kBrowserChromeItems;
// Each drawn line is cut to kRowCharBudget; the header block's two lines are one string with a
// newline between them, and ImGui measures that newline too.
constexpr size_t kBrowserGlyphBudget =
    (kBrowserRowsPerPage + kBrowserHeaderLines) * (kRowCharBudget + 1) + kBrowserChromeButtonChars;

// The combined ceiling for one frame with `ui.debug_panel=true`: the demoted panel's heaviest page
// plus the bar. This is the configuration every diag-driven launch runs in, so it is the one that
// has to hold.
constexpr size_t kBarAndPanelItemBudget = kBarItemBudget + kBrowserItemBudget;
constexpr size_t kBarAndPanelGlyphBudget = kBarGlyphBudget + kBrowserGlyphBudget;

// The demoted panel's TRANSPORT page, which never had a published budget of its own: two 115-cell
// strips at 6 px over kContentWidthPx, the marker glyph rows, the rounds strip, the bookmark rows
// and the four control rows. tests/ui asserts the drawn stream against it with `ui.debug_panel` on,
// which is the configuration every diag-driven launch runs in.
constexpr size_t kPanelTransportItemBudget = 2 * 116 + 2 * 33 + kRoundStripItemBudget + 96;

// The row's own line, pre-joined on the tick: a verdict mark, then fixed-width columns, then
// whatever reason fits. One widget per row whatever the row's verdict is -- a refused row used to
// cost a second Text, so a page of refused rows cost twice a page of loadable ones.
constexpr size_t kRowNameChars = 34;
constexpr size_t kRowDurationChars = 8;
constexpr size_t kRowCountsChars = 19;   // "1 rounds, 3 players": 18 cut the final s
// MULTI-LOAD: WHERE, beside how long and who. The name column already carries when -- these file
// names open with the capture's own timestamp -- so the level is the one thing the row could not
// say, and it is what decides whether opening the capture re-enters the session.
constexpr size_t kRowLevelChars = 18;      // "combat_combustion", the longest after the mpl_ cut
// "x " / "? " / "> " / "  ". It is the first thing on the line so that the cut can never eat it.
constexpr size_t kRowMarkChars = 2;

// Bound once at module init. `index` is borrowed and outlives the panel.
void Init(ReplayServices* services, DemoIndex* index);

// Called from the stage-0 tick, on the tick's own thread, BEFORE the frame the panel draws into.
// This is where every read of engine or transport state happens; Draw() only renders what this
// left behind. Splitting them is what keeps a facade call from ever sitting between two reads of a
// playhead that moved in between.
void Sample(uint64_t frameDeltaUs);

// Called from the row-2.5 in-frame draw site (src/ui/inframe.h), and from the owner-tick fallback
// while that detour is unarmed. Draws, and nothing else.
//
// The return value is NOT a courtesy. NRadGui::Begin refuses when the engine's frame is shut, and a
// caller that discarded the answer counted a refused draw as a draw -- AUDIT_F3J F3J-4, and the
// defect ARCH-15 was written about. Drawn / NotShowing / FacadeRefused are three different answers
// and the site scores each one differently.
inframe::DrawOutcome Draw();

void Shutdown();

// modreg pre-exit hook (RadPluginShutdown, game thread, outside the loader lock): asks the browser
// worker to stop and waits at most `budgetMs` for it. Process exit needs neither this nor Shutdown
// to have run -- the worker holds nothing whose destructor runs at static teardown.
void StopWorkerForExit(uint32_t budgetMs);
bool BrowserWorkerRunningForTest();

// Test seam: where Sample() reads the toggle key's physical state. The live read is GetAsyncKeyState
// plus a foreground check, which no offline test can drive, and the wiring from key edge to panel
// visibility to cursor is the half that was missing rather than the read itself. Null restores the
// live source.

// Test seam: the camera control the camera row drives, in place of camera_control::Get(), which
// is only available inside the game. Resets the selector. Null restores the live one.
void SetCameraForTest(IReplayCameraControl* cam);

// Test seam: block until the browser worker has published a snapshot for every rescan asked for so
// far, or `timeoutMs` passes. False on timeout. The worker exists so that a folder scan never
// happens on a thread the game renders on; a test that wants to see the listing therefore has to
// wait for it, and a test that forgets to gets an empty page rather than a stale one.
bool WaitForBrowserForTest(uint32_t timeoutMs);

// The browser's playable-only filter, which the chrome toggles. Exposed so a case can assert both
// sides of it: with the filter on the worker classifies the WHOLE folder (one manifest per file,
// once, cached), and with it off nothing past the drawn page is opened at all.
void SetPlayableOnlyForTest(bool on);
bool PlayableOnlyForTest();
// The header block's first line, as BuildHeaderBlock left it -- already cut to kRowCharBudget. A
// test reads it to prove the pager sentence still fits the one width this panel is drawn at.
const char* HeaderLineForTest();

// FORMATS-P6. Replaces the linked converter (null restores it); a test never runs the real one.
void SetConverterForTest(legacy::ConvertFn convert);
bool ImportBusyForTest();
// The panel's own status sentence. StatusLine() hands csregister's diagnosis first, and in a
// process with no engine that is all it ever says.
const char* StatusForTest();
// True once the conversion worker has finished, or when none is running; false on timeout.
bool WaitForImportForTest(uint32_t timeoutMs);

}  // namespace panel
}  // namespace ui
}  // namespace replay
