// echodemo - the offline tool for .echodemo files (design S13.4, W1-B).
//
//   info    header, sections, rounds, players, and the derived index
//   dump    chapters, stream blocks and their completeness counters
//   verify  the structural and completeness laws, with a named failure for each
//   strip   rewrite without a section; --voip is the privacy control of S5.4.6
//   fuzz    damage a copy and assert the recovery path behaves, over a stated corpus

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "capture/keyframe.h"
#include "capture/roster.h"      // RecordTeamAgreesWithFill: the writer's rule, not a copy
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "import/convert.h"
#include "rows.h"

using namespace replay;

namespace {

// ---------------------------------------------------------------------------------------------
// Display names, platform strings and account ids are PERSONAL DATA, and this tool's output is read by
// the harness, archived with a run and quoted into reports. The harness already solved this for the
// endpoints it logs (the test harness Get-EndpointAlias / Test-EndpointHostIsReserved): one pseudonym
// per distinct sensitive value, stable for the life of ONE process, meaningless outside it, and never
// reused -- out of pseudonyms it falls back to a marker, because two players rendered as one name would
// be a false fact. This is that rule with that shape, over player identities instead of hosts.
//
// The per-run salt is what stops two runs over the same file being comparable: the sequence number is
// first-seen order, so it is stable within the run, and the salt rotates where the sequence starts.
// ECHOREPLAY_PSEUDONYM_SALT pins it so a test can assert the mapping; nothing ever prints the map,
// because the map is the thing that would undo the redaction.
constexpr uint64_t kPseudonymSpace = 100;
const char* const kPseudonymExhausted = "player-unnamed";

bool g_pseudonymise = false;
uint64_t g_pseudonymSalt = 0;
std::vector<std::pair<std::string, std::string>> g_aliases;

std::string PersonalAlias(const std::string& raw) {
    for (const std::pair<std::string, std::string>& kv : g_aliases) {
        if (kv.first == raw) {
            return kv.second;
        }
    }
    if (g_aliases.size() >= kPseudonymSpace) {
        return std::string(kPseudonymExhausted);
    }
    // Distinct within the run: kPseudonymSpace consecutive sequence numbers are distinct modulo it.
    const uint64_t nth = (g_pseudonymSalt + g_aliases.size()) % kPseudonymSpace;
    char buf[16];
    sprintf_s(buf, "player%02u", static_cast<unsigned>(nth));
    g_aliases.push_back(std::make_pair(raw, std::string(buf)));
    return g_aliases.back().second;
}

// The manifest's char fields are NUL-padded, not NUL-guaranteed.
std::string Personal(const char* field, size_t cap) {
    size_t len = 0;
    while (len < cap && field[len]) {
        ++len;
    }
    if (len == 0) {
        return std::string("(none)");
    }
    const std::string raw(field, len);
    return g_pseudonymise ? PersonalAlias(raw) : raw;
}

std::string PersonalId(uint64_t id) {
    if (id == 0) {
        return std::string("(none)");
    }
    char hex[24];
    sprintf_s(hex, "%016llX", static_cast<unsigned long long>(id));
    return g_pseudonymise ? PersonalAlias(hex) : std::string(hex);
}

// ON because of the environment, not because of the switch: a guard keyed on a switch's PRESENCE is
// vacuous when nobody passed the switch. Under the harness --raw is REFUSED by name rather than
// ignored, so a caller that asked for personal data in archived output gets an answer, not silence.
bool InitPrivacy(bool wantPseudonymise, bool wantRaw) {
    wchar_t probe[2];
    SetLastError(ERROR_SUCCESS);
    GetEnvironmentVariableW(L"ECHOREPLAY_HARNESS", probe, 2);
    const bool harness = GetLastError() != ERROR_ENVVAR_NOT_FOUND;

    if (harness && wantRaw) {
        printf("REFUSED: --raw is not available while ECHOREPLAY_HARNESS is set; this output is "
               "archived with the run\n");
        return false;
    }
    g_pseudonymise = harness || wantPseudonymise;
    if (!g_pseudonymise) {
        return true;
    }
    wchar_t pinned[32];
    if (GetEnvironmentVariableW(L"ECHOREPLAY_PSEUDONYM_SALT", pinned, 32)) {
        g_pseudonymSalt = _wcstoui64(pinned, nullptr, 0);
    } else {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        g_pseudonymSalt = static_cast<uint64_t>(qpc.QuadPart) ^ GetTickCount64() ^
                          (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    }
    return true;
}

// "-" is the absence value rendered, never a number a reader could mistake for a team.
const char* Num16(char* buf, size_t cap, uint16_t v, uint16_t none) {
    if (v == none) {
        strcpy_s(buf, cap, "-");
    } else {
        sprintf_s(buf, cap, "%u", v);
    }
    return buf;
}

// "-" for the absence value, so a stat the capture never sourced can never be read as a zero.
const char* StatNum(char* buf, size_t cap, int32_t v) {
    if (v == kPlayerStatNone) {
        strcpy_s(buf, cap, "-");
    } else {
        sprintf_s(buf, cap, "%d", v);
    }
    return buf;
}

const char* StatFloat(char* buf, size_t cap, float v) {
    if (v == kPlayerStatFloatNone) {
        strcpy_s(buf, cap, "-");
    } else {
        sprintf_s(buf, cap, "%.3f", v);
    }
    return buf;
}

// The scoreboard as it stood when the file closed: the last S_PLAYER_STATS record for each user. The
// name comes from the roster row with the same account id and goes through the same pseudonymiser the
// roster does, so `info` cannot leak a name here that it hides two lines above.
void PrintPlayerStatsRow(const Manifest& m, const PlayerStatsRecordDisk& d) {
    std::string who = PersonalId(d.xplatformId);
    for (uint16_t i = 0; i < m.playerCount; ++i) {
        if (m.players[i].xplatformId == d.xplatformId && d.xplatformId != 0) {
            who = Personal(m.players[i].displayNameFull, sizeof(m.players[i].displayNameFull));
            break;
        }
    }
    char pos[16], pts[12], gls[12], stn[12], blk[12], ast[12], sav[12];
    char itc[12], stl[12], cat[12], pas[12], sht[12], png[12], los[16];
    printf("    %-16s pos %-8s pts %-4s g %-4s stn %-4s blk %-4s ast %-4s sv %-4s int %-4s stl %-4s "
           "cat %-4s pas %-4s sht %-4s ping %-5s loss %-7s emote %s fill %04X\n",
           who.c_str(), StatFloat(pos, sizeof pos, d.possessionTime),
           StatNum(pts, sizeof pts, d.points), StatNum(gls, sizeof gls, d.goals),
           StatNum(stn, sizeof stn, d.stuns), StatNum(blk, sizeof blk, d.blocks),
           StatNum(ast, sizeof ast, d.assists), StatNum(sav, sizeof sav, d.saves),
           StatNum(itc, sizeof itc, d.interceptions), StatNum(stl, sizeof stl, d.steals),
           StatNum(cat, sizeof cat, d.catches), StatNum(pas, sizeof pas, d.passes),
           StatNum(sht, sizeof sht, d.shotsTaken), StatNum(png, sizeof png, d.ping),
           StatFloat(los, sizeof los, d.packetLossRatio),
           d.isEmotePlaying == kPlayerStatEmoteNone ? "-" : (d.isEmotePlaying ? "yes" : "no"),
           d.fill);
}

void PrintPlayerRow(uint16_t i, const PlayerRecord& p) {
    char team[8], slot[8], align[8];
    const std::string name = Personal(p.displayNameFull, sizeof(p.displayNameFull));
    const std::string plat = Personal(p.platformStrFull, sizeof(p.platformStrFull));
    printf("    [%2u] %-16s plat %-16s xplat %-16s team %-3s slot %-3s align %-3s loadout %u/%u "
           "fill %04X%s json %u%s/%u%s\n",
           i, name.c_str(), plat.c_str(), PersonalId(p.xplatformId).c_str(),
           Num16(team, sizeof team, p.teamId16, kPlayerNoTeam16),
           Num16(slot, sizeof slot, p.teamSlot16, kPlayerNoTeam16),
           Num16(align, sizeof align, p.alignment16, kPlayerNoAlignment16), p.loadoutNumber,
           p.loadoutNumberBody, p.fill, p.extended ? "" : " (not recorded)", p.entrantJson.n,
           (p.jsonTruncated & kPlayerJsonTruncEntrant) ? "T" : "", p.unlocksJson.n,
           (p.jsonTruncated & kPlayerJsonTruncUnlocks) ? "T" : "");
}
// ---------------------------------------------------------------------------------------------

// How far a stream's own record rate may move between adjacent chapters before verify calls it a step.
//
// S5.6.2's named mutation is "halving the capture's sample rate", which is a ratio of exactly 2.0 -- a
// strict `> 2.0` test is calibrated out of the one case it exists to catch. The band is set below that
// and above the largest legitimate swing the design describes: S5.4.3 puts the local sim/render rate at
// 72/80/90 Hz, so 90/72 = 1.25x is a chapter-to-chapter change that means nothing. This is a bound on
// how much a stream's rate may CHANGE, not an assumed rate; S5.4.2's "the format never encodes a rate
// assumption" is intact.
constexpr double kRateStepLimit = 1.4;

struct StreamTally {
    uint16_t stream = 0;
    uint64_t observed = 0;
    uint64_t stored = 0;
    uint64_t accountedGaps = 0;
    uint32_t blocks = 0;
    bool everDiverged = false;
    // Self-calibrating record width. The container cannot derive a record's byte size -- records are
    // bit-packed by the codec -- so it learns the width from the stream's own first block and holds the
    // stream to it. That is what relates `stored` to `byteLen`, which are otherwise two independent
    // assertions by the same caller.
    uint32_t recordBytes = 0;
    bool widthKnown = false;
    bool variableWidth = false;
    uint32_t widthChecked = 0;
};

// The reference is valid only until the next call: a new stream appends and can reallocate.
StreamTally& TallyFor(std::vector<StreamTally>& t, uint16_t stream) {
    for (StreamTally& x : t) {
        if (x.stream == stream) {
            return x;
        }
    }
    t.push_back(StreamTally{});
    t.back().stream = stream;
    return t.back();
}

int RetentionOf(const Manifest& m, uint16_t schemaId, bool* known) {
    for (uint16_t i = 0; i < m.schemaCount; ++i) {
        if (m.schemas[i].id == schemaId) {
            *known = true;
            return static_cast<int>(m.schemas[i].retention);
        }
    }
    *known = false;
    return -1;
}

void PrintFlags(uint64_t f) {
    printf("  flags            :");
    if (f & kFlagOpen) printf(" OPEN");
    if (f & kFlagHasVoip) printf(" HAS_VOIP");
    if (f & kFlagHasAsSeen) printf(" HAS_ASSEEN");
    if (f & kFlagHasOracle) printf(" HAS_ORACLE");
    if (f & kFlagImported) printf(" IMPORTED");
    if (f & kFlagTruncatedByBudget) printf(" TRUNCATED_BY_BUDGET");
    if (f == 0) printf(" (none)");
    printf("\n");
}

// Names the set bits of a present mask. An empty set prints "(none)" rather than a bare label, so a
// reader can tell "no field answered" from a line the tool forgot to write.
void PrintFieldSet(const char* label, uint32_t mask) {
    printf("%s", label);
    if ((mask & kGameStatePresentAll) == 0) {
        printf(" (none)\n");
        return;
    }
    for (unsigned bit = 0; bit < kGameStateFieldCount; ++bit) {
        if (mask & (1u << bit)) {
            printf(" %s", GameStateFieldName(bit));
        }
    }
    printf("\n");
}

int CmdInfo(const wchar_t* path) {
    DemoReader r;
    if (!r.Open(path)) {
        printf("REFUSED: %s\n", r.RefusalReason());
        return 2;
    }
    const Manifest& m = r.GetManifest();
    const ReaderStats st = r.Stats();

    uint64_t tMin = ~0ull, tMax = 0;
    uint32_t perSection[kSectionCount] = {};
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.tBeginUs < tMin) tMin = k.tBeginUs;
        if (k.tEndUs > tMax) tMax = k.tEndUs;
        if (k.section < kSectionCount) ++perSection[k.section];
    }
    if (tMin == ~0ull) tMin = 0;

