// The registration lock's deadline.
//
// Wiretap::Listen takes the CR15NetGame listener lock with the engine's own sequence, and until
// 2026-09-02 it took it with the engine's own unbounded spin. capture::Tick registers every stream
// from the game thread, so a lock held by a thread we do not own hung the game thread with no log
// and no refusal -- the same shape as the L5 stall. Every wait has a deadline and a
// named refusal.
//
// Both arms run in a child process (lockhost.cpp), because the mutant that proves the deadline is
// load-bearing does not return, and a wait that does not return cannot be observed from inside the
// doctest process. This file only starts the children and reads what they did.

#include <windows.h>

#include <string>
#include <vector>

#include <doctest.h>

#include "../local_inputs.h"

namespace {

// Every verdict lockhost.cpp can report. Kept as one list here so a child that dies some other way
// is not silently scored as one of them.
constexpr DWORD kOk            = 50;
constexpr DWORD kNoEngineImage = 51;
constexpr DWORD kDidNotExit    = 0xFFFFFFFF;

// The shipped host maps echovr.exe, builds a broadcaster out of engine code and runs three cases.
constexpr DWORD kShippedTimeoutMs = 60000;

// What the mutant gets before it is called hung. The deadline it removed is 2 ms, so this is more
// than three orders of magnitude past the bound the shipped host is held to -- and the shipped host
// having already finished inside kShippedTimeoutMs is what rules out "the machine was just slow".
constexpr DWORD kMutantHangMs = 10000;

std::string BinDir() {
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, sizeof self);
    const std::string s(self);
    return s.substr(0, s.find_last_of(0x5C));
}

DWORD RunChild(const char* stem, DWORD waitMs) {
    const char        q   = 0x22;
    std::string       cmd = q + BinDir() + "\\" + stem + ".exe" + q;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    STARTUPINFOA        si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return kDidNotExit;
    }
    DWORD code = kDidNotExit;
    if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 0xDEAD);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// Run once and answered twice: the host maps a 100 MB image and both cases need the same answer.
DWORD ShippedResult() {
    static const DWORD code = RunChild("wiretap_lockhost", kShippedTimeoutMs);
    return code;
}

// Named in the refusal so the failure says which path was tried, not just that one was. Both this
// and lockhost.cpp resolve it through tests/echovr_path.h, so they cannot disagree.
std::string EnginePath() {
    const std::string exe = replay_test::FindEchoVrExeA();
    return exe.empty() ? "<echovr.exe not found>" : exe;
}

}  // namespace

TEST_CASE("a held registration lock is refused inside the deadline, with the engine untouched") {
    // A missing engine image is not a pass: this case cannot ask its question, so it refuses. The
    // WARN this replaced let the whole file report SUCCESS with zero assertions against a bad path.
    const DWORD code = ShippedResult();
    if (code == kNoEngineImage && replay_test::FindEchoVrExeW().empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    REQUIRE_MESSAGE(code != kNoEngineImage,
                    "wiretap_lockhost could not map echovr.exe from ", EnginePath(),
                    " -- the lock deadline was NOT exercised; set REPLAY_ECHOVR_EXE to its path");
    // The child's own printout names which of its checks failed; the code is the verdict.
    CHECK_MESSAGE(code == kOk, "wiretap_lockhost exit code ", code);
}

TEST_CASE("with the deadline compiled out the same wait never returns") {
    // Only meaningful once the shipped host has been shown to complete: a mutant that exits early
    // because there is no echovr.exe would otherwise read as "it did not hang".
    const DWORD shipped = ShippedResult();
    if (shipped == kNoEngineImage && replay_test::FindEchoVrExeW().empty()) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    REQUIRE_MESSAGE(shipped == kOk,
                    "the shipped lock host exited ", shipped, " with REPLAY_ECHOVR_EXE=",
                    EnginePath(), " -- the mutation control was NOT exercised");
    const DWORD mutant = RunChild("wiretap_lockhost_unbounded", kMutantHangMs);
    CHECK_MESSAGE(mutant == kDidNotExit, "wiretap_lockhost_unbounded exited with ", mutant,
                  " -- the deadline is not what stops the spin");
}
