#pragma once

// Mirrors the ENG-GOAL-1 fact drop, which W0-2 merges into facts/facts.tsv.  Once that merge
// has happened, configure with -DGOALFEED_FACTS_MERGED and every value below becomes an assertion
// against the generated registry instead of a second copy of it.
//
// The two entry points are the engine's OWN receive handlers for SR15NetReliablePostScoreUpdate.
// Playback calls them directly rather than broadcasting, for the reason the stat feed states and
// one more: only ONE of the two is registered on this process's role.  CR15NetGameplay's handler
// is registered inside the `!isAuthority` arm of the gameplay ctor, and an offline replay host is
// the authority -- so a re-broadcast would run half the path and the banner would read "[NO GOAL]",
// because the goal-type expression node reads the field only that handler writes.

#include <stdint.h>

#include "foundation/gametype.h"   // SymbolHash: the rig's event symbol is derived, not transcribed
#include "generated/facts_generated.h"

namespace replay::goalfeed_facts {

using replay::gametype::SymbolHash;

// CR15NetGameplay::OnPostScoreStats -- eight stores into the gameplay object, no calls, no branch.
// These are the fields the banner's expression nodes read, so this call IS the level restore.
inline constexpr uint64_t kGameplayFillVa = 0x140185630ull;  // facts-ok: GOAL.GAMEPLAY_FILL_VA an unmerged pending drop, so gen_facts emits no constant for it yet
// CR15NetGame::ReliablePostScoreUpdateCB -- the /session score cache plus the tail raise of
// nevt_post_score_update_received, which is what STARTS the banner/FX/announcer graphs.  Calling
// it is the EDGE; not calling it is what makes a seek silent.
inline constexpr uint64_t kReceiveVa = 0x14019EDE0ull;  // facts-ok: GOAL.RECEIVE_VA an unmerged pending drop, so gen_facts emits no constant for it yet
// CR15NetGame::PlayGoalSFX -- the scorer's equipped goal_fx.  Its script node hard-refuses on a
// server process, so on a replay host it is reachable only by a direct call.  Default OFF: see
// goalfeed.h on the probe that decides whether calling it can double-fire.
inline constexpr uint64_t kGoalSfxVa = 0x14018C3B0ull;  // facts-ok: GOAL.SFX_VA an unmerged pending drop, so gen_facts emits no constant for it yet
// The deferred named-event raise that CR15NetGame::ReliablePostScoreUpdateCB itself tail-jumps to:
// (netGame, CSymbol64) -> sub_140510060(space, sym, -1, -1, 1), on the level space at netGame+11024
// with the engine's own two fallbacks.  Taking this entry rather than the primitive is what keeps
// the space choice the engine's: nothing here picks a space or builds an event.
inline constexpr uint64_t kEventRaiseVa = 0x1401A9FE0ull;  // facts-ok: GOAL.EVENT_RAISE_VA an unmerged pending drop, so gen_facts emits no constant for it yet

inline constexpr uint8_t kGameplayFillPrologue[] = {
    0x48, 0x8B, 0x42, 0x10, 0x4C, 0x8B, 0xC9, 0x44, 0x0F, 0xB7, 0x00, 0x66,
    0x44, 0x89, 0x41, 0x10, 0x48, 0x8B, 0x42, 0x10, 0x44, 0x0F, 0xB7, 0x40};
inline constexpr uint8_t kReceivePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89,
    0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x42, 0x10};
inline constexpr uint8_t kGoalSfxPrologue[] = {
    0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xD9, 0x48, 0x81, 0xEC, 0xB0};
inline constexpr uint8_t kEventRaisePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48,
    0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x89};

