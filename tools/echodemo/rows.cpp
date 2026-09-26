#include "rows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <vector>

#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "foundation/gametype.h"
#include "playback/core/goalfeed_facts.h"
#include "playback/core/recorded_loadout.h"
#include "playback/core/stunfeed_facts.h"
#include "playback/core/windowbuild.h"

namespace replay::rows {

namespace {

using playback::WireClocks;

// Everything that did not become a row, printed to stderr so the CSV stays one table. A row that is
// silently absent would read as "the player did nothing" to every measurement downstream.
struct Census {
    uint32_t rows = 0;
    uint32_t filtered = 0;
    uint32_t decodeFailures = 0;
    uint32_t verbatimSkipped = 0;
    uint32_t noUser = 0;
    uint32_t noClock = 0;
    uint32_t blocksBad = 0;
    uint32_t chapterFaults = 0;
};

struct Range {
    uint64_t fromUs, toUs;
    bool Has(uint64_t t) const { return t >= fromUs && t <= toUs; }
};

struct Block {
    bitio::BitReader reader;
    uint32_t declaredBits = 0;

    explicit Block(const StreamView& sv)
        : reader(sv.records + capture::kBlockPrefixBytes, sv.bytes - capture::kBlockPrefixBytes) {
        memcpy(&declaredBits, sv.records, sizeof declaredBits);
    }
    bool Consumed() const {
        return playback::VerifyBlockConsumed(declaredBits, reader.BitsRead(), reader.Overrun()).ok;
    }
};

const StreamView* Usable(const DecodedChapter& dc, uint16_t stream) {
    const StreamView* sv = dc.Find(stream);
    return sv && sv->records && sv->bytes > capture::kBlockPrefixBytes ? sv : nullptr;
}

void Quat(const float* q) { printf(",%.6f,%.6f,%.6f,%.6f", q[0], q[1], q[2], q[3]); }
void Vec(const float* v) { printf(",%.5f,%.5f,%.5f", v[0], v[1], v[2]); }

// Wire rows are placed on the capture axis by the sender's measured offset, exactly as the playback
// window places them; sampled rows are stamped on it already.
bool PlaceRow(bool sampled, const WireClocks& clocks, bool hasHeader, uint64_t hdr0, uint64_t stampUs,
              uint64_t& tUs, Census& c) {
    if (sampled) {
        tUs = stampUs;
        return true;
    }
    if (!hasHeader || !clocks.ToCaptureUs(hdr0, stampUs, tUs)) {
        ++c.noClock;
        return false;
    }
    return true;
}

void PlayerRows(DemoReader& r, const WireClocks& clocks, Range range, Census& c) {
    printf("chapter,src,tUs,stampUs,slot,gen,hmdX,hmdY,hmdZ,hmdW,lqX,lqY,lqZ,lqW,rqX,rqY,rqZ,rqW,"
           "lOffX,lOffY,lOffZ,rOffX,rOffY,rOffZ,velX,velY,velZ,bendBack,bendFwd,restPoseHeight,"
           "trigL,trigR,bits28,holdLSlot,holdLGen,holdRSlot,holdRGen,anchor\n");
    static uint8_t scratch[512];
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter dc{};
        if (!r.DecodeChapter(i, dc)) {
            ++c.chapterFaults;
            continue;
        }
        for (const bool sampled : {false, true}) {
            const StreamView* sv = Usable(dc, sampled ? kStreamPlayerSampled : kStreamPlayerWire);
            if (!sv) continue;
            Block b(*sv);
            capture::PlayerDecoder dec(sampled);
            bool whole = true;
            for (uint32_t n = 0; n < sv->recordCount; ++n) {
                capture::DecodedPlayer d{};
                if (!dec.Decode(b.reader, d, scratch, sizeof scratch)) {
                    ++c.decodeFailures;
                    whole = false;
                    break;
                }
                if (d.verbatim && (sampled || !playback::RecoverWireVerbatim(d))) {
                    ++c.verbatimSkipped;
                    continue;
                }
                const capture::PlayerFrame& f = d.frame;
                const bool hasHeader = (f.fill & capture::kFillHeader) != 0;
                uint16_t slot = d.entity, gen = 0;
                if (!sampled) {
                    const uint32_t user = uint32_t(f.hdr0);
                    if (!hasHeader || user == UINT32_MAX) {
                        ++c.noUser;
                        continue;
                    }
                    slot = uint16_t(user);
                    gen = uint16_t(user >> 16);
                }
                uint64_t t = 0;
                if (!PlaceRow(sampled, clocks, hasHeader, f.hdr0, d.stampUs, t, c)) continue;
                if (!range.Has(t)) {
                    ++c.filtered;
                    continue;
                }
                printf("%zu,%s,%llu,%llu,%u,%u", i, sampled ? "sampled" : "wire", (unsigned long long)t,
                       (unsigned long long)d.stampUs, slot, gen);
                Quat(f.hmdOri);
                Quat(f.lHandOri);
                Quat(f.rHandOri);
                Vec(f.lHandOff);
                Vec(f.rHandOff);
                Vec(f.refVel);
                printf(",%.5f,%.5f,%.5f,%.3f,%.3f,0x%07X,%u,%u,%u,%u,%u\n", f.bendBack, f.bendFwd,
                       f.restPoseHeight, f.trigL, f.trigR, f.bits28, f.holdLSlot, f.holdLGen, f.holdRSlot,
                       f.holdRGen, f.anchor);
                ++c.rows;
            }
            if (whole && !b.Consumed()) ++c.blocksBad;
        }
    }
}

void BodyRows(DemoReader& r, const WireClocks& clocks, Range range, Census& c) {
    printf("chapter,src,tUs,stampUs,entity,owner,idTag,idValue,comX,comY,comZ,oriX,oriY,oriZ,oriW,"
           "velX,velY,velZ,angX,angY,angZ,lastHeldX,lastHeldY,lastHeldZ,bits11\n");
    static uint8_t scratch[512];
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter dc{};
        if (!r.DecodeChapter(i, dc)) {
            ++c.chapterFaults;
            continue;
        }
        for (const bool sampled : {false, true}) {
            const StreamView* sv = Usable(dc, sampled ? kStreamBodySampled : kStreamBodyWire);
            if (!sv) continue;
            Block b(*sv);
            capture::BodyDecoder dec(sampled);
            bool whole = true;
            for (uint32_t n = 0; n < sv->recordCount; ++n) {
                capture::DecodedBody d{};
                if (!dec.Decode(b.reader, d, scratch, sizeof scratch)) {
                    ++c.decodeFailures;
                    whole = false;
                    break;
                }
                if (d.verbatim && (sampled || !playback::RecoverWireVerbatim(d))) {
                    ++c.verbatimSkipped;
                    continue;
                }
                const capture::BodyFrame& f = d.frame;
                const bool hasHeader = (f.fill & capture::kFillBodyHeader) != 0;
                uint64_t t = 0;
                if (!PlaceRow(sampled, clocks, hasHeader, f.hdr0, d.stampUs, t, c)) continue;
                if (!range.Has(t)) {
                    ++c.filtered;
                    continue;
                }
                // A wire row's idValue is the whole tagged pair: tag in the high dword
                // (windowbuild.cpp BodyKeyOfWire); a sampled row carries its tag separately.
                uint32_t tag = f.idTag;
                uint64_t value = f.idValue;
                if (!sampled) {
                    tag = uint32_t(f.idValue >> 32);
                    if (tag < playback::kBodyIdTagNetIdMin) value = f.idValue & UINT32_MAX;
                }
                const uint32_t owner = hasHeader ? WireClocks::SenderOf(f.hdr0) : UINT32_MAX;
                printf("%zu,%s,%llu,%llu,%u,0x%08X,%u,0x%llX", i, sampled ? "sampled" : "wire",
                       (unsigned long long)t, (unsigned long long)d.stampUs, d.entity, owner, tag,
                       (unsigned long long)value);
                Vec(f.com);
                Quat(f.ori);
                Vec(f.vel);
                Vec(f.angVel);
                Vec(f.lastHeldCom);
                printf(",0x%03X\n", f.bits11);
                ++c.rows;
            }
            if (whole && !b.Consumed()) ++c.blocksBad;
        }
    }
}

