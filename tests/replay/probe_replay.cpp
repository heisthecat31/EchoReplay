// The observer module of probe_replay.dll -- the product plus this one translation unit.
//
// Probe_replay links echoreplay's own SOURCES, so the code under
// observation IS the shipped code. This file adds no product behaviour and re-implements no product
// path. It watches, and it writes what it saw where the harness reads.
//
// ★ IT IS A MODULE, NOT A PROBE DLL. The product's own src/plugin/dllmain.cpp is compiled into this
// binary and already calls bootstrap::Attach, so a second DllMain here would not link, a second
// bootstrap would contend on the instance claim, and a second tick claimant would abort on
// AbortBothTaken (src/engine/tick_policy.cpp). One DLL, one instance claim, one tick slot.
//
// ★ ORDER 900. Every product module registers below 400; this one runs last, so what it samples is
// what the product left, not what it interrupted.
//
// ★ IT NEVER ENDS A LAUNCH EARLY. Every record it writes before its declared completion point
// carries "provisional": true, which the harness's Get-VerdictFinality reads as PROVISIONAL and its
// completion predicate reads past. probe_clock's PASS at 16.1 s of a 300 s launch is what that rule
// exists to prevent.
//
// ★ IT NEVER WRITES sc7_text_diff.json. src/bootstrap/sc7.cpp is in this binary and writes that
// record itself; a second writer here would race the real one and the loser's content would decide
// a seven-row block. The observer READS it back and scores what the product wrote.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bootstrap/egress.h"
#include "bootstrap/hookmgr.h"
#include "bootstrap/inbound.h"
#include "engine/entity.h"
#include "bootstrap/paths.h"
#include "bootstrap/services.h"
#include "capture/capturestop.h"
#include "capture/manifestbuild.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "engine/audiorate.h"
#include "engine/tick.h"
#include "camera/camera_control.h"
#include "control/seekgoto.h"
#include "engine/wiretap.h"
#include "control/command.h"
#include "control/dispatch.h"
#include "playback/core/playback.h"
#include "playback/core/transport.h"
#include "playback/residue/residue.h"
#include "ui/csregister.h"
#include "ui/uiapi.h"

#include "observer_rows.h"

namespace {

using replay::LogLevel;
using probe_replay::Ask;
using probe_replay::LogFacts;
using probe_replay::StatusFacts;

// The arm is a property of THIS launch or it is not a property at all (RK-11). It is read from the
// process environment, never from the command line -- an unregistered switch voids the whole engine
// line, pins included -- and never from a config key. The value AND where it came
// from are logged, so an arm nobody intended is visible on disk instead of inferred from what the
// run did.
enum class Arm { Capture, Playback, PlaybackIdle, Browser, Refused };

replay::ReplayServices* g_services = nullptr;
Arm g_arm = Arm::Capture;
unsigned g_armBit = probe_replay::kArmCapture;
char g_armReason[320] = {0};
char g_probeId[128] = "W2-R-unknown";
char g_artifactDir[MAX_PATH] = {0};
char g_coexist[256] = {0};
bool g_coexistViolation = false;
HANDLE g_worker = nullptr;
volatile LONG g_stop = 0;

// Teardown sets g_stop and then JOINS this thread with a bound. Every wait the
// worker can be sitting in has to hear that, or the join times out and the worker keeps calling
// g_services (and racing WriteRecord) through the module's own Shutdown -- the same thread and the
// same class of fault as the AV this packet fixed.
bool StopRequested() { return InterlockedCompareExchange(&g_stop, 0, 0) != 0; }

// Sleeps in slices no longer than one poll so a stop is noticed within a slice, and answers whether
// it was cut short. Callers that only settle can ignore the answer; callers that measure a window
// must not, and take their timing from the clock either way.
bool StopAwareSleep(DWORD ms) {
    const DWORD kSliceMs = 100;
    for (DWORD left = ms; left > 0;) {
        if (StopRequested()) return false;
        const DWORD slice = left < kSliceMs ? left : kSliceMs;
        Sleep(slice);
        left -= slice;
    }
    return !StopRequested();
}
LARGE_INTEGER g_qpcFreq = {0};
LARGE_INTEGER g_qpcStart = {0};
LONG g_heartbeats = 0;

LogFacts g_log;
StatusFacts g_status;
// The BROWSER arm's cross-thread state, one writer each. The worker latches g_levelReached from
// the entity module's bound-space line; the TICK reads it and is the only writer of the two
// browser flags, because ui::OpenBrowser reaches panel state that is drawn from the tick.
volatile LONG g_levelReached = 0;
volatile bool g_browserOpened = false;
volatile double g_browserOpenedMs = 0.0;

// Written by the tick, read by the observer worker. Only the ENUM crosses the thread boundary;
// the name and the diagnostic sentence are resolved on the reader's side, so no buffer is shared.
volatile LONG g_cameraProbeLast = -1;      // camera_control::RigProbe, -1 = never called
volatile LONG g_cameraProbeReadyEver = 0;

// A window that elapsed before the browser was even opened observed the browser for no time at
// all. The completion point holds for this much browser-open time as well as for the window.
const double kBrowserDwellMs = 5000.0;
bool g_batteryRan = false;
// Set only after the LAST step has been issued and collected. `g_batteryRan` goes true the
// moment the battery starts, so it cannot answer "did the battery finish" -- and the playback
// arm's completion point now waits on that answer.
bool g_batteryComplete = false;
bool g_completed = false;
bool g_batteryCompleteReported = false;

// L3-RUN-4 F2. Why the record was written, published beside `provisional` so a reader can
// tell a heartbeat snapshot from the write that closed the launch.
enum class WriteReason { Periodic, BatteryComplete, FileEnd, Shutdown };

const char* WriteReasonName(WriteReason r) {
    switch (r) {
        case WriteReason::Periodic:        return "periodic";
        case WriteReason::BatteryComplete: return "battery-complete";
        case WriteReason::FileEnd:         return "file-end";
        case WriteReason::Shutdown:        return "shutdown";
    }
    return "unknown";
}
char g_completion[256] = "not reached: the launch has not got to its declared completion point";

// A launch that files its verdict long before its budget is the defect pattern 9.8 names -- two
// probes killed their own launches that way. Even when an arm's completion observable fires, the
// final record waits for this floor and says so.
const double kEarliestFinalMs = 20000.0;

double SinceStartMs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_qpcFreq.QuadPart == 0) {
        return 0.0;
    }
    return static_cast<double>(now.QuadPart - g_qpcStart.QuadPart) * 1000.0 /
           static_cast<double>(g_qpcFreq.QuadPart);
}

void Log(LogLevel level, const char* fmt, ...) {
    if (g_services == nullptr || g_services->log == nullptr) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, args);
    va_end(args);
    g_services->log->Write(level, "observer", "%s", line);
}

HMODULE SelfModule() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&SelfModule), &self);
    return self;
}

