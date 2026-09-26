#pragma once

// Mirrors the ENG-STUN-1 fact drop, which W0-2 merges into facts/facts.tsv.  Once that merge
// has happened, configure with -DSTUNFEED_FACTS_MERGED and every value below becomes an assertion
// against the generated registry instead of a second copy of it.
//
// WHAT THIS FEED CALLS, AND WHAT IT DELIBERATELY DOES NOT.  The engine's receive handler for
// SR15StunPlayerMsg is CR15NetPunchableCS::StunPlayerCB (sub_140D53DA0).  Its head writes three
// fields on the victim's punchable record and then raises delegate_OnHit and delegate_OnStun; its
// TAIL is role-gated at netGame+0x2DA0 and does the announcer event, the STAT BUMP and a relay.
// Playback already replays the recorded stat rows, so entering that tail would double-count stuns
// on the HUD.  This feed therefore reproduces the head -- the same three stores in the same order
// with the same masks, then the same two wrappers in the same order -- and never calls the handler.
//
// The two wrappers are the only trigger of the remote/spectator stun-FX script
// (script_fa6d80c1532607cf: run_stun_fx, ext_force_remote_stunned_fx, evt_spectator_pov_check), so
// this feed plays no sound and draws nothing itself.  RE-STUNS.md S2.1/S2.4.

#include <stdint.h>

#include "engine/localpawn_facts.h"
#include "generated/facts_generated.h"

namespace replay::stunfeed_facts {

namespace lp = replay::localpawn_facts;

// CR15NetPunchableCS::RaiseOnHit -- (punchableCS, SComponentID).  Resolves the component id through
// the system's OWN vtable+472 twice and posts delegate_OnHit through the shared emitter.  No flag
// write, no timer write, no wire traffic.
inline constexpr uint64_t kOnHitVa = 0x140CCF7A0ull;  // facts-ok: STUN.ONHIT_VA an unmerged pending drop, so gen_facts emits no constant for it yet
// CR15NetPunchableCS::RaiseOnStun -- same shape, delegate_OnStun.  The engine's own order is OnHit
// first, and the two are called back to back with the same component id.
inline constexpr uint64_t kOnStunVa = 0x140CD0030ull;  // facts-ok: STUN.ONSTUN_VA an unmerged pending drop, so gen_facts emits no constant for it yet

// THESE TWO FUNCTIONS ARE BYTE-IDENTICAL FOR 76 BYTES.  Both are 130 bytes long and differ in
// exactly six: the two `lea rdx, <symbol string>` displacements at +76 and +105.  A 32-byte
// prologue -- the length every other site in this tree guards with -- would pass the OnStun bytes
// against the OnHit address and vice versa, so a drift that swapped the two would go unseen and a
// stun would announce as a hit.  So each wrapper is guarded TWICE: the ordinary prologue, which
// proves the family, and a second 32-byte window at +48, which is the shortest aligned window that
// reaches a discriminating byte.  The displacement is RIP-relative and therefore the same value
// wherever the image loads.
inline constexpr uint32_t kWindowOff = 48u;

inline constexpr uint8_t kOnHitPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
    0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0x01, 0x8B,
    0xFA, 0x48, 0x8B, 0xA9, 0x80, 0x00, 0x00, 0x00};
inline constexpr uint8_t kOnHitWindow[] = {
    0x00, 0x44, 0x8B, 0xC7, 0x48, 0x8D, 0x54, 0x24, 0x40, 0x48, 0x8B, 0xCE,
    0x48, 0x8B, 0x58, 0x08, 0x48, 0x8B, 0x06, 0xFF, 0x90, 0xD8, 0x01, 0x00,
    0x00, 0x48, 0x8D, 0x15, 0x37, 0xFF, 0xFD, 0x00};
inline constexpr uint8_t kOnStunPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
    0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0x01, 0x8B,
    0xFA, 0x48, 0x8B, 0xA9, 0x80, 0x00, 0x00, 0x00};
inline constexpr uint8_t kOnStunWindow[] = {
    0x00, 0x44, 0x8B, 0xC7, 0x48, 0x8D, 0x54, 0x24, 0x40, 0x48, 0x8B, 0xCE,
    0x48, 0x8B, 0x58, 0x08, 0x48, 0x8B, 0x06, 0xFF, 0x90, 0xD8, 0x01, 0x00,
    0x00, 0x48, 0x8D, 0x15, 0x6F, 0x9B, 0xA0, 0x00};
static_assert(sizeof kOnHitWindow == sizeof kOnStunWindow, "same window on both");
static_assert(kOnHitWindow[28] != kOnStunWindow[28],
              "the second window must reach a byte that tells the two wrappers apart");