enum class Tap : uint8_t { Stun, Goal, Stat, TeamStat, Count };
constexpr const char* kTapNames[] = {"SR15StunPlayerMsg", "SR15NetReliablePostScoreUpdate",
                                     "SR15NetReliableStatUpdate", "SR15NetReliableTeamStatUpdate"};
static_assert(sizeof kTapNames / sizeof kTapNames[0] == size_t(Tap::Count));

template <class T>
T Load(const uint8_t* p, uint32_t off) {
    T v{};
    memcpy(&v, p + off, sizeof v);
    return v;
}

void CsvText(const uint8_t* p, uint32_t n) {
    putchar('"');
    for (uint32_t i = 0; i < n && p[i] != 0; ++i) {
        const char ch = char(p[i]);
        if (ch == '"') fputs("\"\"", stdout);
        else putchar(ch == '\n' || ch == '\r' ? ' ' : ch);
    }
    putchar('"');
}

void Hex(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) printf("%02x", p[i]);
}

struct EventRow {
    uint64_t stampUs;
    uint16_t typeIndex;
    uint32_t bodyAt, bodyBytes;
};

// The stun and goal feeds place an event inside its chapter by where its arrival stamp falls in the
// chapter's arrival window (stunfeed.cpp RecordedStuns::EndChapter); rows uses the same rule so its
// times are the times playback fires them at.
uint64_t PlaceEvent(uint64_t arrival, uint64_t first, uint64_t last, uint64_t tBegin, uint64_t tEnd) {
    const uint64_t span = tEnd > tBegin ? tEnd - tBegin : 0;
    if (span == 0) return tBegin;
    const uint64_t arrivalSpan = last > first ? last - first : 0;
    if (arrivalSpan == 0) return tBegin + span / 2;
    const uint64_t d = arrival > first ? arrival - first : 0;
    const uint64_t t = tBegin + (d >= arrivalSpan ? span - 1 : (span * d) / arrivalSpan);
    return t >= tEnd ? tEnd - 1 : t;
}

