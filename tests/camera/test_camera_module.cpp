// The wiring check for W2-E: not whether IReplayCameraControl works, but whether anything on the
// shipping path ever creates it. DIAG-WIRING gap 3 found camera_control::Create with zero
// non-test callers, so camera_control::Get() -- the predicate W2-G's dispatcher (dispatch.cpp:163)
// and W2-F's panel gate on -- was null for the life of every session and every camera command
// was refused NotAvailable.
//
// This file links the real module translation unit through the real modreg and drives it with a
// fake netGame: a game space whose component-system table is laid out the way the engine's
// CTable constructor leaves it, and a spectator CS whose instance array holds one local instance.
// The two engine routines the control calls are replaced through the test seam; everything else
// -- the probe, the table check, the identity tracking, the cross-check between the lookup and
// netGame's rig slot -- is the shipping code. Every case turns red if REPLAY_MODULE in
// camera_module.cpp is deleted, if Tick stops calling Create, or if Create stops validating.

#include "doctest.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "bootstrap/modreg.h"
#include "bootstrap/services.h"
#include "camera/camera_control.h"
#include "camera/camera_module.h"
#include "control/command.h"
#include "engine/camera_writepoint.h"
#include "engine/camera_writepoint_facts.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"

using replay::CameraMode;
using replay::ControlRefusal;
using replay::IReplayCameraControl;
namespace camera_control = replay::camera_control;
namespace camera_module = replay::camera_module;
namespace camera = replay::camera;

namespace {

struct Line {
    replay::LogLevel level;
    std::string module;
    std::string text;
};

class RecordingLog final : public replay::ILog {
public:
    void Write(replay::LogLevel level, const char* module, const char* fmt, ...) override {
        char text[1024];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, args);
        va_end(args);
        lines.push_back(Line{level, module ? module : "", text});
    }
    bool Enabled(replay::LogLevel) const override { return true; }
    void DumpLedger() override {}

    size_t Count(const char* needle) const {
        size_t n = 0;
        for (const Line& l : lines)
            if (l.module == "camera" && l.text.find(needle) != std::string::npos) ++n;
        return n;
    }

    std::vector<Line> lines;
};

class DefaultConfig final : public replay::IConfig {
public:
    bool GetBool(const char*, bool dflt) const override { return dflt; }
    int64_t GetInt(const char*, int64_t dflt) const override { return dflt; }
    double GetDouble(const char*, double dflt) const override { return dflt; }
    const char* GetString(const char*, const char* dflt) const override { return dflt; }
    bool Has(const char*) const override { return false; }
};

// The CTable header the engine's constructor writes at space+0x420 (CS.TABLE_OFF) and the element
// block it points at, as src/bootstrap/tableguard.h reads them.
const uint32_t kCsTableOff = 0x420u;
const uint64_t kEntryStride = 16u;

const uint64_t kMaxEntries = 99;   // L5 run 3's cs-space count, so the recorded rig fits

struct FakeSpace {
    alignas(16) uint8_t bytes[kCsTableOff + 0x40] = {};
    alignas(16) uint8_t elements[kMaxEntries * kEntryStride] = {};
    alignas(16) uint8_t elementsRebuilt[kMaxEntries * kEntryStride] = {};
    uint64_t allocatorObject = 0;

    void Put(uint32_t off, uint64_t v) { memcpy(bytes + kCsTableOff + off, &v, sizeof v); }

    void Construct(const uint8_t* block, uint64_t entries = 4) {
        Put(0x00, reinterpret_cast<uintptr_t>(block));        // base
        Put(0x08, entries * kEntryStride);                    // bytes
        Put(0x10, reinterpret_cast<uintptr_t>(&allocatorObject));
        Put(0x20, entries);                                   // grow
        Put(0x28, entries);                                   // capacity
        Put(0x30, entries);                                   // count
        Put(0x38, 0);                                         // flags
    }

    // SPACE.NAME_SYMBOL_OFF, the CSymbol64 SetCurrentSpawnGameSpace copies into the CR15Game.
    // Nothing in camera_control reads it; it is here because the rig is shared with W1-H and
    // ARCH-12 section 5.3 specifies the whole object, not the part one packet happens to use.
    void SetName(uint64_t symbol) {
        memcpy(bytes + facts::SPACE_NAME_SYMBOL_OFF, &symbol, sizeof symbol);
    }
};

// CR15NetSpectatorCameraCS as the control reads it: an instance count, a local index and an
// instance array of SPECTATOR.CI_STRIDE-byte CIs, whose +0x3C is the mode.
struct FakeRig {
    alignas(16) uint8_t cs[facts::SPECTATOR_CS_LOCAL_INDEX_OFF + 0x10] = {};
    alignas(16) uint8_t instances[2 * replay::camera_facts::kCiStride] = {};

    FakeRig() {
        const uint16_t count = 2, local = 1;
        memcpy(cs + facts::SPECTATOR_CS_INSTANCE_COUNT_OFF, &count, sizeof count);
        memcpy(cs + facts::SPECTATOR_CS_LOCAL_INDEX_OFF, &local, sizeof local);
        void* base = instances;
        memcpy(cs + facts::SPECTATOR_CS_INSTANCE_BASE_OFF, &base, sizeof base);
    }

    uint8_t* LocalCi() { return instances + replay::camera_facts::kCiStride; }
    void SetFreeCam(int32_t v) {
        memcpy(LocalCi() + replay::camera_facts::kCiFreeCamActiveOff, &v, sizeof v);
    }
    void SetInstanceBase(const void* base) {
        memcpy(cs + facts::SPECTATOR_CS_INSTANCE_BASE_OFF, &base, sizeof base);
    }
    int32_t Mode() {
        int32_t m;
        memcpy(&m, LocalCi() + facts::SPECTATOR_CI_MODE_OFF, sizeof m);
        return m;
    }
    void SetLocalIndex(uint16_t local) {
        memcpy(cs + facts::SPECTATOR_CS_LOCAL_INDEX_OFF, &local, sizeof local);
    }
};

// The netGame carries TWO spaces (ARCH-12 section 2): the CncaGameSpace at NETGAME.CS_SPACE_OFF
// (+0x2B08) and the LEVEL space at NETGAME.SPECTATORCAM_CS_OFF (+0x2B10), whose id is a misnomer
// -- it is not the CS. +0x8 is the CR15Game back-pointer (ARCH-10 section 1), and this rig plants
// a CR15Game-shaped object there on purpose: an accessor that regresses to +0x8 reads that
// object's +0x420 as a CTable header, gets Empty, and every "bound" expectation below reddens.
struct FakeNetGame {
    // Long enough for NETGAME.UIFLAGS_PTR_OFF as well: ARCH-23 S14.11's P0a is bit 2 of the word
    // that slot POINTS AT, and a rig too short to hold the slot could not fail the read.
    alignas(16) uint8_t bytes[facts::NETGAME_UIFLAGS_PTR_OFF + 0x10] = {};
    alignas(16) uint8_t gameFromNetGame[facts::GAME_SPAWN_GAMESPACE_SYM_OFF + 0x10] = {};
    uint32_t sessionFlags = 0;

    FakeNetGame() {
        void* game = gameFromNetGame;
        memcpy(bytes + facts::IDENT_NETGAME_GAMESPACE_OFF, &game, sizeof game);
        SetSessionFlagsPtr(&sessionFlags);
    }

    void SetSessionFlagsPtr(const uint32_t* p) {
        memcpy(bytes + facts::NETGAME_UIFLAGS_PTR_OFF, &p, sizeof p);
    }

    void PointAt(const FakeSpace* cs, const FakeSpace* level) {
        void* csSpace = cs ? const_cast<uint8_t*>(cs->bytes) : nullptr;
        void* levelSpace = level ? const_cast<uint8_t*>(level->bytes) : nullptr;
        memcpy(bytes + facts::NETGAME_CS_SPACE_OFF, &csSpace, sizeof csSpace);
        memcpy(bytes + facts::NETGAME_SPECTATORCAM_CS_OFF, &levelSpace, sizeof levelSpace);
    }

    // GAME.SPAWN_GAMESPACE_SYM_OFF: what SetCurrentSpawnGameSpace stored. Information, never a
    // gate (ARCH-12 R2), and read by no code in this packet.
    void SetSpawnSymbol(uint64_t symbol) {
        memcpy(gameFromNetGame + facts::GAME_SPAWN_GAMESPACE_SYM_OFF, &symbol, sizeof symbol);
    }
};

// ARCH-12 section 5.3, and ARCH-10 section 3.4's rule: a rig too short to hold +0x2B10 that still
// passes is a rig that is not reading the field. 0x2B18 is the smallest buffer the product's own
// reads can be satisfied out of.
static_assert(sizeof(FakeNetGame::bytes) >= facts::NETGAME_SPECTATORCAM_CS_OFF + sizeof(void*),
              "the netGame rig must be at least 0x2B18 bytes");
static_assert(sizeof(FakeNetGame::bytes) >= facts::NETGAME_UIFLAGS_PTR_OFF + sizeof(void*),
              "the netGame rig must reach the session-flags slot P0a dereferences");
static_assert(sizeof(FakeNetGame::gameFromNetGame) >= facts::GAME_SPAWN_GAMESPACE_SYM_OFF + 8,
              "the CR15Game rig must be at least 0x8A50 bytes");

// The fake engine: CS.LOOKUP_BY_SYMBOL answers with the rig registered for that space and
// symbol, and CAM.SET_CAMERA_MODE_IMPL writes ci+0x3C the way the real one does, on the LOCAL
// instance, recording what it was handed.
// Every space asked is recorded, not just the last: the bind asks the level space and the
// differential then asks the CS space, so a single "which space" field would report whichever ran
// second and no test could tell the two questions apart.
struct EngineCalls {
    void*    asked[8] = {};
    int      lookupCalls = 0;
    uint64_t lookupSymbol = 0;
    void*    registeredSpace = nullptr;   // the level space: where the spectator CS is registered
    void*    alsoAnswers = nullptr;       // a second space answering too: the ambiguity control
    void*    registeredRig = nullptr;
    void*    setModeCs = nullptr;
    int      setModeMode = -1;
    unsigned setModeNum = 0xFFFF;
    int      setModeCalls = 0;

    bool Asked(const void* space) const {
        const int n = lookupCalls < 8 ? lookupCalls : 8;
        for (int i = 0; i < n; ++i)
            if (asked[i] == space) return true;
        return false;
    }
    void ForgetLookups() {
        lookupCalls = 0;
        for (int i = 0; i < 8; ++i) asked[i] = nullptr;
    }
};
EngineCalls g_engine;

void* FakeLookup(void* space, uint64_t symbol) {
    if (g_engine.lookupCalls < 8) g_engine.asked[g_engine.lookupCalls] = space;
    ++g_engine.lookupCalls;
    g_engine.lookupSymbol = symbol;
    if (symbol != facts::CAM_SPECTATOR_SYMBOL) return nullptr;
    if (space == g_engine.registeredSpace) return g_engine.registeredRig;
    if (space != nullptr && space == g_engine.alsoAnswers) return g_engine.registeredRig;
    return nullptr;
}

// The ENGINE's own selector derivation, transcribed from CAM.SET_CAMERA_MODE_IMPL --
// 0x140D44FD7-0x140D45009, ending in the guarded `mov [rbx+26h], eax`. It lives in the HARNESS and
// never in the product: this is the other arm of p10-cam-9, and a product carrying its own copy
// would be scoring itself. num == 0 is the engine's 0x40000, which collides with num == 10 and is
// why PackSelector refuses selector 0 rather than reproducing it.
int32_t EngineSelectorWord(unsigned num) {
    // num == 0 is the engine writing 4 into the high word and 0 into the low one, which is the
    // same word it computes for num == 10 -- written as the composition rather than as the
    // constant it folds to, so the collision is visible and no bare literal is transcribed.
    const uint32_t lo = (num >= 1u && num <= 5u) ? 1u : 0u;
    const uint32_t hi = (num == 0u) ? 4u : ((num <= 5u) ? (num - 1u) : (num - 6u));
    return static_cast<int32_t>((hi << 16) | lo);
}

// 0 = the engine's rule. 1 = an engine whose two halves are the other way round, which is the
// packed-wrong mutant kept as a permanent case rather than as a one-off run.
int g_engineSelectorSkew = 0;

void FakeSetMode(void* cs, int mode, unsigned short num) {
    g_engine.setModeCs = cs;
    g_engine.setModeMode = mode;
    g_engine.setModeNum = num;
    ++g_engine.setModeCalls;
    uint16_t local;
    memcpy(&local, static_cast<uint8_t*>(cs) + facts::SPECTATOR_CS_LOCAL_INDEX_OFF, sizeof local);
    uint8_t* base;
    memcpy(&base, static_cast<uint8_t*>(cs) + facts::SPECTATOR_CS_INSTANCE_BASE_OFF, sizeof base);
    const int32_t m = mode;
    memcpy(base + local * replay::camera_facts::kCiStride + facts::SPECTATOR_CI_MODE_OFF, &m, sizeof m);
    // SPECTATOR.CI_SELECTOR_PACKED_OFF. The real function writes this on every call, before and
    // independently of any occupancy check -- which is why p10-cam-9 can ask every n whether or
    // not a player sits in that slot (AUDIT_ARCH_19_20 C6's open question 3).
    int32_t packed = EngineSelectorWord(num);
    if (g_engineSelectorSkew == 1) {
        packed = static_cast<int32_t>((static_cast<uint32_t>(packed) >> 16) |
                                      (static_cast<uint32_t>(packed) << 16));
    }
    memcpy(base + local * replay::camera_facts::kCiStride + 0x26, &packed, sizeof packed);
}

const camera_control::RigEngine kFakeEngine = {&FakeLookup, &FakeSetMode};

// modreg::InitAll is once per process, so the file shares one environment and every case drives
// the binding from a known state through Tick().
struct Env {
    RecordingLog log;
    DefaultConfig config;
    replay::ReplayServices services{};
    size_t failures = 0;

    Env() {
        services.log = &log;
        services.config = &config;
        camera_control::SetEngineForTest(&kFakeEngine);
        replay::modreg::InitAll(&services);
        failures = replay::modreg::InitFailures();
    }

    void Tick(void* netGame) {
        replay::TickInfo info{};
        info.frameIndex = 1;
        info.netGame = netGame;
        replay::modreg::TickAll(info);
    }
    void Ticks(void* netGame, int n) {
        for (int i = 0; i < n; ++i) Tick(netGame);
    }
};

Env& TheEnv() {
    static Env env;
    return env;
}

bool Registered(const char* name) {
    for (size_t i = 0; i < replay::modreg::Count(); ++i)
        if (strcmp(replay::modreg::At(i)->name, name) == 0) return true;
    return false;
}

// ARCH-12 section 5.3's shared rig contract, in full. BOTH spaces are populated and DISTINCT:
//
//   +0x2B08 -> space C: constructed, Ready, 99 entries, WITHOUT the spectator symbol. This is the
//              object L5 run 3 recorded as "+0x2B08=0000027118DA0000 Ready/Ready ... cap=99"
//   +0x2B10 -> space L: constructed, Ready, 35 entries, WITH the spectator symbol
//   +0x8    -> a CR15Game whose +0x8A48 carries space L's own name symbol
//
// The two tables differ in CONTENT, not only in count: a rig in which both spaces answer the
// spectator symbol has no must-fail control in it, because the accessor could read either slot and
// still bind.
struct Rig {
    static const uint64_t kLevelName = 0x4C45564C00000101ULL;   // a CSymbol64 stand-in

    FakeSpace csSpace;      // +0x2B08
    FakeSpace levelSpace;   // +0x2B10
    FakeRig rig;
    FakeNetGame netGame;

    Rig() {
        csSpace.Construct(csSpace.elements, 99);
        levelSpace.Construct(levelSpace.elements, 35);
        levelSpace.SetName(kLevelName);
        netGame.SetSpawnSymbol(kLevelName);
        netGame.PointAt(&csSpace, &levelSpace);
        g_engine = EngineCalls();
        g_engine.registeredSpace = levelSpace.bytes;   // only space L answers
        g_engine.registeredRig = rig.cs;
    }
};

}  // namespace

TEST_CASE("the camera module registers, Init succeeds, and nothing is created before a tick") {
    Env& e = TheEnv();
    REQUIRE(Registered("camera"));
    CHECK(e.failures == 0);
    CHECK(camera_control::Get() == nullptr);
    CHECK(e.log.Count("waits for a constructed game space") == 1);
}

TEST_CASE("a tick with a constructed space and a rig creates the control, and a cam command is not refused") {
    Env& e = TheEnv();
    Rig r;

    const size_t boundBefore = e.log.Count("camera control bound");
    e.Tick(r.netGame.bytes);

    IReplayCameraControl* cam = camera_control::Get();
    REQUIRE(cam != nullptr);
    CHECK(cam->Available());
    CHECK(e.log.Count("camera control bound") == boundBefore + 1);
    // The lookup ran on the LEVEL space at +0x2B10, which is where both of the engine's own
    // consumers run it (ARCH-12 section 2.3), and the differential then asked the +0x2B08 space
    // the same question and got nothing.
    CHECK(g_engine.Asked(r.levelSpace.bytes));
    CHECK(g_engine.Asked(r.csSpace.bytes));
    CHECK(g_engine.lookupSymbol == facts::CAM_SPECTATOR_SYMBOL);
    CHECK(std::string(camera_control::Diagnostic()).find("differential CsSpaceClear") !=
          std::string::npos);
    CHECK(camera_control::BoundSpectatorCs() == static_cast<void*>(r.rig.cs));

    // dispatch.cpp:163 refuses NotAvailable exactly when the command needs a camera and Get() is
    // null. Same parse, same spec bit, same accessor: the refusal cannot fire.
    const replay::control::ParseResult parsed = replay::control::Parse(L"cam free");
    REQUIRE(parsed.ok);
    const replay::control::CommandSpec* spec = replay::control::SpecFor(parsed.cmd.id);
    REQUIRE(spec != nullptr);
    REQUIRE((spec->needs & replay::control::kNeedsCamera) != 0);
    CHECK(camera_control::Get() != nullptr);

    // What RunCam then does: the free camera, mode 5, through CAM.SET_CAMERA_MODE_IMPL on the
    // rig the lookup returned.
    CameraMode mode = CameraMode::Pov;
    REQUIRE(camera_control::ModeFromName(parsed.cmd.camMode, &mode));
    REQUIRE(cam->SetMode(mode, 0));
    CHECK(cam->LastRefusal() == ControlRefusal::None);
    CHECK(g_engine.setModeCalls == 1);
    CHECK(g_engine.setModeCs == r.rig.cs);
    CHECK(g_engine.setModeMode == 5);
    CHECK(g_engine.setModeNum == 0);
    CHECK(r.rig.Mode() == 5);
    CameraMode observed = CameraMode::Pov;
    REQUIRE(cam->ObservedMode(&observed));
    CHECK(observed == CameraMode::Free);
    CHECK(cam->RequestedMode() == CameraMode::Free);

    // Same identity, next tick: no re-bind, and the instance stays.
    e.Tick(r.netGame.bytes);
    CHECK(e.log.Count("camera control bound") == boundBefore + 1);
    CHECK(camera_control::Get() == cam);

    e.Tick(nullptr);
    CHECK(camera_control::Get() == nullptr);
    CHECK(e.log.Count("camera control released") >= 1);
}

