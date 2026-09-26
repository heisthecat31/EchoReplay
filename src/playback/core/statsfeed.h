#pragma once
// THE PLAYBACK SIDE OF THE PER-PLAYER AND PER-TEAM STATS.
//
// The recorder already stored the two stat messages verbatim in S_EVENT.  Nothing decoded them, so
// during playback the spectator HUD's bottom row read POINTS 0 / ASSISTS 0 / SAVES 0 / STEALS 0 /
// STUNS 0 and the scoreboard's per-player columns were empty.  This file decodes those rows and
// feeds them back through the ENGINE'S OWN two appliers -- it does not reimplement what a stat is.
//
// LEVEL SEMANTICS, AND WHY A SCRUB IS FREE.  CGameStats::From zeroes every stat record it owns and
// then applies the payload, so a payload is a full REPLACE and not an increment.  Applying the
// newest row at or before the playhead is therefore idempotent, correct after a backward seek, and
// correct in reverse -- the same shape the recorded loadout uses, for the same reason.
//
// TWO AXES, AND THIS ONE IS THE FOREIGN ONE.  An S_EVENT row's stampUs is the tap's ARRIVAL axis
// (capture.cpp's WiretapArrivalNowUs; chapterbuilder's StampIsOnSourceAxis declares it foreign for
// kRecEvent) and is NOT comparable with the playhead.  Unlike an identity block, a stat body
// carries no capture-axis time of its own.  What the file does say is which CHAPTER a row was
// written into, so rows are placed inside their chapter's [tBegin, tEnd) by the same even spread
// windowbuild uses for an old-file netvar chapter: monotone, right on average for a steady stream,
// and bounded strictly inside the chapter.  It is an INTERPOLATION and the load line says so.

#include <stddef.h>
#include <stdint.h>

#include "foundation/demofile/demofile.h"

namespace replay {
namespace playback {
namespace statsfeed {

// The engine's own gate refuses a slot at or above this, so a row keyed past it can never be fed.
inline constexpr uint16_t kMaxSlots = 16;
inline constexpr uint16_t kMaxTeams = 2;
inline constexpr uint16_t kRowsPerKey = 256;
inline constexpr uint32_t kMaxRows = 2048;
inline constexpr uint32_t kArenaBytes = 256u * 1024u;

struct Payload {
    const uint8_t* bytes = nullptr;
    uint32_t len = 0;
};

// What the scan met, so a zero on the HUD can be told apart from a file that carries nothing.
// Every one of these is printed by the load line.
struct ScanCensus {
    uint32_t event_rows = 0;      // S_EVENT rows walked
    uint32_t tap_chapters = 0;    // chapters whose keyframe carried a tap table
    uint32_t player_rows = 0;     // SR15NetReliableStatUpdate rows stored
    uint32_t team_rows = 0;
    uint32_t refused_rows = 0;    // decoded as the right type and refused by the decoder
    uint32_t dropped_rows = 0;    // over a bound: arena, row cap or per-key cap
    uint32_t spread_rows = 0;     // rows the chapter spread moved off tBegin
    uint16_t player_tap = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    uint16_t team_tap = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const char* first_drop = nullptr;   // program-lifetime; names the bound that was hit
};

// The rows the file carries, keyed by recorded slot and by team.  Fixed storage: a load fills it
// once and every read after that is a binary search, so nothing here allocates on a tick.
class RecordedStats {
public:
    void Reset();

    // `tUs` is the chapter's begin; the spread below moves it.  Returns false and names the bound
    // in `census` when a row does not fit.
    bool OfferPlayer(uint64_t tUs, uint32_t entrantIndex, const void* json, uint32_t len,
                     ScanCensus& census);
    bool OfferTeam(uint64_t tUs, uint16_t team, const void* rows, uint32_t len, ScanCensus& census);

    // Marks where this chapter's rows begin, then spreads everything added since evenly inside
    // [tBeginUs, tEndUs) per key.  A chapter with no end after its begin leaves its rows on tBegin.
    void BeginChapter();
    void EndChapter(uint64_t tBeginUs, uint64_t tEndUs, ScanCensus& census);

    // The newest row at or before `tUs`, or false when this key has none there.  `rowId` is stable
    // for the life of the store and is what the applier compares to know a row has changed.
    bool PlayerAt(uint16_t slot, int64_t tUs, Payload& out, uint32_t& rowId) const;
    bool TeamAt(uint16_t team, int64_t tUs, Payload& out, uint32_t& rowId) const;

    uint32_t RowCount() const { return row_count_; }
    uint16_t PlayerSlotCount() const;
    uint16_t PlayerSlotAt(uint16_t i) const;
    uint16_t PlayerRowCount(uint16_t slot) const {
        return slot < kMaxSlots ? players_[slot].count : 0;
    }
    uint16_t TeamRowCount(uint16_t team) const {
        return team < kMaxTeams ? teams_[team].count : 0;
    }
    // The row a PlayerAt/TeamAt answered with, for a test that wants the payload by index.
    bool RowPayload(uint32_t rowId, Payload& out) const;
    uint64_t RowTimeUs(uint32_t rowId) const;

private:
    struct Row {
        uint64_t t_us = 0;
        uint32_t off = 0;
        uint32_t len = 0;
        // Which series this row belongs to: the slot, or kMaxSlots + the team. Carried on the row
        // so the chapter spread counts per key in one pass instead of searching every series.
        uint16_t key = 0;
    };
    struct Series {
        uint16_t count = 0;
        uint16_t rows[kRowsPerKey] = {};
    };

