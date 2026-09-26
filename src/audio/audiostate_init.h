#pragma once

// Wiring for the B.11.4 binding. Kept out of engine/audiostate.h so that header stays a contract
// with no dependency on the module implementing it -- B.13's freeze gate includes it, and a
// contract that drags in its implementation's headers is a contract that can stop compiling for
// reasons nothing to do with the contract.

#include <stdint.h>

#include "audio/akbind.h"
#include "engine/audiostate.h"

namespace replay {
namespace audio {

// Binds the exports and resolves CSoundPlayer. `soundPlayerOverride` is the test seam: a non-zero
// value points the whole memo and emitter walk at a synthetic CSoundPlayer laid out in a test
// buffer, so every path below Reconcile is driven on a machine with no game. Returns false when
// CSoundPlayer could not be reached, which is the -noaudio case and is not an error.
bool InitAudioState(const AkExports& ak, uintptr_t soundPlayerOverride = 0);

// Samples what is sounding, once per capture tick. Separate from the keyframe snapshot on purpose:
// the loop-versus-one-shot discriminator measures how long a voice has been continuously playing,
// and a 0.5 Hz keyframe cannot measure a duration shorter than its own period.
void AudioTrackVoices(uint32_t nowMs);

// Drops the loop tracker. Called from the audio path's N-5a residue row on every discontinuity:
// the tracker measures how long a voice has been continuously sounding, and a playhead jump ends
// the window that duration was measured over.
void AudioForgetVoices();

// Targets handed to SetTargets that did not fit. Non-zero is a fidelity statement the UI shows, not
// a log line: a partial restore that looked complete is exactly the failure this counter exists for.
uint32_t AudioTargetsDropped();

// How many times the module has retried the CSoundPlayer resolve since init. CSoundPlayer exists
// only once the game has brought Wwise up, which on the -offline host is long after module init:
// L2-RUN-13 had it unresolved at init and therefore for the whole process, so every keyframe of a
// four-minute capture carried an empty pin block. The retry is what fixes that; this counter is
// what a test on a machine with no game can see, since there the resolve can never succeed.
uint32_t AudioStateArmAttempts();

// The RETRY's resolver, so a test can make CSoundPlayer appear on the Nth tick the way the game
// does: return 0 while it is not there yet, then the address of a synthetic one. Null -- the
// default -- means the module resolves the real CSoundPlayer, which is what ships.
using SoundPlayerProbe = uintptr_t (*)();
void SetSoundPlayerProbeForTest(SoundPlayerProbe);

void ResetAudioStateForTest();

}  // namespace audio
}  // namespace replay
