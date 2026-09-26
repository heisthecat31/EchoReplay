#include "engine/audiostate.h"

#include <string.h>

#include "audio/akbind.h"
#include "audio/audio_facts.h"
#include "audio/audiostate_init.h"
#include "audio/memprobe.h"
#include "bootstrap/buildguard.h"

namespace replay {
namespace audio {
namespace {

using namespace replay::audio_facts;

constexpr size_t kMaxStates = 64;
constexpr size_t kMaxSwitches = 512;
constexpr size_t kMaxLoops = 256;
constexpr size_t kMaxEmitters = 512;
constexpr size_t kMaxPlayingPerObject = 64;
// The memo is a live engine container. A count far past anything this game plausibly holds means we
// are reading the wrong field, and continuing would walk arbitrary memory as if it were pairs.
constexpr uint64_t kImplausibleMemoCount = 4096;

// ---- the memo container, hand-declared beside its derivation ---------------------------------
// CSoundPlayer::SetState reads the entry array through `*(container)` and the live count through
// `*(container + 0x30)`, then indexes `*(u32*)(data + 8*i + 4)` to compare the cached value. That
// is the whole of what this code needs to know, and it is three reads rather than a struct
// declaration, so a field we never touch cannot be wrong.
struct MemoView {
    uintptr_t data = 0;
    uint64_t count = 0;
    bool ok = false;
};

MemoView ReadMemo(uintptr_t container) {
    MemoView v;
    uintptr_t data = 0;
    uint64_t count = 0;
    if (!ReadAt(container, &data) || !ReadAt(container + kMemoCountOff, &count)) {
        return v;
    }
    if (count > kImplausibleMemoCount) {
        return v;
    }
    if (count != 0 && !Readable(reinterpret_cast<const void*>(data),
                                static_cast<size_t>(count) * kMemoEntrySize)) {
        return v;
    }
    v.data = data;
    v.count = count;
    v.ok = true;
    return v;
}

bool MemoEntry(const MemoView& m, uint64_t index, uint32_t* group, uint32_t* value) {
    if (!m.ok || index >= m.count) {
        return false;
    }
    const uintptr_t at = m.data + static_cast<uintptr_t>(index) * kMemoEntrySize;
    *group = *reinterpret_cast<const uint32_t*>(at);
    *value = *reinterpret_cast<const uint32_t*>(at + 4);
    return true;
}

// Corrects the cached value for a group already in the memo.
//
// The three outcomes are distinguished rather than collapsed into a bool, because the caller's
// counter is a claim about what CHANGED. A counter that incremented on "the group was there" would
// report corrections for a write that never happened, and a mutation removing the store would then
// leave the counter looking healthy -- which is the counter counting its own polling.
//
// NotCached is not an error: it means the game's next SetState for that group reaches Wwise, which
// is what we want anyway, and inserting would need the engine's own allocator.
enum class MemoWrite : uint8_t { NotCached, AlreadyEqual, Written };

MemoWrite WriteMemoValue(const MemoView& m, uint32_t group, uint32_t value) {
    // This one is not an AkExport, so the read-only view does not reach it -- and it is the write
    // with the longest reach of any in this plugin: a memo entry we correct makes the ENGINE's own
    // next SetSwitch for that group return early, so our write can stop the game's write from ever
    // reaching Wwise. RE-AUDIO-CUTOFF R3.
    if (!audio::StateWritesEnabled()) {
        return MemoWrite::NotCached;
    }
    for (uint64_t i = 0; i < m.count; ++i) {
        const uintptr_t at = m.data + static_cast<uintptr_t>(i) * kMemoEntrySize;
        if (*reinterpret_cast<const uint32_t*>(at) != group) {
            continue;
        }
        if (*reinterpret_cast<const uint32_t*>(at + 4) == value) {
            return MemoWrite::AlreadyEqual;
        }
        *reinterpret_cast<uint32_t*>(at + 4) = value;
        return MemoWrite::Written;
    }
    return MemoWrite::NotCached;
}

// ---- the emitter table -------------------------------------------------------------------------
// CSoundPlayer::FindEmitter binary-searches a sorted array of 16-byte {u32 gameObj, pad, u64
// emitter} entries at +EMITTER_MAP_OFF, sized by +EMITTER_MAP_COUNT_OFF. Walking it gives the
// complete set of game objects the game has sound state for -- which is why this design does not
// need AK::Query::GetActiveGameObjects and its AkArray ABI at all.
struct EmitterRow {
    uint32_t gameObj;
    uintptr_t emitter;
};

size_t ReadEmitters(uintptr_t soundPlayer, EmitterRow* out, size_t cap, uint32_t* total) {
    if (total != nullptr) {
        *total = 0;
    }
    uintptr_t base = 0;
    uint64_t count = 0;
    if (!ReadAt(soundPlayer + kEmitterMapOff, &base) ||
        !ReadAt(soundPlayer + kEmitterMapCountOff, &count)) {
        return 0;
    }
    if (count > kMaxEmitters * 8 ||
        (count != 0 && !Readable(reinterpret_cast<const void*>(base),
                                 static_cast<size_t>(count) * kEmitterMapStride))) {
        return 0;
    }
    if (total != nullptr) {
        *total = static_cast<uint32_t>(count);
    }
    size_t written = 0;
    for (uint64_t i = 0; i < count && written < cap; ++i) {
        const uintptr_t at = base + static_cast<uintptr_t>(i) * kEmitterMapStride;
        out[written].gameObj = *reinterpret_cast<const uint32_t*>(at);
        out[written].emitter = *reinterpret_cast<const uintptr_t*>(at + 8);
        ++written;
    }
    return written;
}

}  // namespace

// ==============================================================================================

class AudioStateBinding final : public IAudioState {
public:
    // `soundPlayer` non-zero overrides the image walk, which is how tests drive the whole memo and
    // emitter logic against a synthetic CSoundPlayer with no game in the process.
    bool Init(const AkExports& ak, uintptr_t soundPlayerOverride = 0) {
        ak_ = ak;
        soundPlayer_ = soundPlayerOverride != 0 ? soundPlayerOverride : ResolveSoundPlayer();
        disableFlag_ = ResolveDisableFlag();
        return soundPlayer_ != 0;
    }

