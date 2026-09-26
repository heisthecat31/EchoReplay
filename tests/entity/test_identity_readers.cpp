// The three readers ENG-W2A-ROSTER S1 asked W1-E for: EntrantCount, LoadoutNumbers and
// ProfileJsonBytes (with CopyJsonBytes, its cap/truncate half).
//
// The fixture is a FAKE CR15NetGame -- a buffer whose entrant count, non-replay count, user array
// and live loadout table sit at the registry's own offsets.  It is not a second copy of a format:
// every offset, stride and bound below comes from facts_generated.h, the same header
// identity_access.cpp reads them from, so a registry change moves both sides at once.
//
// The three mutants the brief names are cases 1 (an off-by-one entrant count), 4 (a wrong loadout
// number length/offset) and 3+6 (a pointer dereferenced without its span check).

#include "doctest.h"

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>
#include <windows.h>

#include "engine/entity.h"
#include "engine/identity_access.h"
#include "generated/facts_generated.h"

using replay::UserIndex;
using replay::UserSlot;
namespace ia = replay::identity_access;
using ia::Refusal;

namespace {

constexpr uint16_t kSlotMax = uint16_t(facts::ENTITY_USER_SLOT_TABLE_MAX);

// The last byte of the netGame this fixture has to own: the live loadout table is
// CR15NetGame + IDENT.LOADOUT_TABLE_OFF + (slot << IDENT.LOADOUT_ENTRY_SHIFT), which for the
// last slot is a long way past the user array.
constexpr size_t kLoadoutTableEnd =
    size_t(facts::IDENT_LOADOUT_TABLE_OFF) + (size_t(kSlotMax) << facts::IDENT_LOADOUT_ENTRY_SHIFT);

size_t NetGameBytes(uint16_t entrants) {
    const size_t users = size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * (entrants + 1u);
    return users > kLoadoutTableEnd ? users : kLoadoutTableEnd;
}

// A netGame in ordinary committed memory.  Everything Readable() is asked about here answers yes,
// which is what makes the uncommitted-page cases below mean something.
struct FakeNetGame {
    std::vector<uint8_t> bytes;

    explicit FakeNetGame(uint16_t entrants) {
        bytes.assign(NetGameBytes(entrants), 0);
        SetCount(entrants);
        SetNonReplay(entrants);
    }
    void* ng() { return bytes.data(); }
    void SetCount(uint16_t v) { Put(facts::IDENT_ENTRANT_COUNT_OFF, v); }
    void SetNonReplay(uint16_t v) { Put(facts::IDENT_NONREPLAY_COUNT_OFF, v); }
    void SetProfilePtr(uint16_t index, void* p) {
        memcpy(bytes.data() + size_t(facts::IDENT_PROFILE_JSON_OFF) +
                   size_t(facts::USER_STRIDE) * index,
               &p, sizeof p);
    }
    void SetNumbers(uint16_t slot, uint16_t number, uint16_t numberBody) {
        const size_t hdr = size_t(facts::IDENT_LOADOUT_TABLE_OFF) +
                           (size_t(slot) << facts::IDENT_LOADOUT_ENTRY_SHIFT);
        memcpy(bytes.data() + hdr + facts::IDENT_LOADOUT_NUMBER_OFF, &number, sizeof number);
        memcpy(bytes.data() + hdr + facts::IDENT_LOADOUT_NUMBER_BODY_OFF, &numberBody,
               sizeof numberBody);
    }
    void Put(uint32_t off, uint16_t v) { memcpy(bytes.data() + off, &v, sizeof v); }
};

// A reservation whose tail is NOT committed, so Readable() has something real to refuse.  One
// region serves both arms: the same pages, two different spans.
struct PartialRegion {
    uint8_t* base = nullptr;
    size_t   reserved = 0;

