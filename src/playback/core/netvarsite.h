#pragma once

// W2-B / ENG-CLOCK-STATE. LOCATING THE NETVAR COMPONENT `EngineNetVarTable` is constructed from.
//
// The object `sub_140D3DF90` receives in `rcx` is the netGame's GLOBAL `CR15NetVarCS`, and the engine
// keeps a `CComponentHandle<CR15NetVarCS>` to it embedded in the netGame
// (`CR15NetGame::SetGlobalNetVars`, sub_1401AF8A0):
//
//   game       = *(void**)(netGame + 0x2E00)        the CncaGame the handle resolves IN -- not a space
//   generation = *(u64*)  (netGame + 0x2E38)        snapshot of *(u64*)(game + 0x2138)
//   netVarCs   = *(void**)(netGame + 0x2E40)
//   handle     = *(u32*)  (netGame + 0x2E48)        low u16 = the pool slot
//
// The generation is NOT a validity test for us. SetGlobalNetVars is the handle's only writer and runs
// twice in a component's life (InitNetVarCI fills it, RemoveReferences clears it to {null, 0xFFFF});
// the live counter moves on every actor, component or space create and destroy, so the pair differs
// within a frame or two of the fill and stays different. The engine's accessors copy the handle and
// re-resolve the copy on every call; they never write the netGame's (RE-NETVAR-CACHE-COLD-PATH S1).
//
// What holds for as long as the component lives is structural, and it is what is checked here:
// the handle is filled, the component's +0xB8 is this netGame, and the component's owning space
// (+0x80) is the netGame's current level space. The generation pair is carried for the log only.
//
// Two verdicts, because two consumers need different things (DIAG-NETVAR-TABLEGUARD S2.6):
//
//   RouteOk()  the handle names this level's live component and the pool's slot map holds a record
//              of the handle's generation for its slot. All the typed setters and getters need.
//   StoreOk()  RouteOk() and the record and its store are readable: what the store decoder needs,
//              because a bind that published a table the apply walk cannot use would refuse every
//              rowset while reporting "bound".

#include <stddef.h>
#include <stdint.h>

#include "playback/core/netvarapply.h"

namespace replay {
namespace playback {

class StateApplier;

inline constexpr uint32_t kNetGameNetVarGameOff       = 0x2E00u;  // facts-ok: NETVAR.GLOBAL_HANDLE_GAME_OFF pending; sub_140D05730 `*(a1 + 11776)`, a CncaGame*
inline constexpr uint32_t kNetGameNetVarGenerationOff = 0x2E38u;  // facts-ok: NETVAR.GLOBAL_HANDLE_GEN_OFF pending; sub_140D05730 `*(a1 + 11832)`
inline constexpr uint32_t kNetGameNetVarCsOff         = 0x2E40u;  // facts-ok: NETVAR.GLOBAL_HANDLE_CS_OFF pending; sub_140D05730 `*(a1 + 11840)`
inline constexpr uint32_t kNetGameNetVarHandleOff     = 0x2E48u;  // facts-ok: NETVAR.GLOBAL_HANDLE_SLOT_OFF pending; sub_140D05730 `*(_DWORD *)(a1 + 11848)`
inline constexpr uint32_t kGameGenerationOff          = 0x2138u;  // facts-ok: NETVAR.GAME_GENERATION_OFF pending; sub_140D05730 `*(v66 + 8504)`, CncaGame topology counter
// NETGAME.SPECTATORCAM_CS_OFF is a misnomer kept by the registry: the slot holds the level space.
inline constexpr uint32_t kNetGameLevelSpaceOff       = facts::NETGAME_SPECTATORCAM_CS_OFF;

// The store `[record+0x10]` (RE-NETVAR-STORE S0.1, read off sub_140F8BCB0 and sub_140D05640).
inline constexpr uint32_t kNetVarStoreRowsOff   = 0x08u;   // facts-ok: NETVAR.STORE_ROWS_OFF pending; sub_140D05640 `*(v5 + 8)`
inline constexpr uint32_t kNetVarStoreCountOff  = 0x38u;   // facts-ok: NETVAR.STORE_COUNT_OFF pending; sub_140D05640 `*(v5 + 56)`
inline constexpr uint32_t kNetVarStoreFlagsOff  = 0x40u;   // facts-ok: NETVAR.STORE_FLAGS_OFF pending; sub_140F8BCB0 `*(v11 + 56) & 1` = unsorted
inline constexpr uint32_t kNetVarRowStride      = 0x120u;  // facts-ok: NETVAR.STORE_ROW_STRIDE pending; sub_140D05640 `288 * v9`
inline constexpr uint32_t kNetVarRowValueOff    = 0x118u;  // facts-ok: NETVAR.STORE_ROW_VALUE_OFF pending; sub_140D05640 `288 * v9 + v7 + 280`
static_assert(kNetVarStoreFlagsOff + sizeof(uint64_t) == kNetVarStoreHeaderBytes,
              "WalkNetVarPool probes the store header through its flags qword");

// Which hop failed. Every value names the step.
enum class NetVarSiteBind : uint8_t {
    NoNetGame = 0,      // no CR15NetGame -- no session
    NoGame,             // the handle's CncaGame* is null: never filled, or cleared by RemoveReferences
    NoNetVarCs,         // the handle carries no component, or the unresolved slot 0xFFFF
    NetGameMismatch,    // the component's +0xB8 is not the netGame we walked from
    SpaceMismatch,      // the component's owning space is not the netGame's current level space
    TableNotReady,      // no longer produced; kept because earlier artifacts carry its sentence
    NoRecord,           // the pool's slot map holds no record for the handle's slot
    NoCaptureLevelId,   // located, but the file records no level id to compare rowset owners against
    Ok,
    PoolUnreadable,     // the component's pool members or the slot's map entry could not be read
    GenerationMismatch, // the slot was recycled: its generation is not the handle's
    StoreUnreadable,    // the route holds; the slot's record or its store could not be read
};

// What the route found, whether or not anything was bound.
struct NetVarSite {
    void*    netVarCs = nullptr;
    void*    game     = nullptr;   // the handle's CncaGame
    void*    space    = nullptr;   // [cs+0x80], the component's owning space
    void*    levelSpace = nullptr; // [netGame+0x2B10]
    void*    store    = nullptr;   // [record+0x10] of the handle's slot
    uint64_t cachedGeneration = 0; // informational: the fill-time snapshot and the live counter
    uint64_t liveGeneration   = 0;
    uint32_t handle   = 0;         // the SCompactPoolHandle; low u16 is the slot
    uint32_t records  = 0;         // u16 [cs+0xFA] appended update records -- the `vars=N` of the log line
    NetVarPoolWalk pool;           // how far the slot -> record -> store walk got, with what it read
    bool     route    = false;
    NetVarSiteBind bind = NetVarSiteBind::NoNetGame;

