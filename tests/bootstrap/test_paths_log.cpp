// The two properties that decide whether this plugin is diagnosable and uninstallable:
//   - every path comes from our own module handle, never from the CWD (the loader scans CWD\plugins);
//   - nothing is written outside the two paths INSTALL.md lists, so uninstall really is one file.
// Plus the log ring, whose contract is that it never blocks the tick thread and never lies about loss.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <doctest.h>
#include <windows.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/log.h"
#include "bootstrap/paths.h"
#include "bootstrap/tickcost.h"
#include "generated/facts_generated.h"

namespace {

std::wstring Root(const wchar_t* modulePath) {
    wchar_t out[replay::kPathMax] = {};
    if (!replay::DeriveInstallRoot(modulePath, out, replay::kPathMax)) {
        return L"<refused>";
    }
    return out;
}

std::wstring TempFile(const wchar_t* prefix) {
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, prefix, 0, path);
    return path;
}

}  // namespace

TEST_CASE("the install root is the plugins folder's parent, and only when it really is plugins") {
    CHECK(Root(L"C:\\game\\bin\\win10\\plugins\\echoreplay.dll") == L"C:\\game\\bin\\win10");
    // Case-insensitive, because Windows is and the folder is not ours to name.
    CHECK(Root(L"C:\\game\\bin\\win10\\PLUGINS\\echoreplay.dll") == L"C:\\game\\bin\\win10");
    // Not inside a plugins folder: keep our own directory rather than reaching into a parent that has
    // nothing to do with us. A test binary and the loader probe both land here.
    CHECK(Root(L"D:\\build\\bin\\echoreplay.dll") == L"D:\\build\\bin");
    // A directory that merely starts with "plugins" is not the plugins directory.
    CHECK(Root(L"C:\\game\\bin\\plugins_old\\echoreplay.dll") == L"C:\\game\\bin\\plugins_old");
    CHECK(Root(L"echoreplay.dll") == L"<refused>");
    CHECK(Root(L"") == L"<refused>");
}

TEST_CASE("containment compares whole path components") {
    CHECK(replay::IsUnderDirectory(L"C:\\a\\bin\\x.log", L"C:\\a\\bin"));
    CHECK(replay::IsUnderDirectory(L"C:\\a\\bin", L"C:\\a\\bin"));
    CHECK(replay::IsUnderDirectory(L"c:\\A\\BIN\\x", L"C:\\a\\bin"));
    // The trap a naive prefix test falls into: a sibling directory whose name extends the root's.
    CHECK_FALSE(replay::IsUnderDirectory(L"C:\\a\\binary\\x.log", L"C:\\a\\bin"));
    CHECK_FALSE(replay::IsUnderDirectory(L"C:\\a", L"C:\\a\\bin"));
    CHECK_FALSE(replay::IsUnderDirectory(L"C:\\b\\bin\\x", L"C:\\a\\bin"));
    CHECK_FALSE(replay::IsUnderDirectory(nullptr, L"C:\\a"));
    CHECK_FALSE(replay::IsUnderDirectory(L"C:\\a", L""));
}

TEST_CASE("the write policy admits exactly the log and the state directory") {
    replay::SetPathsForTest(L"C:\\game\\bin\\win10\\plugins\\echoreplay.dll");
    REQUIRE(replay::Resolved());
    CHECK(std::wstring(replay::LogPath()) == L"C:\\game\\bin\\win10\\echoreplay.log");
    CHECK(std::wstring(replay::StateDir()) == L"C:\\game\\bin\\win10\\echoreplay");
    CHECK(std::wstring(replay::ConfigPath()) == L"C:\\game\\bin\\win10\\echoreplay\\config.json");

    CHECK(replay::IsWritablePath(L"C:\\game\\bin\\win10\\echoreplay.log"));
    CHECK(replay::IsWritablePath(L"C:\\game\\bin\\win10\\echoreplay"));
    CHECK(replay::IsWritablePath(L"C:\\game\\bin\\win10\\echoreplay\\config.json"));
    CHECK(replay::IsWritablePath(L"C:\\game\\bin\\win10\\echoreplay\\sub\\thing.bin"));

    // Everything else, and these are the ones that matter: the install tree, the plugins folder, the
    // game's own data, and anywhere at all outside the install.
    CHECK_FALSE(replay::IsWritablePath(L"C:\\game\\bin\\win10\\echovr.exe"));
    CHECK_FALSE(replay::IsWritablePath(L"C:\\game\\bin\\win10\\plugins\\rlay.dll"));
    CHECK_FALSE(replay::IsWritablePath(L"C:\\game\\bin\\win10\\sourcedb\\x"));
    CHECK_FALSE(replay::IsWritablePath(L"C:\\game\\bin\\win10\\echoreplay.log.bak"));
    CHECK_FALSE(replay::IsWritablePath(L"C:\\Windows\\System32\\drivers\\etc\\hosts"));
    CHECK_FALSE(replay::IsWritablePath(nullptr));
}