// C1: the artifact directory is the one the harness NAMED in REPLAY_PROBE_DIRS, looked up by our own
// module base name. Re-deriving the layout from REPLAY_PROBE_ROOT is what left probe_playback's
// verdict unread for a whole sweep.
bool ResolveArtifactDir(char* dir, size_t dirN, char* probeId, size_t idN) {
    char dll[MAX_PATH] = {0};
    if (GetModuleFileNameA(SelfModule(), dll, sizeof dll) == 0) {
        return false;
    }
    const char* leaf = strrchr(dll, '\\');
    leaf = leaf != nullptr ? leaf + 1 : dll;
    char stem[MAX_PATH] = {0};
    strncpy_s(stem, sizeof stem, leaf, _TRUNCATE);
    if (char* dot = strrchr(stem, '.')) {
        *dot = '\0';
    }

    char dirs[2048] = {0};
    if (GetEnvironmentVariableA("REPLAY_PROBE_DIRS", dirs, sizeof dirs) == 0) {
        return false;   // no sweep, so nothing is written into somebody's install
    }
    char key[MAX_PATH];
    _snprintf_s(key, sizeof key, _TRUNCATE, "%s=", stem);
    const char* found = strstr(dirs, key);
    if (found == nullptr) {
        return false;
    }
    const char* value = found + strlen(key);
    size_t n = strcspn(value, ";");
    if (n >= dirN) {
        n = dirN - 1;
    }
    memcpy(dir, value, n);
    dir[n] = '\0';
    CreateDirectoryA(dir, nullptr);

    char ids[1024] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    if (const char* idFound = strstr(ids, key)) {
        const char* idValue = idFound + strlen(key);
        size_t m = strcspn(idValue, ";");
        if (m >= idN) {
            m = idN - 1;
        }
        memcpy(probeId, idValue, m);
        probeId[m] = '\0';
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// THE LAUNCH MARKER.
//
// The window a windowless arm is given, and whether a human is watching, are properties of THIS
// launch. They cannot travel in the environment: the harness's child-environment allow-list carries
// exactly two names (ECHOREPLAY_OPEN and ECHOREPLAY_CAPTURE_SECS, the design notes), an unlisted
// third is REFUSED at Test-LaunchWindows, and a module that fell back to a default when the name was
// absent would answer a question nobody could ask. So the harness writes them into a small JSON
// beside the launch and the observer reads that file.
//
// It is looked for by the SAME resolution the capture module already uses for its stop request
// (src/capture/capture.cpp: the current directory, then StateDir() when that differs) rather than a
// second layout of our own -- two resolutions for one directory is how a file gets written in one
// place and looked for in another.
//
// Absent, or present without the field, the answer is could-not-ask named `no-launch-marker`, and
// the rows say so. Nothing here defaults.
// ---------------------------------------------------------------------------------------------
const char kLaunchMarkerName[] = "launch-marker.json";

// Reads the marker out of one directory into the observer's facts. The parse is
// probe_replay::ParseLaunchMarker, which tests/replay/test_observer.cpp drives without a game.
bool ReadMarkerFrom(const char* dir) {
    if (dir == nullptr || dir[0] == '\0') {
        return false;
    }
    char full[MAX_PATH];
    _snprintf_s(full, sizeof full, _TRUNCATE, "%s\\%s", dir, kLaunchMarkerName);
    FILE* fh = nullptr;
    if (fopen_s(&fh, full, "rb") != 0 || fh == nullptr) {
        return false;
    }
    char text[4096];
    const size_t got = fread(text, 1, sizeof text - 1, fh);
    fclose(fh);
    text[got] = '\0';
    probe_replay::ParseLaunchMarker(text, full, g_status);
    return true;
}

void LoadLaunchMarker() {
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char state[MAX_PATH] = {0};
    const wchar_t* wide = replay::StateDir();
    if (wide != nullptr && wide[0] != L'\0') {
        WideCharToMultiByte(CP_ACP, 0, wide, -1, state, sizeof state, nullptr, nullptr);
    }
    if (!ReadMarkerFrom(cwd) && (_stricmp(state, cwd) == 0 || !ReadMarkerFrom(state))) {
        char where[320];
        _snprintf_s(where, sizeof where, _TRUNCATE, "%s is in neither '%s' nor '%s'",
                    kLaunchMarkerName, cwd, state[0] != '\0' ? state : "(StateDir unresolved)");
        probe_replay::LaunchMarkerAbsent(where, g_status);
    }
    Log(LogLevel::Info, "launch marker: %s (window %s, attended %s)",
        g_status.markerPresent ? g_status.markerSource : "ABSENT",
        g_status.markerWindowKnown ? "declared" : g_status.markerWindowReason,
        g_status.markerAttendedKnown ? (g_status.markerAttended ? "true" : "false")
                                     : g_status.markerAttendedReason);
}

// PLAYBACK-IDLE is selected by the LAUNCH, not by the file: it is the same ECHOREPLAY_OPEN value as
// PLAYBACK and differs only in that a human is watching (6.4). Nothing carries `attended` to the
// module -- the child's environment is the harness's five REPLAY_* names plus the profile's two
// allow-listed ones -- so the arm keys on the probe id the harness DOES hand us, which the
// descriptor declares. That keeps the arm a property of this launch's descriptor row rather than of
// a machine-wide variable, which is RK-11's whole requirement.
bool ProbeIdDeclaresIdle() {
    const size_t n = strlen(g_probeId);
    const char* suffix = "-idle";
    const size_t s = strlen(suffix);
    return n >= s && _stricmp(g_probeId + (n - s), suffix) == 0;
}

Arm SelectArm(char* reason, size_t reasonN) {
    char open[MAX_PATH] = {0};
    const DWORD len = GetEnvironmentVariableA("ECHOREPLAY_OPEN", open, sizeof open);
    if (len == 0) {
        _snprintf_s(reason, reasonN, _TRUNCATE, "CAPTURE: ECHOREPLAY_OPEN is unset in this process");
        return Arm::Capture;
    }
    if (len >= sizeof open) {
        _snprintf_s(reason, reasonN, _TRUNCATE,
                    "REFUSED: ECHOREPLAY_OPEN is %lu bytes, longer than MAX_PATH", len);
        return Arm::Refused;
    }
    if (open[0] == '\0') {
        // An empty value is refused rather than being a third meaning (section 6.4 R4).
        _snprintf_s(reason, reasonN, _TRUNCATE, "REFUSED: ECHOREPLAY_OPEN is set but empty");
        return Arm::Refused;
    }
    const DWORD attrs = GetFileAttributesA(open);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        _snprintf_s(reason, reasonN, _TRUNCATE,
                    "REFUSED: ECHOREPLAY_OPEN='%s' names nothing on disk (GetFileAttributes %lu)",
                    open, GetLastError());
        return Arm::Refused;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        _snprintf_s(reason, reasonN, _TRUNCATE, "BROWSER: ECHOREPLAY_OPEN='%s' is a directory", open);
        return Arm::Browser;
    }
    if (ProbeIdDeclaresIdle()) {
        _snprintf_s(reason, reasonN, _TRUNCATE,
                    "PLAYBACK-IDLE: ECHOREPLAY_OPEN='%s' is a file and the descriptor's probe id "
                    "'%s' declares the attended launch, so nothing here drives the transport; the "
                    "launch marker says attended=%s (the id selects, the marker is scored beside it "
                    "by w1j-launch-attendance-declared)",
                    open, g_probeId,
                    g_status.markerAttendedKnown ? (g_status.markerAttended ? "true" : "false")
                                           : "no-launch-marker");
        return Arm::PlaybackIdle;
    }
    _snprintf_s(reason, reasonN, _TRUNCATE, "PLAYBACK: ECHOREPLAY_OPEN='%s' is a file", open);
    return Arm::Playback;
}

unsigned ArmBit(Arm arm) {
    switch (arm) {
        case Arm::Playback:     return probe_replay::kArmPlayback;
        case Arm::PlaybackIdle: return probe_replay::kArmIdle;
        case Arm::Browser:      return probe_replay::kArmBrowser;
        default:                return probe_replay::kArmCapture;
    }
}

const char* ArmName(Arm arm) {
    switch (arm) {
        case Arm::Playback:     return "PLAYBACK";
        case Arm::PlaybackIdle: return "PLAYBACK-IDLE";
        case Arm::Browser:      return "BROWSER";
        case Arm::Refused:      return "REFUSED";
        default:                return "CAPTURE";
    }
}

// Exactly one of echoreplay.dll and probe_replay.dll may be installed for a launch (section 6.2).
// Both carry bootstrap, and coexist::ClaimInstance keys the mutex on the install root's log path, so
// the second to attach installs nothing and files nothing -- a silence the sweep would have to
// explain. This says so on disk instead, and it is a report, not a repair: the harness enforces the
// install set (Rule G over C5's manifest), and a module that deleted or unloaded the other one would
// be changing the launch it is measuring.
void RecordCoexistence(char* out, size_t outN) {
    char dll[MAX_PATH] = {0};
    if (GetModuleFileNameA(SelfModule(), dll, sizeof dll) == 0) {
        _snprintf_s(out, outN, _TRUNCATE, "could not ask: GetModuleFileName failed (%lu)", GetLastError());
        return;
    }
    char* leaf = strrchr(dll, '\\');
    if (leaf == nullptr) {
        _snprintf_s(out, outN, _TRUNCATE, "could not ask: module path '%s' has no directory", dll);
        return;
    }
    *(leaf + 1) = '\0';
    char sibling[MAX_PATH];
    _snprintf_s(sibling, sizeof sibling, _TRUNCATE, "%sechoreplay.dll", dll);
    const bool onDisk = GetFileAttributesA(sibling) != INVALID_FILE_ATTRIBUTES;
    const bool inProcess = GetModuleHandleA("echoreplay.dll") != nullptr;
    if (onDisk || inProcess) {
        g_coexistViolation = true;
        _snprintf_s(out, outN, _TRUNCATE,
                    "COEXIST VIOLATION: echoreplay.dll is %s%s%s beside this module, so one of the two "
                    "bootstraps lost the instance claim and measured nothing",
                    onDisk ? "installed" : "", (onDisk && inProcess) ? " and " : "",
                    inProcess ? "loaded in this process" : "");
        return;
    }
    _snprintf_s(out, outN, _TRUNCATE, "sole plugin: echoreplay.dll is neither installed nor loaded");
}

// ---------------------------------------------------------------------------------------------
// The log tail. The product's log file IS its published line surface, and reading it is what makes
// this an observer: it consumes what the shipped modules already write, on their own schedule, and
// nothing here calls into their internals to make a line happen.
//
// The path is the product's own replay::LogPath() and nothing else. An observer-private
// environment name would be a THIRD name in a child environment whose allow-list is frozen at two
// So the harness would refuse the launch that set it and every launch that did
// not set it would silently use the fallback -- a switch that is vacuous when present and invisible
// when absent. The pure-function battery in tests/replay/test_observer.cpp is what drives the arms
// without a game, so nothing needs to redirect this tail.
// ---------------------------------------------------------------------------------------------
class LogTail {
public:
    void Open() {
        const wchar_t* wide = replay::LogPath();
        if (wide == nullptr || wide[0] == L'\0') {
            _snprintf_s(source_, sizeof source_, _TRUNCATE, "unresolved");
            return;
        }
        WideCharToMultiByte(CP_ACP, 0, wide, -1, path_, sizeof path_, nullptr, nullptr);
        _snprintf_s(source_, sizeof source_, _TRUNCATE, "replay::LogPath()");
    }

    const char* Path() const { return path_; }
    const char* Source() const { return source_; }
    unsigned long long Lines() const { return lines_; }

    void Poll(LogFacts& facts) {
        if (path_[0] == '\0') {
            return;
        }
        HANDLE fh = CreateFileA(path_, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) {
            return;
        }
        LARGE_INTEGER size = {0};
        if (GetFileSizeEx(fh, &size) && size.QuadPart < offset_) {
            offset_ = 0;          // the file was rotated under us; start again rather than skip
            partialN_ = 0;
        }
        LARGE_INTEGER at;
        at.QuadPart = offset_;
        SetFilePointerEx(fh, at, nullptr, FILE_BEGIN);
        char buf[8192];
        DWORD got = 0;
        while (ReadFile(fh, buf, sizeof buf, &got, nullptr) && got > 0) {
            offset_ += got;
            for (DWORD i = 0; i < got; ++i) {
                const char c = buf[i];
                if (c == '\n' || partialN_ + 1 >= sizeof partial_) {
                    partial_[partialN_] = '\0';
                    if (partialN_ > 0) {
                        ++lines_;
                        facts.OnLine(partial_);
                    }
                    partialN_ = 0;
                    continue;
                }
                if (c != '\r') {
                    partial_[partialN_++] = c;
                }
            }
        }
        CloseHandle(fh);
    }

private:
    char path_[MAX_PATH] = {0};
    char source_[64] = {0};
    LONGLONG offset_ = 0;
    char partial_[1024] = {0};
    size_t partialN_ = 0;
    unsigned long long lines_ = 0;
};

LogTail g_tail;

// ---------------------------------------------------------------------------------------------
// The status reads. Each is the module's OWN published accessor; nothing here recomputes a value
// the product already keeps.
// ---------------------------------------------------------------------------------------------
void SampleCsRegister(StatusFacts& s) {
    const replay::ui::csregister::Report r = replay::ui::csregister::Status_();
    s.csAsked = true;
    s.csRunning = r.status == replay::ui::csregister::Status::Running;
    strncpy_s(s.csStatus, sizeof s.csStatus, replay::ui::csregister::StatusName(r.status), _TRUNCATE);
    s.csInvocations = r.invocations;
    s.csRegistrationInvocations = r.registrationInvocations;
    s.csStaleInvocations = r.staleInvocations;
    s.csSpaceChanges = r.spaceChanges;
    s.csRegistrations = r.registrations;
    s.csDeferrals = r.deferrals;
    s.csMarshalledDraws = r.marshalledDraws;
    // ARCH-13 S2.3 makes ownerTicks the panel-drawn row's denominator and its only censoring
    // predicate. W2-F R3 publishes it at both sample points, so a build in which the field is
    // absent does not compile rather than reporting a confident zero.
    s.csOwnerTicks = r.ownerTicks;
    s.csOwnerTicksKnown = true;
    s.csCoalescedInvocations = r.coalescedInvocations;
    s.csPendingInvocations = r.pendingInvocations;
    s.csStaleDetected = r.staleDetected;
    s.csSpacesSeen = r.spacesSeen;
    s.csOwnerThreadId = r.mainThreadId;
    s.csCallbackThreadId = r.lastCallbackThreadId;

    // ★ THE ROW-2.5 COUNTERS (ARCH-15 S4.3), read off the same Report as everything above and at
    // the same instant. Same ruling as ownerTicks: a build whose Report lacks one of these fails
    // to compile rather than writing a confident zero, and nothing here re-derives a count from a
    // log line. `inFrameDraws` is the coverage numerator; marshalledDraws feeds no term (S4.1).
    s.csInFrameKnown = true;
    // ★ THE STAGE-0 FRAME BYTE (W2-F C1-R2). Read off the same Report, at the same instant, as a
    // struct read: a build whose Report lacks one of these three fails to compile rather than
    // leaving ui-gui-frame-open-at-stage0 to report a zero nobody measured. `Samples` counts the
    // ticks the byte could be ASKED on, so samples == 0 is could-not-ask and never "it read 0".
    s.csFrameOpenSampled = true;
    s.csOwnerTicksSampled = r.frameOpenAtOwnerTickSamples;
    s.csFrameOpenAtOwnerTick = r.frameOpenAtOwnerTickNonZero;
    s.csFrameOpenLastNonZeroTick = r.frameOpenAtOwnerTickLastNonZeroTick;
    s.csOffThreadSiteEntries = r.offThreadSiteEntries;
    s.csInFrameInstalled = r.inframeInstalled;
    strncpy_s(s.csInFrameStatus, sizeof s.csInFrameStatus,
              replay::HookStatusName(r.inframeStatus), _TRUNCATE);
    s.csFrameSiteEntries = r.frameSiteEntries;
    s.csInFrameDraws = r.inFrameDraws;
    s.csRefusedDraws = r.refusedDraws;
    s.csPanelClosedEntries = r.panelClosedEntries;
    s.csFallbackEntries = r.fallbackEntries;
    s.csRefusedNoFrameByte = r.refusedNoFrameByte;
    s.csRefusedNoLatch = r.refusedNoLatch;
    s.csRefusedNoContext = r.refusedNoContext;
    s.csRefusedByFacade = r.refusedByFacade;
    // The two halves the product computes that aggregate FROM, plus the facade's
    // own account of the last refusal, read off the SAME Report at the same instant. A build whose
    // Report lacks one of these fails to compile rather than letting the record carry a zero.
    s.csRefusedNoDraw = r.refusedNoDraw;
    s.csRefusedBeginFalse = r.refusedBeginFalse;
    strncpy_s(s.csBeginRefusal, sizeof s.csBeginRefusal,
              replay::ui::inframe::BeginRefusalName(r.beginRefusal), _TRUNCATE);
    s.csFacadeBoundEntries = r.facadeBoundEntries;
    s.csFacadeEntryCount = r.facadeEntryCount;
    s.csEnableStampsKnown = true;
    s.csEnableStamps = r.enableStamps;

    // ★ ARCH-20 S5 -- the `ui-gui-begin-gate` block, off the SAME Report, at the same instant, on
    // the owner thread, as one struct read. Without this copy the producer published all twenty
    // fields and the consumer read none of them: the row answered could-not-ask on BOTH runs of
    // L5 #6 and S6.3(a) had no `renderedTotalVtxCount` on either side to subtract (L5-RUN-6 s2/s3).
    probe_replay::CopyGuiGateFacts(r, s);

    // The `[gate ...]` counters, same read. Diagnostic() has printed them since W2-F-C2 and no
    // artifact carried them; done.json does now. The hover SOURCE travels with the hover count
    // because 0 with `hoversource=none` is could-not-ask, not a measurement of "never hovered".
    s.csGateKnown = true;
    s.csGateScopeEnabled = r.gateScopeEnabled;
    s.csGateUnresolved = r.gateUnresolved;
    s.csGateAlreadyOpen = r.gateAlreadyOpen;
    s.csGateScopeEntries = r.suppressScopeEntries;
    s.csGateScopeRestores = r.suppressScopeRestores;
    s.csGateScopeChanged = r.suppressScopeChanged;
    s.csGateSetterUnmapped = r.suppressSetterUnmapped;
    s.csPanelHoveredFrames = r.panelHoveredFrames;
    s.csDrawNanosTotal = r.drawNanosTotal;
    s.csDrawNanosMax = r.drawNanosMax;
    s.csFirstDrawNanos = r.firstDrawNanos;
    s.csDrawNanosBudget = r.drawNanosBudget;
    // The split of that maximum, off the same Report and the same instant. Without it the record
    // carries a number and no way to attribute it, which is how L5 #10 ended with a 257 000 ns
    // tail nobody could place.
    CopyDrawPhaseFacts(r, s);
    strncpy_s(s.csPanelHoverSource, sizeof s.csPanelHoverSource,
              replay::ui::inframe::PanelHoverSourceName(), _TRUNCATE);

    // The same two ids as W2-F PUBLISHES them, off the frozen suffix Diagnostic() carries on every
    // line. Taken from the LAST occurrence: the suffix is appended after the sentence, and a
    // sentence that quoted the token must not answer for the module's own trailer.
    s.csThreadsPublished = false;
    const char* diag = replay::ui::csregister::Diagnostic();
    // Diagnostic() can answer with a line that names ITSELF a refusal. It is
    // matched by name at the head of the line, nothing on it is parsed, and the word travels to
    // the record and to the rows so an operator grades the module's own refusal, not a zero.
    strncpy_s(s.csDiagRefusal, sizeof s.csDiagRefusal,
              probe_replay::DiagRefusalWord(probe_replay::ClassifyDiagnostic(diag)), _TRUNCATE);
    if (diag != nullptr) {
        const char* at = nullptr;
        for (const char* p = strstr(diag, " [threads owner="); p != nullptr;
             p = strstr(p + 1, " [threads owner=")) {
            at = p;
        }
        unsigned long owner = 0, callback = 0;
        if (at != nullptr && sscanf_s(at, " [threads owner=%lu callback=%lu]", &owner, &callback) == 2) {
            s.csThreadsPublished = true;
            s.csOwnerThreadIdPublished = static_cast<uint32_t>(owner);
            s.csCallbackThreadIdPublished = static_cast<uint32_t>(callback);
        }
    }

    // The row-2.5 counters as the module PUBLISHES them, parsed off the same line and compared
    // with the struct read above. `draws=` appears in two groups meaning two counters, so the
    // parse keys on the group opener -- see ParseInFrameLine. A disagreement is carried to the
    // reader, which censors the row; it is never resolved here in favour of either reading.
    probe_replay::InFrameLine inframeLine;
    if (probe_replay::ParseInFrameLine(diag, inframeLine)) {
        s.csInFramePublished = true;
        s.csInFrameLineAgrees = probe_replay::InFrameLineAgrees(inframeLine, s);
    }

    // The stage-0 frame-byte sample, cross-checked the same way. Two groups sit side by side on
    // this line and both carry bare decimal counters, so the parse keys on its own group opener.
    probe_replay::Stage0Line stage0Line;
    if (probe_replay::ParseStage0Line(diag, stage0Line)) {
        s.csStage0Published = true;
        s.csStage0LineAgrees = stage0Line.samples == s.csOwnerTicksSampled &&
                               stage0Line.nonzero == s.csFrameOpenAtOwnerTick &&
                               stage0Line.lastTick == s.csFrameOpenLastNonZeroTick;
    }

    // The facade group, the third on this line, keyed on its own opener for the same reason as the
    // other two. Under a refusal the line carries no group and this parse finds nothing -- which
    // stays `not published`, and never becomes five zeros.
    probe_replay::FacadeLine facadeLine;
    if (probe_replay::ParseFacadeLine(diag, facadeLine)) {
        s.csFacadePublished = true;
        s.csFacadeNoDrawPublished = facadeLine.nodraw;
        s.csFacadeBeginFalsePublished = facadeLine.beginfalse;
        strncpy_s(s.csFacadeReasonPublished, sizeof s.csFacadeReasonPublished, facadeLine.reason,
                  _TRUNCATE);
        s.csFacadeBoundPublished = facadeLine.bound;
        s.csFacadeEntryCountPublished = facadeLine.entryCount;
    }

    // The baseline the panel-drawn row differences against, taken ONCE, at the first sample on
    // which the level had been reached. Arm-independent on purpose: the level-load answer has one
    // holder and it is the [session] line, whichever arm is running.
    const bool levelNow = probe_replay::LevelReached(g_log);
    if (!s.csDrawBaselineKnown && levelNow) {
        s.csDrawBaselineKnown = true;
        s.csDrawsAtLevel = r.marshalledDraws;
        s.csInvocationsAtLevel = r.invocations;
        s.csOwnerTicksAtLevel = r.ownerTicks;
    }

    // The row-2.5 window baseline: ALL FOUR counters at ONE instant, at the LATER of the level-load
    // stamp and the panel's closed->open stamp (ARCH-15 S4.3 -- one counter at one stamp and
    // another at a different stamp is not a window). These two flags are the observer worker's
    // alone; SampleCsRegister runs on that one thread and nowhere else. Which stamp was later is
    // read off which condition was already true when the other arrived; when both first show up on
    // the same sample there is nothing to order them by and the record says so.
    static bool levelSeenBefore = false;
    static bool enableSeenBefore = false;
    const bool enableNow = r.enableStamps > 0;
    if (!s.csInFrameBaselineKnown && levelNow && enableNow) {
        const char* which = "level+enable (both first observed on one sample)";
        if (levelSeenBefore && !enableSeenBefore) {
            which = "enable";
        } else if (enableSeenBefore && !levelSeenBefore) {
            which = "level";
        }
        s.csInFrameBaselineKnown = true;
        strncpy_s(s.csInFrameBaselineStamp, sizeof s.csInFrameBaselineStamp, which, _TRUNCATE);
        s.csFrameSiteEntriesAtBaseline = r.frameSiteEntries;
        s.csInFrameDrawsAtBaseline = r.inFrameDraws;
        s.csRefusedDrawsAtBaseline = r.refusedDraws;
        s.csPanelClosedEntriesAtBaseline = r.panelClosedEntries;
    }
    levelSeenBefore = levelSeenBefore || levelNow;
    enableSeenBefore = enableSeenBefore || enableNow;
}

// W2-E's own probe, read on the worker from what the tick left behind. Its header states it makes
// no engine call and no write, which is why asking it every tick costs the launch nothing.
// L2-RUN-2 F5. The entity surface, read HERE, on the tick the row is scored on. The observer used
// to leave this row to the log tail, and the log tail is a history: L2 #2 filed asked-fail off a
// refusal the session had already superseded 1.2 s later. A row whose subject is a state samples
// the state.
void SampleEntity(StatusFacts& s) {
    const replay::IEntity* e = g_services != nullptr ? g_services->entity : nullptr;
    if (e == nullptr) {
        // Deliberately does NOT clear a previous sample: services can go away at teardown and an
        // erased sample would read as "never asked" on a launch that did ask.
        return;
    }
    s.entitySampled = true;
    s.entityAvailable = e->Available();
    const char* why = e->Diagnostic();
    _snprintf_s(s.entityDiagnostic, sizeof s.entityDiagnostic, _TRUNCATE, "%s",
                why != nullptr ? why : "(the entity surface returned no diagnostic)");
}

// L2-RUN-2 F4. W1-F's loopback-only hook, asked in-process rather than parsed out of its own log
// line: bootstrap/inbound.cpp is compiled into this binary, so the verdict scored is the product's
// own Classify over the product's own SelfView and there is no second copy of the rule to drift.
void SampleInbound(StatusFacts& s) {
    const replay::inbound::VerdictInput in = replay::inbound::SelfView();
    const replay::inbound::Verdict v = replay::inbound::Classify(in);
    s.inboundSampled = true;
    _snprintf_s(s.inboundVerdict, sizeof s.inboundVerdict, _TRUNCATE, "%s",
                replay::inbound::VerdictName(v));
    s.inboundArmed = in.armed;
    s.inboundEndpointsRead = in.endpointsRead;
    s.inboundRewritten = in.rewritten;
    s.inboundAlreadyLoopback = in.alreadyLoopback;
    s.inboundBoundNonLoopback = in.boundNonLoopback;
}

void SampleCameraProbe(StatusFacts& s) {
    namespace cc = replay::camera_control;
    const LONG last = InterlockedCompareExchange(&g_cameraProbeLast, -1, -1);
    if (last < 0) {
        return;
    }
    s.cameraProbed = true;
    s.cameraProbeReadyEver = InterlockedCompareExchange(&g_cameraProbeReadyEver, 0, 0) != 0;
    strncpy_s(s.cameraProbeResult, sizeof s.cameraProbeResult,
              cc::RigProbeName(static_cast<cc::RigProbe>(last)), _TRUNCATE);
    const char* diag = cc::Diagnostic();
    strncpy_s(s.cameraProbeDiagnostic, sizeof s.cameraProbeDiagnostic,
              diag != nullptr ? diag : "", _TRUNCATE);
}

void SampleWiretap(StatusFacts& s) {
    // Available() answers whether the tap COULD work, not whether anything is registered on it --
    // Row 8 passed with 0 of 37 registrations. capture.cpp is compiled into this
    // probe, so the count comes from the module itself rather than from a log line.
    //
    // It is read BEFORE the wiretap, and unconditionally. On L5 run 3 this function returned on the
    // null wiretap and left all five counts at zero, so the row could not have scored them however
    // it was written -- while capture.cpp in the same process had logged `36 of 38 message taps
    // registered`. TapRegistrationNow() does not travel through services->wiretap and reads one
    // atomic word from any thread, so there is nothing here for a null tap to gate.
    const replay::capture::TapRegistration reg = replay::capture::TapRegistrationNow();
    s.tapRegStarted = reg.started;
    s.tapRegistered = reg.registered;
    s.tapDeferred = reg.deferred;
    s.tapGivenUp = reg.givenUp;
    s.tapUnavailable = reg.unavailable;
    s.tapTotal = reg.total;

    const replay::IWiretap* tap = g_services != nullptr ? g_services->wiretap : nullptr;
    s.tapPresent = tap != nullptr;
    if (tap == nullptr) {
        return;
    }
    s.tapAvailable = tap->Available();
    const char* diag = tap->Diagnostic();
    strncpy_s(s.tapDiagnostic, sizeof s.tapDiagnostic, diag != nullptr ? diag : "", _TRUNCATE);
}

void SampleTransportAndAudio(StatusFacts& s) {
    const replay::IReplayTransport* t = g_services != nullptr ? g_services->transport : nullptr;
    s.transportPresent = t != nullptr;
    s.durationUs = t != nullptr ? t->DurationUs() : 0;

    replay::IAudioRate* a = g_services != nullptr ? g_services->audioRate : nullptr;
    s.audioPresent = a != nullptr;
    if (a == nullptr) {
        return;
    }
    s.audioAvailable = a->Available();
    const char* diag = a->Diagnostic();
    strncpy_s(s.audioDiagnostic, sizeof s.audioDiagnostic, diag != nullptr ? diag : "", _TRUNCATE);
    const char* label = a->RateLabel();
    strncpy_s(s.audioRateLabel, sizeof s.audioRateLabel, label != nullptr ? label : "", _TRUNCATE);
}

// W2-P3, L3 #11 F3. The module publishes what its last apply DECIDED about every body track
// (playback.h:211); nothing here decodes anything or keeps its own idea of the format.
//
// The poll that drove the MOST poses is kept, whole -- one coherent tick, never a maximum
// assembled out of five different ones. A session that never installed keeps the tick that walked
// the most tracks instead, so the row can still say what the applier decided about them.
void SampleBodyApply(StatusFacts& s) {
    const replay::playback::ApplyStats a = replay::playback::ApplyStatsSnapshot();
    ++s.bodyApplyPolls;
    const unsigned tracks = a.bodies_installed + a.bodies_absent + a.bodies_unhandled +
                            a.bodies_locally_controlled + a.bodies_install_failed;
    const bool better = !s.bodyApplySampled || a.bodies_installed > s.bodyInstalled ||
                        (a.bodies_installed == s.bodyInstalled &&
                         tracks > BodyTracksWalked(s));
    if (!better) {
        return;
    }
    s.bodyApplySampled = true;
    s.bodyInstalled = a.bodies_installed;
    s.bodyAbsent = a.bodies_absent;
    s.bodyUnhandled = a.bodies_unhandled;
    s.bodyLocallyControlled = a.bodies_locally_controlled;
    s.bodyInstallFailed = a.bodies_install_failed;
    s.bodyOwnerClaimed = a.bodies_owner_claimed;
    s.bodyReadBack = a.bodies_read_back;
    s.bodyInstalledUnread = a.bodies_installed_unread;
    s.bodyIdentityRefused = a.bodies_install_identity_refused;
}

// L3-RUN-7 F3. S_GAMESTATE and S_PLAYER_STATS are on disk and nothing on this arm asked about
// either. The read goes through replay::DemoReader -- the PRODUCT's own reader, linked into this
// binary -- over a SECOND read-only handle on the file the session loaded (reader.cpp opens with
// FILE_SHARE_READ | FILE_SHARE_WRITE, so the session's handle is not disturbed and nothing here
// writes anything). Every constant it uses is the format's own, so a checker with its own idea of
// the layout cannot certify a file the loader would refuse.
//
// It runs ONCE: the first attempt records either a scan or the reason there is none, and neither
// is retried. Until a file loads it records nothing at all -- an unloaded session is not a refusal.
void SampleDemoStreams(StatusFacts& s) {
    if (s.demoScanned || s.demoScanRefusal[0] != '\0') {
        return;
    }
    if (g_log.uiLoadedPath[0] == '\0') {
        return;
    }
    strncpy_s(s.demoPath, sizeof s.demoPath, g_log.uiLoadedPath, _TRUNCATE);
    wchar_t wide[512];
    if (MultiByteToWideChar(CP_UTF8, 0, s.demoPath, -1, wide, 512) == 0) {
        _snprintf_s(s.demoScanRefusal, sizeof s.demoScanRefusal, _TRUNCATE,
                    "the loaded path '%s' is not representable as a wide path", s.demoPath);
        return;
    }
    replay::DemoReader r;
    if (!r.Open(wide)) {
        const char* why = r.RefusalReason();
        _snprintf_s(s.demoScanRefusal, sizeof s.demoScanRefusal, _TRUNCATE,
                    "the product's reader refused '%s': %s", s.demoPath,
                    why != nullptr && why[0] != '\0' ? why : "it gave no reason");
        return;
    }
    s.gsPresentAll = replay::kGameStatePresentAll;
    s.psFillAll = replay::kPlayerStatsFillAll;
    unsigned presentAlways = 0xFFFFFFFFu;
    unsigned long long phaseBits = 0;
    unsigned long long runPhase = 0;
    bool runOpen = false;
    float runTimer = 0.0f;
    int runDir = 0;
    replay::PlayerStatsCursor cursor = {};
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        replay::DecodedChapter c = {};
        if (!r.DecodeChapter(i, c)) {
            continue;
        }
        // Each chapter is consumed before the next is decoded: the reader alternates two buffers
        // and a third decode would overwrite what a collected pointer still pointed at.
        for (unsigned b = 0; b < c.streamCount; ++b) {
            const replay::StreamView& v = c.streams[b];
            if (v.id == replay::kStreamGameState &&
                v.bytes % sizeof(replay::GameStateRecordDisk) == 0) {
                const unsigned gn =
                    v.bytes / static_cast<unsigned>(sizeof(replay::GameStateRecordDisk));
                for (unsigned g = 0; g < gn; ++g) {
                    replay::GameStateRecordDisk gd = {};
                    memcpy(&gd, v.records + static_cast<size_t>(g) * sizeof(gd), sizeof(gd));
                    ++s.gsRecords;
                    s.gsPresentUnion |= gd.present;
                    presentAlways &= gd.present;
                    if ((gd.present & replay::kGsPhaseSymbol) != 0 && gd.phaseSymbol != 0 &&
                        gd.phase < 64) {
                        phaseBits |= 1ull << gd.phase;
                    }
                    // Monotone WITHIN a phase. A round reset opens a new run rather than counting
                    // as a reversal, and a countdown is as monotone as a count-up; what is neither
                    // is a timer that moves both ways while the phase symbol has not changed.
                    if ((gd.present & replay::kGsGameTimer) == 0) {
                        continue;
                    }
                    if (!runOpen || gd.phaseSymbol != runPhase) {
                        runOpen = true;
                        runPhase = gd.phaseSymbol;
                        runTimer = gd.gameTimer;
                        runDir = 0;
                        continue;
                    }
                    const int dir = gd.gameTimer > runTimer ? 1 : (gd.gameTimer < runTimer ? -1 : 0);
                    if (dir != 0) {
                        if (runDir != 0 && dir != runDir) {
                            ++s.gsTimerReversals;
                        }
                        runDir = dir;
                        runTimer = gd.gameTimer;
                    }
                }
            }
            if (v.id == replay::kStreamPlayerStats &&
                v.bytes % sizeof(replay::PlayerStatsRecordDisk) == 0) {
                const unsigned pn =
                    v.bytes / static_cast<unsigned>(sizeof(replay::PlayerStatsRecordDisk));
                for (unsigned k = 0; k < pn; ++k) {
                    replay::PlayerStatsRecordDisk pd = {};
                    memcpy(&pd, v.records + static_cast<size_t>(k) * sizeof(pd), sizeof(pd));
                    ++s.psRecords;
                    s.psFillUnion |= pd.fill;
                    // The monotone rule has exactly one implementation and this is it -- the same
                    // function `echodemo verify` walks the stream with.
                    if (!replay::PlayerStatsNoteTime(cursor, pd.xplatformId, pd.engineTimeUs)) {
                        ++s.psTimeBacksteps;
                    }
                }
            }
        }
    }
    r.Close();
    s.psUsers = cursor.n;
    s.gsPresentAlways = s.gsRecords > 0 ? (presentAlways & s.gsPresentAll) : 0u;
    s.gsUndefinedBits = s.gsPresentUnion & ~s.gsPresentAll;
    for (unsigned p = 0; p < 64; ++p) {
        if (((phaseBits >> p) & 1ull) != 0) {
            ++s.gsPhasesSeen;
        }
    }
    s.demoScanned = true;
}