    printf("file             : %ls\n", path);
    printf("  format         : %u.%u   writer state: %s\n", m.formatMajor, m.formatMinor,
           r.WasRecovered() ? "RECOVERED (scanned)" : "clean close (index read)");
    PrintFlags(r.FileFlags());
    printf("  t0_us            : %llu\n", static_cast<unsigned long long>(m.t0Us));
    printf("  duration         : %.3f s\n", (tMax - tMin) / 1e6);
    printf("  capture_role     : %u\n", m.captureRole);
    printf("  level / gametype : %llu / %llu\n", static_cast<unsigned long long>(m.levelId),
           static_cast<unsigned long long>(m.gametypeId));
    printf("  keyframe_us      : %u\n", m.keyframeIntervalUs);
    printf("  partial_start    : %s\n", m.partialStart ? "yes" : "no");
    printf("  chapters         : %zu  (resyncs %u, skipped %llu B, torn tail %llu B)\n", r.ChapterCount(),
           st.resyncs, static_cast<unsigned long long>(st.skippedBytes),
           static_cast<unsigned long long>(st.truncatedTailBytes));
    for (uint16_t s = 0; s < kSectionCount; ++s) {
        if (perSection[s]) {
            printf("    %-9s    : %u\n", SectionName(s), perSection[s]);
        }
    }
    printf("  players          : %u\n", m.playerCount);
    printf("  ROSTER %u pseudonymised=%s\n", m.playerCount, g_pseudonymise ? "yes" : "no");
    for (uint16_t i = 0; i < m.playerCount; ++i) {
        PrintPlayerRow(i, m.players[i]);
    }
    printf("  markers          : %u\n", m.roundCount);
    for (uint16_t i = 0; i < m.roundCount; ++i) {
        // The SEEK axis is the file's origin, not the first chapter key. A mark stamped below t0_us
        // is not a seek target at the time it records -- the reader clamps it to the origin and
        // calls the round PARTIAL-START (src/playback/core/playback.h, ClampRoundSeekUs) -- so the
        // tool prints what the reader will do with it instead of only where the byte says it is.
        const int64_t rel = (int64_t)m.rounds[i].tUs - (int64_t)m.t0Us;
        printf("    kind %u index %u @ %.3f s", m.rounds[i].kind, m.rounds[i].index,
               (m.rounds[i].tUs - tMin) / 1e6);
        if (rel < 0) {
            printf("   PARTIAL-START %lld us before t0; seeks to 0.000 s",
                   static_cast<long long>(rel));
        }
        printf("\n");
    }
    // The final scoreboard. Every chapter is walked because the last record for a user may sit in any
    // of them -- a player who left at half time has no record in the chapters after that, and taking
    // the last chapter alone would report them as absent rather than as they finished.
    std::vector<PlayerStatsRecordDisk> finalStats;
    uint32_t statsRecords = 0;
    // The present mask is the file's answer to "which fields did this build's engine actually give up",
    // so it is reported as a SET and not as a number: a bit set on every record is a field the capture
    // carries throughout, a bit set on none is a field this build could not read at all, and a bit set
    // on some is a field that came and went. L2-RUN-8 F10 stayed invisible because nothing printed it.
    uint32_t gameStateRecords = 0;
    uint32_t presentUnion = 0;
    uint32_t presentAlways = UINT32_MAX;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            continue;
        }
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            if (v.id == kStreamGameState && v.bytes % sizeof(GameStateRecordDisk) == 0) {
                const uint32_t gn = v.bytes / static_cast<uint32_t>(sizeof(GameStateRecordDisk));
                for (uint32_t g = 0; g < gn; ++g) {
                    GameStateRecordDisk gd{};
                    memcpy(&gd, v.records + size_t(g) * sizeof(gd), sizeof(gd));
                    ++gameStateRecords;
                    presentUnion |= gd.present;
                    presentAlways &= gd.present;
                }
            }
            if (v.id != kStreamPlayerStats || v.bytes % sizeof(PlayerStatsRecordDisk) != 0) {
                continue;
            }
            const uint32_t n = v.bytes / static_cast<uint32_t>(sizeof(PlayerStatsRecordDisk));
            for (uint32_t k = 0; k < n; ++k) {
                PlayerStatsRecordDisk d{};
                memcpy(&d, v.records + size_t(k) * sizeof(d), sizeof(d));
                ++statsRecords;
                bool replaced = false;
                for (PlayerStatsRecordDisk& have : finalStats) {
                    if (have.xplatformId == d.xplatformId && have.userIndex == d.userIndex) {
                        if (d.engineTimeUs >= have.engineTimeUs) {
                            have = d;
                        }
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    finalStats.push_back(d);
                }
            }
        }
    }
    if (gameStateRecords == 0) {
        printf("  GAMESTATE   no records\n");
    } else {
        printf("  GAMESTATE %u record(s), present union=0x%03x always=0x%03x\n", gameStateRecords,
               presentUnion, presentAlways & kGameStatePresentAll);
        PrintFieldSet("    present  :", presentAlways & presentUnion);
        PrintFieldSet("    absent   :", ~presentUnion & kGameStatePresentAll);
        const uint32_t sometimes = presentUnion & ~presentAlways & kGameStatePresentAll;
        if (sometimes != 0) {
            PrintFieldSet("    sometimes:", sometimes);
        }
        const uint32_t undefined = presentUnion & ~kGameStatePresentAll;
        if (undefined != 0) {
            printf("    UNDEFINED: 0x%x -- present bits this format does not define\n", undefined);
        }
    }
    printf("  PLAYERSTATS %u record(s), %zu user(s) pseudonymised=%s\n", statsRecords,
           finalStats.size(), g_pseudonymise ? "yes" : "no");
    for (const PlayerStatsRecordDisk& d : finalStats) {
        PrintPlayerStatsRow(m, d);
    }

    printf("  schemas          : %u\n", m.schemaCount);
    return 0;
}

