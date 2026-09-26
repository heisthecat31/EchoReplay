#pragma once

// Mirrors the ENG-EVENTS-1 fact drop, which W0-2 merges into facts/facts.tsv.  Once that merge
// has happened, configure with -DCOMPONENTFEED_FACTS_MERGED and every value below becomes an
// assertion against the generated registry instead of a second copy of it.
//
// The entry point is the engine's OWN receive handler for SR15NetComponentEvent.  Playback hands it
// the recorded 24 bytes and lets it build the 40-byte SComponentEvent and raise it; nothing here
// re-derives a field, and no tube is animated from the plugin.  See
// The RE note RE-COMPONENT-EVENT-ENTRY (not in this repository) has the derivation of every value below.

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay::componentfeed_facts {

// CR15NetGame::ComponentEventCB.  Registered against the broadcaster at 0x140158FD4 through the
// size-checking trampoline sub_14013CCF0, whose `cmp r9,18h` is the independent proof that the wire
// body is 24 bytes.  Called with (netGame, &smsg); it re-broadcasts only when the relay bit is set,
// resolves the handle through RESOLVE_ACTOR_VA, and raises on the level space at netGame+0x2B10.
inline constexpr uint64_t kEntryVa = 0x14015D810ull;
inline constexpr uint8_t kEntryPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81,
                                             0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x81,
                                             0xA0, 0x2D, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x48};

// CR15NetIdCS::Actor.  THE ENGINE'S OWN IsComponentValid: index < count, generation match, live bit,
// else the invalid sentinel.  Called before every raise so the plugin never walks the pool itself.
// Its prologue is also what pins the pool: bytes +4..+10 are `mov r9,[rip+0x142DEFD]`, and
// 0x140CA5BAB + 0x142DEFD = 0x1420D3AA8, so verifying these 24 bytes verifies the pool address too.
inline constexpr uint64_t kResolveActorVa = 0x140CA5BA0ull;
inline constexpr uint8_t kResolveActorPrologue[] = {0x48, 0x83, 0xEC, 0x18, 0x4C, 0x8B, 0x0D, 0xFD,
                                                    0xDE, 0x42, 0x01, 0x4C, 0x8B, 0xD1, 0x4D, 0x85,
                                                    0xC9, 0x74, 0x68, 0x0F, 0xB7, 0xC2, 0xB9, 0xFF};

// The CR15NetIdCS pool, read for the probe line only -- the validation itself is the call above.
// 4096 slots of 16 bytes, then a 4096-bit live map, then 4096 generations, then the count: the four
// offsets chain exactly, which is what makes the 12-bit index / 4-bit generation split structural.
inline constexpr uint64_t kPoolPtrVa      = 0x1420D3AA8ull;
// Derived from the prologue above rather than trusted: bytes 4..10 are `mov r9,[rip+disp32]`, the
// instruction ends 11 bytes into the function, and rip-relative operands are measured from there.
inline constexpr uint64_t kPoolPtrFromPrologue =
    kResolveActorVa + 11ull +
    (uint64_t(kResolveActorPrologue[7]) | (uint64_t(kResolveActorPrologue[8]) << 8) |
     (uint64_t(kResolveActorPrologue[9]) << 16) | (uint64_t(kResolveActorPrologue[10]) << 24));
static_assert(kPoolPtrFromPrologue == kPoolPtrVa,
              "the pool address and the prologue that references it disagree -- one of them was "
              "transcribed rather than read");
inline constexpr uint32_t kPoolLiveOff    = 0x10000u;
inline constexpr uint32_t kPoolGenOff     = 66048u;
inline constexpr uint32_t kPoolCountOff   = 74240u;
inline constexpr uint32_t kPoolSlots      = 4096u;

// The local peer id the handler compares the SMsg's sender against; on equality it stamps
// SComponentEvent+36 with 0xFFFFFFFE, which is what a locally-originated event looks like.  It is
// runtime-initialised, so the plugin LOADS it rather than baking a value.
//
// ANCHORED, NOT TRANSCRIBED.  The reference is `cmp rcx,[rip+disp]` at kEntryVa+0xCA; the ten bytes
// below sit at kEntryVa+0xC4 and contain that displacement, so verifying them at load re-derives
// 0x142025F20 out of the image instead of trusting this line.
inline constexpr uint64_t kSelfPeerVa       = 0x142025F20ull;
inline constexpr uint64_t kSelfPeerAnchorVa = kEntryVa + 0xC4ull;
inline constexpr uint8_t kSelfPeerAnchor[] = {0x45, 0x33, 0xC0, 0x48, 0x3B,
                                              0x0D, 0x42, 0x86, 0xEC, 0x01};
