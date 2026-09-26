// ENG-SPECTATOR-VIEWER + ARCH-23 S14.10 W2-B-31. The viewer's role: the label a run prints for
// each of the eight values the engine's 3-bit team field can hold, the predicate that decides
// whether the shipping spectator camera comes on at all, and the V0..V4 chain that SEATS the
// viewer as a spectator through the engine's own local-seat call.
//
// The set {2, 4} is asserted THREE ways, and only one of them is a list. The list is here so a
// silent widening of the mask is caught; the other two are the engine's own expression
// (GAME_SPECTATOR_GATE_MASK, read from CAM.SPECTATOR_TEAM_GATE) and the registry's own spectator
// constant. A test that only replayed the shipped predicate back at itself would pass with the
// mask set to 0xFFFF, which accepts every even role.
//
// ★ NO CASE BELOW TAKES A REFUSAL AS ITS PASS. T1 and T3 assert the ISSUED state -- one call
// recorded AND the record reading spectator afterwards. The refusal cases are separate and say so
// in their names.

#include "doctest.h"

#include <windows.h>

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "bootstrap/bootstrap.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/services.h"
#include "engine/pawn_provision.h"
#include "generated/facts_generated.h"
#include "playback/core/viewer_role.h"

using namespace replay::playback;

namespace {

// The whole population, not a sample: the field is three bits wide, so eight values IS every value
// this function can ever be called with.
constexpr uint16_t kRoleValues = 8;

// The word CAM.SPECTATOR_TEAM_GATE substitutes for an out-of-range team, written as the complement
// of zero so this file names no value the registry also carries.
constexpr uint16_t kAllBits = uint16_t(~uint16_t(0));

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
        for (const Line& l : lines) {
            if (l.text.find(needle) != std::string::npos) ++n;
        }
        return n;
    }

    std::vector<Line> lines;
};

// A netgame with room for four entrant records, laid out at the offsets the shipped reader uses.
// Not a copy of the layout: every displacement below is the registry symbol viewer_role.cpp reads
// through, so a fixture built against a stale offset cannot agree with the code under test.
class FakeNetGame {
public:
    FakeNetGame()
        : mem_(size_t(facts::LOCAL_NETGAME_USER_RECORD_BASE_OFF) + 4u * facts::USER_STRIDE, 0) {
        SetState(facts::NETGAME_STATE_INMATCH);
        SetLocalUsers(1);
        // Entrant 1 carries bits OUTSIDE the team field as well, so T7 can say the whole word is
        // untouched rather than only the three bits the seat writes.
        Word(1) = 0x1234;
        SetRole(1, 1);
    }

    void* Ng() { return mem_.data(); }

    void SetState(uint32_t s) { memcpy(mem_.data(), &s, sizeof s); }
    void SetLocalUsers(uint16_t n) {
        memcpy(mem_.data() + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &n, sizeof n);
    }

    uint16_t& Word(uint16_t index) {
        return *reinterpret_cast<uint16_t*>(mem_.data() +
                                            facts::LOCAL_NETGAME_USER_RECORD_BASE_OFF +
                                            size_t(index) * facts::USER_STRIDE);
    }

    void SetRole(uint16_t index, uint16_t role) {
        const uint16_t field = uint16_t(uint16_t(facts::IDENT_USER_TEAM_MASK)
                                        << facts::IDENT_USER_TEAM_SHIFT);
        const uint16_t bits = uint16_t(uint16_t(role & facts::IDENT_USER_TEAM_MASK)
                                       << facts::IDENT_USER_TEAM_SHIFT);
        Word(index) = uint16_t(uint16_t(Word(index) & uint16_t(~field)) | bits);
    }

    uint16_t Role(uint16_t index) {
        uint16_t out = kAllBits;
        ReadEntrantRole(Ng(), index, out);
        return out;
    }

private:
    std::vector<uint8_t> mem_;
};

