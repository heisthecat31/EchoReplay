#pragma once

// Mirrors the W1-E_R2 fact drop, which W0-2 merges into facts/facts.tsv.  Once that merge
// has happened, configure with -DLOCALPAWN_FACTS_MERGED and every value below becomes a
// static_assert against the generated registry instead of a second copy of it.
//
// THE PROLOGUE GUARDS HERE ARE 32 BYTES, NOT 20, AND THAT IS NOT A STYLE CHOICE.  Sweeping every
// function start in echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05, the first 20 bytes of
// LOCAL.ECHOUNIT_BASE_XF are shared by 32 OTHER functions, ECHOUNIT_HEAD_XF by 13 and
// ECHOUNIT_HAND_XF by 3.  They are leaf getters over one pool whose bodies differ only in a
// displacement, so a 20-byte window cannot tell them apart and would not notice a build that
// reordered them.  All three become unique at 28 bytes; 32 is the round number past that.
// docs/spikes/W1-E.md S10.6 carries the sweep and its counts.

#include <stdint.h>

#include "foundation/gametype.h"   // SymbolHash -- the net-touch system is resolved from its name
#include "generated/facts_generated.h"

namespace replay::localpawn_facts {

// ---- cited from the registry; NOT copied ---------------------------------------------------
// The component-system lookup E-4 already resolves both B.4 pools through.  One entry point,
// one guard, and the component TYPE arrives as an argument rather than as a second address.
inline constexpr uint64_t kFindComponentSystem = facts::CS_LOOKUP_BY_SYMBOL;
inline constexpr const uint8_t* kFindComponentSystemPrologue = facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE;

inline constexpr size_t kGuardLen = 32;

// LOCAL.ANCHOR_BODY_PART needs 44 bytes to become unique across the same sweep, so its guard is
// longer still.  The number is measured, not chosen.
inline constexpr size_t kGuardLenLong = 48;

// ---- pending in the W1-E_R2 fact drop ---------------------------------------------------

// The CSymbol64 each component system is registered under.  Read as the qword that sits
// immediately before the OlPrEfIx name string in .rdata -- the same derivation that produced
// CS.SYM_R14NETPHYSICS and CS.SYM_R14REMOTEPLAYER, and re-running it on those two reproduces
// the registry values exactly, which is what makes the reader trustworthy for the new three.
inline constexpr uint64_t kSymPlayerBroadcaster  = facts::LOCAL_SYM_R14PLAYERBROADCASTER;  // LOCAL.SYM_R14PLAYERBROADCASTER, qword at 0x1416D9E60 (OlPrEfIxR14PlayerBroadcaster)
inline constexpr uint64_t kSymEchoUnit           = facts::LOCAL_SYM_R15ECHOUNIT;  // LOCAL.SYM_R15ECHOUNIT, qword at 0x1416D7038 (OlPrEfIxR15EchoUnit)
// PC_PRESENT/U since W0-2's merge 4d44b9d re-derived it from guard evidence: the only binary
// evidence is a prologue seeded from this row's own address, which detects drift and not
// identity. A lookup key; it addresses nothing.
inline constexpr uint64_t kSymBody               = 0x2FC5DECB5C7B5393ull;  // LOCAL.SYM_R14BODY, qword at 0x1416D7018 (OlPrEfIxR14Body)  // facts-ok: LOCAL.SYM_R14BODY PC_PRESENT, so gen_facts emits an incomplete type with no value to name
// PC_PRESENT/U since W0-2's merge 4d44b9d re-derived it from guard evidence: the only binary
// evidence is a prologue seeded from this row's own address, which detects drift and not
// identity. A lookup key; it addresses nothing.
inline constexpr uint64_t kSymTouchAnimBlenders  = 0x7C863D4D8AC8B5E3ull;  // LOCAL.SYM_R14TOUCHANIMBLENDERS, qword at 0x141C467C0 (OlPrEfIxR14TouchAnimBlenders)  // facts-ok: LOCAL.SYM_R14TOUCHANIMBLENDERS PC_PRESENT, so gen_facts emits an incomplete type with no value to name

// CR15PlayerBroadcasterCS.  Its compact pool has the SAME layout as CR15RemotePlayerCS -- both
// dispatch the actor lookup through the +8-shifted group of CComponentSystem field offsets -- so
// count / element base sit where PLAYER_CS_COUNT_OFF and PLAYER_CS_POOL_BASE_OFF already say,
// and those two are cited rather than restated.  What is new is the KEY array beside the
// elements, and the 24-byte element itself.
inline constexpr uint32_t kBroadcasterKeysOff   = facts::LOCAL_BROADCASTER_CS_KEYS_OFF;  // LOCAL.BROADCASTER_CS_KEYS_OFF
// PC_PRESENT/U since W0-2's merge 4d44b9d re-derived it from guard evidence: the only binary
// evidence is a prologue seeded from this row's own address, which detects drift and not
// identity. It indexes memory: entity.cpp tests the element with Readable.
inline constexpr uint32_t kBroadcasterCiStride  = 24u;     // LOCAL.BROADCASTER_CI_STRIDE
inline constexpr uint32_t kBroadcasterUserIdOff = 4u;      // LOCAL.BROADCASTER_CI_USERID_OFF
inline constexpr uint32_t kActorGuidSize        = 16u;     // LOCAL.ACTOR_GUID_SIZE

// CR15EchoUnitCS and CR15BodyCS.  Both use the UNSHIFTED group, so their handle map and element
// base are at 0xC8 / 0xF8 -- the same numbers PHYS_CS_HANDLE_MAP_OFF and PHYS_CS_POOL_BASE_OFF
// carry.  They are restated here rather than cited: a shared offset is evidence about the
// address space and never about the object, and these were read out of THESE getters.
inline constexpr uint32_t kCsHandleMapOff = 0xC8u;   // LOCAL.CS_HANDLE_MAP_OFF
inline constexpr uint32_t kCsPoolBaseOff  = 0xF8u;   // LOCAL.CS_POOL_BASE_OFF
// The actor index the engine's own actor->component resolver binary-searches: a sorted array of
// 24-byte entries at cs+0x110 whose element COUNT is the qword at cs+0x118.  Read out of
// sub_140D53DA0, where the array base is `a1[34]`, the bound `a1[35]` and the stride the literal
// 24 in `v13 + 24 * v14`.  Only the count is read here, and only as a diagnostic population.
inline constexpr uint32_t kCsActorIndexOff      = 0x110u;  // facts-ok: LOCAL.CS_ACTOR_INDEX_OFF read from sub_140D53DA0, diagnostic only
inline constexpr uint32_t kCsActorIndexCountOff = 0x118u;  // facts-ok: LOCAL.CS_ACTOR_INDEX_COUNT_OFF read from sub_140D53DA0, diagnostic only
// PC_PRESENT/U since W0-2's merge 4d44b9d re-derived it from guard evidence: the only binary
// evidence is a prologue seeded from this row's own address, which detects drift and not
// identity. It indexes memory: LeafPool::Element tests the element with Readable.
inline constexpr uint32_t kEchoCiStride   = 640u;    // LOCAL.ECHOUNIT_CI_STRIDE  // facts-ok: LOCAL.ECHOUNIT_CI_STRIDE PC_PRESENT, so gen_facts emits an incomplete type with no value to name
inline constexpr uint32_t kBodyCiStride   = facts::LOCAL_BODY_CI_STRIDE;  // LOCAL.BODY_CI_STRIDE

// The three CR15BodyCS scalars, read rather than called.  Their getters are 7-instruction leaves
// that differ ONLY in this displacement and share 35 identical bytes, so calling them would cost
// three guards that cannot discriminate to buy three field reads that contain no logic.  The
// displacement is the fact; the getter is its anchor.
inline constexpr uint32_t kBodyBendBackOff = facts::LOCAL_BODY_BEND_BACK_OFF;  // LOCAL.BODY_BEND_BACK_OFF
inline constexpr uint32_t kBodyBendFwdOff  = facts::LOCAL_BODY_BEND_FWD_OFF;  // LOCAL.BODY_BEND_FWD_OFF
inline constexpr uint32_t kBodyRestPoseOff = 168u;    // LOCAL.BODY_REST_POSE_OFF

// CR15TouchAnimBlendersCS -- the trigger blend the HUMAN arm of the broadcaster reads. Its two
// floats are converted to fp16 on the way to the wire; E-4 keeps the float, which is the same
// value before the wire quantised it.
inline constexpr uint32_t kTouchAnimCiStride = facts::LOCAL_TOUCHANIM_CI_STRIDE;  // LOCAL.TOUCHANIM_CI_STRIDE
inline constexpr uint32_t kTouchAnimTrigLOff = 72u;   // LOCAL.TOUCHANIM_TRIG_L_OFF
inline constexpr uint32_t kTouchAnimTrigROff = 144u;  // LOCAL.TOUCHANIM_TRIG_R_OFF

// The anchor chain. The EchoUnit element points at the collision object it is attached to; that
// object carries the constraint id the solver is keyed on; the solver hangs off CR15Game.
inline constexpr uint32_t kEchoAttachedOff      = 216u;    // LOCAL.ECHOUNIT_ATTACHED_OFF
inline constexpr uint32_t kAttachedConstraintOff = facts::LOCAL_ATTACHED_CONSTRAINT_OFF;   // LOCAL.ATTACHED_CONSTRAINT_OFF
inline constexpr uint32_t kGameSpaceGameOff     = 8u;      // LOCAL.GAMESPACE_GAME_OFF
inline constexpr uint32_t kGameSolverOff        = facts::LOCAL_GAME_CONSTRAINT_SOLVER_OFF;  // LOCAL.GAME_CONSTRAINT_SOLVER_OFF

// The component-system table on the game space, the one CS.LOOKUP_BY_SYMBOL searches: `mov r10,[rdi+420h]`
// and the +0x450 count displacement inside CS_LOOKUP_BY_SYMBOL_PROLOGUE, 16-byte entries. Flags bit 0 is
// the engine's own unsorted mark, and its lookup sorts in place when it is set (CSREGISTER-EPOCH rule 1).
// CS.TABLE_OFF has since merged as a PC_DECISIVE V-PRIMARY row, so this is an alias for it and no
// longer a second copy of the number (ARCH-10 section 3.3). The space it is measured on is the
// CncaGameSpace at NETGAME.CS_SPACE_OFF, never the CR15Game at IDENT.NETGAME_GAMESPACE_OFF.
inline constexpr uint32_t kGameSpaceSystemTableOff = facts::CS_TABLE_OFF;
inline constexpr uint64_t kSystemTableStride       = 16u;
inline constexpr uint64_t kSystemTableUnsorted     = 1u;
// WALKING that same table names every component system the level registered, which is the only way
// to ask what an actor CLASS carries rather than what one actor carries. Entry layout is read
// straight off CS_LOOKUP_BY_SYMBOL (`*(u64*)(base + 16*i)` is the key it compares, `+8` the system
// it returns). The walk never sorts and never clears the unsorted mark: an unsorted table holds the
// same rows in a different order, and reordering it would be an engine write.
inline constexpr uint32_t kSystemTableCountOff     = facts::CS_TABLE_COUNT_OFF;
inline constexpr uint32_t kSystemTableEntryCsOff   = 8u;   // facts-ok: LOCAL.GAMESPACE_GAME_OFF the second qword of a 16-byte table entry, not that pointer

// CR15RemotePlayerCS. PLAYER_CS_COUNT_OFF (0xFC) bounds the ENABLED partition -- the CDs the
// per-CD update loop runs -- and the u16 two bytes below it is the total the system holds. A CD at
// or above the enabled bound is inert, scripts included, which is DESIGN-REMOTE-ACTOR's A-d.
inline constexpr uint32_t kRemoteCsTotalCdOff = 0xFAu;  // facts-ok: PLAYER.CS_TOTAL_CD_OFF not in facts/facts.tsv
// Vtable slot 472 on that CS: (cs, &out, SCompactPoolHandle) -> {u64 actorId, u64 spaceNameHash}.
// sub_140CCE580 resolves through exactly this before every delegate it raises, so calling it with
// the handle the plugin builds prints the actor those delegates actually land on.
inline constexpr uint32_t kRemoteCsVtblActorOfHandle = 472u;  // facts-ok: PLAYER.CS_VTBL_ACTOR_OF_HANDLE not in facts/facts.tsv
// SR15RemotePlayerCD+664 is the snapshot-history list; +16 inside it is its head. NULL means no
// player-state packet was ever received for this CD, and that is what makes UpdateBeforeScripts
// promote the plugin's raw block verbatim instead of interpolating. DESIGN-REMOTE-ACTOR 1.4's
// invariant, asserted rather than assumed.
inline constexpr uint32_t kRemoteCdHistoryOff     = 664u;  // facts-ok: PLAYER.CD_HISTORY_OFF not in facts/facts.tsv
inline constexpr uint32_t kRemoteCdHistoryHeadOff = 16u;   // facts-ok: PLAYER.CD_HISTORY_HEAD_OFF not in facts/facts.tsv

// The per-class actor pools on that same CncaGameSpace, read straight off the allocator
// `sub_1404E61E0`, which is the only producer and the only consumer of all five numbers:
//   v6 = *(u64*)(space + 1000) + 136 * classId;
//   if ( (u64)*(u16*)(v6 + 130) - *(u64*)(v6 + 48) < *(u16*)(v6 + 130) )   // i.e. free > 0
// and the class's prefab name, off `sub_1401B0D80`'s own class-name read:
//   nameId = *(u64*)( *(u64*)(*(u64*)(space + 880) + 184) + 8 * classId );
// Rows pending in the ENG-RELOAD-1 fact drop.
inline constexpr uint32_t kSpacePoolTableOff   = 1000u;  // GSPACE.POOL_TABLE_OFF  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF in facts/facts.tsv; the collision is a different object's vtable slot
inline constexpr uint32_t kPoolDescStride      = 136u;   // GSPACE.POOL_DESC_STRIDE  // facts-ok: GSPACE.POOL_DESC_STRIDE not in facts/facts.tsv
inline constexpr uint32_t kPoolDescCapOff      = 130u;   // GSPACE.POOL_DESC_CAP_OFF  // facts-ok: GSPACE.POOL_DESC_CAP_OFF not in facts/facts.tsv
inline constexpr uint32_t kPoolDescFreeOff     = 48u;    // GSPACE.POOL_DESC_FREE_OFF  // facts-ok: GSPACE.POOL_DESC_FREE_OFF not in facts/facts.tsv; 48 is a common offset and the registered ids sharing it are other objects
inline constexpr uint32_t kSpaceClassTableOff  = 880u;   // GSPACE.CLASS_TABLE_OFF  // facts-ok: GSPACE.CLASS_TABLE_OFF not in facts/facts.tsv
inline constexpr uint32_t kClassNameTableOff   = 184u;   // GSPACE.CLASS_NAME_TABLE_OFF  // facts-ok: GSPACE.CLASS_NAME_TABLE_OFF not in facts/facts.tsv
// The class id is the actor id's LOW WORD (`8 * *(u16*)(netGame + 592*rec + 392)` above), so the
// table is indexed by a u16 and a scan of it is bounded by what reads back sanely, not by a count
// the space publishes. 256 covers every class a shipped level defines with room to spare.
inline constexpr uint32_t kClassScanMax        = 256u;
inline constexpr uint32_t kPoolCapSane         = 4096u;
// The two arrays the same allocator reads to build the guid it returns, and the only way to know
// WHICH pool entry the next allocation will take before it takes it:
//   idx = *(u16*)( *(u64*)(desc + 40) + 2 * *(u64*)(desc + 48) - 2 );   // sub_1404F4420, LIFO pop
//   gen = *(u16*)( *(u64*)(desc +  8) + 2 * idx );                      // sub_1404E61E0, WORD6
// A released entry goes back on top of that stack, so on a reload the next pop is a RECYCLED entry
// and its generation is the one the engine will stamp into the actor guid.
inline constexpr uint32_t kPoolDescFreeStackOff = 40u;  // GSPACE.POOL_DESC_FREESTACK_OFF  // facts-ok: GSPACE.POOL_DESC_FREESTACK_OFF not in facts/facts.tsv
inline constexpr uint32_t kPoolDescGenTableOff  = 8u;   // GSPACE.POOL_DESC_GEN_TABLE_OFF  // facts-ok: GSPACE.POOL_DESC_GEN_TABLE_OFF not in facts/facts.tsv
// The ncaActorAlias system at space+3144 -- the registry whose exact-match binary
// search on {classId, index, generation} (sub_140215DC0) is what answers "no such actor" for a
// recycled pool entry, and therefore the one table that says whether the free's re-key
// (sub_140216620, broadcast slot +640 of sub_1404F1C60) ran for that entry.  Records are 24 bytes,
// {key:{cls:u16, idx:u16, gen:u16, pad:u16}}{spaceId:u64}{localIdx:u16}, sorted, count beside them.
// Rows pending in the ENG-RELOAD-3 fact drop.  Read-only, every one of them.
inline constexpr uint32_t kSpaceActorAliasOff = 3144u;  // GSPACE.ACTOR_ALIAS_OFF  // facts-ok: GSPACE.ACTOR_ALIAS_OFF not in facts/facts.tsv
inline constexpr uint32_t kAliasRowsOff       = 272u;   // GSPACE.ALIAS_ROWS_OFF  // facts-ok: GSPACE.ALIAS_ROWS_OFF not in facts/facts.tsv
inline constexpr uint32_t kAliasCountOff      = 280u;   // GSPACE.ALIAS_COUNT_OFF  // facts-ok: GSPACE.ALIAS_COUNT_OFF not in facts/facts.tsv
inline constexpr uint32_t kAliasRowStride     = 24u;    // GSPACE.ALIAS_ROW_STRIDE  // facts-ok: GSPACE.ALIAS_ROW_STRIDE not in facts/facts.tsv
inline constexpr uint32_t kAliasCountSane     = 1u << 20;  // a band, not a fact: no arena publishes a million actors
// The "actor guid re-keyed" virtual every component system publishes, and the one
// the free's own broadcast uses to carry a pooled record to the next generation: slot +640, called
// as (system, oldKey) at sub_1404F1C60 line 245 and landing in sub_140216620 @ 0x140216620 through
// a one-instruction per-system thunk (e.g. sub_140304FF0: `lea rdx,[rcx+0C0h]`, which is the record
// store whose rows are kAliasRowsOff/kAliasCountOff above).
// Every such thunk is the same three instructions -- `mov r8,rdx` / `lea rdx,[rcx+STORE]` /
// `jmp sub_140216620` -- and its `lea` displacement is the ONLY self-proving statement of where
// that particular system keeps its actor-record store.  The stores are NOT all at the same place
// (0xC0 for the one whose rows land on kAliasRowsOff, 0x2A8, 0x1B8, 0x1160 and more elsewhere), so
// a reader that assumes one offset for every system reads garbage from most of them -- measured:
// re-keying off such a reading killed the process at the unload (ENG-RELOAD-5 launch f1).
inline constexpr uint32_t kRekeyVtableOff     = 640u;   // GSPACE.ACTOR_REKEY_VTBL_OFF  // facts-ok: ENG-RELOAD-5
inline constexpr uint32_t kStoreRowsOff       = 80u;    // GSPACE.STORE_ROWS_OFF, relative to the store  // facts-ok: ENG-RELOAD-5
inline constexpr uint32_t kStoreCountOff      = 88u;    // GSPACE.STORE_COUNT_OFF, relative to the store  // facts-ok: ENG-RELOAD-5
static_assert(kStoreRowsOff + 0xC0u == kAliasRowsOff, "the hatched alias offsets are the 0xC0 store");
static_assert(kStoreCountOff + 0xC0u == kAliasCountOff, "the hatched alias offsets are the 0xC0 store");
// sub_1404ECD40's sticky per-(class,index) mark-for-destroy byte.  Its already-set arm skips the
// whole destroy broadcast, so this byte decides which actors queue a deferred destroy at all.
inline constexpr uint32_t kSpaceDestroyFlagTableOff = 944u;  // GSPACE.DESTROY_FLAG_TABLE_OFF  // facts-ok: GSPACE.DESTROY_FLAG_TABLE_OFF not in facts/facts.tsv
inline constexpr uint32_t kSpaceDestroyFlagStride   = 56u;   // GSPACE.DESTROY_FLAG_STRIDE  // facts-ok: GSPACE.DESTROY_FLAG_STRIDE not in facts/facts.tsv

inline constexpr uint64_t kNoConstraint         = 0xFFFFFFFFFFFFFFFFull;  // LOCAL.NO_CONSTRAINT  // facts-ok: LOBBY.JOIN_FREE_SENTINEL this file's own LOCAL.NO_CONSTRAINT; the registered ids that share this value are other fields
inline constexpr uint32_t kAnchorDefault        = 2u;      // LOCAL.ANCHOR_DEFAULT

// CTransfQ, the 48-byte transform every EchoUnit accessor writes: quaternion, then position,
// then a scale triple.  The layout is read off the composer the accessors themselves call.
inline constexpr uint32_t kTransformQuatOff = 0u;    // LOCAL.TRANSFORM_QUAT_OFF
// PC_PRESENT/U since W0-2's merge 4d44b9d re-derived it from guard evidence: the only binary
// evidence is a prologue seeded from this row's own address, which detects drift and not
// identity. NO CONSUMER: the accessors write a CTransfQ into the Transform struct and the
// fields are named there, so nothing reads this offset. A future consumer must bound it.
inline constexpr uint32_t kTransformPosOff  = 16u;   // LOCAL.TRANSFORM_POS_OFF
inline constexpr uint32_t kTransformSize    = 48u;   // LOCAL.TRANSFORM_SIZE

// The engine leaves an absent component as 0xFFFF in the low word of the SComponentID it
// writes, so a resolved handle needs no separate validity call.
inline constexpr uint16_t kNoComponent = 0xFFFFu;  // LOCAL.NO_COMPONENT  // facts-ok: LOBBY.TEAM_NONE this file's own LOCAL.NO_COMPONENT; the registered ids that share this value are other fields

// The CComponentSystem vtable slot the engine calls to turn an actor into a component id.  The
// index alone is not an identification, so the code checks the slot CONTAINS
// kFindActorComponent before calling through it -- the index says where to look and the bytes
// say what is there.  Read off the two call sites in the broadcaster update, which spell it as
// `call qword ptr [rax+1A8h]`.
inline constexpr uint32_t kFindActorComponentSlotOff = facts::LOCAL_CS_VTSLOT_FIND_COMPONENT;  // LOCAL.CS_VTSLOT_FIND_COMPONENT

// CComponentSystem::FirstActorComponent -- (cs, out, actorLo, actorHi); forwards through the object's OWN vtable+432, so it dispatches correctly for any component system
inline constexpr uint64_t kFindActorComponent = facts::LOCAL_FIND_ACTOR_COMPONENT;  // LOCAL.FIND_ACTOR_COMPONENT
// ★ THE SECOND HALF OF THE PAIR IS A GAME SPACE, NOT MORE ACTOR, AND -1 MEANS "THIS ONE".
// The searcher behind vtable+432 (sub_1402D4D10) opens with
// `if (guid[1] == -1) guid[1] = cs[5];` and then binary-searches its actor index on
// (guid[0] as three u16s, then guid[1] against the entry's second qword). PRESENT-G's census read
// that index directly: every EchoUnit row's second qword is EFE7A55EDEBAE6B6 and every CR15Body
// row's is 2FC5DECB5C7B5393 -- one constant per system, which is exactly `cs[5]`, while the first
// qword is the actor id in (index << 16 | class) form. Passing anything else as the second half
// makes the tiebreak fail on every entry, so the search misses whatever the first half says.
inline constexpr uint64_t kThisGameSpace = ~0ull;  // facts-ok: the engine's own "substitute cs+40" sentinel, read out of sub_1402D4D10
// CR15EchoUnitCS::BaseTransform -- (cs, out48, handle); element+0..47 verbatim
inline constexpr uint64_t kEchoBaseTransform = facts::LOCAL_ECHOUNIT_BASE_XF;  // LOCAL.ECHOUNIT_BASE_XF
// CR15EchoUnitCS::WorldHeadTransform -- (cs, out48, handle); element+48 composed with element+0
inline constexpr uint64_t kEchoHeadTransform = facts::LOCAL_ECHOUNIT_HEAD_XF;  // LOCAL.ECHOUNIT_HEAD_XF
// CR15EchoUnitCS::WorldHandTransform -- (cs, out48, handle, arm); element+48*(arm+2) composed with element+0
inline constexpr uint64_t kEchoHandTransform = facts::LOCAL_ECHOUNIT_HAND_XF;  // LOCAL.ECHOUNIT_HAND_XF
// CR15GameplayConstraintSolver::AnchorBodyPart -- (solver, constraintId); linear scan of the solver table at solver+944 (count at +992) for the entry whose +504 equals the id, returning that entry +492. Returns 2 from every failure path, which is the same value the builder uses when there is no constraint at all
inline constexpr uint64_t kAnchorBodyPart = facts::LOCAL_ANCHOR_BODY_PART;  // LOCAL.ANCHOR_BODY_PART
// CR15EchoUnitCS::ReferenceVelocity -- (cs, handle) -> const float*; three floats, or a static zero triple when nothing is attached
inline constexpr uint64_t kEchoReferenceVel = facts::LOCAL_ECHOUNIT_REF_VEL;  // LOCAL.ECHOUNIT_REF_VEL
// The engine's own angular velocity for a CPhBody -- (body, float out[3]) -> out.  There is no
// stored field: this least-squares omega out of the body's particle cloud (positions 0x408,
// count 0x438, velocities 0x478) and returns (0,0,0) when 0xBC bit 2 is set or the count is 0.
// It is the SAME call the netCS send path makes at 0x140D7EF6A to fill the outgoing sim-state
// row, so a local body reads what a remote peer would have received.  Pure: stack temporaries
// and the caller's out buffer only, no allocation, no globals, no locks.
inline constexpr uint64_t kPhysAngVelFromCloud = facts::PHYS_ANGVEL_FROM_CLOUD;

inline constexpr uint8_t kFindActorComponentPrologue[kGuardLen] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B,
    0x01, 0x48, 0x8B, 0xDA, 0x4C, 0x89, 0x44, 0x24,
    0x20, 0x4C, 0x8D, 0x44, 0x24, 0x20, 0x4C, 0x89,
    0x4C, 0x24, 0x28, 0x0F, 0x28, 0x44, 0x24, 0x20
};  // LOCAL.FIND_ACTOR_COMPONENT.PROLOGUE, 32 B at 0x1402D4CD0
inline constexpr uint8_t kEchoBaseTransformPrologue[kGuardLen] = {
    0x48, 0x8B, 0x81, 0xC8, 0x00, 0x00, 0x00, 0x45,
    0x0F, 0xB7, 0xC8, 0x46, 0x0F, 0xB7, 0x04, 0x88,
    0x48, 0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00, 0x4F,
    0x8D, 0x0C, 0x80, 0x49, 0xC1, 0xE1, 0x07, 0x41
};  // LOCAL.ECHOUNIT_BASE_XF.PROLOGUE, 32 B at 0x14090A570
inline constexpr uint8_t kEchoHeadTransformPrologue[kGuardLen] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0x81, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xDA,
    0x45, 0x0F, 0xB7, 0xC8, 0x46, 0x0F, 0xB7, 0x04,
    0x88, 0x4B, 0x8D, 0x14, 0x80, 0x48, 0xC1, 0xE2
};  // LOCAL.ECHOUNIT_HEAD_XF.PROLOGUE, 32 B at 0x14090EE60
inline constexpr uint8_t kEchoHandTransformPrologue[kGuardLen] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0x81, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xDA,
    0x45, 0x0F, 0xB7, 0xD0, 0x46, 0x0F, 0xB7, 0x04,
    0x90, 0x41, 0x8B, 0xC1, 0x48, 0x83, 0xC0, 0x02
};  // LOCAL.ECHOUNIT_HAND_XF.PROLOGUE, 32 B at 0x14090EE00
inline constexpr uint8_t kAnchorBodyPartPrologue[kGuardLenLong] = {
    0x4C, 0x8B, 0x89, 0xE0, 0x03, 0x00, 0x00, 0x45,
    0x33, 0xC0, 0x4D, 0x85, 0xC9, 0x74, 0x2C, 0x4C,
    0x8B, 0x91, 0xB0, 0x03, 0x00, 0x00, 0x49, 0x8B,
    0xCA, 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x01, 0x48, 0x39, 0x90, 0xF8, 0x01,
    0x00, 0x00, 0x74, 0x15, 0x41, 0xFF, 0xC0, 0x48
};  // LOCAL.ANCHOR_BODY_PART.PROLOGUE, 48 B at 0x1400FD590
inline constexpr uint8_t kEchoReferenceVelPrologue[kGuardLen] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x45, 0x33, 0xC0, 0x8B, 0xDA, 0x48,
    0x8B, 0xF9, 0xE8, 0xA9, 0xE1, 0xFF, 0xFF, 0x85,
    0xC0, 0x75, 0x12, 0x44, 0x8D, 0x40, 0x01, 0x8B
};  // LOCAL.ECHOUNIT_REF_VEL.PROLOGUE, 32 B at 0x14090C2F0
inline constexpr const uint8_t* kPhysAngVelFromCloudPrologue = facts::PHYS_ANGVEL_FROM_CLOUD_PROLOGUE;
// The guard reads kGuardLen bytes through that pointer, so a registry row carried at a
// different width would read past it silently.
static_assert(sizeof facts::PHYS_ANGVEL_FROM_CLOUD_PROLOGUE == kGuardLen,
              "PHYS.ANGVEL_FROM_CLOUD.PROLOGUE is not kGuardLen bytes wide");

