// W1-E's half of `cam look at <n>`: the composer camera_control reads the aim point through.
//
// The roster here is a FAKE CR15NetGame -- a buffer whose entrant count and user words sit at the
// registry's own offsets -- and the pools are a fake IEntity. Neither is a second copy of a
// format: the offsets and the two bit fields come from facts_generated.h, the same header
// identity_access.cpp reads them from, so a registry change moves both sides at once.
//
// What each case is for is written on it. The three the brief names as mutants -- team and slot
// swapped, an unreadable pointer dereferenced, the install line removed -- are cases 2, 5 and 8.

#include "doctest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>
#include <windows.h>

#include "camera/camera_control.h"
#include "engine/entity.h"
#include "engine/entity_emote.h"
#include "engine/entity_subject.h"
#include "engine/identity_access.h"
#include "generated/facts_generated.h"

using replay::BodyState;
using replay::IEntity;
using replay::PhysHandle;
using replay::PlayerHandle;
using replay::PlayerState;
using replay::UserIndex;
namespace entity = replay::entity;
namespace camera_control = replay::camera_control;
namespace identity_access = replay::identity_access;

namespace {

// B.4's no-handle answer, and the value the fake returns for a user it does not carry.
constexpr uint16_t kNoHandle = 0xFFFFu;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value

uint16_t UserWord(uint8_t teamId, uint8_t teamSlot) {
    return uint16_t((uint16_t(teamId & facts::IDENT_USER_TEAM_MASK) << facts::IDENT_USER_TEAM_SHIFT) |
                    (uint16_t(teamSlot & facts::IDENT_USER_TEAMSLOT_MASK) << facts::IDENT_USER_TEAMSLOT_SHIFT));
}

// A roster of `count` entrants, occupied by the (team, slot) pairs handed in.
struct FakeRoster {
    std::vector<uint8_t> bytes;

    explicit FakeRoster(uint16_t count) {
        bytes.assign(size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * (count + 1u), 0);
        SetCount(count);
    }
    void* ng() { return bytes.data(); }
    void SetCount(uint16_t count) {
        memcpy(bytes.data() + facts::IDENT_ENTRANT_COUNT_OFF, &count, sizeof count);
    }
    void Occupy(uint16_t index, uint8_t teamId, uint8_t teamSlot) {
        const uint16_t word = UserWord(teamId, teamSlot);
        memcpy(bytes.data() + facts::USER_ARRAY_OFF + size_t(facts::USER_STRIDE) * index,
               &word, sizeof word);
    }
};

// The pools half. PlayerOf/PhysOf/ReadBody are the three steps the contract names; everything else
// on B.4 is unreachable from the composer and answers the interface's documented unavailable value.
class FakeEntity final : public IEntity {
public:
    // user -> player handle, player -> phys handle, phys -> com. Absent means "no such thing".
    uint16_t playerOfUser[64];
    uint16_t physOfPlayer[64];
    float    comOfPhys[64][3];
    bool     bodyReadable[64];
    int      playerCalls = 0, physCalls = 0, bodyCalls = 0;

    FakeEntity() {
        for (int i = 0; i < 64; ++i) {
            playerOfUser[i] = kNoHandle;
            physOfPlayer[i] = kNoHandle;
            bodyReadable[i] = false;
            comOfPhys[i][0] = comOfPhys[i][1] = comOfPhys[i][2] = 0.0f;
        }
    }
    void Wire(uint16_t user, uint16_t player, uint16_t phys, float x, float y, float z) {
        playerOfUser[user] = player;
        physOfPlayer[player] = phys;
        comOfPhys[phys][0] = x; comOfPhys[phys][1] = y; comOfPhys[phys][2] = z;
        bodyReadable[phys] = true;
    }

    PlayerHandle PlayerOf(UserIndex u) const override {
        ++const_cast<FakeEntity*>(this)->playerCalls;
        return PlayerHandle(u.v < 64 ? playerOfUser[u.v] : kNoHandle);
    }
    PhysHandle PhysOf(PlayerHandle p) const override {
        ++const_cast<FakeEntity*>(this)->physCalls;
        return PhysHandle(p.v < 64 ? physOfPlayer[p.v] : kNoHandle);
    }
    bool ReadBody(PhysHandle h, BodyState& out) const override {
        ++const_cast<FakeEntity*>(this)->bodyCalls;
        if (h.v >= 64 || !bodyReadable[h.v]) return false;
        memset(&out, 0, sizeof out);
        out.com[0] = comOfPhys[h.v][0];
        out.com[1] = comOfPhys[h.v][1];
        out.com[2] = comOfPhys[h.v][2];
        // A body whose OTHER fields are junk must not change the aim: com is the whole answer.
        out.vel[0] = 999.0f;
        out.lastHeldCom[0] = -777.0f;
        return true;
    }