// The stand-in for IDENT.SERVER_ASSIGN_TEAM: it records what it was asked for and, unless the case
// says otherwise, does what the engine's own call does -- writes the role into the record.
struct SeatEngine {
    unsigned calls = 0;
    uint16_t sawIndex = kAllBits;
    uint16_t sawTeam = kAllBits;
    void* sawCtx = nullptr;
    FakeNetGame* game = nullptr;
    bool applies = true;   // false: the call is accepted and the record does not move
    bool answers = true;   // what the call RETURNS, which is never the verdict
};

bool SeatAssign(void* ctx, uint16_t index, uint16_t team) {
    SeatEngine* e = static_cast<SeatEngine*>(ctx);
    ++e->calls;
    e->sawIndex = index;
    e->sawTeam = team;
    e->sawCtx = ctx;
    if (e->applies && e->game != nullptr) e->game->SetRole(index, team);
    return e->answers;
}

replay::PawnProvision::ActorHops HopsFor(SeatEngine& e) {
    replay::PawnProvision::ActorHops h;
    h.ctx = &e;
    h.assignTeam = SeatAssign;
    return h;
}

// V1's deferral budget, taken from the design rather than from the implementation's private
// constant: a case that read the shipped number could not notice it being lowered to 1.
constexpr unsigned kAutoPickPumps = 240;

}  // namespace

TEST_CASE("every role the 3-bit field can hold has its own word") {
    const char* const expected[kRoleValues] = {"blue",         "orange",       "spectator",
                                               "non-team",     "moderator",    "out-of-range",
                                               "out-of-range", "no-team"};
    for (uint16_t role = 0; role < kRoleValues; ++role) {
        CAPTURE(role);
        CHECK(strcmp(ViewerRoleName(role), expected[role]) == 0);
    }
    // Two labels are load-bearing and the rest are prose, so the two are pinned to the registry
    // rather than to this file's memory of them.
    CHECK(strcmp(ViewerRoleName(uint16_t(facts::GAME_TEAM_SPECTATOR)), "spectator") == 0);
    // Above the field's width the answer is a word, never a read past the table.
    CHECK(strcmp(ViewerRoleName(kRoleValues), "out-of-range") == 0);
    CHECK(strcmp(ViewerRoleName(kAllBits), "out-of-range") == 0);
    // The all-ones state is the value V1 waits out, and it is the mask itself rather than a 7.
    CHECK(RoleNoTeam() == uint16_t(facts::IDENT_USER_TEAM_MASK));
    CHECK(strcmp(ViewerRoleName(RoleNoTeam()), "no-team") == 0);
}

TEST_CASE("the spectating set is exactly {spectator, moderator}") {
    for (uint16_t role = 0; role < kRoleValues; ++role) {
        CAPTURE(role);
        // The list. Independent of the shipped expression, which is the point of writing it out.
        const bool wanted = (role == 2 || role == 4);
        CHECK(RoleSpectates(role) == wanted);
    }
    CHECK(RoleSpectates(uint16_t(facts::GAME_TEAM_SPECTATOR)));
    // The two teams that PLAY are the two the owner has been landing on, and they are the failure
    // this whole packet exists to make visible.
    CHECK_FALSE(RoleSpectates(0));
    CHECK_FALSE(RoleSpectates(1));
    // 3 is a participating non-team role: the gate is false for it, so -lobbyteam 3 would not do.
    CHECK_FALSE(RoleSpectates(3));
    CHECK_FALSE(RoleSpectates(RoleNoTeam()));
}

