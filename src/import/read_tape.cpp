// .tape reader (DESIGN-FORMATS §3.2, §7.3): EchoTools/tape telemetry v2, a zstd stream (independent
// frames plus a skippable seek table) of varint-length-prefixed Envelope messages. The protobuf is decoded
// by hand from the nevr-proto field numbers; per-frame state the tape carries only as events (roster,
// holds, stats, round score) is integrated here so the output matches what the echoreplay reader fills.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <chrono>
#include <memory>
#include <string_view>
#include <vector>

#define ZSTD_STATIC_LINKING_ONLY  // ZSTD_DCtx_setMaxWindowSize
#include <zstd.h>

#include "import/basis.h"
#include "import/pbwire.h"
#include "import/reader.h"
#include "import/readcommon.h"

namespace replay::import {
namespace {

constexpr size_t kInputChunk = size_t(128) << 10;
constexpr size_t kMaxEnvelopeBytes = size_t(64) << 20;
// tape's writer never exceeds an 8 MiB window (tape pkg/codec/seekable.go maxWriterWindow); twice that
// keeps a hostile frame header from sizing our allocation.
constexpr size_t kMaxZstdWindow = size_t(16) << 20;
constexpr uint32_t kProgressEveryEnvelopes = 256;
constexpr int kSlotCount = 256;

const float kIdentityQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};

// GoalType enum -> the API's own spelling, so both readers hand Emit the same text.
std::string_view GoalTypeName(uint32_t value) {
    switch (value) {
    case 1: return "INSIDE SHOT";
    case 2: return "LONG SHOT";
    case 3: return "BOUNCE SHOT";
    case 4: return "LONG BOUNCE SHOT";
    case 5: return "SELF GOAL";
    case 6: return "[NO GOAL]";
    case 7: return "SLAM DUNK";
    case 8: return "HEADBUTT";
    case 9: return "LONG HEADBUTT";
    case 10: return "BUMPER SHOT";
    case 11: return "LONG BUMPER SHOT";
    default: return "";
    }
}

uint8_t TeamFromRole(uint32_t role) {
    switch (role) {
    case 1: return kTeamBlue;
    case 2: return kTeamOrange;
    default: return kTeamSpectator;
    }
}

PauseState PauseFromEnum(uint32_t value) {
    switch (value) {
    case 2:  // PAUSED
    case 4:  // AUTOPAUSE_REPLAY
        return PauseState::Paused;
    case 3: return PauseState::Unpausing;
    default: return PauseState::Unpaused;  // unspecified, not paused, none, paused_requested
    }
}

std::string_view Text(const pb::Field& f) {
    return f.wire == pb::kLen ? std::string_view(reinterpret_cast<const char*>(f.data), f.size) : std::string_view();
}

void ReadVec3(const pb::Field& f, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (f.wire != pb::kLen) return;
    pb::Reader r(f.data, f.size);
    pb::Field g;
    while (r.Next(g)) {
        if (g.number >= 1 && g.number <= 3) out[g.number - 1] = g.Float();
    }
}

// Returns false when the orientation is absent or not a usable rotation.
bool ReadPose(const pb::Field& f, float pos[3], float q[4]) {
    pos[0] = pos[1] = pos[2] = 0.0f;
    q[0] = q[1] = q[2] = q[3] = 0.0f;
    if (f.wire != pb::kLen) return false;
    pb::Reader r(f.data, f.size);
    pb::Field g;
    while (r.Next(g)) {
        if (g.number == 1) {
            ReadVec3(g, pos);
        } else if (g.number == 2 && g.wire == pb::kLen) {
            pb::Reader qr(g.data, g.size);
            pb::Field h;
            while (qr.Next(h)) {
                if (h.number >= 1 && h.number <= 4) q[h.number - 1] = h.Float();
            }
        }
    }
    const float norm = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(norm > 0.5f && norm < 1.5f)) return false;  // also rejects NaN
    for (int i = 0; i < 4; ++i) q[i] /= norm;
    return true;
}

