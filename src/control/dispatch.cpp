#include "control/dispatch.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include "audio/akbind.h"
#include "bootstrap/egress.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "capture/capturestop.h"
#include "camera/camera_control.h"
#include "control/campose.h"
#include "control/console.h"
#include "control/diagwriter.h"
#include "control/loadgate.h"
#include "control/notes.h"
#include "control/seekgoto.h"
#include "engine/session.h"
#include "playback/core/playback.h"
#include "playback/core/transport.h"
#include "ui/uiapi.h"

namespace replay {
namespace control {
namespace dispatch {
namespace {

const char* const kReplayModuleName = "control";

ReplayServices* g_services = nullptr;
CommandQueue    g_queue;

std::atomic<uint64_t> g_queued{0};
std::atomic<uint64_t> g_executed{0};

// A seqlock rather than a mutex. The console reader thread reads this while the tick writes it, and
// a mutex on the tick path is how a stalled reader becomes a dropped frame. An odd sequence means a
// write is in progress; a reader that sees one, or that sees the sequence move under it, retries.
std::atomic<uint32_t> g_seq{0};
Snapshot g_snapshot;

// Written by the surfaces' own threads at init, read by the tick's publish, under their own
// sequence for the same reason.
struct SurfaceState {
    bool up = false;
    char note[160] = {0};
};
std::atomic<uint32_t> g_surfaceSeq{0};
SurfaceState g_console;
SurfaceState g_http;
// The launch request carries a count as well as a note: a refusal the owner has to go and
// find in the log is a refusal the surface did not report.
struct LaunchSurface {
    bool     up = false;
    uint32_t refusals = 0;
    char     note[160] = {0};
};
LaunchSurface g_openReq;

char     g_lastResult[256] = {0};
uint64_t g_lastSeq = 0;
std::atomic<uint64_t> g_nextSeq{1};
std::atomic<uint64_t> g_declined{0};

// ---- The finished results, so a surface can find out what its command actually did ------------
//
// A ring rather than a slot. `last` was one slot, so two commands in one tick meant the first
// answer was gone before anyone could read it, and the console's only recovery was to type `diag`
// fast enough. Sixteen is far more than the sixteen-per-tick drain limit will produce between one
// surface's submit and its collect, and a sequence number that has been overwritten reads as
// "pending" rather than as somebody else's answer.
constexpr size_t kDoneSlots = 16;

struct DoneSlot {
    std::atomic<uint64_t> seq{0};   // odd while being written, even and == 2*n when slot n is ready
    Result result;
};
DoneSlot g_done[kDoneSlots];

void FileResult(uint64_t seq, const Result& r) {
    if (seq == 0) {
        return;
    }
    DoneSlot& slot = g_done[seq % kDoneSlots];
    // Seqlock, the same shape as the snapshot's: the tick writes, any surface reads.
    const uint64_t stamp = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(stamp | 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.result = r;
    slot.result.seq = seq;
    std::atomic_thread_fence(std::memory_order_release);
    slot.seq.store(seq << 1, std::memory_order_release);
}

// The notes sidecar of whatever is loaded. Read on the tick, once per capture, so `marks` and
// `goto` are answers about a file this module has actually opened rather than disk I/O done from a
// console thread or inside an HTTP handler.
notes::Sidecar    g_notes;
notes::LoadResult g_notesState = notes::LoadResult::Absent;
wchar_t           g_notesFor[kMaxPathChars] = {0};
bool              g_notesKnown = false;

void CopyStr(char* dst, size_t cap, const char* src) {
    if (cap == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = 0;
        return;
    }
    size_t i = 0;
    for (; i + 1 < cap && src[i] != 0; ++i) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

void CopyWide(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (cap == 0) {
        return;
    }
    size_t i = 0;
    if (src != nullptr) {
        for (; i + 1 < cap && src[i] != 0; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = 0;
}

Result Say(bool ok, const char* fmt, ...) {
    Result r;
    r.ok = ok;
    va_list args;
    va_start(args, fmt);
    vsnprintf(r.text, sizeof r.text, fmt, args);
    va_end(args);
    return r;
}

// The refusal used wherever a service a command needs has not registered. It names the service and
// the packet that owns it, because "cam: unavailable" sends the user looking at their own typing.
Result MissingService(const char* name, uint8_t missing) {
    if ((missing & kNeedsLoaded) != 0) {
        return Say(false,
                   "%s: nothing loaded -- no capture is open, so there is nothing to drive. "
                   "`list` opens the browser, `load <file>` opens one by name.", name);
    }
    if ((missing & kNeedsTransport) != 0) {
        return Say(false,
                   "%s: no replay transport has registered, so there is nothing to drive. Load a "
                   "capture first.", name);
    }
    if ((missing & kNeedsCamera) != 0) {
        return Say(false,
                   "%s: the spectator camera rig has not checked out, so no mode can be selected",
                   name);
    }
    return Say(false, "%s: a service it needs is not available", name);
}

uint8_t MissingFor(const CommandSpec* spec) {
    if (spec == nullptr || g_services == nullptr) {
        return 0;
    }
    uint8_t missing = 0;
    if ((spec->needs & kNeedsTransport) != 0 && g_services->transport == nullptr) {
        missing |= kNeedsTransport;
    }
    // Against camera_control::Get(), NOT ReplayServices::camera. They are different objects:
    // B.7's IReplayCamera is the pose sink, and mode selection lives on W2-E's control interface.
    // Checking the pose sink here would pass while the thing `cam` actually calls was absent.
    if ((spec->needs & kNeedsCamera) != 0 && camera_control::Get() == nullptr) {
        missing |= kNeedsCamera;
    }
    // ★ AGAINST THE UI's ANSWER, NOT AGAINST THE TRANSPORT POINTER. The transport object exists
    // from module init onward, so testing it said nothing about whether a capture was open --
    // which is how every transport command reported success over a Play() that returned at its
    // first line. ui::LoadedPath() is null exactly when nothing is loaded. Tick thread only, which
    // is where Execute runs.
    //
    // ★★ AND AGAINST THE TRANSPORT'S OWN WORD TOO, now that W2-B has given it one. The two can
    // disagree -- a load in flight, or one the transport refused after the panel recorded the path
    // -- and CaptureIsOpen makes the disagreement a refusal instead of a command sent to a capture
    // that is not there.
    if ((spec->needs & kNeedsLoaded) != 0) {
        IReplayTransport* t = g_services->transport;
        if (!CaptureIsOpen(ui::LoadedPath() != nullptr, t != nullptr,
                           t != nullptr ? t->State() : TransportState::NotLoaded)) {
            missing |= kNeedsLoaded;
        }
    }
    return missing;
}

// ---- handing the bookmarks to the panel ------------------------------------------------------
//
// Staged in the shape ui::SetBookmarks takes. Static rather than automatic: 32 KB does not belong
// on the tick's stack, and like every other replay::ui call this is tick-thread-only.
ui::BookmarkView g_view[notes::kMaxBookmarks];
char             g_viewLabels[notes::kMaxBookmarks][128];

// ★ FOUR OF OUR SIX ANSWERS COLLAPSE HERE, AND ONE MUST NOT. Unreadable, Damaged, UnknownVersion
// and AbsoluteAxisVersion all mean the same thing to a panel: there IS a sidecar, we have not read
// it, and it has been left alone. None of them may become Absent -- "there are no bookmarks" over a file
// that may hold forty is the false all-clear both packets exist to make unrepresentable. The exact
// reason stays in `diag` and /replay/status, which have room to print it.
//
// NotSupplied is not produced here at all. It is the panel's own "nobody has told us yet", and a
// surface that pushed it would be claiming ignorance it does not have.
ui::NotesState PanelNotesState(notes::LoadResult r) {
    switch (r) {
        case notes::LoadResult::Ok:
            return ui::NotesState::Ok;
        case notes::LoadResult::Absent:
            return ui::NotesState::Absent;
        case notes::LoadResult::Unreadable:
        case notes::LoadResult::Damaged:
        case notes::LoadResult::UnknownVersion:
        case notes::LoadResult::AbsoluteAxisVersion:
            break;
    }
    return ui::NotesState::Damaged;
}

// ★★★ THE TIMES GO ACROSS UNCHANGED, AND THAT IS THE DECISION RATHER THAN AN OMISSION.
//
// uiapi.h asks for the PLAYHEAD's axis -- "the same 0..duration the timeline draws and `mark`
// recorded from". `mark` records exactly IReplayTransport::PlayheadUs(), `goto` hands exactly that
// value back to ui::SeekUs, and the panel sets its own g.playheadUs from the same PlayheadUs()
// call. So an unconverted push puts a bookmark on precisely the axis the panel draws its playhead
// on: whatever that axis is, the marker and the playhead agree, which is the property that makes a
// marker mean anything.
//
// ★★ WHAT I COULD NOT SETTLE, AND DID NOT PAPER OVER. Transport::DurationUs() returns
// `end - m.t0Us`, a 0-BASED length, while the playhead comes from a tick table filled with
// `rec.sourceTimeUs` and seeked to `t0` at load. If those two are not the same origin then the
// panel's playhead, ui::SeekFraction, the console's `seek` and this push are all off by t0Us
// TOGETHER -- and subtracting a t0 here would fix nothing while putting a second, disagreeing
// answer next to the panel's own playhead. There is no t0 accessor on this seam to subtract with.
// Reported to the orchestrator as a cross-packet question for W2-B and W2-F; not guessed at here.
void PushBookmarks() {
    if (!g_notesKnown) {
        // Nothing is open. The panel clears itself on Unload and on every LoadPath, and a push
        // about no capture would be a statement about a file we have not got.
        return;
    }
    const ui::NotesState state = PanelNotesState(g_notesState);
    if (state != ui::NotesState::Ok) {
        ui::SetBookmarks(state, nullptr, 0, 0);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < g_notes.BookmarkCount() && n < notes::kMaxBookmarks; ++i) {
        const notes::Bookmark* b = g_notes.BookmarkAt(i);
        if (b == nullptr) {
            break;
        }
        g_viewLabels[n][0] = 0;
        if (b->label[0] != 0) {
            // WC_ERR_INVALID_CHARS can only fail on an unpaired surrogate, which AddBookmark and
            // the parser both refuse -- so a failure here means that guard stopped holding, and an
            // empty label is the honest answer to it rather than a mojibake one.
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, b->label, -1, g_viewLabels[n],
                                    static_cast<int>(sizeof g_viewLabels[n]), nullptr,
                                    nullptr) <= 0) {
                g_viewLabels[n][0] = 0;
            }
        }
        g_view[n].tUs = b->tUs;
        g_view[n].label = g_viewLabels[n];
        ++n;
    }
    // Every bookmark, not the 64 the panel draws: SetBookmarks reports the SUPPLIED count
    // separately from the drawn one, so "24 of 256" is only sayable if it is handed all of them.
    ui::SetBookmarks(ui::NotesState::Ok, g_view, n, g_notes.LastPlayheadUs());
}

// Re-reads the sidecar when the open capture changes. On the tick, once per load: the alternative
// is a surface reading a file from its own thread, and the console's thread and the HTTP handler
// are both places this project has ruled out for exactly that.
void RefreshNotes(bool force) {
    const wchar_t* path = ui::LoadedPath();
    if (path == nullptr) {
        if (g_notesKnown) {
            g_notes.Clear();
            g_notesState = notes::LoadResult::Absent;
            g_notesFor[0] = 0;
            g_notesKnown = false;
        }
        return;
    }
    if (!force && g_notesKnown) {
        size_t i = 0;
        for (; path[i] != 0 && g_notesFor[i] != 0 && path[i] == g_notesFor[i]; ++i) {
        }
        if (path[i] == 0 && g_notesFor[i] == 0) {
            return;
        }
    }
    CopyWide(g_notesFor, kMaxPathChars, path);
    g_notesKnown = true;
    wchar_t sidecar[kMaxPathChars];
    if (!notes::SidecarPathFor(path, sidecar, kMaxPathChars)) {
        g_notes.Clear();
        g_notesState = notes::LoadResult::Unreadable;
        PushBookmarks();
        return;
    }
    // ★ WHOSE SIDECAR IS IT. L3-RUN-3 F3: the W2-G fixture was regenerated between two launches and
    // its sidecar carried four bookmarks and a resume point onto the new file, which flipped a probe
    // row. The capture's own header answers this, and a capture whose header cannot be read gets no
    // bookmarks at all -- "we could not ask" is not "it matches".
    notes::CaptureIdentity id;
    if (!notes::IdentityForCapture(path, &id)) {
        g_notes.Clear();
        g_notesState = notes::LoadResult::Unreadable;
        PushBookmarks();
        return;
    }
    g_notesState = g_notes.LoadFor(sidecar, id);
    if (g_notesState != notes::LoadResult::Ok) {
        g_notes.Clear();
    }
    PushBookmarks();
}

// ---- The commands that mutate. Tick thread only ----------------------------------------------

Result RunLoad(const Command& cmd) {
    // loadMostRecent is a flag the ECHOREPLAY_OPEN carrier sets, never something inferred from an
    // empty path: `load ""` used to reach this branch and open a capture the owner had not named.
    if (!(cmd.loadMostRecent ? ui::LoadMostRecent() : ui::LoadPath(cmd.text))) {
        // A legacy replay was taken for conversion and loads when it finishes. Not a refusal, and
        // not a load either: nothing below runs, so nothing is armed on intent.
        if (ui::ImportInProgress()) {
            return Say(true, "load accepted, converting first: %s", ui::StatusLine());
        }
        // The UI's own reason, verbatim. Rewording it here would give the console and the panel two
        // different explanations of one refusal.
        return Say(false, "load refused: %s", ui::StatusLine());
    }
    RefreshNotes(true);

    // ============================================================================================
    // THE LATE ARM. A session launched to play a replay is armed by the bootstrap at worker time,
    // but a session that began as normal play and BECOMES a replay here is not - and the owner's
    // "works offline, no server" constraint is about the replay, whichever way it started.
    //
    // On success rather than on intent, deliberately: arming is not reversible, and a mistyped
    // filename during a live match must not permanently cut that match off from its server. A load
    // that actually succeeded is the unambiguous transition.
    //
    // W1-C made this callable: RecordNotArmed no longer sets the attempted flag, so a capture-mode
    // decision does not silently refuse this. Arm() is idempotent and reports the first state.
    // ============================================================================================
    const egress::ArmState state = egress::Arm(egress::ArmTrigger::ReplayMode);
    log::Direct(state == egress::ArmState::Armed ? LogLevel::Info : LogLevel::Warn,
                kReplayModuleName,
                "playback began in this session: egress blocker %s (trigger %s)",
                egress::ArmStateName(state),
                egress::ArmTriggerName(egress::Current().trigger));

    // The sidecar is named in the reply, because a bookmark nobody knows about is a bookmark
    // nobody reaches. `marks` lists them; `goto resume` returns to where this capture was left.
    char notesNote[160] = {0};
    if (g_notesState == notes::LoadResult::Ok && g_notes.BookmarkCount() != 0) {
        snprintf(notesNote, sizeof notesNote, " -- %zu bookmark(s); `marks` lists them%s",
                 g_notes.BookmarkCount(),
                 g_notes.LastPlayheadUs() != 0
                     ? ", `goto resume` returns to where you left off"
                     : "");
    } else if (g_notesState != notes::LoadResult::Ok && g_notesState != notes::LoadResult::Absent) {
        snprintf(notesNote, sizeof notesNote, " -- the notes sidecar %s",
                 notes::LoadResultName(g_notesState));
    }
    return Say(true, "loaded: %s%s", ui::StatusLine(), notesNote);
}

Result RunMark(const Command& cmd) {
    // Whatever is actually open, from the one accessor that knows -- including a capture the owner
    // opened by clicking it in the browser, which this command used to refuse outright.
    const wchar_t* loaded = ui::LoadedPath();
    if (loaded == nullptr) {
        return Say(false, "mark: no capture is open");
    }
    wchar_t sidecar[kMaxPathChars];
    if (!notes::SidecarPathFor(loaded, sidecar, kMaxPathChars)) {
        return Say(false,
                   "mark: a sidecar can only be written beside a capture path of %zu characters or "
                   "fewer, and this one is longer. Move it, or shorten a folder name in its path.",
                   notes::MaxDemoPathChars());
    }

    // THE FILE IS READ FIRST, AND ONLY *ABSENT* AUTHORISES A NEW ONE. This used to be
    // `if (!s.Load(...) && !s.FileMissing())`, and FileMissing was set by EVERY open failure -- so
    // a sidecar held open by another handle, or carrying a deny-read ACE, or met during descriptor
    // exhaustion, took the first-ever-mark branch, and a week of the owner's bookmarks was replaced
    // by a fresh file holding one.
    notes::CaptureIdentity id;
    if (!notes::IdentityForCapture(loaded, &id)) {
        return Say(false,
                   "mark: %ls: this capture's own header could not be read, so a sidecar written "
                   "beside it could not be tied to it. Nothing was written.",
                   loaded);
    }

    notes::Sidecar s;
    const notes::LoadResult read = s.LoadFor(sidecar, id);
    if (read != notes::LoadResult::Ok && !notes::MayCreateNew(read)) {
        // Named in full, and with the way out. Nothing in this product can reset a sidecar -- that
        // would be a command whose whole job is deleting the owner's notes -- so a damaged one
        // stops `mark` on that capture until somebody removes the file, and the owner has no way
        // to know that unless this line says it.
        return Say(false,
                   "mark: %ls: %s. Nothing was written and it is exactly as it was. If it is beyond "
                   "saving, delete that file and the next mark starts a new one.",
                   sidecar, notes::LoadResultName(read));
    }

    const int64_t at = g_services->transport->PlayheadUs();
    const notes::AddResult added = s.AddBookmark(at, cmd.text[0] != 0 ? cmd.text : nullptr);
    if (added != notes::AddResult::Ok) {
        // One reason. The message this replaces offered the reader a choice of two, where the code
        // knew which one it was.
        return Say(false, "mark: %s", notes::AddResultName(added));
    }
    s.SetLastPlayheadUs(at);
    // Stamped on every save, not only on the first: a sidecar this build rewrites is a sidecar this
    // build can prove belongs here.
    s.SetIdentity(id);
    const notes::SaveResult wrote = s.Save(sidecar);
    if (wrote != notes::SaveResult::Ok) {
        return Say(false, "mark: %ls: %s", sidecar, notes::SaveResultName(wrote));
    }
    RefreshNotes(true);
    return Say(true, "bookmarked %lld us as #%zu (`marks` lists them, `goto %zu` comes back)",
               static_cast<long long>(at), s.BookmarkCount(), s.BookmarkCount());
}

// One page of the sidecar. This listing is the reader that did not exist: `mark` wrote bookmarks
// that nothing in the product could show or jump to, which made the command a file generator
// rather than a feature.
Result RunMarks(const Command& cmd) {
    if (g_notesState == notes::LoadResult::Absent) {
        return Say(true, "no bookmarks in this capture yet. `mark` makes one.");
    }
    if (g_notesState != notes::LoadResult::Ok) {
        return Say(false, "marks: the sidecar beside this capture %s",
                   notes::LoadResultName(g_notesState));
    }
    const size_t total = g_notes.BookmarkCount();
    if (total == 0) {
        return Say(true, "no bookmarks in this capture yet. `mark` makes one.");
    }
    constexpr size_t kPerPage = 20;
    const size_t pages = (total + kPerPage - 1) / kPerPage;
    size_t page = static_cast<size_t>(cmd.whole);
    if (page > pages) {
        page = pages;
    }
    const size_t first = (page - 1) * kPerPage;
    const size_t last = (first + kPerPage) < total ? (first + kPerPage) : total;

    Result r;
    r.ok = true;
    TextOut t(r.text, sizeof r.text);
    t.Line("%zu bookmark(s), page %zu of %zu. `goto <n>` jumps to one.\n", total, page, pages);
    for (size_t i = first; i < last; ++i) {
        const notes::Bookmark* b = g_notes.BookmarkAt(i);
        if (b == nullptr) {
            break;
        }
        const long long ms = static_cast<long long>(b->tUs / 1000);
        t.Line("  %3zu  %lld:%02lld.%03lld  %ls\n", i + 1, ms / 60000, (ms / 1000) % 60, ms % 1000,
               b->label[0] != 0 ? b->label : L"(no label)");
    }
    if (g_notes.LastPlayheadUs() != 0) {
        t.Line("  resume point %lld us -- `goto resume`\n",
               static_cast<long long>(g_notes.LastPlayheadUs()));
    }
    if (t.Truncated()) {
        r.ok = false;
    }
    return r;
}

// The landing tolerance is ASKED FOR, once, here. Returns nullptr when the landing may
// be reported and `*landing` is written; otherwise the word the arm must refuse with. There are
// FOUR answers and SeekLanding holds three: "this machine has published no frame period yet" is
// not a landing verdict at all, and an arm handed `on-target` in that case would be stating where
// the playhead is on the strength of a number nobody measured.
const char* SeekLandingRefusal(int64_t targetUs, int64_t landedUs, SeekLanding* landing) {
    // The tolerance is one tick OF THE FILE, so it is asked of the transport that loaded the file
    // and of nothing else. The render frame period this used to ask for is a fact about this
    // machine; judging a 30 Hz capture's landings against a 90 Hz machine's frames refused correct
    // nearest-tick landings. The decision itself lives in seekgoto.h because tests/control cannot
    // link this file.
    return SeekLandingRefusalFor(g_services != nullptr ? g_services->transport : nullptr, targetUs,
                                 landedUs, landing);
}

Result RunGoto(const Command& cmd) {
    if (cmd.gotoResume) {
        // The resume point is the field 5.9.2 put in the sidecar and nothing ever read back.
        // `mark` wrote it on every bookmark and there was no way to return to it.
        if (g_notesState != notes::LoadResult::Ok || g_notes.LastPlayheadUs() == 0) {
            return Say(false,
                       "goto resume: this capture has no resume point yet. One is recorded every "
                       "time you `mark`.");
        }
        const int64_t at = g_notes.LastPlayheadUs();
        if (!ui::SeekUs(at)) {
            return Say(false, "goto resume refused: %s", ui::StatusLine());
        }
        const int64_t landed = g_services->transport->PlayheadUs();
        SeekLanding landing = SeekLanding::OnTarget;
        if (const char* bad = SeekLandingRefusal(at, landed, &landing)) {
            return Say(false, "goto resume: %s -- the resume point is %lld us and the playhead is "
                              "at %lld us (%+lld us)",
                       bad, static_cast<long long>(at),
                       static_cast<long long>(landed), static_cast<long long>(landed - at));
        }
        return Say(true, "resumed at %lld us", static_cast<long long>(landed));
    }
    if (g_notesState != notes::LoadResult::Ok) {
        return Say(false, "goto: the sidecar beside this capture %s",
                   notes::LoadResultName(g_notesState));
    }
    if (cmd.gotoNext || cmd.gotoPrev) {
        const bool forward = cmd.gotoNext;
        const char* word = forward ? "next" : "prev";
        int64_t marks[notes::kMaxBookmarks];
        size_t n = 0;
        for (size_t i = 0; i < g_notes.BookmarkCount() && n < notes::kMaxBookmarks; ++i) {
            const notes::Bookmark* b = g_notes.BookmarkAt(i);
            if (b != nullptr) {
                marks[n++] = b->tUs;
            }
        }
        const int64_t now = g_services->transport->PlayheadUs();
        int64_t land = 0;
        const MarkerStep step = StepMarkerUs(marks, n, now, forward, &land);
        if (step == MarkerStep::NoMarkers) {
            return Say(false, "goto %s: this capture has no bookmarks yet. `mark` makes one.",
                       word);
        }
        if (step == MarkerStep::AtEdge) {
            // No wrap. A wrap would turn "step forward through the markers" into a silent jump
            // backwards to the start (ARCH-17 S8.2), and the count is named so this refusal cannot
            // be mistaken for the empty-sidecar one above.
            return Say(false, "goto %s: no bookmark is %s %lld us; this capture has %zu and "
                              "`goto %s` does not wrap",
                       word, forward ? "after" : "before", static_cast<long long>(now), n, word);
        }
        if (!ui::SeekUs(land)) {
            return Say(false, "goto %s refused: %s", word, ui::StatusLine());
        }
        // Read back, same rule as the seek arms: the bookmark's time is where we ASKED to go, and
        // L3-RUN-3 F1 recorded this line answering "bookmark at 2133312" having landed on 2255533.
        const int64_t landed = g_services->transport->PlayheadUs();
        SeekLanding landing = SeekLanding::OnTarget;
        if (const char* bad = SeekLandingRefusal(land, landed, &landing)) {
            return Say(false, "goto %s: %s -- the bookmark is at %lld us and the playhead is at "
                              "%lld us (%+lld us)",
                       word, bad, static_cast<long long>(land),
                       static_cast<long long>(landed), static_cast<long long>(landed - land));
        }
        return Say(true, "goto %s: bookmark at %lld us, playhead at %lld us (from %lld us)", word,
                   static_cast<long long>(land), static_cast<long long>(landed),
                   static_cast<long long>(now));
    }
    const notes::Bookmark* b = g_notes.BookmarkAt(static_cast<size_t>(cmd.whole) - 1);
    if (b == nullptr) {
        return Say(false, "goto %lld: this capture has %zu bookmark(s)",
                   static_cast<long long>(cmd.whole), g_notes.BookmarkCount());
    }
    const int64_t at = b->tUs;
    if (!ui::SeekUs(at)) {
        return Say(false, "goto refused: %s", ui::StatusLine());
    }
    const int64_t landed = g_services->transport->PlayheadUs();
    SeekLanding landing = SeekLanding::OnTarget;
    if (const char* bad = SeekLandingRefusal(at, landed, &landing)) {
        return Say(false, "goto %lld: %s -- the bookmark is at %lld us and the playhead is at "
                          "%lld us (%+lld us)",
                   static_cast<long long>(cmd.whole), bad,
                   static_cast<long long>(at), static_cast<long long>(landed),
                   static_cast<long long>(landed - at));
    }
    return Say(true, "bookmark %lld at %lld us, playhead at %lld us: %ls",
               static_cast<long long>(cmd.whole), static_cast<long long>(at),
               static_cast<long long>(landed), b->label[0] != 0 ? b->label : L"(no label)");
}

Result RunSpeed(const Command& cmd) {
    if (cmd.rate == 0.0) {
        // Zero is the pause bit, never a zero rate: S7.3.3's clock derives a reciprocal from the
        // scaled delta, so a zero rate is +inf into every consumer. uiapi.h refuses it too.
        g_services->transport->Pause();
        return Say(true, "paused (speed 0)");
    }
    const char* why = nullptr;
    if (!ui::SetRate(cmd.rate, &why)) {
        // The ceiling is 66666 / raw_frame_us and the engine does not report crossing it, so a
        // refusal that named no number would leave the user believing the rate had been applied.
        return Say(false, "speed refused: %s", why != nullptr ? why : ui::StatusLine());
    }
    // Read back, not assumed -- and now it can genuinely differ: ui::SetRate routes a paused
    // transport through IReplayTransport::SetRate, which does not clear the pause bit, so `speed 2`
    // while paused answers "paused" and the picture does not move until `play`.
    // Four-valued, through the one table that names every TransportState. The two-valued form
    // this replaces read "not paused" as "playing", so a scrub, a seek and -- once NotLoaded
    // existed -- a transport with no file at all all reported themselves as playing.
    return Say(true, "speed %.4g (%s%s)", cmd.rate,
               TransportRunWord(g_services->transport->State()),
               TransportEndWord(g_services->transport->Ended()));
}

// ★ `rate` IS NOT `speed`. OWNER-WATCH-FIX Part 2 already made ui::SetRate pause-preserving, so
// the two verbs agree on every non-zero number by construction -- and they differ on zero, which is
// the whole reason DESIGN-UI-2 S7.2 asked for a second spelling. `speed 0` pauses; `rate 0` is a
// rate nobody can run at (S7.3.3's clock takes a reciprocal of it) and is refused by name.
Result RunRate(const Command& cmd) {
    if (cmd.rate == 0.0) {
        return Say(false, "rate 0 is not a rate -- the clock derives a reciprocal from it. Use "
                          "`pause` to hold the playhead, or `rate 0.1` for the slowest run");
    }
    const char* why = nullptr;
    if (!ui::SetRate(cmd.rate, &why)) {
        return Say(false, "rate refused: %s", why != nullptr ? why : ui::StatusLine());
    }
    // Read back, never echoed: the point of this verb is that a paused viewer is STILL paused
    // afterwards, and a line that asserted it without asking could not tell anyone it was wrong.
    return Say(true, "rate %.4g (%s%s) at %lld us", cmd.rate,
               TransportRunWord(g_services->transport->State()),
               TransportEndWord(g_services->transport->Ended()),
               static_cast<long long>(g_services->transport->PlayheadUs()));
}

// The player bar's Reverse button as a verb, to the same rule (bar.cpp: a rate change, never a
// play, so reversing while paused leaves the picture where it is). A stopped transport reports no
// rate to flip, so -1 is the direction the button picks there too.
Result RunReverse() {
    const double now = g_services->transport->Rate();
    const double flipped = now != 0.0 ? -now : -1.0;
    const char* why = nullptr;
    if (!ui::SetRate(flipped, &why)) {
        return Say(false, "reverse refused: %s", why != nullptr ? why : ui::StatusLine());
    }
    return Say(true, "rate %.4g (%s%s) at %lld us", flipped,
               TransportRunWord(g_services->transport->State()),
               TransportEndWord(g_services->transport->Ended()),
               static_cast<long long>(g_services->transport->PlayheadUs()));
}

// A scrub target in the two forms `seek` accepts, minus the relative one. Returns false having
// named the reason, exactly as RunSeek's percent arm does: a percentage of a duration nobody knows
// is a refusal, not a jump to zero.
bool ScrubTargetUs(const Command& cmd, int64_t* outUs, const char** why) {
    if (!cmd.seekIsPercent) {
        *outUs = cmd.whole;
        return true;
    }
    const uint64_t raw = g_services->transport->DurationUs();
    if (raw == 0) {
        *why = "this capture's duration is not known yet, so a percentage names no time";
        return false;
    }
    const double dur = static_cast<double>(raw);
    *outUs = static_cast<int64_t>(dur * cmd.percent / 100.0);
    return true;
}

Result RunScrub(const Command& cmd) {
    IReplayTransport* const t = g_services->transport;
    if (cmd.scrubPhase == Command::ScrubPhase::Begin) {
        t->BeginScrub();
        if (t->State() != TransportState::Scrub) {
            // The one case that refuses silently inside the transport is the pre-roll, where the
            // track is inert by design -- so the reply says which it was rather than "ok".
            return Say(false, "scrub begin refused: %s",
                       t->Preparing().preparing ? "the replay is still preparing"
                                                : ui::StatusLine());
        }
        return Say(true, "scrub begin at %lld us",
                   static_cast<long long>(t->PlayheadUs()));
    }

    int64_t target = -1;
    if (cmd.scrubHasTarget) {
        const char* why = "unstated";
        if (!ScrubTargetUs(cmd, &target, &why)) {
            return Say(false, "scrub: %s", why);
        }
    }

    if (cmd.scrubPhase == Command::ScrubPhase::To) {
        if (t->State() != TransportState::Scrub) {
            return Say(false, "scrub to: no drag is active -- `scrub begin` first");
        }
        t->ScrubTo(target);
        const ScrubStats st = t->Scrub();
        return Say(true, "scrub to %lld us (pending %lld us, applied %lld us, %u coalesced)",
                   static_cast<long long>(target), static_cast<long long>(st.pendingUs),
                   static_cast<long long>(st.appliedUs), st.skipped);
    }

    // `scrub end` with no drag active is not an error: EndScrub is idempotent, and a script that
    // ends a drag it never began has still left the transport in the state it wanted.
    const bool wasScrubbing = t->State() == TransportState::Scrub;
    const ScrubStats before = t->Scrub();
    t->EndScrub(target);
    return Say(true, "scrub end at %lld us (%s%s)%s, %u coalesced",
               static_cast<long long>(t->PlayheadUs()),
               TransportRunWord(t->State()), TransportEndWord(t->Ended()),
               wasScrubbing ? "" : " -- no drag was active", before.skipped);
}

Result RunSeek(const Command& cmd) {
    // ★ THE SPAN-RELATIVE FORM, and it is resolved here rather than in the parser because only the
    // transport knows where this capture's content begins. `seek @12.5` lands 12.5 s into the
    // recorded content -- the same axis the timeline draws and the readout prints.
    if (cmd.seekIsSpanRelative) {
        IReplayTransport* t = g_services->transport;
        int64_t first = 0, last = 0;
        if (!t->PlayableSpanUs(&first, &last) || last <= first) {
            return Say(false, "seek: this capture's recorded span is not known, so a time inside "
                              "it names nothing; `seekfile <t>` is the whole-file axis");
        }
        int64_t at = 0;
        if (cmd.seekIsPercent) {
            at = first + static_cast<int64_t>(static_cast<double>(last - first) * cmd.percent / 100.0);
        } else {
            at = first + cmd.whole;
        }
        if (at < first) at = first;
        if (at > last) at = last;
        if (!ui::SeekUs(at)) {
            return Say(false, "seek refused: %s", ui::StatusLine());
        }
        const int64_t landed = t->PlayheadUs();
        return Say(true, "seek @%lld us into the recorded span [%lld..%lld] asked for %lld us and "
                         "the playhead is at %lld us (%+lld us into the span)",
                   static_cast<long long>(at - first), static_cast<long long>(first),
                   static_cast<long long>(last), static_cast<long long>(at),
                   static_cast<long long>(landed), static_cast<long long>(landed - first));
    }
    if (cmd.seekIsRelative) {
        IReplayTransport* t = g_services->transport;
        const uint64_t rawDuration = t->DurationUs();
        const int64_t durationUs =
            rawDuration > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX
                                                           : static_cast<int64_t>(rawDuration);
        const int64_t from = t->PlayheadUs();
        int64_t land = 0;
        const RelativeSeek how = ResolveRelativeSeekUs(from, cmd.whole, durationUs, &land);
        if (how == RelativeSeek::NoRange) {
            return Say(false, "seek %+lld us: this capture's duration is not known yet, so there "
                              "is no range to move inside",
                       static_cast<long long>(cmd.whole));
        }
        if (!ui::SeekUs(land)) {
            return Say(false, "seek refused: %s", ui::StatusLine());
        }
        // The landing READ BACK, never the arithmetic target. `land` is this arm's own sum, and
        // printing it claimed a playhead position nobody had asked the transport for: L3-RUN-3 F1
        // saw `seek +10` answer "to 12133312 us" having landed on 2255533 us. 6e18393 fixed exactly
        // this for the absolute arm below and left this one echoing itself. The clamp is still said
        // out loud, because a nudge that ran into the end of the file and reported the delta it was
        // asked for would be a second silent lie on the same line.
        const int64_t landed = g_services->transport->PlayheadUs();
        SeekLanding landing = SeekLanding::OnTarget;
        if (const char* bad = SeekLandingRefusal(land, landed, &landing)) {
            return Say(false, "seek %+lld us: %s -- from %lld us this asked for %lld us and the "
                              "playhead is at %lld us (%+lld us)",
                       static_cast<long long>(cmd.whole), bad,
                       static_cast<long long>(from), static_cast<long long>(land),
                       static_cast<long long>(landed), static_cast<long long>(landed - land));
        }
        return Say(true, "seek %+lld us from %lld us to %lld us%s%s",
                   static_cast<long long>(cmd.whole), static_cast<long long>(from),
                   static_cast<long long>(landed),
                   how == RelativeSeek::Clamped ? " (clamped to the file)" : "",
                   landing == SeekLanding::NearestTick
                       ? " (the nearest tick this capture records)" : "");
    }
    if (cmd.seekIsPercent) {
        // This arm used to be ui::SeekFraction, which ends on a void SeekTo and returns
        // true whatever the transport did -- so `seek 50%` reported success on a capture with no
        // duration, and reported "seek to 50%" wherever the playhead actually ended up. It is now
        // the absolute arm: a resolved time, one Seek, and the landing read back and classified.
        IReplayTransport* t = g_services->transport;
        const uint64_t rawDuration = t->DurationUs();
        const int64_t durationUs =
            rawDuration > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX
                                                           : static_cast<int64_t>(rawDuration);
        int64_t at = 0;
        if (ResolvePercentSeekUs(cmd.percent, durationUs, &at) == PercentSeek::NoRange) {
            return Say(false, "seek %.4g%%: this capture's duration is not known yet, so a "
                              "percentage of it is not a time",
                       cmd.percent);
        }
        if (!ui::SeekUs(at)) {
            return Say(false, "seek refused: %s", ui::StatusLine());
        }
        const int64_t landed = t->PlayheadUs();
        SeekLanding landing = SeekLanding::OnTarget;
        if (const char* bad = SeekLandingRefusal(at, landed, &landing)) {
            return Say(false, "seek %.4g%%: %s -- this asked for %lld us and the playhead is at "
                              "%lld us (%+lld us)",
                       cmd.percent, bad, static_cast<long long>(at),
                       static_cast<long long>(landed), static_cast<long long>(landed - at));
        }
        return Say(true, "seek to %.4g%% (%lld us), playhead at %lld us%s", cmd.percent,
                   static_cast<long long>(at), static_cast<long long>(landed),
                   landing == SeekLanding::NearestTick
                       ? " (the nearest tick this capture records)" : "");
    }
    if (!ui::SeekUs(cmd.whole)) {
        return Say(false, "seek refused: %s", ui::StatusLine());
    }
    // Where it LANDED, and the miss said out loud. The relative form has always reported its clamp;
    // this one echoed the target, so `seek 4999950` answered "seek to 4999950 us" having landed on
    // 4022182 -- the last tick the file records -- and nothing in the answer said so (L3-RUN-2 F3).
    const int64_t landed = g_services->transport->PlayheadUs();
    if (landed == cmd.whole) {
        return Say(true, "seek to %lld us", static_cast<long long>(cmd.whole));
    }
    return Say(true, "seek to %lld us landed on %lld us (%+lld us: the nearest tick this capture "
                     "records)",
               static_cast<long long>(cmd.whole), static_cast<long long>(landed),
               static_cast<long long>(landed - cmd.whole));
}

Result RunRound(const Command& cmd) {
    if (!ui::RoundsKnown()) {
        // "no rounds" and "we could not read the manifest" are different answers, and uiapi.h keeps
        // them apart precisely so this refusal can say which one it is.
        return Say(false, "round: this capture's round table is not known (%s)", ui::StatusLine());
    }
    if (!ui::SeekRoundNumber(cmd.whole)) {
        return Say(false, "round %lld: no round is labelled that; this capture has %zu",
                   static_cast<long long>(cmd.whole), ui::RoundCount());
    }
    return Say(true, "round %lld", static_cast<long long>(cmd.whole));
}

// The mode word is resolved by the engine's own table, never by one of ours: camera_control.h's
// ModeFromName reads the seven names out of CAM.HTTP_CAMERA_MODE_HANDLER. That is also why `orbit`
// and `asseen` -- which DESIGN 8.5.2 lists -- are refused here with the real set named, rather than
// silently accepted and doing nothing.
// The camera-taking verbs. `cam release` is not here: it only hands gates
// back, which is safe in any session, and `cam flight` only marks a window and writes nothing.
//
// The write itself refuses in camera_writepoint::SetPose -- that is the gate that cannot be
// walked around. This one exists so the operator gets a sentence naming the verb and the reason
// instead of a command that reports success and quietly does nothing.
bool CamVerbRefusedLive(const char* verb, Result* out) {
    if (livesafety::Allowed(livesafety::Feature::Camera)) return false;
    *out = Say(false,
               "%s: refused -- %s. The spectator camera and every view state's input gate stay "
               "the engine's",
               verb, livesafety::VerdictReason(livesafety::Current()));
    return true;
}

Result RunCam(const Command& cmd) {
    Result refusal;
    if (CamVerbRefusedLive("cam", &refusal)) return refusal;
    IReplayCameraControl* cam = camera_control::Get();
    if (cam == nullptr) {
        return Say(false,
                   "cam: the spectator rig has not checked out, so no camera mode can be selected");
    }
    CameraMode mode = CameraMode::Pov;
    if (!camera_control::ModeFromName(cmd.camMode, &mode)) {
        return Say(false,
                   "cam: the engine has no camera called \"%s\". Its own set is pov, follow, "
                   "level, disc, side and free.", cmd.camMode);
    }
    if (!camera_control::IsSelectableByHuman(mode)) {
        return Say(false, "cam: %s is reserved for the oracle track and is not selectable",
                   camera_control::ModeName(mode));
    }

    // Which modes take a selector is the engine's rule, so it is enforced against the engine's
    // table rather than guessed at parse time. Passing a selector to a mode that ignores it would
    // look accepted and change nothing.
    const bool takesSelector =
        mode == CameraMode::Pov || mode == CameraMode::Follow || mode == CameraMode::Level;
    if (takesSelector && !cmd.camHasIndex) {
        return Say(false, "cam %s: needs a selector, as in `cam %s 1`",
                   camera_control::ModeName(mode), camera_control::ModeName(mode));
    }
    if (!takesSelector && cmd.camHasIndex) {
        return Say(false, "cam %s: takes no selector; the engine ignores one",
                   camera_control::ModeName(mode));
    }
    // The range is the parser's rule, applied again here on the value about to reach the
    // engine: a Command can be built by any producer, and 0 is packed by the engine as the same
    // word it packs for player 10. One predicate, one sentence -- both from command.h -- so this
    // arm and `cam look at` cannot come to differ.
    if (IsPlayerSelectorMode(cmd.camMode) && !IsPlayerSelector(cmd.whole)) {
        char refusal[kMaxErrorChars];
        FormatSelectorOutOfRange(refusal, sizeof refusal, "cam", camera_control::ModeName(mode),
                                 cmd.whole);
        return Say(false, "%s", refusal);
    }

    if (!cam->SetMode(mode, static_cast<int32_t>(cmd.whole))) {
        return Say(false, "cam %s refused: %s", camera_control::ModeName(mode),
                   ControlRefusalName(cam->LastRefusal()));
    }
    // Report what the engine LANDED on, not what we asked for. Level clamps the index itself, and
    // a reply echoing the request would hide that.
    int32_t packed = 0;
    uint16_t cameraIndex = 0;
    if (cam->ObservedSelector(&packed, &cameraIndex)) {
        if (mode == CameraMode::Level) {
            return Say(true, "cam level %u (engine clamped to its set)",
                       static_cast<unsigned>(cameraIndex));
        }
        return Say(true, "cam %s %lld (selector 0x%08X)", camera_control::ModeName(mode),
                   static_cast<long long>(cmd.whole), static_cast<unsigned>(packed));
    }
    return Say(true, "cam %s", camera_control::ModeName(mode));
}

// THE COMMANDED POSE FAMILY (ARCH-19 E1). The two interface calls live here and not in campose.h:
// the S9 done-when is an anchored `->CommandPose(` / `->ReleasePose(` grep over src/**.cpp that must
// name this file, and a call site in a header would leave that criterion unsatisfied. Every decision
// either call feeds -- the flag mapping, the three-way outcome, each sentence -- is in campose.h,
// which tests/control can sweep; this arm is the wiring and nothing else.
Result RunCamPose(const Command& cmd) {
    char verb[40];
    if (cmd.id == CommandId::CamMove) {
        snprintf(verb, sizeof verb, "cam move");
    } else if (cmd.camLookAt) {
        // The form that was typed, so a refusal names the command the operator gave and not a
        // shortened one they would have to translate back.
        snprintf(verb, sizeof verb, "cam look at %lld", static_cast<long long>(cmd.whole));
    } else {
        snprintf(verb, sizeof verb, "cam look");
    }

    Result refusal;
    if (CamVerbRefusedLive(verb, &refusal)) return refusal;

    IReplayCameraControl* cam = camera_control::Get();
    if (cam == nullptr) {
        return Say(false, "%s: the spectator rig has not checked out, so there is no pose to place",
                   verb);
    }

    CommandedPose p;
    PoseFromCommand(cmd, &p);
    const bool accepted = cam->CommandPose(p);

    // Read back through the engine, always, and only when the write was accepted. ARCH-17 S3.4:
    // the commanded sentence and the landed sentence are different on purpose -- an absolute move
    // the engine clamps, or a relative one it resolves against a pose that moved under us, lands
    // somewhere the request does not name.
    // Through camera_control::SampleViewPose, not GetViewPose directly: it is the published
    // sampler ARCH-17 S7.4 aims a probe row at, so the row that scores this landing and the
    // sentence that reports it read the pose through ONE body and cannot answer differently. It is
    // GetViewPose plus a wall stamp, and a null control never reaches this line.
    camera_control::ViewPoseSample sample;
    camera::ViewPoseRead read = camera::ViewPoseRead::NotBound;
    if (accepted) {
        read = camera_control::SampleViewPose(cam, &sample);
    }

    char text[kResultChars];
    const CamPoseOutcome outcome = ClassifyCamPose(accepted, read);
    switch (outcome) {
        case CamPoseOutcome::Refused:
            FormatCamPoseRefused(verb, cam->LastRefusal(), camera_control::Diagnostic(), text,
                                 sizeof text);
            break;
        case CamPoseOutcome::Landed:
            if (p.lookAt) {
                FormatCamLookAtLanded(verb, p.lookAtSelector, sample.pose, cam->Regime(), text,
                                      sizeof text);
            } else {
                FormatCamPoseLanded(verb, sample.pose, cam->Regime(), text, sizeof text);
            }
            break;
        case CamPoseOutcome::CommandedNotReadBack:
            FormatCamPoseNotReadBack(verb, read, camera_control::ViewReadRefusal(), text,
                                     sizeof text);
            break;
    }
    return Say(CamPoseOk(outcome), "%s", text);
}

// ARCH-23 S14.11 (W2-G-32a). The owner brackets his own free flight and the run scores the stretch
// between the markers. The line is written by the camera module, on the clock its pose samples
// carry -- a stamp taken here would put the window and the samples on two timelines.
//
// The open/closed belief is this side's, not the log's: the log holds every marker ever stamped,
// while this flag only answers "did the operator already say begin" so the reply can say so.
bool g_flightWindowOpen = false;

Result RunCamFlight(const Command& cmd) {
    // A dropped mark never reaches the log, so this side's belief must not move
    // either -- and the reply says the drop by its own name rather than "the window is open".
    if (!camera_control::MarkFlightWindow(cmd.camFlightBegin)) {
        return Say(false, "cam flight: %s", camera_control::FlightMarkerDroppedText());
    }
    const FlightMarkOutcome outcome = ClassifyFlightMark(cmd.camFlightBegin, g_flightWindowOpen);
    g_flightWindowOpen = FlightWindowAfter(outcome);
    char text[kResultChars];
    FormatFlightMark(outcome, text, sizeof text);
    return Say(true, "%s", text);
}

Result RunCamRelease(const Command&) {
    IReplayCameraControl* cam = camera_control::Get();
    if (cam == nullptr) {
        return Say(false, "cam release: the spectator rig has not checked out, so no pose is ours "
                          "to hand back");
    }
    // Both sides of the call: after a release the regime is Free either way, so only the regime
    // BEFORE says whether this command did anything, and "it did nothing" is a success that has to
    // say so rather than claim a hand-back that never happened.
    const PoseRegime before = cam->Regime();
    char text[kResultChars];
    if (!cam->ReleasePose()) {
        FormatCamPoseRefused("cam release", cam->LastRefusal(), camera_control::Diagnostic(), text,
                             sizeof text);
        return Say(false, "%s", text);
    }
    FormatCamRelease(before, cam->Regime(), text, sizeof text);
    return Say(true, "%s", text);
}

Result Execute(const Command& cmd) {
    const CommandSpec* spec = SpecFor(cmd.id);
    if (spec == nullptr) {
        return Say(false, "no such command");
    }
    if (g_services == nullptr) {
        return Say(false, "%s: the control module is not initialised", spec->name);
    }
    const uint8_t missing = MissingFor(spec);
    if (missing != 0) {
        return MissingService(spec->name, missing);
    }

    IReplayTransport* t = g_services->transport;
    switch (cmd.id) {
        case CommandId::List:
            ui::OpenBrowser();
            return Say(true, "%s", ui::StatusLine());

        case CommandId::Load:
            return RunLoad(cmd);

        case CommandId::Play: {
            // ★ THE RATE THE VIEWER ALREADY CHOSE, not a fresh 1x. `rate 0.5` then `play` answered
            // "playing at 1x" and meant it, and `rate -1` then `play` ran FORWARD to the end --
            // the one sequence the owner reaches for most ("pause, pick a speed, press play").
            // Rate() is the latched signed rate; it survives a pause, which §5.1 of the launch
            // verified separately. Only a rate of exactly 0 has no direction to resume in.
            const float latched = t->Rate();
            const float resume = latched != 0.0f ? latched : 1.0f;
            t->Play(resume);
            return Say(true, "playing at %gx", static_cast<double>(resume));
        }

        case CommandId::Pause:
            t->Pause();
            return Say(true, "paused at %lld us", static_cast<long long>(t->PlayheadUs()));

        case CommandId::Speed:
            return RunSpeed(cmd);

        case CommandId::Rate:
            return RunRate(cmd);

        case CommandId::Reverse:
            return RunReverse();

        case CommandId::Scrub:
            return RunScrub(cmd);

        case CommandId::Debug:
            // Read back rather than echoed: the key is a runtime latch inside the UI module now,
            // and a line that asserted the new value without asking could not report a refusal.
            ui::SetDebugPanel(cmd.debugOn);
            return Say(true, "debug panel %s", ui::DebugPanelOn() ? "on" : "off");

        case CommandId::Record: {
            char line[kResultChars];
            if (cmd.recordVerb == Command::RecordVerb::Start) {
                if (capture::CaptureIsRecording()) {
                    return Say(false, "already recording -- `record stop` closes it");
                }
                // REFUSED BY NAME, not armed and silently swallowed. The session gate is the thing
                // that would refuse this a tick later, so it is asked here: the owner typing
                // `record start` in a playback session was being told it would open on the next
                // live tick, and no file was ever written.
                const char* kind = "";
                if (!capture::CaptureSessionRecordable(&kind)) {
                    capture::RecordStatusLine(line, sizeof line);
                    return Say(false,
                               "recording is only available while spectating a live match; this is "
                               "a %s session and nothing will be written. %s",
                               kind, line);
                }
                capture::RequestManualRecordStart();
                capture::RecordStatusLine(line, sizeof line);
                return Say(true,
                           "record start asked for; it opens on the next tick of a live match. %s",
                           line);
            }
            if (cmd.recordVerb == Command::RecordVerb::Stop) {
                const bool stopping = capture::RequestManualRecordStop();
                capture::RecordStatusLine(line, sizeof line);
                return Say(stopping,
                           stopping ? "closing the recording. %s" : "nothing is recording. %s",
                           line);
            }
            capture::RecordStatusLine(line, sizeof line);
            return Say(true, "%s", line);
        }

        case CommandId::Bar:
            // Read back for the reason `debug` reads back, and with the reason word too: `bar on`
            // during a live capture is honoured, but `bar on` with `ui.bar` false cannot be, and a
            // line that echoed the request would not say which happened.
            if (cmd.barMode == Command::BarMode::Toggle) {
                ui::ToggleBar();
            } else {
                ui::SetBarShown(cmd.barMode == Command::BarMode::On);
            }
            return Say(true, "bar %s (%s)", ui::BarShown() ? "shown" : "hidden",
                       ui::BarShowReason());

        case CommandId::Seek:
        case CommandId::SeekFile:
            // One arm: the parser has already said which axis by setting seekIsSpanRelative.
            return RunSeek(cmd);

        case CommandId::Step: {
            // Count, never echo: the transport stops at either end of the tick table, and
            // reporting the number that was asked for turned that into an `ok` (L3-RUN-2 F4). The
            // count is the transport's own; the playhead is kept as the fallback for a transport
            // that does not count, and is not trusted over the count where the two can disagree.
            const int64_t before = t->PlayheadUs();
            t->Step(static_cast<int>(cmd.whole));
            const int64_t after = t->PlayheadUs();
            const int moved = t->LastStepTicks();
            if (ClassifyStep(before, after, moved) == StepOutcome::NoTickThatWay) {
                return Say(false,
                           "step %+lld: %s -- the playhead did not move off %lld us, so this "
                           "capture has no further recorded tick in that direction",
                           static_cast<long long>(cmd.whole), kStepNoTickThatWay,
                           static_cast<long long>(before));
            }
            // The count that was made, not the count that was asked for: a `step 5` that reached
            // two ticks and stopped used to answer `stepped 5`.
            const long long reported =
                moved == kStepTicksUncounted ? static_cast<long long>(cmd.whole) : moved;
            return Say(true, "stepped %lld tick(s) from %lld us to %lld us", reported,
                       static_cast<long long>(before), static_cast<long long>(after));
        }

        case CommandId::Round:
            return RunRound(cmd);

        case CommandId::Mode:
            // Through the UI so the panel's own checkbox and this command cannot disagree about
            // which mode is active.
            ui::SetAsSeen(cmd.asSeen);
            return Say(true, "mode %s (%s)", ui::AsSeen() ? "asseen" : "ground",
                       t->ActiveModeLabel());

        case CommandId::Mark:
            return RunMark(cmd);

        case CommandId::Marks:
            return RunMarks(cmd);

        case CommandId::Goto:
            return RunGoto(cmd);

        case CommandId::Console:
            // Said before the surface goes, and from the tick rather than from the reader thread:
            // the reader is about to be stopped and cannot be relied on to print its own farewell.
            console::Notice("closing this console. Echo VR keeps running; the panel and the HTTP "
                            "surface are unaffected.");
            console::Stop();
            SetConsoleState(false, "closed by the `console close` command");
            return Say(true, "console closed");

        case CommandId::Dress:
            // Contained-only, and the module says so: ReArmIdentityDress refuses outside a
            // contained process, where dressing would write another user's identity.
            if (!playback::ReArmIdentityDress(cmd.dressForce))
                return Say(false, "dress refused: nothing loaded, dressing is off, or this "
                                  "process is not contained");
            return Say(true, cmd.dressForce ? "dressing every recorded player again"
                                            : "dressing the players who are not dressed yet");

        case CommandId::Census: {
            const uint32_t lines = playback::LogComponentCensus();
            if (lines == 0)
                return Say(false, "census refused: no engine entity surface is bound");
            return Say(true, "census: %u line(s) written to the plugin log", lines);
        }

        case CommandId::AudioWrites: {
            const bool was = audio::StateWritesEnabled();
            if (cmd.audioWritesVerb != Command::AudioWritesVerb::Status) {
                const bool want = cmd.audioWritesVerb == Command::AudioWritesVerb::On;
                audio::SetStateWritesEnabled(want);
                // The init key is the outer term, so asking for ON in a session started with
                // audio.writes_enabled=false changes nothing -- and saying so is the difference
                // between a switch that refused and a switch that silently did not apply.
                if (want && !audio::StateWritesEnabled())
                    return Say(false, "audio writes stay OFF: this session started with "
                                      "audio.writes_enabled=false, which no console verb undoes");
            }
            const bool now = audio::StateWritesEnabled();
            return Say(true,
                       "audio writes %s%s: recorded Wwise states, per-emitter switches, the memo "
                       "write-back and the seek reconcile are %s. Sink pitch and "
                       "Suspend/Wakeup are NOT on this switch -- they are only correct in pairs",
                       now ? "ON" : "OFF", now == was ? " (unchanged)" : "",
                       now ? "issued" : "not issued");
        }

        case CommandId::Unload:
            ui::Unload();
            RefreshNotes(true);
            return Say(true, "unloaded");

        case CommandId::Cam:
            return RunCam(cmd);

        // needs = kNeedsCamera is already honoured by MissingFor / MissingService above, so these
        // three reach here only with a rig bound.
        case CommandId::CamMove:
            return RunCamPose(cmd);

        case CommandId::CamLook:
            return RunCamPose(cmd);

        case CommandId::CamRelease:
            return RunCamRelease(cmd);

        case CommandId::CamFlight:
            return RunCamFlight(cmd);

        case CommandId::Help:
        case CommandId::Info:
        case CommandId::Diag:
        case CommandId::None:
            return Say(false, "%s is answered immediately and should not have been queued",
                       CommandName(cmd.id));
    }
    return Say(false, "%s: unhandled", CommandName(cmd.id));
}

// ★ PERF-1. StatusLine() and WriteDiagnostics() are the whole reason the playback tick pays for the
// in-frame percentile work: each chains into inframe::Read(), which walks two 512-bucket histograms
// (~3-4k relaxed atomic loads), and WriteDiagnostics then runs a ~45-argument, ~1.5 KB snprintf. They ran
// on every tick to fill two strings that nothing reads until a human types `diag`/`info` or something
// fetches /replay/status. Rebuilt on a cadence and carried forward; a quarter second of staleness in a
// diagnostic string is not observable by either consumer.
constexpr uint64_t kUiTextPeriodMs = 250;
char     g_uiStatusText[sizeof(Snapshot::uiStatus)] = {0};
char     g_uiDiagText[sizeof(Snapshot::uiDiagnostics)] = {0};
uint64_t g_uiTextBuiltMs = 0;

void Publish() {
    Snapshot s;

    if (g_services != nullptr) {
        s.transportPresent = g_services->transport != nullptr;
        s.sessionPresent = g_services->session != nullptr;
        // The CONTROL interface, which is what `cam` calls -- not ReplayServices::camera, which is
        // the pose sink. Reporting the pose sink here would say "camera present" while the thing
        // the command needs was absent.
        s.cameraPresent = camera_control::Get() != nullptr;
        if (s.transportPresent) {
            IReplayTransport* t = g_services->transport;
            s.transportState = static_cast<int32_t>(t->State());
            s.rate = t->Rate();
            s.playheadUs = t->PlayheadUs();
            s.durationUs = t->DurationUs();
            CopyStr(s.modeLabel, sizeof s.modeLabel, t->ActiveModeLabel());
        }
        if (s.sessionPresent) {
            s.sessionStateRaw = static_cast<int32_t>(g_services->session->State());
            CopyStr(s.sessionStrategy, sizeof s.sessionStrategy, g_services->session->Strategy());
        }
    }

    // Every replay::ui read happens here, on the tick, and is rendered later from the copy.
    s.panelVisible = ui::PanelVisible();
    s.browserOpen = ui::BrowserOpen();
    s.asSeen = ui::AsSeen();
    s.uiDegradations = ui::DegradationCount();
    s.rateCeiling = ui::RateCeiling();
    s.roundsKnown = ui::RoundsKnown();
    s.roundCount = ui::RoundCount();
    const uint64_t nowMs = GetTickCount64();
    if (g_uiTextBuiltMs == 0 || nowMs - g_uiTextBuiltMs >= kUiTextPeriodMs) {
        g_uiTextBuiltMs = nowMs;
        CopyStr(g_uiStatusText, sizeof g_uiStatusText, ui::StatusLine());
        ui::WriteDiagnostics(g_uiDiagText, sizeof g_uiDiagText);
    }
    CopyStr(s.uiStatus, sizeof s.uiStatus, g_uiStatusText);
    CopyStr(s.uiDiagnostics, sizeof s.uiDiagnostics, g_uiDiagText);

    for (;;) {
        const uint32_t before = g_surfaceSeq.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        s.consoleUp = g_console.up;
        CopyStr(s.consoleNote, sizeof s.consoleNote, g_console.note);
        s.httpUp = g_http.up;
        CopyStr(s.httpNote, sizeof s.httpNote, g_http.note);
        s.launchRequested = g_openReq.up;
        s.launchRefusals = g_openReq.refusals;
        CopyStr(s.launchNote, sizeof s.launchNote, g_openReq.note);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_surfaceSeq.load(std::memory_order_relaxed) == before) {
            break;
        }
    }

    s.queued = g_queued.load(std::memory_order_relaxed);
    s.executed = g_executed.load(std::memory_order_relaxed);
    s.refused = g_queue.Refused();
    s.declined = g_declined.load(std::memory_order_relaxed);
    s.lastSeq = g_lastSeq;
    CopyStr(s.lastResult, sizeof s.lastResult, g_lastResult);
    CopyWide(s.loadedPath, kMaxPathChars, ui::LoadedPath());
    s.notesState = static_cast<int32_t>(g_notesState);
    s.bookmarkCount = g_notesState == notes::LoadResult::Ok ? g_notes.BookmarkCount() : 0;
    s.resumeUs = g_notesState == notes::LoadResult::Ok ? g_notes.LastPlayheadUs() : 0;

    const egress::Report& e = egress::Current();
    s.egressArmed = e.state == egress::ArmState::Armed;
    CopyStr(s.egressTrigger, sizeof s.egressTrigger, egress::ArmTriggerName(e.trigger));

    const uint32_t seq = g_seq.load(std::memory_order_relaxed);
    g_seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_snapshot = s;
    std::atomic_thread_fence(std::memory_order_release);
    g_seq.store(seq + 2, std::memory_order_release);
}

}  // namespace

void Init(ReplayServices* services) {
    g_services = services;
    g_lastResult[0] = 0;
    g_notes.Clear();
    g_notesState = notes::LoadResult::Absent;
    g_notesFor[0] = 0;
    g_notesKnown = false;
    Publish();
}

void Shutdown() {
    g_services = nullptr;
    g_notes.Clear();
    g_notesFor[0] = 0;
    g_notesKnown = false;
}

CommandQueue& Queue() { return g_queue; }

bool IsImmediate(CommandId id) {
    return id == CommandId::Help || id == CommandId::Info || id == CommandId::Diag ||
           id == CommandId::None;
}

const wchar_t* LoadedPath() { return ui::LoadedPath(); }

bool TakeResult(uint64_t seq, Result* out) {
    if (out == nullptr || seq == 0) {
        return false;
    }
    const DoneSlot& slot = g_done[seq % kDoneSlots];
    for (;;) {
        const uint64_t before = slot.seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;   // a result is being written into this slot
        }
        if ((before >> 1) != seq) {
            return false;   // not landed yet, or already displaced by a later command
        }
        *out = slot.result;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.seq.load(std::memory_order_relaxed) == before) {
            return true;
        }
    }
}

Result Submit(const Command& cmd) {
    if (cmd.id == CommandId::None) {
        Result r;
        r.ok = true;
        return r;
    }
    if (IsImmediate(cmd.id)) {
        Snapshot snap;
        ReadSnapshot(&snap);
        Result r;
        r.ok = true;
        if (cmd.id == CommandId::Info) {
            FormatInfo(snap, r.text, sizeof r.text);
        } else if (cmd.id == CommandId::Diag) {
            FormatDiag(snap, r.text, sizeof r.text);
        } else {
            // Help is a rendering of the shared table, and the surfaces render it in their own
            // format. Nothing to say here.
            r.text[0] = 0;
        }
        return r;
    }

    Command stamped = cmd;
    stamped.seq = g_nextSeq.fetch_add(1, std::memory_order_relaxed);
    if (!Queue().Push(stamped)) {
        return Say(false,
                   "the command queue is full (%zu deep) and %llu command(s) have been refused. "
                   "The tick is not draining it.",
                   CommandQueue::kCapacity,
                   static_cast<unsigned long long>(Queue().Refused()));
    }
    g_queued.fetch_add(1, std::memory_order_relaxed);

    // Accepted, not done -- and the number is how the caller finds out which. Saying "seeked" from
    // a thread that has only queued a seek would be a claim the caller cannot check; saying
    // nothing ever again, which is what this used to do, leaves a failed load looking like a
    // successful one.
    Result r = Say(true, "%s accepted", CommandName(cmd.id));
    r.seq = stamped.seq;
    return r;
}

uint32_t Tick() {
    uint32_t ran = 0;
    Command cmd;
    // Bounded per tick. An unbounded drain would let a script's burst turn one frame into a hitch,
    // which is the same defect W1-K's "never block in a handler" rule exists to prevent.
    // Before the drain, so a command that arrives in the same tick as a load taken from the panel
    // sees that capture's bookmarks rather than the previous one's.
    RefreshNotes(false);
    // ★ AND A PUSH IS OWED MORE OFTEN THAN THE PATH CHANGES. The panel resets itself to
    // NotSupplied on every LoadPath -- success or refusal -- and on Unload, so reloading the SAME
    // capture leaves it saying "nobody has told us" while our cache is unchanged and RefreshNotes
    // returns early. BookmarksState() is the seam's own way to ask whether our push arrived, which
    // is what W2-F built it for; one enum read per tick closes every reset path without this file
    // having to know which ui calls clear.
    if (g_notesKnown && ui::BookmarksState() == ui::NotesState::NotSupplied) {
        PushBookmarks();
    }
    while (ran < 16 && Queue().Pop(&cmd)) {
        const Result r = Execute(cmd);
        CopyStr(g_lastResult, sizeof g_lastResult, r.text);
        g_lastSeq = cmd.seq;
        FileResult(cmd.seq, r);
        log::Direct(r.ok ? LogLevel::Info : LogLevel::Warn, kReplayModuleName, "%s", r.text);
        g_executed.fetch_add(1, std::memory_order_relaxed);
        if (!r.ok) {
            // A command that RAN and was turned down. `refused` counts only the pushes a full
            // queue rejected, so without this the surfaces reported "refused 0" through a run of
            // refusals the owner was watching happen.
            g_declined.fetch_add(1, std::memory_order_relaxed);
        }
        ++ran;
    }
    Publish();
    return ran;
}

void ReadSnapshot(Snapshot* out) {
    if (out == nullptr) {
        return;
    }
    for (;;) {
        const uint32_t before = g_seq.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;   // a publish is in progress
        }
        *out = g_snapshot;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_seq.load(std::memory_order_relaxed) == before) {
            return;
        }
    }
}

