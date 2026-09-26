#include "capture/netvarsnapshot.h"

#include <windows.h>

#include <atomic>
#include <string.h>

#include "engine/gamestate.h"
#include "playback/core/netvarhud.h"
#include "playback/core/netvarsite.h"

namespace replay {
namespace capture {
namespace {

using playback::HudVarKind;
using playback::kHudVars;
using playback::kHudVarCount;

// The engine call, alone in a function so the SEH frame carries nothing that needs unwinding. The
// raw encoding is the recorded SValue's: float bits for Real, the u32 for Int, the CSymbol64 qword
// for Symbol -- the same three the playback writer hands back to the engine's setters, so a row this
// produces and a row the delta tap produces are the same kind of thing.
bool ReadKeyGuarded(gamestate::IEngineAccess* eng, HudVarKind kind, void* netGame, uint64_t symbol,
                    uint64_t& raw) {
    __try {
        switch (kind) {
            case HudVarKind::Real: {
                const float f = eng->GetFloat(netGame, symbol);
                uint32_t bits = 0;
                memcpy(&bits, &f, sizeof bits);
                raw = bits;
                break;
            }
            case HudVarKind::Int:
                raw = static_cast<uint32_t>(eng->GetInt(netGame, symbol));
                break;
            case HudVarKind::Symbol:
                raw = eng->GetSymbol(netGame, symbol);
                break;
            case HudVarKind::Flag: {
                bool v = false;
                if (!eng->GetBoolChecked(netGame, symbol, v)) return false;
                raw = v ? 1u : 0u;
                break;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Shared {
    // Seqlock: the tick makes it odd while it writes. The worker copies between two equal even
    // readings or takes nothing. One writer, one reader, no lock on the game thread.
    std::atomic<uint32_t> seq{0};
    std::atomic<uint64_t> value[kHudVarCount];
    std::atomic<uint32_t> mask{0};   // bit i: value[i] was read this sample
    static_assert(playback::kHudVarCount <= 32,
                  "the readable mask is one uint32_t bit per HUD key");
    std::atomic<uint16_t> slot{0};
    std::atomic<uint64_t> nextSampleUs{0};
    std::atomic<uint32_t> samples{0};
    std::atomic<uint32_t> routeRefused{0};
    std::atomic<uint32_t> keysRead{0};
    std::atomic<uint32_t> keysFaulted{0};
    // Sticky across the whole file: bit i means key i has been read successfully at least once.
    // The warm-up cadence below runs until every bit is set, so "the first snapshot of the capture
    // found nothing" cannot survive into the footage.
    std::atomic<uint32_t> everMask{0};
    std::atomic<uint64_t> warmStartUs{0};
};

constexpr uint32_t kAllKeysMask =
    kHudVarCount >= 32 ? 0xFFFFFFFFu : ((1u << kHudVarCount) - 1u);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop


Shared& State() {
    static Shared s;
    return s;
}

}  // namespace

void NetvarSnapshotCache::SampleOnTick(void* netGame, uint64_t nowUs, uint64_t minIntervalUs) {
    Shared& st = State();
    if (netGame == nullptr) {
        return;
    }
    if (st.warmStartUs.load(std::memory_order_relaxed) == 0) {
        st.warmStartUs.store(nowUs ? nowUs : 1, std::memory_order_relaxed);
    }
    const uint64_t warmStart = st.warmStartUs.load(std::memory_order_relaxed);
    const uint64_t interval = NetvarSnapshotIntervalUs(
        st.everMask.load(std::memory_order_relaxed) == kAllKeysMask,
        nowUs > warmStart ? nowUs - warmStart : 0, minIntervalUs);

    const uint64_t due = st.nextSampleUs.load(std::memory_order_relaxed);
    if (due != 0 && nowUs < due) {
        return;
    }
    gamestate::IEngineAccess* eng = gamestate::LiveEngineAccess();
    if (eng == nullptr) {
        return;   // the getters did not bind on this build; gamestate reports that once, for both
    }
    st.samples.fetch_add(1, std::memory_order_relaxed);

    // The same structural check the playback writer makes before it touches a getter: the netGame's
    // cached global-netvar handle must still name this level's live component. The getters resolve
    // that handle themselves, so a stale one is a read through whatever the slot holds now.
    const playback::NetVarSite site = playback::LocateNetVarSite(netGame);
    if (!site.RouteOk()) {
        st.routeRefused.fetch_add(1, std::memory_order_relaxed);
        // The due time is committed HERE and not above, so a route that is not bound yet costs the
        // warm-up interval and never the keyframe interval. That ordering is the whole of F3.
        st.nextSampleUs.store(nowUs + interval, std::memory_order_relaxed);
        return;
    }
    st.nextSampleUs.store(nowUs + interval, std::memory_order_relaxed);

    uint64_t raw[kHudVarCount] = {};
    uint32_t mask = 0;
    uint32_t faulted = 0;
    for (uint16_t i = 0; i < kHudVarCount; ++i) {
        if (ReadKeyGuarded(eng, kHudVars[i].kind, netGame, kHudVars[i].symbol, raw[i])) {
            mask |= 1u << i;
        } else {
            ++faulted;
        }
    }
    st.keysRead.fetch_add(__popcnt(mask), std::memory_order_relaxed);
    st.keysFaulted.fetch_add(faulted, std::memory_order_relaxed);
    st.everMask.fetch_or(mask, std::memory_order_relaxed);

    const uint32_t s = st.seq.load(std::memory_order_relaxed);
    st.seq.store(s + 1, std::memory_order_release);
    st.slot.store(site.Slot(), std::memory_order_relaxed);
    for (uint16_t i = 0; i < kHudVarCount; ++i) {
        st.value[i].store(raw[i], std::memory_order_relaxed);
    }
    st.mask.store(mask, std::memory_order_relaxed);
    st.seq.store(s + 2, std::memory_order_release);
}

uint16_t NetvarSnapshotCache::Snapshot(NetvarSnapshotRow* out, uint16_t cap, uint16_t& unreadable) {
    Shared& st = State();
    unreadable = static_cast<uint16_t>(kHudVarCount);
    if (out == nullptr || cap == 0) {
        return 0;
    }
    uint32_t mask = 0;
    uint16_t slot = 0;
    uint64_t raw[kHudVarCount] = {};
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t a = st.seq.load(std::memory_order_acquire);
        if (a & 1u) {
            continue;
        }
        mask = st.mask.load(std::memory_order_relaxed);
        slot = st.slot.load(std::memory_order_relaxed);
        for (uint16_t i = 0; i < kHudVarCount; ++i) {
            raw[i] = st.value[i].load(std::memory_order_relaxed);
        }
        if (st.seq.load(std::memory_order_acquire) == a) {
            uint16_t n = 0;
            for (uint16_t i = 0; i < kHudVarCount && n < cap; ++i) {
                if ((mask & (1u << i)) == 0) {
                    continue;
                }
                out[n].name = kHudVars[i].symbol;
                out[n].value = raw[i];
                out[n].slot = slot;
                ++n;
            }
            unreadable = static_cast<uint16_t>(kHudVarCount - __popcnt(mask));
            return n;
        }
    }
    return 0;   // the tick held the seqlock through eight reads: report every key unreadable
}

void NetvarSnapshotCache::Reset() {
    Shared& st = State();
    const uint32_t s = st.seq.load(std::memory_order_relaxed);
    st.seq.store(s + 1, std::memory_order_release);
    st.mask.store(0, std::memory_order_relaxed);
    st.slot.store(0, std::memory_order_relaxed);
    for (uint16_t i = 0; i < kHudVarCount; ++i) {
        st.value[i].store(0, std::memory_order_relaxed);
    }
    st.seq.store(s + 2, std::memory_order_release);
    st.nextSampleUs.store(0, std::memory_order_relaxed);
    st.samples.store(0, std::memory_order_relaxed);
    st.routeRefused.store(0, std::memory_order_relaxed);
    st.keysRead.store(0, std::memory_order_relaxed);
    st.keysFaulted.store(0, std::memory_order_relaxed);
    st.everMask.store(0, std::memory_order_relaxed);
    st.warmStartUs.store(0, std::memory_order_relaxed);
}

NetvarSnapshotStats NetvarSnapshotCache::Stats() const {
    Shared& st = State();
    NetvarSnapshotStats out;
    out.samples = st.samples.load(std::memory_order_relaxed);
    out.routeRefused = st.routeRefused.load(std::memory_order_relaxed);
    out.keysRead = st.keysRead.load(std::memory_order_relaxed);
    out.keysFaulted = st.keysFaulted.load(std::memory_order_relaxed);
    out.everMask = st.everMask.load(std::memory_order_relaxed);
    out.keysNeverRead =
        static_cast<uint16_t>(kHudVarCount - __popcnt(out.everMask & kAllKeysMask));
    return out;
}

uint16_t NetvarSnapshotCache::KeyCount() { return static_cast<uint16_t>(kHudVarCount); }

const char* NetvarSnapshotCache::KeyName(uint16_t i) {
    return i < kHudVarCount ? kHudVars[i].name : "";
}

NetvarSnapshotCache& LiveNetvarSnapshot() {
    static NetvarSnapshotCache cache;
    return cache;
}

}  // namespace capture
}  // namespace replay
