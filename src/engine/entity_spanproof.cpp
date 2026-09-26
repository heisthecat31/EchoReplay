// The span cache, and the bind generation it keys on.
//
// Both used to sit in entity.cpp's anonymous namespace, which was correct while entity.cpp was the
// only caller. identity_access.cpp's per-entrant scoreboard read is a second caller, and it links
// through replay_identity_access -- an object library that deliberately does NOT carry entity.cpp,
// so that the identity probe DLL does not inherit the entity module's static registrar. Putting the
// definition in its own translation unit is what lets both link edges share ONE implementation and
// ONE generation counter; a copy in either file would be a second answer to "is this span proved".

#include "engine/entity.h"

#include <string.h>
#include <windows.h>

#include "bootstrap/buildguard.h"
#include "generated/facts_generated.h"

namespace {

// The bind generation cannot live on Entity and it cannot live on the pointer. Init's first
// statement is `*this = Entity()` -- deliberately, so a rebuilt game space inherits no field from
// the old one -- and EntityCreate hands back one process-lifetime singleton. It is a property of
// the BINDING, and EntityBind is the only thing that advances it.
uint64_t g_bindEpoch = 0;

// Counts the replay::Readable calls the cache actually made, so a test can tell a cached read from
// a fresh probe: the number is a running total and it is the DIFFERENCE across a tick that means
// anything -- the thing a per-tick budget is written against.
uint64_t g_spanProbes = 0;

// One full user table read at the engine's 60 Hz tick: the largest burst a single accessor
// makes between two ticks of other work, so the re-probe rides after that burst rather than
// inside it. One VirtualQuery per this many reads is the whole cost of not latching.
constexpr uint32_t kMaxCachedReads = facts::ENTITY_USER_SLOT_TABLE_MAX * 60u;

// How many calls a REFUSAL is honoured for. The gather asks each pool slot once a
// tick through that slot's own line, so a call on a refusing line is a tick, and this is one second
// of them at the engine's fixed step: the interval a pool that appears mid-match waits to be seen.
// It is deliberately tiny beside kMaxCachedReads -- a stale success hands back a base that has been
// freed, a stale refusal only hides a base that has arrived -- and it is what turns 64 refusing
// slots from 64 VirtualQuery a tick (~1 ms late in a match) into two.
constexpr uint32_t kReprobeTicks = 1000000u / facts::CLOCK_FIXEDSTEP_30HZ_US;
static_assert(kReprobeTicks > 1u, "a re-probe interval of one call is no cadence at all");

// Where in the interval a line's FIRST refusal puts it. Slots that start refusing on the same tick
// -- every slot of a pool that is not there, which is the whole of ENG-GATHER-COST's case -- would
// otherwise come due on the same tick and pay the 64-syscall tick this cadence exists to remove,
// once a second instead of always. The page number of the base is stable per slot and consecutive
// across a pool, so it spreads a pool's lines evenly with no state and no clock. Only the first
// refusal is staggered: re-staggering on every refusal would give each line its own PERIOD, and a
// line that drew a late phase would re-probe every other tick for the life of the binding.
uint32_t ReprobePhase(const uint8_t* b) {
    return static_cast<uint32_t>((reinterpret_cast<uintptr_t>(b) >> 12) % kReprobeTicks);
}

// STATUS_GUARD_PAGE_VIOLATION. Named here because winnt.h only defines it when WIN32_NO_STATUS is
// unset, and this file must not depend on which header a translation unit reached first. A load
// onto a guard page raises this and neither of the other two codes; without the row an unhandled
// exception on the GAME thread would end the owner's match. Swallowing it is right:
// the load consumes the guard on its way past, so the page is ordinary afterwards and the only
// answer that was ever true is the one replay::Readable gave -- refuse.
constexpr unsigned long kStatusGuardPageViolation = 0x80000001ul;
#ifdef STATUS_GUARD_PAGE_VIOLATION
static_assert(kStatusGuardPageViolation ==
                  static_cast<unsigned long>(STATUS_GUARD_PAGE_VIOLATION),
              "STATUS_GUARD_PAGE_VIOLATION is not 0x80000001 on this SDK");
#endif

int SafeReadFilter(unsigned long code) {
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
            code == kStatusGuardPageViolation)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

namespace replay {
namespace entity {

uint64_t SpanProbeCount() { return g_spanProbes; }

uint64_t BindEpochCounter() { return g_bindEpoch; }

uint32_t SpanProofMaxCachedReads() { return kMaxCachedReads; }

uint32_t SpanProofReprobeTicks() { return kReprobeTicks; }

void BindEpochAdvance() { ++g_bindEpoch; }

// Returns the proved base, or null. Both answers are cached, each on its own cadence: a proof for
// kMaxCachedReads reads, a refusal for kReprobeTicks calls. Neither is a LATCH -- a pool built a
// frame later is picked up one interval on, which is the property "a refusal is never cached" was
// protecting and the one ENG-W1D-4 kept out of the clock cell, at a bounded price instead of a
// syscall per unreadable slot per tick.
const uint8_t* SpanProof::Prove(void* rawBase, size_t bytes) {
    const uint8_t* b = static_cast<const uint8_t*>(rawBase);
#if defined(W1E_MUTATE_SPANPROOF_LATCHED)
    // Mutation control (AUDIT_ARCH21 CONDITION-B6): the pre-fix shape. The proof is honoured for
    // the whole binding, so a pool freed at an unchanged base is handed back for the rest of it.
    if (b == base && bytes <= proved && epoch == g_bindEpoch) return base;
#else
    if (b == base && bytes <= proved && epoch == g_bindEpoch && reads < kMaxCachedReads) {
        ++reads;
        return base;
    }
#endif
#if defined(W1E_MUTATE_SPANPROOF_REFUSAL_LATCHED)
    // Mutation control: the refusal is cached with no cadence, so a span that becomes readable is
    // never seen again through this line.
    if (b == refusedBase && bytes >= refusedBytes && refusedEpoch == g_bindEpoch) return nullptr;
#elif !defined(W1E_MUTATE_SPANPROOF_UNCACHED_REFUSAL)
    // A remembered refusal covers any span from that base at least as long as the one that was
    // refused: replay::Readable answers about the whole range, so a range containing an unreadable
    // one cannot be readable. A SHORTER span is a different question and is asked.
    if (b == refusedBase && bytes >= refusedBytes && refusedEpoch == g_bindEpoch &&
        refusedCalls < kReprobeTicks) {
        ++refusedCalls;
        return nullptr;
    }
#endif
    ++g_spanProbes;
    if (!replay::Readable(b, bytes)) {
        const bool sameLine = (b == refusedBase && refusedEpoch == g_bindEpoch);
        base         = nullptr;
        proved       = 0;
        refusedBase  = b;
        refusedBytes = bytes;
        refusedEpoch = g_bindEpoch;
        refusedCalls = sameLine ? 0u : ReprobePhase(b);
        return nullptr;
    }
    base   = b;
    proved = bytes;
    epoch  = g_bindEpoch;
    reads  = 0;
    // The span is mapped, so whatever this line last refused at this base is no longer true. A
    // wider Prove through the same line would otherwise hit that stale refusal and answer null for
    // memory this call just proved readable.
    refusedBase  = nullptr;
    refusedBytes = 0;
    return base;
}

// POD only, and nothing in this frame needs unwinding -- MSVC forbids __try in a function that
// also carries an object with a destructor, which is why the guarded load is a free function here
// rather than a lambda at each call site.
bool TryLoadBytes(const void* at, void* out, size_t bytes) {
    __try {
        memcpy(out, at, bytes);
    } __except (SafeReadFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

// The same guard on the way out. A proof says the span was mapped when VirtualQuery was asked; the
// STORE is what meets a page that has been freed or turned read-only since, and a fault on the
// game's thread ends the owner's match.
bool TryStoreBytes(void* at, const void* in, size_t bytes) {
    __try {
        memcpy(at, in, bytes);
    } __except (SafeReadFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

}  // namespace entity
}  // namespace replay
