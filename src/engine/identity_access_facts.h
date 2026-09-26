#pragma once

// Generated from the W2-C fact drop by the script that decoded every field out of
// the instruction bytes in the W2-C anchor drop.  DO NOT EDIT BY HAND: a
// number typed in here would be a transcription, which is the one thing the design
// forbids outright.  Once W0-2 has merged the drop, configure with
// -DIDENTITY_FACTS_MERGED and every value below becomes a static_assert against
// generated/facts_generated.h rather than a second copy of it.

#include <stdint.h>

#ifdef IDENTITY_FACTS_MERGED
#include "generated/facts_generated.h"
#define IDENT_FACT(name, value) \
    inline constexpr auto k##name = (value); \
    static_assert(k##name == ::facts::IDENT_##name, \
                  "identity_access_facts.h disagrees with the merged registry for " #name)
#else
#define IDENT_FACT(name, value) inline constexpr auto k##name = (value)
#endif

namespace replay::identity_facts {

IDENT_FACT(SLOT_TABLE_OFF, uint32_t(0x138u));  // facts-ok: IDENT.SLOT_TABLE_OFF in facts/facts.tsv; base CR15NetGame
IDENT_FACT(GEN_TABLE_OFF, uint32_t(0x13Au));  // facts-ok: IDENT.GEN_TABLE_OFF in facts/facts.tsv; base CR15NetGame
IDENT_FACT(PROFILE_JSON_OFF, uint32_t(0x3C0u));  // facts-ok: IDENT.PROFILE_JSON_OFF in facts/facts.tsv; base CR15NetGame
IDENT_FACT(LOADOUT_TABLE_OFF, uint32_t(0x51420u));  // facts-ok: IDENT.LOADOUT_TABLE_OFF in facts/facts.tsv; base CR15NetGame
IDENT_FACT(USER_SLOT_MASK, uint32_t(0x1Fu));  // facts-ok: IDENT.USER_SLOT_MASK in facts/facts.tsv; base the SR15NetUser packed word at CR15NetGame+0x178
IDENT_FACT(LOADOUT_ENTRY_SHIFT, uint32_t(0x6u));  // facts-ok: IDENT.LOADOUT_ENTRY_SHIFT in facts/facts.tsv; base -
IDENT_FACT(LOADOUT_COUNT_OFF, uint32_t(0x30u));  // facts-ok: IDENT.LOADOUT_COUNT_OFF in facts/facts.tsv; base the 64-byte SR15NetLoadout header
IDENT_FACT(LOADOUT_NUMBER_OFF, uint32_t(0x38u));  // facts-ok: IDENT.LOADOUT_NUMBER_OFF in facts/facts.tsv; base the 64-byte SR15NetLoadout header
IDENT_FACT(LOADOUT_NUMBER_BODY_OFF, uint32_t(0x3Au));  // facts-ok: IDENT.LOADOUT_NUMBER_BODY_OFF in facts/facts.tsv; base the 64-byte SR15NetLoadout header
IDENT_FACT(INSTANCE_ITEM_COUNT_OFF, uint32_t(0x38u));  // facts-ok: IDENT.INSTANCE_ITEM_COUNT_OFF in facts/facts.tsv; base the 64-byte loadout instance
IDENT_FACT(USER_ACTOR_GUID_OFF, uint32_t(0x180u));  // facts-ok: IDENT.USER_ACTOR_GUID_OFF in facts/facts.tsv; base CR15NetGame
IDENT_FACT(ENTRANT_COUNT_OFF, uint32_t(0xDCu));  // facts-ok: IDENT.ENTRANT_COUNT_OFF in facts/facts.tsv; base CR15NetGame
IDENT_FACT(NONREPLAY_COUNT_OFF, uint32_t(0xE2u));  // facts-ok: IDENT.NONREPLAY_COUNT_OFF in facts/facts.tsv; base CR15NetGame
IDENT_FACT(USER_TEAM_SHIFT, uint32_t(0xAu));  // facts-ok: IDENT.USER_TEAM_SHIFT in facts/facts.tsv; base the SR15NetUser packed word at CR15NetGame+0x178
IDENT_FACT(USER_TEAM_MASK, uint32_t(0x7u));  // facts-ok: IDENT.USER_TEAM_MASK in facts/facts.tsv; base the SR15NetUser packed word at CR15NetGame+0x178
IDENT_FACT(USER_TEAMSLOT_SHIFT, uint32_t(0x5u));  // facts-ok: IDENT.USER_TEAMSLOT_SHIFT in facts/facts.tsv; base the SR15NetUser packed word at CR15NetGame+0x178
IDENT_FACT(USER_TEAMSLOT_MASK, uint32_t(0x1Fu));  // facts-ok: IDENT.USER_TEAMSLOT_MASK in facts/facts.tsv; base the SR15NetUser packed word at CR15NetGame+0x178
IDENT_FACT(ALIGN_TEAM_LIMIT, uint32_t(0x2u));  // facts-ok: IDENT.ALIGN_TEAM_LIMIT in facts/facts.tsv; base -
IDENT_FACT(CUST_INDEX_TABLE_OFF, uint32_t(0xD0u));  // facts-ok: IDENT.CUST_INDEX_TABLE_OFF in facts/facts.tsv; base CR15NetCustomizationCS
IDENT_FACT(CUST_RECORD_STRIDE, uint32_t(0x1C0u));  // facts-ok: IDENT.CUST_RECORD_STRIDE in facts/facts.tsv; base -
IDENT_FACT(CUST_RECORDS_OFF, uint32_t(0x100u));  // facts-ok: IDENT.CUST_RECORDS_OFF in facts/facts.tsv; base CR15NetCustomizationCS
IDENT_FACT(CUST_OVERRIDE_ALIGN_OFF, uint32_t(0x42u));  // facts-ok: IDENT.CUST_OVERRIDE_ALIGN_OFF in facts/facts.tsv; base the 448-byte customisation record
IDENT_FACT(CUST_SLOT_COUNT_OFF, uint32_t(0x1A0u));  // facts-ok: IDENT.CUST_SLOT_COUNT_OFF in facts/facts.tsv; base the 448-byte customisation record
IDENT_FACT(CUST_SLOTS_OFF, uint32_t(0x170u));  // facts-ok: IDENT.CUST_SLOTS_OFF in facts/facts.tsv; base the 448-byte customisation record
IDENT_FACT(CUST_SLOT_FLAGS_OFF, uint32_t(0x8u));  // facts-ok: IDENT.CUST_SLOT_FLAGS_OFF in facts/facts.tsv; base the 48-byte customisation slot entry
IDENT_FACT(CUST_SLOT_APPLIED_BIT, uint32_t(0x1u));  // facts-ok: IDENT.CUST_SLOT_APPLIED_BIT in facts/facts.tsv; base IDENT.CUST_SLOT_FLAGS_OFF
IDENT_FACT(CUST_SLOT_ALIGNREQ_OFF, uint32_t(0xCu));  // facts-ok: IDENT.CUST_SLOT_ALIGNREQ_OFF in facts/facts.tsv; base the 48-byte customisation slot entry
IDENT_FACT(CUST_SLOT_ITEM_OFF, uint32_t(0x10u));  // facts-ok: IDENT.CUST_SLOT_ITEM_OFF in facts/facts.tsv; base the 48-byte customisation slot entry
IDENT_FACT(CUST_SLOT_PENDING_OFF, uint32_t(0x20u));  // facts-ok: IDENT.CUST_SLOT_PENDING_OFF not in facts/facts.tsv; base the 48-byte customisation slot entry
IDENT_FACT(NETGAME_GAMESPACE_OFF, uint32_t(0x8u));  // facts-ok: IDENT.NETGAME_GAMESPACE_OFF in facts/facts.tsv; base CR15NetGame -> CR15Game (the back-pointer to GAME.SINGLETON_VA), NOT a CncaGameSpace and NOT the object CS.TABLE_OFF is measured on -- that is NETGAME.CS_SPACE_OFF (+0x2B08).  The id is a MISNOMER kept for continuity
IDENT_FACT(CUST_NETGAME_OFF, uint32_t(0xB8u));  // facts-ok: IDENT.CUST_NETGAME_OFF in facts/facts.tsv; base CR15NetCustomizationCS

// AUD-W2-P1-45 sec B: the CDeferredMethodQueue IDENT.APPLY_LOADOUT_ITEMS decides inline-vs-deferred on.
IDENT_FACT(APPLY_QUEUE_SUBSYS_OFF, uint32_t(0x80u));  // facts-ok: IDENT.APPLY_QUEUE_SUBSYS_OFF not in facts/facts.tsv; base the actor component system; mov rax,[r15+80h]@0x140155072
IDENT_FACT(DEFERRED_QUEUE_INLINE_FLAG_OFF, uint32_t(0x368u));  // facts-ok: DEFERREDQUEUE.INLINE_FLAG_OFF not in facts/facts.tsv; base the CDeferredMethodQueue; cmp dword [rax+368h],0@0x140155080; nonzero -> apply runs inline
IDENT_FACT(DEFERRED_QUEUE_HEAD_OFF, uint32_t(0x358u));  // facts-ok: DEFERREDQUEUE.HEAD_OFF not in facts/facts.tsv; base the CDeferredMethodQueue; lock xadd [rax+358h],r15@0x1401550ae byte head
IDENT_FACT(DEFERRED_QUEUE_CAP_OFF, uint32_t(0x338u));  // facts-ok: DEFERREDQUEUE.CAP_OFF not in facts/facts.tsv; base the CDeferredMethodQueue; add rax,338h@0x1401550b7 byte capacity
IDENT_FACT(DEFERRED_QUEUE_ENTRY_STRIDE, uint32_t(0x38u));  // facts-ok: DEFERREDQUEUE.ENTRY_STRIDE not in facts/facts.tsv; base -; 56 bytes reserved per parked request (xadd 0x38 @0x1401550ae)

// AUD-W2-P1-45 sec P1-42 F2: the guid canonicaliser both engine build sites run before building the ref.
IDENT_FACT(ACTOR_GUID_CANON, uint64_t(0x140363610ull));  // facts-ok: ENGINE.ACTOR_GUID_CANON_VA merged in facts/facts.tsv (sub_140363610); canonicalises the guid its arg points at, writing back through it

IDENT_FACT(OVERRIDE_ALIGNMENT, uint64_t(0x140D202C0ull));  // facts-ok: IDENT.OVERRIDE_ALIGNMENT in facts/facts.tsv
IDENT_FACT(APPLY_ALIGNMENT_CHANGE, uint64_t(0x140CAA5F0ull));  // facts-ok: IDENT.APPLY_ALIGNMENT_CHANGE in facts/facts.tsv
IDENT_FACT(APPLY_CURRENT_LOADOUT, uint64_t(0x140CAB3B0ull));  // facts-ok: IDENT.APPLY_CURRENT_LOADOUT in facts/facts.tsv
IDENT_FACT(APPLY_CHASSIS, uint64_t(0x140CABF10ull));  // facts-ok: IDENT.APPLY_CHASSIS not in facts/facts.tsv; the productive per-item apply APPLY_CURRENT_LOADOUT calls per worn-record entry; a chassis-type item reaches sub_140CC4B50 -> BeginModelSwap
IDENT_FACT(APPLY_ALIGNMENT_ALL_USERS, uint64_t(0x140155900ull));  // facts-ok: IDENT.APPLY_ALIGNMENT_ALL_USERS in facts/facts.tsv
IDENT_FACT(NETGAME_SPAWN_NOTIFY_VA, uint64_t(0x1401CAC00ull));  // facts-ok: IDENT.NETGAME_SPAWN_NOTIFY_VA not in facts/facts.tsv; the CR15NetGame spawn handler sub_1401CAC00(ng,index) whose remote arm installs the ncaModel/swapper CI (DIAG-SPAWN-MODEL-CI Lever B)
IDENT_FACT(USER_DISPLAY_NAME, uint64_t(0x1401C98C0ull));  // facts-ok: IDENT.USER_DISPLAY_NAME in facts/facts.tsv
IDENT_FACT(LOADOUT_ASSIGN, uint64_t(0x14014F230ull));  // facts-ok: IDENT.LOADOUT_ASSIGN in facts/facts.tsv
IDENT_FACT(ACTOR_COMPONENT_REF, uint64_t(0x1401416F0ull));  // facts-ok: IDENT.ACTOR_COMPONENT_REF in facts/facts.tsv
IDENT_FACT(JSON_IS_EMPTY, uint64_t(0x1405F1360ull));  // facts-ok: IDENT.JSON_IS_EMPTY in facts/facts.tsv
IDENT_FACT(JSON_SET_STRING, uint64_t(0x1405FDB30ull));  // facts-ok: IDENT.JSON_SET_STRING in facts/facts.tsv
IDENT_FACT(JSON_SET_INT, uint64_t(0x1405FD4F0ull));  // facts-ok: IDENT.JSON_SET_INT in facts/facts.tsv
IDENT_FACT(JSON_GET_STRING, uint64_t(0x1405FE290ull));  // facts-ok: IDENT.JSON_GET_STRING in facts/facts.tsv
IDENT_FACT(JSON_CACHED_OFF, uint64_t(0x8ull));      // facts-ok: IDENT.JSON_CACHED_OFF in facts/facts.tsv
IDENT_FACT(JSON_DOC_CTOR, uint64_t(0x1401F4FB0ull));  // facts-ok: IDENT.JSON_DOC_CTOR not in facts/facts.tsv
IDENT_FACT(JSON_DOC_SIZE, uint64_t(24ull));           // facts-ok: IDENT.JSON_DOC_SIZE not in facts/facts.tsv
IDENT_FACT(JSON_PARSE_TEXT, uint64_t(0x1405F0BD0ull));  // facts-ok: IDENT.JSON_PARSE_TEXT not in facts/facts.tsv
IDENT_FACT(JSON_GET_INT, uint64_t(0x1405F3080ull));   // facts-ok: IDENT.JSON_GET_INT not in facts/facts.tsv

// Prologues, checked once before the first call to each entry point above.
inline constexpr uint8_t kOVERRIDE_ALIGNMENTPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F };
inline constexpr uint8_t kAPPLY_ALIGNMENT_CHANGEPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xF9, 0x8B, 0xDA, 0xFF, 0x90 };
inline constexpr uint8_t kAPPLY_CURRENT_LOADOUTPrologue[20] = { 0x4C, 0x8B, 0xDC, 0x53, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x99, 0xB8, 0x00, 0x00 };
inline constexpr uint8_t kAPPLY_CHASSISPrologue[20] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57 };
inline constexpr uint8_t kAPPLY_ALIGNMENT_ALL_USERSPrologue[20] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x33, 0xED, 0x4C, 0x8B, 0xF9, 0x66, 0x3B };
inline constexpr uint8_t kNETGAME_SPAWN_NOTIFY_VAPrologue[20] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD9, 0x48, 0x81, 0xEC, 0xB0 };
inline constexpr uint8_t kUSER_DISPLAY_NAMEPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB7, 0xFA, 0x48, 0x8B, 0xD9, 0x66, 0x39, 0xB9, 0xE2 };
inline constexpr uint8_t kLOADOUT_ASSIGNPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55 };
inline constexpr uint8_t kACTOR_COMPONENT_REFPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 };
inline constexpr uint8_t kJSON_IS_EMPTYPrologue[20] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x15, 0x74, 0xB9, 0x0D, 0x01, 0xE8, 0xF0, 0xE6, 0xFF, 0xFF, 0x33, 0xC9, 0x48, 0x85 };
inline constexpr uint8_t kJSON_SET_STRINGPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x30, 0x06, 0x00 };
inline constexpr uint8_t kJSON_SET_INTPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x30, 0x06, 0x00 };
inline constexpr uint8_t kJSON_GET_STRINGPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57 };
// The document constructor is THIRTEEN bytes long -- xor eax,eax / mov [rcx],rax / mov [rcx+8],rax /
// mov rax,rcx / ret -- so the last seven of these twenty are the int3 padding and the first two bytes
// of its neighbour.  They are as pinned as the rest in this image and the guard wants twenty.
inline constexpr uint8_t kJSON_DOC_CTORPrologue[20] = { 0x33, 0xC0, 0x48, 0x89, 0x01, 0x48, 0x89, 0x41, 0x08, 0x48, 0x8B, 0xC1, 0xC3, 0xCC, 0xCC, 0xCC, 0x48, 0x89, 0x5C, 0x24 };
inline constexpr uint8_t kJSON_PARSE_TEXTPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x50, 0x01, 0x00 };
inline constexpr uint8_t kJSON_GET_INTPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57 };
// 20 B at ENGINE.ACTOR_GUID_CANON_VA (0x140363610), from facts/anchors.tsv ENGINE.ACTOR_GUID_CANON.PROLOGUE.
inline constexpr uint8_t kACTOR_GUID_CANONPrologue[20] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x85, 0xC9, 0x74, 0x50, 0x48, 0x8B, 0x12, 0xE8, 0x4A, 0xFF };