void EventRows(DemoReader& r, Range range, Census& c) {
    printf("chapter,tUs,stampUs,tap,typeIndex,bytes,victim,puncher,duration,blocked,team,points,goalType,"
           "scorer,assist,speed,distance,possession,entrant,payload\n");
    namespace sf = replay::stunfeed_facts;
    namespace gf = replay::goalfeed_facts;
    static uint8_t keyblob[identity::kTapTableScratchBytes];
    static uint8_t body[capture::kMaxEventBodyBytes];
    uint16_t tapIndex[size_t(Tap::Count)];
    for (uint16_t& t : tapIndex) t = capture::kTapTypeIndexNone;
    std::vector<EventRow> rows;
    std::vector<uint8_t> bodies;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter dc{};
        if (!r.DecodeChapter(i, dc)) {
            ++c.chapterFaults;
            continue;
        }
        // The tap table rides the keyframe; a chapter without one keeps the last table seen.
        for (size_t k = 0; k < size_t(Tap::Count); ++k) {
            const uint16_t here =
                identity::TapIndexForName(dc, kTapNames[k], strlen(kTapNames[k]), keyblob, sizeof keyblob);
            if (here != capture::kTapTypeIndexNone) tapIndex[k] = here;
        }
        const StreamView* sv = Usable(dc, kStreamEvent);
        if (!sv) continue;
        Block b(*sv);
        capture::EventDecoder dec;
        rows.clear();
        bodies.clear();
        bool whole = true;
        for (uint32_t n = 0; n < sv->recordCount; ++n) {
            capture::EventMessage m{};
            if (!dec.Decode(b.reader, m, body, sizeof body)) {
                ++c.decodeFailures;
                whole = false;
                break;
            }
            rows.push_back({m.stampUs, m.typeIndex, uint32_t(bodies.size()), m.bodyBytes});
            bodies.insert(bodies.end(), m.body, m.body + m.bodyBytes);
        }
        if (whole && !b.Consumed()) ++c.blocksBad;
        if (rows.empty()) continue;
        uint64_t first = rows[0].stampUs, last = rows[0].stampUs;
        for (const EventRow& e : rows) {
            if (e.stampUs < first) first = e.stampUs;
            if (e.stampUs > last) last = e.stampUs;
        }
        const ChapterKey key = r.ChapterAt(i);
        for (const EventRow& e : rows) {
            const uint64_t t = PlaceEvent(e.stampUs, first, last, key.tBeginUs, key.tEndUs);
            if (!range.Has(t)) {
                ++c.filtered;
                continue;
            }
            size_t tap = size_t(Tap::Count);
            for (size_t k = 0; k < size_t(Tap::Count); ++k) {
                if (tapIndex[k] == e.typeIndex) tap = k;
            }
            const uint8_t* p = bodies.data() + e.bodyAt;
            printf("%zu,%llu,%llu,%s,%u,%u,", i, (unsigned long long)t, (unsigned long long)e.stampUs,
                   tap < size_t(Tap::Count) ? kTapNames[tap] : "", e.typeIndex, e.bodyBytes);
            if (tap == size_t(Tap::Stun) && e.bodyBytes >= sf::kBodyBytes) {
                printf("0x%08X,0x%08X,%.4f,%u,,,,,,,,,,", Load<uint32_t>(p, sf::kOffVictim),
                       Load<uint32_t>(p, sf::kOffPuncher), Load<float>(p, sf::kOffDuration),
                       Load<uint32_t>(p, sf::kOffFlag));
            } else if (tap == size_t(Tap::Goal) && e.bodyBytes >= gf::kBodyBytes) {
                printf(",,,,%u,%u,%u,0x%08X,0x%08X,%.4f,%.4f,%.4f,,", Load<uint16_t>(p, gf::kOffTeam),
                       Load<uint16_t>(p, gf::kOffPoints), Load<uint16_t>(p, gf::kOffGoalType),
                       Load<uint32_t>(p, gf::kOffScorer), Load<uint32_t>(p, gf::kOffAssist),
                       Load<float>(p, gf::kOffSpeed), Load<float>(p, gf::kOffDistance),
                       Load<float>(p, gf::kOffPossession));
            } else if (tap == size_t(Tap::Stat) && e.bodyBytes >= sizeof(uint32_t)) {
                printf(",,,,,,,,,,,,0x%08X,", Load<uint32_t>(p, 0));
                CsvText(p + sizeof(uint32_t), e.bodyBytes - uint32_t(sizeof(uint32_t)));
            } else if (tap == size_t(Tap::TeamStat)) {
                printf(",,,,,,,,,,,,,");
                Hex(p, e.bodyBytes);
            } else {
                printf(",,,,,,,,,,,,,");
            }
            putchar('\n');
            ++c.rows;
        }
    }
}

