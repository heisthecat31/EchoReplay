#pragma once

// The .echodemo stream codec: the bit layout of every CORE / ASSEEN record this packet writes, and its
// exact inverse.
//
// ---------------------------------------------------------------------------------------------
// ★ ENCODE AND DECODE ARE ONE FUNCTION, WALKED TWICE.
//
// A record whose writer and reader are two separate listings desynchronises the first time one of them
// gains a field, and the failure is silent: every record after the divergence decodes as garbage that
// is still structurally valid. So each record has exactly ONE field walk, templated on a codec that is
// either a writer or a reader. Adding a field to the writer without adding it to the reader is not a
// bug this code can have, because there is only one place to add it.
//
// Two further guards, because symmetry alone would not catch a wrong WIDTH:
//   * every stream block records the exact bit length its records occupy, and the decoder refuses the
//     block if the cursor does not land there after `stored` records;
//   * tests/capture round-trips every record shape, including the awkward values (NaN, +/-0, denormals,
//     +/-inf, the top and bottom of every quantisation domain).
//
// ★ AND THE WIRE PATH CARRIES A THIRD, WHICH IS THE ONE THE LOSSLESS RULE RESTS ON.
// A record that arrived as engine-packed bits is stored in decoded form ONLY IF re-encoding the decoded
// values through the engine mirror reproduces the source body bit for bit, with every remaining bit of
// the body zero. A record that fails that test is stored VERBATIM instead -- 64 bytes rather than ~15,
// and the count is reported, because a non-zero count means our field schedule disagrees with the
// engine's and somebody needs to know. Nothing is ever dropped or approximated to make the check pass.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "capture/capture_types.h"
#include "capture/manifestbuild.h"   // kMaxEventBodyBytes: the bound the encoder and decoder share
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demotypes.h"

namespace replay {
namespace capture {

// ---- the storage frames ----------------------------------------------------------------------
// These are what a record decodes TO. S5.3.1: the wire and sampled streams decode to the identical
// struct, so the player cannot branch on where a sample came from -- there is no origin field here.

// ---- the per-row fill mask ---------------------------------------------------------------------
// Which fields of a PlayerFrame were actually SOURCED. A bit that is clear means the field is zero
// because nothing supplied it, not because it measured zero.
//
// ★ THIS IS THE LOSSLESS INVARIANT AT FIELD GRANULARITY, and it is the same rule S5.6.2 states over
// records: a reader holding only the file must be able to tell what is missing. AsSeenSample::valid
// and IEntity::ReadLocalPlayer's return already work this way, for the reason S12.0 gives -- a wrong
// replay renders happily and silently. Without this, `trigL = 0.0` from a source that does not exist
// is indistinguishable in the file from a trigger genuinely at rest, and a player would reproduce the
// second when the truth was the first.
//
// The groups are the granularity at which a SOURCE is present or absent -- one engine accessor each --
// because that is the only granularity at which the answer is ever different.
enum PlayerFill : uint32_t {
    kFillPose = 1u << 0,        // hmdOri, lHandOri, rHandOri, lHandOff, rHandOff
    kFillRefVel = 1u << 1,      // refVel
    kFillBody = 1u << 2,        // bendBack, bendFwd, restPoseHeight
    kFillTrigger = 1u << 3,     // trigL, trigR
    kFillAnchor = 1u << 4,      // anchor
    kFillActionWord = 1u << 5,  // bits28
    kFillHolding = 1u << 6,     // holdLSlot/Gen, holdRSlot/Gen
    kFillHeader = 1u << 7,      // hdr0 -- wire rows only
    kFillRefVelDup = 1u << 8,   // refVelDup -- wire rows only