    size_t PhysCount() const override { return 64; }
    bool   IsLocallyControlled(PhysHandle) const override { return false; }
    void   SetSyntheticOwner(PhysHandle) override {}
    void   ClearSyntheticOwner(PhysHandle) override {}
    void   PushHistory(PhysHandle, const BodyState&, uint64_t) override {}
    void   TrimHistory(PhysHandle, uint64_t, uint64_t) override {}
    float  GetTimeOffset(PhysHandle) const override { return 0.0f; }
    void   SetTimeOffset(PhysHandle, float) override {}
    size_t PlayerCount() const override { return 64; }
    bool   ReadPlayer(PlayerHandle, PlayerState&) const override { return false; }
    void   PushPlayerHistory(PlayerHandle, const PlayerState&, uint64_t) override {}
    void   SetLodThrottle(bool) override {}
    bool   Available() const override { return true; }
    const char* Diagnostic() const override { return "fake"; }
};

// Installs a fake for the life of a case and takes it back out, so one case cannot leave the
// composer pointing at a destroyed object.
struct SubjectScope {
    SubjectScope(IEntity* e, void* ng) { entity::SetSubjectEntity(e, ng); }
    ~SubjectScope() { entity::SetSubjectEntity(nullptr, nullptr); }
};

std::string ReadRepoFile(const char* relative) {
    std::string path = std::string(REPLAY_REPO_ROOT) + "/" + relative;
    FILE* f = fopen(path.c_str(), "rb");
    REQUIRE_MESSAGE(f != nullptr, path);
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

bool IdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Word-bounded, and blind to // comments: a name that appears only in prose is not a call.
size_t CountWordInCode(const std::string& text, const std::string& word) {
    size_t hits = 0;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        std::string line = text.substr(lineStart, lineEnd - lineStart);
        const size_t comment = line.find("//");
        if (comment != std::string::npos) line.erase(comment);
        for (size_t p = line.find(word); p != std::string::npos; p = line.find(word, p + 1)) {
            const bool leftOk = p == 0 || !IdentChar(line[p - 1]);
            const size_t e = p + word.size();
            const bool rightOk = e >= line.size() || !IdentChar(line[e]);
            if (leftOk && rightOk) ++hits;
        }
        if (lineEnd == text.size()) break;
        lineStart = lineEnd + 1;
    }
    return hits;
}

}  // namespace

// 1. The headline: every selector the engine accepts resolves to the entrant occupying the team
// slot the ENGINE's own packing names, on both teams, and the aim point is that entrant's com.
TEST_CASE("look at: each selector resolves to the user in the team slot the engine's packing names") {
    FakeRoster roster(10);
    FakeEntity fake;
    // Deliberately scrambled: entrant i does NOT hold team slot i, so a composer that returned the
    // index it was handed, or scanned in selector order, cannot pass.
    const uint8_t team[10] = {0, 1, 1, 0, 0, 1, 0, 1, 1, 0};
    const uint8_t slot[10] = {3, 4, 0, 0, 2, 2, 1, 1, 3, 4};
    for (uint16_t i = 0; i < 10; ++i) {
        roster.Occupy(i, team[i], slot[i]);
        fake.Wire(i, uint16_t(20 + i), uint16_t(40 + i),
                  1.0f * i + 0.5f, 2.0f * i + 0.25f, -3.0f * i - 0.75f);
    }
    SubjectScope scope(&fake, roster.ng());

    for (int32_t n = 1; n <= 10; ++n) {
        uint8_t wantTeam = 0, wantSlot = 0;
        REQUIRE(camera_control::PackSelector(n, &wantTeam, &wantSlot));

        uint16_t expected = kNoHandle;
        for (uint16_t i = 0; i < 10; ++i) {
            if (team[i] == wantTeam && slot[i] == wantSlot) expected = i;
        }
        REQUIRE_MESSAGE(expected != kNoHandle, n);   // the fixture covers every selector

        uint16_t got = kNoHandle;
        CHECK(entity::UserOfSelector(roster.ng(), n, &got) == camera_control::SubjectResolve::Resolved);
        CHECK(got == expected);

        float pt[3] = {0, 0, 0};
        CHECK(entity::WorldPointOfUser(roster.ng(), got, pt));
        CHECK(pt[0] == doctest::Approx(1.0f * expected + 0.5f));
        CHECK(pt[1] == doctest::Approx(2.0f * expected + 0.25f));
        CHECK(pt[2] == doctest::Approx(-3.0f * expected - 0.75f));
    }
    CHECK(fake.bodyCalls == 10);
}