    bool Ready() const { return soundPlayer_ != 0; }
    uintptr_t SoundPlayer() const { return soundPlayer_; }

    // ---- B.11.4 -------------------------------------------------------------------------------

    bool SetState(Symbol64 group, Symbol64 value) override {
        if (!Narrows(group) || !Narrows(value) || ak_.SetState == nullptr ||
            !audio::StateWritesEnabled()) {
            return false;
        }
        const auto g = static_cast<uint32_t>(group);
        const auto v = static_cast<uint32_t>(value);
        // Wwise first, with the real value and no intermediate one. Then the cache is corrected to
        // agree, so the engine's own next call is not short-circuited into disagreeing with it.
        if (ak_.SetState(g, v) != kAkSuccess) {
            return false;
        }
        const MemoView memo = ReadMemo(soundPlayer_ + kMemoCacheOff);
        if (memo.ok) {
            WriteMemoValue(memo, g, v);
        }
        return true;
    }

    bool SetSwitch(Symbol64 group, Symbol64 value, uint64_t gameObj) override {
        if (!Narrows(group) || !Narrows(value) || !Narrows(gameObj) || ak_.SetSwitch == nullptr ||
            !audio::StateWritesEnabled()) {
            return false;
        }
        const auto g = static_cast<uint32_t>(group);
        const auto v = static_cast<uint32_t>(value);
        if (ak_.SetSwitch(g, v, gameObj) != kAkSuccess) {
            return false;
        }
        uintptr_t emitter = 0;
        if (FindEmitter(static_cast<uint32_t>(gameObj), &emitter)) {
            const MemoView memo = ReadMemo(emitter + kEmitterSwitchMemoOff);
            if (memo.ok) {
                WriteMemoValue(memo, g, v);
            }
        }
        return true;
    }

