#include "engine/wiretap.h"

#include "bootstrap/buildguard.h"
#include "engine/wiretap_facts.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

namespace replay {
namespace {

using namespace wiretap_facts;

using ListenFn      = uint16_t(__fastcall*)(void*, uint64_t, uint32_t, const void*, uint32_t);
using UnlistenFn    = int(__fastcall*)(void*, uint16_t);
using UnlistenAllFn = int(__fastcall*)(void*, void*);
using HashFn        = uint64_t(__fastcall*)(const char*);
using CombineFn     = uint64_t(__fastcall*)(uint64_t, uint64_t);

// The 32 bytes CBroadcaster::Listen copies into the listener node.  The engine builds one
// of these for a member function and puts a generated ListenProxy in the last slot; we put
// our own proxy there, which is why no member-function ABI has to be reproduced.
#if defined(W1L_MUTATE_PROXY_SLOT)
// The shape this packet believed before it read the two dispatchers: the invoker one slot
// after the context, which is where a search for `call qword ptr [rax+18h]` puts it.  Both
// dispatchers call node+40, so a tap built this way registers cleanly and is never invoked.
// tests/wiretap builds exactly one target with this defined, as the control that shows the
// feed check can fail; nothing else may define it.
struct EngineDelegate {
    void* context;
    void* proxy;
    void* unusedTail;
    void* memberSlot;
};
static_assert(sizeof(EngineDelegate) == kDelegateSize, "delegate is not 32 bytes");
#else
struct EngineDelegate {
    void* context;     // handed back as the proxy's first argument
    void* memberSlot;  // the proxy receives the ADDRESS of this slot, not its value
    void* unusedTail;  // the engine leaves it uninitialised; we zero it
    void* proxy;
};
static_assert(sizeof(EngineDelegate) == kDelegateSize, "delegate is not 32 bytes");
static_assert(offsetof(EngineDelegate, proxy) == kDelegateProxyOff, "proxy slot moved");
#endif

// A live listener node holds the address of one of these, so a Tap must not move while it
// is registered.  Slots are therefore claimed and released in place, never compacted.
struct Tap {
    WiretapFn fn;
    void*     ctx;
    uint64_t  symbol;
    uint16_t  handle;
    bool      active;
};

constexpr size_t kMaxTaps = 64;

// The bound on the engine's registration spinlock, and the name the refusal carries.
//
// capture::Tick creates the tap and registers every stream from the GAME THREAD
// (src/capture/capture.cpp Tick -> RegisterTaps), so a wait here is a frame the engine does not
// get. The engine holds this lock only across one CBroadcaster::Listen, so a couple of
// milliseconds is already pathological -- but the engine's own copy of this sequence spins
// unbounded, and reproducing that put a hang with no log and no refusal on the shipping path
// (the design: every wait has a deadline and a named refusal).
constexpr uint64_t    kLockTimeoutUs  = 2000;
constexpr const char* kLockTimeoutTag = "wiretap-lock-timeout";

// Deadlines are compared in raw QPC ticks. Converting the counter to microseconds first overflows
// a uint64 after a few weeks of uptime, and a wrapped deadline is a wait that never ends.
uint64_t QpcNow() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

// Zero on a machine with no performance counter, which makes the deadline expire immediately: the
// wiretap then refuses under any contention at all. That is the safe direction to fail in.
uint64_t QpcFrequency() {
    LARGE_INTEGER f{};
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
        return 0;
    }
    return static_cast<uint64_t>(f.QuadPart);
}

uint64_t QpcTicksFor(uint64_t microseconds) {
    const uint64_t freq = QpcFrequency();
    return freq == 0 ? 0 : (freq * microseconds) / 1000000ull;
}

// The tap's own arrival clock.
//
// A dispatch carries no time we may read. The engine's per-dispatch qword is all ones for a local
// event and was never traced to a definition on the network path (docs/spikes/W1-L.md S9.2), and a
// consumer that stored it as microseconds recorded arrival times of 6, 34, 36, 65, 66,
// 16369959894457074595 and 0xFFFFFFFFFFFFFFFF for one 32 s match. So the tap stamps the message
// itself, at the one point every kind of message passes through.
//
// One basis for the whole process, not one per IWiretap: two taps' stamps have to be comparable,
// and a second basis would make that silently false. QPC rather than RDTSC because taps run on
// engine threads we do not schedule and a TSC read is not comparable across cores.
struct ArrivalClock {
    uint64_t              epoch;
    uint64_t              freq;
    std::atomic<uint64_t> lastUs;
};

ArrivalClock& Arrival() {
    static ArrivalClock clock{QpcNow(), QpcFrequency(), {0}};
    return clock;
}

uint64_t ArrivalNowUs() {
    ArrivalClock& clock = Arrival();
    uint64_t      us    = 0;
    if (clock.freq != 0) {
        const uint64_t now = QpcNow();
        // A counter that ran backwards would produce a value near 2^64 -- indistinguishable from
        // the defect this clock replaces -- so it reads as the epoch instead.
        const uint64_t elapsed = now > clock.epoch ? now - clock.epoch : 0;
        us = (elapsed / clock.freq) * 1000000ull + ((elapsed % clock.freq) * 1000000ull) / clock.freq;
    }
    // Non-decreasing is a property of this function rather than of the counter's guarantees.
    uint64_t seen = clock.lastUs.load(std::memory_order_relaxed);
    while (seen < us && !clock.lastUs.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
    }
    return us > seen ? us : seen;
}

// Set while one of our proxies is on this thread's stack.  CBroadcaster::Unlisten checks
// the engine's own dispatch flag and defers; CBroadcaster::Listen has no such path and
// would relink the bucket chain the dispatcher is walking.
thread_local bool tlsInDispatch = false;

// A guard that meets an image it does not understand has to REFUSE, and refusing is not the same
// answer as disagreeing.  The derived VA is only a hypothesis until the memory
// manager says the whole compare span is mapped: on an image shorter than kBroadcasterUnlistenAll
// -- precisely the wrong build this check exists to reject -- the bare memcmp took an access
// violation instead of answering, in-game and in tests/capture alike.
enum class SiteVerdict { Match, Mismatch, Unreadable };

SiteVerdict CheckPrologue(uintptr_t va, const uint8_t (&want)[20]) {
    const void* at = reinterpret_cast<const void*>(va);
#if defined(W1L_MUTATE_NO_READABLE_PROBE)
    // The shape this file shipped before the probe landed.  tests/wiretap builds exactly one target
    // with this defined, as the control that shows the probe is what stops the fault rather than the
    // address happening to be mapped; nothing else may define it.
#else
    if (!Readable(at, sizeof want)) {
        return SiteVerdict::Unreadable;
    }
#endif
    return std::memcmp(at, want, sizeof want) == 0 ? SiteVerdict::Match : SiteVerdict::Mismatch;
}

struct PrologueSite {
    const char*    name;
    uintptr_t      va;
    const uint8_t (&want)[20];
};

class Wiretap final : public IWiretap {
public:
    Wiretap(void* netGame, const void* engineBase) {
        lockTimeoutTicks_ = QpcTicksFor(kLockTimeoutUs);
        Init(netGame, engineBase);
    }