// THE GOAL RIG'S OWN EVENT, computed rather than transcribed.
//
// A CSymbol64 is the hash of the lower-cased name, so the plugin derives it the way capture/markers
// derives its netvar keys.  The value below is the `mov rax, imm64` the compiled scripts carry for
// the same name, which is the independent check that the derivation and the script compiler agree.
//
// Listener set, scanned BY HASH across all 567 local script DBs: exactly one, 38965d90ab25e230,
// the goal rig.  Its 41 node classes are CUE/PLAYSOUND/SETINSTANCEMODELCOLOR/DELAY/ENABLECOMPONENT/
// DEBUGPRINT plus expressions -- no send-event node, no R15NETSET*, no teleport, no physics, no
// reset or lobby node.  The two other scripts carrying the symbol (38965d90a823f03f, the disc) hold
// it in SENDEVENTGLOBALNODE executors: they RAISE it, they do not listen for it.
inline constexpr uint64_t kExplosionSymbol = 0x271885E087064B50ull;  // facts-ok: SCRIPT.EVT_GLB_SCORED_MATANIMATION an unmerged pending drop, so gen_facts emits no constant for it yet
static_assert(kExplosionSymbol == gametype::SymbolHash("evt_glb_scored_matanimation"),
              "evt_glb_scored_matanimation");

// THE EXPLOSION ITSELF.  The matanimation above is the rig's material flash -- its single listener
// owns no particle node, which is exactly why a goal that raised it was HEARD and not SEEN.  The
// particles hang off these, whose listeners are three FX graphs (676dd8d22f877d73,
// 4d027c6e7133f91b, 12220d6560c7f5d3) carrying particles, beams, materials, anim and Wwise and
// NONE of the node classes the S3.4a clearance forbids -- and, unlike the rig's entry, carrying no
// actor filter, so an unaddressed level-space raise reaches all three.  The one SENDEVENTGLOBALNODE
// among them re-raises fx_send_score_event and AA3E7D5A, whose listener sets are those same two FX
// graphs: the transitive closure is closed and cosmetic.  RE-PRESENT-FALLBACKS.md B2-#1/B2-#2.
//
// Each value is the hash the compiled scripts carry for that name; the derivation below is the
// independent check that the name and the constant agree.
inline constexpr uint64_t kScoreFxBlueSymbol = 0xE14549F2F4C40266ull;  // facts-ok: SCRIPT.FX_BLUE_SCORE_EVENT an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint64_t kScoreFxRedSymbol  = 0xF7B1DE946009B29Eull;  // facts-ok: SCRIPT.FX_RED_SCORE_EVENT an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint64_t kScoreFxSendSymbol = 0xFA9415C11A6A3B6Cull;  // facts-ok: SCRIPT.FX_SEND_SCORE_EVENT an unmerged pending drop, so gen_facts emits no constant for it yet
static_assert(kScoreFxBlueSymbol == gametype::SymbolHash("fx_blue_score_event"),
              "fx_blue_score_event");
static_assert(kScoreFxRedSymbol == gametype::SymbolHash("fx_red_score_event"),
              "fx_red_score_event");
static_assert(kScoreFxSendSymbol == gametype::SymbolHash("fx_send_score_event"),
              "fx_send_score_event");

// WHICH SIDE THE RECORDED `team` NAMES: the team the point went to, own goals included (OnScoreCB
// sub_140185DF0 writes the goal object's team there). 0 is blue on screen (ENG-PRESENT-D: BLUE 2->4
// on a `team 0` fire). It only picks the explosion's colour.
inline constexpr uint16_t kTeamBlue = 0;

// The 28-byte SR15NetReliablePostScoreUpdate body, as OnScoreCB assembles it and as the receive
// trampoline's `if (a4 >= 0x1C)` sizes it.  +6 and +10 are UNALIGNED u32s -- read and written by
// memcpy, never by a cast through a u32*.
inline constexpr uint32_t kBodyBytes      = 28;
inline constexpr uint32_t kOffTeam        = 0;   // u16
inline constexpr uint32_t kOffPoints      = 2;   // u16
inline constexpr uint32_t kOffGoalType    = 4;   // u16
inline constexpr uint32_t kOffScorer      = 6;   // u32 SR15NetUserId, unaligned
inline constexpr uint32_t kOffAssist      = 10;  // u32 SR15NetUserId, unaligned
inline constexpr uint32_t kOffSpeed       = 16;  // f32
inline constexpr uint32_t kOffDistance    = 20;  // f32
inline constexpr uint32_t kOffPossession  = 24;  // f32

