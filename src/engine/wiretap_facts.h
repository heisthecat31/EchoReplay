#pragma once

// Mirrors the W1-L fact drop, which W0-2 merges into facts/facts.tsv.  Once that merge
// has happened, configure with -DWIRETAP_FACTS_MERGED and every value below becomes an
// assertion against the generated registry instead of a second copy of it.  Until then this
// header is the only place W1-L's numbers are written down, and docs/spikes/W1-L.md carries
// the derivation of each one.

#include <cstdint>

namespace replay::wiretap_facts {

inline constexpr uint64_t kImageBase = 0x140000000ull;

// NRadEngine::CBroadcaster, the game-message bus.  NOT the TCP broadcaster at 0x140F81110.
inline constexpr uint64_t kBroadcasterListen      = 0x140F80ED0ull;
inline constexpr uint64_t kBroadcasterUnlisten    = 0x140F8DF20ull;
inline constexpr uint64_t kBroadcasterUnlistenAll = 0x140F8E150ull;

// CMatSym::Hash and the 8x8 GF(2) matrix product it folds with.
inline constexpr uint64_t kMatSymHash      = 0x140107F80ull;
inline constexpr uint64_t kMatSymCombine   = 0x140107FD0ull;
inline constexpr uint64_t kMatSymTable     = 0x1416CF920ull;
inline constexpr uint64_t kMatSymCharTable = 0x1416D0120ull;
inline constexpr uint64_t kMatSymNamespace = 0x6D451003FB4B172Eull;

inline constexpr uint32_t kBroadcasterOff = 0x28C8u;  // CR15NetGame -> CBroadcaster*
inline constexpr uint32_t kListenLockOff  = 0x30u;    // CR15NetGame -> volatile int32*

inline constexpr uint32_t kNodeStride       = 48u;
inline constexpr uint32_t kNodeSkipOff      = 0x04u;
inline constexpr uint32_t kNodeDelegateOff  = 0x10u;
inline constexpr uint32_t kDelegateSize     = 32u;
inline constexpr uint32_t kDelegateProxyOff = 0x18u;

// TypeId(name) computed from the two .rdata tables above.  Never literals in .text.
inline constexpr uint64_t kSymDeltaUnreliableVars = 0x2DED551BABC46DBFull;
inline constexpr uint64_t kSymPlayerStateMsg      = 0xC6BB60E8D9E9A3D3ull;
inline constexpr uint64_t kSymComponentEvent      = 0x6E54D0D298BFFB17ull;

inline constexpr const char* kNameDeltaUnreliableVars = "SR15NetReplicatedDeltaUnreliableVars";
inline constexpr const char* kNamePlayerStateMsg      = "SR15NetPlayerStateMsg";
inline constexpr const char* kNameComponentEvent      = "SR15NetComponentEvent";

// 20 bytes at each entry point, read out of echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05.
// Checked before the first call: a wrong image base or a different build stops here instead
// of jumping into the middle of some other function.
inline constexpr uint8_t kListenPrologue[20] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
    0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55 };
inline constexpr uint8_t kUnlistenPrologue[20] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48,
    0x83, 0xEC, 0x40, 0x0F, 0xB7, 0xF2, 0x4C, 0x8B, 0xF1, 0x48 };
inline constexpr uint8_t kUnlistenAllPrologue[20] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
    0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20 };
inline constexpr uint8_t kMatSymHashPrologue[20] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x39, 0x00, 0x48,
    0x8B, 0xD9, 0x75, 0x10, 0x48, 0xB8, 0x01, 0x02, 0x04, 0x08 };
inline constexpr uint8_t kMatSymCombinePrologue[20] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20 };

// ------------------------------------------------------------------------------------------
// What tests/wirefeed's offline host needs to build a CBroadcaster out of the engine's own code.
//
// These are not used by the shipped DLL: the plugin is handed a broadcaster the engine already
// built. They are here rather than in the test because a test that faults on a different build
// instead of refusing is the expensive kind of scaffolding, and because the registry is where a
// value stops being one file's comment.
// ------------------------------------------------------------------------------------------

inline constexpr uint64_t kBroadcasterHashMapInit = 0x1400E9A50ull;
inline constexpr uint64_t kBroadcasterArrayInit   = 0x1400D1F30ull;
inline constexpr uint64_t kAllocatorTlsAccessor   = 0x1400CE8A0ull;

