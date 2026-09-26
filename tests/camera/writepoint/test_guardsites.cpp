// W1-H. The write point's build guard, driven over a reserved address range.
//
// The defect this exists for: the guard used to memcmp at the derived address with no readability
// probe, so on a short or wrong image -- the case it is there to catch -- it took an access
// violation instead of answering. Six sites, two of them at mid-function offsets.
//
// A reserved-but-uncommitted VirtualAlloc range stands in for a mapped image, so every case runs
// offline with no echovr.exe and no engine code executed. `slide` is the module's own rebasing
// term, so pointing it at the reservation puts the whole site table inside memory this test owns
// and can commit, zero, or plant one site at a time.

#include <doctest.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"
#include "fake_image.h"
#include "generated/facts_generated.h"

using replay::camera::Refusal;
using replay::camera::detail::CheckGuardedSites;
using replay::camera::detail::GuardResult;
namespace cf = replay::camera_facts;

namespace {

// The site table's span, from the lowest fact to the highest plus the page its 20 bytes may run
// into. Reserved, never committed: VirtualQuery answers MEM_RESERVE, which is exactly what it
// answers for an address past the end of a shorter echovr.exe.
constexpr SIZE_T kReserveBytes = 0xE00000;

constexpr size_t kPrologueLen = 20;

// Every site the module guards, in the order it walks them. Held here rather than reached through
// the module so the test can plant bytes at one site without asking the subject where it is --
// but the ADDRESSES are still the module's, because both sides derive them from the same facts
// headers and the assertions below are on the site NAME the module reports.
struct Site {
    const char*    name;
    uint64_t       va;
    const uint8_t* bytes;
};

const Site kSites[] = {
    {"CAM.VIEW_COMMIT", cf::kViewCommitVA, cf::kViewCommitPrologue},
    {"CAM.SETCAMERA_API_IMPL", cf::kSetCameraApiImplVA, cf::kSetCameraApiImplPrologue},
    {"CAM.SETCAMERA_API_STORES", cf::kSetCameraApiImplVA + cf::kSetCameraApiStoresOff,
     cf::kSetCameraApiStores},
    {"CS.LOOKUP_BY_SYMBOL", ::facts::CS_LOOKUP_BY_SYMBOL,
     ::facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE},
    {"VIEWSTATE.INPUT_GATE_FREE_SITE", cf::kIntegrateFreeVA + cf::kIntegrateFreeGateOff,
     cf::kIntegrateFreeGate},
    {"VIEWSTATE.INPUT_GATE_BASE_SITE", cf::kIntegrateBaseVA + cf::kIntegrateBaseGateOff,
     cf::kIntegrateBaseGate},
};
constexpr size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

// The shared fabricated image, indexed by this table's sites so the cases below read the same as
// they did when the reservation was open-coded here.
class FakeImage {
public:
    FakeImage() : img_(kReserveBytes) {}

    bool ok() const { return img_.ok(); }
    uintptr_t slide() const { return img_.slide(); }
    uintptr_t addressOf(size_t i) const { return img_.at(kSites[i].va); }

    bool commit(size_t i, bool plant) {
        const uintptr_t at = addressOf(i);
        return plant ? img_.plant(at, kSites[i].bytes, kPrologueLen)
                     : img_.commit(at, kPrologueLen);
    }

private:
    replay_test::FakeImage img_;
};

bool Has(const char* haystack, const char* needle) {
    return std::strstr(haystack, needle) != nullptr;
}

}  // namespace

TEST_CASE("W1-H guard: an unmapped site is refused, not faulted, and it is its own outcome") {
    FakeImage img;
    REQUIRE(img.ok());

    const GuardResult r = CheckGuardedSites(img.slide());

    // Reaching this line at all is the assertion: the pre-fix guard memcmp'd reserved memory here.
    REQUIRE(r.refusal == Refusal::PrologueUnreadable);
    CHECK(Has(r.why, "prologue-unreadable"));
    CHECK(Has(r.why, "CAM.VIEW_COMMIT"));
    CHECK(Has(r.why, "20 bytes"));

    // The third outcome is not a dressed-up mismatch: a build we could not read must never be
    // reported as a build whose bytes we compared.
    CHECK_FALSE(Has(r.why, "mismatch"));
    CHECK(std::strcmp(replay::camera::RefusalName(Refusal::PrologueUnreadable),
                      "prologue-unreadable") == 0);
    CHECK(std::strcmp(replay::camera::RefusalName(Refusal::PrologueUnreadable),
                      replay::camera::RefusalName(Refusal::PrologueMismatch)) != 0);
}

