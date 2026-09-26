// PageSpanReadable is the predicate under every guarded engine read in the plugin, and the only
// one whose wrong answer is silent: a false "yes" is a crash in the game, a false "no" is a feature
// that quietly never runs. The cases below build each memory shape the probe claims to survive --
// committed, freed, non-canonical, guarded, and a span whose MIDDLE page is missing -- rather than
// testing the happy path twice.
//
// The guard-page cases are the reason this file exists. Reading a PAGE_GUARD page satisfies the
// guard, so a probe that did not re-arm would disarm a thread stack's growth page and then report
// it readable on the next ask. "Still guarded after the probe" and "a second probe still refuses"
// are two different statements and both are made here.
//
// No commas in test case names: doctest reads a comma in --test-case as a filter separator.

#include <doctest.h>

#include <windows.h>

#include <stdint.h>

#include "bootstrap/pageprobe.h"

using replay::PageSpanReadable;
using replay::ProbeFaultCount;

namespace {

size_t PageSize() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 4096u;
}

bool IsGuarded(void* page) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(page, &mbi, sizeof mbi) != sizeof mbi) return false;
    return (mbi.Protect & PAGE_GUARD) != 0;
}

}  // namespace

TEST_CASE("a committed page reads and a refused one does not") {
    const size_t page = PageSize();
    uint8_t* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE));
    REQUIRE(p != nullptr);
    p[0] = 1;
    p[page] = 2;

    // The positive control: without it every refusal below could be the probe refusing everything.
    CHECK(PageSpanReadable(p, page * 2));
    CHECK(PageSpanReadable(p + page - 1, 2));

    CHECK_FALSE(PageSpanReadable(nullptr, 16));
    CHECK_FALSE(PageSpanReadable(p, 0));
    CHECK_FALSE(PageSpanReadable(p, ~static_cast<size_t>(0)));  // wraps the address space

    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("a freed page is refused and charges a fault") {
    const size_t page = PageSize();
    uint8_t* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE));
    REQUIRE(p != nullptr);
    p[0] = 7;
    REQUIRE(PageSpanReadable(p, page));

    const uint64_t before = ProbeFaultCount();
    REQUIRE(VirtualFree(p, 0, MEM_RELEASE) != 0);
    CHECK_FALSE(PageSpanReadable(p, page));
    CHECK(ProbeFaultCount() == before + 1);
}

TEST_CASE("a reserved but uncommitted page is refused") {
    const size_t page = PageSize();
    uint8_t* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(p != nullptr);
    CHECK_FALSE(PageSpanReadable(p, page));
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("a non-canonical address is refused") {
    // Bit 63 set with the rest clear is non-canonical on x86-64: the touch raises an access
    // violation rather than a page fault, which is the third code the filter accepts.
    void* wild = reinterpret_cast<void*>(static_cast<uintptr_t>(1) << 63);
    CHECK_FALSE(PageSpanReadable(wild, 8));
    CHECK_FALSE(PageSpanReadable(reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000)), 8));
}

TEST_CASE("a span whose middle page is missing is refused") {
    const size_t page = PageSize();
    uint8_t* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 3, MEM_RESERVE,
                                                       PAGE_NOACCESS));
    REQUIRE(base != nullptr);
    REQUIRE(VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE) != nullptr);
    REQUIRE(VirtualAlloc(base + page * 2, page, MEM_COMMIT, PAGE_READWRITE) != nullptr);
    base[0] = 1;
    base[page * 2] = 1;

    // Both ends read; only the hole between them does not. A probe that touched the first and last
    // byte alone -- which is what this plugin shipped before PERF-4 F5 -- passes this span.
    CHECK(PageSpanReadable(base, page));
    CHECK(PageSpanReadable(base + page * 2, page));
    CHECK_FALSE(PageSpanReadable(base, page * 3));

    VirtualFree(base, 0, MEM_RELEASE);
}

TEST_CASE("a guard page is refused and stays guarded") {
    const size_t page = PageSize();
    uint8_t* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE));
    REQUIRE(p != nullptr);
    p[0] = 3;
    DWORD old = 0;
    REQUIRE(VirtualProtect(p, page, PAGE_READWRITE | PAGE_GUARD, &old) != 0);
    REQUIRE(IsGuarded(p));

    CHECK_FALSE(PageSpanReadable(p, 8));
    CHECK(IsGuarded(p));          // the probe satisfied the guard; the re-arm put it back
    CHECK_FALSE(PageSpanReadable(p, 8));   // ...so the second ask still refuses
    CHECK(IsGuarded(p));

    // The page after the guard is ordinary memory and must still read, or "refuses everything"
    // would pass the two checks above.
    CHECK(PageSpanReadable(p + page, page));

    REQUIRE(VirtualProtect(p, page, PAGE_READWRITE, &old) != 0);
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("a guard page inside a longer span is refused and re-armed") {
    const size_t page = PageSize();
    uint8_t* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 3, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_READWRITE));
    REQUIRE(p != nullptr);
    DWORD old = 0;
    REQUIRE(VirtualProtect(p + page, page, PAGE_READWRITE | PAGE_GUARD, &old) != 0);

    const uint64_t before = ProbeFaultCount();
    CHECK_FALSE(PageSpanReadable(p, page * 3));
    CHECK(ProbeFaultCount() == before + 1);
    CHECK(IsGuarded(p + page));

    REQUIRE(VirtualProtect(p + page, page, PAGE_READWRITE, &old) != 0);
    CHECK(PageSpanReadable(p, page * 3));
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST_CASE("HOST-SYNC-3: QueriedSpans answers every memory shape without a single fault") {
    const size_t page = PageSize();
    uint8_t* const base =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, 4 * page, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(base != nullptr);
    REQUIRE(VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE) != nullptr);
    REQUIRE(VirtualAlloc(base + 2 * page, page, MEM_COMMIT, PAGE_READONLY | PAGE_GUARD) != nullptr);
    REQUIRE(VirtualAlloc(base + 3 * page, page, MEM_COMMIT, PAGE_NOACCESS) != nullptr);

    const uint64_t faultsBefore = ProbeFaultCount();
    replay::QueriedSpans spans;
    CHECK(spans.Readable(base, page));
    CHECK(spans.Readable(base + 16, 64));
    CHECK_FALSE(spans.Readable(base + page - 8, 16));    // runs into the reserved page
    CHECK_FALSE(spans.Readable(base + page, 8));         // reserved, never committed
    CHECK_FALSE(spans.Readable(base + 2 * page, 8));     // guarded
    CHECK(IsGuarded(base + 2 * page));                   // and still guarded: nothing touched it
    CHECK_FALSE(spans.Readable(base + 3 * page, 8));     // no access
    CHECK_FALSE(spans.Readable(reinterpret_cast<const void*>(uintptr_t(0x20)), 8));
    CHECK_FALSE(spans.Readable(reinterpret_cast<const void*>(~uintptr_t(0)), 1));
    CHECK_FALSE(spans.Readable(reinterpret_cast<const void*>(uintptr_t(0x8000000000000000ull)), 8));
    CHECK_FALSE(spans.Readable(nullptr, 8));
    CHECK_FALSE(spans.Readable(base, 0));
    CHECK(ProbeFaultCount() == faultsBefore);

    // A proved region is not asked about again; refusals are never remembered.
    const uint32_t asked = spans.Queries();
    CHECK(spans.Readable(base + 128, 8));
    CHECK(spans.Queries() == asked);
    CHECK_FALSE(spans.Readable(base + page, 8));
    CHECK(spans.Queries() == asked + 1);

    VirtualFree(base, 0, MEM_RELEASE);
}
