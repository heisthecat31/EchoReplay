#pragma once

// A set of SPSC rings, one per producing thread, drained by the single capture worker.
//
// ---------------------------------------------------------------------------------------------
// ★ WHY THIS IS NOT ONE RING.
//
// S6.1 draws one lock-free SPSC ring from the game tick to the worker, and for the tick that is exactly
// right. But two of the three producers are NOT the tick: W1-L's BroadcasterListen callbacks deliver
// netvars and events, and W1-L's own report lists "thread identity -- log GetCurrentThreadId() in the
// callback against the tick's" as an OPEN in-vivo question (docs/spikes/W1-L.md S9 check 6). Pushing
// two threads into one SPSC ring is not a race that shows up as a dropped record; it is a torn write
// that produces a plausible-looking corrupt one.
//
// Assuming the answer would be the cheapest possible way to corrupt a capture, so this does not assume
// it. Each producing thread CLAIMS a ring the first time it pushes, with one compare-exchange on a slot
// table, and is single-producer on it forever after. The worker drains all claimed rings and merges by
// the records' own stamps, which the reader stable-sorts anyway (E-C8).
//
// The claim also MEASURES the thing W1-L could not: ThreadIds() reports which threads actually produced,
// so "the tap fires on the tick thread" stops being an assumption and becomes a line in the report.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include <atomic>

#include "foundation/ring/ring.h"

namespace replay {
namespace capture {

// Four is the shape the design predicts -- tick, message bus, and room for the voice path -- and the
// fifth is an event, not an error: a claim that cannot be satisfied is counted and reported, and the
// record it could not carry becomes a DROPPED record rather than a silent loss.
constexpr uint32_t kMaxProducers = 4;

class RingSet {
public:
    // `memory` is caller-owned and split evenly across the rings; `bytes` must be a power of two at
    // least 64 * kMaxProducers. Nothing here allocates, so a session sizes the ring once at open and
    // the tick thread never touches the heap again (S6.4 enforcement 1).
    bool Init(void* memory, uint32_t bytes);
    void Reset();

    // Producer side. Safe from any thread; the first push from a thread claims a ring.
    bool Push(const void* data, uint32_t bytes);

    // Consumer side. Drains at most `maxRecords` across all rings into `dst`, oldest ring first.
    // Returns the number of bytes written and sets `records`.
    uint32_t Pop(void* dst, uint32_t cap, uint32_t* outBytes);

    // Round-robins the rings so one busy producer cannot starve another.
    uint32_t Drain(void* scratch, uint32_t scratchBytes, void (*sink)(const void*, uint32_t, void*),
                   void* ctx, uint32_t maxRecords);

    uint32_t ProducerCount() const { return claimed_.load(std::memory_order_acquire); }
    uint32_t ThreadIdAt(uint32_t i) const;
    uint64_t DroppedRecords() const;
    uint64_t DroppedBytes() const;
    uint64_t PushedRecords() const;
    uint64_t ClaimFailures() const { return claimFailures_.load(std::memory_order_relaxed); }
    uint32_t BytesUsed() const;

private:
    int ClaimIndex();

    SpscRing rings_[kMaxProducers];
    std::atomic<uint32_t> owner_[kMaxProducers] = {};   // thread id, 0 = unclaimed
    std::atomic<uint32_t> claimed_{0};
    std::atomic<uint64_t> claimFailures_{0};
    uint32_t next_ = 0;
    bool ready_ = false;
};

}  // namespace capture
}  // namespace replay
