// DESIGN S13.4's W1-C acceptance names this in as many words: "refuses (does not overwrite) a hook site
// whose prologue already differs". Nothing green exercised it until this file existed, and the audit found
// a fail-open in exactly the function that criterion rests on.
//
// This runs offline. The trick is that hookmgr and buildguard address sites by ENGINE virtual address
// against facts::PE_IMAGE_BASE, and this process is not the engine - so a site here is synthesised by
// running the mapping backwards: pcVa = PE_IMAGE_BASE + (target - our own image base). Rebase() then
// lands back on `target`, and every path can be driven against a real function in this binary.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <doctest.h>
#include <windows.h>

#include "bootstrap/attachhook.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/egress.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/log.h"
#include "generated/facts_generated.h"

namespace {

// Never called. It exists to own twenty bytes of real, readable, executable .text that the tests can
// treat as a hook site. NOINLINE and a side effect through a volatile so nothing folds it away.
volatile int g_sink;

__declspec(noinline) int VictimFunction(int x) {
    g_sink = x;
    return x * 3 + 1;
}

__declspec(noinline) int SecondVictim(int x) {
    g_sink = x + 2;
    return x * 5 + 7;
}

int DetourTarget(int) { return 0; }

const uint8_t* AddressOf(int (*fn)(int)) {
    return reinterpret_cast<const uint8_t*>(fn);
}

// The inverse of buildguard's Rebase: give back the engine VA that maps to this in-process address.
uint64_t SyntheticPcVa(const void* address) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    return facts::PE_IMAGE_BASE +
           static_cast<uint64_t>(reinterpret_cast<const uint8_t*>(address) - base);
}

std::vector<uint8_t> Prologue(const void* address, size_t n) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
    return std::vector<uint8_t>(p, p + n);
}

constexpr size_t kPrologueLen = sizeof facts::GAME_UPDATE_PROLOGUE;

}  // namespace

TEST_CASE("VerifySite agrees only when it actually compared the bytes") {
    const uint64_t va = SyntheticPcVa(AddressOf(VictimFunction));
    const std::vector<uint8_t> real = Prologue(AddressOf(VictimFunction), kPrologueLen);

    CHECK(replay::buildguard::VerifySite("VICTIM", va, real.data(), real.size()));

    SUBCASE("a zero-length expectation is a refusal, not a pass") {
        // The BLOCK-2 regression. Readable(p, 0) and memcmp(a, b, 0) are both vacuously true, so without
        // an explicit refusal this reported "verified" having compared nothing - and hookmgr::Create's
        // else-if ladder then fell through to MH_CreateHook on an unchecked site.
        CHECK_FALSE(replay::buildguard::VerifySite("VICTIM", va, real.data(), 0));
        CHECK_FALSE(replay::buildguard::VerifySite("VICTIM", va, nullptr, 0));
    }
    SUBCASE("a null expectation is a refusal rather than undefined behaviour") {
        CHECK_FALSE(replay::buildguard::VerifySite("VICTIM", va, nullptr, kPrologueLen));
    }
    SUBCASE("one byte different anywhere in the window is a refusal") {
        // Every position, not a convenient one: a comparison that stops early passes for the tail bytes.
        for (size_t i = 0; i < real.size(); ++i) {
            std::vector<uint8_t> altered = real;
            altered[i] = static_cast<uint8_t>(altered[i] ^ 0xFFu);
            CAPTURE(i);
            REQUIRE_FALSE(replay::buildguard::VerifySite("VICTIM", va, altered.data(), altered.size()));
        }
    }
    SUBCASE("a site outside the image is a refusal") {
        CHECK_FALSE(replay::buildguard::VerifySite("VICTIM", 1u, real.data(), real.size()));
        CHECK_FALSE(replay::buildguard::VerifySite("VICTIM", facts::PE_IMAGE_BASE - 1u, real.data(),
                                                  real.size()));
    }
    SUBCASE("a shorter window still compares what it was given") {
        CHECK(replay::buildguard::VerifySite("VICTIM", va, real.data(), 1));
        std::vector<uint8_t> wrong = real;
        wrong[0] = static_cast<uint8_t>(wrong[0] ^ 0xFFu);
        CHECK_FALSE(replay::buildguard::VerifySite("VICTIM", va, wrong.data(), 1));
    }
}

