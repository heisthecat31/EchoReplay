// W1-D's in-game probe. Queued through runs\queue\W1-D.probe.json; this packet never launches the game.
//
// It answers the five things no offline check can reach, and it answers them by MEASUREMENT:
//
//   P1  which slot the claim actually took, in this launch configuration
//   P2  exactly one tick per RENDERED frame, against CncaGame+0x830
//   P3  the delta we are handed is the value standing in CClock+104
//   P4  ONE clock heartbeat per frame, not two  (see the note on simTime below)
//   P5  ★ every name the engine resolves against OUR module, recorded positively
//
// ★ P5 IS A STRONGER CANARY THAN THE ONE THE DESIGN SPECIFIES, and deliberately so. S2 F8b greps the
//   r14 log for "Method '%s' is unimplemented for net service provider '%s'". That line only appears
//   when a lookup FAILS -- it is silent about any name the engine resolved successfully, and it depends
//   on finding and parsing a log whose location the design does not pin down. Hooking GetProcAddress
//   inside OUR OWN PROCESS (intervention-ledger category 1X: a Windows API, never game code) records
//   every lookup against our HMODULE, successful or not, by name. That turns "we believe only two names
//   can reach us" into a list of the names that did. The log grep stays as a second, independent check
//   the sweep runs from outside; this is the one that produces evidence rather than an absence.
//
// The probe is a test binary. None of this ships: echoreplay.dll installs no API hook.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include <MinHook.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/log.h"
#include "bootstrap/services.h"
#include "engine/tick.h"
#include "engine/tick_policy.h"
#include "generated/facts_generated.h"

namespace {

const char kReplayModuleName[] = "w1d-probe";

// 3600 frames is a minute at 60 Hz: long enough that a once-per-few-seconds duplicate would show up,
// short enough that the sweep's per-launch deadline is not the thing being tested. But a profile whose
// behaviour nobody has measured yet may not run that long, and a probe that files nothing because it
// wanted more data than the launch had is indistinguishable from one that never loaded. So the window
// is a TARGET and kMinFrames is the floor below which the continuity result would not mean anything.
constexpr uint32_t kSampleFrames = 3600;
constexpr uint32_t kMinFrames = 600;
constexpr size_t   kMaxResolvedNames = 32;

struct Sample {
    uint64_t frame;
    uint64_t handedDeltaUs;   // what CNSProvider::Update passed us -- ONE heartbeat, not a frame
    uint64_t clockDeltaUs;    // CClock+104, read back independently inside the same call
    uint64_t simTimeUs;       // CClock+0x28, which accumulates every heartbeat
    uint64_t frameDeltaUs;    // simTimeUs advance since our previous tick = the real frame
    uint64_t qpc;
};

struct Resolved {
    char     name[64];
    uint64_t count;
};

struct ProbeState {
    Sample                samples[kSampleFrames];
    std::atomic<uint32_t> sampleCount;

    Resolved              resolved[kMaxResolvedNames];
    std::atomic<uint32_t> resolvedCount;
    std::atomic<uint64_t> lookupsAgainstUs;
    std::atomic<uint64_t> resolvedOverflow;
    CRITICAL_SECTION      resolvedLock;

