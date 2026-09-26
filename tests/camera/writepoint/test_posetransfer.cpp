// W1-H, ARCH-17 S3.3. The two halves of "the product can command a pose without killing the
// camera the owner actually flies".
//
// (b) THE TRANSFER. `IReplayCamera::SetPose` is unreachable from EchoReplay.dll today: camera_control
//     claims StockFreeCam at bind and holds it for the session, so camera::Create's WritePoint claim
//     can never succeed and the write point is linked, dead code. The fix is not a second arbiter and
//     not release-then-claim -- it is one interlocked move of the token, so the holder is never
//     observably unheld and a third claimant cannot win a window.
//
// (c) THE RE-ARM. SetPose clears viewstate+0x9C on every view it writes, +0x9C is written in exactly
//     one place in echovr.exe (the base constructor, W1-H S4), and nothing in the image re-arms it.
//     Relinquish() therefore has to put back what SetPose took, or `cam free` after `cam move` hands
//     the operator a camera that ignores input for the rest of the session -- the owner's headline
//     verb, broken by the feature added to serve it.
//
// The gate half is driven over FABRICATED view states through the same detail::WriteViewStates the
// shipping SetPose calls, and the hand-back through the shipping camera::Release(). No engine, no
// echovr.exe, and no second copy of either loop.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <doctest.h>

#include "bootstrap/livesafety.h"
#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"
#include "engine/camera_writepoint_poseowner.h"

// The write loop below refuses in a process that is not contained, and a
// test binary has never classified itself, so it would otherwise read as NotDecided and every case
// in this file would be measuring the gate instead of the loop. The session these cases describe
// is a contained offline playback; this says so once, before doctest runs anything.
const bool g_containedForTheseCases = [] {
    replay::livesafety::SetForTest(false, true);
    return true;
}();

using replay::camera::ClaimPoseOwner;
using replay::camera::LastPoseOwnerConflict;
using replay::camera::PoseOwner;
using replay::camera::PoseOwnerHeld;
using replay::camera::PoseOwnerEpoch;
using replay::camera::PoseOwnerTransferRefusals;
using replay::camera::PoseOwnerTransfers;
using replay::camera::ReleasePoseOwner;
using replay::camera::ResetPoseOwnerForTest;
using replay::camera::TransferPoseOwner;
namespace camera = replay::camera;

using namespace replay::camera_facts;

namespace {

// One fabricated view state per element, each big enough to hold every offset the write touches,
// and an array of pointers to them shaped like the engine's own. Real heap memory, so the restore
// path's readability probe answers about a page that exists.
struct FakeViews {
    std::vector<std::vector<unsigned char>> objects;
    std::vector<uintptr_t>                  slots;

    explicit FakeViews(size_t n, int32_t gate = 1) {
        objects.resize(n);
        slots.resize(n);
        for (size_t i = 0; i < n; ++i) {
            objects[i].assign(0x100, 0);
            slots[i] = reinterpret_cast<uintptr_t>(objects[i].data());
            SetGate(i, gate);
        }
    }

    uintptr_t Array() const { return reinterpret_cast<uintptr_t>(slots.data()); }

    int32_t Gate(size_t i) const {
        int32_t v = 0;
        std::memcpy(&v, objects[i].data() + kViewStateInputEnaOff, sizeof v);
        return v;
    }

    void SetGate(size_t i, int32_t v) {
        std::memcpy(objects[i].data() + kViewStateInputEnaOff, &v, sizeof v);
    }

    float Fov(size_t i) const {
        float v = 0.0f;
        std::memcpy(&v, objects[i].data() + kViewStateFovYOff, sizeof v);
        return v;
    }

    void Pos(size_t i, float out[3]) const {
        std::memcpy(out, objects[i].data() + kViewStatePosOff, 12);
    }
};

const float kPos[3]  = {12.5f, -3.25f, 8.0f};
const float kQuat[4] = {0.0f, 0.7071f, 0.0f, 0.7071f};

// The shipping hand-back. Release() is camera::Relinquish() plus the token drop, and it is the only
// drain there is -- so a test that called RestoreViewGates directly would be testing a function
// rather than the path Relinquish takes.
camera::Stats HandBack() {
    camera::Release();
    camera::Stats s{};
    camera::GetStats(&s);
    return s;
}

}  // namespace

