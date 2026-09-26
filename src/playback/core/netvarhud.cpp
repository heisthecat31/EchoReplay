#include "playback/core/netvarhud.h"

#include <windows.h>

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/log.h"
#include "bootstrap/tickcost.h"
#include "engine/gamestate_facts.h"
#include "foundation/gametype.h"
#include "engine/pawn_provision.h"
#include "playback/core/netvarguard.h"
#include "playback/core/window.h"

namespace replay {
namespace playback {

// Symbols: RE-CLOCK-SCOREBOARD-PHASE S4.4 / S5.2 / S5.3 / S8.1, each hash-confirmed there.
const HudVarSpec kHudVars[kHudVarCount] = {
    {0xB2996AD484F1D1E8ull, HudVarKind::Real,   HudVarGroup::Clock, "game_timer"},        // facts-ok: capture_facts kNvGameTimer
    {0x3E766A9AFE6C97B0ull, HudVarKind::Real,   HudVarGroup::Clock, "countdown_timer"},   // facts-ok: RE-4 S4.4 countdown_timer
    {0xB2996AD399FDC0FFull, HudVarKind::Symbol, HudVarGroup::Clock, "game_state"},        // facts-ok: RE-8 S8.1 game_state
    {0x1C97052A21E09F11ull, HudVarKind::Int,    HudVarGroup::Score, "blue_score"},        // facts-ok: RE-5 S5.2 blue_score
    {0x2ED07B1C8C27548Dull, HudVarKind::Int,    HudVarGroup::Score, "red_score"},         // facts-ok: RE-5 S5.2 red_score
    {0x19079A534E208DC1ull, HudVarKind::Int,    HudVarGroup::Score, "blue_round_score"},  // facts-ok: RE-5 S5.3 blue_round_score
    {0xF7DFD2D55B871001ull, HudVarKind::Int,    HudVarGroup::Score, "red_round_score"},   // facts-ok: RE-5 S5.3 red_round_score
    // ★ APPENDED at indexes 7 and 8, never inserted: the recorder's keyframe mask is a bit per
    // position and files already written number the seven above.  Phase is LEVEL, not an edge --
    // the value at or before T, applied on every seek in both directions.  No round event is ever
    // raised from here: sub_1401A6230 re-arms host scoring, which would rewrite the match.
    {0xEE2E44F738A30C35ull, HudVarKind::Flag,   HudVarGroup::Score, "sudden_death_active"},
    {0xB42C6FF86B5CFB10ull, HudVarKind::Flag,   HudVarGroup::Score, "match_is_over"},
};

// The two symbols above, checked against the routine that produces them rather than transcribed.
static_assert(gametype::SymbolHash("sudden_death_active") == 0xEE2E44F738A30C35ull,
              "sudden_death_active");
static_assert(gametype::SymbolHash("match_is_over") == 0xB42C6FF86B5CFB10ull, "match_is_over");

namespace {

// Owned positions in the host-write guard: the HUD keys in kHudVars order, then the frisbee owner,
// then one per host-hold key.
// The four keys the goal-FX scripts poll-and-diff: 253eb047/b7d10bcf for the panel flash and the
// accrual SFX, 4d027c6e for the goal_fx explosion. A change here IS the celebration's trigger, so
// these are the keys the probe reports the notifier verdict for.
constexpr size_t kScoreProbeFirst = 3;
constexpr size_t kScoreProbeCount = 4;
bool IsScoreProbeKey(size_t i) {
    return i >= kScoreProbeFirst && i < kScoreProbeFirst + kScoreProbeCount;
}

constexpr size_t kFrisbeeOwnedIndex = kHudVarCount;
constexpr size_t kHostHoldOwnedBase = kFrisbeeOwnedIndex + 1;
constexpr uint64_t kFrisbeeOwnerSymbol = gamestate::facts::kVarFrisbeeOwnerId.symbol;

// ---- HOST-HOLD -------------------------------------------------------------------------------
//
// These are the HOST's keys, not the recording's: nothing here is resolved from the window. Each is
// held at one constant for as long as the host is playing under a loaded replay, and given back --
// at the value the engine held before -- when the replay unloads.
//
// `round_time` is the netint the arena script SEEDS its script-local round clock from, verbatim:
// execute_node_2e769936 @ 0x18001B6A0 is `local_game_timer = (float)R15NETINT::Get(this key)`. The
// clock itself is a graph->deltatime countdown nothing reads back from a netvar, so this seed is
// the only reachable handle on it -- hold it at an hour and the round the host is about to start
// outlasts any viewing. Read by nobody in the arena: the other two consumers in the corpus are a
// second gametype's script and the private-match settings MenuPage.
//
// `paused` is the stronger lever and the risky one -- it gates EVERY countdown in the script
// (expression_3c9b90da_condition @ 0x180031030 and the three UpdateCountdownTimer gates) -- but the
// UI script that owns "PAUSED" / "PAUSE REQUESTED" / "UNPAUSING IN " listens for a change of
// exactly this key, so it is off until the owner has looked at the screen once. See O-G.
struct HoldKey {
    uint64_t    symbol;
    HudVarKind  kind;
    uint64_t    value;
    const char* name;
};

constexpr size_t kHoldKeyCount = 2;
constexpr uint32_t kHoldRoundSeconds = 3600;

constexpr HoldKey kHoldKeys[kHoldKeyCount] = {
    {0x5B748A2B4792DD2Dull, HudVarKind::Int,    kHoldRoundSeconds,
     "net_match_settings_round_time_in_seconds"},
    {0x02FE0AA4F2637309ull, HudVarKind::Symbol, 0x41D2D73C0F30021Aull, "net_game_paused_state"},
};

static_assert(gametype::SymbolHash("net_match_settings_round_time_in_seconds") ==
                  kHoldKeys[0].symbol, "net_match_settings_round_time_in_seconds");
static_assert(gametype::SymbolHash("net_game_paused_state") == kHoldKeys[1].symbol,
              "net_game_paused_state");
static_assert(gametype::SymbolHash("paused") == kHoldKeys[1].value, "the `paused` symbol");
static_assert(kHostHoldOwnedBase + kHoldKeyCount <= netvarguard::kMaxOwned,
              "the guard's owned table is too small");

constexpr uint32_t kCellDue = 0x10000u;
// How long a memo of what the record holds is trusted before the getter is paid for again.
constexpr uint64_t kMemoVerifyMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF a millisecond cadence of this code's own, not the CR15Game vtable slot
constexpr uint64_t kMeasureWindowMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
constexpr uint16_t kNoRefusal = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

struct Cell {
    std::atomic<uint64_t> raw{0};
    std::atomic<uint32_t> meta{0};   // kCellDue | recorded slot
};

struct State {
    // Written by Arm/Disarm before `armed` flips, read by both threads after.
    std::atomic<uint8_t>  armed{0};
    std::atomic<uint8_t>  faulted{0};
    std::atomic<uint32_t> live_slot{kNetVarSlotNone};   // the global handle's slot, once the route held
    std::atomic<uint32_t> load_epoch{0};                // bumped per Arm: the sweep resets its per-load state
    std::atomic<uint8_t>  applying{0};
    // ★★★ ONE DELEGATE-FIRING WRITE AFTER A LOAD OR A SEEK, EVEN WHEN THE VALUE IS ALREADY THERE.
    // The widgets redraw on the netvar-CHANGE event (netvarhud.h), and the sweep below writes only
    // when the getter reads back something else -- so a playhead that is not moving publishes the
    // right value and emits nothing, and a widget that has not drawn a value yet has no reason to.
    // This is the one state the launch found dashes in: paused at T0 before the first play. Set on
    // arm and on every invalidate, consumed by the first sweep that has a route, so the cost is one
    // extra setter call per key per jump and nothing at all per frame.
    std::atomic<uint8_t>  force_write{0};
    std::atomic<uint8_t>  key_refused[kHudVarCount] = {};  // the setter's own gate refused this key
    bool clock_on = false;
    bool score_on = false;
    bool host_filter_on = false;
    // playback.goal_score_probe. The score keys are the engine's own trigger for the goal flash,
    // the accrual SFX and the goal_fx explosion (DESIGN-GOAL-SEQUENCE G1/G2), and nothing has ever
    // measured whether our write reaches the change notifier. `playhead_us` is the tick's, for the
    // line's axis only.
    std::atomic<uint8_t> score_probe{1};
    std::atomic<int64_t> playhead_us{0};
    HudNetVarSetters setters;
    Cell cells[kHudVarCount];
    Cell frisbee_owner;

