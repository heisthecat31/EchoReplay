// The child process for the probe's mutation control.
//
// Removing the readability probe makes the guard FAULT, and a fault cannot be observed from
// inside the doctest process without either killing the suite or wrapping the subject in an SEH
// frame that would itself change what is being measured. So the unmapped case is driven here, in
// a child, and the parent reads only the exit code. Two targets are built from this one file:
// wiretap_guardhost with the shipped wiretap.cpp, wiretap_guardhost_noprobe with the probe
// compiled out. The first must report kRefused, the second must report kFaulted -- a control that
// has only ever agreed has not been shown able to disagree.

#include <windows.h>

#include <cstdio>

#include "engine/wiretap.h"
#include "engine/wiretap_facts.h"

namespace {

// Distinct from every code the CRT or a Windows fault would produce on its own, so "the child
// died some other way" cannot be read as either verdict.
constexpr int kRefused = 40;  // the guard answered without touching the unmapped span
constexpr int kFaulted = 41;  // the guard read the unmapped span and took an access violation
constexpr int kNoRepro = 42;  // the reservation failed, so nothing was tested

// Reserved and never committed: every derived entry point lands inside it, and VirtualQuery
// reports MEM_RESERVE. That is the in-game case exactly -- an image too short to hold
// EVENT_BROADCASTER_UNLISTEN_ALL at 0xF8E150 -- reproduced without a second echovr.exe.
constexpr SIZE_T kReserveBytes = 0x1000000;

// No C++ object with a destructor may live in a function that owns an __try frame, so the whole
// exercise is one call and the reporting happens in the caller.
int RunGuard(const void* base, void* netGame) {
    __try {
        replay::IWiretap* tap = replay::WiretapCreateAt(netGame, base);
        if (tap == nullptr) {
            return kNoRepro;
        }
        std::printf("GUARDHOST available=%d diagnostic=%s\n", tap->Available() ? 1 : 0,
                    tap->Diagnostic());
        replay::WiretapDestroy(tap);
        return kRefused;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        std::printf("GUARDHOST access violation at the derived prologue\n");
        return kFaulted;
    }
}

}  // namespace

int main() {
    // The mutant is expected to fault, and a fault that raises WER would hang the parent's wait
    // and drop a dump in the owner's profile. The SEH filter above catches it first; this closes
    // the paths SEH does not own.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    void* const reserved = VirtualAlloc(nullptr, kReserveBytes, MEM_RESERVE, PAGE_NOACCESS);
    if (reserved == nullptr) {
        std::printf("GUARDHOST could not reserve %llu bytes\n",
                    static_cast<unsigned long long>(kReserveBytes));
        return kNoRepro;
    }

    // A real, mapped CR15NetGame stand-in: the guard must fail on the entry points, not on this.
    static unsigned char netGame[replay::wiretap_facts::kBroadcasterOff + 64]{};
    const int code = RunGuard(reserved, netGame);

    VirtualFree(reserved, 0, MEM_RELEASE);
    std::printf("GUARDHOST exit=%d\n", code);
    return code;
}