TEST_CASE("the mask is the engine's, and the label table agrees with the predicate") {
    // CAM.SPECTATOR_TEAM_GATE clears bit 1 of the difference and nothing else; a mask that had
    // widened would accept roles the engine refuses, and the run would report a spectator seat it
    // does not have.
    CHECK(uint16_t(~uint16_t(facts::GAME_SPECTATOR_GATE_MASK)) == uint16_t(1u << 1));
    CHECK(facts::GAME_TEAM_SPECTATOR == 2u);
    // The gate first maps a role of 5 or more to 0xFFFF and viewer_role.cpp does not reproduce that
    // clamp, on the stated ground that it changes no answer for a 3-bit field. That ground is
    // checked here rather than asserted: the clamped form is computed alongside the shipped one.
    for (uint16_t role = 0; role < kRoleValues; ++role) {
        CAPTURE(role);
        const uint16_t clamped = role < 5 ? role : kAllBits;
        const bool engineForm =
            uint16_t(uint16_t(clamped - uint16_t(facts::GAME_TEAM_SPECTATOR)) &
                     uint16_t(facts::GAME_SPECTATOR_GATE_MASK)) == 0;
        CHECK(RoleSpectates(role) == engineForm);
    }
    // The refusal line and the label must name the same thing: every role the predicate rejects has
    // a name that is not "spectator", and the one it accepts beside the spectator is the moderator.
    for (uint16_t role = 0; role < kRoleValues; ++role) {
        CAPTURE(role);
        if (RoleSpectates(role)) {
            CHECK(((strcmp(ViewerRoleName(role), "spectator") == 0) ||
                   (strcmp(ViewerRoleName(role), "moderator") == 0)));
        } else {
            CHECK(strcmp(ViewerRoleName(role), "spectator") != 0);
        }
    }
}

TEST_CASE("the packed role is read out of the entrant record at the engine's own stride") {
    FakeNetGame game;
    game.SetRole(0, 5);
    game.SetRole(2, 3);
    uint16_t role = kAllBits;
    REQUIRE(ReadEntrantRole(game.Ng(), 0, role));
    CHECK(role == 5);
    REQUIRE(ReadEntrantRole(game.Ng(), 1, role));
    CHECK(role == 1);
    REQUIRE(ReadEntrantRole(game.Ng(), 2, role));
    CHECK(role == 3);
    // A null netgame is refused rather than answered: there is no record to read, and a zero would
    // be indistinguishable from a viewer seated on blue.
    role = kAllBits;
    CHECK_FALSE(ReadEntrantRole(nullptr, 0, role));
    CHECK(role == kAllBits);
}

// T1. THE PASS CASE, and it asserts the ISSUED STATE: one call, for entrant 0, with the registry's
// own spectator constant, and the RECORD reading spectator afterwards.
// BACKLOG-1 O1. The seat's engine call moves the person at the keyboard to spectator, so it now
// asks the process predicate before anything else -- including the scope term, which SEAT_IN_CAPTURE
// compiles out. This binary never installs the offline-bit lock, so every case below would be
// measuring that gate instead of the chain unless it is pinned contained first.
namespace {
const struct PinContained {
    PinContained() { replay::livesafety::SetForTest(/*liveOnlineExempt=*/false,
                                                    /*offlineLockHeld=*/true); }
} g_pin_contained;
}  // namespace

TEST_CASE("BACKLOG-1 O1: an uncontained process issues no seat and calls nothing") {
    FakeNetGame game;
    game.SetRole(0, 1);            // blue: the chain would otherwise issue
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    RecordingLog log;

    // Both uncontained verdicts, and the scope is Playback in each -- the load gate cannot carry
    // this case, so only the predicate can refuse it.
    const bool arms[2][2] = {{true, false}, {false, false}};
    for (const auto& arm : arms) {
        replay::livesafety::SetForTest(arm[0], arm[1]);
        ViewerSeat seat;
        CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::Uncontained);
        CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::Uncontained);
        CHECK(seat.Issues() == 0);
        CHECK(seat.Pumps() == 0);      // the pump counter is below the gate too
        CHECK(eng.calls == 0);
        CHECK(game.Role(0) == 1);      // the record the engine gave it, untouched
    }

    // The control: the same roles, the same hops, contained -- it issues. Without this the case
    // would pass against a chain that had simply stopped seating anybody.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    ViewerSeat seated;
    CHECK(seated.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::Issued);
    CHECK(eng.calls == 1);
}