TEST_CASE("the log ring carries a line through unchanged and truncates rather than growing") {
    auto ring = std::make_unique<replay::LogRing>();
    const std::string text = "hello from the tick thread";
    ring->Push(replay::LogLevel::Warn, "unit", text.data(), text.size(), 1234);

    replay::LogLevel level = replay::LogLevel::Trace;
    const char* module = nullptr;
    char out[replay::kLogLineMax];
    size_t len = 0;
    uint64_t tickMs = 0;
    REQUIRE(ring->Pop(&level, &module, out, sizeof out, &len, &tickMs));
    CHECK(level == replay::LogLevel::Warn);
    CHECK(std::string(module) == "unit");
    CHECK(std::string(out, len) == text);
    CHECK(tickMs == 1234);
    CHECK_FALSE(ring->Pop(&level, &module, out, sizeof out, &len, &tickMs));

    const std::string oversized(replay::kLogLineMax * 3, 'x');
    ring->Push(replay::LogLevel::Info, "unit", oversized.data(), oversized.size(), 1);
    REQUIRE(ring->Pop(&level, &module, out, sizeof out, &len, &tickMs));
    CHECK(len == replay::kLogLineMax);
    CHECK(ring->Dropped() == 0);
}

TEST_CASE("the log ring counts what it loses instead of blocking or lying") {
    auto ring = std::make_unique<replay::LogRing>();
    // Far more than the ring holds, with nobody draining: the contract is that Push never blocks, so the
    // only honest outcome is to overwrite and say how much was overwritten.
    const int pushes = 5000;
    for (int i = 0; i < pushes; ++i) {
        char line[32];
        const int n = std::snprintf(line, sizeof line, "line %d", i);
        ring->Push(replay::LogLevel::Info, "unit", line, static_cast<size_t>(n), static_cast<uint64_t>(i));
    }
    replay::LogLevel level = replay::LogLevel::Trace;
    const char* module = nullptr;
    char out[replay::kLogLineMax];
    size_t len = 0;
    uint64_t tickMs = 0;

    int recovered = 0;
    uint64_t firstTick = 0;
    while (ring->Pop(&level, &module, out, sizeof out, &len, &tickMs)) {
        if (recovered == 0) {
            firstTick = tickMs;
        }
        ++recovered;
    }
    // Every pushed record is either recovered or counted as dropped. Neither number is allowed to be a
    // guess: recovered + dropped has to be exactly what went in.
    CHECK(static_cast<uint64_t>(recovered) + ring->Dropped() == static_cast<uint64_t>(pushes));
    CHECK(recovered > 0);
    CHECK(ring->Dropped() > 0);
    // What survives is the newest, not the oldest: a log that drops the last thing before a crash is
    // worse than useless.
    CHECK(firstTick == static_cast<uint64_t>(pushes - recovered));
    CHECK(tickMs == static_cast<uint64_t>(pushes - 1));
}

TEST_CASE("the direct log path writes a readable line to the file it was pointed at") {
    const std::wstring path = TempFile(L"w1l");
    DeleteFileW(path.c_str());
    replay::log::RedirectForTest(path.c_str());
    replay::log::SetLevel(replay::LogLevel::Info);

    replay::log::Direct(replay::LogLevel::Error, "unit", "refused because %s (%d)", "the md5 differs", 7);
    replay::log::Interface()->Write(replay::LogLevel::Info, "unit", "through the ring");
    replay::log::Drain();

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    char buffer[4096] = {};
    DWORD read = 0;
    ReadFile(file, buffer, sizeof buffer - 1, &read, nullptr);
    CloseHandle(file);
    const std::string body(buffer, read);

    CHECK(body.find("refused because the md5 differs (7)") != std::string::npos);
    CHECK(body.find("ERROR [unit]") != std::string::npos);
    CHECK(body.find("through the ring") != std::string::npos);
    CHECK(body.find("\r\n") != std::string::npos);

    // Below the level, so it must not appear. A level control that logs everything anyway is not one.
    replay::log::SetLevel(replay::LogLevel::Error);
    CHECK_FALSE(replay::log::Interface()->Enabled(replay::LogLevel::Info));
    CHECK(replay::log::Interface()->Enabled(replay::LogLevel::Error));
    replay::log::SetLevel(replay::LogLevel::Info);

    replay::log::EndRedirectForTest();
    DeleteFileW(path.c_str());
}

