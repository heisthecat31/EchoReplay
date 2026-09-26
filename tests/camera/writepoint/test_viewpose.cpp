// W1-H. camera::detail::ReadViewStatePose -- the view-state pose read that RULING 4b rests on.
//
// The ruling (DESIGN S8.1.2) is that the stock free camera keeps flying while the transport is
// PAUSED, because CAM.VIEWSTATE_INTEGRATE_FREE advances on wall time. Observing that means reading
// the pose the engine is about to render WHILE THE WRITE POINT IS NOT BOUND -- the stock free
// camera holds the pose owner in exactly that scenario -- so Observe(), which answers only when
// stats_.available is set, cannot be the read. This one can.
//
// Every case here constructs the object graph the read walks, so both outcomes of every check are
// reachable: a game with a view-state that answers, and one whose links are absent, null or
// reserved-but-not-committed. The reserved cases are the ones that matter: with the Readable()
// probe taken out the read does not return a wrong answer, it faults, and a check whose failing
// input has never been constructed is a check nobody has shown able to fail.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <doctest.h>

#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"
#include "fake_image.h"
#include "generated/facts_generated.h"

namespace {

namespace cf = replay::camera_facts;
using replay::camera::ViewPoseRead;
using replay::camera::ViewStatePose;

// CR15Game, its view-state array and one view-state object, laid out at the displacements the read
// uses and nowhere else.
struct FakeGame {
    std::vector<uint8_t> game;
    std::vector<uint8_t> array;
    std::vector<uint8_t> viewState;

    FakeGame()
        : game(cf::kViewStateArrayOff + 0x20, 0),
          array(cf::kMaxViews * sizeof(void*), 0),
          viewState(cf::kViewStateInputEnaOff + 0x20, 0) {
        PointArrayAt(array.data());
        PointViewStateAt(viewState.data());
    }

    uintptr_t Base() { return reinterpret_cast<uintptr_t>(game.data()); }

    void PointArrayAt(const void* p) {
        std::memcpy(game.data() + cf::kViewStateArrayOff, &p, sizeof p);
    }
    void PointViewStateAt(const void* p) { std::memcpy(array.data(), &p, sizeof p); }

    void SetPose(const float pos[3], const float quat[4], float fovY, int32_t gate) {
        std::memcpy(viewState.data() + cf::kViewStateQuatOff, quat, 16);
        std::memcpy(viewState.data() + cf::kViewStatePosOff, pos, 12);
        std::memcpy(viewState.data() + cf::kViewStateFovYOff, &fovY, sizeof fovY);
        std::memcpy(viewState.data() + cf::kViewStateInputEnaOff, &gate, sizeof gate);
    }
};

// Reserved, never committed: the shape of a view-state array whose object has been freed under us.
void* ReservedPage() {
    static void* p = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
    return p;
}

// Field by field, not memcmp: the struct carries a trailing bool and therefore trailing padding,
// and aggregate initialisation does not promise to zero padding. A Blank() that read padding would
// be a check that fails or passes on the compiler's mood.
bool Blank(const ViewStatePose& p) {
    for (float v : p.pos) {
        if (v != 0.0f) return false;
    }
    for (float v : p.quat) {
        if (v != 0.0f) return false;
    }
    return p.fovY == 0.0f && p.integratorArmed == 0 && !p.integratorArmedKnown;
}

bool Has(const char* haystack, const char* needle) {
    return std::strstr(haystack, needle) != nullptr;
}

}  // namespace

TEST_CASE("view-state pose: the offsets are the generated facts, not a second copy") {
    CHECK(cf::kViewStateQuatOff == facts::VIEWSTATE_QUAT_OFF);
    CHECK(cf::kViewStatePosOff == facts::VIEWSTATE_POS_OFF);
    CHECK(cf::kViewStateFovYOff == facts::VIEWSTATE_FOVY_OFF);
    CHECK(cf::kViewStateInputEnaOff == facts::VIEWSTATE_INPUT_ENABLE_OFF);
    CHECK(cf::kViewStateArrayOff == facts::VIEWSTATE_ARRAY_OFF);
}