// W1-C, READ ONLY. src/bootstrap/sc7.cpp writes this record; the observer scores what it finds and
// writes nothing to this name. The `writer` field is how a second writer would be caught, so it is
// reported rather than merely checked.
void SampleSc7(StatusFacts& s) {
    if (g_artifactDir[0] == '\0') {
        return;
    }
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\sc7_text_diff.json", g_artifactDir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "rb") != 0 || fh == nullptr) {
        return;
    }
    static char body[65536];
    const size_t n = fread(body, 1, sizeof body - 1, fh);
    fclose(fh);
    body[n] = '\0';
    s.sc7Present = true;
    s.sc7Refused = strstr(body, "\"refused\"") != nullptr;
    probe_replay::WordAfter(body, "\"verdict\": \"", s.sc7Verdict, sizeof s.sc7Verdict, "\"");
    if (const char* at = strstr(body, "\"declared_sites\":")) {
        s.sc7DeclaredSites = _atoi64(at + strlen("\"declared_sites\":"));
    }
    if (const char* at = strstr(body, "\"text_bytes\":")) {
        s.sc7TextBytes = _atoi64(at + strlen("\"text_bytes\":"));
    }
    // How many ledger rows the build declares, counted out of the record's own array. sc7.cpp OMITS
    // the key on a refusal rather than writing an empty one, so absent stays -1 and the row reports
    // could-not-ask instead of scoring declared_sites against a number nobody wrote.
    if (const char* at = strstr(body, "\"expected_rows\": [")) {
        const char* p = at + strlen("\"expected_rows\": [");
        long long quotes = 0;
        for (; *p != '\0' && *p != ']'; ++p) {
            if (*p == '"') {
                ++quotes;
            }
        }
        if (*p == ']') {
            s.sc7ExpectedRows = quotes / 2;
        }
    }
}

