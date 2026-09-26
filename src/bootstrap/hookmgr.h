// B-4: MinHook's lifecycle, and the rule that we never overwrite somebody else's patch.
//
// S4.5.3 rule 4: "No hook site is claimed exclusively. MinHook trampolines chain; a raw byte patch does
// not. We install no raw byte patches. If a hook site's prologue byte-guard already differs, we refuse
// that hook and log - we never overwrite another tool's patch." plugins\ is a shared community folder,
// so a site whose first bytes are not the recorded ones is the normal way another mod announces itself.
//
// ★ THE LOADER-LOCK SPLIT, which is why creating and arming are two calls.
//
//   Create  MH_Initialize and MH_CreateHook allocate from MinHook's own private heap and VirtualAlloc a
//           trampoline. Neither loads a DLL and neither touches another thread, so both are safe from
//           DllMain, which is what the loader contract asks for ("install hooks inline, CreateThread,
//           return").
//   Arm     MH_EnableHook calls Freeze(), which is CreateToolhelp32Snapshot plus SuspendThread on every
//           other thread in the process. Suspending a thread that holds a lock and then taking that lock
//           is the classic deadlock, and doing it while we hold the loader lock narrows the window
//           without closing it. Arm() therefore belongs on the worker thread.
//
// W1-C arms nothing inline. ArmInline exists for the one row that has no choice - ledger 1X.1 hooks
// GetCommandLine before the game parses it - and it names the risk at the call site rather than hiding
// it behind a default.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {

enum class HookStatus : uint8_t {
    Absent,
    Created,             // trampoline built, detour not yet live
    Armed,
    RefusedByConfig,     // its ledger row is switched off
    RefusedByBuild,      // the build guard did not accept this executable
    RefusedByPrologue,   // the site does not hold the recorded bytes - someone else got here first
    RefusedByMinHook,    // MinHook itself said no
};

const char* HookStatusName(HookStatus status);

namespace hookmgr {

// Safe from DllMain. Idempotent.
bool Initialize();
bool Initialized();

// Builds the trampoline for one site after checking (a) the build guard passed, (b) the ledger row's
// config key is on, and (c) the mapped bytes are the recorded prologue. `trampoline` receives the address
// to call through for the original. Returns the status, which is also what Status(factId) reports later.
HookStatus Create(const char* factId, uint64_t pcVa, const uint8_t* prologue, size_t prologueLen,
                  void* detour, void** trampoline, const char* configKey);

// Worker thread. Enables a created hook.
HookStatus Arm(const char* factId);

// DllMain only, and only for a row that must be live before the game reads the thing it guards. Same
// work as Arm with the loader lock held; see the header comment.
HookStatus ArmInline(const char* factId);

HookStatus Status(const char* factId);

// One registered site, as SC7's in-process reader needs to see it: where the patch would land, which
// ledger row declares it, and whether it is actually armed. `ledgerRow` is the row the site's config key
// claims in config_keys.inc, or null for a key that claims none - the reader's attribution is by ROW,
// because the ledger and the harness both speak rows, not fact ids.
struct Site {
    const char* factId;
    uint64_t    pcVa;
    size_t      prologueLen;
    const char* ledgerRow;
    HookStatus  status;
};

bool SiteAt(size_t i, Site& out);

size_t     Count();
size_t     ArmedCount();
const char* FactIdAt(size_t i);

// Disables and removes every hook, then MH_Uninitialize. Never called from DLL_PROCESS_DETACH at process
// exit: by then other threads are gone and unpatching buys nothing but a chance to fault.
void Shutdown();

void ResetForTest();

}  // namespace hookmgr
}  // namespace replay
