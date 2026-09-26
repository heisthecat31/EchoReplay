// The gametype id the capture records. The defect is OpenParams::gametypeId: declared, read by
// SetLevel and by BuildFileName, assigned nowhere -- so every file ever written carried
// manifest.gametypeId == 0 and every capture's file name claimed gametype 0.
//
// These drive the SHIPPED walk over synthetic memory laid out the way the engine lays it out:
// netGame -> +0x8 -> CR15Game -> +0x8568. The assertions are on the value that comes out and on
// the hop named when one does not.

#include "doctest.h"

#include "capture/gametypeid.h"
#include "capture/manifestbuild.h"

#include <string.h>

using namespace replay;
using namespace replay::capture;

namespace {

// Sizes and stand-in values are built from the registry offsets they must cover plus one pad, so
// no bare number in this file is the value of a registered fact.
constexpr size_t kPadLen = 137;
// echo_combat's CSymbol64. The engine's load is a qword and the engine compares all of it, so the
// recorder must keep all of it: the low dword alone names no gametype.
constexpr uint64_t kPlantedGametype = 0x3D5C3976578A321Aull;
// The qword ABOVE the gametype, planted non-zero so a read wider than the field cannot pass.
constexpr uint64_t kNeighbourAbove = 0x7A3D5E92C4F1B608ull;

uint8_t g_netGame[facts::IDENT_NETGAME_GAMESPACE_OFF + kPadLen];
uint8_t g_game[facts::GAME_GAMETYPE_HASH_OFF + 2 * sizeof(uint64_t) + kPadLen];

void* Slot(void* base, uint32_t off) { return static_cast<uint8_t*>(base) + off; }

void Put(void* base, uint32_t off, const void* value) {
    memcpy(Slot(base, off), &value, sizeof value);
}

// A complete, healthy session: the CR15Game hanging off the netGame, its gametype hash planted
// with a live neighbour above it.
void PlantHealthy() {
    memset(g_netGame, 0, sizeof g_netGame);
    memset(g_game, 0, sizeof g_game);

    Put(g_netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, g_game);
    const uint64_t hash = kPlantedGametype;
    memcpy(Slot(g_game, kGametypeHashOff), &hash, sizeof hash);
    const uint64_t above = kNeighbourAbove;
    memcpy(Slot(g_game, kGametypeHashOff + uint32_t(sizeof hash)), &above, sizeof above);
}

}  // namespace

TEST_CASE("the gametype id is read off the CR15Game the netGame carries") {
    PlantHealthy();

    const GametypeIdRead r = ReadGametypeIdFrom(g_netGame);

    CHECK(r.why == GametypeIdWhy::Ok);
    CHECK(r.id == kPlantedGametype);
    CHECK(r.game == static_cast<void*>(g_game));
}

TEST_CASE("the whole qword the engine loads is recorded, not its low dword") {
    PlantHealthy();

    const GametypeIdRead r = ReadGametypeIdFrom(g_netGame);

    // THE MUTANT: build with SHIP_MUTATE_GAMETYPE_LOW_DWORD (the four-byte read every earlier build
    // made) and both lines go red -- the file would say 0x00000000578A321A, which the engine does
    // not recognize as any gametype.
    CHECK((r.id >> 32) == (kPlantedGametype >> 32));
    CHECK(replay::gametype::IsFull(r.id));
    CHECK((r.id & 0xFFFFFFFFull) == (kPlantedGametype & 0xFFFFFFFFull));  // facts-ok: PHYS.ID_DEAD a u32 mask over a packed id; PHYS.ID_DEAD is PC_PRESENT and GOAL.USERID_UNSET is unmerged, so neither emits a constant
}

TEST_CASE("an unfilled gametype at open yields at close to what the lobby's owner state said") {
    using replay::capture::LateGametype;
    constexpr uint64_t kArena = 0xCB60A4DE7E1CAF73ull;

    // The spectate path: all-ones at open (either read width), or nothing read at all.
    CHECK(LateGametype(~0ull, kArena) == kArena);
    CHECK(LateGametype(0x00000000FFFFFFFFull, kArena) == kArena);  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    CHECK(LateGametype(0, kArena) == kArena);
    // The engine's own whole word stands, even against a different owner-state value.
    CHECK(LateGametype(kPlantedGametype, kArena) == 0);
    // No owner state, or one that states nothing usable: the read stands.
    CHECK(LateGametype(~0ull, 0) == 0);
    CHECK(LateGametype(~0ull, ~0ull) == 0);
    CHECK(LateGametype(~0ull, 0x7E1CAF73ull) == 0);
}

TEST_CASE("a netGame with no CR15Game yet names that hop") {
    PlantHealthy();
    Put(g_netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, nullptr);

    const GametypeIdRead r = ReadGametypeIdFrom(g_netGame);

    CHECK(r.why == GametypeIdWhy::NoGame);
    CHECK(r.id == 0);
    CHECK(r.game == nullptr);
}

TEST_CASE("a CR15Game whose gametype hash is not committed memory is refused, never guessed at") {
    PlantHealthy();
    // A pointer that is not mapped at all. The hop is span-proved, so this is a named refusal and
    // not an access violation inside a capture open.
    Put(g_netGame, facts::IDENT_NETGAME_GAMESPACE_OFF,
        reinterpret_cast<void*>(static_cast<uintptr_t>(1893ull)));

    const GametypeIdRead r = ReadGametypeIdFrom(g_netGame);

    CHECK(r.why == GametypeIdWhy::GameUnreadable);
    CHECK(r.id == 0);
    CHECK(r.game != nullptr);  // a refusal still names what it refused
}

TEST_CASE("no netGame reports itself rather than an empty answer") {
    const GametypeIdRead r = ReadGametypeIdFrom(nullptr);

    CHECK(r.why == GametypeIdWhy::NoNetGame);
    CHECK(r.id == 0);
    CHECK(strstr(GametypeIdWhyText(r.why), "CR15NetGame") != nullptr);
}

TEST_CASE("FillGametypeId assigns the field the manifest carries, and the manifest carries it") {
    PlantHealthy();

    uint64_t pending = 0;
    GametypeIdRead r;
    const bool got = FillGametypeId(pending, g_netGame, r);

    CHECK(got);
    // THE MUTANT: build with W2A_MUTATE_NO_GAMETYPEID and this is the line that goes red -- it is
    // the whole pre-fix defect, one dropped assignment.
    CHECK(pending == kPlantedGametype);

    static ManifestBuilder mb;
    mb.Begin(987654321uLL, 0u, 0u);
    mb.SetLevel(0u, pending, 0u);
    CHECK(mb.View().gametypeId == kPlantedGametype);
}

TEST_CASE("a refused read leaves the field alone rather than writing a wrong gametype") {
    PlantHealthy();
    Put(g_netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, nullptr);

    uint64_t pending = 0;
    GametypeIdRead r;

    CHECK_FALSE(FillGametypeId(pending, g_netGame, r));
    CHECK(pending == 0);
    CHECK(r.why == GametypeIdWhy::NoGame);
    CHECK(strstr(GametypeIdWhyText(r.why), "CR15Game") != nullptr);  // the line names the hop
}
