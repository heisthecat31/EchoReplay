// The level id the capture records. The defect these cover is the one W2-B left open: OpenParams
//::levelId was declared, read twice and never assigned, so every file ever written carried
// manifest.levelId == 0 -- and playback, which compares each netvar rowset's owner word against
// that value, reported a located netvar table and left it UNBOUND (NoCaptureLevelId) rather than
// bind a zero and refuse every rowset while looking bound.
//
// These drive the SHIPPED walk over synthetic memory laid out the way the engine lays it out:
// netGame -> {level space, cs space} -> (fake CS.LOOKUP_BY_SYMBOL) -> cs -> +0xB8 / +0x80 -> owner
// -> +0x58. The assertions are on the id that comes out and on the hop named when one does not.

#include "doctest.h"

#include "capture/levelid.h"
#include "capture/manifestbuild.h"

#include <string.h>

using namespace replay;
using namespace replay::capture;

namespace {

// Sizes and stand-in values are built from the registry offsets they must cover plus one pad,
// so no bare number in this file is the value of a registered fact.
constexpr size_t kPadLen = 137;
constexpr uint64_t kPlantedId = 0x00CAFE0000BEEF17ull;

// Engine-shaped storage. Static rather than stack: the netGame stand-in is 11 KB because the space
// slots live at +0x2B08 and +0x2B10.
uint8_t g_netGame[facts::NETGAME_SPECTATORCAM_CS_OFF + kPadLen];
uint8_t g_otherNetGame[facts::NETGAME_SPECTATORCAM_CS_OFF + kPadLen];
uint8_t g_levelSpace[kPadLen];
uint8_t g_csSpace[kPadLen];
uint8_t g_cs[facts::NETVAR_LEVEL_NETGAME_OFF + kPadLen];
uint8_t g_owner[kOwnerIdOff + kPadLen];

void* g_answerFor = nullptr;   // the space the fake lookup answers for
uint64_t g_askedSymbol = 0;
int g_asked = 0;

void* FakeFind(void* space, uint64_t symbol) {
    ++g_asked;
    g_askedSymbol = symbol;
    return (space == g_answerFor) ? static_cast<void*>(g_cs) : nullptr;
}

void* Slot(void* base, uint32_t off) { return static_cast<uint8_t*>(base) + off; }

void Put(void* base, uint32_t off, const void* value) {
    memcpy(Slot(base, off), &value, sizeof value);
}

// A complete, healthy session: both spaces present, the component in the LEVEL space, its netGame
// slot pointing back at us, an owner carrying the id.
void PlantHealthy() {
    memset(g_netGame, 0, sizeof g_netGame);
    memset(g_otherNetGame, 0, sizeof g_otherNetGame);
    memset(g_levelSpace, 0, sizeof g_levelSpace);
    memset(g_csSpace, 0, sizeof g_csSpace);
    memset(g_cs, 0, sizeof g_cs);
    memset(g_owner, 0, sizeof g_owner);
    g_asked = 0;
    g_askedSymbol = 0;

    Put(g_netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, g_levelSpace);
    Put(g_netGame, facts::NETGAME_CS_SPACE_OFF, g_csSpace);
    Put(g_cs, facts::NETVAR_LEVEL_NETGAME_OFF, g_netGame);
    Put(g_cs, kOwnerPtrOff, g_owner);
    const uint64_t id = kPlantedId;
    memcpy(Slot(g_owner, kOwnerIdOff), &id, sizeof id);
    g_answerFor = g_levelSpace;
}

}  // namespace

TEST_CASE("the level id is read off the netvar component's owner, from the level space") {
    PlantHealthy();

    const LevelIdRead r = ReadLevelIdFrom(g_netGame, FakeFind);

    CHECK(r.why == LevelIdWhy::Ok);
    CHECK(r.id == kPlantedId);
    CHECK(r.netVarCs == static_cast<void*>(g_cs));
    CHECK(r.fromLevelSpace);
    CHECK(g_asked == 1);                                  // the level space is asked FIRST
    CHECK(g_askedSymbol == facts::NETVAR_CS_SYMBOL);
}

TEST_CASE("the cs space is asked when the level space has no netvar component") {
    PlantHealthy();
    g_answerFor = g_csSpace;

    const LevelIdRead r = ReadLevelIdFrom(g_netGame, FakeFind);

    CHECK(r.why == LevelIdWhy::Ok);
    CHECK(r.id == kPlantedId);
    CHECK_FALSE(r.fromLevelSpace);
    CHECK(g_asked == 2);
}