TEST_CASE("W1-H guard: mapped bytes that differ are a mismatch, and not an unreadable") {
    FakeImage img;
    REQUIRE(img.ok());
    for (size_t i = 0; i < kSiteCount; ++i) {
        REQUIRE(img.commit(i, /*plant=*/false));
    }

    const GuardResult r = CheckGuardedSites(img.slide());

    REQUIRE(r.refusal == Refusal::PrologueMismatch);
    CHECK(Has(r.why, "prologue-mismatch"));
    CHECK(Has(r.why, "CAM.VIEW_COMMIT"));
    CHECK_FALSE(Has(r.why, "unreadable"));
}

TEST_CASE("W1-H guard: a site that matches vouches for itself and for nothing after it") {
    FakeImage img;
    REQUIRE(img.ok());
    // Site 0 mapped AND holding its real bytes; site 1 left reserved. Sites 1 and 2 share a page,
    // so site 0 is the only one whose commit cannot make a later site readable by accident.
    REQUIRE(img.commit(0, /*plant=*/true));

    const GuardResult r = CheckGuardedSites(img.slide());

    REQUIRE(r.refusal == Refusal::PrologueUnreadable);
    CHECK(Has(r.why, "CAM.SETCAMERA_API_IMPL"));
    CHECK_FALSE(Has(r.why, "CAM.VIEW_COMMIT"));
}

TEST_CASE("W1-H guard: the refusal names the site that refused, mid-function offsets included") {
    FakeImage img;
    REQUIRE(img.ok());
    // Everything planted except the free-cam integrator gate, which is checked at
    // kIntegrateFreeVA + 0x4A -- one of the two mid-function offsets the old guard read blind.
    for (size_t i = 0; i < kSiteCount; ++i) {
        REQUIRE(img.commit(i, /*plant=*/i != 4));
    }

    const GuardResult r = CheckGuardedSites(img.slide());

    REQUIRE(r.refusal == Refusal::PrologueMismatch);
    CHECK(Has(r.why, "VIEWSTATE.INPUT_GATE_FREE_SITE"));
    CHECK_FALSE(Has(r.why, "CS.LOOKUP_BY_SYMBOL"));

    char expectedAddress[32];
    std::snprintf(expectedAddress, sizeof expectedAddress, "0x%llX",
                  static_cast<unsigned long long>(img.addressOf(4)));
    CHECK(Has(r.why, expectedAddress));
}

TEST_CASE("W1-H guard: all six mapped and holding the recorded bytes is a clean pass") {
    FakeImage img;
    REQUIRE(img.ok());
    for (size_t i = 0; i < kSiteCount; ++i) {
        REQUIRE(img.commit(i, /*plant=*/true));
    }

    const GuardResult r = CheckGuardedSites(img.slide());

    REQUIRE(r.refusal == Refusal::None);
    CHECK(r.why[0] == '\0');

    // And the pass is not free: flipping one byte of the last site, alone, must lose it. Without
    // this the case above would pass for a guard that compares nothing.
    const uintptr_t last = img.addressOf(kSiteCount - 1);
    *reinterpret_cast<uint8_t*>(last + kPrologueLen - 1) ^= 0xFFu;
    const GuardResult after = CheckGuardedSites(img.slide());
    REQUIRE(after.refusal == Refusal::PrologueMismatch);
    CHECK(Has(after.why, "VIEWSTATE.INPUT_GATE_BASE_SITE"));
}

// ★ THE MUTATION PAIR. Everything above would still pass if `Readable` were replaced by a function
// that always answered true -- the reserved range would simply be memcmp'd and, on this machine,
// most likely still refused as a mismatch. What proves the probe is load-bearing is the pair:
// identical input, identical source file, the one difference being the probe. Refuses (40) versus
// faults (41). Both run as children, because the mutant's whole point is that it dies.
namespace {

std::wstring SiblingExe(const wchar_t* name) {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring path(self, n);
    const size_t cut = path.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return std::wstring();
    return path.substr(0, cut + 1) + name;
}

// -1 means the child could not be started or waited on, which is a failure and never a pass.
int RunChild(const wchar_t* name) {
    const std::wstring exe = SiblingExe(name);
    if (exe.empty()) return -1;
    std::wstring cmd = L"\"" + exe + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return -1;
    }
    int code = -1;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(pi.hProcess, &exitCode)) code = static_cast<int>(exitCode);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

}  // namespace

TEST_CASE("W1-H guard: the probe is what stops the fault -- mutation pair in child processes") {
    const int shipped = RunChild(L"writepoint_guardhost.exe");
    const int mutant  = RunChild(L"writepoint_guardhost_noprobe.exe");

    INFO("shipped=" << shipped << " mutant=" << mutant);
    CHECK(shipped == 40);  // kRefused: answered PrologueUnreadable, never touched the span
    CHECK(mutant == 41);   // kFaulted: memcmp'd the reserved span and took an access violation
    CHECK(shipped != mutant);
}
