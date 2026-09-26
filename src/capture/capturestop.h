// The second half: closing the capture on a stop, and PROVING it closed.
//
// F8 is the defect this exists for -- no launch in this project's history has ever run `Close()`, so
// F-REAL, the file L2 is launched to produce, would otherwise be a file with no index, no footer and an
// unrewritten header. C6's answer is in-process: a deadline (capturesecs.h) and a sentinel file the
// module polls, whichever comes first.
//
// ★ WHY THE CLOSE IS NOT THE WHOLE ANSWER. `Close()` returning is not evidence that a readable file is
// on disk -- the writer's own bookkeeping saying it wrote a footer and the footer being there are two
// different claims, and the recurring pattern is a checker carrying its own copy of a format and
// certifying broken files with it. So the stop path RE-OPENS the closed file with the shipped
// DemoReader -- the same reader playback will use -- and the sentinel carries that reader's verdict.
// A round trip through the real reader is the only claim worth making about a file nobody will look at
// again until L4.
//
// ★ WHY A SENTINEL FILE AT ALL, when the module already logs `closed: ...`. The log line is what the
// harness waits for before `taskkill` (H10) and it says what the WRITER counted. The sentinel says what
// the READER found, it is machine-readable, and it exists on disk after the process is gone. An absent
// sentinel and a `verified:false` sentinel are two different findings; a capture that stopped and left
// neither is the silence C6 exists to forbid.
#pragma once

#include "capture/chapterbuilder.h"   // EncodeRefusalNote, and the builder's own refusal limit
#include <stddef.h>
#include <stdint.h>

#include "bootstrap/paths.h"         // kPathMax: the module's path budget sizes the copied path
#include "capture/capture_types.h"   // StatsRefusals, carried into the sentinel
#include "capture/manifestbuild.h"   // TapRegistration, carried into the sentinel
#include "capture/markers.h"         // MarkerDeriver::Stats, carried into the sentinel
#include "capture/identity_emit.h"   // the producer's refusal vocabulary, carried into the sentinel
#include "capture/roster.h"          // RosterRefusals, carried into the sentinel

