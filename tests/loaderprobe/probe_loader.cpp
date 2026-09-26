// A stand-in for dbgcore.dll (the EchoLoader hijack), reproducing the two things about it that shape this
// whole packet: it LoadLibrary's every *.dll in current_path()/plugins FROM INSIDE ITS OWN DllMain, and it
// discards the return value.
//
// So a plugin loaded this way runs its DLL_PROCESS_ATTACH with the Windows loader lock held, entered from
// another DLL's DllMain, and a failed load tells nobody anything. That is the condition W1-C's entry point
// is written for and the condition no unit test reaches, which is why this is a real DLL rather than a
// mock.
//
// Deliberately not a faithful copy of EchoLoader - it is a faithful copy of the two properties that
// constrain us. REPLAY_PROBE_ORDER=reverse flips the scan so SC9's "either load order" is exercised.

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

void LoadPlugins() {
    wchar_t cwd[1024];
    const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd);
    if (n == 0 || n >= std::size(cwd)) {
        return;
    }
    const std::wstring dir = std::wstring(cwd) + L"\\plugins";

    std::vector<std::wstring> found;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring name = entry.cFileName;
        // EchoLoader's own filter: the file name must contain lowercase ".dll".
        if (name.find(L".dll") == std::wstring::npos) {
            continue;
        }
        found.push_back(dir + L"\\" + name);
    } while (FindNextFileW(search, &entry));
    FindClose(search);

    std::sort(found.begin(), found.end());
    wchar_t order[16] = {};
    if (GetEnvironmentVariableW(L"REPLAY_PROBE_ORDER", order, static_cast<DWORD>(std::size(order))) != 0 &&
        std::wstring(order) == L"reverse") {
        std::reverse(found.begin(), found.end());
    }

    for (const std::wstring& path : found) {
        // Return value discarded, exactly as the real loader does.
        LoadLibraryW(path.c_str());
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LoadPlugins();
    }
    return TRUE;
}
