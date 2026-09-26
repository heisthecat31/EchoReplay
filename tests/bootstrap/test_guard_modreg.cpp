// Two things that must hold before this plugin writes a byte into a running game:
//   - a build it does not recognise produces a refusal, in every combination of evidence, and never an
//     "accept anyway";
//   - the module registry dispatches in a defined order and one broken module does not take the rest down.

#include <cstring>
#include <string>
#include <vector>

#include <doctest.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/md5.h"
#include "generated/facts_generated.h"
#include "bootstrap/log.h"
#include "bootstrap/modreg.h"
#include "bootstrap/services.h"

namespace {

replay::BuildIdentity Good() {
    replay::BuildIdentity id{};
    id.md5Read = true;
    std::memcpy(id.md5, replay::ExpectedMd5(), replay::kMd5HexChars);
    id.fileSize = 35397120;
    id.imageScanned = true;
    id.versionStringCount = 1;
    id.sitesChecked = true;
    id.siteCount = 10;
    id.siteMismatches = 0;
    return id;
}

replay::BuildVerdict Verdict(const replay::BuildIdentity& id, std::string* why) {
    char buffer[512] = {};
    const replay::BuildVerdict v = replay::EvaluateBuild(id, buffer, sizeof buffer);
    *why = buffer;
    return v;
}

}  // namespace

TEST_CASE("a build the registry was read from is accepted, and every deviation from it is not") {
    std::string why;
    CHECK(Verdict(Good(), &why) == replay::BuildVerdict::Accept);
    CHECK(why.find(replay::ExpectedMd5()) != std::string::npos);

    SUBCASE("a different executable") {
        replay::BuildIdentity id = Good();
        // A second, different build of the same game installed side by side is a real case, and
        // targeting it by accident is exactly the mistake this refusal exists for.
        std::memcpy(id.md5, "7d2eb1fc00000000000000000000dead", replay::kMd5HexChars);
        CHECK(Verdict(id, &why) == replay::BuildVerdict::RefuseWrongBuild);
        CHECK(why.find("7d2eb1fc") != std::string::npos);
        CHECK(why.find(replay::ExpectedMd5()) != std::string::npos);
    }
    SUBCASE("an md5 that differs in one digit") {
        replay::BuildIdentity id = Good();
        id.md5[31] = id.md5[31] == '5' ? '6' : '5';
        CHECK(Verdict(id, &why) == replay::BuildVerdict::RefuseWrongBuild);
    }
    SUBCASE("the executable could not be hashed") {
        replay::BuildIdentity id = Good();
        id.md5Read = false;
        // Distinct from a wrong build: "we do not know" and "we know it is wrong" need different
        // messages, and both need the same refusal.
        CHECK(Verdict(id, &why) == replay::BuildVerdict::RefuseUnreadable);
    }
    SUBCASE("the build string is missing from the image") {
        replay::BuildIdentity id = Good();
        id.versionStringCount = 0;
        CHECK(Verdict(id, &why) == replay::BuildVerdict::RefuseWrongBuild);
        CHECK(why.find(replay::ExpectedVersionString()) != std::string::npos);
    }
    SUBCASE("the build string appears more than once") {
        replay::BuildIdentity id = Good();
        id.versionStringCount = 2;
        CHECK(Verdict(id, &why) == replay::BuildVerdict::RefuseWrongBuild);
    }
    SUBCASE("the image could not be scanned") {
        replay::BuildIdentity id = Good();
        id.imageScanned = false;
        CHECK(Verdict(id, &why) == replay::BuildVerdict::RefuseUnreadable);
    }
    SUBCASE("a foreign hook on a fingerprint site does NOT refuse the load") {
        // ★ The orchestrator's 2026-08-28 ruling, locked in. rlay.dll - a Detours DLL
        // sharing plugins with us - had hooked SESSION.STATE_MACHINE, and the old guard refused a correct
        // build of the right executable outright. That broke "coexist, never replace" and SC9 and bought
        // nothing: the md5 had already identified the build, and none of the ten sites is one we hook.
        replay::BuildIdentity id = Good();
        id.siteMismatches = 1;
        id.siteForeignDetours = 1;
        std::memcpy(id.firstMismatch, "SESSION_STATE_MACHINE", sizeof "SESSION_STATE_MACHINE");
        CHECK(Verdict(id, &why) == replay::BuildVerdict::Accept);
        // ... and it is still said out loud, naming the site.
        CHECK(why.find("SESSION_STATE_MACHINE") != std::string::npos);
        CHECK(why.find("another module") != std::string::npos);
    }
    SUBCASE("an unexplained difference is loud but still not an identity question") {
        replay::BuildIdentity id = Good();
        id.siteMismatches = 2;
        id.siteForeignDetours = 1;
        id.siteUnexplained = 1;
        std::memcpy(id.firstMismatch, "GAME_UPDATE", sizeof "GAME_UPDATE");
        CHECK(Verdict(id, &why) == replay::BuildVerdict::Accept);
        CHECK(why.find("not recognisable as a hook") != std::string::npos);
    }
    SUBCASE("the md5 still decides, whatever the sites say") {
        // The control for the two above: if a wrong md5 passed too, the split would have removed the
        // guard rather than sharpened it.
        replay::BuildIdentity id = Good();
        id.siteMismatches = 0;
        std::memcpy(id.md5, "7d2eb1fc00000000000000000000dead", replay::kMd5HexChars);
        CHECK(Verdict(id, &why) == replay::BuildVerdict::RefuseWrongBuild);
    }
    SUBCASE("a zeroed identity") {
        replay::BuildIdentity id{};
        CHECK(Verdict(id, &why) != replay::BuildVerdict::Accept);
    }
}

