#pragma once
// THE PLAYBACK SIDE OF THE IDENTITY BLOCK: what the recording says an entrant was wearing.
//
// Capture writes one IDN1 block per entrant into S_EVENT whenever that entrant's identity changes
// (src/capture/identity_emit.cpp).  Until this file NOTHING decoded one off disk, so every recorded
// loadout key stopped at the container and the provisioner dressed from positions -- the exact
// fidelity loss the pair format exists to stop.
//
// THE LAYERING RULE THIS HEADER OBEYS.  src/engine reads this contract (PawnProvisioner's consumer),
// and identity_access.h already includes pawn_provision.h, so a capture-side include here would
// invert the layering and make the engine name the on-disk structs.  Therefore: stdint only.  Every
// number below is pinned to the capture constant it mirrors by a static_assert in the .cpp, which is
// the one translation unit that may name both sides.
//
// TIME, AND WHY A MID-REPLAY LOADOUT CHANGE IS HONOURED.  A block carries TWO times and they are on
// different axes.  The S_EVENT row's stamp is the tap's ARRIVAL axis (capture.cpp: WiretapArrivalNowUs)
// and is not comparable with anything the transport reports.  The block's own header carries the
// CAPTURE axis instant the identity was gathered at -- the same axis as the playhead -- so that is the
// one selection uses, and this store never looks at the row stamp at all.  The source holds the
// blocks the file carries PER ENTRANT; `SetHorizonUs` is the playhead, and each read answers with the
// LATEST block at or before it.  A player who swaps a loadout at 4:12 therefore wears the old one
// until the playhead reaches 4:12 and the new one after, in both directions, because the selection is
// a search and not a running state that a backward seek would have to undo.

#include <stddef.h>
#include <stdint.h>

namespace replay {

class IDemoReader;
struct DecodedChapter;

namespace identity {

// ---- the contract the provisioner's consumer codes against ------------------------------------

// The width of the recorded pair array.  Asserted equal to capture::kIdentityBlockSlotCount.
constexpr uint16_t kRecordedLoadoutPairCap = 20;

// "Could not ask": an older block that carries no count, or a capture whose scan could not read one.
// Derived rather than transcribed, and asserted equal to capture::kIdentityPairCountUnknown -- zero
// is the different, real answer "the component held nothing" and must not collapse into this one.
constexpr uint16_t kRecordedPairCountUnknown = uint16_t(~uint16_t(0));

struct RecordedLoadoutPair {
    uint64_t slotKey;
    uint64_t itemId;
};

// FOUR states, because the first two are the same zero bytes on disk: a document the engine refused
// to hand over and an entrant who genuinely has none differ only in the flags the block records.
enum class RecordedProfileState : uint8_t { NotAsked, Present, Absent, Refused };

// On THIS side of the contract too (part2 F13): every member defaults to its
// COULD-NOT-ASK value, so a value-initialised record is a question nobody asked rather than an
// entrant who was asked and wears nothing.  Zero pairs is a real, different answer -- the reader
// only ever writes it off a block that said so -- and a construction site that forgets the ritual
// must not manufacture it.  Layout is unchanged; these are initialisers, not fields.
struct RecordedLoadout {
    uint16_t filledPairCount = kRecordedPairCountUnknown;  // could not ask (older block / unknown)
    bool     hasSlotKeys = false;   // v1 block: keys were not recorded, and none are invented
    RecordedProfileState profile = RecordedProfileState::NotAsked;
    RecordedLoadoutPair pairs[kRecordedLoadoutPairCap] = {};  // valid [0, min(filledPairCount,cap)) when hasSlotKeys
    // Points INTO the source's own copy of the block and lives exactly as long as the source does.
    const char* document = nullptr;
    uint32_t    documentBytes = 0;
};

// ---- the store ---------------------------------------------------------------------------------
//
// ONE BLOCK IS ONE ENTRANT (identity_emit.cpp builds a writer, Adds one entrant and sinks it, inside
// its per-entrant loop).  So a per-FILE block cap is really a per-entrant-record cap, and past it an
// entrant that already had an entry would go on answering with its FIRST loadout for the rest of the
// match -- true, unrefused and stale.  The bounds are therefore per entrant, and every one of them
// refuses BY NAME when it is hit.

// The entrant table's own width: asserted equal to capture::kIdentityMaxEntrants, the widest entrant
// list that can reach this format.
constexpr uint16_t kRecordedEntrantCap = 64;
// How many blocks one entrant may keep.  A loadout change writes a block, so this is the depth of
// history a seek can reach BACK through; older ones are evicted and the eviction is named, never
// silently answered from a block that is no longer the latest before the playhead.
constexpr uint16_t kRecordedBlocksPerEntrant = 8;
// Fixed cells, recycled through a free list, so an evicted block gives its bytes back.  DERIVED
// from the two bounds above and not chosen: a smaller pool is a THIRD, invisible bound -- the ring
// only recycles a cell once it is full, so a pool short of entrants x depth denies a cell to an
// entrant whose own ring has room, and that entrant is then refused at instants its held blocks
// answer correctly.
constexpr uint16_t kRecordedBlockCells = kRecordedEntrantCap * kRecordedBlocksPerEntrant;
// >= capture::kMaxEventBodyBytes, asserted in the .cpp. It tracks the format's document cap, which
// RECORDER-5 raised from 4 KiB to 16 KiB after a live capture lost 8 of 9 profile documents to a
// buffer shorter than a real one; the pool this sizes is 512 cells, so the cap and this number are
// the two halves of one memory decision.
constexpr uint32_t kRecordedCellBytes = 17408;
constexpr uint16_t kRecordedCellNone = uint16_t(~uint16_t(0));
// No tap row for the identity block was found in any keyframe this file carries.
constexpr uint32_t kRecordedTapIndexNone = uint32_t(~uint32_t(0));
// Versions counted separately in the census.  Index 0 is "a version this build has no column for".
constexpr uint16_t kRecordedVersionColumns = 4;

struct RecordedIdentityEntry {
    uint32_t entrantSlot;    // the RECORDED userSlot -- see ReadRecordedLoadout
    uint16_t blockEntrant;   // which record inside the block this entry names
    uint32_t version;        // the block's own layout version, for the census line
    uint64_t tUs;            // the block header's capture-axis time
    uint16_t cell;           // which arena cell holds the bytes
    uint32_t length;
    uint32_t seq;            // feed order: the tiebreak when two blocks share an instant
};

// One entrant's blocks, newest last.
struct RecordedEntrantTrack {
    bool     used = false;
    uint32_t slot = 0;
    uint16_t count = 0;
    RecordedIdentityEntry ring[kRecordedBlocksPerEntrant] = {};
    uint32_t evicted = 0;       // dropped off the front to make room
    uint64_t oldestKeptUs = 0;  // so "your block was evicted" can be told from "not yet"
    // No cell was free at this instant.  HORIZON-AWARE: the blocks this entrant already holds are
    // still true before it, so only a playhead at or after the dropped block refuses -- a bound hit
    // at 6:00 must not blank the loadout the file recorded at 0:30.
    bool     capExceeded = false;
    uint64_t capExceededFromUs = 0;
};

// Filled by decoding the IDN1 bodies out of S_EVENT as the demo is read.  Copies the bytes, because
// a DecodedChapter's pointers survive exactly one further DecodeChapter (demofile.h:34).
struct RecordedIdentitySource {
    void Reset();

