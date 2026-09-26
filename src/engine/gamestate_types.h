#pragma once

// The LOCAL game-state sample -- ARCH-21 S4.1's POD, declared here so W1-E's sampler, W2-A's
// marker source and W2-R's round rows all compile against one text.
//
// Two rules run through the whole struct and neither is negotiable:
//
//   1. NO FIELD IS EVER SYNTHESISED.  Every value carries a `present` bit; a read that did not
//      answer is reported ABSENT.  R1 used INT32_MIN for a missing roundCounter -- a magic value
//      inside the value's own range, which cannot survive a truncation rule and which a reader
//      cannot tell from data.  A separate bit can never be confused with data.
//   2. A FIELD WITH NO SOURCE IS DELETED, NOT DEFAULTED.  `suddenDeath` and `matchOver` are gone
//      (ARCH-21 D10): their netvar symbols are placeholders by capture_facts.h's own admission,
//      and a field nothing writes is the confident zero the recurring-defect list forbids.  Both
//      are derivable from `phase`, which IS sourced.

#include <stdint.h>

#include "foundation/demofile/demotypes.h"  // the ON-DISK present numbering -- B.0, owned by W1-B

namespace replay {
namespace gamestate {

// Finer than the engine's coarse id, never coarser.  TeamRoomWait exists BECAUSE the engine's own
// name mapper collapses it into "round_start": entering the team room happens between rounds, and
// a build that classified by name emitted a round marker there.
enum class Phase : uint8_t {
    NotSampled = 0,  // no sample taken yet -- NOT the same as "sampled and unknown"
    Absent,          // the game_state var is not in the store (no game mode loaded)
    Unknown,         // sampled, symbol read, matched NO row in our table -> never named, never a round
    TeamRoomWait,
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

inline const char* PhaseName(Phase phase) {
    switch (phase) {
        case Phase::NotSampled:      return "not_sampled";
        case Phase::Absent:          return "absent";
        case Phase::Unknown:         return "unknown";
        case Phase::TeamRoomWait:    return "team_room_wait";
        case Phase::PreMatch:        return "pre_match";
        case Phase::RoundStart:      return "round_start";
        case Phase::Playing:         return "playing";
        case Phase::Score:           return "score";
        case Phase::RoundOver:       return "round_over";
        case Phase::PostMatch:       return "post_match";
        case Phase::PreSuddenDeath:  return "pre_sudden_death";
        case Phase::SuddenDeath:     return "sudden_death";
        case Phase::PostSuddenDeath: return "post_sudden_death";
    }
    return "invalid";
}

// Why a sample is what it is.  A two-state gate cannot express asked-and-refused, could-not-ask
// and never-asked, so this does not have two states.
enum class Availability : uint8_t {
    NotAsked = 0,      // never attempted -- distinct from every refusal
    Ok,                // every field this sample reports as present actually answered
    NoNetGame,         // ISessionEntry::NetGame() is null
    NoRulesBlock,      // *(netGame + kRulesOff) is null              (depth-1 miss)
    NoScoreStore,      // *(*(netGame + kRulesOff)) is null           (depth-2 miss)
    NotReadable,       // a Readable() probe refused a cached slot
    VocabularyRefused, // our phase table disagreed with the engine's own at startup
};

inline const char* AvailabilityName(Availability availability) {
    switch (availability) {
        case Availability::NotAsked:          return "not_asked";
        case Availability::Ok:                return "ok";
        case Availability::NoNetGame:         return "no_net_game";
        case Availability::NoRulesBlock:      return "no_rules_block";
        case Availability::NoScoreStore:      return "no_score_store";
        case Availability::NotReadable:       return "not_readable";
        case Availability::VocabularyRefused: return "vocabulary_refused";
    }
    return "invalid";
}

// One bit per optional field.  Bit set == that read answered on this tick.
enum PresentBit : uint32_t {
    kHasPhaseSymbol        = 1u << 0,
    kHasPausedSymbol       = 1u << 1,
    kHasRoundCounter       = 1u << 2,
    kHasRoundPoints        = 1u << 3,   // bluePoints and redPoints together -- one store, one read
    kHasMatchScore         = 1u << 4,   // blueMatchScore and redMatchScore together
    kHasFrisbeeOwnerId     = 1u << 5,
    kHasGameTimer          = 1u << 6,
    kHasCountdownTimer     = 1u << 7,
    kHasFirstLaunchOfRound = 1u << 8,
    kHasRoundWinningTeam   = 1u << 9,
    kHasLastScore          = 1u << 10,  // lastScoreTeam and lastScorePoints together
};

// The numbering above is NOT ours. demofile/demotypes.h owns the on-disk mask and holds the only
// copy; these names are the sampler's spelling of the same bits, asserted one at a time exactly as
// src/capture/playerstats.h asserts kPsFill* against the format's. Nothing here may drift: a bit that
// moves on either side fails to compile instead of mis-decoding every capture already on disk.
// (L2-RUN-8 F10: for one release the two enums disagreed and the format header's copy had zero
// callers, so nothing noticed.)
static_assert(uint32_t(kHasPhaseSymbol) == uint32_t(kGsPhaseSymbol), "phaseSymbol bit");
static_assert(uint32_t(kHasPausedSymbol) == uint32_t(kGsPausedSymbol), "pausedSymbol bit");
static_assert(uint32_t(kHasRoundCounter) == uint32_t(kGsRoundCounter), "roundCounter bit");
static_assert(uint32_t(kHasRoundPoints) == uint32_t(kGsRoundPoints), "roundPoints bit");
static_assert(uint32_t(kHasMatchScore) == uint32_t(kGsMatchScore), "matchScore bit");
static_assert(uint32_t(kHasFrisbeeOwnerId) == uint32_t(kGsFrisbeeOwnerId), "frisbeeOwnerId bit");
static_assert(uint32_t(kHasGameTimer) == uint32_t(kGsGameTimer), "gameTimer bit");
static_assert(uint32_t(kHasCountdownTimer) == uint32_t(kGsCountdownTimer), "countdownTimer bit");
static_assert(uint32_t(kHasFirstLaunchOfRound) == uint32_t(kGsFirstLaunchOfRound),
              "firstLaunchOfRound bit");
static_assert(uint32_t(kHasRoundWinningTeam) == uint32_t(kGsRoundWinningTeam), "roundWinningTeam bit");
static_assert(uint32_t(kHasLastScore) == uint32_t(kGsLastScore), "lastScore bit");
// Bit-for-bit is not enough on its own: two enums can agree on every name and still differ in how
// many names there are, which is how a 13-bit "on-disk" mask coexisted with an 11-bit writer.
static_assert((kHasPhaseSymbol | kHasPausedSymbol | kHasRoundCounter | kHasRoundPoints |
               kHasMatchScore | kHasFrisbeeOwnerId | kHasGameTimer | kHasCountdownTimer |
               kHasFirstLaunchOfRound | kHasRoundWinningTeam | kHasLastScore) ==
                  kGameStatePresentAll,
              "the sampler's bits are exactly the on-disk set -- none missing, none extra");

struct GameStateSample {
    uint64_t engineTimeUs;      // the capture clock's source time for this tick
    uint64_t captureTimeUs;     // our monotonic capture time, the file's own axis
    uint64_t phaseSymbol;       // raw CSymbol64, ALWAYS stored, even when phase == Unknown
    uint64_t pausedSymbol;      // raw CSymbol64; meaning deliberately not claimed (ARCH-21 S3.4)
    uint32_t roundCounter;      // depth 0
    uint32_t bluePoints;        // depth 2 -- ROUND points, the JSON blue_points
    uint32_t redPoints;         // depth 2
    uint32_t blueMatchScore;    // depth 0 -- MATCH score, the JSON blue_match_score
    uint32_t redMatchScore;     // depth 0
    uint32_t frisbeeOwnerId;    // 0xFFFFFFFF == unowned, the engine's own absence value
    float    gameTimer;         // depth 0 -- `game_timer`, NOT the JSON key `game_clock`
    float    countdownTimer;    // depth 0
    uint32_t firstLaunchOfRound;
    int32_t  roundWinningTeam;  // depth 0
    uint16_t lastScoreTeam;     // depth 1 raw, block +0x10
    uint16_t lastScorePoints;   // depth 1 raw, block +0x12
    uint32_t present;           // PresentBit mask; 0 = that field did not answer
    uint8_t  phase;             // Phase
    uint8_t  availability;      // Availability
    uint8_t  pad[2];
};

inline bool Has(const GameStateSample& sample, PresentBit bit) {
    return (sample.present & static_cast<uint32_t>(bit)) != 0;
}

// The engine's own absence value for an unowned disc.  Named so no caller writes the literal and
// no caller mistakes it for a player id.
inline constexpr uint32_t kFrisbeeUnowned = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD not emitted by gen_facts, and this is the disc engines own all-ones absence value, not that id

struct Stats {
    uint64_t samples;
    uint64_t refusalsByReason[7];   // indexed by Availability
    uint64_t cacheInvalidations;
    uint64_t readableRefusals;
    uint64_t vocabularyChecked;
    uint64_t vocabularyMismatches;
    uint64_t reArms;
    uint64_t driftInvalidations;
};

class IGameState {
public:
    virtual ~IGameState() {}

    // O(1) on the capture tick: no allocation, no walk, no VirtualQuery, no lock.  Returns Ok
    // only when every field whose present bit it set actually answered.
    virtual Availability Sample(GameStateSample& out) = 0;
    virtual Availability State() const = 0;
    virtual const Stats& GetStats() const = 0;
};

}  // namespace gamestate
}  // namespace replay
