#include "doctest.h"

#include <string.h>

#include <set>
#include <string>
#include <type_traits>

#include "import/basis.h"
#include "import/convert.h"
#include "import/emit.h"
#include "import/import_facts.h"
#include "import/inferred.h"
#include "import/reader.h"
#include "import/report.h"
#include "import/result.h"
#include "import/sourcemodel.h"

using namespace replay::import;

// Flat vectors of these are the whole model; a non-trivial member would bring per-frame allocation.
static_assert(std::is_trivially_copyable_v<SrcPlayer>);
static_assert(std::is_trivially_copyable_v<SrcFrame>);
static_assert(std::is_trivially_copyable_v<SrcEvent>);
static_assert(std::is_trivially_copyable_v<SrcIdentity>);
static_assert(std::is_trivially_copyable_v<SampleInference>);
static_assert(std::is_trivially_copyable_v<StunRow>);
static_assert(std::is_trivially_copyable_v<GoalRow>);
static_assert(std::is_trivially_copyable_v<StatRow>);
static_assert(kStatCount == 11);
static_assert(kRuleCount == 10);

TEST_CASE("import facts name every hypothesis and rule the design lists") {
    const char* expectedRules[] = {"I-1", "I-2", "I-3", "I-4", "I-5", "I-6", "I-7", "I-8", "I-9", "I-10"};
    for (int i = 0; i < kRuleCount; ++i) {
        CHECK(std::string(kInferFacts.rules[i].id) == expectedRules[i]);
        const bool heuristic = i >= kRuleBoost || i == kRuleMovingAttached;
        CHECK(kInferFacts.rules[i].gated == heuristic);
        // Direct readings always run; heuristics stay off until held-out validation turns them on.
        CHECK(kInferFacts.rules[i].enabled == !heuristic);
    }

    const EmitFacts& e = kEmitFacts;
    std::set<std::string> hypotheses{e.worldAxesMatchEngine.id, e.comFromHead.id, e.hmdOri.id,
                                     e.bodyOri.id,              e.handAxesDirect.id, e.discOwner.id};
    CHECK(hypotheses == std::set<std::string>{"H0", "H1", "H2", "H3", "H4", "H5"});

    // Measured values and their status are pinned to native_facts.json by test_facts.cpp.
    // O-1 is read from the engine's goal-type namer (P4).
    CHECK(e.goalTypes.status == FactStatus::kMeasured);
    CHECK(e.gametypeId.value == 0xCB60A4DE7E1CAF73ull);
    CHECK(kInferFacts.geoBitLeft.value != kInferFacts.geoBitRight.value);
}

TEST_CASE("result carries a code and a bounded sentence") {
    Result ok = Success();
    CHECK(ok.ok);
    CHECK(ok.code == ResultCode::Ok);
    CHECK(ok.message[0] == '\0');

    Result map = Refuse(ResultCode::UnsupportedMap, "map %s is not supported", "mpl_combat_dyson");
    CHECK_FALSE(map.ok);
    CHECK(std::string(map.message) == "map mpl_combat_dyson is not supported");

    std::string longName(sizeof(Result::message) * 4, 'x');
    Result truncated = Refuse(ResultCode::Corrupt, "%s", longName.c_str());
    CHECK(strlen(truncated.message) == sizeof(truncated.message) - 1);

    std::set<std::string> names;
    for (uint16_t c = 0; c <= static_cast<uint16_t>(ResultCode::TooLarge); ++c) {
        names.insert(ResultCodeName(static_cast<ResultCode>(c)));
    }
    CHECK(names.size() == static_cast<size_t>(ResultCode::TooLarge) + 1);
    CHECK(names.count("E_UNKNOWN") == 0);

    CHECK(ExitCodeFor(ok) == 0);
    CHECK(ExitCodeFor(map) == 2);
    CHECK(ExitCodeFor(Refuse(ResultCode::Io, "disk full")) == 3);
}

TEST_CASE("a stage's progress fills only its share of the bar") {
    Progress progress;
    progress.Report(1, 2);
    CHECK(progress.permille.load() == 500);
    progress.stageBase = 650;
    progress.stageSpan = 50;
    progress.Report(1, 2);
    CHECK(progress.permille.load() == 675);
    progress.Report(9, 3);
    CHECK(progress.permille.load() == 700);
    CHECK(RunConvertCommand(0, nullptr) == 2);
}

TEST_CASE("convert options default to the design's cadence and the longest session") {
    ConvertOptions options;
    CHECK(options.rateHz == 45);
    CHECK(options.session == -1);
    CHECK(options.reportPath == nullptr);
    CHECK(kConverterVersion >= 1);
}