// The netvars DESIGN-FORMATS 3.3 names, hashed by the engine's own symbol routine. netvarhud.cpp's
// table holds the same symbols but drags the engine-facing HUD code into a file tool.
struct NetvarName {
    uint64_t hash;
    const char* name;
    bool real;
};

constexpr NetvarName kNetvarNames[] = {
    {gametype::SymbolHash("game_timer"), "game_timer", true},
    {gametype::SymbolHash("countdown_timer"), "countdown_timer", true},
    {gametype::SymbolHash("game_state"), "game_state", false},
    {gametype::SymbolHash("blue_score"), "blue_score", false},
    {gametype::SymbolHash("red_score"), "red_score", false},
    {gametype::SymbolHash("blue_round_score"), "blue_round_score", false},
    {gametype::SymbolHash("red_round_score"), "red_round_score", false},
    {gametype::SymbolHash("sudden_death_active"), "sudden_death_active", false},
    {gametype::SymbolHash("match_is_over"), "match_is_over", false},
    {gametype::SymbolHash("game_frisbee_ownerid"), "game_frisbee_ownerid", false},
};
static_assert(kNetvarNames[0].hash == 0xB2996AD484F1D1E8ull, "game_timer as netvarhud.cpp records it");
static_assert(kNetvarNames[3].hash == 0x1C97052A21E09F11ull, "blue_score as netvarhud.cpp records it");