    // A row decoded from a wire body has every one of them: the message carries the whole
    // SR15NetPlayerState schedule, trigger blends and anchor included, and DecodePlayerBody proves it
    // by re-encoding to the source bits.
    kFillWireAll = kFillPose | kFillRefVel | kFillBody | kFillTrigger | kFillAnchor |
                   kFillActionWord | kFillHolding | kFillHeader | kFillRefVelDup,
};
const char* PlayerFillName(uint32_t bit);

struct PlayerFrame {
    float hmdOri[4], lHandOri[4], rHandOri[4];
    float refVel[3];
    float refVelDup[3];   // the wire carries refvelocity twice; see hasDup
    float lHandOff[3], rHandOff[3];
    float bendBack, bendFwd, restPoseHeight, trigL, trigR;
    uint32_t bits28;      // stored raw and never decoded (S5.4.5 rule 1)
    uint32_t holdLSlot, holdLGen, holdRSlot, holdRGen;
    uint32_t anchor;
    // The message header word that precedes the packed record on the wire, stored rather than decoded.
    // Nothing on this build establishes what it holds -- the corpus shows an owner-looking dword beside
    // a level-looking one -- and a capture format is the wrong place to bake in a reading nobody has.
    // It is delta-coded, so once it stops changing it costs one bit. Wire records only; see hasDup.
    uint64_t hdr0;
    // False on a sampled record, where there is one refvelocity and no message header, and inventing
    // either would put a value in the file that nothing measured.
    //
    // ★ hasDup and `fill` answer DIFFERENT questions and both are needed. hasDup is about the SCHEMA
    // -- does this stream carry the field at all. `fill` is about the SOURCE -- was the value it
    // carries a measurement. A field can be in the schema and unsourced, which is exactly the case
    // this mask exists for.
    bool hasDup;
    uint32_t fill;
};

// Which fields of a BodyFrame were sourced, on the same rule as PlayerFill: a clear bit means the
// field is zero because nothing supplied it.
//
// ★ THE IDENTITY OF A NETWORKED BODY IS A TAGGED UNION, and storing the value without the tag makes
// it unresolvable. SR15NetPhysicsSimState is 80 bytes and the dword at +0x4C selects how the qword at
// +0x48 is read:
//
//     tag == 0   an SR15NetUserId: u16 slot, u16 generation   (read as TWO u16 at +0x48 / +0x4A)
//     tag == 1   a component id
//     tag >= 2   a u64 net id, resolved through the net-actor table
//
// The same eight bytes are a qword on one arm of that branch and two u16 on another. So a capture that
// stores idValue alone has recorded a number whose meaning is gone -- and this half is unfixable
// retroactively, which is why it landed before the recordings.
//
// ★ AND NOTE WHAT IT SAYS ABOUT THE PREVIOUS TWO CORRECTIONS. This packet moved S_WORLD and
// S_BODY_SAMPLED from a local slot to "the net id", and "the net id" is the right reading for only one
// of three tag values. A key that is correct for the common case and silently wrong for the others is
// the alignment sentinel one level up: it works in every test anyone would write today.
enum BodyFill : uint32_t {
    kFillBodyTransform = 1u << 0,   // com, ori, vel, angVel
    kFillBodyLastHeld = 1u << 1,    // lastHeldCom
    kFillBodyBits = 1u << 2,        // bits11

    // ---- identity, and it takes more than one bit because it takes more than one field ----------
    kFillBodyIdValue = 1u << 3,     // idValue was sourced
    kFillBodyIdTag = 1u << 5,       // idTag was read from the CI, so idValue's MEANING is known
    // The value came from IEntity::NetIdOf, which resolves through the net-actor table and therefore
    // yields a net id whatever tag byte the component happened to hold. Distinct from kFillBodyIdTag
    // because it is a guarantee about the ACCESSOR, not a byte anybody read.
    kFillBodyIdNetActor = 1u << 6,

    kFillBodyHeader = 1u << 4,      // hdr0, hdr2 -- wire rows only

