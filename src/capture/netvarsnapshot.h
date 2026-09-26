#pragma once

// SPEC-RECORDER-2 S2.3 / RECORDER-4. THE LIVE HALF OF THE KEYFRAME NETVAR SNAPSHOT.
//
// The recorder's netvar tap is a listener on the DELTA message, so a var that never changes during a
// capture is in no record and in no keyframe. Playback reads value-at-T0 for the HUD keys out of the
// keyframe netvar block (NetVarPrimer, playback/core/windowbuild.h), so without a snapshot the
// scoreboard and both clocks open on the offline host's own residual numbers until the first change
// -- and in a capture with no goal, forever.
//
// ★ THREADS. ChapterBuilder runs on the WORKER, and the engine's netvar pool belongs to the game
// thread: calling a getter from the worker would race the store's own insertion. So the engine reads
// happen on the CAPTURE TICK -- SampleOnTick -- and the worker's keyframe only copies the qwords
// that tick left behind. That also puts the cost where the cost meter measures it.
//
// ★ NOT gated by livesafety. That predicate gates the writes this plugin AUTHORS, and capture
// exists to run in exactly the session it refuses (capture.cpp's session gate requires
// LiveOnlineExempt). Four typed getters -- Real, Int, Symbol and, since the phase flags, Flag --
// each bound and prologue-checked by gamestate_live, over a route the playback side validates
// structurally every tick.
//
// "Read-only" is the caller's intent, not the engine's behaviour, and the difference is worth
// stating: all four getters lazily quicksort the store's own key array in place and clear its
// dirty bit (echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05, sub_1400D1660 from each getter,
// four sites in each). That is the engine reordering its own index on the thread that owns it --
// the same thing the game's own HUD read does every frame -- and it is identical across all four
// kinds, so the Flag keys add no class of engine contact the shipped Int/Float/Symbol keys did not
// already make. Nothing here writes a VALUE the game will read back as gameplay state.

#include <stdint.h>

#include "capture/chapterbuilder.h"

namespace replay {
namespace capture {

// What one sample found, for the log and for the report.
struct NetvarSnapshotStats {
    uint32_t samples = 0;       // SampleOnTick calls that actually read the engine
    uint32_t routeRefused = 0;  // of those, the netvar route was not valid: nothing read
    uint32_t keysRead = 0;      // keys a getter returned a value for
    uint32_t keysFaulted = 0;   // keys whose getter raised; the key is reported unreadable
    // Bit i: key i answered at least once in this capture. A key that never answers keeps the
    // warm-up cadence for the whole match, so this is also the number that says WHY the sampler
    // never settled -- a wrong symbol and a match that simply never set the key look the same in
    // `keysRead`, and only this tells them apart at close.
    uint32_t everMask = 0;
    uint16_t keysNeverRead = 0;
};

// Filled on the capture tick, read by the worker's keyframe. One writer, one reader.
class NetvarSnapshotCache : public INetvarSnapshotSource {
public:
    // GAME THREAD. Reads every HUD key through the engine's typed getters. A `netGame` of null, an
    // unbound getter table or a netvar route that is not valid yet (a fresh spectate join, before
    // the component is filled) all return with the last sample untouched.
    //
    // CADENCE: `minIntervalUs` is the SETTLED cadence and applies only once every key has answered
    // once. Until then the sampler warms up -- every tick for two seconds, then four times a second
    // -- and a refusal costs the warm-up interval rather than the settled one. Without that, the
    // first sample of a capture landing one frame before the netvar component is filled left the
    // file with no HUD state at all until the second keyframe.
    void SampleOnTick(void* netGame, uint64_t nowUs, uint64_t minIntervalUs);

    // WORKER THREAD. Copies the freshest complete sample.
    uint16_t Snapshot(NetvarSnapshotRow* out, uint16_t cap, uint16_t& unreadable) override;

    // Per file. The next tick samples immediately.
    void Reset();

    NetvarSnapshotStats Stats() const;

    // The key list, which is playback's HUD table verbatim -- see the .cpp. Exposed so a report can
    // name the keys without a second copy of them.
    static uint16_t KeyCount();
    static const char* KeyName(uint16_t i);
};

// RECORDER-5 F3. The first snapshot of the 09-19 capture read 0 of 9 keys -- a fresh spectate join
// has not filled the netvar component one frame after the session gate fires -- and the NEXT one was
// a whole keyframe interval later, because the interval was committed before the route was even
// checked. So a viewer seeking to T+0 had no HUD at all until T+2.0 s.
//
// Until every key has answered once: every tick for the first two seconds, then four times a second.
// After that the caller's keyframe cadence, which is what this costs for the rest of the match. One
// sample is ~40 us on the live build, so the whole warm-up is a few milliseconds, once per capture.
constexpr uint64_t kNetvarWarmEveryTickUntilUs = 2000000u;
constexpr uint64_t kNetvarWarmIntervalUs = 250000u;

// THE CADENCE RULE, as a function rather than three lines inside SampleOnTick, because SampleOnTick
// needs a live engine and this needs nothing. Returns how long after `nowUs` the next sample is due;
// zero means the next tick.
inline uint64_t NetvarSnapshotIntervalUs(bool everyKeyAnswered, uint64_t sinceWarmStartUs,
                                         uint64_t settledIntervalUs) {
    if (everyKeyAnswered) {
        return settledIntervalUs;
    }
    return sinceWarmStartUs >= kNetvarWarmEveryTickUntilUs ? kNetvarWarmIntervalUs : 0;
}

// The process-wide cache the capture tick fills and the builder reads.
NetvarSnapshotCache& LiveNetvarSnapshot();

}  // namespace capture
}  // namespace replay
