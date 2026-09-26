// W1-H's in-game probe. Queued through runs\queue\W1-H.probe.json; this packet never launches the
// game itself.
//
// ★ WHAT IT MEASURES, AND WHY IT IS ARRANGED AS FIVE ARMS RATHER THAN ONE.
//
// The claim under test is "a pose written into every view-state, with viewstate+0x9C cleared and the
// spectator CI mirrored, is what CCameraNode[0] renders". A single arm that writes one pose and
// finds it in the node cannot distinguish that from three other worlds: the node already held that
// pose; our write is not what moved it; or the pose arrived through the CI arm rather than the
// view-state arm. Each arm below exists to make one of those distinguishable, and every criterion
// this probe files has BOTH a construction that passes it and a construction that fails it, present
// in this same run:
//
//   arm 0  BASELINE   60 frames, nothing written. Records where the engine's own camera is. P0 is
//                     400 m above the arena, so `nodePos != P0` here is a known-answer control: if
//                     it does not hold, every later equality is vacuous and the run says
//                     COULD-NOT-DETERMINE rather than PASS.
//   arm A  WRITE P0   120 frames, the shipped configuration. `nodePos == P0` bit-exactly.
//   arm B  WRITE P1   120 frames, a DIFFERENT pose. `nodePos == P1`. This is what makes arm A mean
//                     "the node follows what we write" rather than "the node happened to hold P0".
//   arm C  CI ARM     120 frames, P0 again but with ci+0x274 cleared after each SetPose, so
//                     CAM.SPECTATOR_UPDATE takes CCameraNode[0] from the CI instead of the
//                     view-state. The pose is the same either way -- the mirror carries it -- but
//                     the CI arm writes node+0x98 with NO aspect conversion, so the aspect
//                     recovered from the node's horizontal FOV collapses to 1.0. That is the
//                     discriminator for WHICH arm drove the node, and arm A is its opposite.
//   arm D  RECOVERY   60 frames, back to the shipped configuration. ci+0x274 was left clear by arm
//                     C, so SetPose must find it clear and re-set it -- which is what proves
//                     `freeCamReasserts` can move at all. A counter that has only ever read zero
//                     has not been shown able to read anything else.
//
// ★ THE GATE COUNTER, WHICH IS THE ONE THE AUDIT CORRECTED. `Stats::gateReasserts` is cumulative
//   and is reset only in Init(), so "== 0 after the first frame" is unsatisfiable by construction:
//   every freshly constructed view-state has +0x9C == 1, so the first SetPose alone drives it to
//   viewCount. What is checkable is the pair -- it must RISE on the first write and then STAY FLAT
//   -- and both halves are in camera_criteria::GateReassertOutcome with their failing inputs named.
//
// ★ WHAT THIS PROBE CANNOT DO, stated here so its verdict is not read as more. It does not capture
//   a frame, so it does not prove the desktop WINDOW shows CCameraNode[0]; it proves the renderer's
//   own world-camera source follows our write. It does not render audio, so it does not prove the
//   Wwise listener panned; it proves the CI carries our pose, which is the input the listener arm
//   reads. Both gaps are in docs/spikes/W1-H.md S9 as unrunnable under the current constraint, not
//   as scheduled work.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bootstrap/bootstrap.h"
#include "bootstrap/egress.h"
#include "bootstrap/log.h"
#include "bootstrap/services.h"
#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"

#include "criteria.h"

