// W1-D's offline half of the game-state sampler: the read chains of ARCH-21 S3.1 driven against a
// fake store, so that a wrong DEPTH, a dropped phase row or a skipped Readable() probe is red here
// rather than silently zero in a capture.
//
// The fake answers a symbol only at the store the fact row names, and every store holds a decoy
// answer for the symbols that belong to a different depth.  That is the discriminator: a sampler
// that asked round_counter at depth 2, or blue_score at depth 0, gets the decoy and the value
// assertion fails -- an "it returned something" test would pass under both readings.

#include <stdint.h>
#include <string.h>

#include <chrono>
#include <string>
#include <type_traits>
#include <vector>

#include <doctest.h>

#include "engine/gamestate.h"
#include "engine/gamestate_facts.h"

using namespace replay::gamestate;
// `facts` alone is ambiguous here: gamestate_facts.h pulls in the global ::facts as well.
namespace gsf = replay::gamestate::facts;

namespace {

const int32_t  kDecoyInt = -777777;
const uint64_t kDecoySymbol = 0xDEADBEEFDEADBEEFULL;
const float    kDecoyFloat = -12345.0f;

struct IntAnswer {
    void*    store;
    uint64_t symbol;
    int32_t  value;
};
struct SymbolAnswer {
    void*    store;
    uint64_t symbol;
    uint64_t value;
};
struct FloatAnswer {
    void*    store;
    uint64_t symbol;
    float    value;
};

class FakeEngine : public IEngineAccess {
public:
    FakeEngine() {
        memset(netGameBuf, 0, sizeof netGameBuf);
        memset(rulesBuf, 0, sizeof rulesBuf);
        memset(rulesBuf2, 0, sizeof rulesBuf2);
        memset(scoresBuf, 0, sizeof scoresBuf);
        WireChain(rulesBuf);
    }

    // netGame -> +kRulesOff -> the game-mode block -> *block -> the score store.
    void WireChain(uint8_t* block) {
        *reinterpret_cast<void**>(netGameBuf + gsf::kRulesOff) = block;
        *reinterpret_cast<void**>(block) = scoresBuf;
    }

    void* NetGame() override { return netGamePresent ? netGameBuf : nullptr; }

    bool Readable(const void* at, size_t bytes) override {
        readableCalls++;
        lastReadableBytes = bytes;
        readableSpans.push_back(bytes);
        for (size_t i = 0; i < refused.size(); ++i) {
            if (refused[i] == at) {
                return false;
            }
        }
        return true;
    }

    bool ReadImageQword(uint64_t va, uint64_t* out) override {
        imageReads++;
        for (uint32_t i = 0; i < gsf::kPhaseFactCount; ++i) {
            if (gsf::kPhaseFacts[i].literalVa == va) {
                *out = (va == corruptLiteralVa) ? ~gsf::kPhaseFacts[i].symbol
                                                : gsf::kPhaseFacts[i].symbol;
                return true;
            }
        }
        return false;
    }

    int32_t GetInt(void* store, uint64_t symbol) override {
        getIntCalls++;
        for (size_t i = 0; i < ints.size(); ++i) {
            if (ints[i].store == store && ints[i].symbol == symbol) {
                return ints[i].value;
            }
        }
        unanswered++;
        return kDecoyInt;
    }

    uint64_t GetSymbol(void* store, uint64_t symbol) override {
        getSymbolCalls++;
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (symbols[i].store == store && symbols[i].symbol == symbol) {
                return symbols[i].value;
            }
        }
        unanswered++;
        return kDecoySymbol;
    }

    float GetFloat(void* store, uint64_t symbol) override {
        getFloatCalls++;
        for (size_t i = 0; i < floats.size(); ++i) {
            if (floats[i].store == store && floats[i].symbol == symbol) {
                return floats[i].value;
            }
        }
        unanswered++;
        return kDecoyFloat;
    }