// ---- S_WORLD: the durable identity of a networked body ------------------------------------
//
// The identity on the wire is a TAGGED UNION, not one field. SR15NetPhysicsSimState is 80 bytes;
// its +0x4C selects how its +0x48 is read: 0 = an SR15NetUserId (u16 slot + u16 generation),
// 1 = a component id outright, >= 2 = a u64 net id resolved through the net-actor table. A
// capture that stores the value without the tag cannot resolve it back.
inline constexpr uint32_t kSimStateStride    = 80u;    // WORLD.SIMSTATE_RECORD_STRIDE
inline constexpr uint32_t kSimStateIdOff     = 0x48u;  // WORLD.SIMSTATE_ID_OFF
inline constexpr uint32_t kSimStateKindOff   = 0x4Cu;  // WORLD.SIMSTATE_KIND_OFF
inline constexpr uint32_t kSimStateKindNetId = 2u;     // the tag value at and above which +0x48 is a u64

// The net-actor table: 16-byte entries of {u64 netId, u32 componentId}, kept sorted by net id.
// E-4 scans it LINEARLY and never touches the dirty bit -- a binary search would have to sort
// first, and a capture read that rewrites the engine table is not a read.
//
// *** kPhysCsNetActorOff and kNetActorCountOff are BOTH 0x160, on two different objects, from two
// different evidence sites. Locality is not identity, and these must never be merged. ***
inline constexpr uint32_t kPhysCsNetActorOff   = facts::WORLD_PHYSCS_NETACTOR_CS_OFF;  // WORLD.PHYSCS_NETACTOR_CS_OFF
inline constexpr uint32_t kNetActorTableOff    = facts::WORLD_NETACTOR_TABLE_OFF;  // WORLD.NETACTOR_TABLE_OFF
inline constexpr uint32_t kNetActorCountOff    = facts::WORLD_NETACTOR_TABLE_COUNT_OFF;  // WORLD.NETACTOR_TABLE_COUNT_OFF
// PC_PRESENT/U since W0-2's merge 4d44b9d re-derived it from guard evidence: the only binary
// evidence is a prologue seeded from this row's own address, which detects drift and not
// identity. It indexes memory: NetIdTable tests the whole walked span with Readable.
inline constexpr uint32_t kNetActorEntryStride = 16u;     // WORLD.NETACTOR_TABLE_ENTRY_STRIDE
inline constexpr uint32_t kNetActorCompIdOff   = 8u;      // WORLD.NETACTOR_TABLE_COMPID_OFF