// exactly these 20 bytes: cmp [rsp+30h],0 / mov rdi,[rsp+70h] / jz / test rdi,rdi. The 80-byte reference was built at rsp+0x30 by IDENT.ACTOR_COMPONENT_REF and its component-system pointer is read back from rsp+0x70, so that field sits at reference+0x40. IDENT.APPLY_LOADOUT_ITEMS reads the same field at a different frame offset.
inline constexpr uint64_t kACTOR_REF_CS_BYTESVa = 0x14015599Full;
inline constexpr uint8_t kACTOR_REF_CS_BYTES[20] = { 0x48, 0x83, 0x7C, 0x24, 0x30, 0x00, 0x48, 0x8B, 0x7C, 0x24, 0x70, 0x0F, 0x84, 0x1C, 0x01, 0x00, 0x00, 0x48, 0x85, 0xFF };
// exactly these 20 bytes: mov rbx,[rax+1C8h] / call sub_14011C8A0 / mov edx,[rsp+78h] / mov rcx,rdi / the first byte of call rbx. rsp+0x78 is two slots past the reference base pinned by IDENT.ACTOR_REF_CS_BYTES, so the SCompactPoolHandle sits at reference+0x48, and it is passed as argument 2 of the vtable predicate.
inline constexpr uint64_t kACTOR_REF_HANDLE_BYTESVa = 0x1401559C1ull;
inline constexpr uint8_t kACTOR_REF_HANDLE_BYTES[20] = { 0x48, 0x8B, 0x98, 0xC8, 0x01, 0x00, 0x00, 0xE8, 0xD3, 0x6E, 0xFC, 0xFF, 0x8B, 0x54, 0x24, 0x78, 0x48, 0x8B, 0xCF, 0xFF };
// exactly these 20 bytes: mov rax,[r8+rax+170h] / lea rdx,[rcx+rcx*2] / shl rdx,4 / add rdx,rax / the first byte of cmp rax,rdx. The 3<<4 = 48-byte stride of the customisation slot array is stored here as the instruction sequence that computes it, because no instruction in this build encodes 48.
inline constexpr uint64_t kCUST_SLOT_WALK_BYTESVa = 0x140CAA635ull;
inline constexpr uint8_t kCUST_SLOT_WALK_BYTES[20] = { 0x49, 0x8B, 0x84, 0x00, 0x70, 0x01, 0x00, 0x00, 0x48, 0x8D, 0x14, 0x49, 0x48, 0xC1, 0xE2, 0x04, 0x48, 0x03, 0xD0, 0x48 };