    void SetInt(void* store, uint64_t symbol, int32_t value) {
        for (size_t i = 0; i < ints.size(); ++i) {
            if (ints[i].store == store && ints[i].symbol == symbol) {
                ints[i].value = value;
                return;
            }
        }
        IntAnswer a = {store, symbol, value};
        ints.push_back(a);
    }
    void SetSymbol(void* store, uint64_t symbol, uint64_t value) {
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (symbols[i].store == store && symbols[i].symbol == symbol) {
                symbols[i].value = value;
                return;
            }
        }
        SymbolAnswer a = {store, symbol, value};
        symbols.push_back(a);
    }
    void SetFloat(void* store, uint64_t symbol, float value) {
        FloatAnswer a = {store, symbol, value};
        floats.push_back(a);
    }

    uint8_t netGameBuf[gsf::kRulesOff + sizeof(void*)];
    uint8_t rulesBuf[0x20];
    uint8_t rulesBuf2[0x20];
    uint8_t scoresBuf[16];

    bool     netGamePresent = true;
    uint64_t corruptLiteralVa = 0;
    std::vector<const void*> refused;

    std::vector<IntAnswer>    ints;
    std::vector<SymbolAnswer> symbols;
    std::vector<FloatAnswer>  floats;

    size_t readableCalls = 0;
    size_t lastReadableBytes = 0;
    std::vector<size_t> readableSpans;
    size_t imageReads = 0;
    size_t getIntCalls = 0;
    size_t getSymbolCalls = 0;
    size_t getFloatCalls = 0;
    size_t unanswered = 0;
};

// A fully wired store: every derivable field answered at the depth its fact row names, and a decoy
// for each depth-crossing symbol planted at the store it must NOT be asked of.
void Populate(FakeEngine& fake) {
    void* net = fake.netGameBuf;
    void* scores = fake.scoresBuf;

    fake.SetSymbol(net, gsf::kVarGameState.symbol, gsf::kPhaseFacts[21].symbol);  // round_start
    fake.SetSymbol(net, gsf::kVarFrisbeeOwnerId.symbol, static_cast<uint64_t>(kFrisbeeUnowned));
    fake.SetFloat(net, gsf::kVarGameTimer.symbol, 42.5f);
    fake.SetInt(net, gsf::kVarRoundCounter.symbol, 3);
    fake.SetInt(net, gsf::kVarBlueGameScore.symbol, 2);
    fake.SetInt(net, gsf::kVarOrangeGameScore.symbol, 1);
    fake.SetInt(net, gsf::kVarRoundWinningTeam.symbol, 0);
    fake.SetInt(scores, gsf::kVarBlueScore.symbol, 7);
    fake.SetInt(scores, gsf::kVarRedScore.symbol, 5);

    // The decoys.  Same symbol, wrong store, unmistakable value.
    fake.SetInt(scores, gsf::kVarRoundCounter.symbol, 999);
    fake.SetInt(scores, gsf::kVarBlueGameScore.symbol, 999);
    fake.SetInt(net, gsf::kVarBlueScore.symbol, 111);
    fake.SetInt(net, gsf::kVarRedScore.symbol, 222);

    *reinterpret_cast<uint16_t*>(fake.rulesBuf + gsf::kLastScoreTeamOff) = 1;
    *reinterpret_cast<uint16_t*>(fake.rulesBuf + gsf::kLastScorePointsOff) = 2;
    *reinterpret_cast<uint16_t*>(fake.rulesBuf2 + gsf::kLastScoreTeamOff) = 1;
    *reinterpret_cast<uint16_t*>(fake.rulesBuf2 + gsf::kLastScorePointsOff) = 2;
}

struct Fixture {
    FakeEngine  fake;
    IGameState* state;

    Fixture() {
        Populate(fake);
        state = CreateGameState(&fake);
    }
    ~Fixture() { DestroyGameState(state); }
};

}  // namespace