    // One S_EVENT body whose typeIndex resolved to the identity tap row.  The block's own header
    // decides where it lands in time; the row's arrival stamp is deliberately not taken.
    bool Offer(const void* body, uint32_t bytes);

    // The playhead, on the capture axis.  Reads answer with the latest block at or before it.
    void SetHorizonUs(uint64_t tUs) { horizonUs_ = tUs; }
    uint64_t HorizonUs() const { return horizonUs_; }

    // The entry a read for `entrantSlot` resolves to right now, or null.
    const RecordedIdentityEntry* Current(uint32_t entrantSlot) const;
    const uint8_t* BlockBytes(const RecordedIdentityEntry& e) const {
        return cells_ + size_t(e.cell) * kRecordedCellBytes;
    }
    // Why THIS entrant has no current entry, by name, or null when the cause is not entrant-specific.
    // Program-lifetime strings.
    const char* EntrantRefusal(uint32_t entrantSlot) const;

    // Every distinct recorded slot the file carried, for the load-time census.
    uint16_t SlotCount() const { return slotCount_; }
    uint32_t SlotAt(uint16_t i) const;

    // THE CONTROL THAT MAKES A ZERO READABLE.  "No block was stored" has three causes -- the file
    // carries none, the file's tap table never named the identity row, or the event stream did not
    // decode -- and they are three different repairs.  The scan records all three and the census line
    // PRINTS all three, so a blind reader cannot read the same as an empty file.
    uint32_t EventRows() const { return eventRows_; }
    uint32_t ChaptersWithTapTable() const { return tapChapters_; }
    uint32_t TapIndex() const { return tapIndex_; }
    void NoteScan(uint32_t eventRows, uint32_t tapChapters, uint32_t tapIndex);

