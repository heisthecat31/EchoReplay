// W1-H. Ruling 4c's both-bound control, built as TWO IMAGES.
//
// The version this replaces linked both claimants into one test binary. That is a configuration the
// product does not ship -- camera_control is in EchoReplay.dll / EchoReplay.dll and
// camera::Create's only caller is probe_camera.dll -- so with the arbiter's state held in a
// per-image static, the shipping arrangement had TWO arbiters, both claimants bound, and both
// printed `conflicts 0`. The single-binary test could not fail on that, because in one binary there
// was only ever one static to share.
//
// So the two claimants are loaded here as two separate DLLs, exactly as the process loads them, and
// the load-bearing assertion is that the count reads the SAME through both: one holder, one
// counter, one arbiter per PROCESS. Run against the header-only arbiter this case reports both
// bound and `conflicts 0` -- the audit's finding, reproduced (see ENG-POSEOWNER-R2 for the run).

#include <doctest.h>

#include <windows.h>

#include <string>

#include "camera/camera_control.h"
#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_poseowner.h"

using replay::camera::PoseOwner;
namespace camera = replay::camera;
namespace camera_control = replay::camera_control;

namespace {

// One export table, two images: poseowner_image.cpp is compiled into both, so a name that resolves
// through one resolves through the other or the load is a hard failure rather than a skipped case.
struct ClaimantImage {
    HMODULE module = nullptr;

    const char* (*Which)()            = nullptr;
    int         (*Claim)()            = nullptr;
    void        (*Release)()          = nullptr;
    int         (*Held)()             = nullptr;
    unsigned long long (*Conflicts)() = nullptr;
    unsigned long long (*ReleaseRefusals)() = nullptr;
    unsigned    (*Epoch)()            = nullptr;
    unsigned    (*Key)()              = nullptr;
    const char* (*Arbiter)()          = nullptr;
    const char* (*LastConflict)()     = nullptr;
    void        (*Reset)()            = nullptr;
    int         (*Transfer)(int, int) = nullptr;
    unsigned long long (*Transfers)() = nullptr;
    unsigned long long (*TransferRefusals)() = nullptr;
    int         (*RealCreate)()       = nullptr;
    const char* (*RealWhy)()          = nullptr;

    PoseOwner HeldOwner() const { return static_cast<PoseOwner>(Held()); }