// The highest goal-type the banner's string switch spells; anything above renders as nothing the
// owner would recognise, so a row carrying one is refused at decode rather than shown.
inline constexpr uint16_t kMaxGoalType = 10;

// THE UNRESOLVABLE SR15NetUserId, AND IT IS DELIBERATELY NOT 0xFFFFFFFF.
//
// The receive handler tests the id in this order: `id == 0xFFFFFFFF` FIRST -- and that arm does not
// answer "[INVALID]", it reuses the PERSISTED scorer name cached at gameplay+112, which on a
// replayed goal would put the PREVIOUS goal's scorer on the banner.  0xFFFFFFFF is the value the
// engine's own constructors store into an empty field, so it means "nothing new here", not
// "nobody".  Only if the id is something else does the handler run its three validity tests --
// `slot >= 0x10`, `generation == 0xFFFF`, generation mismatch against the live slot table -- any of
// which yields the engine's own "[INVALID]" string.
//
// So the id a goal with no mappable scorer carries is built out of the first two of those tests:
// slot 0x10 is one past the engine's own `slot < 0x10` bound (a slot that cannot exist), and the
// generation is the engine's own 0xFFFF free-element sentinel.  Both guards refuse it, no live
// entrant can ever collide with it, and the string on the banner is the engine's, not ours.
// Deliberately NOT a registry row: it is not a value read out of the binary, it is a value built
// here out of two of the binary's own guards, and the facts gate is right to refuse a constant that
// code emits and nothing could check.
inline constexpr uint32_t kNoUserId = 0xFFFF0010u;
// The engine's "field is empty" value, kept named so the line above cannot be read as an oversight.
inline constexpr uint32_t kUnsetUserId = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

// The five-qword message both handlers are called with.  Only the body pointer is read on this
// path -- neither handler touches the blob pair -- but the struct is the engine's, so it is spelled
// whole rather than truncated to what today's callee happens to dereference.
inline constexpr uint32_t kSmsgBodyOff = 0x10u;

// netGame -> CR15NetGameplay is a LOAD, not an addition: the decompile reads
// `*(_QWORD *)(netGame + 10920)` and the goal-type expression node resolves the object the same
// way.  Adding 0x2AA8 instead of dereferencing it would hand the fill handler the address of the
// pointer and scribble eight fields over whatever follows it.
inline constexpr uint32_t kGameplayPtrOff = facts::GS_RULES_OFF;

// The role word the double-presentation question hangs off: `(**(_DWORD **)(netGame + 11680) & 4)`
// is the arm the client-only Listen registrations sit in.  Read once, logged once, never acted on.
inline constexpr uint32_t kRoleWordPtrOff = 0x2DA0u;  // facts-ok: GOAL.ROLE_WORD_PTR_OFF an unmerged pending drop, so gen_facts emits no constant for it yet; the merged ids listed merely share the value
inline constexpr uint32_t kRoleClientBit  = 4u;

static_assert(kGameplayPtrOff == 0x2AA8u,  // facts-ok: GS.RULES_OFF the expected value this assert exists to pin; naming the constant on both sides would make it a tautology
              "GS.RULES_OFF is the netGame field the gameplay object is LOADED from; if the "
              "registry ever answers something else this feed is writing through the wrong pointer");

#ifdef GOALFEED_FACTS_MERGED
static_assert(kGameplayFillVa == facts::GOAL_GAMEPLAY_FILL_VA, "merged registry disagrees");
static_assert(kReceiveVa == facts::GOAL_RECEIVE_VA, "merged registry disagrees");
static_assert(kGoalSfxVa == facts::GOAL_SFX_VA, "merged registry disagrees");
static_assert(kBodyBytes == facts::GOAL_BODY_BYTES, "merged registry disagrees");
#endif

}  // namespace replay::goalfeed_facts
