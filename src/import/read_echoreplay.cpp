// .echoreplay reader (DESIGN-FORMATS §3.1, §7.2): Spark / EchoVRAPI recordings, one line per API poll:
// `time \t session-JSON [\t bones-JSON]`. Streamed through a rolling line buffer; the inflated text is
// never whole, the bones column is never parsed, and duplicate lines are dropped before any JSON work.

#include <string.h>

#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <yyjson.h>

#include "import/basis.h"
#include "import/reader.h"
#include "import/readcommon.h"

namespace replay::import {
namespace {

constexpr size_t kChunkBytes = size_t(1) << 20;
// A real line is ~11.6 KB. The JSON pool is sized from the line, so this also bounds the pool.
constexpr size_t kMaxLineBytes = size_t(1) << 20;
constexpr uint32_t kProgressEveryLines = 256;
constexpr size_t kReserveFramesCap = 40000;
constexpr size_t kTypicalLineBytes = 11000;  // measured ~11.6 KB of session JSON per line (§8)

const float kIdentityQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};

bool Digits(std::string_view s, size_t at, size_t count, int& value) {
    value = 0;
    for (size_t i = at; i < at + count; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (s[i] - '0');
    }
    return true;
}

// `yyyy/MM/dd HH:mm:ss.fff` (UTC). EchoVRAPI also writes `HH.mm.ss.fff`, so ':' and '.' are both
// accepted at 13 and 16; the fraction may carry 1-7 digits.
bool ParseStamp(std::string_view s, int64_t& utcUs) {
    if (s.size() < 19) return false;
    const auto isDateSep = [](char c) { return c == '/' || c == '-'; };
    const auto isTimeSep = [](char c) { return c == ':' || c == '.'; };
    if (!isDateSep(s[4]) || !isDateSep(s[7]) || (s[10] != ' ' && s[10] != 'T') || !isTimeSep(s[13]) ||
        !isTimeSep(s[16])) {
        return false;
    }
    int year, month, day, hour, minute, second;
    if (!Digits(s, 0, 4, year) || !Digits(s, 5, 2, month) || !Digits(s, 8, 2, day) || !Digits(s, 11, 2, hour) ||
        !Digits(s, 14, 2, minute) || !Digits(s, 17, 2, second)) {
        return false;
    }
    const std::chrono::year_month_day date{std::chrono::year(year), std::chrono::month(unsigned(month)),
                                           std::chrono::day(unsigned(day))};
    if (!date.ok() || hour > 23 || minute > 59 || second > 60) return false;
    int64_t fractionUs = 0;
    if (s.size() > 19) {
        if (s[19] != '.' || s.size() == 20 || s.size() > 27) return false;
        int64_t scale = 100000;
        for (size_t i = 20; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            fractionUs += (s[i] - '0') * scale;
            scale /= 10;
        }
    }
    using namespace std::chrono;
    const auto stamp = sys_days(date) + hours(hour) + minutes(minute) + seconds(second);
    utcUs = duration_cast<microseconds>(stamp.time_since_epoch()).count() + fractionUs;
    return true;
}

std::string_view Str(yyjson_val* v) {
    return yyjson_is_str(v) ? std::string_view(yyjson_get_str(v), yyjson_get_len(v)) : std::string_view();
}

double Num(yyjson_val* v, double fallback = 0.0) { return yyjson_is_num(v) ? yyjson_get_num(v) : fallback; }

float Float(yyjson_val* obj, const char* key) { return float(Num(yyjson_obj_get(obj, key))); }

uint64_t U64(yyjson_val* v) {
    if (yyjson_is_uint(v)) return yyjson_get_uint(v);
    if (yyjson_is_sint(v)) return yyjson_get_sint(v) < 0 ? 0 : uint64_t(yyjson_get_sint(v));
    if (yyjson_is_real(v) && yyjson_get_real(v) >= 0.0 && yyjson_get_real(v) < 1.8e19) {
        return uint64_t(yyjson_get_real(v));
    }
    return 0;
}

uint16_t U16(yyjson_val* v) {
    const double d = Num(v);
    return d <= 0.0 ? 0 : d >= double(UINT16_MAX) ? UINT16_MAX : uint16_t(d);
}

uint8_t ByteOr(yyjson_val* v, uint8_t fallback) {
    if (!yyjson_is_num(v)) return fallback;
    const double d = yyjson_get_num(v);
    return d < 0.0 ? 0 : d >= 254.0 ? 254 : uint8_t(d);
}

bool Bool(yyjson_val* obj, const char* key) { return yyjson_get_bool(yyjson_obj_get(obj, key)); }

bool Vec3(yyjson_val* arr, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!yyjson_is_arr(arr) || yyjson_arr_size(arr) < 3) return false;
    yyjson_arr_iter it;
    yyjson_arr_iter_init(arr, &it);
    for (int i = 0; i < 3; ++i) {
        yyjson_val* e = yyjson_arr_iter_next(&it);
        if (!yyjson_is_num(e)) return false;
        out[i] = float(yyjson_get_num(e));
    }
    return true;
}

