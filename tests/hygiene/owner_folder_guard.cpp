// Runs a test suite and fails if the run left anything new in the owner's real capture folder.
//
// The folder is resolved with replay::ResolveCaptureDir(nullptr, ...) -- this process never calls
// SetPathsForTest, so that is the production rule and not a copy of it. Nothing here writes: it
// lists the folder, runs the child, lists it again, and compares. A name that is new, or an old
// name whose size changed, is a stray and the run is RED.
//
// Exit: 0 clean, 1 strays found, 2 refused (could not resolve, could not list, could not run the
// child). A guard that cannot ask its question must say so rather than report a clean folder.

#include <windows.h>

#include <stdint.h>
#include <stdio.h>

#include <map>
#include <string>

#include "bootstrap/paths.h"

namespace {

using Snapshot = std::map<std::wstring, uint64_t>;

enum class ListResult { Ok, Absent, Failed };

ListResult List(const wchar_t* dir, Snapshot& out) {
    const std::wstring pattern = std::wstring(dir) + L"\\*";
    WIN32_FIND_DATAW fd = {};
    const HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return ListResult::Absent;
        }
        ::fwprintf(stdout, L"GUARD list-error=%lu\n", static_cast<unsigned long>(err));
        return ListResult::Failed;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const uint64_t size =
            (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out[fd.cFileName] = size;
    } while (::FindNextFileW(h, &fd) != FALSE);
    ::FindClose(h);
    return ListResult::Ok;
}

// argv[first..] as one command line, quoting the arguments that carry spaces. The child is a test
// binary we build ourselves, so this stays deliberately simple rather than implementing the full
// CommandLineToArgvW escape.
std::wstring BuildCommandLine(int argc, wchar_t** argv, int first) {
    std::wstring cmd;
    for (int i = first; i < argc; ++i) {
        if (!cmd.empty()) {
            cmd += L' ';
        }
        const bool quote = wcschr(argv[i], L' ') != nullptr && argv[i][0] != L'"';
        if (quote) {
            cmd += L'"';
        }
        cmd += argv[i];
        if (quote) {
            cmd += L'"';
        }
    }
    return cmd;
}

bool RunChild(std::wstring& cmd, DWORD& exitCode) {
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                         &pi) == FALSE) {
        ::fwprintf(stdout, L"GUARD spawn-error=%lu\n", static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        ::fwprintf(stdout, L"GUARD VERDICT=REFUSED usage: owner_folder_guard <exe> [args...]\n");
        return 2;
    }

    wchar_t dir[replay::kPathMax] = {};
    if (!replay::ResolveCaptureDir(nullptr, dir, replay::kPathMax) || dir[0] == L'\0') {
        ::fwprintf(stdout, L"GUARD VERDICT=REFUSED the capture directory could not be resolved\n");
        return 2;
    }
    ::fwprintf(stdout, L"GUARD dir=%s\n", dir);

    Snapshot before;
    if (List(dir, before) == ListResult::Failed) {
        ::fwprintf(stdout, L"GUARD VERDICT=REFUSED the folder could not be listed before the run\n");
        return 2;
    }

    std::wstring cmd = BuildCommandLine(argc, argv, 1);
    ::fwprintf(stdout, L"GUARD child=%s\n", cmd.c_str());
    ::fflush(stdout);
    DWORD childExit = 0;
    if (!RunChild(cmd, childExit)) {
        ::fwprintf(stdout, L"GUARD VERDICT=REFUSED the child could not be started\n");
        return 2;
    }
    ::fwprintf(stdout, L"GUARD child-exit=%lu\n", static_cast<unsigned long>(childExit));

    Snapshot after;
    if (List(dir, after) == ListResult::Failed) {
        ::fwprintf(stdout, L"GUARD VERDICT=REFUSED the folder could not be listed after the run\n");
        return 2;
    }

    int strays = 0;
    for (const auto& entry : after) {
        const auto was = before.find(entry.first);
        if (was == before.end()) {
            ::fwprintf(stdout, L"GUARD NEW %s %llu\n", entry.first.c_str(),
                       static_cast<unsigned long long>(entry.second));
            ++strays;
        } else if (was->second != entry.second) {
            ::fwprintf(stdout, L"GUARD CHANGED %s %llu -> %llu\n", entry.first.c_str(),
                       static_cast<unsigned long long>(was->second),
                       static_cast<unsigned long long>(entry.second));
            ++strays;
        }
    }

    ::fwprintf(stdout, L"GUARD before=%zu after=%zu strays=%d\n", before.size(), after.size(),
               strays);
    ::fwprintf(stdout, L"GUARD VERDICT=%s\n", strays == 0 ? L"CLEAN" : L"STRAYS");
    return strays == 0 ? 0 : 1;
}