namespace {

const char kReplayModuleName[] = "w1h-probe";

using namespace replay::camera_facts;
namespace crit = replay::camera_criteria;

// Frame budgets. The arms are counted in TICKS, never in wall clock: `offline-local` has never been
// launched, so its frame rate is unmeasured and a time-based schedule would silently shorten the
// arms on a slow machine. A run that ends before an arm completes reports that arm's criteria as
// COULD-NOT-DETERMINE; it never reports them as passing.
constexpr uint32_t kBaselineFrames = 60;
constexpr uint32_t kArmFrames      = 120;
constexpr uint32_t kRecoveryFrames = 60;

// P0 is 400 m above the arena floor. Nothing the engine's own camera does puts it there, which is
// what makes "the baseline is not already at P0" a control rather than a hope.
constexpr float kP0[3] = { 0.0f, 400.0f, 0.0f };
constexpr float kQ0[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
// P1 is displaced by an irregular vector so no axis-aligned confusion can make it equal P0, and
// rotated a quarter turn about Y so the quaternion differs in two lanes rather than one.
constexpr float kP1[3] = { 37.0f, 381.0f, 61.0f };
constexpr float kQ1[4] = { 0.0f, 0.70710678f, 0.0f, 0.70710678f };

constexpr float kFovA = 0.95993108f;   // 55 degrees, the shipped free_cam_settings.fov
constexpr float kFovB = 1.57079633f;   // 90 degrees

enum class Arm : uint8_t { Baseline, WriteP0, WriteP1, CiArm, Recovery, Done };

struct Sample {
    uint32_t frame;          // frames since the camera checked out
    uint8_t  arm;
    uint8_t  fovIndex;       // 0 = kFovA, 1 = kFovB
    replay::camera::Observation obs;
    uint64_t gateReasserts;
    uint64_t freeCamReasserts;
    uint64_t poses;
    uint64_t viewsWritten;
};

constexpr size_t kMaxSamples = 64;

struct ProbeState {
    HMODULE  self;
    uint64_t attachMs;

    std::atomic<bool> bootstrapOk;
    std::atomic<bool> tickRegistered;
    std::atomic<bool> finished;

    // Everything below is touched only on the tick thread, and read by the worker only after
    // `finished` has been released.
    replay::IReplayCamera*  camera;
    replay::camera::Refusal createRefusal;
    bool     createAttempted;
    bool     netGameSeen;
    uint32_t ticksSeen;
    uint32_t frame;
    Arm      arm;
    uint32_t armFrame;
    bool     armCompleted[6];

    Sample   samples[kMaxSamples];
    uint32_t sampleCount;

    // Counter readings taken at the arm boundaries, which is where the deltas that matter live.
    uint64_t gateBeforeFirstWrite;
    uint64_t gateAfterFirstWrite;
    uint64_t gateAtArmAEnd;
    uint64_t gateAtEnd;
    uint64_t freeCamAfterFirstWrite;
    uint64_t freeCamAtCiArmEnd;
    uint64_t freeCamAtEnd;

    uint32_t viewCountAtFirstWrite;
    float    baselineNodePos[3];
    bool     baselineValid;
    bool     released;
};

ProbeState g;

// -------------------------------------------------------------------------------------------
// Verdict filing. Everything here works with no bootstrap, no log and no services, because the
// whole point of filing from a static initialiser is that it runs when none of those did.
// -------------------------------------------------------------------------------------------

void JsonEscape(char* out, size_t cap, const char* in) {
    size_t o = 0;
    for (size_t i = 0; in[i] != 0 && o + 2 < cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x22 || c == 0x5C) {
            out[o++] = 0x5C;
            out[o++] = static_cast<char>(c);
        } else if (c < 0x20) {
            if (o + 7 >= cap) {
                break;
            }
            o += static_cast<size_t>(_snprintf_s(out + o, cap - o, _TRUNCATE, "\\u%04X", c));
        } else {
            out[o++] = static_cast<char>(c);
        }
    }
    out[o] = 0;
}

void DllStem(char* out, size_t cap) {
    char dll[MAX_PATH] = {0};
    GetModuleFileNameA(g.self, dll, sizeof dll);
    const char* base = strrchr(dll, 0x5C);
    const char* alt = strrchr(dll, '/');
    if (alt != nullptr && (base == nullptr || alt > base)) {
        base = alt;
    }
    strncpy_s(out, cap, base != nullptr ? base + 1 : dll, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) {
        *dot = 0;
    }
}

void ProbeDir(char* out, size_t cap) {
    char root[MAX_PATH] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root);
    char stem[MAX_PATH];
    DllStem(stem, sizeof stem);
    _snprintf_s(out, cap, _TRUNCATE, "%s%c%s", root, static_cast<char>(0x5C), stem);
    CreateDirectoryA(out, nullptr);
}

// ★ "W1-H-unknown", never a real id. An unresolved identity falls back to the option that cannot be
// mistaken for a result: a done.json whose `probe` does not match the plan is NOT-RUN, which is the
// correct reading of "this DLL could not tell which probe it was".
void ProbeId(char* out, size_t cap) {
    strncpy_s(out, cap, "W1-H-unknown", _TRUNCATE);
    char ids[1024] = {0};
    GetEnvironmentVariableA("REPLAY_PROBE_IDS", ids, sizeof ids);
    char stem[MAX_PATH];
    DllStem(stem, sizeof stem);
    for (char* entry = ids; *entry != 0;) {
        char* end = strchr(entry, ';');
        if (end != nullptr) {
            *end = 0;
        }
        if (char* eq = strchr(entry, '=')) {
            *eq = 0;
            if (_stricmp(entry, stem) == 0) {
                strncpy_s(out, cap, eq + 1, _TRUNCATE);
                return;
            }
        }
        if (end == nullptr) {
            break;
        }
        entry = end + 1;
    }
}

bool ProbeRootSet() {
    char root[MAX_PATH] = {0};
    return GetEnvironmentVariableA("REPLAY_PROBE_ROOT", root, sizeof root) != 0;
}

void OpenArtifact(FILE** fh, const char* name) {
    char dir[MAX_PATH];
    ProbeDir(dir, sizeof dir);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s%c%s", dir, static_cast<char>(0x5C), name);
    if (fopen_s(fh, path, "wb") != 0) {
        *fh = nullptr;
    }
}

struct Check {
    const char*   name;
    crit::Outcome outcome;
};

void WriteProvisional(const char* id) {
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    JsonEscape(cwdEscaped, sizeof cwdEscaped, cwd);
    FILE* fh = nullptr;
    OpenArtifact(&fh, "done.json");
    if (fh != nullptr) {
        fprintf(fh,
                "{\"probe\":\"%s\",\"verdict\":\"NOT-RUN\",\"checked\":0,\"provisional\":true,"
                "\"cwd\":\"%s\",\"detail\":\"attached and measuring; this file is provisional and is "
                "overwritten by the real verdict. If you are reading it, the process was killed "
                "first.\"}\n",
                id, cwdEscaped);
        fclose(fh);
    }
}