// Older writers spell a hand's position `position`, newer ones `pos`; take whichever is present.
yyjson_val* Position(yyjson_val* part) {
    yyjson_val* v = yyjson_obj_get(part, "pos");
    return v ? v : yyjson_obj_get(part, "position");
}

struct LastScore {
    float discSpeed, distance;
    int team, points;
    std::string goalType, scorer, assist;

    bool operator==(const LastScore&) const = default;
};

int TeamFromName(std::string_view s) {
    if (s == "blue" || s == "BLUE TEAM") return kTeamBlue;
    if (s == "orange" || s == "ORANGE TEAM") return kTeamOrange;
    if (s == "SPECTATORS" || s == "spectator") return kTeamSpectator;
    return -1;
}

enum class LineOutcome { Kept, Skipped, Bad };

class EchoReplayParser {
public:
    explicit EchoReplayParser(SourceMatch& out) : out_(out), roster_(out.roster) {}

    // `line` excludes the newline. False only for a fatal refusal (written to `fatal`).
    bool Line(char* line, size_t length, bool terminated, Result& fatal);

    uint32_t unterminatedBad = 0;

private:
    LineOutcome Frame(yyjson_val* root, int64_t stampUs, Result& fatal);
    void Hold(int64_t stampUs);
    bool Player(yyjson_val* obj, uint8_t team, SrcPlayer& p);
    void Basis(yyjson_val* part, BasisPart which, float q[4], float prior[4], bool& priorValid);
    void GoalSnapshot(yyjson_val* root, const SrcFrame& frame);
    uint64_t ResolveName(std::string_view name, const SrcFrame& frame);
    uint16_t Session(std::string_view id);

    struct PlayerQuats {
        float q[4][4];
        bool valid[4];
    };

    SourceMatch& out_;
    RosterBuilder roster_;
    std::vector<PlayerQuats> playerQuats_;
    float discQuat_[4] = {0, 0, 0, 1};
    bool discQuatValid_ = false;
    std::unordered_map<std::string, uint32_t> sessionIndex_;
    std::string sessionOfLastFrame_;
    uint32_t sessionIndexOfLastFrame_ = 0;
    std::string previousJson_;
    bool previousKept_ = false;  // the line previousJson_ holds became a frame (not err_code, not bad)
    std::vector<uint8_t> jsonPool_;
    LastScore lastScore_{};
    bool haveLastScore_ = false;
    bool haveEpoch_ = false;
};