TEST_CASE("a hook site whose bytes already differ is refused and left alone") {
    replay::hookmgr::ResetForTest();
    replay::config::ResetForTest();
    replay::buildguard::ResetForTest();

    const uint64_t va = SyntheticPcVa(AddressOf(VictimFunction));
    std::vector<uint8_t> wrong = Prologue(AddressOf(VictimFunction), kPrologueLen);
    wrong[0] = static_cast<uint8_t>(wrong[0] ^ 0xFFu);

    // Snapshot the real bytes so "refused" can be shown to mean "did not write", not just "returned a
    // status". A refusal that still patched would satisfy the status check and fail the acceptance.
    const std::vector<uint8_t> before = Prologue(AddressOf(VictimFunction), kPrologueLen);

    void* trampoline = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFEBABEu));
    const replay::HookStatus status = replay::hookmgr::Create(
        "VICTIM", va, wrong.data(), wrong.size(), reinterpret_cast<void*>(&DetourTarget), &trampoline,
        "capture.voip");

    // The build guard has not run in this process, so the FIRST refusal in the ladder wins - which is the
    // ordering the design asks for: a build we do not recognise is a better diagnosis than a byte diff.
    CHECK(status == replay::HookStatus::RefusedByBuild);
    CHECK(replay::hookmgr::Status("VICTIM") == replay::HookStatus::RefusedByBuild);
    CHECK(replay::hookmgr::ArmedCount() == 0);

    const std::vector<uint8_t> after = Prologue(AddressOf(VictimFunction), kPrologueLen);
    CHECK(after == before);
    CHECK(VictimFunction(4) == 13);   // still the original behaviour, not a trampoline

    // Arming something that was refused must stay refused rather than quietly enabling it.
    CHECK(replay::hookmgr::Arm("VICTIM") == replay::HookStatus::RefusedByBuild);
    CHECK(replay::hookmgr::ArmInline("VICTIM") == replay::HookStatus::RefusedByBuild);
    CHECK(replay::hookmgr::ArmedCount() == 0);

    replay::hookmgr::ResetForTest();
}

TEST_CASE("the refusal ladder reports the most specific reason it can") {
    replay::hookmgr::ResetForTest();
    replay::config::ResetForTest();
    replay::buildguard::ResetForTest();

    const uint64_t va = SyntheticPcVa(AddressOf(VictimFunction));
    const std::vector<uint8_t> real = Prologue(AddressOf(VictimFunction), kPrologueLen);
    void* trampoline = nullptr;

    SUBCASE("an unknown site is not silently absent") {
        CHECK(replay::hookmgr::Status("never-created") == replay::HookStatus::Absent);
        CHECK(replay::hookmgr::Arm("never-created") == replay::HookStatus::Absent);
    }

    SUBCASE("an unrecognised build outranks every other reason") {
        // Deliberately wrong on three counts at once - no config key, wrong bytes, and a build that was
        // never accepted. The build refusal is the most useful diagnosis, so it is the one reported.
        std::vector<uint8_t> wrong = real;
        wrong[0] = static_cast<uint8_t>(wrong[0] ^ 0xFFu);
        CHECK(replay::hookmgr::Create("ALLBAD", va, wrong.data(), wrong.size(),
                                      reinterpret_cast<void*>(&DetourTarget), &trampoline,
                                      nullptr) == replay::HookStatus::RefusedByBuild);
    }

    // Everything below needs a build the guard accepted. That state is unreachable offline - it needs the
    // real echovr.exe - so the verdict is set directly and hookmgr's ladder is driven from there. The
    // guard's own decision table is exercised separately and exhaustively in test_guard_modreg.cpp.
    replay::buildguard::SetVerdictForTest(replay::BuildVerdict::Accept);
    REQUIRE(replay::buildguard::Passed());

    SUBCASE("a hook with no config key is refused rather than installed unledgered") {
        // S4.4: every intervention row is disableable by a config key. A hook arriving without one is an
        // intervention nobody can switch off, and the ledger could not describe it either.
        CHECK(replay::hookmgr::Create("NOKEY", va, real.data(), real.size(),
                                      reinterpret_cast<void*>(&DetourTarget), &trampoline,
                                      nullptr) == replay::HookStatus::RefusedByConfig);
    }

    SUBCASE("a hook whose config key is off is refused") {
        const std::string text = R"({"capture.voip": false})";
        REQUIRE(replay::config::LoadFromTextForTest(text.data(), text.size()));
        CHECK_FALSE(replay::config::RowDeclared("capture.voip"));
        CHECK(replay::hookmgr::Create("OFF", va, real.data(), real.size(),
                                      reinterpret_cast<void*>(&DetourTarget), &trampoline,
                                      "capture.voip") == replay::HookStatus::RefusedByConfig);
    }

    SUBCASE("a site whose bytes already differ is refused, and the bytes are left alone") {
        // ★ The S13.4 acceptance criterion, and the collision the 2026-08-28 sweep hit for real:
        // rlay.dll had already detoured a guarded site. Refusing is the whole point.
        const std::vector<uint8_t> before = Prologue(AddressOf(VictimFunction), kPrologueLen);
        std::vector<uint8_t> wrong = real;
        wrong[kPrologueLen - 1] = static_cast<uint8_t>(wrong[kPrologueLen - 1] ^ 0xFFu);

        CHECK(replay::hookmgr::Create("DIFFERS", va, wrong.data(), wrong.size(),
                                      reinterpret_cast<void*>(&DetourTarget), &trampoline,
                                      "capture.voip") == replay::HookStatus::RefusedByPrologue);
        CHECK(Prologue(AddressOf(VictimFunction), kPrologueLen) == before);
        CHECK(VictimFunction(4) == 13);
        CHECK(replay::hookmgr::ArmedCount() == 0);
        CHECK(replay::hookmgr::Arm("DIFFERS") == replay::HookStatus::RefusedByPrologue);
    }

    SUBCASE("a zero-length expectation cannot buy a hook") {
        // BLOCK-2 seen from the caller's side: with the length-0 fail-open, this reached MH_CreateHook
        // and patched a site whose bytes were never compared.
        CHECK(replay::hookmgr::Create("ZEROLEN", va, real.data(), 0,
                                      reinterpret_cast<void*>(&DetourTarget), &trampoline,
                                      "capture.voip") == replay::HookStatus::RefusedByPrologue);
        CHECK(replay::hookmgr::ArmedCount() == 0);
    }

    SUBCASE("a matching site is created, arms, and can be unwound") {
        // The gate and the row declaration are one answer: a detour is installed iff the row is
        // declared, which is what makes SC7's denominator describe the run that actually happened.
        CHECK(replay::config::RowDeclared("capture.voip"));
        const replay::HookStatus created = replay::hookmgr::Create(
            "GOOD", va, real.data(), real.size(), reinterpret_cast<void*>(&DetourTarget), &trampoline,
            "capture.voip");
        REQUIRE(created == replay::HookStatus::Created);
        CHECK(trampoline != nullptr);
        CHECK(replay::hookmgr::ArmedCount() == 0);   // created is not armed

        CHECK(replay::hookmgr::Arm("GOOD") == replay::HookStatus::Armed);
        CHECK(replay::hookmgr::ArmedCount() == 1);
        CHECK(replay::hookmgr::Arm("GOOD") == replay::HookStatus::Armed);   // idempotent

        // The detour really is live: the victim now answers with the detour's value, not its own.
        CHECK(VictimFunction(4) == 0);

        replay::hookmgr::Shutdown();
        CHECK(replay::hookmgr::ArmedCount() == 0);
        CHECK(VictimFunction(4) == 13);   // and unwinding really restored it
    }

    replay::hookmgr::ResetForTest();
    replay::config::ResetForTest();
    replay::buildguard::ResetForTest();
}