// 2. MUTANT: team and slot compared the wrong way round. The two entrants below are each other's
// transpose, so a composer that matched TeamOf against teamSlot answers the OTHER one and this
// case reds -- which a roster whose team and slot happened to be equal could never show.
TEST_CASE("look at: the team is matched against the team and the slot against the slot") {
    FakeRoster roster(2);
    roster.Occupy(0, 1, 0);   // teamId 1, teamSlot 0 -- selector 1
    roster.Occupy(1, 0, 1);   // teamId 0, teamSlot 1 -- selector 7, and selector 1 transposed
    FakeEntity fake;
    SubjectScope scope(&fake, roster.ng());

    uint16_t got = kNoHandle;
    CHECK(entity::UserOfSelector(roster.ng(), 1, &got) == camera_control::SubjectResolve::Resolved);
    CHECK(got == 0);
    got = kNoHandle;
    CHECK(entity::UserOfSelector(roster.ng(), 7, &got) == camera_control::SubjectResolve::Resolved);
    CHECK(got == 1);

    // And the pair is not symmetric by accident: PackSelector's own answers differ in both halves.
    uint8_t t1 = 0, s1 = 0, t7 = 0, s7 = 0;
    REQUIRE(camera_control::PackSelector(1, &t1, &s1));
    REQUIRE(camera_control::PackSelector(7, &t7, &s7));
    CHECK(t1 == 1); CHECK(s1 == 0);
    CHECK(t7 == 0); CHECK(s7 == 1);
}

// 3. The refusals on the roster half, each by its own name.
TEST_CASE("look at: every way the roster cannot answer refuses by its own name") {
    FakeRoster roster(4);
    roster.Occupy(0, 1, 0);
    roster.Occupy(1, 1, 1);
    roster.Occupy(2, 0, 0);
    roster.Occupy(3, 0, 1);
    uint16_t got = kNoHandle;

    // No netGame at all: the tables could not be asked.
    CHECK(entity::UserOfSelector(nullptr, 1, &got) == camera_control::SubjectResolve::RosterUnreadable);

    // Out of the engine's own selector range, both ends. The roster is fine; nobody is named.
    CHECK(entity::UserOfSelector(roster.ng(), 0, &got) == camera_control::SubjectResolve::NoSuchSubject);
    CHECK(entity::UserOfSelector(roster.ng(), 11, &got) == camera_control::SubjectResolve::NoSuchSubject);
    CHECK(entity::UserOfSelector(roster.ng(), -4, &got) == camera_control::SubjectResolve::NoSuchSubject);

    // In range, readable, and nobody occupies that slot: team 1 slot 2 is selector 3.
    CHECK(entity::UserOfSelector(roster.ng(), 3, &got) == camera_control::SubjectResolve::NoSuchSubject);

    // An empty roster is empty, not unreadable -- and it must not resolve anyone.
    FakeRoster empty(0);
    CHECK(entity::UserOfSelector(empty.ng(), 1, &got) == camera_control::SubjectResolve::NoSuchSubject);

    // The answer is not written on a refusal.
    got = 4242;
    CHECK(entity::UserOfSelector(roster.ng(), 3, &got) == camera_control::SubjectResolve::NoSuchSubject);
    CHECK(got == 4242);
}

// 4. A composer that scans is only as bounded as the count it scans under: an entrant PAST the
// count is not in the roster, however inviting its team word looks.
TEST_CASE("look at: the scan stops at the engine's entrant count") {
    FakeRoster roster(2);
    roster.Occupy(0, 1, 0);
    roster.Occupy(1, 1, 1);
    roster.Occupy(2, 1, 2);   // written, but the count says two entrants
    uint16_t got = kNoHandle;
    CHECK(entity::UserOfSelector(roster.ng(), 3, &got) == camera_control::SubjectResolve::NoSuchSubject);
    roster.SetCount(3);
    CHECK(entity::UserOfSelector(roster.ng(), 3, &got) == camera_control::SubjectResolve::Resolved);
    CHECK(got == 2);
}

// 5. MUTANT: an unreadable pointer dereferenced. The count sits in a committed page and claims a
// user array that runs into a RESERVED one. Without the Readable() span check in UserTableBound
// this scan faults; with it, the refusal is named. The case also pins that the check is the SPAN
// and not just the first byte.
TEST_CASE("look at: a roster whose user array leaves committed memory refuses instead of reading it") {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    uint8_t* const region = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, page * 2, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(region != nullptr);
    REQUIRE(VirtualAlloc(region, page, MEM_COMMIT, PAGE_READWRITE) != nullptr);

    // ng sits so that the count is inside the committed page and the array crosses the boundary.
    void* const ng = region;
    uint16_t got = kNoHandle;

    const uint16_t plenty = 40;   // 40 * 592 bytes of user array does not fit in one page
    memcpy(region + facts::IDENT_ENTRANT_COUNT_OFF, &plenty, sizeof plenty);
    uint16_t bound = 0;
    CHECK_FALSE(replay::identity_access::UserTableBound(ng, bound));
    CHECK(entity::UserOfSelector(ng, 1, &got) == camera_control::SubjectResolve::RosterUnreadable);

    // The same region with a count whose array DOES fit answers, so the refusal above is the span
    // and not the region.
    const uint16_t few = 3;
    memcpy(region + facts::IDENT_ENTRANT_COUNT_OFF, &few, sizeof few);
    const uint16_t word = UserWord(1, 0);
    memcpy(region + facts::USER_ARRAY_OFF, &word, sizeof word);
    CHECK(replay::identity_access::UserTableBound(ng, bound));
    CHECK(bound == 3);
    FakeEntity fake;
    SubjectScope scope(&fake, ng);
    CHECK(entity::UserOfSelector(ng, 1, &got) == camera_control::SubjectResolve::Resolved);
    CHECK(got == 0);

    // And a netGame that is entirely reserved is unreadable, count included.
    uint8_t* const dead = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(dead != nullptr);
    CHECK_FALSE(replay::identity_access::UserTableBound(dead, bound));
    CHECK(entity::UserOfSelector(dead, 1, &got) == camera_control::SubjectResolve::RosterUnreadable);

    VirtualFree(dead, 0, MEM_RELEASE);
    VirtualFree(region, 0, MEM_RELEASE);
}

