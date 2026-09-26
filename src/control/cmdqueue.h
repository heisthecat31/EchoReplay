#pragma once

// W2-G. The one queue every control surface pushes into and the tick thread drains.
//
// * WHY A QUEUE AT ALL, and it is not thread-safety theatre. W1-K measured that the engine's HTTP
// pump runs inside CR15Game::Update AFTER all three plugin Update slots have already run, so a
// seek applied inside an HTTP handler lands mid-frame, after our tick computed this frame's state.
// W1-K's rule is explicit: push commands into a queue the transport drains at the top of the next
// tick. Doing that for HTTP and not for the console would give the two surfaces different ordering
// semantics for the same command, so both go through here and there is one answer.
//
// * MULTI-PRODUCER, SINGLE-CONSUMER, LOCK-FREE. The producers are the console reader thread, the
// HTTP handler on the game thread, and the module's own init replaying an ECHOREPLAY_OPEN request.
// The consumer is the tick. A mutex would be defensible for the console but not for the handler:
// W1-K's other rule is that a handler must never block, and a handler blocked on a lock held by
// the console thread would hitch the frame.
//
// * A FULL QUEUE REFUSES AND COUNTS. It never overwrites the oldest command and never drops one
// silently. Sixty-four commands in flight is far past any burst a person or a script produces, so
// reaching the cap means something is wrong, and the count is how that becomes visible instead of
// becoming "the console sometimes ignores me".

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "control/command.h"

namespace replay {
namespace control {

// The alignas below pads the object, which is exactly what it is for. Saying so beats a warning
// every other packet sees in its own build output.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

class CommandQueue {
public:
    // A power of two, because the slot index is a mask rather than a modulo. Fixed rather than
    // configurable: honouring a config key would mean allocating, and there is no burst this does
    // not cover.
    static constexpr size_t kCapacity = 64;

    CommandQueue();

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    // Any thread. False when the queue is full; the command is NOT stored and Refused() advances.
    bool Push(const Command& cmd);

    // The tick thread. False when empty.
    bool Pop(Command* out);

    uint64_t Refused() const;

private:
    struct Slot {
        std::atomic<size_t> seq;
        Command             cmd;
    };

    Slot slots_[kCapacity];
    // Separate cache lines: the producers hammer enqueue_ while the consumer hammers dequeue_, and
    // sharing a line would make every push invalidate the consumer's read for no reason.
    alignas(64) std::atomic<size_t>   enqueue_;
    alignas(64) std::atomic<size_t>   dequeue_;
    alignas(64) std::atomic<uint64_t> refused_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace control
}  // namespace replay
