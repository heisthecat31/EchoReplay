#include "bootstrap/hookmgr.h"

#include <windows.h>

#include <cstring>

#include <MinHook.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/log.h"

namespace replay {

const char* HookStatusName(HookStatus status) {
    switch (status) {
        case HookStatus::Absent:            return "absent";
        case HookStatus::Created:           return "created";
        case HookStatus::Armed:             return "armed";
        case HookStatus::RefusedByConfig:   return "refused: its config key is off";
        case HookStatus::RefusedByBuild:    return "refused: the build guard did not accept this exe";
        case HookStatus::RefusedByPrologue: return "refused: the site does not hold the recorded bytes";
        case HookStatus::RefusedByMinHook:  return "refused: MinHook declined";
    }
    return "?";
}

namespace hookmgr {
namespace {

// The ledger has three Level-2 rows and one Level-1 fallback; 1X adds two. Sixteen is generous and fixed,
// because a growable table on this path would mean allocating from DllMain.
constexpr size_t kMaxHooks = 16;

struct Entry {
    const char* factId;
    void*       target;
    void*       trampoline;   // what Create promised the caller; replayed on a duplicate call
    HookStatus  status;
    uint64_t    pcVa;         // the fact's address at the declared image base; SC7 rebases it itself
    size_t      prologueLen;
    const char* configKey;    // the ledger row's switch, and how the row is looked up later
};

struct State {
    bool   initialized;
    size_t count;
    Entry  entries[kMaxHooks];
};

State g_state;

Entry* Find(const char* factId) {
    for (size_t i = 0; i < g_state.count; ++i) {
        if (std::strcmp(g_state.entries[i].factId, factId) == 0) {
            return &g_state.entries[i];
        }
    }
    return nullptr;
}

HookStatus Enable(const char* factId, bool underLoaderLock) {
    Entry* entry = Find(factId);
    if (entry == nullptr) {
        return HookStatus::Absent;
    }
    if (entry->status == HookStatus::Armed) {
        return HookStatus::Armed;
    }
    if (entry->status != HookStatus::Created) {
        return entry->status;
    }
    const MH_STATUS rc = MH_EnableHook(entry->target);
    if (rc != MH_OK) {
        entry->status = HookStatus::RefusedByMinHook;
        log::Direct(LogLevel::Error, "hookmgr", "%s: MH_EnableHook returned %s", factId,
                    MH_StatusToString(rc));
        return entry->status;
    }
    entry->status = HookStatus::Armed;
    log::Direct(LogLevel::Info, "hookmgr", "%s: armed%s", factId,
                underLoaderLock ? " inline, under the loader lock" : "");
    return HookStatus::Armed;
}

}  // namespace

bool Initialize() {
    if (g_state.initialized) {
        return true;
    }
    // HeapCreate plus one VirtualAlloc. No DLL is loaded and no thread is touched, which is what makes
    // this safe from DllMain; MH_EnableHook is the call that is not.
    const MH_STATUS rc = MH_Initialize();
    if (rc != MH_OK && rc != MH_ERROR_ALREADY_INITIALIZED) {
        log::Direct(LogLevel::Error, "hookmgr", "MH_Initialize returned %s", MH_StatusToString(rc));
        return false;
    }
    g_state.initialized = true;
    return true;
}

bool Initialized() { return g_state.initialized; }

HookStatus Create(const char* factId, uint64_t pcVa, const uint8_t* prologue, size_t prologueLen,
                  void* detour, void** trampoline, const char* configKey) {
    if (Entry* existing = Find(factId)) {
        // The header promises the out-param is written. A refused site has no trampoline, so the honest
        // value is null - never the caller's uninitialised variable.
        if (trampoline != nullptr) {
            *trampoline = existing->trampoline;
        }
        return existing->status;
    }
    if (g_state.count >= kMaxHooks) {
        log::Direct(LogLevel::Error, "hookmgr", "%s: the hook table is full", factId);
        return HookStatus::RefusedByMinHook;
    }

    Entry& entry = g_state.entries[g_state.count];
    entry.factId = factId;
    entry.pcVa = pcVa;
    entry.prologueLen = prologueLen;
    entry.configKey = configKey;
    entry.target = nullptr;
    entry.trampoline = nullptr;
    entry.status = HookStatus::Absent;
    if (trampoline != nullptr) {
        *trampoline = nullptr;
    }

    // The order of these three refusals is the order of their severity, so the log names the real reason
    // rather than the first one that happened to be checked.
    if (factId == nullptr || detour == nullptr) {
        entry.factId = factId != nullptr ? factId : "<unnamed site>";
        entry.status = HookStatus::RefusedByMinHook;
    } else if (!buildguard::Passed()) {
        entry.status = HookStatus::RefusedByBuild;
    } else if (configKey == nullptr) {
        // S4.4: "Every row is disableable by a config key." A hook arriving without one is an
        // intervention nobody can switch off, and the ledger could not describe it either.
        entry.status = HookStatus::RefusedByConfig;
    } else if (!config::RowDeclared(configKey)) {
        entry.status = HookStatus::RefusedByConfig;
    } else if (!buildguard::VerifySite(factId, pcVa, prologue, prologueLen)) {
        entry.status = HookStatus::RefusedByPrologue;
    } else if (!Initialize()) {
        entry.status = HookStatus::RefusedByMinHook;
    } else {
        void* target = const_cast<void*>(buildguard::MappedAddress(pcVa));
        const MH_STATUS rc = target != nullptr ? MH_CreateHook(target, detour, trampoline)
                                               : MH_ERROR_NOT_EXECUTABLE;
        if (rc == MH_OK) {
            entry.target = target;
            entry.trampoline = trampoline != nullptr ? *trampoline : nullptr;
            entry.status = HookStatus::Created;
        } else {
            entry.status = HookStatus::RefusedByMinHook;
            log::Direct(LogLevel::Error, "hookmgr", "%s: MH_CreateHook returned %s", factId,
                        MH_StatusToString(rc));
        }
    }

    ++g_state.count;
    if (entry.status != HookStatus::Created) {
        log::Direct(LogLevel::Warn, "hookmgr", "%s: %s", factId, HookStatusName(entry.status));
    }
    return entry.status;
}

HookStatus Arm(const char* factId) { return Enable(factId, false); }
HookStatus ArmInline(const char* factId) { return Enable(factId, true); }

HookStatus Status(const char* factId) {
    const Entry* entry = Find(factId);
    return entry != nullptr ? entry->status : HookStatus::Absent;
}

bool SiteAt(size_t i, Site& out) {
    if (i >= g_state.count) {
        return false;
    }
    const Entry& entry = g_state.entries[i];
    out.factId = entry.factId;
    out.pcVa = entry.pcVa;
    out.prologueLen = entry.prologueLen;
    out.status = entry.status;
    out.ledgerRow = nullptr;
    if (entry.configKey != nullptr) {
        if (const ConfigKey* key = FindConfigKey(entry.configKey)) {
            out.ledgerRow = key->ledgerRow;
        }
    }
    return true;
}

size_t Count() { return g_state.count; }

size_t ArmedCount() {
    size_t n = 0;
    for (size_t i = 0; i < g_state.count; ++i) {
        if (g_state.entries[i].status == HookStatus::Armed) {
            ++n;
        }
    }
    return n;
}

const char* FactIdAt(size_t i) { return i < g_state.count ? g_state.entries[i].factId : nullptr; }

void Shutdown() {
    if (!g_state.initialized) {
        return;
    }
    for (size_t i = 0; i < g_state.count; ++i) {
        if (g_state.entries[i].status == HookStatus::Armed) {
            MH_DisableHook(g_state.entries[i].target);
            g_state.entries[i].status = HookStatus::Created;
        }
    }
    // Deliberately NOT MH_Uninitialize. That removes EVERY hook in the process, and the egress
    // blocker's trampolines are not ours to remove - they are armed before the build guard has a
    // verdict and must survive a refusal. Removing our own entries is the whole of this packet's
    // obligation; MinHook's private heap outliving us is a few pages, not a hook.
    g_state.count = 0;
}

void ResetForTest() {
    g_state.initialized = false;
    g_state.count = 0;
    std::memset(g_state.entries, 0, sizeof g_state.entries);
}

}  // namespace hookmgr
}  // namespace replay
