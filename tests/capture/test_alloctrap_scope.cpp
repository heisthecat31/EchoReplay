// W2-A. The allocation trap shares one MinHook instance with hookmgr and with every other module in the
// process. Before this arm existed, TrapInstall armed MH_ALL_HOOKS and TrapUninstall called
// MH_Uninitialize, so turning `capture.trap` on would have armed -- and turning it off would have torn out
// -- the VOIP, offlinelock and session-cmdline detours, whose owners would still believe they were
// installed. hookmgr.cpp refuses MH_Uninitialize for exactly this reason at its own Shutdown.
//
// The foreign hook here is installed with raw MinHook rather than through hookmgr on purpose.
// hookmgr::Create runs the build guard first, and the build guard has not run in a test process, so every
// hookmgr::Create in a unit test returns RefusedByBuild (tests/bootstrap/test_hookmgr.cpp pins that). A
// refused hook is not installed, so asserting it "survived" would assert nothing. Raw MH_CreateHook +
// MH_EnableHook is what hookmgr::Create/Arm do underneath, so this is the same object under test with the
// unreachable guard removed -- and it is the only form of the control that can actually go red.

#include "doctest.h"

#include <windows.h>

#include "MinHook.h"
#include "bootstrap/hookmgr.h"
#include "foundation/ring/alloc_trap.h"

namespace {

// A victim with an observable return value, so "still detoured" is read from behaviour and not merely from
// a status word. noinline + volatile keeps the call from being folded away at the call site.
int g_victimSalt = 0;

__declspec(noinline) int ForeignVictim(int n) {
    volatile int salt = g_victimSalt;
    return n + 1 + salt;
}

using PfnVictim = int (*)(int);
PfnVictim g_realVictim = nullptr;

__declspec(noinline) int ForeignDetour(int n) { return 1000 + g_realVictim(n); }

PfnVictim VictimAddress() { return &ForeignVictim; }

bool ForeignHookIsLive() { return ForeignVictim(4) == 1005; }

// A hook another module has CREATED but deliberately NOT armed yet. hookmgr splits Create (safe from
// DllMain) from Arm (worker thread) precisely so a ledger row sits in this state for a while, and
// MH_EnableHook(MH_ALL_HOOKS) arms it anyway -- on the loader lock, which is the deadlock hookmgr's
// header spends its whole comment avoiding. An ALREADY-armed foreign hook cannot witness this: MinHook's
// ALL_HOOKS path skips entries that are already enabled, so it returns MH_OK and nothing looks wrong.
int g_pendingSalt = 0;

__declspec(noinline) int PendingVictim(int n) {
    volatile int salt = g_pendingSalt;
    return n + 2 + salt;
}

PfnVictim g_realPending = nullptr;

__declspec(noinline) int PendingDetour(int n) { return 2000 + g_realPending(n); }

PfnVictim PendingAddress() { return &PendingVictim; }

bool PendingHookIsLive() { return PendingVictim(4) == 2006; }

}  // namespace

TEST_CASE("the alloc trap arms and removes only its own sites") {
    // hookmgr owns MinHook's lifetime; initialising through it is the same order the DLL uses.
    REQUIRE(replay::hookmgr::Initialize());
    REQUIRE(replay::hookmgr::Initialized());

    // A detour this module does not own, standing in for VOIP / offlinelock / session-cmdline.
    REQUIRE(MH_CreateHook(reinterpret_cast<LPVOID>(VictimAddress()), reinterpret_cast<LPVOID>(&ForeignDetour),
                          reinterpret_cast<LPVOID*>(&g_realVictim)) == MH_OK);
    REQUIRE(MH_EnableHook(reinterpret_cast<LPVOID>(VictimAddress())) == MH_OK);

    // The control must be able to fail: prove the foreign hook is genuinely live BEFORE the trap runs.
    // Without this, a victim that was never detoured would satisfy every assertion below vacuously.
    REQUIRE(ForeignHookIsLive());

    REQUIRE(MH_CreateHook(reinterpret_cast<LPVOID>(PendingAddress()), reinterpret_cast<LPVOID>(&PendingDetour),
                          reinterpret_cast<LPVOID*>(&g_realPending)) == MH_OK);
    REQUIRE_FALSE(PendingHookIsLive());

    SUBCASE("TrapInstall does not arm foreign hooks, and TrapUninstall does not remove them") {
        const uint32_t live = replay::TrapInstall(replay::kTrapAll);
        CHECK((live & replay::kTrapHeap) != 0);

        // The trap's own sites work.
        replay::TrapTake();
        replay::TrapArm();
        volatile char* p = new char[64];
        delete[] p;
        replay::TrapDisarm();
        const replay::TrapReport r = replay::TrapTake();
        CHECK(r.allocations > 0);

        // THE OTHER ASSERTION THIS FILE EXISTS FOR. MH_EnableHook(MH_ALL_HOOKS) arms this one too.
        CHECK_FALSE(PendingHookIsLive());

        // The foreign hook is untouched by the trap being on.
        CHECK(ForeignHookIsLive());

        replay::TrapUninstall();
        CHECK(replay::TrapLiveSites() == 0u);

        // THE ASSERTION THIS FILE EXISTS FOR. With MH_DisableHook(MH_ALL_HOOKS)/MH_Uninitialize, the
        // foreign detour is gone here and ForeignVictim(4) is 5.
        CHECK(ForeignHookIsLive());

        // And MinHook itself was not torn down under hookmgr's feet. MH_Initialize on a live MinHook
        // reports ALREADY_INITIALIZED; after MH_Uninitialize it would report MH_OK.
        CHECK(MH_Initialize() == MH_ERROR_ALREADY_INITIALIZED);
        CHECK(replay::hookmgr::Initialized());
    }

    SUBCASE("a second install/uninstall cycle is still scoped to its own sites") {
        // Re-arming a site that MinHook already has enabled returns MH_ERROR_ENABLED. If that were read as
        // a failure the second cycle would silently install nothing.
        CHECK((replay::TrapInstall(replay::kTrapAll) & replay::kTrapHeap) != 0);
        replay::TrapUninstall();
        CHECK((replay::TrapInstall(replay::kTrapAll) & replay::kTrapHeap) != 0);
        CHECK(ForeignHookIsLive());
        replay::TrapUninstall();
        CHECK(ForeignHookIsLive());
    }

    MH_RemoveHook(reinterpret_cast<LPVOID>(PendingAddress()));
    MH_DisableHook(reinterpret_cast<LPVOID>(VictimAddress()));
    MH_RemoveHook(reinterpret_cast<LPVOID>(VictimAddress()));
}