TEST_CASE("W2-B-31 T1: V3 issues the seat for entrant 0 and the re-read record says spectator") {
    FakeNetGame game;
    game.SetRole(0, 1);
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    RecordingLog log;
    ViewerSeat seat;

    CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::Issued);

    CHECK(eng.calls == 1);
    CHECK(eng.sawIndex == 0);
    CHECK(eng.sawTeam == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(eng.sawCtx == hops.ctx);
    CHECK(game.Role(0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(RoleSpectates(game.Role(0)));
    CHECK(seat.Issues() == 1);
    // Not one refusal word appears on the pass.
    CHECK(log.Count("ViewerSeatRefused") == 0);
    CHECK(log.Count("ViewerSeatNotIssued") == 0);
    CHECK(log.Count("ViewerAutoPickNotObserved") == 0);
}

// T2. The engine call that is accepted and changes nothing -- IDENT.SERVER_ASSIGN_TEAM's own
// silent early-out. The verdict must come from the record, not from the call's answer.
TEST_CASE("W2-B-31 T2: a call that does not move the record is ViewerSeatRefused") {
    FakeNetGame game;
    game.SetRole(0, 1);
    SeatEngine eng;
    eng.game = &game;
    eng.applies = false;
    eng.answers = true;  // the call says yes; the record says no
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    RecordingLog log;
    ViewerSeat seat;

    CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::PostconditionFailed);
    CHECK(eng.calls == 1);
    CHECK(game.Role(0) == 1);
    CHECK(log.Count("ViewerSeatRefused") == 1);
}

// T3b. A REFUSAL case, kept apart from T1/T3 and never counted as a pass: the route is a dead
// letter when IIdentity::ActorHops() answers null, and it says so by name.
TEST_CASE("W2-B-31 T3b: a null hop surface refuses ViewerSeatNotIssued and calls nothing") {
    FakeNetGame game;
    game.SetRole(0, 1);
    RecordingLog log;
    ViewerSeat seat;

    for (unsigned i = 0; i < kAutoPickPumps; ++i) {
        CHECK(seat.Pump(SeatScope::Playback, game.Ng(), nullptr, &log) == SeatStep::NoHopSurface);
    }
    CHECK(seat.Issues() == 0);
    CHECK(game.Role(0) == 1);
    CHECK(log.Count("ViewerSeatNotIssued") == 1);
    CHECK(log.Count("ActorHops() answered null") == 1);
    // A surface that exists but carries no bind is a DIFFERENT answer, and the line names it.
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops unbound = HopsFor(eng);
    unbound.assignTeam = nullptr;
    ViewerSeat second;
    RecordingLog log2;
    for (unsigned i = 0; i < kAutoPickPumps; ++i) {
        CHECK(second.Pump(SeatScope::Playback, game.Ng(), &unbound, &log2) == SeatStep::NoAssignTeamBind);
    }
    CHECK(eng.calls == 0);
    CHECK(log2.Count("no IDENT.SERVER_ASSIGN_TEAM bind") == 1);
}

// T4. The all-ones state is "the engine has not seated anybody yet", so the chain WAITS. It must
// not issue into a record the engine has not written, and after the budget it says so by name --
// carrying the role it kept reading, which is the measurement the refusal exists to deliver.
TEST_CASE("W2-B-31 T4: no team at all defers, refuses by name, and seats once the engine does") {
    FakeNetGame game;
    game.SetRole(0, RoleNoTeam());
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    RecordingLog log;
    ViewerSeat seat;

    for (unsigned i = 0; i < kAutoPickPumps; ++i) {
        CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::AwaitingSeat);
    }
    CHECK(eng.calls == 0);
    CHECK(log.Count("ViewerAutoPickNotObserved") == 1);
    CHECK(log.Count("(role=7)") == 1);
    CHECK(log.Count("ViewerSeatRefused") == 0);

    // The engine finally auto-picks a playing team, which is the failure the owner reported.
    game.SetRole(0, 1);
    CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::Issued);
    CHECK(eng.calls == 1);
    CHECK(game.Role(0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
}

// T5. Already in the gate's set: zero calls, and the chain says so as a PASS rather than as a
// refusal. `-lobbyteam 2` being honoured one day would land exactly here.
TEST_CASE("W2-B-31 T5: a viewer the engine already seated as spectator is not re-seated") {
    FakeNetGame game;
    game.SetRole(0, uint16_t(facts::GAME_TEAM_SPECTATOR));
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    RecordingLog log;
    ViewerSeat seat;

    for (int i = 0; i < 10; ++i) {
        CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::AlreadySeated);
    }
    CHECK(eng.calls == 0);
    CHECK(seat.Issues() == 0);
    CHECK(game.Role(0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(log.lines.size() == 1);  // the preseat measurement, and nothing else
    // The moderator seat passes the same gate and is likewise left alone.
    FakeNetGame mod;
    mod.SetRole(0, 4);
    SeatEngine modEng;
    modEng.game = &mod;
    replay::PawnProvision::ActorHops modHops = HopsFor(modEng);
    ViewerSeat modSeat;
    CHECK(modSeat.Pump(SeatScope::Playback, mod.Ng(), &modHops, nullptr) == SeatStep::AlreadySeated);
    CHECK(modEng.calls == 0);
}

// T6. V4's bound. An engine that keeps re-seating the local user (LobbyJoinAcceptedCB legitimately
// re-issues its own assign) gets three answers and then a named refusal -- never an unbounded
// fight for the same word.
TEST_CASE("W2-B-31 T6: a role that keeps snapping back costs exactly three issues") {
    FakeNetGame game;
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    RecordingLog log;
    ViewerSeat seat;

    // WINDOWS, and they are real ones: kPumpsPerWindow pumps each, so the case exercises V4 at
    // the cadence S14.10 specifies rather than burning three issues inside three ticks.
    for (unsigned window = 0; window < 8; ++window) {
        game.SetRole(0, 1);  // the engine puts the viewer back on orange every window
        const SeatStep first = seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log);
        CHECK(first == (window < 3 ? SeatStep::Issued : SeatStep::BudgetSpent));
        for (unsigned i = 1; i < kPumpsPerWindow; ++i) {
            const SeatStep step = seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log);
            // Inside a window that already issued, the record reads spectator, so V2 answers.
            CHECK(step == (window < 3 ? SeatStep::AlreadySeated : SeatStep::BudgetSpent));
        }
    }
    CHECK(eng.calls == 3);
    CHECK(seat.Issues() == 3);
    CHECK(log.Count("ViewerSeatRefused") == 1);
}

