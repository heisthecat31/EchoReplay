#pragma once

#include <stdint.h>

#include <atomic>

namespace replay {

// Lock-free SPSC byte ring carrying length-framed records (design F-5, S6.1).
//
// The producer is the game tick. Push allocates nothing, takes no lock, makes no syscall and never
// blocks: when the ring is full it refuses the record whole and counts it. A refused record is a
// completeness loss, so the caller MUST turn Dropped* into a DROPPED record in the file (E-C10) --
// the ring counts, it does not decide.
//
// E-C10 also asks for oldest-first eviction. That is deliberately NOT here: reclaiming the oldest
// record means moving the consumer's tail, and a producer that writes the consumer's index is no
// longer SPSC. Eviction belongs to the chapter builder on the worker side, which owns both ends of
// its own buffer.
// The two ends run on different threads at 90 Hz, so head_ and tail_ get their own cache lines. C4324
// says the struct grew because of that, which is the point.
#pragma warning(push)
#pragma warning(disable : 4324)
class SpscRing {
public:
    // `memory` is caller-owned and must outlive the ring; `bytes` must be a power of two >= 64.
    // Nothing here allocates, so a session can size the ring once and never touch the heap again.
    bool Init(void* memory, uint32_t bytes) noexcept;

    // Both ends must be quiesced. Not safe to call while either side is running.
    void Reset() noexcept;

    uint32_t Capacity() const noexcept { return capacity_; }

    // ---- producer ------------------------------------------------------------------------
    bool Push(const void* data, uint32_t bytes) noexcept;
    uint64_t DroppedRecords() const noexcept { return droppedRecords_.load(std::memory_order_relaxed); }
    uint64_t DroppedBytes() const noexcept { return droppedBytes_.load(std::memory_order_relaxed); }
    uint64_t PushedRecords() const noexcept { return pushedRecords_.load(std::memory_order_relaxed); }

    // ---- consumer ------------------------------------------------------------------------
    // 0 when empty. The value is the payload size of the next record, excluding the length frame.
    uint32_t PeekSize() const noexcept;

    // Returns false when the ring is empty (*outBytes == 0) or when `cap` is too small for the next
    // record (*outBytes == its size, and the record stays queued).
    bool Pop(void* dst, uint32_t cap, uint32_t* outBytes) noexcept;

    uint32_t BytesUsed() const noexcept;

private:
    void WriteWrapped(uint32_t at, const void* src, uint32_t n) noexcept;
    void ReadWrapped(uint32_t at, void* dst, uint32_t n) const noexcept;

    static constexpr uint32_t kFrameBytes = 4;
    static constexpr size_t kCacheLine = 64;

    uint8_t* buffer_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;

    // head_ and tail_ are monotonic counters masked on use, so a full ring is head - tail == capacity
    // and no slot is wasted to disambiguate full from empty.
    alignas(kCacheLine) std::atomic<uint32_t> head_{0};
    alignas(kCacheLine) std::atomic<uint32_t> tail_{0};

    alignas(kCacheLine) std::atomic<uint64_t> droppedRecords_{0};
    std::atomic<uint64_t> droppedBytes_{0};
    std::atomic<uint64_t> pushedRecords_{0};
};
#pragma warning(pop)

}  // namespace replay
