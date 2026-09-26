#pragma once

// S5.6.2's accounting, as a component with one job: for every stream, over every chapter, say how many
// records THE ENGINE numbered and how many reached the file, and name every difference.
//
// ---------------------------------------------------------------------------------------------
// ★ THE ONE RULE THIS FILE EXISTS TO HOLD: `observed` NEVER COMES FROM OUR SAMPLE LOOP.
//
// A counter that counts what the writer chose to look at can never detect under-sampling -- poll a
// 118 Hz stream at 10 Hz and observed == stored == 10, with every gate green and 108 samples a second
// gone. So each stream declares WHERE its observed count comes from, and a stream with no engine source
// in the current capture role reports `None` and the file carries a DEGRADED record saying its
// completeness is unverified. An invariant that quietly returns true when its input is unavailable
// would be worse than no invariant.
//
//   S_TICK, S_*_SAMPLED, S_ASSEEN   the engine frame counter, CncaGame+0x830. Per entity: an entity
//                                   present in two ticks whose frame indices differ by more than one
//                                   lost (delta - 1) samples, and the range is named. An entity that
//                                   was ABSENT clears its tracker, because E-C2's absence is absence
//                                   and must not read as loss.
//   S_NETVAR                        the DUV packet number, lifted from the message body (duv.h). The
//                                   number is also STORED in the record, so a reader can redo the
//                                   arithmetic rather than believe ours.
//   S_*_WIRE, S_EVENT, S_VOIP       the tap's own delivery. S5.6.2 blesses this for the tap because a
//                                   registration is not a poll: the engine hands us every dispatch it
//                                   makes, so a delivery count IS the engine's numbering.
//
// ★ AND THE WIRE STREAMS ARE WHY THIS IS NOT THE DESIGN'S OWN TABLE. S5.6.2 marks S_PLAYER_WIRE and
// S_BODY_WIRE `[U]` -- their proposed source was the 30-node history list, and a bounded buffer can
// report what it holds but never what it lost (F-11). This packet does not poll that list: it takes the
// same messages off W1-L's BroadcasterListen tap, which is the mechanism S5.6.2 already accepts for
// S_EVENT. That moves both streams from "no detector can fire" to the same footing as the event stream,
// and it is a change to the design, reported as one.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "capture/capture_types.h"
#include "foundation/demofile/format.h"

namespace replay {
namespace capture {

enum class ObservedSource : uint8_t {
    None = 0,          // no engine source in this role: completeness is UNVERIFIED and the file says so
    EngineFrameIndex,  // CncaGame+0x830
    WireSequence,      // the DUV packet number carried in the message body
    TapDelivery,       // a per-symbol registration on the engine's own message bus
    // The producer's own decision to emit. Legal ONLY for a stream the producer writes on change:
    // there, the emit decision IS the numbering, because nothing else numbers the records and the
    // deficit the law measures is the difference between deciding to emit and reaching the file.
    // Using it on a per-frame stream would be the fabricated denominator the top of this file
    // forbids, which is why NoteEmitted refuses it and ObservedSourceFitsCadence will not compile it.
    SamplerDecision,
};
const char* ObservedSourceName(ObservedSource);

// ★ THE RULE THE S_GAMESTATE DEFECT BROKE. A stream written on change has no engine frame numbering,
// so EngineFrameIndex can never be its source; a stream written once per frame has one, and must use
// it rather than counting its own emits. Which of the remaining sources an on-change stream uses
// depends on where its records come from and is chosen at the Configure site.
constexpr bool ObservedSourceFitsCadence(uint16_t stream, ObservedSource src) {
    return CadenceOf(stream) == StreamCadence::FixedCadence
               ? src == ObservedSource::EngineFrameIndex
               : src != ObservedSource::EngineFrameIndex;
}

constexpr uint16_t kMaxTrackedStreams = 24;
// Per chapter. A 2 s chapter at 120 Hz with every sampled stream losing a frame is a few hundred
// records; 2048 is well past that, and overflow past it is still accounted -- see SealChapter.
constexpr uint16_t kMaxGapRecords = 2048;

class Completeness {
public:
    void Configure(StreamId, ObservedSource);
    ObservedSource SourceOf(StreamId) const;