// ★ ARCH-12 R2. The level space cannot cross-check itself -- the lookup IS the accessor, and the
// old "the answer must equal the +0x2B10 slot" arm compared a component system against a space and
// could only ever fail. What replaces it is a differential across the netGame's two spaces, and
// every arm of it is exercised here, in both directions: the pass and the refusal.
TEST_CASE("the two-space differential: the symbol answers in the level space and nowhere else") {
    Env& e = TheEnv();

    SUBCASE("both spaces answering is ambiguous, and no camera is bound on it") {
        Rig r;
        g_engine.alsoAnswers = r.csSpace.bytes;   // a future build registering the CS in +0x2B08
        const size_t refusedBefore = e.log.Count("camera control refused");
        e.Tick(r.netGame.bytes);
        CHECK(camera_control::Get() == nullptr);
        CHECK(e.log.Count("camera control refused") == refusedBefore + 1);
        const std::string d = camera_control::Diagnostic();
        CHECK(d.find("SpaceAmbiguous") != std::string::npos);
        // Not misfiled as the lookup having failed: it succeeded, twice, which is the problem.
        CHECK(d.find("LookupNull") == std::string::npos);
        e.Tick(nullptr);
    }

    SUBCASE("one pointer in both slots cannot be a differential, so it is refused") {
        Rig r;
        r.netGame.PointAt(&r.levelSpace, &r.levelSpace);
        e.Tick(r.netGame.bytes);
        CHECK(camera_control::Get() == nullptr);
        CHECK(std::string(camera_control::Diagnostic()).find("SpaceAliased") != std::string::npos);
        // Before any lookup at all, which is the order P-B's ResolveSpectatorCs uses.
        CHECK(g_engine.lookupCalls == 0);
        e.Tick(nullptr);
    }

    // AUD-F3H F3H-3. The aliasing arm used to sit AFTER the level-space lookup, so the one case it
    // was written for -- two slots holding a space that does not carry the symbol, which is what a
    // wrong field typically looks like -- came out as LookupNull and pointed the reader at the
    // space instead of at the slot. P-B answers SpaceAliased on the same memory; so must this.
    SUBCASE("aliased onto a space WITHOUT the symbol is still SpaceAliased, not LookupNull") {
        Rig r;
        r.netGame.PointAt(&r.csSpace, &r.csSpace);   // space C: constructed, 99 entries, no symbol
        e.Tick(r.netGame.bytes);
        CHECK(camera_control::Get() == nullptr);
        const std::string d = camera_control::Diagnostic();
        CHECK(d.find("SpaceAliased") != std::string::npos);
        CHECK(d.find("LookupNull") == std::string::npos);
        CHECK(g_engine.lookupCalls == 0);
        e.Tick(nullptr);
    }

    SUBCASE("a null CS space is could-not-ask, and binds the camera anyway") {
        // ARCH-12 R4: +0x2B08 is filled by a different transition, and on L5 run 3 it arrived
        // 1.3 s before +0x2B10 -- so neither order may be assumed and its absence is not a camera
        // refusal. This is the arm the shipped version got wrong in the other direction.
        Rig r;
        r.netGame.PointAt(nullptr, &r.levelSpace);
        e.Tick(r.netGame.bytes);
        REQUIRE(camera_control::Get() != nullptr);
        CHECK(std::string(camera_control::Diagnostic()).find("differential CsSpaceAbsent") !=
              std::string::npos);
        CHECK(g_engine.lookupCalls == 1);   // nothing was asked of a space that is not there
        e.Tick(nullptr);
    }

    SUBCASE("a CS space W1-C cannot judge is not asked, because the lookup would sort it") {
        Rig r;
        r.csSpace.Put(0x38, 1);   // CS.TABLE_FLAGS_OFF bit 0: the engine would sort it in place
        e.Tick(r.netGame.bytes);
        REQUIRE(camera_control::Get() != nullptr);
        CHECK(std::string(camera_control::Diagnostic()).find("differential CsSpaceNotAsked") !=
              std::string::npos);
        CHECK_FALSE(g_engine.Asked(r.csSpace.bytes));
        e.Tick(nullptr);
    }

    SUBCASE("a level space that does not carry the symbol is LookupNull") {
        Rig r;
        g_engine.registeredRig = nullptr;
        e.Tick(r.netGame.bytes);
        CHECK(camera_control::Get() == nullptr);
        CHECK(std::string(camera_control::Diagnostic()).find("LookupNull") != std::string::npos);
        e.Tick(nullptr);
    }
}

// ★ THE MUST-FAIL CONTROL (ARCH-12 section 5.3). The accessor regressed to the +0x2B08 space is
// exactly the recorded L5 run 3 state, and it must red. Driven through Probe rather than through a
// mutated build, by making the two spaces swap slots: whichever slot the product reads, it reads
// the object that is in it, so this fails if and only if the accessor is reading +0x2B08.
// AUD-F3H F3H-2. csSpace is deliberately outside RigIdentity equality (ARCH-12 R4) so that a late
// arrival cannot tear down a working camera -- but that also means `changed` stays false when the
// slot fills, and the differential used to be asked only inside Init. sub_14017A050 writes the
// LEVEL slot first and fills the CS slot afterwards through SESSION.STATE_MACHINE(netGame,7), so on
// the ordering ARCH-12 R4 proves statically the bind records CsSpaceAbsent and R2's second arm is
// never asked again for the life of that bind. It is now re-asked on the bind epoch -- the same
// one-second cadence the refusal retry uses, never per frame (ARCH-10 S7).
//
// kBindEpochTicks mirrors camera_module's kRetryEveryTicks, which is file-private. It is not taken
// on trust: the first subcase fails if the module's cadence is longer and the second fails if it is
// shorter, so the pair pins the number from both sides.
const int kBindEpochTicks = 60;

TEST_CASE("late arrival: the CS slot filling after a bind re-asks the differential without rebinding") {
    Env& e = TheEnv();

    SUBCASE("CsSpaceAbsent becomes CsSpaceClear, on the same control, with no rebind") {
        Rig r;
        r.netGame.PointAt(nullptr, &r.levelSpace);
        e.Tick(r.netGame.bytes);
        auto* const bound = camera_control::Get();
        REQUIRE(bound != nullptr);
        REQUIRE(std::string(camera_control::Diagnostic()).find("differential CsSpaceAbsent") !=
                std::string::npos);
        const size_t bindsBefore    = e.log.Count("camera control bound");
        const size_t releasesBefore = e.log.Count("camera control released");
        const size_t movesBefore    = e.log.Count("differential CsSpaceAbsent -> CsSpaceClear");

        // L5 run 3's recorded 1.3 s reversal, at this test's cadence: the slot fills while the
        // binding stands.
        r.netGame.PointAt(&r.csSpace, &r.levelSpace);
        e.Ticks(r.netGame.bytes, kBindEpochTicks);

        CHECK(camera_control::Get() == bound);                                  // not torn down
        CHECK(e.log.Count("camera control bound") == bindsBefore);              // and not re-bound
        CHECK(e.log.Count("camera control released") == releasesBefore);
        REQUIRE(camera_control::DifferentialWord() != nullptr);
        CHECK(std::string(camera_control::DifferentialWord()) == "CsSpaceClear");
        CHECK(std::string(camera_control::Diagnostic()).find("differential CsSpaceClear") !=
              std::string::npos);
        CHECK(e.log.Count("differential CsSpaceAbsent -> CsSpaceClear") == movesBefore + 1);
        e.Tick(nullptr);
    }

    SUBCASE("the arm is not re-asked before the epoch, and costs no lookup while the slot stands still") {
        Rig r;
        r.netGame.PointAt(nullptr, &r.levelSpace);
        e.Tick(r.netGame.bytes);
        REQUIRE(camera_control::Get() != nullptr);
        const int afterBind = g_engine.lookupCalls;

        r.netGame.PointAt(&r.csSpace, &r.levelSpace);
        e.Ticks(r.netGame.bytes, kBindEpochTicks - 1);

        REQUIRE(camera_control::DifferentialWord() != nullptr);
        CHECK(std::string(camera_control::DifferentialWord()) == "CsSpaceAbsent");
        CHECK(g_engine.lookupCalls == afterBind);   // ARCH-10 S7: not once per frame
        e.Tick(nullptr);
    }

    SUBCASE("a late slot that makes the object undecided releases instead of driving on") {
        Rig r;
        r.netGame.PointAt(nullptr, &r.levelSpace);
        e.Tick(r.netGame.bytes);
        REQUIRE(camera_control::Get() != nullptr);
        const size_t releasesBefore = e.log.Count("camera control released");
        const size_t refusalsBefore  = e.log.Count("differential now refuses");
        const size_t ambiguousBefore = e.log.Count("SpaceAmbiguous");

        g_engine.alsoAnswers = r.csSpace.bytes;   // a build that registers the CS in +0x2B08 too
        r.netGame.PointAt(&r.csSpace, &r.levelSpace);
        e.Ticks(r.netGame.bytes, kBindEpochTicks);

        CHECK(camera_control::Get() == nullptr);
        CHECK(e.log.Count("camera control released") == releasesBefore + 1);
        CHECK(e.log.Count("differential now refuses") == refusalsBefore + 1);
        CHECK(e.log.Count("SpaceAmbiguous") == ambiguousBefore + 1);
        e.Tick(nullptr);
    }

    SUBCASE("a late slot aliased onto the level space is the other refusing arm") {
        Rig r;
        r.netGame.PointAt(nullptr, &r.levelSpace);
        e.Tick(r.netGame.bytes);
        REQUIRE(camera_control::Get() != nullptr);
        const size_t releasesBefore = e.log.Count("camera control released");
        // Release() resets the control's diagnostic to "released", so the sentence that names the
        // arm is read where an operator reads it: the line the module wrote before unbinding.
        const size_t aliasedBefore = e.log.Count("SpaceAliased");

        r.netGame.PointAt(&r.levelSpace, &r.levelSpace);
        e.Ticks(r.netGame.bytes, kBindEpochTicks);

        CHECK(camera_control::Get() == nullptr);
        CHECK(e.log.Count("camera control released") == releasesBefore + 1);
        CHECK(e.log.Count("SpaceAliased") == aliasedBefore + 1);
        e.Tick(nullptr);
    }
}

TEST_CASE("must-fail: the accessor on the CS space finds nothing, which is L5 run 3") {
    Env& e = TheEnv();
    Rig r;
    r.netGame.PointAt(&r.levelSpace, &r.csSpace);   // slots swapped: +0x2B10 now holds space C
    REQUIRE(static_cast<void*>(r.csSpace.bytes) != static_cast<void*>(r.levelSpace.bytes));

    camera_control::RigIdentity id;
    REQUIRE(camera_control::Probe(r.netGame.bytes, &id) == camera_control::RigProbe::Ready);
    REQUIRE(id.levelSpace == static_cast<void*>(r.csSpace.bytes));

    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(std::string(camera_control::Diagnostic()).find("LookupNull") != std::string::npos);
    CHECK(g_engine.Asked(r.csSpace.bytes));
    e.Tick(nullptr);
}

TEST_CASE("a refused identity is retried, so a rig whose instance array fills later still binds") {
    Env& e = TheEnv();
    Rig r;
    r.rig.SetLocalIndex(7);   // local >= count: no instance yet

    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(std::string(camera_control::Diagnostic()).find("no local instance") != std::string::npos);

    r.rig.SetLocalIndex(1);
    e.Ticks(r.netGame.bytes, 59);
    CHECK(camera_control::Get() == nullptr);   // retried at one second, not every frame
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() != nullptr);

    e.Tick(nullptr);
}

// ★ ARCH-12 R1/R4: which slot the lookup's space is read out of, asserted on the resolve itself
// rather than through a bind, so a regression to +0x8 or to +0x2B08 cannot hide behind a
// downstream refusal.
TEST_CASE("the level space is resolved at NETGAME.SPECTATORCAM_CS_OFF, and +0x8 is never a space") {
    Rig r;
    camera_control::RigIdentity id;
    CHECK(camera_control::Probe(r.netGame.bytes, &id) == camera_control::RigProbe::Ready);
    CHECK(id.levelSpace == static_cast<void*>(r.levelSpace.bytes));
    CHECK(id.csSpace == static_cast<void*>(r.csSpace.bytes));
    CHECK(id.levelSpace != static_cast<void*>(r.netGame.gameFromNetGame));

    // A null CS space is recorded and NOT a refusal -- the whole of R4 in one assertion.
    void* none = nullptr;
    memcpy(r.netGame.bytes + facts::NETGAME_CS_SPACE_OFF, &none, sizeof none);
    CHECK(camera_control::Probe(r.netGame.bytes, &id) == camera_control::RigProbe::Ready);
    CHECK(id.csSpace == nullptr);
    CHECK(id.levelSpace == static_cast<void*>(r.levelSpace.bytes));

    // A null level space is the refusal, and it has its own word: no map is loaded, which is the
    // L3/L4 state -- an absent question, not a negative answer.
    void* level = r.levelSpace.bytes;
    memcpy(r.netGame.bytes + facts::NETGAME_SPECTATORCAM_CS_OFF, &none, sizeof none);
    CHECK(camera_control::Probe(r.netGame.bytes, &id) == camera_control::RigProbe::NoLevelSpace);
    CHECK(strcmp(camera_control::RigProbeName(camera_control::RigProbe::NoLevelSpace),
                 "level space null") == 0);

    // The mirror rigs: a real, constructed space parked where each superseded accessor looked. An
    // accessor reading +0x8 or +0x2B08 answers Ready here; this one must still answer NoLevelSpace.
    memcpy(r.netGame.bytes + facts::IDENT_NETGAME_GAMESPACE_OFF, &level, sizeof level);
    memcpy(r.netGame.bytes + facts::NETGAME_CS_SPACE_OFF, &level, sizeof level);
    CHECK(camera_control::Probe(r.netGame.bytes, &id) == camera_control::RigProbe::NoLevelSpace);

    // And an unreadable level space is its own word, distinct from both of the above.
    void* nowhere = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10));
    memcpy(r.netGame.bytes + facts::NETGAME_SPECTATORCAM_CS_OFF, &nowhere, sizeof nowhere);
    CHECK(camera_control::Probe(r.netGame.bytes, &id) ==
          camera_control::RigProbe::LevelSpaceUnreadable);
}

// Rewritten to ARCH-12 R1. Retroactive control over L5 re-run #3 (20260902-165645Z),
// the 12 [camera] refusals from 12:57:06.393 to 13:00:46.753. What those lines record, field by
// field: the space the product asked was 0000027118DA0000, which W1-E's independent
// cs-space-candidates line at 12:57:05.050 renders as "+0x2B08=0000027118DA0000 Ready/Ready ...
// cap=99 count=99 flags=0"; the "rig 0000027155760000" the line names is *(netGame+0x2B10), which
// ARCH-12 section 2.2 proves is not a rig at all but the LEVEL space; and the lookup answered null
// twelve times.
//
// The refusals were correct AND the product was wrong: 99 systems are installed in the +0x2B08
// space and the R14NetSpectatorCamera is not one of them, because it is registered in the level
// space that the product was printing as its "rig" and never asking. So the same recorded state
// must now BIND, and the +0x2B08 lookup answering null -- the thing that used to be the failure --
// is now the differential arm passing.
TEST_CASE("L5 run 3: the recorded state binds, and the +0x2B08 lookup answering null is the control") {
    Env& e = TheEnv();
    Rig r;   // space C: 99 entries, no spectator symbol. Space L: 35, with it.
    const size_t boundBefore = e.log.Count("camera control bound");

    camera_control::RigIdentity id;
    REQUIRE(camera_control::Probe(r.netGame.bytes, &id) == camera_control::RigProbe::Ready);
    REQUIRE(id.csSpace == static_cast<void*>(r.csSpace.bytes));      // 0000027118DA0000, cap=99
    REQUIRE(id.levelSpace == static_cast<void*>(r.levelSpace.bytes));  // 0000027155760000, non-null

    e.Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);
    CHECK(e.log.Count("camera control bound") == boundBefore + 1);
    const std::string d = camera_control::Diagnostic();
    CHECK(d.find("differential CsSpaceClear") != std::string::npos);
    // The refusal words the recorded state no longer earns, each one named so a regression to any
    // of them is a red test rather than a differently-worded pass.
    CHECK(d.find("LookupNull") == std::string::npos);
    CHECK(d.find("SpaceAmbiguous") == std::string::npos);
    CHECK(d.find("level space null") == std::string::npos);
    // Both questions were asked, of the right objects, and the CS space answered nothing.
    CHECK(g_engine.Asked(r.levelSpace.bytes));
    CHECK(g_engine.Asked(r.csSpace.bytes));
    CHECK(g_engine.lookupSymbol == facts::CAM_SPECTATOR_SYMBOL);
    CHECK(camera_control::BoundSpectatorCs() == static_cast<void*>(r.rig.cs));
    e.Tick(nullptr);
}

// AUD-F3B F3B-1, against W1-C's landed guard (f5a7191). A table that would otherwise be Ready or
// Empty answers NotReady/SortPending while CS.TABLE_FLAGS_OFF bit 0 is set, because the engine's
// lookup sorts the block IN PLACE before searching it -- an engine write on our tick thread
// (ARCH-10 section 7). So the bind must defer and CS.LOOKUP_BY_SYMBOL must never be called.
//
// The probe answers TableSortPending, not TableNotReady: this table IS constructed, and the two
// states wait on different things -- one on the loading window, one on the engine's own sort.
// Naming the wrong one is the false-cause defect, not a wording preference.
TEST_CASE("sort-pending: a constructed but unsorted table defers, and the engine is never called") {
    Env& e = TheEnv();
    Rig r;
    r.levelSpace.Put(0x38, 1);   // CS.TABLE_FLAGS_OFF bit 0, on an otherwise constructed table

    camera_control::RigIdentity id;
    CHECK(camera_control::Probe(r.netGame.bytes, &id) == camera_control::RigProbe::TableSortPending);
    CHECK(strcmp(camera_control::RigProbeName(camera_control::RigProbe::TableSortPending),
                 "system table sort pending") == 0);

    const size_t deferredBefore = e.log.Count("deferred: system table sort pending");
    g_engine.ForgetLookups();
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(g_engine.lookupCalls == 0);   // the sort never ran on our tick
    CHECK(e.log.Count("deferred: system table sort pending") == deferredBefore + 1);
    // And it is not filed as the loading window, which is a different wait with a different fix.
    CHECK(e.log.Count("deferred: system table not constructed") == 0);

    // Clearing the bit and nothing else binds, so the deferral was caused by the bit alone.
    r.levelSpace.Put(0x38, 0);
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() != nullptr);
    CHECK(g_engine.Asked(r.levelSpace.bytes));
    e.Tick(nullptr);
}