int CmdDump(const wchar_t* path, int chapterFilter, int sectionFilter, int streamFilter, bool hex) {
    DemoReader r;
    if (!r.Open(path)) {
        printf("REFUSED: %s\n", r.RefusalReason());
        return 2;
    }
    uint32_t shown = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        if (chapterFilter >= 0 && static_cast<size_t>(chapterFilter) != i) {
            continue;
        }
        const ChapterKey k = r.ChapterAt(i);
        if (sectionFilter >= 0 && k.section != sectionFilter) {
            continue;
        }
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            printf("chapter %-4zu seq %-5u %-9s  FAULT: %s\n", i, k.seq, SectionName(k.section),
                   ChapterFaultName(r.LastFault()));
            continue;
        }
        printf("chapter %-4zu seq %-5u %-9s  t=[%llu..%llu] off=%llu payload=%u streams=%u\n", i, k.seq,
               SectionName(k.section), static_cast<unsigned long long>(k.tBeginUs),
               static_cast<unsigned long long>(k.tEndUs),
               static_cast<unsigned long long>(r.ChapterOffset(i)), r.ChapterPayloadLen(i), c.streamCount);
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            if (streamFilter >= 0 && v.id != streamFilter) {
                continue;
            }
            printf("    %-17s schema=%-4u bytes=%-8u observed=%-8u stored=%-8u%s\n", StreamName(v.id),
                   v.schema, v.bytes, v.observedCount, v.storedCount,
                   v.observedCount > v.storedCount ? "  <- deficit" : "");
            ++shown;
            if (hex) {
                const uint32_t n = v.bytes < 256 ? v.bytes : 256;
                for (uint32_t o = 0; o < n; o += 16) {
                    printf("      %04x ", o);
                    for (uint32_t j = o; j < o + 16 && j < n; ++j) {
                        printf("%02x ", v.records[j]);
                    }
                    printf("\n");
                }
                if (v.bytes > n) {
                    printf("      ... %u more bytes\n", v.bytes - n);
                }
            }
        }
    }
    printf("DUMP-BLOCKS %u\n", shown);
    return 0;
}