// ---- the disc -----------------------------------------------------------------------------
// CR15FrisbeeCS is registered under the CSymbol64 of "R14Frisbee": the qword at 0x1416D7700, its
// name string OlPrEfIxR14Frisbee at +8 -- the {u64 hash, char name[]} layout CS.SYM_R14NETPHYSICS
// documents, read out of the same image with that row as the control. Its pool is a
// CComponentSystem's: CR15FrisbeeCS::Owner sub_1409174D0 addresses `cs[31] + 560 * map[4*h]` and
// hands the dword at CI+24 to the physics system's IsComponentValid before indexing the physics
// pool with its low word, so SR15FrisbeeCI+0x18 is the disc's net-physics SComponentID. Update
// sub_1409177E0 walks the pool densely, `i < *(u16*)(cs+244)` over `*(cs+248) + 560*i`.
inline constexpr uint64_t kSymR14Frisbee        = 0x84C65832F0B8B181ull;  // DISC.SYM_R14FRISBEE
inline constexpr uint32_t kFrisbeeCsCountOff    = 0xF4u;   // DISC.FRISBEE_CS_COUNT_OFF
inline constexpr uint32_t kFrisbeeCsPoolBaseOff = 0xF8u;   // DISC.FRISBEE_CS_POOL_BASE_OFF
// CR15FrisbeeCS::Owner sub_1409174D0 reads `a1[25] + 4*id` -- cs+0xC8, the forward handle map --
// before it multiplies by 560. Same offset as the physics system's map on a different object.
inline constexpr uint32_t kFrisbeeCsMapOff      = 0xC8u;   // DISC.FRISBEE_CS_HANDLE_MAP_OFF
inline constexpr uint32_t kFrisbeeCiStride      = 560u;    // DISC.FRISBEE_CI_STRIDE
inline constexpr uint32_t kFrisbeeCiPhysIdOff   = 0x18u;   // DISC.FRISBEE_CI_PHYS_COMPONENT_OFF

// `timesincelastheld`, the FX script's untouched gate. CR15FrisbeeCS::Update sub_140917EC0 does
// `*(float*)(CI+120) += dt` every frame and `*(dword*)(CI+120) = 0` only where
// sub_14075FD80(*(u64*)(cs+320), *(u32*)(CI+4)) answers true -- so +4 is the disc's own
// touch-interact handle and cs+320 is the touch-interact system it belongs to.
inline constexpr uint32_t kFrisbeeCiTimeSinceHeldOff = 120u;  // facts-ok: DISC.FRISBEE_CI_TIMESINCEHELD_OFF not in facts/facts.tsv
inline constexpr uint32_t kFrisbeeCiTouchIdOff       = 4u;    // facts-ok: DISC.FRISBEE_CI_TOUCH_ID_OFF not in facts/facts.tsv
inline constexpr uint32_t kFrisbeeCsTouchCsOff       = 320u;  // facts-ok: DISC.FRISBEE_CS_TOUCH_CS_OFF not in facts/facts.tsv
// sub_14075FD80's own reads on that touch-interact system, in its order: element base at cs+248,
// forward handle map at cs+200 (stride 4), CI stride 680; `+392 || +396` non-zero answers HELD, and
// bit 12 of the word at +66 is "ask the parent TouchInteract instead".
inline constexpr uint32_t kTouchCsPoolBaseOff  = 248u;  // facts-ok: DISC.TOUCH_CS_POOL_BASE_OFF not in facts/facts.tsv
inline constexpr uint32_t kTouchCsMapOff       = 200u;  // facts-ok: DISC.TOUCH_CS_HANDLE_MAP_OFF not in facts/facts.tsv
inline constexpr uint32_t kTouchCiStride       = 680u;  // facts-ok: DISC.TOUCH_CI_STRIDE not in facts/facts.tsv
inline constexpr uint32_t kTouchCiHolderAOff   = 392u;  // facts-ok: DISC.TOUCH_CI_HOLDER_A_OFF not in facts/facts.tsv
inline constexpr uint32_t kTouchCiHolderBOff   = 396u;  // facts-ok: DISC.TOUCH_CI_HOLDER_B_OFF not in facts/facts.tsv
inline constexpr uint32_t kTouchCiFlagsOff     = 66u;   // facts-ok: DISC.TOUCH_CI_FLAGS_OFF not in facts/facts.tsv
inline constexpr uint16_t kTouchCiAskParentBit = 1u << 12;  // facts-ok: DISC.TOUCH_CI_ASK_PARENT_BIT not in facts/facts.tsv

