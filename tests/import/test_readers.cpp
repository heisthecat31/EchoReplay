// P2: the .echoreplay and .tape readers (DESIGN-FORMATS §7.2, §7.3, §11.1). Hand-built fixtures pin each
// format rule; the MIT sample pair (tests/import/data) pins real data and the two readers' agreement.

#include "doctest.h"

#include <math.h>
#include <process.h>
#include <string.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include <miniz.h>
#include <zstd.h>

#include "import/reader.h"

using namespace replay::import;

namespace {

namespace fs = std::filesystem;

fs::path DataFile(const char* name) { return fs::path(IMPORT_TEST_DATA_DIR) / name; }

// A scratch file under %TEMP% that removes itself.
struct TempFile {
    fs::path path;
    TempFile(const char* name, std::string_view bytes) {
        path = fs::temp_directory_path() / (std::string("import_p2_") + std::to_string(_getpid()) + "_" + name);
        std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    }
    ~TempFile() {
        std::error_code ignored;
        fs::remove(path, ignored);
    }
    const wchar_t* c_str() const { return path.c_str(); }
};

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Put16(std::string& s, uint32_t v) { s.append({char(v), char(v >> 8)}); }
void Put32(std::string& s, uint32_t v) { s.append({char(v), char(v >> 8), char(v >> 16), char(v >> 24)}); }

// One stored (uncompressed) entry; deflated entries are covered by the MIT sample.
std::string StoredZip(std::string_view name, std::string_view content) {
    const uint32_t crc = uint32_t(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(content.data()),
                                           content.size()));
    std::string zip;
    Put32(zip, 0x04034b50);
    Put16(zip, 20), Put16(zip, 0), Put16(zip, 0), Put16(zip, 0), Put16(zip, 0);
    Put32(zip, crc), Put32(zip, uint32_t(content.size())), Put32(zip, uint32_t(content.size()));
    Put16(zip, uint32_t(name.size())), Put16(zip, 0);
    zip += name;
    zip += content;
    const uint32_t directory = uint32_t(zip.size());
    Put32(zip, 0x02014b50);
    Put16(zip, 20), Put16(zip, 20), Put16(zip, 0), Put16(zip, 0), Put16(zip, 0), Put16(zip, 0);
    Put32(zip, crc), Put32(zip, uint32_t(content.size())), Put32(zip, uint32_t(content.size()));
    Put16(zip, uint32_t(name.size())), Put16(zip, 0), Put16(zip, 0), Put16(zip, 0), Put16(zip, 0);
    Put32(zip, 0), Put32(zip, 0);
    zip += name;
    const uint32_t directorySize = uint32_t(zip.size()) - directory;
    Put32(zip, 0x06054b50);
    Put16(zip, 0), Put16(zip, 0), Put16(zip, 1), Put16(zip, 1);
    Put32(zip, directorySize), Put32(zip, directory), Put16(zip, 0);
    return zip;
}

// Checks the slicing and index invariants every downstream stage relies on.
void CheckModel(const SourceMatch& m) {
    uint32_t nextPlayer = 0;
    for (const SrcFrame& f : m.frames) {
        REQUIRE(f.firstPlayer == nextPlayer);
        nextPlayer += f.playerCount;
        REQUIRE(uint64_t(f.firstEvent) + f.eventCount <= m.events.size());
        REQUIRE(f.session < m.sessions.size());
        const float* q = f.disc.pose.q;
        REQUIRE(fabsf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] - 1.0f) < 1e-3f);
    }
    REQUIRE(nextPlayer == m.players.size());
    for (const SrcPlayer& p : m.players) {
        REQUIRE(p.rosterIndex < m.roster.size());
        REQUIRE(m.roster[p.rosterIndex].userId == p.userId);
        for (const Pose* pose : {&p.head, &p.body, &p.lhand, &p.rhand}) {
            const float* q = pose->q;
            REQUIRE(fabsf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] - 1.0f) < 1e-3f);
        }
    }
}

// ---- .echoreplay fixtures ----

// forward (1,0,0), up (0,1,0), left (0,0,-1): yaw +90 degrees, q = (0, .7071, 0, .7071) (§5.4).
constexpr const char* kYawBasis = R"("forward":[1,0,0],"up":[0,1,0],"left":[0,0,-1])";

std::string PlayerJson(const char* name, int slot, uint64_t userId, float x, bool stunned, const char* holdLeft,
                       int points) {
    char buf[1400];
    snprintf(buf, sizeof buf,
             R"({"name":"%s","playerid":%d,"userid":%llu,"number":7,"level":30,"stunned":%s,"possession":false,)"
             R"("blocking":false,"invulnerable":false,"is_emote_playing":false,"holding_left":"%s",)"
             R"("holding_right":"none","velocity":[1,2,3],)"
             R"("head":{"position":[%g,1,2],%s},"body":{"position":[%g,1,2],%s},)"
             R"("lhand":{"pos":[%g,0.5,2],%s},"rhand":{"position":[%g,0.6,2],%s},)"
             R"("stats":{"possession_time":1.5,"points":%d,"saves":4,"goals":1,"stuns":2,"passes":0,"catches":0,)"
             R"("steals":0,"blocks":0,"interceptions":0,"assists":0,"shots_taken":3}})",
             name, slot, static_cast<unsigned long long>(userId), stunned ? "true" : "false", holdLeft, x, kYawBasis, x,
             kYawBasis, x + 0.3f, kYawBasis, x - 0.3f, kYawBasis, points);
    return buf;
}

