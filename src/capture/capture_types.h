#pragma once

// The shapes that cross the capture ring, and the stream schema ids the file declares.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THE TICK DOES NOT ENCODE, WHICH IS A DELIBERATE DEPARTURE FROM S6.1'S DIAGRAM.
//
// S6.1 draws gather -> encode -> push on the tick and chapterise -> zstd -> write on the worker. This
// implementation moves `encode` to the worker as well: the tick gathers into a POD, memcpys it into the
// ring, and nothing else. Three reasons, and the first is the one that matters:
//
//  1. LOSSLESSNESS. A wire record reaches us as the engine's own bit-packed body. Storing it means
//     decoding 284 or 370 bits, delta-coding, and re-packing -- and the ONLY way to know that survived
//     bit-exactly is to re-encode the decoded values and compare against the source bytes. That check
//     needs the source bytes, so either the tick carries them anyway (and the encode buys nothing) or
//     the check cannot be made. Deferring the decode keeps the original bytes alive until a verified
//     re-encode has proved the decoded form is a faithful substitute -- see streamcodec.h.
//  2. COST. S6.4 budgets 20 us median on the tick. A memcpy of a 64-byte body is ~50 ns; a decode plus
//     a re-encode plus a verify is not. Moving it off strictly improves the budget it is measured
//     against, and the ring throughput it costs (33 KB/s at 8 players) is inside S6.4's 70 KB/s.
//  3. ONE OWNER FOR DELTA STATE. Delta coding needs the previous record per entity per stream. On the
//     worker that state has exactly one reader and one writer. Split across two threads it would need
//     synchronisation the tick is forbidden to take.
//
// The departure is reported rather than assumed: it is in W2-A's handoff, and S11.2's "encode 90-120 Hz"
// row is the sentence it contradicts.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "engine/asseen.h"
#include "engine/entity.h"
#include "engine/gamestate_types.h"
#include "foundation/demofile/demotypes.h"