// W2-A, READ ONLY -- two facts from two sources, deliberately kept apart.
//
// The NAME and the file's presence answer `w2a-capture-stop-sentinel`, a question about the document
// on disk. The name is composed by the product's own StopSentinelNameFor, so this observer cannot
// report a name the capture module would not have written.
//
// The stop FACTS come from LastStopFacts: the const snapshot the capture module publishes from the
// SWEPT path, under its own lock, with every string already copied. ENG-W2R-STOPFACTS deleted the
// fopen_s read of that JSON in favour of this call, so there is no stdio open here for check.ps1's
// observer-object-writers census to classify, no 16 KiB parse buffer to truncate a document in, and
// the in-process answer and the on-disk answer are the same close by construction. False means NO
// CLOSE HAS PUBLISHED YET, which is not the same fact as a close that reached its tables and found
// nothing -- so it is could-not-ask, in those words.
void SampleStopSentinel(StatusFacts& s) {
    replay::capture::StopFactsSnapshot facts;
    if (replay::capture::LastStopFacts(facts)) {
        probe_replay::FillFromStopFacts(s, facts, replay::MarkerSourceName(facts.markerSource));
    } else {
        _snprintf_s(s.sentinelReadReason, sizeof s.sentinelReadReason, _TRUNCATE,
                    "no capture close has published its stop facts in this process yet, so there is "
                    "no marker choice and no round table for this observer to read -- which is not "
                    "the same fact as a close that reached them and found nothing");
    }

    wchar_t name[128] = {0};
    if (!replay::capture::StopSentinelNameFor(replay::capture::LaunchFileTime(), GetCurrentProcessId(),
                                              name, 128)) {
        return;
    }
    s.stopSentinelNameKnown = true;
    WideCharToMultiByte(CP_ACP, 0, name, -1, s.stopSentinelName, sizeof s.stopSentinelName, nullptr,
                        nullptr);
    const wchar_t* dir = replay::StateDir();
    if (dir == nullptr || dir[0] == L'\0') {
        return;
    }
    WideCharToMultiByte(CP_ACP, 0, dir, -1, s.stopSentinelDir, sizeof s.stopSentinelDir, nullptr,
                        nullptr);
    wchar_t full[replay::kPathMax];
    _snwprintf_s(full, replay::kPathMax, _TRUNCATE, L"%s\\%s", dir, name);
    s.stopSentinelPresent = GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES;
}

void CopyWord(char* dst, size_t dstN, const char* src) {
    _snprintf_s(dst, dstN, _TRUNCATE, "%s", src != nullptr ? src : "");
}

// ---------------------------------------------------------------------------------------------
// The PLAYBACK battery. It drives the product's OWN transport interface -- Pause, Play(rate) -- and
// measures through the product's own audio accessor. It reimplements no seek, no clock and no rate
// plan. PLAYBACK-IDLE never calls it: that arm samples nothing a human's seeking would perturb and
// never drives the transport itself (6.4).
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// THE TRANSPORT BATTERY'S DRIVER (RES-TRANSPORT-CMDS s5).
//
// Every step goes through control::dispatch::Submit and is collected with TakeResult -- the seam
// the console, the HTTP surface and the product's own launch load (control_module.cpp
// SubmitLaunchLoad) all push onto. It is NOT IReplayTransport: a raw Play(rate) skips ui::SetRate,
// the only place the engine's fast-forward ceiling is enforced, so a probe on that path reports
// success on rates the panel refuses and leaves the panel's own g.rate stale.
// ---------------------------------------------------------------------------------------------
namespace ctl = replay::control;
namespace disp = replay::control::dispatch;
namespace cc = replay::camera_control;

// The residue battery publishes no fire counter, so what the battery differences is the SUM of its
// EFFECT counters. residue.h names them one by one; adding a field there without adding it here
// understates the sum, which is why they are spelled out rather than memcmp'd.
uint64_t ResidueEffects(bool* armed, unsigned long long* ciResets) {
    const replay::playback::residue::ResidueStats& st = replay::playback::residue::Stats();
    if (armed != nullptr) {
        *armed = st.armed;
    }
    // L3-RUN-6 F5. The sum below cannot say which effect moved, so the counter the row names is
    // read out beside it rather than being recovered from a total that cannot be decomposed.
    if (ciResets != nullptr) {
        *ciResets = st.ciResets;
    }
    return st.ciResets + st.warpRequests + st.timeSnaps + st.grabClears + st.motionBitsRaised +
           st.wakesCalled + st.hysteresisCleared + st.worldOriginShifts + st.clockWrapsRepaired +
           st.clockBackwardsSeen + st.orderingViolations;
}

// W3-A. `fires` counts RAISES -- one per OnDiscontinuity delivery -- so it
// answers the question the effect sum above cannot: whether the battery fired at all. The
// Seek bucket is read beside it because every jump this battery commands is a Seek to the
// residue packet, and `lastFireCause` alone cannot say which of six jumps produced it.
unsigned long long ResidueFires(unsigned long long* seekFires, int* cause, int* causeCount) {
    namespace res = replay::playback::residue;
    namespace pb = replay::playback;  // Cause lives in playback/core/discontinuity.h, not in residue
    const res::ResidueStats& st = res::Stats();
    if (seekFires != nullptr) {
        *seekFires = st.firesByCause[static_cast<size_t>(pb::Cause::Seek)];
    }
    if (cause != nullptr) { *cause = static_cast<int>(st.lastFireCause); }
    if (causeCount != nullptr) { *causeCount = static_cast<int>(pb::Cause::Count); }
    return st.fires;
}

bool WaitForResult(uint64_t seq, disp::Result* out, double* waitMs) {
    const double t0 = SinceStartMs();
    for (int i = 0; i < 300; ++i) {  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF -- 300 polls of 10 ms is this wait's own budget, not an offset
        if (disp::TakeResult(seq, out)) {
            *waitMs = SinceStartMs() - t0;
            return true;
        }
        Sleep(10);
    }
    *waitMs = SinceStartMs() - t0;
    return false;
}

// `goto next` / `goto prev` and `seek +/-<us>`, in the Command shapes control::command.h
// documents: flags, never sentinel indices, because 0 and -1 are real bookmark numbers and a
// zero delta is a real (if useless) nudge.
ctl::Command GotoStep(bool forward) {
    ctl::Command c;
    c.id = ctl::CommandId::Goto;
    c.gotoNext = forward;
    c.gotoPrev = !forward;
    return c;
}

ctl::Command CamLookAngles(double yawDeg, double pitchDeg, bool absolute) {
    ctl::Command c;
    c.id = ctl::CommandId::CamLook;
    c.camVec[0] = static_cast<float>(yawDeg);
    c.camVec[1] = static_cast<float>(pitchDeg);
    c.camAbsolute = absolute;
    return c;
}

ctl::Command CamLookAtSelector(int64_t selector) {
    ctl::Command c;
    c.id = ctl::CommandId::CamLook;
    c.camLookAt = true;
    c.camAbsolute = true;
    c.whole = selector;
    return c;
}

ctl::Command RelSeek(int64_t deltaUs) {
    ctl::Command c;
    c.id = ctl::CommandId::Seek;
    c.whole = deltaUs;
    c.seekIsRelative = true;
    return c;
}

void RunStep(StatusFacts& s, probe_replay::Verb v, const ctl::Command& cmd, const char* asked,
             int64_t target) {
    probe_replay::TransportStep& st = s.steps[static_cast<size_t>(v)];
    st.attempted = true;
    st.askedRate = cmd.rate;
    st.target = target;
    _snprintf_s(st.asked, sizeof st.asked, _TRUNCATE, "%s", asked);

    disp::Snapshot before;
    disp::ReadSnapshot(&before);
    st.playheadBefore = before.playheadUs;
    st.residueBefore = ResidueEffects(&st.residueArmed, &st.residueCiResetsBefore);
    st.residueFiresBefore = ResidueFires(&st.residueSeekFiresBefore, nullptr, nullptr);
    ++s.stepsAttempted;

    disp::Result res = disp::Submit(cmd);
    st.submitted = true;
    if (res.seq == 0) {
        // Answered at submission: an immediate command, or a refusal the queue never took.
        st.answered = true;
        st.ok = res.ok;
    } else {
        disp::Result done;
        double waited = 0.0;
        if (WaitForResult(res.seq, &done, &waited)) {
            st.answered = true;
            st.ok = done.ok;
            res = done;
        }
        st.waitMs = waited;
        // The snapshot the surfaces read is published by dispatch::Tick, so it can still be the
        // one from before this command ran. Wait for the tick that carried our sequence number.
        for (int i = 0; i < 100; ++i) {
            disp::Snapshot now;
            disp::ReadSnapshot(&now);
            if (now.lastSeq >= res.seq) {
                break;
            }
            Sleep(10);
        }
    }
    _snprintf_s(st.said, sizeof st.said, _TRUNCATE, "%s", res.text);
    if (st.answered) {
        ++s.stepsAnswered;
    }
    // The frame loop consumes the discontinuity and fires the residue battery on the tick AFTER
    // the command ran (playback_module.cpp TakeDiscontinuity), so the counters are read one beat
    // later than the result.
    Sleep(60);
    disp::Snapshot after;
    disp::ReadSnapshot(&after);
    st.readBack = true;
    st.state = after.transportState;
    st.rate = after.rate;
    st.playheadAfter = after.playheadUs;
    // ENG-W2B-STEP2 `d77cd0b`. The SIGNED recorded ticks the transport says it just consumed. Read
    // from the transport rather than differenced off the snapshot: a recorded tick table is
    // non-decreasing, so a stalled source records the same boundary twice and a step that really
    // consumed a tick can leave the playhead on the microsecond it started from. Absent transport
    // leaves stepTicksRead false, which the rows report rather than reading as a count.
    if (g_services != nullptr && g_services->transport != nullptr) {
        st.stepTicksRead = true;
        st.lastStepTicks = g_services->transport->LastStepTicks();
    }
    st.residueAfter = ResidueEffects(nullptr, &st.residueCiResetsAfter);
    st.residueFiresAfter = ResidueFires(&st.residueSeekFiresAfter, &st.residueCauseAfter,
                                        &st.residueCauseCount);
}

ctl::Command Simple(ctl::CommandId id) {
    ctl::Command c;
    c.id = id;
    return c;
}

ctl::Command SpeedCommand(double rate) {
    ctl::Command c;
    c.id = ctl::CommandId::Speed;
    c.rate = rate;
    return c;
}

ctl::Command WholeCommand(ctl::CommandId id, int64_t whole) {
    ctl::Command c;
    c.id = id;
    c.whole = whole;
    return c;
}