    uint32_t Offered() const { return offered_; }
    uint32_t Decoded() const { return decoded_; }
    uint32_t Refused() const { return refused_; }
    uint32_t Evicted() const { return evicted_; }
    // The entrant TABLE filled: blocks for entrants past the format's own entrant bound were
    // dropped, so from that instant on an unknown slot cannot be told from a dropped one and says so.
    bool EntrantTableFull() const { return tableFullFromUs_ != 0; }
    // Blocks decoded at one layout version; index 0 collects versions with no column here.
    uint32_t VersionDecoded(uint16_t column) const {
        return column < kRecordedVersionColumns ? versions_[column] : 0;
    }
    // The first refusal, by name.  A block whose VERSION this build cannot derive cannot be
    // attributed to an entrant -- its layout is exactly what is unknown -- so it is recorded here.
    const char* FirstRefusal() const { return firstRefusal_; }
    // The whole scan in one line, written by ScanRecordedIdentity: rows, tap chapters, tap index,
    // the offered/decoded/refused counts and the per-version breakdown.  Never empty after a scan.
    const char* CensusLine() const { return census_; }
    char* CensusBuffer() { return census_; }
    static constexpr size_t kCensusChars = 224;

private:
    RecordedEntrantTrack* Track(uint32_t slot);
    const RecordedEntrantTrack* Track(uint32_t slot) const;
    uint16_t TakeCell();
    void GiveCell(uint16_t cell);

    RecordedEntrantTrack tracks_[kRecordedEntrantCap] = {};
    uint8_t  cells_[size_t(kRecordedBlockCells) * kRecordedCellBytes] = {};
    bool     cellUsed_[kRecordedBlockCells] = {};
    uint32_t offered_ = 0;
    uint32_t decoded_ = 0;
    uint32_t refused_ = 0;
    uint32_t evicted_ = 0;
    uint32_t versions_[kRecordedVersionColumns] = {};
    uint32_t seq_ = 0;
    uint64_t horizonUs_ = 0;
    uint16_t slotCount_ = 0;
    uint32_t eventRows_ = 0;
    uint32_t tapChapters_ = 0;
    uint32_t tapIndex_ = kRecordedTapIndexNone;
    uint64_t tableFullFromUs_ = 0;
    const char* firstRefusal_ = nullptr;
    char census_[kCensusChars] = {};
};

// `entrantIndex` IS THE RECORDED USER SLOT -- the number the block's IdentityEntrant::userSlot
// carries, the number the capture census prints as "slot N", and the number
// PawnProvisioner::AssignMappedSlot takes as its `recorded` argument.  It is NOT a live slot and
// NOT a position in any array this build holds.
//
// False with a NAMED refusal, never a shrugged default.  EVERY refusal string this returns is a
// program-lifetime pointer -- composed once per cause into a static table -- so a caller may hold it
// across ticks (pawn_provision.cpp keeps it in `loadoutSourceWhy`), which a shared scratch buffer
// would have quietly invalidated on the next refused read.
//   IdentityBlockNotRecorded    nothing in this file carried an identity block at all
//   EntrantNotInBlock           blocks were decoded, none of them for this entrant
//   RecordedBlockNotYetReached  this entrant IS recorded, but only after the playhead
//   IdentityBlockRefused:<why>  a block was refused -- <why> is IdentityBlockRefusalText's word, or
//                               RecordedBlockCapExceeded / RecordedBlockEvicted for a bound this
//                               store hit, which refuses instead of answering from a stale block
bool ReadRecordedLoadout(const RecordedIdentitySource& src, uint32_t entrantIndex,
                         RecordedLoadout& out, const char** refusal);

// The scan, in one line, into `out`.  Reads every control accessor, so the numbers that tell a blind
// reader from an empty file cannot exist without reaching the log.
void RecordedIdentityCensusLine(const RecordedIdentitySource& src, char* out, size_t cap);

// The index a named tap row has in this chapter's keyframe tap table, or capture::kTapTypeIndexNone
// when the chapter carries no table or no such row. `scratch` holds the decoded keyframe; a keyframe
// larger than it answers none.
inline constexpr size_t kTapTableScratchBytes = 64 * 1024;
uint16_t TapIndexForName(const DecodedChapter& dc, const char* name, size_t nameChars,
                         uint8_t* scratch, size_t scratchBytes);

// Walks every chapter's S_EVENT stream and offers the identity bodies to `into`.  The event type
// index is resolved from the FILE'S OWN tap table in the keyframe, never from a compiled-in one: an
// S_EVENT row's typeIndex is an index into the table that was live when the row was written, and a
// compiled table is right until somebody reorders it and then silently wrong for every file already
// recorded (windowbuild.h's standing rule).  Returns the blocks stored, and leaves CensusLine() set.
uint32_t ScanRecordedIdentity(IDemoReader& reader, RecordedIdentitySource& into);

// The instance the shipped playback load path fills and the provisioner's consumer reads.  One per
// process, like the transport: a session plays one file at a time.
RecordedIdentitySource& PlaybackRecordedIdentity();

}  // namespace identity
}  // namespace replay
