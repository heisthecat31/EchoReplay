#pragma once

// The worker side: ring records in, .echodemo chapters out.
//
// One idea collapses three (S5.2): a chapter boundary is a keyframe, a seek target, a compression unit
// and a recovery unit. So this class does all four in one place -- it resets every encoder's delta state
// at the boundary, writes S_KEY from its own shadow of the world, and hands the container a chapter it
// can decode with nothing before it.
//
// ★ A CHAPTER CLOSES ON EITHER OF TWO CONDITIONS, and the size one is not an optimisation. S5.2 caps a
// chapter at 1 MiB so a recovery scan is bounded and a torn tail costs at most one chapter. Each stream
// buffer therefore carries headroom for one whole record past its soft cap, so a record is never half
// written when the cap is reached -- a BitWriter refusal is sticky and has no rewind, and a half record
// would desynchronise every record after it.
//
// ★ ENTITY SLOTS ARE A COMPRESSION GROUPING, NOT THE FILE'S NOTION OF IDENTITY. A wire record's real
// identity is the message header, which is stored (delta-coded, so it costs a bit or two once it stops
// changing) rather than decoded into a key we would have had to guess. The slot this class assigns is
// only which delta predecessor a record is coded against: if the key it groups on turns out to churn,
// the file gets larger and stays exactly as correct. Nothing downstream may treat the slot as an
// entity id, and W3-E can key on the stored header once its layout is decoded.

#include <stdint.h>

#include "capture/capture_types.h"
#include "capture/completeness.h"
#include "bootstrap/services.h"
#include "capture/duv.h"
#include "capture/keyframe.h"
#include "capture/markers.h"
#include "capture/streamcodec.h"
#include "events/lobbytrack.h"
#include "foundation/demofile/demofile.h"