// T7. The seat is entrant 0's and nobody else's: a recorded entrant's word must come out of the
// pump byte-identical, team bits and the bits around them alike.
TEST_CASE("W2-B-31 T7: entrant 1's record is untouched by the viewer's seat") {
    FakeNetGame game;
    game.SetRole(0, 1);
    const uint16_t before = game.Word(1);
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    ViewerSeat seat;

    CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, nullptr) == SeatStep::Issued);
    CHECK(eng.sawIndex == 0);
    CHECK(game.Word(1) == before);
    CHECK(game.Role(1) == 1);
}

// T8. The pre-seat measurement AUD-S14 F4/R1-F2 left open, and the ONE thing that can close V1's
// residual: the value the record held before anything of ours touched it, printed once, before any
// issue. The line's shape is the acceptance's, so a rename here fails the L3 clause here first.
TEST_CASE("W2-B-31 T8: the first emission carries the pre-seat role, before any issue") {
    FakeNetGame game;
    game.SetRole(0, 1);
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    RecordingLog log;
    ViewerSeat seat;

    CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::Issued);

    REQUIRE(log.lines.size() >= 1);
    // FIRST line of the session, and the call had not been made when it was written.
    CHECK(log.lines[0].module == kViewerLogModule);
    CHECK(log.lines[0].text == "viewer slot 0 role=1 (orange) preseat");
    CHECK(seat.PreseatMeasured());
    CHECK(seat.PreseatRole() == 1);
    // Once per session, not once per pump.
    for (int i = 0; i < 5; ++i) seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log);
    CHECK(log.Count("preseat") == 1);
    // And it is the PRE-seat value: the record moved and the measurement did not follow it.
    CHECK(game.Role(0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(seat.PreseatRole() == 1);
}

// V0's terms are preconditions for READING the record, so they must be silent deferrals: a
// plugin that ticks before the engine has a netgame would otherwise print a refusal on every
// session, and a refusal that always fires cannot report anything.
//
// AND SO IS THE STATE. Before NETGAME.STATE_INMATCH the engine has not been
// asked for a seat, so no budget is spent, no refusal word is printed, and the only line is an
// Info deferral at most once per window.
TEST_CASE("W2-B-31: V0 and the pre-match state defer silently and spend no budget") {
    RecordingLog log;
    ViewerSeat seat;
    CHECK(seat.Pump(SeatScope::Playback, nullptr, nullptr, &log) == SeatStep::NoNetGame);

    FakeNetGame game;
    game.SetLocalUsers(0);
    CHECK(seat.Pump(SeatScope::Playback, game.Ng(), nullptr, &log) == SeatStep::NoLocalUser);
    CHECK(log.lines.size() == 0);

    game.SetLocalUsers(1);
    game.SetRole(0, 1);
    game.SetState(facts::NETGAME_STATE_INMATCH - 1);
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    // A FRESH chain and a fresh log for the wait, so the deferral count below is exactly four
    // windows and not four windows plus whatever partial window the two V0 pumps above sat in.
    ViewerSeat waiting;
    RecordingLog wlog;
    ViewerSeat& seatRef = waiting;
    RecordingLog& logRef = wlog;
    for (unsigned i = 0; i < 4 * kPumpsPerWindow; ++i) {
        CHECK(seatRef.Pump(SeatScope::Playback, game.Ng(), &hops, &logRef) == SeatStep::SessionNotInMatch);
    }
    CHECK(eng.calls == 0);
    CHECK_FALSE(seatRef.InMatchSeen());
    // Not one word from the acceptance's refusal set, however long the wait runs.
    CHECK(logRef.Count("ViewerSeatNotIssued") == 0);
    CHECK(logRef.Count("ViewerSeatRefused") == 0);
    CHECK(logRef.Count("ViewerAutoPickNotObserved") == 0);
    // One deferral line per window, not per pump.
    CHECK(logRef.Count("viewer seat: deferred (SessionNotInMatch)") == 4);
    CHECK(logRef.Count("preseat") == 1);

    // The session reaches the state and the seat lands, with the whole budget still unspent.
    game.SetState(facts::NETGAME_STATE_INMATCH);
    CHECK(seatRef.Pump(SeatScope::Playback, game.Ng(), &hops, &logRef) == SeatStep::Issued);
    CHECK(seatRef.InMatchSeen());
    CHECK(game.Role(0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(logRef.Count("ViewerSeatNotIssued") == 0);
}

// THE F1 CASE, in the shape the field produces it: L3 #28 saw entrant 0 readable and on a
// PLAYING team while the session was still pre-match, and state 9 arrives seconds later. A run
// like that must reach the seat carrying ZERO refusal words -- otherwise the L3 clause "no
// refusal after the seat" cannot be told from "this build always refuses".
TEST_CASE("W2-B-31 F1: a session that reaches state 9 late seats once and refuses nothing") {
    FakeNetGame game;
    game.SetRole(0, 1);
    game.SetState(facts::NETGAME_STATE_INMATCH - 2);
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    RecordingLog log;
    ViewerSeat seat;

    // 480 pumps -- twice the old refusal budget -- of a session that is simply not there yet.
    for (unsigned i = 0; i < 480; ++i) {
        CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::SessionNotInMatch);
    }
    CHECK(eng.calls == 0);

    game.SetState(facts::NETGAME_STATE_INMATCH);
    CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::Issued);
    for (unsigned i = 0; i < 2 * kPumpsPerWindow; ++i) {
        CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, &log) == SeatStep::AlreadySeated);
    }

    CHECK(eng.calls == 1);
    CHECK(seat.Issues() == 1);
    CHECK(game.Role(0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
    CHECK(RoleSpectates(game.Role(0)));
    CHECK(log.Count("ViewerSeatNotIssued") == 0);
    CHECK(log.Count("ViewerSeatRefused") == 0);
    CHECK(log.Count("ViewerAutoPickNotObserved") == 0);
}

// F2. A pump that finds the shared stage-0 latch held is a deferral under ITS OWN name.
// Reporting it as the state term would name the wrong cause on the one occasion it fires.
TEST_CASE("W2-B-31: a held engine-call latch defers under its own name and issues nothing") {
    FakeNetGame game;
    game.SetRole(0, 1);
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    ViewerSeat seat;
    {
        replay::PawnProvision::EngineCallLatch held;
        REQUIRE(held.taken);
        CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, nullptr) == SeatStep::EngineCallInFlight);
        CHECK(eng.calls == 0);
    }
    // Released, and the seat lands on a later pump.
    for (unsigned i = 0; i < kPumpsPerWindow; ++i) seat.Pump(SeatScope::Playback, game.Ng(), &hops, nullptr);
    CHECK(eng.calls == 1);
    CHECK(game.Role(0) == uint16_t(facts::GAME_TEAM_SPECTATOR));
}

