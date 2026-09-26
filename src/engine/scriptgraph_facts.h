#pragma once
// DESIGN-HOST-SYNC P0. The constants the script-graph seam spends: the two graphs it reaches (the
// arena gametype's and the disc FX's), the variables and events it names, and the two module binding
// offsets beside the `get` one localpawn_facts.h already carries.
//
// Every NAME is hashed by the project's own SymbolHash and pinned against the value the script DBs
// hold, so a SymbolHash that stopped agreeing with the image fails the build. Module hashes, the
// variable id and the type hash are not names and go through the DESIGN-HOST-SYNC fact drop.

#include <stdint.h>

#include "foundation/gametype.h"

namespace replay::scriptgraph_facts {

// *(u64*)(module+56) of the arena gametype graph (script_08317e89cc3ae611) and of the disc FX graph
// (script_ae38d37e88d14ffd), the disc actor's script[1].
inline constexpr uint64_t kArenaScriptHash  = 0x08317E89CC3AE611ull;  // facts-ok: SCRIPTGRAPH.ARENA_SCRIPT_HASH not in facts/facts.tsv
inline constexpr uint64_t kDiscFxScriptHash = 0xAE38D37E88D14FFDull;  // facts-ok: SCRIPTGRAPH.DISC_FX_SCRIPT_HASH not in facts/facts.tsv

// script_set_variable @ 0x18001ED70 (arena DB): case type 0xC8C33E4828761FBC, name game_timer or id
// 0x61BAC64470FDD435 -> graph->data->game_timer = *value; return 1. The float getter answers the
// same type hash (echovr sub_1412ECCC0 and the setter wrapper sub_1412ED110 both load it from
// 0x141715300).
inline constexpr uint64_t kSymGameTimer   = replay::gametype::SymbolHash("game_timer");
inline constexpr uint64_t kGameTimerId    = 0x61BAC64470FDD435ull;  // facts-ok: SCRIPTGRAPH.GAME_TIMER_ID not in facts/facts.tsv
inline constexpr uint64_t kRealType       = 0xC8C33E4828761FBCull;  // facts-ok: SCRIPTGRAPH.REAL_TYPE not in facts/facts.tsv
static_assert(kSymGameTimer == 0xB2996AD484F1D1E8ull,
              "SymbolHash does not reproduce the arena graph's game_timer variable");

// The disc FX graph's own inputs (event_handler @ 0x18000DB80 of script_ae38d37e88d14ffd).
inline constexpr uint64_t kSymFxStateRed   = replay::gametype::SymbolHash("forceSetFXState_Red");
inline constexpr uint64_t kSymFxStateBlue  = replay::gametype::SymbolHash("forceSetFXState_Blue");
inline constexpr uint64_t kSymSpawnOrange  = replay::gametype::SymbolHash("spawn_orange_particles");
inline constexpr uint64_t kSymSpawnBlue    = replay::gametype::SymbolHash("spawn_blue_particles");
// The grab delegate's event symbol, as the FX graph's event_handler compares it. The name hashed is
// the delegate's own, prefix included: the eight script DBs that handle it (FX graph among them) name
// it `delegate_onnettouchgrab`, and SymbolHash lower-cases. Bare "OnNetTouchGrab" is 0xCDD058661A24736A.
inline constexpr uint64_t kSymOnNetTouchGrab = replay::gametype::SymbolHash("delegate_OnNetTouchGrab");
inline constexpr uint64_t kSymNetTouchInteract = replay::gametype::SymbolHash("R14NetTouchInteract");
static_assert(kSymFxStateRed == 0x52AE13902597CF66ull, "forceSetFXState_Red");
static_assert(kSymFxStateBlue == 0x9B7A2D609A74BD91ull, "forceSetFXState_Blue");
static_assert(kSymSpawnOrange == 0x4ABCB4B852FF7365ull, "spawn_orange_particles");
static_assert(kSymSpawnBlue == 0x1B632DB893CE95DBull, "spawn_blue_particles");
static_assert(kSymOnNetTouchGrab == 0xA6DB19A3FA527F88ull, "delegate_OnNetTouchGrab");
static_assert(kSymNetTouchInteract == 0xD669C4681BA186D0ull, "R14NetTouchInteract");

// EXECUTION_DATA words of the FX graph: the per-delegate sentinels of the two particle-spawn arms.
inline constexpr uint32_t kFxWordSpawnBlue   = 1u;  // facts-ok: SCRIPTGRAPH.FX_WORD_SPAWN_BLUE not in facts/facts.tsv
inline constexpr uint32_t kFxWordSpawnOrange = 6u;  // facts-ok: SCRIPTGRAPH.FX_WORD_SPAWN_ORANGE not in facts/facts.tsv

// EXECUTION_BINDINGS at module+120: `set` is +0x80 and `sendevent` +0x188. Both read out of the
// engine's own wrappers: sub_1412ED110 calls *(module+248)(graph, name, realType, &value,
// bool* delegatevariable, id) for sub_141045740 (inst -> *(inst+8), inst+24), and sub_1412ECF00
// calls *(module+512)(graph, &event) for sub_141045470 with the 56-byte event laid out below.
inline constexpr uint32_t kModuleSetOff       = 248u;  // facts-ok: SCRIPTGRAPH.MODULE_SET_OFF not in facts/facts.tsv
inline constexpr uint32_t kModuleSendEventOff = 512u;  // facts-ok: SCRIPTGRAPH.MODULE_SENDEVENT_OFF not in facts/facts.tsv

// EXECUTION_GRAPH_EVENT as sub_1412ECF00 builds it: name, actor_var (24), actorname, component,
// userdata.
inline constexpr uint32_t kEventSize         = 56u;  // facts-ok: SCRIPTGRAPH.EVENT_SIZE not in facts/facts.tsv
inline constexpr uint32_t kEventActorOff     = 8u;   // facts-ok: SCRIPTGRAPH.EVENT_ACTOR_OFF not in facts/facts.tsv
inline constexpr uint32_t kEventActorNameOff = 32u;  // facts-ok: SCRIPTGRAPH.EVENT_ACTORNAME_OFF not in facts/facts.tsv
inline constexpr uint32_t kEventComponentOff = 40u;  // facts-ok: SCRIPTGRAPH.EVENT_COMPONENT_OFF not in facts/facts.tsv
inline constexpr uint32_t kEventUserDataOff  = 48u;  // facts-ok: SCRIPTGRAPH.EVENT_USERDATA_OFF not in facts/facts.tsv

}  // namespace replay::scriptgraph_facts
