#include "control/cmdqueue.h"

namespace replay {
namespace control {

// The bounded MPSC ring is the standard sequence-number design: each slot carries a counter that
// says whose turn it is, so a producer can claim a slot with one CAS and publish with one release
// store, and a consumer can tell "empty" from "a producer has claimed this slot but has not filled
// it yet". The distinction matters here: without it a consumer could read a slot mid-write and
// execute a half-built command.
//
// slot.seq == pos            -> free, this producer may claim it
// slot.seq == pos + 1        -> filled, the consumer at pos may take it
// anything else              -> another thread is between those two states

CommandQueue::CommandQueue() : enqueue_(0), dequeue_(0), refused_(0) {
    for (size_t i = 0; i < kCapacity; ++i) {
        slots_[i].seq.store(i, std::memory_order_relaxed);
    }
}

bool CommandQueue::Push(const Command& cmd) {
    Slot* slot = nullptr;
    size_t pos = enqueue_.load(std::memory_order_relaxed);
    for (;;) {
        slot = &slots_[pos & (kCapacity - 1)];
        const size_t seq = slot->seq.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
        if (diff == 0) {
            if (enqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else if (diff < 0) {
            // Full. Counted rather than overwritten: dropping the oldest command would execute a
            // sequence the user never typed, which is worse than refusing the newest and saying so.
            refused_.fetch_add(1, std::memory_order_relaxed);
            return false;
        } else {
            pos = enqueue_.load(std::memory_order_relaxed);
        }
    }
    slot->cmd = cmd;
    slot->seq.store(pos + 1, std::memory_order_release);
    return true;
}

bool CommandQueue::Pop(Command* out) {
    if (out == nullptr) {
        return false;
    }
    Slot* slot = nullptr;
    size_t pos = dequeue_.load(std::memory_order_relaxed);
    for (;;) {
        slot = &slots_[pos & (kCapacity - 1)];
        const size_t seq = slot->seq.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
        if (diff == 0) {
            if (dequeue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                break;
            }
        } else if (diff < 0) {
            return false;   // empty, or a producer has claimed this slot and not published yet
        } else {
            pos = dequeue_.load(std::memory_order_relaxed);
        }
    }
    *out = slot->cmd;
    slot->seq.store(pos + kCapacity, std::memory_order_release);
    return true;
}

uint64_t CommandQueue::Refused() const {
    return refused_.load(std::memory_order_relaxed);
}

}  // namespace control
}  // namespace replay
