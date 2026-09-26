#pragma once

// S5.7's `S_MARK` records, derived from the netvar stream the capture is already carrying.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS IS DERIVED ON THE WORKER AND NOT GATHERED ON THE TICK.
//
// Every marker S5.7 lists is an EDGE in replicated state: a `game_state` symbol changing, a score
// incrementing, `first_launch_of_round` going 1 -> 0, the pause cluster moving. The netvar stream
// already carries every one of those changes, at the engine's own cadence, with the packet number that
// makes its completeness checkable. Polling the same state from the tick would be a second, slower,
// unpoliced view of data we already have -- and S6.2.1's rule is explicit that a 118 Hz stream cannot
// be observed by a 72-120 Hz poller.
//
// So a marker is a function of the S_NETVAR records, computed where they are decoded. That has a
// property worth stating: THE MARKERS IN A FILE ARE REPRODUCIBLE FROM THE FILE. A reader that
// disagrees with our marker table can recompute it from the stream and see which of us is wrong,
// which is not true of a marker we merely observed and wrote down.
//
// ★ WHAT THIS DOES NOT COVER, and it is most of the table. S5.7 lists eleven kinds. This derives the
// six that a netvar edge determines. `JOIN` / `LEAVE` / `TEAM_CHANGE` come from lobby messages and
// `BOOKMARK` is a playback-time sidecar entry (S5.9.2), so they are not derivable here and are NOT
// silently emitted as something else. `LAUNCH_RELEASE` needs a four-way conjunction within one rowset
// and is derived only when all four conditions are visible in the state we hold.
//
// ★ AND THE NAMES ARE TIER U. The CSymbol64 of each netvar comes from R4's recovery through the
// engine's own radhash string table. No PC read in this project re-derives them, and a wrong symbol
// here does not fail loudly -- it produces a file with no round markers, which looks like a match
// nobody scored in. MarkerDeriver::Stats therefore reports how many of the watched names were ever
// SEEN in the stream, so "no markers" and "we were watching for the wrong names" are distinguishable.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "capture/capture_types.h"
#include "capture/streamcodec.h"
#include "engine/gamestate_types.h"
#include "foundation/demofile/demotypes.h"
#include "foundation/demofile/format.h"

namespace replay {
namespace capture {

// S5.7's kinds. The numbering is the file's; a reader cites the name.
enum MarkKind : uint16_t {
    kMarkMatchBegin = 1,
    kMarkMatchEnd = 2,
    kMarkRoundBegin = 3,
    kMarkRoundEnd = 4,
    kMarkLaunchRelease = 5,
    kMarkGoal = 6,
    kMarkSuddenDeath = 7,
    kMarkOvertime = 8,
    kMarkMatchOver = 9,
    kMarkPauseBegin = 10,
    kMarkPauseEnd = 11,
    kMarkJoin = 12,        // not derivable here; listed so the numbering is stable
    kMarkLeave = 13,
    kMarkTeamChange = 14,
    kMarkBookmark = 15,
    // Not in S5.7's table. A `game_state` edge is a real, derivable event whose MEANING needs a symbol
    // vocabulary this project has not re-derived on PC, so it is recorded as what it is rather than
    // asserted to be one of the eleven named kinds. S5.7 calls S_MARK "round and phase markers", and
    // this is the phase half stated without a guess.
    kMarkPhase = 16,
};
const char* MarkKindName(uint16_t);

// ★ A MARKER WITH NO TIME CANNOT BE SEEKED TO, WHICH IS THE ENTIRE POINT OF A MARKER.
//
// UINT64_MAX is the engine's own local-event sentinel, and it reached L2's file as three
// markers at 18446744073709.551 s. The stamp is corrected at its source now, but a deriver that would
// file the sentinel if it ever saw it again is a deriver that cannot fail loudly. Refusing is the
// behaviour the recurring-defect list asks for: a tool meeting input it does not understand refuses
// rather than shrugs, and the refusal is counted so the file says the table is short.
inline bool MarkTimeTrusted(uint64_t tUs) { return tUs != UINT64_MAX; }

class MarkerDeriver {
public:
    struct Stats {
        uint32_t emitted;
        uint32_t rowsSeen;
        // One bit per watched name, set the first time that name appears in the stream. Zero here with
        // a non-zero rowsSeen means the symbols are wrong, which is a different fault from a quiet
        // match and must not read as one.
        uint32_t namesSeenMask;
        uint32_t namesWatched;
        // Markers derived but NOT filed because their source time was not a time. Zero on a healthy
        // capture; non-zero says the marker table is incomplete and names the reason.
        uint32_t timeRefused;
    };

