#include "replay.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace erv {
namespace {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::map<uint64_t, std::string>& Roster() {
    static std::map<uint64_t, std::string> r;
    static bool loaded = false;
    if (loaded) return r;
    loaded = true;
    std::ifstream f(std::filesystem::path(ERV_ECHOCONSTRUCT_DIR) / "registry" / "level_roster.tsv");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const size_t tab2 = line.find('\t', tab + 1);
        const uint64_t id = strtoull(line.c_str(), nullptr, 16);
        r[id] = line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
    }
    return r;
}

template <class K>
size_t Upper(const std::vector<K>& v, double t) {
    return size_t(std::upper_bound(v.begin(), v.end(), t, [](double x, const K& k) { return x < k.t; }) - v.begin());
}

float Mix(double a, double b, double t) { return b > a ? float((t - a) / (b - a)) : 0.f; }

// Wire body samples of a slot vs a local track: how far apart they are on average where both exist.
double MeanGap(const std::vector<BodyKey>& wire, const std::vector<BodyKey>& local) {
    if (wire.empty() || local.empty()) return 1e9;
    double sum = 0;
    int n = 0;
    const size_t step = std::max<size_t>(1, local.size() / 200);
    for (size_t i = 0; i < local.size(); i += step) {
        const size_t j = Upper(wire, local[i].t);
        if (j == 0 || j >= wire.size()) continue;
        if (wire[j].t - wire[j - 1].t > 0.5) continue;
        const V3 w = Lerp(wire[j - 1].com, wire[j].com, Mix(wire[j - 1].t, wire[j].t, local[i].t));
        sum += Len(w - local[i].com);
        ++n;
    }
    return n >= 5 ? sum / n : 1e9;
}

double TrackTravel(const std::vector<BodyKey>& k) {
    double d = 0;
    for (size_t i = 1; i < k.size(); ++i) d += Len(k[i].com - k[i - 1].com);
    return d;
}