void RunBattery(StatusFacts& s) {
    replay::IReplayTransport* t = g_services != nullptr ? g_services->transport : nullptr;
    replay::IAudioRate* a = g_services != nullptr ? g_services->audioRate : nullptr;
    if (t == nullptr || t->DurationUs() == 0) {
        return;
    }
    g_batteryRan = true;
    const uint64_t duration = t->DurationUs();

    // P10-seek-landing scores a seek's sentence against the playhead that read back,
    // and the interval separating "the neighbouring recorded tick" from "a false statement" is the
    // CAPTURE's sample spacing -- so it is asked of the transport that loaded the capture, and of
    // nothing else. Never a control constant (the one this replaced was deleted) and never a frame
    // period: a 30 Hz capture on a 90 Hz machine puts every correct landing outside a render frame.
    // A transport that will not publish a spacing leaves this 0, which the row reports as
    // could-not-ask rather than scoring a landing against a number nobody measured.
    int64_t tickSpacingUs = 0;
    s.tickSpacingUs =
        t->TickSpacingUs(&tickSpacingUs) ? static_cast<long long>(tickSpacingUs) : 0;
    // One measurement, two named consumers, assigned at this one site. p10-seek-landing's landing
    // tolerance and w2r-step-moves-by-ticks' step distance are the SAME fact about the capture;
    // deriving them separately is how a timeline comes to disagree with the thing it scrubs.
    s.seekLandingToleranceUs = s.tickSpacingUs;
    // The product's own "this transport counts nothing" value, copied in for the reason
    // stepNoTickToken is: observer_rows.h spells no product constant of its own, and 0 is a
    // COUNTED refusal rather than an absent count.
    s.stepTicksUncountedKnown = true;
    s.stepTicksUncounted = replay::kStepTicksUncounted;

    // The panel's own numbers, off the snapshot every surface reads. The ceiling is
    // 66666 / raw_frame_us (uiapi.h), so raw_frame_us comes back out of it; two of those is the
    // tolerance a landing is scored against, because the snapshot is published once per tick and a
    // read can be one tick stale. A ceiling of 0 is UNKNOWN and leaves the tolerance at 0, which
    // the rows report as could-not-ask rather than scoring against a number nobody measured.
    {
        disp::Snapshot snap;
        disp::ReadSnapshot(&snap);
        s.rateCeiling = snap.rateCeiling;
        s.roundsKnown = snap.roundsKnown;
        s.roundCount = snap.roundCount;
        if (snap.rateCeiling > 0.0f) {
            const double rawFrameUs = 66666.0 / static_cast<double>(snap.rateCeiling);
            s.seekToleranceUs = static_cast<int64_t>(rawFrameUs * 2.0) + 1;
        }
    }
    s.batteryDispatched = true;

    // RULING 4b: pause, then watch the view across a WALL-time span. The read is the product's own
    // (camera_control::SampleViewPose, W2-E `494f2e1` over W1-H `78a4502`) and the comparison is its
    // PoseMotionBetween -- nothing here re-derives a pose or a threshold. The transport state is
    // read at BOTH samples, because "paused" has to hold across the span, not just when it was
    // commanded.
    namespace cc = replay::camera_control;
    RunStep(s, probe_replay::Verb::Pause, Simple(ctl::CommandId::Pause), "pause", 0);
    s.pausedForView = probe_replay::StepFor(s, probe_replay::Verb::Pause).ok;
    // The rig binds ~1.8 s after the file loads, and this pair used to be taken
    // before that -- so `w2e-4b-view-moves-while-paused` and
    // `w2e-view-read-refusal-names-its-site` filed `not-bound` against a session that bound a
    // second later. The SAME bounded wait the cam battery uses, run here first; on an already-bound
    // rig it costs zero, and the cam battery's own call below then returns immediately.
    const probe_replay::CamBindWait viewBind = probe_replay::AwaitCameraBind(
        [] { return cc::Get() != nullptr; }, [](unsigned ms) { Sleep(ms); }, 4000, 100,
        [] { return StopRequested(); });
    (void)viewBind;
    cc::ViewPoseSample first{}, second{};
    const bool pausedAtFirst = t->State() == replay::TransportState::Pause;
    cc::SampleViewPose(cc::Get(), &first);
    StopAwareSleep(300);  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF -- a settle delay, not an offset
    cc::SampleViewPose(cc::Get(), &second);
    const bool pausedAtSecond = t->State() == replay::TransportState::Pause;
    s.viewPoseSampled = true;
    s.viewPausedAcross = pausedAtFirst && pausedAtSecond;
    s.viewSteeringDeclared = g_status.markerAttendedKnown && g_status.markerAttended;
    CopyWord(s.viewReadFirst, sizeof s.viewReadFirst, replay::camera::ViewPoseReadName(first.read));
    CopyWord(s.viewReadSecond, sizeof s.viewReadSecond, replay::camera::ViewPoseReadName(second.read));
    CopyWord(s.viewSource, sizeof s.viewSource, replay::ViewPoseSourceName(second.pose.source));
    s.viewGateKnown = second.pose.integratorArmedKnown;
    s.viewGateArmed = second.pose.integratorArmed;
    const cc::PoseMotion motion =
        cc::PoseMotionBetween(first, second, &s.viewMetres, &s.viewSeconds);
    CopyWord(s.viewMotion, sizeof s.viewMotion, cc::PoseMotionName(motion));
    // The enums are what the row scores; the words above are for its detail text.
    s.viewReadFirstCode = first.read;
    s.viewReadSecondCode = second.read;
    s.viewSourceCode = second.pose.source;
    s.viewMotionCode = motion;

    // ARCH-19 R3/R6, ENG-W2G-P17E S7. The cam-pose asks. Each landing is read back through the
    // PRODUCT's own sampler and decomposer -- E3 forbids inferring one from the Result bool -- and
    // the RELATIVE nudge goes first on purpose: its row scores the degenerate facing the engine
    // happens to hold, and an absolute look issued before it would have replaced that facing with
    // ours. The pose is handed back at the end, because these three asks are the only reason it
    // was taken and the rows after them read the rig as the launch left it.
    // L3-RUN-5 F2. The rig binds LATE -- 1.8 s after the capture loaded on the launch that found
    // this, by which time the transport battery had reached `goto prev` -- so one early read of
    // cc::Get() filed "null on this launch" for a session that bound successfully and whose rows 9
    // and 19 read that very bind. Wait for it, with a BOUND. 4000 ms is a little over twice the
    // 1.8 s the field measured; an expired bound is a fact the rows name (CamBindRefusal), not a
    // silence, and the battery cannot hang here waiting for a rig that will never come.
    const probe_replay::CamBindWait bind = probe_replay::AwaitCameraBind(
        []() { return cc::Get() != nullptr; },
        [](long long ms) { Sleep(static_cast<DWORD>(ms)); }, 4000, 100,
        [] { return StopRequested(); });
    s.camBindWaitKnown = true;
    s.camBindBound = bind.bound;
    s.camBindWaitedMs = bind.waitedMs;
    s.camBindBudgetMs = bind.budgetMs;
    if (bind.bound) {
        s.camPoseAsked = true;
        cc::ViewPoseSample poseBefore{};
        s.camFacingReadBefore =
            cc::SampleViewPose(cc::Get(), &poseBefore) == replay::camera::ViewPoseRead::Ok;
        if (s.camFacingReadBefore) {
            float yaw = 0.0f, pitch = 0.0f;
            s.camFacingVerticalBefore = !cc::DecomposeFacing(poseBefore.pose.quat, &yaw, &pitch);
            s.camYawBefore = yaw;
        }
        RunStep(s, probe_replay::Verb::CamLookRelative, CamLookAngles(10.0, 0.0, false),
                "cam look 10 0", 0);

        s.camLookYawAsked = 30.0;
        s.camLookPitchAsked = 0.0;
        RunStep(s, probe_replay::Verb::CamLookAbsolute,
                CamLookAngles(s.camLookYawAsked, s.camLookPitchAsked, true), "cam look to 30 0", 0);
        cc::ViewPoseSample poseAfter{};
        s.camFacingReadAfter =
            cc::SampleViewPose(cc::Get(), &poseAfter) == replay::camera::ViewPoseRead::Ok;
        if (s.camFacingReadAfter) {
            float yaw = 0.0f, pitch = 0.0f;
            s.camFacingVerticalAfter = !cc::DecomposeFacing(poseAfter.pose.quat, &yaw, &pitch);
            s.camYawAfter = yaw;
            s.camPitchAfter = pitch;
        }
        // p10-cam-9 half A -- the packing, once per run and before the aim. Ten selectors,
        // ten words that must all differ, and the two out-of-range asks. This is the
        // product's own arithmetic; nothing here re-derives the split.
        {
            int32_t words[10] = {0};
            bool allPacked = true;
            for (int32_t sel = 1; sel <= 10; ++sel) {
                uint8_t team = 0, slot = 0;
                if (!cc::PackSelector(sel, &team, &slot)) {
                    allPacked = false;
                    continue;
                }
                words[sel - 1] = cc::PackedSelectorWord(team, slot);
            }
            bool distinct = allPacked;
            for (int a = 0; a < 10 && distinct; ++a) {
                for (int b = a + 1; b < 10; ++b) {
                    if (words[a] == words[b]) {
                        distinct = false;
                        break;
                    }
                }
            }
            uint8_t t0 = 0, s0 = 0;
            s.camPackRefusesZero = !cc::PackSelector(0, &t0, &s0);
            s.camPackRefusesEleven = !cc::PackSelector(11, &t0, &s0);
            s.camPackAllPacked = allPacked;
            s.camPackDistinct = distinct;
            s.camPackKnown = true;
        }
        s.subjectPoseInstalledKnown = true;
        s.subjectPoseInstalled = cc::InstalledSubjectPose() != nullptr;

        // The selector this aim actually carried, recorded so p10-cam-9 can cross a `no player
        // occupies that selector` refusal against the roster's occupancy for THAT slot rather
        // than against a number assumed here.
        s.camLookAtSelector = 1;
        // A rig released across this dispatch comes back as dispatch's own
        // "the spectator rig has not checked out", which carries no composer word -- so the row
        // would score an asked-FAIL against the composer for a loss nobody in it caused. Ask at
        // both ends and let the row read the observation instead of the sentence.
        const bool rigBeforeLookAt = cc::Get() != nullptr;
        RunStep(s, probe_replay::Verb::CamLookAtSubject, CamLookAtSelector(1), "cam look at 1", 0);
        s.camRigAtLookAt = probe_replay::RigAcrossStep(rigBeforeLookAt, cc::Get() != nullptr);

        // Half B's oracle. The engine wrote ITS derivation of our selector to ci+0x26 when the
        // command set the mode; ObservedSelector reads that word back, and
        // RecordSelectorObservation is the product's own scoring step -- this probe does not
        // compare the two itself, it asks camera_control what its own comparison concluded.
        {
            uint8_t team = 0, slot = 0;
            int32_t packed = 0;
            uint16_t cameraIndex = 0;
            if (cc::PackSelector(1, &team, &slot)) {
                // Re-ASK for the rig here. `bind.bound` above is a fact about a moment several
                // commands ago; camera_module Releases the singleton on unbind and L3 #28 took a
                // level load in this very second, so Get() can be null and the loss has a name.
                s.camSelectorReadBack =
                    probe_replay::ReadObservedSelector(cc::Get(), &packed, &cameraIndex);
                if (s.camSelectorReadBack == probe_replay::SelectorReadBack::Ok) {
                    cc::RecordSelectorObservation(1, packed, team, slot);
                    s.camSelectorObserved = true;
                    s.camSelectorPacked = static_cast<int>(packed);
                    s.camSelectorExpected = static_cast<int>(cc::PackedSelectorWord(team, slot));
                }
            }
            const cc::SelectorDerivation derived = cc::SelectorDerivationState(1);
            s.camSelectorCertified = derived == cc::SelectorDerivation::Certified;
            s.camSelectorFalsified = derived == cc::SelectorDerivation::Falsified;
            CopyWord(s.camSelectorDerivation, sizeof s.camSelectorDerivation,
                     cc::SelectorDerivationName(derived));
        }
        RunStep(s, probe_replay::Verb::CamRelease, Simple(ctl::CommandId::CamRelease),
                "cam release", 0);
    }

    // command 0.5x and measure GetBufferTick's slope against our own QPC.
    if (a != nullptr && a->Available()) {
        const double a0 = a->AudioSeconds();
        const double t0 = SinceStartMs();
        RunStep(s, probe_replay::Verb::SpeedHalf, SpeedCommand(0.5), "speed 0.5", 0);
        StopAwareSleep(800);
        const double a1 = a->AudioSeconds();
        const double t1 = SinceStartMs();
        s.slopeWindowMs = t1 - t0;
        if (s.slopeWindowMs > 1.0 && (a1 - a0) > 0.02) {
            s.slope = (a1 - a0) / (s.slopeWindowMs / 1000.0);
            s.slopeMeasured = true;
        }
    } else {
        RunStep(s, probe_replay::Verb::SpeedHalf, SpeedCommand(0.5), "speed 0.5", 0);
        StopAwareSleep(800);
    }

    // FAST FORWARD, on both sides of the machine's own ceiling. The number is ui::RateCeiling()'s,
    // read off the snapshot; 0 means no frame period has been measured and uiapi.h forbids reading
    // that as 4.0, so both rate steps are simply not issued and their rows say why.
    char asked[72];
    if (s.rateCeiling > 0.0f) {
        const double ceiling = static_cast<double>(s.rateCeiling);
        // The midpoint of the OPEN interval (1, ceiling). `ceiling * 0.5` answers exactly 1.0 at
        // a ceiling of 2.00 -- which is not above 1x -- so the row refused on a machine where
        // 1.25x, 1.5x and 1.75x are all askable (L3-RUN-3 F4). At a ceiling of 1.0 or below this
        // still answers 1.0 and the row still refuses, correctly: the interval really is empty.
        s.rateUnderCeiling = 1.0 + (ceiling - 1.0) * 0.5;
        s.rateOverCeiling = ceiling + 1.0;
        _snprintf_s(asked, sizeof asked, _TRUNCATE, "speed %.4g", s.rateUnderCeiling);
        RunStep(s, probe_replay::Verb::SpeedUnderCeiling, SpeedCommand(s.rateUnderCeiling), asked, 0);
        _snprintf_s(asked, sizeof asked, _TRUNCATE, "speed %.4g", ceiling);
        RunStep(s, probe_replay::Verb::SpeedAtCeiling, SpeedCommand(ceiling), asked, 0);
        _snprintf_s(asked, sizeof asked, _TRUNCATE, "speed %.4g", s.rateOverCeiling);
        RunStep(s, probe_replay::Verb::SpeedOverCeiling, SpeedCommand(s.rateOverCeiling), asked, 0);
    }

    // The jumps are taken PAUSED so each one raises exactly the discontinuity it commanded: a
    // playing transport also crosses recorded gaps, and Cause::GapCross fires the same rows.
    RunStep(s, probe_replay::Verb::PauseAgain, Simple(ctl::CommandId::Pause), "pause", 0);

    // P17-B T18. `goto next` with nothing to go to, asked BEFORE the battery makes its own
    // bookmark -- after `mark` there is one and the empty-sidecar refusal is unreachable for
    // the rest of the run. The count is read first so the row can say the sidecar was empty
    // rather than assume it.
    {
        disp::Snapshot atStart;
        disp::ReadSnapshot(&atStart);
        s.bookmarksAtStartKnown = true;
        s.bookmarksAtStart = atStart.bookmarkCount;
    }
    RunStep(s, probe_replay::Verb::GotoNextNoMarkers, GotoStep(true), "goto next", 0);

    // P17-B T14. The signed percentage never reaches the dispatcher: control::command.cpp
    // refuses it at the parser, which is the surface both the console line and the panel's
    // text field go through. Read here so the row scores the product's own refusal text.
    {
        const ctl::ParseResult pr = ctl::Parse(L"seek +50%");
        s.seekPercentAsked = true;
        s.seekPercentParsed = pr.ok;
        _snprintf_s(s.seekPercentError, sizeof s.seekPercentError, _TRUNCATE, "%s", pr.error);
    }

    const int64_t mid = static_cast<int64_t>(duration / 2);
    _snprintf_s(asked, sizeof asked, _TRUNCATE, "seek %lld us", static_cast<long long>(mid));
    RunStep(s, probe_replay::Verb::Seek, WholeCommand(ctl::CommandId::Seek, mid), asked, mid);

    // The bookmark records wherever the playhead is when the command RUNS, so its target is the
    // read-back after the step and not the number we asked to seek to.
    RunStep(s, probe_replay::Verb::Mark, Simple(ctl::CommandId::Mark), "mark", 0);
    probe_replay::TransportStep& mark = s.steps[static_cast<size_t>(probe_replay::Verb::Mark)];
    mark.target = mark.playheadAfter;

    RunStep(s, probe_replay::Verb::StepForward, WholeCommand(ctl::CommandId::Step, 1), "step +1", 0);
    RunStep(s, probe_replay::Verb::StepBack, WholeCommand(ctl::CommandId::Step, -1), "step -1", 0);

    // Clause 4 of w2r-step-moves-by-ticks, askable at last: W2-B published the tick table (382990f),
    // so the boundary comes from the SAME accessor StepTicks lands on rather than from a spacing
    // multiplied out here -- two tick tables is how a timeline comes to disagree with the thing it
    // scrubs. It is asked at the microsecond each step actually landed on, so the observer's
    // comparison IS IsOnTickBoundary for that landing. Either read refusing, or a step that never
    // answered, leaves the clause unasked and the row keeps saying `boundary-not-published`, which
    // is a different statement from "not a boundary": NearestTickUs is also false BEFORE the first
    // recorded boundary, which a reverse step can legitimately reach.
    const probe_replay::TransportStep& fwdStep =
        s.steps[static_cast<size_t>(probe_replay::Verb::StepForward)];
    const probe_replay::TransportStep& bakStep =
        s.steps[static_cast<size_t>(probe_replay::Verb::StepBack)];
    if (fwdStep.ok && fwdStep.readBack && bakStep.ok && bakStep.readBack) {
        int64_t fwdBoundaryUs = 0;
        int64_t bakBoundaryUs = 0;
        if (t->NearestTickUs(static_cast<int64_t>(fwdStep.playheadAfter), &fwdBoundaryUs) &&
            t->NearestTickUs(static_cast<int64_t>(bakStep.playheadAfter), &bakBoundaryUs)) {
            s.stepBoundaryKnown = true;
            s.stepBoundaryForwardUs = static_cast<long long>(fwdBoundaryUs);
            s.stepBoundaryBackUs = static_cast<long long>(bakBoundaryUs);
        }
    }

    // `round` takes the number the timeline LABELS, not a position (transport.h B.8 amendment).
    // Both the label and its recorded time come from the panel's own marker table, filled at load
    // and not written afterwards.
    if (s.roundsKnown && s.roundCount > 0) {
        const int64_t label = static_cast<int64_t>(replay::ui::RoundNumberAt(0));
        const int64_t at = replay::ui::RoundTimeUs(0);
        _snprintf_s(asked, sizeof asked, _TRUNCATE, "round %lld", static_cast<long long>(label));
        RunStep(s, probe_replay::Verb::Round, WholeCommand(ctl::CommandId::Round, label), asked, at);
    } else if (s.roundsKnown) {
        // ARCH-21 S5.5: zero rounds is a LEGAL file and SeekRound refuses every number on it. The
        // refusal is the behaviour under test, so it has to be ASKED FOR -- a battery that skips
        // the command on an empty table leaves the one arm that can catch a silent clamp unrun.
        RunStep(s, probe_replay::Verb::Round, WholeCommand(ctl::CommandId::Round, 1), "round 1", 0);
    }

    // Away from the bookmark first: a `goto` issued where the playhead already is cannot tell a
    // working jump from one that does nothing.
    const int64_t away = static_cast<int64_t>(duration / 4);
    _snprintf_s(asked, sizeof asked, _TRUNCATE, "seek %lld us", static_cast<long long>(away));
    RunStep(s, probe_replay::Verb::SeekAway, WholeCommand(ctl::CommandId::Seek, away), asked, away);

    disp::Snapshot marked;
    disp::ReadSnapshot(&marked);
    const int64_t bookmark = static_cast<int64_t>(marked.bookmarkCount);
    _snprintf_s(asked, sizeof asked, _TRUNCATE, "goto %lld", static_cast<long long>(bookmark));
    RunStep(s, probe_replay::Verb::Goto, WholeCommand(ctl::CommandId::Goto, bookmark), asked, 0);

    // P17-B T15-T17, the bookmark WALK. A second mark late in the file, back to 0, then
    // `goto next` twice (which must reach the two marks in order, never the first twice),
    // once more at the last one (which must refuse without wrapping and without moving the
    // playhead) and `goto prev` back. Placed after `goto <n>` so the single-bookmark row
    // above is still scored against the file as it was.
    const int64_t late = static_cast<int64_t>(duration - duration / 4);
    _snprintf_s(asked, sizeof asked, _TRUNCATE, "seek %lld us", static_cast<long long>(late));
    RunStep(s, probe_replay::Verb::SeekLate, WholeCommand(ctl::CommandId::Seek, late), asked,
            late);
    RunStep(s, probe_replay::Verb::MarkSecond, Simple(ctl::CommandId::Mark), "mark", 0);
    probe_replay::TransportStep& mark2 =
        s.steps[static_cast<size_t>(probe_replay::Verb::MarkSecond)];
    mark2.target = mark2.playheadAfter;

    RunStep(s, probe_replay::Verb::SeekZero, WholeCommand(ctl::CommandId::Seek, 0), "seek 0",
            0);
    RunStep(s, probe_replay::Verb::GotoNextFirst, GotoStep(true), "goto next", 0);
    RunStep(s, probe_replay::Verb::GotoNextSecond, GotoStep(true), "goto next", 0);
    RunStep(s, probe_replay::Verb::GotoNextAtEnd, GotoStep(true), "goto next", 0);
    RunStep(s, probe_replay::Verb::GotoPrev, GotoStep(false), "goto prev", 0);

    // P17-B T11-T13, the RELATIVE nudges -- the panel's own -10s/+1s buttons. The delta is
    // ten seconds unless the file is too short to hold one, in which case it is an eighth of
    // the duration: the row scores against the delta the battery RECORDED, so a shorter
    // fixture changes the number without changing what the row can decide.
    const int64_t nudge = (duration / 4) > static_cast<uint64_t>(10ll * 1000000)
                              ? 10ll * 1000000
                              : static_cast<int64_t>(duration / 8);
    _snprintf_s(asked, sizeof asked, _TRUNCATE, "seek +%lld", static_cast<long long>(nudge / 1000000));
    RunStep(s, probe_replay::Verb::SeekRelForward, RelSeek(nudge), asked, nudge);
    _snprintf_s(asked, sizeof asked, _TRUNCATE, "seek -%lld", static_cast<long long>(nudge / 1000000));
    RunStep(s, probe_replay::Verb::SeekRelBack, RelSeek(-nudge), asked, -nudge);
    {
        disp::Snapshot here;
        disp::ReadSnapshot(&here);
        // Strictly past the start whatever the playhead is: the clamp is the subject, so the
        // delta is derived from where we actually are and not from a wished-for hour.
        const int64_t past = -(here.playheadUs + static_cast<int64_t>(duration));
        _snprintf_s(asked, sizeof asked, _TRUNCATE, "seek %lld us (past the start)",
                    static_cast<long long>(past));
        RunStep(s, probe_replay::Verb::SeekRelClampStart, RelSeek(past), asked, past);
    }

    // "and back": a negative rate, then the playhead sampled across several published ticks. One
    // sample cannot carry a direction.
    // L3-RUN-4 s6. `p10-seek-landing` had a single point -- the relative nudge above -- so the
    // row had no denominator. Three ABSOLUTE seeks at 25, 50 and 90 percent of the file, each
    // scored by the same predicate. 90 rather than 100 because the end of the file is already
    // the SeekLate / played-out arm's subject, and a seek to the last microsecond scores a
    // clamp rather than a landing.
    {
        static const unsigned kPct[] = {25u, 50u, 90u};
        static const probe_replay::Verb kAbsVerb[] = {
            probe_replay::Verb::SeekAbs25, probe_replay::Verb::SeekAbs50,
            probe_replay::Verb::SeekAbs90};
        for (size_t i = 0; i < sizeof kPct / sizeof kPct[0]; ++i) {
            // duration/100 first: a long capture in microseconds overflows int64 well before
            // this matters, but the division-first form costs nothing and cannot.
            const int64_t at = static_cast<int64_t>(duration / 100u * kPct[i]);
            _snprintf_s(asked, sizeof asked, _TRUNCATE, "seek %lld us (%u%% of %llu us)",
                        static_cast<long long>(at), kPct[i],
                        static_cast<unsigned long long>(duration));
            RunStep(s, kAbsVerb[i], WholeCommand(ctl::CommandId::Seek, at), asked, at);
        }
    }

    RunStep(s, probe_replay::Verb::Reverse, SpeedCommand(-1.0), "speed -1", 0);
    {
        disp::Snapshot r;
        disp::ReadSnapshot(&r);
        s.reversePlayheadFirst = r.playheadUs;
        s.reverseSamples = 1;
        for (int i = 0; i < 8; ++i) {
            Sleep(50);
            disp::ReadSnapshot(&r);
            ++s.reverseSamples;
        }
        s.reversePlayheadLast = r.playheadUs;
    }

    // Back to 1.0x and left running: the file playing out to its end IS this arm's declared
    // completion point, so the battery must not leave the transport parked.
    RunStep(s, probe_replay::Verb::Play, Simple(ctl::CommandId::Play), "play", 0);
    Log(LogLevel::Info, "battery complete: %u of %u step(s) answered, slope %.4f over %.0f ms "
        "(measured %d)", s.stepsAnswered, s.stepsAttempted, s.slope, s.slopeWindowMs,
        s.slopeMeasured ? 1 : 0);
    s.batteryComplete = true;
    g_batteryComplete = true;
}