// 6. The pools half refuses at each of its own steps, and never hands back a point it did not read.
TEST_CASE("look at: every way the body cannot be read refuses and writes no point") {
    FakeRoster roster(1);
    roster.Occupy(0, 1, 0);
    float pt[3] = {11.0f, 12.0f, 13.0f};

    // Nothing published: this is the shipping image before entity_module's Init has run.
    CHECK_FALSE(entity::WorldPointOfUser(roster.ng(), 0, pt));

    FakeEntity fake;
    SubjectScope scope(&fake, roster.ng());

    CHECK_FALSE(entity::WorldPointOfUser(roster.ng(), 0, pt));    // no player for this user
    fake.playerOfUser[0] = 5;
    CHECK_FALSE(entity::WorldPointOfUser(roster.ng(), 0, pt));    // player carries no body
    fake.physOfPlayer[5] = 9;
    CHECK_FALSE(entity::WorldPointOfUser(roster.ng(), 0, pt));    // the body read itself refused
    CHECK(pt[0] == 11.0f);  CHECK(pt[1] == 12.0f);  CHECK(pt[2] == 13.0f);

    fake.bodyReadable[9] = true;
    fake.comOfPhys[9][0] = 4.5f; fake.comOfPhys[9][1] = -6.25f; fake.comOfPhys[9][2] = 0.125f;
    CHECK(entity::WorldPointOfUser(roster.ng(), 0, pt));
    // com exactly, byte for byte, with no offset applied to any axis.
    CHECK(pt[0] == 4.5f);  CHECK(pt[1] == -6.25f);  CHECK(pt[2] == 0.125f);

    // A roster index from a DIFFERENT session read against these pools names a different player.
    FakeRoster other(1);
    CHECK_FALSE(entity::WorldPointOfUser(other.ng(), 0, pt));
    CHECK(pt[0] == 4.5f);
}

// 7. The seam struct camera_control is handed is these two functions and nothing else.
TEST_CASE("look at: the installed seam is the composer") {
    CHECK(entity::kSubjectPose.userOfSelector == &entity::UserOfSelector);
    CHECK(entity::kSubjectPose.worldPointOfUser == &entity::WorldPointOfUser);
    CHECK(sizeof(camera_control::SubjectPoseSeam) == 2 * sizeof(void (*)()));
}

// 8. MUTANT: the install line removed. A composer nothing installs is a feature that refuses
// NoSeam in the shipping image -- the exact state ENG-W2E-P17I shipped and this packet exists to
// end -- and no unit test of the two functions above can see it.
TEST_CASE("look at: entity_module installs the seam, in code and not in a comment") {
    const std::string module = ReadRepoFile("src/engine/entity_module.cpp");
    CHECK(CountWordInCode(module, "SetSubjectPose") >= 1);
    CHECK(CountWordInCode(module, "kSubjectPose") >= 1);
    // The install is entity_module's, not camera_control's: the camera library must stay linkable
    // into an image that does not carry this module.
    const std::string control = ReadRepoFile("src/camera/camera_control.cpp");
    CHECK(CountWordInCode(control, "kSubjectPose") == 0);
    CHECK(CountWordInCode(control, "entity_subject") == 0);
    const std::string cameraModule = ReadRepoFile("src/camera/camera_module.cpp");
    CHECK(CountWordInCode(cameraModule, "kSubjectPose") == 0);

    // The counter-case, so the reader above is not one that passes on anything: a name that is
    // NOT in the file must come back zero, and a name that appears only in a comment must too.
    CHECK(CountWordInCode(module, "SetSubjectPoseNotAName") == 0);
    CHECK(CountWordInCode("// SetSubjectPose(&kSubjectPose);\n", "SetSubjectPose") == 0);
    CHECK(CountWordInCode("    camera_control::SetSubjectPose(&kSubjectPose);\n", "SetSubjectPose") == 1);
    CHECK(CountWordInCode("    MySetSubjectPoseX(0);\n", "SetSubjectPose") == 0);
}

