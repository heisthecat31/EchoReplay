// FORMATS-P4: resample and emit. Every file here is written by the shipping ManifestBuilder /
// ChapterBuilder / DemoWriter and read back through the viewer's own DemoReader + windowbuild, so a
// pass is a statement about what playback will see, not about what the emitter meant to write.

#include "doctest.h"

#include <windows.h>
#include <psapi.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "capture/capture_facts.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "foundation/gametype.h"
#include "import/emit.h"
#include "import/resample.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"

using namespace replay;
using namespace replay::import;
using namespace replay::playback;

namespace {

constexpr int64_t kEpochUs = 1754234157000000;  // an arbitrary UTC epoch for the synthetic sources
constexpr uint64_t kArena = 0x576ED3F8428EBC4Bull;  // mpl_arena_a, as Normalise resolves it
constexpr int64_t kStep30HzUs = 1000000 / 30;

struct TempFile {
    std::wstring path;
    explicit TempFile(const wchar_t* tag) {
        wchar_t dir[MAX_PATH];
        GetTempPathW(MAX_PATH, dir);
        path = std::wstring(dir) + L"fmtp4_" + tag + L"_" + std::to_wstring(GetCurrentProcessId()) + L".echodemo";
        DeleteFileW(path.c_str());
    }
    ~TempFile() { DeleteFileW(path.c_str()); }
};

void Quat(float yaw, float out[4]) {
    out[0] = 0.0f;
    out[1] = sinf(yaw / 2);
    out[2] = 0.0f;
    out[3] = cosf(yaw / 2);
}

void SetPose(Pose& p, float x, float y, float z, const float q[4]) {
    p.pos[0] = x;
    p.pos[1] = y;
    p.pos[2] = z;
    memcpy(p.q, q, sizeof p.q);
}

struct Presence {
    uint8_t team;
    double fromS, toS;  // present while fromS <= t < toS
};

// A normalised synthetic match: players run circles, the disc follows player 0 then flies free.
struct Synth {
    SourceMatch m{};
    InferredTracks t{};

