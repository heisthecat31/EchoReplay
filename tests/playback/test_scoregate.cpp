// DISC-BIND: the OnScoreCB gate. What the detour does in each mode, and when it installs at all.

#include "doctest.h"

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "bootstrap/config.h"
#include "playback/core/scoregate.h"

using namespace replay;
namespace gate = replay::playback::scoregate;

namespace {

struct Reached {
    void* rules;
    void* msg;
};
std::vector<Reached> g_reached;
constexpr int64_t kOriginalReturn = 0x5C0BE;

int64_t __fastcall FakeOnScore(void* rules, void* msg) {
    g_reached.push_back({rules, msg});
    return kOriginalReturn;
}

struct DetourFixture {
    gate::OnScoreFn detour = gate::DetourForTest();
    int rules = 0, msg = 0;
    DetourFixture() {
        gate::ResetForTest();
        g_reached.clear();
        gate::SetOriginalForTest(&FakeOnScore);
    }
    ~DetourFixture() { gate::ResetForTest(); }
    gate::Counts Drain() {
        gate::Counts c;
        gate::DrainCounts(c);
        return c;
    }
};

struct HookLog {
    std::vector<std::string> created, armed, verified, keys;
    std::vector<uint64_t> vas;
    std::vector<std::vector<uint8_t>> prologues;
    bool refuseCreate = false;
    bool refuseArm = false;
    bool badPrologue = false;
};
HookLog g_hooks;

HookStatus FakeCreate(const char* factId, uint64_t va, const uint8_t* prologue, size_t n,
                      void* detour, void** trampoline, const char* key) {
    g_hooks.created.push_back(factId);
    g_hooks.keys.push_back(key);
    g_hooks.vas.push_back(va);
    g_hooks.prologues.emplace_back(prologue, prologue + n);
    if (g_hooks.refuseCreate) {
        *trampoline = nullptr;
        return HookStatus::RefusedByMinHook;
    }
    *trampoline = detour;   // any non-null address: nothing calls it here
    return HookStatus::Created;
}
HookStatus FakeArm(const char* factId) {
    g_hooks.armed.push_back(factId);
    return g_hooks.refuseArm ? HookStatus::RefusedByMinHook : HookStatus::Armed;
}
bool FakeVerify(const char* factId, uint64_t, const uint8_t*, size_t) {
    g_hooks.verified.push_back(factId);
    return !g_hooks.badPrologue;
}

struct InstallFixture {
    explicit InstallFixture(const char* resolved) {
        gate::ResetForTest();
        g_hooks = HookLog{};
        config::ResetForTest();
        REQUIRE(config::LoadFromTextForTest("{}", 2));
        if (resolved) REQUIRE(config::SetEffectiveString(gate::kLedgerKey, resolved));
        gate::SetHookAccessForTest(&FakeCreate, &FakeArm, &FakeVerify);
    }
    ~InstallFixture() {
        gate::ResetForTest();
        config::ResetForTest();
    }
};

}  // namespace

TEST_CASE("scoregate: a score is swallowed only while the replay is loaded and in a match") {
    DetourFixture f;

    // Nothing loaded: the host's goal is the host's.
    CHECK(f.detour(&f.rules, &f.msg) == kOriginalReturn);
    REQUIRE(g_reached.size() == 1);
    CHECK(g_reached[0].rules == &f.rules);
    CHECK(g_reached[0].msg == &f.msg);

    gate::Publish(gate::Mode::Swallow, 61'250'000);
    CHECK(f.detour(&f.rules, &f.msg) == 0);   // the original's one exit returns 0
    CHECK(f.detour(&f.rules, &f.msg) == 0);
    CHECK(g_reached.size() == 1);
    gate::Counts c = f.Drain();
    CHECK(c.swallowed == 2);
    CHECK(c.passed == 0);
    CHECK(c.lastSwallowedPlayheadUs == 61'250'000);
    CHECK(f.Drain().swallowed == 0);           // drained, not latched

    // Unload, or the match ended: pass again, and uncounted.
    gate::Publish(gate::Mode::PassThrough, 70'000'000);
    CHECK(f.detour(&f.rules, &f.msg) == kOriginalReturn);
    CHECK(g_reached.size() == 2);
    c = f.Drain();
    CHECK(c.swallowed == 0);
    CHECK(c.passed == 0);
}

TEST_CASE("scoregate: with the gate switched off a score in a match is counted and passed") {
    DetourFixture f;
    gate::Publish(gate::Mode::Observe, 5'000'000);
    CHECK(f.detour(&f.rules, &f.msg) == kOriginalReturn);
    CHECK(g_reached.size() == 1);
    const gate::Counts c = f.Drain();
    CHECK(c.swallowed == 0);
    CHECK(c.passed == 1);
}