namespace replay {
namespace capture {

class RosterTable;

struct BuilderConfig {
    uint32_t keyframeIntervalUs = 2000000;   // S5.5, a manifest field and never a constant in a reader
    bool streamEnabled[32] = {};             // indexed by Stream id
    bool sectionEnabled[kSectionCount] = {};
    uint32_t captureRole = 0;                // 0 client, 1 offline_host, 2 dedicated
};

// One refused encode, named. `stream` and `field` point at static storage -- a stream name and
// BodyFieldName's table -- never at a formatted buffer, so a note outlives the tick that made it.
struct EncodeRefusalNote {
    const char* stream = nullptr;
    const char* field = nullptr;   // nullptr: the refusal named no field of the record
    uint64_t value = 0;
    uint32_t width = 0;
};

struct BuilderStats {
    uint64_t recordsIn = 0;
    uint64_t recordsWritten = 0;
    uint64_t chaptersWritten = 0;
    uint64_t keyframes = 0;
    uint64_t wireDecoded = 0;    // stored in decoded form, the decode PROVED by re-encoding
    uint64_t wireVerbatim = 0;   // the decode did not reproduce the source: stored whole, counted
    uint64_t wireDecodeFail[5] = {};   // by WireDecode reason, so a count names its own cause
    uint64_t encodeRefusals = 0;
    // The first refusals NAMED. L2 #15's close line said `7080 refusals` and nothing about which
    // field could not be stored, which is the whole diagnosis. Rate-limited on markers.cpp's rule --
    // the limit is on what is RECORDED, not on what is taken -- and at the marker source's own
    // magnitude rather than a second one invented here.
    static constexpr uint32_t kMaxRefusalsNamed = LocalMarkerSource::kMaxRefusalsNamed;
    uint32_t encodeRefusalsNamed = 0;
    EncodeRefusalNote encodeRefusalNotes[kMaxRefusalsNamed] = {};
    uint64_t netvarMalformed = 0;
    // DUV records this builder was HANDED, counted before the parse. L2-RUN-2 F3: the marker deriver
    // reported `rowsSeen 0` and that number has three causes -- no DUV record ever arrived, records
    // arrived and every one failed to parse, or the mark stream was off -- which a zero row count
    // cannot tell apart. It was the first, and nothing in the file said so.
    uint64_t netvarRecords = 0;
    uint64_t bytesToWriter = 0;
    uint64_t slotEvictions = 0;
    // Blocks refused because byte length and record count disagreed. Must stay 0; non-zero
    // means a stream wrote records without accounting for them, which is A2's shape.
    uint64_t blockWidthMismatch = 0;  // the delta grouping ran out of slots; larger file, same contents
    // Chapters ended early because the world re-origin shifted. One per re-origin the capture saw:
    // the split is what makes "every record in a chapter shares one coordinate frame" true, so a
    // number here is the only evidence that guarantee was actually enforced rather than assumed.
    uint64_t worldOriginSplits = 0;
    // Chapters ended because a round boundary marker arrived. Locked constraint 6 asks for round
    // markers; this is the number that says a boundary is actually a seek target rather than a row
    // buried mid-chapter.
    uint64_t roundBoundarySplits = 0;
    // Chapters ended because the next record's time was a whole keyframe interval or more past the
    // open chapter's end. Zero on a healthy file: a non-zero count is a hole in the footage or a
    // clock that moved, and either way it is now a chapter boundary rather than a long chapter.
    uint64_t axisJumpSplits = 0;
    // Records whose own stamp is not on the file's source-time axis and which therefore rode the
    // last axis time instead of redefining a chapter's span. Non-zero is normal -- every wire, tap
    // and voice record is one. Zero on a file that carries S_EVENT means the classification was
    // removed, which is the L5 re-run #4 defect back again.
    uint64_t foreignStampsRidden = 0;
    // Callers that asked for a stream whose id is retired. Non-zero is a caller bug, not a file
    // defect: the request was refused, so the file is correct either way.
    uint64_t retiredStreamsRefused = 0;
    // Wire-derived markers the corroborator produced while LOCAL owned the table (ARCH-21 S4.4).
    // Non-zero is the intended shape on a wire capture, not a fault: it says the corroborator had
    // something to say and the file did not file it, because one file has one emitter.
    uint64_t wireMarksSuppressed = 0;
    // Wire-derived markers lost because the arm-up hold filled before the choice settled. Zero on
    // every shipped path; non-zero says the marker table is short and names the reason, which is the
    // one thing a silently dropped edge cannot do.
    uint64_t wireMarksHeldDropped = 0;
    // Round boundaries filed WITHOUT cutting a chapter, because the mark waited out the arm-up
    // window and the open chapter has already run past its instant. Cutting there would make two
    // chapters overlap, so the marker is filed anyway -- losing a real boundary is worse than one
    // that shares a chapter -- and this count is how a reader knows which boundaries did not split.
    uint64_t boundaryMarksNotCut = 0;
    // S_GAMESTATE records written. Zero with a sampler configured is the L2 shape, and it is the
    // number that separates "no rounds happened" from "nothing ever sampled the game state".
    // Keyframes that carried KB_AUDIO_PINS, and keyframes written with none published. The pair is
    // the denominator: a file with no pin blocks is a file recorded with no audio state bound, and
    // without the second count that is indistinguishable from a capture that never tried.
    uint64_t audioPinBlocks = 0;
    uint64_t audioPinsAbsent = 0;
    uint64_t gameStateRecords = 0;
    // S_PLAYER_STATS records handed to the writer, and the ones it refused. The refusals are here
    // rather than only in LastError() because WritePlayerStats refuses per RECORD -- an engine time
    // that went backwards for one user does not stop the stream -- and a file short by four records
    // with nothing saying so is exactly the silence this project keeps finding.
    uint64_t playerStatsRecords = 0;
    uint64_t playerStatsRefused = 0;
    // Keyframe netvar snapshot (SPEC-RECORDER-2 S2.3). `netvarSnapshotSeeded` counts the keys the
    // engine supplied a value for that no recorded delta had yet reached -- the rows that only exist
    // because of the snapshot. `netvarSnapshotUnreadable` is what the source could not read this
    // keyframe; a nonzero total with zero seeded says the store was bound and every key already had
    // a delta, which is a different fact from "the source never ran".
    uint64_t netvarSnapshots = 0;
    uint64_t netvarSnapshotSeeded = 0;
    uint64_t netvarSnapshotUnreadable = 0;
};

// One playback-driven HUD key as the engine currently holds it. `value` is the raw SValue qword in
// the same encoding the delta rows carry, so the keyframe block stores it untranslated.
struct NetvarSnapshotRow {
    uint64_t name = 0;
    uint64_t value = 0;
    uint16_t slot = 0;
};

// THE DELTA TAP'S HOLE, AND WHO FILLS IT (SPEC-RECORDER-2 S2.3).
//
// The netvar tap is a listener on the delta message, so a var that does not change during a capture
// is in no record and in no keyframe, and playback's primer -- which reads value-at-T0 out of the
// keyframe netvar block -- has nothing to hand the HUD. The scoreboard then shows the offline host's
// own residual numbers. This reads the engine's CURRENT value for those keys at keyframe time.
//
// It is READ-ONLY on the engine and lives behind an interface for one reason: replay_capture must
// build and test on a machine with no game. The live implementation is capture/netvarsnapshot.cpp,
// compiled only into the DLL.
class INetvarSnapshotSource {
public:
    virtual ~INetvarSnapshotSource() {}

