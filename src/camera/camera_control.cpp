// W2-E. IReplayCameraControl over the shipping spectator rig.
//
// The whole control is CAM.SET_CAMERA_MODE_IMPL plus three guarded reads. It resolves the rig
// itself rather than borrowing camera_writepoint.cpp's: that file is W1-H's, its state is
// private, and the two serve different mechanisms -- the rig (primary) and the view-state write
// point (the retained fallback, S8.1.1).
//
// ★ How the rig is found (ARCH-12 sections 2 and 3, which corrected what was here before).
// netGame carries TWO spaces, not one, and the earlier text got which is which exactly backwards:
//
//   +0x2B08  NETGAME.CS_SPACE_OFF          a CncaGameSpace. On L5 run 3 it held 99 systems and the
//                                          spectator symbol was NOT among them
//   +0x2B10  NETGAME.SPECTATORCAM_CS_OFF   the LEVEL space, a CncaGameSpaceMT built at map load.
//                                          The id is a MISNOMER: it is not the CS. This is where
//                                          the spectator camera CS is registered
//   +0x8     IDENT.NETGAME_GAMESPACE_OFF   the CR15Game back-pointer, not a space at all; its
//                                          +0x420 is the render-target budget block, which is why
//                                          the table check refused it forever
//
// The accessor is the engine's own and there is no alternative to rule on: THREE independent chains
// in the shipping image -- GUI.CAMERA_EDITOR at 0x140195FC8, CAM.SET_CAMERA_API at 0x140165641, and
// sub_140163D10 -> sub_1401B2EA0 (@0x1401B3530, closed by AUD-ARCH F-E) -- pass that pointer
// UNCHANGED as argument 1 of CS.LOOKUP_BY_SYMBOL with CAM.SPECTATOR_SYMBOL. So the lookup runs on
// the level space, after W1-C's table check has passed on THAT space.
//
// STRUCK, and it was false: "Nothing anywhere reads a spectator CS pointer directly" stood here
// until AUD-ARCH F-E found sub_140DA28E0 taking the level space through sub_140181980 and walking
// +0x420 / +0x450 itself, never calling the lookup. So the lookup by symbol is the accessor OF OUR
// PRODUCT BY CONSTRUCTION, not of the engine by exhaustion. The census that settles it is committed
// as kSpectatorSymbolRefSites in camera_control.h.
//
// The old cross-check ("the lookup's answer must equal the +0x2B10 slot") is deleted: it compared
// a component system against a space and could only ever fail. Its replacement is the two-space
// differential -- the level space must answer, the CS space must not, and the two must not be the
// same pointer -- which encodes L5 run 3's recorded observation as a live invariant instead.

#include "camera/camera_control.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/tableguard.h"
#include "engine/camera_writepoint_facts.h"
#include "generated/facts_generated.h"

namespace replay {
namespace {

using namespace camera_facts;

// The three stores whose displacements ARE SPECTATOR.CI_SELECTOR_PACKED_OFF,
// SPECTATOR.CI_CAMERA_INDEX_OFF and SPECTATOR.CI_CAMERA_SET_OFF, from the W2-E fact drop.
// Guarding them is what stops a silent layout change from looking like a camera that selects the
// wrong player.
const uint8_t kSelectorStore[3]  = {0x89, 0x43, 0x26};
const uint8_t kCamIndexStore[4]  = {0x66, 0x89, 0x43, 0x32};
const uint8_t kCamSetRead[7]     = {0x0F, 0xB7, 0x8B, 0x7C, 0x02, 0x00, 0x00};

const uint32_t kSetCameraModeSelectorOff = 0x99u;  // 0x140D45009 - 0x140D44F70
const uint32_t kSetCameraModeCamIndexOff = 0xE8u;  // 0x140D45058 - 0x140D44F70
const uint32_t kSetCameraModeCamSetOff   = 0xB1u;  // 0x140D45021 - 0x140D44F70

const uint32_t kCiSelectorPackedOff = 0x26u;
const uint32_t kCiCameraIndexOff    = 0x32u;
const uint32_t kCiCameraSetOff      = 0x27Cu;

// The component-system table CS.LOOKUP_BY_SYMBOL indexes, 16 bytes per element. The offset is
// facts::CS_TABLE_OFF and nothing else: a private copy of a V-PRIMARY value is a second source
// that cannot be corrected when the first one is (ARCH-10 section 3.3).
const uint32_t kCsTableOff     = facts::CS_TABLE_OFF;
const uint64_t kCsEntryStride  = 16u;

// The rig's header, up to and including the last field LocalInstance reads.
const size_t kCsHeaderBytes = facts::SPECTATOR_CS_LOCAL_INDEX_OFF + sizeof(uint16_t);

template <typename T>
T Load(uintptr_t p) {
    T v{};
    memcpy(&v, reinterpret_cast<const void*>(p), sizeof(T));
    return v;
}

camera_control::RigEngine g_realEngine{};
const camera_control::RigEngine* g_engineForTest = nullptr;

char g_diagnostic[256] = "not created";

// AUDIT_L1 finding 3. Diagnostic() has exactly one non-test caller and it sits in the bind-REFUSAL
// branch, which cannot run in a session where GetViewPose is being asked -- so W1-H's sentence
// reached a static nothing shipped ever printed. This latch is what camera_module publishes on the
// [camera] status line the observer already reads. The FIRST refusal is kept, because a frame loop
// would otherwise leave whichever refusal happened last, and every refusal is counted.
char g_viewReadRefusal[192] = {};
unsigned long long g_viewReadRefusals = 0;

void ClearViewReadRefusals() {
    g_viewReadRefusal[0] = '\0';
    g_viewReadRefusals = 0;
}

void RecordViewReadRefusal(const char* why) {
    ++g_viewReadRefusals;
    if (g_viewReadRefusal[0] == '\0' && why && why[0] != '\0') {
        _snprintf_s(g_viewReadRefusal, sizeof g_viewReadRefusal, _TRUNCATE, "%s", why);
    }
}

camera_control::ViewStateReader g_viewStateReader = nullptr;
const camera_control::WritePointSeam* g_writePoint = nullptr;
const camera_control::SubjectPoseSeam* g_subjectPose = nullptr;
// ARCH-19 R3 / AUDIT_ARCH_19_20 C6. Index 0 is unused: selector 0 is a parse refusal.
camera_control::SelectorDerivation
    g_selectorDerivation[IReplayCameraControl::kPlayerSelectorMax + 1] = {};
// ARCH-19 R1 / AUDIT_ARCH_19_20 C4. Hand-backs that had to take the pre-R1 route.
uint64_t g_handbackDegraded = 0;

void SetDiagnostic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(g_diagnostic, sizeof g_diagnostic, _TRUNCATE, fmt, args);
    va_end(args);
}

// Every site the real engine path calls, guarded by bytes and rebased in one call. Four packets
// have been bitten by the raw-address form and the failure is silent, so nothing here
// dereferences a literal.
const camera_control::RigEngine* ResolveEngine() {
    if (g_engineForTest) return g_engineForTest;

    struct Site {
        const char*    fact;
        uint64_t       va;
        const uint8_t* bytes;
        size_t         len;
    };
    const Site sites[] = {
        {"CAM.SET_CAMERA_MODE_IMPL", facts::CAM_SET_CAMERA_MODE_IMPL,
         facts::CAM_SET_CAMERA_MODE_IMPL_PROLOGUE, sizeof facts::CAM_SET_CAMERA_MODE_IMPL_PROLOGUE},
        {"SPECTATOR.CI_SELECTOR_PACKED_OFF", facts::CAM_SET_CAMERA_MODE_IMPL + kSetCameraModeSelectorOff,
         kSelectorStore, sizeof kSelectorStore},
        {"SPECTATOR.CI_CAMERA_INDEX_OFF", facts::CAM_SET_CAMERA_MODE_IMPL + kSetCameraModeCamIndexOff,
         kCamIndexStore, sizeof kCamIndexStore},
        {"SPECTATOR.CI_CAMERA_SET_OFF", facts::CAM_SET_CAMERA_MODE_IMPL + kSetCameraModeCamSetOff,
         kCamSetRead, sizeof kCamSetRead},
        {"CS.LOOKUP_BY_SYMBOL", facts::CS_LOOKUP_BY_SYMBOL, facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
         sizeof facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE},
    };
    for (const Site& s : sites) {
        if (!buildguard::VerifySite(s.fact, s.va, s.bytes, s.len)) {
            SetDiagnostic("site guard refused %s", s.fact);
            return nullptr;
        }
    }

    g_realEngine.lookupSystem = reinterpret_cast<void* (*)(void*, uint64_t)>(
        const_cast<void*>(buildguard::MappedAddress(facts::CS_LOOKUP_BY_SYMBOL)));
    g_realEngine.setMode = reinterpret_cast<void (*)(void*, int, unsigned short)>(
        const_cast<void*>(buildguard::MappedAddress(facts::CAM_SET_CAMERA_MODE_IMPL)));
    if (!g_realEngine.lookupSystem || !g_realEngine.setMode) {
        SetDiagnostic("engine sites are not mapped in this process");
        return nullptr;
    }
    return &g_realEngine;
}

// ARCH-12 R2. The level space cannot cross-check itself, so what remains is a DIFFERENTIAL against
// the other space: the spectator symbol must resolve in the level space and NOT in the CS space.
// The arm is four-state on purpose -- absent and unjudgeable are "could not ask", which is not
// "passed" -- and the refusing arms set the diagnostic that names them.
enum class DiffArm { Absent, NotAsked, Clear, Aliased, Ambiguous };

const char* DiffArmWord(DiffArm arm) {
    switch (arm) {
        case DiffArm::Absent:    return "CsSpaceAbsent";
        case DiffArm::NotAsked:  return "CsSpaceNotAsked";
        case DiffArm::Clear:     return "CsSpaceClear";
        case DiffArm::Aliased:   return "SpaceAliased";
        case DiffArm::Ambiguous: return "SpaceAmbiguous";
    }
    return "?";
}

bool ArmRefuses(DiffArm arm) { return arm == DiffArm::Aliased || arm == DiffArm::Ambiguous; }

// The two could-not-ask arms, which a later tick can still turn into an answer: absent because the
// slot has not been written yet, not-asked because the CS space's table is not yet indexable.
bool ArmUnresolved(DiffArm arm) { return arm == DiffArm::Absent || arm == DiffArm::NotAsked; }

void SayAliased(const camera_control::RigIdentity& id) {
    SetDiagnostic("SpaceAliased: NETGAME.CS_SPACE_OFF and NETGAME.SPECTATORCAM_CS_OFF both "
                  "hold %p, so the differential cannot tell the two spaces apart",
                  id.levelSpace);
}

DiffArm Differential(const camera_control::RigIdentity& id,
                     const camera_control::RigEngine* engine) {
    if (!id.csSpace) return DiffArm::Absent;
    if (id.csSpace == id.levelSpace) {
        SayAliased(id);
        return DiffArm::Aliased;
    }
    // Asked through W1-C's check first: the engine's lookup sorts an unsorted block IN PLACE, and
    // a control arm that corrupts the thing it is measuring is worse than an unasked one.
    const tableguard::TableCheck other =
        tableguard::CheckSpaceTable(id.csSpace, kCsTableOff, kCsEntryStride, false);
    if (other.verdict != tableguard::TableVerdict::Ready) return DiffArm::NotAsked;
    if (engine->lookupSystem(id.csSpace, facts::CAM_SPECTATOR_SYMBOL) != nullptr) {
        SetDiagnostic("SpaceAmbiguous: the spectator symbol resolves in CS space %p as well as in "
                      "level space %p", id.csSpace, id.levelSpace);
        return DiffArm::Ambiguous;
    }
    return DiffArm::Clear;
}

class Control final : public IReplayCameraControl {
public:
    bool Init(void* netGame) {
        // A fresh binding is a fresh control: a request or an oracle bracket made against the
        // previous rig did not reach this one.
        Drop();
        requested_ = CameraMode::Pov;

        camera_control::RigIdentity id;
        const camera_control::RigProbe probe = camera_control::Probe(netGame, &id);
        if (probe != camera_control::RigProbe::Ready) {
            SetDiagnostic("%s", camera_control::RigProbeName(probe));
            return false;
        }

        const camera_control::RigEngine* engine = ResolveEngine();
        if (!engine) return false;

        // ★ The alias arm is asked BEFORE the level-space lookup, which is the order P-B's
        // ResolveSpectatorCs already uses (camera_writepoint.cpp): one contract, one order in both
        // consumers. Aliased slots mean the field being read is not the field intended, and such a
        // space will typically answer null -- filed as LookupNull that reads as "the symbol is not
        // in this space" and points at the space instead of at the slot (AUD-F3H F3H-3). Zero
        // lookup calls on this arm, here as there.
        if (id.csSpace == id.levelSpace) {
            SayAliased(id);
            return false;
        }

        void* found = engine->lookupSystem(id.levelSpace, facts::CAM_SPECTATOR_SYMBOL);
        if (!found) {
            SetDiagnostic("LookupNull: the spectator CS is not in level space %p's system table",
                          id.levelSpace);
            return false;
        }
        const DiffArm arm = Differential(id, engine);
        if (ArmRefuses(arm)) return false;

        spectatorCs_ = reinterpret_cast<uintptr_t>(found);
        if (!LocalInstance()) {
            SetDiagnostic("no local instance on rig %p (%s)", found, InstanceText());
            spectatorCs_ = 0;
            return false;
        }

        engine_        = engine;
        available_     = true;
        boundId_       = id;
        arm_           = arm;
        judgedCsSpace_ = id.csSpace;
        ClearViewReadRefusals();  // a new binding session starts with no refusals of its own
        refusal_   = ControlRefusal::None;
        SetDiagnostic("ready (differential %s)", DiffArmWord(arm));
        return true;
    }