// ---------------------------------------------------------------------------------------------
// The BROWSER arm.
//
// It does NOT hold the game in the menu. The profile it rides (`replay-local-bare`, from
// `offline-local`) carries `-level`, so the engine loads a level exactly as it does for every other
// arm, and this arm opens the replay browser OVER that level -- which is both what the descriptor's
// `completesAt: "match"` is scored on and
// what a person actually does.
//
// The browser is opened through the product's PUBLIC entry, ui::OpenBrowser (src/ui/uiapi.h:40) --
// the same call the panel's Browse button (src/ui/panel.cpp:1206) and the console dispatch
// (src/control/dispatch.cpp:607) make. Nothing private is reached, and no panel state is written
// here.
// ---------------------------------------------------------------------------------------------
void SampleBrowser(StatusFacts& s) {
    // The level-load answer comes from probe_replay::LevelReached -- [session]'s own state line --
    // and not from the entity bind, which is what made L5 report "level-not-reached" over a loaded
    // level (ARCH-10 4.1 row 4).
    const bool levelReached = probe_replay::LevelReached(g_log);
    if (levelReached) {
        InterlockedExchange(&g_levelReached, 1);
    }
    s.browserOpenRequested = g_browserOpened;
    s.browserOpen = g_browserOpened && replay::ui::BrowserOpen();
    s.browserOpenedAtKnown = g_browserOpened;
    s.browserOpenedAtMs = g_browserOpenedMs;
    s.panelEnabled = replay::ui::PanelEnabled();
    s.panelVisible = replay::ui::PanelVisible();
    if (!levelReached) {
        char why[400] = {0};
        probe_replay::LevelLoadReason(g_log, why, sizeof why);
        _snprintf_s(s.browserReason, sizeof s.browserReason, _TRUNCATE,
                    "%s (after %.0f ms), so there is nothing for the browser to be opened over",
                    why, SinceStartMs());
    } else if (!s.browserOpenRequested) {
        char why[400] = {0};
        probe_replay::LevelLoadReason(g_log, why, sizeof why);
        _snprintf_s(s.browserReason, sizeof s.browserReason, _TRUNCATE,
                    "%s; the tick has not yet opened the browser", why);
    } else {
        _snprintf_s(s.browserReason, sizeof s.browserReason, _TRUNCATE,
                    "replay::ui::OpenBrowser() called on the tick at %.0f ms over the level the profile "
                    "loaded; replay::ui::BrowserOpen()=%d, replay::ui::PanelEnabled()=%d, replay::ui::PanelVisible()=%d",
                    g_browserOpenedMs, replay::ui::BrowserOpen() ? 1 : 0, replay::ui::PanelEnabled() ? 1 : 0,
                    replay::ui::PanelVisible() ? 1 : 0);
    }
}

