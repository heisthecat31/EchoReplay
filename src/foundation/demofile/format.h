#pragma once

#include <stddef.h>
#include <stdint.h>

#include "foundation/demofile/demotypes.h"

// The .echodemo byte layout of design S5.2, and nothing else. Every struct here is packed and
// size-asserted: the assert is what stops a field insertion from silently moving the rest of the file.
//
// S5.2 states a 48-byte chapter header, lists 44 bytes of fields before head_crc32, and then calls that
// run "the 40 bytes above" while S5.5 calls the whole thing 44 bytes. Those three numbers cannot all be
// right. The layout implemented here is the one the field list produces -- 44 bytes covered by a
// head_crc32 that brings the header to 48 -- and the discrepancy is reported to the architect rather
// than resolved silently in code.

namespace replay {

constexpr char kFileMagic[8] = {'E', 'C', 'H', 'O', 'D', 'E', 'M', 'O'};
constexpr char kFooterMagic[8] = {'E', 'C', 'H', 'O', 'F', 'T', 'R', '!'};
constexpr uint32_t kChapterMagic = 0x54504843u;  // 'CHPT' little-endian
constexpr uint32_t kRoundTableTag = 0x54444E52u;  // 'RNDT' little-endian

constexpr uint16_t kFormatMajor = 1;
constexpr uint16_t kFormatMinor = 0;

constexpr uint32_t kFileHeaderBytes = 128;
constexpr uint32_t kFooterBytes = 48;
constexpr uint32_t kChapterHeadBytes = 48;
constexpr uint32_t kChapterHeadCrcCover = 44;
constexpr uint32_t kStreamBlockBytes = 16;

// S5.2: a recovery scan must be bounded and a torn tail must cost at most one chapter.
constexpr uint32_t kMaxChapterPayload = 1u << 20;

// The trailer's whole index must fit one stream block, so the chapter count is bounded by
// (kMaxChapterPayload - kStreamBlockBytes) / sizeof(IndexEntryDisk) = 32767. At the S5.5 cadence -- one
// CORE chapter per 2 s plus the parallel ASSEEN, VOIP and ORACLE chapters -- that is about four and a
// half hours of continuous recording, well past S5.7's one file per match. Past the limit the writer
// stops appending and says so; it never writes a chapter the index cannot describe.
constexpr uint32_t kMaxChapters = (kMaxChapterPayload - kStreamBlockBytes) / 32;

enum FileFlag : uint64_t {
    kFlagOpen = 1ull << 0,
    kFlagHasVoip = 1ull << 1,
    kFlagHasAsSeen = 1ull << 2,
    kFlagHasOracle = 1ull << 3,
    kFlagImported = 1ull << 4,
    kFlagTruncatedByBudget = 1ull << 5,
};

enum ChapterCodec : uint16_t { kCodecRaw = 0, kCodecZstd = 1 };

#pragma pack(push, 1)

struct FileHeaderDisk {
    char magic[8];
    uint16_t formatMajor;
    uint16_t formatMinor;
    uint32_t headerCrc32;  // over [0,12) + [16,128)
    uint64_t flags;
    uint64_t trailerOffset;
    uint64_t firstChapterOffset;
    uint64_t t0Us;
    uint8_t demoUuid[16];
    uint8_t echovrMd5[16];
    uint64_t buildStringHash;
    uint64_t contentHash;
    uint32_t captureRole;
    uint32_t writerVersion;
    uint64_t authorityFlags;
    uint8_t reserved[16];
};

struct ChapterHeadDisk {
    uint32_t magic;
    uint16_t section;
    uint16_t codec;
    uint32_t payloadLen;
    uint32_t payloadCrc32;
    uint64_t tBeginUs;
    uint64_t tEndUs;
    uint32_t seq;
    uint32_t uncompressedLen;
    uint16_t streamCount;
    uint16_t reserved;
    uint32_t headCrc32;
};

struct StreamBlockDisk {
    uint16_t streamId;
    uint16_t schemaId;
    uint32_t byteLen;
    uint32_t observed;  // from the engine's own numbering, never the sampler's loop (S5.6.2)
    uint32_t stored;
};

struct IndexEntryDisk {
    uint64_t tBeginUs;
    uint64_t tEndUs;
    uint64_t offset;
    uint32_t len;  // payload bytes; the frame occupies kChapterHeadBytes + len
    uint16_t section;
    uint16_t seq;
};

struct FileFooterDisk {
    char magic[8];
    uint64_t trailerOffset;
    uint64_t tEndUs;
    uint64_t totalBytes;
    uint32_t chapterCount;
    uint32_t flags;
    uint32_t reserved;
    uint32_t footerCrc32;  // over the preceding 44 bytes
};

// S5.7's S_MARK record. The writer parses these to mirror the round table into the trailer manifest,
// so the layout is shared rather than reimplemented at the call site.
struct MarkRecordDisk {
    uint64_t tUs;
    uint16_t kind;
    uint16_t index;
    uint64_t payload;
};

// ARCH-21's S_GAMESTATE record: one on-change sample of the local game state, field-for-field the disk
// form of gamestate::GameStateSample (W1-D's src/engine/gamestate_types.h). The producer copies field by
// field; nothing here is a reinterpret of that struct, so its padding and ours are independent.
//
// `roundCounter` is 32 bits ON DISK, the full width the engine holds. Only the marker index and the
// round-table row number are 16 bits, and RoundNumberFromCounter is the one place that narrowing
// happens (ARCH-21 C4). A sample whose read did not answer sets no bit for that field in `present`; it
// is never defaulted to zero.
struct GameStateRecordDisk {
    uint64_t engineTimeUs;
    uint64_t captureTimeUs;
    uint64_t phaseSymbol;   // raw CSymbol64, stored even when phase == Unknown
    uint64_t pausedSymbol;  // raw CSymbol64; the format claims no meaning for its values
    uint32_t roundCounter;
    uint32_t bluePoints, redPoints;            // depth-2 ROUND points
    uint32_t blueMatchScore, redMatchScore;    // depth-0 MATCH score
    uint32_t frisbeeOwnerId;                   // 0xFFFFFFFF is the engine's own "unowned"
    float gameTimer;                           // `game_timer`, NOT the /session key `game_clock`
    float countdownTimer;
    uint32_t firstLaunchOfRound;
    int32_t roundWinningTeam;
    uint16_t lastScoreTeam, lastScorePoints;
    uint32_t present;  // GameStateField bits; a clear bit means the field did not answer
    uint8_t phase;         // gamestate::Phase, numbered in demotypes.h
    uint8_t availability;  // gamestate::Availability, numbered in demotypes.h
    uint8_t reserved[2];
};

// The MANIFEST round table, appended after the manifest's blob list as a tagged, length-framed block.
// A reader that reaches the end of the payload without seeing the tag reports MarkerSource::NotRecorded
// -- which is exactly what every file written before ARCH-21 does, and it still verifies.
struct RoundTableHeadDisk {
    uint16_t rowCount;
    uint16_t rowStride;    // sizeof(RoundRowDisk); a longer stride from a later minor is skipped per row
    uint8_t markerSource;  // MarkerSource, never NotRecorded: the block's presence is the answer
    uint8_t reserved[3];
};


// The S_PLAYER_STATS record: one player's live match block at one instant. Field-for-field
// PlayerStatsRecord (demotypes.h). Written on change and copied at every keyframe, so a reader that
// starts mid-file has a complete scoreboard from the first keyframe it decodes.
constexpr uint32_t kPlayerStatsIdBytes = 24;     // xplatformId and the two clocks
constexpr uint32_t kPlayerStatsValueCount = 14;  // the twelve stats, ping, packetlossratio
constexpr uint32_t kPlayerStatsTailBytes = 8;    // fill, userIndex, the two flags

struct PlayerStatsRecordDisk {
    uint64_t xplatformId;
    uint64_t engineTimeUs;
    uint64_t captureTimeUs;
    float possessionTime;
    int32_t points, goals, stuns, blocks, assists, saves;
    int32_t interceptions, steals, catches, passes, shotsTaken;
    int32_t ping;
    float packetLossRatio;
    uint32_t fill;
    uint16_t userIndex;
    uint8_t isEmotePlaying;
    uint8_t keyframe;
};

// The per-user clock the monotone rule needs, carried across blocks and chapters by whoever walks the
// stream. The writer keeps one for the file it is writing and `verify` keeps one for the file it is
// reading, so the rule has exactly one implementation.
struct PlayerStatsCursor {
    uint64_t id[kPlayerStatsMaxUsers];
    uint64_t lastEngineUs[kPlayerStatsMaxUsers];
    uint16_t n;
};

// False when this record's engine time is behind the last one seen for its user, or when the cursor
// has no room for another user. Both make the stream unsearchable, so both refuse.
bool PlayerStatsNoteTime(PlayerStatsCursor& cursor, uint64_t xplatformId, uint64_t engineTimeUs);

// Copies field by field and then applies the fill rule: every value whose bit is clear is written as
// its absence value. A producer therefore cannot publish a number it never claimed.
void PlayerStatsToDisk(const PlayerStatsRecord& r, PlayerStatsRecordDisk& out);
void PlayerStatsFromDisk(const PlayerStatsRecordDisk& d, PlayerStatsRecord& out);

// nullptr when every record in the block is one this format can have written; otherwise the refusal,
// named. Checks the block width, the fill mask's vocabulary, fill-vs-value in both directions, the
// two boolean flags, that the record names a player at all, and the per-user monotone time.
const char* CheckPlayerStatsBlock(const void* data, uint32_t bytes, PlayerStatsCursor& cursor);

struct RoundRowDisk {
    uint16_t number;
    uint16_t flags;
    uint8_t phaseAtBegin, phaseAtEnd;
    uint16_t bluePointsAtEnd, redPointsAtEnd;
    uint16_t blueMatchScoreAtEnd, redMatchScoreAtEnd;
    uint16_t reserved;
    uint64_t beginUs, endUs;
    uint64_t beginEngineUs, endEngineUs;
};

// One lost run, named with its range. GAP and DROPPED share the layout because they differ in cause,
// not in shape: GAP is upstream of us, DROPPED is downstream (E-C10).
struct GapRecordDisk {
    uint64_t tUs;
    uint16_t streamId;
    uint16_t reason;
    uint32_t firstMissing;  // engine-side index of the first missing record, or 0 when unknown
    uint32_t count;
};

struct DegradedRecordDisk {
    uint64_t tUs;
    uint16_t what;    // a Stream id that was shed, or 0 for the keyframe interval
    uint16_t reason;
    uint32_t detail;
};

#pragma pack(pop)

static_assert(sizeof(FileHeaderDisk) == kFileHeaderBytes, "FileHeader is 128 bytes, fixed forever");
static_assert(sizeof(ChapterHeadDisk) == kChapterHeadBytes, "ChapterFrame header is 48 bytes");
static_assert(offsetof(ChapterHeadDisk, headCrc32) == kChapterHeadCrcCover, "head_crc32 covers 44 bytes");
static_assert(sizeof(StreamBlockDisk) == kStreamBlockBytes, "StreamBlockHeader is 16 bytes");
static_assert(sizeof(IndexEntryDisk) == 32, "index entry is 32 bytes");
static_assert(kMaxChapters * sizeof(IndexEntryDisk) + kStreamBlockBytes <= kMaxChapterPayload,
              "the whole index must fit one stream block, or the trailer splits and the fast path degrades");
static_assert(kMaxChapters <= 0xFFFF, "ChapterKey::seq is uint16_t (Appendix B.2)");
static_assert(sizeof(FileFooterDisk) == kFooterBytes, "FileFooter is 48 bytes");
static_assert(offsetof(FileFooterDisk, footerCrc32) == 44, "footer_crc32 covers 44 bytes");
static_assert(sizeof(MarkRecordDisk) == 20, "S_MARK record is 20 bytes");
static_assert(sizeof(MarkRecordDisk::index) == 2, "S_MARK index is 16 bits; ARCH-21 D4 forbids widening it");
static_assert(sizeof(GameStateRecordDisk) == 84, "S_GAMESTATE record is 84 bytes");
static_assert(sizeof(GameStateRecordDisk::roundCounter) == 4,
              "round_counter is stored at its full engine width; narrowing it here would truncate "
              "silently where ARCH-21 C4 requires a refusal");
static_assert(offsetof(GameStateRecordDisk, roundCounter) == 32, "round_counter at +32");
static_assert(offsetof(GameStateRecordDisk, present) == 76, "present at +76");
static_assert(offsetof(GameStateRecordDisk, phase) == 80, "phase at +80");
static_assert(sizeof(RoundTableHeadDisk) == 8, "round table head is 8 bytes");
static_assert(sizeof(RoundRowDisk) == 48, "round table row is 48 bytes");
static_assert(sizeof(PlayerStatsRecordDisk) ==
                  kPlayerStatsIdBytes + kPlayerStatsValueCount * 4 + kPlayerStatsTailBytes,
              "S_PLAYER_STATS is an id block, fourteen 4-byte values and an 8-byte tail");
static_assert(offsetof(PlayerStatsRecordDisk, possessionTime) == kPlayerStatsIdBytes,
              "the values start where the id block ends");
static_assert(offsetof(PlayerStatsRecordDisk, fill) ==
                  kPlayerStatsIdBytes + kPlayerStatsValueCount * 4,
              "the fill mask starts where the values end; a value inserted before it would silently "
              "renumber every stat a stored mask claims");
static_assert(sizeof(PlayerStatsRecordDisk::ping) == 4 && sizeof(PlayerStatsRecordDisk::points) == 4,
              "the engine's integer setter is what these mirror");
static_assert(kPlayerStatsFillAll == (1u << 15) - 1u, "fifteen values, fifteen bits");
static_assert(sizeof(RoundRow::number) == 2 && sizeof(RoundRowDisk::number) == 2,
              "a round row number is 16 bits in memory and on disk, and out-of-range refuses");
static_assert(uint32_t(kRoundNumberNone) == kMaxRoundNumber + 1,
              "the reserved no-number value sits immediately above the representable range");
static_assert(sizeof(GapRecordDisk) == 20, "GAP record is 20 bytes");
static_assert(sizeof(DegradedRecordDisk) == 16, "DEGRADED record is 16 bytes");
static_assert(offsetof(FileHeaderDisk, flags) == 16, "flags at +16");
static_assert(offsetof(FileHeaderDisk, t0Us) == 40, "t0_us at +40");
static_assert(offsetof(FileHeaderDisk, echovrMd5) == 64, "echovr_md5 at +64");
static_assert(offsetof(FileHeaderDisk, authorityFlags) == 104, "authority_flags at +104");

// ARCH-21 C4/M5: the ONE place a 32-bit engine `round_counter` becomes a 16-bit round number. A counter
// outside [0, kMaxRoundNumber] is REFUSED -- `out` is set to kRoundNumberNone and the call returns false
// -- so the caller records a numberless, time-seekable row instead of claiming round 1 for round 65537.
// Never wraps, never masks. A negative engine value arrives here as a huge unsigned and is refused too.
bool RoundNumberFromCounter(uint32_t counter, uint16_t& out);

uint32_t Crc32(const void* data, size_t bytes, uint32_t seed = 0);
uint32_t HeaderCrc(const FileHeaderDisk& h);

// Reasons a gap or dropped run was recorded.
enum GapReason : uint16_t {
    kGapEngineSequence = 1,  // the engine numbered more than we stored
    kGapRingFull = 2,        // E-C10
    kGapBlockTooLarge = 3,   // a single stream block would not fit one chapter
};

}  // namespace replay