    // AUD-F3H F3H-2. Called on the module's bind-epoch cadence, never per frame.
    bool Revise(const camera_control::RigIdentity& id) {
        if (!available_) return true;
        // A different binding is the module's business, not this call's: it will re-Bind.
        if (id.netGame != boundId_.netGame || id.levelSpace != boundId_.levelSpace) return true;

        // The arm is re-asked when the slot has moved, and when a could-not-ask arm now has a
        // non-null slot to ask about (CsSpaceNotAsked -> the same space's table finished
        // constructing). An arm that already answered cannot change while the slot stands still,
        // so the steady state costs one pointer compare per epoch and no engine call.
        const bool slotMoved = id.csSpace != judgedCsSpace_;
        if (!slotMoved && !(ArmUnresolved(arm_) && id.csSpace != nullptr)) return true;

        camera_control::RigIdentity now = boundId_;
        now.csSpace = id.csSpace;
        const DiffArm arm = Differential(now, engine_);
        judgedCsSpace_   = id.csSpace;
        boundId_.csSpace = id.csSpace;
        if (ArmRefuses(arm)) {
            // Differential() has said which arm and why. The binding stops driving anything now,
            // before the module's Release, so nothing can steer a camera whose object is undecided.
            arm_       = arm;
            available_ = false;
            return false;
        }
        if (arm != arm_) {
            arm_ = arm;
            SetDiagnostic("ready (differential %s)", DiffArmWord(arm));
        }
        return true;
    }

    const char* DifferentialWord() const { return DiffArmWord(arm_); }

    bool SetMode(CameraMode mode, int32_t num) override {
        if (!available_) return Refuse(ControlRefusal::NotAvailable);
        if (mode == CameraMode::Api) return Refuse(ControlRefusal::ModeReservedForOracle);
        if (oracleTrack_) return Refuse(ControlRefusal::OracleTrackActive);
        if (num < 0 || num > kPlayerSelectorMax) return Refuse(ControlRefusal::NumOutOfRange);

        // ARCH-23 S14.11. Refused BEFORE the hand-back below, because a refusal that has already
        // released the pose has changed the thing it declined to change.
#ifndef W2E32_MUTATE_MODE_IGNORES_FREECAM
        int32_t freeCam = 0;
        if (ObservedFreeCam(&freeCam) && freeCam != 0) {
            return Refuse(ControlRefusal::FreeCamHoldsNode);
        }
#endif

        // A HUMAN MODE SELECTION HANDS THE POSE BACK FIRST (ARCH-17 S5). Selecting while we still
        // hold the write point's token leaves viewstate+0x9C cleared under the mode the human just
        // chose, and the camera they selected would not answer input -- the owner's headline verb,
        // broken by the feature added to serve it. A hand-back that cannot complete refuses the
        // selection rather than performing it half-owned.
        if (regime_ == PoseRegime::Commanded && !ReleasePose()) return false;

        // Re-resolved per call: the instance array reallocates, and a stale CS is how a
        // selection lands on nothing and reads as an unresponsive button.
        if (!LocalInstance()) return Refuse(ControlRefusal::NotAvailable);

        engine_->setMode(reinterpret_cast<void*>(spectatorCs_), static_cast<int>(mode),
                         static_cast<unsigned short>(num));
        requested_ = mode;
        refusal_   = ControlRefusal::None;
        CertifySelector(num);
        return true;
    }