struct SlotState {
    bool known;
    uint64_t account;
    char name[64];
    uint8_t number, level;
    Hold holdL, holdR;
    uint8_t holdLSlot, holdRSlot;
    uint16_t stats[kStatCount];
};

struct PlayerQuats {
    float q[4][4];
    bool valid[4];
};

class TapeParser {
public:
    explicit TapeParser(SourceMatch& out) : out_(out), roster_(out.roster) { ResetSlots(); }

    // Fatal refusals only; a malformed envelope is counted and skipped.
    Result Envelope(const uint8_t* data, size_t size);
    bool sawHeader() const { return sawHeader_; }
    bool sawFooter() const { return sawFooter_; }

private:
    Result Header(const uint8_t* data, size_t size);
    bool Frame(const uint8_t* data, size_t size, Result& fatal);
    bool Event(const pb::Field& event, int64_t tUs);
    bool Player(const pb::Field& f, SrcPlayer& p, Result& fatal);
    void Orientation(bool valid, BasisPart part, float q[4], float prior[4], bool& priorValid);
    void PlayerInfo(const pb::Field& f, bool fromJoin, uint64_t* account, uint8_t* team);
    uint64_t SlotUser(int64_t slot);
    SlotState* Slot(int64_t slot) { return slot >= 0 && slot < kSlotCount ? &slots_[size_t(slot)] : nullptr; }
    uint64_t NamedUser(std::string_view name, int64_t fallbackSlot);
    SrcEvent& PushEvent(int64_t tUs, SrcEventKind kind);
    void ResetSlots();

    SourceMatch& out_;
    RosterBuilder roster_;
    std::array<SlotState, kSlotCount> slots_;
    std::vector<PlayerQuats> playerQuats_;
    float discQuat_[4] = {0, 0, 0, 1};
    bool discQuatValid_ = false;
    uint16_t blueRounds_ = 0, orangeRounds_ = 0;
    uint32_t pendingEventStart_ = 0;  // events not yet sliced into a kept frame
    bool sawHeader_ = false;
    bool sawFooter_ = false;
};

void TapeParser::ResetSlots() {
    for (SlotState& s : slots_) {
        memset(&s, 0, sizeof s);
        s.number = s.level = kNotInFrame;
        s.holdLSlot = s.holdRSlot = kNotInFrame;
    }
}

Result TapeParser::Envelope(const uint8_t* data, size_t size) {
    pb::Reader r(data, size);
    pb::Field f;
    bool parsed = true;
    Result fatal = Success();
    while (r.Next(f)) {
        if (f.wire != pb::kLen) continue;
        if (f.number == 1) {
            if (sawHeader_) {
                parsed = false;  // a second header is not part of the stream contract
                continue;
            }
            Result header = Header(f.data, f.size);
            if (!header.ok) return header;
        } else if (!sawHeader_) {
            return Refuse(ResultCode::Container, "the tape does not begin with a capture header");
        } else if (f.number == 2) {
            parsed &= Frame(f.data, f.size, fatal);
            if (!fatal.ok) return fatal;
        } else if (f.number == 3) {
            sawFooter_ = true;
        }
    }
    if (!r.ok() || !parsed) ++out_.stats.badLines;
    if (!sawHeader_) return Refuse(ResultCode::Container, "the tape does not begin with a capture header");
    return Success();
}

