#include "audio/akbind.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace replay {
namespace audio {
namespace {

// The one place a decorated name appears. Adding a member to AkExports without adding a row here
// leaves that member null, which every caller already handles -- but tests/audio counts the rows
// against the struct's own bind list, so a forgotten row is a red check rather than a silent
// no-op. The order is the order Bind fills them, and firstMissing names the first gap in it.
struct Entry {
    const char* member;
    const char* decorated;
    size_t offset;   // byte offset of the AkExports member this fills
};

#define AK_ENTRY(FIELD, NAME) {#FIELD, NAME, offsetof(AkExports, FIELD)}

const Entry kEntries[] = {
    AK_ENTRY(GetState, "?GetState@Query@SoundEngine@AK@@YA?AW4AKRESULT@@KAEAK@Z"),
    AK_ENTRY(GetSwitch, "?GetSwitch@Query@SoundEngine@AK@@YA?AW4AKRESULT@@K_KAEAK@Z"),
    AK_ENTRY(SetState, "?SetState@SoundEngine@AK@@YA?AW4AKRESULT@@KK@Z"),
    AK_ENTRY(SetSwitch, "?SetSwitch@SoundEngine@AK@@YA?AW4AKRESULT@@KK_K@Z"),
    AK_ENTRY(GetActiveGameObjects,
             "?GetActiveGameObjects@Query@SoundEngine@AK@@YA?AW4AKRESULT@@AEAV?$AkArray@_K_KU?$Ak"
             "ArrayAllocatorNoAlign@U_ArrayPoolDefault@@@@$0CA@U?$AkAssignmentMovePolicy@_K@@@@@Z"),
    AK_ENTRY(GetPlayingIDsFromGameObject,
             "?GetPlayingIDsFromGameObject@Query@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEAKPEAK@Z"),
    AK_ENTRY(GetEventIDFromPlayingID, "?GetEventIDFromPlayingID@Query@SoundEngine@AK@@YAKK@Z"),
    AK_ENTRY(GetGameObjectFromPlayingID, "?GetGameObjectFromPlayingID@Query@SoundEngine@AK@@YA_KK@Z"),
    AK_ENTRY(GetIsGameObjectActive, "?GetIsGameObjectActive@Query@SoundEngine@AK@@YA_N_K@Z"),
    AK_ENTRY(GetSourcePlayPosition, "?GetSourcePlayPosition@SoundEngine@AK@@YA?AW4AKRESULT@@KPEAJ_N@Z"),
    AK_ENTRY(GetRTPCValue,
             "?GetRTPCValue@Query@SoundEngine@AK@@YA?AW4AKRESULT@@K_KKAEAMAEAW4RTPCValue_type@123@@Z"),
    AK_ENTRY(PostEvent,
             "?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAX"
             "KPEAUAkExternalSourceInfo@@K@Z"),
    AK_ENTRY(StopAll, "?StopAll@SoundEngine@AK@@YAX_K@Z"),
    AK_ENTRY(StopPlayingID, "?StopPlayingID@SoundEngine@AK@@YAXKJW4AkCurveInterpolation@@@Z"),
    AK_ENTRY(ExecuteActionOnPlayingID,
             "?ExecuteActionOnPlayingID@SoundEngine@AK@@YAXW4AkActionOnEventType@12@KJW4AkCurve"
             "Interpolation@@@Z"),
    AK_ENTRY(RenderAudio, "?RenderAudio@SoundEngine@AK@@YA?AW4AKRESULT@@_N@Z"),
    AK_ENTRY(SeekOnEventMs, "?SeekOnEvent@SoundEngine@AK@@YA?AW4AKRESULT@@K_KJ_NK@Z"),
    AK_ENTRY(SetRandomSeed, "?SetRandomSeed@SoundEngine@AK@@YAXK@Z"),
    AK_ENTRY(GetContainerHistory, "?GetContainerHistory@SoundEngine@AK@@YA?AW4AKRESULT@@PEAVIWriteBytes@2@@Z"),
    AK_ENTRY(SetContainerHistory, "?SetContainerHistory@SoundEngine@AK@@YA?AW4AKRESULT@@PEAVIReadBytes@2@@Z"),
    AK_ENTRY(GetAudioSettings, "?GetAudioSettings@SoundEngine@AK@@YA?AW4AKRESULT@@AEAUAkAudioSettings@@@Z"),
    AK_ENTRY(GetBufferTick, "?GetBufferTick@SoundEngine@AK@@YAKXZ"),
    AK_ENTRY(Suspend, "?Suspend@SoundEngine@AK@@YA?AW4AKRESULT@@_N@Z"),
    AK_ENTRY(WakeupFromSuspend, "?WakeupFromSuspend@SoundEngine@AK@@YA?AW4AKRESULT@@XZ"),
    AK_ENTRY(SetOutputVolume, "?SetOutputVolume@SoundEngine@AK@@YA?AW4AKRESULT@@_KM@Z"),
    AK_ENTRY(RegisterAudioDeviceStatusCallback,
             "?RegisterAudioDeviceStatusCallback@SoundEngine@AK@@YA?AW4AKRESULT@@P6AXPEAVIAkGlobal"
             "PluginContext@2@KKW4AkAudioDeviceEvent@2@W43@@Z@Z"),
    AK_ENTRY(GetWwiseXAudio2Interface, "?GetWwiseXAudio2Interface@AK@@YAPEAUIUnknown@@XZ"),
};

#undef AK_ENTRY

constexpr size_t kEntryCount = sizeof kEntries / sizeof kEntries[0];

// Cached, because the shipping resolver asks the loader for the main module every call otherwise
// and Bind runs once per session. Not a hot path; the cache exists so the null check has one place.
void* ResolveFromMainImage(const char* name, void*) {
#if defined(_WIN32)
    static HMODULE image = GetModuleHandleW(nullptr);
    if (image == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(image, name));
#else
    (void)name;
    return nullptr;
#endif
}

}  // namespace

const AkSymbolName* SymbolTable(size_t* count) {
    // Built once from kEntries so the two lists cannot diverge: the check that reads this is
    // checking the list Bind actually uses, not a copy of it.
    static AkSymbolName table[kEntryCount + 1];
    static bool built = false;
    if (!built) {
        for (size_t i = 0; i < kEntryCount; ++i) {
            table[i].member = kEntries[i].member;
            table[i].decorated = kEntries[i].decorated;
        }
        table[kEntryCount].member = nullptr;
        table[kEntryCount].decorated = nullptr;
        built = true;
    }
    if (count != nullptr) {
        *count = kEntryCount;
    }
    return table;
}

bool Bind(AkExports* out, BindReport* report, SymbolResolver resolver, void* user) {
    if (out == nullptr) {
        return false;
    }
    *out = AkExports{};
    if (resolver == nullptr) {
        resolver = &ResolveFromMainImage;
    }

    uint32_t resolved = 0;
    const char* firstMissing = nullptr;
    uint8_t* base = reinterpret_cast<uint8_t*>(out);
    for (size_t i = 0; i < kEntryCount; ++i) {
        void* fn = resolver(kEntries[i].decorated, user);
        if (fn == nullptr) {
            if (firstMissing == nullptr) {
                firstMissing = kEntries[i].member;
            }
            continue;
        }
        memcpy(base + kEntries[i].offset, &fn, sizeof fn);
        ++resolved;
    }

    if (report != nullptr) {
        report->requested = static_cast<uint32_t>(kEntryCount);
        report->resolved = resolved;
        report->firstMissing = firstMissing;
    }
    return resolved == kEntryCount;
}

AkExports ReadOnlyAkExports(const AkExports& in) {
    AkExports out = in;
    // Every entry point that changes Wwise's state. The reads -- GetState, GetSwitch, the playing-ID
    // queries, GetAudioSettings, GetBufferTick, GetContainerHistory, the sink interface -- are all
    // kept, because a census that cannot read is not a read-only build, it is a disabled one.
    out.SetState = nullptr;
    out.SetSwitch = nullptr;
    out.PostEvent = nullptr;
    out.StopAll = nullptr;
    out.StopPlayingID = nullptr;
    out.ExecuteActionOnPlayingID = nullptr;
    out.RenderAudio = nullptr;
    out.SeekOnEventMs = nullptr;
    out.SetRandomSeed = nullptr;
    out.SetContainerHistory = nullptr;
    out.Suspend = nullptr;
    out.WakeupFromSuspend = nullptr;
    out.SetOutputVolume = nullptr;
    return out;
}

// Default true: the switch exists to take writes AWAY for one measurement, and a key nobody set
// must leave the product exactly as it shipped.
bool g_audioWrites = true;
bool g_stateWrites = true;

bool AudioWritesEnabled() { return g_audioWrites; }
void SetAudioWritesEnabled(bool enabled) { g_audioWrites = enabled; }

// The init key is the outer term, so `audio.writes_enabled = false` cannot be talked back on by a
// console verb, and a session that started read-only stays read-only.
bool StateWritesEnabled() { return g_audioWrites && g_stateWrites; }
void SetStateWritesEnabled(bool enabled) { g_stateWrites = enabled; }

}  // namespace audio
}  // namespace replay