int CmdVerify(const wchar_t* path, bool quiet, bool allowGaps) {
    DemoReader r;
    if (!r.Open(path)) {
        printf("REFUSED: %s\n", r.RefusalReason());
        printf("VERIFY-CHECKS 1\nVERIFY-FAILS 1\n");
        return 2;
    }
    const Manifest& m = r.GetManifest();
    // An imported file has no engine to lose records: every GAP in it is a hole in the legacy source,
    // written there by the converter. Those are known and listed, not failures; a native capture's
    // gaps stay failures unless --allow-gaps.
    const bool imported = (r.FileFlags() & kFlagImported) != 0;
    std::vector<GapRecordDisk> importGaps;

    uint32_t checks = 0, fails = 0, notRunDegraded = 0;
    auto Check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) {
            ++fails;
            printf("FAIL: %s\n", what);
        }
    };

    std::vector<StreamTally> tallies;
    uint64_t prevOffset = 0;
    uint64_t lastBeginPerSection[kSectionCount] = {};
    bool sawVoip = false;
    bool sawAsSeen = false;
    bool sawOracle = false;
    uint32_t undecodable = 0;
    uint32_t degradedRecords = 0;
    // The records the file carried BEFORE its final chapter. The writer files a "this stream produced
    // nothing all session" declaration for every enabled-but-silent stream at Close, and those land in
    // the last chapter, so nearly every capture carries some S_DEGRADED records: an excuse keyed on the
    // total would never refuse anything. A degradation declared while the capture was still running is
    // the one that says a shed ladder fired.
    uint32_t degradedBeforeFinalChapter = 0;
    // S6.4's ladder drops cadence ON PURPOSE and writes an S_DEGRADED record saying so. A two-state
    // gate can only pass (which certifies the drop) or fail (which calls the capture's own declaration
    // a defect); neither is true, so this is the third verdict. It is printed per check, names the
    // stream and the chapter, and is counted apart from `checks` -- a skipped check must never leave
    // as a pass.
    auto NotRunDegraded = [&](const char* what, const char* where, uint32_t declared) {
        ++notRunDegraded;
        printf("NOT-RUN-DEGRADED: %s -- the capture declared %u S_DEGRADED record(s) %s\n", what,
               declared, where);
    };
    // The flags are stamped at close from what the writer saw. A file whose capture declared a
    // degradation while it was still running is one this tool cannot score on that disagreement.
    auto CheckUnlessDeclaredShed = [&](bool ok, const char* what) {
        if (!ok && degradedBeforeFinalChapter > 0) {
            NotRunDegraded(what, "while it was still running", degradedBeforeFinalChapter);
            return;
        }
        ++checks;
        if (!ok) {
            ++fails;
            printf("FAIL: %s\n", what);
        }
    };
    uint32_t gameStateRecords = 0;
    uint32_t playerStatsRecords = 0;
    PlayerStatsCursor statsCursor{};
    uint32_t gameStateBadRecords = 0;

    // The tap-registration DENOMINATOR. An empty S_EVENT stream means one of
    // two opposite things -- nothing happened, or nothing was ever registered to notice it -- and only
    // this block, written into every keyframe, tells them apart. It is decoded through the capture
    // module's OWN reader; a copy of the layout here would certify exactly the files that reader
    // refuses.
    std::vector<uint8_t> keyBlob(64 * 1024);
    uint32_t keyRecords = 0;    // S_KEY records that decoded as records
    uint32_t keyBlobs = 0;      // of those, the ones that opened as keyframe blobs
    uint32_t tapBlocks = 0;     // of those, the ones carrying TAP_REGISTRATION
    uint32_t tapTableSeen = 0;  // ... alongside a TAP_TABLE to cross-check the row count against
    capture::TapRegistrationBlock tapFirst{};
    capture::TapRegistrationBlock tapLast{};
    bool tapDisagrees = false;
    bool tapTableSkew = false;

    // Per chapter, per stream, the record rate implied by the file's own timestamps. Comparing a stream
    // against itself across chapters catches a sampler whose rate changed, without encoding any rate
    // assumption into the tool -- S5.4.2 is explicit that the format has none. It is a compensating
    // control for the blind spot in the counters, not a proof: see kRateStepLimit for what it misses.
    //
    // A chapter shorter than half the file's own declared keyframe interval is a partial chapter -- the
    // last one before a close, or one cut by a marker -- and its rate says nothing about the sampler.
    // The threshold comes from the manifest (S5.5 makes the interval a field), not from a constant here.
    struct RateSample {
        uint16_t stream;
        double lastHz;
        uint32_t lastCount;
    };
    std::vector<RateSample> rates;
    std::vector<StreamTally> perChapter;  // reused each chapter; only `stream` and `stored` are read
    const double fullChapterS = m.keyframeIntervalUs ? (m.keyframeIntervalUs / 2e6) : 0.0;

    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        // Read before this chapter's blocks are counted, so after the loop it holds what the file
        // declared before its LAST chapter -- which is where the close path files its own records.
        degradedBeforeFinalChapter = degradedRecords;
        const ChapterKey k = r.ChapterAt(i);
        Check(k.tBeginUs <= k.tEndUs, "chapter t_begin is after t_end");
        Check(r.ChapterOffset(i) >= prevOffset, "chapter offsets do not ascend");
        prevOffset = r.ChapterOffset(i);
        if (k.section < kSectionCount) {
            Check(k.tBeginUs >= lastBeginPerSection[k.section], "chapters in a section are out of order");
            lastBeginPerSection[k.section] = k.tBeginUs;
            if (k.section == kSectionVoip) sawVoip = true;
            if (k.section == kSectionAsSeen) sawAsSeen = true;
            if (k.section == kSectionOracle) sawOracle = true;
        }

        perChapter.clear();
        DecodedChapter c{};
        ++checks;
        if (!r.DecodeChapter(i, c)) {
            ++fails;
            ++undecodable;
            printf("FAIL: chapter %zu (%s, seq %u) %s\n", i, SectionName(k.section), k.seq,
                   ChapterFaultName(r.LastFault()));
            continue;
        }

        const double spanS = (k.tEndUs > k.tBeginUs) ? (k.tEndUs - k.tBeginUs) / 1e6 : 0.0;
        for (uint16_t b = 0; b < c.streamCount; ++b) {
            const StreamView& v = c.streams[b];
            if (v.id == kStreamVoip) sawVoip = true;
            if (v.id == kStreamAsSeen) sawAsSeen = true;
            if (v.id == kStreamOracle) sawOracle = true;

            // The writer no longer clamps this pair, so an impossible block reaches the file intact and
            // is named here by the block that carries it. `chapter` is not part of the reported fault;
            // it is what makes `block` address one block rather than one per chapter.
            ++checks;
            if (v.observedCount < v.storedCount) {
                ++fails;
                printf("FAIL: OBSERVED < STORED stream=%u block=%u observed=%u stored=%u chapter=%zu\n",
                       static_cast<unsigned>(v.id), static_cast<unsigned>(b),
                       static_cast<unsigned>(v.observedCount), static_cast<unsigned>(v.storedCount), i);
            }

            if (v.id == kStreamDegraded) {
                degradedRecords += v.bytes / static_cast<uint32_t>(sizeof(DegradedRecordDisk));
            }
            // S_GAMESTATE is fixed-width, so a block that is not a whole number of records is not one,
            // and a record's phase, availability and present mask are closed vocabularies the format
            // defines (demotypes.h). Reading them through those constants rather than through a copy is
            // what stops this tool certifying a file the loader would refuse.
            if (v.id == kStreamGameState) {
                ++checks;
                if (v.bytes % sizeof(GameStateRecordDisk) != 0) {
                    ++fails;
                    printf("FAIL: an S_GAMESTATE block of %u bytes is not a whole number of %u-byte "
                           "records\n",
                           v.bytes, static_cast<unsigned>(sizeof(GameStateRecordDisk)));
                } else {
                    const uint32_t n = v.bytes / static_cast<uint32_t>(sizeof(GameStateRecordDisk));
                    gameStateRecords += n;
                    for (uint32_t g = 0; g < n; ++g) {
                        GameStateRecordDisk rec{};
                        memcpy(&rec, v.records + g * sizeof(GameStateRecordDisk), sizeof(rec));
                        if (rec.phase > kMaxGameStatePhase ||
                            rec.availability > kMaxGameStateAvailability ||
                            (rec.present & ~kGameStatePresentAll) != 0) {
                            ++gameStateBadRecords;
                        }
                    }
                }
            }
            // S_PLAYER_STATS is checked through the format's own walker rather than through a copy
            // of the rules here, so this tool cannot certify a record the loader would refuse. The
            // cursor spans the whole file: a user's engine time may not go backwards across a chapter
            // boundary either, and that is exactly where an out-of-order flush would show up.
            if (v.id == kStreamPlayerStats) {
                ++checks;
                const char* why = CheckPlayerStatsBlock(v.records, v.bytes, statsCursor);
                if (why) {
                    ++fails;
                    printf("FAIL: %s (chapter %zu)\n", why, i);
                } else {
                    playerStatsRecords +=
                        v.bytes / static_cast<uint32_t>(sizeof(PlayerStatsRecordDisk));
                }
            }
            // A gap block is bookkeeping for other streams, so it is tallied against the streams it
            // names. Its own row only counts blocks.
            if (v.id == kStreamGap || v.id == kStreamDropped) {
                const uint32_t n = v.bytes / static_cast<uint32_t>(sizeof(GapRecordDisk));
                for (uint32_t g = 0; g < n; ++g) {
                    GapRecordDisk rec{};
                    memcpy(&rec, v.records + g * sizeof(GapRecordDisk), sizeof(rec));
                    TallyFor(tallies, rec.streamId).accountedGaps += rec.count;
                    if (imported && v.id == kStreamGap) importGaps.push_back(rec);
                }
                ++TallyFor(tallies, v.id).blocks;
                continue;
            }

            StreamTally& t = TallyFor(tallies, v.id);
            ++t.blocks;
            // An S_ORACLE record carries its own session and bone documents, so its byte size is
            // per-record DATA, not a stream constant. The F10 width rule cannot hold this stream to
            // one width; the report prints "variable" for it instead of failing every wider block.
            // An imported file's S_NETVAR and S_EVENT carry a legacy message body verbatim, so they vary
            // the same way. A native capture's keep F10: it is the only check that sees a block claiming
            // more records than its bytes hold.
            const bool importedBody = imported && (v.id == kStreamNetvar || v.id == kStreamEvent);
            if (v.id == kStreamOracle || importedBody) t.variableWidth = true;
            t.observed += v.observedCount;
            t.stored += v.storedCount;
            if (v.observedCount != v.storedCount) {
                t.everDiverged = true;
            }
            if (StreamCarriesSamples(v.id)) {
                bool known = false;
                const int ret = RetentionOf(m, v.schema, &known);
                Check(known, "a stream block cites a schema the manifest does not carry");
                if (known) {
                    // Retention::Edge licensed storing fewer records than the engine numbered, with no
                    // per-record index or run boundary in the format to tell an unchanged run from a
                    // dropped one. It is not writable any more, and a file that carries it is named.
                    Check(ret == static_cast<int>(Retention::All),
                          "a schema declares a retention other than ALL; sample dropping is not lossless");
                }
            }

            // F10: relate `stored` to the bytes actually present.
            if (StreamCarriesSamples(v.id) && v.storedCount > 0 && !t.variableWidth) {
                const bool divides = (v.bytes % v.storedCount) == 0;
                const uint32_t width = divides ? v.bytes / v.storedCount : 0;
                if (!t.widthKnown) {
                    t.widthKnown = divides;
                    t.recordBytes = width;
                } else if (!divides || width != t.recordBytes) {
                    ++checks;
                    if (t.widthChecked >= 1) {
                        ++fails;
                        printf("FAIL: %s block %u bytes / %u stored is not the %u-byte record this "
                               "stream has used\n",
                               StreamName(v.id), v.bytes, v.storedCount, t.recordBytes);
                    }
                    // A stream that legitimately varies is marked and excluded, with the report saying so.
                    t.variableWidth = true;
                } else {
                    ++checks;
                    ++t.widthChecked;
                }
            }

            // Rates are compared per chapter, not per block: one stream may legitimately occupy several
            // blocks in a chapter, and comparing those against each other measures the writer's blocking
            // rather than the sampler's rate.
            TallyFor(perChapter, v.id).stored += v.storedCount;
        }

        if (const StreamView* kv = c.Find(kStreamKey)) {
            if (kv->bytes > capture::kBlockPrefixBytes) {
                bitio::BitReader kb(kv->records + capture::kBlockPrefixBytes,
                                    kv->bytes - capture::kBlockPrefixBytes);
                for (uint32_t j = 0; j < kv->storedCount; ++j) {
                    uint16_t n = 0;
                    if (!capture::DecodeRawRecord(kb, keyBlob.data(), keyBlob.size(), n)) {
                        break;
                    }
                    ++keyRecords;
                    capture::KeyframeReader kr;
                    if (!kr.Open(keyBlob.data(), n)) {
                        continue;
                    }
                    ++keyBlobs;
                    uint16_t id = 0;
                    const uint8_t* d = nullptr;
                    uint32_t len = 0;
                    uint16_t tableCount = 0;
                    bool haveTable = false;
                    while (kr.Next(id, d, len)) {
                        if (id == capture::kKeyBlockTapTable && len >= sizeof(uint16_t)) {
                            memcpy(&tableCount, d, sizeof tableCount);
                            haveTable = true;
                            continue;
                        }
                        if (id != capture::kKeyBlockTapRegistration) {
                            continue;
                        }
                        capture::TapRegistrationBlock rb{};
                        if (!capture::ReadTapRegistrationBlock(d, len, rb)) {
                            // A denominator that does not add up is not a smaller denominator, it is an
                            // unreadable one, and everything else this tool would print about the file
                            // would be a number with no population behind it.
                            printf("REFUSED: TAP_REGISTRATION in keyframe %u of chapter %zu does not "
                                   "decode (%u bytes): the block must be at least %u bytes and satisfy "
                                   "registered+deferred+givenUp+unavailable == total and "
                                   "total+excluded == table\n",
                                   j, i, len, capture::kTapRegistrationBlockBytes);
                            printf("VERIFY-CHECKS 1\nVERIFY-FAILS 1\n");
                            return 2;
                        }
                        if (tapBlocks == 0) {
                            tapFirst = rb;
                        } else if (rb.total != tapFirst.total || rb.excluded != tapFirst.excluded ||
                                   rb.tableRows != tapFirst.tableRows) {
                            tapDisagrees = true;
                        }
                        tapLast = rb;
                        ++tapBlocks;
                        if (haveTable) {
                            ++tapTableSeen;
                            if (tableCount != rb.tableRows) {
                                tapTableSkew = true;
                            }
                        }
                    }
                }
            }
        }

        if (spanS >= fullChapterS && spanS > 0.0) {
            for (const StreamTally& cs : perChapter) {
                if (cs.stored == 0) {
                    continue;
                }
                // A rate is only a measurement for a stream the engine clocks. S_MARK, S_EVENT and
                // S_GAMESTATE are written ON CHANGE, so their per-chapter count is the match's own
                // shape: a round boundary puts three marks in one chapter and one in the next, and
                // scoring that ratio failed correct files for having rounds in them. The cadence
                // comes from the format's own table, not from a threshold tuned here.
                if (CadenceOf(cs.stream) != StreamCadence::FixedCadence) {
                    continue;
                }
                const uint32_t count = static_cast<uint32_t>(cs.stored);
                const double hz = count / spanS;
                RateSample* found = nullptr;
                for (RateSample& rs : rates) {
                    if (rs.stream == cs.stream) {
                        found = &rs;
                        break;
                    }
                }
                if (!found) {
                    rates.push_back(RateSample{cs.stream, hz, count});
                    continue;
                }
                const double ratio = hz > found->lastHz ? hz / found->lastHz : found->lastHz / hz;
                const uint32_t delta =
                    count > found->lastCount ? count - found->lastCount : found->lastCount - count;
                // One record is the smallest a count can move, so it is never evidence of a rate change:
                // on a stream carrying two records per chapter it is a 1.5x "step" that means nothing,
                // and the ratio band alone would fire on it.
                const bool steps = (ratio >= kRateStepLimit && delta >= 2);
                // A step the capture already explained is not this tool's finding. `degradedRecords` is
                // the running count over chapters 0..i, so a step that PRECEDES every shed record is
                // still scored: the excuse has to have been written before the drop it excuses.
                // An imported file's hole thins the chapter it falls in; that step is the known gap.
                bool importGapSoFar = false;
                for (const GapRecordDisk& g : importGaps) importGapSoFar |= g.streamId == cs.stream;
                if (steps && degradedRecords > 0) {
                    char what[160];
                    _snprintf_s(what, sizeof(what), _TRUNCATE, "%s rate step at chapter %zu",
                                StreamName(cs.stream), i);
                    NotRunDegraded(what, "at or before that chapter", degradedRecords);
                } else if (steps && importGapSoFar) {
                    printf("  %s rate step %.1f Hz -> %.1f Hz at chapter %zu follows a known import gap\n",
                           StreamName(cs.stream), found->lastHz, hz, i);
                } else {
                    ++checks;
                    if (steps) {
                        ++fails;
                        printf("FAIL: %s rate steps %.1f Hz -> %.1f Hz at chapter %zu\n",
                               StreamName(cs.stream), found->lastHz, hz, i);
                    }
                }
                found->lastHz = hz;
                found->lastCount = count;
            }
        }
    }

    // The completeness law (S5.6.2), in two parts. First: a retention-ALL stream that lost records is a
    // failure even though the file is well formed. A GAP is legal content -- a capture that lost samples
    // is still the best record of that match -- but "did this capture keep everything?" is the question
    // verify exists to answer, and the honest answer is no.
    for (const StreamTally& t : tallies) {
        if (t.accountedGaps == 0) {
            continue;
        }
        ++checks;
        if (!allowGaps && !imported) {
            ++fails;
            printf("FAIL: %s retention=ALL lost %llu records; the file records the gap\n",
                   StreamName(t.stream), static_cast<unsigned long long>(t.accountedGaps));
        }
    }

    if (!importGaps.empty()) {
        printf("IMPORTED: %zu known import gap(s) from the legacy source (not failures)\n", importGaps.size());
        for (const GapRecordDisk& g : importGaps) {
            printf("  import gap at %+.3f s: %s lost %u record(s)\n",
                   static_cast<double>(static_cast<int64_t>(g.tUs - m.t0Us)) / 1e6, StreamName(g.streamId), g.count);
        }
    }

    // Second: every record the engine numbered is either stored or named by a GAP or DROPPED record.
    // An unaccounted deficit is worse than a gap, because nothing in the file says where it went.
    for (const StreamTally& t : tallies) {
        if (t.blocks == 0 || !StreamCarriesSamples(t.stream)) {
            continue;
        }
        ++checks;
        if (t.stored + t.accountedGaps != t.observed) {
            ++fails;
            // The sum can now exceed `observed`, because an impossible block is written through rather
            // than clamped. Subtracting in that direction would print a wrapped 64-bit number, so the
            // line says which way the imbalance runs instead.
            const uint64_t held = t.stored + t.accountedGaps;
            printf("FAIL: %s retention=ALL observed=%llu stored=%llu accounted=%llu (%llu %s)\n",
                   StreamName(t.stream), static_cast<unsigned long long>(t.observed),
                   static_cast<unsigned long long>(t.stored),
                   static_cast<unsigned long long>(t.accountedGaps),
                   static_cast<unsigned long long>(held > t.observed ? held - t.observed
                                                                     : t.observed - held),
                   held > t.observed ? "beyond what the engine numbered" : "unaccounted");
        }
    }

    // A file written before the block existed is NAMED, not passed: "this capture does not say how many
    // of its own surfaces were live" is a different statement from "all of them were", and a checker
    // that cannot tell them apart is the defect this row exists to close.
    Check(tapBlocks > 0,
          "missing TAP_REGISTRATION: no keyframe in this file states how many taps were live when it "
          "was written");
    if (tapBlocks > 0) {
        // The row count has two independent writers in the same keyframe -- TAP_TABLE counts the rows
        // it emits, TAP_REGISTRATION reports the registration snapshot's own tableRows -- so the two
        // can disagree, and a disagreement means the snapshot is not describing this table.
        Check(tapTableSeen == tapBlocks,
              "a keyframe carries TAP_REGISTRATION with no TAP_TABLE to cross-check its table count");
        Check(!tapTableSkew,
              "TAP_REGISTRATION's table count disagrees with the keyframe's own TAP_TABLE row count");
        Check(!tapDisagrees,
              "keyframes disagree about the tap table: total, excluded or table moved within the file");
    }

    // ARCH-21's MANIFEST round table. A file that carries no table is not a file with no rounds: every
    // capture written before the block existed says `not_recorded`, and printing that as `0` would turn
    // "nobody asked" into "asked and there were none". A malformed table never reaches this line -- the
    // reader refuses the BLOCK by name at Open, keeps the file, and verify reports that refusal.
    if (m.roundsRefused != nullptr) {
        // The file opened; only the ROUNDS block did not. That is reported and NOT scored: failing a
        // verify over a degraded feature would read as a bad recording, and the recording is fine.
        Check(m.roundRowCount == 0, "the file carries round rows the reader refused");
        printf("ROUNDS refused: %s\n", m.roundsRefused);
    } else if (m.markerSource == MarkerSource::NotRecorded) {
        Check(m.roundRowCount == 0, "the file carries round rows but names no marker source");
        printf("ROUNDS none source=not_recorded\n");
    } else {
        bool sawNumber = false;
        uint16_t firstNumber = 0, lastNumber = 0;
        // The reader-side clamp's population, counted rather than assumed: rows and marks whose
        // recorded begin is below t0_us. Those are seekable only at the origin, and the round is
        // PARTIAL-START (src/playback/core/playback.h, ClampRoundSeekUs). It is NOT a verify
        // failure -- an old capture is not a corrupt one, and the bytes are exactly what the writer
        // of the day wrote -- but a file that carries them must not read as one that does not.
        uint16_t partialStartRows = 0, partialStartMarks = 0;
        for (uint16_t i = 0; i < m.roundCount; ++i) {
            if (m.rounds[i].tUs < m.t0Us) {
                ++partialStartMarks;
            }
        }
        for (uint16_t i = 0; i < m.roundRowCount; ++i) {
            const RoundRow& row = m.roundRows[i];
            if (row.beginUs < m.t0Us) {
                ++partialStartRows;
            }
            Check(row.beginUs <= row.endUs, "a round row ends before it begins");
            // Two fields, one fact: a row is numberless exactly when its number was refused.
            Check((row.number == kRoundNumberNone) == ((row.flags & kRoundNumberRefused) != 0),
                  "a round row's number and its NUMBER_REFUSED flag disagree");
            if (row.number != kRoundNumberNone) {
                if (!sawNumber) {
                    firstNumber = row.number;
                    sawNumber = true;
                }
                lastNumber = row.number;
            }
        }
        printf("ROUNDS %u source=%s", m.roundRowCount,
               MarkerSourceName(static_cast<uint8_t>(m.markerSource)));
        if (sawNumber) {
            printf(" first=%u last=%u", firstNumber, lastNumber);
        } else {
            printf(" first=none last=none");
        }
        // Appended, never prepended: the fields above are what existing readers of this line parse.
        if (partialStartRows != 0 || partialStartMarks != 0) {
            printf(" PARTIAL-START rows=%u marks=%u", partialStartRows, partialStartMarks);
        }
        printf("\n");
    }
    // The manifest roster. `ROSTER 0` is a real answer -- a solo host with no entrants writes it -- so
    // the rows are checked for internal agreement rather than for existing. A v0 row (`extended` clear)
    // carries no fill mask at all, and asserting a mask it never had would turn "this capture did not
    // record what it could source" into a failure of the capture.
    uint16_t rosterExtended = 0;
    for (uint16_t i = 0; i < m.playerCount; ++i) {
        const PlayerRecord& p = m.players[i];
        if (p.extended) {
            ++rosterExtended;
        }
        Check((p.fill & ~kPlayerFillAll) == 0,
              "a player row's fill mask names a bit the format does not define");
        Check((p.jsonTruncated & ~(kPlayerJsonTruncEntrant | kPlayerJsonTruncUnlocks)) == 0,
              "a player row's truncation flags name a blob the format does not define");
        Check(p.entrantJson.n <= kPlayerJsonCap,
              "a player row's entrant json is longer than the format's cap");
        Check(p.unlocksJson.n <= kPlayerJsonCap,
              "a player row's unlocks json is longer than the format's cap");
        // A blob is flagged truncated only if it was stored at exactly the cap: a flag over a short
        // blob would mean bytes went missing somewhere the writer cannot account for.
        Check((p.jsonTruncated & kPlayerJsonTruncEntrant) == 0 || p.entrantJson.n == kPlayerJsonCap,
              "a player row flags a truncated entrant json that is not stored at the cap");
        Check((p.jsonTruncated & kPlayerJsonTruncUnlocks) == 0 || p.unlocksJson.n == kPlayerJsonCap,
              "a player row flags a truncated unlocks json that is not stored at the cap");
        if (!p.extended) {
            continue;
        }
        // Two fields, one fact -- and the fact is the WRITER's, imported rather than restated here.
        // The TEAM bit means the read happened; an entrant the engine seats on no side is read as
        // kPlayerNoTeam16, so a set bit and the sentinel agree. A clear bit still admits nothing but
        // the sentinel.
        Check(replay::capture::RecordTeamAgreesWithFill(p),
              "a player row's team and its TEAM fill bit disagree: a set bit needs a team the engine "
              "accepts or the no-team sentinel, and a clear bit needs the sentinel");
        // The colour's half of the same one-source rule, imported from the writer for the same
        // reason. IDENT.ALIGN_TEAM_LIMIT gives teams 2 and 4 no alignment at all, so a set bit
        // beside the sentinel is the engine's own answer rather than a contradiction.
        Check(replay::capture::RecordAlignmentAgreesWithFill(p),
              "a player row's alignment and its ALIGN fill bit disagree: a clear bit needs the "
              "no-alignment sentinel, and a set bit admits that sentinel or a colour on a team "
              "the engine gives one to");
        Check((p.fill & kPlayerFillName) == 0 || p.displayNameFull[0] != 0,
              "a player row claims a sourced display name and carries none");
        Check((p.fill & kPlayerFillProfile) != 0 || p.entrantJson.n == 0,
              "a player row carries entrant json but claims no sourced profile");
        Check(p.firstSeenUs == kPlayerTimeNone || p.lastSeenUs == kPlayerTimeNone ||
                  p.firstSeenUs <= p.lastSeenUs,
              "a player row was last seen before it was first seen");
    }
    Check((m.playerCount == 0) == (m.players == nullptr),
          "the manifest's player count disagrees with the roster rows it carries");
    printf("ROSTER %u extended=%u pseudonymised=%s\n", m.playerCount, rosterExtended,
           g_pseudonymise ? "yes" : "no");
    if (!quiet) {
        for (uint16_t i = 0; i < m.playerCount; ++i) {
            PrintPlayerRow(i, m.players[i]);
        }
    }

    Check(gameStateBadRecords == 0,
          "an S_GAMESTATE record declares a phase, availability or present bit the format does not "
          "define");
    printf("PLAYERSTATS %u record(s), %u user(s)\n", playerStatsRecords, statsCursor.n);
    if (gameStateRecords) {
        printf("GAMESTATE %u record(s), %u outside the format's vocabularies\n", gameStateRecords,
               gameStateBadRecords);
    }

    if (tapBlocks > 0) {
        printf("TAPS registered=%u deferred=%u givenUp=%u unavailable=%u excluded=%u total=%u table=%u"
               " started=%u keyframes=%u\n",
               tapLast.registered, tapLast.deferred, tapLast.givenUp, tapLast.unavailable,
               tapLast.excluded, tapLast.total, tapLast.tableRows, tapLast.started, tapBlocks);
        if (tapLast.registered != tapFirst.registered) {
            printf("TAPS registered moved %u -> %u over the file\n", tapFirst.registered,
                   tapLast.registered);
        }
        if (!tapLast.started) {
            printf("TAPS the registration pass had not started when the last keyframe was written; "
                   "these counts are 'not asked yet', not 'asked and none'\n");
        }
    } else {
        printf("TAPS none: %u S_KEY record(s), %u keyframe blob(s), no TAP_REGISTRATION block\n",
               keyRecords, keyBlobs);
    }
    // AUDIT_M6 wants the file cross-checked against the per-close sentinel. It cannot be done from the
    // file: the sentinel is named for the launch FILETIME and pid (capture/capturestop.h), and the
    // manifest carries neither those nor the file's own name. Said rather than approximated -- a
    // cross-reference against a name this tool derived itself would check the derivation, not the file.
    printf("TAPS sentinel-xref: not possible -- the manifest carries no self-name and no launch "
           "identity; its only file identity is demoUuid %016llX%016llX\n",
           static_cast<unsigned long long>(m.demoUuidHi), static_cast<unsigned long long>(m.demoUuidLo));

    Check(((r.FileFlags() & kFlagHasVoip) != 0) == sawVoip, "HAS_VOIP disagrees with the file's content");
    CheckUnlessDeclaredShed(((r.FileFlags() & kFlagHasAsSeen) != 0) == sawAsSeen,
                            "HAS_ASSEEN disagrees with the file's content");
    CheckUnlessDeclaredShed(((r.FileFlags() & kFlagHasOracle) != 0) == sawOracle,
                            "HAS_ORACLE disagrees with the file's content");
    Check(r.ChapterCount() > 0, "the file carries no chapters");
    Check(r.WasRecovered() || r.Stats().footerPresent, "a cleanly closed file has no valid footer");

    if (!quiet) {
        printf("\n%-17s %10s %10s %10s %9s %s\n", "stream", "observed", "stored", "accounted", "blocks",
               "record width");
        for (const StreamTally& t : tallies) {
            char width[64];
            if (!StreamCarriesSamples(t.stream)) {
                _snprintf_s(width, sizeof(width), _TRUNCATE, "-");
            } else if (t.variableWidth || !t.widthKnown) {
                _snprintf_s(width, sizeof(width), _TRUNCATE, "variable (stored not cross-checked)");
            } else {
                _snprintf_s(width, sizeof(width), _TRUNCATE, "%u B x %u blocks", t.recordBytes,
                            t.widthChecked);
            }
            printf("%-17s %10llu %10llu %10llu %9u %-36s%s\n", StreamName(t.stream),
                   static_cast<unsigned long long>(t.observed), static_cast<unsigned long long>(t.stored),
                   static_cast<unsigned long long>(t.accountedGaps), t.blocks, width,
                   (StreamCarriesSamples(t.stream) && !t.everDiverged)
                       ? "  [advisory: observed never diverged]"
                       : "");
        }
        printf("\n");
        printf("recovered        : %s\n", r.WasRecovered() ? "yes" : "no");
        printf("undecodable      : %u of %zu chapters\n", undecodable, r.ChapterCount());
        // The two detectors that are bands rather than proofs, published with the numbers they use.
        printf("rate-step limit  : %.2fx between adjacent chapters of one FIXED-CADENCE stream\n",
               kRateStepLimit);
        printf("rate-step scope  : on-change streams (S_MARK, S_EVENT, S_GAMESTATE, ...) are not\n"
               "                   scored on rate -- their per-chapter count is match data\n");
        printf("  it needs a change of at least two records, and misses a step below that factor,\n");
        printf("  or one applied uniformly to the whole file\n");
        printf("  a fixed-width stream's stored is cross-checked against its bytes; a uniformly wrong\n");
        printf("  stored stays self-consistent and is not caught\n");
    }
    printf("VERIFY-DEGRADED %u record(s), %u of them before the final chapter\n", degradedRecords,
           degradedBeforeFinalChapter);
    printf("VERIFY-CHECKS %u\nVERIFY-NOT-RUN-DEGRADED %u\nVERIFY-FAILS %u\n", checks, notRunDegraded,
           fails);
    return fails == 0 ? 0 : 1;
}