namespace replay {
namespace capture {

// ★ WHY THE SENTINEL IS NAMED AFTER THE LAUNCH (AUDIT_D_SC7 D-4). A fixed `capture_stop.json` in a
// directory every launch shares is a document with no owner: a launch killed before its stop leaves the
// PREVIOUS launch's `verified:true` sitting there, and a reader that tests only its fields reports the
// old launch's success as this one's. So the launch's identity is in the document AND in the file name,
// and the stop directory is swept at Init: after that sweep the only sentinel that can exist is this
// launch's, and an absent one means the capture never finalised.
extern const wchar_t kStopSentinelPrefix[];    // capture_stop.
extern const wchar_t kStopSentinelSuffix[];    // .json
extern const wchar_t kStopSentinelPattern[];   // capture_stop.*  (the Init sweep, and the tmp files)

// ★ WHY A SECOND COPY (AUDIT_G G-8). The sweep above is what makes an absent sentinel mean "this
// launch never finalised" -- but it also means the ONE artefact of launch N is destroyed by launch
// N+1's Init, and nothing in the harness copies it out today. So every stop writes its document
// twice: once at the swept path the reader polls, and once into this subdirectory, which the sweep's
// `capture_stop.*` pattern cannot match (no dot after `capture_stop`) and which nothing ever deletes.
// The archived name carries the launch, so the copies of many launches coexist; the harness copies
// the one it wants into `<runDir>\launch-NN` after the process is gone. Never the reader's input: a
// verdict is read only off the swept path, or the archive would resurrect exactly the stale-document
// read D-4 closed.
extern const wchar_t kStopArchiveDirName[];    // capture_stop_archive

// This launch's identity: the process CREATION time as a FILETIME. Nothing carries a launch id into
// the plugin -- H17 closes the child's environment block at exactly ECHOREPLAY_OPEN and
// ECHOREPLAY_CAPTURE_SECS, and no registered switch survives ApplyCommandLine -- so the id is derived
// from what the process already is. The harness can compute the same pair for the child it started
// (`$proc.Id` and `$proc.StartTime` as a FILETIME), so both ends can name the same launch without a
// third carrier. Falls back to the current time if GetProcessTimes refuses; never returns 0.
//
// `fromProcessCreation` (optional) says which of the two it is. The fallback is an id the harness
// CANNOT reproduce from `$proc.StartTime`, so a stop that used it would read as a stamp mismatch --
// the plan's CAPTURE-STALE-SENTINEL, a loud wrong answer. The caller logs the substitution instead
// (AUDIT_G G-10), so the mismatch is explained on the launch that caused it.
uint64_t LaunchFileTime(bool* fromProcessCreation = nullptr);

// `capture_stop.<launchFileTime as 16 upper-case hex>.<pid>.json`. False if it does not fit.
//
// This is the LAUNCH-scoped name: one path a reader can poll without knowing how many times the
// launch closed. It is replaced by every close, so the document under it names the LAST close --
// which is why it carries `closeSeq` and `file`. Never treat it as the launch's only sentinel.
bool StopSentinelNameFor(uint64_t launchFileTime, uint32_t pid, wchar_t* out, size_t cap);

// ★ `capture_stop.<launchFileTime as 16 upper-case hex>.<pid>.<closeSeq as 3 digits>.json`
// -- the PER-CLOSE name, and the one that carries the invariant "every closed file has exactly one
// sentinel that survives every later close". Capture is always on (locked constraint 6), so a launch
// that runs past a match closes more than once: L5 run 3 closed a 3287-record file and re-opened
// 47 ms later. Under one launch-scoped name the second close overwrote the first, and the match that
// was actually captured ended the launch with no sentinel of its own. `closeSeq` counts closes within
// the launch from 1, zero-padded so the names sort in the order they were written; the document's
// `file` field is what a harness correlates on. False if it does not fit.
bool StopSentinelNameForClose(uint64_t launchFileTime, uint32_t pid, uint32_t closeSeq, wchar_t* out,
                              size_t cap);

// Reports one file the sweep tried: `deleted` false means it is still there. The plan asks for each
// deletion to be logged, and a survivor is the one case where the detail decides a verdict, so the
// sweep names files rather than only counting them (AUDIT_G G-11).
typedef void (*StopSweepReport)(const wchar_t* name, bool deleted, void* ctx);

// Deletes every `capture_stop.*` in `dir` and returns how many went. Call at Init, BEFORE this launch
// has written one: after it, a sentinel in that directory belongs to this launch or to nobody.
// `failures` (optional) counts the ones that would not delete -- a sentinel that survives the sweep is
// a stale document that can still be read as this launch's, so the count is reported, not swallowed.
// `report` (optional) is called once per file, before the count is returned.
uint32_t DeleteStaleStopSentinels(const wchar_t* dir, uint32_t* failures,
                                  StopSweepReport report = nullptr, void* ctx = nullptr);

// The name the harness CREATES to ask for a close (C6's second trigger). Presence is the whole
// message; the file's contents are never read, so a zero-byte `echo. >` is a valid request.
extern const wchar_t kStopRequestName[];    // stop-capture

enum class StopReason : uint8_t {
    Deadline = 0,   // ECHOREPLAY_CAPTURE_SECS elapsed
    Request,        // the stop-capture sentinel appeared
    Shutdown,       // the worker is exiting (DLL detach, module shutdown)
    // ★ The match ended and the file was closed on its own -- no stop was ever
    // asked for. Capture is ALWAYS ON (locked constraint 6), so this is the ORDINARY way a capture
    // finishes, and it used to write no sentinel at all: L5 run 3 closed a real 3287-record file and
    // the harness recorded CAPTURE-NOT-ASKED because nothing was on disk to read. A close with no
    // evidence is indistinguishable from a capture that never ran.
    MatchEnd,
    // ★ S6.4's rung-5 kill switch closed the file. The match did NOT end -- it is still
    // being played -- so this close borrowed `match-end` and asserted a cause that did not happen.
    // A close that cannot name its cause must not take one that means something else; this is that
    // word. The trigger is the cost meter, not the game, and the sentinel says so.
    CostKill,
    // Init found capture.enabled false with a deadline asked for. No file was ever
    // opened and no worker ever ran, so this is the one close Init announces itself; the word exists
    // so that the `closed:` line the launcher waits for still has a document behind it.
    Disabled,
    // ★ A CLOSE THAT CLOSED NOTHING. L2's stop request and its deadline both arrived
    // at a capture the cost meter had closed 3.5 minutes earlier, and each wrote a full sentinel
    // naming that file with its bytes, its chapter count and `verified: true` -- three documents for
    // one close, two of them describing an event that did not happen. Only `fileClosed: false` said
    // so, and the reader did not read it, which dragged a run whose real closes were both CLEAN down
    // to PARTIAL. "I closed a file" and "there was nothing to close" are different events and the
    // sentinel now has a word for the second. The TRIGGER is not lost: `closeCauses` still carries
    // the request or the deadline that arrived, so the document says both what was asked and that
    // nothing answered. READERS: treat `reason: "no-op"` as evidence of a stop that met no
    // recording -- never as a close, and never as a capture outcome.
    NoOp,
};
const char* StopReasonName(StopReason r);

// One bit per cause in a close's `closeCauses` mask. Here rather than in capture.cpp because the
// sentinel writer renders the mask and must not carry a second copy of the mapping.
constexpr uint8_t CloseCauseBit(StopReason r) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(r));
}
static_assert(static_cast<uint8_t>(StopReason::Disabled) < 8, "closeCauses is a byte of bits");

