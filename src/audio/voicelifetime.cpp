#include "audio/voicelifetime.h"

#include <atomic>
#include <chrono>

#include "audio/audio_facts.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace replay {
namespace audio {
namespace {

// Bounded because the table is scanned by the destroyer while it holds the audio thread's budget.
// Thirty-two is every thread this process has ever had inside a voice call at once, an order of
// magnitude over.
constexpr int kMaxReaderSlots = 32;

struct ReaderSlot {
    std::atomic<uint32_t> threadId{0};
    std::atomic<uintptr_t> sink{0};
};

// One row per sink address that has entered Term. Sixty-four is eight times the eight concurrent
// sinks the chain can hold, and the owner's twelve-minute session met fifty-six distinct addresses.
constexpr int kMaxTermRows = 64;

struct TermRow {
    std::atomic<uintptr_t> sink{0};
    std::atomic<uint64_t> stamp{0};
};

struct State {
    // Never reset outside ResetVoiceLifetimeForTest. A generation that could go backwards would
    // make a proof taken before a teardown match one taken after it, which is the ABA this counter
    // exists to close.
    std::atomic<uint64_t> generation{1};
    std::atomic<uintptr_t> dying{0};
    ReaderSlot readers[kMaxReaderSlots];

    TermRow terms[kMaxTermRows];
    // What SinkGeneration answers for a sink with no row. Bumped -- from the same monotonic
    // counter, so the value has never been anyone's stamp -- whenever a row is evicted, because
    // after an eviction "no row" no longer means "never torn down". Every proof taken before that
    // moment therefore compares unequal and is re-taken, which is the safe direction; the unsafe
    // one, a stale proof comparing EQUAL, is unreachable because no stamp is ever issued twice.
    std::atomic<uint64_t> termBase{0};

    std::atomic<uint64_t> termEntries{0};
    std::atomic<uint64_t> drainWaits{0};
    std::atomic<uint64_t> drainTimeouts{0};
    std::atomic<uint64_t> refusals{0};
    std::atomic<uint64_t> noReaderSlot{0};