void NetvarRows(DemoReader& r, Range range, Census& c) {
    printf("chapter,tUs,stamped,slot,name,hash,raw,value\n");
    const bool stamped = playback::NetvarStampsPresent(r.GetManifest());
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter dc{};
        if (!r.DecodeChapter(i, dc)) {
            ++c.chapterFaults;
            continue;
        }
        const StreamView* sv = Usable(dc, kStreamNetvar);
        if (!sv) continue;
        const ChapterKey key = r.ChapterAt(i);
        Block b(*sv);
        capture::NetvarDecoder dec(stamped);
        bool whole = true;
        for (uint32_t n = 0; n < sv->recordCount; ++n) {
            static capture::NetvarMessage m;
            memset(&m, 0, sizeof m);
            if (!dec.Decode(b.reader, m)) {
                ++c.decodeFailures;
                whole = false;
                break;
            }
            // Clamped into the chapter it was filed in, as windowbuild does; an unstamped file only
            // knows the chapter.
            uint64_t t = key.tBeginUs;
            if (stamped) t = m.stampUs < key.tBeginUs ? key.tBeginUs : m.stampUs > key.tEndUs ? key.tEndUs : m.stampUs;
            if (!range.Has(t)) {
                c.filtered += m.varCount;
                continue;
            }
            for (uint16_t v = 0; v < m.varCount; ++v) {
                const capture::NetvarRow& row = m.rows[v];
                const NetvarName* known = nullptr;
                for (const NetvarName& nn : kNetvarNames) {
                    if (nn.hash == row.name) known = &nn;
                }
                const uint32_t low = uint32_t(row.value);
                printf("%zu,%llu,%d,%u,%s,0x%016llX,0x%016llX,", i, (unsigned long long)t, stamped ? 1 : 0,
                       m.slot, known ? known->name : "", (unsigned long long)row.name,
                       (unsigned long long)row.value);
                if (known && known->real) {
                    float f;
                    memcpy(&f, &low, sizeof f);
                    printf("%.4f\n", f);
                } else {
                    printf("%d\n", int32_t(low));
                }
                ++c.rows;
            }
        }
        if (whole && !b.Consumed()) ++c.blocksBad;
    }
}

void GameStateRows(DemoReader& r, Range range, Census& c) {
    printf("chapter,tUs,engineTimeUs,present,phase,availability,phaseSymbol,roundCounter,bluePoints,redPoints,"
           "blueMatchScore,redMatchScore,frisbeeOwnerId,gameTimer,countdownTimer,lastScoreTeam,lastScorePoints\n");
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter dc{};
        if (!r.DecodeChapter(i, dc)) {
            ++c.chapterFaults;
            continue;
        }
        const StreamView* sv = dc.Find(kStreamGameState);
        if (!sv || !sv->records) continue;
        const size_t rec = sizeof(GameStateRecordDisk);
        if (sv->bytes % rec != 0) {
            ++c.blocksBad;
            continue;
        }
        for (size_t k = 0; k < sv->bytes / rec; ++k) {
            GameStateRecordDisk d;
            memcpy(&d, sv->records + k * rec, rec);
            if (!range.Has(d.captureTimeUs)) {
                ++c.filtered;
                continue;
            }
            printf("%zu,%llu,%llu,0x%X,%u,%u,0x%016llX,%u,%u,%u,%u,%u,0x%08X,%.4f,%.4f,%u,%u\n", i,
                   (unsigned long long)d.captureTimeUs, (unsigned long long)d.engineTimeUs, d.present, d.phase,
                   d.availability, (unsigned long long)d.phaseSymbol, d.roundCounter, d.bluePoints,
                   d.redPoints, d.blueMatchScore, d.redMatchScore, d.frisbeeOwnerId, d.gameTimer,
                   d.countdownTimer, d.lastScoreTeam, d.lastScorePoints);
            ++c.rows;
        }
    }
}

