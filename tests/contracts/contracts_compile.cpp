// B.13, half one: "contracts_compile.cpp includes every header in Appendix B and instantiates a
// null-returning stub for every interface. It must compile and link."
//
// Half two is contracts_use.cpp, which calls the methods through base pointers from a second translation
// unit - proving the headers are usable from a packet that can see no implementation.

#include <cstring>
#include <string>

#include <doctest.h>

#include "appendix_b.h"

namespace contracts {

struct Row {
    const char* appendix;
    const char* name;
    const char* header;
    const char* owner;
    bool        present;
};

#define REPLAY_CONTRACT_ROW(APPENDIX, NAME, HEADER, OWNER, PRESENT) \
    {APPENDIX, NAME, HEADER, OWNER, (PRESENT) != 0},

const Row kRows[] = {REPLAY_CONTRACT_ROWS(REPLAY_CONTRACT_ROW)};

#undef REPLAY_CONTRACT_ROW

const size_t kRowCount = sizeof kRows / sizeof kRows[0];

}  // namespace contracts

namespace {

// A stub for each W1-C interface. The point is not the behaviour: it is that a translation unit which can
// see the header and nothing else can implement it, which is the property "frozen" actually means.
class StubLog final : public replay::ILog {
public:
    void Write(replay::LogLevel, const char*, const char*, ...) override { ++writes; }
    bool Enabled(replay::LogLevel) const override { return true; }
    void DumpLedger() override { ++dumps; }

    int writes = 0;
    int dumps = 0;
};

class StubConfig final : public replay::IConfig {
public:
    bool        GetBool(const char*, bool dflt) const override { return dflt; }
    int64_t     GetInt(const char*, int64_t dflt) const override { return dflt; }
    double      GetDouble(const char*, double dflt) const override { return dflt; }
    const char* GetString(const char*, const char* dflt) const override { return dflt; }
    bool        Has(const char*) const override { return false; }
};

StubLog    g_log;
StubConfig g_config;

bool StubInit(replay::ReplayServices*) { return true; }
void StubShutdown() {}

}  // namespace

// A REPLAY_MODULE in a translation unit that has seen only the frozen header. If this stops compiling,
// every Wave-2 module stops compiling with it.
REPLAY_MODULE("contract-stub", 1000, StubInit, nullptr, StubShutdown);

TEST_CASE("Appendix B's contracts compile, and the run says how many of them exist yet") {
    size_t present = 0;
    for (size_t i = 0; i < contracts::kRowCount; ++i) {
        const contracts::Row& row = contracts::kRows[i];
        CAPTURE(row.appendix);
        CAPTURE(row.header);
        CHECK(std::strlen(row.name) > 0);
        if (row.present) {
            ++present;
        } else {
            // std::string, not const char*: doctest streams a bare char pointer as an ADDRESS, so this
            // line printed seven hex numbers and told nobody anything.
            MESSAGE("contract " << std::string(row.appendix) << " (" << std::string(row.name) << ", "
                                << std::string(row.owner) << ") has no header yet at "
                                << std::string(row.header));
        }
    }
    // The floor. Without it this walks an empty table and reports a pass, which is precisely how a
    // freeze gate turns into decoration.
    CHECK(contracts::kRowCount >= 16);
    CHECK(present >= 1);
    CHECK(contracts::kRows[0].present);   // B.12 is this packet's own and is never allowed to be absent
    MESSAGE(present << " of " << contracts::kRowCount << " Appendix B contracts have landed");
}

TEST_CASE("the frozen types are implementable from outside the packet that declared them") {
    replay::ReplayServices services{};
    services.log = &g_log;
    services.config = &g_config;

    // Every pointer B.12 declares. All of them may legitimately be null right now, which is exactly why
    // B.12 says every consumer checks.
    CHECK(services.tick == nullptr);
    CHECK(services.clock == nullptr);
    CHECK(services.entity == nullptr);
    CHECK(services.pose == nullptr);
    CHECK(services.session == nullptr);
    CHECK(services.camera == nullptr);
    CHECK(services.asSeen == nullptr);
    CHECK(services.netVar == nullptr);
    CHECK(services.identity == nullptr);
    CHECK(services.audioState == nullptr);
    CHECK(services.audioRate == nullptr);
    CHECK(services.transport == nullptr);
    CHECK(services.demoIndex == nullptr);
    REQUIRE(services.log != nullptr);
    REQUIRE(services.config != nullptr);

    CHECK(services.config->GetBool("anything", true) == true);
    CHECK(services.config->GetInt("anything", 7) == 7);
    CHECK(services.config->GetDouble("anything", 1.5) == doctest::Approx(1.5));
    CHECK(std::strcmp(services.config->GetString("anything", "d"), "d") == 0);
    CHECK_FALSE(services.config->Has("anything"));
}
