// HOST-NV. The six setter detours driven the way the engine enters them, forwarding to stand-in
// originals that record exactly what reached them. No MinHook and no game: the install path is
// driven through hook stand-ins, and the (slot, symbol) filter through the detours themselves.

#include "doctest.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "bootstrap/config.h"
#include "playback/core/netvarguard.h"
#include "playback/core/netvarsite.h"

using namespace replay;
using namespace replay::playback;
namespace guard = replay::playback::netvarguard;

namespace {

constexpr uint64_t kTimer = 0xB2996AD484F1D1E8ull;
constexpr uint64_t kState = 0xB2996AD399FDC0FFull;
constexpr uint64_t kOther = 0x02FE0AA4F2637309ull;
constexpr uint16_t kSlot = 3;
constexpr uint16_t kOtherSlot = 4;
constexpr uint32_t kGeneration = 0x00070000u;   // a handle is generation << 16 | slot

struct Reached {
    char     site;
    void*    target;
    uint32_t handle;
    uint64_t symbol;
    uint64_t value;
    float    real;
};
std::vector<Reached> g_reached;
void (*g_onOriginal)() = nullptr;   // what the engine's synchronous change delegate would run

void Note(const Reached& r) {
    g_reached.push_back(r);
    if (g_onOriginal) g_onOriginal();
}
void __fastcall OrigFlag(void* cs, uint32_t h, uint64_t sym, uint8_t v) { Note({'f', cs, h, sym, v, 0}); }
void __fastcall OrigInt(void* cs, uint32_t h, uint64_t sym, uint32_t v) { Note({'i', cs, h, sym, v, 0}); }
void __fastcall OrigReal(void* cs, uint32_t h, uint64_t sym, float v) { Note({'r', cs, h, sym, 0, v}); }
void __fastcall OrigRealNetGame(void* ng, uint64_t sym, float v) { Note({'R', ng, 0, sym, 0, v}); }
void __fastcall OrigSymbol(void* cs, uint32_t h, uint64_t sym, uint64_t v, float life) {
    Note({'s', cs, h, sym, v, life});
}
void __fastcall OrigSymbolNetGame(void* ng, uint64_t sym, uint64_t v, float life) {
    Note({'S', ng, 0, sym, v, life});
}

guard::Setters FakeOriginals() {
    return {&OrigFlag, &OrigInt, &OrigReal, &OrigRealNetGame, &OrigSymbol, &OrigSymbolNetGame};
}

// Just enough netGame for the two netGame-form detours: the global netvar handle.
struct FakeNetGame {
    uint8_t bytes[kNetGameNetVarHandleOff + sizeof(uint32_t)] = {};
    explicit FakeNetGame(uint16_t slot) {
        const uint32_t handle = kGeneration | slot;
        memcpy(bytes + kNetGameNetVarHandleOff, &handle, sizeof handle);
    }
    void* Ptr() { return bytes; }
};

struct Fixture {
    int cs = 0;
    const guard::Setters& detour = guard::DetoursForTest();

    Fixture() {
        guard::ResetForTest();
        g_reached.clear();
        g_onOriginal = nullptr;
        guard::SetOriginalsForTest(FakeOriginals());
    }
    ~Fixture() {
        guard::ResetForTest();
        g_onOriginal = nullptr;
    }
    void Own(uint64_t symbol, uint16_t slot, size_t position = 0) {
        guard::OwnedKey keys[guard::kMaxOwned] = {};
        keys[position].symbol = symbol;
        keys[position].slot = slot;
        guard::PublishOwned(keys);
    }
    guard::Counts Drain() {
        guard::Counts c;
        guard::DrainCounts(c);
        return c;
    }
};

}  // namespace