    Synth(double seconds, uint32_t hz, const std::vector<Presence>& who) {
        m.kind = SourceKind::EchoReplay;
        m.nominalHz = hz;
        strcpy_s(m.mapName, "mpl_arena_a");
        m.mapSymbol = kArena;
        m.epochUtcUs = kEpochUs;
        m.sessions.push_back(SrcSession{});
        for (size_t i = 0; i < who.size(); ++i) {
            SrcIdentity id{};
            id.userId = 0x1000 + i;
            _snprintf_s(id.name, sizeof id.name, _TRUNCATE, "synth\"%zu", i);
            id.number = static_cast<uint8_t>(10 + i);
            id.level = 30;
            id.team = who[i].team;
            id.firstSeenUs = int64_t(who[i].fromS * 1e6);
            id.lastSeenUs = int64_t(fmin(who[i].toS, seconds) * 1e6);
            m.roster.push_back(id);
        }
        const uint32_t frames = uint32_t(seconds * hz);
        for (uint32_t f = 0; f < frames; ++f) {
            const double ts = double(f) / hz;
            SrcFrame fr{};
            fr.tUs = int64_t(ts * 1e6);
            fr.status = ts < 5 ? GameStatus::PreMatch : ts < 8 ? GameStatus::RoundStart
                      : ts < 25 ? GameStatus::Playing : ts < 28 ? GameStatus::Score
                      : ts < 30 ? GameStatus::RoundOver : ts < 33 ? GameStatus::RoundStart
                      : ts < 55 ? GameStatus::Playing : GameStatus::PostMatch;
            fr.gameClock = float(fmax(0.0, 300.0 - ts));
            fr.bluePoints = ts >= 25 && ts < 30 ? 2 : 0;
            fr.blueRounds = ts >= 28 ? 1 : 0;
            float dq[4];
            Quat(float(ts), dq);
            SetPose(fr.disc.pose, float(5 * cos(ts)), 1.5f, float(5 * sin(ts)), dq);
            fr.firstPlayer = uint32_t(m.players.size());
            for (size_t i = 0; i < who.size(); ++i) {
                if (ts < who[i].fromS || ts >= who[i].toS) continue;
                SrcPlayer p{};
                p.userId = 0x1000 + i;
                p.rosterIndex = uint16_t(i);
                p.team = who[i].team;
                p.apiSlot = uint8_t(i);
                const double a = ts + double(i);
                float q[4];
                Quat(float(a + 1.5707963), q);
                const float x = float(3 * cos(a) + i), z = float(3 * sin(a));
                SetPose(p.head, x, 1.6f, z, q);
                SetPose(p.body, x, 1.2f, z, q);
                SetPose(p.lhand, x + 0.3f, 1.3f, z, q);
                SetPose(p.rhand, x - 0.3f, 1.3f, z, q);
                p.vel[0] = float(-3 * sin(a));
                p.vel[2] = float(3 * cos(a));
                p.number = uint8_t(10 + i);
                m.players.push_back(p);
                SampleInference s{};
                s.bits28 = 0x10;
                s.holdLSlot = s.holdRSlot = kNoSlot;
                t.samples.push_back(s);
            }
            fr.playerCount = uint32_t(m.players.size()) - fr.firstPlayer;
            m.frames.push_back(fr);
            t.discOwner.push_back(ts > 9 && ts < 12 ? uint16_t(0) : kNoSlot);
        }
        GoalRow g{};
        g.tUs = 25000000;
        g.team = import::kTeamBlue;
        g.points = 2;
        strcpy_s(g.goalTypeText, "INSIDE SHOT");
        g.scorerSlot = 0;
        g.assistSlot = kNoSlot;
        g.speedMps = 12.5f;
        g.distanceM = 7.0f;
        t.goals.push_back(g);
        StunRow st{};
        st.tUs = 15000000;
        st.victimSlot = 1;
        st.puncherSlot = 2;
        st.durationS = 2.5f;
        t.stuns.push_back(st);
        for (int64_t us = 6000000; us < int64_t(seconds * 1e6); us += 5000000) {
            StatRow r{};
            r.tUs = us;
            r.slot = 0;
            r.stats[kStatPoints] = uint16_t(us / 5000000);
            r.possessionTime = float(us) * 1e-7f;
            t.stats.push_back(r);
        }
    }
};

struct Decoded {
    std::map<uint16_t, std::vector<std::pair<uint64_t, PlayerState>>> players;  // by recorded slot
    std::map<uint64_t, std::vector<std::pair<uint64_t, BodyState>>> bodies;     // by id value
    StreamStats playerWire, bodyWire;
    uint64_t flags = 0, t0 = 0;
    uint16_t playerCount = 0, roundRows = 0;
    std::vector<PlayerRecord> records;
};

void Add(StreamStats& to, const StreamStats& s) {
    to.read += s.read;
    to.keyed += s.keyed;
    to.dropped_verbatim += s.dropped_verbatim;
    to.dropped_identity += s.dropped_identity;
    to.dropped_clock += s.dropped_clock;
    to.dropped_window += s.dropped_window;
}

// The viewer's read path, whole file: DemoReader, ScanTrackSources, BuildWindow per core chapter.
Decoded Decode(const wchar_t* path) {
    Decoded d;
    DemoReader reader;
    REQUIRE(reader.Open(path));
    d.flags = reader.FileFlags();
    const Manifest& m = reader.GetManifest();
    d.t0 = m.t0Us;
    d.playerCount = m.playerCount;
    d.roundRows = m.roundRowCount;
    d.records.assign(m.players, m.players + m.playerCount);
    TrackScan scan;
    const char* why = "";
    REQUIRE_MESSAGE(ScanTrackSources(reader, scan, &why), why);
    Window w;
    REQUIRE_MESSAGE(w.Reserve(CapsForScan(CapsForCadence(2000000, 22222), scan), &why), why);
    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        if (reader.ChapterAt(i).section != kSectionCore) continue;
        BuildStats st;
        REQUIRE_MESSAGE(BuildWindow(reader, i, w, st, &scan.clocks), st.refusal);
        Add(d.playerWire, st.player_wire);
        Add(d.bodyWire, st.body_wire);
        for (size_t p = 0; p < w.PlayerCount(); ++p) {
            const PlayerTrack& tr = w.PlayerAt(p);
            for (size_t k = 0; k < tr.count; ++k) {
                d.players[tr.user_slot].push_back({w.PlayerTimes(tr)[k], w.PlayerFrames(tr)[k]});
            }
        }
        for (size_t b = 0; b < w.BodyCount(); ++b) {
            const BodyTrack& tr = w.BodyAt(b);
            for (size_t k = 0; k < tr.count; ++k) {
                d.bodies[tr.id.value | (uint64_t(tr.id.tag) << 32)].push_back({w.BodyTimes(tr)[k], w.BodyFrames(tr)[k]});
            }
        }
    }
    return d;
}