TEST_CASE("the fact rows carry ARCH-21 S3.1's depths, and the sampler is built on them") {
    CHECK(gsf::kLastScoreTeamOff == 0x10u);
    CHECK(gsf::kLastScorePointsOff == 0x12u);

    CHECK(gsf::kVarGameState.depth == 0);
    CHECK(gsf::kVarGameTimer.depth == 0);
    CHECK(gsf::kVarRoundCounter.depth == 0);
    CHECK(gsf::kVarBlueGameScore.depth == 0);
    CHECK(gsf::kVarOrangeGameScore.depth == 0);
    CHECK(gsf::kVarRoundWinningTeam.depth == 0);
    CHECK(gsf::kVarFrisbeeOwnerId.depth == 0);
    CHECK(gsf::kVarBlueScore.depth == 2);
    CHECK(gsf::kVarRedScore.depth == 2);

    // game_timer, never game_clock: the JSON key resolves to no symbol at all (S3.3).
    CHECK(std::string(gsf::kVarGameTimer.name) == "game_timer");
    CHECK(std::string(gsf::kVarRoundCounter.name) == "round_counter");
    CHECK(std::string(gsf::kVarBlueScore.name) == "blue_score");
}

TEST_CASE("a whole chain answers Ok, and every value comes from the store its depth names") {
    Fixture f;
    GameStateSample s;
    CHECK(f.state->State() == Availability::NotAsked);
    CHECK(f.state->Sample(s) == Availability::Ok);
    CHECK(f.state->State() == Availability::Ok);

    CHECK(f.fake.unanswered == 0);

    CHECK(Has(s, kHasRoundCounter));
    CHECK(s.roundCounter == 3u);  // 999 is the decoy planted at depth 2
    CHECK(Has(s, kHasRoundPoints));
    CHECK(s.bluePoints == 7u);  // 111 is the decoy planted at depth 0
    CHECK(s.redPoints == 5u);
    CHECK(Has(s, kHasMatchScore));
    CHECK(s.blueMatchScore == 2u);
    CHECK(s.redMatchScore == 1u);
    CHECK(Has(s, kHasGameTimer));
    CHECK(s.gameTimer == doctest::Approx(42.5f));
    CHECK(Has(s, kHasFrisbeeOwnerId));
    CHECK(s.frisbeeOwnerId == kFrisbeeUnowned);
    CHECK(Has(s, kHasLastScore));
    CHECK(s.lastScoreTeam == 1);
    CHECK(s.lastScorePoints == 2);
    CHECK(Has(s, kHasPhaseSymbol));
    CHECK(s.phase == static_cast<uint8_t>(Phase::RoundStart));
    CHECK(s.availability == static_cast<uint8_t>(Availability::Ok));

    // The caller stamps the clocks; the sampler has none and must not invent one.
    CHECK(s.engineTimeUs == 0u);
    CHECK(s.captureTimeUs == 0u);

    // Nothing that has no source is defaulted into looking answered.
    CHECK_FALSE(Has(s, kHasPausedSymbol));
    CHECK_FALSE(Has(s, kHasCountdownTimer));
    CHECK_FALSE(Has(s, kHasFirstLaunchOfRound));
    CHECK(s.pausedSymbol == 0u);
    CHECK(s.countdownTimer == doctest::Approx(0.0f));

    const Stats& stats = f.state->GetStats();
    CHECK(stats.samples == 1u);
    CHECK(stats.vocabularyChecked == gsf::kPhaseFactCount);
    CHECK(stats.vocabularyMismatches == 0u);
    // No slot address is ever cached, so S4.2's re-arm and drift counters stay at zero by
    // construction rather than by omission.
    CHECK(stats.reArms == 0u);
    CHECK(stats.driftInvalidations == 0u);
}

