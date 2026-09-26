// Binding a recorded speaker to a live receiver instance, and giving the slot back.
//
// ★ WHAT THESE ARMS EXIST TO PREVENT, in order of how much it would cost:
//
//   1. Releasing a slot the SESSION owned. That hands the owner's next real match a pool of
//      disconnected talkers -- worse than a replay with no voice, and it outlives the replay.
//   2. Binding a second instance for a user who already has one. Two instances carrying one id,
//      and the engine's dispatcher takes whichever its scan reaches first: voice routed by array
//      order, with nothing complaining.
//   3. Recording ownership of a slot we never actually got. Connect returns void, so believing
//      the call rather than reading back what it wrote produces a release of somebody else's slot.
//
// The fake Connect and Disconnect MUTATE the synthetic component system exactly as the engine's do,
// so the read-back is a real read of a real effect. The last arm supplies one that does nothing,
// which is the input that must make BindTalker refuse.

#include <string.h>

#include <vector>

#include "audio/audio_facts.h"
#include "audio/voip/voip_facts.h"
#include "audio/voip/voipplay.h"
#include "audio/voip/voipsite.h"
#include "doctest.h"
#include "fakeengine.h"

using namespace replay;
using namespace replay::audio;
using namespace replay::audio::voip;
using w3c::FakeEngine;

namespace {

// The engine's own "nobody is in this slot" id, supplied to the code under test the way the
// mapped image would supply it.
uint32_t g_sentinel = 0xFFFFFFFFu;

struct BindCalls {
    int connects = 0;
    int disconnects = 0;
    void* cs = nullptr;
    uint16_t poolIndex = 0xFFFF;
    uint32_t userId = 0;
};
BindCalls g_calls;
FakeEngine* g_target = nullptr;
bool g_connectWrites = true;
// A Disconnect that COMPACTS: the engine is free to drop the CI entry and shorten the dense array,
// which leaves the pool slot we hold naming an index past the live count.
bool g_disconnectCompacts = false;

uint16_t DenseOfPool(FakeEngine& e, uint16_t poolIndex) {
    return static_cast<uint16_t>(e.pool[poolIndex] & 0xFFFFu);
}

void __fastcall FakeConnect(void* cs, uint16_t poolIndex, uint32_t userId) {
    ++g_calls.connects;
    g_calls.cs = cs;
    g_calls.poolIndex = poolIndex;
    g_calls.userId = userId;
    if (!g_connectWrites || g_target == nullptr) {
        return;
    }
    // What the engine's Connect does to the fields this code reads back.
    const uint16_t dense = DenseOfPool(*g_target, poolIndex);
    g_target->Poke(g_target->ci, g_target->CiOff(dense, voip_facts::kCiUserIdOff), userId);
}

void __fastcall FakeDisconnect(void* cs, uint16_t poolIndex) {
    ++g_calls.disconnects;
    g_calls.cs = cs;
    g_calls.poolIndex = poolIndex;
    if (g_target == nullptr) {
        return;
    }
    const uint16_t dense = DenseOfPool(*g_target, poolIndex);
    g_target->Poke(g_target->ci, g_target->CiOff(dense, voip_facts::kCiUserIdOff), g_sentinel);
    if (g_disconnectCompacts) {
        g_target->Poke(g_target->cs, voip_facts::kCsCountOff, dense);
    }
}

void Arm(FakeEngine& e) {
    g_calls = BindCalls{};
    g_target = &e;
    g_connectWrites = true;
    g_disconnectCompacts = false;
    SetBindCallsForTest(&FakeConnect, &FakeDisconnect, &g_sentinel);
}

void Disarm() {
    g_target = nullptr;
    SetBindCallsForTest(nullptr, nullptr, nullptr);
}

// A receiver system with `live` instances, all free, plus whatever the caller binds afterwards.
void BuildFree(FakeEngine& e, uint16_t live) {
    e.Build(live);
    for (uint16_t i = 0; i < live; ++i) {
        // pool slot i+2 for each, so a pool index is never accidentally equal to its dense index
        // -- a walk that confused the two would pass on 0 and 1 and fail here.
        e.denseMap[i] = static_cast<uint16_t>(i + 2);
        e.pool[i + 2] = static_cast<uint32_t>(i) | (static_cast<uint32_t>(0x50 + i) << 16);
        e.Poke(e.ci, e.CiOff(i, voip_facts::kCiUserIdOff), g_sentinel);
        e.Poke(e.ci, e.CiOff(i, voip_facts::kCiDecoderStateOff), 0u);
    }
}

}  // namespace

