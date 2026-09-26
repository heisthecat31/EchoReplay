// W1-D's offline checks: the slot-selection decision table, the stage-index filter, and the frame
// continuity arithmetic that the acceptance criterion is stated in.
//
// These run against src/engine/tick_policy.cpp -- the shipped code -- not a reimplementation of it.
// What they cannot reach is anything that needs a live CR15Game; that is queued as a probe in
// runs/queue/W1-D.probe.json and listed in docs/spikes/W1-D.md S8.

#include <chrono>
#include <string>

#include <doctest.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "engine/tick_policy.h"
#include "generated/facts_generated.h"

using replay::tick::AuditFrame;
using replay::tick::ChooseFreeSlot;
using replay::tick::CommandLineHasFlag;
using replay::tick::FrameAudit;
using replay::tick::IsTickStage;
using replay::tick::kEngineStagesPerFrame;
using replay::tick::SlotChoice;
using replay::tick::SlotChoiceOffset;

namespace {

constexpr bool kFree = false;
constexpr bool kTaken = true;

}  // namespace

TEST_CASE("the free slot is probed, and -noovr flips which one it is") {
    // Launched WITHOUT -noovr: CR15Game::Initialize created pnsovr into +0x8500 and pnsrad into
    // +0x8508; pnsdemo was never created, so +0x8510 is the free one.
    CHECK(ChooseFreeSlot(kTaken, kFree) == SlotChoice::ClaimC);
    CHECK(SlotChoiceOffset(ChooseFreeSlot(kTaken, kFree)) == facts::GAME_PROVIDER_SLOT_C);

    // Launched WITH -noovr, which is what our own desktop viewer passes: bit 27 of CR15Game+0x7AE0 is
    // set, pnsdemo went into +0x8510, and +0x8500 is the free one. A hard-coded offset gets exactly
    // this case wrong, which is why the answer differs here and nowhere in the code assumes one.
    CHECK(ChooseFreeSlot(kFree, kTaken) == SlotChoice::ClaimA);
    CHECK(SlotChoiceOffset(ChooseFreeSlot(kFree, kTaken)) == facts::GAME_PROVIDER_SLOT_A);

    // The two answers must actually differ, or the test above would pass on a function that ignored
    // its arguments.
    CHECK(SlotChoiceOffset(ChooseFreeSlot(kTaken, kFree)) !=
          SlotChoiceOffset(ChooseFreeSlot(kFree, kTaken)));
}

TEST_CASE("a slot shape we did not verify aborts instead of guessing") {
    // Both free: the provider block did not run at all. The design says abort, and it
    // matters -- writing into a slot in a process where CNSProvider::Create never ran means the engine
    // never validated anything about that handle.
    CHECK(ChooseFreeSlot(kFree, kFree) == SlotChoice::AbortBothFree);
    CHECK(SlotChoiceOffset(ChooseFreeSlot(kFree, kFree)) == 0);

    // Both taken: three of three occupied. Either another plugin claimed the free slot first, or the
    // two-of-three model is wrong for this configuration. Overwriting would evict a real provider.
    CHECK(ChooseFreeSlot(kTaken, kTaken) == SlotChoice::AbortBothTaken);
    CHECK(SlotChoiceOffset(ChooseFreeSlot(kTaken, kTaken)) == 0);
}

TEST_CASE("the stage-index filter yields exactly one tick per frame, not four") {
    // The engine calls CR15NetGame::Update once per stage, 0..3, every frame. This is the shape of the
    // bug: an implementation that treats the second argument as a delta ticks on all four.
    constexpr uint64_t kFrames = 600;
    uint64_t ticked = 0;
    uint64_t offered = 0;
    for (uint64_t frame = 0; frame < kFrames; ++frame) {
        for (uint64_t stage = 0; stage < kEngineStagesPerFrame; ++stage) {
            ++offered;
            if (IsTickStage(stage)) {
                ++ticked;
            }
        }
    }
    CHECK(offered == kFrames * kEngineStagesPerFrame);
    CHECK(ticked == kFrames);

    // The check above only means something if an unfiltered implementation would give a different
    // number. It would give four times as many, and that difference is the whole finding.
    CHECK(offered != ticked);
    CHECK(offered == ticked * kEngineStagesPerFrame);
}

