// DESIGN-HOST-SYNC P1: the host clock pin -- armed only while loaded and allowed, every sweep,
// read back, refused once and loudly, and never written at unload.

#include "doctest.h"

#include "engine/scriptgraph_facts.h"
#include "fake_script_graph.h"
#include "playback/core/clockpin.h"
#include "playback/core/levelspace.h"

using namespace replay;
namespace sgf = replay::scriptgraph_facts;

namespace {

constexpr uint64_t kSpaceA = 1;
constexpr uint64_t kSpaceB = 2;

clockpin::Policy Loaded(bool allowed = true, bool enabled = true, uint64_t space = kSpaceA) {
    clockpin::Policy p;
    p.loaded = true;
    p.space = space;
    p.allowed = allowed;
    p.enabled = enabled;
    return p;
}

}  // namespace

TEST_CASE("CLOCKPIN: Decide arms only when loaded, allowed and enabled") {
    for (int mask = 0; mask < 8; ++mask) {
        clockpin::Policy p;
        p.loaded = (mask & 1) != 0;
        p.allowed = (mask & 2) != 0;
        p.enabled = (mask & 4) != 0;
        const clockpin::Decision d = clockpin::Decide(p);
        CHECK(d.arm == (mask == 7));
        CHECK(d.value == clockpin::kPinValue);
    }
}