TEST_CASE("the startup identity set is populated and every entry names a fact") {
    // An empty site table would make the guard vacuous while still reporting a verdict, so the count is
    // asserted here and cross-checked against facts.tsv and the real binary by tests/ledgerkeys.
    REQUIRE(replay::buildguard::SiteCount() >= 6);
    for (size_t i = 0; i < replay::buildguard::SiteCount(); ++i) {
        const char* id = replay::buildguard::SiteFactId(i);
        REQUIRE(id != nullptr);
        CHECK(std::strlen(id) > 0);
    }
    CHECK(replay::buildguard::SiteFactId(replay::buildguard::SiteCount()) == nullptr);
}

// ---------------------------------------------------------------------------------------------------

namespace {

std::vector<std::string> g_order;

bool InitA(replay::ReplayServices*) { g_order.push_back("init:a"); return true; }
bool InitB(replay::ReplayServices*) { g_order.push_back("init:b"); return true; }
bool InitFails(replay::ReplayServices*) { g_order.push_back("init:broken"); return false; }
void TickA(const replay::TickInfo&) { g_order.push_back("tick:a"); }
void TickBroken(const replay::TickInfo&) { g_order.push_back("tick:broken"); }
void ShutA() { g_order.push_back("down:a"); }
void ShutB() { g_order.push_back("down:b"); }
void ShutBroken() { g_order.push_back("down:broken"); }

const replay::ReplayModule kLate{"late", 20, InitB, nullptr, ShutB};
const replay::ReplayModule kEarly{"early", 10, InitA, TickA, ShutA};
const replay::ReplayModule kBroken{"broken", 15, InitFails, TickBroken, ShutBroken};
const replay::ReplayModule kTieB{"b-tie", 10, nullptr, nullptr, nullptr};

}  // namespace

TEST_CASE("modules run in declared order, and one that refuses does not take the others with it") {
    replay::modreg::ResetForTest();
    g_order.clear();

    // Registered out of order on purpose: the ordering must come from the `order` field, not from link
    // order, or it would change when someone adds a file.
    replay::modreg::RegisterForTest(kLate);
    replay::modreg::RegisterForTest(kBroken);
    replay::modreg::RegisterForTest(kEarly);
    replay::modreg::RegisterForTest(kTieB);
    REQUIRE(replay::modreg::Count() == 4);

    replay::ReplayServices services{};
    const size_t live = replay::modreg::InitAll(&services);
    CHECK(live == 3);
    CHECK(replay::modreg::InitFailures() == 1);
    // order 10 twice (b-tie before early by name), then 15, then 20.
    REQUIRE(g_order.size() == 3);
    CHECK(g_order[0] == "init:a");
    CHECK(g_order[1] == "init:broken");
    CHECK(g_order[2] == "init:b");
    CHECK(std::string(replay::modreg::At(0)->name) == "b-tie");
    CHECK(std::string(replay::modreg::At(1)->name) == "early");

    // The tick walks only modules that initialised AND declared a tick, so the broken one is not called.
    //
    // replay::TickInfo is W1-D's type (Appendix B.3) and is incomplete here, which is exactly the situation this
    // packet froze the signature for. A reference binds to storage without the type being complete, and
    // no module under test reads through it.
    alignas(16) unsigned char tickStorage[128] = {};
    const replay::TickInfo& info = *reinterpret_cast<const replay::TickInfo*>(tickStorage);

    g_order.clear();
    replay::modreg::TickAll(info);
    REQUIRE(g_order.size() == 1);
    CHECK(g_order[0] == "tick:a");

    // Shutdown is reverse order and skips whatever never came up.
    g_order.clear();
    replay::modreg::ShutdownAll();
    REQUIRE(g_order.size() == 2);
    CHECK(g_order[0] == "down:b");
    CHECK(g_order[1] == "down:a");

    // Ticking after shutdown calls nothing: a module that has released its state must not be re-entered.
    g_order.clear();
    replay::modreg::TickAll(info);
    CHECK(g_order.empty());

    replay::modreg::ResetForTest();
    CHECK(replay::modreg::Count() == 0);
}