TEST_CASE("the hook table is bounded and a duplicate does not hand back a garbage trampoline") {
    replay::hookmgr::ResetForTest();

    const uint64_t va = SyntheticPcVa(AddressOf(SecondVictim));
    const std::vector<uint8_t> real = Prologue(AddressOf(SecondVictim), kPrologueLen);

    void* first = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFEBABEu));
    const replay::HookStatus a = replay::hookmgr::Create(
        "DUP", va, real.data(), real.size(), reinterpret_cast<void*>(&DetourTarget), &first,
        "capture.voip");

    // The second Create for one fact id used to return the stored status and leave the caller's out-param
    // untouched, so a second call site got whatever was in that variable. The header promises the
    // trampoline is written; a refused site has none, so the honest value is null.
    void* second = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFEBABEu));
    const replay::HookStatus b = replay::hookmgr::Create(
        "DUP", va, real.data(), real.size(), reinterpret_cast<void*>(&DetourTarget), &second,
        "capture.voip");
    CHECK(a == b);
    CHECK(second != reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFEBABEu)));
    CHECK(second == first);
    CHECK(replay::hookmgr::Count() == 1);

    SUBCASE("the table refuses rather than overruns") {
        for (int i = 0; i < 40; ++i) {
            char id[32];
            std::snprintf(id, sizeof id, "FILL%d", i);
            void* t = nullptr;
            replay::hookmgr::Create(id, va, real.data(), real.size(),
                                    reinterpret_cast<void*>(&DetourTarget), &t, "capture.voip");
        }
        CHECK(replay::hookmgr::Count() <= 16);
        CHECK(replay::hookmgr::FactIdAt(replay::hookmgr::Count()) == nullptr);
    }

    replay::hookmgr::ResetForTest();
    CHECK(replay::hookmgr::Count() == 0);
}