// The join key from a row's slot to the manifest roster, without names or account ids.
void RosterRows(DemoReader& r, Census& c) {
    printf("index,userSlot,generation,team,alignment\n");
    const Manifest& m = r.GetManifest();
    for (size_t i = 0; i < m.playerCount; ++i) {
        const PlayerRecord& p = m.players[i];
        printf("%zu,%u,%u,%u,%u\n", i, p.userSlot, p.generation, p.teamId16, p.alignment16);
        ++c.rows;
    }
}

bool Eq(const wchar_t* a, const wchar_t* b) { return wcscmp(a, b) == 0; }

}  // namespace

int CmdRows(const wchar_t* path, What what, uint64_t fromUs, uint64_t toUs) {
    DemoReader r;
    if (!r.Open(path)) {
        fprintf(stderr, "REFUSED: %s\n", r.RefusalReason());
        return 2;
    }
    static char outBuf[1 << 20];
    setvbuf(stdout, outBuf, _IOFBF, sizeof outBuf);

    playback::TrackScan scan;
    if (what == What::Player || what == What::Body) {
        const char* why = nullptr;
        if (!playback::ScanTrackSources(r, scan, &why)) {
            fprintf(stderr, "REFUSED: the wire clock scan failed: %s\n", why ? why : "no reason given");
            return 2;
        }
    }
    const Range range{fromUs, toUs};
    Census c;
    switch (what) {
        case What::Player: PlayerRows(r, scan.clocks, range, c); break;
        case What::Body: BodyRows(r, scan.clocks, range, c); break;
        case What::Event: EventRows(r, range, c); break;
        case What::Netvar: NetvarRows(r, range, c); break;
        case What::GameState: GameStateRows(r, range, c); break;
        case What::Roster: RosterRows(r, c); break;
    }
    fflush(stdout);
    fprintf(stderr,
            "ROWS %u filtered=%u decode_failures=%u blocks_bad=%u chapter_faults=%u verbatim_skipped=%u "
            "no_user=%u no_clock=%u\n",
            c.rows, c.filtered, c.decodeFailures, c.blocksBad, c.chapterFaults, c.verbatimSkipped, c.noUser,
            c.noClock);
    return c.decodeFailures || c.blocksBad || c.chapterFaults ? 1 : 0;
}

int RunRows(const wchar_t* path, int argc, wchar_t** argv) {
    bool haveWhat = false;
    What what = What::Player;
    uint64_t fromUs = 0, toUs = UINT64_MAX;
    for (int i = 0; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (Eq(argv[i], L"--what") && hasValue) {
            const wchar_t* w = argv[++i];
            haveWhat = true;
            if (Eq(w, L"player")) what = What::Player;
            else if (Eq(w, L"body")) what = What::Body;
            else if (Eq(w, L"event")) what = What::Event;
            else if (Eq(w, L"netvar")) what = What::Netvar;
            else if (Eq(w, L"gamestate")) what = What::GameState;
            else if (Eq(w, L"roster")) what = What::Roster;
            else return -1;
        } else if (Eq(argv[i], L"--from") && hasValue) {
            fromUs = uint64_t(_wtof(argv[++i]) * 1e6);
        } else if (Eq(argv[i], L"--to") && hasValue) {
            toUs = uint64_t(_wtof(argv[++i]) * 1e6);
        } else if (Eq(argv[i], L"--pseudonymise") || Eq(argv[i], L"--raw")) {
            // Privacy switches are read by wmain; rows prints no names or account ids.
        } else {
            return -1;
        }
    }
    if (!haveWhat) return -1;
    return CmdRows(path, what, fromUs, toUs);
}

}  // namespace replay::rows