    void BeginChapter(uint64_t tUs);

    // A record reached the chapter builder and will be written.
    void NoteStored(StreamId, uint32_t count = 1);

    // The engine numbered `count` records for this stream, whether or not we saw them.
    void NoteObserved(StreamId, uint32_t count = 1);

    // Observation and store in ONE call, for a stream whose source is SamplerDecision.
    //
    // ★ It is one call because the two used to be two, and the gap between them was a defect. The
    // S_GAMESTATE handler counted the observation, then filed a round marker, and filing a round
    // marker CUTS THE CHAPTER -- so the observation landed in the chapter that closed and the record
    // in the one that opened. Per chapter that reads as one record lost and one record stored the
    // engine never numbered; over a file it was `observed` two above `stored` on a capture that had
    // not lost anything. An on-change stream decides to emit and emits at one instant, so the two
    // counters must move at one instant too, and the only way to guarantee that is to leave the
    // caller no way to separate them.
    //
    // A stream whose configured source is not SamplerDecision is still STORED -- the block must
    // declare what it holds -- but its emit is NOT counted as an observation, and WrongSourceEmits()
    // says so. Nothing is clamped: the block then carries observed below stored and the writer
    // refuses it, which is the accounting reporting its own misconfiguration.
    void NoteEmitted(StreamId, uint32_t count = 1);

    // A run the engine numbered and we never saw. Both counters move: observed by `count`, and the run
    // is named so `observed == stored + sum(gap_ranges)` closes for a reader holding only the file.
    void NoteGap(StreamId, uint64_t tUs, uint16_t reason, uint32_t firstMissing, uint32_t count);

    // A run we DID see and lost downstream -- a full ring, a chapter that could not hold the block.
    // Different cause, same shape: it is stored on S_DROPPED so the file distinguishes the two (E-C10).
    // `alreadyObserved` true when the caller has already counted the observation, as the
    // per-entity path has. Counting it twice would inflate observed around a number nobody read.
    void NoteDropped(StreamId, uint64_t tUs, uint16_t reason, uint32_t count,
                     bool alreadyObserved = false);

    // ---- the per-entity frame tracker, which is the engine-sourced half for the sampled streams ----
    //
    // A3. PRESENCE AND SAMPLING ARE REPORTED SEPARATELY, and they have to be. The single combined call
    // this replaced could not tell a sampler that stopped producing from an entity that legitimately
    // left: both showed up as "no record for this entity", and the audit got BIT-IDENTICAL output for
    // the two. A dropped sample is a loss the file must name; a departed player is not.
    //
    // So the gather reports what the ENGINE HELD -- NoteEntityPresent, once per entity per tick, which
    // is the observation -- and separately what reached the file -- NoteEntitySampled. Present without
    // sampled is a DROP. Absent is absence, and clears the tracker so a returning entity does not
    // report the frames it was gone as lost samples (E-C2).
    void BeginFrameSet(StreamId, uint32_t frameIndex);
    void NoteEntityPresent(StreamId, uint16_t entity, uint32_t frameIndex, uint64_t tUs);
    void NoteEntitySampled(StreamId, uint16_t entity);
    void EndFrameSet(StreamId, uint64_t tUs);

    // A3. A stream that never observed anything has not been shown to be complete -- its arithmetic
    // closes at 0 == 0 + 0, which is the vacuous pass the invariant exists to forbid. A caller that
    // enabled a stream and got this back must report it, not treat silence as success.
    bool EverObserved(StreamId) const;

    uint32_t ChapterObserved(StreamId) const;
    uint32_t ChapterStored(StreamId) const;