std::string SessionJson(float clock, int bluePoints, const char* scorer, const std::string& bluePlayers,
                        const char* orangeTeamExtra = "") {
    char head[900];
    snprintf(head, sizeof head,
             R"({"sessionid":"AAAA-1","map_name":"mpl_arena_a","private_match":true,"game_status":"playing",)"
             R"("game_clock":%g,"blue_points":%d,"orange_points":0,"blue_round_score":1,"orange_round_score":0,)"
             R"("pause":{"paused_state":"unpaused"},"err_code":0,)"
             R"("last_score":{"disc_speed":10.5,"team":"blue","goal_type":"INSIDE SHOT","point_amount":2,)"
             R"("distance_thrown":7.5,"person_scored":"%s","assist_scored":"[INVALID]"},)"
             R"("disc":{"position":[0,0,5],%s,"velocity":[0,0,1],"bounce_count":2},)",
             clock, bluePoints, scorer, kYawBasis);
    return std::string(head) + R"("teams":[{"team":"BLUE TEAM","players":[)" + bluePlayers +
           R"(],"stats":{"points":2,"possession_time":3.5}},{"team":"ORANGE TEAM")" + orangeTeamExtra +
           R"(},{"team":"SPECTATORS"}]})";
}

std::string FixtureLines() {
    const std::string players0 = PlayerJson("Alpha", 0, 1111, 1.0f, false, "none", 0) + "," +
                                 PlayerJson("Bravo", 1, 2222, -1.0f, false, "geo", 0);
    const std::string players1 = PlayerJson("Alpha", 0, 1111, 1.1f, true, "disc", 2) + "," +
                                 PlayerJson("Bravo", 1, 2222, -1.1f, false, "0", 0);
    const std::string frame0 = SessionJson(300.0f, 0, "[INVALID]", players0);
    const std::string frame1 = SessionJson(299.9f, 2, "Alpha", players1);
    const std::string frame2 = SessionJson(299.8f, 2, "Alpha", players1, R"(,"players":[])");
    std::string text;
    text += "2025/08/03 17:15:57.100\t" + frame0 + "\n";
    text += "2025/08/03 17.15.57.133\t" + frame1 + "\r\n";  // legacy time form, CRLF
    text += "2025/08/03 17:15:57.166\t" + frame1 + "\n";   // byte-identical JSON: a hold, not parsed
    text += "2025/08/03 17:15:57.170\t{\"err_code\":-6,\"teams\":[]}\n";
    text += "2025/08/03 17:15:57.200\t" + frame2 + "\t{\"user_bones\":[{\"playerid\":0}],\"err_code\":0}\n";
    text += "2025/08/03 17:15:57.233\t{\"disc\":{\"posi";  // crash-cut final line, no newline
    return text;
}

