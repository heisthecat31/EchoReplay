// W1-H's offline check. Three things, and the first is the one the audit asked for.
//
//  1. ★ EVERY PROBE CRITERION HAS A PASSING INPUT AND A FAILING INPUT, both constructed here.
//     The audit rejected an assertion -- `Stats::gateReasserts == 0` after the first frame --
//     that could never pass, because the counter is cumulative and every freshly constructed
//     view-state has +0x9C == 1. Nothing in the engine was wrong; the criterion was. A criterion
//     whose two outcomes have both been constructed cannot carry that defect, and this file
//     constructs them for all six of the probe's arithmetic checks, plus a case that the rejected
//     assertion is unsatisfiable for every view count the engine can produce.
//
//  2. A known-answer control: every number and byte string in camera_writepoint_facts.h re-derived
//     from the shipping echovr.exe with a PE reader that shares no code with the module under test.
//     If the two agreed only because they shared a reader, this would prove nothing.
//
//  3. The reporting proof: the probe still files a verdict when its instance claim is refused,
//     driven against the real refusal in a real second process -- and a mutation that files the
//     OLD way goes silent under the same condition, so the control has been shown able to disagree.
//
// It reads echovr.exe as a file and never launches it, so it runs in T0 with the other offline
// checks.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <doctest.h>

#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"
#include "generated/facts_generated.h"

#include "criteria.h"
#include "probe_teamgate_outcome.h"
#include "../local_inputs.h"

namespace {

namespace cf = replay::camera_facts;
namespace crit = replay::camera_criteria;


// A minimal PE-on-disk reader, independent of src/engine/camera_writepoint.cpp, which walks the
// MAPPED image through GetModuleHandleW. Two readers, one answer.
class PeFile {
public:
    bool Load(const char* path) {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
            return false;
        }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            fclose(f);
            return false;
        }
        bytes_.resize(static_cast<size_t>(size));
        const size_t got = fread(bytes_.data(), 1, bytes_.size(), f);
        fclose(f);
        if (got != bytes_.size()) {
            return false;
        }
        return Parse();
    }

    uint64_t ImageBase() const { return imageBase_; }
    size_t   Size() const { return bytes_.size(); }

    const uint8_t* At(uint64_t va, size_t len) const {
        for (const Section& s : sections_) {
            const uint64_t start = imageBase_ + s.virtualAddress;
            if (va >= start && va + len <= start + s.rawSize) {
                return bytes_.data() + s.rawPointer + (va - start);
            }
        }
        return nullptr;
    }

    uint64_t Qword(uint64_t va) const {
        const uint8_t* p = At(va, 8);
        return p != nullptr ? *reinterpret_cast<const uint64_t*>(p) : 0;
    }

private:
    struct Section {
        uint64_t virtualAddress;
        uint64_t virtualSize;
        uint64_t rawPointer;
        uint64_t rawSize;
    };

    bool Parse() {
        if (bytes_.size() < 0x40) {
            return false;
        }
        const uint32_t peOff = *reinterpret_cast<const uint32_t*>(bytes_.data() + 0x3C);
        if (peOff + sizeof(IMAGE_NT_HEADERS64) > bytes_.size() ||
            memcmp(bytes_.data() + peOff, "PE\0\0", 4) != 0) {
            return false;
        }
        const uint8_t* fh = bytes_.data() + peOff + 4;
        const uint16_t sectionCount = *reinterpret_cast<const uint16_t*>(fh + 2);
        const uint16_t optSize = *reinterpret_cast<const uint16_t*>(fh + 16);
        const uint8_t* opt = fh + 20;
        imageBase_ = *reinterpret_cast<const uint64_t*>(opt + 24);
        const uint8_t* sec = opt + optSize;
        for (uint16_t i = 0; i < sectionCount; ++i, sec += 40) {
            Section s{};
            s.virtualSize = *reinterpret_cast<const uint32_t*>(sec + 8);
            s.virtualAddress = *reinterpret_cast<const uint32_t*>(sec + 12);
            s.rawSize = *reinterpret_cast<const uint32_t*>(sec + 16);
            s.rawPointer = *reinterpret_cast<const uint32_t*>(sec + 20);
            if (s.rawPointer + s.rawSize <= bytes_.size()) {
                sections_.push_back(s);
            }
        }
        return !sections_.empty();
    }

    std::vector<uint8_t> bytes_;
    std::vector<Section> sections_;
    uint64_t imageBase_ = 0;
};

const PeFile* Exe() {
    static PeFile pe;
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        const std::string path = replay_test::FindEchoVrExeA();
        ok = !path.empty() && pe.Load(path.c_str());
    }
    return ok ? &pe : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// 1. The criteria, with both outcomes constructed for each.
// ---------------------------------------------------------------------------------------------