    ~Wiretap() override { DropAll(); }

    bool        Available() const override { return available_; }
    const char* Diagnostic() const override { return diagnostic_; }
    uint32_t    ActiveCount() const override { return activeTaps_; }
    uint32_t    LockTimeouts() const override { return lockTimeouts_; }

    uint64_t Symbol(const char* structName) const override {
        return available_ ? ComputeSymbol(structName) : 0;
    }

    uint16_t Listen(const char* structName, WiretapFn fn, void* ctx) override {
        return ListenSymbol(Symbol(structName), fn, ctx);
    }

    uint16_t ListenSymbol(uint64_t symbol, WiretapFn fn, void* ctx) override {
        if (!available_ || symbol == 0 || fn == nullptr) {
            return kInvalidHandle;
        }
        if (tlsInDispatch) {
            diagnostic_ = "Listen refused: called from inside a broadcaster callback";
            return kInvalidHandle;
        }
        Tap* tap = ClaimSlot();
        if (tap == nullptr) {
            diagnostic_ = "Listen refused: tap table full";
            return kInvalidHandle;
        }
        tap->fn     = fn;
        tap->ctx    = ctx;
        tap->symbol = symbol;
        tap->handle = kInvalidHandle;

        EngineDelegate del{};
        del.context    = this;  // what UnlistenAll matches on
        del.memberSlot = tap;
        del.proxy      = reinterpret_cast<void*>(&Wiretap::Proxy);

        if (!AcquireRegistrationLock()) {
            // Nothing reached the engine, so there is nothing to undo there: the lock stays the
            // holder's, the slot goes back, and the caller registers again on a later tick.
            // Registering past the deadline would instead leave a live node behind a Tap that this
            // refusal has already handed to the next caller.
            tap->active = false;
            ++lockTimeouts_;
            _snprintf_s(diagBuf_, sizeof diagBuf_, _TRUNCATE,
                        "%s: the CR15NetGame listener lock was still held after %llu us "
                        "(refused %u; retry on a later tick)",
                        kLockTimeoutTag, static_cast<unsigned long long>(kLockTimeoutUs),
                        lockTimeouts_);
            diagnostic_ = diagBuf_;
            return kInvalidHandle;
        }
        // Third argument 0 and fifth argument 0 are what every stock SR15Net* registration
        // passes: no dispatcher reads the third, and 0 appends at the tail so our tap runs
        // after the engine's own handler for the same symbol.
        const uint16_t handle = listen_(broadcaster_, symbol, 0, &del, 0);
        ReleaseRegistrationLock();

        if (handle == kInvalidHandle) {
            tap->active = false;
            diagnostic_ = "Listen refused by the engine: listener pool exhausted";
            return kInvalidHandle;
        }
        tap->handle = handle;
        ++activeTaps_;
        return handle;
    }

