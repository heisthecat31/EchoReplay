#include "engine/netvar.h"

#include <windows.h>

#include <atomic>
#include <string.h>

#include "capture/duv.h"
#include "engine/wiretap.h"

namespace replay {
namespace netvar {
namespace {

// The 79 netvars of a loaded Arena level (56 slot-0 + 23 slot-1), with room to spare. A name beyond the
// table is not dropped -- it is reported through `dropped` and its row still reaches the file, because
// the file gets the whole message and this mirror is only a convenience for readers.
constexpr uint16_t kMirrorMax = 256;
// Rows waiting for a Poll. Two clock ramps at 118 Hz against a 72-120 Hz consumer needs a couple of
// frames of slack; 1024 is about nine frames at the measured rate.
constexpr uint32_t kQueueSlots = 1024;

const char* kSymbolName = "SR15NetReplicatedDeltaUnreliableVars";

class NetVarImpl final : public INetVar {
public:
    NetVarImpl(IWiretap* tap, MessageFn fn, void* ctx) : tap_(tap), onMessage_(fn), ctx_(ctx) {
        if (!tap_) {
            status_ = Status::NoWiretap;
            return;
        }
        if (!tap_->Available()) {
            status_ = Status::TapUnavailable;
            return;
        }
        // Not registered here: the owner of the tick's lock budget drives TryListen. Until it does,
        // this surface has never been asked for, which is a different answer from having been refused.
        status_ = Status::ListenPending;
    }

    Status TryListen() {
        if (status_ != Status::ListenPending && status_ != Status::ListenRefused) {
            return status_;   // already registered, or refused for a reason a retry cannot change
        }
        handle_ = tap_->Listen(kSymbolName, &NetVarImpl::Thunk, this);
        status_ = handle_ == IWiretap::kInvalidHandle ? Status::ListenRefused : Status::Ok;
        return status_;
    }

    ~NetVarImpl() {
        if (tap_ && handle_ != IWiretap::kInvalidHandle) {
            tap_->Unlisten(handle_);
        }
    }

    size_t Poll(NetVarWrite* out, size_t cap) override {
        size_t n = 0;
        uint32_t tail = tail_;
        while (n < cap && tail != head_.load(std::memory_order_acquire)) {
            out[n++] = queue_[tail % kQueueSlots];
            ++tail;
        }
        tail_ = tail;
        return n;
    }

    bool Write(const NetVarWrite&) override {
        // * RE-PREMISED 2026-09-05 (W2-B, AUD-FINAL R1a). The old premise -- "there is no
        // established apply path on this build" -- is false: NETVAR.RECEIVE_PATH and
        // NETVAR.STORE_DECODE are registry rows, and playback now locates the R15NetVar component
        // system through NETVAR.CS_SYMBOL and writes through the engine's own decoder
        // (src/playback/core/netvarsite.cpp, netvarapply.cpp). The refusal here is UNCHANGED, and
        // for a reason that does not expire: THIS object is the message tap. It holds a listener on
        // SR15NetReplicatedDeltaUnreliableVars and a mirror of what that listener delivered -- no
        // store, no component, no level. Writing from here would mean a store offset guessed at
        // this seam rather than walked at the one that owns it, which is the corruption the
        // original comment was right about. A playback with no level bound still reaches this
        // surface (apply.cpp's per-row fallback), so the refusal is the shipping answer on a solo
        // host, where the engine never registers the receive path at all (DIAG-SOLO-NETVAR).
        //
        // The mirror is deliberately NOT advanced here, and that rule outlives this refusal. A shadow
        // that recorded an INTENT would make the next frame skip a write that never happened, and
        // keep skipping once a real apply path arrived, so the defect would outlive its cause. The
        // mirror advances only on tap delivery, which is an effect the engine has already applied.
        ++stats_.writeRefusals;
        writeRefused_ = true;
        return false;
    }

    bool Read(Symbol64 name, uint8_t slot, uint64_t& rawSValue) const override {
        for (uint16_t i = 0; i < mirrorCount_; ++i) {
            if (mirror_[i].name == name && mirror_[i].slot == slot) {
                rawSValue = mirror_[i].value;
                return true;
            }
        }
        // Absent means we have not seen it, which is a different thing from zero. A caller that wants
        // to know whether the whole set is present asks BaselineSeen().
        return false;
    }

    size_t ObservedCount() const override {
        return static_cast<size_t>(seq_.Observed());
    }

    // ---- the tap callback ----------------------------------------------------------------------
    static void Thunk(const WireMessage& m, void* ctx) {
        static_cast<NetVarImpl*>(ctx)->OnMessage(m);
    }