// ---- the net-touch component system: where the disc's ring edge is raised --------------------
// CR15NetTouchInteractCS::Update sub_140D70F40 is the sole raiser of `delegate_OnNetTouchGrab`,
// `delegate_OnNetTouchRelease` and `delegate_OnNetTouchResetOnIdle`, which is the family the disc
// script 38965d90a823f03f gates its `bubble` actor -- the "never touched" ring -- on. The whole
// body of that update is under `*(u32*)netGame == 9`; inside it, per component instance, it reads
// the body's holder (sub_140CEDEB0, which is the same PHYS_CI_OWNER_OFF dword the two holder gates
// read), compares it against bit 2 of CI+52, and raises the matching edge when they disagree,
// then stores the raw owner in CI+48 and rewrites the bit. The CI fields are sub_140CF2C60's
// constructor stores: +0 the actor handle, +4 the ncaPhysics id, +8 the R14NetPhysics id, +12 the
// R15TouchInteract id. The system's own table offsets are the stock CS layout.
// Derived, not transcribed: the name is what the level registers the system under, and the two
// asserts below are the positive control on the derivation -- the first reproduces a symbol the
// plugin already binds a system by, the second pins the value the disc script's own gate compares
// `event->component` against, so a miss in the table is a missing system and not a wrong hash.
inline constexpr uint64_t kNetTouchSymbol = gametype::SymbolHash("R14NetTouchInteract");
static_assert(gametype::SymbolHash("R14NetPhysics") == facts::CS_SYM_R14NETPHYSICS,
              "SymbolHash does not reproduce a symbol the plugin already resolves a system by");
static_assert(kNetTouchSymbol == 0xD669C4681BA186D0ull,
              "R14NetTouchInteract must hash to the component the disc script gates its ring on");
inline constexpr uint32_t kNetTouchCiStride       = 72u;   // facts-ok: DISC.NETTOUCH_CI_STRIDE the 72 multiplier in sub_140CEDEB0, diagnostic only
inline constexpr uint32_t kNetTouchCiPhysIdOff    = 8u;    // facts-ok: DISC.NETTOUCH_CI_PHYS_ID_OFF sub_140CF2C60 a2+8, diagnostic only
inline constexpr uint32_t kNetTouchCiTouchIdOff   = 12u;   // facts-ok: DISC.NETTOUCH_CI_TOUCH_ID_OFF sub_140CF2C60 a2+12, diagnostic only
inline constexpr uint32_t kNetTouchCiLastOwnerOff = 48u;   // facts-ok: DISC.NETTOUCH_CI_LAST_OWNER_OFF sub_140D70F40's tail store, diagnostic only
inline constexpr uint32_t kNetTouchCiFlagsOff     = 52u;   // facts-ok: DISC.NETTOUCH_CI_FLAGS_OFF sub_140D70F40 v138, diagnostic only
inline constexpr uint8_t  kNetTouchHeldLatchBit   = 0x04u; // facts-ok: DISC.NETTOUCH_HELD_LATCH_BIT the bit sub_140D70F40 raises its edges off, diagnostic only
inline constexpr uint32_t kNetGameStateOff        = 0u;    // facts-ok: DISC.NETGAME_STATE_OFF the word sub_140D70F40 demands read 9, diagnostic only
inline constexpr uint32_t kNetGameStateInMatch    = 9u;    // facts-ok: DISC.NETGAME_STATE_INMATCH NETGAME_STATE_INMATCH, diagnostic only

// The two sibling systems the net-touch system holds pointers to. sub_140CEDEB0 -- the holder
// resolve the ring edge is raised off -- takes its IF arm only when BOTH `*(cs+368)` (the
// R14NetPhysics system) validates the body handle at NETTOUCH_CI_PHYS_ID_OFF and `*(cs+376)`
// (the R15TouchInteract system) validates the touch handle at NETTOUCH_CI_TOUCH_ID_OFF; if either
// declines it falls to a per-user, per-hand scan that answers "nobody" unless the engine's own
// local attach filled it. The touch system's own table offsets are sub_14075FD80's.
inline constexpr uint32_t kNetTouchCsPhysCsOff    = 368u;  // facts-ok: DISC.NETTOUCH_CS_PHYS_CS_OFF sub_140CEDEB0 a1+368, diagnostic only
inline constexpr uint32_t kNetTouchCsTouchCsOff   = 376u;  // facts-ok: DISC.NETTOUCH_CS_TOUCH_CS_OFF sub_140CEDEB0 a1+376, diagnostic only
inline constexpr uint32_t kTouchCsCountOff        = 244u;  // facts-ok: DISC.TOUCH_CS_COUNT_OFF the stock CS live-count word, diagnostic only
inline constexpr uint32_t kTouchCiHandOff         = 420u;  // facts-ok: DISC.TOUCH_CI_HAND_OFF sub_1407596C0 stores the attached hand at CI+420, diagnostic only

// ---- the ncaScript component system, and the graph behind one of its components ---------------
// CncaScriptCS::InitializePendingScripts sub_140446220 is the whole chain in one expression:
//   ci   = *(u64*)(cs + 248) + 184 * *(u16*)(*(u64*)(cs + 272) + 24*row + 16)
//   inst = *(u64*)(ci + 24);   module = *(u64*)(inst + 8);   graph = inst + 24
// and its failure message formats the symbol at *(u64*)(module + 56), which sub_1412EBB50 opens as
// "/scripts/%016llx.%s" -- so that qword IS the script's identity at runtime. sub_141045980 leaves
// *(int*)(inst + 224) at 5 on the checkpoint path and sub_141045350's failure arm leaves it at 2.
// cs+272 is kCsActorIndexOff, so the row's third field (+16) is the component index; the CI array
// base at cs+248 is the same pool base the touch-interact system spends.
inline constexpr uint32_t kScriptCsPoolBaseOff   = 248u;  // facts-ok: SCRIPT.CS_POOL_BASE_OFF not in facts/facts.tsv
inline constexpr uint32_t kScriptCiStride        = 184u;  // facts-ok: SCRIPT.CI_STRIDE not in facts/facts.tsv
inline constexpr uint32_t kScriptCiInstanceOff   = 24u;   // facts-ok: SCRIPT.CI_INSTANCE_OFF not in facts/facts.tsv
inline constexpr uint32_t kScriptInstModuleOff   = 8u;    // facts-ok: SCRIPT.INST_MODULE_OFF not in facts/facts.tsv
inline constexpr uint32_t kScriptInstGraphOff    = 24u;   // facts-ok: SCRIPT.INST_GRAPH_OFF not in facts/facts.tsv
inline constexpr uint32_t kScriptInstStateOff    = 224u;  // facts-ok: SCRIPT.INST_STATE_OFF not in facts/facts.tsv
inline constexpr uint32_t kScriptModuleHashOff   = 56u;   // facts-ok: SCRIPT.MODULE_HASH_OFF not in facts/facts.tsv
// EXECUTION_BINDINGS is wrapped at module + 120, so `get` (+0x78 in the binding block) is
// *(fn**)(module + 240): sub_1412EC3A0 calls it as (graph, name, typeHash, out) and the compiled
// script_get_variable has exactly that signature, returning 1 only when it recognised both hashes.
inline constexpr uint32_t kScriptModuleGetOff    = 240u;  // facts-ok: SCRIPT.MODULE_GET_OFF not in facts/facts.tsv
// EXECUTION_GRAPH::data, which script_get_variable dereferences on every recognised name. A graph
// whose data pointer is null has never been initialised and must not be asked.
inline constexpr uint32_t kScriptGraphDataOff    = 104u;  // facts-ok: SCRIPT.GRAPH_DATA_OFF not in facts/facts.tsv
inline constexpr uint32_t kScriptGraphGameTimeOff = 88u;  // facts-ok: SCRIPT.GRAPH_GAMETIME_OFF not in facts/facts.tsv
// actor_var_t, 24 bytes: actor_t{u64 gamespace; u16 idx; u16 poolidx; u16 genidx; u16 pad} then
// u64 nodeid. The three u16s occupy one qword, laid out exactly as the actor id the engine's own
// handle resolve answers, so the comparison the silence turns on is a qword compare and needs no
// field-by-field mapping. (The script DBs' own type, not RE-P1C 3.1's u32 `idx` reading.)
inline constexpr uint32_t kActorVarSpaceOff      = 0u;    // facts-ok: SCRIPT.ACTORVAR_SPACE_OFF not in facts/facts.tsv
inline constexpr uint32_t kActorVarActorOff      = 8u;    // facts-ok: SCRIPT.ACTORVAR_ACTOR_OFF not in facts/facts.tsv
inline constexpr uint32_t kActorVarNodeOff       = 16u;   // facts-ok: SCRIPT.ACTORVAR_NODE_OFF not in facts/facts.tsv
inline constexpr uint32_t kActorVarSize          = 24u;   // facts-ok: SCRIPT.ACTORVAR_SIZE not in facts/facts.tsv
// The "this hand is holding nothing" sentinel sub_140CCE580 compares holdL/holdR against before it
// will raise either hand-attached-to-geo delegate. No instruction in the image writes it.
inline constexpr uint64_t kHoldSentinelVa        = 0x141FFCEA0ull;  // facts-ok: SCRIPT.HOLD_SENTINEL_VA not in facts/facts.tsv