void TapeParser::PlayerInfo(const pb::Field& info, bool fromJoin, uint64_t* accountOut, uint8_t* teamOut) {
    int64_t slot = 0;  // proto3 omits a zero slot
    uint64_t account = 0;
    std::string_view name;
    uint32_t role = 0;
    int32_t jersey = -1, level = -1;
    pb::Reader r(info.data, info.size);
    pb::Field g;
    while (r.Next(g)) {
        switch (g.number) {
        case 1: slot = g.Int32(); break;
        case 2: account = g.scalar; break;
        case 3: name = Text(g); break;
        case 4: role = g.Uint32(); break;
        case 5: jersey = g.Int32(); break;
        case 6: level = g.Int32(); break;
        default: break;
        }
    }
    if (accountOut) *accountOut = account;
    if (teamOut) *teamOut = TeamFromRole(role);
    if (slot < 0 || slot >= kSlotCount) {
        ++out_.stats.unresolvedPlayerRefs;
        return;
    }
    SlotState& s = slots_[size_t(slot)];
    if (fromJoin || !s.known || s.account != account) {
        memset(&s, 0, sizeof s);
        s.holdLSlot = s.holdRSlot = kNotInFrame;
    }
    s.known = true;
    s.account = account;
    CopyText(s.name, sizeof s.name, name);
    s.number = jersey >= 0 && jersey < kNotInFrame ? uint8_t(jersey) : kNotInFrame;
    s.level = level >= 0 && level < kNotInFrame ? uint8_t(level) : kNotInFrame;
}

Result TapeParser::Header(const uint8_t* data, size_t size) {
    pb::Reader r(data, size);
    pb::Field f;
    uint32_t version = 0;
    while (r.Next(f)) {
        switch (f.number) {
        case 2: {  // google.protobuf.Timestamp created_at
            int64_t seconds = 0;
            int32_t nanos = 0;
            pb::Reader t(f.data, f.size);
            pb::Field g;
            while (t.Next(g)) {
                if (g.number == 1) seconds = g.Int64();
                if (g.number == 2) nanos = g.Int32();
            }
            out_.epochUtcUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::seconds(seconds) + std::chrono::nanoseconds(nanos))
                                  .count();
            break;
        }
        case 3: version = f.Uint32(); break;
        case 4: {  // metadata map<string, string>
            std::string_view key, value;
            pb::Reader m(f.data, f.size);
            pb::Field g;
            while (m.Next(g)) {
                if (g.number == 1) key = Text(g);
                if (g.number == 2) value = Text(g);
            }
            if (key == "private_match") out_.privateMatch = value == "true" || value == "1";
            break;
        }
        case 10: {
            pb::Reader a(f.data, f.size);
            pb::Field g;
            while (a.Next(g)) {
                if (g.number == 1) CopyText(out_.sessions.emplace_back().id, sizeof(SrcSession::id), Text(g));
                if (g.number == 2) CopyText(out_.mapName, sizeof out_.mapName, Text(g));
                if (g.number == 8 && g.wire == pb::kLen) PlayerInfo(g, false, nullptr, nullptr);
            }
            if (!a.ok()) return Refuse(ResultCode::Container, "the tape's arena header is malformed");
            break;
        }
        case 101: out_.nominalHz = f.Uint32(); break;
        default: break;
        }
    }
    if (!r.ok()) return Refuse(ResultCode::Container, "the tape's capture header is malformed");
    // tape's own reader treats 0 (captures before 2.1 wrote none) as 2.
    if (version != 0 && version != 2) {
        return Refuse(ResultCode::TapeVersion, "tape format_version %u is not supported (only 2)", version);
    }
    if (out_.sessions.empty()) out_.sessions.emplace_back() = SrcSession{};
    sawHeader_ = true;
    return Success();
}

uint64_t TapeParser::SlotUser(int64_t slot) {
    if (const SlotState* s = Slot(slot); s && s->known) return s->account;
    ++out_.stats.unresolvedPlayerRefs;
    return kNoUser;
}

// The echoreplay reader resolves goal credits by name; do the same here, falling back to the slot.
uint64_t TapeParser::NamedUser(std::string_view name, int64_t fallbackSlot) {
    if (name.empty() || name == "[INVALID]") return kNoUser;
    for (const SlotState& s : slots_) {
        if (s.known && name == s.name) return s.account;
    }
    return SlotUser(fallbackSlot);
}

SrcEvent& TapeParser::PushEvent(int64_t tUs, SrcEventKind kind) {
    SrcEvent& e = out_.events.emplace_back();
    memset(&e, 0, sizeof e);
    e.tUs = tUs;
    e.kind = kind;
    return e;
}

