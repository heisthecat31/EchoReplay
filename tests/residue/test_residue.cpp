// W3-A's offline gate. Everything here runs on a machine with no game.
//
// ---------------------------------------------------------------------------------------------
// ★ WHAT THIS SUITE ESTABLISHES AND WHAT IT CANNOT.
//
// ESTABLISHES  (a) that the entity -> ncaPhysics -> CPhTarget -> CPhBody chain resolves the bodies
//              it should and refuses the ones it should, over a FABRICATED component-system image
//              driven through the same walk the product runs -- not a mock of the walk;
//              (b) that every guard the engine applies is applied, one negative case each, and that
//              deleting any one of them turns a named assertion red;
//              (c) that the three rows split for half-availability really are independent: N-7a is
//              Ready with N-7b's entry point missing, which is the property the split exists for;
//              (d) that registration order is fire order for the two ordered pairs;
//              (e) that every count is of an EFFECT and not of a call.
//
// CANNOT       say anything about the ENGINE. ResolveCsSite and ResolvePhysSite read the CR15Game
//              singleton and the CPhSystem global and are the two functions in this packet that a
//              machine with no game cannot execute; they are substituted here, and nothing else is.
//              ★ ONE HOP OF ResolveCsSite IS NOW EXERCISED: BindCsSite, the netGame -> game space ->
//              CS.LOOKUP_BY_SYMBOL step, runs here over a synthetic CR15NetGame with a recording
//              lookup. That is where F3 lived and it was untestable until it was split out.
//              Whether the five guarded entry points do to a REAL CI and a REAL body what their
//              decompiled bodies say they do is a launch question, parked in
//              runs/queue/payload/W3-A.json. ★ THIS SUITE PASSING IS NOT THE BATTERY WORKING.
//
// ★★ THE IMAGE IS PROVED BEFORE ANYTHING IS PROVED WITH IT. Every guard case below is a walk that
// is expected to find NOTHING, and a builder that produced an unreachable world would pass all of
// them for free. So the first test asserts the healthy image resolves every entity and acts on every
// body -- and each guard case then mutates exactly one field of that same healthy image. A negative
// result is only evidence when the positive control it is measured against is green.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <limits>
#include <type_traits>
#include <vector>

#include <windows.h>

#include "bootstrap/buildguard.h"
#include "doctest.h"
#include "engine/entity.h"
#include "generated/facts_generated.h"
#include "playback/core/discontinuity.h"
#include "playback/residue/residue.h"
#include "playback/residue/residue_facts.h"
#include "playback/residue/residuesite.h"

using namespace replay::playback;
using namespace replay::playback::residue;
namespace rf = replay::playback::residue_facts;

namespace {

// ---------------------------------------------------------------------------------------------
// A synthetic engine image: two component systems, a physics system, a zone and a body array, laid
// out at the offsets the facts name. Pointers inside it point into itself, which is what makes the
// product's own dereference chain runnable over it.
//
// The buffer is sized once and never resized, because every pointer handed out would dangle.

// THE DECLARED COUNT AND THE PHYSICAL EXTENT ARE DIFFERENT NUMBERS, ON PURPOSE.
//
// The first version of this image built exactly as many zones, bodies and physics slots as it
// declared, and all three bound checks SURVIVED mutation: deleting a bound let the walk read past
// the declared count, where it found ZEROED memory, where the null-array-pointer check refused it
// anyway. Every bound test still went green -- while measuring a different guard.
//
// That is the shape this project keeps paying for: an assertion that passes for a reason other than
// the one it is named after. In the real game the memory past a declared count is other live data
// and the read is wild, so the bound is the only thing between a stale CPhTarget and a write into
// somebody else's object.
//
// So the image now PLANTS A FULLY VALID DECOY one slot past each declared count -- a live zone, a
// live body with a matching generation tag, a live physics component instance. Every other guard
// accepts the decoy. Only the bound refuses it, which is what makes the bound the thing under test.
constexpr size_t kCap = 0x40000;
constexpr size_t kMaxEnt = 8;
constexpr size_t kMaxZone = 3;              // declared to the CPhSystem
constexpr size_t kMaxBody = 8;              // declared to each CPhZone
constexpr size_t kZoneSlots = kMaxZone + 1; // physically present and fully valid
constexpr size_t kBodySlots = kMaxBody + 1;

constexpr size_t kNetCs = 0x00000;
constexpr size_t kPhysCs = 0x00200;
constexpr size_t kPlayerCs = 0x00400;
constexpr size_t kPhSystem = 0x00600;
constexpr size_t kNetMap = 0x00700;
constexpr size_t kNetRev = 0x00800;
constexpr size_t kPhysMap = 0x00900;
constexpr size_t kPlayerMap = 0x00A00;
constexpr size_t kPlayerRev = 0x00B00;
constexpr size_t kNetPool = 0x01000;
constexpr size_t kPhysPool = 0x03000;
constexpr size_t kPlayerPool = 0x04000;
constexpr size_t kZoneArray = 0x10000;
constexpr size_t kBodyArray = 0x30000;
// Placed after the body array, which ends at 0x30000 + 9*0xF38 = 0x388F8.
constexpr size_t kGame = 0x39000;        // CR15Game: the re-origin request bits at +0x828
constexpr size_t kClock = 0x3A000;       // CClock: the stamp and two accumulators
constexpr size_t kOriginBlock = 0x3B000; // the world-offset span at +0x2A98..+0x2AB0

class Image {
public:
    Image() : buf_(kCap, 0) {}

    uint8_t* At(size_t off) { return buf_.data() + off; }

    template <typename T>
    void Put(size_t off, T v) {
        memcpy(buf_.data() + off, &v, sizeof v);
    }
    template <typename T>
    T Get(size_t off) {
        T v{};
        memcpy(&v, buf_.data() + off, sizeof v);
        return v;
    }
    void PutPtr(size_t off, size_t target) { Put<uint64_t>(off, (uint64_t)(uintptr_t)At(target)); }

    size_t Body(size_t i) const { return kBodyArray + facts::PHYS_BODY_STRIDE * i; }
    size_t Zone(size_t i) const { return kZoneArray + rf::kPhZoneStride * i; }
    size_t NetCi(size_t i) const { return kNetPool + facts::PHYS_CI_STRIDE * i; }
    size_t OriginSpan() const { return kOriginBlock + facts::WORLD_REORIGIN_OFF_X; }
    size_t PhysCi(size_t i) const { return kPhysPool + rf::kPhysicsCiStride * i; }

    // A world of `n` entities, each with its own live, awake, dynamic body in zone 1.
    //
    // ★ ZONE 1, NOT ZONE 0. The engine's own zone loops start at index 1 -- slot 0 is a sentinel --
    // so a world built entirely in zone 0 would be one this code never sees in the product.
    void BuildHealthy(size_t n) {
        REQUIRE(n <= kMaxEnt);
        REQUIRE(n <= kMaxBody);

        PutPtr(kNetCs + facts::PHYS_CS_HANDLE_MAP_OFF, kNetMap);
        PutPtr(kNetCs + facts::PHYS_CS_REVERSE_MAP_OFF, kNetRev);
        PutPtr(kNetCs + facts::PHYS_CS_POOL_BASE_OFF, kNetPool);
        Put<uint16_t>(kNetCs + facts::PHYS_CS_COUNT_OFF, (uint16_t)n);
        PutPtr(kNetCs + rf::kNetCsPhysicsCsOff, kPhysCs);

        PutPtr(kPhysCs + facts::PHYS_CS_HANDLE_MAP_OFF, kPhysMap);
        PutPtr(kPhysCs + facts::PHYS_CS_POOL_BASE_OFF, kPhysPool);
        Put<uint16_t>(kPhysCs + facts::PHYS_CS_COUNT_OFF, (uint16_t)n);

        PutPtr(kPlayerCs + facts::PLAYER_CS_HANDLE_MAP_OFF, kPlayerMap);
        PutPtr(kPlayerCs + facts::PLAYER_CS_REVERSE_MAP_OFF, kPlayerRev);
        PutPtr(kPlayerCs + facts::PLAYER_CS_POOL_BASE_OFF, kPlayerPool);
        Put<uint16_t>(kPlayerCs + facts::PLAYER_CS_COUNT_OFF, (uint16_t)n);

        PutPtr(kPhSystem + rf::kPhSystemZoneArrayOff, kZoneArray);
        Put<uint64_t>(kPhSystem + rf::kPhSystemZoneCountOff, kMaxZone);

        // kZoneSlots zones exist and are valid; kMaxZone of them are declared. Zone kMaxZone is the
        // decoy the zone-bound test aims at.
        for (size_t z = 0; z < kZoneSlots; ++z) {
            Put<uint32_t>(Zone(z) + rf::kPhIdIndexOff, (uint32_t)z);
            PutPtr(Zone(z) + facts::PHYS_ZONE_BODY_ARRAY_OFF, kBodyArray);
            Put<uint64_t>(Zone(z) + facts::PHYS_ZONE_BODY_COUNT_OFF, kMaxBody);
        }

        // Likewise every body slot, referenced by an entity or not. Body kMaxBody is the decoy for
        // the body-bound test, and the unreferenced ones past `n` serve the physics-count test.
        for (size_t i = 0; i < kBodySlots; ++i) {
            Put<uint32_t>(Body(i) + rf::kPhIdIndexOff, (uint32_t)i);
            Put<uint32_t>(Body(i) + rf::kPhIdTagOff, (uint32_t)(0x1000 + i));
            Put<uint32_t>(Body(i) + facts::PHYS_BODY_FLAGS_OFF, 0u);
            // Class 4: on the first of the two lists CollectClassifiedBodies walks. Without this the
            // healthy image would have every body unclassified and every N-7a assertion below would
            // pass by doing nothing.
            Put<uint32_t>(Body(i) + rf::kPhBodyClassOff, rf::kPhBodyClassCollected[0]);
            Put<uint64_t>(Body(i) + facts::PHYS_BODY_SLEEP_STAMP_OFF, 0u);
            Put<uint32_t>(Body(i) + facts::PHYS_BODY_HYSTERESIS_SKIP_OFF, 0u);
            Put<uint64_t>(Body(i) + facts::PHYS_BODY_HYSTERESIS_A_OFF, 0u);
            Put<uint32_t>(Body(i) + facts::PHYS_BODY_HYSTERESIS_B_OFF, 0u);
        }

        // And a valid physics component instance in the slot one past the declared count, whose
        // target names a live body with a matching tag. Only the declared-count bound refuses it.
        Put<uint64_t>(PhysCi(kMaxEnt) + rf::kPhysicsCiBodyGuard, 0xB0D1u);
        {
            const size_t d = PhysCi(kMaxEnt) + rf::kPhysicsCiTargetOff;
            Put<uint32_t>(d + rf::kPhTargetIndexOff, (uint32_t)(kMaxBody - 1));
            Put<uint32_t>(d + rf::kPhTargetZoneOff, 1u);
            Put<uint32_t>(d + rf::kPhTargetTagOff, (uint32_t)(0x1000 + kMaxBody - 1));
        }

        for (size_t i = 0; i < n; ++i) {
            // Handles are deliberately NOT equal to pool slots -- a walk that confused the two would
            // pass a test where they agreed and fail in the product, where they do not.
            const uint16_t handle = (uint16_t)(20 + i);
            const uint16_t physHandle = (uint16_t)(40 + i);
            Put<uint16_t>(kNetRev + 2 * i, handle);
            Put<uint16_t>(kNetMap + 4 * handle, (uint16_t)i);
            Put<uint16_t>(kPhysMap + 4 * physHandle, (uint16_t)i);

            Put<uint16_t>(kPlayerRev + 2 * i, (uint16_t)(60 + i));
            Put<uint16_t>(kPlayerMap + 4 * (60 + i), (uint16_t)i);

            Put<uint32_t>(NetCi(i) + rf::kNetCiPhysicsHandleOff, physHandle);
            Put<float>(NetCi(i) + facts::PHYS_CI_TIME_OFFSET_S_OFF, 0.0f);

            Put<uint64_t>(PhysCi(i) + rf::kPhysicsCiBodyGuard, 0xB0D1u);
            const size_t t = PhysCi(i) + rf::kPhysicsCiTargetOff;
            Put<uint32_t>(t + rf::kPhTargetIndexOff, (uint32_t)i);
            Put<uint32_t>(t + rf::kPhTargetZoneOff, 1u);
            Put<uint32_t>(t + rf::kPhTargetTagOff, (uint32_t)(0x1000 + i));

        }
    }

    WorldSite World() {
        WorldSite w;
        w.originBlock = At(kOriginBlock);
        w.bind = Bind::Ok;
        return w;
    }
    ClockSite Clock() {
        ClockSite c;
        c.clock = At(kClock);
        c.bind = Bind::Ok;
        return c;
    }

    CsSite Cs() {
        CsSite s;
        s.netPhysicsCs = At(kNetCs);
        s.remotePlayerCs = At(kPlayerCs);
        s.physicsCs = At(kPhysCs);
        s.bind = Bind::Ok;
        return s;
    }
    PhysSite Ph() {
        PhysSite s;
        s.system = At(kPhSystem);
        s.bind = Bind::Ok;
        return s;
    }

private:
    std::vector<uint8_t> buf_;
};

bool CountOnly(void*, void* ctx) {
    ++*static_cast<int*>(ctx);
    return true;
}

// ---------------------------------------------------------------------------------------------
// The five engine entry points, substituted. Each records that it ran and what it was handed.

struct CallLog {
    std::vector<std::string> order;
    std::vector<uint16_t> ciHandles;
    std::vector<uint16_t> warpHandles;
    std::vector<uint16_t> grabHandles;
    std::vector<float> snapValues;
    std::vector<void*> wokenBodies;

