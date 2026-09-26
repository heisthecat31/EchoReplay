#include "bootstrap/coexist.h"

#include <cstdio>

#include <windows.h>

#include "bootstrap/log.h"
#include "bootstrap/md5.h"
#include "bootstrap/paths.h"

namespace replay {
namespace coexist {
namespace {

HANDLE g_instanceMutex;

wchar_t LowerW(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool FileNameIs(const wchar_t* path, const wchar_t* name) {
    size_t n = 0;
    while (path[n] != L'\0') {
        ++n;
    }
    size_t leaf = 0;
    for (size_t i = n; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
            leaf = i;
            break;
        }
    }
    size_t i = 0;
    for (; name[i] != L'\0'; ++i) {
        if (LowerW(path[leaf + i]) != LowerW(name[i])) {
            return false;
        }
    }
    return path[leaf + i] == L'\0';
}

}  // namespace

bool IsModuleLoaded(const char* moduleName) {
    // GetModuleHandleA, never LoadLibrary: asking whether something is present must not make it present,
    // and this is called from the worker thread while the process is running.
    return GetModuleHandleA(moduleName) != nullptr;
}

bool ClaimInstance(Report* report) {
    *report = Report{};
    if (!Resolved()) {
        return false;
    }

    // Rule 2. dbgcore.dll is the loader's own slot; a copy of us under that name would evict whatever
    // loader is installed and take every other plugin with it.
    if (FileNameIs(SelfModulePath(), L"dbgcore.dll")) {
        report->ourNameIsTheLoaderSlot = true;
        log::Direct(LogLevel::Error, "coexist",
                    "this DLL has been renamed to dbgcore.dll, which is the loader's own slot. Taking it "
                    "would evict the loader and every other plugin. Refusing; rename it back.");
        return false;
    }

    // Two copies under different names both get loaded - the loader takes every *.dll in the folder, so
    // echoreplay.dll beside "echoreplay - Copy.dll" gives two workers, two provider-slot claims and two
    // writers on one log file. That is a realistic user mistake and this is the check for it.
    //
    // Keyed on the resolved LOG PATH and on OUR OWN PROCESS ID.
    //
    // The log path, because that is the resource two copies of this plugin actually collide over; keying
    // on our own file name would miss the renamed-copy case, which is the only case there is.
    //
    // ★ The process id, because the hazard is process-scoped and the previous session-scoped name was
    // not. Two CNSProvider slot claims can only collide inside one process. Across processes the only
    // shared thing is the log file, which is opened FILE_APPEND_DATA with full sharing, so appends are
    // atomic and nothing is corrupted. Without the pid, a second Echo instance from the same install got
    // a silently inert plugin - and the sweep, which launches exactly that way, paid a launch for it.
    wchar_t mutexName[kPathMax];
    {
        // Hashed as UTF-16 rather than truncated to char: two install paths differing only in a
        // non-ASCII character with the same low byte would otherwise hash the same and the second,
        // genuinely different, product would be refused.
        wchar_t lowered[kPathMax];
        size_t n = 0;
        for (const wchar_t* p = LogPath(); *p != L'\0' && n + 1 < kPathMax; ++p, ++n) {
            lowered[n] = LowerW(*p);
        }
        lowered[n] = L'\0';
        Md5 md5;
        md5.Update(lowered, n * sizeof(wchar_t));
        uint8_t digest[16];
        md5.Final(digest);
        char hex[kMd5HexChars];
        Md5::ToHex(digest, hex);
        _snwprintf_s(mutexName, kPathMax, _TRUNCATE, L"Local\\echoreplay.instance.%lu.%hs",
                     GetCurrentProcessId(), hex);
    }
    g_instanceMutex = CreateMutexW(nullptr, TRUE, mutexName);
    if (g_instanceMutex == nullptr) {
        return true;   // cannot tell; proceeding is the lesser risk, and the log says nothing was claimed
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        report->duplicateInstance = true;
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        log::Direct(LogLevel::Error, "coexist",
                    "another module IN THIS PROCESS (pid %lu) has already claimed %ls; we are %ls. That "
                    "is a second copy of this plugin in one plugins folder - refusing, so the two do not "
                    "both claim a provider slot. A second echovr.exe is unaffected.",
                    GetCurrentProcessId(), LogPath(), SelfModulePath());
        return false;
    }
    return true;
}

void Survey(Report* report) {
    report->rlayLoaded = IsModuleLoaded("rlay.dll");

    wchar_t pattern[kPathMax];
    size_t n = 0;
    const wchar_t* dir = SelfModuleDir();
    while (dir[n] != L'\0' && n + 8 < kPathMax) {
        pattern[n] = dir[n];
        ++n;
    }
    const wchar_t suffix[] = L"\\*.dll";
    for (size_t i = 0; i < sizeof suffix / sizeof suffix[0]; ++i) {
        pattern[n + i] = suffix[i];
    }

    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        ++report->pluginDllCount;
        if (FileNameIs(SelfModulePath(), found.cFileName)) {
            continue;
        }
        ++report->otherPluginDlls;
        log::Direct(LogLevel::Info, "coexist", "sharing this folder with %ls%ls", found.cFileName,
                    GetModuleHandleW(found.cFileName) != nullptr ? L" (loaded)" : L" (not loaded)");
    } while (FindNextFileW(search, &found));
    FindClose(search);

    // Load order is FindFirstFileW order and nothing in this plugin may depend on it. Recording the
    // observation is the point: V-I11 replays this log with rlay.dll present and absent.
    log::Direct(LogLevel::Info, "coexist",
                "%u DLL(s) in this folder, %u besides us; rlay.dll is %s. No code path here depends on "
                "load order.", report->pluginDllCount, report->otherPluginDlls,
                report->rlayLoaded ? "loaded" : "absent");
}

void ReleaseForTest() {
    if (g_instanceMutex != nullptr) {
        ReleaseMutex(g_instanceMutex);
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
}

}  // namespace coexist
}  // namespace replay