const char* ArmName(uint8_t arm) {
    static const char* kNames[] = { "baseline", "writeP0", "writeP1", "ciArm", "recovery", "done" };
    return arm < 6 ? kNames[arm] : "?";
}

void WriteFrames() {
    FILE* fh = nullptr;
    OpenArtifact(&fh, "frames.jsonl");
    if (fh == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < g.sampleCount; ++i) {
        const Sample& s = g.samples[i];
        const replay::camera::Observation& o = s.obs;
        fprintf(fh,
                "{\"frame\":%u,\"arm\":\"%s\",\"fovY\":%.9g,\"valid\":%s,\"viewCount\":%u,"
                "\"spectatorInstances\":%u,\"spectatorMode\":%d,\"freeCamActive\":%d,"
                "\"integratorEnabled\":%d,"
                "\"viewStatePos\":[%.9g,%.9g,%.9g],\"viewStateQuat\":[%.9g,%.9g,%.9g,%.9g],"
                "\"viewStateFovY\":%.9g,"
                "\"ciPos\":[%.9g,%.9g,%.9g],\"ciQuat\":[%.9g,%.9g,%.9g,%.9g],\"ciFovY\":%.9g,"
                "\"nodePos\":[%.9g,%.9g,%.9g],\"nodeQuat\":[%.9g,%.9g,%.9g,%.9g],"
                "\"nodeFovH\":%.9g,\"recoveredAspect\":%.9g,"
                "\"gateReasserts\":%llu,\"freeCamReasserts\":%llu,\"poses\":%llu,"
                "\"viewsWritten\":%llu}\n",
                s.frame, ArmName(s.arm), s.fovIndex == 0 ? kFovA : kFovB,
                o.valid ? "true" : "false", o.viewCount, o.spectatorInstances, o.spectatorMode,
                o.freeCamActive, o.integratorEnabled,
                o.viewStatePos[0], o.viewStatePos[1], o.viewStatePos[2],
                o.viewStateQuat[0], o.viewStateQuat[1], o.viewStateQuat[2], o.viewStateQuat[3],
                o.viewStateFovY,
                o.ciPos[0], o.ciPos[1], o.ciPos[2],
                o.ciQuat[0], o.ciQuat[1], o.ciQuat[2], o.ciQuat[3], o.ciFovY,
                o.nodePos[0], o.nodePos[1], o.nodePos[2],
                o.nodeQuat[0], o.nodeQuat[1], o.nodeQuat[2], o.nodeQuat[3], o.nodeFovH,
                crit::RecoverAspect(o.nodeFovH, s.fovIndex == 0 ? kFovA : kFovB),
                (unsigned long long)s.gateReasserts, (unsigned long long)s.freeCamReasserts,
                (unsigned long long)s.poses, (unsigned long long)s.viewsWritten);
    }
    fclose(fh);
}

void WriteVerdict(const char* id, const Check* checks, size_t count, const char* extra) {
    char detail[1800] = {0};
    crit::Outcome outcomes[16];
    // ★ `checked` counts what was actually EVALUATED, which is HARNESS 2.3's own wording. Reporting
    // the number of criteria we intended to evaluate would say "ten checks" for a launch that
    // reached no session and evaluated none of them, and the harness reads that number as evidence.
    unsigned evaluated = 0;
    for (size_t i = 0; i < count && i < 16; ++i) {
        outcomes[i] = checks[i].outcome;
        if (checks[i].outcome != crit::Outcome::Undetermined) {
            ++evaluated;
        }
        char one[192];
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s%s=%s", i != 0 ? "; " : "", checks[i].name,
                    crit::OutcomeName(checks[i].outcome));
        strncat_s(detail, sizeof detail, one, _TRUNCATE);
    }
    const char* verdict = crit::AggregateVerdict(outcomes, count < 16 ? count : 16);
    strncat_s(detail, sizeof detail, extra, _TRUNCATE);

    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof cwd, cwd);
    char cwdEscaped[MAX_PATH * 2];
    JsonEscape(cwdEscaped, sizeof cwdEscaped, cwd);
    char detailEscaped[sizeof detail * 2];
    JsonEscape(detailEscaped, sizeof detailEscaped, detail);

    // The hook-name list is a growable field, so it is bounded HERE rather than being allowed to
    // truncate the document. Sweep 4 lost a verdict to exactly that: a writer cut off mid-string
    // inside its hooks array and the whole file became unparseable.
    char hooks[256] = {0};
    strncpy_s(hooks, sizeof hooks, replay::egress::HookNamesJson(), _TRUNCATE);
    const replay::egress::Report& eg = replay::egress::Current();

    FILE* fh = nullptr;
    OpenArtifact(&fh, "done.json");
    if (fh == nullptr) {
        return;
    }
    fprintf(fh,
            "{\"probe\":\"%s\",\"verdict\":\"%s\",\"checked\":%u,\"cwd\":\"%s\","
            "\"egress\":{\"armed\":%s,\"state\":\"%s\",\"armedAtMs\":%llu,\"refused\":%llu,"
            "\"allowedLocal\":%llu,\"hooksLive\":%u,\"hooksRequested\":%u,\"hooks\":[%s],"
            "\"firstSocketAfterMs\":%lld},"
            "\"detail\":\"%s\"}\n",
            id, verdict, evaluated, cwdEscaped,
            eg.state == replay::egress::ArmState::Armed ? "true" : "false",
            replay::egress::ArmStateName(eg.state), (unsigned long long)eg.armedAtMs,
            (unsigned long long)eg.refusedRemote, (unsigned long long)eg.allowedLocal,
            eg.hooksLive, eg.hooksRequested, hooks, (long long)eg.firstSocketAfterMs,
            detailEscaped);
    fclose(fh);
}