bool EchoReplayParser::Line(char* line, size_t length, bool terminated, Result& fatal) {
    if (length > 0 && line[length - 1] == '\r') --length;
    if (length == 0) return true;
    SourceStats& stats = out_.stats;
    ++stats.lines;

    const auto bad = [&] {
        ++stats.badLines;
        if (!terminated) ++unterminatedBad;
        return true;
    };

    char* tab = static_cast<char*>(memchr(line, '\t', length));
    if (!tab) return bad();
    int64_t stampUs;
    if (!ParseStamp(std::string_view(line, size_t(tab - line)), stampUs)) return bad();

    char* json = tab + 1;
    const char* lineEnd = line + length;
    const char* bones = static_cast<const char*>(memchr(json, '\t', size_t(lineEnd - json)));
    const size_t jsonLength = size_t((bones ? bones : lineEnd) - json);
    if (bones) ++stats.bonesSkipped;
    if (jsonLength == 0) return bad();

    const std::string_view jsonText(json, jsonLength);
    if (jsonText == previousJson_) {
        ++stats.duplicateFrames;
        if (previousKept_) Hold(stampUs);
        return true;
    }
    previousJson_.assign(jsonText);
    previousKept_ = false;

    const size_t poolNeed = yyjson_read_max_memory_usage(jsonLength, 0);
    if (jsonPool_.size() < poolNeed) jsonPool_.resize(poolNeed + poolNeed / 4);
    yyjson_alc alc;
    yyjson_alc_pool_init(&alc, jsonPool_.data(), jsonPool_.size());
    yyjson_doc* doc = yyjson_read_opts(json, jsonLength, 0, &alc, nullptr);
    if (!doc) return bad();
    const LineOutcome outcome = Frame(yyjson_doc_get_root(doc), stampUs, fatal);
    yyjson_doc_free(doc);
    if (!fatal.ok) return false;
    if (outcome == LineOutcome::Bad) return bad();
    previousKept_ = outcome == LineOutcome::Kept;
    return true;
}

// Spark writes the last API response again, verbatim, for as long as the API stalls (runs of up to
// 1.87 s were seen). Nothing changed in the game, so the repeat is a hold, not a gap: the previous
// state again at the new stamp, nothing moving and no events.
void EchoReplayParser::Hold(int64_t stampUs) {
    if (out_.players.size() > UINT32_MAX - 64) return;  // Frame refuses the file at this size
    const SrcFrame last = out_.frames.back();
    SrcFrame& held = out_.frames.emplace_back(last);
    held.tUs = stampUs - out_.epochUtcUs;
    held.firstEvent = uint32_t(out_.events.size());
    held.eventCount = 0;
    held.disc.vel[0] = held.disc.vel[1] = held.disc.vel[2] = 0.0f;
    held.firstPlayer = uint32_t(out_.players.size());
    for (uint32_t i = 0; i < last.playerCount; ++i) {
        SrcPlayer p = out_.players[last.firstPlayer + i];
        p.vel[0] = p.vel[1] = p.vel[2] = 0.0f;
        out_.players.push_back(p);
    }
}

uint16_t EchoReplayParser::Session(std::string_view id) {
    if (!out_.sessions.empty() && id == sessionOfLastFrame_) return uint16_t(sessionIndexOfLastFrame_);
    SrcSession probe;
    CopyText(probe.id, sizeof probe.id, id);  // stored truncated, so look it up truncated
    auto [it, inserted] = sessionIndex_.try_emplace(probe.id, uint32_t(out_.sessions.size()));
    if (inserted) out_.sessions.push_back(probe);
    sessionOfLastFrame_.assign(id);
    sessionIndexOfLastFrame_ = it->second;
    return uint16_t(it->second);
}

void EchoReplayParser::Basis(yyjson_val* part, BasisPart which, float q[4], float prior[4], bool& priorValid) {
    float forward[3], up[3], left[3];
    const bool present = Vec3(yyjson_obj_get(part, "forward"), forward) & Vec3(yyjson_obj_get(part, "up"), up) &
                         Vec3(yyjson_obj_get(part, "left"), left);
    if (present && BasisToQuat(forward, up, left, q) == BasisVerdict::Ok) {
        if (priorValid) AlignHemisphere(prior, q);
        memcpy(prior, q, 4 * sizeof(float));
        priorValid = true;
        return;
    }
    SourceStats& stats = out_.stats;
    if (stats.basisTrips[which]++ == 0) stats.basisFirstTripFrame[which] = uint32_t(out_.frames.size() - 1);  // called after the frame is appended
    memcpy(q, priorValid ? prior : kIdentityQuat, 4 * sizeof(float));
}