TEST_CASE("the drain interval is clamped at both ends") {
    // Both ends are harmful and both are one keystroke away in a hand-edited file: 0 spins a core for the
    // process lifetime inside a VR title, and a negative value casts to INFINITE so the ring is never
    // drained and every module's log line is invisible until exit.
    CHECK(replay::bootstrap::ClampDrainIntervalMs(250) == 250);
    CHECK(replay::bootstrap::ClampDrainIntervalMs(10) == 10);
    CHECK(replay::bootstrap::ClampDrainIntervalMs(60000) == 60000);

    CHECK(replay::bootstrap::ClampDrainIntervalMs(0) >= 10);
    CHECK(replay::bootstrap::ClampDrainIntervalMs(9) == 10);
    CHECK(replay::bootstrap::ClampDrainIntervalMs(-1) == 10);
    CHECK(replay::bootstrap::ClampDrainIntervalMs(-9007199254740993ll) == 10);
    CHECK(replay::bootstrap::ClampDrainIntervalMs(60001) == 60000);
    CHECK(replay::bootstrap::ClampDrainIntervalMs(9007199254740993ll) == 60000);

    // The cast the clamp exists to make safe: INFINITE would mean the ring is never drained at all.
    CHECK(static_cast<unsigned long>(replay::bootstrap::ClampDrainIntervalMs(-1)) != 0xFFFFFFFFul);
}

TEST_CASE("log.level silences the direct path too, but never the attach line") {
    const std::wstring path = TempFile(L"w1v");
    DeleteFileW(path.c_str());
    replay::log::RedirectForTest(path.c_str());
    replay::log::SetLevel(replay::LogLevel::Error);

    replay::log::Direct(replay::LogLevel::Info, "unit", "routine line that should be silenced");
    replay::log::Direct(replay::LogLevel::Error, "unit", "a refusal, which is never silenced");
    // The attach line is the only evidence this DLL ran at all, so no setting may suppress it.
    replay::log::DirectAlways(replay::LogLevel::Info, "unit", "attach-equivalent line");

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    char buffer[4096] = {};
    DWORD read = 0;
    ReadFile(file, buffer, sizeof buffer - 1, &read, nullptr);
    CloseHandle(file);
    const std::string body(buffer, read);

    CHECK(body.find("routine line that should be silenced") == std::string::npos);
    CHECK(body.find("a refusal, which is never silenced") != std::string::npos);
    CHECK(body.find("attach-equivalent line") != std::string::npos);

    replay::log::SetLevel(replay::LogLevel::Info);
    replay::log::EndRedirectForTest();
    DeleteFileW(path.c_str());
}

// `src/bootstrap/paths.cpp` shipped
// `L"%s' + chr(92) + chr(92) + 'EchoReplay"` -- a generator's own Python source pasted into the wide
// format string -- from bdc38c6 to 258c0e8. Every character of it is legal in a Windows filename, so
// the call returned true and the redirect arm silently named a SIBLING of the state directory. The
// suite reddened when the arm was deleted and did not move when the literal was repaired, because
// nothing here asserted the arm's VALUE. These cases assert the value.
TEST_CASE("the capture directory rule is one string, pinned byte for byte") {
    wchar_t out[replay::kPathMax] = {};

    REQUIRE(replay::CaptureDirUnder(L"C:\\Users\\known\\Documents", out, replay::kPathMax));
    CHECK(std::wstring(out) == L"C:\\Users\\known\\Documents\\EchoReplay");

    // The resolved production path for a known USERPROFILE. Drop one backslash anywhere in either
    // function and this is the case that reddens.
    REQUIRE(replay::CaptureDirForProfile(L"C:\\Users\\known", out, replay::kPathMax));
    CHECK(std::wstring(out) == L"C:\\Users\\known\\Documents\\EchoReplay");

    // A truncated path is a path to somewhere else: refused, and the buffer left empty rather than
    // holding a prefix a caller could mkdir.
    wchar_t small[8] = {L'x'};
    CHECK_FALSE(replay::CaptureDirUnder(L"C:\\Users\\known\\Documents", small, 8));
    CHECK(small[0] == L'\0');
    CHECK_FALSE(replay::CaptureDirForProfile(L"C:\\Users\\known", small, 8));
    CHECK(small[0] == L'\0');

    // Nothing to build from is a refusal, not `\EchoReplay` at the root of the current drive.
    CHECK_FALSE(replay::CaptureDirUnder(L"", out, replay::kPathMax));
    CHECK_FALSE(replay::CaptureDirUnder(nullptr, out, replay::kPathMax));
    CHECK_FALSE(replay::CaptureDirForProfile(L"", out, replay::kPathMax));
}