void SetConsoleState(bool up, const char* note) {
    const uint32_t seq = g_surfaceSeq.load(std::memory_order_relaxed);
    g_surfaceSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_console.up = up;
    CopyStr(g_console.note, sizeof g_console.note, note);
    std::atomic_thread_fence(std::memory_order_release);
    g_surfaceSeq.store(seq + 2, std::memory_order_release);
}

void SetHttpState(bool up, const char* note) {
    const uint32_t seq = g_surfaceSeq.load(std::memory_order_relaxed);
    g_surfaceSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_http.up = up;
    CopyStr(g_http.note, sizeof g_http.note, note);
    std::atomic_thread_fence(std::memory_order_release);
    g_surfaceSeq.store(seq + 2, std::memory_order_release);
}

void SetLaunchState(bool requested, uint32_t refusals, const char* note) {
    const uint32_t seq = g_surfaceSeq.load(std::memory_order_relaxed);
    g_surfaceSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_openReq.up = requested;
    g_openReq.refusals = refusals;
    CopyStr(g_openReq.note, sizeof g_openReq.note, note);
    std::atomic_thread_fence(std::memory_order_release);
    g_surfaceSeq.store(seq + 2, std::memory_order_release);
}

void FormatInfo(const Snapshot& s, char* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return;
    }
    if (!s.transportPresent) {
        snprintf(out, cap, "no transport; ui: %s", s.uiStatus);
        return;
    }
    static const char* const kStates[] = {"pause", "play", "scrub", "seek", "step"};
    const char* state =
        (s.transportState >= 0 && s.transportState < 5) ? kStates[s.transportState] : "?";
    // The ceiling is printed as "unknown" when it is unknown. Printing 4.0 there would be right
    // only at 60 Hz and wrong on the machine the user is actually running.
    char ceiling[24];
    if (s.rateCeiling > 0.0f) {
        snprintf(ceiling, sizeof ceiling, "%.3g", static_cast<double>(s.rateCeiling));
    } else {
        CopyStr(ceiling, sizeof ceiling, "unknown");
    }
    snprintf(out, cap,
             "%s %.4gx (ceiling %s)  %lld / %llu us  mode %s  session %d (%s)  ui: %s", state,
             static_cast<double>(s.rate), ceiling, static_cast<long long>(s.playheadUs),
             static_cast<unsigned long long>(s.durationUs),
             s.modeLabel[0] != 0 ? s.modeLabel : (s.asSeen ? "asseen" : "ground"),
             s.sessionStateRaw, s.sessionStrategy[0] != 0 ? s.sessionStrategy : "-", s.uiStatus);
}