TEST_CASE("netvarguard: a host write to an owned (slot, symbol) is swallowed; every neighbour passes") {
    Fixture f;
    f.Own(kTimer, kSlot, 2);
    guard::SetMode(guard::Mode::Filter);

    f.detour.setReal(&f.cs, kGeneration | kSlot, kTimer, 300.0f);
    CHECK(g_reached.empty());

    // The same symbol in a component the replay does not drive.
    f.detour.setReal(&f.cs, kGeneration | kOtherSlot, kTimer, 300.0f);
    REQUIRE(g_reached.size() == 1);
    CHECK(g_reached[0].handle == (kGeneration | kOtherSlot));

    // Another symbol in the owned component.
    f.detour.setReal(&f.cs, kGeneration | kSlot, kOther, 1.0f);
    REQUIRE(g_reached.size() == 2);
    CHECK(g_reached[1].symbol == kOther);

    const guard::Counts c = f.Drain();
    CHECK(c.swallowed[2] == 1);
    CHECK(c.passed[2] == 0);
    for (size_t i = 0; i < guard::kMaxOwned; ++i) {
        if (i != 2) CHECK(c.swallowed[i] + c.passed[i] == 0);
    }
    CHECK(f.Drain().swallowed[2] == 0);   // a drain starts the next second from zero
}

TEST_CASE("netvarguard: the generation half of a handle does not take part in ownership") {
    Fixture f;
    f.Own(kTimer, kSlot);
    guard::SetMode(guard::Mode::Filter);
    f.detour.setInt(&f.cs, 0x00990000u | kSlot, kTimer, 5);
    CHECK(g_reached.empty());
}

TEST_CASE("netvarguard: pass-through and observe never swallow; observe counts, pass-through does not") {
    Fixture f;
    f.Own(kTimer, kSlot);

    guard::SetMode(guard::Mode::PassThrough);
    f.detour.setReal(&f.cs, kGeneration | kSlot, kTimer, 300.0f);
    CHECK(g_reached.size() == 1);
    guard::Counts c = f.Drain();
    CHECK(c.passed[0] == 0);
    CHECK(c.swallowed[0] == 0);

    guard::SetMode(guard::Mode::Observe);
    f.detour.setReal(&f.cs, kGeneration | kSlot, kTimer, 300.0f);
    f.detour.setReal(&f.cs, kGeneration | kSlot, kOther, 300.0f);
    CHECK(g_reached.size() == 3);
    c = f.Drain();
    CHECK(c.passed[0] == 1);
    CHECK(c.swallowed[0] == 0);
}

TEST_CASE("netvarguard: each of the six forwards its own arguments untouched, the stack float included") {
    Fixture f;
    FakeNetGame ng(kSlot);
    guard::SetMode(guard::Mode::Filter);   // nothing owned: the filter path itself must not disturb a call

    f.detour.setFlag(&f.cs, 0x11, kOther, 1);
    f.detour.setInt(&f.cs, 0x22, kOther, 0xDEADBEEFu);
    f.detour.setReal(&f.cs, 0x33, kOther, 1.25f);
    f.detour.setRealNetGame(ng.Ptr(), kOther, 2.5f);
    f.detour.setSymbol(&f.cs, 0x55, kOther, 0x0123456789ABCDEFull, 7.75f);
    f.detour.setSymbolNetGame(ng.Ptr(), kOther, 0xFEDCBA9876543210ull, INFINITY);

    REQUIRE(g_reached.size() == 6);
    const char order[] = {'f', 'i', 'r', 'R', 's', 'S'};
    for (size_t i = 0; i < 6; ++i) {
        CHECK(g_reached[i].site == order[i]);
        CHECK(g_reached[i].symbol == kOther);
    }
    CHECK(g_reached[0].target == &f.cs);
    CHECK(g_reached[0].handle == 0x11);
    CHECK(g_reached[0].value == 1);
    CHECK(g_reached[1].handle == 0x22);
    CHECK(g_reached[1].value == 0xDEADBEEFu);
    CHECK(g_reached[2].handle == 0x33);
    CHECK(g_reached[2].real == 1.25f);
    CHECK(g_reached[3].target == ng.Ptr());
    CHECK(g_reached[3].real == 2.5f);
    CHECK(g_reached[4].handle == 0x55);
    CHECK(g_reached[4].value == 0x0123456789ABCDEFull);
    CHECK(g_reached[4].real == 7.75f);
    CHECK(g_reached[5].target == ng.Ptr());
    CHECK(g_reached[5].value == 0xFEDCBA9876543210ull);
    CHECK(isinf(g_reached[5].real));
}