TEST_CASE("the redirected capture directory is a named child of the state directory") {
    replay::SetPathsForTest(L"C:\\game\\bin\\win10\\plugins\\echoreplay.dll");
    wchar_t out[replay::kPathMax] = {};

    REQUIRE(replay::ResolveCaptureDir(nullptr, out, replay::kPathMax));
    CHECK(std::wstring(out) == std::wstring(replay::StateDir()) + L"\\EchoReplay");
    // The property the arm's comment claims, and the one the corruption broke: a sibling passes the
    // string compare above only if the string compare is also wrong.
    CHECK(replay::IsUnderDirectory(out, replay::StateDir()));

    REQUIRE(replay::ResolveCaptureDir("", out, replay::kPathMax));
    CHECK(std::wstring(out) == std::wstring(replay::StateDir()) + L"\\EchoReplay");

    // `capture.dir` is an override, so it wins over the redirect and arrives unchanged -- no
    // `EchoReplay` appended to a directory the owner already named.
    REQUIRE(replay::ResolveCaptureDir("D:\\replays\\mine", out, replay::kPathMax));
    CHECK(std::wstring(out) == L"D:\\replays\\mine");
}

// ---------------------------------------------------------------------------------------------
// PERF-1. log::Direct used to be CreateFileW + WriteFile + CloseHandle per line, on whichever thread
// called it, and 44 of its callers are on the game's frame thread. These are the properties the fix has
// to keep while it stops doing that: order, loss that is counted rather than hidden, a last line before
// a fault that still reaches the disk, and a log that does not grow to 133 MB inside Program Files.

namespace {

std::string ReadAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got != 0) {
        out.append(buf, got);
    }
    CloseHandle(h);
    return out;
}

// A redirected log that is deferred exactly the way a running plugin's is, and put back afterwards.
struct DeferredLog {
    explicit DeferredLog(const wchar_t* prefix) : path(TempFile(prefix)) {
        replay::log::RedirectForTest(path.c_str());
        replay::log::SetLevel(replay::LogLevel::Info);
        replay::log::SetDeferred(true);
    }
    ~DeferredLog() {
        replay::log::SetDeferred(false);
        replay::log::EndRedirectForTest();
        DeleteFileW(path.c_str());
    }
    std::wstring path;
};

}  // namespace

TEST_CASE("a queued Direct line opens no file, and the drain opens exactly one for the batch") {
    DeferredLog log(L"lg1");

    const uint64_t opensBefore = replay::log::FileOpenCount();
    for (int i = 0; i < 64; ++i) {
        replay::log::Direct(replay::LogLevel::Info, "t", "ordered %d", i);
    }
    // The whole point: 64 lines from the frame thread, zero CreateFileW.
    CHECK(replay::log::FileOpenCount() == opensBefore);

    replay::log::Drain();
    CHECK(replay::log::FileOpenCount() == opensBefore + 1);

    const std::string text = ReadAll(log.path);
    size_t at = 0;
    for (int i = 0; i < 64; ++i) {
        char want[32];
        std::snprintf(want, sizeof want, "ordered %d\r\n", i);
        const size_t found = text.find(want, at);
        REQUIRE_MESSAGE(found != std::string::npos, want);
        at = found + 1;   // strictly increasing, so the file is in call order
    }
}