    void Unlisten(uint16_t handle) override {
        if (!available_ || handle == kInvalidHandle) {
            return;
        }
        for (Tap& tap : taps_) {
            if (!tap.active || tap.handle != handle) {
                continue;
            }
            unlisten_(broadcaster_, handle);
            tap.active = false;
            --activeTaps_;
            return;
        }
    }

    void UnlistenAll() override { DropAll(); }

private:
    static void __fastcall Proxy(void* context, void** memberSlot, const void* data,
                                 uint64_t bytes, uint64_t stamp, uint64_t peer) {
        auto* tap = static_cast<Tap*>(*memberSlot);
        if (context == nullptr || tap == nullptr || !tap->active || tap->fn == nullptr) {
            return;
        }
        const WireMessage msg{data, bytes, ArrivalNowUs(), stamp, peer};
        const bool wasInDispatch = tlsInDispatch;
        tlsInDispatch = true;
        tap->fn(msg, tap->ctx);
        tlsInDispatch = wasInDispatch;
    }

    uint64_t ComputeSymbol(const char* structName) const {
        if (structName == nullptr || structName[0] != 'S') {
            return 0;
        }
        // The engine hashes the struct name without its leading 'S' and folds the result
        // into a fixed namespace matrix; see the TypeId thunks at 0x1400A4A50 / 0x1400A46F0.
        return combine_(kMatSymNamespace, hash_(structName + 1));
    }

    Tap* ClaimSlot() {
        for (Tap& tap : taps_) {
            if (!tap.active) {
                tap.active = true;
                return &tap;
            }
        }
        return nullptr;
    }