    bool Store(uint64_t tUs, const void* bytes, uint32_t len, uint16_t key, Series& into,
               ScanCensus& census);
    bool Resolve(const Series& s, int64_t tUs, Payload& out, uint32_t& rowId) const;

    Row rows_[kMaxRows];
    Series players_[kMaxSlots];
    Series teams_[kMaxTeams];
    uint32_t row_count_ = 0;
    uint32_t arena_used_ = 0;
    uint32_t chapter_first_row_ = 0;
    uint8_t arena_[kArenaBytes];
};

// Walks every chapter's S_EVENT and fills `into` from the two stat taps, resolving both type
// indices out of the FILE'S OWN keyframe tap table -- never a compiled-in index, for the reason
// windowbuild.h's standing rule states.  Returns the rows stored.
uint32_t ScanRecordedStats(IDemoReader& reader, RecordedStats& into, ScanCensus& census);

// The store the shipped load path fills.  One per process, like the recorded identity: a session
// plays one file at a time.
RecordedStats& PlaybackRecordedStats();
ScanCensus& PlaybackStatsCensus();

// ---------------------------------------------------------------------------------------------
// The feed.  Everything below reaches the engine.
// ---------------------------------------------------------------------------------------------

// The message the engine's Listen path hands its handlers: five qwords, three of them read.
struct EngineStatMsg {
    uint64_t selfPeerId = 0;
    uint64_t cookie = 0;
    uint64_t body = 0;
    uint64_t blob = 0;
    uint64_t blobLen = 0;
};
using StatApplyFn = void (*)(void* netGame, const void* msg);

struct Handlers {
    StatApplyFn player = nullptr;
    StatApplyFn team = nullptr;
};
// Test seam: a fake pair that records what it was handed.  Null restores the bind against the
// mapped image, which on a machine with no game resolves to nothing and refuses by name.
void InjectHandlersForTest(const Handlers* h);

// One mapped entrant: the recorded slot the rows are keyed by, and the live user index the roster
// mapping resolved it to.  The live SLOT and generation are read off the engine's own slot table,
// because that table is exactly what the handler's gate compares against.
struct MappedPlayer {
    uint16_t rec_slot = 0;
    uint16_t live_index = 0;
};

struct ApplyCensus {
    uint32_t player_writes = 0;
    uint32_t team_writes = 0;
    uint32_t zero_writes = 0;    // the fresh-entrant baseline, before a player's first row
    uint32_t unmapped = 0;       // a recorded slot the roster mapping does not carry
    uint32_t no_live_slot = 0;   // mapped, but the engine's slot table does not hold that index
    uint32_t reapplies = 0;      // the >=1 s cadence, against the host's own stat accrual
    uint32_t faulted = 0;
    const char* refusal = nullptr;   // program-lifetime; why the last sweep wrote nothing
};

// Armed by the load once the scan is in, disarmed by unload.  A disarmed feed writes nothing.
void Arm(bool on);
bool Armed();

// A discontinuity: every key re-applies on the next sweep whether or not its row changed.
void Invalidate();

// The file's origin, so the change lines speak the playhead's axis rather than the file's absolute
// one. Reporting only -- it never moves a row.
void SetPlayheadOriginUs(int64_t originUs);

// One stage-3 tail pass.  `nowMs` drives the re-apply cadence; see the note in the .cpp on why
// there is one at all.
void Sweep(void* netGame, int64_t tUs, const MappedPlayer* mapped, size_t mappedCount,
           uint64_t nowMs);

// RELEASE.  Zeroes every mapped player's and both teams' stat records through the same handlers,
// so a later load does not open on the previous capture's numbers.  Reload-phase adoption keeps
// entrants across loads, which is exactly what makes this necessary rather than tidy.
void ReleaseThrough(void* netGame, const MappedPlayer* mapped, size_t mappedCount);

// Unload runs on a thread with no netGame in its hand, so it records WHAT to release here and the
// next stage-3 tail -- which does have one -- performs it.  ServiceRelease is a no-op with nothing
// pending, and clears the request whether the release wrote or refused: a request that survived
// its own attempt would be re-run against the NEXT file's entrants.
void RequestRelease(const MappedPlayer* mapped, size_t mappedCount);
bool ReleasePending();
void ServiceRelease(void* netGame);

// Forgets what was applied without touching the engine.  Pairs with Arm(false) at unload.
void Forget();

const ApplyCensus& Census();
void ResetCensusForTest();

// The live SR15NetUserId for a user index: the slot whose slot-table element holds that index,
// with the generation from the element's high half.  False when the table holds no such index --
// which is a refusal, never a guessed slot, because a wrong slot writes another player's record.
bool LiveNetUserId(void* netGame, uint16_t liveIndex, uint32_t& id);

}  // namespace statsfeed
}  // namespace playback
}  // namespace replay