TEST_CASE("a free instance is bound through the engine's own Connect") {
    FakeEngine e;
    BuildFree(e, 3);
    e.InstallLookup();
    Arm(e);

    const uint32_t userId = MakeUserId(6, 0x0003);
    uint16_t poolIndex = 0xFFFF;
    CHECK(BindTalker(e.Site(), userId, &poolIndex) == Bind::Bound);
    CHECK(g_calls.connects == 1);
    CHECK(g_calls.cs == e.cs.data());
    CHECK(g_calls.userId == userId);
    CHECK(g_calls.poolIndex == 2u);   // dense 0 maps to pool slot 2
    CHECK(poolIndex == 2u);

    SUBCASE("and the handle walk now finds it, which is the point of binding") {
        uint32_t handle = 0;
        const uint8_t* ci = nullptr;
        Resolve why = Resolve::Ok;
        REQUIRE(HandleOf(e.Site(), userId, &handle, &ci, &why));
        CHECK((handle & 0xFFFFu) == 2u);
        CHECK((handle >> 16) == 0x50u);
    }

    SUBCASE("giving it back restores the engine's own no-user id") {
        CHECK(UnbindTalker(e.Site(), poolIndex, userId) == Release::Released);
        CHECK(g_calls.disconnects == 1);
        CHECK(g_calls.poolIndex == 2u);
        uint32_t handle = 0;
        const uint8_t* ci = nullptr;
        Resolve why = Resolve::Ok;
        CHECK_FALSE(HandleOf(e.Site(), userId, &handle, &ci, &why));
        CHECK(why == Resolve::NoSuchTalker);
    }

    Disarm();
}

TEST_CASE("a user the session already bound is never bound a second time") {
    FakeEngine e;
    BuildFree(e, 3);
    e.InstallLookup();
    Arm(e);

    const uint32_t userId = MakeUserId(9, 0x0004);
    // The session owns dense 2; dense 0 and 1 are free and come FIRST in the scan.
    e.Poke(e.ci, e.CiOff(2, voip_facts::kCiUserIdOff), userId);

    uint16_t poolIndex = 0xFFFF;
    CHECK(BindTalker(e.Site(), userId, &poolIndex) == Bind::AlreadyBound);
    // ★ The whole point: a one-pass scan would have taken the free slot at dense 0 and produced a
    // SECOND instance carrying this id, with the dispatcher routing by array order thereafter.
    CHECK(g_calls.connects == 0);
    CHECK(poolIndex == 4u);   // dense 2 maps to pool slot 4

    Disarm();
}

TEST_CASE("no free instance is a refusal that touches nothing") {
    FakeEngine e;
    BuildFree(e, 2);
    e.InstallLookup();
    Arm(e);
    e.Poke(e.ci, e.CiOff(0, voip_facts::kCiUserIdOff), MakeUserId(1, 1));
    e.Poke(e.ci, e.CiOff(1, voip_facts::kCiUserIdOff), MakeUserId(2, 1));

    uint16_t poolIndex = 0xFFFF;
    CHECK(BindTalker(e.Site(), MakeUserId(7, 0x0002), &poolIndex) == Bind::NoFreeInstance);
    CHECK(g_calls.connects == 0);
    CHECK(g_calls.disconnects == 0);

    Disarm();
}

TEST_CASE("an unreadable no-user id refuses rather than assuming one") {
    FakeEngine e;
    BuildFree(e, 2);
    e.InstallLookup();
    g_calls = BindCalls{};
    g_target = &e;
    // Connect and Disconnect are supplied; the SENTINEL is not.
    SetBindCallsForTest(&FakeConnect, &FakeDisconnect, nullptr);

    uint16_t poolIndex = 0xFFFF;
    // With no game there is no mapped image to read the real global from, so this must refuse.
    // Guessing 0xFFFFFFFF would make a TAKEN slot look free, and binding over a live talker is the
    // one mistake here that damages the session rather than the replay.
    CHECK(BindTalker(e.Site(), MakeUserId(3, 1), &poolIndex) == Bind::NoSentinel);
    CHECK(g_calls.connects == 0);

    Disarm();
}