// ---------------------------------------------------------------------------------------------
// SPIKE-S-A: the LOCAL synchronous dispatch primitive the reply route publishes through, and the
// two globals it must read at runtime.  sub_140F80860 delivers a message to the registered
// subscribers of a message id purely from its arguments -- it takes the LOCAL fast-path (skipping
// the per-peer routing table we do not have offline) exactly when its peerId argument equals the
// engine's own local-peer sentinel.  Its own entry does `cmp rdi, cs:qword_142025F20`.  Prologue
// derived from instructions.bytes of echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05.
IDENT_FACT(EVENT_BROADCASTER_DISPATCH_LOCAL_VA, uint64_t(0x140F80860ull));  // facts-ok: EVENT.BROADCASTER_DISPATCH_LOCAL_VA not in facts/facts.tsv; sub_140F80860 __fastcall(router,msgId,ctx,peerId,payload,size); size<8 dropped by trampoline sub_14013CD50
inline constexpr uint8_t kEVENT_BROADCASTER_DISPATCH_LOCAL_VAPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41 };

// The engine's canonical local/self peer id, a read-only .data qword.  Passed as peerId so the
// dispatch takes the local fast-path; its raw value is not stored in the DB (.data payload), so it
// is READ AT RUNTIME by VA rather than transcribed.  Sibling broadcast sentinels 0x142025F28 /
// 0x30 / 0x38 route to real peers and MUST NOT be used (LOBBY.JOIN_FREE_SENTINEL_VA is 0x38).
IDENT_FACT(EVENT_LOCAL_PEER_SENTINEL_VA, uint64_t(0x142025F20ull));  // facts-ok: EVENT.BROADCASTER_LOCAL_PEER_SENTINEL_VA not in facts/facts.tsv; .data qword, reads-only; value read live

