#pragma once

// Mirrors the ENG-STATS-2 fact drop, which W0-2 merges into facts/facts.tsv.  Once that merge
// has happened, configure with -DSTATSFEED_FACTS_MERGED and every value below becomes an assertion
// against the generated registry instead of a second copy of it.
//
// The two entry points are the engine's own appliers for the two stat messages.  Playback calls
// them directly: both Listen registrations live inside one role arm of sub_140157FB0, so on the
// other role no broadcast ever reaches them, and a direct call is the only path that does not
// depend on which arm this process took.

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay::statsfeed_facts {

inline constexpr uint64_t kPlayerApplyVa = 0x14019EFA0ull;  // facts-ok: STATS.PLAYER_APPLY_VA an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint64_t kTeamApplyVa   = 0x14019F060ull;  // facts-ok: STATS.TEAM_APPLY_VA an unmerged pending drop, so gen_facts emits no constant for it yet

inline constexpr uint8_t kPlayerApplyPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B,
    0xDA, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x42, 0x10, 0x8B, 0x10, 0x8B, 0xC2};
inline constexpr uint8_t kTeamApplyPrologue[] = {
    0x4C, 0x8B, 0x42, 0x20, 0x4D, 0x85, 0xC0, 0x74, 0x28, 0x48, 0x8B, 0x42,
    0x10, 0x44, 0x0F, 0xB7, 0x08, 0x41, 0x83, 0xF9, 0x02, 0x73, 0x1A, 0x48};

// The CGameStats records the two appliers write, so a test can read back what a call left behind
// without a second derivation of the layout.
inline constexpr uint32_t kPlayerRecBaseOff = 0x7098u;  // facts-ok: STATS.PLAYER_REC_BASE_OFF an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint32_t kPlayerRecStride  = 0x4D08u;  // facts-ok: STATS.PLAYER_REC_STRIDE an unmerged pending drop, so gen_facts emits no constant for it yet; USER.PROFILE_STRIDE merely shares the value
inline constexpr uint32_t kTeamRecBaseOff   = 0x4FF60u;  // facts-ok: STATS.TEAM_REC_BASE_OFF an unmerged pending drop, so gen_facts emits no constant for it yet
inline constexpr uint32_t kTeamRecStride    = 0xA60u;  // facts-ok: STATS.TEAM_REC_STRIDE an unmerged pending drop, so gen_facts emits no constant for it yet

// The message the handlers are called with: five qwords, and only three of them are read.
inline constexpr uint32_t kSmsgBodyOff    = 0x10u;
inline constexpr uint32_t kSmsgBlobOff    = 0x18u;
inline constexpr uint32_t kSmsgBlobLenOff = 0x20u;

// The slot table the player handler's gate reads, and the bound it applies to the slot.  These ARE
// merged, so they are read from the registry rather than restated.
inline constexpr uint32_t kSlotTableOff = facts::IDENT_SLOT_TABLE_OFF;
inline constexpr uint32_t kGenTableOff  = facts::IDENT_GEN_TABLE_OFF;
inline constexpr uint16_t kSlotTableMax = uint16_t(facts::ENTITY_USER_SLOT_TABLE_MAX);

static_assert(kGenTableOff == kSlotTableOff + 2,
              "the generation is the high half of the same 4-byte slot-table element; if those two "
              "registry rows ever stop being two bytes apart the element is not what this feed reads");

#ifdef STATSFEED_FACTS_MERGED
static_assert(kPlayerApplyVa == facts::STATS_PLAYER_APPLY_VA, "merged registry disagrees");
static_assert(kTeamApplyVa == facts::STATS_TEAM_APPLY_VA, "merged registry disagrees");
static_assert(kPlayerRecBaseOff == facts::STATS_PLAYER_REC_BASE_OFF, "merged registry disagrees");
static_assert(kPlayerRecStride == facts::STATS_PLAYER_REC_STRIDE, "merged registry disagrees");
static_assert(kTeamRecBaseOff == facts::STATS_TEAM_REC_BASE_OFF, "merged registry disagrees");
static_assert(kTeamRecStride == facts::STATS_TEAM_REC_STRIDE, "merged registry disagrees");
#endif

}  // namespace replay::statsfeed_facts
