#include "offlinepatch.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "attachhook.h"
#include "bootstrap.h"
#include "buildguard.h"
#include "coexist.h"
#include "livesafety.h"
#include "log.h"
#include "offlinepatch_facts.h"

namespace replay::offlinepatch {
namespace {

namespace pf = replay::offlinepatch::facts;

const char* const kModuleName = "offline-patch";

using CliApplyFn = int64_t (*)(uint8_t* game);

struct State {
    std::atomic<Status> status{Status::NotRequested};
    CliApplyFn real = nullptr;
    uint8_t* image = nullptr;
    LONG fired = 0;
    Span written[pf::kSiteCount] = {};
    size_t writtenCount = 0;
};

State& S() {
    static State s;
    return s;
}

uint8_t* SiteAddress(uint8_t* image, size_t i) {
    return image + (pf::kSites[i].va - pf::kPreferredImageBase);
}

// "E8 5D FC FF FF" into `out`.
const char* Hex(const void* bytes, size_t len, char* out, size_t cap) {
    const uint8_t* b = static_cast<const uint8_t*>(bytes);
    size_t at = 0;
    out[0] = '\0';
    for (size_t i = 0; i < len && at + 4 <= cap; ++i) {
        at += static_cast<size_t>(std::snprintf(out + at, cap - at, i == 0 ? "%02X" : " %02X", b[i]));
    }
    return out;
}

bool WriteCode(uint8_t* at, const char* bytes, size_t len) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    std::memcpy(at, bytes, len);
    DWORD ignored = 0;
    VirtualProtect(at, len, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return true;
}

Status Finish(Status status) {
    S().status.store(status, std::memory_order_release);
    return status;
}

void LogRefusal(uint8_t* image, const SiteState* states) {
    for (size_t i = 0; i < pf::kSiteCount; ++i) {
        const pf::Site& site = pf::kSites[i];
        char live[32];
        char original[32];
        char patched[32];
        const uint8_t* at = SiteAddress(image, i);
        log::Direct(LogLevel::Error, kModuleName, "  %s @0x%llX: %s, live [%s], original [%s], patched [%s]",
                    site.name, static_cast<unsigned long long>(site.va),
                    states[i] == SiteState::Original  ? "original"
                    : states[i] == SiteState::Patched ? "patched"
                                                      : "FOREIGN",
                    Readable(at, site.len) ? Hex(at, site.len, live, sizeof live) : "unreadable",
                    Hex(site.original, site.len, original, sizeof original),
                    Hex(site.patched, site.len, patched, sizeof patched));
    }
}

void SetWindowed(uint8_t* game) {
    auto* flags = reinterpret_cast<uint64_t*>(game + pf::kGameFlagsOff);
    if (!Readable(flags, sizeof *flags)) {
        log::Direct(LogLevel::Error, kModuleName,
                    "windowed: CR15Game+0x7AE0 is not readable; the game will ask for a headset");
        return;
    }
    const uint64_t before = *flags;
    *flags = before | pf::kWindowedBit;
    log::Direct(LogLevel::Info, kModuleName,
                "windowed: CR15Game+0x7AE0 0x%llX -> 0x%llX (the -windowed bit, set before CLI.APPLY reads it)",
                static_cast<unsigned long long>(before), static_cast<unsigned long long>(*flags));
}

// Slot 0 of the CR15Game vtable. CLI.APPLY is called exactly once, on the main thread, but the latch
// makes a second call a plain pass-through rather than a second decision.
int64_t ReplayCliApply(uint8_t* game) {
    State& s = S();
    const bool first = InterlockedCompareExchange(&s.fired, 1, 0) == 0;
    if (first && game != nullptr) {
        SetWindowed(game);
    }
    // rlay's -offline runs inside this call, from its detour on the function's entry, so by the time it
    // returns every site is either still original or already rlay's.
    const int64_t result = s.real(game);
    if (first) {
        PatchImage(s.image);
    }
    return result;
}

}  // namespace

const char* StatusName(Status s) {
    switch (s) {
        case Status::NotRequested: return "not-requested";
        case Status::Pending: return "pending";
        case Status::Applied: return "applied";
        case Status::AlreadyApplied: return "already-applied";
        case Status::RefusedBuild: return "refused-build";
        case Status::RefusedSlot: return "refused-slot";
        case Status::RefusedBytes: return "refused-bytes";
        case Status::WriteFailed: return "write-failed";
    }
    return "?";
}

bool OfflineHostExpected(Status s) {
    return s == Status::Pending || s == Status::Applied || s == Status::AlreadyApplied;
}

SiteState ClassifySite(const uint8_t* live, size_t siteIndex) {
    const pf::Site& site = pf::kSites[siteIndex];
    if (std::memcmp(live, site.original, site.len) == 0) return SiteState::Original;
    if (std::memcmp(live, site.patched, site.len) == 0) return SiteState::Patched;
    return SiteState::Foreign;
}

Plan PlanSites(const SiteState* states, size_t count) {
    size_t original = 0;
    size_t patched = 0;
    for (size_t i = 0; i < count; ++i) {
        if (states[i] == SiteState::Original) ++original;
        else if (states[i] == SiteState::Patched) ++patched;
    }
    if (count > 0 && original == count) return Plan::Write;
    if (count > 0 && patched == count) return Plan::AlreadyApplied;
    return Plan::Refuse;
}

Status PatchImage(uint8_t* image) {
    State& s = S();
    SiteState states[pf::kSiteCount];
    for (size_t i = 0; i < pf::kSiteCount; ++i) {
        const uint8_t* at = SiteAddress(image, i);
        states[i] = Readable(at, pf::kSites[i].len) ? ClassifySite(at, i) : SiteState::Foreign;
    }

    switch (PlanSites(states, pf::kSiteCount)) {
        case Plan::Refuse:
            log::Direct(LogLevel::Error, kModuleName,
                        "REFUSED: the offline patch sites are not in a state this build's table knows, so "
                        "NOTHING was written. The replay will not host an offline match. Per site:");
            LogRefusal(image, states);
            return Finish(Status::RefusedBytes);
        case Plan::AlreadyApplied:
            if (s.writtenCount != 0) {
                return Finish(Status::Applied);  // they are ours, from an earlier pass
            }
            log::Direct(LogLevel::Info, kModuleName,
                        "already applied: all %u sites hold the patched bytes (rlay.dll's -offline, %s); "
                        "nothing written",
                        static_cast<unsigned>(pf::kSiteCount),
                        coexist::IsModuleLoaded("rlay.dll") ? "rlay.dll is loaded" : "rlay.dll is NOT loaded");
            return Finish(Status::AlreadyApplied);
        case Plan::Write:
            break;
    }

    for (size_t i = 0; i < pf::kSiteCount; ++i) {
        const pf::Site& site = pf::kSites[i];
        if (WriteCode(SiteAddress(image, i), site.patched, site.len)) {
            continue;
        }
        log::Direct(LogLevel::Error, kModuleName,
                    "REFUSED: VirtualProtect failed at %s @0x%llX (error %lu); restoring the %u site(s) "
                    "already written",
                    site.name, static_cast<unsigned long long>(site.va), GetLastError(),
                    static_cast<unsigned>(i));
        for (size_t k = 0; k < i; ++k) {
            WriteCode(SiteAddress(image, k), pf::kSites[k].original, pf::kSites[k].len);
        }
        return Finish(Status::WriteFailed);
    }

    for (size_t i = 0; i < pf::kSiteCount; ++i) {
        const pf::Site& site = pf::kSites[i];
        s.written[i] = {site.va - pf::kPreferredImageBase, site.len};
        char original[32];
        char patched[32];
        log::Direct(LogLevel::Info, kModuleName, "applied %s @0x%llX: [%s] -> [%s] -- %s", site.name,
                    static_cast<unsigned long long>(site.va),
                    Hex(site.original, site.len, original, sizeof original),
                    Hex(site.patched, site.len, patched, sizeof patched), site.purpose);
    }
    s.writtenCount = pf::kSiteCount;
    return Finish(Status::Applied);
}

bool InstallAt(void** slot, void* expected, uint8_t* image) {
    State& s = S();
    if (*slot == reinterpret_cast<void*>(&ReplayCliApply)) {
        return true;
    }
    if (*slot != expected) {
        log::Direct(LogLevel::Error, kModuleName,
                    "REFUSED: GAME.VTABLE slot 0 holds %p, not CLI.APPLY %p -- another module owns it. "
                    "Nothing installed; the replay will not host an offline match",
                    *slot, expected);
        Finish(Status::RefusedSlot);
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &oldProtect)) {
        log::Direct(LogLevel::Error, kModuleName, "REFUSED: VirtualProtect on GAME.VTABLE failed (error %lu)",
                    GetLastError());
        Finish(Status::WriteFailed);
        return false;
    }
    s.real = reinterpret_cast<CliApplyFn>(expected);
    s.image = image;
    *slot = reinterpret_cast<void*>(&ReplayCliApply);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof *slot, oldProtect, &ignored);
    Finish(Status::Pending);
    return true;
}