// ---------------------------------------------------------------------------------------------------

namespace {

const char* const kReplayModuleName = "require-demo";

// B.12: "A pointer in replay::ReplayServices may be null ... Every consumer checks. W1-C's acceptance includes a
// module that asserts on a null it did not declare." This is that check, exercised rather than described.
bool NeedsEntity(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, entity);
    return true;
}

}  // namespace

TEST_CASE("a module refuses to initialise on a service it declared and did not get") {
    replay::ReplayServices services{};
    CHECK_FALSE(NeedsEntity(&services));           // log is null
    CHECK_FALSE(NeedsEntity(nullptr));             // no locator at all

    services.log = replay::log::Interface();
    CHECK_FALSE(NeedsEntity(&services));           // entity is still null

    // Not a real replay::IEntity - the point is only that a non-null pointer satisfies the requirement, and this
    // packet cannot see W1-E's implementation.
    services.entity = reinterpret_cast<replay::IEntity*>(&services);
    CHECK(NeedsEntity(&services));
}

TEST_CASE("a differing site is classified, because 'wrong build' and 'someone hooked it' need opposite answers") {
    // Detours and MinHook both write a branch at the front of the target. That is the whole signal, and
    // without it the guard cannot tell a foreign patch from a different executable - which is how a
    // correct build came to be refused in the 2026-08-28 sweep.
    const uint8_t jmpRel32[]  = {0xE9, 0x11, 0x22, 0x33, 0x44};
    const uint8_t jmpShort[]  = {0xEB, 0xF9};
    const uint8_t jmpIndirect[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const uint8_t movRaxImm[] = {0x48, 0xB8, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const uint8_t breakpoint[] = {0xCC, 0x55, 0x48};
    const uint8_t ordinary[]  = {0x40, 0x55, 0x57, 0x41, 0x57};

    CHECK(replay::LooksLikeForeignHook(jmpRel32, sizeof jmpRel32));
    CHECK(replay::LooksLikeForeignHook(jmpShort, sizeof jmpShort));
    CHECK(replay::LooksLikeForeignHook(jmpIndirect, sizeof jmpIndirect));
    CHECK(replay::LooksLikeForeignHook(movRaxImm, sizeof movRaxImm));

    // A breakpoint is not a hook, and neither is an ordinary prologue that simply differs. Classifying
    // those as foreign detours would let a genuinely wrong build hide behind the coexistence path.
    CHECK_FALSE(replay::LooksLikeForeignHook(breakpoint, sizeof breakpoint));
    CHECK_FALSE(replay::LooksLikeForeignHook(ordinary, sizeof ordinary));
    CHECK_FALSE(replay::LooksLikeForeignHook(nullptr, 0));
    CHECK_FALSE(replay::LooksLikeForeignHook(jmpIndirect, 1));   // FF alone is not FF 25

    CHECK(std::string(replay::ClassifyMismatch(jmpRel32, sizeof jmpRel32)).find("JMP rel32") !=
          std::string::npos);
    CHECK(std::string(replay::ClassifyMismatch(breakpoint, sizeof breakpoint)).find("breakpoint") !=
          std::string::npos);
    CHECK(std::string(replay::ClassifyMismatch(ordinary, sizeof ordinary)).find("simply different") !=
          std::string::npos);
    CHECK(replay::ClassifyMismatch(nullptr, 0) != nullptr);
}

TEST_CASE("not one build-guard site is a site this project hooks") {
    // The reason the split above is safe, asserted rather than remembered. If a future packet ever adds
    // a hook on one of these ten, this stops being true and somebody has to think about it again.
    //
    // The sites we hook come from DESIGN S4.4: NETGAME.UPDATE (Level 1 fallback tick), RENDER.FN
    // (2.1), VOIP.RECEIVE (2.3). Row 2.2's GUI.NETGAME_PROCESSUI was superseded by the stock 0.20 route.
    const uint64_t hooked[] = {
        facts::NETGAME_UPDATE,
        facts::RENDER_FN,
        facts::VOIP_RECEIVE,
    };
    size_t overlaps = 0;
    const uint64_t sites[] = {
        facts::ENTRY_WINMAIN,   facts::ENTRY_PLUGIN_LOAD, facts::GAME_INITIALIZE,
        facts::GAME_SINGLETON_WRITE_VA, facts::GAME_UPDATE, facts::NSPROVIDER_UPDATE,
        facts::TICK_RUNLOOP,    facts::CLI_PARSE,         facts::SESSION_STATE_MACHINE,
        facts::GAME_SHUTDOWN,
    };
    CHECK(sizeof sites / sizeof sites[0] == replay::buildguard::SiteCount());
    for (const uint64_t site : sites) {
        for (const uint64_t va : hooked) {
            if (site == va) {
                ++overlaps;
            }
        }
    }
    CHECK(overlaps == 0);
}