    CameraMode RequestedMode() const override { return requested_; }

    bool ObservedMode(CameraMode* out) const override {
        const uintptr_t ci = LocalInstance();
        if (!ci || !out) return false;
        *out = static_cast<CameraMode>(Load<int32_t>(ci + facts::SPECTATOR_CI_MODE_OFF));
        return true;
    }

    bool ObservedSelector(int32_t* packedOut, uint16_t* cameraIndexOut) const override {
        const uintptr_t ci = LocalInstance();
        if (!ci) return false;
        if (packedOut) *packedOut = Load<int32_t>(ci + kCiSelectorPackedOff);
        if (cameraIndexOut) *cameraIndexOut = Load<uint16_t>(ci + kCiCameraIndexOff);
        return true;
    }

    bool ActiveLevelCameraSet(uint16_t* out) const override {
        const uintptr_t ci = LocalInstance();
        if (!ci || !out) return false;
        *out = Load<uint16_t>(ci + kCiCameraSetOff);
        return true;
    }

    bool ObservedFreeCam(int32_t* out) const override {
        const uintptr_t ci = LocalInstance();
        if (!ci || !out) return false;
        *out = Load<int32_t>(ci + kCiFreeCamActiveOff);
        return true;
    }

    camera::ViewPoseRead GetViewPose(ViewPose* out) const override {
        if (!out) return camera::ViewPoseRead::Unreadable;
        *out = ViewPose{};
        if (!available_ || !spectatorCs_) return camera::ViewPoseRead::NotBound;

        // Bound, but the rig's instance array is not committed memory this frame. The question
        // could not be asked, which is not the same answer as "nothing is bound" -- and this is the
        // same LocalInstance() SetMode drives, Readable-checked over the whole CI, not a second
        // walk of the rig.
        const uintptr_t ci = LocalInstance();
        if (!ci) {
            // The read below never runs on this arm, so this refusal carries its own sentence or
            // the operator gets the bare word Unreadable for a cause W1-H never saw.
            RecordViewReadRefusal("the bound rig's camera instance is not committed memory");
            return camera::ViewPoseRead::Unreadable;
        }

        const int32_t mode    = Load<int32_t>(ci + facts::SPECTATOR_CI_MODE_OFF);
        const int32_t freeCam = Load<int32_t>(ci + kCiFreeCamActiveOff);

        camera::ViewStatePose vs;
        const camera::ViewPoseRead vsRead = g_viewStateReader
                                                ? g_viewStateReader(&vs)
                                                : camera::ViewPoseRead::Unreadable;

        // AUDIT_VIEWREAD_R2 VR2-2. The read's word has several causes and the read already wrote
        // the sentence naming the site and the check that refused; this is the only channel an
        // operator has for it. Without it a drifted build and an uncommitted page are the same
        // string on screen.
        // No `&& vs.why[0]` guard: the read fills the sentence on every refusal it issues
        // (AUDIT_L1 finding 2), so a guard on the field would only hide the day it stops.
        if (vsRead != camera::ViewPoseRead::Ok) {
            SetDiagnostic("view-state read refused: %s", vs.why);
            RecordViewReadRefusal(vs.why);
        }

#ifdef W2E32_MUTATE_POSE_FROM_CI_ALWAYS
        if (false) {
#else
        if (freeCam) {
#endif
            // CAM.SPECTATOR_UPDATE sources CCameraNode[0] from view-state 0 while ci+0x274 is
            // set, so on this arm the view state is the rendered pose and the CI is a DIFFERENT
            // object -- one the engine also writes, at seven DIRECT STORES inside
            // CAM.SPECTATOR_UPDATE itself, plus three instructions there that hand those field
            // addresses to a callee and are recorded unresolved (docs/spikes/W1-H.md). That is why this arm never reads the CI at all and a
            // refusal is published as a refusal: a fallback would answer a question about the
            // view state with a value from somewhere else, and ruling 4b would score it as a
            // pose. AUDIT_VIEWREAD_R2 VR2-1: the older "SetPose is the only writer of the CI
            // pose" rationale here was false, and the behaviour never rested on it.
            if (vsRead != camera::ViewPoseRead::Ok) return vsRead;
            memcpy(out->pos, vs.pos, sizeof out->pos);
            memcpy(out->quat, vs.quat, sizeof out->quat);
            out->fovYRadians = vs.fovY;
            out->source      = ViewPoseSource::ViewState;
        } else {
            memcpy(out->pos, reinterpret_cast<const void*>(ci + kCiPosOff), sizeof out->pos);
            memcpy(out->quat, reinterpret_cast<const void*>(ci + kCiQuatOff), sizeof out->quat);
            out->fovYRadians = Load<float>(ci + facts::SPECTATOR_CI_FOVY_OFF);
            out->source      = ViewPoseSource::CameraInstance;
        }
        out->mode = mode;
        // Known comes from the READ and not from "the read returned Ok": +0x9C's only evidence
        // site can have drifted on a build whose pose sites still match, and a gate published as
        // read while holding whatever now lives at that displacement is worse than no gate at all.
        if (vsRead == camera::ViewPoseRead::Ok) {
            out->integratorArmedKnown = vs.integratorArmedKnown;
            out->integratorArmed      = vs.integratorArmed;
        }
        return camera::ViewPoseRead::Ok;
    }

    // THE COMMANDED POSE (ARCH-17 S3.2). The header carries the contract; what follows is the
    // order the failure modes have to be asked in.
    bool CommandPose(const CommandedPose& p) override {
        if (!available_) return Refuse(ControlRefusal::NotAvailable);
        if (oracleTrack_) return Refuse(ControlRefusal::OracleTrackHolds);

        // ARCH-19 R3. `cam look at <n>` aims at the subject a selector names. Every read it takes
        // is somebody else's: the engine's own derivation of n (PackSelector, certified against
        // ci+0x26), and the installed seam's roster and body reads. Resolved BEFORE the write
        // point is touched, so a selector that names nobody costs the operator nothing.
        float lookTarget[3] = {0.0f, 0.0f, 0.0f};
        bool  haveLookTarget = false;
        if (p.lookAt) {
            const camera_control::SubjectResolve sr =
                ResolveSubjectPoint(p.lookAtSelector, lookTarget);
            if (sr != camera_control::SubjectResolve::Resolved) {
                SetDiagnostic("cam look at %d refused: %s", p.lookAtSelector,
                              camera_control::SubjectResolveName(sr));
                return Refuse(ControlRefusal::LookAtUnresolvable);
            }
            haveLookTarget = true;
        }

        const camera_control::WritePointSeam* wp = g_writePoint;
        if (!wp || !wp->create || !wp->release) {
            SetDiagnostic("no write point is installed in this image, so no pose can be placed");
            return Refuse(ControlRefusal::WritePointRefused);
        }

        // The pose is read BEFORE the token moves: a command that is going to refuse must not take
        // the camera off the human on its way to refusing. The read is also where fovY comes from
        // -- SetPose writes all three, CommandedPose carries no field of view, and a substituted
        // one would silently change the shot -- so an unreadable pose refuses in the absolute form
        // too, which is where this departs from ARCH-17 S4's row (reported, not assumed).
        ViewPose current;
        if (GetViewPose(&current) != camera::ViewPoseRead::Ok) {
            SetDiagnostic("commanded pose refused: the engine's pose could not be read (%s)",
                          g_viewReadRefusal[0] ? g_viewReadRefusal : "no sentence");
            return Refuse(ControlRefusal::PoseUnreadable);
        }

        // ARCH-19 R3(c): the aim becomes an ORDINARY absolute orientation and follows the one
        // path R6 gives that row. `setPosition` is left as the grammar left it -- `cam look at`
        // carries no position -- and the facing is computed through FacingToward, which is
        // BuildFacing's own inverse and not a second convention.
        CommandedPose composed = p;
        if (haveLookTarget) {
            float yawDeg = 0.0f, pitchDeg = 0.0f;
            if (!camera_control::FacingToward(current.pos, lookTarget, &yawDeg, &pitchDeg)) {
                SetDiagnostic("cam look at %d refused: the camera is standing inside the subject, "
                              "so there is no direction to face", p.lookAtSelector);
                return Refuse(ControlRefusal::FacingDegenerate);
            }
            composed.setOrientation = true;
            composed.absolute       = true;
            composed.yawDeg         = yawDeg;
            composed.pitchDeg       = pitchDeg;
        }

        float pos[3];
        float quat[4];
        if (!ComposePose(composed, current, pos, quat)) return false;  // ComposePose named the reason

        // Regime FIRST: a transfer whose `from` is not the holder returns false, and in regime
        // Commanded the token is already ours. Entering costs one transfer and one camera::Create;
        // staying costs neither, which is what keeps the engine's table-sorting lookup off the
        // per-command path (ARCH-17 S6).
        const bool entering = regime_ != PoseRegime::Commanded;
        if (entering) {
            if (!camera::TransferPoseOwner(camera::PoseOwner::StockFreeCam,
                                           camera::PoseOwner::WritePoint)) {
                SetDiagnostic("%s", camera::LastPoseOwnerConflict());
                return Refuse(ControlRefusal::PoseNotOwned);
            }
            writePoint_ = wp->create(boundId_.netGame);
            if (!writePoint_) {
                // Rolled back in the same call: a write point that writes no pose must not sit on
                // the token and lock the stock free camera out of a slot it could have had.
                camera::TransferPoseOwner(camera::PoseOwner::WritePoint,
                                          camera::PoseOwner::StockFreeCam);
                SetDiagnostic("write point refused: %s", wp->why ? wp->why() : "no reason given");
                return Refuse(ControlRefusal::WritePointRefused);
            }
            regime_ = PoseRegime::Commanded;
            // Entering means the token moved to this write point, so whatever hand-back the
            // arbiter refused earlier is over: the outstanding one is this regime's, not that one.
            handbackPending_ = Handback::None;
        }

        // One call, both halves: position and orientation land together or neither does.
        writePoint_->SetPose(pos, quat, current.fovYRadians);

        memcpy(commandedPos_, pos, sizeof commandedPos_);
        memcpy(commandedQuat_, quat, sizeof commandedQuat_);
        haveCommanded_ = true;
        refusal_       = ControlRefusal::None;
        // The regime word and the pose that was COMMANDED, on the channel an operator already
        // reads. What LANDED is read back through GetViewPose by the caller (ARCH-17 S3.4); the
        // two are deliberately different sentences.
        // A command that sets no orientation has no facing to report: R6's
        // !setOrientation row copies the engine's quaternion through and deliberately does NOT
        // touch commandedYawDeg_ / commandedPitchDeg_ (C8), so printing them after a pure `cam
        // move` reported the LAST commanded facing -- or 0.0 / 0.0 on a camera that has never been
        // aimed -- as though this command had just aimed it. `facing unchanged` is R6's own word
        // for that row, so the operator channel and the pose composer say the same thing.
        if (composed.setOrientation) {
            SetDiagnostic("regime %s: commanded (%.3f, %.3f, %.3f) yaw %.1f pitch %.1f",
                          PoseRegimeName(regime_), static_cast<double>(pos[0]),
                          static_cast<double>(pos[1]), static_cast<double>(pos[2]),
                          static_cast<double>(commandedYawDeg_),
                          static_cast<double>(commandedPitchDeg_));
        } else {
            SetDiagnostic("regime %s: commanded (%.3f, %.3f, %.3f) facing unchanged",
                          PoseRegimeName(regime_), static_cast<double>(pos[0]),
                          static_cast<double>(pos[1]), static_cast<double>(pos[2]));
        }
        return true;
    }

    PoseRegime Regime() const override {
        if (!available_) return PoseRegime::Unavailable;
        // The arbiter's own answer outranks ours: a section that could not be established is not
        // a session in regime Free, and reporting Free there would tell a probe the human has a
        // camera nobody can prove is theirs.
        if (camera::PoseOwnerHeld() == camera::PoseOwner::Unavailable) {
            return PoseRegime::Unavailable;
        }
        return regime_;
    }

    // WHICH STEP OF THE HAND-BACK THE ARBITER REFUSED, so the next `cam release` can
    // retry exactly that step. A refused hand-back leaves the token where it was and the regime
    // Unavailable, and the early return below answered `true` to every later attempt without
    // re-trying anything: the operator cleared the cause and the camera never came back.
    enum class Handback { None, Transfer, Claim };

    // The seam call is NOT repeated on a retry. releaseKeepingToken() (or release()) already ran
    // on the refused attempt -- the engine has its gates back and only the token move is
    // outstanding -- and re-running it would re-arm a write point this control no longer holds.
    bool RetryHandback() {
        const bool ok = handbackPending_ == Handback::Transfer
                            ? camera::TransferPoseOwner(camera::PoseOwner::WritePoint,
                                                        camera::PoseOwner::StockFreeCam)
                            : camera::ClaimPoseOwner(camera::PoseOwner::StockFreeCam);
        if (!ok) {
            regime_ = PoseRegime::Unavailable;
            SetDiagnostic("%s", camera::LastPoseOwnerConflict());
            return Refuse(ControlRefusal::PoseNotOwned);
        }
        handbackPending_ = Handback::None;
        regime_          = PoseRegime::Free;
        refusal_         = ControlRefusal::None;
        SetDiagnostic("regime %s: the pose is back with the stock free camera, on the retry of a "
                      "hand-back the arbiter had refused",
                      PoseRegimeName(regime_));
        return true;
    }

    bool ReleasePose() override {
        // Idempotent, and deliberately not a refusal: `cam release` is what a confused operator
        // types and it must always be safe.
        if (regime_ != PoseRegime::Commanded) {
            // ... but a hand-back the arbiter REFUSED is still outstanding, and `cam release` is
            // the only command that can finish it. Answering true here left the
            // camera stranded with the token in the wrong hands for the rest of the session.
            if (handbackPending_ != Handback::None) {
                return RetryHandback();
            }
            refusal_ = ControlRefusal::None;
            return true;
        }

        const camera_control::WritePointSeam* wp = g_writePoint;
        writePoint_ = nullptr;
        regime_     = PoseRegime::Free;

        // ORDER (ARCH-17 S3.3(b), restored by ARCH-19 R1). The hand-back is two facts and they are
        // separable: the engine gets viewstate+0x9C back -- Relinquish() re-arms every gate SetPose
        // cleared, which is the whole of S3.3(c) and the only thing that makes `cam free` after
        // `cam move` hand back a camera that answers input -- and the token gets back to
        // StockFreeCam. camera::ReleaseKeepingToken() does the first WITHOUT dropping the token, so
        // the second is the arbiter's own atomic transfer: no window in which a third claimant
        // wins, no counted release refusal for a hand-back that worked, and the pose-owner epoch
        // does not move. A transfer is a rename of the holder and not a new bind
        // (camera_writepoint_poseowner.h:315-317), and a second image polling PoseOwnerEpoch()
        // must not read every `cam release` as a rebind.
        if (wp && wp->releaseKeepingToken) {
            wp->releaseKeepingToken();
            if (!camera::TransferPoseOwner(camera::PoseOwner::WritePoint,
                                           camera::PoseOwner::StockFreeCam)) {
                handbackPending_ = Handback::Transfer;
                regime_ = PoseRegime::Unavailable;
                SetDiagnostic("%s", camera::LastPoseOwnerConflict());
                return Refuse(ControlRefusal::PoseNotOwned);
            }
        } else {
            // An image whose seam predates the fourth pointer. It is NOT refused: `cam release` is
            // what a confused operator types, and a refusal here strands the camera in regime
            // Commanded with no way back. The pre-R1 route works -- it is
            // what shipped -- so it is taken, said out loud in the sentence, and counted, because
            // it moves the epoch and re-opens the window R1 closed.
            ++g_handbackDegraded;
            if (wp && wp->release) wp->release();
            if (!camera::ClaimPoseOwner(camera::PoseOwner::StockFreeCam)) {
                handbackPending_ = Handback::Claim;
                regime_ = PoseRegime::Unavailable;
                SetDiagnostic("%s", camera::LastPoseOwnerConflict());
                return Refuse(ControlRefusal::PoseNotOwned);
            }
            handbackPending_ = Handback::None;
            refusal_ = ControlRefusal::None;
            SetDiagnostic("regime %s: the pose is back with the stock free camera, through the "
                          "degraded hand-back -- this image installed no releaseKeepingToken, so "
                          "the token went through None and the pose-owner epoch moved",
                          PoseRegimeName(regime_));
            return true;
        }
        handbackPending_ = Handback::None;
        refusal_ = ControlRefusal::None;
        SetDiagnostic("regime %s: the pose is back with the stock free camera",
                      PoseRegimeName(regime_));
        return true;
    }

    bool BeginOracleTrack() override {
        if (!available_) return Refuse(ControlRefusal::NotAvailable);
        oracleTrack_ = true;
        refusal_     = ControlRefusal::None;
        return true;
    }

    void EndOracleTrack() override { oracleTrack_ = false; }
    bool OracleTrackActive() const override { return oracleTrack_; }

    bool Available() const override { return available_; }

    // The lookup's answer, for the module's bound/refused line. Null while nothing is bound, which
    // is what a refusal must print rather than a space pointer wearing the word "rig".
    void* SpectatorCs() const { return reinterpret_cast<void*>(spectatorCs_); }
    ControlRefusal LastRefusal() const override { return refusal_; }

    // Refused before Init ran: nothing was resolved and nothing in the engine was read.
    void RefusePoseOwner() {
        available_ = false;
        refusal_   = ControlRefusal::PoseOwnerConflict;
    }

    void Drop() {
        // The token must not be stranded on the write point when the bind goes away, and the
        // engine must not be left with a cleared gate: both are ReleasePose's, so both happen
        // here rather than at one of Release()'s three call sites (ARCH-17 S4).
        ReleasePose();
        regime_    = PoseRegime::Free;
        available_ = false;
        refusal_   = ControlRefusal::NotAvailable;
        spectatorCs_ = 0;
        engine_ = nullptr;
        oracleTrack_ = false;
        boundId_ = camera_control::RigIdentity();
        arm_ = DiffArm::Absent;
        judgedCsSpace_ = nullptr;
    }

private:
    // The rig's local instance, every field asked of the memory manager first. Zero when the rig
    // or its instance array is not there to read, which SetMode reports as NotAvailable.
    uintptr_t LocalInstance() const {
        if (!spectatorCs_) return 0;
        if (!Readable(reinterpret_cast<const void*>(spectatorCs_), kCsHeaderBytes)) return 0;
        const uint16_t count = Load<uint16_t>(spectatorCs_ + facts::SPECTATOR_CS_INSTANCE_COUNT_OFF);
        const uint16_t local = Load<uint16_t>(spectatorCs_ + facts::SPECTATOR_CS_LOCAL_INDEX_OFF);
        if (local >= count) return 0;
        const uintptr_t base = Load<uintptr_t>(spectatorCs_ + facts::SPECTATOR_CS_INSTANCE_BASE_OFF);
        if (!base) return 0;
        const uintptr_t ci = base + static_cast<uintptr_t>(local) * kCiStride;
        if (!Readable(reinterpret_cast<const void*>(ci), kCiStride)) return 0;
        return ci;
    }

    const char* InstanceText() const {
        static char text[96];
        if (!Readable(reinterpret_cast<const void*>(spectatorCs_), kCsHeaderBytes)) {
            return "header unreadable";
        }
        _snprintf_s(text, sizeof text, _TRUNCATE, "count %u, local %u, base %llx",
                    Load<uint16_t>(spectatorCs_ + facts::SPECTATOR_CS_INSTANCE_COUNT_OFF),
                    Load<uint16_t>(spectatorCs_ + facts::SPECTATOR_CS_LOCAL_INDEX_OFF),
                    static_cast<unsigned long long>(
                        Load<uintptr_t>(spectatorCs_ + facts::SPECTATOR_CS_INSTANCE_BASE_OFF)));
        return text;
    }

    // AUDIT_ARCH_19_20 C6's explicit release for ARCH-19 R3(b)'s pin. The engine has just been
    // handed `num` and has written ITS OWN derivation of it to ci+0x26; reading that back is
    // p10-cam-9's oracle with the engine as the other arm, and it runs here so the row and the
    // shipping image share one body. A read that cannot be taken leaves the state alone --
    // could-not-ask is not a verdict.
    void CertifySelector(int32_t num) {
        uint8_t teamId = 0, teamSlot = 0;
        if (!camera_control::PackSelector(num, &teamId, &teamSlot)) return;
        int32_t packed = 0;
        if (!ObservedSelector(&packed, nullptr)) return;
        camera_control::RecordSelectorObservation(num, packed, teamId, teamSlot);
    }

    camera_control::SubjectResolve ResolveSubjectPoint(int32_t selector, float outPos[3]) {
        using camera_control::SubjectResolve;
        const camera_control::SubjectPoseSeam* sp = g_subjectPose;
        if (!sp || !sp->userOfSelector || !sp->worldPointOfUser) return SubjectResolve::NoSeam;

        uint8_t teamId = 0, teamSlot = 0;
        if (!camera_control::PackSelector(selector, &teamId, &teamSlot)) {
            return SubjectResolve::NoSuchSubject;
        }
        if (camera_control::SelectorDerivationState(selector) !=
            camera_control::SelectorDerivation::Certified) {
            return SubjectResolve::DerivationUncertified;
        }

        uint16_t userIndex = 0xFFFFu;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
        const SubjectResolve r = sp->userOfSelector(boundId_.netGame, selector, &userIndex);
        if (r != SubjectResolve::Resolved) return r;

        float pt[3] = {0.0f, 0.0f, 0.0f};
        if (!sp->worldPointOfUser(boundId_.netGame, userIndex, pt)) return SubjectResolve::NoBody;
        for (int i = 0; i < 3; ++i) {
            // A body read that succeeded and handed back a nan is not a point to aim at, and it
            // must not reach BuildFacing under the name of one.
            if (!isfinite(pt[i])) return SubjectResolve::NoBody;
        }
        memcpy(outPos, pt, sizeof pt);
        return SubjectResolve::Resolved;
    }

    bool Refuse(ControlRefusal r) {
        refusal_ = r;
        return false;
    }

    // The whole of the family's arithmetic (ARCH-17 FC-2): a vector added to a position, and a
    // quaternion built from a yaw and a pitch. No velocity, no acceleration, nothing that advances
    // on a clock -- that would be the integrator S8.1.2 deleted, re-entering under a new name.
    //
    // THE AXIS CONVENTION IS [U] AND IT IS STATED IN EXACTLY ONE PLACE -- camera_control.h's
    // DecomposeFacing / BuildFacing (ARCH-19 E2), which are the only bodies in the tree that
    // encode it. DL-02's w2e-p17-axis-plus-z carries the in-vivo check.
    //
    // The relative form decomposes the pose the engine currently holds through the same
    // convention, so encode and decode are inverses by construction and a delta of zero is
    // exactly the current facing. ROLL IS DROPPED by that round trip, which is why ARCH-19 R6
    // puts the round trip on the two rows that command a facing and nowhere else.
    bool ComposePose(const CommandedPose& p, const ViewPose& current, float pos[3],
                     float quat[4]) {
        pos[0] = current.pos[0];
        pos[1] = current.pos[1];
        pos[2] = current.pos[2];
        if (p.setPosition) {
            for (int i = 0; i < 3; ++i) {
                pos[i] = p.absolute ? p.pos[i] : current.pos[i] + p.pos[i];
            }
        }

        for (int i = 0; i < 3; ++i) {
            if (!isfinite(pos[i])) {
                SetDiagnostic("commanded pose refused: component %d of the destination is not a "
                              "finite number", i);
                refusal_ = ControlRefusal::PoseNotFinite;
                return false;
            }
        }

        // ARCH-19 R6, the three-way split: the current facing is read only on the one row where it
        // is an INPUT.
        if (!p.setOrientation) {
            // A position command asks for no orientation at all, so the orientation the engine
            // holds is copied through byte for byte. Roll survives -- a yaw/pitch round trip
            // cannot carry one and re-writing it would be a change nobody asked for -- and a
            // vertical facing is not grounds to refuse a command it is not an input to.
            // commandedYawDeg_ / commandedPitchDeg_ are LAST COMMANDED ORIENTATION and are
            // deliberately not touched here.
            memcpy(quat, current.quat, sizeof(float) * 4);
        } else if (p.absolute) {
            // An absolute facing names its destination outright. The parser already refuses a
            // commanded pitch at +-90 (ARCH-17 S3.1, decision 8), so the degenerate DESTINATION is
            // guarded upstream; the current facing is not one of this command's inputs.
            camera_control::BuildFacing(p.yawDeg, p.pitchDeg, quat);
            commandedYawDeg_   = p.yawDeg;
            commandedPitchDeg_ = p.pitchDeg;
        } else {
            float yawDeg   = 0.0f;
            float pitchDeg = 0.0f;
            if (!camera_control::DecomposeFacing(current.quat, &yawDeg, &pitchDeg)) {
                // Straight up or straight down: the yaw axis is degenerate and there is no facing
                // to add a delta to, so a relative look would silently re-aim the camera. This is
                // the one branch that reason applies to, and the refusal survives on it alone.
                SetDiagnostic("commanded pose refused: the view is within a tenth of a degree of "
                              "vertical, where a yaw has no meaning to add to");
                refusal_ = ControlRefusal::FacingDegenerate;
                return false;
            }
            yawDeg += p.yawDeg;
            pitchDeg += p.pitchDeg;
            camera_control::BuildFacing(yawDeg, pitchDeg, quat);
            commandedYawDeg_   = yawDeg;
            commandedPitchDeg_ = pitchDeg;
        }
        return true;
    }

    // ARCH-17. The regime, the write point it is held through, and what the last command asked
    // for -- the pose that LANDED is GetViewPose's answer and is never copied from here.
    PoseRegime                        regime_        = PoseRegime::Free;
    IReplayCamera*                    writePoint_    = nullptr;
    Handback                          handbackPending_ = Handback::None;
    bool                              haveCommanded_ = false;
    float                             commandedPos_[3]  = {0.0f, 0.0f, 0.0f};
    float                             commandedQuat_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float                             commandedYawDeg_   = 0.0f;
    float                             commandedPitchDeg_ = 0.0f;

    uintptr_t                         spectatorCs_ = 0;
    const camera_control::RigEngine*  engine_      = nullptr;
    CameraMode                        requested_   = CameraMode::Pov;
    bool                              oracleTrack_ = false;
    bool                              available_   = false;
    ControlRefusal                    refusal_     = ControlRefusal::NotAvailable;

    // The binding's own identity and the R2 arm published for it. judgedCsSpace_ is the CS slot the
    // arm was last asked about, which is what makes "the slot moved" a comparison and not a guess.
    camera_control::RigIdentity       boundId_{};
    DiffArm                           arm_           = DiffArm::Absent;
    void*                             judgedCsSpace_ = nullptr;
};

Control g_control;
bool    g_created = false;

struct ModeRow {
    CameraMode  mode;
    const char* name;
};
// The engine's own seven, in CAM.HTTP_CAMERA_MODE_HANDLER's own order.
const ModeRow kModes[] = {
    {CameraMode::Pov, "pov"},     {CameraMode::Follow, "follow"}, {CameraMode::Level, "level"},
    {CameraMode::Disc, "disc"},   {CameraMode::Side, "side"},     {CameraMode::Free, "free"},
    {CameraMode::Api, "api"},
};

bool SameName(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

}  // namespace

const char* ViewPoseSourceName(ViewPoseSource s) {
    switch (s) {
        case ViewPoseSource::ViewState:      return "view-state";
        case ViewPoseSource::CameraInstance: return "camera-instance";
    }
    return "unknown";
}

const char* PoseRegimeName(PoseRegime r) {
    switch (r) {
        case PoseRegime::Free:        return "free";
        case PoseRegime::Commanded:   return "commanded";
        case PoseRegime::Unavailable: return "unavailable";
    }
    return "unavailable";
}

const char* ControlRefusalName(ControlRefusal r) {
    switch (r) {
        case ControlRefusal::None:                  return "none";
        case ControlRefusal::NotAvailable:          return "not-available";
        case ControlRefusal::ModeReservedForOracle: return "mode-reserved-for-oracle";
        case ControlRefusal::OracleTrackActive:     return "oracle-track-active";
        case ControlRefusal::NumOutOfRange:         return "num-out-of-range";
        case ControlRefusal::UnknownModeName:       return "unknown-mode-name";
        case ControlRefusal::PoseOwnerConflict:     return "pose-owner-conflict";
        case ControlRefusal::PoseNotOwned:          return "pose-not-owned";
        case ControlRefusal::OracleTrackHolds:      return "oracle-track-holds";
        case ControlRefusal::PoseUnreadable:        return "pose-unreadable";
        case ControlRefusal::WritePointRefused:     return "write-point-refused";
        case ControlRefusal::FacingDegenerate:      return "facing-degenerate";
        case ControlRefusal::PoseNotFinite:         return "pose-not-finite";
        case ControlRefusal::LookAtUnresolvable:    return "look-at-unresolvable";
        // ARCH-23 S14.11 spells the enum value FreeCamHoldsNode and quotes its two neighbours in
        // this vocabulary's own kebab case in the same breath; the PRINTED word is one vocabulary
        // and every other member of it is kebab, so this joins them and the L3 refusal pattern
        // matches what is emitted rather than what a second spelling promised.
        case ControlRefusal::FreeCamHoldsNode:      return "free-cam-holds-node";
    }
    return "unknown";
}

namespace camera_control {

const char* ModeName(CameraMode m) {
    for (const ModeRow& r : kModes)
        if (r.mode == m) return r.name;
    return "unknown";
}

bool ModeFromName(const char* name, CameraMode* out) {
    if (!name || !out) return false;
    for (const ModeRow& r : kModes) {
        if (SameName(name, r.name)) {
            *out = r.mode;
            return true;
        }
    }
    return false;
}

bool IsSelectableByHuman(CameraMode m) { return m != CameraMode::Api; }

const char* RigProbeName(RigProbe p) {
    switch (p) {
        case RigProbe::NoNetGame:            return "no netGame";
        case RigProbe::NetGameUnreadable:    return "netGame unreadable";
        case RigProbe::NoLevelSpace:         return "level space null";
        case RigProbe::LevelSpaceUnreadable: return "level space table unreadable";
        case RigProbe::TableNotReady:        return "system table not constructed";
        case RigProbe::TableSortPending:     return "system table sort pending";
        case RigProbe::TableEmpty:           return "system table empty";
        case RigProbe::RetiredSlotNull:      return "retired probe value";
        case RigProbe::Ready:                return "ready";
    }
    return "?";
}

RigProbe Probe(void* netGame, RigIdentity* out) {
    *out = RigIdentity();
    out->netGame = netGame;
    if (!netGame) return RigProbe::NoNetGame;

    const uint8_t* base = static_cast<const uint8_t*>(netGame);
    const uint8_t* levelSlot = base + facts::NETGAME_SPECTATORCAM_CS_OFF;
    const uint8_t* csSlot    = base + facts::NETGAME_CS_SPACE_OFF;
    if (!Readable(levelSlot, sizeof(void*)) || !Readable(csSlot, sizeof(void*))) {
        return RigProbe::NetGameUnreadable;
    }
    memcpy(&out->levelSpace, levelSlot, sizeof(void*));
    memcpy(&out->csSpace, csSlot, sizeof(void*));
    // csSpace is read and recorded here and judged nowhere: a null one is the L3/L4 state and the
    // first second of a load, and gating the camera on a slot the camera does not use is exactly
    // what made the shipped version refuse a rig that was there (ARCH-12 R4).
    if (!out->levelSpace) return RigProbe::NoLevelSpace;

    const tableguard::TableCheck table =
        tableguard::CheckSpaceTable(out->levelSpace, kCsTableOff, kCsEntryStride, false);
    switch (table.verdict) {
        case tableguard::TableVerdict::Ready:      break;
        case tableguard::TableVerdict::Empty:      return RigProbe::TableEmpty;
        case tableguard::TableVerdict::Unreadable: return RigProbe::LevelSpaceUnreadable;
        case tableguard::TableVerdict::NotReady:
            // "not constructed" would be a false cause for a table that IS constructed and merely
            // unsorted, and the two wait on different things: one on the loading window, one on
            // the engine's own sort.
            return table.why == tableguard::TableWhy::SortPending ? RigProbe::TableSortPending
                                                                  : RigProbe::TableNotReady;
        case tableguard::TableVerdict::NotChecked: return RigProbe::TableNotReady;
    }
    out->tableBase = table.raw.base;
    return RigProbe::Ready;
}

IReplayCameraControl* Create(void* netGame) {
    // Before anything is resolved: the S8.1.7 write point clears viewstate+0x9C, which is the gate
    // the engine's own integrator reads, so a camera bound while that owns the pose would stop
    // responding to input the moment it wrote its first pose. camera_writepoint_poseowner.h holds
    // the rule; this is one of its two claim sites.
    if (!camera::ClaimPoseOwner(camera::PoseOwner::StockFreeCam)) {
        g_control.RefusePoseOwner();
        g_created = false;
        SetDiagnostic("%s", camera::LastPoseOwnerConflict());
        return nullptr;
    }
    g_created = g_control.Init(netGame);
    if (!g_created) {
        // A control that did not check out drives nothing, so it must not keep the token and lock
        // the fallback out of a pose it could have owned.
        camera::ReleasePoseOwner(camera::PoseOwner::StockFreeCam);
    }
    return g_created ? &g_control : nullptr;
}

void Release() {
    g_control.Drop();
    g_created = false;
    camera::ReleasePoseOwner(camera::PoseOwner::StockFreeCam);
    SetDiagnostic("released");
    // The latch is per binding session: a released control's refusals belong to the session that
    // ended, and the next bind must be able to say "none yet" and mean it.
    ClearViewReadRefusals();
}

const char* Diagnostic() { return g_diagnostic; }

void* BoundSpectatorCs() { return g_created ? g_control.SpectatorCs() : nullptr; }

const char* DifferentialWord() { return g_created ? g_control.DifferentialWord() : nullptr; }

bool ReviseDifferential(const RigIdentity& id) {
    return g_created ? g_control.Revise(id) : true;
}
const char* ViewReadRefusal() { return g_viewReadRefusal; }
unsigned long long ViewReadRefusals() { return g_viewReadRefusals; }

IReplayCameraControl* Get() { return g_created && g_control.Available() ? &g_control : nullptr; }

void SetEngineForTest(const RigEngine* engine) { g_engineForTest = engine; }

void            SetViewStateReader(ViewStateReader reader) { g_viewStateReader = reader; }
ViewStateReader InstalledViewStateReader() { return g_viewStateReader; }

void                  SetWritePoint(const WritePointSeam* wp) { g_writePoint = wp; }
const WritePointSeam* InstalledWritePoint() { return g_writePoint; }

void                   SetSubjectPose(const SubjectPoseSeam* sp) { g_subjectPose = sp; }
const SubjectPoseSeam* InstalledSubjectPose() { return g_subjectPose; }

const char* SubjectResolveName(SubjectResolve r) {
    switch (r) {
        case SubjectResolve::Resolved:              return "resolved";
        case SubjectResolve::NoSeam:                return "no subject-pose seam in this image";
        case SubjectResolve::RosterUnreadable:      return "the roster could not be read";
        case SubjectResolve::NoSuchSubject:         return "no player occupies that selector";
        case SubjectResolve::NoBody:                return "that player has no body this tick";
        case SubjectResolve::DerivationUncertified:
            return "the selector derivation is not certified against this engine yet "
                   "(select that player once with cam pov)";
    }
    return "unknown";
}

bool PackSelector(int32_t selector, uint8_t* teamIdOut, uint8_t* teamSlotOut) {
    if (selector < 1 || selector > IReplayCameraControl::kPlayerSelectorMax) return false;
    const bool firstTeam = selector <= 5;
    if (teamIdOut)   *teamIdOut   = firstTeam ? 1u : 0u;
    if (teamSlotOut) *teamSlotOut = static_cast<uint8_t>(firstTeam ? selector - 1 : selector - 6);
    return true;
}

int32_t PackedSelectorWord(uint8_t teamId, uint8_t teamSlot) {
    const uint32_t word = (static_cast<uint32_t>(teamSlot) << 16) | static_cast<uint32_t>(teamId);
    return static_cast<int32_t>(word);
}

SelectorDerivation SelectorDerivationState(int32_t selector) {
    if (selector < 1 || selector > IReplayCameraControl::kPlayerSelectorMax) {
        return SelectorDerivation::Uncertified;
    }
    return g_selectorDerivation[selector];
}

const char* SelectorDerivationName(SelectorDerivation d) {
    switch (d) {
        case SelectorDerivation::Uncertified: return "uncertified";
        case SelectorDerivation::Certified:   return "certified";
        case SelectorDerivation::Falsified:   return "falsified";
    }
    return "unknown";
}

void RecordSelectorObservation(int32_t selector, int32_t packed, uint8_t teamId, uint8_t teamSlot) {
    if (selector < 1 || selector > IReplayCameraControl::kPlayerSelectorMax) return;
    SelectorDerivation& state = g_selectorDerivation[selector];
    if (state == SelectorDerivation::Falsified) return;  // sticky: never re-certified
    state = (packed == PackedSelectorWord(teamId, teamSlot)) ? SelectorDerivation::Certified
                                                             : SelectorDerivation::Falsified;
}

bool FacingToward(const float from[3], const float to[3], float* yawDeg, float* pitchDeg) {
    if (!from || !to || !yawDeg || !pitchDeg) return false;
    const double dx = static_cast<double>(to[0]) - static_cast<double>(from[0]);
    const double dy = static_cast<double>(to[1]) - static_cast<double>(from[1]);
    const double dz = static_cast<double>(to[2]) - static_cast<double>(from[2]);
    const double horizontal = sqrt(dx * dx + dz * dz);
    const double len        = sqrt(dx * dx + dy * dy + dz * dz);
    if (!isfinite(len) || len < 1.0e-4) return false;
    const double kToDeg = 180.0 / 3.14159265358979323846;
    *yawDeg   = static_cast<float>(atan2(dx, dz) * kToDeg);
    *pitchDeg = static_cast<float>(atan2(dy, horizontal) * kToDeg);
    return true;
}
uint64_t              PoseHandbackDegraded() { return g_handbackDegraded; }

void BuildFacing(float yawDeg, float pitchDeg, float quat[4]) {
    const double kToRad = 3.14159265358979323846 / 180.0;
    const double y = static_cast<double>(yawDeg) * kToRad * 0.5;
    const double p = static_cast<double>(-pitchDeg) * kToRad * 0.5;  // +pitch raises the view
    const double cy = cos(y), sy = sin(y), cp = cos(p), sp = sin(p);
    quat[0] = static_cast<float>(cy * sp);
    quat[1] = static_cast<float>(sy * cp);
    quat[2] = static_cast<float>(-sy * sp);
    quat[3] = static_cast<float>(cy * cp);
}

// Through the forward vector rather than by inverting the product term by term: the vector is
// continuous everywhere the facing is, so a pose the engine wrote with a roll in it, or with
// the opposite quaternion sign (q and -q are one rotation), still decomposes to the facing a
// human would name. Returns false where the facing is vertical and a yaw does not exist.
bool DecomposeFacing(const float q[4], float* yawDeg, float* pitchDeg) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double fx = 2.0 * (x * z + w * y);
    const double fy = 2.0 * (y * z - w * x);
    const double fz = 1.0 - 2.0 * (x * x + y * y);
    const double horizontal = sqrt(fx * fx + fz * fz);
    if (!isfinite(horizontal) || horizontal < 1.0e-3) return false;
    const double kToDeg = 180.0 / 3.14159265358979323846;
    *yawDeg   = static_cast<float>(atan2(fx, fz) * kToDeg);
    // The sign is +fy, not -fy. BuildFacing puts sin(pitch) in the forward vector's Y --
    // ARCH-19 R5's "positive pitch raises the view" -- so atan2(-fy, .) handed back the
    // NEGATIVE of the pitch that built the quaternion, and the pair were not inverses at
    // any non-zero pitch. Every landed round trip used pitch 0, where the two agree.
    *pitchDeg = static_cast<float>(atan2(fy, horizontal) * kToDeg);
    return true;
}


const char* PoseMotionName(PoseMotion m) {
    switch (m) {
        case PoseMotion::Moved:       return "moved";
        case PoseMotion::Still:       return "still";
        case PoseMotion::CouldNotAsk: return "could-not-ask";
    }
    return "unknown";
}

camera::ViewPoseRead SampleViewPose(const IReplayCameraControl* control, ViewPoseSample* out) {
    *out = ViewPoseSample{};
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    out->wallTicks = now.QuadPart;
    if (!control) return out->read;  // NotBound: the observer runs rows whether or not we bound
    out->read = control->GetViewPose(&out->pose);
    return out->read;
}

PoseMotion PoseMotionBetween(const ViewPoseSample& first, const ViewPoseSample& second,
                             double* metresMoved, double* wallSeconds) {
    if (metresMoved) *metresMoved = 0.0;
    if (wallSeconds) *wallSeconds = 0.0;

    if (first.read != camera::ViewPoseRead::Ok || second.read != camera::ViewPoseRead::Ok) {
        return PoseMotion::CouldNotAsk;
    }
    // Two poses from two different sources are two different quantities, and their difference is
    // not motion: the engine changed arms between the samples and the pair proves nothing.
    if (first.pose.source != second.pose.source) return PoseMotion::CouldNotAsk;

    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return PoseMotion::CouldNotAsk;
    if (second.wallTicks <= first.wallTicks) return PoseMotion::CouldNotAsk;

    if (wallSeconds) {
        *wallSeconds = static_cast<double>(second.wallTicks - first.wallTicks) /
                       static_cast<double>(freq.QuadPart);
    }
    if (metresMoved) {
        double sum = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double d = static_cast<double>(second.pose.pos[i]) -
                             static_cast<double>(first.pose.pos[i]);
            sum += d * d;
        }
        *metresMoved = sqrt(sum);
    }