TEST_CASE("F5: the rejected assertion could not have passed, for any view count") {
    // The counter increments once per view per frame whenever the gate is found armed, and every
    // freshly constructed view-state has +0x9C == 1 (CAM.VIEWSTATE_CTOR writes
    // `mov dword ptr [rcx+9Ch], 1`). So after the first SetPose it equals the number of views
    // written, which is at least one whenever there is a view at all.
    for (uint32_t views = 1; views <= cf::kMaxViews; ++views) {
        const uint64_t afterFirstWrite = views;   // what the shipped SetPose loop produces
        CHECK_MESSAGE(afterFirstWrite != 0,
                      "gateReasserts == 0 after the first frame is unsatisfiable");
    }
    // And the corrected criterion accepts exactly that reading.
    for (uint32_t views = 1; views <= cf::kMaxViews; ++views) {
        CHECK(crit::GateReassertOutcome(0, views, views, views, true) == crit::Outcome::Ok);
    }
}

TEST_CASE("criterion: gate reasserts must rise once and then stay flat") {
    SUBCASE("passing input: rises by the view count, then does not move") {
        CHECK(crit::GateReassertOutcome(0, 2, 2, 2, true) == crit::Outcome::Ok);
        CHECK(crit::GateReassertOutcome(0, 1, 1, 8, true) == crit::Outcome::Ok);
    }
    SUBCASE("failing input: the counter never moved, so the instrument reads zero") {
        // This is the case the rejected assertion would have called a PASS.
        CHECK(crit::GateReassertOutcome(0, 0, 0, 2, true) == crit::Outcome::Failed);
    }
    SUBCASE("failing input: something re-armed the gate during the arm") {
        CHECK(crit::GateReassertOutcome(0, 2, 120, 2, true) == crit::Outcome::Failed);
    }
    SUBCASE("failing input: it rose by more views than exist") {
        CHECK(crit::GateReassertOutcome(0, 9, 9, 8, true) == crit::Outcome::Failed);
    }
    SUBCASE("undetermined: the arm did not complete, or no view was ever written") {
        CHECK(crit::GateReassertOutcome(0, 2, 2, 2, false) == crit::Outcome::Undetermined);
        CHECK(crit::GateReassertOutcome(0, 0, 0, 0, true) == crit::Outcome::Undetermined);
    }
}

TEST_CASE("criterion: the freecam-flag counter is proved to be an instrument") {
    const uint32_t frames = 120;
    SUBCASE("passing input: arm C moves it once a frame, arm D lets it settle") {
        CHECK(crit::FreeCamInstrumentOutcome(1, 1 + frames - 1, 1 + frames, frames, true, true) ==
              crit::Outcome::Ok);
    }
    SUBCASE("failing input: it never moved, so a flat reading in arm A meant nothing") {
        CHECK(crit::FreeCamInstrumentOutcome(1, 1, 1, frames, true, true) == crit::Outcome::Failed);
    }
    SUBCASE("failing input: it kept climbing through the recovery arm") {
        CHECK(crit::FreeCamInstrumentOutcome(1, 1 + frames, 1 + frames + 60, frames, true, true) ==
              crit::Outcome::Failed);
    }
    SUBCASE("undetermined: an arm did not complete") {
        CHECK(crit::FreeCamInstrumentOutcome(1, 120, 121, frames, false, true) ==
              crit::Outcome::Undetermined);
        CHECK(crit::FreeCamInstrumentOutcome(1, 120, 121, frames, true, false) ==
              crit::Outcome::Undetermined);
    }
}

TEST_CASE("criterion: which arm drove CCameraNode[0], read out of the FOV conversion") {
    // 16:9. The commit computes nodeFovH = 2*atan(tan(fovY/2) * w/h), so recovering the aspect
    // gives back the same number at any FOV.
    const float aspect = 16.0f / 9.0f;
    auto convert = [&](float fovY) {
        return 2.0f * std::atan(std::tan(fovY * 0.5f) * aspect);
    };
    const float fovA = 0.95993108f;
    const float fovB = 1.57079633f;
    const float recoveredA = crit::RecoverAspect(convert(fovA), fovA);
    const float recoveredB = crit::RecoverAspect(convert(fovB), fovB);

    SUBCASE("passing input: the view-state arm recovers one aspect at both FOVs") {
        CHECK(crit::ViewStateArmOutcome(recoveredA, recoveredB) == crit::Outcome::Ok);
        CHECK(recoveredA == doctest::Approx(aspect).epsilon(0.001));
    }
    SUBCASE("failing input: the CI arm's unconverted FOV recovers 1.0 and cannot agree") {
        // CAM.SPECTATOR_UPDATE writes node+0x98 = ci+0x50 with no conversion at all.
        const float ciA = crit::RecoverAspect(fovA, fovA);
        const float ciB = crit::RecoverAspect(fovB, fovB);
        CHECK(ciA == doctest::Approx(1.0f).epsilon(0.0001));
        // Fed to the view-state criterion, an unconverted pair is undetermined rather than a pass:
        // 1.0 is exactly the square-viewport band, and calling that a pass would be the false
        // all-clear the third outcome exists to prevent.
        CHECK(crit::ViewStateArmOutcome(ciA, ciB) == crit::Outcome::Undetermined);
        // And the CI criterion accepts it, against a real view-state aspect for contrast.
        CHECK(crit::CiArmOutcome(ciB, recoveredA) == crit::Outcome::Ok);
    }
    SUBCASE("failing input: the CI arm came back converted, so ci+0x274 did not take effect") {
        CHECK(crit::CiArmOutcome(recoveredB, recoveredA) == crit::Outcome::Failed);
    }
    SUBCASE("failing input: the two view-state FOVs disagree on the aspect") {
        CHECK(crit::ViewStateArmOutcome(1.777f, 1.333f) == crit::Outcome::Failed);
    }
    SUBCASE("undetermined: a square viewport makes the two arms indistinguishable") {
        const float squareA = crit::RecoverAspect(fovA, fovA);
        CHECK(crit::ViewStateArmOutcome(squareA, squareA) == crit::Outcome::Undetermined);
        CHECK(crit::CiArmOutcome(squareA, squareA) == crit::Outcome::Undetermined);
    }
    SUBCASE("undetermined: an FOV outside the range an angle can take") {
        CHECK(crit::RecoverAspect(0.0f, fovA) < 0.0f);
        CHECK(crit::RecoverAspect(fovA, 0.0f) < 0.0f);
        CHECK(crit::RecoverAspect(4.0f, fovA) < 0.0f);
        CHECK(crit::ViewStateArmOutcome(-1.0f, 1.777f) == crit::Outcome::Undetermined);
    }
}

