#include "engine/gamestate.h"

#include <string.h>

#include "engine/gamestate_facts.h"

namespace replay {
namespace gamestate {
namespace {

// What a set present bit MEANS here, said once and exactly.  The engine's getters are
// (store, CSymbol64) -> value with no out-of-band failure channel: a variable the store does not
// hold and a variable holding zero come back the same for the three integer fields.  So a set bit
// means "the read chain to the store this fact row names was intact and the engine's own getter was
// called on it" -- never "the store definitely held this variable".  The bit is still the whole
// point: it is CLEAR whenever the chain was not intact, which is the case R1's INT32_MIN sentinel
// could not express.  game_state and game_frisbee_ownerid do better: 0 is not a legal CSymbol64 and
// the disc's own absence value is 0xFFFFFFFF, so those two carry their own absence.
const UnreadableField kUnreadable[] = {
    {"pausedSymbol",
     "paused_state has no OlPrEfIx literal anywhere in echovr.exe; that symbol exists only on the "
     "netvar side (src/capture/capture_facts.h), so this sampler has nothing to ask the store for"},
    {"countdownTimer",
     "countdown_timer has no OlPrEfIx literal anywhere in echovr.exe; same netvar-only source"},
    {"firstLaunchOfRound",
     "first_launch_of_round has no OlPrEfIx literal anywhere in echovr.exe; same netvar-only source"},
};

Phase ClassifySymbol(uint64_t symbol) {
    for (uint32_t i = 0; i < facts::kPhaseFactCount; ++i) {
        if (facts::kPhaseFacts[i].symbol == symbol) {
            return facts::kPhaseFacts[i].phase;
        }
    }
    // Sampled, read, and in none of the 27 rows.  Never named and never a round -- the engine's own
    // coarse mapper would have called several of these "round_start".
    return Phase::Unknown;
}

class GameState final : public IGameState {
public:
    explicit GameState(IEngineAccess* access) : access_(access) { CheckVocabulary(); }

    Availability Sample(GameStateSample& out) override;
    Availability State() const override { return state_; }
    const Stats& GetStats() const override { return stats_; }

private:
    void CheckVocabulary();
    Availability Finish(Availability availability);