TEST_CASE("a level space whose table is not constructed, or absent, is deferred and retried") {
    Env& e = TheEnv();
    Rig r;
    const size_t boundBefore = e.log.Count("camera control bound");

    // Header fields that disagree with each other: allocated, not yet built. (An all-zero header
    // is the constructor's own empty state and the shared check answers Empty for it.)
    FakeSpace bare;
    bare.Construct(bare.elements);
    bare.Put(0x30, 5);   // count > capacity
    r.netGame.PointAt(&r.csSpace, &bare);
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(e.log.Count("deferred: system table not constructed") == 1);

    bare.Construct(bare.elements);
    bare.Put(0x30, 0);   // built, nothing installed
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(e.log.Count("deferred: system table empty") == 1);

    // No map loaded: the level space slot is null. This is the L3/L4 state, and it is a deferral
    // with its own word, not a failure.
    r.netGame.PointAt(&r.csSpace, nullptr);
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(e.log.Count("deferred: level space null") == 1);
    CHECK(e.log.Count("camera control bound") == boundBefore);

    // Not latched: the map arriving binds on the next tick.
    r.netGame.PointAt(&r.csSpace, &r.levelSpace);
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() != nullptr);
    CHECK(e.log.Count("camera control bound") == boundBefore + 1);

    e.Tick(nullptr);
}

TEST_CASE("a rebuilt table behind the same space, a rebuilt rig, or a new netGame re-creates") {
    Env& e = TheEnv();
    Rig r;
    const size_t boundBefore = e.log.Count("camera control bound");
    e.Tick(r.netGame.bytes);
    REQUIRE(e.log.Count("camera control bound") == boundBefore + 1);
    REQUIRE(camera_control::Get()->SetMode(CameraMode::Free, 0));

    // AVFIX F3: same address, new contents. The table base is the identity, not the space pointer.
    r.levelSpace.Construct(r.levelSpace.elementsRebuilt, 35);
    e.Tick(r.netGame.bytes);
    CHECK(e.log.Count("camera control bound") == boundBefore + 2);
    CHECK(e.log.Count("re-binding") >= 1);
    REQUIRE(camera_control::Get() != nullptr);
    // A fresh control: the old request does not survive a rebuild it never reached.
    CHECK(camera_control::Get()->RequestedMode() == CameraMode::Pov);

    // The rig rebuilt under a surviving space. The rig is the LOOKUP's answer and the lookup is
    // the only accessor there is (ARCH-12 R1), so it is not in the identity and cannot be: asking
    // it every tick is an engine call, and on an unsorted table an engine WRITE, on our thread.
    // What re-keys the identity is the space's table being rebuilt under it -- which is what a
    // re-registration does, and what this asserts.
    FakeRig rebuilt;
    g_engine.registeredRig = rebuilt.cs;
    r.levelSpace.Construct(r.levelSpace.elements, 35);
    e.Tick(r.netGame.bytes);
    CHECK(e.log.Count("camera control bound") == boundBefore + 3);
    REQUIRE(camera_control::Get() != nullptr);
    REQUIRE(camera_control::Get()->SetMode(CameraMode::Disc, 0));
    CHECK(g_engine.setModeCs == rebuilt.cs);
    CHECK(camera_control::BoundSpectatorCs() == static_cast<void*>(rebuilt.cs));

    // A new netGame carrying a new space.
    Rig second;
    e.Tick(second.netGame.bytes);
    CHECK(e.log.Count("camera control bound") == boundBefore + 4);
    CHECK(camera_control::Get() != nullptr);

    // The level space going away drops the control without a write into it.
    second.netGame.PointAt(&second.csSpace, nullptr);
    const int callsBefore = g_engine.setModeCalls;
    e.Tick(second.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(g_engine.setModeCalls == callsBefore);

    e.Tick(nullptr);
}

TEST_CASE("an unreadable netGame is refused before either slot is read") {
    Env& e = TheEnv();
    const size_t boundBefore = e.log.Count("camera control bound");
    e.Tick(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10)));
    CHECK(camera_control::Get() == nullptr);
    CHECK(e.log.Count("camera control bound") == boundBefore);

    // A rig whose instance array points into unmapped memory is refused at the instance, not
    // dereferenced.
    Rig r;
    void* nowhere = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10));
    memcpy(r.rig.cs + facts::SPECTATOR_CS_INSTANCE_BASE_OFF, &nowhere, sizeof nowhere);
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(std::string(camera_control::Diagnostic()).find("no local instance") != std::string::npos);
    e.Tick(nullptr);
}

TEST_CASE("without the fake engine the real site guards refuse on this host, and Get() stays null") {
    Env& e = TheEnv();
    Rig r;
    camera_control::SetEngineForTest(nullptr);
    e.Tick(r.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);
    CHECK(std::string(camera_control::Diagnostic()).find("site guard refused") != std::string::npos);
    camera_control::SetEngineForTest(&kFakeEngine);
    e.Tick(nullptr);
}

// ★ Ruling 4c, the positive half: the bind that actually happens on the shipping path TAKES the
// pose, and holds it. The refusal arms live in tests/camera/writepoint/test_poseowner.cpp; what
// only a real bind can show is that the claim LATCHES -- a claim replaced by a look would leave
// the token free and the write point would bind straight through the refusal that appears to work.
TEST_CASE("the shipping bind takes the pose owner and holds it until it unbinds") {
    Env& e = TheEnv();
    Rig r;
    camera::ResetPoseOwnerForTest();

    e.Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);
    CHECK(camera::PoseOwnerConflicts() == 0);
    // The whole trailing field, not a prefix of it: "conflicts 0" alone is a substring of
    // "conflicts 0..." for any count, and `arb` is the field that separates one process-wide
    // arbiter from a per-image one on a launch log.
    const std::string arb = camera::PoseOwnerArbiter();
    const bool arbCreated = arb.rfind("created#", 0) == 0;
    const bool arbOpened = arb.rfind("opened#", 0) == 0;
    const bool arbNamed = arbCreated || arbOpened;
    CHECK(arbNamed);
    CHECK(e.log.Count(("pose-owner: StockFreeCam, conflicts 0, arb " + arb + ", rel-refused 0")
                          .c_str()) >= 1);

    // The write point's own entry point, against the same netGame, refused before it resolves.
    CHECK(camera::Create(r.netGame.bytes) == nullptr);
    camera::Stats stats{};
    camera::GetStats(&stats);
    CHECK(stats.refusal == camera::Refusal::PoseOwnerConflict);
    CHECK(camera::PoseOwnerConflicts() == 1);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);
    CHECK(std::string(camera::LastPoseOwnerConflict()) ==
          "pose-owner-conflict: StockFreeCam vs WritePoint");

    // Unbinding hands the pose back; the fallback can then have it.
    e.Tick(nullptr);
    CHECK(camera_control::Get() == nullptr);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::None);
    CHECK(camera::ClaimPoseOwner(camera::PoseOwner::WritePoint));

    camera::ResetPoseOwnerForTest();
}

// ★ AUDIT_I finding 1, the release side, from the module that makes it live traffic: this file's
// Init and Shutdown both call camera_control::Release() unconditionally, whether or not the control
// ever bound. With release keyed on the owner alone that call frees whatever token happens to be
// out; keyed on the holding image it is a REFUSAL, and the refusal is counted and on the status
// line rather than being a silent no-op that leaves the pose quietly reassigned.
TEST_CASE("camera_control::Release from a module that never bound is refused, not honoured") {
    camera::ResetPoseOwnerForTest();
    REQUIRE(camera::ClaimPoseOwner(camera::PoseOwner::WritePoint));
    CHECK(camera::PoseOwnerReleaseRefusals() == 0);

    camera_control::Release();

    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::WritePoint);
    CHECK(camera::PoseOwnerReleaseRefusals() == 1);
    CHECK(camera::PoseOwnerConflicts() == 0);   // a refused RELEASE is not a refused CLAIM
    CHECK(std::string(camera::LastPoseOwnerConflict()).rfind("pose-owner-release-refused: ", 0) ==
          0);

    // The holder still lets go of its own token.
    camera::ReleasePoseOwner(camera::PoseOwner::WritePoint);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::None);
    CHECK(camera::PoseOwnerReleaseRefusals() == 1);

    camera::ResetPoseOwnerForTest();
}

// ---------------------------------------------------------------------------------------------
// W2-E. GetViewPose -- the view-pose READ ruling 4b needs, driven against the fake rig.
//
// The gap it closes: IReplayCamera publishes SetPose and nothing else, and camera::Observe()
// answers only while the W1-H write point is BOUND -- which is the one state ruling 4b excludes,
// because the stock free camera holds the pose owner whenever it is the thing being observed. So
// the observer row could not ask, and said so.
//
// The case that carries the correctness, and it is the third one below: on the free camera's arm
// (ci+0x274 != 0) the engine renders the VIEW-STATE's pose, so that arm never reads the CI at all.
// The CI is a different object the engine writes itself -- seven stores to ci+0x1C8 / ci+0x1D8
// inside CAM.SPECTATOR_UPDATE, docs/spikes/W1-H.md -- so a read that fell back to it when the view
// state could not be reached would answer a question about the view state with a value from
// somewhere else, and the ruling would score that value as a pose. Refused, not approximated.
// (AUDIT_VIEWREAD_R2 VR2-1: "SetPose is the only writer of the CI pose" was false.)
namespace {

camera::ViewPoseRead  g_fakeVsRead = camera::ViewPoseRead::Ok;
camera::ViewStatePose g_fakeVs;
int                   g_fakeVsCalls = 0;

camera::ViewPoseRead FakeViewStateRead(camera::ViewStatePose* out) {
    ++g_fakeVsCalls;
    if (g_fakeVsRead == camera::ViewPoseRead::Ok) {
        *out = g_fakeVs;
        return g_fakeVsRead;
    }
    // A refusal blanks every scored field and carries only the sentence -- the shape W1-H's read
    // produces, so the propagation under test is the production one.
    *out = camera::ViewStatePose{};
    snprintf(out->why, sizeof out->why, "%s", g_fakeVs.why);
    return g_fakeVsRead;
}

// Installs the fake through the same call camera_module.cpp uses for the real one, and puts the
// production reader back: there is one route to this pointer and no fixture flag beside it.
struct ReaderSwap {
    camera_control::ViewStateReader prev;
    ReaderSwap() : prev(camera_control::InstalledViewStateReader()) {
        g_fakeVsCalls = 0;
        g_fakeVsRead = camera::ViewPoseRead::Ok;
        g_fakeVs = camera::ViewStatePose{};
        // The default is a build whose VIEWSTATE.INPUT_GATE_FREE_SITE still matches, which is what
        // W1-H's read reports on a good image. The case that clears it says so.
        g_fakeVs.integratorArmedKnown = true;
        camera_control::SetViewStateReader(&FakeViewStateRead);
    }
    ~ReaderSwap() { camera_control::SetViewStateReader(prev); }
};

void PutCi(FakeRig& rig, uint32_t off, const void* src, size_t n) {
    memcpy(rig.LocalCi() + off, src, n);
}

void PutCiPose(FakeRig& rig, const float pos[3], const float quat[4], float fovY, int32_t freeCam) {
    PutCi(rig, replay::camera_facts::kCiPosOff, pos, 12);
    PutCi(rig, replay::camera_facts::kCiQuatOff, quat, 16);
    PutCi(rig, replay::camera_facts::kCiFovYOff, &fovY, sizeof fovY);
    PutCi(rig, replay::camera_facts::kCiFreeCamActiveOff, &freeCam, sizeof freeCam);
}

bool BlankPose(const replay::ViewPose& p) {
    const replay::ViewPose zero;
    return memcmp(p.pos, zero.pos, sizeof p.pos) == 0 &&
           memcmp(p.quat, zero.quat, sizeof p.quat) == 0 && p.fovYRadians == 0.0f;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// W2-E, ARCH-17 P17-D. The commanded pose: CommandPose / Regime / ReleasePose.
//
// The fake write point is the shape W1-H's real one has -- it takes the pose and it writes it
// somewhere the engine's own read can see it -- so the read-back these cases score is the
// production read (GetViewPose on the CI arm) and not the fixture's memory of what it was handed.
// That is what makes "the delta landed" a comparison rather than a restatement.
namespace {

struct FakeWritePoint final : replay::IReplayCamera {
    FakeRig*  rig     = nullptr;
    int       poses   = 0;
    int       creates = 0;
    int       releases = 0;
    void*     lastNetGame = nullptr;
    float     pos[3]  = {0.0f, 0.0f, 0.0f};
    float     quat[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float     fovY    = 0.0f;

    void SetPose(const float p[3], const float q[4], float fov) override {
        ++poses;
        memcpy(pos, p, sizeof pos);
        memcpy(quat, q, sizeof quat);
        fovY = fov;
        // What the real write point does that matters here: the pose becomes the one the engine
        // holds, so the next relative command resolves against THIS and not against the pose the
        // session started with.
        if (rig) PutCiPose(*rig, p, q, fov, 0);
    }
    bool        AppliesToAllViews() const override { return true; }
    const char* Strategy() const override { return "fake"; }
};

FakeWritePoint g_fakeWp;
bool           g_fakeWpRefuses = false;

replay::IReplayCamera* FakeCreate(void* netGame) {
    ++g_fakeWp.creates;
    g_fakeWp.lastNetGame = netGame;
    if (g_fakeWpRefuses) return nullptr;
    // The real camera::Create re-claims the token this control has just transferred to itself.
    REQUIRE(camera::ClaimPoseOwner(camera::PoseOwner::WritePoint));
    return &g_fakeWp;
}
void        FakeRelease() { ++g_fakeWp.releases; camera::ReleasePoseOwner(camera::PoseOwner::WritePoint); }
// ARCH-19 R1. The real camera::ReleaseKeepingToken is Relinquish() and nothing else: the token
// stays with WritePoint so the caller can hand it over through the arbiter's atomic transfer. The
// fake mirrors exactly that -- it counts the re-arm and does NOT touch the holder word.
void        FakeReleaseKeepingToken() { ++g_fakeWp.releases; }
const char* FakeWhy() { return "no-game"; }

const camera_control::WritePointSeam kFakeSeam = {&FakeCreate, &FakeRelease, &FakeWhy,
                                                  &FakeReleaseKeepingToken};
// The pre-R1 image: three real pointers and a null fourth. AUDIT_ARCH_19_20 C4 -- this must hand
// the camera back through the degraded route, never refuse.
const camera_control::WritePointSeam kFakeSeamNoKeepToken = {&FakeCreate, &FakeRelease, &FakeWhy,
                                                             nullptr};

// Installs the fake through the same call camera_module.cpp uses for the real one: one route, no
// fixture flag beside it.
struct WritePointSwap {
    const camera_control::WritePointSeam* prev;
    WritePointSwap() : prev(camera_control::InstalledWritePoint()) {
        g_fakeWp = FakeWritePoint{};
        g_fakeWpRefuses = false;
        camera_control::SetWritePoint(&kFakeSeam);
    }
    ~WritePointSwap() { camera_control::SetWritePoint(prev); }
};

// ARCH-19 R3's seam, installed through the same call camera_module.cpp uses for the real one.
struct FakeSubject {
    camera_control::SubjectResolve verdict = camera_control::SubjectResolve::Resolved;
    bool     bodyOk    = true;
    float    point[3]  = {0.0f, 0.0f, 0.0f};
    uint16_t userIndex = 7;
    int      userCalls = 0;
    int      pointCalls = 0;
    int32_t  lastSelector = -1;
};
FakeSubject g_subject;

camera_control::SubjectResolve FakeUserOfSelector(void*, int32_t selector, uint16_t* out) {
    ++g_subject.userCalls;
    g_subject.lastSelector = selector;
    if (g_subject.verdict != camera_control::SubjectResolve::Resolved) return g_subject.verdict;
    if (out) *out = g_subject.userIndex;
    return camera_control::SubjectResolve::Resolved;
}

bool FakeWorldPointOfUser(void*, uint16_t userIndex, float out[3]) {
    ++g_subject.pointCalls;
    if (!g_subject.bodyOk || userIndex != g_subject.userIndex) return false;
    memcpy(out, g_subject.point, sizeof g_subject.point);
    return true;
}

const camera_control::SubjectPoseSeam kFakeSubjectSeam = {&FakeUserOfSelector,
                                                          &FakeWorldPointOfUser};

struct SubjectSwap {
    const camera_control::SubjectPoseSeam* prev;
    SubjectSwap() : prev(camera_control::InstalledSubjectPose()) {
        g_subject = FakeSubject{};
        camera_control::SetSubjectPose(&kFakeSubjectSeam);
    }
    ~SubjectSwap() { camera_control::SetSubjectPose(prev); }
};

// The forward vector the LANDED quaternion actually points along, read the way ARCH-19 R5 reads
// viewstate+0x50 and NOT through DecomposeFacing: a check scored through the encoder's own inverse
// cannot see the two of them agreeing on the wrong thing.
void ForwardOf(const float q[4], float out[3]) {
    out[0] = 2.0f * (q[0] * q[2] + q[3] * q[1]);
    out[1] = 2.0f * (q[1] * q[2] - q[3] * q[0]);
    out[2] = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
}

// A bound control over a rig whose CI already holds a known pose, which is the starting state
// every case below measures a delta from.
IReplayCameraControl* BindWithPose(Env& e, Rig& r, const float pos[3], const float quat[4],
                                   float fovY) {
    PutCiPose(r.rig, pos, quat, fovY, 0);
    const int32_t mode = 5;
    PutCi(r.rig, facts::SPECTATOR_CI_MODE_OFF, &mode, sizeof mode);
    e.Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    REQUIRE(c != nullptr);
    return c;
}

const float kStartPos[3]  = {12.0f, 3.0f, -8.0f};
const float kStartQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // yaw 0, pitch 0 under this file's convention

}  // namespace

TEST_CASE("commanded pose: the write point is installed by the module, not by a test") {
    // The wiring assertion, for the reason the view-state reader's is: camera_control.cpp cannot
    // call camera::Create directly (four images compile it without camera_writepoint.cpp), so the
    // install is a line in camera_module.cpp, and a line nothing asserts is a line that can be
    // deleted.
    TheEnv();
    const camera_control::WritePointSeam* wp = camera_control::InstalledWritePoint();
    REQUIRE(wp != nullptr);
    CHECK(wp->create == &replay::camera::Create);
    CHECK(wp->release == &replay::camera::Release);
    // ARCH-19 R1's fourth pointer. A null here is not a crash and not a refusal -- it is the
    // DEGRADED hand-back, which works and moves the epoch -- so nothing but this assertion would
    // notice the shipping image losing it.
    CHECK(wp->releaseKeepingToken == &replay::camera::ReleaseKeepingToken);
}

TEST_CASE("commanded pose: a move is written once, with the exact pose, and the regime says so") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    CHECK(c->Regime() == replay::PoseRegime::Free);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[0] = 1.0f;                       // `cam move 1 0 0`
    REQUIRE(c->CommandPose(p));

    CHECK(g_fakeWp.creates == 1);
    CHECK(g_fakeWp.lastNetGame == static_cast<void*>(r.netGame.bytes));
    CHECK(g_fakeWp.poses == 1);
    CHECK(g_fakeWp.pos[0] == doctest::Approx(13.0));    // the delta resolved against the ENGINE's pose
    CHECK(g_fakeWp.pos[1] == doctest::Approx(3.0));
    CHECK(g_fakeWp.pos[2] == doctest::Approx(-8.0));
    CHECK(g_fakeWp.fovY == doctest::Approx(0.96));      // the field of view is the read's, never substituted
    CHECK(c->Regime() == replay::PoseRegime::Commanded);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::WritePoint);
    CHECK(std::string(camera_control::Diagnostic()).find("regime commanded") != std::string::npos);

    // P17-G's row reads the delta back through the engine, not through us.
    replay::ViewPose back;
    REQUIRE(c->GetViewPose(&back) == camera::ViewPoseRead::Ok);
    CHECK(back.pos[0] == doctest::Approx(13.0));

    // A second command resolves against the first's RESULT, and enters no regime it is already in.
    REQUIRE(c->CommandPose(p));
    CHECK(g_fakeWp.creates == 1);                       // no second camera::Create, no second lookup
    CHECK(camera::PoseOwnerTransfers() == 1);
    CHECK(g_fakeWp.pos[0] == doctest::Approx(14.0));

    REQUIRE(c->ReleasePose());
    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: an absolute move and a look are exactly what was asked for") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    replay::CommandedPose p;
    p.setPosition = true;
    p.absolute = true;
    p.pos[0] = -4.5f;
    p.pos[1] = 2.25f;
    p.pos[2] = 7.0f;
    REQUIRE(c->CommandPose(p));
    CHECK(g_fakeWp.pos[0] == doctest::Approx(-4.5));
    CHECK(g_fakeWp.pos[1] == doctest::Approx(2.25));
    CHECK(g_fakeWp.pos[2] == doctest::Approx(7.0));

    // `cam look 90 0`: the quaternion changes and the position does not (P10-CAM-4's question).
    replay::CommandedPose look;
    look.setOrientation = true;
    look.absolute = true;
    look.yawDeg = 90.0f;
    REQUIRE(c->CommandPose(look));
    CHECK(g_fakeWp.pos[0] == doctest::Approx(-4.5));
    CHECK(g_fakeWp.pos[2] == doctest::Approx(7.0));
    CHECK(g_fakeWp.quat[1] == doctest::Approx(0.70710678).epsilon(0.001));   // 90 deg about +Y
    CHECK(g_fakeWp.quat[3] == doctest::Approx(0.70710678).epsilon(0.001));

    // A relative look adds to the facing the engine now holds, so encode and decode are inverses:
    // two 90 degree turns are a half turn and not a quarter one.
    replay::CommandedPose again;
    again.setOrientation = true;
    again.yawDeg = 90.0f;
    REQUIRE(c->CommandPose(again));
    CHECK(g_fakeWp.quat[1] == doctest::Approx(1.0).epsilon(0.001));
    CHECK(g_fakeWp.quat[3] == doctest::Approx(0.0).epsilon(0.001));

    REQUIRE(c->ReleasePose());
    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: release hands the token back and re-arms through the write point") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[1] = 2.0f;
    REQUIRE(c->CommandPose(p));
    REQUIRE(c->Regime() == replay::PoseRegime::Commanded);

    REQUIRE(c->ReleasePose());
    // The hand-back is BOTH facts: W1-H's Relinquish ran (it is what re-arms viewstate+0x9C,
    // ARCH-17 S3.3(c)) and the token is back with the stock free camera. A release that did only
    // one of the two leaves the owner's "freely move around" broken for the session.
    CHECK(g_fakeWp.releases == 1);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);
    CHECK(c->Regime() == replay::PoseRegime::Free);
    CHECK(camera::PoseOwnerReleaseRefusals() == 0);