// The cached NSLobbyEntrantAdded message id, a .data qword computed at first use by the netgame's
// own registrar (seed 0x6D451003FB4B172E).  READ AT RUNTIME, never hardcoded: the same id the
// subscribe call keyed the handler under, so publishing under it provably reaches sub_14017B850.
IDENT_FACT(NETGAME_ENTRANT_ADDED_MSG_ID_VA, uint64_t(0x1420A0E38ull));  // facts-ok: NETGAME.ENTRANT_ADDED_MSG_ID_VA not in facts/facts.tsv; .data qword, populated once a lobby exists

// The re-entrancy/dispatching flag word on the router (== *hub == *(ng+NETGAME_BROADCASTER_OFF)):
// sub_140F80860 sets router+1096 |= 0x20000 across its subscriber walk and clears it after, so the
// bit is SET exactly while the engine is delivering a message -- which is the one phase (and the
// only stack) our own broadcaster listener is ever invoked from.  inReceivePhase reads it.
IDENT_FACT(EVENT_HUB_DISPATCH_FLAGS_OFF, uint32_t(1096u));  // facts-ok: EVENT.HUB_DISPATCH_FLAGS_OFF not in facts/facts.tsv; base the hub router object; or 0x20000 during delivery
IDENT_FACT(EVENT_HUB_DISPATCHING_BIT, uint32_t(0x20000u));  // facts-ok: EVENT.HUB_DISPATCHING_BIT not in facts/facts.tsv; OR'd into router+1096 across sub_140F80860's subscriber loop

