#pragma once

// The one number W1-D needs that facts/facts.tsv does not carry yet. It is submitted in
// the W1-D fact drop for W0-2 to merge; docs/spikes/W1-D.md S3 carries the derivation.
//
// Configure with -DTICK_FACTS_MERGED once the merge has landed and the static_assert below turns this
// header from a second copy of the value into an assertion against the registry -- the same shape
// W1-L used for wiretap_facts.h.
//
// Everything else W1-D touches is already in the registry and is used through facts:: directly:
//   GAME_SINGLETON_VA / GAME_SINGLETON_RVA, GAME_PROVIDER_SLOT_A/B/C, GAME_NETGAME_PTR_OFF,
//   CLOCK_GLOBAL_PTR, CLOCK_SCALEDDELTA_OFF, CLOCK_ACCUM1_OFF,
//   NETGAME_UPDATE, NETGAME_UPDATE_RVA, NETGAME_UPDATE_PROLOGUE.

#include <stdint.h>

namespace replay {
namespace tick_facts {

// CncaGame + 0x830, u64. Incremented by `inc qword ptr [rbx+830h]` at 0x1405096CF, the last instruction
// before sub_140509480's epilogue. That function is the unconditional tail of CR15Game vtbl[+0x1C0],
// which CncaGame::RunLoop calls once per iteration, and no branch inside it skips the increment -- so
// this counter advances exactly once per RENDERED FRAME. It is the oracle W1-D's acceptance test uses
// to prove "exactly one tick per frame", which is why its derivation is spelled out here.
//
// CR15Game derives from CncaGame, so this offset is read off the CR15Game singleton directly.
inline constexpr uint32_t kFrameCounterOff = 0x830u;  // facts-ok: GAME.FRAME_COUNTER_OFF in facts/facts.tsv

}  // namespace tick_facts
}  // namespace replay

#ifdef TICK_FACTS_MERGED
#include "generated/facts_generated.h"
static_assert(replay::tick_facts::kFrameCounterOff == facts::GAME_FRAME_COUNTER_OFF,
              "tick_facts disagrees with the merged fact registry");
#endif
