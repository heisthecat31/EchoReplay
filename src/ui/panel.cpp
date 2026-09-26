#include "ui/panel.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio/voip/voipplay.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/paths.h"
#include "camera/camera_control.h"
#include "engine/session.h"
#include "foundation/gametype.h"
#include "generated/facts_generated.h"
#include "playback/core/entry_gametype.h"
#include "playback/core/playback.h"
#include "playback/core/transport.h"
#include "ui/bar.h"
#include "ui/csregister.h"
#include "ui/gui.h"
#include "ui/legacyimport.h"
#include "ui/levelname.h"
#include "ui/timeline.h"
#include "ui/uiapi.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace replay {
namespace ui {
namespace {

const char* const kReplayModuleName = "ui";

// The window is pinned on its first frame and then left alone, so it stays draggable while the
// strip arithmetic still knows exactly how wide the content is. Both properties matter: a strip
// whose cell count changed as the window resized would move under the pointer between the frame a
// click was aimed and the frame it landed.
constexpr float kPanelW = 720.0f;
constexpr float kPanelH = panel::kPanelHeightPx;
constexpr float kContentW = panel::kContentWidthPx;
constexpr float kCellW = 6.0f;
constexpr float kCellH = 10.0f;
constexpr float kMarkerH = 16.0f;
constexpr float kPlayheadH = 22.0f;
constexpr float kLabelSpacingPx = 26.0f;

// The most rows any snapshot can carry, which is one page: the worker builds the page the pager
// asks for, so the listing is whole however many captures the folder holds.
constexpr size_t kBrowserRows = 24;
// What the window it pinned can show. Prev/Next move the window over the whole listing.
constexpr size_t kRowsPerPage = panel::kBrowserRowsPerPage;
constexpr size_t kRowChars = panel::kRowCharBudget;
constexpr size_t kRowNameChars = panel::kRowNameChars;
constexpr size_t kRowDurationChars = panel::kRowDurationChars;
constexpr size_t kRowCountsChars = panel::kRowCountsChars;
constexpr size_t kRowLevelChars = panel::kRowLevelChars;
constexpr size_t kBrowserChromeToggleChars = panel::kBrowserChromeToggleChars;
constexpr size_t kRowMarkChars = panel::kRowMarkChars;
// What survives at the right-hand end of a name that had to be cut: ".echodemo" plus enough of the
// id in front of it that two captures cut at the same place are still different lines.
constexpr size_t kRowNameTail = 12;
// ---- THE BROWSER SNAPSHOT ---------------------------------------------------------------------
//
// * NO FILE IS OPENED AND NO STRING IS BUILT ON THE THREAD THE GAME RENDERS ON. A worker thread
// lists the folder, reads every header and manifest the page shows, and formats every byte the
// browser draws into one immutable object. The owner tick takes it with a single atomic exchange
// and Draw() reads it -- gui:: calls and nothing else.
//
// Amortising the reads over ticks (8 headers and 4 manifests a tick, which is what stood here)
// moved the cost out of the DRAW and left it on the game's own thread, where a cold CreateFileW
// still costs the frame it lands in. L5-RUN-9 measured drawNanosMax = 1 653 400 ns against a
// 68 500 ns mean: a rare stall, which is exactly the shape a filesystem call has and nothing the
// per-frame path does has. Off the thread entirely, the frame cannot pay for it at all.
struct SnapshotRow {
    std::wstring path;
    // The row's columns, joined and padded to fixed widths by the worker and carrying no widget
    // id: the tick puts the verdict mark in front of it, the reason behind it, cuts the result to
    // the width the window has and only then appends the ##rowN. One string, one widget, whatever
    // the row's verdict turns out to be.
    std::string columns;
    // Kept OFF the columns so that the tick can put the row's REASON in front of them. A row you
    // cannot load says why before it says what is in it; the cut takes the badges first.
    std::string badges;
    std::string faultDetail;   // "" when the row has no fault
    uint64_t levelId = 0;
    uint64_t gametypeId = 0;
    uint16_t playerCount = 0;   // meaningful only when manifestKnown
    bool manifestKnown = false;
    bool legacyUncached = false;   // a .echoreplay/.tape with no cached conversion yet
};

// Immutable once published. The worker builds one, hands it over and never touches it again, so
// there is nothing for a reader to see half of.
struct BrowserSnapshot {
    std::string header;
    std::string overflowLine;   // "" when nothing was cut
    size_t overflowShown = 0;   // rows the listing was cut to, for the sentence that explains it
    std::vector<SnapshotRow> rows;
    size_t firstIndex = 0;      // the index of rows[0] within the SHOWN listing, newest first
    size_t fileCount = 0;       // rows the pager pages over: the shown listing, filtered or not
    size_t totalCount = 0;      // every .echodemo in the folder, whatever the filter hides
    size_t hiddenCount = 0;     // totalCount - fileCount, and always said out loud
    bool   filtered = false;    // whether the playable-only filter was applied to this listing
    bool scanFailed = false;
    uint32_t scanError = 0;
    uint64_t scanNanos = 0;
    uint64_t metadataNanos = 0;
    uint64_t formatNanos = 0;
    uint64_t seq = 0;
};

uint64_t NowNanos() {
    static const int64_t freq = [] {
        LARGE_INTEGER f{};
        return ::QueryPerformanceFrequency(&f) ? f.QuadPart : 0;
    }();
    LARGE_INTEGER t{};
    if (freq <= 0 || !::QueryPerformanceCounter(&t)) return 0;
    const uint64_t ticks = static_cast<uint64_t>(t.QuadPart);
    return (ticks / static_cast<uint64_t>(freq)) * 1000000000ull +
           ((ticks % static_cast<uint64_t>(freq)) * 1000000000ull) / static_cast<uint64_t>(freq);
}

constexpr int64_t kFineWindows[] = {5000000, 10000000, 30000000, 60000000};

// ★ THE TOGGLE KEY. The panel is drawn from a band-3 callback with no input of its own, and this
// build's facade has no keyboard path, so the only switch we can offer is a raw key poll on the
// tick -- and UI-3 moved that poll to bar.cpp, which now owns every key this plugin reads.
//
// F8 is the GAME's key -- it hides nameplates -- so this plugin binds no key of its own
// here at all. The cursor claim is reached through ShowPanel, and the keys that reach it
// (F9 for the whole replay UI, F10 for the cursor alone) live in bar.cpp, which is the one
// place that reads a keyboard.
const char* const kHotkeyName = "F10";

// How long the panel waits for a requested session entry before it says on screen that the world
// is still not the capture's. Accumulated from frameDeltaUs -- one frame's worth, handed to every
// module -- and NEVER differenced from an absolute sim clock, which reads 0 until the engine's own
// clock resolves and would hand the whole accumulated run over as elapsed on the tick it does.
constexpr uint64_t kEntryWaitLimitUs = 30000000;

struct State {
    ReplayServices* services = nullptr;
    DemoIndex* index = nullptr;

    // `enabled` is the ui.panel feature switch; `visible` is whether the window is on screen right
    // now. Collapsing them is what put a 720x430 window over ordinary gameplay from the moment the
    // game had a display object.
    bool enabled = true;
    bool visible = false;
    bool browserOpen = false;
    bool placed = false;
    bool cursorHeld = false;   // true only while WE have the engine's cursor enabled

    // Sampled on the tick, rendered on the callback. Never read from the engine inside Draw().
    TransportState transportState = TransportState::Pause;
    float rate = 1.0f;
    int64_t playheadUs = 0;
    int64_t durationUs = 0;
    bool haveTransport = false;
    char modeLabel[128] = {};

    uint32_t maxElapsedUs = facts::CLOCK_MAXELAPSED_US;
    bool maxElapsedLive = false;
    double framePeriodEma = 0.0;
    float ceiling = 0.0f;
    // Why the ceiling is what it is, so an unknown one can name its own cause instead of leaving
    // four launch rows to guess. `offered` counts the ticks whose state could have measured a
    // period; `zeroDelta` counts how many of those carried no engine sim-time advance at all.
    uint64_t periodOffered = 0;
    uint64_t periodSamples = 0;
    uint64_t periodZeroDelta = 0;

    Marker markers[64] = {};
    size_t markerCount = 0;
    bool markersKnown = false;

    wchar_t replayDir[kPathMax] = {};
    // The same directory as a narrow string, converted ONCE. It is drawn every frame and the wide
    // conversion is not free; nothing but Init and a rescan can change it.
    char replayDirNarrow[kPathMax] = {};
    // The browser's header line, rebuilt only when one of the two numbers in it moves. Formatting
    // it per frame put a vsnprintf on the draw path for a string that changes on a rescan.
    char browserHeader[128] = {};
    size_t browserHeaderCount = static_cast<size_t>(-1);
    uint64_t browserHeaderMicros = static_cast<uint64_t>(-1);
    wchar_t loadedPath[kPathMax] = {};   // empty means nothing loaded; LoadedPath() maps that to null
    char loadedName[128] = {};
    // Captured at load, so the fidelity line is about the capture that is PLAYING rather than
    // whatever row the browser happens to be showing.
    uint32_t loadedFlags = 0;
    bool loadedFlagsKnown = false;
    BuildMatch loadedBuildMatch = BuildMatch::NotChecked;
    char status[512] = {};
    uint32_t degradations = 0;
    uint32_t notLoadedRefusals = 0;   // drive commands refused because no capture is open

    // The owner's bookmarks, pushed across the seam by W2-G. 64 because the strip cannot usefully
    // label more than that at this width, and the count is reported so a cut is never silent.
    static const size_t kMaxBookmarks = 64;
    NotesState notesState = NotesState::NotSupplied;
    Marker bookmarks[kMaxBookmarks] = {};
    char bookmarkLabels[kMaxBookmarks][48] = {};
    size_t bookmarkCount = 0;    // drawn
    size_t bookmarkTotal = 0;    // supplied; may exceed bookmarkCount
    int64_t resumeUs = 0;

    // The DESIGN 8.4.3 decision about the capture that is loaded, and the wait it may have started.
    EntryVerdict entryVerdict = EntryVerdict::NotChecked;
    char entryReason[512] = {};
    bool entryPending = false;
    // The pending entry is one the session has not taken YET and must be re-offered, rather than
    // one it took and is performing. Two different waits; one flag cannot carry both.
    bool entryDeferred = false;
    bool entryTimedOut = false;
    uint64_t entryWaitUs = 0;
    uint64_t entryLevelId = 0;
    uint64_t entryGametypeId = 0;
    // session.gametype_override and the gametype its fallback name hashes to.
    bool gametypeResolve = true;
    uint64_t gametypeFallback = 0;

    int64_t fineWindowUs = kFineWindows[1];
    float exactSeconds = 0.0f;
    bool asSeen = false;
    int32_t camSelector = 1;   // 1-based player, or a level camera index

    // ★ Draw() does no file I/O AND NO ALLOCATION. It reads `live` -- which the tick swapped in
    // before the frame opened -- and calls the facade. Everything a row says was formatted by the
    // worker; everything a row's SESSION verdict says was decided on the tick, where every engine
    // read in this packet happens. A click is recorded and serviced on the next tick for the same
    // reason: LoadPath opens files, and a click must not turn one frame into a disk seek.
    // Set by the Rescan button inside the draw and turned into a worker job by the next tick,
    // because the request itself takes a lock and the render path takes none.
    bool scanRequested = false;
    // Default ON. Of the owner's captures most are solo and lobby sessions that load to an empty
    // rail; the toggle is one click and the hidden count is always on screen, so nothing is lost
    // silently.
    bool playableOnly = true;
    const BrowserSnapshot* live = nullptr;
    uint64_t liveSeq = 0;
    // The per-row session verdict, decided on the tick and re-formatted only when it changes.
    // `rowWhy` is the reason string DecideEntry returned; comparing the POINTER is what stops 24
    // snprintf calls happening every tick for a page that says the same thing it said last tick.
    // ★ THE WHOLE BROWSER FRAME, BUILT ON THE TICK, EACH LINE CARRYING WHAT IT WILL COST. One
    // widget per row whatever the row's verdict is: the mark, the columns and the reason are one
    // string, so a page of refused rows hands ImGui exactly what a page of loadable ones does.
    // The glyph counts travel with the strings so that the draw can publish the stream it issued
    // without running a strlen inside the bracket it is being timed by.
    char rowLine[kBrowserRows][kRowChars + 16] = {};
    uint32_t rowGlyphs[kBrowserRows] = {};
    // The listing/pager line and the status line, as ONE Text: two widgets' worth of chrome for
    // one ItemAdd and one ID hash.
    char headerBlock[2 * (kRowChars + 2)] = {};
    char headerFirst[kRowChars + 2] = {};   // the same first line, alone, for the fit test
    uint32_t headerGlyphs = 0;
    // A greyed row that was clicked. Its reason is longer than the line it fits on, so the click
    // is where the owner asks for the rest of it; the tick puts it on the status line.
    int refusedClick = -1;
    // The page the browser is showing, and the sentence that says so. The sentence is built on the
    // tick like every other string this panel draws -- Draw formats nothing.
    size_t browserPage = 0;
    size_t rowsFromIndex = static_cast<size_t>(-1);   // which listing index g.rowLine[0] is
    size_t pageLabelPage = static_cast<size_t>(-1);
    size_t pageLabelRows = static_cast<size_t>(-1);
    size_t pageLabelHidden = static_cast<size_t>(-1);
    char pageLabel[96] = {};
    const char* rowWhy[kBrowserRows] = {};
    uint8_t rowVerdict[kBrowserRows] = {};
    bool rowRefused[kBrowserRows] = {};
    bool loadRequested = false;
    wchar_t loadRequestPath[kPathMax] = {};

    // FORMATS-P6: the legacy replay being converted, as the status line and the Cancel button see
    // it. Set on the tick; Draw reads importRunning and records a click in importCancelRequested.
    bool importRunning = false;
    bool importCancelRequested = false;
    // Cleared by Unload or by loading something else: the conversion then finishes or stops, and
    // nothing is loaded over what the owner chose instead.
    bool importLoadWhenDone = false;
    char importName[128] = {};
    uint32_t importPercentShown = 0;
    // The name the next committed load shows instead of the file's own: a converted replay is
    // loaded from `<stem>.<key16>.echodemo`, and the owner picked `<stem>.echoreplay`.
    char loadNameOverride[128] = {};

    CellClass cells[kMaxCells] = {};
};

State g;

// ★ ONE INDEX, TWO THREADS, ONE LOCK. The worker owns the index for the length of a build; the
// owner thread takes the same lock for the three things it does with the index that are user
// actions rather than frames -- a load, a synchronous listing, the diagnostic dump. Recursive
// because LoadMostRecent lists and then loads, and both halves need it.
// Never destroyed, for the reason g_worker below is not: process exit can kill the worker while it
// holds this, and a mutex destroyed while held is the STL's abort().
alignas(std::recursive_mutex) unsigned char g_indexMutexStorage[sizeof(std::recursive_mutex)];
std::recursive_mutex& g_indexMutex = *new (g_indexMutexStorage) std::recursive_mutex;
using IndexLock = std::lock_guard<std::recursive_mutex>;

struct BrowserWorker {
    // A raw handle, like every other worker in this tree. The C++ thread object this used to be was
    // still joinable when the CRT ran this module's static destructors: terminate() -> abort() ->
    // __fastfail at every process exit.
    HANDLE thread = nullptr;
    std::mutex m;
    std::condition_variable cv;
    uint64_t requested = 0;   // guarded by m
    uint64_t started = 0;     // guarded by m
    bool stop = false;        // guarded by m
    std::atomic<uint64_t> published{0};
    // The handoff. The worker exchanges a fresh snapshot in; the owner thread exchanges it out.
    // Whoever gets a non-null pointer out of the exchange owns it exclusively, so neither side can
    // free an object the other is reading and neither can see a half-built one.
    std::atomic<BrowserSnapshot*> pending{nullptr};
    // Which page the next snapshot is built for. The worker reads every file's name and time, and
    // opens the header and manifest of the PAGE only -- so 152 captures cost one directory
    // enumeration and seventeen file opens, and the newest is always the first row of page 0.
    std::atomic<size_t> page{0};
    // Whether the next snapshot hides the rows whose manifest records no players. Read once per
    // build, so a toggle mid-scan changes the snapshot after it, never the one being built.
    std::atomic<bool> playableOnly{true};
};

// Never destroyed. Process exit kills the worker where it is parked in cv.wait, and a destructor
// running after that would tear down a mutex and a condition variable with a dead waiter on them.
alignas(BrowserWorker) unsigned char g_workerStorage[sizeof(BrowserWorker)];
BrowserWorker& g_worker = *new (g_workerStorage) BrowserWorker;

// Never destroyed, for the same reason: a conversion still running at process exit owns this.
alignas(legacy::ConversionJob) unsigned char g_importStorage[sizeof(legacy::ConversionJob)];
legacy::ConversionJob& g_import = *new (g_importStorage) legacy::ConversionJob;
legacy::ConvertFn g_convertOverride = nullptr;

legacy::ConvertFn Converter() {
    return g_convertOverride != nullptr ? g_convertOverride : legacy::LinkedConverter();
}

void SetStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.status, sizeof g.status, fmt, ap);
    va_end(ap);
}

// The narrow copy the browser draws, refreshed wherever the directory is decided.
void NoteReplayDir() {
    if (::WideCharToMultiByte(CP_UTF8, 0, g.replayDir, -1, g.replayDirNarrow,
                              static_cast<int>(sizeof g.replayDirNarrow), nullptr,
                              nullptr) == 0) {
        snprintf(g.replayDirNarrow, sizeof g.replayDirNarrow, "%s",
                 "(the capture directory is not representable as text)");
    }
}