    bool GetStateTruth(Symbol64 group, Symbol64& value) const override {
        if (!Narrows(group) || ak_.GetState == nullptr) {
            return false;
        }
        uint32_t out = 0;
        if (ak_.GetState(static_cast<uint32_t>(group), &out) != kAkSuccess) {
            return false;
        }
        value = out;
        return true;
    }

    bool GetSwitchTruth(Symbol64 group, uint64_t gameObj, Symbol64& value) const override {
        if (!Narrows(group) || ak_.GetSwitch == nullptr) {
            return false;
        }
        uint32_t out = 0;
        if (ak_.GetSwitch(static_cast<uint32_t>(group), gameObj, &out) != kAkSuccess) {
            return false;
        }
        value = out;
        return true;
    }

    // The N-16 residue, and it does two independent repairs. Driving Wwise to the replay's target
    // is the obvious one. Correcting the game's cache to match Wwise is the one that keeps working
    // after we stop looking: leave a stale cache behind and the next state the GAME tries to set is
    // the one that silently does nothing.
    void Reconcile() override {
        report_ = ReconcileReport{};
        if (soundPlayer_ == 0) {
            ++report_.refusals;
            return;
        }
        report_.soundDisabled = SoundDisabled();

        const MemoView memo = ReadMemo(soundPlayer_ + kMemoCacheOff);
        if (!memo.ok) {
            ++report_.refusals;
            return;
        }

        // Pass 1: every target the file carried.
        for (size_t i = 0; i < targetStateCount_; ++i) {
            const AudioStatePair& want = targetStates_[i];
            ++report_.statesChecked;
            Symbol64 truth = 0;
            if (!GetStateTruth(want.group, truth)) {
                ++report_.refusals;
                continue;
            }
            if (static_cast<uint32_t>(truth) != want.value) {
                if (SetState(want.group, want.value)) {
                    ++report_.statesCorrectedInWwise;
                } else {
                    ++report_.refusals;
                }
            } else {
                // Wwise already agreed. The game's cache still may not, and that disagreement is
                // exactly N-16: leave it and the next state the GAME sets is the silent no-op.
                WriteMemoValue(memo, want.group, want.value);
            }
        }

        // Pass 2: the memo-versus-Wwise repair, over every group the session has ever touched. This
        // runs whether or not the file carried pins, which is what makes a file recorded before the
        // pin block existed still seek without leaving the cache lying.
        for (uint64_t i = 0; i < memo.count; ++i) {
            uint32_t group = 0;
            uint32_t cached = 0;
            if (!MemoEntry(memo, i, &group, &cached)) {
                break;
            }
            Symbol64 truth = 0;
            if (!GetStateTruth(group, truth)) {
                ++report_.refusals;
                continue;
            }
            if (static_cast<uint32_t>(truth) != cached &&
                WriteMemoValue(memo, group, static_cast<uint32_t>(truth)) == MemoWrite::Written) {
                ++report_.memoEntriesCorrected;
            }
        }

        for (size_t i = 0; i < targetSwitchCount_; ++i) {
            const AudioSwitchTriple& want = targetSwitches_[i];
            ++report_.switchesChecked;
            Symbol64 truth = 0;
            if (!GetSwitchTruth(want.group, want.gameObj, truth)) {
                ++report_.refusals;
                continue;
            }
            if (static_cast<uint32_t>(truth) != want.value) {
                if (SetSwitch(want.group, want.value, want.gameObj)) {
                    ++report_.switchesCorrectedInWwise;
                } else {
                    ++report_.refusals;
                }
            }
        }
    }