std::wstring BinDir() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring s(self);
    return s.substr(0, s.find_last_of(L'\\'));
}

// `echodemo verify` on the file, as the CLI a user would run.
int RunVerify(const std::wstring& path) {
    std::wstring cmd = L"\"" + BinDir() + L"\\echodemo.exe\" verify \"" + path + L"\" --quiet";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

std::string VerifyOutput(const std::wstring& path) {
    const std::wstring cmd = L"\"\"" + BinDir() + L"\\echodemo.exe\" verify \"" + path + L"\"\"";
    std::string out;
    FILE* pipe = _wpopen(cmd.c_str(), L"r");
    if (!pipe) return out;
    char buf[512];
    while (fgets(buf, sizeof buf, pipe)) out += buf;
    _pclose(pipe);
    return out;
}

Result EmitTo(const Synth& s, const std::wstring& path, ConvertReport& rep, Progress* progress = nullptr) {
    ConvertOptions opt;
    rep = ConvertReport{};
    rep.sourceHash = 0x5EED;
    return Emit(s.m, s.t, kEmitFacts, opt, path.c_str(), rep, progress);
}

void Column(const float* q, int axis, float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    if (axis == 0) { out[0] = 1 - 2 * (y * y + z * z); out[1] = 2 * (x * y + w * z); out[2] = 2 * (x * z - w * y); }
    if (axis == 1) { out[0] = 2 * (x * y - w * z); out[1] = 1 - 2 * (x * x + z * z); out[2] = 2 * (y * z + w * x); }
    if (axis == 2) { out[0] = 2 * (x * z + w * y); out[1] = 2 * (y * z - w * x); out[2] = 1 - 2 * (x * x + y * y); }
}

double AngleDeg(const float* a, const float* b) {
    const double d = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
    return acos(fmax(-1.0, fmin(1.0, d))) * 57.29577951308232;
}

}  // namespace

TEST_CASE("P4 grid: 45 Hz ticks over the source, a > 250 ms gap skips frame indices") {
    Synth s(2.0, 30, {{import::kTeamBlue, 0, 99}});
    std::vector<TickFrame> grid;
    REQUIRE(BuildTickGrid(s.m, 45, grid).ok);
    CHECK(grid.size() == 89);  // 0 .. 1.9667 s at 22.222 ms
    for (const TickFrame& t : grid) {
        CHECK(t.alpha >= 0.0f);
        CHECK(t.alpha <= 1.0f);
        CHECK_FALSE(t.gapBefore);
        CHECK(s.m.frames[t.older].tUs <= t.tUs);
    }
    // Open a 400 ms hole after 1 s: no tick lands inside it, the one after it carries the GAP.
    for (SrcFrame& f : s.m.frames) {
        if (f.tUs > 1000000) f.tUs += 400000;
    }
    REQUIRE(BuildTickGrid(s.m, 45, grid).ok);
    size_t gaps = 0;
    for (size_t i = 0; i < grid.size(); ++i) {
        const int64_t t = grid[i].tUs;
        CHECK_FALSE((t > 1000000 && t < 1433333));
        if (grid[i].gapBefore) {
            ++gaps;
            CHECK(t >= 1433333);
            CHECK(t - grid[i - 1].tUs > kGapUs);
        }
    }
    CHECK(gaps == 1);
    CHECK_FALSE(BuildTickGrid(s.m, 0, grid).ok);
}