// ---------------------------------------------------------------------------------------------
// SPIKE-S-B: the 216-byte per-slot LOBBY MEMBER RECORD the entrant-added handler reads to take its
// remote arm and build a model-bearing actor.  Offsets are within one record; the record base is
// the same one SlotNameField already reaches (base - IDENT.SLOTNAME_FIELD_OFF), 216-byte stride
// (== LOBBY.MEMBER_STRIDE).  All fields are plain LOCAL memory, offline-writable.
IDENT_FACT(LOBBY_REC_ACCTTYPE_OFF, uint32_t(0x00u));  // facts-ok: LOBBY.REC_ACCTTYPE_OFF not in facts/facts.tsv; qword; nibble &0xF is the fork (6==bot arm), bit4 &0x10 selects platform-key. sub_1406093C0 -> v76[0]&0xF
IDENT_FACT(LOBBY_REC_ID64_OFF, uint32_t(0x08u));  // facts-ok: LOBBY.REC_ID64_OFF not in facts/facts.tsv; qword id64 (v76[1]); {platform-key,id64} is the identity/eviction key
IDENT_FACT(LOBBY_REC_FLAGS_OFF, uint32_t(136u));  // facts-ok: LOBBY.REC_FLAGS_OFF not in facts/facts.tsv; u16; bit0 slot valid/ready (sub_14060BF70), bit2 has-extra
IDENT_FACT(LOBBY_REC_FLAGS_VALID_BIT, uint32_t(0x1u));  // facts-ok: LOBBY.REC_FLAGS_VALID_BIT not in facts/facts.tsv; base LOBBY.REC_FLAGS_OFF
IDENT_FACT(LOBBY_REC_TEAM_OFF, uint32_t(142u));  // facts-ok: LOBBY.REC_TEAM_OFF not in facts/facts.tsv; u16 team/status word; sub_1406092E0, handler outer guard !=4
IDENT_FACT(LOBBY_REC_KV_OFF, uint32_t(144u));  // facts-ok: LOBBY.REC_KV_OFF not in facts/facts.tsv; netvar/KV blob; "bodytype" lives here, absent -> PICK_BODY_TEMPLATE template 0 (model-bearing)

// Documentary anchors (VA + prologue) that PIN the S-B record layout and the S-A dispatch keying:
// the accessor functions whose bodies read each field, derived from echovr.exe md5 caa05ee3...
// Not called by this module -- their prologues are the drift guard on the offsets above.
IDENT_FACT(LOBBY_REC_READ_HEAD_VA, uint64_t(0x1406093C0ull));  // facts-ok: LOBBY.REC_READ_HEAD_VA not in facts/facts.tsv; sub_1406093C0 reads record head -> v76 (acct-type nibble + id64)
inline constexpr uint8_t kLOBBY_REC_READ_HEAD_VAPrologue[20] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xDA, 0x4C, 0x3B, 0x81, 0x68, 0x03, 0x00, 0x00, 0x73, 0x2C, 0x48, 0x8B };
IDENT_FACT(LOBBY_REC_READ_TEAM_VA, uint64_t(0x1406092E0ull));  // facts-ok: LOBBY.REC_READ_TEAM_VA not in facts/facts.tsv; sub_1406092E0 reads record+142
inline constexpr uint8_t kLOBBY_REC_READ_TEAM_VAPrologue[20] = { 0x48, 0x3B, 0x91, 0x68, 0x03, 0x00, 0x00, 0x73, 0x17, 0x48, 0x8B, 0x81, 0x60, 0x03, 0x00, 0x00, 0x48, 0x69, 0xD2, 0xD8 };
IDENT_FACT(LOBBY_REC_READ_KV_VA, uint64_t(0x140609270ull));  // facts-ok: LOBBY.REC_READ_KV_VA not in facts/facts.tsv; sub_140609270 reads record+144 netvar ("bodytype")
inline constexpr uint8_t kLOBBY_REC_READ_KV_VAPrologue[20] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x4D, 0x8B, 0xD1, 0x48, 0x8B, 0xDA, 0x4C, 0x3B, 0x81, 0x68, 0x03, 0x00, 0x00, 0x73 };
IDENT_FACT(LOBBY_PICK_BODY_TEMPLATE_VA, uint64_t(0x14019F100ull));  // facts-ok: LOBBY.PICK_BODY_TEMPLATE_VA not in facts/facts.tsv; sub_14019F100 maps bodytype netvar -> body template (default index 0)
inline constexpr uint8_t kLOBBY_PICK_BODY_TEMPLATE_VAPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF1, 0x49, 0x8B, 0xE8, 0x48, 0x8B };
IDENT_FACT(LOBBY_ACCEPT_ENTRANT_VA, uint64_t(0x1406051C0ull));  // facts-ok: LOBBY.ACCEPT_ENTRANT_VA not in facts/facts.tsv; sub_1406051C0 authors record head/flags/team + dedup/evict on {platform-key,id64}
inline constexpr uint8_t kLOBBY_ACCEPT_ENTRANT_VAPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00 };
IDENT_FACT(EVENT_DISPATCH_TRAMPOLINE_VA, uint64_t(0x14013CD50ull));  // facts-ok: EVENT.ENTRANT_ADDED_TRAMPOLINE_VA not in facts/facts.tsv; sub_14013CD50 packs {peerId,ctx,payload} and drops size<8 (cmp r9,8; jb)
inline constexpr uint8_t kEVENT_DISPATCH_TRAMPOLINE_VAPrologue[20] = { 0x48, 0x83, 0xEC, 0x58, 0x4C, 0x8B, 0xD2, 0x49, 0x83, 0xF9, 0x08, 0x72, 0x46, 0x48, 0x8B, 0x84, 0x24, 0x88, 0x00, 0x00 };
IDENT_FACT(EVENT_BROADCASTER_SUBSCRIBE_VA, uint64_t(0x140F80ED0ull));  // facts-ok: EVENT.BROADCASTER_SUBSCRIBE_VA not in facts/facts.tsv; sub_140F80ED0 keys the handler under the msg id (reference only, proves keying)
inline constexpr uint8_t kEVENT_BROADCASTER_SUBSCRIBE_VAPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55 };
IDENT_FACT(NETGAME_LOBBY_SUBSCRIBE_REGISTER_VA, uint64_t(0x14019D6F0ull));  // facts-ok: NETGAME.LOBBY_SUBSCRIBE_REGISTER_VA not in facts/facts.tsv; sub_14019D6F0 registers sub_14017B850 + caches the msg id
inline constexpr uint8_t kNETGAME_LOBBY_SUBSCRIBE_REGISTER_VAPrologue[20] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x50, 0xFE, 0xFF, 0xFF, 0x48 };

