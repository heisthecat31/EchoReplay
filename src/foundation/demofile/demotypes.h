#pragma once

#include <stdint.h>

// Appendix B.0's shared types plus the .echodemo section and stream identifiers.
//
// FieldClass and the field descriptor belong to Appendix B.1, which W1-A owns. B.0 calls the descriptor
// FieldDef and B.1 calls it FieldSpec; they are one struct, so this aliases rather than declares a
// second one -- a schema field read out of a manifest is handed straight to the codec with no
// conversion, and there is no second definition to drift. The local declaration below exists only so
// the container still builds if bitio is not in the tree.
#if defined(__has_include)
#if __has_include("foundation/bitio/bitio.h")
#include "foundation/bitio/bitio.h"
#define REPLAY_HAVE_BITIO 1
#endif
#endif

namespace replay {

using StreamId = uint16_t;
using SectionId = uint16_t;
using SchemaId = uint16_t;
using Symbol64 = uint64_t;

#ifdef REPLAY_HAVE_BITIO
using FieldClass = bitio::FieldClass;
using FieldProv = bitio::FieldProv;
using FieldDef = bitio::FieldSpec;
#else
enum class FieldClass : uint8_t { WireQ = 0, F32 = 1, Raw = 2, WireQuat = 3 };
enum class FieldProv : uint8_t { Unknown = 0, Pc = 1, Arm64 = 2 };
struct FieldDef {
    uint16_t id;
    FieldClass cls;
    float min, max, step;
    uint8_t rawBits;
    FieldProv prov;
};
#endif

// The on-disk numbering. If the codec's enum ever reorders, this fails the build rather than silently
// changing what a stored schema means.
static_assert(static_cast<uint8_t>(FieldClass::WireQ) == 0, "FieldClass::WireQ is stored as 0");
static_assert(static_cast<uint8_t>(FieldClass::F32) == 1, "FieldClass::F32 is stored as 1");
static_assert(static_cast<uint8_t>(FieldClass::Raw) == 2, "FieldClass::Raw is stored as 2");
static_assert(static_cast<uint8_t>(FieldClass::WireQuat) == 3, "FieldClass::WireQuat is stored as 3");
static_assert(static_cast<uint8_t>(FieldProv::Unknown) == 0, "FieldProv::Unknown is stored as 0");
static_assert(static_cast<uint8_t>(FieldProv::Arm64) == 2, "FieldProv::Arm64 is stored as 2");
constexpr uint8_t kMaxFieldClass = static_cast<uint8_t>(FieldClass::WireQuat);
constexpr uint8_t kMaxFieldProv = static_cast<uint8_t>(FieldProv::Arm64);

// S5.6.2 rule 1 requires that a sample-dropping transform not be expressible. Edge was one: it licensed
// storing fewer records than the engine numbered, and the format carries no per-record engine index and
// no run boundary, so no reader can tell an unchanged run from a dropped one. DemoWriter::Open now
// refuses any schema that is not All, and echodemo verify names a file that carries one. The value stays
// in the enum only so a file written before that could still be read and reported.
enum class Retention : uint8_t { All = 0, Edge = 1 };

struct SchemaDef {
    SchemaId id;
    Retention retention;
    bool required;
    uint16_t fieldCount;
    const FieldDef* fields;
};

struct Blob {
    const uint8_t* p;
    uint32_t n;
};

// The ABSENCE values of the roster row's v1 fields. A field the capture could not source is written as
// its absence value AND left out of `fill`; the pair is what lets a reader tell "no team" from "team
// 0". B.0's uint8_t teamId/teamSlot/alignment cannot express absence at all -- they stay so that a row
// this version writes is still a B.0 row, and the 16-bit fields beside them are the answer.
// The value collides with the engine's own LOBBY.TEAM_NONE, and deliberately: a row whose team the
// engine reported as "none" is a row this file says has no team. The fact is PC_PRESENT, so there is
// no generated constant to name.
constexpr uint16_t kPlayerNoTeam16 = 0xFFFF;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
constexpr uint16_t kPlayerNoAlignment16 = 0xFFFF;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
constexpr uint64_t kPlayerTimeNone = ~0ull;

// Which fields of a roster row the capture actually sourced. This is the ON-DISK numbering, so there is
// exactly one copy of it and it lives here: `src/capture/roster.h`'s kRosterFill* must static_assert
// itself against these, the same rule kMaxGameStatePhase applies to gamestate::Phase.
enum PlayerFillBit : uint32_t {
    kPlayerFillSlot = 1u << 0,
    kPlayerFillName = 1u << 1,
    kPlayerFillPlatform = 1u << 2,
    kPlayerFillXplat = 1u << 3,
    kPlayerFillTeam = 1u << 4,
    kPlayerFillAlign = 1u << 5,
    kPlayerFillLoadout = 1u << 6,  // the 20 item ids; the two loadout NUMBERS have no reader yet
    kPlayerFillGen = 1u << 7,
    kPlayerFillProfile = 1u << 8,
    kPlayerFillLocal = 1u << 9,
};
constexpr uint32_t kPlayerFillAll = (1u << 10) - 1;

// The v2 extension's own word (RECORDER-3 F5). These bits were capture-side only, so the file had
// no room for them and a row whose jersey number was never sourced wrote 0 -- indistinguishable
// from a player genuinely wearing 0. They live in `fillExt`, NOT in `fill`: a reader that predates
// them sees the v1 record it has always seen, and `extendedV2` is what tells a reader that the
// absence of a bit here is a statement rather than an old file with nothing to say.
enum PlayerFillExtBit : uint32_t {
    kPlayerFillNumbers = 1u << 10,       // the two jersey numbers were read, whatever their value
    kPlayerFillLoadoutKeys = 1u << 11,   // the twenty slot keys, not just the items
    kPlayerFillLoadoutAvail = 1u << 12,  // how many entries the engine's component holds
    kPlayerFillProfileFromEntrantRec = 1u << 13,  // the document came from the lobby record
    kPlayerFillProfileLossyNumber = 1u << 14,     // the walk wrote null over a non-integer number
};
constexpr uint32_t kPlayerFillExtAll = kPlayerFillNumbers | kPlayerFillLoadoutKeys |
                                       kPlayerFillLoadoutAvail |
                                       kPlayerFillProfileFromEntrantRec |
                                       kPlayerFillProfileLossyNumber;

// The per-blob cap on the two profile documents, and the budget it comes from. The manifest is one
// chapter payload and a payload is kMaxChapterPayload (1 MiB, format.h), so a full roster's two blobs
// each have to leave room for the schemas, the round table and the marker list; manifest_io.cpp
// static_asserts that arithmetic against the real record size. A blob over the cap is stored SHORT with
// its bit set in `jsonTruncated` -- never silently cut, and the length on disk is the length stored.
// RECORDER-5 F2. This was 4096 and it is the buffer the capture asks the ENGINE with, so a profile
// document longer than it was refused ShortBuffer at the source and never reached the file at all:
// on the 09-19 live capture that lost 8 of 9 profiles, the one that fit being 2910 B. Real live
// ServerProfile documents are several KB.
//
// 16 KiB, and the three bounds above it are why it is not the 64 KiB a first reading suggests. All
// three are structural, none is a preference:
//   * S_EVENT's body length is a uint16_t (EventRec::bodyBytes), and ONE entrant's identity block is
//     header + entrant + 20 slots + a length word + a whole document. 64 KiB would put that block
//     past 65535 and desynchronise the stream it cannot be encoded into.
//   * the capture worker's drain scratch is 64 KiB and has to hold one whole record.
//   * playback's recorded-block pool is 64 entrants x 8 blocks of cells that must each hold the
//     widest legal body, so every byte here costs 512 bytes of that pool.
// 16 KiB clears all three, is four times the cap that lost the documents and five times the largest
// profile actually observed, and a document above it is still the named ShortBuffer refusal.
constexpr uint32_t kPlayerJsonCap = 16384;  // facts-ok: SESSION.FLAG_BIT_ACTIVE PC_PRESENT, so gen_facts emits no value; 16384 is this buffer's cap

// What every row of a FULL roster is guaranteed even when the manifest budget is tight. It is the
// old cap, so no roster that fits today can be made to fit less by this change. Rows are serialised
// in order and each one may take up to kPlayerJsonCap, but never the reserve the rows behind it need
// at this floor -- manifest_io.cpp holds that arithmetic, because the worst case (64 rows x 2 blobs
// x 32 KiB = 4 MiB) cannot be a static bound against a 1 MiB chapter payload.
constexpr uint32_t kPlayerJsonFloorCap = 4096;
constexpr uint32_t kPlayerJsonRosterBudget = 64;      // capture/roster.h's entrant band cap
constexpr uint32_t kManifestNonRosterBudget = 1u << 18;  // 256 KiB kept for the rest of the manifest

enum PlayerJsonTruncBit : uint8_t {
    kPlayerJsonTruncEntrant = 1u << 0,
    kPlayerJsonTruncUnlocks = 1u << 1,
};

struct PlayerRecord {
    uint64_t xplatformId;
    char platformStr[32];  // NUL-padded, not NUL-guaranteed
    char displayName[32];
    uint16_t userSlot, userIndex, generation;
    uint8_t teamId, teamSlot, alignment, bodyType;
    uint16_t loadoutNumber, loadoutNumberBody;
    Symbol64 loadoutSlots[20];  // the 168-byte SR15NetLoadout, ids only
    Blob entrantJson;
    Blob unlocksJson;  // REQUIRED - S9.3 breaker 2