TEST_CASE("P4 resample: no overshoot into a pause, velocity is the curve's derivative, a teleport breaks") {
    Track t;
    const float q[4] = {0, 0, 0, 1};
    // Moves +x at 3 m/s, then holds (a pause repeats one position), then teleports 30 m.
    std::vector<float> xs;
    for (int i = 0; i < 10; ++i) xs.push_back(0.1f * i);
    for (int i = 0; i < 10; ++i) xs.push_back(0.9f);
    for (int i = 0; i < 5; ++i) xs.push_back(30.0f + 0.1f * i);
    for (uint32_t i = 0; i < xs.size(); ++i) {
        const float p[3] = {xs[i], 1, 0};
        TrackAdd(t, i, int64_t(i) * kStep30HzUs, p, q, i);
    }
    TrackFinish(t, kPlayerTeleportMps);
    CHECK(t.teleports == 1);
    CHECK(t.joinsNext[19] == 0);
    for (uint32_t i = 0; i + 1 < xs.size(); ++i) {
        for (float a = 0.0f; a < 1.0f; a += 0.125f) {
            TickFrame tick{int64_t(i) * kStep30HzUs + int64_t(a * kStep30HzUs), i, i + 1, a, false};
            TrackSample s;
            TrackEval(t, i, tick, s);
            const float lo = fminf(xs[i], xs[i + 1]), hi = fmaxf(xs[i], xs[i + 1]);
            if (t.joinsNext[i]) {
                CHECK(s.pos[0] >= lo - 1e-5f);
                CHECK(s.pos[0] <= hi + 1e-5f);
            } else {
                CHECK(s.pos[0] == xs[i]);  // held at a break, never swept across it
                CHECK(s.vel[0] == 0.0f);
            }
            if (i >= 10 && i < 19) CHECK(s.vel[0] == 0.0f);  // inside the pause
            if (i >= 1 && i < 8) CHECK(fabsf(s.vel[0] - 3.0f) < 0.01f);
        }
    }
    // The derivative matches the curve it came from: a finite difference of Hermite positions.
    Track c;
    for (uint32_t i = 0; i < 30; ++i) {
        const float p[3] = {float(sin(i * 0.2)), 0, 0};
        TrackAdd(c, i, int64_t(i) * 66667, p, q, i);
    }
    TrackFinish(c, kPlayerTeleportMps);
    for (uint32_t i = 2; i < 26; ++i) {
        TickFrame a{0, i, i + 1, 0.4f, false}, b{0, i, i + 1, 0.4001f, false};
        TrackSample sa, sb;
        TrackEval(c, i, a, sa);
        TrackEval(c, i, b, sb);
        const float fd = (sb.pos[0] - sa.pos[0]) / (0.0001f * 0.066667f);
        CHECK(fabsf(fd - sa.vel[0]) < 0.02f);
    }
}

TEST_CASE("P4 netvar names hash to the engine symbols the HUD reads") {
    CHECK(gametype::SymbolHash("game_timer") == capture_facts::kNvGameTimer);
    CHECK(gametype::SymbolHash("game_state") == capture_facts::kNvGameState);
    CHECK(gametype::SymbolHash("blue_score") == capture_facts::kNvBlueScore);
    CHECK(gametype::SymbolHash("red_score") == capture_facts::kNvRedScore);
    CHECK(gametype::SymbolHash("sudden_death_active") == capture_facts::kNvSuddenDeath);
    CHECK(gametype::SymbolHash("match_is_over") == capture_facts::kNvMatchIsOver);
    CHECK(gametype::SymbolHash("game_frisbee_ownerid") == capture_facts::kNvFrisbeeOwnerId);
}

