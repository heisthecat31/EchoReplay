#pragma once

// The world re-origin shift, read once per tick so a keyframe can state the coordinate frame its
// chapter was recorded in.
//
// Echo VR recentres the floating origin when the local position passes 100 m
// (comiss against 100.0f at 0x140127DF6). Everything recorded after that instant is in a shifted
// frame, and playback publishing it into an unshifted world puts the match in the wrong place with
// nothing failing. Recording the offset means the runtime question "does Arena ever cross 100 m"
// does not have to be answered: if it never re-origins the field is a constant and entropy-codes to
// nothing; if it does, the file is correct.
//
// ★ THE ADDRESS IS NOT THE ONE W2-B R5 ASKED FOR, and that is the whole point of docs/spikes/REORIGIN.md.
// R5 named 0x1420A27C0, which is NRadEngine::C3Vector::zero -- 1187 reads and ZERO writes image-wide,
// sitting past the raw end of .data so it is loader-zeroed and permanently (0,0,0). Recording it would
// have put (0,0,0) in every keyframe forever while asserting the world never moved. The real base is
// WORLD.REORIGIN_GLOBAL, and it is a POINTER that must be dereferenced, not a vector.

#include <stdint.h>

namespace replay {
namespace capture {

struct WorldOriginSample {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // The pointer chain resolved and three floats were read. False means "not yet booted or not this
    // build" -- which is a state the file records as such, never as (0,0,0).
    bool valid = false;
    // The read may not be a single instant. See ReadWorldOrigin.
    bool suspect = false;
};

// Reads the CUMULATIVE triple. Cheap: two pointer derefs and seven loads, no allocation, no lock.
WorldOriginSample ReadWorldOrigin();

}  // namespace capture
}  // namespace replay
