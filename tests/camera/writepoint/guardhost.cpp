// W1-H's mutation control for the write point's build guard, in a child process.
//
// Removing the readability probe makes the guard FAULT, and a fault cannot be observed from inside
// the doctest process without either killing the suite or wrapping the subject in an SEH frame
// that would itself change what is being measured. So the unmapped case is driven here and the
// parent reads only an exit code. Two targets, one source, one difference: writepoint_guardhost
// with the shipped camera_writepoint.cpp, writepoint_guardhost_noprobe with the probe compiled
// out. The first must report kRefused, the second kFaulted -- a control that has only ever agreed
// has not been shown able to disagree.
//
// The slide comes from a reserved range rather than from this executable's own base, so both
// builds meet the same guaranteed-unmapped addresses. Deriving it from the host module would make
// the outcome depend on where ASLR happened to put the heap.

#include <windows.h>

#include <cstdio>

#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"

namespace {

// Distinct from every code the CRT or a Windows fault would produce on its own, so "the child died
// some other way" cannot be read as either verdict.
constexpr int kRefused = 40;  // the guard answered without touching the unmapped span
constexpr int kFaulted = 41;  // the guard read the unmapped span and took an access violation
constexpr int kNoRepro = 42;  // the reservation failed, or the guard answered something else

constexpr SIZE_T kReserveBytes = 0xE00000;

// No C++ object with a destructor may live in a function that owns an __try frame, so the whole
// exercise is one call and the reporting happens in the caller.
int RunGuard(uintptr_t slide) {
    __try {
        const replay::camera::detail::GuardResult r =
            replay::camera::detail::CheckGuardedSites(slide);
        std::printf("GUARDHOST refusal=%s why=%s\n",
                    replay::camera::RefusalName(r.refusal), r.why);
        return r.refusal == replay::camera::Refusal::PrologueUnreadable ? kRefused : kNoRepro;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        std::printf("GUARDHOST access violation at the derived prologue\n");
        return kFaulted;
    }
}

}  // namespace

int main() {
    // The mutant is expected to fault, and a fault that raises WER would hang the parent's wait and
    // drop a dump in the owner's profile. The SEH filter above catches it first; this closes the
    // paths SEH does not own.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    void* const reserved = VirtualAlloc(nullptr, kReserveBytes, MEM_RESERVE, PAGE_NOACCESS);
    if (reserved == nullptr) {
        std::printf("GUARDHOST could not reserve %llu bytes\n",
                    static_cast<unsigned long long>(kReserveBytes));
        return kNoRepro;
    }

    const uintptr_t slide = reinterpret_cast<uintptr_t>(reserved) -
                            static_cast<uintptr_t>(replay::camera_facts::kImageBase);
    const int code = RunGuard(slide);

    VirtualFree(reserved, 0, MEM_RELEASE);
    std::printf("GUARDHOST exit=%d\n", code);
    return code;
}