    std::atomic<LifetimeArm> arm{LifetimeArm::NeverAttempted};
};

State g;

// The slot this thread keeps for the life of the process. Claiming is a scan; every call after the
// first is a thread-local read, which is what keeps the ctor at "two atomics" on the tick path.
thread_local int t_slot = -1;

uint32_t SelfThreadId() {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentThreadId());
#else
    return 1;
#endif
}

void Pause() {
#if defined(_WIN32)
    YieldProcessor();
#endif
}

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Returns the slot index, or -1 when the table is full. `owned` says whether the caller has to
// give the slot back: a nested call borrows one, the outermost keeps its thread's own.
int ClaimSlot(uint32_t self, bool* owned) {
    *owned = false;
    if (t_slot >= 0 && g.readers[t_slot].sink.load(std::memory_order_relaxed) == 0) {
        return t_slot;
    }
    for (int i = 0; i < kMaxReaderSlots; ++i) {
        uint32_t expected = 0;
        if (g.readers[i].threadId.compare_exchange_strong(expected, self, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
            if (t_slot < 0) {
                t_slot = i;      // this thread's permanent slot
                return i;
            }
            *owned = true;       // borrowed for this nested call only
            return i;
        }
    }
    return -1;
}

bool ForeignReaderPresent(uintptr_t sink, uint32_t self) {
    for (int i = 0; i < kMaxReaderSlots; ++i) {
        // ★ THE CALLING THREAD IS EXCLUDED, AND THAT IS NOT AN OPTIMISATION (ARCH-22 S3.2/D6).
        // Ledger row 0.19 calls AK::SoundEngine::ReplaceOutput, which tears the device down
        // unconditionally FIRST -- reaching Term on our own thread. A spin that counted the caller
        // would wait for itself for the whole budget on every hidden seek, which is a frame hitch
        // per seek and a fidelity defect, not merely slowness.
        if (g.readers[i].sink.load(std::memory_order_seq_cst) == sink &&
            g.readers[i].threadId.load(std::memory_order_seq_cst) != self) {
            return true;
        }
    }
    return false;
}

HookStatus (*g_hookCreate)(const char*, uint64_t, const uint8_t*, size_t, void*, void**,
                           const char*) = &hookmgr::Create;
HookStatus (*g_hookArm)(const char*) = &hookmgr::Arm;

// CAkSinkXAudio2::Term. Two arguments, and the count is read out of the shipped bytes rather than
// guessed: the prologue's `mov rbp, rdx` (48 8B EA, the last three of the twenty recorded bytes)
// makes the second argument live, and the tail jump at the end of the body goes to a function the
// extraction types as taking exactly two. A trampoline called with the wrong arity would pass
// garbage in the registers the original still reads.
using TermFn = int64_t(__fastcall*)(void*, void*);
TermFn g_original = nullptr;

int64_t __fastcall SinkTermDetour(void* self, void* a2) {
    // The original's own return value is handed back untouched. Term's tail returns 1 today, but a
    // detour that invented a return value would be answering for the engine.
    return TermDetourBody(reinterpret_cast<uintptr_t>(self), g_original, self, a2);
}

}  // namespace

uint64_t VoiceGeneration() { return g.generation.load(std::memory_order_seq_cst); }

uint64_t SinkGeneration(uintptr_t sink) {
    if (sink == 0) {
        return g.termBase.load(std::memory_order_seq_cst);
    }
    for (int i = 0; i < kMaxTermRows; ++i) {
        if (g.terms[i].sink.load(std::memory_order_seq_cst) == sink) {
            return g.terms[i].stamp.load(std::memory_order_seq_cst);
        }
    }
    return g.termBase.load(std::memory_order_seq_cst);
}

namespace {

// Called from the Term detour, on the audio thread, with the new stamp already drawn.
void RecordSinkTerm(uintptr_t sink, uint64_t stamp) {
    if (sink == 0) {
        return;
    }
    int free = -1;
    int oldest = 0;
    uint64_t oldestStamp = UINT64_MAX;
    for (int i = 0; i < kMaxTermRows; ++i) {
        const uintptr_t held = g.terms[i].sink.load(std::memory_order_seq_cst);
        if (held == sink) {
            g.terms[i].stamp.store(stamp, std::memory_order_seq_cst);
            return;
        }
        if (held == 0) {
            if (free < 0) {
                free = i;
            }
            continue;
        }
        const uint64_t s = g.terms[i].stamp.load(std::memory_order_seq_cst);
        if (s < oldestStamp) {
            oldestStamp = s;
            oldest = i;
        }
    }
    if (free >= 0) {
        g.terms[free].stamp.store(stamp, std::memory_order_seq_cst);
        g.terms[free].sink.store(sink, std::memory_order_seq_cst);
        return;
    }
    // Full: the longest-untorn row goes, and the fallback moves with it so the sink it described
    // cannot be mistaken for one that never entered Term. The row is emptied BEFORE it is refilled,
    // so a reader caught mid-replacement reads the fallback rather than a mismatched pair.
    g.termBase.store(g.generation.fetch_add(1, std::memory_order_seq_cst) + 1,
                     std::memory_order_seq_cst);
    g.terms[oldest].sink.store(0, std::memory_order_seq_cst);
    g.terms[oldest].stamp.store(stamp, std::memory_order_seq_cst);
    g.terms[oldest].sink.store(sink, std::memory_order_seq_cst);
}

}  // namespace

VoiceCall::VoiceCall(const ProvenSink& proven) {
    const uintptr_t sink = proven.sink;
    const uint32_t self = SelfThreadId();
    slot_ = ClaimSlot(self, &ownsSlot_);
    if (slot_ < 0) {
        g.noReaderSlot.fetch_add(1, std::memory_order_relaxed);
        return;      // unpublishable, therefore refused
    }
    // Store first, then load: this half of Dekker is what makes the destroyer's load see us.
    g.readers[slot_].sink.store(sink, std::memory_order_seq_cst);
    const uintptr_t dying = g.dying.load(std::memory_order_seq_cst);
    // THIS sink's stamp, not the process's. A refusal here unlatches the whole rate module, so a
    // comparison that moved when any of the other seven sinks was torn down produced a module-wide
    // `sink gone` about a voice that was never touched -- three of them in the owner's log.
    const uint64_t generation = SinkGeneration(sink);
    if (dying == sink || generation != proven.generation) {
        g.refusals.fetch_add(1, std::memory_order_relaxed);
        return;      // vtbl_ stays null: SinkVerdict::VoiceGone, skip the call
    }
    // The identity test still runs. It catches the two teardowns the engine performs WITHOUT
    // entering Term at all -- a zeroed vtable field and a voice rebuilt at the same address -- and
    // the detour catches the window it cannot see. Neither subsumes the other.
    vtbl_ = LiveVtbl(proven);
}

VoiceCall::~VoiceCall() {
    if (slot_ < 0) {
        return;
    }
    g.readers[slot_].sink.store(0, std::memory_order_seq_cst);
    if (ownsSlot_) {
        g.readers[slot_].threadId.store(0, std::memory_order_seq_cst);
    }
}

int64_t TermDetourBody(uintptr_t sink, int64_t (*original)(void*, void*), void* a1, void* a2) {
    // Order is the claim T2 tests: bump, publish, fence, drain, original, unpublish. The stamp is
    // recorded against THIS sink before the mark goes up, so a reader that misses the mark cannot
    // also miss the stamp.
    const uint64_t stamp = g.generation.fetch_add(1, std::memory_order_seq_cst) + 1;
    RecordSinkTerm(sink, stamp);
    g.dying.store(sink, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    g.termEntries.fetch_add(1, std::memory_order_relaxed);

    const uint32_t self = SelfThreadId();
    if (ForeignReaderPresent(sink, self)) {
        g.drainWaits.fetch_add(1, std::memory_order_relaxed);
        const int64_t deadline = NowUs() + static_cast<int64_t>(kTermDrainBudgetUs);
        while (ForeignReaderPresent(sink, self)) {
            if (NowUs() >= deadline) {
                // ★ THE ORIGINAL RUNS ANYWAY. A Term that never returns hangs Wwise's device
                // thread, which is worse than the crash this row exists to close: a hang has no
                // stack, no log and no recovery. The give-up is COUNTED instead of hidden.
                g.drainTimeouts.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            Pause();
        }
    }

    const int64_t result = original != nullptr ? original(a1, a2) : 1;
    // Cleared only after the original has returned: for the whole of DestroyVoice the mark is the
    // one true statement about the voice, and clearing it earlier would re-open exactly the window
    // AUDIO.SINK_VOICE_CLEAR_SITE proves the engine leaves open.
    g.dying.store(0, std::memory_order_seq_cst);
    return result;
}

const char* LifetimeArmName(LifetimeArm a) {
    switch (a) {
        // The verdict word belongs to the caller that knows which of the four it got; these are
        // the reasons only. A name that spelled REFUSED itself made the never-armed default read
        // as a refusal that had happened (L3 #10 F1).
        case LifetimeArm::Armed: return "armed";
        case LifetimeArm::RefusedByConfig: return "audio.sink_term_hook is off";
        case LifetimeArm::PrologueMismatch:
            return "CAkSinkXAudio2::Term does not hold the recorded prologue -- another tool "
                   "patched it first, and we never overwrite somebody else's patch";
        case LifetimeArm::HookFailed:
            return "the build guard, MinHook or the arm rejected the site";
        case LifetimeArm::NeverAttempted:
            return "nothing called ArmVoiceLifetime(): the site was never offered to the hook";
    }
    return "unknown";
}

LifetimeArm VoiceLifetimeState() { return g.arm.load(std::memory_order_relaxed); }

LifetimeArm ArmVoiceLifetime() {
    if (g.arm.load(std::memory_order_relaxed) == LifetimeArm::Armed) {
        return LifetimeArm::Armed;      // idempotent; never uninstalled
    }
    void* trampoline = nullptr;
    // The address and the bytes come from the fact row, both of them, and nothing here transcribes
    // either: a wrong byte is an install REFUSAL, never a wild detour into the middle of an
    // instruction.
    const HookStatus created = g_hookCreate(
        "AUDIO.SINK_TERM", audio_facts::kSinkTermVa, audio_facts::kSinkTermPrologue,
        audio_facts::kSinkTermPrologueLen, reinterpret_cast<void*>(&SinkTermDetour), &trampoline,
        "audio.sink_term_hook");
    LifetimeArm result = LifetimeArm::HookFailed;
    switch (created) {
        case HookStatus::Created:
            g_original = reinterpret_cast<TermFn>(trampoline);
            result = g_hookArm("AUDIO.SINK_TERM") == HookStatus::Armed ? LifetimeArm::Armed
                                                                       : LifetimeArm::HookFailed;
            break;
        case HookStatus::RefusedByConfig:
            result = LifetimeArm::RefusedByConfig;
            break;
        case HookStatus::RefusedByPrologue:
            result = LifetimeArm::PrologueMismatch;
            break;
        default:
            result = LifetimeArm::HookFailed;
            break;
    }
    g.arm.store(result, std::memory_order_relaxed);
    return result;
}

VoiceLifetimeStats VoiceLifetimeStatsSnapshot() {
    VoiceLifetimeStats s;
    s.termEntries = g.termEntries.load(std::memory_order_relaxed);
    s.drainWaits = g.drainWaits.load(std::memory_order_relaxed);
    s.drainTimeouts = g.drainTimeouts.load(std::memory_order_relaxed);
    s.refusals = g.refusals.load(std::memory_order_relaxed);
    s.noReaderSlot = g.noReaderSlot.load(std::memory_order_relaxed);
    return s;
}

void SetHookAccessForTest(HookCreateFn create, HookArmFn arm) {
    g_hookCreate = create != nullptr ? create : &hookmgr::Create;
    g_hookArm = arm != nullptr ? arm : &hookmgr::Arm;
}

void ResetVoiceLifetimeForTest() {
    g.dying.store(0, std::memory_order_seq_cst);
    for (int i = 0; i < kMaxReaderSlots; ++i) {
        g.readers[i].sink.store(0, std::memory_order_seq_cst);
        g.readers[i].threadId.store(0, std::memory_order_seq_cst);
    }
    t_slot = -1;
    // The rows go; the counter and the fallback do not. A test that could rewind either would be
    // able to prove the ABA guard works by removing the thing that makes it work.
    for (int i = 0; i < kMaxTermRows; ++i) {
        g.terms[i].sink.store(0, std::memory_order_seq_cst);
        g.terms[i].stamp.store(0, std::memory_order_seq_cst);
    }
    g.termEntries.store(0, std::memory_order_relaxed);
    g.drainWaits.store(0, std::memory_order_relaxed);
    g.drainTimeouts.store(0, std::memory_order_relaxed);
    g.refusals.store(0, std::memory_order_relaxed);
    g.noReaderSlot.store(0, std::memory_order_relaxed);
    g.arm.store(LifetimeArm::NeverAttempted, std::memory_order_relaxed);
    g_original = nullptr;
    // The generation is NOT reset: it is a history, and a test that could rewind it would be able
    // to prove the ABA guard works by removing the thing that makes it work.
}

}  // namespace audio
}  // namespace replay