    // v1. A file written before these existed deserialises with every one of them at its absence value
    // and `extended` clear, so "the capture did not record this" and "the capture recorded nothing
    // here" stay different statements.
    char displayNameFull[36];  // the engine field is CSysString 0x24, not 32
    char platformStrFull[36];
    uint16_t teamId16, teamSlot16, alignment16;  // alignment16 is the COLOUR
    uint64_t firstSeenUs, lastSeenUs;            // capture clock; kPlayerTimeNone when not recorded
    uint32_t fill;                               // PlayerFillBit
    uint8_t jsonTruncated;                       // PlayerJsonTruncBit
    uint8_t extended;                            // reader-set: this row came from a v1 record

    // v2. Absent from every file written before RECORDER-3, and `extendedV2` is the only thing that
    // says so: with it clear, `fillExt` is unknown rather than empty, and loadoutNumber keeps
    // exactly the meaning it had -- a value, with no statement about where it came from.
    uint32_t fillExt;      // PlayerFillExtBit
    uint8_t extendedV2;    // reader-set: this row came from a v2 record
};

struct RoundMarker {
    uint64_t tUs;
    uint16_t kind, index;
    uint64_t payload;
};

// ---- ARCH-21: the round table and the game-state sample --------------------------------------
// Where a marker table came from. Three values are not enough: a file written before this block existed
// carries no answer at all, and reporting that as `None` would turn "nobody asked" into "we asked and
// there was nothing", which is the two-state gate ARCH-21 C6/D11 exists to refuse.
enum class MarkerSource : uint8_t {
    NotRecorded = 0,  // the file carries no round-table block; every capture written before ARCH-21
    None = 1,         // the writer chose, and neither source was available (ARCH-21 S4.4)
    Local = 2,        // IGameState
    Wire = 3,         // the netvar deriver
};
constexpr uint8_t kMaxMarkerSource = static_cast<uint8_t>(MarkerSource::Wire);
const char* MarkerSourceName(uint8_t source);

// The ON-DISK numbering of gamestate::Phase and gamestate::Availability (ARCH-21 S4.1). The enums
// themselves live in W1-D's src/engine/gamestate_types.h, which static_asserts its values against these
// bounds; the numbering is a property of the file, so it is pinned here, in the file's own header, and
// there is no second copy for a reader to drift away from.
//
//  phase: 0 NotSampled, 1 Absent, 2 Unknown, 3 TeamRoomWait, 4 PreMatch, 5 RoundStart, 6 Playing,
//         7 Score, 8 RoundOver, 9 PostMatch, 10 PreSuddenDeath, 11 SuddenDeath, 12 PostSuddenDeath
//  availability: 0 NotAsked, 1 Ok, 2 NoNetGame, 3 NoRulesBlock, 4 NoScoreStore, 5 NotReadable,
//         6 VocabularyRefused
constexpr uint8_t kMaxGameStatePhase = 12;
constexpr uint8_t kMaxGameStateAvailability = 6;

// One bit per optional field of GameStateSample. ARCH-21 S4.1: no field is ever synthesised, so a field
// whose read did not answer is reported ABSENT rather than defaulted -- and a separate bit cannot be
// confused with data the way an in-range sentinel can. The numbering is on disk, so it is defined once,
// here, and W1-D's src/engine/gamestate_types.h static_asserts its PresentBit against these names
// bit for bit, as src/capture/playerstats.h does against kPsFill*.
//
// Fields that the sampler obtains from ONE store read share ONE bit: bluePoints/redPoints are a single
// depth-2 read, blueMatchScore/redMatchScore a single depth-0 read, and lastScoreTeam/lastScorePoints
// one adjacent pair at +0x10/+0x12. A bit per field would let a reader believe half of a pair answered
// while the other half did not -- a state no read this writer performs can produce.
//
// L2-RUN-8 F10: until 2026-09-05 this enum numbered 13 UNPAIRED bits that nothing in the tree ever
// wrote, while the sampler wrote the 11 below. A reader that trusted this header decoded the real
// 0x67D of `4baf2ec11acaf6d05eee1317d30d0a53` as gameTimer ABSENT on records carrying a live
// 300.000 -> 129.184 s clock, and as countdownTimer/firstLaunchOfRound PRESENT on two fields this
// build cannot read at all. The numbering below is what is on disk.
enum GameStateField : uint32_t {
    kGsPhaseSymbol = 1u << 0,
    kGsPausedSymbol = 1u << 1,
    kGsRoundCounter = 1u << 2,
    kGsRoundPoints = 1u << 3,   // bluePoints and redPoints -- one depth-2 read
    kGsMatchScore = 1u << 4,    // blueMatchScore and redMatchScore -- one depth-0 read
    kGsFrisbeeOwnerId = 1u << 5,
    kGsGameTimer = 1u << 6,
    kGsCountdownTimer = 1u << 7,
    kGsFirstLaunchOfRound = 1u << 8,
    kGsRoundWinningTeam = 1u << 9,
    kGsLastScore = 1u << 10,    // lastScoreTeam and lastScorePoints -- one +0x10/+0x12 pair
};
constexpr unsigned kGameStateFieldCount = 11;
constexpr uint32_t kGameStatePresentAll = (1u << kGameStateFieldCount) - 1u;

// Bit INDEX (not mask) -> the name a reader prints; nullptr for an index this numbering does not
// define. One table, so `echodemo info` cannot name a bit the format does not have.
const char* GameStateFieldName(unsigned bitIndex);

// ---- S_PLAYER_STATS: the per-entrant LIVE match block (ARCH-21 MAJOR-B2) ----------------------
// The engine writes these into /session and nothing in this repo captured them, so a replay could not
// reconstruct the scoreboard. The twelve scoreboard values are `sub_140155C80`'s
// `teams[..]|players[..]|stats|` keys in that function's own emission order; `ping`,
// `packetlossratio` and `is_emote_playing` come from the identity writer `sub_1401B2EA0`.
// The widths mirror the engine's own setters: `possession_time` and `packetlossratio` go through its
// float setter and every other number through its integer setter.
//
// These values MOVE during a match, so they are a stream keyed by user id and engine time, not a
// manifest roster row -- the roster row keeps the at-close values, this stream keeps the history.
// The record names its player by xplatformId when the roster sourced one and by userIndex always: on a
// host whose roster row carries no account id (L2-RUN-8 F13 -- every record of the 2026-09-05 capture)
// the id is kPlayerStatsNoXplatformId, and joining two such records by id alone would merge two
// players. userIndex is the roster row and is never the absence value for an active entrant.
// 0xFFFF collides with the engine's LOBBY.TEAM_NONE
// and that fact is PC_PRESENT, so gen_facts emits no value and there is no constant to name.
// The roster could not source an account id for this player. Named so no reader writes the literal
// and no reader mistakes it for user 0.
constexpr uint64_t kPlayerStatsNoXplatformId = 0;
constexpr uint16_t kPlayerStatsNoIndex = 0xFFFF;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
constexpr int32_t kPlayerStatNone = -1;           // an integer stat the capture could not source
constexpr float kPlayerStatFloatNone = -1.0f;     // possession_time / packetlossratio are both >= 0
constexpr uint8_t kPlayerStatEmoteNone = 0xFF;    // is_emote_playing is 0 or 1; this is neither
constexpr uint16_t kPlayerStatsMaxUsers = 64;     // distinct users one file's stream may name

// Which stats a record actually carries, and the same rule PlayerRecord::fill has: a value whose bit
// is clear is written as its absence value, so a zero-initialised record lands on disk as "not
// recorded" and never as "0 goals". This is the ON-DISK numbering and there is exactly one copy of it;
// a capture-side mirror must static_assert against these names, as kRosterFill* does against
// kPlayerFill*.
enum PlayerStatBit : uint32_t {
    kPsFillPossessionTime = 1u << 0,
    kPsFillPoints = 1u << 1,
    kPsFillGoals = 1u << 2,
    kPsFillStuns = 1u << 3,
    kPsFillBlocks = 1u << 4,
    kPsFillAssists = 1u << 5,
    kPsFillSaves = 1u << 6,
    kPsFillInterceptions = 1u << 7,
    kPsFillSteals = 1u << 8,
    kPsFillCatches = 1u << 9,
    kPsFillPasses = 1u << 10,
    kPsFillShotsTaken = 1u << 11,
    kPsFillPing = 1u << 12,
    kPsFillPacketLoss = 1u << 13,
    kPsFillEmote = 1u << 14,
};
constexpr uint32_t kPlayerStatsFillAll = (1u << 15) - 1u;

// What a producer hands DemoWriter::WritePlayerStats. Field-for-field the disk record
// (PlayerStatsRecordDisk, format.h); the writer copies field by field, so the two paddings are
// independent and neither struct is ever reinterpreted as the other.
struct PlayerStatsRecord {
    uint64_t xplatformId;    // the engine's `userid`, or kPlayerStatsNoXplatformId -- join on userIndex
    uint64_t engineTimeUs;   // the engine clock; per user this never goes backwards
    uint64_t captureTimeUs;  // the file's own axis
    float possessionTime;
    int32_t points, goals, stuns, blocks, assists, saves;
    int32_t interceptions, steals, catches, passes, shotsTaken;
    int32_t ping;  // milliseconds
    float packetLossRatio;
    uint32_t fill;       // PlayerStatBit
    uint16_t userIndex;  // the roster row this belongs to, or kPlayerStatsNoIndex
    uint8_t isEmotePlaying;
    uint8_t keyframe;  // 1 = the periodic copy, 0 = written because a value moved
};


// ARCH-21 C4/D4/M5. `round_counter` is a 32-bit engine value; MarkRec::index, MarkRecordDisk::index and
// RoundRow::number are all 16 bits, and D4 forbids widening the record. So the narrowing has a rule
// instead of a truncation: [0, 65534] is representable, 65535 means "no number", and a counter outside
// the range is REFUSED loudly by RoundNumberFromCounter -- never wrapped, never masked.
constexpr uint16_t kRoundNumberNone = UINT16_MAX;
constexpr uint32_t kMaxRoundNumber = UINT16_MAX - 1u;

enum RoundRowFlag : uint16_t {
    kRoundPartialStart = 1,      // the capture opened after this round began
    kRoundPartialEnd = 2,        // the capture closed before this round ended
    kRoundNumberSynthesised = 4, // the number is ours, not the engine's
    kRoundNumberRefused = 8,     // round_counter was out of range; the row is seekable by time only
    // ARCH-21 C5. The engine restarted `round_counter` under this file (a rematch inside one state-9
    // session). The row's number belongs to a NEW counter segment, re-based so the table still
    // ascends, and this flag is how a reader tells that number from the ones before it.
    kRoundCounterReset = 16,
};
constexpr uint16_t kAllRoundRowFlags = kRoundPartialStart | kRoundPartialEnd |
                                       kRoundNumberSynthesised | kRoundNumberRefused |
                                       kRoundCounterReset;

// One row per round played (ARCH-21 S4.5). `number` IS the engine's `round_counter` -- the number the
// browser shows and the number `echodemo extract --round N` takes -- so there is one round unit on
// every surface. Rows with a number ascend strictly and the number is unique within a file; rows whose
// number was refused carry kRoundNumberNone and are seekable by time only.
struct RoundRow {
    uint16_t number;
    uint16_t flags;
    uint8_t phaseAtBegin, phaseAtEnd;  // gamestate::Phase, numbered above
    uint16_t bluePointsAtEnd, redPointsAtEnd;          // depth-2 ROUND points (JSON blue_points)
    uint16_t blueMatchScoreAtEnd, redMatchScoreAtEnd;  // depth-0 MATCH score (JSON blue_match_score)
    uint16_t reserved;
    uint64_t beginUs, endUs;              // capture time, the file's own axis
    uint64_t beginEngineUs, endEngineUs;  // the engine clock the sample was taken against
};

struct Manifest {
    uint16_t formatMajor, formatMinor;
    uint64_t t0Us, demoUuidLo, demoUuidHi;
    uint8_t echovrMd5[16];
    uint64_t buildStringHash, contentHash;
    uint32_t captureRole;  // 0 client, 1 offline_host, 2 dedicated
    uint64_t authorityFlags;
    uint32_t flags;
    uint64_t levelId, gametypeId, startTimeEpoch;
    uint32_t keyframeIntervalUs;  // a field, never a constant - S5.5

