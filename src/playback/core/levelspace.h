#pragma once
// The identity of the level space the plugin may touch on this update. Every handle cached into a
// space (a script graph, a disc pointer) is taken under this key and dropped the moment it changes:
// a level cycle or a load funnel's level change builds a fresh space whose graphs sit at new
// addresses, while the old instances can still pass a plausibility check after they are freed.

#include <stdint.h>

namespace replay::levelspace {

// 0 means "no space": a level cycle holds the load, NetGame is not in game, or the entity has no
// bound space. Otherwise the entity's bind epoch, which advances on every new space binding,
// offset so that epoch 0 is still a real key.
inline uint64_t Key(bool cycleHolds, bool inGame, bool bound, uint64_t bindEpoch) {
    if (cycleHolds || !inGame || !bound) return 0;
    return bindEpoch + 1;
}

}  // namespace replay::levelspace
