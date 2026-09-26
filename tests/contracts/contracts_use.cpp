// B.13, half two: "contracts_use.cpp is a second translation unit that CALLS every method through base
// pointers, proving the headers are usable from a packet that cannot see any implementation."
//
// Separate from contracts_compile.cpp on purpose. One TU that both declares and calls proves less: the
// declaration could depend on something only that file has. This one sees the headers and nothing else,
// which is the situation every Wave-2 packet is in.

#include <cstring>

#include <doctest.h>

#include "appendix_b.h"

namespace {

// Deliberately implemented here rather than shared with the other TU: a caller that could reach the other
// file's stubs would be proving they link, not that the contract is callable through its own header.
class CountingLog final : public replay::ILog {
public:
    void Write(replay::LogLevel level, const char* module, const char* fmt, ...) override {
        lastLevel = level;
        lastModule = module;
        lastFormat = fmt;
        ++writes;
    }
    bool Enabled(replay::LogLevel level) const override { return level != replay::LogLevel::Trace; }
    void DumpLedger() override { ++dumps; }

    replay::LogLevel    lastLevel = replay::LogLevel::Trace;
    const char* lastModule = nullptr;
    const char* lastFormat = nullptr;
    int         writes = 0;
    int         dumps = 0;
};

class EchoConfig final : public replay::IConfig {
public:
    bool        GetBool(const char*, bool dflt) const override { return !dflt; }
    int64_t     GetInt(const char*, int64_t dflt) const override { return dflt + 1; }
    double      GetDouble(const char*, double dflt) const override { return dflt * 2.0; }
    const char* GetString(const char*, const char*) const override { return "answered"; }
    bool        Has(const char* key) const override { return std::strcmp(key, "known") == 0; }
};

}  // namespace

TEST_CASE("every replay::ILog and replay::IConfig method is callable through a base pointer") {
    CountingLog log;
    EchoConfig config;

    replay::ILog* logPtr = &log;
    const replay::IConfig* configPtr = &config;

    // Varargs through a virtual, which is the shape B.12 froze and the shape that would break first if
    // someone "simplified" the signature.
    logPtr->Write(replay::LogLevel::Warn, "contracts", "%s %d %llu %.2f", "mixed", -1, 2ull, 3.5);
    logPtr->Write(replay::LogLevel::Error, "contracts", "no arguments");
    logPtr->DumpLedger();

    CHECK(log.writes == 2);
    CHECK(log.dumps == 1);
    CHECK(log.lastLevel == replay::LogLevel::Error);
    CHECK(std::strcmp(log.lastModule, "contracts") == 0);
    CHECK(logPtr->Enabled(replay::LogLevel::Error));
    CHECK_FALSE(logPtr->Enabled(replay::LogLevel::Trace));

    CHECK(configPtr->GetBool("k", false) == true);
    CHECK(configPtr->GetInt("k", 41) == 42);
    CHECK(configPtr->GetDouble("k", 1.5) == doctest::Approx(3.0));
    CHECK(std::strcmp(configPtr->GetString("k", "unused"), "answered") == 0);
    CHECK(configPtr->Has("known"));
    CHECK_FALSE(configPtr->Has("unknown"));
}

TEST_CASE("a replay::ReplayModule can be built and dispatched through the frozen struct alone") {
    static int inits = 0;
    static int shutdowns = 0;
    struct Fns {
        static bool Init(replay::ReplayServices*) { ++inits; return true; }
        static void Shutdown() { ++shutdowns; }
    };

    const replay::ReplayModule mod{"from-contracts-use", 5, Fns::Init, nullptr, Fns::Shutdown};
    CHECK(std::strcmp(mod.name, "from-contracts-use") == 0);
    CHECK(mod.order == 5);
    CHECK(mod.tick == nullptr);          // B.12: tick may be null

    replay::ReplayServices services{};
    REQUIRE(mod.init != nullptr);
    CHECK(mod.init(&services));
    REQUIRE(mod.shutdown != nullptr);
    mod.shutdown();
    CHECK(inits == 1);
    CHECK(shutdowns == 1);
}
