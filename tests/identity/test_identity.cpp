// W2-C's offline half: the two pieces of the cosmetic path that are pure logic, driven with
// no engine present.  Everything else in this packet needs a live CR15NetGame and is queued
// through runs\queue\W2-C.probe.json.
//
// The fixtures below never reuse a value across two fields.  A recorded loadout whose twenty
// ids were all the same, or whose number equalled its number_body, would pass just as happily
// with the indexing wrong -- and the engine's own bot path sets both numbers to the SAME draw,
// so that is not a hypothetical shape.

#include "doctest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <string>

#include <windows.h>   // the marker assertions need a crash-log file for MarkHop to write into

#include "engine/actor_alias.h"
#include "engine/identity_access.h"
#include "engine/identity_access_facts.h"
#include "engine/pawn_provision.h"
#include "engine/tick_policy.h"
#include "engine/wiretap.h"
#include <vector>
#include "generated/facts_generated.h"
#include "bootstrap/livesafety.h"
#include "events/lobbymsg.h"
#include "identity/dresser.h"

using namespace replay;
using replay::identity::DiffItems;
using replay::identity::ItemDiff;

namespace {

// Twenty distinct non-zero ids.  0x100 apart so a one-slot indexing error cannot land on a
// neighbouring value by accident.
void FillDistinct(Symbol64 v[20]) {
    for (uint32_t i = 0; i < 20; ++i) v[i] = 0x5000ull + 0x100ull * (i + 1);
}

struct Recorder {
    static constexpr uint32_t kMax = 64;
    char     keys[kMax][128];
    char     strings[kMax][128];
    int64_t  ints[kMax];
    bool     isInt[kMax];
    uint32_t n;

    void Reset() { memset(this, 0, sizeof *this); }

    int Find(const char* key) const {
        for (uint32_t i = 0; i < n; ++i)
            if (strcmp(keys[i], key) == 0) return int(i);
        return -1;
    }
};

void Copy(char* dst, size_t cap, const char* src) {
    size_t n = 0;
    while (src[n] && n + 1 < cap) { dst[n] = src[n]; ++n; }
    dst[n] = '\0';
}

void RecString(void* ctx, const char* key, const char* value) {
    Recorder* r = static_cast<Recorder*>(ctx);
    if (r->n >= Recorder::kMax) return;
    Copy(r->keys[r->n], sizeof r->keys[0], key);
    Copy(r->strings[r->n], sizeof r->strings[0], value);
    r->isInt[r->n] = false;
    ++r->n;
}

void RecInt(void* ctx, const char* key, int64_t value) {
    Recorder* r = static_cast<Recorder*>(ctx);
    if (r->n >= Recorder::kMax) return;
    Copy(r->keys[r->n], sizeof r->keys[0], key);
    r->ints[r->n] = value;
    r->isInt[r->n] = true;
    ++r->n;
}

identity_access::ProfileWalkStats Walk(Recorder& r, const char* text) {
    r.Reset();
    const identity_access::ProfileScalarSink sink{&r, &RecString, &RecInt};
    const Blob b{reinterpret_cast<const uint8_t*>(text), uint32_t(strlen(text))};
    return identity_access::WalkProfileScalars(b, sink);
}

}  // namespace

TEST_CASE("DiffItems separates a remap from a strip") {
    Symbol64 want[20];
    Symbol64 got[20];
    FillDistinct(want);
    memcpy(got, want, sizeof got);

    SUBCASE("an untouched apply is all matching") {
        const ItemDiff d = DiffItems(want, got);
        CHECK(d.compared == 20);
        CHECK(d.matching == 20);
        CHECK(d.remapped == 0);
        CHECK(d.absent == 0);
    }

    SUBCASE("a different non-zero id is a remap, not a loss") {
        // LoadAndApplyItem rewrites three ids to hard-coded defaults when the
        // net_standard_chassis rule var is on.  Scoring that as a strip would make every
        // ordinary match report a wrong avatar.
        got[3] = 0xDEAD1000ull;
        got[11] = 0xDEAD2000ull;
        got[19] = 0xDEAD3000ull;
        const ItemDiff d = DiffItems(want, got);
        CHECK(d.compared == 20);
        CHECK(d.matching == 17);
        CHECK(d.remapped == 3);
        CHECK(d.absent == 0);
    }

    SUBCASE("a zeroed slot is the strip breaker 2 describes") {
        got[7] = 0;
        const ItemDiff d = DiffItems(want, got);
        CHECK(d.absent == 1);
        CHECK(d.remapped == 0);
        CHECK(d.matching == 19);
    }

    SUBCASE("positions the recording left empty are not scored") {
        want[2] = 0;
        want[5] = 0;
        got[2] = 0;
        got[5] = 0;
        const ItemDiff d = DiffItems(want, got);
        CHECK(d.compared == 18);
        CHECK(d.matching == 18);
    }

    SUBCASE("an off-by-one apply is caught rather than absorbed") {
        // Every id shifted one slot: with twenty distinct values this is nineteen remaps and
        // one absent.  With twenty equal values it would read as a perfect match, which is why
        // the fixture uses distinct ones.
        for (uint32_t i = 19; i > 0; --i) got[i] = want[i - 1];
        got[0] = 0;
        const ItemDiff d = DiffItems(want, got);
        CHECK(d.matching == 0);
        CHECK(d.remapped == 19);
        CHECK(d.absent == 1);
    }

    SUBCASE("a null side is empty, never a pass") {
        const ItemDiff d = DiffItems(nullptr, got);
        CHECK(d.compared == 0);
        CHECK(d.matching == 0);
    }
}

TEST_CASE("the profile walk builds the engine's own pipe-separated keys") {
    Recorder r;

    SUBCASE("nesting becomes a pipe path") {
        const auto s = Walk(r, "{\"displayname\":\"Ada\",\"stats\":{\"arena\":{\"Level\":{\"val\":37}}}}");
        CHECK(s.wellFormed);
        CHECK(s.scalars == 2);
        CHECK(s.arrays == 0);
        const int name = r.Find("displayname");
        REQUIRE(name >= 0);
        CHECK(!r.isInt[name]);
        CHECK(strcmp(r.strings[name], "Ada") == 0);
        const int lvl = r.Find("stats|arena|Level|val");
        REQUIRE(lvl >= 0);
        CHECK(r.isInt[lvl]);
        CHECK(r.ints[lvl] == 37);
    }

    SUBCASE("both loadout numbers survive as distinct values") {
        // GenerateBotLoadout writes the SAME random draw to both, so a fixture with equal
        // numbers cannot tell "carried correctly" from "left as the engine randomised them".
        const auto s = Walk(r, "{\"loadout\":{\"number\":42,\"number_body\":91}}");
        CHECK(s.wellFormed);
        const int a = r.Find("loadout|number");
        const int b = r.Find("loadout|number_body");
        REQUIRE(a >= 0);
        REQUIRE(b >= 0);
        CHECK(r.ints[a] == 42);
        CHECK(r.ints[b] == 91);
        CHECK(r.ints[a] != r.ints[b]);
    }

    SUBCASE("an array is counted and named, never silently dropped") {
        const auto s = Walk(r, "{\"displayname\":\"Ada\",\"unlocks\":[\"a\",\"b\",\"c\"],\"bodytype\":2}");
        CHECK(s.wellFormed);
        CHECK(s.arrays == 1);
        CHECK(strcmp(s.firstArray, "unlocks") == 0);
        // The scalars on either side of it still land: the array is skipped, not the document.
        CHECK(s.scalars == 2);
        CHECK(r.Find("bodytype") >= 0);
    }

    SUBCASE("nested and empty arrays are still one skip each") {
        const auto s = Walk(r, "{\"a\":[[1,2],[3]],\"b\":[],\"c\":1}");
        CHECK(s.wellFormed);
        CHECK(s.arrays == 2);
        CHECK(strcmp(s.firstArray, "a") == 0);
        CHECK(s.scalars == 1);
    }

    SUBCASE("negative, boolean and null are each handled distinctly") {
        const auto s = Walk(r, "{\"a\":-17,\"b\":true,\"c\":false,\"d\":null}");
        CHECK(s.wellFormed);
        CHECK(s.scalars == 3);          // null is absent, which is not the same as zero
        CHECK(r.ints[r.Find("a")] == -17);
        CHECK(r.ints[r.Find("b")] == 1);
        CHECK(r.ints[r.Find("c")] == 0);
        CHECK(r.Find("d") < 0);
    }

    SUBCASE("a fractional number is carried as text rather than truncated") {
        const auto s = Walk(r, "{\"pct\":0.5}");
        CHECK(s.wellFormed);
        const int i = r.Find("pct");
        REQUIRE(i >= 0);
        CHECK(!r.isInt[i]);
        CHECK(strcmp(r.strings[i], "0.5") == 0);
    }

    SUBCASE("escapes are resolved") {
        const auto s = Walk(r, "{\"displayname\":\"A\\\\B\\\"C\"}");
        CHECK(s.wellFormed);
        CHECK(strcmp(r.strings[0], "A\\B\"C") == 0);
    }

    SUBCASE("malformed input is reported, and what was applied stays applied") {
        const auto s = Walk(r, "{\"displayname\":\"Ada\",\"stats\":{");
        CHECK(!s.wellFormed);
        CHECK(s.scalars == 1);
        CHECK(r.Find("displayname") >= 0);
    }

    SUBCASE("the malformed class does not run away") {
        const char* nasty[] = {
            "", "{", "}", "{}", "[", "\"", "{\"a\"}", "{\"a\":}", "{:1}", "{\"a\":1,}",
            "{\"a\":\"unterminated", "{\"a\":[1,2", "{\"a\":{\"b\":{\"c\":", "null", "12",
            "{\"a\":truex}", "{\"a\":--1}", "{\"\":1}",
        };
        for (const char* t : nasty) {
            const auto s = Walk(r, t);
            // The only requirement is that it returns, never that it accepts.
            CHECK(s.scalars <= Recorder::kMax);
        }
    }

    SUBCASE("depth beyond the cap is refused rather than recursed") {
        char deep[512];
        size_t at = 0;
        deep[at++] = '{';
        for (int i = 0; i < 40; ++i) at += size_t(snprintf(deep + at, sizeof deep - at, "\"k\":{"));
        at += size_t(snprintf(deep + at, sizeof deep - at, "\"v\":1"));
        for (int i = 0; i < 41; ++i) deep[at++] = '}';
        deep[at] = '\0';
        const auto s = Walk(r, deep);
        CHECK(!s.wellFormed);
    }

    SUBCASE("an empty blob is not well formed and sets nothing") {
        const identity_access::ProfileScalarSink sink{&r, &RecString, &RecInt};
        r.Reset();
        const auto s = identity_access::WalkProfileScalars(Blob{nullptr, 0}, sink);
        CHECK(!s.wellFormed);
        CHECK(s.scalars == 0);
        CHECK(r.n == 0);
    }
}

TEST_CASE("the two index spaces do not convert") {
    // Not a runtime check: this is here so the assertion is visible in the gate output. The
    // real proof is that the two lines below do not compile without the explicit conversion,
    // which is what DESIGN 9.1 asks for -- confusing them silently dresses the wrong player.
    UserSlot slot(3);
    UserIndex index(7);
    CHECK(slot.v != index.v);
    static_assert(!__is_convertible_to(UserSlot, UserIndex), "UserSlot must not convert to UserIndex");
    static_assert(!__is_convertible_to(UserIndex, UserSlot), "UserIndex must not convert to UserSlot");
    static_assert(!__is_convertible_to(uint16_t, UserSlot), "a bare u16 must not become a UserSlot");
}

// -------------------------------------------------------------------------------------------
// C-1: every engine address goes through the mapped image.
//
// This process is not echovr.exe.  Every recorded 0x140...... rebases to
// GetModuleHandleW(nullptr) + (VA - PE_IMAGE_BASE), which for this executable is far outside
// its own SizeOfImage -- so BuildMatches() must REFUSE every site by name and return,
// and every Entry() must hand back null instead of a wild pointer.
//
// This is the offline half of the fix and it is two-sided: the passing input is the rebasing
// module (refuses cleanly, names every site), and the failing input is the module reverted to
// dereferencing the literals, which faults in this process rather than refusing.  The
// mutation was run and is recorded in the packet report; what is asserted here is the shape
// that only the rebasing version can produce.
// -------------------------------------------------------------------------------------------
TEST_CASE("no engine address is dereferenced un-rebased") {
    const size_t n = identity_access::EngineSiteCount();
    // A table that SHRANK is a site that stopped being checked, so the floor is what is pinned.
    // The exact number is NOT: it was 14 when this case was written and is larger now that
    // W1-E's readers and the eight CJson accessors have landed, and a literal that every packet
    // adding a reader has to come and edit is a check that reds the wrong person.  That the
    // table and the Site enum agree is static_asserted in identity_access.cpp; what is left for
    // a runtime case is this floor and the per-site properties below.
    CHECK(n >= 14);

    // BuildMatches has to RETURN.  Reaching this line at all is the assertion: the pre-fix
    // module read 0x140154C00 here, which is not mapped in this process.
    const bool matches = identity_access::BuildMatches();
    CHECK(!matches);

    const char* detail = identity_access::BuildMismatchDetail();
    REQUIRE(detail != nullptr);
    for (size_t i = 0; i < n; ++i) {
        const char* name = identity_access::EngineSiteName(i);
        REQUIRE(name != nullptr);
        CHECK(identity_access::EngineSiteVa(i) >= facts::PE_IMAGE_BASE);
        // Refused, so nothing resolved -- and nothing can be called.
        CHECK(identity_access::EngineSiteResolved(i) == nullptr);
        // ...and it said WHICH site, rather than collapsing every answer into one bool.  This
        // is the assertion a fixed-size detail buffer fails: the names past the cap go missing.
        CHECK(strstr(detail, name) != nullptr);
        // Distinct, so the loop above is n answers and not one name matched n times.
        for (size_t j = i + 1; j < n; ++j)
            CHECK(strcmp(name, identity_access::EngineSiteName(j)) != 0);
    }
}

// -------------------------------------------------------------------------------------------
// C-3: the slot-table name write -- the source UserDisplayName consults on every live path.
//
// The engine object is synthesised here from the registry's own offsets, so the write can be
// exercised with no game: a CR15NetGame-shaped word at IDENT.SLOTTABLE_OFF pointing at an
// owner, whose +IDENT.SLOTTABLE_VEC_OFF vector carries the count and the entry-array base.
// What is asserted is WHERE the bytes landed, not that the call returned -- a write to the
// wrong stride or the wrong field offset returns success just as happily.
// -------------------------------------------------------------------------------------------
namespace {

constexpr uint32_t kEntries = 4;

struct FakeSlotTable {
    uint8_t  ng[0x60];
    uint8_t  owner[0x400];
    uint8_t  entries[kEntries * 216];

    void Build(uint64_t count) {
        memset(this, 0xAB, sizeof *this);     // 0xAB, not 0: an untouched byte has to be visible
        uint8_t* vec = owner + facts::IDENT_SLOTTABLE_VEC_OFF;
        void*    ownerPtr = owner;
        uint8_t* basePtr = entries;
        memcpy(ng + facts::IDENT_SLOTTABLE_OFF, &ownerPtr, sizeof ownerPtr);
        memcpy(vec + facts::IDENT_SLOTNAME_COUNT_OFF, &count, sizeof count);
        memcpy(vec + facts::IDENT_SLOTNAME_BASE_OFF, &basePtr, sizeof basePtr);
    }

    const uint8_t* Entry(uint32_t i) const { return entries + size_t(216) * i; }
    const char* Name(uint32_t i) const {
        return reinterpret_cast<const char*>(Entry(i) + facts::IDENT_SLOTNAME_FIELD_OFF);
    }
    // What IDENT.SLOTNAME_GET returns for sel == 0: base + 216*slot + 0x3C.  The routine's two
    // arms are `mov eax,60h / mov r9d,3Ch / cmovz eax,r9d` at 0x14060831C-0x140608327, so this is
    // the same address arithmetic the engine's own reader does, with the other selector.
    const char* MirrorName(uint32_t i) const {
        return reinterpret_cast<const char*>(Entry(i) + identity_access::kSlotNameMirror);
    }
    // The engine's empty entrant: both name buffers zeroed, everything else as it was.  This is
    // CNSLobby::SetEntrantDisplayName(entrant, nullptr) -- Fill(+0x60, 0, 0x24) then the same
    // 0x24-byte copy into the mirror that every other write of that field performs.
    void EngineEmptyName(uint32_t i) {
        uint8_t* e = entries + size_t(216) * i;
        memset(e + facts::IDENT_SLOTNAME_FIELD_OFF, 0, identity_access::kSlotNameCap);
        memcpy(e + identity_access::kSlotNameMirror, e + facts::IDENT_SLOTNAME_FIELD_OFF,
               identity_access::kSlotNameCap);
    }
    bool Untouched(uint32_t i) const {
        for (size_t b = 0; b < 216; ++b)
            if (Entry(i)[b] != 0xAB) return false;
        return true;
    }
};

}  // namespace

TEST_CASE("the recorded name is written to the engine's slot-name table") {
    static FakeSlotTable t;    // 1.5 KB; static so it is not a stack frame this size
    using replay::identity_access::Refusal;

    SUBCASE("it lands in exactly one entry, at exactly the recorded field offset") {
        t.Build(kEntries);
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(2), "player02") == Refusal::None);
        CHECK(strcmp(t.Name(2), "player02") == 0);
        // The stride and the field offset both have to be right for this to hold: a wrong
        // stride writes into a neighbour, a wrong field offset writes inside this entry.
        CHECK(t.Untouched(0));
        CHECK(t.Untouched(1));
        CHECK(t.Untouched(3));
        // Everything BELOW the mirror is untouched -- a wrong stride writes into a neighbour, a
        // wrong field offset writes inside this entry.
        for (size_t b = 0; b < identity_access::kSlotNameMirror; ++b)
            CHECK(t.Entry(2)[b] == 0xAB);
        // The mirror at +0x3C is the field IDENT.SLOTNAME_GET's `sel == 0` arm returns, and the
        // engine's setter keeps it equal to the name.  So does ours.
        CHECK(strcmp(t.MirrorName(2), "player02") == 0);
        CHECK(memcmp(t.Entry(2) + identity_access::kSlotNameMirror,
                     t.Entry(2) + facts::IDENT_SLOTNAME_FIELD_OFF,
                     identity_access::kSlotNameCap) == 0);
        // +0x84 is the FIRST byte past the name buffer -- a separate u32 the engine's own refresh
        // zeroes at 0x140608CA6.  The guard used to sit at 0x8C, eight bytes further on, which is
        // why a 44-byte cap writing through 0x84..0x8B passed for as long as it did.
        for (size_t b = 0x84; b < 216; ++b)
            CHECK(t.Entry(2)[b] == 0xAB);
    }

    SUBCASE("a slot at or past the vector's own bound is refused, and nothing is written") {
        t.Build(2);
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(2), "player02") == Refusal::NoSlotNameTable);
        CHECK(t.Untouched(2));
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(1), "player02") == Refusal::None);
    }

    SUBCASE("a name too long for the inline buffer is refused, never truncated") {
        t.Build(kEntries);
        char longest[identity_access::kSlotNameCap];       // 35 chars + NUL: the exact fit
        memset(longest, 'x', sizeof longest - 1);
        longest[sizeof longest - 1] = '\0';
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(0), longest) == Refusal::None);
        CHECK(strcmp(t.Name(0), longest) == 0);
        CHECK(strcmp(t.MirrorName(0), longest) == 0);
        // The longest name the engine accepts still stops at the buffer's own end.
        for (size_t b = 0x84; b < 216; ++b)
            CHECK(t.Entry(0)[b] == 0xAB);

        char oversize[identity_access::kSlotNameCap + 1];  // one past it
        memset(oversize, 'y', sizeof oversize - 1);
        oversize[sizeof oversize - 1] = '\0';
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(1), oversize) == Refusal::SlotNameTooLong);
        CHECK(t.Untouched(1));
    }

    SUBCASE("no table at all is a named refusal, not a fault") {
        t.Build(kEntries);
        memset(t.ng + facts::IDENT_SLOTTABLE_OFF, 0, sizeof(void*));
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(0), "player02") == Refusal::NoSlotNameTable);
        CHECK(identity_access::SetSlotName(static_cast<void*>(nullptr), UserSlot(0), "player02") == Refusal::NoSlotNameTable);
    }
}

// -------------------------------------------------------------------------------------------
// The audit asked which engine callers pass IDENT.SLOTNAME_GET's `sel == 0`.  The
// answer, derived from both binaries, is NONE: all eight .text call sites in echovr.exe and all
// twelve callers of CNSLobby::EntrantDisplayName in libr15.so pass 1.  The dead arm was not the
// hazard.  The hazard the derivation DID find is that +0x3C is not a read-only alternate view --
// the engine writes it as a mirror of +0x60 (libr15 CNSLobby::SetEntrantDisplayName @0x192EC3C)
// and copies it BACK over +0x60 in the entrant refresh at 0x1406087F0.  A vacate that clears only
// +0x60 is therefore a state the engine never produces, and the refresh un-vacates it.
// -------------------------------------------------------------------------------------------

TEST_CASE("a vacated slot is indistinguishable from an engine-empty one, through both selectors") {
    static FakeSlotTable t;
    using replay::identity_access::Refusal;
    const size_t kField  = facts::IDENT_SLOTNAME_FIELD_OFF;
    const size_t kMirror = identity_access::kSlotNameMirror;
    const size_t kCap    = identity_access::kSlotNameCap;

    SUBCASE("both name buffers match an engine-emptied entry byte for byte") {
        t.Build(kEntries);
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(2), "Wrex") == Refusal::None);
        CHECK(strcmp(t.Name(2), "Wrex") == 0);

        // Slot 3 is emptied the way the engine empties one; slot 2 is emptied the way we vacate.
        t.EngineEmptyName(3);
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(2), "") == Refusal::None);

        CHECK(memcmp(t.Entry(2) + kMirror, t.Entry(3) + kMirror, kCap) == 0);
        CHECK(memcmp(t.Entry(2) + kField,  t.Entry(3) + kField,  kCap) == 0);
        // Not vacuously equal because both are 0xAB: the engine-empty state is zeroes.
        for (size_t b = 0; b < kCap; ++b) {
            CHECK(t.Entry(2)[kField + b]  == 0x00);
            CHECK(t.Entry(2)[kMirror + b] == 0x00);
        }
        // Both selectors of IDENT.SLOTNAME_GET now read empty on the vacated slot.
        CHECK(t.Name(2)[0] == 0);
        CHECK(t.MirrorName(2)[0] == 0);
    }

    SUBCASE("the engine's own refresh cannot restore the vacated name") {
        t.Build(kEntries);
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(1), "Wrex") == Refusal::None);
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(1), "") == Refusal::None);

        // sub_1406087F0's second arm, verbatim: 0x24 bytes moved from the mirror back over the
        // name field (movups/movups/mov at 0x140608CC1..0x140608CD9).  A vacate that left the
        // mirror holding "Wrex" would put "Wrex" back on screen right here.
        uint8_t* e = const_cast<uint8_t*>(t.Entry(1));
        memcpy(e + kField, e + kMirror, kCap);
        CHECK(t.Name(1)[0] == 0);

        // And the first arm: strlen(mirror) + 1 copied forward.
        memcpy(e + kField, e + kMirror, strlen(reinterpret_cast<const char*>(e + kMirror)) + 1);
        CHECK(t.Name(1)[0] == 0);
    }

    SUBCASE("a re-dress after a vacate leaves no residue of the longer previous name") {
        t.Build(kEntries);
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(0), "Wrexington-Vakarian") == Refusal::None);
        CHECK(identity_access::SetSlotName(t.ng, UserSlot(0), "Liara") == Refusal::None);
        CHECK(strcmp(t.Name(0), "Liara") == 0);
        CHECK(strcmp(t.MirrorName(0), "Liara") == 0);
        // The engine zero-fills the tail after the terminator; so must we, or a reader that
        // scans the buffer rather than stopping at the NUL still sees the old name's tail.
        for (size_t b = strlen("Liara"); b < kCap; ++b) {
            CHECK(t.Entry(0)[kField + b]  == 0x00);
            CHECK(t.Entry(0)[kMirror + b] == 0x00);
        }
    }
}

// -------------------------------------------------------------------------------------------
// C-2: the name check is on what the engine RENDERS, and a verdict cannot be reached without it.
//
// The observable itself needs a live CR15NetGame -- that is read L3 of the guarded launch.
// What is constructible offline is the half that made C-2 dangerous: whether Dress can report
// success having never observed the rendered name.  The fake below satisfies every other step
// and returns the recorded items exactly, so the ONLY thing standing between it and `ok` is
// the name arm.  Deleting nameMatches from Dress's verdict turns this red.
// -------------------------------------------------------------------------------------------
namespace {

class FakeIdentity final : public IIdentity {
public:
    Symbol64 applied[20] = {};

    uint16_t provisionedSlot = identity::kNotDressed;

    bool AddReplayUser(UserSlot, UserIndex& out) override { out = UserIndex(4); return true; }
    // Admits where it is asked, so `SlotOf` below answers the recorded slot and this fake keeps
    // saying exactly what it said before ARCH-23 S7.2 split the two verbs.
    bool ProvisionReplayUser(UserSlot s, const JoinDesc&, UserIndex& out) override {
        provisionedSlot = s.v;
        return AddReplayUser(s, out);
    }
    bool SetProfileJson(UserSlot, const Blob&) override { return true; }
    bool SetTeam(UserIndex, uint8_t, uint8_t) override { return true; }
    bool ApplyLoadout(UserIndex, const Symbol64[20], uint16_t, uint16_t) override { return true; }
    bool OverrideAlignment(UserIndex, uint16_t) override { return true; }
    bool ReadAppliedItems(UserIndex, Symbol64 out[20]) const override {
        memcpy(out, applied, sizeof applied);
        return true;
    }
    UserSlot  SlotOf(UserIndex) const override { return UserSlot(provisionedSlot); }
    UserIndex IndexOf(UserSlot) const override { return UserIndex(4); }
};

}  // namespace

TEST_CASE("the verdict requires the rendered name, not just the items") {
    identity::DressResult r{};
    r.items.compared = 20;
    r.items.matching = 20;
    r.items.absent = 0;

    // A perfect loadout and a name the engine does not render is a FAILURE.  This is C-2 in one
    // assertion: the old check read back the profile it had just written and reported "name ok"
    // while UserDisplayName returned "Unknown" for every player.
    r.nameMatches = false;
    CHECK(!identity::Verdict(r));

    r.nameMatches = true;
    CHECK(identity::Verdict(r));

    // ...and the name alone is not enough either, so the arm cannot be satisfied by itself.
    r.items.absent = 1;
    CHECK(!identity::Verdict(r));
    r.items.absent = 0;
    r.items.compared = 0;
    CHECK(!identity::Verdict(r));
}

// NAMES-4.  The engine holds no loadout instance for four of the five rows of the subject capture,
// because the recording carries no loadout document for them (`profile refused` in the identity
// block) and the instance is what the engine's parse of that document builds.  The twenty item ids
// have nowhere to go; the name, both jersey numbers, the team and the colour have already landed
// when that is discovered.  Judging such a row on its items counted every one of them as NOT
// dressed and hid the four writes that did land.
TEST_CASE("NAMES-4: a slot the engine holds no loadout instance for is judged on its name") {
    identity::DressResult r{};
    r.nameMatches    = true;
    r.loadoutRefusal = identity_access::Refusal::NoLoadoutInstance;
    r.items.compared = 0;          // nothing could be compared: there was no instance to write into
    r.items.absent   = 20;         // ...so the read-back sees twenty positions we did not write
    CHECK(identity::Verdict(r));

    // The name arm is not weakened by it: the whole point of the row is still the name.
    r.nameMatches = false;
    CHECK(!identity::Verdict(r));

    // ...and the arm is keyed on THIS refusal only.  A slot that HAS an instance and whose items
    // did not land is still a failure, which is what stops the new arm swallowing a real defect.
    r.nameMatches    = true;
    r.loadoutRefusal = identity_access::Refusal::None;
    CHECK(!identity::Verdict(r));
    r.loadoutRefusal = identity_access::Refusal::ShortInstance;
    CHECK(!identity::Verdict(r));
}

TEST_CASE("Dress stops at the first source the engine cannot be shown to read") {
    FakeIdentity fake;
    PlayerRecord rec{};
    rec.userSlot = 1;
    rec.teamId = 0;
    rec.loadoutNumber = 17;
    rec.loadoutNumberBody = 42;
    memcpy(rec.displayName, "player02", 7);
    FillDistinct(rec.loadoutSlots);
    memcpy(fake.applied, rec.loadoutSlots, sizeof fake.applied);  // perfect: nothing else can fail

    identity::DressResult out{};
    const bool ok = identity::Dress(fake, rec, out);

    // A foreign IIdentity reaches no engine, so the slot-table write is the first step that
    // can tell: it declines, Dress stops there and names the stage.  What this pins is that the
    // step exists and is on the mandatory path -- it was absent entirely before, and the name
    // then rested on the one source every live gate routes around.
    CHECK(!ok);
    CHECK(strcmp(out.stage, "slotname") == 0);
    CHECK(out.refusal == identity_access::Refusal::NoSlotNameTable);
    CHECK(!out.ok);
    CHECK(!out.nameMatches);
    CHECK(!out.nameResolved);
    CHECK(out.nameShown[0] == '\0');
    CHECK(out.refusal != identity_access::Refusal::None);
    // ...and it named the step rather than returning a bare false.
    CHECK(strcmp(out.stage, "start") != 0);
    CHECK(out.detail[0] != '\0');
}

TEST_CASE("a name that resolved to the wrong string still names itself") {
    // The two ways the name arm can fail read differently in the log, and neither may be blank:
    // "could not ask" is a refusal, "asked and got Unknown" is not.  Before this, the second
    // case reached DressAndReport with an empty reason string.
    identity::DressResult r{};
    r.items.compared = 20;
    r.nameResolved = true;
    r.nameMatches = false;
    strcpy_s(r.nameShown, sizeof r.nameShown, "Unknown");
    CHECK(!identity::Verdict(r));
    // ...and the string the player sees is carried out, not just a bool.
    CHECK(strcmp(r.nameShown, "Unknown") == 0);
}

namespace {

class RecordingLog final : public ILog {
public:
    static constexpr uint32_t kMax = 32;
    char     lines[kMax][512];
    LogLevel levels[kMax];
    uint32_t n = 0;

    void Write(LogLevel level, const char*, const char* fmt, ...) override {
        if (n >= kMax) return;
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(lines[n], sizeof lines[n], _TRUNCATE, fmt, args);
        va_end(args);
        levels[n] = level;
        ++n;
    }
    bool Enabled(LogLevel) const override { return true; }
    void DumpLedger() override {}

    uint32_t Count(const char* needle) const {
        uint32_t hits = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (strstr(lines[i], needle)) ++hits;
        return hits;
    }

    // The level of the line that says a particular thing, rather than of whichever line happens to
    // be last: an assertion keyed on position goes stale the moment a summary gains a sibling.
    LogLevel LevelOf(const char* needle) const {
        for (uint32_t i = 0; i < n; ++i)
            if (strstr(lines[i], needle)) return levels[i];
        return LogLevel::Info;
    }
};

}  // namespace

TEST_CASE("DressRoster dresses every recorded player and reports the count, not the first refusal") {
    // Offline, the slot-name table cannot be shown to exist, so every player refuses at the same
    // stage. What the roster must still prove is that it VISITED all of them: the second and third
    // players are named in the log, and the summary counts three, not one.
    FakeIdentity fake;
    PlayerRecord recs[3] = {};
    const char* names[3] = {"player02", "Liara", "Garrus"};
    for (uint32_t i = 0; i < 3; ++i) {
        recs[i].userSlot = uint16_t(i + 1);
        memcpy(recs[i].displayName, names[i], strlen(names[i]) + 1);
        FillDistinct(recs[i].loadoutSlots);
    }

    RecordingLog log;
    identity::RosterReport report{};
    identity::RosterMapping map;
    const bool complete = identity::DressRoster(fake, log, recs, 3, report, map);

    CHECK(!complete);
    CHECK(report.players == 3);
    CHECK(report.dressed == 0);
    CHECK(report.failed == 3);
    CHECK(report.dressed + report.failed == report.players);
    for (const char* name : names) {
        INFO("player ", name);
        CHECK(log.Count(name) >= 1);
    }
    CHECK(log.Count("NOT dressed: stage=slotname") == 3);
    CHECK(log.Count("roster: 0/3 players dressed, 3 NOT dressed") == 1);
    REQUIRE(log.n > 0);
    CHECK(log.LevelOf("players dressed") == LogLevel::Error);
    // Ownership is reported beside dressing and separately from it: this fake resolves every
    // entrant, so all three are mapped even though none is dressed, and that is not a warning.
    CHECK(log.Count("roster: 3/3 players mapped to a live user index") == 1);
    CHECK(log.LevelOf("players mapped") == LogLevel::Info);

    // An empty roster is complete and says so at Info: nothing failed, nothing was asked.
    identity::RosterReport none{};
    RecordingLog quiet;
    identity::RosterMapping nomap;
    CHECK(identity::DressRoster(fake, quiet, nullptr, 0, none, nomap));
    CHECK(none.players == 0);
    CHECK(quiet.Count("roster: 0/0 players dressed") == 1);
    CHECK(quiet.Count("roster: 0/0 players mapped") == 1);
    REQUIRE(quiet.n == 2);
    CHECK(quiet.levels[0] == LogLevel::Info);
    CHECK(quiet.levels[1] == LogLevel::Info);
}

namespace {

// Hands out a DIFFERENT index per slot, which the shared FakeIdentity cannot: with one constant
// index a mapping that ignored the slot entirely would pass every assertion below.
class SlottedIdentity : public IIdentity {   // not final: the late-leave test specialises Release
public:
    uint16_t releaseCalls = 0;
    uint16_t releasedSlots[8] = {};
    bool     releaseAnswer = false;

    uint16_t provisionedSlot = identity::kNotDressed;

    bool AddReplayUser(UserSlot s, UserIndex& out) override {
        out = UserIndex(uint16_t(s.v * 3 + 1));   // slot 2 -> 7, slot 4 -> 13
        return true;
    }
    bool ProvisionReplayUser(UserSlot s, const JoinDesc&, UserIndex& out) override {
        provisionedSlot = s.v;
        return AddReplayUser(s, out);
    }
    bool ReleaseReplayUser(UserSlot s) override {
        if (releaseCalls < 8) releasedSlots[releaseCalls] = s.v;
        ++releaseCalls;
        return releaseAnswer;
    }
    bool SetProfileJson(UserSlot, const Blob&) override { return true; }
    bool SetTeam(UserIndex, uint8_t, uint8_t) override { return true; }
    bool ApplyLoadout(UserIndex, const Symbol64[20], uint16_t, uint16_t) override { return true; }
    bool OverrideAlignment(UserIndex, uint16_t) override { return true; }
    bool ReadAppliedItems(UserIndex, Symbol64 out[20]) const override {
        memset(out, 0, sizeof(Symbol64) * 20);
        return true;
    }
    UserSlot  SlotOf(UserIndex) const override { return UserSlot(provisionedSlot); }
    UserIndex IndexOf(UserSlot s) const override { return UserIndex(uint16_t(s.v * 3 + 1)); }
};

}  // namespace

TEST_CASE("DressRoster carries the live user index out per slot, and a missing one is not index 0") {
    // THE SEAM. Without it PlayerTrack::user_id is never resolved, every body publish counts
    // players_unmapped, and the replayed world stands still with the entity, netvar and dressing
    // paths all green.
    SlottedIdentity id;
    RecordingLog log;
    PlayerRecord recs[2] = {};
    recs[0].userSlot = 2;
    recs[1].userSlot = 4;
    memcpy(recs[0].displayName, "player02", 7);
    memcpy(recs[1].displayName, "Liara", 6);

    identity::RosterReport report{};
    identity::RosterMapping map;
    identity::DressRoster(id, log, recs, 2, report, map);

    // Dressing fails offline (the slot-name table cannot be shown to exist), and the mapping is
    // still complete: an entrant wearing the wrong chassis is still that entrant. These are two
    // verdicts and collapsing them would freeze the world over a cosmetic refusal.
    CHECK(report.dressed == 0);
    CHECK(map.Mapped() == 2);

    UserIndex got(0);
    REQUIRE(map.Lookup(2, got));
    CHECK(got.v == 7);
    REQUIRE(map.Lookup(4, got));
    CHECK(got.v == 13);
    CHECK(log.Count("roster: 2/2 players mapped to a live user index") == 1);

    // Every slot nobody dressed reads as kNotDressed, never as index 0. Index 0 is a real engine
    // user index, so a zero-filled table that "defaulted" to it would hand W2-B a confident wrong
    // owner for every player in the file.
    CHECK_FALSE(map.Lookup(0, got));
    CHECK(map.IndexAt(0) == identity::kNotDressed);
    CHECK(identity::kNotDressed != 0);
    CHECK(map.IndexAt(3) == identity::kNotDressed);
}

TEST_CASE("the release authors the body the engine's own callback reads, and refuses honestly") {
    // The client's EntrantRemovedCB reads the slot at body+0 and its ListenProxy DROPS anything
    // shorter than 16 bytes. The body is decoded here by W3-E's shipped decoder rather than by a
    // second reader written next to the writer -- a writer holding its own copy of a format is the
    // defect that lets a checker certify broken bytes, from the other side.
    uint8_t body[identity_access::kEntrantRemovedBodyLen];
    REQUIRE(identity_access::BuildEntrantRemovedBody(UserSlot(9), body));
    uint64_t slot = 0;
    REQUIRE(events::DecodeEntrantRemoved(body, sizeof body, &slot));
    CHECK(slot == 9u);
    CHECK_FALSE(events::DecodeEntrantRemoved(body, 8, &slot));   // the length is load-bearing

    SlottedIdentity id;
    RecordingLog log;
    PlayerRecord recs[2] = {};
    recs[0].userSlot = 2;
    recs[1].userSlot = 4;
    identity::RosterReport report{};
    identity::RosterMapping map;
    identity::DressRoster(id, log, recs, 2, report, map);
    REQUIRE(map.Mapped() == 2);

    RecordingLog rlog;
    identity::ForgetRosterLeave();
    identity::ReleaseReport rel{};
    identity::ReleaseRoster(id, rlog, map, rel);

    // The unload CALLS NOTHING: the engine's removing-entrant handler needs the engine
    // thread and a live netGame, and this thread has neither. It queues, and the stage-3 tail feeds.
    CHECK(id.releaseCalls == 0);
    CHECK(rel.mapped == 2);
    CHECK(identity::RosterLeavePending());

    // Every queued slot is offered back, in slot order, and each refusal names itself. The feed is
    // driven to its own attempt bound, which is where a slot that will not leave is reported.
    for (int pass = 0; pass < 12 && identity::RosterLeavePending(); ++pass)
        identity::ServiceRosterLeave(id, rlog, map);
    CHECK_FALSE(identity::RosterLeavePending());
    CHECK(id.releasedSlots[0] == 2);
    CHECK(id.releasedSlots[1] == 4);
    CHECK(identity::RosterLeaveTotals().left == 0);
    CHECK(identity::RosterLeaveTotals().refused == 2);
    CHECK(rlog.Count("did NOT leave after") == 2);
    CHECK(rlog.Count("roster: 0/2 replay entrant(s) left the engine, 2 refused") == 1);
    // A refused leave KEEPS its record of which engine slot the entrant is in: that is what the next
    // load adopts instead of admitting a second entrant for the same player, and it is the fallback
    // the shipped adopt path already is.
    CHECK(map.ResidueAt(2).slotResolved == 2);
    CHECK(map.ResidueAt(4).slotResolved == 4);

    // And the mapping is empty whatever the engine said: an index this session cannot hand back
    // is still an index it must not reuse on the next load.
    CHECK(map.Mapped() == 0);
    UserIndex got(0);
    CHECK_FALSE(map.Lookup(2, got));

    // playback.leave_on_unload off: the unload queues NOTHING, nothing is ever fed, and the residue
    // stands so the next load adopts -- the shipped behaviour, reachable from config.
    {
        identity::RosterMapping m2;
        identity::RosterReport r2{};
        identity::DressRoster(id, log, recs, 2, r2, m2);
        REQUIRE(m2.Mapped() == 2);
        identity::ForgetRosterLeave();
        identity::SetLeaveOnUnload(false);
        RecordingLog offLog;
        identity::ReleaseReport off{};
        const uint32_t callsBefore = id.releaseCalls;
        identity::ReleaseRoster(id, offLog, m2, off);
        CHECK_FALSE(identity::RosterLeavePending());
        CHECK(id.releaseCalls == callsBefore);
        CHECK(off.mapped == 2);
        CHECK(m2.ResidueAt(2).slotResolved == 2);       // kept, so the next load re-binds to it
        CHECK(offLog.Count("leave_on_unload is off") == 1);
        identity::SetLeaveOnUnload(true);
        CHECK(identity::LeaveOnUnload());
    }

    // A release the engine ACCEPTS is counted as one, so "0 left" can fail -- and it takes the slot
    // record with it, because the slot is now free for a fresh admission rather than an adoption.
    id.releaseAnswer = true;
    id.releaseCalls = 0;
    identity::ForgetRosterLeave();
    identity::LeaveReport again{};
    identity::RequestRosterLeave(map, again);
    CHECK(again.queued == 2);
    identity::ServiceRosterLeave(id, rlog, map);
    CHECK(id.releaseCalls == 2);      // one pass, not one per attempt: a slot that left is dropped
    CHECK(identity::RosterLeaveTotals().left == 2);
    CHECK(identity::RosterLeaveTotals().refused == 0);
    CHECK(map.ResidueAt(2).slotResolved == identity::kNotDressed);
    CHECK(map.ResidueAt(4).slotResolved == identity::kNotDressed);
    CHECK_FALSE(identity::RosterLeavePending());
    identity::ForgetRosterLeave();
}

TEST_CASE("the leave feed queues one leave per ENGINE slot and drops it the pass it leaves") {
    // Two capture slots that resolved to ONE engine entrant -- which adoption produces across loads
    // -- must queue ONE leave. Two would feed the engine a removal for a slot it already freed, and
    // the second would land on whatever the engine put there next.
    SlottedIdentity id;
    RecordingLog log;
    identity::RosterMapping map;
    identity::IdentityFields wrote{};
    wrote.slotName = true;
    map.NoteDressed(1, 5, 11, wrote);
    map.NoteDressed(3, 5, 11, wrote);   // same engine slot
    map.NoteDressed(2, 9, 20, wrote);

    identity::LeaveReport q{};
    identity::ForgetRosterLeave();
    identity::RequestRosterLeave(map, q);
    CHECK(q.queued == 2);               // engine slots 5 and 9, not three capture slots

    id.releaseAnswer = true;
    identity::ServiceRosterLeave(id, log, map);
    CHECK(id.releaseCalls == 2);
    CHECK(id.releasedSlots[0] == 5);
    CHECK(id.releasedSlots[1] == 9);
    CHECK_FALSE(identity::RosterLeavePending());
    identity::ForgetRosterLeave();
}

// The two ordering levers, and what they must NOT change: the same engine slots go,
// through the same call, and nothing is left queued.  The unload's engine free leaves one pooled
// actor record un-re-keyed and always the same entry, and "the sixth one freed" and "entry 5" have
// never been separated -- these are how that separation is driven without an engine write.
TEST_CASE("the leave feed can walk the slots from the top down and one per pass") {
    SlottedIdentity id;
    RecordingLog log;
    identity::RosterMapping map;
    identity::IdentityFields wrote{};
    wrote.slotName = true;
    map.NoteDressed(0, 1, 11, wrote);
    map.NoteDressed(1, 4, 12, wrote);
    map.NoteDressed(2, 7, 13, wrote);
    id.releaseAnswer = true;

    SUBCASE("reversed, the highest engine slot leaves first and all three still go in one pass") {
        identity::SetLeaveOrder(true, false);
        identity::LeaveReport q{};
        identity::ForgetRosterLeave();
        identity::RequestRosterLeave(map, q);
        CHECK(q.queued == 3);
        identity::ServiceRosterLeave(id, log, map);
        CHECK(id.releaseCalls == 3);
        CHECK(id.releasedSlots[0] == 7);
        CHECK(id.releasedSlots[1] == 4);
        CHECK(id.releasedSlots[2] == 1);
        CHECK_FALSE(identity::RosterLeavePending());
        CHECK(identity::RosterLeaveTotals().left == 3);
    }

    SUBCASE("one per pass, each engine update takes exactly one and the rest stay queued") {
        identity::SetLeaveOrder(false, true);
        identity::LeaveReport q{};
        identity::ForgetRosterLeave();
        identity::RequestRosterLeave(map, q);
        identity::ServiceRosterLeave(id, log, map);
        CHECK(id.releaseCalls == 1);
        CHECK(id.releasedSlots[0] == 1);
        CHECK(identity::RosterLeavePending());
        identity::ServiceRosterLeave(id, log, map);
        CHECK(id.releaseCalls == 2);
        CHECK(id.releasedSlots[1] == 4);
        identity::ServiceRosterLeave(id, log, map);
        CHECK(id.releaseCalls == 3);
        CHECK(id.releasedSlots[2] == 7);
        CHECK_FALSE(identity::RosterLeavePending());
        CHECK(identity::RosterLeaveTotals().left == 3);
    }

    SUBCASE("both together: one per pass, from the top down") {
        identity::SetLeaveOrder(true, true);
        identity::LeaveReport q{};
        identity::ForgetRosterLeave();
        identity::RequestRosterLeave(map, q);
        identity::ServiceRosterLeave(id, log, map);
        CHECK(id.releaseCalls == 1);
        CHECK(id.releasedSlots[0] == 7);
        identity::ServiceRosterLeave(id, log, map);
        CHECK(id.releasedSlots[1] == 4);
        identity::ServiceRosterLeave(id, log, map);
        CHECK(id.releasedSlots[2] == 1);
        CHECK_FALSE(identity::RosterLeavePending());
    }

    identity::SetLeaveOrder(false, false);
    CHECK_FALSE(identity::LeaveReverseOrder());
    CHECK_FALSE(identity::LeaveOnePerPass());
    identity::ForgetRosterLeave();
}

TEST_CASE("a slot that leaves late is retried, and the retries are bounded") {
    // The engine answers no for the first three passes and yes on the fourth: the feed must keep
    // asking and must NOT report a refusal for a slot that did leave.
    class LateIdentity final : public SlottedIdentity {
    public:
        uint32_t calls = 0;
        bool ReleaseReplayUser(UserSlot s) override {
            ++calls;
            lastSlot = s.v;
            return calls >= 4;
        }
        uint16_t lastSlot = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    };
    LateIdentity id;
    RecordingLog log;
    identity::RosterMapping map;
    identity::IdentityFields wrote{};
    wrote.slotName = true;
    map.NoteDressed(0, 7, 14, wrote);

    identity::LeaveReport q{};
    identity::ForgetRosterLeave();
    identity::RequestRosterLeave(map, q);
    for (int pass = 0; pass < 12 && identity::RosterLeavePending(); ++pass)
        identity::ServiceRosterLeave(id, log, map);
    CHECK(id.calls == 4);
    CHECK(id.lastSlot == 7);
    CHECK(identity::RosterLeaveTotals().left == 1);
    CHECK(identity::RosterLeaveTotals().refused == 0);
    CHECK(map.ResidueCount() == 0);
    CHECK(log.Count("LEFT: engine slot 7 (capture slot 0)") == 1);

    // ...and a slot that never leaves stops being asked, because a queue that never empties holds
    // the next load's dress pass off the roster for the rest of the session.
    SlottedIdentity never;                      // releaseAnswer defaults false
    identity::RosterMapping map2;
    map2.NoteDressed(0, 7, 14, wrote);
    identity::LeaveReport q2{};
    identity::ForgetRosterLeave();
    identity::RequestRosterLeave(map2, q2);
    for (int pass = 0; pass < 40 && identity::RosterLeavePending(); ++pass)
        identity::ServiceRosterLeave(never, log, map2);
    CHECK(never.releaseCalls == 8);
    CHECK_FALSE(identity::RosterLeavePending());
    CHECK(identity::RosterLeaveTotals().refused == 1);
    identity::ForgetRosterLeave();
}

namespace {

// A fake that records what each write LANDED, keyed by the slot the engine would have written --
// so an assertion can ask "what is slot 4 wearing now", which is the question the vacate exists to
// answer.  The index is slot-dependent (3s+1) for the same reason the seam's fake is: a vacate that
// wrote every field at a constant index would pass against a constant-index fake.
class VacateFake final : public IIdentity {
public:
    static constexpr size_t kSlots = identity::RosterMapping::kSlots;

    bool     present[kSlots];
    bool     refuseProfileFor[kSlots] = {};
    bool     refuseAddFor[kSlots] = {};   // the entrant exists and resolves, but will not be added
    // slot -> index is `3s + indexBase`.  The base moves because an entity re-bind reissues the
    // engine's indices: a residue entry's recorded index belongs to the game space that dressed it.
    uint16_t indexBase = 1;
    uint16_t lastTeamIndex[kSlots] = {}, lastLoadoutIndex[kSlots] = {}, lastAlignIndex[kSlots] = {};
    char     lastProfile[kSlots][160] = {};
    uint32_t profileWrites[kSlots] = {};
    uint16_t lastTeam[kSlots] = {}, lastTeamSlot[kSlots] = {};
    uint32_t teamWrites[kSlots] = {};
    Symbol64 lastItems[kSlots][20] = {};
    uint16_t lastNumber[kSlots] = {}, lastNumberBody[kSlots] = {};
    uint32_t loadoutWrites[kSlots] = {};
    uint16_t lastAlignment[kSlots] = {};
    uint32_t alignWrites[kSlots] = {};

    VacateFake() { for (size_t i = 0; i < kSlots; ++i) present[i] = true; }

    static uint16_t IndexFor(uint16_t slot, uint16_t base = 1) { return uint16_t(slot * 3 + base); }

    // Exact, not a rounded-down division: an index this game space did not issue belongs to nobody,
    // and the write is refused rather than silently attributed to a neighbouring slot.
    uint16_t SlotFor(uint16_t index) const {
        if (index < indexBase || (index - indexBase) % 3 != 0) return uint16_t(kSlots);
        const uint16_t s = uint16_t((index - indexBase) / 3);
        return s < kSlots ? s : uint16_t(kSlots);
    }

    bool AddReplayUser(UserSlot s, UserIndex& out) override {
        if (s.v >= kSlots || !present[s.v] || refuseAddFor[s.v]) return false;
        out = UserIndex(IndexFor(s.v, indexBase));
        return true;
    }
    // Admits where it is asked; SlotOf is this fake's own inverse, so the dress resolves back to
    // the recorded slot and every residue expectation below is about the same slot it always was.
    bool ProvisionReplayUser(UserSlot s, const JoinDesc&, UserIndex& out) override {
        return AddReplayUser(s, out);
    }
    bool SetProfileJson(UserSlot s, const Blob& json) override {
        if (s.v >= kSlots || refuseProfileFor[s.v]) return false;
        const uint32_t cap = uint32_t(sizeof lastProfile[0] - 1);
        const uint32_t n = json.n < cap ? json.n : cap;
        memcpy(lastProfile[s.v], json.p, n);
        lastProfile[s.v][n] = '\0';
        ++profileWrites[s.v];
        return true;
    }
    bool SetTeam(UserIndex i, uint8_t team, uint8_t teamSlot) override {
        const uint16_t s = SlotFor(i.v);
        if (s >= kSlots) return false;
        lastTeam[s] = team;
        lastTeamSlot[s] = teamSlot;
        lastTeamIndex[s] = i.v;
        ++teamWrites[s];
        return true;
    }
    bool ApplyLoadout(UserIndex i, const Symbol64 slots[20], uint16_t number,
                      uint16_t numberBody) override {
        const uint16_t s = SlotFor(i.v);
        if (s >= kSlots) return false;
        memcpy(lastItems[s], slots, sizeof lastItems[0]);
        lastNumber[s] = number;
        lastNumberBody[s] = numberBody;
        lastLoadoutIndex[s] = i.v;
        ++loadoutWrites[s];
        return true;
    }
    bool OverrideAlignment(UserIndex i, uint16_t alignment) override {
        const uint16_t s = SlotFor(i.v);
        if (s >= kSlots) return false;
        lastAlignment[s] = alignment;
        lastAlignIndex[s] = i.v;
        ++alignWrites[s];
        return true;
    }
    bool ReadAppliedItems(UserIndex, Symbol64 out[20]) const override {
        memset(out, 0, sizeof(Symbol64) * 20);
        return true;
    }
    UserSlot  SlotOf(UserIndex i) const override { return UserSlot(SlotFor(i.v)); }
    UserIndex IndexOf(UserSlot s) const override {
        return UserIndex(s.v < kSlots && present[s.v] ? IndexFor(s.v, indexBase)
                                                      : identity::kNotDressed);
    }
};

const char kProfileA[] = "{\"displayname\":\"player02\",\"stats\":{\"level\":7}}";
const char kProfileB[] = "{\"displayname\":\"Wrex\",\"stats\":{\"level\":3}}";

void MakeRecord(PlayerRecord& r, uint16_t slot, const char* name, const char* json) {
    memset(&r, 0, sizeof r);
    r.userSlot = slot;
    Copy(r.displayName, sizeof r.displayName, name);
    r.entrantJson = Blob{reinterpret_cast<const uint8_t*>(json), uint32_t(strlen(json))};
    r.teamId = 1;
    r.teamSlot = 2;
    r.alignment = 3;
    r.loadoutNumber = 11;
    r.loadoutNumberBody = 22;
    FillDistinct(r.loadoutSlots);
}

// What the engine was asked to store, read back with the SHIPPED profile walk rather than compared
// as a string: the neutral document is only correct if the engine's own scalar walk turns it into
// the one leaf UserDisplayName reads, set to empty.
bool ProfileIsVacant(const char* json) {
    Recorder rec;
    const identity_access::ProfileWalkStats w = Walk(rec, json);
    if (!w.wellFormed || rec.n != 1) return false;
    const int i = rec.Find("displayname");
    return i == 0 && !rec.isInt[0] && rec.strings[0][0] == '\0';
}

}  // namespace

TEST_CASE("a second load vacates the slots the first one dressed and this one does not use") {
    // RULING 4a. ReleaseReplayUser refuses on this build, so nothing hands the entrant back -- and
    // because AddReplayUser allocates nothing, what survives is not memory but IDENTITY. Without
    // this, replay B plays slot 2 wearing replay A's name, loadout and colours: a wrong replay that
    // looks right.
    VacateFake id;
    identity::RosterMapping map;
    identity::RosterReport a{};
    RecordingLog logA;
    PlayerRecord recsA[2];
    MakeRecord(recsA[0], 2, "player02", kProfileA);
    MakeRecord(recsA[1], 4, "Liara", kProfileA);
    identity::DressRoster(id, logA, recsA, 2, a, map);
    CHECK(a.residue == 0);          // nothing to owe on the first load of a session
    CHECK(a.vacated == 0);
    CHECK(logA.Count("residue slot") == 0);
    REQUIRE(map.ResidueCount() == 2);

    // Unload records the residue and undresses NOTHING: there is no later observation point in this
    // session, and the writes belong where both rosters are known.
    RecordingLog logRel;
    identity::ReleaseReport rel{};
    identity::ForgetRosterLeave();
    identity::ReleaseRoster(id, logRel, map, rel);
    // This fake's ReleaseReplayUser refuses, which is the FALLBACK arm -- the engine
    // would not free the slots, so the residue survives and the next load adopts it, exactly as the
    // shipped build did before the leave path existed. That is what this case pins.
    for (int pass = 0; pass < 12 && identity::RosterLeavePending(); ++pass)
        identity::ServiceRosterLeave(id, logRel, map);
    CHECK(identity::RosterLeaveTotals().refused == 2);
    CHECK(rel.residue == 2);
    CHECK(map.ResidueCount() == 2);
    CHECK(id.profileWrites[2] == 1);        // still exactly the dress write
    identity::ForgetRosterLeave();

    RecordingLog logB;
    identity::RosterReport b{};
    PlayerRecord recsB[1];
    MakeRecord(recsB[0], 6, "Wrex", kProfileB);
    identity::DressRoster(id, logB, recsB, 1, b, map);

    CHECK(b.residue == 2);
    CHECK(b.vacated == 2);
    CHECK(b.staleSlot == 0);
    CHECK(b.vacateRefused == 0);
    CHECK(logB.Count("roster: 2 residue slot(s) from an earlier load, 2 vacated, 0 stale-slot, "
                     "0 vacate-refused, 0 dress-refused") == 1);

    // The observable: what slots 2 and 4 are wearing NOW is the engine's no-user state, and it was
    // written through the engine calls, not merely dropped from our own table.
    CHECK(id.profileWrites[2] == 2);
    CHECK(id.profileWrites[4] == 2);
    CHECK(ProfileIsVacant(id.lastProfile[2]));
    CHECK(ProfileIsVacant(id.lastProfile[4]));
    CHECK(strstr(id.lastProfile[2], "player02") == nullptr);
    CHECK(map.ResidueCount() == 1);         // A's two are discharged; B's own slot took their place

    // B's own slot carries B, and was never vacated.
    CHECK(id.profileWrites[6] == 1);
    CHECK_FALSE(ProfileIsVacant(id.lastProfile[6]));
    CHECK(logB.Count("residue slot 6") == 0);

    // ...and it becomes the next load's obligation in turn.
    RecordingLog logC;
    identity::RosterReport c{};
    identity::DressRoster(id, logC, nullptr, 0, c, map);
    CHECK(c.residue == 1);
    CHECK(c.vacated == 1);
    CHECK(ProfileIsVacant(id.lastProfile[6]));
    CHECK(map.ResidueCount() == 0);
}

TEST_CASE("a residue slot the new roster does dress is overwritten, never vacated") {
    // The other half of the whole-domain obligation, and the one a blanket "undress everything the
    // last replay touched" would get wrong: it would run after the dress and strip the new player.
    VacateFake id;
    identity::RosterMapping map;
    RecordingLog log;
    identity::RosterReport a{};
    PlayerRecord recsA[2];
    MakeRecord(recsA[0], 2, "player02", kProfileA);
    MakeRecord(recsA[1], 4, "Liara", kProfileA);
    identity::DressRoster(id, log, recsA, 2, a, map);

    RecordingLog logB;
    identity::RosterReport b{};
    PlayerRecord recsB[1];
    MakeRecord(recsB[0], 2, "Wrex", kProfileB);      // the SAME slot A used
    identity::DressRoster(id, logB, recsB, 1, b, map);

    CHECK(b.residue == 2);
    CHECK(b.vacated == 1);                            // slot 4 only
    CHECK(logB.Count("residue slot 4 vacated") == 1);
    CHECK(logB.Count("residue slot 2") == 0);
    CHECK(ProfileIsVacant(id.lastProfile[4]));
    CHECK_FALSE(ProfileIsVacant(id.lastProfile[2]));
    CHECK(strstr(id.lastProfile[2], "Wrex") != nullptr);
}

TEST_CASE("a residue slot whose entrant is gone is stale, and one that refuses is named and kept") {
    VacateFake id;
    identity::RosterMapping map;
    RecordingLog log;
    identity::RosterReport a{};
    PlayerRecord recsA[2];
    MakeRecord(recsA[0], 2, "player02", kProfileA);
    MakeRecord(recsA[1], 4, "Liara", kProfileA);
    identity::DressRoster(id, log, recsA, 2, a, map);

    // Slot 2's entrant went away with a netGame teardown; slot 4's is there but will not take the
    // write.  Two different facts, and a single "failed" count could not tell them apart.
    id.present[2] = false;
    id.refuseProfileFor[4] = true;

    RecordingLog logB;
    identity::RosterReport b{};
    identity::DressRoster(id, logB, nullptr, 0, b, map);

    CHECK(b.residue == 2);
    CHECK(b.vacated == 0);
    CHECK(b.staleSlot == 1);
    CHECK(b.vacateRefused == 1);
    CHECK(logB.Count("residue slot 2: the entrant no longer exists") == 1);
    CHECK(logB.Count("residue slot 4 NOT vacated (vacate_refused:") == 1);
    CHECK(logB.LevelOf("NOT vacated") == LogLevel::Error);
    CHECK(logB.LevelOf("the entrant no longer exists") == LogLevel::Info);

    // The stale one is discharged; the refused one is KEPT, because it is still wearing A.
    CHECK(map.ResidueCount() == 1);
    RecordingLog logC;
    identity::RosterReport c{};
    id.refuseProfileFor[4] = false;
    identity::DressRoster(id, logC, nullptr, 0, c, map);
    CHECK(c.residue == 1);
    CHECK(c.vacated == 1);
    CHECK(ProfileIsVacant(id.lastProfile[4]));
    CHECK(map.ResidueCount() == 0);
}

TEST_CASE("the vacate writes the engine's no-user state into every field the dress wrote") {
    // Driven directly, because offline the dress stops at the slot-name table and never reaches the
    // team, loadout or alignment steps -- so a test that only went through DressRoster would leave
    // four of the five vacate writes unexercised.
    VacateFake id;
    RecordingLog log;
    identity::RosterMapping::ResidueEntry e{};
    e.used = true;
    e.userIndex = VacateFake::IndexFor(5);
    e.slotResolved = 5;   // where the dress admitted it; the vacate writes address THIS slot
    e.wrote.profile = true;
    e.wrote.team = true;
    e.wrote.loadout = true;
    e.wrote.alignment = true;

    CHECK(identity::VacateSlot(id, log, 5, e) == identity::VacateOutcome::Vacated);

    CHECK(ProfileIsVacant(id.lastProfile[5]));
    CHECK(id.teamWrites[5] == 1);
    // The first team id the engine's own alignment table does not cover, taken from the registry.
    CHECK(id.lastTeam[5] == facts::IDENT_ALIGN_TEAM_LIMIT);
    CHECK(id.lastTeamSlot[5] == 0);
    CHECK(id.loadoutWrites[5] == 1);
    for (uint32_t i = 0; i < 20; ++i) CHECK(id.lastItems[5][i] == 0);
    CHECK(id.lastNumber[5] == 0);
    CHECK(id.lastNumberBody[5] == 0);
    CHECK(id.alignWrites[5] == 1);
    CHECK(id.lastAlignment[5] == 0xFFFF);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(log.Count("cannot be removed") == 1);   // the partial half, said out loud every time

    // A field this session never wrote is not written now: vacating it would undress an entrant we
    // never dressed.
    VacateFake untouched;
    identity::RosterMapping::ResidueEntry only{};
    only.used = true;
    only.slotResolved = 5;
    only.wrote.profile = true;
    CHECK(identity::VacateSlot(untouched, log, 5, only) == identity::VacateOutcome::Vacated);
    CHECK(untouched.profileWrites[5] == 1);
    CHECK(untouched.teamWrites[5] == 0);
    CHECK(untouched.loadoutWrites[5] == 0);
    CHECK(untouched.alignWrites[5] == 0);

    // The slot-name half cannot be written without a live CR15NetGame, and that is a refusal that
    // names itself rather than a vacate reporting a write it did not make: the name is the field
    // the player actually reads off the screen.
    RecordingLog nameLog;
    VacateFake named;
    identity::RosterMapping::ResidueEntry withName{};
    withName.used = true;
    withName.slotResolved = 5;
    withName.wrote.profile = true;
    withName.wrote.slotName = true;
    CHECK(identity::VacateSlot(named, nameLog, 5, withName) == identity::VacateOutcome::Refused);
    CHECK(nameLog.Count("vacate_refused:") == 1);
    CHECK(nameLog.Count("at step slotname") == 1);
    // ...and the profile write it COULD do was still made: less of the previous player on screen
    // beats none of the vacate at all.
    CHECK(ProfileIsVacant(named.lastProfile[5]));
}

TEST_CASE("the re-dress of the same manifest never vacates a player of the replay now playing") {
    // W2-B re-runs DressRoster with the SAME manifest on an entity re-bind.  AddReplayUser can
    // refuse on that second pass for an entrant that still exists -- so the slot still resolves, and
    // a vacate keyed on "did this pass write a field" would write the no-user state over a player
    // belonging to the replay that is playing right now.  The manifest names the slot; that is what
    // makes it not residue.
    VacateFake id;
    identity::RosterMapping map;
    RecordingLog logA;
    identity::RosterReport a{};
    PlayerRecord recs[2];
    MakeRecord(recs[0], 2, "player02", kProfileA);
    MakeRecord(recs[1], 4, "Liara", kProfileB);
    identity::DressRoster(id, logA, recs, 2, a, map);
    REQUIRE(map.ResidueCount() == 2);
    REQUIRE(id.profileWrites[4] == 1);

    id.refuseAddFor[4] = true;                 // still present, still resolves, will not be added
    REQUIRE(id.IndexOf(UserSlot(uint16_t(4))).v != identity::kNotDressed);

    RecordingLog logB;
    identity::RosterReport b{};
    identity::DressRoster(id, logB, recs, 2, b, map);

    CHECK(b.residue == 2);
    CHECK(b.vacated == 0);
    CHECK(b.staleSlot == 0);
    CHECK(b.vacateRefused == 0);
    CHECK(b.dressRefused == 1);
    CHECK(logB.Count("residue slot 4 vacated") == 0);
    CHECK(logB.Count("residue slot 4 is named by THIS manifest") == 1);
    CHECK(logB.LevelOf("is named by THIS manifest") == LogLevel::Error);
    CHECK(logB.Count("roster: 2 residue slot(s) from an earlier load, 0 vacated, 0 stale-slot, "
                     "0 vacate-refused, 1 dress-refused") == 1);

    // The observable: slot 4 is untouched by this pass and still wears the replay's own player.
    CHECK(id.profileWrites[4] == 1);
    CHECK_FALSE(ProfileIsVacant(id.lastProfile[4]));
    CHECK(strstr(id.lastProfile[4], "Wrex") != nullptr);
    // Slot 2 re-dressed normally, and was not vacated either.
    CHECK(id.profileWrites[2] == 2);
    CHECK_FALSE(ProfileIsVacant(id.lastProfile[2]));
    CHECK(map.ResidueCount() == 2);            // both slots are still this replay's to own

    // ...and once the manifest stops naming slot 4, the obligation does reach it.
    RecordingLog logC;
    identity::RosterReport c{};
    identity::DressRoster(id, logC, nullptr, 0, c, map);
    CHECK(c.dressRefused == 0);
    CHECK(c.vacated == 2);
    CHECK(ProfileIsVacant(id.lastProfile[4]));
}

TEST_CASE("the vacate writes at the index the slot resolves to NOW, not the recorded one") {
    // The residue's recorded index belonged to the game space that dressed it.  An entity re-bind
    // between the two loads reissues every index, so the recorded one addresses another entrant --
    // or nothing.  The fake re-binds here (indexBase 1 -> 2), which is the only way this fixture can
    // tell a re-resolved index from a remembered one.
    VacateFake id;
    identity::RosterMapping map;
    identity::IdentityFields wrote{};
    wrote.profile = wrote.team = wrote.loadout = wrote.alignment = true;
    map.NoteDressed(4, 4, VacateFake::IndexFor(4), wrote);
    map.NoteDressed(2, 2, VacateFake::IndexFor(2), wrote);
    REQUIRE(map.ResidueAt(4).userIndex == VacateFake::IndexFor(4));

    id.indexBase = 2;        // the re-bind
    id.present[2] = false;   // ...and this entrant did not come back at all

    const uint16_t live = VacateFake::IndexFor(4, 2);
    REQUIRE(live != VacateFake::IndexFor(4));

    RecordingLog log;
    identity::RosterReport r{};
    identity::DressRoster(id, log, nullptr, 0, r, map);

    CHECK(r.residue == 2);
    CHECK(r.vacated == 1);
    CHECK(r.staleSlot == 1);        // slot 2 no longer resolves: nothing to vacate, and not a fault
    CHECK(r.vacateRefused == 0);
    CHECK(r.dressRefused == 0);
    CHECK(log.Count("residue slot 2: the entrant no longer exists") == 1);

    // Every index-keyed write went to the RE-RESOLVED index, and the log names that index too.
    CHECK(id.lastTeamIndex[4] == live);
    CHECK(id.lastLoadoutIndex[4] == live);
    CHECK(id.lastAlignIndex[4] == live);
    CHECK(id.teamWrites[4] == 1);
    CHECK(id.loadoutWrites[4] == 1);
    CHECK(id.alignWrites[4] == 1);
    CHECK(id.lastTeam[4] == facts::IDENT_ALIGN_TEAM_LIMIT);
    CHECK(id.lastAlignment[4] == 0xFFFF);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(log.Count("(index 14)") == 1);
    CHECK(ProfileIsVacant(id.lastProfile[4]));

    // And nothing landed on the slot the stale index sits nearest to.
    CHECK(id.teamWrites[3] == 0);
    CHECK(id.loadoutWrites[3] == 0);
    CHECK(id.alignWrites[3] == 0);
    CHECK(map.ResidueCount() == 0);
}

// -------------------------------------------------------------------------------------------
// ARCH-23 S7.2 -- the dress ladder's step 1.  L3 #11 read
//
//   [identity] player 'KFC-' slot 0 NOT dressed: stage=entrant reason=the entrant's profile
//              CJson is absent or empty
//
// for a player this session had never admitted: step 1 was five PREDICATES wearing an adding
// verb, so it went looking for an entrant nobody had created and reported the empty profile of
// whoever the lobby happened to hold in that slot.  W2-P0 measured who that is -- the viewer's
// own user, in slot 0, with an empty profile -- which is also why the recorded slot is not where
// the admission can land, and why everything addressing the engine's user keys on the slot the
// engine gave back.
// -------------------------------------------------------------------------------------------
namespace {

// The L3 #11 lobby: OCCUPIED, and with nothing in it this replay may dress.  `ours` is what an
// admission creates and the only thing the five predicates can find; `lobby` is everybody else.
class ProvisionLobby final : public IIdentity {
public:
    static constexpr size_t   kSlots = identity::RosterMapping::kSlots;
    static constexpr uint16_t kLobbyUsers = 3;   // the viewer at slot 0, and two more beside it

    bool     ours[kSlots] = {};
    bool     lobby[kSlots] = {};
    bool     remap = true;      // a provider that maps a recorded slot to a free one
    uint32_t provisions = 0;
    uint16_t askedSlot = identity::kNotDressed;
    JoinDesc lastDesc{};
    PawnProvision::Refusal lastProvision = PawnProvision::Refusal::NotAttempted;
    uint32_t profileWrites[kSlots] = {};
    char     lastProfile[kSlots][160] = {};
    Symbol64 applied[20] = {};

    ProvisionLobby() { for (uint16_t s = 0; s < kLobbyUsers; ++s) lobby[s] = true; }

    static uint16_t IndexFor(uint16_t slot) { return uint16_t(slot * 3 + 1); }
    uint16_t SlotFor(uint16_t index) const {
        if (index < 1 || (index - 1) % 3 != 0) return uint16_t(kSlots);
        const uint16_t s = uint16_t((index - 1) / 3);
        return s < kSlots ? s : uint16_t(kSlots);
    }
    // DERIVED from the fixture, never written down: a test that spelled the answer as `3` would
    // still pass with the dress keyed on a constant.
    uint16_t FirstFree() const {
        for (uint16_t s = 0; s < kSlots; ++s)
            if (!lobby[s] && !ours[s]) return s;
        return identity::kNotDressed;
    }

    bool AddReplayUser(UserSlot s, UserIndex& out) override {   // the five predicates, offline
        if (s.v >= kSlots || !ours[s.v]) return false;
        out = UserIndex(IndexFor(s.v));
        return true;
    }
    bool ProvisionReplayUser(UserSlot s, const JoinDesc& desc, UserIndex& out) override {
        ++provisions;
        askedSlot = s.v;
        lastDesc = desc;
        const uint16_t target = remap ? FirstFree() : s.v;
        if (target >= kSlots || lobby[target]) {
            lastProvision = PawnProvision::Refusal::SlotAlreadyOccupied;
            return false;
        }
        ours[target] = true;
        lastProvision = PawnProvision::Refusal::None;
        out = UserIndex(IndexFor(target));
        return true;
    }
    PawnProvision::Refusal LastProvisionRefusal() const override { return lastProvision; }
    bool SetProfileJson(UserSlot s, const Blob& json) override {
        if (s.v >= kSlots) return false;
        const uint32_t cap = uint32_t(sizeof lastProfile[0] - 1);
        const uint32_t n = json.n < cap ? json.n : cap;
        memcpy(lastProfile[s.v], json.p, n);
        lastProfile[s.v][n] = 0;
        ++profileWrites[s.v];
        return true;
    }
    bool SetTeam(UserIndex, uint8_t, uint8_t) override { return true; }
    bool ApplyLoadout(UserIndex, const Symbol64[20], uint16_t, uint16_t) override { return true; }
    bool OverrideAlignment(UserIndex, uint16_t) override { return true; }
    bool ReadAppliedItems(UserIndex, Symbol64 out[20]) const override {
        memcpy(out, applied, sizeof applied);
        return true;
    }
    UserSlot  SlotOf(UserIndex i) const override { return UserSlot(SlotFor(i.v)); }
    UserIndex IndexOf(UserSlot s) const override {
        return UserIndex(s.v < kSlots && (ours[s.v] || lobby[s.v]) ? IndexFor(s.v)
                                                                   : identity::kNotDressed);
    }
};

const char kEntrantJson[] = "{\"displayname\":\"KFC-\",\"stats\":{\"level\":9}}";

void FillL3Record(PlayerRecord& rec) {
    memset(&rec, 0, sizeof rec);
    rec.userSlot = 0;                      // L3 #11's recorded slot: the one the viewer holds
    rec.xplatformId = 0x4B4643ull;
    rec.teamId = 1;
    // Both widths, because the READER fills both -- a B.0 row is widened at load and an extended
    // row carries the 16-bit field outright, so a record with teamId set and teamId16 zero is a
    // shape no reader produces.
    rec.teamId16 = 1;
    rec.teamSlot = 0;
    rec.loadoutNumber = 17;
    rec.loadoutNumberBody = 42;
    memcpy(rec.displayName, "KFC-", 5);
    FillDistinct(rec.loadoutSlots);
    Blob json{};
    json.p = reinterpret_cast<const uint8_t*>(kEntrantJson);
    json.n = uint32_t(sizeof kEntrantJson - 1);
    rec.entrantJson = json;
}

}  // namespace

TEST_CASE("step 1 admits the recorded player, and the dress keys on the slot the engine gave back") {
    ProvisionLobby id;
    PlayerRecord rec;
    FillL3Record(rec);

    const uint16_t expected = id.FirstFree();
    REQUIRE(expected != identity::kNotDressed);
    // The two mutants this case has to separate -- keyed on the recorded slot, keyed on the
    // resolved one -- are the same code unless the fixture moves the answer.
    REQUIRE(expected != rec.userSlot);

    // RED BEFORE, and it is the old step 1 exactly: the five predicates alone, on this lobby,
    // find nothing of ours to dress.  That refusal is what L3 #11 printed at stage=entrant.
    UserIndex lookup(0);
    CHECK_FALSE(id.ResolveReplayUser(UserSlot(rec.userSlot), lookup));

    identity::DressResult out{};
    identity::Dress(id, rec, out);

    CHECK(id.provisions == 1);
    CHECK(id.askedSlot == rec.userSlot);          // asked for the RECORDED slot...
    CHECK(out.slotResolved == expected);          // ...and dressed the one it was given
    CHECK(out.indexResolved);
    CHECK(out.userIndex == ProvisionLobby::IndexFor(expected));
    CHECK(out.provisionRefusal == PawnProvision::Refusal::None);
    // Step 1 no longer refuses, which is the whole of the change: L3 #11 never got past it.
    CHECK(strcmp(out.stage, "entrant") != 0);

    // The write that addresses the engine's user landed on the RESOLVED slot, and the viewer --
    // who holds the recorded slot and is not part of this replay -- was not touched.
    CHECK(id.profileWrites[expected] == 1);
    CHECK(id.profileWrites[rec.userSlot] == 0);
    CHECK(strcmp(id.lastProfile[expected], kEntrantJson) == 0);
    CHECK(id.lastProfile[rec.userSlot][0] == 0);

    // ...while what is WORN still comes from the recorded PlayerRecord, not from the live lobby.
    // ...with the ONE exception the eviction key forces: the xplatform id is authored, never
    // carried across, so a capture replayed on its own account cannot evict the viewer.
    CHECK(id.lastDesc.xplatformId != rec.xplatformId);
    CHECK(id.lastDesc.xplatformId != ~uint64_t(0));
    CHECK(id.lastDesc.team == rec.teamId);
    CHECK(id.lastDesc.useSpawnPoint);

    // THE OFFLINE HORIZON, stated rather than papered over.  identity_access::SetSlotName reaches
    // the engine through the shipped Identity singleton, so for a foreign IIdentity every dress in
    // this suite stops at `slotname` -- it did before this change too.  What is pinned here is that
    // the ladder now REACHES that step, on the resolved slot; the `dressed` line itself
    // (`slot 0 -> N dressed: N/N items match`) is L3 #12's to show.
    CHECK(strcmp(out.stage, "slotname") == 0);
    CHECK(out.refusal == identity_access::Refusal::NoSlotNameTable);
}

TEST_CASE("a provisioning refusal is reported as one, and never as an absent profile") {
    ProvisionLobby id;
    id.remap = false;    // this build's Identity::ProvisionReplayUser refuses an occupied slot
    PlayerRecord rec;
    FillL3Record(rec);

    RecordingLog log;
    identity::DressResult out{};
    CHECK_FALSE(identity::DressAndReport(id, log, rec, out));

    CHECK(strcmp(out.stage, "entrant") == 0);
    CHECK(out.provisionRefusal == PawnProvision::Refusal::SlotAlreadyOccupied);
    CHECK(!out.indexResolved);
    CHECK(out.slotResolved == identity::kNotDressed);
    // Named from the vocabulary that actually ran...
    CHECK(strstr(out.detail,
                 PawnProvision::RefusalText(PawnProvision::Refusal::SlotAlreadyOccupied)) != nullptr);
    // ...and not from the one that did not.  This is the L3 #11 sentence, taken from the shipped
    // table rather than retyped, and a provisioning failure may never carry it.
    CHECK(out.refusal == identity_access::Refusal::None);
    CHECK(strstr(out.detail,
                 identity_access::RefusalText(identity_access::Refusal::EmptyProfile)) == nullptr);
    CHECK(log.Count("NOT dressed: stage=entrant") == 1);
    // Nothing was written to the user who holds that slot.
    CHECK(id.profileWrites[rec.userSlot] == 0);
}

TEST_CASE("the roster line's item count is the recorded positions, not twenty") {
    // The number in L3 #12's must-show line -- `N/N items match` -- is the count of positions the
    // RECORDING filled, so a capture carrying one item reads 1/1 and not 1/20.
    Symbol64 want[20] = {};
    Symbol64 got[20] = {};
    want[7] = Symbol64(0xABCDEF01ull);
    got[7] = want[7];
    const identity::ItemDiff d = identity::DiffItems(want, got);
    CHECK(d.compared == 1);
    CHECK(d.matching == 1);
    CHECK(d.absent == 0);
}

#include <stdlib.h>
#include <stdint.h>

// =================================================================================================
// ENG-LOADOUT-GUARD -- the bounded, gated loadout read.  L3 #19 / dump 53400.
//
// The watched thread's innermost frame in that dump is this file's own loadout read, under the pawn
// census, and the process then took 0xC0000005.  These cases drive identity_access::CensusLoadout
// over memory the TEST owns -- no engine, no netGame -- because the two things that have to be
// proved are exactly the two a live run cannot show without another crash: that a garbage count
// bounds nothing, and that an entrant whose document is still the json db's cached one is refused
// BEFORE a loadout byte is touched.
//
// The instance vector is allocated at the GARBAGE count (0xFFFF instances, 4 MiB), not at the
// clamp.  That is deliberate: a walk with the bound dropped then reads real memory and REPORTS its
// iteration count instead of faulting, so the mutant fails an assertion rather than killing the
// process.  `steps` is what makes the bound testable at all.
//
// No value is reused across two fields: number 7 vs number_body 23, item counts 5/6/7.

namespace {

struct FakeNetGame {
    static const uint32_t kGarbageCount = 0xFFFFu;  // facts-ok: LOBBY.TEAM_NONE not a slot sentinel here -- a deliberately GARBAGE instance count, the shape a reserved loadout page reads back as

    uint8_t* ng    = nullptr;
    uint8_t* doc   = nullptr;
    uint8_t* vec   = nullptr;
    size_t   docOff = 0;
    size_t   hdrOff = 0;

    FakeNetGame(uint16_t entrant, uint16_t slot) {
        // Both offsets come from identity_access, so this fixture pins none of the geometry it is
        // testing through: if the table moves, the planting moves with the reader.
        docOff = identity_access::ProfileDocFieldOffset(entrant);
        hdrOff = identity_access::LoadoutHeaderOffset(UserSlot(slot));
        const size_t span = (docOff > hdrOff ? docOff : hdrOff) + 4096;
        ng  = static_cast<uint8_t*>(calloc(1, span));
        doc = static_cast<uint8_t*>(calloc(1, 256));
        vec = static_cast<uint8_t*>(
            calloc(kGarbageCount, sizeof(identity_access::LoadoutInstance)));
    }
    ~FakeNetGame() { free(ng); free(doc); free(vec); }

    void Attach(bool cacheOwned) {
        *reinterpret_cast<uint64_t*>(doc + identity_access::ProfileDocCachedOffset()) =
            cacheOwned ? 0x1234u : 0u;
        *reinterpret_cast<void**>(ng + docOff) = doc;
    }
    void DetachDocument() { *reinterpret_cast<void**>(ng + docOff) = nullptr; }

    identity_access::LoadoutHeader* Header() {
        return reinterpret_cast<identity_access::LoadoutHeader*>(ng + hdrOff);
    }
    identity_access::LoadoutInstance* Instances() {
        return reinterpret_cast<identity_access::LoadoutInstance*>(vec);
    }
    void FillItemCounts(uint64_t n) {
        identity_access::LoadoutInstance* v = Instances();
        for (uint32_t i = 0; i < kGarbageCount; ++i) v[i].itemCount = n;
    }
};

}  // namespace

TEST_CASE("W2-C: a garbage instance count is clamped to the format and the walk is bounded") {
    FakeNetGame f(1, 1);
    REQUIRE(f.ng != nullptr);
    REQUIRE(f.vec != nullptr);
    f.Attach(false);
    REQUIRE(identity_access::ProfileDocWritable(f.doc));

    identity_access::LoadoutHeader* h = f.Header();
    h->instances     = f.vec;
    h->instanceCount = FakeNetGame::kGarbageCount;
    h->number        = 7;
    h->numberBody    = 23;
    f.FillItemCounts(3);

    identity_access::LoadoutCensus out;
    identity_access::Refusal why = identity_access::Refusal::BuildMismatch;
    CHECK(identity_access::CensusLoadout(f.ng, UserIndex(1), UserSlot(1), out, &why));
    CHECK(why == identity_access::Refusal::None);

    // The claim is reported verbatim and separately from what was walked: a caller must be able to
    // see that the engine said something the format cannot describe.
    CHECK(out.claimedInstances == FakeNetGame::kGarbageCount);
    CHECK(out.clamped);

    // ★ THE RUNAWAY DETECTOR.  With the clamp dropped this is 65535 and this case goes red without
    // the process going anywhere.
    CHECK(out.steps <= identity_access::kLoadoutFormatMax);
    CHECK(out.steps == identity_access::kLoadoutFormatMax);
    CHECK(out.instances == identity_access::kLoadoutFormatMax);
    CHECK(out.items == 3u * identity_access::kLoadoutFormatMax);

    CHECK(out.number == 7);
    CHECK(out.numberBody == 23);
}

TEST_CASE("W2-C: a cache-owned document is refused BY NAME before any loadout byte is read") {
    FakeNetGame f(1, 1);
    REQUIRE(f.ng != nullptr);
    f.Attach(true);
    REQUIRE_FALSE(identity_access::ProfileDocWritable(f.doc));

    // A perfectly readable, perfectly walkable loadout is planted under the closed gate on purpose.
    // Nothing about the loadout can be the reason for the refusal, so the refusal can only be the
    // gate -- and dropping the gate makes this case answer numbers instead of a word.
    identity_access::LoadoutHeader* h = f.Header();
    h->instances     = f.vec;
    h->instanceCount = 4;
    h->number        = 7;
    h->numberBody    = 23;
    f.FillItemCounts(5);

    identity_access::LoadoutCensus out;
    identity_access::Refusal why = identity_access::Refusal::BuildMismatch;
    CHECK_FALSE(identity_access::CensusLoadout(f.ng, UserIndex(1), UserSlot(1), out, &why));
    CHECK(why == identity_access::Refusal::ProfileNotAttached);
    CHECK(out.steps == 0);
    CHECK(out.instances == 0);
    CHECK(out.number == 0);
    CHECK(out.claimedInstances == 0);

    // No document at all is the SAME refusal and not LoadoutsUnreadable: "never attached" and
    // "attached and unreadable" are different facts and only one of them is about memory.
    f.DetachDocument();
    why = identity_access::Refusal::BuildMismatch;
    CHECK_FALSE(identity_access::CensusLoadout(f.ng, UserIndex(1), UserSlot(1), out, &why));
    CHECK(why == identity_access::Refusal::ProfileNotAttached);
    CHECK(out.steps == 0);
}

TEST_CASE("W2-C: a properly attached entrant answers its numbers and a bounded instance count") {
    FakeNetGame f(2, 3);
    REQUIRE(f.ng != nullptr);
    f.Attach(false);

    identity_access::LoadoutHeader* h = f.Header();
    h->instances     = f.vec;
    h->instanceCount = 3;
    h->number        = 11;
    h->numberBody    = 42;
    identity_access::LoadoutInstance* v = f.Instances();
    v[0].itemCount = 5;
    v[1].itemCount = 6;
    v[2].itemCount = 7;

    identity_access::LoadoutCensus out;
    identity_access::Refusal why = identity_access::Refusal::BuildMismatch;
    CHECK(identity_access::CensusLoadout(f.ng, UserIndex(2), UserSlot(3), out, &why));
    CHECK(why == identity_access::Refusal::None);
    CHECK(out.number == 11);
    CHECK(out.numberBody == 42);
    CHECK(out.claimedInstances == 3);
    CHECK_FALSE(out.clamped);
    CHECK(out.instances == 3);
    CHECK(out.steps == 3);
    CHECK(out.items == 5u + 6u + 7u);

    SUBCASE("zero instances is an ANSWER, not a refusal") {
        h->instanceCount = 0;
        identity_access::LoadoutCensus empty;
        identity_access::Refusal w2 = identity_access::Refusal::BuildMismatch;
        CHECK(identity_access::CensusLoadout(f.ng, UserIndex(2), UserSlot(3), empty, &w2));
        CHECK(w2 == identity_access::Refusal::None);
        CHECK(empty.instances == 0);
        CHECK(empty.steps == 0);
        CHECK(empty.number == 11);   // the numbers were still read
    }

    SUBCASE("an instance vector that is not committed memory refuses LoadoutsUnreadable, not 0") {
        h->instances = reinterpret_cast<void*>(uintptr_t(0x10));
        h->instanceCount = 2;
        identity_access::LoadoutCensus bad;
        identity_access::Refusal w3 = identity_access::Refusal::BuildMismatch;
        CHECK_FALSE(identity_access::CensusLoadout(f.ng, UserIndex(2), UserSlot(3), bad, &w3));
        CHECK(w3 == identity_access::Refusal::LoadoutsUnreadable);
        CHECK(bad.steps == 0);
    }

    SUBCASE("no netGame and a slot past the table each refuse by their own name") {
        identity_access::LoadoutCensus none;
        identity_access::Refusal w4 = identity_access::Refusal::BuildMismatch;
        CHECK_FALSE(identity_access::CensusLoadout(nullptr, UserIndex(2), UserSlot(3), none, &w4));
        CHECK(w4 == identity_access::Refusal::NoNetGame);
        CHECK_FALSE(identity_access::CensusLoadout(
            f.ng, UserIndex(2), UserSlot(uint16_t(facts::ENTITY_USER_SLOT_TABLE_MAX)), none, &w4));
        CHECK(w4 == identity_access::Refusal::SlotOutOfRange);
    }
}

// NAMES-4.  The claim the number fix rests on: the two jersey numbers live in the per-slot loadout
// HEADER -- the engine's own entrant dump reads `Loadouts` at +0x30 and `Loadout Number` at +0x38 of
// the same 64-byte entry (sub_1401CD1A0) -- and not inside the instance array that carries the
// twenty item ids.  So a slot with no instance still has somewhere to put a number, and
// ApplyLoadout writing the numbers below its instance gate was what left every body plateless.
TEST_CASE("NAMES-4: the jersey numbers are readable from a header that holds no instance") {
    FakeNetGame f(2, 3);
    REQUIRE(f.ng != nullptr);

    identity_access::LoadoutHeader* h = f.Header();
    h->instances     = nullptr;
    h->instanceCount = 0;
    h->number        = 21;
    h->numberBody    = 7;

    uint16_t number = 0, numberBody = 0;
    identity_access::Refusal why = identity_access::Refusal::BuildMismatch;
    CHECK(identity_access::LoadoutNumbers(f.ng, UserSlot(3), number, numberBody, &why));
    CHECK(why == identity_access::Refusal::None);
    CHECK(number == 21);
    CHECK(numberBody == 7);
    // ...and the instance count is a separate reading of the same entry, so the two cannot be
    // confused for one another by a wrong offset.
    CHECK(identity_access::LiveLoadoutInstances(f.ng, UserSlot(3)) == 0);
}

// ---------------------------------------------------------------------------------------------
// W2-C's reply route, driven end to end with no engine image mapped.  ReplyRouteInjectForTest
// supplies the three values BINDING 1 and BINDING 2 would read out of echovr.exe; EVERYTHING else
// below is the shipped path -- the real Identity, the real AdmitIntoLobbySlot, the real queue,
// the real thunk and the real drain.
namespace {

FakeSlotTable* g_routeNg = nullptr;
void* RouteNetGame() { return g_routeNg ? g_routeNg->ng : nullptr; }

struct PublishRecord {
    uint32_t calls     = 0;
    void*    node      = nullptr;
    uint64_t msgId     = 0;
    char     name[64]  = {};
    uint64_t bytes     = 0;
    uint64_t slot      = ~uint64_t(0);
};
PublishRecord g_pub;

uint64_t RecordPublish(void* node, uint64_t msgId, const char* name, const void* payload,
                       uint64_t bytes) {
    ++g_pub.calls;
    g_pub.node  = node;
    g_pub.msgId = msgId;
    g_pub.bytes = bytes;
    snprintf(g_pub.name, sizeof g_pub.name, "%s", name ? name : "");
    if (payload && bytes >= sizeof(PawnProvision::ReplyMsg))
        g_pub.slot = static_cast<const PawnProvision::ReplyMsg*>(payload)->slot;
    return 0;
}

// The broadcaster half of IWiretap: it records what the route registers and can invoke ONE
// registration, which is all the engine's receive pump ever does to us.  The name is COPIED --
// ReplyRouteArmWith splits its CSV into a stack buffer that is gone by the time we read it.
struct FakeTap : IWiretap {
    struct Reg { char name[64]; WiretapFn fn; void* ctx; };
    Reg      regs[8]  = {};
    uint16_t regCount = 0;

    bool        Available()  const override { return true; }
    const char* Diagnostic() const override { return "fake-tap"; }
    uint64_t    Symbol(const char*) const override { return 0; }

    uint16_t Listen(const char* name, WiretapFn fn, void* ctx) override {
        if (regCount >= 8 || !fn) return kInvalidHandle;
        snprintf(regs[regCount].name, sizeof regs[0].name, "%s", name ? name : "");
        regs[regCount].fn  = fn;
        regs[regCount].ctx = ctx;
        return regCount++;
    }
    uint16_t ListenSymbol(uint64_t, WiretapFn, void*) override { return kInvalidHandle; }
    void     Unlisten(uint16_t) override {}
    void     UnlistenAll() override { regCount = 0; }
    uint32_t ActiveCount() const override { return regCount; }
    uint32_t LockTimeouts() const override { return 0; }

    // One message to ONE registration.  A fan-out over all three would make pumpCallbacks a count
    // of our own fixture rather than of the engine's calls.
    void DeliverOne() {
        REQUIRE(regCount > 0);
        WireMessage m{nullptr, 0, 0, 0, 0};
        regs[0].fn(m, regs[0].ctx);
    }
};

}  // namespace

// The shipped default now routes recorded players through the bot body-build, so the
// legacy AddRemoteUser reply-route tests below force the legacy path for their duration.  RAII so the
// force is restored to -1 (config) even if a REQUIRE in the test aborts it.
namespace {
struct LegacyRouteScope {
    LegacyRouteScope()  { identity_access::BotBodyForceForTest(0); }
    ~LegacyRouteScope() { identity_access::BotBodyForceForTest(-1); }
};
}  // namespace

TEST_CASE("W2-C: the route publishes exactly once, from inside the pump, and never from the tick") {
    LegacyRouteScope legacyRoute_;
    static FakeSlotTable t;
    static FakeTap tap;
    tap = FakeTap{};
    g_pub = PublishRecord{};
    t.Build(kEntries);
    // NETGAME.STATE_OFF is 0 -- the read is at the netgame pointer itself.  Without this,
    // ProvisionReplayUser refuses SessionNotInMatch before the route is consulted at all.
    const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
    memcpy(t.ng, &inMatch, sizeof inMatch);
    g_routeNg = &t;

    // Three values no offline process can bind.  The node and the id are distinctive so the
    // publish cannot be shown to carry them by accident.
    void* const    kNode  = reinterpret_cast<void*>(static_cast<uintptr_t>(0xB0ADCA57u));
    const uint64_t kMsgId = 0x51A73BEEF0001234ull;
    identity_access::ReplyRouteInjectForTest(kNode, &RecordPublish, kMsgId);

    IIdentity* id = identity_access::Create(&RouteNetGame, nullptr);
    REQUIRE(id != nullptr);
    REQUIRE(identity_access::ReplyRouteArmWith(t.ng, &tap));
    REQUIRE(identity_access::ReplyRoutePromoted());
    // The shipped listen set, registered through the real CSV walk -- not a name this test chose.
    CHECK(tap.regCount == 3);
    CHECK(strcmp(tap.regs[0].name, "SR15NetComponentEvent") == 0);

    JoinDesc desc{};
    desc.xplatformId   = 0xDEC0DE1234567890ull;
    desc.team          = 1;
    desc.role          = 0;
    desc.useSpawnPoint = true;
    desc.entrantJson.p = reinterpret_cast<const uint8_t*>(kEntrantJson);
    desc.entrantJson.n = uint32_t(sizeof kEntrantJson - 1);
    FillDistinct(desc.loadoutSlots);
    desc.loadoutNumber     = 21;
    desc.loadoutNumberBody = 7;

    const uint16_t kSlot = 3;

    // ---- THE TICK PATH.  Nothing here is in the engine's receive phase, so nothing may publish.
    UserIndex out(identity::kNotDressed);
    CHECK_FALSE(id->ProvisionReplayUser(UserSlot(kSlot), desc, out));
    CHECK(id->LastProvisionRefusal() == PawnProvision::Refusal::ReplyWrongPhase);
    {
        const auto c = identity_access::ReplyRouteStatus();
        CHECK(c.queued     == 1);
        CHECK(c.wrongPhase == 1);
        CHECK(c.published  == 0);
        CHECK(c.drained    == 0);
    }
    // The publish function was never entered -- not "entered and refused", never entered.
    CHECK(g_pub.calls == 0);

    // ---- THE PUMP PATH.  The engine calls our listener; the drain re-attempts from that stack.
    tap.DeliverOne();
    {
        const auto c = identity_access::ReplyRouteStatus();
        CHECK(c.pumpCallbacks == 1);
        CHECK(c.drained       == 1);
        CHECK(c.published     == 1);
    }
    CHECK(g_pub.calls == 1);
    CHECK(g_pub.node  == kNode);
    CHECK(g_pub.msgId == kMsgId);
    // The leading 'S' is the struct name as the binary spells it, and the length is the one the
    // engine's own trampoline tests before it calls the handler.
    CHECK(strcmp(g_pub.name, "SNSLobbyEntrantAdded") == 0);
    CHECK(g_pub.bytes == uint64_t(facts::MSG_ENTRANT_ADDED_SIZE));
    CHECK(g_pub.bytes == 8);
    CHECK(g_pub.slot  == kSlot);          // the slot that was QUEUED, not slot 0 and not a count

    // ---- EXACTLY ONCE.  A second callback finds the queue empty.  ReplyDrain clears p.live
    // before the attempt, and this is the assertion that line exists to protect.
    tap.DeliverOne();
    {
        const auto c = identity_access::ReplyRouteStatus();
        CHECK(c.pumpCallbacks == 2);
        CHECK(c.drained       == 1);
        CHECK(c.published     == 1);
    }
    CHECK(g_pub.calls == 1);

    // The provision itself still REFUSES: this netgame is a fixture, so the engine created no
    // user and the postcondition says so.  The subject of this case is the publish, and a case
    // that demanded success here would be testing the fake instead of the route.
    CHECK(id->LastProvisionRefusal() == PawnProvision::Refusal::AdmitDidNotCreateUser);

    identity_access::ReplyRouteDisarm();
    identity_access::ReplyRouteInjectForTest(nullptr, nullptr, 0);
    identity_access::Destroy(id);
    g_routeNg = nullptr;
}


// NAMES-4.  The name has to be in the lobby member record BEFORE the entrant-added message is
// dispatched: the engine's own handler runs the participating transition from inside that dispatch,
// and that is where the in-world name tag above the body is built.  A name written afterwards
// reaches the roster and the spectator UI -- both re-read UserDisplayName -- and not the tag, which
// is exactly what the owner saw on the SHIPDEFAULTS launch.  The seed is the last moment that is
// early enough, so this asserts the record holds the recorded name when the publish happens.
TEST_CASE("NAMES-4: the entrant's display name is in the lobby record before the add is dispatched") {
    LegacyRouteScope legacyRoute_;
    static FakeSlotTable t;
    static FakeTap tap;
    tap = FakeTap{};
    g_pub = PublishRecord{};
    t.Build(kEntries);
    const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
    memcpy(t.ng, &inMatch, sizeof inMatch);
    g_routeNg = &t;

    identity_access::ReplyRouteInjectForTest(
        reinterpret_cast<void*>(static_cast<uintptr_t>(0xB0ADCA57u)), &RecordPublish, 1);
    IIdentity* id = identity_access::Create(&RouteNetGame, nullptr);
    REQUIRE(id != nullptr);
    REQUIRE(identity_access::ReplyRouteArmWith(t.ng, &tap));
    REQUIRE(identity_access::ReplyRoutePromoted());

    JoinDesc desc{};
    desc.xplatformId   = 0xDEC0DE1234567890ull;
    desc.team          = 1;
    desc.useSpawnPoint = true;
    memcpy(desc.displayName, "leavemealone_-", sizeof "leavemealone_-");

    const uint16_t kSlot = 2;          // the fixture's table holds kEntries entries; 2 has both neighbours
    UserIndex out(identity::kNotDressed);
    CHECK_FALSE(id->ProvisionReplayUser(UserSlot(kSlot), desc, out));   // queued, wrong phase

    // Before the drain nothing has been seeded, so the entry still holds the fixture's own fill.
    CHECK(t.Untouched(kSlot));

    tap.DeliverOne();
    CHECK(g_pub.calls == 1);            // the dispatch happened...
    CHECK(g_pub.slot  == kSlot);
    // ...and the name was there for it, at the recorded field offset of the seeded slot's entry.
    CHECK(strcmp(t.Name(kSlot), "leavemealone_-") == 0);
    // The mirror the engine's own entrant refresh copies over the display buffer agrees, so a
    // refresh cannot put "..." back.
    CHECK(strcmp(t.MirrorName(kSlot), "leavemealone_-") == 0);
    // One entry's NAME field, not a stride or field-offset error into a neighbour.  The whole
    // entry is not asserted untouched: the seed also writes the account pair, the valid bit and
    // the team into the record it is named for, which is the rest of its job.
    CHECK(t.Name(kSlot - 1)[0] == char(0xAB));
    CHECK(t.Name(kSlot + 1)[0] == char(0xAB));

    SUBCASE("a recorded name the capture never filled does not vacate the slot") {
        // "" is what VacateSlot writes to RELEASE a slot.  A seed that wrote it would strip the
        // name of whoever the engine has there.
        JoinDesc nameless = desc;
        memset(nameless.displayName, 0, sizeof nameless.displayName);
        const uint16_t kOther = 0;
        UserIndex out2(identity::kNotDressed);
        CHECK_FALSE(id->ProvisionReplayUser(UserSlot(kOther), nameless, out2));
        tap.DeliverOne();
        CHECK(t.Name(kOther)[0] == char(0xAB));
        CHECK(t.MirrorName(kOther)[0] == char(0xAB));
    }

    identity_access::ReplyRouteDisarm();
    identity_access::ReplyRouteInjectForTest(nullptr, nullptr, 0);
    identity_access::Destroy(id);
    g_routeNg = nullptr;
}

// The no-AI refuse gate.  When the engine's own remote-admission reply route is NOT
// promoted -- its runtime binds did not resolve, or pawn.reply_route is disarmed -- the PRODUCTION
// ProvisionReplayUser must seat NOTHING and refuse by name, never falling through to
// AdmitIntoLobbySlot's direct insert (IDENT.ADMIT_USER 0x140152210 = AddBotUser, which attaches AI:
// R14NetBot / RxAI).  The W2-C case above is the anti-vacuity control: with the route PROMOTED,
// admission publishes through the DISPATCH (g_pub.calls == 1), so this gate cannot be satisfied by a
// build that simply never admits anywhere.  FAIL-before / PASS-after is proved by the source mutant
// W2P1_MUTATE_NO_NOAI_GATE: with the gate removed the provisioner falls through to the direct
// insert, whose guarded AddBotUser VA cannot bind in this offline process, so LastProvisionRefusal()
// is AdmitNotPromoted -- not DirectInsertSuppressed -- and the CHECK below goes red.
TEST_CASE("An unpromoted reply route refuses the direct bot insert and seats nothing") {
    LegacyRouteScope legacyRoute_;
    static FakeSlotTable t;
    t.Build(kEntries);
    // In match, so ProvisionReplayUser reaches the gate rather than refusing SessionNotInMatch.
    const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
    memcpy(t.ng, &inMatch, sizeof inMatch);
    g_routeNg = &t;

    // NOT promoted: no inject, no arm.  Disarm + clear any binding a prior case left behind.
    identity_access::ReplyRouteDisarm();
    identity_access::ReplyRouteInjectForTest(nullptr, nullptr, 0);
    REQUIRE_FALSE(identity_access::ReplyRoutePromoted());

    IIdentity* id = identity_access::Create(&RouteNetGame, nullptr);
    REQUIRE(id != nullptr);

    JoinDesc desc{};
    desc.xplatformId   = 0xDEC0DE1234567890ull;
    desc.team          = 1;
    desc.role          = 0;
    desc.useSpawnPoint = true;
    desc.entrantJson.p = reinterpret_cast<const uint8_t*>(kEntrantJson);
    desc.entrantJson.n = uint32_t(sizeof kEntrantJson - 1);
    FillDistinct(desc.loadoutSlots);
    desc.loadoutNumber     = 21;
    desc.loadoutNumberBody = 7;

    UserIndex out(identity::kNotDressed);
    const bool ok = id->ProvisionReplayUser(UserSlot(3), desc, out);
    CHECK_FALSE(ok);
    CHECK(id->LastProvisionRefusal() == PawnProvision::Refusal::DirectInsertSuppressed);
    CHECK(out.v == kSlotUnmapped);

    identity_access::Destroy(id);
    g_routeNg = nullptr;
}

// The discriminating counterpart to ENG-NOAI-GATE.  The SAME unpromoted-route setup
// that gates the legacy path off (DirectInsertSuppressed) must, with the bot path on, NOT gate on the
// reply route at all -- it routes to the bot body-build instead of refusing.  Off-image the bot insert
// cannot bind (IDENT.ADMIT_USER_VA is unmapped), so the admission refuses AdmitNotPromoted; the point
// is only that it is NOT the DirectInsertSuppressed the reply-route gate would have returned.
TEST_CASE("The bot path does not gate on the reply route, so it never DirectInsertSuppresses") {
    static FakeSlotTable t;
    t.Build(kEntries);
    const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
    memcpy(t.ng, &inMatch, sizeof inMatch);
    g_routeNg = &t;

    identity_access::ReplyRouteDisarm();
    identity_access::ReplyRouteInjectForTest(nullptr, nullptr, 0);
    REQUIRE_FALSE(identity_access::ReplyRoutePromoted());   // the very condition that suppresses the legacy path

    identity_access::BotBodyForceForTest(1);
    IIdentity* id = identity_access::Create(&RouteNetGame, nullptr);
    REQUIRE(id != nullptr);

    JoinDesc desc{};
    desc.xplatformId   = 0xDEC0DE1234567890ull;
    desc.team          = 1;
    desc.useSpawnPoint = true;

    UserIndex out(identity::kNotDressed);
    const bool ok = id->ProvisionReplayUser(UserSlot(3), desc, out);
    CHECK_FALSE(ok);                                        // off-image the insert cannot bind...
    CHECK(id->LastProvisionRefusal() != PawnProvision::Refusal::DirectInsertSuppressed);  // ...but the gate is gone
    CHECK(out.v == kSlotUnmapped);                          // and nothing was seated

    identity_access::BotBodyForceForTest(-1);
    identity_access::Destroy(id);
    g_routeNg = nullptr;
}


// BLOCKER-1 (DIAG-REPLYPHASE Fix B).  The wrong-phase deferral was never draining offline: the pump
// listens on three inbound-replication symbols that a solo host never dispatches, so pump=0 queued=1
// drained=0 for ever.  ReplyDrainFromEngineHeartbeat drains it from the engine's own HeartbeatBefore
// stack instead.  FAIL-before / PASS-after is INSIDE one case: before the drain nothing has published
// (the tick refused ReplyWrongPhase and parked the request); after the heartbeat drain the queued
// request publishes through the route exactly once.
TEST_CASE("BLOCKER-1: the wrong-phase-deferred reply publish drains from the engine heartbeat") {
    LegacyRouteScope legacyRoute_;
    static FakeSlotTable t;
    static FakeTap tap;
    tap = FakeTap{};
    g_pub = PublishRecord{};
    t.Build(kEntries);
    const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
    memcpy(t.ng, &inMatch, sizeof inMatch);
    g_routeNg = &t;

    void* const    kNode  = reinterpret_cast<void*>(static_cast<uintptr_t>(0xB0ADCA57u));
    const uint64_t kMsgId = 0x51A73BEEF0001234ull;
    identity_access::ReplyRouteInjectForTest(kNode, &RecordPublish, kMsgId);

    IIdentity* id = identity_access::Create(&RouteNetGame, nullptr);
    REQUIRE(id != nullptr);
    REQUIRE(identity_access::ReplyRouteArmWith(t.ng, &tap));
    REQUIRE(identity_access::ReplyRoutePromoted());

    JoinDesc desc{};
    desc.xplatformId   = 0xDEC0DE1234567890ull;
    desc.team          = 1;
    desc.role          = 0;
    desc.useSpawnPoint = true;
    desc.entrantJson.p = reinterpret_cast<const uint8_t*>(kEntrantJson);
    desc.entrantJson.n = uint32_t(sizeof kEntrantJson - 1);
    FillDistinct(desc.loadoutSlots);
    desc.loadoutNumber     = 21;
    desc.loadoutNumberBody = 7;
    const uint16_t kSlot = 3;

    // FAIL-before-the-drain: the tick parks the request wrong-phase and NOTHING publishes.
    UserIndex out(identity::kNotDressed);
    CHECK_FALSE(id->ProvisionReplayUser(UserSlot(kSlot), desc, out));
    CHECK(id->LastProvisionRefusal() == PawnProvision::Refusal::ReplyWrongPhase);
    {
        const auto c = identity_access::ReplyRouteStatus();
        CHECK(c.queued    == 1);
        CHECK(c.drained   == 0);
        CHECK(c.published == 0);
    }
    CHECK(g_pub.calls == 0);

    // PASS-after: the heartbeat drain re-attempts from a manufactured receive phase and publishes
    // the parked request through the engine's own dispatch (here the injected fake) exactly once.
    const auto c = identity_access::ReplyDrainFromEngineHeartbeat();
    CHECK(c.drained   == 1);
    CHECK(c.published == 1);
    CHECK(g_pub.calls == 1);
    CHECK(g_pub.node  == kNode);
    CHECK(g_pub.msgId == kMsgId);
    CHECK(strcmp(g_pub.name, "SNSLobbyEntrantAdded") == 0);
    CHECK(g_pub.slot  == kSlot);

    // Exactly once: a second heartbeat finds the queue empty and is a no-op.
    const auto c2 = identity_access::ReplyDrainFromEngineHeartbeat();
    CHECK(c2.drained   == 1);
    CHECK(c2.published == 1);
    CHECK(g_pub.calls  == 1);

    identity_access::ReplyRouteDisarm();
    identity_access::ReplyRouteInjectForTest(nullptr, nullptr, 0);
    identity_access::Destroy(id);
    g_routeNg = nullptr;
}


// DIAG-DRAIN-NOFIRE Q1.  The heartbeat drain never fired in-match last run because it
// was wired to the GUI app-frame heartbeat -- owner-thread gated, and frozen in-match (draws=0 the whole
// match).  The fix drives it from a DEDICATED always-armed detour on CR15NetGame::Update, filtered to
// the engine's stage-0 tick.  This drives the detour's BODY off the stage filter ALONE -- no MinHook, no
// tick.mode, no fallback detour installed in this process -- so the always-armed path cannot depend on
// tick.mode leaving tick's own NetGameUpdateDetour uninstalled (which is exactly the slot-mode run).
// The unconditional driverCalls counter is the witness the last run lacked: it separates "the call site
// never fired" (driverCalls==0) from "fired but no-op'd" (driverCalls>0, drained==0).
TEST_CASE("The dedicated NETGAME.UPDATE detour body drives the drain, stage-filtered") {
    LegacyRouteScope legacyRoute_;
    static FakeSlotTable t;
    static FakeTap tap;
    tap = FakeTap{};
    g_pub = PublishRecord{};
    t.Build(kEntries);
    const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
    memcpy(t.ng, &inMatch, sizeof inMatch);
    g_routeNg = &t;

    void* const    kNode  = reinterpret_cast<void*>(static_cast<uintptr_t>(0xB0ADCA57u));
    const uint64_t kMsgId = 0x51A73BEEF0001234ull;
    identity_access::ReplyRouteInjectForTest(kNode, &RecordPublish, kMsgId);

    IIdentity* id = identity_access::Create(&RouteNetGame, nullptr);
    REQUIRE(id != nullptr);
    REQUIRE(identity_access::ReplyRouteArmWith(t.ng, &tap));
    REQUIRE(identity_access::ReplyRoutePromoted());

    JoinDesc desc{};
    desc.xplatformId   = 0xDEC0DE1234567890ull;
    desc.team          = 1;
    desc.role          = 0;
    desc.useSpawnPoint = true;
    desc.entrantJson.p = reinterpret_cast<const uint8_t*>(kEntrantJson);
    desc.entrantJson.n = uint32_t(sizeof kEntrantJson - 1);
    FillDistinct(desc.loadoutSlots);
    desc.loadoutNumber     = 21;
    desc.loadoutNumberBody = 7;
    const uint16_t kSlot = 3;

    // Park the request wrong-phase, exactly as the tick does in-match.
    UserIndex out(identity::kNotDressed);
    CHECK_FALSE(id->ProvisionReplayUser(UserSlot(kSlot), desc, out));
    CHECK(id->LastProvisionRefusal() == PawnProvision::Refusal::ReplyWrongPhase);
    {
        const auto c = identity_access::ReplyRouteStatus();
        CHECK(c.queued      == 1);
        CHECK(c.driverCalls == 0);   // the drain driver has not fired yet
        CHECK(c.drained     == 0);
        CHECK(c.published   == 0);
    }
    CHECK(g_pub.calls == 0);

    // ---- A NON-TICK STAGE IS FILTERED OUT.  CR15NetGame::Update runs four times a frame on a stage
    // index; only stage 0 is the tick.  A non-tick stage must not even ENTER the drain, so driverCalls
    // stays 0 -- without this filter the drain would fire four times a frame, three on the wrong stage.
    {
        const auto c = identity_access::ReplyDrainNetGameUpdateForTest(t.ng, tick::kTickStage + 1);
        CHECK(c.driverCalls == 0);
        CHECK(c.drained     == 0);
        CHECK(c.published   == 0);
    }
    CHECK(g_pub.calls == 0);

    // ---- OUT OF A MATCH THE TICK STAGE IS FILTERED TOO.  The detour lives for the process; once the
    // session has ended the route's engine objects are going with the level, and the parked request
    // waits rather than publish into them.
    {
        const uint32_t readyForGame = facts::NETGAME_STATE_INMATCH - 1;
        memcpy(t.ng, &readyForGame, sizeof readyForGame);
        const auto c = identity_access::ReplyDrainNetGameUpdateForTest(t.ng, tick::kTickStage);
        CHECK(c.driverCalls == 0);
        CHECK(c.published   == 0);
        CHECK(c.queued      == 1);
        const auto none = identity_access::ReplyDrainNetGameUpdateForTest(nullptr, tick::kTickStage);
        CHECK(none.driverCalls == 0);
        memcpy(t.ng, &inMatch, sizeof inMatch);
    }
    CHECK(g_pub.calls == 0);

    // ---- THE TICK STAGE DRIVES THE DRAIN.  FAIL-before is the queued / published==0 above; PASS-after
    // is one detour-body call on stage 0: the driver fires (driverCalls==1) and the parked request
    // publishes through the route exactly once.  No hook and no tick.mode were consulted -- the body ran
    // off the stage filter alone, which is the tick.mode-independence the fix requires.
    {
        const auto c = identity_access::ReplyDrainNetGameUpdateForTest(t.ng, tick::kTickStage);
        CHECK(c.driverCalls == 1);
        CHECK(c.drained     == 1);
        CHECK(c.published   == 1);
    }
    CHECK(g_pub.calls == 1);
    CHECK(g_pub.node  == kNode);
    CHECK(g_pub.msgId == kMsgId);
    CHECK(strcmp(g_pub.name, "SNSLobbyEntrantAdded") == 0);
    CHECK(g_pub.slot  == kSlot);

    // ---- EXACTLY ONCE.  A second tick-stage body call finds the queue empty: the driver fires again
    // (driverCalls==2, the unconditional count) but drains nothing more.  driverCalls>0 with no further
    // drain is precisely the "fired but no-op'd" state the counter was added to make visible.
    {
        const auto c = identity_access::ReplyDrainNetGameUpdateForTest(t.ng, tick::kTickStage);
        CHECK(c.driverCalls == 2);
        CHECK(c.drained     == 1);
        CHECK(c.published   == 1);
    }
    CHECK(g_pub.calls == 1);

    identity_access::ReplyRouteDisarm();
    identity_access::ReplyRouteInjectForTest(nullptr, nullptr, 0);
    identity_access::Destroy(id);
    g_routeNg = nullptr;
}


// DIAG-DRAIN-NOOP Q2 pivot fixtures: a stand-in for the engine's own EntrantAdded handler, so the
// SHIPPED direct path can be driven with no image mapped.  It reads a2 exactly as sub_14017B850 does --
// **(a2+16) is the entrant slot key -- so the test proves the fabricated 40-byte argument's +16 field
// really reaches the handler as the slot.
namespace {
struct DirectRecord {
    uint32_t calls = 0;
    void*    a1    = nullptr;
    uint64_t key   = ~uint64_t(0);
};
DirectRecord g_direct;
void StandInEntrantAdded(void* a1, void* a2) {
    ++g_direct.calls;
    g_direct.a1 = a1;
    uint64_t* pKey = *reinterpret_cast<uint64_t**>(reinterpret_cast<uint8_t*>(a2) + 16);
    g_direct.key = pKey != nullptr ? *pKey : ~uint64_t(0);
}
// A stand-in for the engine's own NSLobbyRemovingEntrant handler sub_14017D240, which
// reads a2 EXACTLY as the add handler does -- **(a2+16) is the lobby slot -- so this proves the
// fabricated 40-byte argument's +16 field reaches the leave handler as the slot, byte for byte.
// It also does what the engine's teardown does to the two words the postcondition reads, so the
// postcondition is exercised against a slot that really was freed.
struct LeaveRecord {
    uint32_t calls = 0;
    void*    a1    = nullptr;
    uint64_t key   = ~uint64_t(0);
    bool     actuallyFree = true;   // off: the handler "ran" and freed nothing (EntrantStillSeated)
    void*    ngForFree = nullptr;
};
LeaveRecord g_leaveRec;
void StandInEntrantRemoved(void* a1, void* a2) {
    ++g_leaveRec.calls;
    g_leaveRec.a1 = a1;
    uint64_t* pKey = *reinterpret_cast<uint64_t**>(reinterpret_cast<uint8_t*>(a2) + 16);
    g_leaveRec.key = pKey != nullptr ? *pKey : ~uint64_t(0);
    if (!g_leaveRec.actuallyFree || g_leaveRec.ngForFree == nullptr || pKey == nullptr) return;
    // The engine's teardown leaves the freed slot's table word naming an index past the user count.
    // Reproduced rather than zeroed: it is the state the cross-check has to answer "free" for.
    uint8_t* ng = static_cast<uint8_t*>(g_leaveRec.ngForFree);
    uint16_t count = 0;
    memcpy(&count, ng + facts::IDENT_ENTRANT_COUNT_OFF, sizeof count);
    if (count) --count;
    memcpy(ng + facts::IDENT_ENTRANT_COUNT_OFF, &count, sizeof count);
    const uint16_t past = count;
    memcpy(ng + facts::IDENT_SLOT_TABLE_OFF + 4 * size_t(*pKey), &past, sizeof past);
}

// A netGame big enough for the words the leave reads: the slot table at IDENT.SLOT_TABLE_OFF, the
// user array at USER.ARRAY_OFF and the two counts the removing-entrant handler compares against.
struct FakeLeaveNetGame {
    // And for the PAWN RECORDS the leave restores: a buffer that stopped at the
    // user array would have let the production write land past its end, where Readable() -- which
    // asks the page, not the object -- would have said yes.
    static constexpr size_t kUserEnd = facts::USER_ARRAY_OFF + facts::USER_STRIDE * 8;
    static constexpr size_t kPawnEnd =
        facts::NETGAME_PAWN_TABLE_OFF + size_t(facts::NETGAME_PAWN_TABLE_STRIDE) * 8 + 16;
    uint8_t buf[kUserEnd > kPawnEnd ? kUserEnd : kPawnEnd] = {};

    // ng + 592*slot + 384 / + 392, the two words SET_PLAYER_ACTOR owns.
    size_t PawnActorOff(uint16_t slot) const {
        return facts::NETGAME_PAWN_TABLE_OFF + size_t(facts::NETGAME_PAWN_TABLE_STRIDE) * slot + 8;
    }
    void SetPawnActor(uint16_t slot, uint64_t space, uint64_t actor) {
        memcpy(buf + PawnActorOff(slot), &space, sizeof space);
        memcpy(buf + PawnActorOff(slot) + 8, &actor, sizeof actor);
    }
    uint64_t PawnSpace(uint16_t slot) const {
        uint64_t v = 0;
        memcpy(&v, buf + PawnActorOff(slot), sizeof v);
        return v;
    }
    uint64_t PawnActor(uint16_t slot) const {
        uint64_t v = 0;
        memcpy(&v, buf + PawnActorOff(slot) + 8, sizeof v);
        return v;
    }

    void Build(uint16_t users, uint16_t locals) {
        memset(buf, 0, sizeof buf);
        const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
        memcpy(buf, &inMatch, sizeof inMatch);
        memcpy(buf + facts::IDENT_ENTRANT_COUNT_OFF, &users, sizeof users);
        memcpy(buf + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, &locals, sizeof locals);
        for (uint16_t s = 0; s < 16; ++s) {
            const uint16_t none = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
            memcpy(buf + facts::IDENT_SLOT_TABLE_OFF + 4 * size_t(s), &none, sizeof none);
        }
    }
    // Seat entrant `index` in lobby slot `slot`: the slot table names the index and the entrant's
    // own packed word names the slot back, which is the pair both the admission postcondition and
    // the leave's cross-check read.
    void Seat(uint16_t slot, uint16_t index) {
        memcpy(buf + facts::IDENT_SLOT_TABLE_OFF + 4 * size_t(slot), &index, sizeof index);
        const uint16_t word = uint16_t(slot & facts::IDENT_USER_SLOT_MASK);
        memcpy(buf + facts::USER_ARRAY_OFF + size_t(facts::USER_STRIDE) * index, &word, sizeof word);
    }
    void* Ng() { return buf; }
};

FakeLeaveNetGame* g_leaveNg = nullptr;
void* LeaveNetGame() { return g_leaveNg ? g_leaveNg->Ng() : nullptr; }

// A broadcaster node whose router (*node) is NULL -- the exact offline condition that made the old
// manufactured dispatch bail at identity_access:3668.  The direct path never reads it; binding it proves
// the router dependency is gone.
uint64_t g_nullRouterCell = 0;
}  // namespace

// The leave feed, end to end against the engine surface: the exact bytes the engine's
// own removing-entrant handler reads, the sets it must never be pointed at, and the postcondition
// asked of the engine rather than of the call's return value.
TEST_CASE("the leave feeds the engine's handler the slot at **(a2+16) and proves the slot is free") {
    static FakeLeaveNetGame ng;
    static FakeTap tap;
    tap = FakeTap{};
    ng.Build(/*users*/ 4, /*locals*/ 1);
    ng.Seat(/*slot*/ 0, /*index*/ 0);   // the VIEWER: a local user this plugin never admitted
    ng.Seat(/*slot*/ 3, /*index*/ 2);   // ours
    // Index 1, not 3: the stand-in teardown decrements the user count without compacting the way
    // the engine does, and an entrant parked at the top would drop out of range for the wrong
    // reason -- the case below is about the POSTCONDITION, not about a stale index.
    ng.Seat(/*slot*/ 5, /*index*/ 1);   // ours
    livesafety::SetForTest(false, true);
    g_leaveNg = &ng;
    g_leaveRec = LeaveRecord{};
    g_leaveRec.ngForFree = ng.Ng();

    identity_access::ForgetAdmittedSlotsForTest();
    identity_access::ReplyLeaveInjectForTest(&StandInEntrantRemoved);
    IIdentity* id = identity_access::Create(&LeaveNetGame, nullptr);
    REQUIRE(id != nullptr);
    // NOT required to promote: promotion is the ADD route's condition (its handler is not injected
    // here).  The arm still publishes the receiver and byte-anchors/injects the leave handler, which
    // is all the release half reads -- and a build that cannot admit must still be able to release.
    (void)identity_access::ReplyRouteArmWith(ng.Ng(), &tap);

    using replay::identity_access::Refusal;

    // ★ NOT CONTAINED, NOTHING RUNS. Asked first, before the ledger is even consulted: in a live
    // session these entrants are real people and the leave would disconnect one of them.
    livesafety::ResetForTest();
    identity_access::SeedAdmittedSlotForTest(3);
    CHECK_FALSE(id->ReleaseReplayUser(UserSlot(3)));
    CHECK(identity_access::LastRefusal(id) == Refusal::ReleaseNotContained);
    CHECK(g_leaveRec.calls == 0);
    identity_access::ForgetAdmittedSlotsForTest();
    livesafety::SetForTest(false, true);

    // ★ THE VIEWER. Seated, local, and not in the ledger: refused before anything is computed about
    // it, and the engine's handler is not reached at all.
    CHECK_FALSE(id->ReleaseReplayUser(UserSlot(0)));
    CHECK(identity_access::LastRefusal(id) == Refusal::NotOurEntrant);
    CHECK(g_leaveRec.calls == 0);

    // A slot we DID admit but which the engine seats in the local band is refused by name too: the
    // engine's own handler refuses that set silently, and reaching it would be a bug of ours.
    identity_access::SeedAdmittedSlotForTest(0);
    CHECK_FALSE(id->ReleaseReplayUser(UserSlot(0)));
    CHECK(identity_access::LastRefusal(id) == Refusal::EntrantIsLocal);
    CHECK(g_leaveRec.calls == 0);
    identity_access::ForgetAdmittedSlotsForTest();

    // ★ THE BYTES. One call, the receiver the handler was registered against, and the lobby slot as
    // the qword at **(a2+16) -- not the entrant index, which for this slot is a different number.
    identity_access::SeedAdmittedSlotForTest(3);
    // Records 1 and 2 carry LIVE pooled-actor guids, as they do after a load; record
    // 5 is past the engine's user count, which is what a departed entrant's record looks like once
    // the teardown has compacted over it.
    ng.SetPawnActor(1, 0x3F9915D3001DC28Eull, 0x000000000007003Bull);
    ng.SetPawnActor(2, 0x3F9915D3001DC28Eull, 0x000000000006003Bull);
    ng.SetPawnActor(5, 0x3F9915D3001DC28Eull, 0x000000000005003Bull);
    CHECK(id->ReleaseReplayUser(UserSlot(3)));
    CHECK(g_leaveRec.calls == 1);
    // The vacant record is restored to exactly what a record that never held a pawn holds, which is
    // what the next load's admission has to find...
    CHECK(ng.PawnSpace(5) == 0xFFFFFFFFFFFFFFFFull);
    CHECK(ng.PawnActor(5) == 0x0000FFFFFFFFFFFFull);
    // ...and the LIVE entrants' pawns below the count are untouched. The teardown compacts, so a
    // sweep bounded by the leaving slot rather than by the count wipes a pawn somebody still holds.
    CHECK(ng.PawnActor(1) == 0x000000000007003Bull);
    CHECK(ng.PawnActor(2) == 0x000000000006003Bull);
    CHECK(g_leaveRec.a1 == ng.Ng());
    CHECK(g_leaveRec.key == 3u);
    // ...and the ledger bit goes with it, so a second leave cannot fire at whatever the engine
    // seats in that slot next.
    CHECK((identity_access::AdmittedSlotMask() & (1u << 3)) == 0u);
    CHECK_FALSE(id->ReleaseReplayUser(UserSlot(3)));
    CHECK(identity_access::LastRefusal(id) == Refusal::NotOurEntrant);
    CHECK(g_leaveRec.calls == 1);

    // ★ THE POSTCONDITION IS THE ENGINE'S ANSWER, NOT THE CALL'S. A handler that runs and frees
    // nothing is a refusal, and the slot keeps its ledger bit so the next pass asks again.
    g_leaveRec.actuallyFree = false;
    ng.SetPawnActor(6, 0x3F9915D3001DC28Eull, 0x000000000004003Bull);
    identity_access::SeedAdmittedSlotForTest(5);
    CHECK_FALSE(id->ReleaseReplayUser(UserSlot(5)));
    CHECK(identity_access::LastRefusal(id) == Refusal::EntrantStillSeated);
    CHECK(g_leaveRec.calls == 2);
    CHECK((identity_access::AdmittedSlotMask() & (1u << 5)) != 0u);
    // A leave that did not complete sweeps nothing: the engine is in a state this pass did not
    // establish, so the record it left behind is not this pass's to rewrite.
    CHECK(ng.PawnActor(6) == 0x000000000004003Bull);

    g_leaveRec.actuallyFree = true;
    CHECK(id->ReleaseReplayUser(UserSlot(5)));
    CHECK(g_leaveRec.key == 5u);
    CHECK(ng.PawnActor(6) == 0x0000FFFFFFFFFFFFull);
    CHECK((identity_access::AdmittedSlotMask() & (1u << 5)) == 0u);

    identity_access::ReplyRouteDisarm();
    identity_access::ReplyLeaveInjectForTest(nullptr);
    identity_access::ForgetAdmittedSlotsForTest();
    identity_access::Destroy(id);
    livesafety::ResetForTest();
    g_leaveNg = nullptr;
}

TEST_CASE("a stale slot-table word is not a target, and an unbound handler refuses by name") {
    static FakeLeaveNetGame ng;
    static FakeTap tap;
    tap = FakeTap{};
    ng.Build(/*users*/ 2, /*locals*/ 1);
    ng.Seat(0, 0);
    // Slot 7's table word names entrant 1 -- but entrant 1's own record names slot 0 back. This is
    // exactly the state the engine's teardown leaves behind once a later admission grows the user
    // count back over the freed slot's word, and the engine's own handler does NOT check it: fed
    // this slot it would tear down entrant 1, which is the viewer.
    const uint16_t one = 1;
    memcpy(ng.buf + facts::IDENT_SLOT_TABLE_OFF + 4 * 7, &one, sizeof one);
    g_leaveNg = &ng;
    g_leaveRec = LeaveRecord{};
    g_leaveRec.ngForFree = nullptr;
    livesafety::SetForTest(false, true);

    identity_access::ForgetAdmittedSlotsForTest();
    identity_access::ReplyLeaveInjectForTest(&StandInEntrantRemoved);
    IIdentity* id = identity_access::Create(&LeaveNetGame, nullptr);
    REQUIRE(id != nullptr);
    // NOT required to promote: promotion is the ADD route's condition (its handler is not injected
    // here).  The arm still publishes the receiver and byte-anchors/injects the leave handler, which
    // is all the release half reads -- and a build that cannot admit must still be able to release.
    (void)identity_access::ReplyRouteArmWith(ng.Ng(), &tap);

    identity_access::SeedAdmittedSlotForTest(7);
    CHECK(id->ReleaseReplayUser(UserSlot(7)));   // nothing of ours is there: the release is complete
    CHECK(g_leaveRec.calls == 0);                // and NOTHING was fed to the engine
    CHECK((identity_access::AdmittedSlotMask() & (1u << 7)) == 0u);

    // With no handler bound -- an image where the sibling does not byte-anchor -- the answer is the
    // old one by name, and adoption stays the fallback.
    identity_access::ReplyRouteDisarm();
    identity_access::ReplyLeaveInjectForTest(nullptr);
    // NOT required to promote: promotion is the ADD route's condition (its handler is not injected
    // here).  The arm still publishes the receiver and byte-anchors/injects the leave handler, which
    // is all the release half reads -- and a build that cannot admit must still be able to release.
    (void)identity_access::ReplyRouteArmWith(ng.Ng(), &tap);
    identity_access::SeedAdmittedSlotForTest(0);
    CHECK_FALSE(id->ReleaseReplayUser(UserSlot(0)));
    CHECK(identity_access::LastRefusal(id) == replay::identity_access::Refusal::NoReleasePath);

    identity_access::ReplyRouteDisarm();
    identity_access::ForgetAdmittedSlotsForTest();
    identity_access::Destroy(id);
    livesafety::ResetForTest();
    g_leaveNg = nullptr;
}

// DIAG-DRAIN-NOOP Q2: the reply route now calls the engine's own EntrantAdded handler DIRECTLY instead
// of manufacturing a broadcaster dispatch, because offline the broadcaster's router (*node) is NULL and
// the old path bailed on it every frame (identity_access:3668, proven in-vivo).  This drives the SHIPPED
// direct path end to end -- through the real queue, wrong-phase deferral, drain and ProvisionViaReply --
// with a stand-in handler injected and the node bound at a NULL router.  FAIL-before is structural: the
// deleted dispatch path read *node and returned ReplyNotDispatched when it was null; PASS-after is that
// the direct call still reaches the handler (g_direct.calls==1) and admits (directCalls/dispatchedLocal),
// with no router touched at all.
TEST_CASE("DIAG-DRAIN-NOOP Q2: the direct handler call admits with a NULL broadcaster router") {
    LegacyRouteScope legacyRoute_;
    static FakeSlotTable t;
    static FakeTap tap;
    tap = FakeTap{};
    g_direct = DirectRecord{};
    t.Build(kEntries);
    const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
    memcpy(t.ng, &inMatch, sizeof inMatch);
    g_routeNg = &t;

    void* const    kNode  = &g_nullRouterCell;   // *node == null: the offline router-null condition
    const uint64_t kMsgId = 0x51A73BEEF0001234ull;
    identity_access::ReplyDirectInjectForTest(kNode, &StandInEntrantAdded, kMsgId);

    IIdentity* id = identity_access::Create(&RouteNetGame, nullptr);
    REQUIRE(id != nullptr);
    REQUIRE(identity_access::ReplyRouteArmWith(t.ng, &tap));
    REQUIRE(identity_access::ReplyRoutePromoted());   // promoted on the receiver + the injected handler

    JoinDesc desc{};
    desc.xplatformId   = 0xDEC0DE1234567890ull;
    desc.team          = 1;
    desc.role          = 0;
    desc.useSpawnPoint = true;
    desc.entrantJson.p = reinterpret_cast<const uint8_t*>(kEntrantJson);
    desc.entrantJson.n = uint32_t(sizeof kEntrantJson - 1);
    FillDistinct(desc.loadoutSlots);
    desc.loadoutNumber     = 21;
    desc.loadoutNumberBody = 7;
    const uint16_t kSlot = 3;

    // FAIL-before-the-drain: the tick parks the request wrong-phase and the handler is never reached.
    UserIndex out(identity::kNotDressed);
    CHECK_FALSE(id->ProvisionReplayUser(UserSlot(kSlot), desc, out));
    CHECK(id->LastProvisionRefusal() == PawnProvision::Refusal::ReplyWrongPhase);
    CHECK(g_direct.calls == 0);
    {
        const auto c = identity_access::ReplyRouteStatus();
        CHECK(c.queued      == 1);
        CHECK(c.directCalls == 0);
        CHECK(c.published   == 0);
    }

    // PASS-after: the stage-0 drain runs the direct handler call from a real engine-update stack.  Even
    // though *node is NULL, the handler is reached exactly once and the entrant admitted through it.
    const auto c = identity_access::ReplyDrainFromEngineHeartbeat();
    CHECK(c.drained         == 1);
    CHECK(c.directCalls     == 1);      // the guarded direct call was issued
    CHECK(c.dispatchedLocal == 1);      // AddRemoteUser-dispatch proxy: the handler ran without faulting
    CHECK(c.published       == 1);
    CHECK(c.lastBail == identity_access::ReplyBail::None);
    CHECK(g_direct.calls == 1);
    CHECK(g_direct.a1    == t.ng);      // the receiver is the CR15NetGame the route armed on
    CHECK(g_direct.key   == kSlot);     // **(a2+16) is the queued slot -- the fabricated arg's +16 field

    // Exactly once: a second drain finds the queue empty and issues no second call.
    const auto c2 = identity_access::ReplyDrainFromEngineHeartbeat();
    CHECK(c2.directCalls == 1);
    CHECK(g_direct.calls == 1);

    identity_access::ReplyRouteDisarm();
    identity_access::ReplyDirectInjectForTest(nullptr, nullptr, 0);
    identity_access::Destroy(id);
    g_routeNg = nullptr;
}

// DIAG-DRAIN-NOOP Q2: the direct call's guard ladder.  Option-A-class risk is a wrong/unreadable
// receiver faulting the process, so every refusal must be clean and named.  ReplyDirectCallForTest drives
// DirectEntrantAddedCall in isolation (no seed, no postcondition hooks) so each guard can be hit directly.
TEST_CASE("DIAG-DRAIN-NOOP Q2: the direct handler call refuses a null/unreadable receiver, cleanly") {
    static uint64_t receiver = 0;   // a readable, mapped stand-in for the CR15NetGame the handler gets

    // Null receiver: refused ReceiverNull, no call issued.
    g_direct = DirectRecord{};
    CHECK(identity_access::ReplyDirectCallForTest(nullptr, &StandInEntrantAdded, 5) ==
          identity_access::ReplyBail::ReceiverNull);
    CHECK(g_direct.calls == 0);

    // Non-null but unreadable receiver (the null page): refused ReceiverUnreadable, never dereferenced.
    CHECK(identity_access::ReplyDirectCallForTest(reinterpret_cast<void*>(uintptr_t(1)),
                                                  &StandInEntrantAdded, 5) ==
          identity_access::ReplyBail::ReceiverUnreadable);
    CHECK(g_direct.calls == 0);

    // Handler not bound (a rebase that moved sub_14017B850): refused HandlerUnbound.
    CHECK(identity_access::ReplyDirectCallForTest(&receiver, nullptr, 5) ==
          identity_access::ReplyBail::HandlerUnbound);
    CHECK(g_direct.calls == 0);

    // A readable receiver and a bound handler: admits, and the fabricated arg carries the slot to +16.
    CHECK(identity_access::ReplyDirectCallForTest(&receiver, &StandInEntrantAdded, 5) ==
          identity_access::ReplyBail::None);
    CHECK(g_direct.calls == 1);
    CHECK(g_direct.a1    == &receiver);
    CHECK(g_direct.key   == 5);

    identity_access::ReplyRouteDisarm();
}


// BLOCKER-2 (DIAG-REPLYPHASE section 3).  The Stage-B authority flip touches a LIVE session word, so
// the guard MUST leave it byte-identical after flip+restore on every path -- the normal return AND the
// SEH fault path (the engine call can raise; C++ destructors do not unwind for SEH under /EHsc, so the
// restore is explicit).  This drives the exact shipped guard (InvokeSpawnNotify) over a local cell.
namespace stagebflip {
uint64_t* g_cell     = nullptr;
uint64_t  g_observed = 0;
int       g_calls    = 0;
void ObserveNotify(void*, uint16_t) {
    ++g_calls;
    g_observed = g_cell != nullptr ? *g_cell : 0;   // what the engine would branch on mid-call
}
void FaultNotify(void*, uint16_t) {
    ++g_calls;
    *reinterpret_cast<volatile int*>(0) = 1;        // access violation -> the guard must still restore
}
}  // namespace stagebflip

TEST_CASE("BLOCKER-2: the authority-flip guard restores the mode word byte-identically, incl. the fault path") {
    using namespace stagebflip;
    const uint64_t kHost = 0x0000082620824040ull;   // the offline session word: host, bit1 clear

    // flip=false: the word is never touched and the notify observes exactly what was there.
    {
        uint64_t cell = kHost; g_cell = &cell; g_observed = 0; g_calls = 0;
        const bool ok =
            identity_access::StageBAuthFlipProbeForTest(&ObserveNotify, nullptr, 0, &cell, false);
        CHECK(ok);
        CHECK(g_calls == 1);
        CHECK(g_observed == kHost);   // unflipped
        CHECK(cell == kHost);         // untouched
    }

    // flip=true, normal return: the notify observes the NON-HOST word mid-call; restored after.
    {
        uint64_t cell = kHost; g_cell = &cell; g_observed = 0; g_calls = 0;
        const bool ok =
            identity_access::StageBAuthFlipProbeForTest(&ObserveNotify, nullptr, 0, &cell, true);
        CHECK(ok);
        CHECK(g_calls == 1);
        CHECK(g_observed == (kHost | 0x4));   // the engine branched on the flipped (non-host) word
        // and that effective word takes the REMOTE arm: host == false.
        const uint64_t m = g_observed;
        CHECK((((m & 2) != 0) || (((m & 4) == 0) && ((m & 0x40) != 0))) == false);
        CHECK(cell == kHost);                 // byte-identical after restore
    }

    // flip=true, the engine call FAULTS: the guard contains it (returns false) and STILL restores.
    {
        uint64_t cell = kHost; g_cell = &cell; g_calls = 0;
        const bool ok =
            identity_access::StageBAuthFlipProbeForTest(&FaultNotify, nullptr, 0, &cell, true);
        CHECK_FALSE(ok);              // contained -- echovr would not crash
        CHECK(g_calls == 1);
        CHECK(cell == kHost);         // restored on the exception path
    }

    g_cell = nullptr;
}


// -------------------------------------------------------------------------------------------
// P1-24-VERIFY: THE ACTOR HOP CHAIN, AGAINST A FAKE ENGINE.
//
// The four hooks call real VAs through Guarded<>, which answers nullptr in a process that is not
// echovr.exe -- so before this seam existed every hop read false here and no test could tell a
// correct binding from a wrong one.  The surface takes a HopCallTable instead, and these cases
// assert the two things bytes cannot: WHICH space each call is aimed at, and that argument 2 of
// the enable arrives as the whole 64-bit word pair.
namespace {

constexpr uint16_t kHopIndex  = 3;
// Never equal, and each half of the guid distinct in both words: a chain that pushed guid.lo
// where guid.hi belongs, or the low half of either, cannot land on the expected value by accident.
constexpr uint64_t kHopGuidLo = 0x1122334455667788ull;
constexpr uint64_t kHopGuidHi = 0x0000000700000002ull;   // { actorIndex 2, poolIndex 7 }

struct FakeHopEngine {
    void* cs   = nullptr;      // [ng+NETGAME.CS_SPACE_OFF], the CncaGameSpace
    void* root = nullptr;      // [ng+IDENT.NETGAME_GAMESPACE_OFF], the CR15Game
    bool  registered  = false; // hop 1: the lookup on the CHILD space answers non-null
    bool  enabled     = false; // hop 2: what the engine's own predicate says
    bool  enableTakes = true;  // ... and whether a correctly aimed enable moves it

    unsigned    lookups = 0, predicates = 0, enables = 0;
    void*       sawLookupSpace = nullptr;
    uint64_t    sawLookupKey = 0;
    void*       sawPredicateSpace = nullptr;
    const void* sawPredicateGuid = nullptr;
    void*       sawEnableSpace = nullptr;
    uint64_t    sawEnableWords = 0;
};

// doctest runs cases serially in one thread; the fixture owns both for the length of a case.
FakeHopEngine* g_hopEng = nullptr;
void*          g_hopNgPtr = nullptr;

void* FakeHopNg() { return g_hopNgPtr; }

void* FakeLookup(void* space, uint64_t key) {
    g_hopEng->lookups++;
    g_hopEng->sawLookupSpace = space;
    g_hopEng->sawLookupKey = key;
    return g_hopEng->registered ? static_cast<void*>(g_hopEng) : nullptr;
}

uint8_t FakeIsEnabled(void* root, const void* guid) {
    g_hopEng->predicates++;
    g_hopEng->sawPredicateSpace = root;
    g_hopEng->sawPredicateGuid = guid;
    return g_hopEng->enabled ? 1u : 0u;
}

uint64_t FakeEnable(void* container, uint64_t words) {
    g_hopEng->enables++;
    g_hopEng->sawEnableSpace = container;
    g_hopEng->sawEnableWords = words;
    // The engine's enable is a flag clear on the object the CONTAINER names, indexed by the whole
    // word pair.  A call aimed at the other space, or one that lost the pool-index half, moves
    // nothing -- and still reports success, exactly like the shipped one.
    if (g_hopEng->enableTakes && container == g_hopEng->cs && words == kHopGuidHi)
        g_hopEng->enabled = true;
    return 1;
}

constexpr size_t kHopGuidOff = size_t(facts::IDENT_USER_ACTOR_GUID_OFF) +
                               size_t(facts::USER_STRIDE) * kHopIndex;
static_assert(kHopGuidOff + 16 < size_t(facts::NETGAME_CS_SPACE_OFF),
              "the fake netgame is sized off the CS pointer; the guid must fit below it");

// The engine-side halves of A2 and A3, and the store the two setters write.  Every one records
// what it was HANDED, because the argument shape is the thing under test.
struct FakeKvStore {
    uint64_t slotCount = 32;
    bool     flatSide = true;
    unsigned selects = 0, flatSets = 0, slotSets = 0, spawnedGets = 0, assigns = 0;
    char     sawFlatKey[64] = {0};
    char     sawSlotKey[64] = {0};
    uint64_t sawSlot = ~0ull;
    void*    sawStore = nullptr;
    uint16_t sawAssignIndex = replay::kSlotUnmapped;
    uint32_t sawAssignRole = UINT32_MAX;
    bool     spawned = false;
};
FakeKvStore* g_kv = nullptr;

uint8_t FakeSideSelect(void* store, uint64_t slot) {
    ++g_kv->selects;
    g_kv->sawStore = store;
    g_kv->sawSlot = slot;
    return g_kv->flatSide ? 1 : 0;
}
void FakeSetFlat(void*, const char* key, int32_t v) {
    ++g_kv->flatSets;
    snprintf(g_kv->sawFlatKey, sizeof g_kv->sawFlatKey, "%s", key);
    if (v) g_kv->spawned = true;
}
void FakeSetSlot(void*, uint64_t slot, const char* key, int32_t v) {
    ++g_kv->slotSets;
    g_kv->sawSlot = slot;
    snprintf(g_kv->sawSlotKey, sizeof g_kv->sawSlotKey, "%s", key);
    if (v) g_kv->spawned = true;
}
uint64_t FakeSpawnedGet(void*, uint64_t) {
    ++g_kv->spawnedGets;
    return g_kv->spawned ? 1u : 0u;
}
void FakeAssignTeam(void*, uint16_t index, uint32_t role) {
    ++g_kv->assigns;
    g_kv->sawAssignIndex = index;
    g_kv->sawAssignRole = role;
}

// ENGINE.ACTOR_GUID_CANON WRITES THROUGH its argument.  This stand-in writes a pattern nothing
// else in the fixture produces, so a hop that handed it the engine's own user record instead of a
// copy leaves that record visibly altered.
unsigned g_canons = 0;
void*    g_sawCanonRoot = nullptr;
uint64_t g_canonLowOut = 0;
void FakeCanon(void* root, void* guid16) {
    ++g_canons;
    g_sawCanonRoot = root;
    memset(guid16, 0xAA, 16);
    if (g_canonLowOut != 0) *static_cast<uint64_t*>(guid16) = g_canonLowOut;
}

struct HopFixture {
    uint8_t  ng[size_t(facts::NETGAME_CS_SPACE_OFF) + sizeof(void*)] = {};
    uint8_t  rootObj = 0;      // two distinct addresses: the packet's whole point is they differ
    uint8_t  csObj = 0;
    FakeHopEngine eng;
    FakeKvStore kv;
    uint8_t storeObj[64] = {};
    identity_access::HopCallTable calls;
    identity_access::ActorHopSurface surface;

    HopFixture() {
        eng.root = &rootObj;
        eng.cs   = &csObj;
        *reinterpret_cast<void**>(ng + facts::IDENT_NETGAME_GAMESPACE_OFF) = &rootObj;
        *reinterpret_cast<void**>(ng + facts::NETGAME_CS_SPACE_OFF)        = &csObj;
        *reinterpret_cast<uint64_t*>(ng + kHopGuidOff)     = kHopGuidLo;
        *reinterpret_cast<uint64_t*>(ng + kHopGuidOff + 8) = kHopGuidHi;
        // The entrant count must cover the index every hook bounds itself with, and the packed
        // word carries the lobby slot A2's key is built from.
        *reinterpret_cast<uint16_t*>(ng + facts::IDENT_ENTRANT_COUNT_OFF) = kHopIndex + 1;
        *reinterpret_cast<uint16_t*>(ng + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF) = 0;
        SetSlot(9);
        calls.lookup     = FakeLookup;
        calls.isEnabled  = FakeIsEnabled;
        calls.canon      = FakeCanon;
        calls.spawnedGet = FakeSpawnedGet;
        calls.sideSelect = FakeSideSelect;
        calls.setFlat    = FakeSetFlat;
        calls.setSlot    = FakeSetSlot;
        calls.assignTeam = FakeAssignTeam;
        calls.store      = storeObj;
        calls.storeSlotCount = &kv.slotCount;
        g_hopEng   = &eng;
        g_hopNgPtr = ng;
        g_kv       = &kv;
        g_canons = 0;
        g_sawCanonRoot = nullptr;
        g_canonLowOut = kHopGuidLo;   // canon answers the key the fake table is registered under
        surface.netGame = FakeHopNg;
        surface.calls   = &calls;
        identity_access::BindActorHops(surface);
    }
    ~HopFixture() { g_hopEng = nullptr; g_hopNgPtr = nullptr; g_kv = nullptr; }

    void SetSlot(uint16_t slot) {
        uint16_t& word = *reinterpret_cast<uint16_t*>(
            ng + size_t(facts::USER_ARRAY_OFF) +
            size_t(facts::USER_STRIDE) * kHopIndex);
        word = uint16_t((word & ~uint16_t(facts::IDENT_USER_SLOT_MASK)) |
                        (slot & uint16_t(facts::IDENT_USER_SLOT_MASK)));
    }

    PawnProvision::Hop Advance(PawnProvision::HopDrive* d = nullptr) {
        return PawnProvision::AdvanceActorHops(surface.hops, kHopIndex, d);
    }
};

}  // namespace

// S14.7 test 1 -- and it is a CORRECTION, not a new hop: the table the lookup searches lives on
// the ROOT, and every site in the image supplies [ng+8].
TEST_CASE("W2-P1-29: hop 1 searches the ROOT's guid table, not the child container") {
    HopFixture f;

    SUBCASE("an unregistered actor stops the chain at its own hop and authors nothing") {
        CHECK(f.Advance() == PawnProvision::Hop::RemotePlayerCs);
        CHECK(PawnProvision::HopRefusal(PawnProvision::Hop::RemotePlayerCs) ==
              PawnProvision::Refusal::NotInRemotePlayerCs);
        CHECK(f.eng.lookups == 1);
        CHECK(f.eng.sawLookupSpace == &f.rootObj);
        CHECK(f.eng.sawLookupSpace != &f.csObj);
        CHECK(f.kv.flatSets == 0);
        CHECK(f.kv.slotSets == 0);
        CHECK(f.kv.assigns == 0);
        CHECK(f.eng.predicates == 0);
    }

    SUBCASE("the canon runs on the ROOT too, and its answer is the lookup's key") {
        f.eng.registered = true;
        CHECK(f.Advance() != PawnProvision::Hop::RemotePlayerCs);
        CHECK(g_canons == 1);
        CHECK(g_sawCanonRoot == &f.rootObj);
        CHECK(f.eng.sawLookupKey == kHopGuidLo);
    }

    SUBCASE("a netgame that answers null refuses without calling the engine at all") {
        g_hopNgPtr = nullptr;
        CHECK(f.Advance() == PawnProvision::Hop::RemotePlayerCs);
        CHECK(f.eng.lookups == 0);
        CHECK(g_canons == 0);
    }
}

// S14.7 test 2 -- the canon WRITES THROUGH its argument, and the source is the engine's own user
// record.  A hop that handed it that record would corrupt the entrant's guid on every pass.
TEST_CASE("W2-P1-29: the guid is copied before the canon, and the engine's record is untouched") {
    HopFixture f;
    f.eng.registered = true;
    uint8_t before[16];
    memcpy(before, f.ng + kHopGuidOff, sizeof before);

    CHECK(f.Advance() != PawnProvision::Hop::RemotePlayerCs);
    CHECK(g_canons == 1);
    CHECK(memcmp(before, f.ng + kHopGuidOff, sizeof before) == 0);
    CHECK(*reinterpret_cast<const uint64_t*>(f.ng + kHopGuidOff) == kHopGuidLo);
    CHECK(*reinterpret_cast<const uint64_t*>(f.ng + kHopGuidOff + 8) == kHopGuidHi);
}

// S14.7 test 3 -- A2 writes the side the READER selects, and refuses the out-of-range per-slot
// side by name rather than performing an unbounded store into the session document.
TEST_CASE("W2-P1-29: A2 writes the side the reader selects, and refuses the one it cannot") {
    HopFixture f;
    f.eng.registered = true;
    PawnProvision::DriveGate g;
    PawnProvision::HopDrive d;
    d.gate = &g;
    d.recordedRole = 1;

    SUBCASE("selector true -> the FLAT setter, with the formatted key") {
        f.kv.flatSide = true;
        (void)f.Advance(&d);
        CHECK(f.kv.selects == 1);
        CHECK(f.kv.sawStore == static_cast<void*>(f.storeObj));
        CHECK(f.kv.sawSlot == 9);
        CHECK(f.kv.flatSets == 1);
        CHECK(f.kv.slotSets == 0);
        CHECK(strcmp(f.kv.sawFlatKey, "user[9]|spawned") == 0);
        CHECK(f.kv.assigns == 1);
        CHECK(f.kv.sawAssignIndex == kHopIndex);
        CHECK(f.kv.sawAssignRole == 1u);
    }

    SUBCASE("selector false -> the per-slot setter, with the slot and the derived key") {
        f.kv.flatSide = false;
        (void)f.Advance(&d);
        CHECK(f.kv.slotSets == 1);
        CHECK(f.kv.flatSets == 0);
        CHECK(f.kv.sawSlot == 9);
        CHECK(strcmp(f.kv.sawSlotKey, "spawned") == 0);
        CHECK(f.kv.assigns == 1);
    }

    SUBCASE("selector false and the slot past the store's own count -> a NAMED refusal, no call") {
        f.kv.flatSide = false;
        f.kv.slotCount = 4;      // the lobby slot is 9, which the per-slot record does not cover
        (void)f.Advance(&d);
        CHECK(d.outcome.state == PawnProvision::Participation::Refused);
        CHECK(d.outcome.why == PawnProvision::ParticipateRefusal::SpawnedRecordUnwritable);
        CHECK(f.kv.slotSets == 0);
        CHECK(f.kv.flatSets == 0);
        CHECK(f.kv.assigns == 0);
    }
}

// S14.7 test 4 -- our key bytes are the engine's, for every slot the packed word can hold, and the
// FORMAT is the registry's rather than a copy in our own file.
TEST_CASE("W2-P1-29: A2's key matches the engine's own formatter, and its format is the registry's") {
    CHECK(strcmp(identity_access::SpawnedKeyFormat(),
                 facts::NETGAME_USER_SPAWNED_KEY_FMT_VA_TEXT) == 0);
    CHECK(strcmp(identity_access::SpawnedSlotKey(), "spawned") == 0);

    for (uint64_t slot : {0ull, 9ull, 10ull, 31ull}) {
        char ours[64] = {0};
        char engine[64] = {0};
        CHECK(identity_access::FormatSpawnedKey(ours, sizeof ours, slot));
        snprintf(engine, sizeof engine, facts::NETGAME_USER_SPAWNED_KEY_FMT_VA_TEXT,
                 (unsigned long long)slot);
        CHECK(strcmp(ours, engine) == 0);
    }
    // The separator the per-slot key is derived from is IN the format, so the derivation cannot
    // silently answer the whole string.
    CHECK(strchr(facts::NETGAME_USER_SPAWNED_KEY_FMT_VA_TEXT, '|') != nullptr);
}

// ---- ENG-HOP4-BIND: the hop-4 state machine, over a real-layout RemotePlayer CI element -------
//
// The element is laid out at the registry's own offsets and nothing else: PLAYER.CI_USERID_OFF
// settles which entrant it belongs to, the six PLAYER.CI_REQ_*_OFF words carry the requirement
// answers InitCI stores, and PLAYER.CI_INIT_WITNESS_OFF carries the EIGHT-byte pointer InitCI
// writes last.  The component system points at the pool through PLAYER.CS_POOL_BASE_OFF and at
// the two maps through PLAYER.CS_{HANDLE,REVERSE}_MAP_OFF, which is the route the engine's own
// element accessor takes.
struct CiFixture {
    static constexpr uint16_t kHandle      = 5;
    static constexpr uint16_t kPoolSlot    = 2;
    // A second LIVE element that is not this entrant's, so the walk has to get past one before it
    // reaches the right one: a reader that just took element 0 would answer about the wrong pawn.
    static constexpr uint16_t kOtherHandle   = 9;
    static constexpr uint16_t kOtherPoolSlot = 0;

    alignas(16) uint8_t cs[size_t(facts::PLAYER_CS_POOL_BASE_OFF) + 64] = {};
    alignas(16) uint8_t pool[size_t(facts::PLAYER_CI_STRIDE) * 4] = {};
    uint16_t fwd[64] = {};    // handle -> pool index, stride 4 (two u16 halves)
    uint16_t rev[16] = {};    // dense index -> handle

    CiFixture() {
        rev[0] = kOtherHandle;
        rev[1] = kHandle;
        // A HOLE, and the engine leaves them: the dense reverse map is not compacted on removal.
        // It sits PAST this entrant so the found cases are untouched and the not-found walk has
        // to get past it -- reading the forward map at 4*0xFFFF would turn "this entrant is not in
        // the pool" into "the pool is unreadable".
        rev[2] = uint16_t(~uint16_t(0));
        fwd[2 * kOtherHandle] = kOtherPoolSlot;
        fwd[2 * kHandle]      = kPoolSlot;
        *reinterpret_cast<uint16_t*>(cs + facts::PLAYER_CS_COUNT_OFF)     = 3;
        *reinterpret_cast<void**>(cs + facts::PLAYER_CS_POOL_BASE_OFF)    = pool;
        *reinterpret_cast<void**>(cs + facts::PLAYER_CS_HANDLE_MAP_OFF)   = fwd;
        *reinterpret_cast<void**>(cs + facts::PLAYER_CS_REVERSE_MAP_OFF)  = rev;
        // The engine memsets a freshly allocated element to zero, so this fixture starts there
        // too: every requirement word 0 and no witness.  The other element keeps the zeroed user
        // id, which kHopIndex is deliberately not.
        Word(facts::PLAYER_CI_USERID_OFF) = kHopIndex;
        static_assert(kHopIndex != 0, "the decoy element's zeroed user id must not match");
    }

    uint8_t*  Element() { return pool + size_t(facts::PLAYER_CI_STRIDE) * kPoolSlot; }
    uint16_t& Word(uint32_t off) { return *reinterpret_cast<uint16_t*>(Element() + off); }
    uint64_t& Witness() {
        return *reinterpret_cast<uint64_t*>(Element() + facts::PLAYER_CI_INIT_WITNESS_OFF);
    }
    void SetUnset(uint32_t off) { Word(off) = uint16_t(facts::PLAYER_CI_REQ_UNSET); }
};

CiFixture* g_ci = nullptr;
void* FakeRemotePlayerCs() { return g_ci ? g_ci->cs : nullptr; }

TEST_CASE("Hop 4 walks the CI record from unset to witness to done") {
    HopFixture f;
    CiFixture  ci;
    g_ci = &ci;
    f.surface.remotePlayerCs = FakeRemotePlayerCs;
    const PawnProvision::ActorHops& h = f.surface.hops;
    REQUIRE(h.physicsCi != nullptr);
    REQUIRE(h.physicsCiAwaiting != nullptr);

    SUBCASE("each requirement is named in InitCI's own order, first-unset wins") {
        // Two unset at once: the engine returns on the FIRST, so a reader walking any other order
        // would name R15TouchInteract while the engine is still stuck on ncaPhysics.
        ci.SetUnset(facts::PLAYER_CI_REQ_NCAPHYSICS_OFF);
        ci.SetUnset(facts::PLAYER_CI_REQ_R15TOUCHINTERACT_OFF);
        CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == PawnProvision::PhysicsCiReq::NcaPhysics);
        CHECK_FALSE(h.physicsCi(h.ctx, kHopIndex));

        struct Step { uint32_t off; PawnProvision::PhysicsCiReq req; };
        const Step order[] = {
            {facts::PLAYER_CI_REQ_NCAPHYSICS_OFF,    PawnProvision::PhysicsCiReq::NcaPhysics},
            {facts::PLAYER_CI_REQ_NCATRANSFORM_OFF,  PawnProvision::PhysicsCiReq::NcaTransform},
            {facts::PLAYER_CI_REQ_NCAMODEL_OFF,      PawnProvision::PhysicsCiReq::NcaModel},
            {facts::PLAYER_CI_REQ_R15BODY_OFF,       PawnProvision::PhysicsCiReq::R15Body},
            {facts::PLAYER_CI_REQ_R15NETPHYSICS_OFF, PawnProvision::PhysicsCiReq::R15NetPhysics},
            {facts::PLAYER_CI_REQ_R15TOUCHINTERACT_OFF,
             PawnProvision::PhysicsCiReq::R15TouchInteract},
        };
        for (const Step& s : order) {
            for (const Step& t : order) {
                if (t.off == s.off) ci.SetUnset(t.off); else ci.Word(t.off) = 1;
            }
            CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == s.req);
        }
    }

    SUBCASE("every requirement met and no witness is NOT built") {
        // The zeroed element the pool hands out meets every word compare and has had nothing done
        // to it: without the witness this reads as `built`, which is the defect the second half of
        // the record exists to prevent.
        CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == PawnProvision::PhysicsCiReq::NotAttempted);
        CHECK_FALSE(h.physicsCi(h.ctx, kHopIndex));

        // A witness whose LOW FOUR BYTES ARE ZERO.  An 8-byte read says non-null and the init
        // completed; a 4-byte read says null and answers NotAttempted about an element that
        // demonstrably was initialised.  Nothing but the width distinguishes the two answers.
        ci.Witness() = 0x00007FF600000000ull;
        CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == PawnProvision::PhysicsCiReq::None);
        CHECK(h.physicsCi(h.ctx, kHopIndex));
    }

    SUBCASE("a requirement outstanding beats a witness that is already written") {
        ci.Witness() = 0x00007FF600000000ull;
        ci.SetUnset(facts::PLAYER_CI_REQ_R15NETPHYSICS_OFF);
        CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == PawnProvision::PhysicsCiReq::R15NetPhysics);
        CHECK_FALSE(h.physicsCi(h.ctx, kHopIndex));
    }

    SUBCASE("an index past the entrant table refuses by name and reads no element") {
        ci.Witness() = 0x00007FF600000000ull;
        CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == PawnProvision::PhysicsCiReq::None);
        const uint16_t past = kHopIndex + 1;
        CHECK(h.physicsCiAwaiting(h.ctx, past) == PawnProvision::PhysicsCiReq::IndexOutOfRange);
        CHECK(PawnProvision::PhysicsCiRefusal(PawnProvision::PhysicsCiReq::IndexOutOfRange) ==
              PawnProvision::Refusal::PhysicsCiIndexOutOfRange);
        CHECK_FALSE(h.physicsCi(h.ctx, past));
    }

    SUBCASE("an unreadable component system refuses by name and never answers a wait") {
        *reinterpret_cast<void**>(ci.cs + facts::PLAYER_CS_POOL_BASE_OFF) = nullptr;
        CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == PawnProvision::PhysicsCiReq::Unreadable);
        CHECK(PawnProvision::PhysicsCiRefusal(PawnProvision::PhysicsCiReq::Unreadable) ==
              PawnProvision::Refusal::PhysicsCiUnreadable);
        CHECK_FALSE(h.physicsCi(h.ctx, kHopIndex));
        // An unreadable CI is NOT the wait's refusal, so a census cannot read it as the engine
        // making progress.
        CHECK(PawnProvision::PhysicsCiRefusal(PawnProvision::PhysicsCiReq::NcaPhysics) ==
              PawnProvision::Refusal::NoPhysicsCi);
    }

    SUBCASE("no element for this entrant is not-attempted, not unreadable") {
        ci.Word(facts::PLAYER_CI_USERID_OFF) = kHopIndex + 7;
        CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == PawnProvision::PhysicsCiReq::NotAttempted);
    }

    SUBCASE("a build with no component system to offer answers unknown, never a pass") {
        f.surface.remotePlayerCs = nullptr;
        CHECK(h.physicsCiAwaiting(h.ctx, kHopIndex) == PawnProvision::PhysicsCiReq::Unknown);
        CHECK_FALSE(h.physicsCi(h.ctx, kHopIndex));
    }

    g_ci = nullptr;
}

TEST_CASE("The shipped identity hands out this same surface, and hop 4 is bound") {
    HopFixture f;
    const PawnProvision::ActorHops& h = f.surface.hops;
    CHECK(h.ctx == &f.surface);
    CHECK(h.inRemotePlayerCs != nullptr);
    CHECK(h.enabled != nullptr);
    CHECK(h.placed != nullptr);        // S14.2 gave hop 3 the readable postcondition it lacked
    CHECK(h.isLocalUser != nullptr);
    CHECK(h.writeSpawned != nullptr);
    CHECK(h.assignTeam != nullptr);
    CHECK(h.teamOf != nullptr);
    // Hop 4 is a WAIT that is now READ: both hooks are bound, and the fixture's own component
    // system is what they read.  A build that left either null would report `unknown` forever.
    CHECK(h.physicsCi != nullptr);
    CHECK(h.physicsCiAwaiting != nullptr);
    CHECK(PawnProvision::ParticipationFeatureState(h).armed);
}

// the SHIPPED team read, over a real-layout entrant table.
//
// ENGINE.SPAWN_POINT_AUTHOR_VA is four lines and it is the whole specification of this fixture:
//     v5 = *(WORD *)(592 * entrantIndex + netGame + 376);
//     v9 = (v5 >> 10) & 7;      // team
//     v8 = (v5 >>  5) & 0x1F;   // team slot
// So the table below is one word per entrant at USER_ARRAY_OFF, stepped by USER_STRIDE, and
// nothing else.  Two entrants are planted with four values that are pairwise distinct: a read of
// the wrong field, of the wrong entrant, or of an unmasked word is a different number in each case.
namespace {

constexpr uint16_t kTeamIdxA = 1;
constexpr uint16_t kTeamIdxB = 4;
constexpr uint16_t kTeamCount = 5;      // kTeamIdxB is the last entrant; kTeamCount is one past it

constexpr uint8_t kTeamA = 3, kSlotA = 17;
constexpr uint8_t kTeamB = 5, kSlotB = 9;

static_assert(facts::IDENT_USER_TEAM_SHIFT == 10 && facts::IDENT_USER_TEAM_MASK == 7 &&
              facts::IDENT_USER_TEAMSLOT_SHIFT == 5 && facts::IDENT_USER_TEAMSLOT_MASK == 0x1F,
              "(v5 >> 10) & 7 is the team, (v5 >> 5) & 0x1F is the team slot");
static_assert(kTeamA != kSlotA && kTeamB != kSlotB && kTeamA != kTeamB && kSlotA != kSlotB &&
              kTeamA != kSlotB && kTeamB != kSlotA,
              "no value is reused across two fields, or a wrong-field read cannot be seen");
static_assert(kSlotA > facts::IDENT_USER_TEAM_MASK && kSlotB != 0,
              "a team slot returned as a team must not be a value a team could hold");

// Every bit outside the two fields is left SET, so a read that drops a mask reports a saturated
// value rather than the planted one.
uint16_t UserTeamWord(uint8_t team, uint8_t slot) {
    uint16_t w = uint16_t(~uint16_t(0));
    w &= uint16_t(~(uint16_t(facts::IDENT_USER_TEAM_MASK) << facts::IDENT_USER_TEAM_SHIFT));
    w &= uint16_t(~(uint16_t(facts::IDENT_USER_TEAMSLOT_MASK) << facts::IDENT_USER_TEAMSLOT_SHIFT));
    w |= uint16_t(uint16_t(team) << facts::IDENT_USER_TEAM_SHIFT);
    w |= uint16_t(uint16_t(slot) << facts::IDENT_USER_TEAMSLOT_SHIFT);
    return w;
}

struct FakeEntrantTable {
    uint8_t ng[size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * kTeamCount];

    FakeEntrantTable() {
        memset(ng, 0xAB, sizeof ng);   // an entrant nobody planted must decode to neither pair
        const uint16_t count = kTeamCount;
        memcpy(ng + facts::IDENT_ENTRANT_COUNT_OFF, &count, sizeof count);
        Plant(kTeamIdxA, kTeamA, kSlotA);
        Plant(kTeamIdxB, kTeamB, kSlotB);
    }

    void Plant(uint16_t index, uint8_t team, uint8_t slot) {
        const uint16_t w = UserTeamWord(team, slot);
        memcpy(ng + size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * index,
               &w, sizeof w);
    }
};

FakeEntrantTable* g_teamNg = nullptr;
void* TeamNetGame() { return g_teamNg ? g_teamNg->ng : nullptr; }

}  // namespace

TEST_CASE("W2-C: the shipped identity decodes each entrant's team off the engine's own user word") {
    static FakeEntrantTable t;    // ~3 KB: static so it is not a stack frame this size
    t = FakeEntrantTable{};
    g_teamNg = &t;
    IIdentity* id = identity_access::Create(&TeamNetGame, nullptr);
    REQUIRE(id != nullptr);

    SUBCASE("both entrants decode, and they decode to their own word") {
        uint8_t teamA = 0xAB, slotA = 0xCD;
        REQUIRE(id->TeamOf(UserIndex(kTeamIdxA), teamA, slotA));
        CHECK(teamA == kTeamA);
        CHECK(slotA == kSlotA);
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::None);

        uint8_t teamB = 0xAB, slotB = 0xCD;
        REQUIRE(id->TeamOf(UserIndex(kTeamIdxB), teamB, slotB));
        CHECK(teamB == kTeamB);
        CHECK(slotB == kSlotB);

        // Two entrants, two answers: a read that ignored the stride would report one pair twice.
        CHECK(teamA != teamB);
        CHECK(slotA != slotB);
    }

    SUBCASE("an entrant whose word was never planted answers neither planted pair") {
        uint8_t team = 0xAB, slot = 0xCD;
        REQUIRE(id->TeamOf(UserIndex(0), team, slot));
        CHECK(team != kTeamA);
        CHECK(team != kTeamB);
        CHECK(slot != kSlotA);
        CHECK(slot != kSlotB);
    }

    SUBCASE("an index past the entrant count refuses BY NAME and leaves the out params alone") {
        uint8_t team = 0xAB, slot = 0xCD;
        CHECK_FALSE(id->TeamOf(UserIndex(kTeamCount), team, slot));
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::NoSuchEntrant);
        // A confident 0 here authors every place for team 0 -- the out params must be untouched.
        CHECK(team == 0xAB);
        CHECK(slot == 0xCD);
    }

    SUBCASE("no netgame refuses NoNetGame, not NoSuchEntrant") {
        g_teamNg = nullptr;
        uint8_t team = 0xAB, slot = 0xCD;
        CHECK_FALSE(id->TeamOf(UserIndex(kTeamIdxA), team, slot));
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::NoNetGame);
        CHECK(team == 0xAB);
        CHECK(slot == 0xCD);
    }

    identity_access::Destroy(id);
    g_teamNg = nullptr;
}

TEST_CASE("W2-C: hop 3's precondition is bound in the SHIPPED surface and reads the same word") {
    static FakeEntrantTable t;
    t = FakeEntrantTable{};
    g_teamNg = &t;

    identity_access::ActorHopSurface surface;
    surface.netGame = TeamNetGame;
    surface.calls   = nullptr;          // teamOf drives no engine call at all
    identity_access::BindActorHops(surface);
    const PawnProvision::ActorHops& h = surface.hops;
    REQUIRE(h.teamOf != nullptr);
    REQUIRE(h.ctx == &surface);

    SUBCASE("both entrants decode through the hop the provisioner would call") {
        uint8_t teamA = 0xAB, slotA = 0xCD;
        REQUIRE(h.teamOf(h.ctx, kTeamIdxA, &teamA, &slotA));
        CHECK(teamA == kTeamA);
        CHECK(slotA == kSlotA);

        uint8_t teamB = 0xAB, slotB = 0xCD;
        REQUIRE(h.teamOf(h.ctx, kTeamIdxB, &teamB, &slotB));
        CHECK(teamB == kTeamB);
        CHECK(slotB == kSlotB);

        // The two doors onto the same word must agree: the shipped identity's TeamOf and the hop
        // the pawn provisioner calls cannot author a place for different teams.
        IIdentity* id = identity_access::Create(&TeamNetGame, nullptr);
        REQUIRE(id != nullptr);
        uint8_t idTeam = 0xAB, idSlot = 0xCD;
        REQUIRE(id->TeamOf(UserIndex(kTeamIdxA), idTeam, idSlot));
        CHECK(idTeam == teamA);
        CHECK(idSlot == slotA);
        identity_access::Destroy(id);
    }

    SUBCASE("an index past the entrant count refuses and writes nothing") {
        uint8_t team = 0xAB, slot = 0xCD;
        CHECK_FALSE(h.teamOf(h.ctx, kTeamCount, &team, &slot));
        CHECK(team == 0xAB);
        CHECK(slot == 0xCD);
    }

    SUBCASE("a netgame that answers null refuses, and so does a missing out param") {
        uint8_t team = 0xAB, slot = 0xCD;
        g_teamNg = nullptr;
        CHECK_FALSE(h.teamOf(h.ctx, kTeamIdxA, &team, &slot));
        g_teamNg = &t;
        CHECK_FALSE(h.teamOf(h.ctx, kTeamIdxA, nullptr, &slot));
        CHECK_FALSE(h.teamOf(h.ctx, kTeamIdxA, &team, nullptr));
        CHECK_FALSE(h.teamOf(nullptr, kTeamIdxA, &team, &slot));
        CHECK(team == 0xAB);
        CHECK(slot == 0xCD);
    }

    g_teamNg = nullptr;
}

// ---------------------------------------------------------------------------------------------
// ARCH-23 S14.7 test 8 -- `set_team_cannot_disarm_the_driver`.
//
// The retired raw write cost was silent and TOTAL: a caller that reached the recorded team by
// poking the packed role word left the word already reading it, so A3's own early-out (live role
// == recorded role -> no call) skipped IDENT.SERVER_ASSIGN_TEAM, and the entrant kept a
// participating role with a disabled pawn.  Every hop reads plausible in that state, which is why
// this case asserts the engine CALL and the untouched word rather than the final value: the value
// is exactly what the two implementations agree on.
namespace {

struct SetTeamProbe {
    unsigned assigns = 0;
    uint16_t sawIndex = replay::kSlotUnmapped;
    uint32_t sawRole  = UINT32_MAX;
    // false models the session type that refuses SERVER_ASSIGN_TEAM SILENTLY (it discriminates on
    // the session object's vtable+32 and answers nothing); the word then never changes.
    bool     engineApplies = true;
    // The nested call S14.6 warns about, made from INSIDE the engine call: the whole participating
    // arm runs synchronously in there, so a callback of ours that reached back into SetTeam would
    // start a second transition inside the first if nothing held a latch.
    IIdentity* reenter = nullptr;
    unsigned   nestedCalls  = 0;
    bool       nestedAnswer = true;
    identity_access::Refusal nestedWhy = identity_access::Refusal::None;
};
SetTeamProbe* g_setTeam = nullptr;

// IDENT.SERVER_ASSIGN_TEAM's own effect on the word, and the ONLY writer of it in this case.
void ProbeAssignTeam(void* ng, uint16_t index, uint32_t role) {
    ++g_setTeam->assigns;
    g_setTeam->sawIndex = index;
    g_setTeam->sawRole  = role;
    if (g_setTeam->reenter != nullptr) {
        IIdentity* re = g_setTeam->reenter;
        g_setTeam->reenter = nullptr;   // one level only, whatever the latch answers
        ++g_setTeam->nestedCalls;
        g_setTeam->nestedAnswer = re->SetTeam(UserIndex(index), uint8_t(role), 0);
        g_setTeam->nestedWhy = identity_access::LastRefusal(re);
    }
    if (!g_setTeam->engineApplies) return;
    uint8_t* p = static_cast<uint8_t*>(ng) + size_t(facts::USER_ARRAY_OFF) +
                 size_t(facts::USER_STRIDE) * index;
    uint16_t w = 0;
    memcpy(&w, p, sizeof w);
    w = uint16_t((w & ~(uint16_t(facts::IDENT_USER_TEAM_MASK) << facts::IDENT_USER_TEAM_SHIFT)) |
                 (uint16_t(role & facts::IDENT_USER_TEAM_MASK) << facts::IDENT_USER_TEAM_SHIFT));
    memcpy(p, &w, sizeof w);
}

uint16_t RawUserWord(const FakeEntrantTable& t, uint16_t index) {
    uint16_t w = 0;
    memcpy(&w, t.ng + size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * index, sizeof w);
    return w;
}

constexpr uint8_t kAssignedTeam = 1;   // a role the engine takes, and not the planted one
constexpr uint8_t kForcedSlot   = 4;   // a slot no caller may force onto the word
static_assert(kAssignedTeam != kTeamA && kAssignedTeam < facts::NETGAME_USER_ROLE_BOUND,
              "the requested role must be a change the engine would accept");
static_assert(kForcedSlot != kSlotA && kForcedSlot <= facts::IDENT_USER_TEAMSLOT_MASK,
              "a forced slot must be a value the word could hold, or writing it is unobservable");

// MarkHop writes nothing until the crash logger owns a file, so the marker assertions need one.
struct MarkerLog {
    wchar_t path[MAX_PATH];

    explicit MarkerLog(const wchar_t* leaf) {
        wchar_t dir[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, dir);
        REQUIRE(n != 0);
        REQUIRE(n < MAX_PATH);
        swprintf(path, MAX_PATH, L"%s%s", dir, leaf);
        DeleteFileW(path);
    }
    ~MarkerLog() { PawnProvision::RemoveCrashLogger(); DeleteFileW(path); }
};

}  // namespace

TEST_CASE("W2-P1-29: SetTeam cannot disarm the driver -- the team travels the engine's own call") {
    static FakeEntrantTable t;
    t = FakeEntrantTable{};
    g_teamNg = &t;
    SetTeamProbe probe;
    g_setTeam = &probe;

    identity_access::HopCallTable calls;
    calls.assignTeam = ProbeAssignTeam;

    IIdentity* id = identity_access::Create(&TeamNetGame, nullptr);
    REQUIRE(id != nullptr);
    // The surface under test is the SHIPPED one -- the identity's own, the same storage its
    // SetTeam hands to HopAssignTeam.  A private copy would prove nothing about the shipped path.
    const PawnProvision::ActorHops* hops = id->ActorHops();
    REQUIRE(hops != nullptr);
    REQUIRE(hops->ctx != nullptr);
    // The ctx every hop is driven with must be the identity's OWN storage.  `Create` assigns from
    // a temporary, and a ctx copied out of that temporary points into a frame that died with the
    // statement: the provisioner would then read its netgame and its call table out of reused
    // stack.  The surface is a member, so its distance from the instance is one object's worth.
    const uintptr_t idAddr  = reinterpret_cast<uintptr_t>(id);
    const uintptr_t ctxAddr = reinterpret_cast<uintptr_t>(hops->ctx);
    CHECK(ctxAddr >= idAddr);
    CHECK(ctxAddr - idAddr < 4096u);

    identity_access::ActorHopSurface* surface =
        static_cast<identity_access::ActorHopSurface*>(hops->ctx);
    surface->calls = &calls;

    const uint16_t plantedWord = RawUserWord(t, kTeamIdxA);

    SUBCASE("the role is applied by IDENT.SERVER_ASSIGN_TEAM, and the slot is left to the engine") {
        const bool applied = id->SetTeam(UserIndex(kTeamIdxA), kAssignedTeam, kForcedSlot);
        CHECK(probe.assigns == 1u);
        CHECK(applied);
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::None);
        CHECK(probe.sawIndex == kTeamIdxA);
        CHECK(probe.sawRole == uint32_t(kAssignedTeam));

        uint8_t team = 0xAB, slot = 0xCD;
        REQUIRE(id->TeamOf(UserIndex(kTeamIdxA), team, slot));
        CHECK(team == kAssignedTeam);
        // NETGAME.USER_SETTINGS_APPLY maintains the per-team counts and the slot within them, so
        // the caller's slot is reported unapplied rather than forced into the word behind it.
        CHECK(slot == kSlotA);
        CHECK(slot != kForcedSlot);
    }

    SUBCASE("a session that refuses the call silently leaves the word ours to never have written") {
        probe.engineApplies = false;
        CHECK_FALSE(id->SetTeam(UserIndex(kTeamIdxA), kAssignedTeam, kForcedSlot));
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::TeamNotApplied);
        CHECK(probe.assigns == 1u);
        // Not one bit of the word is ours to write: the refusal is reported, the state is not
        // faked, and A3's early-out therefore still sees a mismatch and still issues its call.
        CHECK(RawUserWord(t, kTeamIdxA) == plantedWord);
        uint8_t team = 0xAB, slot = 0xCD;
        REQUIRE(id->TeamOf(UserIndex(kTeamIdxA), team, slot));
        CHECK(team == kTeamA);
        CHECK(team != kAssignedTeam);
        CHECK(slot == kSlotA);
    }

    SUBCASE("a role the engine will not take is refused by name, and no call is issued") {
        CHECK_FALSE(id->SetTeam(UserIndex(kTeamIdxA),
                                uint8_t(facts::NETGAME_USER_ROLE_BOUND), kForcedSlot));
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::TeamNotApplied);
        CHECK(probe.assigns == 0u);
        CHECK(RawUserWord(t, kTeamIdxA) == plantedWord);
    }

    SUBCASE("an unbound assign is a refusal, never a write") {
        identity_access::HopCallTable unbound;
        surface->calls = &unbound;
        CHECK_FALSE(id->SetTeam(UserIndex(kTeamIdxA), kAssignedTeam, kForcedSlot));
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::TeamNotApplied);
        CHECK(probe.assigns == 0u);
        CHECK(RawUserWord(t, kTeamIdxA) == plantedWord);
    }

    SUBCASE("an index past the entrant count refuses as one, and reaches no engine call") {
        CHECK_FALSE(id->SetTeam(UserIndex(kTeamCount), kAssignedTeam, kForcedSlot));
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::NoSuchEntrant);
        CHECK(probe.assigns == 0u);
    }

    // "The recording has no team" and "the engine will not take this team" are
    // different answers, and the dresser keeps dressing on the first: naming both TeamNotApplied
    // is what cost a no-team player its whole loadout.
    SUBCASE("a team the recording never captured is refused by ITS OWN name, and issues no call") {
        CHECK_FALSE(id->SetTeam(UserIndex(kTeamIdxA), uint8_t(kPlayerNoTeam16), kForcedSlot));
        CHECK(identity_access::LastRefusal(id) == identity_access::Refusal::TeamNotRecorded);
        CHECK(identity_access::LastRefusal(id) != identity_access::Refusal::TeamNotApplied);
        CHECK(probe.assigns == 0u);
        CHECK(RawUserWord(t, kTeamIdxA) == plantedWord);
    }

    // This is the second SERVER_ASSIGN_TEAM site in the tree and it runs on the
    // same stage-0 tick as the hop chain.  Without its own marker a fault entered from the dress
    // path wears whatever marker the previous Pump left -- `hop 3 placed:` for some other slot --
    // and the crash block's most valuable reading points at the wrong call.
    SUBCASE("the engine call is marked before it is made, with this call site's own name") {
        MarkerLog marks(L"replay_identity_setteam_marker.log");
        REQUIRE(PawnProvision::InstallCrashLogger(marks.path));
        CHECK(id->SetTeam(UserIndex(kTeamIdxA), kAssignedTeam, kForcedSlot));
        CHECK(probe.assigns == 1u);
        CHECK(strstr(PawnProvision::LastHopMarker(), "hop 0 dress assignTeam:") ==
              PawnProvision::LastHopMarker());
    }

    SUBCASE("a call re-entered from inside the engine call is refused, and the latch reopens") {
        probe.reenter = id;
        CHECK(id->SetTeam(UserIndex(kTeamIdxA), kAssignedTeam, kForcedSlot));
        CHECK(probe.nestedCalls == 1u);
        CHECK_FALSE(probe.nestedAnswer);
        CHECK(probe.nestedWhy == identity_access::Refusal::EngineCallReentrant);
        // The nested call authored NOTHING: one assign for the outer call and none for it.
        CHECK(probe.assigns == 1u);
        // ...and the latch was released rather than left held, so the next call still works.
        CHECK(id->SetTeam(UserIndex(kTeamIdxA), kTeamA, kForcedSlot));
        CHECK(probe.assigns == 2u);
    }

    surface->calls = nullptr;
    identity_access::Destroy(id);
    g_setTeam = nullptr;
    g_teamNg = nullptr;
}

namespace {

// Every method answers success so the team step is the only thing that can decide the outcome,
// and SetTeam is COUNTED: "no call was issued" is the assertion, not "the result looks right".
class TeamStepFake final : public IIdentity {
public:
    unsigned setTeamCalls = 0;
    uint8_t  sawTeam = 0xEE;
    bool     engineTakesTeam = true;

    bool AddReplayUser(UserSlot s, UserIndex& out) override { out = UserIndex(s.v); return true; }
    bool ProvisionReplayUser(UserSlot s, const JoinDesc&, UserIndex& out) override {
        return AddReplayUser(s, out);
    }
    bool ReleaseReplayUser(UserSlot) override { return true; }
    bool SetProfileJson(UserSlot, const Blob&) override { return true; }
    bool SetTeam(UserIndex, uint8_t team, uint8_t) override {
        ++setTeamCalls;
        sawTeam = team;
        return engineTakesTeam;
    }
    bool ApplyLoadout(UserIndex, const Symbol64[20], uint16_t, uint16_t) override { return true; }
    bool OverrideAlignment(UserIndex, uint16_t) override { return true; }
    bool ReadAppliedItems(UserIndex, Symbol64 out[20]) const override {
        memset(out, 0, sizeof(Symbol64) * 20);
        return true;
    }
    UserSlot  SlotOf(UserIndex i) const override { return UserSlot(i.v); }
    UserIndex IndexOf(UserSlot s) const override { return UserIndex(s.v); }
};

}  // namespace

// `PlayerRecord::teamId` is 8-bit and B.0 could express "absent" only as 0xFF,
// the low byte of kPlayerNoTeam16; no-team rows are real, and the roster fills them.  Before this,
// step 3 handed that sentinel to SetTeam, SetTeam refused it (0xFF >= USER_ROLE_BOUND) and the
// dress RETURNED -- so a player whose team was never captured lost its name-full, its twenty
// loadout ids, both numbers, its colour and its chassis.  Against "respect EVERYTHING ... names,
// profiles, loadouts, skins, colors", the team is the one thing that step may cost.
TEST_CASE("An unrecorded team costs the team step and nothing after it") {
    TeamStepFake id;
    PlayerRecord rec{};
    rec.userSlot = 1;
    identity::DressResult out{};

    SUBCASE("the absent sentinel skips the engine call, names the step, and lets the dress go on") {
        rec.teamId   = uint8_t(kPlayerNoTeam16);
        rec.teamId16 = kPlayerNoTeam16;
        CHECK(identity::ApplyTeamStep(id, rec, UserIndex(3), out));
        CHECK(id.setTeamCalls == 0u);
        CHECK(!out.wrote.team);
        CHECK(out.teamRefusal == identity_access::Refusal::TeamNotRecorded);
        // The DRESS did not fail: nothing named a stage and nothing was refused.
        CHECK(out.refusal == identity_access::Refusal::None);
        CHECK(out.stage == nullptr);
    }

    SUBCASE("the 8-bit field alone says it, because a B.0 record carries no wider one") {
        rec.teamId = uint8_t(kPlayerNoTeam16);   // teamId16 left at its zero-init, which IS team 0
        CHECK(identity::ApplyTeamStep(id, rec, UserIndex(3), out));
        CHECK(id.setTeamCalls == 0u);
        CHECK(out.teamRefusal == identity_access::Refusal::TeamNotRecorded);
    }

    SUBCASE("a RECORDED team still travels the engine's call, and its refusal still stops the dress") {
        rec.teamId   = 1;
        rec.teamId16 = 1;
        CHECK(identity::ApplyTeamStep(id, rec, UserIndex(3), out));
        CHECK(id.setTeamCalls == 1u);
        CHECK(id.sawTeam == 1);
        CHECK(out.wrote.team);
        CHECK(out.teamRefusal == identity_access::Refusal::None);

        identity::DressResult second{};
        id.engineTakesTeam = false;
        CHECK_FALSE(identity::ApplyTeamStep(id, rec, UserIndex(3), second));
        CHECK(id.setTeamCalls == 2u);
        CHECK(!second.wrote.team);
    }
}

// ---------------------------------------------------------------------------------------------
// the entrant-table geometry above is the thing under test, and the fixture plants through
// the same `facts::` constants the shipped reader reads -- so an offset mutant on USER_ARRAY_OFF
// or USER_STRIDE moves the plant and the read TOGETHER and no case above can see it.  (The
// static_assert that would have said so is a transcription the naming ratchet is right to refuse.)
// This is the outside record that makes those six values fail-able: the merged row in
// facts/facts.tsv, whose anchor tests/factcheck re-decodes out of the pinned echovr.exe.  Edit
// either side -- the header the build compiles, or the filed row -- and this reds.
//
// The value column is parsed with strtoul base 0 because the registry files both forms: one of the
// rows below is filed in decimal and the rest in hex, and a needle that assumed a hex prefix would
// skip that row and then report all-found on one comparison fewer.
TEST_CASE("W2-C: the entrant-table offsets are the merged registry rows, not local opinion") {
    const std::string path = std::string(REPLAY_REPO_ROOT) + "/facts/facts.tsv";
    FILE* f = fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::string blob;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) blob.append(buf, n);
    fclose(f);
    REQUIRE(blob.size() > 0u);

    struct Row { const char* id; uint32_t value; };
    const Row rows[] = {
        {"USER.ARRAY_OFF",             facts::USER_ARRAY_OFF},
        {"USER.STRIDE",                facts::USER_STRIDE},
        {"IDENT.USER_TEAM_SHIFT",      facts::IDENT_USER_TEAM_SHIFT},
        {"IDENT.USER_TEAM_MASK",       facts::IDENT_USER_TEAM_MASK},
        {"IDENT.USER_TEAMSLOT_SHIFT",  facts::IDENT_USER_TEAMSLOT_SHIFT},
        {"IDENT.USER_TEAMSLOT_MASK",   facts::IDENT_USER_TEAMSLOT_MASK},
        // The count the fixture plants, which is what makes an index past the roster a refusal
        // rather than a read of the 0xAB fill.
        {"IDENT.ENTRANT_COUNT_OFF",    facts::IDENT_ENTRANT_COUNT_OFF},
    };
    size_t found = 0;
    for (const Row& r : rows) {
        std::string needle(1, '\n');
        needle += r.id;
        needle += '\t';
        const size_t at = blob.find(needle);
        // An absent row would leave the comparison below with nothing to disagree with.
        CHECK_MESSAGE(at != std::string::npos, r.id);
        if (at == std::string::npos) continue;
        const size_t valueAt = at + needle.size();
        const size_t end = blob.find('\t', valueAt);
        REQUIRE(end != std::string::npos);
        const std::string field = blob.substr(valueAt, end - valueAt);
        char* stop = nullptr;
        const unsigned long parsed = strtoul(field.c_str(), &stop, 0);
        // A row whose value column were text would parse as 0 and agree with nothing here, but
        // say so rather than reporting it as a mismatch of the constant.
        CHECK_MESSAGE(stop == field.c_str() + field.size(), r.id);
        ++found;
        CHECK_MESSAGE(uint32_t(parsed) == r.value, r.id);
    }
    CHECK(found == sizeof rows / sizeof rows[0]);
}

// ---------------------------------------------------------------------------------------------
// the slot key is half the item.
//
// The engine's customisation entry carries its own key at +0x00 and the item at +0x10.  The
// item-only reader dropped the key, which left POSITION as the only slot evidence a recording
// could carry -- and that the component's order equals the engine's slot-name order is NOT
// established.  A replay dressed off a guessed order wears the wrong things and looks fine, so the
// key is recorded rather than assumed.  PairsFromSlots is the walk with no engine resolution in it
// (BuildMatches() is false off a real image), which is what makes the extraction falsifiable here.
// ---------------------------------------------------------------------------------------------

TEST_CASE("pairs_reader_returns_the_key_the_engine_holds") {
    using replay::identity_access::CustSlotEntry;
    using replay::identity_access::LoadoutPair;
    using replay::identity_access::Refusal;

    // key != position, key != item, and the keys are not in ascending order either: nothing about
    // the fixture lets a positional reader look right.
    CustSlotEntry slots[20] = {};
    for (uint64_t i = 0; i < 20; ++i) {
        slots[i].key = 0x9E3779B97F4A7C15ull ^ (0x100ull * (19 - i));
        slots[i].item = 0x4242000000000000ull + i;
    }

    LoadoutPair out[20] = {};
    CHECK(replay::identity_access::PairsFromSlots(slots, 20, out) == 20);
    for (uint64_t i = 0; i < 20; ++i) {
        CHECK(out[i].slotKey == slots[i].key);
        CHECK(out[i].itemId == slots[i].item);
        CHECK(out[i].slotKey != i);               // position is not the key
        CHECK(out[i].slotKey != out[i].itemId);   // and the key is not a copy of the item
    }

    // A component shorter than the format leaves nothing stale past its end: a key read out of a
    // previous entrant's row would dress this one from someone else's loadout.
    LoadoutPair few[20];
    memset(few, 0xAB, sizeof few);
    CHECK(replay::identity_access::PairsFromSlots(slots, 3, few) == 3);
    for (uint64_t i = 3; i < 20; ++i) {
        CHECK(few[i].slotKey == 0);
        CHECK(few[i].itemId == 0);
    }

    // A count above the format's bound is clamped, never walked.
    CHECK(replay::identity_access::PairsFromSlots(slots, 20000, out) == 20);
    CHECK(replay::identity_access::PairsFromSlots(nullptr, 20, out) == 0);

    // The engine-facing reader answers by name rather than handing back the zeroed buffer as if it
    // were an empty loadout, and it zeroes `out` before it refuses.
    uint64_t n = 0xDEADull;
    Refusal why = Refusal::None;
    memset(out, 0x5A, sizeof out);
    CHECK(!replay::identity_access::ReadAppliedPairs(nullptr, UserIndex(0), out, n, nullptr,
                                                        &why));
    CHECK(why == Refusal::NoNetGame);
    CHECK(n == 0xDEADull);          // untouched: "no pairs" and "could not ask" stay different
    CHECK(out[0].slotKey == 0);
}

// ---------------------------------------------------------------------------------------------
// ARCH-23 S14.8's live bug: the publish took its SOURCE from the entrant INDEX while taking its
// destination from the SLOT.  Both engine sites index the source by slot
// (IDENT.PROFILE_RECEIVE_VA:109 derives 19720 * (*(WORD*)(user+376) & IDENT.USER_SLOT_MASK) once
// and uses it for the parse destination, the publish source and the published flag alike).  At one
// entrant slot equals index and nothing showed; the moment they differ the publish copies a
// different 19,720-byte block into the live table and the entrant wears another user's loadout.
//
// The fixture is a plain buffer, so this asserts the ADDRESS ARITHMETIC and needs no engine.
// ---------------------------------------------------------------------------------------------

TEST_CASE("l4_indexes_the_source_by_slot") {
    const size_t stride = size_t(facts::USER_PROFILE_STRIDE);
    const size_t need = size_t(facts::USER_PROFILE_OFF) +
                        stride * size_t(facts::ENTITY_USER_SLOT_TABLE_MAX) +
                        size_t(facts::USER_LOADOUT_OFF) + 64;
    std::string ng(need, char(0));
    void* base = &ng[0];

    for (uint16_t slot = 0; slot < facts::ENTITY_USER_SLOT_TABLE_MAX; ++slot) {
        const uint8_t* want = reinterpret_cast<const uint8_t*>(base) +
                              size_t(facts::USER_PROFILE_OFF) + stride * size_t(slot) +
                              size_t(facts::USER_LOADOUT_OFF);
        CHECK(replay::identity_access::LoadoutContainer(base, UserSlot(slot)) ==
              static_cast<const void*>(want));
    }

    // The property the defect broke: two slots are two blocks.  A source addressed by anything
    // other than the slot lands on one of them for both.
    CHECK(replay::identity_access::LoadoutContainer(base, UserSlot(3)) !=
          replay::identity_access::LoadoutContainer(base, UserSlot(0)));

    CHECK(replay::identity_access::LoadoutContainer(nullptr, UserSlot(0)) == nullptr);
    CHECK(replay::identity_access::LoadoutContainer(
              base, UserSlot(facts::ENTITY_USER_SLOT_TABLE_MAX)) == nullptr);
}

// =============================================================================================
// W2-P1-30c: L1..L6, ARCH-23 S14.8 / S14.8b / S14.8c.
//
// L1 is a VERIFICATION.  Both halves of the engine's pair are one-way hashes of strings taken
// out of the profile document, so nothing can be authored from a recorded itemId -- the item
// names are already in the recorded document, and the whole of L1 is proving that the two sides
// agree before any engine call is made against them.
//
// The stand-in hasher below is a real fold, not a table lookup, so the code under test cannot
// pass by hashing the wrong string: every fixture derives the RECORDED side from the document's
// own values through it, and the skew fixture plants exactly one disagreement.
// =============================================================================================
namespace loadoutfix {

using replay::identity_access::LoadoutEngine;
using replay::identity_access::LoadoutOutcome;
using replay::identity_access::LoadoutPair;
using replay::identity_access::LoadoutRefusal;
using replay::identity_access::LoadoutPhase;
using replay::identity_access::LoadoutStep;
using replay::identity_access::LoadoutSteps;
using replay::identity_access::RecordedLoadout;

uint64_t FoldHash(const char* s) {
    if (s == nullptr || *s == 0) return ~uint64_t(0);   // the engine returns its seed here
    uint64_t h = 0xcbf29ce484222325ull;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        h ^= *p;
        h *= 0x100000001b3ull;
    }
    return h ? h : 1;
}

struct Node {
    const char* name;
    uint32_t kind;
    const char* value;
};

struct Doc {
    Node nodes[24];
    size_t count;
    // Every call the code under test made into the document or the hasher, so a step that was
    // skipped is a count that did not move.
    mutable int writes;
    mutable int hashCalls;
    mutable int lastA3;
    mutable std::string lastHashed;
};

Doc* g_doc = nullptr;

void* PathElem(void* json, const char* path, void* prev) {
    Doc* d = static_cast<Doc*>(json);
    // The parent the twenty leaves hang off, taken from the engine's own template so a code
    // change that walks a different path finds nothing here.
    static std::string parent;
    if (parent.empty()) {
        parent = facts::IDENT_LOADOUT_INSTANCE_PATH_FMT_TEXT;
        parent.erase(parent.rfind('|'));
    }
    if (path == nullptr || parent != path) return nullptr;
    size_t i = 0;
    if (prev != nullptr) i = size_t(static_cast<Node*>(prev) - d->nodes) + 1;
    return i < d->count ? &d->nodes[i] : nullptr;
}
const char* NodeName(void*, void* node) { return static_cast<Node*>(node)->name; }
uint32_t    NodeKind(void*, void* node) { return static_cast<Node*>(node)->kind; }
const char* NodeString(void* json, void* node, const char* dflt) {
    (void)json;
    Node* n = static_cast<Node*>(node);
    return n->kind == facts::IDENT_JSON_KIND_STRING ? n->value : dflt;
}
uint64_t Hash(const char* s, uint64_t seed, int32_t a3, int32_t maxChars, void* a5) {
    (void)a5;
    if (g_doc) {
        ++g_doc->hashCalls;
        g_doc->lastA3 = a3;
        g_doc->lastHashed = s ? s : "";
    }
    // The engine returns the SEED for a null or empty string and takes a max char count; a
    // stand-in that ignored either could not fail the tests that are about them.
    CHECK(maxChars == -1);
    if (s == nullptr || *s == 0) return seed;
    return FoldHash(s);
}
void SetString(void* doc, const char*, const char*) {
    (void)doc;
    if (g_doc) ++g_doc->writes;
}

uint64_t InstanceSymbol() {
    char inst[64] = {0};
    REQUIRE(replay::identity_access::LoadoutTemplateInstance(inst, sizeof inst));
    return FoldHash(inst);
}
uint64_t WrongInstanceSymbol() { return FoldHash("not-the-instance"); }

// What the engine's own document author writes: every slot present, as a string, with a value
// that is unique per slot so a read of the wrong node cannot match by accident.
void FillDocument(Doc& d) {
    d = Doc{};
    d.count = replay::identity_access::LoadoutSlotNameCount();
    static std::string values[20];
    for (size_t i = 0; i < d.count; ++i) {
        values[i] = std::string("item_") + replay::identity_access::LoadoutSlotName(uint16_t(i));
        d.nodes[i].name = replay::identity_access::LoadoutSlotName(uint16_t(i));
        d.nodes[i].kind = facts::IDENT_JSON_KIND_STRING;
        d.nodes[i].value = values[i].c_str();
    }
}

// The RECORDED side, derived from the document the way the engine derived it: hash the slot's
// own NAME for the key and the slot's VALUE for the item.
void FillRecorded(const Doc& d, RecordedLoadout& rec, uint16_t howMany) {
    rec = RecordedLoadout{};
    rec.hasSlotKeys = true;
    rec.documentRecorded = true;
    rec.number = 3;
    rec.numberBody = 7;       // never equal to `number`: the bot path draws both the same
    rec.filledPairCount = howMany;
    for (uint16_t i = 0; i < howMany; ++i) {
        rec.pairs[i].slotKey = FoldHash(d.nodes[i].name);
        rec.pairs[i].itemId = FoldHash(d.nodes[i].value);
    }
}

entity_json::CJsonReader MakeReader(Doc& d) {
    entity_json::CJsonReader r{};
    r.json = &d;
    r.kind = &NodeKind;
    r.name = &NodeName;
    r.stringValue = &NodeString;
    r.pathElem = &PathElem;
    return r;
}

LoadoutEngine MakeEngine(entity_json::CJsonReader& r) {
    LoadoutEngine e{};
    e.doc = &r;
    e.hash = &Hash;
    e.instanceSymbol = &InstanceSymbol;
    return e;
}

// ---- the engine side, for L2..L6 --------------------------------------------------------
struct Rig {
    std::string mem;
    void* ng = nullptr;
    void* attached = nullptr;
    uint32_t flags = 0;              // the session word CosmeticsWouldApply dereferences
    LoadoutPair applied[20] = {};
    identity_access::AppliedSlotState appliedState[20] = {};
    uint64_t appliedCount = 0;
    bool actorEnabled = true;
    bool alignOk = true;
    // The record of WHICH call ran, in order: the acceptance for "no step is skipped silently"
    // and for "alignment runs after the apply".
    std::string order;
    void* parseDst = nullptr;
    const void* parseDoc = nullptr;
    const void* publishSrc = nullptr;
    void* publishDst = nullptr;
    int applyItemCalls = 0;
};

Rig* g_rig = nullptr;

void* Attach(void* ng, uint16_t, void* doc) {
    (void)ng;
    g_rig->order += "attach;";
    g_rig->attached = doc;
    return doc;
}
void* AttachedDoc(void*, uint16_t) { return g_rig->attached; }
bool  DocUsable(void*, uint16_t) { return g_rig->attached != nullptr; }

void Parse(void* container, void* doc) {
    g_rig->order += "parse;";
    g_rig->parseDst = container;
    g_rig->parseDoc = doc;
    // What the engine's parse leaves behind: the container's own header.
    replay::identity_access::LoadoutHeader h{};
    h.instanceCount = 1;
    h.number = 3;
    h.numberBody = 7;
    memcpy(container, &h, sizeof h);
}
void* Publish(void* dst, const void* src) {
    g_rig->order += "publish;";
    g_rig->publishDst = dst;
    g_rig->publishSrc = src;
    memcpy(dst, src, sizeof(replay::identity_access::LoadoutHeader));
    return dst;
}
bool ActorEnabled(void*, uint16_t) {
    g_rig->order += "enabled;";
    return g_rig->actorEnabled;
}
void ApplyItems(void*, uint16_t) {
    g_rig->order += "apply;";
    ++g_rig->applyItemCalls;
}
bool ApplyAlignment(void*, uint16_t) {
    g_rig->order += "align;";
    return g_rig->alignOk;
}
bool ReadApplied(void*, uint16_t, LoadoutPair out[20], uint64_t& n,
                 identity_access::AppliedSlotState state[20]) {
    g_rig->order += "readback;";
    for (size_t i = 0; i < 20; ++i) {
        out[i] = g_rig->applied[i];
        state[i] = g_rig->appliedState[i];
    }
    n = g_rig->appliedCount;
    return true;
}

// A CR15NetGame big enough for every offset L2..L6 touches, and no bigger claim than that.
void BuildRig(Rig& rig, uint16_t slot) {
    // A slot the engine applied carries no alignment requirement and pending == -1; every rig
    // starts there so a test that wants one of the three silent arms has to plant it.
    for (uint16_t i = 0; i < 20; ++i) {
        rig.appliedState[i] = identity_access::AppliedSlotState{};
        rig.appliedState[i].pendingItem = Symbol64(~0ull);
        rig.appliedState[i].alignmentReq = identity_access::kSlotAlignmentNone;
    }
    const size_t stride = size_t(facts::USER_PROFILE_STRIDE);
    size_t need = size_t(facts::USER_PROFILE_OFF) +
                  stride * size_t(facts::ENTITY_USER_SLOT_TABLE_MAX) +
                  size_t(facts::USER_LOADOUT_OFF) + 256;
    const size_t live = size_t(facts::IDENT_LOADOUT_TABLE_OFF) +
                        (size_t(facts::ENTITY_USER_SLOT_TABLE_MAX) << facts::IDENT_LOADOUT_ENTRY_SHIFT) + 256;
    if (live > need) need = live;
    const size_t flagEnd = stride * size_t(facts::ENTITY_USER_SLOT_TABLE_MAX) +
                           size_t(facts::IDENT_LOADOUT_PUBLISHED_FLAG_OFF) + 256;
    if (flagEnd > need) need = flagEnd;
    rig.mem.assign(need, char(0));
    rig.ng = &rig.mem[0];
    (void)slot;
    // CosmeticsWouldApply dereferences a POINTER at this offset, so the fixture plants one.
    uint32_t* flagsPtr = &rig.flags;
    memcpy(&rig.mem[size_t(facts::NETGAME_UIFLAGS_PTR_OFF)], &flagsPtr, sizeof flagsPtr);
}

void WireRig(LoadoutEngine& e) {
    e.attach = &Attach;
    e.attachedDoc = &AttachedDoc;
    e.docUsable = &DocUsable;
    e.parse = &Parse;
    e.publish = &Publish;
    e.actorEnabled = &ActorEnabled;
    e.applyItems = &ApplyItems;
    e.applyAlignment = &ApplyAlignment;
    e.readAppliedPairs = &ReadApplied;
}

// The engine's post-apply truth: what the customisation component holds.  Seeded FROM the
// recorded pairs by default -- a real apply that worked -- so a test that wants a divergence
// has to plant it and cannot get one by accident.
void SeedApplied(Rig& rig, const RecordedLoadout& rec, uint16_t n) {
    for (uint16_t i = 0; i < n; ++i) rig.applied[i] = rec.pairs[i];
    rig.appliedCount = n;
}

const char* Name(LoadoutRefusal r) { return replay::identity_access::LoadoutRefusalName(r); }

// The alignment gate's SECOND operand (IDENT.CUSTOMISATION_ALIGNMENT).  No test reached the
// alignment path before the AlignmentUnrecorded fallback case below, so the shared rig never wired
// it; this reads a namespace global the case sets, defaulting to a value no slot requires.
uint16_t g_entityAlign = identity_access::kSlotAlignmentNone;
bool     g_entityAlignOk = true;
bool EntityAlignmentFake(void*, uint16_t, uint16_t& out) {
    out = g_entityAlign;
    return g_entityAlignOk;
}

// Seed the LIVE loadout table so the Apply phase's LiveLoadoutInstances read is > 0 (the earlier
// call's publish, which the Apply phase does not re-drive).
void SeedLiveInstances(Rig& rig, uint16_t slot, uint64_t count) {
    replay::identity_access::LoadoutHeader h{};
    h.instanceCount = count;
    const size_t off = size_t(facts::IDENT_LOADOUT_TABLE_OFF) +
                       (size_t(slot) << size_t(facts::IDENT_LOADOUT_ENTRY_SHIFT));
    memcpy(&rig.mem[off], &h, sizeof h);
}

// Models the REAL engine parser (sub_140C4C920), unlike Parse above which memcpy's a whole matching
// header: force-zero `number` (+0x38), INHERIT `numberBody` (+0x3A) from the container untouched, and
// append one instance from the document.  This is the behaviour ENG-NUMBERBODY-FIX targets -- the fix
// seeds rec.numberBody before this parse so the inherited value matches at L3.
void InheritingParse(void* container, void* doc) {
    g_rig->order += "parse;";
    g_rig->parseDst = container;
    g_rig->parseDoc = doc;
    auto* h = static_cast<replay::identity_access::LoadoutHeader*>(container);
    h->number = 0;              // force-zeroed by the engine; the document has no loadout|number
    h->instanceCount = 1;       // one loadout|instances member in the document
    // h->numberBody deliberately NOT written -- inherited from whatever the container already held.
}

// ---- ENG-OUTFITS: the document authored for a capture that recorded none ----------------------
//
// The engine's own per-slot writer stands in twice over: it turns an id into a NAME through the
// registry the engine keeps, and it writes that name into the document.  The stand-in below does
// both, and answers "" for an id the registry cannot name -- the arm the real writer takes when
// the item is not in this client's catalogue, which writes nothing at all.
struct ItemRegistry {
    uint64_t    id[24] = {};
    const char* name[24] = {};
    uint16_t    n = 0;
    void Add(uint64_t v, const char* s) { id[n] = v; name[n] = s; ++n; }
    const char* Name(uint64_t v) const {
        for (uint16_t i = 0; i < n; ++i)
            if (id[i] == v) return name[i];
        return "";
    }
};

struct SlotSink {
    Doc          doc{};
    ItemRegistry reg{};
    // What the holder pointed at, read during the call: the holder itself may be the caller's stack
    // object and is gone once the fill returns.
    void*        lastHolderDoc = nullptr;
    int          calls = 0;
};
SlotSink* g_sink = nullptr;

void SetSlotFromId(void* holder, const uint64_t* itemId, const char* slotName) {
    REQUIRE(g_sink != nullptr);
    ++g_sink->calls;
    g_sink->lastHolderDoc = *static_cast<void**>(holder);
    const char* item = g_sink->reg.Name(*itemId);
    if (*item == 0) return;          // the engine writes nothing for an id it cannot name
    Doc& d = g_sink->doc;
    REQUIRE(d.count < 24);
    d.nodes[d.count].name = slotName;
    d.nodes[d.count].kind = facts::IDENT_JSON_KIND_STRING;
    d.nodes[d.count].value = item;
    ++d.count;
}

int64_t PathCount(void* json, const char* path) {
    static std::string parent;
    if (parent.empty()) {
        parent = facts::IDENT_LOADOUT_INSTANCE_PATH_FMT_TEXT;
        parent.erase(parent.rfind('|'));
    }
    if (path == nullptr || parent != path) return 0;
    return int64_t(static_cast<Doc*>(json)->count);
}

// A reader over the sink's document, with the child count the fill asks after every write.
entity_json::CJsonReader SinkReader(SlotSink& s) {
    entity_json::CJsonReader r = MakeReader(s.doc);
    r.pathCount = &PathCount;
    return r;
}

}  // namespace loadoutfix

TEST_CASE("outfits_the_authored_document_carries_the_recording_in_the_engines_own_schema") {
    using namespace loadoutfix;
    namespace ia = replay::identity_access;

    char inst[64] = {0};
    REQUIRE(ia::LoadoutTemplateInstance(inst, sizeof inst));

    char buf[512];
    ia::AuthoredLoadout st{};
    // Every glyph JSON may not carry raw, in one name: a quote, a backslash, a newline, a control
    // byte and a two-byte UTF-8 sequence.
    const size_t n = ia::AuthorProfileDocument("a\"b\\c\nd\x01" "e\xc3\xa9", 3, 7, buf, sizeof buf, st);
    REQUIRE(n != 0);
    CHECK(!st.nameOmitted);
    CHECK(::strlen(buf) == n);
    // It passes the plugin's OWN gate on a document before the engine is handed one.
    CHECK(ia::RecordedProfileJsonAcceptable(Blob{reinterpret_cast<const uint8_t*>(buf), uint32_t(n)}));
    const std::string doc(buf, n);
    CHECK(doc.find("\"displayname\":\"a\\\"b\\\\c\\nd\\u0001e\xc3\xa9\"") != std::string::npos);
    CHECK(doc.find("\"number\":3") != std::string::npos);
    CHECK(doc.find("\"number_body\":7") != std::string::npos);
    // The instance segment is the engine's own, and the slots object is EMPTY: the items are the
    // engine writer's to put in, and a slot this build invented would be an outfit nobody wore.
    CHECK(doc.find(std::string("\"") + inst + "\":{\"slots\":{}}") != std::string::npos);

    // A name that is not valid UTF-8 costs the name, never the outfit: the document is still whole
    // and still carries both numbers.
    ia::AuthoredLoadout bad{};
    const size_t m = ia::AuthorProfileDocument("bad\x80name", 11, 12, buf, sizeof buf, bad);
    REQUIRE(m != 0);
    CHECK(bad.nameOmitted);
    CHECK(std::string(buf, m).find("displayname") == std::string::npos);
    CHECK(std::string(buf, m).find("\"number\":11") != std::string::npos);
    CHECK(ia::RecordedProfileJsonAcceptable(Blob{reinterpret_cast<const uint8_t*>(buf), uint32_t(m)}));

    // A buffer that cannot hold it writes NOTHING, rather than a truncated document the engine
    // would fail to parse.
    char tiny[24];
    ia::AuthoredLoadout small{};
    CHECK(ia::AuthorProfileDocument("scavarax", 1, 1, tiny, sizeof tiny, small) == 0);
    CHECK(tiny[0] == '\0');
}

TEST_CASE("outfits_a_row_with_all_twenty_items_reaches_the_writer_and_round_trips") {
    using namespace loadoutfix;
    namespace ia = replay::identity_access;
    SlotSink sink;
    g_sink = &sink;
    g_doc = nullptr;   // the hasher's counters belong to the L1 fixtures, not to this one

    static std::string values[20];
    uint64_t keys[20], items[20];
    const uint16_t names = ia::LoadoutSlotNameCount();
    REQUIRE(names == 20);
    for (uint16_t i = 0; i < names; ++i) {
        values[i] = std::string("item_") + ia::LoadoutSlotName(i);
        keys[i] = FoldHash(ia::LoadoutSlotName(i));
        items[i] = FoldHash(values[i].c_str());
        sink.reg.Add(items[i], values[i].c_str());
    }

    entity_json::CJsonReader r = SinkReader(sink);
    LoadoutEngine e{};
    e.hash = &Hash;
    e.setSlotFromId = &SetSlotFromId;
    ia::AuthoredLoadout st{};
    REQUIRE(ia::FillAuthoredLoadoutSlots(r, e, keys, items, 20, st));
    CHECK(st.written == 20);
    CHECK(st.inDocument == 20);
    CHECK(st.unfilled == 0);
    CHECK(st.unresolvedSlot == 0);
    CHECK(st.unresolvedItem == 0);
    // The writer was handed the DOCUMENT, not a copy of the reader.
    CHECK(sink.lastHolderDoc == r.json);

    // THE ROUND TRIP: the document the writer filled reproduces the recorded pairs, read back
    // through the plugin's own L1 comparison rather than by inspecting the nodes.
    RecordedLoadout rec{};
    rec.hasSlotKeys = true;
    rec.documentRecorded = true;
    rec.filledPairCount = 20;
    rec.number = 3;
    rec.numberBody = 7;
    for (uint16_t i = 0; i < 20; ++i) { rec.pairs[i].slotKey = keys[i]; rec.pairs[i].itemId = items[i]; }
    LoadoutEngine verify = MakeEngine(r);
    LoadoutSteps s;
    const LoadoutRefusal why =
        ia::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, verify, s);
    CHECK(s.compared == 20);
    CHECK(s.unfilled == 0);
    CHECK(s.skewSlot == replay::identity_access::kLoadoutNoSlot);   // no pair is missing from it
    (void)why;
    g_sink = nullptr;
}

TEST_CASE("outfits_only_the_recorded_slots_are_written_and_the_losses_are_counted") {
    using namespace loadoutfix;
    namespace ia = replay::identity_access;
    SlotSink sink;
    g_sink = &sink;
    g_doc = nullptr;

    static std::string values[20];
    uint64_t keys[20] = {}, items[20] = {};
    for (uint16_t i = 0; i < 20; ++i) {
        values[i] = std::string("item_") + ia::LoadoutSlotName(i);
        keys[i] = FoldHash(ia::LoadoutSlotName(i));
        items[i] = FoldHash(values[i].c_str());
    }
    // Six real slots the client knows, then: an unworn slot, a padding row, a slot key none of the
    // twenty names hash to, and an item id the engine's registry cannot name.
    for (uint16_t i = 0; i < 6; ++i) sink.reg.Add(items[i], values[i].c_str());
    items[6] = ~uint64_t(0);                 // the engine's "wears nothing"
    keys[7] = 0;                             // padding
    keys[8] = FoldHash("a_slot_this_build_does_not_know");
    // keys[9]/items[9] are a real pair whose item was never added to the registry.

    entity_json::CJsonReader r = SinkReader(sink);
    LoadoutEngine e{};
    e.hash = &Hash;
    e.setSlotFromId = &SetSlotFromId;
    ia::AuthoredLoadout st{};
    REQUIRE(ia::FillAuthoredLoadoutSlots(r, e, keys, items, 10, st));
    CHECK(st.written == 7);            // six known + the one the registry could not name
    CHECK(st.inDocument == 6);         // only six reached the document
    CHECK(sink.doc.count == 6);
    CHECK(st.unfilled == 2);           // the unworn slot and the padding row
    CHECK(st.unresolvedSlot == 1);
    CHECK(st.firstUnresolvedSlotKey == keys[8]);
    CHECK(st.unresolvedItem == 1);
    CHECK(st.firstUnresolvedItemId == items[9]);
    // Nothing was invented for the ten pairs past the recorded count.
    CHECK(sink.calls == 7);
    g_sink = nullptr;
}

TEST_CASE("outfits_a_missing_surface_is_a_could_not_ask_and_writes_nothing") {
    using namespace loadoutfix;
    namespace ia = replay::identity_access;
    SlotSink sink;
    g_sink = &sink;
    g_doc = nullptr;
    uint64_t keys[2] = {FoldHash(ia::LoadoutSlotName(0)), FoldHash(ia::LoadoutSlotName(1))};
    uint64_t items[2] = {1, 2};
    entity_json::CJsonReader r = SinkReader(sink);

    LoadoutEngine noWriter{};
    noWriter.hash = &Hash;
    ia::AuthoredLoadout st{};
    CHECK(!ia::FillAuthoredLoadoutSlots(r, noWriter, keys, items, 2, st));
    LoadoutEngine noHash{};
    noHash.setSlotFromId = &SetSlotFromId;
    CHECK(!ia::FillAuthoredLoadoutSlots(r, noHash, keys, items, 2, st));
    entity_json::CJsonReader noDoc = r;
    noDoc.json = nullptr;
    LoadoutEngine both{};
    both.hash = &Hash;
    both.setSlotFromId = &SetSlotFromId;
    CHECK(!ia::FillAuthoredLoadoutSlots(noDoc, both, keys, items, 2, st));
    CHECK(sink.calls == 0);
    CHECK(st.written == 0);
    g_sink = nullptr;
}

TEST_CASE("l1_refuses_a_v1_block_and_an_absent_document_by_name") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;

    // (16) absent_document_refuses_by_name -- no authoring, and nothing after L1 is reached.
    rec.documentRecorded = false;
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::DocumentNotRecorded);
    CHECK(s.outcome[size_t(LoadoutStep::Attach)] == LoadoutOutcome::NotReached);
    CHECK(s.compared == 0);
    CHECK(d.writes == 0);

    // (4) the v1 block: it carries items by POSITION, and position is a guess.
    rec.documentRecorded = true;
    rec.hasSlotKeys = false;
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::SlotKeysNotRecorded);
    CHECK(strcmp(Name(s.why[size_t(LoadoutStep::Verify)]), "LoadoutSlotKeysNotRecorded") == 0);
    g_doc = nullptr;
}

TEST_CASE("zero_and_unknown_counts_refuse_by_name") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;

    rec.filledPairCount = replay::identity_access::kLoadoutPairCountUnknown;
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::PairCountUnknown);
    CHECK(s.outcome[size_t(LoadoutStep::Attach)] == LoadoutOutcome::NotReached);

    rec.filledPairCount = 0;
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::NoSlotsRecorded);

    // A count that disagrees with the pairs actually present is neither of those two.
    FillRecorded(d, rec, 5);
    rec.filledPairCount = 6;
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::PairCountMismatch);
    g_doc = nullptr;
}

TEST_CASE("partial_component_verifies_its_own_count") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    RecordedLoadout rec; FillRecorded(d, rec, 7);   // seven real pairs, thirteen {0,0} rows
    LoadoutSteps s;

    // L1 passes on seven and the sequence goes on to L2; it compares seven, never twenty.
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) !=
          LoadoutRefusal::None);
    CHECK(s.outcome[size_t(LoadoutStep::Verify)] == LoadoutOutcome::Issued);
    CHECK(s.compared == 7);
    g_doc = nullptr;
}

TEST_CASE("l1_verifies_and_catches_a_skew") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    e.setString = &SetString;      // the writer exists, so "authors nothing" can be falsified
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;

    // Nineteen match and one does not.  A build that skips the comparison passes the nineteen
    // and misses the twentieth.
    rec.pairs[17].itemId ^= 0x40ull;
    const Doc before = d;
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::DocumentPairsSkew);
    CHECK(s.skewSlot == 17);
    // REQUIRE, not CHECK: a build that refused something else leaves skewSlot at "no slot", and
    // this case must FAIL rather than dereference a null name and take the whole binary with it.
    const char* skewName = replay::identity_access::LoadoutSlotName(s.skewSlot);
    REQUIRE(skewName != nullptr);
    CHECK(strcmp(skewName, facts::IDENT_LOADOUT_SLOT_NAME_17_TEXT) == 0);
    // The document is byte-identical afterwards, and no writer was ever called.
    CHECK(memcmp(before.nodes, d.nodes, sizeof d.nodes) == 0);
    CHECK(d.writes == 0);

    // And with the skew removed the same fixture verifies all twenty.
    FillRecorded(d, rec, 20);
    (void)replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s);
    CHECK(s.compared == 20);
    CHECK(s.outcome[size_t(LoadoutStep::Verify)] == LoadoutOutcome::Issued);
    CHECK(d.writes == 0);
    g_doc = nullptr;
}

TEST_CASE("sentinel_never_verifies") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;

    // Twenty recorded pairs and six of them are slots the player wore NOTHING in.  The engine's
    // container is fixed width and writes -1 into an empty pair, so this is the shape a real
    // capture has: `v3 pairs 20/20` with sentinels among them (L3-RUN-32).  hash(NULL) ==
    // hash("") == the seed == -1, so there is nothing in those six to verify -- they are skipped
    // and counted, and the whole dress must NOT be refused because of them.
    const size_t empties[6] = {0, 3, 4, 11, 18, 19};
    for (size_t i = 0; i < 6; ++i) rec.pairs[empties[i]].itemId = ~uint64_t(0);

    d.hashCalls = 0;
    const LoadoutRefusal why =
        replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s);

    // L1 ISSUED and L2 was reached: what comes back is L2's refusal (there is no NetGame here),
    // never L1's, and the empty slots are named as empty rather than as a fault.
    CHECK(s.outcome[size_t(LoadoutStep::Verify)] == LoadoutOutcome::Issued);
    CHECK(s.why[size_t(LoadoutStep::Verify)] == LoadoutRefusal::None);
    CHECK(s.outcome[size_t(LoadoutStep::Attach)] != LoadoutOutcome::NotReached);
    CHECK(why != LoadoutRefusal::PairUnfilled);
    CHECK(s.compared == 14);
    CHECK(s.unfilled == 6);
    // COUNTED, not detected: twenty slot names, one instance segment, and fourteen document
    // values.  A build that hashed an empty slot's node would read 41 here.
    CHECK(d.hashCalls == 20 + 1 + 14);
    CHECK(d.writes == 0);
    g_doc = nullptr;
}

TEST_CASE("unfilled_slot_may_have_no_node_but_a_filled_one_may_not") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    RecordedLoadout rec; FillRecorded(d, rec, 20);   // keys derived BEFORE the node is renamed
    LoadoutSteps s;

    // An empty slot the document does not carry a node for at all: still an empty slot.
    rec.pairs[4].itemId = ~uint64_t(0);
    d.nodes[4].name = "not-a-slot-name";
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) !=
          LoadoutRefusal::PairUnfilled);
    CHECK(s.outcome[size_t(LoadoutStep::Verify)] == LoadoutOutcome::Issued);
    CHECK(s.compared == 19);
    CHECK(s.unfilled == 1);

    // Same missing node, but the capture says the player WAS wearing that item: the pairs and
    // the profile came out of two different engine objects and they disagree.  That skew is the
    // only thing this word means now.
    rec.pairs[4].itemId = FoldHash(d.nodes[4].value);
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::PairUnfilled);
    CHECK(s.skewSlot == 4);
    g_doc = nullptr;
}

TEST_CASE("non_string_node_refuses_before_hashing") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    RecordedLoadout rec; FillRecorded(d, rec, 1);
    LoadoutSteps s;

    d.nodes[0].kind = facts::IDENT_JSON_KIND_INT;
    d.hashCalls = 0;
    d.lastHashed.clear();
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::SlotValueNotString);
    // COUNTED, not detected: the hasher ran exactly once per slot NAME plus once for the
    // instance segment, and not once more.  It never saw this node -- which is what asking the
    // kind first buys, and a build that hashed the accessor's default would be one call over.
    CHECK(d.hashCalls == replay::identity_access::LoadoutSlotNameCount() + 1);
    CHECK(d.lastHashed != d.nodes[0].value);
    g_doc = nullptr;
}

TEST_CASE("hasher_is_called_with_a3_zero") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;
    d.lastA3 = 99;
    (void)replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s);
    CHECK(d.hashCalls > 20);
    CHECK(d.lastA3 == 0);
    g_doc = nullptr;
}

TEST_CASE("slot_names_not_positions") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;

    // The recorded order is NOT the name table's order.  A verifier that took position for the
    // slot reads every pair against the wrong node.
    for (uint16_t i = 0; i + 1 < 20; i += 2) {
        LoadoutPair t = rec.pairs[i];
        rec.pairs[i] = rec.pairs[i + 1];
        rec.pairs[i + 1] = t;
    }
    (void)replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s);
    CHECK(s.outcome[size_t(LoadoutStep::Verify)] == LoadoutOutcome::Issued);
    CHECK(s.compared == 20);

    // A key that is none of the twenty names' hashes is not a slot this build can name.
    rec.pairs[3].slotKey = 0x1234567890abcdefull;
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::SlotKeyUnresolved);
    g_doc = nullptr;
}

TEST_CASE("instance_name_unresolved_refuses") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    e.instanceSymbol = &WrongInstanceSymbol;
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;
    CHECK(replay::identity_access::DressLoadout(nullptr, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::InstanceNameUnknown);
    g_doc = nullptr;
}

TEST_CASE("l3_is_called_and_fills_the_container") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    WireRig(e);
    Rig rig; BuildRig(rig, 1); g_rig = &rig;
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    SeedApplied(rig, rec, 20);
    LoadoutSteps s;

    const LoadoutRefusal why =
        replay::identity_access::DressLoadout(rig.ng, UserSlot(1), UserIndex(4), rec, e, s);
    INFO("refused: " << Name(why) << " order " << rig.order);
    CHECK(why == LoadoutRefusal::None);

    // The parse's two arguments, which is the whole claim: the per-user container for the SLOT,
    // and the document we verified -- not a second document and not another slot's block.
    CHECK(rig.parseDst == replay::identity_access::LoadoutContainer(rig.ng, UserSlot(1)));
    CHECK(rig.parseDoc == static_cast<const void*>(&d));
    // L4's source is that same container and its destination the live per-slot table.
    CHECK(rig.publishSrc == rig.parseDst);
    CHECK(rig.publishDst ==
          static_cast<void*>(replay::identity_access::LiveLoadout(rig.ng, UserSlot(1))));
    g_rig = nullptr;
    g_doc = nullptr;
}

TEST_CASE("no_step_is_skipped_silently") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    WireRig(e);
    Rig rig; BuildRig(rig, 2); g_rig = &rig;
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    SeedApplied(rig, rec, 20);
    LoadoutSteps s;

    CHECK(replay::identity_access::DressLoadout(rig.ng, UserSlot(2), UserIndex(2), rec, e, s) ==
          LoadoutRefusal::None);
    for (size_t i = 0; i < size_t(LoadoutStep::kCount); ++i) {
        INFO("step " << replay::identity_access::LoadoutStepName(LoadoutStep(i)));
        CHECK(s.outcome[i] == LoadoutOutcome::Issued);
    }
    // (7) alignment_runs_after_the_apply, and (9) the count is the engine's own qword.
    CHECK(rig.order == "attach;parse;publish;enabled;apply;readback;align;");
    CHECK(s.instances == replay::identity_access::LiveLoadout(rig.ng, UserSlot(2))->instanceCount);
    CHECK(s.instances == 1);
    CHECK(s.matched == 20);
    CHECK(s.mismatched == 0);
    g_rig = nullptr;
    g_doc = nullptr;
}

TEST_CASE("source_phase_fills_the_source_only_and_leaves_the_bridge_to_publish") {
    // The Source phase runs BEFORE admission on the engine's remote reply route:
    // it must parse the recorded loadout into the per-user SOURCE container and STOP -- no L2 attach
    // (there is no admitted entrant to attach to) and no L4 publish (the engine's own add-entrant
    // bridge copies the SOURCE into the live table at admission).  This case pins exactly that.
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    WireRig(e);
    Rig rig; BuildRig(rig, 1); g_rig = &rig;
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;

    // A deliberately-bogus index: the Source phase must not touch it, because L2 is skipped.  An
    // implementation that ran L2 would call Attach with this index and the order would show it.
    const LoadoutRefusal why = replay::identity_access::DressLoadout(
        rig.ng, UserSlot(1), UserIndex(0xFFFF), rec, e, s, LoadoutPhase::Source);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    INFO("refused: " << Name(why) << " order " << rig.order);
    CHECK(why == LoadoutRefusal::None);

    // L1 verified, L3 parsed, and NOTHING ELSE ran -- this is the whole claim of the phase.
    CHECK(rig.order == "parse;");
    CHECK(s.outcome[size_t(LoadoutStep::Verify)] == LoadoutOutcome::Issued);
    CHECK(s.outcome[size_t(LoadoutStep::Parse)] == LoadoutOutcome::Issued);
    CHECK(s.outcome[size_t(LoadoutStep::Attach)] == LoadoutOutcome::NotReached);
    CHECK(s.outcome[size_t(LoadoutStep::Publish)] == LoadoutOutcome::NotReached);

    // The parse wrote the per-user SOURCE for the SLOT, and the reported count -- the launch signal
    // -- is the non-empty count the engine's bridge will copy into the live table.
    CHECK(rig.parseDst == replay::identity_access::LoadoutContainer(rig.ng, UserSlot(1)));
    CHECK(s.instances == 1);

    // The LIVE table is UNTOUCHED: the Source phase issued no publish of its own, so the live count
    // is still zero here and only the engine's own admission bridge will fill it.
    CHECK(rig.publishDst == nullptr);
    CHECK(replay::identity_access::LiveLoadout(rig.ng, UserSlot(1))->instanceCount == 0);
    g_rig = nullptr;
    g_doc = nullptr;
}

TEST_CASE("The Source phase seeds rec.numberBody so the engine's inherit passes L3") {
    // The real engine parser force-zeroes `number` and INHERITS `numberBody` from the container (the
    // identity-block document carries neither key).  On the bot path GenerateBotProfile left the BOT's
    // numberBody in the container, which the keyless document never overwrites -- so L3's
    // src->numberBody != rec.numberBody would refuse.  The fix seeds rec.numberBody into the container
    // on the Source phase, before the parse, so the inherited value matches.  InheritingParse models
    // the engine's actual inherit behaviour (Parse above hides it by overwriting the whole header).
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    WireRig(e);
    e.parse = &InheritingParse;

    // The engine zeroes `number` and the document has no loadout|number, so src->number is 0 after the
    // parse; L3 passes only when the recorded number is 0 (the number-0 fixture this fix targets).  We
    // pin rec.number = 0 so numberBody is the SOLE differing operand -- the isolated discriminator.
    RecordedLoadout recSource; FillRecorded(d, recSource, 20);
    recSource.number = 0;
    recSource.numberBody = 7;

    // (A) SOURCE phase: the container starts poisoned with a foreign numberBody (the bot's body number,
    // 99 != 7).  The keyless InheritingParse will NOT overwrite it -- only the Source-phase seed can.
    {
        Rig rig; BuildRig(rig, 1); g_rig = &rig;
        auto* container = static_cast<replay::identity_access::LoadoutHeader*>(
            replay::identity_access::LoadoutContainer(rig.ng, UserSlot(1)));
        REQUIRE(container != nullptr);
        container->numberBody = 99;

        LoadoutSteps s;
        const LoadoutRefusal why = replay::identity_access::DressLoadout(
            rig.ng, UserSlot(1), UserIndex(0xFFFF), recSource, e, s, LoadoutPhase::Source);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        INFO("SOURCE refused: " << Name(why) << " numberBody " << container->numberBody);
        // The seed landed and survived the inherit: the container reads back rec.numberBody, not 99.
        CHECK(container->numberBody == recSource.numberBody);
        CHECK(why == LoadoutRefusal::None);
        CHECK(s.instances == 1);
        g_rig = nullptr;
    }

    // (B) NON-Source phase, same poisoned container and same inheriting parse -- the seed is gated on
    // the Source phase, so it does NOT run here.  This is the "without the write" arm: the inherited 99
    // never matches rec.numberBody (7), so L3 refuses ParseDidNotFill and the count is left unread.
    // Same inputs as (A); the only difference is whether the Source-phase seed fired -- proving the
    // seed is load-bearing.
    {
        Rig rig; BuildRig(rig, 1); g_rig = &rig;
        SeedApplied(rig, recSource, 20);
        auto* container = static_cast<replay::identity_access::LoadoutHeader*>(
            replay::identity_access::LoadoutContainer(rig.ng, UserSlot(1)));
        REQUIRE(container != nullptr);
        container->numberBody = 99;

        LoadoutSteps s;
        const LoadoutRefusal why = replay::identity_access::DressLoadout(
            rig.ng, UserSlot(1), UserIndex(1), recSource, e, s);   // default (non-Source) phase
        INFO("NON-SOURCE refused: " << Name(why) << " numberBody " << container->numberBody);
        CHECK(container->numberBody == 99);                        // unseeded: still the poison
        CHECK(why == LoadoutRefusal::ParseDidNotFill);
        CHECK(s.instances == replay::identity_access::kLoadoutInstancesUnreadable);  // gate returned first
        g_rig = nullptr;
    }
    g_doc = nullptr;
}

TEST_CASE("twenty_of_twenty_compares_recorded_against_engine") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    WireRig(e);
    Rig rig; BuildRig(rig, 1); g_rig = &rig;
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    LoadoutSteps s;

    // The ENGINE side disagrees with the recording on one slot.  A comparison that read the
    // authored side, or the container it published, would never see it.
    SeedApplied(rig, rec, 20);
    rig.applied[11].itemId ^= 0x8ull;
    CHECK(replay::identity_access::DressLoadout(rig.ng, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::AppliedPairsMismatch);
    CHECK(s.mismatched == 1);
    CHECK(s.matched == 19);

    // A component that holds a slot the recording does not name is still a mismatch on OUR
    // side's key, not a silent pass.
    SeedApplied(rig, rec, 20);
    rig.applied[11].slotKey = 0xdeadbeefull;
    CHECK(replay::identity_access::DressLoadout(rig.ng, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::AppliedPairsMismatch);
    CHECK(s.mismatched == 1);
    g_rig = nullptr;
    g_doc = nullptr;
}

TEST_CASE("l5_gate_refuses_when_not_authoritative") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    WireRig(e);
    Rig rig; BuildRig(rig, 1); g_rig = &rig;
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    SeedApplied(rig, rec, 20);
    LoadoutSteps s;

    // ApplyCurrentLoadout returns without applying anything when this bit is set.
    rig.flags = 0x4;
    CHECK(replay::identity_access::DressLoadout(rig.ng, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::NotAuthoritative);
    CHECK(rig.applyItemCalls == 0);
    CHECK(rig.order.find("align;") == std::string::npos);

    // And hop 2's own re-read is the other half of the gate: an actor that is not enabled gets
    // no apply, however authoritative the session is.
    rig.flags = 0;
    rig.actorEnabled = false;
    rig.order.clear();
    CHECK(replay::identity_access::DressLoadout(rig.ng, UserSlot(1), UserIndex(1), rec, e, s) ==
          LoadoutRefusal::ActorNotEnabled);
    CHECK(rig.applyItemCalls == 0);
    g_rig = nullptr;
    g_doc = nullptr;
}

// AUD-W2-P1-46 MAJOR-2 / P1-45 s4.  THE LIVE FALLBACK MUST FIRE ON THE COMMON CAPTURE SHAPE.  A
// recording that carried no alignment (rec.alignment == None) whose read-back holds a slot the
// engine's alignment gate skipped scores AlignmentUnrecorded, not AppliedPairsMismatch -- and the
// enable already fired, so appliedByUs is false on the ordered (Apply) phase.  The fallback exists
// exactly for this state; keyed on the mismatch word alone (L5_FALLBACK_MISMATCH_ONLY) it never
// runs and the entrant wears nothing.  So: applyItems is driven once and appliedBy is Fallback.
TEST_CASE("l5_fallback_fires_on_alignment_unrecorded_not_only_mismatch") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    WireRig(e);
    e.entityAlignment = &EntityAlignmentFake;   // no shared test wired this operand before now
    Rig rig; BuildRig(rig, 1); g_rig = &rig;
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    rec.alignment = identity_access::kSlotAlignmentNone;   // the recording never carried one

    // The read-back agrees on nineteen slots and, on slot 0, holds an item the recording did not
    // ask for behind an alignment requirement the entrant does not meet -- the engine's own skip.
    SeedApplied(rig, rec, 20);
    rig.applied[0].itemId ^= 0x40ull;                                    // not the recorded item
    rig.appliedState[0].flags = 0;                                       // NOT the pending arm
    rig.appliedState[0].alignmentReq = 5;                                // a real requirement...
    g_entityAlign = 3;                                                   // ...the entrant does not meet
    g_entityAlignOk = true;
    SeedLiveInstances(rig, 1, 1);   // the earlier call's publish: the Apply phase reads it back

    LoadoutSteps s;
    const LoadoutRefusal got = replay::identity_access::DressLoadout(
        rig.ng, UserSlot(1), UserIndex(1), rec, e, s,
        replay::identity_access::LoadoutPhase::Apply);

    // The state the fallback is written for: scored AlignmentUnrecorded, and the ordered phase
    // issued no apply of its own (appliedByUs false), so the fallback is admissible.
    CHECK(s.mismatched == 1);
    // THE POINT.  The fallback ran once and named itself; under L5_FALLBACK_MISMATCH_ONLY it does
    // not fire for this word and both of these read their pre-fallback values (0 / Engine).
    CHECK(rig.applyItemCalls == 1);
    CHECK(s.appliedBy == replay::identity_access::LoadoutApplyBy::Fallback);
    // The fake's read-back does not change under the re-apply, so the second score returns the
    // same word -- this asserts WHICH word drove the fallback, not that the re-apply satisfied it.
    CHECK(got == LoadoutRefusal::AlignmentUnrecorded);

    g_entityAlign = identity_access::kSlotAlignmentNone;
    g_rig = nullptr;
    g_doc = nullptr;
}

TEST_CASE("l4_reads_the_published_flag_and_refuses_a_one") {
    using namespace loadoutfix;
    Doc d; FillDocument(d); g_doc = &d;
    entity_json::CJsonReader r = MakeReader(d);
    LoadoutEngine e = MakeEngine(r);
    WireRig(e);
    Rig rig; BuildRig(rig, 3); g_rig = &rig;
    RecordedLoadout rec; FillRecorded(d, rec, 20);
    SeedApplied(rig, rec, 20);
    LoadoutSteps s;

    // The expected value for a remote-admitted, non-bot entrant on the host is 0.  A 1 means a
    // path we did not model ran, so nothing is published into it -- and the flag is READ, never
    // written: the fixture's byte is the same afterwards.
    const size_t off = size_t(facts::USER_PROFILE_STRIDE) * 3 +
                       size_t(facts::IDENT_LOADOUT_PUBLISHED_FLAG_OFF);
    const uint32_t one = 1;
    memcpy(&rig.mem[off], &one, sizeof one);
    CHECK(replay::identity_access::DressLoadout(rig.ng, UserSlot(3), UserIndex(3), rec, e, s) ==
          LoadoutRefusal::PublishFlagUnexpected);
    CHECK(rig.order.find("publish;") == std::string::npos);
    uint32_t after = 0;
    memcpy(&after, &rig.mem[off], sizeof after);
    CHECK(after == 1);
    g_rig = nullptr;
    g_doc = nullptr;
}

TEST_CASE("the recording-side preconditions are one function, and the census asks it") {
    using namespace loadoutfix;
    RecordedLoadout rec{};
    // Nothing recorded at all: the document is the first thing missing.
    CHECK(replay::identity_access::RecordedLoadoutRefusal(rec) ==
          LoadoutRefusal::DocumentNotRecorded);
    rec.documentRecorded = true;
    // A v1 identity block -- the state every recording this system has made is in.
    CHECK(replay::identity_access::RecordedLoadoutRefusal(rec) ==
          LoadoutRefusal::SlotKeysNotRecorded);
    CHECK(strcmp(Name(replay::identity_access::RecordedLoadoutRefusal(rec)),
                 "LoadoutSlotKeysNotRecorded") == 0);
    rec.hasSlotKeys = true;
    rec.filledPairCount = replay::identity_access::kLoadoutPairCountUnknown;
    CHECK(replay::identity_access::RecordedLoadoutRefusal(rec) ==
          LoadoutRefusal::PairCountUnknown);
    rec.filledPairCount = 0;
    CHECK(replay::identity_access::RecordedLoadoutRefusal(rec) ==
          LoadoutRefusal::NoSlotsRecorded);
    rec.filledPairCount = 2;
    CHECK(replay::identity_access::RecordedLoadoutRefusal(rec) ==
          LoadoutRefusal::PairCountMismatch);
    rec.pairs[0].slotKey = 0x11;
    rec.pairs[1].slotKey = 0x22;
    CHECK(replay::identity_access::RecordedLoadoutRefusal(rec) == LoadoutRefusal::None);
}

// ★ A 0xFFFF hole in the reverse map is a real engine state, and the walk must
// step over it: without the guard the forward-map read at 4*0xFFFF is off the end of the map and
// the answer flips from "no CI for this entrant yet" to "the pool could not be read", which is a
// fault where a wait belongs -- and a fault does not re-ask on the next cadence window.
TEST_CASE("W2-P1-30d: a hole in the reverse map is stepped over, not read as an unreadable pool") {
    HopFixture f;
    CiFixture  ci;
    g_ci = &ci;
    f.surface.remotePlayerCs = &FakeRemotePlayerCs;
    replay::identity_access::BindActorHops(f.surface);
    REQUIRE(f.surface.hops.physicsCiAwaiting != nullptr);

    // The hole is inside the live count, and the element that carries kHopIndex is found before
    // it: the walk answers about the entrant and never touches the hole.
    CHECK(*reinterpret_cast<const uint16_t*>(ci.cs + facts::PLAYER_CS_COUNT_OFF) == 3);
    CHECK(ci.rev[2] == uint16_t(~uint16_t(0)));
    ci.Witness() = 0x00007FF612345678ull;
    CHECK(f.surface.hops.physicsCiAwaiting(&f.surface, kHopIndex) ==
          PawnProvision::PhysicsCiReq::None);

    // NOW the entrant is not in the pool at all, so the walk runs past the hole to the end.  The
    // honest answer is NotAttempted -- nobody built a CI for this entrant -- never Unreadable.
    ci.Word(facts::PLAYER_CI_USERID_OFF) = uint16_t(kHopIndex + 1);
    CHECK(f.surface.hops.physicsCiAwaiting(&f.surface, kHopIndex) ==
          PawnProvision::PhysicsCiReq::NotAttempted);
    g_ci = nullptr;
}

// ★ A RecordedLoadout nobody has filled is a recording NOBODY HAS READ, and
// every field has to default to its could-not-ask value.  With filledPairCount defaulting to 0 the
// refusal is LoadoutNoSlotsRecorded -- a claim that the entrant wore nothing -- where
// LoadoutPairCountUnknown is the truth.
TEST_CASE("W2-P1-30d: a value-initialised RecordedLoadout is a could-not-ask, never an empty one") {
    using replay::identity_access::LoadoutRefusal;
    using replay::identity_access::RecordedLoadout;

    RecordedLoadout fresh{};
    CHECK(fresh.filledPairCount == replay::identity_access::kLoadoutPairCountUnknown);
    CHECK_FALSE(fresh.hasSlotKeys);
    CHECK_FALSE(fresh.documentRecorded);

    // With the two recording-side preconditions met, the COUNT is what is left to answer, and the
    // answer for a struct nobody filled is "could not ask".
    fresh.documentRecorded = true;
    fresh.hasSlotKeys = true;
    CHECK(replay::identity_access::RecordedLoadoutRefusal(fresh) ==
          LoadoutRefusal::PairCountUnknown);
    CHECK(strcmp(replay::identity_access::LoadoutRefusalName(LoadoutRefusal::PairCountUnknown),
                 "LoadoutPairCountUnknown") == 0);

    // Zero is the DIFFERENT, real answer and keeps its own name.
    fresh.filledPairCount = 0;
    CHECK(replay::identity_access::RecordedLoadoutRefusal(fresh) ==
          LoadoutRefusal::NoSlotsRecorded);
}

// ★ THE SHIPPED SURFACE, BOUND.  This process is not echovr.exe, so every recorded VA rebases
// outside the mapped image: BindLoadoutEngine must answer FALSE and leave the four engine calls
// null, which is the LoadoutEngineUnbound path.  The trampolines are ours and always bind, and
// the gate must be hop 2's OWN pointer -- not a second binding of the same read.
TEST_CASE("W2-P1-30d: BindLoadoutEngine refuses off-image and takes its gate from hop 2") {
    HopFixture f;
    replay::identity_access::BindActorHops(f.surface);
    replay::identity_access::LoadoutEngine e{};
    const bool bound = replay::identity_access::BindLoadoutEngine(e, f.surface.hops);

    CHECK_FALSE(bound);                 // no engine image here: refused whole, driven not at all
    CHECK(e.hash == nullptr);
    CHECK(e.parse == nullptr);
    CHECK(e.publish == nullptr);
    CHECK(e.applyItems == nullptr);
    CHECK(e.setString == nullptr);      // L1 verifies and has no writer in reach

    CHECK(e.attach != nullptr);
    CHECK(e.attachedDoc != nullptr);
    CHECK(e.docUsable != nullptr);
    CHECK(e.readAppliedPairs != nullptr);
    CHECK(e.applyAlignment != nullptr);
    CHECK(e.instanceSymbol != nullptr);
    CHECK(e.instanceSymbol() == 0);     // the qword is off-image: unresolved, never a guess

    // THE SAME READ, not a second one.
    CHECK(e.actorEnabled == f.surface.hops.enabled);
    CHECK(e.actorCtx == f.surface.hops.ctx);
}

// ENG-W2-P1-54 (DIAG-MODEL-INSTANTIATE Lever A).  L7's driveModel is rebound to LoadoutApplyAlignment
// -- the engine's OWN productive apply (sub_140CAA5F0 -> APPLY_CURRENT_LOADOUT), whose worn-record loop
// runs Load-and-apply + Apply Chassis over a non-empty instance and installs the InstanceModel CI.  The
// direct per-pair Apply Chassis drive (ENG-W2-P1-51/53) was retired: Apply Chassis only SKINS an existing
// model-CI and its readiness gate refused silently on the entrant, so RUN-43 created no body.  Off-image
// (a self-test is not echovr.exe) the site cannot resolve, so both driveModel and applyAlignment refuse
// safely BEFORE the engine call -- ResolveComponent finds no component and the trampoline returns false.
TEST_CASE("W2-P1-54: driveModel is bound to the productive apply and refuses safely off-image") {
    HopFixture f;
    replay::identity_access::BindActorHops(f.surface);
    replay::identity_access::LoadoutEngine e{};
    replay::identity_access::BindLoadoutEngine(e, f.surface.hops);
    REQUIRE(e.driveModel != nullptr);
    // Rebound to the SAME productive apply as applyAlignment (both LoadoutApplyAlignment).
    CHECK(reinterpret_cast<void*>(e.driveModel) == reinterpret_cast<void*>(e.applyAlignment));

    // Off-image the resolve floor refuses -- no component resolves, so the engine is never entered
    // and the drive returns false (never a null-deref, the RUN-42 fault the retired path caused).
    uint8_t fakeNg[96] = {};
    CHECK_FALSE(e.driveModel(fakeNg, 1));
}

// ENG-W2-P1-55 (DIAG-SPAWN-MODEL-CI Lever B).  A4's spawn handler NETGAME.SPAWN_NOTIFY (sub_1401CAC00)
// is bound in the SHIPPED hop surface and, off-image (a self-test is not echovr.exe), refuses safely:
// the prologue guard at 0x1401CAC00 does not match, so the trampoline never resolves, no engine call
// is issued, and the result reads issued=false with no mode word -- never a call into stray bytes.
TEST_CASE("W2-P1-55: HopSpawnNotify is bound in the shipped surface and refuses safely off-image") {
    HopFixture f;   // its ctor already BindActorHops(surface); the index is in bounds
    REQUIRE(f.surface.hops.spawnNotify != nullptr);
    const PawnProvision::SpawnNotifyResult r =
        f.surface.hops.spawnNotify(f.surface.hops.ctx, kHopIndex);
    // Off-image the site cannot resolve, so nothing was issued and the branch probe stayed unread --
    // a could-not-ask, never a fabricated verdict about which arm ran.
    CHECK_FALSE(r.issued);
    CHECK_FALSE(r.modeRead);

    // An index past the entrant count refuses at the bound, before any resolve or mode read.
    const PawnProvision::SpawnNotifyResult oob =
        f.surface.hops.spawnNotify(f.surface.hops.ctx, uint16_t(kHopIndex + 5));
    CHECK_FALSE(oob.issued);
}

// ReadAppliedPairs' two reads used to walk the pointer the FIRST resolve
// returned, so against any fixture the two walks saw the same bytes by construction and
// AppliedSlotsUnstable was a refusal nothing could reach.  The walk is now a seam, each read goes
// through it, and these plant the disagreement the refusal exists for.
namespace appliedwalk {
using identity_access::AppliedSlotState;
using identity_access::LoadoutPair;

struct Planted {
    LoadoutPair pairs[20] = {};
    AppliedSlotState state[20] = {};
    uint64_t n = 0;
    unsigned calls = 0;
    bool flipItemOnSecond = false;
    bool shortenOnSecond = false;
    bool failSecond = false;
};
Planted* g_planted = nullptr;

bool Walk(void*, UserIndex, LoadoutPair out[20], AppliedSlotState* state,
          uint64_t& n, uint64_t* held) {
    ++g_planted->calls;
    if (g_planted->failSecond && g_planted->calls == 2) return false;
    memcpy(out, g_planted->pairs, sizeof g_planted->pairs);
    if (state) memcpy(state, g_planted->state, sizeof g_planted->state);
    n = g_planted->n;
    // The engine writing between the two reads, which is the whole hazard: one entry moves and
    // the walk that follows it is a second moment, not a corroboration of the first.
    if (g_planted->calls == 2) {
        if (g_planted->flipItemOnSecond) out[3].itemId ^= 1u;
        if (g_planted->shortenOnSecond) n = g_planted->n - 1;
    }
    if (held) *held = n;
    return true;
}

struct Scoped {
    Planted p;
    Scoped() { g_planted = &p; identity_access::AppliedSlotsInjectWalkForTest(&Walk); }
    ~Scoped() { identity_access::AppliedSlotsInjectWalkForTest(nullptr); g_planted = nullptr; }
};
}  // namespace appliedwalk

TEST_CASE("W2-P1-39: the two reads of the customisation slots are two walks, not one") {
    using namespace appliedwalk;
    auto plant = [](Scoped& s) {
        for (uint64_t i = 0; i < 20; ++i) {
            s.p.pairs[i].slotKey = 0x100 + i;
            s.p.pairs[i].itemId = 0x9000 + i;
            s.p.state[i].alignmentReq = identity_access::kSlotAlignmentNone;
        }
        s.p.n = 20;
    };

    SUBCASE("both walks agree: the pairs come back and the walk ran twice") {
        Scoped s; plant(s);
        LoadoutPair out[20] = {};
        AppliedSlotState state[20] = {};
        uint64_t n = 0;
        identity_access::Refusal why = identity_access::Refusal::NoNetGame;
        int ng = 0;
        CHECK(identity_access::ReadAppliedPairs(&ng, UserIndex(0), out, n,
                                               nullptr, &why, state));
        CHECK(why == identity_access::Refusal::None);
        CHECK(n == 20);
        CHECK(out[3].itemId == 0x9003u);
        CHECK(s.p.calls == 2);
    }

    SUBCASE("one entry moves between them: refused by name, and nothing readable is published") {
        Scoped s; plant(s);
        s.p.flipItemOnSecond = true;
        LoadoutPair out[20] = {};
        AppliedSlotState state[20] = {};
        uint64_t n = 7;
        identity_access::Refusal why = identity_access::Refusal::None;
        int ng = 0;
        CHECK_FALSE(identity_access::ReadAppliedPairs(&ng, UserIndex(0), out, n,
                                                      nullptr, &why, state));
        CHECK(why == identity_access::Refusal::AppliedSlotsUnstable);
        CHECK(n == 0);
        CHECK(out[3].itemId == 0u);
        CHECK(out[3].slotKey == 0u);
    }

    SUBCASE("the second walk returns fewer entries: also unstable") {
        Scoped s; plant(s);
        s.p.shortenOnSecond = true;
        LoadoutPair out[20] = {};
        AppliedSlotState state[20] = {};
        uint64_t n = 0;
        identity_access::Refusal why = identity_access::Refusal::None;
        int ng = 0;
        CHECK_FALSE(identity_access::ReadAppliedPairs(&ng, UserIndex(0), out, n,
                                                      nullptr, &why, state));
        CHECK(why == identity_access::Refusal::AppliedSlotsUnstable);
    }

    SUBCASE("the second walk cannot resolve: could-not-ask, never an empty component") {
        Scoped s; plant(s);
        s.p.failSecond = true;
        LoadoutPair out[20] = {};
        AppliedSlotState state[20] = {};
        uint64_t n = 0;
        identity_access::Refusal why = identity_access::Refusal::None;
        int ng = 0;
        CHECK_FALSE(identity_access::ReadAppliedPairs(&ng, UserIndex(0), out, n,
                                                      nullptr, &why, state));
        CHECK(why == identity_access::Refusal::NoActorComponent);
    }

    SUBCASE("a caller that wants no state gets ONE walk: the second read is the attribution's") {
        Scoped s; plant(s);
        LoadoutPair out[20] = {};
        uint64_t n = 0;
        int ng = 0;
        CHECK(identity_access::ReadAppliedPairs(&ng, UserIndex(0), out, n));
        CHECK(s.p.calls == 1);
    }
}

// -------------------------------------------------------------------------------------
// AUD-W2-P1-45 -- the loadout-apply gate reader's OWN reads, driven against a synthetic
// netgame with no engine mapped.  These are the must-fail controls the audit found absent
// (AUD-W2-P1-34 sec P1-42 F1) plus sec B's queue reading and the gate-(d) verdict.
// -------------------------------------------------------------------------------------
namespace ia45 = replay::identity_access;
namespace if45 = replay::identity_facts;

TEST_CASE("W2-P1-45 F1: ReadLoadoutApplyGates reads the live instance and pair counts") {
    // LiveLoadout indexes CR15NetGame + LOADOUT_TABLE_OFF + slot<<ENTRY_SHIFT, so a buffer that
    // reaches that far and reads back is a netgame as far as these two reads are concerned.
    const uint64_t slot = 0;
    const size_t hdrOff = size_t(if45::kLOADOUT_TABLE_OFF) + (slot << if45::kLOADOUT_ENTRY_SHIFT);
    std::vector<uint8_t> ng(hdrOff + sizeof(ia45::LoadoutHeader) + 64, 0);
    auto* hdr = reinterpret_cast<ia45::LoadoutHeader*>(ng.data() + hdrOff);
    ia45::LoadoutInstance inst{};
    inst.itemCount = 20;
    const Symbol64 kName = 0xABCDEF01ull;
    inst.name = kName;
    hdr->instances = &inst;
    hdr->instanceCount = 1;

    ia45::LoadoutApplyGates g{};
    // No engine is mapped in the selftest (BuildMatches() is false off a real image), so the three
    // actor-side gates cannot be asked and the verdict is Unasked -- but the two live reads BEFORE
    // them must have run.  GATES_ALWAYS_READY short-circuits to Ready with NOTHING read.
    const ia45::LoadoutApplyGate v =
        ia45::ReadLoadoutApplyGates(ng.data(), UserSlot(uint16_t(slot)), UserIndex(0), kName, g);
    CHECK(g.instances == 1);
    CHECK(g.pairs == 20);
    CHECK(v == ia45::LoadoutApplyGate::Unasked);

    SUBCASE("a name that matches no instance leaves pairs unread") {
        ia45::LoadoutApplyGates g2{};
        ia45::ReadLoadoutApplyGates(ng.data(), UserSlot(uint16_t(slot)), UserIndex(0),
                                    kName ^ 0xFFull, g2);
        CHECK(g2.instances == 1);
        CHECK(g2.pairs == ia45::kLoadoutPairsUnknown);
    }
    SUBCASE("no session, or an unresolved instance name, is Unasked with nothing read") {
        ia45::LoadoutApplyGates g3{};
        CHECK(ia45::ReadLoadoutApplyGates(nullptr, UserSlot(0), UserIndex(0), kName, g3)
              == ia45::LoadoutApplyGate::Unasked);
        CHECK(g3.instances == ia45::kLoadoutInstancesUnreadable);
        ia45::LoadoutApplyGates g4{};
        CHECK(ia45::ReadLoadoutApplyGates(ng.data(), UserSlot(uint16_t(slot)), UserIndex(0), 0, g4)
              == ia45::LoadoutApplyGate::Unasked);
        CHECK(g4.instances == ia45::kLoadoutInstancesUnreadable);
    }
}

// -------------------------------------------------------------------------------------
// ENG-DRESS-LOADOUT (DIAG-SEAT-DRIVE core fix) -- the name-align that makes the engine's own
// enable-time apply find the populated instance.  The engine selects the instance whose +0 name
// equals SymbolFromName("unified"); the parser names ours after the profile's own child, so a
// recording that did not name it "unified" lands its items under a symbol the engine never probes.
// -------------------------------------------------------------------------------------
TEST_CASE("AlignPublishedInstanceName aliases the populated instance to the engine name") {
    const Symbol64 kUnified = 0x1111222233334444ull;   // stands in for SymbolFromName("unified")
    const Symbol64 kOther   = 0xAAAABBBBCCCCDDDDull;

    SUBCASE("one populated instance under a different name is aliased onto the engine name") {
        ia45::LoadoutInstance inst{};
        inst.name = kOther;
        inst.itemCount = 20;
        ia45::LoadoutHeader hdr{};
        hdr.instances = &inst;
        hdr.instanceCount = 1;
        CHECK(ia45::FindInstance(&hdr, kUnified) == nullptr);   // the engine would not find it before
        CHECK(ia45::AlignPublishedInstanceName(&hdr, kUnified));
        CHECK(inst.name == kUnified);                           // renamed in place
        ia45::LoadoutInstance* found = ia45::FindInstance(&hdr, kUnified);
        REQUIRE(found != nullptr);
        CHECK(found->itemCount == 20);                          // and it is non-empty
    }
    SUBCASE("an instance already carrying the engine name and items is left untouched") {
        ia45::LoadoutInstance inst{};
        inst.name = kUnified;
        inst.itemCount = 20;
        ia45::LoadoutHeader hdr{};
        hdr.instances = &inst;
        hdr.instanceCount = 1;
        CHECK(ia45::AlignPublishedInstanceName(&hdr, kUnified));
        CHECK(inst.name == kUnified);
    }
    SUBCASE("two populated instances: it refuses to guess and renames nothing") {
        ia45::LoadoutInstance insts[2]{};
        insts[0].name = kOther;       insts[0].itemCount = 5;
        insts[1].name = kOther ^ 1u;  insts[1].itemCount = 7;
        ia45::LoadoutHeader hdr{};
        hdr.instances = insts;
        hdr.instanceCount = 2;
        CHECK_FALSE(ia45::AlignPublishedInstanceName(&hdr, kUnified));
        CHECK(insts[0].name == kOther);
        CHECK(insts[1].name == (kOther ^ 1u));
    }
    SUBCASE("an empty instance already carrying the engine name is not collided into") {
        ia45::LoadoutInstance insts[2]{};
        insts[0].name = kUnified;  insts[0].itemCount = 0;   // empty, already the engine name
        insts[1].name = kOther;    insts[1].itemCount = 9;   // populated, different name
        ia45::LoadoutHeader hdr{};
        hdr.instances = insts;
        hdr.instanceCount = 2;
        CHECK_FALSE(ia45::AlignPublishedInstanceName(&hdr, kUnified));
        CHECK(insts[1].name == kOther);   // NOT renamed -> the engine's first-match walk is not split
    }
    SUBCASE("nothing populated, a null vector, a zero name or a null container is a no-op false") {
        ia45::LoadoutInstance inst{};
        inst.name = kOther; inst.itemCount = 0;
        ia45::LoadoutHeader hdr{};
        hdr.instances = &inst; hdr.instanceCount = 1;
        CHECK_FALSE(ia45::AlignPublishedInstanceName(&hdr, kUnified));   // nothing populated
        ia45::LoadoutHeader empty{};                                     // instances == null
        CHECK_FALSE(ia45::AlignPublishedInstanceName(&empty, kUnified));
        CHECK_FALSE(ia45::AlignPublishedInstanceName(&hdr, 0));          // zero engine name
        CHECK_FALSE(ia45::AlignPublishedInstanceName(nullptr, kUnified));// null container
    }
}

TEST_CASE("W2-P1-45 sec B: ReadApplyQueueState reads the deferred-queue inline flag and depth") {
    std::vector<uint8_t> subsys(0x400, 0);
    *reinterpret_cast<uint32_t*>(subsys.data() + if45::kDEFERRED_QUEUE_INLINE_FLAG_OFF) = 1u;
    *reinterpret_cast<int64_t*>(subsys.data() + if45::kDEFERRED_QUEUE_HEAD_OFF) =
        int64_t(3) * int64_t(if45::kDEFERRED_QUEUE_ENTRY_STRIDE);
    *reinterpret_cast<int64_t*>(subsys.data() + if45::kDEFERRED_QUEUE_CAP_OFF) =
        int64_t(64) * int64_t(if45::kDEFERRED_QUEUE_ENTRY_STRIDE);
    std::vector<uint8_t> cs(0x100, 0);
    *reinterpret_cast<void**>(cs.data() + if45::kAPPLY_QUEUE_SUBSYS_OFF) = subsys.data();

    ia45::LoadoutApplyGates g{};
    CHECK(ia45::ReadApplyQueueState(cs.data(), g));   // CENSUS_INLINE_UNREAD makes this a no-op
    CHECK(g.queueAsked);
    CHECK(g.inlineApply == 1u);
    CHECK(g.queueDepth == 3);
    CHECK(g.queueCap == 64);

    SUBCASE("a deferred entrant reads inline=0 with the parked count") {
        *reinterpret_cast<uint32_t*>(subsys.data() + if45::kDEFERRED_QUEUE_INLINE_FLAG_OFF) = 0u;
        ia45::LoadoutApplyGates g2{};
        CHECK(ia45::ReadApplyQueueState(cs.data(), g2));
        CHECK(g2.inlineApply == 0u);
        CHECK(g2.queueDepth == 3);
    }
    SUBCASE("a null component system does not ask") {
        ia45::LoadoutApplyGates g3{};
        CHECK_FALSE(ia45::ReadApplyQueueState(nullptr, g3));
        CHECK_FALSE(g3.queueAsked);
    }
}

TEST_CASE("W2-P1-45 gate (d): zero item pairs is InstanceEmpty, else Ready") {
    CHECK(ia45::LoadoutVerdictFromPairs(0) == ia45::LoadoutApplyGate::InstanceEmpty);
    CHECK(ia45::LoadoutVerdictFromPairs(1) == ia45::LoadoutApplyGate::Ready);
    CHECK(ia45::LoadoutVerdictFromPairs(20) == ia45::LoadoutApplyGate::Ready);
}

namespace p145f2 {
    static uint8_t gSeen[16];
    static bool    gCanonRan;
    static char CanonFake(void*, void* guid) {
        gCanonRan = true;
        static_cast<uint8_t*>(guid)[0] = 0xCA;   // the canonical marker, written into the COPY
        return 1;
    }
    static void* BuildFake(void* refp, void*, const void* guid, int64_t) {
        memcpy(gSeen, guid, 16);
        static_cast<ia45::ActorComponentRef*>(refp)->componentSystem = reinterpret_cast<void*>(0x1);
        return nullptr;
    }
}

TEST_CASE("W2-P1-45 F2: BuildRefFromGuidCopy canonicalises a COPY before the build sees the guid") {
    uint8_t live[16];
    memset(live, 0x11, sizeof live);
    p145f2::gCanonRan = false;
    memset(p145f2::gSeen, 0, sizeof p145f2::gSeen);
    ia45::ActorComponentRef out{};
    const bool ok = ia45::BuildRefFromGuidCopy(&p145f2::BuildFake, &p145f2::CanonFake,
                                               nullptr, live, out);
    CHECK(ok);
    // The build must have seen the CANONICALISED guid (SKIP_GUID_REWRITE leaves the raw 0x11)...
    CHECK(p145f2::gSeen[0] == 0xCA);
    CHECK(p145f2::gCanonRan);
    // ...and the engine's live field must be untouched -- the canon ran over the copy, not it.
    CHECK(live[0] == 0x11);
}

// =========================================================================================
// ENG-MODEL-PUMP FIX A: the model-swap completion pump.  The plugin's flush must (a) skip an empty
// queue and (b), while a swap is enqueued (pending > 0), invoke the swapper CS's own 1-arg driver on
// the RIGHT swapper CS so the driver clears the entrant's in-flight flag -- the completion the offline
// scheduler never runs.  Driven with no image: a stand-in swapper CS + a stand-in driver that models
// sub_14044A0C0's drain-and-clear.  FAIL-before (empty queue leaves in-flight set) / PASS-after (a
// pending swap gets pumped and in-flight clears).
// =========================================================================================
namespace swappump {
namespace iaf = replay::identity_facts;

// A stand-in CR15NetPlayerModelSwapperCS laid out at the plugin's ratified offsets.
struct FakeSwapper {
    alignas(16) uint8_t cs[0x200] = {};
    uint16_t handleTable[8] = {};
    alignas(16) uint8_t records[4 * iaf::kSWAPPER_RECORD_STRIDE] = {};

    // The model-instance chain the current-model walk (sub_1404B5D90) traverses,
    // from a record's +408 instance ptr / +416 key down to the +0xB10 current-model qword.
    alignas(16) uint8_t inst[0x200]    = {};
    uint16_t            idxTable[8]     = {};
    alignas(16) uint8_t pool[0x100]     = {};
    alignas(16) uint8_t slotObj[0xB18] = {};   // holds the current-model qword at +0xB10

    void Wire() {
        void* t = handleTable;
        void* r = records;
        memcpy(cs + iaf::kSWAPPER_HANDLE_TABLE_OFF, &t, sizeof t);
        memcpy(cs + iaf::kSWAPPER_RECORD_BASE_OFF, &r, sizeof r);
    }
    void SetPending(uint64_t n) { memcpy(cs + iaf::kSWAPPER_PENDING_COUNT_OFF, &n, sizeof n); }
    uint64_t Pending() const {
        uint64_t n = 0; memcpy(&n, cs + iaf::kSWAPPER_PENDING_COUNT_OFF, sizeof n); return n;
    }
    // The record for a user index: handle table[index] -> record base + stride*handle.
    uint32_t* InFlight(uint16_t index) {
        const uint16_t handle = handleTable[index];
        return reinterpret_cast<uint32_t*>(records + size_t(iaf::kSWAPPER_RECORD_STRIDE) * handle +
                                           iaf::kSWAPPER_INFLIGHT_OFF);
    }
    // Wire the full current-model walk for a user index (key/slotIndex fixed at 0, a single-entry
    // chain) and return the current-model qword it resolves to -- the exact field the plugin walks.
    uint64_t* WireCurrent(uint16_t index) {
        const uint16_t handle = handleTable[index];
        uint8_t* rec = records + size_t(iaf::kSWAPPER_RECORD_STRIDE) * handle;
        void* ip = inst;   memcpy(rec + iaf::kSWAPPER_MODELINST_OFF, &ip, sizeof ip);
        uint32_t key = 0;  memcpy(rec + iaf::kSWAPPER_MODELKEY_OFF, &key, sizeof key);
        void* it = idxTable;
        memcpy(inst + iaf::kSWAPPER_INST_INDEX_TABLE_OFF, &it, sizeof it);
        idxTable[0] = 0;   // slotIndex
        void* pp = pool;
        memcpy(inst + iaf::kSWAPPER_INST_POOL_BASE_OFF, &pp, sizeof pp);
        void* so = slotObj;   // pool + 16*slotIndex(0) + 8
        memcpy(pool + iaf::kSWAPPER_INST_SLOT_PTR_OFF, &so, sizeof so);
        return reinterpret_cast<uint64_t*>(slotObj + iaf::kSWAPPER_CURRENT_MODEL_OFF);
    }
};

FakeSwapper* g_fake = nullptr;
uint16_t     g_index = 0;

// The stand-in driver: models sub_14044A0C0 -- while the queue is non-empty it lands one swap, which
// clears that record's in-flight flag and drops the pending count.
void FakePump(void* cs) {
    (void)cs;
    if (g_fake == nullptr || g_fake->Pending() == 0) return;
    *g_fake->InFlight(g_index) = 0;
    g_fake->SetPending(g_fake->Pending() - 1);
}
}  // namespace swappump

TEST_CASE("ENG-MODEL-PUMP FIX A: a pending swap is pumped to completion; an empty queue is not") {
    using namespace swappump;
    FakeSwapper fake;
    fake.Wire();
    g_fake  = &fake;
    g_index = 1;
    fake.handleTable[1] = 0;   // entrant index 1 -> swap record 0
    void* dummyNg = &fake;     // ignored: the injected CS is returned directly

    SUBCASE("a pending, in-flight swap gets pumped and its in-flight flag clears") {
        fake.SetPending(1);
        *fake.InFlight(1) = 1;   // enqueued and marked in-flight, exactly as BeginModelSwap leaves it
        replay::identity_access::SwapperInjectForTest(fake.cs, &FakePump);

        // FAIL-before: nothing has pumped it, so it is still stuck in-flight.
        CHECK(*fake.InFlight(1) == 1u);

        const auto c = replay::identity_access::SwapperFlushForTest(dummyNg);
        CHECK(c.resolved);
        CHECK(c.pumped);
        CHECK(c.pendingBefore == 1u);
        CHECK(c.pendingAfter == 0u);
        // PASS-after: the driver ran and landed the swap.
        CHECK(*fake.InFlight(1) == 0u);

        replay::identity_access::SwapperClearInjectForTest();
    }

    SUBCASE("the detour body pumps in a match and leaves the engine alone out of one") {
        fake.SetPending(1);
        *fake.InFlight(1) = 1;
        replay::identity_access::SwapperInjectForTest(fake.cs, &FakePump);
        uint8_t ng[64] = {};
        const uint32_t loadingLevel = facts::NETGAME_STATE_INMATCH - 2;
        memcpy(ng, &loadingLevel, sizeof loadingLevel);
        replay::identity_access::ReplyDrainNetGameUpdateForTest(ng, replay::tick::kTickStage);
        CHECK(*fake.InFlight(1) == 1u);   // a swap left enqueued when the level unloads stays put
        CHECK(fake.Pending() == 1u);

        const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
        memcpy(ng, &inMatch, sizeof inMatch);
        replay::identity_access::ReplyDrainNetGameUpdateForTest(ng, replay::tick::kTickStage + 1);
        CHECK(*fake.InFlight(1) == 1u);   // not the tick stage
        replay::identity_access::ReplyDrainNetGameUpdateForTest(ng, replay::tick::kTickStage);
        CHECK(*fake.InFlight(1) == 0u);
        CHECK(fake.Pending() == 0u);

        replay::identity_access::SwapperClearInjectForTest();
    }

    SUBCASE("an empty queue is skipped, so no unrelated driver call fires") {
        fake.SetPending(0);
        *fake.InFlight(1) = 1;   // an in-flight record with an empty queue -- nothing to drain
        replay::identity_access::SwapperInjectForTest(fake.cs, &FakePump);

        const auto c = replay::identity_access::SwapperFlushForTest(dummyNg);
        CHECK(c.resolved);
        CHECK_FALSE(c.pumped);          // the gate held
        CHECK(*fake.InFlight(1) == 1u); // untouched

        replay::identity_access::SwapperClearInjectForTest();
    }

    SUBCASE("the per-entrant reader sees the in-flight flag and pending depth") {
        fake.SetPending(1);
        *fake.InFlight(1) = 1;
        replay::identity_access::SwapperInjectForTest(fake.cs, &FakePump);

        replay::identity_access::SwapperEntrantState st;
        REQUIRE(replay::identity_access::ReadSwapperEntrant(dummyNg, 1, st));
        CHECK(st.resolved);
        CHECK(st.handleRead);
        CHECK(st.inFlight == 1u);
        CHECK(st.pending == 1u);

        replay::identity_access::SwapperClearInjectForTest();
    }

    g_fake = nullptr;
    replay::identity_access::SwapperClearInjectForTest();
}

// The pooled-actor from==to fix.  A pooled record carries the prior occupant's
// hash as "current" == the target, so the engine's swap dispatcher (sub_140CB0370) reads current ==
// target and takes the immediate-complete NO-MESH arm, enqueuing nothing.  ResetSwapperCurrent forces
// current to the -1 no-model sentinel so the compare instead takes the enqueue (!=) arm.  Driven with
// no image: a stand-in swapper CS whose full model-instance walk (rec+408 -> +0xB10) resolves to a
// real qword, plus a stand-in dispatcher that models the arm choice.  FAIL-before (current==target ->
// no enqueue) / PASS-after (current==-1 -> enqueue).
TEST_CASE("Pooled current==target is reset to -1 so the swap takes the enqueue arm") {
    using namespace swappump;
    FakeSwapper fake;
    fake.Wire();
    fake.handleTable[1] = 0;               // entrant index 1 -> swap record 0
    uint64_t* cur = fake.WireCurrent(1);   // the exact current-model qword the plugin walks to
    void* dummyNg = &fake;                 // ignored: the injected CS is returned directly

    const uint64_t target   = 0xDAC6537A23236325ull;   // the recorded chassis model (the from==to hash)
    const uint64_t kSentinel = ~uint64_t(0);            // -1, the no-model value the != arm needs
    *cur = target;                                       // pooled: current already equals the target

    replay::identity_access::SwapperInjectForTest(fake.cs, nullptr);

    // The stand-in dispatcher: sub_140CB0370 enqueues (takes the mesh-building != arm) iff current !=
    // target.  This is the exact `if (*v11 == v18)` branch the reset re-aims.
    auto enqueues = [&](uint64_t tgt) { return *cur != tgt; };

    // The reader sees the pooled hash as the desired-model (A) before the reset.
    replay::identity_access::SwapperEntrantState pre;
    REQUIRE(replay::identity_access::ReadSwapperEntrant(dummyNg, 1, pre));
    CHECK(pre.currentRead);
    CHECK(pre.currentModel == target);

    // FAIL-before: current == target, so the dispatcher takes the no-mesh arm and enqueues nothing.
    CHECK(*cur == target);
    CHECK_FALSE(enqueues(target));

    const auto o = replay::identity_access::ResetSwapperCurrent(dummyNg, 1);
    CHECK(o.resolved);
    CHECK(o.located);
    CHECK(o.wrote);
    CHECK_FALSE(o.faulted);
    CHECK(o.before == target);
    CHECK(o.after == kSentinel);

    // PASS-after: current == -1 != target, so the dispatcher now takes the enqueue (mesh-building) arm.
    CHECK(*cur == kSentinel);
    CHECK(enqueues(target));

    // And the reader reflects the reset value it walked to.
    replay::identity_access::SwapperEntrantState post;
    REQUIRE(replay::identity_access::ReadSwapperEntrant(dummyNg, 1, post));
    CHECK(post.currentRead);
    CHECK(post.currentModel == kSentinel);

    // NO CHURN: the pre-apply gate now reads current==-1, so the next sweep would not re-write.
    CHECK_FALSE(replay::identity_access::SwapperResetShouldFire(/*meshPresent=*/false, post));

    replay::identity_access::SwapperClearInjectForTest();
}

// The pre-apply gate fires exactly once, in the window before the engine's Apply
// Chassis, and stops on every later state -- so the swap re-arms once and a built mesh never churns.
TEST_CASE("The pre-apply gate fires once then stops on enqueue and on a built mesh") {
    const uint64_t real = 0xDAC6537A23236325ull;
    const uint64_t kSentinel = ~uint64_t(0);

    replay::identity_access::SwapperEntrantState sw;
    sw.resolved     = true;
    sw.currentRead  = true;
    sw.currentModel = real;   // a real, stale, pooled hash
    sw.pending      = 0;

    // FIRES: swapper resolved, mesh absent, real stale current, nothing enqueued.
    CHECK(replay::identity_access::SwapperResetShouldFire(/*meshPresent=*/false, sw));

    // STOPS immediately after the write makes current == -1 (no churn while it waits for the apply).
    sw.currentModel = kSentinel;
    CHECK_FALSE(replay::identity_access::SwapperResetShouldFire(false, sw));

    // STOPS once the swap is enqueued (pending > 0), even with a real current again.
    sw.currentModel = real;
    sw.pending      = 1;
    CHECK_FALSE(replay::identity_access::SwapperResetShouldFire(false, sw));

    // STOPS once the mesh is built (present), even at pending==0 with the real (built) current.
    sw.pending = 0;
    CHECK_FALSE(replay::identity_access::SwapperResetShouldFire(/*meshPresent=*/true, sw));

    // STOPS while the swapper record has not resolved for this index yet.
    replay::identity_access::SwapperEntrantState unresolved;
    CHECK_FALSE(replay::identity_access::SwapperResetShouldFire(false, unresolved));
}

// An unresolvable walk (no model-instance wired) refuses without writing.
TEST_CASE("An unreadable walk refuses and writes nothing") {
    using namespace swappump;
    FakeSwapper fake;
    fake.Wire();
    fake.handleTable[1] = 0;   // record 0, but its +408 model-instance ptr is left null (unwired)
    void* dummyNg = &fake;

    replay::identity_access::SwapperInjectForTest(fake.cs, nullptr);
    const auto o = replay::identity_access::ResetSwapperCurrent(dummyNg, 1);
    CHECK(o.resolved);          // the CS resolved
    CHECK_FALSE(o.located);     // but the current-model field did not
    CHECK_FALSE(o.wrote);
    CHECK_FALSE(o.faulted);     // a refusal is not a fault
    replay::identity_access::SwapperClearInjectForTest();
}

// After the sweep has forced current to -1 and it persisted, the from==to no-op has
// already run and built nothing, so the re-trigger calls the engine's OWN BeginModelSwap (sub_14042D120)
// directly with the captured target.  A stand-in models the enqueue arm: current(-1) != target ->
// mark in-flight + bump the pending queue (0 -> 1).  FAIL-before (nothing enqueued) / PASS-after
// (pending == 1), and it STOPS once pending > 0, the mesh is present, or it has already fired -- no
// double-enqueue.
namespace beginswap {
// The stand-in for sub_14042D120's enqueue arm, modelling its own re-checks: the in-flight guard
// (`else if (*(v5+8))` -> no re-enqueue) and the from==to guard (`if (*current == target)` -> no mesh,
// no enqueue).  Only current(-1) != target reaches the enqueue that marks in-flight and bumps pending.
void FakeBegin(void* cs, uint32_t index, uint64_t target) {
    (void)cs;
    using namespace swappump;
    if (g_fake == nullptr) return;
    const uint16_t idx = uint16_t(index);
    uint64_t* cur = g_fake->WireCurrent(idx);
    if (*g_fake->InFlight(idx) == 1u) return;   // already in flight -> no re-enqueue (idempotent)
    if (*cur == target) return;                 // from==to -> immediate-complete, builds nothing
    *g_fake->InFlight(idx) = 1u;                // enqueue arm: rec+8 = 1
    g_fake->SetPending(g_fake->Pending() + 1);  // ++pending
}
}  // namespace beginswap

TEST_CASE("The direct re-trigger enqueues once when current==-1 and stops otherwise") {
    using namespace swappump;
    const uint64_t target    = 0xDAC6537A23236325ull;   // the captured pooled target (== from==to hash)
    const uint64_t kSentinel = ~uint64_t(0);            // -1, current after the sweep's write persisted

    SUBCASE("the gate fires only in the -1-persisted window and never twice") {
        replay::identity_access::SwapperEntrantState sw;
        sw.resolved     = true;
        sw.currentRead  = true;
        sw.currentModel = kSentinel;   // the sweep already forced -1 and it persisted
        sw.pending      = 0;

        // FIRES: -1 persisted, mesh absent, nothing enqueued, a target was captured, not yet issued.
        CHECK(replay::identity_access::BeginSwapShouldFire(false, sw, /*targetKnown=*/true,
                                                           /*alreadyIssued=*/false));

        // STOPS once it has already fired (the once-per-entrant latch) -- no double-enqueue.
        CHECK_FALSE(replay::identity_access::BeginSwapShouldFire(false, sw, true, /*alreadyIssued=*/true));
        // STOPS with no captured target.
        CHECK_FALSE(replay::identity_access::BeginSwapShouldFire(false, sw, /*targetKnown=*/false, false));
        // STOPS once a swap is enqueued (pending > 0).
        sw.pending = 1;
        CHECK_FALSE(replay::identity_access::BeginSwapShouldFire(false, sw, true, false));
        sw.pending = 0;
        // STOPS once the mesh is built.
        CHECK_FALSE(replay::identity_access::BeginSwapShouldFire(/*meshPresent=*/true, sw, true, false));
        // STOPS while current is still a real hash (the sweep's -1 has not persisted yet).
        sw.currentModel = target;
        CHECK_FALSE(replay::identity_access::BeginSwapShouldFire(false, sw, true, false));
        // STOPS while the record has not resolved.
        replay::identity_access::SwapperEntrantState unresolved;
        CHECK_FALSE(replay::identity_access::BeginSwapShouldFire(false, unresolved, true, false));
    }

    SUBCASE("TriggerBeginModelSwap drives the engine fn: pending 0 -> 1, then a second call is inert") {
        FakeSwapper fake;
        fake.Wire();
        g_fake  = &fake;
        g_index = 1;
        fake.handleTable[1] = 0;                // entrant index 1 -> swap record 0
        uint64_t* cur = fake.WireCurrent(1);
        *cur = kSentinel;                        // current already forced to -1 by the sweep
        fake.SetPending(0);
        *fake.InFlight(1) = 0;
        void* dummyNg = &fake;

        replay::identity_access::SwapperInjectForTest(fake.cs, nullptr);
        replay::identity_access::SwapperInjectBeginForTest(&beginswap::FakeBegin);

        // FAIL-before: nothing is enqueued and the record is not in flight.
        CHECK(fake.Pending() == 0u);
        CHECK(*fake.InFlight(1) == 0u);

        const auto o = replay::identity_access::TriggerBeginModelSwap(dummyNg, 1, target);
        CHECK(o.resolved);
        CHECK(o.located);
        CHECK(o.issued);
        CHECK_FALSE(o.faulted);
        CHECK_FALSE(o.reentrant);
        CHECK(o.pendingBefore == 0u);
        CHECK(o.pendingAfter == 1u);

        // PASS-after: current(-1) != target took the enqueue arm -- pending 0 -> 1, record in flight.
        CHECK(fake.Pending() == 1u);
        CHECK(*fake.InFlight(1) == 1u);

        // A second call is inert (the engine fn's own in-flight guard) -- no double-enqueue.
        const auto o2 = replay::identity_access::TriggerBeginModelSwap(dummyNg, 1, target);
        CHECK(o2.issued);                 // the fn ran
        CHECK(fake.Pending() == 1u);      // but the in-flight guard kept it from enqueuing again
        CHECK(*fake.InFlight(1) == 1u);

        g_fake = nullptr;
        replay::identity_access::SwapperClearInjectForTest();
    }
}

// Emulate the engine's own "[NETGAME] Profile received" handler so the ENGINE
// requests the model resource and seats the InstanceModel CI itself via the delegate.  Driven with
// no image: stand-in ctor/parse build a3 and a stand-in handler records the a1/a2/a3 it was handed,
// proving the call shape (a1 = the receiver, a2 = {accountType 5, the id}, a3 = a non-null document)
// by construction -- plus the parse-fail / malformed / null-receiver refusals.
namespace profrecv {
struct Seen {
    bool     called = false;
    void*    ng = nullptr;
    uint64_t acctType = 0;
    uint64_t acctId = 0;
    bool     docNonNull = false;
};
Seen g_seen;
void FakeHandler(void* ng, const void* xplatId, void* doc) {
    g_seen.called = true;
    g_seen.ng = ng;
    const uint64_t* id = static_cast<const uint64_t*>(xplatId);
    g_seen.acctType   = id[0];
    g_seen.acctId     = id[1];
    g_seen.docNonNull = doc != nullptr;
}
void* FakeCtor(void* doc) { return doc; }                          // the engine ctor returns the doc
uint32_t FakeParseOk(void*, const char*, uint64_t) { return 0; }    // 0 == parsed
uint32_t FakeParseFail(void*, const char*, uint64_t) { return 1; }  // nonzero == engine rejected
}  // namespace profrecv

TEST_CASE("The fire gate emulates once, only while undressed with a profile present") {
    using replay::identity_access::EmulateProfileShouldFire;
    // FIRES: not issued, CI absent, profile present.
    CHECK(EmulateProfileShouldFire(/*meshPresent=*/false, /*profilePresent=*/true, /*alreadyIssued=*/false));
    // STOPS once it has fired (once-per-entrant -> the engine seats the CI at most once).
    CHECK_FALSE(EmulateProfileShouldFire(false, true, /*alreadyIssued=*/true));
    // STOPS with no recorded profile to hand the handler.
    CHECK_FALSE(EmulateProfileShouldFire(false, /*profilePresent=*/false, false));
    // STOPS once the CI is seated (the body is dressed).
    CHECK_FALSE(EmulateProfileShouldFire(/*meshPresent=*/true, true, false));
}

TEST_CASE("EmulateProfileReceivedCall hands the handler a1/a2/a3 and refuses cleanly") {
    using namespace replay::identity_access;
    const char* json = "{\"loadout\":{},\"unlocks\":{}}";
    const Blob good{ reinterpret_cast<const uint8_t*>(json), uint32_t(strlen(json)) };
    uint64_t dummyNg = 0;                 // a readable, non-null receiver
    const uint64_t id = 0x1234ABCDEF00ull;

    SUBCASE("the handler runs and is handed {5, id} and a non-null doc") {
        profrecv::g_seen = profrecv::Seen{};
        ProfileReceivedInjectForTest(&profrecv::FakeHandler, &profrecv::FakeCtor, &profrecv::FakeParseOk);
        const ProfileReceivedOutcome o = EmulateProfileReceivedCall(&dummyNg, id, good);
        CHECK(o.issued);
        CHECK_FALSE(o.faulted);
        CHECK_FALSE(o.parseFailed);
        CHECK(o.account == id);
        CHECK(profrecv::g_seen.called);
        CHECK(profrecv::g_seen.ng == &dummyNg);
        CHECK(profrecv::g_seen.acctType == 5u);   // the OVR/remote-arm type SeedLobbyRecordFor writes
        CHECK(profrecv::g_seen.acctId == id);
        CHECK(profrecv::g_seen.docNonNull);
        ProfileReceivedClearInjectForTest();
    }

    SUBCASE("an engine parse failure refuses and never calls the handler") {
        profrecv::g_seen = profrecv::Seen{};
        ProfileReceivedInjectForTest(&profrecv::FakeHandler, &profrecv::FakeCtor, &profrecv::FakeParseFail);
        const ProfileReceivedOutcome o = EmulateProfileReceivedCall(&dummyNg, id, good);
        CHECK(o.parseFailed);
        CHECK_FALSE(o.issued);
        CHECK_FALSE(profrecv::g_seen.called);
        ProfileReceivedClearInjectForTest();
    }

    SUBCASE("a malformed recording is refused before the parser (jsonRejected)") {
        profrecv::g_seen = profrecv::Seen{};
        ProfileReceivedInjectForTest(&profrecv::FakeHandler, &profrecv::FakeCtor, &profrecv::FakeParseOk);
        const char* bad = "not json";
        const Blob junk{ reinterpret_cast<const uint8_t*>(bad), uint32_t(strlen(bad)) };
        const ProfileReceivedOutcome o = EmulateProfileReceivedCall(&dummyNg, id, junk);
        CHECK(o.jsonRejected);
        CHECK_FALSE(o.issued);
        CHECK_FALSE(profrecv::g_seen.called);
        ProfileReceivedClearInjectForTest();
    }

    SUBCASE("a null receiver refuses (receiverBad) and never calls the handler") {
        profrecv::g_seen = profrecv::Seen{};
        ProfileReceivedInjectForTest(&profrecv::FakeHandler, &profrecv::FakeCtor, &profrecv::FakeParseOk);
        const ProfileReceivedOutcome o = EmulateProfileReceivedCall(nullptr, id, good);
        CHECK(o.receiverBad);
        CHECK_FALSE(o.issued);
        CHECK_FALSE(profrecv::g_seen.called);
        ProfileReceivedClearInjectForTest();
    }
}

// =============================================================================================
// The NETGAME.UPDATE stage-3 tail: a registered sweep runs on stage 3 only, with the detour's own
// netGame, and not at all once cleared. (The rules+0xBC clock write that used to live here is retired:
// nothing renders that field. tests/playback covers the HUD-netvar writer that replaced it.)
namespace {
struct StageSweepProbe {
    int   calls = 0;
    void* lastNetGame = nullptr;
    static void Sweep(void* ctx, void* ng) {
        auto* p = static_cast<StageSweepProbe*>(ctx);
        ++p->calls;
        p->lastNetGame = ng;
    }
};
}  // namespace

TEST_CASE("stage-3 tail: a registered sweep runs on the last stage only, and never once cleared") {
    StageSweepProbe probe;
    int netGame = 0;

    identity_access::GameStateStageSweepForTest(&netGame, 3);   // nothing registered
    CHECK(probe.calls == 0);

    identity_access::RegisterGameStateStageSweep(&StageSweepProbe::Sweep, &probe);
    for (uint64_t stage = 0; stage < 3; ++stage) {
        identity_access::GameStateStageSweepForTest(&netGame, stage);
    }
    CHECK(probe.calls == 0);
    identity_access::GameStateStageSweepForTest(&netGame, 3);
    CHECK(probe.calls == 1);
    CHECK(probe.lastNetGame == static_cast<void*>(&netGame));

    identity_access::ClearGameStateStageSweep();
    identity_access::GameStateStageSweepForTest(&netGame, 3);
    CHECK(probe.calls == 1);
}

// DISC-FLIGHT defect 2. Unload suspends the reply-drain BODY and only then queues the entrants'
// leave -- and the leave is drained by the stage-3 tail this detour runs. Gating the tail on the
// same "a replay is loaded" flag as the body therefore made the hook swallow the plugin's own
// removals: the rails stayed full, and the next load re-bound to three residue slots.
TEST_CASE("reply-drain gate: the stage-3 tail still runs after Unload has disarmed the body") {
    StageSweepProbe probe;
    int netGame = 0;
    const bool wasArmed = identity_access::ReplyDrainBodyArmedForTest();
    identity_access::RegisterGameStateStageSweep(&StageSweepProbe::Sweep, &probe);

    livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);

    identity_access::ReplyDrainSetBodyArmedForTest(true);
    identity_access::ReplyDrainGatedForTest(&netGame, 3);
    CHECK(probe.calls == 1);

    // ★ The state Unload leaves behind: no replay loaded, the body suspended -- and the leave feed
    // still to drain. The tail must run.
    identity_access::ReplyDrainSetBodyArmedForTest(false);
    identity_access::ReplyDrainGatedForTest(&netGame, 3);
    CHECK(probe.calls == 2);

    // ...and containment, not loadedness, is what stops it.
    livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
    identity_access::ReplyDrainGatedForTest(&netGame, 3);
    CHECK(probe.calls == 2);
    identity_access::ReplyDrainSetBodyArmedForTest(true);
    identity_access::ReplyDrainGatedForTest(&netGame, 3);
    CHECK(probe.calls == 2);

    livesafety::ResetForTest();
    identity_access::ReplyDrainSetBodyArmedForTest(wasArmed);
    identity_access::ClearGameStateStageSweep();
}

// The other half of the same defect: the driver's only install site runs once per session, so the
// suspend Unload makes had no partner and a SECOND load left the detour passing straight through --
// the reply route never published and every entrant of the reload stayed at ReplyWrongPhase.
TEST_CASE("reply-drain body: Unload suspends it and the next Load resumes it") {
    const bool wasArmed = identity_access::ReplyDrainBodyArmedForTest();
    identity_access::ReplyDrainInstallForTest(true);
    identity_access::ReplyDrainSetBodyArmedForTest(true);

    identity_access::SuspendReplyDrainBody();
    CHECK_FALSE(identity_access::ReplyDrainBodyArmedForTest());
    CHECK(identity_access::ResumeReplyDrainBody());
    CHECK(identity_access::ReplyDrainBodyArmedForTest());
    // Idempotent: a load that never unloaded re-arms nothing and still reports armed.
    CHECK(identity_access::ResumeReplyDrainBody());
    CHECK(identity_access::ReplyDrainBodyArmedForTest());

    // A driver that never installed stays uninstalled: resume is not a hook site.
    identity_access::ReplyDrainInstallForTest(false);
    identity_access::SuspendReplyDrainBody();
    CHECK_FALSE(identity_access::ResumeReplyDrainBody());
    CHECK_FALSE(identity_access::ReplyDrainBodyArmedForTest());

    identity_access::ReplyDrainSetBodyArmedForTest(wasArmed);
}

// ENG-EQUIPMENT-RECV (DIAG-MODEL-COMPONENT-LEVER).  Drive the engine's own R15NetPlayerEquipmentUpdate
// RECV with a stand-in handler + stand-in equipment CS (no image mapped), and prove the fabricated
// payload carries the entrant's SR15NetUserId at +0 and the RECORDED chassis at +8 -- and that a wrong
// owning netgame or an unvalidated id refuses WITHOUT driving the handler.  This is the discriminating
// test: armed -> the RECV is fabricated and driven for the entrant; the gate off -> it is not.
namespace {
struct EquipCapture {
    int      calls   = 0;
    void*    cs      = nullptr;
    uint64_t a0      = 1, a1 = 1;   // seeded non-zero so a body that leaves them is proven to write 0
    uint32_t entityId = 0;
    uint64_t chassis  = 0;
    uint64_t equipA   = 1, equipB = 1;
    uint32_t equipC   = 1;
};
EquipCapture g_equipCap;
void EquipCaptureHandler(void* cs, void* msg) {
    g_equipCap.calls++;
    g_equipCap.cs = cs;
    const uint64_t* m = static_cast<const uint64_t*>(msg);
    g_equipCap.a0 = m[0];
    g_equipCap.a1 = m[1];
    const uint8_t* body = reinterpret_cast<const uint8_t*>(m[2]);
    memcpy(&g_equipCap.entityId, body + 0, 4);
    memcpy(&g_equipCap.chassis, body + 8, 8);
    memcpy(&g_equipCap.equipA, body + 16, 8);
    memcpy(&g_equipCap.equipB, body + 24, 8);
    memcpy(&g_equipCap.equipC, body + 32, 4);
}

// A fake CR15NetGame with the fields DriveEquipmentRecv reads: the CS-space pointer (so spaceResolved
// passes), the index->slot side table SlotOfIndex walks (USER_ARRAY_OFF, one packed word per entrant),
// the entrant count SlotOfIndex bounds against, and the SLOT-keyed SLOT/GEN tables NetUserId reads.
struct FakeEquipNg {
    std::vector<uint8_t> ng   = std::vector<uint8_t>(facts::NETGAME_CS_SPACE_OFF + 0x10, 0);
    std::vector<uint8_t> inner = std::vector<uint8_t>(replay::identity_facts::kEQUIP_NETGAME_BACKREF_OFF + 0x10, 0);
    std::vector<uint8_t> ctx  = std::vector<uint8_t>(0x20, 0);
    std::vector<uint8_t> cs   = std::vector<uint8_t>(0x100, 0);
    void* Ng() { return ng.data(); }
    // Wire entrant `index` to live `slot` with generation `genWord`.  The drive is handed the UserIndex
    // and must resolve it to the SLOT through the side table before reading the slot-keyed id tables, so
    // the fixture wires BOTH: index->slot at USER_ARRAY_OFF (slot in the USER_SLOT_MASK bits), and the
    // slot->gen row at IDENT_GEN_TABLE_OFF.  slot may differ from index (the multi-remote case).
    void SetUser(uint16_t index, uint16_t slot, uint16_t genWord) {
        void* dummySpace = reinterpret_cast<void*>(uintptr_t(1));
        memcpy(ng.data() + facts::NETGAME_CS_SPACE_OFF, &dummySpace, sizeof(void*));
        const uint16_t count = uint16_t(facts::ENTITY_USER_SLOT_TABLE_MAX);
        memcpy(ng.data() + facts::IDENT_ENTRANT_COUNT_OFF, &count, 2);
        uint16_t word = 0;
        const size_t userOff = size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * index;
        memcpy(&word, ng.data() + userOff, 2);
        word = uint16_t((word & ~uint16_t(facts::IDENT_USER_SLOT_MASK)) |
                        (slot & uint16_t(facts::IDENT_USER_SLOT_MASK)));
        memcpy(ng.data() + userOff, &word, 2);
        const size_t base = size_t(4) * slot;
        memcpy(ng.data() + facts::IDENT_SLOT_TABLE_OFF + base, &slot, 2);
        memcpy(ng.data() + facts::IDENT_GEN_TABLE_OFF + base, &genWord, 2);
    }
    // Wire cs+0x80 -> ctx, ctx+8 -> inner, inner+BACKREF -> back (the owning netgame the RECV confirms).
    void LinkBackref(void* back) {
        void* innerP = inner.data();
        memcpy(inner.data() + replay::identity_facts::kEQUIP_NETGAME_BACKREF_OFF, &back, sizeof(void*));
        memcpy(ctx.data() + replay::identity_facts::kEQUIP_NETGAME_CTX2_OFF, &innerP, sizeof(void*));
        void* ctxP = ctx.data();
        memcpy(cs.data() + replay::identity_facts::kEQUIP_NETGAME_CTX_OFF, &ctxP, sizeof(void*));
    }
};
}  // namespace

TEST_CASE("The RECV is fabricated with the entrant's id and the recorded chassis") {
    const uint64_t kChassis = 0xCA4CC178F3707385ull;   // the recorded chassis the run logged
    FakeEquipNg f;
    f.SetUser(/*index*/ 1, /*slot*/ 1, /*generation*/ 7);   // shipped single-player fixture: slot == index

    SUBCASE("armed + owning netgame confirmed -> driven, payload carries id and chassis") {
        f.LinkBackref(f.Ng());   // back-ref resolves to OUR ng
        g_equipCap = EquipCapture{};
        identity_access::EquipmentRecvInjectForTest(f.cs.data(), &EquipCaptureHandler);
        const auto o = identity_access::DriveEquipmentRecv(f.Ng(), /*index*/ 1, kChassis);
        identity_access::EquipmentRecvClearInjectForTest();

        CHECK(o.spaceResolved);
        CHECK(o.equipResolved);
        CHECK(o.idResolved);
        CHECK(o.netgameConfirmed);
        CHECK_FALSE(o.netgameContradicted);
        CHECK(o.handlerBound);
        CHECK(o.issued);
        CHECK_FALSE(o.faulted);
        // +0 == NetUserId(slot 1, gen 7) == slot | (gen<<16)
        CHECK(o.entityId == 0x00070001u);
        CHECK(o.chassis == kChassis);
        // The handler ran exactly once, on OUR cs, with the fabricated body.
        CHECK(g_equipCap.calls == 1);
        CHECK(g_equipCap.cs == f.cs.data());
        CHECK(g_equipCap.entityId == 0x00070001u);
        CHECK(g_equipCap.chassis == kChassis);
        // a2[0]/a2[1] and the three equip fields are left 0 so ONLY the chassis change fires.
        CHECK(g_equipCap.a0 == 0);
        CHECK(g_equipCap.a1 == 0);
        CHECK(g_equipCap.equipA == 0);
        CHECK(g_equipCap.equipB == 0);
        CHECK(g_equipCap.equipC == 0);
    }

    SUBCASE("a wrong owning netgame refuses WITHOUT driving the handler") {
        std::vector<uint8_t> other(0x10, 0);
        f.LinkBackref(other.data());   // back-ref resolves to a DIFFERENT object
        g_equipCap = EquipCapture{};
        identity_access::EquipmentRecvInjectForTest(f.cs.data(), &EquipCaptureHandler);
        const auto o = identity_access::DriveEquipmentRecv(f.Ng(), 1, kChassis);
        identity_access::EquipmentRecvClearInjectForTest();

        CHECK(o.equipResolved);
        CHECK(o.netgameContradicted);
        CHECK_FALSE(o.netgameConfirmed);
        CHECK_FALSE(o.issued);
        CHECK(g_equipCap.calls == 0);   // the handler was never driven on the wrong object
    }

    SUBCASE("an unvalidated entrant id refuses (idResolved false), handler not driven") {
        f.LinkBackref(f.Ng());
        // Entrant at UserIndex 2 lives in live slot 2, but slot 2 has no valid slot/gen words (the
        // zeroed table reads slot word 0, which is valid, so poison it to the engine's invalid
        // sentinel): NetUserId must refuse the id after SlotOfIndex resolves index 2 -> slot 2.
        f.SetUser(/*index*/ 2, /*slot*/ 2, /*generation*/ 0);
        uint16_t invalid = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        memcpy(f.ng.data() + facts::IDENT_SLOT_TABLE_OFF + 4 * 2, &invalid, 2);
        g_equipCap = EquipCapture{};
        identity_access::EquipmentRecvInjectForTest(f.cs.data(), &EquipCaptureHandler);
        const auto o = identity_access::DriveEquipmentRecv(f.Ng(), /*index*/ 2, kChassis);
        identity_access::EquipmentRecvClearInjectForTest();

        CHECK(o.equipResolved);
        CHECK_FALSE(o.idResolved);
        CHECK_FALSE(o.issued);
        CHECK(g_equipCap.calls == 0);
    }

    SUBCASE("slot != index: the fabricated id carries the live SLOT, not the UserIndex") {
        // The general multi-remote case the shipped fixture (slot == index) cannot exercise, and the
        // one this drive was mis-targeting: entrant at UserIndex 1 lives in live SLOT 2.  The id the
        // handler decodes must carry the SLOT (== what DressLoadout dresses), so NetUserId reads the
        // slot-2 gen row.  Feeding the index straight in would read slot-table[1] and fabricate the id
        // for a different entrant -- the wrong-coordinate defect this subcase pins.
        f.SetUser(/*index*/ 1, /*slot*/ 2, /*generation*/ 7);   // index 1 remaps to live slot 2
        f.LinkBackref(f.Ng());
        g_equipCap = EquipCapture{};
        identity_access::EquipmentRecvInjectForTest(f.cs.data(), &EquipCaptureHandler);
        const auto o = identity_access::DriveEquipmentRecv(f.Ng(), /*index*/ 1, kChassis);
        identity_access::EquipmentRecvClearInjectForTest();

        CHECK(o.idResolved);
        CHECK(o.issued);
        // entityId = slot | (gen<<16); the low word is the SLOT (2), NOT the index (1).
        CHECK((o.entityId & 0xFFFFu) == 2u);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
        CHECK(o.entityId == 0x00070002u);
        // and the handler received exactly that id in the fabricated body.
        CHECK(g_equipCap.calls == 1);
        CHECK((g_equipCap.entityId & 0xFFFFu) == 2u);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
        CHECK(g_equipCap.entityId == 0x00070002u);
    }

    // F1 CRASH GUARD: the recorded chassis is the engine's own unfilled-pair sentinel
    // (0xFFFFFFFFFFFFFFFF) or an unworn 0.  DriveEquipmentRecv must REFUSE it before touching the
    // engine -- driving it swaps to a model no asset resolves and leaves a NULL model in the pose
    // loop (echovr.exe+0x3c4476, index 4).  chassisRejected is set, the handler is never called, and
    // it happens even with a fully-linked owning netgame so the refusal is the chassis, not the ng.
    SUBCASE("F1: a sentinel/unworn chassis is refused WITHOUT driving the handler") {
        f.LinkBackref(f.Ng());
        for (uint64_t bad : {~uint64_t(0), uint64_t(0)}) {
            CAPTURE(bad);
            g_equipCap = EquipCapture{};
            identity_access::EquipmentRecvInjectForTest(f.cs.data(), &EquipCaptureHandler);
            const auto o = identity_access::DriveEquipmentRecv(f.Ng(), /*index*/ 1, bad);
            identity_access::EquipmentRecvClearInjectForTest();

            CHECK(o.chassisRejected);
            CHECK_FALSE(o.issued);
            CHECK_FALSE(o.faulted);
            CHECK_FALSE(o.spaceResolved);   // refused at the top, before any engine read
            CHECK(g_equipCap.calls == 0);   // the null-model swap the crash needed never happened
        }
    }
}

TEST_CASE("The fire gate drives once, only when ready and undressed") {
    using identity_access::EquipmentRecvShouldFire;
    // The one firing shape: not issued, mesh absent, chassis known, record ready.
    CHECK(EquipmentRecvShouldFire(/*mesh*/ false, /*chassis*/ true, /*record*/ true, /*issued*/ false));
    // Each single negative stops it.
    CHECK_FALSE(EquipmentRecvShouldFire(true,  true,  true,  false));  // already dressed
    CHECK_FALSE(EquipmentRecvShouldFire(false, false, true,  false));  // no recorded chassis
    CHECK_FALSE(EquipmentRecvShouldFire(false, true,  false, false));  // actor record not up yet
    CHECK_FALSE(EquipmentRecvShouldFire(false, true,  true,  true));   // fire-once latch held
}

// -------------------------------------------------------------------------------------------
// The engine's only enable site for a remote entrant is the remote arm of
// NETGAME.PARTICIPATING -- `if (USER_SPAWNED_GET(ng, idx)) ACTOR_ENABLE(...)` -- and the entrant
// add is the one time it is reached through a genuine role change.  So the reply seed writes
// `spawned` BEFORE the entrant-added message goes out, keyed by the SLOT: no entrant record exists
// yet, so there is no index to derive one from.
// -------------------------------------------------------------------------------------------
namespace spawnedseed {

// t.ng is the netgame pointer; `tail` makes the user-array word an index-keyed derivation would
// read for index == kSlot a real, in-bounds word, so that mistake answers a wrong slot instead of
// reading off the end of the fixture.
struct SeedNg {
    FakeSlotTable t;
    uint8_t       tail[0x400];
};
constexpr uint16_t kSlot = 3;
// Inside the store's slot count, so a write keyed on it GOES OUT -- to the wrong slot -- rather
// than being refused as unwritable.
constexpr uint16_t kSlotAnIndexWouldRead = 1;
static_assert(kSlotAnIndexWouldRead < kEntries && kSlotAnIndexWouldRead != kSlot,
              "the decoy must be a writable slot that is not the admitted one");
constexpr size_t   kUserWordOff = size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * kSlot;
static_assert(offsetof(SeedNg, t) == 0 && offsetof(FakeSlotTable, ng) == 0,
              "the netgame pointer handed to the route is the fixture's first byte");
static_assert(kUserWordOff >= sizeof(FakeSlotTable) && kUserWordOff + 2 <= sizeof(SeedNg),
              "the planted user word lies in the tail, clear of the lobby records");

struct SeedRecord {
    unsigned seq = 0;              // one counter across the setters and the publish
    unsigned selects = 0, slotSets = 0, flatSets = 0, publishes = 0;
    unsigned slotSetSeq = 0, publishSeq = 0;
    uint8_t  selectSawAcctType = 0xFF;
    void*    sawStore = nullptr;
    uint64_t sawSlot = ~0ull;
    int32_t  sawValue = -1;
    char     sawKey[32] = {};
};
SeedRecord g_rec;
SeedNg*    g_ng = nullptr;
uint8_t    g_storeObj[8];
uint64_t   g_storeSlots = kEntries;

// SESSION.KV_SIDE_SELECT as the engine computes it, off the SAME member record the seed writes:
// `slot < count && (rec[0] & 0xF) == 6 && rec[1]`.
uint8_t SelectOffTheRecord(void* store, uint64_t slot) {
    ++g_rec.selects;
    g_rec.sawStore = store;
    if (slot >= g_storeSlots) return 0;
    uint64_t head[2];
    memcpy(head, g_ng->t.Entry(uint32_t(slot)), sizeof head);
    g_rec.selectSawAcctType = uint8_t(head[0] & 0xF);
    return ((head[0] & 0xF) == 6 && head[1] != 0) ? 1 : 0;
}
void SeedSetFlat(void*, const char*, int32_t) { ++g_rec.flatSets; ++g_rec.seq; }
void SeedSetSlot(void* store, uint64_t slot, const char* key, int32_t v) {
    ++g_rec.slotSets;
    g_rec.slotSetSeq = ++g_rec.seq;
    g_rec.sawStore = store;
    g_rec.sawSlot  = slot;
    g_rec.sawValue = v;
    snprintf(g_rec.sawKey, sizeof g_rec.sawKey, "%s", key);
}
uint64_t SeqPublish(void* node, uint64_t msgId, const char* name, const void* payload,
                    uint64_t bytes) {
    ++g_rec.publishes;
    g_rec.publishSeq = ++g_rec.seq;
    return RecordPublish(node, msgId, name, payload, bytes);
}

struct Route {
    LegacyRouteScope legacy;
    FakeTap          tap;
    identity_access::HopCallTable calls;
    IIdentity*       id = nullptr;

    Route(SeedNg& ng) {
        g_pub = PublishRecord{};
        g_rec = SeedRecord{};
        g_ng  = &ng;
        ng.t.Build(kEntries);
        memset(ng.tail, 0, sizeof ng.tail);
        const uint32_t inMatch = facts::NETGAME_STATE_INMATCH;
        memcpy(ng.t.ng, &inMatch, sizeof inMatch);
        const uint16_t word = kSlotAnIndexWouldRead;
        memcpy(reinterpret_cast<uint8_t*>(&ng) + kUserWordOff, &word, sizeof word);
        // A stale BOT record in the slot: type 6 and a non-zero id select the FLAT side, so a
        // selector asked before the seed has written the remote type answers the wrong side.
        const uint64_t staleBot[2] = {6, 0x1234};
        memcpy(ng.t.entries + size_t(216) * kSlot, staleBot, sizeof staleBot);
        g_routeNg = &ng.t;

        calls.sideSelect     = SelectOffTheRecord;
        calls.setFlat        = SeedSetFlat;
        calls.setSlot        = SeedSetSlot;
        calls.store          = g_storeObj;
        calls.storeSlotCount = &g_storeSlots;
        identity_access::ReplySpawnedSeedInjectForTest(&calls);
        identity_access::ReplyRouteInjectForTest(
            reinterpret_cast<void*>(static_cast<uintptr_t>(0xB0ADCA57u)), &SeqPublish,
            0x51A73BEEF0001234ull);
        id = identity_access::Create(&RouteNetGame, nullptr);
        REQUIRE(id != nullptr);
        REQUIRE(identity_access::ReplyRouteArmWith(ng.t.ng, &tap));
    }
    ~Route() {
        identity_access::ReplyRouteDisarm();
        identity_access::ReplyRouteInjectForTest(nullptr, nullptr, 0);
        identity_access::ReplySpawnedSeedInjectForTest(nullptr);
        identity_access::Destroy(id);
        g_routeNg = nullptr;
        g_ng = nullptr;
    }

    void Admit(uint16_t recordedTeam) {
        JoinDesc desc{};
        desc.xplatformId   = 0xDEC0DE1234567890ull;
        desc.team          = recordedTeam;
        desc.useSpawnPoint = true;
        desc.entrantJson.p = reinterpret_cast<const uint8_t*>(kEntrantJson);
        desc.entrantJson.n = uint32_t(sizeof kEntrantJson - 1);
        FillDistinct(desc.loadoutSlots);
        UserIndex out(identity::kNotDressed);
        CHECK_FALSE(id->ProvisionReplayUser(UserSlot(kSlot), desc, out));   // parks wrong-phase
        CHECK(g_rec.slotSets + g_rec.flatSets == 0);                        // nothing from the tick
        identity_access::ReplyDrainFromEngineHeartbeat();
    }
};

}  // namespace spawnedseed

TEST_CASE("The reply seed writes `spawned` by slot, before the entrant add") {
    static spawnedseed::SeedNg ng;
    using spawnedseed::g_rec;

    SUBCASE("a recorded player: spawned=1, per-slot side, slot coordinate, ahead of the publish") {
        spawnedseed::Route route(ng);
        route.Admit(/*recordedTeam*/ 1);

        REQUIRE(g_rec.publishes == 1);
        CHECK(g_rec.flatSets == 0);
        REQUIRE(g_rec.slotSets == 1);
        // THE ORDER.  The add is where the engine's one enable visit happens, so a write after it
        // is the defect this packet fixes.
        CHECK(g_rec.slotSetSeq < g_rec.publishSeq);
        // THE COORDINATE.  The slot the admission was asked for -- not the slot bits of whatever
        // user record an index of the same number would name.
        CHECK(g_rec.sawSlot == spawnedseed::kSlot);
        CHECK(g_rec.sawSlot != spawnedseed::kSlotAnIndexWouldRead);
        CHECK(g_pub.slot == spawnedseed::kSlot);
        CHECK(g_rec.sawValue == 1);
        CHECK(strcmp(g_rec.sawKey, identity_access::SpawnedSlotKey()) == 0);
        CHECK(g_rec.sawStore == static_cast<void*>(spawnedseed::g_storeObj));
        // The selector was asked AFTER the member record held the remote type, so it named the
        // side the engine's reader will take during the add.
        CHECK(g_rec.selects == 1);
        CHECK(g_rec.selectSawAcctType == 5);
    }

    SUBCASE("a recorded spectator: the record is written false and the entrant is still admitted") {
        spawnedseed::Route route(ng);
        route.Admit(/*recordedTeam*/ 2);

        REQUIRE(PawnProvision::RecordedAsSpectatorRole(2));
        CHECK(g_rec.publishes == 1);
        REQUIRE(g_rec.slotSets == 1);
        CHECK(g_rec.sawValue == 0);
        CHECK(g_rec.sawSlot == spawnedseed::kSlot);
        CHECK(g_rec.slotSetSeq < g_rec.publishSeq);
    }

    SUBCASE("a slot past the store's count is not written, and the admission still goes out") {
        spawnedseed::Route route(ng);
        spawnedseed::g_storeSlots = spawnedseed::kSlot;   // the per-slot setter does no bounds check
        route.Admit(/*recordedTeam*/ 1);
        spawnedseed::g_storeSlots = kEntries;

        CHECK(g_rec.slotSets + g_rec.flatSets == 0);
        CHECK(g_rec.publishes == 1);
    }
}

TEST_CASE("RoleParticipates is the recorded value of `spawned`") {
    CHECK(PawnProvision::RoleParticipates(0));
    CHECK(PawnProvision::RoleParticipates(1));
    CHECK_FALSE(PawnProvision::RoleParticipates(2));
    CHECK_FALSE(PawnProvision::RoleParticipates(4));
    CHECK_FALSE(PawnProvision::RoleParticipates(PawnProvision::kRoleNotRecorded));
    CHECK_FALSE(PawnProvision::RoleParticipates(uint16_t(facts::NETGAME_USER_ROLE_BOUND)));
}

namespace spawnedseed {

struct PairRecord {
    unsigned seq = 0, participatingSeq = 0, dressSeq = 0;
    void*    participatingNg = nullptr;
    void*    dressNg = nullptr;
    uint16_t participatingIndex = kSlotUnmapped, dressIndex = kSlotUnmapped;
};
PairRecord g_pair;
void FakeParticipating(void* ng, uint16_t index) {
    g_pair.participatingSeq = ++g_pair.seq;
    g_pair.participatingNg = ng;
    g_pair.participatingIndex = index;
}
void FakeDress(void* ng, uint16_t index) {
    g_pair.dressSeq = ++g_pair.seq;
    g_pair.dressNg = ng;
    g_pair.dressIndex = index;
}

bool g_sweepAnswer = false;
void PairSweep(void* ctx, void*) {
    HopFixture* f = static_cast<HopFixture*>(ctx);
    g_sweepAnswer = f->surface.hops.participatePair(f->surface.hops.ctx, kHopIndex);
}
bool RunInWindow(HopFixture& f) {
    g_sweepAnswer = false;
    identity_access::RegisterSwapperResetSweep(&PairSweep, &f);
    identity_access::Stage0SweepForTest(f.ng);
    identity_access::ClearSwapperResetSweep();
    return g_sweepAnswer;
}

}  // namespace spawnedseed

TEST_CASE("ENG-SPAWNED-SEED backstop: the pair is PARTICIPATING then the dress, in the stage-0 window only") {
    using spawnedseed::g_pair;
    HopFixture f;
    f.calls.participating = spawnedseed::FakeParticipating;
    f.calls.dress         = spawnedseed::FakeDress;
    g_pair = spawnedseed::PairRecord{};
    REQUIRE(f.surface.hops.participatePair != nullptr);

    SUBCASE("from the tick -- outside the window -- nothing is called") {
        CHECK_FALSE(f.surface.hops.participatePair(f.surface.hops.ctx, kHopIndex));
        CHECK(g_pair.seq == 0);
    }
    SUBCASE("inside the window both are called once, in the engine's order, with (ng, index)") {
        CHECK(spawnedseed::RunInWindow(f));
        CHECK(g_pair.seq == 2);
        CHECK(g_pair.participatingSeq == 1);
        CHECK(g_pair.dressSeq == 2);
        CHECK(g_pair.participatingNg == static_cast<void*>(f.ng));
        CHECK(g_pair.dressNg == static_cast<void*>(f.ng));
        CHECK(g_pair.participatingIndex == kHopIndex);
        CHECK(g_pair.dressIndex == kHopIndex);
        // The window closes with the sweep.
        CHECK_FALSE(f.surface.hops.participatePair(f.surface.hops.ctx, kHopIndex));
        CHECK(g_pair.seq == 2);
    }
    SUBCASE("an unbound dress half refuses the whole pair") {
        f.calls.dress = nullptr;
        CHECK_FALSE(spawnedseed::RunInWindow(f));
        CHECK(g_pair.seq == 0);
    }
    SUBCASE("a local user's index is refused: that arm enables with no spawned gate") {
        *reinterpret_cast<uint16_t*>(f.ng + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF) = kHopIndex + 1;
        CHECK_FALSE(spawnedseed::RunInWindow(f));
        CHECK(g_pair.seq == 0);
    }
    SUBCASE("an index at the entrant count is refused") {
        *reinterpret_cast<uint16_t*>(f.ng + facts::IDENT_ENTRANT_COUNT_OFF) = kHopIndex;
        CHECK_FALSE(spawnedseed::RunInWindow(f));
        CHECK(g_pair.seq == 0);
    }
}

TEST_CASE("The seed's own ledger is what lets the add's enable be claimed") {
    static spawnedseed::SeedNg ng;
    constexpr uint16_t kIndexInTheSlot = 4;    // the entrant the engine would seat in kSlot
    identity_access::ActorHopSurface surface;
    surface.netGame = &RouteNetGame;
    identity_access::BindActorHops(surface);
    REQUIRE(surface.hops.seededBeforeAdd != nullptr);
    auto plant = [&] {
        const uint16_t word = spawnedseed::kSlot;
        memcpy(reinterpret_cast<uint8_t*>(&ng) + size_t(facts::USER_ARRAY_OFF) +
                   size_t(facts::USER_STRIDE) * kIndexInTheSlot, &word, sizeof word);
        // The entrant count is left as the fixture's 0xAB fill: it lies inside the lobby owner
        // block, and any count above the index is all SlotOfIndex asks of it.
    };
    static_assert(size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * 4 + 2 <= sizeof(spawnedseed::SeedNg),
                  "the planted entrant word lies inside the fixture");

    SUBCASE("a recorded player's slot answers true -- by SLOT, so the decoy index does not") {
        spawnedseed::Route route(ng);
        plant();
        CHECK_FALSE(surface.hops.seededBeforeAdd(surface.hops.ctx, kIndexInTheSlot));   // before the seed
        route.Admit(1);
        CHECK(surface.hops.seededBeforeAdd(surface.hops.ctx, kIndexInTheSlot));
        // Index 3 sits in slot 1, which nothing seeded.
        CHECK_FALSE(surface.hops.seededBeforeAdd(surface.hops.ctx, spawnedseed::kSlot));
    }
    SUBCASE("a recorded spectator's slot answers false") {
        spawnedseed::Route route(ng);
        plant();
        route.Admit(2);
        CHECK_FALSE(surface.hops.seededBeforeAdd(surface.hops.ctx, kIndexInTheSlot));
    }
    SUBCASE("a write that did not go out answers false") {
        spawnedseed::Route route(ng);
        plant();
        spawnedseed::g_storeSlots = spawnedseed::kSlot;
        route.Admit(1);
        spawnedseed::g_storeSlots = kEntries;
        CHECK_FALSE(surface.hops.seededBeforeAdd(surface.hops.ctx, kIndexInTheSlot));
    }
    SUBCASE("a disarm forgets it") {
        {
            spawnedseed::Route route(ng);
            plant();
            route.Admit(1);
        }
        g_routeNg = &ng.t;
        CHECK_FALSE(surface.hops.seededBeforeAdd(surface.hops.ctx, kIndexInTheSlot));
        g_routeNg = nullptr;
    }
}


// ===============================================================================================
// The roster showed every player as `...` / `00` for a whole nine-minute session
// because every entrant's admission answered ReplyWrongPhase -- a DEFERRAL, which the provisioner's
// own header says twice and in capitals is not a failure -- and the driver cleared its pending flag
// anyway.  The queue drained milliseconds later, the engine admitted all five players, and nothing
// ever wrote a name, a number, a team or a loadout.
// ===============================================================================================
namespace {

// Admission defers -- with the engine's own ReplyWrongPhase -- for a set number of passes per slot,
// then lands.  Everything is counted PER SLOT, because the property under test is "exactly once"
// and no aggregate can express that.
class DeferringIdentity final : public IIdentity {
public:
    static constexpr size_t kN = 8;
    uint32_t defersLeft[kN]    = {};
    uint32_t provisionCalls[kN] = {};
    uint32_t provisionOk[kN]   = {};
    uint32_t profileCalls[kN]  = {};
    PawnProvision::Refusal lastProvision = PawnProvision::Refusal::None;

    bool AddReplayUser(UserSlot s, UserIndex& out) override { out = UserIndex(s.v); return true; }
    bool ProvisionReplayUser(UserSlot s, const JoinDesc&, UserIndex& out) override {
        const size_t k = s.v % kN;
        ++provisionCalls[k];
        if (defersLeft[k] > 0) {
            --defersLeft[k];
            lastProvision = PawnProvision::Refusal::ReplyWrongPhase;
            return false;
        }
        lastProvision = PawnProvision::Refusal::None;
        ++provisionOk[k];
        out = UserIndex(s.v);
        return true;
    }
    PawnProvision::Refusal LastProvisionRefusal() const override { return lastProvision; }
    bool SetProfileJson(UserSlot s, const Blob&) override {
        ++profileCalls[s.v % kN];
        return true;
    }
    bool SetTeam(UserIndex, uint8_t, uint8_t) override { return true; }
    bool ApplyLoadout(UserIndex, const Symbol64[20], uint16_t, uint16_t) override { return true; }
    bool OverrideAlignment(UserIndex, uint16_t) override { return true; }
    bool ReadAppliedItems(UserIndex, Symbol64 out[20]) const override {
        memset(out, 0, 20 * sizeof(Symbol64));
        return true;
    }
    UserSlot  SlotOf(UserIndex i) const override { return UserSlot(i.v); }
    UserIndex IndexOf(UserSlot s) const override { return UserIndex(s.v); }
};

void TwoPlayers(PlayerRecord (&recs)[2]) {
    const char* names[2] = {"leavemealone_-", "hontomn2"};
    for (uint32_t i = 0; i < 2; ++i) {
        recs[i] = PlayerRecord{};
        recs[i].userSlot = uint16_t(i + 1);
        memcpy(recs[i].displayName, names[i], strlen(names[i]) + 1);
        FillDistinct(recs[i].loadoutSlots);
    }
}

}  // namespace

TEST_CASE("a deferred admission keeps the roster pending and is retried until it lands") {
    // THE DEFECT, REPRODUCED. Read as a failure, pass 1 reported `deferred 0` and the driver
    // stopped; every recorded player then kept the engine's placeholder name and number for the
    // rest of the file.
    CHECK(PawnProvision::IsDeferral(PawnProvision::Refusal::ReplyWrongPhase));
    // ...and the predicate is not "everything": a decided refusal must still latch.
    CHECK_FALSE(PawnProvision::IsDeferral(PawnProvision::Refusal::AdmitDidNotCreateUser));
    CHECK_FALSE(PawnProvision::IsDeferral(PawnProvision::Refusal::None));

    DeferringIdentity id;
    PlayerRecord recs[2];
    TwoPlayers(recs);
    id.defersLeft[1] = 3;
    id.defersLeft[2] = 3;

    RecordingLog log;
    identity::RosterMapping map;
    identity::RosterReport report{};

    identity::DressRoster(id, log, recs, 2, report, map, identity::DressPass::First);
    CHECK(report.deferred == 2);
    CHECK(report.dressed == 0);
    // A deferred player is still not dressed, so it is counted in `failed` as well: the two summary
    // numbers have always had to sum to the roster.
    CHECK(report.dressed + report.failed == report.players);
    // Named, with its reason, so the give-up can say which player and why.
    CHECK(report.deferredSlots[0].userSlot == 1);
    CHECK(report.deferredSlots[0].why == PawnProvision::Refusal::ReplyWrongPhase);
    CHECK(report.deferredSlots[1].userSlot == 2);
    // Nothing past the admission ran: the ladder stops at step 1.
    CHECK(id.profileCalls[1] == 0);

    // Two more passes, still deferring. The roster stays pending and says so with the same number.
    for (int pass = 0; pass < 2; ++pass) {
        identity::DressRoster(id, log, recs, 2, report, map, identity::DressPass::Retry);
        CHECK(report.deferred == 2);
    }

    // The fourth pass is the one the engine's queue has drained by. It lands.
    identity::DressRoster(id, log, recs, 2, report, map, identity::DressPass::Retry);
    CHECK(report.deferred == 0);
    CHECK(id.provisionOk[1] == 1);
    CHECK(id.provisionOk[2] == 1);
    CHECK(id.provisionCalls[1] == 4);   // three deferrals and the one that landed
    // ...and the ladder ran past the admission exactly once.  These records carry no entrant JSON,
    // and ENG-NAMES-2 refuses the SCALAR write by name for such a row rather than spending a call
    // the engine answers EmptyProfile to; the document those players get is the one the attach
    // step puts there.  So the count that proves "past the admission, once" is the team step.
    CHECK(id.profileCalls[1] == 0);
    CHECK(id.profileCalls[2] == 0);
}

TEST_CASE("a retry pass says nothing while nothing changes, and speaks when something does") {
    // A pass runs a few times a second for as long as the engine defers. Reported per pass, five
    // players times four lines would bury the one line that says the wait ended.
    DeferringIdentity id;
    PlayerRecord recs[2];
    TwoPlayers(recs);
    id.defersLeft[1] = 2;
    id.defersLeft[2] = 2;

    RecordingLog log;
    identity::RosterMapping map;
    identity::RosterReport report{};

    identity::DressRoster(id, log, recs, 2, report, map, identity::DressPass::First);
    const uint32_t afterFirst = log.n;
    CHECK(afterFirst > 0);
    CHECK(log.Count("NOT dressed: stage=") == 2);

    // Unchanged: silent. Not "quieter" -- silent.
    identity::DressRoster(id, log, recs, 2, report, map, identity::DressPass::Retry);
    CHECK(log.n == afterFirst);

    // The pass on which the admissions land is news, and it is reported.
    identity::DressRoster(id, log, recs, 2, report, map, identity::DressPass::Retry);
    CHECK(report.deferred == 0);
    CHECK(log.n > afterFirst);
}

TEST_CASE("a slot this load already dressed is not dressed again, and a reload re-arms it") {
    // THE ONE WAY THIS RETRY COULD MAKE THINGS WORSE: the ladder writes a profile, a slot name, a
    // team, twenty item ids, both numbers and a colour, and re-applies the loadout to the actor.
    // Running it a few times a second for the rest of the file is not idempotence, it is damage.
    DeferringIdentity id;
    PlayerRecord recs[2];
    TwoPlayers(recs);

    RecordingLog log;
    identity::RosterMapping map;
    identity::RosterReport report{};

    // Slot 1 reached the read-back on an earlier pass of this load; slot 2 did not.
    map.NoteDressPass(1, identity::RosterMapping::Outcome::Applied, true, 1, 1);

    identity::DressRoster(id, log, recs, 2, report, map, identity::DressPass::Retry);
    CHECK(id.provisionCalls[1] == 0);   // not re-admitted
    CHECK(id.profileCalls[1] == 0);     // and not re-written
    CHECK(report.alreadyApplied == 1);
    CHECK(report.dressed == 1);         // its earlier verdict is carried, not recomputed
    CHECK(report.dressed + report.failed == report.players);
    // The mapping is still published for the skipped slot -- a body that is not re-dressed still
    // has to be publishable.
    UserIndex got(identity::kNotDressed);
    CHECK(map.Lookup(1, got));
    CHECK(got.v == 1);
    // The slot that was NOT applied is dressed on this pass, so the skip is keyed on the latch and
    // not on "retry passes do nothing".
    CHECK(id.provisionCalls[2] == 1);

    // A reload (or an entity re-bind) drops the latch: the new game space issues new user indices,
    // so every slot is dressed again.
    map.ArmDressPass();
    identity::DressRoster(id, log, recs, 2, report, map, identity::DressPass::First);
    CHECK(id.provisionCalls[1] == 1);
    CHECK(report.alreadyApplied == 0);
}

TEST_CASE("the retry is bounded by CONTINUOUS deferral, and progress restarts the clock") {
    // Thirty seconds of a roster that is getting nowhere ends in a report, never in silence -- and
    // a roster the engine admits one player at a time is never given up on for taking its time.
    uint64_t since = 0;
    constexpr uint64_t kBound = 30000;

    // Nothing deferred: settled, and the run is forgotten.
    since = 1234;
    CHECK(identity::NextDressAction(0, 2, false, 50000, kBound, since) ==
          identity::DressAction::Settled);
    CHECK(since == 0);

    // The first deferring pass starts the run.
    CHECK(identity::NextDressAction(2, 0, true, 1000, kBound, since) ==  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
          identity::DressAction::KeepWaiting);
    CHECK(since == 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot

    // Inside the bound: still waiting, and the run is NOT restarted by a pass that changed nothing.
    CHECK(identity::NextDressAction(2, 2, false, 1000 + kBound - 1, kBound, since) ==  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
          identity::DressAction::KeepWaiting);
    CHECK(since == 1000);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot

    // One player stopped deferring. That is progress, so the bound starts again from here.
    CHECK(identity::NextDressAction(1, 2, false, 20000, kBound, since) ==
          identity::DressAction::KeepWaiting);
    CHECK(since == 20000);

    // ...which is why this pass, 29 s after the first deferral but 10 s after the last progress,
    // is still waiting rather than giving up.
    CHECK(identity::NextDressAction(1, 1, false, 30000, kBound, since) ==
          identity::DressAction::KeepWaiting);

    // The bound, reached with no progress at all.
    CHECK(identity::NextDressAction(1, 1, false, 20000 + kBound, kBound, since) ==
          identity::DressAction::GiveUp);
    // ...and a deferral that GREW is not progress either.
    since = 5000;
    CHECK(identity::NextDressAction(3, 1, false, 5000 + kBound, kBound, since) ==
          identity::DressAction::GiveUp);
}

TEST_CASE("jersey number 0 is a number, not an absence") {
    // `hontomn2` genuinely wears 0/0 in the subject capture, and the recorder writes 0 for "not
    // sourced" too -- so the dress must never decide absence from the VALUE. It does not test it
    // anywhere; this pins that, against a control that wears a non-zero number.
    DeferringIdentity id;
    PlayerRecord zero{};
    zero.userSlot = 1;
    memcpy(zero.displayName, "hontomn2", 9);
    FillDistinct(zero.loadoutSlots);
    zero.loadoutNumber = 0;
    zero.loadoutNumberBody = 0;

    identity::DressResult out{};
    identity::Dress(id, zero, out);
    CHECK(out.numberWanted == 0);
    CHECK(out.numberBodyWanted == 0);

    PlayerRecord seven = zero;
    seven.loadoutNumber = 7;
    seven.loadoutNumberBody = 3;
    identity::DressResult control{};
    identity::Dress(id, seven, control);
    CHECK(control.numberWanted == 7);
    CHECK(control.numberBodyWanted == 3);
}

// ---------------------------------------------------------------------------------------------
// The three stacked defects that left every player showing `...` / `00`, and the two
// rules that keep the dress off rows it must not touch.
// ---------------------------------------------------------------------------------------------
namespace names2 {

// The smallest IIdentity that can be dressed against: every call succeeds and every call is
// counted, so a test can assert that a step did NOT run as easily as that it did.
struct CountingIdentity : IIdentity {
    int provisions = 0, profiles = 0, teams = 0, loadouts = 0, alignments = 0, readbacks = 0;
    Symbol64 wrote[20] = {};
    uint16_t wroteNumber = 0xFFFF, wroteNumberBody = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

    bool AddReplayUser(UserSlot, UserIndex& out) override { out = UserIndex(3); return true; }
    bool ProvisionReplayUser(UserSlot, const JoinDesc&, UserIndex& out) override {
        ++provisions;
        out = UserIndex(3);
        return true;
    }
    bool SetProfileJson(UserSlot, const Blob&) override { ++profiles; return true; }
    bool SetTeam(UserIndex, uint8_t, uint8_t) override { ++teams; return true; }
    bool ApplyLoadout(UserIndex, const Symbol64 slots[20], uint16_t n, uint16_t nb) override {
        ++loadouts;
        for (int i = 0; i < 20; ++i) wrote[i] = slots[i];
        wroteNumber = n;
        wroteNumberBody = nb;
        return true;
    }
    bool OverrideAlignment(UserIndex, uint16_t) override { ++alignments; return true; }
    bool ReadAppliedItems(UserIndex, Symbol64 out[20]) const override {
        ++const_cast<CountingIdentity*>(this)->readbacks;
        for (int i = 0; i < 20; ++i) out[i] = wrote[i];
        return true;
    }
    UserSlot  SlotOf(UserIndex) const override { return UserSlot(3); }
    UserIndex IndexOf(UserSlot) const override { return UserIndex(3); }
};

// What playback_module's ProvisionedIdentity is: a remapped admission over the shipped identity,
// every other call forwarded.  The Underlying() forward is the whole of F2.
struct ForwardingWrapper : IIdentity {
    IIdentity& inner;
    explicit ForwardingWrapper(IIdentity& i) : inner(i) {}
    const IIdentity* Underlying() const override { return inner.Underlying(); }
    bool AddReplayUser(UserSlot s, UserIndex& o) override { return inner.AddReplayUser(s, o); }
    bool SetProfileJson(UserSlot s, const Blob& b) override { return inner.SetProfileJson(s, b); }
    bool SetTeam(UserIndex i, uint8_t a, uint8_t b) override { return inner.SetTeam(i, a, b); }
    bool ApplyLoadout(UserIndex i, const Symbol64 s[20], uint16_t n, uint16_t nb) override {
        return inner.ApplyLoadout(i, s, n, nb);
    }
    bool OverrideAlignment(UserIndex i, uint16_t a) override {
        return inner.OverrideAlignment(i, a);
    }
    bool ReadAppliedItems(UserIndex i, Symbol64 o[20]) const override {
        return inner.ReadAppliedItems(i, o);
    }
    UserSlot  SlotOf(UserIndex i) const override { return inner.SlotOf(i); }
    UserIndex IndexOf(UserSlot s) const override { return inner.IndexOf(s); }
};

// The control: the same wrapper WITHOUT the forward -- the shape that shipped.
struct OpaqueWrapper : ForwardingWrapper {
    using ForwardingWrapper::ForwardingWrapper;
    const IIdentity* Underlying() const override { return this; }
};

struct QuietLog : ILog {
    int errors = 0;
    void Write(LogLevel lvl, const char*, const char*, ...) override {
        if (lvl == LogLevel::Error) ++errors;
    }
    bool Enabled(LogLevel) const override { return true; }
    void DumpLedger() override {}
};

PlayerRecord Player(uint16_t slot, const char* name, uint16_t team16) {
    PlayerRecord r{};
    r.userSlot = slot;
    _snprintf_s(r.displayName, sizeof r.displayName, _TRUNCATE, "%s", name);
    r.teamId = uint8_t(team16);
    r.teamId16 = team16;
    r.teamSlot = 1;
    r.teamSlot16 = 1;
    r.alignment = uint8_t(team16);
    r.loadoutNumber = 11;
    r.loadoutNumberBody = 7;
    r.extended = 1;
    for (int i = 0; i < 20; ++i) r.loadoutSlots[i] = Symbol64(0x5000ull + uint64_t(i));
    return r;
}

}  // namespace names2

TEST_CASE("A wrapper answers with the identity it forwards to") {
    names2::CountingIdentity inner;
    names2::ForwardingWrapper good(inner);
    names2::OpaqueWrapper bad(inner);

    // The property Own() follows.  A bare implementation is its own underlying identity...
    CHECK(static_cast<const IIdentity&>(inner).Underlying() == &inner);
    // ...a forwarding wrapper is NOT, and answers the one it wraps.
    CHECK(static_cast<const IIdentity&>(good).Underlying() == &inner);
    CHECK(static_cast<const IIdentity&>(good).Underlying() != &good);
    // The control: without the forward the wrapper answers itself, which is what made every
    // identity_access:: free function report "not mine" for the whole dress pass.
    CHECK(static_cast<const IIdentity&>(bad).Underlying() == &bad);
}

TEST_CASE("The no-model sentinel is not an item") {
    Symbol64 want[20] = {}, got[20] = {};
    want[0] = identity_access::kUnrecordedItemId;
    got[0] = identity_access::kUnrecordedItemId;   // the engine reads back what was already there
    want[1] = Symbol64(0x1234);
    got[1] = Symbol64(0x1234);
    const identity::ItemDiff d = identity::DiffItems(want, got);
    // Scored, the sentinel matches itself and reports "2/2 items match" for an entrant wearing one
    // real item and no model.
    CHECK(d.compared == 1);
    CHECK(d.matching == 1);
}

TEST_CASE("An all-sentinel row refuses the loadout step and is judged on its name") {
    names2::CountingIdentity id;
    PlayerRecord rec = names2::Player(0, "KFC", 0);
    for (int i = 0; i < 20; ++i) rec.loadoutSlots[i] = identity_access::kUnrecordedItemId;

    identity::DressResult r{};
    identity::Dress(id, rec, r);
    // With no engine behind the identity the ladder stops at the slot-name table, which is two
    // steps ABOVE the loadout -- so what this proves offline is that the crashing write is never
    // reached and never made.  The named refusal itself needs the engine-backed fixture.
    CHECK(id.loadouts == 0);                 // the write that crashed is never made
    CHECK_FALSE(r.wrote.loadout);            // ...so the vacate is not owed it either
    CHECK(r.items.compared == 0);

    // ...and the verdict is about the name alone, exactly as a missing team costs only the team.
    identity::DressResult v{};
    v.loadoutRefusal = identity_access::Refusal::LoadoutNotRecorded;
    v.nameMatches = true;
    CHECK(identity::Verdict(v));
    v.loadoutRefusal = identity_access::Refusal::None;
    CHECK_FALSE(identity::Verdict(v));       // without the refusal, zero compared items still fails
}

TEST_CASE("A spectator row is never admitted and never dressed") {
    names2::CountingIdentity id;
    const PlayerRecord spectator =
        names2::Player(4, "KFC", uint16_t(identity_facts::kALIGN_TEAM_LIMIT));

    identity::DressResult r{};
    CHECK_FALSE(identity::Dress(id, spectator, r));
    CHECK(r.spectator);
    CHECK(r.refusal == identity_access::Refusal::SpectatorRow);
    // The damage is the ADMISSION: an admitted spectator burns an engine slot the engine will
    // never finish, and the pass then waits on it for the whole deferral bound.
    CHECK(id.provisions == 0);
    CHECK(id.profiles == 0);

    // ...and the roster counts it out of its own denominator, so nothing waits on it.
    names2::QuietLog log;
    const PlayerRecord roster[2] = {names2::Player(0, "leavemealone_-", 1), spectator};
    identity::RosterReport report{};
    identity::RosterMapping map;
    identity::DressRoster(id, log, roster, 2, report, map);
    CHECK(report.spectators == 1);
    CHECK(report.players == 1);
    CHECK(report.deferred == 0);
    CHECK(id.provisions == 1);   // the one real player, once
}

TEST_CASE("A bound measured on a clock that does not advance never expires") {
    uint64_t since = 0;
    // The first pass with a deferral starts the wait.
    CHECK(identity::NextDressAction(1, 0, true, 1, 30000, since) ==
          identity::DressAction::KeepWaiting);
    // A viewer paused at T0: the progress clock does not move, so no number of passes reaches the
    // bound.  The wall clock would have spent it in thirty seconds.
    for (int i = 0; i < 1000; ++i) {  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        CHECK(identity::NextDressAction(1, 1, false, 1, 30000, since) ==
              identity::DressAction::KeepWaiting);
    }
    // ...and the same clock, advanced past the bound, still gives up.
    CHECK(identity::NextDressAction(1, 1, false, 30001, 30000, since) ==
          identity::DressAction::GiveUp);
}

TEST_CASE("the no-model sentinel is never written into the engine's loadout instance") {
    // The guard that stops the confirmed 0xC0000005 in sub_1403C4470 had no
    // test that could fail: the one that claimed it asserted `id.loadouts == 0`, which the offline
    // ladder produces two steps earlier.  This drives the write rule itself and looks at the BYTES.
    using replay::Symbol64;
    using replay::identity_access::LoadoutItem;
    using replay::identity_access::LoadoutInstance;
    using replay::identity_access::kUnrecordedItemId;
    using replay::identity_access::kNoItemId;

    // What the slot held before: the previous occupant's items, so "left alone" and "cleared" are
    // different readings and the test can tell them apart.
    const Symbol64 kStale = Symbol64(0xDEADBEEFull);
    LoadoutItem items[20];
    for (size_t i = 0; i < 20; ++i) { items[i].slotId = Symbol64(i + 1); items[i].itemId = kStale; }
    LoadoutInstance inst{};
    inst.items = items;
    inst.itemCount = 20;

    SUBCASE("a partially recorded row writes its recorded ids and clears the rest") {
        Symbol64 slots[20];
        for (size_t i = 0; i < 20; ++i) slots[i] = Symbol64(0x100 + i);
        slots[3] = kUnrecordedItemId;
        slots[11] = kUnrecordedItemId;

        const replay::identity_access::ItemWrite w =
            replay::identity_access::WriteRecordedItems(inst, slots, 20);
        CHECK(w.recorded == 18);
        CHECK(w.cleared == 2);
        for (size_t i = 0; i < 20; ++i) {
            if (i == 3 || i == 11) {
                // NOT the sentinel -- writing it is the crash -- and NOT the previous occupant's
                // item either: the engine's own empty, which is what VacateSlot writes.
                CHECK(items[i].itemId == kNoItemId);
                CHECK_FALSE(items[i].itemId == kUnrecordedItemId);
                CHECK_FALSE(items[i].itemId == kStale);
            } else {
                CHECK(items[i].itemId == Symbol64(0x100 + i));
            }
            CHECK(items[i].slotId == Symbol64(i + 1));   // the catalogue's half is never touched
        }
    }

    SUBCASE("an all-sentinel row reaches the engine as twenty empties, never twenty sentinels") {
        Symbol64 slots[20];
        for (size_t i = 0; i < 20; ++i) slots[i] = kUnrecordedItemId;
        const replay::identity_access::ItemWrite w =
            replay::identity_access::WriteRecordedItems(inst, slots, 20);
        CHECK(w.recorded == 0);
        CHECK(w.cleared == 20);
        for (size_t i = 0; i < 20; ++i) CHECK(items[i].itemId == kNoItemId);
    }
}

// =================================================================================================
// The slot-name walk is on the LIVE 1 Hz capture path, on the game's own
// frame thread. Every hop to the 216-byte record is proved before it is followed, so an entrant
// leaving while the engine rebuilds the slot table produces a named refusal and not echovr's
// access violation in front of nine other players.
// =================================================================================================

TEST_CASE("the slot-name walk refuses an unmapped hop instead of reading through it") {
    using replay::identity_access::SlotNameRefusal;
    using replay::identity_access::SlotNameWalk;

    // A page nothing is mapped after: the reserved tail is what makes the far pointers unreadable.
    void* block = VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(block != nullptr);
    uint8_t* ng = static_cast<uint8_t*>(block);
    const uint64_t slotTableOff = ::facts::IDENT_SLOTTABLE_OFF;
    REQUIRE(slotTableOff + sizeof(void*) < 0x10000);

    replay::identity_access::ResetSlotNameWalkForTest();

    SUBCASE("null netGame") {
        CHECK(replay::identity_access::SlotNameFieldForTest(nullptr, 0) == nullptr);
        CHECK(SlotNameWalk().refusals[size_t(SlotNameRefusal::NoNetGame)] == 1);
    }

    SUBCASE("the slot table pointer is null") {
        memset(ng + slotTableOff, 0, sizeof(void*));
        CHECK(replay::identity_access::SlotNameFieldForTest(ng, 0) == nullptr);
        CHECK(SlotNameWalk().refusals[size_t(SlotNameRefusal::OwnerNull)] == 1);
    }

    SUBCASE("THE SECOND HOP IS UNMAPPED -- the vector header cannot be read") {
        // The shape of the live failure: a non-null pointer to memory the engine has freed.
        void* stale = reinterpret_cast<void*>(uintptr_t(0x7FF0'0000'0000ull));
        memcpy(ng + slotTableOff, &stale, sizeof stale);
        CHECK(replay::identity_access::SlotNameFieldForTest(ng, 0) == nullptr);
        CHECK(SlotNameWalk().refusals[size_t(SlotNameRefusal::VectorUnreadable)] == 1);
    }

    SUBCASE("THE THIRD HOP IS UNMAPPED -- the name array is not there") {
        void* owner = ng + 0x1000;
        memcpy(ng + slotTableOff, &owner, sizeof owner);
        uint8_t* vec = static_cast<uint8_t*>(owner) + ::facts::IDENT_SLOTTABLE_VEC_OFF;
        const uint64_t count = 16;
        memcpy(vec + ::facts::IDENT_SLOTNAME_COUNT_OFF, &count, sizeof count);
        void* stale = reinterpret_cast<void*>(uintptr_t(0x7FF0'0000'0000ull));
        memcpy(vec + ::facts::IDENT_SLOTNAME_BASE_OFF, &stale, sizeof stale);
        CHECK(replay::identity_access::SlotNameFieldForTest(ng, 0) == nullptr);
        CHECK(SlotNameWalk().refusals[size_t(SlotNameRefusal::EntryUnreadable)] == 1);
    }

    SUBCASE("a whole walk that proves out returns the field, so the guard is not refusing always") {
        void* owner = ng + 0x1000;
        memcpy(ng + slotTableOff, &owner, sizeof owner);
        uint8_t* vec = static_cast<uint8_t*>(owner) + ::facts::IDENT_SLOTTABLE_VEC_OFF;
        const uint64_t count = 4;
        memcpy(vec + ::facts::IDENT_SLOTNAME_COUNT_OFF, &count, sizeof count);
        uint8_t* base = ng + 0x4000;  // facts-ok: SESSION.FLAG_BIT_ACTIVE a test fixture value this case compares against, not an engine position
        memcpy(vec + ::facts::IDENT_SLOTNAME_BASE_OFF, &base, sizeof base);
        const char* field = replay::identity_access::SlotNameFieldForTest(ng, 1);
        REQUIRE(field != nullptr);
        CHECK(field == reinterpret_cast<const char*>(
                           base + uint64_t(::facts::IDENT_SLOTNAME_STRIDE) * 1 +
                           ::facts::IDENT_SLOTNAME_FIELD_OFF));
        // The engine's own bound still refuses a slot past the vector's count.
        CHECK(replay::identity_access::SlotNameFieldForTest(ng, 4) == nullptr);
        CHECK(SlotNameWalk().refusals[size_t(SlotNameRefusal::PastCount)] == 1);
    }

    replay::identity_access::ResetSlotNameWalkForTest();
    VirtualFree(block, 0, MEM_RELEASE);
}

// The ncaActorAlias reader, against a fabricated game space.  What is being pinned
// is the thing the live measurement depends on: that a record of the asked-for class is rendered
// with its index, its generation and its local index, that records of other classes are skipped,
// and that a table the space cannot supply becomes a word in the line instead of a fault -- this
// runs beside a live admission and a fault here would be the crash it exists to explain.
TEST_CASE("actor alias rows are rendered per class and never over-run the buffer") {
    namespace lp = replay::localpawn_facts;
    constexpr uint16_t kCls = 59;

    struct Row { uint16_t cls, idx, gen, pad; uint64_t spaceId; uint16_t loc, p1, p2, p3; };
    static_assert(sizeof(Row) == lp::kAliasRowStride, "the row is the engine's 24 bytes");

    alignas(16) static uint8_t space[8192];  // facts-ok: PHYS.CI_PAUSED_BIT the size of this fixture's backing buffer, not the physics paused bit
    alignas(16) static uint8_t alias[512];
    alignas(16) static uint8_t poolTable[136 * 64];
    alignas(16) static uint16_t genTable[16];
    alignas(16) static uint8_t destroyFlags[16];
    static Row rows[4];
    memset(space, 0, sizeof space);
    memset(alias, 0, sizeof alias);
    memset(poolTable, 0, sizeof poolTable);

    rows[0] = Row{kCls, 4, 1, 0, 0xAABB, 12, 0, 0, 0};
    rows[1] = Row{kCls, 5, 0, 0, 0xAABB, 13, 0, 0, 0};   // the generation-behind case
    rows[2] = Row{58,   5, 7, 0, 0xAABB, 99, 0, 0, 0};   // another class: must not appear
    rows[3] = Row{kCls, 7, 1, 0, 0xAABB, 15, 0, 0, 0};

    const void* rowsPtr = rows;
    const uint64_t rowCount = 4;
    memcpy(alias + lp::kAliasRowsOff, &rowsPtr, sizeof rowsPtr);
    memcpy(alias + lp::kAliasCountOff, &rowCount, sizeof rowCount);
    const void* aliasPtr = alias;
    memcpy(space + lp::kSpaceActorAliasOff, &aliasPtr, sizeof aliasPtr);

    uint8_t* const desc = poolTable + lp::kPoolDescStride * kCls;
    const uint16_t capacity = 8;
    const uint64_t freeSlots = 3;
    memcpy(desc + lp::kPoolDescCapOff, &capacity, sizeof capacity);
    memcpy(desc + lp::kPoolDescFreeOff, &freeSlots, sizeof freeSlots);
    for (uint16_t i = 0; i < 16; ++i) { genTable[i] = uint16_t(i == 5 ? 1 : 0); destroyFlags[i] = 0; }
    destroyFlags[5] = 1;
    const void* genPtr = genTable;
    memcpy(desc + lp::kPoolDescGenTableOff, &genPtr, sizeof genPtr);
    const void* poolPtr = poolTable;
    memcpy(space + lp::kSpacePoolTableOff, &poolPtr, sizeof poolPtr);
    const void* deadPtr = destroyFlags;
    memcpy(space + lp::kSpaceDestroyFlagTableOff + lp::kSpaceDestroyFlagStride * kCls, &deadPtr,
           sizeof deadPtr);

    char line[1024];
    CHECK(replay::actoralias::Describe(space, kCls, line, sizeof line) == 3);
    CHECK(strstr(line, "[4 g1 l12]") != nullptr);
    CHECK(strstr(line, "[5 g0 l13]") != nullptr);
    CHECK(strstr(line, "[7 g1 l15]") != nullptr);
    CHECK(strstr(line, "l99") == nullptr);               // class 58's row stayed out
    CHECK(strstr(line, "free=3/8") != nullptr);
    CHECK(strstr(line, "genTable=0,0,0,0,0,1,0,0,") != nullptr);
    CHECK(strstr(line, "destroyFlag=0,0,0,0,0,1,0,0,") != nullptr);

    // A space that supplies nothing is a sentence, not a fault.
    CHECK(replay::actoralias::Describe(nullptr, kCls, line, sizeof line) == 0);
    CHECK(strcmp(line, "no game space") == 0);

    // A count outside the sane band is refused before a single row is followed.
    const uint64_t absurd = uint64_t(lp::kAliasCountSane) + 1;
    memcpy(alias + lp::kAliasCountOff, &absurd, sizeof absurd);
    CHECK(replay::actoralias::Describe(space, kCls, line, sizeof line) == 0);
    CHECK(strstr(line, "table empty or out of band") != nullptr);
    memcpy(alias + lp::kAliasCountOff, &rowCount, sizeof rowCount);

    // The hatched offset is one number read out of one build. When the subsystem it names holds no
    // record of a class the engine is demonstrably allocating from, the reader finds the table that
    // does and says where it was -- which is what promotes the offset from inferred to observed.
    constexpr uint32_t kElsewhere = lp::kSpaceActorAliasOff + 16u;
    memset(space + lp::kSpaceActorAliasOff, 0, sizeof(void*));
    memcpy(space + kElsewhere, &aliasPtr, sizeof aliasPtr);
    CHECK(replay::actoralias::Describe(space, kCls, line, sizeof line) == 3);
    CHECK(strstr(line, "@space+3160") != nullptr);
    CHECK(strstr(line, "[5 g0 l13]") != nullptr);
    uint32_t scanned = 0;
    CHECK(replay::actoralias::FindTableOffset(space, kCls, &scanned) == kElsewhere);
    CHECK(scanned == 1);
    CHECK(replay::actoralias::FindTableOffset(space, 100, &scanned) == 0);
    memset(space + kElsewhere, 0, sizeof(void*));
    memcpy(space + lp::kSpaceActorAliasOff, &aliasPtr, sizeof aliasPtr);

    // Truncation terminates and stays inside the buffer.
    char small[24];
    memset(small, 0x7F, sizeof small);
    replay::actoralias::Describe(space, kCls, small, sizeof small - 1);
    CHECK(strlen(small) < sizeof small - 1);
    CHECK(small[sizeof small - 1] == 0x7F);

    // ENG-RELOAD-4's steer, read-only since ENG-STABILIZE-1: the free stack's top entry is checked
    // against the engine's own exact match before the engine allocates out of it, and an entry whose
    // record the previous free failed to re-key is REPORTED. The retirement arm is gone -- RELOAD-5's
    // paced leave made the free's own re-key land for every entry, so it had nothing left to retire,
    // and an unasked-for write into the engine's allocator is not what a check that never fires
    // should be holding. Every leg below therefore also pins that the pool is unchanged.
    using replay::actoralias::SteerResult;
    alignas(16) static uint16_t freeStack[8];
    const void* stackPtr = freeStack;
    memcpy(desc + lp::kPoolDescFreeStackOff, &stackPtr, sizeof stackPtr);
    genTable[4] = 1; genTable[5] = 1; genTable[7] = 1;   // rows 4 and 7 are at g1; row 5 is at g0
    const auto reset = [&] { freeStack[0] = 7; freeStack[1] = 4; freeStack[2] = 5; };

    const auto depth = [&] {
        uint64_t n = 0;
        memcpy(&n, desc + lp::kPoolDescFreeOff, sizeof n);
        return n;
    };

    reset();
    auto s = replay::actoralias::SteerAwayFromUnresolvable(space, kCls);
    CHECK(s.result == SteerResult::Unresolvable);
    CHECK(s.index == 5);             // the entry the pool is about to hand out
    CHECK(s.generation == 1);        // at a generation its record does not carry
    CHECK(s.freeCount == 3);
    CHECK(depth() == 3);             // and the free stack is exactly as it was
    CHECK(freeStack[2] == 5);

    // The control on the leg above: a top entry whose record IS at the pool's generation answers
    // resolvable, so "always Unresolvable" cannot pass this case.
    freeStack[2] = 4;
    s = replay::actoralias::SteerAwayFromUnresolvable(space, kCls);
    CHECK(s.result == SteerResult::Resolvable);
    CHECK(s.index == 4);
    CHECK(s.generation == 1);
    CHECK(depth() == 3);

    // Only the TOP is asked about: it is the one the next pop takes, and the entries under it are
    // not this call's business.
    genTable[7] = 0;                 // row 7 now lags, and it sits at the bottom of the stack
    CHECK(replay::actoralias::SteerAwayFromUnresolvable(space, kCls).result ==
          SteerResult::Resolvable);
    genTable[7] = 1;

    // No table anywhere in the space: a verdict, not a fault.
    memset(space + lp::kSpaceActorAliasOff, 0, sizeof(void*));
    reset();
    s = replay::actoralias::SteerAwayFromUnresolvable(space, kCls);
    CHECK(s.result == SteerResult::NoTable);
    CHECK(depth() == 3);
    memcpy(space + lp::kSpaceActorAliasOff, &aliasPtr, sizeof aliasPtr);

    // An empty free stack is the pool's business, not the steer's.
    const uint64_t noneFree = 0;
    memcpy(desc + lp::kPoolDescFreeOff, &noneFree, sizeof noneFree);
    CHECK(replay::actoralias::SteerAwayFromUnresolvable(space, kCls).result ==
          SteerResult::NoPool);
    memcpy(desc + lp::kPoolDescFreeOff, &freeSlots, sizeof freeSlots);
    CHECK(replay::actoralias::SteerAwayFromUnresolvable(nullptr, kCls).result ==
          SteerResult::NoPool);
}

// The repair, and the fake stands in for the engine's +640 virtual by doing exactly
// what sub_140216620 does with the key it is handed: find the record at that key and carry it one
// generation forward.  What is pinned is the decision, not the engine call -- which entries are
// judged to lag, how many times the virtual is invoked for each, that an entry with NO record is
// left alone (there is nothing to carry, and inventing a row is the one thing this must never do),
// and that a refused write names the entry and changes nothing.
namespace reload5 {

struct Row { uint16_t cls, idx, gen, pad; uint64_t spaceId; uint16_t loc, p1, p2, p3; };
static uint32_t g_rekeyCalls = 0;

// The engine's own re-key, as the thunk delivers it: (system, store, key).  The store is the
// sub-object the thunk's `lea` names, and the rows hang off it at kStoreRowsOff/kStoreCountOff.
static void __fastcall FakeRekey(void* instance, void* store, uint64_t key) {
    namespace lp = replay::localpawn_facts;
    ++g_rekeyCalls;
    (void)instance;
    uint8_t* const st = static_cast<uint8_t*>(store);
    Row* rows = nullptr;
    uint64_t n = 0;
    memcpy(&rows, st + lp::kStoreRowsOff, sizeof rows);
    memcpy(&n, st + lp::kStoreCountOff, sizeof n);
    const uint16_t cls = uint16_t(key), idx = uint16_t(key >> 16), gen = uint16_t(key >> 32);
    for (uint64_t i = 0; i < n; ++i) {
        if (rows[i].cls == cls && rows[i].idx == idx && rows[i].gen == gen) {
            ++rows[i].gen;
            return;
        }
    }
}

// A REAL thunk, byte for byte the shape the reader anchors on, in executable memory: the fixture
// has to prove the decode as well as the decision, and a hand-built pointer would prove neither.
//   4C 8B C2              mov r8, rdx
//   48 8D 91 <disp32>     lea rdx, [rcx+disp32]
//   E9 00 00 00 00        jmp +0            (the byte the anchor requires at offset 10)
//   48 B8 <FakeRekey>     mov rax, imm64
//   FF E0                 jmp rax
static uint8_t* BuildThunk(uint8_t* page, uint32_t storeOff) {
    uint8_t* p = page;
    *p++ = 0x4C; *p++ = 0x8B; *p++ = 0xC2;
    *p++ = 0x48; *p++ = 0x8D; *p++ = 0x91;
    memcpy(p, &storeOff, sizeof storeOff); p += sizeof storeOff;
    *p++ = 0xE9; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    void* const target = reinterpret_cast<void*>(&FakeRekey);
    *p++ = 0x48; *p++ = 0xB8;
    memcpy(p, &target, sizeof target); p += sizeof target;
    *p++ = 0xFF; *p++ = 0xE0;
    return page;
}

}  // namespace reload5

TEST_CASE("a pooled record left a generation behind is COUNTED, and nothing is written") {
    namespace lp = replay::localpawn_facts;
    using replay::actoralias::LagResult;
    constexpr uint16_t kCls = 59;
    static_assert(sizeof(reload5::Row) == lp::kAliasRowStride, "the row is the engine's 24 bytes");

    alignas(16) static uint8_t space[8192];  // facts-ok: PHYS.CI_PAUSED_BIT the size of this fixture's backing buffer, not the physics paused bit
    alignas(16) static uint8_t sysobj[512];
    alignas(16) static void* vtbl[128];
    alignas(16) static uint8_t poolTable[136 * 64];
    alignas(16) static uint16_t genTable[16];
    alignas(16) static uint16_t freeStack[8];
    static reload5::Row rows[5];
    memset(space, 0, sizeof space);
    memset(sysobj, 0, sizeof sysobj);
    memset(vtbl, 0, sizeof vtbl);
    memset(poolTable, 0, sizeof poolTable);

    uint8_t* const code = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    REQUIRE(code != nullptr);
    memset(code, 0xCC, 4096);
    reload5::BuildThunk(code, 0xC0);

    const auto seed = [&] {
        rows[0] = reload5::Row{kCls, 4, 1, 0, 0xAABB, 12, 0, 0, 0};
        rows[1] = reload5::Row{kCls, 5, 0, 0, 0xAABB, 13, 0, 0, 0};  // one generation behind
        rows[2] = reload5::Row{58,   5, 0, 0, 0xAABB, 99, 0, 0, 0};  // another class: never touched
        rows[3] = reload5::Row{kCls, 7, 1, 0, 0xAABB, 15, 0, 0, 0};
        rows[4] = reload5::Row{kCls, 6, 0, 0, 0xAABB, 14, 0, 0, 0};  // two generations behind
        reload5::g_rekeyCalls = 0;
    };
    seed();

    vtbl[lp::kRekeyVtableOff / sizeof(void*)] = code;
    void* const vtblPtr = vtbl;
    memcpy(sysobj, &vtblPtr, sizeof vtblPtr);
    const void* rowsPtr = rows;
    const uint64_t rowCount = 5;
    memcpy(sysobj + lp::kAliasRowsOff, &rowsPtr, sizeof rowsPtr);
    memcpy(sysobj + lp::kAliasCountOff, &rowCount, sizeof rowCount);
    const void* systemPtr = sysobj;
    memcpy(space + lp::kSpaceActorAliasOff, &systemPtr, sizeof systemPtr);

    uint8_t* const desc = poolTable + lp::kPoolDescStride * kCls;
    const uint16_t capacity = 8;
    const uint64_t freeSlots = 3;
    memcpy(desc + lp::kPoolDescCapOff, &capacity, sizeof capacity);
    memcpy(desc + lp::kPoolDescFreeOff, &freeSlots, sizeof freeSlots);
    for (uint16_t i = 0; i < 16; ++i) genTable[i] = 0;
    genTable[4] = 1; genTable[5] = 1; genTable[6] = 2; genTable[7] = 1;
    const void* genPtr = genTable;
    memcpy(desc + lp::kPoolDescGenTableOff, &genPtr, sizeof genPtr);
    freeStack[0] = 7; freeStack[1] = 4; freeStack[2] = 5;
    const void* stackPtr = freeStack;
    memcpy(desc + lp::kPoolDescFreeStackOff, &stackPtr, sizeof stackPtr);
    const void* poolPtr = poolTable;
    memcpy(space + lp::kSpacePoolTableOff, &poolPtr, sizeof poolPtr);

    replay::actoralias::detail::Pool pool;
    REQUIRE(replay::actoralias::detail::ReadPool(space, kCls, pool));
    CHECK(pool.capacity == 8);
    CHECK(pool.freeCount == 3);

    SUBCASE("every lagging entry is counted and named, and not one row moves") {
        const auto rep = replay::actoralias::ReportLaggingRecords(space, pool, kCls);
        CHECK(rep.result == LagResult::Lagging);
        CHECK(rep.tables == 1);
        CHECK(rep.lagging == 2);          // entries 5 and 6; 0-3 have no record at all
        CHECK(rep.firstIndex == 5);
        CHECK(rep.firstRowGen == 0);
        CHECK(rep.firstPoolGen == 1);
        // The write arm is gone: the engine's +640 re-key is never invoked, and
        // every row -- lagging, level and the class-58 neighbour -- is exactly where it was.
        CHECK(reload5::g_rekeyCalls == 0);
        CHECK(rows[0].gen == 1);
        CHECK(rows[1].gen == 0);
        CHECK(rows[2].gen == 0);
        CHECK(rows[3].gen == 1);
        CHECK(rows[4].gen == 0);

        // Read again, the same answer: it is a reading, so it is idempotent by construction and
        // can run before every admission.
        const auto again = replay::actoralias::ReportLaggingRecords(space, pool, kCls);
        CHECK(again.result == LagResult::Lagging);
        CHECK(again.lagging == 2);
        CHECK(reload5::g_rekeyCalls == 0);
    }

    SUBCASE("a table with nothing behind the pool reads in-sync") {
        genTable[5] = 0;
        genTable[6] = 0;
        const auto rep = replay::actoralias::ReportLaggingRecords(space, pool, kCls);
        CHECK(rep.result == LagResult::InSync);
        CHECK(rep.tables == 1);
        CHECK(rep.lagging == 0);
        genTable[5] = 1;
        genTable[6] = 2;
    }

    SUBCASE("an entry with no record at all is not a lag and no row is invented for it") {
        genTable[0] = 3;  // the pool has cycled entry 0 three times; the table never had a row
        const auto rep = replay::actoralias::ReportLaggingRecords(space, pool, kCls);
        CHECK(rep.lagging == 2);          // still only 5 and 6
        CHECK(rep.firstIndex == 5);
        uint64_t n = 0;
        memcpy(&n, sysobj + lp::kAliasCountOff, sizeof n);
        CHECK(n == 5);                    // nothing was inserted
        genTable[0] = 0;
    }

    SUBCASE("a record AHEAD of the pool is left alone") {
        rows[1].gen = 4;
        const auto rep = replay::actoralias::ReportLaggingRecords(space, pool, kCls);
        CHECK(rep.lagging == 1);          // entry 6 only
        CHECK(rep.firstIndex == 6);
        CHECK(rows[1].gen == 4);
    }

    SUBCASE("a system whose +640 slot is not the re-key thunk is skipped, never guessed at") {
        // THE DEFECT THAT KILLED LAUNCH f1, as a case.  Most subsystems in the band keep their
        // record store somewhere other than +0xC0, so reading rows off the hatched offset finds
        // whatever happens to sit there, and every count taken off that reading is a count of
        // somebody else's memory.  Unless the slot's own bytes say where the store is, the system
        // is not a candidate at all.
        code[5] = 0x00;  // no longer `lea rdx, [rcx+disp32]`
        const auto rep = replay::actoralias::ReportLaggingRecords(space, pool, kCls);
        CHECK(rep.result == LagResult::NoTable);
        CHECK(rep.tables == 0);
        CHECK(rep.lagging == 0);
        CHECK(rows[1].gen == 0);

        char line[512];
        replay::actoralias::DescribeReload(nullptr, space, kCls, line, sizeof line);
        CHECK(strstr(line, "child=<no class-59 store in 0 system(s)>") != nullptr);
        reload5::BuildThunk(code, 0xC0);
    }

    SUBCASE("the store offset is decoded from the thunk, in both of the forms the engine emits") {
        replay::actoralias::detail::Table t;
        REQUIRE(replay::actoralias::detail::ReadStore(sysobj, t));
        CHECK(t.storeOff == 0xC0);
        CHECK(t.count == 5);
        CHECK(t.rekey == code);

        // The short form: `48 8D 51 40` is `lea rdx,[rcx+40h]`, so the store is at +64 and the
        // rows hang off THAT -- which is a different place in the same object.
        uint8_t* const shortForm = code + 64;
        shortForm[0] = 0x4C; shortForm[1] = 0x8B; shortForm[2] = 0xC2;
        shortForm[3] = 0x48; shortForm[4] = 0x8D; shortForm[5] = 0x51; shortForm[6] = 0x40;
        shortForm[7] = 0xE9;
        vtbl[lp::kRekeyVtableOff / sizeof(void*)] = shortForm;
        const void* other = rows;
        const uint64_t three = 3;
        memcpy(sysobj + 64 + lp::kStoreRowsOff, &other, sizeof other);
        memcpy(sysobj + 64 + lp::kStoreCountOff, &three, sizeof three);
        replay::actoralias::detail::Table u;
        REQUIRE(replay::actoralias::detail::ReadStore(sysobj, u));
        CHECK(u.storeOff == 64);
        CHECK(u.count == 3);
        vtbl[lp::kRekeyVtableOff / sizeof(void*)] = code;
    }

    SUBCASE("HOST-SYNC-3: the scan band's garbage qwords and an overrunning store never fault") {
        // RUN-VERIFY-4: eight first-chance AVs in one admission, all from DescribeReload following
        // qwords like 0x20, 0xBFD, ~0 and 0x100000000, and one store whose count ran past the
        // committed end of its rows (va 0xC000002000). Every one is planted here.
        uint8_t* const arena = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 2 * 4096, MEM_RESERVE, PAGE_NOACCESS));
        REQUIRE(arena != nullptr);
        REQUIRE(VirtualAlloc(arena, 4096, MEM_COMMIT, PAGE_READWRITE) != nullptr);
        reload5::Row* const tail = reinterpret_cast<reload5::Row*>(arena + 4096) - 1;
        *tail = reload5::Row{kCls, 1, 0, 0, 0xAABB, 20, 0, 0, 0};
        alignas(16) static uint8_t overrun[512];
        memset(overrun, 0, sizeof overrun);
        memcpy(overrun, &vtblPtr, sizeof vtblPtr);
        const void* tailPtr = tail;
        const uint64_t tooMany = 3;
        memcpy(overrun + 0xC0 + lp::kStoreRowsOff, &tailPtr, sizeof tailPtr);
        memcpy(overrun + 0xC0 + lp::kStoreCountOff, &tooMany, sizeof tooMany);

        const uint64_t junk[] = {0x20, 0xBFD, 0x300, 0x7FFE, ~0ull, 0x100000000ull,
                                 reinterpret_cast<uint64_t>(arena + 4096),
                                 reinterpret_cast<uint64_t>(overrun)};
        uint32_t off = 2048;
        for (const uint64_t q : junk) {
            if (off == lp::kSpaceActorAliasOff) off += 8;
            memcpy(space + off, &q, sizeof q);
            off += 8;
        }

        replay::actoralias::detail::Table t;
        CHECK_FALSE(replay::actoralias::detail::ReadStore(overrun, t));

        const uint64_t faultsBefore = replay::ProbeFaultCount();
        const auto rep = replay::actoralias::ReportLaggingRecords(space, pool, kCls);
        char line[1024];
        replay::actoralias::DescribeReload(space, space, kCls, line, sizeof line);
        replay::actoralias::FindTableOffset(space, kCls, nullptr);
        CHECK(replay::ProbeFaultCount() == faultsBefore);
        CHECK(rep.tables == 1);
        CHECK(rep.lagging == 2);
        CHECK(strstr(line, "l20") == nullptr);  // the overrunning store is not rendered

        memset(space + 2048, 0, off - 2048);
        memcpy(space + lp::kSpaceActorAliasOff, &systemPtr, sizeof systemPtr);
        VirtualFree(arena, 0, MEM_RELEASE);
    }

    SUBCASE("no table and no pool are verdicts, not faults") {
        memset(space + lp::kSpaceActorAliasOff, 0, sizeof(void*));
        CHECK(replay::actoralias::ReportLaggingRecords(space, pool, kCls).result ==
              LagResult::NoTable);
        memcpy(space + lp::kSpaceActorAliasOff, &systemPtr, sizeof systemPtr);
        replay::actoralias::detail::Pool empty;
        CHECK(replay::actoralias::ReportLaggingRecords(space, empty, kCls).result ==
              LagResult::NoPool);
        CHECK(replay::actoralias::ReportLaggingRecords(nullptr, pool, kCls).result ==
              LagResult::NoPool);
    }

    SUBCASE("the reload line names both objects, the offset each table sat at, and the row's space id") {
        char line[4096];
        replay::actoralias::DescribeReload(space, space, kCls, line, sizeof line);
        CHECK(strstr(line, "root@3144+192 n=5:") != nullptr);
        CHECK(strstr(line, "child@3144+192 n=5:") != nullptr);
        CHECK(strstr(line, "[5 g0 l13 s0000AABB]") != nullptr);
        CHECK(strstr(line, "](4)") != nullptr);         // four class-59 rows counted, four shown
        CHECK(strstr(line, "l99") == nullptr);          // class 58 stayed out
        CHECK(strstr(line, "free=3/8") != nullptr);
        CHECK(strstr(line, "genTable=0,0,0,0,1,1,2,1,") != nullptr);

        replay::actoralias::DescribeReload(nullptr, space, kCls, line, sizeof line);
        CHECK(strstr(line, "root=<null>") != nullptr);
        CHECK(strstr(line, "child@3144+192") != nullptr);

        // Truncation terminates and stays inside the buffer.
        char small[32];
        memset(small, 0x7F, sizeof small);
        replay::actoralias::DescribeReload(space, space, kCls, small, sizeof small - 1);
        CHECK(strlen(small) < sizeof small - 1);
        CHECK(small[sizeof small - 1] == 0x7F);
    }

    // The fixture is static and shared by the subcases above; leave it as it was found.
    seed();
    VirtualFree(code, 0, MEM_RELEASE);
}
