// W1-H. A fabricated echovr.exe for the build guards, shared by test_guardsites.cpp (the write
// point's six sites) and test_viewpose.cpp (the read's two, plus GAME_SINGLETON_RVA).
//
// A reserved-but-uncommitted VirtualAlloc range stands in for a mapped image: VirtualQuery answers
// MEM_RESERVE, which is exactly what it answers for an address past the end of a shorter
// echovr.exe. `slide()` is the module's own rebasing term, so pointing a guard at it puts the whole
// site table inside memory the test owns and can commit, zero, or plant one site at a time.
//
// It holds no site table and no displacement: every address a caller asks for is a fact the
// SUBJECT and the test both derive from the generated facts headers.

#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "engine/camera_writepoint_facts.h"

namespace replay_test {

class FakeImage {
public:
    explicit FakeImage(SIZE_T reserveBytes)
        : base_(static_cast<uint8_t*>(
              VirtualAlloc(nullptr, reserveBytes, MEM_RESERVE, PAGE_NOACCESS))) {}
    ~FakeImage() {
        if (base_) VirtualFree(base_, 0, MEM_RELEASE);
    }

    FakeImage(const FakeImage&) = delete;
    FakeImage& operator=(const FakeImage&) = delete;

    bool ok() const { return base_ != nullptr; }

    uintptr_t base() const { return reinterpret_cast<uintptr_t>(base_); }

    // What a guarded module computes as `module - kImageBase`.
    uintptr_t slide() const {
        return base() - static_cast<uintptr_t>(replay::camera_facts::kImageBase);
    }

    uintptr_t at(uint64_t va) const { return static_cast<uintptr_t>(va) + slide(); }

    // Commit the pages `n` bytes from `addr` touch, leaving them zeroed. A span can straddle a
    // page boundary, so the range is committed page-first to page-last rather than one page.
    bool commit(uintptr_t addr, size_t n) const {
        const uintptr_t first = addr & ~static_cast<uintptr_t>(0xFFF);
        const uintptr_t last  = (addr + n - 1) & ~static_cast<uintptr_t>(0xFFF);
        return VirtualAlloc(reinterpret_cast<void*>(first), last - first + 0x1000, MEM_COMMIT,
                            PAGE_READWRITE) != nullptr;
    }

    // Commit, then write the bytes facts.tsv recorded -- the only way a site can answer Match.
    bool plant(uintptr_t addr, const void* bytes, size_t n) const {
        if (!commit(addr, n)) return false;
        std::memcpy(reinterpret_cast<void*>(addr), bytes, n);
        return true;
    }

private:
    uint8_t* base_ = nullptr;
};

}  // namespace replay_test
