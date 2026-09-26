#pragma once

// Mirrors the W3-C fact drop, which W0-2 merges into facts/facts.tsv. Once that merge has
// happened, configure with -DW3C_FACTS_MERGED and every pending value below becomes a
// static_assert against the generated registry instead of a second copy of it.
//
// ★ NOTHING HERE IS TRANSCRIBED. Every pending value was decoded out of the operand bytes at the
// site its comment names by tools/w3c/gen_w3c_facts.py, which checks the instruction encoding
// BEFORE it decodes an operand off it and aborts rather than emit a row whose decode disagrees.
// Re-run it and this header's twin reproduces byte for byte.
//
// ★ AND THE SIX VALUES ALREADY IN THE REGISTRY ARE CITED, NOT COPIED. VOIP.RECEIVE and its
// prologue are W0-2's; the component-system lookup and its prologue are the ones W1-E's E-4 and
// W2-C already call; the two per-slot netGame tables are W2-C's. A fact that exists is never
// restated -- a second copy is a second thing to go stale, and only one of them would be noticed.

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay::audio::voip_facts {

// ---- cited from the registry; NOT copied -----------------------------------------------------
inline constexpr uint64_t kImageBase = facts::PE_IMAGE_BASE;

// CR15NetVoipReceiverCS::ReceiveVoip(SCompactPoolHandle, SR15NetUserVoipEvent const&,
//                                    void const*, unsigned int)  -- the capture AND injection point.
inline constexpr uint64_t kReceiveVa = facts::VOIP_RECEIVE;
inline constexpr const uint8_t* kReceivePrologue = facts::VOIP_RECEIVE_PROLOGUE;

// CncaGameSpace::FindComponentSystem(CSymbol64) -> CComponentSystem*.
inline constexpr uint64_t kLookupBySymbolVa = facts::CS_LOOKUP_BY_SYMBOL;
inline constexpr const uint8_t* kLookupBySymbolPrologue = facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE;

// The two per-slot tables an SR15NetUserId is validated against, both on the CR15NetGame itself.
//
// ★ THERE IS NO GAMESPACE ALIAS HERE ANY MORE. The component-system space the receiver is looked
// up in is facts::NETGAME_CS_SPACE_OFF (+0x2B08) and voipsite.cpp spells it that way at its one
// site: an alias that renames a fact is a private copy in disguise, and this one carried the wrong
// type -- it named facts::IDENT_NETGAME_GAMESPACE_OFF (+0x8), which is the CR15Game back-pointer
// and has never been a CncaGameSpace (ARCH-10 S1.1). W3-C hands nothing to an identity or actor
// routine, so +0x8 has no consumer in this subsystem at all.
inline constexpr uint32_t kSlotTableOff = facts::IDENT_SLOT_TABLE_OFF;                // +0x138
inline constexpr uint32_t kGenTableOff = facts::IDENT_GEN_TABLE_OFF;                  // +0x13A
inline constexpr uint32_t kUserSlotTableMax = facts::ENTITY_USER_SLOT_TABLE_MAX;      // 16

// ---- pending in the W3-C fact drop --------------------------------------------------------

// The CSymbol64 CR15NetVoipReceiverCS registers under. Walked out of the engine's own
// `lea rdx, <name>-1 / and rdx,~7 / mov rdx,[rdx] / call CS.LOOKUP_BY_SYMBOL` at 0x140D32504, by a
// reader that must first reproduce W1-E's LOCAL.SYM_R14PLAYERBROADCASTER from the same walk.
inline constexpr uint64_t kReceiverCsSymbol = 0x7A06E9E827234EB0ull;

// The four fields of the component system a talker's SCompactPoolHandle is built from. All four
// are read by the engine's own dispatcher sub_140D8A630, and ReceiveVoip reads the pool table in
// the opposite direction, which is what makes the pair of maps a relationship rather than a guess.
inline constexpr uint32_t kCsCountOff = 0xFCu;       // u16 live instance count   @0x140D8A639
inline constexpr uint32_t kCsCiBaseOff = 0x100u;     // -> dense CI array          @0x140D8A666
inline constexpr uint32_t kCsPoolTableOff = 0xD0u;   // -> pool entries            @0x140D8A743
inline constexpr uint32_t kCsDenseMapOff = 0xE0u;    // -> dense to pool index     @0x140D8A734
inline constexpr uint32_t kCsNetGameOff = 0xB8u;     // -> CR15NetGame             @0x140D8A6BF

inline constexpr uint32_t kCiStride = 0x78u;         // 120 B per SR15NetVoipReceiverCI @0x140D8A684
inline constexpr uint32_t kCiUserIdOff = 0x20u;      // the SR15NetUserId join key      @0x140D8A688

// One pool entry is {u16 dense, u16 generation}; the generation half becomes the HIWORD of the
// handle. The stride comes from the SIB scale of the instruction that reads it, not from a 4
// somebody typed.
inline constexpr uint32_t kPoolEntryStride = 4u;     // @0x140D8A74F, SIB scale
inline constexpr uint32_t kPoolGenOff = 0x2u;        // @0x140D8A74F, disp8

// The gates VOIP.RECEIVE itself checks before it queues anything. Read so a refusal can be named.
inline constexpr uint64_t kProviderGateVa = 0x1420A2FE0ull;  // gate 1  @0x140D376E3
inline constexpr uint32_t kNetGameSubOff = 0x20u;            // gate 2  @0x140D376FD
inline constexpr uint32_t kCiDecoderStateOff = 0x2Cu;        // gate 3a @0x140D3772E
inline constexpr uint32_t kCiTalkerAOff = 0x24u;             // gate 3b @0x140D37734
inline constexpr uint32_t kCiTalkerBOff = 0x28u;             // gate 3c @0x140D3773D
inline constexpr uint64_t kMuteQueryVa = 0x14020C9D0ull;     // gate 4  @0x140D37749

// ★ THE ENGINE'S OWN BIND AND UNBIND, DRIVEN RATHER THAN REIMPLEMENTED. A decoder constructed
// here would be a second implementation of something the engine already does correctly, and it
// would drift. Connect(this, u16 poolIndex, SR15NetUserId) writes the id to CI+0x20, resets the
// sequence word to its unset marker, and -- ONLY behind the same two gates VOIP.RECEIVE checks --
// creates the talker's decoder through the net service provider and starts the Loudness Gator.
//
// ★ IT DOES NOT ALLOCATE AN INSTANCE. The pool index must already exist, so this binds a receiver
// component that is there and never creates one; whether a replay HAS them is the live read.
inline constexpr uint64_t kConnectVa = 0x140CB82F0ull;
inline constexpr uint8_t kConnectPrologue[20] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
    0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B};