namespace replay {
namespace capture {

// The width of a per-BodyRefusal histogram. Taken from the LAST enumerator rather than written as
// a number, so engine/entity.h stays the one place the arm list lives. The name has to be the arm
// that is actually last: naming TagMismatch left this four short when W1-E appended the ncaPhysics
// search arms, and gather.h bounds the increment, so every refusal in that family landed in
// bodyRefusalUnknown and the stop line could not name any of them. The case in tests/capture that
// asserts index kBodyRefusalCount is uncased -- BodyRefusalName answers "unknown" there, and only
// there -- is what caught it, and is the control for the next append.
constexpr unsigned kBodyRefusalCount = unsigned(BodyRefusal::NoPhysicsCsForEntity) + 1u;

// The same shape for the angular-velocity arms, and the same trap: the name has to be the
// enumerator that is actually last, and gather.h bounds the increment, so a source appended
// past Unavailable without moving this lands every reading in angVelUnknown. The case in
// tests/capture that asks AngVelSourceName for index kAngVelSourceCount -- "unknown" there and
// only there -- is the control for the next append.
constexpr unsigned kAngVelSourceCount = unsigned(AngVelSource::Unavailable) + 1u;

// THE STREAMS THIS CAPTURE CAN PRODUCE, in one place, because two lists that must agree and are
// written down twice have now disagreed twice: S_GAMESTATE (L2-RUN-5 F3) and S_PLAYER_STATS
// (L2-RUN-7 F2) were each added to the format and to the writer and left out of capture.cpp's
// config table, and a stream with no config row is left disabled by a zero-initialised array with
// nothing anywhere saying so. capture.cpp static_asserts its table against this list, and
// chapterbuilder.cpp static_asserts its per-stream array bound against the maximum -- so the next
// stream added to the writer and not to the table is a BUILD failure and not a silent absence.
constexpr uint16_t kProducibleStreams[] = {
    kStreamTick,     kStreamPlayerWire, kStreamPlayerSampled, kStreamBodyWire,
    kStreamBodySampled, kStreamNetvar,  kStreamEvent,         kStreamKey,
    kStreamMark,     kStreamAsSeen,     kStreamGameState,     kStreamPlayerStats,
    kStreamVoip,     kStreamOracle,
};
constexpr uint16_t kProducibleStreamCount =
    static_cast<uint16_t>(sizeof kProducibleStreams / sizeof kProducibleStreams[0]);

constexpr uint16_t MaxProducibleStreamId() {
    uint16_t m = 0;
    for (uint16_t id : kProducibleStreams) {
        if (id > m) m = id;
    }
    return m;
}

// The highest id the FORMAT defines (demotypes.h's enum), not the highest one this packet happens
// to produce: the accounting streams the container writes for us (GAP/DROPPED/DEGRADED) are indexed
// out of the same arrays. It is stated once here and asserted where it is used.
constexpr uint16_t kFormatMaxStreamId = kStreamPlayerStats;
static_assert(MaxProducibleStreamId() <= kFormatMaxStreamId,
              "a producible stream sits above the format's highest id");
static_assert(kFormatMaxStreamId >= kStreamDegraded,
              "the accounting streams are indexed out of the same arrays");

// Schema ids as the manifest declares them. The container numbers STREAMS (demotypes.h); this numbers
// the field layouts, and the two are not the same thing: S_PLAYER_WIRE and S_PLAYER_SAMPLED carry the
// identical field list under different storage classes, which is S5.3.1's whole point.
enum Schema : SchemaId {
    kSchemaTick = 1,
    kSchemaPlayerWire = 2,
    kSchemaPlayerSampled = 3,
    kSchemaBodyWire = 4,
    kSchemaBodySampled = 5,
    kSchemaNetvar = 6,
    kSchemaEvent = 7,
    // RETIRED with S_WORLD. The constant stays so nothing reuses 8: DESIGN S5.4.0's INV-STREAM-ID
    // holds for the schema table too -- MANIFEST is indexed by this number, so handing 8 to a new
    // schema would re-point every already-recorded file's world blocks at whatever inherited it. No
    // schema entry is emitted for it any more; the id is simply absent from the table a file carries.
    kSchemaWorld_RETIRED = 8,
    kSchemaKey = 9,
    kSchemaMark = 10,
    kSchemaAsSeen = 11,
    kSchemaVoip = 12,
    kSchemaOracle = 13,
    kSchemaGap = 14,      // shared by S_GAP and S_DROPPED: same 20-byte record, different cause
    kSchemaDegraded = 15,
    kSchemaManifest = 16,
    kSchemaIndex = 17,
    kSchemaCount = 18,
};

// ---- ring records ---------------------------------------------------------------------------
// One producer writes these; the worker reads them. Everything here is trivially copyable and
// self-contained: a record must not point at engine memory, because the worker reads it a chapter
// later and the engine will have moved on.

enum RecKind : uint16_t {
    kRecTick = 1,
    kRecWirePlayer = 2,     // SR15NetPlayerStateMsg body, verbatim
    kRecWireBody = 3,       // SR15NetPhysicsSimStateEvent body, verbatim
    kRecSampledPlayer = 4,
    kRecSampledBody = 5,
    kRecNetvar = 6,         // SR15NetReplicatedDeltaUnreliableVars body, verbatim
    kRecEvent = 7,          // any other tapped message body, verbatim
    kRecAsSeen = 8,
    kRecMark = 9,
    // RETIRED with S_WORLD. Ring-internal rather than format, but reserved for the same reason the
    // stream and schema ids are: a stale producer somewhere pushing kind 10 must not be read as
    // whatever kind 10 becomes next.
    kRecWorld_RETIRED = 10,
    kRecOracle = 11,
    kRecDegraded = 12,
    kRecSessionOpen = 13,
    kRecSessionClose = 14,
    kRecVoip = 15,
    // ★ Capture-internal transport only. It carries the world re-origin shift from the tick thread,
    // which is the only thread that may read engine memory, to the worker that owns the file. It
    // becomes a KEYFRAME BLOCK, not a stream block, so it needs no schema and no stream id.
    // A3. What the ENGINE HELD this frame, as opposed to what we managed to record. Only the tick
    // enumerates the component pool, so only the tick can say an entity existed; without this the
    // accounting cannot tell a sampler that silently skipped an entity -- ReadBody returning false is
    // a real path -- from an entity that legitimately was not there. The audit got bit-identical
    // output for those two. This record is consumed for ACCOUNTING ONLY and never stored.
    kRecFrameSet = 16,
    // ★ Capture-internal transport only. It carries the world re-origin shift from the tick thread,
    // which is the only thread that may read engine memory, to the worker that owns the file. It
    // becomes a KEYFRAME BLOCK, not a stream block, so it needs no schema and no stream id.
    kRecWorldOrigin = 17,
    // * Capture-internal transport, ARCH-21 S2. W1-D's local game-state sample as the TICK read it,
    // carried to the worker that owns the file: the tick is the only thread that may touch engine
    // memory, and the sample is a POD by construction so the worker can read it a chapter later.
    kRecGameState = 18,
    // * Capture-internal transport, ARCH-21 MAJOR-B2. One entrant's live scoreboard as the TICK read
    // it, carried to the worker that owns the file. Same rule as kRecGameState: only the tick may
    // touch engine memory, and the payload is a POD so the worker can read it a chapter later. It
    // becomes S_PLAYER_STATS through DemoWriter::WritePlayerStats, which is where the format's
    // absence sentinels and its per-user monotone rule live -- there is no second copy of either.
    kRecPlayerStats = 19,
};

// The highest record kind, and the bound every per-kind table and loop must use. Taking it from
// the last enumerator is what stops a kind appended past a hand-written bound from being counted
// nowhere: kRecGameState and kRecPlayerStats sat outside `kRecWorldOrigin` and a ring drop of
// either was charged to no stream at all.
constexpr uint16_t kRecKindMax = kRecPlayerStats;

// The live scoreboard's refusals, counted BY REASON with the engine's own word for each -- the same
// shape RosterRefusals has, and for the same reason: "12 entrants refused" is a shrug, "12 refused
// (StatsUnreadable)" is a repair.
//
// A SPARSE table rather than one counter per enumerator. identity_access::Refusal has thirty-odd
// values and this sampler can produce about eight of them; a document listing thirty reasons at zero
// reads as a report on thirty things that went wrong. Reasons are appended as they first fire, and a
// thirteenth distinct reason is COUNTED in `overflow` rather than dropped, because "we stopped
// recording reasons" and "no further reasons fired" are different facts.
//
// The reason code is a bare uint8 and the word is a pointer the SOURCE hands back, so this header
// stays free of the engine: tests/capture drives it with its own words, and the sentinel writer
// prints them without linking the identity module.
constexpr uint8_t kStatsReasonCap = 12;

struct StatsRefusals {
    uint32_t count[kStatsReasonCap];
    uint8_t  reason[kStatsReasonCap];
    const char* word[kStatsReasonCap];
    uint8_t  used;
    uint32_t overflow;