int CmdStrip(const wchar_t* in, const wchar_t* out, uint32_t mask) {
    const StripResult res = StripSections(in, out, mask);
    if (res.error) {
        printf("STRIP FAILED: %s\n", res.error);
        return 2;
    }
    printf("chapters in      : %u\n", res.chaptersIn);
    printf("chapters out     : %u\n", res.chaptersOut);
    printf("chapters dropped : %u (%u undecodable)\n", res.chaptersDropped, res.chaptersUndecodable);
    printf("blocks stripped  : %u  (inside kept chapters)\n", res.strippedBlocksInKeptChapters);
    printf("bytes removed    : %llu\n", static_cast<unsigned long long>(res.bytesRemoved));
    printf("verified         : %s\n", res.verified ? "yes - the output was re-scanned" : "NO");
    return res.verified ? 0 : 2;
}

// ---- fuzz ------------------------------------------------------------------------------------
// A verification instrument, so it states what it covers and what it does not, and it is deterministic
// under a seed so a failure is reproducible.

uint64_t NextRandom(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

bool ReadWholeFile(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) &&
                                    got == out.size());
    CloseHandle(h);
    return ok;
}

bool WriteWholeFile(const wchar_t* path, const uint8_t* p, size_t n) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD done = 0;
    const bool ok = n == 0 || (WriteFile(h, p, static_cast<DWORD>(n), &done, nullptr) && done == n);
    CloseHandle(h);
    return ok;
}

