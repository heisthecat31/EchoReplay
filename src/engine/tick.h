#pragma once

// B.3 ITick -- the per-frame tick. Owned by W1-D; consumed by every module that needs a frame.
//
// TickInfo, TickFn and ITick are in the global namespace and spelled exactly as Appendix B.3 declares
// them, because bootstrap/services.h forward-declares them there and twelve packets write their own
// copy from the same appendix.
//
// ★ WHERE THIS TICK LANDS, and ★ WHY THE DELTA THE ENGINE HANDS US IS NOT A FRAME.
//   All VERIFIED against echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05; derivations in
//   docs/spikes/W1-D.md S3.
//
//     RunLoop, one iteration = one rendered frame
//       CR15Game vtbl[+0x3F8] -> ... -> CClock::HeartbeatBefore(global)      BEAT 1  clock+104 written
//       CR15Game vtbl[+0x400](0)  stage 0
//         0x140127440/59/6A   if (slot) CNSProvider::Update(slot)            <-- OUR TICK reads BEAT 1
//         0x1401278FE         CClock::HeartbeatBefore(global) again          BEAT 2  (bit 6 of +0x7AE0)
//         0x140127914 [+0x178] CncaGame::UpdateEngine(0)
//                              -> CEngine::HeartbeatBefore -> the same clock  BEAT 3
//       CR15Game vtbl[+0x400](1..3)  simulation, then the game's own ImGui in stage 3
//       CR15Game vtbl[+0x1C0]  render -> inc qword [CncaGame+0x830]          frame counter
//
//   ★ THREE beats per frame, and TWO of them run AFTER us. Every beat recomputes clock+104 as the
//   scaled time since the PREVIOUS beat, and every beat does clock+0x28 += clock+104. So:
//
//     scaledDeltaUs   is beat 1 alone -- the interval from beat 3 of the previous frame to beat 1 of
//                     this one. A real, contiguous interval, but a FRACTION of the frame, and not
//                     aligned to frame boundaries. Integrating it under-reports elapsed time.
//     simTimeUs       counts every beat, so it is the engine's authoritative sim clock.
//     frameDeltaUs    simTimeUs minus its value at our previous tick = beat2(N-1) + beat3(N-1) +
//                     beat1(N), which is exactly one frame of scaled sim time, beat-1 to beat-1.
//
//   ★ frameDeltaUs IS THE FRAME DELTA. scaledDeltaUs is not. Capture, playback and the transport
//   must use frameDeltaUs or difference simTimeUs themselves; scaledDeltaUs is kept because it is
//   what the engine itself acts on at stage 0 and discarding a measured value would be worse.
//
//   What is unchanged: we still run before all gameplay simulation and before the game's own UI emit
//   point, and the frame counter still increments once per frame after us -- which is what makes
//   "exactly one tick per rendered frame" a checkable statement rather than a hope.

#include <stdint.h>

// ★ Everything here is in `namespace replay`. Architect ruling 2026-08-28, over the whole of Appendix B
// rather than case by case: a global ILog / IConfig / IEntity / ITick in a DLL that shares a process
// with the engine AND with rlay.dll collides at link time in somebody else's translation unit.
// bootstrap/services.h forward-declares TickInfo and ITick inside this namespace, so this is where they
// have to be for ReplayServices::tick to name the same type W1-D defines.
namespace replay {

struct TickInfo {
    // CClock + 0x68 exactly as CNSProvider::Update handed it to us. ★ NOT a frame: it is one of at
    // least three heartbeats per frame (see above). Do not integrate it to measure elapsed time.
    uint64_t scaledDeltaUs;
    uint64_t simTimeUs;       // CClock + 0x28, monotonic, accumulates EVERY heartbeat
    uint32_t frameIndex;      // CncaGame + 0x830
    void*    netGame;         // CR15NetGame*, null before state 9
    // ★ simTimeUs advance since the previous tick = exactly one frame of scaled sim time. THIS is
    // the per-frame delta every consumer wants. Zero on the very first tick, which has no previous.
    // Appended to B.3 by W1-D after the audit found the three-beat behaviour; no Wave-2 consumer
    // existed yet, and the alternative was shipping a field whose documented meaning was false.
    uint64_t frameDeltaUs;
};
using TickFn = void (*)(const TickInfo&, void* ctx);

class ITick {
public:
    // Top of stage 0: before gameplay simulation and before the game's own ImGui emit point, with the
    // engine's own scaled delta. Registration is allowed before a tick source is live; callbacks simply
    // start firing when one is. False only when the callback table is full.
    virtual bool RegisterEarly(TickFn, void* ctx) = 0;

