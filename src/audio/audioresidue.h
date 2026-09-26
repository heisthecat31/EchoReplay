#pragma once

// The audio half of the category-N battery: N-16, N-17a and N-17b.
//
// ==============================================================================================
// ★ THERE IS NO SEEK PATH HERE, AND THAT IS THE POINT
// ==============================================================================================
//
// W2-B's central rule binds this packet: one ApplyStateAt(window, t, mode) serves play, scrub,
// reverse and fast-forward, with no parameter through which it could learn a jump happened. What a
// jump COSTS subscribes to the discontinuity flag. So everything below is an IResidue registered
// into the battery, fired by the frame loop's step 5, and nothing in this packet is reachable from
// a "if (seeking)" branch anywhere.
//
// Audio is the hardest residue in the system for two opposite reasons, and the three primitives
// split along exactly that line:
//
//   a scrub must not replay every sound between here and there   -> one-shots that fired before
//                                                                   the target are never re-fired
//   a scrub must not kill what the engine keeps sounding         -> nothing is stopped: the engine
//                                                                   owns every voice, the pose feed
//                                                                   hands it the action edges of the
//                                                                   jump, and it ends its own loops
//
// ==============================================================================================
// WHICH CAUSES EACH RUNS ON, AND WHY THE ANSWERS DIFFER
// ==============================================================================================
//
//   N-16  reconcile     Seek, MicroSeek, GapCross, ReverseTick, ChapterEpochRoll
//         S7.5.5 puts N-16 in the per-tick reverse subset, and it belongs there: the resolved
//         target state changes as the playhead decreases, and a memo that says "already set" makes
//         that permanently wrong. It is cheap on a reverse tick because it does nothing unless the
//         TARGET SET CHANGED -- which is an exact trigger, not a heuristic, because the targets
//         only change when the window resolves a new one.
//
//   N-17a/b  mute and pins       Seek and GapCross ONLY
//         ★ NOT MicroSeek. Above the sub-step ceiling the transport micro-seeks EVERY FRAME
//         (S7.5.2), and a mute per frame is not a seek battery, it is silence with a fast-forward
//         label on it. ★ NOT ReverseTick, for the same reason at 90 Hz.
//         Within Seek, the pin restore is gated on `needs_preroll`: a held scrub delivers a stream
//         of jumps at pointer rate, so it mutes (E-A5) and reconciles, and restores the pins once,
//         on release, when the full seek arrives.

#include <stdint.h>

#include "audio/akbind.h"
#include "audio/audiopins.h"
#include "engine/audiorate.h"
#include "engine/audiostate.h"
#include "playback/core/discontinuity.h"

namespace replay {
namespace audio {

// What the battery did, so a seek that fixed nothing is distinguishable from a seek with nothing to
// fix. Both are "no change"; only one is a bug.
struct AudioResidueStats {
    uint32_t reconciles = 0;
    uint32_t pinsRestored = 0;
    uint32_t mutes = 0;
    uint32_t clockRebases = 0;          // N-5a fired: the accumulators were invalidated
};

// How the rows reach the audio state: a getter called at USE time, not a pointer read once at
// registration. replay::audio::AudioState is what the module passes; a test passes its own.
using AudioStateGetter = IAudioState* (*)();

struct AudioResidueConfig {
    bool reconcileState = true;
    bool muteOnSeek = true;
};

// Registers N-5a, N-17a, N-16 and N-17b into the battery, in S10.3's order with the accumulator
// rebase ahead of it. Returns the number registered. Every primitive registers even when its dependency is missing -- as Unmapped with a
// diagnostic -- because a battery that silently omitted a primitive would make "we could not do it"
// and "it was not needed" indistinguishable, and the user is entitled to know which parts of
// EVERYTHING this build is not respecting.
//
// * THE STATE IS ASKED FOR, NEVER CAPTURED. CSoundPlayer is created when the game brings Wwise up,
// which is long after this module's init: L2-RUN-13 registered with AudioState() == null and the
// rows held that null for the whole four-minute session, so the late resolve reached the capture's
// keyframe pins and never reached the seek residue. A getter costs one load where a captured
// pointer cost a load, and it cannot go stale.
size_t RegisterAudioResidue(playback::ResidueBattery&, AudioStateGetter, IAudioRate*,
                            const AudioResidueConfig&);

// Whether the registered rows can reach an IAudioState RIGHT NOW. False at arm time and true a
// second later is the normal, expected sequence, and it is the only way a caller can tell a build
// that never resolved CSoundPlayer from one that resolved it late.
bool AudioResidueStateBound();

// The pins the next full jump restores: the RNG seed and container history (N-17b) and the state
// targets (N-16). The loop rows a keyframe carries are not acted on here -- nothing is stopped at a
// jump, so there is nothing to put back.
void SetAudioPins(const AudioPins&);

// E-A5. A drag holds the mute across many jumps; the battery's per-jump bracket is not enough on
// its own because the transport, not the battery, knows a drag is in progress. N-17a takes a hold
// of its own, so holds NEST: every true must be paired with exactly one false, and the sink wakes
// when the LAST holder lets go. Two trues for one drag and one false leaves the replay silent.
void HoldAudioSeekMute(bool held);
// What the rate driver asks before it commands the sink: while any hold stands, a transport change
// waits rather than waking a pre-roll the hold exists to keep silent.
bool AudioSeekMuteHeld();
// The same fact as an object, for the one consumer that also has to report an unload. The driver
// reads through this and never writes the hold; this file takes it, releases it, and drops it when
// the replay it belonged to goes away.
class ISeekMute;   // ratedrive.h
ISeekMute* AudioSeekMuteSeam();

const AudioResidueStats& AudioResidueStatsRef();
// The pins SetAudioPins last received, or null before any.
const AudioPins* AudioPinsForTest();
void ResetAudioResidueForTest();

// ---- what the playback core calls -------------------------------------------------------------
// The battery belongs to W2-B and does not exist when the audio module initialises, so arming is a
// call the core makes once it owns one rather than something this module does to itself.
size_t ArmAudioResidue(playback::ResidueBattery&);

}  // namespace audio
}  // namespace replay
