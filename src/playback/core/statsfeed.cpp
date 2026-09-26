#include "playback/core/statsfeed.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "engine/pawn_provision.h"
#include "events/events_facts.h"
#include "events/lobbymsg.h"
#include "foundation/demofile/demotypes.h"
#include "playback/core/recorded_loadout.h"
#include "playback/core/statsfeed_facts.h"

namespace replay {
namespace playback {
namespace statsfeed {

namespace sf = replay::statsfeed_facts;

namespace {

const char kPlayerTapName[] = "SR15NetReliableStatUpdate";
const char kTeamTapName[] = "SR15NetReliableTeamStatUpdate";

// The empty object the player handler needs for a fresh entrant: CGameStats::From zeroes every
// record before it walks the members, so an object with no members IS the zeroing baseline.
const char kEmptyJson[] = "{}";

// The team applier takes RAW 20-byte rows, so it has no empty-object spelling.  What it does have
// is the same zeroing loop ahead of `a2 + 20 * (len / 20)`: any length below one whole row zeroes
// every record and applies nothing.  Four bytes of zero is that, and it is a real readable buffer
// rather than the null the handler's own `blobLen != 0` gate would refuse.
const uint8_t kTeamZeroBlob[4] = {0, 0, 0, 0};
inline constexpr uint32_t kTeamZeroBlobLen = uint32_t(sizeof kTeamZeroBlob);

// windowbuild.cpp's placement, restated here rather than shared: that one takes KeyedRecord and
// this one takes a row index, and a shared helper over a common shape would be a third type.
uint64_t SpreadAt(uint64_t begin, uint64_t span, uint32_t i, uint32_t n) {
    const uint64_t d = uint64_t(n) + 1;
    const uint64_t k = uint64_t(i) + 1;
    return begin + (span / d) * k + ((span % d) * k) / d;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------------------------

void RecordedStats::Reset() {
    row_count_ = 0;
    arena_used_ = 0;
    chapter_first_row_ = 0;
    for (uint16_t i = 0; i < kMaxSlots; ++i) players_[i] = Series{};
    for (uint16_t i = 0; i < kMaxTeams; ++i) teams_[i] = Series{};
}

bool RecordedStats::Store(uint64_t tUs, const void* bytes, uint32_t len, uint16_t key,
                          Series& into, ScanCensus& census) {
    if (row_count_ >= kMaxRows) {
        ++census.dropped_rows;
        if (!census.first_drop) census.first_drop = "the row table is full";
        return false;
    }
    if (into.count >= kRowsPerKey) {
        ++census.dropped_rows;
        if (!census.first_drop) census.first_drop = "one key carried more rows than the per-key cap";
        return false;
    }
    // The extra byte is a NUL the JSON parser may want and the length never counts: the applier is
    // handed `len`, so a payload stays byte-exact over exactly the bytes the file carried.
    if (size_t(arena_used_) + len + 1 > kArenaBytes) {
        ++census.dropped_rows;
        if (!census.first_drop) census.first_drop = "the payload arena is full";
        return false;
    }
    Row& r = rows_[row_count_];
    r.t_us = tUs;
    r.off = arena_used_;
    r.len = len;
    r.key = key;
    if (len != 0) memcpy(arena_ + arena_used_, bytes, len);
    arena_[arena_used_ + len] = 0;
    arena_used_ += len + 1;
    into.rows[into.count++] = uint16_t(row_count_);
    ++row_count_;
    return true;
}

bool RecordedStats::OfferPlayer(uint64_t tUs, uint32_t entrantIndex, const void* json, uint32_t len,
                                ScanCensus& census) {
    // The engine's own gate refuses a slot at or above the slot table's width, so a row keyed past
    // it could never be fed and is dropped here by name rather than stored and silently skipped.
    if (entrantIndex >= kMaxSlots) {
        ++census.dropped_rows;
        if (!census.first_drop) census.first_drop = "a stat row named a slot past the engine's bound";
        return false;
    }
    if (!Store(tUs, json, len, uint16_t(entrantIndex), players_[entrantIndex], census)) return false;
    ++census.player_rows;
    return true;
}

bool RecordedStats::OfferTeam(uint64_t tUs, uint16_t team, const void* rows, uint32_t len,
                              ScanCensus& census) {
    if (team >= kMaxTeams) {
        ++census.dropped_rows;
        if (!census.first_drop) census.first_drop = "a team stat row named a team past 1";
        return false;
    }
    if (!Store(tUs, rows, len, uint16_t(kMaxSlots + team), teams_[team], census)) return false;
    ++census.team_rows;
    return true;
}

void RecordedStats::BeginChapter() { chapter_first_row_ = row_count_; }

void RecordedStats::EndChapter(uint64_t tBeginUs, uint64_t tEndUs, ScanCensus& census) {
    if (tEndUs <= tBeginUs || chapter_first_row_ >= row_count_) return;
    const uint64_t span = tEndUs - tBeginUs;
    // Per key, exactly like the netvar spread: a key's rows arrived at that key's own rate and a
    // shared denominator would bunch a busy key's rows against a quiet one's.
    uint16_t seen[kMaxSlots + kMaxTeams] = {};
    uint16_t total[kMaxSlots + kMaxTeams] = {};
    for (uint32_t r = chapter_first_row_; r < row_count_; ++r) ++total[rows_[r].key];
    for (uint32_t r = chapter_first_row_; r < row_count_; ++r) {
        const uint16_t b = rows_[r].key;
        rows_[r].t_us = SpreadAt(tBeginUs, span, seen[b], total[b]);
        ++seen[b];
        ++census.spread_rows;
    }
    chapter_first_row_ = row_count_;
}

bool RecordedStats::Resolve(const Series& s, int64_t tUs, Payload& out, uint32_t& rowId) const {
    if (s.count == 0 || tUs < 0) return false;
    const uint64_t t = uint64_t(tUs);
    // Rows go in in file order and the spread is monotone inside a chapter, so the series is
    // sorted and the newest row at or before t is an upper bound.
    uint16_t lo = 0, hi = s.count;
    while (lo < hi) {
        const uint16_t mid = uint16_t(lo + (hi - lo) / 2);
        if (rows_[s.rows[mid]].t_us <= t) {
            lo = uint16_t(mid + 1);
        } else {
            hi = mid;
        }
    }
    if (lo == 0) return false;
    const uint32_t row = s.rows[lo - 1];
    out.bytes = arena_ + rows_[row].off;
    out.len = rows_[row].len;
    rowId = row;
    return true;
}

bool RecordedStats::PlayerAt(uint16_t slot, int64_t tUs, Payload& out, uint32_t& rowId) const {
    return slot < kMaxSlots && Resolve(players_[slot], tUs, out, rowId);
}

bool RecordedStats::TeamAt(uint16_t team, int64_t tUs, Payload& out, uint32_t& rowId) const {
    return team < kMaxTeams && Resolve(teams_[team], tUs, out, rowId);
}

uint16_t RecordedStats::PlayerSlotCount() const {
    uint16_t n = 0;
    for (uint16_t i = 0; i < kMaxSlots; ++i) {
        if (players_[i].count != 0) ++n;
    }
    return n;
}

uint16_t RecordedStats::PlayerSlotAt(uint16_t i) const {
    uint16_t seen = 0;
    for (uint16_t s = 0; s < kMaxSlots; ++s) {
        if (players_[s].count == 0) continue;
        if (seen == i) return s;
        ++seen;
    }
    return kMaxSlots;
}

bool RecordedStats::RowPayload(uint32_t rowId, Payload& out) const {
    if (rowId >= row_count_) return false;
    out.bytes = arena_ + rows_[rowId].off;
    out.len = rows_[rowId].len;
    return true;
}

uint64_t RecordedStats::RowTimeUs(uint32_t rowId) const {
    return rowId < row_count_ ? rows_[rowId].t_us : 0;
}

// ---------------------------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------------------------

uint32_t ScanRecordedStats(IDemoReader& reader, RecordedStats& into, ScanCensus& census) {
    into.Reset();
    census = ScanCensus{};
    static uint8_t body[capture::kMaxEventBodyBytes];
    static uint8_t keyblob[identity::kTapTableScratchBytes];
    uint32_t stored = 0;
    uint16_t playerTap = capture::kTapTypeIndexNone;
    uint16_t teamTap = capture::kTapTypeIndexNone;

    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) continue;
        const uint16_t herePlayer = identity::TapIndexForName(
            dc, kPlayerTapName, sizeof kPlayerTapName - 1, keyblob, sizeof keyblob);
        const uint16_t hereTeam = identity::TapIndexForName(
            dc, kTeamTapName, sizeof kTeamTapName - 1, keyblob, sizeof keyblob);
        if (herePlayer != capture::kTapTypeIndexNone || hereTeam != capture::kTapTypeIndexNone) {
            ++census.tap_chapters;
        }
        if (herePlayer != capture::kTapTypeIndexNone) playerTap = herePlayer;
        if (hereTeam != capture::kTapTypeIndexNone) teamTap = hereTeam;
        if (playerTap == capture::kTapTypeIndexNone && teamTap == capture::kTapTypeIndexNone) {
            continue;
        }

        const StreamView* sv = dc.Find(kStreamEvent);
        if (!sv || !sv->records || sv->bytes <= capture::kBlockPrefixBytes) continue;
        const ChapterKey key = reader.ChapterAt(i);
        into.BeginChapter();
        bitio::BitReader r(sv->records + capture::kBlockPrefixBytes,
                           sv->bytes - capture::kBlockPrefixBytes);
        capture::EventDecoder dec;
        for (uint32_t k = 0; k < sv->recordCount; ++k) {
            capture::EventMessage m{};
            // A refused row leaves the reader off a record boundary, so the rest of this chapter's
            // events are unreadable: stop rather than decode garbage, exactly as the identity scan
            // does.  The count is what says something was missed.
            if (!dec.Decode(r, m, body, sizeof body)) break;
            ++census.event_rows;
            if (m.typeIndex == playerTap && playerTap != capture::kTapTypeIndexNone) {
                events::StatUpdate su{};
                if (!events::DecodeStatUpdate(m.body, m.bodyBytes, &su)) {
                    ++census.refused_rows;
                    continue;
                }
                if (into.OfferPlayer(key.tBeginUs, su.entrantIndex, su.json.p, su.json.n, census)) {
                    ++stored;
                }
            } else if (m.typeIndex == teamTap && teamTap != capture::kTapTypeIndexNone) {
                events::TeamStatUpdate tu{};
                if (!events::DecodeTeamStatUpdate(m.body, m.bodyBytes, &tu)) {
                    ++census.refused_rows;
                    continue;
                }
                // The handler is handed the ROWS, not the body: the team byte is the routing key
                // and lives at body+0, and the payload starts after the 2-byte header.
                if (into.OfferTeam(key.tBeginUs, tu.team, tu.rows,
                                   uint32_t(tu.rowCount) * events_facts::kTeamStatRowLen, census)) {
                    ++stored;
                }
            }
        }
        into.EndChapter(key.tBeginUs, key.tEndUs, census);
    }
    census.player_tap = playerTap;
    census.team_tap = teamTap;
    return stored;
}

RecordedStats& PlaybackRecordedStats() {
    static RecordedStats s;
    return s;
}

ScanCensus& PlaybackStatsCensus() {
    static ScanCensus c;
    return c;
}

// ---------------------------------------------------------------------------------------------
// The feed
// ---------------------------------------------------------------------------------------------

namespace {

struct AppliedKey {
    bool valid = false;
    bool zeroed = false;
    uint32_t row_id = 0;
    // WHICH ENTRANT the row was written into, slot | gen<<16.  A re-admission hands the recorded
    // slot a new live entrant with a bumped generation and a zeroed record; keyed on the row alone
    // the feed would see "same row, already applied" and leave the new entrant's stats blank until
    // the next re-apply tick.  This is the field that makes a re-admission a change.
    uint32_t net_user_id = 0;
};

struct Feed {
    bool armed = false;
    bool dirty = true;
    uint64_t last_sweep_ms = 0;
    // The playhead axis is the file's, minus its origin; tUs arrives absolute. Logging the absolute
    // number next to a `seek @N` that speaks the playhead's axis reads as a 21 s error that is not
    // there, which is what LAUNCH-STATS2 measured.
    int64_t origin_us = 0;
    // What the last sweep could actually reach, so the count in the log is one the feed observed
    // rather than one taken before the roster was dressed.
    size_t mapped_logged = size_t(-1);
    AppliedKey players[kMaxSlots];
    AppliedKey teams[kMaxTeams];
    uint32_t logged = 0;
    MappedPlayer release[kMaxSlots];
    size_t release_count = 0;
    bool release_pending = false;
    ApplyCensus census;
    const Handlers* injected = nullptr;
    Handlers bound;
    bool bind_tried = false;
};

Feed g;

// The cadence.  UpdateTrackingStatsArena (sub_1401C5130) bumps Steals, ShotsOnGoal, Catches,
// Passes and PossessionTime into the SAME per-player record this feed writes, from live
// possession transitions -- and replay drives possession for real, so the host keeps accruing on
// top of what was applied.  A re-apply is a full REPLACE, so putting one on a slow cadence bounds
// that drift to a second without a detour and without a second writer.
inline constexpr uint64_t kReapplyMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot

// At most this many change lines per load, so a file that rewrites a stat every tick cannot turn
// the log into the payload.  The census counts every write whether or not it was printed.
inline constexpr uint32_t kLogCap = 64;

StatApplyFn Bind(const char* factId, uint64_t va, const uint8_t* pro, size_t n) {
    if (!buildguard::VerifySite(factId, va, pro, n)) return nullptr;
    return reinterpret_cast<StatApplyFn>(const_cast<void*>(buildguard::MappedAddress(va)));
}

const Handlers* Resolve() {
    if (g.injected != nullptr) return g.injected;
    if (!g.bind_tried) {
        g.bind_tried = true;
        g.bound.player = Bind("STATS.PLAYER_APPLY_VA", sf::kPlayerApplyVa, sf::kPlayerApplyPrologue,
                              sizeof sf::kPlayerApplyPrologue);
        g.bound.team = Bind("STATS.TEAM_APPLY_VA", sf::kTeamApplyVa, sf::kTeamApplyPrologue,
                            sizeof sf::kTeamApplyPrologue);
    }
    return &g.bound;
}

// SEH lives in a POD-only function: MSVC C2712 forbids __try beside the EngineCallLatch.
bool CallGuarded(StatApplyFn fn, void* netGame, const EngineStatMsg* msg) {
    __try {
        fn(netGame, msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadSlotElementGuarded(const void* at, uint16_t& index, uint16_t& gen) {
    __try {
        index = *static_cast<const uint16_t*>(at);
        gen = *(static_cast<const uint16_t*>(at) + 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TurnOffAfterFault(const char* what) {
    ++g.census.faulted;
    g.armed = false;
    g.census.refusal = "the engine's stat applier raised";
    log::Direct(LogLevel::Error, "playback",
                "stats: the engine %s applier raised; the stat feed is off for the rest of this "
                "session", what);
}

void Summarise(const Payload& p, char* out, size_t cap) {
    const size_t n = p.len < cap - 4 ? size_t(p.len) : cap - 5;
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p.bytes[i];
        out[w++] = (c >= 0x20 && c < 0x7F) ? char(c) : '.';
    }
    if (n < p.len) {
        out[w++] = '.';
        out[w++] = '.';
        out[w++] = '.';
    }
    out[w] = 0;
}

bool ApplyPlayer(const Handlers& h, void* netGame, uint32_t netUserId, const uint8_t* blob,
                 uint32_t len) {
    EngineStatMsg msg;
    msg.body = uint64_t(uintptr_t(&netUserId));
    msg.blob = uint64_t(uintptr_t(blob));
    msg.blobLen = len;
    PawnProvision::EngineCallLatch latch;
    return CallGuarded(h.player, netGame, &msg);
}

bool ApplyTeam(const Handlers& h, void* netGame, uint16_t team, const uint8_t* blob, uint32_t len) {
    EngineStatMsg msg;
    msg.body = uint64_t(uintptr_t(&team));
    msg.blob = uint64_t(uintptr_t(blob));
    msg.blobLen = len;
    PawnProvision::EngineCallLatch latch;
    return CallGuarded(h.team, netGame, &msg);
}

struct SlotTableSnapshot {
    uint16_t index[sf::kSlotTableMax];
    uint16_t gen[sf::kSlotTableMax];
    uint16_t read;
};

}  // namespace

// ★ PERF-1. One slot table, one read of it. It used to be walked from scratch for every mapped player
// -- 8 players x 16 SEH-guarded element reads = 128 a frame -- for an answer that is the same table
// every time. `read` is how far the walk got before an element faulted, so a lookup that stops there
// answers exactly what the per-player walk answered, refusals included.
void SnapshotSlotTable(void* netGame, SlotTableSnapshot& out) {
    out.read = 0;
    if (netGame == nullptr) return;
    const uint8_t* base = static_cast<const uint8_t*>(netGame) + sf::kSlotTableOff;
    for (uint16_t slot = 0; slot < sf::kSlotTableMax; ++slot) {
        if (!ReadSlotElementGuarded(base + 4u * slot, out.index[slot], out.gen[slot])) return;
        ++out.read;
    }
}

bool LiveNetUserIdIn(const SlotTableSnapshot& t, uint16_t liveIndex, uint32_t& id) {
    for (uint16_t slot = 0; slot < t.read; ++slot) {
        if (t.index[slot] != liveIndex || t.index[slot] == 0xFFFF || t.gen[slot] == 0xFFFF) continue;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
        id = uint32_t(slot) | (uint32_t(t.gen[slot]) << 16);
        return true;
    }
    return false;
}

bool LiveNetUserId(void* netGame, uint16_t liveIndex, uint32_t& id) {
    SlotTableSnapshot t;
    SnapshotSlotTable(netGame, t);
    return LiveNetUserIdIn(t, liveIndex, id);
}

void InjectHandlersForTest(const Handlers* h) {
    g.injected = h;
    g.bind_tried = false;
    g.bound = Handlers{};
}

void Arm(bool on) {
    g.armed = on;
    g.dirty = true;
    g.last_sweep_ms = 0;
    g.logged = 0;
    g.mapped_logged = size_t(-1);
    g.census.refusal = nullptr;
    if (!on) Forget();
}

void SetPlayheadOriginUs(int64_t originUs) { g.origin_us = originUs; }

bool Armed() { return g.armed; }

void Invalidate() { g.dirty = true; }

void Forget() {
    for (uint16_t i = 0; i < kMaxSlots; ++i) g.players[i] = AppliedKey{};
    for (uint16_t i = 0; i < kMaxTeams; ++i) g.teams[i] = AppliedKey{};
    g.dirty = true;
}

const ApplyCensus& Census() { return g.census; }

void ResetCensusForTest() {
    g.census = ApplyCensus{};
    g.logged = 0;
}

void Sweep(void* netGame, int64_t tUs, const MappedPlayer* mapped, size_t mappedCount,
           uint64_t nowMs) {
    if (!g.armed) {
        g.census.refusal = "not armed";
        return;
    }
    if (!livesafety::Allowed(livesafety::Feature::Stats)) {
        g.census.refusal = "containment";
        return;
    }
    if (netGame == nullptr) {
        g.census.refusal = "no netGame";
        return;
    }
    const Handlers* h = Resolve();
    if (h->player == nullptr || h->team == nullptr) {
        g.census.refusal = "the stat appliers did not bind";
        return;
    }
    g.census.refusal = nullptr;

    const bool cadence = g.dirty || nowMs == 0 || nowMs - g.last_sweep_ms >= kReapplyMs;
    if (cadence) {
        g.last_sweep_ms = nowMs;
        if (!g.dirty) ++g.census.reapplies;
    }
    g.dirty = false;

    const RecordedStats& src = PlaybackRecordedStats();
    if (mappedCount != g.mapped_logged) {
        g.mapped_logged = mappedCount;
        log::Direct(LogLevel::Info, "playback",
                    "stats: %zu of %u recorded player(s) now map to a live entrant",
                    mappedCount, unsigned(src.PlayerSlotCount()));
    }

    SlotTableSnapshot slots;
    SnapshotSlotTable(netGame, slots);

    for (size_t i = 0; i < mappedCount; ++i) {
        const MappedPlayer& m = mapped[i];
        if (m.rec_slot >= kMaxSlots) {
            ++g.census.unmapped;
            continue;
        }
        uint32_t netUserId = 0;
        if (!LiveNetUserIdIn(slots, m.live_index, netUserId)) {
            ++g.census.no_live_slot;
            continue;
        }
        AppliedKey& was = g.players[m.rec_slot];
        Payload p;
        uint32_t rowId = 0;
        const bool have = src.PlayerAt(m.rec_slot, tUs, p, rowId);
        const bool sameEntrant = was.net_user_id == netUserId;
        if (have) {
            if (!cadence && sameEntrant && was.valid && !was.zeroed && was.row_id == rowId) continue;
            if (!ApplyPlayer(*h, netGame, netUserId, p.bytes, p.len)) {
                TurnOffAfterFault("player");
                return;
            }
            ++g.census.player_writes;
            const bool changed = !was.valid || was.zeroed || was.row_id != rowId || !sameEntrant;
            was.valid = true;
            was.zeroed = false;
            was.row_id = rowId;
            was.net_user_id = netUserId;
            if (changed && g.logged < kLogCap) {
                ++g.logged;
                char blob[88];
                Summarise(p, blob, sizeof blob);
                log::Direct(LogLevel::Info, "playback",
                            "stats: playhead %lld us rec slot %u -> live slot %u gen %u: %s",
                            (long long)(tUs - g.origin_us), unsigned(m.rec_slot),
                            unsigned(netUserId & 0xFFFF), unsigned(netUserId >> 16), blob);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
            }
        } else {
            // BEFORE A PLAYER'S FIRST ROW the recording says nothing, and what the engine shows for
            // a fresh entrant is a zeroed record -- which is exactly what the applier leaves behind
            // for an object with no members.  Reproduced through the handler rather than by writing
            // the record, so the one path that touches a stat record stays the engine's own.
            if (!cadence && sameEntrant && was.valid && was.zeroed) continue;
            if (!ApplyPlayer(*h, netGame, netUserId,
                             reinterpret_cast<const uint8_t*>(kEmptyJson),
                             uint32_t(sizeof kEmptyJson - 1))) {
                TurnOffAfterFault("player");
                return;
            }
            ++g.census.zero_writes;
            was.valid = true;
            was.zeroed = true;
            was.net_user_id = netUserId;
        }
    }

    for (uint16_t t = 0; t < kMaxTeams; ++t) {
        AppliedKey& was = g.teams[t];
        Payload p;
        uint32_t rowId = 0;
        const bool have = src.TeamAt(t, tUs, p, rowId);
        if (have) {
            if (!cadence && was.valid && !was.zeroed && was.row_id == rowId) continue;
            if (!ApplyTeam(*h, netGame, t, p.bytes, p.len)) {
                TurnOffAfterFault("team");
                return;
            }
            ++g.census.team_writes;
            was.valid = true;
            was.zeroed = false;
            was.row_id = rowId;
        } else {
            if (!cadence && was.valid && was.zeroed) continue;
            if (!ApplyTeam(*h, netGame, t, kTeamZeroBlob, kTeamZeroBlobLen)) {
                TurnOffAfterFault("team");
                return;
            }
            ++g.census.zero_writes;
            was.valid = true;
            was.zeroed = true;
        }
    }
}

void ReleaseThrough(void* netGame, const MappedPlayer* mapped, size_t mappedCount) {
    if (netGame == nullptr) return;
    if (!livesafety::Allowed(livesafety::Feature::Stats)) return;
    const Handlers* h = Resolve();
    if (h->player == nullptr || h->team == nullptr) return;
    for (size_t i = 0; i < mappedCount; ++i) {
        uint32_t netUserId = 0;
        if (!LiveNetUserId(netGame, mapped[i].live_index, netUserId)) continue;
        if (!ApplyPlayer(*h, netGame, netUserId, reinterpret_cast<const uint8_t*>(kEmptyJson),
                         uint32_t(sizeof kEmptyJson - 1))) {
            TurnOffAfterFault("player");
            return;
        }
        ++g.census.zero_writes;
    }
    for (uint16_t t = 0; t < kMaxTeams; ++t) {
        if (!ApplyTeam(*h, netGame, t, kTeamZeroBlob, kTeamZeroBlobLen)) {
            TurnOffAfterFault("team");
            return;
        }
        ++g.census.zero_writes;
    }
    Forget();
}

void RequestRelease(const MappedPlayer* mapped, size_t mappedCount) {
    g.release_count = mappedCount < kMaxSlots ? mappedCount : kMaxSlots;
    for (size_t i = 0; i < g.release_count; ++i) g.release[i] = mapped[i];
    g.release_pending = true;
}

bool ReleasePending() { return g.release_pending; }

void ServiceRelease(void* netGame) {
    if (!g.release_pending) return;
    g.release_pending = false;
    ReleaseThrough(netGame, g.release, g.release_count);
    g.release_count = 0;
}

}  // namespace statsfeed
}  // namespace playback
}  // namespace replay