TEST_CASE("criterion: the baseline known-answer control") {
    const float p0[3] = {0.0f, 400.0f, 0.0f};
    SUBCASE("passing input: the engine's own camera is nowhere near P0") {
        const float engineCam[3] = {3.0f, 1.7f, -12.0f};
        CHECK(crit::BaselineControlOutcome(true, engineCam, p0) == crit::Outcome::Ok);
    }
    SUBCASE("failing input: the camera was already at P0, so every later equality is vacuous") {
        CHECK(crit::BaselineControlOutcome(true, p0, p0) == crit::Outcome::Failed);
        const float nearby[3] = {0.0f, 360.0f, 0.0f};
        CHECK(crit::BaselineControlOutcome(true, nearby, p0) == crit::Outcome::Failed);
    }
    SUBCASE("undetermined: no valid observation was taken during the baseline arm") {
        const float engineCam[3] = {3.0f, 1.7f, -12.0f};
        CHECK(crit::BaselineControlOutcome(false, engineCam, p0) == crit::Outcome::Undetermined);
    }
}

TEST_CASE("the run verdict aggregates FAIL over NOT-RUN over PASS") {
    const crit::Outcome allOk[] = {crit::Outcome::Ok, crit::Outcome::Ok};
    const crit::Outcome oneUnknown[] = {crit::Outcome::Ok, crit::Outcome::Undetermined};
    const crit::Outcome bothKinds[] = {crit::Outcome::Failed, crit::Outcome::Undetermined};
    CHECK(std::string(crit::AggregateVerdict(allOk, 2)) == "PASS");
    CHECK(std::string(crit::AggregateVerdict(oneUnknown, 2)) == "NOT-RUN");
    // ★ An undetermined check must never be able to hide a failing one.
    CHECK(std::string(crit::AggregateVerdict(bothKinds, 2)) == "FAIL");
}

// ---------------------------------------------------------------------------------------------
// 2. The known-answer control.
// ---------------------------------------------------------------------------------------------