// * THE PATH AND THE REASON, NOT ONE OR THE OTHER. "the replay folder could not be read" sends a
// user looking for a permissions problem; without the OS error, a missing folder, a denied one and
// a disconnected drive are the same sentence. DemoIndex reports the code FindFirstFileW gave it.
void ReportScanFailure(uint32_t osError) {
    SetStatus("the replay folder could not be read: %ls (Windows error %lu)", g.replayDir,
              static_cast<unsigned long>(osError));
}

// The two index reads the diagnostic dump makes, taken under the lock: the worker may be replacing
// the row vector, and a count read out of a vector that is growing is not a count.
size_t IndexCount() {
    IndexLock lock(g_indexMutex);
    return g.index != nullptr ? g.index->Count() : 0u;
}

uint64_t IndexScanMicros() {
    IndexLock lock(g_indexMutex);
    return g.index != nullptr ? g.index->LastScanMicros() : 0u;
}

IReplayTransport* Transport() {
    return g.services != nullptr ? g.services->transport : nullptr;
}

// ★ EVERY COMMAND THAT DRIVES THE TRANSPORT ASKS THIS FIRST, AND A "NO" NEVER REACHES THE
// TRANSPORT. The object is published from module init onward, so a null check answered a different
// question: SetRate against a fileless transport ran Play(rate) and returned true, and the console
// printed a rate that had been applied to nothing. NotLoaded is the transport's own word for "there
// is no file" -- it is asked, the refusal is counted, and it is put on the status line so the user
// is told why rather than left with a control that silently does nothing.
bool RefuseWithNothingLoaded(IReplayTransport* t, const char** why) {
    if (t == nullptr || t->State() != TransportState::NotLoaded) {
        return false;
    }
    ++g.notLoadedRefusals;
    SetStatus("nothing loaded: no capture is open, so there is nothing to drive. Pick one in the "
              "browser, or `load <file>` in the console.");
    if (why != nullptr) {
        *why = g.status;
    }
    return true;
}

ISessionEntry* Session() {
    return g.services != nullptr ? g.services->session : nullptr;
}

void SetEntryReason(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.entryReason, sizeof g.entryReason, fmt, ap);
    va_end(ap);
}

// ★ DESIGN 8.4.3's table, and the ONLY place in this packet that decides it.
//
// It has no side effect, which is what lets the browser call it once per visible row per frame to
// preview the same answer LoadPath will act on. Performing the entry is LoadPath's half, because
// RequestEntry tears the session down and rebuilds it on the tick and a preview must never do that.
//
// A null manifest is NotChecked rather than reachable: a capture whose header we could not read has
// no level id, and treating "we do not know" as "it is fine" is the whole class of defect this
// packet spent its BuildMatch and ClassifyAvailability work making unrepresentable elsewhere.
EntryVerdict DecideEntryIds(bool haveManifest, uint64_t levelId, uint64_t gametypeId,
                           const char** why) {
    static const char kNoSession[] =
        "no ISessionEntry has registered, so whether this session can reach the capture's level is "
        "UNKNOWN - not confirmed";
    static const char kNoManifest[] =
        "this capture's manifest could not be read, so its level and gametype are UNKNOWN and the "
        "running session cannot be checked against them";
    static const char kMatches[] =
        "the running session is already in this capture's level and gametype";
    static const char kCanEnter[] =
        "this capture was recorded in a level this session is not in; the entry strategy can change "
        "level, so entry is requested before the capture is opened";

    *why = kNoSession;
    ISessionEntry* s = Session();
    if (s == nullptr) return EntryVerdict::NotChecked;

    if (!haveManifest) {
        *why = kNoManifest;
        return EntryVerdict::NotChecked;
    }
    if (s->Matches(levelId, gametypeId)) {
        *why = kMatches;
        return EntryVerdict::Matches;
    }
    if (s->CanChangeLevelMidSession()) {
        *why = kCanEnter;
        return EntryVerdict::MustEnter;
    }
    const char* reason = s->FailureReason();
    *why = reason != nullptr && reason[0] != '\0'
               ? reason
               : "the session-entry strategy cannot change level after startup";
    return EntryVerdict::Refused;
}

// The gametype a capture is entered with. Older captures recorded 32 bits of it, or the all-ones
// the engine field held at open, and the engine recognizes neither. `path` is read for the
// capture's own owner-state event only when the recorded value settles nothing; the browser passes
// null, because a preview must not decode every file in the folder.
gametype::Resolved ResolveEntryGametype(const wchar_t* path, uint64_t recorded) {
    if (!g.gametypeResolve) return {recorded, gametype::Rule::Unresolved};
    const uint64_t ownerState = path != nullptr && gametype::NeedsOwnerState(recorded)
                                    ? playback::OwnerStateGametypeOfFile(path)
                                    : 0;
    return gametype::Resolve(recorded, ownerState, g.gametypeFallback);
}

// Whether the DEBUG panel is actually on screen. Its own draw is keyed on this.
bool PanelIsDrawing() {
    return g.enabled && g.visible && gui::Ready() && gui::WindowGateOpen();
}

// ★ THE CURSOR IS THE BAR'S, NOT THE DEMOTED PANEL'S (UI-3). It used to be keyed on
// PanelIsDrawing(), so `ui.panel=false` -- a key about a debug window nobody sees -- silently took
// the mouse away from the player bar, and LAUNCH-BARFIX watched two F8 presses leave the cursor
// unclaimed and `hovered=0` with the pointer sitting on the track. `visible` is the replay UI's
// "I want the mouse" flag now; the debug window keeps its own gate above.
bool CursorWanted() { return g.visible && gui::Ready() && gui::WindowGateOpen(); }

// Bring the cursor (and, if ui.panel allows it, the debug window) up.
void MakeVisible() { g.visible = true; }

// The engine's own ceiling on fast-forward, read live rather than taken from the constant, so a
// build or a launch flag that raised it is reflected instead of contradicted. Falls back to the
// V-PRIMARY constant and records which of the two we used.
void SampleMaxElapsed() {
    const void* slot = buildguard::MappedAddress(facts::CLOCK_GLOBAL_PTR);
    if (slot != nullptr) {
        const uint8_t* clk = *reinterpret_cast<const uint8_t* const*>(slot);
        if (clk != nullptr) {
            g.maxElapsedUs =
                *reinterpret_cast<const volatile uint32_t*>(clk + facts::CLOCK_MAXELAPSED_OFF);
            g.maxElapsedLive = true;
        }
    }
    if (!g.maxElapsedLive) g.maxElapsedUs = facts::CLOCK_MAXELAPSED_US;
}

// ★ AN UNKNOWN CEILING MUST NAME ITS OWN CAUSE. "0" alone sent four launch rows to could-not-ask
// with nothing to act on. The three answers are genuinely different repairs: never in a measuring
// state, in one with a sim clock that is not advancing, or measuring but not settled yet.
const char* CeilingWhy() {
    static char why[224];
    if (g.ceiling > 0.0f) {
        snprintf(why, sizeof why, "measured over %llu sample(s)",
                 static_cast<unsigned long long>(g.periodSamples));
    } else if (g.periodOffered == 0) {
        snprintf(why, sizeof why,
                 "UNKNOWN - not one tick has been in a state that measures a frame period: no "
                 "capture has been open with the engine's own clock running, and none has played. "
                 "It is NOT 4x");
    } else if (g.periodZeroDelta == g.periodOffered) {
        snprintf(why, sizeof why,
                 "UNKNOWN - %llu tick(s) could have measured one and every single frame delta was "
                 "0: the engine's sim clock is not advancing for us (CLOCK.GLOBAL_PTR unresolved "
                 "when the tick claimed its slot), and it is NOT 4x",
                 static_cast<unsigned long long>(g.periodOffered));
    } else {
        snprintf(why, sizeof why,
                 "UNKNOWN - %llu tick(s) offered a period, %llu of them a zero frame delta, and "
                 "none was inside the 0..%u us the clamp allows; it is NOT 4x",
                 static_cast<unsigned long long>(g.periodOffered),
                 static_cast<unsigned long long>(g.periodZeroDelta), g.maxElapsedUs);
    }
    return why;
}

// WHY THE PLAYHEAD IS NOT MOVING, which "paused" does not answer at either end of the file: the
// transport clamps there and pauses itself, and a viewer who pressed play and saw "paused" has no
// way to tell that from a transport that refused him.
const char* TransportStateName(TransportState s);

const char* TransportStateWord(TransportState s, EndState end) {
    if (s == TransportState::Pause) {
        if (end == EndState::AtEnd) return "end of recording";
        if (end == EndState::AtStart) return "start of recording";
    }
    return TransportStateName(s);
}

const char* TransportStateName(TransportState s) {
    switch (s) {
        case TransportState::Pause: return "paused";
        case TransportState::Play: return "playing";
        case TransportState::Scrub: return "scrubbing";
        case TransportState::Seek: return "seeking";
        case TransportState::Step: return "stepping";
        case TransportState::NotLoaded: return "no replay loaded";
    }
    return "?";
}

void SeekTo(int64_t tUs) {
    IReplayTransport* t = Transport();
    if (t == nullptr) return;
    if (tUs < 0) tUs = 0;
    if (g.durationUs > 0 && tUs > g.durationUs) tUs = g.durationUs;
    // ★ The scrubber has no path of its own. It assigns the playhead through B.8's Seek, which
    // raises the discontinuity flag the frame loop consumes; the single ApplyStateAt then serves
    // this exactly as it serves linear play. There is no second way to apply state here and there
    // must never be one.
    t->Seek(tUs);
    g.playheadUs = tUs;
}

// A strip of contiguous buttons across kContentW. Returns true if a cell was clicked and writes the
// time it seeks to. The cell HEIGHT carries the class -- see timeline.h for why not colour.
// ★ THE CELL IDS ARE CONSTANTS, so the draw does not rebuild them. Two strips of
// CellCount(kContentW, kCellW) cells put ~230 snprintf calls a frame inside the bracket
// drawNanosMax is measured over, to produce the same 230 strings every time. They are the widget
// IDs; nothing about them depends on the capture, the span or the playhead.
struct CellIdTable {
    char id[2][kMaxCells][12];
    CellIdTable() {
        for (size_t i = 0; i < kMaxCells; ++i) {
            snprintf(id[0][i], sizeof id[0][i], "##c%zu", i);
            snprintf(id[1][i], sizeof id[1][i], "##f%zu", i);
        }
    }
};

// ---- THE TRANSPORT PAGE'S STREAM, COUNTED WHERE IT IS ISSUED (W2-F PANEL4) -------------------
// DRAW4 counted the BROWSER page; the page the owner actually plays on had no measured budget at
// all, so "the transport draw fits" was an assertion nobody could fail. Counted() wraps the label
// in place at every widget on this page, so a widget added without a count is a widget that does
// not compile past review rather than one that quietly leaves the number behind.
//
// Glyphs are counted up to "##", which is where ImGui stops measuring and starts hashing. The
// browser hands its lengths in because its strings are built on the worker tick; this page builds
// every string in the frame it draws, so the length is taken here from the string itself.
struct GuiStreamCount {
    uint32_t items;
    uint32_t glyphs;
};
GuiStreamCount g_stream = {0, 0};

const char* Counted(const char* label) {
    ++g_stream.items;
    uint32_t n = 0;
    while (label[n] != '\0' && !(label[n] == '#' && label[n + 1] == '#')) ++n;
    g_stream.glyphs += n;
    return label;
}

void CText(const char* s) { gui::Text(Counted(s)); }

// A separator is a widget the frame pays for and measures nothing.
void CSeparator() {
    ++g_stream.items;
    gui::Separator();
}

bool DrawStrip(const char* idPrefix, const Span& span, int64_t* seekOut) {
    static const CellIdTable kCellIds;
    const char (*ids)[12] = kCellIds.id[idPrefix[0] == 'f' ? 1 : 0];
    const uint32_t cells = CellCount(kContentW, kCellW);
    ClassifyCells(span, cells, g.markers, g.markerCount, g.playheadUs, g.haveTransport, g.cells);

    bool clicked = false;
    for (uint32_t i = 0; i < cells; ++i) {
        const char* const label = ids[i];
        gui::Vec2 size;
        size.x = kCellW;
        switch (g.cells[i]) {
            case CellClass::Playhead:
            case CellClass::MarkerPlayhead: size.y = kPlayheadH; break;
            case CellClass::Marker: size.y = kMarkerH; break;
            default: size.y = kCellH; break;
        }
        if (gui::Button(Counted(label), size)) {
            *seekOut = SeekTargetForCell(span, cells, i);
            clicked = true;
        }
        if (i + 1 < cells) gui::SameLine(0.0f, 0.0f);
    }
    return clicked;
}

void DrawMarkerLabels(const Span& span) {
    if (g.markerCount == 0) return;
    MarkerLabel labels[32];
    size_t dropped = 0;
    const size_t n = LayoutMarkerLabels(span, kContentW, kLabelSpacingPx, g.markers, g.markerCount,
                                        labels, 32, &dropped);
    if (n == 0 && dropped == 0) return;

    gui::Text(Counted(" "));
    for (size_t i = 0; i < n; ++i) {
        char buf[16];
        FormatRoundLabel(labels[i].kind, labels[i].index, 1, buf, sizeof buf);
        gui::SameLine(labels[i].x, 0.0f);
        gui::Text(Counted(buf));
    }
    if (dropped > 0) {
        char buf[96];
        // Never silently show fewer rounds than the file has: a timeline missing a marker is a
        // timeline that says the round is not there.
        snprintf(buf, sizeof buf, "   (+%zu more round marks than fit at this zoom)", dropped);
        gui::SameLine(0.0f, 0.0f);
        gui::Text(Counted(buf));
    }
}

// ★ THE PERSISTENT FIDELITY LINE, WHICH DESIGN 8.5.3 ASKS FOR BY NAME: "flags.IMPORTED also drives
// a persistent on-screen label during playback, so a legacy file's differences can never read as
// bugs." A browser badge is not that -- it is gone the moment the capture opens, which is precisely
// when the differences start showing. So this draws every frame, beside the transport.
//
// It covers the same ground for the other truths a capture carries: a recording made against a
// different game build, a truncated capture, and the streams that are simply not in the file. Those
// are all readable from the header this packet already parsed, and none of them were on screen
// during playback before.
//
// ★ AND IT DISTINGUISHES "NOTHING IS DEGRADED" FROM "NOBODY ASKED". The reset-battery's unmapped
// primitives are the fidelity statement that matters most and there is no accessor to the live
// battery yet, so the honest answer today is UNKNOWN -- not the empty list that would render as a
// clean bill of health. ClassifyAvailability makes that distinction unrepresentable rather than
// remembered; when W2-B exposes the battery, `consulted` becomes true and this line starts naming
// ids without another word of it changing.
void DrawFidelityLine() {
    char line[512];

    if (g.loadedFlagsKnown) {
        if ((g.loadedFlags & kFlagImported) != 0) {
            gui::Text(Counted("IMPORTED - reduced fidelity: this capture was converted from a legacy "
                      "format, so differences from a native recording are expected, not bugs"));
        }
        // Three states, three lines. "We checked and it matches" is silence; the other two are
        // not, and they are not each other either.
        if (g.loadedBuildMatch == BuildMatch::Mismatch) {
            gui::Text(Counted("CONTENT MISMATCH: recorded against a different echovr.exe than the one "
                      "running. Offsets and content ids may not mean here what they meant there."));
        } else if (g.loadedBuildMatch == BuildMatch::NotRecorded) {
            gui::Text(Counted("BUILD NOT RECORDED: this capture does not say which echovr.exe it was "
                      "made against, so whether it matches the running game is UNKNOWN - it is not "
                      "a mismatch and not a match."));
        } else if (g.loadedBuildMatch == BuildMatch::NotChecked) {
            gui::Text(Counted("BUILD NOT CHECKED: the expected build fingerprint was unavailable, so "
                      "whether this capture matches the running game is UNKNOWN - not confirmed."));
        }
        if ((g.loadedFlags & kFlagTruncatedByBudget) != 0) {
            gui::Text(Counted("TRUNCATED: the recorder hit its budget, so this capture stops short of the "
                      "match it came from"));
        }
        if ((g.loadedFlags & kFlagOpen) != 0) {
            gui::Text(Counted("NOT CLOSED CLEANLY: the recorder did not finish, so the tail of this "
                      "capture may be missing"));
        }
        char missing[128];
        missing[0] = '\0';
        auto note = [&](const char* what) {
            const size_t used = strlen(missing);
            snprintf(missing + used, sizeof missing - used, "%s%s", used ? ", " : "", what);
        };
        if ((g.loadedFlags & kFlagHasVoip) == 0) note("VOIP");
        if ((g.loadedFlags & kFlagHasAsSeen) == 0) note("as-seen");
        if ((g.loadedFlags & kFlagHasOracle) == 0) note("oracle");
        if (missing[0] != '\0') {
            snprintf(line, sizeof line, "not in this capture: %s", missing);
            gui::Text(Counted(line));
        }
    } else if (g.loadedPath[0] != L'\0') {
        gui::Text(Counted("this capture's header was not read, so its fidelity flags are UNKNOWN - not "
                  "assumed clean"));
    }

    // ★ WHICH WORLD THIS IS PLAYING OVER. A capture whose level the session never entered plays
    // perfectly over the wrong map: the timeline populates, the playhead moves and every control
    // works. Silence here is what makes that indistinguishable from success, so anything other
    // than a confirmed match is a line on screen for as long as it is true.
    if (g.loadedPath[0] != L'\0' && g.entryVerdict != EntryVerdict::Matches) {
        snprintf(line, sizeof line, "WORLD %s: %s", EntryVerdictName(g.entryVerdict), g.entryReason);
        gui::Text(Counted(line));
    }

    // The panel is click-only. If this did not bind we cannot enable the cursor, and a cursor the
    // engine has switched off makes every button here inert while the window looks perfect.
    if (!gui::Have(gui::Entry::SetCursorEnabled)) {
        gui::Text(Counted("GUI.SETCURSORENABLED did not bind: this panel cannot enable the mouse cursor, so "
                  "if the engine's own cursor is off none of these buttons can be clicked"));
    }

    // ★ THE RATE MOVES THE PLAYHEAD; IT DOES NOT YET MOVE THE WORLD. The frame loop computes the
    // transport's step but deliberately does not write the engine clock -- that is IReplayClock,
    // and ReplayServices::clock is still null. So the playhead advances at the rate we asked for
    // and state publishes at it, while the engine's own clock runs free. Saying "2.00x" with
    // nothing else on screen would be a claim about the world that is not true yet, and the first
    // person to notice would reasonably file it as a transport bug.
    if (g.services == nullptr || g.services->clock == nullptr) {
        gui::Text(Counted("rate applies to the PLAYHEAD only: no replay clock has registered, so the "
                  "engine's own clock still runs at real time and animation, effects and audio "
                  "are not yet rate-scaled"));
    }

    // ★ THE RESIDUE BATTERY, ASKED PROPERLY. `consulted` is the witness that somebody COULD ask --
    // it is false when the playback module never initialised, and the counts beside it then mean
    // nothing. Asking with the count alone is the two-state answer that reads a zero as "everything
    // is available", which is why this takes the snapshot first and the ids second.
    const playback::ResidueStatus residue = playback::ResidueSnapshot();

    if (residue.consulted && residue.registered == 0) {
        // ★ A THIRD THING THE COUNT ALONE CANNOT SAY. Consulted, zero unavailable -- and zero
        // registered. Nothing is unavailable because there is NOTHING, which is a battery that
        // failed to populate rather than a replay in good health, and ClassifyAvailability would
        // rightly call it AllAvailable over an empty set. Vacuous truth, named as such.
        gui::Text(Counted("reset primitives: the battery is reachable but EMPTY - zero primitives are "
                  "registered, so 'none unavailable' is vacuous rather than reassuring"));
        return;
    }

    switch (ClassifyAvailability(residue.consulted, residue.unavailable)) {
        case Availability3::Unknown:
            gui::Text(Counted("reset primitives: NOT REPORTED - the playback module has not initialised, so "
                      "this is 'we could not ask' and not 'nothing is missing'"));
            break;
        case Availability3::AllAvailable:
            snprintf(line, sizeof line,
                     "reset primitives: all %zu registered primitive(s) are available",
                     residue.registered);
            gui::Text(Counted(line));
            break;
        case Availability3::SomeUnavailable: {
            const char* ids[16] = {};
            const size_t cap = sizeof ids / sizeof ids[0];
            // ★ RETURNS HOW MANY EXIST, WHICH MAY EXCEED cap. Rendering `returned` entries would
            // walk off the end of the array; rendering cap and saying nothing would silently drop
            // the rest. So: render min(returned, cap), and print the total either way.
            const size_t total = playback::UnavailableResidueIds(ids, cap);
            const size_t shown = total < cap ? total : cap;
            snprintf(line, sizeof line,
                     "reset primitives: %zu of %zu registered are UNAVAILABLE in this build",
                     residue.unavailable, residue.registered);
            gui::Text(Counted(line));
            for (size_t i = 0; i < shown; ++i) {
                const char* why = playback::ResidueDiagnostic(ids[i]);
                // Thirteen ids without reasons tell a user something is wrong and not what.
                snprintf(line, sizeof line, "    %s - %s", ids[i],
                         why != nullptr && why[0] != '\0' ? why : "no reason recorded");
                gui::Text(Counted(line));
            }
            if (total > shown) {
                snprintf(line, sizeof line, "    ... and %zu more not listed here", total - shown);
                gui::Text(Counted(line));
            }
            break;
        }
    }
}

