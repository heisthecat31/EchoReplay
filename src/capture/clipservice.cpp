// The clip service: "save the last N seconds" for other plugins in the same process (EDITH-HUD's
// "Edith, clip the last 3 minutes").
//
//   extern "C" int  EchoReplay_Clip(uint32_t seconds);                       -> job id (>0), or 0
//   extern "C" int  EchoReplay_ClipStatus(int job, wchar_t* out, uint32_t cap);
//                       0 still cutting, 1 done (out = the clip's .json playlist), -1 failed (out = why)
//
// The recorder writes one .echodemo per match into Documents\EchoReplay. A window can reach back past
// the start of the current match, so the cut walks the files newest first: the one being written now
// counts as ending "now", a closed file as ending at its last write time. Each file contributes the
// part of the window it covers, and the parts are listed oldest first in clips\<stamp>.json:
//
//   { "version": 1, "seconds": 180, "created": "2026-11-14 21:03:11",
//     "parts": ["2026-11-14_21-03-11_180s_p1.echodemo", "..._p2.echodemo"] }
//
// Nothing here runs on the game thread: the cut opens files and decompresses chapters.

#include <windows.h>

#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "capture/capturestop.h"
#include "capture/clipcut.h"

namespace {

constexpr size_t kPath = 1024;
constexpr uint32_t kMaxSeconds = 30 * 60;

struct Job {
    int id = 0;
    int state = 0;  // 0 running, 1 ok, -1 failed
    std::wstring result;
};

std::mutex g_lock;
std::vector<Job> g_jobs;
int g_next = 1;

void Finish(int id, int state, const std::wstring& text) {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto& j : g_jobs) {
        if (j.id == id) {
            j.state = state;
            j.result = text;
        }
    }
}

uint64_t FileTimeUs(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10;  // 100 ns -> us
}

std::wstring Widen(const char* s) {
    if (!s) return L"";
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 512);
    return w;
}

struct Candidate {
    std::wstring path;
    std::wstring name;
    uint64_t endWallUs;  // wall clock at which the file's content ends
};

