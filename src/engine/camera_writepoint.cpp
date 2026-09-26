#include "engine/camera_writepoint.h"

#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/tableguard.h"
#include "engine/camera_writepoint_facts.h"
#include "generated/facts_generated.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace replay {
namespace camera {
namespace {

using namespace camera_facts;

// CAM.SETCAMERA_API_IMPL. Called directly rather than through CAM.SETCAMERA_API, whose wrapper
// may push the write onto a CDeferredMethodQueue instead of performing it -- and a pose that
// lands at an unspecified later point is not the exact function of sim time S12.5.1 requires.
using SetCameraApiFn = void(__fastcall*)(void* cs, const void* transfQ, float fovY);

// The 32 bytes CAM.SETCAMERA_API_IMPL copies into the component instance: 16 at ci+0x1C8 from
// offset 0, then 16 at ci+0x1D8 from offset 16. That second store is 16 bytes wide and covers
// SPECTATOR.CI_SCALE_OFF, which CAM.SPECTATOR_UPDATE replicates into the audio listener's scale
// lanes. The shipped /camera_transform handler builds this half from a splat of 1.0f and
// overwrites only x, y and z -- so `scale` is 1.0f here for the same reason.
struct CTransfQ {
    float quat[4];
    float pos[3];
    float scale;
};
static_assert(sizeof(CTransfQ) == 32, "CTransfQ is the 32 bytes SetCameraAPI reads");

// ★ A GUARD THAT FAULTS HAS NOT REFUSED. This used to be a bare memcmp at the derived address.
// Six sites are compared, two of them at mid-function offsets, and every one of them is derived
// from a fact recorded against a 35 MB image -- so on a short, or simply different, image the
// address is past the end of what is mapped and the compare takes an access violation instead of
// answering. The unmapped case is not an edge case here: it is EXACTLY the case the guard exists
// to catch, and the design says the outcome is "a clean refusal with a log line, never a
// corruption."
//
// So a site has three answers, never two. replay::Readable (bootstrap/buildguard.h -- the probe
// every check in this plugin uses, imported rather than reimplemented) is asked for the FULL
// compare span before the compare, and "could not ask" stays its own outcome all the way out to
// camera::Refusal.
enum class SiteVerdict { Match, Mismatch, Unreadable };

constexpr size_t kPrologueLen = 20;

struct GuardedSite {
    const char*    name;
    uint64_t       va;    // the fact's PC_VA, before the slide
    const uint8_t* want;  // kPrologueLen bytes
};

SiteVerdict CheckSite(uintptr_t at, const uint8_t* want) {
#ifndef W1H_MUTATE_NO_READABLE_PROBE
    if (!Readable(reinterpret_cast<const void*>(at), kPrologueLen)) {
        return SiteVerdict::Unreadable;
    }
#endif
    return std::memcmp(reinterpret_cast<const void*>(at), want, kPrologueLen) == 0
               ? SiteVerdict::Match
               : SiteVerdict::Mismatch;
}

template <typename T>
T Load(uintptr_t p) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(p), sizeof(T));
    return v;
}

template <typename T>
void Store(uintptr_t p, const T& v) {
    std::memcpy(reinterpret_cast<void*>(p), &v, sizeof(T));
}

class Camera final : public IReplayCamera {
public:
    void Init(void* netGame) {
        stats_ = Stats{};
        stats_.refusal = Refusal::None;

        const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!module) return Refuse(Refusal::NoModule);
        slide_ = module - static_cast<uintptr_t>(kImageBase);

        // Every site we call or depend on, guarded before anything is written. The two integrator
        // gates are included even though we never call them: the write point's whole correctness
        // rests on `cmp dword ptr [reg+9Ch], 0` still gating those bodies.
        const detail::GuardResult guard = detail::CheckGuardedSites(slide_);
        if (guard.refusal != Refusal::None) {
            std::memcpy(stats_.prologueWhy, guard.why, sizeof(stats_.prologueWhy));
            return Refuse(guard.refusal);
        }

        game_ = Load<uintptr_t>(module + facts::GAME_SINGLETON_RVA);
        if (!game_) return Refuse(Refusal::NoGame);

        if (!ViewStates()) return Refuse(Refusal::NoViewStates);

