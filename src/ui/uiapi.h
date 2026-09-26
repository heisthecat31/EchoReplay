#pragma once

// W2-F. The seam W2-G's console and -replayviewer call, and the only way in from outside src/ui.
//
// DESIGN section 13 gives src/control/ -- the console window, the -replayviewer switch and the HTTP
// routes -- to W2-G, and the panel, timeline and browser to W2-F. The owner's three entry points
// (a console command to a file, a launch switch with a file menu, and the in-session UI) therefore
// meet HERE: W2-G owns getting the request, this file owns what the request does. Nothing under
// src/control/ is written by this packet, and nothing in src/ui parses a command line.
//
// Every function is safe to call before the facade has bound, before a game space exists and after
// shutdown. A request that cannot be served returns false and leaves StatusLine() saying why --
// W2-G's console prints that string verbatim, so the console and the panel never disagree about
// what happened.

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace ui {

// The in-session panel.
//
// It starts HIDDEN and is brought up by the hotkey below, by opening the browser, or by loading a
// capture. `ui.panel` is the FEATURE switch (ledger row 0.20) and is a separate question from
// whether the window is on screen right now: a panel that drew over ordinary gameplay from the
// moment the game had a window is against the owner's "it will look the same as it did before",
// and one that could only be dismissed through its own close box could never be brought back.
void ShowPanel(bool visible);
bool PanelVisible();
void Toggle();
// False when `ui.panel` is off, in which case ShowPanel and Toggle do nothing and say so.
bool PanelEnabled();
// The key that toggles the panel, for the surfaces that have to tell the owner about it. Never
// null; a name, not a virtual-key code, because every reader of it prints it.
const char* PanelHotkeyName();

// Opens the browser view and rescans the replay folder. Safe to call repeatedly; this is what
// `-replayviewer` with no path lands on, and what the console's `list` shows.
void OpenBrowser();
bool BrowserOpen();

// ★ DESIGN 8.4.3's entry table, and the reason it lives on an enum rather than in a comment.
//
// A capture recorded in a level the running session is not in loads perfectly: Transport::Load is
// file I/O and succeeds anywhere. The timeline populates, the playhead moves, every button works,
// and the world on screen is the wrong one -- a failure that looks exactly like success, which is
// worse than a crash because nothing prompts anyone to look.
//
// So the decision has a name, one place decides it, and the answer is carried where the panel, the
// console and `diag` can all read it. NotChecked is a distinct value on purpose: "we could not ask"
// and "we asked and it matches" must never render the same.
enum class EntryVerdict : uint8_t {
    NotChecked = 0,  // no ISessionEntry has registered, or the capture's manifest could not be read
    Matches,         // the running session is already in this capture's level and gametype
    MustEnter,       // it is not, and the strategy CAN change level -- a decision, not an outcome
    Requested,       // MustEnter, and ISessionEntry::RequestEntry accepted; entry lands on the tick
    Refused,         // it is not and the strategy cannot change level: the load is refused
    RequestFailed,   // the strategy could change level and RequestEntry itself refused
    // ** "NOT YET" IS NOT "NO" (P10-C s4, ENG-W1F-L3F1 s3). RequestEntry answers false at NetGame
    // states 1 and 2 because the session is still being BUILT, and EntryRefusal::NotInLevelYet
    // says exactly that. Filing it as RequestFailed retires a capture the session will accept two
    // ticks later -- the two-state-gate defect at the entry point. Appended rather than inserted:
    // the values above are already written into records.
    EntryDeferred,   // RequestEntry said not-yet; the file is kept and the request is re-offered
};

const char* EntryVerdictName(EntryVerdict);

// The verdict the most recent LoadPath reached, and the sentence behind it. The reason is never
// null and never empty, including for Matches -- "it is fine" is a statement, and the surfaces
// print it rather than inferring silence.
EntryVerdict LastEntryVerdict();
const char* LastEntryReason();

// Loads one capture through IReplayTransport::Load, after consulting ISessionEntry the way
// DESIGN 8.4.3 requires: a file whose level and gametype the running session cannot reach is
// REFUSED with a named reason, never attempted. False also when no transport has registered yet.
//
// This is the ONE funnel. The browser's greyed rows are a preview of the same decision, taken by
// the same function, so a row the browser offers and a path the console hands us cannot get
// different answers -- and `-replayviewer` and `load <path>`, which reach no browser at all, are
// checked here rather than nowhere.
bool LoadPath(const wchar_t* path);

// A .echoreplay or .tape given to LoadPath is converted on a worker thread first (DESIGN-FORMATS
// 7.8), so LoadPath answers false with nothing loaded yet. This says the request was taken: true
// from then until the converted capture is loaded, or the conversion fails or is cancelled.
bool ImportInProgress();

// `-replayviewer` with no path: DESIGN 8.4.3 says enter from the most recent capture and open the
// browser. Scans the replay folder, loads the newest readable file, and opens the browser either
// way -- a folder with nothing loadable in it must still show the user the folder.
bool LoadMostRecent();

// Drops the loaded capture and every piece of state derived from it, including the round markers.
// Safe when nothing is loaded.
void Unload();

// The full path of the capture currently open, or NULL when none is. Null rather than an empty
// string, because "no capture is loaded" and "a capture with an empty path" must not be the same
// answer to a caller deciding whether it may write beside that file.
//
// ★ THIS EXISTS FOR W2-G's `mark`. A bookmark is written to a sidecar named after the capture, so
// the command needs the path of whatever is actually playing -- and a capture opened from the
// PANEL was, until this accessor, unknown to the control surfaces. W2-G refused correctly in that
// case rather than writing a bookmark beside the wrong file, but the refusal fell on the owner's
// most likely route: open it in the browser, then mark it. One accessor, and the sidecar works
// from either surface. Valid until the next successful LoadPath or Unload.
const wchar_t* LoadedPath();

// ---- the owner's bookmarks, drawn where he made them -----------------------------------------
//
// ★ THE DIRECTION IS control -> ui, WHICH IS THE DIRECTION EVERY OTHER CALL ON THIS SEAM ALREADY
// RUNS. W2-G owns the sidecar: it parses it, caches it on the tick, and refuses a damaged one whole.
// Reading that file a second time from here would put two readers on one file and duplicate the
// damage handling; including src/control from src/ui would also make the dependency a cycle, since
// W2-G's `load`, `mark` and `-replayviewer` all call into this header. So W2-G PUSHES, exactly as
// it already calls LoadPath and LoadedPath.
//
// Until it does, the panel says "not supplied" -- see NotesState below. It does NOT say "no
// bookmarks", because that is the false all-clear this packet has spent its whole existence making
// unrepresentable, and here it would tell the owner his marks are gone.

struct BookmarkView {
    // On the PLAYHEAD's axis -- the same 0..duration the timeline draws and `mark` recorded from --
    // not the capture's absolute t0. The two differ by t0Us and a bookmark on the wrong axis lands
    // in a plausible wrong place, which is the failure mode round markers already have a type for.
    int64_t tUs;
    const char* label;   // UTF-8, may be empty. Borrowed for the duration of the call only.
};

// Four answers, because the three the sidecar can be in do not include "nobody told us".
enum class NotesState : uint8_t {
    NotSupplied = 0,  // no control surface has pushed anything yet. NOT "there are none".
    Absent,           // there is no sidecar beside this capture: nothing has ever been marked
    Ok,
    Damaged,          // it exists and does not parse, and W2-G has left it alone
};

const char* NotesStateName(NotesState);

// Hands the panel the bookmark table of the capture that is loaded. Tick thread only, like every
// other replay::ui call. COPIED, not borrowed: the panel outlives the caller's cache refresh.
// `resumeUs` is the sidecar's lastPlayheadUs, or 0 when it holds none.
void SetBookmarks(NotesState state, const BookmarkView* items, size_t count, int64_t resumeUs);

// What the panel currently holds, so a surface can tell whether its push arrived.
NotesState BookmarksState();
size_t BookmarkCount();

// Ground truth (false, the default) or as-seen. Mirrors IReplayTransport::SetMode so the console's
// `mode` command and the panel's checkbox cannot disagree about which is active.
void SetAsSeen(bool asSeen);
bool AsSeen();

// Motion smoothing, forwarded to IReplayTransport. MotionSmoothing() is strength << 8 | mode, and 0
// when no transport offers it.
void SetMotionSmoothing(uint8_t mode, uint8_t strength);
uint16_t MotionSmoothing();

// Where the browser looks. Defaults to <install root>\echoreplay\replays. Never null.
const wchar_t* ReplayDirectory();

// ---- what W2-G's command table needs and IReplayTransport alone cannot answer ----------------
//
// src/control/command.h parses one command set for all three surfaces. Four of those commands
// cannot be served by calling the transport directly without reintroducing a defect the panel
// already fixed, so they route through here instead:
//
//   speed  the engine's fast-forward ceiling is 66666 / the raw frame period, and ABOVE IT
//          FAST-FORWARD SILENTLY STOPS ACCELERATING. A console that called Play(8.0) would appear
//          to work and would not. SetRate refuses and names the ceiling; the panel greys the same
//          rates for the same reason, from the same number.
//   seek   percent needs the duration, and every seek must land on the one Seek path so the
//          playhead the panel shows is the one it published.
//   round  Command::whole carries "the number the panel labels" -- RoundMarker::index, from the
//          manifest the browser read. B.8's SeekRound(int) does not say whether its argument is
//          that number or a position in Manifest::rounds, and the two diverge on a capture that
//          starts mid-match. SeekRoundNumber takes the labelled number and seeks to the recorded
//          time, so the console and the panel's round buttons cannot disagree.
//   diag   the facade bind report, the band-3 registration status and the browser's scan cost live
//          here; StatusLine is one line, and `diag` wants all of it.

// 0.0 when no frame period has been measured yet. A caller must render that as UNKNOWN and must
// not substitute 4.0 -- that is the ceiling at 60 Hz only.
float RateCeiling();

// False when the rate cannot actually be achieved, with `why` set to a sentence naming the number.
// A rate of zero is always refused: the clock derives a reciprocal from the scaled delta, so zero
// yields +inf into every consumer. Pause is the pause bit, never a zero rate.
bool RateIsAchievable(double rate, const char** why);

// Applies `rate` after RateIsAchievable. Signed; negative is reverse. False and unchanged if the
// rate was refused.
bool SetRate(double rate, const char** why);

// ---- what the bar drives (UI-2 P2) ------------------------------------------------------------
//
// The bar issues no transport call of its own: everything it does comes through this header, which
// is the same funnel the console verbs use, so the two surfaces cannot drift apart about what
// "play" means or about which refusals are counted.

// Resumes at the transport's current signed rate. False with nothing loaded, and the refusal is
// counted like every other. Never resumes at a rate of zero.
bool Play();
bool Pause();
// Recorded ticks, not seconds: Step is the only unit in which "one frame back" is exact.
bool Step(int frames);

// `ui.debug_panel` at runtime -- the old panel as an opt-in window, and the console's
// `debug panel on|off`. IConfig is an immutable load-time snapshot, so this is the only thing that
// can move the key after init. Turning it on also makes the panel window visible,
// because a debug window switched on and still hidden behind F9 reads as a broken switch.
void SetDebugPanel(bool on);
bool DebugPanelOn();

// The player bar's runtime switch -- the console's `bar on|off|toggle` and the F9 key. `off` stays
// off until something turns it back on, which is what makes it a dismissal: the bar is an overlay
// over a live capture, and the mouse-motion unhide must not undo an owner who asked for it gone.
// `BarShown` answers for right now and `BarShowReason` says why in one word.
void SetBarShown(bool on);
void ToggleBar();
bool BarShown();
const char* BarShowReason();

bool SeekUs(int64_t tUs);
// False when the transport holds no capture, and false when the loaded capture's duration is not
// known yet: a fraction of an unknown span is not 0, and the command that means the start of the
// file is spelled `seek 0`.
bool SeekFraction(double zeroToOne);

// The interval the playhead advances by on THIS machine, in microseconds: the frame period the
// panel measures, which is the only cadence this process measures rather than assumes. False means
// no frame has been measured yet -- a session that has never been in a state where the engine's
// delta is a real frame -- and false must NOT be turned into a number by the caller: a landing
// classified against an invented interval is a verdict about a machine that does not exist. This
// is what control/seekgoto.h's compiled-in 33334 us -- one 30 Hz frame, asked of nobody -- became
bool TickIntervalUs(uint64_t& outUs);

// How many times SetRate / SeekUs / SeekFraction / SeekRoundNumber have refused because the
// transport holds no capture. A refusal that is not counted is a control that silently does
// nothing, and this is the number that says otherwise.
uint32_t NotLoadedRefusals();

// The round table of the loaded capture, as the panel labels it. Count is 0 when no capture is
// loaded OR when its manifest could not be read -- RoundsKnown separates those, because "no rounds"
// and "we could not tell" are different answers to `round`.
bool RoundsKnown();
size_t RoundCount();
uint16_t RoundNumberAt(size_t i);
int64_t RoundTimeUs(size_t i);
// The marker's kind (capture::MarkKind), which is what decides its family and therefore its colour
// on the bar. RoundNumberAt gives the label; two markers can share a label and not a kind.
uint16_t RoundKindAt(size_t i);
// Bookmark `i` on the playhead's axis, for the bar's green ticks. 0 past BookmarkCount().
int64_t BookmarkTimeUs(size_t i);
// Seeks to the round LABELLED `roundNumber`. False when no such round is in the table.
bool SeekRoundNumber(int64_t roundNumber);

// ---- the command layer's door, pushed in by W2-G (BACKLOG-1) -----------------------------------
//
// ★ THE DIRECTION IS control -> ui, for the reason SetBookmarks above gives: src/control already
// calls this header for `load`, `mark` and `-replayviewer`, so including it from src/ui would make
// the dependency a cycle. W2-G registers one function from its own module init and the UI issues
// command TEXT through it.
//
// It exists for the recorder surface's Record/Stop buttons. They issue the console's own
// `record start` / `record stop` rather than calling the recorder, because the session gate, the
// "already recording" refusal and the status line the owner is asked to paste all live in the
// command layer -- and a second caller of the recorder would be a second copy of all three.
//
// The sink runs on the TICK, like every other replay::ui call, and returns whether the command was
// accepted for execution -- not whether it succeeded, which a queued command cannot know yet.
using CommandSinkFn = bool (*)(const wchar_t* line);
void SetCommandSink(CommandSinkFn fn);
// False until a control surface has registered one. A UI control whose command has nowhere to go
// is drawn unavailable rather than pressed into nothing.
bool CommandSinkPresent();
bool RunCommand(const wchar_t* line);

// Multi-line, NUL-terminated, truncated rather than grown. Returns the length written.
size_t WriteDiagnostics(char* out, size_t cap);

// One line, always populated: what the UI is doing, or the reason it is not. This is the string the
// console's `info` prints and the panel's own status row shows, so there is one answer and not two.
const char* StatusLine();

// The count of things the UI knows it cannot do right now -- an unbound widget, an unmapped reset
// primitive, a rate above the engine ceiling. Zero means the panel is telling the whole truth.
uint32_t DegradationCount();

}  // namespace ui
}  // namespace replay