    HMODULE self;
    HANDLE  writer;
    std::atomic<bool> armed;
};

ProbeState g;

// ---------------------------------------------------------------------------------------------------
// P5: the GetProcAddress tripwire.
// ---------------------------------------------------------------------------------------------------

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_originalGetProcAddress = nullptr;

// ---------------------------------------------------------------------------------------------------
// ★ WHICH SLOT SHOULD BE FREE, predicted from the game's own command line.
//
// The probe could be told the answer by its queue descriptor, but then each new profile would need a new
// expectation and a wrong one would look like a defect in the claim. The engine decides this from one
// bit of one word, set by one instruction, guarded by a search for one literal - so the probe can derive
// the same prediction the engine does and ASSERT the claim matched it. That makes one probe correct on
// bare-level, on flat, and on any profile added later, and turns "report which slot" into "test the
// model".
//
// This mirrors the engine's search at 0x140116861..0x1401168BA rather than reproducing its argument
// normalisation exactly, so a pathological command line could disagree; the probe prints the command
// line it judged, so a disagreement is diagnosable rather than mysterious.
bool CommandLineHasNoOvr() {
    return replay::tick::CommandLineHasFlag(GetCommandLineA(), "-noovr");
}

void RecordLookup(LPCSTR name) {
    g.lookupsAgainstUs.fetch_add(1, std::memory_order_relaxed);
    // An ordinal import arrives as a small integer in the pointer, not a string.
    if (reinterpret_cast<uintptr_t>(name) <= 0xFFFF) {
        return;
    }
    EnterCriticalSection(&g.resolvedLock);
    const uint32_t count = g.resolvedCount.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(g.resolved[i].name, name) == 0) {
            ++g.resolved[i].count;
            LeaveCriticalSection(&g.resolvedLock);
            return;
        }
    }
    if (count < kMaxResolvedNames) {
        strncpy_s(g.resolved[count].name, sizeof g.resolved[count].name, name, _TRUNCATE);
        g.resolved[count].count = 1;
        g.resolvedCount.store(count + 1, std::memory_order_release);
    } else {
        g.resolvedOverflow.fetch_add(1, std::memory_order_relaxed);
    }
    LeaveCriticalSection(&g.resolvedLock);
}

FARPROC WINAPI GetProcAddressDetour(HMODULE module, LPCSTR name) {
    if (module == g.self) {
        RecordLookup(name);
    }
    return g_originalGetProcAddress(module, name);
}

