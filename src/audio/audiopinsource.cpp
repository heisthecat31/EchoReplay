#include "audio/audiopinsource.h"

#include <string.h>

#include "audio/audio_facts.h"
#include "audio/audiostate_init.h"
#include "audio/memprobe.h"
#include "bootstrap/buildguard.h"
#include "engine/audiostate.h"

namespace replay {
namespace audio {
namespace {

using namespace replay::audio_facts;

// One tick's slice of the emitter table. The whole table used to be walked inside the keyframe
// sample; sixteen of them is the unit that keeps a tick's probe count in the tens rather than the
// thousands, and at a 27 Hz tick it still turns a 512-emitter table over well inside a second.
#if defined(W2H_MUTATE_NO_STEP_BOUND)
constexpr uint32_t kEmittersPerTick = 4096;
constexpr uint32_t kTruthPerTick = 4096;
#else
constexpr uint32_t kEmittersPerTick = 16;

// Wwise queries a single tick may make. GetState and GetSwitch take the sound engine's global lock,
// which the audio render thread holds in bursts, so this is the term that has to stay small. The
// positional reuse below means a scene whose audio is not changing spends almost none of it.
constexpr uint32_t kTruthPerTick = 8;
#endif

// Rows re-asked each cycle even when the game's memo looks unchanged. The memo is the game's cache
// and Wwise is the truth; a value changed in Wwise without going through CSoundPlayer leaves the
// cache identical, so without this backstop the ledger would never notice it. Bounded, because a
// backstop that cost a full re-ask per cycle would be the walk again.
constexpr uint32_t kRevalidatePerCycle = 4;

// Emitters the ledger will store rows for. The same bound the engine binding uses, for the same
// reason: a count past it means we are reading the wrong field.
constexpr uint32_t kMaxEmitters = 512;

// A plausibility bound on a live engine container, not a format constant: a count far past anything
// this game holds means the read is wrong and continuing would walk arbitrary memory as if it were
// pairs. src/engine/audiostate.cpp bounds its own memo reads at the same 4096 for the same reason;
// if the two ever diverge both are still a clean refusal.
constexpr uint64_t kMaxPlausibleMemoCount = 4096;

// A memo container, read with two probes instead of three. The data pointer at +0 and the live
// count at +kMemoCountOff sit inside one span, and at sixteen emitters a tick the difference
// between two probes and three is most of the tick's budget.
struct Memo {
    uintptr_t data = 0;
    uint64_t count = 0;
    bool ok = false;
};

Memo ReadMemoSpan(uintptr_t container) {
    Memo m;
    if (!Readable(reinterpret_cast<const void*>(container), kMemoCountOff + sizeof(uint64_t))) {
        return m;
    }
    const uintptr_t data = *reinterpret_cast<const uintptr_t*>(container);
    const uint64_t count = *reinterpret_cast<const uint64_t*>(container + kMemoCountOff);
    if (count > kMaxPlausibleMemoCount) {
        return m;
    }
    if (count != 0 && !Readable(reinterpret_cast<const void*>(data),
                                static_cast<size_t>(count) * kMemoEntrySize)) {
        return m;
    }
    m.data = data;
    m.count = count;
    m.ok = true;
    return m;
}

// The test seam, the same one InitAudioState takes: a synthetic CSoundPlayer laid out in a test
// buffer, so the bounded walk is driven on a machine with no game. Zero -- the default -- means
// the ledger resolves the real one, which is what ships.
uintptr_t g_soundPlayerOverride = 0;

// The same three reads AudioStateBinding::ResolveSoundPlayer makes, over the same constants out of
// audio_facts.h. It is repeated here rather than borrowed because IAudioState deliberately exposes
// no cursor over the emitter table -- it snapshots or it does not -- and a walk that must stop
// halfway needs the table itself.
uintptr_t ResolveSoundPlayer() {
    if (g_soundPlayerOverride != 0) {
        return g_soundPlayerOverride;
    }
    const void* slot = buildguard::MappedAddress(kSoundSystemsPtrVa);
    uintptr_t systems = 0;
    if (slot == nullptr || !ReadAt(reinterpret_cast<uintptr_t>(slot), &systems) || systems == 0) {
        return 0;
    }
    uintptr_t player = 0;
    if (!ReadAt(systems + kSoundPlayerSlotOff, &player)) {
        return 0;
    }
    return player;
}

enum class Phase : uint8_t { States, Switches, Tail };

struct Ledger {
    // What EncodeCurrentAudioPins serialises, and what the next cycle compares against.
    AudioPins ready{};
    AudioPins build{};