// ---------------------------------------------------------------------------------------------
// W1-E EMOTE -- scoreboard bit 14, the entity-layer walk.
//
// The component system here is FAKE, and deliberately so: the engine's own R15NetCustomization
// vtable is only reachable inside a live game space.  What is real is every number the walk uses
// -- IDENT_EMOTE_VTSLOT_OFF, STATS_EMOTE_VTSLOT and IDENT_EMOTE_CS_COUNT_OFF all come out of
// facts_generated.h, the same header entity_emote.cpp reads them from -- and the call shape, which
// is asserted from inside the fake: the key lands in argument 3 and the -1 sentinel in argument 4,
// byte for byte, or the case fails.
namespace {

struct FakeElement {
    void*    vtable;
    uint64_t sawKey;
    uint64_t sawSentinel;
    int      calls;
    uint32_t id;
    bool     owns;
    uint32_t idAtPlusFour;
};

// The engine reads the id back through the RETURNED pointer, so the fake returns the buffer it was
// handed and plants a decoy one dword further on: a reader that takes +4 instead of +0 gets a
// number no element claims.
void* FakeRecord(void* self, void* out, uint64_t key, uint64_t sentinel) {
    FakeElement* const e = static_cast<FakeElement*>(self);
    e->sawKey = key;
    e->sawSentinel = sentinel;
    ++e->calls;
    static_cast<uint32_t*>(out)[0] = e->id;
    static_cast<uint32_t*>(out)[1] = e->idAtPlusFour;
    return out;
}

int FakeOwns(void* self, uint32_t emoteId) {
    FakeElement* const e = static_cast<FakeElement*>(self);
    return (e->owns && emoteId == e->id) ? 1 : 0;
}

// A component system is a pointer array at [cs+0] bounded by a count at [cs+COUNT_OFF].
struct FakeCs {
    unsigned char bytes[facts::IDENT_EMOTE_CS_COUNT_OFF + 8];
    void*         slots[4];
    void* Build(void** elements, uint64_t n) {
        memset(bytes, 0, sizeof(bytes));
        for (uint64_t i = 0; i < n && i < 4; ++i) slots[i] = elements[i];
        void* const array = slots;
        memcpy(bytes, &array, sizeof(array));
        memcpy(bytes + facts::IDENT_EMOTE_CS_COUNT_OFF, &n, sizeof(n));
        return bytes;
    }
};

void FillVtable(void** vt, size_t n) {
    for (size_t i = 0; i < n; ++i) vt[i] = nullptr;
    vt[facts::IDENT_EMOTE_VTSLOT_OFF / sizeof(void*)] = reinterpret_cast<void*>(&FakeRecord);
    vt[facts::STATS_EMOTE_VTSLOT / sizeof(void*)] = reinterpret_cast<void*>(&FakeOwns);
}

}  // namespace

// 9. The walk resolves, and the second element is the one that claims the entrant. A reader that
// returned the first element, or took the id from the wrong dword, cannot pass this.
TEST_CASE("emote: the component walk picks the element that claims the id") {
    void* vt[80];
    FillVtable(vt, 80);
    FakeElement a = { vt, 0, 0, 0, 0x11112222u, false, 0xDEADBEEFu };
    FakeElement b = { vt, 0, 0, 0, 0x5A5A1234u, true,  0xDEADBEEFu };
    void* elements[2] = { &a, &b };
    FakeCs cs;
    void* const csp = cs.Build(elements, 2);

    identity_access::EmoteSource src;
    identity_access::Refusal why = identity_access::Refusal::None;
    const uint64_t key = 0x0123456789ABCDEFull;
    CHECK(entity::ResolveEmoteOnSystem(csp, key, src, &why) == true);
    CHECK(src.componentSystem == &b);
    CHECK(src.emoteId == 0x5A5A1234u);
    // The call shape, from inside the callee: both elements asked, each with the key in argument 3
    // and -1 in argument 4.
    CHECK(a.calls == 1);
    CHECK(b.calls == 1);
    CHECK(a.sawKey == key);
    CHECK(b.sawKey == key);
    CHECK(b.sawSentinel == 0xFFFFFFFFFFFFFFFFull);
}

// 10. Nobody claims it. That is a refusal by name, never a published "not playing".
TEST_CASE("emote: an unclaimed entrant refuses EmoteNoSystem") {
    void* vt[80];
    FillVtable(vt, 80);
    FakeElement a = { vt, 0, 0, 0, 0x11112222u, false, 0xDEADBEEFu };
    void* elements[1] = { &a };
    FakeCs cs;
    identity_access::EmoteSource src;
    identity_access::Refusal why = identity_access::Refusal::None;
    CHECK(entity::ResolveEmoteOnSystem(cs.Build(elements, 1), 7u, src, &why) == false);
    CHECK(why == identity_access::Refusal::EmoteNoSystem);
    CHECK(src.componentSystem == nullptr);
    CHECK(src.emoteId == 0u);
    // and a null system is the same refusal for the other reason
    why = identity_access::Refusal::None;
    CHECK(entity::ResolveEmoteOnSystem(nullptr, 7u, src, &why) == false);
    CHECK(why == identity_access::Refusal::EmoteNoSystem);
}