TEST_CASE("the view-state write refuses outright in a live online session") {
    // ★ In the owner's live public spectate this loop would zero the input
    // gate on EVERY view state and hand the engine's camera to us -- reachable from one `cam look
    // at 3` typed into the console. Nothing on the chain asked the predicate before this fix.
    //
    // The view states here are fabricated, so the assertion is on the bytes themselves: a live
    // session leaves the gate at the value it found and the position untouched.
    HandBack();

    FakeViews views(3);
    views.SetGate(0, 7);
    views.SetGate(1, 7);
    views.SetGate(2, 7);

    replay::livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
    camera::Stats live{};
    camera::detail::WriteViewStates(views.Array(), 3, kPos, kQuat, 1.2f, live);
    CHECK(live.viewsWritten == 0);
    CHECK(live.gateReasserts == 0);
    for (size_t i = 0; i < 3; ++i) {
        INFO("view ", i);
        CHECK(views.Gate(i) == 7);       // the engine's own integrator is still armed
        float pos[3] = {};
        views.Pos(i, pos);
        CHECK(pos[0] == 0.0f);           // and nothing of ours landed
        CHECK(pos[1] == 0.0f);
        CHECK(pos[2] == 0.0f);
    }

    // A lock that did not take is not containment either -- the same refusal, a different reason.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/false);
    camera::Stats unlocked{};
    camera::detail::WriteViewStates(views.Array(), 3, kPos, kQuat, 1.2f, unlocked);
    CHECK(unlocked.viewsWritten == 0);
    CHECK(views.Gate(0) == 7);

    // ★ THE CONTROL. Contained, same views, same call: it writes. Without this arm the two above
    // would pass against a build whose write loop had simply stopped working.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    camera::Stats contained{};
    camera::detail::WriteViewStates(views.Array(), 3, kPos, kQuat, 1.2f, contained);
    CHECK(contained.viewsWritten == 3);
    CHECK(contained.gateReasserts == 3);
    CHECK(views.Gate(0) == 0);

    HandBack();  // and give the fabricated gates back before the next case
}

// ------------------------------------------------------------------ (c) the re-arm

TEST_CASE("a commanded pose disarms the gate and the hand-back puts back what it found") {
    HandBack();  // start from an empty ledger whatever ran before this

    FakeViews views(3);
    views.SetGate(2, 7);  // a non-zero that is not 1: the restore owes the PRE-VALUE, not a literal

    camera::Stats wrote{};
    camera::detail::WriteViewStates(views.Array(), 3, kPos, kQuat, 1.2f, wrote);

    CHECK(wrote.viewsWritten == 3);
    CHECK(wrote.viewCount == 3);
    CHECK(wrote.gateReasserts == 3);
    CHECK(wrote.gateUnrecorded == 0);
    for (size_t i = 0; i < 3; ++i) {
        INFO("view ", i);
        CHECK(views.Gate(i) == 0);   // the stock free camera is switched off, as SetPose intends
        float pos[3] = {};
        views.Pos(i, pos);
        CHECK(pos[0] == doctest::Approx(kPos[0]));
        CHECK(pos[2] == doctest::Approx(kPos[2]));
        CHECK(views.Fov(i) == doctest::Approx(1.2f));
    }

    // ★ THE DEFECT. Against today's Relinquish() -- which zeroes this module's bookkeeping and
    // restores nothing -- every one of these stays 0 and the free camera is dead for the session.
    const camera::Stats after = HandBack();
    CHECK(after.gateRestored == 3);
    CHECK(after.gateNotOurs == 0);
    CHECK(after.gateUnreadable == 0);
    CHECK(views.Gate(0) == 1);
    CHECK(views.Gate(1) == 1);
    CHECK(views.Gate(2) == 7);  // its own pre-value, not the constructor's 1
}

TEST_CASE("the hand-back re-arms only what the write point disarmed") {
    HandBack();

    // Nothing was ever written, so nothing is owed. A hand-back that armed a gate it never cleared
    // would be switching the engine's integrator on in a session that never touched it.
    const camera::Stats none = HandBack();
    CHECK(none.gateRestored == 0);
    CHECK(none.gateNotOurs == 0);

    // A view whose gate was already clear before we arrived is not ours either.
    FakeViews views(2, 0);
    camera::Stats wrote{};
    camera::detail::WriteViewStates(views.Array(), 2, kPos, kQuat, 1.0f, wrote);
    CHECK(wrote.viewsWritten == 2);
    CHECK(wrote.gateReasserts == 0);

    const camera::Stats after = HandBack();
    CHECK(after.gateRestored == 0);
    CHECK(views.Gate(0) == 0);
    CHECK(views.Gate(1) == 0);
}