    const bool same = memcmp(first.pose.pos, second.pose.pos, sizeof first.pose.pos) == 0 &&
                      memcmp(first.pose.quat, second.pose.quat, sizeof first.pose.quat) == 0;
    return same ? PoseMotion::Still : PoseMotion::Moved;
}

// ============================================================================================
// ARCH-23 S14.11.
// ============================================================================================

int NetGameUpdateGateBit(void* netGame) {
    if (!netGame) return -1;
    const uintptr_t slot = reinterpret_cast<uintptr_t>(netGame) + facts::NETGAME_UIFLAGS_PTR_OFF;
    if (!Readable(reinterpret_cast<const void*>(slot), sizeof(void*))) return -1;
    // +0x2DA0 holds a POINTER to the flag word, which is why this dereferences twice and guards
    // both -- identity_access.cpp:378 takes the same two steps for bit 0x4 of the same word.
    const uint32_t* flags = Load<const uint32_t*>(slot);
    if (!flags || !Readable(flags, sizeof(uint32_t))) return -1;
    return (*flags & 4u) != 0 ? 1 : 0;
}

bool BoundInstanceCounts(uint16_t* count, uint16_t* local) {
    void* cs = BoundSpectatorCs();
    if (!cs) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(cs);
    if (!Readable(cs, kCsHeaderBytes)) return false;
    if (count) *count = Load<uint16_t>(base + facts::SPECTATOR_CS_INSTANCE_COUNT_OFF);
    if (local) *local = Load<uint16_t>(base + facts::SPECTATOR_CS_LOCAL_INDEX_OFF);
    return true;
}

// Eight is four windows' worth of the fastest a human types, and an overrun is counted rather
// than dropped silently: a lost `end` would leave a window open and the next `begin` would
// bracket the wrong samples.
const size_t kFlightMarkQueue = 8;
bool g_flightMarks[kFlightMarkQueue] = {};
size_t g_flightMarkHead = 0;
size_t g_flightMarkCount = 0;
unsigned long long g_flightMarksDropped = 0;

int64_t WallTicksNow() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double WallSeconds(int64_t ticks) {
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0.0;
    return static_cast<double>(ticks) / static_cast<double>(freq.QuadPart);
}

const char* FlightMarkerText(bool begin) {
    return begin ? "flight window begin" : "flight window end";
}

const char* FlightMarkerDroppedText() { return "flight marker dropped (queue full)"; }

bool MarkFlightWindow(bool begin) {
    if (g_flightMarkCount == kFlightMarkQueue) {
        ++g_flightMarksDropped;
#ifdef W2E32_MUTATE_MARKER_DROP_SILENT
        return true;
#else
        return false;
#endif
    }
    g_flightMarks[(g_flightMarkHead + g_flightMarkCount) % kFlightMarkQueue] = begin;
    ++g_flightMarkCount;
    return true;
}

bool TakeFlightMark(bool* begin) {
    if (g_flightMarkCount == 0) return false;
    if (begin) *begin = g_flightMarks[g_flightMarkHead];
    g_flightMarkHead = (g_flightMarkHead + 1) % kFlightMarkQueue;
    --g_flightMarkCount;
    return true;
}

unsigned long long FlightMarksDropped() { return g_flightMarksDropped; }

const char* FlightVerdictName(FlightVerdict v) {
    switch (v) {
        case FlightVerdict::Pass:   return "PASS";
        case FlightVerdict::Fail:   return "FAIL";
        case FlightVerdict::NotRun: return "NOT-RUN";
    }
    return "NOT-RUN";
}

namespace {

bool FlightScored(const FlightSample& s, const FlightWindow& w) {
    if (!s.poseOk || !s.freeCam || !s.armed) return false;
#ifdef W2E32_MUTATE_FLIGHT_WINDOW_IGNORED
    (void)w;
    return true;
#else
    return s.seconds >= w.beginSeconds && s.seconds <= w.endSeconds;
#endif
}

double FlightDistance(const FlightSample& a, const FlightSample& b) {
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double d = static_cast<double>(b.pos[i]) - static_cast<double>(a.pos[i]);
        sum += d * d;
    }
    return sqrt(sum);
}

}  // namespace