        const auto lookup = reinterpret_cast<detail::LookupComponentSystemFn>(
            Va(facts::CS_LOOKUP_BY_SYMBOL));
        const detail::Resolution r = detail::ResolveSpectatorCs(netGame, lookup);
        stats_.levelSpace    = r.levelSpace;
        stats_.csFromLookup  = r.csFromLookup;
        stats_.csSpace       = r.csSpace;
        stats_.csFromCsSpace = r.csFromCsSpace;
        stats_.spaceTable    = r.spaceTable;
        stats_.csSpaceTable  = r.csSpaceTable;
        std::snprintf(stats_.resolveWhy, sizeof stats_.resolveWhy, "%s", r.why);
        if (r.refusal != Refusal::None) return Refuse(r.refusal);
        spectatorCs_ = static_cast<uintptr_t>(r.csFromLookup);

        if (!Readable(reinterpret_cast<const void*>(spectatorCs_), kCsLocalIndexOff + sizeof(uint16_t))) {
            return Refuse(Refusal::SpectatorCsUnreadable);
        }
        if (!LocalInstance()) return Refuse(Refusal::NoLocalInstance);

        setCameraApi_ = reinterpret_cast<SetCameraApiFn>(Va(kSetCameraApiImplVA));
        stats_.available = true;
    }

    // Refused before Init ran, so nothing was resolved and nothing in the engine was touched.
    void RefusePoseOwner() {
        stats_ = Stats{};
        stats_.refusal = Refusal::PoseOwnerConflict;
    }

    // Back to the never-created state -- but not before the engine is put back too. SetPose
    // disarmed the stock free camera's integrator gate on every view it wrote and nothing in
    // echovr.exe re-arms it (W1-H S4), so a Relinquish that restored nothing left the owner's
    // free camera dead for the session (ARCH-17 S3.3(c)). The re-arm happens FIRST: after the
    // token is dropped the stock free camera may already be flying against a cleared gate.
    //
    // The three restore counts outlive the reset because the reset would otherwise erase the
    // only evidence that the hand-back happened at all.
    void Relinquish() {
        Stats handback{};
        detail::RestoreViewGates(handback);

        stats_                = Stats{};
        stats_.gateRestored   = handback.gateRestored;
        stats_.gateNotOurs    = handback.gateNotOurs;
        stats_.gateUnreadable = handback.gateUnreadable;
        spectatorCs_  = 0;
        setCameraApi_ = nullptr;
    }

    void SetPose(const float pos[3], const float quat[4], float fovYRadians) override {
        // THE write path, and the reason the gate is here rather than only
        // on the console verbs: everything below this line -- the zeroed per-view input gate, the
        // quat/pos/FOV stores and the free-cam flag -- lands in the LIVE session's view states.
        // A verb-only refusal would leave the next caller of this method ungated.
        if (!livesafety::Allowed(livesafety::Feature::Camera)) {
            static bool told = false;
            if (!told) {
                told = true;
                log::Direct(LogLevel::Warn, "camera",
                            "camera pose write refused: %s -- the spectator camera stays the "
                            "engine's",
                            livesafety::VerdictReason(livesafety::Current()));
            }
            return;
        }
        if (!stats_.available) return;

        // The engine clamps its own per-view loops at 8 and yields a null node beyond that
        // (VIEWSTATE.MAX_VIEWS), so the count is only ever a lower bound on what to write.
        const uintptr_t viewStates = ViewStates();
        if (!viewStates) return;

        detail::WriteViewStates(viewStates, Load<uint32_t>(Va(kViewCountGlobalVA)), pos, quat,
                                fovYRadians, stats_);

        const uintptr_t ci = LocalInstance();
        if (ci) {
            // ci+0x274 non-zero makes CAM.SPECTATOR_UPDATE take CCameraNode[0] from the view-state
            // rather than from the CI. That is the arm we want: it is the one whose FOV goes
            // through the commit point's aspect conversion, so we never have to reproduce the
            // viewport aspect ourselves.
            if (Load<int32_t>(ci + kCiFreeCamActiveOff) == 0) {
                Store<int32_t>(ci + kCiFreeCamActiveOff, 1);
                ++stats_.freeCamReasserts;
            }

            // Mirror the same pose into the CI. The camera does not need it -- the view-state arm
            // above already owns CCameraNode[0] -- but the audio listener does: CAM.SPECTATOR_UPDATE
            // feeds the Wwise listener from the view-state ONLY when ci+0x3C == 5, and from the CI
            // in every other mode. Writing both means the listener is correct whichever arm the
            // spectator state machine is in, without us forcing it into free-cam mode.
            const CTransfQ t = { { quat[0], quat[1], quat[2], quat[3] },
                                 { pos[0], pos[1], pos[2] },
                                 1.0f };
            setCameraApi_(reinterpret_cast<void*>(spectatorCs_), &t, fovYRadians);
            ++stats_.ciWrites;
        }

        ++stats_.poses;
    }

    bool        AppliesToAllViews() const override { return true; }
    const char* Strategy() const override { return "viewstate+api"; }

    const Stats& GetStats() const { return stats_; }

    bool Observe(Observation* out) const {
        *out = Observation{};
        if (!stats_.available) return false;

        uint32_t views = Load<uint32_t>(Va(kViewCountGlobalVA));
        if (views > kMaxViews) views = kMaxViews;
        out->viewCount = views;

        const uintptr_t viewStates = ViewStates();
        if (!viewStates) return false;
        const uintptr_t vs = Load<uintptr_t>(viewStates);
        if (!vs) return false;
        out->integratorEnabled = Load<int32_t>(vs + kViewStateInputEnaOff);
        std::memcpy(out->viewStateQuat, reinterpret_cast<const void*>(vs + kViewStateQuatOff), 16);
        std::memcpy(out->viewStatePos, reinterpret_cast<const void*>(vs + kViewStatePosOff), 12);
        out->viewStateFovY = Load<float>(vs + kViewStateFovYOff);

        out->spectatorInstances = Load<uint16_t>(spectatorCs_ + kCsInstanceCountOff);
        const uintptr_t ci = LocalInstance();
        if (ci) {
            out->spectatorMode = Load<int32_t>(ci + kCiModeOff);
            out->freeCamActive = Load<int32_t>(ci + kCiFreeCamActiveOff);
            std::memcpy(out->ciQuat, reinterpret_cast<const void*>(ci + kCiQuatOff), 16);
            std::memcpy(out->ciPos, reinterpret_cast<const void*>(ci + kCiPosOff), 12);
            out->ciFovY = Load<float>(ci + kCiFovYOff);
        }

        // CCameraNode[0]'s pose does not live on the node. Both setters write a shared SoA pool
        // reached through node+0x70 -> +0x28, indexed by node+0x78 with a 48-byte stride, and they
        // store nothing into the node itself -- so reading the node would read a field nobody wrote.
        const uintptr_t nodeMgr = Load<uintptr_t>(Va(kNodeMgrGlobalVA));
        if (!nodeMgr) return false;
        const uintptr_t node = Load<uintptr_t>(nodeMgr + kNodeArrayOff);
        if (!node) return false;
        const uintptr_t owner = Load<uintptr_t>(node + kNodeXformOwnerOff);
        if (!owner) return false;
        const uintptr_t pool = Load<uintptr_t>(owner + kNodeXformPoolOff);
        if (!pool) return false;
        const uintptr_t elem = pool + Load<uint16_t>(node + kNodeXformIndexOff) * kNodeXformStride;
        std::memcpy(out->nodeQuat, reinterpret_cast<const void*>(elem), 16);
        std::memcpy(out->nodePos, reinterpret_cast<const void*>(elem + kNodeXformPosOff), 12);
        out->nodeFovH = Load<float>(node + kNodeFovHOff);

        out->valid = true;
        return true;
    }

