// W1-L's feed check. Runs the offline host three times and reads its verdict.
//
// The audit that rejected this packet found that section 9 check 3 -- "callbacks arrive at
// ~44 Hz", the memo's own "the one that matters" -- was queued against a launch profile
// docs/HARNESS.md section 4a refuses, so its passing case could not be constructed. This is the
// replacement, and it is not a weaker one: the same 44.02 Hz arrives out of R4's recorded
// session, through the engine's own CBroadcaster::Listen, in a process that never opens a
// socket and never starts the game.
//
// Every number checked here was published before this packet existed. The design
// records the capture as 5,520 PlayerState records at 44.02 Hz and the DUV channel at 118 Hz
// combined; FRESHCAP_FINDINGS.md records 34,791 datagrams and 3,616 SR15NetComponentEvent, all
// 24 bytes. If the corpus reader silently shortened the feed, these would move.
//
// The exit code is asserted with REQUIRE and not CHECK, for both mutants as well as the real
// host. "The mutation delivered nothing" is only evidence if the mutant got far enough to map
// the engine image and register, and the exit code is what says which of those happened.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <doctest.h>

namespace {

std::string BinDir() {
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, sizeof self);
    const std::string s(self);
    return s.substr(0, s.find_last_of(0x5C));
}

std::string TempVerdictPath(const char* stem) {
    char root[MAX_PATH] = {0};
    GetTempPathA(sizeof root, root);
    char out[MAX_PATH];
    _snprintf_s(out, sizeof out, _TRUNCATE, "%sw1l_%s_%lu.json", root, stem,
                GetCurrentProcessId());
    return out;
}

struct HostRun {
    bool        started = false;
    DWORD       exitCode = 0xFFFFFFFF;
    std::string verdict;
};

// The paced run replays the capture's own 125.387 s, so the wait has to allow for it. A host
// that has not exited by then is not a slow pass; it is a host that did not finish, and the
// caller treats the missing exit code as such.
constexpr DWORD kHostTimeoutMs = 300000;

HostRun RunHost(const char* exeStem) {
    HostRun run;
    const std::string verdictPath = TempVerdictPath(exeStem);
    DeleteFileA(verdictPath.c_str());

    const char q = 0x22;
    std::string cmd = q + BinDir() + "\\" + exeStem + ".exe" + q + " " + q + verdictPath + q;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return run;
    }
    run.started = true;
    if (WaitForSingleObject(pi.hProcess, kHostTimeoutMs) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &run.exitCode);
    } else {
        TerminateProcess(pi.hProcess, 0xDEAD);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::FILE* f = nullptr;
    if (fopen_s(&f, verdictPath.c_str(), "rb") == 0 && f != nullptr) {
        char chunk[4096];
        size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof chunk, f)) != 0) {
            run.verdict.append(chunk, got);
        }
        std::fclose(f);
    }
    DeleteFileA(verdictPath.c_str());
    return run;
}

// Minimal readers over the verdict document. `from` lets a caller scope a lookup to one stream
// object, so three streams with the same field names do not read each other's numbers.
size_t StreamAt(const std::string& doc, const char* name) {
    const std::string key = std::string("\"name\": \"") + name + "\"";
    return doc.find(key);
}

bool Number(const std::string& doc, const char* key, double& out, size_t from = 0) {
    const std::string needle = std::string(1, 0x22) + key + "\": ";
    const size_t at = doc.find(needle, from);
    if (at == std::string::npos) {
        return false;
    }
    out = std::strtod(doc.c_str() + at + needle.size(), nullptr);
    return true;
}

double Field(const std::string& doc, const char* key, size_t from = 0) {
    double value = -1;
    REQUIRE_MESSAGE(Number(doc, key, value, from), "the verdict has no field named ", key);
    return value;
}

// What the corpus is, published before this packet: the design and R4's
// FRESHCAP_FINDINGS.md. A feed that reads fewer messages than the session held would still
// deliver everything it read, so the counts are the control on the reader.
struct Expected {
    const char* name;
    double      count;
    double      hzOverCapture;
    double      bodyMin;
    double      bodyMax;
};
constexpr Expected kExpected[] = {
    {"SR15NetPlayerStateMsg", 5520, 44.02, 64, 64},
    {"SR15NetReplicatedDeltaUnreliableVars", 14796, 118.00, 24, 926},
    {"SR15NetComponentEvent", 3616, 28.84, 24, 24},
};
constexpr double kCaptureSpanSeconds = 125.387346;

}  // namespace