    // True when this stream is IN the completeness law in this capture: it was configured, and its
    // role gives it a source. A caller writing a block header must report ChapterObserved verbatim
    // for such a stream -- zero included -- and must NOT substitute the block's own record count,
    // which would compare the count against itself.
    bool IsAccounted(StreamId) const;

    // Call once, after the chapter's last record and before reading the blocks below. It appends one
    // record per stream for any run the per-record array could not hold, so the per-STREAM arithmetic
    // stays exact even when the individual ranges no longer fit.
    //
    // ★ This exists because the first version folded an overflow into whatever record happened to be
    // last, which belonged to a different stream. The file's totals stayed right and the attribution
    // was wrong -- S_ASSEEN under-reported by 24 and S_TICK over-reported by 14, and `echodemo verify`
    // printed an unsigned underflow rather than a wrong-looking number. A cap that redistributes
    // between streams is worse than a cap that says it ran out.
    void SealChapter(uint64_t tUs);

    // The GAP and DROPPED blocks for this chapter, as the container's own record layout. Empty spans are
    // legal and normal; a non-empty one is the file being honest about itself.
    const GapRecordDisk* GapRecords(uint32_t& count) const;
    const GapRecordDisk* DroppedRecords(uint32_t& count) const;

    // Streams whose completeness could not be verified in this role. The caller writes one DEGRADED
    // record for each, so a reader is told rather than left to infer green from silence.
    uint32_t UnverifiedStreams(StreamId* out, uint32_t cap) const;

    // Totals over the whole session, for the handoff report and tools/sizecheck.
    struct Totals {
        StreamId stream;
        ObservedSource source;
        uint64_t observed, stored, gapped, dropped;
    };
    uint32_t Snapshot(Totals* out, uint32_t cap) const;

    // Non-zero means the accounting itself is suspect: a stream stored more than the engine numbered.
    // That is a bug in this file's caller, not a completeness signal, and it is counted rather than
    // clamped away.
    uint32_t Impossible() const { return impossible_; }
    // Non-zero means at least one chapter had more distinct lost runs than the array holds, so those
    // runs are accounted in a total rather than named individually. Reported, never silent.
    uint32_t GapOverflows() const { return gapOverflows_; }
    // Non-zero means a stream's emit decision was offered as an observation for a stream whose
    // records something else numbers -- a configuration fault, reported rather than absorbed.
    uint32_t WrongSourceEmits() const { return wrongSourceEmits_; }

private:
    struct Track {
        StreamId stream = 0;
        ObservedSource source = ObservedSource::None;
        uint64_t observed = 0, stored = 0, gapped = 0, dropped = 0;
        uint32_t chapterObserved = 0, chapterStored = 0;
        // The per-entity frame tracker. Index is entity % kMaxEntities, like the codec's delta state.
        uint32_t lastFrame[kMaxEntities] = {};
        bool present[kMaxEntities] = {};
        bool seenThisFrame[kMaxEntities] = {};
        bool sampledThisFrame[kMaxEntities] = {};
        // Runs this chapter's gap array could not hold, kept per stream so the totals never migrate.
        uint32_t overflow = 0;
        uint32_t overflowFirst = 0;
        bool used = false;
    };
    Track* Find(StreamId);
    const Track* Find(StreamId) const;
    Track* FindOrAdd(StreamId);

    Track tracks_[kMaxTrackedStreams] = {};
    uint16_t trackCount_ = 0;
    GapRecordDisk gaps_[kMaxGapRecords] = {};
    uint32_t gapCount_ = 0;
    GapRecordDisk drops_[kMaxGapRecords] = {};
    uint32_t dropCount_ = 0;
    uint32_t impossible_ = 0;
    uint32_t gapOverflows_ = 0;
    uint32_t wrongSourceEmits_ = 0;
    uint64_t chapterUs_ = 0;
};

}  // namespace capture
}  // namespace replay
