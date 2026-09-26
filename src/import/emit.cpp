#include "import/emit.h"

#include <windows.h>

#include <math.h>
#include <string.h>

#include <ratio>

#include <string>
#include <vector>

#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/manifestbuild.h"
#include "capture/playerstats.h"
#include "capture/roster.h"
#include "capture/streamcodec.h"
#include "capture/wirebuild.h"
#include "engine/gamestate_facts.h"
#include "engine/entity.h"
#include "engine/gamestate_types.h"
#include "foundation/bitio/enginewire.h"
#include "foundation/bitio/wirefields.h"
#include "foundation/demofile/demofile.h"
#include "foundation/gametype.h"
#include "import/convert.h"
#include "import/hypotheses.h"
#include "import/resample.h"

namespace replay::import {
namespace {

using namespace replay::capture;

// The native wire shape (windowbuild.cpp, apply.cpp MapRecordedOwner): an SR15NetUserId is
// slot | generation << 16, and a recorded owner at or above 0xFFFF reads as "nobody". Native captures
// carry generation 0, so every id written here does too.
constexpr uint32_t kUserIdNone = kBodyOwnerNone;
constexpr uint32_t kHoldNone = UINT16_MAX;  // the hold field's own slot-none and gen-none
constexpr uint64_t kScalarTag = 0x0000021200000000ull;  // SValue tag of an int or real netvar
constexpr uint64_t kDiscIdTag = 1ull << 32;             // SimState id tag 1: a component id
// Native `anchor` names the attached arm: 0 while geo bit L is set, 1 while R is, 2 when free (P1).
constexpr uint32_t kAnchorLeft = 0;
constexpr uint32_t kAnchorRight = 1;
constexpr uint32_t kAnchorFree = 2;
constexpr uint64_t kKeyframeIntervalUs = 2000000;
constexpr int64_t kRefreshUs = 1000000;                 // netvar and S_GAMESTATE periodic copies
constexpr int64_t kStatsRefreshUs = 10 * kRefreshUs;     // periodic scoreboard copy per player

enum HudVar : uint8_t {
    kHudGameTimer,
    kHudGameState,
    kHudBlueScore,
    kHudRedScore,
    kHudBlueRounds,
    kHudRedRounds,
    kHudSuddenDeath,
    kHudMatchOver,
    kHudFrisbeeOwner,
    kHudVarCount,
};

// The HUD's own names (playback/core/netvarhud.cpp), hashed the engine's way rather than restated.
constexpr uint64_t kHudSymbols[kHudVarCount] = {
    gametype::SymbolHash("game_timer"),
    gametype::SymbolHash("game_state"),
    gametype::SymbolHash("blue_score"),
    gametype::SymbolHash("red_score"),
    gametype::SymbolHash("blue_round_score"),
    gametype::SymbolHash("red_round_score"),
    gametype::SymbolHash("sudden_death_active"),
    gametype::SymbolHash("match_is_over"),
    gametype::SymbolHash("game_frisbee_ownerid"),
};

uint32_t UserId(uint16_t slot) { return slot == kNoSlot ? kUserIdNone : slot; }

gamestate::Phase PhaseOf(GameStatus s) {
    switch (s) {
        case GameStatus::PreMatch: return gamestate::Phase::PreMatch;
        case GameStatus::RoundStart: return gamestate::Phase::RoundStart;
        case GameStatus::Playing: return gamestate::Phase::Playing;
        case GameStatus::Score: return gamestate::Phase::Score;
        case GameStatus::RoundOver: return gamestate::Phase::RoundOver;
        case GameStatus::PostMatch: return gamestate::Phase::PostMatch;
        case GameStatus::PreSuddenDeath: return gamestate::Phase::PreSuddenDeath;
        case GameStatus::SuddenDeath: return gamestate::Phase::SuddenDeath;
        case GameStatus::PostSuddenDeath: return gamestate::Phase::PostSuddenDeath;
        case GameStatus::Empty: break;
    }
    return gamestate::Phase::Unknown;
}

// The steady-state ("_update") symbol of each phase, looked up by name in the engine's own table.
uint64_t PhaseSymbol(gamestate::Phase phase) {
    const char* name = nullptr;
    switch (phase) {
        case gamestate::Phase::PreMatch: name = "pre_match_wait_for_ready_update"; break;
        case gamestate::Phase::RoundStart: name = "pre_launch_update"; break;
        case gamestate::Phase::Playing: name = "main_loop_update"; break;
        case gamestate::Phase::Score: name = "point_score_update"; break;
        case gamestate::Phase::RoundOver: name = "round_over_update"; break;
        case gamestate::Phase::PostMatch: name = "post_match_celebration_update"; break;
        case gamestate::Phase::PreSuddenDeath: name = "pre_sudden_death_update"; break;
        case gamestate::Phase::SuddenDeath: name = "sudden_death_new_round_update"; break;
        case gamestate::Phase::PostSuddenDeath: name = "post_sudden_death_update"; break;
        default: return 0;
    }
    for (const gamestate::facts::PhaseFact& f : gamestate::facts::kPhaseFacts) {
        if (strcmp(f.name, name) == 0) return f.symbol;
    }
    return 0;
}

bool InRound(GameStatus s) { return s == GameStatus::RoundStart || s == GameStatus::Playing || s == GameStatus::Score; }

float OnGrid(float v, const bitio::WireField& f, bool* clamped = nullptr) {
    if (!(v >= f.min)) {  // also catches NaN
        if (clamped) *clamped = true;
        v = f.min;
    } else if (v > f.max) {
        if (clamped) *clamped = true;
        v = f.max;
    }
    uint64_t index = 0;
    bitio::QuantReal(v, f.min, f.max, f.step, index);
    return bitio::DequantReal(index, f.min, f.step);
}

bool OnGridVec(const float* in, const bitio::WireField& f, float* out) {
    bool clamped = false;
    for (int a = 0; a < 3; ++a) out[a] = OnGrid(in[a], f, &clamped);
    return clamped;
}

void OnGridQuat(const float* in, const bitio::WireField& f, float* out) {
    float q[4];
    float n = sqrtf(in[0] * in[0] + in[1] * in[1] + in[2] * in[2] + in[3] * in[3]);
    if (!(n > 0.0f)) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = n = 1.0f;
    } else {
        for (int a = 0; a < 4; ++a) q[a] = in[a] / n;
    }
    bitio::Quat10 packed{};
    bitio::QuantQuat(q, f.bits, packed);
    bitio::DequantQuat(packed, f.bits, out);
}

bool ComInRange(const float* p) {
    const bitio::WireField& f = bitio::kSimStateWire[2];
    for (int a = 0; a < 3; ++a) {
        if (!(p[a] >= f.min && p[a] <= f.max)) return false;
    }
    return true;
}

class RecordBuf {
public:
    template <class Head>
    void Feed(ChapterBuilder& b, uint16_t kind, const Head& head, const void* body = nullptr, uint32_t bodyBytes = 0) {
        bytes_.resize(sizeof(RecHeader) + sizeof(Head) + bodyBytes);
        RecHeader h{};
        h.kind = kind;
        h.bytes = static_cast<uint16_t>(sizeof(Head) + bodyBytes);
        memcpy(bytes_.data(), &h, sizeof h);
        memcpy(bytes_.data() + sizeof h, &head, sizeof(Head));
        if (bodyBytes) memcpy(bytes_.data() + sizeof h + sizeof(Head), body, bodyBytes);
        b.Consume(bytes_.data(), static_cast<uint32_t>(bytes_.size()));
    }

private:
    std::vector<uint8_t> bytes_;
};

struct PlayerTracks {
    Track head, lhand, rhand, body;
    float lastHeldCom[3];
    bool haveLastHeld = false;
    uint32_t prevBits = 0;
};

class Emitter {
public:
    Emitter(const SourceMatch& m, const InferredTracks& t, const EmitFacts& f, const ConvertOptions& o,
            ConvertReport& r)
        : match_(m), tracks_(t), facts_(f), options_(o), report_(r) {}

