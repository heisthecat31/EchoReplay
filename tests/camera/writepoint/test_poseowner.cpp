// W1-H / W2-E. At most one pose owner, and it is enforced rather than described.
//
// The hazard: the stock free camera flies on the engine's own
// integrator, gated by `cmp dword ptr [reg+9Ch], 0`, and this packet's write point CLEARS that gate
// and re-clears it every frame. Bind both and the fallback's first pose silently switches the
// primary's input off -- a wrong camera that looks like a UI fault. Nothing enforced the exclusion;
// what kept it safe was that camera::Create() had no caller, and "currently uncalled" is not an
// invariant.
//
// The control that matters is the BOTH-BOUND case, in both orders, through each module's real entry
// point. Neither entry point needs an engine here: both claim the pose BEFORE they resolve
// anything, so the refusal arm runs offline exactly as it would in vivo.

#include <doctest.h>

#include <string>

#include "camera/camera_control.h"
#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_poseowner.h"

using replay::IReplayCameraControl;
using replay::camera::ClaimPoseOwner;
using replay::camera::LastPoseOwnerConflict;
using replay::camera::PoseOwner;
using replay::camera::PoseOwnerConflicts;
using replay::camera::PoseOwnerHeld;
using replay::camera::PoseOwnerName;
using replay::camera::ReleasePoseOwner;
using replay::camera::ResetPoseOwnerForTest;
namespace camera = replay::camera;
namespace camera_control = replay::camera_control;

TEST_CASE("the token is exclusive, re-entrant for its holder, and released only by its holder") {
    ResetPoseOwnerForTest();
    CHECK(PoseOwnerHeld() == PoseOwner::None);
    CHECK(PoseOwnerConflicts() == 0);
    CHECK(std::string(LastPoseOwnerConflict()) == "none");

    REQUIRE(ClaimPoseOwner(PoseOwner::WritePoint));
    CHECK(PoseOwnerHeld() == PoseOwner::WritePoint);

    // Both modules re-bind whenever the rig identity changes, so re-claiming is ordinary traffic
    // and must not read as a conflict.
    CHECK(ClaimPoseOwner(PoseOwner::WritePoint));
    CHECK(PoseOwnerConflicts() == 0);

    CHECK_FALSE(ClaimPoseOwner(PoseOwner::StockFreeCam));
    CHECK(PoseOwnerHeld() == PoseOwner::WritePoint);   // never overridden
    CHECK(PoseOwnerConflicts() == 1);
    CHECK(std::string(LastPoseOwnerConflict()) ==
          "pose-owner-conflict: WritePoint vs StockFreeCam");

    // The camera module calls camera_control::Release() from both Init and Shutdown, unconditionally.
    // If release were not keyed on the claimant, that would hand the write point's pose away.
    ReleasePoseOwner(PoseOwner::StockFreeCam);
    CHECK(PoseOwnerHeld() == PoseOwner::WritePoint);

    ReleasePoseOwner(PoseOwner::WritePoint);
    CHECK(PoseOwnerHeld() == PoseOwner::None);
    CHECK(ClaimPoseOwner(PoseOwner::StockFreeCam));
    CHECK(PoseOwnerHeld() == PoseOwner::StockFreeCam);
    CHECK(PoseOwnerConflicts() == 1);                  // the release was not a conflict
    ResetPoseOwnerForTest();
}

TEST_CASE("the write point owns the pose: camera_control::Create is refused, not bound") {
    ResetPoseOwnerForTest();
    REQUIRE(ClaimPoseOwner(PoseOwner::WritePoint));

    IReplayCameraControl* control = camera_control::Create(nullptr);
    CHECK(control == nullptr);
    CHECK(camera_control::Get() == nullptr);
    CHECK(PoseOwnerHeld() == PoseOwner::WritePoint);
    CHECK(PoseOwnerConflicts() == 1);

    // The refusal is NAMED, and it is not the refusal a rig-less netGame would produce. Delete the
    // claim from camera_control::Create and this line reads "no-netgame" instead: that is the arm
    // this check exists to be.
    CHECK(std::string(camera_control::Diagnostic()) ==
          "pose-owner-conflict: WritePoint vs StockFreeCam");

    ResetPoseOwnerForTest();
    camera_control::Release();
}

TEST_CASE("the stock free camera owns the pose: camera::Create is refused before it resolves") {
    ResetPoseOwnerForTest();
    REQUIRE(ClaimPoseOwner(PoseOwner::StockFreeCam));

    CHECK(camera::Create(nullptr) == nullptr);
    camera::Stats stats{};
    camera::GetStats(&stats);
    CHECK(stats.refusal == camera::Refusal::PoseOwnerConflict);
    CHECK(std::string(camera::RefusalName(stats.refusal)) == "pose-owner-conflict");
    CHECK(stats.available == false);

    // Refused before Init, so no site was probed and no engine pointer was resolved: a refusal that
    // had run Init would carry the resolve step's working.
    CHECK(stats.levelSpace == 0);
    CHECK(stats.csSpace == 0);
    CHECK(stats.resolveWhy[0] == 0);
    CHECK(stats.prologueWhy[0] == 0);

    CHECK(PoseOwnerHeld() == PoseOwner::StockFreeCam);
    CHECK(PoseOwnerConflicts() == 1);
    CHECK(std::string(LastPoseOwnerConflict()) ==
          "pose-owner-conflict: StockFreeCam vs WritePoint");
    ResetPoseOwnerForTest();
}

TEST_CASE("release then re-claim: neither module keeps a pose it did not check out") {
    ResetPoseOwnerForTest();

    // A write point that cannot check out (no engine in this process) writes nothing, so holding
    // the token would lock the stock free camera out of a pose it could have had.
    CHECK(camera::Create(nullptr) == nullptr);
    CHECK(PoseOwnerHeld() == PoseOwner::None);

    // Same obligation on the other side: no rig here, so Create fails and the token comes back.
    CHECK(camera_control::Create(nullptr) == nullptr);
    CHECK(PoseOwnerHeld() == PoseOwner::None);
    CHECK(PoseOwnerConflicts() == 0);
    CHECK(std::string(camera_control::Diagnostic()) != "pose-owner-conflict: WritePoint vs StockFreeCam");

    // And an explicit release re-opens the slot for the other owner.
    REQUIRE(ClaimPoseOwner(PoseOwner::WritePoint));
    camera::Release();
    CHECK(PoseOwnerHeld() == PoseOwner::None);
    CHECK(ClaimPoseOwner(PoseOwner::StockFreeCam));
    camera_control::Release();
    CHECK(PoseOwnerHeld() == PoseOwner::None);

    ResetPoseOwnerForTest();
}

// The owner-naming sweep lives in test_poseowner_images.cpp beside the Unavailable case, so there
// is one place that has to be updated when the enum grows.
