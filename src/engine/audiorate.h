#pragma once

// B.9 IAudioRate -- owned by W2-H, consumed by the transport (W2-B / W3-B) and the UI.
//
// ★ WHY THIS FILE IS UNDER engine/ AND NOT audio/. B.13's freeze gate (tests/contracts) looks for
// B.9 at engine/audiorate.h, while the design's ownership table lists src/audio/ and
// src/engine/audiostate* for W2-H and never mentions audiorate. The gate is the executable
// statement of the contract, so the declaration lives where the gate looks; the implementation is
// src/audio/audiorate.cpp, which is squarely inside this packet's grant. The ownership table's gap
// is reported rather than resolved unilaterally.
//
// This header declares an interface and nothing else -- no engine address, no offset, no
// dereference -- so it does not make src/engine/ depend on the audio module.
//
// ==============================================================================================
// THE ONE VALUE THAT CARRIES THE WHOLE DEGRADATION STORY
// ==============================================================================================
//
// Ruling Q-A accepts the Level-1X XAudio2 hook only on condition that it degrades gracefully, and
// MaxRate() is what carries that. Everything above 1.0x reads it; nothing else does. So:
//
//   Available() false            the S10.4.2 sink proofs refused. ALL rate control is off, audio
//                                plays at 1.0x whatever the transport does, and the rest of the
//                                product is unaffected.
//   Available() true, MaxRate 1  pause and the ENTIRE slow-motion range work exactly as designed;
//                                only pitch above 1.0x degrades, and Diagnostic() says which of
//                                "hook disabled" or "hook unavailable" it was.
//   MaxRate() > 1                the installed MaxFrequencyRatio, and speeds up to it are honest.
//
// The property this buys, and it is testable: the hook is never load-bearing for anything except
// pitch above 1.0x.

#include <stddef.h>
#include <stdint.h>

namespace replay {

class IAudioRate {
public:
    virtual bool Available() const = 0;       // false if the S10.4.2 proofs failed
    virtual float MaxRate() const = 0;        // 1.0 without the Level-1X hook
    virtual void SetRate(float r) = 0;
    virtual void Suspend(bool renderAnyway) = 0;
    virtual void Wakeup() = 0;
    virtual double AudioSeconds() const = 0;  // GetBufferTick scaled by the RUNTIME settings
    virtual const char* Diagnostic() const = 0;   // why unavailable, if it is. Never empty then.

    // ---- amendments, declared rather than slipped in ------------------------------------------
    // ★ S10.4.1a needs a consumer, and B.9 as frozen has nowhere to put it. The sink carries about
    // 85 ms of buffering, flat in SCENE time, which is 85/r ms of WALL time -- 853 ms at 0.1x, in
    // the mode the owner asked for by name. The renderer corrects it by presenting from
    // `displayPlayhead - DisplayBiasSceneUs()`. It is a constant subtraction because the quantity
    // is constant in scene time, and it is MEASURED from GetAudioSettings and the voice's queue
    // depth, never assumed to be 85 ms: that figure is one device's 8 x 512 / 48000.
    //
    // Zero until the measurement latches, and zero is the right answer until then -- no correction
    // beats a wrong one.
    virtual int64_t DisplayBiasSceneUs() const = 0;

    // The label S10.4.5 makes mandatory. IReplayTransport::ActiveModeLabel appends it, so a replay
    // whose audio has degraded says so on screen instead of quietly desyncing.
    virtual const char* RateLabel() const = 0;

    // What this module's state IS, for one log line, when a frame ran late. Not pure: a fake that
    // exists to drive the transport has nothing to say here, and saying nothing is correct for it.
    // The hitch report's whole value is the baseline/live pair -- printing it once would have named
    // the stale-read-back ratchet on its first occurrence instead of on the third launch.
    virtual void DescribeState(char* out, size_t cap) const {
        if (out != nullptr && cap != 0) {
            out[0] = '\0';
        }
    }

protected:
    ~IAudioRate() = default;
};

namespace audio {

// Registered into ReplayServices by the audio module; null until it has initialised.
IAudioRate* AudioRate();

}  // namespace audio
}  // namespace replay