bool InstallForThisProcess() {
    const wchar_t* line = GetCommandLineW();
    const bool rlayLoaded = coexist::IsModuleLoaded("rlay.dll");
    const bootstrap::SessionMode mode =
        bootstrap::ClassifySession(line, bootstrap::OpenRequestPresent(), rlayLoaded);
    const bool liveExempt =
        bootstrap::IsLiveOnlineExempt(mode, line != nullptr && bootstrap::HasSwitch(line, L"-offline"));
    if (!livesafety::OfflinePatchAllowed(mode == bootstrap::SessionMode::Replay, liveExempt)) {
        log::Direct(LogLevel::Info, kModuleName,
                    "offline patch: not applied (%s) -- session mode '%s'. Only a replay session (%ls set) "
                    "gets it; nothing was installed",
                    liveExempt ? "live" : "no replay requested", bootstrap::SessionModeName(mode),
                    bootstrap::OpenEnvVarName());
        return false;
    }

    auto* image = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != pf::kTimeDateStamp) {
        log::Direct(LogLevel::Error, kModuleName,
                    "REFUSED: echovr.exe TimeDateStamp 0x%08lX is not 0x%08X, the build the offline patch "
                    "sites were read from. Nothing installed",
                    static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), pf::kTimeDateStamp);
        Finish(Status::RefusedBuild);
        return false;
    }

    auto** slot = reinterpret_cast<void**>(image + (pf::kGameVtable - pf::kPreferredImageBase));
    void* cliApply = image + (pf::kCliApply - pf::kPreferredImageBase);
    if (!Readable(slot, sizeof *slot)) {
        log::Direct(LogLevel::Error, kModuleName, "REFUSED: GAME.VTABLE slot 0 at %p is not readable", slot);
        Finish(Status::RefusedSlot);
        return false;
    }
    if (!InstallAt(slot, cliApply, image)) {
        return false;
    }
    log::Direct(LogLevel::Info, kModuleName,
                "offline patch: armed for this replay session (rlay.dll %s at attach). The sites are "
                "written when CLI.APPLY runs, unless rlay's -offline already wrote them",
                rlayLoaded ? "loaded" : "not loaded yet");
    return true;
}

Status Current() { return S().status.load(std::memory_order_acquire); }

size_t WrittenSpans(Span* out, size_t cap) {
    if (Current() != Status::Applied) {
        return 0;
    }
    const State& s = S();
    const size_t n = s.writtenCount < cap ? s.writtenCount : cap;
    for (size_t i = 0; i < n; ++i) {
        out[i] = s.written[i];
    }
    return n;
}

void ResetForTest() {
    State& s = S();
    s.status.store(Status::NotRequested, std::memory_order_release);
    s.real = nullptr;
    s.image = nullptr;
    s.fired = 0;
    s.writtenCount = 0;
}

// Replay sessions only; see offlinepatch.h. Runs under the loader lock: one pointer store in .rdata.
REPLAY_ATTACH_HOOK("offline-patch", "session.offline_patch", InstallForThisProcess);

}  // namespace replay::offlinepatch