    // Idempotent: `cam release` twice is a success that did nothing, not a refusal, and it does
    // not call the write point a second time.
    CHECK(c->ReleasePose());
    CHECK(g_fakeWp.releases == 1);
    CHECK(c->LastRefusal() == replay::ControlRefusal::None);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}


// -------------------------------------------------------------------------------------------
// ARCH-19 R1 / R6 (packet P17-C2). Written RED against P17-D's landed ReleasePose and ComposePose
// before either was changed; the RED output is quoted in ENG-W2E-P17C2.md.
// -------------------------------------------------------------------------------------------

TEST_CASE("commanded pose: a command / release round trip does not move the pose-owner epoch") {
    // ARCH-19 R1. TransferPoseOwner is specified NOT to move the epoch
    // (camera_writepoint_poseowner.h:315-317) because two images comparing epochs are asking
    // whether they share one arbiter, and a rename of the holder is not a new bind. A hand-back
    // that goes through a None holder claims from zero, which DOES move it -- so every
    // `cam release` would tell a second image that the arbiter rebound.
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    const uint32_t epoch    = camera::PoseOwnerEpoch();
    const uint64_t degraded  = camera_control::PoseHandbackDegraded();

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[1]      = 2.0f;
    REQUIRE(c->CommandPose(p));
    CHECK(camera::PoseOwnerEpoch() == epoch);   // the acquire direction already promises this

    REQUIRE(c->ReleasePose());
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);
    CHECK(camera::PoseOwnerEpoch() == epoch);
    CHECK(camera::PoseOwnerReleaseRefusals() == 0);   // and the hand-back is not a counted refusal
    CHECK(camera_control::PoseHandbackDegraded() == degraded);   // through the transfer, not around it

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: a pure `cam move` preserves the orientation it was not asked to change") {
    // ARCH-19 R6 defect 2. The command asks for no orientation at all, so the current facing is
    // not one of its inputs and must be copied through -- roll included. A yaw/pitch round trip
    // drops roll by construction (DecomposeFacing's own comment), which is a change nobody asked
    // for.
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    // 30 degrees of pure ROLL about the forward axis: yaw 0, pitch 0, and a quaternion the
    // yaw/pitch pair cannot express.
    const float rolled[4] = {0.0f, 0.0f, 0.258819f, 0.965926f};
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, rolled, 0.96f);

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[0]      = 1.0f;
    REQUIRE(c->CommandPose(p));

    CHECK(g_fakeWp.pos[0] == doctest::Approx(13.0));   // the move happened
    CHECK(g_fakeWp.quat[0] == rolled[0]);              // and the facing is bit-identical, not
    CHECK(g_fakeWp.quat[1] == rolled[1]);              //   re-composed from a yaw and a pitch
    CHECK(g_fakeWp.quat[2] == rolled[2]);
    CHECK(g_fakeWp.quat[3] == rolled[3]);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: a vertical facing does not refuse the two commands it is not an input to") {
    // ARCH-19 R6 defects 1 and 3. A pure `cam move` and an ABSOLUTE `cam look` both name a
    // destination outright; neither reads the current facing, so neither can be refused for what
    // that facing happens to be.
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    // Straight down: forward is (0, -1, 0) and the yaw axis is degenerate.
    const float down[4] = {0.70710678f, 0.0f, 0.0f, 0.70710678f};
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, down, 0.96f);

    replay::CommandedPose move;
    move.setPosition = true;
    move.absolute    = true;
    move.pos[0]      = 1.0f;
    move.pos[1]      = 2.0f;
    move.pos[2]      = 3.0f;
    REQUIRE(c->CommandPose(move));
    CHECK(g_fakeWp.pos[0] == doctest::Approx(1.0));
    CHECK(g_fakeWp.pos[1] == doctest::Approx(2.0));
    CHECK(g_fakeWp.pos[2] == doctest::Approx(3.0));

    replay::CommandedPose look;
    look.setOrientation = true;
    look.absolute       = true;
    look.yawDeg         = 45.0f;
    REQUIRE(c->CommandPose(look));
    CHECK(g_fakeWp.quat[0] == doctest::Approx(0.0).epsilon(0.001));
    CHECK(g_fakeWp.quat[1] == doctest::Approx(0.38268343).epsilon(0.001));
    CHECK(g_fakeWp.quat[2] == doctest::Approx(0.0).epsilon(0.001));
    CHECK(g_fakeWp.quat[3] == doctest::Approx(0.92387953).epsilon(0.001));

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}


TEST_CASE("commanded pose: a RELATIVE look off a vertical facing still refuses, by its own name") {
    // ARCH-19 R6, the one row the refusal survives on: this command's yaw IS added to the current
    // facing, and at vertical there is no facing to add it to. A clamp would silently change which
    // way the camera points, which is worse than a named refusal.
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    const float down[4] = {0.70710678f, 0.0f, 0.0f, 0.70710678f};
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, down, 0.96f);

    replay::CommandedPose look;
    look.setOrientation = true;
    look.yawDeg         = 10.0f;
    CHECK_FALSE(c->CommandPose(look));
    CHECK(c->LastRefusal() == replay::ControlRefusal::FacingDegenerate);
    CHECK(std::string(replay::ControlRefusalName(c->LastRefusal())) == "facing-degenerate");
    CHECK(g_fakeWp.poses == 0);
    // And it is no longer the drawer everything else is put in: NotAvailable is the rig not
    // checking out, and this is not that.
    CHECK(c->LastRefusal() != replay::ControlRefusal::NotAvailable);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: an image with no releaseKeepingToken hands the camera back, degraded") {
    // A null fourth pointer must never be a refusal: that would leave the
    // camera in regime Commanded with no way back, which is a regression against the behaviour
    // that shipped. The pre-R1 route is taken instead -- named in the sentence and counted,
    // because it moves the epoch R1 exists to hold still.
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();   // BEFORE the seam swap: the module installs the real one at Init
    Rig r;
    g_fakeWp.rig = &r.rig;
    camera_control::SetWritePoint(&kFakeSeamNoKeepToken);
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);
    // The precondition is asserted, not assumed: this case is about what happens with a THREE-
    // field seam, so a case that silently ran against the four-field one would be green for the
    // wrong reason.
    REQUIRE(camera_control::InstalledWritePoint() == &kFakeSeamNoKeepToken);

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[1]      = 2.0f;
    REQUIRE(c->CommandPose(p));
    REQUIRE(camera_control::InstalledWritePoint() == &kFakeSeamNoKeepToken);

    const uint64_t degraded = camera_control::PoseHandbackDegraded();
    const uint32_t epoch    = camera::PoseOwnerEpoch();

    CHECK(c->ReleasePose());                                       // a success, not a refusal
    CHECK(c->LastRefusal() == replay::ControlRefusal::None);
    CHECK(c->Regime() == replay::PoseRegime::Free);                // and never stranded
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);
    CHECK(g_fakeWp.releases == 1);                                 // the S3.3(c) re-arm still ran
    CHECK(camera_control::PoseHandbackDegraded() == degraded + 1);
    CHECK(camera::PoseOwnerEpoch() != epoch);                      // the cost, measured not hidden
    CHECK(std::string(camera_control::Diagnostic()).find("degraded hand-back") !=
          std::string::npos);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("A pure move says `facing unchanged`, never a facing nobody commanded") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    // The camera starts aimed somewhere that is NOT 0/0, which is what makes the two readings
    // distinguishable: R6's !setOrientation row never writes commandedYawDeg_/commandedPitchDeg_,
    // so a sentence that printed them here would print 0.0 / 0.0 -- a facing this camera is not in
    // and has never been in.
    float startQuat[4];
    camera_control::BuildFacing(41.25f, -6.0f, startQuat);
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, startQuat, 0.96f);

    replay::CommandedPose move;
    move.setPosition = true;
    move.absolute    = true;
    move.pos[0]      = 1.0f;
    move.pos[1]      = 2.0f;
    move.pos[2]      = 3.0f;
    REQUIRE_FALSE(move.setOrientation);
    REQUIRE(c->CommandPose(move));
    const std::string said(camera_control::Diagnostic());
    CHECK_MESSAGE(said.find("facing unchanged") != std::string::npos, said);
    CHECK(said.find("yaw") == std::string::npos);
    CHECK(said.find("pitch") == std::string::npos);
    CHECK(said.find("2.000") != std::string::npos);   // the half it DOES command is still reported

    // The control: a command that sets an orientation still prints the angles it commanded, so the
    // branch above is a branch and not the only sentence this function can say.
    replay::CommandedPose look;
    look.setOrientation = true;
    look.absolute       = true;
    look.yawDeg         = 41.25f;
    look.pitchDeg       = -6.0f;
    REQUIRE(c->CommandPose(look));
    char expect[64];
    snprintf(expect, sizeof expect, "yaw %.1f pitch %.1f", 41.25, -6.0);
    const std::string aimed(camera_control::Diagnostic());
    CHECK_MESSAGE(aimed.find(expect) != std::string::npos, aimed);
    CHECK(aimed.find("facing unchanged") == std::string::npos);

    REQUIRE(c->ReleasePose());
    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("A refused hand-back stays outstanding, and the next release retries it") {
    // The refusal is reached through the arbiter's own rule -- a transfer whose `from` is not the
    // holder fails -- and not through a flag inside the code under test.
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    replay::CommandedPose move;
    move.setPosition = true;
    move.pos[1]      = 2.0f;
    REQUIRE(c->CommandPose(move));
    REQUIRE(c->Regime() == replay::PoseRegime::Commanded);
    REQUIRE(camera::PoseOwnerHeld() == camera::PoseOwner::WritePoint);

    // Something else took the token out from under the hand-back.
    camera::ReleasePoseOwner(camera::PoseOwner::WritePoint);
    REQUIRE(camera::PoseOwnerHeld() == camera::PoseOwner::None);
    CHECK_FALSE(c->ReleasePose());
    CHECK(c->LastRefusal() == replay::ControlRefusal::PoseNotOwned);
    CHECK(c->Regime() == replay::PoseRegime::Unavailable);
    CHECK(g_fakeWp.releases == 1);   // the S3.3(c) re-arm ran; only the token move is outstanding

    // The cause cleared. The second `cam release` must RE-ATTEMPT the hand-back: answering true
    // here without moving the token is the defect -- it strands the camera for the session.
    REQUIRE(camera::ClaimPoseOwner(camera::PoseOwner::WritePoint));
    CHECK(c->ReleasePose());
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);
    CHECK(c->LastRefusal() == replay::ControlRefusal::None);
    CHECK(c->Regime() == replay::PoseRegime::Free);
    CHECK(g_fakeWp.releases == 1);   // and the seam was NOT re-armed a second time
    CHECK(std::string(camera_control::Diagnostic()).find("retry") != std::string::npos);

    // Idempotent again once it is done: a third release is a success that does nothing, not a
    // third transfer.
    CHECK(c->ReleasePose());
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("write point: ReleaseKeepingToken keeps the token, and Release is it plus the drop") {
    // ARCH-19 R1's done-when, both halves. The runtime half: the token is still WritePoint's after
    // the re-arm, which is what makes the caller's TransferPoseOwner take its success arm.
    camera::ResetPoseOwnerForTest();
    REQUIRE(camera::ClaimPoseOwner(camera::PoseOwner::WritePoint));
    camera::ReleaseKeepingToken();
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::WritePoint);
    CHECK(camera::PoseOwnerReleaseRefusals() == 0);
    camera::Release();
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::None);
    camera::ResetPoseOwnerForTest();

    // The source half, and it is the one that matters: "Stats::gateRestored rises identically on
    // both" holds only while there is ONE body doing the re-arm. Two bodies that agree today are
    // two bodies that can drift, and no runtime assertion over an unbound module can see it --
    // the gate counters are zero on both paths there, which is a check that cannot fail.
    namespace fs = std::filesystem;
    std::ifstream in(fs::path(REPLAY_REPO_ROOT) / "src" / "engine" / "camera_writepoint.cpp");
    REQUIRE(in.good());
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string opener = std::string("void Release") + "() {";
    const size_t at = all.find(opener);
    REQUIRE(at != std::string::npos);
    const std::string closer = std::string(1, static_cast<char>(10)) + "}";
    const std::string body   = all.substr(at, all.find(closer, at) - at);
    CHECK(body.find("ReleaseKeepingToken();") != std::string::npos);
    CHECK(body.find("Relinquish(") == std::string::npos);   // it does not carry a second re-arm
}