    uint16_t playerCount;
    const PlayerRecord* players;
    uint16_t roundCount;
    const RoundMarker* rounds;  // the S_MARK edges mirrored; unchanged by ARCH-21
    // The ARCH-21 round table. `markerSource == MarkerSource::NotRecorded` means the file carries no
    // round-table block at all, which is what every capture written before ARCH-21 says and is not a
    // failure; a file that carries the block always names a source, even when that source is `None`.
    uint16_t roundRowCount;
    const RoundRow* roundRows;
    MarkerSource markerSource;
    // Non-null when the file CARRIED a round table this reader could not trust. The ROUNDS block is
    // then refused by this name and carries no rows, and the rest of the file opens and plays: a
    // capture thrown away to protect one feature is a lost recording, which is the worse outcome.
    const char* roundsRefused;
    uint16_t schemaCount;
    const SchemaDef* schemas;

    Blob balanceBlobZstd;  // stored verbatim, never re-encoded
    Blob slot0Baselines;
    Blob slot1RuleVars;
    Blob netIdSlotMap;

    bool partialStart;  // E-C17
};

struct ChapterKey {
    uint64_t tBeginUs, tEndUs;
    uint16_t section, seq;
};

struct StreamView {
    StreamId id;
    SchemaId schema;
    uint32_t recordCount;
    uint32_t observedCount, storedCount;  // S5.6.2 completeness counters
    const uint8_t* records;
    uint32_t bytes;
};

// ---- lifetime, and it is load-bearing -------------------------------------------------------
// A DecodedChapter, its `streams` array and every StreamView::records pointer borrow from one of TWO
// buffers the reader alternates (S11.3). A result stays valid across exactly ONE further successful
// DecodeChapter on the same reader; the SECOND one overwrites the buffer it points into.
//
// So two chapters may be live at once -- which is what a seek window straddling a boundary needs -- and
// three may not. The loop that collects N chapters and then walks them is a use-after-free that will not
// fail on two and will fail on three. Copy out what has to outlive that window.
struct DecodedChapter {
    ChapterKey key;
    uint16_t streamCount;
    const StreamView* streams;

