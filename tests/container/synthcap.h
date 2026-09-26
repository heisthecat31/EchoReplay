#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"

// A scripted capture with the shape of a real one: keyframes, a tick spine, per-player state, netvars at
// a higher rate than the tick, markers, and optional ASSEEN / VOIP / ORACLE sections.
//
// Plan() below is the single description of what a capture contains. The writer and the round-trip check
// both walk it, so the expectation cannot drift away from what was written.

namespace synth {

struct Config {
    uint32_t chapters = 12;
    uint32_t ticksPerChapter = 88;  // ~2 s at 44 Hz, the S5.5 cadence
    uint32_t playerCount = 8;

    // Display names for the first players, when a fixture needs names that cannot be confused
    // with anything else a tool prints. Beyond its size the default numbering continues.
    std::vector<std::string> playerNames;
    uint64_t t0Us = 1700000000000000ull;
    uint64_t chapterSpanUs = 2000000;
    uint32_t keyRecordBytes = 1800;
    bool voip = true;
    bool asSeen = true;
    bool oracle = true;
    // S_ORACLE the shape the capture writes it: 117 records over the file, at one width for the
    // first chapters and a wider one after. The F10 record-width rule learns a width from a stream's
    // own blocks, so a stream that legitimately varies has to be exempt or it fails its own file.
    bool oracleVariableWidth = false;

    // The S5.6.2 mutation, expressed as sampling rather than as counting: the netvar sampler stores one
    // record in `sampleDivisor`, starting at `divisorFromChapter`. `honestObserved` decides whether the
    // block reports the engine's own numbering or the sampler's own tally -- the second is the counter
    // that cannot detect under-sampling, and it is here so its blindness can be measured.
    uint32_t sampleDivisor = 1;
    uint32_t divisorFromChapter = 0;
    bool honestObserved = true;
    // Thin S_ASSEEN by the same divisor. S_NETVAR is written on change, so verify never scores its rate;
    // S_ASSEEN is one record per engine frame, where a halved sampler is a rate step verify can see.
    bool divisorThinsAsSeen = false;

    // Claim twice as many stored records as the block actually holds, from this chapter on, with
    // `observed` moved to match so the counter law stays balanced. This is the F10 attack: both counters
    // and the payload are caller assertions, and nothing related them until verify learned the stream's
    // own record width. 0xFFFFFFFF disables it.
    uint32_t lieStoredFromChapter = 0xFFFFFFFFu;

    uint32_t sleepMsPerChapter = 0;  // lets a killer land inside the run

    // S_KEY as REAL keyframe blobs, in the layout chapterbuilder writes: [u32 bit length][packed raw
    // records], each record a keyframe blob carrying TAP_TABLE and TAP_REGISTRATION. kTapRegOff leaves
    // the stream as opaque fill, which is what a capture recorded before the block existed looks like
    // to a reader -- so the OFF fixture is the pre-block file, not a file this test forgot to write.
    uint32_t tapReg = 0;

