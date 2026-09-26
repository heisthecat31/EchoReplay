#include "synthengine.h"

#include "engine/wiretap_facts.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace w1l {
namespace {

// Every engine address and every structure offset below comes from wiretap_facts.h, which mirrors
// the W1-L fact drop and asserts against the registry once W0-2 has merged it. They were one
// file's comments until the orchestrator ruled on section 10.9: scaffolding that faults on a
// different build instead of refusing is the expensive kind.
using namespace replay::wiretap_facts;

constexpr uint64_t kHashMapInit = kBroadcasterHashMapInit;  // called on data+0x5E0 by the ctor
constexpr uint64_t kArrayInit   = kBroadcasterArrayInit;    // called on data+0x648 by the ctor
constexpr uint64_t kTlsIndexRef = kAllocatorTlsAccessor;    // mov ecx,[idx]; jmp TlsGetValue

// SBroadcasterData, the fields CBroadcaster::Listen and both dispatchers touch.
constexpr uint32_t kDataAllocator = 0;  // the allocator Listen swaps into TLS for its own calls
constexpr uint32_t kDataHashMap   = kDataHashMapOff;
constexpr uint32_t kDataNodeArray = kDataNodePoolOff;
// A size, not a fact: the constructor touches up to +0xB78 and this is the next power of two above it.
constexpr size_t   kDataBytes     = 0x4000;  // facts-ok: SESSION.FLAG_BIT_ACTIVE is a flag bit, and this is a block size that happens to share the value

// The growable-array header, from EVENT.BROADCASTER_ARRAY_INIT and its push-back.
constexpr uint32_t kArrayBase        = 0;
constexpr uint32_t kArrayAllocator   = kArrayAllocatorOff;
constexpr uint32_t kArrayGranularity = kArrayGranularityOff;
constexpr uint32_t kArrayCapacity    = kArrayCapacityOff;
constexpr uint32_t kArrayCount       = kArrayCountOff;

// CR15NetGame, from sub_140D1C790.
constexpr uint32_t kNetGameLock        = kListenLockOff;
constexpr uint32_t kNetGameBroadcaster = kBroadcasterOff;
// Room for the two fields above and nothing more is read; not a measured structure size.
constexpr size_t   kNetGameBytes       = 0x2A00;

using HashMapInitFn = void(__fastcall*)(void*, const void*);
using ArrayInitFn   = void*(__fastcall*)(void*, uint64_t, uint64_t, void*);

// What sub_1400E9A50 reads: reserve, entries per chunk, allocator. The broadcaster ctor passes
// {0, 0x100, allocator}, and reserve 0 is what keeps the call from touching the chunk
// preallocator -- which is the only other engine path this file would have to satisfy.
struct HashMapDesc {
    uint64_t reserve;
    uint64_t entriesPerChunk;
    void*    allocator;
};

constexpr size_t kAlign = 16;

EngineAllocator* gAllocator = nullptr;

void CountAlloc(size_t bytes) {
    if (gAllocator != nullptr) {
        gAllocator->liveBytes += bytes;
        ++gAllocator->allocations;
    }
}

void* __fastcall AllocSlot(void*, uint64_t size) {
    CountAlloc(size);
    return _aligned_malloc(size, kAlign);
}

void* __fastcall ReallocSlot(void*, void* ptr, uint64_t size) {
    CountAlloc(size);
    return _aligned_realloc(ptr, size, kAlign);
}

void* __fastcall AllocAlignedSlot(void*, uint64_t align, uint64_t size) {
    CountAlloc(size);
    return _aligned_malloc(size, align < kAlign ? kAlign : align);
}

void __fastcall FreeSlot(void*, void* ptr) {
    // sub_1400D8D40 hands the old pointer over unconditionally, and an array that has never
    // allocated carries the -1 sentinel sub_1400D1F30 wrote into it.
    if (ptr == nullptr || ptr == reinterpret_cast<void*>(~uintptr_t{0})) {
        return;
    }
    if (gAllocator != nullptr) {
        ++gAllocator->frees;
    }
    _aligned_free(ptr);
}

// Any vtable slot the engine reaches for that W1-L has not identified. Dying here names the
// slot; returning something plausible would let the offline host report a verdict about a code
// path nobody has read.
template <int Slot>
void* __fastcall TrapSlot(void*, uint64_t, uint64_t, uint64_t) {
    std::fprintf(stderr, "SYNTH-ALLOCATOR-TRAP vtable slot +0x%02X\n", Slot * 8);
    std::fflush(stderr);
    ExitProcess(91);
}

void* gVtable[16] = {
    reinterpret_cast<void*>(&TrapSlot<0>),  reinterpret_cast<void*>(&TrapSlot<1>),
    reinterpret_cast<void*>(&TrapSlot<2>),  reinterpret_cast<void*>(&TrapSlot<3>),
    reinterpret_cast<void*>(&TrapSlot<4>),  reinterpret_cast<void*>(&TrapSlot<5>),
    reinterpret_cast<void*>(&TrapSlot<6>),  reinterpret_cast<void*>(&TrapSlot<7>),
    reinterpret_cast<void*>(&TrapSlot<8>),  reinterpret_cast<void*>(&TrapSlot<9>),
    reinterpret_cast<void*>(&TrapSlot<10>), reinterpret_cast<void*>(&TrapSlot<11>),
    reinterpret_cast<void*>(&TrapSlot<12>), reinterpret_cast<void*>(&TrapSlot<13>),
    reinterpret_cast<void*>(&TrapSlot<14>), reinterpret_cast<void*>(&TrapSlot<15>),
};

uint8_t* DataOf(void* broadcaster) {
    return *reinterpret_cast<uint8_t**>(broadcaster);
}

}  // namespace