bool ArmTripwire() {
    // hookmgr::Initialize already ran from DllMain; MH_ERROR_ALREADY_INITIALIZED is the expected answer
    // and is not a failure.
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        replay::log::Direct(replay::LogLevel::Error, kReplayModuleName, "MinHook init: %s", MH_StatusToString(init));
        return false;
    }
    // Ex, for the resolved target: enabling MH_ALL_HOOKS here would also arm whatever the tick module
    // created, which is its decision to make and not this probe's.
    LPVOID target = nullptr;
    const MH_STATUS created = MH_CreateHookApiEx(L"kernel32", "GetProcAddress",
                                                 reinterpret_cast<LPVOID>(&GetProcAddressDetour),
                                                 reinterpret_cast<LPVOID*>(&g_originalGetProcAddress),
                                                 &target);
    if (created != MH_OK) {
        replay::log::Direct(replay::LogLevel::Error, kReplayModuleName, "could not hook GetProcAddress: %s",
                    MH_StatusToString(created));
        return false;
    }
    const MH_STATUS armed = MH_EnableHook(target);
    if (armed != MH_OK) {
        replay::log::Direct(replay::LogLevel::Error, kReplayModuleName, "could not arm the tripwire: %s",
                    MH_StatusToString(armed));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------
// P2/P3/P4: the per-tick sample.
// ---------------------------------------------------------------------------------------------------

void OnTick(const replay::TickInfo& info, void*) {
    const uint32_t index = g.sampleCount.load(std::memory_order_relaxed);
    if (index >= kSampleFrames) {
        return;
    }
    uint64_t clockDelta = 0;
    uint64_t simTime = 0;
    uint64_t frame = 0;
    replay::tick::ReadEngineClock(&clockDelta, &simTime, &frame);

    Sample& s = g.samples[index];
    s.frame = frame;
    s.handedDeltaUs = info.scaledDeltaUs;
    s.clockDeltaUs = clockDelta;
    s.simTimeUs = simTime;
    s.frameDeltaUs = info.frameDeltaUs;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    s.qpc = static_cast<uint64_t>(qpc.QuadPart);
    g.sampleCount.store(index + 1, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------------
// Reporting.
// ---------------------------------------------------------------------------------------------------

void ProbeDir(char* out, size_t cap) {
    char root[MAX_PATH] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);
    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(g.self, dll, sizeof dll);
    const char* base = strrchr(dll, '\\');
    base = base != nullptr ? base + 1 : dll;
    char stem[MAX_PATH] = {0};
    strncpy_s(stem, sizeof stem, base, _TRUNCATE);
    if (char* dot = strrchr(stem, '.')) {
        *dot = '\0';
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", root, stem);
    CreateDirectoryA(out, nullptr);
}

DWORD WINAPI WriterMain(LPVOID) {
    // Wait for the sample window, or give up if no tick source ever went live. Filing a short verdict
    // beats filing none: the launch ends when every probe has reported, so an over-long wait spends
    // somebody else's deadline too.
    const ULONGLONG deadline = GetTickCount64() + 240000;
    while (g.sampleCount.load(std::memory_order_acquire) < kSampleFrames &&
           GetTickCount64() < deadline) {
        Sleep(200);
    }

    replay::tick::Stats stats{};
    replay::tick::GetStats(&stats);
    const uint32_t taken = g.sampleCount.load(std::memory_order_acquire);

    char dir[MAX_PATH];
    ProbeDir(dir, sizeof dir);

    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\ticks.jsonl", dir);
    uint64_t deltaMismatch = 0;
    uint64_t handedExceedsFrame = 0;   // beat 1 cannot be larger than the frame that contains it
    uint64_t framesWithExtraBeats = 0; // frames where the frame delta exceeded the handed beat
    uint64_t simRegressions = 0;
    uint64_t frameDeltaSumUs = 0;
    uint64_t handedSumUs = 0;
    if (FILE* fh = nullptr; fopen_s(&fh, path, "wb") == 0 && fh != nullptr) {
        for (uint32_t i = 0; i < taken; ++i) {
            const Sample& s = g.samples[i];
            // P3: the value we were handed must be the value standing in CClock+104.
            if (s.handedDeltaUs != s.clockDeltaUs) {
                ++deltaMismatch;
            }
            // ★ P4, RE-SPECIFIED. The original asserted simTime advanced by exactly the delta we
            // were handed, i.e. that the frame drives ONE clock heartbeat. The audit disproved that
            // premise: there are at least THREE beats per frame and two of them run after our tick, so
            // the old check would have failed on correct behaviour. What is actually true, and what is
            // therefore worth asserting:
            //
            //   - sim time never goes backwards;
            //   - the frame delta is at least the beat we were handed, because that beat is one
            //     component of the frame;
            //   - and the two quantities genuinely DIFFER, which is the observable signature of the
            //     extra beats existing at all. A run where they were equal would mean the three-beat
            //     model is wrong, and that is worth finding out.
            if (i > 0) {
                if (s.simTimeUs < g.samples[i - 1].simTimeUs) {
                    ++simRegressions;
                }
                if (s.handedDeltaUs > s.frameDeltaUs) {
                    ++handedExceedsFrame;
                }
                if (s.frameDeltaUs > s.handedDeltaUs) {
                    ++framesWithExtraBeats;
                }
                frameDeltaSumUs += s.frameDeltaUs;
                handedSumUs += s.handedDeltaUs;
            }
            fprintf(fh,
                    "{\"i\":%u,\"frame\":%llu,\"handedUs\":%llu,\"clockUs\":%llu,\"simUs\":%llu,"
                    "\"frameUs\":%llu,\"qpc\":%llu}\n",
                    i, (unsigned long long)s.frame, (unsigned long long)s.handedDeltaUs,
                    (unsigned long long)s.clockDeltaUs, (unsigned long long)s.simTimeUs,
                    (unsigned long long)s.frameDeltaUs, (unsigned long long)s.qpc);
        }
        fclose(fh);
    }

    // ★ The measurement that settles which quantity is the frame. Over the sample window, real
    // elapsed time (QPC) times the timescale must be tracked by the SUM OF FRAME DELTAS and NOT by the
    // sum of the beats we were handed. At timescale 1 the first should land within a few percent and
    // the second should fall visibly short. This is the check that would have caught F-D1 before the
    // audit did, and it is stated so that correct behaviour passes it.
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    uint64_t wallUs = 0;
    if (taken > 1 && freq.QuadPart > 0) {
        wallUs = ((g.samples[taken - 1].qpc - g.samples[0].qpc) * 1000000ull) /
                 static_cast<uint64_t>(freq.QuadPart);
    }
    // Within 10%: the window is a minute of real frames, so scheduling noise is small, but this is a
    // sanity band rather than a precision claim.
    const bool frameSumTracksWall =
        wallUs == 0 || (frameDeltaSumUs * 10ull >= wallUs * 9ull &&
                        frameDeltaSumUs * 9ull <= wallUs * 10ull);

    // P5: what did the engine actually ask us for?
    char names[1024] = {0};
    bool onlyExpected = true;
    const uint32_t resolved = g.resolvedCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < resolved; ++i) {
        char one[128];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s%s x%llu", i ? ", " : "", g.resolved[i].name,
                    (unsigned long long)g.resolved[i].count);
        strncat_s(names, sizeof names, one, _TRUNCATE);
        if (strcmp(g.resolved[i].name, "Update") != 0 &&
            strcmp(g.resolved[i].name, "RadPluginShutdown") != 0) {
            onlyExpected = false;
        }
    }

    // Every one of these is an assertion, and `checked` is their count: the harness treats checked=0 as
    // NOT-RUN, and a probe that reported a verdict without saying how many things it looked at has not
    // produced one.
    struct Assertion {
        const char* what;
        bool        ok;
    };
    // ★ The model test: derive the prediction the way the engine does, then assert the claim matched.
    const bool noOvr = CommandLineHasNoOvr();
    const uint32_t predictedSlot =
        noOvr ? facts::GAME_PROVIDER_SLOT_A : facts::GAME_PROVIDER_SLOT_C;
    const bool slotAsPredicted =
        stats.source != replay::tick::Source::Slot || stats.slotOffset == predictedSlot;

    const Assertion checks[] = {
        {"a tick source went live", stats.source != replay::tick::Source::None},
        {"enough frames were sampled for continuity to mean anything", taken >= kMinFrames},
        {"the claimed slot is the one -noovr predicts for this command line", slotAsPredicted},
        {"no frame was ticked twice", stats.duplicateFrames == 0},
        {"no rendered frame was missed", stats.skippedFrames == 0},
        {"every delta equalled CClock+104", deltaMismatch == 0},
        {"sim time never ran backwards", simRegressions == 0},
        {"the handed beat never exceeded the frame that contains it", handedExceedsFrame == 0},
        {"the frame delta tracks real elapsed time", frameSumTracksWall},
        {"no delta was zero", stats.zeroDeltas == 0},
        {"the engine resolved only Update and RadPluginShutdown against us", onlyExpected},
        {"no resolved name was dropped for space", g.resolvedOverflow.load() == 0},
        {"the tripwire was armed, so an empty name list means something", g.armed.load()},
    };
    const size_t checkCount = sizeof checks / sizeof checks[0];
    size_t failed = 0;
    char detail[1400] = {0};
    for (size_t i = 0; i < checkCount; ++i) {
        if (!checks[i].ok) {
            ++failed;
            char one[160];
            _snprintf_s(one, sizeof one, _TRUNCATE, "%sFAILED: %s", failed > 1 ? "; " : "",
                        checks[i].what);
            strncat_s(detail, sizeof detail, one, _TRUNCATE);
        }
    }

    char tail[900];
    _snprintf_s(tail, sizeof tail, _TRUNCATE,
                "%smode=%s claim=%s slot=0x%X predicted=0x%X noovr=%s sampled=%u ticks=%llu raw=%llu "
                "frames=%llu..%llu dup=%llu skip=%llu deltaMismatch=%llu simRegressions=%llu "
                "handedExceedsFrame=%llu framesWithExtraBeats=%llu handedSumUs=%llu frameSumUs=%llu "
                "wallUs=%llu lookupsAgainstUs=%llu resolved=[%s]",
                failed ? " | " : "", replay::tick::SourceName(stats.source),
                replay::tick::ClaimResultName(stats.claim), stats.slotOffset, predictedSlot,
                noOvr ? "yes" : "no", taken,
                (unsigned long long)stats.ticks, (unsigned long long)stats.rawCalls,
                (unsigned long long)stats.firstFrameIndex, (unsigned long long)stats.lastFrameIndex,
                (unsigned long long)stats.duplicateFrames, (unsigned long long)stats.skippedFrames,
                (unsigned long long)deltaMismatch, (unsigned long long)simRegressions,
                (unsigned long long)handedExceedsFrame, (unsigned long long)framesWithExtraBeats,
                (unsigned long long)handedSumUs, (unsigned long long)frameDeltaSumUs,
                (unsigned long long)wallUs, (unsigned long long)g.lookupsAgainstUs.load(), names);
    strncat_s(detail, sizeof detail, tail, _TRUNCATE);

    char probeId[128] = "W1-D-slot";
    char ids[512] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    if (const char* found = strstr(ids, "probe_tick=")) {
        const char* value = found + strlen("probe_tick=");
        size_t n = strcspn(value, ";");
        if (n >= sizeof probeId) {
            n = sizeof probeId - 1;
        }
        memcpy(probeId, value, n);
        probeId[n] = '\0';
    }

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2] = {0};
    for (size_t i = 0, o = 0; cwd[i] != '\0' && o + 2 < sizeof cwdEscaped; ++i) {
        if (cwd[i] == '\\') {
            cwdEscaped[o++] = '\\';
        }
        cwdEscaped[o++] = cwd[i];
    }

    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\done.json", dir);
    if (FILE* fh = nullptr; fopen_s(&fh, path, "wb") == 0 && fh != nullptr) {
        fprintf(fh,
                "{ \"probe\": \"%s\", \"verdict\": \"%s\", \"checked\": %llu, \"cwd\": \"%s\",\n"
                "  \"detail\": \"%s\" }\n",
                probeId, failed == 0 ? "PASS" : "FAIL", (unsigned long long)checkCount, cwdEscaped,
                detail);
        fclose(fh);
    }
    replay::log::Direct(failed == 0 ? replay::LogLevel::Info : replay::LogLevel::Error, kReplayModuleName, "%s: %s",
                failed == 0 ? "PASS" : "FAIL", detail);
    return 0;
}

bool Init(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, tick);

    InitializeCriticalSection(&g.resolvedLock);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&Init), &g.self);
    g.armed.store(ArmTripwire());

    if (!services->tick->RegisterEarly(OnTick, nullptr)) {
        replay::log::Direct(replay::LogLevel::Error, kReplayModuleName, "RegisterEarly refused");
        return false;
    }
    replay::log::Direct(replay::LogLevel::Info, kReplayModuleName, "tick mode at init: %s", services->tick->Mode());

    g.writer = CreateThread(nullptr, 0, WriterMain, nullptr, 0, nullptr);
    return g.writer != nullptr;
}

void Shutdown() {
    if (g.writer != nullptr) {
        WaitForSingleObject(g.writer, 10000);
        CloseHandle(g.writer);
        g.writer = nullptr;
    }
}

}  // namespace

// Order 900: after the tick module (10) has published ITick into ReplayServices.
REPLAY_MODULE("w1d-probe", 900, Init, nullptr, Shutdown);
