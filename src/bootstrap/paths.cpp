#include "bootstrap/paths.h"

#include <windows.h>

#include <stdio.h>

namespace replay {
namespace {

struct Resolved_ {
    bool    ok;
    bool    testRedirect;
    wchar_t self[kPathMax];
    wchar_t selfDir[kPathMax];
    wchar_t root[kPathMax];
    wchar_t log[kPathMax];
    wchar_t state[kPathMax];
    wchar_t config[kPathMax];
};

Resolved_ g_paths;

size_t Length(const wchar_t* s) {
    size_t n = 0;
    while (s[n] != L'\0') {
        ++n;
    }
    return n;
}

bool Copy(wchar_t* out, size_t cap, const wchar_t* s, size_t n) {
    if (n + 1 > cap) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = s[i];
    }
    out[n] = L'\0';
    return true;
}

bool Join(wchar_t* out, size_t cap, const wchar_t* dir, const wchar_t* leaf) {
    const size_t dn = Length(dir);
    const size_t ln = Length(leaf);
    if (dn + 1 + ln + 1 > cap) {
        return false;
    }
    Copy(out, cap, dir, dn);
    out[dn] = L'\\';
    Copy(out + dn + 1, cap - dn - 1, leaf, ln);
    return true;
}

bool IsSeparator(wchar_t c) {
    return c == L'\\' || c == L'/';
}

wchar_t Lower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// Index just past the last separator, or 0 when there is none.
size_t LeafStart(const wchar_t* path, size_t n) {
    size_t i = n;
    while (i > 0) {
        --i;
        if (IsSeparator(path[i])) {
            return i + 1;
        }
    }
    return 0;
}

bool LeafEqualsPlugins(const wchar_t* path, size_t start, size_t end) {
    const wchar_t kPlugins[] = L"plugins";
    const size_t n = end - start;
    if (n != Length(kPlugins)) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (Lower(path[start + i]) != kPlugins[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool DeriveInstallRoot(const wchar_t* modulePath, wchar_t* out, size_t cap) {
    if (modulePath == nullptr || out == nullptr || cap == 0) {
        return false;
    }
    const size_t n = Length(modulePath);
    const size_t leaf = LeafStart(modulePath, n);
    if (leaf == 0) {
        return false;  // no directory part: nothing to derive a root from
    }
    size_t dirEnd = leaf - 1;  // drop the separator

    const size_t parentLeaf = LeafStart(modulePath, dirEnd);
    if (parentLeaf > 0 && LeafEqualsPlugins(modulePath, parentLeaf, dirEnd)) {
        dirEnd = parentLeaf - 1;
    }
    if (dirEnd == 0) {
        return false;
    }
    return Copy(out, cap, modulePath, dirEnd);
}

bool IsUnderDirectory(const wchar_t* path, const wchar_t* root) {
    if (path == nullptr || root == nullptr) {
        return false;
    }
    const size_t rn = Length(root);
    if (rn == 0) {
        return false;
    }
    for (size_t i = 0; i < rn; ++i) {
        const wchar_t a = Lower(path[i]);
        const wchar_t b = Lower(root[i]);
        if (a == L'\0') {
            return false;
        }
        if (a != b && !(IsSeparator(a) && IsSeparator(b))) {
            return false;
        }
    }
    // Equal, or the next character starts a new component. Without this, C:\a\binary passes for C:\a\bin.
    return path[rn] == L'\0' || IsSeparator(path[rn]);
}

namespace {

bool Populate(const wchar_t* modulePath) {
    g_paths.ok = false;
    const size_t n = Length(modulePath);
    if (!Copy(g_paths.self, kPathMax, modulePath, n)) {
        return false;
    }
    const size_t leaf = LeafStart(modulePath, n);
    if (leaf == 0 || !Copy(g_paths.selfDir, kPathMax, modulePath, leaf - 1)) {
        return false;
    }
    if (!DeriveInstallRoot(modulePath, g_paths.root, kPathMax)) {
        return false;
    }
    if (!Join(g_paths.log, kPathMax, g_paths.root, L"echoreplay.log") ||
        !Join(g_paths.state, kPathMax, g_paths.root, L"echoreplay") ||
        !Join(g_paths.config, kPathMax, g_paths.state, L"config.json")) {
        return false;
    }
    g_paths.ok = true;
    return true;
}

}  // namespace

bool Resolve(void* selfModule) {
    wchar_t module[kPathMax];
    const DWORD written = GetModuleFileNameW(static_cast<HMODULE>(selfModule), module, kPathMax);
    // GetModuleFileNameW returns the buffer size on truncation and does not NUL-terminate reliably on
    // pre-Windows-10 - either way, a truncated path is a path to somewhere else.
    if (written == 0 || written >= kPathMax) {
        g_paths.ok = false;
        return false;
    }
    module[written] = L'\0';
    return Populate(module);
}

bool Resolved() {
    return g_paths.ok;
}

const wchar_t* SelfModulePath() { return g_paths.self; }
const wchar_t* SelfModuleDir()  { return g_paths.selfDir; }
const wchar_t* InstallRoot()    { return g_paths.root; }
const wchar_t* LogPath()        { return g_paths.log; }
const wchar_t* StateDir()       { return g_paths.state; }
const wchar_t* ConfigPath()     { return g_paths.config; }

bool IsWritablePath(const wchar_t* path) {
    if (!g_paths.ok || path == nullptr) {
        return false;
    }
    // The log is one exact file, not a directory: a prefix test would also admit anything "under" it.
    size_t i = 0;
    for (; g_paths.log[i] != L'\0'; ++i) {
        const wchar_t a = Lower(path[i]);
        const wchar_t b = Lower(g_paths.log[i]);
        if (a != b && !(IsSeparator(a) && IsSeparator(b))) {
            break;
        }
    }
    if (g_paths.log[i] == L'\0' && path[i] == L'\0') {
        return true;
    }
    return IsUnderDirectory(path, g_paths.state);
}

bool CaptureDirUnder(const wchar_t* root, wchar_t* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = L'\0';
    if (root == nullptr || root[0] == L'\0') {
        return false;
    }
    const int wrote = _snwprintf_s(out, cap, _TRUNCATE, L"%s\\EchoReplay", root);
    if (wrote < 0) {
        out[0] = L'\0';
        return false;
    }
    return true;
}

bool CaptureDirForProfile(const wchar_t* userProfile, wchar_t* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = L'\0';
    if (userProfile == nullptr || userProfile[0] == L'\0') {
        return false;
    }
    wchar_t documents[kPathMax];
    if (_snwprintf_s(documents, kPathMax, _TRUNCATE, L"%s\\Documents", userProfile) < 0) {
        return false;
    }
    return CaptureDirUnder(documents, out, cap);
}

bool ResolveCaptureDir(const char* configDir, wchar_t* out, size_t cap) {
    if (out == nullptr || cap == 0) {
        return false;
    }
    out[0] = L'\0';
    // * A TEST MUST NEVER ENUMERATE, LET ALONE WRITE INTO, THE OWNER'S REAL Documents\EchoReplay.
    // SetPathsForTest redirects every other write this plugin makes into a scratch tree and the
    // capture directory has to follow it; nothing in the shipping DLL calls that seam, so this arm
    // is unreachable in production and the profile path below is the only answer a launch gets.
    if (g_paths.testRedirect && (configDir == nullptr || configDir[0] == '\0')) {
        return CaptureDirUnder(g_paths.state, out, cap);
    }
    if (configDir != nullptr && configDir[0] != '\0') {
        const int wrote = ::MultiByteToWideChar(CP_UTF8, 0, configDir, -1, out,
                                                static_cast<int>(cap));
        if (wrote > 0) {
            return true;
        }
        out[0] = L'\0';
        return false;
    }
    wchar_t home[kPathMax] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", home, kPathMax);
    if (n == 0 || n >= kPathMax) {
        // No profile directory to put them in. `.\EchoReplay` is relative to a CWD this plugin does
        // not control (see the header), so it is a last resort and not a location.
        const int wrote = _snwprintf_s(out, cap, _TRUNCATE, L".\\EchoReplay");
        return wrote >= 0;
    }
    return CaptureDirForProfile(home, out, cap);
}

std::vector<std::wstring> ResolveImportDirs(const char* configValue) {
    std::vector<std::wstring> dirs;
    if (configValue == nullptr) {
        if (g_paths.testRedirect) {
            return dirs;
        }
        wchar_t home[kPathMax] = {};
        const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", home, kPathMax);
        if (n != 0 && n < kPathMax) {
            dirs.push_back(std::wstring(home) + L"\\Documents\\Spark\\replays");
        }
        return dirs;
    }
    const int wideChars = ::MultiByteToWideChar(CP_UTF8, 0, configValue, -1, nullptr, 0);
    if (wideChars <= 1) {
        return dirs;
    }
    std::wstring list(static_cast<size_t>(wideChars), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, configValue, -1, &list[0], wideChars);
    list.resize(static_cast<size_t>(wideChars) - 1);

    size_t begin = 0;
    while (begin <= list.size()) {
        size_t end = list.find(L';', begin);
        if (end == std::wstring::npos) {
            end = list.size();
        }
        size_t first = begin;
        size_t last = end;
        while (first < last && (list[first] == L' ' || list[first] == L'\t')) {
            ++first;
        }
        while (last > first && (list[last - 1] == L' ' || list[last - 1] == L'\t')) {
            --last;
        }
        if (last > first && last - first < kPathMax) {
            dirs.push_back(list.substr(first, last - first));
        }
        begin = end + 1;
    }
    return dirs;
}

void SetPathsForTest(const wchar_t* modulePath) {
    g_paths.testRedirect = true;
    Populate(modulePath);
}

}  // namespace replay