    void PinRandom(uint32_t seed, const Blob& containerHistory) override;
    bool ReadRandomPins(uint32_t& seed, Blob& containerHistory) const override;

    // ---- the amendment --------------------------------------------------------------------------

    size_t SnapshotStates(AudioStatePair* out, size_t cap, uint32_t* total) const override {
        if (total != nullptr) {
            *total = 0;
        }
        if (out == nullptr || soundPlayer_ == 0) {
            return 0;
        }
        const MemoView memo = ReadMemo(soundPlayer_ + kMemoCacheOff);
        if (!memo.ok) {
            return 0;
        }
        if (total != nullptr) {
            *total = static_cast<uint32_t>(memo.count);
        }
        size_t written = 0;
        for (uint64_t i = 0; i < memo.count && written < cap; ++i) {
            uint32_t group = 0;
            uint32_t cached = 0;
            if (!MemoEntry(memo, i, &group, &cached)) {
                break;
            }
            // The memo supplies the GROUP LIST; Wwise supplies the VALUE. Storing the cached value
            // would store the thing N-16 says can be wrong, which is the opposite of the point.
            Symbol64 truth = 0;
            out[written].group = group;
            out[written].value = GetStateTruth(group, truth) ? static_cast<uint32_t>(truth) : cached;
            ++written;
        }
        return written;
    }

    size_t SnapshotSwitches(AudioSwitchTriple* out, size_t cap, uint32_t* total) const override {
        if (total != nullptr) {
            *total = 0;
        }
        if (out == nullptr || soundPlayer_ == 0) {
            return 0;
        }
        EmitterRow rows[kMaxEmitters];
        const size_t emitters = ReadEmitters(soundPlayer_, rows, kMaxEmitters, nullptr);

        uint32_t seen = 0;
        size_t written = 0;
        for (size_t e = 0; e < emitters; ++e) {
            const MemoView memo = ReadMemo(rows[e].emitter + kEmitterSwitchMemoOff);
            if (!memo.ok) {
                continue;
            }
            for (uint64_t i = 0; i < memo.count; ++i) {
                uint32_t group = 0;
                uint32_t cached = 0;
                if (!MemoEntry(memo, i, &group, &cached)) {
                    break;
                }
                ++seen;
                if (written >= cap) {
                    continue;
                }
                Symbol64 truth = 0;
                out[written].gameObj = rows[e].gameObj;
                out[written].group = group;
                out[written].value = GetSwitchTruth(group, rows[e].gameObj, truth)
                                         ? static_cast<uint32_t>(truth)
                                         : cached;
                ++written;
            }
        }
        if (total != nullptr) {
            *total = seen;
        }
        return written;
    }

    size_t SnapshotLoops(AudioLoopPin* out, size_t cap, uint32_t* total) const override;

    void SetTargets(const AudioStatePair* states, size_t stateCount,
                    const AudioSwitchTriple* switches, size_t switchCount) override {
        // A cap that silently dropped targets would make a partial restore look like a complete
        // one. The excess is counted so ActiveModeLabel can name it.
        targetsDropped_ = 0;
        if (stateCount > kMaxStates) {
            targetsDropped_ += static_cast<uint32_t>(stateCount - kMaxStates);
        }
        if (switchCount > kMaxSwitches) {
            targetsDropped_ += static_cast<uint32_t>(switchCount - kMaxSwitches);
        }
        targetStateCount_ = stateCount < kMaxStates ? stateCount : kMaxStates;
        for (size_t i = 0; i < targetStateCount_; ++i) {
            targetStates_[i] = states[i];
        }
        targetSwitchCount_ = switchCount < kMaxSwitches ? switchCount : kMaxSwitches;
        for (size_t i = 0; i < targetSwitchCount_; ++i) {
            targetSwitches_[i] = switches[i];
        }
    }

    const ReconcileReport& LastReconcile() const override { return report_; }
    uint32_t TargetsDropped() const { return targetsDropped_; }

