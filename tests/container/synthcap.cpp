#include "synthcap.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <vector>

#include "capture/keyframe.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"

using namespace replay;

namespace synth {
namespace {

// Deterministic and incompressible enough that a padded chapter really grows on disk.
uint64_t Mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

struct SchemaSpec {
    uint16_t id;
    Retention retention;
    bool required;
};

// Every schema declares ALL, including the four that used to declare EDGE. Their producers are
// change-triggered, so there are no samples between edges and their observed already equals their
// stored -- which is the evidence that they never needed EDGE in the first place.
const SchemaSpec kSchemas[] = {
    {kSchemaTick, Retention::All, true},    {kSchemaPlayer, Retention::All, true},
    {kSchemaNetvar, Retention::All, true},  {kSchemaEvent, Retention::All, true},
    {kSchemaMark, Retention::All, true},    {kSchemaKey, Retention::All, true},
    {kSchemaAsSeen, Retention::All, false}, {kSchemaVoip, Retention::All, false},
    {kSchemaOracle, Retention::All, false}, {kSchemaWorld, Retention::All, true},
};

struct ManifestBacking {
    std::vector<PlayerRecord> players;
    std::vector<SchemaDef> schemas;
    std::vector<FieldDef> fields;
    std::vector<RoundMarker> rounds;
    std::vector<uint8_t> entrant, unlocks, balance, slot0, slot1, netmap;
};

ManifestBacking& Backing() {
    static ManifestBacking b;
    return b;
}

}  // namespace

void FillRecord(uint8_t* p, uint32_t bytes, uint32_t chapter, uint16_t stream, uint32_t index) {
    uint64_t s = Mix((uint64_t(chapter) << 40) ^ (uint64_t(stream) << 24) ^ index);
    // Voice is opaque and incompressible (S5.4.6); everything else is delta-coded and near-constant
    // (S5.6), so it compresses. Both are here so a capture exercises the zstd path and the raw fallback.
    const uint32_t entropy = (stream == replay::kStreamVoip) ? bytes : (bytes < 8 ? bytes : 8);
    for (uint32_t i = 0; i < entropy; ++i) {
        if ((i & 7) == 0) {
            s = Mix(s);
        }
        p[i] = static_cast<uint8_t>(s >> ((i & 7) * 8));
    }
    for (uint32_t i = entropy; i < bytes; ++i) {
        p[i] = static_cast<uint8_t>(stream * 17 + (i & 3));
    }
}

void BuildManifest(const Config& cfg, Manifest& out) {
    ManifestBacking& b = Backing();
    out = Manifest{};
    out.formatMajor = kFormatMajor;
    out.formatMinor = kFormatMinor;
    out.t0Us = cfg.t0Us;
    out.demoUuidLo = 0x0123456789ABCDEFull;
    out.demoUuidHi = 0xFEDCBA9876543210ull;
    for (int i = 0; i < 16; ++i) {
        out.echovrMd5[i] = static_cast<uint8_t>(0xA0 + i);
    }
    out.buildStringHash = 0x1122334455667788ull;
    out.contentHash = 0x99AABBCCDDEEFF00ull;
    out.captureRole = 0;
    out.authorityFlags = 0x40;
    out.flags = 0;
    if (cfg.voip) out.flags |= kFlagHasVoip;
    if (cfg.asSeen) out.flags |= kFlagHasAsSeen;
    if (cfg.oracle) out.flags |= kFlagHasOracle;
    out.levelId = 0xDEADBEEFull;
    out.gametypeId = 0xCAFEBABEull;
    out.startTimeEpoch = 1700000000ull;
    out.keyframeIntervalUs = static_cast<uint32_t>(cfg.chapterSpanUs);
    out.partialStart = false;

    b.entrant.assign(200, 'e');
    b.unlocks.assign(512, 'u');
    b.balance.assign(3334, 'z');
    b.slot0.assign(896, 0x50);
    b.slot1.assign(368, 0x51);
    b.netmap.assign(256, 0x4E);

    b.players.assign(cfg.playerCount, PlayerRecord{});
    for (uint32_t i = 0; i < cfg.playerCount; ++i) {
        PlayerRecord& p = b.players[i];
        p.xplatformId = 0x1000 + i;
        memset(p.platformStr, 0, sizeof(p.platformStr));
        memcpy(p.platformStr, "OVR", 3);
        memset(p.displayName, 0, sizeof(p.displayName));
        if (i < cfg.playerNames.size()) {
            _snprintf_s(p.displayName, sizeof(p.displayName), _TRUNCATE, "%s",
                        cfg.playerNames[i].c_str());
        } else {
            _snprintf_s(p.displayName, sizeof(p.displayName), _TRUNCATE, "player_%02u", i);
        }
        p.userSlot = static_cast<uint16_t>(i);
        p.userIndex = static_cast<uint16_t>(i);
        p.generation = 1;
        p.teamId = static_cast<uint8_t>(i % 2);
        p.teamSlot = static_cast<uint8_t>(i / 2);
        p.alignment = static_cast<uint8_t>(i % 2);
        p.bodyType = 1;
        p.loadoutNumber = static_cast<uint16_t>(i);
        p.loadoutNumberBody = static_cast<uint16_t>(i + 1);
        for (int s = 0; s < 20; ++s) {
            p.loadoutSlots[s] = 0x5000 + i * 20 + s;
        }
        p.entrantJson = Blob{b.entrant.data(), static_cast<uint32_t>(b.entrant.size())};
        p.fill |= kPlayerFillProfile;
        p.unlocksJson = Blob{b.unlocks.data(), static_cast<uint32_t>(b.unlocks.size())};
    }

    b.fields.clear();
    b.schemas.clear();
    std::vector<uint32_t> starts;
    for (const SchemaSpec& spec : kSchemas) {
        starts.push_back(static_cast<uint32_t>(b.fields.size()));
        FieldDef f0{};
        f0.id = static_cast<uint16_t>(spec.id * 10 + 1);
        f0.cls = FieldClass::WireQ;
        f0.min = -250.0f;
        f0.max = 250.0f;
        f0.step = 0.001f;
        f0.rawBits = 0;
        f0.prov = FieldProv::Pc;
        FieldDef f1{};
        f1.id = static_cast<uint16_t>(spec.id * 10 + 2);
        f1.cls = FieldClass::Raw;
        f1.rawBits = 28;
        f1.prov = FieldProv::Pc;
        b.fields.push_back(f0);
        b.fields.push_back(f1);
    }
    for (size_t i = 0; i < sizeof(kSchemas) / sizeof(kSchemas[0]); ++i) {
        SchemaDef d{};
        d.id = kSchemas[i].id;
        d.retention = kSchemas[i].retention;
        d.required = kSchemas[i].required;
        d.fieldCount = 2;
        d.fields = b.fields.data() + starts[i];
        b.schemas.push_back(d);
    }
    b.rounds.clear();

    out.playerCount = static_cast<uint16_t>(b.players.size());
    out.players = b.players.data();
    out.roundCount = 0;
    out.rounds = nullptr;
    out.schemaCount = static_cast<uint16_t>(b.schemas.size());
    out.schemas = b.schemas.data();
    out.balanceBlobZstd = Blob{b.balance.data(), static_cast<uint32_t>(b.balance.size())};
    out.slot0Baselines = Blob{b.slot0.data(), static_cast<uint32_t>(b.slot0.size())};
    out.slot1RuleVars = Blob{b.slot1.data(), static_cast<uint32_t>(b.slot1.size())};
    out.netIdSlotMap = Blob{b.netmap.data(), static_cast<uint32_t>(b.netmap.size())};
}

// One keyframe blob, written with the loader's OWN writer. The fixture must not spell the layout
// itself: a fixture with a private copy of the format proves only that two copies agree.
static size_t BuildKeyBlob(const Config& cfg, uint32_t chapter, uint32_t index, uint8_t* dst,
                           size_t cap) {
    using namespace replay::capture;
    static const char* const kRows[4] = {"Goal", "Stun", "Catch", "Restart"};
    KeyframeBlobWriter k(dst, cap, cfg.t0Us + uint64_t(chapter) * cfg.chapterSpanUs + index);

    k.Block(kKeyBlockTapTable);
    const uint16_t rows = 4;
    k.Put(rows);
    for (uint16_t i = 0; i < rows; ++i) {
        const uint16_t len = static_cast<uint16_t>(strlen(kRows[i]));
        k.Put(i);
        k.Put(static_cast<uint16_t>(kStreamEvent));
        k.Put(len);
        k.Raw(kRows[i], len);
    }

    TapRegistrationBlock rb{};
    rb.registered = 2;
    rb.deferred = 1;
    rb.givenUp = 0;
    rb.unavailable = 0;
    rb.excluded = 1;
    rb.total = 3;
    rb.tableRows = rows;
    rb.started = 1;
    if (cfg.tapReg == kTapRegCorrupt) {
        rb.total = 9;  // registered+deferred+givenUp+unavailable != total
    } else if (cfg.tapReg == kTapRegTableSkew) {
        rb.excluded = 2;
        rb.tableRows = 5;  // both identities still close; TAP_TABLE above says 4
    }
    PutTapRegistrationBlock(k, rb);
    return k.Finish();
}

// The block payload, generated once and read by both the writer and the round-trip check, so the
// expectation cannot drift away from what was written.
static void FillBlock(const Config& cfg, const BlockPlan& b, std::vector<uint8_t>& buf) {
    if (b.stream == kStreamKey && cfg.tapReg != kTapRegOff) {
        buf.assign(replay::capture::kBlockPrefixBytes + 4096 + size_t(512) * b.recordCount, 0);
        bitio::BitWriter w(buf.data() + replay::capture::kBlockPrefixBytes,
                           buf.size() - replay::capture::kBlockPrefixBytes);
        std::vector<uint8_t> blob(4096);
        for (uint32_t i = 0; i < b.recordCount; ++i) {
            const size_t n = BuildKeyBlob(cfg, b.chapter, i, blob.data(), blob.size());
            replay::capture::EncodeRawRecord(w, blob.data(), static_cast<uint16_t>(n));
        }
        const uint32_t bits = static_cast<uint32_t>(w.BitsWritten());
        memcpy(buf.data(), &bits, sizeof bits);
        buf.resize(replay::capture::kBlockPrefixBytes + (bits + 7u) / 8u);
        return;
    }
    buf.resize(size_t(b.recordBytes) * b.recordCount);
    for (uint32_t i = 0; i < b.recordCount; ++i) {
        FillRecord(buf.data() + size_t(i) * b.recordBytes, b.recordBytes, b.chapter, b.stream, i);
    }
}

void PlanPlayerStats(const Config& cfg, uint32_t chapter, uint32_t who, PlayerStatsRecord& out) {
    const uint32_t c = chapter + 1;
    out = PlayerStatsRecord{};
    out.xplatformId = 0x1000 + who;
    out.userIndex = static_cast<uint16_t>(who);
    out.captureTimeUs = cfg.t0Us + uint64_t(chapter) * cfg.chapterSpanUs;
    out.engineTimeUs = out.captureTimeUs;

    // Every value moves at a different rate, so a field read out of the wrong slot does not land on a
    // plausible number, and the three players never share a whole row.
    out.possessionTime = float(c * (who + 2)) * 0.25f;
    out.goals = int32_t(c / (who + 2));
    out.points = out.goals * int32_t(who + 2);
    out.stuns = int32_t((c + who) / 3);
    out.blocks = int32_t(c / 4);
    out.assists = int32_t((c + 2 * who) / 5);
    out.saves = int32_t(c / (who + 3));
    out.interceptions = int32_t(c / 6);
    out.steals = int32_t((c + who) / 7);
    out.catches = int32_t(c + who);
    out.passes = int32_t(c * 2 + who);
    out.shotsTaken = out.goals + int32_t(c / 2);
    out.ping = int32_t(18 + who * 7 + c % 5);
    out.packetLossRatio = float(who) * 0.01f;
    out.isEmotePlaying = uint8_t((c + who) % 2);
    out.fill = kPlayerStatsFillAll;

    // The last player's packet loss is the one field this fixture never sources, so every fixture
    // carries at least one unclaimed value and the writer's fill rule is exercised by default.
    if (who + 1 == cfg.statsPlayers) {
        out.fill &= ~kPsFillPacketLoss;
        out.packetLossRatio = 0.0f;
    }
}

bool Write(const wchar_t* path, const Config& cfg, WriterStats* stats, uint32_t* chaptersOut) {
    Manifest m{};
    BuildManifest(cfg, m);

    DemoWriter w;
    if (!w.Open(path, m)) {
        return false;
    }

    std::vector<uint8_t> buf;
    uint32_t chapters = 0;
    uint32_t lastChapter = 0xFFFFFFFFu;
    bool statsOk = true;
    Plan(cfg, [&](const BlockPlan& b) {
        if (b.firstInChapter) {
            w.BeginChapter(b.section, b.tBeginUs);
            if (b.chapter != lastChapter) {
                lastChapter = b.chapter;
                if (cfg.sleepMsPerChapter) {
                    Sleep(cfg.sleepMsPerChapter);
                }
            }
        }
        FillBlock(cfg, b, buf);
        w.WriteStreamBlock(b.stream, b.schema, buf.data(), buf.size(), b.observed, b.storedClaim);
        if (b.lastInChapter && b.section == kSectionCore) {
            // Both writer paths in one chapter: the keyframe copy every player gets, and the on-change
            // record for the players whose scoreboard actually moved since the previous chapter.
            for (uint32_t who = 0; who < cfg.statsPlayers; ++who) {
                PlayerStatsRecord now{};
                PlanPlayerStats(cfg, b.chapter, who, now);
                now.keyframe = 1;
                now.engineTimeUs = b.tBeginUs;
                statsOk = statsOk && w.WritePlayerStats(now);
                if (b.chapter == 0) {
                    continue;
                }
                PlayerStatsRecord before{};
                PlanPlayerStats(cfg, b.chapter - 1, who, before);
                if (before.goals == now.goals && before.points == now.points) {
                    continue;
                }
                now.keyframe = 0;
                now.engineTimeUs = b.tBeginUs + (b.tEndUs - b.tBeginUs) / 2;
                statsOk = statsOk && w.WritePlayerStats(now);
            }
        }
        if (b.lastInChapter) {
            w.EndChapter(b.tEndUs);
            ++chapters;
        }
    });

    w.Close();
    if (!statsOk) {
        return false;
    }
    if (stats) {
        *stats = w.Stats();
    }
    if (chaptersOut) {
        *chaptersOut = chapters;
    }
    return true;
}

uint64_t VerifyRoundTrip(const wchar_t* path, const Config& cfg, std::string* error) {
    auto fail = [&](const char* why) -> uint64_t {
        if (error) {
            *error = why;
        }
        return 0;
    };

    DemoReader r;
    if (!r.Open(path)) {
        return fail(r.RefusalReason() ? r.RefusalReason() : "open failed");
    }

    const Manifest& m = r.GetManifest();
    if (m.playerCount != cfg.playerCount) {
        return fail("player count did not round-trip");
    }
    if (m.schemaCount != kSchemaCount) {
        return fail("schema count did not round-trip");
    }
    if (m.levelId != 0xDEADBEEFull || m.gametypeId != 0xCAFEBABEull) {
        return fail("manifest scalars did not round-trip");
    }
    if (m.balanceBlobZstd.n != 3334 || m.balanceBlobZstd.p[0] != 'z') {
        return fail("the balance blob did not round-trip verbatim");
    }
    if (m.players[0].unlocksJson.n != 512) {
        return fail("the unlocks blob did not round-trip");
    }

    // Rebuild the expectation from the same plan the writer walked.
    struct Expect {
        BlockPlan b;
    };
    std::vector<Expect> expected;
    Plan(cfg, [&](const BlockPlan& b) { expected.push_back(Expect{b}); });

    uint64_t compared = 0;
    size_t at = 0;
    std::vector<uint8_t> want;

    // MANIFEST is chapter 0; content chapters follow in plan order.
    if (r.ChapterCount() == 0 || r.ChapterAt(0).section != kSectionManifest) {
        return fail("chapter 0 is not MANIFEST");
    }

    size_t chapterIdx = 1;
    while (at < expected.size()) {
        if (chapterIdx >= r.ChapterCount()) {
            return fail("the file ran out of chapters before the plan did");
        }
        DecodedChapter c{};
        if (!r.DecodeChapter(chapterIdx, c)) {
            return fail(ChapterFaultName(r.LastFault()));
        }
        const ChapterKey key = r.ChapterAt(chapterIdx);
        if (key.section != expected[at].b.section || key.tBeginUs != expected[at].b.tBeginUs ||
            key.tEndUs != expected[at].b.tEndUs) {
            return fail("a chapter key does not match the plan");
        }

        uint16_t seen = 0;
        for (;;) {
            const BlockPlan& b = expected[at].b;
            if (seen >= c.streamCount) {
                return fail("a chapter holds fewer blocks than the plan");
            }
            const StreamView& v = c.streams[seen];
            if (v.id != b.stream || v.schema != b.schema) {
                return fail("a stream block is not the one the plan placed there");
            }
            if (v.storedCount != b.storedClaim || v.observedCount != b.observed) {
                return fail("a block's completeness counters did not round-trip");
            }
            FillBlock(cfg, b, want);
            if (v.bytes != want.size()) {
                return fail("a block's byte length did not round-trip");
            }
            if (v.bytes && memcmp(want.data(), v.records, v.bytes) != 0) {
                return fail("record bytes did not round-trip");
            }
            compared += b.recordCount;
            ++seen;
            ++at;
            if (b.lastInChapter) {
                break;
            }
        }
        ++chapterIdx;
    }
    if (error) {
        error->clear();
    }
    return compared;
}

}  // namespace synth