void CheckFixture(const SourceMatch& m) {
    CheckModel(m);
    CHECK(m.kind == SourceKind::EchoReplay);
    CHECK(std::string(m.mapName) == "mpl_arena_a");
    CHECK(m.privateMatch);
    REQUIRE(m.frames.size() == 4);
    CHECK(m.stats.lines == 6);
    CHECK(m.stats.duplicateFrames == 1);
    CHECK(m.stats.errCodeFrames == 1);
    CHECK(m.stats.bonesSkipped == 1);
    CHECK(m.stats.badLines == 1);
    CHECK(m.frames[0].tUs == 0);
    CHECK(m.frames[1].tUs == 33000);
    CHECK(m.frames[2].tUs == 66000);
    CHECK(m.frames[3].tUs == 100000);
    CHECK(m.epochUtcUs == 1754241357100000LL);  // 2025-08-03T17:15:57.100Z
    REQUIRE(m.roster.size() == 2);
    CHECK(std::string(m.roster[0].name) == "Alpha");
    CHECK(m.roster[1].userId == 2222);
    CHECK(m.frames[0].playerCount == 2);  // orange `players` absent: no players, no failure
    CHECK(m.frames[1].status == GameStatus::Playing);
    CHECK(m.frames[1].bluePoints == 2);
    CHECK(m.frames[1].blueRounds == 1);
    CHECK(m.frames[1].hasTeamStats);
    CHECK(m.frames[1].teamStats[kTeamBlue][kStatPoints] == 2);
    CHECK(m.frames[1].teamPossessionTime[kTeamBlue] == doctest::Approx(3.5f));
    CHECK(m.frames[0].disc.bounceCount == 2);
    CHECK(m.frames[0].disc.holderApiSlot == kNotInFrame);

    const SrcPlayer& alpha1 = m.players[m.frames[1].firstPlayer];
    CHECK(alpha1.userId == 1111);
    CHECK(alpha1.apiSlot == 0);
    CHECK(alpha1.team == kTeamBlue);
    CHECK(alpha1.stunned);
    CHECK(alpha1.holdL == Hold::Disc);
    CHECK(alpha1.stats[kStatPoints] == 2);
    CHECK(alpha1.stats[kStatSaves] == 4);
    CHECK(alpha1.stats[kStatShots] == 3);
    CHECK(alpha1.possessionTime == doctest::Approx(1.5f));
    CHECK(alpha1.number == 7);
    CHECK(alpha1.lhand.pos[0] == doctest::Approx(1.4f));  // `pos`
    CHECK(alpha1.rhand.pos[0] == doctest::Approx(0.8f));  // older `position` spelling
    CHECK(alpha1.head.q[1] == doctest::Approx(0.70710678f).epsilon(1e-4));
    CHECK(alpha1.head.q[3] == doctest::Approx(0.70710678f).epsilon(1e-4));
    const SrcPlayer& bravo1 = m.players[m.frames[1].firstPlayer + 1];

    // The repeated line is a hold (R1): frame 1's state again at its own stamp, nothing moving, no events.
    const SrcFrame& held = m.frames[2];
    CHECK(held.eventCount == 0);
    CHECK(held.bluePoints == m.frames[1].bluePoints);
    CHECK(held.disc.vel[0] == 0.0f);
    CHECK(held.disc.vel[1] == 0.0f);
    CHECK(held.disc.vel[2] == 0.0f);
    CHECK(held.disc.pose.pos[0] == m.frames[1].disc.pose.pos[0]);
    REQUIRE(held.playerCount == m.frames[1].playerCount);
    for (uint32_t j = 0; j < held.playerCount; ++j) {
        const SrcPlayer& was = m.players[m.frames[1].firstPlayer + j];
        const SrcPlayer& now = m.players[held.firstPlayer + j];
        CHECK(now.userId == was.userId);
        CHECK(now.head.pos[0] == was.head.pos[0]);
        CHECK(now.head.q[1] == was.head.q[1]);
        CHECK(now.holdL == was.holdL);
        CHECK(now.stunned == was.stunned);
        CHECK(now.vel[0] == 0.0f);
        CHECK(now.vel[1] == 0.0f);
        CHECK(now.vel[2] == 0.0f);
    }
    CHECK(bravo1.holdL == Hold::Player);
    CHECK(bravo1.holdLSlot == 0);
    CHECK(bravo1.holdR == Hold::None);

    // last_score changed between frames 0 and 1: one goal snapshot, credited by name.
    REQUIRE(m.events.size() == 1);
    CHECK(m.frames[1].eventCount == 1);
    const SrcEvent& goal = m.events[0];
    CHECK(goal.kind == SrcEventKind::GoalScored);
    CHECK(goal.a == 1111);
    CHECK(goal.b == kNoUser);
    CHECK(goal.c == (kTeamBlue | 2u << 8));
    CHECK(std::string(goal.text) == "INSIDE SHOT");
    for (int part = 0; part < kPartCount; ++part) CHECK(m.stats.basisTrips[part] == 0);
}

// ---- .tape fixtures ----

struct Pb {
    std::string b;
    Pb& Varint(uint64_t v) {
        while (v >= 0x80) {
            b += char(v | 0x80);
            v >>= 7;
        }
        b += char(v);
        return *this;
    }
    Pb& U(uint32_t field, uint64_t v) { return Varint(field << 3).Varint(v); }
    Pb& F(uint32_t field, float v) {
        Varint(field << 3 | 5);
        b.append(reinterpret_cast<const char*>(&v), 4);
        return *this;
    }
    Pb& S(uint32_t field, std::string_view s) {
        Varint(field << 3 | 2).Varint(s.size());
        b += s;
        return *this;
    }
    Pb& M(uint32_t field, const Pb& m) { return S(field, m.b); }
};

Pb Vec(float x, float y, float z) { return Pb().F(1, x).F(2, y).F(3, z); }
Pb TapePose(float x) { return Pb().M(1, Vec(x, 1, 2)).M(2, Pb().F(2, 0.70710678f).F(4, 0.70710678f)); }

struct TapeOptions {
    uint32_t version = 2;
    int frames = 6;
    bool footer = true;
};