// The observer's only action on the tick thread, and it takes it once.
void Tick(const replay::TickInfo& info) {
    // How far the rig resolves, asked on every arm and every tick. Nothing is
    // written and no engine call is made (camera_control.h:262), so this cannot perturb the run.
    {
        namespace cc = replay::camera_control;
        cc::RigIdentity id;
        const cc::RigProbe p = cc::Probe(info.netGame, &id);
        InterlockedExchange(&g_cameraProbeLast, static_cast<LONG>(p));
        if (p == cc::RigProbe::Ready) {
            InterlockedExchange(&g_cameraProbeReadyEver, 1);
        }
    }
    if (g_arm != Arm::Browser || g_levelReached == 0 || g_browserOpened) {
        return;
    }
    g_browserOpened = true;
    g_browserOpenedMs = SinceStartMs();
    replay::ui::OpenBrowser();
}

// ---------------------------------------------------------------------------------------------
// completesAt, as the observer can SEE it. The harness declares the word; what the module can
// observe is the event that word names, and each arm has exactly one.
// ---------------------------------------------------------------------------------------------
bool CompletionReached(char* why, size_t n) {
    const double ms = SinceStartMs();
    switch (g_arm) {
        case Arm::Capture:
            if (g_log.captureClosedSeen && g_log.captureClosedEmpty) {
                // A close that closed no capture is not this arm's completion point: capture.cpp
                // prints it when nothing was recording, or when capture.enabled is false. Going
                // final here would file "the arm got what it asked for" over a launch that
                // captured nothing.
                _snprintf_s(why, n, _TRUNCATE,
                            "nothing-captured: the capture module closed without a capture, so this "
                            "is not the completion point the arm asked for: %s",
                            g_log.captureClosedLine);
                return false;
            }
            if (!g_log.captureClosedSeen) {
                _snprintf_s(why, n, _TRUNCATE,
                            "waiting for the capture module's own 'closed:' line, which is what the "
                            "harness waits for before taskkill; its absence is CAPTURE-UNFINALIZED");
                return false;
            }
            _snprintf_s(why, n, _TRUNCATE, "capture closed in-process: %s", g_log.captureClosedLine);
            break;
        case Arm::Playback:
        case Arm::PlaybackIdle: {
            const replay::IReplayTransport* t = g_services != nullptr ? g_services->transport : nullptr;
            if (t == nullptr || t->DurationUs() == 0) {
                _snprintf_s(why, n, _TRUNCATE, "waiting for a loaded file (DurationUs() is still 0)");
                return false;
            }
            const int64_t playhead = t->PlayheadUs();
            const int64_t duration = static_cast<int64_t>(t->DurationUs());
            g_status.playedOut = playhead >= duration - 100000;
            // L3-RUN-2 (a). The world is asked for FIRST: a launch that never reached state 9
            // has a world problem, and reporting how far the file had played would name the
            // wrong one. State 7 is LOADING the level -- the camera rig bound 14.8 s after it
            // on the recorded run, and the record filed at 7 carried a false asked-fail for
            // w2e-camera-rig-probe against a rig that had not been built.
            if (!probe_replay::InGameReached(g_log)) {
                _snprintf_s(why, n, _TRUNCATE,
                            "not-in-game: [session] never reported NetGame state 9 (in game); the file is at %lld of %lld us. Loading a level is not having one",
                            static_cast<long long>(playhead),
                            static_cast<long long>(duration));
                return false;
            }            if (!g_status.playedOut) {
                _snprintf_s(why, n, _TRUNCATE,
                            "playing: %lld of %lld us; the file reaching its end is this arm's "
                            "completion point", static_cast<long long>(playhead),
                            static_cast<long long>(duration));
                return false;
            }
            // Kept as a second, narrower guard: InGameReached above is the world test, and this
            // one cannot fire without it. Left in place because deleting it would make the two
            // level facts one, and a launch that reaches 9 without any level state line at all
            // is a log this observer should refuse rather than complete over.
            if (!probe_replay::LevelReached(g_log)) {
                _snprintf_s(why, n, _TRUNCATE,
                            "played-with-no-world: the file played out (%lld of %lld us) and "
                            "[session] never reported NetGame state 7, 8 or 9, so nothing was "
                            "played INTO a level and the arm has not reached its completion "
                            "point", static_cast<long long>(playhead),
                            static_cast<long long>(duration));
                return false;
            }
            if (g_arm == Arm::Playback && !g_batteryComplete) {
                _snprintf_s(why, n, _TRUNCATE,
                            "the file played out (%lld of %lld us) with a level loaded, but "
                            "the verb battery has not finished (%u of %u step(s) answered); "
                            "filing here would file over the questions it is still asking",
                            static_cast<long long>(playhead), static_cast<long long>(duration),
                            g_status.stepsAnswered, g_status.stepsAttempted);
                return false;
            }
            _snprintf_s(why, n, _TRUNCATE,
                        "the file played out: %lld of %lld us, with a level loaded%s",
                        static_cast<long long>(playhead), static_cast<long long>(duration),
                        g_arm == Arm::Playback ? " and the verb battery finished" : "");
            break;
        }
        default: {
            // BROWSER never plays a file, so it has no completion event of its own beyond the
            // level its profile loads. Rather than invent one, the observer stays provisional --
            // which the harness renders TRUNCATED -- unless the launch marker declares a window it
            // can see. That cost is real and it is recorded here rather than hidden behind a
            // confident final record.
            //
            // The word this branch prints is LevelLoadReason's, not one composed here, so the
            // completion predicate and the row cannot disagree about why the arm is waiting.
            if (!probe_replay::LevelReached(g_log)) {
                _snprintf_s(why, n, _TRUNCATE,
                            "%s. This arm declares completesAt 'match' and the sweep scores that "
                            "word on level load, so a record filed from the menu would be TRUNCATED "
                            "whatever it said", g_status.browserReason);
                return false;
            }
            if (!g_status.browserOpenRequested) {
                _snprintf_s(why, n, _TRUNCATE, "waiting for the tick to open the browser: %s",
                            g_status.browserReason);
                return false;
            }
            if (!g_status.markerWindowKnown) {
                _snprintf_s(why, n, _TRUNCATE,
                            "the browser is open over a loaded level, but no-launch-marker: %s, so "
                            "the record stays provisional and the sweep renders it TRUNCATED rather "
                            "than a verdict on a window nobody declared",
                            g_status.markerWindowReason);
                return false;
            }
            const double want = g_status.markerWindowSec * 1000.0;
            if (want <= 0.0 || ms < want) {
                _snprintf_s(why, n, _TRUNCATE, "observing: %.0f of the declared %.0f ms", ms, want);
                return false;
            }
            // The dwell is measured from max(browserOpenedAtMs,
            // levelLoadAtMs), not from the open. L5 re-run 3's 174621 ms was survivalMs minus the
            // open and included 13.6 s during which no level existed.
            double levelMs = 0.0;
            const double from = probe_replay::LevelLoadMonotonicMs(g_log, &levelMs) &&
                                        levelMs > g_browserOpenedMs
                                    ? levelMs
                                    : g_browserOpenedMs;
            const double openFor = ms - from;
            if (openFor < kBrowserDwellMs) {
                _snprintf_s(why, n, _TRUNCATE,
                            "the declared %.0f ms window elapsed, but the browser has only been "
                            "open for %.0f of the %.0f ms this arm holds for", want, openFor,
                            kBrowserDwellMs);
                return false;
            }
            _snprintf_s(why, n, _TRUNCATE,
                        "the declared %.0f ms window elapsed with the browser open over a loaded "
                        "level for %.0f ms", want, openFor);
            break;
        }
    }
    if (ms < kEarliestFinalMs) {
        _snprintf_s(why, n, _TRUNCATE,
                    "RED FLAG: the completion event fired at %.0f ms, far below any launch budget; "
                    "holding the final record until %.0f ms rather than ending the launch early",
                    ms, kEarliestFinalMs);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The record.
// ---------------------------------------------------------------------------------------------
void AppendEscaped(char* out, size_t outN, const char* in) {
    size_t at = strlen(out);
    for (const char* p = in; *p != '\0' && at + 2 < outN; ++p) {
        if (*p == '"' || *p == '\\') {
            out[at++] = '\\';
            out[at++] = *p == '\\' ? '\\' : '"';
        } else if (static_cast<unsigned char>(*p) < 0x20) {
            out[at++] = ' ';
        } else {
            out[at++] = *p;
        }
    }
    out[at] = '\0';
}

// L3-RUN-4 F2. Both records from that run carried `provisional: true`, were written ~250 ms
// before the file loaded, were never superseded after the crash, and were still the only
// answer the session left -- 39 of their rows stated the absence of events that then
// happened. A record is FINAL only when the arm reached its declared completion point AND the
// two things every absence sentence in it depends on have actually been asked: the verb
// battery, which is what fills steps[], and the world, which every entity/roster/camera row
// sits behind. The completion predicate already implies both on the playback arms; stating
// them here means a completion predicate that drifts cannot quietly make a snapshot final.
bool RecordIsProvisional() {
    if (!g_completed) {
        return true;
    }
    if (g_arm == Arm::Playback && !g_status.batteryComplete) {
        return true;
    }
    return !probe_replay::LevelReached(g_log);
}

void WriteRecord(WriteReason reason) {
    if (g_artifactDir[0] == '\0') {
        return;
    }
    const bool provisional = RecordIsProvisional();
    // -1 is "there was no transport to ask", which is not a playhead of 0.
    long long playheadUs = -1;
    {
        const replay::IReplayTransport* t =
            g_services != nullptr ? g_services->transport : nullptr;
        if (t != nullptr) {
            playheadUs = static_cast<long long>(t->PlayheadUs());
        }
    }

    size_t rowCount = 0;
    const probe_replay::RowSpec* rows = probe_replay::Rows(&rowCount);

    static char answers[131072];
    answers[0] = '\0';
    int passed = 0, failed = 0, undetermined = 0, listed = 0;
    for (size_t i = 0; i < rowCount; ++i) {
        if ((rows[i].arms & g_armBit) == 0) {
            continue;
        }
        // 2048, not 1024. ARCH-12 4.3's cs-space row quotes five <CAND> verbatim plus two
        // <SYM16>, and a detail cut mid-evidence reads to an operator exactly like a short one.
        char detail[2048] = {0};
        Ask outcome = Ask::CouldNotAsk;
        if (!probe_replay::EvaluateRow(rows[i].id, g_log, g_status, &outcome, detail, sizeof detail)) {
            continue;
        }
        // Every could-not-ask sentence in a provisional record is an absence claim with a
        // horizon, and it says so. Applied here, once, so no row can be added without it.
        if (provisional && outcome == Ask::CouldNotAsk) {
            probe_replay::AppendProvisionalHorizon(detail, sizeof detail, g_status.observedMs,
                                                  playheadUs);
        }
        if (outcome == Ask::Pass) {
            ++passed;
        } else if (outcome == Ask::Fail) {
            ++failed;
        } else {
            ++undetermined;
        }
        char head[512];
        _snprintf_s(head, sizeof head, _TRUNCATE,
                    "%s    {\"id\": \"%s\", \"filedBy\": \"%s\", \"state\": \"%s\", \"detail\": \"",
                    listed == 0 ? "" : ",\n", rows[i].id, rows[i].filedBy,
                    probe_replay::AskWord(outcome));
        strncat_s(answers, sizeof answers, head, _TRUNCATE);
        AppendEscaped(answers, sizeof answers, detail);
        strncat_s(answers, sizeof answers, "\"}", _TRUNCATE);
        ++listed;
    }

    const char* verdict = "UNDETERMINED";
    if (failed > 0) {
        verdict = "FAIL";
    } else if (undetermined == 0 && passed > 0) {
        verdict = "PASS";
    }

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    for (char* p = cwd; *p != '\0'; ++p) {
        if (*p == '\\') {
            *p = '/';
        }
    }

    static char record[131072];
    _snprintf_s(record, sizeof record, _TRUNCATE,
                "{\n"
                "  \"probe\": \"%s\",\n"
                "  \"verdict\": \"%s\",\n"
                "  \"provisional\": %s,\n"
                "  \"writeReason\": \"%s\",\n"
                "  \"playheadUs\": %lld,\n"
                "  \"checked\": %d,\n"
                "  \"passed\": %d,\n"
                "  \"failed\": %d,\n"
                "  \"undetermined\": %d,\n"
                "  \"notAsked\": 0,\n"
                "  \"cwd\": \"%s\",\n"
                "  \"arm\": \"%s\",\n"
                "  \"armSource\": \"",
                g_probeId, verdict, provisional ? "true" : "false", WriteReasonName(reason),
                playheadUs, passed + failed, passed, failed,
                undetermined, cwd, ArmName(g_arm));
    AppendEscaped(record, sizeof record, g_armReason);
    strncat_s(record, sizeof record, "\",\n  \"coexist\": \"", _TRUNCATE);
    AppendEscaped(record, sizeof record, g_coexist);
    strncat_s(record, sizeof record, "\",\n  \"completion\": \"", _TRUNCATE);
    AppendEscaped(record, sizeof record, g_completion);
    strncat_s(record, sizeof record, "\",\n", _TRUNCATE);

    // W1-J. The harness's only probe-side source for the last layer. Every value comes out of the
    // egress module's published accessors -- the state word is ArmStateName's, verbatim, not the
    // legacy `armed` boolean and not anything derived from the hook counts. This arm is exclusive
    // (W2-R.probe.json: `exclusive: true` on all five), so the observer is the only reporter on
    // its launch, which is what W1-J requires. A state the module would not spell files nothing.
    {
        const replay::egress::Report& eg = replay::egress::Current();
        probe_replay::EgressFacts ef;
        ef.state = replay::egress::ArmStateName(eg.state);
        ef.armedBy = replay::egress::ArmTriggerName(eg.trigger);
        ef.hooksJson = replay::egress::HookNamesJson();
        ef.uncovered = replay::egress::CoverageNote();
        // AUD-L5 s12 item 8. Already redacted by egress.h, and FAIL-CLOSED: when the salt is
        // unavailable both accessors return nullptr, and the record then carries the reason instead
        // of the arrays. It never carries the counts alone -- that is the L5 record.
        ef.refusedDestinations = replay::egress::RefusedDestinationsJson();
        ef.allowedDestinations = replay::egress::AllowedDestinationsJson();
        ef.redactionRefusal = replay::egress::RedactionRefusalReason();
        ef.armedAtMs = eg.armedAtMs;
        ef.refused = eg.refusedRemote;
        probe_replay::AppendEgressObject(record, sizeof record, ef);
    }

    // The panel counters as FIELDS on the record, so the L5 #6 runner grades what
    // the module published instead of parsing a row's prose. Absent means unread throughout, and a
    // self-named refusal replaces the published group rather than zeroing it.
    probe_replay::AppendCsRegisterObject(record, sizeof record, g_status);

    char markerSource[MAX_PATH];
    strncpy_s(markerSource, sizeof markerSource,
              g_status.markerPresent ? g_status.markerSource : kLaunchMarkerName, _TRUNCATE);
    for (char* q = markerSource; *q != '\0'; ++q) {
        if (*q == '\\') {
            *q = '/';
        }
    }

    char tailInfo[1024];
    _snprintf_s(tailInfo, sizeof tailInfo, _TRUNCATE,
                "  \"launchMarker\": {\"present\": %s, \"source\": \"%s\", \"windowSec\": %.0f, "
                "\"windowKnown\": %s, \"attended\": %s, \"attendedKnown\": %s},\n"
                "  \"logSource\": \"%s\",\n"
                "  \"logLines\": %llu,\n"
                "  \"survivalMs\": %.0f,\n"
                "  \"heartbeats\": %ld,\n"
                "  \"batteryRan\": %s,\n"
                "  \"batteryComplete\": %s,\n"
                "  \"playedOut\": %s,\n"
                "  \"answers\": [\n",
                g_status.markerPresent ? "true" : "false", markerSource, g_status.markerWindowSec,
                g_status.markerWindowKnown ? "true" : "false", g_status.markerAttended ? "true" : "false",
                g_status.markerAttendedKnown ? "true" : "false",
                g_tail.Source(), g_tail.Lines(), SinceStartMs(), g_heartbeats,
                g_batteryRan ? "true" : "false", g_status.batteryComplete ? "true" : "false",
                g_status.playedOut ? "true" : "false");
    strncat_s(record, sizeof record, tailInfo, _TRUNCATE);
    strncat_s(record, sizeof record, answers, _TRUNCATE);
    // DESIGN S12.8.3 / ARCH-11 S2.2: an observer ROW id names a question this module
    // SCORES and produces a verdict word; an R1 read id names a question and produces a
    // NUMBER. Two meanings, and now two keys. `rows[]` is the observer's. `answers[]` keeps
    // the same content for one revision because ARCH-11's routing is land `rows[]` BESIDE
    // `answers[]` first -- W1-J's reader and CLEAN(L5) clause 4 both name `answers[]` today,
    // and dropping the field before they move would lose the verdict mid-flight.
    strncat_s(record, sizeof record, "\n  ],\n  \"rows\": [\n", _TRUNCATE);
    strncat_s(record, sizeof record, answers, _TRUNCATE);
    strncat_s(record, sizeof record, "\n  ]\n}\n", _TRUNCATE);

    // Lands atomically: the harness polls done.json while the game runs, and a reader that catches
    // the file mid-write gets a parse error. On a provisional record that costs nothing; on the
    // final one it would be a lost verdict.
    char path[MAX_PATH];
    char tmp[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", g_artifactDir);
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s\\done.json.writing", g_artifactDir);
    FILE* fh = nullptr;
    if (fopen_s(&fh, tmp, "wb") != 0 || fh == nullptr) {
        return;
    }
    fputs(record, fh);
    fclose(fh);
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
            return;
        }
        Sleep(50);
    }
    if (CopyFileA(tmp, path, FALSE)) {
        DeleteFileA(tmp);
    }
}

// SURVIVE, and it is not instrumentation garnish: it is the only thing that turns a crash from an
// exit code into a number. One line a second with the elapsed time and the heartbeat count, and a
// provisional record beside it, so a process that dies at 13 s leaves 13 s on disk rather than a
// silence somebody has to interpret.
DWORD WINAPI ObserverWorker(LPVOID) {
    g_tail.Open();
    Log(LogLevel::Info, "log tail: %s (from %s)", g_tail.Path(), g_tail.Source());

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        InterlockedIncrement(&g_heartbeats);
        g_tail.Poll(g_log);
        g_status.observedMs = SinceStartMs();
        SampleCsRegister(g_status);
        SampleEntity(g_status);
        SampleInbound(g_status);
        SampleCameraProbe(g_status);
        SampleWiretap(g_status);
        SampleTransportAndAudio(g_status);
        SampleSc7(g_status);
        SampleDemoStreams(g_status);
        SampleBodyApply(g_status);
        if (g_arm == Arm::Capture) {
            SampleStopSentinel(g_status);
        }
        if (g_arm == Arm::Browser) {
            SampleBrowser(g_status);
        }
        if (g_arm == Arm::Playback && !g_batteryRan) {
            RunBattery(g_status);
        }

        if (!g_completed && CompletionReached(g_completion, sizeof g_completion)) {
            g_completed = true;
            Log(LogLevel::Info, "completesAt reached at %.0f ms: %s", SinceStartMs(), g_completion);
        }

        Log(LogLevel::Info, "SURVIVE %.0f ms, heartbeat %ld, arm %s", SinceStartMs(), g_heartbeats,
            ArmName(g_arm));
        WriteReason reason = WriteReason::Periodic;
        if (g_status.batteryComplete && !g_batteryCompleteReported) {
            g_batteryCompleteReported = true;
            reason = WriteReason::BatteryComplete;
        }
        if (g_completed) {
            reason = WriteReason::FileEnd;
        }
        WriteRecord(reason);
        if (g_completed) {
            return 0;      // the final record is written once, at the declared completion point
        }
        Sleep(1000);
    }
    return 0;
}

