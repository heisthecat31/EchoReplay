#include "foundation/ring/ring.h"

#include <string.h>

namespace replay {

bool SpscRing::Init(void* memory, uint32_t bytes) noexcept {
    if (!memory || bytes < 64 || (bytes & (bytes - 1)) != 0) {
        return false;
    }
    buffer_ = static_cast<uint8_t*>(memory);
    capacity_ = bytes;
    mask_ = bytes - 1;
    Reset();
    return true;
}

void SpscRing::Reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    droppedRecords_.store(0, std::memory_order_relaxed);
    droppedBytes_.store(0, std::memory_order_relaxed);
    pushedRecords_.store(0, std::memory_order_relaxed);
}

void SpscRing::WriteWrapped(uint32_t at, const void* src, uint32_t n) noexcept {
    const uint32_t start = at & mask_;
    const uint32_t first = (capacity_ - start < n) ? capacity_ - start : n;
    memcpy(buffer_ + start, src, first);
    if (n > first) {
        memcpy(buffer_, static_cast<const uint8_t*>(src) + first, n - first);
    }
}

void SpscRing::ReadWrapped(uint32_t at, void* dst, uint32_t n) const noexcept {
    const uint32_t start = at & mask_;
    const uint32_t first = (capacity_ - start < n) ? capacity_ - start : n;
    memcpy(dst, buffer_ + start, first);
    if (n > first) {
        memcpy(static_cast<uint8_t*>(dst) + first, buffer_, n - first);
    }
}

bool SpscRing::Push(const void* data, uint32_t bytes) noexcept {
    if (!buffer_ || bytes == 0 || bytes > capacity_ - kFrameBytes) {
        droppedRecords_.store(droppedRecords_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        droppedBytes_.store(droppedBytes_.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
        return false;
    }
    const uint32_t need = bytes + kFrameBytes;
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t used = head - tail_.load(std::memory_order_acquire);
    if (capacity_ - used < need) {
        droppedRecords_.store(droppedRecords_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        droppedBytes_.store(droppedBytes_.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
        return false;
    }
    WriteWrapped(head, &bytes, kFrameBytes);
    WriteWrapped(head + kFrameBytes, data, bytes);
    head_.store(head + need, std::memory_order_release);
    pushedRecords_.store(pushedRecords_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return true;
}

uint32_t SpscRing::PeekSize() const noexcept {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (head_.load(std::memory_order_acquire) == tail) {
        return 0;
    }
    uint32_t len = 0;
    ReadWrapped(tail, &len, kFrameBytes);
    return len;
}

bool SpscRing::Pop(void* dst, uint32_t cap, uint32_t* outBytes) noexcept {
    if (outBytes) {
        *outBytes = 0;
    }
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (head_.load(std::memory_order_acquire) == tail) {
        return false;
    }
    uint32_t len = 0;
    ReadWrapped(tail, &len, kFrameBytes);
    if (outBytes) {
        *outBytes = len;
    }
    if (len > cap) {
        return false;
    }
    ReadWrapped(tail + kFrameBytes, dst, len);
    tail_.store(tail + kFrameBytes + len, std::memory_order_release);
    return true;
}

uint32_t SpscRing::BytesUsed() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

}  // namespace replay
