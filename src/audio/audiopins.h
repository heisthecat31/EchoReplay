#pragma once

// KB_AUDIO_PINS -- block id 16 of S_KEY, reserved by W2-A and filled here.
//
// ==============================================================================================
// WHAT THE FORMAT HAS TO CARRY, AND WHY EACH ROW IS IN OR OUT
// ==============================================================================================
//
// A seek target is a moment, and the question the pin block answers is "what does audio need that
// cannot be recomputed from the file at that moment". The test for every candidate row was: does
// publishing the recorded entity state at T cause the engine to produce this by itself?
//
//   IN   global state groups      set by an edge; a seek into mid-round never re-issues them
//   IN   per-emitter switches     same
//   IN   playing loops + phase    the posting event is an edge in the past; nothing replays it
//   IN   the Wwise random seed    not derivable from anything (ruling Q4)
//   IN   the container history    same
//   OUT  RTPCs                    re-driven every frame by the components that own them
//   OUT  one-shots already fired  S10.3: re-firing them machine-guns on every scrub
//   OUT  emitter positions        the listener and the emitters follow the published poses
//   OUT  the state/switch SHADOW  W2-B S2 F4 withdrew it, and correctly: Reconcile reads truth from
//                                 Wwise, so what it needs is the TARGET at T, which is what the
//                                 three IN rows above are. A shadow of what we drove would be a
//                                 second source of truth for a value Wwise already holds.
//
// ==============================================================================================
// THE BLOCK IS ITSELF SUB-FRAMED, AND THAT IS THE POINT
// ==============================================================================================
//
// W2-A's S_KEY is already block-framed with skip-unknown-and-counted, so adding this block breaks
// no file. The same argument applies one level down: the exact field list here rests on a live
// reading that has not happened yet (runs/queue/W2-H.probe.json), and a positional struct would
// make the first correction a format break for every match recorded before it. So the payload is a
// sequence of tagged sections with the same discipline -- skip an unknown tag by its length, and
// COUNT it, so a player that cannot read a section says so instead of seeking to a moment that
// quietly sounds wrong.
//
//     [u16 pinVersion][u16 sectionCount]
//     repeated: [u16 tag][u32 byteLen][bytes]
//
// Sections are fixed-stride arrays of the PODs in engine/audiostate.h, so a decoder is a length
// division and a memcpy, and a length that is not a whole number of records is a refusal.

#include <stddef.h>
#include <stdint.h>

#include "engine/audiostate.h"