TEST_CASE("view-state pose: a constructed game answers with what the view-state holds") {
    FakeGame g;
    const float pos[3] = {1.5f, -2.25f, 3.75f};
    const float quat[4] = {0.0f, 0.70710678f, 0.0f, 0.70710678f};
    g.SetPose(pos, quat, 0.96f, 1);

    ViewStatePose out;
    CHECK(replay::camera::detail::ReadViewStatePose(g.Base(), true, &out) == ViewPoseRead::Ok);
    CHECK(out.pos[0] == doctest::Approx(1.5));
    CHECK(out.pos[1] == doctest::Approx(-2.25));
    CHECK(out.pos[2] == doctest::Approx(3.75));
    CHECK(out.quat[1] == doctest::Approx(0.70710678));
    CHECK(out.quat[3] == doctest::Approx(0.70710678));
    CHECK(out.fovY == doctest::Approx(0.96));
    CHECK(out.integratorArmed == 1);

    // The gate is the ruling's subject, so it is read and not assumed: a disarmed one reads back
    // disarmed rather than being folded into "no answer".
    g.SetPose(pos, quat, 0.96f, 0);
    CHECK(replay::camera::detail::ReadViewStatePose(g.Base(), true, &out) == ViewPoseRead::Ok);
    CHECK(out.integratorArmed == 0);
}

