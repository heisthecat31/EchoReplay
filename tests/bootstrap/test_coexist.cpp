// The upgrade hazard of the rename: the old probe_replay.dll left beside EchoReplay.dll loads too, and
// the plugin must say so in its log rather than leave one copy silently refused.

#include <string>

#include <doctest.h>
#include <windows.h>

#include "bootstrap/coexist.h"
#include "bootstrap/paths.h"

namespace {

struct PluginsDir {
    std::wstring root;
    std::wstring plugins;

    explicit PluginsDir(const wchar_t* name) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        root = std::wstring(tmp) + name;
        plugins = root + L"\\plugins";
        CreateDirectoryW(root.c_str(), nullptr);
        CreateDirectoryW(plugins.c_str(), nullptr);
        DeleteFileW(Legacy().c_str());
    }

    ~PluginsDir() { DeleteFileW(Legacy().c_str()); }

    std::wstring Legacy() const { return plugins + L"\\probe_replay.dll"; }

    void PlaceLegacy() const {
        HANDLE h = CreateFileW(Legacy().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        CloseHandle(h);
    }
};

replay::coexist::Report Claim(const std::wstring& selfPath) {
    replay::SetPathsForTest(selfPath.c_str());
    replay::coexist::Report report{};
    CHECK(replay::coexist::ClaimInstance(&report));
    replay::coexist::ReleaseForTest();
    return report;
}

}

TEST_CASE("an old probe_replay.dll beside EchoReplay.dll is reported, and the claim still succeeds") {
    PluginsDir dir(L"coexist_legacy");
    dir.PlaceLegacy();
    CHECK(Claim(dir.plugins + L"\\EchoReplay.dll").legacyCopyBesideUs);
}

TEST_CASE("without the old file nothing is reported") {
    PluginsDir dir(L"coexist_clean");
    CHECK_FALSE(Claim(dir.plugins + L"\\EchoReplay.dll").legacyCopyBesideUs);
}

TEST_CASE("a module that is itself named probe_replay.dll does not report itself") {
    PluginsDir dir(L"coexist_self");
    dir.PlaceLegacy();
    CHECK_FALSE(Claim(dir.Legacy()).legacyCopyBesideUs);
}