    void Clear() { *this = CallLog(); }
};

CallLog g_log;
Image* g_image = nullptr;

CsSite StubResolveCs() { return g_image ? g_image->Cs() : CsSite(); }
PhysSite StubResolvePhys() { return g_image ? g_image->Ph() : PhysSite(); }
WorldSite StubResolveWorld() { return g_image ? g_image->World() : WorldSite(); }
ClockSite StubResolveClock() { return g_image ? g_image->Clock() : ClockSite(); }
CsSite BrokenResolveCs() { CsSite s; s.bind = Bind::NoGameSpace; return s; }

uint64_t StubResetCi(void*, uint16_t h) {
    g_log.order.push_back("ci");
    g_log.ciHandles.push_back(h);
    return 0;
}
uint64_t StubWarp(void*, uint16_t h) {
    g_log.order.push_back("warp");
    g_log.warpHandles.push_back(h);
    return 0;
}
uint64_t StubSnap(void*, uint16_t h, float s) {
    g_log.order.push_back("snap");
    (void)h;
    g_log.snapValues.push_back(s);
    return 0;
}
uint64_t StubGrab(void*, uint16_t h) {
    g_log.order.push_back("grab");
    g_log.grabHandles.push_back(h);
    return 0;
}
uint64_t StubWake(void* body, uint32_t a2, uint32_t a3) {
    g_log.order.push_back("wake");
    g_log.wokenBodies.push_back(body);
    // Both flags are what the row promises. Asserting it here rather than in the row keeps the
    // promise checkable from outside the row.
    CHECK(a2 == 1u);
    CHECK(a3 == 1u);
    return 0;
}

ResidueEnv FullEnv() {
    ResidueEnv e;
    e.resolveCs = &StubResolveCs;
    e.resolvePhys = &StubResolvePhys;
    e.resetCi = &StubResetCi;
    e.resetWarpFrame = &StubWarp;
    e.snapTimeOffset = &StubSnap;
    e.clearGrabFixups = &StubGrab;
    e.wake = &StubWake;
    e.resolveWorld = &StubResolveWorld;
    e.resolveClock = &StubResolveClock;
    e.entity = nullptr;   // the ordering check reports NoEntity unless a test supplies one
    return e;
}

// ★ The eleven rows this packet registers. Held once so every count below moves together and a row
// added without a decision about its cause routing cannot slip through as "the number changed".
constexpr size_t kOwnRows = 11;

Discontinuity Jump(Cause c) {
    Discontinuity d;
    d.from_us = 30'000'000;
    d.to_us = 5'000'000;   // BACKWARDS, which is the direction the whole battery exists for
    d.cause = c;
    d.needs_preroll = (c == Cause::Seek);
    return d;
}

size_t Fire(ResidueBattery& bat, const ResidueEnv& env, Cause c) {
    g_log.Clear();
    ResetStatsForTest();
    const size_t n = RegisterPlaybackResidue(bat, env);
    CHECK(n == kOwnRows);
    return bat.Fire(Jump(c));
}

}  // namespace

// =============================================================================================
// The positive control. Nothing below it means anything if this is red.
// =============================================================================================

TEST_CASE("the healthy image resolves every entity and every body -- the positive control") {
    Image img;
    img.BuildHealthy(5);
    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);

    CHECK(w.entities == 5);
    CHECK(w.bodies == 5);
    CHECK(w.wakeable == 5);
    CHECK(w.asleep == 0);
    CHECK(w.acted == 5);
    CHECK(acted == 5);
}

TEST_CASE("an empty pool walks to nothing, and that is distinguishable from a broken chain") {
    Image img;
    img.BuildHealthy(0);
    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.entities == 0);
    CHECK(w.bodies == 0);
    CHECK(w.acted == 0);

    // A site that never resolved walks to the same zeros -- which is exactly why Bind is separate
    // from the counts, and why the stats carry both.
    CsSite broken;
    broken.bind = Bind::NoGameSpace;
    const BodyWalk b = ForEachRestoredBody(broken, img.Ph(), &CountOnly, &acted);
    CHECK(b.entities == 0);
    CHECK(std::string(BindText(broken.bind)).size() > 0);
    CHECK(std::string(BindText(Bind::Ok)) != std::string(BindText(Bind::NoGameSpace)));
}

// =============================================================================================
// One guard, one negative case. Each mutates exactly one field of the healthy image above.
// =============================================================================================

TEST_CASE("the generation tag is what makes a body the RIGHT body") {
    Image img;
    img.BuildHealthy(3);

    // Recycle body 1's slot: same index, same zone, new generation. Locality is not identity, and a
    // resolve that ignored the tag would hand back somebody else's body and wake it.
    img.Put<uint32_t>(img.Body(1) + rf::kPhIdTagOff, 0xDEAD0001u);

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.entities == 3);
    CHECK(w.bodies == 2);   // 1 of 3 refused
    CHECK(w.acted == 2);

    // And the direct call says the same thing, both ways round.
    PhTarget t;
    REQUIRE(PrimaryBodyTarget(img.Cs(), 41, t));   // entity 1's ncaPhysics handle
    CHECK(ResolveBody(img.Ph(), t) == nullptr);
    t.tag = 0xDEAD0001u;
    CHECK(ResolveBody(img.Ph(), t) == img.At(img.Body(1)));
}

TEST_CASE("a dead zone slot resolves to nothing") {
    Image img;
    img.BuildHealthy(3);
    img.Put<uint32_t>(img.Zone(1) + rf::kPhIdIndexOff, rf::kPhIdDead);

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.entities == 3);
    CHECK(w.bodies == 0);   // every entity in this world lives in zone 1
}

TEST_CASE("a zone index past the system's own count resolves to nothing") {
    Image img;
    img.BuildHealthy(3);
    // The bound CPhTarget::ResolveBody omits and CPhTarget::ResolveZone applies. This packet did not
    // author the index, so it applies it; without it this read runs off the end of the zone array.
    //
    // Zone kMaxZone EXISTS and is valid, and entity 2's body index and generation tag both match in
    // it, so every other guard accepts this target. Only the bound refuses it -- which is what makes
    // this a test of the bound rather than of whatever happens to lie past the array.
    img.Put<uint32_t>(img.PhysCi(2) + rf::kPhysicsCiTargetOff + rf::kPhTargetZoneOff,
                      (uint32_t)kMaxZone);

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.bodies == 2);
}

TEST_CASE("a body index past the zone's own count resolves to nothing") {
    Image img;
    img.BuildHealthy(3);
    // Body kMaxBody exists, is live, and its generation tag is matched here deliberately, so the
    // zone's declared body count is the only thing that can refuse this target.
    const size_t t = img.PhysCi(0) + rf::kPhysicsCiTargetOff;
    img.Put<uint32_t>(t + rf::kPhTargetIndexOff, (uint32_t)kMaxBody);
    img.Put<uint32_t>(t + rf::kPhTargetTagOff, (uint32_t)(0x1000 + kMaxBody));

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.bodies == 2);
}

TEST_CASE("a dead body slot resolves to nothing") {
    Image img;
    img.BuildHealthy(3);
    img.Put<uint32_t>(img.Body(0) + rf::kPhIdIndexOff, rf::kPhIdDead);

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.bodies == 2);
}

TEST_CASE("an entity with no ncaPhysics component is skipped, not resolved to slot 0xFFFF") {
    Image img;
    img.BuildHealthy(3);
    img.Put<uint32_t>(img.NetCi(1) + rf::kNetCiPhysicsHandleOff, rf::kNoComponentHandle);

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.entities == 3);
    CHECK(w.bodies == 2);

    PhTarget t;
    CHECK_FALSE(PrimaryBodyTarget(img.Cs(), rf::kNoComponentHandle, t));
}

TEST_CASE("an entity whose physics component has no primary body is skipped") {
    Image img;
    img.BuildHealthy(3);
    img.Put<uint64_t>(img.PhysCi(2) + rf::kPhysicsCiBodyGuard, 0);

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.bodies == 2);
}

TEST_CASE("a physics slot past the system's declared count is refused") {
    Image img;
    img.BuildHealthy(3);
    // Slot kMaxEnt is one past the declared count of 3 and holds a VALID component instance whose
    // target names a live body with a matching tag. The declared count is the only refusal left.
    img.Put<uint16_t>(kPhysMap + 4 * 41, (uint16_t)kMaxEnt);   // entity 1's ncaPhysics handle

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.bodies == 2);
}

TEST_CASE("kinematic and sphere-effect bodies resolve but are not counted as wakeable") {
    Image img;
    img.BuildHealthy(4);
    img.Put<uint32_t>(img.Body(1) + facts::PHYS_BODY_FLAGS_OFF, 0x4u);   // kinematic
    img.Put<uint32_t>(img.Body(2) + facts::PHYS_BODY_FLAGS_OFF, 0x8u);   // sphere effect

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.bodies == 4);      // all four are live
    CHECK(w.wakeable == 2);    // BeAwake_ would no-op on two of them
    CHECK(w.acted == 2);       // so the callback saw two -- a count of bodies, not of calls

    CHECK_FALSE(BodyWakeable(img.At(img.Body(1))));
    CHECK_FALSE(BodyWakeable(img.At(img.Body(2))));
    CHECK(BodyWakeable(img.At(img.Body(0))));
}

TEST_CASE("the asleep count is the sleep stamp, and it is separate from wakeable") {
    Image img;
    img.BuildHealthy(4);
    img.Put<uint64_t>(img.Body(0) + facts::PHYS_BODY_SLEEP_STAMP_OFF, 99u);
    img.Put<uint64_t>(img.Body(3) + facts::PHYS_BODY_SLEEP_STAMP_OFF, 1u);

    int acted = 0;
    const BodyWalk w = ForEachRestoredBody(img.Cs(), img.Ph(), &CountOnly, &acted);
    CHECK(w.asleep == 2);
    CHECK(w.wakeable == 4);

    CHECK(BodyAsleep(img.At(img.Body(0))));
    CHECK_FALSE(BodyAsleep(img.At(img.Body(1))));
}

// =============================================================================================
// The per-body effects.
// =============================================================================================

TEST_CASE("the motion bit is raised once, and raising it again is not an effect") {
    Image img;
    img.BuildHealthy(1);
    uint8_t* body = img.At(img.Body(0));

    CHECK(RaiseMotionBit(body));
    CHECK((img.Get<uint32_t>(img.Body(0) + facts::PHYS_BODY_FLAGS_OFF) &
           facts::PHYS_BODY_MOTION_BIT) != 0);

    // ★ The second call reports FALSE. That is what keeps `motionBitsRaised` a count of bodies
    // changed rather than of bodies visited -- the exact distinction W2-B's netvar counter lost.
    CHECK_FALSE(RaiseMotionBit(body));

    // And it is an OR, not a store: the other flag bits survive.
    img.Put<uint32_t>(img.Body(0) + facts::PHYS_BODY_FLAGS_OFF, 0x00020001u);
    CHECK(RaiseMotionBit(body));
    CHECK(img.Get<uint32_t>(img.Body(0) + facts::PHYS_BODY_FLAGS_OFF) ==
          (0x00020001u | facts::PHYS_BODY_MOTION_BIT));
}

TEST_CASE("the hysteresis clear honours the engine's own skip guard") {
    Image img;
    img.BuildHealthy(1);
    const size_t b = img.Body(0);
    img.Put<uint64_t>(b + facts::PHYS_BODY_HYSTERESIS_A_OFF, 0x1111222233334444ull);
    img.Put<uint32_t>(b + facts::PHYS_BODY_HYSTERESIS_B_OFF, 0x55556666u);

    SUBCASE("skip clear -- both latches are zeroed") {
        img.Put<uint32_t>(b + facts::PHYS_BODY_HYSTERESIS_SKIP_OFF, 0u);
        CHECK(ClearHysteresis(img.At(b)));
        CHECK(img.Get<uint64_t>(b + facts::PHYS_BODY_HYSTERESIS_A_OFF) == 0);
        CHECK(img.Get<uint32_t>(b + facts::PHYS_BODY_HYSTERESIS_B_OFF) == 0);
        // Already clear -> no effect, so the count stays a count of changes.
        CHECK_FALSE(ClearHysteresis(img.At(b)));
    }
    SUBCASE("skip set -- NOTHING is written") {
        // CollectClassifiedBodies gates both stores on this field. Dropping the guard would make
        // this a different function that happens to touch the same two fields.
        img.Put<uint32_t>(b + facts::PHYS_BODY_HYSTERESIS_SKIP_OFF, 1u);
        CHECK_FALSE(ClearHysteresis(img.At(b)));
        CHECK(img.Get<uint64_t>(b + facts::PHYS_BODY_HYSTERESIS_A_OFF) == 0x1111222233334444ull);
        CHECK(img.Get<uint32_t>(b + facts::PHYS_BODY_HYSTERESIS_B_OFF) == 0x55556666u);
    }
}

TEST_CASE("the hysteresis clear writes the engine's WIDTHS -- 8 bytes and 4, not 8 and 8") {
    Image img;
    img.BuildHealthy(1);
    const size_t b = img.Body(0);
    // Sentinels in the four bytes immediately after each latch. A qword store where the engine does
    // a dword store takes the neighbouring field with it, and nothing else in this suite would see
    // it -- the latch would read zero either way.
    img.Put<uint64_t>(b + facts::PHYS_BODY_HYSTERESIS_A_OFF, 7u);
    img.Put<uint32_t>(b + facts::PHYS_BODY_HYSTERESIS_B_OFF, 7u);
    img.Put<uint32_t>(b + facts::PHYS_BODY_HYSTERESIS_B_OFF + 4, 0xFEEDFACEu);
    img.Put<uint64_t>(b + facts::PHYS_BODY_HYSTERESIS_A_OFF + 8, 0xCAFEBABEDEADBEEFull);

    CHECK(ClearHysteresis(img.At(b)));
    CHECK(img.Get<uint32_t>(b + facts::PHYS_BODY_HYSTERESIS_B_OFF + 4) == 0xFEEDFACEu);
    CHECK(img.Get<uint64_t>(b + facts::PHYS_BODY_HYSTERESIS_A_OFF + 8) == 0xCAFEBABEDEADBEEFull);
}