TEST_CASE("frame continuity: the healthy case is clean") {
    FrameAudit audit{};
    // CncaGame+0x830 as our stage-0 tick sees it: one higher each frame, because Render increments it
    // after we run. The starting value is arbitrary -- we join a session already in progress.
    constexpr uint64_t kStart = 918273;
    constexpr uint64_t kFrames = 3600;
    for (uint64_t i = 0; i < kFrames; ++i) {
        AuditFrame(&audit, kStart + i, 16666ull * (i + 1));
    }
    CHECK(audit.ticks == kFrames);
    CHECK(audit.first == kStart);
    CHECK(audit.last == kStart + kFrames - 1);
    CHECK(audit.duplicates == 0);
    CHECK(audit.skips == 0);
}

TEST_CASE("frame continuity: ticking four times a frame is caught") {
    // What the fallback would look like without the stage filter: four ticks against one frame counter
    // value. Three of every four see no advance.
    FrameAudit audit{};
    constexpr uint64_t kFrames = 100;
    for (uint64_t i = 0; i < kFrames; ++i) {
        for (uint64_t stage = 0; stage < kEngineStagesPerFrame; ++stage) {
            AuditFrame(&audit, i, 4166ull * (i * kEngineStagesPerFrame + stage + 1));
        }
    }
    CHECK(audit.ticks == kFrames * kEngineStagesPerFrame);
    CHECK(audit.duplicates == kFrames * (kEngineStagesPerFrame - 1));
    CHECK(audit.skips == 0);
}

TEST_CASE("frame continuity: a missed frame is caught, and counted by how many") {
    FrameAudit audit{};
    AuditFrame(&audit, 10, 100);
    AuditFrame(&audit, 11, 200);
    AuditFrame(&audit, 15, 300);  // four frames rendered, one tick delivered: three went by without us  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF 300 us on this fixture's frame clock, not the extrapolation cap
    AuditFrame(&audit, 16, 400);
    CHECK(audit.ticks == 4);
    CHECK(audit.duplicates == 0);
    CHECK(audit.skips == 3);
}

TEST_CASE("frame continuity: the first tick sets the baseline and is never a skip") {
    FrameAudit audit{};
    CHECK(audit.started == false);
    AuditFrame(&audit, 5000000, 12345);
    CHECK(audit.started == true);
    CHECK(audit.ticks == 1);
    CHECK(audit.skips == 0);
    CHECK(audit.duplicates == 0);
    CHECK(audit.first == 5000000);
}

TEST_CASE("frame continuity survives the counter wrapping") {
    // The engine's counter is a u64 `inc`, so this is unreachable in practice -- but the audit does
    // unsigned subtraction and the test pins that it is deliberate rather than an accident waiting to
    // report 18 quintillion skipped frames.
    FrameAudit audit{};
    AuditFrame(&audit, 0xFFFFFFFFFFFFFFFEull, 10);
    AuditFrame(&audit, 0xFFFFFFFFFFFFFFFFull, 20);
    AuditFrame(&audit, 0ull, 30);
    AuditFrame(&audit, 1ull, 40);
    CHECK(audit.ticks == 4);
    CHECK(audit.duplicates == 0);
    CHECK(audit.skips == 0);
}

