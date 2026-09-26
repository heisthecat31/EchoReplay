// W2-E. Proves IReplayCameraControl is CONSUMABLE, not merely well-formed.
//
// This translation unit uses the seam the way W2-F's panel and W2-G's console will, and it exists
// because "the header compiles" and "another packet can use the header" are different claims. It
// caught one real defect already: kPlayerSelectorMax was `static const`, which is not implicitly
// inline in C++20, so the std::clamp call below -- the natural way a panel bounds an index
// control -- produced an unresolved external at the CONSUMER's link step and nothing at ours.
//
// It runs offline with no engine: Get() returns null outside the game, which is the branch every
// consumer has to handle anyway, and the name/refusal tables are pure.

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "camera/camera_control.h"

using namespace replay;

namespace {

int failures = 0;
int checks   = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// How W2-G's `cam <free|pov N|...>` parser resolves its argument: the engine's vocabulary, one
// table, no second hand-written list.
bool ParseCamCommand(const char* word, int arg, CameraMode* mode, int32_t* num,
                     ControlRefusal* why) {
    if (!camera_control::ModeFromName(word, mode)) {
        *why = ControlRefusal::UnknownModeName;
        return false;
    }
    if (!camera_control::IsSelectableByHuman(*mode)) {
        *why = ControlRefusal::ModeReservedForOracle;
        return false;
    }
    // ★ The odr-use: binds kPlayerSelectorMax to a const int&.
    *num = std::clamp(arg, 0, IReplayCameraControl::kPlayerSelectorMax);
    *why = ControlRefusal::None;
    return true;
}

}  // namespace

int main() {
    // 1. Every engine name round-trips to its engine value, and the values are the ones the
    //    /camera_mode handler matches against.
    const struct { const char* name; CameraMode mode; } kExpect[] = {
        {"pov", CameraMode::Pov},   {"follow", CameraMode::Follow}, {"level", CameraMode::Level},
        {"disc", CameraMode::Disc}, {"side", CameraMode::Side},     {"free", CameraMode::Free},
        {"api", CameraMode::Api},
    };
    for (const auto& e : kExpect) {
        CameraMode m{};
        Check(camera_control::ModeFromName(e.name, &m), e.name);
        Check(m == e.mode, e.name);
        Check(std::strcmp(camera_control::ModeName(e.mode), e.name) == 0, e.name);
    }
    Check(static_cast<int32_t>(CameraMode::Free) == 5, "free is EMode 5");
    Check(static_cast<int32_t>(CameraMode::Api) == 6, "api is EMode 6");

    // 2. A name the engine does not define is refused, not silently mapped to mode 0. The failing
    //    input for check 1 is a table typo; the failing input here is a table that accepts anything.
    CameraMode junk{};
    Check(!camera_control::ModeFromName("freecam", &junk), "unknown name refused");
    Check(!camera_control::ModeFromName("", &junk), "empty name refused");

    // 3. api is reserved for the oracle and no human surface may select it.
    Check(!camera_control::IsSelectableByHuman(CameraMode::Api), "api not human-selectable");
    for (const auto& e : kExpect)
        if (e.mode != CameraMode::Api)
            Check(camera_control::IsSelectableByHuman(e.mode), e.name);

    // 4. W2-G's parse path, both arms.
    CameraMode m{};
    int32_t num = 0;
    ControlRefusal why = ControlRefusal::None;
    Check(ParseCamCommand("free", 0, &m, &num, &why) && m == CameraMode::Free, "cam free");
    Check(ParseCamCommand("follow", 3, &m, &num, &why) && num == 3, "cam follow 3");
    Check(ParseCamCommand("follow", 99, &m, &num, &why) && num == 10, "selector clamped");
    Check(!ParseCamCommand("api", 0, &m, &num, &why) &&
              why == ControlRefusal::ModeReservedForOracle, "cam api refused");
    Check(!ParseCamCommand("nope", 0, &m, &num, &why) && why == ControlRefusal::UnknownModeName,
          "cam nope refused");

    // 5. Every refusal has a distinct, non-empty name -- W2-G prints these, so two refusals that
    //    render identically would be a diagnosis that does not diagnose.
    const ControlRefusal kAll[] = {
        ControlRefusal::None,              ControlRefusal::NotAvailable,
        ControlRefusal::ModeReservedForOracle, ControlRefusal::OracleTrackActive,
        ControlRefusal::NumOutOfRange,     ControlRefusal::UnknownModeName,
        ControlRefusal::PoseOwnerConflict, ControlRefusal::PoseNotOwned,
        ControlRefusal::OracleTrackHolds,  ControlRefusal::PoseUnreadable,
        ControlRefusal::WritePointRefused, ControlRefusal::FacingDegenerate,
        ControlRefusal::PoseNotFinite,     ControlRefusal::LookAtUnresolvable};
    for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i) {
        const char* a = ControlRefusalName(kAll[i]);
        Check(a && *a, "refusal name non-empty");
        for (size_t j = i + 1; j < sizeof(kAll) / sizeof(kAll[0]); ++j)
            Check(std::strcmp(a, ControlRefusalName(kAll[j])) != 0, "refusal names distinct");
    }

    // 6. Outside the game there is no rig, and Get() says so rather than handing back a live-
    //    looking object. This is the branch W2-F renders its "camera unavailable" state from.
    Check(camera_control::Get() == nullptr, "Get() null with no engine");
    Check(camera_control::Create(nullptr) == nullptr, "Create(nullptr) refuses");

    std::printf("GATE-RESULT checked=%d subject=W2-E-camera-control-consumer failures=%d\n",
                checks, failures);
    return failures ? 1 : 0;
}