// The round markers earn their keep here rather than on the ruler: a labelled button per round
// that jumps to it.
//
// Every button goes through SeekRoundNumber -- the same seam W2-G's `round <n>` uses -- so the
// panel and the console cannot disagree, and a disagreement between the TRANSPORT's round table and
// the manifest's is reported once, in one place, rather than twice or not at all.
//
// (This used to seek by recorded time, because B.8's original SeekRound(int) did not say whether its
// argument was a position in Manifest::rounds or the round NUMBER in RoundMarker::index. The
// amendment put that distinction in the type system -- SeekRound(RoundNumber), returning bool -- so
// the ambiguity is gone and the transport's own answer is preferred.)
void DrawRoundJumps() {
    if (g.markerCount == 0) return;
    CText("rounds:");
    RoundEntry entries[panel::kMaxRoundButtons];
    size_t clamped = 0;
    const size_t n =
        BuildRoundStrip(g.markers, g.markerCount, entries, panel::kMaxRoundButtons, &clamped);
    for (size_t i = 0; i < n; ++i) {
        char b[32];
        // The id keys on the SOURCE marker, not on the label: two runs can legitimately show the
        // same text, and two identical ids are one widget as far as ImGui's hash is concerned.
        snprintf(b, sizeof b, "%s##rj%zu", entries[i].label, entries[i].sourceIndex);
        gui::SameLine(0.0f, 4.0f);
        // Through the same seam the console's `round <n>` uses, so a disagreement between the
        // transport's round table and the manifest's is reported once, in one place.
        if (gui::Button(Counted(b), gui::Vec2{44.0f, 0.0f}))
            SeekRoundNumber(g.markers[entries[i].sourceIndex].index);
    }
    if (clamped > 0) {
        char b[24];
        snprintf(b, sizeof b, "...+%zu", clamped);
        gui::SameLine(0.0f, 4.0f);
        // Text, not a button: a button that seeks nowhere is a control that lies about what it does.
        CText(b);
    }
}

// ★ THE OWNER'S BOOKMARKS, WHICH UNTIL NOW NOTHING IN THE PRODUCT COULD SHOW HIM BACK.
//
// `mark` wrote a sidecar that was write-only: the console said "bookmarked 123456 us" and there was
// no path anywhere in the product to that instant. W2-G built the reader and the `marks` / `goto`
// commands; this is the half that puts them on the timeline he made them on.
//
// ★ BUILT FROM Button AND SameLine, because that is what this facade has. There is no Selectable,
// no scrolling child and no mouse-position query, which is why the scrub bar is a strip of buttons
// in the first place. The positions come from LayoutMarkerLabels -- the same arithmetic the round
// marks use, already swept by tests/ui -- so bookmark labels sit at their real x along the strip
// and drop rather than overlap, with the dropped count stated.
void DrawBookmarks(const Span& span) {
    char line[512];

    // ★ FOUR STATES, AND THE FIRST IS THE ONE THAT MATTERS. "Nobody has told us" must never render
    // as "you have no bookmarks" -- that would tell the owner his marks are gone.
    if (g.notesState == NotesState::NotSupplied) {
        gui::Text(Counted("bookmarks: NOT SUPPLIED - no control surface has handed this panel a sidecar, so "
                  "whether this capture has bookmarks is UNKNOWN, not none"));
        return;
    }
    if (g.notesState == NotesState::Damaged) {
        gui::Text(Counted("bookmarks: this capture's sidecar exists and does not parse. It has been left "
                  "alone rather than overwritten, so nothing has been lost - but nothing can be "
                  "read from it either until it is repaired or removed."));
        return;
    }

    if (g.resumeUs > 0) {
        char at[32];
        FormatTime(g.resumeUs, at, sizeof at);
        snprintf(line, sizeof line, "Resume %s##resume", at);
        // Offered, never applied behind his back: moving the playhead on load because a file said
        // so is the transport doing something nobody asked for on this frame.
        if (gui::Button(Counted(line), gui::Vec2{140.0f, 0.0f})) SeekTo(g.resumeUs);
        gui::SameLine(0.0f, 8.0f);
        gui::Text(Counted("(where you left off)"));
    }

    if (g.bookmarkCount == 0) {
        gui::Text(Counted(g.notesState == NotesState::Absent
                      ? "bookmarks: none yet - `mark` records one at the playhead"
                      : "bookmarks: this capture's sidecar holds none"));
        return;
    }

    // Labels along the strip, at their real positions.
    MarkerLabel labels[32];
    size_t dropped = 0;
    const size_t n = LayoutMarkerLabels(span, kContentW, kLabelSpacingPx, g.bookmarks,
                                        g.bookmarkCount, labels, 32, &dropped);
    if (n > 0) {
        gui::Text(Counted(" "));
        for (size_t i = 0; i < n; ++i) {
            char buf[16];
            snprintf(buf, sizeof buf, "B%u", static_cast<unsigned>(labels[i].index));
            gui::SameLine(labels[i].x, 0.0f);
            gui::Text(Counted(buf));
        }
    }

    snprintf(line, sizeof line, "bookmarks (%zu):", g.bookmarkTotal);
    gui::Text(Counted(line));
    for (size_t i = 0; i < g.bookmarkCount; ++i) {
        char at[32];
        FormatTime(g.bookmarks[i].tUs, at, sizeof at);
        char b[96];
        // The label is the owner's own words; the time is what makes two of them distinguishable.
        if (g.bookmarkLabels[i][0] != '\0') {
            snprintf(b, sizeof b, "B%u %s  %s##bm%zu", static_cast<unsigned>(g.bookmarks[i].index),
                     at, g.bookmarkLabels[i], i);
        } else {
            snprintf(b, sizeof b, "B%u %s##bm%zu", static_cast<unsigned>(g.bookmarks[i].index), at,
                     i);
        }
        gui::SameLine(0.0f, 4.0f);
        if (gui::Button(Counted(b), gui::Vec2{0.0f, 0.0f})) SeekTo(g.bookmarks[i].tUs);
    }

    if (g.bookmarkTotal > g.bookmarkCount) {
        snprintf(line, sizeof line,
                 "   (+%zu more bookmarks than this panel draws; the console's `marks` lists them "
                 "all)",
                 g.bookmarkTotal - g.bookmarkCount);
        gui::Text(Counted(line));
    }
    if (dropped > 0) {
        snprintf(line, sizeof line,
                 "   (+%zu bookmark label(s) too close together to draw at this zoom; the buttons "
                 "above still reach every one)",
                 dropped);
        gui::Text(Counted(line));
    }
}

// The owner's "freely move around". W2-E's camera_control.h is the seam B.7 does not expose, and
// this is the row it exists for.
//
// ★ IT RENDERS RequestedMode(), NOT ObservedMode(), AND THAT IS NOT INTERCHANGEABLE. W1-D found the
// panel draws into a GUI frame whose input state is one frame behind; ObservedMode() is the CI read
// back, so a panel built on it shows the answer to last frame's question and its buttons feel wrong
// under the pointer. RequestedMode() is the value we published, which is the one the user just
// clicked. ObservedMode is diagnostics -- and it is genuinely useful there, because the engine's own
// auto-director and its SwitchTo* input bindings move the camera out from under us, so a disagreement
// between the two is real information rather than a glitch.
//
// ★ Api IS GREYED, NOT OMITTED. It is a real engine mode reserved for the oracle track; a mode that
// simply vanished from the row would read as one this build does not support.
//
// ★ THE LEVEL INDEX IS A STEPPER, NOT A SLIDER, because there is deliberately no LevelCameraCount().
// Reading it needs an unguarded call into game code, and W2-E's header is explicit that a method
// returning a constant 0 would be worse -- the panel could not tell an empty set from an
// unimplemented bound. So the engine's own clamp stands and ObservedSelector() shows where it
// landed, which makes the clamp visible instead of silent.
IReplayCameraControl* g_cameraOverride = nullptr;

IReplayCameraControl* CameraControl() {
    return g_cameraOverride != nullptr ? g_cameraOverride : camera_control::Get();
}

bool ModeTakesPlayer(CameraMode m) { return m == CameraMode::Pov || m == CameraMode::Follow; }

// The player row is 1-based over two teams of five; the level index is the engine's own, from 0.
int32_t SelectorFloor(CameraMode m) { return ModeTakesPlayer(m) ? 1 : 0; }

void DrawCameraRow() {
    IReplayCameraControl* cam = CameraControl();
    if (cam == nullptr || !cam->Available()) {
        gui::Text(Counted("camera: the spectator rig is not available in this session"));
        return;
    }
    if (cam->OracleTrackActive()) {
        // Not a refusal to explain after the fact: SetMode cannot succeed while a deterministic
        // track owns the camera, so the row says so instead of offering buttons that will refuse.
        gui::Text(Counted("camera: a deterministic oracle track owns the camera; mode selection is held"));
        return;
    }

    const CameraMode requested = cam->RequestedMode();
    char line[256];
    snprintf(line, sizeof line, "camera: %s", camera_control::ModeName(requested));
    gui::Text(Counted(line));

    // ★ NO LIST OF MODES LIVES HERE. camera_control's own table is derived from the binary -- from
    // the HTTP handler that string-matches the engine's seven names against 0..6 -- and a second
    // list in the panel is how two surfaces come to disagree about what the engine offers. DESIGN
    // 8.5.2's `cam` vocabulary is already wrong in exactly that way: it lists `orbit` and `asseen`,
    // which the engine does not have, and omits `disc` and `side`, which it does. So we walk the
    // enum's own range and let ModeName resolve each one; a value the table does not carry answers
    // "unknown" and is skipped rather than drawn as a button that resolves to nothing.
    for (int i = 0; i <= static_cast<int>(CameraMode::Api); ++i) {
        const CameraMode m = static_cast<CameraMode>(i);
        const char* name = camera_control::ModeName(m);
        if (strcmp(name, "unknown") == 0) continue;
        char b[40];
        snprintf(b, sizeof b, "%s##cam%d", name, i);
        gui::SameLine(0.0f, 4.0f);
        const bool selectable = camera_control::IsSelectableByHuman(m);
        if (!selectable) gui::PushButtonDisabled(gui::Vec4{0.35f, 0.35f, 0.35f, 1.0f});
        const bool hit = gui::Button(Counted(b), gui::Vec2{56.0f, 0.0f});
        if (!selectable) {
            gui::PopButtonDisabled();
        } else if (hit) {
            // A selector left at 0 by the level row is not a player: the player row is 1-based.
            if (g.camSelector < SelectorFloor(m)) g.camSelector = SelectorFloor(m);
            if (!cam->SetMode(m, g.camSelector)) {
                SetStatus("camera mode %s refused: %s", camera_control::ModeName(m),
                          ControlRefusalName(cam->LastRefusal()));
            }
        }
    }

    // The selector, stepped rather than slid. Pov and Follow take a 1-based player over two teams
    // of five; Level takes a camera index the engine clamps itself; the rest ignore it.
    const bool usesPlayer = ModeTakesPlayer(requested);
    const bool usesIndex = requested == CameraMode::Level;
    if (usesPlayer || usesIndex) {
        if (gui::Button(Counted("-##camdec"), gui::Vec2{28.0f, 0.0f}) &&
            g.camSelector > SelectorFloor(requested)) {
            --g.camSelector;
            cam->SetMode(requested, g.camSelector);
        }
        gui::SameLine(0.0f, 4.0f);
        // Only the PLAYER selector has a bound this packet can honour. The level index has none --
        // see the header -- so it steps without a ceiling and the engine's clamp is what stops it.
        const int32_t limit =
            usesPlayer ? IReplayCameraControl::kPlayerSelectorMax : 0x7FFFFFFF;
        if (gui::Button(Counted("+##caminc"), gui::Vec2{28.0f, 0.0f}) && g.camSelector < limit) {
            ++g.camSelector;
            cam->SetMode(requested, g.camSelector);
        }
        gui::SameLine(0.0f, 6.0f);

        int32_t packed = 0;
        uint16_t cameraIndex = 0;
        const bool observed = cam->ObservedSelector(&packed, &cameraIndex);
        uint16_t set = 0;
        const bool haveSet = cam->ActiveLevelCameraSet(&set);
        if (usesIndex) {
            snprintf(line, sizeof line,
                     "index %d -> engine %s%u%s   (no count is readable, so the engine's clamp is "
                     "the bound and this is where it landed)",
                     g.camSelector, observed ? "" : "?", observed ? cameraIndex : 0,
                     haveSet ? "" : "");
            if (haveSet) {
                const size_t n = strlen(line);
                snprintf(line + n, sizeof line - n, "  set %u", set);
            }
        } else {
            snprintf(line, sizeof line, "player %d of %d -> engine packed 0x%08X%s", g.camSelector,
                     IReplayCameraControl::kPlayerSelectorMax, observed ? packed : 0,
                     observed ? "" : " (unreadable)");
        }
        gui::Text(Counted(line));
    }

    // Diagnostics, and the one place ObservedMode belongs: the engine's auto-director and its own
    // input bindings can move the camera, and a silent disagreement here is a real one.
    CameraMode seen = requested;
    if (cam->ObservedMode(&seen) && seen != requested) {
        snprintf(line, sizeof line,
                 "  the engine has the camera in %s, not the %s we asked for -- its auto-director "
                 "or an input binding moved it",
                 camera_control::ModeName(seen), camera_control::ModeName(requested));
        gui::Text(Counted(line));
    }
}

// DESIGN 10.5's "mute voice off 1.0x", the default-on convenience. The voice
// player refuses every rate that is not 1.0x forward on its own -- a codec frame fed at the wrong
// rate is a broken decoder, not slow speech (voipplay.h) -- so the switch has no OFF position the
// panel could honour, and an unticked box that changed nothing would be the silent control this
// panel exists to refuse. What the panel owns is saying which state voice is in right now and
// how many frames the player has held back, in the player's own numbers.
void DrawVoiceRow() {
    const audio::voip::VoipPlayStats& v = audio::voip::PlaybackStats();
    char line[256];
    if (g.transportState == TransportState::Play && g.rate == 1.0f) {
        snprintf(line, sizeof line, "voice: live at 1.0x (%llu frame(s) injected)",
                 static_cast<unsigned long long>(v.injected));
    } else {
        snprintf(line, sizeof line,
                 "voice: muted off 1.0x (%s, rate %.2fx) -- always on: speech at any other rate is "
                 "a decoder fed at the wrong rate; %llu frame(s) held back at rate, %llu while "
                 "paused or scrubbing",
                 TransportStateName(g.transportState), static_cast<double>(g.rate),
                 static_cast<unsigned long long>(v.skippedRate),
                 static_cast<unsigned long long>(v.skippedNoStep));
    }
    gui::Text(Counted(line));
}