// ---- CSoundPlayer: the listener table Wwise attenuates against ---------------------------------
// The whole read hangs off `mov rax, cs:qword_1420A0470 / mov rsi, [rax+2CF80h]`, the idiom every
// audio call site opens with (sub_1409291C0+0x14, sub_1404133E0's own copy). It is a POINTER to a
// pointer: the outer global is null until the engine root exists, which is why the census probes
// both links rather than assuming either.
inline constexpr uint64_t kAudioRootVa           = 0x1420A0470ull;  // facts-ok: AUDIO.ROOT_VA not in facts/facts.tsv
inline constexpr uint32_t kAudioSystemOff        = 0x2CF80u;  // facts-ok: AUDIO.SYSTEM_OFF not in facts/facts.tsv
// The listener table, from sub_14020E560 (SetListenerPosition): binary search over `count` records
// of 36 bytes at `base`, keyed by the u32 id at +0, whose +4 and +20 OWORDs are overwritten with
// the transform immediately before AK::SoundEngine::SetPosition is called with it. The transform
// is {quat[4]; pos[3]; 1.0f} -- sub_1404133E0 builds exactly that from vt+256 (rotation) and
// vt+240 (position), and sub_14020E560 reads the position back out of +16..+27.
inline constexpr uint32_t kAudioListenerBaseOff  = 352u;  // facts-ok: AUDIO.LISTENER_TABLE_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioListenerCountOff = 400u;  // facts-ok: AUDIO.LISTENER_COUNT_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioListenerStride   = 36u;   // facts-ok: AUDIO.LISTENER_STRIDE not in facts/facts.tsv
inline constexpr uint32_t kAudioListenerQuatOff  = 4u;    // facts-ok: AUDIO.LISTENER_QUAT_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioListenerPosOff   = 20u;   // facts-ok: AUDIO.LISTENER_POS_OFF not in facts/facts.tsv
// The id csoundplayer.cpp registers as "Spatialized Listener" and hands to SetDefaultListeners on
// the spatial-audio arm of init (sub_140209920).
inline constexpr uint32_t kAudioSpatializedOff   = 416u;  // facts-ok: AUDIO.SPATIALIZED_LISTENER_OFF not in facts/facts.tsv
// The camera/view registry: ids handed out by the counter at +424, records of 48 bytes at +432
// with `count` at +480 and the position at +8 (sub_14020E470 writes only the position, and calls
// no Wwise entry point). sub_14020E410 latches the current one at +428.
inline constexpr uint32_t kAudioActiveViewOff    = 428u;  // facts-ok: AUDIO.ACTIVE_VIEW_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioViewBaseOff      = 432u;  // facts-ok: AUDIO.VIEW_TABLE_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioViewCountOff     = 480u;  // facts-ok: AUDIO.VIEW_COUNT_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioViewStride       = 48u;   // facts-ok: AUDIO.VIEW_STRIDE not in facts/facts.tsv
inline constexpr uint32_t kAudioViewPosOff       = 8u;    // facts-ok: AUDIO.VIEW_POS_OFF not in facts/facts.tsv
// The emitter registry -- the set of game objects a sound can be posted ON. sub_140208FD0 binary-
// searches `count` 16-byte pairs {u32 id; u64 record} at sys+96 keyed by the id, and its one
// consumer sub_14020EA80 caches, in the record, the eight floats it last built an AkTransform from:
// {quat[4] @+192; pos[3] @+208; float @+220}, position `(-x, y, z)` exactly as the listener path
// flips it. The record's own id is at +184 -- sub_14020BD80 reads it back after a successful post
// -- and the flags at +188 gate the post on bit 0x20.
inline constexpr uint32_t kAudioEmitterMapOff    = 96u;   // facts-ok: AUDIO.EMITTER_MAP_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioEmitterMapCountOff = 144u; // facts-ok: AUDIO.EMITTER_MAP_COUNT_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioEmitterMapStride = 16u;   // facts-ok: AUDIO.EMITTER_MAP_STRIDE not in facts/facts.tsv
inline constexpr uint32_t kAudioEmitterMapRecOff = 8u;    // facts-ok: AUDIO.EMITTER_MAP_REC_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioEmitterIdOff     = 184u;  // facts-ok: AUDIO.EMITTER_ID_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioEmitterFlagsOff  = 188u;  // facts-ok: AUDIO.EMITTER_FLAGS_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioEmitterPosOff    = 208u;  // facts-ok: AUDIO.EMITTER_POS_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioEmitterRecSpan   = 224u;  // facts-ok: AUDIO.EMITTER_REC_SPAN not in facts/facts.tsv
// ENG-LIVE-CENSUS-2 item 2(a). The emitter's stored LISTENER SELECTOR. CSoundPlayer::SetEmitter2D
// (sub_140209890) writes 128 here and, in the same breath, calls AK::SoundEngine::SetListeners with
// the single listener game object 7; the ctor and every recycle write 0. Nothing in the engine ever
// READS it, so it is a write-through record of which listener set the emitter was bound to -- which
// makes it the cheap way to count the emitters that are routed to the 2D listener rather than to
// the camera. It is NOT an attenuation flag: whether the sound falls off with distance is decided
// by the bank's curve and by where listener 7 is, and this field is a correlate of 2D-ness, not a
// cause of it. The emitter's real flag word is kAudioEmitterFlagsOff above.
inline constexpr uint32_t kAudioEmitterListenerSelOff = 228u;  // facts-ok: AUDIO.EMITTER_LISTENER_SEL_OFF not in facts/facts.tsv
inline constexpr uint32_t kAudioEmitterListener2D     = 128u;  // facts-ok: AUDIO.EMITTER_LISTENER_2D not in facts/facts.tsv
// The sound pool CSoundPlayer::Init builds: a free STACK of 256 CSound*, so the number in use is
// capacity minus free. The engine refuses a post with "Attempting to create a sound when max sounds
// has been reached" the moment the free count hits zero (sub_140206CD0) and counts that refusal
// nowhere, so the free count IS the instrument -- how close a live match runs to the wall.
inline constexpr uint32_t kSoundPoolCapOff       = 272u;  // facts-ok: AUDIO.SOUND_POOL_CAP_OFF not in facts/facts.tsv
inline constexpr uint32_t kSoundPoolFreeOff      = 280u;  // facts-ok: AUDIO.SOUND_POOL_FREE_OFF not in facts/facts.tsv
inline constexpr uint32_t kSoundActiveCountOff   = 544u;  // facts-ok: AUDIO.SOUND_ACTIVE_COUNT_OFF not in facts/facts.tsv
inline constexpr uint32_t kEmitterPoolFreeOff    = 208u;  // facts-ok: AUDIO.EMITTER_POOL_FREE_OFF not in facts/facts.tsv
// Wwise's own voice accounting, as globals. `uMaxNumVoicesLimit` is the CONFIGURED cap that
// AK::SoundEngine::SetMaxNumVoicesLimit writes; the two dwords beside it are the live counters
// sub_1413DE4B0 subtracts and compares against it before admitting another physical voice.
// dword_142029D00 is NOT a voice count -- it is the volume threshold in dB that
// AK::SoundEngine::SetVolumeThreshold writes, below which a voice goes virtual, and it is read here
// as a float beside the limits because it is the other half of "why did that voice not sound".
inline constexpr uint64_t kWwiseMaxVoicesVa      = 0x1420F4D38ull;  // facts-ok: AUDIO.WWISE_MAX_VOICES_VA not in facts/facts.tsv
inline constexpr uint64_t kWwiseVoiceHighVa      = 0x1420F4CC0ull;  // facts-ok: AUDIO.WWISE_VOICE_HIGH_VA not in facts/facts.tsv
inline constexpr uint64_t kWwiseVoiceLowVa       = 0x1420F4CB4ull;  // facts-ok: AUDIO.WWISE_VOICE_LOW_VA not in facts/facts.tsv
inline constexpr uint64_t kWwiseVolumeThreshVa   = 0x142029D00ull;  // facts-ok: AUDIO.WWISE_VOLUME_THRESHOLD_VA not in facts/facts.tsv
// Wwise's own read-only query API, the only way to see the listener SET an emitter is attenuated
// against: the game feeds it through SetDefaultListeners and keeps no copy. Both take the sound
// engine's critical section and neither writes anything. Called by VA, so each carries the first
// 24 bytes of its prologue and the census refuses when they do not match.
inline constexpr uint64_t kAkGetListenersVa        = 0x141396C80ull;  // facts-ok: AUDIO.AK_GET_LISTENERS_VA not in facts/facts.tsv
inline constexpr uint64_t kAkGetListenerPositionVa = 0x141396B60ull;  // facts-ok: AUDIO.AK_GET_LISTENER_POSITION_VA not in facts/facts.tsv
inline constexpr uint8_t kAkGetListenersSig[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x4D, 0x8B};
inline constexpr uint8_t kAkGetListenerPositionSig[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0x48, 0x8B, 0xFA, 0x48, 0x8D, 0x0D, 0xF1, 0xCE, 0xD5, 0x00, 0xFF};
// FINAL-FIX-1. Four more of the same read-only family: what is PLAYING on a game object, which
// bank event it came from, and the attenuation curve's last point. GetPositioningInfo's out
// parameter is the 68-byte AkPositioningInfo sub_1413E9220 zeroes through +64 before filling --
// bEnableAttenuation at +20, fMaxDistance at +44, fVolDryAtMaxDist at +48, then the two aux ends.
// GetMaxRadius returns the radius over the voices currently on the object, or a negative when it
// has none. All four enter stru_1420F3A68 and write nothing but their out parameters.
inline constexpr uint64_t kAkGetMaxRadiusVa      = 0x141396DB0ull;  // facts-ok: AUDIO.AK_GET_MAX_RADIUS_VA not in facts/facts.tsv
inline constexpr uint64_t kAkGetPlayingIdsVa     = 0x141396EE0ull;  // facts-ok: AUDIO.AK_GET_PLAYING_IDS_VA not in facts/facts.tsv
inline constexpr uint64_t kAkGetEventIdVa        = 0x1413967C0ull;  // facts-ok: AUDIO.AK_GET_EVENT_ID_VA not in facts/facts.tsv
inline constexpr uint64_t kAkGetPositioningInfoVa = 0x141397030ull; // facts-ok: AUDIO.AK_GET_POSITIONING_INFO_VA not in facts/facts.tsv
inline constexpr uint8_t kAkGetMaxRadiusSig[24] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x74,
    0x24, 0x20, 0x48, 0x8D, 0x0D, 0xA3, 0xCC, 0xD5, 0x00, 0xFF, 0x15, 0xDD};
inline constexpr uint8_t kAkGetPlayingIdsSig[24] = {
    0x48, 0x8B, 0xC1, 0x48, 0x8B, 0x0D, 0x46, 0xCB, 0xD5, 0x00, 0x48, 0x85,
    0xC9, 0x74, 0x0E, 0x4D, 0x8B, 0xC8, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0xD0};
inline constexpr uint8_t kAkGetEventIdSig[24] = {
    0x8B, 0xD1, 0x48, 0x8B, 0x0D, 0x67, 0xD2, 0xD5, 0x00, 0x48, 0x85, 0xC9,
    0x0F, 0x85, 0x5E, 0x6F, 0x03, 0x00, 0x33, 0xC0, 0xC3, 0xCC, 0xCC, 0xCC};
inline constexpr uint8_t kAkGetPositioningInfoSig[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
    0x83, 0xEC, 0x20, 0x8B, 0xD9, 0x48, 0x8B, 0xF2, 0x48, 0x8D, 0x0D, 0x1D};
// FAR-SOUND-2. Three more read-only Query entry points, all keyed on a GAME OBJECT rather than a
// playing id -- which is the whole reason they are here: `GetPositioningInfo` takes a playing id
// and refused on every one of them, and these three cannot miss the same way. `GetPosition` is the
// decisive one: every emitter position this codebase has ever printed came out of the GAME's cache
// of what it last sent SetPosition, and "the game believes the emitter is on the pawn" and "Wwise
// believes the emitter is on the pawn" are two different claims.
inline constexpr uint64_t kAkGetObjPositionVa = 0x141396F10ull;  // facts-ok: AUDIO.AK_GET_OBJ_POSITION_VA not in facts/facts.tsv
inline constexpr uint64_t kAkGetAuxSendsVa    = 0x1413967E0ull;  // facts-ok: AUDIO.AK_GET_AUX_SENDS_VA not in facts/facts.tsv
inline constexpr uint64_t kAkGetDryLevelVa    = 0x141396950ull;  // facts-ok: AUDIO.AK_GET_DRY_LEVEL_VA not in facts/facts.tsv
inline constexpr uint8_t kAkGetObjPositionSig[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0x48, 0x8B, 0xFA, 0x48, 0x8D, 0x0D, 0x41, 0xCB, 0xD5, 0x00, 0xFF};
inline constexpr uint8_t kAkGetAuxSendsSig[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48,
    0x83, 0xEC, 0x40, 0x41, 0x83, 0x38, 0x00, 0x49, 0x8B, 0xF8, 0x48, 0x8B};
inline constexpr uint8_t kAkGetDryLevelSig[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
    0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x49, 0x8B, 0xF8, 0x48, 0x8D, 0x0D};
// ★ THE LINK FINAL-FIX-1 WAS MISSING. `Query::GetPositioningInfo` takes an AkUniqueID -- a BANK
// object id -- and not a playing id: sub_1413B0BC0 looks its argument up in the loaded-node index,
// so a playing id finds nothing and the call returns AK_Fail every single time, which is exactly
// what that packet saw on every voice. `QueryAudioObjectIDs` is the converter: it takes the event
// id we already read from the playing id and answers with the objects the event references.
inline constexpr uint64_t kAkQueryObjectIdsVa = 0x1413976A0ull;  // facts-ok: AUDIO.AK_QUERY_OBJECT_IDS_VA not in facts/facts.tsv
inline constexpr uint8_t kAkQueryObjectIdsSig[24] = {
    0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56,
    0x48, 0x83, 0xEC, 0x20, 0x83, 0x3A, 0x00, 0x49, 0x8B, 0xE8, 0x4C, 0x8B};
// AkObjectInfo is {AkUniqueID objID; AkUniqueID parentID; AkInt32 iDepth}. The stride is not taken
// on trust: the census prints the id it used, and a wrong stride shows up as a positioning query
// that refuses on an id no bank holds.
inline constexpr uint32_t kAkObjectInfoBytes = 12u;  // facts-ok: AUDIO.AK_OBJECTINFO_BYTES not in facts/facts.tsv
// AkTransform is three AkVectors and GetPosition copies all 36 bytes. Which of the three is the
// POSITION is not guessed here: the census prints all three and the pawn's own motion names it.
inline constexpr uint32_t kAkTransformFloats = 9u;  // facts-ok: AUDIO.AK_TRANSFORM_FLOATS not in facts/facts.tsv
// AkPositioningInfo, the fields this census reads. sub_1413E9220 zeroes +0..+64 on entry, so an
// unfilled field reads 0 rather than stack garbage.
inline constexpr uint32_t kAkPosInfoAttenuationOff = 20u;  // facts-ok: AUDIO.AK_POSINFO_ATTENUATION_OFF not in facts/facts.tsv
inline constexpr uint32_t kAkPosInfoMaxDistanceOff = 44u;  // facts-ok: AUDIO.AK_POSINFO_MAXDIST_OFF not in facts/facts.tsv
inline constexpr uint32_t kAkPosInfoVolDryOff      = 48u;  // facts-ok: AUDIO.AK_POSINFO_VOLDRY_OFF not in facts/facts.tsv
inline constexpr uint32_t kAkPosInfoVolAuxGameOff  = 52u;  // facts-ok: AUDIO.AK_POSINFO_VOLAUXGAME_OFF not in facts/facts.tsv
inline constexpr uint32_t kAkPosInfoVolAuxUserOff  = 56u;  // facts-ok: AUDIO.AK_POSINFO_VOLAUXUSER_OFF not in facts/facts.tsv
// CComponentSystem::IsComponentValid sub_1403319D0 opens with `(u16)id < *(u16*)(cs+0xF0)`: the
// number of forward-map entries, tested before the map is indexed by an id off the wire.
inline constexpr uint32_t kPhysCsIdCapacityOff  = 0xF0u;   // DISC.PHYS_CS_ID_CAPACITY_OFF
// ...and then bounds the DENSE index it read out of the map with *(u16*)(cs+0xF2), not +0xF4.
// The three fields are ordered F4 <= F2 <= F0: +0xF0 counts forward-map entries, +0xF2 counts LIVE
// components (the getter at vt+0x240 returns it, and the teardown loop iterates it), +0xF4 counts
// ENABLED ones -- Enable swaps a component up and increments it, Disable swaps down and decrements.
// So +0xF4 as a validity bound is too STRICT, and refuses a live disc parked between rounds.
inline constexpr uint32_t kPhysCsLiveCountOff   = 0xF2u;   // DISC.PHYS_CS_LIVE_COUNT_OFF