    // The memo's CACHED value beside each stored row. This is what makes "has this row changed"
    // answerable without asking Wwise, and it is not stored in the block: N-16 says the cache is
    // the thing that can be wrong, so the block carries the truth and the ledger carries the cache.
    uint32_t readyStateCached[AudioPins::kMaxStates]{};
    uint32_t buildStateCached[AudioPins::kMaxStates]{};
    uint32_t readySwitchCached[AudioPins::kMaxSwitches]{};
    uint32_t buildSwitchCached[AudioPins::kMaxSwitches]{};

    Phase phase = Phase::States;
    uint32_t stateCursor = 0;
    uint32_t emitterCursor = 0;
    // Where in the CURRENT emitter's memo the last tick ran out of budget, and the memo it was
    // reading. A tick that stops here resumes here; the two identity fields make a memo that moved
    // under the cursor a restart rather than a half-and-half read, exactly as a moved emitter table
    // already is.
    uint64_t rowCursor = 0;
    uintptr_t rowMemoData = 0;
    uint64_t rowMemoCount = 0;
    uint32_t revalidateAt = 0;
    uint32_t ticksThisCycle = 0;
    uintptr_t cycleMapBase = 0;    // the emitter table this cycle started on
    uint64_t cycleMapCount = 0;
    bool everBound = false;
    PinLedgerStats stats{};