    kFillBodyWireAll = kFillBodyTransform | kFillBodyLastHeld | kFillBodyBits | kFillBodyIdValue |
                       kFillBodyHeader,
};

// ★ THE RULE A READER MUST APPLY, in one place so it is not restated three ways. An identity is
// resolvable only when the value is present AND something says how to read it -- either the raw tag,
// or the net-actor guarantee. Value alone is a number with no meaning and must not be keyed on.
inline bool BodyIdentityResolvable(uint32_t fill) {
    return (fill & kFillBodyIdValue) != 0 &&
           (fill & (kFillBodyIdTag | kFillBodyIdNetActor)) != 0;
}
const char* BodyFillName(uint32_t bit);

struct BodyFrame {
    // ★ THE KEY, and it is TWO fields. idValue is the qword at SimState+0x48; idTag is the dword at
    // +0x4C that says whether it is a user id, a component id or a net id. Read them only through
    // BodyIdentityResolvable above -- idValue on its own is a number whose meaning was not recorded.
    uint64_t idValue;
    uint32_t idTag;
    float com[3], ori[4], vel[3], angVel[3], lastHeldCom[3];
    uint32_t bits11;
    uint32_t fill;
    // The two header words either side of the source stamp, same reasoning as PlayerFrame::hdr0.
    uint64_t hdr0, hdr2;
    bool hasHeader;
};

struct NetvarRow {
    uint64_t name;
    uint64_t value;   // the raw SValue qword, verbatim
    uint16_t slot;
};

// ---- schemas ---------------------------------------------------------------------------------
// The manifest declares these; a reader meeting an unknown schema refuses the stream by name (S5.9).
// Every one is Retention::All, which is the only legal value (S5.6.2.0 rule 1).
const SchemaDef* Schemas();
uint16_t SchemaCount();
const SchemaDef* SchemaById(SchemaId);

// Non-null if any shipped schema is malformed -- an unpinned field, a WireQ field on a sampled stream,
// a retention that is not All. Checked at capture start and by tests/capture, so a bad table refuses
// before a file exists rather than after one is unreadable.
const char* SchemaTableRefusal();

// ---- record framing --------------------------------------------------------------------------
// A stream block is: [u32 bitLength][packed records]. The length is what lets the decoder prove it
// consumed exactly what the encoder produced instead of assuming it.
constexpr size_t kBlockPrefixBytes = 4;

// ---- the encoders ----------------------------------------------------------------------------
// One per stream, each holding the delta state its stream needs. Every one is reset at a chapter
// boundary, which is what makes a chapter independently decodable and therefore a legal seek target.

constexpr uint16_t kNameDictMax = 128;  // 79 netvars plus room; the dictionary is per chapter

// What a decoded entity record yields. `verbatim` is the honest half of the wire path: a record whose
// decode could not be proved is stored whole, and the reader is told so rather than handed a frame
// nobody verified.
struct DecodedPlayer {
    uint16_t entity;
    uint64_t stampUs;
    bool verbatim;
    PlayerFrame frame;        // valid when !verbatim
    uint16_t bodyBytes;       // valid when verbatim
    const uint8_t* body;
};

struct DecodedBody {
    uint16_t entity;
    uint64_t stampUs;
    bool verbatim;
    BodyFrame frame;
    uint16_t bodyBytes;
    const uint8_t* body;
};

class PlayerEncoder {
public:
    explicit PlayerEncoder(bool sampled) : sampled_(sampled) {}
    void Reset();
    // False if the writer refused; the caller must abandon the whole block, because a half-written
    // record desynchronises everything after it.
    bool Encode(bitio::BitWriter&, uint16_t entity, uint64_t stampUs, const PlayerFrame&);
    // The fallback for a wire body whose decode did not reproduce the source. Delta state is left
    // untouched, because there is no verified frame to delta the next record against.
    bool EncodeVerbatim(bitio::BitWriter&, uint16_t entity, uint64_t stampUs, const void* body,
                        uint16_t bodyBytes);

private:
    bool sampled_;
    bool seen_[kMaxEntities] = {};
    PlayerFrame prev_[kMaxEntities] = {};
    uint64_t prevStamp_[kMaxEntities] = {};
    uint16_t lastEntity_ = 0;
};

class PlayerDecoder {
public:
    explicit PlayerDecoder(bool sampled) : sampled_(sampled) {}
    void Reset();
    bool Decode(bitio::BitReader&, DecodedPlayer&, uint8_t* bodyBuf, size_t bodyCap);

private:
    bool sampled_;
    bool seen_[kMaxEntities] = {};
    PlayerFrame prev_[kMaxEntities] = {};
    uint64_t prevStamp_[kMaxEntities] = {};
    uint16_t lastEntity_ = 0;
};

// A write bitio refused, named. `BitWriter::RefusalReason()` is a sentence about the WRITER; this
// says which FIELD of the record could not be stored, what it held, and how wide the format
// declares it -- the three things that tell a value which outgrew its width from a buffer that ran
// out. `identified` is false when the refusal happened where the record carries no field (a flag,
// or the buffer itself): the name is then unknown, which is not the same as "no field was involved".
struct RefusedField {
    const char* name = nullptr;
    uint64_t value = 0;
    uint32_t width = 0;
    bool identified = false;
};

class BodyEncoder {
public:
    explicit BodyEncoder(bool sampled) : sampled_(sampled) {}
    void Reset();
    bool Encode(bitio::BitWriter&, uint16_t entity, uint64_t stampUs, const BodyFrame&);
    bool EncodeVerbatim(bitio::BitWriter&, uint16_t entity, uint64_t stampUs, const void* body,
                        uint16_t bodyBytes);

    // The refusal of the most recent Encode(). Meaningful only after Encode() returned false.
    const RefusedField& LastRefusal() const { return lastRefusal_; }

private:
    RefusedField lastRefusal_{};
    bool sampled_;
    bool seen_[kMaxBodies] = {};
    BodyFrame prev_[kMaxBodies] = {};
    uint64_t prevStamp_[kMaxBodies] = {};
    uint16_t lastEntity_ = 0;
};

class BodyDecoder {
public:
    explicit BodyDecoder(bool sampled) : sampled_(sampled) {}
    void Reset();
    bool Decode(bitio::BitReader&, DecodedBody&, uint8_t* bodyBuf, size_t bodyCap);