// What re-opening the closed file with the shipped reader found.
struct ReopenVerdict {
    bool verified;        // the file is provably closed to EOF and readable through the fast path
    uint32_t chapters;    // the capture's own chapters as the READER counts them: the container's
                          // MANIFEST and INDEX chapters are excluded, so this compares to the writer
    uint64_t bytes;       // the file's size on disk
    char detail[192];     // always set: why it is not verified, or how it was verified
};

// Re-opens `path` and decides whether the container is closed. Verified requires ALL of: Open()
// succeeded, the trailer index was read without a recovery scan (fastPath), the footer is present, no
// bytes hang off the tail, at least one chapter is indexed, and the LAST chapter decodes -- the index
// claiming a chapter and the payload being there are separate facts, and a truncated tail is exactly
// where they come apart. Never faults on a missing, empty or garbage file; `out` is always filled.
void VerifyClosedFile(const wchar_t* path, ReopenVerdict* out);

// L3-RUN-13 F3. THE COST METER'S TAIL RUNG, AS COUNTS, ON A CLOSE THAT SURVIVED. `tailOverBudget`
// and the charged mean were published on the kill line and nowhere else, so the run that proves the
// amount term works -- the one where no file is closed -- was exactly the run that could not show
// the rung had been asked. Counters, not the last window's quantiles: what a reader needs from a
// survivor is how many windows the rung ran on, how many it could not be asked on, how many read
// over its line, and how many met the whole kill condition without reaching kKillWindows in a row.
// `measured` 0 beside `notRun` 0 means no window completed; it is not a pass.
struct CostTailFacts {
    uint32_t measured = 0;
    uint32_t notRun = 0;
    uint32_t overBudget = 0;
    uint32_t kills = 0;
    uint32_t chargedMeanUs = 0;
};