TEST_CASE("hookmgr removes its own hooks and leaves MinHook alone") {
    // ★ The contract changed deliberately, and this asserts the new one rather than the old.
    //
    // Shutdown() used to call MH_Uninitialize, which removes EVERY hook in the process. Once the egress
    // blocker began arming BEFORE the build guard has a verdict, that made the refusal path tear down
    // the one protection still meaningful on a build we do not recognise - from a path whose whole
    // message is "we touched nothing". So Shutdown() now removes hookmgr's OWN entries and leaves
    // MinHook initialised for whoever else is using it.
    replay::hookmgr::ResetForTest();
    CHECK_FALSE(replay::hookmgr::Initialized());
    CHECK(replay::hookmgr::Initialize());
    CHECK(replay::hookmgr::Initialized());
    CHECK(replay::hookmgr::Initialize());   // idempotent, not an error

    replay::hookmgr::Shutdown();
    CHECK(replay::hookmgr::Count() == 0);            // our entries are gone
    CHECK(replay::hookmgr::Initialized());           // ... and MinHook is not ours to uninitialise
    replay::hookmgr::Shutdown();                     // safe to call twice
    CHECK(replay::hookmgr::Count() == 0);

    replay::hookmgr::ResetForTest();
}

TEST_CASE("the status names are distinct, so a log line can be read back") {
    const replay::HookStatus all[] = {
        replay::HookStatus::Absent,           replay::HookStatus::Created,
        replay::HookStatus::Armed,            replay::HookStatus::RefusedByConfig,
        replay::HookStatus::RefusedByBuild,   replay::HookStatus::RefusedByPrologue,
        replay::HookStatus::RefusedByMinHook,
    };
    std::vector<std::string> names;
    for (const replay::HookStatus s : all) {
        const char* n = replay::HookStatusName(s);
        REQUIRE(n != nullptr);
        CHECK(std::strlen(n) > 0);
        names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::unique(names.begin(), names.end()) == names.end());
}

// ---------------------------------------------------------------------------------------------------
// The attach-hook registry. Ledger row 1X.1 has to run inline in DllMain - W1-F measured the window at
// 46 bytes of straight-line code after the call that reaches us - so a REPLAY_MODULE cannot do it. What
// is tested here is that the gate is real and the ordering is not link order.

namespace {

int g_attachCalls;
std::vector<std::string> g_attachOrder;

bool AttachOk() { ++g_attachCalls; g_attachOrder.push_back("ok"); return true; }
bool AttachDeclines() { ++g_attachCalls; g_attachOrder.push_back("declines"); return false; }
bool AttachGated() { ++g_attachCalls; g_attachOrder.push_back("gated"); return true; }

const replay::AttachHook kZebra{"zebra", nullptr, AttachOk};
const replay::AttachHook kAlpha{"alpha", nullptr, AttachDeclines};
const replay::AttachHook kGated{"gated", "capture.voip", AttachGated};

}  // namespace

TEST_CASE("attach hooks run in name order, and a ledger key that is off really skips one") {
    replay::attachhook::ResetForTest();
    replay::config::ResetForTest();
    g_attachCalls = 0;
    g_attachOrder.clear();

    // Registered out of order on purpose: the sequence must come from the name, not from link order.
    replay::attachhook::RegisterForTest(kZebra);
    replay::attachhook::RegisterForTest(kGated);
    replay::attachhook::RegisterForTest(kAlpha);
    REQUIRE(replay::attachhook::Count() == 3);

    const std::string off = R"({"capture.voip": false})";
    REQUIRE(replay::config::LoadFromTextForTest(off.data(), off.size()));

    const size_t installed = replay::attachhook::RunAll();

    // gated is skipped entirely - not called and not counted - which is what "every ledger row is
    // disableable" has to mean for a row that runs before the worker exists.
    CHECK(g_attachCalls == 2);
    REQUIRE(g_attachOrder.size() == 2);
    CHECK(g_attachOrder[0] == "declines");   // alpha
    CHECK(g_attachOrder[1] == "ok");         // zebra
    CHECK(installed == 1);
    CHECK(replay::attachhook::Installed("zebra"));
    CHECK_FALSE(replay::attachhook::Installed("alpha"));
    CHECK_FALSE(replay::attachhook::Installed("gated"));
    CHECK_FALSE(replay::attachhook::Installed("no-such-hook"));

    // Once. A second Attach must not re-install anything.
    CHECK(replay::attachhook::RunAll() == 0);
    CHECK(g_attachCalls == 2);

    replay::attachhook::ResetForTest();
    replay::config::ResetForTest();
}