// The last int32 field `number` of a sub-message; proto3 omits zeros, so absent reads as 0.
int64_t IntField(const pb::Field& msg, uint32_t number) {
    pb::Reader r(msg.data, msg.size);
    pb::Field g;
    int64_t value = 0;
    while (r.Next(g)) {
        if (g.number == number && g.wire == pb::kVarint) value = g.Int32();
    }
    return value;
}

uint32_t NonNegative(int64_t v) { return v < 0 ? 0 : uint32_t(v > INT32_MAX ? INT32_MAX : v); }

uint16_t Clamp16(uint32_t v) { return v > UINT16_MAX ? UINT16_MAX : uint16_t(v); }

bool TapeParser::Event(const pb::Field& event, int64_t tUs) {
    pb::Reader r(event.data, event.size);
    pb::Field e;
    while (r.Next(e)) {
        if (e.wire != pb::kLen) continue;
        switch (e.number) {
        case 10: PushEvent(tUs, SrcEventKind::RoundStarted).c = NonNegative(IntField(e, 1)); break;
        case 13: PushEvent(tUs, SrcEventKind::RoundEnded).c = NonNegative(IntField(e, 1)); break;
        case 14: PushEvent(tUs, SrcEventKind::MatchEnded); break;
        case 15: {  // ScoreboardUpdated: the only per-frame source of the round score
            const int64_t blue = IntField(e, 3), orange = IntField(e, 4);
            blueRounds_ = Clamp16(NonNegative(blue));
            orangeRounds_ = Clamp16(NonNegative(orange));
            break;
        }
        case 20: {
            uint64_t account;
            uint8_t team;
            PlayerInfo(e, true, &account, &team);
            SrcEvent& j = PushEvent(tUs, SrcEventKind::PlayerJoined);
            j.a = account;
            j.c = team;
            break;
        }
        case 21: {
            const int64_t slot = IntField(e, 1);
            PushEvent(tUs, SrcEventKind::PlayerLeft).a = SlotUser(slot);
            if (SlotState* s = Slot(slot)) {
                memset(s->stats, 0, sizeof s->stats);
                s->holdL = s->holdR = Hold::None;
                s->holdLSlot = s->holdRSlot = kNotInFrame;
            }
            break;
        }
        case 22: {
            SrcEvent& s = PushEvent(tUs, SrcEventKind::PlayerSwitchedTeam);
            s.a = SlotUser(IntField(e, 1));
            s.c = TeamFromRole(NonNegative(IntField(e, 2)));
            break;
        }
        case 25: {  // GrabChanged: holds exist only as edges in a tape
            SlotState* s = Slot(IntField(e, 1));
            if (!s) break;
            std::string_view left = "none", right = "none";
            pb::Reader g(e.data, e.size);
            pb::Field h;
            while (g.Next(h)) {
                if (h.number == 2) left = Text(h);
                if (h.number == 3) right = Text(h);
            }
            ParseHold(left, s->holdL, s->holdLSlot);
            ParseHold(right, s->holdR, s->holdRSlot);
            break;
        }
        case 26: {  // PlayerInfoUpdated
            SlotState* s = Slot(IntField(e, 1));
            if (!s) break;
            const int64_t jersey = IntField(e, 2), level = IntField(e, 3);
            s->number = jersey >= 0 && jersey < kNotInFrame ? uint8_t(jersey) : kNotInFrame;
            s->level = level >= 0 && level < kNotInFrame ? uint8_t(level) : kNotInFrame;
            break;
        }
        case 31: PushEvent(tUs, SrcEventKind::DiscThrown).a = SlotUser(IntField(e, 1)); break;
        case 32: PushEvent(tUs, SrcEventKind::DiscCaught).a = SlotUser(IntField(e, 1)); break;
        case 40: {
            float speed = 0, distance = 0;
            uint32_t role = 0, goalType = 0, points = 0;
            int64_t scorerSlot = 0, assistSlot = -1;
            std::string_view scorer, assist;
            pb::Reader g(e.data, e.size);
            pb::Field h;
            while (g.Next(h)) {
                switch (h.number) {
                case 1: speed = h.Float(); break;
                case 2: role = h.Uint32(); break;
                case 3: goalType = h.Uint32(); break;
                case 4: points = h.Uint32(); break;
                case 5: distance = h.Float(); break;
                case 6: scorerSlot = h.Int32(); break;
                case 7: assistSlot = h.Int32(); break;
                case 8: scorer = Text(h); break;
                case 9: assist = Text(h); break;
                default: break;
                }
            }
            SrcEvent& goal = PushEvent(tUs, SrcEventKind::GoalScored);
            goal.a = NamedUser(scorer, scorerSlot);
            goal.b = NamedUser(assist, assistSlot);
            goal.c = uint32_t(role == 1 ? kTeamBlue : role == 2 ? kTeamOrange : kNotInFrame) | (points & 0xFF) << 8;
            goal.f0 = speed;
            goal.f1 = distance;
            CopyText(goal.text, sizeof goal.text, GoalTypeName(goalType));
            break;
        }
        case 51: PushEvent(tUs, SrcEventKind::StunCredited).a = SlotUser(IntField(e, 1)); break;
        case 58: {  // PlayerStatsUpdated: absolute counters, carried forward
            SlotState* s = Slot(IntField(e, 1));
            if (!s) break;
            // Wire field number -> StatIndex; note saves (3) precedes goals (4) on the wire.
            static constexpr int8_t kWireToStat[13] = {-1,          -1,          kStatPoints, kStatSaves,
                                                       kStatGoals,  kStatStuns,  kStatPasses, kStatCatches,
                                                       kStatSteals, kStatBlocks, kStatInterceptions,
                                                       kStatAssists, kStatShots};
            memset(s->stats, 0, sizeof s->stats);
            pb::Reader g(e.data, e.size);
            pb::Field h;
            while (g.Next(h)) {
                if (h.number < 13 && kWireToStat[h.number] >= 0) {
                    const int32_t v = h.Int32();
                    s->stats[kWireToStat[h.number]] = Clamp16(NonNegative(v));
                }
            }
            break;
        }
        default: break;
        }
    }
    return r.ok();
}

