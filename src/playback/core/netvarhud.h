#pragma once

// The netvars the in-arena HUD renders, driven from the playhead through the
// engine's own typed setters.
//
// The clock the player sees is the `game_timer` NETVAR, read by script widgets that redraw on the
// netvar-change event (RE-CLOCK-SCOREBOARD-PHASE S4.4). The engine's setters do both halves -- store
// the value and emit the event -- so these seven keys go through them instead of the store decoder
// every other recorded netvar takes:
//
//   CR15NetVarCS::SetReal  (CR15NetGameMT&, CSymbol64, float)             sub_140D47E50
//   CR15NetVarCS::SetSymbol(CR15NetGameMT&, CSymbol64, CSymbol64, float)  sub_140D48EC0
//   CR15NetVarCS::SetInt   (SCompactPoolHandle, CSymbol64, int)           sub_140D46790  (cs, handle, ..)
//
// All three are authority-gated; the offline replay host is the authority. The two netGame forms
// address the netGame's global netvar handle, and SetInt is handed that same handle, so a recorded
// key is written only when its recorded slot IS that handle's slot -- (slot, symbol), never symbol
// alone.
//
// Two threads' worth of state, kept apart. The playback tick owns the window, so it resolves the
// values at T and publishes them; the NETGAME.UPDATE stage-3 tail owns every engine touch. On the
// host the arena script also publishes `game_timer` every frame, and which of the two lands last is
// not knowable statically, so the sweep measures it (HudClockMeasure) rather than assuming.
//
// The writer needs the ROUTE (NetVarSite::RouteOk), never the store: what a key holds is read back
// through the engine's typed getters, which walk the same pool the setters do.

#include <stddef.h>
#include <stdint.h>

#include "engine/gamestate.h"
#include "playback/core/netvarsite.h"