void DrawTransportBody() {
    IReplayTransport* t = Transport();
    char now[32], total[32], line[256];
    FormatTime(g.playheadUs, now, sizeof now);
    FormatTime(g.durationUs, total, sizeof total);

    if (!g.haveTransport) {
        gui::Text(Counted("No replay transport has registered yet."));
        gui::Text(Counted("W2-B publishes IReplayTransport into ReplayServices; until it does, the panel"));
        gui::Text(Counted("can browse captures but cannot play one."));
        return;
    }

    snprintf(line, sizeof line, "%s / %s   %s   rate %.2fx", now, total,
             TransportStateWord(g.transportState, t ? t->Ended() : EndState::None),
             static_cast<double>(g.rate));
    gui::Text(Counted(line));

    // ★ WHY IT IS NOT PLAYING YET. A held picture with the word "paused" under it is what a hang
    // looks like; this is the one line that says the wait is the replay getting the disc ready.
    const PrepareStatus prep = t ? t->Preparing() : PrepareStatus();
    if (prep.preparing) {
        char bar[24];
        const int filled = (int)(prep.progress * 20.0 + 0.5);
        for (int i = 0; i < 20; ++i) bar[i] = i < filled ? '=' : '.';
        bar[20] = '\0';
        snprintf(line, sizeof line, "preparing replay... [%s] %2.0f%% (host pre-match %.0f s)%s",
                 bar, prep.progress * 100.0, prep.hostRemainingS,
                 prep.playRequested ? "  - play starts when it is ready" : "");
        gui::Text(Counted(line));
    }

    // ★ B.8's own words: "never silent degradation". Whatever the transport says it is actually
    // doing is printed verbatim beside what we asked for, so "FF 6x (micro-seek)" reaches the user
    // rather than a 6x we are not really achieving.
    if (g.modeLabel[0] != '\0') {
        snprintf(line, sizeof line, "engine: %s", g.modeLabel);
        gui::Text(Counted(line));
    }
    DrawVoiceRow();

    if (g.durationUs <= 0) {
        // A strip over a zero-length span is 115 buttons that all seek to the same instant: it
        // looks like a working scrubber and is not one.
        gui::Text(Counted("this capture has no duration yet, so there is no timeline to scrub"));
        CSeparator();
        DrawCameraRow();
        return;
    }

    const Span whole = {0, g.durationUs};
    DrawMarkerLabels(whole);
    int64_t seek = 0;
    if (DrawStrip("c", whole, &seek)) SeekTo(seek);

    CSeparator();

    const Span fine = FineSpan(g.playheadUs, g.durationUs, g.fineWindowUs);
    char fb[32], fe[32];
    FormatTime(fine.beginUs, fb, sizeof fb);
    FormatTime(fine.endUs, fe, sizeof fe);
    snprintf(line, sizeof line, "fine  %s - %s   (%.2f s per cell)", fb, fe,
             static_cast<double>(fine.DurationUs()) / 1e6 /
                 static_cast<double>(CellCount(kContentW, kCellW)));
    gui::Text(Counted(line));
    DrawMarkerLabels(fine);
    if (DrawStrip("f", fine, &seek)) SeekTo(seek);

    for (size_t i = 0; i < sizeof kFineWindows / sizeof kFineWindows[0]; ++i) {
        char b[24];
        snprintf(b, sizeof b, "%llds##fw%zu",
                 static_cast<long long>(kFineWindows[i] / 1000000), i);
        if (i > 0) gui::SameLine(0.0f, 4.0f);
        if (gui::Button(Counted(b), gui::Vec2{44.0f, 0.0f})) g.fineWindowUs = kFineWindows[i];
    }

    CSeparator();

    // Transport row. Step is frames, not seconds: B.8's Step(int) consumes recorded ticks, which is
    // the only unit in which "one frame back" is exact.
    struct Cmd {
        const char* label;
        int step;
        int64_t nudgeUs;
    };
    static const Cmd kCmds[] = {
        {"|<##home", 0, 0},   {"-10s##b10", 0, -10000000}, {"-1s##b1", 0, -1000000},
        {"-1f##bf", -1, 0},   {"+1f##ff", 1, 0},           {"+1s##f1", 0, 1000000},
        {"+10s##f10", 0, 10000000}, {">|##end", 0, 0},
    };
    for (size_t i = 0; i < sizeof kCmds / sizeof kCmds[0]; ++i) {
        if (i > 0) gui::SameLine(0.0f, 4.0f);
        if (!gui::Button(Counted(kCmds[i].label), gui::Vec2{52.0f, 0.0f})) continue;
        if (strncmp(kCmds[i].label, "|<", 2) == 0) {
            SeekTo(0);
        } else if (strncmp(kCmds[i].label, ">|", 2) == 0) {
            SeekTo(g.durationUs);
        } else if (kCmds[i].step != 0) {
            t->Step(kCmds[i].step);
        } else {
            SeekTo(g.playheadUs + kCmds[i].nudgeUs);
        }
    }

    if (gui::Button(Counted(g.transportState == TransportState::Play ? "Pause##pp" : "Play##pp"),
                    gui::Vec2{72.0f, 0.0f})) {
        if (g.transportState == TransportState::Play) {
            t->Pause();
        } else {
            // ★ NEVER a rate of zero. The clock derives a reciprocal from the scaled delta and a
            // zero yields +inf into every consumer; pause is the pause bit, and a resume from
            // rate 0 would be indistinguishable from it here if we let the rate reach 0.
            t->Play(g.rate != 0.0f ? g.rate : 1.0f);
        }
    }
    gui::SameLine(0.0f, 8.0f);
    if (gui::Button(Counted("Reverse##rev"), gui::Vec2{72.0f, 0.0f})) {
        // A toggle: the flipped rate is written back, so pressing it twice returns to forward. And
        // it is a rate change, not a play -- reversing while paused leaves the picture still.
        g.rate = (g.rate != 0.0f ? -g.rate : -1.0f);
        if (g.transportState == TransportState::Play) {
            t->Play(g.rate);
        } else {
            t->SetRate(g.rate);
        }
    }

    // The row is DERIVED from the ceiling, never listed. A fixed ladder put a 4x button in front of
    // a machine whose ceiling is 2.00x, one line above the sentence that says so (L3-RUN-10 F4);
    // greying it out was not enough, because the set on offer and the sentence beneath it still
    // disagreed about what this machine can do. OfferedRates answers with rates this ceiling
    // admits, the ceiling itself among them.
    float offered[kOfferedRateCap];
    const size_t offeredCount = OfferedRates(g.ceiling, offered, kOfferedRateCap);
    for (size_t i = 0; i < offeredCount; ++i) {
        char b[24];
        // %.4g keeps the ladder's short labels (0.1x, 0.25x, 2x) and prints a measured ceiling the
        // way the sentence below prints it, so the row and the sentence never state different
        // numbers for the same limit.
        snprintf(b, sizeof b, "%.4gx##r%zu", static_cast<double>(offered[i]), i);
        gui::SameLine(0.0f, 4.0f);
        if (gui::Button(Counted(b), gui::Vec2{48.0f, 0.0f})) {
            g.rate = offered[i];
            if (g.transportState == TransportState::Play) t->Play(g.rate);
        }
    }

    if (g.ceiling > 0.0f) {
        snprintf(line, sizeof line,
                 "fast-forward ceiling %.2fx = %u us / %.0f us per frame (%s); above it the engine "
                 "stops accelerating without saying so",
                 ReportedRate(static_cast<double>(g.ceiling)), g.maxElapsedUs,
                 g.framePeriodEma,
                 g.maxElapsedLive ? "read from the live clock" : "CLOCK.MAXELAPSED_US constant");
    } else {
        snprintf(line, sizeof line,
                 "fast-forward ceiling unknown (%s), so no rate is being refused. It is NOT assumed "
                 "to be 4x -- that is only true at 60 Hz.", CeilingWhy());
    }
    gui::Text(Counted(line));

    CSeparator();
    if (gui::Have(gui::Entry::InputReal)) {
        gui::InputReal(Counted("seconds##exact"), &g.exactSeconds, 1.0f, 10.0f, 3, 0, 0.0f,
                       static_cast<float>(g.durationUs) / 1e6f);
        gui::SameLine(0.0f, 6.0f);
        if (gui::Button(Counted("Go##goexact"), gui::Vec2{48.0f, 0.0f})) {
            SeekTo(static_cast<int64_t>(static_cast<double>(g.exactSeconds) * 1e6));
        }
    } else {
        gui::Text(Counted("exact seek unavailable: GUI.INPUTREAL did not bind (use the console `seek`)"));
    }

    CSeparator();
    DrawBookmarks(whole);

    CSeparator();
    DrawRoundJumps();
    if (!g.markersKnown) {
        gui::Text(Counted("round markers unavailable: this capture's manifest could not be read"));
    } else if (g.markerCount == 0) {
        gui::Text(Counted("this capture records no round markers"));
    }

    CSeparator();
    if (gui::Have(gui::Entry::Checkbox)) {
        const bool was = g.asSeen;
        gui::Checkbox(Counted("as-seen mode (what the client saw, not ground truth)##mode"),
                  &g.asSeen);
        if (g.asSeen != was) t->SetMode(g.asSeen);
    } else {
        gui::Text(Counted("mode switch unavailable: GUI.CHECKBOX did not bind (use the console `mode`)"));
    }
    gui::SameLine(0.0f, 12.0f);
    if (gui::Button(Counted("Unload##unload"), gui::Vec2{80.0f, 0.0f})) Unload();

    CSeparator();
    DrawFidelityLine();

    CSeparator();
    DrawCameraRow();
}

// The counter is reset and published HERE rather than inside the body, so the body's three early
// returns cannot leave a frame uncounted -- and a fourth return added later is counted by
// construction, not by remembering.
void DrawTransportView() {
    g_stream.items = 0;
    g_stream.glyphs = 0;
    DrawTransportBody();
    inframe::NoteTransportGuiStream(g_stream.items, g_stream.glyphs);
}

// Builds one snapshot on the worker thread: the listing, every header and manifest the page shows,
// and every string the browser will draw. Nothing here runs on a thread the game renders on.
// The capture's name, fitted to its column. These names are a timestamp, two session ids and an
// extension -- the timestamp IS the row's date and time, which is why there is no separate column
// for it -- so a plain right-hand cut throws away the extension and keeps the 32 characters of
// zeroes every row shares. The cut is taken out of the MIDDLE, where the shared part is. `dst`
// always comes back padded to exactly cap-1 characters, which is what makes the columns line up
// without a separator widget between them.
void FitName(char* dst, size_t cap, const char* src) {
    const size_t width = cap - 1;
    const size_t len = strlen(src);
    if (len <= width) {
        snprintf(dst, cap, "%-*s", static_cast<int>(width), src);
        return;
    }
    size_t head = width - kRowNameTail - 3;
    while (head > 0 && (static_cast<unsigned char>(src[head]) & 0xC0) == 0x80) --head;
    size_t tailAt = len - kRowNameTail;
    while (tailAt < len && (static_cast<unsigned char>(src[tailAt]) & 0xC0) == 0x80) ++tailAt;
    size_t n = 0;
    memcpy(dst, src, head);
    n += head;
    memcpy(dst + n, "...", 3);
    n += 3;
    memcpy(dst + n, src + tailAt, len - tailAt);
    n += len - tailAt;
    while (n < width) dst[n++] = ' ';
    dst[n] = '\0';
}

void BuildSnapshot(uint64_t seq) {
    BrowserSnapshot* snap = new BrowserSnapshot();
    snap->seq = seq;
    {
        IndexLock lock(g_indexMutex);
        if (g.index != nullptr) {
            const uint64_t t0 = NowNanos();
            const bool listed = g.index->Scan(g.replayDir);
            const uint64_t t1 = NowNanos();
            snap->scanNanos = t1 - t0;
            if (!listed) {
                snap->scanFailed = true;
                snap->scanError = g.index->LastScanError();
            }
            snap->totalCount = g.index->Count();
            // ★ THE PLAYABLE-ONLY FILTER, and it is a filter on the LISTING, not on the page. Of
            // the owner's 154 captures most are solo and lobby sessions with nobody in them: a
            // pager over all of them is seven pages of files that load to an empty rail. A row is
            // hidden only when its manifest was actually read AND recorded zero players -- a row
            // nobody could open stays visible, because a capture that vanishes from the browser is
            // the worst possible way to learn its footer is torn (demoindex.h:66-69).
            //
            // Classifying the whole listing costs one stage-2 manifest read per file, once: both
            // stages are idempotent and cached in the index, and every one of them happens HERE, on
            // the browser worker. fileio::DrawThreadOpenCount() stays 0.
            snap->filtered = g_worker.playableOnly.load(std::memory_order_relaxed);
            std::vector<size_t> shownIndex;
            shownIndex.reserve(snap->totalCount);
            for (size_t i = 0; i < snap->totalCount; ++i) {
                if (snap->filtered) {
                    g.index->LoadManifest(i);
                    if (g.index->ManifestLoaded(i) && g.index->At(i).playerCount == 0) continue;
                }
                shownIndex.push_back(i);
            }
            snap->fileCount = shownIndex.size();
            snap->hiddenCount = snap->totalCount - snap->fileCount;
            // ★ THE PAGE IS WHAT IS READ, NOT THE FIRST 24 FILES. The old cut was absolute: with
            // 152 captures the owner's newest was the 25th and no amount of Next reached it
            // (LAUNCH-UI3 §9). The listing is whole; only the window of rows this page draws has
            // its header and manifest opened.
            size_t page = g_worker.page.load(std::memory_order_relaxed);
            const size_t pages = (snap->fileCount + kRowsPerPage - 1) / kRowsPerPage;
            if (pages > 0 && page >= pages) page = pages - 1;
            snap->firstIndex = page * kRowsPerPage;
            if (snap->firstIndex > snap->fileCount) snap->firstIndex = 0;
            const size_t remaining = snap->fileCount - snap->firstIndex;
            const size_t shown = remaining < kRowsPerPage ? remaining : kRowsPerPage;
            // Every row the page shows is read HERE, in full. The old code read eight headers and
            // four manifests per game tick, which spread the cost over frames without taking it
            // off the frames.
            for (size_t k = snap->firstIndex; k < snap->firstIndex + shown; ++k) {
                g.index->ReadRow(shownIndex[k]);
                g.index->LoadManifest(shownIndex[k]);
            }
            const uint64_t t2 = NowNanos();
            snap->metadataNanos = t2 - t1;

            char buf[512];
            snprintf(buf, sizeof buf, "%zu capture(s), scanned in %llu us", snap->totalCount,
                     static_cast<unsigned long long>(g.index->LastScanMicros()));
            snap->header = buf;

            snap->rows.reserve(shown);
            for (size_t k = snap->firstIndex; k < snap->firstIndex + shown; ++k) {
                const size_t i = shownIndex[k];
                const DemoSummary& sum = g.index->At(i);
                SnapshotRow row;
                row.path = g.index->Path(i);

                row.legacyUncached = g.index->IsLegacy(i) && !g.index->LegacyCached(i);
                char dur[32] = "-";
                if (!row.legacyUncached) {
                    FormatTime(static_cast<int64_t>(sum.durationUs), dur, sizeof dur);
                }

                char badges[192];
                badges[0] = '\0';
                auto badge = [&](const char* b) {
                    const size_t used = strlen(badges);
                    snprintf(badges + used, sizeof badges - used, "%s[%s]", used ? " " : "", b);
                };
                if (!g.index->RowRead(i)) {
                    // Zero flags would read as NO VOIP, NO AS-SEEN and NO ORACLE, which is three
                    // claims about a file whose header never parsed.
                    badge("HEADER UNREAD");
                } else if (row.legacyUncached) {
                    // No header exists until it is converted, so no flag badge applies; the LEGACY
                    // word rides in the columns below, where the width cut cannot reach it.
                } else {
                    if ((sum.flags & kFlagImported) != 0) badge("IMPORTED - reduced fidelity");
                    if ((sum.flags & kFlagOpen) != 0) badge("NOT CLOSED");
                    if ((sum.flags & kFlagTruncatedByBudget) != 0) badge("TRUNCATED");
                    if ((sum.flags & kFlagHasVoip) == 0) badge("NO VOIP");
                    if ((sum.flags & kFlagHasAsSeen) == 0) badge("NO AS-SEEN");
                    if ((sum.flags & kFlagHasOracle) == 0) badge("NO ORACLE");
                    if (g.index->BuildMatchOf(i) == BuildMatch::Mismatch) badge("CONTENT MISMATCH");
                    if (g.index->BuildMatchOf(i) == BuildMatch::NotChecked) badge("BUILD NOT CHECKED");
                    if (g.index->BuildMatchOf(i) == BuildMatch::NotRecorded) badge("BUILD NOT RECORDED");
                    if (g.index->Fault(i) != RowFault::None) badge(RowFaultName(g.index->Fault(i)));
                }

                char counts[64];
                char level[kRowLevelChars + 1] = "-";
                if (g.index->ManifestLoaded(i)) {
                    // Manifest::roundCount is the S_MARK EDGE count -- fourteen edges around one
                    // round is routine (F-REAL-3: `markers: 14` beside `ROUNDS 1`). The rounds the
                    // file can be seeked by are the round-TABLE rows, and a capture written before
                    // the table carries no block at all, which is could-not-ask, not zero.
                    char roundsField[8] = "?";
                    if (sum.manifest != nullptr &&
                        sum.manifest->markerSource != replay::MarkerSource::NotRecorded) {
                        snprintf(roundsField, sizeof roundsField, "%u",
                                 sum.manifest->roundRowCount);
                    }
                    snprintf(counts, sizeof counts, "%s rounds, %u players", roundsField,
                             sum.playerCount);
                    row.manifestKnown = true;
                    row.playerCount = sum.playerCount;
                    FormatLevel(sum.manifest != nullptr, sum.manifest ? sum.manifest->levelId : 0,
                                level, sizeof level);
                    row.levelId = sum.manifest != nullptr ? sum.manifest->levelId : 0;
                    row.gametypeId =
                        sum.manifest != nullptr
                            ? ResolveEntryGametype(nullptr, sum.manifest->gametypeId).id
                            : 0;
                } else {
                    // A zero here would read as "no rounds, no players", a different claim.
                    snprintf(counts, sizeof counts, "rounds -, players -");
                }

                // ★ FIXED-WIDTH COLUMNS, JOINED ONCE. Four strings drawn as four widgets cost four
                // ItemAdds and four ID hashes for one row; joined here they cost one, and the
                // padding is what a column separator would otherwise have to be.
                char name[kRowNameChars + 1];
                FitName(name, sizeof name, g.index->DisplayName(i));
                // The level is LAST and unpadded: it is the only column with nothing to its right
                // to line up against, and eighteen padded characters per row are eighteen the
                // width cut takes off the reason that follows.
                if (row.legacyUncached) {
                    // DESIGN-FORMATS 7.8: name, size and time, and the badge. A duration, counts
                    // and level column would be three dashes about a file nobody has converted.
                    char size[16];
                    const double mb = static_cast<double>(sum.sizeBytes) / (1024.0 * 1024.0);
                    if (mb >= 1.0) {
                        snprintf(size, sizeof size, "%.1f MB", mb);
                    } else {
                        snprintf(size, sizeof size, "%.0f KB",
                                 static_cast<double>(sum.sizeBytes) / 1024.0);
                    }
                    char when[24] = "-";
                    FILETIME ft;
                    ft.dwLowDateTime = static_cast<DWORD>(sum.mtime);
                    ft.dwHighDateTime = static_cast<DWORD>(sum.mtime >> 32);
                    SYSTEMTIME utc, local;
                    if (::FileTimeToSystemTime(&ft, &utc) != 0 &&
                        ::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) != 0) {
                        snprintf(when, sizeof when, "%04u-%02u-%02u %02u:%02u", local.wYear,
                                 local.wMonth, local.wDay, local.wHour, local.wMinute);
                    }
                    snprintf(buf, sizeof buf, "%s %*s  LEGACY - converts on open  %s", name,
                             static_cast<int>(kRowDurationChars), size, when);
                } else {
                    snprintf(buf, sizeof buf, "%s %*s  %-*.*s  %s", name,
                             static_cast<int>(kRowDurationChars), dur,
                             static_cast<int>(kRowCountsChars),
                             static_cast<int>(kRowCountsChars), counts, level);
                }
                row.columns = buf;
                row.badges = badges;

                if (g.index->Fault(i) != RowFault::None) row.faultDetail = g.index->FaultDetail(i);
                snap->rows.push_back(std::move(row));
            }

            snap->overflowShown = shown;
            snap->formatNanos = NowNanos() - t2;
        }
    }
    inframe::NoteSnapshotPhases(snap->scanNanos, snap->metadataNanos, snap->formatNanos);
    // Whoever wins this exchange owns what it takes out. A snapshot the tick never collected is
    // freed by the worker that displaced it, so a browser nobody is looking at cannot leak.
    delete g_worker.pending.exchange(snap, std::memory_order_acq_rel);
    g_worker.published.store(seq, std::memory_order_release);
}

