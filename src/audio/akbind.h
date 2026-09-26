#pragma once

// E-6 `akaudio`. The audio control surface, resolved by NAME out of echovr.exe's own export
// directory. No RVAs, nothing that moves when the binary is rebased or rebuilt.
//
// ---------------------------------------------------------------------------------------------
// WHY BY NAME AND NOT BY ADDRESS. echovr.exe exports 308 `AK::` symbols, so
// every call this subsystem makes is a GetProcAddress against the shipping export table. An
// address in facts.tsv would be a second statement of the same fact and a weaker one: the export
// table IS the relationship, and a name that has gone away fails loudly at bind time instead of
// calling into whatever now lives at a stale address.
//
// The names below are the decorated MSVC symbols. They are checked against the shipped binary by
// tests/audio's export arm, which parses the export directory and fails on any name this header
// asks for and cannot find. Misspell one and that check goes red; that is the input that makes it
// fail, and the unmodified list is the input that makes it pass.
//
// ---------------------------------------------------------------------------------------------
// WHAT IS DELIBERATELY NOT HERE. `AK::SoundEngine::SetOfflineRendering` does not exist in this
// 2018 build and this header does not pretend otherwise -- the rate mechanism is the XAudio2 sink
// (S10.4), not an offline-render switch. `CSoundPlayer::DestroyAllEmitters` is refused by
// The design and is not bound at all: a function that is never to be called does not get a
// pointer someone can find later and use.

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace audio {

// ---- Wwise vocabulary, declared here so no consumer includes a Wwise SDK we do not have ------
using AkUniqueID = uint32_t;         // event, state group, state, switch group, switch, RTPC
using AkGameObjectID = uint64_t;     // the engine passes u32 ids zero-extended into this
using AkPlayingID = uint32_t;
using AkRtpcValue = float;
using AkTimeMs = int32_t;

// AKRESULT, the two values this subsystem branches on. The rest are reported, never interpreted.
enum : int32_t {
    kAkNotImplemented = 0,
    kAkSuccess = 1,
    kAkNotInitialized = 2,
    kAkIdNotFound = 15,       // AUDIO.SHARESET_ABSENT_RESULT: ReplaceOutput with an unknown shareset
};

constexpr AkGameObjectID kAkInvalidGameObject = ~AkGameObjectID(0);

// AkAudioSettings, hand-declared beside its derivation: exported GetAudioSettings writes
// uNumSamplesPerFrame at +0 and uNumSamplesPerSecond at +4 and nothing else
// (AUDIO.AKAUDIOSETTINGS_RATE_OFF). Two u32s, and the struct is 8 bytes.
struct AkAudioSettings {
    uint32_t samplesPerFrame;
    uint32_t samplesPerSecond;
};
static_assert(sizeof(AkAudioSettings) == 8, "GetAudioSettings writes exactly two dwords");

// AK::SoundEngine::Query::RTPCValue_type. Only `Global` and the "was it actually set" question
// matter here; the enumerators are named so a caller cannot pass a bare integer.
enum class RtpcValueType : int32_t { Default = 0, Global = 1, GameObject = 2, PlayingID = 3, Unavailable = 4 };

// ---- the bound surface -----------------------------------------------------------------------
// One struct of function pointers, filled by Bind(). A null member means that name was not in the
// export table; every caller checks, because a subsystem that crashed on a missing export would
// convert a diagnosable absence into an unattributable fault in someone else's game.
struct AkExports {
    // state and switch: the truth side of S10.2's reconciliation
    int32_t (*GetState)(AkUniqueID stateGroup, AkUniqueID* out) = nullptr;
    int32_t (*GetSwitch)(AkUniqueID switchGroup, AkGameObjectID obj, AkUniqueID* out) = nullptr;
    int32_t (*SetState)(AkUniqueID stateGroup, AkUniqueID state) = nullptr;
    int32_t (*SetSwitch)(AkUniqueID switchGroup, AkUniqueID sw, AkGameObjectID obj) = nullptr;

    // what is playing, and where in the source it is
    int32_t (*GetActiveGameObjects)(void* akArrayOfU64) = nullptr;
    int32_t (*GetPlayingIDsFromGameObject)(AkGameObjectID obj, uint32_t* count, AkPlayingID* out) = nullptr;
    AkUniqueID (*GetEventIDFromPlayingID)(AkPlayingID) = nullptr;
    AkGameObjectID (*GetGameObjectFromPlayingID)(AkPlayingID) = nullptr;
    bool (*GetIsGameObjectActive)(AkGameObjectID) = nullptr;
    int32_t (*GetSourcePlayPosition)(AkPlayingID, int32_t* posMs, bool extrapolate) = nullptr;
    int32_t (*GetRTPCValue)(AkUniqueID rtpc, AkGameObjectID obj, AkPlayingID playing,
                            float* value, RtpcValueType* type) = nullptr;

    // the seek battery
    // PostEvent re-arms a loop the file says should be sounding at the target. Posted with no
    // callback and no cookie: the game's own emitter bookkeeping does not learn about these voices,
    // which is correct here because the next seek's StopAll is what ends them, and the game would
    // never have re-posted them itself -- that is precisely the gap E-A6 describes.
    AkPlayingID (*PostEvent)(AkUniqueID event, AkGameObjectID obj, uint32_t flags,
                             void (*callback)(int32_t, void*), void* cookie, uint32_t numExternals,
                             void* externals, AkPlayingID playingId) = nullptr;
    void (*StopAll)(AkGameObjectID) = nullptr;
    void (*StopPlayingID)(AkPlayingID, AkTimeMs fadeMs, int32_t curve) = nullptr;
    // AkActionOnEventType: 0 Stop, 1 Pause, 2 Resume, 3 Break, 4 ReleaseEnvelope. Lets one voice be
    // silenced without a StopAll, which is what reverse's one-shot suppression needs.
    void (*ExecuteActionOnPlayingID)(int32_t action, AkPlayingID, AkTimeMs fadeMs, int32_t curve) = nullptr;
    int32_t (*RenderAudio)(bool allowSyncRender) = nullptr;
    int32_t (*SeekOnEventMs)(AkUniqueID event, AkGameObjectID obj, AkTimeMs position,
                             bool seekToNearestMarker, AkPlayingID) = nullptr;

    // determinism pins (ruling Q4)
    void (*SetRandomSeed)(uint32_t) = nullptr;
    int32_t (*GetContainerHistory)(void* iWriteBytes) = nullptr;
    int32_t (*SetContainerHistory)(void* iReadBytes) = nullptr;

    // rate and transport
    int32_t (*GetAudioSettings)(AkAudioSettings*) = nullptr;
    uint32_t (*GetBufferTick)() = nullptr;
    int32_t (*Suspend)(bool renderAnyway) = nullptr;
    int32_t (*WakeupFromSuspend)() = nullptr;
    int32_t (*SetOutputVolume)(AkGameObjectID outputDevice, float volume) = nullptr;

    // device lifecycle, for E-A1
    int32_t (*RegisterAudioDeviceStatusCallback)(void (*cb)(void*, uint32_t, uint32_t, int32_t, int32_t)) = nullptr;

    // the sink identity proof of S10.4.2
    void* (*GetWwiseXAudio2Interface)() = nullptr;
};

// How a bind attempt went. Reported rather than asserted: -noaudio and a stripped build are both
// legitimate states in which the whole subsystem must stand down cleanly.
struct BindReport {
    uint32_t requested = 0;
    uint32_t resolved = 0;
    const char* firstMissing = nullptr;   // null when nothing was missing
};

// Resolver indirection so the whole binding is testable with no game in the process: tests pass a
// table-driven resolver and drive both the all-present and the one-missing case. The shipping call
// passes nullptr, which means GetProcAddress against GetModuleHandleW(nullptr).
using SymbolResolver = void* (*)(const char* decoratedName, void* user);

// Fills `out`. Returns true when every name resolved. A false return still leaves every name that
// DID resolve usable, because degrading to "no fast-forward pitch" needs more of the surface than
// degrading to "no audio control at all" does.
bool Bind(AkExports* out, BindReport* report, SymbolResolver resolver = nullptr, void* user = nullptr);

// ---- the master switch: `audio.writes_enabled` ------------------------------------------------
//
// ★ ONE SWITCH, AND IT WORKS BY SUBTRACTION. Config key `audio.writes_enabled = false` hands every
// consumer an AkExports whose WRITING entry points are null. Every one of them already refuses a
// null export and says which -- degrading cleanly is what this struct was built for -- so a
// read-only build needs no new branch at any call site, and no write added later can silently miss
// the switch: it can only be added as a pointer that is already null here.
//
// It exists for the cutoff A/B. The question "is the missing cutoff produced by something WE write"
// has no answer that a code review can give, because the thing being changed is inside Wwise's bank
// data. With this key the same session runs with our audio writes on and off, and the per-voice
// attenuation reads answer it directly.
AkExports ReadOnlyAkExports(const AkExports& in);

// The same switch, for the one write that is not an AkExport: the sink voice's COM
// SetFrequencyRatio. Read by sinkvoice.cpp; set once from config at audio-module init.
bool AudioWritesEnabled();
void SetAudioWritesEnabled(bool enabled);

// ---- the RUNTIME half of the same A/B, and why it is a second switch ---------------------------
//
// The key above is read once at init, because turning the whole of src/audio read-only mid-session
// would strand two writes that are only correct in pairs: `Suspend`/`Wakeup` (a flip between the
// halves leaves the owner's game muted with nothing left that will wake it) and the sink pitch (a
// flip at 0.1x leaves our ratio in a voice with no path back to the engine's baseline).
//
// This switch carries the writes that have no other half: the recorded Wwise states and per-emitter
// switches, the memo write-back that can suppress the ENGINE's own next SetSwitch, the seek StopAll
// and the loop re-arm. Those are exactly RE-AUDIO-CUTOFF R3 -- the suspects in the cutoff A/B -- and
// every one of them is idempotent, so flipping it mid-session strands nothing and the far-camera
// A/B runs inside a single launch instead of needing two.
//
// The init key implies this one: with `audio.writes_enabled = false` there is nothing to toggle.
bool StateWritesEnabled();
void SetStateWritesEnabled(bool enabled);

// The exact decorated names Bind asks for, in one place so tests/audio can check them against the
// binary without duplicating the list. Terminated by a null entry.
struct AkSymbolName {
    const char* member;     // the AkExports field it fills, for diagnostics
    const char* decorated;  // the MSVC decorated export name
};
const AkSymbolName* SymbolTable(size_t* count);

}  // namespace audio
}  // namespace replay