TEST_CASE("ownership advances on effect, never on the call having been made") {
    FakeEngine e;
    BuildFree(e, 2);
    e.InstallLookup();
    Arm(e);
    g_connectWrites = false;   // a Connect that returns without binding anything

    uint16_t poolIndex = 0xFFFF;
    // ★ Connect returns void. Believing it rather than reading back the field it writes would
    // record ownership of a slot we never got -- and teardown would then Disconnect somebody else's.
    CHECK(BindTalker(e.Site(), MakeUserId(4, 2), &poolIndex) == Bind::Unreadable);
    CHECK(g_calls.connects == 1);   // it WAS called; the effect is what failed

    Disarm();
}

TEST_CASE("teardown releases what we bound and leaves what the session owned") {
    FakeEngine e;
    BuildFree(e, 3);
    e.InstallLookup();
    Arm(e);

    const uint32_t ours = MakeUserId(5, 0x0007);
    const uint32_t theirs = MakeUserId(8, 0x0009);
    e.Poke(e.ci, e.CiOff(2, voip_facts::kCiUserIdOff), theirs);   // the session's

    uint16_t oursPool = 0xFFFF;
    uint16_t theirsPool = 0xFFFF;
    REQUIRE(BindTalker(e.Site(), ours, &oursPool) == Bind::Bound);
    REQUIRE(BindTalker(e.Site(), theirs, &theirsPool) == Bind::AlreadyBound);
    REQUIRE(g_calls.connects == 1);

    // Only the slot BindTalker said Bound for is ours to give back.
    CHECK(UnbindTalker(e.Site(), oursPool, ours) == Release::Released);
    CHECK(g_calls.disconnects == 1);

    // ★ AND THE INDEX IS RE-ESTABLISHED, NOT TRUSTED (AUD-F3L F3L-1). `theirsPool` names a slot the
    // SESSION owns, exactly as a cached index does after the engine hands that slot to somebody
    // else. The receiver is unchanged and every pointer term agrees; only the id at the slot can
    // tell, and it must refuse. Delete the pre-check and this disconnects a live talker.
    CHECK(UnbindTalker(e.Site(), theirsPool, ours) == Release::PoolMoved);
    CHECK(g_calls.disconnects == 1);

    // The session's is untouched and still resolves.
    uint32_t handle = 0;
    const uint8_t* ci = nullptr;
    Resolve why = Resolve::Ok;
    CHECK(HandleOf(e.Site(), theirs, &handle, &ci, &why));

    Disarm();
}

// ★ A SLOT THAT CAME BACK IS NOT A LEAK, EVEN WHERE THE READ-BACK CANNOT SEE IT (AUD-F3M F3M-3).
// Disconnect may compact the dense array, and the pool entry we hold then names an index past the
// live count: the CI entry our talker occupied is GONE, which is the release itself and not a
// failure to observe it. Filed as Unreadable it lands under releaseFailed -- the one counter that
// is supposed to mean "a slot we could not give back" -- and a correct release then reads as the
// leak this whole path exists to prevent. Collapse the two PoolRead failures back into one and
// this reds.
TEST_CASE("a release the engine compacted out of the dense array is Released and not a leak") {
    FakeEngine e;
    BuildFree(e, 3);
    // Somebody else on dense 0, so ours lands on dense 1 and the compacted count stays non-zero.
    // A count of zero is a different refusal and would leave this arm untested.
    const uint32_t theirs = MakeUserId(8, 0x0009);
    e.Poke(e.ci, e.CiOff(0, voip_facts::kCiUserIdOff), theirs);
    e.InstallLookup();
    Arm(e);

    const uint32_t ours = MakeUserId(5, 0x0007);
    uint16_t oursPool = 0;
    REQUIRE(BindTalker(e.Site(), ours, &oursPool) == Bind::Bound);
    REQUIRE(DenseOfPool(e, oursPool) == 1);

    g_disconnectCompacts = true;
    CHECK(UnbindTalker(e.Site(), oursPool, ours) == Release::Released);
    CHECK(g_calls.disconnects == 1);

    // The pre-check side is unchanged: the SAME unreadable dense index refuses before the write,
    // because there it is no proof the slot is ours. The two sides are only allowed to differ.
    CHECK(UnbindTalker(e.Site(), oursPool, ours) == Release::Unreadable);
    CHECK(g_calls.disconnects == 1);

    Disarm();
}