TEST_CASE("every field that cannot be read is named, with its reason") {
    size_t count = 0;
    const UnreadableField* fields = UnreadableFields(&count);
    REQUIRE(count == 3u);
    bool sawPaused = false, sawCountdown = false, sawFirstLaunch = false;
    for (size_t i = 0; i < count; ++i) {
        CHECK(strlen(fields[i].field) > 0);
        CHECK(strlen(fields[i].reason) > 20);
        std::string field(fields[i].field);
        if (field == "pausedSymbol") sawPaused = true;
        if (field == "countdownTimer") sawCountdown = true;
        if (field == "firstLaunchOfRound") sawFirstLaunch = true;
    }
    CHECK(sawPaused);
    CHECK(sawCountdown);
    CHECK(sawFirstLaunch);
}

TEST_CASE("all 27 phase rows classify, and team_room_wait never folds into round_start") {
    Fixture f;
    GameStateSample s;
    for (uint32_t i = 0; i < gsf::kPhaseFactCount; ++i) {
        f.fake.SetSymbol(f.fake.netGameBuf, gsf::kVarGameState.symbol,
                         gsf::kPhaseFacts[i].symbol);
        REQUIRE(f.state->Sample(s) == Availability::Ok);
        const std::string row(gsf::kPhaseFacts[i].name);
        CHECK_MESSAGE(s.phase == static_cast<uint8_t>(gsf::kPhaseFacts[i].phase), row);
        CHECK_MESSAGE(s.phaseSymbol == gsf::kPhaseFacts[i].symbol, row);
        CHECK_MESSAGE(Has(s, kHasPhaseSymbol), row);
        CHECK_MESSAGE(s.phase != static_cast<uint8_t>(Phase::Unknown), row);
    }

    // The two the engine's own name mapper calls "round_start". Classifying by that name is what
    // emitted a round marker on entering the team room.
    for (uint32_t i = 0; i < gsf::kPhaseFactCount; ++i) {
        const std::string name(gsf::kPhaseFacts[i].name);
        if (name.compare(0, 15, "team_room_wait_") == 0) {
            CHECK(gsf::kPhaseFacts[i].phase == Phase::TeamRoomWait);
            CHECK(std::string(gsf::kPhaseFacts[i].engineName) == "round_start");
            CHECK(gsf::kPhaseFacts[i].engineId == 0);
        }
    }
}

TEST_CASE("a symbol in no row is Unknown, and a zero symbol is Absent") {
    Fixture f;
    GameStateSample s;

    f.fake.SetSymbol(f.fake.netGameBuf, gsf::kVarGameState.symbol, 0x1234567812345678ULL);
    REQUIRE(f.state->Sample(s) == Availability::Ok);
    CHECK(s.phase == static_cast<uint8_t>(Phase::Unknown));
    CHECK(Has(s, kHasPhaseSymbol));
    CHECK(s.phaseSymbol == 0x1234567812345678ULL);

    f.fake.SetSymbol(f.fake.netGameBuf, gsf::kVarGameState.symbol, 0);
    REQUIRE(f.state->Sample(s) == Availability::Ok);
    CHECK(s.phase == static_cast<uint8_t>(Phase::Absent));
    CHECK_FALSE(Has(s, kHasPhaseSymbol));
}

TEST_CASE("the refusal ladder: each missing link in the chain has its own name") {
    Fixture f;
    GameStateSample s;

    SUBCASE("no netGame") {
        f.fake.netGamePresent = false;
        CHECK(f.state->Sample(s) == Availability::NoNetGame);
        CHECK(s.present == 0u);
        CHECK(s.phase == static_cast<uint8_t>(Phase::Absent));
        CHECK(f.state->GetStats().refusalsByReason[static_cast<size_t>(Availability::NoNetGame)] == 1u);
    }
    SUBCASE("no game-mode block: depth 0 still answers, the points do not") {
        *reinterpret_cast<void**>(f.fake.netGameBuf + gsf::kRulesOff) = nullptr;
        CHECK(f.state->Sample(s) == Availability::NoRulesBlock);
        CHECK(Has(s, kHasRoundCounter));
        CHECK(s.roundCounter == 3u);
        CHECK(Has(s, kHasMatchScore));
        CHECK_FALSE(Has(s, kHasRoundPoints));
        CHECK_FALSE(Has(s, kHasLastScore));
        CHECK(s.bluePoints == 0u);
    }
    SUBCASE("no score store: the depth-1 last-score pair still answers") {
        *reinterpret_cast<void**>(f.fake.rulesBuf) = nullptr;
        CHECK(f.state->Sample(s) == Availability::NoScoreStore);
        CHECK(Has(s, kHasLastScore));
        CHECK_FALSE(Has(s, kHasRoundPoints));
        CHECK(s.bluePoints == 0u);
    }
}