void RunJob(int id, uint32_t seconds) {
    wchar_t dir[kPath] = {}, cur[kPath] = {};
    replay::capture::CaptureRecordingPaths(dir, kPath, cur, kPath);
    if (!dir[0]) {
        Finish(id, -1, L"no recordings folder");
        return;
    }
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const uint64_t nowUs = FileTimeUs(nowFt);
    const uint64_t spanUs = uint64_t(seconds) * 1000000ull;

    std::vector<Candidate> files;
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = std::wstring(dir) + L"\\*.echodemo";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Candidate c;
            c.name = fd.cFileName;
            c.path = std::wstring(dir) + L"\\" + fd.cFileName;
            const bool live = cur[0] && _wcsicmp(c.path.c_str(), cur) == 0;
            c.endWallUs = live ? nowUs : FileTimeUs(fd.ftLastWriteTime);
            if (c.endWallUs + spanUs < nowUs) continue;  // ended before the window
            files.push_back(std::move(c));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // Names start with the local time the file opened, so name order is time order.
    std::sort(files.begin(), files.end(), [](const Candidate& a, const Candidate& b) { return a.name > b.name; });
    if (files.empty()) {
        Finish(id, -1, L"nothing was recorded in that window (recording runs in live matches only)");
        return;
    }

    const std::wstring clipDir = std::wstring(dir) + L"\\clips";
    CreateDirectoryW(clipDir.c_str(), nullptr);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t stamp[64];
    _snwprintf_s(stamp, 64, _TRUNCATE, L"%04u-%02u-%02u_%02u-%02u-%02u_%us", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond, seconds);

    std::vector<std::wstring> parts;  // newest first while cutting
    std::wstring firstError;
    int part = 0;
    for (const Candidate& c : files) {
        const uint64_t age = nowUs > c.endWallUs ? nowUs - c.endWallUs : 0;
        if (age >= spanUs) break;
        const uint64_t want = spanUs - age;
        const uint64_t end = replay::capture::RecordingEndUs(c.path.c_str());
        if (end == 0) continue;
        const uint64_t from = end > want ? end - want : 0;
        wchar_t out[kPath];
        _snwprintf_s(out, kPath, _TRUNCATE, L"%s\\%s_p%d.echodemo", clipDir.c_str(), stamp, ++part);
        const replay::capture::ClipResult r = replay::capture::ClipRange(c.path.c_str(), out, from, 0);
        if (r.ok) {
            const wchar_t* slash = wcsrchr(out, L'\\');
            parts.push_back(slash ? slash + 1 : out);
        } else if (firstError.empty()) {
            firstError = c.name + L": " + Widen(r.error);
            DeleteFileW(out);
        }
    }
    if (parts.empty()) {
        Finish(id, -1, firstError.empty() ? L"nothing could be cut" : firstError);
        return;
    }
    std::reverse(parts.begin(), parts.end());  // oldest first: the order to play them in
    // Parts were numbered newest first; rename them so p1 is the oldest.
    std::vector<std::wstring> named;
    for (size_t i = 0; i < parts.size(); ++i) {
        wchar_t tmp[kPath], fin[kPath];
        _snwprintf_s(tmp, kPath, _TRUNCATE, L"%s\\%s", clipDir.c_str(), parts[i].c_str());
        _snwprintf_s(fin, kPath, _TRUNCATE, L"%s\\%s_p%zu.echodemo.tmp", clipDir.c_str(), stamp, i + 1);
        MoveFileExW(tmp, fin, MOVEFILE_REPLACE_EXISTING);
        named.push_back(fin);
    }
    std::vector<std::wstring> finalNames;
    for (size_t i = 0; i < named.size(); ++i) {
        std::wstring fin = named[i].substr(0, named[i].size() - 4);  // drop ".tmp"
        MoveFileExW(named[i].c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING);
        const size_t slash = fin.find_last_of(L'\\');
        finalNames.push_back(fin.substr(slash + 1));
    }

    wchar_t jsonPath[kPath];
    _snwprintf_s(jsonPath, kPath, _TRUNCATE, L"%s\\%s.json", clipDir.c_str(), stamp);
    FILE* f = nullptr;
    if (_wfopen_s(&f, jsonPath, L"wb") != 0 || !f) {
        Finish(id, -1, L"cannot write the clip list");
        return;
    }
    fprintf(f, "{\n  \"version\": 1,\n  \"seconds\": %u,\n  \"created\": \"%04u-%02u-%02u %02u:%02u:%02u\",\n  \"parts\": [",
            seconds, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    for (size_t i = 0; i < finalNames.size(); ++i) {
        char n[512];
        WideCharToMultiByte(CP_UTF8, 0, finalNames[i].c_str(), -1, n, sizeof n, nullptr, nullptr);
        fprintf(f, "%s\n    \"%s\"", i ? "," : "", n);
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    Finish(id, 1, jsonPath);
}

DWORD WINAPI JobThread(LPVOID p) {
    const uint64_t packed = reinterpret_cast<uint64_t>(p);
    RunJob(static_cast<int>(packed >> 32), static_cast<uint32_t>(packed & 0xffffffffu));
    return 0;
}

}  // namespace

extern "C" __declspec(dllexport) int EchoReplay_Clip(uint32_t seconds) {
    if (seconds == 0) return 0;
    if (seconds > kMaxSeconds) seconds = kMaxSeconds;
    int id;
    {
        std::lock_guard<std::mutex> l(g_lock);
        id = g_next++;
        Job j;
        j.id = id;
        g_jobs.push_back(j);
        if (g_jobs.size() > 32) g_jobs.erase(g_jobs.begin());
    }
    const uint64_t packed = (uint64_t(uint32_t(id)) << 32) | seconds;
    HANDLE th = CreateThread(nullptr, 0, JobThread, reinterpret_cast<LPVOID>(packed), 0, nullptr);
    if (!th) {
        Finish(id, -1, L"could not start the clip thread");
        return id;
    }
    CloseHandle(th);
    return id;
}

extern "C" __declspec(dllexport) int EchoReplay_ClipStatus(int job, wchar_t* out, uint32_t cap) {
    std::lock_guard<std::mutex> l(g_lock);
    for (const auto& j : g_jobs) {
        if (j.id != job) continue;
        if (out && cap) wcsncpy_s(out, cap, j.result.c_str(), _TRUNCATE);
        return j.state;
    }
    if (out && cap) wcsncpy_s(out, cap, L"unknown job", _TRUNCATE);
    return -1;
}