TEST_CASE("the engine taking the gate back is recorded once and never clobbered") {
    HandBack();

    FakeViews views(1);
    views.SetGate(0, 3);

    camera::Stats wrote{};
    camera::detail::WriteViewStates(views.Array(), 1, kPos, kQuat, 1.0f, wrote);
    CHECK(wrote.gateReasserts == 1);

    // CAM.ENTER_FREECAM and the base constructor put 1 back between our writes. The second write
    // must not overwrite the pre-state with the engine's re-arm: 3 is what the session started at.
    views.SetGate(0, 1);
    camera::detail::WriteViewStates(views.Array(), 1, kPos, kQuat, 1.0f, wrote);
    CHECK(wrote.gateReasserts == 2);

    CHECK(HandBack().gateRestored == 1);
    CHECK(views.Gate(0) == 3);
}

TEST_CASE("a gate the engine re-armed after our last write is left alone") {
    HandBack();

    FakeViews views(2);
    camera::Stats wrote{};
    camera::detail::WriteViewStates(views.Array(), 2, kPos, kQuat, 1.0f, wrote);
    views.SetGate(1, 1);  // the engine took it back after our last write

    const camera::Stats after = HandBack();
    CHECK(after.gateRestored == 1);
    CHECK(after.gateNotOurs == 1);   // its own outcome: "already armed" is not "re-armed by us"
    CHECK(views.Gate(0) == 1);
    CHECK(views.Gate(1) == 1);
}

TEST_CASE("the write clamps at MAX_VIEWS and steps over a null slot") {
    HandBack();

    FakeViews views(camera::detail::kGateLedgerMax);
    views.slots[1] = 0;  // the engine yields a null node in a slot it has not filled

    camera::Stats wrote{};
    camera::detail::WriteViewStates(views.Array(), 999, kPos, kQuat, 1.0f, wrote);
    CHECK(wrote.viewCount == kMaxViews);            // clamped, not trusted
    CHECK(wrote.viewsWritten == kMaxViews - 1);     // the null slot was skipped, not written
    CHECK(views.Gate(1) == 1);                      // and its gate was not touched

    const camera::Stats after = HandBack();
    CHECK(after.gateRestored == kMaxViews - 1);
}

TEST_CASE("a gate the ledger had no room for is counted, not silently dropped") {
    HandBack();

    // The engine replaces every view-state object on CAM.ENTER_FREECAM, so a long session can show
    // the write point more distinct view states than the ledger holds. That is a gate nobody can
    // give back, and it must say so rather than read as a clean hand-back.
    const uint32_t generations = camera::detail::kGateLedgerMax / kMaxViews + 1;
    std::vector<FakeViews> rounds;
    camera::Stats wrote{};
    for (uint32_t g = 0; g < generations; ++g) {
        rounds.emplace_back(kMaxViews);
        camera::detail::WriteViewStates(rounds.back().Array(), kMaxViews, kPos, kQuat, 1.0f, wrote);
    }
    CHECK(wrote.gateReasserts == generations * kMaxViews);
    CHECK(wrote.gateUnrecorded == kMaxViews);

    const camera::Stats after = HandBack();
    CHECK(after.gateRestored == camera::detail::kGateLedgerMax);
    for (size_t i = 0; i < kMaxViews; ++i) {
        CHECK(rounds.back().Gate(i) == 0);  // the generation nobody could record stays disarmed
    }
}

// ------------------------------------------------------------------ (b) the transfer

TEST_CASE("the token moves from one owner to the other without ever being unheld") {
    ResetPoseOwnerForTest();
    REQUIRE(ClaimPoseOwner(PoseOwner::StockFreeCam));
    const uint32_t epoch = PoseOwnerEpoch();

    // ARCH-17 S3.3(b): this is the call that gives IReplayCamera::SetPose its first reachable path.
    REQUIRE(TransferPoseOwner(PoseOwner::StockFreeCam, PoseOwner::WritePoint));
    CHECK(PoseOwnerHeld() == PoseOwner::WritePoint);
    CHECK(PoseOwnerTransfers() == 1);
    CHECK(PoseOwnerTransferRefusals() == 0);

    // A rename of the holder is not a new bind, and two images compare epochs to decide whether
    // they share one arbiter.
    CHECK(PoseOwnerEpoch() == epoch);

    // ReleasePose() hands it straight back -- again without dropping the rig bind.
    REQUIRE(TransferPoseOwner(PoseOwner::WritePoint, PoseOwner::StockFreeCam));
    CHECK(PoseOwnerHeld() == PoseOwner::StockFreeCam);
    CHECK(PoseOwnerTransfers() == 2);

    ReleasePoseOwner(PoseOwner::StockFreeCam);
    CHECK(PoseOwnerHeld() == PoseOwner::None);
    ResetPoseOwnerForTest();
}

