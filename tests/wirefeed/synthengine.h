#pragma once

// A CR15NetGame and a CBroadcaster built offline, out of the engine's own code.
//
// The containers inside SBroadcasterData are initialised by calling the engine's own
// initialisers in the mapped image, with an allocator this file supplies. Nothing about the
// listener pool or the symbol hash map is reimplemented here, so a listener registered through
// NRadEngine::CBroadcaster::Listen lands in the structure the engine would have built.
//
// The allocator is the seam that makes that possible: the engine reaches every allocation
// through a vtable on an object the containers are handed, and hands nothing else out.

#include <cstddef>
#include <cstdint>
#include <string>

#include "engineimage.h"

namespace w1l {

// Byte offsets into the engine's allocator vtable, read off the container code in
// sub_1400D1F30 and sub_1400D8D40. Anything the engine calls that is not one of these lands on
// a trap that names the slot rather than running.
struct EngineAllocator {
    void*    vtable = nullptr;
    uint64_t liveBytes = 0;
    uint64_t allocations = 0;
    uint64_t frees = 0;
};

class SynthNetGame {
public:
    // `image` must outlive this object. Returns false with `error` set if any engine call
    // needed to build the broadcaster did not behave as W1-L derived.
    bool Build(const EngineImage& image, std::string& error);

    // What TickInfo::netGame would hand the wiretap.
    void* NetGame() const { return netGame_[0]; }
    void* Broadcaster() const { return broadcaster_[0]; }

    // The wrong-bus control. A second CR15NetGame, identical except that the broadcaster slot
    // holds a second, separately built CBroadcaster. A tap registered through it is well
    // formed and holds a valid handle, and never sees a dispatch made through the first --
    // which is what a tap on the TCP bus would have looked like.
    void* RivalNetGame() const { return netGame_[1]; }
    void* RivalBroadcaster() const { return broadcaster_[1]; }

    const EngineAllocator& Allocator() const { return allocator_; }

private:
    bool BuildBroadcaster(const EngineImage& image, int which, std::string& error);
    bool BuildNetGame(int which, std::string& error);

    void*           netGame_[2] = {nullptr, nullptr};
    void*           broadcaster_[2] = {nullptr, nullptr};
    void*           data_[2] = {nullptr, nullptr};
    EngineAllocator allocator_;
};

// One entry in the listener pool, read back out of the structure the engine wrote.
struct ListenerNode {
    uint16_t next;
    uint16_t flags;
    uint16_t skip;
    uint64_t symbol;
    void*    context;
    void*    memberSlot;
    void*    unusedTail;
    void*    invoker;
};

// Reads node `index` out of a broadcaster's pool. Returns false if the pool holds fewer.
bool ReadListenerNode(void* broadcaster, uint32_t index, ListenerNode& out);

// The address of node+24, which is what both dispatchers hand the proxy as its second
// argument -- the slot's address, not its value.
void** ListenerMemberSlot(void* broadcaster, uint32_t index);

// How many nodes the pool holds.
uint64_t ListenerNodeCount(void* broadcaster);

}  // namespace w1l