bool Init(replay::ReplayServices* services) {
    g_services = services;
    // The product's own spelling of the step refusal, published to the row table so observer_rows.h
    // -- which includes no product header -- does not carry a second copy of it.
    _snprintf_s(g_status.stepNoTickToken, sizeof g_status.stepNoTickToken, _TRUNCATE, "%s",
                ctl::kStepNoTickThatWay);
    // P10-F. The same rule for three more product words: the rows score on these, so they arrive
    // from the product's own name functions and never from a spelling of the observer's.
    _snprintf_s(g_status.seekLandingOffToken, sizeof g_status.seekLandingOffToken, _TRUNCATE, "%s",
                ctl::SeekLandingName(ctl::SeekLanding::OffTarget));
    _snprintf_s(g_status.facingDegenerateToken, sizeof g_status.facingDegenerateToken, _TRUNCATE,
                "%s", replay::ControlRefusalName(replay::ControlRefusal::FacingDegenerate));
    _snprintf_s(g_status.lookAtUnresolvableToken, sizeof g_status.lookAtUnresolvableToken,
                _TRUNCATE, "%s",
                replay::ControlRefusalName(replay::ControlRefusal::LookAtUnresolvable));
    // With the composer installed, p10-cam-9 has to tell an unresolvable
    // SEAM from an unoccupied SLOT. These are camera_control's own spellings of the three.
    // All FIVE of camera_control's non-Resolved spellings, and through
    // PublishToken rather than a _TRUNCATE copy: `DerivationUncertified` is 103 bytes, and
    // truncating it into a char[40] published a token that looked filled and could never match
    // the word the product had actually said. PublishToken refuses instead, leaving the buffer
    // empty and counted, so p10-cam-9 reports an unpublished spelling rather than scoring a
    // prefix of one.
    {
        static const cc::SubjectResolve kCauses[] = {
            cc::SubjectResolve::NoSuchSubject, cc::SubjectResolve::RosterUnreadable,
            cc::SubjectResolve::NoBody, cc::SubjectResolve::NoSeam,
            cc::SubjectResolve::DerivationUncertified};
        char* const kBufs[] = {g_status.subjNoSuchSubjectToken,
                               g_status.subjRosterUnreadableToken,
                               g_status.subjNoBodyToken,
                               g_status.subjNoSeamToken,
                               g_status.subjDerivationUncertifiedToken};
        const size_t kCaps[] = {sizeof g_status.subjNoSuchSubjectToken,
                                sizeof g_status.subjRosterUnreadableToken,
                                sizeof g_status.subjNoBodyToken,
                                sizeof g_status.subjNoSeamToken,
                                sizeof g_status.subjDerivationUncertifiedToken};
        for (size_t i = 0; i < sizeof kCauses / sizeof kCauses[0]; ++i) {
            const char* spelling = cc::SubjectResolveName(kCauses[i]);
            const unsigned len = probe_replay::TokenLen(spelling);
            if (len > g_status.subjTokenLongestLen) { g_status.subjTokenLongestLen = len; }
            if (!probe_replay::PublishToken(kBufs[i], kCaps[i], spelling)) {
                ++g_status.subjTokenDropped;
                Log(LogLevel::Warn,
                    "camera_control SubjectResolve %u spells %u bytes and does not fit its %u-byte "
                    "published token; p10-cam-9 will read it as unpublished",
                    static_cast<unsigned>(i), len, static_cast<unsigned>(kCaps[i]));
            }
        }
    }
    // The landing tolerance is NOT set here. It used to be control::kSeekLandingToleranceUs -- a
    // compiled-in 30 Hz frame AUD-P17 F4 deleted -- and Init is the wrong moment for it anyway:
    // no file is loaded yet, and the interval is a property of the capture. RunBattery asks the
    // transport for it once a file is open.
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);

    if (!ResolveArtifactDir(g_artifactDir, sizeof g_artifactDir, g_probeId, sizeof g_probeId)) {
        // No sweep named a directory for us, so there is nowhere this launch's evidence belongs.
        // The module still runs; it just does not write into somebody's install.
        Log(LogLevel::Warn, "REPLAY_PROBE_DIRS names no directory for this module; no records written");
    } else {
        Log(LogLevel::Info, "artifacts: %s (probe id %s)", g_artifactDir, g_probeId);
    }

    // After the probe id is known: PLAYBACK-IDLE is declared by the descriptor's row, not by the file.
    LoadLaunchMarker();
    g_arm = SelectArm(g_armReason, sizeof g_armReason);
    g_armBit = ArmBit(g_arm);
    Log(LogLevel::Info, "arm selected from the process environment: %s", g_armReason);
    RecordCoexistence(g_coexist, sizeof g_coexist);
    Log(g_coexistViolation ? LogLevel::Error : LogLevel::Info, "coexistence: %s", g_coexist);

    if (g_artifactDir[0] == '\0') {
        return true;
    }
    g_worker = CreateThread(nullptr, 0, &ObserverWorker, nullptr, 0, nullptr);
    return true;
}

void Shutdown() {
    InterlockedExchange(&g_stop, 1);
    bool joined = true;
    if (g_worker != nullptr) {
        // The old bound was 3000 ms against a battery that alone holds two
        // 4000 ms waits and never read g_stop: it timed out by construction, and Shutdown then
        // closed the handle and wrote the final record WHILE the worker was still sampling and
        // still calling through g_services. The waits are cooperative now, so this budget is slack
        // rather than a race -- but a timeout is a FACT, not a silence: the record says so, the
        // handle stays open and g_services stays valid, because a thread that is still running
        // needs both more than this teardown needs to be tidy.
        const DWORD kJoinBudgetMs = 15000;   // > the battery's worst case (two 4000 ms waits)
        joined = WaitForSingleObject(g_worker, kJoinBudgetMs) == WAIT_OBJECT_0;
        if (joined) {
            CloseHandle(g_worker);
            g_worker = nullptr;
        } else {
            Log(LogLevel::Error,
                "shutdown: the observer worker did not join within %lu ms -- the handle is left "
                "open and g_services left valid, because it is still running", kJoinBudgetMs);
        }
    }
    // THE LAST WRITE, and the only one a launch that faulted after the worker's last
    // heartbeat gets. L3-RUN-4's two records were snapshots taken before the file loaded and
    // were never superseded, so the whole session was graded off them. The worker is joined
    // above, so nothing is sampling while this writes. It does NOT survive a hard crash: a
    // crash handler is out of scope for this module (it is DIAG-CRASH's). What it buys is
    // every fault the process outlives, including an orderly teardown after one.
    g_status.observedMs = SinceStartMs();
    g_status.workerJoinKnown = true;
    g_status.workerJoined = joined;
    WriteRecord(WriteReason::Shutdown);
    // Only now is nobody else reading it. If the join failed we keep it: a live worker with a null
    // g_services faults on its next Log, which is the thing this whole packet exists to prevent.
    if (joined) {
        g_services = nullptr;
    }
}

}  // namespace

REPLAY_MODULE("observer", 900, Init, Tick, Shutdown);