    // Advances the loop tracker. Called once per capture tick with the wall time in milliseconds;
    // separate from SnapshotLoops so a snapshot does not have to be the thing that measures
    // duration -- a keyframe is 0.5 Hz and a one-shot is shorter than that.
    void TrackVoices(uint32_t nowMs);

    // ★ The tracker is a cache of an observation over a TIME WINDOW, and a playhead jump ends that
    // window. Carried across one, a voice first seen before the jump and still sounding after it
    // accumulates "sustained" time it was never observed for -- and after a BACKWARD jump the two
    // stamps straddle the discontinuity, so the duration is not merely wrong, it is meaningless.
    // INV-P: on a discontinuity a cache is invalidated, never updated.
    void ForgetVoices();

    const AkExports& Exports() const { return ak_; }

private:
    struct VoiceTrack {
        uint32_t gameObj = 0;
        uint32_t eventId = 0;
        uint32_t playingId = 0;
        uint32_t firstSeenMs = 0;
        uint32_t lastSeenMs = 0;
        // The phase Wwise last reported for this voice, refreshed a few tracks per tick rather than
        // all of them at publish time. -1 is "not captured", which is what the re-arm treats as
        // "leave it silent" -- the same answer a refused GetSourcePlayPosition used to give.
        int32_t positionMs = -1;
        bool live = false;
    };
    // ★ THE PUBLISH BURST, PACED. SnapshotLoops used to ask Wwise for every live track's phase in
    // the tick that published, so one tick could carry 256 GetSourcePlayPosition calls -- a spike
    // on the game thread whose size is set by how loud the match is. The phase is now refreshed
    // round-robin, at most kPhasePerTick per tick, exactly as the emitter switch walk is paced.
    // The staleness this buys is bounded: kMaxLoops / kPhasePerTick = 32 ticks covers the whole
    // table, and a voice is not a re-arm candidate until it has sustained loopSustainedMs (2000 ms
    // by default), which is an order of magnitude more than 32 ticks at any frame rate this game
    // runs at. So no track can reach the re-arm gate without having been refreshed.
    static constexpr size_t kPhasePerTick = 8;

    static bool Narrows(uint64_t v) { return v <= 0xFFFFFFFFull; }

    static uintptr_t ResolveSoundPlayer() {
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

    static const uint32_t* ResolveDisableFlag() {
        const void* p = buildguard::MappedAddress(kSoundDisableFlagVa);
        return Readable(p, sizeof(uint32_t)) ? static_cast<const uint32_t*>(p) : nullptr;
    }

    // Nonzero and both engine setters return SUCCESS without touching Wwise. A reconcile that ran
    // against this set would report every write as having taken and would change nothing at all, so
    // it is read and reported rather than discovered as "the fix does not work".
    bool SoundDisabled() const { return disableFlag_ != nullptr && *disableFlag_ != 0; }

    // The engine binary-searches this table and so does this, over the array in place. Copying it
    // per lookup would make a reconcile of n switches over m emitters quadratic for no reason.
    bool FindEmitter(uint32_t gameObj, uintptr_t* out) const {
        uintptr_t base = 0;
        uint64_t count = 0;
        if (!ReadAt(soundPlayer_ + kEmitterMapOff, &base) ||
            !ReadAt(soundPlayer_ + kEmitterMapCountOff, &count) || count == 0 ||
            count > kMaxEmitters * 8 ||
            !Readable(reinterpret_cast<const void*>(base),
                      static_cast<size_t>(count) * kEmitterMapStride)) {
            return false;
        }
        uint64_t lo = 0;
        uint64_t hi = count;
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo) / 2;
            const uintptr_t at = base + static_cast<uintptr_t>(mid) * kEmitterMapStride;
            const uint32_t id = *reinterpret_cast<const uint32_t*>(at);
            if (id == gameObj) {
                *out = *reinterpret_cast<const uintptr_t*>(at + 8);
                return true;
            }
            if (id < gameObj) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return false;
    }