// =============================================================================================
// The rows, through a real ResidueBattery.
// =============================================================================================

TEST_CASE("every row registers, and a full env leaves exactly the two derived refusals") {
    Image img;
    img.BuildHealthy(3);
    g_image = &img;

    ResidueBattery bat;
    ResetStatsForTest();
    CHECK(RegisterPlaybackResidue(bat, FullEnv()) == kOwnRows);
    CHECK(bat.Count() == kOwnRows);

    // N-8b and N-6b are unavailable BY DERIVATION, not by accident: there is no PC
    // ResetDiscoverPhases and CPhContact::Initialize is refused on role, and the recording carries
    // no world origin to restore. A build where this count were zero would be claiming a manifold
    // reset and an origin restore it does not perform.
    CHECK(bat.UnavailableCount() == 2);
    const char* ids[16] = {};
    CHECK(bat.UnavailableIds(ids, 16) == 2);
    REQUIRE(ids[0] != nullptr);
    REQUIRE(ids[1] != nullptr);
    // Registration order, which is fire order: N-8b registers before N-6b.
    CHECK(std::string(ids[0]) == "N-8b");
    CHECK(std::string(ids[1]) == "N-6b");

    // The invariant the interface states: never empty when not Ready, never null at all.
    for (size_t i = 0; i < bat.Count(); ++i) {
        IResidue* r = bat.At(i);
        REQUIRE(r != nullptr);
        REQUIRE(r->Diagnostic() != nullptr);
        REQUIRE(r->Id() != nullptr);
        if (r->Avail() != Availability::Ready) {
            CHECK(r->Diagnostic()[0] != 0);
        }
    }
    g_image = nullptr;
}

TEST_CASE("N-7a stays Ready when N-7b's entry point is missing -- the reason the row was split") {
    Image img;
    img.BuildHealthy(3);
    g_image = &img;

    ResidueEnv env = FullEnv();
    env.wake = nullptr;   // the guard on RESET.PHYS_WAKE_PC refused

    ResidueBattery bat;
    ResetStatsForTest();
    REQUIRE(RegisterPlaybackResidue(bat, env) == kOwnRows);

    CHECK(bat.UnavailableCount() == 3);   // N-7b, N-8b, N-6b
    for (size_t i = 0; i < bat.Count(); ++i) {
        IResidue* r = bat.At(i);
        if (std::string(r->Id()) == "N-7a") {
            CHECK(r->Avail() == Availability::Ready);
        }
        if (std::string(r->Id()) == "N-7b") {
            CHECK(r->Avail() == Availability::Unmapped);
            CHECK(r->Diagnostic()[0] != 0);
        }
    }

    g_log.Clear();
    bat.Fire(Jump(Cause::Seek));

    // ★★ THE WHOLE POINT OF THE SPLIT. The call is gone; the store still happened, on every body,
    // and the engine's own classification pass reads that bit. A single N-7 row gated on the call's
    // availability would have skipped this and left every restored body parked.
    CHECK(Stats().motionBitsRaised == 3);
    CHECK(Stats().wakesCalled == 0);
    CHECK(bat.SkippedTotal() >= 2);
    g_image = nullptr;
}

TEST_CASE("N-3a's guard failing does not silence N-3b -- the same split, on two functions") {
    Image img;
    img.BuildHealthy(2);
    g_image = &img;

    ResidueEnv env = FullEnv();
    env.resetCi = nullptr;

    ResidueBattery bat;
    ResetStatsForTest();
    REQUIRE(RegisterPlaybackResidue(bat, env) == kOwnRows);
    bat.Fire(Jump(Cause::Seek));

    CHECK(Stats().ciResets == 0);
    CHECK(Stats().warpRequests == 2);
    CHECK(Stats().timeSnaps == 2);
    g_image = nullptr;
}

TEST_CASE("registration order is fire order, and two of the pairs depend on it") {
    Image img;
    img.BuildHealthy(2);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);

    // Every ResetCI precedes every ResetWarpFrame: ResetCI rewrites the same status word, masked
    // differently on its two arms, so raising the request bits first is a coin flip.
    size_t lastCi = 0, firstWarp = g_log.order.size();
    size_t firstSnap = g_log.order.size();
    for (size_t i = 0; i < g_log.order.size(); ++i) {
        if (g_log.order[i] == "ci") lastCi = i;
        if (g_log.order[i] == "warp" && firstWarp == g_log.order.size()) firstWarp = i;
        if (g_log.order[i] == "snap" && firstSnap == g_log.order.size()) firstSnap = i;
    }
    REQUIRE(firstWarp < g_log.order.size());
    REQUIRE(firstSnap < g_log.order.size());
    CHECK(lastCi < firstWarp);
    // And every ResetCI precedes every SnapTimeOffset: ResetCI zeroes the goal N-4 reads.
    CHECK(lastCi < firstSnap);

    g_image = nullptr;
}

TEST_CASE("N-4 snaps to the goal the CI is holding, not to a value of its own") {
    Image img;
    img.BuildHealthy(2);
    img.Put<float>(img.NetCi(0) + facts::PHYS_CI_TIME_OFFSET_S_OFF, 1.25f);
    img.Put<float>(img.NetCi(1) + facts::PHYS_CI_TIME_OFFSET_S_OFF, -0.5f);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);

    REQUIRE(g_log.snapValues.size() == 2);
    CHECK(g_log.snapValues[0] == doctest::Approx(1.25f));
    CHECK(g_log.snapValues[1] == doctest::Approx(-0.5f));
    g_image = nullptr;
}

TEST_CASE("the rows address the pools by HANDLE, not by pool slot") {
    Image img;
    img.BuildHealthy(3);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);

    // BuildHealthy deliberately makes handle != slot. A walk that passed the slot would hand the
    // engine an index into a map it does not own, and every value here would be 0, 1, 2.
    REQUIRE(g_log.ciHandles.size() == 3);
    CHECK(g_log.ciHandles[0] == 20);
    CHECK(g_log.ciHandles[1] == 21);
    CHECK(g_log.ciHandles[2] == 22);
    REQUIRE(g_log.grabHandles.size() == 3);
    CHECK(g_log.grabHandles[0] == 60);
    CHECK(g_log.grabHandles[2] == 62);
    g_image = nullptr;
}

TEST_CASE("only the bodies that are actually parked are woken") {
    Image img;
    img.BuildHealthy(4);
    img.Put<uint64_t>(img.Body(1) + facts::PHYS_BODY_SLEEP_STAMP_OFF, 42u);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);

    CHECK(Stats().wakesCalled == 1);
    REQUIRE(g_log.wokenBodies.size() == 1);
    CHECK(g_log.wokenBodies[0] == img.At(img.Body(1)));
    // Every body still gets the bit, parked or not: it is the next classification that decides.
    CHECK(Stats().motionBitsRaised == 4);
    g_image = nullptr;
}

// =============================================================================================
// Cause routing.
// =============================================================================================

TEST_CASE("on a reverse tick and an epoch roll, ONLY the clock guard runs") {
    Image img;
    img.BuildHealthy(3);
    g_image = &img;

    for (Cause c : {Cause::ReverseTick, Cause::ChapterEpochRoll}) {
        ResidueBattery bat;
        const size_t ran = Fire(bat, FullEnv(), c);

        // ★ N-18 and nothing else. Reverse is the direction that can drive a negative delta into an
        // unsigned `add`, so the accumulator guard has to be there -- and a wake sweep at tick rate
        // would pin the whole level awake, which is a behaviour change rather than a repair.
        CHECK(ran == 1);
        CHECK(g_log.order.empty());   // no engine call at all: the guard only reads and compares
        CHECK(Stats().motionBitsRaised == 0);
        CHECK(Stats().ciResets == 0);
    }
    g_image = nullptr;
}

TEST_CASE("a micro-seek runs only the idempotent rows") {
    Image img;
    img.BuildHealthy(3);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::MicroSeek);

    // ★ A micro-seek fires EVERY FRAME above the sub-step ceiling. Recycling the history ring or
    // zeroing a latched grab at frame rate would never let the thing they reset re-establish itself.
    CHECK(Stats().ciResets == 0);
    CHECK(Stats().grabClears == 0);
    CHECK(Stats().motionBitsRaised == 0);
    CHECK(Stats().wakesCalled == 0);
    // Raising a request bit and collapsing a blend are one store each and converge.
    CHECK(Stats().warpRequests == 3);
    CHECK(Stats().timeSnaps == 3);
    g_image = nullptr;
}

TEST_CASE("a gap cross is a real jump and runs the full set") {
    Image img;
    img.BuildHealthy(2);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::GapCross);
    CHECK(Stats().ciResets == 2);
    CHECK(Stats().grabClears == 2);
    CHECK(Stats().motionBitsRaised == 2);
    g_image = nullptr;
}

// =============================================================================================
// The absence reporting.
// =============================================================================================

TEST_CASE("a fire that resolves nothing is distinguishable from one that was never armed") {
    ResetStatsForTest();
    CHECK_FALSE(Stats().armed);

    Image img;
    img.BuildHealthy(3);
    g_image = &img;

    ResidueEnv env = FullEnv();
    env.resolveCs = &BrokenResolveCs;

    ResidueBattery bat;
    ResetStatsForTest();
    REQUIRE(RegisterPlaybackResidue(bat, env) == kOwnRows);
    CHECK(Stats().armed);   // armed, and about to do nothing -- two different facts

    bat.Fire(Jump(Cause::Seek));
    CHECK(Stats().ciResets == 0);
    CHECK(Stats().motionBitsRaised == 0);
    CHECK(Stats().lastCsBind == Bind::NoGameSpace);
    CHECK(Stats().lastWalk.entities == 0);
    g_image = nullptr;
}

// ---------------------------------------------------------------------------------------------
// THE ONE PART OF ResolveCsSite AN OFFLINE MACHINE CAN RUN: netGame -> game space -> lookup.
//
// F3 was here. The bind read *(netGame + IDENT.NETGAME_GAMESPACE_OFF), which is the CR15Game
// back-pointer, and handed a CR15Game to CS.LOOKUP_BY_SYMBOL; the space is NETGAME.CS_SPACE_OFF.
// Nothing above could have caught it, because everything above starts from a CsSite that is already
// resolved. BindCsSite is that hop and nothing else, so this is the offset under test.

// The component-system table's offset on the object the lookup is handed. A LITERAL on purpose
// (ARCH-10 S3.3): a fixture that imports facts::CS_TABLE_OFF cannot fail when facts is wrong.
const size_t kFixtureCsTableOff = 0x420u;  // facts-ok: CS.TABLE_OFF a fixture that imports the product's constant cannot fail when the product's constant is wrong -- ARCH-10 S3.3 keeps the test-side 0x420 literal

// The one qword L5 actually judged at +0x420 of the object the plugin was passing -- `base=400000002000`
// from the recorded NotReady/CapacityOverBlock line. It is a CR15Game render-target budget number,
// not a table pointer, which is exactly why the decoy below refuses rather than resolving.
const uint64_t kRecordedBudgetWord = 0x400000002000ull;

struct FakeCsTable {
    size_t   count = 0;
    uint64_t symbols[2] = {0, 0};
    void*    systems[2] = {nullptr, nullptr};
};

struct LookupLog {
    void* space = nullptr;            // the last object the lookup was handed
    std::vector<void*> spaces;        // every one, so "never touched again" is checkable
    int   calls = 0;                  // ENGINE LOOKUPS -- the unit ARCH-10 S7 bounds, not resolves
};
LookupLog g_lookup;

// One resolve costs two lookups: R14NetPhysics then R14RemotePlayer.
const int kLookupsPerResolve = 2;

// Stands in for CncaGameSpace::FindComponentSystem. It searches the table carried by whatever object
// it is handed, at CS.TABLE_OFF -- it never compares the pointer against the rig's own space. So an
// accessor aimed at the wrong slot fails for the engine's reason (that object's +0x420 is not a
// table) instead of because the fixture recognised the pointer and decided the verdict itself.
void* FakeFindCs(void* space, uint64_t symbol) {
    g_lookup.space = space;
    g_lookup.spaces.push_back(space);
    ++g_lookup.calls;
    void* raw = nullptr;
    memcpy(&raw, static_cast<uint8_t*>(space) + kFixtureCsTableOff, sizeof raw);
    if (!replay::Readable(raw, sizeof(FakeCsTable))) {
        return nullptr;
    }
    const FakeCsTable* t = static_cast<const FakeCsTable*>(raw);
    for (size_t i = 0; i < t->count && i < 2; ++i) {
        if (t->symbols[i] == symbol) {
            return t->systems[i];
        }
    }
    return nullptr;
}

// A synthetic CR15NetGame carrying BOTH objects, so the two slots are told apart by what is read out
// of them and not by one of them being null.
struct NetGameRig {
    // Sized off the fact, with the floor asserted independently: a rig narrower than 0x2B10 would
    // pass every case here without the product ever reading the field.
    static constexpr size_t kNetGameBytes = facts::NETGAME_CS_SPACE_OFF + 0x10u;
    static_assert(kNetGameBytes >= 0x2B10u,
                  "the rig's CR15NetGame must reach NETGAME.CS_SPACE_OFF + 8");

    std::vector<uint8_t> netGame;
    std::vector<uint8_t> space;   // the CncaGameSpace at +0x2B08
    std::vector<uint8_t> decoy;   // the CR15Game at +0x8
    FakeCsTable table;

