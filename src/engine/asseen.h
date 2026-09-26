#pragma once

// B.11.1 IAsSeen -- the S_ASSEEN inputs of §5.4.4. Owned by W1-E; consumed by W2-A (capture) and
// W2-B (playback).
//
// Everything here is in `namespace replay`, which is where bootstrap/services.h forward-declares
// IAsSeen. While it lived at global scope it was a DIFFERENT type with the same spelling, so
// ReplayServices::asSeen named a class that was declared and never defined and W2-A could not
// gather S_ASSEEN through it at all. The sample structs are B.11.1's vocabulary and move with it;
// the using-declarations at the foot keep the unqualified spelling valid, naming this same class.
//
// The point of this stream is ruling Q1: a replay can be played as GROUND TRUTH -- every entity at
// the playhead exactly -- or AS SEEN, reproducing the client's own view, which lags remote players
// by a hard-coded 44.44 ms and drops bodies the animation LOD throttle skipped that frame. The
// difference is entirely in these fields, so they are captured per tick and are cheap.
//
// Two fields the design lists are NOT served by this implementation and Read() says so rather than
// filling them with zero: see AsSeenSample::valid below.

#include <stdint.h>

#include "engine/entity.h"   // replay::UserIndex, replay::IEntity

namespace replay {

struct AsSeenSample {                       // one per tick
    uint64_t peerTimeUs;                    // NETGAME.PEERTIME / SRVTIME per authority role
    uint64_t serverTimeEstUs, serverTimeSlew;
    float    renderDelaySec;                // the -0.04444f literal, as applied
    uint32_t lodThrottleEnable, lodPerFrameBudget, lodCursor;

    // Not in B.11.1's text. A bitmask of the fields above that were actually read; anything clear
    // was not available on this build and its value is zero because it is absent, not because it
    // measured zero. §12.0's own observation is that a wrong replay renders happily and silently,
    // and a silently-zero slew would do exactly that.
    uint32_t valid;
};

// Which bits of AsSeenSample::valid mean what.
enum : uint32_t {
    kAsSeenPeerTime    = 1u << 0,
    kAsSeenServerTime  = 1u << 1,
    kAsSeenSlew        = 1u << 2,
    kAsSeenRenderDelay = 1u << 3,
    kAsSeenLod         = 1u << 4,
};

// ★ Only peerTimeUs is sourced on this build. rttMs comes from SR15NetUserPingEstMsg, which is a
// wire message W1-L's tap would have to carry, and occluded / bodyUpdatedThisFrame come from
// SetOccluded and UpdateRemoteBodyCI, neither of which W1-E re-derived on PC. ReadPlayer leaves
// those three at zero and false and returns true only for peerTimeUs; docs/spikes/W1-E.md §9
// records them as the open half of this stream rather than shipping three fields that always read
// the same value and would pass any test that never varied them.
struct AsSeenPerPlayer { uint64_t peerTimeUs; uint16_t rttMs; bool occluded, bodyUpdatedThisFrame; };

class IAsSeen {
public:
    virtual bool Read(AsSeenSample&) const = 0;
    virtual bool ReadPlayer(UserIndex, AsSeenPerPlayer&) const = 0;
    virtual void Apply(const AsSeenSample&) = 0;                 // as-seen playback
    virtual void ApplyPlayer(UserIndex, const AsSeenPerPlayer&) = 0;

protected:
    ~IAsSeen() = default;
};

namespace asseen {

// `netGame` is TickInfo::netGame; `entity` supplies the CR15RemotePlayerCS the LOD triple lives on.
// Either may be null, in which case the corresponding `valid` bits stay clear.
IAsSeen* AsSeenCreate(void* netGame, IEntity* entity);
void     AsSeenDestroy(IAsSeen*);

}  // namespace asseen
}  // namespace replay

// Transitional, exactly as in engine/entity.h: the unqualified spelling for translation units still
// at global scope. One class, two ways to name it.
using replay::AsSeenSample;
using replay::AsSeenPerPlayer;
using replay::IAsSeen;
using replay::kAsSeenPeerTime;
using replay::kAsSeenServerTime;
using replay::kAsSeenSlew;
using replay::kAsSeenRenderDelay;
using replay::kAsSeenLod;
