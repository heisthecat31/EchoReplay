#pragma once

// S_KEY's payload: a sequence of tagged blocks, not a positional struct.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS SHAPE, AND WHY IT HAD TO BE DECIDED BEFORE THE FIELD LIST WAS KNOWN.
//
// A keyframe is the seek target. S7.4 and rulings Q4 / Q-G require determinism PINS in it -- the Wwise
// seed and container history, the particle RNG, animation clip phase, trail state, per-body owner and
// sleep bit -- and W2-B, which has now designed seek, reports that version 1 carried the replicated
// half and none of those. Three of them are blocked on other packets and nobody can write them today.
//
// The cost of waiting is not that seek is late. It is that EVERY MATCH RECORDED IN THE MEANTIME can
// never carry them: a positional struct has no room a later reader could find, so adding a field is a
// format break and every file written before it becomes unreadable or wrong. A container decision is
// cheap now and impossible retroactively, which is why this landed before the fields it exists for.
//
// So the payload is:
//
//     [u32 magic][u32 version][u64 tUs][u16 blockCount][u16 reserved]
//     repeated blockCount times:  [u16 blockId][u32 len][len bytes]
//
// ★ THE PROPERTY BEING BOUGHT IS "SKIP UNKNOWN AND COUNT", and the second half matters as much as the
// first. A reader that silently ignores a block it does not understand cannot tell a file that carries
// nothing from a file whose contents it cannot read -- the same shape as a counter that counts its own
// polling. KeyframeReader therefore reports UnknownBlocks() and UnknownBytes(), so a player built
// before the audio pins existed can say "this file carries two blocks I do not understand" instead of
// seeking to a moment that quietly sounds wrong.
//
// ★ AND THE IDS BELOW ARE RESERVED NOW, unwritten, precisely so two packets adding pins in parallel
// cannot collide on a number. An id is claimed by appearing here, not by being written.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

// For MaxPinBytes(), which sizes the published block. A header, not a link edge: the encoder and
// the decoder both live in W2-H's library and this packet calls neither.
#include "audio/audiopins.h"

namespace replay {
namespace capture {

constexpr uint32_t kKeyframeMagic = 0x3159454Bu;   // 'KEY1' -- the container tag, not the version

// ★ Version 2 IS the block framing. Version 1 was the positional struct and is not readable by the
// loop below, so a reader refuses it by version rather than misparsing it into plausible values. No
// version-1 file exists outside this packet's own test output; if one ever turns up it is refused with
// its version named, which is the only honest thing a reader can do with a layout it cannot decode.
constexpr uint32_t kKeyframeVersion = 2;

enum KeyBlockId : uint16_t {
    // ---- written today: the replicated half ----------------------------------------------------
    kKeyBlockTick = 1,           // TickRec, plus whether one had been seen
    kKeyBlockPlayerWire = 2,     // {u16 slot, u64 stampUs, PlayerFrame} x n
    kKeyBlockPlayerSampled = 3,
    kKeyBlockBodyWire = 4,       // {u16 slot, u64 stampUs, BodyFrame} x n
    kKeyBlockBodySampled = 5,
    // RETIRED with S_WORLD (DESIGN S5.4.5). NOT WRITTEN, and the id is RESERVED for good by
    // S5.4.0's INV-STREAM-ID. A reader meeting it in a file recorded before this change skips it by
    // its byteLen and counts it, exactly as it would any other block this build does not know.
    kKeyBlockWorld_RETIRED = 6,
    kKeyBlockNetvar = 7,         // u16 n, then {u16 slot, u64 name, u64 raw SValue} x n
    // ★ THE EVENT TYPE TABLE, and it is here because without it the file cannot say what its own
    // S_EVENT rows mean. Every event row stores a typeIndex; that index points into the capture's
    // registered tap list, which lived only in the writer's source. Add, remove or reorder one entry
    // in that array and every file recorded before the change decodes its events as the WRONG message
    // type -- a goal reading as a stun, with nothing anywhere disagreeing. The table travels with the
    // recording instead. Written in every keyframe, so losing a chapter does not cost the vocabulary.
    // {u16 count} then count x {u16 index, u16 stream, u16 nameLen, nameLen bytes}.
    kKeyBlockTapTable = 8,