    AkExports ak_{};
    uintptr_t soundPlayer_ = 0;
    const uint32_t* disableFlag_ = nullptr;

    AudioStatePair targetStates_[kMaxStates]{};
    size_t targetStateCount_ = 0;
    AudioSwitchTriple targetSwitches_[kMaxSwitches]{};
    size_t targetSwitchCount_ = 0;

    ReconcileReport report_{};
    uint32_t targetsDropped_ = 0;

    // The loop discriminator's memory. Mutable because it is a cache of an observation, which is
    // what SnapshotLoops needs it to be and what INV-P calls a cache rather than carried state.
    mutable VoiceTrack tracks_[kMaxLoops]{};
    size_t phaseCursor_ = 0;   // where the next tick's kPhasePerTick phase refreshes start
    mutable uint32_t nowMs_ = 0;

    uint32_t pinnedSeed_ = 0;
    bool pinnedSeedValid_ = false;
    mutable uint8_t history_[16384]{};
    mutable uint32_t historyBytes_ = 0;
};

// ---- container history, and the two callback interfaces it needs -------------------------------
//
// ★ THE SHAPE OF THESE COMES FROM THE CALL SITE, NOT FROM A GUESS. GetContainerHistory reaches our
// object with `mov rax,[rcx]; call qword ptr [rax]` and SetContainerHistory with
// `mov r10,[rsi]; call qword ptr [r10]`. Both dispatch through SLOT ZERO, so AK::IWriteBytes and
// AK::IReadBytes have no virtual destructor ahead of their one method. Declaring one here would
// take slot 0 and silently redirect every call into the destructor.
namespace {

struct AkWriteBytes {
    virtual bool WriteBytes(const void* data, int32_t bytes, int32_t& written) = 0;
};
struct AkReadBytes {
    virtual bool ReadBytes(void* data, int32_t bytes, int32_t& read) = 0;
};

struct FixedWriter final : AkWriteBytes {
    uint8_t* dst;
    uint32_t cap;
    uint32_t at = 0;
    bool overflowed = false;

    FixedWriter(uint8_t* d, uint32_t c) : dst(d), cap(c) {}

    bool WriteBytes(const void* data, int32_t bytes, int32_t& written) override {
        if (bytes < 0) {
            return false;
        }
        const uint32_t n = static_cast<uint32_t>(bytes);
        if (at + n > cap) {
            // Reporting false rather than a short write: a truncated container history restored
            // later would pin the wrong variation and look like a working pin.
            overflowed = true;
            written = 0;
            return false;
        }
        memcpy(dst + at, data, n);
        at += n;
        written = bytes;
        return true;
    }
};

struct FixedReader final : AkReadBytes {
    const uint8_t* src;
    uint32_t size;
    uint32_t at = 0;

    FixedReader(const uint8_t* s, uint32_t n) : src(s), size(n) {}