    uint32_t Total() const {
        uint32_t n = overflow;
        for (uint8_t i = 0; i < used; ++i) n += count[i];
        return n;
    }

    void Note(uint8_t reasonCode, const char* why) {
        for (uint8_t i = 0; i < used; ++i) {
            if (reason[i] == reasonCode) {
                ++count[i];
                if (why) word[i] = why;
                return;
            }
        }
        if (used >= kStatsReasonCap) {
            ++overflow;
            return;
        }
        reason[used] = reasonCode;
        word[used] = why;
        count[used] = 1;
        ++used;
    }

    void Merge(const StatsRefusals& o) {
        for (uint8_t i = 0; i < o.used; ++i) {
            uint8_t j = 0;
            for (; j < used; ++j) {
                if (reason[j] == o.reason[i]) break;
            }
            if (j < used) {
                count[j] += o.count[i];
                if (o.word[i]) word[j] = o.word[i];
            } else if (used < kStatsReasonCap) {
                reason[used] = o.reason[i];
                word[used] = o.word[i];
                count[used] = o.count[i];
                ++used;
            } else {
                overflow += o.count[i];
            }
        }
        overflow += o.overflow;
    }
};

#pragma pack(push, 1)

// Every ring record starts with this. `bytes` counts the payload after the header.
struct RecHeader {
    uint16_t kind;
    uint16_t bytes;
    uint32_t producer;  // the ring index that carried it; the worker uses it for DROPPED attribution
};

// S_TICK. S5.4.2 plus the two clocks S5.4.3 keeps apart, so nothing has to be reconstructed later.
struct TickRec {
    uint64_t sourceTimeUs;   // the replay time axis: peer time on a client, sim time on a host
    uint64_t simTimeUs;      // CClock+0x28 at our tick
    uint64_t peerTimeUs;     // NETGAME.PEERTIME, 0 when not sourced (asSeenValid says which)
    uint32_t frameIndex;     // CncaGame+0x830 -- the engine numbering S5.6.2 requires
    uint32_t scaledDeltaUs;  // CClock+0x68, beat 1 only. Recorded, never integrated (B.3.1)
    uint32_t frameDeltaUs;   // simTimeUs advance since our previous tick == exactly one frame
    uint32_t asSeenValid;    // AsSeenSample::valid, so a zero field is never read as a measurement
};

// A tapped wire body, carried verbatim. `stampUs` is the sender's stamp from the message's own
// [8:16), not our arrival time: S6.3 forbids re-stamping a wire-sourced record.
// ARCH-21 S2's S_GAMESTATE sample, plus the file's own time axis. The sample carries the engine
// clock it was read against; `sourceTimeUs` is where it lands in the replay, which is the tick's
// axis and not the sampler's business.
struct GameStateRec {
    uint64_t sourceTimeUs;
    gamestate::GameStateSample sample;
};


struct WireRec {
    uint64_t stampUs;
    uint64_t dst;         // the dispatch's destination id, which is how an entity is told apart
    uint64_t arrivalUs;   // our clock when it arrived; diagnostics only, never the time axis
    uint32_t seq;         // the producer's own delivery counter for this stream
    uint16_t bodyBytes;
    uint16_t pad;
    // uint8_t body[bodyBytes] follows
};

struct SampledPlayerRec {
    uint64_t sourceTimeUs;
    uint32_t frameIndex;
    // ★ THE USER SLOT, for local and remote alike. The broadcaster CI's user-id low word is the same
    // u16 IEntity::PlayerOf takes, so one key serves both pools and a local row and a remote row for
    // the same player land in one index space instead of two that have to be reconciled later.
    uint16_t entity;
    uint16_t pad;
    // Which fields of `state` were sourced. A clear bit means the field is zero because nothing
    // supplied it -- see PlayerFill in streamcodec.h.
    uint32_t fill;
    PlayerState state;
};

struct SampledBodyRec {
    uint64_t sourceTimeUs;
    uint32_t frameIndex;
    // ★ THE KEY IS A TAGGED PAIR, not the handle and not a bare net id. CR15NetPhysicsCS::SimStateCB resolves a body through
    // FindNetActor(u64 netId) before it reaches a component, so the net id is what the wire carries
    // and what a later session can resolve. `handle` is the local PhysCI index and is kept as a
    // DIAGNOSTIC only -- it is a per-session pool position and means nothing on playback.
    uint64_t idValue;
    uint32_t idTag;
    uint32_t fill;        // BodyFill; kFillBodyNetId clear means netId was not sourced
    uint16_t handle;      // PhysHandle, local to this session
    uint16_t pad;
    BodyState state;
};

struct NetvarRec {
    uint64_t arrivalUs;
    uint32_t seq;         // pkt_seq lifted from the body; the engine's own numbering
    uint16_t bodyBytes;
    uint16_t pad;
    // uint8_t body[bodyBytes] follows -- the DUV message verbatim
};

struct EventRec {
    uint64_t stampUs;
    uint64_t dst;
    uint16_t typeIndex;   // index into the manifest's tap table; never a raw symbol per record
    uint16_t bodyBytes;
    uint32_t seq;
    // uint8_t body[bodyBytes] follows
};

struct AsSeenRec {
    uint64_t sourceTimeUs;
    uint32_t frameIndex;
    uint16_t playerCount;
    uint16_t pad;
    AsSeenSample sample;
    // AsSeenPerPlayer[playerCount] follows
};

struct MarkRec {
    uint64_t tUs;
    uint16_t kind;
    uint16_t index;
    uint64_t payload;
};

// S_WORLD's record is GONE, not commented out: DESIGN S5.4.5 retires the stream because it was carved
// out by SUBJECT in a format carved out by MECHANISM (S5.4.0 INV-STREAM-MECH), so it could only ever be
// redundant or empty. The 22 networked slots land in S_BODY_*, S_NETVAR and the MANIFEST blobs; the one
// genuinely world-global item, the re-origin offset, is chapter state in S_KEY's KB_WORLD_ORIGIN.


struct OracleRec {
    uint64_t sourceTimeUs;
    uint32_t frameIndex;
    uint8_t  status;       // TapStatus, so a NotBound sample is visible rather than absent
    uint8_t  pad;
    uint16_t sessionBytes;
    uint16_t bonesBytes;
    uint16_t pad2;
    // uint8_t session[sessionBytes]; uint8_t bones[bonesBytes]
};

struct DegradedRec {
    uint64_t tUs;
    uint16_t what;
    uint16_t reason;
    uint32_t detail;
};

// The entities a stream held on one frame. `count` u16 ids follow.
struct FrameSetRec {
    uint64_t tUs;
    uint32_t frameIndex;
    uint16_t stream;
    uint16_t count;
};

// The cumulative world offset as of one tick. See capture/worldorigin.h for why this address and
// not the one W2-B R5 named.
struct WorldOriginRec {
    uint64_t sourceTimeUs;
    float x, y, z;
    uint8_t valid;      // the pointer chain resolved; 0 means "not sourced", never "(0,0,0)"
    uint8_t suspect;    // the read may have been torn, or a re-origin was mid-apply
    uint16_t pad;
};

struct VoipRec {
    uint64_t tUs;
    uint32_t speakerSlot;
    uint16_t frameCtr;
    uint16_t bodyBytes;
    // uint8_t payload[bodyBytes]
};

#pragma pack(pop)

// Was a reserved stand-in id from when the local pawn could not be named at all. W1-E has since found
// that it DOES have a component instance -- in the broadcaster pool rather than the remote pool -- and
// that its user id indexes the same slot table as every remote. So local rows are keyed by user slot
// like everything else and nothing needs a reserved id. Kept only as an out-of-range marker for a row
// whose slot could not be established, which must never be confused with slot 0.
constexpr uint16_t kUnknownEntity = 0xFFFEu;

// The ceiling on tracked entities, shared by the codec's delta state and the completeness accounting so
// the two index the same slots. E-C4 is explicit that the stream count is DYNAMIC and never a fixed 8 --
// this is only the size of the arrays, and an entity id beyond it folds rather than being refused.
constexpr uint16_t kMaxEntities = 64;
constexpr uint16_t kMaxBodies = 64;

// Reasons this packet writes into DegradedRecordDisk::reason. The container leaves the field opaque.
enum DegradeReason : uint16_t {
    kDegradeOracleCadence = 1,   // S6.4 rung 1: the oracle's rate was reduced, never to zero
    kDegradeKeyframeInterval = 2,
    kDegradeKillSwitch = 3,
    kDegradeCompletenessUnverified = 4,  // a stream whose `observed` has no engine source in this role
    kDegradeRingClaimExhausted = 5,      // more producer threads than the ring set holds
    kDegradePreOpenIdentityDropped = 6,  // identity traffic seen BEFORE the file existed and shed
    kDegradeStreamNeverProduced = 7,     // an ENABLED stream that observed nothing all session
    // RECORDER-5 F1. Records the tap held BEFORE the file existed and that the open then wrote at
    // the file origin. They carry their own arrival stamps, which are on the pre-open axis and are
    // older than every other record in the file, so a reader that takes those stamps as file times
    // would place them outside the footage. `count` is how many were filed this way.
    kDegradePreOpenIdentityPlaced = 8,
};

// Reasons this packet writes into GapRecordDisk::reason beyond the container's own three.
enum CaptureGapReason : uint16_t {
    kGapWireSeqJump = 16,     // the DUV / tap sequence skipped
    kGapFrameSkip = 17,       // CncaGame+0x830 advanced by more than one between our ticks
    kGapChapterOverflow = 18, // a chapter could not hold the block and records were shed
};

// The file's own identity (format.h `demoUuid`). L2-RUN-2 F7: the field was written as sixteen zero
// bytes on every capture ever taken, so a demo could not be tied to the sentinel that describes its
// close, and every pre-existing demo in the owner's folder is indistinguishable from every other.
//
// Mixed rather than drawn from a UUID service: the capture runs inside the game with no network and
// no new dependency, and the property required is uniqueness across the captures ONE MACHINE takes,
// not global registrability. The mixer is split from its entropy so the shape is testable without
// the clock: the caller supplies the words, this decides what a demo uuid looks like.
struct DemoUuid {
    uint64_t lo, hi;
};

inline uint64_t DemoUuidMix64(uint64_t x) {
    // SplitMix64's finaliser. Chosen because it is a bijection: two opens that differ in one bit of
    // one input cannot collide, which a truncating hash could.
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline DemoUuid DemoUuidFrom(uint64_t a, uint64_t b, uint64_t c) {
    DemoUuid id{};
    id.lo = DemoUuidMix64(a ^ DemoUuidMix64(c));
    id.hi = DemoUuidMix64(b + 0x9E3779B97F4A7C15ull * c);
    // RFC 4122 version 4 and variant 1, in the byte positions writer.cpp lays lo then hi into. Not
    // decoration: a reader that meets a 128-bit field needs the two bytes that say how to read it,
    // and an all-zero field is the one value that says nothing at all.
    id.lo = (id.lo & ~(0xF0ull << 48)) | (0x40ull << 48);   // byte 6 high nibble = 4
    id.hi = (id.hi & ~(0xC0ull)) | 0x80ull;                 // byte 8 top two bits = 10
    return id;
}

// A demo uuid a capture actually wrote. All-zero is the ABSENCE value F7 recorded, and no mixed id
// can be zero (the version nibble is set unconditionally), so the two are distinguishable forever.
inline bool DemoUuidPresent(const DemoUuid& id) { return id.lo != 0 || id.hi != 0; }

}  // namespace capture
}  // namespace replay