// ---- the rules block: the engine's own validated refs to the disc actor's components ----------
// CR15NetGame+0x2AA8 is a 0x4D8-byte block sub_140172AD0 fills with six 0x50-byte component-ref
// records, one per component it needs on the frisbee actor. Record layout, from the shared
// resolve/store helpers sub_14011C8A0 / sub_1400EEAC0 / sub_140128B20:
//   +0x00 resolved game space (0 = not resolved)   +0x38 freshness stamp, compared against
//   *(u64*)(space+0x2138)                          +0x40 the CComponentSystem*
//   +0x48 the SComponentID (u16 handle | u16 generation << 16), -1 as a WORD on failure.
inline constexpr uint32_t kNetGameRulesOff      = 0x2AA8u;  // DISC.NETGAME_RULES_OFF  // facts-ok: GS.RULES_OFF this file's own DISC.NETGAME_RULES_OFF; the registered ids that share this value are other fields
inline constexpr uint32_t kRulesFrisbeeRefOff   = 0x160u;   // DISC.RULES_FRISBEE_REF_OFF  // facts-ok: ENTITY.REMOTEPLAYER_CS_R15TOUCHINTERACT_OFF this file's own DISC.RULES_FRISBEE_REF_OFF; the registered ids that share this value are other fields
inline constexpr uint32_t kRulesPhysCsOff       = 0x290u;   // DISC.RULES_NETPHYSICS_CS_OFF
inline constexpr uint32_t kRulesPhysIdOff       = 0x298u;   // DISC.RULES_NETPHYSICS_ID_OFF  // facts-ok: PHYS.ZONE_BODY_COUNT_OFF this file's own DISC.RULES_NETPHYSICS_ID_OFF; the registered ids that share this value are other fields
inline constexpr uint32_t kCompRefSpaceOff      = 0x00u;    // DISC.COMPREF_SPACE_OFF
inline constexpr uint32_t kCompRefStampOff      = 0x38u;    // DISC.COMPREF_STAMP_OFF
inline constexpr uint32_t kCompRefCsOff         = 0x40u;    // DISC.COMPREF_CS_OFF
inline constexpr uint32_t kCompRefIdOff         = 0x48u;    // DISC.COMPREF_ID_OFF
inline constexpr uint32_t kGameSpaceEpochOff    = 0x2138u;  // DISC.GAMESPACE_EPOCH_OFF
// CR15FrisbeeCS::Initialize sub_140915870 caches the net-physics system it resolved out of its OWN
// space (frisbeeCs+0x80, the level one) here, and CR15FrisbeeCS::Owner indexes that instance.
inline constexpr uint32_t kFrisbeeCsPhysCsOff   = 0x158u;   // DISC.FRISBEE_CS_PHYSICS_CS_OFF  // facts-ok: ENTITY.REMOTEPLAYER_CS_R15NETPHYSICS_OFF this file's own DISC.FRISBEE_CS_PHYSICS_CS_OFF; the registered ids that share this value are other fields

// ---- the EARLY path: the level space's own actor registry --------------------------------------
// The rules block above is filled only by sub_140172AD0, which the engine runs on nevt_round_start
// / pre_launch_enter. An offline host sits in pre_match for ~98 s, so these are the offsets the
// second resolution path walks instead -- the same ones sub_140172AD0 itself walks, minus the
// rules block (DIAG-CLOCK-BIND-2 QC/C3). Re-derived here from the offline DB:
//
//   sub_140172AD0 @ 0x140172B39/0x140172B3E `mov rcx,[rsi]; mov r13,[rcx+2B10h]` on the ARENA arm.
//                  `rsi` is the rules block, and the rules constructor sub_1401471E0 @ 0x140147203
//                  `mov [rcx], rdx` stores the CR15NetGame it is built for at rules+0 -- so the
//                  space the engine searches IS *(netGame+0x2B10). [VERIFIED; this settles
//                  DIAG-HOST-ROUNDSTART Q2 item 5, which read the expression as `rules+0x2B10`.]
//   sub_1404E4F50  `a1 = *(_QWORD *)(*a3 + 24)`                -> space+0x18 is the PARENT space
//   sub_1404FF1A0  `v3 = *(__int64 **)(a1 + 880)`              -> space+0x370 is the actor registry
//                  `v5 = v3[6]`                                -> registry+0x30 is the entry count
//                  `v6 = *v3`                                  -> registry+0x00 is the entry array
//                  `*(_QWORD *)(v6 + 16 * v8)`                 -> 16-byte entries, CSymbol64 at +0
//                  `*(_WORD *)(v6 + 16 * v8 + 8)`              -> the actor index at +8
//                  return `*(_WORD *)(*(_QWORD *)(*(_QWORD *)(a1 + 880) + 352) + 2 * idx) == 0xFFFF`
//                  -> registry+0x160 holds a POINTER to the slot-word array; the word is read
//                  THROUGH it. Reading the word inline at registry+0x160+2*idx -- which is what
//                  this file used to say -- lands on the low half of that pointer and can only
//                  miss, which is the 552 s refusal LAUNCH-DISC-EARLY recorded. [VERIFIED]
// NETGAME.SPECTATORCAM_CS_OFF is a ratified misnomer: the slot holds the LEVEL CncaGameSpaceMT*,
// written once at map load by sub_14017A050. Aliased rather than re-spelled, so there is one
// authoring of the number.
inline constexpr uint32_t kNetGameLevelSpaceOff  = facts::NETGAME_SPECTATORCAM_CS_OFF;
// The sibling CncaGameSpace, and the rules block's back-pointer to the netGame it was built for.
// Both exist only so the early path can print what each candidate space held on the one launch
// that has to settle where the actor really lives.
inline constexpr uint32_t kNetGameCsSpaceOff     = facts::NETGAME_CS_SPACE_OFF;
inline constexpr uint32_t kRulesOwnerNetGameOff  = 0x00u;
inline constexpr uint32_t kGameSpaceParentOff    = 0x18u;
inline constexpr uint32_t kGameSpaceActorRegOff  = 0x370u;
inline constexpr uint32_t kActorRegBaseOff       = 0x00u;
inline constexpr uint32_t kActorRegCountOff      = 0x30u;
inline constexpr uint32_t kActorRegEntryStride   = 16u;
inline constexpr uint32_t kActorRegEntryIndexOff = 8u;
// Renamed from kActorRegLiveWordsOff so that every reader of the old, inline reading has to be
// revisited rather than silently keep it: this slot holds a POINTER to the slot-word array.
inline constexpr uint32_t kActorRegSlotWordsPtrOff = 0x160u;  // facts-ok: PHYS.CI_COUNT_OFF this file's own actor-registry slot-words pointer offset; the registered ids that share 0x160 are other fields
// The engine's own liveness test is an equality against 0xFFFF, not a "not cleared" test: the word
// holds a free-list link for a slot nobody occupies.
inline constexpr uint16_t kActorSlotLive         = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE the actor-registry free-list link word, a fourth engine field that spends 0xFFFF and has no id of its own
// The arena disc's actor name. sub_140172AD0 loads qword_1420A0C00 on the arena arm, and
// 0x1400A5740 fills that global from the static {u64 hash, "OlPrEfIx" + name} record at
// 0x1416D5B28 (`lea rax, aSolprefixgameF; and rax, 0FFFFFFFFFFFFFFF8h; mov rax, [rax]`), so the
// hash is in the image and not computed at startup. Read out of the shipped echovr.exe at file
// offset 23937320, md5 caa05ee3bca2f5df0809994e93e2cc05, with DISC.SYM_R14FRISBEE as the control.
inline constexpr uint64_t kSymGameFrisbee        = 0x53DDC0E28407FA17ull;
// Bounds on a registry we are reading rather than being handed. A space chain deeper than this or
// an entry count past it is a wrong object, not a big level: the arena's registry holds hundreds.
inline constexpr uint32_t kSpaceParentWalkMax    = 8u;
// "no id", for a reader that could not name one. Both halves are the engine's own cleared word:
// sub_140172AD0 writes 0xFFFF into ref+0x48 as a WORD on every failure arm.
inline constexpr uint32_t kClearedComponentId    = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
inline constexpr uint64_t kActorRegMaxEntries    = 65536u;
// ---- the hold ids -------------------------------------------------------------------------
// holdL and holdR are resolved against the NET GAME, not against any component, which is why
// they are the one part of the action-word group whose source is identical on both arms of the
// builder -- and why they can be sourced today while the other twenty bits cannot.
inline constexpr uint32_t kBroadcasterNetGameOff = 0xB8u;  // LOCAL.BROADCASTER_NETGAME_OFF

// ---- the netGame's own answer to "which user slot is this machine's" ----------------------
// The broadcaster pool answers "who does this machine AUTHOR", and on the -offline in-client host
// that is the human PLUS every bot -- more than one, so it cannot name the human on its own. The
// second source is CR15NetGame: AddPlayerUser (PC sub_140152D80) constructs the local player's
// SR15NetUser at netGame+0x178, which is user record 0, and gates every later read of it on the
// local-user count at netGame+0xDE (`cmp di, [r15+0DEh]` with edi = 0, at 0x140152F3D). The
// record's first word carries the user slot in its low five bits (`and ax, 1Fh` at 0x140152F31).
//
// Record 0 being the local one is the ENGINE'S CONVENTION, and the engine re-establishes it on
// every player add: AddPlayerUser (libr15 0x1253758) ends its insert chain with a literal
// UserSwap(this, 0, newRecord) and logs "idx 0"; AddBotUser (0x12547EC), AddRemoteUser (0x1256674)
// and AddReplayUser (0x125828C) stop at the +0xDC / +0xE0 / +0xE2 partition boundaries, all of
// which AddPlayerUser increments, so they land at record >= 1 once a player exists.
//
// The read still cross-checks the slot table at 0x138 -- the same table
// ENTITY.IS_LOCALLY_CONTROLLED indexes with an owner's low word -- back to record 0, but that
// cross-check cannot detect record 0 ceasing to be local: UserSwap (0x124E004) swaps the 0x138
// entries in lockstep with the records, so it would pass after such a swap.  It refuses a torn,
// uninitialised or wrong-object netGame, which is what it is kept for.
inline constexpr uint32_t kNetGameLocalUserCountOff = 0xDEu;   // LOCAL.NETGAME_LOCAL_USER_COUNT_OFF
inline constexpr uint32_t kNetGameUserRecordOff     = facts::LOCAL_NETGAME_USER_RECORD_BASE_OFF;  // LOCAL.NETGAME_USER_RECORD_BASE_OFF
inline constexpr uint32_t kNetGameUserSlotMask      = 0x1Fu;   // LOCAL.NETGAME_USER_RECORD_SLOT_MASK
inline constexpr uint32_t kNetGameSlotTableOff      = facts::LOCAL_NETGAME_SLOT_TABLE_OFF;  // LOCAL.NETGAME_SLOT_TABLE_OFF
// PC_PRESENT/U since W0-2's merge 4d44b9d re-derived it from guard evidence: the only binary
// evidence is a prologue seeded from this row's own address, which detects drift and not
// identity. It indexes memory: NetGameLocalSlot tests the back-map entry with Readable.
inline constexpr uint32_t kNetGameSlotTableStride   = 4u;      // LOCAL.NETGAME_SLOT_TABLE_STRIDE
inline constexpr uint64_t kHeldPlayer = facts::LOCAL_HELD_PLAYER;    // LOCAL.HELD_PLAYER
inline constexpr uint8_t kHeldPlayerPrologue[kGuardLen] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24,
    0xB9, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00
};  // LOCAL.HELD_PLAYER.PROLOGUE, 32 B at 0x140170CB0
// ---- visibility: was it drawn, or did it merely not move ----------------------------------
//
// CR15NetSpectatorCameraCS::EnableRemoteHeads and EnableRemoteBodies both walk the user slots and
// call CModelCS::SetVisible(handle, 1).  So visibility is a CModelCS field, and reading it is what
// separates "the simulation did nothing" from "nothing was rendered" -- two states that look the
// same in a screenshot and lead to opposite conclusions about a launch.
// RATIFIED V-PRIMARY: this IS the qword the engine loads as the ncaModel component
// symbol -- the 8 bytes at 0x1416D0D78, immediately before the "OlPrEfIxncaModel" string at
// 0x1416D0D80 (the `lea ...+7; and ~7; mov [rdx]` idiom the engine keys the model CS off). Verified by
// reading the md5-matched image and by reproducing the known kSymEchoUnit hash with the same
// derivation. The plugin's find() (CS_LOOKUP_BY_SYMBOL / FindComponentSystem) resolves the base
// CModelCS from it, so model_.cs is NON-null; the reader's blindness was never this key.
inline constexpr uint64_t kSymNcaModel = 0x38EE951A26FB816Aull;  // facts-ok: VIS.SYM_NCAMODEL ratified -- qword before OlPrEfIxncaModel @0x1416D0D80 (see block above)
// RATIFIED: the handle-map (+0xD0) and pool-base (+0x100) offsets are the canonical CS getter shape --
// `mov rax,[rcx+0D0h]; movzx edx,word[rax+r8*4]; mov rax,[rcx+100h]; imul r8,rdx,STRIDE` -- shared by
// 1034 getters in the image.
inline constexpr uint32_t kModelCsHandleMapOff = 0xD0u;   // VIS.MODEL_CS_HANDLE_MAP_OFF
inline constexpr uint32_t kModelCsPoolBaseOff  = 0x100u;  // VIS.MODEL_CS_POOL_BASE_OFF
inline constexpr uint32_t kModelCiStrideShift  = 4u;      // VIS.MODEL_CI_STRIDE_SHIFT
// REFUTED / UNRATIFIED: 16 is NOT the CModelCS CI stride. Across 1034 CS getters the
// element strides are 0x38..0x628 and NONE is 16; no `shl reg,4` CI index exists in the image. The 16
// was mis-derived from the swap-completion notifier sub_14050FED0, which strides a 16-byte TLS event
// record -- an event array, not the model CI pool. Entity::ModelVisible therefore does NOT decide
// PRESENCE from this stride: presence is the FirstActorComponent handle (kNoComponent vs not). The
// stride is kept only for the BEST-EFFORT `visible` draw-bit hint, which is diagnostic and not trusted.
inline constexpr uint32_t kModelCiStride       = 1u << kModelCiStrideShift;  // VIS.MODEL_CI_STRIDE
inline constexpr uint32_t kModelCiVisibleOff   = 4u;      // VIS.MODEL_CI_VISIBLE_OFF