struct OpenOutcome {
    bool opened;
    size_t chapters;
    uint32_t decodable;
};

OpenOutcome Probe(const wchar_t* path) {
    OpenOutcome o{};
    DemoReader r;
    if (!r.Open(path)) {
        return o;
    }
    o.opened = true;
    o.chapters = r.ChapterCount();
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        DecodedChapter c{};
        if (r.DecodeChapter(i, c)) {
            ++o.decodable;
        }
    }
    return o;
}

int CmdFuzz(const wchar_t* path, uint64_t seed, uint32_t cases, bool doTruncate, bool doFlip) {
    std::vector<uint8_t> original;
    if (!ReadWholeFile(path, original)) {
        printf("cannot read %ls\n", path);
        return 2;
    }
    const OpenOutcome base = Probe(path);
    if (!base.opened) {
        printf("the input does not open; nothing to fuzz\n");
        return 2;
    }

    std::wstring temp = path;
    temp += L".fuzz";

    uint32_t ran = 0, failures = 0;
    uint64_t state = seed ? seed : 0x9E3779B97F4A7C15ull;

    if (doTruncate) {
        for (uint32_t i = 0; i < cases; ++i) {
            const size_t cut = static_cast<size_t>(NextRandom(state) % original.size());
            if (!WriteWholeFile(temp.c_str(), original.data(), cut)) {
                printf("cannot write %ls\n", temp.c_str());
                return 2;
            }
            const OpenOutcome o = Probe(temp.c_str());
            ++ran;
            // A truncated file may open with fewer chapters, or refuse when the header or the manifest
            // did not survive. It may never report more chapters than the intact file, and every chapter
            // it does report must decode.
            if (o.chapters > base.chapters || (o.opened && o.decodable != o.chapters)) {
                ++failures;
                printf("FAIL truncate@%zu: opened=%d chapters=%zu decodable=%u (base %zu)\n", cut,
                       (int)o.opened, o.chapters, o.decodable, base.chapters);
            }
        }
    }

    if (doFlip) {
        for (uint32_t i = 0; i < cases; ++i) {
            std::vector<uint8_t> copy = original;
            const size_t at = static_cast<size_t>(NextRandom(state) % copy.size());
            copy[at] ^= static_cast<uint8_t>(1u << (NextRandom(state) & 7));
            if (!WriteWholeFile(temp.c_str(), copy.data(), copy.size())) {
                printf("cannot write %ls\n", temp.c_str());
                return 2;
            }
            const OpenOutcome o = Probe(temp.c_str());
            ++ran;
            // A single flipped bit may cost at most the chapter that held it. It may never produce more
            // chapters than the intact file, and a chapter that survives must still decode.
            if (o.chapters > base.chapters || (o.opened && o.decodable + 1 < o.chapters)) {
                ++failures;
                printf("FAIL bitflip@%zu: opened=%d chapters=%zu decodable=%u (base %zu)\n", at,
                       (int)o.opened, o.chapters, o.decodable, base.chapters);
            }
        }
    }

    DeleteFileW(temp.c_str());
    printf("base chapters    : %zu (all decodable: %s)\n", base.chapters,
           base.decodable == base.chapters ? "yes" : "no");
    printf("FUZZ-CASES %u\nFUZZ-FAILS %u\n", ran, failures);
    printf("\nnot covered by this instrument:\n");
    printf("  - concurrent damage: every case flips or truncates once, never both\n");
    printf("  - multi-bit and burst corruption within one chapter payload\n");
    printf("  - a crafted head whose crc32 was recomputed to match the damage\n");
    printf("  - filesystem-level damage: sparse holes, sector reordering, ADS\n");
    printf("  - record-level content: the container checks framing, not what a codec decodes\n");
    return failures == 0 ? 0 : 1;
}

