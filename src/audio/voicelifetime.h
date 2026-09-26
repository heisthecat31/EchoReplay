#pragma once

// The design row 2.6 (`audio.sink_term_hook`), ARCH-22 decision A. The lifetime signal for the
// XAudio2 source voice under Wwise's sink.
//
// ★ WHY A READABLE FIELD CANNOT ANSWER THIS. AUDIO.SINK_VOICE_CLEAR_SITE (0x141401F15) is eleven
// bytes AFTER AUDIO.SINK_DESTROYVOICE_CALLSITE (0x141401F0A): CAkSinkXAudio2::Term destroys the
// voice and only THEN nulls sink+0x68, and unlinks the sink from AUDIO.SINK_LIST_PTR last of all.
// So both things a prober can read -- the field and the sink's presence on the list -- publish a
// destroyed voice for the whole duration of DestroyVoice. Re-reading at every use cannot close
// that window; the pages of a freed voice still read and its vtable pointer is still intact, which
// is why L3 #9 faulted INSIDE XAudio2 with every readability proof passing. The answer has to come
// from the engine, before the call that invalidates the object, and that is what this file is.
//
// The producer is an entry detour on AUDIO.SINK_TERM installed through hookmgr, prologue-guarded
// against facts::AUDIO_SINK_TERM_PROLOGUE. The consumer is VoiceCall, a Dekker handshake: the
// reader publishes "inside, on sink S" and then reads the dying mark; the destroyer publishes the
// mark and then reads the readers. Either the reader sees the mark and refuses, or the destroyer
// sees the reader and waits. Both stores are seq_cst, and that is the whole of the protocol.
//
// ★ THE GENERATION IS NOT DECORATION (ARCH-22 S3.1/D5). sub_14139A640 frees the real sink and
// installs a 40-byte fallback in its place, so a later rebuild can put a DIFFERENT object at an
// address a reader already proved. A mark keyed on the sink pointer alone is ABA-defeatable: mark
// set, Term returns, mark cleared, new sink at the same address, the stale ProvenSink matches and
// passes. The counter is bumped on every Term entry and never reset, so a proof taken before any
// teardown can never be mistaken for one taken after it.

#include <stdint.h>

#include "audio/sinkvoice.h"
#include "bootstrap/hookmgr.h"

namespace replay {
namespace audio {

// Process-global. Bumped by the Term detour on EVERY entry, before anything else.
uint64_t VoiceGeneration();

// ★ PER SINK, AND THAT IS THE WHOLE OF PITCH-FIX-2's S1. The counter above is one word for a
// process that runs up to eight CAkSinkXAudio2 sinks at once and churns them hard, so ANY sink's
// Term moved it and every consumer that compared it against a per-voice stamp concluded that every
// OTHER live voice had been rebuilt too. That is what retired the ratio ledger's rows under a voice
// this module was actively commanding, and a retired row is what let `baseline` be re-learned from
// our own slowed reading: 0.1000 -> 0.0500 -> 0.0125 in the owner's 2026-09-20 log.
//
// So the identity a proof carries is this: the value of the global counter AT THE MOMENT THIS SINK
// last entered Term, or the eviction epoch when that is no longer recorded. Values are drawn from
// the one monotonic counter and never reused, so a stamp that matches is a sink that has not been
// through Term since the proof -- whatever any other sink did in between. Forgetting a sink can
// only make a live proof compare unequal (one re-probe), never make a stale proof compare equal.
uint64_t SinkGeneration(uintptr_t sink);

// RAII. Construct before ANY call through a foreign vtable on `proven`; the call is legal only
// while Ok() is true. Never blocks. Never allocates. Two seq_cst atomics on each of ctor/dtor.
//
// A reader that cannot register -- every slot in the table taken -- refuses. It is not that a
// thirty-third audio thread is expected; it is that a reader the destroyer cannot see is exactly
// the reader this class exists to make impossible, and "we could not publish ourselves" must never
// resolve to "go ahead".
class VoiceCall {
public:
    explicit VoiceCall(const ProvenSink& proven);
    ~VoiceCall();

    bool Ok() const { return vtbl_ != nullptr; }
    const XAudio2SourceVoiceVtbl* Vtbl() const { return vtbl_; }

    VoiceCall(const VoiceCall&) = delete;
    VoiceCall& operator=(const VoiceCall&) = delete;

private:
    const XAudio2SourceVoiceVtbl* vtbl_ = nullptr;
    int slot_ = -1;
    bool ownsSlot_ = false;   // a nested call borrows a slot and gives it back; the first keeps its
};

// The producer's arming. Idempotent; the detour is never removed once installed (ARCH-22 S3.3/D4:
// unpatching a trampoline a thread is inside is the same use-after-free, on the audio thread, with
// no guard that can see it).
enum class LifetimeArm : uint8_t {
    Armed,
    RefusedByConfig,     // audio.sink_term_hook is off
    PrologueMismatch,    // the site does not hold the recorded bytes -- another mod got there first
    HookFailed,          // the build guard, MinHook, or the arm itself said no
    // ...and the state before anybody asks. L3 #10 read `HookFailed`'s sentence off a module that
    // had never called this function and went hunting MinHook for a missing call site: a gate that
    // cannot say "never attempted" reports it as "attempted and refused".
    NeverAttempted,
};
const char* LifetimeArmName(LifetimeArm);

LifetimeArm ArmVoiceLifetime();

// What the last arming decided, for the ledger. Before ArmVoiceLifetime runs the answer is
// NeverAttempted -- not any of the three refusals, none of which happened.
LifetimeArm VoiceLifetimeState();

// Bound on the producer's wait, in microseconds. A constant, not a config key: it is a property of
// the audio thread's deadline, and it is exported so the test can assert the spin terminates.
constexpr uint32_t kTermDrainBudgetUs = 2000;

// Counters, because "it never happened" and "nobody counted" are different findings. L3 #10 prints
// drainTimeouts: non-zero is not a failure, it is the number that says whether
// kTermDrainBudgetUs is the right constant.
struct VoiceLifetimeStats {
    uint64_t termEntries = 0;
    uint64_t drainWaits = 0;      // a foreign reader was inside and the producer waited for it
    uint64_t drainTimeouts = 0;   // it gave up on one and called the original anyway
    uint64_t refusals = 0;        // VoiceCall refused: the mark, or a stale generation
    uint64_t noReaderSlot = 0;    // a reader could not publish itself, and therefore refused
};
VoiceLifetimeStats VoiceLifetimeStatsSnapshot();

// The detour body, with the original supplied. The shipped detour calls this with the trampoline,
// so the production path and tests T2..T6 drive ONE body -- a re-implemented body in the test
// would be free to get the ordering right while the shipped one had it wrong.
int64_t TermDetourBody(uintptr_t sink, int64_t (*original)(void*, void*), void* a1, void* a2);

// tests/audio. `create`/`arm` null restores hookmgr's own. The seam exists because the three
// outcomes that matter -- refused by config, refused by prologue, armed -- are unreachable on a
// machine with no echovr.exe mapped, and an install ladder nobody can drive is an install ladder
// nobody has tested (ARCH-22 T7).
using HookCreateFn = HookStatus (*)(const char* factId, uint64_t pcVa, const uint8_t* prologue,
                                    size_t prologueLen, void* detour, void** trampoline,
                                    const char* configKey);
using HookArmFn = HookStatus (*)(const char* factId);
void SetHookAccessForTest(HookCreateFn create, HookArmFn arm);
void ResetVoiceLifetimeForTest();

}  // namespace audio
}  // namespace replay