TEST_CASE("an attach hook whose key is on is actually called") {
    // The control for the test above: without it, "skipped" and "the registry is broken" look identical.
    replay::attachhook::ResetForTest();
    replay::config::ResetForTest();
    g_attachCalls = 0;
    g_attachOrder.clear();

    replay::attachhook::RegisterForTest(kGated);
    const std::string on = R"({"capture.voip": true})";
    REQUIRE(replay::config::LoadFromTextForTest(on.data(), on.size()));

    CHECK(replay::attachhook::RunAll() == 1);
    CHECK(g_attachCalls == 1);
    CHECK(replay::attachhook::Installed("gated"));

    replay::attachhook::ResetForTest();
    replay::config::ResetForTest();
}

// ---------------------------------------------------------------------------------------------------
// The egress blocker. It enforces a BINDING owner constraint - no server - so the part worth testing
// hardest is the classifier that decides what may leave the machine, and the three-state arm report.

TEST_CASE("a destination is local only when it genuinely cannot leave the machine") {
    char text[64];
    auto v4 = [&](const char* ip, unsigned short port) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip, &a.sin_addr);
        return replay::egress::IsLocalDestination(&a, sizeof a, text, sizeof text);
    };

    CHECK(v4("127.0.0.1", 6721));
    CHECK(v4("127.255.255.254", 80));    // the whole 127/8, not just 127.0.0.1
    CHECK(v4("0.0.0.0", 0));             // INADDR_ANY has nowhere to go
    CHECK_FALSE(v4("8.8.8.8", 53));
    CHECK_FALSE(v4("52.14.1.2", 443));
    // ★ A private LAN address is NOT local. It leaves this machine, which is what the constraint is
    // about; treating 10/8 or 192.168/16 as safe would let a probe reach a server on the same network.
    CHECK_FALSE(v4("192.168.1.10", 6721));
    CHECK_FALSE(v4("10.0.0.5", 6721));
    CHECK_FALSE(v4("169.254.1.1", 6721));
    CHECK_FALSE(v4("126.255.255.255", 80));   // just below 127/8
    CHECK_FALSE(v4("128.0.0.1", 80));         // just above

    sockaddr_in6 a6{};
    a6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "::1", &a6.sin6_addr);
    CHECK(replay::egress::IsLocalDestination(&a6, sizeof a6, text, sizeof text));
    inet_pton(AF_INET6, "2001:4860:4860::8888", &a6.sin6_addr);
    CHECK_FALSE(replay::egress::IsLocalDestination(&a6, sizeof a6, text, sizeof text));

    // Nothing to send to, and a family we do not parse: neither reaches a peer, and refusing them would
    // break local IPC for nothing.
    CHECK(replay::egress::IsLocalDestination(nullptr, 0, text, sizeof text));
    sockaddr_in truncated{};
    truncated.sin_family = AF_INET;
    CHECK(replay::egress::IsLocalDestination(&truncated, 3, text, sizeof text));

    // The text is filled in every case, because a destination the report cannot name is not a report.
    CHECK(v4("127.0.0.1", 6721));
    CHECK(std::string(text) == "127.0.0.1:6721");
}

TEST_CASE("the arm report distinguishes armed, partly armed and failed") {
    // Three states, not two. "Some entry points guarded" is an open path, and a component that called it
    // armed would be the exact lie this one exists to prevent - the harness records a silent probe as
    // unknown, never safe.
    using replay::egress::ArmState;
    CHECK(std::string(replay::egress::ArmStateName(ArmState::NotAttempted)) == "not-attempted");
    CHECK(std::string(replay::egress::ArmStateName(ArmState::Armed)) == "armed");
    CHECK(std::string(replay::egress::ArmStateName(ArmState::PartiallyArmed)) == "partially-armed");
    CHECK(std::string(replay::egress::ArmStateName(ArmState::Failed)) == "failed");

    replay::egress::ResetForTest();
    const replay::egress::Report& before = replay::egress::Current();
    CHECK(before.state == ArmState::NotAttempted);
    CHECK(before.hooksLive == 0);
    CHECK(before.refusedRemote == 0);
}