namespace replay {
namespace playback {

class Window;

enum class HudVarKind : uint8_t { Real, Int, Symbol, Flag };
enum class HudVarGroup : uint8_t { Clock, Score };   // playback.gamestate_clock / playback.gamestate_score

struct HudVarSpec {
    uint64_t    symbol;
    HudVarKind  kind;
    HudVarGroup group;
    const char* name;
};

// APPEND ONLY.  Recorded keyframe rows and at least one test index into this table by position,
// so a row inserted anywhere but the end re-labels values in files already on disk.
inline constexpr size_t kHudVarCount = 9;
inline constexpr size_t kHudGameTimerIndex = 0;   // the key the script conflict is measured on
// The one key a forced redraw skips when its value has not changed: the host's own scripts act on
// `game_state`, so re-asserting a value the record already holds is a state change to everything
// downstream of it and buys no pixel -- the widget is already showing it.
inline constexpr size_t kHudGameStateIndex = 2;
extern const HudVarSpec kHudVars[kHudVarCount];

// The index of `symbol` in kHudVars, or kHudVarCount.
size_t HudVarIndexOf(uint64_t symbol);

// The engine's setters, as function pointers so a test supplies stand-ins. A null member means that
// kind cannot be written (its site failed the prologue guard) and its keys stay with the decoder.
//
// Each kind also needs its getter -- the sites gamestate::LiveEngineAccess binds (GS.GET_FLOAT /
// GS.GET_INT / GS.GET_SYMBOL), under the same argument-move guard and called as gamestate.h's
// typedefs spell them, handed the netGame. A kind that cannot be read back is not written.
struct HudNetVarSetters {
    gamestate::GetFloatFn  getReal = nullptr;
    gamestate::GetIntFn    getInt = nullptr;
    gamestate::GetSymbolFn getSymbol = nullptr;
    gamestate::GetBoolFn   getBool = nullptr;
    void (__fastcall* setReal)(void* netGame, uint64_t symbol, float value) = nullptr;
    // The netVarCs/handle shape, like setInt -- NOT the netGame shape the real and symbol setters
    // take.  sub_140D45820, already one of the six ledgered setter sites.
    void (__fastcall* setFlag)(void* netVarCs, uint32_t handle, uint64_t symbol, uint8_t value) = nullptr;
    void (__fastcall* setSymbol)(void* netGame, uint64_t symbol, uint64_t value, float lifetime) = nullptr;
    void (__fastcall* setInt)(void* netVarCs, uint32_t handle, uint64_t symbol, uint32_t value) = nullptr;
};
HudNetVarSetters LiveHudNetVarSetters();

// What the playhead resolved for each HUD key this tick. `due` is false when the window carries no
// row for the key or its recorded slot is unknown. Which groups are written is the sweep's call.
struct HudNetVarFrame {
    struct Entry {
        bool     due = false;
        uint16_t slot = 0;
        uint64_t raw = 0;   // the recorded SValue qword
    };
    Entry v[kHudVarCount];
    // game_frisbee_ownerid. The store decoder writes it; it is here so the host's native per-tick
    // write of it can be filtered (netvarguard). The caller clears `due` while the decoder is unbound.
    Entry frisbee_owner;
};

// value(key, T) = the last recorded write at or before T -- never interpolated, so a round reset
// (0 -> 120) is a step. Before the key's first row in the window it is that first row's value. The
// same function forwards, backwards, paused and after a seek.
void ResolveHudNetVars(Window& w, int64_t t_us, HudNetVarFrame& out);

// One second of the script-conflict measurement on game_timer.
struct HudClockMeasure {
    uint32_t frames = 0;        // sweeps that had a previous plugin value to compare against
    uint32_t overwritten = 0;   // of those, the getter no longer read it back
    uint32_t writes = 0;        // setter calls for game_timer
    float    last_written = 0;  // the value the plugin last left in the store
    float    found_next = 0;    // what the getter read at the following sweep
    bool     have_written = false;
    bool     have_found = false;
};
void FormatHudClockMeasure(const HudClockMeasure& m, char* line, size_t cap);

struct HudNetVarStats {
    uint32_t sweeps = 0;
    uint32_t route_invalid = 0;   // sweeps refused because the route was not valid
    uint32_t writes = 0;          // setter calls
    uint32_t unchanged = 0;       // the getter already read the value: no call
    uint32_t memoed = 0;          // of those, the ones that cost no getter either: the memo answered
    uint32_t setter_refused = 0;  // keys whose write did not read back: the setter's own gate refused
    uint32_t slot_refused = 0;    // recorded slot is not the global handle's slot
    uint32_t reentrant = 0;       // EngineCallLatch already held
    uint32_t faulted = 0;         // a setter raised; the writer is off for the session
    uint32_t writes_by_key[kHudVarCount] = {};
};

// ---- the module's surface ------------------------------------------------------------------------

// Arms the writer for one loaded replay. Nothing is written unless `contained` (not a live match)
// and at least one group is on. `host_filter_on` (playback.host_netvar_filter) has the setter
// detours swallow the host's writes to the keys this writer is holding; off, they only count them.
// Disarm on unload.
void HudNetVarsArm(bool contained, bool clock_on, bool score_on, bool host_filter_on,
                   const HudNetVarSetters& setters);
void HudNetVarsDisarm();
// Drops every cell's assertion WITHOUT disarming or releasing a key. The caller calls this the
// moment the playhead stops meaning what the cells were resolved at -- a window rebuild or a jump --
// and re-publishes at the new T later in the same tick. Without it a cell with no row in the new
// window keeps asserting the value from before the seek, forever.
// `force_redraw` is the ACTION half and it is not free: it re-asserts every display key even where
// the record already holds the recorded value.  A user seek and a load owe the viewer that redraw;
// a window roll, a catch-up micro-seek and a capture-gap crossing do not -- they are reports, and
// wiring a report to an action is what re-wrote `game_state` at every chapter seam.
void HudNetVarsInvalidate(bool force_redraw = true);
// Whether the writer owns (slot, symbol) right now, so the store-decoder path leaves it alone. Only
// while the route holds and the recorded slot is the global handle's: a key this writer would
// refuse stays with the decoder, which addresses any slot.
bool HudNetVarsOwns(uint64_t symbol, uint16_t recorded_slot);
// The playback tick hands over what it resolved at T. A key that is not due keeps the value it was
// last handed, so the writer never goes quiet on a key it has driven during this load.
void HudNetVarsPublish(const HudNetVarFrame& frame);
// The stage-3 tail. `now_ms` paces the once-a-second measurement lines.
void HudNetVarsSweep(void* netGame, uint64_t now_ms);
// The stage-3 tail outside a running match: the host's writes pass untouched.
void HudNetVarsIdle();

// ★ HOST-HOLD. Two of the HOST's own netvars, held at a constant while a replay is loaded and the
// host is playing, so its arena script's round clock never reaches zero and the viewing outlives
// the host's 5-minute round. Not resolved from the recording and not part of kHudVars: a recorded
// value for either key loses to the hold for as long as the hold is up, which costs nothing because
// a recorded pause is a fact about the match that was played, not about the viewing of it.
// Published by the playback tick; taken and given back by the sweep, which restores the value the
// engine held before it -- including on the sweep after an unload, when the writer is already
// disarmed. `round_time` is the invisible one and defaults on; `pause` is stronger and may draw a
// PAUSED overlay (open item O-G), so it defaults off.
void HudNetVarsSetHostHold(bool round_time, bool pause);
// 0 = the round-time seed, 1 = the pause state. Held means written and owned right now.
bool HudNetVarsHostHoldHeld(size_t which);
const char* HudNetVarsHostHoldName(size_t which);

// playback.goal_score_probe. One log line per blue_score/red_score/blue_round_score/red_round_score
// write, carrying the value before, the value after, the playhead, whether the write was the forced
// load-time one, and whether the engine's change notifier ran -- which is the trigger for the goal
// flash, the accrual SFX and the goal_fx explosion. Default on; costs one line per score change.
void HudNetVarsSetScoreProbe(bool on);
// The probe line's axis, handed over by the playback tick. Reporting only.
void HudNetVarsSetPlayheadUs(int64_t playhead_us);
// Why a score change would NOT reach that notifier right now, as a program-lifetime string; nullptr
// means it would. The goal feed prints it as the reason the flash and the explosion did not happen.
const char* HudNetVarsScoreTriggerRefusal();

HudNetVarStats HudNetVarsStats();
// The last sweep found every due key holding its recorded value, by its own write or already.
bool HudNetVarsApplying();
// The measurement line most recently completed; false until a full second has been measured.
bool HudNetVarsLastMeasureLine(char* line, size_t cap);
// The host-write line for the same second; false until a second with the guard watching has closed.
bool HudNetVarsLastHostWriteLine(char* line, size_t cap);
void HudNetVarsResetForTest();

}  // namespace playback
}  // namespace replay