bool LoadPart(const std::filesystem::path& file, Part& part, std::string& error) {
    ErvFile* f = erv_open(file.c_str());
    if (!f) {
        error = Narrow(file.filename().wstring()) + ": " + erv_last_error();
        return false;
    }
    const ErvInfo& info = *erv_info(f);
    part.path = file;
    part.level_id = info.level_id;
    part.gametype_id = info.gametype_id;
    part.level_name = LevelName(info.level_id);
    part.duration = info.duration_s;
    part.imported = info.imported != 0;
    part.voice_frames = info.voice_count;

    std::map<uint32_t, size_t> bySlot;
    auto track = [&](uint32_t slot) -> PlayerTrack& {
        auto it = bySlot.find(slot);
        if (it != bySlot.end()) return part.players[it->second];
        bySlot[slot] = part.players.size();
        part.players.push_back(PlayerTrack{});
        part.players.back().slot = slot;
        return part.players.back();
    };
    for (uint32_t i = 0; i < info.player_count; ++i) {
        const ErvPlayer& p = erv_players(f)[i];
        PlayerTrack& t = track(p.slot);
        if (p.name[0]) t.name = p.name;
        if (p.team != 0xFFFF) t.team = p.team;
    }

    // poses: a slot's own local samples when it has them (45 Hz), else its network records
    std::map<uint32_t, bool> hasSampled;
    for (uint32_t i = 0; i < info.player_sample_count; ++i)
        if (erv_player_samples(f)[i].sampled) hasSampled[erv_player_samples(f)[i].slot] = true;
    for (uint32_t i = 0; i < info.player_sample_count; ++i) {
        const ErvPlayerSample& s = erv_player_samples(f)[i];
        if (hasSampled[s.slot] && !s.sampled) continue;
        PoseKey k{};
        k.t = s.t;
        k.head_q = {s.head_q[0], s.head_q[1], s.head_q[2], s.head_q[3]};
        k.l_q = {s.l_q[0], s.l_q[1], s.l_q[2], s.l_q[3]};
        k.r_q = {s.r_q[0], s.r_q[1], s.r_q[2], s.r_q[3]};
        k.l_off = {s.l_off[0], s.l_off[1], s.l_off[2]};
        k.r_off = {s.r_off[0], s.r_off[1], s.r_off[2]};
        k.trig_l = s.trig_l;
        k.trig_r = s.trig_r;
        k.hold_l = s.hold_l;
        k.hold_r = s.hold_r;
        track(s.slot).pose.push_back(k);
    }

    // bodies: user bodies by slot (wire), local tracks by pool handle, components / net ids
    std::map<uint32_t, std::vector<BodyKey>> wireBodies;
    std::map<uint64_t, std::vector<BodyKey>> localBodies, otherBodies;
    for (uint32_t i = 0; i < info.body_sample_count; ++i) {
        const ErvBodySample& b = erv_body_samples(f)[i];
        BodyKey k{b.t, {b.com[0], b.com[1], b.com[2]}, {b.ori[0], b.ori[1], b.ori[2], b.ori[3]}, {b.vel[0], b.vel[1], b.vel[2]}};
        if (b.com[0] == 0 && b.com[1] == 0 && b.com[2] == 0) continue;   // an empty pool entry
        if (b.id_tag == 0) wireBodies[uint32_t(b.id_value)].push_back(k);   // {slot, generation}
        else if (b.id_tag == ERV_TAG_LOCAL) localBodies[b.id_value].push_back(k);
        else otherBodies[(uint64_t(b.id_tag) << 48) ^ b.id_value].push_back(k);
    }
    // Local tracks (the recorder's engine, 45 Hz) replace a player's network body where they follow it. The
    // engine reuses a local pool handle when one player leaves and another joins, so a track is matched per
    // player and only inside that player's own time on the wire, never glued on whole.
    std::map<uint64_t, size_t> usedKeys;
    for (auto& [slot, wire] : wireBodies) {
        const double lo = wire.front().t - 0.3, hi = wire.back().t + 0.3;
        double best = 0.6;
        uint64_t bestHandle = 0;
        std::vector<BodyKey> bestKeys;
        for (auto& [handle, keys] : localBodies) {
            const auto a = std::lower_bound(keys.begin(), keys.end(), lo, [](const BodyKey& k, double t) { return k.t < t; });
            const auto b = std::upper_bound(keys.begin(), keys.end(), hi, [](double t, const BodyKey& k) { return t < k.t; });
            if (b - a < 20) continue;
            std::vector<BodyKey> span(a, b);
            const double g = MeanGap(wire, span);
            if (g < best) best = g, bestHandle = handle, bestKeys = std::move(span);
        }
        if (!bestKeys.empty()) {
            usedKeys[bestHandle] += bestKeys.size();
            track(slot).body = std::move(bestKeys);   // higher rate than the wire
        } else {
            track(slot).body = wire;
        }
    }
    std::vector<uint64_t> unmatched;   // tracks no player's wire body explains: the recorder's own, or lobby players
    for (auto& [handle, keys] : localBodies)
        if (keys.size() > 20 && usedKeys[handle] < keys.size() * 3 / 10) unmatched.push_back(handle);
    // the recorder's first: the longest unexplained track
    std::sort(unmatched.begin(), unmatched.end(), [&](uint64_t a, uint64_t b) { return localBodies[a].size() > localBodies[b].size(); });
    for (auto u = unmatched.begin(); u != unmatched.end();) {   // the recorder's own: the posed player with no body
        bool used = false;
        for (PlayerTrack& p : part.players) {
            if (p.body.empty() && !p.pose.empty()) {
                p.body = localBodies[*u];
                used = true;
                break;
            }
        }
        u = used ? unmatched.erase(u) : u + 1;
    }
    // Bodies simulated locally with no net identity (lobbies record the others this way): matched to the
    // roster players left without one, in slot order. A guess, marked as such.
    if (wireBodies.empty()) {   // only a recording with no networked bodies at all (lobbies); elsewhere they are not players
        std::vector<PlayerTrack*> bare;
        for (PlayerTrack& p : part.players)
            if (p.body.empty() && p.team != 2) bare.push_back(&p);
        std::sort(bare.begin(), bare.end(), [](const PlayerTrack* a, const PlayerTrack* b) { return a->slot < b->slot; });
        for (size_t i = 0; i < bare.size() && i < unmatched.size(); ++i) {
            bare[i]->body = localBodies[unmatched[i]];
            bare[i]->guessed = true;
        }
    }
    // the disc: the component body that travels the most (arena), else the busiest other body
    double bestTravel = 0;
    for (auto& [key, keys] : otherBodies) {
        const double d = TrackTravel(keys) * ((key >> 48) == 1 ? 4.0 : 1.0);
        if (d > bestTravel) bestTravel = d, part.disc = keys;
    }

    for (PlayerTrack& p : part.players) {
        if (!p.pose.empty()) p.first = p.pose.front().t, p.last = p.pose.back().t;
        if (!p.body.empty()) p.first = std::min(p.first, p.body.front().t), p.last = std::max(p.last, p.body.back().t);
        if (p.slot == ERV_SLOT_LOCAL) p.name = RecorderName(), p.recorder = true;
        if (p.name.empty()) p.name = "Player " + std::to_string(p.slot & 0xFFFF);
    }
    // An id as an event or a voice frame carries it: exact {slot, generation}, else whoever held that slot then.
    auto indexOf = [&](uint32_t id, double t) -> int {
        if (id == 0xFFFFFFFFu) return -1;
        auto it = bySlot.find(id);
        if (it != bySlot.end()) return int(it->second);
        int best = -1;
        for (auto& [key, idx] : bySlot) {
            if ((key & 0xFFFF) != (id & 0xFFFF) || key == ERV_SLOT_LOCAL) continue;
            const PlayerTrack& p = part.players[idx];
            if (p.pose.empty() && p.body.empty()) continue;
            if (t >= p.first - 1 && t <= p.last + 1) return int(idx);
            best = int(idx);
        }
        return best;
    };
    for (uint32_t i = 0; i < info.voice_count; ++i) {
        const ErvVoice& v = erv_voices(f)[i];
        const int who = indexOf(v.slot, v.t);
        if (who >= 0) part.players[who].voice.push_back(v.t);
        const uint32_t slot = who >= 0 ? part.players[who].slot : v.slot;
        auto vt = std::find_if(part.voices.begin(), part.voices.end(), [&](const VoiceTrack& x) { return x.slot == slot; });
        if (vt == part.voices.end()) {
            part.voices.push_back(VoiceTrack{});
            vt = part.voices.end() - 1;
            vt->slot = slot;
        }
        vt->t.push_back(v.t);   // erv_voices is sorted by t already
        vt->off.push_back(uint32_t(vt->bytes.size()));
        if (v.payload && v.bytes) vt->bytes.insert(vt->bytes.end(), v.payload, v.payload + v.bytes);
    }
    for (VoiceTrack& vt : part.voices) {
        vt.off.push_back(uint32_t(vt.bytes.size()));
        auto it = bySlot.find(vt.slot);
        vt.name = it != bySlot.end() ? part.players[it->second].name : "Player " + std::to_string(vt.slot & 0xFFFF);
    }
    for (uint32_t i = 0; i < info.event_count; ++i) {
        const ErvEvent& e = erv_events(f)[i];
        Event ev{e.t, e.kind, indexOf(e.a, e.t), indexOf(e.b, e.t), e.team, e.points, e.f0, e.f1, e.goal_type};
        part.events.push_back(ev);
        if (e.kind == ERV_EVENT_STUN && ev.a >= 0)
            part.players[ev.a].stunned.push_back({e.t, e.t + std::max(0.5f, std::min(e.f0, 6.f))});
    }
    {
        uint32_t n = 0;
        const ErvRosterRow* rows = erv_roster(f, &n);
        for (uint32_t i = 0; rows && i < n; ++i)
            part.roster.push_back({rows[i].t, rows[i].entrant, rows[i].generation, rows[i].team, rows[i].active});
        std::stable_sort(part.roster.begin(), part.roster.end(), [](const RosterRow& a, const RosterRow& b) { return a.t < b.t; });
    }
    for (uint32_t i = 0; i < info.state_count; ++i) {
        const ErvState& s = erv_states(f)[i];
        part.states.push_back({s.t, s.phase, s.round, s.blue_points, s.orange_points, s.game_timer, s.countdown});
    }
    for (PlayerTrack& me : part.players) {
        if (!me.recorder || (me.team == 0 || me.team == 1) || me.body.empty()) continue;
        double close_to[2] = {0, 0};
        for (size_t k = 0; k < me.body.size(); k += 45) {
            const BodyKey& b = me.body[k];
            for (const PlayerTrack& o : part.players) {
                if (&o == &me || (o.team != 0 && o.team != 1) || o.body.empty()) continue;
                const size_t j = std::min(Upper(o.body, b.t), o.body.size() - 1);
                if (std::fabs(o.body[j].t - b.t) > 0.5) continue;
                const float d = Len(o.body[j].com - b.com);
                if (d < 4.f) close_to[o.team] += 4.f - d;
            }
        }
        if (close_to[0] + close_to[1] > 20 && std::max(close_to[0], close_to[1]) > 1.5 * std::min(close_to[0], close_to[1]))
            me.team = close_to[0] > close_to[1] ? 0 : 1, me.guessed = true;
    }
    // drop slots that carry nothing to draw
    part.players.erase(std::remove_if(part.players.begin(), part.players.end(),
                                      [](const PlayerTrack& p) { return p.body.empty(); }),
                       part.players.end());
    // indices in events referred to the unfiltered list; re-resolve by slot
    std::map<uint32_t, int> now;
    for (size_t i = 0; i < part.players.size(); ++i) now[part.players[i].slot] = int(i);
    std::vector<uint32_t> slotOf(bySlot.size() + 64, 0xFFFFFFFFu);
    for (auto& [slot, idx] : bySlot) if (idx < slotOf.size()) slotOf[idx] = slot;
    for (Event& e : part.events) {
        auto fix = [&](int& who) {
            if (who < 0) return;
            const uint32_t slot = slotOf[size_t(who)];
            auto it = now.find(slot);
            who = it == now.end() ? -1 : it->second;
        };
        fix(e.a);
        fix(e.b);
    }
    erv_close(f);
    return true;
}

}  // namespace