struct StopFacts {
    uint64_t launchFileTime;     // LaunchFileTime(): whose launch this sentinel is about (D-4)
    uint32_t pid;          // the process that wrote it, so the pair is checkable against the child
    const wchar_t* file;   // the capture just closed; null or empty means nothing was recording
    uint64_t records;      // the WRITER's counts, kept beside the reader's so a disagreement is visible
    uint64_t chapters;
    StopReason reason;
    // Which close of this launch this is, counted from 1. Part of the per-close sentinel's name and
    // written into the document, so a harness holding two sentinels knows their order without
    // comparing timestamps.
    uint32_t closeSeq;
    bool matchStillLive;   // a match was in progress when this stop closed its file
    // Whether this close also stopped a SECOND file being opened for the match that is still live.
    // The rule is one line: a close that leaves a match live suppresses a second file for THAT match
    // and nothing beyond it -- the latch clears when the match ends -- and it is stated here rather
    // than left to be inferred from `matchStillLive`, because a cost kill ending a live match's
    // capture must not be a silent side effect of the close.
    bool reopenSuppressed;
    // ★ Did this close actually close a file? A stop that met no recording still writes
    // its document -- a deadline that closed nothing is a fact the harness needs -- but `file` then
    // names the PREVIOUS capture, and that is the one document a reader could take for a second
    // close of it. With the close path's invariant (no early return may announce a close without a
    // sentinel), the `fileClosed: true` documents stand one per .echodemo the launch produced, so a
    // missing one is a missing close rather than a skipped write.
    bool fileClosed;
    // ★ The open-retry's degraded state, for a harness that reads documents and not
    // logs: how many opens failed over this launch (0 normally) and which stage last refused
    // ("none" once an open succeeded). A non-zero count with `fileClosed: false` is a launch that
    // could not record at all, and it says why.
    uint32_t openFailures;
    // The streak above is zeroed by the first successful open, so a launch that failed
    // for most of a match and then recovered closed with `openFailures: 0, openFailState: "none"` --
    // byte-identical to a launch that never failed, for the document-reading harness these fields
    // exist for. This one is never cleared by a success: it is how many opens failed over the WHOLE
    // launch, where `openFailures` is how many were failing when this close was taken.
    uint32_t openFailTotal;
    const char* openFailState;
    // EVERY cause that asked for this close, not only the one that took the word. A
    // cost kill and a match end can land in the same worker pass, and the loser used to be reported
    // in the log alone -- which this document's audience does not read.
    uint8_t closeCauses;
    // "n/a" on every close the worker writes: it is written before Shutdown attempts
    // the join, so the worker cannot know. "timed out" appears only on the close Shutdown writes
    // itself when the worker did not exit within its 2 s -- the one close the worker cannot report.
    const char* workerJoin;
    ReopenVerdict verdict;
    // ★ From the harness's end. How many of the capture's own surfaces were live when
    // this file was closed: without it a reader has no denominator, and "the file holds no events"
    // cannot be told from "no tap was ever registered". The keyframe block carries the same counts
    // for a reader of the FILE; this is the copy a harness that never opens the file can read.
    TapRegistration taps;
    // ★ AUDIT_M8 F2 / L2 F5. The marker deriver's own counters, for the same reason `taps` is here:
    // "the file holds no round markers" and "the netvar symbols we watch for are not the ones this
    // build publishes" are different faults that look identical from outside. `namesWatched` is the
    // denominator (zero means the watch table was never built at all), `namesSeenMask` names WHICH
    // symbols arrived, and `rowsSeen` separates a wrong symbol set from a stream that carried
    // nothing. Until this the answer was only in the log, which this document's audience does not
    // read.
    MarkerDeriver::Stats markers;
    // ARCH-21 S4.4. WHICH source emitted this file's markers, and -- separately -- whether the
    // corroborator was ever fed anything at all. `corroboratorAsked false` with zero rows is COULD
    // NOT ASK, which is the honest answer on a solo host and is a different fact from agreement.
    uint8_t markerSource = 0;  // MarkerSource; 0 == NotRecorded
    uint32_t localBoundaries = 0;
    bool corroboratorAsked = false;
    uint32_t wireRowsSeen = 0;
    // L2-RUN-3 F6. The DENOMINATOR the marker block was missing. `namesWatched 0` means the watch
    // table was never built and `namesSeen 0` with `rowsSeen > 0` means the symbols are wrong, but a
    // reader holding only the sentinel could not tell either of those from the third case -- the
    // deriver was never FED, which is not a capture defect at all. The counter existed on
    // BuilderStats and reached the log line only; it is on the document now.
    uint64_t netvarRecords = 0;

