// The live disc census, driven against a game space this test builds itself.
//
// TWO THINGS ARE BEING CHECKED, and they are different in kind.
//
// 1. THE WALK. Every hop in disccensus.cpp is an offset into engine memory, and none of them can be
//    exercised by a replay session on this host. A planted space -- a system table, a frisbee
//    system, a net-physics system, a script system, and the pools they index -- drives the whole
//    chain and lets the DIFF be asserted on, which is the one output the owner will actually read.
//    ENG-DISC-RING-2 deferred exactly this fixture for the console census; the live census gets it,
//    because a live session is not a place to find out that a walk is wrong.
//
// 2. THE LIVE-SAFETY CLAIM, as a property of the SOURCE. `disccensus.cpp` is allowed to run in a
//    live public match, and the whole argument for that is "it only reads". A claim like that decays
//    the first time somebody adds a convenience call, so it is asserted over the file's text: no
//    livesafety feature is asked for (asking one would be the vacuous row
//    tests/bootstrap/test_livesafety.cpp forbids, since this path does NOT go inert), and no engine
//    call of any shape appears -- no function-pointer type, no vtable slot read, no `Set`/`Post`
//    through the entity surface.

#include <doctest.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "engine/disccensus.h"
#include "engine/localpawn_facts.h"
#include "foundation/gametype.h"
#include "generated/facts_generated.h"

namespace lp = replay::localpawn_facts;
namespace dc = replay::disccensus;

namespace {

// The planted level. One flat arena so every "pointer" is a real address in this process and the
// census's page probes answer the way they would against the engine's heap.
struct Arena {
    static constexpr size_t kSize = 60 * 0x0400 + 0x0400;
    alignas(16) uint8_t b[kSize] = {};