private:
    uintptr_t Va(uint64_t va) const { return static_cast<uintptr_t>(va) + slide_; }

    // Re-read rather than cached: sub_1404F5180 allocates a fresh array, and CAM.VIEWSTATE_REALLOC
    // replaces every object in it, so a pointer captured at Init survives neither a level change
    // nor a free-cam entry.
    uintptr_t ViewStates() const {
        return game_ ? Load<uintptr_t>(game_ + kViewStateArrayOff) : 0;
    }

    // The instance CAM.SETCAMERA_API_IMPL will write: the one whose index equals the component
    // system's local index. Re-resolved per call because the instance array can be reallocated.
    uintptr_t LocalInstance() const {
        const uint16_t count = Load<uint16_t>(spectatorCs_ + kCsInstanceCountOff);
        const uint16_t local = Load<uint16_t>(spectatorCs_ + kCsLocalIndexOff);
        if (local >= count) return 0;
        const uintptr_t base = Load<uintptr_t>(spectatorCs_ + kCsInstanceBaseOff);
        if (!base) return 0;
        const uintptr_t ci = base + static_cast<uintptr_t>(local) * kCiStride;
        // The instance array is the engine's, and an index inside the count is not a promise the
        // array behind it is still mapped -- CAM.SETCAMERA_API_IMPL would write 32 bytes into it.
        if (!Readable(reinterpret_cast<const void*>(ci), kCiStride)) return 0;
        return ci;
    }

    void Refuse(Refusal r) {
        stats_.refusal   = r;
        stats_.available = false;
    }

    uintptr_t      slide_        = 0;
    uintptr_t      game_         = 0;
    uintptr_t      spectatorCs_  = 0;
    SetCameraApiFn setCameraApi_ = nullptr;
    Stats          stats_{};
};

