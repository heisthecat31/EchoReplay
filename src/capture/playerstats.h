// THE LIVE SCOREBOARD ON THE CAPTURE TICK -- ARCH-21 MAJOR-B2, the producer half.
//
// W1-B built the format (S_PLAYER_STATS, kStreamPlayerStats, PlayerStatsRecord) and W1-E built the
// read (identity_access::ReadPlayerStats). Nothing joined them: no capture ever produced a record,
// so `echodemo info` printed `PLAYERSTATS 0 record(s)` on every real file. This header is the join,
// and it is a header rather than a .cpp for the same reason roster.h is: the composition has to be
// drivable from a test with no engine behind it.
//
// ★ WHY A GATE, AND WHY THIS GATE. ReadPlayerStats is O(1) per entrant but it is not free -- W1-E's
// own suite measures it at ~1.0 us per player. Sixteen entrants read on every one of 90 ticks a
// second is 16 us of tick, which is eight times the budget this phase declared and is exactly the
// shape of the defect L2-RUN-5 F1 found in the roster (1247-1371 us on every one of 1489 ticks, the
// cost meter killing the capture at 50 s). So the sweep runs only when a signal MOVES.
//
// ★ WHY THE SIGNAL COSTS NOTHING. It is composed from values the tick has ALREADY read this pass --
// the S_GAMESTATE sample's round counter, the two point totals, the last score, the phase word --
// plus the roster's own row count. Not one extra engine load is issued to decide whether to read.
// A signal that had to be sampled would be a second read of the thing we are trying not to read.
//
// ★ WHY A CADENCE AS WELL. The signal sees scoring, rounds and composition. It does NOT see stuns,
// blocks, passes or catches, which move with no scoreboard consequence -- so a signal-only gate
// would hold a stale stun count for a whole match. `intervalUs` is the declared bound on how stale
// any stat can get, passed in rather than baked in, exactly as kRosterRescanIntervalUs is. It is
// also what guarantees a sweep on a build where the game state cannot be sampled at all: the signal
// is then constant and the cadence is the only arm that fires, which is the honest degradation.
#pragma once

#include <stdint.h>
#include <string.h>

#include "capture/capture_types.h"
#include "capture/roster.h"
#include "engine/identity_access.h"
#include "foundation/demofile/demotypes.h"