DWORD WINAPI WorkerMain(LPVOID) {
    for (;;) {
        uint64_t seq = 0;
        {
            std::unique_lock<std::mutex> lk(g_worker.m);
            g_worker.cv.wait(
                lk, [] { return g_worker.stop || g_worker.requested > g_worker.started; });
            if (g_worker.stop) return 0;
            seq = g_worker.requested;
            g_worker.started = seq;
        }
        BuildSnapshot(seq);
    }
}

// Requested from the tick, never from the draw: the request itself takes a lock, and the render
// path takes none.
void RequestScan() {
    g_worker.page.store(g.browserPage, std::memory_order_relaxed);
    g_worker.playableOnly.store(g.playableOnly, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_worker.m);
        ++g_worker.requested;
    }
    g_worker.cv.notify_one();
}

void StartWorker() {
    std::lock_guard<std::mutex> lk(g_worker.m);
    if (g_worker.thread != nullptr) return;
    g_worker.stop = false;
    g_worker.thread = CreateThread(nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
}

// False when the worker did not finish inside `waitMs`: it is left running and keeps everything it
// may still touch.
bool StopWorker(DWORD waitMs) {
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_worker.m);
        g_worker.stop = true;
        thread = g_worker.thread;
    }
    g_worker.cv.notify_one();
    if (thread != nullptr) {
        if (WaitForSingleObject(thread, waitMs) != WAIT_OBJECT_0) return false;
        CloseHandle(thread);
        std::lock_guard<std::mutex> lk(g_worker.m);
        g_worker.thread = nullptr;
    }
    delete g_worker.pending.exchange(nullptr, std::memory_order_acq_rel);
    return true;
}

// ★ THE SWAP, and it is ONE atomic exchange. The tick owns what it takes out and frees what it
// replaces; the draw that follows reads a pointer no other thread can touch. A non-atomic
// hand-over -- a flag beside a pointer, or a vector filled in place -- is a frame that draws half
// of one listing and half of another.
// ★ CUTS A DRAWN LINE TO WHAT THE WINDOW CAN SHOW, ON THE TICK, BEFORE ITS ##id IS APPENDED.
// ImGui sizes the WHOLE string and then clips it, so a character past the right edge costs exactly
// what a visible one costs and is read by nobody. Two things this gets right and the obvious
// version does not: a UTF-8 character is never halved, and the "..." is INSIDE the budget -- a cut
// line that came back three characters wider than the budget would make the budget unassertable.
// Returns the characters ImGui will measure, which is what the caller publishes as glyphs.
size_t CutToWidth(char* s) {
    const size_t len = strlen(s);
    if (len <= kRowChars) return len;
    size_t cut = kRowChars - 3;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    // Back up to the last space, so a line that does not fit ends on a whole word instead of
    // "(18 hidden: no ...". Bounded, because one long token must not eat the line that precedes it.
    const size_t floorAt = cut > 12 ? cut - 12 : 0;
    for (size_t at = cut; at > floorAt; --at) {
        if (s[at - 1] == ' ') {
            cut = at - 1;
            break;
        }
    }
    memcpy(s + cut, "...", 3);
    s[cut + 3] = '\0';
    return cut + 3;
}

void TakeSnapshot() {
    BrowserSnapshot* fresh = g_worker.pending.exchange(nullptr, std::memory_order_acquire);
    if (fresh == nullptr) return;
    delete g.live;
    g.live = fresh;
    g.liveSeq = fresh->seq;
    // The pager owns the page, not the snapshot: a snapshot in flight when the owner clicks Next
    // would otherwise undo the click. All this does is keep the page inside a listing that may
    // have shrunk since it was chosen.
    const size_t pages = (fresh->fileCount + kRowsPerPage - 1) / kRowsPerPage;
    if (pages == 0) {
        g.browserPage = 0;
    } else if (g.browserPage >= pages) {
        g.browserPage = pages - 1;
    }
    for (size_t i = 0; i < kBrowserRows; ++i) {
        g.rowWhy[i] = nullptr;
        g.rowLine[i][0] = '\0';
        g.rowGlyphs[i] = 0;
        g.rowRefused[i] = false;
    }
    if (fresh->scanFailed) ReportScanFailure(fresh->scanError);
}

// The session half of each row, decided on the tick because ISessionEntry is engine state and the
// tick is the only place this packet reads engine state. Re-formatted only when a row's answer
// changes, so a page that says the same thing costs nothing to keep saying.
void UpdateRowVerdicts() {
    const BrowserSnapshot* snap = g.live;
    if (snap == nullptr) return;
    const size_t rows = snap->rows.size() < kBrowserRows ? snap->rows.size() : kBrowserRows;
    // The pager's sentence, rebuilt only when the page or the listing changes. It counts against
    // the whole listing, newest first, so "showing 1-17 of 152" is the truth and not the cut.
    if (g.pageLabelPage != snap->firstIndex || g.pageLabelRows != rows ||
        g.pageLabelHidden != snap->hiddenCount) {
        g.pageLabelPage = snap->firstIndex;
        g.pageLabelRows = rows;
        g.pageLabelHidden = snap->hiddenCount;
        // The hidden ones are COUNTED, not dropped: "of 9 playable" without "(145 hidden)" would
        // be a browser quietly claiming the folder holds nine files.
        // ★ SHORT ENOUGH TO SURVIVE THE DEFAULT WINDOW. Beside the scan header this line is one
        // drawn row, and the old wording ran past kRowCharBudget and was cut mid-word at every
        // default-width load (LAUNCH-MULTILOAD §3.1). "no players" is what the toggle beside it
        // already says and what the all-hidden sentence spells out, so the clause keeps the number
        // and drops the explanation.
        char hidden[64];
        hidden[0] = '\0';
        if (snap->filtered) {
            snprintf(hidden, sizeof hidden, ", %zu hidden", snap->hiddenCount);
        }
        snprintf(g.pageLabel, sizeof g.pageLabel, "newest first, %zu-%zu of %zu%s%s",
                 rows == 0 ? 0 : snap->firstIndex + 1, snap->firstIndex + rows, snap->fileCount,
                 snap->filtered ? " playable" : "", hidden);
    }
    // ★ THE ROW TEXT IS CACHED BY ROW INDEX, so a page change has to drop it: row 0 of page 1 is a
    // different capture with the same verdict as row 0 of page 0, and the `continue` below would
    // keep drawing the old page's filenames under the new page's listing.
    if (g.rowsFromIndex != snap->firstIndex) {
        g.rowsFromIndex = snap->firstIndex;
        for (size_t i = 0; i < kBrowserRows; ++i) {
            g.rowWhy[i] = nullptr;
            g.rowVerdict[i] = 0xFF;
        }
    }
    for (size_t i = 0; i < rows; ++i) {
        const SnapshotRow& r = snap->rows[i];
        const char* why = "";
        const EntryVerdict verdict = DecideEntryIds(r.manifestKnown, r.levelId, r.gametypeId, &why);
        if (g.rowWhy[i] == why && g.rowVerdict[i] == static_cast<uint8_t>(verdict)) continue;
        g.rowWhy[i] = why;
        g.rowVerdict[i] = static_cast<uint8_t>(verdict);
        g.rowRefused[i] = verdict == EntryVerdict::Refused;
        // ★ DESIGN 8.4.3, three states and not two -- and the state is the FIRST thing on the
        // line, where the width cut can never reach it. A row whose manifest could not be read is
        // NOT a row we checked and cleared, and drawing them identically offers an action that
        // fails. The reason follows the columns and is cut with them; the click on a greyed row is
        // what asks for the whole of it.
        const char* mark = "  ";
        char tail[512];
        tail[0] = '\0';
        if (verdict == EntryVerdict::Refused) {
            mark = "x ";
            snprintf(tail, sizeof tail, "cannot load in this session: %s", why);
        } else if (verdict == EntryVerdict::NotChecked && r.legacyUncached) {
            // Nothing can be checked before the conversion exists; the load checks it after, and
            // the row's own columns already say LEGACY.
            mark = "? ";
        } else if (verdict == EntryVerdict::NotChecked) {
            mark = "? ";
            snprintf(tail, sizeof tail, "NOT CHECKED: %s", why);
        } else if (verdict == EntryVerdict::MustEnter) {
            mark = "> ";
            snprintf(tail, sizeof tail, "this capture is in another level; opening it re-enters "
                                        "the session");
        } else if (!r.faultDetail.empty()) {
            snprintf(tail, sizeof tail, "%s", r.faultDetail.c_str());
        }
        // Reason first, badges last: the line is cut from the right, and "cannot load in this
        // session: <why>" is what the owner needs before "[NO VOIP]".
        char line[1024];
        snprintf(line, sizeof line, "%s%s%s%s  %s", mark, r.columns.c_str(),
                 tail[0] != '\0' ? "  " : "", tail, r.badges.c_str());
        g.rowGlyphs[i] = static_cast<uint32_t>(CutToWidth(line));
        snprintf(g.rowLine[i], sizeof g.rowLine[i], "%s##row%zu", line, i);
    }
}

// The two chrome lines the browser always draws, as ONE Text. The listing's numbers and the page
// it is showing belong on one line; the status line is what csregister and the facade have to say
// about the panel, and the browser page needs it for the same reason the transport page does -- a
// scan failure has nowhere else to be read. Two lines, one ItemAdd, one ID hash.
//
// StatusLine() reads csregister's report and may format; it runs HERE, on the tick, and never
// again inside the frame that is being timed.
void BuildHeaderBlock() {
    const BrowserSnapshot* snap = g.live;
    char first[kRowChars * 4];
    if (snap == nullptr) {
        snprintf(first, sizeof first,
                 "listing the capture folder... (the scan runs off this thread)");
    } else {
        snprintf(first, sizeof first, "%s  |  %s%s%s", snap->header.c_str(), g.pageLabel,
                 snap->overflowLine.empty() ? "" : "  |  ", snap->overflowLine.c_str());
    }
    char second[kRowChars * 4];
    snprintf(second, sizeof second, "%s", StatusLine());
    const size_t firstChars = CutToWidth(first);
    const size_t secondChars = CutToWidth(second);
    snprintf(g.headerFirst, sizeof g.headerFirst, "%s", first);
    snprintf(g.headerBlock, sizeof g.headerBlock, "%s\n%s", first, second);
    // The newline is a character ImGui measures like any other, so it is inside the budget.
    g.headerGlyphs = static_cast<uint32_t>(firstChars + 1 + secondChars);
}

// ★ READS THE SNAPSHOT AND CALLS THE FACADE. No file, no allocation, no snprintf, no lock. Every
// string below was built on the worker; every verdict below was decided on the tick.
void DrawBrowserView() {
    const BrowserSnapshot* snap = g.live;
    // ★ THE STREAM THIS FRAME ISSUES, COUNTED AS IT IS ISSUED. Not derived from the layout by a
    // second copy of it -- a count that agreed with the code only until one of them changed would
    // certify the draw against itself. Every length was measured on the tick and travels with its
    // string, so this adds two adds per widget and not one strlen.
    uint32_t items = 0;
    uint32_t glyphs = 0;
    const auto text = [&items, &glyphs](const char* s, uint32_t chars) {
        ++items;
        glyphs += chars;
        gui::Text(s);
    };
    const auto button = [&items, &glyphs](const char* label, uint32_t chars, float w) {
        ++items;
        glyphs += chars;
        return gui::Button(label, gui::Vec2{w, 0.0f});
    };

    // The two chrome buttons come first, on their own line: SameLine after the header block would
    // place them past the widest of its two lines, which is a full-width status line often enough
    // to push them off the window the panel pinned.
    if (button("Rescan##rescan", 6, 72.0f)) g.scanRequested = true;
    gui::SameLine(0.0f, 8.0f);
    if (button("Close##closebrowse", 5, 72.0f)) g.browserOpen = false;
    gui::SameLine(0.0f, 8.0f);
    // ★ DRAWN BEFORE THE EMPTY-LISTING RETURN BELOW, because a filter that hid every row would
    // otherwise be a browser with nothing in it and no way to turn the filter off. One click, and
    // the pager's sentence beside it always says how many rows it is hiding.
    if (button(g.playableOnly ? "playable only: ON##playable" : "playable only: OFF##playable",
               static_cast<uint32_t>(g.playableOnly ? kBrowserChromeToggleChars - 1
                                                    : kBrowserChromeToggleChars),
               160.0f)) {
        g.playableOnly = !g.playableOnly;
        g.browserPage = 0;   // page 4 of the unfiltered listing is off the end of the filtered one
        g.scanRequested = true;
    }
    // The conversion's progress is the status line in the header block below; this stops it.
    if (g.importRunning) {
        gui::SameLine(0.0f, 8.0f);
        static const char kCancel[] = "Cancel conversion##cancelconvert";
        if (button(kCancel, 17, 150.0f)) g.importCancelRequested = true;
    }
    // Empty only when the browser was opened from outside the frame loop and no tick has run yet:
    // one frame, and it says so rather than drawing an empty line.
    static const char kOpening[] = "opening the capture browser...";
    const bool haveHeader = g.headerBlock[0] != '\0';
    text(haveHeader ? g.headerBlock : kOpening,
         haveHeader ? g.headerGlyphs : static_cast<uint32_t>(sizeof kOpening - 1));

    if (snap == nullptr) {
        inframe::NoteGuiStream(items, glyphs);
        return;
    }
    if (snap->rows.empty()) {
        static const char kAllHidden[] =
            "every capture here records no players; turn `playable only` off to see them:";
        const bool hiddenAll = snap->filtered && snap->hiddenCount > 0;
        static const char kNoReplays[] = "No .echodemo, .echoreplay or .tape files in:";
        text(hiddenAll ? kAllHidden : kNoReplays,
             static_cast<uint32_t>(hiddenAll ? sizeof kAllHidden - 1 : sizeof kNoReplays - 1));
        text(g.replayDirNarrow, static_cast<uint32_t>(strlen(g.replayDirNarrow)));
        inframe::NoteGuiStream(items, glyphs);
        return;
    }

    // ★ ONE PAGE OF ROWS, WHICH IS WHAT A CLIPPER IS FOR. The facade cannot be asked what is
    // visible, so the panel decides it: kRowsPerPage is what the window it pinned can show. The
    // snapshot now IS the page -- the worker built it for g.browserPage -- so every row here is
    // drawn and Prev/Next ask the worker for the next window of the whole listing.
    const size_t rows = snap->rows.size() < kBrowserRows ? snap->rows.size() : kBrowserRows;
    for (size_t i = 0; i < rows; ++i) {
        const SnapshotRow& row = snap->rows[i];
        const bool refused = g.rowRefused[i];
        if (refused) gui::PushButtonDisabled(gui::Vec4{0.35f, 0.35f, 0.35f, 1.0f});
        const bool hit = button(g.rowLine[i], g.rowGlyphs[i], kContentW);
        if (refused) gui::PopButtonDisabled();
        // ★ A CLICK IS RECORDED, NOT SERVICED. LoadPath opens the file, reads its manifest and
        // hands it to the transport; doing that here would put a disk seek inside the frame the
        // owner clicked in, which is the whole defect this file was rewritten to remove.
        if (!hit) continue;
        if (refused) {
            // The reason was cut to fit the row. The click is the owner asking for the rest of it.
            g.refusedClick = static_cast<int>(i);
        } else {
            wcsncpy_s(g.loadRequestPath, kPathMax, row.path.c_str(), _TRUNCATE);
            g.loadRequested = true;
        }
    }

    // The pager's sentence rides the header line, so these two are the whole control: two widgets
    // rather than two widgets and a third that repeats what the header already said.
    if (snap->fileCount > kRowsPerPage) {
        if (button("< Prev##browseprev", 6, 72.0f) && snap->firstIndex > 0) {
            --g.browserPage;
            g.scanRequested = true;
        }
        gui::SameLine(0.0f, 8.0f);
        if (button("Next >##browsenext", 6, 72.0f) &&
            snap->firstIndex + rows < snap->fileCount) {
            ++g.browserPage;
            g.scanRequested = true;
        }
    }
    inframe::NoteGuiStream(items, glyphs);
}