// Envelope stream: header (roster slots 0 and 3), frames, footer. Unknown fields are sprinkled in to
// prove they are skipped; slot 0 is never written, as proto3 omits zeros.
std::string TapeEnvelopes(const TapeOptions& o) {
    std::string stream;
    const auto envelope = [&](uint32_t field, const Pb& body) {
        const Pb env = Pb().M(field, body);
        stream += Pb().Varint(env.b.size()).b + env.b;
    };
    const Pb roster0 = Pb().U(2, 1111).S(3, "Alpha").U(4, 1).U(5, 7).U(6, 30);
    const Pb roster3 = Pb().U(1, 3).U(2, 2222).S(3, "Bravo").U(4, 2).U(5, 9).U(6, 12);
    const Pb arenaHeader = Pb().S(1, "AAAA-1").S(2, "mpl_arena_a").M(8, roster0).M(8, roster3).U(77, 5);
    envelope(1, Pb()
                    .M(2, Pb().U(1, 1754241357).U(2, 100000000))
                    .U(3, o.version)
                    .U(5, 3)
                    .M(4, Pb().S(1, "private_match").S(2, "true"))
                    .M(10, arenaHeader)
                    .U(101, 30)
                    .S(900, "future"));
    for (int i = 0; i < o.frames; ++i) {
        Pb arena;
        arena.U(1, 3).F(2, 300.0f - i).U(3, 1);
        arena.M(4, Pb().M(1, TapePose(0)).M(2, Vec(0, 0, 1)).U(3, 2));
        const uint32_t alphaFlags = (i >= 2 ? 1u : 0u) | 0u << 5;
        arena.M(5, Pb().M(2, TapePose(1)).M(3, TapePose(1)).M(4, TapePose(1.3f)).M(5, TapePose(0.7f))
                        .M(6, Vec(1, 2, 3)).U(7, alphaFlags).F(10, 1.5f));
        arena.M(5, Pb().U(1, 3).M(2, TapePose(-1)).M(3, TapePose(-1)).M(4, TapePose(-1)).M(5, TapePose(-1))
                        .U(7, 1u << 5).U(42, 1));
        if (i >= 1) arena.U(7, 0);  // disc_holder_slot 0: present-but-zero is still "slot 0"
        arena.U(9, i >= 3 ? 2 : 0);
        if (i == 1) {
            arena.M(12, Pb().M(25, Pb().S(2, "disc").S(3, "geo")));
            arena.M(12, Pb().M(58, Pb().U(2, 2).U(3, 4).U(4, 1).U(12, 3)));
            arena.M(12, Pb().M(15, Pb().U(3, 1).U(4, 0)));
        }
        if (i == 3) {
            arena.M(12, Pb().M(40, Pb().F(1, 10.5f).U(2, 1).U(3, 1).U(4, 2).F(5, 7.5f).S(8, "Alpha").S(9, "[INVALID]")));
            arena.M(12, Pb().M(51, Pb().U(1, 3).U(2, 1)));
            arena.M(12, Pb().M(99, Pb().U(1, 1)));  // an event kind this reader does not know
        }
        envelope(2, Pb().U(1, i).U(2, uint64_t(33 * i)).U(3, i == 0).M(10, arena).U(50, 1));
    }
    if (o.footer) envelope(3, Pb().U(1, o.frames).U(2, 33 * (o.frames - 1)));
    return stream;
}

// Two independent zstd frames plus a skippable frame, the shape tape's seekable writer produces.
std::string Zstd(std::string_view raw) {
    std::string out;
    const size_t half = raw.size() / 2;
    for (std::string_view part : {raw.substr(0, half), raw.substr(half)}) {
        std::string frame(ZSTD_compressBound(part.size()), '\0');
        const size_t n = ZSTD_compress(frame.data(), frame.size(), part.data(), part.size(), 3);
        REQUIRE_FALSE(ZSTD_isError(n));
        out.append(frame.data(), n);
    }
    Put32(out, 0x184D2A5E);
    Put32(out, 8);
    out.append("seektab!");
    return out;
}

void CheckTapeFixture(const SourceMatch& m, int frames) {
    CheckModel(m);
    CHECK(m.kind == SourceKind::Tape);
    CHECK(m.nominalHz == 30);
    CHECK(m.privateMatch);
    CHECK(std::string(m.mapName) == "mpl_arena_a");
    REQUIRE(m.sessions.size() == 1);
    CHECK(std::string(m.sessions[0].id) == "AAAA-1");
    CHECK(m.epochUtcUs == 1754241357100000LL);
    REQUIRE(int(m.frames.size()) == frames);
    CHECK(m.stats.badLines == 0);
    CHECK(m.stats.unresolvedPlayerRefs == 0);
    REQUIRE(m.roster.size() == 2);
    CHECK(m.roster[0].userId == 1111);
    CHECK(std::string(m.roster[0].name) == "Alpha");
    CHECK(m.roster[0].number == 7);
    CHECK(m.roster[1].team == kTeamOrange);

    const SrcFrame& f0 = m.frames[0];
    CHECK(f0.status == GameStatus::Playing);
    CHECK(f0.pause == PauseState::Unpaused);
    CHECK(f0.disc.holderApiSlot == kNotInFrame);
    CHECK(f0.disc.bounceCount == 2);
    const SrcPlayer& alpha0 = m.players[f0.firstPlayer];
    CHECK(alpha0.apiSlot == 0);
    CHECK(alpha0.userId == 1111);
    CHECK(alpha0.holdL == Hold::None);
    CHECK(alpha0.number == kNotInFrame);
    CHECK(alpha0.head.q[1] == doctest::Approx(0.70710678f));
    CHECK(alpha0.lhand.pos[0] == doctest::Approx(1.3f));
    CHECK(alpha0.possessionTime == doctest::Approx(1.5f));
    const SrcPlayer& bravo0 = m.players[f0.firstPlayer + 1];
    CHECK(bravo0.apiSlot == 3);
    CHECK(bravo0.team == kTeamOrange);

    if (frames < 4) return;
    const SrcFrame& f1 = m.frames[1];
    CHECK(f1.disc.holderApiSlot == 0);
    CHECK(f1.blueRounds == 1);
    const SrcPlayer& alpha1 = m.players[f1.firstPlayer];
    CHECK(alpha1.holdL == Hold::Disc);  // GrabChanged in this very frame applies to it
    CHECK(alpha1.holdR == Hold::Geo);
    CHECK(alpha1.stats[kStatPoints] == 2);
    CHECK(alpha1.stats[kStatSaves] == 4);  // wire field 3 is saves, not goals
    CHECK(alpha1.stats[kStatGoals] == 1);
    CHECK(alpha1.stats[kStatShots] == 3);
    CHECK_FALSE(alpha1.stunned);
    CHECK(m.players[m.frames[2].firstPlayer].stunned);
    CHECK(m.players[m.frames[3].firstPlayer].holdL == Hold::Disc);  // carried forward
    CHECK(m.frames[3].bluePoints == 2);

    REQUIRE(m.frames[3].eventCount == 2);
    const SrcEvent& goal = m.events[m.frames[3].firstEvent];
    CHECK(goal.kind == SrcEventKind::GoalScored);
    CHECK(goal.a == 1111);
    CHECK(goal.b == kNoUser);
    CHECK(goal.c == (kTeamBlue | 2u << 8));
    CHECK(goal.f0 == doctest::Approx(10.5f));
    CHECK(std::string(goal.text) == "INSIDE SHOT");
    const SrcEvent& stun = m.events[m.frames[3].firstEvent + 1];
    CHECK(stun.kind == SrcEventKind::StunCredited);
    CHECK(stun.a == 2222);
}