TEST_CASE("commanded pose: a pose owner we cannot take is refused BY NAME, and nothing is written") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    // The other claimant in this process took the pose after the bind. RULING 4b: a camera that is
    // somebody else's is never silently taken -- the command refuses and the holder is named.
    camera::ResetPoseOwnerForTest();
    REQUIRE(camera::ClaimPoseOwner(camera::PoseOwner::WritePoint));

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[0] = 1.0f;
    CHECK_FALSE(c->CommandPose(p));
    CHECK(c->LastRefusal() == replay::ControlRefusal::PoseNotOwned);
    CHECK(std::string(replay::ControlRefusalName(c->LastRefusal())) == "pose-not-owned");
    CHECK(g_fakeWp.poses == 0);        // nothing was written while somebody else held the pose
    CHECK(g_fakeWp.creates == 0);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::WritePoint);
    CHECK(camera::PoseOwnerTransferRefusals() == 1);
    CHECK(c->Regime() == replay::PoseRegime::Free);   // the regime is UNCHANGED by a refusal

    camera::ResetPoseOwnerForTest();
    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: the oracle's track holds the camera, and says which refusal that is") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    REQUIRE(c->BeginOracleTrack());
    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[0] = 1.0f;
    CHECK_FALSE(c->CommandPose(p));
    CHECK(c->LastRefusal() == replay::ControlRefusal::OracleTrackHolds);
    CHECK(std::string(replay::ControlRefusalName(c->LastRefusal())) == "oracle-track-holds");
    CHECK(g_fakeWp.poses == 0);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);
    // SetMode's OracleTrackActive is a different value for a different question, and neither
    // wording changes (ARCH-17 S3.2).
    CHECK_FALSE(c->SetMode(replay::CameraMode::Free, 0));
    CHECK(c->LastRefusal() == replay::ControlRefusal::OracleTrackActive);
    c->EndOracleTrack();

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: a write point that does not check out rolls the transfer back") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    g_fakeWpRefuses = true;
    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[0] = 1.0f;
    CHECK_FALSE(c->CommandPose(p));
    CHECK(g_fakeWp.poses == 0);
    // Rolled back in the same call, so the stock free camera is not locked out of a slot it could
    // have had, and the write point's own twenty-value refusal is what the operator reads.
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);
    CHECK(c->Regime() == replay::PoseRegime::Free);
    CHECK(c->LastRefusal() == replay::ControlRefusal::WritePointRefused);   // ARCH-19 R4
    CHECK(std::string(camera_control::Diagnostic()).find("no-game") != std::string::npos);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: an unreadable pose refuses instead of teleporting to the origin") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    // The free camera's arm, where the pose is the view state's -- and a view-state read that
    // refuses is the whole answer: a relative move off a zeroed pose is a teleport to the world
    // origin, and an absolute one still has no field of view to write.
    const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PutCiPose(r.rig, kStartPos, quat, 0.96f, 1);
    e.Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    REQUIRE(c != nullptr);
    g_fakeVsRead = camera::ViewPoseRead::Unreadable;
    snprintf(g_fakeVs.why, sizeof g_fakeVs.why, "the view-state array is not committed memory");

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[0] = 1.0f;
    CHECK_FALSE(c->CommandPose(p));
    CHECK(c->LastRefusal() == replay::ControlRefusal::PoseUnreadable);    // ARCH-19 R4
    CHECK(g_fakeWp.poses == 0);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);   // the token never moved

    replay::CommandedPose abs;
    abs.setPosition = true;
    abs.absolute = true;
    CHECK_FALSE(c->CommandPose(abs));
    CHECK(g_fakeWp.poses == 0);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: a human mode selection hands the pose back before it selects") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[0] = 1.0f;
    REQUIRE(c->CommandPose(p));
    REQUIRE(c->Regime() == replay::PoseRegime::Commanded);

    // ARCH-17 S5: selecting a mode while we still hold the write point's token would leave
    // viewstate+0x9C cleared under the mode the human just chose.
    REQUIRE(c->SetMode(replay::CameraMode::Free, 0));
    CHECK(g_fakeWp.releases == 1);
    CHECK(c->Regime() == replay::PoseRegime::Free);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: an unbind while commanded hands the pose back rather than stranding it") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    replay::CommandedPose p;
    p.setPosition = true;
    p.pos[0] = 1.0f;
    REQUIRE(c->CommandPose(p));

    e.Tick(nullptr);   // the rig goes away: camera_module unbinds
    CHECK(camera_control::Get() == nullptr);
    CHECK(g_fakeWp.releases == 1);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::None);
    CHECK(camera::PoseOwnerReleaseRefusals() == 0);

    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: with no write point in this image nothing is written and it says so") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    Env& e = TheEnv();
    Rig r;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);
    const camera_control::WritePointSeam* prev = camera_control::InstalledWritePoint();
    camera_control::SetWritePoint(nullptr);

    replay::CommandedPose p;
    p.setPosition = true;
    CHECK_FALSE(c->CommandPose(p));
    CHECK(c->LastRefusal() == replay::ControlRefusal::WritePointRefused);
    CHECK(std::string(camera_control::Diagnostic()).find("no write point is installed") !=
          std::string::npos);
    CHECK(camera::PoseOwnerHeld() == camera::PoseOwner::StockFreeCam);

    camera_control::SetWritePoint(prev);
    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: `cam look at` is refused because no selector's pose is published") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    replay::CommandedPose p;
    p.setOrientation = true;
    p.lookAt = true;
    p.lookAtSelector = 3;
    CHECK_FALSE(c->CommandPose(p));
    CHECK(c->LastRefusal() == replay::ControlRefusal::LookAtUnresolvable);   // ARCH-19 R4
    CHECK(g_fakeWp.poses == 0);
    CHECK(std::string(camera_control::Diagnostic()).find("no subject-pose seam") != std::string::npos);

    e.Tick(nullptr);
    camera::ResetPoseOwnerForTest();
}

TEST_CASE("commanded pose: the regime is Unavailable before a bind and after a release") {
    camera::ResetPoseOwnerForTest();
    Env& e = TheEnv();
    e.Tick(nullptr);
    CHECK(camera_control::Get() == nullptr);

    struct Bare final : IReplayCameraControl {
        bool SetMode(replay::CameraMode, int32_t) override { return false; }
        replay::CameraMode RequestedMode() const override { return replay::CameraMode::Pov; }
        bool ObservedMode(replay::CameraMode*) const override { return false; }
        bool ObservedSelector(int32_t*, uint16_t*) const override { return false; }
        bool ActiveLevelCameraSet(uint16_t*) const override { return false; }
        bool BeginOracleTrack() override { return false; }
        void EndOracleTrack() override {}
        bool OracleTrackActive() const override { return false; }
        bool Available() const override { return false; }
        replay::ControlRefusal LastRefusal() const override {
            return replay::ControlRefusal::NotAvailable;
        }
    } bare;
    // The three defaults, which is what W2-F's panel fake inherits: a control with no camera
    // behind it has no pose to command, is in no regime, and releasing nothing is safe.
    replay::CommandedPose p;
    CHECK_FALSE(bare.CommandPose(p));
    CHECK(bare.Regime() == replay::PoseRegime::Unavailable);
    CHECK(bare.ReleasePose());
    CHECK(std::string(replay::PoseRegimeName(replay::PoseRegime::Commanded)) == "commanded");
    CHECK(std::string(replay::PoseRegimeName(replay::PoseRegime::Free)) == "free");

    camera::ResetPoseOwnerForTest();
}

TEST_CASE("view pose: the production reader is installed by the module, not by a test") {
    // ★ The wiring assertion. camera_control.cpp cannot call W1-H's read directly -- it is linked
    // into three images that do not carry camera_writepoint.cpp -- so the install is a line in
    // camera_module.cpp, and a line that is never asserted is a line that can be deleted.
    TheEnv();
    CHECK(camera_control::InstalledViewStateReader() == &replay::camera::ReadViewStatePose);
}

TEST_CASE("view pose: on the CI arm the pose is the CI's, read through the bound rig") {
    ReaderSwap swap;
    Rig r;
    const float pos[3] = {10.0f, 11.0f, 12.0f};
    const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PutCiPose(r.rig, pos, quat, 1.2f, 0);
    const int32_t mode = 1;
    PutCi(r.rig, facts::SPECTATOR_CI_MODE_OFF, &mode, sizeof mode);
    g_fakeVs.integratorArmed = 1;

    TheEnv().Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    INFO("camera diagnostic: " << camera_control::Diagnostic());
    REQUIRE(c != nullptr);

    replay::ViewPose out;
    CHECK(c->GetViewPose(&out) == camera::ViewPoseRead::Ok);
    CHECK(out.source == replay::ViewPoseSource::CameraInstance);
    CHECK(out.pos[0] == doctest::Approx(10.0));
    CHECK(out.pos[2] == doctest::Approx(12.0));
    CHECK(out.quat[3] == doctest::Approx(1.0));
    CHECK(out.fovYRadians == doctest::Approx(1.2));
    CHECK(out.mode == 1);
    // The gate is read on this arm too, because "armed" is the fact S8.1.2 names.
    CHECK(out.integratorArmedKnown);
    CHECK(out.integratorArmed == 1);
}

TEST_CASE("view pose: on the free camera's arm the pose is the view-state's, not the CI's") {
    ReaderSwap swap;
    Rig r;
    const float ciPos[3] = {10.0f, 11.0f, 12.0f};
    const float ciQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PutCiPose(r.rig, ciPos, ciQuat, 1.2f, 1);  // ci+0x274 set: CAM.SPECTATOR_UPDATE reads the view-state
    g_fakeVs.pos[0] = 40.0f;
    g_fakeVs.pos[1] = 41.0f;
    g_fakeVs.pos[2] = 42.0f;
    g_fakeVs.quat[0] = 0.5f;
    g_fakeVs.fovY = 0.96f;
    g_fakeVs.integratorArmed = 1;

    TheEnv().Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    INFO("camera diagnostic: " << camera_control::Diagnostic());
    REQUIRE(c != nullptr);

    replay::ViewPose out;
    CHECK(c->GetViewPose(&out) == camera::ViewPoseRead::Ok);
    CHECK(out.source == replay::ViewPoseSource::ViewState);
    CHECK(g_fakeVsCalls == 1);
    CHECK(out.pos[0] == doctest::Approx(40.0));
    CHECK(out.pos[2] == doctest::Approx(42.0));
    CHECK(out.quat[0] == doctest::Approx(0.5));
    CHECK(out.fovYRadians == doctest::Approx(0.96));
    CHECK(out.integratorArmed == 1);
}

TEST_CASE("view pose: a gate the read could not evidence is unknown, and costs nothing else") {
    // AUDIT_K2b finding 2. viewstate+0x9C's only V-PRIMARY evidence is
    // VIEWSTATE.INPUT_GATE_FREE_SITE; the pose displacements come out of a different window. So a
    // drifted image can hand back a good pose and a meaningless gate, and W1-H reports that as
    // Known=false. This control must PASS THAT THROUGH -- not overwrite it with "the read said Ok,
    // so the gate is read" -- because the observer's ruling-4b row keys on Known.
    ReaderSwap swap;
    Rig r;
    const float ciPos[3] = {10.0f, 11.0f, 12.0f};
    const float ciQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PutCiPose(r.rig, ciPos, ciQuat, 1.2f, 1);
    g_fakeVs.pos[0] = 40.0f;
    g_fakeVs.fovY = 0.96f;
    g_fakeVs.integratorArmed = 0;
    g_fakeVs.integratorArmedKnown = false;

    TheEnv().Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    INFO("camera diagnostic: " << camera_control::Diagnostic());
    REQUIRE(c != nullptr);

    replay::ViewPose out;
    REQUIRE(c->GetViewPose(&out) == camera::ViewPoseRead::Ok);
    CHECK_FALSE(out.integratorArmedKnown);
    CHECK(out.integratorArmed == 0);
    // The pose is unaffected: the two facts are independent and only one of them was lost.
    CHECK(out.source == replay::ViewPoseSource::ViewState);
    CHECK(out.pos[0] == doctest::Approx(40.0));
    CHECK(out.fovYRadians == doctest::Approx(0.96));

    // And the flag is not simply always false: the same call on a read that DID evidence it.
    g_fakeVs.integratorArmed = 1;
    g_fakeVs.integratorArmedKnown = true;
    REQUIRE(c->GetViewPose(&out) == camera::ViewPoseRead::Ok);
    CHECK(out.integratorArmedKnown);
    CHECK(out.integratorArmed == 1);
}

TEST_CASE("view pose: the engine having no view-state is its own word, not the plugin's NotBound") {
    // AUDIT_K2b finding 4. "this control is not bound" and "the engine has no CR15Game / no
    // view-state array / no view-state 0" used to be the same word, so a consumer reading it could
    // not tell an idle plugin from an engine that had nothing to give.
    ReaderSwap swap;
    Rig r;
    const float ciPos[3] = {10.0f, 11.0f, 12.0f};
    const float ciQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PutCiPose(r.rig, ciPos, ciQuat, 1.2f, 1);
    g_fakeVsRead = camera::ViewPoseRead::NoViewState;

    TheEnv().Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    REQUIRE(c != nullptr);

    replay::ViewPose out;
    CHECK(c->GetViewPose(&out) == camera::ViewPoseRead::NoViewState);
    CHECK(BlankPose(out));
    CHECK_FALSE(out.integratorArmedKnown);

    // The control's own refusal keeps its own word, and the two are distinct.
    struct Bare final : replay::IReplayCameraControl {
        bool SetMode(replay::CameraMode, int32_t) override { return false; }
        replay::CameraMode RequestedMode() const override { return replay::CameraMode::Pov; }
        bool ObservedMode(replay::CameraMode*) const override { return false; }
        bool ObservedSelector(int32_t*, uint16_t*) const override { return false; }
        bool ActiveLevelCameraSet(uint16_t*) const override { return false; }
        bool BeginOracleTrack() override { return false; }
        void EndOracleTrack() override {}
        bool OracleTrackActive() const override { return false; }
        bool Available() const override { return false; }
        replay::ControlRefusal LastRefusal() const override {
            return replay::ControlRefusal::NotAvailable;
        }
    } bare;
    CHECK(bare.GetViewPose(&out) == camera::ViewPoseRead::NotBound);
    CHECK(camera::ViewPoseRead::NotBound != camera::ViewPoseRead::NoViewState);
}

TEST_CASE("view pose: a refused read names its site, and drift is not the unmapped string") {
    // AUDIT_VIEWREAD_R2 VR2-2. Unreadable is one word for several facts. W1-H's read computes the
    // sentence that separates them and this call used to discard it, so a drifted build and an
    // uncommitted page were the same string everywhere outside W1-H's own tests.
    ReaderSwap swap;
    Rig r;
    const float ciPos[3] = {10.0f, 11.0f, 12.0f};
    const float ciQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PutCiPose(r.rig, ciPos, ciQuat, 1.2f, 1);
    TheEnv().Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    REQUIRE(c != nullptr);

    replay::ViewPose out;
    g_fakeVsRead = camera::ViewPoseRead::Unreadable;
    snprintf(g_fakeVs.why, sizeof g_fakeVs.why,
             "prologue-mismatch: CAM.VIEW_COMMIT at 0x140D12340 + 20 bytes does not hold the "
             "bytes facts.tsv recorded for this build");
    REQUIRE(c->GetViewPose(&out) == camera::ViewPoseRead::Unreadable);
    const std::string drift = camera_control::Diagnostic();
    CHECK(drift.find("CAM.VIEW_COMMIT") != std::string::npos);
    CHECK(drift.find("prologue-mismatch") != std::string::npos);

    snprintf(g_fakeVs.why, sizeof g_fakeVs.why,
             "view-state 0 at 0x1A2B3C40 + 160 bytes is not committed memory");
    REQUIRE(c->GetViewPose(&out) == camera::ViewPoseRead::Unreadable);
    const std::string unmapped = camera_control::Diagnostic();
    CHECK(unmapped.find("not committed memory") != std::string::npos);
    CHECK(unmapped.find("prologue-mismatch") == std::string::npos);
    CHECK(drift != unmapped);
}

TEST_CASE("view pose: a refused read reaches the [camera] status line an operator actually reads") {
    // AUDIT_L1 finding 3. The sentence above lands in camera_control's diagnostic static, whose
    // one shipped reader is the bind-REFUSAL branch -- a branch that cannot run in a session where
    // GetViewPose is being asked at all. So in VR2-2's own scenario the operator saw nothing. The
    // module publishes it instead, on the line that already carries pose-owner and arb.
    Env& e = TheEnv();
    ReaderSwap swap;
    Rig r;
    const float ciPos[3] = {1.0f, 2.0f, 3.0f};
    const float ciQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PutCiPose(r.rig, ciPos, ciQuat, 1.2f, 1);  // free cam: the arm that answers from the view state
    e.Tick(nullptr);                           // end whatever session an earlier case left bound
    e.Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    REQUIRE(c != nullptr);
    // A binding session owns its own refusals: Create() clears the latch, so this is not reading
    // a sentence some earlier case in this process left behind.
    CHECK(camera_control::ViewReadRefusals() == 0);
    CHECK(camera_control::ViewReadRefusal()[0] == '\0');
    const size_t before = e.log.Count("camera control view read refused");

    replay::ViewPose out;
    g_fakeVsRead = camera::ViewPoseRead::NoViewState;
    snprintf(g_fakeVs.why, sizeof g_fakeVs.why,
             "CR15Game at 0x1A2B3C40 holds no view-state array");
    REQUIRE(c->GetViewPose(&out) == camera::ViewPoseRead::NoViewState);
    CHECK(camera_control::ViewReadRefusals() == 1);

    // Asked every frame, said once: the second refusal raises the count and does not repeat the
    // line, and the FIRST sentence is the one kept -- a frame loop must not leave the operator
    // with whichever refusal happened to be last.
    snprintf(g_fakeVs.why, sizeof g_fakeVs.why, "view-state 0 at 0x50 + 160 bytes is not committed");
    REQUIRE(c->GetViewPose(&out) == camera::ViewPoseRead::NoViewState);
    CHECK(camera_control::ViewReadRefusals() == 2);
    CHECK(std::string(camera_control::ViewReadRefusal()).find("holds no view-state array") !=
          std::string::npos);

    e.Tick(r.netGame.bytes);
    e.Tick(r.netGame.bytes);
    CHECK(e.log.Count("camera control view read refused") == before + 1);

    // One line, and it carries BOTH: the sentence W1-H computed and the pose-owner fields the
    // observer row parses off this family of lines. A sentence on a line with neither would not
    // be readable in vivo.
    bool complete = false;
    for (const Line& l : e.log.lines) {
        const bool refusal = l.text.find("camera control view read refused") != std::string::npos;
        const bool named   = l.text.find("holds no view-state array") != std::string::npos;
        const bool owner   = l.text.find("pose-owner: ") != std::string::npos &&
                             l.text.find("arb ") != std::string::npos;
        if (refusal && named && owner) complete = true;
    }
    CHECK(complete);

    // Release ends the session: the next binding starts from "none yet" and can say so.
    camera_control::Release();
    CHECK(camera_control::ViewReadRefusals() == 0);
    CHECK(camera_control::ViewReadRefusal()[0] == '\0');
}

// ★ AUDIT_K2b finding 5. camera_control::Release() clears this file's g_created, but the module
// keeps its own bound flag, so a call from outside wedges the module un-rebindable. The severity
// ruling was a caller census: all three product calls are in camera_module.cpp -- Unbind (which
// clears the flag on the next line), Init and Shutdown (which reset the whole state) -- and no
// probe or tool calls it. This case asserts the BEHAVIOUR that census rests on; the census itself
// -- exactly three call sites, all in camera_module.cpp -- is the source scan below, because no
// assertion inside this process can see a fourth caller (AUDIT_VIEWREAD_R2 VR2-3).
TEST_CASE("camera_control::Release is the module's to call, and the module's own paths rebind") {
    Rig first;
    TheEnv().Tick(first.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);

    // The module's own unbind. Every product call site has this shape -- Release() and the bound
    // flag cleared together -- and the proof is that the next rig binds on the tick it arrives.
    TheEnv().Tick(nullptr);
    REQUIRE(camera_control::Get() == nullptr);
    Rig second;
    TheEnv().Tick(second.netGame.bytes);
    CHECK(camera_control::Get() != nullptr);

    // Behind the module's back: the instance is dropped but the module still believes it is bound,
    // so a tick on the SAME rig identity does not rebind and Get() stays null. W2-F and W2-G must
    // unbind through the module. If a later change makes Tick treat Get()==nullptr as unbound this
    // goes red -- which is the asymmetry being gone, and the case should then be updated, not the
    // fix reverted.
    camera_control::Release();
    TheEnv().Tick(second.netGame.bytes);
    CHECK(camera_control::Get() == nullptr);

    // Leave the module where the rest of the file expects it.
    TheEnv().Tick(nullptr);
    Rig third;
    TheEnv().Tick(third.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);
    TheEnv().Tick(nullptr);
}

