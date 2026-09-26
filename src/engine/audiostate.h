#pragma once

// B.11.4 IAudioState -- the audio half of "playback should make the state correct". Owned by W2-H;
// consumed by W2-A (keyframe capture), W2-B (the residue battery) and W3-C (VOIP placement).
//
// ==============================================================================================
// WHAT "CORRECT AUDIO STATE AT TIME T" IS IN THIS ENGINE, AND HOW WE KNOW
// ==============================================================================================
//
// The engine answered this itself. `CSoundCS::SaveLoadGlobal` (SOUND.SAVELOAD_GLOBAL,
// csoundcs.cpp:360) is the shipping save-game path for audio, and what it serialises is exactly one
// thing: the {stateGroup, stateValue} table at CSoundPlayer + SOUND.MEMO_CACHE_OFF. Its LOAD arm
// walks that table and replays every pair through CSoundPlayer::SetState. So the engine's own
// designers, asked "what is the audio state of this session", answered "the set of state-group
// assignments" -- and the pin block this header feeds carries precisely that set, plus the three
// things a save-game never needed and a seek does.
//
// The model has five layers, and each is here for a stated reason:
//
//   1. GLOBAL STATES -- {u32 group, u32 value}. Music, atmosphere, round phase. Set by an EDGE
//      (a script node fires once when the round starts) and never re-driven, so a seek to the
//      middle of a round will not re-issue them. NOT RE-DERIVABLE. Must be stored.
//
//   2. PER-EMITTER SWITCHES -- {u32 gameObj, u32 group, u32 value}. Same argument, same answer:
//      set at spawn or on a transition, never re-driven per frame. NOT RE-DERIVABLE. Stored.
//
//   3. RTPCs -- continuous, driven every frame from game state by the components that own them
//      (speed, distance, VOIP_LOUDNESS). Publishing the recorded entity state re-drives them on the
//      very next frame. SELF-HEALING, and deliberately NOT stored: storing a value the engine is
//      about to overwrite would be a second source of truth for a number that already has one.
//      The exception is named in S10.5 and handled there, not here.
//
//   4. PLAYING LOOPS -- an event posted before T is an edge in the past; nothing replays it. A
//      long loop that should be sounding at T therefore has to be re-armed with its event id, its
//      game object and its PHASE. NOT RE-DERIVABLE. Stored.
//
//   5. RNG PINS -- the Wwise random seed and container history decide which of N variations plays.
//      Not derivable from anything. Ruling Q4 makes the pin required. Stored.
//
// One-shots that fired before T are deliberately absent from all five, and that is a decision
// rather than an omission: re-firing them would machine-gun on every scrub (S10.3).
//
// ==============================================================================================
// THE MEMOISATION TRAP, AND WHY THIS DESIGN DOES NOT USE THE SENTINEL
// ==============================================================================================
//
// CSoundPlayer::SetState and ::SetSwitch cache every pair they set and return SUCCESS without
// calling Wwise when asked for a pair they already hold (N-16). After a backwards seek the engine
// believes it has already set the state it is being asked for, so music and atmosphere can be
// permanently wrong for the rest of the session.
//
// The design proposes forcing the memo "through a sentinel value first, then the real one".
// ★ THIS IMPLEMENTATION DOES NOT DO THAT, and the reason is a correctness one rather than a
// preference. A sentinel is a value handed to AK::SoundEngine::SetState, and it has two failure
// modes with no upside: an id that is not a real state produces a Wwise error through the game's
// own error path on every reconcile, and an id that IS a real state briefly sets the wrong one --
// audibly, on a group whose whole purpose is to select music. Both are avoidable, because the memo
// is not opaque: it is a sorted vector of 8-byte pairs whose data pointer is at +0 and whose live
// count is at +SOUND.MEMO_COUNT_OFF.
//
// So the mechanism here is: call the EXPORTED AK::SoundEngine::SetState directly -- Wwise gets the
// truth with no intermediate value -- and then correct the game's cached dword to agree. That is
// "authoring data rather than defeating a check" in the fullest sense: afterwards the cache is not
// merely bypassed, it is RIGHT, so the game's own next call behaves correctly too.
//
// A group that is not in the memo at all is set through Wwise and left uncached. That is safe and
// deliberate: inserting would need the engine's allocator, and the only consequence of not
// inserting is that the game's next SetState for that group actually reaches Wwise, which is the
// behaviour we want anyway.

#include <stddef.h>
#include <stdint.h>

#include "foundation/demofile/demotypes.h"