bool EchoReplayParser::Player(yyjson_val* obj, uint8_t team, SrcPlayer& p) {
    memset(&p, 0, sizeof p);
    const std::string_view name = Str(yyjson_obj_get(obj, "name"));
    p.userId = U64(yyjson_obj_get(obj, "userid"));
    p.team = team;
    p.number = ByteOr(yyjson_obj_get(obj, "number"), kNotInFrame);
    p.level = ByteOr(yyjson_obj_get(obj, "level"), kNotInFrame);
    p.rosterIndex = roster_.Intern(p.userId, name, p.number, p.level, team);
    if (p.rosterIndex == kNoSlot) return false;
    p.apiSlot = ByteOr(yyjson_obj_get(obj, "playerid"), kNotInFrame);

    if (playerQuats_.size() <= p.rosterIndex) playerQuats_.resize(size_t(p.rosterIndex) + 1, PlayerQuats{});
    PlayerQuats& quats = playerQuats_[p.rosterIndex];
    static constexpr struct {
        const char* key;
        BasisPart part;
        Pose SrcPlayer::*pose;
    } kParts[] = {
        {"head", kPartHead, &SrcPlayer::head},
        {"body", kPartBody, &SrcPlayer::body},
        {"lhand", kPartLHand, &SrcPlayer::lhand},
        {"rhand", kPartRHand, &SrcPlayer::rhand},
    };
    for (const auto& part : kParts) {
        yyjson_val* v = yyjson_obj_get(obj, part.key);
        Pose& pose = p.*part.pose;
        Vec3(Position(v), pose.pos);
        Basis(v, part.part, pose.q, quats.q[part.part], quats.valid[part.part]);
    }
    Vec3(yyjson_obj_get(obj, "velocity"), p.vel);
    ParseHold(Str(yyjson_obj_get(obj, "holding_left")), p.holdL, p.holdLSlot);
    ParseHold(Str(yyjson_obj_get(obj, "holding_right")), p.holdR, p.holdRSlot);
    p.stunned = Bool(obj, "stunned");
    p.possession = Bool(obj, "possession");
    p.blocking = Bool(obj, "blocking");
    p.invulnerable = Bool(obj, "invulnerable");
    p.emote = Bool(obj, "is_emote_playing");

    yyjson_val* stats = yyjson_obj_get(obj, "stats");
    static constexpr const char* kStatKeys[kStatCount] = {"points", "goals",  "saves",         "stuns",
                                                          "passes", "catches", "steals",       "blocks",
                                                          "interceptions", "assists", "shots_taken"};
    for (int i = 0; i < kStatCount; ++i) p.stats[i] = U16(yyjson_obj_get(stats, kStatKeys[i]));
    p.possessionTime = Float(stats, "possession_time");
    return true;
}

uint64_t EchoReplayParser::ResolveName(std::string_view name, const SrcFrame& frame) {
    if (name.empty() || name == "[INVALID]") return kNoUser;
    for (uint32_t i = 0; i < frame.playerCount; ++i) {
        const SrcPlayer& p = out_.players[frame.firstPlayer + i];
        if (name == out_.roster[p.rosterIndex].name) return p.userId;
    }
    ++out_.stats.unresolvedPlayerRefs;
    return kNoUser;
}

// last_score is a snapshot of the most recent goal; a change is a new goal (or a new session's reset),
// which Infer pairs with the points edge. The first frame only sets the baseline.
void EchoReplayParser::GoalSnapshot(yyjson_val* root, const SrcFrame& frame) {
    yyjson_val* ls = yyjson_obj_get(root, "last_score");
    if (!yyjson_is_obj(ls)) return;
    LastScore now;
    now.discSpeed = Float(ls, "disc_speed");
    now.distance = Float(ls, "distance_thrown");
    now.team = TeamFromName(Str(yyjson_obj_get(ls, "team")));
    now.points = int(Num(yyjson_obj_get(ls, "point_amount")));
    now.goalType = Str(yyjson_obj_get(ls, "goal_type"));
    now.scorer = Str(yyjson_obj_get(ls, "person_scored"));
    now.assist = Str(yyjson_obj_get(ls, "assist_scored"));
    const bool changed = haveLastScore_ && !(now == lastScore_);
    const bool first = !haveLastScore_;
    haveLastScore_ = true;
    if (first || !changed) {
        lastScore_ = std::move(now);
        return;
    }
    SrcEvent& e = out_.events.emplace_back();
    memset(&e, 0, sizeof e);
    e.tUs = frame.tUs;
    e.kind = SrcEventKind::GoalScored;
    e.a = ResolveName(now.scorer, frame);
    e.b = ResolveName(now.assist, frame);
    e.c = uint32_t(now.team < 0 ? kNotInFrame : now.team) | uint32_t(now.points & 0xFF) << 8;
    e.f0 = now.discSpeed;
    e.f1 = now.distance;
    CopyText(e.text, sizeof e.text, now.goalType);
    lastScore_ = std::move(now);
}