    // The lobby roster as of this keyframe, at the width the ENGINE uses. It is here rather than in
    // MANIFEST's PlayerRecord rows for two reasons, and only the first is about ownership:
    //
    //   1. B.0's PlayerRecord declares displayName and platformStr as char[32] when the engine field is
    //      36 (libr15 CNSLobby::EntrantStateCB compares a 0x24-byte CSysString), and teamId as uint8_t
    //      when the wire value is a u16 whose 0xFFFF means NO TEAM. Writing the roster through those
    //      fields truncates a legal 36-character name and turns "no team" into team 255 -- a value that
    //      is not the sentinel and is not distinguishable from a real one. That is INV-W1 twice over
    //      (S5.4.1a, the same defect it already fixed for `alignment`), and it is RAISED, not worked
    //      around: this block carries all three at full width so nothing is lost while B.0 is frozen.
    //   2. A manifest holds ONE roster for the whole file, so it cannot express a team change at 4:12.
    //      A roster belongs at a seek target, which is what a keyframe is.
    //
    // {u16 count}{ per entrant: u64 platformCode, u64 accountId, u64 sessionId, u64 firstSeenUs,
    //             u64 lastSeenUs, u16 entrantIndex, u16 generation, u16 team, u16 marker,
    //             u8 active, u8 hasProfileJson, u16 nameLen, name bytes, u16 platLen, plat bytes }
    kKeyBlockRoster = 9,

    // The world re-origin shift as of this chapter: the CUMULATIVE offset, absolute, never
    // accumulated by the reader. {float x, y, z}{u8 valid}{u8 suspect}.
    //
    // A chapter carries exactly ONE, because a change in it ends the chapter (see chapterbuilder's
    // kRecWorldOrigin case). So playback is one constant add per chapter rather than a per-record
    // correction nobody could place in time.
    //
    // ★ `valid` is written even when 0, and that is the point of the field. An absent block and a
    // zero vector are different claims -- "this build could not source the offset" is not "the world
    // is at the origin" -- and recording the second while meaning the first is exactly the failure
    // W2-B R5's address would have shipped: 0x1420A27C0 is NRadEngine::C3Vector::zero, 1187 reads and
    // zero writes image-wide, in the loader-zeroed tail of .data. docs/spikes/REORIGIN.md refuted it
    // and supplied the real base and offsets; capture/worldorigin.h holds the read.
    //
    // `suspect` says the triple may not have been read at one instant. The engine writes x, y and z
    // with three unfenced stores and we read from another thread.
    kKeyBlockWorldOrigin = 10,

    // ★ HOW MANY OF THE CAPTURE'S OWN SURFACES WERE LIVE WHEN THIS KEYFRAME WAS WRITTEN, and it is
    // here because without it a file has no DENOMINATOR. "S_EVENT holds 0 records" is not
    // distinguishable from "the tap for it was never registered" by reading the file: the tap TABLE
    // above says which types the capture COULD have recorded, which is one level up from whether a
    // listener was live. A completeness verdict read off the file is unsupported without this
    // And registration finishes over the first ticks of a session, so the count
    // belongs at a seek target rather than once in the manifest.
    //
    // {u16 registered, u16 retrying, u16 givenUp, u16 unavailable, u16 excludedByDesign,
    //  u16 total, u16 tableRows, u8 started}. registered + retrying + givenUp + unavailable == total
    // and total + excludedByDesign == tableRows, so a reader can check the row it is handed.
    kKeyBlockTapRegistration = 11,

    // ---- reserved for the determinism pins S7.4 and rulings Q4 / Q-G require --------------------
    // NOT WRITTEN, and their absence from a file is the honest state today: three are blocked on
    // packets that have not reported. They are numbered here so that two packets adding pins in
    // parallel cannot pick the same id, and so a reader can name what it is missing rather than
    // discovering an unknown number.
    // WRITTEN, at keyframe cadence, from the snapshot the tick thread publishes: audio::EncodePins'
    // output verbatim (states, per-emitter switches, sustained loops with their phase, the Wwise
    // seed and container history). See PublishAudioPins below.
    kKeyBlockAudioPins = 16,        // Wwise seed + container history (ruling Q4)
    kKeyBlockParticleRng = 17,      // W1-I / W3-A: particle RNG state (S7.4.3 N-9)
    kKeyBlockAnimPhase = 18,        // animation clip phase per entity
    kKeyBlockTrailState = 19,       // trail / ribbon state
    kKeyBlockBodyOwnerSleep = 20,   // per-body owner id and CPhBody sleep bit