TEST_CASE("view pose: a view-state that cannot be read is refused, never answered from the CI") {
    ReaderSwap swap;
    Rig r;
    const float ciPos[3] = {10.0f, 11.0f, 12.0f};
    const float ciQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PutCiPose(r.rig, ciPos, ciQuat, 1.2f, 1);
    g_fakeVsRead = camera::ViewPoseRead::Unreadable;

    TheEnv().Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    INFO("camera diagnostic: " << camera_control::Diagnostic());
    REQUIRE(c != nullptr);

    replay::ViewPose out;
    CHECK(c->GetViewPose(&out) == camera::ViewPoseRead::Unreadable);
    // ★ The whole point: the CI pose is RIGHT THERE and readable, and publishing it would be a
    // camera reported as stationary while it flies. Blank, so nothing downstream can score it.
    CHECK(BlankPose(out));
    CHECK_FALSE(out.integratorArmedKnown);
}

TEST_CASE("view pose: unbound is NotBound, and a bound rig with no readable CI is Unreadable") {
    ReaderSwap swap;
    Rig r;
    TheEnv().Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    INFO("camera diagnostic: " << camera_control::Diagnostic());
    REQUIRE(c != nullptr);

    replay::ViewPose out;
    SUBCASE("the rig's instance array is not committed memory") {
        void* reserved = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
        REQUIRE(reserved != nullptr);
        memcpy(r.rig.cs + facts::SPECTATOR_CS_INSTANCE_BASE_OFF, &reserved, sizeof reserved);
        CHECK(c->GetViewPose(&out) == camera::ViewPoseRead::Unreadable);
        CHECK(BlankPose(out));
    }
    SUBCASE("nothing is bound") {
        // Through the module own unbind (a tick with no netGame) rather than
        // camera_control::Release() behind its back: the module would still believe it was bound,
        // would not rebind on the next tick, and every later case would find Get() null.
        TheEnv().Tick(nullptr);
        CHECK(camera_control::Get() == nullptr);
        CHECK(c->GetViewPose(&out) == camera::ViewPoseRead::NotBound);
        CHECK(BlankPose(out));
    }
    SUBCASE("a control that never implemented the read publishes no pose") {
        // The interface's default, which W2-F's panel fake inherits.
        struct Bare final : IReplayCameraControl {
            bool SetMode(replay::CameraMode, int32_t) override { return false; }
            replay::CameraMode RequestedMode() const override { return replay::CameraMode::Pov; }
            bool ObservedMode(replay::CameraMode*) const override { return false; }
            bool ObservedSelector(int32_t*, uint16_t*) const override { return false; }
            bool ActiveLevelCameraSet(uint16_t*) const override { return false; }
            bool BeginOracleTrack() override { return false; }
            void EndOracleTrack() override {}
            bool OracleTrackActive() const override { return false; }
            bool Available() const override { return false; }
            replay::ControlRefusal LastRefusal() const override {
                return replay::ControlRefusal::NotAvailable;
            }
        } bare;
        CHECK(bare.GetViewPose(&out) == camera::ViewPoseRead::NotBound);
    }
}

TEST_CASE("view pose: the observer's two-sample row, and its three outcomes") {
    camera_control::ViewPoseSample a{}, b{};
    a.read = camera::ViewPoseRead::Ok;
    b.read = camera::ViewPoseRead::Ok;
    a.pose.source = b.pose.source = replay::ViewPoseSource::ViewState;
    a.wallTicks = 1000;
    b.wallTicks = 1000 + 5000000;  // whatever QPF is, a strictly later stamp

    double metres = -1.0, seconds = -1.0;

    SUBCASE("a pose that changed while paused is Moved, and reports its distance") {
        b.pose.pos[0] = 3.0f;
        b.pose.pos[1] = 4.0f;
        CHECK(camera_control::PoseMotionBetween(a, b, &metres, &seconds) ==
              camera_control::PoseMotion::Moved);
        CHECK(metres == doctest::Approx(5.0));
        CHECK(seconds > 0.0);
    }
    SUBCASE("orientation alone counts as motion") {
        b.pose.quat[0] = 0.01f;
        CHECK(camera_control::PoseMotionBetween(a, b, &metres, &seconds) ==
              camera_control::PoseMotion::Moved);
        CHECK(metres == doctest::Approx(0.0));
    }
    SUBCASE("an unchanged pose over a measurable span is Still") {
        CHECK(camera_control::PoseMotionBetween(a, b, &metres, &seconds) ==
              camera_control::PoseMotion::Still);
        CHECK(seconds > 0.0);
    }
    SUBCASE("a refused sample is CouldNotAsk, not Still") {
        b.read = camera::ViewPoseRead::Unreadable;
        b.pose.pos[0] = 3.0f;
        CHECK(camera_control::PoseMotionBetween(a, b, &metres, &seconds) ==
              camera_control::PoseMotion::CouldNotAsk);
        CHECK(metres == 0.0);
        CHECK(seconds == 0.0);
    }
    SUBCASE("two different sources are two different quantities") {
        b.pose.source = replay::ViewPoseSource::CameraInstance;
        b.pose.pos[0] = 3.0f;
        CHECK(camera_control::PoseMotionBetween(a, b, &metres, &seconds) ==
              camera_control::PoseMotion::CouldNotAsk);
    }
    SUBCASE("no wall time between the samples proves nothing") {
        b.wallTicks = a.wallTicks;
        b.pose.pos[0] = 3.0f;
        CHECK(camera_control::PoseMotionBetween(a, b, &metres, &seconds) ==
              camera_control::PoseMotion::CouldNotAsk);
    }
    CHECK(strcmp(camera_control::PoseMotionName(camera_control::PoseMotion::CouldNotAsk),
                 "could-not-ask") == 0);
}

TEST_CASE("view pose: SampleViewPose stamps the wall clock and survives a null control") {
    ReaderSwap swap;
    Rig r;
    TheEnv().Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);

    camera_control::ViewPoseSample first{}, second{};
    CHECK(camera_control::SampleViewPose(camera_control::Get(), &first) ==
          camera::ViewPoseRead::Ok);
    CHECK(camera_control::SampleViewPose(camera_control::Get(), &second) ==
          camera::ViewPoseRead::Ok);
    // Wall time, not sim time: at pause the sim step is 1 us and a row measuring its own span from
    // the transport clock would divide by ~0.
    CHECK(second.wallTicks >= first.wallTicks);
    CHECK(first.wallTicks != 0);

    camera_control::ViewPoseSample none{};
    CHECK(camera_control::SampleViewPose(nullptr, &none) == camera::ViewPoseRead::NotBound);
    CHECK(none.wallTicks != 0);
}

TEST_CASE("camera_control::Release has three call sites and all three are in camera_module.cpp") {
    // AUDIT_VIEWREAD_R2 VR2-3. The severity ruling on AUDIT_K2b finding 5 was a caller census,
    // and a census is a property of the source tree rather than of a running process: the case
    // above drives Release() and cannot observe a fourth caller anywhere. So this one reads the
    // tree and counts. Each line is cut at its first "//" -- a prose mention of the name in a
    // comment is not a call site, and must not have to be updated when one is added.
    namespace fs = std::filesystem;
    const fs::path root = fs::path(REPLAY_REPO_ROOT);
    REQUIRE(fs::is_directory(root));

    // The token is BUILT here and never spelled: this file is inside the scanned tree now, and a
    // scanner that spells its own subject counts itself. The qualifier is required and whitespace
    // is removed first, so `Release ()` and a newline between the name and the paren are seen.
    const std::string qualified = std::string("camera_control") + "::Release(";

    std::map<std::string, int> perFile;
    int filesRead = 0;
    int usingDirectives = 0;
    // AUDIT_L1 finding 4: tests/ is where every probe in this repo lives, so a scan of src/ and
    // tools/ could not see the half of the claim that says "no probe calls it".
    for (const char* dir : {"src", "tools", "tests"}) {
        const fs::path start = root / dir;
        if (!fs::is_directory(start)) continue;
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(start)) {
            if (!e.is_regular_file()) continue;
            const std::string ext = e.path().extension().string();
            if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
            std::ifstream in(e.path());
            REQUIRE(in.good());
            ++filesRead;
            const std::string name = e.path().lexically_relative(root).generic_string();
            // Two passes: a namespace ALIAS for camera_control makes `alias::Release()` a real
            // call site the qualified token cannot see (tests/replay/observer.cpp has one),
            // so the aliases are collected first and searched for alongside.
            std::vector<std::string> tokens{qualified};
            std::vector<std::string> code;
            std::string line;
            while (std::getline(in, line)) {
                std::string bare = line.substr(0, line.find("//"));
                bare.erase(std::remove_if(bare.begin(), bare.end(),
                                          [](unsigned char c) { return std::isspace(c) != 0; }),
                           bare.end());
                if (bare.find("usingnamespace") != std::string::npos &&
                    bare.find("camera_control") != std::string::npos) {
                    INFO("using-directive for camera_control in " << name << ": " << line);
                    ++usingDirectives;
                }
                const size_t eq = bare.find('=');
                if (bare.rfind("namespace", 0) == 0 && eq != std::string::npos &&
                    bare.compare(eq + 1, std::string::npos, "replay::camera_control;") == 0) {
                    // These files alias it to its own name (`namespace camera_control =
                    // replay::camera_control;`), so an un-deduplicated token list counts every
                    // call in them TWICE -- which is what the first run of this scan did.
                    const std::string alias = bare.substr(9, eq - 9) + "::Release(";
                    if (std::find(tokens.begin(), tokens.end(), alias) == tokens.end()) {
                        tokens.push_back(alias);
                    }
                }
                code.push_back(bare);
            }
            for (const std::string& bare : code) {
                for (const std::string& tok : tokens) {
                    for (size_t at = bare.find(tok); at != std::string::npos;
                         at = bare.find(tok, at + 1)) {
                        ++perFile[name];
                    }
                }
            }
        }
    }

    // Not vacuous: the scan opened a real tree, and the file it must find is the file it finds
    // with the count the ruling rests on. A scan that read nothing would fail here first.
    CHECK(filesRead > 20);
    CHECK(perFile["src/camera/camera_module.cpp"] == 3);

    // An unqualified Release() reached through a using-directive is a call site no qualifier scan
    // can see, so the scan asserts there is none rather than assuming it.
    CHECK(usingDirectives == 0);

    // The census, pinned per file rather than by exclusion, so that a call APPEARING in a probe
    // and a call DISAPPEARING from a driver are both red. Product: three, all in camera_module.cpp
    // (Unbind, Init, Shutdown). Tests: the three files that drive Release() on purpose, and
    // nowhere else -- notably no probe_*.cpp.
    const std::map<std::string, int> expected = {
        {"src/camera/camera_module.cpp", 3},
        {"tests/camera/test_camera_module.cpp", 3},
        {"tests/camera/writepoint/test_poseowner.cpp", 2},
        {"tests/camera/writepoint/test_poseowner_images.cpp", 1},
    };
    int total = 0;
    for (const std::pair<const std::string, int>& f : perFile) {
        INFO("Release call sites in " << f.first << ": " << f.second);
        const std::map<std::string, int>::const_iterator want = expected.find(f.first);
        CHECK(want != expected.end());
        if (want != expected.end()) CHECK(want->second == f.second);
        total += f.second;
    }
    CHECK(perFile.size() == expected.size());
    CHECK(total == 9);
}

// ARCH-12 R5 / ENG-W2E-PA-R2 section 6. The pointer these three lines carry is the LEVEL space
// (+0x2B10), so the noun is "level space"; the bare "game space" is what ARCH-12 reserves
// against, and tests/replay/observer_rows.h parses this wording, so it is load-bearing on both
// sides. Env and its log are shared across this file, so every bind, refusal and release driven
// above is still here -- revert any one of the three emitters and a zero-count below reds.
TEST_CASE("the three camera control lines name the level space, never the bare \"game space\"") {
    Env& e = TheEnv();
    const size_t bound    = e.log.Count("camera control bound: rig");
    const size_t refused  = e.log.Count("camera control refused rig");
    const size_t released = e.log.Count("camera control released (netGame");
    REQUIRE(bound > 0);       // none of the counts below can be vacuous
    REQUIRE(refused > 0);
    REQUIRE(released > 0);
    CHECK(e.log.Count("on level space ") >= bound + refused);
    CHECK(e.log.Count(", level space ") >= released);
    CHECK(e.log.Count("on game space ") == 0);
    CHECK(e.log.Count(", game space ") == 0);
    // The one camera line that still says it, correctly: the pre-bind wait names no pointer.
    CHECK(e.log.Count("waits for a constructed game space") >= 1);
}

// THE SPECTATOR-SYMBOL CENSUS, COUNTED. Routed to W2-E by ARCH-15 S7 item 2. The
// 17 sites now live in camera_control.h; this is what makes them countable rather than readable, so
// that a site added, dropped or duplicated cannot land without moving a number here. The three VAs
// spelled out below are transcribed from AUDIT_ARCH_11_12_13.md's own quoted disassembly (lines
// 102, 277, 289), not from the census, so this is a cross-check between two sources and not a
// restatement of one.
TEST_CASE("the spectator-symbol reference census is 17 sites, four attributed and thirteen not") {
    const uint64_t* const sites = camera_control::kSpectatorSymbolRefSites;
    const size_t total = sizeof(camera_control::kSpectatorSymbolRefSites) / sizeof(sites[0]);
    const uint64_t* const attributed = camera_control::kSpectatorSymbolAttributedSites;
    const size_t attributedCount =
        sizeof(camera_control::kSpectatorSymbolAttributedSites) / sizeof(attributed[0]);

    CHECK(total == 17);                              // AUD-ARCH F-E(iii)'s count
    CHECK(attributedCount == 4);                     // ARCH-12 S2.3's table
    CHECK(total - attributedCount == 13);            // ARCH-14 S6 item 3's unclassified set
    CHECK(camera_control::kSpectatorSymbolStringVa == 0x1416D800Full);

    // Counted, not asserted per element: one CHECK per site would inflate the gate's assertion
    // total by 17 and say nothing these three do not.
    size_t ascending = 0;
    for (size_t i = 1; i < total; ++i) {
        ascending += sites[i - 1] < sites[i] ? 1u : 0u;   // strict, so no VA is listed twice
    }
    CHECK(ascending == total - 1);

    size_t attributedInCensus = 0;
    for (size_t a = 0; a < attributedCount; ++a) {
        for (size_t i = 0; i < total; ++i) {
            attributedInCensus += sites[i] == attributed[a] ? 1u : 0u;
        }
    }
    CHECK(attributedInCensus == attributedCount);    // the split is of THIS census, not beside it

    const uint64_t quoted[] = {0x140D1F9ACull, 0x1401B3525ull, 0x140DA2965ull};
    size_t quotedFound = 0;
    for (size_t q = 0; q < sizeof(quoted) / sizeof(quoted[0]); ++q) {
        for (size_t i = 0; i < total; ++i) {
            quotedFound += sites[i] == quoted[q] ? 1u : 0u;
        }
    }
    CHECK(quotedFound == sizeof(quoted) / sizeof(quoted[0]));
}
TEST_CASE("the ARCH-17 S9 non-test-caller criterion is word-bounded, because unbounded it is vacuous") {
    // S9 / ARCH-19 E4 freeze the done-when as
    //     grep -rn "CommandPose|ReleasePose" src/ --include=*.cpp
    // and require it non-empty. IT IS ALREADY NON-EMPTY AT HEAD, before any consumer exists,
    // because `ReleasePose` is a substring of `ReleasePoseOwner` -- an unrelated symbol in
    // src/engine/camera_writepoint.cpp. That is the "substring matching is the commonest false
    // pass" defect, and the criterion as frozen could go green with no shipping caller at all.
    // This case is the criterion re-encoded so it CAN fail, plus the record of why it had to be.
    namespace fs = std::filesystem;
    const fs::path root = fs::path(REPLAY_REPO_ROOT);
    REQUIRE(fs::is_directory(root));

    // The names are BUILT, never spelled, because this file is not the tree being scanned but a
    // scanner that spells its own subject is one edit away from counting itself.
    const std::string cmd = std::string("Command") + "Pose";
    const std::string rel = std::string("Release") + "Pose";

    // Word-bounded on BOTH sides: the name is followed by its call paren and preceded by a
    // non-identifier character. `ReleasePoseOwner(` fails the first test, `MyReleasePose(` the
    // second.
    auto callSites = [](const std::string& text, const std::string& name) {
        int n = 0;
        const std::string tok = name + "(";
        for (size_t at = text.find(tok); at != std::string::npos; at = text.find(tok, at + 1)) {
            if (at == 0) { ++n; continue; }
            const unsigned char before = static_cast<unsigned char>(text[at - 1]);
            if (!std::isalnum(before) && before != '_') ++n;
        }
        return n;
    };

    // The known-answer control, because a matcher nobody has seen say NO has not been shown able
    // to. Both strings are assembled so neither is a literal this scan could find in itself.
    const std::string decoy = rel + "Owner(PoseOwner::WritePoint);";
    const std::string real  = "cam->" + rel + "();";
    CHECK(callSites(decoy, rel) == 0);   // the exact false pass the frozen grep takes
    CHECK(callSites(real, rel) == 1);
    CHECK(callSites(rel + "(", rel) == 1);           // at offset 0, no preceding character
    CHECK(callSites("My" + rel + "();", rel) == 0);  // and not a suffix of a longer name

    std::vector<std::string> callers;
    bool writePointIsACaller = false;
    bool loosePatternHitsWritePoint = false;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(root / "src")) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
        std::ifstream in(e.path());
        REQUIRE(in.good());
        std::string code;
        std::string line;
        while (std::getline(in, line)) {
            code += line.substr(0, line.find("//"));   // a prose mention is not a call site
            code += static_cast<char>(10);
        }
        const std::string name = e.path().lexically_relative(root).generic_string();
        if (name == "src/camera/camera_control.cpp" || name == "src/camera/camera_control.h")
            continue;   // the family's own definition is not one of its consumers
        if (callSites(code, cmd) + callSites(code, rel) > 0) {
            callers.push_back(name);
            if (name == "src/engine/camera_writepoint.cpp") writePointIsACaller = true;
        }
        if (name == "src/engine/camera_writepoint.cpp" && code.find(rel) != std::string::npos)
            loosePatternHitsWritePoint = true;
    }

    // The finding, recorded as an assertion rather than as prose: the LOOSE pattern matches this
    // file and the anchored one does not. If ReleasePoseOwner is ever renamed, this case goes red
    // and the next reader is told the criterion no longer needs the anchor -- which is the honest
    // outcome, not a silent relaxation.
    CHECK(loosePatternHitsWritePoint);
    CHECK_FALSE(writePointIsACaller);

    // Grep B, stated as C1 asks: no file under src/ui/ reaches the family directly. W2-F goes
    // through dispatch::Submit (ARCH-17 S3.4), and a panel that called the interface itself would
    // sit below the command gate.
    for (const std::string& name : callers) {
        CHECK(name.rfind("src/ui/", 0) != 0);
    }

    // P17-E is what makes this non-empty for a real reason. Until it lands the set is empty, and
    // an empty set here is a STATE, not a pass: the case says which.
    MESSAGE("S9 anchored call sites in src/ (excluding the definition): " << callers.size());
    for (const std::string& name : callers) MESSAGE("  " << name);
}