    // The refusal of the most recent Encode(). Meaningful only after Encode() returned false.
    const RefusedField& LastRefusal() const { return lastRefusal_; }

private:
    RefusedField lastRefusal_{};
    bool sampled_;
    bool seen_[kMaxBodies] = {};
    BodyFrame prev_[kMaxBodies] = {};
    uint64_t prevStamp_[kMaxBodies] = {};
    uint16_t lastEntity_ = 0;
};

class TickEncoder {
public:
    void Reset();
    bool Encode(bitio::BitWriter&, const TickRec&);

private:
    bool started_ = false;
    TickRec prev_ = {};
    // The previous record's own delta, so a clock is coded against a PREDICTION rather than against
    // its predecessor. Only the integer clock fields of this struct are used as deltas.
    TickRec delta_ = {};
};

class TickDecoder {
public:
    void Reset();
    bool Decode(bitio::BitReader&, TickRec&);

private:
    bool started_ = false;
    TickRec prev_ = {};
    TickRec delta_ = {};
};

// S_NETVAR. One record per DUV message, because that is the unit the engine numbers: storing loose rows
// would make `observed` and `stored` count different things and the completeness arithmetic could not
// close. Every row of every message is present; the message's own packet number is stored, so a reader
// can recompute the accounting without trusting ours.
struct NetvarMessage {
    uint64_t owner;
    // ★ The message's own time on THIS FILE'S source-time axis -- the same axis chapter spans, ticks
    // and sampled poses are on, so a window can place the row at the playhead rather than at the
    // chapter's beginning. It is NOT NetvarRec::arrivalUs: that field is filled from the engine
    // delegate's argument 5 (capture.cpp OnNetvarMessage), the uninterpreted qword W1-L found is -1
    // for a locally raised message and was never a clock. ChapterBuilder's dispatch axis time is,
    // and is what fills this.
    // Zero on a file recorded before the fifth netvar schema field existed; ask the manifest, never
    // the value (0 is a legal time).
    uint64_t stampUs;
    uint16_t slot;
    uint16_t gen;
    uint32_t pktSeq;
    bool     hasRowset;
    uint32_t rowsetSeq;
    uint16_t varCount;
    static constexpr uint16_t kMaxRows = 128;   // the biggest real rowset is 56
    NetvarRow rows[kMaxRows];
};

// One dictionary entry: a (name, slot) pair seen in this chapter and the last value stored for it.
// The dictionary is a CODING, not a retention policy -- every row of every message is written, and the
// index simply replaces a repeated 64-bit symbol with seven bits.
struct NetvarKnown {
    uint64_t name;
    uint16_t slot;
    uint64_t value;
};

class NetvarEncoder {
public:
    void Reset();
    bool Encode(bitio::BitWriter&, const NetvarMessage&);

private:
    bool started_ = false;
    uint64_t owner_ = 0;
    uint64_t prevStamp_ = 0;
    uint32_t prevSeq_ = 0;
    uint16_t prevSlot_ = 0;
    uint16_t dictCount_ = 0;
    NetvarKnown dict_[kNameDictMax] = {};
};

class NetvarDecoder {
public:
    // Whether the FILE this decoder is reading carries the fifth netvar schema field. A capture
    // written before it does not, and reading a stamp out of one desynchronises the whole block --
    // so there is NO default: every construction site states which kind of file it is reading, and
    // a caller that forgets does not compile instead of mis-decoding every old capture.
    //
    // The answer comes from the manifest's schema table (playback's NetvarStampsPresent), never
    // from the bits, and Reset (which clears per-stream coding state between blocks) leaves it
    // alone because it describes the file rather than the block.
    explicit NetvarDecoder(bool stampPresent) : stampPresent_(stampPresent) {}