Camera g_camera;

}  // namespace

namespace detail {

Resolution Refuse(Resolution& r, Refusal what, const char* why) {
    r.refusal = what;
    std::snprintf(r.why, sizeof r.why, "%s: %s", RefusalName(what), why);
    return r;
}

Resolution ResolveSpectatorCs(void* netGame, LookupComponentSystemFn lookup) {
    Resolution r;
    if (!netGame || !lookup) {
        return Refuse(r, Refusal::NoNetGame, "no CR15NetGame pointer, so no slot was read");
    }

    // ARCH-12 R1: the lookup's space is the LEVEL space at NETGAME.SPECTATORCAM_CS_OFF. The +0x2B08
    // CncaGameSpace is read for the differential and gates nothing (R4); the +0x8 slot is the
    // CR15Game back-pointer and is not read here at all.
    const uintptr_t ng         = reinterpret_cast<uintptr_t>(netGame);
    const uintptr_t levelSlot  = ng + facts::NETGAME_SPECTATORCAM_CS_OFF;
    const uintptr_t csSlot     = ng + facts::NETGAME_CS_SPACE_OFF;
    if (!Readable(reinterpret_cast<const void*>(levelSlot), sizeof(uint64_t))) {
        // Could-not-ask, not never-was: we have a pointer and its level-space slot is uncommitted.
        return Refuse(r, Refusal::NetGameUnreadable,
                      "a CR15NetGame was held and its level-space slot is not committed memory");
    }
    r.levelSpace = Load<uintptr_t>(levelSlot);
    if (Readable(reinterpret_cast<const void*>(csSlot), sizeof(uint64_t))) {
        r.csSpace = Load<uintptr_t>(csSlot);
    }

    if (!r.levelSpace) {
        return Refuse(r, Refusal::NoLevelSpace,
                      "the netGame carries no level space yet, which is what no map loaded looks "
                      "like -- there is no table to ask, so this is not a negative answer");
    }
    if (r.levelSpace == r.csSpace) {
        return Refuse(r, Refusal::SpaceAliased,
                      "the level space and the CncaGameSpace are one pointer; two adjacent slots "
                      "holding one object means the field being read is not the field intended");
    }

    // CS.LOOKUP_BY_SYMBOL reads the count at table+0x30 and then indexes table+0x00 with no further
    // check, so a space that is allocated but not yet constructed is an access violation inside the
    // engine, on our stack. Ask first.
    r.spaceTable = tableguard::CheckSpaceTable(reinterpret_cast<const void*>(r.levelSpace),
                                               camera_facts::kSpaceCsTableOff,
                                               camera_facts::kSpaceCsTableStride, false);
    if (r.spaceTable.verdict == tableguard::TableVerdict::Unreadable) {
        return Refuse(r, Refusal::LevelSpaceUnreadable,
                      "the level-space pointer is non-null and its component-system table could "
                      "not be read, so the table was never asked");
    }
    if (r.spaceTable.verdict != tableguard::TableVerdict::Ready) {
        return Refuse(r, Refusal::SpaceTableNotReady,
                      tableguard::WhyName(r.spaceTable.why));
    }

    r.csFromLookup = reinterpret_cast<uintptr_t>(
        lookup(reinterpret_cast<void*>(r.levelSpace), camera_facts::kSpectatorSymbol));
    if (!r.csFromLookup) {
        return Refuse(r, Refusal::LookupNull,
                      "CAM.SPECTATOR_SYMBOL is not registered in this level space");
    }

    // The differential (ARCH-12 R2). It is the one arm that can show the level space was the right
    // object rather than merely an object that answered: the same symbol asked of the csSpace must
    // come back null, as it did on the recorded L5 #3 run. A csSpace that is absent or not indexable
    // is a control we could not ask -- said so, and never confused with one that passed.
    const char* differential;
    if (!r.csSpace) {
        differential = "the differential could not be asked: the netGame carries no CncaGameSpace";
    } else {
        r.csSpaceTable = tableguard::CheckSpaceTable(reinterpret_cast<const void*>(r.csSpace),
                                                     camera_facts::kSpaceCsTableOff,
                                                     camera_facts::kSpaceCsTableStride, false);
        if (r.csSpaceTable.verdict != tableguard::TableVerdict::Ready) {
            differential = "the differential could not be asked: the CncaGameSpace's table is not "
                           "indexable";
        } else {
            r.csFromCsSpace = reinterpret_cast<uintptr_t>(
                lookup(reinterpret_cast<void*>(r.csSpace), camera_facts::kSpectatorSymbol));
            if (r.csFromCsSpace) {
                return Refuse(r, Refusal::SpaceAmbiguous,
                              "the CncaGameSpace answers CAM.SPECTATOR_SYMBOL too, so which object "
                              "the camera binds is no longer decided");
            }
            differential = "the differential passed: the CncaGameSpace was asked and answered null";
        }
    }

    r.refusal = Refusal::None;
    std::snprintf(r.why, sizeof r.why,
                  "resolved on the level space at +0x%X; %s",
                  static_cast<unsigned>(facts::NETGAME_SPECTATORCAM_CS_OFF), differential);
    return r;
}

// One wording for both site tables below, so "unreadable" and "mismatch" cannot drift apart
// between the write point's guard and the read's.
void DescribeRefusal(GuardResult& out, const char* name, uintptr_t at, SiteVerdict v) {
    if (v == SiteVerdict::Unreadable) {
        out.refusal = Refusal::PrologueUnreadable;
        std::snprintf(out.why, sizeof out.why,
                      "prologue-unreadable: %s at 0x%llX + %llu bytes is not mapped in this "
                      "image, so the build check could not be asked",
                      name, static_cast<unsigned long long>(at),
                      static_cast<unsigned long long>(kPrologueLen));
    } else {
        out.refusal = Refusal::PrologueMismatch;
        std::snprintf(out.why, sizeof out.why,
                      "prologue-mismatch: %s at 0x%llX + %llu bytes does not hold the bytes "
                      "facts.tsv recorded for this build",
                      name, static_cast<unsigned long long>(at),
                      static_cast<unsigned long long>(kPrologueLen));
    }
}

GuardResult CheckGuardedSites(uintptr_t slide) {
    using namespace camera_facts;

    // The order is the order they are needed in, and it is load-bearing for the test: the walk
    // stops at the first site that does not match, so a matching site is never allowed to stand in
    // for the ones after it.
    static const GuardedSite kSites[] = {
        {"CAM.VIEW_COMMIT", kViewCommitVA, kViewCommitPrologue},
        {"CAM.SETCAMERA_API_IMPL", kSetCameraApiImplVA, kSetCameraApiImplPrologue},
        {"CAM.SETCAMERA_API_STORES", kSetCameraApiImplVA + kSetCameraApiStoresOff, kSetCameraApiStores},
        {"CS.LOOKUP_BY_SYMBOL", facts::CS_LOOKUP_BY_SYMBOL, facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE},
        {"VIEWSTATE.INPUT_GATE_FREE_SITE", kIntegrateFreeVA + kIntegrateFreeGateOff, kIntegrateFreeGate},
        {"VIEWSTATE.INPUT_GATE_BASE_SITE", kIntegrateBaseVA + kIntegrateBaseGateOff, kIntegrateBaseGate},
    };

    GuardResult out;
    for (const GuardedSite& site : kSites) {
        const uintptr_t at = static_cast<uintptr_t>(site.va) + slide;
        const SiteVerdict v = CheckSite(at, site.want);
        if (v == SiteVerdict::Match) {
            continue;
        }
        DescribeRefusal(out, site.name, at, v);
        return out;
    }
    return out;
}

// The read's own table. Held here beside CheckGuardedSites because both walk the same CheckSite,
// but it is a DIFFERENT table: see the header for why Create()'s other four sites are not in it.
GuardResult CheckViewPoseSites(uintptr_t slide) {
    using namespace camera_facts;

    static const GuardedSite kSites[] = {
        {"CAM.VIEW_COMMIT", kViewCommitVA, kViewCommitPrologue},
        // 0x140D6A806 - 0x140D6A350.
        {"CAM.LISTENER_SOURCE_TEST", facts::CAM_SPECTATOR_UPDATE + 0x4B6ull,
         facts::CAM_LISTENER_SOURCE_TEST},
    };

    GuardResult out;
    for (const GuardedSite& site : kSites) {
        const uintptr_t at = static_cast<uintptr_t>(site.va) + slide;
        const SiteVerdict v = CheckSite(at, site.want);
        if (v == SiteVerdict::Match) {
            continue;
        }
        DescribeRefusal(out, site.name, at, v);
        return out;
    }
    return out;
}

bool IntegratorGateSiteMatches(uintptr_t slide) {
    using namespace camera_facts;
    return CheckSite(static_cast<uintptr_t>(kIntegrateFreeVA) + kIntegrateFreeGateOff + slide,
                     kIntegrateFreeGate) == SiteVerdict::Match;
}

ViewPoseRead ReadViewStatePose(uintptr_t game, bool integratorGateSiteMatched,
                               ViewStatePose* out) {
    *out = ViewStatePose{};
    // AUDIT_L1 finding 2. NoViewState is one word for three engine absences, enumerated apart in
    // the enum's own comment, so each one says which link was null. A consumer keying on why[0]
    // as "did the read refuse?" -- camera_control.cpp does -- got false on all three.
    if (!game) {
        std::snprintf(out->why, sizeof out->why,
                      "no CR15Game: the game-singleton slot holds null");
        return ViewPoseRead::NoViewState;
    }

    const uintptr_t arraySlot = game + kViewStateArrayOff;
    if (!Readable(reinterpret_cast<const void*>(arraySlot), sizeof(uintptr_t))) {
        std::snprintf(out->why, sizeof out->why,
                      "view-state array slot at 0x%llX is not committed memory",
                      static_cast<unsigned long long>(arraySlot));
        return ViewPoseRead::Unreadable;
    }
    const uintptr_t array = Load<uintptr_t>(arraySlot);
    if (!array) {
        std::snprintf(out->why, sizeof out->why,
                      "CR15Game at 0x%llX holds no view-state array",
                      static_cast<unsigned long long>(game));
        return ViewPoseRead::NoViewState;
    }
    if (!Readable(reinterpret_cast<const void*>(array), sizeof(uintptr_t))) {
        std::snprintf(out->why, sizeof out->why,
                      "view-state array at 0x%llX is not committed memory",
                      static_cast<unsigned long long>(array));
        return ViewPoseRead::Unreadable;
    }
    const uintptr_t vs = Load<uintptr_t>(array);
    if (!vs) {
        std::snprintf(out->why, sizeof out->why,
                      "the view-state array at 0x%llX holds no view state 0",
                      static_cast<unsigned long long>(array));
        return ViewPoseRead::NoViewState;
    }

    // One span for every field below: quat at +0x50, pos at +0x60, fovY at +0x88, the gate at
    // +0x9C. Asked before any of them is touched, because an array slot that holds a pointer is
    // not a promise the object behind it is still mapped.
    if (!Readable(reinterpret_cast<const void*>(vs), kViewStateInputEnaOff + sizeof(int32_t))) {
        std::snprintf(out->why, sizeof out->why,
                      "view-state 0 at 0x%llX + %llu bytes is not committed memory",
                      static_cast<unsigned long long>(vs),
                      static_cast<unsigned long long>(kViewStateInputEnaOff + sizeof(int32_t)));
        return ViewPoseRead::Unreadable;
    }

    std::memcpy(out->quat, reinterpret_cast<const void*>(vs + kViewStateQuatOff), 16);
    std::memcpy(out->pos, reinterpret_cast<const void*>(vs + kViewStatePosOff), 12);
    out->fovY = Load<float>(vs + kViewStateFovYOff);

    // A build whose VIEWSTATE.INPUT_GATE_FREE_SITE no longer matches is a build that may have
    // moved +0x9C. Reading it anyway and flagging it Known would hand the observer garbage that
    // certifies itself as read; leaving it 0 with Known clear says "not asked", which is what
    // happened.
    if (integratorGateSiteMatched) {
        out->integratorArmed      = Load<int32_t>(vs + kViewStateInputEnaOff);
        out->integratorArmedKnown = true;
    }
    return ViewPoseRead::Ok;
}

ViewPoseRead ReadViewStatePoseAt(uintptr_t module, ViewStatePose* out) {
    *out = ViewStatePose{};
    if (!module) {
        std::snprintf(out->why, sizeof out->why,
                      "no module handle: the read was asked before the image resolved");
        return ViewPoseRead::Unreadable;
    }
    const uintptr_t slide = module - static_cast<uintptr_t>(camera_facts::kImageBase);

    // The guard already writes the sentence that separates a DRIFTED site from a site whose page
    // is not mapped, and both reach the caller as the one word Unreadable. Throwing it away
    // (AUDIT_VIEWREAD_R2 VR2-2) left an operator on a drifted build reading the same word an
    // uncommitted page produces, with nothing anywhere naming the site or the check.
    const GuardResult guard = CheckViewPoseSites(slide);
    if (guard.refusal != Refusal::None) {
        std::snprintf(out->why, sizeof out->why, "%s", guard.why);
        return ViewPoseRead::Unreadable;
    }
    const bool gate = IntegratorGateSiteMatches(slide);

    const uintptr_t gameSlot = module + facts::GAME_SINGLETON_RVA;
    if (!Readable(reinterpret_cast<const void*>(gameSlot), sizeof(uintptr_t))) {
        std::snprintf(out->why, sizeof out->why,
                      "game-singleton slot at 0x%llX is not committed memory",
                      static_cast<unsigned long long>(gameSlot));
        return ViewPoseRead::Unreadable;
    }
    return ReadViewStatePose(Load<uintptr_t>(gameSlot), gate, out);
}

}  // namespace detail