namespace replay {

// A Wwise id is 32 bits. B.11.4 spells these Symbol64 and this header keeps that spelling, but a
// value that does not fit in 32 bits is a caller error and is refused rather than truncated -- a
// silent narrowing here would set a state nobody asked for.
struct AudioStatePair {
    uint32_t group;
    uint32_t value;
};
struct AudioSwitchTriple {
    uint32_t gameObj;
    uint32_t group;
    uint32_t value;
};
// One sounding voice, enough to put it back. `positionMs` is where in the source it was, so a loop
// re-arms at its phase instead of restarting -- E-A6's requirement.
//
// ★ `sustainedMs` is what separates a loop from a one-shot, and it is measured rather than guessed.
// Wwise will not tell us whether an event loops, and S10.3 forbids re-firing one-shots on a seek
// (it would machine-gun on every scrub). But a one-shot in this game is short and a loop is not, so
// the capture tracks how long each (gameObj, eventId) has been continuously sounding and stores it.
// Seek re-arms only voices that have outlasted the threshold. The threshold is a config key, so the
// discriminator is inspectable and adjustable rather than a constant somebody has to trust.
struct AudioLoopPin {
    uint32_t gameObj;
    uint32_t eventId;
    int32_t positionMs;
    uint32_t sustainedMs;
};

// What a snapshot found, including what it could not fit. ★ A cap that silently truncated would
// move the defect rather than report it: the pin block would look complete and a seek would restore
// a subset nobody could name. Every Snapshot* call reports the TOTAL it saw alongside the count it
// wrote, and the pin encoder refuses to write a block whose totals exceed what it stored.
struct AudioSnapshotCounts {
    uint32_t states = 0, statesTotal = 0;
    uint32_t switches = 0, switchesTotal = 0;
    uint32_t loops = 0, loopsTotal = 0;
    uint32_t emittersWalked = 0;
};

// Why a reconcile did what it did. Reported so a seek that fixed nothing is distinguishable from a
// seek that had nothing to fix -- which are the same observation and opposite diagnoses.
struct ReconcileReport {
    uint32_t statesChecked = 0;
    uint32_t statesCorrectedInWwise = 0;   // Wwise disagreed with the target
    uint32_t memoEntriesCorrected = 0;     // the game's cache disagreed with Wwise
    uint32_t switchesChecked = 0;
    uint32_t switchesCorrectedInWwise = 0;
    uint32_t refusals = 0;                 // a call the bindings could not make
    bool soundDisabled = false;            // SOUND.DISABLE_FLAG_VA was set: every setter no-ops
};

// ---- B.11.4, plus the amendment the pin block forced -----------------------------------------
// ★ AMENDMENT, DECLARED RATHER THAN SLIPPED IN. B.11.4 as frozen has no way to ENUMERATE audio
// state, only to set and read one entry at a time. The design's own note says the keyframe
// needs a slot "only if some Wwise state is set by an edge that cannot be resolved at T; that is
// W2-H's question to answer". The answer is yes -- layers 1, 2 and 4 above are all edge-set -- and
// answering it requires enumeration, because a capture cannot store a set it cannot list. The four
// Snapshot/Apply members below are that amendment. They widen the interface and break no existing
// consumer; B.13's freeze gate checks that the contract compiles, and it still does.
class IAudioState {
public:
    virtual bool SetState(Symbol64 group, Symbol64 value) = 0;
    virtual bool SetSwitch(Symbol64 group, Symbol64 value, uint64_t gameObj) = 0;
    virtual bool GetStateTruth(Symbol64 group, Symbol64& value) const = 0;
    virtual bool GetSwitchTruth(Symbol64 group, uint64_t gameObj, Symbol64& value) const = 0;
    virtual void Reconcile() = 0;
    virtual void PinRandom(uint32_t seed, const Blob& containerHistory) = 0;
    virtual bool ReadRandomPins(uint32_t& seed, Blob& containerHistory) const = 0;

    // ---- the amendment ----
    virtual size_t SnapshotStates(AudioStatePair* out, size_t cap, uint32_t* total) const = 0;
    virtual size_t SnapshotSwitches(AudioSwitchTriple* out, size_t cap, uint32_t* total) const = 0;
    virtual size_t SnapshotLoops(AudioLoopPin* out, size_t cap, uint32_t* total) const = 0;
    // Sets the target the next Reconcile drives towards. Passing an empty set is not "no targets",
    // it is "the file carried no pins", and Reconcile then does only the memo-vs-Wwise repair --
    // which is the honest behaviour for a file recorded before the pin block existed.
    virtual void SetTargets(const AudioStatePair* states, size_t stateCount,
                            const AudioSwitchTriple* switches, size_t switchCount) = 0;
    virtual const ReconcileReport& LastReconcile() const = 0;

protected:
    ~IAudioState() = default;
};

namespace audio {

// Process-wide accessor, registered into ReplayServices by the audio module. Null until the module
// has bound the AK exports and resolved CSoundPlayer -- which is the state -noaudio leaves us in,
// and every consumer already checks its ReplayServices pointers for null.
//
// Construction and the test seams live in audio/audiostate_init.h rather than here, so this file
// stays what B.13's freeze gate expects: a contract, with no dependency on the module that
// implements it.
IAudioState* AudioState();

}  // namespace audio
}  // namespace replay