void TapeParser::Orientation(bool valid, BasisPart part, float q[4], float prior[4], bool& priorValid) {
    if (valid) {
        if (priorValid) AlignHemisphere(prior, q);
        memcpy(prior, q, 4 * sizeof(float));
        priorValid = true;
        return;
    }
    SourceStats& stats = out_.stats;
    if (stats.basisTrips[part]++ == 0) stats.basisFirstTripFrame[part] = uint32_t(out_.frames.size() - 1);
    memcpy(q, priorValid ? prior : kIdentityQuat, 4 * sizeof(float));
}

bool TapeParser::Player(const pb::Field& f, SrcPlayer& p, Result& fatal) {
    memset(&p, 0, sizeof p);
    pb::Field parts[4] = {};
    pb::Field velocity = {};
    int64_t slot = 0;
    uint32_t flags = 0;
    pb::Reader r(f.data, f.size);
    pb::Field g;
    while (r.Next(g)) {
        switch (g.number) {
        case 1: slot = g.Int32(); break;
        case 2: case 3: case 4: case 5: parts[g.number - 2] = g; break;
        case 6: velocity = g; break;
        case 7: flags = g.Uint32(); break;
        case 10: p.possessionTime = g.Float(); break;
        default: break;
        }
    }
    if (!r.ok() || slot < 0 || slot >= kSlotCount - 1) return false;

    const SlotState& s = slots_[size_t(slot)];
    const uint8_t team = uint8_t((flags >> 5) & 3) > kTeamSpectator ? kTeamSpectator : uint8_t((flags >> 5) & 3);
    char anonymous[16];
    std::string_view name = s.name;
    if (!s.known) {
        ++out_.stats.unresolvedPlayerRefs;
        snprintf(anonymous, sizeof anonymous, "slot %d", int(slot));
        name = anonymous;
    }
    p.userId = s.known ? s.account : kNoUser;
    p.team = team;
    p.apiSlot = uint8_t(slot);
    p.rosterIndex = roster_.Intern(p.userId, name, s.number, s.level, team);
    if (p.rosterIndex == kNoSlot) {
        fatal = Refuse(ResultCode::Roster, "more than %u distinct players in one file", unsigned(kNoSlot));
        return false;
    }
    if (playerQuats_.size() <= p.rosterIndex) playerQuats_.resize(size_t(p.rosterIndex) + 1, PlayerQuats{});
    PlayerQuats& quats = playerQuats_[p.rosterIndex];
    Pose* poses[4] = {&p.head, &p.body, &p.lhand, &p.rhand};
    for (int i = 0; i < 4; ++i) {
        const bool valid = ReadPose(parts[i], poses[i]->pos, poses[i]->q);
        Orientation(valid, BasisPart(i), poses[i]->q, quats.q[i], quats.valid[i]);
    }
    ReadVec3(velocity, p.vel);
    p.stunned = flags & 1;
    p.invulnerable = flags & 2;
    p.blocking = flags & 4;
    p.possession = flags & 8;
    p.emote = flags & 16;
    p.holdL = s.holdL;
    p.holdR = s.holdR;
    p.holdLSlot = s.holdLSlot;
    p.holdRSlot = s.holdRSlot;
    memcpy(p.stats, s.stats, sizeof p.stats);
    p.number = kNotInFrame;
    p.level = kNotInFrame;
    return true;
}