// CR15NetGame::UserActor -- the actor of ANY user slot, local or remote, which is what makes the
// visibility read work for every player rather than only for the ones this machine broadcasts.
inline constexpr uint64_t kUserActor           = facts::VIS_USER_ACTOR;  // VIS.USER_ACTOR
inline constexpr uint32_t kNetGameUserStride   = facts::VIS_NETGAME_USER_STRIDE;            // VIS.NETGAME_USER_STRIDE
inline constexpr uint32_t kNetGameUserActorOff = facts::VIS_NETGAME_USER_ACTOR_OFF;            // VIS.NETGAME_USER_ACTOR_OFF
inline constexpr uint8_t kUserActorPrologue[kGuardLen] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F,
    0xB7, 0xC0, 0x48, 0x8B, 0xDA, 0x4C, 0x69, 0xC0,
    0x50, 0x02, 0x00, 0x00, 0x41, 0x0F, 0x10, 0x84,
    0x08, 0x80, 0x01, 0x00, 0x00, 0x0F, 0x11, 0x02
};  // VIS.USER_ACTOR.PROLOGUE, 32 B at 0x1401C76B0

// ---- B1: the LIVE state of a LOCALLY simulated body ----------------------------------------
//
// Entity::ReadBody reads the newest HISTORY node, and a body this machine simulates has no
// history -- on a solo capture that is EVERY body, and no PHYS_BODY sample ever existed. These
// are the slots the local integrator writes each tick, on the CPhBody reached as
// CR15NetPhysicsCS + kNetCsPhysicsCsOff -> PHYS.PRIMARY_BODY_PC(ci + kPhysCiBodyIdOff) ->
// PHYS.RESOLVE_BODY_PC, which is the chain ENTITY.UPDATE_TARGET_STATE uses on itself.
//
// Merged PC_DECISIVE / V-PRIMARY at 565a07e, each anchored at a site the engine itself uses, so
// these are aliases for the registry and no longer a second copy of it. The extent below is still
// derived rather than written down, and the pinning case in tests/entity reads facts/facts.tsv at
// run time -- the generated header is IN this build, so a mutation there moves the fixture's plant
// and the reader's expectation together and only an outside record can see it.
inline constexpr uint32_t kPhBodyPosOff       = facts::PHYS_BODY_POS_OFF;
inline constexpr uint32_t kPhBodyComOffsetOff = facts::PHYS_BODY_COM_OFFSET_OFF;
inline constexpr uint32_t kPhBodyOriOff       = facts::PHYS_BODY_ORI_OFF;
inline constexpr uint32_t kPhBodyVelOff       = facts::PHYS_BODY_VEL_OFF;
// NOT angular velocity -- the previous tick's LINEAR velocity.  Kept as an alias because the
// pinning case still has to check this displacement against facts/facts.tsv, and because the
// mutation control in ReadLocalBody reinstates the wrong read through it.
inline constexpr uint32_t kPhBodyPrevVelOff   = facts::PHYS_BODY_PREV_VEL_OFF;
inline constexpr uint32_t kPhysCiBodyIdOff    = facts::PHYS_CI_BODY_ID_OFF;

// The SR15NetPhysicsCI's own entity id, and the reason it is DERIVED here rather than filed as a
// row: it sits at displacement 0, and an x86 displacement of zero is not encoded, so there is no
// byte for an anchor guard to point at.  The record is CR15NetPhysicsCS::InitNetPhysicsCI, whose
// first statement stores the entity id it was handed at the base of the CI it is building, and
// ENTITY.UPDATE_TARGET_STATE, which reaches the same number the other way round through
// PHYS.CS_REVERSE_MAP_OFF -- the two maps this reader checks against each other.  Zero is also the
// value an unwritten CI carries, which is exactly why the gate compares it to the HANDLE and not
// to a constant: a zeroed CI passes only for handle 0, and handle 0's own reverse-map entry has
// to agree with it as well.
inline constexpr uint32_t kNetCiEntityIdOff = 0u;
inline constexpr uint32_t kNetCsPhysicsCsOff  = facts::PHYS_NETCS_PHYSICS_CS_OFF;

// ---- B1 CLOSED: the ncaPhysics component systems are a LIST, and the right one is a SEARCH ---
//
// Every value here is a merged registry row, spelled through facts:: at its use site in
// entity.cpp.  Nothing in this block is a second copy of a number.
//
// The engine's own per-frame body producer, facts::CS_NCAPHYSICS_BODY_PRODUCER, does NOT use
// facts::PHYS_NETCS_PHYSICS_CS_OFF.  It takes the space off the component system, asks the
// space's registry for the ncaPhysics symbol, and walks the LIST that comes back, taking the
// first component system whose HasComponent answers true for the entity:
//
//   0x140D49EAC  49 8B 8F 80 00 00 00   mov  rcx, [r15+80h]               ; the space
//   0x140D49EDF  48 8D 15 B9 6E 98 00   lea  rdx, "[OlPrEfIxncaPhysics"   -> 0x1416D0D9F
//   0x140D49EE6  48 83 E2 F8            and  rdx, ~7                      -> 0x1416D0D98
//   0x140D49EEA  48 8B 12               mov  rdx, [rdx]                   ; the symbol qword
//   0x140D49EED  E8 CE B6 3B FF         call 0x1401055C0                  ; registry lookup
//   0x140D49EFD  4C 39 60 30            cmp  [rax+30h], r12               ; count == 0 ?
//   0x140D49F10  49 8B 0E               mov  rcx, [r14]                   ; arr = entry + 0
//   0x140D49F13  48 8B 0C F1            mov  rcx, [rcx+rsi*8]             ; cs = arr[i]
//   0x140D49F3B  FF 90 C8 01 00 00      call [rax+1C8h]                   ; HasComponent(cs, eid)
//   0x140D49F43  75 32                  jnz  ...                          ; FIRST match wins
//   0x140D49F48  49 3B 76 30            cmp  rsi, [r14+30h]               ; i < count ?
//   0x140D49F4C  72 C2                  jb   0x140D49F10
//
// WE DO NOT CALL facts::CS_LOOKUP_LIST_BY_SYMBOL.  It is not side-effect free: when
// facts::CS_LIST_TABLE_FLAGS_OFF bit 0 is set it SORTS the array in place and clears the bit
// (`F6 81 98 04 00 00 01` / `74 29` past `call 0x1400F0580` / `83 A7 98 04 00 00 FE`).  A capture
// reader must not write engine state on a tick, and must not binary-search a registry it has not
// sorted, so entity.cpp walks the records linearly and compares the key.
//
// It is also NOT facts::CS_LOOKUP_BY_SYMBOL (0x140105510).  Those are two sibling searches over
// two different registries on the same space, and their first NINETEEN bytes are identical --
// they diverge only at index 19 (0x50 vs 0x90), which is why
// facts::CS_LOOKUP_LIST_BY_SYMBOL_PROLOGUE is exactly 20 bytes wide and not one fewer.
//
// The vtable slots the loop dispatches through are generic component-system base virtuals and
// are already in the registry under the names they were first found at: HasComponent (+0x1C8) is
// facts::STATS_EMOTE_VTSLOT and GetEntityIdForActor (+0x1A8) is facts::IDENT_EMOTE_VTSLOT_OFF.
// We reproduce HasComponent's RULE rather than dispatch, so neither is read here.  Note that
// sub_1402F7F90 -- whose bytes that rule comes from -- sits at +0x218 in all 169 vtables holding
// it, so it is a DIFFERENT function from the +0x1C8 the loop calls.
//
// The space field is facts::PHYS_PHYSICS_CS_STATE_OBJ_OFF: the same +0x80 on the same component
// -system base class that PHYS.PRIMARY_BODY_PC reads, reached here off a CR15NetPhysicsCS.  One
// displacement, one row -- the registry's name records where it was first anchored, not the only
// object that carries it.
//
// facts::CS_LIST_ARR_OFF and facts::CS_LIST_RECORD_KEY_OFF are WITHHELD by the registry: both are
// 0x00, an offset encoded as NO displacement byte, which no anchor kind can certify.  entity.cpp
// spells those two reads as +0 outright and carries the byte string in a comment, rather than
// citing a row the registry declined to make.