    // Before the first tick the honest reading is the one an unbound host gets: there is no audio
    // state to have asked. Unstated is a DECODE-side default ("the block said nothing"), and a
    // writer must never claim it.
    Ledger() { ready.source = PinSource::NoAudioState; }
};

Ledger g_ledger;

// A row is re-asked when the game's cache moved under it, when it is new, or when it is this
// cycle's turn in the revalidation sweep.
bool Stale(uint32_t index, uint32_t liveRows) {
    if (kRevalidatePerCycle == 0 || liveRows == 0) {
        return false;
    }
    const uint32_t base = g_ledger.revalidateAt % liveRows;
    for (uint32_t k = 0; k < kRevalidatePerCycle; ++k) {
        if (index == (base + k) % liveRows) {
            return true;
        }
    }
    return false;
}

void BeginCycle() {
    g_ledger.build.stateCount = 0;
    g_ledger.build.switchCount = 0;
    g_ledger.build.loopCount = 0;
    g_ledger.build.totals = PinTotals{};
    g_ledger.build.seed = 0;
    g_ledger.build.seedValid = false;
    g_ledger.build.historyBytes = 0;
    g_ledger.stateCursor = 0;
    g_ledger.emitterCursor = 0;
    g_ledger.rowCursor = 0;
    g_ledger.rowMemoData = 0;
    g_ledger.rowMemoCount = 0;
    g_ledger.ticksThisCycle = 0;
    g_ledger.cycleMapBase = 0;
    g_ledger.cycleMapCount = 0;
    g_ledger.phase = Phase::States;
}

// Returns true when the state memo is exhausted for this cycle.
bool StepStates(IAudioState* state, uintptr_t player, uint32_t& truthBudget, uint32_t& spent) {
    const Memo memo = ReadMemoSpan(player + kMemoCacheOff);
    if (!memo.ok) {
        g_ledger.build.stateCount = 0;
        g_ledger.build.totals.statesSeen = 0;
        g_ledger.build.totals.statesStored = 0;
        return true;
    }
    g_ledger.build.totals.statesSeen = static_cast<uint32_t>(memo.count);
    while (g_ledger.stateCursor < memo.count) {
        const uint32_t i = g_ledger.build.stateCount;
        if (i >= AudioPins::kMaxStates) {
            break;
        }
        const uintptr_t at = memo.data + static_cast<uintptr_t>(g_ledger.stateCursor) *
                                             kMemoEntrySize;
        const uint32_t group = *reinterpret_cast<const uint32_t*>(at);
        const uint32_t cached = *reinterpret_cast<const uint32_t*>(at + 4);

        const bool reusable = i < g_ledger.ready.stateCount &&
                              g_ledger.ready.states[i].group == group &&
                              g_ledger.readyStateCached[i] == cached &&
                              !Stale(i, g_ledger.ready.stateCount);
        uint32_t value;
        if (reusable) {
            value = g_ledger.ready.states[i].value;
        } else {
            if (truthBudget == 0) {
                // ★ Out of budget DEFERS the row, it does not settle it. Writing the cache here
                // and marking the row for a re-ask next cycle costs the same query one cycle
                // later and puts a value N-16 calls untrustworthy in the file meanwhile -- and
                // with more rows walked per tick than queries allowed, the ledger would churn
                // there forever instead of converging.
                ++g_ledger.stats.deferrals;
                return false;
            }
            --truthBudget;
            ++spent;
            ++g_ledger.stats.truthQueries;
            Symbol64 truth = 0;
            // The memo supplies the GROUP LIST; Wwise supplies the VALUE. Storing the cached value
            // would store the thing N-16 says can be wrong, which is the opposite of the point.
            value = state->GetStateTruth(group, truth) ? static_cast<uint32_t>(truth) : cached;
        }
        g_ledger.build.states[i].group = group;
        g_ledger.build.states[i].value = value;
        g_ledger.buildStateCached[i] = cached;
        ++g_ledger.build.stateCount;
        ++g_ledger.stateCursor;
    }
    g_ledger.build.totals.statesStored = g_ledger.build.stateCount;
    return true;
}

// Returns true when the emitter table is exhausted for this cycle.
bool StepSwitches(IAudioState* state, uintptr_t player, uint32_t& truthBudget, uint32_t& spent,
                  uint32_t& emittersThisTick) {
    // The map base and its count sit 0x30 apart, so one probe covers both.
    if (!Readable(reinterpret_cast<const void*>(player + kEmitterMapOff),
                  (kEmitterMapCountOff - kEmitterMapOff) + sizeof(uint64_t))) {
        g_ledger.build.switchCount = 0;
        g_ledger.build.totals.switchesSeen = 0;
        return true;
    }
    const uintptr_t base = *reinterpret_cast<const uintptr_t*>(player + kEmitterMapOff);
    const uint64_t count = *reinterpret_cast<const uint64_t*>(player + kEmitterMapCountOff);
    if (count > kMaxEmitters * 8 ||
        (count != 0 && !Readable(reinterpret_cast<const void*>(base),
                                 static_cast<size_t>(count) * kEmitterMapStride))) {
        g_ledger.build.switchCount = 0;
        g_ledger.build.totals.switchesSeen = 0;
        return true;
    }

    // ★ A cursor into a table that moved is a cursor into a different set, and the half-old,
    // half-new pin set it would build is worse than one more cycle of latency. Restart instead,
    // and count it -- a table that moves every cycle would otherwise show as a ledger that simply
    // never publishes.
    if (g_ledger.emitterCursor != 0 &&
        (base != g_ledger.cycleMapBase || count != g_ledger.cycleMapCount)) {
        ++g_ledger.stats.restarts;
        BeginCycle();
        return false;
    }
    g_ledger.cycleMapBase = base;
    g_ledger.cycleMapCount = count;

    const uint32_t limit = count > kMaxEmitters ? kMaxEmitters : static_cast<uint32_t>(count);
    while (g_ledger.emitterCursor < limit) {
        // ★ THE TICK ENDS WHEREVER THE BUDGET RUNS OUT, INCLUDING INSIDE AN EMITTER
        // (AUDIO-FIX-3 C). It used to end only on an emitter boundary, so one emitter could
        // overrun the query budget by its own group count -- up to kMaxPlausibleMemoCount Wwise
        // queries, each taking the sound engine's global lock, in one frame-thread tick. That is
        // the shape of the 21-32 ms audio stalls in the 2026-09-21 hitch lines. The row cursor
        // below is what makes stopping mid-emitter safe: rows already stored this tick keep their
        // slots and their `switchesSeen`, and the resume starts at the row that was refused.
        if (emittersThisTick >= kEmittersPerTick ||
            (truthBudget == 0 && (emittersThisTick != 0 || g_ledger.rowCursor != 0))) {
            ++g_ledger.stats.deferrals;
            return false;
        }
        const uintptr_t at =
            base + static_cast<uintptr_t>(g_ledger.emitterCursor) * kEmitterMapStride;
        const uint32_t gameObj = *reinterpret_cast<const uint32_t*>(at);
        const uintptr_t emitter = *reinterpret_cast<const uintptr_t*>(at + 8);
        const Memo memo = ReadMemoSpan(emitter + kEmitterSwitchMemoOff);
        // A cursor into a memo that moved is a cursor into a different set of groups, and the rows
        // already stored from the old one cannot be reconciled with the new. Same remedy as a
        // moved emitter table: drop the half-built set and start the cycle again.
        if (memo.ok && g_ledger.rowCursor != 0 &&
            (memo.data != g_ledger.rowMemoData || memo.count != g_ledger.rowMemoCount)) {
            ++g_ledger.stats.restarts;
            BeginCycle();
            return false;
        }
        if (memo.ok) {
            for (uint64_t k = g_ledger.rowCursor; k < memo.count; ++k) {
                ++g_ledger.build.totals.switchesSeen;
                const uint32_t i = g_ledger.build.switchCount;
                if (i >= AudioPins::kMaxSwitches) {
                    continue;   // seen, not stored: the totals row is what makes that visible
                }
                const uintptr_t entry = memo.data + static_cast<uintptr_t>(k) * kMemoEntrySize;
                const uint32_t group = *reinterpret_cast<const uint32_t*>(entry);
                const uint32_t cached = *reinterpret_cast<const uint32_t*>(entry + 4);

                const bool reusable = i < g_ledger.ready.switchCount &&
                                      g_ledger.ready.switches[i].gameObj == gameObj &&
                                      g_ledger.ready.switches[i].group == group &&
                                      g_ledger.readySwitchCached[i] == cached &&
                                      !Stale(i, g_ledger.ready.switchCount);
                uint32_t value;
                if (reusable) {
                    value = g_ledger.ready.switches[i].value;
                } else {
                    if (truthBudget == 0) {
                        // Out of budget defers the ROW, for the reason StepStates gives: a value
                        // written from the cache is the thing N-16 calls untrustworthy. The seen
                        // count is rolled back with it so the row is counted by the tick that
                        // stores it, not by both.
                        --g_ledger.build.totals.switchesSeen;
                        g_ledger.rowCursor = k;
                        g_ledger.rowMemoData = memo.data;
                        g_ledger.rowMemoCount = memo.count;
                        ++g_ledger.stats.deferrals;
                        return false;
                    }
                    --truthBudget;
                    ++spent;
                    ++g_ledger.stats.truthQueries;
                    Symbol64 truth = 0;
                    value = state->GetSwitchTruth(group, gameObj, truth)
                                ? static_cast<uint32_t>(truth)
                                : cached;
                }
                g_ledger.build.switches[i].gameObj = gameObj;
                g_ledger.build.switches[i].group = group;
                g_ledger.build.switches[i].value = value;
                g_ledger.buildSwitchCached[i] = cached;
                ++g_ledger.build.switchCount;
            }
        }
        ++g_ledger.emitterCursor;
        ++emittersThisTick;
        ++g_ledger.stats.emitterSteps;
        g_ledger.rowCursor = 0;
        g_ledger.rowMemoData = 0;
        g_ledger.rowMemoCount = 0;
    }
    g_ledger.build.totals.switchesStored = g_ledger.build.switchCount;
    g_ledger.build.totals.emittersWalked = g_ledger.emitterCursor;
    return true;
}

// The loop table and the random history close the cycle. Neither is a walk of engine memory: the
// loop tracker is our own array, already advanced once per tick by AudioTrackVoices, and the
// history is a single Wwise call.
void Publish(IAudioState* state) {
    uint32_t loopsSeen = 0;
    g_ledger.build.loopCount = static_cast<uint32_t>(
        state->SnapshotLoops(g_ledger.build.loops, AudioPins::kMaxLoops, &loopsSeen));
    g_ledger.build.totals.loopsSeen = loopsSeen;
    g_ledger.build.totals.loopsStored = g_ledger.build.loopCount;
    g_ledger.build.totals.targetsDropped = AudioTargetsDropped();

    // A refused read leaves seedValid false, and that is the honest state: "Wwise would not give us
    // the history" is not "the history was empty".
    Blob history{};
    uint32_t seed = 0;
    if (state->ReadRandomPins(seed, history)) {
        g_ledger.build.seed = seed;
        g_ledger.build.seedValid = true;
        if (history.p != nullptr && history.n != 0) {
            const uint32_t n =
                history.n > AudioPins::kMaxHistory ? uint32_t(AudioPins::kMaxHistory) : history.n;
            memcpy(g_ledger.build.history, history.p, n);
            g_ledger.build.historyBytes = n;
        }
    }
    g_ledger.build.source = PinSource::Bound;

    g_ledger.ready = g_ledger.build;
    memcpy(g_ledger.readyStateCached, g_ledger.buildStateCached,
           sizeof g_ledger.readyStateCached);
    memcpy(g_ledger.readySwitchCached, g_ledger.buildSwitchCached,
           sizeof g_ledger.readySwitchCached);

    ++g_ledger.stats.cycles;
    g_ledger.stats.lastCycleTicks = g_ledger.ticksThisCycle;
    g_ledger.stats.emittersWalked = g_ledger.build.totals.emittersWalked;
    g_ledger.revalidateAt += kRevalidatePerCycle;
    BeginCycle();
}

}  // namespace

void AudioPinsTick() {
    ++g_ledger.stats.ticks;
    ++g_ledger.ticksThisCycle;

    IAudioState* const state = AudioState();
    const uintptr_t player = state != nullptr ? ResolveSoundPlayer() : 0;
    if (state == nullptr || player == 0) {
        // Not a refusal to write. A keyframe with no block at all cannot be told from one recorded
        // before block 16 existed, so the published set stays empty and carries the reason.
        g_ledger.ready = AudioPins{};
        g_ledger.ready.source = PinSource::NoAudioState;
        memset(g_ledger.readyStateCached, 0, sizeof g_ledger.readyStateCached);
        memset(g_ledger.readySwitchCached, 0, sizeof g_ledger.readySwitchCached);
        BeginCycle();
        return;
    }
    g_ledger.everBound = true;

    uint32_t truthBudget = kTruthPerTick;
    uint32_t spent = 0;
    uint32_t emittersThisTick = 0;
    switch (g_ledger.phase) {
        case Phase::States:
            if (StepStates(state, player, truthBudget, spent)) {
                g_ledger.phase = Phase::Switches;
            }
            break;
        case Phase::Switches:
            if (StepSwitches(state, player, truthBudget, spent, emittersThisTick)) {
                g_ledger.phase = Phase::Tail;
            }
            break;
        case Phase::Tail:
            Publish(state);
            break;
    }
    if (emittersThisTick > g_ledger.stats.maxEmittersInATick) {
        g_ledger.stats.maxEmittersInATick = emittersThisTick;
    }
    if (spent > g_ledger.stats.maxTruthInATick) {
        g_ledger.stats.maxTruthInATick = spent;
    }
}

bool GetAudioPins(AudioPins* out) {
    if (out == nullptr || !g_ledger.everBound) {
        return false;
    }
    *out = g_ledger.ready;
    return true;
}

size_t EncodeCurrentAudioPins(void* dst, size_t cap, PinSource* source) {
#if defined(W2H_MUTATE_PIN_WALK_AT_KEYFRAME)
    // The RUN-14 behaviour: gather the whole set here, at keyframe cadence, on the game thread.
    AudioPins pins{};
    IAudioState* const state = AudioState();
    if (state == nullptr) {
        pins.source = PinSource::NoAudioState;
    } else {
        pins.source = PinSource::Bound;
        pins.stateCount = static_cast<uint32_t>(
            state->SnapshotStates(pins.states, AudioPins::kMaxStates, &pins.totals.statesSeen));
        pins.switchCount = static_cast<uint32_t>(state->SnapshotSwitches(
            pins.switches, AudioPins::kMaxSwitches, &pins.totals.switchesSeen));
        pins.loopCount = static_cast<uint32_t>(
            state->SnapshotLoops(pins.loops, AudioPins::kMaxLoops, &pins.totals.loopsSeen));
        pins.totals.statesStored = pins.stateCount;
        pins.totals.switchesStored = pins.switchCount;
        pins.totals.loopsStored = pins.loopCount;
        pins.totals.targetsDropped = AudioTargetsDropped();
        Blob history{};
        uint32_t seed = 0;
        if (state->ReadRandomPins(seed, history)) {
            pins.seed = seed;
            pins.seedValid = true;
            if (history.p != nullptr && history.n != 0) {
                const uint32_t n = history.n > AudioPins::kMaxHistory
                                       ? uint32_t(AudioPins::kMaxHistory)
                                       : history.n;
                memcpy(pins.history, history.p, n);
                pins.historyBytes = n;
            }
        }
    }
    if (source != nullptr) {
        *source = pins.source;
    }
    return EncodePins(pins, dst, cap);
#else
    if (source != nullptr) {
        *source = g_ledger.ready.source;
    }
    return EncodePins(g_ledger.ready, dst, cap);
#endif
}

PinLedgerStats AudioPinLedger() { return g_ledger.stats; }

void ResetAudioPinLedgerForTest() {
    g_ledger = Ledger{};
    g_soundPlayerOverride = 0;
}

void SetPinLedgerSoundPlayerForTest(uintptr_t soundPlayer) {
    g_soundPlayerOverride = soundPlayer;
}

}  // namespace audio
}  // namespace replay
