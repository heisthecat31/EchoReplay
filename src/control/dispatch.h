#pragma once

// W2-G. The one place a command actually runs, and the snapshot every surface reads from.
//
// ============================================================================================
// READS ANSWER IMMEDIATELY. WRITES GO ON THE QUEUE. That split is W1-K's rule made concrete.
//
// W1-K measured that the engine's HTTP pump runs inside CR15Game::Update AFTER all three plugin
// Update slots have run, so a seek applied inside a handler lands mid-frame, after our tick already
// computed this frame's state. Its rule: queue the mutation, drain it at the top of the next tick.
//
// A read is different, and only because of what it reads. `info` and `diag` do not touch the
// transport, the UI or the engine at all -- they format a Snapshot this module published from its
// own tick. So they can be answered on the calling thread, in the handler, without violating
// anything.
//
// The consequence worth stating: a mutating command's REPLY says it was accepted, not that it
// happened. Reporting "seeked" from a thread that has only queued a seek would be a claim the
// caller cannot check.
//
// ★★★ THAT IS A REASON NOT TO CLAIM SUCCESS. IT IS NOT A REASON TO STAY SILENT, and this module
// used to read it as one. Every mutating command answered `<name> accepted` and the real outcome
// went to the log and to a single-slot `last` line -- so a load of a file that is not there looked
// exactly like a load that worked, and the refusal RunSpeed goes to such trouble to phrase reached
// nobody.
// Submit now stamps each queued command with a SEQUENCE NUMBER and returns it; Tick writes the
// finished Result into a small ring under that number; and the surfaces collect it the way each
// one can. The console waits briefly on its own thread, because it has one. The HTTP handler must
// never block, so it answers 202 with the sequence number and the client reads the result
// back from /replay/result or /replay/status.
// ============================================================================================
//
// * EVERY replay::ui CALL HAPPENS ON THE TICK. uiapi.h promises its functions are safe before the
// facade binds and after shutdown; it does not promise they are safe from another thread. So the
// console thread never calls one. Everything a surface needs to READ is captured into the Snapshot
// during the tick's publish, including replay::ui::WriteDiagnostics, and rendered from there.
//
// * FOUR COMMANDS ROUTE THROUGH replay::ui RATHER THAN THE TRANSPORT, and uiapi.h says why: the
// engine stops accelerating above 66666 / raw_frame_us without reporting it, so a console that
// called Play(8.0) would look like it worked; a percentage seek needs the duration; and B.8's
// SeekRound(int) does not say whether its argument is a label or an array position, which diverge
// on a capture that starts mid-match. Going through the seam the panel uses is what stops the two
// surfaces disagreeing.

#include <stddef.h>
#include <stdint.h>

#include "bootstrap/services.h"
#include "control/cmdqueue.h"
#include "control/command.h"

namespace replay {
namespace control {
namespace dispatch {

// One answer back to whoever issued the command. Sized for `diag`, which is the only multi-line
// one; every other result is a single line and most are far shorter.
inline constexpr size_t kResultChars = 2048;

struct Result {
    bool ok = false;
    // Non-zero for a command that was QUEUED: the number to collect its outcome under. Zero for an
    // immediate command, whose answer is already in `text`, and for a refusal at submission.
    uint64_t seq = 0;
    char text[kResultChars] = {0};
};

// What the surfaces report, published once per tick. Plain data with no pointers into anything the
// tick owns, so a reader on another thread cannot be looking at freed or half-written state.
struct Snapshot {
    bool     transportPresent = false;
    bool     sessionPresent = false;
    bool     cameraPresent = false;
    int32_t  transportState = -1;     // TransportState, or -1 when there is no transport
    float    rate = 0.0f;
    int64_t  playheadUs = 0;
    uint64_t durationUs = 0;
    char     modeLabel[64] = {0};     // IReplayTransport::ActiveModeLabel

    int32_t  sessionStateRaw = 0;
    char     sessionStrategy[16] = {0};

    bool     panelVisible = false;
    bool     browserOpen = false;
    bool     asSeen = false;
    uint32_t uiDegradations = 0;
    float    rateCeiling = 0.0f;      // 0 means UNKNOWN, never 4.0 (uiapi.h)
    bool     roundsKnown = false;
    size_t   roundCount = 0;
    char     uiStatus[192] = {0};     // replay::ui::StatusLine, verbatim
    char     uiDiagnostics[1024] = {0};

