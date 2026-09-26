#include "capture/markers.h"

#include <string.h>

#include "capture/capture_facts.h"
#include "foundation/gametype.h"   // SymbolHash: the watch list is asserted, not transcribed

namespace replay {
namespace capture {
namespace {

// The order here is the bit order of Stats::namesSeenMask.
enum WatchSlot : uint16_t {
    kwGameState = 0,
    kwBlueScore,
    kwRedScore,
    kwFirstLaunch,
    kwSuddenDeath,
    kwMatchOver,
    kwPausedState,
    kwCountdown,
    kwFrisbeeOwner,
};

const uint64_t kWatchedNames[] = {
    capture_facts::kNvGameState,      capture_facts::kNvBlueScore,
    capture_facts::kNvRedScore,       capture_facts::kNvFirstLaunchOfRound,
    capture_facts::kNvSuddenDeath,    capture_facts::kNvMatchIsOver,
    capture_facts::kNvPausedState,    capture_facts::kNvCountdownTimer,
    capture_facts::kNvFrisbeeOwnerId,
};

// RECORDER-5. The deriver's watch list and playback's kHudVars are two transcriptions of one set of
// names, and two of these rows held placeholders that collided with real netvars. Both are now
// checked against the name hash itself, here, at the one site that reads them -- the same assertion
// netvarhud.cpp makes over its own copy, so a value that drifts in either list stops the build.
static_assert(capture_facts::kNvSuddenDeath == gametype::SymbolHash("sudden_death_active"),
              "sudden_death_active");
static_assert(capture_facts::kNvMatchIsOver == gametype::SymbolHash("match_is_over"),
              "match_is_over");
static_assert(capture_facts::kNvPausedState == gametype::SymbolHash("net_game_paused_state"),
              "net_game_paused_state -- NOT paused_state; the value was always right, the name was not");
static_assert(capture_facts::kNvGameState == gametype::SymbolHash("game_state"), "game_state");
static_assert(capture_facts::kNvGameTimer == gametype::SymbolHash("game_timer"), "game_timer");
static_assert(capture_facts::kNvCountdownTimer == gametype::SymbolHash("countdown_timer"),
              "countdown_timer");
static_assert(capture_facts::kNvBlueScore == gametype::SymbolHash("blue_score"), "blue_score");
static_assert(capture_facts::kNvRedScore == gametype::SymbolHash("red_score"), "red_score");
static_assert(capture_facts::kNvFirstLaunchOfRound == gametype::SymbolHash("first_launch_of_round"),
              "first_launch_of_round");
static_assert(capture_facts::kNvFrisbeeOwnerId == gametype::SymbolHash("game_frisbee_ownerid"),
              "game_frisbee_ownerid");

// The scalar SValue tag: hi dword 0x212, low dword the value in its declared native type. Anything
// else is not a scalar and must not be read as one -- the client's own getter branches on the encoding,
// which is why nothing here ever converts an SValue and every comparison is on the raw qword or on a
// value this predicate has confirmed is a scalar.
bool ScalarInt(uint64_t v, uint32_t& out) {
    if (static_cast<uint32_t>(v >> 32) != 0x00000212u) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool ScalarFloat(uint64_t v, float& out) {
    uint32_t bits;
    if (!ScalarInt(v, bits)) {
        return false;
    }
    memcpy(&out, &bits, sizeof out);
    return true;
}

}  // namespace

const char* MarkKindName(uint16_t k) {
    switch (k) {
        case kMarkMatchBegin: return "MATCH_BEGIN";
        case kMarkMatchEnd: return "MATCH_END";
        case kMarkRoundBegin: return "ROUND_BEGIN";
        case kMarkRoundEnd: return "ROUND_END";
        case kMarkLaunchRelease: return "LAUNCH_RELEASE";
        case kMarkGoal: return "GOAL";
        case kMarkSuddenDeath: return "SUDDEN_DEATH";
        case kMarkOvertime: return "OT";
        case kMarkMatchOver: return "MATCH_OVER";
        case kMarkPauseBegin: return "PAUSE_BEGIN";
        case kMarkPauseEnd: return "PAUSE_END";
        case kMarkJoin: return "JOIN";
        case kMarkLeave: return "LEAVE";
        case kMarkTeamChange: return "TEAM_CHANGE";
        case kMarkBookmark: return "BOOKMARK";
        case kMarkPhase: return "PHASE";
        default: return "?";
    }
}

MarkerDeriver::MarkerDeriver() { Reset(); }

void MarkerDeriver::Reset() {
    for (uint16_t i = 0; i < kWatchedCount; ++i) {
        watched_[i].name = kWatchedNames[i];
        watched_[i].value = 0;
        watched_[i].baseline = 0;
        watched_[i].seen = false;
    }
    primed_ = true;
    roundIndex_ = 0;
    stats_ = Stats{};
    stats_.namesWatched = kWatchedCount;
}

int MarkerDeriver::Index(uint64_t name) const {
    for (uint16_t i = 0; i < kWatchedCount; ++i) {
        if (watched_[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

MarkRec MarkerDeriver::MatchBegin(uint64_t tUs) const {
    MarkRec m{};
    m.tUs = tUs;
    m.kind = kMarkMatchBegin;
    return m;
}

MarkRec MarkerDeriver::MatchEnd(uint64_t tUs) const {
    MarkRec m{};
    m.tUs = tUs;
    m.kind = kMarkMatchEnd;
    m.index = roundIndex_;
    return m;
}

uint32_t MarkerDeriver::Note(const NetvarMessage& msg, uint64_t tUs, MarkRec* out, uint32_t cap) {
    if (!primed_) {
        Reset();
    }
    uint32_t n = 0;
    auto emit = [&](uint16_t kind, uint16_t index, uint64_t payload) {
        if (n < cap) {
            out[n].tUs = tUs;
            out[n].kind = kind;
            out[n].index = index;
            out[n].payload = payload;
            ++n;
            ++stats_.emitted;
        }
    };

    for (uint16_t r = 0; r < msg.varCount; ++r) {
        const NetvarRow& row = msg.rows[r];
        ++stats_.rowsSeen;
        const int at = Index(row.name);
        if (at < 0) {
            continue;
        }
        Watched& w = watched_[at];
        const uint64_t prev = w.value;
        const bool first = !w.seen;
        if (first) {
            // A first sighting is a BASELINE, not an edge. A capture that joined a match already in
            // progress opens with blue_score 2; nobody just scored twice.
            w.seen = true;
            w.baseline = row.value;
            w.value = row.value;
            stats_.namesSeenMask |= 1u << at;
            continue;
        }
        w.value = row.value;
        if (prev == row.value) {
            continue;
        }

        switch (at) {
            case kwGameState:
                // ★ A phase change, and nothing stronger is claimed. R4 recovered ten `game_state`
                // values; this code knows none of them and deliberately does not carry a table of
                // symbols nobody re-derived on PC. So the edge is recorded as PHASE with the new
                // symbol as its payload -- a reader that HAS the vocabulary can name the phase, and we
                // have not guessed which of the ten means "the round started". Calling every one of
                // them ROUND_BEGIN would have produced about ten round markers per match, which looks
                // exactly like a correct result.
                emit(kMarkPhase, roundIndex_, row.value);
                break;
            case kwBlueScore:
            case kwRedScore: {
                uint32_t now = 0, was = 0;
                if (ScalarInt(row.value, now) && ScalarInt(prev, was) && now > was) {
                    emit(kMarkGoal, at == kwBlueScore ? 0 : 1, now);
                }
                break;
            }
            case kwFirstLaunch: {
                // ★ THE ROUND BOUNDARY, and it is derived from R4's four-way conjunction rather than
                // from a symbol nobody pinned: `first_launch_of_round` going 1 -> 0, with the disc
                // unowned and the countdown at zero. Each round has exactly one, so this is what
                // advances the round index -- which is what "capture by round played" needs, and it
                // rests on a conjunction R4 verified instead of on the phase vocabulary it did not.
                uint32_t now = 0, was = 0;
                float countdown = 0.0f;
                const bool haveCountdown =
                    watched_[kwCountdown].seen && ScalarFloat(watched_[kwCountdown].value, countdown);
                const bool unowned =
                    watched_[kwFrisbeeOwner].seen &&
                    static_cast<uint32_t>(watched_[kwFrisbeeOwner].value) == 0xFFFFFFFFu;
                if (ScalarInt(row.value, now) && ScalarInt(prev, was) && was == 1 && now == 0 &&
                    haveCountdown && countdown < 1.0f && unowned) {
                    emit(kMarkLaunchRelease, roundIndex_, 0);
                    emit(kMarkRoundBegin, roundIndex_, 0);
                    ++roundIndex_;
                }
                break;
            }
            case kwSuddenDeath: {
                uint32_t now = 0;
                if (ScalarInt(row.value, now) && now != 0) {
                    emit(kMarkSuddenDeath, roundIndex_, row.value);
                }
                break;
            }
            case kwMatchOver: {
                uint32_t now = 0;
                if (ScalarInt(row.value, now) && now != 0) {
                    emit(kMarkMatchOver, roundIndex_, row.value);
                }
                break;
            }
            case kwPausedState:
                // Which CSymbol64 means "paused" is R4's reading, not a value this project re-derived.
                // So the marker is stated against the value the capture OPENED with: moving away from
                // it is a pause edge, returning to it is the release. That is computable from the
                // stream alone and needs no vocabulary -- and the payload carries the symbol either
                // way, so a reader who knows the pair can check us.
                emit(row.value == w.baseline ? kMarkPauseEnd : kMarkPauseBegin, roundIndex_,
                     row.value);
                break;
            default:
                break;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------
// LocalMarkerSource -- ARCH-21 S4.3's rules B1..B9, in that order, against W1-D's sample.
// ---------------------------------------------------------------------------------------------

namespace {

bool PhaseIsSuddenDeath(gamestate::Phase p) {
    return p == gamestate::Phase::PreSuddenDeath || p == gamestate::Phase::SuddenDeath ||
           p == gamestate::Phase::PostSuddenDeath;
}

// The phases a capture can open INSIDE. Opening here is a late join: the round is already running,
// the baseline rule forbids inventing a ROUND_BEGIN for it, and the row still exists because this is
// the only footage of that round anyone has (S5 rule 4).
bool PhaseIsInRound(gamestate::Phase p) {
    return p == gamestate::Phase::RoundStart || p == gamestate::Phase::Playing ||
           p == gamestate::Phase::Score;
}

uint16_t ClampPoints(uint32_t v, uint32_t& clamped) {
    if (v > UINT16_MAX) {
        ++clamped;
        return UINT16_MAX;
    }
    return static_cast<uint16_t>(v);
}

}  // namespace

void LocalMarkerSource::Reset() {
    rowCount_ = 0;
    roundOpen_ = false;
    last_ = gamestate::GameStateSample{};
    primed_ = false;
    prevPhase_ = 0;
    prevPhaseSymbol_ = 0;
    suddenDeathAnnounced_ = false;
    hasCounter_ = hasPoints_ = hasLaunch_ = hasPaused_ = false;
    prevCounter_ = prevBlue_ = prevRed_ = prevLaunch_ = 0;
    prevPaused_ = pausedBaseline_ = 0;
    curNumber_ = kRoundNumberNone;
    lastNumber_ = 0;
    sawNumber_ = false;
    counterRebased_ = false;
    lastRoundBeginUs_ = lastOkUs_ = gapOpenUs_ = 0;
    refusalPending_ = refusalsNamed_ = 0;
    stats_ = Stats{};
}

void LocalMarkerSource::OpenRow(uint16_t number, uint16_t flags) {
    if (rowCount_ >= kMaxRows) {
        ++stats_.rowsDropped;
        return;
    }
    RoundRow& r = rows_[rowCount_++];
    r = RoundRow{};
    r.number = number;
    r.flags = flags;
    r.phaseAtBegin = last_.phase;
    r.phaseAtEnd = static_cast<uint8_t>(gamestate::Phase::NotSampled);
    r.beginUs = last_.captureTimeUs;
    r.beginEngineUs = last_.engineTimeUs;
    roundOpen_ = true;
    if (number != kRoundNumberNone) {
        lastNumber_ = number;
        sawNumber_ = true;
    }
}

bool LocalMarkerSource::AdmitNumber(uint16_t& number, uint16_t& flags) {
    // A numberless row makes no ordering claim, so it can never break the ordering.
    if (number == kRoundNumberNone) {
        return true;
    }
    if (!sawNumber_ || number > lastNumber_) {
        if (counterRebased_) {
            flags |= kRoundCounterReset;
        }
        return true;
    }
    // The number would not ascend. Exactly one thing rescues it: the engine restarted its counter,
    // which makes this a new segment of the table rather than a contradiction inside the old one.
    if (!counterRebased_) {
        ++stats_.refusedNonAscending;
        NoteRefusal(number);
        return false;
    }
    const uint32_t next = uint32_t(lastNumber_) + 1u;
    if (next > kMaxRoundNumber) {
        ++stats_.roundNumberOutOfRange;
        number = kRoundNumberNone;
        flags |= kRoundNumberRefused;
        return true;
    }
    number = static_cast<uint16_t>(next);
    flags |= kRoundNumberSynthesised | kRoundCounterReset;
    ++stats_.rebasedAfterReset;
    return true;
}

void LocalMarkerSource::NoteRefusal(uint16_t offered) {
    // The rate limit is on what is RECORDED, not on what is taken: draining does not buy a
    // pathological session another kMaxRefusalsNamed lines.
    if (refusalsNamed_ >= kMaxRefusalsNamed) {
        return;
    }
    ++refusalsNamed_;
    RefusalNote& n = refusals_[refusalPending_++];
    n.captureTimeUs = last_.captureTimeUs;
    n.offered = offered;
    n.admitted = lastNumber_;
    n.phase = last_.phase;
}

uint32_t LocalMarkerSource::TakeRefusals(RefusalNote* out, uint32_t cap) {
    const uint32_t n = refusalPending_ < cap ? refusalPending_ : cap;
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = refusals_[i];
    }
    for (uint32_t i = n; i < refusalPending_; ++i) {
        refusals_[i - n] = refusals_[i];
    }
    refusalPending_ -= n;
    return n;
}

void LocalMarkerSource::CloseRow(uint16_t flags) {
    if (!roundOpen_) {
        return;
    }
    RoundRow& r = rows_[rowCount_ - 1];
    r.flags |= flags;
    r.phaseAtEnd = last_.phase;
    r.endUs = last_.captureTimeUs;
    r.endEngineUs = last_.engineTimeUs;
    r.bluePointsAtEnd = ClampPoints(last_.bluePoints, stats_.pointsClamped);
    r.redPointsAtEnd = ClampPoints(last_.redPoints, stats_.pointsClamped);
    r.blueMatchScoreAtEnd = ClampPoints(last_.blueMatchScore, stats_.pointsClamped);
    r.redMatchScoreAtEnd = ClampPoints(last_.redMatchScore, stats_.pointsClamped);
    roundOpen_ = false;
}

uint32_t LocalMarkerSource::Note(const gamestate::GameStateSample& s, MarkRec* out, uint32_t cap) {
    if (s.availability != static_cast<uint8_t>(gamestate::Availability::Ok)) {
        ++stats_.samplesRefused;
        // The source is NOT switched mid-file (S4.4). The table simply has a hole, and the hole is
        // measured so a reader can see how much of the match had no marker source.
        if (primed_ && gapOpenUs_ == 0) {
            gapOpenUs_ = lastOkUs_;
        }
        return 0;
    }
    ++stats_.samples;
    if (gapOpenUs_ != 0) {
        if (s.captureTimeUs > gapOpenUs_) {
            stats_.localSampleGapUs += s.captureTimeUs - gapOpenUs_;
        }
        gapOpenUs_ = 0;
    }
    lastOkUs_ = s.captureTimeUs;
    last_ = s;

    uint32_t n = 0;
    const auto emit = [&](uint16_t kind, uint16_t index, uint64_t payload) {
        if (n >= cap) {
            ++stats_.marksDroppedForCap;
            return;
        }
        MarkRec& m = out[n++];
        m.tUs = s.captureTimeUs;
        m.kind = kind;
        m.index = index;
        m.payload = payload;
        ++stats_.emitted;
        if (kind == kMarkRoundBegin || kind == kMarkRoundEnd) {
            ++stats_.localBoundaries;
        }
    };

    const gamestate::Phase phase = static_cast<gamestate::Phase>(s.phase);
    if (phase == gamestate::Phase::Unknown) {
        ++stats_.phaseUnknown;
    }
    const bool firstSample = !primed_;
    if (firstSample) {
        primed_ = true;
        prevPhase_ = s.phase;
        prevPhaseSymbol_ = s.phaseSymbol;
        if (PhaseIsInRound(phase)) {
            uint16_t number = kRoundNumberNone;
            uint16_t flags = kRoundPartialStart;
            if (gamestate::Has(s, gamestate::kHasRoundCounter)) {
                if (RoundNumberFromCounter(s.roundCounter, number)) {
                    curNumber_ = number;
                } else {
                    ++stats_.roundNumberOutOfRange;
                    flags |= kRoundNumberRefused;
                }
            } else {
                flags |= kRoundNumberRefused;
            }
            if (AdmitNumber(number, flags)) {
                OpenRow(number, flags);
            }
        }
    }

    // B1 -- the counter went up. Decidable with no vocabulary at all, which is why it is primary and
    // B2 is its fallback rather than its tie-break.
    bool b1 = false;
    bool roundBeginEmitted = false;
    if (gamestate::Has(s, gamestate::kHasRoundCounter)) {
        if (!hasCounter_) {
            hasCounter_ = true;
            prevCounter_ = s.roundCounter;
            uint16_t number = kRoundNumberNone;
            if (RoundNumberFromCounter(s.roundCounter, number)) {
                curNumber_ = number;
            }
        } else if (s.roundCounter > prevCounter_) {
            uint16_t number = kRoundNumberNone;
            uint16_t flags = 0;
            if (RoundNumberFromCounter(s.roundCounter, number)) {
                if (AdmitNumber(number, flags)) {
                    if (roundOpen_) {
                        emit(kMarkRoundEnd, CurrentNumber(), 0);
                        CloseRow(0);
                    }
                    curNumber_ = number;
                    emit(kMarkRoundBegin, number, 0);
                    roundBeginEmitted = true;
                    OpenRow(number, flags);
                    lastRoundBeginUs_ = s.captureTimeUs;
                }
                // Filed or refused, the counter moved: B2 must not retry the same boundary.
                b1 = true;
            } else {
                // C4/M5. The boundary is REFUSED, not truncated: no marker, and the row that follows
                // is numberless and seekable by time only.
                ++stats_.roundNumberOutOfRange;
                if (roundOpen_) {
                    CloseRow(0);
                }
                curNumber_ = kRoundNumberNone;
                OpenRow(kRoundNumberNone, kRoundNumberRefused);
            }
            prevCounter_ = s.roundCounter;
        } else if (s.roundCounter < prevCounter_) {
            // C5. The engine restarted its counter under one file, so its value is no longer an
            // ascending key. Every later row is numbered from where the table already stands and says
            // so; a descending row -- which the reader refuses the file over -- is never written.
            stats_.roundCounterReset = true;
            counterRebased_ = true;
            prevCounter_ = s.roundCounter;
        }
    }

    if (s.phase != prevPhase_ || s.phaseSymbol != prevPhaseSymbol_) {
        prevPhase_ = s.phase;
        prevPhaseSymbol_ = s.phaseSymbol;
        switch (phase) {
            case gamestate::Phase::RoundStart: {
                // B2. The id-1 symbols only -- `team_room_wait_*` is id 0 and lands in B8, which is
                // BL-3: the engine's own NAME mapper collapses the two and R1 filed a round marker
                // every time anyone entered the team room.
                const bool recentB1 =
                    lastRoundBeginUs_ != 0 && s.captureTimeUs >= lastRoundBeginUs_ &&
                    s.captureTimeUs - lastRoundBeginUs_ <= kB2SuppressUs;
                if (!b1 && !recentB1) {
                    uint16_t number = kRoundNumberNone;
                    uint16_t flags = 0;
                    bool ok = true;
                    if (gamestate::Has(s, gamestate::kHasRoundCounter)) {
                        ok = RoundNumberFromCounter(s.roundCounter, number);
                    } else {
                        flags |= kRoundNumberRefused;
                    }
                    if (!ok) {
                        ++stats_.roundNumberOutOfRange;
                    } else if (AdmitNumber(number, flags)) {
                        if (roundOpen_) {
                            emit(kMarkRoundEnd, CurrentNumber(), 0);
                            CloseRow(0);
                        }
                        curNumber_ = number;
                        emit(kMarkRoundBegin, number, 0);
                        roundBeginEmitted = true;
                        OpenRow(number, flags);
                        lastRoundBeginUs_ = s.captureTimeUs;
                    }
                }
                // The round table's refusal is not the phase timeline's problem. A RoundStart
                // change that files no boundary -- refused number, unrepresentable counter, or B1's
                // window -- still gets the mark B8 gives every other phase change, so a reader of
                // S_MARK alone never sees a hole where S_GAMESTATE shows a phase (DIAG R1).
                if (!roundBeginEmitted && s.phaseSymbol != 0) {
                    emit(kMarkPhase, CurrentNumber(), s.phaseSymbol);
                }
                break;
            }
            case gamestate::Phase::RoundOver:
                // B3.
                if (roundOpen_) {
                    emit(kMarkRoundEnd, CurrentNumber(), 0);
                    CloseRow(0);
                }
                break;
            case gamestate::Phase::PostMatch:
                // B4. Derived from the phase, which is sourced -- `kNvMatchIsOver` is a placeholder
                // symbol by capture_facts.h's own admission (S3.4) and is not consulted here.
                emit(kMarkMatchOver, CurrentNumber(), s.phaseSymbol);
                break;
            default:
                if (PhaseIsSuddenDeath(phase)) {
                    // B7. SUDDEN_DEATH once, on whichever of the three the match reaches first; the
                    // rest of the cluster is a phase change like any other.
                    if (!suddenDeathAnnounced_) {
                        suddenDeathAnnounced_ = true;
                        emit(kMarkSuddenDeath, CurrentNumber(), s.phaseSymbol);
                    } else {
                        emit(kMarkPhase, CurrentNumber(), s.phaseSymbol);
                    }
                } else if (s.phaseSymbol != 0) {
                    // B8, including TeamRoomWait and Unknown. The raw symbol is the payload, so a
                    // reader with a vocabulary we have not re-derived can name what we would not.
                    // A zero symbol is Phase::Absent -- no game mode loaded, nothing to name.
                    emit(kMarkPhase, CurrentNumber(), s.phaseSymbol);
                }
                break;
        }
    }

    // B5. `index` is the TEAM here and the round number everywhere else, which is the shipped wire
    // deriver's convention (markers.cpp emit(kMarkGoal, at == kwBlueScore ? 0 : 1, now)) and the
    // reason AUDIT_ARCH_21 R2-c1 excludes GOAL from the differential's index mapping.
    if (gamestate::Has(s, gamestate::kHasRoundPoints)) {
        if (!hasPoints_) {
            hasPoints_ = true;
        } else {
            if (s.bluePoints > prevBlue_) {
                emit(kMarkGoal, 0, s.bluePoints);
            }
            if (s.redPoints > prevRed_) {
                emit(kMarkGoal, 1, s.redPoints);
            }
        }
        prevBlue_ = s.bluePoints;
        prevRed_ = s.redPoints;
    }

    // B6 -- R4's four-way conjunction, kept verbatim so the local sequence can contain the one kind
    // both sources derive from the same edge.
    if (gamestate::Has(s, gamestate::kHasFirstLaunchOfRound)) {
        if (!hasLaunch_) {
            hasLaunch_ = true;
        } else if (prevLaunch_ == 1 && s.firstLaunchOfRound == 0 &&
                   gamestate::Has(s, gamestate::kHasFrisbeeOwnerId) &&
                   s.frisbeeOwnerId == gamestate::kFrisbeeUnowned &&
                   gamestate::Has(s, gamestate::kHasCountdownTimer) && s.countdownTimer < 1.0f) {
            emit(kMarkLaunchRelease, CurrentNumber(), 0);
        }
        prevLaunch_ = s.firstLaunchOfRound;
    }

    // B9 -- baseline-relative, because which CSymbol64 means "paused" is not a fact this project
    // re-derived. The payload carries the symbol, so a reader who knows the pair can check us.
    if (gamestate::Has(s, gamestate::kHasPausedSymbol)) {
        if (!hasPaused_) {
            hasPaused_ = true;
            pausedBaseline_ = s.pausedSymbol;
        } else if (s.pausedSymbol != prevPaused_) {
            emit(s.pausedSymbol == pausedBaseline_ ? kMarkPauseEnd : kMarkPauseBegin, CurrentNumber(),
                 s.pausedSymbol);
        }
        prevPaused_ = s.pausedSymbol;
    }
    return n;
}

void LocalMarkerSource::Finish(uint64_t captureTimeUs) {
    if (!roundOpen_) {
        return;
    }
    if (captureTimeUs > last_.captureTimeUs) {
        last_.captureTimeUs = captureTimeUs;
    }
    CloseRow(kRoundPartialEnd);
}

// ---------------------------------------------------------------------------------------------

void MarkerSourceChoice::Reset() {
    source_ = MarkerSource::NotRecorded;
    locked_ = false;
    localCandidate_ = true;
    corroboratorAsked_ = false;
    wireRowsSeen_ = 0;
    ticks_ = 0;
    openedUs_ = 0;
}

void MarkerSourceChoice::ExpireIfDue(uint64_t tUs) {
    if (locked_) {
        return;
    }
    const bool byTicks = ticks_ >= kArmTicks;
    const bool byTime = openedUs_ != 0 && tUs > openedUs_ && tUs - openedUs_ >= kArmWindowUs;
    if (!byTicks && !byTime) {
        return;
    }
    // The window is over, so the wire path may file if it has anything. None is NOT written here:
    // a window that expired with nothing readable has established that the first two seconds were
    // quiet and nothing else, and a capture that opens in the lobby is quiet by construction.
    // Settle() is the only place allowed to say a file never had a source.
    if (source_ == MarkerSource::NotRecorded && wireRowsSeen_ > 0) {
        source_ = MarkerSource::Wire;
    }
}

void MarkerSourceChoice::NoteLocalSample(bool ok, uint64_t tUs) {
    if (locked_ || source_ == MarkerSource::Local) {
        return;
    }
    ++ticks_;
    if (ok && localCandidate_) {
        // LOCAL is primary in every role (D2), so one readable sample ends the choice -- inside the
        // window or long after it, as long as no wire mark has been filed yet.
        source_ = MarkerSource::Local;
        return;
    }
    ExpireIfDue(tUs);
}

void MarkerSourceChoice::NoteCorroboratorFed(uint32_t rows, uint64_t tUs) {
    corroboratorAsked_ = true;
    wireRowsSeen_ += rows;
    if (locked_ || source_ == MarkerSource::Local) {
        return;
    }
    if (!localCandidate_) {
        // Nothing can ever answer LOCAL in this build, so waiting out the window would only throw
        // away the wire markers derived during it.
        source_ = MarkerSource::Wire;
        return;
    }
    ExpireIfDue(tUs);
}

void MarkerSourceChoice::NoteMarkFiled() {
    if (source_ != MarkerSource::NotRecorded) {
        locked_ = true;
    }
}

MarkerSource MarkerSourceChoice::Settle() {
    if (source_ == MarkerSource::NotRecorded) {
        source_ = wireRowsSeen_ > 0 ? MarkerSource::Wire : MarkerSource::None;
    }
    locked_ = true;
    return source_;
}

}  // namespace capture
}  // namespace replay