    // ★ DIAG-L2: Reset() had no caller in src/, so on a capture that saw no netvar row at all the
    // table was never built and Stats reported `0 of 0 watched names`. The seen-mask exists precisely
    // to separate "no rounds occurred" from "the netvar symbols are wrong", and a denominator of zero
    // answers neither. Priming in the constructor makes namesWatched a property of the type rather
    // than of somebody remembering to call this.
    MarkerDeriver();
    void Reset();
    // A marker whose time could not be trusted, refused rather than filed. See MarkTimeTrusted.
    void NoteTimeRefused() { ++stats_.timeRefused; }
    // Feeds one decoded netvar message. Emits at most `cap` markers into `out` and returns how many.
    // `tUs` is the record's own source time, so a marker lands at the instant its edge did.
    uint32_t Note(const NetvarMessage&, uint64_t tUs, MarkRec* out, uint32_t cap);
    // The one marker no netvar edge can produce: the file's own boundaries.
    MarkRec MatchBegin(uint64_t tUs) const;
    MarkRec MatchEnd(uint64_t tUs) const;

    const Stats& GetStats() const { return stats_; }

private:
    struct Watched {
        uint64_t name;
        uint64_t value;
        // What this name held the first time we saw it. A capture that joined a match in progress has
        // a baseline that is not zero, and treating the first sighting as an edge would invent events.
        uint64_t baseline;
        bool seen;
    };
    static constexpr uint16_t kWatchedCount = 9;
    Watched watched_[kWatchedCount] = {};
    bool primed_ = false;
    uint16_t roundIndex_ = 0;
    Stats stats_ = {};

    int Index(uint64_t name) const;
};

// ---------------------------------------------------------------------------------------------
// ★ THE LOCAL SOURCE (ARCH-21 R2 S4.3), AND WHY THE ARGUMENT AT THE TOP OF THIS FILE IS NOT WRONG
// BUT IS NOT ENOUGH.
//
// Deriving markers where the netvar stream is decoded makes the table recomputable from the file,
// and that is still true. It just requires a netvar stream. On the owner's sanctioned `-offline`
// host there is none: a 51 s solo session delivered 1234 S_EVENT records through the same
// CBroadcaster and ZERO netvar rows (L2-RUN-2 F3), because a per-peer delta has no peer to
// serialise for. Every marker above is unreachable in the configuration the goal names, so ARCH-21
// D2 makes the LOCAL sample primary in EVERY capture role -- one source, one code path, one test
// set -- and demotes MarkerDeriver to a corroborator whose output is compared, never filed.
//
// The two derivers keep the same baseline rule (a first sighting is a value, not an edge), which is
// what makes the comparison in tests/capture/test_localmarkers.cpp mean anything.
class LocalMarkerSource {
public:
    struct Stats {
        uint32_t samples;          // samples whose availability was Ok, so a marker could be derived
        uint32_t samplesRefused;   // not Ok: nothing derived, and the span is recorded below
        uint32_t emitted;
        uint32_t localBoundaries;  // ROUND_BEGIN + ROUND_END -- what "capture by round played" costs out to
        // ARCH-21 C4. A round_counter outside [0, kMaxRoundNumber] REFUSES its boundary. It is not
        // truncated into a 16-bit round number, so no file ever claims round 1 for round 65537.
        uint32_t roundNumberOutOfRange;
        uint32_t phaseUnknown;     // symbol read, matched no row in W1-D's table: never a round
        uint32_t rowsDropped;      // rounds past kMaxRows; the markers still land, the table says short
        uint32_t pointsClamped;    // a score wider than the row's 16 bits, named rather than wrapped
        uint32_t marksDroppedForCap;  // Note() ran out of caller buffer; zero on every shipped path
        // The span between the last Ok sample and the next one, summed. S4.4: the source is never
        // switched mid-file, so a degradation is a GAP in the marker table and the file says how big.
        uint64_t localSampleGapUs;
        // S4.5: round_counter going DOWN is a rematch inside one state-9 session. It is a named,
        // file-closing event, because two rows numbered 3 make SeekRound(3) ambiguous with no refusal.
        bool roundCounterReset;
        // Boundaries this source refused rather than write, because their number would have broken
        // the reader's ascent invariant. A capture the reader refuses is a lost recording, so the one
        // row that cannot be represented is dropped BY NAME here instead of costing the whole file.
        uint32_t refusedNonAscending;
        // Rows numbered from where the table already stood because the engine restarted its counter.
        // They carry kRoundCounterReset | kRoundNumberSynthesised, so ours are never read as theirs.
        uint32_t rebasedAfterReset;
    };