bool SynthNetGame::Build(const EngineImage& image, std::string& error) {
    gVtable[kAllocVtAlloc / sizeof(void*)] = reinterpret_cast<void*>(&AllocSlot);
    gVtable[kAllocVtRealloc / sizeof(void*)] = reinterpret_cast<void*>(&ReallocSlot);
    gVtable[kAllocVtAllocAligned / sizeof(void*)] = reinterpret_cast<void*>(&AllocAlignedSlot);
    gVtable[kAllocVtFree / sizeof(void*)] = reinterpret_cast<void*>(&FreeSlot);
    allocator_.vtable = gVtable;
    gAllocator = &allocator_;

    // The engine reads its allocator context out of a TLS slot whose index lives in a global
    // that its own startup fills in. Nothing here runs that startup, so the index is claimed
    // now and written where the engine looks, rather than left as whatever the file holds.
    const auto* ref = image.At<const uint8_t*>(kTlsIndexRef);
    if (ref[0] != 0x8B || ref[1] != 0x0D) {
        error = "the allocator TLS accessor is not the 'mov ecx,[rip+d]' W1-L read";
        return false;
    }
    int32_t rel = 0;
    std::memcpy(&rel, ref + 2, sizeof rel);
    auto* tlsIndex = reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(ref) + 6 + rel);
    const DWORD slot = TlsAlloc();
    if (slot == TLS_OUT_OF_INDEXES) {
        error = "no TLS slot available for the engine allocator context";
        return false;
    }
    *tlsIndex = slot;
    TlsSetValue(slot, &allocator_);

    for (int i = 0; i < 2; ++i) {
        if (!BuildBroadcaster(image, i, error) || !BuildNetGame(i, error)) {
            return false;
        }
    }
    return true;
}

bool SynthNetGame::BuildNetGame(int which, std::string& error) {
    auto* netGame = static_cast<uint8_t*>(_aligned_malloc(kNetGameBytes, 64));
    auto* lock = static_cast<long*>(_aligned_malloc(sizeof(long), 64));
    if (netGame == nullptr || lock == nullptr) {
        error = "cannot allocate the synthetic CR15NetGame";
        return false;
    }
    std::memset(netGame, 0, kNetGameBytes);
    *lock = 0;
    *reinterpret_cast<void**>(netGame + kNetGameLock) = lock;
    *reinterpret_cast<void**>(netGame + kNetGameBroadcaster) = broadcaster_[which];
    netGame_[which] = netGame;
    return true;
}