// One CSV row per recorded voice frame, in file order: what the capture actually kept, so its
// continuity per speaker can be measured without a game.
int CmdVoip(const wchar_t* path) {
    DemoReader r;
    if (!r.Open(path)) {
        printf("REFUSED: %s\n", r.RefusalReason());
        return 2;
    }
    printf("chapter,tBeginUs,tEndUs,tUs,userSlot,generation,seq,bytes\n");
    static uint8_t buf[sizeof(capture::VoipRec) + 0x10000];
    uint32_t malformed = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section != kSectionVoip) {
            continue;
        }
        DecodedChapter c{};
        if (!r.DecodeChapter(i, c)) {
            printf("# chapter %zu FAULT %s\n", i, ChapterFaultName(r.LastFault()));
            continue;
        }
        const StreamView* v = c.Find(kStreamVoip);
        if (v == nullptr || v->bytes <= capture::kBlockPrefixBytes) {
            continue;
        }
        if (v->observedCount != v->storedCount) {
            printf("# chapter %zu deficit observed %u stored %u\n", i, v->observedCount, v->storedCount);
        }
        bitio::BitReader br(v->records + capture::kBlockPrefixBytes, v->bytes - capture::kBlockPrefixBytes);
        for (uint32_t n = 0; n < v->recordCount; ++n) {
            uint16_t len = 0;
            if (!capture::DecodeRawRecord(br, buf, sizeof buf, len) || len < sizeof(capture::VoipRec)) {
                ++malformed;
                break;
            }
            capture::VoipRec rec{};
            memcpy(&rec, buf, sizeof rec);
            printf("%zu,%llu,%llu,%llu,%u,%u,%u,%u\n", i, static_cast<unsigned long long>(k.tBeginUs),
                   static_cast<unsigned long long>(k.tEndUs), static_cast<unsigned long long>(rec.tUs),
                   static_cast<uint16_t>(rec.speakerSlot), rec.speakerSlot >> 16, rec.frameCtr, rec.bodyBytes);
        }
    }
    if (malformed != 0) {
        printf("# malformed blocks %u\n", malformed);
    }
    return malformed == 0 ? 0 : 1;
}