    Result Run(const wchar_t* outPath, Progress* progress);

private:
    const SourceMatch& match_;
    const InferredTracks& tracks_;
    const EmitFacts& facts_;
    const ConvertOptions& options_;
    ConvertReport& report_;

    std::vector<TickFrame> grid_;
    std::vector<PlayerTracks> players_;
    Track disc_;
    uint64_t axisBase_ = 0;
    int64_t firstFrameUs_ = 0;

    ChapterBuilder builder_;
    RecordBuf rb_;
    uint32_t duvSeq_ = 0;
    uint64_t hudValue_[kHudVarCount] = {};
    bool hudHave_[kHudVarCount] = {};
    int64_t hudRefreshUs_ = INT64_MIN;
    gamestate::GameStateSample gsPrev_{};
    bool gsHave_ = false;
    int64_t gsRefreshUs_ = INT64_MIN;
    GameStatus heldStatus_ = GameStatus::Empty;
    uint32_t roundCounter_ = 0;
    bool wasInRound_ = false;
    size_t nextGoal_ = 0, nextStun_ = 0, nextStat_ = 0;
    std::vector<int64_t> statsRefreshUs_;
    std::vector<int32_t> lastStatRow_;

    uint64_t Stamp(int64_t tUs) const { return axisBase_ + static_cast<uint64_t>(tUs - firstFrameUs_); }
    Result Check() const;
    void BuildTracks();
    Result BuildManifest(ManifestBuilder& mb, std::vector<std::string>& docs) const;
    void EmitTick(const TickFrame& tick, uint32_t frameIndex, int64_t prevTUs);
    void EmitPlayer(uint16_t slot, PlayerTracks& pt, const TickFrame& tick, uint64_t stamp);
    void EmitDisc(const TickFrame& tick, uint64_t stamp);
    void EmitMatchState(const TickFrame& tick, uint64_t stamp);
    void EmitNetvar(HudVar var, uint64_t value, uint64_t stamp, bool refresh);
    void EmitRowsUpTo(int64_t tUs, uint64_t stamp);
    void EmitStats(const StatRow& row, uint64_t stamp, bool keyframe);
};

Result Emitter::Check() const {
    if (match_.frames.empty()) return Refuse(ResultCode::TooShort, "the source has no frames to emit");
    if (Result facts = CheckPoseFacts(facts_); !facts.ok) return facts;
    if (tracks_.samples.size() != match_.players.size() || tracks_.discOwner.size() != match_.frames.size()) {
        return Refuse(ResultCode::BadArguments,
                      "inferred tracks (%zu samples, %zu owners) do not match the source (%zu players, %zu frames)",
                      tracks_.samples.size(), tracks_.discOwner.size(), match_.players.size(), match_.frames.size());
    }
    if (match_.mapSymbol == 0) {
        return Refuse(ResultCode::UnsupportedMap, "map '%s' was not resolved by Normalise", match_.mapName);
    }
    if (match_.roster.size() > kRosterMaxRows) {
        return Refuse(ResultCode::Roster, "%zu players exceed the capture roster's %u rows", match_.roster.size(),
                      unsigned(kRosterMaxRows));
    }
    for (const SrcPlayer& p : match_.players) {
        if (p.rosterIndex >= match_.roster.size()) {
            return Refuse(ResultCode::BadArguments, "a player sample names roster row %u of %zu", p.rosterIndex,
                          match_.roster.size());
        }
    }
    return Success();
}

void Emitter::BuildTracks() {
    players_.assign(match_.roster.size(), PlayerTracks{});
    for (uint32_t f = 0; f < match_.frames.size(); ++f) {
        const SrcFrame& frame = match_.frames[f];
        TrackAdd(disc_, f, frame.tUs, frame.disc.pose.pos, frame.disc.pose.q, f);
        for (uint32_t i = 0; i < frame.playerCount; ++i) {
            const uint32_t pi = frame.firstPlayer + i;
            const SrcPlayer& p = match_.players[pi];
            if (p.team == kTeamSpectator) continue;
            PlayerTracks& pt = players_[p.rosterIndex];
            TrackAdd(pt.head, f, frame.tUs, p.head.pos, HmdQuat(p, facts_), pi);
            TrackAdd(pt.lhand, f, frame.tUs, p.lhand.pos, HandQuat(p.lhand, facts_), pi);
            TrackAdd(pt.rhand, f, frame.tUs, p.rhand.pos, HandQuat(p.rhand, facts_), pi);
            TrackAdd(pt.body, f, frame.tUs, p.head.pos, BodyQuat(p, facts_), pi);
        }
    }
    TrackFinish(disc_, kDiscTeleportMps);
    for (PlayerTracks& pt : players_) {
        TrackFinish(pt.head, kPlayerTeleportMps);
        TrackFinish(pt.lhand, kPlayerTeleportMps);
        TrackFinish(pt.rhand, kPlayerTeleportMps);
        TrackFinish(pt.body, kPlayerTeleportMps);
    }
}

Result Emitter::BuildManifest(ManifestBuilder& mb, std::vector<std::string>& docs) const {
    mb.Begin(Stamp(firstFrameUs_), 0, 0);
    const uint8_t md5NotRecorded[16] = {};
    char build[64];
    const char* kind = match_.kind == SourceKind::Tape ? "tape" : "echoreplay";
    _snprintf_s(build, sizeof build, _TRUNCATE, "import:%s:v%u", kind,
                kConverterVersion);
    mb.SetBuild(md5NotRecorded, BuildStringHash64(build), report_.sourceHash);
    const uint64_t startEpochS = static_cast<uint64_t>((match_.epochUtcUs + firstFrameUs_) / 1000000);
    mb.SetLevel(match_.mapSymbol, facts_.gametypeId.value, startEpochS);
    mb.SetKeyframeIntervalUs(kKeyframeIntervalUs);
    mb.SetFlags(static_cast<uint32_t>(kFlagImported));
    // Deterministic, so converting the same source twice gives the same bytes (R13).
    mb.SetUuid(report_.sourceHash, BuildStringHash64(build) ^ uint64_t(options_.rateHz));

    docs.assign(match_.roster.size() * 2, std::string());
    uint8_t teamSlots[3] = {};
    for (uint16_t slot = 0; slot < match_.roster.size(); ++slot) {
        const SrcIdentity& id = match_.roster[slot];
        PlayerRecord p{};
        p.xplatformId = id.userId;
        p.userSlot = slot;
        p.userIndex = slot;
        p.generation = 0;
        const uint8_t team = id.team <= kTeamSpectator ? id.team : kTeamSpectator;
        p.teamId = team;
        p.teamId16 = team;
        p.teamSlot = teamSlots[team];
        p.teamSlot16 = teamSlots[team]++;
        // The colour follows the team; a spectator has none, which the format spells as its sentinel.
        p.alignment = team == kTeamSpectator ? 0xFF : team;
        p.alignment16 = team == kTeamSpectator ? kPlayerNoAlignment16 : team;
        _snprintf_s(p.displayName, sizeof p.displayName, _TRUNCATE, "%s", id.name);
        _snprintf_s(p.displayNameFull, sizeof p.displayNameFull, _TRUNCATE, "%s", id.name);
        // O-3: no default loadout is recoverable, so the loadout is recorded as absent; the dresser then
        // applies name, team and colour and skips jersey numbers rather than dressing a guess.
        for (Symbol64& item : p.loadoutSlots) item = identity_access::kUnrecordedItemId;
        if (id.number != kNotInFrame) {
            p.loadoutNumber = id.number;
            p.loadoutNumberBody = id.number;
            p.fillExt = kPlayerFillNumbers;
        }
        p.firstSeenUs = Stamp(id.firstSeenUs);
        p.lastSeenUs = Stamp(id.lastSeenUs);
        std::string name;
        for (const char* c = id.name; *c; ++c) {
            if (*c == '"' || *c == '\\') name += '\\';
            if (static_cast<unsigned char>(*c) >= 0x20) name += *c;
        }
        std::string& entrant = docs[slot * 2];
        entrant = "{\"displayname\":\"" + name + "\"";
        if (id.level != kNotInFrame) entrant += ",\"level\":" + std::to_string(id.level);
        entrant += "}";
        docs[slot * 2 + 1] = "{}";
        p.entrantJson.p = reinterpret_cast<const uint8_t*>(docs[slot * 2].data());
        p.entrantJson.n = static_cast<uint32_t>(docs[slot * 2].size());
        p.unlocksJson.p = reinterpret_cast<const uint8_t*>(docs[slot * 2 + 1].data());
        p.unlocksJson.n = static_cast<uint32_t>(docs[slot * 2 + 1].size());
        p.fill = kPlayerFillSlot | kPlayerFillName | kPlayerFillXplat | kPlayerFillTeam | kPlayerFillAlign |
                 kPlayerFillGen | kPlayerFillProfile;
        mb.AddPlayer(p);
    }
    if (const char* why = mb.Refusal()) return Refuse(ResultCode::Writer, "manifest refused: %s", why);
    return Success();
}

void Emitter::EmitPlayer(uint16_t slot, PlayerTracks& pt, const TickFrame& tick, uint64_t stamp) {
    const int32_t at = TrackLocate(pt.head, tick);
    if (at < 0) return;
    const int32_t atL = TrackLocate(pt.lhand, tick);
    const int32_t atR = TrackLocate(pt.rhand, tick);
    TrackSample head, lhand, rhand, body;
    TrackEval(pt.head, static_cast<uint32_t>(at), tick, head);
    TrackEval(pt.lhand, static_cast<uint32_t>(atL), tick, lhand);
    TrackEval(pt.rhand, static_cast<uint32_t>(atR), tick, rhand);
    TrackEval(pt.body, static_cast<uint32_t>(at), tick, body);  // same samples as the head track
    const SampleInference& inf = tracks_.samples[pt.head.sample[at]];

    float com[3];
    ComFromHead(head.pos, facts_, com);
    if (!ComInRange(com)) {
        ++report_.comRejected;
        return;
    }

    const bitio::WireField* PW = bitio::kPlayerStateWire;
    PlayerFrame pf{};
    OnGridQuat(head.ori, PW[0], pf.hmdOri);
    OnGridQuat(lhand.ori, PW[1], pf.lHandOri);
    OnGridQuat(rhand.ori, PW[2], pf.rHandOri);
    const uint32_t geoL = kInferFacts.geoBitLeft.value, geoR = kInferFacts.geoBitRight.value;
    const bool attached = (inf.bits28 & (geoL | geoR)) != 0;
    // Native refVel is velocity relative to the attached reference: 0 whenever free (P1). The API
    // carries no reference velocity, so an attached player's own velocity is the nearest reading.
    const float still[3] = {};
    OnGridVec(attached ? head.vel : still, PW[3], pf.refVel);
    memcpy(pf.refVelDup, pf.refVel, sizeof pf.refVel);
    float off[3];
    for (int a = 0; a < 3; ++a) off[a] = lhand.pos[a] - com[a];
    bool clamped = OnGridVec(off, PW[12], pf.lHandOff);
    for (int a = 0; a < 3; ++a) off[a] = rhand.pos[a] - com[a];
    clamped |= OnGridVec(off, PW[13], pf.rHandOff);
    if (clamped) ++report_.handOffsetClamped;
    pf.bendBack = OnGrid(facts_.bendBack.value, PW[9]);
    pf.bendFwd = OnGrid(facts_.bendFwd.value, PW[10]);
    pf.restPoseHeight = OnGrid(facts_.restPoseHeight.value, PW[11]);
    pf.trigL = OnGrid(inf.trigL, PW[14]);
    pf.trigR = OnGrid(inf.trigR, PW[15]);
    pf.bits28 = inf.bits28 & 0x0FFFFFFFu;
    pf.holdLSlot = inf.holdLSlot == kNoSlot ? kHoldNone : inf.holdLSlot;
    pf.holdLGen = inf.holdLSlot == kNoSlot ? kHoldNone : 0;
    pf.holdRSlot = inf.holdRSlot == kNoSlot ? kHoldNone : inf.holdRSlot;
    pf.holdRGen = inf.holdRSlot == kNoSlot ? kHoldNone : 0;
    pf.anchor = (inf.bits28 & geoL) ? kAnchorLeft : (inf.bits28 & geoR) ? kAnchorRight : kAnchorFree;
    pf.hasDup = true;
    pf.fill = kFillWireAll;

    const uint64_t hdr0 = UserId(slot);
    uint8_t body64[kWireBodyBytes];
    if (BuildPlayerBody(body64, sizeof body64, pf, hdr0, stamp)) {
        WireRec wr{};
        wr.stampUs = stamp;
        wr.arrivalUs = stamp;
        wr.dst = hdr0;
        wr.bodyBytes = kWireBodyBytes;
        rb_.Feed(builder_, kRecWirePlayer, wr, body64, kWireBodyBytes);
        ++report_.playerRows;
    }

    // lastHeldCom: com at the latest geo-attach onset (§6.2), the first com until one happens.
    if (!pt.haveLastHeld || ((pf.bits28 & (geoL | geoR)) & ~pt.prevBits) != 0) {
        memcpy(pt.lastHeldCom, com, sizeof com);
        pt.haveLastHeld = true;
    }
    pt.prevBits = pf.bits28;

    const bitio::WireField* SW = bitio::kSimStateWire;
    BodyFrame bf{};
    bf.idValue = hdr0;  // tag 0: an SR15NetUserId
    bf.idTag = 0;
    OnGridVec(com, SW[2], bf.com);
    OnGridQuat(body.ori, SW[3], bf.ori);
    OnGridVec(head.vel, SW[4], bf.vel);
    OnGridVec(body.angVel, SW[5], bf.angVel);
    OnGridVec(pt.lastHeldCom, SW[6], bf.lastHeldCom);
    bf.fill = kFillBodyWireAll;
    if (BuildBodyBody(body64, sizeof body64, bf, hdr0, stamp)) {
        WireRec wr{};
        wr.stampUs = stamp;
        wr.arrivalUs = stamp;
        wr.dst = hdr0;
        wr.bodyBytes = kWireBodyBytes;
        rb_.Feed(builder_, kRecWireBody, wr, body64, kWireBodyBytes);
        ++report_.bodyRows;
    }
}

void Emitter::EmitDisc(const TickFrame& tick, uint64_t stamp) {
    const int32_t at = TrackLocate(disc_, tick);
    if (at < 0) return;
    TrackSample s;
    TrackEval(disc_, static_cast<uint32_t>(at), tick, s);
    if (!ComInRange(s.pos)) {
        ++report_.comRejected;
        return;
    }
    const bitio::WireField* SW = bitio::kSimStateWire;
    BodyFrame bf{};
    bf.idValue = kDiscIdTag | facts_.discIdValue.value;
    bf.idTag = 1;
    OnGridVec(s.pos, SW[2], bf.com);
    OnGridQuat(s.ori, SW[3], bf.ori);
    OnGridVec(s.vel, SW[4], bf.vel);
    OnGridVec(s.angVel, SW[5], bf.angVel);
    memcpy(bf.lastHeldCom, bf.com, sizeof bf.com);
    bf.fill = kFillBodyWireAll;
    const uint64_t hdr0 = UserId(tracks_.discOwner[tick.older]);
    uint8_t body64[kWireBodyBytes];
    if (BuildBodyBody(body64, sizeof body64, bf, hdr0, stamp)) {
        WireRec wr{};
        wr.stampUs = stamp;
        wr.arrivalUs = stamp;
        wr.dst = hdr0;
        wr.bodyBytes = kWireBodyBytes;
        rb_.Feed(builder_, kRecWireBody, wr, body64, kWireBodyBytes);
        ++report_.bodyRows;
    }
}

void Emitter::EmitNetvar(HudVar var, uint64_t value, uint64_t stamp, bool refresh) {
    if (hudHave_[var] && hudValue_[var] == value && !refresh) return;
    hudHave_[var] = true;
    hudValue_[var] = value;
    uint8_t duv[kDuvBodyBytes];
    const uint32_t bytes = BuildDuvBody(duv, sizeof duv, match_.mapSymbol, 0, ++duvSeq_, kHudSymbols[var], value);
    NetvarRec nv{};
    nv.arrivalUs = stamp;
    nv.seq = duvSeq_;
    nv.bodyBytes = static_cast<uint16_t>(bytes);
    rb_.Feed(builder_, kRecNetvar, nv, duv, bytes);
    ++report_.netvarRows;
}

void Emitter::EmitMatchState(const TickFrame& tick, uint64_t stamp) {
    const SrcFrame& f = match_.frames[tick.older];
    if (f.status != GameStatus::Empty) heldStatus_ = f.status;
    const GameStatus status = heldStatus_;
    // The round number the round table is keyed on: rounds already decided, plus the one being played.
    // It moves only when a round starts, so a round won at RoundOver does not open the next one early.
    const bool inRound = InRound(status);
    if (inRound && !wasInRound_) roundCounter_ = uint32_t(f.blueRounds) + f.orangeRounds + 1;
    wasInRound_ = inRound;

    const gamestate::Phase phase = PhaseOf(status);
    const uint64_t phaseSymbol = PhaseSymbol(phase);
    const uint32_t owner = UserId(tracks_.discOwner[tick.older]);
    uint32_t clockBits;
    memcpy(&clockBits, &f.gameClock, sizeof clockBits);

    const bool refresh = int64_t(tick.tUs) - hudRefreshUs_ >= kRefreshUs;
    if (refresh) hudRefreshUs_ = tick.tUs;
    EmitNetvar(kHudGameTimer, kScalarTag | clockBits, stamp, refresh);
    if (phaseSymbol) EmitNetvar(kHudGameState, phaseSymbol, stamp, refresh);
    EmitNetvar(kHudBlueScore, kScalarTag | f.bluePoints, stamp, refresh);
    EmitNetvar(kHudRedScore, kScalarTag | f.orangePoints, stamp, refresh);
    EmitNetvar(kHudBlueRounds, kScalarTag | f.blueRounds, stamp, refresh);
    EmitNetvar(kHudRedRounds, kScalarTag | f.orangeRounds, stamp, refresh);
    // Flags are read as "whole value != 0" by the HUD, so they are bare 0/1, never tagged.
    const bool suddenDeath = status == GameStatus::PreSuddenDeath || status == GameStatus::SuddenDeath ||
                             status == GameStatus::PostSuddenDeath;
    EmitNetvar(kHudSuddenDeath, suddenDeath ? 1 : 0, stamp, refresh);
    EmitNetvar(kHudMatchOver, status == GameStatus::PostMatch ? 1 : 0, stamp, refresh);
    EmitNetvar(kHudFrisbeeOwner, owner, stamp, refresh);

    GameStateRec gs{};
    gs.sourceTimeUs = stamp;
    gamestate::GameStateSample& s = gs.sample;
    s.engineTimeUs = stamp;
    s.captureTimeUs = stamp;
    s.phase = static_cast<uint8_t>(phase);
    s.phaseSymbol = phaseSymbol;
    s.roundCounter = roundCounter_;
    s.bluePoints = f.bluePoints;
    s.redPoints = f.orangePoints;
    s.blueMatchScore = f.blueRounds;
    s.redMatchScore = f.orangeRounds;
    s.frisbeeOwnerId = owner;
    s.gameTimer = f.gameClock;
    s.roundWinningTeam = -1;
    s.present = gamestate::kHasRoundCounter | gamestate::kHasRoundPoints | gamestate::kHasMatchScore |
                gamestate::kHasFrisbeeOwnerId | gamestate::kHasGameTimer;
    if (phaseSymbol) s.present |= gamestate::kHasPhaseSymbol;
    s.availability = static_cast<uint8_t>(gamestate::Availability::Ok);
    const bool changed = !gsHave_ || s.phase != gsPrev_.phase || s.phaseSymbol != gsPrev_.phaseSymbol ||
                         s.roundCounter != gsPrev_.roundCounter || s.bluePoints != gsPrev_.bluePoints ||
                         s.redPoints != gsPrev_.redPoints || s.blueMatchScore != gsPrev_.blueMatchScore ||
                         s.redMatchScore != gsPrev_.redMatchScore;
    if (changed || int64_t(tick.tUs) - gsRefreshUs_ >= kRefreshUs) {
        gsHave_ = true;
        gsPrev_ = s;
        gsRefreshUs_ = tick.tUs;
        rb_.Feed(builder_, kRecGameState, gs);
    }
}

void Emitter::EmitStats(const StatRow& row, uint64_t stamp, bool keyframe) {
    if (row.slot >= match_.roster.size()) return;
    PlayerStatsRec sr{};
    sr.sourceTimeUs = stamp;
    sr.engineTimeUs = stamp;
    sr.xplatformId = match_.roster[row.slot].userId;
    sr.userIndex = row.slot;
    sr.keyframe = keyframe ? 1 : 0;
    sr.v.possessionTime = row.possessionTime;
    sr.v.points = row.stats[kStatPoints];
    sr.v.goals = row.stats[kStatGoals];
    sr.v.saves = row.stats[kStatSaves];
    sr.v.stuns = row.stats[kStatStuns];
    sr.v.passes = row.stats[kStatPasses];
    sr.v.catches = row.stats[kStatCatches];
    sr.v.steals = row.stats[kStatSteals];
    sr.v.blocks = row.stats[kStatBlocks];
    sr.v.interceptions = row.stats[kStatInterceptions];
    sr.v.assists = row.stats[kStatAssists];
    sr.v.shotsTaken = row.stats[kStatShots];
    sr.fill = static_cast<uint16_t>(kPsFillPossessionTime | kPsFillPoints | kPsFillGoals | kPsFillStuns |
                                    kPsFillBlocks |
                                    kPsFillAssists | kPsFillSaves | kPsFillInterceptions | kPsFillSteals |
                                    kPsFillCatches | kPsFillPasses | kPsFillShotsTaken);
    rb_.Feed(builder_, kRecPlayerStats, sr);
    statsRefreshUs_[row.slot] = static_cast<int64_t>(stamp);
}

void Emitter::EmitRowsUpTo(int64_t tUs, uint64_t stamp) {
    for (; nextGoal_ < tracks_.goals.size() && tracks_.goals[nextGoal_].tUs <= tUs; ++nextGoal_) {
        const GoalRow& g = tracks_.goals[nextGoal_];
        uint16_t goalType = facts_.genericGoalType;
        bool known = false;
        for (const GoalTypeName* n = facts_.goalTypes.value; n && n->apiName; ++n) {
            if (_stricmp(n->apiName, g.goalTypeText) == 0) {
                goalType = n->code;
                known = true;
                break;
            }
        }
        if (!known) ++report_.unknownGoalTypes;
        uint8_t body[28] = {};
        const uint16_t team = g.team, points = g.points;
        const uint32_t scorer = UserId(g.scorerSlot), assist = UserId(g.assistSlot);
        const float possession = 0.0f;
        memcpy(body + 0, &team, 2);
        memcpy(body + 2, &points, 2);
        memcpy(body + 4, &goalType, 2);
        memcpy(body + 6, &scorer, 4);
        memcpy(body + 10, &assist, 4);
        memcpy(body + 16, &g.speedMps, 4);
        memcpy(body + 20, &g.distanceM, 4);
        memcpy(body + 24, &possession, 4);
        EventRec ev{};
        ev.stampUs = stamp;
        ev.typeIndex = TapTypeIndex("SR15NetReliablePostScoreUpdate");
        ev.bodyBytes = sizeof body;
        rb_.Feed(builder_, kRecEvent, ev, body, sizeof body);
        ++report_.eventRows;
    }
    for (; nextStun_ < tracks_.stuns.size() && tracks_.stuns[nextStun_].tUs <= tUs; ++nextStun_) {
        const StunRow& s = tracks_.stuns[nextStun_];
        uint8_t body[16] = {};
        const uint32_t victim = UserId(s.victimSlot), puncher = UserId(s.puncherSlot), blocked = 0;
        const float duration = fminf(fmaxf(s.durationS, 1e-3f), 30.0f);
        memcpy(body + 0, &victim, 4);
        memcpy(body + 4, &puncher, 4);
        memcpy(body + 8, &duration, 4);
        memcpy(body + 12, &blocked, 4);
        EventRec ev{};
        ev.stampUs = stamp;
        ev.typeIndex = TapTypeIndex("SR15StunPlayerMsg");
        ev.bodyBytes = sizeof body;
        rb_.Feed(builder_, kRecEvent, ev, body, sizeof body);
        ++report_.eventRows;
    }
    for (; nextStat_ < tracks_.stats.size() && tracks_.stats[nextStat_].tUs <= tUs; ++nextStat_) {
        const StatRow& row = tracks_.stats[nextStat_];
        if (row.slot < lastStatRow_.size()) {
            EmitStats(row, stamp, lastStatRow_[row.slot] < 0);
            lastStatRow_[row.slot] = static_cast<int32_t>(nextStat_);
        }
    }
    // The periodic scoreboard copy a live capture writes, so a seek lands on a full scoreboard.
    for (size_t slot = 0; slot < lastStatRow_.size(); ++slot) {
        if (lastStatRow_[slot] >= 0 && int64_t(stamp) - statsRefreshUs_[slot] >= kStatsRefreshUs) {
            EmitStats(tracks_.stats[lastStatRow_[slot]], stamp, true);
        }
    }
}

void Emitter::EmitTick(const TickFrame& tick, uint32_t frameIndex, int64_t prevTUs) {
    const uint64_t stamp = Stamp(tick.tUs);
    TickRec tr{};
    tr.sourceTimeUs = stamp;
    tr.simTimeUs = stamp;
    tr.peerTimeUs = stamp;
    tr.frameIndex = frameIndex;
    tr.frameDeltaUs = static_cast<uint32_t>(tick.tUs - prevTUs);
    tr.scaledDeltaUs = tr.frameDeltaUs;
    rb_.Feed(builder_, kRecTick, tr);
    for (uint16_t slot = 0; slot < players_.size(); ++slot) EmitPlayer(slot, players_[slot], tick, stamp);
    EmitDisc(tick, stamp);
    EmitMatchState(tick, stamp);
    EmitRowsUpTo(tick.tUs, stamp);
}

Result Emitter::Run(const wchar_t* outPath, Progress* progress) {
    Result r = Check();
    if (!r.ok) return r;
    r = BuildTickGrid(match_, options_.rateHz, grid_);
    if (!r.ok) return r;
    firstFrameUs_ = match_.frames.front().tUs;
    // The capture axis is UTC microseconds, as a live capture's is; a source without an epoch starts at 1 s.
    axisBase_ = match_.epochUtcUs > 0 ? static_cast<uint64_t>(match_.epochUtcUs + firstFrameUs_) : 1000000ull;
    BuildTracks();
    statsRefreshUs_.assign(match_.roster.size(), 0);
    lastStatRow_.assign(match_.roster.size(), -1);

    ManifestBuilder mb;
    std::vector<std::string> docs;
    r = BuildManifest(mb, docs);
    if (!r.ok) return r;

    DemoWriter writer;
    if (!writer.Open(outPath, mb.View())) {
        return Refuse(ResultCode::Writer, "the writer refused to open the output: %s", writer.LastError());
    }
    BuilderConfig cfg;
    cfg.keyframeIntervalUs = kKeyframeIntervalUs;
    for (uint16_t i = 0; i < 32; ++i) cfg.streamEnabled[i] = true;
    for (uint16_t i = 0; i < kSectionCount; ++i) cfg.sectionEnabled[i] = true;
    for (uint16_t off : {kStreamVoip, kStreamAsSeen, kStreamOracle, kStreamPlayerSampled, kStreamBodySampled}) {
        cfg.streamEnabled[off] = false;
    }
    auto abandon = [&](Result why) {
        writer.Close();
        DeleteFileW(outPath);
        return why;
    };
    if (!builder_.Open(&writer, mb.View(), cfg)) {
        return abandon(Refuse(ResultCode::Writer, "the chapter builder refused to open"));
    }
    builder_.SetStatsWriter(&writer);

    WorldOriginRec origin{};
    origin.sourceTimeUs = Stamp(firstFrameUs_);
    origin.valid = 1;
    rb_.Feed(builder_, kRecWorldOrigin, origin);
    builder_.EmitMatchBegin(Stamp(firstFrameUs_));

    const int64_t t0 = grid_.front().tUs;
    int64_t prev = t0;
    for (size_t i = 0; i < grid_.size(); ++i) {
        if (progress) {
            if (progress->cancel.load(std::memory_order_relaxed)) {
                return abandon(Refuse(ResultCode::Cancelled, "conversion cancelled"));
            }
            if ((i & 255) == 0) progress->Report(i, grid_.size());
        }
        const TickFrame& tick = grid_[i];
        const uint32_t frameIndex =
            1 + static_cast<uint32_t>((uint64_t(tick.tUs - t0) * options_.rateHz + 500000) / 1000000);
        EmitTick(tick, frameIndex, i ? prev : tick.tUs - int64_t(1000000 / options_.rateHz));
        prev = tick.tUs;
    }
    const uint64_t endStamp = Stamp(grid_.back().tUs);
    builder_.EmitMatchEnd(endStamp);
    builder_.Close();
    if (!writer.SetRoundTable(builder_.RoundRows(), builder_.RoundRowCount(), builder_.ChosenMarkerSource())) {
        return abandon(Refuse(ResultCode::Writer, "the writer refused the round table: %s", writer.LastError()));
    }
    const BuilderStats& bs = builder_.Stats();
    if (bs.encodeRefusals != 0) {
        return abandon(Refuse(ResultCode::Writer, "the encoder refused %llu record(s)",
                              static_cast<unsigned long long>(bs.encodeRefusals)));
    }
    const WriterStats ws = writer.Stats();
    if (ws.observedBelowStored || ws.unknownSchemaBlocks || ws.oversizeBlocksRefused || ws.blocksOutsideChapter) {
        return abandon(Refuse(ResultCode::Writer,
                              "the writer refused blocks (schema %u, oversize %u, outside %u, counters %u)",
                              ws.unknownSchemaBlocks, ws.oversizeBlocksRefused, ws.blocksOutsideChapter,
                              ws.observedBelowStored));
    }
    writer.Close();
    // Close reports nothing, so the file is proven by opening it: a short write leaves no footer.
    {
        DemoReader check;
        if (!check.Open(outPath) || (check.FileFlags() & kFlagOpen) != 0) {
            DeleteFileW(outPath);
            return Refuse(ResultCode::Io, "the written output does not reopen cleanly (disk full or write error)");
        }
    }

    report_.ticks = static_cast<uint32_t>(grid_.size());
    report_.durationUs = grid_.back().tUs - t0;
    if (progress) progress->Report(1, 1);
    return Success();
}

}  // namespace

Result Emit(const SourceMatch& match, const InferredTracks& tracks, const EmitFacts& facts,
            const ConvertOptions& options, const wchar_t* outPath, ConvertReport& report, Progress* progress) {
    Emitter emitter(match, tracks, facts, options, report);
    return emitter.Run(outPath, progress);
}

}  // namespace replay::import