void FormatDiag(const Snapshot& s, char* out, size_t cap) {
    // One value per line rather than one eighteen-argument format string. The monolithic version of
    // this function shifted its arguments by one when a line was added, which reads an int32 as a
    // char* -- the compiler caught it here, and this shape cannot express the bug.
    TextOut t(out, cap);
    t.Line("control surfaces\n");
    t.Line("  console   %-5s %s\n", s.consoleUp ? "up" : "down",
           s.consoleNote[0] != 0 ? s.consoleNote : "-");
    t.Line("  http      %-5s %s\n", s.httpUp ? "up" : "down",
           s.httpNote[0] != 0 ? s.httpNote : "-");
    t.Line("  open      %-5s refusals %u  %s\n", s.launchRequested ? "asked" : "-",
           s.launchRefusals, s.launchNote[0] != 0 ? s.launchNote : "-");
    t.Line("  commands  queued %llu, executed %llu, refused at the queue %llu, declined when run "
           "%llu\n",
           static_cast<unsigned long long>(s.queued),
           static_cast<unsigned long long>(s.executed),
           static_cast<unsigned long long>(s.refused),
           static_cast<unsigned long long>(s.declined));
    t.Line("  last      %s\n", s.lastResult[0] != 0 ? s.lastResult : "-");

    t.Line("services\n");
    t.Line("  transport %s\n", s.transportPresent ? "present" : "absent");
    t.Line("  session   %s (state %d)\n",
           s.sessionPresent ? (s.sessionStrategy[0] != 0 ? s.sessionStrategy : "present")
                            : "absent",
           s.sessionStateRaw);
    // Named, not just absent: "absent" alone sends the reader looking for a bug in the command.
    t.Line("  camera    %s\n", s.cameraPresent ? "spectator rig checked out" : "rig has not checked out");
    t.Line("  egress    %s (%s)\n", s.egressArmed ? "armed" : "NOT armed",
           s.egressTrigger[0] != 0 ? s.egressTrigger : "-");

    t.Line("playback\n");
    if (s.loadedPath[0] != 0) {
        t.Line("  loaded    %ls\n", s.loadedPath);
    } else {
        t.Line("  loaded    nothing through a control surface\n");
    }
    // 0.0 is UNKNOWN and is printed as such. Printing 4.0 there would be right at 60 Hz only, and
    // wrong on the machine the reader is actually running (uiapi.h).
    if (s.rateCeiling > 0.0f) {
        t.Line("  ceiling   %.3gx\n", static_cast<double>(s.rateCeiling));
    } else {
        t.Line("  ceiling   unknown (no frame period measured yet)\n");
    }
    if (s.roundsKnown) {
        t.Line("  rounds    %zu\n", s.roundCount);
    } else {
        t.Line("  rounds    unknown - the manifest has not been read\n");
    }
    // The sidecar, so a bookmark is visible from a surface rather than only from the file.
    const auto notesState = static_cast<notes::LoadResult>(s.notesState);
    if (notesState == notes::LoadResult::Ok) {
        t.Line("  bookmarks %zu (`marks` lists them)%s\n", s.bookmarkCount,
               s.resumeUs != 0 ? ", resume point recorded" : "");
    } else if (notesState == notes::LoadResult::Absent) {
        t.Line("  bookmarks none yet\n");
    } else {
        t.Line("  bookmarks unavailable - %s\n", notes::LoadResultName(notesState));
    }

    t.Line("ui\n%s", s.uiDiagnostics[0] != 0 ? s.uiDiagnostics : "  (no diagnostics)\n");

    // Truncation is stated, not left to look like the end of the list. The marker overwrites the
    // tail rather than being appended, because by definition there is no room to append.
    if (t.Truncated() && cap > 24) {
        CopyStr(out + cap - 20, 20, "\n[diag truncated]\n");
    }
}

}  // namespace dispatch
}  // namespace control
}  // namespace replay