TEST_CASE("netvarguard: every site filters on its own (slot, symbol) -- handle forms by the handle, netGame forms by the global handle") {
    Fixture f;
    FakeNetGame owned(kSlot);
    FakeNetGame elsewhere(kOtherSlot);
    f.Own(kState, kSlot);
    guard::SetMode(guard::Mode::Filter);

    f.detour.setFlag(&f.cs, kGeneration | kSlot, kState, 1);
    f.detour.setInt(&f.cs, kGeneration | kSlot, kState, 1);
    f.detour.setReal(&f.cs, kGeneration | kSlot, kState, 1.0f);
    f.detour.setRealNetGame(owned.Ptr(), kState, 1.0f);
    f.detour.setSymbol(&f.cs, kGeneration | kSlot, kState, 1, 0.0f);
    f.detour.setSymbolNetGame(owned.Ptr(), kState, 1, 0.0f);
    CHECK(g_reached.empty());
    CHECK(f.Drain().swallowed[0] == 6);

    f.detour.setFlag(&f.cs, kGeneration | kOtherSlot, kState, 1);
    f.detour.setInt(&f.cs, kGeneration | kOtherSlot, kState, 1);
    f.detour.setReal(&f.cs, kGeneration | kOtherSlot, kState, 1.0f);
    f.detour.setRealNetGame(elsewhere.Ptr(), kState, 1.0f);
    f.detour.setSymbol(&f.cs, kGeneration | kOtherSlot, kState, 1, 0.0f);
    f.detour.setSymbolNetGame(elsewhere.Ptr(), kState, 1, 0.0f);
    CHECK(g_reached.size() == 6);
}

TEST_CASE("netvarguard: the plugin's own write lands while filtering, and a script write re-entering from its change delegate is still the host's") {
    Fixture f;
    FakeNetGame ng(kSlot);
    f.Own(kTimer, kSlot);
    guard::SetMode(guard::Mode::Filter);

    static Fixture* self;
    static FakeNetGame* game;
    self = &f;
    game = &ng;
    g_onOriginal = [] {
        g_onOriginal = nullptr;   // the delegate fires once
        self->detour.setRealNetGame(game->Ptr(), kTimer, 300.0f);   // the script republishing its clock
        self->detour.setRealNetGame(game->Ptr(), kOther, 9.0f);     // and a netvar of its own
    };

    guard::Originals().setRealNetGame(ng.Ptr(), kTimer, 113.77f);

    REQUIRE(g_reached.size() == 2);
    CHECK(g_reached[0].symbol == kTimer);
    CHECK(g_reached[0].real == 113.77f);
    CHECK(g_reached[1].symbol == kOther);
    const guard::Counts c = f.Drain();
    CHECK(c.swallowed[0] == 1);
}

TEST_CASE("netvarguard: symbol 0 owns nothing, so an unused position never matches a write") {
    Fixture f;
    const guard::OwnedKey none[guard::kMaxOwned] = {};
    guard::PublishOwned(none);
    guard::SetMode(guard::Mode::Filter);
    f.detour.setInt(&f.cs, 0, 0, 1);
    CHECK(g_reached.size() == 1);
}

TEST_CASE("netvarguard: a write racing a publish is never swallowed for a pair neither set owns") {
    Fixture f;
    guard::SetMode(guard::Mode::Filter);
    guard::OwnedKey a[guard::kMaxOwned] = {};
    guard::OwnedKey b[guard::kMaxOwned] = {};
    a[0].symbol = kTimer;
    a[0].slot = kSlot;
    b[0].symbol = kState;
    b[0].slot = kOtherSlot;

    std::atomic<bool> stop{false};
    std::thread publisher([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            guard::PublishOwned(a);
            guard::PublishOwned(b);
        }
    });
    // (kSlot, kState) and (kOtherSlot, kTimer) are in neither set; only a torn cell matches them.
    size_t sent = 0;
    for (; sent < 400'000; ++sent) {
        f.detour.setInt(&f.cs, kGeneration | kSlot, kState, 1);
        f.detour.setInt(&f.cs, kGeneration | kOtherSlot, kTimer, 1);
        g_reached.clear();
    }
    stop.store(true, std::memory_order_relaxed);
    publisher.join();
    const guard::Counts c = f.Drain();
    CHECK(c.swallowed[0] == 0);
}

// ---- install -------------------------------------------------------------------------------------

