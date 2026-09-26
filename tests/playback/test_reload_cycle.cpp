// RELOAD-6: the reload's level-cycle sequence as a pure machine. Every path must end with the hold
// released -- a reload that cannot re-enter the level still opens, in place.

#include <string>

#include "doctest.h"

#include "playback/core/reload_cycle.h"

namespace {

using replay::reloadcycle::Action;
using replay::reloadcycle::Limits;
using replay::reloadcycle::Machine;
using replay::reloadcycle::Observation;
using replay::reloadcycle::Outcome;
using replay::reloadcycle::Phase;
using replay::reloadcycle::RequestAnswer;

constexpr int32_t kInGame = 9;
constexpr int32_t kLoadingGlobal = 4;
constexpr int32_t kLobby = 5;
constexpr int32_t kLoadingLevel = 7;
constexpr int32_t kLoginFailed = -94;

Limits Tight() {
    Limits l;
    l.leaveMs = 100;
    l.requestMs = 50;
    l.exitMs = 40;
    l.entryMs = 900;
    return l;
}

Observation At(uint64_t now, int32_t state, bool leavePending = false) {
    return Observation{now, leavePending, state};
}

// Arms at t=1001, lets the leave land at t=1010 and has the request accepted there.
Machine AcceptedAt1010() {
    Machine m(Tight());
    m.Arm(1001, false);
    REQUIRE(m.Step(At(1005, kInGame, true)) == Action::None);
    REQUIRE(m.Step(At(1010, kInGame)) == Action::Request);
    m.Answer(RequestAnswer::Accepted, 1010);
    REQUIRE(m.phase() == Phase::AwaitExit);
    return m;
}

}  // namespace

TEST_CASE("RELOAD-6 cycle: an idle machine holds nothing and asks for nothing") {
    Machine m;
    CHECK_FALSE(m.Holds());
    CHECK(m.Step(At(5, kInGame)) == Action::None);
    CHECK(m.outcome() == Outcome::None);
    CHECK_FALSE(m.FinishedNow());
}

TEST_CASE("RELOAD-6 cycle: leave, request, exit, entry -- in that order, and only then released") {
    Machine m = AcceptedAt1010();
    CHECK(m.Holds());
    CHECK(m.Step(At(1020, kInGame)) == Action::None);   // accepted, the stock call not yet run
    CHECK(m.phase() == Phase::AwaitExit);
    CHECK(m.Step(At(1030, kLoadingGlobal)) == Action::None);
    CHECK(m.phase() == Phase::AwaitEntry);
    CHECK(m.LeftLevel());
    CHECK(m.ExitAtMs() == 1030);
    // The way back in passes through the lobby and the level load; none of them is "in game".
    for (int32_t s : {kLoadingGlobal, kLobby, kLoadingLevel, 8}) {
        CHECK(m.Step(At(1500, s)) == Action::None);
        CHECK(m.Holds());
    }
    CHECK_FALSE(m.FinishedNow());
    m.Step(At(1600, kInGame));
    CHECK_FALSE(m.Holds());
    CHECK(m.outcome() == Outcome::Cycled);
    CHECK(m.FinishedNow());
    CHECK_FALSE(m.FinishedNow());   // an edge, reported once
    CHECK(m.ElapsedMs(1600) == 599);
}

TEST_CASE("RELOAD-6 cycle: nothing is requested while the previous roster is still leaving") {
    Machine m(Tight());
    m.Arm(0, false);
    for (uint64_t t = 0; t < 100; t += 10) CHECK(m.Step(At(t, kInGame, true)) == Action::None);
    CHECK(m.phase() == Phase::AwaitLeave);
    m.Step(At(100, kInGame, true));
    CHECK_FALSE(m.Holds());
    CHECK(m.outcome() == Outcome::LeaveStuck);
    CHECK_FALSE(m.LeftLevel());
}

