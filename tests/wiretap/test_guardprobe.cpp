// The build guard's third answer.
//
// Wiretap::Init derives five entry points from a stated image base and compares 20 bytes at each.
// Until 2026-09-01 it compared them with a bare memcmp, so on any image too short to hold
// EVENT_BROADCASTER_UNLISTEN_ALL at 0xF8E150 -- exactly the wrong build the check exists to
// reject -- it took an access violation instead of refusing. A check that meets
// input it does not understand refuses, and "could not ask" is not "asked and disagreed".
//
// The whole file runs offline against a reserved address range that stands in for a mapped image,
// so the three outcomes are reachable on a host with no echovr.exe and with no engine code ever
// executed. The one case that must FAULT -- the probe compiled out -- runs in a child process;
// see guardhost.cpp.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <doctest.h>

#include "engine/wiretap.h"
#include "engine/wiretap_facts.h"

namespace {

using namespace replay::wiretap_facts;

// Large enough that every derived entry point lands inside it. Reserved, not committed:
// VirtualQuery answers MEM_RESERVE, which is what an address past the end of a shorter image
// looks like, and reading it faults.
constexpr SIZE_T kSpan = 0x1000000;

class FakeImage {
public:
    FakeImage() { base_ = VirtualAlloc(nullptr, kSpan, MEM_RESERVE, PAGE_NOACCESS); }
    ~FakeImage() {
        if (base_ != nullptr) {
            VirtualFree(base_, 0, MEM_RELEASE);
        }
    }
    FakeImage(const FakeImage&)            = delete;
    FakeImage& operator=(const FakeImage&) = delete;

    bool         ok() const { return base_ != nullptr; }
    const void*  base() const { return base_; }
    uint8_t*     At(uint64_t va) const {
        return static_cast<uint8_t*>(base_) + (va - kImageBase);
    }
    uintptr_t Derived(uint64_t va) const { return reinterpret_cast<uintptr_t>(At(va)); }

    // Commits the page holding `va`. Committing per page rather than wholesale is what lets one
    // site be readable while the next is not, which is the case that shows the verdict is taken
    // per site instead of once for the whole image.
    bool CommitPageOf(uint64_t va) const {
        uint8_t* const page = At(va) - (reinterpret_cast<uintptr_t>(At(va)) & 0xFFFu);
        return VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }

    void Plant(uint64_t va, const uint8_t (&bytes)[20]) const {
        std::memcpy(At(va), bytes, sizeof bytes);
    }

private:
    void* base_ = nullptr;
};

// A mapped, zeroed CR15NetGame stand-in. Its broadcaster slot is null, so a run that gets past
// all five prologues stops on the next check and says so -- which is how a MATCH is observed
// without executing the engine's own code.
struct FakeNetGame {
    alignas(16) uint8_t bytes[kBroadcasterOff + 64]{};
};

std::string DiagnoseAt(const void* base, void* netGame) {
    replay::IWiretap* tap = replay::WiretapCreateAt(netGame, base);
    REQUIRE(tap != nullptr);
    const std::string why = tap->Diagnostic();
    CHECK_FALSE(tap->Available());
    replay::WiretapDestroy(tap);
    return why;
}

std::string HexVa(uintptr_t va) {
    char out[32];
    _snprintf_s(out, sizeof out, _TRUNCATE, "0x%llX", static_cast<unsigned long long>(va));
    return out;
}

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::string BinDir() {
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, sizeof self);
    const std::string s(self);
    return s.substr(0, s.find_last_of(0x5C));
}

constexpr DWORD kChildTimeoutMs = 60000;

// 0xFFFFFFFF is not a code either child can return, so "did not exit" is distinguishable from
// both verdicts rather than being scored as one of them.
DWORD RunChild(const char* stem) {
    const char  q   = 0x22;
    std::string cmd = q + BinDir() + "\\" + stem + ".exe" + q;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    STARTUPINFOA        si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return 0xFFFFFFFF;
    }
    DWORD code = 0xFFFFFFFF;
    if (WaitForSingleObject(pi.hProcess, kChildTimeoutMs) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 0xDEAD);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

constexpr DWORD kChildRefused = 40;
constexpr DWORD kChildFaulted = 41;

}  // namespace

TEST_CASE("an unmapped entry point is refused as unreadable, not as a mismatch") {
    FakeImage image;
    REQUIRE(image.ok());
    FakeNetGame netGame;

    const std::string why = DiagnoseAt(image.base(), netGame.bytes);

    CHECK(why.rfind("prologue-unreadable", 0) == 0);
    CHECK(Contains(why, "BroadcasterListen"));
    CHECK(Contains(why, HexVa(image.Derived(kBroadcasterListen))));
    CHECK(Contains(why, "20 bytes"));
    // The two verdicts must not share a word: a harness reading this line has to be able to tell
    // "we could not ask" from "we asked and it is a different build".
    CHECK_FALSE(Contains(why, "mismatch"));
}

