// What DLL_PROCESS_DETACH is allowed to do, and the pre-exit path that took over the one job it had.
//
// Launch 07:37 (2026-09-18): ExitProcess -> Detach(true) -> modreg::ShutdownAll() -> the clock's
// restore wrote into a CClock the engine had freed two seconds earlier. Every engine call and every
// thread join on the exit path lives in some module's Shutdown, so "Detach ran no module shutdown"
// is the whole claim, and a fake module is enough to observe it.

#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <doctest.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/log.h"
#include "bootstrap/modreg.h"

namespace {

int g_shutdowns = 0;
bool InitOk(replay::ReplayServices*) { return true; }
void CountShutdown() { ++g_shutdowns; }
const replay::ReplayModule kEngineToucher{"engine-toucher", 10, InitOk, nullptr, CountShutdown};

std::vector<uint32_t> g_budgets;
std::vector<char> g_ran;
void HookA(uint32_t budgetMs) { g_ran.push_back('a'); g_budgets.push_back(budgetMs); }
void HookSlow(uint32_t budgetMs) {
    g_ran.push_back('s');
    g_budgets.push_back(budgetMs);
    Sleep(120);
}
void HookB(uint32_t budgetMs) { g_ran.push_back('b'); g_budgets.push_back(budgetMs); }

std::wstring TempLog(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    REQUIRE(GetTempPathW(MAX_PATH, dir) != 0);
    std::wstring path = std::wstring(dir) + name;
    DeleteFileW(path.c_str());
    return path;
}

std::string Slurp(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

struct LiveFakeModule {
    std::wstring logPath;
    explicit LiveFakeModule(const wchar_t* logName) : logPath(TempLog(logName)) {
        replay::log::RedirectForTest(logPath.c_str());
        replay::modreg::ResetForTest();
        replay::modreg::RegisterForTest(kEngineToucher);
        replay::ReplayServices services{};
        REQUIRE(replay::modreg::InitAll(&services) == 1);
        g_shutdowns = 0;
        g_ran.clear();
        g_budgets.clear();
    }
    ~LiveFakeModule() {
        replay::modreg::ResetForTest();
        replay::log::EndRedirectForTest();
        DeleteFileW(logPath.c_str());
    }
};

}  // namespace

TEST_CASE("process-terminating detach shuts no module down and runs no pre-exit hook") {
    LiveFakeModule live(L"echoreplay_test_detach_exit.log");
    REQUIRE(replay::modreg::RegisterPreExit(&HookA));

    replay::bootstrap::Detach(true);
    CHECK(g_shutdowns == 0);
    CHECK(g_ran.empty());

    // The control: the module was live and its shutdown observable, so the zero above is Detach's.
    replay::modreg::ShutdownAll();
    CHECK(g_shutdowns == 1);
}

TEST_CASE("a FreeLibrary detach still says so, and still joins and shuts down nothing") {
    // The module pins itself, so this path is a should-never-happen: it warns and signals the worker.
    // It never ran module shutdowns -- they join threads, and the loader lock is held here too.
    LiveFakeModule live(L"echoreplay_test_detach_unload.log");

    replay::bootstrap::Detach(false);
    CHECK(g_shutdowns == 0);
    CHECK(Slurp(live.logPath).find("FreeLibrary on a pinned module") != std::string::npos);

    // Only the unload path says it: the exit path's log must not carry the warning.
    const std::wstring exitLog = TempLog(L"echoreplay_test_detach_exit2.log");
    replay::log::RedirectForTest(exitLog.c_str());
    replay::bootstrap::Detach(true);
    CHECK(Slurp(exitLog).find("FreeLibrary on a pinned module") == std::string::npos);
    DeleteFileW(exitLog.c_str());
}

TEST_CASE("pre-exit hooks run in registration order, once each, inside one shared budget") {
    LiveFakeModule live(L"echoreplay_test_preexit.log");
    REQUIRE(replay::modreg::RegisterPreExit(&HookA));
    REQUIRE(replay::modreg::RegisterPreExit(&HookSlow));
    REQUIRE(replay::modreg::RegisterPreExit(&HookA));   // a second Init registers again: still one
    REQUIRE(replay::modreg::RegisterPreExit(&HookB));
    CHECK_FALSE(replay::modreg::RegisterPreExit(nullptr));

    replay::modreg::RunPreExit(1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    REQUIRE(g_ran.size() == 3);
    CHECK(std::string(g_ran.begin(), g_ran.end()) == "asb");
    CHECK(g_budgets[0] == 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(g_budgets[1] <= 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    // The slow hook spent 120 ms of it; GetTickCount64 is 16 ms coarse, so the bound is loose on
    // one side and exact on the other.
    CHECK(g_budgets[2] <= 1000 - 100);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(g_budgets[2] >= 1000 - 400);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(g_shutdowns == 0);

    // A budget already spent is handed on as zero, never wrapped.
    g_ran.clear();
    g_budgets.clear();
    replay::modreg::RunPreExit(50);
    REQUIRE(g_budgets.size() == 3);
    CHECK(g_budgets[0] == 50);
    CHECK(g_budgets[2] == 0);

    replay::modreg::ResetForTest();
    g_ran.clear();
    replay::modreg::RunPreExit(1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
    CHECK(g_ran.empty());
}
