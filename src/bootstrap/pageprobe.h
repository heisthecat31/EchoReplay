#pragma once

// The readability probe every guarded read in this plugin is built on, in the form that does not
// cost a syscall.
//
// ==============================================================================================
// WHY THIS IS A TOUCH AND NOT A VirtualQuery (PERF-4)
// ==============================================================================================
// Six files here had each written the same probe: walk [p, p+len) with VirtualQuery and refuse
// unless every region is MEM_COMMIT, readable and unguarded. In THIS process that question costs
// about 100 us to ask, because echovr.exe's address space is enormous and NtQueryVirtualMemory
// takes its lock and walks its VAD tree. PERF-4 sampled the engine's frame thread 500 times a
// second and found 94-98 % of its ntdll time -- and ~90 % of the whole thread -- inside
// ZwQueryVirtualMemory, called from this plugin about 10,000 times a second.
//
// Touching one byte per page under a structured-exception filter answers the SAME question, in
// nanoseconds, and answers it about a nearer instant: VirtualQuery describes the map as it was
// when the query ran and the caller's read happens after it, which is the race
// src/engine/tick.cpp:126 and src/engine/entity.h:1373 both already write down. An unmapped,
// reserved, no-access or guarded page faults on the touch and the filter turns that into the same
// `false` the query returned.
//
// The one behaviour that is NOT identical: reading a PAGE_GUARD page satisfies the guard as well
// as reporting it. A probe that lands on a thread stack's guard page would therefore refuse once
// and then, having silently disarmed the page, say `true` on the next probe -- and worse, that
// thread's stack would no longer grow. So the guard-page arm RE-ARMS the page it tripped before it
// answers. The re-arm costs a VirtualQuery and a VirtualProtect, which is affordable precisely
// because it is on the fault path: the steady state is the touch and nothing else.

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace replay {

#if defined(_WIN32)
namespace detail {

inline volatile LONG64 g_probeFaults = 0;

// The filter keeps the two things the handler needs: WHICH exception, and -- for a guard page --
// the address that tripped it. ExceptionInformation[1] is the referenced address on all three of
// the codes accepted here.
inline LONG TouchFilter(EXCEPTION_POINTERS* ep, DWORD* code, uintptr_t* at) {
    const DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_GUARD_PAGE &&
        c != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    *code = c;
    *at = ep->ExceptionRecord->NumberParameters >= 2
              ? static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1])
              : 0u;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Put the guard back on the page the touch just satisfied. Read-only about everything else: the
// protection comes from the page itself, so a guarded read-only page goes back guarded read-only.
inline void RearmGuardPage(uintptr_t at) {
    if (at == 0) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t granularity = si.dwPageSize ? si.dwPageSize : 4096u;
    void* page = reinterpret_cast<void*>(at & ~(granularity - 1));
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(page, &mbi, sizeof mbi) != sizeof mbi) return;
    if (mbi.State != MEM_COMMIT) return;
    DWORD old = 0;
    VirtualProtect(page, 1, mbi.Protect | PAGE_GUARD, &old);
}

// Separate, and free of anything that unwinds, because __try may not share a frame with a C++
// object that has a destructor.
inline bool TouchSpan(const uint8_t* p, size_t len) {
    DWORD code = 0;
    uintptr_t at = 0;
    __try {
        const volatile uint8_t* q = reinterpret_cast<const volatile uint8_t*>(p);
        for (size_t off = 0; off < len; off += 4096) {
            (void)q[off];
        }
        (void)q[len - 1];
        return true;
    } __except (TouchFilter(GetExceptionInformation(), &code, &at)) {
        InterlockedIncrement64(&g_probeFaults);
        if (code == EXCEPTION_GUARD_PAGE) RearmGuardPage(at);
        return false;
    }
}

}  // namespace detail

// Every probe fault since the process started. A per-frame faulting probe is a performance defect
// of its own -- each fault is a kernel round trip -- so the pacing line prints the rate.
inline uint64_t ProbeFaultCount() {
    return static_cast<uint64_t>(InterlockedCompareExchange64(&detail::g_probeFaults, 0, 0));
}
#else
inline uint64_t ProbeFaultCount() { return 0; }
#endif

// The probe for a CONTENT SCAN: a walk that treats arbitrary qwords as candidate pointers, where
// most candidates are integers, floats or the tail of a reserved-but-uncommitted arena. Touching
// those is a first-chance fault each (HOST-SYNC-3: eight logged in one admission diagnostic, every
// one a garbage qword), so this one asks the memory manager and never touches. A query is ~100 us,
// which is why the regions it has proved are remembered for the life of the object: one scan pays
// once per region, not once per read. For cold, bounded scans only -- the touch above stays the
// probe for every per-frame read.
class QueriedSpans {
public:
    bool Readable(const void* p, size_t len) {
#if defined(_WIN32)
        const uintptr_t start = reinterpret_cast<uintptr_t>(p);
        if (p == nullptr || len == 0 || start + len < start) return false;
        const uintptr_t end = start + len;
        if (start < kLowestUserAddress || end > kHighestUserAddress) return false;
        uintptr_t at = start;
        while (at < end) {
            const Region* known = Find(at);
            if (known == nullptr) known = Query(at);
            if (known == nullptr) return false;
            at = known->hi;
        }
        return true;
#else
        (void)p;
        (void)len;
        return false;
#endif
    }

    uint32_t Queries() const { return queries_; }

private:
    // Below the first 64 KiB nothing is ever mapped; above the user half nothing a user-mode read
    // can reach. A qword outside is refused without a syscall.
    static constexpr uintptr_t kLowestUserAddress = 0x10000u;
    static constexpr uintptr_t kHighestUserAddress = uintptr_t(0x7FFFFFFF0000ull);
    static constexpr uint32_t kRegionCap = 32u;

    struct Region {
        uintptr_t lo = 0;
        uintptr_t hi = 0;
    };

    const Region* Find(uintptr_t at) const {
        for (uint32_t i = 0; i < count_; ++i) {
            if (at >= regions_[i].lo && at < regions_[i].hi) return &regions_[i];
        }
        return nullptr;
    }

#if defined(_WIN32)
    const Region* Query(uintptr_t at) {
        ++queries_;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof mbi) != sizeof mbi) {
            return nullptr;
        }
        constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & kReadable) == 0 ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return nullptr;
        }
        Region& slot = regions_[next_];
        next_ = (next_ + 1) % kRegionCap;
        if (count_ < kRegionCap) ++count_;
        slot.lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        slot.hi = slot.lo + mbi.RegionSize;
        return &slot;
    }
#endif

    Region regions_[kRegionCap];
    uint32_t count_ = 0;
    uint32_t next_ = 0;
    uint32_t queries_ = 0;
};

// True when every page of [p, p+len) can be read right now. A null pointer, a zero length ("nothing
// was asked" is not "safe to read") and a span that wraps the address space are all false.
inline bool PageSpanReadable(const void* p, size_t len) {
#if defined(_WIN32)
    if (p == nullptr || len == 0) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(p);
    if (start + len < start) {
        return false;
    }
    return detail::TouchSpan(static_cast<const uint8_t*>(p), len);
#else
    (void)p;
    (void)len;
    return false;
#endif
}

}  // namespace replay