    // S_PLAYER_STATS, so both of the writer's paths are exercised by every fixture: a keyframe copy
    // for each player in every CORE chapter, plus the on-change records the chapter's scoring produced.
    // The block the writer composes lands after the plan's last block in the chapter, which is why
    // VerifyRoundTrip still matches -- it walks the plan and stops there.
    uint32_t statsPlayers = 3;
};

enum : uint32_t {
    kTapRegOff = 0,
    kTapRegPresent = 1,
    kTapRegCorrupt = 2,    // total that the four states do not sum to; the loader's reader refuses it
    kTapRegTableSkew = 3,  // identities close, but tableRows disagrees with TAP_TABLE's own row count
};

struct BlockPlan {
    uint32_t chapter;
    uint16_t section;
    uint64_t tBeginUs, tEndUs;
    uint16_t stream;
    uint16_t schema;
    uint32_t recordBytes;
    uint32_t recordCount;   // records actually written into the block
    uint32_t storedClaim;   // what the block header says, which is not always the same thing
    uint32_t observed;
    bool firstInChapter;
    bool lastInChapter;
};

// Schema ids, and the retention each declares. S_NETVAR is retention ALL at a rate above the tick, which
// is the stream S5.6.1 says must never be decimated.
enum : uint16_t {
    kSchemaTick = 1,
    kSchemaPlayer = 2,
    kSchemaNetvar = 3,
    kSchemaEvent = 4,
    kSchemaMark = 5,
    kSchemaKey = 6,
    kSchemaAsSeen = 7,
    kSchemaVoip = 8,
    kSchemaOracle = 9,
    kSchemaWorld = 10,
    kSchemaCount = 10,
};

void FillRecord(uint8_t* p, uint32_t bytes, uint32_t chapter, uint16_t stream, uint32_t index);

// The scoreboard this fixture scripts: player `who`'s stats as they stand at the end of chapter
// `chapter`. Derived, not tabulated, so a fixture with more chapters needs no new numbers -- and the
// values differ per player and per chapter, so a record attributed to the wrong one is visible.
void PlanPlayerStats(const Config& cfg, uint32_t chapter, uint32_t who, replay::PlayerStatsRecord& out);
void BuildManifest(const Config& cfg, replay::Manifest& out);
bool Write(const wchar_t* path, const Config& cfg, replay::WriterStats* stats, uint32_t* chaptersOut);
uint64_t VerifyRoundTrip(const wchar_t* path, const Config& cfg, std::string* error);

template <typename Fn>
void Plan(const Config& cfg, Fn rawFn) {
    using namespace replay;
    // storedClaim defaults to the truth and is bent in exactly one place, so no call site can forget it.
    auto fn = [&](BlockPlan& b) {
        b.storedClaim = b.recordCount;
        if (b.stream == kStreamNetvar && b.chapter >= cfg.lieStoredFromChapter) {
            b.storedClaim = b.recordCount * 2;
            b.observed = b.storedClaim;
        }
        rawFn(const_cast<const BlockPlan&>(b));
    };
    for (uint32_t c = 0; c < cfg.chapters; ++c) {
        const uint64_t tb = cfg.t0Us + uint64_t(c) * cfg.chapterSpanUs;
        const uint64_t te = tb + cfg.chapterSpanUs;
        const uint32_t divisor =
            (cfg.sampleDivisor > 1 && c >= cfg.divisorFromChapter) ? cfg.sampleDivisor : 1;

        BlockPlan b{};
        b.chapter = c;
        b.tBeginUs = tb;
        b.tEndUs = te;

        // ---- CORE ---------------------------------------------------------------------------
        b.section = kSectionCore;
        b.firstInChapter = true;
        b.lastInChapter = false;

        b.stream = kStreamKey;
        b.schema = kSchemaKey;
        b.recordBytes = cfg.keyRecordBytes;
        b.recordCount = 1;
        b.observed = 1;
        fn(b);
        b.firstInChapter = false;

        b.stream = kStreamTick;
        b.schema = kSchemaTick;
        b.recordBytes = 8;
        b.recordCount = cfg.ticksPerChapter;
        b.observed = cfg.ticksPerChapter;
        fn(b);

        b.stream = kStreamPlayerWire;
        b.schema = kSchemaPlayer;
        b.recordBytes = 68;
        b.recordCount = cfg.ticksPerChapter * cfg.playerCount;
        b.observed = b.recordCount;
        fn(b);

        // Two netvar samples per tick: the 118 Hz combined rate S5.6.1 refuses to decimate.
        const uint32_t netvarObserved = cfg.ticksPerChapter * 2;
        b.stream = kStreamNetvar;
        b.schema = kSchemaNetvar;
        b.recordBytes = 17;
        b.recordCount = netvarObserved / divisor;
        b.observed = cfg.honestObserved ? netvarObserved : b.recordCount;
        fn(b);

        b.stream = kStreamEvent;
        b.schema = kSchemaEvent;
        b.recordBytes = 24;
        b.recordCount = 3;
        b.observed = 3;
        fn(b);

        b.stream = kStreamWorld;
        b.schema = kSchemaWorld;
        b.recordBytes = 12;
        b.recordCount = 2;
        b.observed = 2;
        fn(b);

        b.stream = kStreamMark;
        b.schema = kSchemaMark;
        b.recordBytes = 20;
        b.recordCount = (c % 4 == 0) ? 1 : 0;
        b.observed = b.recordCount;
        b.lastInChapter = true;
        if (b.recordCount) {
            fn(b);
        } else {
            // Keep the chapter closed even when no marker landed in it.
            b.stream = kStreamWorld;
            b.schema = kSchemaWorld;
            b.recordBytes = 12;
            b.recordCount = 1;
            b.observed = 1;
            fn(b);
        }
        b.lastInChapter = false;

        // ---- ASSEEN -------------------------------------------------------------------------
        if (cfg.asSeen) {
            b.section = kSectionAsSeen;
            b.stream = kStreamAsSeen;
            b.schema = kSchemaAsSeen;
            b.recordBytes = 40;
            b.recordCount = cfg.divisorThinsAsSeen ? cfg.ticksPerChapter / divisor : cfg.ticksPerChapter;
            b.observed = cfg.honestObserved ? cfg.ticksPerChapter : b.recordCount;
            b.firstInChapter = true;
            b.lastInChapter = true;
            fn(b);
            b.firstInChapter = false;
            b.lastInChapter = false;
        }

        // ---- VOIP ---------------------------------------------------------------------------
        if (cfg.voip) {
            b.section = kSectionVoip;
            b.stream = kStreamVoip;
            b.schema = kSchemaVoip;
            b.recordBytes = 96;
            b.recordCount = cfg.ticksPerChapter / 2;
            b.observed = b.recordCount;
            b.firstInChapter = true;
            b.lastInChapter = true;
            fn(b);
            b.firstInChapter = false;
            b.lastInChapter = false;
        }

        // ---- ORACLE -------------------------------------------------------------------------
        if (cfg.oracle) {
            b.section = kSectionOracle;
            b.stream = kStreamOracle;
            b.schema = kSchemaOracle;
            b.recordBytes = 300;
            b.recordCount = 1;
            if (cfg.oracleVariableWidth) {
                b.recordBytes = c < 3 ? 26 : 48;
                b.recordCount = (c + 1 == cfg.chapters) ? 7 : 10;
            }
            b.observed = b.recordCount;
            b.firstInChapter = true;
            b.lastInChapter = true;
            fn(b);
        }
    }
}

}  // namespace synth
