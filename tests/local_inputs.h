// Inputs some tests read from this PC rather than from the repository: the retail echovr.exe, the
// user's own captures, private corpora. A test that needs one skips, never fails, when it is absent.
//
// echovr.exe: REPLAY_ECHOVR_EXE (the file), then ECHOVR_DIR (the install folder, or its bin\win10),
// then the Oculus libraries registered on this PC. A variable that is set but points at nothing means
// "absent" and does not fall through, so a machine that has the game can still exercise the skip.
#pragma once

#include <windows.h>

#include <stdlib.h>

#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace replay_test {

inline bool IsFileW(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool EnvW(const wchar_t* name, std::wstring& out) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) {
        return false;
    }
    out.assign(buf, n);
    return true;
}

inline std::wstring JoinW(std::wstring dir, const wchar_t* tail) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
        dir.pop_back();
    }
    return dir + L"\\" + tail;
}

inline std::wstring ExeInGameDir(const std::wstring& dir) {
    for (const wchar_t* tail : {L"bin\\win10\\echovr.exe", L"echovr.exe"}) {
        const std::wstring exe = JoinW(dir, tail);
        if (IsFileW(exe)) {
            return exe;
        }
    }
    return {};
}

inline bool RegStringW(HKEY root, const wchar_t* subkey, const wchar_t* value, std::wstring& out) {
    wchar_t buf[MAX_PATH * 2];
    DWORD bytes = sizeof buf;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) {
        return false;
    }
    out = buf;
    return !out.empty();
}

// Each Oculus library holds its apps under <library>\Software\<app>.
inline std::vector<std::wstring> OculusLibraries() {
    std::vector<std::wstring> libraries;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Oculus VR, LLC\\Oculus\\Libraries", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD nameLen = sizeof name / sizeof name[0];
            if (RegEnumKeyExW(key, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            std::wstring path;
            if (RegStringW(key, name, L"OriginalPath", path) || RegStringW(key, name, L"Path", path)) {
                libraries.push_back(path);
            }
        }
        RegCloseKey(key);
    }
    std::wstring base;
    if (RegStringW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Oculus VR, LLC\\Oculus", L"Base", base)) {
        libraries.push_back(JoinW(base, L"Software"));
    }
    std::wstring programFiles;
    if (EnvW(L"ProgramFiles", programFiles)) {
        libraries.push_back(JoinW(programFiles, L"Oculus\\Software"));
    }
    return libraries;
}

inline std::wstring FindEchoVrExeW() {
    std::wstring value;
    if (EnvW(L"REPLAY_ECHOVR_EXE", value)) {
        return IsFileW(value) ? value : std::wstring();
    }
    if (EnvW(L"ECHOVR_DIR", value)) {
        return ExeInGameDir(value);
    }
    for (const std::wstring& library : OculusLibraries()) {
        const std::wstring exe = ExeInGameDir(JoinW(library, L"Software\\ready-at-dawn-echo-arena"));
        if (!exe.empty()) {
            return exe;
        }
    }
    return {};
}

inline std::string FindEchoVrExeA() {
    const std::wstring wide = FindEchoVrExeW();
    if (wide.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string narrow(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, &narrow[0], n, nullptr, nullptr);
    narrow.resize(static_cast<size_t>(n) - 1);
    return narrow;
}

// Optional local inputs (a private capture corpus and the like) come from an environment variable
// only; "" when it is unset.
inline std::string OptionalInputA(const char* envName) {
    char buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableA(envName, buf, sizeof buf);
    return (n > 0 && n < sizeof buf) ? std::string(buf, n) : std::string();
}

// Where the plugin records by default (src/bootstrap/paths.cpp, ResolveCaptureDir):
// %USERPROFILE%\Documents\EchoReplay. REPLAY_CAPTURE_DIR overrides it.
inline std::wstring CaptureDirW() {
    std::wstring dir;
    if (EnvW(L"REPLAY_CAPTURE_DIR", dir)) {
        return dir;
    }
    std::wstring profile;
    return EnvW(L"USERPROFILE", profile) ? JoinW(profile, L"Documents\\EchoReplay") : std::wstring();
}

// A private capture that is absent skips its case. ECHOREPLAY_TEST_REQUIRE_CAPTURE=1 makes absence a
// failure instead, for a machine that is supposed to have them.
constexpr char kRequireCaptureEnv[] = "ECHOREPLAY_TEST_REQUIRE_CAPTURE";

inline bool MissingCaptureFails() {
    char value[8] = {};
    size_t n = 0;
    return getenv_s(&n, value, sizeof value, kRequireCaptureEnv) == 0 && n == 2 && value[0] == '1';
}

// A named capture in CaptureDirW(); "" when there is no such folder to look in.
inline std::wstring CaptureFileW(const wchar_t* name) {
    const std::wstring dir = CaptureDirW();
    return dir.empty() ? std::wstring() : JoinW(dir, name);
}

}  // namespace replay_test