// A .echoreplay/.tape is never opened on the tick: it is handed to the conversion worker, and the
// tick loads the cached .echodemo once the worker reports it (ServiceImport). Answers false because
// nothing is loaded yet; ImportInProgress() tells a caller that the request was taken.
bool BeginImport(const wchar_t* path) {
    const wchar_t* leaf = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') leaf = p + 1;
    }
    char name[sizeof g.importName];
    if (::WideCharToMultiByte(CP_UTF8, 0, leaf, -1, name, sizeof name, nullptr, nullptr) == 0) {
        snprintf(name, sizeof name, "the legacy replay");
    }

    if (!livesafety::Allowed(livesafety::Feature::ReplayLoad)) {
        SetStatus("load refused: %s. A legacy replay is converted only to be played, and playback "
                  "needs a contained offline session",
                  livesafety::VerdictReason(livesafety::Current()));
        return false;
    }
    if (g_import.Busy()) {
        if (g.importLoadWhenDone && _wcsicmp(g_import.SourcePath().c_str(), path) == 0) {
            SetStatus("%s is already converting: %u%%", g.importName, g_import.Permille() / 10);
        } else {
            SetStatus("load refused: %s is still converting; cancel it or let it finish first",
                      g.importName);
        }
        MakeVisible();
        return false;
    }
    uint64_t size = 0;
    uint64_t mtime = 0;
    if (!legacy::StatFile(path, &size, &mtime)) {
        SetStatus("load refused: %s could not be read (error %lu)", name, ::GetLastError());
        return false;
    }
    wchar_t cachePath[kPathMax];
    if (!legacy::CachePathFor(g.replayDir, path, size, mtime, cachePath, kPathMax)) {
        SetStatus("load refused: the converted copy of %s would need a path longer than %zu "
                  "characters under %s",
                  name, kPathMax, g.replayDirNarrow);
        return false;
    }
    const char* why = "";
    if (!g_import.Start(path, cachePath, Converter(), &why)) {
        SetStatus("load refused: %s", why);
        return false;
    }
    snprintf(g.importName, sizeof g.importName, "%s", name);
    g.importRunning = true;
    g.importLoadWhenDone = true;
    g.importPercentShown = 0;
    SetStatus("converting %s: 0%%", g.importName);
    log::Direct(LogLevel::Info, kReplayModuleName, "import: %ls -> %ls (converter version %u)",
                path, cachePath, import::kConverterVersion);
    // The browser is where the progress and the Cancel button are drawn.
    if (g.browserOpen) {
        MakeVisible();
    } else {
        OpenBrowser();
    }
    return false;
}

// Tick only. Polls one atomic while the worker runs; collects it and loads the cache when it ends.
void ServiceImport() {
    if (g.importCancelRequested) {
        g.importCancelRequested = false;
        if (g_import.Busy() && g.importLoadWhenDone) {
            g_import.Cancel();
            g.importLoadWhenDone = false;
            SetStatus("cancelling the conversion of %s...", g.importName);
        }
    }
    if (!g_import.Busy()) {
        g.importRunning = false;
        return;
    }
    legacy::Outcome outcome;
    if (!g_import.Collect(&outcome)) {
        const uint32_t percent = g_import.Permille() / 10;
        if (g.importLoadWhenDone && percent != g.importPercentShown) {
            g.importPercentShown = percent;
            SetStatus("converting %s: %u%%", g.importName, percent);
        }
        return;
    }
    g.importRunning = false;
    if (!outcome.ok) {
        if (outcome.cancelled) {
            SetStatus("the conversion of %s was cancelled; nothing was kept", g.importName);
        } else {
            SetStatus("could not convert %s: %s", g.importName, outcome.message);
            MakeVisible();
        }
        return;
    }
    if (!g.importLoadWhenDone) {
        SetStatus("%s finished converting as it was cancelled; the converted copy is kept for the "
                  "next time it is opened",
                  g.importName);
        return;
    }
    g.importLoadWhenDone = false;
    const std::wstring cachePath = g_import.CachePath();
    snprintf(g.loadNameOverride, sizeof g.loadNameOverride, "%s", g.importName);
    LoadPath(cachePath.c_str());
    g.loadNameOverride[0] = '\0';
}

// Unload, or a load of something else: the owner has moved on, so the conversion stops and its
// result, if it still arrives, is not loaded over his choice.
void AbandonImport(const char* because) {
    if (!g_import.Busy() || !g.importLoadWhenDone) return;
    g_import.Cancel();
    g.importLoadWhenDone = false;
    log::Direct(LogLevel::Info, kReplayModuleName, "import: the conversion of %s was cancelled: %s",
                g.importName, because);
}

}  // namespace

namespace panel {

void Init(ReplayServices* services, DemoIndex* index) {
    g.services = services;
    g.index = index;
    snprintf(g.modeLabel, sizeof g.modeLabel, "%s", "");
    // * THE BROWSER SCANS WHAT THE CAPTURE WRITER WRITES, and the rule now lives in exactly one
    // place (replay::ResolveCaptureDir). It used to be the state directory + "\replays" while
    // src/capture wrote captures to Documents\EchoReplay, which is why the panel in
    // L5-RUN-7-run2.png reports "No .echodemo files in:" a folder no capture has ever landed in.
    const char* const configuredDir =
        services != nullptr && services->config != nullptr
            ? services->config->GetString(kCaptureDirKey, nullptr)
            : nullptr;
    if (!ResolveCaptureDir(configuredDir, g.replayDir, kPathMax)) {
        log::Direct(LogLevel::Error, kReplayModuleName,
                    "the capture directory could not be resolved: the browser has nowhere to scan");
    }
    NoteReplayDir();
    if (index != nullptr) {
        index->SetImportDirs(ResolveImportDirs(
            services != nullptr && services->config != nullptr
                ? services->config->GetString(kImportDirsKey, nullptr)
                : nullptr));
    }
    const double cacheMb = services != nullptr && services->config != nullptr
                               ? services->config->GetDouble(legacy::kImportCacheMbKey,
                                                             legacy::kDefaultImportCacheMb)
                               : legacy::kDefaultImportCacheMb;
    if (!g_import.Busy()) g_import.SetCacheCapBytes(cacheMb > 0 ? uint64_t(cacheMb * 1048576.0) : 0);
    g.enabled = services != nullptr && services->config != nullptr
                    ? services->config->GetBool("ui.panel", true)
                    : true;
    g.gametypeResolve = true;
    const char* fallbackName = "echo_arena";
    if (services != nullptr && services->config != nullptr) {
        g.gametypeResolve = services->config->GetBool("session.gametype_override", true);
        fallbackName = services->config->GetString("session.gametype_override_name", "echo_arena");
    }
    g.gametypeFallback =
        fallbackName != nullptr && fallbackName[0] != '\0' ? gametype::SymbolHash(fallbackName) : 0;
    // Hidden until something asks for it: F9, a load, or the browser. See uiapi.h.
    g.visible = false;
    g.browserOpen = false;
    g.cursorHeld = false;
    // Nothing has been measured yet, whatever a previous Init left behind.
    g.framePeriodEma = 0.0;
    g.ceiling = 0.0f;
    g.periodOffered = 0;
    g.periodSamples = 0;
    g.periodZeroDelta = 0;
    g.maxElapsedLive = false;
    StartWorker();
    SetStatus("panel initialised; press %s to show it. %s", kHotkeyName, gui::Diagnostic());
    log::Direct(LogLevel::Info, kReplayModuleName,
                "panel %s and hidden; press %s in the game window to show or hide it",
                g.enabled ? "enabled" : "DISABLED by ui.panel", kHotkeyName);
}

// The live key read, and the only Win32 input call in this packet. Foreground is tested against our
// own PROCESS rather than a window handle: the panel and the console window are both ours, and a
// key pressed while another application has focus must never reach either.

void SetCameraForTest(IReplayCameraControl* cam) {
    g_cameraOverride = cam;
    g.camSelector = 1;
}

void Sample(uint64_t frameDeltaUs) {
    // ARCH-20 S6.3(a)'s geometry, taken from the owner tick because this runs BEFORE the frame:
    // ImGui clears ImDrawData at NewFrame and fills it at EndFrame, so the tick and the in-frame
    // draw site straddle the only window in which the struct holds a number. It latches whichever
    // of the two lands inside it and refuses rather than publishing the other one's zero.
    inframe::SampleGuiDrawData();

    // ★ THE ONLY WAY IN AND OUT OF THE PANEL FROM INSIDE THE GAME. Edge-triggered: holding the key
    // toggles once, not once per frame.

    // ★ THE CURSOR, WHICH DECIDES WHETHER ANY OF THIS IS DRIVABLE. Every interaction on this panel
    // is a gui::Button click -- the facade has no slider, no drag and no keyboard path -- so a
    // visible panel with the engine's cursor disabled renders perfectly and does nothing.
    //
    // Edge-triggered, so the engine's own cursor bind (action 0x0E) is not fought every frame, and
    // symmetric: we take the cursor when the panel goes up and give it back when it comes down. The
    // one case that costs something is an owner who had the cursor on for the engine's own debug
    // window and then dismisses ours -- he gets it back with his own key. We cannot read the
    // engine's cursor state through the facade, so this reports what WE did and claims nothing more.
    const bool wantCursor = CursorWanted();
    if (wantCursor != g.cursorHeld && gui::Have(gui::Entry::SetCursorEnabled)) {
        gui::SetCursorEnabled(wantCursor ? 1u : 0u);
        g.cursorHeld = wantCursor;
    }

    // ★ A REQUESTED ENTRY IS NOT AN ENTRY. RequestEntry performs on the tick, so the frames between
    // the load and the level change are frames where the capture plays over the wrong world. This
    // watches for the world to become the capture's and says so when it does not -- the whole point
    // of the check is that "the world is wrong" must be readable rather than invisible.
    if (g.entryPending) {
        ISessionEntry* s = Session();
        if (s == nullptr) {
            g.entryPending = false;
            g.entryDeferred = false;
        } else if (s->Matches(g.entryLevelId, g.entryGametypeId)) {
            g.entryPending = false;
            g.entryVerdict = EntryVerdict::Matches;
            SetEntryReason("the session entered this capture's level and gametype");
        } else {
            // ★ A DEFERRED REQUEST IS RE-OFFERED, NOT MERELY WAITED ON (P10-C s4). NotInLevelYet is
            // transient by definition, so waiting for Matches() without asking again would wait
            // forever on a session that is ready and was never asked a second time.
            if (g.entryDeferred) {
                if (s->RequestEntry(g.entryLevelId, g.entryGametypeId)) {
                    g.entryDeferred = false;
                    g.entryVerdict = EntryVerdict::Requested;
                    SetEntryReason("entry into this capture's level was deferred and has now been "
                                   "REQUESTED; it performs on the tick");
                } else if (s->LastRefusal() != EntryRefusal::NotInLevelYet) {
                    const char* failure = s->FailureReason();
                    g.entryDeferred = false;
                    g.entryPending = false;
                    g.entryVerdict = EntryVerdict::RequestFailed;
                    SetEntryReason("the deferred entry was re-offered and the session refused: %s",
                                   failure != nullptr && failure[0] != '\0'
                                       ? failure
                                       : "no reason was given");
                }
            }
            g.entryWaitUs += frameDeltaUs;
            if (g.entryWaitUs > kEntryWaitLimitUs && !g.entryTimedOut) {
                g.entryTimedOut = true;
                SetEntryReason("the session was asked to enter this capture's level %llu s ago and "
                               "has not: the world on screen is NOT this capture's world",
                               static_cast<unsigned long long>(g.entryWaitUs / 1000000));
                log::Direct(LogLevel::Warn, kReplayModuleName, "%s", g.entryReason);
            }
        }
    }

    // The browser's file I/O, serviced here rather than in Draw, and never a whole folder in one
    // frame. A rescan is ONE directory enumeration -- names and mtimes out of the directory index,
    // no file opened -- and everything per file, the 176-byte header read as much as the manifest,
    // is amortised a few rows per tick. Reading every header inside the scan used to be 200
    // CreateFileW calls in one frame, which on a cold disk is the second of frozen game this
    // split exists to prevent.
    if (g.scanRequested) {
        g.scanRequested = false;
        RequestScan();
    }
    TakeSnapshot();
    if (g.browserOpen) UpdateRowVerdicts();
    // The click Draw recorded. Serviced here, one tick later, because this is where file I/O is
    // allowed to happen and the frame the owner clicked in is not.
    if (g.loadRequested) {
        g.loadRequested = false;
        LoadPath(g.loadRequestPath);
    }
    ServiceImport();
    // The greyed row somebody clicked. Its reason was cut to the width of the row it rode on, so
    // the whole of it goes where there is room for it rather than being widened on every frame.
    if (g.refusedClick >= 0) {
        const size_t row = static_cast<size_t>(g.refusedClick);
        g.refusedClick = -1;
        if (row < kBrowserRows) {
            SetStatus("that capture cannot be loaded in this session: %s",
                      g.rowWhy[row] != nullptr ? g.rowWhy[row] : "no reason was recorded");
        }
    }

    IReplayTransport* t = Transport();
    g.haveTransport = t != nullptr;
    if (t != nullptr) {
        g.transportState = t->State();
        g.rate = t->Rate();
        g.playheadUs = t->PlayheadUs();
        g.durationUs = static_cast<int64_t>(t->DurationUs());
        const char* label = t->ActiveModeLabel();
        snprintf(g.modeLabel, sizeof g.modeLabel, "%s", label != nullptr ? label : "");
    }

    // frameDeltaUs is one frame of SCALED sim time (B.3), so the raw frame period is recovered by
    // dividing the rate back out: with no transport nothing scales it; playing at rate r it is r
    // times the raw period; paused, stepping, scrubbing or seeking it is not a frame's worth of
    // anything. A delta that reached CLOCK.MAX_ELAPSED is the engine's clamp rather than a
    // measurement -- dividing it by the rate would put the ceiling exactly at whatever rate was
    // asked for. The old gate accepted 1x play alone, so a session that went from paused straight
    // to 2x never measured anything and the ceiling stayed unknown for as long as it ran.
    //
    // An exponential mean rather than a minimum, because a mean is pulled UP by hitches and so
    // biases the ceiling DOWN -- the safe direction, since the cost of a ceiling that is slightly
    // too low is one unavailable button and the cost of one that is too high is a fast-forward
    // that silently does not accelerate.
    //
    // ★ THE UNSCALED CASE IS "NOTHING IS LOADED", NOT "THERE IS NO TRANSPORT". Transport() hands
    // back the services' object, which is published from module init onward, so `t == nullptr` is
    // true in a unit test and never once in the game -- the only live path left was the Play one,
    // and a session that boots, sits in a level and does not play measures nothing at all. That is
    // L3-RUN-2 F3: RateCeiling() 0 for an entire launch, which made four transport rows unaskable.
    // Our clock module engages on load and releases on unload, so while the transport is NotLoaded
    // the engine's clock is the engine's own and its delta IS the raw frame period.
    SampleMaxElapsed();
    const bool engineClockUnscaled =
        t == nullptr || g.transportState == TransportState::NotLoaded;
    const bool measuring =
        engineClockUnscaled || (g.transportState == TransportState::Play && g.rate != 0.0f);
    double rawPeriodUs = 0.0;
    if (measuring && frameDeltaUs < g.maxElapsedUs) {
        const double scale = engineClockUnscaled ? 1.0 : (g.rate < 0.0f ? -g.rate : g.rate);
        rawPeriodUs = static_cast<double>(frameDeltaUs) / scale;
    }
    if (measuring) ++g.periodOffered;
    if (rawPeriodUs > 0.0 && rawPeriodUs < 200000.0) {
        ++g.periodSamples;
        g.framePeriodEma =
            g.framePeriodEma == 0.0 ? rawPeriodUs : g.framePeriodEma * 0.95 + rawPeriodUs * 0.05;
    } else if (measuring && frameDeltaUs == 0) {
        // The state that measures was reached and the engine's sim time did not move. This is what
        // an unresolved CLOCK.GLOBAL_PTR looks like from up here, and it is the difference between
        // "we have not been in a measuring state yet" and "we are measuring nothing".
        ++g.periodZeroDelta;
    }
    g.ceiling = RateCeiling(g.maxElapsedUs, static_cast<uint64_t>(g.framePeriodEma + 0.5));

    g.degradations = static_cast<uint32_t>(gui::Entry::Count) - gui::BoundCount();
    if (!g.markersKnown && g.haveTransport && g.durationUs > 0) ++g.degradations;

    // Built LAST, so the block the next frame draws carries everything this tick decided --
    // including the degradation count StatusLine() reports.
    if (g.browserOpen) BuildHeaderBlock();
}

inframe::DrawOutcome Draw() {
    if (!PanelIsDrawing()) return inframe::DrawOutcome::NotShowing;

    // ★ UI-2 P2: THE PANEL IS DEMOTED, NOT DELETED. `ui.debug_panel` is off by default and the bar
    // is what the owner sees; every line, strip, row and sentence below is untouched and comes back
    // whole when the key is on. The browser is the exception, and it has to be: it is the file
    // picker the bar's own menu opens, so an open browser draws this window whatever the key says.
    // The cursor claim and StatusLine are deliberately ABOVE this gate -- they are what makes the
    // bar clickable, and keying them on the debug panel would leave the owner a bar he cannot
    // press.
    if (!bar::DebugPanelOn() && !g.browserOpen) return inframe::DrawOutcome::NotShowing;

    // ** THE THREAD NO FILE MAY BE OPENED ON, NAMED RATHER THAN ASSUMED. Every DemoIndex entry
    // point that opens a file reports the thread it ran on, and the launch record publishes how
    // many of them landed here. It is 0 in this build; a regression makes it a number.
    fileio::NoteDrawThread();
    const uint64_t drawStartNanos = NowNanos();

    if (!g.placed) {
        gui::SetNextWindowPos(gui::Vec2{40.0f, 40.0f}, 0);
        gui::SetNextWindowSize(gui::Vec2{kPanelW, kPanelH}, 0);
        g.placed = true;   // set once, so the window stays draggable and resizable afterwards
    }

    bool open = true;
    const bool shown = gui::Begin("Open a capture##replaybrowser", &open, 0);
    // ★ End is called whether or not Begin returned true. The facade runs a Begin/End balance
    // checker that logs "Missing call to ... before ..." into the game's own log, once per frame,
    // for as long as we get this wrong.
    if (shown) {
        if (g.browserOpen) {
            DrawBrowserView();
        } else {
            gui::Text(g.loadedName[0] != '\0' ? g.loadedName : "No capture loaded.");
            gui::SameLine(0.0f, 8.0f);
            if (gui::Button("Browse##openbrowse", gui::Vec2{80.0f, 0.0f})) OpenBrowser();
            gui::Separator();
            DrawTransportView();
            gui::Separator();
            gui::Text(StatusLine());
        }
    }
    gui::End();

    if (!open) g.visible = false;
    if (!shown) {
        // ** WHAT THE FALSE WAS, read here because this is the only place that can see the facade's
        // binding state at the instant Begin answered (L5-RUN-5 G2). Only what is actually readable
        // is named; the residual keeps its own word rather than borrowing a cause's.
        const inframe::BeginRefusal why =
            !gui::Have(gui::Entry::Begin)
                ? inframe::BeginRefusal::BeginUnbound
                : (!gui::Have(gui::Entry::SetNextWindowPos) ||
                   !gui::Have(gui::Entry::SetNextWindowSize))
                      ? inframe::BeginRefusal::NoPlacement
                      : inframe::BeginRefusal::BoundNoFrame;
        inframe::NoteBeginRefusal(why, gui::BoundCount(),
                                  static_cast<uint32_t>(gui::Entry::Count));
    }
    // What the facade cost, which after this rewrite is very nearly all of drawNanosMax: the panel
    // formats nothing and opens nothing here, so a tail that is NOT in this number is a tail in
    // our own code and a tail that IS in it is the engine's ImGui.
    inframe::NoteImguiNanos(NowNanos() - drawStartNanos);
    // Begin's own answer, forwarded rather than discarded (AUDIT_F3J F3J-4). A false `shown` means
    // the facade produced no state for this window, whatever the frame byte said a moment earlier.
    return shown ? inframe::DrawOutcome::Drawn : inframe::DrawOutcome::FacadeRefused;
}

void Shutdown() {
    // Give the cursor back before we stop drawing anything to click on. Leaving it enabled would
    // hand the owner a free mouse pointer over a game with no window under it.
    if (g.cursorHeld && gui::Have(gui::Entry::SetCursorEnabled)) {
        gui::SetCursorEnabled(0u);
        g.cursorHeld = false;
    }
    // Before the index pointer is dropped: the worker holds it for the length of a build, and a
    // Shutdown that raced one would pull the folder out from under a scan in progress.
    StopWorker(INFINITE);
    g_import.Stop(INFINITE);
    g.importRunning = false;
    g.importLoadWhenDone = false;
    g.importCancelRequested = false;
    delete g.live;
    g.live = nullptr;
    g.liveSeq = 0;
    g.services = nullptr;
    g.index = nullptr;
    g.visible = false;
}

void StopWorkerForExit(uint32_t budgetMs) {
    const bool stopped = StopWorker(budgetMs);
    log::Direct(LogLevel::Info, kReplayModuleName, "pre-exit: browser worker %s",
                stopped ? "stopped" : "still scanning after its budget; left to process exit");
    if (g_import.Busy()) {
        const bool importStopped = g_import.Stop(budgetMs);
        log::Direct(LogLevel::Info, kReplayModuleName, "pre-exit: conversion worker %s",
                    importStopped ? "cancelled and stopped"
                                  : "still running after its budget; left to process exit");
    }
}

void SetConverterForTest(legacy::ConvertFn convert) { g_convertOverride = convert; }
bool ImportBusyForTest() { return g_import.Busy(); }
const char* StatusForTest() { return g.status; }

bool WaitForImportForTest(uint32_t timeoutMs) {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    while (g_import.Busy() && !g_import.Finished()) {
        if (::GetTickCount64() > deadline) return false;
        ::Sleep(1);
    }
    return true;
}

bool BrowserWorkerRunningForTest() {
    std::lock_guard<std::mutex> lk(g_worker.m);
    return g_worker.thread != nullptr;
}

void SetPlayableOnlyForTest(bool on) { g.playableOnly = on; }
bool PlayableOnlyForTest() { return g.playableOnly; }
const char* HeaderLineForTest() { return g.headerFirst; }

bool WaitForBrowserForTest(uint32_t timeoutMs) {
    uint64_t want = 0;
    {
        std::lock_guard<std::mutex> lk(g_worker.m);
        want = g_worker.requested;
    }
    const uint64_t deadline = NowNanos() + static_cast<uint64_t>(timeoutMs) * 1000000ull;
    while (g_worker.published.load(std::memory_order_acquire) < want) {
        if (NowNanos() > deadline) return false;
        ::Sleep(1);
    }
    return true;
}

}  // namespace panel