    bool RouteOk() const { return route; }
    bool StoreOk() const { return route && bind == NetVarSiteBind::Ok && store != nullptr; }
    uint16_t Slot() const { return static_cast<uint16_t>(handle & 0xFFFFu); }  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    uint16_t HandleGeneration() const { return static_cast<uint16_t>(handle >> 16); }
    // One value per distinct refusal, so a log that prints once per reason has something to compare.
    uint16_t RefusalKey() const {
        return static_cast<uint16_t>((static_cast<uint16_t>(bind) << 8) |
                                     static_cast<uint16_t>(pool.step));
    }
    // The same component instance: a level re-entry changes at least one of these.
    bool SameInstanceAs(const NetVarSite& o) const {
        return netVarCs == o.netVarCs && handle == o.handle && space == o.space && game == o.game;
    }
};

// One sentence for the log line. Never null.
const char* NetVarSiteBindText(NetVarSiteBind);
// The sentence, the step the pool walk stopped at, and every raw value the verdict was made from.
void FormatNetVarSite(const NetVarSite& site, char* line, size_t cap);
// `*(*(netGame + 0x2DA0))`, the word the setters' authority gate tests. False when unreadable.
bool ReadNetGameRoleWord(void* netGame, uint64_t& word);
inline bool RoleWordIsAuthority(uint64_t w) { return (w & 2u) != 0 || (w & 0x44u) == 0x40u; }

// The handle and its structural checks, over guarded reads only. No engine call, no write: cheap
// enough to re-run every tick, which is how a bind is re-validated.
NetVarSite LocateNetVarSite(void* netGame);

// Locates the component and, when everything holds, constructs the `EngineNetVarTable` and hands it
// to the applier with the capture's own level id -- the value `ApplyRowset` compares each rowset's
// owner field against. `captureLevelId == 0` is the manifest's "not recorded" default and is refused
// as `NoCaptureLevelId`: binding a zero would make the engine's owner check refuse everything.
//
// `line` receives the one line the module logs, in every case. The table object lives in this
// translation unit and outlives the call; a second bind replaces it.
NetVarSite BindNetVarTableOn(StateApplier& applier, void* netGame, uint64_t captureLevelId,
                            EngineNetVarTable::StoreDecodeFn decode, char* line, size_t lineCap);

// Takes the table back from the applier: the route no longer validates, so the component it was
// built from may be gone.
void UnbindNetVarTable(StateApplier& applier);

// The product's two engine reads, kept apart from the walk so everything above runs with no game.
void*     LiveNetGame();
EngineNetVarTable::StoreDecodeFn LiveStoreDecode();
// NetGame state word (`*(i32*)netGame`), -1 when unreadable. 9 is in-match.
int32_t   NetGameStateOf(void* netGame);

// Drops the bound table. Tests only; the product replaces it by binding again.
void ResetNetVarSiteForTest();

}  // namespace playback
}  // namespace replay
