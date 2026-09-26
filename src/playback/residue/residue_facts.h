#pragma once

// W3-A. The constants this packet derived and `W0-2` has not merged yet.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS FILE EXISTS, AND WHY IT IS SMALL ON PURPOSE.
//
// facts.tsv is frozen to W0-2, so a value derived after the freeze cannot be read through
// facts_generated.h until it is merged. The tree's answer to that is a per-packet header carrying
// the derivation inline -- src/audio/audio_facts.h, src/audio/voip/voip_facts.h and
// src/engine/localpawn_facts.h are the same shape. Every row below is queued in
// the W3-A fact drop with the same evidence.
//
// ★★ NOTHING HERE IS AN ADDRESS. Every function address and every already-derived offset this
// packet uses comes from facts_generated.h by id, so there is exactly one place in the tree that
// carries an engine address and it is not this one. What is below is the field layout of three
// structs, each read out of a named instruction whose base register is an attributable `this`.
//
// ★★★ AND ONE ADDRESS THIS PACKET DID **NOT** REGISTER, which is the point of the anchors rule.
// The CPhSystem singleton lives at 0x1420A00F0 and the registry ALREADY CARRIES IT, as
// CLOCK.THIRD_PTR -- derived from the clock side, by a different packet, for a different reason.
// Registering it again under a physics name would have put two ids on one address and made a later
// disagreement between them undetectable. It is used below through facts::CLOCK_THIRD_PTR, and the
// identity note is queued for W0-2 as an amendment to that row rather than as a new row.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay {
namespace playback {
namespace residue_facts {

// -------------------------------------------------------------------------------------------
// CR15NetPhysicsCS -> CPhysicsCS. The net-physics system does not own bodies; it owns the
// ncaPhysics component handle per entity and defers to CPhysicsCS for the body itself.
//
// Evidence: CR15NetPhysicsCS::ValidatePhysicsSimState at 0x140D8A8F0 resolves a body as
//   sub_14044CC40( *(this + 0x170), &target, *(u32*)(netCi + 0x0C) )
// and CR15NetPhysicsCS::InitNetPhysicsCI at 0x140CF59C0 fills netCi+0x0C from
//   (**(this + 0x170) + 424)(...)   -- the same slot, refused with the literal
//   "R14NetPhysics requires ncaPhysics" when the low word comes back 0xFFFF.
// Two functions, one slot, and the refusal string names the component.
inline constexpr uint32_t kNetCsPhysicsCsOff = 0x170u;

// The ncaPhysics SCompactPoolHandle inside the 504-byte SR15NetPhysicsCI. Its LOW WORD is the
// handle CPhysicsCS indexes by; 0xFFFF is the engine's own "no component" sentinel, tested at
// 0x140CF59C0 before the refusal above.
inline constexpr uint32_t kNetCiPhysicsHandleOff = 0x0Cu;
inline constexpr uint16_t kNoComponentHandle     = 0xFFFFu;

// -------------------------------------------------------------------------------------------
// The CPhysicsCS component instance, and the CPhTarget embedded in it.
//
// Evidence, all four from CPhysicsCS::PrimaryBody at 0x14044CC40, whose `this` is rcx:
//   0x14044CC75  imul rcx, 78h                  -> stride 120
//   0x14044CC80  cmp  qword ptr [rcx+38h], 0    -> the "has a primary body" guard
//   0x14044CC87  movups xmm0, [rcx+40h]         -> CPhTarget bytes 0..15
//   0x14044CC91  movsd  xmm1, [rcx+50h]         -> CPhTarget bytes 16..23
// The handle map, count and pool base it reads are the standard component-system triple already
// in the registry (PHYS_CS_HANDLE_MAP_OFF / _COUNT_OFF / _POOL_BASE_OFF), read here off a
// CPhysicsCS rather than a CR15NetPhysicsCS -- same shape, different instance.
inline constexpr uint32_t kPhysicsCiStride    = 120u;
inline constexpr uint32_t kPhysicsCiBodyGuard = 0x38u;
inline constexpr uint32_t kPhysicsCiTargetOff = 0x40u;

// -------------------------------------------------------------------------------------------
// CPhTarget, 24 bytes. Evidence: CPhTarget::ResolveBody at 0x140638830 reads all three fields,
//   0x140638830  mov  r9d, [rcx+8]     -> index, compared against 0xFFFFFFFF as the dead marker
//   0x14063883A  mov  eax, [rcx+0Ch]   -> zone index, scaled by the zone stride
//   0x140638871  mov  ecx, [rcx+10h]   -> generation tag, compared against the body's own
// and PrimaryBody's not-found arm at 0x14044CCA1 writes exactly that shape as its sentinel:
// qword -1, qword -1, dword 0, dword -1.
inline constexpr uint32_t kPhTargetIndexOff = 0x08u;
inline constexpr uint32_t kPhTargetZoneOff  = 0x0Cu;
inline constexpr uint32_t kPhTargetTagOff   = 0x10u;
inline constexpr uint32_t kPhTargetDeadIndex = 0xFFFFFFFFu;

// -------------------------------------------------------------------------------------------
// CPhSystem -- the zone table. Two functions agree, which is why these are here and not guessed:
//   CPhTarget::TargetZone   0x140638F50:  mov rcx,[qword_1420A00F0]; imul rax,1E18h; add rax,[rcx+8]
//   CPhTarget::ResolveBody  0x140638830:  mov rax,[qword_1420A00F0]; mov rdx,[rax+8]; imul r8,rax,1E18h
//   CPhTarget::ResolveZone  0x140638EF0:  cmp rax,[rdx+38h]   -> the zone COUNT, which ResolveBody
//                                                                omits because it trusts its own index
// The count is taken from ResolveZone deliberately: this packet reads a CPhTarget it did not
// author, so it applies the bound the engine applies when IT does not trust the index either.
inline constexpr uint32_t kPhSystemZoneArrayOff = 0x08u;
inline constexpr uint32_t kPhSystemZoneCountOff = 0x38u;
inline constexpr uint32_t kPhZoneStride         = 0x1E18u;

// The dead-slot marker and the generation tag, on both CPhZone and CPhBody. Evidence:
//   0x140638F13  cmp dword ptr [rax+8], 0FFFFFFFFh   -- on a CPhZone, in ResolveZone
//   0x14063886B  cmp dword ptr [rax+8], 0FFFFFFFFh   -- on a CPhBody, in ResolveBody
//   0x140638F1C  cmp [rax+10h], ecx                  -- CPhZone generation vs CPhTarget tag
//   0x140638874  cmp [rax+10h], ecx                  -- CPhBody generation vs CPhTarget tag
// One CPhId header at the same two offsets on both types; that is why one pair of names serves.
inline constexpr uint32_t kPhIdIndexOff = 0x08u;
inline constexpr uint32_t kPhIdTagOff   = 0x10u;
inline constexpr uint32_t kPhIdDead     = 0xFFFFFFFFu;

// -------------------------------------------------------------------------------------------
// CPhBody. Every field this packet writes is already a merged fact; the two below are the read-only
// classification the sweep filters on, and only the flag mask is new.
//
// ★ THE MASK IS A REFUSAL, NOT A SELECTOR. CPhBody::BeAwake_ (RESET_PHYS_WAKE_PC) branches at
// 0x140646BC7 on `flags & 0xC` and sends kinematic (0x4) and sphere-effect (0x8) bodies down a
// path that does not wake anything. Filtering them out here is not an optimisation: it is the
// difference between a sweep whose count means "bodies woken" and one whose count means "calls
// made", and this packet reports the former.
inline constexpr uint32_t kPhBodyNotWakeableMask = 0xCu;

// -------------------------------------------------------------------------------------------
// ★★★ THE BODY'S CLASS, AND WHY N-7a IS GATED ON IT RATHER THAN ON THE FLAG MASK ABOVE.
//
// N-7a raises PHYS.BODY_MOTION_BIT so the next CPhZone::CollectClassifiedBodies pass takes its wake
// arm. That is only true for a body that pass VISITS -- and it is not a sweep. It walks exactly two
// intrusive lists, whose head indices are zone+0xD70 and zone+0xD78 and whose next-link is
// body+0x374, resolving each index through the zone body array:
//
//   0x14068CB8E  v21 = *(u32*)(zone + 0xD70)      first head
//   0x14068CB98  v22 = *(u32*)(zone + 0xD78)      second head
//   0x14068CC12  body = *(void**)(zone + 0x268) + 0xF38 * index
//   0x14068CC23  index = *(u32*)(body + 0x374)    next
//
// The classifier sub_14066C2F0 decides which list a body joins, and for a body that is neither
// kinematic nor sphere-effect it is NOT simply "class 4 or 6":
//
//   0x14066C382  test dword ptr [rsi+0C0h], 40000h   -> class 8 if set. A SECOND flags word, at
//                                                       +0xC0, which kPhBodyNotWakeableMask never
//                                                       looks at
//   0x14066C395  and edx, 20000000h                  -> else class (v7 & 0x20000000 | 0x40000000)>>28,
//                                                       i.e. 4 when bit 29 is clear, 6 when set
//   0x14066C417  lea r8, [rdi+0D70h]                 -> class 4's list head
//   0x14066C435  lea r8, [rdi+0D78h]                 -> class 6's list head
//
// and the classifier's own first line makes class 0 possible outright when its second argument is
// non-zero. So a body can pass `(flags@0xBC & 0xC) == 0` and still be class 8 or class 0, on neither
// list, never visited.
//
// ★★ AND FOR SUCH A BODY THE BIT IS NEVER READ AND NEVER CLEARED. Searching on the MASK rather than
// on the displacement text -- the distinction W1-I paid for and which this packet's first search
// repeated -- the whole image contains exactly three stores of 0FEFFFFFFh and two tests of 1000000h
// against a CPhBody flags word: the clear inside CPhBody::GoToSleep (0x14065468B), and the test and
// clear inside CollectClassifiedBodies (0x14068CC4A / 0x14068CC92). Nothing else reads or clears it.
// Raising it on a body the collector never visits therefore sets a flag with no reader and no
// eventual clearer: inert, and permanent until the body is destroyed. That is litter in an object
// this project does not own.
//
// So N-7a is gated on the body's OWN class field -- the engine's bookkeeping of which list it is on,
// read at the offset the classifier compares and writes:
//   0x14066C3AC  cmp [rsi+378h], edx     the compare against the freshly computed class
//   0x14066C51E  mov [rsi+378h], rax     the store when it changes
// That is exact rather than a proxy: it answers "will the collector visit this body" directly.
inline constexpr uint32_t kPhBodyClassOff = 0x378u;
inline constexpr uint32_t kPhBodyClassCollected[2] = {4u, 6u};


// -------------------------------------------------------------------------------------------
// N-6. WHY THIS PACKET WRITES NOTHING TO THE RE-ORIGIN FLAG WORD, AND WHY IT USED TO.
//
// ★★★ THE FIRST DROP OF THIS PACKET CLEARED BIT 0x200 AND WAS WRONG. The reasoning was: the
// re-origin function loads the flag word at 0x140517E36, selects bit 9 with `shr rax,9`, and clears
// exactly that bit on its way out with `and qword ptr [rbx+828h],0FFFFFFFFFFFFFDFFh` @0x140517FE7.
// That reads as "0x200 is the request; clear it and the cumulative shift does not run", which is
// what S7.4.3 N-6's fallback asks for.
//
// It is wrong because the bit is not a request, it is HALF A TWO-PHASE STATE, and the other half is
// the one the reasoning never looked at. Inside sub_14050BD00, in ONE read-modify-write:
//
//   0x14050BF02  mov rcx, [rbx+828h]
//   0x14050BF09  btr rcx, 8          <- clears 0x100
//   0x14050BF0E  bts rcx, 9          <- sets   0x200
//   0x14050BF13  mov [rbx+828h], rcx
//
// 0x100 and 0x200 are swapped atomically: 0x100 means a translation is PENDING and 0x200 means it
// has BEGUN. The re-origin function is the second half, and clearing 0x200 from outside cancels a
// translation that has already started -- with 0x100 already cleared, no bit remains that would ever
// finish it, and the world is left permanently half-shifted. The fallback would have caused the
// defect the row exists to prevent.
//
// ★★ IT WAS CAUGHT BY ANOTHER PACKET'S WORK, NOT BY THIS ONE'S. W2-A's src/capture/worldorigin.cpp
// derived the same two-phase machine from the capture side and named it. The lesson is the one this
// project keeps paying for and it is not "check twice": a mask read off ONE of its two writers is a
// census that answered a narrower question than the row needed. The `and ...FDFF` was real, guarded,
// PC-valid, and described the END of a sequence whose BEGINNING decided what it meant.
//
// So there is no request-bit constant here any more, and nothing in this packet writes to
// WORLD.REORIGIN_FLAG_OFF. N-6a latches the cumulative offset and reports drift; N-6b is registered
// as unavailable with this as its reason. The registry's WORLD.REORIGIN_FLAG_BITS (0x300,
// PC_PRESENT, sourced from the design's prose) should not be promoted on the strength of that prose:
// the W3-A fact drop asks for it to carry this finding instead.
//
// ---------------------------------------------------------------------------------------------
// ★ AND THE SPAN WAS WRONG TOO, IN A WAY THAT WOULD HAVE FIRED ON EVERY SEEK.
//
// The first drop latched 24 bytes from WORLD.REORIGIN_OFF_LO to WORLD.REORIGIN_OFF_HI+4, reading the
// two as the ends of one span. W0-2 has since retired both ids: OFF_LO is the X of a float3, and
// OFF_HI is "the z of a DIFFERENT, transient float3" (facts/retired.tsv). The 24 bytes therefore
// covered the CUMULATIVE triple AND the PENDING one -- and the pending triple is zeroed by the render
// thread on every particle compute pass, so a drift detector watching it would have reported a world
// shift on essentially every discontinuity. A false positive on the one observable this row has.
//
// The latch is now the cumulative triple only: WORLD_REORIGIN_OFF_X..OFF_Z, twelve bytes, ending
// strictly below WORLD_REORIGIN_PENDING_OFF_X. residuesite.h static_asserts exactly that.

}  // namespace residue_facts
}  // namespace playback
}  // namespace replay