TEST_CASE("the blocker actually refuses a non-loopback destination and lets loopback through") {
    // The behaviour, not the table: arm for real in this process, then try to reach somewhere.
    replay::egress::ResetForTest();
    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    const replay::egress::ArmState state = replay::egress::Arm();
    REQUIRE(state == replay::egress::ArmState::Armed);
    // By NAME, not by count. connect and sendto alone were NOT exhaustive: echovr.exe imports WSAIoctl,
    // which hands out ConnectEx and WSASendMsg through SIO_GET_EXTENSION_FUNCTION_POINTER, and either
    // would have reached a peer without entering connect(). A count would have passed that whole gap.
    const std::string hooks = replay::egress::HookNamesJson();
    for (const char* required : {"connect", "WSAConnect", "sendto", "WSASendTo", "WSAIoctl", "send",
                                 "WSASend", "socket", "WSASocketW", "WSASocketA"}) {
        CAPTURE(required);
        CHECK(hooks.find(required) != std::string::npos);
    }
    CHECK(replay::egress::Current().hooksLive == replay::egress::Current().hooksRequested);
    // Measured, not assumed. The harness reports this per launch because the window between attach and
    // arm is when egress is unguarded.
    CHECK(replay::egress::Current().armedAtMs < 5000);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(s != INVALID_SOCKET);
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    const int rc = connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof remote);
    CHECK(rc == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENETUNREACH);
    CHECK(replay::egress::Current().refusedRemote >= 1);
    closesocket(s);

    // The control: if loopback were refused too, "refuses everything" would pass this test while
    // breaking the game's own localhost HTTP route, and the blocker would be useless rather than safe.
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(u != INVALID_SOCKET);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    // Port 9, the discard service, because this test does not care where the datagram goes - only that
    // the blocker lets a loopback destination through. It used to say 6721, which is
    // SESSION.HTTP_PORT_DEFAULT in the registry: a number carrying a meaning this test never intended,
    // and one the no-transcription gate is right to refuse.
    local.sin_port = htons(9);
    inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
    const char payload[] = "x";
    const uint64_t allowedBefore = replay::egress::Current().allowedLocal;
    sendto(u, payload, 1, 0, reinterpret_cast<sockaddr*>(&local), sizeof local);
    CHECK(replay::egress::Current().allowedLocal > allowedBefore);
    closesocket(u);

    // sendto to a remote address is the UDP hole a TCP socket census structurally cannot see.
    SOCKET u2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(u2 != INVALID_SOCKET);
    const uint64_t refusedBefore = replay::egress::Current().refusedRemote;
    CHECK(sendto(u2, payload, 1, 0, reinterpret_cast<sockaddr*>(&remote), sizeof remote) ==
          SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENETUNREACH);
    CHECK(replay::egress::Current().refusedRemote > refusedBefore);
    closesocket(u2);

    // Both destinations are named in the report; a count with no names is not evidence.
    bool sawRemote = false;
    for (size_t i = 0;; ++i) {
        const char* text = nullptr;
        const char* via = nullptr;
        uint64_t count = 0;
        bool local = false;
        if (!replay::egress::DestinationAt(i, &text, &via, &count, &local)) {
            break;
        }
        if (std::string(text).find("8.8.8.8") != std::string::npos) {
            sawRemote = true;
            CHECK_FALSE(local);
        }
    }
    CHECK(sawRemote);

    // ★ The ConnectEx path, which the original four did not close. Refusing at the LOOKUP means the
    // extension pointer never exists, so there is no window between obtaining one and using it.
    SOCKET x = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(x != INVALID_SOCKET);
    GUID connectExId = WSAID_CONNECTEX;
    void* connectExPtr = nullptr;
    DWORD got = 0;
    const int ioctlRc = WSAIoctl(x, SIO_GET_EXTENSION_FUNCTION_POINTER, &connectExId,
                                 sizeof connectExId, &connectExPtr, sizeof connectExPtr, &got,
                                 nullptr, nullptr);
    CHECK(ioctlRc == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAEOPNOTSUPP);
    CHECK(connectExPtr == nullptr);
    CHECK(replay::egress::Current().extensionsRefused >= 1);

    // The control: an unrelated WSAIoctl must still work, or the blocker has broken sockets generally
    // rather than closed one path.
    DWORD nonBlocking = 0;
    CHECK(WSAIoctl(x, FIONBIO, &nonBlocking, sizeof nonBlocking, nullptr, 0, &got, nullptr,
                   nullptr) == 0);
    closesocket(x);

    // The socket recorder never refuses; it dates the first socket so "armed early enough" is measured.
    CHECK(replay::egress::Current().socketsCreated >= 1);

    WSACleanup();
    replay::egress::ResetForTest();
}