// THE LEAVE, and it is the add's SIBLING -- NETGAME.LOBBY_SUBSCRIBE_REGISTER_VA
// registers sub_14017B850 under NSLobbyEntrantAdded and sub_14017D240 under NSLobbyRemovingEntrant
// against the same receiver, so the leave is drivable exactly the way the add already is: same a2
// shape (the slot is **(a2+16)), same byte-anchored direct call, same stage-0 stack.
//
// What the handler itself decides, before any teardown happens (decompile of sub_14017D240):
//   slot  = **(u64**)(a2+16)                       refused at >= ENTITY.USER_SLOT_TABLE_MAX
//   index = u16 at ng + IDENT.SLOT_TABLE_OFF + 4*slot
//   index refused when >= the count at IDENT.ENTRANT_COUNT_OFF (0xFFFF is spelled that way too)
//   index refused when <  the count at NETGAME.LOCAL_USER_COUNT_OFF -- THE LOCAL BAND, which is
//     where the owner's own viewer entrant sits.  The engine will not tear a local user down
//     through this path, and neither will we: ReleaseReplayUser refuses it by name first.
// Only then does it call the teardown NETGAME.ENTRANT_TEARDOWN_VA(ng, index).
IDENT_FACT(NETGAME_MSG_ENTRANT_REMOVED_HANDLER_VA, uint64_t(0x14017D240ull));  // facts-ok: NETGAME.MSG_ENTRANT_REMOVED_HANDLER_VA not in facts/facts.tsv; __fastcall(ng, smsg*), the NSLobbyRemovingEntrant listener
inline constexpr uint8_t kNETGAME_MSG_ENTRANT_REMOVED_HANDLER_VAPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC, 0x20, 0x04, 0x00, 0x00, 0x4C, 0x8B, 0x4A, 0x10, 0x4C, 0x8D, 0x05 };

// The teardown it calls, sub_1401C8000, is documented in the ENG-LEAVE-1 fact drop and named
// nowhere in code: this plugin never calls it directly.  The local-band floor the handler compares
// against is already in the merged registry as NETGAME.LOCAL_USER_COUNT_OFF.

// ENG-MODEL-PUMP FIX A: the CR15NetPlayerModelSwapperCS async completion pump, and the per-actor swap
// record whose in-flight flag it clears.  All from echovr.exe md5 caa05ee3..., verified against
// sub_14042D120 (BeginModelSwap) and sub_14044A0C0 disassembly and the CR15NetPlayerModelSwapperCS
// vtable @0x141CAEE00 (RTTI .?AVCR15NetPlayerModelSwapperCS@NRadGame@NRadEngine@@).
//
// The swapper CS is resolved via FindComponentSystem (facts::CS_LOOKUP_BY_SYMBOL) off the CncaGameSpace
// at CR15NetGame + facts::NETGAME_CS_SPACE_OFF -- the same call the entity layer resolves R14NetPhysics
// and R14RemotePlayer through.  SWAPPER_SYM is the 8 bytes at 0x141C46C50, immediately before the
// "OlPrEfIxR15NetPlayerModelSwapper" string (@0x141C46C58), which is the engine's own model-swapper key
// (derivation calibrated against the known kSymEchoUnit hash, which it reproduces exactly).
IDENT_FACT(SWAPPER_SYM, uint64_t(0xEDE09DF4FE7EB326ull));  // facts-ok: SWAPPER.SYM pending; qword before OlPrEfIxR15NetPlayerModelSwapper @0x141C46C58
// The 1-argument async driver sub_14044A0C0.  It clears the re-schedule guard, and while the pending
// queue count (SWAPPER_PENDING_COUNT_OFF) is non-zero it drains the queue, installs the model on a live
// handle (vtable+768/+776) and CLEARS the per-record in-flight flag (SWAPPER_INFLIGHT_OFF) -- the
// completion the offline scheduler never pumps.  Leaf-safe: a depth-4 BFS over it and the swapper vtable
// slots it calls (57/95/96/97) reaches NEITHER the InstanceModel register sub_140323790 NOR any AI fn
// (sub_140CB7B80 / sub_140F667E0 / AddBotUser).  It is NOT sub_1404299EC -- that forwards to vtable+95 =
// sub_140CB0370, a 3-argument BeginModelSwap variant that would fault if called with one argument.
IDENT_FACT(SWAPPER_PUMP_VA, uint64_t(0x14044A0C0ull));  // facts-ok: SWAPPER.PUMP_VA pending; sub_14044A0C0 (1-arg completion driver)
inline constexpr uint8_t kSWAPPER_PUMP_VAPrologue[24] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x70 };
IDENT_FACT(SWAPPER_HANDLE_TABLE_OFF, uint32_t(0xD0u));    // facts-ok: SWAPPER.HANDLE_TABLE_OFF pending; a1[26], actor-index -> handle
IDENT_FACT(SWAPPER_RECORD_BASE_OFF, uint32_t(0x100u));    // facts-ok: SWAPPER.RECORD_BASE_OFF pending; a1[32], swap-record array base
IDENT_FACT(SWAPPER_RECORD_STRIDE, uint32_t(472u));        // facts-ok: SWAPPER.RECORD_STRIDE pending; 0x1D8 bytes per record
IDENT_FACT(SWAPPER_INFLIGHT_OFF, uint32_t(0x8u));         // facts-ok: SWAPPER.INFLIGHT_OFF pending; dword in record, set 1 on enqueue, cleared 0 on completion
IDENT_FACT(SWAPPER_PENDING_COUNT_OFF, uint32_t(0x148u));  // facts-ok: SWAPPER.PENDING_COUNT_OFF pending; a1[41], pending-queue count the driver drains while non-zero

