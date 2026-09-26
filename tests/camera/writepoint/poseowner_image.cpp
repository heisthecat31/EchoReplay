// W1-H. One of the two claimant IMAGES for the pose-owner control.
//
// The defect AUDIT_E E3 found was invisible to a single-binary test: the arbiter's state was a
// per-image static, and the two claimants ship in different images -- camera_control in
// EchoReplay.dll / EchoReplay.dll, camera::Create's only caller in probe_camera.dll. Linking both
// claimants into ONE test binary is a configuration the product does not have, so the check could
// not fail on the thing it was written to catch.
//
// So this file is compiled TWICE into two SHARED libraries -- one linking the write point, one
// linking the camera control -- and the driver loads both. Two targets, one source, one difference,
// which is the same shape as guardhost.cpp beside it. One source also means the two export tables
// are identical by construction, so the driver calls the same names through both images and cannot
// accidentally exercise one of them differently.
//
// Each image claims only ITS OWN owner. There is no "claim anything" entry point, because the
// interesting release case is precisely a module releasing the owner it is responsible for while a
// DIFFERENT owner holds the pose -- camera_module::Init and ::Shutdown both call
// camera_control::Release() unconditionally, and an unkeyed release there would hand the write
// point's pose away.

#include <string.h>

#include "engine/camera_writepoint_poseowner.h"

#if defined(POSEOWNER_IMAGE_WRITEPOINT)
#include "engine/camera_writepoint.h"
#elif defined(POSEOWNER_IMAGE_CONTROL)
#include "camera/camera_control.h"
#else
#error "poseowner_image.cpp needs POSEOWNER_IMAGE_WRITEPOINT or POSEOWNER_IMAGE_CONTROL"
#endif

using replay::camera::PoseOwner;

namespace {

#if defined(POSEOWNER_IMAGE_WRITEPOINT)
constexpr PoseOwner kMine = PoseOwner::WritePoint;
constexpr const char* kMyName = "WritePoint";
#else
constexpr PoseOwner kMine = PoseOwner::StockFreeCam;
constexpr const char* kMyName = "StockFreeCam";
#endif

}  // namespace

#define POSE_EXPORT extern "C" __declspec(dllexport)

POSE_EXPORT const char* PoseImage_Which() { return kMyName; }

POSE_EXPORT int PoseImage_Claim() { return replay::camera::ClaimPoseOwner(kMine) ? 1 : 0; }

POSE_EXPORT void PoseImage_Release() { replay::camera::ReleasePoseOwner(kMine); }

POSE_EXPORT int PoseImage_Held() { return static_cast<int>(replay::camera::PoseOwnerHeld()); }

POSE_EXPORT unsigned long long PoseImage_Conflicts() {
    return replay::camera::PoseOwnerConflicts();
}

POSE_EXPORT unsigned long long PoseImage_ReleaseRefusals() {
    return replay::camera::PoseOwnerReleaseRefusals();
}

POSE_EXPORT unsigned PoseImage_Epoch() { return replay::camera::PoseOwnerEpoch(); }

POSE_EXPORT unsigned PoseImage_Key() { return replay::camera::PoseOwnerImageKey(); }

POSE_EXPORT const char* PoseImage_Arbiter() { return replay::camera::PoseOwnerArbiter(); }

POSE_EXPORT const char* PoseImage_LastConflict() {
    return replay::camera::LastPoseOwnerConflict();
}

POSE_EXPORT void PoseImage_Reset() { replay::camera::ResetPoseOwnerForTest(); }

// ARCH-17 S3.3(b), asked from inside this image. `from` and `to` are passed through as raw
// PoseOwner values rather than fixed to kMine, because the case that matters is an image
// asking to move a token a DIFFERENT image holds -- which no "transfer mine" entry point
// could express, and which is the E3 harm with a new name if it succeeds.
POSE_EXPORT int PoseImage_Transfer(int from, int to) {
    return replay::camera::TransferPoseOwner(static_cast<PoseOwner>(from),
                                            static_cast<PoseOwner>(to))
               ? 1
               : 0;
}

POSE_EXPORT unsigned long long PoseImage_Transfers() {
    return replay::camera::PoseOwnerTransfers();
}

POSE_EXPORT unsigned long long PoseImage_TransferRefusals() {
    return replay::camera::PoseOwnerTransferRefusals();
}

// The shipping call site, run from inside this image. Neither module resolves an engine pointer
// before it claims, so the refusal arm runs offline here exactly as it would in vivo; with the pose
// free, both fail for want of an engine instead, which is the other arm the driver checks.
POSE_EXPORT int PoseImage_RealCreate() {
#if defined(POSEOWNER_IMAGE_WRITEPOINT)
    return replay::camera::Create(nullptr) != nullptr ? 1 : 0;
#else
    return replay::camera_control::Create(nullptr) != nullptr ? 1 : 0;
#endif
}

// Why the real Create refused, in each module's own vocabulary: the write point publishes a refusal
// code, the control publishes a diagnostic string. The driver asserts against both spellings rather
// than inventing a third.
POSE_EXPORT const char* PoseImage_RealWhy() {
#if defined(POSEOWNER_IMAGE_WRITEPOINT)
    replay::camera::Stats stats{};
    replay::camera::GetStats(&stats);
    return replay::camera::RefusalName(stats.refusal);
#else
    return replay::camera_control::Diagnostic();
#endif
}