    static constexpr uint16_t kMaxRows = 256;
    // S4.3's B2 window: a phase-derived ROUND_BEGIN is suppressed this long after a counter-derived
    // one, so the two rules cannot both file the same boundary.
    static constexpr uint64_t kB2SuppressUs = 500000;

    // A refused boundary is named in the log ONCE, and only the first few refusals are named at all.
    // What a reader needs is the pattern -- the number offered, the number that stands, the phase
    // that offered it -- and the pattern repeats: the lobby oscillation that refused twice in a 237 s
    // capture (DIAG-ROUNDS-NONASC) refuses on every re-entry, so a session left running overnight
    // would offer thousands. This bounds the log at a handful of lines per FILE while losing nothing:
    // Stats::refusedNonAscending still counts every refusal, named or not. Sized to name several
    // distinct oscillations rather than one, so a second cause is visible behind the first.
    static constexpr uint32_t kMaxRefusalsNamed = 8;

    // One refused boundary, with enough to name it: what was offered, what already stands, and where
    // in the file the offer was made.
    struct RefusalNote {
        uint64_t captureTimeUs;
        uint16_t offered;   // the round number this sample offered
        uint16_t admitted;  // the number already in the table that `offered` failed to ascend past
        uint8_t phase;      // gamestate::Phase of the sample that offered it
    };

    LocalMarkerSource() { Reset(); }
    void Reset();

    // One local sample. Emits at most `cap` markers into `out` and returns how many; the markers are
    // stamped with the sample's own captureTimeUs, so a boundary lands where the edge did.
    uint32_t Note(const gamestate::GameStateSample&, MarkRec* out, uint32_t cap);
    // The file is closing. An open round becomes a kRoundPartialEnd row -- the footage exists, so the
    // row exists and says what it is.
    void Finish(uint64_t captureTimeUs);

    const RoundRow* Rows() const { return rows_; }
    uint16_t RowCount() const { return rowCount_; }
    const Stats& GetStats() const { return stats_; }

    // Hands back the refusals recorded since the last call and forgets them, so each one can be
    // named exactly once. Returns how many were written; anything that did not fit stays for the
    // next call rather than being dropped.
    uint32_t TakeRefusals(RefusalNote* out, uint32_t cap);

private:
    // S4.5's ascent invariant, enforced HERE so no capture can be written that fails it at the
    // reader. Returns false when the boundary must not be written at all; may re-base `number` and
    // add flags when the engine's counter restarted under this file.
    bool AdmitNumber(uint16_t& number, uint16_t& flags);
    void NoteRefusal(uint16_t offered);
    void OpenRow(uint16_t number, uint16_t flags);
    void CloseRow(uint16_t flags);
    uint16_t CurrentNumber() const { return roundOpen_ ? rows_[rowCount_ - 1].number : curNumber_; }

    RoundRow rows_[kMaxRows] = {};
    uint16_t rowCount_ = 0;
    bool roundOpen_ = false;