// ---- B1 FALLBACK: the ncaTransform chain, for an entity with no ncaPhysics instance ---------
//
// CR15NetPhysicsCS::GetEntityPosition (sub_140D51500) is the engine's OWN reader for an entity's
// position and it never touches ncaPhysics: netCS handle map -> SR15NetPhysicsCI -> the CI's
// cached ncaTransform component ref -> the ncaTransform CS at netCS + kNetCsTransformCsOff ->
// that CS's own handle map -> a 0x118-stride instance -> either the transform on the instance or
// a CNode3D the instance delegates to.  It is a POSE and nothing more: no centre of mass, no
// velocity, no particle cloud to derive omega from, which is why BodyDiag::origin exists.
//
// EVERY VALUE BELOW IS DERIVED, NOT YET FILED.  Each is a USE site inside sub_140D51500 or
// sub_1403C4470 -- the anchors are in ENG-W1E-LOCALBODY-5 section 2.3 -- but that section is [D],
// read by a delegated agent and not re-read against the pinned exe at the time this landed.  The
// drop is OWED and the reader is honest about it: a row filed on an unverified byte string is
// worse than no row.  When the anchors are re-read the drop becomes the W1-E_xform fact drop
// and these become aliases for it, the way the B1 block above became aliases at 565a07e.
inline constexpr uint32_t kNetCsTransformCsOff = 0x178u;  // XFORM.NETCS_XFORM_CS_OFF -- facts-ok: LOCAL.NETGAME_USER_RECORD_BASE_OFF 0x178 V-PRIMARY carries the same NUMBER on a different object (a CR15NetGame, not a CR15NetPhysicsCS); this one is read at 0x140D5154D
inline constexpr uint32_t kNetCiXformRefOff    = 0x10u;   // XFORM.CI_XFORM_REF_OFF -- facts-ok: LOCAL.TRANSFORM_POS_OFF 16 V-PRIMARY is that number on a CTransfQ; this is the SR15NetPhysicsCI field InitNetPhysicsCI writes at 0x140CF5A5E
// The ncaTransform CS is the UNSHIFTED CComponentSystem shape.  The three displacements are
// CITED from the physics CS rows rather than restated: they are the same NUMBER and an alias
// says so once.  The claim that they are the right number FOR THIS OBJECT rests on the
// GetWorldTransform anchors (0x1403C4476 map, 0x1403C4490 pool base), not on the citation --
// a shared offset is evidence about the address space and never about the object.
inline constexpr uint32_t kXformCsHandleMapOff = facts::PHYS_CS_HANDLE_MAP_OFF;
inline constexpr uint32_t kXformCsPoolBaseOff  = facts::PHYS_CS_POOL_BASE_OFF;
inline constexpr uint32_t kXformCsCountOff     = facts::PHYS_CS_COUNT_OFF;
inline constexpr uint32_t kXformCsInstStride   = 0x118u;  // XFORM.CS_INST_STRIDE -- facts-ok: XFORM.CS_INST_STRIDE 0x118 the imul immediate at 0x1403C4489; no registry row carries it yet
inline constexpr uint32_t kXformInstNodeOff    = 0x08u;   // XFORM.INST_NODE_OFF
inline constexpr uint32_t kXformInstRotOff     = 0xE8u;   // XFORM.INST_ROT_OFF -- facts-ok: XFORM.INST_ROT_OFF 0xE8 the movups displacement at 0x1403C44A6
inline constexpr uint32_t kXformInstPosOff     = 0xF8u;   // XFORM.INST_POS_OFF, x and y -- facts-ok: PHYS.CS_POOL_BASE_OFF 0xF8 V-PRIMARY is that number on a component SYSTEM; this is a field of an INSTANCE, read at 0x1403C44B0
inline constexpr uint32_t kXformInstPosZOff    = 0x100u;  // XFORM.INST_POSZ_OFF, z, read alone -- facts-ok: VIS.MODEL_CS_POOL_BASE_OFF 0x100 V-PRIMARY is that number on a CModelCS; this is a field of an ncaTransform instance, read at 0x1403C44A0
// Derived from the map load itself (`46 0F B7 04 88` -- scale 4, WORD at displacement 0), which
// is the same derivation kPhysicsCsMapEntryBytes records for the physics CS.
inline constexpr uint32_t kXformMapEntryBytes  = 4u;
// One span probe for every field on the instance.  Derived, so a field that moves moves it.
inline constexpr uint32_t kXformInstReadExtent = kXformInstPosZOff + sizeof(float);

// The CNode3D path (sub_140628680), taken when the instance carries a node pointer.
inline constexpr uint32_t kNode3dXformOwnerOff = 0x70u;  // XFORM.NODE3D_XFORM_OWNER_OFF
inline constexpr uint32_t kNode3dXformIdxOff   = 0x78u;  // XFORM.NODE3D_XFORM_IDX_OFF -- facts-ok: PHYS.PHYSICS_CI_STRIDE 0x78 V-PRIMARY is that number as a STRIDE; this is a u16 field of a CNode3D
inline constexpr uint32_t kNode3dParentOff     = 0x60u;  // XFORM.NODE3D_PARENT_OFF
inline constexpr uint32_t kNode3dClassIdOff    = 0x20u;  // XFORM.NODE3D_CLASSID_OFF
inline constexpr uint32_t kXformBlockBaseOff   = 0x28u;  // XFORM.BLOCK_BASE_OFF -- facts-ok: XFORM.BLOCK_BASE_OFF 0x28 the entry-block pointer sub_140628680 loads off the node owner
inline constexpr uint32_t kXformEntryStride    = 48u;    // XFORM.ENTRY_STRIDE -- facts-ok: LOCAL.TRANSFORM_SIZE 48 V-PRIMARY is the same 48-byte record; this is its stride in the owner block
inline constexpr uint32_t kXformEntryQuatOff   = 0u;     // derived: displacement 0, no byte to anchor
inline constexpr uint32_t kXformEntryPosOff    = 16u;    // XFORM.ENTRY_POS_OFF -- facts-ok: LOCAL.TRANSFORM_POS_OFF 16 V-PRIMARY is the same field of the same 48-byte record
inline constexpr uint32_t kXformEntryScaleOff  = 0x1Cu;  // XFORM.ENTRY_SCALE_OFF -- facts-ok: XFORM.ENTRY_SCALE_OFF 0x1C the three scalar scale slots CR15Node3D's composer writes at 0x1401DCD65/74/83, and the three the identity path fills with 1.0f. The record is PACKED (quat 0x00, translate 0x10, scale 0x1C, 8 bytes of pad), NOT three 16-byte quads -- the composer's movups straddle the field boundaries
inline constexpr uint32_t kNode3dReadExtent    = kNode3dXformIdxOff + sizeof(uint16_t);

// OURS, not the engine's: the engine terminates the parent walk on the class-id test and applies
// no depth bound at all.  A reader walking engine pointers on the owner's game thread needs one,
// because a corrupt or cyclic parent link would otherwise spin inside a capture tick.  32 is far
// past any scene depth this game builds and it is a REFUSAL, never a truncation -- a partially
// composed pose is a wrong pose.
inline constexpr uint32_t kNodeChainDepthMax = 32u;

// The class id the parent walk tests node+kNode3dClassIdOff against.  Derived exactly the way
// CS.SYM_R14NETPHYSICS and the three LOCAL.SYM_* rows above are -- the qword immediately before
// the OlPrEfIx name string in .rdata -- and the derivation is CONTROLLED: re-running it on
// "OlPrEfIxR15EchoUnit" reproduces kSymEchoUnit's 0xEFE7A55EDEBAE6B6 exactly.  The 8 bytes sit
// at 0x1416CFDE8, ahead of "OlPrEfIxCNode3D" at 0x1416CFDF0, and the value appears NOWHERE else
// in the image -- not once as an immediate -- so the engine's own test loads it from there.
inline constexpr uint64_t kNode3dClassId = 0x2FD481905A71048Eull;  // XFORM.NODE3D_CLASSID

// CPhysicsCS::PrimaryBody does NOT apply its live-count bound unconditionally, and a reader that
// says it does refuses every slot the engine accepts.  L2-RUN-10 refused 1476 of 1476 body reads
// at that bound.  The five instructions, read back out of echovr.exe md5
// caa05ee3bca2f5df0809994e93e2cc05:
//
//   0x14044CC57  42 0F B7 0C 88        movzx ecx, word ptr [rax+r9*4]
//   0x14044CC5C  48 8B 82 80 00 00 00  mov   rax, [rdx+80h]
//   0x14044CC63  F6 80 40 0C 00 00 01  test  byte ptr [rax+0C40h], 1
//   0x14044CC6A  75 09                 jnz   loc_14044CC75    -- past the bound, into the imul
//   0x14044CC6C  66 3B 8A F4 00 00 00  cmp   cx, [rdx+0F4h]
//   0x14044CC73  73 2C                 jnb   loc_14044CCA1    -- the -1 sentinel
//
// so the engine's condition is
// `(*(BYTE*)(*(QWORD*)(this+80h) + 0C40h) & 1) != 0 || slot < *(WORD*)(this+0F4h)`.
inline constexpr uint32_t kPhysicsCsStateObjOff   = facts::PHYS_PHYSICS_CS_STATE_OBJ_OFF;
inline constexpr uint32_t kPhysicsCsStateFlagsOff = facts::PHYS_PHYSICS_CS_STATE_FLAGS_OFF;

// The imm8 of that `test`, and the only bit of the flag word this walk reads.  Derived rather
// than filed: guard_anchors decodes an imm8 as the raw byte, and 0x01 is the least
// discriminating immediate in the image -- a row on it would be a value-rarity accident rather
// than evidence.  The byte string above is the record.
inline constexpr uint8_t kPhysicsCsSlotBoundBypassBit = 0x1u;

// The handle map's entry stride, and the half of the entry PrimaryBody takes.  The movzx above
// scales by 4 and loads a WORD at displacement 0, so the entry is four bytes and the slot is its
// LOW half; the other half is never read here.  Derived from that one instruction, which is what
// PHYS.CS_HANDLE_MAP_OFF's own note already records, so this is not a second opinion.
inline constexpr uint32_t kPhysicsCsMapEntryBytes  = 4u;
inline constexpr uint32_t kPhysicsCsMapSlotHalfOff = 0u;

// The widest of the four reads, so ONE span probe covers all of them. Derived from the offsets
// above rather than written down, so a field that moves moves the extent with it.
inline constexpr uint32_t kPhBodyReadExtent = kPhBodyOriOff + 16u;

// ---- the chain, REPRODUCED rather than called ----------------------------------------------
//
// facts/facts.tsv says of PHYS.PRIMARY_BODY_PC: "NOT CALLED by W3-A: it returns a 24-byte
// aggregate, so which register carries `this` versus the hidden return slot is an ABI question
// the disassembly does not settle". entity.cpp used to call it anyway. It now walks the same
// nine instructions -- `map=[rcx+0C8h]; slot=[map+4*r8w]; slot<[rcx+0F4h];
// ci=[rcx+0F8h]+78h*slot; [ci+38h]!=0; out=24 B from ci+40h` -- and the 19 of
// PHYS.RESOLVE_BODY_PC after it, so no convention is guessed on the owner's machine. Every
// displacement in that walk is a merged registry row spelled through facts:: at the use site;
// the three below are the exception and the reason is on each.
//
// sizeof(CPhTarget): movups [rcx+40h] plus movsd [rcx+50h] at 0x14044CC87/91 -- 16 + 8.
inline constexpr uint32_t kPhTargetBytes = 24u;
inline constexpr uint32_t kPhysicsCiReadExtent = facts::PHYS_PHYSICS_CI_TARGET_OFF + kPhTargetBytes;

// The two validity tests CPhTarget::ResolveBody applies to the CPhBody it reaches. Dropping them
// would let a recycled slot answer with a stale transform -- a silent wrong com, which is worse
// than a refusal. PHYS.ID_INDEX_OFF and PHYS.TARGET_TAG_OFF each gained a USE-site anchor at
// 2551af7 and are PC_DECISIVE, so they are cited below rather than restated; PHYS.ID_DEAD is
// still withheld, for the reason on its own line. The sites they were decoded from:
//   0x14063886B  cmp dword ptr [rax+8], 0FFFFFFFFh  -- CPhBody dead marker, in ResolveBody
//   0x140638F13  cmp dword ptr [rax+8], 0FFFFFFFFh  -- the same header on a CPhZone, ResolveZone
//   0x140638871  mov ecx, [rcx+10h]                 -- the CPhTarget's generation tag
//   0x140638874  cmp [rax+10h], ecx                 -- the CPhBody's generation vs that tag
// The pinning case in tests/entity reads the filed rows out of facts/facts.tsv at run time and
// compares all three, so these are checked aliases rather than a fourth opinion. The CPhBody
// side of the tag comparison is facts::PHYS_ID_TAG_OFF, which IS generated, and the two are
// deliberately not merged: same number, two objects.
inline constexpr uint32_t kPhIdIndexOff   = facts::PHYS_ID_INDEX_OFF;
inline constexpr uint32_t kPhTargetTagOff = facts::PHYS_TARGET_TAG_OFF;
// PHYS.ID_DEAD stays withheld and there is nothing to cite: 0xFFFFFFFF is nowhere encoded as a
// 32-bit field, the test being `cmp dword ptr [rax+8], 0FFFFFFFFh` = 83 78 08 FF, opcode 0x83,
// whose operand is a ONE-byte immediate the CPU sign-extends. The marker is every bit of the
// index field set, which is what PHYS.TARGET_INDEX_OFF's own note records the producer doing --
// "the not-found arm at 0x14044CCA4 writes -1 over the same eight bytes" -- so the dead value is
// the width's maximum rather than a number of ours.
inline constexpr uint32_t kPhIdDead       = UINT32_MAX;
}  // namespace replay::localpawn_facts