uint32_t NextRandom(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

// Truncations and byte flips must end in a clean refusal or a model that keeps every invariant.
void Mutate(const std::string& original, const char* name, int rounds, uint32_t seed) {
    uint32_t state = seed;
    int accepted = 0;
    for (int round = 0; round < rounds; ++round) {
        std::string bytes = original;
        if (round % 3 == 0) {
            bytes.resize(NextRandom(state) % bytes.size());
        } else {
            const int flips = 1 + int(NextRandom(state) % 8);
            for (int i = 0; i < flips; ++i) bytes[NextRandom(state) % bytes.size()] ^= char(1 + NextRandom(state) % 255);
        }
        TempFile file(name, bytes);
        SourceMatch m;
        const Result r = ReadSource(file.c_str(), m, nullptr);
        if (r.ok) {
            ++accepted;
            REQUIRE_FALSE(m.frames.empty());
            CheckModel(m);
        } else {
            REQUIRE(r.message[0] != '\0');
            REQUIRE(r.code != ResultCode::NotImplemented);
        }
    }
    MESSAGE(std::string(name) << ": " << accepted << " of " << rounds << " mutants read, the rest refused cleanly");
}

}  // namespace

TEST_CASE("echoreplay: plain text fixture covers every line rule") {
    TempFile file("plain.echoreplay", FixtureLines());
    SourceMatch m;
    const Result r = ReadEchoReplay(file.c_str(), m, nullptr);
    REQUIRE_MESSAGE(r.ok, r.message);
    CheckFixture(m);
}

TEST_CASE("echoreplay: the same fixture zipped, entry named by a full path") {
    TempFile file("zipped.echoreplay", StoredZip("home/user/Downloads/rec_2025-08-03.echoreplay", FixtureLines()));
    SourceMatch m;
    Progress progress;
    const Result r = ReadSource(file.c_str(), m, &progress);
    REQUIRE_MESSAGE(r.ok, r.message);
    CheckFixture(m);
    CHECK(m.stats.zipEntries == 1);
    CHECK(progress.permille.load() == std::milli::den);
}

TEST_CASE("echoreplay: more than 1% unreadable lines refuses; at or under 1% reads") {
    const std::string good = "2025/08/03 17:15:57.100\t" +
                             SessionJson(300.0f, 0, "x", PlayerJson("Alpha", 0, 1111, 1.0f, false, "none", 0));
    std::string text;
    for (int i = 0; i < 99; ++i) {
        std::string line = good;
        line.replace(line.find("300"), 3, std::to_string(200 + i));  // distinct JSON, not duplicates
        text += line + "\n";
    }
    text += "not a stamp\t{}\n";
    {
        TempFile file("onepct.echoreplay", text);
        SourceMatch m;
        const Result r = ReadEchoReplay(file.c_str(), m, nullptr);
        REQUIRE_MESSAGE(r.ok, r.message);
        CHECK(m.stats.badLines == 1);
        CHECK(m.frames.size() == 99);
    }
    text += "2025/08/03 17:15:58.000\t{\"teams\": [ broken\n";
    {
        TempFile file("twopct.echoreplay", text);
        SourceMatch m;
        const Result r = ReadEchoReplay(file.c_str(), m, nullptr);
        CHECK_FALSE(r.ok);
        CHECK(r.code == ResultCode::Corrupt);
    }
}

TEST_CASE("echoreplay: containers that are not recordings refuse with a reason") {
    SourceMatch m;
    CHECK(ReadEchoReplay(L"Z:\\definitely\\missing.echoreplay", m, nullptr).code == ResultCode::Io);
    {
        TempFile file("empty.echoreplay", "");
        CHECK(ReadEchoReplay(file.c_str(), m, nullptr).code == ResultCode::Corrupt);
    }
    {
        const std::string zip = StoredZip("a.echoreplay", FixtureLines());
        TempFile file("cut.echoreplay", zip.substr(0, zip.size() / 2));
        const Result r = ReadEchoReplay(file.c_str(), m, nullptr);
        CHECK(r.code == ResultCode::Container);
    }
    {
        std::string zip = StoredZip("a.echoreplay", FixtureLines());
        zip[60] ^= 0x55;  // inside the stored entry: the CRC no longer matches
        TempFile file("crc.echoreplay", zip);
        CHECK(ReadEchoReplay(file.c_str(), m, nullptr).code == ResultCode::Container);
    }
    CHECK(ReadSource(L"x.butter", m, nullptr).code == ResultCode::BadArguments);
}