TEST_CASE("the three provider slots are distinct, ordered and pointer-aligned") {
    // A bad merge into facts.tsv that collapsed two of these would make the probe claim a slot the
    // engine is already using, which is the one failure this packet must never have.
    CHECK(facts::GAME_PROVIDER_SLOT_A < facts::GAME_PROVIDER_SLOT_B);
    CHECK(facts::GAME_PROVIDER_SLOT_B < facts::GAME_PROVIDER_SLOT_C);
    CHECK(facts::GAME_PROVIDER_SLOT_C < facts::GAME_NETGAME_PTR_OFF);
    CHECK((facts::GAME_PROVIDER_SLOT_A % sizeof(void*)) == 0);
    CHECK((facts::GAME_PROVIDER_SLOT_B % sizeof(void*)) == 0);
    CHECK((facts::GAME_PROVIDER_SLOT_C % sizeof(void*)) == 0);
    // Consecutive qwords: the three handles are adjacent fields, which is how the census read them.
    CHECK(facts::GAME_PROVIDER_SLOT_B - facts::GAME_PROVIDER_SLOT_A == sizeof(void*));
    CHECK(facts::GAME_PROVIDER_SLOT_C - facts::GAME_PROVIDER_SLOT_B == sizeof(void*));
}

TEST_CASE("the -noovr derivation matches the token the engine searches for") {
    // The engine sets bit 27 of CR15Game+0x7AE0 only when its own command-line search for the literal
    // "-noovr" hits, and that bit is the whole of what decides which provider slot stays free. A probe
    // that predicts the free slot has to get this exactly right, or a correct claim looks like a defect.
    CHECK(CommandLineHasFlag("echovr.exe -noovr", "-noovr"));
    CHECK(CommandLineHasFlag("echovr.exe -noovr -spectatorstream echo_arena", "-noovr"));
    CHECK(CommandLineHasFlag("echovr.exe -level mpl_arena_a -noovr", "-noovr"));
    CHECK(CommandLineHasFlag("-noovr", "-noovr"));

    // bare-level: the profile the matrix now depends on. No -noovr, so bit 27 stays clear, pnsovr takes
    // +0x8500 and the free slot is +0x8510.
    CHECK_FALSE(CommandLineHasFlag("echovr.exe -level mpl_arena_a", "-noovr"));
    CHECK_FALSE(CommandLineHasFlag("echovr.exe", "-noovr"));
    CHECK_FALSE(CommandLineHasFlag("", "-noovr"));
    CHECK_FALSE(CommandLineHasFlag(nullptr, "-noovr"));

    // A token, not a substring. Both of these would flip the prediction to the wrong slot, and the
    // probe would then assert against the wrong expectation and blame the claim.
    CHECK_FALSE(CommandLineHasFlag("echovr.exe -noovrfoo", "-noovr"));
    CHECK_FALSE(CommandLineHasFlag("echovr.exe --noovr", "-noovr"));
    CHECK_FALSE(CommandLineHasFlag("echovr.exe x-noovr", "-noovr"));

    // Quoted and tab-separated forms are still the token.
    CHECK(CommandLineHasFlag("\"C:\\echovr.exe\" -noovr", "-noovr"));
    CHECK(CommandLineHasFlag("echovr.exe\t-noovr\tmore", "-noovr"));

    // A flag appearing inside a path must not count, which is the case a plain strstr gets wrong.
    CHECK_FALSE(CommandLineHasFlag("C:\\games\\-noovr-build\\echovr.exe", "-noovr"));
}

TEST_CASE("the predicted free slot is the opposite one in the two configurations") {
    // This is the property the in-vivo run tests, stated here so the offline half is not vacuous: the
    // prediction must actually differ between the two command lines, or asserting on it proves nothing.
    const bool flat = CommandLineHasFlag("echovr.exe -noovr -spectatorstream echo_arena", "-noovr");
    const bool bareLevel = CommandLineHasFlag("echovr.exe -level mpl_arena_a", "-noovr");
    CHECK(flat != bareLevel);
    const uint32_t flatSlot = flat ? facts::GAME_PROVIDER_SLOT_A : facts::GAME_PROVIDER_SLOT_C;
    const uint32_t bareLevelSlot = bareLevel ? facts::GAME_PROVIDER_SLOT_A : facts::GAME_PROVIDER_SLOT_C;
    CHECK(flatSlot == facts::GAME_PROVIDER_SLOT_A);
    CHECK(bareLevelSlot == facts::GAME_PROVIDER_SLOT_C);
    CHECK(flatSlot != bareLevelSlot);
}