namespace replay {
namespace capture {

// ★ THE FIFTEEN NAMES, TIED. W1-B's report asks W2-A to static_assert its capture-side mirror
// against the format's numbering. A third copy of the numbering would be a third thing to keep in
// step, so the mirror is this assertion and nothing else: the READER's bit and the DISK's bit are
// two statements of one fact, and the capture is where they meet. Renumber either side and the
// build stops here rather than writing a mask a reader decodes against the other numbering.
static_assert(uint16_t(identity_access::kPsFillPossessionTime) == uint16_t(kPsFillPossessionTime), "possession_time bit");
static_assert(uint16_t(identity_access::kPsFillPoints) == uint16_t(kPsFillPoints), "points bit");
static_assert(uint16_t(identity_access::kPsFillGoals) == uint16_t(kPsFillGoals), "goals bit");
static_assert(uint16_t(identity_access::kPsFillStuns) == uint16_t(kPsFillStuns), "stuns bit");
static_assert(uint16_t(identity_access::kPsFillBlocks) == uint16_t(kPsFillBlocks), "blocks bit");
static_assert(uint16_t(identity_access::kPsFillAssists) == uint16_t(kPsFillAssists), "assists bit");
static_assert(uint16_t(identity_access::kPsFillSaves) == uint16_t(kPsFillSaves), "saves bit");
static_assert(uint16_t(identity_access::kPsFillInterceptions) == uint16_t(kPsFillInterceptions), "interceptions bit");
static_assert(uint16_t(identity_access::kPsFillSteals) == uint16_t(kPsFillSteals), "steals bit");
static_assert(uint16_t(identity_access::kPsFillCatches) == uint16_t(kPsFillCatches), "catches bit");
static_assert(uint16_t(identity_access::kPsFillPasses) == uint16_t(kPsFillPasses), "passes bit");
static_assert(uint16_t(identity_access::kPsFillShotsTaken) == uint16_t(kPsFillShotsTaken), "shots_taken bit");
static_assert(uint16_t(identity_access::kPsFillPing) == uint16_t(kPsFillPing), "ping bit");
static_assert(uint16_t(identity_access::kPsFillPacketLoss) == uint16_t(kPsFillPacketLoss), "packetlossratio bit");
static_assert(uint16_t(identity_access::kPsFillEmote) == uint16_t(kPsFillEmote), "is_emote_playing bit");
static_assert(uint32_t(identity_access::kPlayerStatsFillAll) == kPlayerStatsFillAll, "the whole mask");

// The stats are re-read at this cadence even when nothing the signal can see has moved. One second
// is 90 ticks of a sweep that costs ~1 us per entrant: a median of zero over any window the cost
// meter measures, and a worst case a reader can name.
constexpr uint64_t kStatsRescanIntervalUs = 1000000;

// Why a sweep ran, or did not. Kept apart because "the gate refused" and "there was no source" are
// different findings and a count of zero records cannot separate them.
enum class StatsSweepWhy : uint8_t {
    SignalUnchanged = 0,  // the gate answered "nothing moved" and NO engine read ran
    SignalMoved,
    Cadence,
    Forced,               // the open-time sweep, which arms the cadence
    SourceAbsent,
};

struct StatsCadence {
    uint64_t lastSweepUs;
    uint64_t lastSignal;
    uint8_t  haveSignal;
};

struct StatsSweepReport {
    uint16_t entrants;    // roster rows the sweep considered
    uint16_t read;        // entrants ReadPlayerStats actually answered for
    uint16_t refused;     // entrants it refused, by reason below
    uint16_t written;     // records emitted (change plus keyframe)
    uint16_t keyframes;   // of those, periodic copies
    StatsSweepWhy why;
    StatsRefusals refusals;
};

// What one entrant's stats looked like when we last WROTE them, so a record is emitted only when a
// value moves. The fill mask is part of the shadow: a stat that stops being sourceable is a change,
// and a reader must see the bit go out rather than keep decoding the last value we had.
struct StatsShadow {
    identity_access::PlayerStats v;
    uint64_t lastWriteUs;
    uint16_t fill;
    uint8_t  have;
};

struct PlayerStatsStore {
    StatsShadow rows[kRosterMaxRows];
    StatsCadence cadence;
};

// The engine read, behind an interface for the same reason IRosterSource is: the composition -- the
// gate, the change test, the keyframe cadence -- is the part with the bugs in it, and it must be
// drivable with no engine in the process.
class IPlayerStatsSource {
public:
    virtual ~IPlayerStatsSource() {}
    virtual bool Usable() const = 0;
    // `fill` is written on success and names which of `out`'s fields the engine answered for. A
    // field whose bit is clear is NOT written and must never be read as a score.
    // `reason` and `why` are the engine's own refusal code and word, handed back by the source
    // exactly as IRosterSource does it: this header must not link the identity module, so the
    // translation from Refusal to text happens on the side that already has it.
    virtual bool Read(uint16_t userIndex, identity_access::PlayerStats& out, uint16_t& fill,
                      uint8_t& reason, const char*& why) const = 0;
};

// kRecPlayerStats' payload. It carries the reader's PlayerStats verbatim plus the two clocks and
// the identity, and NOT the format's PlayerStatsRecord: the worker builds that, so the absence
// substitution happens once, on the side that owns the sentinels.
#pragma pack(push, 1)
struct PlayerStatsRec {
    uint64_t sourceTimeUs;   // the file's axis, the same stamp the tick's other records carry
    uint64_t engineTimeUs;   // the engine clock; per user this never goes backwards
    uint64_t xplatformId;    // 0 when the roster row could not source an account id
    identity_access::PlayerStats v;
    uint16_t fill;           // exactly what ReadPlayerStats answered; never widened here
    uint16_t userIndex;
    uint8_t  keyframe;       // 1 = the periodic copy, 0 = written because a value moved
};
#pragma pack(pop)

typedef void (*PlayerStatsEmit)(void* ctx, const PlayerStatsRec& rec);

// The cheap change signal, composed from values the tick has already read. Every argument is a
// value some other phase paid for; this function issues no load of its own.
inline uint64_t StatsSignalOf(uint32_t roundCounter, uint32_t bluePoints, uint32_t redPoints,
                              uint32_t lastScorePoints, uint16_t lastScoreTeam, uint8_t phase,
                              uint8_t availability, uint16_t rosterRows) {
    // A plain mix, not a hash: every field gets its own bits, so no two states of the scoreboard
    // can collide into one signal word. Collisions here would be missed reads, which is the one
    // failure this gate must not have.
    // The field width is DERIVED from the shift it is placed at, never transcribed: a mask and the
    // place its field sits in are one fact, and a mask spelled as a literal is a second copy of it.
    constexpr uint64_t kField16 = (uint64_t(1) << 16) - uint64_t(1);
    uint64_t s = uint64_t(roundCounter) & kField16;
    s |= (uint64_t(bluePoints) & kField16) << 16;
    s |= (uint64_t(redPoints) & kField16) << 32;
    s |= (uint64_t(lastScorePoints) & 0xFFu) << 48;
    s |= (uint64_t(lastScoreTeam) & 0x7u) << 56;
    s |= (uint64_t(phase) & 0x1Fu) << 59;
    // availability and the row count ride in the high nibble by xor: a source that starts or stops
    // answering, and a player joining or leaving, are both changes the scoreboard must follow.
    s ^= (uint64_t(availability) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(rosterRows) << 5);
    return s;
}

// True when a sweep should run. `force` is the open-time sweep: it runs unconditionally and arms
// the cadence, so the first tick after an open does not sweep twice.
inline bool StatsSweepDue(StatsCadence& cad, uint64_t signal, uint64_t tUs, uint64_t intervalUs,
                          bool force, StatsSweepWhy& why) {
    const bool moved = !cad.haveSignal || signal != cad.lastSignal;
    // `tUs < lastSweepUs` is a clock that went backwards, which a capture axis can do at a chapter
    // boundary. Treat it as due rather than as "not yet": never being due again is the worse fault.
    const bool due = !cad.haveSignal || tUs < cad.lastSweepUs ||
                     (intervalUs && tUs - cad.lastSweepUs >= intervalUs);
    if (!force && !moved && !due) {
        why = StatsSweepWhy::SignalUnchanged;
        return false;
    }
    why = force ? StatsSweepWhy::Forced : (moved ? StatsSweepWhy::SignalMoved : StatsSweepWhy::Cadence);
    cad.lastSignal = signal;
    cad.haveSignal = 1;
    cad.lastSweepUs = tUs;
    return true;
}

// A value moved, or a bit went out. Compared field by field rather than with memcmp: PlayerStats
// has no padding today, but a memcmp would start reporting padding as a scoreboard change the day
// it gains a field of a different width.
inline bool StatsChanged(const identity_access::PlayerStats& a, uint16_t af,
                         const identity_access::PlayerStats& b, uint16_t bf) {
    if (af != bf) return true;
    // Only claimed fields are compared. An unclaimed field holds whatever the caller left there,
    // and comparing it would emit records for a value nobody sourced.
    if ((af & kPsFillPossessionTime) && a.possessionTime != b.possessionTime) return true;
    if ((af & kPsFillPoints) && a.points != b.points) return true;
    if ((af & kPsFillGoals) && a.goals != b.goals) return true;
    if ((af & kPsFillStuns) && a.stuns != b.stuns) return true;
    if ((af & kPsFillBlocks) && a.blocks != b.blocks) return true;
    if ((af & kPsFillAssists) && a.assists != b.assists) return true;
    if ((af & kPsFillSaves) && a.saves != b.saves) return true;
    if ((af & kPsFillInterceptions) && a.interceptions != b.interceptions) return true;
    if ((af & kPsFillSteals) && a.steals != b.steals) return true;
    if ((af & kPsFillCatches) && a.catches != b.catches) return true;
    if ((af & kPsFillPasses) && a.passes != b.passes) return true;
    if ((af & kPsFillShotsTaken) && a.shotsTaken != b.shotsTaken) return true;
    if ((af & kPsFillPing) && a.ping != b.ping) return true;
    if ((af & kPsFillPacketLoss) && a.packetLossRatio != b.packetLossRatio) return true;
    if ((af & kPsFillEmote) && a.isEmotePlaying != b.isEmotePlaying) return true;
    return false;
}

// One sweep of the roster's active rows. Returns false when nothing was read -- the gate refused,
// or there is no source -- and `out.why` says which.
//
// The record carries the READER's fill mask unchanged. Nothing here sets a bit, and nothing here
// substitutes a value for a clear one: the on-disk absence sentinels are the format's and there is
// one copy of them, in PlayerStatsToDisk. A capture that fabricated a zero for a stat it could not
// source would be writing a scoreboard nobody played.
inline bool SweepPlayerStats(const IPlayerStatsSource* src, const RosterTable& roster,
                             PlayerStatsStore& st, uint64_t tUs, uint64_t engineUs,
                             uint64_t signal, uint64_t intervalUs, uint64_t keyframeUs, bool force,
                             PlayerStatsEmit emit, void* ctx, StatsSweepReport& out) {
    out = StatsSweepReport{};
    if (!src || !src->Usable()) {
        out.why = StatsSweepWhy::SourceAbsent;
        return false;
    }
    if (!StatsSweepDue(st.cadence, signal, tUs, intervalUs, force, out.why)) {
        return false;
    }

    const uint16_t rows = roster.Count();
    for (uint16_t i = 0; i < rows && i < kRosterMaxRows; ++i) {
        const RosterRow& row = roster.At(i);
        if (!row.active) {
            continue;
        }
        ++out.entrants;

        identity_access::PlayerStats v = {};
        uint16_t fill = 0;
        uint8_t reason = 0;
        const char* why = nullptr;
        if (!src->Read(row.userIndex, v, fill, reason, why)) {
            ++out.refused;
            out.refusals.Note(reason, why);
            continue;
        }
        ++out.read;

        StatsShadow& sh = st.rows[i];
        const bool periodic = !sh.have || tUs < sh.lastWriteUs ||
                              (keyframeUs && tUs - sh.lastWriteUs >= keyframeUs);
        if (!periodic && !StatsChanged(v, fill, sh.v, sh.fill)) {
            continue;
        }

        PlayerStatsRec rec = {};
        rec.sourceTimeUs = tUs;
        rec.engineTimeUs = engineUs;
        rec.xplatformId = row.xplatformId;
        rec.v = v;
        rec.fill = fill;
        rec.userIndex = row.userIndex;
        rec.keyframe = periodic ? 1u : 0u;
        if (emit) {
            emit(ctx, rec);
        }
        ++out.written;
        out.keyframes += rec.keyframe;

        sh.v = v;
        sh.fill = fill;
        sh.have = 1;
        sh.lastWriteUs = tUs;
    }
    return true;
}

}  // namespace capture
}  // namespace replay