const char* RefusalName(Refusal r) {
    switch (r) {
        case Refusal::None:               return "none";
        case Refusal::NoModule:           return "no-module";
        case Refusal::NoGame:             return "no-game";
        case Refusal::PrologueMismatch:   return "prologue-mismatch";
        case Refusal::PrologueUnreadable: return "prologue-unreadable";
        case Refusal::NoViewStates:       return "no-view-states";
        case Refusal::NoNetGame:          return "no-netgame";
        case Refusal::LookupNull:         return "lookup-null";
        case Refusal::NoLocalInstance:    return "no-local-instance";
        case Refusal::NoLevelSpace:       return "no-level-space";
        case Refusal::SpaceTableNotReady: return "space-table-not-ready";
        case Refusal::SpaceAliased:       return "space-aliased";
        case Refusal::PoseOwnerConflict:  return "pose-owner-conflict";
        case Refusal::NetGameUnreadable:  return "netgame-unreadable";
        case Refusal::LevelSpaceUnreadable:   return "level-space-unreadable";
        case Refusal::SpaceAmbiguous:         return "space-ambiguous";
        case Refusal::SpectatorCsUnreadable:  return "spectator-cs-unreadable";
    }
    return "unknown";
}

void GetStats(Stats* out) { *out = g_camera.GetStats(); }