// -------------------------------------------------------------------------------------------
// Engine access beyond the shipped module's interface. There is exactly one of these -- arm C's
// clear of SPECTATOR.CI_FREECAM_ACTIVE_OFF -- and it resolves the component instance through the
// same facts header the shipped module uses, so a drift in either is a drift in both.
// -------------------------------------------------------------------------------------------

uintptr_t Slide() {
    const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return module == 0 ? 0 : module - static_cast<uintptr_t>(kImageBase);
}

bool ReadableRange(uintptr_t va, size_t len) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(va), &mbi, sizeof mbi) != sizeof mbi) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0 || (mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return va + len <= end;
}

// ★ Is this process the build the packet's numbers describe? Asked by USAGE, not by name: the
// question that matters is whether CAM.VIEW_COMMIT's recorded bytes are at the address the facts
// header puts them, because if they are not, no amount of waiting will produce a measurement. A
// filename test would say "yes" for a renamed binary and "no" for a correct one.
//
// This is also what lets the offline reporting proof finish in seconds instead of sitting out the
// in-game deadline: in the test host the bytes are not there, so the probe files at once with every
// criterion COULD-NOT-DETERMINE, which is the truthful verdict for "there is no engine here".
bool HostCarriesTheGuardedSites() {
    if (GetModuleHandleW(nullptr) == nullptr) {
        return false;
    }
    const uintptr_t commit = static_cast<uintptr_t>(kViewCommitVA) + Slide();
    if (!ReadableRange(commit, sizeof kViewCommitPrologue)) {
        return false;
    }
    return memcmp(reinterpret_cast<const void*>(commit), kViewCommitPrologue,
                  sizeof kViewCommitPrologue) == 0;
}

uintptr_t ResolveLocalInstance(void* netGame) {
    const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (module == 0 || netGame == nullptr) {
        return 0;
    }
    // The same resolve the module performs, by calling it -- not a second copy of it. A probe with
    // its own derivation certifies its own bug: this one used to read the "gamespace" from
    // netGame+0x2B10, which is the spectator CS itself.
    const auto lookup = reinterpret_cast<replay::camera::detail::LookupComponentSystemFn>(
        static_cast<uintptr_t>(facts::CS_LOOKUP_BY_SYMBOL) + Slide());
    const replay::camera::detail::Resolution r =
        replay::camera::detail::ResolveSpectatorCs(netGame, lookup);
    if (r.refusal != replay::camera::Refusal::None) {
        return 0;
    }
    const uintptr_t cs = static_cast<uintptr_t>(r.csFromLookup);
    const uint16_t count = *reinterpret_cast<uint16_t*>(cs + kCsInstanceCountOff);
    const uint16_t local = *reinterpret_cast<uint16_t*>(cs + kCsLocalIndexOff);
    if (local >= count) {
        return 0;
    }
    const uintptr_t base = *reinterpret_cast<uintptr_t*>(cs + kCsInstanceBaseOff);
    if (base == 0) {
        return 0;
    }
    return base + static_cast<uintptr_t>(local) * kCiStride;
}

// ★ Arm C's only engine write beyond SetPose. Clearing ci+0x274 makes CAM.SPECTATOR_UPDATE take
// CCameraNode[0] from the CI rather than the view-state -- the state the memo's documented fallback
// would run in. It is written AFTER SetPose, so the next frame's SetPose finds it clear and re-sets
// it, which is what makes `freeCamReasserts` an observable instrument rather than an untested zero.
void ClearFreeCamFlag(void* netGame) {
    const uintptr_t ci = ResolveLocalInstance(netGame);
    if (ci != 0) {
        *reinterpret_cast<int32_t*>(ci + kCiFreeCamActiveOff) = 0;
    }
}

