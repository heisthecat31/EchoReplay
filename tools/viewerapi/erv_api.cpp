#define ERV_BUILD
#include "erv_api.h"

#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "capture/capture_types.h"
#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "playback/core/goalfeed_facts.h"
#include "playback/core/recorded_loadout.h"
#include "playback/core/stunfeed_facts.h"
#include "playback/core/windowbuild.h"

using namespace replay;

struct ErvFile {
    ErvInfo info{};
    std::vector<ErvPlayer> players;
    std::vector<ErvPlayerSample> ps;
    std::vector<ErvBodySample> bs;
    std::vector<ErvState> states;
    std::vector<ErvEvent> events;
    std::vector<ErvVoice> voices;
    std::vector<std::vector<uint8_t>> voiceBytes;
    struct RawRow { uint64_t keyUs, first, last, platform; uint32_t entrant, generation, team, active; char name[40]; };
    std::vector<RawRow> rosterRaw;
    std::vector<ErvRosterRow> roster;
};

namespace {

thread_local std::string g_err;

template <class T>
T Load(const uint8_t* p, uint32_t off) {
    T v{};
    memcpy(&v, p + off, sizeof v);
    return v;
}

const StreamView* Usable(const DecodedChapter& dc, uint16_t stream) {
    const StreamView* sv = dc.Find(stream);
    return sv && sv->records && sv->bytes > capture::kBlockPrefixBytes ? sv : nullptr;
}

bitio::BitReader ReaderOf(const StreamView& sv) {
    return bitio::BitReader(sv.records + capture::kBlockPrefixBytes, sv.bytes - capture::kBlockPrefixBytes);
}

uint64_t PlaceEvent(uint64_t arrival, uint64_t first, uint64_t last, uint64_t tBegin, uint64_t tEnd) {
    const uint64_t span = tEnd > tBegin ? tEnd - tBegin : 0;
    if (span == 0) return tBegin;
    const uint64_t arrivalSpan = last > first ? last - first : 0;
    if (arrivalSpan == 0) return tBegin + span / 2;
    const uint64_t d = arrival > first ? arrival - first : 0;
    const uint64_t t = tBegin + (d >= arrivalSpan ? span - 1 : (span * d) / arrivalSpan);
    return t >= tEnd ? tEnd - 1 : t;
}

void Copy(char* dst, size_t cap, const char* src, size_t srcCap) {
    size_t n = 0;
    while (n < srcCap && src[n]) ++n;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// kKeyBlockRoster: {u16 count}{ u64 platformCode, accountId, sessionId, firstSeenUs, lastSeenUs,
// u16 entrantIndex, generation, team, marker, u8 active, hasProfileJson, u16 nameLen, name,
// u16 platLen, plat }
void ReadRoster(const DecodedChapter& dc, std::vector<ErvPlayer>& players, std::vector<ErvFile::RawRow>* raw = nullptr,
                uint64_t keyUs = 0) {
    const StreamView* sv = dc.Find(kStreamKey);
    if (!sv || !sv->records || sv->bytes <= capture::kBlockPrefixBytes) return;
    static uint8_t blob[256 * 1024];
    bitio::BitReader r(sv->records + capture::kBlockPrefixBytes, sv->bytes - capture::kBlockPrefixBytes);
    uint16_t blobLen = 0;
    if (!capture::DecodeRawRecord(r, blob, sizeof blob, blobLen) || !blobLen) return;
    capture::KeyframeReader kr;
    if (!kr.Open(blob, blobLen)) return;
    uint16_t id = 0;
    const uint8_t* d = nullptr;
    uint32_t len = 0;
    while (kr.Next(id, d, len)) {
        if (id != capture::kKeyBlockRoster || len < 2) continue;
        uint16_t count = Load<uint16_t>(d, 0);
        uint32_t at = 2;
        for (uint16_t i = 0; i < count; ++i) {
            if (at + 40 + 8 + 2 + 2 > len) return;
            const uint64_t platform = Load<uint64_t>(d, at + 0);
            const uint64_t firstUs = Load<uint64_t>(d, at + 24), lastUs = Load<uint64_t>(d, at + 32);
            const uint16_t entrant = Load<uint16_t>(d, at + 40);
            const uint16_t generation = Load<uint16_t>(d, at + 42);
            const uint16_t team = Load<uint16_t>(d, at + 44);
            const uint8_t active = d[at + 48];
            at += 50;
            const uint16_t nameLen = Load<uint16_t>(d, at);
            at += 2;
            if (at + nameLen + 2 > len) return;
            std::string name(reinterpret_cast<const char*>(d + at), nameLen);
            at += nameLen;
            const uint16_t platLen = Load<uint16_t>(d, at);
            at += 2 + platLen;
            if (raw) {
                ErvFile::RawRow rr{keyUs, firstUs, lastUs, platform, entrant, generation, team, active, {}};
                Copy(rr.name, sizeof rr.name, name.c_str(), name.size());
                raw->push_back(rr);
            }
            if (!active && name.empty()) continue;
            ErvPlayer* hit = nullptr;
            for (ErvPlayer& p : players)
                if (p.slot == entrant) hit = &p;
            if (!hit) {
                players.push_back(ErvPlayer{});
                hit = &players.back();
                hit->slot = entrant;
                hit->team = 0xFFFF;
            }
            // the last team they PLAYED on: someone who left or went idle reads as no team / spectator at the
            // end of the file, which said nothing about the match they were in
            const bool playing = active && (team == 0 || team == 1);
            const bool hadTeam = hit->team == 0 || hit->team == 1;
            if (playing || !hadTeam) hit->team = team;
            if (!name.empty()) Copy(hit->name, sizeof hit->name, name.c_str(), name.size());
        }
    }
}

}  // namespace

extern "C" {

int erv_version(void) { return ERV_API_VERSION; }
const char* erv_last_error(void) { return g_err.c_str(); }

ErvFile* erv_open(const wchar_t* path) {
    g_err.clear();
    DemoReader r;
    if (!r.Open(path)) {
        g_err = r.RefusalReason() ? r.RefusalReason() : "cannot open";
        return nullptr;
    }
    auto* f = new ErvFile();
    const Manifest& m = r.GetManifest();
    f->info.level_id = m.levelId;
    f->info.gametype_id = m.gametypeId;
    f->info.t0_us = m.t0Us;
    f->info.imported = (r.FileFlags() & kFlagImported) ? 1 : 0;
    f->info.recovered = r.WasRecovered() ? 1 : 0;

    uint64_t begin = UINT64_MAX, end = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section != kSectionCore) continue;
        begin = std::min(begin, k.tBeginUs);
        end = std::max(end, k.tEndUs);
    }
    if (begin == UINT64_MAX) {
        g_err = "the file has no CORE chapters";
        delete f;
        return nullptr;
    }
    f->info.duration_s = (end - begin) / 1e6;
    auto T = [begin](uint64_t us) { return us >= begin ? (us - begin) / 1e6 : -((begin - us) / 1e6); };