    gamestate::GameStateSample last_ = {};
    bool primed_ = false;
    uint8_t prevPhase_ = 0;
    uint64_t prevPhaseSymbol_ = 0;
    bool suddenDeathAnnounced_ = false;
    bool hasCounter_ = false, hasPoints_ = false, hasLaunch_ = false, hasPaused_ = false;
    uint32_t prevCounter_ = 0, prevBlue_ = 0, prevRed_ = 0, prevLaunch_ = 0;
    uint64_t prevPaused_ = 0, pausedBaseline_ = 0;
    uint16_t curNumber_ = kRoundNumberNone;
    uint16_t lastNumber_ = 0;
    bool sawNumber_ = false;
    bool counterRebased_ = false;
    uint64_t lastRoundBeginUs_ = 0, lastOkUs_ = 0, gapOpenUs_ = 0;
    RefusalNote refusals_[kMaxRefusalsNamed] = {};
    uint32_t refusalPending_ = 0;  // recorded and not yet taken
    uint32_t refusalsNamed_ = 0;   // recorded since Reset(), taken or not -- this is the rate limit
    Stats stats_ = {};
};

// ★ ONE EMITTER PER FILE, CHOSEN ONCE (ARCH-21 S4.4). Dedup by unrepresentability: a marker table
// with two producers is the mixed-provenance fork the design refuses, and no per-boundary tie-break
// can be as strong as never having two candidates at the same time.
//
// C6's third state is the point. R1 latched on the first sample after the file opened, so one
// transient non-Ok condemned a whole file to zero markers, silently -- the goal failing quietly. The
// window below waits, and when it ends with nothing decidable it writes `MarkerSource::None`, which
// is a NAMED refusal in the file rather than an unlabelled `Wire` that read as agreement.
class MarkerSourceChoice {
public:
    static constexpr uint32_t kArmTicks = 240;
    static constexpr uint64_t kArmWindowUs = 2000000;

    void Reset();
    // False when this build has no local candidate at all (S_GAMESTATE off, no sampler): the window
    // would then only ever expire, so the wire path is chosen at its first row instead of after 2 s
    // of markers nobody filed.
    void SetLocalCandidate(bool yes) { localCandidate_ = yes; }
    void NoteFileOpen(uint64_t tUs) { openedUs_ = tUs; }
    // One local sample. `ok` is IGameState's own Availability::Ok for it.
    void NoteLocalSample(bool ok, uint64_t tUs);
    // One netvar record handed to the corroborator, with the rows it carried.
    void NoteCorroboratorFed(uint32_t rows, uint64_t tUs);
    // The choice is PROVISIONAL until a marker is actually filed under it.
    //
    // R1 latched at the window's end, so a file that opened before netGame was live -- the always-on
    // path, which starts in the lobby -- was condemned for its whole length even when local became
    // readable a tick after the window closed. What makes one-emitter-per-file true is not the timer
    // but the FIRST FILED MARK: while nothing has been filed there is no provenance to mix, so the
    // choice may still improve to LOCAL. The caller calls this the moment a mark reaches the stream,
    // and from then on the source cannot move.
    void NoteMarkFiled();
    // At close: a file shorter than the arm-up window still names a source, and None now means
    // NEITHER PROVIDER WAS EVER READABLE -- for the whole file, not merely for its first two seconds.
    MarkerSource Settle();

    MarkerSource Source() const { return source_; }
    bool Chosen() const { return source_ != MarkerSource::NotRecorded; }
    // Was the wire path ever fed anything? A zero corroboration count then reads as COULD NOT ASK
    // rather than as disagreement -- which on the owner's solo host is the honest answer.
    bool CorroboratorAsked() const { return corroboratorAsked_; }
    uint32_t WireRowsSeen() const { return wireRowsSeen_; }
    uint32_t LocalSamplesInWindow() const { return ticks_; }

private:
    void ExpireIfDue(uint64_t tUs);

    MarkerSource source_ = MarkerSource::NotRecorded;
    bool locked_ = false;
    bool localCandidate_ = true;
    bool corroboratorAsked_ = false;
    uint32_t wireRowsSeen_ = 0;
    uint32_t ticks_ = 0;
    uint64_t openedUs_ = 0;
};

}  // namespace capture
}  // namespace replay