    // S5.4.7 / ruling Q-B. A stream that ships by default and is empty has to say WHICH reason it is
    // empty for, on the document and not only in the log. `notBound` is empty when a real producer
    // pair was bound; otherwise it names the producers that do not exist yet.
    uint64_t    oracleSamples = 0;
    uint64_t    oracleOk = 0;
    uint64_t    oracleDropped = 0;
    const char* oracleStatus = nullptr;
    const char* oracleNotBound = nullptr;
    // L2-RUN-3 F8. The file's own identity, so a sentinel can be tied to the capture it reports on.
    // Zero on both halves means no file was opened by this stop, which is why they are not one
    // string: an absent uuid must not be spelled as thirty-two zeros a reader can mistake for a value.
    uint64_t demoUuidLo = 0, demoUuidHi = 0;
    // WHY THE ROSTER IS THE SIZE IT IS. `rows 0` has three causes a count cannot separate: no identity
    // source at all, an entrant count the engine would not give up (`bandRefusals`), and a session
    // that really had nobody in it. And a row present but half filled has as many causes as it has
    // fields, so the refusals are carried BY REASON with the engine's own word for each --
    // "loadoutNumbers: 12 (BuildMismatch)" is a repair, "fill 0x1BF" is a shrug.
    // The round table is decided at CLOSE and filed on the writer, and
    // nothing outside the file said how many rows it got -- so the probe's round-table row could
    // only ever answer could-not-ask. Three states, not two: `roundRowsRecorded false` means this
    // close never reached the filing site (the sentinel omits the field entirely), while a recorded
    // ZERO means the table was reached and was empty. Collapsing those is the shrug this project
    // keeps finding.
    uint32_t roundRowCount = 0;
    bool roundRowsRecorded = false;
    // Boundaries the round table refused, and rows it re-based after the engine restarted its
    // counter. Both are rounds the file describes differently from the way they happened, and both
    // are readable ONLY from here: the table itself cannot carry a row it declined to write.
    uint32_t roundsRefusedNonAscending = 0;
    uint32_t roundsRebasedAfterReset = 0;
    // The last game-state phase this capture classified, as the engine's own word, and the last
    // round counter it read. Both carry their own absence word rather than a number a reader could
    // mistake for a measurement: "not-sampled" means no game-state sample ever reached the tick,
    // which on a solo host is the honest answer and is not a defect.
    const char* phase = nullptr;         // null renders as "not-sampled"
    uint32_t roundCounter = 0;
    bool roundCounterRead = false;
    uint16_t rosterRows = 0, rosterLocalRows = 0;
    uint32_t rosterBandRefusals = 0, rosterUnchanged = 0;
    // L2-RUN-5 F1's own number: ticks on which the cheap entrant-count gate said nothing moved, so
    // no scan ran. `unchangedRowScans` equal to the tick count is what the per-tick scan looked
    // like; this field is what the fix looks like, and the two together are the regression's oracle.
    uint32_t rosterSignalUnchanged = 0;
    // Entrants the engine counted past this table -- players who are in the match
    // and NOT in the file. It reached a log line and nothing else.
    uint32_t rosterBandClamped = 0;
    // Both had no reader outside the builder: a wire marker lost to the arm-up hold
    // and a round boundary filed without cutting a chapter are the two ways this file's marker table
    // can be short or blunt, and neither was observable from the sentinel.
    uint64_t wireMarksHeldDropped = 0, boundaryMarksNotCut = 0;
    RosterRefusals rosterRefusals = {};
    // The identity block producer's own numbers. `blocks 0` has five causes and a
    // count cannot separate them, so every refusal reason is carried by NAME beside the totals --
    // the same rule the roster's refusals above are here for. `deferred` is not a loss: those rows
    // were held back by the armed tick's document budget and are offered again on the next scan.
    uint32_t identityBlocks = 0, identityJsonRefused = 0, identityKeyless = 0;
    uint32_t identityTruncated = 0, identityTruncationUnknown = 0, identityDeferred = 0;
    uint32_t identityRefusals[kIdentityEmitRefusalCount] = {};
    const char* identityLastJsonWhy = nullptr;   // the engine's own word, never a number

