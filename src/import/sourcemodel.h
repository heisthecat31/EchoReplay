#pragma once

// The neutral, source-rate match model (DESIGN-FORMATS §7.1). Both readers fill it; Normalise, Infer
// and Emit read it, so nothing downstream knows which format a file came from. No engine types.
//
// Units: metres, seconds, m/s. Axes: API world (H0 says they are the engine's). Quaternions: xyzw,
// engine convention, from BasisToQuat (basis.h) or taken from the tape as given. Times: microseconds
// since SourceMatch::epochUtcUs. Frozen by P0: extend by appending, never reorder.

#include <stdint.h>
#include <vector>

namespace replay::import {

enum class SourceKind : uint8_t { EchoReplay = 1, Tape = 2 };

// API game_status; Empty is the "" the API reports on ~8% of frames (Normalise holds the prior phase).
enum class GameStatus : uint8_t {
    Empty = 0,
    PreMatch,
    RoundStart,
    Playing,
    Score,
    RoundOver,
    PostMatch,
    PreSuddenDeath,
    SuddenDeath,
    PostSuddenDeath,
};

enum class PauseState : uint8_t { Unpaused = 0, Paused, Unpausing };

enum class Hold : uint8_t { None = 0, Geo, Disc, Player };

constexpr uint8_t kTeamBlue = 0;
constexpr uint8_t kTeamOrange = 1;
constexpr uint8_t kTeamSpectator = 2;

// Order of SrcPlayer::stats and every stats row; possession_time is carried separately as a float.
enum StatIndex : uint8_t {
    kStatPoints,
    kStatGoals,
    kStatSaves,
    kStatStuns,
    kStatPasses,
    kStatCatches,
    kStatSteals,
    kStatBlocks,
    kStatInterceptions,
    kStatAssists,
    kStatShots,
    kStatCount,
};

enum BasisPart : uint8_t { kPartHead, kPartBody, kPartLHand, kPartRHand, kPartDisc, kPartCount };

constexpr uint16_t kNoSlot = UINT16_MAX;  // no file slot; Emit maps it to the wire's own slot-none
constexpr uint8_t kNotInFrame = 0xFF;    // a per-frame byte this source did not carry at this frame
constexpr uint64_t kNoUser = 0;          // userId sentinel: unresolved or none

struct Pose {
    float pos[3];  // world position; for hands this is the API `pos`, not an offset
    float q[4];
};

struct SrcPlayer {
    uint64_t userId;       // API userid / tape account_number (== native xplatformId); the identity key
    uint16_t rosterIndex;  // into SourceMatch::roster; after Normalise it IS the output file slot
    uint8_t team;          // kTeam*
    uint8_t apiSlot;       // API playerid / tape slot at THIS frame; reusable, never a key across frames
    Pose head, body, lhand, rhand;
    float vel[3];
    Hold holdL, holdR;
    uint8_t holdLSlot, holdRSlot;  // apiSlot of the held player when Hold::Player, else kNotInFrame
    bool stunned, possession, blocking, invulnerable, emote;
    uint16_t stats[kStatCount];
    float possessionTime;
    uint8_t number, level;  // kNotInFrame when this frame does not carry them (tape)
};

struct SrcDisc {
    Pose pose;
    float vel[3];
    uint16_t bounceCount;
    uint8_t holderApiSlot;  // tape disc_holder_slot; kNotInFrame when absent (echoreplay derives from holds)
};

struct SrcFrame {
    int64_t tUs;       // readers store the raw stamp in file order; Normalise repairs it (§7.4)
    uint16_t session;  // index into SourceMatch::sessions
    GameStatus status;
    PauseState pause;
    float gameClock;
    uint16_t bluePoints, orangePoints, blueRounds, orangeRounds;
    bool hasTeamStats;  // echoreplay teams[i].stats; tape leaves it false and Infer sums players
    uint16_t teamStats[2][kStatCount];
    float teamPossessionTime[2];
    SrcDisc disc;
    uint32_t firstPlayer, playerCount;  // slice of SourceMatch::players
    uint32_t firstEvent, eventCount;    // slice of SourceMatch::events stamped at this frame
};

// Field use per kind; unused fields are zero. Player references are userIds, resolved by the reader
// from names or slots at the event's frame (kNoUser when that fails, counted in SourceStats).
enum class SrcEventKind : uint16_t {
    GoalScored = 1,          // a scorer, b assist, c team | points<<8, f0 disc speed, f1 distance,
                             // text goal_type as the source spells it. Echoreplay: emitted whenever
                             // last_score changes, a snapshot Infer pairs with the points edge.
    StunCredited = 2,        // tape per-player stun stat: a the credited player. A puncher candidate only.
    PlayerJoined = 3,        // a player, c team
    PlayerLeft = 4,          // a player
    PlayerSwitchedTeam = 5,  // a player, c new team
    RoundStarted = 6,        // c round number
    RoundEnded = 7,          // c round number
    MatchEnded = 8,
    DiscThrown = 9,          // a thrower
    DiscCaught = 10,         // a catcher
};

struct SrcEvent {
    int64_t tUs;
    SrcEventKind kind;
    uint32_t c;
    uint64_t a, b;
    float f0, f1;
    char text[32];
};

struct SrcIdentity {
    uint64_t userId;
    char name[64];
    uint8_t number, level, team;       // as first seen; kNotInFrame when never carried
    int64_t firstSeenUs, lastSeenUs;   // filled by Normalise from present frames
};

struct SrcSession {
    char id[40];  // API sessionid (UUID) / tape session_id
};

// Reader-side counters. Normalise/Infer/Emit count into ConvertReport (report.h), which embeds this.
struct SourceStats {
    uint64_t sourceBytes;
    uint32_t zipEntries;          // > 1 is a warning; entry 0 is read
    uint32_t lines;               // echoreplay lines / tape envelopes
    uint32_t badLines;            // no tab, bad time, bad JSON or bad envelope; > 1% refuses (Corrupt)
    uint32_t errCodeFrames;       // err_code != 0, skipped
    uint32_t duplicateFrames;     // byte-identical JSON to the previous line, dropped
    uint32_t bonesSkipped;        // bones columns skipped unparsed
    uint32_t basisTrips[kPartCount];       // mirror guard (§5.2 step 2): prior quaternion reused
    uint32_t basisFirstTripFrame[kPartCount];
    uint32_t unresolvedPlayerRefs;  // event name/slot that matched no present player
    bool truncatedSource;           // tape stream ended mid-envelope; frames up to it are kept
};

struct SourceMatch {
    SourceKind kind;
    uint32_t nominalHz;      // tape header rate; 0 for echoreplay (Normalise measures the median dt)
    char mapName[64];        // as the source spells it
    uint64_t mapSymbol;      // 0 until Normalise's map gate resolves mapName
    bool privateMatch;
    int64_t epochUtcUs;      // UTC of tUs == 0: first line's stamp / tape created_at
    std::vector<SrcSession> sessions;
    std::vector<SrcIdentity> roster;  // union over the file, first-appearance order
    std::vector<SrcFrame> frames;     // file order, deduplicated
    std::vector<SrcPlayer> players;   // flat, sliced by frames
    std::vector<SrcEvent> events;     // sliced by frames
    SourceStats stats;
};

}  // namespace replay::import
