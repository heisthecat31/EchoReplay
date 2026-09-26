// Stands in for echovr.exe's WinMain statement 2: load the loader, let the plugin's worker thread get as
// far as its verdict, exit cleanly.
//
// It polls the log for a marker instead of sleeping a fixed time, so the run is as fast as the machine
// allows and a slow machine produces a longer wait rather than a false failure.
//
//   probe_host.exe <loader dll> <log path> <marker> [timeout ms]
//
// 0  the marker appeared and the process is exiting normally
// 2  the loader DLL would not load
// 3  the marker never appeared within the timeout - which is also what a deadlock under the loader lock
//    looks like from here, and telling those apart is the check's job, not this program's

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string ReadAll(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    std::string out;
    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(file, buffer, static_cast<DWORD>(sizeof buffer), &read, nullptr) && read != 0) {
        out.append(buffer, read);
    }
    CloseHandle(file);
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::fwprintf(stderr, L"usage: probe_host <loader dll> <log path> <marker> [timeout ms]\n");
        return 2;
    }
    const wchar_t* loaderPath = argv[1];
    const wchar_t* logPath = argv[2];
    const std::wstring markerW = argv[3];
    // The marker is ASCII by construction (the check passes a literal), so a byte-wise narrowing is
    // right here - and saying so explicitly keeps the compiler from guessing a locale conversion.
    std::string marker;
    marker.reserve(markerW.size());
    for (const wchar_t c : markerW) {
        marker.push_back(static_cast<char>(c));
    }
    const DWORD timeoutMs = argc > 4 ? static_cast<DWORD>(_wtoi(argv[4])) : 20000;

    // This is the moment being reproduced: a LoadLibrary whose DllMain LoadLibrarys our plugin, so our
    // DLL_PROCESS_ATTACH runs under a loader lock this thread already holds.
    const HMODULE loader = LoadLibraryW(loaderPath);
    if (loader == nullptr) {
        std::fwprintf(stderr, L"probe_host: LoadLibraryW(%s) failed with %lu\n", loaderPath,
                      GetLastError());
        return 2;
    }
    std::wprintf(L"probe_host: loader mapped, waiting for '%s' in %s\n", markerW.c_str(), logPath);

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (ReadAll(logPath).find(marker) != std::string::npos) {
            std::wprintf(L"probe_host: marker seen; exiting\n");
            return 0;
        }
        Sleep(25);
    }
    std::fwprintf(stderr, L"probe_host: marker never appeared within %lu ms\n", timeoutMs);
    return 3;
}