    // ARCH-21 MAJOR-B2, the live scoreboard. `records 0` has the same three causes the roster's
    // `rows 0` has and a count cannot separate them: no source at all (`sourceAbsentTicks`), a stat
    // block the reader refused (`statsRefusals`, by reason, with the engine's own word), and a match
    // in which genuinely nothing moved. `signalUnchangedTicks` is the gate's own number -- ticks on
    // which NO engine read was issued -- and it is what makes a regression to a per-tick sweep
    // visible to a reader of this document rather than only to the cost meter.
    uint64_t statsRecords = 0, statsKeyframes = 0, statsSweeps = 0;
    uint64_t statsSignalUnchanged = 0, statsSourceAbsent = 0;
    uint16_t statsPlayers = 0;
    StatsRefusals statsRefusals = {};
    // From the document-reading end. S_BODY_SAMPLED is the only carrier of a
    // world position in the format and the sentinel said NOTHING about it: a close that recorded no
    // body was byte-identical to one that recorded every body, for the harness this document exists
    // for. `bodiesSampled` is the record count; the four source counters say WHICH of the two body
    // sources answered, so "history only" -- a recording of everyone except the owner -- is readable
    // rather than being a healthy-looking count. `notAsked` is not `none`: it means the entity
    // surface predates the detailed read, which is a fact about this build and not about the engine.
    uint32_t bodiesSampled = 0, bodiesRefused = 0;
    uint32_t bodiesLocal = 0, bodiesHistory = 0, bodiesNone = 0, bodiesNotAsked = 0;
    // WHICH GATE of the local body read refused, by BodyRefusal ordinal. `bodiesRefused` above is a
    // count with no cause: L2 #9 read `refused 1` every tick and five different engine conditions
    // produce it. Index 0 (None) is the healthy arm and is never published.
    uint32_t bodyRefusals[kBodyRefusalCount] = {};
    uint32_t bodyRefusalUnknown = 0;
    // WHICH ARM answered angular velocity, by AngVelSource ordinal. A pose-only read carries a
    // structural zero omega and a history node carries a measurement; both are three zero-ish
    // floats in the file, and until this the sentinel could not tell them apart. Index 0
    // (NotAsked) IS published: unlike the refusal histogram's None it is a statement -- those
    // bodies reached no arm at all.
    uint32_t angVelBy[kAngVelSourceCount] = {};
    uint32_t angVelUnknown = 0;
    CostTailFacts costTail = {};
    // WHAT THE GATES SAW at the first refused body read of the capture. `bodiesRefused` is a
    // count and `bodyRefusals` names the arm; neither carries the numbers behind that arm, so
    // a reader still cannot tell a slot the reader never got to ask about from one the engine
    // itself would refuse. False means nothing refused, and the sentinel then omits the key
    // rather than publishing a block of zeros a reader would score as a measurement.
    bool bodyFirstRefusalSeen = false;
    replay::BodyReading bodyFirstRefusal{};
    // WHICH FIELD the WRITE could not store. `bodyRefusals` above names the gate of the local body
    // READ; this is the other half of L2 #15's bare `7080 refusals`, which said nothing about the
    // field, its value or the width the format declares for it. Bounded by the builder's own limit.
    uint32_t encodeRefusalsNamed = 0;
    EncodeRefusalNote encodeRefusalNotes[BuilderStats::kMaxRefusalsNamed] = {};
};

// ★ THE IN-PROCESS READER. Until this, the ONLY way to read
// what a close decided was to parse the sentinel FILE -- which is what tests/replay's probe did,
// with its own JSON reader, against a document it could truncate. A probe that parses a document to
// learn what the module in its own address space just did is reading a photocopy.
//
// This is a SNAPSHOT, not a view: every string is copied into the struct, so a caller holds no
// pointer into module state and nothing it reads can be rewritten under it by the next close. It is
// published from WriteStopSentinel -- the SWEPT path, the same one D-4 makes the only readable
// verdict -- so the in-process answer and the on-disk answer are the same close by construction.
// False means no close has published yet, which is a different fact from a close with zero records.
struct StopFactsSnapshot {
    wchar_t file[kPathMax];
    char openFailState[32];
    char workerJoin[32];
    char phase[64];
    uint64_t launchFileTime;
    uint64_t records, chapters;
    uint64_t netvarRecords;
    uint64_t wireMarksHeldDropped, boundaryMarksNotCut;
    uint64_t statsRecords, statsKeyframes, statsSweeps;
    uint64_t statsSignalUnchanged, statsSourceAbsent;
    uint64_t demoUuidLo, demoUuidHi;
    uint32_t pid;
    uint32_t closeSeq;
    uint32_t openFailures, openFailTotal;
    uint32_t localBoundaries, wireRowsSeen;
    uint32_t roundRowCount, roundCounter;
    uint32_t roundsRefusedNonAscending, roundsRebasedAfterReset;
    uint32_t rosterBandRefusals, rosterUnchanged, rosterSignalUnchanged, rosterBandClamped;
    uint32_t identityBlocks, identityJsonRefused, identityKeyless;
    uint32_t identityTruncated, identityTruncationUnknown, identityDeferred;
    uint32_t identityRefusals[kIdentityEmitRefusalCount];
    const char* identityLastJsonWhy;
    uint16_t rosterRows, rosterLocalRows;
    uint16_t statsPlayers;
    uint8_t  markerSource;
    uint8_t  reason;          // StopReason
    uint8_t  closeCauses;
    bool corroboratorAsked;
    bool matchStillLive, reopenSuppressed, fileClosed;
    bool roundRowsRecorded, roundCounterRead;
    bool phaseSampled;        // false means no game-state sample ever reached the tick
    ReopenVerdict verdict;
    TapRegistration taps;
    MarkerDeriver::Stats markers;
    RosterRefusals rosterRefusals;
    StatsRefusals statsRefusals;
    uint32_t bodiesSampled, bodiesRefused;
    uint32_t bodiesLocal, bodiesHistory, bodiesNone, bodiesNotAsked;
    uint32_t bodyRefusals[kBodyRefusalCount];
    uint32_t bodyRefusalUnknown;
    bool bodyFirstRefusalSeen;
    replay::BodyReading bodyFirstRefusal;   // a value, like every other field here
    uint32_t encodeRefusalsNamed;
    EncodeRefusalNote encodeRefusalNotes[BuilderStats::kMaxRefusalsNamed];
};