namespace {
std::string g_recorder = "You";
}
const std::string& RecorderName() { return g_recorder; }
void SetRecorderName(const std::string& n) { g_recorder = n.empty() ? "You" : n; }

std::string LevelName(uint64_t id) {
    auto& r = Roster();
    auto it = r.find(id);
    return it == r.end() ? std::string() : it->second;
}

std::string PrettyLevel(const std::string& n) {
    if (n.empty()) return "Unknown map";
    if (n == "mpl_arena_a") return "Arena";
    if (n.rfind("mpl_lobby", 0) == 0) return "Social Lobby";
    if (n.rfind("mpl_combat_", 0) == 0) {
        std::string s = n.substr(11);
        if (s.rfind("celebration_room", 0) == 0) return "Combat Celebration Room";
        s[0] = char(toupper(s[0]));
        return "Combat: " + s;
    }
    if (n.rfind("mpl_tutorial", 0) == 0) return "Tutorial";
    return n;
}

std::vector<std::pair<uint64_t, std::string>> KnownMaps() {
    std::vector<std::pair<uint64_t, std::string>> out;
    for (auto& [id, name] : Roster()) {
        if (name == "mpl_arena_a" || name == "mpl_lobby_b2" ||
            (name.rfind("mpl_combat_", 0) == 0 && name.find("celebration") == std::string::npos && name.find("war_room") == std::string::npos))
            out.push_back({id, name});
    }
    return out;
}