TEST_CASE("an ERROR line writes through, and takes the queued lines with it so the order holds") {
    DeferredLog log(L"lg2");

    replay::log::Direct(replay::LogLevel::Info, "t", "queued-first");
    const uint64_t opens = replay::log::FileOpenCount();
    replay::log::Direct(replay::LogLevel::Error, "t", "fault-last");
    CHECK(replay::log::FileOpenCount() > opens);   // it did not wait for the worker

    const std::string text = ReadAll(log.path);
    const size_t first = text.find("queued-first");
    const size_t last = text.find("fault-last");
    REQUIRE(first != std::string::npos);
    REQUIRE(last != std::string::npos);
    CHECK(first < last);
}

TEST_CASE("a burst past the ring is counted and the count reaches the log") {
    DeferredLog log(L"lg3");

    const uint64_t before = replay::log::Dropped();
    for (int i = 0; i < 600; ++i) {
        replay::log::Direct(replay::LogLevel::Info, "t", "flood %d", i);
    }
    replay::log::Drain();
    CHECK(replay::log::Dropped() > before);
    CHECK(ReadAll(log.path).find("log line(s) dropped so far") != std::string::npos);
}

TEST_CASE("shutdown flushes what the ring still holds and stops deferring") {
    DeferredLog log(L"lg4");

    replay::log::Direct(replay::LogLevel::Info, "t", "last-gasp");
    replay::log::Shutdown();
    CHECK(ReadAll(log.path).find("last-gasp") != std::string::npos);
    CHECK_FALSE(replay::log::Deferred());
}

TEST_CASE("an oversized log is archived under the state directory rather than appended to forever") {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"perf1rot";
    CreateDirectoryW(dir.c_str(), nullptr);
    replay::SetPathsForTest((dir + L"\\plugins\\echoreplay.dll").c_str());

    const std::wstring logPath = replay::LogPath();
    const std::wstring archive = std::wstring(replay::StateDir()) + L"\\echoreplay.prev.log";
    DeleteFileW(archive.c_str());
    {
        HANDLE h = CreateFileW(logPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        DWORD wrote = 0;
        WriteFile(h, "0123456789", 10, &wrote, nullptr);
        CloseHandle(h);
    }

    // Under the cap: left exactly where it is, because the history is worth keeping until it is not.
    replay::log::RotateForTest(1024);
    CHECK(GetFileAttributesW(logPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(archive.c_str()) == INVALID_FILE_ATTRIBUTES);

    replay::log::RotateForTest(8);
    CHECK(GetFileAttributesW(logPath.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(archive.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(ReadAll(archive) == "0123456789");

    DeleteFileW(archive.c_str());
    RemoveDirectoryW(replay::StateDir());
    RemoveDirectoryW(dir.c_str());
}

TEST_CASE("a mapped address costs no VirtualQuery after the first one resolves the headers") {
    // The in-frame UI site resolves 14 to 18 constant VAs per rendered frame and every one of them used
    // to walk the PE headers behind two VirtualQuery calls.
    REQUIRE(replay::buildguard::MappedAddress(facts::PE_IMAGE_BASE) != nullptr);

    const uint64_t before = replay::ReadableProbeCount();
    for (int i = 0; i < 128; ++i) {
        CHECK(replay::buildguard::MappedAddress(facts::PE_IMAGE_BASE + 0x100) != nullptr);
    }
    CHECK(replay::ReadableProbeCount() == before);
}

TEST_CASE("the cost meter reports one line per window, then starts an empty one") {
    replay::tickcost::ResetForTest();
    char line[256];
    // Nothing ran, so there is nothing to say: a 1 Hz caller stays silent while the viewer is idle.
    CHECK_FALSE(replay::tickcost::FormatAndReset(line, sizeof line));

    {
        const replay::tickcost::Scope scope(replay::tickcost::kApply);
        Sleep(2);
    }
    replay::tickcost::NoteFrame(8);
    CHECK(replay::tickcost::PhaseUsForTest(replay::tickcost::kApply) > 0);

    REQUIRE(replay::tickcost::FormatAndReset(line, sizeof line));
    const std::string s(line);
    CHECK(s.rfind("cost: total=", 0) == 0);
    CHECK(s.find("players=8") != std::string::npos);
    CHECK(s.find("frames=1") != std::string::npos);
    CHECK(s.find(", other=") != std::string::npos);

    CHECK(replay::tickcost::PhaseUsForTest(replay::tickcost::kApply) == 0);
    CHECK_FALSE(replay::tickcost::FormatAndReset(line, sizeof line));
}