    IEngineAccess* access_;
    Stats          stats_;
    Availability   state_;
    bool           vocabularyOk_;
    void*          netGame_;
    void*          rules_;
    void*          scores_;

public:
    GameState(const GameState&) = delete;
    GameState& operator=(const GameState&) = delete;
};

void GameState::CheckVocabulary() {
    memset(&stats_, 0, sizeof stats_);
    state_ = Availability::NotAsked;
    vocabularyOk_ = false;
    netGame_ = nullptr;
    rules_ = nullptr;
    scores_ = nullptr;

    // Our table against the image's own bytes, once, at construction.  A build whose phase
    // vocabulary drifted from the binary it is running inside must refuse to classify rather than
    // name a phase from a stale symbol -- that is what VocabularyRefused is for.
    bool ok = true;
    for (uint32_t i = 0; i < facts::kPhaseFactCount; ++i) {
        uint64_t got = 0;
        stats_.vocabularyChecked++;
        if (!access_->ReadImageQword(facts::kPhaseFacts[i].literalVa, &got) ||
            got != facts::kPhaseFacts[i].symbol) {
            stats_.vocabularyMismatches++;
            ok = false;
        }
    }
    vocabularyOk_ = ok;
    if (!ok) {
        state_ = Availability::VocabularyRefused;
    }
}

Availability GameState::Finish(Availability availability) {
    state_ = availability;
    stats_.samples++;
    if (availability != Availability::Ok) {
        stats_.refusalsByReason[static_cast<size_t>(availability)]++;
    }
    return availability;
}

Availability GameState::Sample(GameStateSample& out) {
    // Zeroed, not merged: a field that did not answer this tick must not show last tick's value
    // behind a clear bit.  engineTimeUs and captureTimeUs are the CALLER's to stamp after this
    // returns -- the sampler has no clock of its own.
    out = GameStateSample();
    out.phase = static_cast<uint8_t>(Phase::NotSampled);

    if (!vocabularyOk_) {
        out.availability = static_cast<uint8_t>(Availability::VocabularyRefused);
        return Finish(Availability::VocabularyRefused);
    }

    void* netGame = access_->NetGame();
    if (netGame == nullptr) {
        netGame_ = nullptr;
        rules_ = nullptr;
        scores_ = nullptr;
        out.phase = static_cast<uint8_t>(Phase::Absent);
        out.availability = static_cast<uint8_t>(Availability::NoNetGame);
        return Finish(Availability::NoNetGame);
    }

    // The three pointer loads of S4.2 step 1, every tick, unconditionally.  Readable() is asked only
    // when a pointer CHANGES: the probe is a VirtualQuery, which takes the address space's lock, and
    // this runs inside the game's frame.  A pointer that has not changed was probed when it arrived.
    if (netGame != netGame_) {
        stats_.cacheInvalidations++;
        netGame_ = netGame;
        rules_ = nullptr;
        scores_ = nullptr;
        if (!access_->Readable(netGame, facts::kRulesOff + sizeof(void*))) {
            stats_.readableRefusals++;
            netGame_ = nullptr;
            out.availability = static_cast<uint8_t>(Availability::NotReadable);
            return Finish(Availability::NotReadable);
        }
    }

    // Depth 0: everything the /session writer and sub_1401AEE50 ask of netGame itself.
    const uint64_t phaseSymbol = access_->GetSymbol(netGame, facts::kVarGameState.symbol);
    out.phaseSymbol = phaseSymbol;
    if (phaseSymbol != 0) {
        out.present |= kHasPhaseSymbol;
        out.phase = static_cast<uint8_t>(ClassifySymbol(phaseSymbol));
    } else {
        // 0 is not a legal CSymbol64: the variable is not in this store, so no game mode is loaded.
        out.phase = static_cast<uint8_t>(Phase::Absent);
    }

    out.roundCounter = static_cast<uint32_t>(access_->GetInt(netGame, facts::kVarRoundCounter.symbol));
    out.present |= kHasRoundCounter;

    out.blueMatchScore =
        static_cast<uint32_t>(access_->GetInt(netGame, facts::kVarBlueGameScore.symbol));
    out.redMatchScore =
        static_cast<uint32_t>(access_->GetInt(netGame, facts::kVarOrangeGameScore.symbol));
    out.present |= kHasMatchScore;

    out.roundWinningTeam = access_->GetInt(netGame, facts::kVarRoundWinningTeam.symbol);
    out.present |= kHasRoundWinningTeam;

    out.gameTimer = access_->GetFloat(netGame, facts::kVarGameTimer.symbol);
    out.present |= kHasGameTimer;

    // The /session writer reads the disc owner through the SYMBOL getter (S3.1), so we do too; the
    // value it yields is the raw id, and 0xFFFFFFFF is the engine's own "unowned".
    out.frisbeeOwnerId =
        static_cast<uint32_t>(access_->GetSymbol(netGame, facts::kVarFrisbeeOwnerId.symbol));
    out.present |= kHasFrisbeeOwnerId;

    void* rules = *reinterpret_cast<void* const volatile*>(static_cast<uint8_t*>(netGame) +
                                                           facts::kRulesOff);
    if (rules == nullptr) {
        rules_ = nullptr;
        scores_ = nullptr;
        out.availability = static_cast<uint8_t>(Availability::NoRulesBlock);
        return Finish(Availability::NoRulesBlock);
    }
    if (rules != rules_) {
        stats_.cacheInvalidations++;
        rules_ = rules;
        scores_ = nullptr;
        if (!access_->Readable(rules, facts::kLastScorePointsOff + sizeof(uint16_t))) {
            stats_.readableRefusals++;
            rules_ = nullptr;
            out.availability = static_cast<uint8_t>(Availability::NotReadable);
            return Finish(Availability::NotReadable);
        }
    }

    // Depth 1, raw: the last-score pair the game-mode block carries at +0x10 / +0x12.
    const uint8_t* block = static_cast<const uint8_t*>(rules);
    out.lastScoreTeam =
        *reinterpret_cast<const volatile uint16_t*>(block + facts::kLastScoreTeamOff);
    out.lastScorePoints =
        *reinterpret_cast<const volatile uint16_t*>(block + facts::kLastScorePointsOff);
    out.present |= kHasLastScore;

    void* scores = *reinterpret_cast<void* const volatile*>(block);
    if (scores == nullptr) {
        scores_ = nullptr;
        out.availability = static_cast<uint8_t>(Availability::NoScoreStore);
        return Finish(Availability::NoScoreStore);
    }
    if (scores != scores_) {
        stats_.cacheInvalidations++;
        scores_ = scores;
        if (!access_->Readable(scores, sizeof(void*))) {
            stats_.readableRefusals++;
            scores_ = nullptr;
            out.availability = static_cast<uint8_t>(Availability::NotReadable);
            return Finish(Availability::NotReadable);
        }
    }

    // Depth 2: the ROUND points.  Asked of blue_score at depth 0 this reads a slot that is not
    // there, silently, forever -- which is why the depth is carried on the fact row.
    out.bluePoints = static_cast<uint32_t>(access_->GetInt(scores, facts::kVarBlueScore.symbol));
    out.redPoints = static_cast<uint32_t>(access_->GetInt(scores, facts::kVarRedScore.symbol));
    out.present |= kHasRoundPoints;

    out.availability = static_cast<uint8_t>(Availability::Ok);
    return Finish(Availability::Ok);
}

}  // namespace

const UnreadableField* UnreadableFields(size_t* count) {
    if (count != nullptr) {
        *count = sizeof kUnreadable / sizeof kUnreadable[0];
    }
    return kUnreadable;
}

IGameState* CreateGameState(IEngineAccess* access) {
    return access != nullptr ? new GameState(access) : nullptr;
}

void DestroyGameState(IGameState* state) { delete state; }

}  // namespace gamestate
}  // namespace replay