// Hands the camera back at the end of the run. The launch ends in taskkill either way, but leaving
// the engine's own free-fly integrator disabled for the remaining seconds is a state this probe
// created and should not walk away from.
void ReleaseCamera() {
    const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (module == 0) {
        return;
    }
    const uintptr_t game = *reinterpret_cast<uintptr_t*>(module + facts::GAME_SINGLETON_RVA);
    if (game == 0) {
        return;
    }
    const uintptr_t viewStates = *reinterpret_cast<uintptr_t*>(game + kViewStateArrayOff);
    if (viewStates == 0) {
        return;
    }
    uint32_t views = *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(kViewCountGlobalVA) +
                                                  Slide());
    if (views > kMaxViews) {
        views = kMaxViews;
    }
    for (uint32_t i = 0; i < views; ++i) {
        const uintptr_t vs = *reinterpret_cast<uintptr_t*>(viewStates + i * sizeof(void*));
        if (vs != 0) {
            *reinterpret_cast<int32_t*>(vs + kViewStateInputEnaOff) = 1;
        }
    }
    g.released = true;
}

// -------------------------------------------------------------------------------------------
// The tick. Every engine write this probe performs happens here, on the engine's own stage-0
// thread, and nothing is written before the camera module has checked out.
// -------------------------------------------------------------------------------------------

uint64_t GateCount() {
    replay::camera::Stats st{};
    replay::camera::GetStats(&st);
    return st.gateReasserts;
}

uint64_t FreeCamCount() {
    replay::camera::Stats st{};
    replay::camera::GetStats(&st);
    return st.freeCamReasserts;
}

void TakeSample(uint8_t fovIndex) {
    if (g.sampleCount >= kMaxSamples) {
        return;
    }
    Sample& s = g.samples[g.sampleCount];
    s.frame = g.frame;
    s.arm = static_cast<uint8_t>(g.arm);
    s.fovIndex = fovIndex;
    replay::camera::Observe(&s.obs);
    replay::camera::Stats st{};
    replay::camera::GetStats(&st);
    s.gateReasserts = st.gateReasserts;
    s.freeCamReasserts = st.freeCamReasserts;
    s.poses = st.poses;
    s.viewsWritten = st.viewsWritten;
    ++g.sampleCount;
}

void OnTick(const replay::TickInfo& info, void* ctx) {
    (void)ctx;
    ++g.ticksSeen;
    if (info.netGame != nullptr) {
        g.netGameSeen = true;
    }
    if (g.finished.load(std::memory_order_acquire)) {
        return;
    }

    if (g.camera == nullptr) {
        if (info.netGame == nullptr) {
            return;   // no session yet; write nothing at all
        }
        g.createAttempted = true;
        g.camera = replay::camera::Create(info.netGame);
        replay::camera::Stats st{};
        replay::camera::GetStats(&st);
        g.createRefusal = st.refusal;
        if (g.camera == nullptr) {
            // A refusal is a result. Stop, and let the worker file it with the name attached.
            g.finished.store(true, std::memory_order_release);
            return;
        }
        g.arm = Arm::Baseline;
        g.armFrame = 0;
        g.frame = 0;
    }

    const uint8_t fovIndex = (g.armFrame >= kArmFrames / 2) ? 1 : 0;
    const float fovY = fovIndex == 0 ? kFovA : kFovB;

    switch (g.arm) {
        case Arm::Baseline:
            // Nothing is written. This records where the engine's own camera is, which is the only
            // thing that makes a later equality mean anything.
            if (g.armFrame == kBaselineFrames - 1) {
                TakeSample(fovIndex);
                replay::camera::Observation obs{};
                if (replay::camera::Observe(&obs) && obs.valid) {
                    g.baselineNodePos[0] = obs.nodePos[0];
                    g.baselineNodePos[1] = obs.nodePos[1];
                    g.baselineNodePos[2] = obs.nodePos[2];
                    g.baselineValid = true;
                }
                g.gateBeforeFirstWrite = GateCount();
            }
            break;

        case Arm::WriteP0:
            g.camera->SetPose(kP0, kQ0, fovY);
            if (g.armFrame == 0) {
                g.gateAfterFirstWrite = GateCount();
                g.freeCamAfterFirstWrite = FreeCamCount();
                replay::camera::Stats st{};
                replay::camera::GetStats(&st);
                g.viewCountAtFirstWrite = st.viewCount;
            }
            if (g.armFrame == 1 || g.armFrame == 2 || g.armFrame == kArmFrames / 2 - 1 ||
                g.armFrame == kArmFrames / 2 + 1 || g.armFrame == kArmFrames - 1) {
                TakeSample(fovIndex);
            }
            if (g.armFrame == kArmFrames - 1) {
                g.gateAtArmAEnd = GateCount();
            }
            break;

        case Arm::WriteP1:
            g.camera->SetPose(kP1, kQ1, fovY);
            if (g.armFrame == 1 || g.armFrame == 2 || g.armFrame == kArmFrames - 1) {
                TakeSample(fovIndex);
            }
            break;

        case Arm::CiArm:
            g.camera->SetPose(kP0, kQ0, fovY);
            ClearFreeCamFlag(info.netGame);
            if (g.armFrame == 1 || g.armFrame == 2 || g.armFrame == kArmFrames / 2 + 1 ||
                g.armFrame == kArmFrames - 1) {
                TakeSample(fovIndex);
            }
            if (g.armFrame == kArmFrames - 1) {
                g.freeCamAtCiArmEnd = FreeCamCount();
            }
            break;

        case Arm::Recovery:
            g.camera->SetPose(kP0, kQ0, kFovA);
            if (g.armFrame == 1 || g.armFrame == kRecoveryFrames - 1) {
                TakeSample(0);
            }
            break;

        case Arm::Done:
            return;
    }

    ++g.frame;
    ++g.armFrame;
    const uint32_t armLength = g.arm == Arm::Baseline   ? kBaselineFrames
                               : g.arm == Arm::Recovery ? kRecoveryFrames
                                                        : kArmFrames;
    if (g.armFrame >= armLength) {
        g.armCompleted[static_cast<uint8_t>(g.arm)] = true;
        g.armFrame = 0;
        if (g.arm == Arm::Recovery) {
            g.gateAtEnd = GateCount();
            g.freeCamAtEnd = FreeCamCount();
            ReleaseCamera();
            g.arm = Arm::Done;
            g.finished.store(true, std::memory_order_release);
            return;
        }
        g.arm = static_cast<Arm>(static_cast<uint8_t>(g.arm) + 1);
    }
}