    // ★ WHAT WE LAST LEFT IN THE RECORD, per key, so a sweep whose recorded value has not moved
    // does not pay an engine getter to be told so. The engine's getter spins on the record's lock,
    // costs ~200 us and the sweep runs about four times a rendered frame; at nine keys that was
    // 2.3 ms of every frame for `writes=0`. The memo is trusted only until `kVerifyMs`, so a host
    // write is still caught -- one verification pass a second per key instead of ~240.
    // ★ AND THE ROUTE ITSELF. PERF-3 measured `LocateNetVarSite` at 2.6 ms of every rendered
    // frame: it is twelve guarded reads, every guarded read is a VirtualQuery, and the engine
    // drives this sweep about four times a frame. `now_ms` is GetTickCount64, so one value of it
    // covers one frame's worth of sweeps and no more -- the route is still re-validated every
    // frame, just not four times within one.
    NetVarSite cached_site;
    void*    cached_site_netgame = nullptr;
    uint64_t cached_site_ms = 0;
    bool     have_cached_site = false;

    uint64_t memo[kHudVarCount] = {};
    uint64_t memo_ms[kHudVarCount] = {};
    bool     memo_valid[kHudVarCount] = {};

    // The sweep's own.
    uint32_t seen_epoch = 0;
    HudNetVarStats  stats;
    HudClockMeasure measure;
    uint64_t measure_start_ms = 0;
    uint64_t timer_expected = 0;
    bool     have_timer_expected = false;
    bool     route_ok = false;
    // Forced sweeps that found no cell due, since the force was armed.  Sweep-thread only.
    uint32_t force_empty = 0;
    uint16_t route_refusal = kNoRefusal;   // NetVarSite::RefusalKey of the last refusal logged
    bool     slot_refusal_logged[kHudVarCount] = {};
    char     last_line[192] = {};
    bool     have_line = false;
    netvarguard::OwnedKey host_owned[netvarguard::kMaxOwned];   // what the guard was last handed
    char     last_host_line[512] = {};
    bool     have_host_line = false;