TEST_CASE("view-state pose: absent links are NoViewState -- the engine has nothing -- and publish no numbers") {
    ViewStatePose out;

    // AUDIT_L1 finding 2. NoViewState collapses three engine absences into one word, so each
    // subcase asserts the sentence that separates them AND that it is not one of the other two.
    // A constant `why`, or one written at only one of the three returns, fails here.
    SUBCASE("no game") {
        CHECK(replay::camera::detail::ReadViewStatePose(0, true, &out) == ViewPoseRead::NoViewState);
        CHECK(Blank(out));
        CHECK(Has(out.why, "no CR15Game"));
        CHECK_FALSE(Has(out.why, "holds no view-state array"));
        CHECK_FALSE(Has(out.why, "holds no view state 0"));
    }
    SUBCASE("no view-state array") {
        FakeGame g;
        const float pos[3] = {9.0f, 9.0f, 9.0f};
        const float quat[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        g.SetPose(pos, quat, 1.0f, 1);
        g.PointArrayAt(nullptr);
        CHECK(replay::camera::detail::ReadViewStatePose(g.Base(), true, &out) == ViewPoseRead::NoViewState);
        // ★ NoViewState is distinct from zeros only if a refusal writes none of them: a consumer that
        // scored these fields would read the 9.0f pose that is sitting in the object right now.
        CHECK(Blank(out));
        CHECK(Has(out.why, "holds no view-state array"));
        CHECK_FALSE(Has(out.why, "holds no view state 0"));
    }
    SUBCASE("no view-state 0") {
        FakeGame g;
        g.PointViewStateAt(nullptr);
        CHECK(replay::camera::detail::ReadViewStatePose(g.Base(), true, &out) == ViewPoseRead::NoViewState);
        CHECK(Blank(out));
        CHECK(Has(out.why, "holds no view state 0"));
        CHECK_FALSE(Has(out.why, "holds no view-state array"));
    }
}

TEST_CASE("view-state pose: uncommitted memory is Unreadable, never a pose") {
    REQUIRE(ReservedPage() != nullptr);
    ViewStatePose out;

    // AUDIT_L1 finding 1. Deleting the sentence at any ONE of these three producers left both
    // suites green, because the only `why` under assertion was the guard's. Each subcase now
    // names its own link and denies the other two, so a shared or constant sentence fails as well
    // as a missing one.
    SUBCASE("the game object itself") {
        const uintptr_t reserved = reinterpret_cast<uintptr_t>(ReservedPage());
        CHECK(replay::camera::detail::ReadViewStatePose(reserved, true, &out) == ViewPoseRead::Unreadable);
        CHECK(Blank(out));
        CHECK(Has(out.why, "view-state array slot at"));
        CHECK(Has(out.why, "not committed memory"));
        CHECK_FALSE(Has(out.why, "view-state 0 at"));
    }
    SUBCASE("the view-state array") {
        FakeGame g;
        g.PointArrayAt(ReservedPage());
        CHECK(replay::camera::detail::ReadViewStatePose(g.Base(), true, &out) == ViewPoseRead::Unreadable);
        CHECK(Blank(out));
        CHECK(Has(out.why, "view-state array at"));
        CHECK(Has(out.why, "not committed memory"));
        CHECK_FALSE(Has(out.why, "view-state array slot at"));
        CHECK_FALSE(Has(out.why, "view-state 0 at"));
    }
    SUBCASE("view-state 0") {
        FakeGame g;
        g.PointViewStateAt(ReservedPage());
        CHECK(replay::camera::detail::ReadViewStatePose(g.Base(), true, &out) == ViewPoseRead::Unreadable);
        CHECK(Blank(out));
        CHECK(Has(out.why, "view-state 0 at"));
        CHECK(Has(out.why, "not committed memory"));
        CHECK_FALSE(Has(out.why, "view-state array"));
    }
}

// ★★ THE READ'S BUILD GUARD, driven over an image this test fabricates.
//
// AUDIT_K2b finding 1: the only exercise of the production entry asserted Unreadable, and the
// LATER GAME_SINGLETON_RVA probe answers Unreadable in any host that is not echovr.exe -- so the
// whole site table could be replaced by no-ops and the suite stayed 20/314 green. The cases below
// put an image under the guard instead: with the recorded bytes planted the read reaches the Ok
// path and hands back the fabricated view-state's pose, and with ONE byte wrong at either site it
// refuses and names that site. Delete the loop and the mismatch cases answer Ok.
namespace {

// Past GAME_SINGLETON_RVA (0x20A0478), which the entry probes after the sites. Reserved, not
// committed: only the pages a case needs are ever backed.
constexpr SIZE_T kImageReserveBytes = 0x2200000;

// 0x140D6A806 - 0x140D6A350. The subject derives the same address from the same two facts.
constexpr uint64_t kListenerSourceTestVA = facts::CAM_SPECTATOR_UPDATE + 0x4B6ull;

enum class SiteState {
    Correct,   // the bytes facts.tsv recorded -- the only input that can answer Match
    Wrong,     // those bytes with the last one flipped: a drifted build, mapped and comparable
    Unmapped,  // reserved, never committed: the short-image case
};

// A fabricated echovr.exe: the read's two guarded sites, the gate site +0x9C's evidence comes
// from, and a GAME_SINGLETON_RVA slot pointing at a constructed CR15Game.
struct ReadImage {
    replay_test::FakeImage img{kImageReserveBytes};
    FakeGame               game;

    bool Build(SiteState viewCommit, SiteState listener, SiteState gate,
               bool commitSingletonSlot = true) {
        if (!img.ok()) return false;
        if (!Site(cf::kViewCommitVA, cf::kViewCommitPrologue, viewCommit)) return false;
        if (!Site(kListenerSourceTestVA, facts::CAM_LISTENER_SOURCE_TEST, listener)) return false;
        if (!Site(cf::kIntegrateFreeVA + cf::kIntegrateFreeGateOff, cf::kIntegrateFreeGate, gate)) {
            return false;
        }

        const uintptr_t slot = img.base() + facts::GAME_SINGLETON_RVA;
        if (!commitSingletonSlot) return true;  // reserved, never committed: the short-image case
        if (!img.commit(slot, sizeof(uintptr_t))) return false;
        const uintptr_t gamePtr = game.Base();
        std::memcpy(reinterpret_cast<void*>(slot), &gamePtr, sizeof gamePtr);
        return true;
    }

private:
    static constexpr size_t kPrologueLen = 20;

    bool Site(uint64_t va, const uint8_t* want, SiteState state) {
        if (state == SiteState::Unmapped) return true;
        const uintptr_t at = img.at(va);
        if (!img.plant(at, want, kPrologueLen)) return false;
        if (state == SiteState::Wrong) {
            *reinterpret_cast<uint8_t*>(at + kPrologueLen - 1) ^= 0xFFu;
        }
        return true;
    }
};

}  // namespace

TEST_CASE("view-state pose: an image holding the recorded bytes reaches the pose") {
    ReadImage image;
    REQUIRE(image.Build(SiteState::Correct, SiteState::Correct, SiteState::Correct));
    const float pos[3] = {4.5f, 5.5f, 6.5f};
    const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    image.game.SetPose(pos, quat, 1.25f, 1);

    CHECK(replay::camera::detail::CheckViewPoseSites(image.img.slide()).refusal ==
          replay::camera::Refusal::None);

    ViewStatePose out;
    REQUIRE(replay::camera::detail::ReadViewStatePoseAt(image.img.base(), &out) ==
            ViewPoseRead::Ok);
    CHECK(out.pos[0] == doctest::Approx(4.5));
    CHECK(out.pos[2] == doctest::Approx(6.5));
    CHECK(out.quat[3] == doctest::Approx(1.0));
    CHECK(out.fovY == doctest::Approx(1.25));
    CHECK(out.integratorArmedKnown);
    CHECK(out.integratorArmed == 1);
    // Empty exactly when the read did not refuse -- so a `why` that is always populated, or
    // always populated with one constant, cannot pass the three subcases below.
    CHECK(out.why[0] == '\0');
}

TEST_CASE("view-state pose: one drifted byte at either guarded site refuses the whole read") {
    // Two cases, not one: with a single case a matching site could stand in for the other.
    ViewStatePose out;

    SUBCASE("CAM.VIEW_COMMIT") {
        ReadImage image;
        REQUIRE(image.Build(SiteState::Wrong, SiteState::Correct, SiteState::Correct));
        const replay::camera::detail::GuardResult g =
            replay::camera::detail::CheckViewPoseSites(image.img.slide());
        REQUIRE(g.refusal == replay::camera::Refusal::PrologueMismatch);
        CHECK(Has(g.why, "CAM.VIEW_COMMIT"));
        CHECK_FALSE(Has(g.why, "unreadable"));
        CHECK(replay::camera::detail::ReadViewStatePoseAt(image.img.base(), &out) ==
              ViewPoseRead::Unreadable);
        CHECK(Blank(out));
        // VR2-2: the entry carries the guard's sentence out with it, so DRIFT is distinguishable
        // from an unmapped page at the caller and not only inside the guard.
        CHECK(Has(out.why, "prologue-mismatch"));
        CHECK(Has(out.why, "CAM.VIEW_COMMIT"));
        CHECK_FALSE(Has(out.why, "unreadable"));
    }
    SUBCASE("CAM.LISTENER_SOURCE_TEST") {
        ReadImage image;
        REQUIRE(image.Build(SiteState::Correct, SiteState::Wrong, SiteState::Correct));
        const replay::camera::detail::GuardResult g =
            replay::camera::detail::CheckViewPoseSites(image.img.slide());
        REQUIRE(g.refusal == replay::camera::Refusal::PrologueMismatch);
        CHECK(Has(g.why, "CAM.LISTENER_SOURCE_TEST"));
        CHECK_FALSE(Has(g.why, "CAM.VIEW_COMMIT"));
        CHECK(replay::camera::detail::ReadViewStatePoseAt(image.img.base(), &out) ==
              ViewPoseRead::Unreadable);
        CHECK(Blank(out));
        CHECK(Has(out.why, "prologue-mismatch"));
        CHECK(Has(out.why, "CAM.LISTENER_SOURCE_TEST"));
        CHECK_FALSE(Has(out.why, "CAM.VIEW_COMMIT"));
    }
    SUBCASE("a site that is not mapped at all is unreadable, and says so") {
        ReadImage image;
        REQUIRE(image.Build(SiteState::Unmapped, SiteState::Correct, SiteState::Correct));
        const replay::camera::detail::GuardResult g =
            replay::camera::detail::CheckViewPoseSites(image.img.slide());
        REQUIRE(g.refusal == replay::camera::Refusal::PrologueUnreadable);
        CHECK(Has(g.why, "prologue-unreadable"));
        CHECK(Has(g.why, "CAM.VIEW_COMMIT"));
        CHECK_FALSE(Has(g.why, "mismatch"));
        CHECK(replay::camera::detail::ReadViewStatePoseAt(image.img.base(), &out) ==
              ViewPoseRead::Unreadable);
        CHECK(Blank(out));
        // The same word, a different sentence: an unmapped site says so and never says mismatch.
        CHECK(Has(out.why, "prologue-unreadable"));
        CHECK(Has(out.why, "CAM.VIEW_COMMIT"));
        CHECK_FALSE(Has(out.why, "mismatch"));
    }
}

TEST_CASE("view-state pose: the entry's own two refusals name themselves, not the walk below") {
    // AUDIT_L1 finding 1, the other half. ReadViewStatePoseAt refuses twice before it reaches the
    // walk, and neither refusal had a control: the suite could not tell "asked before the image
    // resolved" from "the singleton slot is gone" from anything the guard says.
    ViewStatePose out;

    SUBCASE("no module handle") {
        CHECK(replay::camera::detail::ReadViewStatePoseAt(0, &out) == ViewPoseRead::Unreadable);
        CHECK(Blank(out));
        CHECK(Has(out.why, "no module handle"));
        CHECK_FALSE(Has(out.why, "prologue"));
        CHECK_FALSE(Has(out.why, "view-state"));
    }
    SUBCASE("the game-singleton slot is not committed") {
        ReadImage image;
        // Every guarded site holds the recorded bytes, so the guard MATCHES and the refusal below
        // is the singleton probe's own -- not the guard answering for it.
        REQUIRE(image.Build(SiteState::Correct, SiteState::Correct, SiteState::Correct, false));
        REQUIRE(replay::camera::detail::CheckViewPoseSites(image.img.slide()).refusal ==
                replay::camera::Refusal::None);
        CHECK(replay::camera::detail::ReadViewStatePoseAt(image.img.base(), &out) ==
              ViewPoseRead::Unreadable);
        CHECK(Blank(out));
        CHECK(Has(out.why, "game-singleton slot at"));
        CHECK(Has(out.why, "not committed memory"));
        CHECK_FALSE(Has(out.why, "prologue"));
        CHECK_FALSE(Has(out.why, "view-state"));
    }
}

TEST_CASE("view-state pose: +0x9C is unknown when its own evidence site drifted, and the pose is "
          "still read") {
    // AUDIT_K2b finding 2. VIEWSTATE.INPUT_GATE_FREE_SITE is the sole V-PRIMARY evidence for
    // +0x9C; the pose displacements are carried by CAM.LISTENER_SOURCE_TEST's window. The two are
    // independent, so a drifted gate site must cost the ruling-4b field and NOTHING else -- and it
    // must cost it as "unknown", never as a confident 0 the observer would read as DISARMED.
    const float pos[3] = {7.0f, 8.0f, 9.0f};
    const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ViewStatePose out;

    SUBCASE("the gate site holds bytes this build did not record") {
        ReadImage image;
        REQUIRE(image.Build(SiteState::Correct, SiteState::Correct, SiteState::Wrong));
        image.game.SetPose(pos, quat, 1.25f, 1);  // ARMED in memory, and it must not be believed

        REQUIRE(replay::camera::detail::ReadViewStatePoseAt(image.img.base(), &out) ==
                ViewPoseRead::Ok);
        CHECK_FALSE(out.integratorArmedKnown);
        CHECK(out.integratorArmed == 0);
        CHECK(out.pos[0] == doctest::Approx(7.0));
        CHECK(out.pos[2] == doctest::Approx(9.0));
        CHECK(out.fovY == doctest::Approx(1.25));
    }
    SUBCASE("the gate site is not mapped") {
        ReadImage image;
        REQUIRE(image.Build(SiteState::Correct, SiteState::Correct, SiteState::Unmapped));
        image.game.SetPose(pos, quat, 1.25f, 1);

        REQUIRE(replay::camera::detail::ReadViewStatePoseAt(image.img.base(), &out) ==
                ViewPoseRead::Ok);
        CHECK_FALSE(out.integratorArmedKnown);
        CHECK(out.integratorArmed == 0);
        CHECK(out.pos[1] == doctest::Approx(8.0));
    }
    SUBCASE("and with the gate site intact the same read answers Known") {
        // Without this the two subcases above would pass for a read that never sets the flag.
        ReadImage image;
        REQUIRE(image.Build(SiteState::Correct, SiteState::Correct, SiteState::Correct));
        image.game.SetPose(pos, quat, 1.25f, 1);

        REQUIRE(replay::camera::detail::ReadViewStatePoseAt(image.img.base(), &out) ==
                ViewPoseRead::Ok);
        CHECK(out.integratorArmedKnown);
        CHECK(out.integratorArmed == 1);
        CHECK(replay::camera::detail::IntegratorGateSiteMatches(image.img.slide()));
    }
}

TEST_CASE("view-state pose: the production entry refuses this image rather than reading it") {
    // No echovr.exe here, so CAM.VIEW_COMMIT's prologue is not at its rebased address in the test
    // host: the guarded read must say the question could not be asked. An entry that answered
    // anything else would be reading whatever this binary happens to have at +0x7688.
    ViewStatePose out;
    CHECK(replay::camera::ReadViewStatePose(&out) == ViewPoseRead::Unreadable);
    CHECK(Blank(out));
    CHECK(std::strcmp(replay::camera::ViewPoseReadName(ViewPoseRead::Unreadable), "unreadable") == 0);
    CHECK(std::strcmp(replay::camera::ViewPoseReadName(ViewPoseRead::NotBound), "not-bound") == 0);
    CHECK(std::strcmp(replay::camera::ViewPoseReadName(ViewPoseRead::NoViewState),
                      "no-view-state") == 0);
    CHECK(std::strcmp(replay::camera::ViewPoseReadName(ViewPoseRead::Ok), "ok") == 0);

    // And the entry is the detail function with the OS's answer passed in, not a second copy of
    // the walk: same host, same verdict.
    ViewStatePose viaDetail;
    CHECK(replay::camera::detail::ReadViewStatePoseAt(
              reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), &viaDetail) ==
          ViewPoseRead::Unreadable);
}