TEST_CASE("Readable is asked on every pointer change, and not on a tick where nothing changed") {
    Fixture f;
    GameStateSample s;

    REQUIRE(f.state->Sample(s) == Availability::Ok);
    CHECK(f.fake.readableCalls == 3u);  // netGame, the game-mode block, the score store
    CHECK(f.state->GetStats().cacheInvalidations == 3u);

    REQUIRE(f.state->Sample(s) == Availability::Ok);
    CHECK(f.fake.readableCalls == 3u);
    CHECK(f.state->GetStats().cacheInvalidations == 3u);

    // A level unload swaps the game-mode block. The new pointer is probed before it is read, and so
    // is the score store reached through it -- the pointer is unchanged in value but not in
    // provenance, and a store this sampler has not probed since the swap is not one it will read.
    f.fake.WireChain(f.fake.rulesBuf2);
    REQUIRE(f.state->Sample(s) == Availability::Ok);
    CHECK(f.fake.readableCalls == 5u);
    CHECK(f.state->GetStats().cacheInvalidations == 5u);
}

TEST_CASE("a pointer Readable refuses is never dereferenced") {
    Fixture f;
    GameStateSample s;

    SUBCASE("the game-mode block") {
        f.fake.refused.push_back(f.fake.rulesBuf);
        CHECK(f.state->Sample(s) == Availability::NotReadable);
        CHECK_FALSE(Has(s, kHasLastScore));
        CHECK_FALSE(Has(s, kHasRoundPoints));
        CHECK(Has(s, kHasRoundCounter));  // depth 0 was answered before the refusal
        CHECK(f.state->GetStats().readableRefusals == 1u);

        // The refusal does not stick: the next tick re-probes, because the pointer was never
        // accepted.  A sampler that cached the refused pointer would go quiet for the session.
        f.fake.refused.clear();
        CHECK(f.state->Sample(s) == Availability::Ok);
        CHECK(Has(s, kHasRoundPoints));
    }
    SUBCASE("netGame itself") {
        f.fake.refused.push_back(f.fake.netGameBuf);
        CHECK(f.state->Sample(s) == Availability::NotReadable);
        CHECK(s.present == 0u);
        CHECK(f.fake.getIntCalls == 0u);
        CHECK(f.fake.getSymbolCalls == 0u);
    }
}

TEST_CASE("the Readable spans cover the offsets that are actually read") {
    Fixture f;
    GameStateSample s;
    f.fake.refused.push_back(f.fake.scoresBuf);
    REQUIRE(f.state->Sample(s) == Availability::NotReadable);
    REQUIRE(f.fake.readableSpans.size() == 3u);
    CHECK(f.fake.readableSpans[0] >= gsf::kRulesOff + sizeof(void*));
    CHECK(f.fake.readableSpans[1] >= gsf::kLastScorePointsOff + sizeof(uint16_t));
    CHECK(f.fake.readableSpans[2] >= sizeof(void*));
    CHECK(f.fake.lastReadableBytes == sizeof(void*));
}