int Usage() {
    printf("echodemo <command> [options]\n\n");
    printf("  info   <file>\n");
    printf("  dump   <file> [--chapter N] [--section N] [--stream N] [--hex]\n");
    printf("  verify <file> [--quiet] [--allow-gaps]\n");
    printf("  convert <in.echoreplay|in.tape> <out.echodemo> [--session N] [--rate 45] [--report <json>]\n");
    printf("                 legacy holes become known import gaps; the report is <out>.import.json\n");
    printf("  strip  <in> <out> --voip [--asseen] [--oracle]\n");
    printf("  voip   <file>   one CSV row per recorded voice frame\n");
    printf("  rows   <file> --what player|body|event|netvar|gamestate|roster [--from s] [--to s]\n");
    printf("                 decoded records as CSV on the capture axis; no names or account ids\n");
    printf("  fuzz   <file> [--seed N] [--cases N] [--truncate] [--bitflip]\n");
    printf("\n  --pseudonymise   render player names, platform strings and account ids as playerNN\n");
    printf("  --raw            print them as captured; REFUSED while ECHOREPLAY_HARNESS is set\n");
    printf("                   pseudonymisation is ON by default under ECHOREPLAY_HARNESS\n");
    return 2;
}

bool Eq(const wchar_t* a, const wchar_t* b) { return wcscmp(a, b) == 0; }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        return Usage();
    }
    // Scanned over the whole line before any command runs: every command prints roster fields, so the
    // privacy decision cannot live in one command's option loop.
    bool wantPseudonymise = false, wantRaw = false;
    for (int i = 1; i < argc; ++i) {
        if (Eq(argv[i], L"--pseudonymise")) wantPseudonymise = true;
        else if (Eq(argv[i], L"--raw")) wantRaw = true;
    }
    if (!InitPrivacy(wantPseudonymise, wantRaw)) {
        return 2;
    }
    const wchar_t* cmd = argv[1];

    if (Eq(cmd, L"info")) {
        return CmdInfo(argv[2]);
    }
    if (Eq(cmd, L"dump")) {
        int chapter = -1, section = -1, stream = -1;
        bool hex = false;
        for (int i = 3; i < argc; ++i) {
            if (Eq(argv[i], L"--chapter") && i + 1 < argc) chapter = _wtoi(argv[++i]);
            else if (Eq(argv[i], L"--section") && i + 1 < argc) section = _wtoi(argv[++i]);
            else if (Eq(argv[i], L"--stream") && i + 1 < argc) stream = _wtoi(argv[++i]);
            else if (Eq(argv[i], L"--hex")) hex = true;
            else return Usage();
        }
        return CmdDump(argv[2], chapter, section, stream, hex);
    }
    if (Eq(cmd, L"rows")) {
        const int rc = rows::RunRows(argv[2], argc - 3, argv + 3);
        return rc < 0 ? Usage() : rc;
    }
    if (Eq(cmd, L"convert")) {
        return import::RunConvertCommand(argc - 2, argv + 2);
    }
    if (Eq(cmd, L"voip")) {
        return CmdVoip(argv[2]);
    }
    if (Eq(cmd, L"verify")) {
        bool quiet = false, allowGaps = false;
        for (int i = 3; i < argc; ++i) {
            if (Eq(argv[i], L"--quiet")) quiet = true;
            else if (Eq(argv[i], L"--allow-gaps")) allowGaps = true;
            else return Usage();
        }
        return CmdVerify(argv[2], quiet, allowGaps);
    }
    if (Eq(cmd, L"strip")) {
        if (argc < 5) {
            return Usage();
        }
        uint32_t mask = 0;
        for (int i = 4; i < argc; ++i) {
            if (Eq(argv[i], L"--voip")) mask |= 1u << kSectionVoip;
            else if (Eq(argv[i], L"--asseen")) mask |= 1u << kSectionAsSeen;
            else if (Eq(argv[i], L"--oracle")) mask |= 1u << kSectionOracle;
            else return Usage();
        }
        if (mask == 0) {
            return Usage();
        }
        return CmdStrip(argv[2], argv[3], mask);
    }
    if (Eq(cmd, L"fuzz")) {
        uint64_t seed = 1;
        uint32_t cases = 64;
        bool truncate = false, flip = false;
        for (int i = 3; i < argc; ++i) {
            if (Eq(argv[i], L"--seed") && i + 1 < argc) seed = _wcstoui64(argv[++i], nullptr, 0);
            else if (Eq(argv[i], L"--cases") && i + 1 < argc) cases = static_cast<uint32_t>(_wtoi(argv[++i]));
            else if (Eq(argv[i], L"--truncate")) truncate = true;
            else if (Eq(argv[i], L"--bitflip")) flip = true;
            else return Usage();
        }
        if (!truncate && !flip) {
            truncate = flip = true;
        }
        return CmdFuzz(argv[2], seed, cases, truncate, flip);
    }
    return Usage();
}
