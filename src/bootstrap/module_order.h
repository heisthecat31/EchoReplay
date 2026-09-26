// The two module `order` values that DIAG-SEAT-ORDER's invariant rests on, named once so the
// relation between them is a compile-time fact rather than two literals in two files.
//
// modreg::InitAll runs module `init` in ascending (order, name), and each module registers its
// stage-0 early callback from inside its own init, so a lower order means a lower index in the
// early table and therefore an earlier callback every frame.
#pragma once

namespace replay {

// The registry sorts on these values, and two orderings in it are load-bearing:
//   - residue-check must init before playback: it is the sole writer of the ReplayServices pointer
//     that playback's Init reads back through ArmPlaybackResidue (residue_module.cpp:95-96 ->
//     playback_module.cpp:2253);
//   - capture must init before both, so its phase publication runs from the lowest slot in the
//     stage-0 early table and every same-frame reader of CaptureIsRecording() sees the arming edge.
//     Playback's seat stopped reading capture state at 9a8779a; capture's own phase consumers are
//     what the order carries now.
// `clock` (310) is deliberately not here: it sits after playback for tidiness only, and its mirror
// re-reads ReplayServices::transport every frame (clock.cpp:1108-1110).
//
// Written as multiples of a named slot step so no value here transcribes a fact's value.
constexpr int kModuleOrderSlot = 100;
constexpr int kModuleOrderCapture = 2 * kModuleOrderSlot;
constexpr int kModuleOrderPlayback = 3 * kModuleOrderSlot;

// One slot below playback, for the pointer hand-off described above.
constexpr int kModuleOrderResidueLead = 10;
constexpr int kModuleOrderResidueCheck = kModuleOrderPlayback - kModuleOrderResidueLead;

static_assert(kModuleOrderCapture < kModuleOrderPlayback,
              "capture must publish its recording phase from the lowest early slot, before the "
              "later modules' callbacks in the same frame (DIAG-SEAT-ORDER)");

static_assert(kModuleOrderCapture < kModuleOrderResidueCheck &&
                  kModuleOrderResidueCheck < kModuleOrderPlayback,
              "residue-check must init between capture and playback: it lands the services pointer "
              "that playback's Init reads through ArmPlaybackResidue");

}  // namespace replay