bool Replay::Load(const std::filesystem::path& file) {
    parts.clear();
    error.clear();
    duration = 0;
    std::vector<std::filesystem::path> files;
    std::wstring ext = file.extension().wstring();
    for (auto& c : ext) c = wchar_t(towlower(c));
    if (ext == L".json") {
        std::ifstream f(file);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string s = ss.str();
        size_t at = s.find("\"parts\"");
        if (at == std::string::npos) {
            error = "not an EchoReplay clip list";
            return false;
        }
        at = s.find('[', at);
        const size_t end = s.find(']', at);
        while (at != std::string::npos && at < end) {
            const size_t q1 = s.find('"', at + 1);
            if (q1 == std::string::npos || q1 > end) break;
            const size_t q2 = s.find('"', q1 + 1);
            const std::string name = s.substr(q1 + 1, q2 - q1 - 1);
            const int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
            std::wstring w(n > 0 ? n - 1 : 0, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, w.data(), n);
            files.push_back(file.parent_path() / w);
            at = q2 + 1;
        }
        title = "Clip " + Narrow(file.stem().wstring());
    } else {
        files.push_back(file);
        title = Narrow(file.stem().wstring());
    }
    for (const auto& p : files) {
        Part part;
        std::string why;
        if (!LoadPart(p, part, why)) {
            error = why;
            continue;
        }
        part.offset = duration;
        duration += part.duration;
        parts.push_back(std::move(part));
    }
    if (parts.empty() && error.empty()) error = "nothing to play";
    return !parts.empty();
}

int Replay::PartAt(double t) const {
    for (size_t i = 0; i < parts.size(); ++i)
        if (t < parts[i].offset + parts[i].duration || i + 1 == parts.size()) return int(i);
    return 0;
}