inline constexpr uint32_t kDataHashMapOff  = 0x5E0u;   // SBroadcasterData -> the symbol hash map
inline constexpr uint32_t kDataNodePoolOff = 0x648u;   // SBroadcasterData -> the 48-byte pool

// The growable-array header both of those initialise.
inline constexpr uint32_t kArrayAllocatorOff   = 0x10u;
inline constexpr uint32_t kArrayGranularityOff = 0x20u;
inline constexpr uint32_t kArrayCapacityOff    = 0x28u;
inline constexpr uint32_t kArrayCountOff       = 0x30u;

// The engine's allocator interface, reached through a vtable on an object the containers hold.
// This is the seam that lets engine container code run in a process with no engine heap.
inline constexpr uint32_t kAllocVtAlloc        = 0x08u;  // Alloc(size)
inline constexpr uint32_t kAllocVtRealloc      = 0x10u;  // Realloc(ptr, size); ptr may be null
inline constexpr uint32_t kAllocVtAllocAligned = 0x28u;  // AllocAligned(align, size)
inline constexpr uint32_t kAllocVtFree         = 0x38u;  // Free(ptr); may be the -1 sentinel

}  // namespace replay::wiretap_facts

#ifdef WIRETAP_FACTS_MERGED
#include "facts_generated.h"
namespace replay::wiretap_facts {
static_assert(kBroadcasterListen == facts::EVENT_BROADCASTER_LISTEN);
static_assert(kBroadcasterUnlisten == facts::EVENT_BROADCASTER_UNLISTEN);
static_assert(kBroadcasterUnlistenAll == facts::EVENT_BROADCASTER_UNLISTEN_ALL);
static_assert(kMatSymHash == facts::EVENT_MATSYM_HASH);
static_assert(kMatSymCombine == facts::EVENT_MATSYM_COMBINE);
static_assert(kMatSymTable == facts::EVENT_MATSYM_TABLE);
static_assert(kMatSymCharTable == facts::EVENT_MATSYM_CHAR_TABLE);
static_assert(kMatSymNamespace == facts::EVENT_MATSYM_NS);
static_assert(kBroadcasterOff == facts::NETGAME_BROADCASTER_OFF);
static_assert(kListenLockOff == facts::NETGAME_LISTENLOCK_OFF);
static_assert(kNodeStride == facts::EVENT_LISTENER_NODE_STRIDE);
static_assert(kNodeSkipOff == facts::EVENT_NODE_SKIP_OFF);
static_assert(kNodeDelegateOff == facts::EVENT_NODE_DELEGATE_OFF);
static_assert(kDelegateSize == facts::EVENT_DELEGATE_SIZE);
static_assert(kDelegateProxyOff == facts::EVENT_DELEGATE_PROXY_OFF);
static_assert(kSymDeltaUnreliableVars == facts::EVENT_SYM_DELTA_UNRELIABLE_VARS);
static_assert(kSymPlayerStateMsg == facts::EVENT_SYM_PLAYER_STATE_MSG);
static_assert(kSymComponentEvent == facts::EVENT_SYM_COMPONENT_EVENT);
static_assert(kBroadcasterHashMapInit == facts::EVENT_BROADCASTER_HASHMAP_INIT);
static_assert(kBroadcasterArrayInit == facts::EVENT_BROADCASTER_ARRAY_INIT);
static_assert(kAllocatorTlsAccessor == facts::EVENT_ALLOCATOR_TLS_ACCESSOR);
static_assert(kDataHashMapOff == facts::EVENT_DATA_HASHMAP_OFF);
static_assert(kDataNodePoolOff == facts::EVENT_DATA_NODEPOOL_OFF);
static_assert(kArrayAllocatorOff == facts::CONTAINER_ARRAY_ALLOCATOR_OFF);
static_assert(kArrayGranularityOff == facts::CONTAINER_ARRAY_GRANULARITY_OFF);
static_assert(kArrayCapacityOff == facts::CONTAINER_ARRAY_CAPACITY_OFF);
static_assert(kArrayCountOff == facts::CONTAINER_ARRAY_COUNT_OFF);
static_assert(kAllocVtAlloc == facts::ALLOC_VT_ALLOC_OFF);
static_assert(kAllocVtRealloc == facts::ALLOC_VT_REALLOC_OFF);
static_assert(kAllocVtAllocAligned == facts::ALLOC_VT_ALLOC_ALIGNED_OFF);
static_assert(kAllocVtFree == facts::ALLOC_VT_FREE_OFF);
}  // namespace replay::wiretap_facts
#endif