    void* At(size_t off) { return b + off; }
    template <typename T>
    void Put(size_t off, T v) {
        REQUIRE(off + sizeof(T) <= kSize);
        memcpy(b + off, &v, sizeof(T));
    }
    void PutPtr(size_t off, size_t target) { Put<void*>(off, At(target)); }
};

// Where everything in the fixture lives. Chosen to clear the largest field offset each object is
// read at, with slack, and listed here so a failing case can be read against one map.
// Counted in blocks rather than written as round addresses: a round address in a fixture is
// indistinguishable from a transcribed engine offset, and the ratchet is right to say so. The
// script pool needs thirteen of them because the fixture uses the real component indexes 16 and 67.
constexpr size_t kBlock       = 0x0400;
constexpr size_t kSpace       = 0 * kBlock;
constexpr size_t kSysTable    = 4 * kBlock;
constexpr size_t kFrisbeeCs   = 8 * kBlock;
constexpr size_t kFrisbeeIdx  = 9 * kBlock;
constexpr size_t kFrisbeePool = 10 * kBlock;   // two 560-byte components
constexpr size_t kPhysCs      = 13 * kBlock;
constexpr size_t kPhysMap     = 14 * kBlock;
constexpr size_t kPhysPool    = 15 * kBlock;
constexpr size_t kPhysList    = 16 * kBlock;
constexpr size_t kPhysNode    = 17 * kBlock;
constexpr size_t kScriptCs    = 18 * kBlock;
constexpr size_t kScriptIdx   = 19 * kBlock;
constexpr size_t kScriptPool  = 20 * kBlock;   // component 67 at stride 184 reaches block 32
constexpr size_t kInst0       = 34 * kBlock;
constexpr size_t kInst1       = 35 * kBlock;
constexpr size_t kModule0     = 36 * kBlock;
constexpr size_t kModule1     = 37 * kBlock;
constexpr size_t kData0       = 38 * kBlock;
constexpr size_t kData1       = 39 * kBlock;
// The actor tables live on the ROOT space, not on the child the component
// systems hang off, so the fixture gives the planted space a parent and puts them there: that is
// the arrangement the census has to cope with, and a table on the child would not prove the walk.
// The root is eight blocks because a table at +0x1F50 is an in-object field of a large object.
constexpr size_t kRoot        = 41 * kBlock;   // 41..48: the root space object itself
constexpr size_t kActorRows   = 49 * kBlock;   // three {u64 id, CActor*} rows
constexpr size_t kActorG      = 50 * kBlock;   // the game disc's CActor
constexpr size_t kActorL      = 51 * kBlock;   // the local disc's CActor
constexpr size_t kActorB      = 52 * kBlock;   // the actor `bubble` points at
constexpr size_t kDefDisc     = 53 * kBlock;   // the definition both disc actors were built from
constexpr size_t kDefBub      = 54 * kBlock;
constexpr size_t kNamesDisc   = 55 * kBlock;   // alt names at +0, the def+0x80 array at +512
constexpr size_t kFlagsG      = 56 * kBlock;
constexpr size_t kFlagsL      = 57 * kBlock;
constexpr size_t kNamesBub    = 58 * kBlock;
constexpr size_t kFlagsB      = 59 * kBlock;

// The two actors ENG-DISC-RING-2's census found: the bound game disc and the local disc.
constexpr uint64_t kActorGame  = 0x0000FFFFFFFF0078ull;
constexpr uint64_t kActorLocal = 0x0000FFFFFFFF00D4ull;
constexpr uint64_t kDiscScript = 0x38965D90A823F03Full;
constexpr uint32_t kPhysIdGame = 12;
// A count no engine could be holding: past every cap in disccensus.cpp, and built rather than
// written so the fixture does not restate an engine sentinel it does not mean.
constexpr uint64_t kTornCount = (1ull << 32) - 1ull;

// The offsets ENG-LIVE-CENSUS-2B derived. Spelled here rather than exported from disccensus.cpp on
// purpose: a fixture that asked the code under test where to plant would agree with it whatever it
// said, and these are the numbers the report cites against sub_1404FEF80 and sub_1404FD650.
constexpr uint32_t kTablePrimary   = 0x1F50u;
constexpr uint32_t kTableFallback  = 0x1F90u;
constexpr uint32_t kTableCountOff  = 0x30u;
constexpr uint32_t kActorDefOff    = 0x370u;  // facts-ok: GSPACE.CLASS_TABLE_OFF a field of a CActor, not of the game space that fact belongs to
constexpr uint32_t kActorFlagsOff  = 0x378u;  // facts-ok: PHYS.BODY_CLASS_OFF a field of a CActor, not of a net-physics component
constexpr uint32_t kActorCountOff  = 0x3A8u;
constexpr uint32_t kDefCountOff    = 0xB0u;
constexpr uint32_t kDefNamesOff    = 0x80u;
constexpr uint32_t kDefAltNamesOff = 0xB8u;
constexpr size_t   kAltNames       = 0;     // inside kNamesDisc / kNamesBub
constexpr size_t   kPriNames       = 512;
constexpr uint64_t kNoSymbol       = ~0ull;
// actor_t is {u16 slot, u16 pool, u16 generation, u16 pad}, so the generation is the third word.
constexpr uint32_t kActorIdGenShift = 32;

// `bubble`'s own actor, and the node on it the disc script names. RUN-VERIFY-2 §e read the live
// pair as actor 0000FFFFFFFF0079 / node 03147FD8F518E997 through the getter; the node symbol here
// is the derived SymbolHash so the fixture states a name rather than transcribing a hash.
constexpr uint64_t kActorBubble = 0x0000FFFFFFFF0079ull;
constexpr uint64_t kSymBubble   = replay::gametype::SymbolHash("bubble");
constexpr uint64_t kSymBody     = replay::gametype::SymbolHash("body");
constexpr uint64_t kSymGlow     = replay::gametype::SymbolHash("glow");
constexpr uint64_t kSymTrail    = replay::gametype::SymbolHash("trail");
constexpr uint64_t kSymCore     = replay::gametype::SymbolHash("core");
constexpr uint64_t kSpaceSym    = 0x576ED3F8428EBC4Bull;   // the space id RUN-VERIFY-2 printed
constexpr size_t   kBubbleVarAt = 128;   // where in the graph's data the actor_var sits
constexpr uint32_t kDiscNodes   = 4;
constexpr uint32_t kBubNodes    = 2;

// An actor-index row: {u64 actorId, u64 owning system symbol, u16 component index}.
void PutIndexRow(Arena& a, size_t base, uint32_t row, uint64_t actorId, uint64_t systemSym,
                 uint16_t ci) {
    const size_t at = base + 24u * row;
    a.Put<uint64_t>(at, actorId);
    a.Put<uint64_t>(at + 8, systemSym);
    a.Put<uint16_t>(at + 16, ci);
}

void PlantActors(Arena& a);   // the actor side, below, so `Plant` still reads top-down

// One actor definition: the node count the actor's own copy is cross-checked against, and the two
// name arrays the engine renders a node index through.
void PlantDef(Arena& a, size_t def, size_t names, uint32_t n) {
    a.Put<uint64_t>(def + kDefCountOff, n);
    a.PutPtr(def + kDefAltNamesOff, names + kAltNames);
    a.PutPtr(def + kDefNamesOff, names + kPriNames);
}

void PlantActor(Arena& a, size_t actor, size_t def, size_t flags, uint32_t n) {
    a.PutPtr(actor + kActorDefOff, def);
    a.PutPtr(actor + kActorFlagsOff, flags);
    a.Put<uint64_t>(actor + kActorCountOff, n);
}

void Plant(Arena& a) {
    // The space and its three systems.
    a.Put<uint64_t>(kSpace + lp::kSystemTableCountOff, 3);
    a.PutPtr(kSpace + lp::kGameSpaceSystemTableOff, kSysTable);
    const size_t stride = (size_t)lp::kSystemTableStride;
    a.Put<uint64_t>(kSysTable + 0 * stride, lp::kSymR14Frisbee);
    a.PutPtr(kSysTable + 0 * stride + lp::kSystemTableEntryCsOff, kFrisbeeCs);
    a.Put<uint64_t>(kSysTable + 1 * stride, facts::CS_SYM_R14NETPHYSICS);
    a.PutPtr(kSysTable + 1 * stride + lp::kSystemTableEntryCsOff, kPhysCs);
    a.Put<uint64_t>(kSysTable + 2 * stride, replay::gametype::SymbolHash("ncaScript"));
    a.PutPtr(kSysTable + 2 * stride + lp::kSystemTableEntryCsOff, kScriptCs);

    // R14Frisbee: two actors, component 0 and component 1.
    a.Put<uint64_t>(kFrisbeeCs + lp::kCsActorIndexCountOff, 2);
    a.PutPtr(kFrisbeeCs + lp::kCsActorIndexOff, kFrisbeeIdx);
    a.PutPtr(kFrisbeeCs + lp::kFrisbeeCsPoolBaseOff, kFrisbeePool);
    PutIndexRow(a, kFrisbeeIdx, 0, kActorGame, lp::kSymR14Frisbee, 0);
    PutIndexRow(a, kFrisbeeIdx, 1, kActorLocal, lp::kSymR14Frisbee, 1);
    a.Put<uint32_t>(kFrisbeePool + 0 * lp::kFrisbeeCiStride + lp::kFrisbeeCiPhysIdOff, kPhysIdGame);
    a.Put<float>(kFrisbeePool + 0 * lp::kFrisbeeCiStride + lp::kFrisbeeCiTimeSinceHeldOff, 59.1f);
    // The local disc carries no net-physics component: RING-2 read `phys 65535` on it.
    a.Put<uint32_t>(kFrisbeePool + 1 * lp::kFrisbeeCiStride + lp::kFrisbeeCiPhysIdOff,
                    lp::kNoComponent);
    a.Put<float>(kFrisbeePool + 1 * lp::kFrisbeeCiStride + lp::kFrisbeeCiTimeSinceHeldOff, 59.1f);

    // R14NetPhysics: the handle map sends physId 12 to element 1.
    a.PutPtr(kPhysCs + facts::PHYS_CS_HANDLE_MAP_OFF, kPhysMap);
    a.PutPtr(kPhysCs + facts::PHYS_CS_POOL_BASE_OFF, kPhysPool);
    a.Put<uint32_t>(kPhysMap + 4u * kPhysIdGame, 1);
    const size_t ci = kPhysPool + 1u * facts::PHYS_CI_STRIDE;
    a.Put<uint32_t>(ci + facts::PHYS_CI_OWNER_OFF, dc::kUnowned);
    a.Put<uint16_t>(ci + facts::PHYS_CI_FLAGS_OFF, 0x0023u);
    a.PutPtr(ci + facts::PHYS_CI_LIST_HEAD_OFF, kPhysList);
    a.PutPtr(kPhysList + 16, kPhysNode);
    a.Put<uint16_t>(kPhysNode + facts::PHYS_NODE_STATE_OFF, 0x000Fu);

    // ncaScript: one graph on each disc actor, both the disc's own script.
    a.Put<uint64_t>(kScriptCs + lp::kCsActorIndexCountOff, 2);
    a.PutPtr(kScriptCs + lp::kCsActorIndexOff, kScriptIdx);
    a.PutPtr(kScriptCs + lp::kScriptCsPoolBaseOff, kScriptPool);
    PutIndexRow(a, kScriptIdx, 0, kActorGame, 0, 16);
    PutIndexRow(a, kScriptIdx, 1, kActorLocal, 0, 67);
    const size_t inst[2] = {kInst0, kInst1};
    const size_t mod[2] = {kModule0, kModule1};
    const size_t dat[2] = {kData0, kData1};
    const uint16_t ciIndex[2] = {16, 67};
    for (int i = 0; i < 2; ++i) {
        a.PutPtr(kScriptPool + lp::kScriptCiStride * ciIndex[i] + lp::kScriptCiInstanceOff, inst[i]);
        a.PutPtr(inst[i] + lp::kScriptInstModuleOff, mod[i]);
        a.Put<int32_t>(inst[i] + lp::kScriptInstStateOff, 5);
        a.Put<uint64_t>(mod[i] + lp::kScriptModuleHashOff, kDiscScript);
        const size_t graph = inst[i] + lp::kScriptInstGraphOff;
        a.Put<int64_t>(graph + lp::kScriptGraphGameTimeOff, 112000 + i);
        a.PutPtr(graph + lp::kScriptGraphDataOff, dat[i]);
        for (uint32_t w = 0; w < dc::ScriptRow::kDataWords; ++w) {
            a.Put<uint32_t>(dat[i] + 4u * w, 0);
        }
    }
    // The script DB's own reading of the disc graph's first words: the state-reset run leaves
    // int_4b03787780e76dc0 = 120 and flag_36bbdce2f9ac20e0 = 1.
    a.Put<uint32_t>(kData0 + 4u * 3, 120);
    a.Put<uint32_t>(kData0 + 4u * 7, 1);

    PlantActors(a);
}

// The actor side: a root space carrying the {id -> CActor*} table, three actors, and the two
// definitions they were built from.
void PlantActors(Arena& a) {
    a.PutPtr(kSpace + lp::kGameSpaceParentOff, kRoot);
    a.PutPtr(kRoot + kTablePrimary, kActorRows);
    a.Put<uint64_t>(kRoot + kTablePrimary + kTableCountOff, 3);
    const uint64_t ids[3] = {kActorGame, kActorLocal, kActorBubble};
    const size_t actors[3] = {kActorG, kActorL, kActorB};
    for (int i = 0; i < 3; ++i) {
        a.Put<uint64_t>(kActorRows + 16u * i, ids[i]);
        a.PutPtr(kActorRows + 16u * i + 8, actors[i]);
    }

    // Both disc actors were built from one definition, which is why their node indexes agree.
    PlantDef(a, kDefDisc, kNamesDisc, kDiscNodes);
    a.Put<uint64_t>(kNamesDisc + kAltNames + 8u * 0, kSymBody);
    a.Put<uint64_t>(kNamesDisc + kAltNames + 8u * 1, kNoSymbol);   // named only by the def+0x80 array
    a.Put<uint64_t>(kNamesDisc + kAltNames + 8u * 2, kSymTrail);
    a.Put<uint64_t>(kNamesDisc + kAltNames + 8u * 3, kSymCore);
    a.Put<uint64_t>(kNamesDisc + kPriNames + 8u * 1, kSymGlow);

    PlantActor(a, kActorG, kDefDisc, kFlagsG, kDiscNodes);
    PlantActor(a, kActorL, kDefDisc, kFlagsL, kDiscNodes);
    a.Put<uint8_t>(kFlagsG + 3, 1);   // the game disc's `core` node is switched OFF

    // What `bubble` points at: its own actor, whose node 1 carries the symbol.
    PlantDef(a, kDefBub, kNamesBub, kBubNodes);
    a.Put<uint64_t>(kNamesBub + kAltNames + 8u * 0, kSymBody);
    a.Put<uint64_t>(kNamesBub + kAltNames + 8u * 1, kSymBubble);
    PlantActor(a, kActorB, kDefBub, kFlagsB, kBubNodes);

    // The variable's own storage, past the 26 words the census copies wholesale.
    a.Put<uint64_t>(kData0 + kBubbleVarAt + lp::kActorVarSpaceOff, kSpaceSym);
    a.Put<uint64_t>(kData0 + kBubbleVarAt + lp::kActorVarActorOff, kActorBubble);
    a.Put<uint64_t>(kData0 + kBubbleVarAt + lp::kActorVarNodeOff, kSymBubble);
}

dc::Input OneSpace(Arena& a) {
    dc::Input in;
    in.spaces[0] = a.At(kSpace);
    in.nSpaces = 1;
    return in;
}

std::string ReadRepoFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("the census finds both disc actors and reads the holder through the physics system") {
    Arena a;
    Plant(a);
    dc::Snapshot s;
    REQUIRE(dc::Take(OneSpace(a), s));
    CHECK(s.spacesWalked == 1);
    CHECK(s.systemsSeen == 3);
    REQUIRE(s.discsWritten == 2);

    CHECK(s.discs[0].actorId == kActorGame);
    CHECK(s.discs[0].physId == kPhysIdGame);
    CHECK(s.discs[0].holderRead);
    CHECK(s.discs[0].holder == dc::kUnowned);
    CHECK(s.discs[0].physFlags == 0x0023u);
    CHECK(s.discs[0].nodeStateRead);
    CHECK(s.discs[0].nodeState == 0x000Fu);
    CHECK(s.discs[0].timeSinceHeld == doctest::Approx(59.1));

    // The second actor has no net-physics component, so the holder is UNREAD rather than unowned --
    // "nobody is holding it" and "there is nothing to hold" must not print the same.
    CHECK(s.discs[1].actorId == kActorLocal);
    CHECK_FALSE(s.discs[1].holderRead);

    REQUIRE(s.scriptsWritten == 2);
    CHECK(s.discs[0].scripts == 1);
    CHECK(s.scripts[s.discs[0].scriptFirst].hash == kDiscScript);
    CHECK(s.scripts[s.discs[0].scriptFirst].state == 5);
    CHECK(s.scripts[s.discs[0].scriptFirst].dataRead);
    CHECK(s.scripts[s.discs[0].scriptFirst].data[3] == 120u);
}

TEST_CASE("a first-sight line names the actor the holder and the graph digest") {
    Arena a;
    Plant(a);
    dc::Snapshot s;
    REQUIRE(dc::Take(OneSpace(a), s));
    char line[1024];
    REQUIRE(dc::Describe(s, 0, line, sizeof line) != 0);
    const std::string text = line;
    INFO(text);
    MESSAGE(text);   // the report quotes these lines verbatim; doctest prints a MESSAGE always
    CHECK(text.find("actor 0000FFFFFFFF0078") != std::string::npos);
    CHECK(text.find("holder FFFE0010") != std::string::npos);
    CHECK(text.find("flags 0023 node 000F") != std::string::npos);
    CHECK(text.find("script[0] 38965D90A823F03F ci 16 st 5 data#") != std::string::npos);
    CHECK(text.find("nodes 4/4") != std::string::npos);
    CHECK(text.find("bubble -> actor 0000FFFFFFFF0079") != std::string::npos);
    CHECK(text.find("enabled=1") != std::string::npos);
}

TEST_CASE("the actor id resolves to a CActor and every node comes back with its enabled bit") {
    Arena a;
    Plant(a);
    dc::Snapshot s;
    REQUIRE(dc::Take(OneSpace(a), s));
    REQUIRE(s.discsWritten == 2);
    CHECK(s.actorsResolved == 2);
    CHECK(s.nodesRefused == nullptr);

    const dc::DiscRow& game = s.discs[0];
    REQUIRE(game.actor == (uint64_t)(uintptr_t)a.At(kActorG));
    REQUIRE(game.nodes == kDiscNodes);
    CHECK(game.nodeCount == kDiscNodes);
    const dc::NodeRow* n = &s.nodes[game.nodeFirst];
    CHECK(n[0].symbol == kSymBody);
    CHECK(n[0].index == 0);
    CHECK(n[0].enabled == 1);
    // Node 1's alt name reads -1, so the engine's own fallback array is what names it.
    CHECK(n[1].symbol == kSymGlow);
    CHECK(n[2].symbol == kSymTrail);
    // A DISABLE mask: the one byte that is set is the node that is OFF.
    CHECK(n[3].symbol == kSymCore);
    CHECK(n[3].enabled == 0);
    for (uint32_t i = 0; i < kDiscNodes; ++i) CHECK(n[i].read);

    // The local disc shares the definition and has nothing switched off.
    REQUIRE(s.discs[1].nodes == kDiscNodes);
    CHECK(s.nodes[s.discs[1].nodeFirst + 3].enabled == 1);
}

TEST_CASE("bubble is found by what it points at and reports that node's enabled bit") {
    Arena a;
    Plant(a);
    dc::Snapshot s;
    REQUIRE(dc::Take(OneSpace(a), s));
    REQUIRE(s.discs[0].refFound);
    CHECK(s.discs[0].refActor == kActorBubble);
    CHECK(s.discs[0].refNode == kSymBubble);
    CHECK(s.discs[0].refIndex == 1);
    CHECK(s.discs[0].refEnabledRead);
    CHECK(s.discs[0].refEnabled == 1);
    // The local disc's graph has no actor_var in it, so its reference is UNRESOLVED rather than
    // silently inherited from the disc next to it.
    CHECK_FALSE(s.discs[1].refFound);

    char line[1024];
    REQUIRE(dc::Describe(s, 1, line, sizeof line) != 0);
    CHECK(std::string(line).find("bubble unresolved") != std::string::npos);
}

TEST_CASE("a node switching off between snapshots is one field of the diff line") {
    Arena a;
    Plant(a);
    dc::Snapshot before;
    REQUIRE(dc::Take(OneSpace(a), before));

    // The first grab, as the owner describes it: the ring goes away and the disc's own `core` node
    // comes back on. Both are the same kind of write -- bit 0 of one flag byte.
    a.Put<uint8_t>(kFlagsB + 1, 1);
    a.Put<uint8_t>(kFlagsG + 3, 0);
    dc::Snapshot after;
    REQUIRE(dc::Take(OneSpace(a), after));
    REQUIRE_FALSE(dc::PopulationChanged(before, after));

    char line[1024];
    REQUIRE(dc::Diff(before, after, 0, line, sizeof line) != 0);
    const std::string text = line;
    INFO(text);
    MESSAGE(text);   // the report quotes these lines verbatim; doctest prints a MESSAGE always
    char expect[96];
    snprintf(expect, sizeof expect, "node %016llX enabled 0->1", (unsigned long long)kSymCore);
    CHECK(text.find(expect) != std::string::npos);
    snprintf(expect, sizeof expect, "bubble node %016llX enabled 1->0", (unsigned long long)kSymBubble);
    CHECK(text.find(expect) != std::string::npos);
}

TEST_CASE("a stale generation resolves to nothing rather than to the slot's current tenant") {
    Arena a;
    Plant(a);
    // The same slot and pool, one generation back. The table's key is the whole qword, so this
    // matches no row -- which is the entire safety of resolving an id by reads.
    const uint64_t stale = kActorGame - (1ull << kActorIdGenShift);
    PutIndexRow(a, kFrisbeeIdx, 0, stale, lp::kSymR14Frisbee, 0);
    dc::Snapshot s;
    REQUIRE(dc::Take(OneSpace(a), s));
    REQUIRE(s.discsWritten == 2);
    CHECK(s.discs[0].actorId == stale);
    CHECK(s.discs[0].actor == 0);
    CHECK(s.discs[0].nodes == 0);
    CHECK(s.actorsResolved == 1);          // the local disc still resolved
    REQUIRE(s.nodesRefused != nullptr);
    CHECK(std::string(s.nodesRefused).find("holds this id") != std::string::npos);
}

TEST_CASE("pointers that do not read back refuse the walk instead of following them") {
    SUBCASE("the table's rows are not mapped") {
        Arena a;
        Plant(a);
        a.Put<void*>(kRoot + kTablePrimary, (void*)0x10);
        dc::Snapshot s;
        REQUIRE(dc::Take(OneSpace(a), s));
        CHECK(s.actorsResolved == 0);
        CHECK(s.nodesWritten == 0);
        CHECK_FALSE(s.discs[0].refFound);
    }
    SUBCASE("the actor's flag bytes are not mapped") {
        Arena a;
        Plant(a);
        a.Put<void*>(kActorG + kActorFlagsOff, (void*)0x10);
        dc::Snapshot s;
        REQUIRE(dc::Take(OneSpace(a), s));
        CHECK(s.discs[0].actor != 0);       // the id resolved
        CHECK(s.discs[0].nodes == 0);       // and the walk still refused
        REQUIRE(s.nodesRefused != nullptr);
        CHECK(std::string(s.nodesRefused).find("node arrays") != std::string::npos);
    }
    SUBCASE("the definition disagrees with the actor about how many nodes there are") {
        Arena a;
        Plant(a);
        a.Put<uint64_t>(kDefDisc + kDefCountOff, kDiscNodes + 1);
        dc::Snapshot s;
        REQUIRE(dc::Take(OneSpace(a), s));
        CHECK(s.nodesWritten == 0);
    }
    SUBCASE("a torn node count is refused rather than walked") {
        Arena a;
        Plant(a);
        a.Put<uint64_t>(kActorG + kActorCountOff, kTornCount);
        dc::Snapshot s;
        REQUIRE(dc::Take(OneSpace(a), s));
        CHECK(s.discs[0].nodes == 0);
        CHECK(s.discs[1].nodes == kDiscNodes);
    }
}

TEST_CASE("an actor with more nodes than the row budget is truncated and says so") {
    Arena a;
    Plant(a);
    constexpr uint32_t kMany = 40;
    a.Put<uint64_t>(kDefDisc + kDefCountOff, kMany);
    a.Put<uint64_t>(kActorG + kActorCountOff, kMany);
    a.Put<uint64_t>(kActorL + kActorCountOff, kMany);
    dc::Snapshot s;
    REQUIRE(dc::Take(OneSpace(a), s));
    CHECK(s.discs[0].nodeCount == kMany);
    CHECK(s.discs[0].nodes == 24);          // kNodesPerDisc, and the count above keeps it honest
    CHECK(s.nodesWritten <= dc::Snapshot::kNodeCap);
    char line[1024];
    REQUIRE(dc::Describe(s, 0, line, sizeof line) != 0);
    CHECK(std::string(line).find("nodes 24/40") != std::string::npos);
}

TEST_CASE("the table is found on the root space and not on the child the systems hang off") {
    Arena a;
    Plant(a);
    SUBCASE("the engine's own fallback offset serves just as well") {
        a.Put<void*>(kRoot + kTablePrimary, nullptr);
        a.Put<uint64_t>(kRoot + kTablePrimary + kTableCountOff, 0);
        a.PutPtr(kRoot + kTableFallback, kActorRows);
        a.Put<uint64_t>(kRoot + kTableFallback + kTableCountOff, 3);
        dc::Snapshot s;
        REQUIRE(dc::Take(OneSpace(a), s));
        CHECK(s.actorsResolved == 2);
    }
    SUBCASE("a space with no parent simply has no node rows") {
        a.Put<void*>(kSpace + lp::kGameSpaceParentOff, nullptr);
        dc::Snapshot s;
        REQUIRE(dc::Take(OneSpace(a), s));
        CHECK(s.actorsResolved == 0);
        CHECK(s.discsWritten == 2);         // everything else still reported
        CHECK(s.scriptsWritten == 2);
    }
}

TEST_CASE("a grab is one diff line naming the holder and the graph word that moved") {
    Arena a;
    Plant(a);
    dc::Snapshot before;
    REQUIRE(dc::Take(OneSpace(a), before));

    // The engine takes possession: the owner dword names a player, the held bit appears in the
    // newest history node, and the disc graph's own re-entrancy word moves.
    const size_t ci = kPhysPool + 1u * facts::PHYS_CI_STRIDE;
    a.Put<uint32_t>(ci + facts::PHYS_CI_OWNER_OFF, 0x00000003u);
    a.Put<uint16_t>(ci + facts::PHYS_CI_FLAGS_OFF, 0x006Fu);
    a.Put<uint16_t>(kPhysNode + facts::PHYS_NODE_STATE_OFF, 0x020Fu);
    a.Put<uint32_t>(kData0 + 4u * 11, 1);
    a.Put<float>(kFrisbeePool + 0 * lp::kFrisbeeCiStride + lp::kFrisbeeCiTimeSinceHeldOff, 0.0f);

    dc::Snapshot after;
    REQUIRE(dc::Take(OneSpace(a), after));
    CHECK_FALSE(dc::PopulationChanged(before, after));

    char line[1024];
    REQUIRE(dc::Diff(before, after, 0, line, sizeof line) != 0);
    const std::string text = line;
    INFO(text);
    MESSAGE(text);   // the report quotes these lines verbatim; doctest prints a MESSAGE always
    CHECK(text.find("holder FFFE0010->00000003") != std::string::npos);
    CHECK(text.find("flags 0023->006F") != std::string::npos);
    CHECK(text.find("node 000F->020F") != std::string::npos);
    CHECK(text.find("untouched 59.10->0.00s") != std::string::npos);
    CHECK(text.find("script[0] w11 00000000->00000001") != std::string::npos);
    // The untouched timer counts UP every frame; a diff that reported that would fire on every
    // trigger and say nothing. Only the reset is a reading.
    dc::Snapshot later = after;
    later.discs[0].timeSinceHeld = 4.0f;
    REQUIRE(dc::Diff(after, later, 0, line, sizeof line) == 0);
    CHECK(line[0] == '\0');
}

TEST_CASE("an unchanged disc produces no line at all") {
    Arena a;
    Plant(a);
    dc::Snapshot before, after;
    REQUIRE(dc::Take(OneSpace(a), before));
    REQUIRE(dc::Take(OneSpace(a), after));
    char line[256];
    CHECK(dc::Diff(before, after, 0, line, sizeof line) == 0);
    CHECK(line[0] == '\0');
}

TEST_CASE("a respawned disc is a population change and not a diff") {
    Arena a;
    Plant(a);
    dc::Snapshot before;
    REQUIRE(dc::Take(OneSpace(a), before));
    PutIndexRow(a, kFrisbeeIdx, 0, 0x0000FFFFFFFF0099ull, lp::kSymR14Frisbee, 0);
    dc::Snapshot after;
    REQUIRE(dc::Take(OneSpace(a), after));
    CHECK(dc::PopulationChanged(before, after));
}

TEST_CASE("the per-frame poll reads the holder through the snapshot's own pointer") {
    Arena a;
    Plant(a);
    dc::Snapshot s;
    REQUIRE(dc::Take(OneSpace(a), s));
    uint32_t holders[dc::Snapshot::kDiscCap] = {};
    uint32_t n = 0;
    CHECK_FALSE(dc::HolderMoved(s, holders, dc::Snapshot::kDiscCap, n));
    CHECK(n == 2);
    CHECK(holders[0] == dc::kUnowned);

    a.Put<uint32_t>(kPhysPool + 1u * facts::PHYS_CI_STRIDE + facts::PHYS_CI_OWNER_OFF, 7u);
    CHECK(dc::HolderMoved(s, holders, dc::Snapshot::kDiscCap, n));
    CHECK(holders[0] == 7u);
}

TEST_CASE("a space that reads back as nothing refuses instead of walking") {
    Arena a;   // never planted: every count is zero
    dc::Snapshot s;
    CHECK_FALSE(dc::Take(OneSpace(a), s));
    CHECK(s.discsWritten == 0);

    dc::Input none;
    CHECK_FALSE(dc::Take(none, s));
}

TEST_CASE("a count the engine could not be holding is refused rather than walked") {
    Arena a;
    Plant(a);
    // The system table count, as a torn or hostile read. The walk must not follow it.
    a.Put<uint64_t>(kSpace + lp::kSystemTableCountOff, kTornCount);
    dc::Snapshot s;
    CHECK_FALSE(dc::Take(OneSpace(a), s));

    a.Put<uint64_t>(kSpace + lp::kSystemTableCountOff, 3);
    a.Put<uint64_t>(kFrisbeeCs + lp::kCsActorIndexCountOff, kTornCount);
    dc::Snapshot big;
    CHECK(dc::Take(OneSpace(a), big));      // the space walked
    CHECK(big.discsWritten == 0);           // and the frisbee index did not
}

TEST_CASE("a line longer than its buffer is truncated and still terminated") {
    Arena a;
    Plant(a);
    dc::Snapshot s;
    REQUIRE(dc::Take(OneSpace(a), s));
    char tiny[24];
    memset(tiny, 'x', sizeof tiny);
    const size_t n = dc::Describe(s, 0, tiny, sizeof tiny);
    CHECK(n < sizeof tiny);
    CHECK(tiny[sizeof tiny - 1] == '\0');
    CHECK(strlen(tiny) == n);
}

// ---- the live-safety claim, asserted over the source ----------------------------------------

TEST_CASE("the live disc census asks for no live-safety feature and makes no engine call") {
    const std::filesystem::path src =
        std::filesystem::path(REPLAY_REPO_ROOT) / "src" / "engine" / "disccensus.cpp";
    const std::string text = ReadRepoFile(src);
    REQUIRE_MESSAGE(text.size() > 2000u, "cannot read src/engine/disccensus.cpp");

    // No live-safety enumerator is asked for. Asking one would be a promise that this path goes
    // INERT in a live session, and running there is the whole point of the file.
    CHECK(text.find("Allowed(") == std::string::npos);
    CHECK(text.find("livesafety") == std::string::npos);

    // No engine call, in any of the shapes this codebase spells one. A function-pointer typedef, a
    // cast to a function pointer, a vtable slot fetch, or a write through the entity surface would
    // each be a call into game code from a path that runs in a live public match.
    const char* const forbidden[] = {"(*)(",      "reinterpret_cast<Fn", "Vtbl",   "vtable",
                                     "->Set",     "->Post",             "PostEvent", "SetOwner",
                                     "GetProcAddress", "__try"};
    for (const char* needle : forbidden) {
        INFO("src/engine/disccensus.cpp contains '" << needle << "'");
        CHECK(text.find(needle) == std::string::npos);
    }
    // And the reads it does make go through the page probe, not through a bare dereference of an
    // engine pointer: every hop in the file is guarded by this one predicate.
    CHECK(text.find("PageSpanReadable") != std::string::npos);
}