// Reset is the load boundary: a second load assigns its own entrants, so the issue budget and the
// pre-seat measurement must not carry across it.
TEST_CASE("W2-B-31: Reset returns the chain to its pre-session state") {
    FakeNetGame game;
    SeatEngine eng;
    eng.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(eng);
    ViewerSeat seat;
    for (unsigned window = 0; window < 4; ++window) {
        game.SetRole(0, 1);
        for (unsigned i = 0; i < kPumpsPerWindow; ++i) seat.Pump(SeatScope::Playback, game.Ng(), &hops, nullptr);
    }
    CHECK(seat.Issues() == 3);
    CHECK(seat.PreseatMeasured());

    seat.Reset();
    CHECK(seat.Issues() == 0);
    CHECK_FALSE(seat.PreseatMeasured());
    game.SetRole(0, 1);
    CHECK(seat.Pump(SeatScope::Playback, game.Ng(), &hops, nullptr) == SeatStep::Issued);
    CHECK(eng.calls == 4);
}

// ================================================================================================
// The seat belongs to a PLAYBACK session and to nothing else.
//
// L2 #17 is the case this one answers: a `capture-local` launch with nothing open, the engine
// seated the recording user ORANGE, and this chain issued the spectator seat 1.2 s later
// (`[identity] viewer slot 0 role=1 (orange) preseat` -> `role=2 (spectator)` at 03:09:05.526).
// The physics pool then answered 0 bodies for the remaining 216 s and the capture carries no
// S_BODY_SAMPLED stream at all.  A capture's recording user IS the player.