    for (size_t i = 0; i < m.playerCount; ++i) {
        const PlayerRecord& p = m.players[i];
        ErvPlayer e{};
        e.slot = uint32_t(p.userSlot) | (uint32_t(p.generation) << 16);
        e.team = p.extended ? p.teamId16 : p.teamId;
        if (p.extended && p.displayNameFull[0]) Copy(e.name, sizeof e.name, p.displayNameFull, sizeof p.displayNameFull);
        else Copy(e.name, sizeof e.name, p.displayName, sizeof p.displayName);
        f->players.push_back(e);
    }

    playback::TrackScan scan;
    const char* why = nullptr;
    const bool haveClocks = playback::ScanTrackSources(r, scan, &why);

    namespace sf = replay::stunfeed_facts;
    namespace gf = replay::goalfeed_facts;
    const char* tapNames[] = {"SR15StunPlayerMsg", "SR15NetReliablePostScoreUpdate"};
    uint16_t tapIndex[2] = {capture::kTapTypeIndexNone, capture::kTapTypeIndexNone};
    static uint8_t keyblob[identity::kTapTableScratchBytes];
    static uint8_t body[capture::kMaxEventBodyBytes];
    static uint8_t scratch[512];
    static uint8_t voipBuf[sizeof(capture::VoipRec) + 0x10000];

    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey key = r.ChapterAt(i);
        DecodedChapter dc{};
        if (!r.DecodeChapter(i, dc)) continue;