LineOutcome EchoReplayParser::Frame(yyjson_val* root, int64_t stampUs, Result& fatal) {
    if (!yyjson_is_obj(root)) return LineOutcome::Bad;
    if (Num(yyjson_obj_get(root, "err_code")) != 0.0) {
        ++out_.stats.errCodeFrames;
        return LineOutcome::Skipped;
    }
    yyjson_val* teams = yyjson_obj_get(root, "teams");
    if (!yyjson_is_arr(teams)) return LineOutcome::Bad;

    if (!haveEpoch_) {
        haveEpoch_ = true;
        out_.epochUtcUs = stampUs;
        CopyText(out_.mapName, sizeof out_.mapName, Str(yyjson_obj_get(root, "map_name")));
        out_.privateMatch = Bool(root, "private_match");
    }

    if (out_.sessions.size() >= kNoSlot) {
        fatal = Refuse(ResultCode::Corrupt, "more than %u session ids in one file", unsigned(kNoSlot));
        return LineOutcome::Bad;
    }
    SrcFrame& f = out_.frames.emplace_back();
    memset(&f, 0, sizeof f);
    f.tUs = stampUs - out_.epochUtcUs;
    f.session = Session(Str(yyjson_obj_get(root, "sessionid")));
    f.status = ParseGameStatus(Str(yyjson_obj_get(root, "game_status")));
    f.pause = ParsePauseState(Str(yyjson_obj_get(yyjson_obj_get(root, "pause"), "paused_state")));
    f.gameClock = Float(root, "game_clock");
    f.bluePoints = U16(yyjson_obj_get(root, "blue_points"));
    f.orangePoints = U16(yyjson_obj_get(root, "orange_points"));
    f.blueRounds = U16(yyjson_obj_get(root, "blue_round_score"));
    f.orangeRounds = U16(yyjson_obj_get(root, "orange_round_score"));

    yyjson_val* disc = yyjson_obj_get(root, "disc");
    Vec3(yyjson_obj_get(disc, "position"), f.disc.pose.pos);
    Basis(disc, kPartDisc, f.disc.pose.q, discQuat_, discQuatValid_);
    Vec3(yyjson_obj_get(disc, "velocity"), f.disc.vel);
    f.disc.bounceCount = U16(yyjson_obj_get(disc, "bounce_count"));
    f.disc.holderApiSlot = kNotInFrame;

    if (out_.players.size() > UINT32_MAX - 64) {
        fatal = Refuse(ResultCode::Corrupt, "more player samples than the model can index (line %u)", out_.stats.lines);
        return LineOutcome::Bad;
    }
    f.firstPlayer = uint32_t(out_.players.size());
    size_t teamIndex = 0;
    yyjson_val* team;
    yyjson_arr_iter teamIt;
    yyjson_arr_iter_init(teams, &teamIt);
    while ((team = yyjson_arr_iter_next(&teamIt)) != nullptr && teamIndex < 3) {
        const int named = TeamFromName(Str(yyjson_obj_get(team, "team")));
        const uint8_t teamId = uint8_t(named >= 0 ? named : int(teamIndex));
        ++teamIndex;
        if (teamId <= kTeamOrange) {
            yyjson_val* ts = yyjson_obj_get(team, "stats");
            if (yyjson_is_obj(ts)) {
                f.hasTeamStats = true;
                static constexpr const char* kTeamStatKeys[kStatCount] = {
                    "points", "goals", "saves", "stuns", "passes", "catches", "steals", "blocks", "interceptions",
                    "assists", "shots_taken"};
                for (int i = 0; i < kStatCount; ++i) f.teamStats[teamId][i] = U16(yyjson_obj_get(ts, kTeamStatKeys[i]));
                f.teamPossessionTime[teamId] = Float(ts, "possession_time");
            }
        }
        yyjson_val* players = yyjson_obj_get(team, "players");
        if (!yyjson_is_arr(players)) continue;
        yyjson_val* player;
        yyjson_arr_iter playerIt;
        yyjson_arr_iter_init(players, &playerIt);
        while ((player = yyjson_arr_iter_next(&playerIt)) != nullptr) {
            if (!yyjson_is_obj(player)) continue;
            SrcPlayer& p = out_.players.emplace_back();
            if (!Player(player, teamId, p)) {
                fatal = Refuse(ResultCode::Roster, "more than %u distinct players in one file", unsigned(kNoSlot));
                return LineOutcome::Bad;
            }
        }
    }
    f.playerCount = uint32_t(out_.players.size() - f.firstPlayer);
    f.firstEvent = uint32_t(out_.events.size());
    GoalSnapshot(root, f);
    f.eventCount = uint32_t(out_.events.size() - f.firstEvent);
    return LineOutcome::Kept;
}

}  // namespace