TEST_CASE("the frame delta comes from sim time, not from the delta the engine hands us") {
    // ★ The engine writes CClock+104 at least three times per frame and TWO of those beats run
    // AFTER our stage-0 tick, so what CNSProvider::Update passes us is one beat, not a frame. CClock
    // +0x28 accumulates every beat, so its advance between consecutive ticks is the frame. This is the
    // arithmetic every Wave-2 consumer depends on, which is why it is pinned here.
    FrameAudit audit{};

    // A 60 Hz frame split as the engine splits it: 12000 us before our tick, then 2000 + 2666 after.
    // Our tick would be handed 12000 and must report a frame of 16666.
    uint64_t sim = 0;
    AuditFrame(&audit, 100, sim += 12000);
    CHECK(audit.frameDeltaUs == 0);  // the first tick has no previous

    sim += 2000 + 2666;              // beats 2 and 3 of frame 100
    AuditFrame(&audit, 101, sim += 12000);
    CHECK(audit.frameDeltaUs == 16666);

    sim += 2000 + 2666;
    AuditFrame(&audit, 102, sim += 12000);
    CHECK(audit.frameDeltaUs == 16666);

    CHECK(audit.simRegressions == 0);
    // The point of the whole exercise: the frame delta is NOT the value we were handed.
    CHECK(audit.frameDeltaUs != 12000);
}

TEST_CASE("sim time running backwards is counted, not turned into a huge frame delta") {
    // Unsigned subtraction on a regressed clock would report about 18 quintillion microseconds and
    // every consumer downstream would act on it. It must be visible instead.
    FrameAudit audit{};
    AuditFrame(&audit, 1, 1000);
    AuditFrame(&audit, 2, 2000);
    CHECK(audit.frameDeltaUs == 1000);
    AuditFrame(&audit, 3, 500);
    CHECK(audit.simRegressions == 1);
    CHECK(audit.frameDeltaUs == 0);
    // and it recovers cleanly from the new baseline
    AuditFrame(&audit, 4, 1500);
    CHECK(audit.frameDeltaUs == 1000);
    CHECK(audit.simRegressions == 1);
}