// 11. MUTANT COVER: an element whose vtable is reserved-but-not-committed. The slot read must
// refuse, not fault -- a test host reaches here with pointers the engine never made.
TEST_CASE("emote: an unreadable vtable refuses instead of dereferencing") {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    void* const reserved = VirtualAlloc(nullptr, si.dwPageSize * 2, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(reserved != nullptr);
    FakeElement a = { reserved, 0, 0, 0, 1u, true, 0u };
    void* elements[1] = { &a };
    FakeCs cs;
    identity_access::EmoteSource src;
    identity_access::Refusal why = identity_access::Refusal::None;
    CHECK(entity::ResolveEmoteOnSystem(cs.Build(elements, 1), 7u, src, &why) == false);
    CHECK(why == identity_access::Refusal::EmoteVtableUnreadable);
    CHECK(a.calls == 0);
    VirtualFree(reserved, 0, MEM_RELEASE);
}

// 12. The seam identity_access asks. With no key function the chain cannot name an entrant, and
// that is EmoteNoSystem -- distinct from EmoteNoSeam, which means nothing was installed at all.
TEST_CASE("emote: the seam names which of the two refusals applies") {
    identity_access::SetEmoteSource(nullptr);
    identity_access::EmoteSource src;
    identity_access::Refusal why = identity_access::Refusal::None;
    CHECK(identity_access::ResolveEmoteSource(nullptr, UserIndex(0), src, &why) == false);
    CHECK(why == identity_access::Refusal::EmoteNoSeam);

    entity::EmoteChain chain;
    entity::SetEmoteChain(&chain);
    identity_access::SetEmoteSource(&entity::kEmoteSource);
    why = identity_access::Refusal::None;
    CHECK(identity_access::ResolveEmoteSource(nullptr, UserIndex(0), src, &why) == false);
    CHECK(why == identity_access::Refusal::EmoteNoSystem);

    entity::SetEmoteChain(nullptr);
    identity_access::SetEmoteSource(nullptr);
    CHECK(entity::InstalledEmoteChain() == nullptr);
}

// 13. MUTANT: the install line removed. A seam nothing installs leaves bit 14 clear in the
// shipping image for a reason no unit test of the walk above can see.
TEST_CASE("emote: entity_module installs the seam, in code and not in a comment") {
    const std::string module = ReadRepoFile("src/engine/entity_module.cpp");
    CHECK(CountWordInCode(module, "SetEmoteSource") >= 1);
    CHECK(CountWordInCode(module, "kEmoteSource") >= 1);
    CHECK(CountWordInCode(module, "SetEmoteChain") >= 1);
    // MUTANT: the key source dropped. Without this line the installed chain has no actorKey and
    // the seam refuses EmoteNoSystem for every entrant, exactly as it did before it was derived.
    CHECK(CountWordInCode(module, "EmoteActorGuidKey") >= 1);
    // The component-system half: resolved from the module, on the space the binding validated.
    // Unit-green cannot see this -- the seam answers from a chain a test can fill by hand.
    CHECK(CountWordInCode(module, "ResolveCustomizationCs") >= 1);
    // counter-cases, so the reader itself cannot pass on anything
    CHECK(CountWordInCode(module, "kEmoteSourceNotAName") == 0);
    CHECK(CountWordInCode("// identity_access::SetEmoteSource(&kEmoteSource);\n", "kEmoteSource") == 0);
    CHECK(CountWordInCode("    identity_access::SetEmoteSource(&kEmoteSource);\n", "kEmoteSource") == 1);
    CHECK(CountWordInCode("    MykEmoteSourceX(0);\n", "kEmoteSource") == 0);
}


// 14. The key an entrant is named by, and the one quantity ENG-W1E-EMOTE-3 derived: the SECOND
// qword of the entrant's SActorGUID (sub_1401B2EA0 0x1401B3EB8 -> 0x1401B3EFF -> r8 at
// 0x1401B3F61). The two halves are planted with DIFFERENT values on purpose: a read of the first
// qword instead -- the one plausible wrong answer -- passes against any GUID whose halves match.
TEST_CASE("emote: the entrant key is the SActorGUID's second qword") {
    FakeRoster roster(4);
    const uint64_t low  = 0x1122334455667788ull;
    const uint64_t high = 0x99AABBCCDDEEFF01ull;
    uint8_t* const guid = static_cast<uint8_t*>(roster.ng()) + facts::IDENT_USER_ACTOR_GUID_OFF +
                          size_t(facts::USER_STRIDE) * 2u;
    memcpy(guid, &low, sizeof low);
    memcpy(guid + sizeof low, &high, sizeof high);

    uint64_t key = 0;
    CHECK(entity::EmoteActorGuidKey(roster.ng(), UserIndex(2), key) == true);
    CHECK(key == high);
    CHECK(key != low);

    // An entrant the roster carries but nobody filled reads as that build's zero, and it is
    // REPORTED as read: the roster is what is empty, not the read.
    key = 0xDEADull;
    CHECK(entity::EmoteActorGuidKey(roster.ng(), UserIndex(3), key) == true);
    CHECK(key == 0u);

    // Past the bound, and no netGame at all. Both refuse, and neither leaves a value behind for a
    // caller that ignored the bool -- 0 is a key the component system would happily be asked about.
    key = 0xDEADull;
    CHECK(entity::EmoteActorGuidKey(roster.ng(), UserIndex(4), key) == false);
    CHECK(key == 0u);
    key = 0xDEADull;
    CHECK(entity::EmoteActorGuidKey(nullptr, UserIndex(0), key) == false);
    CHECK(key == 0u);
}

// ---------------------------------------------------------------------------------------
// W1-E bit 14, the OTHER input: the R15NetCustomization component system, resolved through the
// engine's own CncaGameSpace::FindComponentSystem.  The fake lookup answers only for the recorded
// symbol on the bound space, so a wrong symbol is a null system and a refusal -- never the system
// this fixture happens to have planted.
namespace {

constexpr size_t   kCsBlobBytes  = 0x5000;
constexpr size_t   kCsMapAt      = 0x0400;
constexpr size_t   kCsPoolAt     = 0x0800;
constexpr size_t   kCsTargetAt   = 0x1E80;
constexpr size_t   kCsTargetVtAt = 0x2680;
constexpr size_t   kCsElemVtAt   = 0x3000;
// The element's vtable must be long enough to hold BOTH dispatch slots, and the system object
// starts past it -- derived from those two slots rather than transcribed as a round number.
constexpr size_t   kCsElemVtSpan = (facts::IDENT_EMOTE_VTSLOT_OFF > facts::STATS_EMOTE_VTSLOT
                                        ? facts::IDENT_EMOTE_VTSLOT_OFF
                                        : facts::STATS_EMOTE_VTSLOT) + sizeof(void*);
constexpr size_t   kCsSystemAt   = kCsElemVtAt + kCsElemVtSpan;
constexpr uint16_t kCsElement    = 1;
constexpr uint32_t kCsEmoteId    = 3;
constexpr int32_t  kCsPlayingArg = 0x51EED;

// The entrant key the walk is expected to carry, and one no element owns.
constexpr uint64_t kCsEntrantKey  = 0x99AABBCCDDEEFF01ull;
constexpr uint64_t kCsStrangerKey = 0x1122334455667788ull;

int EmoteQueryFn(void*, int32_t arg)   { return arg == kCsPlayingArg ? 1 : 0; }
int EmoteIdleFn(void*, int32_t)        { return 0; }

// IDENT.EMOTE_VTSLOT: the record call answers about the entrant it was KEYED with, so a wrong key
// yields an id no element owns and the walk falls out of its bound instead of naming a player.
void* EmoteRecordFn(void*, void* out, uint64_t key, uint64_t) {
    const uint32_t id = (key == kCsEntrantKey) ? kCsEmoteId : 0xDEADu;
    memcpy(out, &id, sizeof id);
    return out;
}
int EmoteOwnsFn(void*, uint32_t id) { return id == kCsEmoteId ? 1 : 0; }

struct EmoteCsFixture {
    uint8_t* blob     = nullptr;   // the element: the walk finds it, ReadEmotePlaying reads it
    uint8_t* cs       = nullptr;   // the component system: [cs+0] element array, +0x30 count
    uint8_t* target   = nullptr;
    uint8_t* handleAt = nullptr;
    void*    elements[1] = { nullptr };

    EmoteCsFixture() {
        blob = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kCsBlobBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(blob != nullptr);
        memset(blob, 0, kCsBlobBytes);

        uint8_t* const map  = blob + kCsMapAt;
        uint8_t* const pool = blob + kCsPoolAt;
        memcpy(blob + facts::STATS_EMOTE_MAP_OFF,  &map,  sizeof map);
        memcpy(blob + facts::STATS_EMOTE_POOL_OFF, &pool, sizeof pool);
        memcpy(map + size_t(4) * kCsEmoteId, &kCsElement, sizeof kCsElement);

        target = blob + kCsTargetAt;
        uint8_t* const targetVt = blob + kCsTargetVtAt;
        void* const idle = reinterpret_cast<void*>(&EmoteIdleFn);
        for (size_t i = 0; i < 0x400; i += sizeof(void*)) memcpy(targetVt + i, &idle, sizeof idle);
        void* const live = reinterpret_cast<void*>(&EmoteQueryFn);
        memcpy(targetVt + facts::STATS_EMOTE_VTSLOT, &live, sizeof live);
        memcpy(target, &targetVt, sizeof targetVt);

        handleAt = pool + size_t(facts::STATS_EMOTE_ELEM_STRIDE) * kCsElement +
                   facts::STATS_EMOTE_ELEM_ADJ;
        memcpy(handleAt, &target, sizeof target);
        memcpy(handleAt + facts::STATS_EMOTE_TARGET_OFF, &target, sizeof target);
        memcpy(handleAt + facts::STATS_EMOTE_ARG_OFF, &kCsPlayingArg, sizeof kCsPlayingArg);

        // The element's OWN vtable, at element+0: the two slots the walk dispatches through.
        uint8_t* const elemVt = blob + kCsElemVtAt;
        void* const record = reinterpret_cast<void*>(&EmoteRecordFn);
        void* const owns   = reinterpret_cast<void*>(&EmoteOwnsFn);
        memcpy(elemVt + facts::IDENT_EMOTE_VTSLOT_OFF, &record, sizeof record);
        memcpy(elemVt + facts::STATS_EMOTE_VTSLOT,     &owns,   sizeof owns);
        memcpy(blob, &elemVt, sizeof elemVt);

        cs = blob + kCsSystemAt;
        elements[0] = blob;
        void* const array = elements;
        const uint64_t count = 1;
        memcpy(cs, &array, sizeof array);
        memcpy(cs + facts::IDENT_EMOTE_CS_COUNT_OFF, &count, sizeof count);
    }
    ~EmoteCsFixture() { if (blob) VirtualFree(blob, 0, MEM_RELEASE); }
};

EmoteCsFixture* g_csFixture = nullptr;
void*           g_csSpace   = nullptr;

// CncaGameSpace::FindComponentSystem's shape.  It knows one symbol and one space.
void* FakeFindCs(void* space, uint64_t symbol) {
    if (!g_csFixture || space != g_csSpace) return nullptr;
    return symbol == facts::IDENT_SYM_R15NETCUSTOMIZATION ? g_csFixture->cs : nullptr;
}

bool FakeEntrantKey(void*, UserIndex entrant, uint64_t& out) {
    out = entrant.v == 0 ? kCsEntrantKey : kCsStrangerKey;
    return true;
}

}  // namespace

TEST_CASE("W1-E bit 14: the customization system resolves through the engine's own lookup and "
          "is_emote_playing round-trips") {
    EmoteCsFixture f;
    uint8_t space[16] = { 0 };
    uint8_t netGame[16] = { 0 };
    g_csFixture = &f;
    g_csSpace = space;

    void* const cs = entity::ResolveCustomizationCs(g_csSpace, &FakeFindCs);
    REQUIRE(cs == f.cs);
    CHECK(entity::ResolveCustomizationCs(nullptr, &FakeFindCs) == nullptr);
    // In a test host nothing is mapped at FACTS.CS_LOOKUP_BY_SYMBOL, so the prologue guard refuses
    // rather than calling whatever address happens to be there.
    CHECK(entity::EngineCsLookup() == nullptr);
    CHECK(entity::ResolveCustomizationCs(g_csSpace, nullptr) == nullptr);

    entity::EmoteChain chain;
    chain.customizationCs = cs;
    chain.actorKey = &FakeEntrantKey;
    chain.netGame = netGame;
    entity::SetEmoteChain(&chain);
    identity_access::SetEmoteSource(&entity::kEmoteSource);

    const UserIndex entrant(0);
    identity_access::EmoteSource src;
    identity_access::Refusal why = identity_access::Refusal::EmoteNoSeam;
    REQUIRE(identity_access::ResolveEmoteSource(netGame, entrant, src, &why));
    CHECK(why == identity_access::Refusal::None);
    CHECK(src.componentSystem == f.blob);
    CHECK(src.emoteId == kCsEmoteId);

    bool playing = false;
    REQUIRE(identity_access::ReadEmotePlaying(src, playing, &why));
    CHECK(why == identity_access::Refusal::None);
    CHECK(playing);

    // The other half of the round trip: the engine's own false, a handle that names no target.
    void* const none = nullptr;
    memcpy(f.handleAt + facts::STATS_EMOTE_TARGET_OFF, &none, sizeof none);
    playing = true;
    REQUIRE(identity_access::ReadEmotePlaying(src, playing, &why));
    CHECK(why == identity_access::Refusal::None);
    CHECK(!playing);
    memcpy(f.handleAt + facts::STATS_EMOTE_TARGET_OFF, &f.target, sizeof f.target);

    // An entrant no element owns is a refusal, not a cleared bit published as "not playing".
    const UserIndex stranger(1);
    why = identity_access::Refusal::None;
    CHECK(!identity_access::ResolveEmoteSource(netGame, stranger, src, &why));
    CHECK(why == identity_access::Refusal::EmoteNoSystem);

    // The system was resolved out of THIS netGame's space.  Asked about another netGame it would
    // name a different player and come back a clean success, so the chain declines the pairing.
    uint8_t otherNetGame[16] = { 0 };
    why = identity_access::Refusal::None;
    CHECK(!identity_access::ResolveEmoteSource(otherNetGame, entrant, src, &why));
    CHECK(why == identity_access::Refusal::EmoteNoSystem);

    identity_access::SetEmoteSource(nullptr);
    entity::SetEmoteChain(nullptr);
    g_csFixture = nullptr;
    g_csSpace = nullptr;
}