namespace {

struct HookLog {
    std::vector<std::string> created, armed, verified, keys;
    const char* refuseCreate = nullptr;
    const char* badPrologue = nullptr;
};
HookLog g_hooks;

HookStatus FakeCreate(const char* factId, uint64_t, const uint8_t*, size_t, void* detour,
                      void** trampoline, const char* key) {
    g_hooks.created.push_back(factId);
    g_hooks.keys.push_back(key);
    if (g_hooks.refuseCreate && strcmp(factId, g_hooks.refuseCreate) == 0) {
        *trampoline = nullptr;
        return HookStatus::RefusedByMinHook;
    }
    *trampoline = detour;   // any non-null address: nothing calls it here
    return HookStatus::Created;
}
HookStatus FakeArm(const char* factId) {
    g_hooks.armed.push_back(factId);
    return HookStatus::Armed;
}
bool FakeVerify(const char* factId, uint64_t, const uint8_t*, size_t) {
    g_hooks.verified.push_back(factId);
    return !(g_hooks.badPrologue && strcmp(factId, g_hooks.badPrologue) == 0);
}

struct InstallFixture {
    explicit InstallFixture(const char* resolved) {
        guard::ResetForTest();
        g_hooks = HookLog{};
        config::ResetForTest();
        REQUIRE(config::LoadFromTextForTest("{}", 2));
        if (resolved) REQUIRE(config::SetEffectiveString(guard::kLedgerKey, resolved));
        guard::SetHookAccessForTest(&FakeCreate, &FakeArm, &FakeVerify);
    }
    ~InstallFixture() {
        guard::ResetForTest();
        config::ResetForTest();
    }
};

}  // namespace

TEST_CASE("netvarguard: a contained process gets all six sites, each under the ledger key") {
    InstallFixture f("hook");
    CHECK(guard::Install());
    CHECK(guard::Installed());
    REQUIRE(g_hooks.created.size() == guard::kSiteCount);
    REQUIRE(g_hooks.armed.size() == guard::kSiteCount);
    for (size_t i = 0; i < guard::kSiteCount; ++i) {
        CHECK(g_hooks.created[i] == guard::SiteAt(guard::Site(i)).factId);
        CHECK(g_hooks.armed[i] == g_hooks.created[i]);
        CHECK(g_hooks.keys[i] == "playback.host_netvar_hook");
    }
    CHECK(guard::Originals().setSymbol != nullptr);
    CHECK(guard::CurrentMode() == guard::Mode::PassThrough);   // armed is not filtering
}

TEST_CASE("netvarguard: a live-exempt or unresolved process installs nothing at all") {
    for (const char* resolved : {"off", static_cast<const char*>(nullptr)}) {
        InstallFixture f(resolved);
        CHECK_FALSE(guard::Install());
        CHECK_FALSE(guard::Installed());
        CHECK(g_hooks.verified.empty());
        CHECK(g_hooks.created.empty());
        CHECK(g_hooks.armed.empty());
        CHECK(guard::Originals().setInt == nullptr);

        guard::SetMode(guard::Mode::Filter);
        CHECK(guard::CurrentMode() == guard::Mode::PassThrough);
    }
}

TEST_CASE("netvarguard: one site with a foreign prologue means no trampoline is built for any") {
    InstallFixture f("hook");
    g_hooks.badPrologue = "NETVAR.SET_SYMBOL_HANDLE";
    CHECK_FALSE(guard::Install());
    CHECK_FALSE(guard::Installed());
    CHECK(g_hooks.created.empty());
    CHECK(g_hooks.armed.empty());
}

TEST_CASE("netvarguard: one refused Create leaves every site unarmed") {
    InstallFixture f("hook");
    g_hooks.refuseCreate = "NETVAR.SET_REAL_NETGAME";
    CHECK_FALSE(guard::Install());
    CHECK_FALSE(guard::Installed());
    CHECK(g_hooks.armed.empty());
    CHECK(guard::Originals().setFlag == nullptr);
}

TEST_CASE("netvarguard: the recorded sites are six distinct addresses with 24-byte prologues") {
    for (size_t i = 0; i < guard::kSiteCount; ++i) {
        const guard::SiteFact& a = guard::SiteAt(guard::Site(i));
        CHECK(a.prologueLen == 24);
        for (size_t j = i + 1; j < guard::kSiteCount; ++j) {
            CHECK(a.va != guard::SiteAt(guard::Site(j)).va);
            CHECK(std::string(a.factId) != guard::SiteAt(guard::Site(j)).factId);
        }
    }
}
