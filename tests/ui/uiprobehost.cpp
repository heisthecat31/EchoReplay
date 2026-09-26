// W2-F. Loads probe_ui.dll the way the game's loader does, so the offline gate can prove the probe
// FILES rather than take the filer's existence as evidence that it works.
//
// The property under test is narrow and it is the one that has silently eaten probe verdicts on
// this project: the PROVISIONAL done.json must be written from a STATIC INITIALISER -- during
// LoadLibrary, before DllMain, before any bootstrap module's coexist claim, and before the sampler
// exists. So this host does nothing but load, wait briefly, and exit. It never reaches a match and
// never should: if the file is there after a plain LoadLibrary in a process that is not the game,
// the filing does not depend on anything the game provides.
//
// Exit codes: 0 loaded, 2 bad usage, 3 LoadLibrary failed (with GetLastError printed).

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: uiprobehost <dll> [waitMs]\n");
        return 2;
    }
    const DWORD waitMs = argc >= 3 ? static_cast<DWORD>(wcstol(argv[2], nullptr, 10)) : 1500;

    const HMODULE h = LoadLibraryW(argv[1]);
    if (h == nullptr) {
        std::printf("LoadLibraryW failed, error %lu\n", GetLastError());
        return 3;
    }
    std::printf("loaded %ls\n", argv[1]);
    // The static initialiser has already run by the time LoadLibrary returns. The wait is only so a
    // sampler that files early cannot be mistaken for the static initialiser having done it.
    Sleep(waitMs);
    std::printf("exiting after %lu ms\n", waitMs);
    return 0;
}
