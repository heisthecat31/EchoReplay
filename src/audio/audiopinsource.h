#pragma once

// The LIVE side of the pin block: what audiopins.h encodes, read off the audio state this process
// is actually driving. It is separate from audiopins.cpp on purpose -- that file is the codec and
// nothing else, so a consumer that only needs to decode a block (a checker, a capture test) can
// compile it without dragging in the binding, the exports and CSoundPlayer.
//
// ==============================================================================================
// WHY THE READ IS A LEDGER AND NOT A WALK (L2-RUN-14 F37)
// ==============================================================================================
// The first version of this file gathered the whole pin set inside the keyframe sample: the state
// memo, then every emitter in CSoundPlayer's table, then every switch group on every emitter, each
// group's value asked of Wwise. RUN-14 measured what that costs on a live game: the `audiopins`
// tick phase ran at 6589-7247 us MEAN and 159358 us MAX, twice a second, on the game thread. The
// session lost 13.6% of its ticks (6104 against RUN-13's 7061 over the same window) and 22% of its
// wall clock. The cost meter's kill rung never fired because it reads a MEDIAN and the stall lands
// on one tick in fifteen.
//
// The walk is O(emitters x switch groups) in VirtualQuery probes and Wwise state queries, and it
// was being paid in full, from scratch, at keyframe cadence. So it is gone from the sample path.
// The ledger below maintains the pin set INCREMENTALLY on the audio module's own per-tick path,
// a bounded slice of the emitter table per tick with a resume cursor, and reuses the value it
// already has for any row whose entry in the game's memo has not changed. EncodeCurrentAudioPins
// then serialises a struct that is already complete: no probing, no engine reads, no allocation.

#include <stddef.h>
#include <stdint.h>

#include "audio/audiopins.h"

namespace replay {
namespace audio {

// The ledger's published pin set. False when the ledger has never seen a bound audio state -- the
// -noaudio case, and a different claim from a pin set that came back empty, which is why it is not
// a zeroed struct.
bool GetAudioPins(AudioPins* out);

// Serialises the published set into the keyframe block. 0 means "did not fit" and nothing else: a
// host with no audio state still gets a block, carrying a PinSource that says the set is empty for
// want of a source rather than for want of sound. `source` may be null.
//
// This call does no engine read and no memory probe. It is the whole of what a keyframe pays.
size_t EncodeCurrentAudioPins(void* dst, size_t cap, PinSource* source);

// One tick's slice of the walk. Called from the audio module's tick, never from the capture path.
// Bounded by kEmittersPerTick emitters and kTruthPerTick Wwise queries; a cycle that cannot finish
// inside those resumes from its cursor on the next tick, so no single tick carries the whole set.
void AudioPinsTick();

// What the ledger did, so a test can bound the per-tick cost by counting the work rather than by
// timing it, and so the capture stop line can say whether the ledger was keeping up.
struct PinLedgerStats {
    uint64_t ticks = 0;            // AudioPinsTick calls
    uint64_t cycles = 0;           // pin sets published
    uint64_t emitterSteps = 0;     // emitters visited, over all ticks
    uint64_t truthQueries = 0;     // GetStateTruth/GetSwitchTruth calls -- the Wwise-lock term
    uint64_t deferrals = 0;        // ticks that ran out of budget and resumed later
    uint64_t restarts = 0;         // cycles abandoned because the emitter table moved under us
    uint32_t maxEmittersInATick = 0;
    uint32_t maxTruthInATick = 0;
    uint32_t lastCycleTicks = 0;   // how many ticks the last published cycle took
    uint32_t emittersWalked = 0;   // emitters in the last published cycle
};
PinLedgerStats AudioPinLedger();

void ResetAudioPinLedgerForTest();

// Points the ledger's emitter walk at a synthetic CSoundPlayer laid out in a test buffer -- the
// same seam InitAudioState takes, because IAudioState hands out no cursor over that table and a
// bounded walk needs one. Zero, the default, means the real one.
void SetPinLedgerSoundPlayerForTest(uintptr_t soundPlayer);

}  // namespace audio
}  // namespace replay