TEST_CASE("ClaimMain's resolution is decided and published by the policy, not by the test") {
    // Every assertion below runs the SHIPPED resolution -- tick::PlanMode and tick::ResolveAndPublishMode
    // are the two calls ClaimMain makes, in ClaimMain's order. What tick.cpp still owns is the engine:
    // the poll loop, the singleton lookup, CreateThread and InstallFallback. Nothing here reimplements
    // the decision, so a mutation of the decision has nowhere to hide.
    using namespace replay;
    using replay::config::RowDeclared;
    config::ResetForTest();

    // The engine facts, as ClaimMain would have measured them on the fallback path.
    tick::ClaimOutcome claimed{};
    claimed.slotClaimed = true;
    tick::ClaimOutcome fellBack{};
    fellBack.gameResolved = true;

    SUBCASE("the request is read as a request: asking does not close the publish window") {
        const tick::ModePlan plan = tick::PlanMode();
        REQUIRE(plan.requested != nullptr);
        CHECK(std::string(plan.requested) == "auto");
        CHECK(plan.slotAllowed);
        CHECK(plan.hookAllowed);
        CHECK_FALSE(RowDeclared("tick.mode"));

        // If PlanMode read through GetString instead, this publish would be refused and row 1 would
        // stay undeclared -- the packet would compile, ship, and leave the fallback dead at the gate.
        CHECK(tick::ResolveAndPublishMode(plan, fellBack) == tick::EffectiveMode::Hook);
        CHECK(std::string(config::Interface()->GetString("tick.mode", "auto")) == "hook");
        CHECK(RowDeclared("tick.mode"));
    }

    SUBCASE("auto that claimed the slot resolves to slot, and must not publish hook") {
        const tick::ModePlan plan = tick::PlanMode();
        CHECK(tick::ResolveAndPublishMode(plan, claimed) == tick::EffectiveMode::Slot);
        CHECK(std::string(config::Interface()->GetString("tick.mode", "auto")) == "slot");
        // The slot arm's "row 1 is not declared" is an assertion, not an absence: the row was published,
        // and what was published names no detour.
        CHECK_FALSE(RowDeclared("tick.mode"));
    }

    SUBCASE("auto that fell back resolves to hook, which is what opens hookmgr's gate") {
        const tick::ModePlan plan = tick::PlanMode();
        CHECK(tick::ResolveAndPublishMode(plan, fellBack) == tick::EffectiveMode::Hook);
        CHECK(std::string(config::Interface()->GetString("tick.mode", "auto")) == "hook");
        CHECK(RowDeclared("tick.mode"));
    }

    SUBCASE("an explicit hook skips the claim and reaches the hook path") {
        const char* asked = "{\"tick.mode\": \"hook\"}";
        REQUIRE(config::LoadFromTextForTest(asked, std::strlen(asked)));
        const tick::ModePlan plan = tick::PlanMode();
        CHECK(std::string(plan.requested) == "hook");
        CHECK_FALSE(plan.slotAllowed);  // ClaimMain never calls RunClaim, so the outcome is not Claimed
        CHECK(plan.hookAllowed);
        CHECK_FALSE(RowDeclared("tick.mode"));  // the file's value is a request, not a declaration

        CHECK(tick::ResolveAndPublishMode(plan, fellBack) == tick::EffectiveMode::Hook);
        CHECK(RowDeclared("tick.mode"));
    }

    SUBCASE("an explicit slot never resolves to hook, however the claim went") {
        const char* asked = "{\"tick.mode\": \"slot\"}";
        REQUIRE(config::LoadFromTextForTest(asked, std::strlen(asked)));
        const tick::ModePlan plan = tick::PlanMode();
        CHECK(plan.slotAllowed);
        CHECK_FALSE(plan.hookAllowed);
        CHECK(tick::ResolveAndPublishMode(plan, fellBack) == tick::EffectiveMode::None);
        CHECK_FALSE(RowDeclared("tick.mode"));
    }

    SUBCASE("the slot claim disabled by its own row leaves the fallback allowed") {
        const char* asked = "{\"tick.slot_claim\": false}";
        REQUIRE(config::LoadFromTextForTest(asked, std::strlen(asked)));
        const tick::ModePlan plan = tick::PlanMode();
        CHECK_FALSE(plan.slotAllowed);
        CHECK(plan.hookAllowed);
        CHECK(tick::ResolveAndPublishMode(plan, fellBack) == tick::EffectiveMode::Hook);
        CHECK(RowDeclared("tick.mode"));
    }

    SUBCASE("no effective mode is published when nothing ticks") {
        // Three states with no answer: shutting down, no CR15Game singleton, and the fallback refused
        // by the request. Publishing a mode here would be a confident answer to missing data, and
        // SC7's row 1 is meant to stay deferred.
        tick::ClaimOutcome stopping{};
        stopping.stopping = true;
        stopping.gameResolved = true;
        tick::ClaimOutcome noGame{};

        const tick::ModePlan plan = tick::PlanMode();
        CHECK(tick::ResolveAndPublishMode(plan, stopping) == tick::EffectiveMode::None);
        CHECK(tick::ResolveAndPublishMode(plan, noGame) == tick::EffectiveMode::None);
        CHECK(config::RowDeclarationState("tick.mode") == config::RowState::Unresolved);
    }

    SUBCASE("an ordinary read in the resolver's place refuses both resolutions") {
        // The positive form of the GetString mutation: this is what PlanMode must never do.
        (void)config::Interface()->GetString("tick.mode", "auto");
        const tick::ModePlan plan = tick::PlanMode();
        CHECK(tick::ResolveAndPublishMode(plan, claimed) == tick::EffectiveMode::Slot);
        CHECK_FALSE(RowDeclared("tick.mode"));
        CHECK(tick::ResolveAndPublishMode(plan, fellBack) == tick::EffectiveMode::Hook);
        CHECK_FALSE(RowDeclared("tick.mode"));
    }

    SUBCASE("the published words are the ones S4.4 declares for the key") {
        CHECK(std::string(tick::EffectiveModeName(tick::EffectiveMode::Slot)) == "slot");
        CHECK(std::string(tick::EffectiveModeName(tick::EffectiveMode::Hook)) == "hook");
    }

    config::ResetForTest();
}