TEST_CASE("P4 emit: a 60 s match with a joiner, a leaver and a spectator decodes through the viewer's reader") {
    // slot 0,1 blue; 2 orange; 3 spectator; 4 joins at 20 s; 1 leaves at 40 s.
    Synth s(60.0, 30, {{import::kTeamBlue, 0, 99}, {import::kTeamBlue, 0, 40}, {import::kTeamOrange, 0, 99}, {import::kTeamSpectator, 0, 99},
                       {import::kTeamOrange, 20, 99}});
    TempFile out(L"match");
    ConvertReport rep;
    const Result r = EmitTo(s, out.path, rep);
    REQUIRE_MESSAGE(r.ok, r.message);
    CHECK(rep.ticks == 2699);
    CHECK(rep.eventRows == 2);
    CHECK(rep.comRejected == 0);
    CHECK(rep.handOffsetClamped == 0);

    CHECK(RunVerify(out.path) == 0);

    const Decoded d = Decode(out.path.c_str());
    CHECK((d.flags & kFlagImported) != 0);
    CHECK((d.flags & kFlagOpen) == 0);
    CHECK((d.flags & (kFlagHasVoip | kFlagHasAsSeen | kFlagHasOracle)) == 0);
    REQUIRE(d.playerCount == 5);
    CHECK(d.records[3].teamId == import::kTeamSpectator);
    CHECK(strcmp(d.records[0].displayName, "synth\"0") == 0);
    CHECK(d.records[4].loadoutNumber == 14);
    CHECK(d.roundRows == 2);  // two RoundStart edges, derived by the real LocalMarkerSource

    CHECK(d.playerWire.read == rep.playerRows);
    CHECK(d.playerWire.keyed == rep.playerRows);
    CHECK(d.bodyWire.keyed == rep.bodyRows);
    CHECK(d.playerWire.dropped_verbatim + d.playerWire.dropped_identity + d.playerWire.dropped_clock == 0);
    CHECK(d.bodyWire.dropped_verbatim + d.bodyWire.dropped_identity + d.bodyWire.dropped_clock == 0);

    // Spectators have no body; a joiner starts at its join, a leaver's rows stop at its leave.
    CHECK(d.players.count(3) == 0);
    REQUIRE(d.players.count(4) == 1);
    REQUIRE(d.players.count(1) == 1);
    const uint64_t t0 = d.t0;
    CHECK(d.players.at(4).front().first >= t0 + 20000000);
    CHECK(d.players.at(4).front().first < t0 + 20100000);
    CHECK(d.players.at(1).back().first < t0 + 40000000);
    CHECK(d.players.at(1).back().first > t0 + 39900000);
    CHECK(d.players.at(0).size() == rep.ticks);

    // The disc is one component-id body; its owner is player 0 while held, nobody otherwise.
    const uint64_t discKey = (1ull << 32);
    REQUIRE(d.bodies.count(discKey) == 1);
    size_t owned = 0;
    for (const auto& [t, b] : d.bodies.at(discKey)) {
        const bool held = t >= t0 + 9000000 && t < t0 + 12000000;
        if (b.owner_slot == 0) ++owned;
        if (!held) CHECK(b.owner_slot == kBodyOwnerNone);
    }
    CHECK(owned > 100);

    // Decoded pose agrees with the source within the wire's own resolution.
    const auto& p0 = d.players.at(0);
    const auto& b0 = d.bodies.at(0);
    REQUIRE(b0.size() == p0.size());
    for (size_t k = 0; k < p0.size(); k += 97) {
        const double ts = double(p0[k].first - t0) * 1e-6;
        CHECK(fabs(b0[k].second.com[0] - 3 * cos(ts)) < 0.01);
        CHECK(fabs(b0[k].second.com[2] - 3 * sin(ts)) < 0.01);
        if (k > 0 && k + 1 < p0.size()) {
            CHECK(fabs(b0[k].second.vel[0] + 3 * sin(ts)) < 0.1);
            CHECK(fabs(b0[k].second.vel[2] - 3 * cos(ts)) < 0.1);
        }
        CHECK(fabs(p0[k].second.lHandOff[0] - 0.3f) < 0.002f);
        CHECK(fabs(p0[k].second.lHandOff[1] + 0.3f) < 0.002f);
        CHECK(p0[k].second.bits28 == 0x10u);
        CHECK(p0[k].second.holdL == (uint32_t(UINT16_MAX) | uint32_t(UINT16_MAX) << 16));
    }
}

