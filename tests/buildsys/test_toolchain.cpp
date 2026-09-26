// Pins the compiler settings the rest of the tree assumes. Every one of these is a setting whose loss
// would otherwise be silent: a /MD build only fails when it meets the game's CRT, a c++17 build only
// fails on the first c++20 construct someone writes.

#include <cstdlib>

#include <doctest.h>

TEST_CASE("compiler is MSVC 14.38 or newer") {
#if defined(__clang__) || defined(__GNUC__)
    FAIL("MSVC only. MinGW on this machine produces zero-byte output with 'ld returned 5'.");
#endif
    INFO("_MSC_VER=" << _MSC_VER << " _MSC_FULL_VER=" << _MSC_FULL_VER);
    // The minimum is MSVC 14.38 (Visual Studio 2022 17.8); tools/build.ps1 refuses anything older.
    CHECK(_MSC_VER >= 1938);
}

TEST_CASE("target is x64") {
#if defined(_M_X64)
    CHECK(sizeof(void*) == 8);
#else
    FAIL("echovr.exe is x64; a 32-bit plugin cannot load into it.");
#endif
}

TEST_CASE("CRT is static (/MT)") {
#if !defined(_MT)
    FAIL("_MT is not defined: the multithreaded CRT was not selected.");
#endif
#if defined(_DLL)
    FAIL("_DLL is defined: this is a /MD build. The plugin ships /MT so it carries no vcruntime "
         "dependency into bin\\win10.");
#endif
    CHECK(true);
}

TEST_CASE("language standard is c++20") {
    INFO("_MSVC_LANG=" << _MSVC_LANG);
    CHECK(_MSVC_LANG == 202002L);
}

TEST_CASE("windows headers are trimmed") {
#if !defined(WIN32_LEAN_AND_MEAN)
    FAIL("WIN32_LEAN_AND_MEAN is not defined.");
#endif
#if !defined(NOMINMAX)
    FAIL("NOMINMAX is not defined: windows.h would define min/max macros over <algorithm>.");
#endif
    CHECK(true);
}

TEST_CASE("gate mutation hook") {
    // Deliberate-failure switch, documented in docs/INSTALL.md and driven by
    // tools/gate.ps1 -Mutate fail-test. It exists so the gate's FAIL path can be watched going red.
    char* value = nullptr;
    size_t size = 0;
    _dupenv_s(&value, &size, "REPLAY_GATE_MUTATE_FAIL");
    const bool mutationRequested = value != nullptr;
    std::free(value);
    CHECK_FALSE(mutationRequested);
}
