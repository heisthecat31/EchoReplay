#include "foundation/ring/alloc_trap.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include <new>

#include "MinHook.h"

namespace replay {
namespace {

// Plain PODs with constant initialisers so MSVC puts them in static TLS. A thread_local needing dynamic
// initialisation would allocate on first touch per thread, inside the very detour meant to catch that.
thread_local uint32_t tArmed = 0;
thread_local uint32_t tAllocations = 0;
thread_local uint32_t tLocks = 0;
thread_local uint32_t tFileOps = 0;
thread_local const char* tFirstSite = nullptr;

uint32_t gLiveSites = 0;
bool gMinHookUp = false;

// The trap shares MinHook with hookmgr and with whatever else is loaded into the process, so it may only
// ever name its OWN targets: MH_ALL_HOOKS would arm and tear down every other module's detour too, and
// MH_Uninitialize would rip out hooks whose owners still believe they are installed (hookmgr.cpp refuses
// MH_Uninitialize at its own Shutdown for the same reason). Nine detours is the whole set installed below.
LPVOID gTargets[9];
uint32_t gTargetCount = 0;
uint32_t gArmedCount = 0;

inline void Hit(uint32_t* counter, const char* site) noexcept {
    if (tArmed == 0) {
        return;
    }
    ++*counter;
    if (!tFirstSite) {
        tFirstSite = site;
    }
}

using PfnHeapAlloc = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using PfnHeapReAlloc = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using PfnEnterCS = void(WINAPI*)(LPCRITICAL_SECTION);
using PfnAcquireSRW = void(WINAPI*)(PSRWLOCK);
using PfnWaitSingle = DWORD(WINAPI*)(HANDLE, DWORD);
using PfnCreateFileW = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PfnWriteFile = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using PfnReadFile = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

PfnHeapAlloc realHeapAlloc = nullptr;
PfnHeapReAlloc realHeapReAlloc = nullptr;
PfnEnterCS realEnterCS = nullptr;
PfnAcquireSRW realAcquireSRWEx = nullptr;
PfnAcquireSRW realAcquireSRWSh = nullptr;
PfnWaitSingle realWaitSingle = nullptr;
PfnCreateFileW realCreateFileW = nullptr;
PfnWriteFile realWriteFile = nullptr;
PfnReadFile realReadFile = nullptr;

LPVOID WINAPI HookHeapAlloc(HANDLE h, DWORD f, SIZE_T n) {
    Hit(&tAllocations, "HeapAlloc");
    return realHeapAlloc(h, f, n);
}
LPVOID WINAPI HookHeapReAlloc(HANDLE h, DWORD f, LPVOID p, SIZE_T n) {
    Hit(&tAllocations, "HeapReAlloc");
    return realHeapReAlloc(h, f, p, n);
}
void WINAPI HookEnterCS(LPCRITICAL_SECTION cs) {
    Hit(&tLocks, "EnterCriticalSection");
    realEnterCS(cs);
}
void WINAPI HookAcquireSRWEx(PSRWLOCK l) {
    Hit(&tLocks, "AcquireSRWLockExclusive");
    realAcquireSRWEx(l);
}
void WINAPI HookAcquireSRWSh(PSRWLOCK l) {
    Hit(&tLocks, "AcquireSRWLockShared");
    realAcquireSRWSh(l);
}
DWORD WINAPI HookWaitSingle(HANDLE h, DWORD ms) {
    Hit(&tLocks, "WaitForSingleObject");
    return realWaitSingle(h, ms);
}
HANDLE WINAPI HookCreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD fl, HANDLE t) {
    Hit(&tFileOps, "CreateFileW");
    return realCreateFileW(n, a, s, sa, c, fl, t);
}
BOOL WINAPI HookWriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, LPOVERLAPPED o) {
    Hit(&tFileOps, "WriteFile");
    return realWriteFile(h, b, n, w, o);
}
BOOL WINAPI HookReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD r, LPOVERLAPPED o) {
    Hit(&tFileOps, "ReadFile");
    return realReadFile(h, b, n, r, o);
}

bool Detour(const wchar_t* mod, const char* name, LPVOID replacement, LPVOID* original) {
    HMODULE m = GetModuleHandleW(mod);
    if (!m) {
        return false;
    }
    FARPROC target = GetProcAddress(m, name);
    if (!target) {
        return false;
    }
    if (gTargetCount >= sizeof(gTargets) / sizeof(gTargets[0])) {
        return false;
    }
    LPVOID site = reinterpret_cast<LPVOID>(target);
    if (MH_CreateHook(site, replacement, original) != MH_OK) {
        return false;
    }
    gTargets[gTargetCount++] = site;
    return true;
}

}  // namespace