TEST_CASE("P4 orientation fixtures (§5.4): all three columns survive the wire for head, hands and disc") {
    struct Fixture {
        const char* name;
        float forward[3], up[3], left[3], q[4];
    };
    const float h = 0.70710678f;
    const Fixture fixtures[] = {
        {"yaw +90", {1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {0, h, 0, h}},
        {"pitch down", {0, -1, 0}, {0, 0, 1}, {1, 0, 0}, {h, 0, 0, h}},
        {"roll", {0, 0, 1}, {-1, 0, 0}, {0, 1, 0}, {0, 0, h, h}},
    };
    for (const Fixture& fx : fixtures) {
        INFO(fx.name);
        Synth s(3.0, 30, {{import::kTeamBlue, 0, 99}});
        for (size_t i = 0; i < s.m.players.size(); ++i) {
            SrcPlayer& p = s.m.players[i];
            SetPose(p.head, 0, 1.5f, 0, fx.q);
            SetPose(p.body, 0, 1.5f, 0, fx.q);
            SetPose(p.lhand, 0.3f, 1.3f, 0.1f, fx.q);
            SetPose(p.rhand, -0.3f, 1.3f, 0.1f, fx.q);
            memset(p.vel, 0, sizeof p.vel);
        }
        for (SrcFrame& f : s.m.frames) SetPose(f.disc.pose, 1, 1, 1, fx.q);
        TempFile out(L"orient");
        ConvertReport rep;
        const Result r = EmitTo(s, out.path, rep);
        REQUIRE_MESSAGE(r.ok, r.message);
        const Decoded d = Decode(out.path.c_str());
        REQUIRE(d.players.count(0) == 1);
        const PlayerState& ps = d.players.at(0)[10].second;
        const BodyState& disc = d.bodies.at(1ull << 32)[10].second;
        const float* quats[4] = {ps.hmdOri, ps.lHandOri, ps.rHandOri, disc.ori};
        for (const float* q : quats) {
            float col[3];
            Column(q, 0, col);
            CHECK(AngleDeg(col, fx.left) < 0.5);
            Column(q, 1, col);
            CHECK(AngleDeg(col, fx.up) < 0.5);
            Column(q, 2, col);
            CHECK(AngleDeg(col, fx.forward) < 0.5);
        }
        // Hand offsets are world-axis hand minus head, not rotated into the head frame.
        CHECK(fabs(ps.lHandOff[0] - 0.3) < 0.001);
        CHECK(fabs(ps.lHandOff[1] + 0.2) < 0.001);
        CHECK(fabs(ps.lHandOff[2] - 0.1) < 0.001);
    }
}

TEST_CASE("P4 emit: a source gap becomes a frame-index GAP, nothing is drawn across it, verify lists it") {
    const int64_t holeFromUs = 10 * kStep30HzUs * 30, holeUs = 30 * kStep30HzUs;
    Synth s(20.0, 30, {{import::kTeamBlue, 0, 99}});
    for (SrcFrame& f : s.m.frames) {
        if (f.tUs > holeFromUs) f.tUs += holeUs;
    }
    TempFile out(L"gap");
    ConvertReport rep;
    const Result r = EmitTo(s, out.path, rep);
    REQUIRE_MESSAGE(r.ok, r.message);
    const Decoded d = Decode(out.path.c_str());
    REQUIRE(d.players.count(0) == 1);
    size_t after = 0;
    for (const auto& [t, ps] : d.players.at(0)) {
        const int64_t rel = int64_t(t - d.t0);
        CHECK_FALSE((rel > holeFromUs && rel < holeFromUs + holeUs));
        if (rel >= holeFromUs + holeUs) ++after;
    }
    CHECK(after * 3 > d.players.at(0).size());  // the ten seconds after the hole are all there
    // A legacy source's hole is known, not a failure of this file (R2): verify passes and names it.
    CHECK(RunVerify(out.path) == 0);
    const std::string said = VerifyOutput(out.path);
    CHECK(said.find("known import gap") != std::string::npos);
    CHECK_MESSAGE(said.find("FAIL:") == std::string::npos, said);
}

TEST_CASE("P4 emit is deterministic, refuses a mismatched input and leaves nothing on cancel") {
    Synth s(10.0, 15, {{import::kTeamBlue, 0, 99}, {import::kTeamOrange, 2, 99}});
    TempFile a(L"det_a"), b(L"det_b");
    ConvertReport rep;
    REQUIRE(EmitTo(s, a.path, rep).ok);
    REQUIRE(EmitTo(s, b.path, rep).ok);
    std::ifstream fa(a.path, std::ios::binary), fb(b.path, std::ios::binary);
    const std::string ba((std::istreambuf_iterator<char>(fa)), {}), bb((std::istreambuf_iterator<char>(fb)), {});
    CHECK(!ba.empty());
    CHECK(ba == bb);

    Synth bad = s;
    bad.t.samples.pop_back();
    TempFile c(L"bad");
    Result r = EmitTo(bad, c.path, rep);
    CHECK(r.code == ResultCode::BadArguments);
    CHECK(GetFileAttributesW(c.path.c_str()) == INVALID_FILE_ATTRIBUTES);

    Progress cancel;
    cancel.cancel = true;
    TempFile d(L"cancel");
    r = EmitTo(s, d.path, rep, &cancel);
    CHECK(r.code == ResultCode::Cancelled);
    CHECK(GetFileAttributesW(d.path.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("P4 IMPORTED guard (§7.7): kFlagImported is named only where provenance is written or shown") {
    namespace fs = std::filesystem;
    const fs::path root = REPLAY_SOURCE_DIR;
    const std::vector<std::string> allowed = {"src/foundation/demofile/format.h", "src/ui/panel.cpp",
                                              "src/import/emit.cpp", "tests/import/test_emit.cpp"};
    size_t scanned = 0;
    for (const char* top : {"src", "tools", "tests"}) {
        for (const auto& e : fs::recursive_directory_iterator(root / top)) {
            if (!e.is_regular_file()) continue;
            const std::string ext = e.path().extension().string();
            if (ext != ".h" && ext != ".cpp" && ext != ".inc") continue;
            ++scanned;
            std::ifstream in(e.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            if (text.find("kFlagImported") == std::string::npos) continue;
            const std::string rel = fs::relative(e.path(), root).generic_string();
            const bool ok = rel.rfind("tools/echodemo/", 0) == 0 || rel.rfind("src/foundation/demofile/writer", 0) == 0 ||
                            std::find(allowed.begin(), allowed.end(), rel) != allowed.end();
            INFO(rel);
            CHECK(ok);
        }
    }
    CHECK(scanned > 500);
}

TEST_CASE("P4 perf: a 10-minute, 30 Hz, 10-player match" * doctest::skip(true)) {
    std::vector<Presence> who;
    for (int i = 0; i < 10; ++i) who.push_back({uint8_t(i % 2), 0, 1e9});
    const auto build0 = std::chrono::steady_clock::now();
    Synth s(600.0, 30, who);
    const auto build1 = std::chrono::steady_clock::now();
    TempFile out(L"perf");
    ConvertReport rep;
    const Result r = EmitTo(s, out.path, rep);
    const auto emit1 = std::chrono::steady_clock::now();
    REQUIRE_MESSAGE(r.ok, r.message);
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    const double emitS = std::chrono::duration<double>(emit1 - build1).count();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    GetFileAttributesExW(out.path.c_str(), GetFileExInfoStandard, &fad);
    printf("P4-PERF source_build_s=%.3f emit_s=%.3f ticks=%u player_rows=%u body_rows=%u out_bytes=%llu "
           "peak_working_set_mb=%.1f peak_private_mb=%.1f\n",
           std::chrono::duration<double>(build1 - build0).count(), emitS, rep.ticks, rep.playerRows, rep.bodyRows,
           (unsigned long long)fad.nFileSizeLow + ((unsigned long long)fad.nFileSizeHigh << 32),
           pmc.PeakWorkingSetSize / 1048576.0, pmc.PeakPagefileUsage / 1048576.0);
    CHECK(emitS < 4.0);
    CHECK(RunVerify(out.path) == 0);
}