TEST_CASE("echoreplay: a sessionid change starts a new session; a returning id reuses its index") {
    const std::string players = PlayerJson("Alpha", 0, 1111, 1.0f, false, "none", 0);
    std::string a1 = SessionJson(300.0f, 0, "x", players), b = a1, a2 = a1;
    b.replace(b.find("AAAA-1"), 6, "BBBB-2");
    a2.replace(a2.find("300"), 3, "250");
    const std::string text = "2025/08/03 17:15:57.100\t" + a1 + "\n2025/08/03 17:15:57.133\t" + b +
                             "\n2025/08/03 17:15:57.166\t" + a2 + "\n";
    TempFile file("sessions.echoreplay", text);
    SourceMatch m;
    REQUIRE(ReadEchoReplay(file.c_str(), m, nullptr).ok);
    REQUIRE(m.sessions.size() == 2);
    CHECK(std::string(m.sessions[1].id) == "BBBB-2");
    REQUIRE(m.frames.size() == 3);
    CHECK(m.frames[0].session == 0);
    CHECK(m.frames[1].session == 1);
    CHECK(m.frames[2].session == 0);
}

TEST_CASE("echoreplay: cancel is observed") {
    std::string text;
    const std::string good = "2025/08/03 17:15:57.100\t" +
                             SessionJson(300.0f, 0, "x", PlayerJson("Alpha", 0, 1111, 1.0f, false, "none", 0)) + "\n";
    for (int i = 0; i < 600; ++i) text += good;
    TempFile file("cancel.echoreplay", text);
    SourceMatch m;
    Progress progress;
    progress.cancel = true;
    CHECK(ReadEchoReplay(file.c_str(), m, &progress).code == ResultCode::Cancelled);
}

TEST_CASE("tape: zstd frames + skippable seek table, unknown fields skipped") {
    TempFile file("fixture.tape", Zstd(TapeEnvelopes({})));
    SourceMatch m;
    const Result r = ReadTape(file.c_str(), m, nullptr);
    REQUIRE_MESSAGE(r.ok, r.message);
    CHECK_FALSE(m.stats.truncatedSource);
    CheckTapeFixture(m, 6);
}

TEST_CASE("tape: uncompressed and zipped envelope streams read the same") {
    const std::string raw = TapeEnvelopes({});
    for (const std::string& bytes : {raw, StoredZip("x.tape", raw), StoredZip("x.tape", Zstd(raw))}) {
        TempFile file("variant.tape", bytes);
        SourceMatch m;
        const Result r = ReadTape(file.c_str(), m, nullptr);
        REQUIRE_MESSAGE(r.ok, r.message);
        CheckTapeFixture(m, 6);
    }
}

TEST_CASE("tape: a crash-cut capture keeps its whole frames and says so") {
    const std::string raw = TapeEnvelopes({});
    // Raw stream cut inside the last frame envelope (no footer): frames 0-4 survive.
    TapeOptions noFooter;
    noFooter.footer = false;
    const std::string cutRaw = TapeEnvelopes(noFooter);
    {
        TempFile file("cutraw.tape", cutRaw.substr(0, cutRaw.size() - 7));
        SourceMatch m;
        const Result r = ReadTape(file.c_str(), m, nullptr);
        REQUIRE_MESSAGE(r.ok, r.message);
        CHECK(m.stats.truncatedSource);
        CheckTapeFixture(m, 5);
    }
    // Compressed stream cut inside its second zstd frame: the first frame's envelopes survive.
    {
        const std::string z = Zstd(raw);
        TempFile file("cutzstd.tape", z.substr(0, z.size() - 40));
        SourceMatch m;
        const Result r = ReadTape(file.c_str(), m, nullptr);
        REQUIRE_MESSAGE(r.ok, r.message);
        CHECK(m.stats.truncatedSource);
        CHECK_FALSE(m.frames.empty());
        CheckModel(m);
    }
}

TEST_CASE("tape: format_version other than 2 refuses and names the number") {
    TapeOptions v3;
    v3.version = 3;
    TempFile file("v3.tape", Zstd(TapeEnvelopes(v3)));
    SourceMatch m;
    const Result r = ReadTape(file.c_str(), m, nullptr);
    CHECK(r.code == ResultCode::TapeVersion);
    CHECK(std::string(r.message).find("3") != std::string::npos);

    TapeOptions v0;
    v0.version = 0;  // pre-2.1 captures wrote no version; tape's own reader treats them as 2
    TempFile legacy("v0.tape", Zstd(TapeEnvelopes(v0)));
    CHECK(ReadTape(legacy.c_str(), m, nullptr).ok);
}