    bool ReadBytes(void* data, int32_t bytes, int32_t& read) override {
        if (bytes < 0) {
            return false;
        }
        const uint32_t n = static_cast<uint32_t>(bytes);
        if (at + n > size) {
            read = 0;
            return false;
        }
        memcpy(data, src + at, n);
        at += n;
        read = bytes;
        return true;
    }
};

}  // namespace

void AudioStateBinding::PinRandom(uint32_t seed, const Blob& containerHistory) {
    if (!audio::StateWritesEnabled()) {
        return;
    }
    if (ak_.SetRandomSeed != nullptr) {
        ak_.SetRandomSeed(seed);
        pinnedSeed_ = seed;
        pinnedSeedValid_ = true;
    }
    if (ak_.SetContainerHistory == nullptr || containerHistory.p == nullptr ||
        containerHistory.n == 0) {
        return;
    }
    FixedReader reader(containerHistory.p, containerHistory.n);
    ak_.SetContainerHistory(&reader);
}

bool AudioStateBinding::ReadRandomPins(uint32_t& seed, Blob& containerHistory) const {
    seed = pinnedSeed_;
    containerHistory.p = nullptr;
    containerHistory.n = 0;
    if (ak_.GetContainerHistory == nullptr) {
        return false;
    }
    FixedWriter writer(history_, sizeof history_);
    const int32_t result = ak_.GetContainerHistory(&writer);
    if (result != kAkSuccess || writer.overflowed) {
        historyBytes_ = 0;
        return false;
    }
    historyBytes_ = writer.at;
    containerHistory.p = history_;
    containerHistory.n = historyBytes_;
    // ★ The seed is write-only in this build: Wwise exports SetRandomSeed and nothing that reads it
    // back. So a pin round-trip proves the HISTORY, and the seed is only as good as the value we
    // last wrote. That is reported here rather than implied by returning true.
    return pinnedSeedValid_;
}

void AudioStateBinding::TrackVoices(uint32_t nowMs) {
    nowMs_ = nowMs;
    if (soundPlayer_ == 0 || ak_.GetPlayingIDsFromGameObject == nullptr ||
        ak_.GetEventIDFromPlayingID == nullptr) {
        return;
    }
    for (auto& t : tracks_) {
        t.live = false;
    }

    EmitterRow rows[kMaxEmitters];
    const size_t emitters = ReadEmitters(soundPlayer_, rows, kMaxEmitters, nullptr);
    for (size_t e = 0; e < emitters; ++e) {
        uint32_t count = 0;
        if (ak_.GetPlayingIDsFromGameObject(rows[e].gameObj, &count, nullptr) != kAkSuccess ||
            count == 0) {
            continue;
        }
        if (count > kMaxPlayingPerObject) {
            count = kMaxPlayingPerObject;
        }
        AkPlayingID ids[kMaxPlayingPerObject];
        if (ak_.GetPlayingIDsFromGameObject(rows[e].gameObj, &count, ids) != kAkSuccess) {
            continue;
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t eventId = ak_.GetEventIDFromPlayingID(ids[i]);
            if (eventId == 0) {
                continue;
            }
            VoiceTrack* slot = nullptr;
            VoiceTrack* free = nullptr;
            for (auto& t : tracks_) {
                if (t.firstSeenMs != 0 && t.gameObj == rows[e].gameObj && t.eventId == eventId) {
                    slot = &t;
                    break;
                }
                if (free == nullptr && t.firstSeenMs == 0) {
                    free = &t;
                }
            }
            if (slot == nullptr) {
                slot = free;
                if (slot == nullptr) {
                    continue;   // the table is full; the pin block reports the total it saw
                }
                slot->gameObj = rows[e].gameObj;
                slot->eventId = eventId;
                slot->firstSeenMs = nowMs != 0 ? nowMs : 1;
            }
            // ★ Rebase rather than freeze. If the clock regressed, firstSeenMs is a stamp from
            // the FUTURE, and saturating the subtraction alone would report the voice as
            // zero-sustained until the clock climbed back past it -- so a loop that should be
            // re-armed on every seek silently never is. That is the opposite failure to the
            // underflow and just as inaudible to a test that only looks for the overflow.
            if (nowMs < slot->firstSeenMs) {
                slot->firstSeenMs = nowMs != 0 ? nowMs : 1;
            }
            slot->playingId = ids[i];
            slot->lastSeenMs = nowMs;
            slot->live = true;
        }
    }

    // A voice that stopped forgets its history. Keeping it would let a one-shot that fires twice
    // accumulate "sustained" time it never had, and the discriminator would promote it to a loop.
    for (auto& t : tracks_) {
        if (!t.live) {
            t = VoiceTrack{};
        }
    }

    // The paced phase refresh. One pass over the table at most, so the cost is bounded by the walk
    // above even when nothing is live; the Wwise calls are bounded by kPhasePerTick.
    if (ak_.GetSourcePlayPosition != nullptr) {
        size_t asked = 0;
        size_t step = 0;
        for (; step < kMaxLoops && asked < kPhasePerTick; ++step) {
            VoiceTrack& t = tracks_[(phaseCursor_ + step) % kMaxLoops];
            if (!t.live) {
                continue;
            }
            ++asked;
            int32_t positionMs = 0;
            t.positionMs = ak_.GetSourcePlayPosition(t.playingId, &positionMs, false) == kAkSuccess
                               ? positionMs
                               : -1;
        }
        // Past the last slot LOOKED AT, not past kPhasePerTick slots: with the live tracks clustered
        // above a run of empty ones, advancing by the call budget would re-ask the same eight every
        // tick and starve the ninth until the cursor crawled up to it.
        phaseCursor_ = (phaseCursor_ + step) % kMaxLoops;
    }
}

void AudioStateBinding::ForgetVoices() {
    for (auto& t : tracks_) {
        t = VoiceTrack{};
    }
    nowMs_ = 0;
}

size_t AudioStateBinding::SnapshotLoops(AudioLoopPin* out, size_t cap, uint32_t* total) const {
    if (total != nullptr) {
        *total = 0;
    }
    if (out == nullptr) {
        return 0;
    }
    uint32_t seen = 0;
    size_t written = 0;
    for (const auto& t : tracks_) {
        if (!t.live || t.firstSeenMs == 0) {
            continue;
        }
        ++seen;
        if (written >= cap) {
            continue;
        }
        // The phase, read from Wwise rather than inferred from how long we have watched: E-A6
        // wants the loop re-armed where it actually was, and sustainedMs is a duration we measured,
        // not a position in the source. Taken from the track, which TrackVoices refreshes a few
        // slots per tick, so publishing is a memory read: -1 here is still E-A6's "phase not
        // captured, leave it silent", now also covering a voice too young to have had its turn.
        const int32_t positionMs = t.positionMs;
        // ★ SATURATE, DO NOT SUBTRACT. These stamps come from the engine's sim clock, which the
        // transport drives and which DECREASES under reverse playback and a backward seek. An
        // unsigned subtraction underflows there, and a voice sounding for 3 ms reports about
        // 4.29e9 -- past every plausible loop threshold, so every one-shot in the file would be
        // re-armed on the next seek. That is the machine-gunning S10.3 forbids, reached from the
        // opposite direction: not by replaying old one-shots, but by mistaking them for loops.
        out[written].gameObj = t.gameObj;
        out[written].eventId = t.eventId;
        out[written].positionMs = positionMs;
        out[written].sustainedMs = t.lastSeenMs > t.firstSeenMs ? t.lastSeenMs - t.firstSeenMs : 0u;
        ++written;
    }
    if (total != nullptr) {
        *total = seen;
    }
    return written;
}

// ---- the process-wide instance -----------------------------------------------------------------
namespace {
AudioStateBinding g_state;
bool g_ready = false;
}  // namespace

IAudioState* AudioState() { return g_ready ? &g_state : nullptr; }

bool InitAudioState(const AkExports& ak, uintptr_t soundPlayerOverride) {
    g_ready = g_state.Init(ak, soundPlayerOverride);
    return g_ready;
}

void AudioTrackVoices(uint32_t nowMs) {
    if (g_ready) {
        g_state.TrackVoices(nowMs);
    }
}

void AudioForgetVoices() {
    if (g_ready) {
        g_state.ForgetVoices();
    }
}

uint32_t AudioTargetsDropped() { return g_ready ? g_state.TargetsDropped() : 0; }

void ResetAudioStateForTest() {
    g_state = AudioStateBinding{};
    g_ready = false;
}

}  // namespace audio
}  // namespace replay