bool Observe(Observation* out) { return g_camera.Observe(out); }

const char* ViewPoseReadName(ViewPoseRead r) {
    switch (r) {
        case ViewPoseRead::Ok:          return "ok";
        case ViewPoseRead::NotBound:    return "not-bound";
        case ViewPoseRead::NoViewState: return "no-view-state";
        case ViewPoseRead::Unreadable:  return "unreadable";
    }
    return "unknown";
}

ViewPoseRead ReadViewStatePose(ViewStatePose* out) {
    if (!out) return ViewPoseRead::Unreadable;
    *out = ViewStatePose{};

    // The whole body is detail::ReadViewStatePoseAt, so the site table, the singleton probe and
    // the walk are all reachable from a test that fabricates an image. Nothing lives here but the
    // one call the OS has to answer.
    return detail::ReadViewStatePoseAt(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), out);
}

namespace detail {

using namespace camera_facts;

bool GateLedger::Record(uintptr_t viewState, int32_t prior) {
    for (uint32_t i = 0; i < count; ++i) {
        // Already held: keep the FIRST pre-state. A later SetPose finding this gate armed again is
        // the engine having re-armed it between our writes, which is not a new pre-state.
        if (entries[i].viewState == viewState) return true;
    }
    if (count >= kGateLedgerMax) return false;
    entries[count].viewState = viewState;
    entries[count].prior     = prior;
    ++count;
    return true;
}

GateLedger& ViewGateLedger() {
    static GateLedger ledger;
    return ledger;
}

void WriteViewStates(uintptr_t viewStates, uint32_t views, const float pos[3], const float quat[4],
                     float fovYRadians, Stats& stats) {
    // And it is HERE as well as at SetPose because this is a public entry
    // of its own with its own callers and its own tests. Below this line the engine's per-view
    // input integrator is disarmed and the live session's view states take our pose.
    if (!livesafety::Allowed(livesafety::Feature::Camera)) return;
    static_assert(kGateLedgerMax >= kMaxViews, "the ledger must hold a whole generation of views");
    if (views > kMaxViews) views = kMaxViews;
    stats.viewCount = views;

    GateLedger& ledger = ViewGateLedger();
    for (uint32_t i = 0; i < views; ++i) {
        const uintptr_t vs = Load<uintptr_t>(viewStates + i * sizeof(void*));
        if (!vs) continue;

        // Disarm the engine's own free-fly integrator BEFORE writing, and count the times it was
        // found armed. CAM.ENTER_FREECAM replaces every view-state object, and the base constructor
        // sets this back to 1, so a non-zero reassert count is the engine having taken the camera
        // back -- not noise. What it held is recorded so Relinquish can give it back.
        const int32_t gate = Load<int32_t>(vs + kViewStateInputEnaOff);
        if (gate != 0) {
            Store<int32_t>(vs + kViewStateInputEnaOff, 0);
            ++stats.gateReasserts;
            if (!ledger.Record(vs, gate)) ++stats.gateUnrecorded;
        }

        std::memcpy(reinterpret_cast<void*>(vs + kViewStateQuatOff), quat, 16);
        std::memcpy(reinterpret_cast<void*>(vs + kViewStatePosOff), pos, 12);
        Store<float>(vs + kViewStateFovYOff, fovYRadians);
        ++stats.viewsWritten;
    }
}

void RestoreViewGates(Stats& stats) {
    GateLedger& ledger = ViewGateLedger();
    for (uint32_t i = 0; i < ledger.count; ++i) {
        const GateLedger::Entry& e = ledger.entries[i];
        const uintptr_t at = e.viewState + kViewStateInputEnaOff;

        // The view state may be gone: CAM.ENTER_FREECAM replaces every one of these objects. So the
        // address is probed, and then the gate must still read as the zero we left it at. If it
        // does not, either the engine re-armed it -- which is the outcome we wanted anyway -- or
        // that memory is no longer a view state, and writing four bytes into it would corrupt
        // whatever took its place.
        if (!Readable(reinterpret_cast<const void*>(at), sizeof(int32_t))) {
            ++stats.gateUnreadable;
            continue;
        }
        if (Load<int32_t>(at) != 0) {
            ++stats.gateNotOurs;
            continue;
        }
        Store<int32_t>(at, e.prior);
        ++stats.gateRestored;
    }
    ledger.count = 0;
}

}  // namespace detail

IReplayCamera* Create(void* netGame) {
    // The claim comes first, before a single engine read: this module clears the integrator gate
    // the stock free camera runs on, so if that camera holds the pose there is nothing here to do
    // and no reason to have looked.
    if (!ClaimPoseOwner(PoseOwner::WritePoint)) {
        g_camera.RefusePoseOwner();
        return nullptr;
    }
    g_camera.Init(netGame);
    if (!g_camera.GetStats().available) {
        // A write point that did not check out writes no pose, so it must not sit on the token and
        // lock the stock free camera out of a slot it could have had.
        ReleasePoseOwner(PoseOwner::WritePoint);
        return nullptr;
    }
    return &g_camera;
}

void ReleaseKeepingToken() { g_camera.Relinquish(); }

void Release() {
    ReleaseKeepingToken();
    ReleasePoseOwner(PoseOwner::WritePoint);
}

}  // namespace camera
}  // namespace replay