    void DropAll() {
        if (!available_ || activeTaps_ == 0) {
            return;
        }
        unlistenAll_(broadcaster_, this);
        for (Tap& tap : taps_) {
            tap.active = false;
        }
        activeTaps_ = 0;
    }

    void Init(void* netGame, const void* engineBase) {
        if (netGame == nullptr) {
            diagnostic_ = "no CR15NetGame yet (TickInfo::netGame is null before state 9)";
            return;
        }

        const auto module = reinterpret_cast<uintptr_t>(engineBase);
        if (module == 0) {
            diagnostic_ = "no engine image base";
            return;
        }
        const uintptr_t slide = module - static_cast<uintptr_t>(kImageBase);

        const uintptr_t listen      = static_cast<uintptr_t>(kBroadcasterListen) + slide;
        const uintptr_t unlisten    = static_cast<uintptr_t>(kBroadcasterUnlisten) + slide;
        const uintptr_t unlistenAll = static_cast<uintptr_t>(kBroadcasterUnlistenAll) + slide;
        const uintptr_t hash        = static_cast<uintptr_t>(kMatSymHash) + slide;
        const uintptr_t combine     = static_cast<uintptr_t>(kMatSymCombine) + slide;

        // Nothing below is called until all five entry points read back byte for byte.  A span that
        // is not mapped is a third outcome, never folded into either of the other two: "this is a
        // different build" and "this image is too short to hold the address at all" are different
        // findings, and only one of them is an answer the check actually made.
        const PrologueSite sites[] = {
            {"BroadcasterListen", listen, kListenPrologue},
            {"BroadcasterUnlisten", unlisten, kUnlistenPrologue},
            {"BroadcasterUnlistenAll", unlistenAll, kUnlistenAllPrologue},
            {"MatSymHash", hash, kMatSymHashPrologue},
            {"MatSymCombine", combine, kMatSymCombinePrologue},
        };
        for (const PrologueSite& site : sites) {
            const SiteVerdict verdict = CheckPrologue(site.va, site.want);
            if (verdict == SiteVerdict::Match) {
                continue;
            }
            if (verdict == SiteVerdict::Unreadable) {
                std::snprintf(diagBuf_, sizeof diagBuf_,
                              "prologue-unreadable: %s at 0x%llX + %u bytes is not mapped in this "
                              "image, so the build check could not be asked",
                              site.name, static_cast<unsigned long long>(site.va),
                              static_cast<unsigned>(sizeof site.want));
            } else {
                std::snprintf(diagBuf_, sizeof diagBuf_,
                              "entry-point prologue mismatch: not the build W1-L derived against "
                              "(%s at 0x%llX)",
                              site.name, static_cast<unsigned long long>(site.va));
            }
            diagnostic_ = diagBuf_;
            return;
        }

        listen_      = reinterpret_cast<ListenFn>(listen);
        unlisten_    = reinterpret_cast<UnlistenFn>(unlisten);
        unlistenAll_ = reinterpret_cast<UnlistenAllFn>(unlistenAll);
        hash_        = reinterpret_cast<HashFn>(hash);
        combine_     = reinterpret_cast<CombineFn>(combine);

        // The same rule one level down.  These two offsets are only meaningful for the build the
        // prologues just confirmed, and a caller can still hand us something shorter than a
        // CR15NetGame; reading either slot blind would fault where it should refuse.
        const auto  base            = reinterpret_cast<uintptr_t>(netGame);
        const void* broadcasterSlot = reinterpret_cast<const void*>(base + kBroadcasterOff);
        const void* lockSlot        = reinterpret_cast<const void*>(base + kListenLockOff);
        if (!Readable(broadcasterSlot, sizeof(void*)) || !Readable(lockSlot, sizeof(void*))) {
            std::snprintf(diagBuf_, sizeof diagBuf_,
                          "netgame-unreadable: %s slot of the CR15NetGame at 0x%llX is not mapped",
                          Readable(broadcasterSlot, sizeof(void*)) ? "the lock" : "the broadcaster",
                          static_cast<unsigned long long>(base));
            diagnostic_ = diagBuf_;
            return;
        }

        broadcaster_ = *static_cast<void* const*>(broadcasterSlot);
        lock_        = *static_cast<volatile long* const*>(lockSlot);
        if (broadcaster_ == nullptr || lock_ == nullptr) {
            diagnostic_ = "CR15NetGame carries no broadcaster yet";
            return;
        }

        // Known-answer control.  The engine derives these from tables we never read, so
        // agreement on all three proves both hash entry points and the namespace matrix at
        // once.  Without it a wrong address fails silently, as a tap on a symbol nothing
        // ever sends -- which looks exactly like a quiet match.
        if (ComputeSymbol(kNameDeltaUnreliableVars) != kSymDeltaUnreliableVars ||
            ComputeSymbol(kNamePlayerStateMsg) != kSymPlayerStateMsg ||
            ComputeSymbol(kNameComponentEvent) != kSymComponentEvent) {
            diagnostic_ = "CMatSym known-answer control failed: symbols would not match the wire";
            return;
        }

        available_  = true;
        diagnostic_ = "ok";
    }