// The per-record CURRENT-MODEL-HASH field the swap dispatcher sub_140CB0370
// compares against the target (`if (*v11 == v18)`; equal -> immediate-complete no-mesh arm, not-equal
// -> sub_14042D120 BeginModelSwap enqueue arm) and that BeginModelSwap's != arm rebuilds.  A pooled
// actor's record still carries the prior occupant's hash here, so from==to and the swap no-ops.
// Reached by the EXACT walk of sub_1404B5D90 (disasm @0x1404B5D90, verified byte-for-byte): from the
// record, the model-instance pointer at +408 and the key dword at +416; then within that instance,
// +0xD0 is a u16 index table read at 4*key, +0x100 is the slot-pointer pool (16 bytes per slot, the
// object pointer at +8 of the slot), and the current model hash sits at +0xB10 of that object.  The
// dispatcher's `*v11` is exactly this qword.  Writing the -1 no-model sentinel here makes the compare
// take the enqueue (!=) arm.  All from echovr.exe md5 caa05ee3..., against sub_1404B5D90 +
// sub_140CB0370 + sub_14042D120 disassembly.
IDENT_FACT(SWAPPER_MODELINST_OFF, uint32_t(408u));         // facts-ok: SWAPPER.MODELINST_OFF pending; rec+408, model-instance ptr (a1 of sub_1404B5D90)
IDENT_FACT(SWAPPER_MODELKEY_OFF, uint32_t(416u));          // facts-ok: SWAPPER.MODELKEY_OFF pending; rec+416, key dword (a3 of sub_1404B5D90, used as u16)
IDENT_FACT(SWAPPER_INST_INDEX_TABLE_OFF, uint32_t(0xD0u)); // facts-ok: SWAPPER.INST_INDEX_TABLE_OFF pending; inst+0xD0, u16 index table read at 4*key
IDENT_FACT(SWAPPER_INST_POOL_BASE_OFF, uint32_t(0x100u));  // facts-ok: SWAPPER.INST_POOL_BASE_OFF pending; inst+0x100, slot-pointer pool base
IDENT_FACT(SWAPPER_INST_SLOT_STRIDE, uint32_t(16u));       // facts-ok: SWAPPER.INST_SLOT_STRIDE pending; 16 bytes/slot (add rcx,rcx then [rax+rcx*8+8])
IDENT_FACT(SWAPPER_INST_SLOT_PTR_OFF, uint32_t(8u));       // facts-ok: SWAPPER.INST_SLOT_PTR_OFF pending; +8 within the slot, the object pointer
IDENT_FACT(SWAPPER_CURRENT_MODEL_OFF, uint32_t(0xB10u));   // facts-ok: SWAPPER.CURRENT_MODEL_OFF pending; +0xB10 (2832), current model hash the compare reads

// The engine's own enqueue fn CModelSwapperCS::BeginModelSwap sub_14042D120, called
// DIRECTLY to force the swap after the pooled from==to no-op has already run.  Decompiled prototype:
//   void __fastcall sub_14042D120(swapperCS* a1, unsigned int userIndex a2, uint64_t targetHash a3)
// a1 is the SAME swapper CS the sweep/pump resolve (a1[26] = +0xD0 handle table, a1[32] = +0x100
// record base -- identical to sub_140CB0370's own reads); a2 is the user index; a3 = v35 is the target
// model hash the compare tests.  It re-checks current itself -- `v9 = sub_1404B5D90(rec+408, out, rec+
// 416); if (*v9 == v35)` -- so current(-1) != target(a3) takes the ELSE (enqueue) arm: it prints
// "Starting model swap from %s to %s", sets rec+8 = 1 (in-flight), enqueues the index, ++a1[41]
// (pending), and schedules the FIX-A pump sub_14044A0C0 via sub_140505940.  The three preceding guards
// are all clear after a from==to no-op (the immediate-complete finalizer sub_1404330A0 sets no flag):
// *(rec+112)+8 == 0, rec+8 == 0, a1[41] != a1[42].  This is NOT the 3-arg dispatcher variant
// sub_140CB0370 (which would fault if mis-called).  A transitive call-graph BFS over sub_14042D120 and
// the scheduled pump sub_14044A0C0 (199 fns) reaches NEITHER sub_140323790 NOR any AI fn.  From
// echovr.exe md5 caa05ee3..., against sub_14042D120 disassembly (prologue anchored below) + pseudocode.
IDENT_FACT(SWAPPER_BEGIN_SWAP_VA, uint64_t(0x14042D120ull));  // facts-ok: SWAPPER.BEGIN_SWAP_VA pending; sub_14042D120 (3-arg BeginModelSwap enqueue fn)
inline constexpr uint8_t kSWAPPER_BEGIN_SWAP_VAPrologue[24] = { 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x53, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00 };

