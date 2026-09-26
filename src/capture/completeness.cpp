#include "capture/completeness.h"

#include <string.h>

namespace replay {
namespace capture {

const char* ObservedSourceName(ObservedSource s) {
    switch (s) {
        case ObservedSource::None: return "none (completeness unverified)";
        case ObservedSource::EngineFrameIndex: return "engine frame counter";
        case ObservedSource::WireSequence: return "DUV packet number";
        case ObservedSource::TapDelivery: return "message-bus delivery";
        case ObservedSource::SamplerDecision: return "the sampler's own emit decision";
    }
    return "unknown";
}

Completeness::Track* Completeness::Find(StreamId s) {
    for (uint16_t i = 0; i < trackCount_; ++i) {
        if (tracks_[i].stream == s) return &tracks_[i];
    }
    return nullptr;
}

const Completeness::Track* Completeness::Find(StreamId s) const {
    for (uint16_t i = 0; i < trackCount_; ++i) {
        if (tracks_[i].stream == s) return &tracks_[i];
    }
    return nullptr;
}

Completeness::Track* Completeness::FindOrAdd(StreamId s) {
    if (Track* t = Find(s)) {
        return t;
    }
    if (trackCount_ >= kMaxTrackedStreams) {
        return nullptr;
    }
    Track& t = tracks_[trackCount_++];
    t.stream = s;
    t.used = true;
    return &t;
}

void Completeness::Configure(StreamId s, ObservedSource src) {
    if (Track* t = FindOrAdd(s)) {
        t->source = src;
    }
}

ObservedSource Completeness::SourceOf(StreamId s) const {
    const Track* t = Find(s);
    return t ? t->source : ObservedSource::None;
}

void Completeness::BeginChapter(uint64_t tUs) {
    chapterUs_ = tUs;
    gapCount_ = 0;
    dropCount_ = 0;
    for (uint16_t i = 0; i < trackCount_; ++i) {
        tracks_[i].chapterObserved = 0;
        tracks_[i].chapterStored = 0;
        tracks_[i].overflow = 0;
        tracks_[i].overflowFirst = 0;
    }
}

void Completeness::NoteStored(StreamId s, uint32_t count) {
    Track* t = FindOrAdd(s);
    if (!t) return;
    t->stored += count;
    t->chapterStored += count;
    if (t->chapterStored > t->chapterObserved) {
        // Only possible if the caller stored a record it never told us the engine numbered. Counted,
        // never hidden: the container refuses such a block anyway, and this says which stream did it.
        ++impossible_;
    }
}

void Completeness::NoteObserved(StreamId s, uint32_t count) {
    Track* t = FindOrAdd(s);
    if (!t) return;
    t->observed += count;
    t->chapterObserved += count;
}

void Completeness::NoteEmitted(StreamId s, uint32_t count) {
    Track* t = FindOrAdd(s);
    if (!t) return;
    if (t->source != ObservedSource::SamplerDecision) {
        ++wrongSourceEmits_;
        NoteStored(s, count);
        return;
    }
    t->observed += count;
    t->chapterObserved += count;
    NoteStored(s, count);
}

bool Completeness::IsAccounted(StreamId s) const {
    const Track* t = Find(s);
    return t && t->source != ObservedSource::None;
}

void Completeness::NoteGap(StreamId s, uint64_t tUs, uint16_t reason, uint32_t firstMissing,
                           uint32_t count) {
    if (count == 0) {
        return;
    }
    Track* t = FindOrAdd(s);
    if (!t) return;
    t->observed += count;
    t->chapterObserved += count;
    t->gapped += count;
    if (gapCount_ < kMaxGapRecords) {
        GapRecordDisk& g = gaps_[gapCount_++];
        g.tUs = tUs;
        g.streamId = s;
        g.reason = reason;
        g.firstMissing = firstMissing;
        g.count = count;
        return;
    }
    // The gap array is full. The run is held against ITS OWN STREAM and emitted by SealChapter, never
    // folded into whatever record happened to be last: that would keep the file's total right while
    // moving losses between streams, which is a worse failure than running out of room.
    t->overflow += count;
    if (firstMissing && (t->overflowFirst == 0 || firstMissing < t->overflowFirst)) {
        t->overflowFirst = firstMissing;
    }
    ++gapOverflows_;
}

void Completeness::SealChapter(uint64_t tUs) {
    for (uint16_t i = 0; i < trackCount_; ++i) {
        Track& t = tracks_[i];
        if (t.overflow == 0) {
            continue;
        }
        if (gapCount_ < kMaxGapRecords) {
            GapRecordDisk& g = gaps_[gapCount_++];
            g.tUs = tUs;
            g.streamId = t.stream;
            g.reason = kGapChapterOverflow;
            g.firstMissing = t.overflowFirst;
            g.count = t.overflow;
        } else if (gapCount_ > 0) {
            // Even the summary record does not fit. Merge into the last record OF THIS STREAM if there
            // is one; if there is not, the arithmetic will not close and verify will say so, which is
            // the correct outcome for a chapter this pathological.
            for (uint32_t k = gapCount_; k-- > 0;) {
                if (gaps_[k].streamId == t.stream) {
                    gaps_[k].count += t.overflow;
                    gaps_[k].reason = kGapChapterOverflow;
                    break;
                }
            }
        }
        t.overflow = 0;
        t.overflowFirst = 0;
    }
}

void Completeness::NoteDropped(StreamId s, uint64_t tUs, uint16_t reason, uint32_t count,
                               bool alreadyObserved) {
    if (count == 0) {
        return;
    }
    Track* t = FindOrAdd(s);
    if (!t) return;
    // A dropped record was observed -- we had it and lost it downstream -- so it counts as observed and
    // is accounted by a DROPPED record rather than a GAP. The distinction is the cause: a GAP is
    // upstream of us, a DROPPED is ours (E-C10).
    //
    // `alreadyObserved` is for the per-entity path, where NoteEntityPresent has ALREADY counted the
    // observation. Counting it twice there would inflate observed and make the arithmetic close around
    // a number nobody measured -- the same family as the defect this call is reporting.
    if (!alreadyObserved) {
        t->observed += count;
        t->chapterObserved += count;
    }
    t->dropped += count;
    if (dropCount_ < kMaxGapRecords) {
        GapRecordDisk& g = drops_[dropCount_++];
        g.tUs = tUs;
        g.streamId = s;
        g.reason = reason;
        g.firstMissing = 0;
        g.count = count;
        return;
    }
    drops_[kMaxGapRecords - 1].count += count;
}

void Completeness::BeginFrameSet(StreamId s, uint32_t /*frameIndex*/) {
    Track* t = FindOrAdd(s);
    if (!t) return;
    memset(t->seenThisFrame, 0, sizeof t->seenThisFrame);
    memset(t->sampledThisFrame, 0, sizeof t->sampledThisFrame);
}

// The OBSERVATION. The engine held this entity on this frame, whether or not we managed to record it.
void Completeness::NoteEntityPresent(StreamId s, uint16_t entity, uint32_t frameIndex, uint64_t tUs) {
    Track* t = FindOrAdd(s);
    if (!t) return;
    const uint16_t slot = entity % kMaxEntities;
    t->seenThisFrame[slot] = true;

    if (t->present[slot] && frameIndex > t->lastFrame[slot]) {
        const uint32_t step = frameIndex - t->lastFrame[slot];
        if (step > 1) {
            // The engine advanced the frame counter past frames in which this entity was present and we
            // produced nothing. That is exactly the loss S5.6.2 asks the file to name, and it is the
            // shape the halved-sample-rate mutation produces.
            NoteGap(s, tUs, kGapFrameSkip, t->lastFrame[slot] + 1, step - 1);
        }
    }
    t->present[slot] = true;
    t->lastFrame[slot] = frameIndex;
    NoteObserved(s, 1);
}

// The RECORD. Only called when one actually reached the chapter.
void Completeness::NoteEntitySampled(StreamId s, uint16_t entity) {
    Track* t = FindOrAdd(s);
    if (!t) return;
    t->sampledThisFrame[entity % kMaxEntities] = true;
    NoteStored(s, 1);
}

void Completeness::EndFrameSet(StreamId s, uint64_t tUs) {
    Track* t = Find(s);
    if (!t) return;
    for (uint16_t i = 0; i < kMaxEntities; ++i) {
        if (t->seenThisFrame[i] && !t->sampledThisFrame[i]) {
            // Present and not recorded. We observed it and lost it, which is a DROPPED record naming
            // the count -- and it is the case that used to be indistinguishable from the entity simply
            // not being there.
            NoteDropped(s, tUs, kGapRingFull, 1, /*alreadyObserved=*/true);
        }
        if (!t->seenThisFrame[i]) {
            // E-C2: absence is absence. Clearing the tracker is what stops a player who left and
            // rejoined from being reported as hundreds of lost samples.
            t->present[i] = false;
        }
    }
}

bool Completeness::EverObserved(StreamId s) const {
    const Track* t = Find(s);
    return t && t->observed > 0;
}

uint32_t Completeness::ChapterObserved(StreamId s) const {
    const Track* t = Find(s);
    return t ? t->chapterObserved : 0;
}

uint32_t Completeness::ChapterStored(StreamId s) const {
    const Track* t = Find(s);
    return t ? t->chapterStored : 0;
}

const GapRecordDisk* Completeness::GapRecords(uint32_t& count) const {
    count = gapCount_;
    return gaps_;
}

const GapRecordDisk* Completeness::DroppedRecords(uint32_t& count) const {
    count = dropCount_;
    return drops_;
}

uint32_t Completeness::UnverifiedStreams(StreamId* out, uint32_t cap) const {
    uint32_t n = 0;
    for (uint16_t i = 0; i < trackCount_; ++i) {
        if (tracks_[i].source == ObservedSource::None && tracks_[i].stored > 0) {
            if (n < cap) out[n] = tracks_[i].stream;
            ++n;
        }
    }
    return n < cap ? n : cap;
}

uint32_t Completeness::Snapshot(Totals* out, uint32_t cap) const {
    uint32_t n = 0;
    for (uint16_t i = 0; i < trackCount_ && n < cap; ++i) {
        out[n].stream = tracks_[i].stream;
        out[n].source = tracks_[i].source;
        out[n].observed = tracks_[i].observed;
        out[n].stored = tracks_[i].stored;
        out[n].gapped = tracks_[i].gapped;
        out[n].dropped = tracks_[i].dropped;
        ++n;
    }
    return n;
}

}  // namespace capture
}  // namespace replay