    void Unload() {
        if (module) FreeLibrary(module);
        module = nullptr;
    }
};

std::wstring SiblingOfThisExe(const wchar_t* leaf) {
    wchar_t self[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    REQUIRE(n > 0);
    REQUIRE(n < MAX_PATH);
    std::wstring path(self, n);
    const size_t slash = path.find_last_of(L"\\/");
    REQUIRE(slash != std::wstring::npos);
    path.resize(slash + 1);
    return path + leaf;
}

template <typename Fn>
void Bind(HMODULE module, const char* name, Fn* out) {
    FARPROC proc = GetProcAddress(module, name);
    INFO("export ", name);
    REQUIRE(proc != nullptr);
    *out = reinterpret_cast<Fn>(proc);
}

ClaimantImage Load(const wchar_t* leaf) {
    ClaimantImage image;
    const std::wstring path = SiblingOfThisExe(leaf);
    image.module = LoadLibraryW(path.c_str());
    INFO("LoadLibraryW gle ", GetLastError());
    REQUIRE(image.module != nullptr);

    Bind(image.module, "PoseImage_Which", &image.Which);
    Bind(image.module, "PoseImage_Claim", &image.Claim);
    Bind(image.module, "PoseImage_Release", &image.Release);
    Bind(image.module, "PoseImage_Held", &image.Held);
    Bind(image.module, "PoseImage_Conflicts", &image.Conflicts);
    Bind(image.module, "PoseImage_ReleaseRefusals", &image.ReleaseRefusals);
    Bind(image.module, "PoseImage_Epoch", &image.Epoch);
    Bind(image.module, "PoseImage_Key", &image.Key);
    Bind(image.module, "PoseImage_Arbiter", &image.Arbiter);
    Bind(image.module, "PoseImage_LastConflict", &image.LastConflict);
    Bind(image.module, "PoseImage_Reset", &image.Reset);
    Bind(image.module, "PoseImage_Transfer", &image.Transfer);
    Bind(image.module, "PoseImage_Transfers", &image.Transfers);
    Bind(image.module, "PoseImage_TransferRefusals", &image.TransferRefusals);
    Bind(image.module, "PoseImage_RealCreate", &image.RealCreate);
    Bind(image.module, "PoseImage_RealWhy", &image.RealWhy);
    return image;
}

bool SaysCreated(const char* arbiter) { return std::string(arbiter).rfind("created", 0) == 0; }

}  // namespace

TEST_CASE("two images, one arbiter: the second claimant is refused and both images count it") {
    ClaimantImage wp  = Load(L"poseowner_image_wp.dll");
    ClaimantImage ctl = Load(L"poseowner_image_ctl.dll");

    CHECK(std::string(wp.Which()) == "WritePoint");
    CHECK(std::string(ctl.Which()) == "StockFreeCam");

    // Two images, not two names for one. The keys are handed out by the shared section itself, so
    // distinct non-zero keys mean two mappings; and exactly one participant created the section
    // while the rest opened it, which is what "one arbiter per process" looks like from inside.
    CHECK(wp.Key() != 0);
    CHECK(ctl.Key() != 0);
    CHECK(wp.Key() != ctl.Key());
    CHECK(wp.Key() != replay::camera::PoseOwnerImageKey());
    CHECK(ctl.Key() != replay::camera::PoseOwnerImageKey());

    const int creators = static_cast<int>(SaysCreated(wp.Arbiter())) +
                         static_cast<int>(SaysCreated(ctl.Arbiter())) +
                         static_cast<int>(SaysCreated(replay::camera::PoseOwnerArbiter()));
    CHECK(creators == 1);

    wp.Reset();
    CHECK(wp.HeldOwner() == PoseOwner::None);
    CHECK(ctl.HeldOwner() == PoseOwner::None);
    CHECK(replay::camera::PoseOwnerHeld() == PoseOwner::None);

    // (i) the first image binds.
    REQUIRE(wp.Claim() == 1);
    CHECK(wp.HeldOwner() == PoseOwner::WritePoint);
    CHECK(ctl.HeldOwner() == PoseOwner::WritePoint);          // read through the OTHER image
    CHECK(replay::camera::PoseOwnerHeld() == PoseOwner::WritePoint);
    CHECK(wp.Epoch() == 1);
    CHECK(ctl.Epoch() == 1);

    // (ii) the second image is REFUSED, and the refusal names the holder it could not have known
    // about if it were reading its own arbiter.
    CHECK(ctl.Claim() == 0);
    CHECK(ctl.HeldOwner() == PoseOwner::WritePoint);
    CHECK(std::string(ctl.LastConflict()) == "pose-owner-conflict: WritePoint vs StockFreeCam");

    // (iii) THE ASSERTION THAT PROVES ONE OBJECT. With a per-image arbiter the refusal above never
    // happens and every one of these reads 0.
    CHECK(ctl.Conflicts() == 1);
    CHECK(wp.Conflicts() == 1);
    CHECK(replay::camera::PoseOwnerConflicts() == 1);
    CHECK(std::string(wp.LastConflict()) == "pose-owner-conflict: WritePoint vs StockFreeCam");

    // The shipping call site, driven from inside its own image: camera_control::Create refuses and
    // publishes the conflict rather than the rig-shaped refusal a netGame-less call would give.
    CHECK(ctl.RealCreate() == 0);
    CHECK(std::string(ctl.RealWhy()) == "pose-owner-conflict: WritePoint vs StockFreeCam");
    CHECK(ctl.HeldOwner() == PoseOwner::WritePoint);
    CHECK(ctl.Conflicts() == 2);

    // Release is keyed on the claimant. camera_module calls camera_control::Release() from both
    // Init and Shutdown unconditionally, so an unkeyed release in the control's image would hand
    // the write point's pose away from another image entirely.
    ctl.Release();
    CHECK(wp.HeldOwner() == PoseOwner::WritePoint);
    CHECK(replay::camera::PoseOwnerHeld() == PoseOwner::WritePoint);

    // (iv) the holder vacates and the other image takes the pose.
    wp.Release();
    CHECK(ctl.HeldOwner() == PoseOwner::None);
    REQUIRE(ctl.Claim() == 1);
    CHECK(wp.HeldOwner() == PoseOwner::StockFreeCam);
    CHECK(wp.Epoch() == 2);

    // And the exclusion holds in the other direction, through the write point's own entry point.
    CHECK(wp.RealCreate() == 0);
    CHECK(std::string(wp.RealWhy()) == "pose-owner-conflict");
    CHECK(std::string(ctl.LastConflict()) == "pose-owner-conflict: StockFreeCam vs WritePoint");
    CHECK(wp.Conflicts() == 3);

    // An image that goes away must not take the pose with it: a probe DLL that is unloaded -- or
    // that dies and is unloaded by its host -- would otherwise wedge the token for the rest of the
    // process, and nothing could ever bind again.
    ctl.Unload();
    CHECK(replay::camera::PoseOwnerHeld() == PoseOwner::None);
    CHECK(wp.HeldOwner() == PoseOwner::None);
    REQUIRE(wp.Claim() == 1);
    CHECK(replay::camera::PoseOwnerHeld() == PoseOwner::WritePoint);

    wp.Unload();
    CHECK(replay::camera::PoseOwnerHeld() == PoseOwner::None);

    replay::camera::ResetPoseOwnerForTest();
}

TEST_CASE("the arbiter names itself, so a status line cannot report a private one as shared") {
    const std::string arbiter = replay::camera::PoseOwnerArbiter();
    CHECK_FALSE(arbiter.empty());
    const bool created = arbiter.rfind("created#", 0) == 0;
    const bool opened  = arbiter.rfind("opened#", 0) == 0;
    const bool named   = created || opened;
    CHECK(named);
}

TEST_CASE("every owner has a name, including the one that means the arbiter is missing") {
    CHECK(std::string(replay::camera::PoseOwnerName(PoseOwner::None)) == "none");
    CHECK(std::string(replay::camera::PoseOwnerName(PoseOwner::StockFreeCam)) == "StockFreeCam");
    CHECK(std::string(replay::camera::PoseOwnerName(PoseOwner::WritePoint)) == "WritePoint");
    CHECK(std::string(replay::camera::PoseOwnerName(PoseOwner::Unavailable)) == "unavailable");

    // Unavailable is a state, never a claimant: an arbiter that could not be established must
    // refuse every claim rather than hand the pose to the first caller.
    CHECK_FALSE(replay::camera::ClaimPoseOwner(PoseOwner::Unavailable));
    CHECK_FALSE(replay::camera::ClaimPoseOwner(PoseOwner::None));
    CHECK(replay::camera::PoseOwnerHeld() == PoseOwner::None);
}

// ★ AUDIT_I finding 1. The pair above is different-owner, so it passes whether the token is keyed
// on the image or only on the owner -- it cannot fail on the half of E3 this packet exists to
// close. Two images claiming the SAME owner is the case that can: keyed on the owner alone, the
// second image is read as a re-claim, so BOTH bind while `conflicts 0`, `epoch 1` and
// `lastConflict none` all print healthy; and then a never-holder frees the holder's token and the
// write point takes a pose out from under a still-bound camera_control, whose first SetPose clears
// viewstate+0x9C and stops the stock free camera responding to input.
//
// poseowner_image_ctl2 is the same source and the same owner as poseowner_image_ctl: one
// difference, the image it lives in.
TEST_CASE("two images, the SAME owner: the second is refused and cannot free the first's token") {
    ClaimantImage ctl  = Load(L"poseowner_image_ctl.dll");
    ClaimantImage ctl2 = Load(L"poseowner_image_ctl2.dll");
    ClaimantImage wp   = Load(L"poseowner_image_wp.dll");

    REQUIRE(std::string(ctl.Which()) == "StockFreeCam");
    REQUIRE(std::string(ctl2.Which()) == "StockFreeCam");
    CHECK(ctl.Key() != ctl2.Key());
    CHECK(ctl2.Key() != 0);

    const int creators = static_cast<int>(SaysCreated(ctl.Arbiter())) +
                         static_cast<int>(SaysCreated(ctl2.Arbiter())) +
                         static_cast<int>(SaysCreated(wp.Arbiter())) +
                         static_cast<int>(SaysCreated(camera::PoseOwnerArbiter()));
    CHECK(creators == 1);

    ctl.Reset();
    REQUIRE(ctl.Claim() == 1);
    CHECK(ctl2.HeldOwner() == PoseOwner::StockFreeCam);

    // (i) the second IMAGE of the same owner is a conflict, not a re-claim.
    CHECK(ctl2.Claim() == 0);
    CHECK(ctl.HeldOwner() == PoseOwner::StockFreeCam);
    CHECK(ctl.Epoch() == 1);                       // one bind happened, not two
    CHECK(ctl.Conflicts() == 1);
    CHECK(ctl2.Conflicts() == 1);
    CHECK(camera::PoseOwnerConflicts() == 1);
    // Same owner on both sides can only be two images: a re-claim from the holder never gets here.
    CHECK(std::string(ctl2.LastConflict()) == "pose-owner-conflict: StockFreeCam vs StockFreeCam");

    // (ii) a release from an image that never held the token is REFUSED and counted, not a silent
    // success. camera_module::Init and ::Shutdown call camera_control::Release() unconditionally.
    ctl2.Release();
    CHECK(ctl.HeldOwner() == PoseOwner::StockFreeCam);
    CHECK(ctl.ReleaseRefusals() == 1);
    CHECK(ctl2.ReleaseRefusals() == 1);
    CHECK(camera::PoseOwnerReleaseRefusals() == 1);
    CHECK(std::string(ctl.LastConflict()) ==
          "pose-owner-release-refused: StockFreeCam is not the holder (StockFreeCam, image " +
              std::to_string(ctl.Key()) + ")");

    // (iii) the harm itself: the write point must still be locked out. On the owner-keyed version
    // this claim SUCCEEDED, under a camera_control that was still bound.
    CHECK(wp.Claim() == 0);
    CHECK(wp.HeldOwner() == PoseOwner::StockFreeCam);
    CHECK(wp.Conflicts() == 2);
    CHECK(std::string(wp.LastConflict()) == "pose-owner-conflict: StockFreeCam vs WritePoint");

    // (iv) the holder still releases its own token, and the loser may then bind in its own right.
    ctl.Release();
    CHECK(ctl2.HeldOwner() == PoseOwner::None);
    CHECK(ctl.ReleaseRefusals() == 1);             // the holder's release is not a refusal
    REQUIRE(ctl2.Claim() == 1);
    CHECK(ctl.HeldOwner() == PoseOwner::StockFreeCam);
    CHECK(ctl2.Epoch() == 2);

    // And it takes the pose with it when it goes, keyed on ITS key rather than on the owner.
    ctl2.Unload();
    CHECK(ctl.HeldOwner() == PoseOwner::None);

    ctl.Unload();
    wp.Unload();
    camera::ResetPoseOwnerForTest();
}

// ★ AUDIT_I finding 2. "If the section cannot be established the claim is REFUSED... there is NO
// fallback to a per-image static" is load-bearing -- a fallback would silently reinstate E3 -- and
// until ForcePoseOwnerUnavailableForTest existed the arm was unreachable, so a mutant that put the
// static back lived in a branch no test entered. The seam fails only the section call; everything
// downstream of it is the shipping path.
TEST_CASE("the arbiter cannot be established: every claim refuses and nothing falls back") {
    camera::ResetPoseOwnerForTest();
    REQUIRE(std::string(camera::PoseOwnerArbiter()) != "unavailable");

    camera::ForcePoseOwnerUnavailableForTest(true);

    CHECK(camera::PoseOwnerHeld() == PoseOwner::Unavailable);
    CHECK(std::string(camera::PoseOwnerArbiter()) == "unavailable");
    CHECK(std::string(camera::LastPoseOwnerConflict())
              .rfind("pose-owner-unavailable: CreateFileMappingW", 0) == 0);
    CHECK(camera::PoseOwnerImageKey() == 0);

    // Refused, and STILL refused on the second ask: a per-image fallback would hand the pose to the
    // first caller and then read back as a healthy holder.
    CHECK_FALSE(camera::ClaimPoseOwner(PoseOwner::WritePoint));
    CHECK(camera::PoseOwnerHeld() == PoseOwner::Unavailable);
    CHECK_FALSE(camera::ClaimPoseOwner(PoseOwner::StockFreeCam));
    CHECK_FALSE(camera::ClaimPoseOwner(PoseOwner::WritePoint));
    CHECK(camera::PoseOwnerHeld() == PoseOwner::Unavailable);

    // An arbiter that does not exist counts nothing and cannot be released -- Unavailable must not
    // read as a clean zero on a status line, which is why Held is the field that says so.
    CHECK(camera::PoseOwnerConflicts() == 0);
    CHECK(camera::PoseOwnerEpoch() == 0);
    camera::ReleasePoseOwner(PoseOwner::WritePoint);
    CHECK(camera::PoseOwnerReleaseRefusals() == 0);

    // Both shipping call sites refuse through it, each in its own vocabulary.
    CHECK(camera_control::Create(nullptr) == nullptr);
    CHECK(std::string(camera_control::Diagnostic()).rfind("pose-owner-unavailable", 0) == 0);
    camera_control::Release();

    CHECK(camera::Create(nullptr) == nullptr);
    camera::Stats stats{};
    camera::GetStats(&stats);
    CHECK(stats.refusal == camera::Refusal::PoseOwnerConflict);

    camera::ForcePoseOwnerUnavailableForTest(false);

    CHECK(camera::PoseOwnerHeld() == PoseOwner::None);
    CHECK(camera::PoseOwnerImageKey() != 0);
    const std::string arbiter = camera::PoseOwnerArbiter();
    CHECK((arbiter.rfind("created#", 0) == 0 || arbiter.rfind("opened#", 0) == 0));
    REQUIRE(camera::ClaimPoseOwner(PoseOwner::WritePoint));
    camera::ReleasePoseOwner(PoseOwner::WritePoint);
    CHECK(camera::PoseOwnerHeld() == PoseOwner::None);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("a transfer is keyed on the holding image, and one arbiter counts it for both") {
    ClaimantImage wp  = Load(L"poseowner_image_wp.dll");
    ClaimantImage ctl = Load(L"poseowner_image_ctl.dll");
    ctl.Reset();

    REQUIRE(ctl.Claim() == 1);
    CHECK(ctl.HeldOwner() == PoseOwner::StockFreeCam);

    // ARCH-17 S3.3(b) moves the pose from the stock free camera to the write point. In the shipping
    // arrangement both claimants are compiled into EchoReplay.dll, so that transfer is one image
    // moving its own token. Asked from a DIFFERENT image it must be REFUSED for the reason an
    // unkeyed release was: probe_camera.dll carries its own copy of the write point, and a transfer
    // that only checked the owner would let it take a pose it never claimed.
    CHECK(wp.Transfer(static_cast<int>(PoseOwner::StockFreeCam),
                      static_cast<int>(PoseOwner::WritePoint)) == 0);
    CHECK(ctl.HeldOwner() == PoseOwner::StockFreeCam);
    CHECK(wp.TransferRefusals() == 1);
    CHECK(ctl.TransferRefusals() == 1);   // one arbiter: both images read the same count
    CHECK(std::string(ctl.LastConflict()).rfind("pose-owner-transfer-refused: ", 0) == 0);

    // The image that holds it moves it, and the token stays THAT image's: the write point image
    // still cannot release a WritePoint token it does not hold.
    CHECK(ctl.Transfer(static_cast<int>(PoseOwner::StockFreeCam),
                       static_cast<int>(PoseOwner::WritePoint)) == 1);
    CHECK(wp.HeldOwner() == PoseOwner::WritePoint);
    CHECK(wp.Transfers() == 1);
    CHECK(ctl.Transfers() == 1);

    const unsigned long long refusedBefore = ctl.ReleaseRefusals();
    wp.Release();
    CHECK(ctl.HeldOwner() == PoseOwner::WritePoint);
    CHECK(ctl.ReleaseRefusals() == refusedBefore + 1);

    ctl.Reset();
    wp.Unload();
    ctl.Unload();
}