TEST_CASE("known-answer control: camera_writepoint_facts.h against the shipping echovr.exe") {
    const PeFile* pe = Exe();
    if (pe == nullptr) {
        MESSAGE("SKIPPED: echovr.exe not found (set ECHOVR_DIR or REPLAY_ECHOVR_EXE)");
        return;
    }
    CHECK(pe->ImageBase() == cf::kImageBase);
    CHECK(pe->ImageBase() == facts::PE_IMAGE_BASE);

    SUBCASE("the four call sites hold their recorded prologues") {
        struct Site {
            const char*    id;
            uint64_t       va;
            const uint8_t* want;
        } sites[] = {
            {"CAM.VIEW_COMMIT", cf::kViewCommitVA, cf::kViewCommitPrologue},
            {"CAM.SETCAMERA_API_IMPL", cf::kSetCameraApiImplVA, cf::kSetCameraApiImplPrologue},
            {"CS.LOOKUP_BY_SYMBOL", facts::CS_LOOKUP_BY_SYMBOL, facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE},
            {"CAM.SETCAMERA_API_STORES",
             cf::kSetCameraApiImplVA + cf::kSetCameraApiStoresOff, cf::kSetCameraApiStores},
        };
        for (const Site& s : sites) {
            const uint8_t* at = pe->At(s.va, 20);
            REQUIRE_MESSAGE(at != nullptr, s.id);
            CHECK_MESSAGE(memcmp(at, s.want, 20) == 0, s.id);
        }
    }

    SUBCASE("both integrator gates read viewstate+0x9C and branch on it") {
        struct Gate {
            const char*    id;
            uint64_t       va;
            const uint8_t* want;
        } gates[] = {
            {"VIEWSTATE.INPUT_GATE_FREE_SITE", cf::kIntegrateFreeVA + cf::kIntegrateFreeGateOff,
             cf::kIntegrateFreeGate},
            {"VIEWSTATE.INPUT_GATE_BASE_SITE", cf::kIntegrateBaseVA + cf::kIntegrateBaseGateOff,
             cf::kIntegrateBaseGate},
        };
        for (const Gate& gate : gates) {
            const uint8_t* at = pe->At(gate.va, 20);
            REQUIRE_MESSAGE(at != nullptr, gate.id);
            CHECK_MESSAGE(memcmp(at, gate.want, 20) == 0, gate.id);
            // 83 /7 ib = cmp dword ptr [reg+disp32], imm8, with 0x9C as the displacement and 0 as
            // the immediate. The instruction's MEANING is what the write point rests on, so it is
            // decoded here rather than only compared as an opaque blob.
            CHECK(at[0] == 0x83);
            CHECK((at[1] & 0x38) == 0x38);                     // /7 -> cmp
            CHECK(*reinterpret_cast<const uint32_t*>(at + 2) == cf::kViewStateInputEnaOff);
            CHECK(at[6] == 0x00);
        }
    }

    SUBCASE("the constructor arms the gate: mov dword ptr [rcx+9Ch], 1") {
        const uint8_t* at = pe->At(0x140712EFFull, 10);
        REQUIRE(at != nullptr);
        const uint8_t want[10] = {0xC7, 0x81, 0x9C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
        CHECK(memcmp(at, want, sizeof want) == 0);
        CHECK(*reinterpret_cast<const uint32_t*>(at + 2) == cf::kViewStateInputEnaOff);
        CHECK(*reinterpret_cast<const uint32_t*>(at + 6) == 1u);
    }

    // ★ The whole instance-selection model, decoded out of CAM.SETCAMERA_API_IMPL's own body rather
    // than taken from the memo. The audit nearly filed the prototype as a blocker on the strength of
    // the three stores alone, which read as if xmm0/xmm1 were incoming vector arguments; the two
    // LOADS are what settle it, and they are asserted here so the ABI cannot drift unnoticed.
    SUBCASE("CAM.SETCAMERA_API_IMPL loads the transform from rdx and indexes the CI itself") {
        const uint8_t* load0 = pe->At(0x140D44D9Aull, 3);   // movups xmm0, xmmword ptr [rdx]
        REQUIRE(load0 != nullptr);
        const uint8_t wantLoad0[3] = {0x0F, 0x10, 0x02};
        CHECK(memcmp(load0, wantLoad0, sizeof wantLoad0) == 0);

        const uint8_t* load1 = pe->At(0x140D44DA1ull, 4);   // movups xmm1, xmmword ptr [rdx+10h]
        REQUIRE(load1 != nullptr);
        const uint8_t wantLoad1[4] = {0x0F, 0x10, 0x4A, 0x10};
        CHECK(memcmp(load1, wantLoad1, sizeof wantLoad1) == 0);
        CHECK(wantLoad1[3] == 16);   // the second half of the 32-byte CTransfQ

        // cmp r8w, [rcx+0FCh]  -- the instance count, as a 16-bit compare
        const uint8_t* count = pe->At(0x140D44D86ull, 8);
        REQUIRE(count != nullptr);
        CHECK(*reinterpret_cast<const uint32_t*>(count + 4) == cf::kCsInstanceCountOff);

        // cmp r8w, [r9+1E8h]  -- the local index
        const uint8_t* local = pe->At(0x140D44D90ull, 8);
        REQUIRE(local != nullptr);
        CHECK(*reinterpret_cast<const uint32_t*>(local + 4) == cf::kCsLocalIndexOff);

        // lea rcx, [rax+rax*4] ; shl rcx, 7  -- index * 5 * 128 = index * 640, the CI stride
        const uint8_t* lea = pe->At(0x140D44DA5ull, 4);
        REQUIRE(lea != nullptr);
        const uint8_t wantLea[4] = {0x48, 0x8D, 0x0C, 0x80};
        CHECK(memcmp(lea, wantLea, sizeof wantLea) == 0);
        const uint8_t* shl = pe->At(0x140D44DA9ull, 4);
        REQUIRE(shl != nullptr);
        CHECK(shl[0] == 0x48);
        CHECK(shl[1] == 0xC1);
        CHECK(shl[2] == 0xE1);
        const uint32_t stride = 5u << shl[3];
        CHECK(stride == cf::kCiStride);

        // add rcx, [r9+100h]  -- the instance array base
        const uint8_t* base = pe->At(0x140D44DADull, 7);
        REQUIRE(base != nullptr);
        CHECK(*reinterpret_cast<const uint32_t*>(base + 3) == cf::kCsInstanceBaseOff);
    }

    SUBCASE("the CGame vtable slots the frame order is read from") {
        // The base is pinned by two slots holding the two functions RunLoop calls as vtbl[1016]
        // and vtbl[1024], not by a guessed origin.
        constexpr uint64_t kVtable       = 0x1416D0FB0ull;  // facts-ok: GAME.VTABLE PC_PRESENT, so gen_facts emits no value
        constexpr uint64_t kClockStepOff = 0x3F8;           // facts-ok: GAME.VTBL_CLOCKSTEP_OFF PC_PRESENT, so gen_facts emits no value
        constexpr uint64_t kRenderOff    = 0x1C0;           // facts-ok: GAME.VTBL_RENDER_OFF PC_PRESENT, so gen_facts emits no value
        CHECK(pe->Qword(kVtable + kClockStepOff) == facts::TICK_UPDATE_CLOCK);
        CHECK(pe->Qword(kVtable + 0x400) == facts::GAME_UPDATE);   // CR15Game::Update(stage)
        CHECK(pe->Qword(kVtable + kRenderOff) == facts::GAME_RENDER_SLOT_FN);
        CHECK(pe->Qword(kVtable + 0x000) == facts::CLI_APPLY);
    }

    SUBCASE("the view count global is a plain reader, so reading it is reading what the engine reads") {
        // sub_14059DE80: 8B 05 <rel32> = mov eax, [rip+rel32] ; C3
        const uint8_t* at = pe->At(0x14059DE80ull, 7);
        REQUIRE(at != nullptr);
        CHECK(at[0] == 0x8B);
        CHECK(at[1] == 0x05);
        const int32_t rel = *reinterpret_cast<const int32_t*>(at + 2);
        CHECK(0x14059DE80ull + 6 + static_cast<int64_t>(rel) == cf::kViewCountGlobalVA);
        CHECK(at[6] == 0xC3);
    }

    // ★ A finding this packet re-derived while reconciling its launch profile, asserted here so it
    // cannot quietly rot. HARNESS 2.1a calls `-level` "the mode lever": an ABSENT -level ORs
    // 0x2000000 into game+0x7AE0 and sets SessionFlags bit 0. That is true where it was measured
    // (`bare-level`, sweep 5) and NOT true for any profile carrying `-windowed`, because the
    // -spectatorstream body -- which is the branch relay's -windowed NOPs its way into -- runs
    // LATER in the same function and ORs 0x2100000, setting bit 25 again.
    SUBCASE("the -level arm and the -spectatorstream arm write the same mode bit") {
        const uint8_t* levelArm = pe->At(0x140116CDFull, 11);
        REQUIRE(levelArm != nullptr);
        const uint8_t wantLevel[11] = {0x48, 0x81, 0x8F, 0xE0, 0x7A, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x02};
        CHECK(memcmp(levelArm, wantLevel, sizeof wantLevel) == 0);
        const uint32_t levelBits = *reinterpret_cast<const uint32_t*>(levelArm + 7);
        CHECK(levelBits == 0x02000000u);

        const uint8_t* streamArm = pe->At(0x140116F4Dull, 11);
        REQUIRE(streamArm != nullptr);
        const uint8_t wantStream[11] = {0x48, 0x81, 0x8F, 0xE0, 0x7A, 0x00,
                                        0x00, 0x00, 0x00, 0x10, 0x02};
        CHECK(memcmp(streamArm, wantStream, sizeof wantStream) == 0);
        const uint32_t streamBits = *reinterpret_cast<const uint32_t*>(streamArm + 7);
        CHECK(streamBits == 0x02100000u);

        // Same destination, and the later write re-sets the bit the earlier one skipped.
        CHECK(memcmp(levelArm + 2, streamArm + 2, 5) == 0);
        CHECK((streamBits & 0x02000000u) != 0);
        CHECK(0x140116F4Dull > 0x140116CDFull);

        // The 6-byte jz taken when -spectatorstream is ABSENT, which is what relay NOPs to
        // implement -windowed. Its presence is what makes the paragraph above reachable.
        const uint8_t* skip = pe->At(0x140116F3Dull, 6);
        REQUIRE(skip != nullptr);
        CHECK(skip[0] == 0x0F);
        CHECK(skip[1] == 0x84);

        // And the lobby-team word the same body forces to 2, which is why -windowed is not a
        // neutral rendering switch: mov word ptr [rdi+8560h], ax with ax = 2.
        const uint8_t* team = pe->At(0x140116F73ull, 7);
        REQUIRE(team != nullptr);
        const uint8_t wantTeam[7] = {0x66, 0x89, 0x87, 0x60, 0x85, 0x00, 0x00};
        CHECK(memcmp(team, wantTeam, sizeof wantTeam) == 0);
        const uint8_t* two = pe->At(0x140116F67ull, 5);
        REQUIRE(two != nullptr);
        CHECK(two[0] == 0xB8);
        CHECK(*reinterpret_cast<const uint32_t*>(two + 1) == 2u);
    }
}

// ---------------------------------------------------------------------------------------------
// 3. The reporting proof.
//
// It reproduces the real condition offline: several probe DLLs in one process, each embedding the
// same bootstrap, the first to load winning coexist::ClaimInstance and every later one refused.
// Two copies of the same DLL under different names reproduce that exactly, because the claim is
// keyed on the resolved log path plus the pid, not on the file name.
//
// Three assertions, and the third is what makes the first two mean anything:
//   the first-loaded copy files             (the bootstrap path still works)
//   the REFUSED copy files                  (the static initialiser is doing its job)
//   the legacy-shaped mutation goes SILENT   (a control that has only ever agreed proves nothing)
// ---------------------------------------------------------------------------------------------

namespace {

std::string BinDir() {
    char self[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, self, sizeof self);
    const std::string s(self);
    return s.substr(0, s.find_last_of(0x5C));
}

bool RunHost(const std::string& root, const std::vector<std::string>& dlls) {
    std::string cmd = std::string(1, 0x22) + BinDir() + "\\camera_probehost.exe" +
                      std::string(1, 0x22) + " " + std::string(1, 0x22) + root +
                      std::string(1, 0x22);
    for (const std::string& d : dlls) {
        cmd += " " + std::string(1, 0x22) + root + "\\" + d + std::string(1, 0x22);
    }
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    // ★ The host exits non-zero if any LoadLibrary failed. Without checking it, "the mutation filed
    // nothing" would pass just as well when the mutation DLL never loaded at all -- the control
    // would be vacuous in exactly the direction that matters.
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

bool Filed(const std::string& root, const std::string& stem) {
    const std::string path = root + "\\" + stem + "\\done.json";
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string ReadDone(const std::string& root, const std::string& stem) {
    const std::string path = root + "\\" + stem + "\\done.json";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) {
        return std::string();
    }
    char buf[8192] = {0};
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return std::string(buf, n);
}

}  // namespace

TEST_CASE("the probe files a verdict even when its instance claim is refused") {
    const std::string bin = BinDir();
    char tempRoot[MAX_PATH] = {0};
    GetTempPathA(sizeof tempRoot, tempRoot);
    char unique[MAX_PATH];
    _snprintf_s(unique, sizeof unique, _TRUNCATE, "%sw1h_reporting_%lu", tempRoot,
                GetCurrentProcessId());
    CreateDirectoryA(unique, nullptr);
    const std::string root(unique);

    // aaa sorts first, so it wins the claim; probe_camera is then the REFUSED one, which is the
    // position it always occupies in a real multi-probe sweep. zzz is the mutation, refused too.
    REQUIRE(CopyFileA((bin + "\\probe_camera.dll").c_str(),
                      (root + "\\probe_aaa_first.dll").c_str(), FALSE) != 0);
    REQUIRE(CopyFileA((bin + "\\probe_camera.dll").c_str(), (root + "\\probe_camera.dll").c_str(),
                      FALSE) != 0);
    REQUIRE(CopyFileA((bin + "\\probe_camera_nofiler.dll").c_str(),
                      (root + "\\probe_zzz_nofiler.dll").c_str(), FALSE) != 0);

    // REQUIRE, not CHECK: a host that did not load all three proves nothing either way.
    REQUIRE_MESSAGE(
        RunHost(root, {"probe_aaa_first.dll", "probe_camera.dll", "probe_zzz_nofiler.dll"}),
        "the host failed to load one of the three DLLs, so the results below are void");

    CHECK(Filed(root, "probe_aaa_first"));
    CHECK_MESSAGE(Filed(root, "probe_camera"),
                  "the refused copy filed nothing: this is exactly sweep 4");
    CHECK_MESSAGE(!Filed(root, "probe_zzz_nofiler"),
                  "the legacy-shaped mutation filed, so this control cannot detect the defect");

    // ★ And the verdict it filed is the truthful one for a host that is not the guarded build:
    // NOT-RUN with every criterion COULD-NOT-DETERMINE, never a PASS and never a FAIL. A probe that
    // reported FAIL here would be accusing an engine it never met.
    const std::string done = ReadDone(root, "probe_camera");
    CHECK_MESSAGE(done.find("\"verdict\":\"NOT-RUN\"") != std::string::npos, done.c_str());
    CHECK_MESSAGE(done.find("\"provisional\"") == std::string::npos,
                  "the real verdict never overwrote the provisional record");
    CHECK(done.find("COULD-NOT-DETERMINE") != std::string::npos);
    CHECK(done.find("onTarget=false") != std::string::npos);
    // ★ And `checked` is 0, because nothing was evaluated. HARNESS 2.3 reads that number as
    // evidence of how much the run established; reporting ten here would claim ten checks were
    // performed against an engine this process never contained.
    CHECK_MESSAGE(done.find("\"checked\":0") != std::string::npos, done.c_str());
    // The document has to be parseable to be a verdict at all: sweep 4 lost one to a writer that
    // truncated mid-string. Cheap structural check, since this test has no JSON parser.
    CHECK(done.find_first_of('{') == 0);
    CHECK(done.find("}") != std::string::npos);
}

// ★ The word probe_teamgate files has to be a word its reader knows. It filed COULD-NOT-DETERMINE,
// which Read-ProbeVerdict rejects and Get-VerdictFinality classifies as `nonverdict`: the record
// carried no verdict at all and the count of questions it could not answer never reached the sweep
// tally. Driven through the host rather than asserted about the source, because the defect is in
// what the writer PRODUCES.
TEST_CASE("probe_teamgate files a verdict word the harness reads, and counts the undetermined") {
    const std::string bin = BinDir();
    char tempRoot[MAX_PATH] = {0};
    GetTempPathA(sizeof tempRoot, tempRoot);
    char unique[MAX_PATH];
    _snprintf_s(unique, sizeof unique, _TRUNCATE, "%sw2e_teamgate_%lu", tempRoot, GetCurrentProcessId());
    CreateDirectoryA(unique, nullptr);
    const std::string root(unique);

    REQUIRE(CopyFileA((bin + "\\probe_teamgate.dll").c_str(),
                      (root + "\\probe_teamgate.dll").c_str(), FALSE) != 0);
    REQUIRE_MESSAGE(RunHost(root, {"probe_teamgate.dll"}),
                    "the host failed to load probe_teamgate.dll, so the record below is void");

    const std::string done = ReadDone(root, "probe_teamgate");
    // The test harness $VerdictWords, spelled here because this test is the thing that would catch
    // a fifth word being introduced. Exactly one must appear.
    int matched = 0;
    for (const char* w : {"\"verdict\":\"PASS\"", "\"verdict\":\"FAIL\"", "\"verdict\":\"NOT-RUN\"",
                          "\"verdict\":\"REFUSED\""}) {
        if (done.find(w) != std::string::npos) ++matched;
    }
    CHECK_MESSAGE(matched == 1, done.substr(0, 200).c_str());
    // This host is not echovr.exe and never attempts the offline-bit lock, so every question but
    // the guard row is could-not-ask. The count is the number the tally reads; a writer that
    // dropped it, or scored an undetermined row as asked, would move it.
    CHECK_MESSAGE(done.find("\"undetermined\":8") != std::string::npos, done.substr(0, 200).c_str());
    CHECK_MESSAGE(done.find("\"checked\":1") != std::string::npos, done.substr(0, 200).c_str());
}

// ★ The branch the offline host cannot reach. This host never attempts the offline-bit lock, so the
// record above is always a FAIL and could not tell a NOT-RUN writer from the COULD-NOT-DETERMINE one.
// The rule itself is drivable because it lives in the shared header, which is the same function the
// DLL files with -- not a second copy of the policy.
TEST_CASE("probe_teamgate: every verdict word it can file is one the test harness reads") {
    namespace probe = replay::probe_teamgate;

    // $VerdictWords, spelled here because this is the check that would catch a fifth word.
    bool allKnown = true;
    for (uint32_t failed = 0; failed <= 2; ++failed) {
        for (uint32_t undetermined = 0; undetermined <= 9; ++undetermined) {
            const std::string w = probe::VerdictWord(failed, undetermined);
            if (w != "PASS" && w != "FAIL" && w != "NOT-RUN" && w != "REFUSED") allKnown = false;
        }
    }
    CHECK(allKnown);

    CHECK(std::string(probe::VerdictWord(1, 0)) == "FAIL");
    CHECK(std::string(probe::VerdictWord(1, 8)) == "FAIL");
    CHECK(std::string(probe::VerdictWord(0, 1)) == "NOT-RUN");
    // Asked nothing: every row could-not-ask, which is the same clause and not a separate one.
    CHECK(std::string(probe::VerdictWord(0, 9)) == "NOT-RUN");
    CHECK(std::string(probe::VerdictWord(0, 0)) == "PASS");
}

// ARCH-11 (6531613), DESIGN S13.1.1 rule 8: the case analysis of
// camera::detail::ResolveSpectatorCs lived here -- W2-E's path, W1-H's subject -- and moved to
// tests/camera/writepoint/test_resolvecs.cpp with W1-H be01256. This is the DELETE half.

// ★ probe_teamgate's spectator-cs row, mapped from the SHIPPING resolve's refusal.
//
// The row used to be `ciFound ? "ok" : "failed"`, which filed a space that is allocated but not yet
// constructed -- the case CheckSpaceTable exists to detect -- as an asked-and-failed read. This
// asserts the mapping is total, that every could-not-ask stays out of "failed", and that the two
// real failures still say "failed". Mutating any could-not-ask row below to "failed" turns the
// first CHECK in that subcase red; mutating "failed" to "undetermined" turns the last two red.
TEST_CASE("probe_teamgate: a could-not-ask spectator-CS resolve is never filed as failed") {
    namespace probe = replay::probe_teamgate;
    using replay::camera::Refusal;

    // Every refusal ResolveSpectatorCs can return, and the site states that precede it. The word is
    // the outcome the row must carry. The claim that this list is COMPLETE is not taken on trust:
    // the last subcase derives the population from Refusal's own naming function and requires every
    // named value to be either here or on kNotFromResolve (AUD-F3H F3H-6).
    struct Row { const char* what; bool reached; probe::CsSite site; Refusal refusal; const char* word; };
    static const Row kRows[] = {
        {"never-asked: no match",        false, probe::CsSite::Verified,   Refusal::None,               "undetermined"},
        {"could-not-ask: site unmapped",  true, probe::CsSite::Unreadable, Refusal::NoNetGame,          "undetermined"},
        {"could-not-ask: site mismatch",  true, probe::CsSite::Mismatch,   Refusal::NoNetGame,          "undetermined"},
        {"could-not-ask: netGame shape",  true, probe::CsSite::Verified,   Refusal::NoNetGame,          "undetermined"},
        {"could-not-ask: level slot uncommitted", true, probe::CsSite::Verified, Refusal::NetGameUnreadable,   "undetermined"},
        {"could-not-ask: level space unreachable", true, probe::CsSite::Verified, Refusal::LevelSpaceUnreadable, "undetermined"},
        {"could-not-ask: no level space", true, probe::CsSite::Verified,   Refusal::NoLevelSpace,       "undetermined"},
        {"could-not-ask: table not ready",true, probe::CsSite::Verified,   Refusal::SpaceTableNotReady, "undetermined"},
        {"asked-fail: lookup answered null", true, probe::CsSite::Verified, Refusal::LookupNull,        "failed"},
        {"asked-fail: both spaces answer",true, probe::CsSite::Verified,   Refusal::SpaceAmbiguous,     "failed"},
        {"asked-fail: one pointer, two slots", true, probe::CsSite::Verified, Refusal::SpaceAliased,    "failed"},
        {"asked-fail: no local instance", true, probe::CsSite::Verified,   Refusal::NoLocalInstance,    "failed"},
        {"asked-pass",                    true, probe::CsSite::Verified,   Refusal::None,               "ok"},
    };

    for (const Row& row : kRows) {
        CAPTURE(row.what);
        const probe::Outcome o = probe::SpectatorCsOutcome(row.reached, row.site, row.refusal);
        CHECK(strcmp(o.word, row.word) == 0);
        // The three words the verdict loop and the test harness recognise, and no fourth.
        const bool known = (strcmp(o.word, "ok") == 0) || (strcmp(o.word, "failed") == 0) ||
                           (strcmp(o.word, "undetermined") == 0);
        CHECK(known);
        CHECK(o.detail != nullptr);
        CHECK(o.detail[0] != '\0');
        // Taught explicitly, not reached through the default: arm. Both classes say "could not
        // ask", so the word alone cannot tell a mapped refusal from an unmapped one -- without
        // this, dropping a case from the switch changes nothing any assertion here can see.
        CHECK(strstr(o.detail, "this probe does not map") == nullptr);
    }

    // AUD-F3H F3H-6: the table above used to CLAIM totality while omitting NetGameUnreadable and
    // LevelSpaceUnreadable, both of which ResolveSpectatorCs returns. A claim of completeness that
    // nothing can falsify is not a claim. The population is taken from RefusalName -- the enum's
    // own naming function, not a second hand-written copy of the enum -- and every named value must
    // be classified: scored here, or listed as one this resolve cannot return.
    SUBCASE("every named Refusal is classified: no enumerator can be appended unnoticed") {
        // Refusals of OTHER paths (the module handle, the game singleton, the site guards, the
        // view states, the pose-owner claim, the CS instance read). ResolveSpectatorCs never
        // returns these; the mapping's default: files them undetermined if one ever arrives.
        static const Refusal kNotFromResolve[] = {
            Refusal::NoModule, Refusal::NoGame, Refusal::PrologueMismatch,
            Refusal::PrologueUnreadable, Refusal::NoViewStates, Refusal::PoseOwnerConflict,
            Refusal::SpectatorCsUnreadable,
        };
        // The boundary, pinned from both sides: 17 values carry a name and the 18th does not, so
        // an appended-and-named enumerator fails here as well as in the classification loop below.
        const unsigned kNamedRefusals = 17u;
        unsigned named = 0;
        while (strcmp(replay::camera::RefusalName(static_cast<Refusal>(named)), "unknown") != 0) {
            ++named;
            REQUIRE(named < 64u);  // a RefusalName that names everything would loop forever
        }
        CHECK(named == kNamedRefusals);
        CHECK(strcmp(replay::camera::RefusalName(static_cast<Refusal>(kNamedRefusals)),
                     "unknown") == 0);
        // The scan is not vacuously short: the last enumerator the header declares is inside it.
        CHECK(static_cast<unsigned>(Refusal::SpectatorCsUnreadable) < named);

        for (unsigned v = 0; v < named; ++v) {
            const Refusal r = static_cast<Refusal>(v);
            CAPTURE(replay::camera::RefusalName(r));
            bool scored = false;
            for (const Row& row : kRows) {
                if (row.refusal == r) scored = true;
            }
            bool unlisted = false;
            for (const Refusal skipped : kNotFromResolve) {
                if (skipped == r) unlisted = true;
            }
            // Classified, and classified once: a value in both lists is a contradiction about
            // whether this resolve can return it.
            CHECK(scored != unlisted);
        }
    }

    SUBCASE("a refusal the mapping has not been taught is undetermined, never a pass") {
        const probe::Outcome o = probe::SpectatorCsOutcome(true, probe::CsSite::Verified,
                                                           Refusal::PoseOwnerConflict);
        CHECK(strcmp(o.word, "undetermined") == 0);
    }

    SUBCASE("the detail separates could-not-ask from never-asked, since the word cannot") {
        const probe::Outcome never = probe::SpectatorCsOutcome(false, probe::CsSite::Verified,
                                                              Refusal::None);
        const probe::Outcome cannot = probe::SpectatorCsOutcome(true, probe::CsSite::Verified,
                                                               Refusal::SpaceTableNotReady);
        CHECK(strcmp(never.word, cannot.word) == 0);
        CHECK(strstr(never.detail, "never asked:") == never.detail);
        CHECK(strstr(cannot.detail, "could not ask:") == cannot.detail);
    }
}