// Byte index of the rip-relative displacement inside the anchor, and the length of the instruction
// that follows it -- `cmp rcx,[rip+disp]` is 7 bytes, so the next instruction is disp+4 away.
inline constexpr uint32_t kSelfPeerDispOff = 6u;

// The 24-byte SR15NetComponentEvent wire body, as the capture stores it verbatim and as the
// trampoline's `if (payloadLen >= 0x18)` sizes it.  Every field is read by memcpy: +16 and +18 are
// two u16s inside what the recorder's own decode calls one u32, and the handler reads them apart.
inline constexpr uint32_t kBodyBytes    = 24;
inline constexpr uint32_t kOffSymbol    = 0;   // u64 CSymbol64 -- the event-DB key
inline constexpr uint32_t kOffScriptId  = 8;   // u64 script content hash, carried to the graph
inline constexpr uint32_t kOffHandle    = 16;  // u16 gen<<12 | index into the CR15NetIdCS pool
inline constexpr uint32_t kOffFlags     = 18;  // u16: bits 0-2 space selector, bit 3 relay
inline constexpr uint32_t kOffParam     = 20;  // u32 param, handed to the graph

// body+18 bit 3.  Set means "the server should relay this to its peers"; the plugin clears it in its
// own copy so sub_14012BB70 is unreachable whatever the host's role flags say.  Declining to
// originate network traffic is not replicating a behaviour.
inline constexpr uint8_t kRelayBit = 0x08;
// body+18 bits 0-2.  5 is the engine's "global / any space" value, which short-circuits the space
// match; anything else must equal the netGame's current space id or the handler drops the row.
inline constexpr uint16_t kSpaceSelectorMask = 0x7;
inline constexpr uint16_t kSpaceSelectorAny  = 5;

inline constexpr uint16_t kHandleIndexMask = 0x0FFF;
inline constexpr uint32_t kHandleGenShift  = 12;
// The allocator's own "ran out of network ids" index, which the capture also uses as "no target
// actor": it resolves to the invalid SActor and the raise then skips the alias fan-out.  Passed
// through untouched rather than refused -- it is a legitimate recorded form.
inline constexpr uint16_t kHandleIndexNone = 0x0FFF;

// What CR15NetIdCS::Actor writes when the three-part check fails.  16 bytes: {u64 space, u64 ref}.
inline constexpr uint64_t kInvalidActorSpace = 0xFFFFFFFFFFFFFFFFull;  // facts-ok: LOBBY.JOIN_FREE_SENTINEL an all-ones u64 "none" local to this code, not the lobby join sentinel
inline constexpr uint64_t kInvalidActorRef   = 0x0000FFFFFFFFFFFFull;
// The low 16 bits of SActor.actorRef index the space's actor tables; 0xFFFF is "none", and
// SendComponentEventGlobal's own `ev+16 != 0xFFFF` gate reads exactly this.
inline constexpr uint16_t kActorRefNone = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE the SActor.actorRef "none" word, a different engine field that spends 0xFFFF

// netGame -> the LEVEL game space, the one the handler raises on.  Read for the probe line only:
// which space a raise lands on is the handler's own decision, with its own fallback chain, and
// entering at the handler rather than at the raise is what keeps that decision the engine's.
inline constexpr uint32_t kLevelSpacePtrOff = 0x2B10u;  // facts-ok: NETGAME.SPECTATORCAM_CS_OFF this feed's own level-space pointer offset, a different field that shares 0x2B10

// The five-qword SMsg the ListenProxy trampoline builds on its stack and the handler is called
// with.  Spelled whole rather than truncated to the three fields this path fills: the struct is the
// engine's.
struct EngineComponentMsg {
    uint64_t senderPeerId = 0;
    uint64_t cookie = 0;
    uint64_t body = 0;
    uint64_t blob = 0;     // 0 for every capture row -- all of them are exactly 24 bytes
    uint64_t blobLen = 0;
};

#ifdef COMPONENTFEED_FACTS_MERGED
static_assert(kEntryVa == facts::CEVT_ENTRY_VA, "merged registry disagrees");
static_assert(kResolveActorVa == facts::CEVT_RESOLVE_ACTOR_VA, "merged registry disagrees");
static_assert(kPoolPtrVa == facts::CEVT_POOL_PTR_VA, "merged registry disagrees");
static_assert(kSelfPeerVa == facts::CEVT_SELF_PEER_VA, "merged registry disagrees");
static_assert(kBodyBytes == facts::CEVT_BODY_BYTES, "merged registry disagrees");
#endif

}  // namespace replay::componentfeed_facts