// ---- uiapi.h, implemented here because it is all state this file owns ------------------------

void ShowPanel(bool visible) {
    if (!g.enabled && visible) {
        // The WINDOW stays off -- that is what ui.panel means -- but the cursor claim below is the
        // player bar's and must not be refused with it.
        SetStatus("the debug panel is switched off by ui.panel; the cursor is still claimed");
    }
    if (visible) {
        MakeVisible();
    } else {
        g.visible = false;
    }
}

bool PanelVisible() { return g.visible; }
void Toggle() { ShowPanel(!g.visible); }
bool PanelEnabled() { return g.enabled; }
const char* PanelHotkeyName() { return kHotkeyName; }
bool BrowserOpen() { return g.browserOpen; }

const char* NotesStateName(NotesState s) {
    switch (s) {
        case NotesState::NotSupplied: return "not supplied";
        case NotesState::Absent: return "absent";
        case NotesState::Ok: return "ok";
        case NotesState::Damaged: return "damaged";
    }
    return "?";
}

void SetBookmarks(NotesState state, const BookmarkView* items, size_t count, int64_t resumeUs) {
    g.notesState = state;
    g.resumeUs = resumeUs;
    g.bookmarkTotal = items != nullptr ? count : 0;
    g.bookmarkCount = 0;
    if (state != NotesState::Ok || items == nullptr) {
        // A state that carries no table carries no table: leaving the old rows up would draw the
        // previous capture's marks over this one.
        g.bookmarkTotal = 0;
        return;
    }
    const size_t n = count < State::kMaxBookmarks ? count : State::kMaxBookmarks;
    for (size_t i = 0; i < n; ++i) {
        g.bookmarks[i].tUs = items[i].tUs;
        // 1-based, so the panel's "B3" is the console's `goto 3`. The two surfaces must not number
        // the same list differently.
        g.bookmarks[i].index = static_cast<uint16_t>(i + 1);
        snprintf(g.bookmarkLabels[i], sizeof g.bookmarkLabels[i], "%s",
                 items[i].label != nullptr ? items[i].label : "");
    }
    g.bookmarkCount = n;
}

NotesState BookmarksState() { return g.notesState; }
size_t BookmarkCount() { return g.bookmarkTotal; }

const char* EntryVerdictName(EntryVerdict v) {
    switch (v) {
        case EntryVerdict::NotChecked: return "not checked";
        case EntryVerdict::Matches: return "matches";
        case EntryVerdict::MustEnter: return "must enter";
        case EntryVerdict::Requested: return "entry requested";
        case EntryVerdict::Refused: return "refused";
        case EntryVerdict::RequestFailed: return "entry request failed";
        case EntryVerdict::EntryDeferred: return "entry deferred";
    }
    return "?";
}

EntryVerdict LastEntryVerdict() { return g.entryVerdict; }

const char* LastEntryReason() {
    return g.entryReason[0] != '\0' ? g.entryReason : "no capture has been loaded in this session";
}

void OpenBrowser() {
    g.browserOpen = true;
    g.browserPage = 0;   // opening the browser shows the newest captures
    MakeVisible();
    g.scanRequested = true;
}

// LoadMostRecent needs the listing to have HAPPENED, not to have been requested: it answers its
// caller in the same call, so it cannot wait a frame. What it takes synchronously is the directory
// enumeration alone -- the per-file reads stay on the tick, and LoadMostRecent opens only the rows
// it has to try.
void ScanNow() {
    if (g.index == nullptr) return;
    g.scanRequested = false;
    IndexLock lock(g_indexMutex);
    if (!g.index->Scan(g.replayDir)) {
        ReportScanFailure(g.index->LastScanError());
    }
}

bool LoadPath(const wchar_t* path) {
    if (path == nullptr || path[0] == L'\0') {
        SetStatus("load refused: no path given");
        return false;
    }
    if (legacy::IsLegacySource(path)) return BeginImport(path);
    AbandonImport("another capture was opened instead");
    // A load is a user action on the owner thread, and it reads files through the same index the
    // browser worker is listing with. This is the only thing in this packet that ever waits on it.
    IndexLock lock(g_indexMutex);
    IReplayTransport* t = Transport();
    if (t == nullptr) {
        SetStatus("load refused: no replay transport has registered yet (W2-B publishes it)");
        return false;
    }

    // ★ NOTHING ABOUT THE CURRENTLY LOADED CAPTURE IS TOUCHED UNTIL THE LOAD IS COMMITTED. The
    // manifest is read first -- the entry decision needs its level id, and the timeline wants its
    // markers on the first frame rather than the second -- but a refusal below must leave the panel
    // describing the capture that is still playing, not the one it declined to open.
    size_t row = g.index != nullptr ? g.index->AddOne(path) : 0;
    const Manifest* m = nullptr;
    if (g.index != nullptr) {
        // AddOne rather than a search of the last Scan. The -replayviewer path is routinely
        // outside the replay folder, and a search would silently come up empty for the single most
        // common way a capture is opened -- costing that launch its round markers.
        if (row < g.index->Count() && g.index->LoadManifest(row)) m = g.index->At(row).manifest;
    }

    // ★★★ THE ENTRY CHECK, WHICH IS WHY THIS FUNCTION EXISTS RATHER THAN A BARE t->Load(). All
    // three of the owner's entry paths -- the browser row, `load <path>` and `-replayviewer` --
    // funnel through here, so this is the one place the check can be made once and not be bypassed.
    gametype::Resolved entryGametype{0, gametype::Rule::Unresolved};
    if (m != nullptr) {
        entryGametype = ResolveEntryGametype(path, m->gametypeId);
        const char* name = gametype::NameOf(entryGametype.id);
        log::Direct(LogLevel::Info, kReplayModuleName,
                    "entry gametype: rule %s, recorded 0x%016llX -> entering 0x%016llX (%s)%s",
                    gametype::RuleName(entryGametype.rule),
                    static_cast<unsigned long long>(m->gametypeId),
                    static_cast<unsigned long long>(entryGametype.id),
                    name != nullptr ? name : "not a known gametype name",
                    g.gametypeResolve ? "" : " - session.gametype_override is off");
    }
    const char* why = "";
    const EntryVerdict decision =
        DecideEntryIds(m != nullptr, m != nullptr ? m->levelId : 0, entryGametype.id, &why);
    g.entryPending = false;
    g.entryDeferred = false;
    g.entryTimedOut = false;
    g.entryWaitUs = 0;

    if (decision == EntryVerdict::Refused) {
        g.entryVerdict = EntryVerdict::Refused;
        SetEntryReason("%s", why);
        SetStatus("load refused: this capture's level is not one this session can reach - %s", why);
        log::Direct(LogLevel::Warn, kReplayModuleName, "load refused for %ls: %s", path, why);
        MakeVisible();   // the refusal is worth a window; a silent decline is the same as a bug
        return false;
    }

    if (decision == EntryVerdict::MustEnter) {
        ISessionEntry* s = Session();
        const bool accepted = s->RequestEntry(m->levelId, entryGametype.id);
        // ★ THE THIRD ANSWER, READ RATHER THAN INFERRED (P10-C s4). A false return with
        // NotInLevelYet is TRANSIENT -- the session is mid-build and will answer differently on a
        // later tick -- so it must never become RequestFailed, which is the settled word. Anything
        // else (CannotEnter, NotAsked from an implementation that predates the question) is
        // terminal and keeps the behaviour every caller already had.
        const bool deferred = !accepted && s->LastRefusal() == EntryRefusal::NotInLevelYet;
        if (!accepted && !deferred) {
            const char* failure = s->FailureReason();
            g.entryVerdict = EntryVerdict::RequestFailed;
            SetEntryReason("the session was asked to enter this capture's level and refused: %s",
                           failure != nullptr && failure[0] != '\0' ? failure
                                                                    : "no reason was given");
            SetStatus("load refused: %s", g.entryReason);
            log::Direct(LogLevel::Warn, kReplayModuleName, "entry request refused for %ls: %s", path,
                        g.entryReason);
            MakeVisible();
            return false;
        }
        // Accepted, not done: RequestEntry tears the session down and rebuilds it on the tick, so
        // the capture opens now and Sample() watches for the world to actually become this one.
        // A DEFERRED request opens the capture too -- the file is kept and nothing is unloaded --
        // and Sample() re-offers it every tick until the session takes it or settles on a no.
        g.entryVerdict = deferred ? EntryVerdict::EntryDeferred : EntryVerdict::Requested;
        g.entryPending = true;
        g.entryDeferred = deferred;
        g.entryLevelId = m->levelId;
        g.entryGametypeId = entryGametype.id;
        if (deferred) {
            SetEntryReason("the session is not in a level yet, so entry into this capture's level "
                           "is DEFERRED and re-offered on every tick; until it lands the world on "
                           "screen is not this capture's");
        } else {
            SetEntryReason("entry into this capture's level has been REQUESTED and performs on the "
                           "tick; until it lands the world on screen is not this capture's");
        }
    } else {
        g.entryVerdict = decision;
        SetEntryReason("%s", why);
    }

    if (!t->Load(path)) {
        SetStatus("the transport refused this capture: %ls", path);
        // ★ Everything derived from a capture is CLEARED, not kept. B.8 does not say whether a
        // refused Load leaves the old capture playing or tears it down first, and most
        // implementations unload before they open -- so after a refusal what is loaded is genuinely
        // unknown. LoadedPath() therefore answers null, and W2-G's `mark` refuses. Losing a bookmark
        // to a spurious refusal is an annoyance; writing one beside the wrong capture is the failure
        // that accessor exists to prevent.
        g.markersKnown = false;
        g.markerCount = 0;
        g.loadedName[0] = '\0';
        g.loadedFlags = 0;
        g.loadedFlagsKnown = false;
        g.loadedBuildMatch = BuildMatch::NotChecked;
        g.loadedPath[0] = L'\0';
        // Voice goes with everything else derived from a capture: after a refused Load we do not
        // know what the transport still holds, and voice from a file that may no longer be open is
        // the same stale state the markers are dropped for.
        audio::voip::SetVoipDemoPath(nullptr);
        SetBookmarks(NotesState::NotSupplied, nullptr, 0, 0);
        return false;
    }

    // ---- committed from here down; the capture on screen is this one --------------------------
    g.markersKnown = false;
    g.markerCount = 0;
    g.loadedName[0] = '\0';
    g.loadedFlags = 0;
    g.loadedFlagsKnown = false;
    g.loadedBuildMatch = BuildMatch::NotChecked;
    if (g.index != nullptr && row < g.index->Count()) {
        if (m != nullptr) {
            const size_t cap = sizeof g.markers / sizeof g.markers[0];
            const size_t n = m->roundCount < cap ? m->roundCount : cap;
            for (size_t r = 0; r < n; ++r) {
                g.markers[r].tUs =
                    static_cast<int64_t>(m->rounds[r].tUs) - static_cast<int64_t>(m->t0Us);
                g.markers[r].index = m->rounds[r].index;
                g.markers[r].kind = m->rounds[r].kind;
            }
            g.markerCount = n;
            g.markersKnown = true;
        }
        snprintf(g.loadedName, sizeof g.loadedName, "%s",
                 g.loadNameOverride[0] != '\0' ? g.loadNameOverride : g.index->DisplayName(row));
        g.loadedFlags = g.index->At(row).flags;
        g.loadedBuildMatch = g.index->BuildMatchOf(row);
        // Known only when the header actually parsed. A file we could not read has no flags, and
        // zero flags would claim it has VOIP, as-seen and oracle all absent -- three statements we
        // have not earned.
        g.loadedFlagsKnown = g.index->Fault(row) != RowFault::Unreadable &&
                             g.index->Fault(row) != RowFault::BadMagic &&
                             g.index->Fault(row) != RowFault::ShortFile;
    }
    // Recorded from the argument, not from the index row: a capture the browser could not read the
    // manifest for is still loaded and still bookmarkable, and its path is still exactly this one.
    _snwprintf_s(g.loadedPath, kPathMax, _TRUNCATE, L"%s", path);
    // ★ VOICE, HANDED OVER FROM THE ONE PLACE THAT KNOWS A FILE WAS OPENED. W3-C's player waits on
    // this call and had none, so replayed voice never played at all. It sits on the SUCCESS arm and
    // after the entry decision on purpose: a file the transport refused, or one whose level this
    // session cannot reach, must not be handed to voice either.
    audio::voip::SetVoipDemoPath(path);
    // A NEW capture: whatever bookmarks the panel held describe the old one. Back to NotSupplied
    // until a control surface pushes this file's sidecar -- drawing the previous capture's marks
    // over this one is exactly the wrong-file class LoadedPath() exists to prevent.
    SetBookmarks(NotesState::NotSupplied, nullptr, 0, 0);
    g.browserOpen = false;
    g.exactSeconds = 0.0f;
    MakeVisible();
    if (g.entryVerdict == EntryVerdict::Matches) {
        SetStatus("loaded %s", g.loadedName[0] != '\0' ? g.loadedName : "capture");
    } else {
        // The load succeeded and the WORLD may not be the capture's. That belongs in the one line
        // every surface prints, not only in a log nobody has open.
        SetStatus("loaded %s - %s", g.loadedName[0] != '\0' ? g.loadedName : "capture",
                  g.entryReason);
    }
    if (m != nullptr && m->roundCount > sizeof g.markers / sizeof g.markers[0]) {
        SetStatus("this capture has %u round markers; the timeline shows the first %zu",
                  m->roundCount, sizeof g.markers / sizeof g.markers[0]);
    }
    log::Direct(LogLevel::Info, kReplayModuleName, "loaded %ls (%zu round marker(s)); entry %s: %s",
                path, g.markerCount, EntryVerdictName(g.entryVerdict), g.entryReason);
    return true;
}