TEST_CASE("RELOAD-6 cycle: 'not yet' is asked again; a settled refusal or the bound releases in place") {
    SUBCASE("not yet, then accepted") {
        Machine m(Tight());
        m.Arm(0, false);
        CHECK(m.Step(At(1, kInGame)) == Action::Request);
        m.Answer(RequestAnswer::NotYet, 1);
        CHECK(m.phase() == Phase::AwaitRequest);
        CHECK(m.Step(At(20, kInGame)) == Action::Request);
        m.Answer(RequestAnswer::Accepted, 20);
        CHECK(m.phase() == Phase::AwaitExit);
    }
    SUBCASE("refused") {
        Machine m(Tight());
        m.Arm(0, false);
        CHECK(m.Step(At(1, kInGame)) == Action::Request);
        m.Answer(RequestAnswer::Refused, 1);
        CHECK_FALSE(m.Holds());
        CHECK(m.outcome() == Outcome::RequestRefused);
    }
    SUBCASE("not yet until the bound") {
        Machine m(Tight());
        m.Arm(0, false);
        CHECK(m.Step(At(1, kInGame)) == Action::Request);
        m.Answer(RequestAnswer::NotYet, 1);
        CHECK(m.Step(At(51, kInGame)) == Action::None);
        CHECK_FALSE(m.Holds());
        CHECK(m.outcome() == Outcome::RequestRefused);
    }
}

TEST_CASE("RELOAD-6 cycle: an accepted request that never leaves the level releases in place") {
    Machine m = AcceptedAt1010();
    CHECK(m.Step(At(1049, kInGame)) == Action::None);
    CHECK(m.Holds());
    m.Step(At(1050, kInGame));
    CHECK_FALSE(m.Holds());
    CHECK(m.outcome() == Outcome::NoExit);
    CHECK_FALSE(m.LeftLevel());
}

TEST_CASE("RELOAD-6 cycle: after the level was left, a failure state or the bound releases the hold") {
    SUBCASE("login failed") {
        Machine m = AcceptedAt1010();
        m.Step(At(1020, kLoadingGlobal));
        m.Step(At(1030, kLoginFailed));
        CHECK_FALSE(m.Holds());
        CHECK(m.outcome() == Outcome::SessionFailed);
        CHECK(m.LeftLevel());
        CHECK(m.LastState() == kLoginFailed);
    }
    SUBCASE("never back in game") {
        Machine m = AcceptedAt1010();
        m.Step(At(1020, kLoadingGlobal));
        m.Step(At(1919, kLoadingLevel));
        CHECK(m.Holds());
        m.Step(At(1920, kLoadingLevel));
        CHECK_FALSE(m.Holds());
        CHECK(m.outcome() == Outcome::EntryTimedOut);
    }
}

TEST_CASE("RELOAD-6 cycle: an entry the load funnel already queued is waited out, not asked twice") {
    Machine m(Tight());
    m.Arm(0, true);
    CHECK(m.phase() == Phase::AwaitExit);
    CHECK(m.Step(At(1, kInGame, true)) == Action::None);
    CHECK(m.Step(At(2, kLoadingGlobal)) == Action::None);
    CHECK(m.Step(At(3, kInGame)) == Action::None);
    CHECK(m.outcome() == Outcome::Cycled);
}

TEST_CASE("RELOAD-6 cycle: cancel ends a holding cycle once, and a re-arm starts clean") {
    Machine m = AcceptedAt1010();
    m.Step(At(1020, kLoadingGlobal));
    m.Cancel();
    CHECK_FALSE(m.Holds());
    CHECK(m.outcome() == Outcome::Cancelled);
    CHECK(m.FinishedNow());
    m.Cancel();
    CHECK_FALSE(m.FinishedNow());   // nothing was holding

    m.Arm(5000, false);
    CHECK(m.Holds());
    CHECK(m.outcome() == Outcome::None);
    CHECK_FALSE(m.LeftLevel());
    CHECK(m.ExitAtMs() == 0);
    CHECK(m.ArmedAtMs() == 5000);
}

TEST_CASE("RELOAD-6 cycle: an answer outside the request phase changes nothing") {
    Machine m = AcceptedAt1010();
    m.Answer(RequestAnswer::Refused, 1011);
    CHECK(m.phase() == Phase::AwaitExit);
    CHECK(m.Holds());
}

TEST_CASE("RELOAD-6 cycle: every phase and outcome has a name for the log") {
    using replay::reloadcycle::OutcomeName;
    using replay::reloadcycle::PhaseName;
    for (int i = 0; i <= static_cast<int>(Phase::Done); ++i)
        CHECK(std::string(PhaseName(static_cast<Phase>(i))) != "?");
    for (int i = 0; i <= static_cast<int>(Outcome::Cancelled); ++i)
        CHECK(std::string(OutcomeName(static_cast<Outcome>(i))) != "?");
}
