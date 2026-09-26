#pragma once

// The buffer that holds identity traffic seen BEFORE the capture file exists.
//
// It is a type rather than four fields beside a memcpy because it carries an invariant that a comment
// could not keep: SHEDDING THE BYTES MUST NEVER SHED THE COUNT OF WHAT WAS SHED. The version this
// replaces cleared its record counter alongside the buffer, so an overflow erased its own evidence --
// the manifest log then under-reported what it had walked, and a real loss was indistinguishable from
// a lobby that had sent nothing. Here the dropped totals are private and no member can lower them.

#include <stdint.h>
#include <string.h>

#include <vector>

namespace replay {
namespace capture {

class PreOpenBuffer {
public:
    // S5.4.1's identity messages are hundreds of bytes each; a megabyte is a long lobby, not a normal
    // one. The cap exists so a client left sitting in a lobby cannot grow this without bound.
    static constexpr size_t kCapBytes = 1u << 20;

    // Appends one framed record. If it would cross the cap, the WHOLE buffer is shed first: the
    // records are variable width and self-delimiting only in sequence, so evicting a prefix means
    // re-walking from the front, and shedding everything keeps the NEWEST identity state -- which is
    // the state the file is about to open with.
    void Push(const void* rec, uint32_t bytes) {
        if (bytes == 0) {
            return;
        }
        if (bytes_.size() + bytes > kCapBytes) {
            droppedRecords_ += records_;
            droppedBytes_ += static_cast<uint32_t>(bytes_.size());
            ++sheds_;
            bytes_.clear();
            records_ = 0;
        }
        const size_t at = bytes_.size();
        bytes_.resize(at + bytes);
        memcpy(bytes_.data() + at, rec, bytes);
        ++records_;
    }

    // Drops what is held, having handed it on. The dropped totals SURVIVE this too -- they are read
    // after the flush, when the file finally exists to record the loss in.
    void Drain() {
        bytes_.clear();
        records_ = 0;
    }

    const uint8_t* Data() const { return bytes_.data(); }
    size_t Size() const { return bytes_.size(); }
    uint32_t Records() const { return records_; }

    uint32_t DroppedRecords() const { return droppedRecords_; }
    uint32_t DroppedBytes() const { return droppedBytes_; }
    uint32_t Sheds() const { return sheds_; }

private:
    std::vector<uint8_t> bytes_;
    uint32_t records_ = 0;
    uint32_t droppedRecords_ = 0;
    uint32_t droppedBytes_ = 0;
    uint32_t sheds_ = 0;
};

}  // namespace capture
}  // namespace replay