TEST_CASE("a mapped entry point holding the wrong bytes is a mismatch") {
    FakeImage image;
    REQUIRE(image.ok());
    REQUIRE(image.CommitPageOf(kBroadcasterListen));
    FakeNetGame netGame;

    const std::string why = DiagnoseAt(image.base(), netGame.bytes);

    CHECK(why.rfind("entry-point prologue mismatch", 0) == 0);
    CHECK(Contains(why, "BroadcasterListen"));
    CHECK(Contains(why, HexVa(image.Derived(kBroadcasterListen))));
    CHECK_FALSE(Contains(why, "unreadable"));
}

TEST_CASE("a readable, matching site does not vouch for the site after it") {
    FakeImage image;
    REQUIRE(image.ok());
    REQUIRE(image.CommitPageOf(kBroadcasterListen));
    REQUIRE(image.CommitPageOf(kBroadcasterUnlisten));
    image.Plant(kBroadcasterListen, kListenPrologue);
    image.Plant(kBroadcasterUnlisten, kUnlistenPrologue);
    // kBroadcasterUnlistenAll's page is left reserved: this is the in-game shape, an image that
    // holds the earlier addresses and ends before the later one.
    FakeNetGame netGame;

    const std::string why = DiagnoseAt(image.base(), netGame.bytes);

    CHECK(why.rfind("prologue-unreadable", 0) == 0);
    CHECK(Contains(why, "BroadcasterUnlistenAll"));
    CHECK(Contains(why, HexVa(image.Derived(kBroadcasterUnlistenAll))));
}

TEST_CASE("five matching prologues carry the guard through to the next check") {
    FakeImage image;
    REQUIRE(image.ok());
    REQUIRE(image.CommitPageOf(kBroadcasterListen));
    REQUIRE(image.CommitPageOf(kBroadcasterUnlisten));
    REQUIRE(image.CommitPageOf(kBroadcasterUnlistenAll));
    REQUIRE(image.CommitPageOf(kMatSymHash));
    REQUIRE(image.CommitPageOf(kMatSymCombine));
    image.Plant(kBroadcasterListen, kListenPrologue);
    image.Plant(kBroadcasterUnlisten, kUnlistenPrologue);
    image.Plant(kBroadcasterUnlistenAll, kUnlistenAllPrologue);
    image.Plant(kMatSymHash, kMatSymHashPrologue);
    image.Plant(kMatSymCombine, kMatSymCombinePrologue);
    FakeNetGame netGame;  // zeroed, so its broadcaster slot is null

    const std::string why = DiagnoseAt(image.base(), netGame.bytes);

    // Reaching this refusal is the assertion: it is the check immediately after the prologue
    // sweep, so all five sites answered MATCH. The tap stays unavailable because this fake image
    // holds no engine code to call, which no offline check can supply.
    CHECK(why == "CR15NetGame carries no broadcaster yet");
}

TEST_CASE("an unmapped CR15NetGame is refused too, once the prologues agree") {
    FakeImage image;
    REQUIRE(image.ok());
    REQUIRE(image.CommitPageOf(kBroadcasterListen));
    REQUIRE(image.CommitPageOf(kBroadcasterUnlisten));
    REQUIRE(image.CommitPageOf(kBroadcasterUnlistenAll));
    REQUIRE(image.CommitPageOf(kMatSymHash));
    REQUIRE(image.CommitPageOf(kMatSymCombine));
    image.Plant(kBroadcasterListen, kListenPrologue);
    image.Plant(kBroadcasterUnlisten, kUnlistenPrologue);
    image.Plant(kBroadcasterUnlistenAll, kUnlistenAllPrologue);
    image.Plant(kMatSymHash, kMatSymHashPrologue);
    image.Plant(kMatSymCombine, kMatSymCombinePrologue);

    // A reserved page inside the same range: the broadcaster and lock slots are addressable and
    // not mapped, which is what a caller handing over something shorter than a CR15NetGame gives.
    FakeImage netGameHome;
    REQUIRE(netGameHome.ok());

    const std::string why = DiagnoseAt(image.base(), netGameHome.At(kImageBase));

    CHECK(why.rfind("netgame-unreadable", 0) == 0);
    CHECK(Contains(why, "broadcaster"));
}

// The control. Without a child process the only way to observe the fault is to let it happen in
// the suite, and an SEH frame around the subject changes what is being measured. Both children
// drive the identical unmapped case; the only difference is whether wiretap.cpp was compiled with
// W1L_MUTATE_NO_READABLE_PROBE.
TEST_CASE("with the probe compiled out the same input faults instead of refusing") {
    CHECK(RunChild("wiretap_guardhost") == kChildRefused);
    CHECK(RunChild("wiretap_guardhost_noprobe") == kChildFaulted);
}
