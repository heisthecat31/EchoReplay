#pragma once

// HOST-NV. Entry detours on the six CR15NetVarCS setters -- the complete set of engine-side netvar
// writers (each is the only caller of its CReplicatedVariables::SetValue<T>):
//
//   SetFlag  (cs, SCompactPoolHandle, CSymbol64, bool)               sub_140D45820
//   SetInt   (cs, SCompactPoolHandle, CSymbol64, int)                sub_140D46790
//   SetReal  (cs, SCompactPoolHandle, CSymbol64, float)              sub_140D47D60
//   SetReal  (CR15NetGameMT&, CSymbol64, float)                      sub_140D47E50
//   SetSymbol(cs, SCompactPoolHandle, CSymbol64, CSymbol64, float)   sub_140D48DD0   5th arg on the stack
//   SetSymbol(CR15NetGameMT&, CSymbol64, CSymbol64, float)           sub_140D48EC0
//
// On the replay host the arena script republishes the match-state netvars through these every graph
// update, and the engine writes game_frisbee_ownerid natively every stage-3 tick, so a replayed value
// lasts until the host's next pass. While a replay is driving a key, a host write to that same
// (slot, symbol) returns here without reaching the original: no store write, no replication, no
// change notifier. All six return void, so there is nothing for a caller to miss.
//
// Installed once at Init on the worker, only in a contained process, and never removed; what the
// detours do is one atomic mode word. Our own writes call the trampolines, so they never enter a
// detour at all -- and a script write re-entering from the change delegate of one of our writes is
// an ordinary host write, filtered like any other.
//
// Ownership is (slot, symbol): the same symbol in a component the replay is not driving passes.

#include <stddef.h>
#include <stdint.h>

#include "bootstrap/hookmgr.h"

namespace replay {
namespace playback {
namespace netvarguard {

// In Setters' member order.
enum Site : size_t {
    kSiteSetFlag,
    kSiteSetInt,
    kSiteSetReal,
    kSiteSetRealNetGame,
    kSiteSetSymbol,
    kSiteSetSymbolNetGame,
    kSiteCount,
};
// One position per HUD key, the frisbee owner, then one per host-hold key.  Grown from 8 with the
// two phase flags (sudden_death_active, match_is_over) and from 10 with the two HOST-HOLD keys;
// netvarhud.cpp static_asserts that it still covers them.
inline constexpr size_t kMaxOwned = 12;
inline constexpr char kLedgerKey[] = "playback.host_netvar_hook";

enum class Mode : uint32_t {
    PassThrough,   // nothing loaded, or not in a match: a load and a branch
    Observe,       // count host writes to owned keys, swallow nothing
    Filter,        // swallow host writes to owned keys
};

using SetFlagFn          = void (__fastcall*)(void* netVarCs, uint32_t handle, uint64_t symbol, uint8_t value);
using SetIntFn           = void (__fastcall*)(void* netVarCs, uint32_t handle, uint64_t symbol, uint32_t value);
using SetRealFn          = void (__fastcall*)(void* netVarCs, uint32_t handle, uint64_t symbol, float value);
using SetRealNetGameFn   = void (__fastcall*)(void* netGame, uint64_t symbol, float value);
using SetSymbolFn        = void (__fastcall*)(void* netVarCs, uint32_t handle, uint64_t symbol, uint64_t value, float lifetime);
using SetSymbolNetGameFn = void (__fastcall*)(void* netGame, uint64_t symbol, uint64_t value, float lifetime);

struct Setters {
    SetFlagFn          setFlag = nullptr;
    SetIntFn           setInt = nullptr;
    SetRealFn          setReal = nullptr;
    SetRealNetGameFn   setRealNetGame = nullptr;
    SetSymbolFn        setSymbol = nullptr;
    SetSymbolNetGameFn setSymbolNetGame = nullptr;
};

struct SiteFact {
    const char*    factId;
    uint64_t       va;
    const uint8_t* prologue;
    size_t         prologueLen;
};
const SiteFact& SiteAt(Site site);

// 0 is not a legal CSymbol64: a position with symbol 0 owns nothing.
struct OwnedKey {
    uint64_t symbol = 0;
    uint16_t slot = 0;
};

// Worker thread, once, at Init. Installs all six or none: every prologue is verified before the
// first trampoline is built, and one refused Create leaves the rest unarmed. Does nothing unless the
// ledger key resolved to "hook", which bootstrap publishes only for a contained process.
bool Install();
bool Installed();

// The originals, for the plugin's own writes. All null until Install succeeds; a caller then still
// owns the pristine sites and resolves them itself.
const Setters& Originals();

// Replaces the owned set. Position i keeps its meaning across calls: counters are per position.
// One writer at a time (the stage-3 tail); the detours read it from any thread.
void PublishOwned(const OwnedKey keys[kMaxOwned]);
void SetMode(Mode mode);
Mode CurrentMode();

// THE HOST'S OWN VALUE FOR ONE SYMBOL. For a key the replay drives, the store answers with OUR
// value and the host's is swallowed here, so these detours are the only place it is still visible.
// Recorded on both SetSymbol sites, for any slot, before the filter -- swallowed or passed.
void WatchHostSymbol(uint64_t symbol);

struct HostSymbol {
    uint64_t value = 0;    // meaningless while writes == 0
    uint32_t writes = 0;   // 0 = the host has never written the watched symbol through these sites
};
HostSymbol LastHostSymbol();

// Host writes to each owned position since the last drain.
struct Counts {
    uint32_t swallowed[kMaxOwned] = {};
    uint32_t passed[kMaxOwned] = {};
};
void DrainCounts(Counts& out);

// ---- test seams -----------------------------------------------------------------------------------

using HookCreateFn = HookStatus (*)(const char*, uint64_t, const uint8_t*, size_t, void*, void**,
                                    const char*);
using HookArmFn = HookStatus (*)(const char*);
using VerifySiteFn = bool (*)(const char*, uint64_t, const uint8_t*, size_t);
void SetHookAccessForTest(HookCreateFn create, HookArmFn arm, VerifySiteFn verify);

// The six detours as the engine would enter them, and stand-ins for what they forward to.
const Setters& DetoursForTest();
// A host write of `symbol`, without the six sites: what the detours record, recorded directly.
// symbol 0 forgets the record instead, so a case leaves nothing behind for the next one.
void NoteHostSymbolForTest(uint64_t symbol, uint64_t value);
void SetOriginalsForTest(const Setters& originals);
void ResetForTest();

}  // namespace netvarguard
}  // namespace playback
}  // namespace replay