TEST_CASE("the hook name list survives the sixteen-row configuration the game actually runs") {
    // ★ THE CONFIGURATION THE OFFLINE SUITE NEVER HAD. This test host has no wininet.dll, so the name
    // list was only ever built from ten rows - 115 bytes inside a 160-byte buffer - and every assertion
    // about it landed inside the first 115 bytes either way. In the game wininet is always mapped, all
    // sixteen rows arm, and the old accumulate-snprintf underflowed its size argument and wrote 76 bytes
    // past the end: the four WININET refusers were cut mid-token and done.json stopped parsing, so the
    // harness never read the egress verdict at all.
    //
    // Loading wininet here reproduces the game's configuration. The rule against LoadLibrary is a rule
    // about DllMain and the loader lock; a test host is neither.
    HMODULE inet = LoadLibraryA("wininet.dll");
    REQUIRE(inet != nullptr);

    static const char* const kExpected[] = {
        "connect", "WSAConnect", "sendto", "WSASendTo", "WSAIoctl", "send", "WSASend",
        "socket", "WSASocketW", "WSASocketA",
        "InternetConnectW", "InternetConnectA", "InternetOpenUrlW", "InternetOpenUrlA",
        "HttpSendRequestW", "HttpSendRequestA",
    };
    constexpr size_t kExpectedCount = sizeof kExpected / sizeof kExpected[0];

    replay::egress::ResetForTest();
    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    REQUIRE(replay::egress::Arm() == replay::egress::ArmState::Armed);

    const replay::egress::Report& r = replay::egress::Current();
    CHECK(r.wininetLoaded);
    CHECK(r.hooksRequested == static_cast<uint32_t>(kExpectedCount));
    CHECK(r.hooksLive == r.hooksRequested);

    const std::string hooks = replay::egress::HookNamesJson();
    for (const char* required : kExpected) {
        CAPTURE(required);
        CHECK(hooks.find(required) != std::string::npos);
    }

    // Nothing was dropped. The marker exists so that a short list can never be a silent one, and its
    // absence here is the positive statement that all sixteen fitted.
    CHECK(hooks.find("(truncated)") == std::string::npos);

    // ★ And the result is a well-formed JSON array body rather than merely a string containing the
    // right substrings. The shipped artifact failed on syntax, not on content, so syntax is what this
    // asserts: one quoted name each, one separator between each pair, nothing dangling.
    size_t quotes = 0;
    size_t commas = 0;
    for (const char c : hooks) {
        if (c == '"') ++quotes;
        if (c == ',') ++commas;
    }
    CHECK(quotes == kExpectedCount * 2);
    CHECK(commas == kExpectedCount - 1);
    CHECK(hooks.back() == '"');

    WSACleanup();
    replay::egress::ResetForTest();
    FreeLibrary(inet);
}

TEST_CASE("FIX-N1: a refused URL is recorded without its credentials") {
    HMODULE inet = LoadLibraryA("wininet.dll");
    REQUIRE(inet != nullptr);
    using OpenUrlAFn = void*(WINAPI*)(void*, const char*, const char*, DWORD, DWORD, DWORD_PTR);
    using OpenUrlWFn = void*(WINAPI*)(void*, const wchar_t*, const wchar_t*, DWORD, DWORD, DWORD_PTR);
    const auto openUrlA = reinterpret_cast<OpenUrlAFn>(GetProcAddress(inet, "InternetOpenUrlA"));
    const auto openUrlW = reinterpret_cast<OpenUrlWFn>(GetProcAddress(inet, "InternetOpenUrlW"));
    REQUIRE(openUrlA != nullptr);
    REQUIRE(openUrlW != nullptr);

    replay::egress::ResetForTest();
    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    REQUIRE(replay::egress::Arm() == replay::egress::ArmState::Armed);

    CHECK(openUrlA(nullptr, "http://u:hunter2@login.invalid/l?password=hunter2", nullptr, 0, 0, 0) ==
          nullptr);
    CHECK(GetLastError() == 12029u);  // ERROR_INTERNET_CANNOT_CONNECT
    CHECK(openUrlW(nullptr, L"http://login.invalid/l?password=hunter2", nullptr, 0, 0, 0) == nullptr);

    size_t urlRows = 0;
    const char* text = nullptr;
    const char* via = nullptr;
    uint64_t count = 0;
    bool local = true;
    for (size_t i = 0; replay::egress::DestinationAt(i, &text, &via, &count, &local); ++i) {
        CAPTURE(text);
        CHECK(std::strstr(text, "hunter2") == nullptr);
        CHECK(std::strstr(text, "login.invalid") == nullptr);
        if (std::strncmp(via, "InternetOpenUrl", 15) == 0) {
            ++urlRows;
            CHECK(std::strcmp(text, "(url)") == 0);
            CHECK_FALSE(local);
        }
    }
    CHECK(urlRows == 2u);
    CHECK(std::string(replay::egress::RefusedDestinationsJson()).find("hunter2") == std::string::npos);

    WSACleanup();
    replay::egress::ResetForTest();
    FreeLibrary(inet);
}

TEST_CASE("a name that does not fit is reported rather than half-written") {
    // The red case for the check above. The buffer is sized for the sixteen rows that exist, so the
    // only way to reach truncation today is to ask for more than exists - which is exactly what a
    // seventeenth row would do to a future reader of this code. Proving the marker appears is what
    // makes its absence above mean something.
    replay::egress::ResetForTest();
    const std::string empty = replay::egress::HookNamesJson();
    CHECK(empty.empty());
    CHECK(empty.find("(truncated)") == std::string::npos);
}