TEST_CASE("tape: streams that are not tapes refuse") {
    SourceMatch m;
    {
        TempFile file("text.tape", "hello, this is not a tape at all");
        CHECK_FALSE(ReadTape(file.c_str(), m, nullptr).ok);
    }
    {
        std::string z = Zstd(TapeEnvelopes({}));
        z[z.size() / 4] ^= 0x5A;
        TempFile file("damaged.tape", z);
        const Result r = ReadTape(file.c_str(), m, nullptr);
        if (!r.ok) CHECK(r.message[0] != '\0');
        else CheckModel(m);
    }
    {
        // A length prefix claiming 1 GB is refused before anything is allocated for it.
        const std::string huge = Pb().Varint(uint64_t(1) << 30).b + "xx";
        TempFile file("huge.tape", huge);
        CHECK(ReadTape(file.c_str(), m, nullptr).code == ResultCode::Corrupt);
    }
}

TEST_CASE("fuzz-ish: mutated fixtures and samples never crash and never break the model") {
    constexpr int kRounds = 240;
    Mutate(Zstd(TapeEnvelopes({})), "mut.tape", kRounds, 1);
    Mutate(TapeEnvelopes({}), "mutraw.tape", kRounds, 2);
    Mutate(FixtureLines(), "mut.echoreplay", kRounds, 3);
    Mutate(StoredZip("a.echoreplay", FixtureLines()), "mutzip.echoreplay", kRounds, 4);
    Mutate(ReadAll(DataFile("sample.tape")), "mutsample.tape", 12, 5);
    Mutate(ReadAll(DataFile("sample.echoreplay")), "mutsample.echoreplay", 12, 6);
}

// ---- the MIT sample pair: one match, two formats ----

TEST_CASE("MIT samples: both load with the expected counts") {
    SourceMatch tape, replay;
    Result r = ReadSource(DataFile("sample.tape").c_str(), tape, nullptr);
    REQUIRE_MESSAGE(r.ok, r.message);
    r = ReadSource(DataFile("sample.echoreplay").c_str(), replay, nullptr);
    REQUIRE_MESSAGE(r.ok, r.message);
    CheckModel(tape);
    CheckModel(replay);
    for (const SourceMatch* m : {&tape, &replay}) {
        CHECK(m->frames.size() == 1023);
        CHECK(m->players.size() == 2046);
        REQUIRE(m->roster.size() == 2);
        CHECK(std::string(m->roster[0].name) == "Milkyway");
        CHECK(std::string(m->roster[1].name) == "sprockee");
        CHECK(std::string(m->mapName) == "mpl_arena_a");
        const double span = (m->frames.back().tUs - m->frames.front().tUs) / 1e6;
        CHECK(span == doctest::Approx(63.3).epsilon(0.01));
        CHECK(m->stats.badLines == 0);
        CHECK(m->stats.unresolvedPlayerRefs == 0);
        for (int part = 0; part < kPartCount; ++part) CHECK(m->stats.basisTrips[part] == 0);
    }
    CHECK(replay.stats.bonesSkipped == 1023);
    CHECK_FALSE(tape.stats.truncatedSource);
}

namespace {

bool Near(const float* a, const float* b, int n, float tolerance) {
    for (int i = 0; i < n; ++i) {
        if (!(fabsf(a[i] - b[i]) <= tolerance)) return false;
    }
    return true;
}

// Same rotation: q and -q are one orientation.
bool SameRotation(const float* a, const float* b, float cosTolerance) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    return fabsf(dot) >= cosTolerance;
}

}  // namespace