    void OnMessage(const WireMessage& m) {
        // Measured, not assumed: W1-L's own open question is which thread this runs on, and the answer
        // decides whether the capture ring is crossing a thread boundary. Recording it costs one store.
        callbackThread_.store(GetCurrentThreadId(), std::memory_order_relaxed);
        ++stats_.messages;

        const uint32_t bytes = static_cast<uint32_t>(m.bytes);
        // The body travels whole and first. S_NETVAR's completeness rests on the packet number inside
        // it, and a malformed body is still bytes the client received.
        if (onMessage_) {
            onMessage_(m.data, bytes, m.stamp, ctx_);
        }

        const capture::DuvMessage d = capture::DuvParse(m.data, bytes);
        if (d.status == capture::DuvStatus::TooShort) {
            ++stats_.malformed;
            return;
        }
        capture::DuvSequence::Gap gap{};
        seq_.Note(d, &gap);

        if (d.slot == 0 && d.varCount >= 40) {
            baselineSlot0_ = true;   // the 56-row baseline; see BaselineSeen
        }
        if (d.slot == 1 && d.varCount >= 20) {
            baselineSlot1_ = true;
        }

        for (uint16_t i = 0; i < d.varCount; ++i) {
            const capture::DuvRow row = capture::DuvRowAt(d, i);
            ++stats_.rows;
            NetVarWrite w{};
            w.name = row.name;
            w.rawSValue = row.value;
            w.slot = static_cast<uint8_t>(d.slot);

            const uint32_t head = head_.load(std::memory_order_relaxed);
            if (head - tail_ >= kQueueSlots) {
                ++stats_.dropped;   // the queue is a convenience; the file already has the message
            } else {
                queue_[head % kQueueSlots] = w;
                head_.store(head + 1, std::memory_order_release);
            }
            Mirror(w);
        }
    }

    Status status_ = Status::NoWiretap;
    Stats stats_ = {};
    capture::DuvSequence seq_;
    std::atomic<uint32_t> callbackThread_{0};
    uint32_t tickThread_ = 0;
    bool baselineSlot0_ = false, baselineSlot1_ = false;
    bool writeRefused_ = false;

private:
    void Mirror(const NetVarWrite& w) {
        for (uint16_t i = 0; i < mirrorCount_; ++i) {
            if (mirror_[i].name == w.name && mirror_[i].slot == w.slot) {
                mirror_[i].value = w.rawSValue;
                return;
            }
        }
        if (mirrorCount_ < kMirrorMax) {
            mirror_[mirrorCount_].name = w.name;
            mirror_[mirrorCount_].slot = w.slot;
            mirror_[mirrorCount_].value = w.rawSValue;
            ++mirrorCount_;
        }
    }

    struct MirrorRow {
        uint64_t name;
        uint64_t value;
        uint8_t slot;
    };

    IWiretap* tap_ = nullptr;
    MessageFn onMessage_ = nullptr;
    void* ctx_ = nullptr;
    uint16_t handle_ = IWiretap::kInvalidHandle;

    NetVarWrite queue_[kQueueSlots] = {};
    std::atomic<uint32_t> head_{0};
    uint32_t tail_ = 0;

    MirrorRow mirror_[kMirrorMax] = {};
    uint16_t mirrorCount_ = 0;
};

}  // namespace

const char* StatusText(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NoWiretap: return "no wiretap was supplied";
        case Status::TapUnavailable: return "the wiretap refused; see its own diagnostic";
        case Status::ListenPending: return "the registration has not been attempted yet";
        case Status::ListenRefused: return "BroadcasterListen refused the registration";
        case Status::WriteUnsupported:
            return "no netvar apply path is established on this build; nothing was written";
    }
    return "unknown";
}

INetVar* Create(IWiretap* tap, MessageFn onMessage, void* ctx) {
    return new NetVarImpl(tap, onMessage, ctx);
}

void Destroy(INetVar* v) { delete static_cast<NetVarImpl*>(v); }

Status TryListen(INetVar* v) {
    if (!v) return Status::NoWiretap;
    return static_cast<NetVarImpl*>(v)->TryListen();
}

Status StatusOf(const INetVar* v) {
    if (!v) return Status::NoWiretap;
    const NetVarImpl* n = static_cast<const NetVarImpl*>(v);
    return n->writeRefused_ ? Status::WriteUnsupported : n->status_;
}

void GetStats(const INetVar* v, Stats* out) {
    if (!v || !out) return;
    const NetVarImpl* n = static_cast<const NetVarImpl*>(v);
    *out = n->stats_;
    out->observedPackets = n->seq_.Observed();
    out->missingPackets = n->seq_.Missing();
    out->duplicateSeq = n->seq_.Duplicates();
    out->regressions = n->seq_.Regressions();
    out->callbackThreadId = n->callbackThread_.load(std::memory_order_relaxed);
    out->tickThreadId = n->tickThread_;
}

bool BaselineSeen(const INetVar* v) {
    if (!v) return false;
    const NetVarImpl* n = static_cast<const NetVarImpl*>(v);
    // Both components, because either alone leaves the mirror partial and a partial mirror answering
    // Read() would be the same failure as answering with a zero.
    return n->baselineSlot0_ && n->baselineSlot1_;
}

void NoteTickThread(INetVar* v, uint32_t threadId) {
    if (v) static_cast<NetVarImpl*>(v)->tickThread_ = threadId;
}

}  // namespace netvar
}  // namespace replay