    // HOST-HOLD. `want` is the tick thread's; the rest is the sweep's alone, and `active` outlives
    // `armed` on purpose -- the key has to be given back on the sweep AFTER the unload.
    std::atomic<uint8_t> hold_want[kHoldKeyCount] = {};
    std::atomic<uint8_t> hold_active[kHoldKeyCount] = {};   // read by the tick for its log line
    bool     hold_refused[kHoldKeyCount] = {};
    uint64_t hold_saved[kHoldKeyCount] = {};
};
State g;

bool GroupOn(HudVarGroup grp) { return grp == HudVarGroup::Clock ? g.clock_on : g.score_on; }

// Anything that can have changed the record under us -- a new load, a lost route, a seek -- costs
// the memos, so the next sweep reads the engine rather than trusting a stale answer.
void ForgetMemos() {
    for (bool& v : g.memo_valid) v = false;
    g.have_cached_site = false;
}

// The getter too: write-when-different and the refused-setter check both read back.
bool SetterFor(const HudNetVarSetters& s, HudVarKind k) {
    switch (k) {
        case HudVarKind::Real:   return s.setReal != nullptr && s.getReal != nullptr;
        case HudVarKind::Int:    return s.setInt != nullptr && s.getInt != nullptr;
        case HudVarKind::Symbol: return s.setSymbol != nullptr && s.getSymbol != nullptr;
        case HudVarKind::Flag:   return s.setFlag != nullptr && s.getBool != nullptr;
    }
    return false;
}

// What the typed SetValue<T> compares and writes: the low dword for a float or an int, the whole
// qword for a symbol (sub_140C87CA0 / sub_140C87A50 / sub_140C87F00).
bool SameStored(HudVarKind k, uint64_t a, uint64_t b) {
    // A flag reads back as the getter's bool, so a recorded 2 and a held 1 are the same state:
    // comparing the dword would make every sweep rewrite a flag that is already set.
    if (k == HudVarKind::Flag) return (a != 0) == (b != 0);
    return k == HudVarKind::Symbol ? a == b : uint32_t(a) == uint32_t(b);
}

float AsFloat(uint64_t raw) {
    const uint32_t bits = uint32_t(raw);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

// SEH lives in a POD-only function (MSVC C2712 forbids __try beside the EngineCallLatch).
bool CallSetterGuarded(const HudNetVarSetters& s, HudVarKind kind, void* netGame,
                       const NetVarSite& site, uint64_t symbol, uint64_t raw) {
    __try {
        switch (kind) {
            case HudVarKind::Real:
                s.setReal(netGame, symbol, AsFloat(raw));
                break;
            case HudVarKind::Int:
                s.setInt(site.netVarCs, site.handle, symbol, uint32_t(raw));
                break;
            case HudVarKind::Symbol:
                // +INF is what the engine's own callers pass (sub_1401C5130 @0x1401C5296 loads
                // dword_1416D5A64 = 0x7F800000), the same lifetime SetReal/SetInt hard-code.
                s.setSymbol(netGame, symbol, raw, INFINITY);
                break;
            case HudVarKind::Flag:
                s.setFlag(site.netVarCs, site.handle, symbol, uint8_t(raw != 0));
                break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What the engine holds for `symbol`, in SameStored's terms. A missing row reads as the getter's
// own default (0, 0.0f, ~0); the setter inserts on the first write that differs from it.
bool ReadBackGuarded(const HudNetVarSetters& s, HudVarKind kind, void* netGame, uint64_t symbol,
                     uint64_t& held) {
    __try {
        switch (kind) {
            case HudVarKind::Real: {
                const float f = s.getReal(netGame, symbol);
                uint32_t bits;
                memcpy(&bits, &f, sizeof bits);
                held = bits;
                break;
            }
            case HudVarKind::Int:
                held = uint32_t(s.getInt(netGame, symbol));
                break;
            case HudVarKind::Symbol:
                // Returned in memory: the engine writes the CSymbol64 through the buffer we pass.
                s.getSymbol(&held, netGame, symbol);
                break;
            case HudVarKind::Flag:
                held = s.getBool(netGame, symbol) ? 1u : 0u;
                break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FormatValue(HudVarKind kind, uint64_t raw, char* out, size_t cap) {
    switch (kind) {
        case HudVarKind::Real:   snprintf(out, cap, "%.3f", AsFloat(raw)); break;
        case HudVarKind::Int:    snprintf(out, cap, "%d", int32_t(uint32_t(raw))); break;
        case HudVarKind::Symbol: snprintf(out, cap, "0x%016llX", (unsigned long long)raw); break;
        case HudVarKind::Flag:   snprintf(out, cap, "%s", raw != 0 ? "true" : "false"); break;
    }
}

void FormatRoleWord(void* netGame, char* out, size_t cap) {
    uint64_t word = 0;
    if (!ReadNetGameRoleWord(netGame, word)) {
        snprintf(out, cap, "unread");
        return;
    }
    snprintf(out, cap, "0x%llX = %s", (unsigned long long)word,
             RoleWordIsAuthority(word) ? "authority" : "NOT authority, the setters will refuse");
}

void TurnOffAfterFault(const char* key, const char* what) {
    ++g.stats.faulted;
    g.faulted.store(1, std::memory_order_relaxed);
    g.applying.store(0, std::memory_order_relaxed);
    netvarguard::SetMode(netvarguard::Mode::PassThrough);
    log::Direct(LogLevel::Error, "playback",
                "hud netvar %s: the engine %s raised; HUD netvar writes are off for the rest of "
                "this session",
                key, what);
}

template <class Fn>
Fn GuardedSite(netvarguard::Site which) {
    const netvarguard::SiteFact& site = netvarguard::SiteAt(which);
    if (!buildguard::VerifySite(site.factId, site.va, site.prologue, site.prologueLen)) return nullptr;
    return reinterpret_cast<Fn>(const_cast<void*>(buildguard::MappedAddress(site.va)));
}

// The getters share one stack frame, so their prologues cannot tell the (store, symbol) family from
// GET_SYMBOL's (result, store, symbol); the argument moves at GS.GETTER_ARGMOVE_OFF do (gamestate.h).
template <class Fn, size_t N>
Fn GuardedGetter(const char* id, uint64_t va, const uint8_t (&argMove)[N]) {
    if (!buildguard::VerifySite(id, va + facts::GS_GETTER_ARGMOVE_OFF, argMove, N)) return nullptr;
    return reinterpret_cast<Fn>(const_cast<void*>(buildguard::MappedAddress(va)));
}

const char* OwnedName(size_t i) {
    if (i < kHudVarCount) return kHudVars[i].name;
    if (i == kFrisbeeOwnedIndex) return gamestate::facts::kVarFrisbeeOwnerId.name;
    return kHoldKeys[i - kHostHoldOwnedBase].name;
}

// HOST-HOLD, on the sweep thread. False means the engine raised and the writer is off for good.
bool DriveHostHolds(void* netGame, const NetVarSite& site, netvarguard::OwnedKey* owned) {
    for (size_t i = 0; i < kHoldKeyCount; ++i) {
        const HoldKey& hk = kHoldKeys[i];
        const size_t pos = kHostHoldOwnedBase + i;
        const bool want = g.hold_want[i].load(std::memory_order_acquire) != 0 && !g.hold_refused[i];
        const bool active = g.hold_active[i].load(std::memory_order_relaxed) != 0;
        if (!want && !active) continue;
        if (!SetterFor(g.setters, hk.kind)) continue;

        PawnProvision::EngineCallLatch latch;
        if (!latch.taken) {
            ++g.stats.reentrant;
            if (active) owned[pos] = g.host_owned[pos];   // a skipped frame is not a release
            continue;
        }
        uint64_t held = 0;
        if (!ReadBackGuarded(g.setters, hk.kind, netGame, hk.symbol, held)) {
            TurnOffAfterFault(hk.name, "getter");
            return false;
        }

        if (want) {
            if (!active) {
                g.hold_active[i].store(1, std::memory_order_relaxed);
                g.hold_saved[i] = held;
                char was[32], now[32];
                FormatValue(hk.kind, held, was, sizeof was);
                FormatValue(hk.kind, hk.value, now, sizeof now);
                log::Direct(LogLevel::Info, "playback",
                            "host hold ARMED: %s %s -> %s, owned in slot %u so the host cannot put "
                            "it back (playback.host_hold)",
                            hk.name, was, now, unsigned(site.Slot()));
            }
            if (!SameStored(hk.kind, held, hk.value)) {
                if (!CallSetterGuarded(g.setters, hk.kind, netGame, site, hk.symbol, hk.value)) {
                    TurnOffAfterFault(hk.name, "setter");
                    return false;
                }
                ++g.stats.writes;
                if (!ReadBackGuarded(g.setters, hk.kind, netGame, hk.symbol, held)) {
                    TurnOffAfterFault(hk.name, "getter");
                    return false;
                }
                if (!SameStored(hk.kind, held, hk.value)) {
                    ++g.stats.setter_refused;
                    g.hold_refused[i] = true;
                    g.hold_active[i].store(0, std::memory_order_relaxed);
                    char found[32], role[64];
                    FormatValue(hk.kind, held, found, sizeof found);
                    FormatRoleWord(netGame, role, sizeof role);
                    log::Direct(LogLevel::Warn, "playback",
                                "host hold %s: the setter was REFUSED -- the getter reads back %s "
                                "straight after (role word %s). The host's own round clock runs, "
                                "so expect it to leave main_loop",
                                hk.name, found, role);
                    continue;
                }
            }
            owned[pos].symbol = hk.symbol;
            owned[pos].slot = site.Slot();
            continue;
        }

        // Release. The engine's own value goes back only while the key still holds what this hold
        // wrote: anything else means the host moved on and the saved value is stale.
        g.hold_active[i].store(0, std::memory_order_relaxed);
        const bool ours = SameStored(hk.kind, held, hk.value);
        if (ours && !SameStored(hk.kind, g.hold_saved[i], hk.value)) {
            if (!CallSetterGuarded(g.setters, hk.kind, netGame, site, hk.symbol, g.hold_saved[i])) {
                TurnOffAfterFault(hk.name, "setter");
                return false;
            }
            ++g.stats.writes;
        }
        char was[32];
        FormatValue(hk.kind, g.hold_saved[i], was, sizeof was);
        log::Direct(LogLevel::Info, "playback", "host hold RELEASED: %s -- %s (%s)", hk.name,
                    ours ? "the engine's own value is back" : "the host had already moved it off "
                                                              "ours, left alone",
                    was);
    }
    return true;
}

bool SameOwned(const netvarguard::OwnedKey* a, const netvarguard::OwnedKey* b) {
    for (size_t i = 0; i < netvarguard::kMaxOwned; ++i) {
        if (a[i].symbol != b[i].symbol || a[i].slot != b[i].slot) return false;
    }
    return true;
}

void HandOwnedToGuard(const netvarguard::OwnedKey* owned) {
    if (SameOwned(owned, g.host_owned)) return;
    netvarguard::PublishOwned(owned);
    for (size_t i = 0; i < netvarguard::kMaxOwned; ++i) g.host_owned[i] = owned[i];
}

bool AnyHoldActive() {
    for (const std::atomic<uint8_t>& a : g.hold_active) {
        if (a.load(std::memory_order_relaxed) != 0) return true;
    }
    return false;
}

// ★★ THE HOLD OUTLIVES THE ARM ON PURPOSE. It is the HOST's key: left held after an unload, the
// host's round clock never restarts for the rest of the session. So the sweep after the unload --
// which still runs, the match is still up -- gives it back before it goes quiet. A sweep with no
// route yet leaves the holds standing and tries again, which is right: no route, no write.
void ReleaseHostHoldsAfterUnload(void* netGame) {
    for (std::atomic<uint8_t>& w : g.hold_want) w.store(0, std::memory_order_relaxed);
    if (g.faulted.load(std::memory_order_relaxed)) {
        for (std::atomic<uint8_t>& a : g.hold_active) {
            a.store(0, std::memory_order_relaxed);   // the setters raised; nothing may be written
        }
        return;
    }
    const NetVarSite site = LocateNetVarSite(netGame);
    if (!site.RouteOk()) return;
    netvarguard::OwnedKey owned[netvarguard::kMaxOwned] = {};
    DriveHostHolds(netGame, site, owned);
    HandOwnedToGuard(owned);
}

// The second's host writes to each key the guard holds. Drained whether or not they are logged, so
// a second never inherits the one before it.
void CloseHostWriteWindow() {
    netvarguard::Counts counts;
    netvarguard::DrainCounts(counts);
    const netvarguard::Mode mode = netvarguard::CurrentMode();
    if (mode == netvarguard::Mode::PassThrough) return;

    char* at = g.last_host_line;
    char* const end = g.last_host_line + sizeof g.last_host_line;
    at += snprintf(at, size_t(end - at), "hud netvar host writes (%s):",
                   mode == netvarguard::Mode::Filter ? "filter" : "observe");
    size_t keys = 0;
    for (size_t i = 0; i < netvarguard::kMaxOwned && at < end; ++i) {
        if (g.host_owned[i].symbol == 0 && counts.swallowed[i] == 0 && counts.passed[i] == 0) continue;
        at += snprintf(at, size_t(end - at), " %s swallowed=%u passed=%u;", OwnedName(i),
                       counts.swallowed[i], counts.passed[i]);
        ++keys;
    }
    if (keys == 0 || at >= end) return;
    snprintf(at, size_t(end - at), " slot %u | sweeps=%u writes=%u unchanged=%u memoed=%u (last 1 s)",
             unsigned(g.live_slot.load(std::memory_order_relaxed)), g.stats.sweeps, g.stats.writes,
             g.stats.unchanged, g.stats.memoed);
    g.have_host_line = true;
    log::Direct(LogLevel::Info, "playback", "%s", g.last_host_line);
}

void CloseMeasureWindow(uint64_t now_ms) {
    if (g.measure_start_ms == 0) g.measure_start_ms = now_ms;
    if (now_ms - g.measure_start_ms < kMeasureWindowMs) return;
    if (g.measure.frames != 0 || g.measure.writes != 0) {
        FormatHudClockMeasure(g.measure, g.last_line, sizeof g.last_line);
        g.have_line = true;
        log::Direct(LogLevel::Info, "playback", "%s", g.last_line);
    }
    CloseHostWriteWindow();
    g.measure.frames = 0;
    g.measure.overwritten = 0;
    g.measure.writes = 0;
    g.measure_start_ms = now_ms;
}

}  // namespace

size_t HudVarIndexOf(uint64_t symbol) {
    for (size_t i = 0; i < kHudVarCount; ++i) {
        if (kHudVars[i].symbol == symbol) return i;
    }
    return kHudVarCount;
}

HudNetVarSetters LiveHudNetVarSetters() {
    HudNetVarSetters s;
    // A detoured site no longer holds its prologue, and its trampoline IS the original: our writes
    // go through it and never enter the host-write filter.
    const netvarguard::Setters& own = netvarguard::Originals();
    s.setReal = own.setRealNetGame ? own.setRealNetGame
                                   : GuardedSite<decltype(s.setReal)>(netvarguard::kSiteSetRealNetGame);
    s.setSymbol = own.setSymbolNetGame
                      ? own.setSymbolNetGame
                      : GuardedSite<decltype(s.setSymbol)>(netvarguard::kSiteSetSymbolNetGame);
    s.setInt = own.setInt ? own.setInt : GuardedSite<decltype(s.setInt)>(netvarguard::kSiteSetInt);
    s.setFlag = own.setFlag ? own.setFlag
                            : GuardedSite<decltype(s.setFlag)>(netvarguard::kSiteSetFlag);
    s.getReal = GuardedGetter<gamestate::GetFloatFn>("GS.GET_FLOAT", facts::GS_GET_FLOAT,
                                                     facts::GS_GET_FLOAT_ARGMOVE);
    s.getInt = GuardedGetter<gamestate::GetIntFn>("GS.GET_INT", facts::GS_GET_INT,
                                                  facts::GS_GET_INT_ARGMOVE);
    s.getSymbol = GuardedGetter<gamestate::GetSymbolFn>("GS.GET_SYMBOL", facts::GS_GET_SYMBOL,
                                                        facts::GS_GET_SYMBOL_ARGMOVE);
    s.getBool = GuardedGetter<gamestate::GetBoolFn>("GS.GET_BOOL", facts::GS_GET_BOOL,
                                                    facts::GS_GET_BOOL_ARGMOVE);
    return s;
}

namespace {

HudNetVarFrame::Entry ResolveEntry(Window& w, uint64_t symbol, int64_t t_us) {
    HudNetVarFrame::Entry e;
    uint64_t raw = 0;
    uint16_t slot = 0;
    // Before the first row the HUD shows what the recording opens on, not the host's own value.
    if (!w.NetVars().ValueAt(symbol, t_us, raw) && !w.NetVars().FirstValue(symbol, raw)) return e;
    if (!w.NetVarSlotOf(symbol, slot)) return e;
    e.due = true;
    e.slot = slot;
    e.raw = raw;
    return e;
}

}  // namespace

void ResolveHudNetVars(Window& w, int64_t t_us, HudNetVarFrame& out) {
    for (size_t i = 0; i < kHudVarCount; ++i) out.v[i] = ResolveEntry(w, kHudVars[i].symbol, t_us);
    out.frisbee_owner = ResolveEntry(w, kFrisbeeOwnerSymbol, t_us);
}

void FormatHudClockMeasure(const HudClockMeasure& m, char* line, size_t cap) {
    char wrote[24] = "none";
    char found[24] = "unread";
    if (m.have_written) snprintf(wrote, sizeof wrote, "%.3f", m.last_written);
    if (m.have_found) snprintf(found, sizeof found, "%.3f", m.found_next);
    snprintf(line, cap,
             "hud netvar game_timer: wrote=%s found_next_frame=%s overwritten=%u/%u frames "
             "writes=%u (last 1 s)",
             wrote, found, m.overwritten, m.frames, m.writes);
}

void HudNetVarsArm(bool contained, bool clock_on, bool score_on, bool host_filter_on,
                   const HudNetVarSetters& setters) {
    HudNetVarsDisarm();
    if (!contained || (!clock_on && !score_on)) return;
    // The forced-redraw rule keys on a POSITION in the table above, so the position is checked
    // against the table rather than trusted: a row inserted before it would silently move the
    // exemption onto a different key.
    if (strcmp(kHudVars[kHudGameStateIndex].name, "game_state") != 0) {
        log::Direct(LogLevel::Error, "playback",
                    "hud netvars: kHudGameStateIndex names '%s', not game_state -- the key table "
                    "and the forced-redraw rule have drifted apart; not armed",
                    kHudVars[kHudGameStateIndex].name);
        return;
    }
    // Same reason for the probe's window into the table: it reports a notifier verdict per key by
    // POSITION, and a row inserted before blue_score would attribute it to the wrong key.
    if (strcmp(kHudVars[kScoreProbeFirst].name, "blue_score") != 0 ||
        strcmp(kHudVars[kScoreProbeFirst + kScoreProbeCount - 1].name, "red_round_score") != 0) {
        log::Direct(LogLevel::Error, "playback",
                    "hud netvars: the score probe window names '%s'..'%s', not "
                    "blue_score..red_round_score -- the key table and the probe have drifted "
                    "apart; not armed",
                    kHudVars[kScoreProbeFirst].name,
                    kHudVars[kScoreProbeFirst + kScoreProbeCount - 1].name);
        return;
    }
    g.clock_on = clock_on;
    g.score_on = score_on;
    g.host_filter_on = host_filter_on;
    g.setters = setters;
    for (std::atomic<uint8_t>& r : g.key_refused) r.store(0, std::memory_order_relaxed);
    for (bool& r : g.hold_refused) r = false;
    g.force_write.store(1, std::memory_order_relaxed);
    g.force_empty = 0;
    g.load_epoch.fetch_add(1, std::memory_order_relaxed);
    g.armed.store(1, std::memory_order_release);
}

void HudNetVarsDisarm() {
    g.armed.store(0, std::memory_order_release);
    netvarguard::SetMode(netvarguard::Mode::PassThrough);
    // ★★ RELEASE THE KEYS, not just the mode.  `host_owned` is what the next load's first sweep
    // carries forward for a cell that is not due yet -- and after an unload every cell is not due,
    // so an owned table left standing here made the second load re-own all seven keys before it had
    // a single recorded value, swallow the host's writes to them, and leave the scoreboard clock on
    // `--:--` for the rest of the session.  A writer that is not armed owns nothing.
    for (netvarguard::OwnedKey& k : g.host_owned) k = netvarguard::OwnedKey{};
    netvarguard::PublishOwned(g.host_owned);
    ForgetMemos();
    g.live_slot.store(kNetVarSlotNone, std::memory_order_relaxed);
    g.applying.store(0, std::memory_order_relaxed);
    for (Cell& c : g.cells) c.meta.store(0, std::memory_order_relaxed);
    g.frisbee_owner.meta.store(0, std::memory_order_relaxed);
}

void HudNetVarsInvalidate(bool force_redraw) {
    // ★★★ A CELL IS A VALUE AT A PLAYHEAD, AND THE PLAYHEAD JUST MOVED. Nothing cleared a cell
    // except unload, so after a seek the sweep kept re-asserting the PRE-SEEK value for as long as
    // the new window had no row for the key -- the clock frozen at 01:29.27 while the chapter set
    // demonstrably walked forward. Clearing them makes the next publish the only writer, and the
    // publish runs later in the same tick, so no frame passes with nothing asserted. Ownership is
    // NOT dropped here: the sweep carries it across an un-asserted cell on purpose.
    for (Cell& c : g.cells) c.meta.store(0, std::memory_order_release);
    g.frisbee_owner.meta.store(0, std::memory_order_release);
    ForgetMemos();
    // The playhead moved, so the widgets owe the viewer a redraw at the new T even when the value
    // at it happens to equal the value they were last given -- but only when the viewer ASKED it
    // to move.  See the header.
    if (force_redraw) {
        g.force_empty = 0;
        g.force_write.store(1, std::memory_order_release);
    }
}

bool HudNetVarsOwns(uint64_t symbol, uint16_t recorded_slot) {
    if (!g.armed.load(std::memory_order_acquire)) return false;
    const size_t i = HudVarIndexOf(symbol);
    if (i == kHudVarCount) return false;
    // A group whose flag is off is driven by nobody: withheld here, never resolved for the sweep.
    if (!GroupOn(kHudVars[i].group)) return true;
    if (g.faulted.load(std::memory_order_relaxed) || !SetterFor(g.setters, kHudVars[i].kind) ||
        g.key_refused[i].load(std::memory_order_relaxed)) {
        return false;   // the setter cannot run or does not land, so the decoder path keeps the key
    }
    return g.live_slot.load(std::memory_order_relaxed) == recorded_slot;
}

void HudNetVarsPublish(const HudNetVarFrame& frame) {
    for (size_t i = 0; i < kHudVarCount; ++i) {
        const HudNetVarFrame::Entry& e = frame.v[i];
        if (!e.due) continue;   // held: a seek into a window with no row for the key changes nothing
        // Value before the flag that says it is there, so a reader never pairs `due` with a stale raw.
        g.cells[i].raw.store(e.raw, std::memory_order_relaxed);
        g.cells[i].meta.store(kCellDue | e.slot, std::memory_order_release);
    }
    // Not held: the decoder writes this key, and it stops being ours the tick the decoder unbinds.
    const HudNetVarFrame::Entry& f = frame.frisbee_owner;
    g.frisbee_owner.meta.store(f.due ? (kCellDue | f.slot) : 0u, std::memory_order_release);
}

void HudNetVarsIdle() { netvarguard::SetMode(netvarguard::Mode::PassThrough); }

void HudNetVarsSetHostHold(bool round_time, bool pause) {
    g.hold_want[0].store(round_time ? 1u : 0u, std::memory_order_release);
    g.hold_want[1].store(pause ? 1u : 0u, std::memory_order_release);
}

bool HudNetVarsHostHoldHeld(size_t which) {
    return which < kHoldKeyCount && g.hold_active[which].load(std::memory_order_relaxed) != 0;
}

const char* HudNetVarsHostHoldName(size_t which) {
    return which < kHoldKeyCount ? kHoldKeys[which].name : "";
}

void HudNetVarsSweep(void* netGame, uint64_t now_ms) {
    // This tail is the only place the guard's mode is raised, so an unload that raced the previous
    // sweep is put right here, one frame later.
    if (!g.armed.load(std::memory_order_acquire) || g.faulted.load(std::memory_order_relaxed)) {
        if (AnyHoldActive()) ReleaseHostHoldsAfterUnload(netGame);
        netvarguard::SetMode(netvarguard::Mode::PassThrough);
        return;
    }
    ++g.stats.sweeps;

    const uint32_t epoch = g.load_epoch.load(std::memory_order_relaxed);
    if (epoch != g.seen_epoch) {
        g.seen_epoch = epoch;
        g.route_ok = false;
        g.route_refusal = kNoRefusal;
        g.have_timer_expected = false;
        ForgetMemos();
        g.measure = HudClockMeasure{};
        g.measure_start_ms = 0;
        memset(g.slot_refusal_logged, 0, sizeof g.slot_refusal_logged);
        g.have_host_line = false;
    }

    // Re-validated every sweep: the netGame-form setters resolve their own copy of this handle, and
    // SetInt indexes the pool by the handle's slot alone, so a handle that no longer names this
    // level's live record must not be written through. The ROUTE is all that is asked for: the
    // setters and getters walk the pool themselves and never need the store pointer.
    tickcost::Scope cost_site(tickcost::kHudSite);
    if (!g.have_cached_site || g.cached_site_netgame != netGame || g.cached_site_ms != now_ms) {
        g.cached_site = LocateNetVarSite(netGame);
        g.cached_site_netgame = netGame;
        g.cached_site_ms = now_ms;
        g.have_cached_site = true;
    }
    const NetVarSite site = g.cached_site;
    cost_site.Close();
    if (!site.RouteOk()) {
        ++g.stats.route_invalid;
        netvarguard::SetMode(netvarguard::Mode::PassThrough);
        g.live_slot.store(kNetVarSlotNone, std::memory_order_relaxed);
        g.applying.store(0, std::memory_order_relaxed);
        g.have_timer_expected = false;   // a new level's store owes us nothing
        ForgetMemos();
        if (g.route_ok || site.RefusalKey() != g.route_refusal) {
            char detail[512];
            FormatNetVarSite(site, detail, sizeof detail);
            log::Direct(LogLevel::Warn, "playback", "hud netvars: route %s at netgame state %d -- %s",
                        g.route_ok ? "lost" : "not valid", (int)NetGameStateOf(netGame), detail);
        }
        g.route_ok = false;
        g.route_refusal = site.RefusalKey();
        return;
    }
    g.live_slot.store(site.Slot(), std::memory_order_relaxed);
    if (!g.route_ok) {
        g.route_ok = true;
        g.route_refusal = kNoRefusal;
        char role[64];
        FormatRoleWord(netGame, role, sizeof role);
        log::Direct(LogLevel::Info, "playback",
                    "hud netvars: route valid at netgame state %d (cs %p, slot %u generation %u -> "
                    "record %u of %u, level space %p, role word %s; store %p [%s] -- not needed by "
                    "the setters)",
                    (int)NetGameStateOf(netGame), site.netVarCs, unsigned(site.Slot()),
                    unsigned(site.HandleGeneration()), unsigned(site.pool.index), site.records,
                    site.space, role, site.pool.store, NetVarPoolStepText(site.pool.step));
    }

    // Consumed here, not at the top: a sweep that never reached the setters has not delivered the
    // redraw the load or the seek asked for, and must not swallow the request for it.
    const bool force = g.force_write.exchange(0, std::memory_order_acq_rel) != 0;

    // The host's writes are filtered for exactly the keys this sweep leaves holding their recorded
    // value: a key nobody here is writing stays the host's.
    netvarguard::OwnedKey owned[netvarguard::kMaxOwned];
    uint32_t held_now = 0;
    uint32_t forced_mask = 0;
    for (size_t i = 0; i < kHudVarCount; ++i) {
        const HudVarSpec& spec = kHudVars[i];
        const bool is_timer = (i == kHudGameTimerIndex);
        const uint32_t meta = g.cells[i].meta.load(std::memory_order_acquire);
        // ★★★ NOT DUE IS NOT "NOT OURS". A window rebuild, a seek or a chapter with no row for the
        // key leaves the cell un-asserted for a frame or two; releasing the key over that hands the
        // record straight back to the host, whose pre-match `game_state` reaches it on the very next
        // engine update and draws `--:--`. Ownership is carried exactly as the reentrant branch
        // below carries it -- one un-asserted frame is not a change of owner.
        if ((meta & kCellDue) == 0) {
            if (is_timer) g.have_timer_expected = false;
            owned[i] = g.host_owned[i];
            continue;
        }
        // These three ARE a change of owner, and must stay one: a group switched off, a setter that
        // does not exist and a key the engine refused are all "nobody here is writing this", so
        // swallowing the host's writes would leave the widget with no writer at all.
        if (!GroupOn(spec.group) || !SetterFor(g.setters, spec.kind) ||
            g.key_refused[i].load(std::memory_order_relaxed)) {
            if (is_timer) g.have_timer_expected = false;
            continue;
        }
        const uint64_t raw = g.cells[i].raw.load(std::memory_order_relaxed);

        if (uint16_t(meta) != site.Slot()) {
            ++g.stats.slot_refused;
            if (!g.slot_refusal_logged[i]) {
                g.slot_refusal_logged[i] = true;
                log::Direct(LogLevel::Warn, "playback",
                            "hud netvar %s: recorded in slot %u, the netGame's global netvar handle "
                            "is slot %u -- not written",
                            spec.name, unsigned(uint16_t(meta)), unsigned(site.Slot()));
            }
            // Transient in the same way a not-due cell is: the live handle's slot moves when the
            // session re-seats its netvar record, and the key is still ours across that.
            owned[i] = g.host_owned[i];
            continue;
        }

        // The forced redraw is for the DISPLAY keys. See kHudGameStateIndex for why that one is
        // written only when its value actually changed.
        const bool forceable = force && i != kHudGameStateIndex;

        // ★ THE MEMO, and it is what makes this sweep affordable. The engine's getter spins on the
        // record's lock and answers the question the memo already answers: what this record holds.
        // Three terms make trusting it sound:
        //   * the guard is in Filter mode, so the host's writes to a key we own are swallowed
        //     before they reach the record -- with the filter off the host CAN move it and the
        //     read-back is the only thing that would notice, so there the getter still runs;
        //   * the timer is excluded: its recorded value moves every frame anyway, and its
        //     read-back is the `overwritten=` measurement;
        //   * a memo is trusted for kMemoVerifyMs, so anything else that reaches the record is
        //     still found -- once a second instead of on every one of the ~240 sweeps a second.
        if (g.host_filter_on && !forceable && !is_timer && g.memo_valid[i] &&
            SameStored(spec.kind, g.memo[i], raw) &&
            now_ms - g.memo_ms[i] < kMemoVerifyMs) {
            ++g.stats.unchanged;
            ++g.stats.memoed;
            ++held_now;
            owned[i].symbol = spec.symbol;
            owned[i].slot = site.Slot();
            continue;
        }

        // Getters and setters both spin on the record's lock with no recursion count, so neither
        // may run from inside a change delegate one of our own setter calls dispatched.
        PawnProvision::EngineCallLatch latch;
        if (!latch.taken) {
            ++g.stats.reentrant;
            if (is_timer) g.have_timer_expected = false;
            owned[i] = g.host_owned[i];   // one skipped frame is not a change of owner
            continue;
        }

        uint64_t held = 0;
        if (!ReadBackGuarded(g.setters, spec.kind, netGame, spec.symbol, held)) {
            TurnOffAfterFault(spec.name, "getter");
            return;
        }

        if (is_timer && g.have_timer_expected) {
            ++g.measure.frames;
            g.measure.have_found = true;
            g.measure.found_next = AsFloat(held);
            if (!SameStored(spec.kind, held, g.timer_expected)) ++g.measure.overwritten;
        }

        if (SameStored(spec.kind, held, raw) && !forceable) {
            ++g.stats.unchanged;
        } else {
            const uint64_t before = held;
            if (forceable) forced_mask |= 1u << i;
            if (!CallSetterGuarded(g.setters, spec.kind, netGame, site, spec.symbol, raw)) {
                TurnOffAfterFault(spec.name, "setter");
                return;
            }
            ++g.stats.writes;
            ++g.stats.writes_by_key[i];
            if (is_timer) ++g.measure.writes;

            // The setters return nothing and refuse in silence; they are synchronous, so what the
            // getter reads now is what the call left.
            if (!ReadBackGuarded(g.setters, spec.kind, netGame, spec.symbol, held)) {
                TurnOffAfterFault(spec.name, "getter");
                return;
            }
            if (!SameStored(spec.kind, held, raw)) {
                ++g.stats.setter_refused;
                g.key_refused[i].store(1, std::memory_order_relaxed);
                if (is_timer) g.have_timer_expected = false;
                char wrote[32], found[32], role[64];
                FormatValue(spec.kind, raw, wrote, sizeof wrote);
                FormatValue(spec.kind, held, found, sizeof found);
                FormatRoleWord(netGame, role, sizeof role);
                log::Direct(LogLevel::Warn, "playback",
                            "hud netvar %s: the setter was REFUSED -- wrote %s, the getter reads back "
                            "%s straight after (the setter's authority gate; role word %s). Not "
                            "written again for this load",
                            spec.name, wrote, found, role);
                continue;
            }

            // ★ THE ONE MEASUREMENT THE GOAL CELEBRATION HANGS ON. Every score netvar setter calls
            // the change notifier sub_140CCF9E0 ONLY when SetValue returned non-zero, and SetValue
            // returns non-zero only when the stored value actually changed -- so the notifier ran
            // exactly when the value the getter read BEFORE the call differed from what we wrote.
            // That is not an inference about the write: it is the same comparison the setter makes,
            // at the same width (SameStored). The scripts that draw the flash and play the accrual
            // SFX are downstream of that one notifier, and until this line nothing had ever
            // recorded whether it fired on a replayed goal. The explosion is NOT downstream of it
            // -- RE-REMOTE-PRESENTATION Q4 traced it to nevt_increment_score_* instead.
            if (g.score_probe.load(std::memory_order_relaxed) && IsScoreProbeKey(i)) {
                const bool notified = !SameStored(spec.kind, before, raw);
                char was[32], now[32];
                FormatValue(spec.kind, before, was, sizeof was);
                FormatValue(spec.kind, raw, now, sizeof now);
                log::Direct(LogLevel::Info, "playback",
                            "goals: score netvar %s %s -> %s at playhead %.3f s: %s write, "
                            "notifier=%s -- the goal flash and the accrual sfx are downstream of "
                            "it; the explosion is not (nevt_increment_score_*)",
                            spec.name, was, now,
                            double(g.playhead_us.load(std::memory_order_relaxed)) / 1e6,
                            forceable ? "forced" : "sweep", notified ? "ran" : "skipped");
            }
        }
        ++held_now;
        owned[i].symbol = spec.symbol;
        owned[i].slot = site.Slot();
        // Reaching here means the record holds `raw`: either the getter read it back unchanged, or
        // the setter wrote it and the confirming read agreed. Refusals `continue` above this.
        g.memo[i] = raw;
        g.memo_ms[i] = now_ms;
        g.memo_valid[i] = true;

        if (is_timer) {
            g.timer_expected = raw;
            g.have_timer_expected = true;
            g.measure.last_written = AsFloat(raw);
            g.measure.have_written = true;
        }
    }
    g.applying.store(held_now != 0, std::memory_order_relaxed);

    // ★★ THE ONE LINE THAT SETTLES THE `--:--`. The launch showed the clock widget drawing dashes
    // while paused at T0 with game_timer demonstrably holding 119.837, and the widget's own gate is
    // not in the executable -- there is no "--:--" string in it, so it is script-side and only a
    // launch can read it. The rival explanations are "no change event while paused" (the forced
    // write above answers that) and "the recording's own game_state at T0 is a pre-match state the
    // widget draws dashes for", which is FAITHFUL and wants no fix. This prints every clock key's
    // value at T at exactly the moments the dashes were seen, so the next launch tells them apart.
    // ★★ A FORCED SWEEP THAT WROTE NOTHING HAS NOT DELIVERED THE REDRAW IT CONSUMED.  On a second
    // load the netvar route is already valid, so the first sweep runs in the same tick as the arm,
    // before any cell has been published -- and that sweep spent the force, so `game_state` was
    // never re-asserted and the scoreboard clock stayed on `--:--` for the whole load.  The request
    // is put back until a sweep actually publishes, bounded so a file that carries no rows at all
    // does not re-arm for ever.
    constexpr uint32_t kForceRetryMax = 120;   // ~4 s at the observed sweep rate
    bool report = force;
    if (force) {
        if (held_now == 0 && g.force_empty < kForceRetryMax) {
            ++g.force_empty;
            g.force_write.store(1, std::memory_order_release);
            report = false;       // nothing to report yet, and nothing spent
        } else {
            g.force_empty = 0;
        }
    }

    if (report) {
        char line[512];
        size_t used = 0;
        for (size_t i = 0; i < kHudVarCount; ++i) {
            const uint32_t meta = g.cells[i].meta.load(std::memory_order_relaxed);
            char val[32] = "no value at T";
            if ((meta & kCellDue) != 0) {
                FormatValue(kHudVars[i].kind, g.cells[i].raw.load(std::memory_order_relaxed), val,
                            sizeof val);
            }
            const int n = _snprintf_s(line + used, sizeof line - used, _TRUNCATE, "%s%s=%s%s",
                                      i ? " " : "", kHudVars[i].name, val,
                                      (forced_mask & (1u << i)) ? "*" : "");
            if (n < 0) break;
            used += (size_t)n;
        }
        log::Direct(LogLevel::Info, "playback",
                    "hud netvars at the playhead after a load or a jump (* = written again to fire "
                    "the change event): %s", line);
    }

    const uint32_t frisbee = g.frisbee_owner.meta.load(std::memory_order_acquire);
    if ((frisbee & kCellDue) != 0 && uint16_t(frisbee) == site.Slot()) {
        owned[kFrisbeeOwnedIndex].symbol = kFrisbeeOwnerSymbol;
        owned[kFrisbeeOwnedIndex].slot = site.Slot();
    }
    {
        const tickcost::Scope cost_holds(tickcost::kHudHolds);
        if (!DriveHostHolds(netGame, site, owned)) return;
    }
    HandOwnedToGuard(owned);
    netvarguard::SetMode(g.host_filter_on ? netvarguard::Mode::Filter : netvarguard::Mode::Observe);

    CloseMeasureWindow(now_ms);
}

HudNetVarStats HudNetVarsStats() { return g.stats; }

bool HudNetVarsApplying() { return g.applying.load(std::memory_order_relaxed) != 0; }

void HudNetVarsSetScoreProbe(bool on) {
    g.score_probe.store(on ? 1u : 0u, std::memory_order_relaxed);
}

void HudNetVarsSetPlayheadUs(int64_t playhead_us) {
    g.playhead_us.store(playhead_us, std::memory_order_relaxed);
}

// Every term that would stop a recorded score change from reaching the engine's change notifier,
// in the order the sweep itself would hit them. Named rather than counted because each one is a
// different defect: an off switch, a bind that failed, and a setter the engine's own authority gate
// refused all look identical on screen -- no flash, no explosion, no horn.
const char* HudNetVarsScoreTriggerRefusal() {
    if (g.armed.load(std::memory_order_acquire) == 0)
        return "the HUD netvar writer is not armed";
    if (g.faulted.load(std::memory_order_relaxed) != 0)
        return "the HUD netvar writer faulted earlier this session";
    if (!g.score_on) return "playback.gamestate_score is off, so nothing writes the score keys";
    if (!SetterFor(g.setters, kHudVars[kScoreProbeFirst].kind))
        return "the int netvar setter did not bind in this image";
    for (size_t i = kScoreProbeFirst; i < kScoreProbeFirst + kScoreProbeCount; ++i) {
        if (g.key_refused[i].load(std::memory_order_relaxed) != 0)
            return "the engine refused the score setter, so the keys are not written again";
    }
    if (!g.route_ok) return "the netvar route was not valid at the last sweep";
    return nullptr;
}

bool HudNetVarsLastMeasureLine(char* line, size_t cap) {
    if (!g.have_line || !line || !cap) return false;
    snprintf(line, cap, "%s", g.last_line);
    return true;
}

bool HudNetVarsLastHostWriteLine(char* line, size_t cap) {
    if (!g.have_host_line || !line || !cap) return false;
    snprintf(line, cap, "%s", g.last_host_line);
    return true;
}

void HudNetVarsResetForTest() {
    HudNetVarsDisarm();
    for (std::atomic<uint8_t>& w : g.hold_want) w.store(0, std::memory_order_relaxed);
    for (std::atomic<uint8_t>& a : g.hold_active) a.store(0, std::memory_order_relaxed);
    for (bool& r : g.hold_refused) r = false;
    for (uint64_t& s : g.hold_saved) s = 0;
    g.faulted.store(0, std::memory_order_relaxed);
    g.clock_on = false;
    g.score_on = false;
    g.host_filter_on = false;
    g.have_host_line = false;
    for (netvarguard::OwnedKey& k : g.host_owned) k = netvarguard::OwnedKey{};
    g.setters = HudNetVarSetters{};
    g.stats = HudNetVarStats{};
    g.measure = HudClockMeasure{};
    g.measure_start_ms = 0;
    g.have_timer_expected = false;
    g.route_ok = false;
    g.route_refusal = kNoRefusal;
    for (std::atomic<uint8_t>& r : g.key_refused) r.store(0, std::memory_order_relaxed);
    g.have_line = false;
    g.force_write.store(0, std::memory_order_relaxed);
    g.force_empty = 0;
    memset(g.slot_refusal_logged, 0, sizeof g.slot_refusal_logged);
}

}  // namespace playback
}  // namespace replay