bool ImportInProgress() { return g_import.Busy() && g.importLoadWhenDone; }

bool LoadMostRecent() {
    IndexLock lock(g_indexMutex);
    // The browser opens whatever happens next. A folder with nothing loadable in it must still put
    // the user in front of the folder rather than in front of an empty panel with no explanation.
    OpenBrowser();
    ScanNow();
    if (g.index == nullptr || g.index->Count() == 0) {
        SetStatus("no captures in %ls", g.replayDir);
        return false;
    }
    // Scan sorted newest first, so the first row that reads is the most recent. Each row's header
    // is read here, on demand: the usual cost is one file, and only a folder of nothing but broken
    // captures reads them all.
    for (size_t i = 0; i < g.index->Count(); ++i) {
        // The most recent CAPTURE: a legacy file would start a conversion rather than load, and the
        // next row tried would then cancel it.
        if (g.index->IsLegacy(i)) continue;
        g.index->ReadRow(i);
        if (g.index->Fault(i) == RowFault::Unreadable ||
            g.index->Fault(i) == RowFault::BadMagic ||
            g.index->Fault(i) == RowFault::ShortFile) {
            continue;
        }
        if (LoadPath(g.index->Path(i))) return true;
    }
    SetStatus("%zu capture(s) in %ls, none of which could be loaded", g.index->Count(),
              g.replayDir);
    return false;
}

void Unload() {
    AbandonImport("unload");
    IReplayTransport* t = Transport();
    if (t != nullptr) t->Unload();
    // The null arm of the same seam IS the unload: it clears the stored path, drops the pending
    // flag and calls the player's own Unload. Voice outliving the capture it came from would be
    // the same class of stale state the marker rows are cleared for below.
    audio::voip::SetVoipDemoPath(nullptr);
    // Everything derived from the capture goes with it. A stale marker row over a capture that is
    // no longer loaded is a timeline claiming a round that is not there.
    g.markerCount = 0;
    g.markersKnown = false;
    g.loadedName[0] = '\0';
    g.loadedPath[0] = L'\0';
    g.loadedFlags = 0;
    g.loadedFlagsKnown = false;
    g.loadedBuildMatch = BuildMatch::NotChecked;
    g.exactSeconds = 0.0f;
    g.playheadUs = 0;
    g.durationUs = 0;
    g.entryVerdict = EntryVerdict::NotChecked;
    g.entryReason[0] = '\0';
    g.entryPending = false;
    g.entryDeferred = false;
    g.entryTimedOut = false;
    g.entryWaitUs = 0;
    // Bookmarks belong to the capture that has just gone. Back to NotSupplied rather than to an
    // empty Ok table: nobody has told us anything about whatever is loaded next, and "none" would
    // be a claim about a file we have not seen.
    SetBookmarks(NotesState::NotSupplied, nullptr, 0, 0);
    SetStatus("unloaded");
}

void SetAsSeen(bool asSeen) {
    g.asSeen = asSeen;
    IReplayTransport* t = Transport();
    if (t != nullptr) t->SetMode(asSeen);
}

bool AsSeen() { return g.asSeen; }

void SetMotionSmoothing(uint8_t mode, uint8_t strength) {
    IReplayTransport* t = Transport();
    if (t != nullptr) t->SetMotionSmoothing(mode, strength);
}

uint16_t MotionSmoothing() {
    IReplayTransport* t = Transport();
    return t != nullptr ? t->MotionSmoothing() : 0;
}

float RateCeiling() { return g.ceiling; }

bool RateIsAchievable(double rate, const char** why) {
    static char buf[224];
    if (why != nullptr) *why = "";
    // One policy, in timeline.cpp, shared with the rate buttons. This function only phrases it.
    switch (CheckRate(rate, g.ceiling)) {
        case RateVerdict::Ok:
            return true;
        case RateVerdict::ZeroRefused:
            snprintf(buf, sizeof buf,
                     "a rate of zero is never written: the clock takes a reciprocal of the scaled "
                     "delta and zero makes it +inf. Use pause, which is the clock's own pause bit.");
            break;
        case RateVerdict::AboveCeiling:
            snprintf(buf, sizeof buf,
                     "%.2fx is above this machine's fast-forward ceiling of %.2fx (%u us / %.0f us "
                     "per frame). Past it the engine stops accelerating without reporting it, so "
                     "the rate is refused rather than accepted and ignored.",
                     ReportedRate(rate < 0.0 ? -rate : rate),
                     ReportedRate(static_cast<double>(g.ceiling)), g.maxElapsedUs,
                     g.framePeriodEma);
            break;
    }
    if (why != nullptr) *why = buf;
    return false;
}

bool SetRate(double rate, const char** why) {
    IReplayTransport* t = Transport();
    if (t == nullptr) {
        if (why != nullptr) *why = "no replay transport has registered yet";
        return false;
    }
    // Before the ceiling, not after: with no capture open, "nothing loaded" is the answer the user
    // needs, and a rate refused for being above a ceiling would send them tuning a number instead.
    if (RefuseWithNothingLoaded(t, why)) return false;
    if (!RateIsAchievable(rate, why)) return false;
    g.rate = static_cast<float>(rate);
    // A speed change is not a play command. Running: keep running at the new rate. Paused: stay
    // paused and take the rate on the next Play. Asked of the transport, not of g.transportState,
    // because the console reaches this without the panel having drawn a frame.
    if (t->State() == TransportState::Play) {
        t->Play(g.rate);
    } else {
        t->SetRate(g.rate);
    }
    return true;
}

bool SeekUs(int64_t tUs) {
    IReplayTransport* t = Transport();
    if (t == nullptr || RefuseWithNothingLoaded(t, nullptr)) return false;
    SeekTo(tUs);
    return true;
}

bool SeekFraction(double zeroToOne) {
    IReplayTransport* t = Transport();
    if (t == nullptr || RefuseWithNothingLoaded(t, nullptr)) return false;
    // A fraction of a span nobody knows is not the start of the file: seeking to 0
    // here would move the playhead where the caller never asked and report it as a success.
    if (!(g.durationUs > 0)) return false;
    if (zeroToOne < 0.0) zeroToOne = 0.0;
    if (zeroToOne > 1.0) zeroToOne = 1.0;
    SeekTo(static_cast<int64_t>(zeroToOne * static_cast<double>(g.durationUs)));
    return true;
}

// ---- what the bar drives (UI-2 P2), through this funnel and not around it ----------------------

bool Play() {
    IReplayTransport* t = Transport();
    if (t == nullptr || RefuseWithNothingLoaded(t, nullptr)) return false;
    // Never a rate of zero: the clock takes a reciprocal of the scaled delta. Pause is the pause
    // bit, and a resume from zero would be indistinguishable from it.
    t->Play(g.rate != 0.0f ? g.rate : 1.0f);
    return true;
}

bool Pause() {
    IReplayTransport* t = Transport();
    if (t == nullptr || RefuseWithNothingLoaded(t, nullptr)) return false;
    t->Pause();
    return true;
}

bool Step(int frames) {
    IReplayTransport* t = Transport();
    if (t == nullptr || RefuseWithNothingLoaded(t, nullptr)) return false;
    t->Step(frames);
    return true;
}

void SetDebugPanel(bool on) {
    bar::SetDebugPanel(on);
    if (on) MakeVisible();
}

bool DebugPanelOn() { return bar::DebugPanelOn(); }

void SetBarShown(bool on) { bar::SetShow(on ? bar::Show::On : bar::Show::Off); }
void ToggleBar() { bar::ToggleShow(); }
bool BarShown() { return bar::Shown(); }
const char* BarShowReason() { return bar::ShowReason(); }

bool TickIntervalUs(uint64_t& outUs) {
    // 0.0 is "never measured", not "zero microseconds": the EMA is fed only from a frame this
    // panel could actually time. Rounded exactly the way the ceiling rounds it above, so the two
    // published numbers cannot disagree about the period they came from.
    if (!(g.framePeriodEma > 0.0)) return false;
    outUs = static_cast<uint64_t>(g.framePeriodEma + 0.5);
    return true;
}

bool RoundsKnown() { return g.markersKnown; }
size_t RoundCount() { return g.markerCount; }

uint16_t RoundNumberAt(size_t i) { return i < g.markerCount ? g.markers[i].index : 0; }
int64_t RoundTimeUs(size_t i) { return i < g.markerCount ? g.markers[i].tUs : 0; }
// The kind, which is what tells a goal from a round boundary. The strip has drawn it as a glyph
// since W2-F PANEL4; the bar draws it as a colour and needs the same number.
uint16_t RoundKindAt(size_t i) { return i < g.markerCount ? g.markers[i].kind : 0; }
int64_t BookmarkTimeUs(size_t i) { return i < g.bookmarkCount ? g.bookmarks[i].tUs : 0; }

bool SeekRoundNumber(int64_t roundNumber) {
    IReplayTransport* t = Transport();
    if (t == nullptr || RefuseWithNothingLoaded(t, nullptr)) return false;
    const size_t i = FindRoundByNumber(g.markers, g.markerCount, roundNumber);
    if (roundNumber < 0 || roundNumber > 0xFFFF) return false;

    // B.8's amendment made SeekRound take a RoundNumber and return bool, so it is now unambiguous
    // and refusable -- prefer it, because the transport may land on the round's keyframe rather
    // than merely on its timestamp.
    if (t->SeekRound(RoundNumber(static_cast<uint16_t>(roundNumber)))) {
        if (i != kNoMarker) g.playheadUs = g.markers[i].tUs;
        return true;
    }

    // ★ Refused. If OUR table has that round, the transport's round table and the manifest the
    // browser read disagree -- that is a real inconsistency and it is reported rather than papered
    // over, while the user still gets where they asked to go by the recorded time.
    if (i == kNoMarker) return false;
    SetStatus("the transport refused round %lld, but this capture's manifest lists it at %lld us; "
              "seeking by the recorded time instead. The two round tables disagree.",
              static_cast<long long>(roundNumber), static_cast<long long>(g.markers[i].tUs));
    return SeekUs(g.markers[i].tUs);
}

size_t WriteDiagnostics(char* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    const csregister::Report r = csregister::Status_();
    char now[32], total[32];
    FormatTime(g.playheadUs, now, sizeof now);
    FormatTime(g.durationUs, total, sizeof total);

    const int n = snprintf(
        out, cap,
        "draw route   : band-3 CS update, orderKey 0x%04X, scheme %u\n"
        "  status     : %s\n"
        "  records    : %llu -> %llu, %u registration(s)\n"
        "  callback   : %llu invocation(s), %llu off the tick thread (tick %lu, last callback %lu)\n"
        "  attributed : %llu to this registration, %llu from a component system we have left\n"
        "  detail     : %s\n"
        "facade       : %u/%u entry points bound\n"
        "  detail     : %s\n"
        "  window gate: %s\n"
        "panel        : %s, %s (%s toggles it)\n"
        "  cursor     : %s\n"
        "bar          : %s\n"
        "transport    : %s, rate %.2fx, %s / %s\n"
        "  engine says: %s\n"
        "  ceiling    : %s\n"
        "capture      : %s\n"
        "  rounds     : %s\n"
        "  world      : %s - %s\n"
        "  bookmarks  : %s, %zu supplied%s\n"
        "browser      : %zu file(s), last scan %llu us, folder %ls\n"
        "degradations : %u\n",
        csregister::kOrderKey, csregister::kSchemeIndex, csregister::StatusName(r.status),
        static_cast<unsigned long long>(r.recordsBefore),
        static_cast<unsigned long long>(r.recordsAfter), r.registrations,
        static_cast<unsigned long long>(r.invocations),
        static_cast<unsigned long long>(r.offThreadInvocations),
        static_cast<unsigned long>(r.mainThreadId),
        static_cast<unsigned long>(r.lastCallbackThreadId),
        static_cast<unsigned long long>(r.registrationInvocations),
        static_cast<unsigned long long>(r.staleInvocations), csregister::Diagnostic(),
        gui::BoundCount(), static_cast<uint32_t>(gui::Entry::Count), gui::Diagnostic(),
        gui::WindowGateOpen() ? "open" : "null (headless; no stock window draws either)",
        PanelEnabled() ? "enabled" : "DISABLED by ui.panel", PanelVisible() ? "visible" : "hidden",
        PanelHotkeyName(),
        !gui::Have(gui::Entry::SetCursorEnabled)
            ? "GUI.SETCURSORENABLED did not bind - the panel cannot enable the cursor, and an "
              "engine cursor that is off makes every button inert"
            : (g.cursorHeld ? "enabled by this panel"
                            : "not touched by this panel (the engine's own state stands)"),
        // The panel's own line keeps the meaning every agent already greps. The bar brought its
        // own rather than redefining one.
        bar::DiagLine(),
        g.haveTransport ? TransportStateName(g.transportState) : "not registered",
        static_cast<double>(g.rate), now, total,
        g.modeLabel[0] != '\0' ? g.modeLabel : "(no label)",
        g.ceiling > 0.0f ? "measured" : CeilingWhy(),
        g.loadedName[0] != '\0' ? g.loadedName : "none loaded",
        g.markersKnown ? "read from the manifest" : "NOT READ - manifest unavailable",
        EntryVerdictName(g.entryVerdict), LastEntryReason(),
        NotesStateName(BookmarksState()), BookmarkCount(),
        g.resumeUs > 0 ? ", resume point recorded" : "",
        IndexCount(),
        static_cast<unsigned long long>(IndexScanMicros()),
        g.replayDir, g.degradations);

    if (n < 0) return 0;
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

const wchar_t* LoadedPath() { return g.loadedPath[0] != L'\0' ? g.loadedPath : nullptr; }

const wchar_t* ReplayDirectory() { return g.replayDir; }

const char* StatusLine() {
    // csregister's diagnosis wins whenever the panel is not actually being driven: "the browser is
    // empty" is a far less useful thing to read than "registered but the callback never fired".
    const csregister::Report r = csregister::Status_();
    if (r.status != csregister::Status::Running) return csregister::Diagnostic();
    if (!gui::Ready()) return gui::Diagnostic();
    if (g.degradations > 0) {
        static char buf[640];
        snprintf(buf, sizeof buf, "%u degradation(s): %s", g.degradations, gui::Diagnostic());
        return buf;
    }
    return g.status[0] != '\0' ? g.status : "ready";
}

uint32_t DegradationCount() { return g.degradations; }

uint32_t NotLoadedRefusals() { return g.notLoadedRefusals; }

}  // namespace ui
}  // namespace replay
