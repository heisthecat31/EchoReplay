#include "playback/core/recorded_loadout.h"

#include <stdio.h>
#include <string.h>

#include "capture/identity_block.h"
#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/demotypes.h"

namespace replay {
namespace identity {

// THE ONE TRANSLATION UNIT THAT MAY NAME BOTH SIDES.  The header is stdint-only so src/engine can
// include it; these assertions are what keep its numbers from drifting away from the format's.
static_assert(kRecordedLoadoutPairCap == capture::kIdentityBlockSlotCount,
              "the recorded pair array must be exactly as wide as the block's slot array");
static_assert(kRecordedPairCountUnknown == capture::kIdentityPairCountUnknown,
              "could-not-ask must be the SAME value on both sides of the disk, or a v1 block's "
              "unknown count arrives as a number the provisioner would compare against");
static_assert(kRecordedEntrantCap == capture::kIdentityMaxEntrants,
              "one slot per entrant the format can carry: a narrower table would refuse entrants a "
              "recording genuinely holds, a wider one would claim a scan cannot produce");
static_assert(kRecordedCellBytes >= capture::kMaxEventBodyBytes,
              "a cell must hold the format's widest S_EVENT body");

namespace {

// Named causes this store adds to the block reader's own vocabulary.  The first two are BOUNDS BEING
// HIT, and both refuse: an entrant whose newer block was dropped must not go on answering with the
// older one.
const char kCapExceeded[] = "RecordedBlockCapExceeded";
const char kEvicted[] = "RecordedBlockEvicted";
const char kNoEntrant[] = "IdentityBlockNoEntrant";
const char kTooLarge[] = "RecordedBlockTooLarge";
// Recorded, but only AFTER the playhead: a different repair from "never recorded", so
// it is its own refusal word rather than a block that was refused.
const char kNotYetReached[] = "RecordedBlockNotYetReached";
const char kTableFull[] = "RecordedEntrantTableFull";

void Say(const char** refusal, const char* text) {
    if (refusal) *refusal = text;
}

// "IdentityBlockRefused:<why>" with a PROGRAM-LIFETIME pointer.  Every `why` in this file is a static
// literal, so one composed string per distinct cause is the whole population; a shared scratch buffer
// would have gone stale under a caller that holds the pointer across ticks -- pawn_provision.cpp
// keeps it in `loadoutSourceWhy`.
const char* Composed(const char* why) {
    static const char* keys[16] = {};
    static char texts[16][96] = {};
    static uint16_t n = 0;
    if (!why) why = "unstated";
    for (uint16_t i = 0; i < n; ++i) {
        if (keys[i] == why) return texts[i];
    }
    if (n >= 16) return "IdentityBlockRefused:unnamed";
    keys[n] = why;
    _snprintf_s(texts[n], sizeof texts[n], _TRUNCATE, "IdentityBlockRefused:%s", why);
    return texts[n++];
}

uint16_t VersionColumn(uint32_t version) {
    return version < kRecordedVersionColumns ? uint16_t(version) : 0;
}

}  // namespace

// The file's own tap table, out of this chapter's keyframe.  {u16 count, then per row: u16 index,
// u16 stream, u16 nameLen, name} -- chapterbuilder.cpp's kKeyBlockTapTable, read here rather than
// re-stated as a compiled array.
uint16_t TapIndexForName(const DecodedChapter& dc, const char* name, size_t nameChars,
                         uint8_t* scratch, size_t scratchBytes) {
    const StreamView* sv = dc.Find(kStreamKey);
    if (!sv || !sv->records || sv->bytes <= capture::kBlockPrefixBytes)
        return capture::kTapTypeIndexNone;
    bitio::BitReader r(sv->records + capture::kBlockPrefixBytes,
                       sv->bytes - capture::kBlockPrefixBytes);
    uint16_t blobLen = 0;
    if (!capture::DecodeRawRecord(r, scratch, scratchBytes, blobLen) || !blobLen)
        return capture::kTapTypeIndexNone;
    capture::KeyframeReader kr;
    if (!kr.Open(scratch, blobLen)) return capture::kTapTypeIndexNone;
    uint16_t blockId = 0;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
    while (kr.Next(blockId, data, len)) {
        if (blockId != capture::kKeyBlockTapTable || len < sizeof(uint16_t)) continue;
        uint16_t rows = 0;
        memcpy(&rows, data, sizeof rows);
        uint32_t at = sizeof(uint16_t);
        for (uint16_t i = 0; i < rows; ++i) {
            if (at + 3 * sizeof(uint16_t) > len) break;
            uint16_t index = 0, nameLen = 0;
            memcpy(&index, data + at, sizeof index);
            memcpy(&nameLen, data + at + 2 * sizeof(uint16_t), sizeof nameLen);
            at += 3 * sizeof(uint16_t);
            if (at + nameLen > len) break;
            if (nameLen == nameChars && memcmp(data + at, name, nameChars) == 0) return index;
            at += nameLen;
        }
    }
    return capture::kTapTypeIndexNone;
}

namespace {

uint16_t IdentityTapIndex(const DecodedChapter& dc) {
    static uint8_t blob[kTapTableScratchBytes];
    return TapIndexForName(dc, capture::kIdentitySnapshotTapName,
                           sizeof(capture::kIdentitySnapshotTapName) - 1, blob, sizeof blob);
}

}  // namespace

void RecordedIdentitySource::Reset() {
    for (uint16_t i = 0; i < kRecordedEntrantCap; ++i) tracks_[i] = RecordedEntrantTrack{};
    memset(cellUsed_, 0, sizeof cellUsed_);
    offered_ = 0;
    decoded_ = 0;
    refused_ = 0;
    evicted_ = 0;
    memset(versions_, 0, sizeof versions_);
    seq_ = 0;
    horizonUs_ = 0;
    slotCount_ = 0;
    eventRows_ = 0;
    tapChapters_ = 0;
    tapIndex_ = kRecordedTapIndexNone;
    tableFullFromUs_ = 0;
    firstRefusal_ = nullptr;
    census_[0] = 0;
}

void RecordedIdentitySource::NoteScan(uint32_t eventRows, uint32_t tapChapters, uint32_t tapIndex) {
    eventRows_ = eventRows;
    tapChapters_ = tapChapters;
    tapIndex_ = tapIndex;
}

RecordedEntrantTrack* RecordedIdentitySource::Track(uint32_t slot) {
    for (uint16_t i = 0; i < kRecordedEntrantCap; ++i) {
        if (tracks_[i].used && tracks_[i].slot == slot) return &tracks_[i];
    }
    return nullptr;
}

const RecordedEntrantTrack* RecordedIdentitySource::Track(uint32_t slot) const {
    for (uint16_t i = 0; i < kRecordedEntrantCap; ++i) {
        if (tracks_[i].used && tracks_[i].slot == slot) return &tracks_[i];
    }
    return nullptr;
}

uint32_t RecordedIdentitySource::SlotAt(uint16_t i) const {
    uint16_t seen = 0;
    for (uint16_t k = 0; k < kRecordedEntrantCap; ++k) {
        if (!tracks_[k].used) continue;
        if (seen == i) return tracks_[k].slot;
        ++seen;
    }
    return 0;
}

uint16_t RecordedIdentitySource::TakeCell() {
#ifdef W3E_MUTATE_POOL_UNDERSIZED
    // A pool short of entrants x depth: the recycle path needs a FULL ring, so entrants that have
    // not filled theirs are denied a cell they will never get back.
    const uint16_t cells = kRecordedBlocksPerEntrant * 6;
#else
    const uint16_t cells = kRecordedBlockCells;
#endif
    for (uint16_t i = 0; i < cells; ++i) {
        if (!cellUsed_[i]) {
            cellUsed_[i] = true;
            return i;
        }
    }
    return kRecordedCellNone;
}

void RecordedIdentitySource::GiveCell(uint16_t cell) {
    if (cell < kRecordedBlockCells) cellUsed_[cell] = false;
}

bool RecordedIdentitySource::Offer(const void* body, uint32_t bytes) {
    ++offered_;
    capture::IdentityBlockReader r;
#ifdef W3E_MUTATE_UNKNOWN_VERSION_PARSED
    // A reader pinned above its own format: a block laid out by a build this one has never seen
    // parses out of the bytes that happen to follow, and every field it reports is a guess.
    r.PinVersion(capture::kIdentityBlockVersion + 1);
#endif
    if (!r.Open(body, bytes)) {
        ++refused_;
        if (!firstRefusal_) firstRefusal_ = capture::IdentityBlockRefusalText(r.Refusal());
        return false;
    }
#ifdef W3E_MUTATE_BLOCK_NOT_DECODED
    // The shape of the defect this packet exists to close: the row is seen, counted, and nothing
    // reaches the store, so every read answers "not recorded" on a file that recorded it.
    return true;
#endif
    if (r.Count() == 0) {
        ++refused_;
        if (!firstRefusal_) firstRefusal_ = kNoEntrant;
        return false;
    }
    if (bytes > kRecordedCellBytes) {
        ++refused_;
        if (!firstRefusal_) firstRefusal_ = kTooLarge;
        return false;
    }

    bool stored = false;
    for (uint16_t e = 0; e < r.Count(); ++e) {
        capture::IdentityBlockEntrantView v{};
        if (!r.Entrant(e, v)) continue;
        const uint32_t slot = v.fields.userSlot;

        RecordedEntrantTrack* t = Track(slot);
        if (!t) {
            for (uint16_t i = 0; i < kRecordedEntrantCap && !t; ++i) {
                if (!tracks_[i].used) {
                    tracks_[i].used = true;
                    tracks_[i].slot = slot;
                    t = &tracks_[i];
                    ++slotCount_;
                }
            }
        }
        if (!t) {   // more entrants than the format's own entrant table can hold
            ++refused_;
            if (!firstRefusal_) firstRefusal_ = kTableFull;
#ifndef W3E_MUTATE_STALE_BLOCK_PAST_CAP
            if (!tableFullFromUs_) tableFullFromUs_ = r.TimeUs();
#endif
            continue;
        }

        // A full ring gives its own oldest cell back, so a long match costs bounded memory and the
        // eviction is COUNTED -- a read that lands before the oldest kept block refuses by name
        // rather than answering from a block that is no longer the latest one before the playhead.
        if (t->count == kRecordedBlocksPerEntrant) {
            t->oldestKeptUs = t->ring[1].tUs;
            GiveCell(t->ring[0].cell);
            for (uint16_t i = 1; i < t->count; ++i) t->ring[i - 1] = t->ring[i];
            --t->count;
            ++t->evicted;
            ++evicted_;
        }
        const uint16_t cell = TakeCell();
        if (cell == kRecordedCellNone) {
            // NOT a silent drop: this entrant already has entries and the newest block did not fit,
            // so it must refuse rather than go on answering with the one before it.
            ++refused_;
            if (!firstRefusal_) firstRefusal_ = kCapExceeded;
            t->capExceeded = true;
            if (!t->capExceededFromUs) t->capExceededFromUs = r.TimeUs();
            continue;
        }
        memcpy(cells_ + size_t(cell) * kRecordedCellBytes, body, bytes);
        RecordedIdentityEntry& row = t->ring[t->count++];
        row.entrantSlot = slot;
        row.blockEntrant = e;
        row.version = r.Version();
        row.tUs = r.TimeUs();
        row.cell = cell;
        row.length = bytes;
        row.seq = seq_++;
        if (t->count == 1) t->oldestKeptUs = row.tUs;
        ++versions_[VersionColumn(r.Version())];
        stored = true;
    }
    if (stored) ++decoded_;
    return stored;
}

const RecordedIdentityEntry* RecordedIdentitySource::Current(uint32_t entrantSlot) const {
    const RecordedEntrantTrack* t = Track(entrantSlot);
    if (!t) return nullptr;
    // Only from the dropped block onwards: before it, what this entrant holds IS the latest.
    if (t->capExceeded && horizonUs_ >= t->capExceededFromUs) return nullptr;
    const RecordedIdentityEntry* best = nullptr;
    for (uint16_t i = 0; i < t->count; ++i) {
        const RecordedIdentityEntry& e = t->ring[i];
        // The horizon is the playhead: a block written LATER in the match than the instant being
        // presented is not yet true, and dressing from it would show a loadout the player had not
        // swapped to yet.
        if (e.tUs > horizonUs_) continue;
#ifdef W3E_MUTATE_LAST_BLOCK_NOT_WINNING
        if (!best) best = &e;
#else
        if (!best || e.tUs > best->tUs || (e.tUs == best->tUs && e.seq > best->seq)) best = &e;
#endif
    }
    return best;
}

const char* RecordedIdentitySource::EntrantRefusal(uint32_t entrantSlot) const {
    const RecordedEntrantTrack* t = Track(entrantSlot);
    if (!t) {
        // Past the entrant bound an unknown slot and a dropped one are the same absence, so it is
        // named rather than reported as "this entrant was never recorded".
        return (tableFullFromUs_ && horizonUs_ >= tableFullFromUs_) ? kTableFull : nullptr;
    }
    if (t->capExceeded && horizonUs_ >= t->capExceededFromUs) return kCapExceeded;
    for (uint16_t i = 0; i < t->count; ++i) {
        if (t->ring[i].tUs <= horizonUs_) return nullptr;   // there is a current entry
    }
    // Recorded, but not at this instant.  Which absence it is depends on whether this entrant's
    // earlier blocks were evicted: "not yet" and "no longer held" are different repairs.
    if (t->evicted && horizonUs_ < t->oldestKeptUs) return kEvicted;
    return kNotYetReached;
}

bool ReadRecordedLoadout(const RecordedIdentitySource& src, uint32_t entrantIndex,
                         RecordedLoadout& out, const char** refusal) {
    out = RecordedLoadout{};
    out.filledPairCount = kRecordedPairCountUnknown;
    out.profile = RecordedProfileState::NotAsked;

    const RecordedIdentityEntry* e = src.Current(entrantIndex);
    if (!e) {
        // Four different absences, four different repairs.  The entrant-specific causes are asked
        // FIRST, and a file-wide refusal only speaks for an entrant when NOTHING decoded: on a file
        // where other entrants' blocks decoded fine, one bad block is not this entrant's story
        if (const char* mine = src.EntrantRefusal(entrantIndex)) {
            Say(refusal, mine == kNotYetReached ? mine : Composed(mine));
        } else if (src.Decoded()) {
            Say(refusal, "EntrantNotInBlock");
        } else if (src.Refused() && src.FirstRefusal()) {
            Say(refusal, Composed(src.FirstRefusal()));
        } else {
            Say(refusal, "IdentityBlockNotRecorded");
        }
        return false;
    }

    capture::IdentityBlockReader r;
    if (!r.Open(src.BlockBytes(*e), e->length)) {
        Say(refusal, Composed(capture::IdentityBlockRefusalText(r.Refusal())));
        return false;
    }
    capture::IdentityBlockEntrantView view{};
    if (!r.Entrant(e->blockEntrant, view)) {
        Say(refusal, "EntrantNotInBlock");
        return false;
    }

    // v1 recorded no key, and a position is not one.  A replay dressed from a guessed slot map
    // wears the wrong things and looks fine, so the array stays zero and the flag says why.
#ifdef W3E_MUTATE_V1_KEYS_GUESSED
    {
        capture::IdentityLoadoutSlot slots[capture::kIdentityBlockSlotCount];
        uint16_t n = 0;
        r.ResolveLoadout(e->blockEntrant, slots, n);
        for (uint16_t i = 0; i < kRecordedLoadoutPairCap; ++i) {
            out.pairs[i].slotKey = i;
            out.pairs[i].itemId = slots[i].itemId;
        }
        out.hasSlotKeys = true;
    }
#else
    if (r.HasSlotKeys()) {
        capture::IdentityLoadoutSlot slots[capture::kIdentityBlockSlotCount];
        uint16_t n = 0;
        if (r.ResolveLoadout(e->blockEntrant, slots, n)) {
            const uint16_t take = n < kRecordedLoadoutPairCap ? n : kRecordedLoadoutPairCap;
            for (uint16_t i = 0; i < take; ++i) {
                out.pairs[i].slotKey = slots[i].slotKey;
                out.pairs[i].itemId = slots[i].itemId;
            }
            out.hasSlotKeys = true;
        }
    }
#endif

    // The block reader answers kIdentityPairCountUnknown for every version written before the count
    // existed; it is carried through unflattened, because zero is the other, real answer.
    uint16_t count = kRecordedPairCountUnknown;
    if (r.FilledPairCount(e->blockEntrant, count)) out.filledPairCount = count;

    bool present = false;
    if (r.ProfileRecorded(e->blockEntrant, present)) {
        out.profile = present ? RecordedProfileState::Present : RecordedProfileState::Absent;
        if (present && view.jsonLen) {
            out.document = reinterpret_cast<const char*>(view.json);
            out.documentBytes = view.jsonLen;
        }
    } else if (r.Refusal() == capture::IdentityBlockRefusal::ProfileRefused) {
        out.profile = RecordedProfileState::Refused;
    } else {
        out.profile = RecordedProfileState::NotAsked;
    }
    Say(refusal, nullptr);
    return true;
}

void RecordedIdentityCensusLine(const RecordedIdentitySource& src, char* out, size_t cap) {
    if (!out || !cap) return;
#ifdef W3E_MUTATE_CENSUS_BLIND_EQUALS_EMPTY
    // The defect this line exists to prevent: a reader that saw nothing and a file that holds nothing
    // print the same sentence, and the field reads the first as the second.
    _snprintf_s(out, cap, _TRUNCATE, "identity blocks: %u offered, %u refused", src.Offered(),
                src.Refused());
#else
    char tap[32];
    if (src.TapIndex() == kRecordedTapIndexNone) {
        _snprintf_s(tap, sizeof tap, _TRUNCATE, "TapRowAbsent");
    } else {
        _snprintf_s(tap, sizeof tap, _TRUNCATE, "%u", src.TapIndex());
    }
    _snprintf_s(out, cap, _TRUNCATE,
                "identity scan: rows %u, tap chapters %u, tap index %s, offered %u, decoded %u, "
                "refused %u%s%s, evicted %u, entrants %u, v1 %u v2 %u v3 %u other %u",
                src.EventRows(), src.ChaptersWithTapTable(), tap, src.Offered(), src.Decoded(),
                src.Refused(), src.FirstRefusal() ? " first " : "",
                src.FirstRefusal() ? src.FirstRefusal() : "", src.Evicted(),
                (unsigned)src.SlotCount(), src.VersionDecoded(1), src.VersionDecoded(2),
                src.VersionDecoded(3), src.VersionDecoded(0));
#endif
}

uint32_t ScanRecordedIdentity(IDemoReader& reader, RecordedIdentitySource& into) {
    into.Reset();
    // The format's own widest body, so a record this buffer cannot hold is a file that is not this
    // format rather than a decode that desynchronises the rest of the chapter.
    static uint8_t body[capture::kMaxEventBodyBytes];
    uint32_t stored = 0;
    uint32_t eventRows = 0;
    uint32_t tapChapters = 0;
    uint16_t tapIndex = capture::kTapTypeIndexNone;
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) continue;
        const uint16_t here = IdentityTapIndex(dc);
        // A chapter with no keyframe carries no table; the one this file already published stands,
        // because the table is written into every keyframe by one writer and a row's index means
        // what that file said it means.  With no table seen yet, nothing is decoded at all -- the
        // alternative is a compiled-in index, which is the defect the standing rule names.
        if (here != capture::kTapTypeIndexNone) {
            tapIndex = here;
            ++tapChapters;
        }
        if (tapIndex == capture::kTapTypeIndexNone) continue;