// tape made sample.tape from sample.echoreplay, so both readers must describe one match. Every field the
// two formats both carry is compared frame by frame; the differences the formats themselves impose are
// listed here with their measured counts rather than tolerated silently.
TEST_CASE("MIT samples: the tape and its echoreplay give equivalent frame streams") {
    SourceMatch tape, replay;
    REQUIRE(ReadSource(DataFile("sample.tape").c_str(), tape, nullptr).ok);
    REQUIRE(ReadSource(DataFile("sample.echoreplay").c_str(), replay, nullptr).ok);
    REQUIRE(tape.frames.size() == replay.frames.size());
    constexpr int64_t kOneMsUs = std::micro::den / std::milli::den;
    CHECK(llabs(tape.epochUtcUs - replay.epochUtcUs) <= kOneMsUs);
    CHECK(std::string(tape.sessions[0].id) == replay.sessions[0].id);
    for (size_t i = 0; i < tape.roster.size(); ++i) {
        CHECK(tape.roster[i].userId == replay.roster[i].userId);
        CHECK(tape.roster[i].team == replay.roster[i].team);
        CHECK(tape.roster[i].number == replay.roster[i].number);
        CHECK(tape.roster[i].level == replay.roster[i].level);
    }

    std::map<std::string, int> mismatches;
    const auto count = [&](const char* field, bool same) {
        if (!same) ++mismatches[field];
    };
    // 3-decimal API vectors vs tape's float quaternions: 0.9999 is ~1.6 degrees of rotation.
    constexpr float kRotation = 0.9999f;
    for (size_t i = 0; i < tape.frames.size(); ++i) {
        const SrcFrame& t = tape.frames[i];
        const SrcFrame& e = replay.frames[i];
        count("tUs", llabs((t.tUs + tape.epochUtcUs) - (e.tUs + replay.epochUtcUs)) <= kOneMsUs);
        count("status", t.status == e.status);
        count("pause", t.pause == e.pause);
        count("gameClock", fabsf(t.gameClock - e.gameClock) < 1e-3f);
        count("points", t.bluePoints == e.bluePoints && t.orangePoints == e.orangePoints);
        count("rounds", t.blueRounds == e.blueRounds && t.orangeRounds == e.orangeRounds);
        count("disc.pos", Near(t.disc.pose.pos, e.disc.pose.pos, 3, 1e-4f));
        count("disc.q", SameRotation(t.disc.pose.q, e.disc.pose.q, kRotation));
        count("disc.vel", Near(t.disc.vel, e.disc.vel, 3, 1e-4f));
        count("disc.bounce", t.disc.bounceCount == e.disc.bounceCount);
        count("playerCount", t.playerCount == e.playerCount);
        if (t.playerCount != e.playerCount) continue;
        for (uint32_t j = 0; j < t.playerCount; ++j) {
            const SrcPlayer& tp = tape.players[t.firstPlayer + j];
            const SrcPlayer& ep = replay.players[e.firstPlayer + j];
            count("userId", tp.userId == ep.userId);
            count("team", tp.team == ep.team);
            count("apiSlot", tp.apiSlot == ep.apiSlot);
            const Pose* tPoses[4] = {&tp.head, &tp.body, &tp.lhand, &tp.rhand};
            const Pose* ePoses[4] = {&ep.head, &ep.body, &ep.lhand, &ep.rhand};
            static const char* kPos[4] = {"head.pos", "body.pos", "lhand.pos", "rhand.pos"};
            static const char* kQ[4] = {"head.q", "body.q", "lhand.q", "rhand.q"};
            for (int k = 0; k < 4; ++k) {
                count(kPos[k], Near(tPoses[k]->pos, ePoses[k]->pos, 3, 1e-4f));
                count(kQ[k], SameRotation(tPoses[k]->q, ePoses[k]->q, kRotation));
            }
            count("vel", Near(tp.vel, ep.vel, 3, 1e-4f));
            count("flags", tp.stunned == ep.stunned && tp.blocking == ep.blocking &&
                               tp.invulnerable == ep.invulnerable && tp.possession == ep.possession &&
                               tp.emote == ep.emote);
            count("holds", tp.holdL == ep.holdL && tp.holdR == ep.holdR && tp.holdLSlot == ep.holdLSlot &&
                               tp.holdRSlot == ep.holdRSlot);
            count("stats", memcmp(tp.stats, ep.stats, sizeof tp.stats) == 0);
            count("possessionTime", fabsf(tp.possessionTime - ep.possessionTime) < 1e-3f);
        }
    }
    std::string report;
    for (const auto& [field, n] : mismatches) report += field + "=" + std::to_string(n) + " ";
    CHECK_MESSAGE(mismatches.empty(), "per-field mismatches: " << report);

    // Non-vacuity: the agreement above covers states that actually occur in this match.
    int held = 0, playerHeld = 0, stunned = 0, withStats = 0, tightRotations = 0, rotations = 0;
    for (size_t i = 0; i < replay.players.size(); ++i) {
        const SrcPlayer& p = replay.players[i];
        held += p.holdL != Hold::None || p.holdR != Hold::None;
        playerHeld += p.holdL == Hold::Player || p.holdR == Hold::Player;
        stunned += p.stunned;
        withStats += p.stats[kStatPoints] + p.stats[kStatSteals] + p.stats[kStatStuns] > 0;
        const Pose* tPoses[4] = {&tape.players[i].head, &tape.players[i].body, &tape.players[i].lhand, &tape.players[i].rhand};
        const Pose* ePoses[4] = {&p.head, &p.body, &p.lhand, &p.rhand};
        for (int k = 0; k < 4; ++k) {
            ++rotations;
            tightRotations += SameRotation(tPoses[k]->q, ePoses[k]->q, 0.999999f);
        }
    }
    int goalPointEdges = 0;
    for (size_t i = 1; i < replay.frames.size(); ++i) {
        goalPointEdges += replay.frames[i].bluePoints + replay.frames[i].orangePoints !=
                          replay.frames[i - 1].bluePoints + replay.frames[i - 1].orangePoints;
    }
    MESSAGE("held " << held << ", player-held " << playerHeld << ", stunned " << stunned << ", with stats " << withStats
            << ", rotations within 0.08 deg " << tightRotations << "/" << rotations << ", point edges " << goalPointEdges);
    CHECK(held > 0);
    CHECK(playerHeld > 0);
    CHECK(tightRotations == rotations);  // BasisToQuat reproduces tape's own quaternion construction

    // Expected differences, each a property of the formats rather than of the readers:
    // - the echoreplay has no events at all; Infer derives them from state.
    // - tape reports the recording's opening last_score as a GoalScored at frame 0, where the echoreplay
    //   reader takes it as the baseline. No points edge exists in this match, so Infer pairs neither.
    CHECK(replay.events.empty());
    CHECK(goalPointEdges == 0);
    int tapeGoals = 0;
    for (const SrcEvent& e : tape.events) {
        if (e.kind != SrcEventKind::GoalScored) continue;
        ++tapeGoals;
        CHECK(e.tUs == tape.frames[0].tUs);
        CHECK(e.c == (kTeamOrange | 3u << 8));
        CHECK(std::string(e.text) == "LONG SHOT");
    }
    CHECK(tapeGoals == 1);
    CHECK(withStats > 0);
}