TEST_CASE("a phase table that disagrees with the image refuses to classify") {
    FakeEngine fake;
    Populate(fake);
    fake.corruptLiteralVa = gsf::kPhaseFacts[24].literalVa;  // team_room_wait_enter

    IGameState* state = CreateGameState(&fake);
    GameStateSample s;
    CHECK(state->State() == Availability::VocabularyRefused);
    CHECK(state->Sample(s) == Availability::VocabularyRefused);
    CHECK(s.present == 0u);
    CHECK(s.phase == static_cast<uint8_t>(Phase::NotSampled));
    CHECK(state->GetStats().vocabularyChecked == gsf::kPhaseFactCount);
    CHECK(state->GetStats().vocabularyMismatches == 1u);
    CHECK(fake.getSymbolCalls == 0u);
    DestroyGameState(state);
}

TEST_CASE("the sampler's own per-tick cost, against a fake store") {
    Fixture f;
    GameStateSample s;
    REQUIRE(f.state->Sample(s) == Availability::Ok);

    const int kIterations = 200000;
    const auto start = std::chrono::steady_clock::now();
    uint64_t sink = 0;
    for (int i = 0; i < kIterations; ++i) {
        f.state->Sample(s);
        sink += s.roundCounter + s.present;
    }
    const auto end = std::chrono::steady_clock::now();
    CHECK(sink != 0u);

    const double ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() /
        static_cast<double>(kIterations);
    MESSAGE("gamestate Sample(): " << ns << " ns/tick over " << kIterations
                                   << " ticks against a fake store (the engine getters' own cost is "
                                      "NOT in this number and cannot be measured offline)");
    // Our own overhead only. The budget is 5 us/tick for the whole sample; a fake getter is a
    // vector walk, so this number is an upper bound on OUR work and says nothing about the engine's.
    CHECK(ns < 5000.0);
}

namespace {

// A software model of the engine's own GS.GET_SYMBOL entry point, echovr.exe 0x140D5CED0.
//
// FakeEngine above is an IEngineAccess -- a C++ interface, so it has whatever calling convention
// the compiler picked and could never have seen this defect.  The engine getter is not an
// interface: it is a raw address called through gamestate::GetSymbolFn, and the bug was which
// registers that call filled.  This model is written to the ABI the exe actually uses, so the
// mis-binding is reproducible offline with no engine in the process.
//
// It does the two things the real getter does.  It dereferences the STORE, which arrives in
// argument 2, at +0x2E00 -- `mov r15,[rax+2E00h]` at 0x140D5CF01 with rax = rdx.  And it writes
// the CSymbol64 through the hidden result buffer in argument 1 and returns that same pointer --
// `mov [r12],rsi` / `mov rax,r12` at 0x140D5D962.  Where the real engine faulted on a store that
// is not a store, the model latches g_symbolFault instead: a test that crashes reports nothing.
struct ModelStore {
    uint8_t  head[0x2E00];
    uint64_t table;
};

struct ModelAnswer {
    const ModelStore* store;
    uint64_t          symbol;
    uint64_t          value;
};

std::vector<const ModelStore*> g_modelStores;
std::vector<ModelAnswer>       g_modelAnswers;
bool                           g_symbolFault = false;
size_t                         g_resultWrites = 0;

void ResetModel() {
    g_modelStores.clear();
    g_modelAnswers.clear();
    g_symbolFault = false;
    g_resultWrites = 0;
}

uint64_t* EngineGetSymbol(uint64_t* result, void* store, uint64_t symbol) {
    for (size_t i = 0; i < g_modelStores.size(); ++i) {
        if (g_modelStores[i] == store) {
            const ModelStore* s = static_cast<const ModelStore*>(store);
            uint64_t value = kDecoySymbol;
            if (s->table != 0) {
                for (size_t j = 0; j < g_modelAnswers.size(); ++j) {
                    if (g_modelAnswers[j].store == s && g_modelAnswers[j].symbol == symbol) {
                        value = g_modelAnswers[j].value;
                        break;
                    }
                }
            }
            *result = value;
            g_resultWrites++;
            return result;
        }
    }
    // 0x140D5CF01 with a CSymbol64 in RDX.  The engine has no check here at all -- it loads
    // [argument2 + 0x2E00] and the process dies at the first symbol read after capture opens.
    g_symbolFault = true;
    *result = kDecoySymbol;
    g_resultWrites++;
    return result;
}

// The model is the engine's entry point, so it must be assignable to the field the live binder
// fills.  A 2-argument getter is not, which is what stops the L2-RUN-6 binding from being written
// again -- reinterpret_cast would have taken it in silence.
typedef uint64_t (*TwoArgSymbolFn)(void* store, uint64_t symbol);
static_assert(std::is_same<decltype(&EngineGetSymbol), GetSymbolFn>::value,
              "the model must have the type gamestate_live.cpp binds GS.GET_SYMBOL as");
static_assert(!std::is_convertible<TwoArgSymbolFn, GetSymbolFn>::value,
              "a 2-argument getter must not be assignable to GetSymbolFn");

// The fake store, but with GS.GET_SYMBOL answered through the real ABI instead of a C++ method.
class ModelEngine final : public FakeEngine {
public:
    explicit ModelEngine(bool misbound) : misbound_(misbound) {
        ResetModel();
        memset(&modelNet_, 0, sizeof modelNet_);
        modelNet_.table = 0x2E00C0DE2E00C0DEULL;
        g_modelStores.push_back(&modelNet_);
    }