        if (key.section == kSectionVoip) {
            const StreamView* v = Usable(dc, kStreamVoip);
            if (!v) continue;
            bitio::BitReader br = ReaderOf(*v);
            for (uint32_t n = 0; n < v->recordCount; ++n) {
                uint16_t len = 0;
                if (!capture::DecodeRawRecord(br, voipBuf, sizeof voipBuf, len) || len < sizeof(capture::VoipRec)) break;
                capture::VoipRec rec{};
                memcpy(&rec, voipBuf, sizeof rec);
                const uint32_t bytes = std::min<uint32_t>(rec.bodyBytes, len - uint32_t(sizeof rec));
                f->voiceBytes.emplace_back(voipBuf + sizeof rec, voipBuf + sizeof rec + bytes);
                ErvVoice e{};
                e.t = T(rec.tUs);
                e.slot = rec.speakerSlot;   // {slot, generation}
                e.bytes = bytes;
                f->voices.push_back(e);
            }
            continue;
        }
        if (key.section != kSectionCore) continue;
        ReadRoster(dc, f->players, &f->rosterRaw, key.tBeginUs);

        // players
        for (const bool sampled : {false, true}) {
            const StreamView* sv = Usable(dc, sampled ? kStreamPlayerSampled : kStreamPlayerWire);
            if (!sv) continue;
            bitio::BitReader br = ReaderOf(*sv);
            capture::PlayerDecoder dec(sampled);
            for (uint32_t n = 0; n < sv->recordCount; ++n) {
                capture::DecodedPlayer d{};
                if (!dec.Decode(br, d, scratch, sizeof scratch)) break;
                if (d.verbatim && (sampled || !playback::RecoverWireVerbatim(d))) continue;
                const capture::PlayerFrame& p = d.frame;
                const bool hasHeader = (p.fill & capture::kFillHeader) != 0;
                uint32_t slot = ERV_SLOT_LOCAL;   // the local per-frame sample: the recorder's own
                uint64_t t = d.stampUs;
                if (!sampled) {
                    const uint32_t user = uint32_t(p.hdr0);
                    if (!hasHeader || user == UINT32_MAX) continue;
                    slot = user;   // {slot, generation}
                    if (!haveClocks || !scan.clocks.ToCaptureUs(p.hdr0, d.stampUs, t)) continue;
                }
                ErvPlayerSample s{};
                s.t = T(t);
                s.slot = slot;
                memcpy(s.head_q, p.hmdOri, sizeof s.head_q);
                memcpy(s.l_q, p.lHandOri, sizeof s.l_q);
                memcpy(s.r_q, p.rHandOri, sizeof s.r_q);
                memcpy(s.l_off, p.lHandOff, sizeof s.l_off);
                memcpy(s.r_off, p.rHandOff, sizeof s.r_off);
                s.trig_l = p.trigL;
                s.trig_r = p.trigR;
                s.hold_l = p.holdLSlot;
                s.hold_r = p.holdRSlot;
                s.bits = p.bits28;
                s.sampled = sampled ? 1 : 0;
                f->ps.push_back(s);
            }
        }
        // bodies
        for (const bool sampled : {false, true}) {
            const StreamView* sv = Usable(dc, sampled ? kStreamBodySampled : kStreamBodyWire);
            if (!sv) continue;
            bitio::BitReader br = ReaderOf(*sv);
            capture::BodyDecoder dec(sampled);
            for (uint32_t n = 0; n < sv->recordCount; ++n) {
                capture::DecodedBody d{};
                if (!dec.Decode(br, d, scratch, sizeof scratch)) break;
                if (d.verbatim && (sampled || !playback::RecoverWireVerbatim(d))) continue;
                const capture::BodyFrame& b = d.frame;
                const bool hasHeader = (b.fill & capture::kFillBodyHeader) != 0;
                uint64_t t = d.stampUs;
                if (!sampled && (!hasHeader || !haveClocks || !scan.clocks.ToCaptureUs(b.hdr0, d.stampUs, t))) continue;
                uint32_t tag = b.idTag;
                uint64_t value = b.idValue;
                if (!sampled) {
                    if ((b.fill & capture::kFillBodyIdValue) == 0) continue;
                    tag = uint32_t(b.idValue >> 32);
                    if (tag < playback::kBodyIdTagNetIdMin) value = b.idValue & UINT32_MAX;
                } else if (!capture::BodyIdentityResolvable(b.fill)) {
                    if (b.idValue != 0) continue;
                    tag = ERV_TAG_LOCAL;   // no net identity: a locally simulated body (the recorder's own)
                    value = d.entity;
                }
                ErvBodySample s{};
                s.t = T(t);
                s.id_tag = tag;
                s.id_value = value;
                s.sampled = sampled ? 1 : 0;
                memcpy(s.com, b.com, sizeof s.com);
                memcpy(s.ori, b.ori, sizeof s.ori);
                memcpy(s.vel, b.vel, sizeof s.vel);
                f->bs.push_back(s);
            }
        }
        // game state
        if (const StreamView* sv = dc.Find(kStreamGameState); sv && sv->records) {
            const size_t rec = sizeof(GameStateRecordDisk);
            if (sv->bytes % rec == 0) {
                for (size_t k = 0; k < sv->bytes / rec; ++k) {
                    GameStateRecordDisk g;
                    memcpy(&g, sv->records + k * rec, rec);
                    ErvState s{};
                    s.t = T(g.captureTimeUs);
                    s.phase = g.phase;
                    s.round = g.roundCounter;
                    s.blue_points = g.bluePoints;
                    s.orange_points = g.redPoints;
                    s.disc_owner = g.frisbeeOwnerId;
                    s.game_timer = g.gameTimer;
                    s.countdown = g.countdownTimer;
                    f->states.push_back(s);
                }
            }
        }
        // events: stuns and goals
        for (int k = 0; k < 2; ++k) {
            const uint16_t here = identity::TapIndexForName(dc, tapNames[k], strlen(tapNames[k]), keyblob, sizeof keyblob);
            if (here != capture::kTapTypeIndexNone) tapIndex[k] = here;
        }
        if (const StreamView* sv = Usable(dc, kStreamEvent)) {
            bitio::BitReader br = ReaderOf(*sv);
            capture::EventDecoder dec;
            struct Row { uint64_t stamp; uint16_t type; std::vector<uint8_t> b; };
            std::vector<Row> rows;
            for (uint32_t n = 0; n < sv->recordCount; ++n) {
                capture::EventMessage em{};
                if (!dec.Decode(br, em, body, sizeof body)) break;
                if (em.typeIndex != tapIndex[0] && em.typeIndex != tapIndex[1]) continue;
                rows.push_back({em.stampUs, em.typeIndex, std::vector<uint8_t>(em.body, em.body + em.bodyBytes)});
            }
            if (!rows.empty()) {
                uint64_t first = rows[0].stamp, last = rows[0].stamp;
                for (const Row& e : rows) first = std::min(first, e.stamp), last = std::max(last, e.stamp);
                for (const Row& e : rows) {
                    ErvEvent ev{};
                    ev.t = T(PlaceEvent(e.stamp, first, last, key.tBeginUs, key.tEndUs));
                    const uint8_t* p = e.b.data();
                    const uint32_t nb = uint32_t(e.b.size());
                    if (e.type == tapIndex[0] && nb >= sf::kBodyBytes) {
                        ev.kind = ERV_EVENT_STUN;
                        ev.a = Load<uint32_t>(p, sf::kOffVictim);
                        ev.b = Load<uint32_t>(p, sf::kOffPuncher);
                        ev.f0 = Load<float>(p, sf::kOffDuration);
                    } else if (e.type == tapIndex[1] && nb >= gf::kBodyBytes) {
                        ev.kind = ERV_EVENT_GOAL;
                        ev.team = Load<uint16_t>(p, gf::kOffTeam);
                        ev.points = Load<uint16_t>(p, gf::kOffPoints);
                        ev.a = Load<uint32_t>(p, gf::kOffScorer);
                        ev.b = Load<uint32_t>(p, gf::kOffAssist);
                        ev.f0 = Load<float>(p, gf::kOffSpeed);
                        ev.f1 = Load<float>(p, gf::kOffDistance);
                        ev.goal_type = Load<uint16_t>(p, gf::kOffGoalType);
                    } else {
                        continue;
                    }
                    f->events.push_back(ev);
                }
            }
        }
    }
    for (size_t i = 0; i < f->voices.size(); ++i) f->voices[i].payload = f->voiceBytes[i].data();

    // v2 players: one per (entrant, generation) the lobby rows name, with the last team they played on.
    // ReadRoster's per-entrant list merged everyone who ever held a number; it is replaced here.
    {
        std::vector<ErvPlayer> byId;
        for (const ErvFile::RawRow& rr : f->rosterRaw) {
            if (!rr.name[0]) continue;
            const uint32_t id = uint32_t(rr.entrant) | (uint32_t(rr.generation) << 16);
            ErvPlayer* hit = nullptr;
            for (ErvPlayer& p : byId)
                if (p.slot == id) hit = &p;
            if (!hit) {
                byId.push_back(ErvPlayer{});
                hit = &byId.back();
                hit->slot = id;
                hit->team = 0xFFFF;
            }
            memcpy(hit->name, rr.name, sizeof hit->name);
            const bool playing = rr.active && (rr.team == 0 || rr.team == 1);
            if (playing || (hit->team != 0 && hit->team != 1)) hit->team = rr.team;
        }
        // manifest rows (already keyed by id) fill in anyone the keyframes never named
        for (const ErvPlayer& m : f->players) {
            bool have = false;
            for (const ErvPlayer& p : byId) have |= p.slot == m.slot;
            if (!have && m.name[0] && (m.slot >> 16) != 0) byId.push_back(m);
        }
        if (!byId.empty()) f->players = std::move(byId);
    }
    for (const ErvFile::RawRow& rr : f->rosterRaw) {
        ErvRosterRow o{};
        o.t = T(rr.keyUs);
        o.first_seen = rr.first ? T(rr.first) : 0;
        o.last_seen = rr.last ? T(rr.last) : 0;
        o.platform_id = rr.platform;
        o.entrant = rr.entrant, o.generation = rr.generation, o.team = rr.team, o.active = rr.active;
        memcpy(o.name, rr.name, sizeof o.name);
        f->roster.push_back(o);
    }
    auto byT = [](const auto& a, const auto& b) { return a.t < b.t; };
    std::stable_sort(f->ps.begin(), f->ps.end(), byT);
    std::stable_sort(f->bs.begin(), f->bs.end(), byT);
    std::stable_sort(f->states.begin(), f->states.end(), byT);
    std::stable_sort(f->events.begin(), f->events.end(), byT);
    // voices keep their recorded order (payload pointers index voiceBytes); sort a copy of the view
    std::stable_sort(f->voices.begin(), f->voices.end(), byT);

    f->info.player_count = uint32_t(f->players.size());
    f->info.player_sample_count = uint32_t(f->ps.size());
    f->info.body_sample_count = uint32_t(f->bs.size());
    f->info.state_count = uint32_t(f->states.size());
    f->info.event_count = uint32_t(f->events.size());
    f->info.voice_count = uint32_t(f->voices.size());
    return f;
}