Result ReadEchoReplay(const wchar_t* path, SourceMatch& out, Progress* progress, uint64_t maxModelBytes) {
    out = SourceMatch{};
    out.kind = SourceKind::EchoReplay;
    SourceStream stream;
    Result opened = stream.Open(path, out.stats);
    if (!opened.ok) return opened;

    const size_t estimatedFrames = size_t(stream.decodedSizeHint() / kTypicalLineBytes) + 16;
    const size_t reserveFrames = estimatedFrames < kReserveFramesCap ? estimatedFrames : kReserveFramesCap;
    out.frames.reserve(reserveFrames);
    out.players.reserve(reserveFrames * 10);

    EchoReplayParser parser(out);
    Result fatal = Success();
    std::vector<char> buffer(kChunkBytes);
    size_t lineStart = 0;  // first byte of the pending line
    size_t used = 0;       // bytes of valid data in buffer
    uint32_t linesAtLastPoll = 0;

    for (;;) {
        if (used == buffer.size()) {
            if (lineStart > 0) {
                memmove(buffer.data(), buffer.data() + lineStart, used - lineStart);
                used -= lineStart;
                lineStart = 0;
            } else if (buffer.size() >= kMaxLineBytes) {
                return Refuse(ResultCode::Corrupt, "line %u is longer than %zu MB", out.stats.lines + 1,
                              kMaxLineBytes >> 20);
            } else {
                buffer.resize(buffer.size() * 2);
            }
        }
        const size_t got = stream.Read(buffer.data() + used, buffer.size() - used);
        if (got == 0) break;
        size_t scan = used;
        used += got;
        while (char* nl = static_cast<char*>(memchr(buffer.data() + scan, '\n', used - scan))) {
            const size_t end = size_t(nl - buffer.data());
            if (!parser.Line(buffer.data() + lineStart, end - lineStart, true, fatal)) return fatal;
            lineStart = scan = end + 1;
        }
        if (out.stats.lines - linesAtLastPoll >= kProgressEveryLines) {
            linesAtLastPoll = out.stats.lines;
            if (ProgressCancelled(progress)) return Refuse(ResultCode::Cancelled, "cancelled while reading");
            const Result budget = CheckModelBudget(out, maxModelBytes);
            if (!budget.ok) return budget;
            ProgressUpdate(progress, stream.consumed(), stream.fileSize());
        }
    }
    if (stream.failed()) return stream.error();
    if (lineStart < used && !parser.Line(buffer.data() + lineStart, used - lineStart, false, fatal)) return fatal;

    // A crash-cut final line is expected damage, not evidence of a broken file; it is counted but not
    // held against the 1% rule.
    const SourceStats& stats = out.stats;
    if (TooManyBadLines(stats.badLines - parser.unterminatedBad, stats.lines)) {
        return Refuse(ResultCode::Corrupt, "%u of %u lines are unreadable (over 1%%)", stats.badLines, stats.lines);
    }
    if (out.frames.empty()) {
        return Refuse(ResultCode::Corrupt, "no readable frames in %u lines (%u unreadable, %u err_code, %u duplicate)",
                      stats.lines, stats.badLines, stats.errCodeFrames, stats.duplicateFrames);
    }
    ProgressUpdate(progress, 1, 1);
    return Success();
}

}  // namespace replay::import
