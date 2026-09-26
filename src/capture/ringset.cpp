#include "capture/ringset.h"

#include <windows.h>

namespace replay {
namespace capture {

bool RingSet::Init(void* memory, uint32_t bytes) {
    ready_ = false;
    if (!memory || bytes < 64u * kMaxProducers) {
        return false;
    }
    const uint32_t each = bytes / kMaxProducers;
    // SpscRing wants a power of two. Round DOWN, because rounding up would hand it memory the caller
    // did not give us.
    uint32_t pow2 = 64;
    while (pow2 * 2u <= each) {
        pow2 *= 2u;
    }
    uint8_t* p = static_cast<uint8_t*>(memory);
    for (uint32_t i = 0; i < kMaxProducers; ++i) {
        if (!rings_[i].Init(p + size_t(i) * each, pow2)) {
            return false;
        }
        owner_[i].store(0, std::memory_order_relaxed);
    }
    claimed_.store(0, std::memory_order_release);
    claimFailures_.store(0, std::memory_order_relaxed);
    next_ = 0;
    ready_ = true;
    return true;
}

void RingSet::Reset() {
    for (uint32_t i = 0; i < kMaxProducers; ++i) {
        rings_[i].Reset();
        owner_[i].store(0, std::memory_order_relaxed);
    }
    claimed_.store(0, std::memory_order_release);
    claimFailures_.store(0, std::memory_order_relaxed);
    next_ = 0;
}

int RingSet::ClaimIndex() {
    const uint32_t me = GetCurrentThreadId();
    for (uint32_t i = 0; i < kMaxProducers; ++i) {
        if (owner_[i].load(std::memory_order_acquire) == me) {
            return static_cast<int>(i);
        }
    }
    for (uint32_t i = 0; i < kMaxProducers; ++i) {
        uint32_t expected = 0;
        if (owner_[i].compare_exchange_strong(expected, me, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            claimed_.fetch_add(1, std::memory_order_acq_rel);
            return static_cast<int>(i);
        }
        if (expected == me) {
            return static_cast<int>(i);
        }
    }
    claimFailures_.fetch_add(1, std::memory_order_relaxed);
    return -1;
}

bool RingSet::Push(const void* data, uint32_t bytes) {
    if (!ready_) {
        return false;
    }
    const int i = ClaimIndex();
    if (i < 0) {
        return false;
    }
    return rings_[static_cast<uint32_t>(i)].Push(data, bytes);
}

uint32_t RingSet::Pop(void* dst, uint32_t cap, uint32_t* outBytes) {
    *outBytes = 0;
    if (!ready_) {
        return 0;
    }
    for (uint32_t n = 0; n < kMaxProducers; ++n) {
        const uint32_t i = (next_ + n) % kMaxProducers;
        uint32_t got = 0;
        if (rings_[i].Pop(dst, cap, &got)) {
            *outBytes = got;
            next_ = (i + 1) % kMaxProducers;
            return got;
        }
        if (got != 0) {
            // The record is bigger than the caller's buffer and stays queued. Reporting the size lets
            // the caller decide; silently skipping it would be a loss nothing counted.
            *outBytes = got;
            return 0;
        }
    }
    return 0;
}

uint32_t RingSet::Drain(void* scratch, uint32_t scratchBytes,
                        void (*sink)(const void*, uint32_t, void*), void* ctx, uint32_t maxRecords) {
    uint32_t n = 0;
    while (n < maxRecords) {
        uint32_t got = 0;
        const uint32_t bytes = Pop(scratch, scratchBytes, &got);
        if (bytes == 0) {
            break;
        }
        sink(scratch, bytes, ctx);
        ++n;
    }
    return n;
}

uint32_t RingSet::ThreadIdAt(uint32_t i) const {
    return i < kMaxProducers ? owner_[i].load(std::memory_order_acquire) : 0;
}

uint64_t RingSet::DroppedRecords() const {
    uint64_t n = 0;
    for (uint32_t i = 0; i < kMaxProducers; ++i) n += rings_[i].DroppedRecords();
    return n;
}

uint64_t RingSet::DroppedBytes() const {
    uint64_t n = 0;
    for (uint32_t i = 0; i < kMaxProducers; ++i) n += rings_[i].DroppedBytes();
    return n;
}

uint64_t RingSet::PushedRecords() const {
    uint64_t n = 0;
    for (uint32_t i = 0; i < kMaxProducers; ++i) n += rings_[i].PushedRecords();
    return n;
}

uint32_t RingSet::BytesUsed() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxProducers; ++i) n += rings_[i].BytesUsed();
    return n;
}

}  // namespace capture
}  // namespace replay
