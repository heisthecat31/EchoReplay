#pragma once

// W2-A's constants, held in ONE place until W0-2 merges the W2-A fact drop.
//
// This is W1-L's pattern from src/engine/wiretap_facts.h, and it is here for the same reason: a fact
// this packet derived cannot be cited from facts_generated.h until the registry merges it, and the
// alternative -- writing the value at each use site -- is how a stale stride of 520 survived in three
// places after the true value was known (S2 F10). One declaration, every consumer cites it, and the
// moment the merge lands the block at the foot of this file turns each one into a static assertion
// rather than a second copy.
//
// ★ WHAT IS NOT HERE, and it matters: the two record-start offsets and the DUV field offsets are used
// through capture/streamcodec.h and capture/duv.h, which document their own derivation. Only the
// message symbol needs a home, because it is a bare value with no structure around it.

#include <stdint.h>

namespace replay {
namespace capture_facts {

// The CMatSym of SR15NetPhysicsSimStateEvent -- the message carrying SR15NetPhysicsSimState, which
// S_BODY_WIRE is sourced from. Like the other three message symbols W1-L needed, the engine COMPUTES
// this at run time and it appears nowhere in .text, so no read of the PC binary can establish it; the
// witness is 11,029 messages in freshcap/arena.bin, every one of which decodes as a 284-bit record at
// bit 192. the W2-A fact drop carries it as tier U for exactly that reason.
inline constexpr uint64_t kSymPhysicsSimStateEvent = 0x8F62ADBA23F15FE0ull;

// ---- the netvar names S5.7's markers are derived from -------------------------------------------
// CSymbol64s recovered by R4 through the engine's own OlPrEfIx radhash string table. No read in this
// project re-derives them on PC, so they are tier U -- and a WRONG symbol here does not fail loudly:
// it produces a file with no round markers, which looks exactly like a match nobody scored in.
// MarkerDeriver::Stats therefore reports which of these names were ever SEEN in the netvar stream, so
// "quiet match" and "we watched for the wrong names" are distinguishable rather than both silent.
inline constexpr uint64_t kNvGameState          = 0xB2996AD399FDC0FFull;  // opaque CSymbol64, ~10/match
inline constexpr uint64_t kNvGameTimer          = 0xB2996AD484F1D1E8ull;  // f32, 57.1 Hz measured
inline constexpr uint64_t kNvCountdownTimer     = 0x3E766A9AFE6C97B0ull;  // f32, 60.9 Hz measured
inline constexpr uint64_t kNvBlueScore          = 0x1C97052A21E09F11ull;  // i32
inline constexpr uint64_t kNvRedScore           = 0x2ED07B1C8C27548Dull;  // i32
inline constexpr uint64_t kNvFirstLaunchOfRound = 0xCD178A7AE1764792ull;  // raw, 1 -> 0 at main_loop
inline constexpr uint64_t kNvFrisbeeOwnerId     = 0x1B10BDC962B47FCCull;  // raw u32, 0xFFFFFFFF unowned
// The name is `net_game_paused_state`, not `paused_state`: the VALUE below has always been right --
// it is in the live capture with 508 rows carrying the symbol `unpaused` -- and only the name was
// wrong. That mistaken name is also why gamestate_facts.h could find no OlPrEfIx literal for it.
inline constexpr uint64_t kNvPausedState        = 0x02FE0AA4F2637309ull;  // net_game_paused_state
inline constexpr uint64_t kNvSuddenDeath        = 0xEE2E44F738A30C35ull;  // sudden_death_active
inline constexpr uint64_t kNvMatchIsOver        = 0xB42C6FF86B5CFB10ull;  // match_is_over

// ★ THE LAST TWO WERE THE WEAKEST ROWS IN THIS FILE AND ARE NOW SOURCED. They used to hold the
// symbols for `inner_door_open` and `outer_door_open` as PLACEHOLDERS, on the understanding that a
// real match would supply the right pair. RECORDER-5: the 2026-09-19 live capture is that match, and
// it did better than supply them -- it showed the placeholders were worse than inert.
//
// `outer_door_open` IS a real netvar and the server really sends it (3 rows in that capture), so the
// deriver's seen-mask reported `match_is_over` as SEEN when what it had seen was a door. The live
// log's `mask 0x1E3` -- "6 of 9 watched names were ever seen" -- is therefore 5, not 6, and the one
// statement that was supposed to tell a wrong symbol from a quiet match was itself wrong. A
// placeholder that collides with a live symbol is not a placeholder.
//
// The pair is no longer recovered, it is COMPUTED: gametype::SymbolHash is this project's own
// implementation of the engine's name hash, and markers.cpp asserts both values against it, so the
// two lists this file and netvarhud.cpp keep cannot drift again.

}  // namespace capture_facts
}  // namespace replay

#ifdef W2A_FACTS_MERGED
#include "generated/facts_generated.h"
namespace replay::capture_facts {
static_assert(kSymPhysicsSimStateEvent == facts::EVENT_SYM_PHYSICS_SIMSTATE_EVENT);
}  // namespace replay::capture_facts
#endif