bool SamplePlayer(const PlayerTrack& p, double t, PlayerPose& o) {
    if (p.body.empty() || t < p.first - 0.05 || t > p.last + 0.05) return false;
    size_t j = Upper(p.body, t);
    const BodyKey& b0 = p.body[j == 0 ? 0 : j - 1];
    const BodyKey& b1 = p.body[j >= p.body.size() ? p.body.size() - 1 : j];
    if (b1.t - b0.t > 1.5 && t > b0.t + 0.2 && t < b1.t - 0.2) return false;   // a gap: not in the match then
    const float bt = Mix(b0.t, b1.t, t);
    o.head = Lerp(b0.com, b1.com, std::clamp(bt, 0.f, 1.f));
    o.vel = Lerp(b0.vel, b1.vel, std::clamp(bt, 0.f, 1.f));
    // EchoReplay's import facts (src/import/import_facts.h): the body record's ori is the FULL head basis (H3), while
    // the pose's hmdOri is the yaw-only body basis (H2) -- the upright frame the chassis hangs in.
    o.head_q = Slerp(b0.ori, b1.ori, std::clamp(bt, 0.f, 1.f));
    o.body_q = o.head_q;
    o.body_only = p.pose.empty();
    if (o.body_only) {   // the body's own orientation, hands at rest
        o.l_q = o.r_q = o.head_q;
        o.lhand = o.head + Rotate(o.head_q, V3{-0.22f, -0.42f, 0.12f});
        o.rhand = o.head + Rotate(o.head_q, V3{0.22f, -0.42f, 0.12f});
        o.trig_l = o.trig_r = 0;
        const auto v = std::upper_bound(p.voice.begin(), p.voice.end(), t);
        o.speaking = v != p.voice.begin() && t - *(v - 1) < 0.25;
        o.stunned = false;
        return true;
    }
    size_t k = Upper(p.pose, t);
    const PoseKey& a = p.pose[k == 0 ? 0 : k - 1];
    const PoseKey& c = p.pose[k >= p.pose.size() ? p.pose.size() - 1 : k];
    const float pt = std::clamp(Mix(a.t, c.t, t), 0.f, 1.f);
    o.body_q = Slerp(a.head_q, c.head_q, pt);   // PoseKey.head_q holds hmdOri: the yaw-only body basis
    o.l_q = Slerp(a.l_q, c.l_q, pt);
    o.r_q = Slerp(a.r_q, c.r_q, pt);
    o.lhand = o.head + Lerp(a.l_off, c.l_off, pt);
    o.rhand = o.head + Lerp(a.r_off, c.r_off, pt);
    o.trig_l = a.trig_l + (c.trig_l - a.trig_l) * pt;
    o.trig_r = a.trig_r + (c.trig_r - a.trig_r) * pt;
    const auto v = std::upper_bound(p.voice.begin(), p.voice.end(), t);
    o.speaking = v != p.voice.begin() && t - *(v - 1) < 0.25;
    o.stunned = false;
    for (const auto& s : p.stunned)
        if (t >= s.first && t <= s.second) o.stunned = true;
    return true;
}

bool SampleDisc(const Part& part, double t, V3& pos, Q4& ori, V3& vel) {
    if (part.disc.empty()) return false;
    size_t j = Upper(part.disc, t);
    const BodyKey& a = part.disc[j == 0 ? 0 : j - 1];
    const BodyKey& b = part.disc[j >= part.disc.size() ? part.disc.size() - 1 : j];
    const float u = std::clamp(Mix(a.t, b.t, t), 0.f, 1.f);
    pos = Lerp(a.com, b.com, u);
    ori = Slerp(a.ori, b.ori, u);
    vel = Lerp(a.vel, b.vel, u);
    return true;
}

int TeamAt(const Part& part, const PlayerTrack& track, double t) {
    const uint32_t entrant = track.slot & 0xFFFF, generation = track.slot >> 16;
    bool named = false;
    for (const RosterRow& r : part.roster) named = named || (r.entrant == entrant && r.generation == generation);
    if (!named || track.slot == ERV_SLOT_LOCAL) {
        if (track.body.empty() || t < track.first - 0.05 || t > track.last + 0.05) return -1;
        return track.team <= 2 ? int(track.team) : -1;
    }
    // the latest keyframe at or before t (the first one before the recording's first)
    auto it = std::upper_bound(part.roster.begin(), part.roster.end(), t, [](double v, const RosterRow& r) { return v < r.t; });
    const double kt = it == part.roster.begin() ? part.roster.front().t : (it - 1)->t;
    for (const RosterRow& r : part.roster) {
        if (r.t != kt || r.entrant != entrant || r.generation != generation) continue;
        return r.active && r.team <= 2 ? int(r.team) : -1;
    }
    return -1;
}

const State* StateAt(const Part& part, double t) {
    if (part.states.empty()) return nullptr;
    size_t j = Upper(part.states, t);
    return &part.states[j == 0 ? 0 : j - 1];
}

}  // namespace erv