namespace replay {
namespace audio {

constexpr uint16_t kPinVersion = 1;

// The reason code carried by kPinSource. An empty pin set has two entirely different
// meanings and this is the field that separates them.
enum class PinSource : uint8_t {
    Bound = 0,          // a bound IAudioState answered; empty means nothing was sustained
    NoAudioState = 1,   // no IAudioState to ask (CSoundPlayer unresolved): empty for want of
                        // a source, which is not a statement about what was sounding
    Unstated = 2,       // the block carried no kPinSource section at all
};
const char* PinSourceName(PinSource s);

enum PinSection : uint16_t {
    kPinStates = 1,      // AudioStatePair[]
    kPinSwitches = 2,    // AudioSwitchTriple[]
    kPinLoops = 3,       // AudioLoopPin[]
    kPinRandom = 4,      // PinRandomHeader followed by the container-history bytes
    kPinCounts = 5,      // PinTotals -- what the capture SAW, against what it stored
    kPinSource = 6,      // PinSourceHeader -- whether there was a state to read at all
};

#pragma pack(push, 1)
struct PinRandomHeader {
    uint32_t seed;
    uint8_t seedValid;    // the seed is write-only in this build; see IAudioState::ReadRandomPins
    uint8_t reserved[3];
    uint32_t historyBytes;
};
// Written on EVERY block. The three cases a reader must tell apart are "no block at all" (a
// file recorded before block 16 existed, or a writer that refused), "we asked and there was no
// audio state" and "we asked and nothing was sustained" -- and the last two are both an empty
// pin set, so without this section they are the same bytes.
struct PinSourceHeader {
    uint8_t reason;
    uint8_t reserved[3];
};
// ★ The totals section is not bookkeeping, it is the thing that makes a truncated pin block
// detectable. Every array above is capped; without the totals beside them, a capture that saw 900
// switches and stored 512 produces a file indistinguishable from one where 512 was the whole truth,
// and a seek would restore a subset nobody could name. `echodemo verify` reports any row where
// stored < seen.
struct PinTotals {
    uint32_t statesSeen, statesStored;
    uint32_t switchesSeen, switchesStored;
    uint32_t loopsSeen, loopsStored;
    uint32_t emittersWalked;
    uint32_t targetsDropped;
};
#pragma pack(pop)
static_assert(sizeof(PinRandomHeader) == 12, "PinRandomHeader is 12 bytes on disk");
static_assert(sizeof(PinSourceHeader) == 4, "PinSourceHeader is 4 bytes on disk");
static_assert(sizeof(PinTotals) == 32, "PinTotals is 32 bytes on disk");
static_assert(sizeof(AudioStatePair) == 8, "AudioStatePair is stored as two dwords");
static_assert(sizeof(AudioSwitchTriple) == 12, "AudioSwitchTriple is stored as three dwords");
static_assert(sizeof(AudioLoopPin) == 16, "AudioLoopPin is stored as four dwords");

// Everything a keyframe carries for audio, in memory. Fixed capacity: the capture path allocates
// nothing (S6.4), and the totals record what did not fit.
struct AudioPins {
    static constexpr size_t kMaxStates = 64;
    static constexpr size_t kMaxSwitches = 512;
    static constexpr size_t kMaxLoops = 256;
    static constexpr size_t kMaxHistory = 16384;

    AudioStatePair states[kMaxStates];
    uint32_t stateCount = 0;
    AudioSwitchTriple switches[kMaxSwitches];
    uint32_t switchCount = 0;
    AudioLoopPin loops[kMaxLoops];
    uint32_t loopCount = 0;

    uint32_t seed = 0;
    bool seedValid = false;
    uint8_t history[kMaxHistory];
    uint32_t historyBytes = 0;

    PinTotals totals{};

    // Unstated is the honest default for a decode: a block with no source section did not
    // say. GetAudioPins sets Bound when it really did read a bound state.
    PinSource source = PinSource::Unstated;
};

// The worst-case block size, so a caller can size a buffer without guessing. Header, five section
// headers, and every array full.
constexpr size_t MaxPinBytes() {
    return 4 + 6 * 6 + sizeof(PinSourceHeader) + AudioPins::kMaxStates * sizeof(AudioStatePair) +
           AudioPins::kMaxSwitches * sizeof(AudioSwitchTriple) +
           AudioPins::kMaxLoops * sizeof(AudioLoopPin) + sizeof(PinRandomHeader) +
           AudioPins::kMaxHistory + sizeof(PinTotals);
}

// Serialises into caller memory. Returns the byte count, or 0 if it did not fit -- and 0 means the
// caller must not write a short block: a truncated pin block decodes into plausible nonsense where
// a missing one is at least visible as an absent section.
size_t EncodePins(const AudioPins&, void* dst, size_t cap);

// Why a decode was refused. Never a bare false: a keyframe that will not restore audio has to say
// which part of it could not be read.
enum class PinDecode : uint8_t {
    Ok = 0,
    TooShort,
    UnsupportedVersion,
    SectionOverrunsBlock,
    RaggedSection,        // a length that is not a whole number of records
    HistoryTruncated,
    Count
};
const char* PinDecodeName(PinDecode);

struct PinDecodeReport {
    PinDecode verdict = PinDecode::Ok;
    uint16_t unknownSections = 0;    // counted, never silently skipped
    uint32_t unknownBytes = 0;
    uint32_t droppedRecords = 0;     // sections longer than this build's caps
};

PinDecode DecodePins(const void* src, size_t bytes, AudioPins* out, PinDecodeReport* report);

}  // namespace audio
}  // namespace replay