void erv_close(ErvFile* f) { delete f; }

int erv_peek(const wchar_t* path, ErvInfo* out) {
    if (!out) return 0;
    *out = ErvInfo{};
    DemoReader r;
    if (!r.Open(path)) {
        g_err = r.RefusalReason() ? r.RefusalReason() : "cannot open";
        return 0;
    }
    const Manifest& m = r.GetManifest();
    out->level_id = m.levelId;
    out->gametype_id = m.gametypeId;
    out->t0_us = m.t0Us;
    out->player_count = m.playerCount;
    out->imported = (r.FileFlags() & kFlagImported) ? 1 : 0;
    out->recovered = r.WasRecovered() ? 1 : 0;
    uint64_t begin = UINT64_MAX, end = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section == kSectionVoip) out->voice_count++;
        if (k.section != kSectionCore) continue;
        begin = std::min(begin, k.tBeginUs);
        end = std::max(end, k.tEndUs);
    }
    out->duration_s = begin == UINT64_MAX ? 0 : (end - begin) / 1e6;
    return 1;
}

const ErvInfo* erv_info(const ErvFile* f) { return f ? &f->info : nullptr; }
const ErvPlayer* erv_players(const ErvFile* f) { return f ? f->players.data() : nullptr; }
const ErvPlayerSample* erv_player_samples(const ErvFile* f) { return f ? f->ps.data() : nullptr; }
const ErvBodySample* erv_body_samples(const ErvFile* f) { return f ? f->bs.data() : nullptr; }
const ErvState* erv_states(const ErvFile* f) { return f ? f->states.data() : nullptr; }
const ErvEvent* erv_events(const ErvFile* f) { return f ? f->events.data() : nullptr; }
const ErvVoice* erv_voices(const ErvFile* f) { return f ? f->voices.data() : nullptr; }
const ErvRosterRow* erv_roster(const ErvFile* f, uint32_t* count) {
    if (count) *count = f ? uint32_t(f->roster.size()) : 0;
    return f ? f->roster.data() : nullptr;
}

}  // extern "C"