TEST_CASE("the recorded session reaches the tap through the engine's own listener nodes") {
    const HostRun run = RunHost("wiretap_feedhost");
    REQUIRE_MESSAGE(run.started, "wiretap_feedhost.exe did not start");
    if (run.exitCode == 10 || run.exitCode == 12) {
        MESSAGE("SKIPPED: needs echovr.exe (ECHOVR_DIR or REPLAY_ECHOVR_EXE) and the private freshcap "
                "capture (REPLAY_FRESHCAP_DIR); the host exited ", run.exitCode);
        return;
    }
    REQUIRE_MESSAGE(run.exitCode == 0, "the host exited ", run.exitCode, ": ", run.verdict);
    REQUIRE_FALSE(run.verdict.empty());

    CHECK(run.verdict.find("\"verdict\": \"PASS\"") != std::string::npos);
    CHECK(Field(run.verdict, "checked") > 20000);
    CHECK(Field(run.verdict, "captureSpanSeconds") == doctest::Approx(kCaptureSpanSeconds));

    for (const Expected& e : kExpected) {
        const size_t at = StreamAt(run.verdict, e.name);
        REQUIRE_MESSAGE(at != std::string::npos, "the verdict has no stream named ", e.name);
        INFO("stream ", e.name);
        CHECK(Field(run.verdict, "handle", at) != 65535);
        CHECK(Field(run.verdict, "fed", at) == e.count);
        CHECK(Field(run.verdict, "delivered", at) == e.count);
        CHECK(Field(run.verdict, "wrong", at) == 0);
        CHECK(Field(run.verdict, "bodyBytesMin", at) == e.bodyMin);
        CHECK(Field(run.verdict, "bodyBytesMax", at) == e.bodyMax);
        CHECK(Field(run.verdict, "hzOverCapture", at) == doctest::Approx(e.hzOverCapture).epsilon(0.001));
    }

    // The host refuses to report a pass on the pacing check when REPLAY_WIRETAP_FAST is set, so
    // an elapsed time near the capture's own span is evidence the feed really was driven at the
    // recorded rate and not merely replayed as fast as memory allows.
    const double elapsed = Field(run.verdict, "elapsedSeconds");
    CHECK_MESSAGE(elapsed >= kCaptureSpanSeconds * 0.98,
                  "the feed finished in ", elapsed,
                  " s against a capture spanning ", kCaptureSpanSeconds,
                  " s, so it was not driven at the recorded rate");
}

TEST_CASE("the shape this packet believed before it read the dispatchers cannot fire") {
    const HostRun run = RunHost("wiretap_feedhost_oldshape");
    REQUIRE_MESSAGE(run.started, "wiretap_feedhost_oldshape.exe did not start");
    if (run.exitCode == 10 || run.exitCode == 12) {
        MESSAGE("SKIPPED: needs echovr.exe and the private freshcap capture (REPLAY_FRESHCAP_DIR)");
        return;
    }
    // 3 is the node audit. Any other code means the mutant failed somewhere else, and a control
    // that fails for the wrong reason has not been shown to detect anything.
    REQUIRE_MESSAGE(run.exitCode == 3, "the old-shape control exited ", run.exitCode,
                    " instead of failing its node audit: ", run.verdict);
    CHECK(run.verdict.find("node+40") != std::string::npos);
}

TEST_CASE("a tap on the wrong bus registers, is well formed, and never fires") {
    const HostRun run = RunHost("wiretap_feedhost_wrongbus");
    REQUIRE_MESSAGE(run.started, "wiretap_feedhost_wrongbus.exe did not start");
    if (run.exitCode == 10 || run.exitCode == 12) {
        MESSAGE("SKIPPED: needs echovr.exe and the private freshcap capture (REPLAY_FRESHCAP_DIR)");
        return;
    }
    // 5 is the delivery check. The control has to reach it: exiting earlier would mean the tap
    // never registered, which is a different and much more visible failure than the one this
    // reproduces.
    REQUIRE_MESSAGE(run.exitCode == 5, "the wrong-bus control exited ", run.exitCode,
                    " instead of failing its delivery check: ", run.verdict);
    CHECK(run.verdict.find("registered and never fired") != std::string::npos);

    for (const Expected& e : kExpected) {
        const size_t at = StreamAt(run.verdict, e.name);
        REQUIRE(at != std::string::npos);
        INFO("stream ", e.name);
        CHECK_MESSAGE(Field(run.verdict, "handle", at) != 65535,
                      "the control did not register, so it does not reproduce a quiet tap");
        CHECK(Field(run.verdict, "delivered", at) == 0);
    }
}