uint32_t TrapInstall(uint32_t sites) {
    if (!gMinHookUp) {
        const MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
            return gLiveSites;
        }
        gMinHookUp = true;
    }

    // operator new is a link-time replacement rather than a detour, so there is no install call whose
    // return value could be checked -- it is either in the image or it is not. Probe it instead of
    // asserting it. This runs before MH_EnableHook, so nothing else can fire and the measurement is not
    // confounded by the heap detour.
    if ((sites & kTrapNew) && !(gLiveSites & kTrapNew)) {
        TrapTake();
        TrapArm();
        volatile char* probe = new char[1];
        delete[] probe;
        TrapDisarm();
        const TrapReport r = TrapTake();
        if (r.allocations > 0 && r.firstSite && strcmp(r.firstSite, "operator new") == 0) {
            gLiveSites |= kTrapNew;
        }
    }

    if ((sites & kTrapHeap) && !(gLiveSites & kTrapHeap)) {
        const bool ok =
            Detour(L"kernel32.dll", "HeapAlloc", &HookHeapAlloc, reinterpret_cast<LPVOID*>(&realHeapAlloc)) &&
            Detour(L"kernel32.dll", "HeapReAlloc", &HookHeapReAlloc, reinterpret_cast<LPVOID*>(&realHeapReAlloc));
        if (ok) {
            gLiveSites |= kTrapHeap;
        }
    }
    if ((sites & kTrapLock) && !(gLiveSites & kTrapLock)) {
        const bool ok =
            Detour(L"kernel32.dll", "EnterCriticalSection", &HookEnterCS,
                   reinterpret_cast<LPVOID*>(&realEnterCS)) &&
            Detour(L"kernel32.dll", "AcquireSRWLockExclusive", &HookAcquireSRWEx,
                   reinterpret_cast<LPVOID*>(&realAcquireSRWEx)) &&
            Detour(L"kernel32.dll", "AcquireSRWLockShared", &HookAcquireSRWSh,
                   reinterpret_cast<LPVOID*>(&realAcquireSRWSh)) &&
            Detour(L"kernel32.dll", "WaitForSingleObject", &HookWaitSingle,
                   reinterpret_cast<LPVOID*>(&realWaitSingle));
        if (ok) {
            gLiveSites |= kTrapLock;
        }
    }
    if ((sites & kTrapFile) && !(gLiveSites & kTrapFile)) {
        const bool ok =
            Detour(L"kernel32.dll", "CreateFileW", &HookCreateFileW,
                   reinterpret_cast<LPVOID*>(&realCreateFileW)) &&
            Detour(L"kernel32.dll", "WriteFile", &HookWriteFile, reinterpret_cast<LPVOID*>(&realWriteFile)) &&
            Detour(L"kernel32.dll", "ReadFile", &HookReadFile, reinterpret_cast<LPVOID*>(&realReadFile));
        if (ok) {
            gLiveSites |= kTrapFile;
        }
    }

    // Sites below gArmedCount are already live from an earlier TrapInstall; re-enabling one returns
    // MH_ERROR_ENABLED, which would read as a failure and drop every detour-backed site.
    for (uint32_t i = gArmedCount; i < gTargetCount; ++i) {
        if (MH_EnableHook(gTargets[i]) != MH_OK) {
            gLiveSites &= kTrapNew;
            break;
        }
        gArmedCount = i + 1;
    }
    return gLiveSites;
}

void TrapUninstall() {
    for (uint32_t i = 0; i < gTargetCount; ++i) {
        MH_DisableHook(gTargets[i]);
        MH_RemoveHook(gTargets[i]);
        gTargets[i] = nullptr;
    }
    gTargetCount = 0;
    gArmedCount = 0;
    gLiveSites = 0;
    // gMinHookUp stays true on purpose. MinHook's lifetime belongs to hookmgr, not to the trap.
}

uint32_t TrapLiveSites() { return gLiveSites; }

void TrapArm() noexcept { ++tArmed; }

void TrapDisarm() noexcept {
    if (tArmed) {
        --tArmed;
    }
}

bool TrapArmedHere() noexcept { return tArmed != 0; }

TrapReport TrapTake() noexcept {
    TrapReport r;
    r.allocations = tAllocations;
    r.locks = tLocks;
    r.fileOps = tFileOps;
    r.firstSite = tFirstSite;
    tAllocations = 0;
    tLocks = 0;
    tFileOps = 0;
    tFirstSite = nullptr;
    return r;
}

void TrapNoteAllocation(const char* site) noexcept { Hit(&tAllocations, site); }

}  // namespace replay

// ---- global operator new / delete replacement --------------------------------------------------
// The C++ half of the trap. These forward to malloc/free so they interoperate with any CRT-provided
// delete that survives in another translation unit.

void* operator new(size_t n) {
    replay::TrapNoteAllocation("operator new");
    void* p = malloc(n ? n : 1);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](size_t n) { return operator new(n); }

void* operator new(size_t n, const std::nothrow_t&) noexcept {
    replay::TrapNoteAllocation("operator new(nothrow)");
    return malloc(n ? n : 1);
}

void* operator new[](size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }

void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { free(p); }

#ifdef __cpp_aligned_new
void* operator new(size_t n, std::align_val_t a) {
    replay::TrapNoteAllocation("operator new(align)");
    void* p = _aligned_malloc(n ? n : 1, static_cast<size_t>(a));
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new[](size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    replay::TrapNoteAllocation("operator new(align,nothrow)");
    return _aligned_malloc(n ? n : 1, static_cast<size_t>(a));
}
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t& t) noexcept {
    return operator new(n, a, t);
}
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { _aligned_free(p); }
#endif