// -------------------------------------------------------------------------------------------
// Scoring. Runs on the worker after the tick thread has stopped writing.
// -------------------------------------------------------------------------------------------

const Sample* LastSample(Arm arm, int fovIndex) {
    for (uint32_t i = g.sampleCount; i > 0; --i) {
        const Sample& s = g.samples[i - 1];
        if (s.arm == static_cast<uint8_t>(arm) && s.obs.valid &&
            (fovIndex < 0 || s.fovIndex == fovIndex)) {
            return &s;
        }
    }
    return nullptr;
}

// Every valid sample of `arm` must carry the pose that arm wrote. Undetermined when the arm
// produced no valid sample at all -- which is what a launch that ended early looks like.
crit::Outcome ScoreTracks(Arm arm, const float pos[3], const float quat[4]) {
    bool any = false;
    bool all = true;
    for (uint32_t i = 0; i < g.sampleCount; ++i) {
        const Sample& s = g.samples[i];
        if (s.arm != static_cast<uint8_t>(arm) || !s.obs.valid) {
            continue;
        }
        any = true;
        if (!crit::SameVec3(s.obs.nodePos, pos) || !crit::SameVec4(s.obs.nodeQuat, quat)) {
            all = false;
        }
    }
    if (!any) {
        return crit::Outcome::Undetermined;
    }
    return all ? crit::Outcome::Ok : crit::Outcome::Failed;
}

crit::Outcome ScoreGateHeld() {
    bool any = false;
    bool all = true;
    for (uint32_t i = 0; i < g.sampleCount; ++i) {
        const Sample& s = g.samples[i];
        if (s.arm != static_cast<uint8_t>(Arm::WriteP0) || !s.obs.valid) {
            continue;
        }
        any = true;
        if (s.obs.integratorEnabled != 0) {
            all = false;
        }
    }
    if (!any) {
        return crit::Outcome::Undetermined;
    }
    return all ? crit::Outcome::Ok : crit::Outcome::Failed;
}

