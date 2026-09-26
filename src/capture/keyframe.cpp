#include "capture/keyframe.h"

#include <atomic>
#include <string.h>

namespace replay {
namespace capture {
namespace {

// One buffer behind a sequence counter, not two buffers behind an index. The writer is the tick
// thread at the pin sample rate and the reader is the chapter worker at keyframe rate, so a
// collision is rare and a bounded retry costs less than a second 27 KiB of static memory. A read
// that keeps colliding returns 0 -- a torn pin block would decode into plausible nonsense.
std::atomic<uint32_t> g_pinSeq{0};   // odd while a publish is in progress
uint32_t g_pinBytes = 0;
uint8_t g_pinBlock[audio::MaxPinBytes()];

}  // namespace

void PublishAudioPins(const void* bytes, uint32_t n) {
    if (bytes == nullptr || n > sizeof g_pinBlock) {
        n = 0;   // a block we cannot hold whole is not published short
    }
    const uint32_t seq = g_pinSeq.load(std::memory_order_relaxed);
    g_pinSeq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_pinBytes = n;
    if (n != 0) {
        memcpy(g_pinBlock, bytes, n);
    }
    std::atomic_thread_fence(std::memory_order_release);
    g_pinSeq.store(seq + 2, std::memory_order_relaxed);
}

uint32_t AudioPinsNow(void* dst, uint32_t cap) {
    if (dst == nullptr) {
        return 0;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t before = g_pinSeq.load(std::memory_order_relaxed);
        if ((before & 1u) != 0) {
            continue;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t n = g_pinBytes;
        if (n != 0 && n <= cap) {
            memcpy(dst, g_pinBlock, n);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_pinSeq.load(std::memory_order_relaxed) == before) {
            return n <= cap ? n : 0;
        }
    }
    return 0;
}

void ResetAudioPinsForTest() {
    g_pinBytes = 0;
    g_pinSeq.store(0, std::memory_order_relaxed);
}

const char* KeyBlockName(uint16_t id) {
    switch (id) {
        case kKeyBlockTick: return "TICK";
        case kKeyBlockPlayerWire: return "PLAYER_WIRE";
        case kKeyBlockPlayerSampled: return "PLAYER_SAMPLED";
        case kKeyBlockBodyWire: return "BODY_WIRE";
        case kKeyBlockBodySampled: return "BODY_SAMPLED";
        case kKeyBlockWorld_RETIRED: return "WORLD(retired)";
        case kKeyBlockNetvar: return "NETVAR";
        case kKeyBlockTapTable: return "TAP_TABLE";
        case kKeyBlockTapRegistration: return "TAP_REGISTRATION";
        case kKeyBlockRoster: return "ROSTER";
        case kKeyBlockWorldOrigin: return "WORLD_ORIGIN";
        case kKeyBlockAudioPins: return "AUDIO_PINS";
        case kKeyBlockParticleRng: return "PARTICLE_RNG (reserved, unwritten)";
        case kKeyBlockAnimPhase: return "ANIM_PHASE (reserved, unwritten)";
        case kKeyBlockTrailState: return "TRAIL_STATE (reserved, unwritten)";
        case kKeyBlockBodyOwnerSleep: return "BODY_OWNER_SLEEP (reserved, unwritten)";
        default: return "unknown";
    }
}

KeyframeBlobWriter::KeyframeBlobWriter(void* dst, size_t cap, uint64_t tUs)
    : dst_(static_cast<uint8_t*>(dst)), cap_(cap) {
    if (!dst_ || cap_ < sizeof(KeyframeHeaderDisk)) {
        ok_ = false;
        return;
    }
    KeyframeHeaderDisk h{};
    h.magic = kKeyframeMagic;
    h.version = kKeyframeVersion;
    h.tUs = tUs;
    memcpy(dst_, &h, sizeof h);
    at_ = sizeof h;
}

void KeyframeBlobWriter::Raw(const void* src, size_t n) {
    if (!ok_) {
        return;
    }
    if (at_ + n > cap_) {
        ok_ = false;
        return;
    }
    memcpy(dst_ + at_, src, n);
    at_ += n;
}

void KeyframeBlobWriter::CloseBlock() {
    if (!blockAt_ || !ok_) {
        return;
    }
    // The length is backfilled rather than predicted: a block whose size had to be known up front
    // would force every producer to count twice and would be wrong the first time somebody forgot.
    KeyBlockHeaderDisk h{};
    memcpy(&h, dst_ + blockAt_, sizeof h);
    h.len = static_cast<uint32_t>(at_ - blockAt_ - sizeof(KeyBlockHeaderDisk));
    memcpy(dst_ + blockAt_, &h, sizeof h);
    blockAt_ = 0;
}

void KeyframeBlobWriter::Block(uint16_t blockId) {
    CloseBlock();
    if (!ok_) {
        return;
    }
    if (at_ + sizeof(KeyBlockHeaderDisk) > cap_) {
        ok_ = false;
        return;
    }
    KeyBlockHeaderDisk h{};
    h.blockId = blockId;
    h.len = 0;
    blockAt_ = at_;
    memcpy(dst_ + at_, &h, sizeof h);
    at_ += sizeof h;
    ++blockCount_;
}

size_t KeyframeBlobWriter::Finish() {
    CloseBlock();
    if (!ok_) {
        return 0;
    }
    KeyframeHeaderDisk h{};
    memcpy(&h, dst_, sizeof h);
    h.blockCount = blockCount_;
    memcpy(dst_, &h, sizeof h);
    return at_;
}

bool KeyframeReader::Open(const void* blob, size_t bytes) {
    p_ = static_cast<const uint8_t*>(blob);
    bytes_ = bytes;
    at_ = 0;
    read_ = 0;
    unknownBlocks_ = 0;
    unknownBytes_ = 0;
    consistent_ = false;
    refusal_ = nullptr;

    if (!p_ || bytes_ < sizeof(KeyframeHeaderDisk)) {
        refusal_ = "keyframe blob is shorter than its header";
        return false;
    }
    KeyframeHeaderDisk h{};
    memcpy(&h, p_, sizeof h);
    if (h.magic != kKeyframeMagic) {
        refusal_ = "keyframe magic does not match";
        return false;
    }
    if (h.version != kKeyframeVersion) {
        // Version 1 was a positional struct with no block table. Reading it with this loop would
        // produce block ids out of pose data, so it is refused by version rather than misparsed.
        refusal_ = "keyframe version is not one this build can read";
        return false;
    }
    version_ = h.version;
    tUs_ = h.tUs;
    blockCount_ = h.blockCount;
    at_ = sizeof h;
    return true;
}

bool KeyframeReader::Next(uint16_t& blockId, const uint8_t*& data, uint32_t& len) {
    if (!p_ || read_ >= blockCount_) {
        // Consistent only if the declared block count landed exactly on the end of the blob. Trailing
        // bytes mean the table and the payload disagree, and a reader that ignored that would be
        // reporting a clean parse of a file it had only partly understood.
        consistent_ = (read_ == blockCount_) && (at_ == bytes_);
        return false;
    }
    if (at_ + sizeof(KeyBlockHeaderDisk) > bytes_) {
        consistent_ = false;
        return false;
    }
    KeyBlockHeaderDisk h{};
    memcpy(&h, p_ + at_, sizeof h);
    at_ += sizeof h;
    if (at_ + h.len > bytes_) {
        consistent_ = false;
        return false;
    }
    blockId = h.blockId;
    data = p_ + at_;
    len = h.len;
    at_ += h.len;
    ++read_;
    return true;
}

void PutTapRegistrationBlock(KeyframeBlobWriter& k, const TapRegistrationBlock& r) {
    k.Block(kKeyBlockTapRegistration);
    k.Put(r.registered);
    k.Put(r.deferred);
    k.Put(r.givenUp);
    k.Put(r.unavailable);
    k.Put(r.excluded);
    k.Put(r.total);
    k.Put(r.tableRows);
    k.Put(r.started);
}

bool ReadTapRegistrationBlock(const uint8_t* data, uint32_t len, TapRegistrationBlock& out) {
    if (!data || len < kTapRegistrationBlockBytes) {
        return false;
    }
    uint16_t v[7] = {};
    memcpy(v, data, sizeof v);
    out.registered = v[0];
    out.deferred = v[1];
    out.givenUp = v[2];
    out.unavailable = v[3];
    out.excluded = v[4];
    out.total = v[5];
    out.tableRows = v[6];
    out.started = data[14];
    // The two identities the block exists to make checkable. A row that does not close is a row a
    // reader must not report as a denominator, so it is refused rather than passed on.
    if (uint32_t(out.registered) + out.deferred + out.givenUp + out.unavailable != out.total) {
        return false;
    }
    return uint32_t(out.total) + out.excluded == out.tableRows;
}

}  // namespace capture
}  // namespace replay