// ================= ARCH-19 R3 / AUDIT_ARCH_19_20 C2, C3, C6 -- `cam look at <n>` ==============

TEST_CASE("look at: the selector packing is the engine's, and the engine is what certifies it") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    // Neither end of the selector range is this function's business, and both are silent wrong
    // answers if it invents one.
    uint8_t team = 0xEE, slot = 0xEE;
    CHECK_FALSE(camera_control::PackSelector(0, &team, &slot));
    CHECK_FALSE(camera_control::PackSelector(IReplayCameraControl::kPlayerSelectorMax + 1,
                                             &team, &slot));
    CHECK(team == 0xEE);   // an out-of-range selector writes nothing at all
    CHECK(slot == 0xEE);

    // p10-cam-9's predicate, run in process: the engine is handed n, the engine writes ci+0x26,
    // and OUR pair has to reproduce the word it wrote. AUDIT_ARCH_19_20 C2 -- the packed word and
    // the (teamId, teamSlot) split are scored TOGETHER, so a correct word reached through a
    // swapped split still fails.
    for (int32_t n = 1; n <= IReplayCameraControl::kPlayerSelectorMax; ++n) {
        REQUIRE(camera_control::PackSelector(n, &team, &slot));
        REQUIRE(c->SetMode(CameraMode::Pov, n));
        int32_t packed = 0;
        REQUIRE(c->ObservedSelector(&packed, nullptr));
        CHECK(packed == camera_control::PackedSelectorWord(team, slot));
        CHECK(camera_control::SelectorDerivationState(n) ==
              camera_control::SelectorDerivation::Certified);
    }

    // The two halves are distinguishable: five slots on one team, five on the other, and no
    // selector shares a word with another.
    CHECK(camera_control::PackedSelectorWord(1, 0) != camera_control::PackedSelectorWord(0, 1));
    for (int32_t a = 1; a <= 10; ++a) {
        uint8_t ta = 0, sa = 0;
        REQUIRE(camera_control::PackSelector(a, &ta, &sa));
        for (int32_t b = a + 1; b <= 10; ++b) {
            uint8_t tb = 0, sb = 0;
            REQUIRE(camera_control::PackSelector(b, &tb, &sb));
            CHECK(camera_control::PackedSelectorWord(ta, sa) !=
                  camera_control::PackedSelectorWord(tb, sb));
        }
    }
}

TEST_CASE("look at: an engine that packs it differently falsifies the derivation and never aims") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    SubjectSwap subject;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);

    // Selector 9 is this case's alone: Falsified is sticky by design, so it must not be a value
    // another case needs certified.
    g_engineSelectorSkew = 1;
    REQUIRE(c->SetMode(CameraMode::Pov, 9));
    g_engineSelectorSkew = 0;
    CHECK(camera_control::SelectorDerivationState(9) ==
          camera_control::SelectorDerivation::Falsified);

    const int poses = g_fakeWp.poses;
    replay::CommandedPose p;
    p.lookAt = true;
    p.lookAtSelector = 9;
    g_subject.point[0] = kStartPos[0] + 4.0f;
    CHECK_FALSE(c->CommandPose(p));
    CHECK(c->LastRefusal() == ControlRefusal::LookAtUnresolvable);
    CHECK(g_fakeWp.poses == poses);          // nothing was aimed
    CHECK(g_subject.userCalls == 0);         // the roster was not even asked
    CHECK(std::string(camera_control::Diagnostic()).find("not certified") != std::string::npos);

    // Sticky: a later agreeing read does not launder it.
    REQUIRE(c->SetMode(CameraMode::Pov, 9));
    CHECK(camera_control::SelectorDerivationState(9) ==
          camera_control::SelectorDerivation::Falsified);
}

TEST_CASE("look at: the camera ends up facing the subject the selector names") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    SubjectSwap subject;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);
    REQUIRE(c->SetMode(CameraMode::Pov, 3));   // certifies selector 3 against the engine

    // Off every axis and on none of the diagonals, so a swapped lane or a flipped sign moves the
    // forward vector somewhere this cannot accept.
    const float target[3] = {kStartPos[0] + 3.0f, kStartPos[1] + 2.0f, kStartPos[2] - 7.0f};
    memcpy(g_subject.point, target, sizeof target);

    replay::CommandedPose p;
    p.lookAt = true;
    p.lookAtSelector = 3;
    REQUIRE(c->CommandPose(p));
    CHECK(g_subject.lastSelector == 3);
    CHECK(g_subject.pointCalls == 1);

    // The pose that LANDED, scored against the direction it was asked to face -- not against the
    // yaw and pitch we computed, and not through DecomposeFacing.
    float fwd[3];
    ForwardOf(g_fakeWp.quat, fwd);
    float d[3] = {target[0] - kStartPos[0], target[1] - kStartPos[1], target[2] - kStartPos[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(fwd[i] - d[i] / len) < 1.0e-3f);

    // `cam look at` carries no position and must not invent one.
    for (int i = 0; i < 3; ++i) CHECK(g_fakeWp.pos[i] == kStartPos[i]);
}

TEST_CASE("look at: every way it cannot resolve refuses by its own name and aims at nothing") {
    camera::ResetPoseOwnerForTest();
    ReaderSwap reader;
    WritePointSwap wpSwap;
    Env& e = TheEnv();
    Rig r;
    g_fakeWp.rig = &r.rig;
    IReplayCameraControl* c = BindWithPose(e, r, kStartPos, kStartQuat, 0.96f);
    REQUIRE(c->SetMode(CameraMode::Pov, 4));

    replay::CommandedPose p;
    p.lookAt = true;
    p.lookAtSelector = 4;

    {   // No seam in this image at all. This is the shipping state until W1-E installs one, and it
        // is a named refusal rather than a camera that swings somewhere.
        const camera_control::SubjectPoseSeam* prev = camera_control::InstalledSubjectPose();
        camera_control::SetSubjectPose(nullptr);
        const int poses = g_fakeWp.poses;
        CHECK_FALSE(c->CommandPose(p));
        CHECK(c->LastRefusal() == ControlRefusal::LookAtUnresolvable);
        CHECK(std::string(camera_control::Diagnostic()).find("no subject-pose seam") != std::string::npos);
        CHECK(g_fakeWp.poses == poses);
        camera_control::SetSubjectPose(prev);
    }

    SubjectSwap subject;
    const camera_control::SubjectResolve refusals[3] = {
        camera_control::SubjectResolve::RosterUnreadable,
        camera_control::SubjectResolve::NoSuchSubject,
        camera_control::SubjectResolve::NoBody,
    };
    for (int i = 0; i < 3; ++i) {
        g_subject = FakeSubject{};
        g_subject.verdict = refusals[i];
        const int poses = g_fakeWp.poses;
        CHECK_FALSE(c->CommandPose(p));
        CHECK(c->LastRefusal() == ControlRefusal::LookAtUnresolvable);
        // Four causes, four sentences: the operator does something different with each.
        const std::string said(camera_control::Diagnostic());
        CAPTURE(said);
        CHECK(said.find(camera_control::SubjectResolveName(refusals[i])) != std::string::npos);
        CHECK(g_fakeWp.poses == poses);
    }

    {   // The user resolves and the body read fails: NoBody, from the second half of the seam.
        g_subject = FakeSubject{};
        g_subject.bodyOk = false;
        const int poses = g_fakeWp.poses;
        CHECK_FALSE(c->CommandPose(p));
        CHECK(c->LastRefusal() == ControlRefusal::LookAtUnresolvable);
        CHECK(g_subject.pointCalls == 1);
        CHECK(g_fakeWp.poses == poses);
    }

    {   // The camera is standing inside the subject. A normalised zero is a NaN facing; this is
        // ARCH-19 R4's FacingDegenerate and not LookAtUnresolvable, because it is a different act.
        g_subject = FakeSubject{};
        memcpy(g_subject.point, kStartPos, sizeof kStartPos);
        const int poses = g_fakeWp.poses;
        CHECK_FALSE(c->CommandPose(p));
        CHECK(c->LastRefusal() == ControlRefusal::FacingDegenerate);
        CHECK(g_fakeWp.poses == poses);
    }
}

TEST_CASE("the facing pair are inverses at a pitch that is not zero") {
    // Every landed round trip used pitch 0, where atan2(-fy, .) and atan2(+fy, .) agree -- the
    // fixture that could not fail. A relative `cam look 0 10` off a pitched view added its delta
    // to the NEGATIVE of the current pitch and aimed the camera somewhere nobody asked for.
    const float yaws[5]    = {0.0f, -0.0f, 37.0f, -120.0f, 175.0f};
    const float pitches[6] = {-40.0f, -15.0f, -0.0f, 0.0f, 15.0f, 40.0f};
    for (int a = 0; a < 5; ++a) {
        for (int b = 0; b < 6; ++b) {
            float q[4];
            camera_control::BuildFacing(yaws[a], pitches[b], q);
            float outYaw = 0.0f, outPitch = 0.0f;
            REQUIRE(camera_control::DecomposeFacing(q, &outYaw, &outPitch));
            CAPTURE(yaws[a]);
            CAPTURE(pitches[b]);
            CHECK(std::fabs(outYaw - yaws[a]) < 1.0e-3f);
            CHECK(std::fabs(outPitch - pitches[b]) < 1.0e-3f);
        }
    }

    // The identity facing must read back as +0 pitch and not -0. A tolerance cannot see that
    // sign, and a read-back sentence that says "-0.0" for a level camera is the same fault.
    float q0[4];
    camera_control::BuildFacing(0.0f, 0.0f, q0);
    float zeroYaw = 1.0f, zeroPitch = 1.0f;
    REQUIRE(camera_control::DecomposeFacing(q0, &zeroYaw, &zeroPitch));
    CHECK_FALSE(std::signbit(zeroPitch));
    CHECK_FALSE(std::signbit(zeroYaw));

    // The convention itself, stated as an assertion: positive pitch RAISES the view, so the
    // forward vector's Y is sin(pitch) and not its negative (ARCH-19 R5).
    float q[4];
    camera_control::BuildFacing(0.0f, 30.0f, q);
    float fwd[3];
    ForwardOf(q, fwd);
    CHECK(fwd[1] > 0.49f);
    CHECK(fwd[1] < 0.51f);

    // FacingToward is that inverse taken from a direction rather than from a quaternion, and the
    // two must land the same facing.
    const float from[3] = {1.0f, 2.0f, 3.0f};
    const float to[3]   = {4.0f, 5.5f, -2.0f};
    float yaw = 0.0f, pitch = 0.0f;
    REQUIRE(camera_control::FacingToward(from, to, &yaw, &pitch));
    camera_control::BuildFacing(yaw, pitch, q);
    ForwardOf(q, fwd);
    float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(fwd[i] - d[i] / len) < 1.0e-3f);
    CHECK_FALSE(camera_control::FacingToward(from, from, &yaw, &pitch));
}

// ---------------------------------------------------------------------------------------------
// W2-E-31c. The rig-state census line (ARCH-23-PAWN-PROVISION S14.10), which exists because the
// acceptance that preceded it could not fail correctly: "rig enabled flag %d -> %d" is a
// TRANSITION, emitted only while bound and only when the value moves, and Bind() re-baselines
// rigEnabled on every attempt -- so a rig already enabled at the first bind prints nothing and a
// working seat failed the clause, while "rig enabled flag 1" substring-matched the DISABLE line.

namespace {

// Verbatim from S14.10's L3 acceptance table. It is compiled here so that the anchoring is a
// property something asserts, not a property a report claims.
// Verbatim from S14.11's L3 acceptance table. It is compiled here so that the anchoring is a
// property something asserts, not a property a report claims. Note what it demands beyond
// S14.10's: p0a=0 (the RAW bit, healthy CLEAR), an instance count of at least one, a mode in
// range, and a POSE -- so the pattern accepts only a rig that a reader could actually fly.
const char* const kRigAcceptPattern =
    R"(^.*\[camera\] spectator rig global=1 \(enabled\), p0a=0, p0b=\?, bound=1, instances=[1-9][0-9]*, local=[0-9]+, mode=[0-6], freecam=[01], armed=[01], pose=-?[0-9.]+,-?[0-9.]+,-?[0-9.]+\s*$)";

// The marker channel W2-G-32a's `cam flight begin|end` verb drives, and the refusal words the run
// greps for. The refusal pattern names the PRINTED vocabulary: S14.11 spells the enum value
// FreeCamHoldsNode and its two neighbours in kebab in the same alternation, and only one of those
// two spellings is ever emitted.
const char* const kFlightMarkerPattern = R"(^.*\[camera\] flight window (begin|end)\s*$)";
const char* const kRefusalPattern =
    R"(.*\[camera\].*(free-cam-holds-node|write-point-refused|pose-not-owned).*)";

bool MatchesRigAccept(const std::string& logLine) {
    static const std::regex re(kRigAcceptPattern);
    return std::regex_match(logLine, re);
}

bool MatchesFlightMarker(const std::string& logLine) {
    static const std::regex re(kFlightMarkerPattern);
    return std::regex_match(logLine, re);
}

bool MatchesRefusal(const std::string& logLine) {
    static const std::regex re(kRefusalPattern);
    return std::regex_match(logLine, re);
}

// The line as a sink renders it, so the control is applied to the text an L3 grep actually reads.
std::string Rendered(const std::string& text) { return "12:34:56.789 INFO [camera] " + text; }

// Exactly the rig-state lines one census window produced. A window, not a tick: the counter is
// process-wide and every earlier case has already advanced its phase, but any 60 consecutive ticks
// carry exactly one emission whatever that phase is -- which is also the assertion that the line
// is neither missing nor per-frame.
std::vector<std::string> RigLinesOverOneWindow(Env& e, void* netGame) {
    const size_t from = e.log.lines.size();
    e.Ticks(netGame, 60);
    std::vector<std::string> out;
    for (size_t i = from; i < e.log.lines.size(); ++i) {
        const Line& l = e.log.lines[i];
        if (l.module == "camera" && l.text.compare(0, 14, "spectator rig ") == 0) out.push_back(l.text);
    }
    return out;
}

std::vector<std::string> CameraLinesStartingWith(Env& e, size_t from, const std::string& prefix) {
    std::vector<std::string> out;
    for (size_t i = from; i < e.log.lines.size(); ++i) {
        const Line& l = e.log.lines[i];
        if (l.module == "camera" && l.text.compare(0, prefix.size(), prefix) == 0) {
            out.push_back(l.text);
        }
    }
    return out;
}

// The engine dword the module reads, owned by the test so the readable renderings have inputs.
struct RigFlag {
    uint32_t value;
    explicit RigFlag(uint32_t v) : value(v) { camera_module::SetRigFlagForTest(&value); }
    ~RigFlag() { camera_module::SetRigFlagForTest(nullptr); }
};

// The tail every rendering carries, so a case that means to test `global` does not silently stop
// testing the other eight fields.
const char* const kUnboundTail =
    "p0a=?, p0b=?, bound=0, instances=?, local=?, mode=?, freecam=?, armed=?, pose=?";

// An address that is mapped by nothing. The `?` renderings must come from a read that was refused.
const void* Unmapped() { return reinterpret_cast<const void*>(static_cast<uintptr_t>(0x10)); }

}  // namespace

TEST_CASE("the rig-state line is emitted every window with no binding at all, and reads 0 and 1") {
    Env& e = TheEnv();
    e.Tick(nullptr);
    REQUIRE(camera_control::Get() == nullptr);

    {
        RigFlag flag(1);
        std::vector<std::string> lines = RigLinesOverOneWindow(e, nullptr);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == std::string("spectator rig global=1 (enabled), ") + kUnboundTail);
        // Unbound is not acceptance: the pattern wants an instance and a pose behind it.
        CHECK_FALSE(MatchesRigAccept(Rendered(lines[0])));
    }
    {
        RigFlag flag(0);
        std::vector<std::string> lines = RigLinesOverOneWindow(e, nullptr);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == std::string("spectator rig global=0 (disabled), ") + kUnboundTail);
        CHECK_FALSE(MatchesRigAccept(Rendered(lines[0])));
    }
}

TEST_CASE("an address that is not readable is a third answer and is neither 0 nor 1") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    // Non-null and mapped by nothing: the Readable() leg.
    camera_module::SetRigFlagForTest(Unmapped());
    std::vector<std::string> lines = RigLinesOverOneWindow(e, nullptr);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::string("spectator rig unreadable, ") + kUnboundTail);
    CHECK_FALSE(MatchesRigAccept(Rendered(lines[0])));

    // And the shipping resolution, unstubbed: CAM.SPECTATOR_ENABLED_GLOBAL is 34 MB past this
    // image's base, so buildguard refuses it and the same third answer is what an unmapped fact
    // produces. "Could not ask" is never spelled like an answer.
    camera_module::SetRigFlagForTest(nullptr);
    lines = RigLinesOverOneWindow(e, nullptr);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::string("spectator rig unreadable, ") + kUnboundTail);
}

TEST_CASE("bound, the line says so and tracks the global on both edges") {
    Env& e = TheEnv();
    Rig r;
    RigFlag flag(1);
    e.Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);

    // freecam=0 is the CI arm, and the CI is committed memory in this rig -- so the POSE reads
    // while "armed" does not: +0x9C lives in the view-state, whose reader is the shipping one and
    // refuses offline. Two fields, two answers, which is the point of giving them separate states.
    const char* const kBoundTail =
        "p0a=0, p0b=?, bound=1, instances=2, local=1, mode=0, freecam=0, armed=?, "
        "pose=0.000,0.000,0.000";

    std::vector<std::string> lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::string("spectator rig global=1 (enabled), ") + kBoundTail);

    flag.value = 0;
    lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::string("spectator rig global=0 (disabled), ") + kBoundTail);

    flag.value = 1;
    lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::string("spectator rig global=1 (enabled), ") + kBoundTail);

    e.Tick(nullptr);
}

TEST_CASE("the census line is read in its own window, never taken from the transition cache") {
    Env& e = TheEnv();
    Rig r;
    RigFlag flag(1);

    // Bind once with the flag set: Bind() is what baselines rigEnabled, to 1.
    e.Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);

    // Drop the binding and move the global. Nothing writes rigEnabled while unbound, so a line
    // rendered from that cache says enabled and a line that reads the flag says disabled.
    e.Tick(nullptr);
    REQUIRE(camera_control::Get() == nullptr);
    flag.value = 0;

    std::vector<std::string> lines = RigLinesOverOneWindow(e, nullptr);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::string("spectator rig global=0 (disabled), ") + kUnboundTail);
}