FlightVerdict EvaluateFlight(const FlightWindow& w, FlightReport* out) {
    FlightReport r;
    const FlightVerdict notRun = FlightVerdict::NotRun;
    if (!w.haveBegin && !w.haveEnd) {
        r.reason = "no marker channel";
        if (out) *out = r;
        return notRun;
    }
    if (!w.haveBegin || !w.haveEnd) {
        r.reason = "no window: one flight marker without its pair";
        if (out) *out = r;
        return notRun;
    }
    if (w.truncated) {
        r.reason = "window overflowed the sample ring";
        if (out) *out = r;
        return notRun;
    }

    size_t first = 0, last = 0;
    bool haveFirst = false;
    for (size_t i = 0; i < w.count; ++i) {
        if (!FlightScored(w.samples[i], w)) continue;
        if (!haveFirst) {
            first = i;
            haveFirst = true;
        }
        last = i;
        ++r.scored;
    }
    if (r.scored < 2) {
        r.reason = "fewer than 2 flying pose samples inside the window";
        if (out) *out = r;
        return notRun;
    }
    r.spanSeconds = w.samples[last].seconds - w.samples[first].seconds;

    for (size_t i = 0; i < w.count; ++i) {
        if (!FlightScored(w.samples[i], w)) continue;
        for (size_t j = i + 1; j < w.count; ++j) {
            if (!FlightScored(w.samples[j], w)) continue;
            const double d = FlightDistance(w.samples[i], w.samples[j]);
            if (d > r.maxDeltaM) r.maxDeltaM = d;
        }
    }

#ifdef W2E32_MUTATE_FLIGHT_FAILS_ON_PAUSE
    for (size_t i = 0; i < w.count; ++i) {
        if (!FlightScored(w.samples[i], w)) continue;
        for (size_t j = i + 1; j < w.count; ++j) {
            if (!FlightScored(w.samples[j], w)) continue;
            if (FlightDistance(w.samples[i], w.samples[j]) <= kFlightMovedMetres) {
                r.reason = "FreeCamPoseStatic";
                if (out) *out = r;
                return FlightVerdict::Fail;
            }
        }
    }
#endif

    if (r.maxDeltaM > kFlightMovedMetres) {
        r.reason = "moved";
        if (out) *out = r;
        return FlightVerdict::Pass;
    }
    if (r.spanSeconds < kFlightStillSeconds) {
        r.reason = "still window shorter than 3 s";
        if (out) *out = r;
        return notRun;
    }
    if (r.scored < kFlightStillSamples) {
        r.reason = "still window holds fewer than 3 flying pose samples";
        if (out) *out = r;
        return notRun;
    }
    r.reason = "FreeCamPoseStatic";
    if (out) *out = r;
#ifdef W2E32_MUTATE_FLIGHT_NEVER_FAILS
    return notRun;
#else
    return FlightVerdict::Fail;
#endif
}

}  // namespace camera_control
}  // namespace replay