bool SynthNetGame::BuildBroadcaster(const EngineImage& image, int which, std::string& error) {
    auto* data = static_cast<uint8_t*>(_aligned_malloc(kDataBytes, 64));
    if (data == nullptr) {
        error = "cannot allocate SBroadcasterData";
        return false;
    }
    std::memset(data, 0, kDataBytes);
    data_[which] = data;

    *reinterpret_cast<void**>(data + kDataAllocator) = &allocator_;

    const HashMapDesc desc{0, 0x100, &allocator_};
    image.At<HashMapInitFn>(kHashMapInit)(data + kDataHashMap, &desc);

    // A hash map that came back without its bucket array would make every later lookup read
    // through a null pointer, and the first symptom would be a fault inside Listen.
    if (*reinterpret_cast<void**>(data + kDataHashMap) == nullptr) {
        error = "the engine hash map initialiser produced no bucket array";
        return false;
    }

    image.At<ArrayInitFn>(kArrayInit)(data + kDataNodeArray, 0, 0, &allocator_);
    *reinterpret_cast<uint64_t*>(data + kDataNodeArray + kArrayGranularity) = 0x20;
    *reinterpret_cast<uint64_t*>(data + kDataNodeArray + kArrayCapacity) = 0;
    *reinterpret_cast<uint64_t*>(data + kDataNodeArray + kArrayCount) = 0;
    if (*reinterpret_cast<void**>(data + kDataNodeArray + kArrayAllocator) != &allocator_) {
        error = "the engine array initialiser did not take the allocator";
        return false;
    }

    auto* broadcaster = static_cast<void**>(_aligned_malloc(sizeof(void*) * 2, 64));
    if (broadcaster == nullptr) {
        error = "cannot allocate CBroadcaster";
        return false;
    }
    broadcaster[0] = data;
    broadcaster[1] = nullptr;
    broadcaster_[which] = broadcaster;
    return true;
}

void** ListenerMemberSlot(void* broadcaster, uint32_t index) {
    uint8_t* pool =
        *reinterpret_cast<uint8_t**>(DataOf(broadcaster) + kDataNodeArray + kArrayBase);
    // node+24 is the member slot: the delegate starts at node+16 and the slot is its second word.
    return reinterpret_cast<void**>(pool + static_cast<size_t>(index) * kNodeStride +
                                    kNodeDelegateOff + sizeof(void*));
}

uint64_t ListenerNodeCount(void* broadcaster) {
    return *reinterpret_cast<const uint64_t*>(DataOf(broadcaster) + kDataNodeArray + kArrayCount);
}

bool ReadListenerNode(void* broadcaster, uint32_t index, ListenerNode& out) {
    if (index >= ListenerNodeCount(broadcaster)) {
        return false;
    }
    const uint8_t* pool =
        *reinterpret_cast<uint8_t* const*>(DataOf(broadcaster) + kDataNodeArray + kArrayBase);
    const uint8_t* node = pool + static_cast<size_t>(index) * kNodeStride;
    // The 48-byte node CBroadcaster::Listen writes: a next index, the flags word nothing reads,
    // the skip word both dispatchers test, the symbol, then the 32-byte delegate verbatim.
    constexpr uint32_t kWord = sizeof(void*);
    std::memcpy(&out.next, node, sizeof out.next);
    std::memcpy(&out.flags, node + sizeof out.next, sizeof out.flags);
    std::memcpy(&out.skip, node + kNodeSkipOff, sizeof out.skip);
    std::memcpy(&out.symbol, node + kWord, sizeof out.symbol);
    const uint8_t* delegate = node + kNodeDelegateOff;
    std::memcpy(&out.context, delegate, kWord);
    std::memcpy(&out.memberSlot, delegate + kWord, kWord);
    std::memcpy(&out.unusedTail, delegate + 2 * kWord, kWord);
    std::memcpy(&out.invoker, delegate + kDelegateProxyOff, kWord);
    return true;
}

}  // namespace w1l