TEST_CASE("scoregate: a contained process gets the one site, under its own ledger key") {
    InstallFixture f("hook");
    CHECK(gate::Install());
    CHECK(gate::Installed());
    REQUIRE(g_hooks.created.size() == 1);
    CHECK(g_hooks.created[0] == "NETGAME.ON_SCORE_CB");
    CHECK(g_hooks.keys[0] == "playback.host_score_hook");
    CHECK(g_hooks.vas[0] == 0x140185DF0ull);
    // mov [rsp+10h],rbx / mov [rsp+18h],rsi / mov [rsp+20h],rdi / push rbp,r12,r13,r14,r15
    const std::vector<uint8_t> prologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                           0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55,
                                           0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    CHECK(g_hooks.prologues[0] == prologue);
    REQUIRE(g_hooks.verified.size() == 1);
    REQUIRE(g_hooks.armed.size() == 1);
    CHECK(g_hooks.armed[0] == g_hooks.created[0]);
    CHECK(gate::CurrentMode() == gate::Mode::PassThrough);   // armed is not swallowing

    CHECK(gate::Install());                                   // a second Init adds nothing
    CHECK(g_hooks.created.size() == 1);
}

TEST_CASE("scoregate: a live-exempt or unresolved process installs nothing, and never swallows") {
    for (const char* resolved : {"off", static_cast<const char*>(nullptr)}) {
        CAPTURE(resolved);
        InstallFixture f(resolved);
        CHECK_FALSE(gate::Install());
        CHECK_FALSE(gate::Installed());
        CHECK(g_hooks.verified.empty());
        CHECK(g_hooks.created.empty());
        CHECK(g_hooks.armed.empty());
        gate::Publish(gate::Mode::Swallow, 1);
        CHECK(gate::CurrentMode() == gate::Mode::PassThrough);
    }
}

TEST_CASE("scoregate: a foreign prologue, a refused create and a refused arm each leave it out") {
    InstallFixture f("hook");
    SUBCASE("prologue") {
        g_hooks.badPrologue = true;
        CHECK_FALSE(gate::Install());
        CHECK(g_hooks.created.empty());
    }
    SUBCASE("create") {
        g_hooks.refuseCreate = true;
        CHECK_FALSE(gate::Install());
        CHECK(g_hooks.armed.empty());
    }
    SUBCASE("arm") {
        g_hooks.refuseArm = true;
        CHECK_FALSE(gate::Install());
        CHECK(g_hooks.armed.size() == 1);
    }
    CHECK_FALSE(gate::Installed());
    gate::Publish(gate::Mode::Swallow, 1);
    CHECK(gate::CurrentMode() == gate::Mode::PassThrough);
}

// The gate's decision. It used to follow the disc bind, which opened it after every host
// round_over until the re-bind and at main_loop_enter: a host goal there counted, and in overtime
// it would end the match. It now follows the load, in a contained process only.
namespace {

gate::GateState Loaded() {
    gate::GateState s;
    s.contained = true;
    s.loaded = true;
    s.gate_on = true;
    return s;
}

}  // namespace

TEST_CASE("the score gate is shut for the whole of a loaded replay") {
    // Bind, unbind, round end and relaunch change only the disc, and the disc is no longer an
    // input: the one state a loaded replay can be in swallows.
    const gate::Decision d = gate::Decide(Loaded());
    CHECK(d.mode == gate::Mode::Swallow);
    CHECK(std::string(d.why).find("replay is loaded") != std::string::npos);
}

TEST_CASE("the score gate opens when no replay is loaded, and never in a live session") {
    SUBCASE("unloaded") {
        gate::GateState s = Loaded();
        s.loaded = false;
        CHECK(gate::Decide(s).mode == gate::Mode::PassThrough);
    }
    SUBCASE("live: loaded or not, the host keeps every goal") {
        gate::GateState s = Loaded();
        s.contained = false;
        CHECK(gate::Decide(s).mode == gate::Mode::PassThrough);
        s.gate_on = false;
        CHECK(gate::Decide(s).mode == gate::Mode::PassThrough);
        s.loaded = false;
        CHECK(gate::Decide(s).mode == gate::Mode::PassThrough);
    }
}

TEST_CASE("playback.disc_score_gate off counts the host's scores instead of swallowing them") {
    gate::GateState s = Loaded();
    s.gate_on = false;
    const gate::Decision d = gate::Decide(s);
    CHECK(d.mode == gate::Mode::Observe);
    CHECK(std::string(d.why) == "playback.disc_score_gate is off");

    SUBCASE("and it is still the load, not the switch, that decides pass-through") {
        s.loaded = false;
        CHECK(gate::Decide(s).mode == gate::Mode::PassThrough);
    }
}

TEST_CASE("every mode edge can name itself") {
    CHECK(std::string(gate::ModeName(gate::Mode::Swallow)) == "swallow");
    CHECK(std::string(gate::ModeName(gate::Mode::Observe)) == "observe");
    CHECK(std::string(gate::ModeName(gate::Mode::PassThrough)) == "pass-through");
}