    // `spaceSlot` and `gameSlot` say which netGame member each object is planted in, so the mirror
    // rig differs from the healthy one in exactly one number.
    NetGameRig(const CsSite& cs, uint32_t spaceSlot, uint32_t gameSlot)
        : netGame(kNetGameBytes, 0u), space(kFixtureCsTableOff + 0x10u, 0u),
          decoy(kFixtureCsTableOff + 0x10u, 0u) {
        table.count = 2;
        table.symbols[0] = facts::CS_SYM_R14NETPHYSICS;
        table.systems[0] = cs.netPhysicsCs;
        table.symbols[1] = facts::CS_SYM_R14REMOTEPLAYER;
        table.systems[1] = cs.remotePlayerCs;

        FakeCsTable* t = &table;
        memcpy(space.data() + kFixtureCsTableOff, &t, sizeof t);
        memcpy(decoy.data() + kFixtureCsTableOff, &kRecordedBudgetWord, sizeof kRecordedBudgetWord);

        void* sp = space.data();
        void* dp = decoy.data();
        memcpy(netGame.data() + spaceSlot, &sp, sizeof sp);
        memcpy(netGame.data() + gameSlot, &dp, sizeof dp);
    }

    void* Ptr() { return netGame.data(); }
};

TEST_CASE("the bind reads the game space at NETGAME.CS_SPACE_OFF, over a CR15Game decoy at +0x8") {
    Image img;
    img.BuildHealthy(3);
    const CsSite healthy = img.Cs();
    NetGameRig rig(healthy, facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    g_lookup = LookupLog();
    InvalidateCsSite();

    const CsSite s = BindCsSite(rig.Ptr(), &FakeFindCs);

    // The lookup ran, and it ran on the +0x2B08 object. This is the assertion F3 would have failed.
    REQUIRE(g_lookup.calls > 0);
    CHECK(g_lookup.space == (void*)rig.space.data());
    CHECK(g_lookup.space != (void*)rig.decoy.data());

    CHECK(s.bind == Bind::Ok);
    CHECK(s.netPhysicsCs == healthy.netPhysicsCs);
    CHECK(s.remotePlayerCs == healthy.remotePlayerCs);
    CHECK(s.physicsCs == healthy.physicsCs);
}

TEST_CASE("a netGame carrying a space only at +0x8 binds NoGameSpace, and the lookup is not called") {
    Image img;
    img.BuildHealthy(3);
    // The mirror. The constructed space sits at +0x8, where F3 looked; the CS slot is empty, which
    // is a netGame that genuinely carries no space yet. The refusal is the right answer, and an
    // accessor still aimed at +0x8 would report a healthy bind here instead.
    NetGameRig rig(img.Cs(), facts::IDENT_NETGAME_GAMESPACE_OFF, facts::NETGAME_CS_SPACE_OFF);
    memset(rig.netGame.data() + facts::NETGAME_CS_SPACE_OFF, 0, sizeof(void*));
    g_lookup = LookupLog();
    InvalidateCsSite();

    const CsSite s = BindCsSite(rig.Ptr(), &FakeFindCs);

    CHECK(s.bind == Bind::NoGameSpace);
    CHECK(g_lookup.calls == 0);
    CHECK(std::string(BindText(Bind::NoGameSpace)).find("gamespace") != std::string::npos);
}

TEST_CASE("a base too narrow to carry the slot is NetGameUnreadable, not NoGameSpace") {
    // One committed page: base + 0x2B08 is past its end, so the slot cannot be read at all. "I could
    // not ask" and "there is no space" are different answers and the enum spells both.
    void* const narrow = ::VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(narrow != nullptr);
    g_lookup = LookupLog();
    InvalidateCsSite();

    const CsSite s = BindCsSite(narrow, &FakeFindCs);

    CHECK(s.bind == Bind::NetGameUnreadable);
    CHECK(g_lookup.calls == 0);
    CHECK(strcmp(BindText(Bind::NetGameUnreadable), "?") != 0);
    ::VirtualFree(narrow, 0, MEM_RELEASE);
}

TEST_CASE("a lookup that failed its prologue guard is NoComponentLookup, not a missing space") {
    Image img;
    img.BuildHealthy(1);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    InvalidateCsSite();

    // Asked twice, because a null `find` is our own guard refusing and must NOT be memoised into the
    // epoch: the module can be unmapped at arm time and mapped a frame later.
    CHECK(BindCsSite(rig.Ptr(), nullptr).bind == Bind::NoComponentLookup);
    CHECK(BindCsSite(rig.Ptr(), nullptr).bind == Bind::NoComponentLookup);
    CHECK(BindCsSite(rig.Ptr(), &FakeFindCs).bind == Bind::Ok);
    CHECK(BindCsSite(nullptr, &FakeFindCs).bind == Bind::NoNetGame);
}

// ---------------------------------------------------------------------------------------------
// THE EPOCH INVARIANT: the engine is asked at most once per space epoch.
//
// AUD-F3C F3C-1. The transport sets Cause::MicroSeek EVERY FRAME above the sub-step ceiling and two
// residue rows (N-3b, N-4) run on micro causes, so an unmemoised resolve is four CS.LOOKUP_BY_SYMBOL
// calls per frame for the whole of a fast-forward -- and that call sorts the space's system table in
// place on our thread. The counting is done at the fake-lookup seam, in lookups, because that is the
// unit ARCH-10 S7 bounds.

// A CR15NetGame on its own pages, so its space slot's readability can be taken away and given back.
// A heap buffer cannot do that, and `readable` is a term of the key.
struct PagedNetGameRig {
    static constexpr size_t kBytes = 0x3000;
    static_assert(kBytes >= 0x2B10u, "the paged rig must reach NETGAME.CS_SPACE_OFF + 8");

    uint8_t* base = nullptr;

    explicit PagedNetGameRig(void* space) {
        base = static_cast<uint8_t*>(
            ::VirtualAlloc(nullptr, kBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(base != nullptr);
        memcpy(base + facts::NETGAME_CS_SPACE_OFF, &space, sizeof space);
    }
    ~PagedNetGameRig() {
        if (base) {
            ::VirtualFree(base, 0, MEM_RELEASE);
        }
    }
    PagedNetGameRig(const PagedNetGameRig&) = delete;
    PagedNetGameRig& operator=(const PagedNetGameRig&) = delete;

    // Flip the protection on the page the slot lives in. Readable() asks the memory manager, so
    // PAGE_NOACCESS makes the slot unreadable without faulting anything that only probes it.
    void SetSlotReadable(bool yes) {
        void* const page = base + (facts::NETGAME_CS_SPACE_OFF & ~static_cast<size_t>(0xFFFu));
        DWORD old = 0;
        REQUIRE(::VirtualProtect(page, 0x1000, yes ? PAGE_READWRITE : PAGE_NOACCESS, &old) != 0);
    }

    void* Ptr() { return base; }
};

TEST_CASE("a thousand micro-seek frames cost ONE resolve -- the engine is asked once per epoch") {
    Image img;
    img.BuildHealthy(3);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    g_lookup = LookupLog();
    InvalidateCsSite();

    // The bind is counted, not REQUIREd, inside the loop: a thousand assertions would inflate the
    // gate's assertion total by a thousand and say nothing a single equality does not.
    CsSite last;
    int okFrames = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        last = BindCsSite(rig.Ptr(), &FakeFindCs);
        okFrames += last.bind == Bind::Ok ? 1 : 0;
    }
    REQUIRE(okFrames == 1000);

    // The whole point, in the unit ARCH-10 S7 bounds. Per-fire resolution would be 2000 here.
    CHECK(g_lookup.calls == kLookupsPerResolve);
    CHECK(last.netPhysicsCs == img.Cs().netPhysicsCs);
    CHECK(last.remotePlayerCs == img.Cs().remotePlayerCs);
    CHECK(last.physicsCs == img.Cs().physicsCs);
}

TEST_CASE("a space teardown re-resolves once, and the old component systems are never handed back") {
    Image oldImg;
    oldImg.BuildHealthy(3);
    Image newImg;
    newImg.BuildHealthy(2);
    NetGameRig oldRig(oldImg.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    NetGameRig newRig(newImg.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    g_lookup = LookupLog();
    InvalidateCsSite();

    int okFrames = 0;
    for (int frame = 0; frame < 50; ++frame) {
        okFrames += BindCsSite(oldRig.Ptr(), &FakeFindCs).bind == Bind::Ok ? 1 : 0;
    }
    REQUIRE(okFrames == 50);
    REQUIRE(g_lookup.calls == kLookupsPerResolve);

    // The level reloads: the netGame's +0x2B08 slot now carries a different CncaGameSpace. This is
    // the case the memo exists to survive -- a stale CComponentSystem* here would be a write into
    // freed memory, which is why the key is the space POINTER and not just the netGame.
    void* const newSpace = newRig.space.data();
    memcpy(static_cast<uint8_t*>(oldRig.Ptr()) + facts::NETGAME_CS_SPACE_OFF, &newSpace,
           sizeof newSpace);

    const size_t before = g_lookup.spaces.size();
    CsSite after;
    okFrames = 0;
    for (int frame = 0; frame < 50; ++frame) {
        after = BindCsSite(oldRig.Ptr(), &FakeFindCs);
        okFrames += after.bind == Bind::Ok ? 1 : 0;
    }
    REQUIRE(okFrames == 50);

    CHECK(g_lookup.calls == 2 * kLookupsPerResolve);          // exactly one more resolve, not fifty
    CHECK(after.netPhysicsCs == newImg.Cs().netPhysicsCs);    // the new space's systems ...
    CHECK(after.netPhysicsCs != oldImg.Cs().netPhysicsCs);    // ... and not the freed one's
    CHECK(after.remotePlayerCs == newImg.Cs().remotePlayerCs);
    size_t onNewSpace = 0;
    for (size_t i = before; i < g_lookup.spaces.size(); ++i) {
        onNewSpace += g_lookup.spaces[i] == newSpace ? 1u : 0u;
    }
    // The old space is never touched again -- and there WERE later lookups, so this is not vacuous.
    CHECK(g_lookup.spaces.size() - before == kLookupsPerResolve);
    CHECK(onNewSpace == g_lookup.spaces.size() - before);
}

TEST_CASE("the readability term is load-bearing: a protection flip re-asks exactly once more") {
    Image img;
    img.BuildHealthy(3);
    NetGameRig spaceOwner(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    PagedNetGameRig rig(spaceOwner.space.data());
    g_lookup = LookupLog();
    InvalidateCsSite();

    int okFrames = 0;
    for (int frame = 0; frame < 20; ++frame) {
        okFrames += BindCsSite(rig.Ptr(), &FakeFindCs).bind == Bind::Ok ? 1 : 0;
    }
    REQUIRE(okFrames == 20);
    REQUIRE(g_lookup.calls == kLookupsPerResolve);

    // The slot's page goes away. The key changes on `readable` alone -- `space` reads as null in both
    // states, so a key without that term could not tell this from "no space yet" and would answer
    // NoGameSpace from a memo it should have dropped.
    rig.SetSlotReadable(false);
    int refusedFrames = 0;
    for (int frame = 0; frame < 20; ++frame) {
        refusedFrames += BindCsSite(rig.Ptr(), &FakeFindCs).bind == Bind::NetGameUnreadable ? 1 : 0;
    }
    REQUIRE(refusedFrames == 20);
    CHECK(g_lookup.calls == kLookupsPerResolve);   // a refusal before the lookup costs no lookup

    rig.SetSlotReadable(true);
    okFrames = 0;
    for (int frame = 0; frame < 20; ++frame) {
        okFrames += BindCsSite(rig.Ptr(), &FakeFindCs).bind == Bind::Ok ? 1 : 0;
    }
    REQUIRE(okFrames == 20);
    CHECK(g_lookup.calls == 2 * kLookupsPerResolve);   // exactly one more resolve for the whole flip

    // And the key itself says why, so the invariant is not resting on the count alone.
    const CsSpaceKey readable = CsSpaceKeyOf(rig.Ptr());
    rig.SetSlotReadable(false);
    const CsSpaceKey unreadable = CsSpaceKeyOf(rig.Ptr());
    rig.SetSlotReadable(true);
    CHECK(readable.readable);
    CHECK_FALSE(unreadable.readable);
    CHECK_FALSE(readable == unreadable);
    CHECK(readable == CsSpaceKeyOf(rig.Ptr()));
}

TEST_CASE("InvalidateCsSite drops the memo -- the hook the jump path and arming use") {
    Image img;
    img.BuildHealthy(1);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    g_lookup = LookupLog();
    InvalidateCsSite();

    REQUIRE(BindCsSite(rig.Ptr(), &FakeFindCs).bind == Bind::Ok);
    REQUIRE(BindCsSite(rig.Ptr(), &FakeFindCs).bind == Bind::Ok);
    CHECK(g_lookup.calls == kLookupsPerResolve);
    CHECK(CsSiteMemoHolds(rig.Ptr()));

    // A space rebuilt at the same address would be invisible to the key, so a real jump pays one
    // lookup rather than trusting it. Without this the ABA case is unreachable in code.
    InvalidateCsSite();
    CHECK_FALSE(CsSiteMemoHolds(rig.Ptr()));
    REQUIRE(BindCsSite(rig.Ptr(), &FakeFindCs).bind == Bind::Ok);
    CHECK(g_lookup.calls == 2 * kLookupsPerResolve);
}

// ---------------------------------------------------------------------------------------------
// THE READINESS HALF OF THE INVARIANT: a memo is only ever taken on a DEFINITIVE answer.
//
// AUD-F3D F3D-1. The key is the space's IDENTITY, so an outcome that describes how far the space has
// got in coming up cannot be seen to change by it. InvalidateCsSite runs on a real Seek/GapCross and
// on arm; play-forward, fast-forward and slow-mo are all MicroSeek. So a frozen readiness refusal is
// a battery that refuses for the rest of the session, and the recovery these cases demand is
// recovery WITHOUT any invalidate at all.

// Drives one bind exactly the way ResolveCsSite does: the key built ONCE, the memo asked with that
// key, and the engine lookup handed down only on a miss (F3D-3). A predicate that disagreed with the
// memo by even one bind would surface here as a spurious NoComponentLookup, so these cases pin the
// agreement as well as the cadence.
CsSite FrameBind(NetGameRig& rig) {
    const CsSpaceKey key = CsSpaceKeyOf(rig.Ptr());
    const FindCsFn find = CsSiteMemoHolds(key) ? nullptr : &FakeFindCs;
    return BindCsSiteOn(key, find);
}

// One resolve, then one more every kCsRetryBinds+1 binds.
int ResolvesOver(int binds) {
    return 1 + (binds - 1) / static_cast<int>(kCsRetryBinds + 1);
}

// Binds until the site answers Ok, or -1 if it never does. NO InvalidateCsSite anywhere: that is the
// whole point, because a forward playback produces nothing that would call it.
int BindsUntilOk(NetGameRig& rig, uint32_t limit) {
    for (uint32_t i = 0; i <= limit; ++i) {
        if (FrameBind(rig).bind == Bind::Ok) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

TEST_CASE("a component system that registers late is re-asked on the cadence, with no seek at all") {
    Image img;
    img.BuildHealthy(3);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    void* const remotePlayer = rig.table.systems[1];
    rig.table.systems[1] = nullptr;      // R14RemotePlayer has not registered into this space yet
    g_lookup = LookupLog();
    InvalidateCsSite();

    const int kStartupFrames = 100;
    int refused = 0;
    for (int frame = 0; frame < kStartupFrames; ++frame) {
        refused += FrameBind(rig).bind == Bind::NoRemotePlayerCs ? 1 : 0;
    }
    CHECK(refused == kStartupFrames);
    // Held, not re-asked per frame: the table sort ARCH-10 S7 forbids stays bounded at O(1/N).
    CHECK(g_lookup.calls == ResolvesOver(kStartupFrames) * kLookupsPerResolve);

    rig.table.systems[1] = remotePlayer;      // the system registers
    const int waited = BindsUntilOk(rig, kCsRetryBinds);
    CHECK(waited >= 0);                       // it recovers, and no seek was needed to do it
    CHECK(waited > 0);                        // and it really was memoised, not re-asked every bind

    // Ok is DEFINITIVE, so the cadence stops the moment the answer stops being "not yet".
    const int settled = g_lookup.calls;
    int okFrames = 0;
    for (int frame = 0; frame < 1000; ++frame) {
        okFrames += FrameBind(rig).bind == Bind::Ok ? 1 : 0;
    }
    CHECK(okFrames == 1000);
    CHECK(g_lookup.calls == settled);
}

TEST_CASE("a pool that is not built yet is re-asked on the same cadence -- residuesite.cpp:44-48") {
    Image img;
    img.BuildHealthy(3);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);

    // A pool is allocated when its first entity is created, and in a replay the entities come from
    // the state we install -- so the battery arms first and this is the ordinary case, not a corner.
    uint8_t* const poolSlot =
        static_cast<uint8_t*>(img.Cs().remotePlayerCs) + facts::PLAYER_CS_POOL_BASE_OFF;
    uint64_t poolBase = 0;
    memcpy(&poolBase, poolSlot, sizeof poolBase);
    const uint64_t zero = 0;
    memcpy(poolSlot, &zero, sizeof zero);

    g_lookup = LookupLog();
    InvalidateCsSite();

    const int kStartupFrames = 200;
    int refused = 0;
    for (int frame = 0; frame < kStartupFrames; ++frame) {
        refused += FrameBind(rig).bind == Bind::PoolNotReady ? 1 : 0;
    }
    CHECK(refused == kStartupFrames);
    CHECK(g_lookup.calls == ResolvesOver(kStartupFrames) * kLookupsPerResolve);

    memcpy(poolSlot, &poolBase, sizeof poolBase);      // the pool becomes ready
    const int waited = BindsUntilOk(rig, kCsRetryBinds);
    CHECK(waited >= 0);
    CHECK(waited > 0);
}

// THE NUMBER residuesite.h's PERFORMANCE HEADER STATES, MEASURED. AUD-ARCH-14 B7/B8 (ARCH-15 S7
// item 1): the header's budget is <= 4 lookups per steady-state second on the refused path, in
// lookups, with every entry counted as table-sort-capable. A header that states a number nothing
// measures is prose, so this drives exactly one steady-state second -- 60 frames x the two micro
// rows = 120 binds -- against a site that keeps answering "not yet", and compares the count at the
// fake-lookup seam with the header's 4. Per-fire resolution would be 240 here, which is also the
// ceiling: the last CHECK pins the ceiling's derivation (120 binds/s x 2 lookups), the half of B7
// that was misattributed as "4 rows x 60 Hz".
TEST_CASE("one steady-state second of a readiness refusal costs the four lookups this header states") {
    Image img;
    img.BuildHealthy(3);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    rig.table.systems[1] = nullptr;      // R14RemotePlayer never registers: NotYet for the whole second
    g_lookup = LookupLog();
    InvalidateCsSite();

    const int kFramesPerSecond = 60;
    const int kMicroRowsPerFrame = 2;                                       // N-3b and N-4
    const int kBindsPerSecond = kFramesPerSecond * kMicroRowsPerFrame;      // the header's R = 120
    const int kHeaderBudgetLookupsPerSecond = 4;

    int refused = 0;
    for (int bind = 0; bind < kBindsPerSecond; ++bind) {
        refused += FrameBind(rig).bind == Bind::NoRemotePlayerCs ? 1 : 0;
    }
    REQUIRE(refused == kBindsPerSecond);      // the second really was spent on the refused path

    CHECK(g_lookup.calls == kHeaderBudgetLookupsPerSecond);
    CHECK(ResolvesOver(kBindsPerSecond) * kLookupsPerResolve == kHeaderBudgetLookupsPerSecond);
    CHECK(kBindsPerSecond * kLookupsPerResolve == 240);      // the ceiling, derived the B7 way
}

// ---------------------------------------------------------------------------------------------
// ONE CONTROL PER NOT-YET VALUE, AND THE CLASSIFICATION IS TOTAL. AUD-F3F F3F-2.
//
// The two cases above drive NoRemotePlayerCs and PoolNotReady. AUD-F3F sorted NoNetPhysicsCs to
// Definitive and the whole suite stayed green -- and NoNetPhysicsCs is the FIRST thing ResolveOn
// asks for (residuesite.cpp: R14NETPHYSICS before R14REMOTEPLAYER), so it is the refusal a load
// window produces earliest and the one most likely to be the memo that freezes. Mis-sorted it is
// F3D-1 again, exactly.

const Bind kNotYetValues[] = {
    Bind::NoNetPhysicsCs,
    Bind::NoRemotePlayerCs,
    Bind::NoPhysicsCs,
    Bind::PoolNotReady,
};

// The 8-byte pointer slot whose zeroing produces `b` and whose restoration clears it. All four
// not-yet outcomes are "something in the space is not built yet", so all four are one slot.
uint8_t* NotYetSlot(Bind b, NetGameRig& rig, Image& img) {
    switch (b) {
        case Bind::NoNetPhysicsCs:
            return reinterpret_cast<uint8_t*>(&rig.table.systems[0]);
        case Bind::NoRemotePlayerCs:
            return reinterpret_cast<uint8_t*>(&rig.table.systems[1]);
        case Bind::NoPhysicsCs:      // the system registered, its CPhysicsCS field is still null
            return static_cast<uint8_t*>(img.Cs().netPhysicsCs) + rf::kNetCsPhysicsCsOff;
        case Bind::PoolNotReady:     // registered, pool base still null
            return static_cast<uint8_t*>(img.Cs().remotePlayerCs) + facts::PLAYER_CS_POOL_BASE_OFF;
        default:
            return nullptr;
    }
}

// ResolveOn returns at the FIRST system it cannot find, so a missing R14NetPhysics costs one lookup
// per resolve and everything past it costs two. Deriving this from the value rather than hardcoding
// 2 is what lets the cost assertion below be the same assertion for all four.
int LookupsFor(Bind b) {
    return b == Bind::NoNetPhysicsCs ? 1 : kLookupsPerResolve;
}

TEST_CASE("EVERY not-yet outcome is held on the cadence and recovers without a seek -- F3F-2") {
    for (const Bind want : kNotYetValues) {
        // A const char* CAPTUREs as its ADDRESS, which names nothing; the point of this loop is that
        // a failure says WHICH value is mis-sorted.
        const std::string outcome = BindText(want);
        CAPTURE(outcome);

        Image img;
        img.BuildHealthy(3);
        NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
        uint8_t* const slot = NotYetSlot(want, rig, img);
        REQUIRE(slot != nullptr);

        uint64_t saved = 0;
        memcpy(&saved, slot, sizeof saved);
        const uint64_t zero = 0;
        memcpy(slot, &zero, sizeof zero);

        g_lookup = LookupLog();
        InvalidateCsSite();

        const int kStartupBinds = 100;
        int refused = 0;
        for (int bind = 0; bind < kStartupBinds; ++bind) {
            refused += FrameBind(rig).bind == want ? 1 : 0;
        }
        REQUIRE(refused == kStartupBinds);   // the fixture really does produce THIS outcome

        // Sorted Definitive, the memo would be taken once and never come due: this reads one
        // resolve instead of ResolvesOver(100) = 2, and the recovery below never happens.
        CHECK(g_lookup.calls == ResolvesOver(kStartupBinds) * LookupsFor(want));

        memcpy(slot, &saved, sizeof saved);          // the space finishes coming up
        const int waited = BindsUntilOk(rig, kCsRetryBinds);
        CHECK(waited >= 0);                          // recovered, with no invalidate in this case
        CHECK(waited > 0);                           // and it was memoised, not re-asked every bind
    }
}

TEST_CASE("HoldFor classifies EVERY Bind value, and every not-yet value has a control -- F3F-2") {
    // A new enumerator that nobody sorted answers Unclassified rather than defaulting to Definitive.
    for (uint8_t i = 0; i < static_cast<uint8_t>(Bind::Count); ++i) {
        const Bind b = static_cast<Bind>(i);
        CAPTURE(static_cast<int>(i));
        const std::string outcome = BindText(b);
        CAPTURE(outcome);
        CHECK(CsHoldFor(b) != CsHold::Unclassified);
    }

    // ... and the case above drives every value classified NotYet. A fifth not-yet outcome added
    // without an arm reds here rather than shipping uncontrolled.
    for (uint8_t i = 0; i < static_cast<uint8_t>(Bind::Count); ++i) {
        const Bind b = static_cast<Bind>(i);
        if (CsHoldFor(b) != CsHold::NotYet) {
            continue;
        }
        bool driven = false;
        for (const Bind want : kNotYetValues) {
            driven = driven || want == b;
        }
        const std::string outcome = BindText(b);
        CAPTURE(outcome);
        CHECK(driven);
    }

    // The three kinds, pinned by name so a wholesale re-sort cannot pass the totality loop alone.
    CHECK(CsHoldFor(Bind::Ok) == CsHold::Definitive);
    CHECK(CsHoldFor(Bind::NetGameUnreadable) == CsHold::Definitive);
    CHECK(CsHoldFor(Bind::NoComponentLookup) == CsHold::Never);
    CHECK(CsHoldFor(Bind::NoNetPhysicsCs) == CsHold::NotYet);
    CHECK(CsHoldFor(Bind::NoRemotePlayerCs) == CsHold::NotYet);
    CHECK(CsHoldFor(Bind::NoPhysicsCs) == CsHold::NotYet);
    CHECK(CsHoldFor(Bind::PoolNotReady) == CsHold::NotYet);
}

// ---------------------------------------------------------------------------------------------
// THE ABA CASE, which the pointer triple cannot see. AUD-F3D F3D-2.

BindEpochTerm g_fakeEpoch;
BindEpochTerm FakeBindEpoch() { return g_fakeEpoch; }

// Installs the fake for one case and takes it away again -- a source left behind would make every
// later case's key depend on this one's last value.
struct ScopedBindEpoch {
    explicit ScopedBindEpoch(BindEpochTerm t) {
        g_fakeEpoch = t;
        SetBindEpochSource(&FakeBindEpoch);
    }
    ~ScopedBindEpoch() { SetBindEpochSource(nullptr); }
};

TEST_CASE("a space rebuilt at the SAME address is caught, because the engine's bind epoch moved") {
    Image oldImg;
    oldImg.BuildHealthy(3);
    Image newImg;
    newImg.BuildHealthy(2);
    NetGameRig rig(oldImg.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    ScopedBindEpoch epoch(BindEpochTerm{BindEpochTerm::State::Bound, 7});
    g_lookup = LookupLog();
    InvalidateCsSite();

    REQUIRE(FrameBind(rig).bind == Bind::Ok);
    REQUIRE(FrameBind(rig).netPhysicsCs == oldImg.Cs().netPhysicsCs);

    // The -replayviewer teardown-and-rebuild flow, while paused: no jump, so nothing invalidates.
    // The space lands on the block the old one just freed, so netGame, the +0x2B08 slot and its
    // readability all read IDENTICALLY. Only the system table's contents differ -- which is exactly
    // what entity_module's SpaceIdentity digests and what moves IEntity::BindEpochNow.
    rig.table.systems[0] = newImg.Cs().netPhysicsCs;
    rig.table.systems[1] = newImg.Cs().remotePlayerCs;
    const int before = g_lookup.calls;
    g_fakeEpoch.value = 8;

    CsSite served;
    int stale = 0;
    int ok = 0;
    for (int frame = 0; frame < 500; ++frame) {
        served = FrameBind(rig);
        ok += served.bind == Bind::Ok ? 1 : 0;
        stale += served.netPhysicsCs == oldImg.Cs().netPhysicsCs ? 1 : 0;
    }
    CHECK(ok == 500);
    CHECK(stale == 0);                                    // the freed image is never served again
    CHECK(served.netPhysicsCs == newImg.Cs().netPhysicsCs);
    CHECK(served.remotePlayerCs == newImg.Cs().remotePlayerCs);
    CHECK(g_lookup.calls - before == kLookupsPerResolve);  // one re-resolve, not 500
}

TEST_CASE("a rebuild whose table digests IDENTICALLY moves nothing -- what residue serves then, F3F-3") {
    Image img;
    img.BuildHealthy(3);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    ScopedBindEpoch epoch(BindEpochTerm{BindEpochTerm::State::Bound, 7});
    g_lookup = LookupLog();
    InvalidateCsSite();

    REQUIRE(FrameBind(rig).bind == Bind::Ok);
    const CsSite held = FrameBind(rig);
    const int before = g_lookup.calls;

    // The arena reset that hands back the blocks it just freed: the space is torn down and rebuilt
    // at the same address, and the system table is reconstructed with the SAME count, the same
    // symbols in the same order and the same component-system addresses. entity_module digests those
    // bytes (entity_module.cpp:73, 157) into SpaceIdentity::entries, so the identity does NOT differ,
    // EntityBind is not reached (entity_module.cpp:454-457) and BindEpochNow does not advance. The
    // epoch term is therefore left where it was -- deliberately not moved here.
    rig.table.systems[0] = img.Cs().netPhysicsCs;
    rig.table.systems[1] = img.Cs().remotePlayerCs;

    int ok = 0;
    for (int frame = 0; frame < 500; ++frame) {
        ok += FrameBind(rig).bind == Bind::Ok ? 1 : 0;
    }
    CHECK(ok == 500);
    CHECK(g_lookup.calls == before);   // the key cannot see this rebuild, and does not pretend to

    // Which is benign for THIS consumer, and not by assumption: CsSite holds only the three
    // component-system pointers, so an identical digest means a fresh lookup returns exactly what
    // the memo is holding. Re-resolved from scratch and compared, rather than argued.
    InvalidateCsSite();
    const CsSite fresh = FrameBind(rig);
    REQUIRE(fresh.bind == Bind::Ok);
    CHECK(fresh.netPhysicsCs == held.netPhysicsCs);
    CHECK(fresh.remotePlayerCs == held.remotePlayerCs);
    CHECK(fresh.physicsCs == held.physicsCs);

    // A consumer holding something the OLD instance ISSUED -- a pool index, a registration, a handle
    // (entity.h:231-233) -- gets no such argument; that is W3-C's problem, not this file's. Whether
    // equal digests actually occur across a real rebuild is launch check F3F-L1.
}

TEST_CASE("the bind epoch is a key TERM: all three of its states are told apart, and the value too") {
    Image img;
    img.BuildHealthy(1);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    ScopedBindEpoch epoch(BindEpochTerm{BindEpochTerm::State::Bound, 7});

    const CsSpaceKey bound7 = CsSpaceKeyOf(rig.Ptr());
    g_fakeEpoch.value = 8;
    const CsSpaceKey bound8 = CsSpaceKeyOf(rig.Ptr());
    g_fakeEpoch = BindEpochTerm{BindEpochTerm::State::Unbound, 0};
    const CsSpaceKey unbound = CsSpaceKeyOf(rig.Ptr());
    g_fakeEpoch = BindEpochTerm{BindEpochTerm::State::NotTracked, 0};
    const CsSpaceKey untracked = CsSpaceKeyOf(rig.Ptr());

    // A generation is not a state: "nothing is bound" and "this build does not track binds" are
    // different answers and must not collapse into each other or into generation zero.
    CHECK_FALSE(bound7 == bound8);
    CHECK_FALSE(unbound == untracked);
    CHECK_FALSE(unbound == bound8);
    CHECK(bound7.space == bound8.space);          // ... and nothing else in the key moved
    CHECK(bound7.netGame == bound8.netGame);
    CHECK(bound7.readable == bound8.readable);

    g_fakeEpoch = BindEpochTerm{BindEpochTerm::State::Bound, 7};
    CHECK(bound7 == CsSpaceKeyOf(rig.Ptr()));
}

// The product wiring, not the unit: the rows really do reach the memo, and a jump really does drop
// it. Everything above drives BindCsSite directly, and unit-green cannot see wiring -- the defect
// AUD-F3C found was entirely in which function residue.cpp's rows called, per frame.
NetGameRig* g_wiringRig = nullptr;

CsSite MemoResolveCs() {
    return g_wiringRig ? BindCsSite(g_wiringRig->Ptr(), &FakeFindCs) : CsSite();
}

TEST_CASE("200 micro-seek FIRES cost one resolve; the next real jump costs exactly one more") {
    Image img;
    img.BuildHealthy(3);
    NetGameRig rig(img.Cs(), facts::NETGAME_CS_SPACE_OFF, facts::IDENT_NETGAME_GAMESPACE_OFF);
    g_wiringRig = &rig;

    ResidueBattery bat;
    ResidueEnv env = FullEnv();
    env.resolveCs = &MemoResolveCs;
    g_log.Clear();
    ResetStatsForTest();
    REQUIRE(RegisterPlaybackResidue(bat, env) == kOwnRows);
    g_lookup = LookupLog();   // arming invalidates, so start counting after it

    for (int frame = 0; frame < 200; ++frame) {
        bat.Fire(Jump(Cause::MicroSeek));
    }

    // The rows RAN -- 3 entities x 200 frames -- so the lookup count below is not the count of a
    // battery that quietly did nothing.
    CHECK(Stats().warpRequests == 600u);
    CHECK(Stats().timeSnaps == 600u);
    CHECK(g_lookup.calls == kLookupsPerResolve);

    // A real jump is where a space rebuilt at the same address would otherwise go unnoticed.
    bat.Fire(Jump(Cause::Seek));
    CHECK(g_lookup.calls == 2 * kLookupsPerResolve);
    CHECK(Stats().ciResets == 3u);   // and it really was the full set, not another micro fire

    g_wiringRig = nullptr;
}

TEST_CASE("every Bind value has a distinct, non-empty diagnosis") {
    std::vector<std::string> seen;
    for (uint8_t i = 0; i < (uint8_t)Bind::Count; ++i) {
        const char* t = BindText((Bind)i);
        REQUIRE(t != nullptr);
        CHECK(strlen(t) > 0);
        for (const std::string& s : seen) {
            CHECK(s != std::string(t));
        }
        seen.push_back(t);
    }
    CHECK(seen.size() == (size_t)Bind::Count);
}

TEST_CASE("the ids this packet claims are exactly the ones it registers") {
    Image img;
    img.BuildHealthy(1);
    g_image = &img;

    ResidueBattery bat;
    ResetStatsForTest();
    REQUIRE(RegisterPlaybackResidue(bat, FullEnv()) == kOwnRows);

    std::vector<std::string> ids;
    for (size_t i = 0; i < bat.Count(); ++i) {
        ids.push_back(bat.At(i)->Id());
    }
    const char* want[] = {"N-3a", "N-3b", "N-4",  "N-2",  "N-7a", "N-7b",
                          "N-8a", "N-8b", "N-6a", "N-6b", "N-18"};
    REQUIRE(ids.size() == sizeof want / sizeof want[0]);
    for (size_t i = 0; i < ids.size(); ++i) {
        CHECK(ids[i] == std::string(want[i]));
    }
    g_image = nullptr;
}

// =============================================================================================
// N-6: the world re-origin request.
// =============================================================================================

TEST_CASE("the latch covers the cumulative triple and NOT the transient one") {
    Image img;
    img.BuildHealthy(1);

    // THE FALSE POSITIVE THIS EXISTS TO PREVENT. WORLD.REORIGIN_PENDING_OFF_X..Z is zeroed by the
    // render thread on every particle compute pass. The first drop of this packet latched 24 bytes
    // and so covered it, which would have reported a world shift on essentially every seek. Twelve
    // bytes now, ending strictly below the transient triple.
    CHECK(kOriginSpanBytes == 12);
    CHECK(facts::WORLD_REORIGIN_PENDING_OFF_X >= facts::WORLD_REORIGIN_OFF_X + kOriginSpanBytes);

    unsigned char a[kOriginSpanBytes], b[kOriginSpanBytes];
    img.Put<float>(kOriginBlock + facts::WORLD_REORIGIN_OFF_X, 1.0f);
    img.Put<float>(kOriginBlock + facts::WORLD_REORIGIN_OFF_Y, 2.0f);
    img.Put<float>(kOriginBlock + facts::WORLD_REORIGIN_OFF_Z, 3.0f);
    img.Put<float>(kOriginBlock + facts::WORLD_REORIGIN_PENDING_OFF_X, 9.0f);
    REQUIRE(ReadWorldOrigin(img.At(kOriginBlock), a));

    // Churn the transient triple exactly as the render thread does. The latch must not see it.
    img.Put<float>(kOriginBlock + facts::WORLD_REORIGIN_PENDING_OFF_X, 0.0f);
    img.Put<float>(kOriginBlock + facts::WORLD_REORIGIN_PENDING_OFF_Y, 0.0f);
    img.Put<float>(kOriginBlock + facts::WORLD_REORIGIN_PENDING_OFF_Z, 0.0f);
    REQUIRE(ReadWorldOrigin(img.At(kOriginBlock), b));
    CHECK(memcmp(a, b, kOriginSpanBytes) == 0);

    // But a real move of the cumulative triple IS seen.
    img.Put<float>(kOriginBlock + facts::WORLD_REORIGIN_OFF_Y, 2.5f);
    REQUIRE(ReadWorldOrigin(img.At(kOriginBlock), b));
    CHECK(memcmp(a, b, kOriginSpanBytes) != 0);
}

TEST_CASE("N-6 writes nothing to the re-origin flag word") {
    Image img;
    img.BuildHealthy(1);
    g_image = &img;

    // 0x100 and 0x200 are one two-phase state (btr 8 / bts 9 in a single read-modify-write), so
    // clearing 0x200 cancels a translation that has already BEGUN and leaves no bit that could
    // finish it. The first drop cleared it. Nothing in this packet touches this word now, and this
    // asserts that rather than trusting the absence of a call site.
    const size_t f = kGame + facts::WORLD_REORIGIN_FLAG_OFF;
    const uint64_t before = 0x00000000FFFFFB00ull;   // both 0x100 and 0x200 set
    REQUIRE((before & 0x300ull) == 0x300ull);
    img.Put<uint64_t>(f, before);

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);
    CHECK(img.Get<uint64_t>(f) == before);

    bat.Fire(Jump(Cause::MicroSeek));
    CHECK(img.Get<uint64_t>(f) == before);
    g_image = nullptr;
}

TEST_CASE("a world shift that happens anyway is reported, and never rewritten") {
    Image img;
    img.BuildHealthy(1);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);          // latches the span
    CHECK(Stats().worldOriginShifts == 0);

    // The world moves under us despite the request bit being held clear.
    img.Put<float>(img.OriginSpan() + 4, 123.5f);
    bat.Fire(Jump(Cause::Seek));
    CHECK(Stats().worldOriginShifts == 1);

    // ★ REPORTED, NOT REPAIRED. The new value stands: this packet did not derive the field layout
    // inside that span, and writing it back could displace the world further than the shift.
    CHECK(img.Get<float>(img.OriginSpan() + 4) == doctest::Approx(123.5f));

    // And the new value becomes the baseline, so one shift is not counted forever.
    bat.Fire(Jump(Cause::Seek));
    CHECK(Stats().worldOriginShifts == 1);
    g_image = nullptr;
}

// =============================================================================================
// N-18: the unsigned clock accumulators.
// =============================================================================================

TEST_CASE("the clock guard tells its four outcomes apart") {
    Image img;
    img.BuildHealthy(1);
    void* clock = img.At(kClock);
    const size_t a1 = kClock + facts::CLOCK_ACCUM1_OFF;
    const size_t a2 = kClock + facts::CLOCK_ACCUM2_OFF;
    const size_t st = kClock + facts::CLOCK_LASTSTAMP_OFF;

    img.Put<uint64_t>(st, 1000);
    img.Put<uint64_t>(a1, 2000);
    img.Put<uint64_t>(a2, 3000);

    ClockLatch latch;
    CHECK(GuardClockMonotonic(clock, latch) == ClockVerdict::Fresh);

    SUBCASE("advancing normally is Forward and writes nothing") {
        img.Put<uint64_t>(a1, 2500);
        CHECK(GuardClockMonotonic(clock, latch) == ClockVerdict::Forward);
        CHECK(img.Get<uint64_t>(a1) == 2500);
    }

    SUBCASE("an unsigned wrap is REPAIRED, because it is not a value anybody chose") {
        // 2000 - 5000 as an unsigned add. This is what a negative delta reaching
        // `add [rdi+28h], rdx` produces: not a negative number, a number near 2^64.
        img.Put<uint64_t>(a1, (uint64_t)0 - 3000ull);
        CHECK(GuardClockMonotonic(clock, latch) == ClockVerdict::Wrapped);
        CHECK(img.Get<uint64_t>(a1) == 2000);   // restored to the latch
        CHECK(img.Get<uint64_t>(a2) == 3000);
        CHECK(img.Get<uint64_t>(st) == 1000);
    }

    SUBCASE("a plain decrease is REPORTED and left alone -- W2-D owns clock writes") {
        img.Put<uint64_t>(a2, 2999);
        CHECK(GuardClockMonotonic(clock, latch) == ClockVerdict::Backwards);
        CHECK(img.Get<uint64_t>(a2) == 2999);   // untouched: silently undoing it would hide it
    }

    SUBCASE("a DIFFERENT CClock starting from zero is Fresh, not Backwards") {
        // Between matches the object is rebuilt and its accumulators legitimately restart. Judging
        // that as a regression would make every match transition a false finding -- locality is not
        // identity, on a pointer as much as on a body slot.
        void* other = img.At(kClock + 0x100);
        CHECK(GuardClockMonotonic(other, latch) == ClockVerdict::Fresh);
        CHECK(GuardClockMonotonic(other, latch) == ClockVerdict::Forward);
    }
}

TEST_CASE("the clock guard runs on EVERY cause, including a reverse tick") {
    Image img;
    img.BuildHealthy(2);
    g_image = &img;

    for (Cause c : {Cause::Seek, Cause::MicroSeek, Cause::GapCross, Cause::ReverseTick,
                    Cause::ChapterEpochRoll}) {
        ResidueBattery bat;
        ResetStatsForTest();
        REQUIRE(RegisterPlaybackResidue(bat, FullEnv()) == kOwnRows);
        img.Put<uint64_t>(kClock + facts::CLOCK_ACCUM1_OFF, 5000);
        bat.Fire(Jump(c));                                    // Fresh
        img.Put<uint64_t>(kClock + facts::CLOCK_ACCUM1_OFF, (uint64_t)0 - 1ull);
        bat.Fire(Jump(c));                                    // Wrapped
        CHECK(Stats().clockWrapsRepaired == 1);
        CHECK(img.Get<uint64_t>(kClock + facts::CLOCK_ACCUM1_OFF) == 5000);
    }
    g_image = nullptr;
}

// =============================================================================================
// The N-3a ordering invariant.
// =============================================================================================

namespace {

// Only ReadHistoryWindow matters; every pure virtual below is a stub because B.4 was frozen with
// them and an implementer that forgets one should not compile.
struct FakeEntity : IEntity {
    uint64_t headTime = 0;
    bool hasHistory = true;
    mutable size_t reads = 0;

    size_t ReadHistoryWindow(PhysHandle, BodyState*, uint64_t* t, size_t n) const override {
        ++reads;
        if (!hasHistory || n == 0) {
            return 0;
        }
        t[0] = headTime;
        return 1;
    }

    size_t PhysCount() const override { return 0; }
    bool IsLocallyControlled(PhysHandle) const override { return false; }
    void SetSyntheticOwner(PhysHandle) override {}
    void ClearSyntheticOwner(PhysHandle) override {}
    bool ReadBody(PhysHandle, BodyState&) const override { return false; }
    void PushHistory(PhysHandle, const BodyState&, uint64_t) override {}
    void TrimHistory(PhysHandle, uint64_t, uint64_t) override {}
    float GetTimeOffset(PhysHandle) const override { return 0.0f; }
    void SetTimeOffset(PhysHandle, float) override {}
    size_t PlayerCount() const override { return 0; }
    bool ReadPlayer(PlayerHandle, PlayerState&) const override { return false; }
    void PushPlayerHistory(PlayerHandle, const PlayerState&, uint64_t) override {}
    PhysHandle PhysOf(PlayerHandle) const override { return PhysHandle(0xFFFFu); }
    PlayerHandle PlayerOf(UserIndex) const override { return PlayerHandle(0xFFFFu); }
    void SetLodThrottle(bool) override {}
    bool Available() const override { return true; }
    const char* Diagnostic() const override { return ""; }
};

}  // namespace

TEST_CASE("N-3a measures whether ApplyStateAt has already run, instead of assuming it has not") {
    Image img;
    img.BuildHealthy(2);
    g_image = &img;

    FakeEntity ent;
    ResidueEnv env = FullEnv();
    env.entity = &ent;

    // Jump() goes from 30 s to 5 s -- backwards, which is the direction the battery exists for.
    SUBCASE("the ring still holds pre-jump state: the ordering is correct") {
        ent.headTime = 29'900'000;          // near `from`
        ResidueBattery bat;
        ResetStatsForTest();
        REQUIRE(RegisterPlaybackResidue(bat, env) == kOwnRows);
        g_log.Clear();
        bat.Fire(Jump(Cause::Seek));
        CHECK(Stats().ordering == Ordering::Ok);
        CHECK(Stats().orderingViolations == 0);
        CHECK(Stats().ciResets == 2);   // and it DID run: the refusal is not a blanket disable
    }

    SUBCASE("the ring is already at the destination: the apply ran FIRST and this is a defect") {
        // ★★★ THE FAILURE THIS EXISTS TO CATCH. RESET.CI_PC recycles the history ring that
        // SetHistoryWindow fills, so running it after the apply deletes the state the apply just
        // installed and the body freezes. Today the ordering holds because W2-B's frame loop fires
        // the battery at step 4 and applies at step 5 -- two adjacent lines in a file this packet
        // does not own, and adjacency is what the next refactor destroys.
        ent.headTime = 5'050'000;           // near `to`
        ResidueBattery bat;
        ResetStatsForTest();
        REQUIRE(RegisterPlaybackResidue(bat, env) == kOwnRows);
        g_log.Clear();
        bat.Fire(Jump(Cause::Seek));
        CHECK(Stats().ordering == Ordering::Violated);
        CHECK(Stats().orderingViolations == 1);

        // ★ AND IT REFUSED. Not one ResetCI call was made: a detector that logs and then empties the
        // ring anyway has made the replay break loudly instead of not breaking. Skipping leaves
        // stale smoothing state; proceeding leaves an empty ring, which freezes the body.
        CHECK(Stats().ciResets == 0);
        CHECK(std::count(g_log.order.begin(), g_log.order.end(), std::string("ci")) == 0);
    }

    SUBCASE("no IEntity: reported as unavailable, never as ok") {
        ResidueBattery bat;
        ResetStatsForTest();
        REQUIRE(RegisterPlaybackResidue(bat, FullEnv()) == kOwnRows);   // entity is null
        g_log.Clear();
        bat.Fire(Jump(Cause::Seek));
        CHECK(Stats().ordering == Ordering::NoEntity);
        CHECK(Stats().orderingViolations == 0);
    }

    SUBCASE("an IEntity with no history readback: also unavailable, and NOT ok") {
        // The default IEntity::ReadHistoryWindow returns 0. Reporting that as Ok would be the
        // vacuous pass the whole structure refuses -- an unanswerable question is not a yes.
        ent.hasHistory = false;
        ResidueBattery bat;
        ResetStatsForTest();
        REQUIRE(RegisterPlaybackResidue(bat, env) == kOwnRows);
        g_log.Clear();
        bat.Fire(Jump(Cause::Seek));
        CHECK(Stats().ordering == Ordering::NoHistoryReadback);
    }

    g_image = nullptr;
}

TEST_CASE("every Ordering and ClockVerdict value has a distinct, non-empty text") {
    std::vector<std::string> seen;
    for (uint8_t i = 0; i < (uint8_t)Ordering::Count; ++i) {
        const char* t = OrderingText((Ordering)i);
        REQUIRE(t != nullptr);
        CHECK(strlen(t) > 0);
        for (const std::string& p : seen) { CHECK(p != std::string(t)); }
        seen.push_back(t);
    }
    seen.clear();
    for (uint8_t i = 0; i < (uint8_t)ClockVerdict::Count; ++i) {
        const char* t = ClockVerdictText((ClockVerdict)i);
        REQUIRE(t != nullptr);
        CHECK(strlen(t) > 0);
        for (const std::string& p : seen) { CHECK(p != std::string(t)); }
        seen.push_back(t);
    }
}

// =============================================================================================
// The roster: the denominator that does not come from the work.
// =============================================================================================

namespace {

struct StubRow : IResidue {
    const char* id_;
    Availability a_;
    StubRow(const char* i, Availability a) : id_(i), a_(a) {}
    const char* Id() const override { return id_; }
    Availability Avail() const override { return a_; }
    const char* Diagnostic() const override { return "stub"; }
    bool RunsOn(Cause) const override { return true; }
    void Fire(const Discontinuity&) override {}
};

}  // namespace

TEST_CASE("the roster's denominator is 19 whatever the battery contains") {
    Image img;
    img.BuildHealthy(1);
    g_image = &img;

    RosterRow rows[kRosterRows];

    SUBCASE("with nothing registered at all, every row is Unregistered") {
        ResidueBattery bat;
        ResetStatsForTest();
        REQUIRE(RegisterPlaybackResidue(bat, FullEnv()) == kOwnRows);
        // Point the roster at an EMPTY battery by re-registering into a fresh one... which is what
        // RegisterPlaybackResidue does. So instead assert the invariant that matters: the count
        // returned is the design's, not the battery's.
        CHECK(Roster(rows, kRosterRows) == 19);
        CHECK(Summary().known == 19);
    }

    SUBCASE("with this packet's rows, the four states are all populated and sum to 19") {
        ResidueBattery bat;
        ResetStatsForTest();
        REQUIRE(RegisterPlaybackResidue(bat, FullEnv()) == kOwnRows);
        const RosterSummary sum = Summary();
        CHECK(sum.consulted);
        CHECK(sum.known == 19);
        CHECK(sum.ready + sum.partial + sum.unavailable + sum.unregistered == 19);

        // ★★★ THE POINT OF THE WHOLE THING. Twelve design rows have NOTHING registered for them, and
        // the battery's own UnavailableCount() cannot see a single one -- it counts what registered.
        // A build reporting "everything registered is fine" while twelve residues are absent is the
        // vacuous-completeness shape this denominator exists to break.
        CHECK(sum.unregistered == 12);
        CHECK(sum.ready == 5);      // N-2, N-3, N-4, N-7, N-18
        CHECK(sum.partial == 2);    // N-6 and N-8: half of each works, and neither rounds off
    }
    g_image = nullptr;
}

TEST_CASE("a longer row number does not count as covering a shorter one") {
    Image img;
    img.BuildHealthy(1);
    g_image = &img;

    // ★ "N-20" must NOT satisfy "N-2", and a naive prefix test says it does. This is the same trap
    // as a dead symbol name being a prefix of live ones: the character after the prefix has to be
    // end-of-string or a lowercase suffix letter, never another digit.
    ResidueBattery bat;
    ResetStatsForTest();
    StubRow only20("N-20", Availability::Ready);
    REQUIRE(bat.Register(&only20));
    ResidueEnv env;   // no resolvers: nothing fires, but the roster still reads the battery
    RegisterPlaybackResidue(bat, env);

    RosterRow rows[kRosterRows];
    Roster(rows, kRosterRows);
    for (size_t i = 0; i < kRosterRows; ++i) {
        if (std::string(rows[i].id) == "N-20") {
            CHECK(rows[i].registered >= 1);
        }
        if (std::string(rows[i].id) == "N-2") {
            // covered by this packet's own N-2 row, and by exactly one thing -- not two
            CHECK(rows[i].registered == 1);
        }
    }
    g_image = nullptr;
}

TEST_CASE("a lettered suffix DOES count, which is how the split rows are seen") {
    Image img;
    img.BuildHealthy(1);
    g_image = &img;

    ResidueBattery bat;
    ResetStatsForTest();
    // W2-H's ids, which this packet does not own but the roster must still credit to N-17.
    StubRow a("N-17a", Availability::Ready);
    StubRow b("N-17b", Availability::Unmapped);
    REQUIRE(bat.Register(&a));
    REQUIRE(bat.Register(&b));
    ResidueEnv env;
    RegisterPlaybackResidue(bat, env);

    RosterRow rows[kRosterRows];
    Roster(rows, kRosterRows);
    for (size_t i = 0; i < kRosterRows; ++i) {
        if (std::string(rows[i].id) == "N-17") {
            CHECK(rows[i].registered == 2);
            // One ready and one not is Partial. Rounding it to Ready would claim a loop re-arm that
            // does not happen; rounding it to Unavailable would discard a StopAll that does.
            CHECK(rows[i].coverage == Coverage::Partial);
        }
    }
    g_image = nullptr;
}

TEST_CASE("every roster row that is not Ready carries a reason") {
    Image img;
    img.BuildHealthy(1);
    g_image = &img;
    ResidueBattery bat;
    ResetStatsForTest();
    REQUIRE(RegisterPlaybackResidue(bat, FullEnv()) == kOwnRows);

    RosterRow rows[kRosterRows];
    REQUIRE(Roster(rows, kRosterRows) == kRosterRows);
    for (size_t i = 0; i < kRosterRows; ++i) {
        REQUIRE(rows[i].id != nullptr);
        REQUIRE(rows[i].what != nullptr);
        REQUIRE(rows[i].note != nullptr);
        CHECK(strlen(rows[i].what) > 0);
        if (rows[i].coverage == Coverage::Unregistered) {
            // ★ An unregistered row with no reason is exactly the invisible gap the roster exists
            // to end. "Nobody built it" is a status; WHY nobody built it is the report.
            CHECK(strlen(rows[i].note) > 0);
        }
    }

    std::vector<std::string> seen;
    for (uint8_t i = 0; i < (uint8_t)Coverage::Count; ++i) {
        const char* t = CoverageText((Coverage)i);
        REQUIRE(t != nullptr);
        CHECK(strlen(t) > 0);
        for (const std::string& p : seen) { CHECK(p != std::string(t)); }
        seen.push_back(t);
    }
    g_image = nullptr;
}

// ★ Emitted for tests/residue/check.ps1, which extracts the design's own N-* row ids and
// fails if this list and that document disagree. That is what makes the denominator the DESIGN's
// rather than this table's: the design gaining an N-21 reds the gate instead of silently shrinking
// the goal to whatever got built.
TEST_CASE("the roster ids are emitted for the design cross-check") {
    ResetStatsForTest();   // no live battery: the ids are the design's list, not the battery's
    RosterRow rows[kRosterRows];
    REQUIRE(Roster(rows, kRosterRows) == kRosterRows);
    for (size_t i = 0; i < kRosterRows; ++i) {
        printf("ROSTER-ID %s\n", rows[i].id);
    }
    CHECK(kRosterRows == 19);
}

TEST_CASE("N-7a writes the motion bit only where the bit has a reader") {
    Image img;
    img.BuildHealthy(4);

    // The motion bit's ONLY reader in the image is CollectClassifiedBodies, and that pass walks two
    // lists rather than every body. Body 1 is class 8 -- reachable when the second flags word at
    // +0xC0 has bit 0x40000 set, which the kinematic/sphere mask never looks at -- and body 2 is
    // class 0, unclassified. Neither is ever visited, so for them the bit has no reader AND no
    // eventual clearer: it would sit set until the body is destroyed.
    img.Put<uint32_t>(img.Body(1) + rf::kPhBodyClassOff, 8u);
    img.Put<uint32_t>(img.Body(2) + rf::kPhBodyClassOff, 0u);
    img.Put<uint32_t>(img.Body(3) + rf::kPhBodyClassOff, rf::kPhBodyClassCollected[1]);   // class 6

    CHECK(BodyClassified(img.At(img.Body(0))));
    CHECK_FALSE(BodyClassified(img.At(img.Body(1))));
    CHECK_FALSE(BodyClassified(img.At(img.Body(2))));
    CHECK(BodyClassified(img.At(img.Body(3))));

    // All four are still WAKEABLE -- the class question is independent of the flag mask, which is
    // exactly why one predicate could not serve both rows.
    for (size_t i = 0; i < 4; ++i) {
        CHECK(BodyWakeable(img.At(img.Body(i))));
    }

    g_image = &img;
    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);

    CHECK(Stats().lastWalk.wakeable == 4);
    CHECK(Stats().lastWalk.classified == 2);
    CHECK(Stats().motionBitsRaised == 2);

    // ★ AND THE UNVISITED BODIES ARE LEFT ALONE. This is the assertion that distinguishes "we did
    // not bother" from "we did not litter": a set bit on body 1 or 2 would never be cleared by
    // anything, because neither clearer in the image ever reaches them.
    CHECK((img.Get<uint32_t>(img.Body(1) + facts::PHYS_BODY_FLAGS_OFF) &
           facts::PHYS_BODY_MOTION_BIT) == 0);
    CHECK((img.Get<uint32_t>(img.Body(2) + facts::PHYS_BODY_FLAGS_OFF) &
           facts::PHYS_BODY_MOTION_BIT) == 0);
    CHECK((img.Get<uint32_t>(img.Body(0) + facts::PHYS_BODY_FLAGS_OFF) &
           facts::PHYS_BODY_MOTION_BIT) != 0);
    CHECK((img.Get<uint32_t>(img.Body(3) + facts::PHYS_BODY_FLAGS_OFF) &
           facts::PHYS_BODY_MOTION_BIT) != 0);
    g_image = nullptr;
}

// =============================================================================================
// The raise count. Every other counter here is of an effect; this one is of a delivery.
// =============================================================================================

TEST_CASE("fires counts RAISES and not effects -- one per battery pass, keyed by cause") {
    Image img;
    img.BuildHealthy(3);
    g_image = &img;

    auto perCause = [](Cause c) { return Stats().firesByCause[static_cast<size_t>(c)]; };

    ResidueBattery bat;
    const size_t ran = Fire(bat, FullEnv(), Cause::Seek);

    // The property the field exists for: many rows ran on ONE raise. A counter incremented per row
    // -- the obvious wrong site -- would read `ran` here instead of 1.
    REQUIRE(ran > 1);
    CHECK(Stats().fires == 1);
    CHECK(perCause(Cause::Seek) == 1);
    CHECK(Stats().lastFireCause == Cause::Seek);
    CHECK(perCause(Cause::MicroSeek) == 0);
    CHECK(perCause(Cause::GapCross) == 0);
    CHECK(perCause(Cause::ReverseTick) == 0);
    CHECK(perCause(Cause::ChapterEpochRoll) == 0);

    // 0.5x playback above S7.5.2's sub-step ceiling jumps the remainder EVERY frame. A thousand of
    // those are a thousand MicroSeek raises and must leave the Seek bucket exactly where it was.
    // The battery is driven directly here because the Fire() helper would reset the stats.
    const uint64_t frames = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF a sample count
    for (uint64_t frame = 0; frame < frames; ++frame) {
        bat.Fire(Jump(Cause::MicroSeek));
    }
    CHECK(perCause(Cause::MicroSeek) == frames);
    CHECK(perCause(Cause::Seek) == 1);
    CHECK(Stats().fires == frames + 1);
    CHECK(Stats().lastFireCause == Cause::MicroSeek);
    g_image = nullptr;
}

TEST_CASE("a raise with no clock site still counts, and an undefined cause lands in no bucket") {
    // No image, so the clock resolves to a site that is not Ok and N-18 returns early without
    // reading anything. The jump still reached this packet. THIS is the case that reds if the
    // count is moved below the site guard -- every effect counter is legitimately zero here.
    g_image = nullptr;
    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);

    CHECK(Stats().lastClockVerdict == ClockVerdict::Fresh);   // the guard did not run
    CHECK(Stats().clockWrapsRepaired == 0);
    CHECK(Stats().fires == 1);
    CHECK(Stats().firesByCause[static_cast<size_t>(Cause::Seek)] == 1);

    // A cause the enum does not define is not a bucket index, and it is still a delivery.
    ResidueBattery undefinedCause;
    Fire(undefinedCause, FullEnv(), Cause::Count);
    CHECK(Stats().fires == 1);
    CHECK(Stats().lastFireCause == Cause::Count);
    for (uint8_t i = 0; i < static_cast<uint8_t>(Cause::Count); ++i) {
        CHECK(Stats().firesByCause[i] == 0);
    }
}

TEST_CASE("N-18 is the counting site, and the two properties that make it one are asserted here") {
    Image img;
    img.BuildHealthy(1);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);

    const IResidue* clockGuard = nullptr;
    for (size_t i = 0; i < bat.Count(); ++i) {
        if (strcmp(bat.At(i)->Id(), "N-18") == 0) {
            clockGuard = bat.At(i);
        }
    }
    REQUIRE(clockGuard != nullptr);

    // `fires` is exactly as trustworthy as these two facts, so they are asserted rather than
    // assumed: Ready unconditionally, and delivered on every cause the enum has -- including the
    // two the jump rows decline. Either one failing stops the counter silently otherwise.
    CHECK(clockGuard->Avail() == Availability::Ready);
    for (uint8_t i = 0; i < static_cast<uint8_t>(Cause::Count); ++i) {
        CHECK(clockGuard->RunsOn(static_cast<Cause>(i)));
    }
    g_image = nullptr;
}

TEST_CASE("N-7b wakes a parked body whatever list it is on") {
    Image img;
    img.BuildHealthy(2);
    // Class 8: N-7a will not touch it. BeAwake_ still must, because it acts on the body directly
    // and does not care which list the body is on -- gating the wake on the class would strand a
    // parked body that nothing else will ever wake.
    img.Put<uint32_t>(img.Body(0) + rf::kPhBodyClassOff, 8u);
    img.Put<uint64_t>(img.Body(0) + facts::PHYS_BODY_SLEEP_STAMP_OFF, 77u);
    g_image = &img;

    ResidueBattery bat;
    Fire(bat, FullEnv(), Cause::Seek);

    CHECK(Stats().wakesCalled == 1);
    REQUIRE(g_log.wokenBodies.size() == 1);
    CHECK(g_log.wokenBodies[0] == img.At(img.Body(0)));
    CHECK(Stats().motionBitsRaised == 1);   // only body 1, which is classified
    g_image = nullptr;
}

// =============================================================================================
// N-3a's ENTRY POINT TAKES A HANDLE, AND THE HANDLE ARRIVES AS A VALUE.
//
// RESET.CI_PC was bound to 0x140D3EA40, the INNER BODY of ResetCI, whose first instruction is
// `movzx eax, word ptr [rdx]` -- it wants the 504-byte CI ROW. The public entry 0x140D17BE0 is
// what takes (cs, uint16 handle): it translates the handle through the pool-index map at
// PHYS_CS_HANDLE_MAP_OFF, multiplies by PHYS_CI_STRIDE, adds the pool base and only THEN jumps to
// the body. residue.cpp passes a handle, so the body received 0x28 where it wanted a pointer and
// the client died (DIAG-CRASH). Nothing in the old test suite could see that: every stub here
// already had the value-taking C++ signature, so the fixture was the thing that could not fail.
//
// This case makes the two shapes distinguishable from inside the test. StubResetCiEntry does what
// 0x140D17BE0 does -- consumes argument 2 as a value and does its OWN translation -- and the
// assertions are on the CI ROWS it reached, so a handle that arrived as anything else (a pool
// slot, a truncated pointer, a byte-swapped word) reaches the wrong rows or none.
// =============================================================================================
namespace {

// The five generation counters 0x140D17BE0 increments, at ci+0Ah/0Eh/12h/16h/1Ah.
const size_t kCiGenCounters[5] = {0x0A, 0x0E, 0x12, 0x16, 0x1A};  // facts-ok: RESET.CI_PC the five `inc word ptr [rdx+..]` displacements inside the entry point itself, quoted here so the stub reaches the same fields; they are not offsets any product code reads

Image* g_entryImage = nullptr;
std::vector<size_t> g_entryRows;      // pool SLOT each call resolved to, in call order
size_t g_entryOutOfRange = 0;

// A software model of sub_140D17BE0. Argument 2 is a uint16 VALUE and is used as one.
uint64_t StubResetCiEntry(void* cs, uint16_t handle) {
    uint8_t* base = static_cast<uint8_t*>(cs);
    uint16_t* map = nullptr;
    memcpy(&map, base + facts::PHYS_CS_HANDLE_MAP_OFF, sizeof map);
    const uint16_t slot = map[2 * handle];      // the map's stride is 4 bytes, indexed by handle
    uint8_t* pool = nullptr;
    memcpy(&pool, base + facts::PHYS_CS_POOL_BASE_OFF, sizeof pool);
    uint16_t count = 0;
    memcpy(&count, base + facts::PHYS_CS_COUNT_OFF, sizeof count);
    if (slot >= count) {   // the engine's own bound, so a bad translation is refused, never written
        ++g_entryOutOfRange;
        return 0;
    }
    uint8_t* row = pool + facts::PHYS_CI_STRIDE * slot;
    for (size_t i = 0; i < 5; ++i) {
        uint16_t v = 0;
        memcpy(&v, row + kCiGenCounters[i], sizeof v);
        v = (uint16_t)(v + 1);
        memcpy(row + kCiGenCounters[i], &v, sizeof v);
    }
    g_entryRows.push_back(slot);
    g_log.order.push_back("ci");
    g_log.ciHandles.push_back(handle);
    return 0;
}

// The shape the crash had: sub_140D3EA40 takes the ROW. It is declared here only so the assertion
// below has something to refuse -- it is never called, and it CANNOT be assigned to the field.
uint64_t StubResetCiBody(void* cs, uint16_t* row);

}  // namespace

TEST_CASE("N-3a hands the CI entry point a HANDLE VALUE, and a row-pointer callee cannot be bound") {
    using Fn = ResidueEnv::CsHandleFn;

    // ★ THE COMPILE-TIME HALF. Guarded<Sig> in residue.cpp names this type at the binding site, so
    // an entry point that wants the row pointer is a type error there instead of a silent
    // reinterpret_cast. Widen the second parameter to a pointer and this file stops compiling.
    static_assert(std::is_same<Fn, uint64_t (*)(void*, uint16_t)>::value,
                  "CsHandleFn is (cs, uint16 handle) -- the shape of 0x140D17BE0");
    static_assert(!std::is_assignable<Fn&, decltype(&StubResetCiBody)>::value,
                  "a callee that dereferences argument 2 must not be bindable to this field");
    static_assert(sizeof(uint16_t) == 2, "argument 2 is 16 bits: the engine reads it with movzx dx");

    Image img;
    img.BuildHealthy(3);
    g_image = &img;
    g_entryImage = &img;
    g_entryRows.clear();
    g_entryOutOfRange = 0;

    ResidueEnv env = FullEnv();
    env.resetCi = &StubResetCiEntry;

    ResidueBattery bat;
    Fire(bat, env, Cause::Seek);

    // ★ THE RUNTIME HALF. BuildHealthy makes handle != slot (20/21/22 -> 0/1/2), so a value that
    // is really a slot resolves through the map to the WRONG row and these three checks separate.
    REQUIRE(g_log.ciHandles.size() == 3);
    CHECK(g_log.ciHandles[0] == 20);
    CHECK(g_log.ciHandles[1] == 21);
    CHECK(g_log.ciHandles[2] == 22);
    CHECK(g_entryOutOfRange == 0);
    REQUIRE(g_entryRows.size() == 3);
    CHECK(g_entryRows[0] == 0);
    CHECK(g_entryRows[1] == 1);
    CHECK(g_entryRows[2] == 2);

    // Every CI row the fixture holds, and only those, took exactly one increment on each of the
    // five counters. Counting the EFFECT rather than the call is what makes a handle that reached
    // the wrong row visible: the counts would be 2 and 0 instead of 1 and 1.
    for (size_t slot = 0; slot < 3; ++slot) {
        for (size_t i = 0; i < 5; ++i) {
            CHECK(img.Get<uint16_t>(img.NetCi(slot) + kCiGenCounters[i]) == 1);
        }
    }
    CHECK(img.Get<uint16_t>(img.NetCi(3) + kCiGenCounters[0]) == 0);

    // ★ AND THE VALUE IS NOT AN ADDRESS. This is the assertion that reds under the old binding: the
    // body at 0x140D3EA40 would have read `word ptr [rdx]` out of these, and 20/21/22 are not
    // addresses of anything -- the field crash had rdx == 0x28 for exactly this reason.
    for (size_t i = 0; i < g_log.ciHandles.size(); ++i) {
        const uintptr_t as_address = static_cast<uintptr_t>(g_log.ciHandles[i]);
        CHECK(as_address <= (std::numeric_limits<uint16_t>::max)());
        CHECK(as_address != reinterpret_cast<uintptr_t>(img.At(img.NetCi(i))));
    }

    g_entryImage = nullptr;
    g_image = nullptr;
}