// THE COMPONENT SYMBOL IS READ OUT OF THE IMAGE, NOT TRANSCRIBED.  CR15NetPunchableCS registers
// under the CSymbol64 that sits in the radhash string table immediately before "OlPrEfIxR14Net-
// Punchable", and every engine site spells that as `lea rdx, str-1 / and rdx,~7 / mov rdx,[rdx]`
// (sub_140D02D40 at 0x140D02D5D, feeding CS.LOOKUP_BY_SYMBOL at 0x140D02D70; the stun handler does
// the same at 0x140D53E22).  The plugin reads the same qword from the same place rather than baking
// a hash it could not check: the ASCII below IS the check, and it is far more discriminating than a
// code prologue.
inline constexpr uint64_t kPunchableSymbolAnchorVa = 0x1416D9BD0ull;  // facts-ok: STUN.PUNCHABLE_SYM_ANCHOR_VA an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint8_t kPunchableSymbolAnchor[] = {
    0x4F, 0x6C, 0x50, 0x72, 0x45, 0x66, 0x49, 0x78, 0x52, 0x31, 0x34, 0x4E,
    0x65, 0x74, 0x50, 0x75, 0x6E, 0x63, 0x68, 0x61, 0x62, 0x6C, 0x65, 0x00};
// `&anchor[-1] & ~7`, exactly as the engine computes it.
inline constexpr uint64_t kPunchableSymbolVa = kPunchableSymbolAnchorVa - 8;

// THE 72-BYTE PUNCHABLE RECORD, off the same unshifted pool group every other component system in
// this plugin is read through.  Restated here rather than cited from localpawn_facts: these two
// displacements were read out of THIS object -- sub_140D53DA0 spells them `a1[25]` (the u16 handle
// map, 4-byte entries {element, generation}) and `a1[31]` (the element base) -- and a shared number
// is evidence about the address space, never about the object.  The static_asserts below keep the
// two readings tied together so a future divergence is a build break rather than a silent write.
inline constexpr uint32_t kCsHandleMapOff = 0xC8u;  // facts-ok: STUN.CS_HANDLE_MAP_OFF an unmerged pending drop; the merged ids that share this value are other objects
inline constexpr uint32_t kCsPoolBaseOff  = 0xF8u;  // facts-ok: STUN.CS_POOL_BASE_OFF an unmerged pending drop; the merged ids that share this value are other objects
static_assert(kCsHandleMapOff == lp::kCsHandleMapOff, "the unshifted pool group moved");
static_assert(kCsPoolBaseOff == lp::kCsPoolBaseOff, "the unshifted pool group moved");

inline constexpr uint32_t kRecordStride = 72u;  // facts-ok: STUN.RECORD_STRIDE an unmerged pending drop, so gen_facts emits no constant for it yet
// The three fields StunPlayerCB writes, in its order and with its masks:
//   *(u32*)(rec+4) = (*(u32*)(rec+4) & ~4u) | 3u;   clear post-stun invulnerable, set bit0|bit1
//   *(u32*)(rec+8) = 0;                             elapsed
//   *(u32*)(rec+28) = <f32 seconds from body+8>;    length
inline constexpr uint32_t kFlagsOff   = 4u;   // facts-ok: STUN.FLAGS_OFF an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint32_t kElapsedOff = 8u;   // facts-ok: STUN.ELAPSED_OFF an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint32_t kLengthOff  = 28u;  // facts-ok: STUN.LENGTH_OFF an unmerged pending drop, so gen_facts emits no constant for it yet

// bit0 is a ONE-FRAME PULSE: CR15NetPunchableCS::Update clears it unconditionally at the bottom of
// every per-component iteration (`*(u32*)(rec+4) &= ~1u`).  bit1 is the persistent "this player is
// stunned" flag the timer runs off.  bit2 is the post-stun invulnerability a fresh stun clears.
// So the EDGE writes bit0|bit1 (the engine's own `| 3`) and a LEVEL restore writes bit1 alone --
// re-asserting a one-frame edge at a seek destination buys nothing past the next frame.
inline constexpr uint32_t kFlagStunPulse   = 1u;
inline constexpr uint32_t kFlagStunned     = 2u;
inline constexpr uint32_t kFlagInvulnerable = 4u;

// The 16-byte SR15StunPlayerMsg body.  +0 and +4 are SR15NetUserIds (slot | generation<<16); the
// recorded halves carry the RECORDING's generation, which is 0 in the owner's capture, so both are
// remapped to the live lobby before anything is resolved off them.
inline constexpr uint32_t kBodyBytes    = 16;
inline constexpr uint32_t kOffVictim    = 0;   // u32 SR15NetUserId
inline constexpr uint32_t kOffPuncher   = 4;   // u32 SR15NetUserId
inline constexpr uint32_t kOffDuration  = 8;   // f32 seconds
inline constexpr uint32_t kOffFlag      = 12;  // u32; 1 = the punch was blocked and the PUNCHER is the victim

