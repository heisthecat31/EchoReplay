#include "audio/voip/voipcapture.h"

#include <atomic>

#include "audio/audio_facts.h"
#include "audio/memprobe.h"
#include "audio/voip/voip_facts.h"
#include "audio/voipformat.h"

namespace replay {
namespace audio {
namespace voip {

const char* const kVoipReceiveFactId = "VOIP.RECEIVE";

namespace {

using ReceiveVoipFn = void(__fastcall*)(void* cs, uint32_t handle, const void* header,
                                        const void* payload, uint32_t length);

ReceiveVoipFn g_trampoline = nullptr;

std::atomic<VoipSink> g_sink{nullptr};
std::atomic<void*> g_sinkCtx{nullptr};

std::atomic<void*> g_latchedCs{nullptr};
std::atomic<uint32_t> g_latchedHandle{0};

struct Counters {
    std::atomic<uint64_t> observed{0};
    std::atomic<uint64_t> stored{0};
    std::atomic<uint64_t> droppedNoSink{0};
    std::atomic<uint64_t> droppedSinkRefused{0};
    std::atomic<uint64_t> droppedOversize{0};
    std::atomic<uint64_t> droppedNoPayload{0};
    std::atomic<uint64_t> droppedNoHeader{0};
    std::atomic<uint64_t> unresolvedSpeaker{0};
    std::atomic<uint32_t> maxPayload{0};
    std::atomic<uint32_t> minPayload{0xFFFFFFFFu};
    std::atomic<uint32_t> maxRefused{0};
};
Counters g;

void RaiseMax(std::atomic<uint32_t>& slot, uint32_t v) {
    uint32_t seen = slot.load(std::memory_order_relaxed);
    while (v > seen && !slot.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
    }
}

void LowerMin(std::atomic<uint32_t>& slot, uint32_t v) {
    uint32_t seen = slot.load(std::memory_order_relaxed);
    while (v < seen && !slot.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
    }
}

const uint8_t* At(const void* base, uint32_t off) {
    return static_cast<const uint8_t*>(base) + off;
}

// The speaker, resolved the way the engine's own dispatcher built the handle in the first place:
// handle low word -> pool entry -> dense index -> CI -> SR15NetUserId. Every hop is guarded,
// because this runs on whatever thread the net layer owns and a torn-down pool is a real state.
uint32_t SpeakerOf(void* cs, uint32_t handle) {
    if (cs == nullptr) {
        return kUnresolvedSpeaker;
    }
    if (!Readable(At(cs, voip_facts::kCsCountOff), sizeof(uint16_t))) {
        return kUnresolvedSpeaker;
    }
    const uint16_t count = *reinterpret_cast<const uint16_t*>(At(cs, voip_facts::kCsCountOff));
    if (count == 0) {
        return kUnresolvedSpeaker;
    }
    if (!Readable(At(cs, voip_facts::kCsPoolTableOff), sizeof(void*)) ||
        !Readable(At(cs, voip_facts::kCsCiBaseOff), sizeof(void*))) {
        return kUnresolvedSpeaker;
    }
    const uint8_t* pool = *reinterpret_cast<const uint8_t* const*>(At(cs, voip_facts::kCsPoolTableOff));
    const uint8_t* base = *reinterpret_cast<const uint8_t* const*>(At(cs, voip_facts::kCsCiBaseOff));
    if (pool == nullptr || base == nullptr) {
        return kUnresolvedSpeaker;
    }
    const uint16_t poolIndex = static_cast<uint16_t>(handle & 0xFFFFu);
    const uint8_t* entry = pool + static_cast<size_t>(voip_facts::kPoolEntryStride) * poolIndex;
    if (!Readable(entry, voip_facts::kPoolEntryStride)) {
        return kUnresolvedSpeaker;
    }
    const uint16_t dense = *reinterpret_cast<const uint16_t*>(entry);
    const uint16_t generation = *reinterpret_cast<const uint16_t*>(entry + voip_facts::kPoolGenOff);
    if (dense >= count) {
        return kUnresolvedSpeaker;
    }
    // ★ THE ENGINE DOES NOT CHECK THIS AND WE DO, ON PURPOSE. VOIP.RECEIVE selects its instance
    // from the handle's low word alone. But the caller built that handle by reading this entry's
    // generation half, so the two agreeing is a property of the layout we believe we have -- and a
    // disagreement means the pool is not shaped the way the W3-C fact drop says. Recording an
    // unresolved speaker there is right: a plausible wrong speaker attributes somebody's voice to
    // somebody else for the life of the file, and no later reader could tell.
    if (generation != static_cast<uint16_t>(handle >> 16)) {
        return kUnresolvedSpeaker;
    }
    const uint8_t* ci = base + static_cast<size_t>(voip_facts::kCiStride) * dense;
    if (!Readable(ci + voip_facts::kCiUserIdOff, sizeof(uint32_t))) {
        return kUnresolvedSpeaker;
    }
    return *reinterpret_cast<const uint32_t*>(ci + voip_facts::kCiUserIdOff);
}

TapVerdict Tap(void* cs, uint32_t handle, const void* header, const void* payload,
               uint32_t length) {
    g.observed.fetch_add(1, std::memory_order_relaxed);
    g_latchedCs.store(cs, std::memory_order_relaxed);
    g_latchedHandle.store(handle, std::memory_order_relaxed);

    if (length > kMaxPayload) {
        // Refused whole, never truncated: a clipped voice frame is a corrupt one, and the bound
        // itself is a claim this packet has parked a measurement for.
        g.droppedOversize.fetch_add(1, std::memory_order_relaxed);
        RaiseMax(g.maxRefused, length);
        return TapVerdict::Oversize;
    }
    if (length != 0 && !Readable(payload, length)) {
        g.droppedNoPayload.fetch_add(1, std::memory_order_relaxed);
        return TapVerdict::NoPayload;
    }

    // VOIP.RECEIVE's third argument is dereferenced at exactly one displacement, +4, as a 16-bit
    // read -- W2-H's recvguard proves that from the shipped bytes and from the instruction stream.
    //
    // ★ AN UNREADABLE HEADER REFUSES THE FRAME. Zero is a valid sequence, so storing it when the
    // word could not be read would put "we did not observe this" in the same field as "packet 0",
    // and the jitter buffer orders on that field.
    if (!Readable(At(header, audio_facts::kVoipRecvSeqOff), sizeof(uint16_t))) {
        g.droppedNoHeader.fetch_add(1, std::memory_order_relaxed);
        return TapVerdict::NoHeader;
    }
    const uint16_t seq = static_cast<uint16_t>(
        *reinterpret_cast<const uint16_t*>(At(header, audio_facts::kVoipRecvSeqOff)) & kSeqMask);

    const uint32_t speaker = SpeakerOf(cs, handle);
    const bool unresolved = speaker == kUnresolvedSpeaker;

    const VoipSink sink = g_sink.load(std::memory_order_acquire);
    if (sink == nullptr) {
        g.droppedNoSink.fetch_add(1, std::memory_order_relaxed);
        return TapVerdict::NoSink;
    }
    if (!sink(speaker, seq, payload, length, g_sinkCtx.load(std::memory_order_acquire))) {
        g.droppedSinkRefused.fetch_add(1, std::memory_order_relaxed);
        return TapVerdict::SinkRefused;
    }

    g.stored.fetch_add(1, std::memory_order_relaxed);
    RaiseMax(g.maxPayload, length);
    LowerMin(g.minPayload, length);
    if (unresolved) {
        g.unresolvedSpeaker.fetch_add(1, std::memory_order_relaxed);
        return TapVerdict::StoredUnresolved;
    }
    return TapVerdict::Stored;
}

// The frame is captured BEFORE the engine sees it. Not for latency: the payload buffer belongs to
// the caller and is only guaranteed for the duration of the call, and the engine's own path can
// return at any of four silent gates. Capturing first records what the client received, which is
// what the file is supposed to hold.
void __fastcall DetourReceiveVoip(void* cs, uint32_t handle, const void* header,
                                  const void* payload, uint32_t length) {
    Tap(cs, handle, header, payload, length);
    if (g_trampoline != nullptr) {
        g_trampoline(cs, handle, header, payload, length);
    }
}

}  // namespace

void SetVoipSink(VoipSink sink, void* ctx) {
    g_sinkCtx.store(ctx, std::memory_order_release);
    g_sink.store(sink, std::memory_order_release);
}

VoipCaptureStats CaptureStats() {
    VoipCaptureStats s;
    s.observed = g.observed.load(std::memory_order_relaxed);
    s.stored = g.stored.load(std::memory_order_relaxed);
    s.droppedNoSink = g.droppedNoSink.load(std::memory_order_relaxed);
    s.droppedSinkRefused = g.droppedSinkRefused.load(std::memory_order_relaxed);
    s.droppedOversize = g.droppedOversize.load(std::memory_order_relaxed);
    s.droppedNoPayload = g.droppedNoPayload.load(std::memory_order_relaxed);
    s.droppedNoHeader = g.droppedNoHeader.load(std::memory_order_relaxed);
    s.unresolvedSpeaker = g.unresolvedSpeaker.load(std::memory_order_relaxed);
    s.maxPayloadSeen = g.maxPayload.load(std::memory_order_relaxed);
    s.minPayloadSeen = g.minPayload.load(std::memory_order_relaxed);
    s.maxLengthRefused = g.maxRefused.load(std::memory_order_relaxed);
    return s;
}

void* LatchedCs() { return g_latchedCs.load(std::memory_order_relaxed); }
uint32_t LatchedHandle() { return g_latchedHandle.load(std::memory_order_relaxed); }

HookStatus CreateVoipCapture() {
    return hookmgr::Create(kVoipReceiveFactId, voip_facts::kReceiveVa, voip_facts::kReceivePrologue,
                           20, reinterpret_cast<void*>(&DetourReceiveVoip),
                           reinterpret_cast<void**>(&g_trampoline), "capture.voip");
}

HookStatus ArmVoipCapture() { return hookmgr::Arm(kVoipReceiveFactId); }

void* ReceiveVoipOriginal() { return reinterpret_cast<void*>(g_trampoline); }
HookStatus VoipCaptureStatus() { return hookmgr::Status(kVoipReceiveFactId); }

const char* TapVerdictText(TapVerdict v) {
    switch (v) {
        case TapVerdict::Stored: return "stored";
        case TapVerdict::NoSink: return "dropped: no capture sink is installed";
        case TapVerdict::SinkRefused: return "dropped: the capture sink refused it";
        case TapVerdict::Oversize: return "dropped: the length is past kMaxPayload";
        case TapVerdict::NoPayload: return "dropped: the payload buffer is not readable";
        case TapVerdict::NoHeader: return "dropped: the packet header's sequence word is not readable";
        case TapVerdict::StoredUnresolved: return "stored with an unresolved speaker";
    }
    return "unknown";
}

TapVerdict TapForTest(void* cs, uint32_t handle, const void* header, const void* payload,
                      uint32_t length) {
    return Tap(cs, handle, header, payload, length);
}

void ResetForTest() {
    g.observed.store(0);
    g.stored.store(0);
    g.droppedNoSink.store(0);
    g.droppedSinkRefused.store(0);
    g.droppedOversize.store(0);
    g.droppedNoPayload.store(0);
    g.droppedNoHeader.store(0);
    g.unresolvedSpeaker.store(0);
    g.maxPayload.store(0);
    g.minPayload.store(0xFFFFFFFFu);
    g.maxRefused.store(0);
    g_latchedCs.store(nullptr);
    g_latchedHandle.store(0);
    SetVoipSink(nullptr, nullptr);
    g_trampoline = nullptr;
}

}  // namespace voip
}  // namespace audio
}  // namespace replay