    // False means the deadline passed with the lock still held: the caller must not touch the
    // broadcaster and must not release a lock it never took.
    bool AcquireRegistrationLock() {
        const uint64_t deadline = QpcNow() + lockTimeoutTicks_;
        for (;;) {
            while (*lock_ != 0) {
#if defined(W1L_MUTATE_UNBOUNDED_LOCK)
                // The shape this file shipped before the deadline landed: the engine's own spin,
                // copied whole. The engine can afford it because it only ever takes this lock from
                // threads it schedules; we take it from the game thread while a thread we do not
                // own may hold it. tests/wiretap builds exactly one target with this defined, as
                // the control that shows the deadline is what stops the spin; nothing else may
                // define it.  The deadline is computed and then deliberately unused, which is
                // the whole of the difference.
                (void)deadline;
#else
                if (QpcNow() >= deadline) {
                    return false;
                }
#endif
                YieldProcessor();
            }
            if (_InterlockedCompareExchange(lock_, 1, 0) == 0) {
                return true;
            }
#if !defined(W1L_MUTATE_UNBOUNDED_LOCK)
            // Lost the race to another taker. The retry is bounded by the same deadline, so a
            // stream of winners cannot turn this into the unbounded wait the inner loop just left.
            if (QpcNow() >= deadline) {
                return false;
            }
#endif
        }
    }

    void ReleaseRegistrationLock() { _InterlockedExchange(lock_, 0); }

    ListenFn      listen_      = nullptr;
    UnlistenFn    unlisten_    = nullptr;
    UnlistenAllFn unlistenAll_ = nullptr;
    HashFn        hash_        = nullptr;
    CombineFn     combine_     = nullptr;

    void*          broadcaster_ = nullptr;
    volatile long* lock_        = nullptr;

    Tap         taps_[kMaxTaps]{};
    uint32_t    activeTaps_ = 0;
    uint32_t    lockTimeouts_ = 0;
    uint64_t    lockTimeoutTicks_ = 0;
    bool        available_  = false;
    const char* diagnostic_ = "not initialised";
    // Backs the refusals that name a site or a deadline.  Diagnostic() is the only channel the probe and the
    // feed host read, so a refusal that cannot say WHERE it refused is one the harness cannot act on.
    char        diagBuf_[176]{};
};

}  // namespace

uint64_t WiretapArrivalNowUs() { return ArrivalNowUs(); }

IWiretap* WiretapCreate(void* netGame) {
    return WiretapCreateAt(netGame, GetModuleHandleW(nullptr));
}

IWiretap* WiretapCreateAt(void* netGame, const void* engineBase) {
    return new (std::nothrow) Wiretap(netGame, engineBase);
}

void WiretapDestroy(IWiretap* tap) { delete tap; }

}  // namespace replay