namespace {

// The resolver stands in for buildguard::MappedAddress -- the header walk itself, two VirtualQuery per
// call. Counting its calls counts them.
const void* CountingResolver(void* ctx) {
    ++*static_cast<unsigned*>(ctx);
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(0x140ABCD0));
}

const void* FailingResolver(void* ctx) {
    ++*static_cast<unsigned*>(ctx);
    return nullptr;
}

double NanosPerCall(std::chrono::steady_clock::time_point t0,
                    std::chrono::steady_clock::time_point t1, int iterations) {
    const double ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return ns / static_cast<double>(iterations);
}

}  // namespace

TEST_CASE("the clock cell is resolved once, so the null tick path walks no PE header after the first") {
    // AUD-P17 finding 3: GlobalClock() called MappedAddress on every dispatch while the clock was
    // null, and MappedAddress is Readable(dos) + Readable(nt), each a VirtualQuery loop.
    unsigned calls = 0;
    replay::tick::ImageCellCache cache;

    SUBCASE("the first ask does resolve, so the counter is not stuck at zero") {
        const void* first = cache.Get(&CountingResolver, &calls);
        CHECK(first != nullptr);
        CHECK(calls == 1u);
        CHECK(cache.Resolved());
    }

    SUBCASE("ten thousand further asks resolve nothing and answer the same cell") {
        const void* cell = cache.Get(&CountingResolver, &calls);
        REQUIRE(cell != nullptr);
        REQUIRE(calls == 1u);
        unsigned differed = 0;
        for (int i = 0; i < 10000; ++i) {
            if (cache.Get(&CountingResolver, &calls) != cell) {
                ++differed;
            }
        }
        CHECK(differed == 0u);
        CHECK(calls == 1u);
    }

    SUBCASE("a cell that does not resolve is NOT cached, or the horizon could never be reached") {
        CHECK(cache.Get(&FailingResolver, &calls) == nullptr);
        CHECK(cache.Get(&FailingResolver, &calls) == nullptr);
        CHECK(calls == 2u);
        CHECK_FALSE(cache.Resolved());
    }
}

