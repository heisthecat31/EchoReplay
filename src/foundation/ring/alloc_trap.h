#pragma once

#include <stdint.h>

namespace replay {

// The S6.4 tick-thread trap: zero heap allocations, zero lock acquisitions, zero syscalls while armed.
//
// It is ordinary release code behind a runtime switch rather than a debug build, because measuring the
// timing half in release and the zero-allocation half in a different binary means the two halves of one
// gate never hold simultaneously (S6.4 enforcement note 2).
//
// Arming is per thread, so the worker side keeps allocating normally while the producer is guarded.

enum TrapSite : uint32_t {
    kTrapNew = 1u << 0,   // replaced global operator new / new[]
    kTrapHeap = 1u << 1,  // HeapAlloc / HeapReAlloc, which is where malloc and the CRT land
    kTrapLock = 1u << 2,  // EnterCriticalSection / AcquireSRWLock* / WaitForSingleObject
    kTrapFile = 1u << 3,  // CreateFileW / WriteFile / ReadFile
    kTrapAll = kTrapNew | kTrapHeap | kTrapLock | kTrapFile,
};

struct TrapReport {
    uint32_t allocations;
    uint32_t locks;
    uint32_t fileOps;
    const char* firstSite;  // static string, or nullptr when nothing fired
};

// Installs the detours. Returns the mask of sites that are actually live; a caller that wants a
// trustworthy verdict must compare it against what it asked for, because a site that failed to install
// is a trap that silently cannot fire. Idempotent.
uint32_t TrapInstall(uint32_t sites = kTrapAll);
void TrapUninstall();
uint32_t TrapLiveSites();

void TrapArm() noexcept;
void TrapDisarm() noexcept;
bool TrapArmedHere() noexcept;

// Reads and clears the calling thread's counters.
TrapReport TrapTake() noexcept;

// Records one allocation against the calling thread when it is armed. The replaced global operator new
// calls it; a test can call it directly to prove the trap is not merely silent.
void TrapNoteAllocation(const char* site) noexcept;

class TrapGuard {
public:
    TrapGuard() noexcept { TrapArm(); }
    ~TrapGuard() noexcept { TrapDisarm(); }
    TrapGuard(const TrapGuard&) = delete;
    TrapGuard& operator=(const TrapGuard&) = delete;
};

}  // namespace replay