    void Reset();
    bool Decode(bitio::BitReader&, NetvarMessage&);

private:
    bool started_ = false;
    const bool stampPresent_;
    uint64_t owner_ = 0;
    uint64_t prevStamp_ = 0;
    uint32_t prevSeq_ = 0;
    uint16_t prevSlot_ = 0;
    uint16_t dictCount_ = 0;
    NetvarKnown dict_[kNameDictMax] = {};
};

// S_EVENT. The body is stored verbatim as received (S5.4.5): five of these layouts are undecoded, and a
// file written today stays correct for every one of them.
struct EventMessage {
    uint64_t stampUs;
    uint64_t dst;
    uint16_t typeIndex;   // into the manifest's tap table
    uint16_t bodyBytes;
    const uint8_t* body;
};

class EventEncoder {
public:
    void Reset();
    bool Encode(bitio::BitWriter&, const EventMessage&);

private:
    bool started_ = false;
    uint64_t prevStamp_ = 0;
    uint64_t prevDst_ = 0;
};

class EventDecoder {
public:
    void Reset();
    // `body` is written into `bodyBuf`, which must hold at least `bodyCap` bytes.  A body the
    // CALLER's buffer cannot hold is still CONSUMED and then reported (BodiesTooSmall), because a
    // decoder that returns mid-record leaves the BitReader off a record boundary and every
    // remaining row in the chapter decodes as garbage -- AUD-W2-A-2 F3.  A body past
    // kMaxEventBodyBytes is not this format's and is refused without consuming anything: the stream
    // is already untrustworthy at that point and skipping a length we do not believe is guesswork.
    bool Decode(bitio::BitReader&, EventMessage&, uint8_t* bodyBuf, size_t bodyCap);

    uint32_t BodiesTooSmall() const { return tooSmall_; }
    uint32_t BodiesOverBound() const { return overBound_; }

private:
    bool started_ = false;
    uint64_t prevStamp_ = 0;
    uint64_t prevDst_ = 0;
    uint32_t tooSmall_ = 0;
    uint32_t overBound_ = 0;
    uint8_t  skip_[kMaxEventBodyBytes] = {};   // where a body too big for the caller is consumed to
};

// S_ASSEEN. Near-constant fields, so almost every record is a run of unchanged flags.
struct AsSeenFrame {
    uint64_t sourceTimeUs;
    uint32_t frameIndex;
    AsSeenSample sample;
    uint16_t playerCount;
    static constexpr uint16_t kMaxPlayers = 32;
    AsSeenPerPlayer players[kMaxPlayers];
};

class AsSeenEncoder {
public:
    void Reset();
    bool Encode(bitio::BitWriter&, const AsSeenFrame&);

private:
    bool started_ = false;
    AsSeenFrame prev_ = {};
    AsSeenFrame delta_ = {};
};

class AsSeenDecoder {
public:
    void Reset();
    bool Decode(bitio::BitReader&, AsSeenFrame&);

private:
    bool started_ = false;
    AsSeenFrame prev_ = {};
    AsSeenFrame delta_ = {};
};

// ---- the wire path ---------------------------------------------------------------------------
// W1-A pinned the record start inside each message body by re-encoding: an offset is accepted only if
// the whole record reproduces the source bits AND every remaining bit is zero padding
// (tests/o1/corpus/arena_idle.tsv). These are those offsets. They are constants of the MESSAGE layout,
// not of the file format, and a capture that disagrees with them stores verbatim rather than guessing.
constexpr size_t kPlayerRecordStartBits = 128;
constexpr size_t kBodyRecordStartBits = 192;

enum class WireDecode : uint8_t {
    Ok = 0,           // decoded and proved: re-encoding reproduces the body bit for bit
    TooShort,         // the body cannot hold a record at the pinned offset
    ReadFailed,       // the engine mirror refused mid-record
    ReencodeDiffers,  // read whole with zero padding, but re-encoding did NOT reproduce the source. Store
                      // verbatim; the decoded values are still the engine's own
    PaddingNonZero,   // the bits after the record are not zero, so the offset is not what we think
};
const char* WireDecodeName(WireDecode);

// Decodes an SR15NetPlayerStateMsg body and PROVES the decode by re-encoding it. Anything but Ok means
// the caller must store the body verbatim.
WireDecode DecodePlayerBody(const void* body, size_t bytes, PlayerFrame& out);
WireDecode DecodeBodyBody(const void* body, size_t bytes, BodyFrame& out);

// The verbatim fallback, and the S_KEY blob, are plain byte runs. They go through the same block
// framing so a stream never mixes two framings.
bool EncodeRawRecord(bitio::BitWriter&, const void* bytes, uint16_t n);
// `cap` is a size_t and is clamped internally to the 16-bit length this framing can express.
// It used to be a uint16_t, which meant a caller with a 64 KiB buffer wrote
// static_cast<uint16_t>(65536) == 0 and had every record refused -- an explicit cast, a
// plausible-looking call, and a total silent failure. The narrowing belongs here, where the
// limit is, not at each call site.
bool DecodeRawRecord(bitio::BitReader&, uint8_t* out, size_t cap, uint16_t& n);

}  // namespace capture
}  // namespace replay