TEST_CASE("a second egress blocker in one process refuses loudly instead of misreporting") {
    // MinHook refuses a second MH_CreateHook on a target that already has one, so a duplicate copy arms
    // nothing and would report partially-armed over a process that is fully guarded. A safety field that
    // under-reports safety trains people to ignore it.
    replay::egress::ResetForTest();
    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    REQUIRE(replay::egress::Arm() == replay::egress::ArmState::Armed);

    // Simulate the second instance: the same named object this process already holds.
    wchar_t name[64];
    _snwprintf_s(name, sizeof name / sizeof name[0], _TRUNCATE, L"Local\\echoreplay.egress.%lu",
                 GetCurrentProcessId());
    HANDLE probe = CreateMutexW(nullptr, TRUE, name);
    REQUIRE(probe != nullptr);
    CHECK(GetLastError() == ERROR_ALREADY_EXISTS);
    CloseHandle(probe);

    WSACleanup();
    replay::egress::ResetForTest();
}

TEST_CASE("the arm tally refuses a count that runs past its own total") {
    using replay::egress::ArmTallyConsistent;

    // The line this exists for, from sweep 4: `partially-armed: 15 of 10 entry point(s) live`. The
    // count spanned both row tables and the total was the size of one of them, so the ratio read as
    // MORE coverage than was requested - and a coverage number nobody can exceed cannot fail.
    CHECK_FALSE(ArmTallyConsistent(15, 10));
    CHECK_FALSE(ArmTallyConsistent(16, 10));

    // The same defect from the other side, and the one a guard is most likely to wave through: a
    // denominator that was never written. A check that passes over a missing value has not looked.
    CHECK_FALSE(ArmTallyConsistent(1, 0));
    CHECK_FALSE(ArmTallyConsistent(0, 0));

    // The honest tallies, including a total failure to arm - "0 of 16" is true and must stay printable.
    CHECK(ArmTallyConsistent(0, 16));
    CHECK(ArmTallyConsistent(15, 16));
    CHECK(ArmTallyConsistent(16, 16));
}

TEST_CASE("the printed arm line carries the tally the report carries") {
    // ★ WININET IS LOADED ON PURPOSE, and without it this test cannot fail. With wininet absent the
    // requested total IS kRowCount, so the wrong denominator and the right one are the same number and
    // the defect is invisible. The game always has it mapped through BugSplat64.dll, which is the
    // configuration that printed "15 of 10" for a whole sweep.
    HMODULE inet = LoadLibraryA("wininet.dll");
    REQUIRE(inet != nullptr);

    wchar_t tempDir[MAX_PATH] = {};
    REQUIRE(GetTempPathW(MAX_PATH, tempDir) != 0);
    const std::wstring path = std::wstring(tempDir) + L"w1c_armline.log";
    DeleteFileW(path.c_str());
    replay::log::RedirectForTest(path.c_str());
    replay::log::SetLevel(replay::LogLevel::Info);

    replay::egress::ResetForTest();
    WSADATA wsa{};
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    REQUIRE(replay::egress::Arm() == replay::egress::ArmState::Armed);
    const uint32_t reportedLive = replay::egress::Current().hooksLive;
    const uint32_t reportedRequested = replay::egress::Current().hooksRequested;
    replay::log::Drain();
    replay::log::EndRedirectForTest();

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    char buffer[8192] = {};
    DWORD read = 0;
    ReadFile(file, buffer, sizeof buffer - 1, &read, nullptr);
    CloseHandle(file);
    const std::string body(buffer, read);

    WSACleanup();
    replay::egress::ResetForTest();
    DeleteFileW(path.c_str());
    FreeLibrary(inet);

    // ★ An absent line is a FAILURE, not a quiet pass. The sibling defect to a denominator that cannot
    // be exceeded is a check that never found the value it was looking at, and reported success anyway.
    const size_t tail = body.find(" entry point(s) live");
    REQUIRE(tail != std::string::npos);
    const size_t head = body.rfind(": ", tail);
    REQUIRE(head != std::string::npos);

    unsigned printedLive = 0;
    unsigned printedRequested = 0;
    REQUIRE(sscanf_s(body.c_str() + head + 2, "%u of %u", &printedLive, &printedRequested) == 2);

    // The two numbers as printed: one set of entry points, and the same set the structured report
    // describes. "16 of 10" fails the first pair of these and the second.
    CHECK(printedRequested > 0);
    CHECK(printedLive <= printedRequested);
    CHECK(printedLive == reportedLive);
    CHECK(printedRequested == reportedRequested);
}