TEST_CASE("CLOCKPIN: every loaded sweep pins game_timer through the setter and reads it back") {
    FakeScriptGraphEntity e;
    e.variable = 719.5f;
    clockpin::Pin pin;

    clockpin::Report r = pin.Step(e, Loaded(), 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 ms, not the CR15Game vtable slot
    CHECK(r.event == clockpin::Event::Armed);
    CHECK(r.firstSight);
    CHECK(r.before == 719.5f);
    CHECK(r.after == clockpin::kPinValue);
    CHECK(e.lastSetName == sgf::kSymGameTimer);
    CHECK(e.lastSetId == sgf::kGameTimerId);

    for (uint64_t tick = 1; tick <= 10; ++tick) {
        e.variable -= 0.011f;   // one graph update of decrement between sweeps
        r = pin.Step(e, Loaded(), 1000 + tick * 11);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 ms, not the CR15Game vtable slot
        CHECK(r.event == clockpin::Event::Pinned);
        CHECK_FALSE(r.firstSight);
        CHECK(r.before > clockpin::kPinValue - 1.0f);
    }
    CHECK(e.setCalls == 11);   // every sweep, not every other one
    CHECK(e.findCalls == 1);   // the ref is cached
}

TEST_CASE("CLOCKPIN: not loaded or not allowed writes nothing") {
    FakeScriptGraphEntity e;
    e.variable = 300.0f;
    clockpin::Pin pin;

    SUBCASE("never loaded") {
        clockpin::Policy p;
        CHECK(pin.Step(e, p, 0).event == clockpin::Event::Idle);
        CHECK(e.setCalls == 0);
    }
    SUBCASE("live session: loaded but not allowed -- measured, never written") {
        const clockpin::Report r = pin.Step(e, Loaded(false), 0);
        CHECK(r.event == clockpin::Event::Observed);
        CHECK(r.haveBefore);
        CHECK(e.setCalls == 0);
        CHECK(e.variable == 300.0f);
    }
    SUBCASE("disabled by config") {
        CHECK(pin.Step(e, Loaded(true, false), 0).event == clockpin::Event::Observed);
        CHECK(e.setCalls == 0);
    }
}

TEST_CASE("CLOCKPIN: the unload tick releases without writing, and later ticks are idle") {
    FakeScriptGraphEntity e;
    e.variable = 500.0f;
    clockpin::Pin pin;
    pin.Step(e, Loaded(), 0);
    pin.Step(e, Loaded(), 10);
    REQUIRE(e.setCalls == 2);
    e.variable = 3599.9f;

    clockpin::Policy unloaded;
    unloaded.space = kSpaceA;   // an unload leaves the level standing
    const clockpin::Report r = pin.Step(e, unloaded, 20);
    CHECK(r.event == clockpin::Event::Released);
    CHECK(r.haveBefore);
    CHECK(r.before == doctest::Approx(3599.9f));
    CHECK(e.setCalls == 2);
    CHECK(e.variable == doctest::Approx(3599.9f));   // left, not restored to 500

    for (int i = 0; i < 5; ++i) CHECK(pin.Step(e, unloaded, 30 + i).event == clockpin::Event::Idle);
    CHECK(e.setCalls == 2);

    // The next load starts a fresh ledger: a new first sight, a new ARMED, and a new walk -- the
    // release dropped the ref, and 100 ms is well inside the re-walk interval.
    const uint32_t finds = e.findCalls;
    const clockpin::Report again = pin.Step(e, Loaded(), 100);
    CHECK(again.event == clockpin::Event::Armed);
    CHECK(again.firstSight);
    CHECK(e.findCalls == finds + 1);
}

TEST_CASE("CLOCKPIN: a read-back mismatch refuses once and never retries this load") {
    FakeScriptGraphEntity e;
    e.variable = 700.0f;
    clockpin::Pin pin;

    SUBCASE("the setter answers 1 but nothing changed -- a binding that is not `set`") {
        e.setStores = false;
    }
    SUBCASE("the setter answers 0") {
        e.setAnswers = false;
    }
    const clockpin::Report r = pin.Step(e, Loaded(), 0);
    CHECK(r.event == clockpin::Event::Refused);
    CHECK(e.setCalls == 1);
    for (int i = 1; i < 50; ++i) {
        CHECK(pin.Step(e, Loaded(), uint64_t(i) * 10).event == clockpin::Event::Observed);
    }
    CHECK(e.setCalls == 1);   // no retry storm
}

TEST_CASE("CLOCKPIN: a lost graph is re-walked at most once a second") {
    FakeScriptGraphEntity e;
    e.present = false;
    clockpin::Pin pin;

    CHECK(pin.Step(e, Loaded(), 0).event == clockpin::Event::NotFound);
    for (uint64_t ms = 10; ms < clockpin::kRewalkIntervalMs; ms += 10) {
        CHECK(pin.Step(e, Loaded(), ms).event == clockpin::Event::Waiting);
    }
    CHECK(e.findCalls == 1);
    CHECK(pin.Step(e, Loaded(), clockpin::kRewalkIntervalMs).event == clockpin::Event::NotFound);
    CHECK(e.findCalls == 2);

    e.present = true;
    CHECK(pin.Step(e, Loaded(), 2000).event == clockpin::Event::Armed);
    // Invalidated (the level reloaded its scripts): the next walk waits out the interval.
    e.valid = false;
    CHECK(pin.Step(e, Loaded(), 2100).event == clockpin::Event::Waiting);
    CHECK(pin.Step(e, Loaded(), 3000).event == clockpin::Event::Pinned);
    CHECK(e.findCalls == 4);
}

// A level cycle builds a fresh space; the old graph must never be written again.
TEST_CASE("CLOCKPIN: a new level space re-walks at once and pins the new graph, not the old one") {
    FakeScriptGraphEntity e;
    e.variable = 700.0f;
    clockpin::Pin pin;
    REQUIRE(pin.Step(e, Loaded(), 0).event == clockpin::Event::Armed);
    REQUIRE(pin.Step(e, Loaded(), 10).event == clockpin::Event::Pinned);
    REQUIRE(e.findCalls == 1);

    e.rebuilt = true;           // the old instance still reads valid
    e.variable = 719.0f;
    const clockpin::Report r = pin.Step(e, Loaded(true, true, kSpaceB), 20);
    CHECK(e.findCalls == 2);    // inside the re-walk interval, and walked anyway
    CHECK(r.ref.inst == &e.rebuiltToken);
    CHECK(r.event == clockpin::Event::Armed);   // a new level's ARMED and first sight
    CHECK(r.firstSight);
    CHECK(e.variable == clockpin::kPinValue);
    CHECK(pin.Step(e, Loaded(true, true, kSpaceB), 30).ref.inst == &e.rebuiltToken);
}

TEST_CASE("CLOCKPIN: with no level space (the cycle holds) the pin touches nothing") {
    FakeScriptGraphEntity e;
    clockpin::Pin pin;
    REQUIRE(pin.Step(e, Loaded(), 0).event == clockpin::Event::Armed);
    const uint32_t finds = e.findCalls, gets = e.getCalls, sets = e.setCalls;

    for (uint64_t ms = 10; ms < 3000; ms += 10) {
        CHECK(pin.Step(e, Loaded(true, true, 0), ms).event == clockpin::Event::Waiting);
    }
    CHECK(e.findCalls == finds);
    CHECK(e.getCalls == gets);
    CHECK(e.setCalls == sets);

    // An unload during the hold releases without reading the graph it can no longer vouch for.
    clockpin::Policy unloaded;
    const clockpin::Report released = pin.Step(e, unloaded, 3000);
    CHECK(released.event == clockpin::Event::Released);
    CHECK_FALSE(released.haveBefore);
    CHECK(e.getCalls == gets);
}

TEST_CASE("CLOCKPIN: the hold ending in the same space still re-walks, since the ref was dropped") {
    FakeScriptGraphEntity e;
    clockpin::Pin pin;
    REQUIRE(pin.Step(e, Loaded(), 0).event == clockpin::Event::Armed);
    pin.Step(e, Loaded(true, true, 0), 10);
    e.rebuilt = true;
    const clockpin::Report r = pin.Step(e, Loaded(), 20);
    CHECK(e.findCalls == 2);
    CHECK(r.ref.inst == &e.rebuiltToken);
    CHECK(r.event == clockpin::Event::Armed);
}

TEST_CASE("CLOCKPIN: Forget (cycle arm, cycle done, Load) drops the ref and the refusal latch") {
    FakeScriptGraphEntity e;
    e.setStores = false;
    clockpin::Pin pin;
    REQUIRE(pin.Step(e, Loaded(), 0).event == clockpin::Event::Refused);
    REQUIRE(pin.Step(e, Loaded(), 10).event == clockpin::Event::Observed);

    pin.Forget();
    e.setStores = true;
    e.rebuilt = true;
    const clockpin::Report r = pin.Step(e, Loaded(), 20);
    CHECK(e.findCalls == 2);                    // re-walked inside the interval
    CHECK(r.ref.inst == &e.rebuiltToken);
    CHECK(r.event == clockpin::Event::Armed);   // the refusal was the old graph's
    CHECK(r.firstSight);
}

TEST_CASE("LEVELSPACE: the key is 0 whenever no space may be touched, and follows the bind epoch") {
    using replay::levelspace::Key;
    for (int mask = 0; mask < 8; ++mask) {
        const bool holds = (mask & 1) != 0, inGame = (mask & 2) != 0, bound = (mask & 4) != 0;
        CAPTURE(mask);
        CHECK((Key(holds, inGame, bound, 5) != 0) == (!holds && inGame && bound));
    }
    CHECK(Key(false, true, true, 0) != 0);   // the first binding is still a space
    CHECK(Key(false, true, true, 7) != Key(false, true, true, 8));
    CHECK(Key(false, true, true, 7) == Key(false, true, true, 7));
}

// HOST-SYNC-3 item 2: RUN-VERIFY-4's host had no arena graph on the bound game space, and the walk
// never looked anywhere else. DESIGN-HOST-SYNC 1.4 step 6 always said to walk the chain.
TEST_CASE("HOST-SYNC-3: the arena graph is found on a later space of the chain and pinned there") {
    FakeScriptGraphEntity e;
    uint8_t child = 0, level = 0, root = 0;
    e.chain = {&child, &level, &root};
    e.presentIn = &root;
    clockpin::Pin pin;

    const clockpin::Report r = pin.Step(e, Loaded(), 0);
    CHECK(r.event == clockpin::Event::Armed);
    CHECK(r.spacesWalked == 3);
    REQUIRE(e.walked.size() == 3);
    CHECK(e.walked[0] == &child);
    CHECK(e.walked[2] == &root);
    CHECK(e.variable == clockpin::kPinValue);

    // Pinned from the cached ref: no second walk.
    CHECK(pin.Step(e, Loaded(), 100).event == clockpin::Event::Pinned);
    CHECK(e.walked.size() == 3);
}

TEST_CASE("HOST-SYNC-3: a miss walks the whole chain once a second; no chain walks the bound space") {
    FakeScriptGraphEntity e;
    uint8_t a = 0, b = 0;
    e.chain = {&a, &b};
    e.present = false;
    clockpin::Pin pin;

    const clockpin::Report r = pin.Step(e, Loaded(), 0);
    CHECK(r.event == clockpin::Event::NotFound);
    CHECK(r.spacesWalked == 2);
    REQUIRE(e.walked.size() == 2);
    CHECK(e.walked[1] == &b);
    CHECK(pin.Step(e, Loaded(), 500).event == clockpin::Event::Waiting);
    CHECK(e.walked.size() == 2);
    CHECK(e.setCalls == 0);

    e.chain.clear();
    CHECK(pin.Step(e, Loaded(), clockpin::kRewalkIntervalMs).event == clockpin::Event::NotFound);
    REQUIRE(e.walked.size() == 3);
    CHECK(e.walked[2] == nullptr);   // nullptr is the entity's bound game space
}