DWORD WINAPI WorkerMain(LPVOID) {
    // Layer 3 of the harness's no-server machinery, armed on this thread's first instruction
    // because MH_EnableHook suspends threads and must never run under the loader lock.
    replay::egress::Arm();

    char id[128];
    ProbeId(id, sizeof id);

    // A ceiling, not a schedule: `offline-local` has never been launched, so how long it takes to
    // reach a session is unmeasured. The probe files whatever it has when this expires, and the
    // harness's own deadline is the outer bound either way. If the host is not the guarded build
    // there is nothing to wait FOR, and waiting anyway would turn "no engine here" into a
    // provisional record and a killed process.
    const bool onTarget = HostCarriesTheGuardedSites();
    const uint64_t deadlineMs = GetTickCount64() + (onTarget ? 280000ull : 0ull);
    while (!g.finished.load(std::memory_order_acquire) && GetTickCount64() < deadlineMs) {
        Sleep(50);
    }

    WriteFrames();

    Check checks[10];
    size_t n = 0;
    char extra[700] = {0};

    // C1 -- the write point checked out at all. Failing input: any of the eight refusals, each of
    // which names itself. Passing input: a session carrying an R14NetSpectatorCamera component
    // system with a local instance. Undetermined: no session ever appeared, so Create() was never
    // called and there is nothing to say about the write point.
    checks[n].name = "camera-available";
    checks[n].outcome = !g.createAttempted ? crit::Outcome::Undetermined
                        : g.camera != nullptr ? crit::Outcome::Ok
                                              : crit::Outcome::Failed;
    ++n;

    // C2 -- the known-answer control. If this does not hold, every equality below is vacuous, so
    // they are all forced undetermined rather than being allowed to pass on a coincidence.
    const crit::Outcome baseline =
        crit::BaselineControlOutcome(g.baselineValid, g.baselineNodePos, kP0);
    checks[n].name = "baseline-not-already-at-P0";
    checks[n].outcome = baseline;
    ++n;
    const bool controlHolds = baseline == crit::Outcome::Ok;

    // C3 / C4 -- the node carries the pose each arm wrote. C4 is what makes C3 mean "the node
    // follows what we write": a node stuck at P0 for any other reason passes C3 and fails C4.
    checks[n].name = "node-tracks-P0";
    checks[n].outcome = controlHolds ? ScoreTracks(Arm::WriteP0, kP0, kQ0)
                                     : crit::Outcome::Undetermined;
    ++n;
    checks[n].name = "node-tracks-P1";
    checks[n].outcome = controlHolds ? ScoreTracks(Arm::WriteP1, kP1, kQ1)
                                     : crit::Outcome::Undetermined;
    ++n;

    // C5 -- the integrator gate held for the whole of arm A. Failing input: any frame on which the
    // engine re-armed it, which is what CAM.ENTER_FREECAM does when it replaces the view-states.
    checks[n].name = "integrator-gate-held";
    checks[n].outcome = ScoreGateHeld();
    ++n;

    // C6 ★ -- the corrected discriminating number, in the only form that can both pass and fail.
    checks[n].name = "gate-reasserts-rise-then-flat";
    checks[n].outcome = crit::GateReassertOutcome(
        g.gateBeforeFirstWrite, g.gateAfterFirstWrite, g.gateAtArmAEnd, g.viewCountAtFirstWrite,
        g.armCompleted[static_cast<uint8_t>(Arm::WriteP0)]);
    ++n;

    // C7 ★ -- ci+0x274's reassert counter proved to be an instrument, by arm C moving it and arm D
    // letting it settle.
    checks[n].name = "freecam-flag-reassert-instrument";
    checks[n].outcome = crit::FreeCamInstrumentOutcome(
        g.freeCamAfterFirstWrite, g.freeCamAtCiArmEnd, g.freeCamAtEnd, kArmFrames,
        g.armCompleted[static_cast<uint8_t>(Arm::CiArm)],
        g.armCompleted[static_cast<uint8_t>(Arm::Recovery)]);
    ++n;

    // C8 / C9 ★ -- which arm drove CCameraNode[0], read out of the FOV conversion. Arm A and arm C
    // are each other's control and neither reading could pass in the other's place.
    float aspectA0 = -1.0f;
    float aspectA1 = -1.0f;
    float aspectCi = -1.0f;
    if (const Sample* s = LastSample(Arm::WriteP0, 0)) {
        aspectA0 = crit::RecoverAspect(s->obs.nodeFovH, kFovA);
    }
    if (const Sample* s = LastSample(Arm::WriteP0, 1)) {
        aspectA1 = crit::RecoverAspect(s->obs.nodeFovH, kFovB);
    }
    if (const Sample* s = LastSample(Arm::CiArm, 1)) {
        aspectCi = crit::RecoverAspect(s->obs.nodeFovH, kFovB);
    }
    checks[n].name = "viewstate-arm-drives-node-fov";
    checks[n].outcome = crit::ViewStateArmOutcome(aspectA0, aspectA1);
    ++n;
    checks[n].name = "ci-arm-fov-unconverted";
    checks[n].outcome = crit::CiArmOutcome(aspectCi, aspectA0);
    ++n;

    // C10 -- the SetCameraAPI ABI, measured rather than assumed: after the mirror call the CI must
    // carry our pose and the mode must be 6. This is not a tautology -- WE did not perform the
    // store, CAM.SETCAMERA_API_IMPL did, from the struct we passed in rdx. A wrong prototype puts
    // caller garbage there and this fails.
    {
        const Sample* s = LastSample(Arm::WriteP0, -1);
        checks[n].name = "ci-mirror-carries-pose-and-mode-6";
        checks[n].outcome =
            s == nullptr ? crit::Outcome::Undetermined
                         : (crit::SameVec3(s->obs.ciPos, kP0) && s->obs.spectatorMode == kModeApi)
                               ? crit::Outcome::Ok
                               : crit::Outcome::Failed;
        ++n;
    }

    _snprintf_s(extra, sizeof extra, _TRUNCATE,
                " | onTarget=%s refusal=%s ticks=%u netGameSeen=%s samples=%u lastArm=%s "
                "released=%s bootstrap=%s tickRegistered=%s"
                " | gate before=%llu afterFirstWrite=%llu armAEnd=%llu end=%llu viewCount=%u"
                " | freeCam afterFirstWrite=%llu ciArmEnd=%llu end=%llu"
                " | aspect A@55=%.6g A@90=%.6g C@90=%.6g",
                onTarget ? "true" : "false",
                replay::camera::RefusalName(g.createRefusal), g.ticksSeen,
                g.netGameSeen ? "true" : "false", g.sampleCount,
                ArmName(static_cast<uint8_t>(g.arm)), g.released ? "true" : "false",
                g.bootstrapOk.load() ? "true" : "false",
                g.tickRegistered.load() ? "true" : "false",
                (unsigned long long)g.gateBeforeFirstWrite,
                (unsigned long long)g.gateAfterFirstWrite, (unsigned long long)g.gateAtArmAEnd,
                (unsigned long long)g.gateAtEnd, g.viewCountAtFirstWrite,
                (unsigned long long)g.freeCamAfterFirstWrite,
                (unsigned long long)g.freeCamAtCiArmEnd, (unsigned long long)g.freeCamAtEnd,
                aspectA0, aspectA1, aspectCi);

    // ★ Ruling 4c, the in-vivo half: which of the two pose owners this launch actually had, and
    // whether the exclusion ever had to fire.
    //
    // Read this line for what it CAN say (AUDIT_I finding 3 -- the earlier reading was not
    // executable). This probe's only claim site is camera::Create -> ClaimPoseOwner(WritePoint), so
    // when this DLL is the image that ran, the pose owner here is WritePoint and never
    // StockFreeCam; the StockFreeCam line comes from camera_module in echoreplay.dll. And the two
    // never both run: bootstrap.cpp:423 refuses the single-instance claim to every image but one
    // and :450 starts WorkerMain -- hence modreg::InitAll and every module Init -- only past it. A
    // launch therefore cannot stage a cross-image conflict; the two- and three-image cases in
    // tests/camera/writepoint/test_poseowner_images.cpp prove that arm offline.
    //
    // What this line witnesses in vivo is the invariant that makes the offline proof sufficient:
    // `arb created#1`, from exactly one image, with `conflicts 0`. An `opened#` here would mean a
    // SECOND image mapped the section and initialised -- instance exclusivity violated -- and is a
    // FAIL, not a healthier-looking variant of `created#`.
    char poseOwner[192] = {0};
    _snprintf_s(poseOwner, sizeof poseOwner, _TRUNCATE,
                " | pose-owner: %s, conflicts %llu, arb %s, rel-refused %llu",
                replay::camera::PoseOwnerName(replay::camera::PoseOwnerHeld()),
                (unsigned long long)replay::camera::PoseOwnerConflicts(),
                replay::camera::PoseOwnerArbiter(),
                (unsigned long long)replay::camera::PoseOwnerReleaseRefusals());
    strncat_s(extra, sizeof extra, poseOwner, _TRUNCATE);

    WriteVerdict(id, checks, n, extra);
    return 0;
}