    // Populate() fills the fake's answers; the model holds its own copy of the netGame ones,
    // keyed on the store the engine would dereference.
    void LoadModelAnswers() {
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (symbols[i].store == netGameBuf) {
                ModelAnswer a = {&modelNet_, symbols[i].symbol, symbols[i].value};
                g_modelAnswers.push_back(a);
            }
        }
    }

    uint64_t GetSymbol(void* store, uint64_t symbol) override {
        if (store != netGameBuf) {
            return FakeEngine::GetSymbol(store, symbol);
        }
        getSymbolCalls++;
        if (misbound_) {
            // The registers the OLD binding filled: argument 1 = store, argument 2 = the symbol,
            // argument 3 = whatever was already in R8.  The caller then read the return value out
            // of RAX, which the sret contract leaves holding argument 1.
            uint64_t* asResultBuffer = reinterpret_cast<uint64_t*>(&modelNet_);
            EngineGetSymbol(asResultBuffer, reinterpret_cast<void*>(symbol), 0);
            return reinterpret_cast<uint64_t>(asResultBuffer);
        }
        uint64_t result = 0;
        EngineGetSymbol(&result, &modelNet_, symbol);
        return result;
    }

private:
    ModelStore modelNet_;
    bool       misbound_;
};

struct ModelFixture {
    ModelEngine engine;
    IGameState* state;

    explicit ModelFixture(bool misbound) : engine(misbound) {
        Populate(engine);
        engine.LoadModelAnswers();
        state = CreateGameState(&engine);
    }
    ~ModelFixture() { DestroyGameState(state); }
};

}  // namespace

TEST_CASE("GS.GET_SYMBOL through the engine's own ABI: the answer arrives in the result buffer") {
    ModelFixture f(false);
    GameStateSample s;
    REQUIRE(f.state->Sample(s) == Availability::Ok);

    CHECK_FALSE(g_symbolFault);
    CHECK(f.engine.getSymbolCalls > 0u);
    CHECK(g_resultWrites == f.engine.getSymbolCalls);
    CHECK(s.phase == static_cast<uint8_t>(Phase::RoundStart));
    CHECK(s.frisbeeOwnerId == kFrisbeeUnowned);
}

TEST_CASE("the 2-argument binding hands the symbol to the store slot -- the L2-RUN-6 fault") {
    ModelFixture f(true);
    GameStateSample s;
    f.state->Sample(s);

    // With the symbol in argument 2 the engine dereferences it as the store.  Offline that is a
    // latched flag; in the client it was the 0xC0000005 at 0x140D5CF01, 31 ms after capture open.
    CHECK(g_symbolFault);
    CHECK(s.phase != static_cast<uint8_t>(Phase::RoundStart));
}