// A recorded duration outside this range is not something the recording can have meant: the engine
// parses stun_length out of SR15MPPunchSettings and every row in the capture in hand carries
// 2.000 s.  A row outside it is counted and refused rather than written into the timer field.
inline constexpr float kMinDuration = 0.0f;
inline constexpr float kMaxDuration = 30.0f;

// THE DIRECT DELEGATE RAISE -- the fallback for a victim whose actor carries no punchable.
//
// sub_140510060(space, CSymbol64, guid0, guid1, deferred) is the primitive both delegate wrappers
// end in: it packs {name, gamespace = *(space+0x58), actor = {guid0, guid1}, nodeid = -1,
// actorname = -1} and hands it to sub_14050FED0.  sub_140CCE580 -- the engine's own per-player
// action-word differ -- calls it exactly this way for every grab/boost/brake/punch delegate, with
// the space read out of its component system at CS+0x80 and the two GUID halves out of the actor
// the handle resolved to.  Raising OnHit/OnStun this way needs NO punchable component: the FX
// graphs listen on the actor, not on the component.
inline constexpr uint64_t kRaiseEventVa = 0x140510060ull;  // facts-ok: STUN.RAISE_EVENT_VA an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint8_t kRaiseEventPrologue[] = {
    0x48, 0x83, 0xEC, 0x58, 0x48, 0x8B, 0x41, 0x58, 0x48, 0x89, 0x54, 0x24,
    0x20, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x30, 0x44};
// The space a component system belongs to, as sub_140CCE580 spells it (`a1[16]`).
inline constexpr uint32_t kCsSpaceOff = 0x80u;  // facts-ok: STUN.CS_SPACE_OFF an unmerged pending drop, so gen_facts emits no constant for it yet

// The two delegate symbols, read out of the image at the same kind of anchor the punchable symbol
// uses: a CSymbol64 sitting 8 bytes before its own "OlPrEfIx<name>" in the radhash string table.
// The ASCII is the check; nothing here bakes a hash it could not verify.
inline constexpr uint64_t kOnHitSymbolAnchorVa = 0x141CAF728ull;  // facts-ok: STUN.ONHIT_SYM_ANCHOR_VA an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint8_t kOnHitSymbolAnchor[] = {
    0x4F, 0x6C, 0x50, 0x72, 0x45, 0x66, 0x49, 0x78, 0x64, 0x65, 0x6C, 0x65,
    0x67, 0x61, 0x74, 0x65, 0x5F, 0x4F, 0x6E, 0x48, 0x69, 0x74, 0x00};
inline constexpr uint64_t kOnHitSymbolVa = kOnHitSymbolAnchorVa - 8;
inline constexpr uint64_t kOnStunSymbolAnchorVa = 0x1416D9BF0ull;  // facts-ok: STUN.ONSTUN_SYM_ANCHOR_VA an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint8_t kOnStunSymbolAnchor[] = {
    0x4F, 0x6C, 0x50, 0x72, 0x45, 0x66, 0x49, 0x78, 0x64, 0x65, 0x6C, 0x65,
    0x67, 0x61, 0x74, 0x65, 0x5F, 0x4F, 0x6E, 0x53, 0x74, 0x75, 0x6E, 0x00};
inline constexpr uint64_t kOnStunSymbolVa = kOnStunSymbolAnchorVa - 8;

// The engine's own "no component on this actor" answer, in the low word of the SComponentID.
inline constexpr uint16_t kNoComponent = lp::kNoComponent;

// "Whatever game space this component system belongs to" -- the second argument of
// FirstActorComponent, which the searcher replaces with `cs+40` when it is -1.
inline constexpr uint64_t kThisGameSpace = lp::kThisGameSpace;

#ifdef STUNFEED_FACTS_MERGED
static_assert(kOnHitVa == facts::STUN_ONHIT_VA, "merged registry disagrees");
static_assert(kOnStunVa == facts::STUN_ONSTUN_VA, "merged registry disagrees");
static_assert(kRecordStride == facts::STUN_RECORD_STRIDE, "merged registry disagrees");
static_assert(kFlagsOff == facts::STUN_FLAGS_OFF, "merged registry disagrees");
static_assert(kElapsedOff == facts::STUN_ELAPSED_OFF, "merged registry disagrees");
static_assert(kLengthOff == facts::STUN_LENGTH_OFF, "merged registry disagrees");
#endif

}  // namespace replay::stunfeed_facts