bool Init(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, tick);
    if (!services->tick->RegisterEarly(OnTick, nullptr)) {
        replay::log::Direct(replay::LogLevel::Error, kReplayModuleName, "RegisterEarly refused");
        return false;
    }
    g.tickRegistered.store(true, std::memory_order_release);
    replay::log::Direct(replay::LogLevel::Info, kReplayModuleName, "tick mode at init: %s",
                        services->tick->Mode());
    return true;
}

// Order 900: after the tick module has published ITick into ReplayServices.
REPLAY_MODULE("w1h-probe", 900, Init, nullptr, nullptr);

// ★ THE VERDICT IS FILED FROM HERE, before DllMain and therefore before the instance claim is even
// attempted. One code path, always taken: whether the bootstrap runs, is refused, or never gets the
// chance. In a multi-probe launch the loader takes DLLs in filename order and coexist::ClaimInstance
// refuses every one after the first, so a verdict written from a module Init is never written at
// all. tests/camera proves this offline against the real refusal, so no launch is spent on it.
struct CameraAttachFiler {
    CameraAttachFiler();
};

CameraAttachFiler::CameraAttachFiler() {
    if (!ProbeRootSet()) {
        return;   // not in a sweep: never scatter files into somebody's install
    }
    g.attachMs = GetTickCount64();
    // Pin, because our worker outlives DllMain and a refused bootstrap would otherwise let the
    // loader unmap the module out from under it.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&WriteProvisional), &g.self);

    char id[128];
    ProbeId(id, sizeof id);
    WriteProvisional(id);

    if (HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr)) {
        CloseHandle(worker);
    }
}

#if !W1H_NO_STATIC_FILER
CameraAttachFiler g_filer;
#else
// ★ THE MUTATION BUILD reproduces the OLD shape rather than deleting the new one: it files from a
// REPLAY_MODULE init, exactly as a probe did in sweep 4. So it files normally when it is the first
// DLL loaded and goes SILENT when it is the refused one -- which is the defect itself, not an
// approximation of it. tests/camera asserts both halves.
bool ProbeInitLegacy(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    if (HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr)) {
        CloseHandle(worker);
    }
    return true;
}

REPLAY_MODULE("w1h-probe-legacy", 901, ProbeInitLegacy, nullptr, nullptr);
#endif

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (g.self == nullptr) {   // the mutation build has no static filer to have set these
                g.self = module;
                g.attachMs = GetTickCount64();
            }
            // The verdict and the worker are already running. Attach is attempted for the log, the
            // module registry and the shared blocker, and its result is REPORTED rather than
            // obeyed: returning FALSE would ask the loader to unload a module whose thread is
            // already measuring, and a refused instance claim is a normal outcome in a multi-probe
            // launch rather than a reason to disappear.
            g.bootstrapOk.store(replay::bootstrap::Attach(module), std::memory_order_release);
            return TRUE;
        case DLL_PROCESS_DETACH:
            replay::bootstrap::Detach(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