    // After the sim stages, before Render. W1-D ships no late source -- the CTimerManager route
    // is a named alternate that this packet did not build -- so this ALWAYS returns
    // false today, which B.3 explicitly permits.
    virtual bool RegisterLate(TickFn, void* ctx) = 0;

    // "slot:0x8500" | "slot:0x8510" | "hook:0x1BBDB0" | "none". Stable for the process lifetime once a
    // source is live; "none" until then, and permanently if both implementations refused.
    virtual const char* Mode() const = 0;

protected:
    // Not in B.3's text; mirrors what W1-C did for ILog and IConfig. The instance has static storage
    // duration and is never deleted, and this makes deleting through the base a compile error rather
    // than a rule someone has to remember.
    ~ITick() = default;
};

namespace tick {

// Which implementation is live. Reported by Mode() and by the probe's done.json.
enum class Source : uint8_t {
    None,      // no source yet, or both refused
    Slot,      // the CNSProvider slot claim -- zero hooks, zero patched bytes
    Hook,      // the CR15NetGame::Update detour, stage-index filtered
};

// Why a slot claim ended the way it did. Every abort reason the design names has its own
// value, because "it fell back" is not a diagnosis.
enum class ClaimResult : uint8_t {
    Pending,             // still waiting for the engine
    Claimed,
    AbortNoGame,         // the CR15Game singleton never appeared
    AbortNoProviders,    // CR15Game+0x8508 never filled -- the provider block did not run at all
    AbortNoFrames,       // the frame counter never advanced twice
    AbortBothFree,       // both 0x8500 and 0x8510 null: not the two-of-three shape we verified
    AbortBothTaken,      // both occupied: somebody else is here, or the model is wrong
    AbortRace,           // the compare-and-swap lost, or the read-back did not hold our handle
    AbortDisabled,       // tick.mode = hook
};

const char* SourceName(Source);
const char* ClaimResultName(ClaimResult);

// Diagnostics. Read by tests/tick, by the probe, and by the startup log; never on the tick path.
struct Stats {
    uint64_t    ticks;            // calls delivered to registered callbacks
    uint64_t    rawCalls;         // calls that reached us, including any we filtered out
    uint64_t    firstFrameIndex;  // CncaGame+0x830 as seen at the first tick
    uint64_t    lastFrameIndex;   // ... and at the most recent one
    uint64_t    duplicateFrames;  // ticks whose frame index equalled the previous tick's ★ must be 0
    uint64_t    skippedFrames;    // frames the counter advanced by more than one ★ must be 0
    uint64_t    zeroDeltas;
    uint64_t    simRegressions;   // CClock+0x28 went backwards; must stay 0
    uint64_t    deltaSumUs;
    Source      source;
    ClaimResult claim;
    uint32_t    slotOffset;       // the offset actually claimed, 0 if none
};

void GetStats(Stats* out);

// The engine's own numbers, read straight from the clock, for a probe that wants to compare what it was
// handed against what the engine holds. Returns false when the engine is not reachable yet.
bool ReadEngineClock(uint64_t* scaledDeltaUs, uint64_t* simTimeUs, uint64_t* frameIndex);

}  // namespace tick
}  // namespace replay