    // Fills up to `cap` rows and returns how many. `unreadable` receives the keys that could not be
    // read this time -- a store not bound yet on a fresh spectate join reads as 0 rows and the full
    // key count unreadable, and the next keyframe asks again. Must never throw, block or write.
    virtual uint16_t Snapshot(NetvarSnapshotRow* out, uint16_t cap, uint16_t& unreadable) = 0;
};

class ChapterBuilder {
public:
    ChapterBuilder();
    ~ChapterBuilder();
    ChapterBuilder(const ChapterBuilder&) = delete;
    ChapterBuilder& operator=(const ChapterBuilder&) = delete;

    bool Open(IDemoWriter*, const Manifest&, const BuilderConfig&);
    // Optional; used only to report a refused block. The builder runs on the worker thread.
    void SetLog(ILog* log) { log_ = log; }
    // Read by the module's own test hook: a builder holding no log drains its refusal notes into
    // nothing, and the count on the stop line is then the only witness (L2-RUN-11 F27).
    ILog* Log() const { return log_; }
    // One ring record. Everything downstream of here runs on the worker thread.
    void Consume(const void* record, uint32_t bytes);
    // Ends the open chapter, if any. Called at a round boundary and at session close (E-C12).
    void FlushChapter();
    void Close();

    const BuilderStats& Stats() const { return stats_; }
    Completeness& Accounting() { return accounting_; }
    const Completeness& Accounting() const { return accounting_; }
    const DuvSequence& NetvarSequence() const;
    // S5.7's markers are derived from the netvar stream as it is decoded, so the marker table in a file
    // is reproducible FROM that file rather than being a second observation nobody can check.
    const MarkerDeriver::Stats& MarkerStats() const;
    // The LOCAL source's own numbers, and the one-per-file source choice (ARCH-21 S4.3 / S4.4).
    // `CorroboratorAsked` is deliberately separate from a corroboration COUNT: on the owner's solo
    // host nothing ever feeds the wire path, and "could not ask" is not "agreed".
    const LocalMarkerSource::Stats& LocalMarkerStats() const;
    MarkerSource ChosenMarkerSource() const;
    bool CorroboratorAsked() const;
    uint32_t WireRowsSeen() const;
    // ARCH-21's MANIFEST round table, complete after Close(). DemoWriter::SetRoundTable is a
    // NON-VIRTUAL accessor -- Appendix B.2's IDemoWriter signatures are frozen -- so the owner of the
    // concrete writer files the table, and this is where it reads it from.
    const RoundRow* RoundRows() const;
    uint16_t RoundRowCount() const;