bool TapeParser::Frame(const uint8_t* data, size_t size, Result& fatal) {
    int64_t tUs = 0;
    pb::Field arena = {};
    bool haveArena = false;
    pb::Reader r(data, size);
    pb::Field f;
    while (r.Next(f)) {
        if (f.number == 2) tUs = std::chrono::microseconds(std::chrono::milliseconds(f.Uint32())).count();
        if (f.number == 10 && f.wire == pb::kLen) {
            arena = f;
            haveArena = true;
        }
    }
    if (!r.ok() || !haveArena) return false;

    // Events first: a GrabChanged or PlayerJoined in this frame describes this frame's state.
    bool ok = true;
    int32_t errCode = 0;
    pb::Reader events(arena.data, arena.size);
    while (events.Next(f)) {
        if (f.number == 12 && f.wire == pb::kLen) ok &= Event(f, tUs);
        if (f.number == 18) errCode = f.Int32();
    }
    if (!events.ok()) return false;
    if (errCode != 0) {
        ++out_.stats.errCodeFrames;
        return ok;  // its events stay pending and are sliced into the next kept frame
    }

    if (out_.players.size() > UINT32_MAX - kSlotCount) {
        fatal = Refuse(ResultCode::Corrupt, "more player samples than the model can index");
        return false;
    }
    SrcFrame& frame = out_.frames.emplace_back();
    memset(&frame, 0, sizeof frame);
    frame.tUs = tUs;
    frame.blueRounds = blueRounds_;
    frame.orangeRounds = orangeRounds_;
    frame.disc.holderApiSlot = kNotInFrame;
    frame.firstPlayer = uint32_t(out_.players.size());
    bool discSeen = false;

    pb::Reader fields(arena.data, arena.size);
    while (fields.Next(f)) {
        switch (f.number) {
        case 1: frame.status = f.Uint32() <= uint32_t(GameStatus::PostSuddenDeath) ? GameStatus(f.Uint32()) : GameStatus::Empty; break;
        case 2: frame.gameClock = f.Float(); break;
        case 3: frame.pause = PauseFromEnum(f.Uint32()); break;
        case 4: {
            discSeen = true;
            pb::Reader d(f.data, f.size);
            pb::Field g;
            bool poseValid = false;
            while (d.Next(g)) {
                if (g.number == 1) poseValid = ReadPose(g, frame.disc.pose.pos, frame.disc.pose.q);
                if (g.number == 2) ReadVec3(g, frame.disc.vel);
                if (g.number == 3) frame.disc.bounceCount = Clamp16(g.Uint32());
            }
            ok &= d.ok();
            Orientation(poseValid, kPartDisc, frame.disc.pose.q, discQuat_, discQuatValid_);
            break;
        }
        case 5: {
            SrcPlayer& p = out_.players.emplace_back();
            if (!Player(f, p, fatal)) {
                out_.players.pop_back();
                if (!fatal.ok) return false;
                ok = false;
            }
            break;
        }
        case 7: {
            const int32_t holder = f.Int32();
            frame.disc.holderApiSlot = holder >= 0 && holder < kNotInFrame ? uint8_t(holder) : kNotInFrame;
            break;
        }
        case 9: frame.bluePoints = Clamp16(f.Uint32()); break;
        case 10: frame.orangePoints = Clamp16(f.Uint32()); break;
        default: break;
        }
    }
    if (!discSeen) Orientation(false, kPartDisc, frame.disc.pose.q, discQuat_, discQuatValid_);
    frame.playerCount = uint32_t(out_.players.size() - frame.firstPlayer);
    frame.firstEvent = pendingEventStart_;
    frame.eventCount = uint32_t(out_.events.size()) - pendingEventStart_;
    pendingEventStart_ = uint32_t(out_.events.size());
    return ok && fields.ok();
}

