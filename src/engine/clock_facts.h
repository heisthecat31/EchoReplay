#pragma once

// The clock values facts/facts.tsv holds at PC_PRESENT -- gen_facts.py emits those as INCOMPLETE
// TYPES on purpose, so naming one is a compile error rather than a silent use of an uncertified
// number -- plus the CClock constructor defaults this packet derived. Submitted in
// the W2-D fact drop for W0-2; docs/spikes/W2-D.md S2 carries every derivation.
//
// Configure with -DCLOCK_FACTS_MERGED once the merge lands and the static_asserts at the foot turn
// this header from a second copy of the values into an assertion against the registry. That is
// W1-D's tick_facts.h pattern and W1-L's before it.
//
// Everything already emitted is used through facts:: directly and is NOT duplicated here:
//   CLOCK_GLOBAL_PTR, CLOCK_THIRD_PTR, CLOCK_TIMESCALE_OFF, CLOCK_FLAGS_OFF, CLOCK_MINELAPSED_OFF,
//   CLOCK_MAXELAPSED_OFF, CLOCK_MAXELAPSED_US, CLOCK_SCALEDDELTA_OFF, CLOCK_STEPONCE_OFF,
//   CLOCK_ACCUM1_OFF, CLOCK_RECIP_OFF, GAME_SINGLETON_VA, GAME_CLOCK_PTR_OFF,
//   TICK_HEARTBEAT_BEFORE(+PROLOGUE), TICK_UPDATE_CLOCK(+PROLOGUE), WATCHDOG_*.

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay {
namespace clock_facts {

// ---------------------------------------------------------------------------------------------
// 1. The two CLOCK.FLAGS_OFF bits the transport owns. PC_PRESENT in the registry because they are
//    derived from control flow rather than from an immediate; the derivation is a byte sequence
//    either way, and it is quoted here so a reader can check it without opening a disassembler.
//
//    TICK.HEARTBEAT_BEFORE @0x1400CF3D2:  48 8B 49 50        mov  rcx, [rcx+50h]
//                          @0x1400CF3D6:  0F B6 C1           movzx eax, cl
//                          @0x1400CF3D9:  D0 E8              shr  al, 1        <- selects BIT 1
//                          @0x1400CF3E4:  A8 01              test al, 1
//                          @0x1400CF3E6:  74 2E              jz   not-paused
//                          @0x1400CF3EB:  C0 E8 02           shr  al, 2        <- selects BIT 2
//                          @0x1400CF3EE:  A8 01              test al, 1
//    The bit-2 arm takes CLOCK.STEPONCE_OFF as the raw delta; the bit-1-only arm takes ZERO. BOTH
//    arms then `and rcx, 0FFFFFFFFFFFFFFFBh` and store back -- so bit 2 SELF-CLEARS on the first
//    beat that runs while paused, whether or not it was set.
// ---------------------------------------------------------------------------------------------
inline constexpr uint64_t kPauseBit    = 0x2u;  // facts-ok: CLOCK.PAUSE_BIT PC_PRESENT, so gen_facts emits no value
inline constexpr uint64_t kStepOnceBit = 0x4u;  // facts-ok: CLOCK.STEPONCE_BIT PC_PRESENT, so gen_facts emits no value

// CClock's vtable and the slot HeartbeatBefore occupies. Used as a RUNTIME TYPE CHECK: an object
// is treated as a CClock only when the qword at [obj+0] points at a table whose slot +8 holds
// TICK.HEARTBEAT_BEFORE. That tests the property this packet's whole arithmetic rests on -- "this
// object is stepped by the function whose clamp order I derived" -- rather than the identity of a
// table, which a derived class could share.
inline constexpr uint64_t kVtableVa         = 0x1416CCDA0ull;  // facts-ok: CLOCK.VTABLE PC_PRESENT, so gen_facts emits no value
inline constexpr uint32_t kVtblHeartbeatOff = 0x8u;   // facts-ok: CLOCK.VTBL_HEARTBEAT_OFF PC_PRESENT, so gen_facts emits no value

// ---------------------------------------------------------------------------------------------
// 2. CClock::CClock, and the field defaults it installs. NEW -- submitted in the W2-D fact drop.
//
//    sub_1400CD8F0 is self-identifying as the constructor: its fourth instruction is
//    `lea rax, off_1416CCDA0` / `mov [rcx], rax`, storing CLOCK.VTABLE into [this+0]. It has three
//    call sites -- 0x14010912A, 0x1404F5ADC, 0x14154905A -- which is the three CClocks this design
//    knows: CR15Game+0x7B00, CLOCK.GLOBAL_PTR and CLOCK.THIRD_PTR. Only two `lea` references to
//    CLOCK.VTABLE exist in the image (0x1400CD8F9 and 0x1400CDC16), so there is no third,
//    differently-initialising constructor.
//
//    ★★★ kCtorMinElapsedUs IS THE LOAD-BEARING ONE. The design reason 2 and facts row
//    CLOCK.MINELAPSED_OFF both record this value as UNMEASURED, and every pause design was
//    conditional on it. It is 0, and an image-wide sweep found the constructor to be its ONLY
//    writer, so it is 0 at every heartbeat. With a zero minimum the engine's own pause bit drives
//    CLOCK.SCALEDDELTA_OFF to 0, hence CLOCK.RECIP_OFF to 1.0f/0.0f = +inf. That is why this
//    module writes the floor BEFORE it ever sets the pause bit.
// ---------------------------------------------------------------------------------------------
inline constexpr uint64_t kCtorVa = 0x1400CD8F0ull;  // facts-ok: CLOCK.CTOR in facts/facts.tsv
inline constexpr uint8_t  kCtorPrologue[20] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48,
    0x8D, 0x05, 0xA0, 0xF4, 0x5F, 0x01, 0x48, 0x89, 0x01, 0x48};

// mov dword ptr [rdi+38h], 3F800000h  @0x1400CD920
// 1.0f. facts-ok: CLOCK.CTOR_TIMESCALE_INIT pending the same merge -- the bit pattern of
// 1.0f collides with AUDIO.MAXFREQRATIO_BITS, which is a different fact about a different
// subsystem that happens to be the same float.
inline constexpr uint32_t kCtorTimescaleBits = 0x3F800000u;  // facts-ok: CLOCK.CTOR_TIMESCALE_INIT pending-merge
// mov [rdi+50h], rax with rax == 0    @0x1400CD93A
inline constexpr uint64_t kCtorFlags         = 0u;
// mov [rdi+58h], rax with rax == 0    @0x1400CD93E   ★★★
inline constexpr uint64_t kCtorMinElapsedUs  = 0u;
// mov qword ptr [rdi+60h], 989680h    @0x1400CD942 -- 10 s, later overwritten to
// CLOCK.MAXELAPSED_US by CLOCK.SETMAXELAPSED_US on all three clocks at boot
inline constexpr uint64_t kCtorMaxElapsedUs  = 10000000ull;
// mov qword ptr [rdi+98h], 8235h @0x1400CD96F -- the same 0x8235 CLOCK.SETFIXEDTIMESTEP_FN
// installs, so it is that fact rather than a second copy of the number.
inline constexpr uint64_t kCtorStepOnceUs    = facts::CLOCK_FIXEDSTEP_30HZ_US;
// The number of CClock instances in the process: the constructor's call-site count.
inline constexpr uint32_t kInstanceCount     = 3u;

// ---------------------------------------------------------------------------------------------
// 3. This packet's own constants, derived rather than read.
// ---------------------------------------------------------------------------------------------
// The design's floor. One microsecond makes a zero scaled delta UNREPRESENTABLE
// anywhere in the engine, which retires the +inf class at its source instead of avoiding it at one
// site. It is a RAISE from the constructor's 0, never a lowering, so it cannot floor away a
// slow-motion rate that was not already rounding to the +inf value.
inline constexpr uint64_t kFloorUs = 1ull;

// Beats per rendered frame (docs/spikes/W1-D.md S3.1): beat 1 before stage 0, beats 2 and 3 inside
// it after our tick. Beat 2 is gated on bit 6 of CR15Game+0x7AE0 so the true count is 2 or 3; 3 is
// the upper bound, and this constant is only ever used AS an upper bound.
inline constexpr uint32_t kMaxBeatsPerFrame = 3u;

}  // namespace clock_facts
}  // namespace replay