        const StreamView* sv = dc.Find(kStreamEvent);
        if (!sv || !sv->records || sv->bytes <= capture::kBlockPrefixBytes) continue;
        bitio::BitReader r(sv->records + capture::kBlockPrefixBytes,
                           sv->bytes - capture::kBlockPrefixBytes);
        capture::EventDecoder dec;
        for (uint32_t k = 0; k < sv->recordCount; ++k) {
            capture::EventMessage m{};
            // A refused row leaves the reader off a record boundary, so the REST of this chapter's
            // events are unreadable: stop here rather than decode garbage.  The window build is
            // untouched by this -- events are not poses -- and the count says what was missed.
            if (!dec.Decode(r, m, body, sizeof body)) break;
            ++eventRows;
            if (m.typeIndex != tapIndex) continue;
            if (into.Offer(m.body, m.bodyBytes)) ++stored;
        }
    }
    into.NoteScan(eventRows, tapChapters,
                  tapIndex == capture::kTapTypeIndexNone ? kRecordedTapIndexNone : tapIndex);
    // The census is written HERE, off the accessors, so the three numbers that tell a blind reader
    // from an empty file cannot exist without a shipped path to the log.
    RecordedIdentityCensusLine(into, into.CensusBuffer(), RecordedIdentitySource::kCensusChars);
    return stored;
}

RecordedIdentitySource& PlaybackRecordedIdentity() {
    static RecordedIdentitySource g_source;
    return g_source;
}

}  // namespace identity
}  // namespace replay