bool IsZstdMagic(const uint8_t* p, size_t n) {
    if (n < 4) return false;
    const uint32_t magic = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    return magic == 0xFD2FB528u || (magic & 0xFFFFFFF0u) == 0x184D2A50u;
}

// Decoded bytes waiting to be cut into envelopes.
class EnvelopeBuffer {
public:
    uint8_t* Reserve(size_t n) {
        if (start_ > 0 && start_ >= bytes_.size() / 2) {
            memmove(bytes_.data(), bytes_.data() + start_, end_ - start_);
            end_ -= start_;
            start_ = 0;
        }
        if (bytes_.size() < end_ + n) bytes_.resize((end_ + n) * 2);
        return bytes_.data() + end_;
    }
    void Commit(size_t n) { end_ += n; }
    size_t pending() const { return end_ - start_; }

    // Hands each whole envelope to `sink`; stops at a partial one. False on an oversized length.
    template <class Sink>
    bool Drain(Sink&& sink, bool& fatal) {
        fatal = false;
        for (;;) {
            const uint8_t* p = bytes_.data() + start_;
            const uint8_t* end = bytes_.data() + end_;
            uint64_t length;
            if (!pb::ReadVarint(p, end, length)) {
                if (end - p >= 10) return false;  // a varint never needs more than 10 bytes
                return true;
            }
            if (length > kMaxEnvelopeBytes) return false;
            if (uint64_t(end - p) < length) return true;
            if (!sink(p, size_t(length))) {
                fatal = true;
                return true;
            }
            start_ = size_t(p + length - bytes_.data());
        }
    }

private:
    std::vector<uint8_t> bytes_;
    size_t start_ = 0;
    size_t end_ = 0;
};

}  // namespace