// The last close this PROCESS published, copied into `out`. False -- and `out` untouched -- when no
// close has happened yet. Safe to call from any thread: the publish and this read are serialised.
bool LastStopFacts(StopFactsSnapshot& out);

// Writes `dir`\capture_stop.<launchFileTime>.<pid>.json, the name built from `facts`. Written to a temporary
// name, flushed, and moved into place, so a harness that reads it the instant it appears never sees
// half a document. Returns false and names the failure in `err` (always NUL-terminated when errCap>0).
bool WriteStopSentinel(const wchar_t* dir, const StopFacts& facts, char* err, size_t errCap);

// The same document under the PER-CLOSE name, and the one write that carries AUDIT_M5 F2's invariant:
// it REFUSES rather than replaces if that name is already on disk, so no close can destroy an earlier
// close's evidence. `err` says so by name when it refuses.
bool WriteStopSentinelForClose(const wchar_t* dir, const StopFacts& facts, char* err, size_t errCap);

// The same document under the PER-CLOSE name, into `dir`\capture_stop_archive\ (created if absent).
// This copy outlives the next launch's Init sweep so the harness can collect it after the process is
// gone (AUDIT_G G-8), and it accumulates: one file per close, never replaced. Same
// failure contract as WriteStopSentinel.
bool ArchiveStopSentinel(const wchar_t* dir, const StopFacts& facts, char* err, size_t errCap);

// Pure: the directory part of `path`, without the trailing separator. False if there is none or it
// does not fit -- a capture path with no directory is not something to guess about.
bool DirectoryOfFile(const wchar_t* path, wchar_t* out, size_t cap);

// Does `dir`\stop-capture exist, and was it written by THIS launch? `notBefore` is a FILETIME as a
// uint64; a request last written before it is stale -- left in a launch directory by an earlier run --
// and is not a request. Without that test a single forgotten file would close the capture of every
// launch after it, which is RK-11's hazard in a second carrier. A path that cannot be built returns
// false: a request nobody could ask must not read as one that was made.
bool StopRequestPresent(const wchar_t* dir, uint64_t notBefore);

// * The capture module's own armed/recording state,
// published so a module that must not run while a capture is live can ASK instead of guessing from
// a launch carrier -- an environment variable can already be set machine-wide (RK-11), so the
// carrier cannot answer this and this does. TRUE from before the phase leaves Idle (Arming,
// Recording, Closing) until after it returns to Idle: armed and recording are one answer, because a
// seat issued in the arming window is still on the record when the file opens.
//
// capture.cpp's SetPhase is the ONLY writer, and it is the only place g.phase is stored.
void PublishCaptureRecording(bool recording);
bool CaptureIsRecording();

// RECORDER-2 part D. The manual door, for `record start|stop` and the bar's Record button. Both are
// called from the tick -- the command queue is drained there -- and neither opens or closes a file
// itself: start arms the same tick gate auto-record uses, stop asks for the same close the match
// end asks for. `RecordStatusLine` fills `out` with the line the owner is asked to paste.
void RequestManualRecordStart();
bool RequestManualRecordStop();
void RecordStatusLine(char* out, size_t cap);

// The SAME verdict the per-tick session gate applies, asked ahead of the arm so `record start` can
// refuse by name instead of promising a file the gate will never open.  False in an offline or
// contained playback session; `kind` is filled with the session kind to say so by name.
bool CaptureSessionRecordable(const char** kind);

}  // namespace capture
}  // namespace replay