TEST_CASE("SEAT-SCOPE: a capture session refuses by name, calls nothing and spends nothing") {
    FakeNetGame game;
    game.SetRole(0, 1);                     // orange, exactly as L2 #17 measured it
    SeatEngine engine;
    engine.game = &game;
    replay::PawnProvision::ActorHops hops = HopsFor(engine);
    RecordingLog log;

    ViewerSeat seat;
    // Well past both the census window and the issue budget: a gate that only defers would have
    // spent them by now.
    for (unsigned i = 0; i < kPumpsPerWindow * 3; ++i) {
        CHECK(seat.Pump(SeatScope::NoDemoOwned, game.Ng(), &hops, &log) == SeatStep::NotPlaybackSession);
    }
    CHECK(engine.calls == 0);
    CHECK(game.Role(0) == 1);
    CHECK(seat.Issues() == 0);
    CHECK(seat.Pumps() == 0);
    CHECK(seat.PreseatMeasured() == false);
    CHECK(seat.InMatchSeen() == false);

    // Named once, not once per tick, and none of the chain's other lines is emitted: no preseat
    // measurement, no deferral, no verdict about a seat nobody asked for.
    CHECK(log.Count("ViewerSeatNotPlayback") == 1);
    CHECK(log.Count("preseat") == 0);
    CHECK(log.Count("viewer seat: deferred") == 0);
    CHECK(log.Count("ViewerSeatNotIssued") == 0);

    // THE CONTROL: the same fixture, the same engine, the same role -- only the session mode
    // differs -- seats on the first pump.  Without this the case above would pass against a
    // chain that can no longer seat anything at all.
    ViewerSeat playback;
    RecordingLog playbackLog;
    CHECK(playback.Pump(SeatScope::Playback, game.Ng(), &hops, &playbackLog) == SeatStep::Issued);
    CHECK(engine.calls == 1);
    CHECK(game.Role(0) == facts::GAME_TEAM_SPECTATOR);
    CHECK(playbackLog.Count("ViewerSeatNotPlayback") == 0);
}