    const StreamView* Find(StreamId id) const {
        for (uint16_t i = 0; i < streamCount; ++i) {
            if (streams[i].id == id) {
                return &streams[i];
            }
        }
        return nullptr;
    }
};

// ---- sections (S5.4) -------------------------------------------------------------------------
enum Section : uint16_t {
    kSectionManifest = 0,
    kSectionCore = 1,
    kSectionAsSeen = 2,
    kSectionVoip = 3,
    kSectionOracle = 4,
    kSectionIndex = 5,
    kSectionCount = 6,
};

// ---- streams (S5.4.5 - S5.4.7) ----------------------------------------------------------------
// The numbering is the container's; every other packet cites the name. New ids are additive and do not
// bump format_major (S5.9).
enum Stream : uint16_t {
    kStreamManifest = 1,   // the serialised Manifest, one record
    kStreamIndex = 2,      // the derived chapter table, one 32-byte entry per chapter
    kStreamTick = 3,       // S_TICK
    kStreamPlayerWire = 4,
    kStreamPlayerSampled = 5,
    kStreamBodyWire = 6,
    kStreamBodySampled = 7,
    kStreamNetvar = 8,
    kStreamEvent = 9,
    kStreamWorld = 10,
    kStreamKey = 11,       // S_KEY, the chapter's opening keyframe
    kStreamMark = 12,      // S_MARK, round and phase markers (S5.7)
    kStreamAsSeen = 13,
    kStreamVoip = 14,
    kStreamOracle = 15,
    // Completeness bookkeeping. Both are first-class content, not an error path (S5.6.2).
    kStreamGap = 16,       // the engine numbered records we never observed
    kStreamDropped = 17,   // records we observed and lost downstream, e.g. a full ring (E-C10)
    kStreamDegraded = 18,  // the S6.4 shed ladder, recorded in the file
    kStreamGameState = 19, // S_GAMESTATE, the on-change local game-state sample (ARCH-21 S2)
    kStreamPlayerStats = 20,  // S_PLAYER_STATS, the per-entrant live match block (ARCH-21 B2)
};

const char* SectionName(uint16_t section);
const char* StreamName(uint16_t stream);

// True for streams whose loss the completeness law must account for. The index and the manifest are
// container bookkeeping and the gap/dropped/degraded streams are the accounting itself.
bool StreamCarriesSamples(uint16_t stream);

// ---- the write policy of each stream ----------------------------------------------------------
// A property of the FORMAT, declared once here, because both sides of the file need the same answer
// and they are in different packets: the capture side picks where a stream's `observed` count may
// legally come from, and a reader decides whether a change in a stream's per-chapter record count is
// evidence of anything.
//
//   FixedCadence  one record per engine frame the stream was enabled for. The engine's own frame
//                 counter numbers these, so a missing record is detectable and a change in their
//                 per-chapter rate is a real event in the capture.
//   OnChange      the producer decides when to emit -- a value moved, a periodic copy, a message the
//                 engine dispatched. Nothing outside the producer numbers these, and their
//                 per-chapter count is DATA: it steps whenever the match does. A rate step on such a
//                 stream says something about the game, never about the capture.
enum class StreamCadence : uint8_t {
    FixedCadence,
    OnChange,
};

// Every id in `Stream` is listed. The static_assert below is the tripwire: a new stream id does not
// compile until it has been given a cadence here, because the silent answer -- whichever one the
// default arm happens to return -- would quietly disarm both readers of this function.
constexpr StreamCadence CadenceOf(uint16_t stream) {
    switch (stream) {
        case kStreamTick:
        case kStreamPlayerSampled:
        case kStreamBodySampled:
        case kStreamAsSeen:
        case kStreamKey:
            return StreamCadence::FixedCadence;
        case kStreamManifest:
        case kStreamIndex:
        case kStreamPlayerWire:
        case kStreamBodyWire:
        case kStreamNetvar:
        case kStreamEvent:
        case kStreamWorld:
        case kStreamMark:
        case kStreamVoip:
        case kStreamGap:
        case kStreamDropped:
        case kStreamDegraded:
        case kStreamGameState:
        case kStreamPlayerStats:
        // S_ORACLE is a SAMPLER decision -- one record per CORE chapter (S5.4.7), not one per engine
        // frame. While it sat in the FixedCadence list the accounting interpolated the 60-frame gap
        // between samples into thousands of phantom lost records (F40).
        case kStreamOracle:
            return StreamCadence::OnChange;
    }
    return StreamCadence::OnChange;
}

constexpr uint16_t kMaxKnownStreamId = kStreamPlayerStats;
static_assert(kMaxKnownStreamId == 20,
              "a new stream id must be given a cadence in CadenceOf before this number moves");

}  // namespace replay