#ifdef CLOCK_FACTS_MERGED
#include "generated/facts_generated.h"
// ONLY the rows gen_facts will actually EMIT. CLOCK.PAUSE_BIT, CLOCK.STEPONCE_BIT, CLOCK.VTABLE,
// CLOCK.VTBL_HEARTBEAT_OFF, CLOCK.CTOR_MINELAPSED_INIT and CLOCK.CTOR_FLAGS_INIT are all withheld --
// PC_PRESENT rows are emitted as INCOMPLETE TYPES on purpose -- so naming one here would turn this
// block from an assertion into a compile error the day the merge lands. They are unanchorable rather
// than uncertain: a bit index carried in an opcode's shift form, and a zero written from a register
// zeroed two instructions earlier, are values no instruction field encodes. What carries them is
// tests/clock/check.ps1 asserting the bytes on every gate run.
static_assert(replay::clock_facts::kCtorVa == facts::CLOCK_CTOR, "clock_facts: ctor");
static_assert(replay::clock_facts::kCtorTimescaleBits == facts::CLOCK_CTOR_TIMESCALE_INIT,
              "clock_facts: the timescale default");
static_assert(replay::clock_facts::kCtorMaxElapsedUs == facts::CLOCK_CTOR_MAXELAPSED_INIT,
              "clock_facts: the maximum clamp default");
#endif