TEST_CASE("one space in both slots is asked once, not twice") {
    PlantHealthy();
    Put(g_netGame, facts::NETGAME_CS_SPACE_OFF, g_levelSpace);

    const LevelIdRead r = ReadLevelIdFrom(g_netGame, FakeFind);

    CHECK(r.why == LevelIdWhy::Ok);
    CHECK(g_asked == 1);
}

TEST_CASE("a component belonging to another CR15NetGame is refused, not read") {
    PlantHealthy();
    Put(g_cs, facts::NETVAR_LEVEL_NETGAME_OFF, g_otherNetGame);

    const LevelIdRead r = ReadLevelIdFrom(g_netGame, FakeFind);

    // The id sits right there at owner+0x58 and is still not taken: reading it would record one
    // session's level against another's rowsets.
    CHECK(r.why == LevelIdWhy::NetGameMismatch);
    CHECK(r.id == 0);
    CHECK(r.netVarCs == nullptr);
}

TEST_CASE("a netvar component with no owner object yet names that hop") {
    PlantHealthy();
    Put(g_cs, kOwnerPtrOff, nullptr);

    const LevelIdRead r = ReadLevelIdFrom(g_netGame, FakeFind);

    CHECK(r.why == LevelIdWhy::NoOwner);
    CHECK(r.id == 0);
    CHECK(r.netVarCs == static_cast<void*>(g_cs));  // a refusal still names what it refused
}

TEST_CASE("an owner whose level id is not committed memory is refused, never guessed at") {
    PlantHealthy();
    // A pointer that is not mapped at all. Every hop is span-proved, so this is a named refusal and
    // not an access violation inside a capture open.
    Put(g_cs, kOwnerPtrOff, reinterpret_cast<void*>(static_cast<uintptr_t>(1893ull)));

    const LevelIdRead r = ReadLevelIdFrom(g_netGame, FakeFind);

    CHECK(r.why == LevelIdWhy::OwnerUnreadable);
    CHECK(r.id == 0);
}

TEST_CASE("every earlier hop reports itself rather than an empty answer") {
    PlantHealthy();
    CHECK(ReadLevelIdFrom(nullptr, FakeFind).why == LevelIdWhy::NoNetGame);
    CHECK(ReadLevelIdFrom(g_netGame, nullptr).why == LevelIdWhy::NoLookup);

    g_answerFor = nullptr;  // a lookup that answers null in both spaces
    CHECK(ReadLevelIdFrom(g_netGame, FakeFind).why == LevelIdWhy::NoNetVarCs);

    PlantHealthy();
    Put(g_netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, nullptr);
    Put(g_netGame, facts::NETGAME_CS_SPACE_OFF, nullptr);
    CHECK(ReadLevelIdFrom(g_netGame, FakeFind).why == LevelIdWhy::NoSpace);

    // Not one of them read an id.
    CHECK(ReadLevelIdFrom(g_netGame, FakeFind).id == 0);
}

TEST_CASE("FillLevelId assigns the field the manifest carries, and the manifest carries it") {
    PlantHealthy();

    uint64_t pending = 0;
    LevelIdRead r;
    const bool got = FillLevelId(pending, g_netGame, FakeFind, r);

    CHECK(got);
    // THE MUTANT: build with W2A_MUTATE_NO_LEVELID and this is the line that goes red -- it is the
    // whole pre-fix defect, one dropped assignment.
    CHECK(pending == kPlantedId);

    static ManifestBuilder mb;
    mb.Begin(987654321uLL, 0u, 0u);
    mb.SetLevel(pending, 0u, 0u);
    CHECK(mb.View().levelId == kPlantedId);
}

TEST_CASE("a refused read leaves the field alone rather than writing a wrong id") {
    PlantHealthy();
    Put(g_cs, kOwnerPtrOff, nullptr);

    uint64_t pending = 0;
    LevelIdRead r;

    CHECK_FALSE(FillLevelId(pending, g_netGame, FakeFind, r));
    CHECK(pending == 0);
    CHECK(r.why == LevelIdWhy::NoOwner);
    CHECK(strstr(LevelIdWhyText(r.why), "owner") != nullptr);  // the line names the hop
}
