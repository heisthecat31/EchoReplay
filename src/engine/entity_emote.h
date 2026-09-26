#pragma once

// The entity-layer half of bit 14 (`is_emote_playing`) on the scoreboard.
//
// WHY IT LIVES HERE.  identity_access reads everything the scoreboard needs out of CR15NetGame,
// and `is_emote_playing` is the one field that is NOT on CR15NetGame: sub_1401B2EA0 leaves the
// netGame entirely and walks the bound game space's R15NetCustomization component system.  That
// walk is entity-layer work -- the same space the entity module already binds -- so identity_access
// asks for it through EmoteSourceSeam and this file answers.
//
// THE ENGINE'S OWN WALK, byte for byte (sub_1401B2EA0, and every number below is a fact row):
//
//   0x1401B3ED2  mov  rcx,[rdi+10h]              ; the writer's own space handle
//   0x1401B3ED6  call sub_1404F37A0              ; -> the game space
//   0x1401B3F11  mov  rdx,[rdx]                  ; IDENT.SYM_R15NETCUSTOMIZATION, the qword at
//                                                ; 0x1416D9A30, directly before the string
//                                                ; "OlPrEfIxR15NetCustomization"
//   0x1401B3F1E  call sub_1401055C0              ; CncaGameSpace::FindComponentSystem(space, sym)
//   0x1401B3F23  mov  rdi,rax                    ; the component system
//   0x1401B3F40  mov  rcx,[rdi]                  ; its component array, [cs+0]
//   0x1401B3F56  mov  rcx,[rcx+rbx*8]            ; element rbx
//   0x1401B3F61  call [rax+1A8h]                 ; IDENT.EMOTE_VTSLOT_OFF; rdx = &out, r8 = the
//                                                ; entrant key, r9 = -1
//   0x1401B3F6B  mov  edx,[rax]                  ; the emote id is the DWORD at the returned
//                                                ; pointer -- `8B10` carries no displacement byte,
//                                                ; which is the only encoding a +0 read has
//   0x1401B3F73  call [rax+1C8h]                 ; STATS.EMOTE_VTSLOT; non-zero -> this element
//                                                ; owns that id, and the walk stops here
//   0x1401B3F80  cmp  rbx,[rdi+30h]              ; IDENT.EMOTE_CS_COUNT_OFF bounds the walk
//
// THE KEY, DERIVED.  EMOTE's scan stopped at 0x1401B3F20 because it took that
// for the end of the function; sub_1401B2EA0 in fact runs to 0x1401B602D, and the frame slot the
// key comes from is filled inside it:
//
//   0x1401B3780  mov    rcx,[rdi+18h]                ; rdi = arg0, [rdi+18h] = CR15NetGame
//   0x1401B378A  imul   r15,rax,250h                 ; USER.STRIDE * the entrant's slot
//   0x1401B3791  movzx  eax,[r15+rcx+178h]           ; USER.ARRAY_OFF -- the same record
//   0x1401B3EB8  movups xmm0,[r15+rcx+180h]          ; IDENT.USER_ACTOR_GUID_OFF, all 16 bytes
//   0x1401B3EC1  movups [rbp-38h],xmm0               ; the frame pair EMOTE saw only the load of
//   0x1401B3EFF  mov    rax,[rbp-30h]                ; = the SActorGUID's SECOND qword
//   0x1401B3F14  mov    [rsp+20h],rax
//   0x1401B3F4A  mov    r8,[rsp+20h]                 ; -> r8 at the 0x1A8 call below
//
// So the key an entrant is named by is the second qword of its SActorGUID, at
// IDENT.USER_ACTOR_GUID_OFF + 8 + USER.STRIDE*slot on CR15NetGame -- the quantity identity_access
// already reads whole for ResolveComponent, and the one IDENT.ACTOR_COMPONENT_REF's registry row
// says selection is by ("Selection is BY THE GUID's second qword").  EmoteActorGuidKey below is
// that read; nothing here is guessed.
//
// THE WALK'S OTHER INPUT, `customizationCs`.  The engine reaches its component
// system through CncaGameSpace::FindComponentSystem(space, IDENT.SYM_R15NETCUSTOMIZATION) at
// 0x1401B3F1E, and that is FACTS.CS_LOOKUP_BY_SYMBOL -- the same stock call entity.cpp resolves its
// two pools through.  ResolveCustomizationCs below makes that call on the space the entity module
// has already validated and bound, so no component-system instance offset is guessed here either.
// A build whose image does not carry the recorded prologue at that address gets a null lookup and
// therefore a null system: bit 14 stays unpublished rather than being read out of a wrong function.

#include <stdint.h>

#include "engine/entity.h"
#include "engine/identity_access.h"

namespace replay {
namespace entity {

// How an entrant is named to the component system.  Returns false when this build cannot name it.
using EmoteActorKeyFn = bool (*)(void* netGame, UserIndex, uint64_t& keyOut);

// The engine's own answer to that: the second qword of the entrant's SActorGUID (see the walk
// above).  False when the roster cannot be read or the entrant is past its bound -- never a
// fabricated zero, because 0 is a key the component system would happily be asked about.
bool EmoteActorGuidKey(void* netGame, UserIndex entrant, uint64_t& keyOut);

// CncaGameSpace::FindComponentSystem, taken from the mapped image only after the bytes at
// FACTS.CS_LOOKUP_BY_SYMBOL match its recorded prologue.  Null on any other build, and null in a
// test host, where nothing is mapped at that address at all.
CsLookupFn EngineCsLookup();

// The walk's component-system input: FindComponentSystem(gameSpace, SYM_R15NETCUSTOMIZATION).
// `find` null means EngineCsLookup(); tests inject their own.  Null out when there is no space, no
// callable lookup, or the lookup itself answered null -- the one state the seam calls EmoteNoSystem.
void* ResolveCustomizationCs(void* gameSpace, CsLookupFn find);

// The three things the walk needs.  A chain missing any of them declines, and says so.
struct EmoteChain {
    void*           customizationCs = nullptr;  // FindComponentSystem(space, SYM_R15NETCUSTOMIZATION)
    EmoteActorKeyFn actorKey        = nullptr;
    void*           netGame         = nullptr;  // the netGame this chain was resolved against
};

// Recorded by the entity module wherever the binding moves; cleared at Shutdown.  A null chain and
// a chain that cannot answer are different states and produce the same refusal for different
// reasons -- see ResolveEmoteOnSystem.
void              SetEmoteChain(const EmoteChain*);
const EmoteChain* InstalledEmoteChain();

// The walk above, on a component system the caller already has.  Every dereference is guarded
// against uncommitted memory, because a test host and an unbound module both reach here with a
// pointer that is not the engine's.
//
// false + Refusal::EmoteNoSystem       -- no component system, or no element claimed the id
// false + Refusal::EmoteVtableUnreadable -- an element, its vtable, or the slot is not committed
// false + Refusal::EmoteUnreadable     -- the call returned a pointer whose dword is not committed
bool ResolveEmoteOnSystem(void* customizationCs, uint64_t entrantKey,
                          identity_access::EmoteSource& out,
                          identity_access::Refusal* why = nullptr);

// The seam identity_access::SetEmoteSource is handed.  Its Resolve refuses unless the recorded
// chain answers for THIS netGame: a component system resolved out of one game space, asked about
// an entrant of another, names a different player and comes back as a clean success.
extern const identity_access::EmoteSourceSeam kEmoteSource;

}  // namespace entity
}  // namespace replay