Result ReadTape(const wchar_t* path, SourceMatch& out, Progress* progress, uint64_t maxModelBytes) {
    out = SourceMatch{};
    out.kind = SourceKind::Tape;
    SourceStream stream;
    Result opened = stream.Open(path, out.stats);
    if (!opened.ok) return opened;

    TapeParser parser(out);
    EnvelopeBuffer envelopes;
    Result fatal = Success();
    uint32_t envelopesAtLastPoll = 0;
    const auto sink = [&](const uint8_t* data, size_t size) {
        ++out.stats.lines;
        fatal = parser.Envelope(data, size);
        return fatal.ok;
    };
    const auto drain = [&]() -> bool {
        bool stop;
        if (!envelopes.Drain(sink, stop)) {
            fatal = Refuse(ResultCode::Corrupt, "envelope %u has an invalid length prefix or one over %zu MB",
                           out.stats.lines + 1, kMaxEnvelopeBytes >> 20);
            return false;
        }
        if (stop) return false;
        if (out.stats.lines - envelopesAtLastPoll >= kProgressEveryEnvelopes) {
            envelopesAtLastPoll = out.stats.lines;
            if (ProgressCancelled(progress)) {
                fatal = Refuse(ResultCode::Cancelled, "cancelled while reading");
                return false;
            }
            fatal = CheckModelBudget(out, maxModelBytes);
            if (!fatal.ok) return false;
            ProgressUpdate(progress, stream.consumed(), stream.fileSize());
        }
        return true;
    };

    std::vector<uint8_t> input(kInputChunk);
    size_t inputSize = stream.Read(input.data(), input.size());
    if (stream.failed()) return stream.error();
    const bool compressed = IsZstdMagic(input.data(), inputSize);

    bool midFrame = false;  // zstd: the stream ended inside a frame
    if (compressed) {
        struct DStreamDeleter {
            void operator()(ZSTD_DStream* d) const { ZSTD_freeDStream(d); }
        };
        std::unique_ptr<ZSTD_DStream, DStreamDeleter> dstream(ZSTD_createDStream());
        if (!dstream || ZSTD_isError(ZSTD_initDStream(dstream.get())) ||
            ZSTD_isError(ZSTD_DCtx_setMaxWindowSize(dstream.get(), kMaxZstdWindow))) {
            return Refuse(ResultCode::Io, "cannot create a zstd decoder");
        }
        const size_t outChunk = ZSTD_DStreamOutSize();
        uint64_t compressedOffset = 0;
        while (inputSize > 0) {
            ZSTD_inBuffer in = {input.data(), inputSize, 0};
            while (in.pos < in.size) {
                ZSTD_outBuffer outBuf = {envelopes.Reserve(outChunk), outChunk, 0};
                const size_t hint = ZSTD_decompressStream(dstream.get(), &outBuf, &in);
                if (ZSTD_isError(hint)) {
                    return Refuse(ResultCode::Container, "the tape's zstd stream is damaged (%s) near compressed byte %llu",
                                  ZSTD_getErrorName(hint),
                                  static_cast<unsigned long long>(compressedOffset + in.pos));
                }
                midFrame = hint != 0;
                envelopes.Commit(outBuf.pos);
                if (!drain()) return fatal;
            }
            compressedOffset += inputSize;
            inputSize = stream.Read(input.data(), input.size());
        }
        // Flush whatever the decoder still holds for a frame whose input is complete.
        for (;;) {
            ZSTD_inBuffer in = {nullptr, 0, 0};
            ZSTD_outBuffer outBuf = {envelopes.Reserve(outChunk), outChunk, 0};
            const size_t hint = ZSTD_decompressStream(dstream.get(), &outBuf, &in);
            if (ZSTD_isError(hint) || outBuf.pos == 0) break;
            midFrame = hint != 0;
            envelopes.Commit(outBuf.pos);
            if (!drain()) return fatal;
        }
    } else {
        while (inputSize > 0) {
            memcpy(envelopes.Reserve(inputSize), input.data(), inputSize);
            envelopes.Commit(inputSize);
            if (!drain()) return fatal;
            inputSize = stream.Read(input.data(), input.size());
        }
    }
    if (stream.failed()) return stream.error();
    if (!parser.sawHeader()) return Refuse(ResultCode::Container, "no capture header: not a tape, or empty");

    // A crash-cut capture is still a replay: keep every whole frame and say the tail is missing.
    out.stats.truncatedSource = midFrame || envelopes.pending() > 0 || !parser.sawFooter();
    if (TooManyBadLines(out.stats.badLines, out.stats.lines)) {
        return Refuse(ResultCode::Corrupt, "%u of %u envelopes are malformed (over 1%%)", out.stats.badLines,
                      out.stats.lines);
    }
    if (out.frames.empty()) return Refuse(ResultCode::Corrupt, "the tape holds no readable frames");
    ProgressUpdate(progress, 1, 1);
    return Success();
}

}  // namespace replay::import