    // S5.7's JOIN / LEAVE / TEAM_CHANGE, derived from the lobby bodies on S_EVENT exactly the way the
    // six netvar kinds are derived from S_NETVAR: recomputable from the file, not a second observation.
    // The roster is also what each keyframe's kKeyBlockRoster is built from.
    const events::LobbyTracker& Lobby() const;
    const events::LobbyTracker::Stats& LobbyStats() const;
    // The first whole gametype an SNSLobbyOwnerState body stated in this file, 0 when none did. The
    // engine field read at open can still be unfilled on the spectate path; this is what the lobby
    // said once it knew.
    uint64_t OwnerStateGametype() const;
    // The file's own boundaries, which no netvar edge can produce.
    void EmitMatchBegin(uint64_t tUs);
    void EmitMatchEnd(uint64_t tUs);

    // Records a shed or a degradation into the file. S6.4 rung 4: the ladder is never silent.
    void NoteDegraded(uint64_t tUs, uint16_t what, uint16_t reason, uint32_t detail);
    // A run we saw and lost downstream -- a full ring, a block that would not fit (E-C10).
    void NoteDropped(StreamId, uint64_t tUs, uint16_t reason, uint32_t count);

    uint64_t LastSourceTimeUs() const { return lastSourceUs_; }
    void SetKeyframeIntervalUs(uint32_t us) { cfg_.keyframeIntervalUs = us; }

    // The LOCALLY sourced roster (capture/roster.h), borrowed and read at each keyframe.
    // kKeyBlockRoster is written from the UNION of this and the lobby-derived entrants: the
    // lobby tracker is fed by S_EVENT bodies, which a solo `-offline` host never produces, so
    // it alone made every such capture nameless. Null is legal and means this build had no
    // local source -- the block still gets the lobby's rows.
    void SetRoster(const RosterTable* r) { roster_ = r; }
    // ★ WHY THE CONCRETE WRITER. DemoWriter::WritePlayerStats is NON-VIRTUAL -- Appendix B.2 freezes
    // IDemoWriter's signatures -- and it is the one implementation of the format's absence sentinels
    // and of its per-user monotone rule. Reproducing those rules here to reach them through the
    // frozen interface would be a second copy of a format, which is this project's most repeated
    // defect. So the owner of the writer hands it over, exactly as it does for SetRoundTable. Null
    // is a supported state: a builder driven by a fake IDemoWriter counts stats records as REFUSED
    // rather than buffering them for a writer that will never arrive.
    void SetStatsWriter(DemoWriter* w) { statsWriter_ = w; }

    // The engine-reading half of the keyframe netvar block. Null -- the default, and what every
    // offline test gets -- writes the delta shadow alone, exactly as before this existed.
    void SetNetvarSnapshotSource(INetvarSnapshotSource* s) { netvarSnapshot_ = s; }

private:
    struct Impl;
    void EnsureChapterOpen(uint64_t tUs);
    // Ends the open chapter and opens the next one at tUs. Every round boundary that reaches the
    // file goes through here, whether it was derived from the stream or emitted by the capture.
    void BeginBoundaryChapter(uint64_t tUs);
    // Files one derived marker: cuts its chapter if it is a boundary, appends it, accounts for it,
    // and commits the file's marker provenance. Every provider-derived mark goes through here.
    void FileMark(const MarkRec&);
    // The ONE point where held wire marks meet the settled choice -- filed under Wire, discarded
    // under Local or None. Called wherever the choice can have moved.
    void ResolveWireMarks();
    void WriteKeyframe(uint64_t tUs);
    void DispatchRecord(const RecHeader&, const uint8_t* body, uint64_t tUs);

    // Reads the engine's current value for every HUD key and seeds the keyframe shadow with the ones
    // no delta has carried. Nothing happens without a source.
    void SnapshotNetvars();

    Impl* impl_;
    const RosterTable* roster_ = nullptr;
    DemoWriter* statsWriter_ = nullptr;
    INetvarSnapshotSource* netvarSnapshot_ = nullptr;
    BuilderConfig cfg_ = {};
    BuilderStats stats_ = {};
    Completeness accounting_;
    uint64_t lastSourceUs_ = 0;
    ILog* log_ = nullptr;
};

}  // namespace capture
}  // namespace replay