TEST_CASE("a transfer whose `from` is not the holder is refused by name and changes nothing") {
    ResetPoseOwnerForTest();

    // Nobody holds it. "Move it from StockFreeCam" is false, and inventing a claim here would let a
    // caller acquire the pose through a call that is not a claim.
    CHECK_FALSE(TransferPoseOwner(PoseOwner::StockFreeCam, PoseOwner::WritePoint));
    CHECK(PoseOwnerHeld() == PoseOwner::None);
    CHECK(PoseOwnerTransferRefusals() == 1);
    CHECK(std::string(LastPoseOwnerConflict()) ==
          "pose-owner-transfer-refused: StockFreeCam->WritePoint but none holds it (image 0)");

    REQUIRE(ClaimPoseOwner(PoseOwner::WritePoint));
    CHECK_FALSE(TransferPoseOwner(PoseOwner::StockFreeCam, PoseOwner::WritePoint));
    CHECK(PoseOwnerHeld() == PoseOwner::WritePoint);   // never overridden
    CHECK(PoseOwnerTransferRefusals() == 2);
    CHECK(std::string(LastPoseOwnerConflict()).rfind(
              "pose-owner-transfer-refused: StockFreeCam->WritePoint but WritePoint holds it", 0) == 0);

    // Neither end may be a non-owner. Unavailable and None are answers about the arbiter, not
    // owners a token can be parked on, and an argument that is not an owner is not a lost race.
    CHECK_FALSE(TransferPoseOwner(PoseOwner::WritePoint, PoseOwner::None));
    CHECK_FALSE(TransferPoseOwner(PoseOwner::Unavailable, PoseOwner::WritePoint));
    CHECK(PoseOwnerTransferRefusals() == 2);
    CHECK(PoseOwnerHeld() == PoseOwner::WritePoint);

    ReleasePoseOwner(PoseOwner::WritePoint);
    ResetPoseOwnerForTest();
}

TEST_CASE("two claimants race one transfer: exactly one wins and the loser is counted") {
    ResetPoseOwnerForTest();
    REQUIRE(ClaimPoseOwner(PoseOwner::StockFreeCam));

    std::atomic<bool> go{false};
    std::atomic<int>  won{0};
    std::thread racers[2];
    for (int i = 0; i < 2; ++i) {
        racers[i] = std::thread([&go, &won] {
            while (!go.load(std::memory_order_acquire)) {}
            if (TransferPoseOwner(PoseOwner::StockFreeCam, PoseOwner::WritePoint)) {
                won.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : racers) t.join();

    CHECK(won.load() == 1);
    CHECK(PoseOwnerHeld() == PoseOwner::WritePoint);
    CHECK(PoseOwnerTransfers() == 1);
    CHECK(PoseOwnerTransferRefusals() == 1);

    ReleasePoseOwner(PoseOwner::WritePoint);
    ResetPoseOwnerForTest();
}

TEST_CASE("the holder is never observed unheld while a transfer is in flight") {
    ResetPoseOwnerForTest();
    REQUIRE(ClaimPoseOwner(PoseOwner::StockFreeCam));

    // This is the whole difference between one compare-exchange and release-then-claim. The window
    // a release opens is not hypothetical: a third claimant that wins it takes the camera and the
    // caller never learns. An observer that can never see PoseOwner::None is the property that
    // makes the window unrepresentable rather than merely narrow.
    std::atomic<bool> stop{false};
    std::atomic<long> unheld{0};
    std::atomic<long> samples{0};
    std::thread observer([&stop, &unheld, &samples] {
        while (!stop.load(std::memory_order_acquire)) {
            if (PoseOwnerHeld() == PoseOwner::None) unheld.fetch_add(1, std::memory_order_relaxed);
            samples.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const int kRounds = 20000;
    int moved = 0;
    for (int i = 0; i < kRounds; ++i) {
        if (TransferPoseOwner(PoseOwner::StockFreeCam, PoseOwner::WritePoint)) ++moved;
        if (TransferPoseOwner(PoseOwner::WritePoint, PoseOwner::StockFreeCam)) ++moved;
    }
    stop.store(true, std::memory_order_release);
    observer.join();

    CHECK(moved == kRounds * 2);
    CHECK(samples.load() > 0);   // an observer that never ran has not seen anything
    CHECK(unheld.load() == 0);
    CHECK(PoseOwnerHeld() == PoseOwner::StockFreeCam);

    ReleasePoseOwner(PoseOwner::StockFreeCam);
    ResetPoseOwnerForTest();
}