// Disconnect(this, u16 poolIndex): the exact inverse in 34 instructions -- destroys the decoder
// behind the same two gates, resets the same sequence word and clock pair, and stores the no-user
// sentinel back into CI+0x20.
inline constexpr uint64_t kDisconnectVa = 0x140CCC630ull;
inline constexpr uint8_t kDisconnectPrologue[20] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
    0x48, 0x8B, 0x81, 0xD0, 0x00, 0x00, 0x00, 0x33, 0xFF, 0x44};

// The SR15NetUserId that means "nobody is in this slot", which Disconnect stores. READ at run time
// through the mapped image, never transcribed: it is a global the engine owns, and a copy here
// would be a second statement of it that only one of us would notice going stale.
inline constexpr uint64_t kNoUserSentinelVa = 0x141FFCEA0ull;

// The value gates 3b and 3c compare against. Not a sentinel we chose -- `mov eax, 0FFFFFFFFh` at
// 0x140D3771E, compared field-wise at both sites.
inline constexpr uint32_t kTalkerUnset = 0xFFFFFFFFu;

#if defined(W3C_FACTS_MERGED)
static_assert(kReceiverCsSymbol == facts::VOIP_RECEIVER_CS_SYMBOL, "W3-C row drifted");
static_assert(kCsCountOff == facts::VOIP_CS_COUNT_OFF, "W3-C row drifted");
static_assert(kCsCiBaseOff == facts::VOIP_CS_CI_BASE_OFF, "W3-C row drifted");
static_assert(kCsPoolTableOff == facts::VOIP_CS_POOL_TABLE_OFF, "W3-C row drifted");
static_assert(kCsDenseMapOff == facts::VOIP_CS_DENSE_MAP_OFF, "W3-C row drifted");
static_assert(kCsNetGameOff == facts::VOIP_CS_NETGAME_OFF, "W3-C row drifted");
static_assert(kCiStride == facts::VOIP_CI_STRIDE, "W3-C row drifted");
static_assert(kCiUserIdOff == facts::VOIP_CI_USERID_OFF, "W3-C row drifted");
static_assert(kPoolEntryStride == facts::VOIP_POOL_ENTRY_STRIDE, "W3-C row drifted");
static_assert(kPoolGenOff == facts::VOIP_POOL_GEN_OFF, "W3-C row drifted");
static_assert(kProviderGateVa == facts::VOIP_PROVIDER_GATE_VA, "W3-C row drifted");
static_assert(kCiDecoderStateOff == facts::VOIP_CI_DECODER_STATE_OFF, "W3-C row drifted");
static_assert(kCiTalkerAOff == facts::VOIP_CI_TALKER_A_OFF, "W3-C row drifted");
static_assert(kCiTalkerBOff == facts::VOIP_CI_TALKER_B_OFF, "W3-C row drifted");
static_assert(kMuteQueryVa == facts::VOIP_MUTE_QUERY, "W3-C row drifted");
#endif

}  // namespace replay::audio::voip_facts