// The engine's OWN R15NetPlayerEquipmentUpdate RECV handler sub_140D37300, driven
// DIRECTLY with a fabricated 56-byte body carrying the recorded chassis so the engine's own
// model-changed delegate sub_140CEC740 installs the net-model component through CModelSwapperCS and the
// framework's phase-16 rebuild seats it -- NEVER the pool rebuild sub_140323790 nor the seat sub_14030F120
// by us (an exhaustive direct-call BFS over the RECV closure and all four equip-changed delegates reaches
// neither; DIAG-MODEL-COMPONENT-LEVER).  Handler prototype (decompiled):
//   void __fastcall sub_140D37300(__int64 *a1 /*equip CS*/, unsigned __int64 *a2 /*{0,0,&body}*/)
// a2[2] -> the 56-byte body: +0 dword SR15NetUserId (slot low, generation high), +8 qword chassis item id,
// +16/+24 qword equip fields, +32 dword equip field.  The handler resolves a2[2]+0 to the entrant's equip
// record and, on `record+24 != body+8`, stores the chassis and fires the model-changed delegate.
//
// a1 is the R14NetPlayerEquipment component system, resolved off the CncaGameSpace (facts::NETGAME_CS_SPACE_OFF
// == ng+0x2B08, the same *(ng+11016) sub_140CB81B0 queries) by the class symbol EQUIP_CS_SYM -- the 8 bytes
// at 0x1416D9B28, immediately before "OlPrEfIxR14NetPlayerEquipment" (@0x1416D9B30), exactly the SWAPPER_SYM
// pattern.  a1[16] (=+0x80) -> +8 -> +0x8518 is a back-reference to the owning CR15NetGame (sub_140113A90),
// used as a correctness confirmation that a1 belongs to OUR ng.  All from echovr.exe md5 caa05ee3..., against
// sub_140D37300 / trampoline sub_140C855F0 / registrar sub_140D1AE50 / sub_140113A90.
IDENT_FACT(EQUIP_RECV_VA, uint64_t(0x140D37300ull));  // facts-ok: EQUIP.RECV_VA pending; sub_140D37300
inline constexpr uint8_t kEQUIP_RECV_VAPrologue[24] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0xFA };
IDENT_FACT(EQUIP_CS_SYM, uint64_t(0xCA14940C84B03F6Eull));  // facts-ok: EQUIP.CS_SYM pending; qword @0x1416D9B28 before "OlPrEfIxR14NetPlayerEquipment"
IDENT_FACT(EQUIP_NETGAME_CTX_OFF, uint32_t(0x80u));      // facts-ok: EQUIP.NETGAME_CTX_OFF pending; a1[16], ptr to the messaging ctx
IDENT_FACT(EQUIP_NETGAME_CTX2_OFF, uint32_t(0x8u));      // facts-ok: EQUIP.NETGAME_CTX2_OFF pending; +8 within it
IDENT_FACT(EQUIP_NETGAME_BACKREF_OFF, uint32_t(0x8518u)); // facts-ok: EQUIP.NETGAME_BACKREF_OFF pending; +34072 -> owning CR15NetGame (sub_140113A90)
// The recorded chassis lives in the loadout slot named "chassis" (IDENT.LOADOUT_SLOT_NAME_03).  The
// recorded pair's slotKey is the engine's own CustSlotEntry.key == IDENT.SYMBOL_FROM_NAME_VA("chassis"),
// so the chassis item id is matched by key, never by position (applied pairs are packed in the engine's
// enumeration order, not one-per-fixed-slot).
inline constexpr char kEQUIP_CHASSIS_SLOT_NAME[] = "chassis";

// ENG-SPAWNED-SEED backstop: the dress half of NETGAME.USER_SETTINGS_APPLY's tail.  sub_1401B1FD0
// ends `sub_1401C9BA0(ng, idx)` (NETGAME.PARTICIPATING_VA) ... `sub_1401C7BE0(ng, idx)`; this is
// the second, `void __fastcall sub_1401C7BE0(__int64 ng, unsigned __int16 idx)`, whose
// sub_140CAA5F0 -> sub_140CAB3B0 chain prints `Applying loadout items for user` and reaches Apply
// Chassis.  24 B at file offset 0x1C6FE0 of echovr.exe md5 caa05ee3...; the mov rax,[rcx+2DA0h] in
// them is the body's own first read, `**(ng + 11680) & 4`.
IDENT_FACT(NETGAME_DRESS_VA, uint64_t(0x1401C7BE0ull));  // facts-ok: NETGAME.DRESS_VA pending; sub_1401C7BE0
inline constexpr uint8_t kNETGAME_DRESS_VAPrologue[24] = { 0x40, 0x53, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x81, 0xA0, 0x2D, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x44, 0x8B, 0x00, 0x41, 0xC0 };

}  // namespace replay::identity_facts