TEST_CASE("the clock re-ask backs off after the first second and never goes silent") {
    using replay::tick::ClockBindHorizonPassed;
    using replay::tick::kClockBindHorizonMs;
    using replay::tick::kClockReaskEveryNth;
    using replay::tick::kClockReaskWarmMs;
    using replay::tick::ShouldReaskClock;

    SUBCASE("every dispatch inside the first second, so a late bind is logged to the frame") {
        unsigned asked = 0;
        for (uint64_t i = 0; i < 2000; ++i) {
            if (ShouldReaskClock(i, i / 4)) {  // 2000 dispatches spread over 500 ms
                ++asked;
            }
        }
        CHECK(asked == 2000u);
    }

    SUBCASE("one dispatch in kClockReaskEveryNth after it -- the ratio, not merely 'fewer'") {
        unsigned asked = 0;
        const uint64_t dispatches = kClockReaskEveryNth * 16;
        for (uint64_t i = 0; i < dispatches; ++i) {
            if (ShouldReaskClock(i, 5000)) {
                ++asked;
            }
        }
        CHECK(asked == 16u);
        CHECK(static_cast<uint64_t>(asked) * kClockReaskEveryNth == dispatches);
    }

    SUBCASE("the boundary is the first second exactly") {
        CHECK(ShouldReaskClock(1, kClockReaskWarmMs - 1));
        CHECK_FALSE(ShouldReaskClock(1, kClockReaskWarmMs));
    }

    SUBCASE("it never stops re-asking, however long the process runs") {
        // A backoff that reached zero would be a silent 'no clock for the rest of this launch'.
        CHECK(ShouldReaskClock(kClockReaskEveryNth * 1000000u, 86400000u));
    }

    SUBCASE("the never-binds horizon has a named edge") {
        CHECK_FALSE(ClockBindHorizonPassed(kClockBindHorizonMs - 1));
        CHECK(ClockBindHorizonPassed(kClockBindHorizonMs));
    }
}

TEST_CASE("the null tick path costs no PE-header walk, measured against the walk it replaced") {
    // The bound is taken from THIS run's measurement of buildguard::MappedAddress rather than from a
    // magic constant, so it means the same thing on a slower machine and any VirtualQuery returning to
    // the null path fails it. MappedAddress here walks our own test image's headers -- both Readable
    // calls run -- and then refuses the rebased address, which is the same work the game path does.
    constexpr int kProbeIters = 20000;
    constexpr int kPathIters = 2000000;

    volatile uintptr_t sink = 0;
    for (int i = 0; i < kProbeIters / 20; ++i) {
        sink += reinterpret_cast<uintptr_t>(replay::buildguard::MappedAddress(facts::CLOCK_GLOBAL_PTR));
    }
    const std::chrono::steady_clock::time_point p0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kProbeIters; ++i) {
        sink += reinterpret_cast<uintptr_t>(replay::buildguard::MappedAddress(facts::CLOCK_GLOBAL_PTR));
    }
    const double probeNs = NanosPerCall(p0, std::chrono::steady_clock::now(), kProbeIters);

    unsigned calls = 0;
    replay::tick::ImageCellCache cache;
    REQUIRE(cache.Get(&CountingResolver, &calls) != nullptr);
    REQUIRE(calls == 1u);

    const uint64_t probesBefore = replay::ReadableProbeCount();
    const std::chrono::steady_clock::time_point c0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPathIters; ++i) {
        sink += reinterpret_cast<uintptr_t>(cache.Get(&CountingResolver, &calls));
        if (replay::tick::ShouldReaskClock(static_cast<uint64_t>(i), 5000)) {
            sink += 1;
        }
    }
    const double pathNs = NanosPerCall(c0, std::chrono::steady_clock::now(), kPathIters);
    const uint64_t probesAfter = replay::ReadableProbeCount();

    MESSAGE("one buildguard::MappedAddress (the two VirtualQuery the re-ask used to pay per frame): "
            << probeNs << " ns; the cached null tick path incl. the backoff predicate: " << pathNs
            << " ns over " << kPathIters << " dispatches");

    CHECK(sink != 0u);
    CHECK(probeNs > 0.0);
    // The packet's stated budget for the whole null path.
    CHECK(pathNs < 1000.0);
    // And the form that actually detects a probe coming back. This used to be a ratio against
    // MappedAddress, which worked while a probe was a 100 us syscall; PERF-4 made it a page touch,
    // so the ratio now compares two sub-nanosecond numbers and means nothing. The count does not
    // depend on what a probe costs.
    CHECK(probesAfter == probesBefore);
    // Two million dispatches, still one resolve.
    CHECK(calls == 1u);
}