// ---------------------------------------------------------------------------------------------
// W2-E-32 (ARCH-23-PAWN-PROVISION S14.11). The five gates the census can see, the refusal that
// stops a half-changed mode, and the flight clause that can actually go red.

TEST_CASE("T3: p0a is the RAW bit and moves in BOTH directions on both arms") {
    Env& e = TheEnv();
    Rig r;
    RigFlag flag(1);
    e.Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);

    // Healthy: bit 2 CLEAR. The gate at Update:290-293 does not fire.
    r.netGame.sessionFlags = 0;
    std::vector<std::string> lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("p0a=0, p0b=?, bound=1") != std::string::npos);

    // The gate FIRING. Nothing else about the session changed, and no predicate is applied: the
    // line reports 1, which is the failing state, not a satisfied precondition spelled as one.
    r.netGame.sessionFlags = 4;
    lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("p0a=1, p0b=?, bound=1") != std::string::npos);
    CHECK_FALSE(MatchesRigAccept(Rendered(lines[0])));

    // Neighbouring bits are not bit 2.
    r.netGame.sessionFlags = 0xFFFFFFFBu;
    lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("p0a=0,") != std::string::npos);

    // Both edges on the UNREADABLE arm too, so no rendering of the line loses the field.
    r.netGame.sessionFlags = 4;
    camera_module::SetRigFlagForTest(Unmapped());
    lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].compare(0, 26, "spectator rig unreadable, ") == 0);
    CHECK(lines[0].find("p0a=1, p0b=?, bound=1") != std::string::npos);
    camera_module::SetRigFlagForTest(&flag.value);

    r.netGame.sessionFlags = 0;
    e.Tick(nullptr);
}

TEST_CASE("T4: every `?` comes from an address that was actually refused, and p0b is on every line") {
    Env& e = TheEnv();
    Rig r;
    RigFlag flag(1);
    e.Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);

    // The session-flags SLOT holds an address nothing maps: the second dereference is the one
    // that fails, which is exactly the shape a single guard would have missed.
    r.netGame.SetSessionFlagsPtr(static_cast<const uint32_t*>(Unmapped()));
    std::vector<std::string> lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("p0a=?, p0b=?,") != std::string::npos);
    r.netGame.SetSessionFlagsPtr(&r.netGame.sessionFlags);

    // The instance ARRAY at an unmapped address: the header still reads, so instances/local stay
    // numbers while mode and freecam -- which live in the CI -- become `?`.
    r.rig.SetInstanceBase(Unmapped());
    lines = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("instances=2, local=1, mode=?, freecam=?, armed=?, pose=?") !=
          std::string::npos);
    r.rig.SetInstanceBase(r.rig.instances);

    // p0b is constant by construction and on every line this file has produced.
    for (const std::string& l : CameraLinesStartingWith(e, 0, "spectator rig ")) {
        CHECK(l.find("p0b=?") != std::string::npos);
    }
    e.Tick(nullptr);
}

TEST_CASE("T1/T2: SetMode refuses FreeCamHoldsNode while ci+0x274 is set, and issues no call") {
    Env& e = TheEnv();
    Rig r;
    e.Tick(r.netGame.bytes);
    IReplayCameraControl* c = camera_control::Get();
    REQUIRE(c != nullptr);

    // T1. The engine's own SwitchTo* arms are exit-then-set; ours cannot call that exit, so the
    // only safe answer is to decline. Asserted on the SEAM'S CALL LOG, never on the return value:
    // a refusal that had already driven the engine would return false just the same.
    r.rig.SetFreeCam(1);
    const int before = g_engine.setModeCalls;
    CHECK_FALSE(c->SetMode(CameraMode::Pov, 3));
    CHECK(c->LastRefusal() == ControlRefusal::FreeCamHoldsNode);
    CHECK(g_engine.setModeCalls == before);
    CHECK(strcmp(replay::ControlRefusalName(ControlRefusal::FreeCamHoldsNode),
                 "free-cam-holds-node") == 0);
    CHECK(MatchesRefusal(Rendered("cam mode pov 3 refused: free-cam-holds-node")));
    CHECK_FALSE(MatchesRefusal(Rendered("cam mode pov 3 ok")));

    // The P4 contingency is itself a SetMode, so it is refused in this state too -- correct,
    // because in this state the operator is already flying.
    CHECK_FALSE(c->SetMode(CameraMode::Level, 0));
    CHECK(c->LastRefusal() == ControlRefusal::FreeCamHoldsNode);
    CHECK(g_engine.setModeCalls == before);

    // T2. The same fake with the flag clear issues exactly one call, with the arguments given.
    r.rig.SetFreeCam(0);
    CHECK(c->SetMode(CameraMode::Pov, 3));
    CHECK(g_engine.setModeCalls == before + 1);
    CHECK(g_engine.setModeMode == 0);
    CHECK(g_engine.setModeNum == 3);
    e.Tick(nullptr);
}

TEST_CASE("T5: two reads -- an integrator that advances moves the census pose, and one that does not does not") {
    Env& e = TheEnv();
    Rig r;
    RigFlag flag(1);
    ReaderSwap swap;
    g_fakeVs.integratorArmedKnown = true;
    g_fakeVs.integratorArmed = 1;
    g_fakeVs.pos[0] = 10.0f;
    g_fakeVs.pos[1] = 20.0f;
    g_fakeVs.pos[2] = 30.0f;
    r.rig.SetFreeCam(1);            // the view-state arm: ci+0x274 selects the source
    e.Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);

    std::vector<std::string> first = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(first.size() == 1);
    CHECK(first[0].find("freecam=1, armed=1, pose=10.000,20.000,30.000") != std::string::npos);
    CHECK(MatchesRigAccept(Rendered(first[0])));

    // Drive the integrator, exactly as the engine's wall-clock one would between two windows.
    g_fakeVs.pos[0] = 12.5f;
    std::vector<std::string> moved = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(moved.size() == 1);
    CHECK(moved[0].find("pose=12.500,20.000,30.000") != std::string::npos);
    CHECK(moved[0] != first[0]);

    // The companion, and it is the half that makes the case above mean something: with the
    // integrator NOT driven the very same code must print the very same pose.
    std::vector<std::string> still = RigLinesOverOneWindow(e, r.netGame.bytes);
    REQUIRE(still.size() == 1);
    CHECK(still[0] == moved[0]);

    r.rig.SetFreeCam(0);
    e.Tick(nullptr);
}

namespace {

// A canned window: `n` samples one second apart, each `step` metres further along X, all of them
// flying (poseOk, freecam, armed) unless the case says otherwise.
std::vector<camera_control::FlightSample> Canned(size_t n, double startSeconds, float step) {
    std::vector<camera_control::FlightSample> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i].seconds = startSeconds + static_cast<double>(i);
        out[i].pos[0]  = step * static_cast<float>(i);
        out[i].poseOk  = true;
        out[i].freeCam = true;
        out[i].armed   = true;
    }
    return out;
}

camera_control::FlightWindow WindowOver(const std::vector<camera_control::FlightSample>& v,
                                        double begin, double end) {
    camera_control::FlightWindow w;
    w.samples = v.data();
    w.count = v.size();
    w.haveBegin = true;
    w.haveEnd = true;
    w.beginSeconds = begin;
    w.endSeconds = end;
    return w;
}

}  // namespace

TEST_CASE("T6: the clause over four canned logs -- moving PASSes, still FAILs, and the MIXED one PASSes") {
    camera_control::FlightReport rep;

    // 1. All moving.
    std::vector<camera_control::FlightSample> moving = Canned(10, 100.0, 1.0f);
    CHECK(camera_control::EvaluateFlight(WindowOver(moving, 99.0, 110.0), &rep) ==
          camera_control::FlightVerdict::Pass);
    CHECK(rep.scored == 10);
    CHECK(rep.maxDeltaM > camera_control::kFlightMovedMetres);

    // 2. All static over more than three seconds and more than three samples: the only FAIL, and
    //    it is a UNIVERSAL -- no pair in the window moved.
    std::vector<camera_control::FlightSample> still = Canned(10, 100.0, 0.0f);
    CHECK(camera_control::EvaluateFlight(WindowOver(still, 99.0, 110.0), &rep) ==
          camera_control::FlightVerdict::Fail);
    CHECK(strcmp(rep.reason, "FreeCamPoseStatic") == 0);
    CHECK(rep.spanSeconds >= camera_control::kFlightStillSeconds);

    // 3. No markers at all -- NOT-RUN by its own name, never a pass and never a fail.
    camera_control::FlightWindow none;
    none.samples = moving.data();
    none.count = moving.size();
    CHECK(camera_control::EvaluateFlight(none, &rep) == camera_control::FlightVerdict::NotRun);
    CHECK(strcmp(rep.reason, "no marker channel") == 0);

    // 4. ★ THE REALISTIC ONE. Twenty seconds of flying, then two seconds stopped inside the SAME
    //    window -- what rotation_deccel 7.0 does every time the operator lets go of the keys.
    //    PASS, never FAIL: PASS is existential and FAIL universal, so a mid-window pause cannot
    //    redden a camera that is working.
    std::vector<camera_control::FlightSample> mixed = Canned(20, 100.0, 1.0f);
    for (int i = 0; i < 3; ++i) {
        camera_control::FlightSample held = mixed.back();
        held.seconds += 1.0;
        mixed.push_back(held);
    }
    CHECK(camera_control::EvaluateFlight(WindowOver(mixed, 99.0, 130.0), &rep) ==
          camera_control::FlightVerdict::Pass);

    // The two floors, each with its own reason, so a short still window is not scored at all.
    std::vector<camera_control::FlightSample> brief = Canned(2, 100.0, 0.0f);
    CHECK(camera_control::EvaluateFlight(WindowOver(brief, 99.0, 102.0), &rep) ==
          camera_control::FlightVerdict::NotRun);
    CHECK(strcmp(rep.reason, "still window shorter than 3 s") == 0);

    std::vector<camera_control::FlightSample> one = Canned(1, 100.0, 0.0f);
    CHECK(camera_control::EvaluateFlight(WindowOver(one, 99.0, 110.0), &rep) ==
          camera_control::FlightVerdict::NotRun);
    CHECK(strcmp(rep.reason, "fewer than 2 flying pose samples inside the window") == 0);

    // A window whose samples are not FLYING scores nothing: a camera in POV standing still is not
    // evidence about the free camera.
    std::vector<camera_control::FlightSample> notFlying = Canned(10, 100.0, 0.0f);
    for (camera_control::FlightSample& s : notFlying) s.freeCam = false;
    CHECK(camera_control::EvaluateFlight(WindowOver(notFlying, 99.0, 110.0), &rep) ==
          camera_control::FlightVerdict::NotRun);

    // A truncated window is refused rather than scored over the subset that survived: the FAIL is
    // a universal, so a subset could redden a window the whole of which would have passed.
    camera_control::FlightWindow lost = WindowOver(still, 99.0, 110.0);
    lost.truncated = true;
    CHECK(camera_control::EvaluateFlight(lost, &rep) == camera_control::FlightVerdict::NotRun);
    CHECK(strcmp(rep.reason, "window overflowed the sample ring") == 0);
}

TEST_CASE("T7: the markers SELECT the samples -- the same log scores three ways") {
    camera_control::FlightReport rep;

    // Twenty seconds of flying followed by ten seconds parked, in ONE log.
    std::vector<camera_control::FlightSample> log = Canned(20, 100.0, 1.0f);
    for (int i = 0; i < 10; ++i) {
        camera_control::FlightSample held = log.back();
        held.seconds += 1.0;
        log.push_back(held);
    }

    // Markers around the moving stretch.
    CHECK(camera_control::EvaluateFlight(WindowOver(log, 100.0, 119.0), &rep) ==
          camera_control::FlightVerdict::Pass);

    // The SAME samples with the markers moved to bracket only the static tail.
    CHECK(camera_control::EvaluateFlight(WindowOver(log, 120.0, 129.0), &rep) ==
          camera_control::FlightVerdict::Fail);
    CHECK(strcmp(rep.reason, "FreeCamPoseStatic") == 0);

    // Markers absent, and a begin without its end: two different could-not-asks.
    camera_control::FlightWindow none = WindowOver(log, 100.0, 119.0);
    none.haveBegin = false;
    none.haveEnd = false;
    CHECK(camera_control::EvaluateFlight(none, &rep) == camera_control::FlightVerdict::NotRun);
    CHECK(strcmp(rep.reason, "no marker channel") == 0);

    camera_control::FlightWindow half = WindowOver(log, 100.0, 119.0);
    half.haveEnd = false;
    CHECK(camera_control::EvaluateFlight(half, &rep) == camera_control::FlightVerdict::NotRun);
    CHECK(strcmp(rep.reason, "no window: one flight marker without its pair") == 0);
}

TEST_CASE("the marker channel: the verb queues, the module stamps and evaluates, nothing is lost silently") {
    Env& e = TheEnv();
    Rig r;
    RigFlag flag(1);
    ReaderSwap swap;
    g_fakeVs.integratorArmedKnown = true;
    g_fakeVs.integratorArmed = 1;
    g_fakeVs.pos[0] = 0.0f;
    r.rig.SetFreeCam(1);
    e.Tick(r.netGame.bytes);
    REQUIRE(camera_control::Get() != nullptr);

    const unsigned long long droppedBefore = camera_control::FlightMarksDropped();
    const size_t from = e.log.lines.size();

    camera_control::MarkFlightWindow(true);
    e.Tick(r.netGame.bytes);
    for (int i = 0; i < 4; ++i) {
        g_fakeVs.pos[0] += 5.0f;
        e.Ticks(r.netGame.bytes, 60);
    }
    camera_control::MarkFlightWindow(false);
    e.Tick(r.netGame.bytes);

    std::vector<std::string> markers = CameraLinesStartingWith(e, from, "flight window ");
    REQUIRE(markers.size() >= 3);
    CHECK(markers.front() == "flight window begin");
    CHECK(MatchesFlightMarker(Rendered("flight window begin")));
    CHECK(MatchesFlightMarker(Rendered("flight window end")));
    CHECK_FALSE(MatchesFlightMarker(Rendered("flight window begin now")));
    CHECK_FALSE(MatchesFlightMarker(Rendered("flight window verdict PASS: moved")));

    const std::string& verdict = markers.back();
    CHECK(verdict.compare(0, 24, "flight window verdict PA") == 0);
    CHECK(verdict.find("marks dropped 0") != std::string::npos);
    CHECK(camera_control::FlightMarksDropped() == droppedBefore);

    // The queue is finite, and a mark that does not fit must SAY SO to its
    // caller -- the verb's reply is the operator's only channel, and "the window is open" for a
    // marker that was never stamped is the clause scoring samples nobody bracketed.
    for (int i = 0; i < 8; ++i) CHECK(camera_control::MarkFlightWindow(i % 2 == 0));
    CHECK(camera_control::FlightMarksDropped() == droppedBefore);   // eight fit exactly
    CHECK_FALSE(camera_control::MarkFlightWindow(true));            // the ninth does not
    CHECK(camera_control::FlightMarksDropped() == droppedBefore + 1);
    CHECK_FALSE(camera_control::MarkFlightWindow(false));
    CHECK(camera_control::FlightMarksDropped() == droppedBefore + 2);
    CHECK(strcmp(camera_control::FlightMarkerDroppedText(),
                 "flight marker dropped (queue full)") == 0);
    // The drop is not the marker: a reader greping for a window must not find one here.
    CHECK_FALSE(MatchesFlightMarker(Rendered(camera_control::FlightMarkerDroppedText())));
    e.Ticks(r.netGame.bytes, 2);
    bool leftover = false;
    CHECK_FALSE(camera_control::TakeFlightMark(&leftover));

    r.rig.SetFreeCam(0);
    e.Tick(nullptr);
}

TEST_CASE("the acceptance pattern is whole-line anchored, so no partial rendering can satisfy it") {
    CHECK(MatchesRigAccept(Rendered(
        "spectator rig global=1 (enabled), p0a=0, p0b=?, bound=1, instances=1, local=0, mode=5, "
        "freecam=1, armed=1, pose=1.000,-2.500,3.000")));

    // The transition line S14.10 replaced, in both directions. "rig enabled flag 1" is a substring
    // of the DISABLE line, which is the false pass AUD-S14 F3 found inside the old acceptance.
    CHECK_FALSE(MatchesRigAccept(Rendered("rig enabled flag 1 -> 0")));
    CHECK_FALSE(MatchesRigAccept(Rendered("rig enabled flag 0 -> 1")));

    // S14.10's own line, which this one supersedes: it is a PREFIX of the accepted rendering and
    // would substring-match anything keyed on the old words.
    CHECK_FALSE(MatchesRigAccept(Rendered("spectator rig global=1 (enabled), bound=1")));

    // Each field on its own, every one of them a failing input the pattern must refuse.
    const std::string kOk =
        "spectator rig global=1 (enabled), p0a=0, p0b=?, bound=1, instances=1, local=0, mode=5, "
        "freecam=1, armed=1, pose=1.000,-2.500,3.000";
    struct Bad { const char* from; const char* to; };
    const Bad bad[] = {
        {"global=1 (enabled)", "global=0 (disabled)"},   // the rig is off
        {"p0a=0", "p0a=1"},                              // Update returns at :290-293
        {"p0a=0", "p0a=?"},                              // could not ask is not a pass
        {"p0b=?", "p0b=0"},                              // a gate nobody can read, answered
        {"bound=1", "bound=0"},
        {"instances=1", "instances=0"},                  // P2: no instance exists
        {"instances=1", "instances=?"},
        {"local=0", "local=?"},
        {"mode=5", "mode=7"},
        {"mode=5", "mode=?"},
        {"freecam=1", "freecam=?"},
        {"armed=1", "armed=?"},
        {"pose=1.000,-2.500,3.000", "pose=?"},
        {"pose=1.000,-2.500,3.000", "pose=1.000,-2.500"},
    };
    for (const Bad& b : bad) {
        std::string line = kOk;
        const size_t at = line.find(b.from);
        REQUIRE(at != std::string::npos);
        line.replace(at, strlen(b.from), b.to);
        CHECK_FALSE(MatchesRigAccept(Rendered(line)));
    }

    // The unreadable arm carries the same tail and must still be refused.
    CHECK_FALSE(MatchesRigAccept(Rendered(
        "spectator rig unreadable, p0a=0, p0b=?, bound=1, instances=1, local=0, mode=5, "
        "freecam=1, armed=1, pose=1.000,-2.500,3.000")));

    // A foreign module saying the same words, and trailing text after the last anchor.
    CHECK_FALSE(MatchesRigAccept("12:34:56.789 INFO [identity] " + kOk));
    CHECK_FALSE(MatchesRigAccept(Rendered(kOk + " then released")));
}