// ---------------------------------------------------------------------------------------------
// The reporting proof for the in-game probe, run offline so no launch is spent on it.
//
// probe_wiretap.dll answers three payload reads on the `offline-local` profile. Its verdict is
// filed from a static initialiser, because coexist::ClaimInstance refuses every module after the
// first the loader reaches -- alphabetically -- and a verdict written from a module init is then
// never written at all. That is not hypothetical: it cost sweep 4 three probes.
//
// session_probehost loads DLLs into one process the way EchoLoader does, so the refusal is
// reproducible here. Two copies of the same DLL under different names reproduce it exactly: the
// claim is keyed on the resolved log path plus the pid, not on the file name.
// ---------------------------------------------------------------------------------------------

namespace {

bool RunProbeHost(const std::string& root, const std::vector<std::string>& dlls) {
    const char q = 0x22;
    std::string cmd = q + BinDir() + "\\session_probehost.exe" + q + " " + q + root + q;
    for (const std::string& d : dlls) {
        cmd += " " + std::string(1, q) + root + "\\" + d + std::string(1, q);
    }
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

std::string ReadVerdictFile(const std::string& root, const std::string& stem) {
    const std::string path = root + "\\" + stem + "\\done.json";
    std::FILE* f = nullptr;
    std::string out;
    if (fopen_s(&f, path.c_str(), "rb") == 0 && f != nullptr) {
        char chunk[4096];
        size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof chunk, f)) != 0) {
            out.append(chunk, got);
        }
        std::fclose(f);
    }
    return out;
}

}  // namespace

TEST_CASE("the in-game probe files a verdict even when its instance claim is refused") {
    const std::string bin = BinDir();
    char tempRoot[MAX_PATH] = {0};
    GetTempPathA(sizeof tempRoot, tempRoot);
    char unique[MAX_PATH];
    _snprintf_s(unique, sizeof unique, _TRUNCATE, "%sw1l_reporting_%lu", tempRoot,
                GetCurrentProcessId());
    CreateDirectoryA(unique, nullptr);
    const std::string root(unique);

    // aaa sorts first and wins the claim, so probe_wiretap is the REFUSED one -- the position it
    // occupies in any real multi-probe launch. zzz is the mutation, refused as well.
    REQUIRE(CopyFileA((bin + "\\probe_wiretap.dll").c_str(),
                      (root + "\\probe_aaa_first.dll").c_str(), FALSE) != 0);
    REQUIRE(CopyFileA((bin + "\\probe_wiretap.dll").c_str(),
                      (root + "\\probe_wiretap.dll").c_str(), FALSE) != 0);
    REQUIRE(CopyFileA((bin + "\\probe_wiretap_nofiler.dll").c_str(),
                      (root + "\\probe_zzz_nofiler.dll").c_str(), FALSE) != 0);

    // There is no game here, so no phase ever finishes; the worker would otherwise sit on its
    // 200 s launch deadline and the host would be gone long before the real verdict landed.
    SetEnvironmentVariableA("REPLAY_PROBE_DEADLINE_MS", "1500");
    const bool hosted = RunProbeHost(root, {"probe_aaa_first.dll", "probe_wiretap.dll",
                                            "probe_zzz_nofiler.dll"});
    SetEnvironmentVariableA("REPLAY_PROBE_DEADLINE_MS", nullptr);

    // REQUIRE, not CHECK: a host that did not load all three proves nothing either way, and "the
    // mutation filed nothing" would pass just as well if the mutation had never loaded.
    REQUIRE_MESSAGE(hosted, "session_probehost did not load all three DLLs, so the results are void");

    const std::string first = ReadVerdictFile(root, "probe_aaa_first");
    const std::string refused = ReadVerdictFile(root, "probe_wiretap");
    const std::string mutant = ReadVerdictFile(root, "probe_zzz_nofiler");

    CHECK_FALSE(first.empty());
    CHECK_MESSAGE(!refused.empty(), "the refused copy filed nothing: this is exactly sweep 4");
    CHECK_MESSAGE(mutant.empty(),
                  "the legacy-shaped mutation filed, so this control cannot detect the defect");

    // A real verdict, not the provisional placeholder, and one that actually evaluated its checks.
    CHECK(refused.find("\"provisional\":true") == std::string::npos);
    CHECK(refused.find("\"checked\":12") != std::string::npos);
    // No game here, so every read is undetermined -- and NOT-RUN is the honest answer for that.
    // Reporting it as PASS would be a false all-clear and as FAIL a false accusation.
    CHECK(refused.find("\"verdict\":\"NOT-RUN\"") != std::string::npos);
    CHECK(refused.find("netgame-reached=COULD-NOT-DETERMINE") != std::string::npos);
    CHECK(refused.find("broadcaster-non-null=COULD-NOT-DETERMINE") != std::string::npos);
    // The blocker object is mandatory: an omitted one is recorded as unknown, and unknown is not safe.
    CHECK(refused.find("\"egress\"") != std::string::npos);
}