    PartialRegion(size_t reserve, size_t commit) {
        reserved = reserve;
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, reserve, MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(base != nullptr);
        REQUIRE(VirtualAlloc(base, commit, MEM_COMMIT, PAGE_READWRITE) != nullptr);
    }
    ~PartialRegion() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
    PartialRegion(const PartialRegion&) = delete;
    PartialRegion& operator=(const PartialRegion&) = delete;
};

size_t PageUp(size_t n) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    return ((n + page - 1) / page) * page;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// 1. EntrantCount is the engine's number, exactly, and the replay band comes with it.
//
// The count planted here is 7 and the buffer is sized for 8, so a reader that returned the
// buffer's capacity, or the count plus or minus one, is a different number from the one asserted.
// ---------------------------------------------------------------------------------------
TEST_CASE("EntrantCount publishes the engine's own count and band") {
    FakeNetGame g(8);
    g.SetCount(7);
    g.SetNonReplay(5);

    uint32_t count = 0xDEADBEEF, band = 0xDEADBEEF;
    Refusal why = Refusal::NoNetGame;
    REQUIRE(ia::EntrantCount(g.ng(), count, band, &why));
    CHECK(why == Refusal::None);
    CHECK(count == 7u);
    CHECK(band == 5u);

    // The two-argument spelling ENG-W2A-ROSTER S1.1 wrote is the same number.
    uint32_t alone = 0;
    REQUIRE(ia::EntrantCount(g.ng(), alone));
    CHECK(alone == count);

    // The band is the one UserDisplayName renames, so it has to agree with InReplayBand rather
    // than being a second reading of the same two fields.
    for (uint16_t i = 0; i < 7; ++i) {
        CHECK(ia::InReplayBand(g.ng(), UserIndex(i)) == (i >= band));
    }

    // A count of zero is an EMPTY roster, not an unreadable one: it answers true with 0.
    g.SetCount(0);
    g.SetNonReplay(0);
    count = 0xDEADBEEF;
    REQUIRE(ia::EntrantCount(g.ng(), count, band, &why));
    CHECK(count == 0u);
    CHECK(why == Refusal::None);
}

// ---------------------------------------------------------------------------------------
// 2. Every refusal is a DIFFERENT word, because the capture logs it and "false" is the silence
//    this packet exists to remove.
// ---------------------------------------------------------------------------------------
TEST_CASE("EntrantCount names which way it declined") {
    uint32_t count = 1, band = 1;
    Refusal why = Refusal::None;

    CHECK_FALSE(ia::EntrantCount(nullptr, count, band, &why));
    CHECK(why == Refusal::NoNetGame);

    // A non-replay count ABOVE the entrant count is not a roster this code can describe: the band
    // [nonReplay, count) is empty and a scan would read replayed entrants as normal ones.  Refused,
    // never clamped -- a clamp here invents a roster shape the engine never held.
    FakeNetGame g(8);
    g.SetCount(4);
    g.SetNonReplay(5);
    why = Refusal::None;
    CHECK_FALSE(ia::EntrantCount(g.ng(), count, band, &why));
    CHECK(why == Refusal::RosterUnreadable);

    // Equal is legal and means the band is empty -- every entrant carries its own name.
    g.SetNonReplay(4);
    REQUIRE(ia::EntrantCount(g.ng(), count, band, &why));
    CHECK(count == 4u);
    CHECK(band == 4u);
}

// ---------------------------------------------------------------------------------------
// 3. The user array the count CLAIMS has to be in committed memory.  Both arms come out of one
//    reservation, so what is being proved is the span and not the region.
// ---------------------------------------------------------------------------------------
TEST_CASE("EntrantCount refuses a count that overruns its own table") {
    const size_t need = NetGameBytes(64);
    PartialRegion r(PageUp(need) + 0x10000, PageUp(need));

    uint16_t small = 4;
    memcpy(r.base + facts::IDENT_ENTRANT_COUNT_OFF, &small, sizeof small);
    memcpy(r.base + facts::IDENT_NONREPLAY_COUNT_OFF, &small, sizeof small);

    uint32_t count = 0, band = 0;
    Refusal why = Refusal::None;
    REQUIRE(ia::EntrantCount(r.base, count, band, &why));
    CHECK(count == 4u);

    // Same region, same pages: only the count changes, and the array it now claims runs off the
    // committed tail.  A reader that skipped the span check would read the reserved page here.
    uint16_t huge = 60000;
    memcpy(r.base + facts::IDENT_ENTRANT_COUNT_OFF, &huge, sizeof huge);
    why = Refusal::None;
    CHECK_FALSE(ia::EntrantCount(r.base, count, band, &why));
    CHECK(why == Refusal::RosterUnreadable);
}

// ---------------------------------------------------------------------------------------
// 4. The two loadout numbers.  ENG-W2A-ROSTER S1.3 reported these as unreadable and wrote them as
//    0; they are the u16 pair at IDENT.LOADOUT_NUMBER_OFF / _BODY_OFF of the live loadout header.
//
//    The two values planted per slot are DIFFERENT from each other and different per slot, so a
//    reader that returned the wrong field, the wrong slot, or the same field twice is visible.
//    They are also chosen to differ in both bytes, so a wrong width shows up as well.
// ---------------------------------------------------------------------------------------
TEST_CASE("LoadoutNumbers reads the jersey and body numbers off the live header") {
    FakeNetGame g(8);
    for (uint16_t s = 0; s < kSlotMax; ++s) {
        g.SetNumbers(s, uint16_t(0x1100 + s), uint16_t(0x2200 + s * 3));
    }

    for (uint16_t s = 0; s < kSlotMax; ++s) {
        uint16_t number = 0, body = 0;
        Refusal why = Refusal::NoNetGame;
        REQUIRE(ia::LoadoutNumbers(g.ng(), UserSlot(s), number, body, &why));
        CHECK(why == Refusal::None);
        CHECK(number == uint16_t(0x1100 + s));
        CHECK(body == uint16_t(0x2200 + s * 3));
        CHECK(number != body);
    }

    // The header the numbers come off is the one FindInstance walks, at the stride the registry
    // records -- not a second address.  Writing through the declared struct and reading back
    // through the accessor is the only way these two can disagree.
    ia::LoadoutHeader* hdr = ia::LiveLoadout(g.ng(), UserSlot(3));
    REQUIRE(hdr != nullptr);
    hdr->number = 0xBEEF;
    hdr->numberBody = 0xF00D;
    uint16_t number = 0, body = 0;
    REQUIRE(ia::LoadoutNumbers(g.ng(), UserSlot(3), number, body));
    CHECK(number == 0xBEEF);
    CHECK(body == 0xF00D);
}

// ---------------------------------------------------------------------------------------
// 5. Out of range is not the same word as unreadable, and neither is "no session".
// ---------------------------------------------------------------------------------------
TEST_CASE("LoadoutNumbers names which way it declined") {
    FakeNetGame g(8);
    uint16_t number = 1, body = 1;
    Refusal why = Refusal::None;

    CHECK_FALSE(ia::LoadoutNumbers(nullptr, UserSlot(0), number, body, &why));
    CHECK(why == Refusal::NoNetGame);

    CHECK_FALSE(ia::LoadoutNumbers(g.ng(), UserSlot(kSlotMax), number, body, &why));
    CHECK(why == Refusal::SlotOutOfRange);
    // The engine's own no-slot word, the value IndexOfSlot and NetUserId already refuse on.
    constexpr uint16_t kNoSlot = 0xFFFFu;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
    CHECK_FALSE(ia::LoadoutNumbers(g.ng(), UserSlot(kNoSlot), number, body, &why));
    CHECK(why == Refusal::SlotOutOfRange);

    // The last in-range slot is in range: the bound is the engine's table size, not one less.
    CHECK(ia::LoadoutNumbers(g.ng(), UserSlot(uint16_t(kSlotMax - 1)), number, body, &why));
}

// ---------------------------------------------------------------------------------------
// 6. The loadout table is 0x51420 past CR15NetGame.  A netGame that has not built it is a
//    reserved page, not a zeroed one, and a reader without the span check reads it.
//
//    Both arms are the SAME reservation: the second call moves the base one page up so the table
//    lands past the committed tail.  The refusal is the span, not the region.
// ---------------------------------------------------------------------------------------
TEST_CASE("LoadoutNumbers refuses a header that is not in committed memory") {
    // The reservation has to be big enough that the SHIFTED base's table still lands inside it.
    // A reservation sized only for the un-shifted table puts the shifted read past the region
    // altogether, where whatever the allocator mapped next decides the answer -- a fixture whose
    // verdict is the heap's, not the code's.
    const size_t commit = PageUp(kLoadoutTableEnd);
    PartialRegion r(commit * 2 + kLoadoutTableEnd + 0x10000, commit);

    uint16_t number = 0, body = 0;
    Refusal why = Refusal::None;
    REQUIRE(ia::LoadoutNumbers(r.base, UserSlot(uint16_t(kSlotMax - 1)), number, body, &why));
    CHECK(why == Refusal::None);

    // Same pages, table shifted past the commit boundary.
    why = Refusal::None;
    CHECK_FALSE(ia::LoadoutNumbers(r.base + commit - 0x1000, UserSlot(uint16_t(kSlotMax - 1)),
                                   number, body, &why));
    CHECK(why == Refusal::NoLoadoutInstance);
}

// ---------------------------------------------------------------------------------------
// 7. CopyJsonBytes -- the cap/truncate half of the JSON contract, which is the half that can be
//    proved with no game present.  Byte-exact on the fitting arm, and NOTHING copied on the short
//    one: a prefix of a JSON document parses as nothing and would reach the demo file looking
//    like a capture.
// ---------------------------------------------------------------------------------------
TEST_CASE("CopyJsonBytes is byte-exact, and never cuts silently") {
    const char doc[] = "{\"displayname\":\"Player \\u00c5\",\"unlocks\":[\"a\",\"b\"]}";
    const size_t len = sizeof(doc) - 1;

    uint8_t out[128];
    size_t written = 0;
    bool truncated = true;
    Refusal why = Refusal::NoNetGame;

    memset(out, 0xCC, sizeof out);
    REQUIRE(ia::CopyJsonBytes(doc, len, out, sizeof out, written, truncated, &why));
    CHECK(why == Refusal::None);
    CHECK(written == len);
    CHECK_FALSE(truncated);
    CHECK(memcmp(out, doc, len) == 0);
    CHECK(out[len] == 0xCC);   // no terminator invented, nothing written past the document

    // cap exactly equal to the length fits.  This is the boundary the ShortBuffer arm turns on.
    memset(out, 0xCC, sizeof out);
    REQUIRE(ia::CopyJsonBytes(doc, len, out, len, written, truncated, &why));
    CHECK(memcmp(out, doc, len) == 0);
    CHECK(out[len] == 0xCC);
    CHECK_FALSE(truncated);

    // One byte short: refused by name, the FULL length reported so the caller can size a second
    // call, and the buffer left exactly as it was.
    memset(out, 0xCC, sizeof out);
    written = 0;
    CHECK_FALSE(ia::CopyJsonBytes(doc, len, out, len - 1, written, truncated, &why));
    CHECK(why == Refusal::ShortBuffer);
    CHECK(truncated);
    CHECK(written == len);
    for (size_t i = 0; i < sizeof out; ++i) CHECK(out[i] == 0xCC);

    // A zero-length document is a document, not a failure, and it writes nothing.
    memset(out, 0xCC, sizeof out);
    REQUIRE(ia::CopyJsonBytes(doc, 0, out, sizeof out, written, truncated, &why));
    CHECK(written == 0u);
    CHECK_FALSE(truncated);
    CHECK(out[0] == 0xCC);

    // A cap of zero with a document to copy is the short arm, not a quiet success.
    CHECK_FALSE(ia::CopyJsonBytes(doc, len, nullptr, 0, written, truncated, &why));
    CHECK(why == Refusal::ShortBuffer);
    CHECK(written == len);
}

// ---------------------------------------------------------------------------------------
// 8. ProfileJsonBytes.  On this image it cannot produce bytes for a live entrant -- the profile is
//    a CJson node tree and there is no serialiser to call -- and the WHOLE point of the function is
//    that the four ways it can decline are four different words.  A capture that logs
//    "NoJsonWriter" knows the gap is the engine's; one that logs "EmptyProfile" knows the player
//    really had none.  Before this, both were an empty blob.
// ---------------------------------------------------------------------------------------
TEST_CASE("ProfileJsonBytes separates the four reasons apart") {
    uint8_t out[64];
    size_t written = 1;
    bool truncated = true;
    Refusal why = Refusal::None;

    CHECK_FALSE(ia::ProfileJsonBytes(nullptr, UserIndex(0), out, sizeof out, written, truncated, &why));
    CHECK(why == Refusal::NoNetGame);
    CHECK(written == 0u);       // the out-params are cleared before anything else can go wrong
    CHECK_FALSE(truncated);

    FakeNetGame g(8);
    g.SetCount(3);
    g.SetNonReplay(3);

    // Past the engine's own entrant count is not an entrant.
    CHECK_FALSE(ia::ProfileJsonBytes(g.ng(), UserIndex(3), out, sizeof out, written, truncated, &why));
    CHECK(why == Refusal::NoSuchEntrant);

    // In range, but the profile pointer is null -- breaker 1, the one that makes ApplyLoadoutItems
    // return without a word.
    CHECK_FALSE(ia::ProfileJsonBytes(g.ng(), UserIndex(1), out, sizeof out, written, truncated, &why));
    CHECK(why == Refusal::EmptyProfile);

    // A count whose user array is not committed is unreadable, which is not the same as empty.
    const size_t need = NetGameBytes(64);
    PartialRegion r(PageUp(need) + 0x10000, PageUp(need));
    uint16_t huge = 60000;
    memcpy(r.base + facts::IDENT_ENTRANT_COUNT_OFF, &huge, sizeof huge);
    CHECK_FALSE(ia::ProfileJsonBytes(r.base, UserIndex(0), out, sizeof out, written, truncated, &why));
    CHECK(why == Refusal::RosterUnreadable);
}

// ---------------------------------------------------------------------------------------
// 9. Every refusal this packet added has its own sentence.  A reason code the log prints as
//    "unknown" is the silence again, one indirection further out.
// ---------------------------------------------------------------------------------------
TEST_CASE("the new refusals each have their own text") {
    const Refusal added[] = {Refusal::RosterUnreadable, Refusal::ShortBuffer, Refusal::NoJsonWriter};
    for (Refusal r : added) {
        const std::string text = ia::RefusalText(r);
        CAPTURE(text);
        CHECK(text != "unknown");
        CHECK(text.size() > 8u);
    }
    // Distinct from each other and from the neighbour they were appended after.
    CHECK(std::string(ia::RefusalText(Refusal::RosterUnreadable)) !=
          std::string(ia::RefusalText(Refusal::ShortBuffer)));
    CHECK(std::string(ia::RefusalText(Refusal::NoJsonWriter)) !=
          std::string(ia::RefusalText(Refusal::ShortBuffer)));
    CHECK(std::string(ia::RefusalText(Refusal::NoJsonWriter)) !=
          std::string(ia::RefusalText(Refusal::NoReleasePath)));
}