    // The control surfaces' own state, so `diag` can say what each one refused rather than being
    // silent about the surface the user is standing on.
    bool     consoleUp = false;
    char     consoleNote[160] = {0};
    bool     httpUp = false;
    char     httpNote[160] = {0};
    // The launch request ECHOREPLAY_OPEN carried, if any. Three states rather than two: `diag` has
    // to be able to say "a capture was named and refused", which is neither "absent" nor "open".
    bool     launchRequested = false;
    uint32_t launchRefusals = 0;
    char     launchNote[160] = {0};

    uint64_t queued = 0;
    uint64_t executed = 0;
    // Pushes the queue refused because it was full. Submit never retries, so each one is a user
    // command that did not run and was told so on the spot.
    uint64_t refused = 0;
    // ★ A DIFFERENT REFUSAL, COUNTED SEPARATELY, because one counter reported zero after five
    // straight refusals. `refused` only ever advanced on a full queue; a command that ran and was
    // turned down -- a rate above the ceiling, a round that does not exist, a service that has not
    // registered -- moved nothing at all, and `diag` printed "refused 0" while the surface was
    // refusing everything the owner typed.
    uint64_t declined = 0;
    uint64_t lastSeq = 0;
    char     lastResult[256] = {0};
    bool     egressArmed = false;
    char     egressTrigger[24] = {0};
    wchar_t  loadedPath[kMaxPathChars] = {0};

    // The notes sidecar of whatever is loaded, so `info`, `diag` and /replay/status can say that
    // bookmarks exist without any surface reading a file off the tick. notesState is a
    // notes::LoadResult; it is carried as an int so this header does not drag notes.h into every
    // reader of a Snapshot.
    int32_t  notesState = 0;
    size_t   bookmarkCount = 0;
    int64_t  resumeUs = 0;      // the sidecar's lastPlayheadUs; 0 when there is none
};

void Init(ReplayServices* services);
void Shutdown();

CommandQueue& Queue();

// True when the command can be answered from the published snapshot alone.
bool IsImmediate(CommandId id);

// Any thread. For an immediate command it produces the answer; for a mutating one it enqueues and
// says so. This is what a surface calls; nothing else pushes to the queue.
Result Submit(const Command& cmd);

// Tick thread only: drains the queue, runs each command, and publishes the snapshot. Returns how
// many commands ran.
uint32_t Tick();

// Any thread. Copies the last published snapshot.
void ReadSnapshot(Snapshot* out);

// Formatters, so the console and the HTTP surface agree on the answer even where they differ on
// the format. Both write a NUL-terminated string and never overrun.
void FormatInfo(const Snapshot& snap, char* out, size_t cap);
void FormatDiag(const Snapshot& snap, char* out, size_t cap);

// Set by the surfaces at init so `diag` can report on them. Copied, not borrowed.
void SetConsoleState(bool up, const char* note);
void SetHttpState(bool up, const char* note);

// The ECHOREPLAY_OPEN request's own state, on the same seqlock as the two surfaces above.
void SetLaunchState(bool requested, uint32_t refusals, const char* note);

// The path of the capture that is actually open, or null.
//
// ★ IT IS replay::ui's ANSWER, NOT A SECOND ONE OF OURS. This module used to keep its own
// g_loadedPath, written only by its own `load` -- so a capture opened from the panel left it empty
// and `mark` refused on the owner's most likely route, and a console `load A` followed by clicking
// B in the browser left it naming A, which is a bookmark written beside the wrong capture. W2-F
// built ui::LoadedPath() for exactly this and documented it as such; the fix was to call it.
// Tick thread only, like every other replay::ui read.
const wchar_t* LoadedPath();

// Collects the outcome of a queued command. False while it is still pending -- which includes a
// sequence number that has already been overwritten by later commands, so a caller that waits must
// bound its wait rather than assume the answer is still coming.
bool TakeResult(uint64_t seq, Result* out);

}  // namespace dispatch
}  // namespace control
}  // namespace replay