    kKeyBlockFirstReserved = 16,
    kKeyBlockLastReserved = 63,     // 64+ is free for anything nobody has thought of yet
};
const char* KeyBlockName(uint16_t);

// kKeyBlockTapRegistration, written and read in one place. A checker with its own copy of a layout
// certifies broken files, so the writer in chapterbuilder and every reader go through this pair --
// including a reader outside this packet, which is what `echodemo verify` needs to print the row.
// Read returns false on a short or self-inconsistent block rather than filling in plausible zeros.
struct TapRegistrationBlock {
    uint16_t registered, deferred, givenUp, unavailable, excluded, total, tableRows;
    uint8_t  started;
};
static_assert(sizeof(TapRegistrationBlock) == 16, "the block is 7 u16 and a u8, padded to 16");
constexpr uint32_t kTapRegistrationBlockBytes = 15;   // on the wire, unpadded

class KeyframeBlobWriter;
void PutTapRegistrationBlock(KeyframeBlobWriter&, const TapRegistrationBlock&);
bool ReadTapRegistrationBlock(const uint8_t* data, uint32_t len, TapRegistrationBlock& out);

// ---- kKeyBlockAudioPins: published by one thread, written by another ------------------------
//
// The pin walk reads engine memory, which is legal on the TICK thread and on no other, and the
// keyframe is written by the chapter worker. So the two halves split exactly the way
// PublishTapRegistration / TapRegistrationNow do (manifestbuild.h), and for the same reason.
//
// What travels is audio::EncodePins' output VERBATIM. The reader hands the whole block to
// audio::DecodePins (playback/core/windowbuild.cpp), so a byte of framing of our own here would
// make every pin block this capture writes undecodable.
//
// PublishAudioPins is TICK THREAD ONLY. n == 0 publishes "there are no pins to give" -- a session
// with no audio state bound -- which is not the claim a zero-loop pin block makes.
void PublishAudioPins(const void* bytes, uint32_t n);

// Copies the last published block into caller memory. Returns its length, or 0 when nothing has
// been published, when the block does not fit, or when the publisher was mid-write throughout the
// read: a torn pin block is refused, never handed on.
uint32_t AudioPinsNow(void* dst, uint32_t cap);
void ResetAudioPinsForTest();

#pragma pack(push, 1)
struct KeyframeHeaderDisk {
    uint32_t magic;
    uint32_t version;
    uint64_t tUs;
    uint16_t blockCount;
    uint16_t reserved;
};
struct KeyBlockHeaderDisk {
    uint16_t blockId;
    uint32_t len;
};
#pragma pack(pop)
static_assert(sizeof(KeyframeHeaderDisk) == 20, "keyframe header is 20 bytes");
static_assert(sizeof(KeyBlockHeaderDisk) == 6, "block header is 6 bytes");

// Writes into caller-owned memory; allocates nothing. Ok() goes false on the first overflow and stays
// false, and the caller must not write a short keyframe -- a truncated seek target decodes into
// plausible nonsense, where a missing one is at least visible.
class KeyframeBlobWriter {
public:
    KeyframeBlobWriter(void* dst, size_t cap, uint64_t tUs);

    // Opens a block; every Put after it lands inside that block until the next Block() or Finish().
    void Block(uint16_t blockId);
    void Raw(const void* src, size_t n);
    template <class T>
    void Put(const T& v) { Raw(&v, sizeof v); }

    // Closes the last block and backfills the header. Returns the byte count, or 0 if anything
    // overflowed.
    size_t Finish();
    bool Ok() const { return ok_; }
    size_t Used() const { return at_; }

private:
    void CloseBlock();

    uint8_t* dst_;
    size_t cap_;
    size_t at_ = 0;
    size_t blockAt_ = 0;      // offset of the open block's header, 0 when none is open
    uint16_t blockCount_ = 0;
    bool ok_ = true;
};

// Walks the blocks. Unknown ids are the point: Next hands them over with their length so a caller can
// skip by length, and the counters below say how much was skipped.
class KeyframeReader {
public:
    // False if the blob is too short, the magic is wrong, or the version is one this build cannot
    // read. Refusal() then names which.
    bool Open(const void* blob, size_t bytes);

    bool Next(uint16_t& blockId, const uint8_t*& data, uint32_t& len);

    uint64_t TimeUs() const { return tUs_; }
    uint32_t Version() const { return version_; }
    uint16_t BlockCount() const { return blockCount_; }
    const char* Refusal() const { return refusal_; }

    // What a caller skipped. NoteUnknown is called by the consumer, not by Next, because only the
    // consumer knows which ids it understands -- and a reader that decided that for you would be
    // reporting its own opinion rather than the caller's coverage.
    void NoteUnknown(uint32_t len) { ++unknownBlocks_; unknownBytes_ += len; }
    uint32_t UnknownBlocks() const { return unknownBlocks_; }
    uint64_t UnknownBytes() const { return unknownBytes_; }
    // True when the block table was consistent: blockCount blocks, ending exactly at the blob's end.
    bool Consistent() const { return consistent_; }

private:
    const uint8_t* p_ = nullptr;
    size_t bytes_ = 0;
    size_t at_ = 0;
    uint64_t tUs_ = 0;
    uint32_t version_ = 0;
    uint16_t blockCount_ = 0;
    uint16_t read_ = 0;
    uint32_t unknownBlocks_ = 0;
    uint64_t unknownBytes_ = 0;
    bool consistent_ = false;
    const char* refusal_ = nullptr;
};

}  // namespace capture
}  // namespace replay
