#include "foundation/demofile/format.h"

#include <string.h>

namespace replay {
namespace {

struct Crc32Table {
    uint32_t v[256];
    constexpr Crc32Table() : v() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};

constexpr Crc32Table kCrc32;

}  // namespace

uint32_t Crc32(const void* data, size_t bytes, uint32_t seed) {
    uint32_t c = seed ^ 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        c = kCrc32.v[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t HeaderCrc(const FileHeaderDisk& h) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&h);
    const uint32_t front = Crc32(p, offsetof(FileHeaderDisk, headerCrc32));
    return Crc32(p + offsetof(FileHeaderDisk, headerCrc32) + sizeof(h.headerCrc32),
                 sizeof(h) - offsetof(FileHeaderDisk, headerCrc32) - sizeof(h.headerCrc32), front);
}

const char* SectionName(uint16_t section) {
    switch (section) {
        case kSectionManifest: return "MANIFEST";
        case kSectionCore: return "CORE";
        case kSectionAsSeen: return "ASSEEN";
        case kSectionVoip: return "VOIP";
        case kSectionOracle: return "ORACLE";
        case kSectionIndex: return "INDEX";
        default: return "?";
    }
}

const char* StreamName(uint16_t stream) {
    switch (stream) {
        case kStreamManifest: return "S_MANIFEST";
        case kStreamIndex: return "S_INDEX";
        case kStreamTick: return "S_TICK";
        case kStreamPlayerWire: return "S_PLAYER_WIRE";
        case kStreamPlayerSampled: return "S_PLAYER_SAMPLED";
        case kStreamBodyWire: return "S_BODY_WIRE";
        case kStreamBodySampled: return "S_BODY_SAMPLED";
        case kStreamNetvar: return "S_NETVAR";
        case kStreamEvent: return "S_EVENT";
        case kStreamWorld: return "S_WORLD";
        case kStreamKey: return "S_KEY";
        case kStreamMark: return "S_MARK";
        case kStreamAsSeen: return "S_ASSEEN";
        case kStreamVoip: return "S_VOIP";
        case kStreamOracle: return "S_ORACLE";
        case kStreamGap: return "S_GAP";
        case kStreamDropped: return "S_DROPPED";
        case kStreamDegraded: return "S_DEGRADED";
        case kStreamGameState: return "S_GAMESTATE";
        case kStreamPlayerStats: return "S_PLAYER_STATS";
        default: return "S_?";
    }
}

const char* MarkerSourceName(uint8_t source) {
    switch (source) {
        case static_cast<uint8_t>(MarkerSource::NotRecorded): return "not_recorded";
        case static_cast<uint8_t>(MarkerSource::None): return "none";
        case static_cast<uint8_t>(MarkerSource::Local): return "local";
        case static_cast<uint8_t>(MarkerSource::Wire): return "wire";
        default: return "?";
    }
}

namespace {
// Position i names on-disk present bit i. The switch below, not the array order, is what ties each
// name to its bit: a constant that moves fails to compile here rather than printing the wrong name.
const char* const kGameStateFieldNames[] = {
    "phaseSymbol", "pausedSymbol",       "roundCounter",     "roundPoints",      "matchScore",
    "frisbeeOwnerId", "gameTimer",       "countdownTimer",   "firstLaunchOfRound",
    "roundWinningTeam", "lastScore",
};
static_assert(sizeof kGameStateFieldNames / sizeof kGameStateFieldNames[0] == kGameStateFieldCount,
              "one name per on-disk present bit");
static_assert(kGsPhaseSymbol == 1u << 0, "phaseSymbol is on-disk bit 0");
static_assert(kGsPausedSymbol == 1u << 1, "pausedSymbol is on-disk bit 1");
static_assert(kGsRoundCounter == 1u << 2, "roundCounter is on-disk bit 2");
static_assert(kGsRoundPoints == 1u << 3, "roundPoints is on-disk bit 3");
static_assert(kGsMatchScore == 1u << 4, "matchScore is on-disk bit 4");
static_assert(kGsFrisbeeOwnerId == 1u << 5, "frisbeeOwnerId is on-disk bit 5");
static_assert(kGsGameTimer == 1u << 6, "gameTimer is on-disk bit 6");
static_assert(kGsCountdownTimer == 1u << 7, "countdownTimer is on-disk bit 7");
static_assert(kGsFirstLaunchOfRound == 1u << 8, "firstLaunchOfRound is on-disk bit 8");
static_assert(kGsRoundWinningTeam == 1u << 9, "roundWinningTeam is on-disk bit 9");
static_assert(kGsLastScore == 1u << 10, "lastScore is on-disk bit 10");
}  // namespace

const char* GameStateFieldName(unsigned bitIndex) {
    if (bitIndex >= kGameStateFieldCount) {
        return nullptr;
    }
    return kGameStateFieldNames[bitIndex];
}

bool RoundNumberFromCounter(uint32_t counter, uint16_t& out) {
    if (counter > kMaxRoundNumber) {
        out = kRoundNumberNone;
        return false;
    }
    out = static_cast<uint16_t>(counter);
    return true;
}


namespace {

// The one place a fill bit and the value it claims are tied together. Everything that has to keep the
// mask and the numbers agreeing -- the writer's fill rule and verify's consistency check -- walks
// these, so the two cannot drift apart.
struct IntStatSlot {
    uint32_t bit;
    int32_t PlayerStatsRecordDisk::*field;
};
const IntStatSlot kIntStats[] = {
    {kPsFillPoints, &PlayerStatsRecordDisk::points},
    {kPsFillGoals, &PlayerStatsRecordDisk::goals},
    {kPsFillStuns, &PlayerStatsRecordDisk::stuns},
    {kPsFillBlocks, &PlayerStatsRecordDisk::blocks},
    {kPsFillAssists, &PlayerStatsRecordDisk::assists},
    {kPsFillSaves, &PlayerStatsRecordDisk::saves},
    {kPsFillInterceptions, &PlayerStatsRecordDisk::interceptions},
    {kPsFillSteals, &PlayerStatsRecordDisk::steals},
    {kPsFillCatches, &PlayerStatsRecordDisk::catches},
    {kPsFillPasses, &PlayerStatsRecordDisk::passes},
    {kPsFillShotsTaken, &PlayerStatsRecordDisk::shotsTaken},
    {kPsFillPing, &PlayerStatsRecordDisk::ping},
};
struct FloatStatSlot {
    uint32_t bit;
    float PlayerStatsRecordDisk::*field;
};
const FloatStatSlot kFloatStats[] = {
    {kPsFillPossessionTime, &PlayerStatsRecordDisk::possessionTime},
    {kPsFillPacketLoss, &PlayerStatsRecordDisk::packetLossRatio},
};

}  // namespace

bool PlayerStatsNoteTime(PlayerStatsCursor& cursor, uint64_t xplatformId, uint64_t engineTimeUs) {
    for (uint16_t i = 0; i < cursor.n; ++i) {
        if (cursor.id[i] != xplatformId) {
            continue;
        }
        if (engineTimeUs < cursor.lastEngineUs[i]) {
            return false;
        }
        cursor.lastEngineUs[i] = engineTimeUs;
        return true;
    }
    if (cursor.n >= kPlayerStatsMaxUsers) {
        return false;
    }
    cursor.id[cursor.n] = xplatformId;
    cursor.lastEngineUs[cursor.n] = engineTimeUs;
    ++cursor.n;
    return true;
}

void PlayerStatsToDisk(const PlayerStatsRecord& r, PlayerStatsRecordDisk& out) {
    out.xplatformId = r.xplatformId;
    out.engineTimeUs = r.engineTimeUs;
    out.captureTimeUs = r.captureTimeUs;
    out.possessionTime = r.possessionTime;
    out.points = r.points;
    out.goals = r.goals;
    out.stuns = r.stuns;
    out.blocks = r.blocks;
    out.assists = r.assists;
    out.saves = r.saves;
    out.interceptions = r.interceptions;
    out.steals = r.steals;
    out.catches = r.catches;
    out.passes = r.passes;
    out.shotsTaken = r.shotsTaken;
    out.ping = r.ping;
    out.packetLossRatio = r.packetLossRatio;
    out.fill = r.fill;
    out.userIndex = r.userIndex;
    out.isEmotePlaying = r.isEmotePlaying;
    out.keyframe = r.keyframe ? 1 : 0;

    for (const IntStatSlot& s : kIntStats) {
        if ((out.fill & s.bit) == 0) {
            out.*s.field = kPlayerStatNone;
        }
    }
    for (const FloatStatSlot& s : kFloatStats) {
        if ((out.fill & s.bit) == 0) {
            out.*s.field = kPlayerStatFloatNone;
        }
    }
    if ((out.fill & kPsFillEmote) == 0) {
        out.isEmotePlaying = kPlayerStatEmoteNone;
    }
}

void PlayerStatsFromDisk(const PlayerStatsRecordDisk& d, PlayerStatsRecord& out) {
    out.xplatformId = d.xplatformId;
    out.engineTimeUs = d.engineTimeUs;
    out.captureTimeUs = d.captureTimeUs;
    out.possessionTime = d.possessionTime;
    out.points = d.points;
    out.goals = d.goals;
    out.stuns = d.stuns;
    out.blocks = d.blocks;
    out.assists = d.assists;
    out.saves = d.saves;
    out.interceptions = d.interceptions;
    out.steals = d.steals;
    out.catches = d.catches;
    out.passes = d.passes;
    out.shotsTaken = d.shotsTaken;
    out.ping = d.ping;
    out.packetLossRatio = d.packetLossRatio;
    out.fill = d.fill;
    out.userIndex = d.userIndex;
    out.isEmotePlaying = d.isEmotePlaying;
    out.keyframe = d.keyframe;
}

const char* CheckPlayerStatsBlock(const void* data, uint32_t bytes, PlayerStatsCursor& cursor) {
    if (bytes % sizeof(PlayerStatsRecordDisk) != 0) {
        return "an S_PLAYER_STATS block is not a whole number of records";
    }
    const uint32_t n = bytes / static_cast<uint32_t>(sizeof(PlayerStatsRecordDisk));
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < n; ++i) {
        PlayerStatsRecordDisk d;
        memcpy(&d, p + size_t(i) * sizeof(d), sizeof(d));

        if ((d.fill & ~kPlayerStatsFillAll) != 0) {
            return "an S_PLAYER_STATS record's fill mask has an undefined bit";
        }
        if (d.xplatformId == 0 && d.userIndex == kPlayerStatsNoIndex) {
            return "an S_PLAYER_STATS record identifies no player";
        }
        if (d.keyframe > 1) {
            return "an S_PLAYER_STATS record's keyframe flag is neither set nor clear";
        }
        // Two fields, one fact, and both directions: a claimed stat that holds its absence value is a
        // mask promising a number that is not there, and an unclaimed stat holding anything else is a
        // number no mask vouches for. Either way the pair does not describe one capture.
        for (const IntStatSlot& s : kIntStats) {
            const bool claimed = (d.fill & s.bit) != 0;
            const bool absent = d.*s.field == kPlayerStatNone;
            if (claimed && absent) {
                return "an S_PLAYER_STATS record claims a stat whose value is the absence value";
            }
            if (!claimed && !absent) {
                return "an S_PLAYER_STATS record carries a stat its fill mask does not claim";
            }
        }
        for (const FloatStatSlot& s : kFloatStats) {
            const bool claimed = (d.fill & s.bit) != 0;
            const bool absent = d.*s.field == kPlayerStatFloatNone;
            if (claimed && absent) {
                return "an S_PLAYER_STATS record claims a stat whose value is the absence value";
            }
            if (!claimed && !absent) {
                return "an S_PLAYER_STATS record carries a stat its fill mask does not claim";
            }
        }
        if ((d.fill & kPsFillEmote) != 0) {
            if (d.isEmotePlaying > 1) {
                return "an S_PLAYER_STATS record's emote flag is neither true nor false";
            }
        } else if (d.isEmotePlaying != kPlayerStatEmoteNone) {
            return "an S_PLAYER_STATS record carries a stat its fill mask does not claim";
        }
        if (!PlayerStatsNoteTime(cursor, d.xplatformId, d.engineTimeUs)) {
            return "S_PLAYER_STATS engine time went backwards for a user, or the stream names more "
                   "users than a match can hold";
        }
    }
    return nullptr;
}

bool StreamCarriesSamples(uint16_t stream) {
    switch (stream) {
        case kStreamManifest:
        case kStreamIndex:
        case kStreamGap:
        case kStreamDropped:
        case kStreamDegraded:
        // S_PLAYER_STATS is composed by the writer from single records the producer hands it, not by a
        // sampler walking an engine sequence. There is no engine numbering of stat changes, so an
        // `observed` for it could only ever be our own loop count -- which is precisely the counter
        // B.2 forbids. A stream whose deficit cannot be measured must stay out of the completeness
        // law rather than be fed a fabricated denominator.
        case kStreamPlayerStats:
            return false;
        default:
            return true;
    }
}

}  // namespace replay
