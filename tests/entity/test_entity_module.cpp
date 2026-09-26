// The wiring check for W1-E: not whether IEntity works, but whether anything ever creates it.
//
// DIAG-WIRING gap 1 found EntityCreate / PosePublisherCreate / AsSeenCreate with zero callers and
// services->entity / pose / asSeen never assigned, after 72k green assertions. This file links the
// real module translation unit through the real modreg and asks the services struct, which is the
// only place a missing registration is visible. Every case below turns red if REPLAY_MODULE in
// entity_module.cpp is deleted, if Init stops assigning, or if Tick stops binding.
//
// The game space here is a fake: a buffer whose component-system table header is laid out the way
// the engine's CTable constructor leaves it. EntityCreate on this host refuses at the site guard
// (no echovr.exe image is mapped), so Available() stays false by design -- what is asserted is that
// the module reached it, published it, and re-validates it.

#include "doctest.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#include "bootstrap/modreg.h"
#include "bootstrap/services.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/tableguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/pageprobe.h"
#include "engine/asseen.h"
#include "camera/camera_control.h"
#include "engine/entity.h"
#include "engine/scriptgraph_facts.h"
#include "engine/entity_subject.h"
#include "engine/localpawn_facts.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"
#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "foundation/gametype.h"
#include <stdio.h>
#include <string>

// AUD-C1 F3's measurement seam, defined in src/engine/entity_module.cpp: the tableguard judgements
// the candidate line has made. Declared here because this file is its only caller and a signature
// that drifts from the definition is a link error, not a silent second spelling.
namespace replay {
namespace entity {
uint64_t CandidateTableChecks();
}
}

namespace {

struct Line {
    replay::LogLevel level;
    std::string module;
    std::string text;
};

class RecordingLog final : public replay::ILog {
public:
    void Write(replay::LogLevel level, const char* module, const char* fmt, ...) override {
        // The section 4.2 evidence line is four <TABLE> renderings wide; a recorder that truncated it
        // would red the byte-exact case for a reason that is not the product's.
        char text[2048];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, args);
        va_end(args);
        lines.push_back(Line{level, module ? module : "", text});
    }
    bool Enabled(replay::LogLevel) const override { return true; }
    void DumpLedger() override {}

    // The whole text of the last line carrying `needle`, empty if there is none. The evidence line
    // is asserted byte for byte, and a substring test cannot see the bytes around the match.
    std::string LastWith(const char* needle, size_t from = 0) const {
        std::string found;
        for (size_t i = from; i < lines.size(); ++i)
            if (lines[i].module == "entity" && lines[i].text.find(needle) != std::string::npos)
                found = lines[i].text;
        return found;
    }

    size_t Count(const char* needle) const {
        size_t n = 0;
        for (const Line& l : lines)
            if (l.module == "entity" && l.text.find(needle) != std::string::npos) ++n;
        return n;
    }

    std::vector<Line> lines;
};

class DefaultConfig final : public replay::IConfig {
public:
    bool GetBool(const char*, bool dflt) const override { return dflt; }
    int64_t GetInt(const char*, int64_t dflt) const override { return dflt; }
    double GetDouble(const char*, double dflt) const override { return dflt; }
    const char* GetString(const char*, const char* dflt) const override { return dflt; }
    bool Has(const char*) const override { return false; }
};

class NoTick final : public replay::ITick {
public:
    bool RegisterEarly(replay::TickFn, void*) override { return true; }
    bool RegisterLate(replay::TickFn, void*) override { return false; }
    const char* Mode() const override { return "none"; }
};

// The CTable header the engine's constructor writes at space+0x420, and the element block it
// points at. Offsets as src/bootstrap/tableguard.h / src/ui/csregister.cpp read them.
const uint32_t kCsTableOff = 0x420u;
const uint64_t kEntryStride = 16u;

struct FakeSpace {
    alignas(16) uint8_t bytes[kCsTableOff + 0x40] = {};
    alignas(16) uint8_t elements[4 * kEntryStride] = {};
    alignas(16) uint8_t elementsRebuilt[4 * kEntryStride] = {};
    // An allocator the append path could call: a vtable with the slots the engine dispatches through.
    uint64_t vtable[8] = {};
    uint64_t allocatorObject = reinterpret_cast<uintptr_t>(vtable);

    void Put(uint32_t off, uint64_t v) { memcpy(bytes + kCsTableOff + off, &v, sizeof v); }
    uint64_t Get(uint32_t off) const {
        uint64_t v;
        memcpy(&v, bytes + kCsTableOff + off, sizeof v);
        return v;
    }

    void Construct(const uint8_t* block, uint64_t count = 1) {
        Put(0x00, reinterpret_cast<uintptr_t>(block));       // base
        Put(0x08, 4 * kEntryStride);                          // bytes
        Put(0x10, reinterpret_cast<uintptr_t>(&allocatorObject));
        Put(0x20, 4);                                         // grow
        Put(0x28, 4);                                         // capacity
        Put(0x30, count);                                     // count
        Put(0x38, 0);                                         // flags
    }

    // ARCH-12 4.3's levelsym: the space's own CSymbol64 at SPACE.NAME_SYMBOL_OFF. Planted with a
    // value no blank fixture holds, so a rendering that reproduces it is answering a question.
    static_assert(facts::SPACE_NAME_SYMBOL_OFF + sizeof(uint64_t) <= kCsTableOff + 0x40,
                  "the rig must reach the space's own name-symbol field");
    void SetNameSymbol(uint64_t sym) { memcpy(bytes + facts::SPACE_NAME_SYMBOL_OFF, &sym, sizeof sym); }
};

// ARCH-12 5.3's third rig object: a CR15Game long enough to hold GAME.SPAWN_GAMESPACE_SYM_OFF.
// 35 KB, so it is heap-allocated by every case that needs one -- a fixture this size on the stack
// is a test that fails for a reason that has nothing to do with what it asserts.
struct FakeGame {
    std::vector<uint8_t> bytes =
        std::vector<uint8_t>(facts::GAME_SPAWN_GAMESPACE_SYM_OFF + sizeof(uint64_t), 0);

    void* Base() { return bytes.data(); }
    void SetSpawnSymbol(uint64_t sym) {
        memcpy(bytes.data() + facts::GAME_SPAWN_GAMESPACE_SYM_OFF, &sym, sizeof sym);
    }
};

// A canonical user-mode address nothing maps: the rigs plant it where a read must fail, so a refusal
// is a refusal about THAT slot rather than about a zero any blank fixture would also have held.
const uintptr_t kUnmapped = 0x0000600DBADBAD00ull;

struct FakeNetGame {
    // Big enough to carry the local-user group as well as the game-space slot: the record array
    // starts at 0x178 and its first word is the field the slot is masked out of. The space slot is
    // NETGAME.CS_SPACE_OFF (ARCH-10 section 3.4), so the buffer has to reach past it -- a rig too
    // short to hold that slot and still passing would be a rig not reading the field at all. Sized
    // FROM the fact rather than to a number, so it cannot fall behind one that moves; where the
    // product READS is what these cases test, and that is asserted below, not here.
    // ARCH-12 5.3's rig contract: the buffer must reach past the LEVEL space slot at
    // NETGAME.SPECTATORCAM_CS_OFF (+0x2B10) as well, "a rig smaller than 0x2B18 that still passes
    // is a rig that is not reading the field". Sized from the facts, and asserted to be the
    // farther of the two so a fact that reorders them cannot shrink this buffer silently.
    static_assert(facts::NETGAME_SPECTATORCAM_CS_OFF > facts::NETGAME_CS_SPACE_OFF,
                  "the level-space slot is the farther one; this buffer is sized from it");
    alignas(16) uint8_t bytes[facts::NETGAME_SPECTATORCAM_CS_OFF + sizeof(void*)] = {};

    // The space at NETGAME.CS_SPACE_OFF, and an UNMAPPED non-null pointer at the CR15Game
    // back-pointer this module used to read. A build that went back to +0x8 does not find this
    // fixture twice -- it finds an address it cannot read, and every case here goes red.
    void PointAt(const FakeSpace* s) {
        void* p = s ? const_cast<uint8_t*>(s->bytes) : nullptr;
        memcpy(bytes + facts::NETGAME_CS_SPACE_OFF, &p, sizeof p);
        void* game = reinterpret_cast<void*>(kUnmapped);
        memcpy(bytes + facts::IDENT_NETGAME_GAMESPACE_OFF, &game, sizeof game);
    }

    // Space L: the LEVEL CncaGameSpaceMT at +0x2B10 (ARCH-12 2.2). Distinct from PointAt's space C
    // by construction -- the rig contract's point is that the two spaces differ in CONTENT, so the
    // caller passes a separately built FakeSpace and never the same one twice.
    void PointLevelAt(const FakeSpace* s) {
        void* p = s ? const_cast<uint8_t*>(s->bytes) : nullptr;
        memcpy(bytes + facts::NETGAME_SPECTATORCAM_CS_OFF, &p, sizeof p);
    }

    // A READABLE CR15Game at +0x8, replacing PointAt's unmapped one, so `spawnsym` can render a
    // symbol instead of the word for a read that was refused.
    void PointGameAt(void* game) {
        memcpy(bytes + facts::IDENT_NETGAME_GAMESPACE_OFF, &game, sizeof game);
    }

    // The aliasing case ARCH-12 4.3 rule 5 exists for: one object in both slots.
    void AliasLevelToCsSpace() {
        void* p = nullptr;
        memcpy(&p, bytes + facts::NETGAME_CS_SPACE_OFF, sizeof p);
        memcpy(bytes + facts::NETGAME_SPECTATORCAM_CS_OFF, &p, sizeof p);
    }

    // The mirror of the defect ARCH-10 closes: the space present ONLY at the old slot. The shipping
    // resolver must answer NoSpace here; a resolver reading +0x8 would bind.
    void PointAtLegacySlotOnly(const FakeSpace* s) {
        void* p = s ? const_cast<uint8_t*>(s->bytes) : nullptr;
        memcpy(bytes + facts::IDENT_NETGAME_GAMESPACE_OFF, &p, sizeof p);
        void* none = nullptr;
        memcpy(bytes + facts::NETGAME_CS_SPACE_OFF, &none, sizeof none);
    }

    void NoGame() {
        void* none = nullptr;
        memcpy(bytes + facts::IDENT_NETGAME_GAMESPACE_OFF, &none, sizeof none);
    }

    void Put16(uint32_t off, uint16_t v) { memcpy(bytes + off, &v, sizeof v); }
    uint16_t Get16(uint32_t off) const {
        uint16_t v;
        memcpy(&v, bytes + off, sizeof v);
        return v;
    }

    // The shape AddPlayerUser leaves behind: one local user, its record at 0x178 carrying the slot
    // in the low five bits, and the slot table mapping that slot back to record 0. The table is
    // POISONED first, so the cross-check is answering a question rather than reading a zero it
    // would have read from a blank fixture anyway; and the record word carries bits outside the
    // mask, so a read that drops the mask cannot come back with the right number.
    void SetLocalUser(uint16_t slot, uint16_t localCount = 1) {
        namespace lpf = replay::localpawn_facts;
        for (uint32_t i = 0; i < facts::ENTITY_USER_SLOT_TABLE_MAX; ++i)
            Put16(lpf::kNetGameSlotTableOff + lpf::kNetGameSlotTableStride * i, 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        Put16(lpf::kNetGameLocalUserCountOff, localCount);
        Put16(lpf::kNetGameUserRecordOff, static_cast<uint16_t>((slot & 0x1Fu) | 0x0940u));
        if (slot < facts::ENTITY_USER_SLOT_TABLE_MAX)
            Put16(lpf::kNetGameSlotTableOff + lpf::kNetGameSlotTableStride * slot, 0);
    }
    void NoLocalUser() {
        Put16(replay::localpawn_facts::kNetGameLocalUserCountOff, 0);
    }
};

// A component system as entity.cpp's Pool addresses it. Every offset is the fact the shipping code
// reads, so a fact that moves moves the fake with it rather than leaving a fixture that agrees with
// nothing. Reached only through EntityCreateWithLookup: a test host maps no echovr.exe, so the byte
// guard on CS.LOOKUP_BY_SYMBOL refuses every EntityCreate and nothing below it -- the pools,
// PoolNotReady, Init's reset -- is otherwise reachable at all. The guarded path is still asserted
// (the cases below check EntityCreate refuses with NoPhysicsCs on this host).
struct FakeCs {
    alignas(16) uint8_t  bytes[0x200] = {};
    alignas(16) uint8_t  elements[4 * 832] = {};   // 832 = the larger of the two CI strides
    uint32_t handleMap[64] = {};
    uint16_t reverseMap[64] = {};

    void PutPtr(uint32_t off, const void* p) { memcpy(bytes + off, &p, sizeof p); }

    void Wire(uint32_t mapOff, uint32_t poolOff, uint32_t countOff, uint32_t revOff, uint16_t count) {
        PutPtr(mapOff, handleMap);
        PutPtr(poolOff, elements);
        PutPtr(revOff, reverseMap);
        memcpy(bytes + countOff, &count, sizeof count);
    }

    void WirePhys() {
        Wire(facts::PHYS_CS_HANDLE_MAP_OFF, facts::PHYS_CS_POOL_BASE_OFF, facts::PHYS_CS_COUNT_OFF,
             facts::PHYS_CS_REVERSE_MAP_OFF, 1);
    }
    void WirePlayer() {
        Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
             facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 1);
    }

    uint32_t& Owner(uint16_t index) {
        return *reinterpret_cast<uint32_t*>(elements + facts::PHYS_CI_STRIDE * index +
                                            facts::PHYS_CI_OWNER_OFF);
    }
};

FakeCs* g_physCs = nullptr;
FakeCs* g_playerCs = nullptr;
FakeCs* g_bcastCs = nullptr;
// The leaf systems, null unless a case wires them: InitLocalPawn stops at the first one that is
// absent, so leaving them null is what keeps every case written before this one at the shape it
// was written for.
FakeCs* g_echoCs = nullptr;
FakeCs* g_bodyCs = nullptr;
FakeCs* g_modelCs = nullptr;
FakeCs* g_touchCs = nullptr;
FakeCs* g_frisbeeCs = nullptr;

// The leaf systems answer null on purpose: InitLocalPawn is deliberately unable to fail
// Entity::Init, so a resolved entity whose local pawn stopped at the broadcaster is a real shape
// and the one these cases want.
void* FakeFind(void*, uint64_t symbol) {
    if (symbol == facts::CS_SYM_R14NETPHYSICS) return g_physCs ? g_physCs->bytes : nullptr;
    if (symbol == facts::CS_SYM_R14REMOTEPLAYER) return g_playerCs ? g_playerCs->bytes : nullptr;
    if (symbol == replay::localpawn_facts::kSymPlayerBroadcaster)
        return g_bcastCs ? g_bcastCs->bytes : nullptr;
    if (symbol == replay::localpawn_facts::kSymEchoUnit) return g_echoCs ? g_echoCs->bytes : nullptr;
    if (symbol == replay::localpawn_facts::kSymBody)     return g_bodyCs ? g_bodyCs->bytes : nullptr;
    if (symbol == replay::localpawn_facts::kSymNcaModel) return g_modelCs ? g_modelCs->bytes : nullptr;
    if (symbol == replay::localpawn_facts::kSymTouchAnimBlenders)
        return g_touchCs ? g_touchCs->bytes : nullptr;
    if (symbol == replay::localpawn_facts::kSymR14Frisbee)
        return g_frisbeeCs ? g_frisbeeCs->bytes : nullptr;
    return nullptr;
}

// modreg::InitAll is once per process, so the file shares one environment and every case drives the
// binding from a known state through Tick().
struct Env {
    RecordingLog log;
    DefaultConfig config;
    NoTick tick;
    replay::ReplayServices services{};
    size_t failures = 0;

    Env() {
        services.log = &log;
        services.config = &config;
        services.tick = &tick;
        replay::modreg::InitAll(&services);
        failures = replay::modreg::InitFailures();
    }

    void Tick(void* netGame) {
        replay::TickInfo info{};
        info.frameIndex = 1;
        info.netGame = netGame;
        replay::modreg::TickAll(info);
    }
};

Env& TheEnv() {
    static Env env;
    return env;
}

using replay::tableguard::TableWhy;

std::string Deferred(TableWhy why) {
    return std::string("deferred: ") + replay::tableguard::WhyName(why);
}

bool Registered(const char* name) {
    for (size_t i = 0; i < replay::modreg::Count(); ++i)
        if (strcmp(replay::modreg::At(i)->name, name) == 0) return true;
    return false;
}

}  // namespace

TEST_CASE("the entity module registers, and Init publishes IEntity and IPosePublisher") {
    Env& e = TheEnv();
    REQUIRE(Registered("entity"));
    CHECK(e.failures == 0);

    REQUIRE(e.services.entity != nullptr);
    REQUIRE(e.services.pose != nullptr);
    CHECK_FALSE(e.services.entity->Available());
    CHECK(std::string(e.services.pose->Strategy()) == "PP-B");
    CHECK_FALSE(e.services.pose->NeedsLateTick());
    // The singleton is what was published: the publisher refuses any other entity, so a second
    // instance would have left pose null.
    CHECK(replay::entity::PosePublisherCreate(e.services.entity, replay::entity::PoseStrategy::PpB) ==
          e.services.pose);
    CHECK(e.services.asSeen == nullptr);

    // The `cam look at <n>` composer is installed by THIS Init, and by nothing in src/camera/:
    // W2-E's seam is null in an image that does not carry this module, and the command refuses
    // SubjectResolve::NoSeam by name rather than aiming at nothing. Asserted through the seam
    // camera_control actually holds, so deleting the install line reds here as well as in
    // test_entity_subject.cpp's source read.
    CHECK(replay::camera_control::InstalledSubjectPose() == &replay::entity::kSubjectPose);
    // And the pools half is pointed at the singleton that was just published, unbound.
    CHECK(replay::entity::SubjectEntity() == e.services.entity);
}

TEST_CASE("a tick with a constructed game space binds the entity and publishes IAsSeen") {
    Env& e = TheEnv();
    FakeSpace space;
    space.Construct(space.elements);
    FakeNetGame netGame;
    netGame.PointAt(&space);

    const size_t boundBefore = e.log.Count("bound game space");
    e.Tick(netGame.bytes);

    REQUIRE(e.services.asSeen != nullptr);
    CHECK(e.services.entity != nullptr);
    CHECK(e.log.Count("bound game space") == boundBefore + 1);
    // This host maps no echovr.exe, so the adapter refuses at its first site guard rather than
    // touching the fake space; the module still reached it and said why.
    CHECK_FALSE(e.services.entity->Available());
    CHECK(std::string(e.services.entity->Diagnostic()) ==
          replay::entity::ResolveText(replay::entity::Resolve::NoPhysicsCs));

    // Same identity, next tick: no re-bind.
    e.Tick(netGame.bytes);
    CHECK(e.log.Count("bound game space") == boundBefore + 1);

    e.Tick(nullptr);
    CHECK(e.services.asSeen == nullptr);
    CHECK(e.log.Count("unbound game space") >= 1);
}

TEST_CASE("a space whose table is not constructed is deferred, not bound, and is retried") {
    Env& e = TheEnv();
    FakeSpace space;
    FakeNetGame netGame;
    netGame.PointAt(&space);

    const size_t boundBefore = e.log.Count("bound game space");

    // Allocated, never constructed: all zero. Then each single inconsistency the constructor's
    // postcondition rules out, named by the shared check's own word so the log line and the
    // refusal cannot drift apart.
    e.Tick(netGame.bytes);
    CHECK(e.services.asSeen == nullptr);
    CHECK(e.log.Count("deferred: ") == 1);

    space.Construct(space.elements);
    space.Put(0x30, 5);  // count > capacity
    e.Tick(netGame.bytes);
    CHECK(e.services.asSeen == nullptr);
    CHECK(e.log.Count(Deferred(TableWhy::CountOverCapacity).c_str()) == 1);

    const size_t emptyBefore = e.log.Count(Deferred(TableWhy::Empty).c_str());
    space.Construct(space.elements);
    space.Put(0x30, 0);  // built, nothing installed: a search finds nothing, so not yet
    e.Tick(netGame.bytes);
    CHECK(e.services.asSeen == nullptr);
    CHECK(e.log.Count(Deferred(TableWhy::Empty).c_str()) == emptyBefore + 1);

    space.Construct(space.elements);
    space.Put(0x00, ~0ull);  // the constructor's no-block marker, which a null test never sees
    e.Tick(netGame.bytes);
    CHECK(e.services.asSeen == nullptr);
    CHECK(e.log.Count(Deferred(TableWhy::NoBlock).c_str()) == 1);

    space.Construct(space.elements);
    space.Put(0x00, 0x10);  // a base that is not mapped
    e.Tick(netGame.bytes);
    CHECK(e.services.asSeen == nullptr);
    CHECK(e.log.Count(Deferred(TableWhy::ArrayUnreadable).c_str()) == 1);

    space.Construct(space.elements);
    space.Put(0x38, 1);  // consistent, but the engine has not sorted it: its lookup would sort in place
    e.Tick(netGame.bytes);
    CHECK(e.services.asSeen == nullptr);
    CHECK(e.log.Count("deferred: system table unsorted") == 1);

    CHECK(e.log.Count("bound game space") == boundBefore);

    // Not latched: the same space becoming ready binds on the next tick.
    space.Construct(space.elements);
    e.Tick(netGame.bytes);
    CHECK(e.services.asSeen != nullptr);
    CHECK(e.log.Count("bound game space") == boundBefore + 1);

    e.Tick(nullptr);
}

TEST_CASE("a rebuilt table behind the same space address, or a new space, re-binds") {
    Env& e = TheEnv();
    FakeSpace space;
    space.Construct(space.elements);
    FakeNetGame netGame;
    netGame.PointAt(&space);

    const size_t boundBefore = e.log.Count("bound game space");
    e.Tick(netGame.bytes);
    REQUIRE(e.log.Count("bound game space") == boundBefore + 1);

    // AVFIX F3: same space address, a table rebuilt into a different block holding different
    // systems. The systems the table NAMES are the identity, not the space pointer and not the
    // block address -- a base that moves while the entries do not is (b) only, the case below.
    memset(space.elementsRebuilt, 0x5C, sizeof space.elementsRebuilt);
    space.Construct(space.elementsRebuilt);
    e.Tick(netGame.bytes);
    CHECK(e.log.Count("bound game space") == boundBefore + 2);
    CHECK(e.log.Count("re-binding") >= 1);
    CHECK(e.services.asSeen != nullptr);

    FakeSpace second;
    second.Construct(second.elements);
    netGame.PointAt(&second);
    e.Tick(netGame.bytes);
    CHECK(e.log.Count("bound game space") == boundBefore + 3);

    // The space going away drops the binding without a write into it.
    netGame.PointAt(nullptr);
    e.Tick(netGame.bytes);
    CHECK(e.services.asSeen == nullptr);
    CHECK_FALSE(e.services.entity->Available());

    e.Tick(nullptr);
}

TEST_CASE("an unreadable netGame slot is refused before it is read") {
    Env& e = TheEnv();
    const size_t boundBefore = e.log.Count("bound game space");
    e.Tick(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10)));
    CHECK(e.services.asSeen == nullptr);
    CHECK(e.log.Count("bound game space") == boundBefore);
    e.Tick(nullptr);
}

// The adapter's own guard, for a caller that is not this module: the table check runs before the
// first call into the engine, so an unconstructed space is a named refusal rather than a crash, and a
// constructed one gets as far as the site guard (which this host fails, by design).
TEST_CASE("EntityCreate itself refuses a space whose table is not ready, before any site guard") {
    Env& e = TheEnv();
    e.Tick(nullptr);
    FakeSpace space;
    replay::IEntity* entity = replay::entity::EntityCreate(space.bytes);
    REQUIRE(entity == e.services.entity);
    CHECK_FALSE(entity->Available());
    CHECK(std::string(entity->Diagnostic()) ==
          replay::entity::ResolveText(replay::entity::Resolve::SpaceNotReady));

    space.Construct(space.elements);
    space.Put(0x38, 1);  // unsorted: the engine's lookup would sort in place
    replay::entity::EntityCreate(space.bytes);
    CHECK(std::string(entity->Diagnostic()) ==
          replay::entity::ResolveText(replay::entity::Resolve::SpaceNotReady));

    space.Construct(space.elements);
    replay::entity::EntityCreate(space.bytes);
    CHECK(std::string(entity->Diagnostic()) ==
          replay::entity::ResolveText(replay::entity::Resolve::NoPhysicsCs));

    replay::entity::EntityCreate(nullptr);
    CHECK(std::string(entity->Diagnostic()) ==
          replay::entity::ResolveText(replay::entity::Resolve::NoGameSpace));
}

// The auditor's own case. CSREGISTER-EPOCH S0's transition is a same-size
// allocation with LIFO reuse: the block comes back, the constructor installs the same number of
// systems, and (base, capacity, count) is re-established EQUAL. The header triple is S1.3's (b) and
// cannot see it. What can is (a) -- the entries the engine's search reads.
TEST_CASE("AUDIT F1: a table rebuilt into the same block with the same cap/count re-binds") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    memset(space.elements, 0xA1, sizeof space.elements);
    space.Construct(space.elements);
    FakeNetGame netGame;
    netGame.PointAt(&space);

    const size_t boundBefore = e.log.Count("bound game space");
    e.Tick(netGame.bytes);
    REQUIRE(e.log.Count("bound game space") == boundBefore + 1);

    // Same block, same base, same capacity, same count. Only the systems it names are new.
    memset(space.elements, 0xB2, sizeof space.elements);
    space.Construct(space.elements);
    CHECK(space.Get(0x00) == reinterpret_cast<uintptr_t>(space.elements));
    CHECK(space.Get(0x28) == 4);
    CHECK(space.Get(0x30) == 1);
    e.Tick(netGame.bytes);
    CHECK(e.log.Count("bound game space") == boundBefore + 2);
    CHECK(e.log.Count("re-binding") >= 1);

    // The other half of S1.3: (b) moving on its own is REPORTED, never acted on. A table copied to
    // a new block still names the same systems, and re-binding on that alone would drop the owner
    // ledger for nothing.
    const size_t movedBefore = e.log.Count("system table moved");
    memcpy(space.elementsRebuilt, space.elements, sizeof space.elements);
    space.Construct(space.elementsRebuilt);
    e.Tick(netGame.bytes);
    CHECK(e.log.Count("bound game space") == boundBefore + 2);
    CHECK(e.log.Count("system table moved") == movedBefore + 1);

    e.Tick(nullptr);
}

// Bind used to latch on the attempt rather than on the result, so any refusal
// below the table check -- PoolNotReady is the in-game one -- was permanent for the life of the
// space. On this host every EntityCreate refuses at the site guard, which makes it exactly the
// stuck state F2 describes: the module must keep asking, and must say how many times and why.
TEST_CASE("AUDIT F2: a bind whose entity did not resolve is retried every tick, not latched") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    FakeNetGame netGame;
    netGame.PointAt(&space);

    const size_t boundBefore = e.log.Count("bound game space");
    const size_t stuckBefore = e.log.Count("still unresolved after");
    e.Tick(netGame.bytes);
    REQUIRE(e.log.Count("bound game space") == boundBefore + 1);
    REQUIRE_FALSE(e.services.entity->Available());

    // The bind counts as refusal 1; 599 more ticks reach the throttle. A latch reaches nothing.
    for (int i = 0; i < 599; ++i) e.Tick(netGame.bytes);
    CHECK(e.log.Count("still unresolved after") == stuckBefore + 1);
    CHECK(e.log.Count("still unresolved after 600 attempts") == 1);
    // Retrying is not re-binding: no second bind line, and the binding is not churned.
    CHECK(e.log.Count("bound game space") == boundBefore + 1);

    e.Tick(nullptr);
}

// `*this = Entity()` at the head of Entity::Init is the only thing that keeps a
// saved owner handle from an old pool out of a NEW pool at RestoreAll, and removing it left the
// suite green. This is the assertion that sees it: the write RestoreAll would make lands in memory
// the test owns and can read back.
TEST_CASE("AUDIT F3: a second Init drops the owner ledger, so RestoreAll cannot reach the new pool") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);

    FakeCs physA, playerA;
    physA.WirePhys();
    playerA.WirePlayer();
    const uint32_t kOriginalA = 0xA1A2A3A4u;
    physA.Owner(0) = kOriginalA;
    g_physCs = &physA;
    g_playerCs = &playerA;

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());
    entity->SetSyntheticOwner(replay::PhysHandle(0));
    REQUIRE(physA.Owner(0) != kOriginalA);   // taken over: the ledger now holds {0, kOriginalA}

    // The rebuilt space: new pools, and handle 0 names a different body with a different owner.
    FakeCs physB, playerB;
    physB.WirePhys();
    playerB.WirePlayer();
    const uint32_t kOriginalB = 0xB1B2B3B4u;
    physB.Owner(0) = kOriginalB;
    g_physCs = &physB;
    g_playerCs = &playerB;
    REQUIRE(replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind)->Available());

    replay::entity::EntityDestroy(entity);   // RestoreAll
    CHECK(physB.Owner(0) == kOriginalB);     // without Init's reset this reads kOriginalA
    CHECK(physA.Owner(0) != kOriginalA);     // and the old pool is deliberately not written into

    g_physCs = nullptr;
    g_playerCs = nullptr;
    replay::entity::EntityCreate(nullptr);   // leave the singleton unresolved for whoever runs next
}

// The seam ENG-USERID-SEAM S8 asked W1-E for. W2-B's OwnerGuard holds the replay session's own user
// id at a placeholder 0 because nothing produced it, and OwnerGuard::Classify refuses `id == local_`
// -- so a genuine engine UserIndex of 0 is refused as is-local-user and that player never publishes.
// The whole point of the accessor is therefore that a REAL 0 and NO ANSWER are different values, and
// that is what these cases pin.
TEST_CASE("the local user index is a validated read, and absence is not 0") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);

    FakeCs phys, player, bcast;
    phys.WirePhys();
    player.WirePlayer();
    bcast.WirePlayer();   // the broadcaster CS has CR15RemotePlayerCS's layout (W1-E.md S10.2)
    g_physCs = &phys;
    g_playerCs = &player;

    // Resolved, but this build gave the local pawn no broadcaster: NOT a user index of 0.
    g_bcastCs = nullptr;
    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());
    replay::UserIndex out(0xEEEEu);
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::NotAvailable);
    CHECK(out.v == 0xEEEEu);   // untouched: the accessor writes only for Ok

    // One authored player whose engine user id IS 0 -- the case the placeholder swallowed.
    g_bcastCs = &bcast;
    const uint32_t kUserIdOff = replay::localpawn_facts::kBroadcasterUserIdOff;
    memset(bcast.elements, 0, sizeof bcast.elements);
    *reinterpret_cast<uint32_t*>(bcast.elements + kUserIdOff) = 0u;
    REQUIRE(replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind)->Available());
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::Ok);
    CHECK(out.v == 0);

    // A non-zero id reads back as itself, so Ok is not a constant.
    *reinterpret_cast<uint32_t*>(bcast.elements + kUserIdOff) = 5u;
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::Ok);
    CHECK(out.v == 5);

    // Nobody authored here, and more than one authored: each its own answer, neither an index.
    bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
               facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 0);
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::None);
    bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
               facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 2);
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::Ambiguous);
    CHECK(out.v == 5);   // still the last Ok: no shape but Ok writes it

    // An unresolved entity answers NotAvailable, not None and not 0.
    g_physCs = nullptr;
    g_playerCs = nullptr;
    g_bcastCs = nullptr;
    replay::entity::EntityCreate(nullptr);
    CHECK_FALSE(entity->Available());
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::NotAvailable);
}

// ENG-W1E-R2 S9's open, closed: on the `-offline` in-client host -- the wave-2 launch
// configuration -- the broadcaster pool authors the human AND every bot, so it is Ambiguous by
// DEFAULT and W2-B's is-local-user arm never arms. CR15NetGame's own local-user slot is the second
// source, and these cases pin every way the two can combine. Absence still never becomes 0.
TEST_CASE("the netGame slot answers where the broadcaster pool cannot, and a disagreement is not an answer") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);

    FakeCs phys, player, bcast;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;
    g_bcastCs = &bcast;

    FakeNetGame netGame;
    netGame.PointAt(&space);
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, netGame.bytes);

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());

    // A bot host: two authored players, so the pool cannot name the human. The netGame can.
    const auto WirePool = [&bcast](uint16_t count) {
        bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
                   facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, count);
    };
    WirePool(2);
    netGame.SetLocalUser(3);

    replay::UserIndex out(0xEEEEu);
    replay::IEntity::LocalUserReading r;
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ok);
    CHECK(r.slotOk);
    CHECK(r.slot.v == 3);
    CHECK_FALSE(r.poolOk);
    CHECK(r.poolCount == 2);
    CHECK(r.index.v == 3);
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::Ok);
    CHECK(out.v == 3);

    // Slot 0 is a real answer on this path too, and it is the one a placeholder would swallow.
    netGame.SetLocalUser(0);
    out = replay::UserIndex(0xEEEEu);
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::Ok);
    CHECK(out.v == 0);

    // Take the second source away and the bot host is back to Ambiguous -- `out` untouched.
    netGame.NoLocalUser();
    out = replay::UserIndex(0xEEEEu);
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ambiguous);
    CHECK_FALSE(r.slotOk);
    CHECK(r.poolCount == 2);
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::Ambiguous);
    CHECK(out.v == 0xEEEEu);

    // Both absent: no local user in the netGame and nobody authored. NotAvailable, never 0.
    WirePool(0);
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::None);
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, nullptr);
    g_bcastCs = nullptr;
    REQUIRE(replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind)->Available());
    out = replay::UserIndex(0xEEEEu);
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::NotAvailable);
    CHECK_FALSE(r.poolOk);
    CHECK_FALSE(r.slotOk);
    CHECK(r.index.v == 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::NotAvailable);
    CHECK(out.v == 0xEEEEu);

    // Both answer and they DISAGREE. Neither wins, and the reading names both numbers.
    g_bcastCs = &bcast;
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, netGame.bytes);
    REQUIRE(replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind)->Available());
    WirePool(1);
    memset(bcast.elements, 0, sizeof bcast.elements);
    *reinterpret_cast<uint32_t*>(bcast.elements + lpf::kBroadcasterUserIdOff) = 5u;
    netGame.SetLocalUser(3);
    out = replay::UserIndex(0xEEEEu);
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ambiguous);
    CHECK(r.poolOk);
    CHECK(r.pool.v == 5);
    CHECK(r.slotOk);
    CHECK(r.slot.v == 3);
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::Ambiguous);
    CHECK(out.v == 0xEEEEu);

    // Both answer and AGREE: one answer, and it came from two places.
    netGame.SetLocalUser(5);
    out = replay::UserIndex(0xEEEEu);
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ok);
    CHECK(r.poolOk);
    CHECK(r.slotOk);
    CHECK(entity->LocalUserIndex(out) == replay::IEntity::LocalUser::Ok);
    CHECK(out.v == 5);

    g_physCs = nullptr;
    g_playerCs = nullptr;
    g_bcastCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// Each refusal inside the netGame read, separately, because three of the four would otherwise be
// covered only by the case where the whole group is blank -- and a blank fixture passes a check
// that reads the wrong field just as happily as one that reads the right one.
TEST_CASE("the netGame local-user read refuses a slot it cannot cross-check, and never returns 0 instead") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    FakeCs phys, player, bcast;
    phys.WirePhys();
    player.WirePlayer();
    bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
               facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 2);   // pool: no answer
    g_physCs = &phys;
    g_playerCs = &player;
    g_bcastCs = &bcast;

    FakeNetGame netGame;
    netGame.PointAt(&space);
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, netGame.bytes);
    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());

    replay::IEntity::LocalUserReading r;
    netGame.SetLocalUser(4);
    REQUIRE(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ok);   // the control

    // A slot past the 16-entry table both owner consumers bound-check.
    netGame.SetLocalUser(17);
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ambiguous);
    CHECK_FALSE(r.slotOk);

    // Two local users: record 0 no longer names a single human.
    netGame.SetLocalUser(4, 2);
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ambiguous);
    CHECK_FALSE(r.slotOk);

    // The slot table points somewhere other than record 0, so "record 0 is the local user" is not
    // true of this netGame and the read says so rather than handing back the slot anyway.
    netGame.SetLocalUser(4);
    netGame.Put16(lpf::kNetGameSlotTableOff + lpf::kNetGameSlotTableStride * 4, 1);
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ambiguous);
    CHECK_FALSE(r.slotOk);

    // An unreadable netGame is refused before it is read.
    netGame.SetLocalUser(4);
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, reinterpret_cast<void*>(uintptr_t(0x10)));
    CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ambiguous);
    CHECK_FALSE(r.slotOk);

    g_physCs = nullptr;
    g_playerCs = nullptr;
    g_bcastCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// The interface addition carries its own default, and the default is the contract's "this build
// could not source it" -- not a fabricated agreement between two sources it never read.
TEST_CASE("an IEntity that predates the second source says NotAvailable, and names neither reading") {
    struct NoSources final : replay::IEntity {
        size_t PhysCount() const override { return 0; }
        bool   IsLocallyControlled(replay::PhysHandle) const override { return false; }
        void   SetSyntheticOwner(replay::PhysHandle) override {}
        void   ClearSyntheticOwner(replay::PhysHandle) override {}
        bool   ReadBody(replay::PhysHandle, replay::BodyState&) const override { return false; }
        void   PushHistory(replay::PhysHandle, const replay::BodyState&, uint64_t) override {}
        void   TrimHistory(replay::PhysHandle, uint64_t, uint64_t) override {}
        float  GetTimeOffset(replay::PhysHandle) const override { return 0.0f; }
        void   SetTimeOffset(replay::PhysHandle, float) override {}
        size_t PlayerCount() const override { return 0; }
        bool   ReadPlayer(replay::PlayerHandle, replay::PlayerState&) const override { return false; }
        void   PushPlayerHistory(replay::PlayerHandle, const replay::PlayerState&, uint64_t) override {}
        replay::PhysHandle   PhysOf(replay::PlayerHandle) const override { return replay::PhysHandle(0xFFFFu); }  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        replay::PlayerHandle PlayerOf(replay::UserIndex) const override { return replay::PlayerHandle(0xFFFFu); }  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        void   SetLodThrottle(bool) override {}
        bool   Available() const override { return false; }
        const char* Diagnostic() const override { return "fake"; }
    };
    NoSources fake;
    replay::IEntity::LocalUserReading r;
    r.slotOk = true;   // the default must OVERWRITE, not leave a caller's stale reading standing
    r.poolOk = true;
    CHECK(fake.ReadLocalUser(r) == replay::IEntity::LocalUser::NotAvailable);
    CHECK_FALSE(r.slotOk);
    CHECK_FALSE(r.poolOk);
    CHECK(r.index.v == 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
}

// AUDIT_WIRING_A F-A. What W2-B needs and could not have: a signal that changes once per bind and
// nothing else. The entity POINTER cannot be it (one process-lifetime singleton) and Available()
// cannot be it (Tick re-binds a changed space by calling Bind with no Unbind, so a rebuilt space
// reads available before and after). Every count below is RELATIVE: the epoch is process-global by
// construction and other cases in this file bind too.
TEST_CASE("the bind epoch advances once per binding, and only on a binding") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    replay::IEntity* const ent = e.services.entity;
    REQUIRE(ent != nullptr);

    uint64_t epoch = 0xEEEEu;
    // Unbound is not epoch 0 and not NotAvailable: three answers, three states.
    CHECK(ent->BindEpochNow(epoch) == replay::IEntity::BindEpoch::Unbound);
    CHECK(epoch == 0xEEEEu);   // `out` is written only for Ok

    FakeSpace first;
    memset(first.elements, 0xC3, sizeof first.elements);
    first.Construct(first.elements);
    FakeNetGame netGame;
    netGame.PointAt(&first);

    e.Tick(netGame.bytes);
    uint64_t one = 0;
    REQUIRE(ent->BindEpochNow(one) == replay::IEntity::BindEpoch::Ok);
    CHECK(one >= 1);

    // Same identity, twice more. The retry path re-Inits the SAME space every tick on this host --
    // no echovr.exe is mapped, so the entity never resolves -- and not one of those is a rebuild.
    const size_t boundBefore = e.log.Count("bound game space");
    e.Tick(netGame.bytes);
    e.Tick(netGame.bytes);
    uint64_t held = 0;
    REQUIRE(ent->BindEpochNow(held) == replay::IEntity::BindEpoch::Ok);
    CHECK(held == one);
    CHECK(e.log.Count("bound game space") == boundBefore);

    // A different space: +1, once.
    FakeSpace second;
    memset(second.elements, 0xD4, sizeof second.elements);
    second.Construct(second.elements);
    netGame.PointAt(&second);
    e.Tick(netGame.bytes);
    uint64_t two = 0;
    REQUIRE(ent->BindEpochNow(two) == replay::IEntity::BindEpoch::Ok);
    CHECK(two == one + 1);

    // ★ The digest case, which is the whole reason the epoch is not keyed on the space address:
    // the same block, the same base/capacity/count, different systems inside it. F1's transition.
    memset(second.elements, 0xE5, sizeof second.elements);
    second.Construct(second.elements);
    REQUIRE(second.Get(0x00) == reinterpret_cast<uintptr_t>(second.elements));
    REQUIRE(second.Get(0x28) == 4);
    REQUIRE(second.Get(0x30) == 1);
    e.Tick(netGame.bytes);
    uint64_t three = 0;
    REQUIRE(ent->BindEpochNow(three) == replay::IEntity::BindEpoch::Ok);
    CHECK(three == two + 1);

    // Unbinding stops the epoch being current without rewinding it, and the next bind is a NEW
    // generation rather than a return to the old one -- a consumer comparing generations must
    // never see a value it has already retired.
    e.Tick(nullptr);
    uint64_t after = 0x1234u;
    CHECK(ent->BindEpochNow(after) == replay::IEntity::BindEpoch::Unbound);
    CHECK(after == 0x1234u);
    e.Tick(netGame.bytes);
    uint64_t four = 0;
    REQUIRE(ent->BindEpochNow(four) == replay::IEntity::BindEpoch::Ok);
    CHECK(four == three + 1);

    e.Tick(nullptr);
}

// An implementer that predates the epoch answers NotAvailable, which a consumer must not read as
// "epoch 0". The default is the contract's own "this build could not source it", the same shape
// LocalUserIndex and ReadLocalPlayer use.
TEST_CASE("an IEntity that does not track binds says so, and does not say zero") {
    struct NoEpoch final : replay::IEntity {
        size_t PhysCount() const override { return 0; }
        bool   IsLocallyControlled(replay::PhysHandle) const override { return false; }
        void   SetSyntheticOwner(replay::PhysHandle) override {}
        void   ClearSyntheticOwner(replay::PhysHandle) override {}
        bool   ReadBody(replay::PhysHandle, replay::BodyState&) const override { return false; }
        void   PushHistory(replay::PhysHandle, const replay::BodyState&, uint64_t) override {}
        void   TrimHistory(replay::PhysHandle, uint64_t, uint64_t) override {}
        float  GetTimeOffset(replay::PhysHandle) const override { return 0.0f; }
        void   SetTimeOffset(replay::PhysHandle, float) override {}
        size_t PlayerCount() const override { return 0; }
        bool   ReadPlayer(replay::PlayerHandle, replay::PlayerState&) const override { return false; }
        void   PushPlayerHistory(replay::PlayerHandle, const replay::PlayerState&, uint64_t) override {}
        replay::PhysHandle   PhysOf(replay::PlayerHandle) const override { return replay::PhysHandle(0xFFFFu); }  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        replay::PlayerHandle PlayerOf(replay::UserIndex) const override { return replay::PlayerHandle(0xFFFFu); }  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        void   SetLodThrottle(bool) override {}
        bool   Available() const override { return false; }
        const char* Diagnostic() const override { return "fake"; }
    };
    NoEpoch fake;
    uint64_t out = 0xABCDu;
    CHECK(fake.BindEpochNow(out) == replay::IEntity::BindEpoch::NotAvailable);
    CHECK(out == 0xABCDu);
}


// =========================================================================================
// AUDIT_K1 K1-3. The five Readable guards 1a4fcb0 added, each driven to BOTH outcomes -- the
// refusal AND the pass-through, which is the half whose failure is silent: an element the guard
// wrongly rejects makes the entity layer refuse on every tick and is discovered at a launch, as
// "the replay shows nothing".
// =========================================================================================
namespace {

// Nothing on the stack or the heap can be made unreadable, so the refusal half needs its own
// mapping: two pages reserved, the FIRST committed. An address in page two is committed by nothing,
// which is exactly what Readable exists to refuse. Every refusal case below places its object a
// couple of bytes short of the boundary, so the read the guard prevents is a FAULT rather than a
// wrong number -- that is what makes each of these cases prove its guard.
struct PagePair {
    uint8_t* base = nullptr;
    size_t   page = 0;
    PagePair() {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        page = si.dwPageSize;
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE, PAGE_NOACCESS));
        if (base && !VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE)) base = nullptr;
    }
    ~PagePair() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
    PagePair(const PagePair&) = delete;
    PagePair& operator=(const PagePair&) = delete;
    uint8_t* Ok(size_t off = 0) const { return base + off; }
    uint8_t* Straddling(size_t inside) const { return base + page - inside; }
    void Decommit() { VirtualFree(base, page, MEM_DECOMMIT); }
};

struct FakeNetActorCs {
    alignas(16) uint8_t bytes[0x200] = {};
    void SetTable(const void* p) {
        memcpy(bytes + replay::localpawn_facts::kNetActorTableOff, &p, sizeof p);
    }
    void SetCount(uint64_t n) {
        memcpy(bytes + replay::localpawn_facts::kNetActorCountOff, &n, sizeof n);
    }
};

// The engine entry points, as the seam takes them. FirstActorComponent answers per component
// system, because one function serves them all.
void* g_findEcho = nullptr;
void* g_findModel = nullptr;

void* FakeFindComponent(void* cs, void* out, uint64_t, uint64_t) {
    uint32_t id = replay::localpawn_facts::kNoComponent;
    if (cs != nullptr && (cs == g_findEcho || cs == g_findModel)) id = 0u;
    memcpy(out, &id, sizeof id);
    return out;
}
const void* FakeUserActor(void*, void* out16, uint16_t slot) {
    memset(out16, 0, replay::localpawn_facts::kActorGuidSize);
    memcpy(out16, &slot, sizeof slot);
    return out16;
}
void* FakeTransform(void*, void* out, uint16_t) { memset(out, 0, 48); return out; }
void* FakeHandTransform(void*, void* out, uint16_t, uint32_t) { memset(out, 0, 48); return out; }
// Never called: the fixture's attachment carries LOCAL.NO_CONSTRAINT, which is the arm that does
// not ask the solver. It has to be non-null all the same, because the whole anchor block -- and
// with it the echo element's span check -- is gated on this pointer.
uint32_t FakeAnchor(void*, uint64_t) { return 0u; }

void ClearLeafSystems() {
    g_echoCs = nullptr;
    g_bodyCs = nullptr;
    g_modelCs = nullptr;
    g_findEcho = nullptr;
    g_findModel = nullptr;
}

}  // namespace

TEST_CASE("K1-3: the net-actor table span is checked both ways, and not once per call") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    FakeCs phys, player;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;

    FakeNetActorCs net;
    phys.PutPtr(lpf::kPhysCsNetActorOff, net.bytes);

    PagePair pp;
    REQUIRE(pp.Ok() != nullptr);
    const uint64_t kNetId = 0x123456789ABCull;
    const uint32_t kCompId = 0x00070003u;   // generation in the high word, phys handle 3 in the low
    const auto Plant = [&](uint8_t* at, size_t entries) {
        memset(at, 0, lpf::kNetActorEntryStride * entries);
        memcpy(at + lpf::kNetActorEntryStride, &kNetId, sizeof kNetId);
        memcpy(at + lpf::kNetActorEntryStride + lpf::kNetActorCompIdOff, &kCompId, sizeof kCompId);
    };
    uint8_t* const table = pp.Ok();
    Plant(table, 2);
    net.SetTable(table);
    net.SetCount(2);

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());

    SUBCASE("an in-window table still answers -- the pass-through half") {
        CHECK(entity->NetIdOf(replay::PhysHandle(3)) == kNetId);
        CHECK(entity->PhysOfNetId(kNetId).v == 3);
        // A body with no entry: the table was READ and had nothing, which is a different fact from
        // the table being unreadable, and both are 0 here by design.
        CHECK(entity->NetIdOf(replay::PhysHandle(9)) == 0);
    }

    SUBCASE("a table whose span leaves the mapping refuses, and does not fault") {
        net.SetTable(pp.Straddling(8));
        CHECK(entity->NetIdOf(replay::PhysHandle(3)) == 0);
        CHECK(entity->PhysOfNetId(kNetId).v == 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }

    SUBCASE("the count is part of what was checked, so a grown one is checked again") {
        // The cache's whole hazard: the first call banks a verdict for a span, and the span the
        // next call would walk is a different one. The edge is exact -- two entries end ON the
        // boundary and read; three cross it and must not.
        uint8_t* const edge = pp.Straddling(lpf::kNetActorEntryStride * 2);
        Plant(edge, 2);
        net.SetTable(edge);
        net.SetCount(2);
        CHECK(entity->NetIdOf(replay::PhysHandle(3)) == kNetId);
        net.SetCount(3);
        CHECK(entity->NetIdOf(replay::PhysHandle(3)) == 0);
        net.SetCount(2);
        CHECK(entity->NetIdOf(replay::PhysHandle(3)) == kNetId);
    }

    SUBCASE("the verdict does not outlive the binding") {
        // Init -- which EntityCreate, EntityBind and EntityUnbind all run -- drops it with the rest
        // of the object. Same address, same count, and the pages under it are gone.
        PagePair doomed;
        REQUIRE(doomed.Ok() != nullptr);
        Plant(doomed.Ok(), 2);
        net.SetTable(doomed.Ok());
        net.SetCount(2);
        REQUIRE(entity->NetIdOf(replay::PhysHandle(3)) == kNetId);

        doomed.Decommit();
        REQUIRE(replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind)->Available());
        CHECK(entity->NetIdOf(replay::PhysHandle(3)) == 0);
    }

    SUBCASE("the span check is off the per-call path -- AUDIT_K1 open item 3") {
        // capture/gather.h calls NetIdOf once per PRESENT BODY per capture tick, and
        // playback/core/windowbuild.cpp calls PhysOfNetId once per transform row, so the
        // VirtualQuery this made per call was a syscall inside both loops. The two arms are
        // interleaved and the best of several repetitions is taken: one ordering on a busy machine
        // measures the machine.
        const int kCalls = 20000;
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        double cachedUs = 1e30, perCallUs = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            LARGE_INTEGER a{}, b{}, c{};
            uint64_t sink = 0;
            QueryPerformanceCounter(&a);
            for (int i = 0; i < kCalls; ++i) sink += entity->NetIdOf(replay::PhysHandle(3));
            QueryPerformanceCounter(&b);
            MEMORY_BASIC_INFORMATION mbi{};
            for (int i = 0; i < kCalls; ++i) sink += VirtualQuery(table, &mbi, sizeof mbi);
            QueryPerformanceCounter(&c);
            REQUIRE(sink != 0);
            const double one = double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart);
            const double two = double(c.QuadPart - b.QuadPart) * 1e6 / double(f.QuadPart);
            if (one < cachedUs) cachedUs = one;
            if (two < perCallUs) perCallUs = two;
        }
        MESSAGE("NetIdOf x" << kCalls << ": " << cachedUs << " us total ("
                << (cachedUs * 1000.0 / kCalls) << " ns/call); the bare VirtualQuery it used to add "
                << "to every one of them: " << perCallUs << " us ("
                << (perCallUs * 1000.0 / kCalls) << " ns/call)");
        // The whole lookup, table walk included, must cost less than the syscall alone did.
        CHECK(cachedUs < perCallUs);
    }

    g_physCs = nullptr;
    g_playerCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

TEST_CASE("K1-3: the broadcaster pool element's span is checked both ways") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    FakeCs phys, player, bcast;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;
    g_bcastCs = &bcast;
    bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
               facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 1);

    PagePair pp;
    REQUIRE(pp.Ok() != nullptr);
    memset(pp.Ok(), 0, lpf::kBroadcasterCiStride);
    *reinterpret_cast<uint32_t*>(pp.Ok() + lpf::kBroadcasterUserIdOff) = 7u;
    bcast.PutPtr(facts::PLAYER_CS_POOL_BASE_OFF, pp.Ok());

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());
    replay::IEntity::LocalUserReading r;

    SUBCASE("an in-window element still names its player") {
        REQUIRE(entity->LocalPlayerCount() == 1);
        CHECK(entity->LocalPlayerAt(0).v == 7);
        CHECK(entity->ReadLocalUser(r) == replay::IEntity::LocalUser::Ok);
        CHECK(r.poolOk);
        CHECK(r.pool.v == 7);
    }

    SUBCASE("an element whose stride runs off the mapping refuses, and does not fault") {
        // Two bytes inside the boundary: the user id at +4 is the read the guard prevents.
        bcast.PutPtr(facts::PLAYER_CS_POOL_BASE_OFF, pp.Straddling(2));
        REQUIRE(entity->LocalPlayerCount() == 1);
        CHECK(entity->LocalPlayerAt(0).v == 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        CHECK(entity->ReadLocalUser(r) != replay::IEntity::LocalUser::Ok);
        CHECK_FALSE(r.poolOk);
        CHECK(r.poolCount == 1);
    }

    g_physCs = nullptr;
    g_playerCs = nullptr;
    g_bcastCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

TEST_CASE("K1-3: the model and echo-unit pool elements are checked both ways") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    // gameSpace+8 is the CGame the anchor path needs before it will report kLocalAnchor at all.
    uint8_t game[64] = {};
    void* gamePtr = game;
    memcpy(space.bytes + lpf::kGameSpaceGameOff, &gamePtr, sizeof gamePtr);

    FakeCs phys, player, bcast, echo, body, model;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;
    g_bcastCs = &bcast;
    g_echoCs = &echo;
    g_bodyCs = &body;
    g_modelCs = &model;
    g_findEcho = echo.bytes;
    g_findModel = model.bytes;

    FakeNetGame netGame;
    netGame.PointAt(&space);
    netGame.SetLocalUser(7);
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, netGame.bytes);

    // One authored player, user id 7, with an actor key block ReadLocalPlayer can hand to the
    // component lookup.
    uint8_t keys[lpf::kActorGuidSize * 2] = {};
    bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
               facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 1);
    memset(bcast.elements, 0, sizeof bcast.elements);
    *reinterpret_cast<uint32_t*>(bcast.elements + lpf::kBroadcasterUserIdOff) = 7u;
    bcast.PutPtr(lpf::kBroadcasterKeysOff, keys);

    // The shifted field group for CModelCS, the unshifted one for the echo unit: two different
    // pairs, which is why LeafPool does not read the model system.
    model.PutPtr(lpf::kModelCsHandleMapOff, model.handleMap);
    echo.PutPtr(lpf::kCsHandleMapOff, echo.handleMap);

    PagePair pp;
    REQUIRE(pp.Ok() != nullptr);
    uint8_t* const modelCi = pp.Ok();
    uint8_t* const echoElem = pp.Ok(1024);
    memset(modelCi, 0, lpf::kModelCiStride);
    memset(echoElem, 0, lpf::kEchoCiStride);
    // The echo element's attachment, with no constraint: LOCAL.NO_CONSTRAINT is the case the solver
    // is deliberately not asked about, and kLocalAnchor is set anyway -- which is the bit that says
    // the ELEMENT was handed back.
    uint8_t attached[512] = {};
    const uint64_t noConstraint = lpf::kNoConstraint;
    memcpy(attached + lpf::kAttachedConstraintOff, &noConstraint, sizeof noConstraint);
    void* attachedPtr = attached;
    memcpy(echoElem + lpf::kEchoAttachedOff, &attachedPtr, sizeof attachedPtr);

    replay::entity::EngineEntryPoints fns;
    fns.findActorComponent = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeFindComponent);
    fns.userActor          = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeUserActor);
    fns.echoBaseTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeTransform);
    fns.echoHeadTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeTransform);
    fns.echoHandTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeHandTransform);
    fns.anchorBodyPart     = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeAnchor);

    SUBCASE("in-window elements still come back") {
        model.PutPtr(lpf::kModelCsPoolBaseOff, modelCi);
        echo.PutPtr(lpf::kCsPoolBaseOff, echoElem);
        replay::IEntity* entity =
            replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
        REQUIRE(entity->Available());

        bool visible = false;
        *reinterpret_cast<uint32_t*>(modelCi + lpf::kModelCiVisibleOff) = 1u;
        CHECK(entity->ModelVisible(replay::UserIndex(7), visible));
        CHECK(visible);
        // Sourced-and-false is a different answer from could-not-source, and only the return value
        // separates them.
        *reinterpret_cast<uint32_t*>(modelCi + lpf::kModelCiVisibleOff) = 0u;
        CHECK(entity->ModelVisible(replay::UserIndex(7), visible));
        CHECK_FALSE(visible);

        replay::PlayerState st{};
        const uint32_t filled = entity->ReadLocalPlayer(replay::UserIndex(7), st);
        CHECK((filled & replay::kLocalAnchor) != 0u);
    }

    SUBCASE("a straddling model pool refuses the draw-bit but presence holds; the echo pool refuses safely") {
        model.PutPtr(lpf::kModelCsPoolBaseOff, pp.Straddling(2));
        echo.PutPtr(lpf::kCsPoolBaseOff, pp.Straddling(2));
        replay::IEntity* entity =
            replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
        REQUIRE(entity->Available());

        // PRESENCE is the FirstActorComponent handle (stride-independent), so a straddling CI pool
        // cannot refuse it -- the model IS on the actor. The unratified-stride draw-bit read refuses
        // safely, so `visible` stays false while the return stays true, and neither faults.
        bool visible = true;
        CHECK(entity->ModelVisible(replay::UserIndex(7), visible));
        CHECK_FALSE(visible);

        replay::PlayerState st{};
        const uint32_t filled = entity->ReadLocalPlayer(replay::UserIndex(7), st);
        CHECK((filled & replay::kLocalAnchor) == 0u);
        // The refusal is the ELEMENT's, not the whole read's: the rest of the local player still
        // arrived, so a guard that had rejected everything would not look like this.
        CHECK((filled & replay::kLocalPose) != 0u);
    }

    ClearLeafSystems();
    g_physCs = nullptr;
    g_playerCs = nullptr;
    g_bcastCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// =========================================================================================
// ENG-MODEL-PUMP FIX C: the reader must PRESENT a slot whose ncaModel component is on its actor, and
// must tell a NULL model system (entity.cpp:2498) apart from a resolved system that carries no
// component on the actor (entity.cpp:2507) -- the conflation that made `modelCI=absent` unreadable.
// Presence is the FirstActorComponent handle, NOT the refuted 16-byte CI stride, so a present model
// reads present regardless of the CI pool layout.
// =========================================================================================
TEST_CASE("ENG-MODEL-PUMP FIX C: model presence is the handle, and null-CS is not no-component") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);

    FakeCs phys, player, bcast, echo, body, model;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;
    g_bcastCs = &bcast;
    g_echoCs = &echo;
    g_bodyCs = &body;

    FakeNetGame netGame;
    netGame.PointAt(&space);
    netGame.SetLocalUser(0);
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, netGame.bytes);
    model.PutPtr(lpf::kModelCsHandleMapOff, model.handleMap);
    model.PutPtr(lpf::kModelCsPoolBaseOff, model.elements);

    replay::entity::EngineEntryPoints fns;
    fns.findActorComponent = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeFindComponent);
    fns.userActor          = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeUserActor);

    // The local viewer (index 0), whose model renders offline: the correctness oracle is that it reads
    // PRESENT and its raw handle is not kNoComponent.
    SUBCASE("a component present on the actor reads present, with a real handle") {
        g_modelCs = &model;
        g_findModel = model.bytes;   // FakeFindComponent hands back handle 0 for this CS
        replay::IEntity* entity =
            replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
        REQUIRE(entity->Available());

        CHECK(entity->ModelSystemResolved());
        CHECK(entity->ModelComponentHandle(replay::UserIndex(0)) != lpf::kNoComponent);
        bool visible = false;
        CHECK(entity->ModelVisible(replay::UserIndex(0), visible));
    }

    // 2507: the system resolved, but the actor carries no ncaModel component.
    SUBCASE("a resolved system with no component on the actor reads absent, system still resolved") {
        g_modelCs = &model;
        g_findModel = nullptr;       // FakeFindComponent returns kNoComponent for this CS
        replay::IEntity* entity =
            replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
        REQUIRE(entity->Available());

        CHECK(entity->ModelSystemResolved());   // the system IS there ...
        CHECK(entity->ModelComponentHandle(replay::UserIndex(0)) == lpf::kNoComponent);
        bool visible = true;
        CHECK_FALSE(entity->ModelVisible(replay::UserIndex(0), visible));  // ... the component is not
        CHECK_FALSE(visible);
    }

    // 2498: the model system itself did not resolve -- a DIFFERENT absence from 2507, and the reader
    // must not report it as an absent component.
    SUBCASE("a null model system is distinguishable from a resolved-but-absent component") {
        g_modelCs = nullptr;         // find() returns null for ncaModel -> model_.cs null
        g_findModel = nullptr;
        replay::IEntity* entity =
            replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
        REQUIRE(entity->Available());

        CHECK_FALSE(entity->ModelSystemResolved());   // the discriminator vs the 2507 case above
        CHECK(entity->ModelComponentHandle(replay::UserIndex(0)) == lpf::kNoComponent);
        bool visible = true;
        CHECK_FALSE(entity->ModelVisible(replay::UserIndex(0), visible));
        CHECK_FALSE(visible);
    }

    ClearLeafSystems();
    g_physCs = nullptr;
    g_playerCs = nullptr;
    g_bcastCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// =========================================================================================
// PRESENT-G. WHAT THE ENGINE IS ASKED FOR, not just that it was asked.
//
// Four sessions read "this actor carries no EchoUnit" off a lookup that was handing the engine a
// stored GUID as its two arguments. The arguments are (actor id, game space), the space half must
// be -1 so the searcher substitutes the system's own, and the actor id is the GUID's SECOND qword.
// The fixtures above pass whatever they are given, which is exactly how the wrong shape survived
// them; this one answers ONLY for the pair the engine would have matched.
// =========================================================================================
namespace presentg_findshape {

uint64_t g_lastActorId = 0;
uint64_t g_lastSpace = 0;
uint32_t g_calls = 0;
uint64_t g_answerFor = 0;

void* RecordingFind(void* /*cs*/, void* out, uint64_t actorId, uint64_t space) {
    ++g_calls;
    g_lastActorId = actorId;
    g_lastSpace = space;
    uint32_t id = replay::localpawn_facts::kNoComponent;
    if (actorId == g_answerFor && space == replay::localpawn_facts::kThisGameSpace) id = 5u;
    memcpy(out, &id, sizeof id);
    return out;
}

// The shape CR15NetGame::UserActor leaves behind: one session-wide constant in the first qword and
// the actor id -- (index << 16 | class) -- in the second.
const void* ActorGuidUserRecord(void*, void* out16, uint16_t slot) {
    const uint64_t session = 0x3F9915D3001DC28Eull;
    const uint64_t actorId = (uint64_t(slot) << 16) | 0x3Bull;
    memcpy(out16, &session, sizeof session);
    memcpy(static_cast<uint8_t*>(out16) + 8, &actorId, sizeof actorId);
    return out16;
}

}  // namespace presentg_findshape

TEST_CASE("PRESENT-G: the actor lookup asks for (actor id, this game space), not the GUID's halves") {
    namespace lpf = replay::localpawn_facts;
    namespace pg = presentg_findshape;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);

    FakeCs phys, player, bcast, echo, body, model;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;
    g_bcastCs = &bcast;
    g_echoCs = &echo;
    g_bodyCs = &body;
    g_modelCs = &model;

    FakeNetGame netGame;
    netGame.PointAt(&space);
    netGame.SetLocalUser(7);
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, netGame.bytes);
    bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
               facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 1);
    memset(bcast.elements, 0, sizeof bcast.elements);
    *reinterpret_cast<uint32_t*>(bcast.elements + lpf::kBroadcasterUserIdOff) = 7u;
    echo.PutPtr(lpf::kCsHandleMapOff, echo.handleMap);
    model.PutPtr(lpf::kModelCsHandleMapOff, model.handleMap);

    replay::entity::EngineEntryPoints fns;
    fns.findActorComponent = reinterpret_cast<replay::entity::AnyEngineFn>(&pg::RecordingFind);
    fns.userActor = reinterpret_cast<replay::entity::AnyEngineFn>(&pg::ActorGuidUserRecord);

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
    REQUIRE(entity->Available());

    const uint16_t kSlot = 3;
    const uint64_t kActorId = (uint64_t(kSlot) << 16) | 0x3Bull;

    SUBCASE("the engine is handed the actor id and its own space, and the handle comes back") {
        pg::g_calls = 0;
        pg::g_answerFor = kActorId;
        CHECK(entity->EchoUnitComponentHandle(replay::UserIndex(kSlot)) == 5u);
        // Three asks, not one: the bind probe resolves EchoUnit, ncaModel and CR15Body on the same
        // actor, and each of the three took the right half first, so none needed a second ask.
        CHECK(pg::g_calls == 3);
        CHECK(pg::g_lastActorId == kActorId);
        CHECK(pg::g_lastSpace == lpf::kThisGameSpace);
    }

    SUBCASE("the other half is tried, and only after the first one missed") {
        // A producer that stores the halves the other way round still binds, at the cost of one
        // lookup, and the space half is -1 in that call too.
        pg::g_calls = 0;
        pg::g_answerFor = 0x3F9915D3001DC28Eull;
        CHECK(entity->EchoUnitComponentHandle(replay::UserIndex(kSlot)) == 5u);
        CHECK(pg::g_calls == 6);   // two halves each for EchoUnit, ncaModel and CR15Body
        CHECK(pg::g_lastActorId == 0x3F9915D3001DC28Eull);
        CHECK(pg::g_lastSpace == lpf::kThisGameSpace);
    }

    SUBCASE("an actor that carries nothing is two asks and a refusal, never the old pair") {
        pg::g_calls = 0;
        pg::g_answerFor = 0xD15C0;   // neither half, so nothing can answer
        CHECK(entity->EchoUnitComponentHandle(replay::UserIndex(kSlot)) == lpf::kNoComponent);
        CHECK(pg::g_calls == 6);
        // The pre-PRESENT-G call passed the actor id as the SPACE. Nothing does that any more.
        CHECK(pg::g_lastSpace != kActorId);
        CHECK(pg::g_lastSpace == lpf::kThisGameSpace);
    }

    ClearLeafSystems();
    g_physCs = nullptr;
    g_playerCs = nullptr;
    g_bcastCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// =========================================================================================
// The span check off the per-ELEMENT path.
//
// PERF counted 110 engine accessor calls per capture tick and measured one replay::Readable at
// 652 ns on this host: 71.7 us of a 30 us tick budget, uncontended, and every one of those calls
// proved its own element. The three pools now prove their range once per (base, extent, bind epoch)
// and touch the element under __try. The three properties below are the three ways that can be
// wrong -- too slow, stale, or unguarded -- and each has a mutant named in the report.
// =========================================================================================
TEST_CASE("A tick's worth of element reads costs no syscall, and picks up a moved pool") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);

    FakeCs phys, player, bcast, echo, body, model;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;
    g_bcastCs = &bcast;
    g_echoCs = &echo;
    g_bodyCs = &body;
    g_modelCs = &model;
    g_findEcho = echo.bytes;
    g_findModel = model.bytes;

    FakeNetGame netGame;
    netGame.PointAt(&space);
    netGame.SetLocalUser(7);
    bcast.PutPtr(lpf::kBroadcasterNetGameOff, netGame.bytes);
    uint8_t keys[lpf::kActorGuidSize * 2] = {};
    bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
               facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 1);
    memset(bcast.elements, 0, sizeof bcast.elements);
    *reinterpret_cast<uint32_t*>(bcast.elements + lpf::kBroadcasterUserIdOff) = 7u;
    bcast.PutPtr(lpf::kBroadcasterKeysOff, keys);
    model.PutPtr(lpf::kModelCsHandleMapOff, model.handleMap);
    echo.PutPtr(lpf::kCsHandleMapOff, echo.handleMap);

    replay::entity::EngineEntryPoints fns;
    fns.findActorComponent = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeFindComponent);
    fns.userActor          = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeUserActor);
    fns.echoBaseTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeTransform);
    fns.echoHeadTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeTransform);
    fns.echoHandTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeHandTransform);
    fns.anchorBodyPart     = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeAnchor);

    // Two model elements a page apart, so "the pool moved" is a real move to a real second object
    // and not the same bytes read twice.
    PagePair pp;
    REQUIRE(pp.Ok() != nullptr);
    uint8_t* const poolA = pp.Ok();
    uint8_t* const poolB = pp.Ok(2048);
    memset(poolA, 0, lpf::kModelCiStride);
    memset(poolB, 0, lpf::kModelCiStride);
    *reinterpret_cast<uint32_t*>(poolA + lpf::kModelCiVisibleOff) = 1u;
    *reinterpret_cast<uint32_t*>(poolB + lpf::kModelCiVisibleOff) = 0u;
    model.PutPtr(lpf::kModelCsPoolBaseOff, poolA);

    // gameSpace+8 is the CGame the anchor path needs before it will report kLocalAnchor at all.
    uint8_t game[64] = {};
    void* gamePtr = game;
    memcpy(space.bytes + lpf::kGameSpaceGameOff, &gamePtr, sizeof gamePtr);

    // The echo unit's pool, on the same page: CModelCS and CR15EchoUnitCS reach their elements
    // through two different field groups and therefore two different caches, so a control that
    // drives only one of them leaves the other's guarded load free to be deleted.
    uint8_t* const echoElem = pp.Ok(3000);
    memset(echoElem, 0, lpf::kEchoCiStride);
    uint8_t attached[512] = {};
    const uint64_t noConstraint = lpf::kNoConstraint;
    memcpy(attached + lpf::kAttachedConstraintOff, &noConstraint, sizeof noConstraint);
    void* attachedPtr = attached;
    memcpy(echoElem + lpf::kEchoAttachedOff, &attachedPtr, sizeof attachedPtr);
    echo.PutPtr(lpf::kCsPoolBaseOff, echoElem);

    replay::IEntity* entity =
        replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
    REQUIRE(entity->Available());

    // ENG-W2A-PERF's own composition: eight players and the disc, and the accessors that prove a
    // pool element on the way to their answer. 110 is the count it measured for a whole tick, so
    // the budget below is the whole tick's element-proving cost and not a per-player slice.
    const int kPlayers = 8;
    const int kCallsPerTick = 110;
    REQUIRE(kCallsPerTick > kPlayers);

    SUBCASE("a whole tick of element reads issues ZERO span probes, and costs under 5 us") {
        bool visible = false;
        REQUIRE(entity->ModelVisible(replay::UserIndex(7), visible));
        REQUIRE(visible);

        // THE COUNT, which is the half that reproduces in the field. The warm call above proved the
        // range; nothing in the tick that follows may ask the kernel again.
        const uint64_t before = replay::entity::SpanProbeCount();
        int seen = 0;
        for (int i = 0; i < kCallsPerTick; ++i) {
            bool v = false;
            if (entity->ModelVisible(replay::UserIndex(static_cast<uint16_t>(7)), v) && v) ++seen;
        }
        CHECK(seen == kCallsPerTick);
        CHECK(replay::entity::SpanProbeCount() - before == 0u);

        // THE COST. Interleaved with the bare VirtualQuery the old code added to every one of these
        // calls, best of five: one ordering on a busy machine measures the machine.
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        double tickUs = 1e30, probesUs = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            LARGE_INTEGER a{}, b{}, c{};
            uint64_t sink = 0;
            QueryPerformanceCounter(&a);
            for (int i = 0; i < kCallsPerTick; ++i) {
                bool v = false;
                sink += entity->ModelVisible(replay::UserIndex(7), v) ? 1u : 0u;
            }
            QueryPerformanceCounter(&b);
            MEMORY_BASIC_INFORMATION mbi{};
            for (int i = 0; i < kCallsPerTick; ++i) sink += VirtualQuery(poolA, &mbi, sizeof mbi);
            QueryPerformanceCounter(&c);
            REQUIRE(sink != 0);
            const double one = double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart);
            const double two = double(c.QuadPart - b.QuadPart) * 1e6 / double(f.QuadPart);
            if (one < tickUs) tickUs = one;
            if (two < probesUs) probesUs = two;
        }
        MESSAGE("ENG-W2A-PERF: " << kCallsPerTick << " element-proving accessor calls at "
                << kPlayers << " players: AFTER " << tickUs << " us ("
                << (tickUs * 1000.0 / kCallsPerTick) << " ns/call); the per-call VirtualQuery the "
                << "BEFORE paid on top of each of them: " << probesUs << " us ("
                << (probesUs * 1000.0 / kCallsPerTick) << " ns/call)");
        // The budget the owner's frame is being defended with, stated as a number and not as a
        // comparison that a slow machine satisfies by making both arms slow.
        CHECK(tickUs <= 5.0);
        CHECK(tickUs < probesUs);
    }

    SUBCASE("a pool that MOVES is read at its new base, at the cost of one probe") {
        bool visible = false;
        REQUIRE(entity->ModelVisible(replay::UserIndex(7), visible));
        REQUIRE(visible);

        // AUDIT_M9 (1): a CR15Game that moved on level reload, or a session torn down and rebuilt.
        // The component system's pool pointer is the thing that changes, and it is re-read per call.
        const uint64_t before = replay::entity::SpanProbeCount();
        model.PutPtr(lpf::kModelCsPoolBaseOff, poolB);
        CHECK(entity->ModelVisible(replay::UserIndex(7), visible));
        // poolB says NOT visible. A cache that kept poolA answers `true` here and is reading a pool
        // the engine has stopped using.
        CHECK_FALSE(visible);
        CHECK(replay::entity::SpanProbeCount() - before == 1u);

        // And the new base is then cached in its turn -- one probe per change, not one per change
        // plus one per call afterwards.
        const uint64_t after = replay::entity::SpanProbeCount();
        for (int i = 0; i < kCallsPerTick; ++i) {
            bool v = true;
            CHECK(entity->ModelVisible(replay::UserIndex(7), v));
        }
        CHECK(replay::entity::SpanProbeCount() - after == 0u);
    }

    SUBCASE("a page freed UNDER a proved span refuses, and does not fault") {
        // The half a cached probe cannot cover on its own, and the reason the element access is a
        // guarded load rather than a bare one: VirtualQuery answered about the instant it ran, and
        // this is what happens afterwards. Without the __try this case does not fail -- it takes
        // the process down, which is the shape the owner's match would have.
        bool visible = false;
        REQUIRE(entity->ModelVisible(replay::UserIndex(7), visible));
        REQUIRE(visible);
        replay::PlayerState warm{};
        // The echo element WAS handed back while the page was committed -- without this the
        // assertion after the decommit is satisfied by a pool that never resolved at all.
        REQUIRE((entity->ReadLocalPlayer(replay::UserIndex(7), warm) & replay::kLocalAnchor) != 0u);

        pp.Decommit();

        const uint64_t before = replay::entity::SpanProbeCount();
        visible = true;
        // PRESENCE is the FirstActorComponent handle, which does not touch the freed pool page, so the
        // model still reads present -- but the BEST-EFFORT draw-bit read touches the decommitted page,
        // and the guarded LOAD refuses it, leaving `visible` false without faulting. The point of this
        // case survives: the touch that hits a freed page does not take the process down.
        CHECK(entity->ModelVisible(replay::UserIndex(7), visible));
        CHECK_FALSE(visible);
        replay::PlayerState st{};
        CHECK((entity->ReadLocalPlayer(replay::UserIndex(7), st) & replay::kLocalAnchor) == 0u);
        // The refusal came from the LOAD, not from a fresh probe: the cache still says the range was
        // proved, and it is the touch that disagrees.
        CHECK(replay::entity::SpanProbeCount() - before == 0u);
    }

    ClearLeafSystems();
    g_physCs = nullptr;
    g_playerCs = nullptr;
    g_bcastCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

TEST_CASE("K1-3: the netGame back-map span check is subsumed by the record check, on these facts") {
    namespace lpf = replay::localpawn_facts;
    // NetGameLocalSlot tests the back-map entry separately because LOCAL.NETGAME_SLOT_TABLE_OFF and
    // _STRIDE are both PC_PRESENT/U. With the numbers this build carries it CANNOT refuse: the far
    // end of the table is below the byte the check at the top of that function has already cleared,
    // so no fixture can drive it to its false arm, and one that appeared to would be proving
    // something else. What is pinned instead is the subsumption -- the day a fact moves the table
    // past the record group this goes red, and the guard starts earning its place.
    const uint32_t tableEnd =
        lpf::kNetGameSlotTableOff +
        lpf::kNetGameSlotTableStride * (facts::ENTITY_USER_SLOT_TABLE_MAX - 1u) +
        uint32_t(sizeof(uint16_t));
    const uint32_t recordEnd = lpf::kNetGameUserRecordOff + uint32_t(sizeof(uint16_t));
    CHECK(tableEnd <= recordEnd);
    MESSAGE("netGame back-map ends at +" << tableEnd << ", inside the +" << recordEnd
            << " the user-record check already cleared");
}

// ---------------------------------------------------------------------------------------------------
// ARCH-10 P1. The game space is NETGAME.CS_SPACE_OFF (+0x2B08). The object at
// IDENT.NETGAME_GAMESPACE_OFF (+0x8) is the CR15Game back-pointer, and reading it as a space is why
// `[entity] bound game space` was never emitted in any recorded run. Below: the accessor, its mirror,
// the frozen evidence line byte for byte, and the retroactive control on the guard itself.
// ---------------------------------------------------------------------------------------------------

namespace {

// ARCH-10 section 4.2's <TABLE>, spelled out here instead of taken from tableguard::FormatTable. A
// fixture that renders the expected line THROUGH the code under test cannot see that code change,
// which is the whole point of asserting this line byte for byte.
std::string Table(const char* verdict, const char* why, uint64_t base, uint64_t bytes, uint64_t alloc,
                  uint64_t grow, uint64_t cap, uint64_t count, uint64_t flags) {
    char t[256];
    snprintf(t, sizeof t,
             "%s/%s base=%llx bytes=%llu alloc=%llx grow=%llu cap=%llu count=%llu flags=%llx", verdict,
             why, (unsigned long long)base, (unsigned long long)bytes, (unsigned long long)alloc,
             (unsigned long long)grow, (unsigned long long)cap, (unsigned long long)count,
             (unsigned long long)flags);
    return t;
}

std::string Ptr16(const void* p) {
    char t[32];
    snprintf(t, sizeof t, "%016llX", (unsigned long long)(uintptr_t)p);
    return t;
}

// ARCH-12 section 4.3's amended grammar, spelled out here rather than taken from the emitter, for
// the same reason Table() is: a fixture that builds the expected line through the code under test
// cannot see that code change.
std::string CandidateLine(const void* ng, const std::string& at8, const std::string& at2B08,
                          const std::string& at2B10, const std::string& gameA,
                          const std::string& gameB, const std::string& spawnsym,
                          const std::string& levelsym) {
    return "cs-space candidates: ng=" + Ptr16(ng) + " | +0x8=" + at8 + " | +0x2B08=" + at2B08 +
           " | +0x2B10=" + at2B10 + " | game+0x7AF0=" + gameA + " | game+0x7AF8=" + gameB +
           " | spawnsym=" + spawnsym + " | levelsym=" + levelsym;
}

// <SYM16>. A symbol that READS is 16 hex digits, the all-zero one included -- ARCH-12 4.3 leaves
// the zero case unstated and entity_module.cpp froze it this way; this is the second spelling that
// makes the choice checkable.
std::string Sym16(uint64_t v) {
    char t[32];
    snprintf(t, sizeof t, "%016llX", (unsigned long long)v);
    return t;
}

// FormatTable's spelling for base/alloc: lowercase, unpadded. Deliberately NOT Ptr16's.
std::string HexLo(uint64_t v) {
    char t[32];
    snprintf(t, sizeof t, "%llx", (unsigned long long)v);
    return t;
}

// One rendered fixture address and the fixed stand-in the golden carries for it.
struct Stand {
    std::string from;
    std::string to;
};

// Addresses are not reproducible run to run, so the golden cannot hold them. Every OTHER byte of the
// line is held byte for byte. The substitution matches the EXACT rendered string, so an emitter that
// changed a pointer's width, case or padding does not substitute -- the raw address lands in the
// comparand and the compare reddens, which is the outcome a normalising regex would have hidden.
// One left-to-right pass, longest needle first, so a 16-digit <PTR16> is never eaten by a shorter
// lowercase table field starting at the same offset.
std::string Canonical(const std::string& line, std::vector<Stand> subs, const char* arm) {
    std::sort(subs.begin(), subs.end(),
              [](const Stand& a, const Stand& b) { return a.from.size() > b.from.size(); });
    std::vector<int> hits(subs.size(), 0);
    std::string out;
    for (size_t i = 0; i < line.size();) {
        size_t k = subs.size();
        for (size_t j = 0; j < subs.size(); ++j) {
            if (!subs[j].from.empty() && line.compare(i, subs[j].from.size(), subs[j].from) == 0) {
                k = j;
                break;
            }
        }
        if (k == subs.size()) {
            out.push_back(line[i++]);
            continue;
        }
        out += subs[k].to;
        i += subs[k].from.size();
        ++hits[k];
    }
    // A stand-in that matched nothing means the line no longer renders that fixture object at all --
    // a silent substitution failure would otherwise write a live address into the golden.
    for (size_t j = 0; j < subs.size(); ++j) {
        INFO("arm " << arm << ": the line must render fixture address " << subs[j].from);
        CHECK(hits[j] > 0);
    }
    return out;
}

std::string ReadAll(const char* path) {
    std::string out;
    if (FILE* f = fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        fclose(f);
    }
    return out;
}

// Binary, because the contract is LF and this host's text mode writes CRLF.
bool WriteAll(const char* path, const std::string& text) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const size_t n = fwrite(text.data(), 1, text.size(), f);
    return fclose(f) == 0 && n == text.size();
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void ReportGoldenDiff(const std::string& have, const std::string& want) {
    const std::vector<std::string> a = SplitLines(have);
    const std::vector<std::string> b = SplitLines(want);
    int shown = 0;
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const std::string x = i < a.size() ? a[i] : std::string("<no such line>");
        const std::string y = i < b.size() ? b[i] : std::string("<no such line>");
        if (x == y) continue;
        if (++shown > 10) {
            MESSAGE("... further differences not shown");
            return;
        }
        MESSAGE("golden line " << (i + 1) << "\n  on disk: " << x << "\n  emitted: " << y);
    }
}

}  // namespace

TEST_CASE("ARCH-10 P1: the space is read at NETGAME.CS_SPACE_OFF, not at the CR15Game back-pointer") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);

    SUBCASE("the space at +0x2B08, with an unreadable CR15Game at +0x8, binds") {
        FakeNetGame netGame;
        netGame.PointAt(&space);
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);

        CHECK_FALSE(e.log.LastWith("bound game space", from).empty());
        CHECK(e.services.asSeen != nullptr);
    }

    SUBCASE("the same space present ONLY at +0x8 is NoSpace, and nothing binds") {
        FakeNetGame netGame;
        netGame.PointAtLegacySlotOnly(&space);
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);

        CHECK(e.log.LastWith("bound game space", from).empty());
        CHECK(e.services.asSeen == nullptr);
        // A NoSpace refusal has no space to name, so the deferral line stays silent -- the evidence
        // line is the only thing carrying the attempt, which is exactly the case it must not miss.
        CHECK(e.log.LastWith("game space", from).empty());
        CHECK_FALSE(e.log.LastWith("cs-space candidates", from).empty());
    }

    e.Tick(nullptr);
}

TEST_CASE("ARCH-12 section 4.3: the five-candidate evidence line renders byte for byte") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    const std::string ready =
        Table("Ready", "Ready", (uint64_t)(uintptr_t)space.elements, 4 * kEntryStride,
              (uint64_t)(uintptr_t)&space.allocatorObject, 4, 4, 1, 0);
    const std::string unreadableTable = Table("Unreadable", "HeaderUnreadable", 0, 0, 0, 0, 0, 0, 0);

    SUBCASE("a bind attempt: +0x2B08 Ready, no level space, CR15Game present but unreadable") {
        FakeNetGame netGame;
        netGame.PointAt(&space);
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);

        const std::string got = e.log.LastWith("cs-space candidates", from);
        CHECK(got == CandidateLine(netGame.bytes,
                                   Ptr16(reinterpret_cast<void*>(kUnmapped)) + " " + unreadableTable,
                                   Ptr16(space.bytes) + " " + ready, "null", "unreadable",
                                   "unreadable", "unreadable", "null"));
        MESSAGE("W2-R sample line: [entity] " << got);
    }

    SUBCASE("a null CR15Game renders `null`, and hands that word down to its fields and spawnsym") {
        FakeNetGame netGame;
        netGame.PointAt(&space);
        netGame.NoGame();
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);

        CHECK(e.log.LastWith("cs-space candidates", from) ==
              CandidateLine(netGame.bytes, "null", Ptr16(space.bytes) + " " + ready, "null", "null",
                            "null", "null", "null"));
    }

    SUBCASE("an unreadable netGame renders `unreadable` in all seven fields, and still emits") {
        void* const bogus = reinterpret_cast<void*>(kUnmapped);
        const size_t from = e.log.lines.size();
        e.Tick(bogus);

        CHECK(e.log.LastWith("cs-space candidates", from) ==
              CandidateLine(bogus, "unreadable", "unreadable", "unreadable", "unreadable",
                            "unreadable", "unreadable", "unreadable"));
    }

    // ARCH-12 5.3's rig, whole: BOTH spaces populated and DISTINCT, differing in table content and
    // not only in count, and a CR15Game carrying space L's name symbol at +0x8A48. This is the
    // shape an L5 is expected to produce, and it is the case the fifth column exists for.
    SUBCASE("both spaces populated and distinct, spawnsym agreeing with levelsym") {
        FakeSpace levelSpace;
        levelSpace.Construct(levelSpace.elements, 3);
        const uint64_t kLevelSym = 0xA17E5B0C99341122ull;
        levelSpace.SetNameSymbol(kLevelSym);

        FakeGame game;
        game.SetSpawnSymbol(kLevelSym);

        FakeNetGame netGame;
        netGame.PointAt(&space);
        netGame.PointLevelAt(&levelSpace);
        netGame.PointGameAt(game.Base());
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);

        // The CR15Game reads now, so +0x8 renders its own +0x420 -- all zero, which tableguard
        // answers Empty. That is 4.2 rule 4's sensitivity control still refusing the back-pointer.
        const std::string emptyTable = Table("Empty", "Empty", 0, 0, 0, 0, 0, 0, 0);
        const std::string levelReady =
            Table("Ready", "Ready", (uint64_t)(uintptr_t)levelSpace.elements, 4 * kEntryStride,
                  (uint64_t)(uintptr_t)&levelSpace.allocatorObject, 4, 4, 3, 0);
        const std::string got = e.log.LastWith("cs-space candidates", from);
        // game+0x7AF* sit inside the 35 KB CR15Game buffer and read as zero -> `null`.
        CHECK(got == CandidateLine(netGame.bytes, Ptr16(game.Base()) + " " + emptyTable,
                                   Ptr16(space.bytes) + " " + ready,
                                   Ptr16(levelSpace.bytes) + " " + levelReady, "null", "null",
                                   Sym16(kLevelSym), Sym16(kLevelSym)));
        // Content, not only count: space C's table names its own block and space L names another.
        CHECK(ready != levelReady);
        MESSAGE("W2-R sample line (ARCH-12 5.3 rig): [entity] " << got);
    }

    // The two symbols are read out of two DIFFERENT objects. An emitter that read levelsym off the
    // CR15Game, or spawnsym off the level space, renders one value twice and passes the case above
    // -- it cannot pass this one. ARCH-12 4.3 rule 6: a mismatch is INFORMATION and changes no
    // verdict, so what is asserted is the rendering, not a refusal.
    SUBCASE("spawnsym and levelsym disagree, and both are still rendered") {
        FakeSpace levelSpace;
        levelSpace.Construct(levelSpace.elements, 3);
        const uint64_t kLevelSym = 0xA17E5B0C99341122ull;
        const uint64_t kSpawnSym = 0x0F0E0D0C0B0A0908ull;
        levelSpace.SetNameSymbol(kLevelSym);

        FakeGame game;
        game.SetSpawnSymbol(kSpawnSym);

        FakeNetGame netGame;
        netGame.PointAt(&space);
        netGame.PointLevelAt(&levelSpace);
        netGame.PointGameAt(game.Base());
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);

        const std::string got = e.log.LastWith("cs-space candidates", from);
        CHECK(got.find(" | spawnsym=" + Sym16(kSpawnSym) + " | levelsym=" + Sym16(kLevelSym)) !=
              std::string::npos);
        CHECK_FALSE(e.log.LastWith("bound game space", from).empty());
    }

    // The frozen minimal choice: a symbol that READS is 16 hex digits even when it is zero. A
    // level space that is present but unnamed must not render as one that is absent.
    SUBCASE("a zero name symbol renders 0000000000000000, not `null`") {
        FakeSpace levelSpace;
        levelSpace.Construct(levelSpace.elements, 3);

        FakeNetGame netGame;
        netGame.PointAt(&space);
        netGame.PointLevelAt(&levelSpace);
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);

        const std::string got = e.log.LastWith("cs-space candidates", from);
        CHECK(got.find(" | levelsym=0000000000000000") != std::string::npos);
        CHECK(got.find(" | levelsym=null") == std::string::npos);
    }

    // ARCH-12 4.3 rule 5's input: one object in both slots. The emitter judges nothing -- it
    // renders the same <PTR16> twice, which is exactly what lets W2-R's aliasing guard fire.
    SUBCASE("the level slot aliased to +0x2B08 renders one pointer in two columns") {
        FakeNetGame netGame;
        netGame.PointAt(&space);
        netGame.AliasLevelToCsSpace();
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);

        const std::string got = e.log.LastWith("cs-space candidates", from);
        CHECK(got == CandidateLine(netGame.bytes,
                                   Ptr16(reinterpret_cast<void*>(kUnmapped)) + " " + unreadableTable,
                                   Ptr16(space.bytes) + " " + ready,
                                   Ptr16(space.bytes) + " " + ready, "unreadable", "unreadable",
                                   "unreadable", Sym16(0)));
    }

    e.Tick(nullptr);
}

// AUDIT_F3H F3H-7: the case above and W2-R's `tests/replay/test_observer.cpp` each carried their own
// private spelling of ARCH-12 4.3's grammar, so mutating one byte of the emitter's frozen label
// reddened `entity_wiretest` 1/5 and left `test_replay` 52/52 GREEN -- the frozen pair was unenforced
// in the direction that matters. This case makes the emitter's own bytes a committed artifact that
// W2-R's parser test reads read-only: one file, two readers, and neither side can move alone.
// L5 re-run #4 F11. The sampler emitted three lines and stopped at the bind on +0x2B08, 1.4 s
// BEFORE the level space appeared at +0x2B10 -- so `+0x2B10 rendered null` was a fact about a
// window that had already closed, and a row was written on it while the camera went on to bind to
// that very space. ARCH-12 section 5 forbids assuming either space's arrival order (L5 #3 had the
// reverse by 1.3 s), so both orders are rigged here, and the stop condition is asserted from BOTH
// sides: the line is still there when the second space arrives, and gone once both have rendered.
TEST_CASE("L5 #4 F11: the candidate line outlives the bind until BOTH real spaces have rendered") {
    Env& e = TheEnv();

    // Only the lines this case caused. RecordingLog::Count spans the whole file's history, and the
    // number under test here is a COUNT -- a per-tick emitter fails these cases by two thousand
    // lines, which a "the last line looks right" assertion would not see at all.
    auto since = [&e](size_t from) {
        size_t n = 0;
        for (size_t i = from; i < e.log.lines.size(); ++i)
            if (e.log.lines[i].module == "entity" &&
                e.log.lines[i].text.find("cs-space candidates") != std::string::npos)
                ++n;
        return n;
    };

    FakeSpace space;
    space.Construct(space.elements);
    const std::string ready =
        Table("Ready", "Ready", (uint64_t)(uintptr_t)space.elements, 4 * kEntryStride,
              (uint64_t)(uintptr_t)&space.allocatorObject, 4, 4, 1, 0);
    const std::string csReady = Ptr16(space.bytes) + " " + ready;
    const std::string at8Unreadable = Ptr16(reinterpret_cast<void*>(kUnmapped)) + " " +
                                      Table("Unreadable", "HeaderUnreadable", 0, 0, 0, 0, 0, 0, 0);

    FakeSpace levelSpace;
    levelSpace.Construct(levelSpace.elements, 3);
    const uint64_t kLevelSym = 0x51A7E4C0DE770011ull;
    levelSpace.SetNameSymbol(kLevelSym);
    const std::string levelReady =
        Ptr16(levelSpace.bytes) + " " +
        Table("Ready", "Ready", (uint64_t)(uintptr_t)levelSpace.elements, 4 * kEntryStride,
              (uint64_t)(uintptr_t)&levelSpace.allocatorObject, 4, 4, 3, 0);

    SUBCASE("the level space arriving AFTER the bind gets its own line, and exactly one") {
        FakeNetGame netGame;
        netGame.PointAt(&space);
        e.Tick(nullptr);
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);
        REQUIRE_FALSE(e.log.LastWith("bound game space", from).empty());
        CHECK(since(from) == 1);
        CHECK(e.log.LastWith("cs-space candidates", from) ==
              CandidateLine(netGame.bytes, at8Unreadable, csReady, "null", "unreadable",
                            "unreadable", "unreadable", "null"));

        // Bound, steady, and the window still open: nothing has changed, so nothing is said.
        for (int i = 0; i < 2000; ++i) e.Tick(netGame.bytes);
        CHECK(since(from) == 1);

        netGame.PointLevelAt(&levelSpace);
        e.Tick(netGame.bytes);
        CHECK(since(from) == 2);
        const std::string got = e.log.LastWith("cs-space candidates", from);
        CHECK(got == CandidateLine(netGame.bytes, at8Unreadable, csReady, levelReady, "unreadable",
                                   "unreadable", "unreadable", Sym16(kLevelSym)));
        MESSAGE("W2-R sample line (post-bind, L5 #4 F11): [entity] " << got);

        // Both real candidates have rendered non-null, so it stops: the binding is not re-judged
        // for the rest of the match, and the line is bounded by changes and not by ticks.
        for (int i = 0; i < 2000; ++i) e.Tick(netGame.bytes);
        CHECK(since(from) == 2);
    }

    SUBCASE("the reverse order -- the level space already there at the bind -- is one line for both") {
        FakeNetGame netGame;
        netGame.PointAt(&space);
        netGame.PointLevelAt(&levelSpace);
        e.Tick(nullptr);
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);
        CHECK(since(from) == 1);
        CHECK(e.log.LastWith("cs-space candidates", from) ==
              CandidateLine(netGame.bytes, at8Unreadable, csReady, levelReady, "unreadable",
                            "unreadable", "unreadable", Sym16(kLevelSym)));

        // Nothing is left to wait for, so 2000 more ticks say nothing -- the same silence the case
        // above reaches only after the second space lands. Both orders end in one steady state.
        for (int i = 0; i < 2000; ++i) e.Tick(netGame.bytes);
        CHECK(since(from) == 1);
    }

    // The change is over EVERY candidate, not over +0x2B10 alone: an emitter watching only the slot
    // it is waiting for passes both cases above and fails this one. The CR15Game becoming readable
    // moves +0x8 from Unreadable to Empty and fills spawnsym, and the window stays open because the
    // level space is still null -- so this line is emitted AND the later one still is.
    SUBCASE("a change in another column speaks too, and does not close the window") {
        FakeGame game;
        const uint64_t kSpawnSym = 0x0F0E0D0C0B0A0908ull;
        game.SetSpawnSymbol(kSpawnSym);

        FakeNetGame netGame;
        netGame.PointAt(&space);
        e.Tick(nullptr);
        const size_t from = e.log.lines.size();
        e.Tick(netGame.bytes);
        CHECK(since(from) == 1);

        netGame.PointGameAt(game.Base());
        e.Tick(netGame.bytes);
        CHECK(since(from) == 2);
        CHECK(e.log.LastWith("cs-space candidates", from) ==
              CandidateLine(netGame.bytes,
                            Ptr16(game.Base()) + " " + Table("Empty", "Empty", 0, 0, 0, 0, 0, 0, 0),
                            csReady, "null", "null", "null", Sym16(kSpawnSym), "null"));

        netGame.PointLevelAt(&levelSpace);
        e.Tick(netGame.bytes);
        CHECK(since(from) == 3);
        for (int i = 0; i < 2000; ++i) e.Tick(netGame.bytes);
        CHECK(since(from) == 3);
    }
}

// AUD-C1 F3 rejected the shape this case measures: the post-bind watch ran the WHOLE rendering --
// five tableguard::CheckSpaceTable judgements, two symbol reads and a 1616-byte snprintf -- on
// every tick until both real candidates had rendered, a window a launch that never loads a level
// never closes. It emitted nothing on those ticks, so every line count above is green under it and
// none of them can see it. CandidateTableChecks is the emitter's own tally of judgements, and every
// number here is a DELTA over a window: what is bounded is the work, not the output.
TEST_CASE("The post-bind watch's per-tick COST is bounded, and by a stated budget") {
    Env& e = TheEnv();

    auto count = [&e](const char* needle, size_t from) {
        size_t n = 0;
        for (size_t i = from; i < e.log.lines.size(); ++i)
            if (e.log.lines[i].module == "entity" &&
                e.log.lines[i].text.find(needle) != std::string::npos)
                ++n;
        return n;
    };

    FakeSpace space;
    space.Construct(space.elements);

    // Bound on +0x2B08 with +0x2B10 still null, so the window is open and the watch is watching --
    // the exact state F3 says has no ceiling.
    auto bind = [&e](FakeNetGame& ng) {
        e.Tick(nullptr);
        e.Tick(ng.bytes);
    };

    SUBCASE("5000 steady ticks with the window open make NO judgement at all") {
        FakeNetGame netGame;
        netGame.PointAt(&space);
        bind(netGame);
        const uint64_t judgements = replay::entity::CandidateTableChecks();
        const size_t from = e.log.lines.size();

        for (int i = 0; i < 5000; ++i) e.Tick(netGame.bytes);

        // THE BUDGET: zero. A steady tick costs five slot reads and two table headers and never
        // reaches the rendering, so the count that F3 asked for is not merely small, it is nothing.
        CHECK(replay::entity::CandidateTableChecks() - judgements == 0u);
        CHECK(count("cs-space candidates", from) == 0);
        // 1800 of those 5000 ticks in, the watch said it was going dormant -- once. W2-R needs it:
        // silence after this line is dormancy, and silence after a line carrying a non-null +0x2B10
        // is the closed window, and those are different facts about the level space.
        CHECK(count("cs-space candidate watch dormant", from) == 1);
    }

    SUBCASE("a candidate flapping on EVERY tick costs a fixed ceiling, not a judgement per tick") {
        FakeGame game;
        FakeNetGame netGame;
        netGame.PointAt(&space);
        bind(netGame);
        const uint64_t judgements = replay::entity::CandidateTableChecks();
        const size_t from = e.log.lines.size();

        // The CR15Game at +0x8 alternates readable and unmapped, so the probe moves on every one of
        // the 5000 ticks and "bounded by the number of changes" is no bound at all.
        for (int i = 0; i < 5000; ++i) {
            netGame.PointGameAt(i % 2 ? game.Base() : reinterpret_cast<void*>(kUnmapped));
            e.Tick(netGame.bytes);
        }

        // Eight change lines and then the binding is done talking. Five judgements per rendering is
        // the stated ceiling; this rig pays only three of them, because game+0x7AF0/+0x7AF8 are not
        // judged at all while the CR15Game they hang off is unreadable.
        CHECK(replay::entity::CandidateTableChecks() - judgements <= 8u * 5u);
        CHECK(replay::entity::CandidateTableChecks() - judgements == 16u);
        CHECK(count("cs-space candidates", from) == 8);
        CHECK(count("cs-space candidate watch stopped", from) == 1);
        // The ceiling is TERMINAL for the binding: a level space arriving after it is spent gets no
        // line, which is why the line above says so rather than the watch just falling silent.
        netGame.PointGameAt(game.Base());
        FakeSpace levelSpace;
        levelSpace.Construct(levelSpace.elements, 3);
        netGame.PointLevelAt(&levelSpace);
        for (int i = 0; i < 100; ++i) e.Tick(netGame.bytes);
        CHECK(count("cs-space candidates", from) == 8);
        CHECK(replay::entity::CandidateTableChecks() - judgements == 16u);
    }

    SUBCASE("dormancy is not deafness -- a level space arriving long after it re-arms the watch") {
        FakeSpace levelSpace;
        levelSpace.Construct(levelSpace.elements, 3);
        const uint64_t kLevelSym = 0x51A7E4C0DE770011ull;
        levelSpace.SetNameSymbol(kLevelSym);

        FakeNetGame netGame;
        netGame.PointAt(&space);
        bind(netGame);
        const uint64_t judgements = replay::entity::CandidateTableChecks();
        const size_t from = e.log.lines.size();

        for (int i = 0; i < 2500; ++i) e.Tick(netGame.bytes);
        CHECK(count("cs-space candidate watch dormant", from) == 1);
        CHECK(replay::entity::CandidateTableChecks() - judgements == 0u);

        // Dormant reads the five slot POINTERS and nothing else, so this is still seen -- and it is
        // the whole point of dormancy rather than a stop: ARCH-12 section 5 does not bound how late
        // the second space may be.
        netGame.PointLevelAt(&levelSpace);
        e.Tick(netGame.bytes);
        CHECK(count("cs-space candidates", from) == 1);
        // One rendering: five judgements at the stated ceiling, three here -- game+0x7AF0/+0x7AF8
        // hang off this rig's unmapped CR15Game and are never judged.
        CHECK(replay::entity::CandidateTableChecks() - judgements <= 5u);
        CHECK(replay::entity::CandidateTableChecks() - judgements == 3u);
        CHECK(e.log.LastWith("cs-space candidates", from).find("levelsym=" + Sym16(kLevelSym)) !=
              std::string::npos);

        // Both candidates have now rendered: the window is CLOSED, which is a stronger silence than
        // dormancy and costs nothing at all.
        for (int i = 0; i < 5000; ++i) e.Tick(netGame.bytes);
        CHECK(count("cs-space candidates", from) == 1);
        CHECK(count("cs-space candidate watch dormant", from) == 1);
        CHECK(replay::entity::CandidateTableChecks() - judgements == 3u);
    }
}

TEST_CASE("ARCH-12 4.3: the emitted line matches the committed golden, arm for arm") {
    static const char* const kHeader =
        "# ARCH-12 4.3 GOLDEN -- the cs-space-candidates line, byte for byte, one line per arm.\n"
        "#\n"
        "# W1-E emits this grammar and W2-R parses it; ARCH-12 4.3 froze them as a pair, and until\n"
        "# this file existed each side carried its own private spelling, so a one-character label\n"
        "# change reddened one suite and left the other green (AUDIT_F3H F3H-7). This file is WRITTEN\n"
        "# AND COMPARED by tests/entity/test_entity_module.cpp from the shipping emitter's own output,\n"
        "# and READ read-only by W2-R's parser test. Either side moving now reddens both.\n"
        "#\n"
        "# CONTRACT: one arm per line, `<arm-name> <TAB> <emitted line>`; LF line endings; ASCII\n"
        "# only; no trailing spaces; a `#` in column 1 is a comment; no blank lines. The payload is\n"
        "# the emitter's own bytes -- the logger's `[entity] ` module prefix is NOT part of it.\n"
        "#\n"
        "# Addresses are not reproducible run to run, so the fixture's own objects render as the\n"
        "# fixed stand-ins below and EVERY OTHER BYTE is exactly what src/engine/entity_module.cpp\n"
        "# wrote. The substitution matches the exact rendered string, so a change of width, case or\n"
        "# padding does not substitute: the raw address lands in the comparand and the compare fails.\n"
        "#\n"
        "#   00000000E6000000  the netGame            00000000E62B0800  the space at +0x2B08\n"
        "#   e62b0840/e62b0880 its block/allocator    00000000E62B1000  the space at +0x2B10\n"
        "#   e62b1040/e62b1080 its block/allocator    00000000E68A4800  the CR15Game at +0x8\n"
        "#\n"
        "# 0000600DBADBAD00 is NOT a stand-in: it is the fixture's unmapped probe address, verbatim.\n"
        "#\n"
        "# To regenerate after an INTENDED emitter change, from the build directory:\n"
        "#   REPLAY_UPDATE_GOLDEN=1 bin/entity_wiretest.exe -tc=\"*committed golden*\"\n"
        "# The case prints every changed line, rewrites this file, re-reads it from disk and compares\n"
        "# again. Commit the regenerated file in the SAME commit as the emitter change.\n";

    const char* const kNgTok = "00000000E6000000";
    const char* const kCsTok = "00000000E62B0800";
    const char* const kLevelTok = "00000000E62B1000";
    const char* const kGameTok = "00000000E68A4800";

    Env& e = TheEnv();
    std::vector<std::pair<std::string, std::string>> arms;

    // Tick(nullptr) drops the binding, so every arm is a fresh bind ATTEMPT and emits its own line.
    auto emit = [&e](void* ng) {
        const size_t from = e.log.lines.size();
        e.Tick(nullptr);
        e.Tick(ng);
        return e.log.LastWith("cs-space candidates", from);
    };
    auto spaceStands = [](const FakeSpace& s, const char* ptrTok, const char* blockTok,
                          const char* allocTok) {
        return std::vector<Stand>{
            {Ptr16(s.bytes), ptrTok},
            {HexLo((uint64_t)(uintptr_t)s.elements), blockTok},
            {HexLo((uint64_t)(uintptr_t)&s.allocatorObject), allocTok}};
    };

    FakeSpace space;
    space.Construct(space.elements);

    {
        FakeNetGame ng;
        ng.PointAt(&space);
        std::vector<Stand> s = spaceStands(space, kCsTok, "e62b0840", "e62b0880");
        s.push_back({Ptr16(ng.bytes), kNgTok});
        arms.push_back({"ready-2b08-only", Canonical(emit(ng.bytes), s, "ready-2b08-only")});
    }
    {
        FakeNetGame ng;
        ng.PointAt(&space);
        ng.NoGame();
        std::vector<Stand> s = spaceStands(space, kCsTok, "e62b0840", "e62b0880");
        s.push_back({Ptr16(ng.bytes), kNgTok});
        arms.push_back({"null-cr15game", Canonical(emit(ng.bytes), s, "null-cr15game")});
    }
    // Wholly deterministic: no fixture object is reachable, so no stand-in is needed and this arm is
    // the one W2-R can also construct by hand.
    arms.push_back({"unreadable-netgame", Canonical(emit(reinterpret_cast<void*>(kUnmapped)),
                                                    std::vector<Stand>(), "unreadable-netgame")});

    const uint64_t kLevelSym = 0xA17E5B0C99341122ull;
    const uint64_t kSpawnSym = 0x0F0E0D0C0B0A0908ull;
    {
        // ARCH-12 5.3's rig: the shape an L5 is expected to produce.
        FakeSpace levelSpace;
        levelSpace.Construct(levelSpace.elements, 3);
        levelSpace.SetNameSymbol(kLevelSym);
        FakeGame game;
        game.SetSpawnSymbol(kLevelSym);
        FakeNetGame ng;
        ng.PointAt(&space);
        ng.PointLevelAt(&levelSpace);
        ng.PointGameAt(game.Base());
        std::vector<Stand> s = spaceStands(space, kCsTok, "e62b0840", "e62b0880");
        const std::vector<Stand> l = spaceStands(levelSpace, kLevelTok, "e62b1040", "e62b1080");
        s.insert(s.end(), l.begin(), l.end());
        s.push_back({Ptr16(ng.bytes), kNgTok});
        s.push_back({Ptr16(game.Base()), kGameTok});
        arms.push_back(
            {"both-spaces-symbols-agree", Canonical(emit(ng.bytes), s, "both-spaces-symbols-agree")});
    }
    {
        FakeSpace levelSpace;
        levelSpace.Construct(levelSpace.elements, 3);
        levelSpace.SetNameSymbol(kLevelSym);
        FakeGame game;
        game.SetSpawnSymbol(kSpawnSym);
        FakeNetGame ng;
        ng.PointAt(&space);
        ng.PointLevelAt(&levelSpace);
        ng.PointGameAt(game.Base());
        std::vector<Stand> s = spaceStands(space, kCsTok, "e62b0840", "e62b0880");
        const std::vector<Stand> l = spaceStands(levelSpace, kLevelTok, "e62b1040", "e62b1080");
        s.insert(s.end(), l.begin(), l.end());
        s.push_back({Ptr16(ng.bytes), kNgTok});
        s.push_back({Ptr16(game.Base()), kGameTok});
        arms.push_back({"symbols-disagree", Canonical(emit(ng.bytes), s, "symbols-disagree")});
    }
    {
        FakeSpace levelSpace;
        levelSpace.Construct(levelSpace.elements, 3);
        FakeNetGame ng;
        ng.PointAt(&space);
        ng.PointLevelAt(&levelSpace);
        std::vector<Stand> s = spaceStands(space, kCsTok, "e62b0840", "e62b0880");
        const std::vector<Stand> l = spaceStands(levelSpace, kLevelTok, "e62b1040", "e62b1080");
        s.insert(s.end(), l.begin(), l.end());
        s.push_back({Ptr16(ng.bytes), kNgTok});
        arms.push_back({"zero-level-symbol", Canonical(emit(ng.bytes), s, "zero-level-symbol")});
    }
    {
        // L5 #4 F11's arm: the level space arrives AFTER the bind, so this is the line the emitter
        // was silent about when W2-R's row was written. Same grammar as a bind-attempt line -- that
        // sameness is what this arm holds still, so W2-R needs no second parser for it.
        FakeSpace lateLevel;
        lateLevel.Construct(lateLevel.elements, 3);
        lateLevel.SetNameSymbol(kLevelSym);
        FakeNetGame ng;
        ng.PointAt(&space);
        const size_t from = e.log.lines.size();
        e.Tick(nullptr);
        e.Tick(ng.bytes);
        ng.PointLevelAt(&lateLevel);
        e.Tick(ng.bytes);
        std::vector<Stand> s = spaceStands(space, kCsTok, "e62b0840", "e62b0880");
        const std::vector<Stand> l = spaceStands(lateLevel, kLevelTok, "e62b1040", "e62b1080");
        s.insert(s.end(), l.begin(), l.end());
        s.push_back({Ptr16(ng.bytes), kNgTok});
        arms.push_back({"level-space-after-bind",
                        Canonical(e.log.LastWith("cs-space candidates", from), s,
                                  "level-space-after-bind")});
    }
    {
        FakeNetGame ng;
        ng.PointAt(&space);
        ng.AliasLevelToCsSpace();
        std::vector<Stand> s = spaceStands(space, kCsTok, "e62b0840", "e62b0880");
        s.push_back({Ptr16(ng.bytes), kNgTok});
        arms.push_back(
            {"level-aliased-to-2b08", Canonical(emit(ng.bytes), s, "level-aliased-to-2b08")});
    }
    e.Tick(nullptr);

    std::string want = kHeader;
    for (size_t i = 0; i < arms.size(); ++i) want += arms[i].first + "\t" + arms[i].second + "\n";

    // The header's contract, enforced rather than merely stated. One assertion each, not one per
    // byte: a per-byte CHECK would inflate the suite's assertion count by thousands.
    bool asciiOnly = true;
    for (unsigned char c : want) {
        asciiOnly = asciiOnly && ((c >= 0x20 && c <= 0x7E) || c == '\t' || c == '\n');
    }
    CHECK(asciiOnly);
    CHECK(want.find('\r') == std::string::npos);
    CHECK(want.find(" \n") == std::string::npos);
    CHECK(want.find("\n\n") == std::string::npos);
    CHECK(arms.size() == 8);

    static const char kPath[] = REPLAY_REPO_ROOT "/tests/entity/golden/cs-space-candidates.txt";
    std::string have = ReadAll(kPath);
    if (have != want) {
        ReportGoldenDiff(have, want);
        if (getenv("REPLAY_UPDATE_GOLDEN") != nullptr) {
            CHECK(WriteAll(kPath, want));
            // Re-read from disk: the comparison below is then about the bytes that actually landed,
            // which is what W2-R will read, not about the string this process happens to hold.
            have = ReadAll(kPath);
            MESSAGE("golden REGENERATED at " << kPath
                                             << " -- review the diff above and commit it in the SAME "
                                                "commit as the emitter change that caused it");
        } else {
            MESSAGE("golden MISMATCH at "
                    << kPath
                    << " -- the emitter and the committed grammar disagree. If the change is "
                       "intended, re-run this case with REPLAY_UPDATE_GOLDEN=1 and commit the "
                       "regenerated file together with the emitter change.");
        }
    }
    // Compared through a bool: a decomposed `have == want` dumps both whole files into the failure,
    // which buries the per-line diff ReportGoldenDiff just printed.
    const bool matchesGolden = (have == want);
    INFO("golden file: " << kPath);
    CHECK(matchesGolden);
}

TEST_CASE("RETROACTIVE (PROOF-F3): the header L5 actually logged is still refused, unchanged") {
    // The seven fields every `[entity] ... deferred: CapacityOverBlock` line of L5-fc499e1 carried,
    // byte for byte out of a memory dump (PROOF-F3 old_accessor_hdr.bin) -- the CR15Game's
    // +0x420 read as a CncaGameSpace table. The accessor moved to +0x2B08; the guard that refused
    // these numbers must NOT have moved with it, or the bind would have been bought by weakening the
    // check rather than by finding the right object.
    replay::tableguard::TableRaw raw;
    raw.base      = 0x400000002000ull;
    raw.bytes     = 2199023256064ull;
    raw.allocator = 0x8000000080ull;
    raw.grow      = 70368744179712ull;
    raw.capacity  = 54043196648849408ull;
    raw.count     = 1048576ull;
    // `flags` is the one field here that is not what the header holds. CS.TABLE_FLAGS_OFF is a DWORD
    // (DIAG-F3-B section 2); the shipping reader took it 8 wide and pulled the next dword in with it,
    // so the bytes `00 00 00 00 01 00 00 00` at +0x38 rendered as 0x100000000 -- flags 0, and a 1 that
    // belongs to +0x3C, not to this field at all. The recorded L5 line says 0x100000000 because that
    // is what the instrument printed, and reproducing THAT line is the point of this control, so the
    // number stays. What the true dword is, is asserted below.
    raw.flags     = 0x100000000ull;

    const replay::tableguard::TableCheck c =
        replay::tableguard::JudgeTable(raw, replay::localpawn_facts::kSystemTableStride, false);
    CHECK(c.verdict == replay::tableguard::TableVerdict::NotReady);
    CHECK(c.why == replay::tableguard::TableWhy::CapacityOverBlock);

    char line[256];
    replay::tableguard::FormatTable(c, line, sizeof line);
    CHECK(std::string(line) ==
          "NotReady/CapacityOverBlock base=400000002000 bytes=2199023256064 alloc=8000000080 "
          "grow=70368744179712 cap=54043196648849408 count=1048576 flags=100000000");

    // The refusal is decided by capacity against bytes and never looks at flags, so the TRUE dword
    // gives the same verdict for the same reason. That is what makes this control survive W1-C's
    // narrowing of the flags read: after it, CheckSpaceTable renders `flags=0` for these bytes and
    // the recorded string above becomes unreproducible from memory, while the judgement below -- the
    // thing this case is a control ON -- does not move. It is also why a bit-0 rule promoted into
    // tableguard cannot reach these bytes: SortPending is said only of a table that would otherwise
    // be Ready or Empty, and this one is neither.
    raw.flags = 0u;
    const replay::tableguard::TableCheck trueDword =
        replay::tableguard::JudgeTable(raw, replay::localpawn_facts::kSystemTableStride, false);
    CHECK(trueDword.verdict == replay::tableguard::TableVerdict::NotReady);
    CHECK(trueDword.why == replay::tableguard::TableWhy::CapacityOverBlock);
}

// ===================================================================================================
// ENG-W1E-R5 / L2 F4. The capture held ZERO player state for the whole match: S_PLAYER_SAMPLED
// declared one local pawn present on all 976 ticks and stored a sample on none of them, because
// ReadLocalPlayer returned a fill mask of 0 every time. The cause is a SINGLE actor source --
// the broadcaster pool's parallel SActorGUID array -- and `if (!actor) return 0` above every
// accessor group, so one null deref took the pose, the reference velocity, the body, the trigger,
// the anchor and the hold ids with it.
//
// Below: the L2 shape retroactively (it must still refuse, and now NAME the arm), the HOST shape
// the second source rescues, the CLIENT shape the first one already served, and the caching
// contract -- resolved once per binding, re-asked only while it is still unresolved.
// ===================================================================================================
namespace {

size_t g_userActorCalls = 0;
uint8_t g_userActorGuid[replay::localpawn_facts::kActorGuidSize] = {};

const void* CountingUserActor(void*, void* out16, uint16_t) {
    ++g_userActorCalls;
    memcpy(out16, g_userActorGuid, sizeof g_userActorGuid);
    return out16;
}

// One handle for every system that is asked, so a fixture cannot pass by resolving only the
// component the assertion happens to name.
void* AnyComponentOnAnyCs(void* cs, void* out, uint64_t, uint64_t) {
    uint32_t id = cs ? 0u : uint32_t(replay::localpawn_facts::kNoComponent);
    memcpy(out, &id, sizeof id);
    return out;
}

float g_refVel[3] = {1.0f, 2.0f, 3.0f};
const float* FakeRefVel(void*, uint16_t) { return g_refVel; }

const uint32_t* FakeHeldPlayer(void* out, void*, const void*, uint32_t arm) {
    uint32_t v = 0x1000u + arm;
    memcpy(out, &v, sizeof v);
    return static_cast<const uint32_t*>(out);
}

// Everything ReadLocalPlayer reads, wired from the facts the shipping code reads. The two big
// strides live on the heap: LOCAL.BODY_CI_STRIDE alone is 16240 bytes, and a fixture that let
// LeafPool's span check run off the end of a stack object would be failing for a reason that has
// nothing to do with the actor.
struct LocalPawnRig {
    static const uint16_t kSlot = 7;

    FakeSpace space;
    FakeCs phys, player, bcast, echo, body, touch, model;
    FakeNetGame netGame;
    uint8_t game[64] = {};
    uint8_t keys[replay::localpawn_facts::kActorGuidSize * 2] = {};
    uint8_t attached[512] = {};
    std::vector<uint8_t> echoElem =
        std::vector<uint8_t>(replay::localpawn_facts::kEchoCiStride, 0);
    std::vector<uint8_t> bodyElem =
        std::vector<uint8_t>(replay::localpawn_facts::kBodyCiStride, 0);
    std::vector<uint8_t> touchElem =
        std::vector<uint8_t>(replay::localpawn_facts::kTouchAnimCiStride, 0);
    replay::entity::EngineEntryPoints fns;

    LocalPawnRig() {
        namespace lpf = replay::localpawn_facts;
        space.Construct(space.elements);
        void* gamePtr = game;
        memcpy(space.bytes + lpf::kGameSpaceGameOff, &gamePtr, sizeof gamePtr);

        phys.WirePhys();
        player.WirePlayer();
        g_physCs = &phys;
        g_playerCs = &player;
        g_bcastCs = &bcast;
        g_echoCs = &echo;
        g_bodyCs = &body;
        g_touchCs = &touch;
        g_modelCs = &model;

        netGame.PointAt(&space);
        netGame.SetLocalUser(kSlot);
        bcast.PutPtr(lpf::kBroadcasterNetGameOff, netGame.bytes);

        bcast.Wire(facts::PLAYER_CS_HANDLE_MAP_OFF, facts::PLAYER_CS_POOL_BASE_OFF,
                   facts::PLAYER_CS_COUNT_OFF, facts::PLAYER_CS_REVERSE_MAP_OFF, 1);
        memset(bcast.elements, 0, sizeof bcast.elements);
        *reinterpret_cast<uint32_t*>(bcast.elements + lpf::kBroadcasterUserIdOff) = kSlot;
        // The L2 shape by default: the pool is populated and the KEY ARRAY IS NULL.
        bcast.PutPtr(lpf::kBroadcasterKeysOff, nullptr);

        const uint64_t noConstraint = lpf::kNoConstraint;
        memcpy(attached + lpf::kAttachedConstraintOff, &noConstraint, sizeof noConstraint);
        void* attachedPtr = attached;
        memcpy(echoElem.data() + lpf::kEchoAttachedOff, &attachedPtr, sizeof attachedPtr);

        echo.PutPtr(lpf::kCsHandleMapOff, echo.handleMap);
        echo.PutPtr(lpf::kCsPoolBaseOff, echoElem.data());
        body.PutPtr(lpf::kCsHandleMapOff, body.handleMap);
        body.PutPtr(lpf::kCsPoolBaseOff, bodyElem.data());
        touch.PutPtr(lpf::kCsHandleMapOff, touch.handleMap);
        touch.PutPtr(lpf::kCsPoolBaseOff, touchElem.data());

        fns.findActorComponent = reinterpret_cast<replay::entity::AnyEngineFn>(&AnyComponentOnAnyCs);
        fns.echoBaseTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeTransform);
        fns.echoHeadTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeTransform);
        fns.echoHandTransform  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeHandTransform);
        fns.anchorBodyPart     = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeAnchor);
        fns.echoReferenceVel   = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeRefVel);
        fns.heldPlayer         = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeHeldPlayer);
    }

    ~LocalPawnRig() {
        ClearLeafSystems();
        g_touchCs = nullptr;
        g_physCs = nullptr;
        g_playerCs = nullptr;
        g_bcastCs = nullptr;
        replay::entity::EntityCreate(nullptr);
    }

    // A key array that names an actor, which is what a CLIENT's own pawn has.
    void PublishKeys() {
        memset(keys, 0, sizeof keys);
        keys[0] = 0xA1;
        keys[15] = 0x5C;
        bcast.PutPtr(replay::localpawn_facts::kBroadcasterKeysOff, keys);
    }
    void PublishZeroKeys() {
        memset(keys, 0, sizeof keys);
        bcast.PutPtr(replay::localpawn_facts::kBroadcasterKeysOff, keys);
    }
    void WithdrawKeys() {
        bcast.PutPtr(replay::localpawn_facts::kBroadcasterKeysOff, nullptr);
    }
    void UseCountingUserActor() {
        g_userActorCalls = 0;
        memset(g_userActorGuid, 0, sizeof g_userActorGuid);
        g_userActorGuid[0] = 0xB2;
        fns.userActor = reinterpret_cast<replay::entity::AnyEngineFn>(&CountingUserActor);
    }
    replay::IEntity* Create() {
        return replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
    }
};

const uint32_t kAllSevenSources = replay::kLocalPose | replay::kLocalRefVel | replay::kLocalBody |
                                 replay::kLocalTrigger | replay::kLocalAnchor | replay::kLocalHolding;

}  // namespace

TEST_CASE("kLocalAll is TOTAL over the declared group bits, by content and not by restatement") {
    // L2-RUN-2 F2: a consumer translated the mask group by group, kLocalHolding had no line, and a
    // sourced pawn came back as fill 0. The translator can only assert its table is complete against
    // a name that MEANS complete, so this pins what kLocalAll contains rather than repeating the OR
    // that defines it -- an assert spelled the same way as the definition cannot fail.
    unsigned bits = 0;
    for (unsigned b = 0; b < 32; ++b) {
        if (replay::kLocalAll & (1u << b)) ++bits;
    }
    CHECK(bits == 6u);
    CHECK(replay::kLocalAll < (1u << 6));   // no bit above kLocalHolding, so no unnamed group hides
    CHECK((replay::kLocalAll & replay::kLocalPose) != 0u);
    CHECK((replay::kLocalAll & replay::kLocalRefVel) != 0u);
    CHECK((replay::kLocalAll & replay::kLocalBody) != 0u);
    CHECK((replay::kLocalAll & replay::kLocalTrigger) != 0u);
    CHECK((replay::kLocalAll & replay::kLocalAnchor) != 0u);
    CHECK((replay::kLocalAll & replay::kLocalHolding) != 0u);
    CHECK(kAllSevenSources == replay::kLocalAll);
}

TEST_CASE("F4 retroactive: the L2 shape still refuses, and now names the arm that refused") {
    Env& e = TheEnv();
    e.Tick(nullptr);
    LocalPawnRig rig;
    // No second source: exactly what shipped in L2, where LOCAL.USER_ACTOR was resolved for
    // ModelVisible and never asked for the pose.
    rig.fns.userActor = nullptr;

    replay::IEntity* entity = rig.Create();
    REQUIRE(entity->Available());
    REQUIRE(entity->LocalPlayerCount() == 1);
    REQUIRE(entity->LocalPlayerAt(0).v == LocalPawnRig::kSlot);

    replay::PlayerState st{};
    replay::IEntity::LocalPlayerReading r;
    CHECK(entity->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r) == 0u);
    // The recorded verdict, and the facts the log could not print beside it.
    CHECK(r.fill == 0u);
    CHECK(r.refusal == replay::IEntity::LocalRefusal::ActorNull);
    CHECK(std::string(replay::LocalRefusalName(r.refusal)) == "actorNull");
    CHECK(r.actorSource == replay::IEntity::ActorSource::None);
    CHECK(r.bcastKeys == nullptr);
    CHECK(r.bcastCs != nullptr);
    CHECK(r.netGame != nullptr);
    // Not the same word as a pawn that was never in the pool: a four-state question, four answers.
    CHECK(entity->ReadLocalPlayerDetailed(replay::UserIndex(30), st, r) == 0u);
    CHECK(r.refusal == replay::IEntity::LocalRefusal::SlotNotInPool);
    CHECK(std::string(replay::LocalRefusalName(r.refusal)) == "slotNotInPool");
}

TEST_CASE("the local pawn's actor has two sources, and each rig resolves through its own") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    SUBCASE("HOST: the key array is not published, and CR15NetGame::UserActor answers") {
        LocalPawnRig rig;
        rig.UseCountingUserActor();
        replay::IEntity* entity = rig.Create();
        REQUIRE(entity->Available());

        replay::PlayerState st{};
        replay::IEntity::LocalPlayerReading r;
        const uint32_t filled =
            entity->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r);
        CHECK(r.refusal == replay::IEntity::LocalRefusal::Ok);
        CHECK(r.actorSource == replay::IEntity::ActorSource::NetGameUserActor);
        CHECK(std::string(replay::ActorSourceName(r.actorSource)) == "netGameUserActor");
        CHECK(r.bcastKeys == nullptr);
        CHECK_FALSE(r.actorSourcesDisagree);
        CHECK(r.echoComponent);
        CHECK(r.heldByFn);
        // Every group the L2 line listed as NOT sourced.
        CHECK((filled & kAllSevenSources) == kAllSevenSources);
        CHECK(filled == r.fill);
        CHECK(st.holdL == 0x1000u);
        CHECK(st.holdR == 0x1001u);
        CHECK(st.refVel[2] == 3.0f);
    }

    SUBCASE("CLIENT: the key array names the pawn, and it is the source that is used") {
        LocalPawnRig rig;
        rig.UseCountingUserActor();
        rig.PublishKeys();
        replay::IEntity* entity = rig.Create();
        REQUIRE(entity->Available());

        replay::PlayerState st{};
        replay::IEntity::LocalPlayerReading r;
        const uint32_t filled =
            entity->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r);
        CHECK(r.refusal == replay::IEntity::LocalRefusal::Ok);
        CHECK(r.actorSource == replay::IEntity::ActorSource::BroadcasterKeys);
        CHECK(r.bcastKeys == static_cast<const void*>(rig.keys));
        CHECK((filled & kAllSevenSources) == kAllSevenSources);
        // Both sources answered and they name different actors. That is not a refusal -- the key
        // array is the one the engine's own broadcast loop indexes -- but it is a fact no mask
        // could carry, and a run that reports it is reporting a real disagreement.
        CHECK(r.actorSourcesDisagree);
    }

    SUBCASE("an all-zero key entry is the UNSET guid, not an actor, and the second source answers") {
        LocalPawnRig rig;
        rig.UseCountingUserActor();
        rig.PublishZeroKeys();
        replay::IEntity* entity = rig.Create();
        REQUIRE(entity->Available());

        replay::PlayerState st{};
        replay::IEntity::LocalPlayerReading r;
        const uint32_t filled =
            entity->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r);
        CHECK(r.bcastKeys == static_cast<const void*>(rig.keys));
        CHECK(r.actorSource == replay::IEntity::ActorSource::NetGameUserActor);
        CHECK((filled & kAllSevenSources) == kAllSevenSources);
    }
}

TEST_CASE("an actor that appears after the bind is picked up, and is then never re-asked") {
    Env& e = TheEnv();
    e.Tick(nullptr);
    LocalPawnRig rig;
    rig.fns.userActor = nullptr;

    replay::IEntity* entity = rig.Create();
    REQUIRE(entity->Available());
    replay::PlayerState st{};
    replay::IEntity::LocalPlayerReading r;

    // Frame 1: neither source has an actor yet.
    CHECK(entity->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r) == 0u);
    CHECK(r.refusal == replay::IEntity::LocalRefusal::ActorNull);

    // Frame 2, SAME binding: the key array is published. A resolve that latched its first answer
    // -- the mutant this case exists for -- still says actorNull here.
    rig.PublishKeys();
    const uint32_t filled =
        entity->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r);
    CHECK(r.refusal == replay::IEntity::LocalRefusal::Ok);
    CHECK(r.actorSource == replay::IEntity::ActorSource::BroadcasterKeys);
    CHECK((filled & kAllSevenSources) == kAllSevenSources);

    // Frame 3: the array goes away again. A resolve that re-asked every frame goes back to
    // actorNull here; the shipping one answers from the actor it already has.
    rig.WithdrawKeys();
    CHECK(entity->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r) != 0u);
    CHECK(r.actorSource == replay::IEntity::ActorSource::BroadcasterKeys);

    // A NEW binding cannot inherit it: the GUID was minted against the old game space.
    replay::IEntity* rebound = rig.Create();
    REQUIRE(rebound->Available());
    CHECK(rebound->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r) == 0u);
    CHECK(r.refusal == replay::IEntity::LocalRefusal::ActorNull);
}

TEST_CASE("the second source is asked once per binding, not once per frame") {
    Env& e = TheEnv();
    e.Tick(nullptr);
    LocalPawnRig rig;
    rig.UseCountingUserActor();
    replay::IEntity* entity = rig.Create();
    REQUIRE(entity->Available());

    replay::PlayerState st{};
    replay::IEntity::LocalPlayerReading r;
    for (int frame = 0; frame < 5; ++frame) {
        CHECK(entity->ReadLocalPlayerDetailed(replay::UserIndex(LocalPawnRig::kSlot), st, r) != 0u);
    }
    CHECK(g_userActorCalls == 1);
}

TEST_CASE("an IEntity that predates the detailed reading says notAsked, and never invents a cause") {
    struct Old final : replay::IEntity {
        size_t PhysCount() const override { return 0; }
        bool   IsLocallyControlled(replay::PhysHandle) const override { return false; }
        void   SetSyntheticOwner(replay::PhysHandle) override {}
        void   ClearSyntheticOwner(replay::PhysHandle) override {}
        bool   ReadBody(replay::PhysHandle, replay::BodyState&) const override { return false; }
        void   PushHistory(replay::PhysHandle, const replay::BodyState&, uint64_t) override {}
        void   TrimHistory(replay::PhysHandle, uint64_t, uint64_t) override {}
        float  GetTimeOffset(replay::PhysHandle) const override { return 0.0f; }
        void   SetTimeOffset(replay::PhysHandle, float) override {}
        size_t PlayerCount() const override { return 0; }
        bool   ReadPlayer(replay::PlayerHandle, replay::PlayerState&) const override { return false; }
        void   PushPlayerHistory(replay::PlayerHandle, const replay::PlayerState&, uint64_t) override {}
        replay::PhysHandle   PhysOf(replay::PlayerHandle) const override { return replay::PhysHandle(0xFFFFu); }  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
        replay::PlayerHandle PlayerOf(replay::UserIndex) const override { return replay::PlayerHandle(0xFFFFu); }  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
        void   SetLodThrottle(bool) override {}
        bool   Available() const override { return false; }
        const char* Diagnostic() const override { return "old"; }
    };
    Old old;
    replay::PlayerState st{};
    replay::IEntity::LocalPlayerReading r;
    CHECK(old.ReadLocalPlayerDetailed(replay::UserIndex(0), st, r) == 0u);
    CHECK(r.refusal == replay::IEntity::LocalRefusal::NotAsked);
    CHECK(std::string(replay::LocalRefusalName(r.refusal)) == "notAsked");
    CHECK(r.actorSource == replay::IEntity::ActorSource::None);
}


// =================================================================================================
// B1: the LIVE state of a LOCALLY simulated body.
//
// GatherBodies asks ReadBody for every element of the physics pool. Before this, ReadBody read the
// newest HISTORY node and nothing else, so a body this machine simulates -- on a solo capture,
// EVERY body -- was refused and no PHYS_BODY sample was ever written. These cases drive both arms
// off one pool and pin the sum that makes the two sources comparable.
// =================================================================================================
namespace w1e_localbody {

namespace lpf = replay::localpawn_facts;

void PutVec3(uint8_t* at, float a, float b, float c) {
    const float v[3] = {a, b, c};
    memcpy(at, v, sizeof v);
}

// PHYS.ANGVEL_FROM_CLOUD stands in for the engine's sub_140643B30, which least-squares an angular
// velocity out of a CPhBody's particle cloud -- structure no fixture can build. The stand-in
// forwards the contract exactly as the call site at 0x140D7EF6A shows it: the result travels
// through the caller's buffer and the same pointer comes back. It records what it was handed, so
// a reader that calls with the wrong pointer, twice, or not at all is distinguishable.
const float kFakeAngVel[3] = {-0.125f, 6.5f, -11.25f};
int         g_angVelCalls = 0;
const void* g_angVelCallBody = nullptr;
const float* FakeAngVelFromCloud(void* body, float* out) {
    ++g_angVelCalls;
    g_angVelCallBody = body;
    memcpy(out, kFakeAngVel, sizeof kFakeAngVel);
    return out;
}
void PutVec4(uint8_t* at, float a, float b, float c, float d) {
    const float v[4] = {a, b, c, d};
    memcpy(at, v, sizeof v);
}

template <typename T>
void Put(uint8_t* at, T v) { memcpy(at, &v, sizeof v); }
void PutPtr(uint8_t* at, void* p) { memcpy(at, &p, sizeof p); }

// The CPhysicsCS half of the chain. ReadLocalBody WALKS this now rather than calling into it, so
// the fixture has to be the memory the walk reads: a handle map, a live count, and a 0x78-strided
// pool whose element carries the has-a-primary-body guard and the 24-byte CPhTarget.
// The space the net physics CS hangs off, its component-system registry, and the ncaPhysics
// LIST that registry hands back. Two component systems are in that list and the local pawn's
// component is live only in the SECOND, so a reader that takes the first -- or that takes the
// single cached pointer at PHYS.NETCS_PHYSICS_CS_OFF, which this fixture aims at the FIRST --
// reads a component system that does not own the entity. That is L2 #12's badSlot, planted.
struct FakeCsSpace {
    alignas(16) uint8_t bytes[0x4A0] = {};   // the registry count at +0x490 is the deepest field
};

struct FakeCsRegistry {
    static constexpr uint64_t kOtherSymbol = 0x1111222233334444ull;
    alignas(16) uint8_t records[facts::CS_LIST_RECORD_STRIDE * 2] = {};
    uint8_t* Record(size_t i) { return records + facts::CS_LIST_RECORD_STRIDE * i; }
    // What the engine's search returns: the VALUE half of the record, not the record.
    uint8_t* Entry(size_t i) { return Record(i) + facts::CS_LIST_RECORD_VALUE_OFF; }
};

struct FakeCsList {
    // Four slots so a dropped count bound has somewhere mapped to walk into: the test that
    // over-declares the count leaves entries 2 and 3 null rather than off the end of an array.
    void* arr[4] = {};
};

struct FakePhysicsCs {
    alignas(16) uint8_t bytes[0x140] = {};  // facts-ok: PHYS.CS_POOL_BASE_OFF 0xF8 V-PRIMARY is the deepest field planted
    alignas(16) uint8_t pool[facts::PHYS_PHYSICS_CI_STRIDE * 8] = {};
    // The handle map is 4 bytes per entry -- PHYS.CS_HANDLE_MAP_OFF says stride 4 for a u16
    // handle -- so a u16 array here would plant every entry at half the offset the walk reads.
    uint32_t handleMap[8] = {};
    // The object PrimaryBody loads at +80h and tests bit 0 of at +0C40h before it applies its
    // live-count bound. Bit 0 clear is the arm that applies it.
    alignas(16) uint8_t state[lpf::kPhysicsCsStateFlagsOff + 8] = {};
    uint8_t* Flags() { return state + lpf::kPhysicsCsStateFlagsOff; }
    uint8_t* Ci(uint16_t slot) { return pool + facts::PHYS_PHYSICS_CI_STRIDE * slot; }
};

// The CPhSystem half: a zone table and, inside the chosen zone, a body array. Both indices are
// deliberately NON-ZERO, so a mutant that drops either stride lands on an element the fixture
// never planted rather than on the one it did.
struct FakePhysWorld {
    static constexpr uint32_t kZoneCount = 2u, kBodyCount = 2u, kZoneIndex = 1u, kBodyIndex = 1u;
    alignas(16) uint8_t system[0x48] = {};  // facts-ok: PHYS.SYSTEM_ZONE_COUNT_OFF 0x38 V-PRIMARY is the deepest field planted
    alignas(16) uint8_t zones[facts::PHYS_ZONE_STRIDE * kZoneCount + 0x300] = {};  // facts-ok: PHYS.ZONE_BODY_COUNT_OFF 0x298 V-PRIMARY is the deepest field planted
    alignas(16) uint8_t bodies[facts::PHYS_BODY_STRIDE * kBodyCount + lpf::kPhBodyReadExtent] = {};
    uint8_t* Zone(uint32_t i) { return zones + static_cast<size_t>(facts::PHYS_ZONE_STRIDE) * i; }
    uint8_t* Body(uint32_t i) { return bodies + static_cast<size_t>(facts::PHYS_BODY_STRIDE) * i; }
};

// The ncaTransform CS: the SAME CComponentSystem shape as the physics one at different
// displacements, so a reader that took the physics CS's numbers for it would read the wrong
// fields of the right object. The handle map is 4 bytes per entry and the HIGH half addresses a
// second instance that resolves to a pose of its own -- a wrong-half read then answers with the
// wrong transform instead of refusing, which is the only shape that can fail it.
struct FakeXformCs {
    alignas(16) uint8_t bytes[0x180] = {};   // facts-ok: XFORM.CS_POOL_BASE_OFF 0xF8 is the deepest field planted
    alignas(16) uint8_t pool[lpf::kXformCsInstStride * 8] = {};
    uint32_t handleMap[16] = {};
    uint8_t* Inst(uint16_t i) { return pool + lpf::kXformCsInstStride * i; }
};

// The CNode3D side: one owner block of 48-byte entries and four nodes that index into it. Every
// node carries the class id the walk's terminator tests, so a case that wants the chain to END
// plants a different one rather than a null pointer -- the two are different reasons to stop.
struct FakeNodeTree {
    static constexpr uint32_t kNodes = 4u;
    alignas(16) uint8_t owner[lpf::kXformBlockBaseOff + 16] = {};
    alignas(16) uint8_t entries[lpf::kXformEntryStride * kNodes] = {};
    alignas(16) uint8_t node[kNodes][0x90] = {};

    uint8_t* Entry(uint32_t i) { return entries + lpf::kXformEntryStride * i; }
    uint8_t* Node(uint32_t i) { return node[i]; }

    void Wire() {
        PutPtr(owner + lpf::kXformBlockBaseOff, entries);
        for (uint32_t i = 0; i < kNodes; ++i) {
            Put<uint64_t>(node[i] + lpf::kNode3dClassIdOff, lpf::kNode3dClassId);
            PutPtr(node[i] + lpf::kNode3dXformOwnerOff, owner);
            Put<uint16_t>(node[i] + lpf::kNode3dXformIdxOff, static_cast<uint16_t>(i));
            PutVec4(Entry(i) + lpf::kXformEntryQuatOff, 0.0f, 0.0f, 0.0f, 1.0f);
            PutVec3(Entry(i) + lpf::kXformEntryPosOff, 0.0f, 0.0f, 0.0f);
            PutVec3(Entry(i) + lpf::kXformEntryScaleOff, 1.0f, 1.0f, 1.0f);
        }
    }
    void SetParent(uint32_t child, void* parent) {
        PutPtr(node[child] + lpf::kNode3dParentOff, parent);
    }
};

// ENTITY.LIST_UNLINK and ENTITY.HISTORY_RECYCLE, the pair every push into a CI's history ring
// goes through. Both are resolved out of the mapped image behind prologue guards, so on a test
// host both are null, SetHistoryWindowDetailed stops at NoRecycler before one node is pushed, and
// NO fixture in this tree could complete an install -- which is why the gate vouching for one
// element while the ring nodes land on another (AUD-W2-B-POSEREAD blocker 1) was unobservable.
// These two stand in for the engine's, over the fixture's own memory and through the same
// displacements the shipping code uses, so a fact that moves moves the fixture with it.
void* GetPtr(void* base, uint32_t off) {
    void* p = nullptr;
    memcpy(&p, static_cast<uint8_t*>(base) + off, sizeof p);
    return p;
}
void SetPtr(void* base, uint32_t off, void* p) {
    memcpy(static_cast<uint8_t*>(base) + off, &p, sizeof p);
}

// The list, as entity.cpp's own linking spells it: a container's HEAD is the NEWEST node and its
// TAIL the oldest, a node's PREV walks toward the head and its NEXT toward the tail. Unlink is
// handed the node alone, so it reaches its list the only way it can -- the node's own owner
// container field, which PushHistoryInto sets on every node it links.
void UnlinkNode(void* node) {
    void* cont  = GetPtr(node, facts::PHYS_NODE_OWNER_CONTAINER_OFF);
    void* newer = GetPtr(node, facts::PHYS_NODE_PREV_OFF);
    void* older = GetPtr(node, facts::PHYS_NODE_NEXT_OFF);
    if (newer)     SetPtr(newer, facts::PHYS_NODE_NEXT_OFF, older);
    else if (cont) SetPtr(cont, facts::PHYS_LIST_HEAD_PTR_OFF, older);
    if (older)     SetPtr(older, facts::PHYS_NODE_PREV_OFF, newer);
    else if (cont) SetPtr(cont, facts::PHYS_LIST_TAIL_PTR_OFF, newer);
    SetPtr(node, facts::PHYS_NODE_NEXT_OFF, nullptr);
    SetPtr(node, facts::PHYS_NODE_PREV_OFF, nullptr);
    SetPtr(node, facts::PHYS_NODE_OWNER_CONTAINER_OFF, nullptr);
}

// The free list is a container whose HEAD pointer lies at PHYS.CS_FREE_TAKE_OFF. That aliasing is
// not decoration: entity.cpp takes a node with Deref(cs, FREE_TAKE) and then calls unlink_(node),
// which is handed no component system at all, so the slot it takes from has to BE the head of the
// list the node's owner-container field names. Nothing else makes the pair consistent.
uint8_t* FreeContainer(void* cs) {
    return static_cast<uint8_t*>(cs) + facts::PHYS_CS_FREE_TAKE_OFF - facts::PHYS_LIST_HEAD_PTR_OFF;
}
void LinkAsHead(void* container, void* node) {
    void* oldHead = GetPtr(container, facts::PHYS_LIST_HEAD_PTR_OFF);
    if (oldHead) SetPtr(oldHead, facts::PHYS_NODE_PREV_OFF, node);
    SetPtr(node, facts::PHYS_NODE_NEXT_OFF, oldHead);
    SetPtr(node, facts::PHYS_NODE_PREV_OFF, nullptr);
    SetPtr(node, facts::PHYS_NODE_OWNER_CONTAINER_OFF, container);
    SetPtr(container, facts::PHYS_LIST_HEAD_PTR_OFF, node);
    if (!GetPtr(container, facts::PHYS_LIST_TAIL_PTR_OFF))
        SetPtr(container, facts::PHYS_LIST_TAIL_PTR_OFF, node);
}

int g_unlinkCalls = 0, g_recycleCalls = 0;
const void* g_lastRecycleCi = nullptr;

void FakeListUnlink(void* node) {
    ++g_unlinkCalls;
    UnlinkNode(node);
}

// Retire one node off a CI: out of the CI's list, one off the CI's own count -- the count
// PushHistoryInto increments and SetHistoryWindowDetailed differences -- and back onto the free
// list. Which CI it was handed is recorded, because a recycle aimed at the element the gate did
// not vouch for is exactly the fault the cases below are written to catch.
void FakeHistoryRecycle(void* cs, void* ci, void* node) {
    ++g_recycleCalls;
    g_lastRecycleCi = ci;
    UnlinkNode(node);
    uint64_t count = 0;
    memcpy(&count, static_cast<uint8_t*>(ci) + facts::PHYS_CI_HIST_COUNT_OFF, sizeof count);
    if (count) --count;
    memcpy(static_cast<uint8_t*>(ci) + facts::PHYS_CI_HIST_COUNT_OFF, &count, sizeof count);
    LinkAsHead(FreeContainer(cs), node);
}

// Handle 0 is net-driven: a history container with a head node on it. Handle 1 is locally
// simulated: no history at all, and a body id the physics CS resolves. Every offset is the fact
// the shipping reader uses, so a fact that moves moves the fixture with it.
struct Fixture {
    FakeSpace space;
    FakeCs    phys, player;
    alignas(16) uint8_t container[0x40] = {};
    alignas(16) uint8_t node[0x80] = {};
    FakePhysicsCs physicsCs;
    // The component system the list offers FIRST and the cached pointer names: it is a real
    // CPhysicsCS with a real handle map and pool, and its live count is 0, so HasComponent
    // answers false for this entity while every displacement below it still resolves.
    std::unique_ptr<FakePhysicsCs> decoyCs{new FakePhysicsCs()};
    FakeCsSpace    csSpace;
    FakeCsRegistry registry;
    FakeCsList     csList;
    FakePhysWorld world;
    // Left UNWIRED by default: netCS + XFORM.NETCS_XFORM_CS_OFF is null unless a case calls
    // WireTransform(), so every case written before the fallback existed keeps the shape it was
    // written for and the fallback cannot answer behind their backs.
    FakeXformCs   xform;
    FakeNodeTree  tree;
    uint8_t* localBody = nullptr;
    replay::entity::EngineEntryPoints fns;

    // The physics slot is 3 and the body id is 1, so the handle map is doing real work: a walk
    // that skipped it and used the body id as the slot would read pool element 1.
    static constexpr uint16_t kLocalBodyId = 1u, kPhysicsSlot = 3u, kPhysicsCount = 8u;
    static constexpr uint32_t kTag = 0x5EED1234u;
    // The high half of the map entry. It is a SECOND pool slot that resolves all the way to a
    // body of its own, so a reader that takes the wrong half of the entry answers with the wrong
    // transform instead of refusing -- the only fixture shape a wrong-half read cannot survive.
    static constexpr uint16_t kDecoySlot = 5u;
    static constexpr uint32_t kDecoyBodyIndex = 0u;

    uint8_t* Ci(uint16_t index) { return phys.elements + facts::PHYS_CI_STRIDE * index; }
    uint8_t* PhysicsCi() { return physicsCs.Ci(kPhysicsSlot); }
    uint8_t* Target() { return PhysicsCi() + facts::PHYS_PHYSICS_CI_TARGET_OFF; }
    uint8_t* Zone() { return world.Zone(FakePhysWorld::kZoneIndex); }
    uint8_t* DecoyCi() { return physicsCs.Ci(kDecoySlot); }
    uint8_t* DecoyTarget() { return DecoyCi() + facts::PHYS_PHYSICS_CI_TARGET_OFF; }
    // The com the walk must produce: PHYS.BODY_POS_OFF + PHYS.BODY_COM_OFFSET_OFF, exact in
    // binary32, so the compare is an equality.
    static void ExpectedCom(float out[3]) {
        out[0] = 10.0f + 0.125f;
        out[1] = -4.5f + 0.25f;
        out[2] = 0.75f + -0.5f;
    }

    Fixture() {
        space.Construct(space.elements);
        phys.Wire(facts::PHYS_CS_HANDLE_MAP_OFF, facts::PHYS_CS_POOL_BASE_OFF,
                  facts::PHYS_CS_COUNT_OFF, facts::PHYS_CS_REVERSE_MAP_OFF, 2);
        player.WirePlayer();
        phys.handleMap[0] = 0;
        phys.handleMap[1] = 1;
        phys.reverseMap[0] = 0;
        phys.reverseMap[1] = 1;
        void* n = node;
        memcpy(container + facts::PHYS_LIST_HEAD_PTR_OFF, &n, sizeof n);
        void* c = container;
        memcpy(Ci(0) + facts::PHYS_CI_HIST_LIST_OFF, &c, sizeof c);

        const uint32_t histFlags = 0xC0DE0001u, localFlags = 0xC0DE0002u;
        memcpy(Ci(0) + facts::PHYS_CI_FLAGS_OFF, &histFlags, sizeof histFlags);
        memcpy(Ci(1) + facts::PHYS_CI_FLAGS_OFF, &localFlags, sizeof localFlags);
        const uint32_t localBodyId = 1u;
        memcpy(Ci(1) + lpf::kPhysCiBodyIdOff, &localBodyId, sizeof localBodyId);

        // The CI's own entity id -- the first field InitNetPhysicsCI writes -- and the reverse map
        // that has to agree with it. Handle 1's CI carries 1: a fixture that left this zero would
        // pass the identity gate only by being handle 0, and could not fail a reader that walked
        // the wrong element.
        Put<uint16_t>(Ci(0) + lpf::kNetCiEntityIdOff, 0u);
        Put<uint16_t>(Ci(1) + lpf::kNetCiEntityIdOff, 1u);

        PutVec3(node + facts::PHYS_NODE_COM_OFF,      1.5f, -2.25f, 3.125f);
        PutVec4(node + facts::PHYS_NODE_ORI_OFF,      0.25f, -0.5f, 0.75f, -0.125f);
        PutVec3(node + facts::PHYS_NODE_VEL_OFF,      11.0f, -12.0f, 13.0f);
        PutVec3(node + facts::PHYS_NODE_ANGVEL_OFF,   -14.0f, 15.0f, -16.0f);
        PutVec3(node + facts::PHYS_NODE_LASTHELD_OFF, 17.0f, 18.0f, 19.0f);

        // Both terms of the sum are non-zero, differ from each other, and differ per component:
        // a fixture whose comOffset is zero cannot fail against a reader that forgot the sum.
        // The sums are exact in binary32, so the compare below is an equality and not an epsilon.
        // CPhysicsCS::PrimaryBody's four steps, as memory: handle map, live count, pool base,
        // and the element's own has-a-body guard plus its CPhTarget.
        physicsCs.handleMap[kLocalBodyId] =
            static_cast<uint32_t>(kPhysicsSlot) | (static_cast<uint32_t>(kDecoySlot) << 16);
        Put<uint16_t>(physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, kPhysicsCount);
        PutPtr(physicsCs.bytes + lpf::kPhysicsCsStateObjOff, physicsCs.state);
        Put<uint64_t>(DecoyCi() + facts::PHYS_PHYSICS_CI_BODY_GUARD_OFF, 1u);
        Put<uint32_t>(DecoyTarget() + facts::PHYS_TARGET_INDEX_OFF, kDecoyBodyIndex);
        Put<uint32_t>(DecoyTarget() + facts::PHYS_TARGET_ZONE_OFF, FakePhysWorld::kZoneIndex);
        Put<uint32_t>(DecoyTarget() + lpf::kPhTargetTagOff, kTag);
        uint8_t* decoy = world.Body(kDecoyBodyIndex);
        Put<uint32_t>(decoy + lpf::kPhIdIndexOff, kDecoyBodyIndex);
        Put<uint32_t>(decoy + facts::PHYS_ID_TAG_OFF, kTag);
        PutVec3(decoy + lpf::kPhBodyPosOff, -99.0f, 42.0f, 7.25f);
        PutVec3(decoy + lpf::kPhBodyComOffsetOff, 1.0f, -1.0f, 2.0f);
        PutPtr(physicsCs.bytes + facts::PHYS_CS_HANDLE_MAP_OFF, physicsCs.handleMap);
        PutPtr(physicsCs.bytes + facts::PHYS_CS_POOL_BASE_OFF, physicsCs.pool);
        Put<uint64_t>(PhysicsCi() + facts::PHYS_PHYSICS_CI_BODY_GUARD_OFF, 1u);
        Put<uint32_t>(Target() + facts::PHYS_TARGET_INDEX_OFF, FakePhysWorld::kBodyIndex);
        Put<uint32_t>(Target() + facts::PHYS_TARGET_ZONE_OFF,  FakePhysWorld::kZoneIndex);
        Put<uint32_t>(Target() + lpf::kPhTargetTagOff, kTag);

        // CPhTarget::ResolveBody's five: zone array, zone count, the zone's own id header, its
        // body count and its body array.
        PutPtr(world.system + facts::PHYS_SYSTEM_ZONE_ARRAY_OFF, world.Zone(0));
        Put<uint64_t>(world.system + facts::PHYS_SYSTEM_ZONE_COUNT_OFF, FakePhysWorld::kZoneCount);
        Put<uint32_t>(Zone() + lpf::kPhIdIndexOff, FakePhysWorld::kZoneIndex);
        Put<uint64_t>(Zone() + facts::PHYS_ZONE_BODY_COUNT_OFF, FakePhysWorld::kBodyCount);
        PutPtr(Zone() + facts::PHYS_ZONE_BODY_ARRAY_OFF, world.Body(0));

        localBody = world.Body(FakePhysWorld::kBodyIndex);
        Put<uint32_t>(localBody + lpf::kPhIdIndexOff, FakePhysWorld::kBodyIndex);
        Put<uint32_t>(localBody + facts::PHYS_ID_TAG_OFF, kTag);
        PutVec3(localBody + lpf::kPhBodyPosOff,       10.0f, -4.5f, 0.75f);
        PutVec3(localBody + lpf::kPhBodyComOffsetOff, 0.125f, 0.25f, -0.5f);
        PutVec4(localBody + lpf::kPhBodyOriOff,       0.5f, -0.5f, 0.5f, 0.5f);
        PutVec3(localBody + lpf::kPhBodyVelOff,       7.0f, -8.0f, 9.0f);
        // PHYS.BODY_PREV_VEL_OFF -- the PREVIOUS tick linear velocity. Planted with a value no
        // other field carries, because the reader must NOT return it as angular velocity.
        PutVec3(localBody + lpf::kPhBodyPrevVelOff,   -1.0f, 2.0f, -3.0f);
        // PHYS.ANGVEL_FROM_CLOUD. The engine derives angular velocity by least squares over the
        // particle cloud; no fixture can model that, so the fake stands in for the call and
        // records the pointer it was handed. Its triple is unlike every planted field, so a
        // reader that answers angVel from ANY of them is caught by identity rather than by a
        // zero test -- and so is a reader that never calls at all.
        g_angVelCalls = 0;
        g_angVelCallBody = nullptr;
        fns.physAngVelFromCloud = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeAngVelFromCloud);

        WirePhysicsCsList();
        fns.phSystem = world.system;
    }

    // The engine's own resolution path: space -> registry -> the ncaPhysics LIST -> the first
    // component system that claims the entity. The cached pointer is deliberately aimed at the
    // DECOY, because that is exactly the state the finding describes -- the cache naming a
    // component system that does not own the entity, in the right space.
    void WirePhysicsCsList(uint64_t declaredCount = 2u) {
        PutPtr(decoyCs->bytes + facts::PHYS_CS_HANDLE_MAP_OFF, decoyCs->handleMap);
        PutPtr(decoyCs->bytes + facts::PHYS_CS_POOL_BASE_OFF, decoyCs->pool);
        PutPtr(decoyCs->bytes + lpf::kPhysicsCsStateObjOff, decoyCs->state);
        decoyCs->handleMap[kLocalBodyId] = kPhysicsSlot;
        Put<uint16_t>(decoyCs->bytes + facts::PHYS_CS_COUNT_OFF, 0u);   // claims nothing

        csList.arr[0] = decoyCs->bytes;
        csList.arr[1] = physicsCs.bytes;
        // A one-entry list has no decoy to be told apart from: it holds the real component system.
        if (declaredCount == 1u) csList.arr[0] = physicsCs.bytes;
        PutPtr(registry.Entry(0) + 0u, static_cast<void*>(csList.arr));
        Put<uint64_t>(registry.Entry(0) + facts::CS_LIST_COUNT_OFF, declaredCount);
        Put<uint64_t>(registry.Record(0) + 0u, facts::CS_SYM_NCAPHYSICS);
        // A second record under another symbol, so the key comparison is doing real work and a
        // reader that returned record 0 regardless would answer this one when it is first.
        Put<uint64_t>(registry.Record(1) + 0u, FakeCsRegistry::kOtherSymbol);
        PutPtr(csSpace.bytes + facts::CS_LIST_TABLE_OFF, static_cast<void*>(registry.records));
        Put<uint64_t>(csSpace.bytes + facts::CS_LIST_TABLE_COUNT_OFF, 2u);
        phys.PutPtr(facts::PHYS_PHYSICS_CS_STATE_OBJ_OFF, csSpace.bytes);
        phys.PutPtr(lpf::kNetCsPhysicsCsOff, decoyCs->bytes);
    }

    // The ncaTransform chain, as GetEntityPosition walks it. The entity id is taken from the
    // CI's own ncaTransform ref and is deliberately NEITHER the handle NOR the body id, so a
    // reader that reached for the wrong CI field lands on an entry this fixture never planted.
    static constexpr uint16_t kXformEntity = 2u, kXformIndex = 3u, kXformDecoyIndex = 6u,
                              kXformLive   = 5u;
    void WireTransform() {
        PutPtr(xform.bytes + lpf::kXformCsHandleMapOff, xform.handleMap);
        PutPtr(xform.bytes + lpf::kXformCsPoolBaseOff, xform.pool);
        Put<uint16_t>(xform.bytes + lpf::kXformCsCountOff, kXformLive);
        xform.handleMap[kXformEntity] = static_cast<uint32_t>(kXformIndex) |
                                        (static_cast<uint32_t>(kXformDecoyIndex) << 16);
        Put<uint32_t>(Ci(1) + lpf::kNetCiXformRefOff, kXformEntity);
        // 7a: the instance carries the transform outright. x,y are one load and z another four
        // bytes on, so a fixture that planted a single 12-byte triple would pass a reader that
        // read the position the wrong way.
        PutVec4(xform.Inst(kXformIndex) + lpf::kXformInstRotOff, 0.0f, 0.0f, 1.0f, 0.0f);
        PutVec3(xform.Inst(kXformIndex) + lpf::kXformInstPosOff, 1.5f, -2.5f, 3.5f);
        // The decoy the HIGH half of the map entry addresses: a complete, different pose.
        PutVec4(xform.Inst(kXformDecoyIndex) + lpf::kXformInstRotOff, 1.0f, 0.0f, 0.0f, 0.0f);
        PutVec3(xform.Inst(kXformDecoyIndex) + lpf::kXformInstPosOff, -99.0f, 77.0f, -55.0f);
        tree.Wire();
        phys.PutPtr(lpf::kNetCsTransformCsOff, xform.bytes);
    }

    // The POOL reading, wired: the CI at index 1 is live and its id has no forward-map entry the
    // live count accepts, because it was never given a net handle. That is the local pawn's own
    // shape -- the only one whose physics-arm refusals fall through to the transform chain.
    void WirePoolWalked() {
        phys.handleMap[1]  = 9u;   // past the live count of 2 -- HasComponent refuses it
        phys.reverseMap[0] = 4u;   // so entity 0 at index 1 is unique over the live range
        phys.reverseMap[1] = 0u;
    }

    // The solo shape: one body, locally simulated, no history anywhere. This is the owner
    // recording himself, and it is the capture that produced zero body samples.
    void MakeSolo() {
        memset(Ci(0) + facts::PHYS_CI_HIST_LIST_OFF, 0, sizeof(void*));
        const uint32_t localBodyId = 1u;
        memcpy(Ci(0) + lpf::kPhysCiBodyIdOff, &localBodyId, sizeof localBodyId);
        const uint16_t one = 1;
        memcpy(phys.bytes + facts::PHYS_CS_COUNT_OFF, &one, sizeof one);
    }

    // Four spare nodes, which is more than any case here installs and few enough that a window
    // asking for more would exhaust the pool rather than silently succeed.
    static constexpr uint32_t kFreeNodes = 4u;
    alignas(16) uint8_t freeNodes[kFreeNodes][0x80] = {};

    // What makes an install COMPLETE on a test host: the unlink/recycle pair, and a free list for
    // them to work over. Opt-in, so every case written before it keeps the shape it was written
    // for -- with the pair null the install still stops at NoRecycler, which is what those cases
    // assert. It also finishes handle 0's ring: the constructor plants a head and no tail, and a
    // container with no tail is not a list ClearHistory can retire anything off.
    void WireRecycler() {
        SetPtr(container, facts::PHYS_LIST_HEAD_PTR_OFF, nullptr);
        SetPtr(container, facts::PHYS_LIST_TAIL_PTR_OFF, nullptr);
        LinkAsHead(container, node);
        uint8_t* freeCont = FreeContainer(phys.bytes);
        SetPtr(freeCont, facts::PHYS_LIST_HEAD_PTR_OFF, nullptr);
        SetPtr(freeCont, facts::PHYS_LIST_TAIL_PTR_OFF, nullptr);
        for (uint32_t i = 0; i < kFreeNodes; ++i) LinkAsHead(freeCont, freeNodes[i]);
        g_unlinkCalls = 0;
        g_recycleCalls = 0;
        g_lastRecycleCi = nullptr;
        fns.historyUnlink  = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeListUnlink);
        fns.historyRecycle = reinterpret_cast<replay::entity::AnyEngineFn>(&FakeHistoryRecycle);
    }

    replay::IEntity* Build() {
        g_physCs = &phys;
        g_playerCs = &player;
        return replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind, &fns);
    }
};

}  // namespace w1e_localbody

TEST_CASE("B1: one pool, two sources -- the net-driven body from history, the local one from the CPhBody") {
    namespace w = w1e_localbody;
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    w::Fixture f;
    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());
    CHECK(entity->PhysCount() == 2u);

    replay::BodyState hist{};
    replay::BodyReading hr;
    REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(0), hist, hr));
    CHECK(hr.source == replay::BodySource::History);
    CHECK(hr.ciResolved);
    CHECK(hr.historyPresent);
    CHECK_FALSE(hr.bodyResolved);
    CHECK(std::string(replay::BodySourceName(hr.source)) == "history");
    CHECK(memcmp(hist.com,         f.node + facts::PHYS_NODE_COM_OFF,      sizeof hist.com) == 0);
    CHECK(memcmp(hist.ori,         f.node + facts::PHYS_NODE_ORI_OFF,      sizeof hist.ori) == 0);
    CHECK(memcmp(hist.vel,         f.node + facts::PHYS_NODE_VEL_OFF,      sizeof hist.vel) == 0);
    CHECK(memcmp(hist.angVel,      f.node + facts::PHYS_NODE_ANGVEL_OFF,   sizeof hist.angVel) == 0);
    CHECK(memcmp(hist.lastHeldCom, f.node + facts::PHYS_NODE_LASTHELD_OFF, sizeof hist.lastHeldCom) == 0);
    CHECK(hist.bits11 == replay::BodyWireBits(0xC0DE0001u));

    replay::BodyState loc{};
    replay::BodyReading lr;
    REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), loc, lr));
    CHECK(lr.source == replay::BodySource::Local);
    CHECK(lr.ciResolved);
    CHECK_FALSE(lr.historyPresent);
    CHECK(lr.bodyResolved);
    CHECK(std::string(replay::BodySourceName(lr.source)) == "local");
    CHECK(memcmp(loc.ori,    f.localBody + lpf::kPhBodyOriOff,    sizeof loc.ori) == 0);
    CHECK(memcmp(loc.vel,    f.localBody + lpf::kPhBodyVelOff,    sizeof loc.vel) == 0);
    // The CPhBody stores NO angular velocity, and the netCS row that carries one is a per-call
    // TLS temporary, so the only durable thing to read is the FUNCTION. The local arm calls it
    // and reports EngineComputed; above all it must not return the previous-tick LINEAR velocity
    // at 0x91C, which is what it did while that displacement was filed as PHYS.BODY_ANGVEL_OFF.
    const float kNoAngVel[3] = {0.0f, 0.0f, 0.0f};
    CHECK(memcmp(loc.angVel, w::kFakeAngVel, sizeof loc.angVel) == 0);
    CHECK(memcmp(loc.angVel, kNoAngVel, sizeof loc.angVel) != 0);
    CHECK(memcmp(loc.angVel, f.localBody + lpf::kPhBodyPrevVelOff, sizeof loc.angVel) != 0);
    // Called exactly once, on the CPhBody the walk resolved -- not the CI, not a neighbouring
    // body. A reader that passed the wrong pointer would still return the fake's triple.
    CHECK(w::g_angVelCalls == 1);
    CHECK(w::g_angVelCallBody == f.localBody);
    CHECK(lr.diag.angVel == replay::AngVelSource::EngineComputed);
    CHECK((lr.diag.filled & replay::kBodyDiagAngVel) != 0);
    {
        char diagText[replay::kBodyDiagTextMax];
        const std::string rendered = replay::DescribeDiag(lr.diag, diagText, sizeof diagText);
        CHECK(rendered.find("angVel=engineComputed") != std::string::npos);
    }
    // The CPhBody does not carry it, and an honest zero is not a fabricated value.
    CHECK(loc.lastHeldCom[0] == 0.0f);
    CHECK(loc.lastHeldCom[1] == 0.0f);
    CHECK(loc.lastHeldCom[2] == 0.0f);
    // Taken from the CI by BOTH arms, so the two sources agree on this field by construction.
    CHECK(loc.bits11 == replay::BodyWireBits(0xC0DE0002u));

    // GatherBodies asks through ReadBody, and that is the call the recorder actually makes.
    replay::BodyState viaReadBody{};
    CHECK(entity->ReadBody(replay::PhysHandle(1), viaReadBody));
    CHECK(memcmp(&viaReadBody, &loc, sizeof loc) == 0);
}

// ENG-RESIDUALS-1 (c) / AUD-W2-B-POSEREAD blocker. THE INVARIANT: the element the identity gate
// vouched for and the element the pose is written to and read back from must be the SAME element,
// on BOTH arms of ResolveCi. On the POOL arm they are not: the gate vouches ElementAt(v)
// (entity.cpp:1349) while PushHistory and ReadHistoryWindow both re-resolve through
// PhysCs::Element(v) = ElementAt(handleMap[4*v]) (entity.cpp:207, :2088, :2228), which no bound and
// no gate constrains. A pose then lands in -- and is read back out of -- an element the identity
// proof never looked at.
//
// This case is RED at 503cd41 and is the acceptance criterion for the fix in flight on
// ENG-W2-B-POSEREAD's thread. It is written against the invariant, not against either code shape:
// the ungated surface must not answer out of the foreign element, whether the fix re-points the
// write and the read at the gated element or refuses the handle outright.
//
// The WRITE half cannot be driven on a test host: PushHistory needs unlink_/recycle_, and both come
// from Mapped(ENTITY_LIST_UNLINK / ENTITY_HISTORY_RECYCLE) (entity.cpp:913-915), which is null
// where no echovr.exe is mapped -- SetHistoryWindowDetailed stops at NoRecycler before the
// divergence. The READ half needs neither, and it addresses the same element the write does.
TEST_CASE("B1: the gate's element and the ring's element are the same element on the POOL arm") {
    namespace w = w1e_localbody;

    auto held = std::make_unique<w::Fixture>();
    w::Fixture& f = *held;

    // Handle 0's forward-map index is past the live count of 2, so ResolveCi falls to the POOL arm
    // and the identity gate vouches for element 0. PhysCs::Element(0) still reads the map and lands
    // on element 3 -- a real element of the same pool, so nothing faults and nothing refuses.
    f.phys.handleMap[0] = 3u;

    // The foreign element's own history, with a com no other node in this fixture carries.
    alignas(16) uint8_t foreignContainer[0x40] = {};
    alignas(16) uint8_t foreignNode[0x80] = {};
    const float kForeignCom[3] = {77.0f, 78.0f, 79.0f};
    memcpy(foreignNode + facts::PHYS_NODE_COM_OFF, kForeignCom, sizeof kForeignCom);
    void* fnode = foreignNode;
    memcpy(foreignContainer + facts::PHYS_LIST_HEAD_PTR_OFF, &fnode, sizeof fnode);
    void* fcont = foreignContainer;
    memcpy(f.Ci(3) + facts::PHYS_CI_HIST_LIST_OFF, &fcont, sizeof fcont);

    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());

    // The divergence is the one described, and not a broken fixture: the gate resolved this handle
    // by the POOL arm, at index 0.
    replay::BodyState s{};
    replay::BodyReading r;
    entity->ReadBodyDetailed(replay::PhysHandle(0), s, r);
    CHECK(r.diag.ciFrom == replay::CiPath::Pool);
    CHECK(r.diag.ciIndex == 0u);

    // THE INVARIANT. Whatever the ring hands back for handle 0, it must not be the history of an
    // element the identity proof never vouched for.
    replay::BodyState back[1]{};
    uint64_t backT[1] = {0};
    const size_t n = entity->ReadHistoryWindow(replay::PhysHandle(0), back, backT, 1);
    CHECK((n == 0 || back[0].com[0] != kForeignCom[0]));

    // And the install must not report a write into an element the gate did not vouch for.
    replay::InstallReading ir;
    const replay::BodyState sample{};
    const uint64_t whenUs = 99u;
    CHECK_FALSE(entity->SetHistoryWindowDetailed(replay::PhysHandle(0), &sample, &whenUs, 1, ir));
}

// AUD-W2-B-POSEREAD residual 3, and the case GATE_WRITE_ELEMENT_DIVERGE survived for want of: no
// fixture in this tree could COMPLETE an install, so the gate vouching for one element while the
// ring nodes landed on another was invisible to every suite. The pair above is what completes it.
//
// The CONTROL first, on the HANDLE arm, where ResolveCi and phys_.Element agree on the element by
// construction: it establishes that this rig installs at all. Without it, a case whose install
// merely refused for some other reason would look exactly like the one below passing.
TEST_CASE("B1: with a recycler and a free pool, an install COMPLETES into the gated element") {
    namespace w = w1e_localbody;
    using Refusal = replay::InstallRefusal;

    auto held = std::make_unique<w::Fixture>();
    w::Fixture& f = *held;
    f.WireRecycler();
    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());

    replay::BodyState sample{};
    sample.com[0] = 5.5f;  sample.com[1] = -6.25f;  sample.com[2] = 7.125f;
    sample.vel[1] = -30.5f;
    const uint64_t whenUs = 4242u;
    replay::InstallReading ir;
    CHECK(entity->SetHistoryWindowDetailed(replay::PhysHandle(0), &sample, &whenUs, 1, ir));
    CHECK(ir.why == Refusal::None);
    CHECK(ir.from == replay::CiPath::Handle);

    // The engine's own pair was CALLED, not merely present: one node off the free list per push,
    // and the constructor's node retired off this CI by ClearHistory.
    CHECK(w::g_unlinkCalls == 1);
    CHECK(w::g_recycleCalls == 1);
    CHECK(w::g_lastRecycleCi == f.Ci(0));

    // The ring, read as memory rather than through the reader that could agree with a wrong write:
    // exactly one node, carrying the sample, and the CI's own count says one.
    void* head = w::GetPtr(f.container, facts::PHYS_LIST_HEAD_PTR_OFF);
    REQUIRE(head != nullptr);
    // The retire put the constructor node back on the free list and the push took it straight
    // back, so node identity says nothing here; what must hold is that it belongs to THIS ring.
    CHECK(w::GetPtr(head, facts::PHYS_NODE_OWNER_CONTAINER_OFF) == static_cast<void*>(f.container));
    CHECK(memcmp(static_cast<uint8_t*>(head) + facts::PHYS_NODE_COM_OFF, sample.com,
                 sizeof sample.com) == 0);
    CHECK(w::GetPtr(head, facts::PHYS_NODE_NEXT_OFF) == nullptr);
    uint64_t histCount = 0;
    memcpy(&histCount, f.Ci(0) + facts::PHYS_CI_HIST_COUNT_OFF, sizeof histCount);
    CHECK(histCount == 1u);

    // And the reader agrees with the memory.
    replay::BodyState back[1]{};
    uint64_t backT[1] = {0};
    REQUIRE(entity->ReadHistoryWindow(replay::PhysHandle(0), back, backT, 1) == 1u);
    CHECK(memcmp(back[0].com, sample.com, sizeof sample.com) == 0);
    CHECK(backT[0] == whenUs);
}

// The installed node's SR15NetPhysicsSimState header, which is an ENGINE INPUT and not just our
// record: the remote-holder gate sub_140CED640 reads bit 0x200 of it through
// PhysCI+0x158 -> head node -> +0x20 before it will say a remote user holds the body. Until the
// recording's bits reached it, that gate answered no however truthful PhysCI+0x1C was.
//
// The two nibbles stay the engine's. Bits 0-3 are the ownership sequence and 4-7 the pose warp;
// UPDATE_TARGET_STATE compares the warp nibble cyclically against the CI's and a recorded value
// there would move it, which is the teleport handling the install exists to keep still.
TEST_CASE("B1c: the installed node carries the recording's state bits and the CI's warp nibble") {
    namespace w = w1e_localbody;

    auto held = std::make_unique<w::Fixture>();
    w::Fixture& f = *held;
    f.WireRecycler();
    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());

    const uint16_t kCiWarp = 0x50u;   // warp nibble 5, which the recording below does NOT carry
    uint32_t ciFlags = 0;
    memcpy(&ciFlags, f.Ci(0) + facts::PHYS_CI_FLAGS_OFF, sizeof ciFlags);
    ciFlags = (ciFlags & ~0xF0u) | kCiWarp;
    memcpy(f.Ci(0) + facts::PHYS_CI_FLAGS_OFF, &ciFlags, sizeof ciFlags);

    replay::BodyState sample{};
    sample.bits11 = 0x7FFu;           // every wire bit, both nibbles included
    const uint64_t whenUs = 4243u;
    replay::InstallReading ir;
    REQUIRE(entity->SetHistoryWindowDetailed(replay::PhysHandle(0), &sample, &whenUs, 1, ir));

    void* head = w::GetPtr(f.container, facts::PHYS_LIST_HEAD_PTR_OFF);
    REQUIRE(head != nullptr);
    uint16_t hdr = 0;
    memcpy(&hdr, static_cast<uint8_t*>(head) + facts::PHYS_NODE_STATE_OFF, sizeof hdr);
    CHECK((hdr & 0x700u) == 0x700u);   // bits 8-10 came from the recording, 0x200 among them
    CHECK((hdr & 0x00F0u) == kCiWarp); // the warp nibble is still the CI's, not the recording's
}

// DISC-RING-4. bits11 carries 0x200 on the tick possession CHANGED and on no other, so a build
// that only copies it hands the remote-holder gate a held bit that lives exactly one node -- and
// the engine reads the newest node every frame the disc is held. The recorded holder is therefore
// a LEVEL: published once, re-applied to every later node, and cleared the same way.
TEST_CASE("B1d: the recorded held level outlives the node bits11 set it on") {
    namespace w = w1e_localbody;
    struct FlagGuard {
        ~FlagGuard() { replay::entity::SetDiscNodeHeldEnabled(false); }
    } guard;

    auto held = std::make_unique<w::Fixture>();
    w::Fixture& f = *held;
    f.WireRecycler();
    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());

    const replay::PhysHandle h(0);
    replay::BodyState sample{};
    sample.bits11 = 0;                 // the recording does NOT set 0x200 on these ticks
    uint64_t whenUs = 4243u;
    replay::InstallReading ir;
    REQUIRE(entity->SetHistoryWindowDetailed(h, &sample, &whenUs, 1, ir));

    auto headerNow = [&]() -> uint16_t {
        void* head = w::GetPtr(f.container, facts::PHYS_LIST_HEAD_PTR_OFF);
        REQUIRE(head != nullptr);
        uint16_t hdr = 0;
        memcpy(&hdr, static_cast<uint8_t*>(head) + facts::PHYS_NODE_STATE_OFF, sizeof hdr);
        return hdr;
    };

    // Off is every build before this one: the call refuses and nothing moves.
    replay::entity::SetDiscNodeHeldEnabled(false);
    CHECK(entity->SetRecordedNodeHeld(h, true) == false);
    CHECK((headerNow() & 0x200u) == 0u);

    // On, the level lands on the node that is already the newest.
    replay::entity::SetDiscNodeHeldEnabled(true);
    CHECK(entity->SetRecordedNodeHeld(h, true));
    CHECK((headerNow() & 0x200u) == 0x200u);

    // And on every node pushed after it, although bits11 still says nothing.
    whenUs = 4244u;
    REQUIRE(entity->SetHistoryWindowDetailed(h, &sample, &whenUs, 1, ir));
    CHECK((headerNow() & 0x200u) == 0x200u);

    // A release is the same level in reverse, on the current node and on the next one. A recording
    // whose bits11 DOES carry the bit must not keep the disc held after the throw.
    CHECK(entity->SetRecordedNodeHeld(h, false));
    CHECK((headerNow() & 0x200u) == 0u);
    sample.bits11 = 0x200u;
    whenUs = 4245u;
    REQUIRE(entity->SetHistoryWindowDetailed(h, &sample, &whenUs, 1, ir));
    CHECK((headerNow() & 0x200u) == 0u);

    // A different body is not the disc: its nodes keep whatever the recording gave them.
    CHECK(entity->SetRecordedNodeHeld(h, true));
    const replay::PhysHandle other(1);
    replay::BodyState plain{};
    plain.bits11 = 0;
    whenUs = 4246u;
    if (entity->SetHistoryWindowDetailed(other, &plain, &whenUs, 1, ir)) {
        void* head = w::GetPtr(f.container, facts::PHYS_LIST_HEAD_PTR_OFF);
        uint16_t hdr = 0;
        if (head) memcpy(&hdr, static_cast<uint8_t*>(head) + facts::PHYS_NODE_STATE_OFF, sizeof hdr);
        CHECK((hdr & 0x200u) == 0u);
    }
}

// THE POOL ARM -- the arm the local pawn's own body takes, and the one blocker 1 lived on. The
// forward map sends handle 0 to element 3 while the pool reading stands at element 0, so the two
// resolutions address DIFFERENT objects and an install that re-derives its element from the handle
// writes into the one the identity gate never saw. Element 3 carries a ring of its own so the
// wrong write SUCCEEDS rather than faulting: a divergence that could only crash would be caught by
// the crash and not by the invariant.
TEST_CASE("B1: on the POOL arm the ring nodes land on the element the gate vouched for") {
    namespace w = w1e_localbody;
    using Refusal = replay::InstallRefusal;

    auto held = std::make_unique<w::Fixture>();
    w::Fixture& f = *held;
    f.phys.handleMap[0] = 3u;   // at or past the live count of 2, and a real element of this pool
    f.WireRecycler();

    alignas(16) uint8_t foreignContainer[0x40] = {};
    alignas(16) uint8_t foreignNode[0x80] = {};
    const float kForeignCom[3] = {77.0f, 78.0f, 79.0f};
    memcpy(foreignNode + facts::PHYS_NODE_COM_OFF, kForeignCom, sizeof kForeignCom);
    w::LinkAsHead(foreignContainer, foreignNode);
    w::SetPtr(f.Ci(3), facts::PHYS_CI_HIST_LIST_OFF, foreignContainer);

    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());

    replay::BodyState sample{};
    sample.com[0] = -1.25f;  sample.com[1] = 2.5f;  sample.com[2] = -3.75f;
    const uint64_t whenUs = 8080u;
    replay::InstallReading ir;
    CHECK(entity->SetHistoryWindowDetailed(replay::PhysHandle(0), &sample, &whenUs, 1, ir));
    CHECK(ir.why == Refusal::None);
    // The arm this ran on, so a fixture that quietly stopped diverging cannot pass by taking the
    // handle arm instead -- the whole case is about the arm on which the two elements differ.
    REQUIRE(ir.from == replay::CiPath::Pool);

    // THE INVARIANT, both halves. The vouched element's ring holds the sample...
    void* head = w::GetPtr(f.container, facts::PHYS_LIST_HEAD_PTR_OFF);
    REQUIRE(head != nullptr);
    CHECK(memcmp(static_cast<uint8_t*>(head) + facts::PHYS_NODE_COM_OFF, sample.com,
                 sizeof sample.com) == 0);
    uint64_t gatedCount = 0;
    memcpy(&gatedCount, f.Ci(0) + facts::PHYS_CI_HIST_COUNT_OFF, sizeof gatedCount);
    CHECK(gatedCount == 1u);

    // ...and the element the forward map addresses -- which nothing vouched for -- is untouched:
    // its head is still its own node, that node still carries its own pose, and its count is still
    // zero. Any one of these failing is a pose written into another entity's history.
    CHECK(w::GetPtr(foreignContainer, facts::PHYS_LIST_HEAD_PTR_OFF) ==
          static_cast<void*>(foreignNode));
    CHECK(memcmp(foreignNode + facts::PHYS_NODE_COM_OFF, kForeignCom, sizeof kForeignCom) == 0);
    uint64_t foreignCount = 0;
    memcpy(&foreignCount, f.Ci(3) + facts::PHYS_CI_HIST_COUNT_OFF, sizeof foreignCount);
    CHECK(foreignCount == 0u);
    // The retire half of the same rule: ClearHistory must not have emptied a ring it was not
    // vouched for either.
    CHECK(w::g_lastRecycleCi == f.Ci(0));

    // The counted read resolves the same element the write did.
    replay::BodyState back[1]{};
    uint64_t backT[1] = {0};
    REQUIRE(entity->ReadHistoryWindow(replay::PhysHandle(0), back, backT, 1) == 1u);
    CHECK(memcmp(back[0].com, sample.com, sizeof sample.com) == 0);
    CHECK(memcmp(back[0].com, kForeignCom, sizeof kForeignCom) != 0);
}

// ENG-RESIDUALS-1 (c) -- the ENGINE half of INSTALL_BLIND. The applier publishes
// `bodies_install_identity_refused` and its own suite can only see its NAMING of that refusal;
// nothing drove the real Entity::SetHistoryWindowDetailed over a planted pool, so the identity
// gate the install now asks (entity.cpp:2192) had no red-on-mutation proof on this side. The plant
// is the same broken link the reader's gate case uses, so one fixture defect is refused by both
// surfaces -- which is the claim: the writer declines the element the reader declines.
TEST_CASE("B1: a pose install is refused BY NAME on an element whose identity does not match") {
    namespace w = w1e_localbody;
    using Refusal = replay::InstallRefusal;

    // Zero-filled on purpose: the gate refuses before a single field of it is read.
    const replay::BodyState sample{};
    const uint64_t whenUs = 1234u;

    // The control. The SAME call on the SAME handle of an unbroken fixture does not refuse for
    // identity -- it gets past the gate and stops at this fixture's missing recycler, by another
    // name. Without it, an install that refused everything would pass the case below.
    {
        auto held = std::make_unique<w::Fixture>();
        replay::IEntity* entity = held->Build();
        REQUIRE(entity->Available());
        replay::InstallReading ir;
        CHECK_FALSE(entity->SetHistoryWindowDetailed(replay::PhysHandle(1), &sample, &whenUs, 1,
                                                     ir));
        CHECK(ir.why != Refusal::CiIdentityMismatch);
        CHECK(ir.why == Refusal::NoRecycler);
    }

    auto held = std::make_unique<w::Fixture>();
    // The CI's own entity id still says 1, so only the round trip through the reverse map fails:
    // an installer that skipped the gate would drive an element belonging to another entity and
    // return true about it.
    held->phys.reverseMap[1] = 4u;
    replay::IEntity* entity = held->Build();
    REQUIRE(entity->Available());

    replay::InstallReading ir;
    CHECK_FALSE(entity->SetHistoryWindowDetailed(replay::PhysHandle(1), &sample, &whenUs, 1, ir));
    CHECK(ir.why == Refusal::CiIdentityMismatch);
    CHECK(std::string(replay::InstallRefusalName(ir.why)) == "CiIdentityMismatch");
    // The ARM the refusal was taken on, published by W2-B R3: Handle and Pool address different
    // elements, so a refusal that does not name one cannot be attributed to an object.
    CHECK(ir.from != replay::CiPath::NotAsked);

    // The reader declines the same element for the same disagreement: two surfaces, one gate.
    replay::BodyState read{};
    replay::BodyReading r;
    CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), read, r));
    CHECK(r.refusedAt == replay::BodyRefusal::RevMapMismatch);

    // And the refusal is the identity's, not a window the call could not have taken anyway.
    replay::InstallReading bad;
    CHECK_FALSE(entity->SetHistoryWindowDetailed(replay::PhysHandle(1), nullptr, &whenUs, 1, bad));
    CHECK(bad.why == Refusal::BadWindow);
}

TEST_CASE("B1: the local com is pos + comOffset -- the quantity the history nodes carry") {
    namespace w = w1e_localbody;
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    w::Fixture f;
    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());

    replay::BodyState loc{};
    replay::BodyReading r;
    REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), loc, r));

    float pos[3], comOff[3];
    memcpy(pos,    f.localBody + lpf::kPhBodyPosOff,       sizeof pos);
    memcpy(comOff, f.localBody + lpf::kPhBodyComOffsetOff, sizeof comOff);
    for (int i = 0; i < 3; ++i) {
        // The fixture has to be able to fail: neither term is zero and they are never equal, so
        // a reader that drops either one lands somewhere else in every component.
        REQUIRE(pos[i] != 0.0f);
        REQUIRE(comOff[i] != 0.0f);
        REQUIRE(pos[i] != comOff[i]);
        CHECK(loc.com[i] == pos[i] + comOff[i]);
        CHECK(loc.com[i] != pos[i]);
        CHECK(loc.com[i] != comOff[i]);
    }
}

TEST_CASE("B1: a solo pool -- every body locally simulated -- is sampled, and names its source") {
    namespace w = w1e_localbody;
    Env& e = TheEnv();
    e.Tick(nullptr);

    w::Fixture f;
    f.MakeSolo();
    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());
    REQUIRE(entity->PhysCount() == 1u);

    // The sweep GatherBodies makes. Before the local arm this counted ZERO on every solo capture.
    size_t sampled = 0, localSourced = 0;
    for (size_t i = 0; i < entity->PhysCount(); ++i) {
        replay::BodyState s{};
        replay::BodyReading r;
        if (entity->ReadBodyDetailed(replay::PhysHandle(static_cast<uint16_t>(i)), s, r)) {
            ++sampled;
            if (r.source == replay::BodySource::Local) ++localSourced;
        }
    }
    CHECK(sampled == 1u);
    CHECK(localSourced == 1u);
}

// Every gate the reproduction can refuse at, driven by planting exactly the defect that gate
// exists to catch. Before `refusedAt`, all of these were one `false` and L2 #9 could not tell
// which of them fired on the owner's machine.
static replay::BodyRefusal RefusalOf(w1e_localbody::Fixture& f) {
    replay::IEntity* entity = f.Build();
    REQUIRE(entity->Available());
    replay::BodyState s{};
    replay::BodyReading r;
    CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
    CHECK(r.source == replay::BodySource::None);
    return r.refusedAt;
}

TEST_CASE("B1: every gate of the reproduced chain names itself when it refuses") {
    namespace w = w1e_localbody;
    namespace lpf = replay::localpawn_facts;
    using R = replay::BodyRefusal;
    Env& e = TheEnv();
    e.Tick(nullptr);

    // A control: undefeated, the chain answers and refuses at nothing. Without it every row
    // below could be passing because the fixture never resolved a body in the first place.
    {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(std::string(replay::BodyRefusalName(r.refusedAt)) == "none");
        CHECK(r.bodyResolved);
    }

    // ---- the identity gates -------------------------------------------------------------------
    // These run BEFORE the CPhysicsCS is even fetched, because everything after them is a number
    // read off this CI. Each fires on a fixture that is internally consistent apart from the single
    // link it breaks, so a reader that skipped the gate would answer confidently about the wrong
    // entity rather than fault.
    SUBCASE("the CI's own entity id is not the handle that addressed it") {
        auto held = std::make_unique<w::Fixture>();
        w::Put<uint16_t>(held->Ci(1) + lpf::kNetCiEntityIdOff, 7u);
        CHECK(RefusalOf(*held) == R::CiEntityMismatch);
    }
    SUBCASE("the forward map sends the handle at an element belonging to another handle") {
        auto held = std::make_unique<w::Fixture>();
        // Handle 1 now resolves to pool index 0 -- an element that exists, is mapped, and carries
        // entity id 0. The forward map alone cannot see this; the CI's own id can.
        held->phys.handleMap[1] = 0u;
        CHECK(RefusalOf(*held) == R::CiEntityMismatch);
    }
    SUBCASE("the reverse map disagrees with the forward map about this index") {
        auto held = std::make_unique<w::Fixture>();
        // The CI itself still says 1, so the first gate passes and only the round trip fails.
        held->phys.reverseMap[1] = 4u;
        CHECK(RefusalOf(*held) == R::RevMapMismatch);
    }
    SUBCASE("no index-to-handle map: the identity is unknown, not assumed") {
        auto held = std::make_unique<w::Fixture>();
        memset(held->phys.bytes + facts::PHYS_CS_REVERSE_MAP_OFF, 0, sizeof(void*));
        CHECK(RefusalOf(*held) == R::NoReverseMap);
    }
    SUBCASE("an identity refusal carries the three numbers that disagreed") {
        // Heap, not stack: doctest gives one TEST_CASE frame every SUBCASE's locals, and a
        // Fixture carries the whole fake zone/body world.
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.phys.reverseMap[1] = 4u;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        REQUIRE(r.refusedAt == R::RevMapMismatch);
        CHECK(r.diag.ciEntityId == 1u);
        CHECK(r.diag.ciIndex == 1u);
        CHECK(r.diag.revHandle == 4u);
        CHECK((r.diag.filled & replay::kBodyDiagRevHandle) != 0u);
        // Nothing past the gate was reached, so nothing past it may print as a number.
        CHECK((r.diag.filled & replay::kBodyDiagBodyId) == 0u);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("ent=1") != std::string::npos);
        CHECK(line.find("idx=1") != std::string::npos);
        CHECK(line.find("rev=4") != std::string::npos);
        CHECK(line.find("bodyId=?") != std::string::npos);
    }
    // The control's own identity numbers, on the arm that ANSWERS: the gates are not merely silent
    // on a healthy read, they record what made them silent.
    SUBCASE("a body that resolves records the identity it was checked against") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.diag.ciEntityId == 1u);
        CHECK(r.diag.ciIndex == 1u);
        CHECK(r.diag.revHandle == 1u);
    }
    // ---- handle 0: the two equality gates CANNOT FAIL there ----------------------------------
    // The local pawn's handle is 0 (L2 #12 read ent=0 idx=1 rev=0) and 0 is also what an unwritten
    // CI and an unwritten reverse map carry, so `ent == handle` and `rev == handle` are 0 == 0 --
    // both satisfied by exactly the object they exist to catch. The proof there is uniqueness over
    // the live range, which a zero fill cannot survive once a second instance is live.
    SUBCASE("handle 0's identity is proved by uniqueness over the live range, not by 0 == 0") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(0), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.diag.identity == replay::IdentityProof::UniqueLiveIndex);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("ident=uniqueLiveIndex") != std::string::npos);
    }
    SUBCASE("a non-zero handle is proved by the equality gates, and the line says which proof") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.diag.identity == replay::IdentityProof::HandleCompare);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("ident=handleCompare") != std::string::npos);
    }
    // The reading no comparison against zero can produce. Before this gate the read SUCCEEDED,
    // with source=History and another entity's pose in `out`.
    SUBCASE("a second live index carrying handle 0 makes the reverse map prove nothing") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.phys.reverseMap[1] = 0u;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(0), s, r));
        CHECK(r.refusedAt == R::RevMapAmbiguous);
        CHECK(r.source == replay::BodySource::None);
        CHECK(r.diag.identity == replay::IdentityProof::Refused);
    }
    // The engine's own map rule, which Pool::Element never applied: HasComponent is
    // `cmp word [map + 4*id], count / setb`, so an index AT the count addresses no instance.
    // Both sides of the boundary in one fixture, so a reader spelling `>` fails one of them.
    SUBCASE("the forward map's index must be inside the net CS live count") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading last, past;
        w::Put<uint16_t>(f.phys.bytes + facts::PHYS_CS_COUNT_OFF, uint16_t(2));
        CHECK(entity->ReadBodyDetailed(replay::PhysHandle(1), s, last));
        CHECK(last.refusedAt == R::None);
        w::Put<uint16_t>(f.phys.bytes + facts::PHYS_CS_COUNT_OFF, uint16_t(1));
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, past));
        CHECK(past.refusedAt == R::CiIndexNotLive);
        CHECK(past.diag.ciIndex == 1u);
    }
    // ---- WHICH READING produced the CI -------------------------------------------------------
    // Two callers produce the u16 and do not mean the same thing by it. The tick walks
    // i in [0, PHYS.CS_COUNT_OFF) and hands the walk's OWN index, already inside that count; a
    // caller holding a component handle hands an id the forward map has to turn into an index.
    // L2 #13 refused 7062 of 7062 bodies at the map's bound -- handle 0, map entry 1, live count 1
    // -- for a CI the walk had already bounded. The map is still asked first and is still
    // authoritative wherever HasComponent accepts its entry; only where the engine's own predicate
    // says "no component of this system" is the value read as the index the walk already had.
    SUBCASE("a pool-walked CI whose id has no net component reaches the arms, and says which") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WireTransform();
        f.WirePhysicsCsList(1u);
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        // The local pawn's shape exactly: its CI is live at pool index 1, and its id has no
        // forward-map entry the live count accepts because it was never given a net handle.
        f.phys.handleMap[1]  = 9u;   // past the live count of 2 -- HasComponent refuses it
        f.phys.reverseMap[0] = 4u;   // so handle 0 at index 1 is unique over the live range
        f.phys.reverseMap[1] = 0u;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.diag.ciFrom == replay::CiPath::Pool);
        CHECK(r.diag.ciIndex == 1u);
        CHECK(r.diag.netLive == 2u);
        CHECK(r.diag.identity == replay::IdentityProof::UniqueLiveIndex);
        // It reached BOTH arms: the ncaPhysics CS-list search ran, and a body answered.
        CHECK(r.source == replay::BodySource::Local);
        CHECK((r.diag.filled & replay::kBodyDiagCsIdx) != 0u);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("ciFrom=pool") != std::string::npos);
        CHECK(line.find("netLive=2") != std::string::npos);
        CHECK(line.find("csIdx=?") == std::string::npos);
    }
    // ---- F36: the two entity ids over one pool index -----------------------------------------
    // L2 #14 refused 6104 of 6104 bodies because the pool reading compared the reverse map's u16
    // against the CI's own and had nothing to say which of them this instance belonged to. BOTH
    // are entity ids -- the reverse map's value is the stride-4 subscript of the forward map at
    // 0x140D87E33 -- so the numbers here are DISTINCT and neither is the pool index: a fixture
    // that reused one number for two roles could not fail a reader that confused them.
    static constexpr uint16_t kRevEntity = 40u, kCiEntity = 3u, kPoolIdx = 1u;
    auto poolF36 = [](w::Fixture& f, uint16_t revMapsTo, uint16_t ciIdMapsTo) {
        f.WireTransform();
        f.WirePhysicsCsList(1u);
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        f.phys.handleMap[1]  = 9u;   // past the live count of 2: the pool reading answers
        f.phys.reverseMap[0] = 4u;
        f.phys.reverseMap[kPoolIdx] = kRevEntity;
        w::Put<uint16_t>(f.Ci(kPoolIdx) + lpf::kNetCiEntityIdOff, kCiEntity);
        f.phys.handleMap[kRevEntity] = revMapsTo;
        f.phys.handleMap[kCiEntity]  = ciIdMapsTo;
    };
    SUBCASE("the reverse map's entity id round-trips and the CI's own does not: the map proves it") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        poolF36(f, kPoolIdx, 7u);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(kPoolIdx), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.diag.ciFrom == replay::CiPath::Pool);
        CHECK(r.diag.identity == replay::IdentityProof::MapRoundTrip);
        CHECK(r.diag.revHandle == kRevEntity);
        CHECK(r.diag.ciEntityId == kCiEntity);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("ident=mapRoundTrip") != std::string::npos);
        CHECK(line.find("ent=3") != std::string::npos);
        CHECK(line.find("rev=40") != std::string::npos);
    }
    SUBCASE("only the CI's own entity id round-trips: the reverse map's entry names another") {
        auto held = std::make_unique<w::Fixture>();
        poolF36(*held, 7u, kPoolIdx);
        replay::IEntity* entity = held->Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(kPoolIdx), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.diag.identity == replay::IdentityProof::CiIdRoundTrip);
    }
    SUBCASE("neither entity id comes back to this index, and the refusal prints both") {
        auto held = std::make_unique<w::Fixture>();
        poolF36(*held, 7u, 7u);
        replay::IEntity* entity = held->Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        // The identity gates are the PHYSICS arm's, and this CI was reached by the pool walk:
        // the entity is one this machine simulates, so its pose is still answerable and the
        // refusal travels as the physics arm's reason rather than as the whole answer.
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(kPoolIdx), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.diag.cphBodyArm == R::CiEntityMismatch);
        CHECK(r.diag.origin == replay::BodyOrigin::LocalTransform);
        CHECK(r.diag.xformArm == R::None);
        CHECK(s.com[0] == 1.5f);
        CHECK(s.com[1] == -2.5f);
        CHECK(s.com[2] == 3.5f);
        CHECK(r.diag.angVel == replay::AngVelSource::Unavailable);
        // The identity is still REFUSED and both ids are still on the line: the pose answers the
        // capture, and the diag is what says the physics chain never identified this instance.
        CHECK(r.diag.identity == replay::IdentityProof::Refused);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("cphBody=ciEntityMismatch") != std::string::npos);
        CHECK(line.find("bodySource=localTransform") != std::string::npos);
        CHECK(line.find("ent=3") != std::string::npos);
        CHECK(line.find("rev=40") != std::string::npos);
    }
    SUBCASE("two entity ids both map to this index, so the forward map identifies no CI") {
        auto held = std::make_unique<w::Fixture>();
        poolF36(*held, kPoolIdx, kPoolIdx);
        replay::IEntity* entity = held->Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(kPoolIdx), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.diag.cphBodyArm == R::ForwardMapAmbiguous);
        CHECK(std::string(replay::BodyRefusalName(r.diag.cphBodyArm)) == "forwardMapAmbiguous");
        CHECK(r.diag.origin == replay::BodyOrigin::LocalTransform);
        CHECK(s.com[0] == 1.5f);
        CHECK(r.diag.identity == replay::IdentityProof::Refused);
        char text[replay::kBodyDiagTextMax];
        CHECK(std::string(replay::DescribeDiag(r.diag, text, sizeof text))
                  .find("cphBody=forwardMapAmbiguous") != std::string::npos);
    }

    // The two remaining arms of the physics chain, on the same pool-walked CI. Together with the
    // two above and the badSlot case further down, every reason the chain can refuse the local
    // pawn for is planted, and each one ANSWERS a pose while naming itself on the diag.
    SUBCASE("no ncaPhysics CS owns the entity: the pool-walked CI still answers a pose") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WireTransform();
        f.WirePoolWalked();
        // The default list holds two component systems and NEITHER claims this entity: the decoy
        // declares a live count of zero and the real one loses the handle map HasComponent is
        // read out of, so the search runs out. That is the engine's own "this entity has no
        // ncaPhysics instance", which is what L2 #12 measured for the local pawn. A one-entry
        // list would not do: selection defers to PrimaryBody there and the search never runs.
        memset(f.physicsCs.bytes + facts::PHYS_CS_HANDLE_MAP_OFF, 0, sizeof(void*));
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(kPoolIdx), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.diag.ciFrom == replay::CiPath::Pool);
        CHECK(r.diag.cphBodyArm == R::NoPhysicsCsForEntity);
        CHECK(r.diag.origin == replay::BodyOrigin::LocalTransform);
        CHECK(s.com[0] == 1.5f);
        CHECK(s.com[2] == 3.5f);
        char text[replay::kBodyDiagTextMax];
        CHECK(std::string(replay::DescribeDiag(r.diag, text, sizeof text))
                  .find("cphBody=noPhysicsCsForEntity") != std::string::npos);
    }
    // The other side of the same rule, and the one that keeps it from being "answer anyway". A
    // handle-resolved CI names an entity the NET layer owns: its transform is filled by replicated
    // state this reader has no claim on, and there is no second local source for it. Its refusals
    // stay refusals -- with the arm named, exactly as before.
    SUBCASE("a handle-resolved CI does not fall through: its refusal stays a refusal") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WireTransform();
        f.WirePhysicsCsList(1u);
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.diag.ciFrom == replay::CiPath::Handle);
        CHECK(r.refusedAt == R::BadSlot);
        CHECK(r.diag.cphBodyArm == R::BadSlot);     // the arm is still named
        CHECK(r.diag.origin == replay::BodyOrigin::NotAsked);
        CHECK(r.diag.xformArm == R::None);
        CHECK((r.diag.filled & replay::kBodyDiagXformArm) == 0u);   // never even asked
        CHECK(s.com[0] != 1.5f);
    }
    SUBCASE("a handle-resolved CI past the count still refuses, and prints what it compared") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        // Neither reading is inside the count: the map's entry addresses no instance and the id
        // is not an index of one either. That is the case CiIndexNotLive is for, and it survives.
        f.phys.handleMap[1] = 6u;
        w::Put<uint16_t>(f.phys.bytes + facts::PHYS_CS_COUNT_OFF, uint16_t(1));
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.refusedAt == R::CiIndexNotLive);
        CHECK(r.diag.ciIndex == 6u);
        CHECK(r.diag.netLive == 1u);
        // No reading stood, so the path is unreached and must print as unreached.
        CHECK((r.diag.filled & replay::kBodyDiagCiFrom) == 0u);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("idx=6") != std::string::npos);
        CHECK(line.find("netLive=1") != std::string::npos);
        CHECK(line.find("ciFrom=?") != std::string::npos);
    }
    // The map entry is FOUR bytes and Element() reads the low two. Nothing had ever read the other
    // two, so a 1-based or half-swapped encoding was invisible in every artefact this reader made.
    SUBCASE("the forward map entry's high half is published beside the index it uses") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.phys.handleMap[1] = 1u | (0xBEEFu << 16);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.diag.ciIndex == 1u);
        CHECK(r.diag.mapHigh == 0xBEEFu);
        CHECK(r.diag.ciFrom == replay::CiPath::Handle);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("mapHigh=48879") != std::string::npos);
        CHECK(line.find("ciFrom=handle") != std::string::npos);
    }
    // The state that used to fire this arm -- a null PHYS.NETCS_PHYSICS_CS_OFF -- is no longer
    // on the path at all (the subcase below asserts that). What "there is no CPhysicsCS" now
    // means is an ncaPhysics list entry that exists and holds nothing, which is a different
    // answer from a list that was walked and said no, and must not print the same word.
    SUBCASE("the space's ncaPhysics list holds no component system") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        f.WirePhysicsCsList(0u);
        // PHYS.NETCS_PHYSICS_CS_OFF is left pointing at the decoy, so a reader still answering
        // off the cached pointer would resolve a body here instead of refusing.
        CHECK(RefusalOf(f) == R::NoPhysicsCs);
    }
    // Broken BELOW HasComponent, which is the only place this arm is still reachable from: the
    // map and the count are intact, so the search picks this component system, and the pool base
    // the walk needs is gone.
    SUBCASE("no pool base on the CPhysicsCS the search chose") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        memset(f.physicsCs.bytes + facts::PHYS_CS_POOL_BASE_OFF, 0, sizeof(void*));
        CHECK(RefusalOf(f) == R::NoPhysicsPool);
    }
    // The same breakage ABOVE HasComponent: a CS with no handle map cannot answer for any entity,
    // so the search skips it rather than walking into it, and the list runs out.
    SUBCASE("no handle map: the CS cannot answer HasComponent and is skipped") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        memset(f.physicsCs.bytes + facts::PHYS_CS_HANDLE_MAP_OFF, 0, sizeof(void*));
        CHECK(RefusalOf(f) == R::NoPhysicsCsForEntity);
    }
    // The list count is the engine's own number with nothing behind it. A list longer than the
    // registry every one of its members is registered in is a count read off an object that is
    // not the container, so the reader refuses instead of walking a third entry of a two-entry
    // list. W1E_MUTATE_CSLIST_NO_COUNT_BOUND drops this and answers off arr[1] instead.
    SUBCASE("a list count larger than the space's own registry count is refused") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        f.WirePhysicsCsList(3u);
        CHECK(RefusalOf(f) == R::PhysicsCsListUnreadable);
    }
    // F29's boundary, BOTH sides, in one fixture. The engine's compare is unsigned and STRICT --
    // 0x14044CC6C `66 3B 8A F4 00 00 00` / `73 2C` (cmp cx,[rdx+0F4h]; jnb -> the -1 sentinel) and
    // HasComponent's `66 42 39 0C 82` / `0F 92 C0` (setb) -- so slot == liveCount is OUT and
    // slot == liveCount - 1 is IN. L2 #12 refused at exactly slot == liveCount == 5. The map holds
    // a RAW 0-based index (the same u16 subscripts the pool unscaled at 0x14044CC75, and the count
    // is decremented one per removal in sub_1402F4C70), so there is no +1 encoding to undo.
    SUBCASE("the live-count bound is strict: the last live slot answers, the next one refuses") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading last, past;
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF,
                         uint16_t(w::Fixture::kPhysicsSlot + 1));
        CHECK(entity->ReadBodyDetailed(replay::PhysHandle(1), s, last));
        CHECK(last.refusedAt == R::None);
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, past));
        // With the search in front of PrimaryBody the strict compare that fires is
        // HasComponent's -- the SAME rule on the SAME map entry and the SAME count -- so the CS
        // is skipped rather than walked into, and the walk runs out. That is also why BadSlot is
        // no longer reachable on this arm: every CS the search returns has already passed a bound
        // at least as strict as PrimaryBody's `bypass || slot < count`.
        CHECK(past.refusedAt == R::NoPhysicsCsForEntity);
        CHECK(past.refusedAt != R::BadSlot);
        CHECK(past.diag.csCount == 2u);
        // Read past the gate that fired, so they must not print as numbers.
        CHECK((past.diag.filled & replay::kBodyDiagSlot) == 0u);
        CHECK((past.diag.filled & replay::kBodyDiagCsIdx) == 0u);
    }
    // The map entry is FOUR bytes and PrimaryBody reads the LOW half of it. The high half here is
    // a live slot of its own, so a wrong-half read SUCCEEDS with the decoy transform rather than
    // refusing, and only the com compare can see it.
    SUBCASE("the slot is the low half of the four-byte map entry") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.refusedAt == R::None);
        float want[3];
        w::Fixture::ExpectedCom(want);
        CHECK(s.com[0] == want[0]);
        CHECK(s.com[1] == want[1]);
        CHECK(s.com[2] == want[2]);
    }
    // `test byte ptr [rax+0C40h], 1 / jnz` at 0x14044CC63 skips the bound entirely. This is the
    // arm L2-RUN-10 hit 1476 times out of 1476: the slot is past the count and the engine takes
    // it anyway, so a reader that only spells the cmp refuses every body in the pool.
    // On a list of ONE the search defers to PrimaryBody, so this arm -- and the bypass bit it
    // turns on -- is reached exactly as it was before the search went in front of it. On a longer
    // list the same fixture refuses at the search instead, which the boundary subcase above pins.
    SUBCASE("the live-count bound is bypassed when the CPhysicsCS state bit is set") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        f.WirePhysicsCsList(1u);
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        *f.physicsCs.Flags() = lpf::kPhysicsCsSlotBoundBypassBit;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.refusedAt == R::None);
        CHECK(r.bodyResolved);
        float want[3];
        w::Fixture::ExpectedCom(want);
        CHECK(s.com[0] == want[0]);
    }
    SUBCASE("every other bit of that flag word leaves the bound standing") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        f.WirePhysicsCsList(1u);
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        *f.physicsCs.Flags() = static_cast<uint8_t>(~lpf::kPhysicsCsSlotBoundBypassBit);
        CHECK(RefusalOf(f) == R::BadSlot);
    }
    SUBCASE("no state object: the bound stands rather than the walk guessing") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        f.WirePhysicsCsList(1u);   // the bound below, not the search, is this subcase's subject
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        memset(f.physicsCs.bytes + lpf::kPhysicsCsStateObjOff, 0, sizeof(void*));
        CHECK(RefusalOf(f) == R::BadSlot);
    }
    // F19 of L2-RUN-10: ReadLocalBody asked the OS five times per body per tick and cost-killed a
    // live capture. SpanProbeCount is a running total of the replay::Readable calls the span
    // cache made, so the FIRST read has to pay for its spans -- a walk that proves nothing is as
    // wrong as one that proves everything twice -- and every read after it must pay nothing.
    SUBCASE("the tick path proves its spans once and then asks the OS nothing") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        const uint64_t before = replay::entity::SpanProbeCount();
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        // handle map, CPhysicsCS state, the 0x78 pool, the zone table and the body array.
        const uint64_t first = replay::entity::SpanProbeCount() - before;
        CHECK(first >= 5u);
        const uint64_t warm = replay::entity::SpanProbeCount();
        // Well inside SpanProofMaxCachedReads, which is what bounds how long one proof stands.
        const int kTicks = 200;
        REQUIRE(uint32_t(kTicks) < replay::entity::SpanProofMaxCachedReads());
        for (int tick = 0; tick < kTicks; ++tick) {
            replay::BodyState s2{};
            replay::BodyReading r2;
            REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s2, r2));
            REQUIRE(r2.refusedAt == R::None);
        }
        CHECK(replay::entity::SpanProbeCount() - warm == 0u);
        // The same run's ratio: 201 reads, one span proof each at most, and in fact none after
        // the first read. A per-tick budget is a difference across the tick, so this is the
        // form that can tell "cached" from "never asked".
        CHECK(replay::entity::SpanProbeCount() - before == first);
    }
    // L2 #11 read `badSlot` 7068 times and had to reason from the ARM alone to "the bypass bit
    // is clear and the slot is genuinely out of range". These carry the numbers that arm was
    // computed from, so #12 measures instead.
    SUBCASE("a badSlot refusal carries the numbers its gate compared") {
        // Heap, not stack: doctest gives one TEST_CASE frame every SUBCASE's locals, and a
        // Fixture carries the whole fake zone/body world.
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WirePhysicsCsList(1u);   // the bound below, not the search, is this subcase's subject
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        REQUIRE(r.refusedAt == R::BadSlot);
        // This CI was HANDLE-resolved: it names an entity the net layer owns, and the local
        // transform chain is not a second source for it, so the fallback is not offered and its
        // arm word is UNREACHED rather than refused. The physics arm's own word is still filled.
        const uint32_t all = replay::kBodyDiagCsIdx | replay::kBodyDiagCsCount |
                             replay::kBodyDiagCi | replay::kBodyDiagPhysCs |
                             replay::kBodyDiagBodyId | replay::kBodyDiagSlot |
                             replay::kBodyDiagLiveCount | replay::kBodyDiagBypass |
                             replay::kBodyDiagCiEntity | replay::kBodyDiagCiIndex |
                             replay::kBodyDiagRevHandle | replay::kBodyDiagIdentity |
                             replay::kBodyDiagCphArm |
                             replay::kBodyDiagCiFrom | replay::kBodyDiagNetLive |
                             replay::kBodyDiagMapHigh;
        CHECK(r.diag.filled == all);
        CHECK(r.diag.cphBodyArm == R::BadSlot);
        CHECK(r.diag.xformArm == R::None);
        CHECK((r.diag.filled & replay::kBodyDiagXformArm) == 0u);
        CHECK(r.diag.origin == replay::BodyOrigin::NotAsked);
        // The identity was PROVED before this gate refused, so the refusal is a statement about
        // the slot and not about the object it was read off.
        CHECK(r.diag.identity == replay::IdentityProof::HandleCompare);
        CHECK(r.diag.bodyId == w::Fixture::kLocalBodyId);
        CHECK(r.diag.slot == w::Fixture::kPhysicsSlot);
        CHECK(r.diag.liveCount == w::Fixture::kPhysicsSlot);
        CHECK(r.diag.bypass == replay::BypassRead::Clear);
        CHECK(r.diag.csCount == 1u);
        // The two tags are what let a stop line say whether the CI and the CS came from the same
        // object; equal tags here would make that question unanswerable.
        CHECK(r.diag.physCsTag != 0u);
        CHECK(r.diag.ciTag != 0u);
        CHECK(r.diag.physCsTag != r.diag.ciTag);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("slot=3") != std::string::npos);
        CHECK(line.find("liveCount=3") != std::string::npos);
        CHECK(line.find("bypass=clear") != std::string::npos);
        CHECK(line.find("csIdx=0") != std::string::npos);
        CHECK(line.find("csCount=1") != std::string::npos);
        // angVel is the ONE field a BadSlot refusal cannot reach -- it is read on the arm
        // this gate stops -- so its unreached mark is the correct answer here, and it is the
        // only one on the line.
        CHECK(line.find("angVel=?") != std::string::npos);
        CHECK(line.find("cphBody=badSlot") != std::string::npos);
        CHECK(line.find("xform=?") != std::string::npos);
        std::string reached = line;
        // The five a refusal on this arm cannot reach: angVel is read past this gate, and the
        // fallback was never offered this CI, so it has no arm, no instance index, no live count
        // and no origin.
        for (const char* gap : {"angVel=?", "bodySource=?", "xform=?", "xfIdx=?", "xfLive=?"}) {
            const size_t at = reached.find(gap);
            REQUIRE(at != std::string::npos);
            reached.erase(at, strlen(gap));
        }
        CHECK(reached.find('?') == std::string::npos);
    }
    // ---- the ncaTransform FALLBACK ---------------------------------------------------------
    // L2 #12 measured `badSlot bodyId=4 slot=5 liveCount=5` for the LOCAL pawn with the identity
    // proved, which is the engine's own answer that entity 4 has no ncaPhysics instance. These
    // cases are the arm that answers anyway -- a pose, said to be a pose.
    SUBCASE("the ncaTransform fallback answers a pose when the entity has no ncaPhysics instance") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WireTransform();
        f.WirePoolWalked();
        f.WirePhysicsCsList(1u);   // the bound below, not the search, is this subcase's subject
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        // It ANSWERED, so nothing refused -- src/capture counts `refusedAt` into its histogram
        // whether or not the read succeeded, and a refusal counted against a body it recorded
        // would make the stop line's refusedBy set disagree with its own sample count.
        CHECK(r.refusedAt == R::None);
        CHECK(r.source == replay::BodySource::Local);
        CHECK(r.diag.origin == replay::BodyOrigin::LocalTransform);
        // The reason the physics chain could not answer SURVIVES the successful read. It is the
        // only place that reason exists once refusedAt is None.
        CHECK(r.diag.cphBodyArm == R::BadSlot);
        CHECK(r.diag.xformArm == R::None);
        CHECK(r.diag.xformIndex == w::Fixture::kXformIndex);
        CHECK(r.diag.xformLive == w::Fixture::kXformLive);
        // The pose, not the decoy the map entry's high half addresses.
        CHECK(s.com[0] == 1.5f);
        CHECK(s.com[1] == -2.5f);
        CHECK(s.com[2] == 3.5f);
        CHECK(s.ori[2] == 1.0f);
        CHECK(s.ori[3] == 0.0f);
        // No CPhBody: no velocity, no cloud to derive omega from. Both are STRUCTURAL zeros and
        // the diag word is what stops them being read as a body at rest.
        CHECK(s.vel[0] == 0.0f);
        CHECK(s.angVel[0] == 0.0f);
        CHECK(r.diag.angVel == replay::AngVelSource::Unavailable);
        CHECK_FALSE(r.bodyResolved);
        // Every one of the eighteen diag fields is reached on this path, so the line carries no
        // `?` at all -- the totality check the refusal cases can only make field by field.
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find('?') == std::string::npos);
        CHECK(line.find("bodySource=localTransform") != std::string::npos);
        CHECK(line.find("angVel=unavailable") != std::string::npos);
        CHECK(line.find("xfIdx=3") != std::string::npos);
        CHECK(line.find("xfLive=5") != std::string::npos);
        CHECK(r.diag.filled == (UINT16_MAX | replay::kBodyDiagCsIdx | replay::kBodyDiagCsCount |
                                replay::kBodyDiagCiFrom | replay::kBodyDiagNetLive |
                                replay::kBodyDiagMapHigh));
    }
    SUBCASE("the fallback applies HasComponent's bound, which the engine's own walk omits") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WireTransform();
        f.WirePoolWalked();
        f.WirePhysicsCsList(1u);   // the bound below, not the search, is this subcase's subject
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        // The bound pinned on BOTH sides: index == live refuses, index == live-1 answers.
        w::Put<uint16_t>(f.xform.bytes + lpf::kXformCsCountOff, w::Fixture::kXformIndex);
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.diag.xformArm == R::NoTransformInstance);
        CHECK(r.refusedAt == R::BadSlot);          // still the FIRST refusal
        CHECK(r.diag.xformLive == w::Fixture::kXformIndex);
        w::Put<uint16_t>(f.xform.bytes + lpf::kXformCsCountOff,
                         static_cast<uint16_t>(w::Fixture::kXformIndex + 1u));
        replay::BodyReading r2;
        CHECK(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r2));
        CHECK(r2.diag.xformArm == R::None);
    }
    SUBCASE("the fallback composes the CNode3D parent chain and stops at a non-node parent") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WireTransform();
        f.WirePoolWalked();
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        // 7b: the instance delegates to a node. Leaf entry 0, parent entry 1, and a grandparent
        // whose class id is NOT CNode3D's -- the engine's own terminator, and the reason the walk
        // stops with a composed pose rather than refusing.
        w::PutPtr(f.xform.Inst(w::Fixture::kXformIndex) + lpf::kXformInstNodeOff, f.tree.Node(0));
        f.tree.SetParent(0, f.tree.Node(1));
        f.tree.SetParent(1, f.tree.Node(2));
        w::Put<uint64_t>(f.tree.Node(2) + lpf::kNode3dClassIdOff, lpf::kNode3dClassId ^ 1ull);
        // Every value here is exact in binary32 and every one of them DISCRIMINATES.
        // (0.5,0.5,0.5,0.5) is the unit quaternion whose rotation CYCLES the axes -- R.(a,b,c) =
        // (c,a,b) -- so it does not commute with a diagonal scale, and the scale (2,4,8) is
        // non-uniform. Under a 180-degree rotation and a uniform scale, applying the scale
        // before the rotation instead of after gives the SAME answer: such a fixture cannot fail
        // a reader that has the composer's order backwards. This one can, and does.
        w::PutVec4(f.tree.Entry(0) + lpf::kXformEntryQuatOff, 1.0f, 0.0f, 0.0f, 0.0f);
        w::PutVec3(f.tree.Entry(0) + lpf::kXformEntryPosOff, 1.0f, 2.0f, 3.0f);
        w::PutVec4(f.tree.Entry(1) + lpf::kXformEntryQuatOff, 0.5f, 0.5f, 0.5f, 0.5f);
        w::PutVec3(f.tree.Entry(1) + lpf::kXformEntryPosOff, 10.0f, 20.0f, 30.0f);
        w::PutVec3(f.tree.Entry(1) + lpf::kXformEntryScaleOff, 2.0f, 4.0f, 8.0f);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.diag.origin == replay::BodyOrigin::LocalTransform);
        // p = p_parent + s_parent * (R(q_parent) . p_leaf)
        //   = (10,20,30) + (2,4,8) * ((3,1,2)) = (16,24,46)
        // The other order -- (2,4,8)*(1,2,3) = (2,8,24), rotated to (24,2,8) -- would give
        // (34,22,38), so this triple is what separates the engine's composer from its mirror.
        CHECK(s.com[0] == 16.0f);
        CHECK(s.com[1] == 24.0f);
        CHECK(s.com[2] == 46.0f);
        // q = q_parent * q_leaf, the PARENT on the left, then renormalised as the engine does.
        CHECK(s.ori[0] == 0.5f);
        CHECK(s.ori[1] == 0.5f);
        CHECK(s.ori[2] == -0.5f);
        CHECK(s.ori[3] == -0.5f);
        // The instance's own transform is NOT the answer when a node is present: it names the
        // pose the 7a arm would have returned, and it differs in every component.
        CHECK(s.com[0] != 1.5f);
    }
    SUBCASE("a cyclic parent link refuses at the depth bound instead of spinning") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WireTransform();
        f.WirePoolWalked();
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        w::PutPtr(f.xform.Inst(w::Fixture::kXformIndex) + lpf::kXformInstNodeOff, f.tree.Node(0));
        f.tree.SetParent(0, f.tree.Node(1));
        f.tree.SetParent(1, f.tree.Node(0));   // the cycle the engine's walk has no bound against
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.diag.xformArm == R::NodeChainTooDeep);
        CHECK(r.diag.origin == replay::BodyOrigin::NotAsked);
    }
    SUBCASE("the wrong build does not fall back: NoChain refuses both arms") {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WireTransform();
        f.WirePoolWalked();
        // On a test host the PRIMARY_BODY / RESOLVE_BODY prologue guards cannot pass -- no
        // echovr.exe is mapped -- and it is the injected phSystem that stands in for them. Take
        // it away and bodyChainOk_ is false, which is exactly the wrong-build shape: the
        // transform chain is spelled out of the same build's bytes, so it must not answer.
        f.fns.phSystem = nullptr;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        REQUIRE(r.refusedAt == R::NoChain);
        CHECK(r.diag.xformArm == R::None);
        CHECK((r.diag.filled & replay::kBodyDiagXformArm) == 0u);
        CHECK(r.diag.origin == replay::BodyOrigin::NotAsked);
    }
    // The whole point of the tri-state. Both of these refuse at badSlot and both leave the bound
    // standing; only the diag can say which one the owner's machine was in, and L2 #11's
    // conclusion ("the bit is CLEAR") is only checkable because they differ here.
    SUBCASE("an unreadable state object reads as unreadable, never as clear") {
        // Heap, not stack: doctest gives one TEST_CASE frame every SUBCASE's locals, and a
        // Fixture carries the whole fake zone/body world.
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WirePhysicsCsList(1u);   // the bound below, not the search, is this subcase's subject
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        memset(f.physicsCs.bytes + lpf::kPhysicsCsStateObjOff, 0, sizeof(void*));
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.refusedAt == R::BadSlot);
        CHECK(r.diag.bypass == replay::BypassRead::Unreadable);
        CHECK((r.diag.filled & replay::kBodyDiagBypass) != 0u);
        char text[replay::kBodyDiagTextMax];
        CHECK(std::string(replay::DescribeDiag(r.diag, text, sizeof text))
                  .find("bypass=unreadable") != std::string::npos);
    }
    SUBCASE("a bit that is set reads as set") {
        // Heap, not stack: doctest gives one TEST_CASE frame every SUBCASE's locals, and a
        // Fixture carries the whole fake zone/body world.
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WirePhysicsCsList(1u);   // the bound below, not the search, is this subcase's subject
        w::Put<uint16_t>(f.physicsCs.bytes + facts::PHYS_CS_COUNT_OFF, w::Fixture::kPhysicsSlot);
        *f.physicsCs.Flags() = lpf::kPhysicsCsSlotBoundBypassBit;
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        REQUIRE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.diag.bypass == replay::BypassRead::Set);
        // The success path fills the diag too: a diag that only exists on refusal cannot show
        // that the numbers it reports on a refusal are the ones the gate really uses.
        CHECK(r.diag.slot == w::Fixture::kPhysicsSlot);
        CHECK(r.diag.liveCount == w::Fixture::kPhysicsSlot);
    }
    // A gate that refuses early must not report the fields it never reached. A zero in any of
    // them would be scored as a measurement -- that is the exact reading error this packet is
    // here to stop making.
    SUBCASE("a field the walk never reached prints ?, not a zero") {
        // Heap, not stack: doctest gives one TEST_CASE frame every SUBCASE's locals, and a
        // Fixture carries the whole fake zone/body world.
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.WirePhysicsCsList(0u);   // the list entry exists and holds no component system
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        REQUIRE(r.refusedAt == R::NoPhysicsCs);
        CHECK((r.diag.filled & replay::kBodyDiagCi) != 0u);
        CHECK((r.diag.filled & replay::kBodyDiagPhysCs) != 0u);
        // The entity id is the search KEY now, not a subscript read later, so it IS reached --
        // every field past the list is not.
        CHECK((r.diag.filled & replay::kBodyDiagBodyId) != 0u);
        CHECK((r.diag.filled & replay::kBodyDiagCsCount) != 0u);
        CHECK((r.diag.filled & replay::kBodyDiagCsIdx) == 0u);
        CHECK((r.diag.filled & replay::kBodyDiagSlot) == 0u);
        CHECK((r.diag.filled & replay::kBodyDiagLiveCount) == 0u);
        CHECK((r.diag.filled & replay::kBodyDiagBypass) == 0u);
        char text[replay::kBodyDiagTextMax];
        const std::string line = replay::DescribeDiag(r.diag, text, sizeof text);
        CHECK(line.find("bodyId=?") == std::string::npos);
        CHECK(line.find("csIdx=?") != std::string::npos);
        CHECK(line.find("slot=?") != std::string::npos);
        CHECK(line.find("liveCount=?") != std::string::npos);
        CHECK(line.find("bypass=?") != std::string::npos);
        // Read and null is a different answer from never read, and the two must not share text.
        CHECK(line.find("physCs=null") != std::string::npos);
        CHECK(line.find("physCs=?") == std::string::npos);
        CHECK(line.find("ci=?") == std::string::npos);
    }
    SUBCASE("the pointer tag is an identity, and is not the pointer") {
        alignas(16) uint8_t block[64] = {};
        const uint32_t a = replay::BodyDiagTag(block);
        CHECK(a == replay::BodyDiagTag(block));            // stable across ticks
        CHECK(a != replay::BodyDiagTag(block + 1));        // discriminating
        CHECK(a != replay::BodyDiagTag(block + 16));
        CHECK(replay::BodyDiagTag(nullptr) == 0u);         // 0 is reserved for null
        CHECK(a != 0u);
        // Not a truncation: a log carrying this must not carry the owner's live address.
        CHECK(a != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block)));
        CHECK(a != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block) >> 32));
    }
    SUBCASE("DescribeDiag truncates rather than overruns, and always terminates") {
        replay::BodyDiag d;
        d.filled = replay::kBodyDiagBodyId;
        d.bodyId = UINT32_MAX;   // the widest value the field can hold, not a fact
        char tiny[8];
        memset(tiny, 'X', sizeof tiny);
        const char* got = replay::DescribeDiag(d, tiny, sizeof tiny);
        CHECK(got == tiny);
        CHECK(strnlen(tiny, sizeof tiny) < sizeof tiny);
        CHECK(std::string(tiny) == "bodyId=");
        // A zero cap has nowhere to write a terminator, so it must not pretend it wrote one.
        CHECK(std::string(replay::DescribeDiag(d, tiny, 0)).empty());
        char full[replay::kBodyDiagTextMax];
        d.filled = static_cast<uint16_t>(replay::kBodyDiagBodyId | replay::kBodyDiagSlot |
                                         replay::kBodyDiagLiveCount | replay::kBodyDiagBypass |
                                         replay::kBodyDiagPhysCs | replay::kBodyDiagCi |
                                         replay::kBodyDiagCiEntity | replay::kBodyDiagCiIndex |
                                         replay::kBodyDiagRevHandle);
        d.slot = UINT16_MAX; d.liveCount = UINT16_MAX;
        d.bypass = replay::BypassRead::Unreadable;
        d.physCsTag = UINT32_MAX; d.ciTag = UINT32_MAX;
        replay::DescribeDiag(d, full, sizeof full);
        CHECK(strnlen(full, sizeof full) < sizeof full - 1);   // the buffer constant is big enough
        CHECK(std::string(full).find("bodyId=" + std::to_string(d.bodyId)) !=
              std::string::npos);
    }
    SUBCASE("the entity has no primary body") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        w::Put<uint64_t>(f.PhysicsCi() + facts::PHYS_PHYSICS_CI_BODY_GUARD_OFF, 0u);
        CHECK(RefusalOf(f) == R::NoPrimaryBody);
    }
    SUBCASE("the CPhTarget carries the dead index") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        w::Put<uint32_t>(f.Target() + facts::PHYS_TARGET_INDEX_OFF, lpf::kPhIdDead);
        CHECK(RefusalOf(f) == R::DeadTarget);
    }
    SUBCASE("no CPhSystem zone array") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        memset(f.world.system + facts::PHYS_SYSTEM_ZONE_ARRAY_OFF, 0, sizeof(void*));
        CHECK(RefusalOf(f) == R::NoPhysSystem);
    }
    SUBCASE("no CPhSystem at all: the product reads it through a global that may not be filled") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        f.fns.phSystem = nullptr;
        // With nothing injected the prologue guards decide, and on a test host no echovr.exe is
        // mapped, so the whole walk refuses as a build mismatch rather than pretending.
        CHECK(RefusalOf(f) == R::NoChain);
    }
    SUBCASE("the zone index is past the zone count") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        w::Put<uint64_t>(f.world.system + facts::PHYS_SYSTEM_ZONE_COUNT_OFF, w::FakePhysWorld::kZoneIndex);
        CHECK(RefusalOf(f) == R::BadZone);
    }
    SUBCASE("the CPhZone is a recycled slot") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        w::Put<uint32_t>(f.Zone() + lpf::kPhIdIndexOff, lpf::kPhIdDead);
        CHECK(RefusalOf(f) == R::DeadZone);
    }
    SUBCASE("the body index is past the zone's body count") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        w::Put<uint64_t>(f.Zone() + facts::PHYS_ZONE_BODY_COUNT_OFF, w::FakePhysWorld::kBodyIndex);
        CHECK(RefusalOf(f) == R::BadBodyIndex);
    }
    SUBCASE("the CPhZone has no body array") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        memset(f.Zone() + facts::PHYS_ZONE_BODY_ARRAY_OFF, 0, sizeof(void*));
        CHECK(RefusalOf(f) == R::NoBodyArray);
    }
    SUBCASE("the CPhBody is a recycled slot") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        w::Put<uint32_t>(f.localBody + lpf::kPhIdIndexOff, lpf::kPhIdDead);
        CHECK(RefusalOf(f) == R::DeadBody);
    }
    SUBCASE("the CPhBody's generation does not match the CPhTarget's") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        w::Put<uint32_t>(f.localBody + facts::PHYS_ID_TAG_OFF, w::Fixture::kTag ^ 1u);
        CHECK(RefusalOf(f) == R::TagMismatch);
    }
    SUBCASE("a CPhBody whose span runs off the mapping refuses, and does not fault") {
        auto heldf = std::make_unique<w::Fixture>();
        w::Fixture& f = *heldf;
        PagePair pp;
        REQUIRE(pp.Ok() != nullptr);
        // Readable up to the page boundary and reserved past it, so every field this reader takes
        // is beyond the mapping: without the span check the first memcpy dies in the reserved page.
        uint8_t* const straddling = pp.Straddling(64);
        memset(straddling, 0, 64);
        // The zone's body array is what the walk indexes, so aiming index 0 at the straddling page
        // is how a real freed body would present.
        w::Put<uint32_t>(f.Target() + facts::PHYS_TARGET_INDEX_OFF, 0u);
        w::PutPtr(f.Zone() + facts::PHYS_ZONE_BODY_ARRAY_OFF, straddling);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        replay::BodyState s{};
        replay::BodyReading r;
        CHECK_FALSE(entity->ReadBodyDetailed(replay::PhysHandle(1), s, r));
        CHECK(r.refusedAt == R::Unreadable);
        // It resolved an ADDRESS and refused to read it, which is a different answer from "the
        // physics CS did not answer" -- and only these two fields together say so.
        CHECK_FALSE(r.bodyResolved);
        CHECK(r.source == replay::BodySource::None);
    }
}

TEST_CASE("B1: every BodyRefusal spells a distinct token and a distinct sentence") {
    using R = replay::BodyRefusal;
    const R all[] = {R::None, R::NoCi, R::NoChain, R::NoPhysicsCs, R::NoPhysicsPool,
                     R::MapUnreadable, R::BadSlot, R::CiUnreadable, R::NoPrimaryBody,
                     R::DeadTarget, R::NoPhysSystem, R::BadZone, R::ZoneUnreadable, R::DeadZone,
                     R::BadBodyIndex, R::NoBodyArray, R::Unreadable, R::DeadBody, R::TagMismatch};
    const size_t n = sizeof all / sizeof all[0];
    for (size_t i = 0; i < n; ++i) {
        CHECK(std::string(replay::BodyRefusalName(all[i])) != "unknown");
        CHECK(std::string(replay::Describe(all[i])) != "unknown refusal");
        for (size_t j = i + 1; j < n; ++j) {
            CHECK(std::string(replay::BodyRefusalName(all[i])) != replay::BodyRefusalName(all[j]));
            CHECK(std::string(replay::Describe(all[i])) != replay::Describe(all[j]));
        }
    }
    // An uncased value must read as unknown, never as a blank field a consumer scores as health.
    const R bogus = static_cast<R>(0xEE);
    CHECK(std::string(replay::BodyRefusalName(bogus)) == "unknown");
    CHECK(std::string(replay::Describe(bogus)) == "unknown refusal");
}

TEST_CASE("B1: an IEntity that predates ReadBodyDetailed says notAsked, and never invents a source") {
    struct OldBody final : replay::IEntity {
        size_t PhysCount() const override { return 0; }
        bool   IsLocallyControlled(replay::PhysHandle) const override { return false; }
        void   SetSyntheticOwner(replay::PhysHandle) override {}
        void   ClearSyntheticOwner(replay::PhysHandle) override {}
        bool   ReadBody(replay::PhysHandle, replay::BodyState& s) const override {
            s.com[0] = 42.0f;
            return true;
        }
        void   PushHistory(replay::PhysHandle, const replay::BodyState&, uint64_t) override {}
        void   TrimHistory(replay::PhysHandle, uint64_t, uint64_t) override {}
        float  GetTimeOffset(replay::PhysHandle) const override { return 0.0f; }
        void   SetTimeOffset(replay::PhysHandle, float) override {}
        size_t PlayerCount() const override { return 0; }
        bool   ReadPlayer(replay::PlayerHandle, replay::PlayerState&) const override { return false; }
        void   PushPlayerHistory(replay::PlayerHandle, const replay::PlayerState&, uint64_t) override {}
        replay::PhysHandle   PhysOf(replay::PlayerHandle) const override { return replay::PhysHandle(0xFFFFu); }  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
        replay::PlayerHandle PlayerOf(replay::UserIndex) const override { return replay::PlayerHandle(0xFFFFu); }  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
        void   SetLodThrottle(bool) override {}
        bool   Available() const override { return false; }
        const char* Diagnostic() const override { return "oldBody"; }
    };
    OldBody old;
    replay::BodyState s{};
    replay::BodyReading r;
    // The default forwards the read AND the answer: it must not turn a successful ReadBody into a
    // claim about which source produced it.
    CHECK(old.ReadBodyDetailed(replay::PhysHandle(0), s, r));
    CHECK(s.com[0] == 42.0f);
    CHECK(r.source == replay::BodySource::NotAsked);
    CHECK(std::string(replay::BodySourceName(r.source)) == "notAsked");
    CHECK_FALSE(r.ciResolved);
    CHECK_FALSE(r.historyPresent);
    CHECK_FALSE(r.bodyResolved);
    CHECK(r.refusedAt == replay::BodyRefusal::None);
}

// The seven offsets above are the thing under test, and localpawn_facts.h now aliases
// facts_generated.h -- which is IN this build, so every fixture that plants a field and every
// read that takes it still move TOGETHER when the generated value is edited, and no case here
// can see it. This is the outside record that makes them fail-able: the merged row in
// facts/facts.tsv, which the generator reads and whose anchor tests/factcheck re-decodes out of
// the pinned echovr.exe. Edit either side and this reds.
TEST_CASE("B1: the CPhBody offsets are the merged registry rows, not local opinion") {
    namespace lpf = replay::localpawn_facts;
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
        {"PHYS.BODY_POS_OFF",        lpf::kPhBodyPosOff},
        {"PHYS.BODY_COM_OFFSET_OFF", lpf::kPhBodyComOffsetOff},
        {"PHYS.BODY_ORI_OFF",        lpf::kPhBodyOriOff},
        {"PHYS.BODY_VEL_OFF",        lpf::kPhBodyVelOff},
        {"PHYS.BODY_PREV_VEL_OFF",   lpf::kPhBodyPrevVelOff},
        {"PHYS.CI_BODY_ID_OFF",      lpf::kPhysCiBodyIdOff},
        {"PHYS.NETCS_PHYSICS_CS_OFF", lpf::kNetCsPhysicsCsOff},
        // The three PC_PRESENT rows facts_generated.h refuses to emit, which localpawn_facts.h
        // therefore restates. This is the outside record that makes those three fail-able too.
        {"PHYS.ID_INDEX_OFF",        lpf::kPhIdIndexOff},
        {"PHYS.ID_DEAD",             lpf::kPhIdDead},
        {"PHYS.TARGET_TAG_OFF",      lpf::kPhTargetTagOff},
    };
    size_t found = 0;
    for (const Row& r : rows) {
        std::string needle(1, '\n');
        needle += r.id;
        needle += "\t0x";
        const size_t at = blob.find(needle);
        // An absent row would leave the comparison below with nothing to disagree with.
        CHECK_MESSAGE(at != std::string::npos, r.id);
        if (at == std::string::npos) continue;
        ++found;
        CHECK(static_cast<uint32_t>(strtoul(blob.c_str() + at + needle.size(), nullptr, 16)) == r.value);
    }
    CHECK(found == sizeof rows / sizeof rows[0]);
}

// ---------------------------------------------------------------------------------------------
// W1-E COST: what ReadLocalBody now pays for calling PHYS.ANGVEL_FROM_CLOUD every tick.
//
// The engine's function cannot be called here: no echovr.exe is mapped in a test host, so
// 0x140643B30 is not code. FakeAngVelFromCloud above is a memcpy -- measuring IT would measure
// nothing, which is the whole trap ("the fake must cost what the real one costs"). So the number
// below is measured against a REPRODUCTION of the engine's algorithm, run on the same shape of
// input, and it is an estimate of the engine's cost, not a reading of it. What licenses the
// reproduction is the callee list decompiled for the PHYS.ANGVEL_FROM_CLOUD registry row:
// sub_140639150 takes the mean of the cloud (sub_1401D8A40), accumulates a 3x3 system over it
// (sub_14063ABB0), then determinant (sub_1401E26B0), inverse (sub_1401EB820), apply
// (sub_140639280). That is the normal-equation least squares for omega minimising
// sum |omega x r_i - dv_i|^2, and the loop below is that, in the same two passes.
//
// N is NOT known: the particle count lives at CPhBody+0x438 and NO registry row carries that
// offset -- PHYS.* has no particle-cloud layout fact, verified by grepping facts/facts.tsv for
// `particle`, `cloud` and `0x438`. So this sweeps N instead of asserting one, which also answers
// the more useful question: at what cloud size would the budget actually break.
namespace w1e_angvel_cost {

constexpr int   kMaxParticles = 512;
constexpr double kGatherBudgetUs = 30.0;   // the body gather budget, flat, per tick

float g_pos[kMaxParticles * 3];
float g_vel[kMaxParticles * 3];
int   g_particles = 0;
float g_costSink = 0.0f;

void FillCloud() {
    // Deterministic and non-degenerate: a degenerate cloud makes the 3x3 singular, and a solver
    // that bails early on a singular matrix would under-report its own cost.
    uint32_t s = 0x9E3779B9u;
    for (int i = 0; i < kMaxParticles * 3; ++i) {
        s = s * 1664525u + 1013904223u;
        g_pos[i] = float(int(s >> 8) % 4001 - 2000) * 0.001f;
        s = s * 1664525u + 1013904223u;
        g_vel[i] = float(int(s >> 8) % 4001 - 2000) * 0.002f;
    }
}

// The reproduction. Two O(N) passes plus fixed 3x3 algebra, exactly the shape of sub_140639150.
void LeastSquaresAngVel(const float* pos, const float* vel, int n, float* out) {
    out[0] = out[1] = out[2] = 0.0f;
    if (n <= 0) return;
    float pm[3] = {0, 0, 0}, vm[3] = {0, 0, 0};
    for (int i = 0; i < n; ++i) {
        pm[0] += pos[i * 3]; pm[1] += pos[i * 3 + 1]; pm[2] += pos[i * 3 + 2];
        vm[0] += vel[i * 3]; vm[1] += vel[i * 3 + 1]; vm[2] += vel[i * 3 + 2];
    }
    const float inv = 1.0f / float(n);
    for (int k = 0; k < 3; ++k) { pm[k] *= inv; vm[k] *= inv; }

    float a[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    float b[3] = {0, 0, 0};
    for (int i = 0; i < n; ++i) {
        const float r[3] = {pos[i * 3] - pm[0], pos[i * 3 + 1] - pm[1], pos[i * 3 + 2] - pm[2]};
        const float d[3] = {vel[i * 3] - vm[0], vel[i * 3 + 1] - vm[1], vel[i * 3 + 2] - vm[2]};
        const float rr = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        a[0] += rr - r[0] * r[0]; a[1] += -r[0] * r[1];      a[2] += -r[0] * r[2];
        a[3] += -r[1] * r[0];     a[4] += rr - r[1] * r[1];  a[5] += -r[1] * r[2];
        a[6] += -r[2] * r[0];     a[7] += -r[2] * r[1];      a[8] += rr - r[2] * r[2];
        b[0] += r[1] * d[2] - r[2] * d[1];
        b[1] += r[2] * d[0] - r[0] * d[2];
        b[2] += r[0] * d[1] - r[1] * d[0];
    }
    const float c0 = a[4] * a[8] - a[5] * a[7];
    const float c1 = a[5] * a[6] - a[3] * a[8];
    const float c2 = a[3] * a[7] - a[4] * a[6];
    const float det = a[0] * c0 + a[1] * c1 + a[2] * c2;
    if (det == 0.0f) return;
    const float id = 1.0f / det;
    const float m[9] = {
        c0 * id,                          (a[2] * a[7] - a[1] * a[8]) * id, (a[1] * a[5] - a[2] * a[4]) * id,
        c1 * id,                          (a[0] * a[8] - a[2] * a[6]) * id, (a[2] * a[3] - a[0] * a[5]) * id,
        c2 * id,                          (a[1] * a[6] - a[0] * a[7]) * id, (a[0] * a[4] - a[1] * a[3]) * id};
    out[0] = m[0] * b[0] + m[1] * b[1] + m[2] * b[2];
    out[1] = m[3] * b[0] + m[4] * b[1] + m[5] * b[2];
    out[2] = m[6] * b[0] + m[7] * b[1] + m[8] * b[2];
}

const float* CostAngVelFromCloud(void*, float* out) {
    LeastSquaresAngVel(g_pos, g_vel, g_particles, out);
    g_costSink += out[0] + out[1] + out[2];
    return out;
}

// The floor: the same reader with no engine call at all -- the mutant's shape, used here as the
// control arm so the angvel share is a DIFFERENCE measured in the same run rather than an
// absolute number compared against a number from some other run on some other machine.
const float* NullAngVelFromCloud(void*, float* out) {
    out[0] = out[1] = out[2] = 0.0f;
    return out;
}

double BestUs(replay::IEntity* entity, int calls, int reps) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    double best = 1e30;
    for (int rep = 0; rep < reps; ++rep) {
        LARGE_INTEGER a{}, b{};
        int ok = 0;
        QueryPerformanceCounter(&a);
        for (int i = 0; i < calls; ++i) {
            replay::BodyState s{};
            replay::BodyReading r;
            ok += entity->ReadBodyDetailed(replay::PhysHandle(1), s, r) ? 1 : 0;
        }
        QueryPerformanceCounter(&b);
        REQUIRE(ok == calls);
        const double us = double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart);
        if (us < best) best = us;
    }
    return best;
}

}  // namespace w1e_angvel_cost

TEST_CASE("W1-E cost: the per-tick angular-velocity call fits the 30 us body gather budget") {
    namespace w = w1e_localbody;
    namespace c = w1e_angvel_cost;
    Env& e = TheEnv();
    e.Tick(nullptr);
    c::FillCloud();

    // The span proof is cached after the first read (SpanProofMaxCachedReads), so a loop measures
    // the WARM per-tick path -- which is the path the budget is about.
    const int kCalls = 2000;
    const int kReps  = 5;
    const int kSweep[4] = {8, 32, 128, 512};
    double perCallAt[4] = {0, 0, 0, 0};
    double floorUs = 0.0;

    {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.fns.physAngVelFromCloud =
            reinterpret_cast<replay::entity::AnyEngineFn>(&c::NullAngVelFromCloud);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        floorUs = c::BestUs(entity, kCalls, kReps) / double(kCalls);
    }

    for (int k = 0; k < 4; ++k) {
        auto held = std::make_unique<w::Fixture>();
        w::Fixture& f = *held;
        f.fns.physAngVelFromCloud =
            reinterpret_cast<replay::entity::AnyEngineFn>(&c::CostAngVelFromCloud);
        replay::IEntity* entity = f.Build();
        REQUIRE(entity->Available());
        c::g_particles = kSweep[k];
        perCallAt[k] = c::BestUs(entity, kCalls, kReps) / double(kCalls);
    }
    REQUIRE(c::g_costSink != 0.0f);   // the solve was not folded away

    for (int k = 0; k < 4; ++k) {
        MESSAGE("W1-E ANGVEL COST: N=" << kSweep[k] << " particles -> "
                << perCallAt[k] << " us per ReadBodyDetailed ("
                << (perCallAt[k] - floorUs) << " us of it the least-squares itself); "
                << "gather floor with no engine call " << floorUs << " us");
    }

    // The budget is 30 us per tick, flat, for the whole body gather. The largest cloud in the
    // sweep is the worst case measured, and it must clear the budget on its own.
    CHECK(perCallAt[3] < c::kGatherBudgetUs);
    // Same-run ratio, machine-independent: the added cost at the largest swept cloud must stay
    // under the budget even if a tick gathers a whole Echo Arena roster of bodies (8 players plus
    // the disc = 9). An absolute microsecond figure alone measures this machine; this does not.
    CHECK((perCallAt[3] - floorUs) * 9.0 < c::kGatherBudgetUs);
    // The cost must actually SCALE with N -- if it did not, the reproduction is not running over
    // the cloud and the whole measurement is of something else.
    CHECK(perCallAt[3] > perCallAt[0]);

    c::g_particles = 0;
    g_physCs = nullptr;
    g_playerCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// ------------------------------------------------------------------------------------------
// bits11 through the SHIPPED encoder.  L2 #15 F38: the reader answered 7080 times and the writer
// refused all 7080 rows, because the CI flags dword was handed to an 11-bit field.  These cases
// drive capture's own BodyEncoder, so the width they check is the one the capture path enforces.
// ------------------------------------------------------------------------------------------
namespace {

replay::capture::BodyFrame BitsFrame(uint32_t bits11) {
    replay::capture::BodyFrame f{};
    f.idValue = (static_cast<uint64_t>(1) << 32) | 1u;  // hi dword 1, DESIGN 5.4.5
    f.idTag = 0;
    f.bits11 = bits11;
    f.fill = replay::capture::kFillBodyTransform | replay::capture::kFillBodyLastHeld | replay::capture::kFillBodyBits |
             replay::capture::kFillBodyIdValue | replay::capture::kFillBodyIdNetActor;
    return f;
}

bool EncodesClean(uint32_t bits11) {
    uint8_t buf[512] = {};
    bitio::BitWriter w(buf, sizeof buf);
    replay::capture::BodyEncoder enc(true);
    enc.Reset();
    const bool ok = enc.Encode(w, 7, 1234567, BitsFrame(bits11));
    return ok && !w.Refused();
}

// The live CI word as the engine holds it during a paused, integration-disabled tick with both
// sequence nibbles set: every named bit of PHYS.CI_FLAGS_OFF at once.
constexpr uint32_t kOwnerSeq = facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK;
constexpr uint32_t kWarpSeq = facts::PHYS_CI_OWNERSEQ_NIBBLE_MASK << 4;  // bits 4-7, same shape
constexpr uint32_t kLiveCiFlags =
    kOwnerSeq | kWarpSeq | facts::PHYS_CI_INTERP_ENABLE_BIT | facts::PHYS_CI_PAUSED_BIT;

}  // namespace

TEST_CASE("the raw CI flags word is refused by the shipped body encoder") {
    // This is the RED the fix exists for: if it ever passes, the width check stopped discriminating
    // and every case below is vacuous.
    CHECK_FALSE(EncodesClean(kLiveCiFlags));
    CHECK_FALSE(EncodesClean(facts::PHYS_CI_PAUSED_BIT));
    CHECK_FALSE(EncodesClean(facts::PHYS_CI_INTERP_ENABLE_BIT));
}

TEST_CASE("BodyWireBits produces a value the shipped body encoder accepts") {
    CHECK(EncodesClean(replay::BodyWireBits(kLiveCiFlags)));
    CHECK(EncodesClean(replay::BodyWireBits(~0u)));
    CHECK(EncodesClean(replay::BodyWireBits(facts::PHYS_CI_PAUSED_BIT)));
}

TEST_CASE("BodyWireBits keeps the wire content and drops only what the field cannot hold") {
    // The two nibbles are what the format's `bits` field is made of -- the corpus in the design D.2
    // varies exactly the pose-warp nibble across its 16 SimState payloads -- so losing either would
    // be a silent re-quantisation, not a width fix.
    CHECK((replay::BodyWireBits(kLiveCiFlags) & kOwnerSeq) == kOwnerSeq);
    CHECK((replay::BodyWireBits(kLiveCiFlags) & kWarpSeq) == kWarpSeq);
    CHECK(replay::BodyWireBits(kOwnerSeq | kWarpSeq) == (kOwnerSeq | kWarpSeq));
    CHECK((replay::BodyWireBits(kLiveCiFlags) & facts::PHYS_CI_PAUSED_BIT) == 0u);
    CHECK((replay::BodyWireBits(kLiveCiFlags) & facts::PHYS_CI_INTERP_ENABLE_BIT) == 0u);
}

TEST_CASE("every bits11 assignment in entity.cpp goes through BodyWireBits") {
    // A unit test cannot see that a READER bypassed the converter, and three of the four assignments
    // are on engine-memory paths no offline test reaches. This reads the shipped source instead: the
    // converter being right is worth nothing if a body reader still assigns the raw dword.
    std::string path = std::string(REPLAY_REPO_ROOT) + "/src/engine/entity.cpp";
    FILE* f = fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::string src;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) src.append(chunk, got);
    fclose(f);
    REQUIRE(src.find("BodyWireBits(") != std::string::npos);

    int assignments = 0, converted = 0;
    size_t line = 0;
    while (line < src.size()) {
        size_t end = src.find('\n', line);
        if (end == std::string::npos) end = src.size();
        const std::string s = src.substr(line, end - line);
        if (s.find("bits11 =") != std::string::npos && s.find("uint32_t bits11") == std::string::npos) {
            ++assignments;
            if (s.find("BodyWireBits(") != std::string::npos) ++converted;
        }
        line = end + 1;
    }
    CHECK(assignments == 4);
    CHECK(converted == assignments);
}

// ===================================================================================================
// THE ENTITY KEY. W2-P1's census printed `entity=none` on every row because nothing here published
// the engine's own name for a pawn, and FACT-PP1's arms (2) and (3) differ in exactly whether the
// two surfaces hold ONE entity or two. These cases plant the two surfaces with DIFFERENT physics
// handles for the same user slot -- which is what the engine does, the remote pool being indexed by
// the player CI's component handle and the local one by the component on the broadcaster's actor --
// so a reader that asks whichever surface it likes cannot pass them.
// ===================================================================================================
namespace {

// The physics component the local pawn's actor carries. Deliberately NOT the handle the player CI
// names, and not 0: a fixture whose two surfaces agree cannot fail a reader that confuses them.
uint16_t g_localPhysHandle = 1u;

void* LocalPhysComponent(void* cs, void* out, uint64_t, uint64_t) {
    uint32_t id = cs ? uint32_t(g_localPhysHandle) : uint32_t(replay::localpawn_facts::kNoComponent);
    memcpy(out, &id, sizeof id);
    return out;
}

struct EntityKeyRig {
    LocalPawnRig rig;
    alignas(16) uint8_t physVtable[512] = {};

    static const uint16_t kRemotePhys = 2u;   // the handle PlayerCI+28 names
    static const uint16_t kLocalPhys  = 1u;   // the handle the actor's component is
    static const uint16_t kLiveCis    = 4u;

    uint8_t* PhysCi(uint16_t index) {
        return rig.phys.elements + facts::PHYS_CI_STRIDE * index;
    }

    EntityKeyRig() {
        namespace lpf = replay::localpawn_facts;
        g_localPhysHandle = kLocalPhys;

        // FirstActorComponent is checked against the PHYSICS system's own vtable slot before
        // LocalPawnAt calls it, so the fake system carries a vtable that names the fake function.
        void* fn = reinterpret_cast<void*>(&LocalPhysComponent);
        memcpy(physVtable + lpf::kFindActorComponentSlotOff, &fn, sizeof fn);
        rig.fns.findActorComponent = reinterpret_cast<replay::entity::AnyEngineFn>(&LocalPhysComponent);
        rig.phys.PutPtr(0, physVtable);
        rig.PublishKeys();

        rig.phys.Wire(facts::PHYS_CS_HANDLE_MAP_OFF, facts::PHYS_CS_POOL_BASE_OFF,
                      facts::PHYS_CS_COUNT_OFF, facts::PHYS_CS_REVERSE_MAP_OFF, kLiveCis);
        // Handle == index here, and both maps say so, so the identity gates are answered by the
        // engine's own round trip rather than by a zero fill.
        for (uint16_t i = 0; i < kLiveCis; ++i) {
            rig.phys.handleMap[i] = i;
            rig.phys.reverseMap[i] = i;
            const uint16_t id = i;
            memcpy(PhysCi(i) + lpf::kNetCiEntityIdOff, &id, sizeof id);
        }

        // The remote surface: one player CI for this user slot, naming a DIFFERENT physics handle.
        const uint16_t slot = LocalPawnRig::kSlot;
        memcpy(rig.player.elements + facts::PLAYER_CI_USERID_OFF, &slot, sizeof slot);
        const uint16_t remote = kRemotePhys;
        memcpy(rig.player.elements + facts::PLAYER_CI_HANDLE_OFF, &remote, sizeof remote);
    }
};

}  // namespace

TEST_CASE("the entity key is read per SURFACE, and the two surfaces do not answer for each other") {
    using Key = replay::IEntity::EntityKey;
    using Surface = replay::IEntity::PawnSurface;
    EntityKeyRig f;
    replay::IEntity* entity = f.rig.Create();
    REQUIRE(entity->Available());
    const replay::UserIndex slot(LocalPawnRig::kSlot);

    // Both surfaces hold this slot, and they hold DIFFERENT pawns -- the shape FACT-PP1 asks about.
    replay::IEntity::LocalPawnRef ref;
    REQUIRE(entity->LocalPawnAt(slot, ref) == replay::IEntity::LocalPawn::Ok);
    REQUIRE(ref.phys.v == EntityKeyRig::kLocalPhys);
    REQUIRE(entity->PhysOf(entity->PlayerOf(slot)).v == EntityKeyRig::kRemotePhys);

    uint16_t remoteKey = 0xEEEEu, localKey = 0xEEEEu;
    CHECK(entity->EntityKeyOf(slot, Surface::Remote, remoteKey) == Key::Ok);
    CHECK(remoteKey == EntityKeyRig::kRemotePhys);
    CHECK(entity->EntityKeyOf(slot, Surface::Local, localKey) == Key::Ok);
    CHECK(localKey == EntityKeyRig::kLocalPhys);
    // The whole point of naming the surface: the answers are not interchangeable.
    CHECK(remoteKey != localKey);
}

TEST_CASE("the entity key refuses by name, and absence is never a value") {
    using Key = replay::IEntity::EntityKey;
    using Surface = replay::IEntity::PawnSurface;
    EntityKeyRig f;
    replay::IEntity* entity = f.rig.Create();
    REQUIRE(entity->Available());

    // A slot neither surface holds. `out` is untouched, because every u16 is a legal entity id and
    // a written 0 would read as the local pawn's.
    const replay::UserIndex absent(LocalPawnRig::kSlot + 1u);
    uint16_t key = 0xEEEEu;
    CHECK(entity->EntityKeyOf(absent, Surface::Remote, key) == Key::NoPawn);
    CHECK(key == 0xEEEEu);
    CHECK(entity->EntityKeyOf(absent, Surface::Local, key) == Key::NoPawn);
    CHECK(key == 0xEEEEu);

    // A CI whose own field disagrees with the handle that addressed it: the identity gate refuses,
    // and the key is NOT published off it.
    const uint16_t wrong = 9u;
    memcpy(f.PhysCi(EntityKeyRig::kRemotePhys) + replay::localpawn_facts::kNetCiEntityIdOff, &wrong,
           sizeof wrong);
    CHECK(entity->EntityKeyOf(replay::UserIndex(LocalPawnRig::kSlot), Surface::Remote, key) ==
          Key::IdentityRefused);
    CHECK(key == 0xEEEEu);

    // A handle NEITHER reading can place inside the live count addresses no instance at all -- the
    // forward map sends it past the count and it is not an index of one either. A different refusal
    // from the one above, and the caller gets to tell them apart.
    const uint16_t restored = EntityKeyRig::kRemotePhys;
    memcpy(f.PhysCi(EntityKeyRig::kRemotePhys) + replay::localpawn_facts::kNetCiEntityIdOff,
           &restored, sizeof restored);
    const uint16_t past = EntityKeyRig::kLiveCis + 3u;
    f.rig.phys.handleMap[past] = EntityKeyRig::kLiveCis + 5u;
    memcpy(f.rig.player.elements + facts::PLAYER_CI_HANDLE_OFF, &past, sizeof past);
    CHECK(entity->EntityKeyOf(replay::UserIndex(LocalPawnRig::kSlot), Surface::Remote, key) ==
          Key::NoCi);
    CHECK(key == 0xEEEEu);
}

// ---- DISC-BIND -------------------------------------------------------------------------------------

namespace {

uint32_t MapEntryWord(uint16_t poolIndex, uint16_t generation) {
    return uint32_t(poolIndex) | (uint32_t(generation) << 16);
}

}  // namespace

TEST_CASE("DISC-BIND: a wire SComponentID resolves the way SimStateCB resolves it, or not at all") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    FakeCs phys, player;
    phys.WirePhys();
    player.WirePlayer();
    // Three fields, ordered enabled <= live <= capacity. A validity test bounds the DENSE index
    // with the LIVE count at +0xF2; +0xF4 counts only the enabled prefix.
    const uint16_t live = 3, ids = 8, enabled = 3;
    memcpy(phys.bytes + lpf::kPhysCsLiveCountOff, &live, sizeof live);
    memcpy(phys.bytes + facts::PHYS_CS_COUNT_OFF, &enabled, sizeof enabled);
    memcpy(phys.bytes + lpf::kPhysCsIdCapacityOff, &ids, sizeof ids);
    phys.handleMap[1] = MapEntryWord(1, 0);
    phys.handleMap[5] = MapEntryWord(2, 7);
    phys.handleMap[6] = MapEntryWord(3, 7);   // a pool index AT the live count
    phys.handleMap[8] = MapEntryWord(1, 7);   // well formed, and past the forward map's own size
    phys.reverseMap[1] = 1;
    phys.reverseMap[2] = 5;
    const uint32_t kFlags = 0x00000810u;
    memcpy(phys.elements + facts::PHYS_CI_STRIDE * 2 + facts::PHYS_CI_FLAGS_OFF, &kFlags,
           sizeof kFlags);
    g_physCs = &phys;
    g_playerCs = &player;

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());
    using Seen = replay::IEntity::ComponentIdReading;

    Seen seen;
    CHECK(entity->PhysOfComponentId(5u | (7u << 16), &seen).v == 5);
    CHECK(seen.mapRead);
    CHECK(seen.poolIndex == 2);
    CHECK(seen.generation == 7);
    CHECK(seen.idCapacity == 8);
    CHECK(seen.liveCount == 3);
    REQUIRE(seen.ciFlagsRead);
    CHECK(seen.ciFlags == kFlags);
    CHECK(entity->PhysOfComponentId(5u | (7u << 16)).v == 5);   // the reading is optional

    // The same index under another generation is a different, dead component.
    CHECK(entity->PhysOfComponentId(5u | (6u << 16), &seen).v == 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(seen.mapRead);
    CHECK(seen.generation == 7);

    CHECK(entity->PhysOfComponentId(6u | (7u << 16), &seen).v == 0xFFFFu);   // pool index == count  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK(seen.poolIndex == 3);

    CHECK(entity->PhysOfComponentId(8u | (7u << 16), &seen).v == 0xFFFFu);   // index == capacity  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    CHECK_FALSE(seen.mapRead);
    CHECK(entity->PhysOfComponentId(7u | (0u << 16)).v == 7);                // capacity - 1 stands

    // A component the engine has DISABLED is still live: its dense index is at or past cs+0xF4 and
    // below cs+0xF2, and bounding with +0xF4 refuses a disc that is merely parked between rounds.
    const uint16_t disabled_only = 1;
    memcpy(phys.bytes + facts::PHYS_CS_COUNT_OFF, &disabled_only, sizeof disabled_only);
    CHECK(entity->PhysOfComponentId(5u | (7u << 16), &seen).v == 5);
    CHECK(seen.poolIndex == 2);
    CHECK(seen.liveCount == 3);
    memcpy(phys.bytes + facts::PHYS_CS_COUNT_OFF, &enabled, sizeof enabled);

    // The disc's own id is {0, 0}: the claim goes through the identity gate's id-0 arm, and the
    // release puts the host's owner back.
    const uint32_t kHostOwner = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
    phys.Owner(0) = kHostOwner;
    const replay::PhysHandle disc = entity->PhysOfComponentId(0);
    REQUIRE(disc.v == 0);
    entity->SetSyntheticOwner(disc);
    CHECK(phys.Owner(0) == 0xFFFE0010u);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
    entity->ClearSyntheticOwner(disc);
    CHECK(phys.Owner(0) == kHostOwner);

    g_physCs = nullptr;
    g_playerCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

namespace {

// The engine's own path to the disc: CR15NetGame+0x2AA8 -> the R14Frisbee component ref -> the
// frisbee system's pool -> SR15FrisbeeCI+0x18 -> the net-physics system the frisbee cached, which
// on a real host is the LEVEL space's instance and not the one player bodies come from.
struct DiscChain {
    alignas(16) uint8_t netGame[0x2B00] = {};
    alignas(16) uint8_t rules[0x300] = {};  // facts-ok: WORLD.REORIGIN_FLAG_BITS a local buffer size, not the engine offset
    alignas(16) uint8_t level[0x2200] = {};
    FakeCs frisbee;
    FakeCs discPhys;
    uint64_t epoch = 0x1122334455667788ull;

    uint8_t* Ref() { return rules + replay::localpawn_facts::kRulesFrisbeeRefOff; }
    static void PutPtr(uint8_t* p, uint32_t off, const void* v) { memcpy(p + off, &v, sizeof v); }
    static void Put32(uint8_t* p, uint32_t off, uint32_t v) { memcpy(p + off, &v, sizeof v); }
    static void Put16(uint8_t* p, uint32_t off, uint16_t v) { memcpy(p + off, &v, sizeof v); }

    // `dense` is where the frisbee component sits in its own pool; `enabled` is cs+0xF4, which is a
    // PREFIX of the live array and must not bound a validity test.
    void Build(uint32_t frisbeeCid, uint32_t physId, uint16_t dense = 0, uint16_t enabled = 4) {
        namespace lpf = replay::localpawn_facts;
        PutPtr(netGame, lpf::kNetGameRulesOff, rules);
        PutPtr(Ref(), lpf::kCompRefSpaceOff, level);
        memcpy(level + lpf::kGameSpaceEpochOff, &epoch, sizeof epoch);
        memcpy(Ref() + lpf::kCompRefStampOff, &epoch, sizeof epoch);
        PutPtr(Ref(), lpf::kCompRefCsOff, frisbee.bytes);
        Put32(Ref(), lpf::kCompRefIdOff, frisbeeCid);

        frisbee.PutPtr(lpf::kFrisbeeCsMapOff, frisbee.handleMap);
        frisbee.PutPtr(lpf::kFrisbeeCsPoolBaseOff, frisbee.elements);
        Put16(frisbee.bytes, lpf::kPhysCsIdCapacityOff, 8);
        Put16(frisbee.bytes, lpf::kPhysCsLiveCountOff, 4);
        Put16(frisbee.bytes, lpf::kFrisbeeCsCountOff, enabled);
        frisbee.handleMap[(uint16_t)frisbeeCid] =
            MapEntryWord(dense, (uint16_t)(frisbeeCid >> 16));
        Put32(frisbee.elements, lpf::kFrisbeeCiStride * dense + lpf::kFrisbeeCiPhysIdOff, physId);
        frisbee.PutPtr(lpf::kFrisbeeCsPhysCsOff, discPhys.bytes);

        discPhys.WirePhys();
        Put16(discPhys.bytes, lpf::kPhysCsIdCapacityOff, 8);
        Put16(discPhys.bytes, lpf::kPhysCsLiveCountOff, 4);
        Put16(discPhys.bytes, facts::PHYS_CS_COUNT_OFF, 4);
        discPhys.handleMap[(uint16_t)physId] = MapEntryWord((uint16_t)physId, (uint16_t)(physId >> 16));
        discPhys.reverseMap[(uint16_t)physId] = (uint16_t)physId;

        PutPtr(rules, lpf::kRulesPhysCsOff, discPhys.bytes);
        Put32(rules, lpf::kRulesPhysIdOff, physId);
    }
};

}  // namespace

TEST_CASE("DISC-BIND: the rules chain finds the disc, at every hop or not at all") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    FakeCs phys, player;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());
    using Bind = replay::IEntity::DiscBind;

    const uint32_t kFrisbeeCid = 0x00070005u;
    const uint32_t kPhysId = 0x00000002u;

    SUBCASE("the chain resolves, and the disc's pool is NOT the players'") {
        DiscChain c;
        c.Build(kFrisbeeCid, kPhysId, /*dense=*/2, /*enabled=*/1);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        CHECK(b.refused == nullptr);
        CHECK(b.frisbeeCid == kFrisbeeCid);
        CHECK(b.physId == kPhysId);
        CHECK(b.frisbeeCs == c.frisbee.bytes);
        CHECK(b.physCs == c.discPhys.bytes);
        CHECK(b.playerPhysCs == phys.bytes);
        CHECK(b.physCs != b.playerPhysCs);
        CHECK(b.shortcutAgreed);
        CHECK(b.handle.v == (uint16_t)kPhysId);
        CHECK(b.handle.pool == replay::PhysHandle::kPoolDisc);
        // cs+0xF4 (1) is BELOW the dense index the frisbee sits at (2): the engine bounds a dense
        // index with cs+0xF2, and the enabled count is a prefix of the same array.
        CHECK(entity->DiscStands(kPhysId));
    }

    SUBCASE("no netGame, no rules block, an unresolved ref, a stale ref") {
        DiscChain c;
        c.Build(kFrisbeeCid, kPhysId);
        Bind b;
        CHECK_FALSE(entity->BindDisc(nullptr, b));
        CHECK(std::string(b.refused).find("no rules block") != std::string::npos);

        DiscChain::PutPtr(c.netGame, lpf::kNetGameRulesOff, nullptr);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        DiscChain::PutPtr(c.netGame, lpf::kNetGameRulesOff, c.rules);

        DiscChain::PutPtr(c.Ref(), lpf::kCompRefSpaceOff, nullptr);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.refused).find("unresolved") != std::string::npos);
        DiscChain::PutPtr(c.Ref(), lpf::kCompRefSpaceOff, c.level);

        const uint64_t moved = c.epoch + 1;
        memcpy(c.level + lpf::kGameSpaceEpochOff, &moved, sizeof moved);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.refused).find("stale") != std::string::npos);
        CHECK_FALSE(entity->DiscStands(kPhysId));   // a refused bind leaves no pool behind
    }

    SUBCASE("the ref names no system, holds the cleared id, or the frisbee does not vouch") {
        DiscChain c;
        c.Build(kFrisbeeCid, kPhysId);
        Bind b;

        DiscChain::PutPtr(c.Ref(), lpf::kCompRefCsOff, nullptr);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.refused).find("no component system") != std::string::npos);
        DiscChain::PutPtr(c.Ref(), lpf::kCompRefCsOff, c.frisbee.bytes);

        DiscChain::Put32(c.Ref(), lpf::kCompRefIdOff, 0x0003FFFFu);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.refused).find("cleared component id") != std::string::npos);

        // Same index, another generation: a component that has been destroyed and recycled.
        DiscChain::Put32(c.Ref(), lpf::kCompRefIdOff, (kFrisbeeCid & 0xFFFFu) | (6u << 16));  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.refused).find("does not vouch for its own") != std::string::npos);
    }

    SUBCASE("no cached net-physics system, or one that refuses the body") {
        DiscChain c;
        c.Build(kFrisbeeCid, kPhysId);
        Bind b;

        c.frisbee.PutPtr(lpf::kFrisbeeCsPhysCsOff, nullptr);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.refused).find("caches no net-physics") != std::string::npos);
        CHECK_FALSE(b.physPoolRefused);
        c.frisbee.PutPtr(lpf::kFrisbeeCsPhysCsOff, c.discPhys.bytes);

        DiscChain::Put16(c.discPhys.bytes, lpf::kPhysCsLiveCountOff, 0);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(b.physPoolRefused);
        CHECK(std::string(b.refused).find("does not vouch for the disc body") != std::string::npos);
    }

    SUBCASE("the claim is stamped into the DISC's pool and the players' pool is untouched") {
        DiscChain c;
        c.Build(kFrisbeeCid, /*physId=*/0);
        // One live element, so the identity gate's uniqueness proof has one candidate to prove.
        DiscChain::Put16(c.discPhys.bytes, facts::PHYS_CS_COUNT_OFF, 1);
        DiscChain::Put16(c.discPhys.bytes, lpf::kPhysCsLiveCountOff, 1);
        const uint32_t kHostOwner = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
        c.discPhys.Owner(0) = kHostOwner;
        phys.Owner(0) = kHostOwner;

        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        entity->SetSyntheticOwner(b.handle);
        CHECK(c.discPhys.Owner(0) == 0xFFFE0010u);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
        CHECK(phys.Owner(0) == kHostOwner);          // the players' pool never saw the write
        entity->ClearSyntheticOwner(b.handle);
        CHECK(c.discPhys.Owner(0) == kHostOwner);

        // ForgetSyntheticOwner drops the save without writing: the component is gone and its old
        // owner belongs to whatever the engine recycled into that slot.
        entity->SetSyntheticOwner(b.handle);
        REQUIRE(c.discPhys.Owner(0) == 0xFFFE0010u);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
        entity->ForgetSyntheticOwner(b.handle);
        entity->ClearSyntheticOwner(b.handle);
        CHECK(c.discPhys.Owner(0) == 0xFFFE0010u);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
        c.discPhys.Owner(0) = kHostOwner;
    }

    SUBCASE("DiscStands falls the moment the engine recycles the component") {
        DiscChain c;
        c.Build(kFrisbeeCid, kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        REQUIRE(entity->DiscStands(kPhysId));
        c.discPhys.handleMap[(uint16_t)kPhysId] = MapEntryWord((uint16_t)kPhysId, 9);
        CHECK_FALSE(entity->DiscStands(kPhysId));
    }

    // 1. DiscStands runs on the game thread at the drive rate, and replay::Readable
    // is a VirtualQuery: clock.cpp's contract for it is "once per take -- never on the tick path",
    // and entity.cpp:740 records the capture L2-RUN-10 cost-killed for breaking that.
    SUBCASE("the per-tick revalidation asks the memory manager nothing") {
        DiscChain c;
        c.Build(kFrisbeeCid, kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));

        // The instrument first: a COLD call must move the counter, or a zero below would be the
        // counter failing to see this path rather than the path not probing.
        replay::entity::BindEpochAdvance();
        const uint64_t cold0 = replay::ReadableProbeCount();
        REQUIRE(entity->DiscStands(kPhysId));
        CHECK(replay::ReadableProbeCount() - cold0 > 0u);

        const uint64_t warm0 = replay::ReadableProbeCount();
        for (int tick = 0; tick < 240; ++tick) REQUIRE(entity->DiscStands(kPhysId));
        CHECK(replay::ReadableProbeCount() - warm0 == 0u);

        // And a REFUSAL is just as cheap: a recycled component refuses on the compare, not on a
        // fresh probe, so a disc that has gone does not cost a syscall a tick either.
        c.discPhys.handleMap[(uint16_t)kPhysId] = MapEntryWord((uint16_t)kPhysId, 9);
        const uint64_t refused0 = replay::ReadableProbeCount();
        for (int tick = 0; tick < 240; ++tick) CHECK_FALSE(entity->DiscStands(kPhysId));
        CHECK(replay::ReadableProbeCount() - refused0 == 0u);
    }

    // 2. The bound pool is a raw pointer into the LEVEL game space. DiscStands reads
    // it every tick and cannot see that the space has gone; the chain re-walk can, and runs at the
    // 2 Hz maintenance cadence instead.
    SUBCASE("a level teardown the per-tick predicate cannot see is caught by the chain re-walk") {
        DiscChain c;
        c.Build(kFrisbeeCid, /*physId=*/0);
        DiscChain::Put16(c.discPhys.bytes, facts::PHYS_CS_COUNT_OFF, 1);
        DiscChain::Put16(c.discPhys.bytes, lpf::kPhysCsLiveCountOff, 1);
        const uint32_t kHostOwner = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop
        c.discPhys.Owner(0) = kHostOwner;

        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        REQUIRE(entity->DiscStands(0));
        CHECK(entity->DiscChainStands(c.netGame));
        entity->SetSyntheticOwner(b.handle);
        REQUIRE(c.discPhys.Owner(0) == 0xFFFE0010u);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word

        // The teardown: the space re-stamps and the ref is not re-resolved yet. Every byte the
        // per-tick predicate reads is untouched, so it still says the disc stands -- which is the
        // whole reason the re-walk exists.
        const uint64_t moved = c.epoch + 1u;
        memcpy(c.level + lpf::kGameSpaceEpochOff, &moved, sizeof moved);
        CHECK(entity->DiscStands(0));
        CHECK_FALSE(entity->DiscChainStands(c.netGame));

        // Dropped without a write: the saved owner belongs to whatever the engine recycled in.
        entity->ForgetSyntheticOwner(b.handle);
        entity->DropDisc();
        CHECK(c.discPhys.Owner(0) == 0xFFFE0010u);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
        CHECK_FALSE(entity->DiscStands(0));
        CHECK_FALSE(entity->DiscChainStands(c.netGame));

        // And nothing writes through the dropped pool afterwards.
        c.discPhys.Owner(0) = kHostOwner;
        entity->SetSyntheticOwner(b.handle);
        entity->ClearSyntheticOwner(b.handle);
        CHECK(c.discPhys.Owner(0) == kHostOwner);

        // Fresh again: the retry re-binds and the claim takes.
        memcpy(c.Ref() + lpf::kCompRefStampOff, &moved, sizeof moved);
        Bind again;
        REQUIRE(entity->BindDisc(c.netGame, again));
        CHECK(entity->DiscChainStands(c.netGame));
        entity->SetSyntheticOwner(again.handle);
        CHECK(c.discPhys.Owner(0) == 0xFFFE0010u);  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
        entity->ClearSyntheticOwner(again.handle);
    }

    SUBCASE("a chain that resolves to a DIFFERENT net-physics system is a drop, not a re-bind") {
        DiscChain c;
        c.Build(kFrisbeeCid, kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        REQUIRE(entity->DiscChainStands(c.netGame));
        // A level rebuilt under us: the ref is fresh and the frisbee system is readable, but the
        // net-physics system it caches is another object.
        FakeCs rebuilt;
        rebuilt.WirePhys();
        c.frisbee.PutPtr(lpf::kFrisbeeCsPhysCsOff, rebuilt.bytes);
        CHECK_FALSE(entity->DiscChainStands(c.netGame));
        CHECK_FALSE(entity->DiscChainStands(nullptr));
        entity->DropDisc();
    }

    g_physCs = nullptr;
    g_playerCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// ---- DISC-EARLY-BIND ---------------------------------------------------------------------------

namespace {

namespace lpf_early = replay::localpawn_facts;

// The chain the early path walks: netGame -> the LEVEL game space -> that space's actor registry,
// where the game_frisbee actor is live from map load -> the space's component-system table -> the
// R14Frisbee system -> its one component -> the net-physics system it caches. No rules block: the
// whole point is that the engine has not filled one yet.
struct EarlyChain {
    alignas(16) uint8_t netGame[lpf_early::kNetGameLevelSpaceOff + 0x20] = {};
    alignas(16) uint8_t rules[0x300] = {};  // facts-ok: WORLD.REORIGIN_FLAG_BITS a local buffer size, not the engine offset
    alignas(16) uint8_t level[0x500] = {};
    alignas(16) uint8_t parent[0x500] = {};
    alignas(16) uint8_t registry[0x400] = {};
    alignas(16) uint8_t parentRegistry[0x400] = {};
    // A second level space that PARENTS onto the one the bind resolved in: the actor, its slot
    // and its system all still answer the same, so only the level-space compare can refuse it.
    alignas(16) uint8_t altLevel[0x500] = {};
    alignas(16) uint8_t refSpace[lpf_early::kGameSpaceEpochOff + 0x20] = {};
    uint64_t actorEntries[2][2] = {};
    uint64_t parentActorEntries[2][2] = {};
    uint16_t slotWords[64] = {};
    uint16_t parentSlotWords[64] = {};
    uint64_t csEntries[2][2] = {};
    uint64_t parentCsEntries[2][2] = {};
    uint64_t vtable[8] = {};
    uint64_t allocatorObject = reinterpret_cast<uintptr_t>(vtable);
    FakeCs frisbee;
    FakeCs discPhys;
    uint64_t epoch = 0x0BADC0DE0BADC0DEull;

    // A key below the frisbee's, so the fixture's tables are sorted the way the engine keeps them
    // and a binary search has something to step over.
    static constexpr uint64_t kLowKey = 0x0000000000000101ull;
    static constexpr uint16_t kActorIndex = 3;
    static constexpr uint16_t kFrisbeeHandle = 2;
    static constexpr uint16_t kFrisbeeGen = 5;

    static void PutPtr(uint8_t* p, uint32_t off, const void* v) { memcpy(p + off, &v, sizeof v); }
    static void Put64(uint8_t* p, uint32_t off, uint64_t v) { memcpy(p + off, &v, sizeof v); }
    static void Put32(uint8_t* p, uint32_t off, uint32_t v) { memcpy(p + off, &v, sizeof v); }
    static void Put16(uint8_t* p, uint32_t off, uint16_t v) { memcpy(p + off, &v, sizeof v); }

    uint8_t* Ref() { return rules + lpf_early::kRulesFrisbeeRefOff; }
    uint32_t FrisbeeCid() const {
        return uint32_t(kFrisbeeHandle) | (uint32_t(kFrisbeeGen) << 16);
    }

    // registry+0x160 holds a POINTER to the slot-word array; the engine reads the word through it
    // (sub_1404FF1A0's return expression). Wiring the word inline at registry+0x160 is the reading
    // that made the early path refuse for 552 s, so the fixture must not offer it.
    void WireRegistry(uint8_t* reg, uint64_t (*entries)[2], uint16_t* words, uint64_t key,
                      uint16_t actorIndex) {
        PutPtr(reg, lpf_early::kActorRegBaseOff, entries);
        Put64(reg, lpf_early::kActorRegCountOff, 2);
        PutPtr(reg, lpf_early::kActorRegSlotWordsPtrOff, words);
        entries[0][0] = kLowKey;
        entries[0][1] = 0;
        entries[1][0] = key;
        entries[1][1] = actorIndex;
        words[actorIndex] = lpf_early::kActorSlotLive;
    }

    void WireCsTable(uint8_t* space, uint64_t (*entries)[2], const void* cs) {
        uint8_t* t = space + lpf_early::kGameSpaceSystemTableOff;
        entries[0][0] = kLowKey;
        entries[0][1] = 0;
        entries[1][0] = lpf_early::kSymR14Frisbee;
        entries[1][1] = reinterpret_cast<uintptr_t>(cs);
        Put64(t, 0x00, reinterpret_cast<uintptr_t>(entries));
        Put64(t, 0x08, 2 * lpf_early::kSystemTableStride);
        Put64(t, 0x10, reinterpret_cast<uintptr_t>(&allocatorObject));
        Put64(t, 0x20, 2);
        Put64(t, 0x28, 2);
        Put64(t, 0x30, 2);
        Put64(t, 0x38, 0);
    }

    void Build(uint32_t physId, uint16_t live = 1) {
        PutPtr(netGame, lpf_early::kNetGameRulesOff, rules);
        PutPtr(netGame, lpf_early::kNetGameLevelSpaceOff, level);
        PutPtr(level, lpf_early::kGameSpaceParentOff, parent);
        PutPtr(level, lpf_early::kGameSpaceActorRegOff, registry);
        PutPtr(parent, lpf_early::kGameSpaceActorRegOff, parentRegistry);
        WireRegistry(registry, actorEntries, slotWords, lpf_early::kSymGameFrisbee, kActorIndex);
        WireRegistry(parentRegistry, parentActorEntries, parentSlotWords, kLowKey + 1u, kActorIndex);
        WireCsTable(level, csEntries, frisbee.bytes);
        WireCsTable(parent, parentCsEntries, frisbee.bytes);

        frisbee.PutPtr(lpf_early::kFrisbeeCsMapOff, frisbee.handleMap);
        frisbee.PutPtr(lpf_early::kFrisbeeCsPoolBaseOff, frisbee.elements);
        frisbee.PutPtr(lpf_early::kFrisbeeCsPhysCsOff, discPhys.bytes);
        Put16(frisbee.bytes, lpf_early::kPhysCsIdCapacityOff, 8);
        Put16(frisbee.bytes, lpf_early::kPhysCsLiveCountOff, live);
        // The disc the host has PARKED between rounds: enabled (+0xF4) is zero while live (+0xF2)
        // is one, and a bind bounded by the enabled prefix would refuse it.
        Put16(frisbee.bytes, lpf_early::kFrisbeeCsCountOff, 0);
        for (uint16_t h = 0; h < 8; ++h) frisbee.handleMap[h] = MapEntryWord(0xFFFFu, 0);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        frisbee.handleMap[kFrisbeeHandle] = MapEntryWord(0, kFrisbeeGen);
        Put32(frisbee.elements, lpf_early::kFrisbeeCiPhysIdOff, physId);

        discPhys.WirePhys();
        Put16(discPhys.bytes, lpf_early::kPhysCsIdCapacityOff, 8);
        Put16(discPhys.bytes, lpf_early::kPhysCsLiveCountOff, 4);
        Put16(discPhys.bytes, facts::PHYS_CS_COUNT_OFF, 4);
        discPhys.handleMap[uint16_t(physId)] =
            MapEntryWord(uint16_t(physId), uint16_t(physId >> 16));
        discPhys.reverseMap[uint16_t(physId)] = uint16_t(physId);
    }

    // The engine reaching round start: sub_140172AD0 fills the ref with what it resolved.
    void ResolveRulesRef(uint32_t physId) {
        PutPtr(Ref(), lpf_early::kCompRefSpaceOff, refSpace);
        memcpy(refSpace + lpf_early::kGameSpaceEpochOff, &epoch, sizeof epoch);
        memcpy(Ref() + lpf_early::kCompRefStampOff, &epoch, sizeof epoch);
        PutPtr(Ref(), lpf_early::kCompRefCsOff, frisbee.bytes);
        Put32(Ref(), lpf_early::kCompRefIdOff, FrisbeeCid());
        Put32(frisbee.elements, lpf_early::kFrisbeeCiPhysIdOff, physId);
    }
};

}  // namespace

TEST_CASE("DISC-EARLY-BIND: with no rules ref, the disc is resolved out of the level space") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    FakeCs phys, player;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());
    using Bind = replay::IEntity::DiscBind;
    using Chain = replay::IEntity::DiscChainReading;
    const uint32_t kPhysId = 0x00000002u;

    SUBCASE("the early path binds the same {physCs, physId} the rules path would") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        CHECK(b.refused == nullptr);
        CHECK(b.early);
        CHECK(b.earlyRefused == nullptr);
        CHECK(b.earlyActorIndex == EarlyChain::kActorIndex);
        CHECK(b.frisbeeCs == c.frisbee.bytes);
        CHECK(b.frisbeeCid == c.FrisbeeCid());
        CHECK(b.physCs == c.discPhys.bytes);
        CHECK(b.physId == kPhysId);
        CHECK(b.playerPhysCs == phys.bytes);
        CHECK(b.physCs != b.playerPhysCs);
        CHECK(b.handle.v == uint16_t(kPhysId));
        CHECK(b.handle.pool == replay::PhysHandle::kPoolDisc);
        CHECK(entity->DiscBoundEarly());
        CHECK(entity->DiscEarlyRefusal() == nullptr);
        CHECK(entity->DiscStands(kPhysId));
        CHECK(entity->DiscChainStands(c.netGame, nullptr));

        // The drive tick is unchanged by any of this: the actor walk is bind-time work only.
        replay::entity::BindEpochAdvance();
        REQUIRE(entity->DiscStands(kPhysId));
        const uint64_t warm0 = replay::ReadableProbeCount();
        for (int tick = 0; tick < 240; ++tick) REQUIRE(entity->DiscStands(kPhysId));
        CHECK(replay::ReadableProbeCount() - warm0 == 0u);
        entity->DropDisc();
    }

    SUBCASE("the disc-ring read reaches timesincelastheld and the touch CI its own handle names") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));

        // The two numbers CR15FrisbeeCS::Update owns, planted where it reads them, plus the touch
        // interact system sub_14075FD80 would ask: its handle map and one CI with a holder set.
        const float kHeldFor = 4.25f;
        memcpy(c.frisbee.elements + lpf::kFrisbeeCiTimeSinceHeldOff, &kHeldFor, sizeof kHeldFor);
        const uint16_t kTouchHandle = 3, kTouchIndex = 2;
        const uint32_t touchId = kTouchHandle;
        const uint16_t one = 1;
        memcpy(c.frisbee.elements + lpf::kFrisbeeCiTouchIdOff, &touchId, sizeof touchId);
        memcpy(c.frisbee.bytes + lpf::kFrisbeeCsCountOff, &one, sizeof one);

        static FakeCs touch;
        static uint8_t touchCis[4 * lpf::kTouchCiStride];
        static uint32_t touchMap[8];
        memset(touchCis, 0, sizeof touchCis);
        memset(touchMap, 0, sizeof touchMap);
        touchMap[kTouchHandle] = kTouchIndex;
        uint8_t* const ci = touchCis + lpf::kTouchCiStride * kTouchIndex;
        const uint32_t holderB = 0x00000011u;
        const uint16_t flags = lpf::kTouchCiAskParentBit;
        memcpy(ci + lpf::kTouchCiHolderBOff, &holderB, sizeof holderB);
        memcpy(ci + lpf::kTouchCiFlagsOff, &flags, sizeof flags);
        touch.PutPtr(lpf::kTouchCsMapOff, touchMap);
        touch.PutPtr(lpf::kTouchCsPoolBaseOff, touchCis);
        c.frisbee.PutPtr(lpf::kFrisbeeCsTouchCsOff, touch.bytes);

        replay::IEntity::DiscRing ring;
        REQUIRE(entity->ReadDiscRing(ring));
        CHECK(ring.refused == nullptr);
        CHECK(ring.timeSinceLastHeld == doctest::Approx(kHeldFor));
        CHECK(ring.enabled == 1);
        CHECK(ring.touchHandle == kTouchHandle);
        CHECK(ring.touchRead);
        CHECK(ring.holderA == 0u);
        CHECK(ring.holderB == 0x00000011u);
        CHECK(ring.askParent);
        // Either holder non-zero is what sub_14075FD80 answers HELD on, not both.
        CHECK(ring.heldByOwnCi);

        // A touch system the frisbee does not name is a short read, never a fault and never a
        // fabricated zero: the timer still comes back, the holders say they were not read.
        c.frisbee.PutPtr(lpf::kFrisbeeCsTouchCsOff, nullptr);
        replay::IEntity::DiscRing partial;
        REQUIRE(entity->ReadDiscRing(partial));
        CHECK(partial.refused == nullptr);
        CHECK(partial.timeSinceLastHeld == doctest::Approx(kHeldFor));
        CHECK_FALSE(partial.touchRead);
        CHECK_FALSE(partial.heldByOwnCi);
        entity->DropDisc();

        // With no disc bound at all the read refuses by name rather than answering zero.
        replay::IEntity::DiscRing unbound;
        CHECK_FALSE(entity->ReadDiscRing(unbound));
        CHECK(unbound.refused != nullptr);
        CHECK(unbound.timeSinceLastHeld == 0.f);
    }

    SUBCASE("a resolved rules ref still wins: the early path is the fallback, not the default") {
        EarlyChain c;
        c.Build(kPhysId);
        c.ResolveRulesRef(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        CHECK_FALSE(b.early);
        CHECK(b.frisbeeCid == c.FrisbeeCid());
        CHECK(b.physId == kPhysId);
        CHECK_FALSE(entity->DiscBoundEarly());
        entity->DropDisc();
    }

    SUBCASE("every hop refuses by name, one hop at a time") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;

        EarlyChain::PutPtr(c.netGame, lpf::kNetGameLevelSpaceOff, nullptr);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.refused).find("unresolved") != std::string::npos);
        CHECK(std::string(b.earlyRefused).find("level game space does not exist") !=
              std::string::npos);
        EarlyChain::PutPtr(c.netGame, lpf::kNetGameLevelSpaceOff, c.level);
        REQUIRE(entity->BindDisc(c.netGame, b));

        // No registry on either space in the walk.
        EarlyChain::PutPtr(c.level, lpf::kGameSpaceActorRegOff, nullptr);
        EarlyChain::PutPtr(c.parent, lpf::kGameSpaceActorRegOff, nullptr);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("no live game_frisbee actor") != std::string::npos);
        EarlyChain::PutPtr(c.level, lpf::kGameSpaceActorRegOff, c.registry);
        EarlyChain::PutPtr(c.parent, lpf::kGameSpaceActorRegOff, c.parentRegistry);
        REQUIRE(entity->BindDisc(c.netGame, b));

        // The actor is in the table but its slot is not live -- the engine's own 0xFFFF test.
        c.slotWords[EarlyChain::kActorIndex] = 0;
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("no live game_frisbee actor") != std::string::npos);
        c.slotWords[EarlyChain::kActorIndex] = lpf::kActorSlotLive;
        REQUIRE(entity->BindDisc(c.netGame, b));

        // The system table's own unsorted bit used to refuse this lookup, on
        // the premise that searching the table would make the ENGINE sort it in place. It does not:
        // the lookup walks the rows itself. The bit costs it its bisection and nothing else, so the
        // system is still found -- which is the whole defect, because the DISC's game space carries
        // that bit and this lookup answered null for it on every sample of every launch.
        EarlyChain::Put64(c.level + lpf::kGameSpaceSystemTableOff, 0x38, lpf::kSystemTableUnsorted);
        REQUIRE(entity->BindDisc(c.netGame, b));
        EarlyChain::Put64(c.level + lpf::kGameSpaceSystemTableOff, 0x38, 0);

        // A table that does not hold the symbol at all.
        c.csEntries[1][0] = EarlyChain::kLowKey + 2u;
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("registers no R14Frisbee") != std::string::npos);
        c.csEntries[1][0] = lpf::kSymR14Frisbee;
        REQUIRE(entity->BindDisc(c.netGame, b));

        EarlyChain::Put16(c.frisbee.bytes, lpf::kPhysCsLiveCountOff, 0);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("no live component") != std::string::npos);
        EarlyChain::Put16(c.frisbee.bytes, lpf::kPhysCsLiveCountOff, 2);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("more than one live component") !=
              std::string::npos);
        EarlyChain::Put16(c.frisbee.bytes, lpf::kPhysCsLiveCountOff, 1);

        c.frisbee.PutPtr(lpf::kFrisbeeCsPhysCsOff, nullptr);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("caches no net-physics") != std::string::npos);
        CHECK_FALSE(b.physPoolRefused);
        c.frisbee.PutPtr(lpf::kFrisbeeCsPhysCsOff, c.discPhys.bytes);

        EarlyChain::Put16(c.discPhys.bytes, lpf::kPhysCsLiveCountOff, 0);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(b.physPoolRefused);
        CHECK(std::string(b.earlyRefused).find("does not vouch for the disc body") !=
              std::string::npos);
        EarlyChain::Put16(c.discPhys.bytes, lpf::kPhysCsLiveCountOff, 4);

        CHECK_FALSE(entity->BindDisc(nullptr, b));
        CHECK(std::string(b.earlyRefused).find("no netGame") != std::string::npos);
        entity->DropDisc();
    }

    SUBCASE("the actor is looked for in the parent space too, and its components there") {
        EarlyChain c;
        c.Build(kPhysId);
        // The level space's own registry no longer names it; the parent's does.
        c.actorEntries[1][0] = EarlyChain::kLowKey + 2u;
        c.parentActorEntries[1][0] = lpf::kSymGameFrisbee;
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        CHECK(b.early);
        CHECK(b.frisbeeCs == c.frisbee.bytes);
        // ...and the system is taken from the space the ACTOR was found in: emptying the parent's
        // table refuses even though the level space's table still holds the symbol.
        EarlyChain::Put64(c.parent + lpf::kGameSpaceSystemTableOff, 0x30, 0);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("registers no R14Frisbee") != std::string::npos);
        entity->DropDisc();
    }

    SUBCASE("an early-bound disc is re-validated against the chain that bound it") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        Chain seen;
        REQUIRE(entity->DiscChainStands(c.netGame, &seen));
        CHECK(seen.early);
        CHECK_FALSE(seen.refResolved);

        // The actor going away is what the re-walk exists to see: the per-tick predicate cannot.
        c.slotWords[EarlyChain::kActorIndex] = 0;
        CHECK(entity->DiscStands(kPhysId));
        CHECK_FALSE(entity->DiscChainStands(c.netGame, &seen));
        c.slotWords[EarlyChain::kActorIndex] = lpf::kActorSlotLive;
        CHECK(entity->DiscChainStands(c.netGame, &seen));

        // A level rebuilt under us: a different space in the slot is a drop even when it parents
        // onto the old one, so every hop below it still answers exactly as it did.
        EarlyChain::PutPtr(c.altLevel, lpf::kGameSpaceParentOff, c.level);
        EarlyChain::PutPtr(c.netGame, lpf::kNetGameLevelSpaceOff, c.altLevel);
        CHECK_FALSE(entity->DiscChainStands(c.netGame, &seen));
        EarlyChain::PutPtr(c.netGame, lpf::kNetGameLevelSpaceOff, c.level);
        CHECK(entity->DiscChainStands(c.netGame, &seen));

        // ...and so is the frisbee system caching another net-physics system.
        FakeCs rebuilt;
        rebuilt.WirePhys();
        c.frisbee.PutPtr(lpf::kFrisbeeCsPhysCsOff, rebuilt.bytes);
        CHECK_FALSE(entity->DiscChainStands(c.netGame, &seen));
        c.frisbee.PutPtr(lpf::kFrisbeeCsPhysCsOff, c.discPhys.bytes);
        entity->DropDisc();
        CHECK_FALSE(entity->DiscChainStands(c.netGame, &seen));
    }

    SUBCASE("when the host reaches round start the two paths agree, and the ref takes over") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        REQUIRE(b.early);

        c.ResolveRulesRef(kPhysId);
        Chain seen;
        REQUIRE(entity->DiscChainStands(c.netGame, &seen));
        CHECK(seen.early);
        CHECK(seen.refResolved);
        CHECK(seen.agreed);
        CHECK(seen.refPhysCs == c.discPhys.bytes);
        CHECK(seen.refPhysId == kPhysId);
        // Handed over without a drop: the next re-validation is the rules chain's, and the disc
        // never stopped being driven.
        CHECK_FALSE(entity->DiscBoundEarly());
        REQUIRE(entity->DiscChainStands(c.netGame, &seen));
        CHECK_FALSE(seen.early);
        CHECK(entity->DiscStands(kPhysId));
        entity->DropDisc();
    }

    SUBCASE("a rules ref that FLAPS does not drop a good early-bound disc") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        REQUIRE(b.early);

        // LAUNCH-FIX3: the chain alternated between answers 425 times at 2 Hz while the host
        // idled. One disagreeing sample followed by an unresolvable one is that, not a respawn.
        const uint32_t kOther = 0x00090003u;
        const uint16_t otherHandle = uint16_t(kOther & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
        c.discPhys.handleMap[otherHandle] = MapEntryWord(otherHandle, uint16_t(kOther >> 16));
        c.discPhys.reverseMap[otherHandle] = otherHandle;
        Chain seen;
        for (int flap = 0; flap < 8; ++flap) {
            c.ResolveRulesRef(kOther);
            CHECK(entity->DiscChainStands(c.netGame, &seen));
            CHECK(seen.disagreeStreak == 1);
            EarlyChain::PutPtr(c.Ref(), lpf::kCompRefSpaceOff, nullptr);
            EarlyChain::Put32(c.frisbee.elements + lpf::kFrisbeeCiPhysIdOff, 0, kPhysId);
            CHECK(entity->DiscChainStands(c.netGame, &seen));
            CHECK(seen.disagreeStreak == 0);
        }
        CHECK(entity->DiscBoundEarly());

        // Two disagreeing samples in a row IS the engine having moved the disc.
        c.ResolveRulesRef(kOther);
        CHECK(entity->DiscChainStands(c.netGame, &seen));
        CHECK_FALSE(entity->DiscChainStands(c.netGame, &seen));
        CHECK(seen.disagreeStreak == 2);
        entity->DropDisc();
    }

    SUBCASE("a round start that RESPAWNS the disc disagrees, and the re-bind takes the ref") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        REQUIRE(b.early);

        // The host re-creates the disc: a new net-physics component id on the same actor.
        const uint32_t kRespawned = 0x00090003u;
        const uint16_t respawnedHandle = uint16_t(kRespawned & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
        const uint16_t respawnedGen = uint16_t(kRespawned >> 16);
        c.discPhys.handleMap[respawnedHandle] = MapEntryWord(respawnedHandle, respawnedGen);
        c.discPhys.reverseMap[respawnedHandle] = respawnedHandle;
        c.ResolveRulesRef(kRespawned);

        Chain seen;
        // Confirmed, not guessed: the first disagreeing sample keeps driving, and only the
        // second one in a row drops -- a ref that flaps cannot cost a good disc its claim.
        CHECK(entity->DiscChainStands(c.netGame, &seen));
        CHECK_FALSE(entity->DiscChainStands(c.netGame, &seen));
        CHECK(seen.early);
        CHECK(seen.refResolved);
        CHECK_FALSE(seen.agreed);
        CHECK(seen.refPhysId == kRespawned);

        // Which is a drop and a re-bind, and the re-bind comes back on the ENGINE's answer.
        entity->DropDisc();
        Bind again;
        REQUIRE(entity->BindDisc(c.netGame, again));
        CHECK_FALSE(again.early);
        CHECK(again.physId == kRespawned);
        CHECK(entity->DiscStands(kRespawned));
        entity->DropDisc();
    }

    // ---- DISC-EARLY-2 -------------------------------------------------------------------------

    SUBCASE("the slot word is read THROUGH registry+0x160, never inline") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        entity->DropDisc();

        // The reading LAUNCH-DISC-EARLY refuted: the live word written inline where the pointer
        // belongs. With the pointed-to array cleared, an inline reader still binds and this one
        // must not.
        c.slotWords[EarlyChain::kActorIndex] = 0;
        EarlyChain::Put16(c.registry,
                          lpf::kActorRegSlotWordsPtrOff + 2u * EarlyChain::kActorIndex,
                          lpf::kActorSlotLive);
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("no live game_frisbee actor") != std::string::npos);
        EarlyChain::PutPtr(c.registry, lpf::kActorRegSlotWordsPtrOff, c.slotWords);
        c.slotWords[EarlyChain::kActorIndex] = lpf::kActorSlotLive;
        REQUIRE(entity->BindDisc(c.netGame, b));
        entity->DropDisc();
    }

    SUBCASE("an actor only the rules-owner candidate reaches still binds, and names itself") {
        EarlyChain c;
        c.Build(kPhysId);
        // *(ng+0x2B10) empty; the engine's own expression -- *(*(ng+0x2AA8)) + 0x2B10 -- reaching
        // a netGame whose level space does hold the actor.
        EarlyChain::PutPtr(c.netGame, lpf::kNetGameLevelSpaceOff, nullptr);
        alignas(16) uint8_t owner[lpf::kNetGameLevelSpaceOff + 0x20] = {};
        EarlyChain::PutPtr(owner, lpf::kNetGameLevelSpaceOff, c.level);
        EarlyChain::PutPtr(c.rules, lpf::kRulesOwnerNetGameOff, owner);

        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        CHECK(b.early);
        CHECK(b.earlySpace == 1);
        CHECK(b.earlyRoot == c.level);
        CHECK(std::string(entity->DiscEarlySpaceName()) == "*(*(ng+0x2AA8))+0x2B10");
        // And the re-walk follows the candidate it bound on, not the level-space slot. altLevel
        // PARENTS onto the space the bind resolved in, so every hop below the root still answers
        // exactly as it did and only the root's identity can refuse it.
        EarlyChain::PutPtr(c.altLevel, lpf::kGameSpaceParentOff, c.level);
        Chain seen;
        CHECK(entity->DiscChainStands(c.netGame, &seen));
        EarlyChain::PutPtr(owner, lpf::kNetGameLevelSpaceOff, c.altLevel);
        CHECK_FALSE(entity->DiscChainStands(c.netGame, &seen));
        EarlyChain::PutPtr(owner, lpf::kNetGameLevelSpaceOff, c.level);
        CHECK(entity->DiscChainStands(c.netGame, &seen));
        entity->DropDisc();
    }

    SUBCASE("the rules owner being the netGame itself is reported as an alias, not a second space") {
        EarlyChain c;
        c.Build(kPhysId);
        EarlyChain::PutPtr(c.rules, lpf::kRulesOwnerNetGameOff, c.netGame);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        CHECK(b.earlySpace == 0);
        const auto* d = entity->DiscEarlyDiagnostics();
        REQUIRE(d != nullptr);
        CHECK(d->cand[0].root == c.level);
        CHECK(d->cand[0].aliasOf == -1);
        CHECK(d->cand[1].root == c.level);
        CHECK(d->cand[1].aliasOf == 0);
        entity->DropDisc();
    }

    SUBCASE("no actor anywhere: the refusal carries every candidate and the registry head") {
        EarlyChain c;
        c.Build(kPhysId);
        c.actorEntries[1][0] = EarlyChain::kLowKey + 2u;   // a registry without game_frisbee
        Bind b;
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        CHECK(std::string(b.earlyRefused).find("no live game_frisbee actor") != std::string::npos);

        const auto* d = entity->DiscEarlyDiagnostics();
        REQUIRE(d != nullptr);
        CHECK(d->n == replay::IEntity::DiscBind::EarlyDiag::kCandidates);
        CHECK(std::string(d->cand[0].name) == "ng+0x2B10");
        CHECK(d->cand[0].root == c.level);
        CHECK(d->cand[0].registryRead);
        CHECK(d->cand[0].count == 2);
        CHECK(d->cand[0].depth == 2);            // the level space and its parent
        CHECK_FALSE(d->cand[0].symbolMatched);
        CHECK_FALSE(d->cand[0].slotLive);
        CHECK(d->cand[1].root == nullptr);       // no rules owner wired
        CHECK(d->cand[2].root == nullptr);       // no CncaGameSpace wired
        // The registry head, so a launch can see the symbols that ARE there.
        CHECK(d->sampleFrom == c.level);
        CHECK(d->sampleN == 2);
        CHECK(d->sample[0] == EarlyChain::kLowKey);
        CHECK(d->sample[1] == EarlyChain::kLowKey + 2u);

        // The symbol being present but the slot dead is a DIFFERENT report, and the log has to
        // separate them: this is the "right space, wrong liveness" case.
        c.actorEntries[1][0] = lpf::kSymGameFrisbee;
        c.slotWords[EarlyChain::kActorIndex] = 0;
        CHECK_FALSE(entity->BindDisc(c.netGame, b));
        const auto* d2 = entity->DiscEarlyDiagnostics();
        REQUIRE(d2 != nullptr);
        CHECK(d2->cand[0].symbolMatched);
        CHECK(d2->cand[0].index == EarlyChain::kActorIndex);
        CHECK_FALSE(d2->cand[0].slotLive);
    }

    SUBCASE("a resolved rules ref that agrees does not drop the early bind") {
        EarlyChain c;
        c.Build(kPhysId);
        Bind b;
        REQUIRE(entity->BindDisc(c.netGame, b));
        REQUIRE(b.early);
        c.ResolveRulesRef(kPhysId);
        Chain seen;
        // The hand-over is reported exactly once -- the module logs off `early && agreed`, and a
        // second report would be a second line for one event.
        REQUIRE(entity->DiscChainStands(c.netGame, &seen));
        CHECK(seen.early);
        CHECK(seen.refResolved);
        CHECK(seen.agreed);
        CHECK(seen.disagreeStreak == 0);
        for (int i = 0; i < 5; ++i) {
            CHECK(entity->DiscChainStands(c.netGame, &seen));
            CHECK_FALSE(seen.early);
            CHECK(seen.disagreeStreak == 0);
        }
        CHECK(entity->DiscStands(kPhysId));
        entity->DropDisc();
    }

    g_physCs = nullptr;
    g_playerCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// ---- P1C SCRIPT-SELF ----------------------------------------------------------------------------
//
// The chain sub_140446220 walks, planted: the space's system table -> the ncaScript system -> its
// actor index -> a component instance -> the script instance -> the module that carries the script
// hash and the `get` binding -> the graph embedded at inst+24. Every link is a fact this packet
// added, so the rig is the only thing between them and a launch.

namespace {

namespace lpf_script = replay::localpawn_facts;

// Which planted graphs the stand-in getter recognises `self` on, and what it answers. Keyed on the
// graph pointer because that is exactly what the product passes: a rig that answered for every
// graph could not fail when the walk handed the getter the wrong one.
struct ScriptSelfAnswer {
    const void* graph = nullptr;
    uint64_t    space = 0, actor = 0, node = 0;
};
ScriptSelfAnswer g_scriptAnswers[4];
uint32_t         g_scriptGetCalls = 0;

int FakeScriptGet(void* graph, uint64_t name, uint64_t type, void* out) {
    ++g_scriptGetCalls;
    // The shipped script_get_variable switches on the type first and writes nothing when either
    // hash is unknown, which is the whole reason asking every graph is safe. The stand-in has to
    // model that, not merely the recognised case.
    if (type != replay::gametype::SymbolHash("actor")) return 0;
    if (name != replay::gametype::SymbolHash("self")) return 0;
    for (const ScriptSelfAnswer& a : g_scriptAnswers) {
        if (a.graph != graph) continue;
        uint8_t var[lpf_script::kActorVarSize] = {};
        memcpy(var + lpf_script::kActorVarSpaceOff, &a.space, sizeof a.space);
        memcpy(var + lpf_script::kActorVarActorOff, &a.actor, sizeof a.actor);
        memcpy(var + lpf_script::kActorVarNodeOff, &a.node, sizeof a.node);
        memcpy(out, var, sizeof var);
        return 1;
    }
    return 0;
}

constexpr uint64_t kScriptFoley = 0xF39961BE341AABB7ull;
constexpr uint64_t kScriptOther = 0x0123456789ABCDEFull;

struct ScriptRig {
    static constexpr uint32_t kInstances = 4;
    uint64_t sysEntries[2][2] = {};
    alignas(16) uint8_t cs[0x140] = {};  // facts-ok: PHYS.CI_GATE_OFF the byte size of this fixture's stand-in component storage, not the physics gate offset
    uint64_t actorIndex[kInstances][3] = {};
    alignas(16) uint8_t ciPool[kInstances * lpf_script::kScriptCiStride] = {};
    alignas(16) uint8_t inst[kInstances][0x100] = {};
    alignas(16) uint8_t module[kInstances][0x220] = {};   // facts-ok: PLAYER.CS_LOD_CURSOR_OFF a fake module's size, past the sendevent binding at +512
    alignas(16) uint8_t graphData[kInstances][0x80] = {};

    static void PutPtr(uint8_t* p, uint32_t off, const void* v) { memcpy(p + off, &v, sizeof v); }
    static void Put64(uint8_t* p, uint32_t off, uint64_t v) { memcpy(p + off, &v, sizeof v); }

    uint8_t* Graph(uint32_t i) { return inst[i] + lpf_script::kScriptInstGraphOff; }

    // `hash` is the script the module names, `state` what the instance last reached, `wireData`
    // whether the graph has been initialised at all -- a null EXECUTION_DATA is the case the
    // product must refuse to call the getter on.
    void WireInstance(uint32_t i, uint64_t guidLo, uint16_t ciIndex, uint64_t hash, int32_t state,
                      bool wireData) {
        actorIndex[i][0] = guidLo;
        actorIndex[i][1] = 0;
        actorIndex[i][2] = ciIndex;
        if (ciIndex != lpf_script::kNoComponent) {
            uint8_t* const ci = ciPool + lpf_script::kScriptCiStride * ciIndex;
            PutPtr(ci, lpf_script::kScriptCiInstanceOff, inst[i]);
        }
        PutPtr(inst[i], lpf_script::kScriptInstModuleOff, module[i]);
        memcpy(inst[i] + lpf_script::kScriptInstStateOff, &state, sizeof state);
        Put64(module[i], lpf_script::kScriptModuleHashOff, hash);
        PutPtr(module[i], lpf_script::kScriptModuleGetOff,
               reinterpret_cast<const void*>(&FakeScriptGet));
        PutPtr(Graph(i), lpf_script::kScriptGraphDataOff, wireData ? graphData[i] : nullptr);
        // The sentinel block and the clock the threads that clear it run on: distinct per
        // instance, so a read that returned the wrong graph's block would be visible.
        Put64(Graph(i), lpf_script::kScriptGraphGameTimeOff, 0x1000ull + i);
        for (uint32_t w = 0; w < replay::IEntity::ScriptSelfRow::kDataWords; ++w) {
            const uint32_t word = 0xD0000000u | (i << 16) | w;
            memcpy(graphData[i] + 4 * w, &word, sizeof word);
        }
    }

    void Build(FakeSpace& space) {
        sysEntries[0][0] = replay::gametype::SymbolHash("ncaTransform");
        sysEntries[0][1] = 0;
        sysEntries[1][0] = replay::gametype::SymbolHash("ncaScript");
        sysEntries[1][1] = reinterpret_cast<uintptr_t>(cs);
        space.Construct(reinterpret_cast<const uint8_t*>(sysEntries), 2);
        PutPtr(cs, lpf_script::kCsActorIndexOff, actorIndex);
        Put64(cs, lpf_script::kCsActorIndexCountOff, kInstances);
        PutPtr(cs, lpf_script::kScriptCsPoolBaseOff, ciPool);
    }
};

}  // namespace

TEST_CASE("SCRIPT-SELF: every ncaScript graph is asked for self, and the walk names what it saw") {
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    FakeCs phys, player;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;

    auto rig = std::unique_ptr<ScriptRig>(new ScriptRig());
    rig->Build(space);
    // 0: the foley script, initialised, and the only one whose `self` is bound.
    // 1: a different script on the same actor -- present, answers nothing, must not be a row.
    // 2: the foley script on a second actor whose graph never initialised: a detail row, unanswered.
    // 3: an actor-index row whose component id is the absent sentinel.
    rig->WireInstance(0, 0x000000000001003Bull, 0, kScriptFoley, 5, true);
    rig->WireInstance(1, 0x000000000001003Bull, 1, kScriptOther, 5, true);
    rig->WireInstance(2, 0x000000000002003Bull, 2, kScriptFoley, 2, false);
    rig->WireInstance(3, 0x000000000003003Bull, lpf_script::kNoComponent, kScriptOther, 5, true);

    for (ScriptSelfAnswer& a : g_scriptAnswers) a = ScriptSelfAnswer();
    g_scriptAnswers[0] = {rig->Graph(0), 0x3F9915D3001DC28Eull, 0x000000000001003Bull,
                          0x999D680B6B01B67Eull};
    // Planted on the graph of the instance whose EXECUTION_DATA is null: if the product asked
    // anyway, this is the answer that would come back, and the case below would fail on it.
    g_scriptAnswers[1] = {rig->Graph(2), 0xDEADDEADDEADDEADull, 0xDEADDEADDEADDEADull,
                          0xDEADDEADDEADDEADull};
    g_scriptGetCalls = 0;

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());

    const uint64_t wanted[1] = {kScriptFoley};
    replay::IEntity::ScriptSelfRow rows[8];
    replay::IEntity::ScriptHashRow hashes[8];
    replay::IEntity::ScriptCensus io;
    io.wanted = wanted;
    io.nWanted = 1;
    io.rows = rows;
    io.rowCap = 8;
    io.hashes = hashes;
    io.hashCap = 8;

    SUBCASE("the walk reaches every instance and reports self only where the graph answered") {
        REQUIRE(entity->ScriptSelfCensus(io));
        CHECK(io.refused == nullptr);
        CHECK(io.walked == ScriptRig::kInstances);
        CHECK(io.unreadable == 1);          // the sentinel component id, and only it
        // The getter is asked once per readable instance whose graph has data -- three of the
        // four -- and never for the one whose EXECUTION_DATA is null.
        CHECK(g_scriptGetCalls == 2);

        REQUIRE(io.hashesWritten == 2);
        for (uint32_t i = 0; i < io.hashesWritten; ++i) {
            if (hashes[i].hash == kScriptFoley) {
                CHECK(hashes[i].instances == 2);
                CHECK(hashes[i].withSelf == 1);
            } else {
                CHECK(hashes[i].hash == kScriptOther);
                CHECK(hashes[i].instances == 1);   // the sentinel row never reaches a module
                CHECK(hashes[i].withSelf == 0);
            }
        }

        REQUIRE(io.rowsWritten == 2);
        CHECK(rows[0].scriptHash == kScriptFoley);
        CHECK(rows[0].actorLo == 0x000000000001003Bull);
        CHECK(rows[0].ciIndex == 0);
        CHECK(rows[0].state == 5);
        CHECK(rows[0].answered);
        CHECK(rows[0].selfSpace == 0x3F9915D3001DC28Eull);
        CHECK(rows[0].selfActor == 0x000000000001003Bull);
        CHECK(rows[0].selfNode == 0x999D680B6B01B67Eull);
        CHECK(rows[0].graphTime == 0x1000);
        REQUIRE(rows[0].dataRead);
        CHECK(rows[0].data[0] == 0xD0000000u);
        CHECK(rows[0].data[replay::IEntity::ScriptSelfRow::kDataWords - 1] ==
              (0xD0000000u | (replay::IEntity::ScriptSelfRow::kDataWords - 1)));
        // The uninitialised graph: present, named, and NOT carrying the answer planted on it.
        CHECK(rows[1].scriptHash == kScriptFoley);
        CHECK(rows[1].actorLo == 0x000000000002003Bull);
        CHECK(rows[1].state == 2);
        CHECK_FALSE(rows[1].answered);
        CHECK(rows[1].selfActor == 0);
        // The uninitialised graph has no EXECUTION_DATA, so there is nothing to dump -- but its
        // clock is still on the graph and still readable.
        CHECK_FALSE(rows[1].dataRead);
        CHECK(rows[1].graphTime == 0x1002);
    }

    SUBCASE("a null get binding is a short row, never a call through a null pointer") {
        ScriptRig::PutPtr(rig->module[0], lpf_script::kScriptModuleGetOff, nullptr);
        REQUIRE(entity->ScriptSelfCensus(io));
        CHECK(io.walked == ScriptRig::kInstances);
        CHECK(g_scriptGetCalls == 1);
        REQUIRE(io.rowsWritten == 2);
        CHECK_FALSE(rows[0].answered);
        CHECK(rows[0].scriptHash == kScriptFoley);   // still identified, just not asked
    }

    SUBCASE("a level with no ncaScript system refuses by name") {
        rig->sysEntries[1][0] = replay::gametype::SymbolHash("ncaTransform");
        CHECK_FALSE(entity->ScriptSelfCensus(io));
        REQUIRE(io.refused != nullptr);
        CHECK(strstr(io.refused, "ncaScript") != nullptr);
        CHECK(io.walked == 0);
    }

    SUBCASE("an unreadable component-instance pool refuses before it is indexed") {
        ScriptRig::PutPtr(rig->cs, lpf_script::kScriptCsPoolBaseOff, nullptr);
        CHECK_FALSE(entity->ScriptSelfCensus(io));
        REQUIRE(io.refused != nullptr);
        CHECK(io.rowsWritten == 0);
    }

    for (ScriptSelfAnswer& a : g_scriptAnswers) a = ScriptSelfAnswer();
    g_physCs = nullptr;
    g_playerCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}

// ---- P1D AUDIO-MIX -------------------------------------------------------------------------
//
// The listener table csoundplayer.cpp hands to AK::SoundEngine::SetDefaultListeners, planted. The
// product's two global links cannot be planted in a test host, so the walk below them is what is
// exercised -- and it is where every field offset, both strides and the two bounds live.

namespace {

namespace lpf_audio = replay::localpawn_facts;

// A sound player big enough for both tables and the fields between them.
struct FakeSoundPlayer {
    uint8_t bytes[1024] = {};
    uint8_t listeners[lpf_audio::kAudioListenerStride * 8] = {};
    uint8_t views[lpf_audio::kAudioViewStride * 8] = {};

    template <typename T>
    void Put(uint32_t off, const T& v) { memcpy(bytes + off, &v, sizeof v); }

    void Wire(uint64_t listenerCount, uint64_t viewCount) {
        void* const l = listeners;
        void* const v = views;
        Put(lpf_audio::kAudioListenerBaseOff, l);
        Put(lpf_audio::kAudioListenerCountOff, listenerCount);
        Put(lpf_audio::kAudioViewBaseOff, v);
        Put(lpf_audio::kAudioViewCountOff, viewCount);
        Put<uint32_t>(lpf_audio::kAudioSpatializedOff, 6u);
        Put<uint32_t>(lpf_audio::kAudioActiveViewOff, 11u);
    }
    void Listener(uint32_t i, uint32_t id, float x, float y, float z, float qw) {
        uint8_t* const rec = listeners + lpf_audio::kAudioListenerStride * i;
        memcpy(rec, &id, sizeof id);
        const float quat[4] = {0.0f, 0.0f, 0.0f, qw};
        const float pos[3] = {x, y, z};
        memcpy(rec + lpf_audio::kAudioListenerQuatOff, quat, sizeof quat);
        memcpy(rec + lpf_audio::kAudioListenerPosOff, pos, sizeof pos);
    }
    void View(uint32_t i, uint32_t id, float x, float y, float z) {
        uint8_t* const rec = views + lpf_audio::kAudioViewStride * i;
        memcpy(rec, &id, sizeof id);
        const float pos[3] = {x, y, z};
        memcpy(rec + lpf_audio::kAudioViewPosOff, pos, sizeof pos);
    }

    static constexpr uint32_t kEmitterSlots = 6;
    uint8_t emitterMap[lpf_audio::kAudioEmitterMapStride * kEmitterSlots] = {};
    uint8_t emitterRecs[lpf_audio::kAudioEmitterRecSpan * kEmitterSlots] = {};

    void WireEmitters(uint64_t count) {
        void* const m = emitterMap;
        Put(lpf_audio::kAudioEmitterMapOff, m);
        Put(lpf_audio::kAudioEmitterMapCountOff, count);
    }
    // `live` false leaves the map entry pointing nowhere, which is the freed-record case the walk
    // has to skip without faulting and without consuming a row.
    void Emitter(uint32_t i, uint32_t id, uint32_t flags, float x, float y, float z,
                 bool live = true) {
        uint8_t* const slot = emitterMap + lpf_audio::kAudioEmitterMapStride * i;
        memcpy(slot, &id, sizeof id);
        uint8_t* const rec = emitterRecs + lpf_audio::kAudioEmitterRecSpan * i;
        void* const p = live ? rec : nullptr;
        memcpy(slot + lpf_audio::kAudioEmitterMapRecOff, &p, sizeof p);
        memcpy(rec + lpf_audio::kAudioEmitterIdOff, &id, sizeof id);
        memcpy(rec + lpf_audio::kAudioEmitterFlagsOff, &flags, sizeof flags);
        const float pos[3] = {x, y, z};
        memcpy(rec + lpf_audio::kAudioEmitterPosOff, pos, sizeof pos);
    }
};

}  // namespace

TEST_CASE("AUDIO-MIX: the listener table is read as csoundplayer.cpp writes it") {
    SUBCASE("two listeners are two rows, each at its own place") {
        FakeSoundPlayer sp;
        sp.Wire(2, 1);
        sp.Listener(0, 4, 1.5f, 2.5f, -3.5f, 1.0f);
        sp.Listener(1, 5, -20.0f, 0.25f, 40.0f, 0.5f);
        sp.View(0, 11, 1.5f, 2.5f, -3.5f);

        replay::IEntity::AudioMix mix;
        REQUIRE(replay::IEntity::AudioMixFrom(sp.bytes, mix));
        CHECK(mix.refused == nullptr);
        CHECK(mix.listenerCount == 2u);
        CHECK(mix.listenersWritten == 2u);
        CHECK(mix.listeners[0].id == 4u);
        CHECK(mix.listeners[0].pos[0] == doctest::Approx(1.5f));
        CHECK(mix.listeners[0].pos[2] == doctest::Approx(-3.5f));
        CHECK(mix.listeners[0].quat[3] == doctest::Approx(1.0f));
        CHECK(mix.listeners[1].id == 5u);
        CHECK(mix.listeners[1].pos[2] == doctest::Approx(40.0f));
        CHECK(mix.spatializedId == 6u);
        CHECK(mix.activeViewId == 11u);
        CHECK(mix.viewCount == 1u);
        CHECK(mix.viewsWritten == 1u);
        CHECK(mix.views[0].id == 11u);
        CHECK(mix.views[0].pos[1] == doctest::Approx(2.5f));
    }

    SUBCASE("a count past the cap is reported and the rows stop at the cap") {
        FakeSoundPlayer sp;
        sp.Wire(replay::IEntity::AudioMix::kListenerCap + 3u, 0);
        for (uint32_t i = 0; i < 8; ++i) sp.Listener(i, i, (float)i, 0.0f, 0.0f, 1.0f);

        replay::IEntity::AudioMix mix;
        REQUIRE(replay::IEntity::AudioMixFrom(sp.bytes, mix));
        // The count is the finding; clamping it away would turn "more listeners than we can print"
        // into "exactly as many as we can print".
        CHECK(mix.listenerCount == replay::IEntity::AudioMix::kListenerCap + 3u);
        CHECK(mix.listenersWritten == replay::IEntity::AudioMix::kListenerCap);
    }

    SUBCASE("an absurd count is refused rather than walked") {
        FakeSoundPlayer sp;
        sp.Wire(1ull << 40, 1ull << 40);

        replay::IEntity::AudioMix mix;
        REQUIRE(replay::IEntity::AudioMixFrom(sp.bytes, mix));
        CHECK(mix.listenerCount == (1ull << 40));
        CHECK(mix.listenersWritten == 0u);
        CHECK(mix.viewsWritten == 0u);
    }

    SUBCASE("a null table pointer leaves the counts readable and writes no rows") {
        FakeSoundPlayer sp;
        sp.Wire(3, 2);
        sp.Put<void*>(lpf_audio::kAudioListenerBaseOff, nullptr);
        sp.Put<void*>(lpf_audio::kAudioViewBaseOff, nullptr);

        replay::IEntity::AudioMix mix;
        REQUIRE(replay::IEntity::AudioMixFrom(sp.bytes, mix));
        CHECK(mix.listenerCount == 3u);
        CHECK(mix.listenersWritten == 0u);
        CHECK(mix.viewCount == 2u);
        CHECK(mix.viewsWritten == 0u);
    }

    SUBCASE("a null sound player is a refusal with a sentence, not an empty answer") {
        replay::IEntity::AudioMix mix;
        CHECK_FALSE(replay::IEntity::AudioMixFrom(nullptr, mix));
        REQUIRE(mix.refused != nullptr);
        CHECK(mix.listenersWritten == 0u);
        CHECK(mix.system == 0u);
    }

    SUBCASE("the emitter map is read through its pointer, id and position off the RECORD") {
        FakeSoundPlayer sp;
        sp.Wire(1, 1);
        sp.Listener(0, 0, 0.0f, 0.0f, 0.0f, 1.0f);
        sp.View(0, 11, 0.0f, 0.0f, 0.0f);
        sp.WireEmitters(3);
        sp.Emitter(0, 12u, 0x20u, 1.0f, 2.0f, 3.0f);
        sp.Emitter(1, 77u, 0x0u, -10.0f, 0.0f, 250.0f);
        sp.Emitter(2, 99u, 0x20u, 0.0f, 0.0f, 0.0f);

        replay::IEntity::AudioMix mix;
        REQUIRE(replay::IEntity::AudioMixFrom(sp.bytes, mix));
        CHECK(mix.emitterCount == 3u);
        CHECK(mix.emittersWritten == 3u);
        CHECK(mix.emitters[0].id == 12u);
        CHECK(mix.emitters[0].flags == 0x20u);
        CHECK(mix.emitters[0].pos[1] == doctest::Approx(2.0f));
        // The flag the post is gated on has to survive the read: an emitter without it is one the
        // engine will not play a sound on, and a census that dropped it would read as silence.
        CHECK(mix.emitters[1].flags == 0u);
        CHECK(mix.emitters[1].pos[2] == doctest::Approx(250.0f));
        CHECK(mix.emitters[2].pos[0] == doctest::Approx(0.0f));
    }

    SUBCASE("a map entry whose record is gone is skipped, and the walk keeps going") {
        FakeSoundPlayer sp;
        sp.Wire(0, 0);
        sp.WireEmitters(3);
        sp.Emitter(0, 12u, 0x20u, 1.0f, 2.0f, 3.0f, false);
        sp.Emitter(1, 77u, 0x20u, 4.0f, 5.0f, 6.0f);
        sp.Emitter(2, 99u, 0x20u, 7.0f, 8.0f, 9.0f);

        replay::IEntity::AudioMix mix;
        REQUIRE(replay::IEntity::AudioMixFrom(sp.bytes, mix));
        CHECK(mix.emitterCount == 3u);
        CHECK(mix.emittersWritten == 2u);
        CHECK(mix.emitters[0].id == 77u);
        CHECK(mix.emitters[1].id == 99u);
    }

    SUBCASE("listener sets fold by identity, and a set longer than the buffer keeps its size") {
        replay::IEntity::AudioMix mix;
        const uint64_t one[1] = {0};
        const uint64_t two[2] = {0, 3};
        replay::IEntity::AudioAccumulateListenerSet(mix, one, 1, 1);
        replay::IEntity::AudioAccumulateListenerSet(mix, one, 1, 1);
        replay::IEntity::AudioAccumulateListenerSet(mix, two, 2, 2);
        replay::IEntity::AudioAccumulateListenerSet(mix, one, 1, 1);
        REQUIRE(mix.setsWritten == 2u);
        CHECK(mix.sets[0].emitters == 3u);
        CHECK(mix.sets[0].size == 1u);
        CHECK(mix.sets[1].emitters == 1u);
        CHECK(mix.sets[1].ids[1] == 3u);

        // Wwise reports the true size even when it wrote fewer ids than that. Folding on the
        // stored prefix alone would merge "answers to 1 listener" with "answers to 9".
        replay::IEntity::AudioAccumulateListenerSet(mix, one, 1, 9);
        REQUIRE(mix.setsWritten == 3u);
        CHECK(mix.sets[2].size == 9u);
        CHECK(mix.sets[2].emitters == 1u);
    }

    SUBCASE("an absurd emitter count is reported and walked zero times") {
        FakeSoundPlayer sp;
        sp.Wire(0, 0);
        sp.WireEmitters(1ull << 40);
        sp.Emitter(0, 12u, 0x20u, 1.0f, 2.0f, 3.0f);

        replay::IEntity::AudioMix mix;
        REQUIRE(replay::IEntity::AudioMixFrom(sp.bytes, mix));
        CHECK(mix.emitterCount == (1ull << 40));
        CHECK(mix.emittersWritten == 0u);
    }
}

// ---- FINAL-FIX-1 AUDIO-PLAYING -------------------------------------------------------------
// The struct sub_1413E9220 fills is the one thing on the far-sound path that a test can pin
// without a sound engine: the decode has to read the four curve-end numbers off the byte offsets
// the facts rows name, and it has to leave a refused (all-zero) answer looking refused.
TEST_CASE("AUDIO-PLAYING: AkPositioningInfo decodes at the offsets the facts name") {
    namespace lpf = replay::localpawn_facts;
    uint8_t info[replay::IEntity::kPositioningInfoBytes] = {};

    SUBCASE("each field comes off its own offset") {
        info[lpf::kAkPosInfoAttenuationOff] = 1;
        const float maxDist = 40.0f, dry = -96.3f, auxGame = -2.5f, auxUser = -11.0f;
        memcpy(info + lpf::kAkPosInfoMaxDistanceOff, &maxDist, sizeof maxDist);
        memcpy(info + lpf::kAkPosInfoVolDryOff, &dry, sizeof dry);
        memcpy(info + lpf::kAkPosInfoVolAuxGameOff, &auxGame, sizeof auxGame);
        memcpy(info + lpf::kAkPosInfoVolAuxUserOff, &auxUser, sizeof auxUser);

        replay::IEntity::AudioPlayingSound s;
        replay::IEntity::AudioDecodePositioningInfo(info, s);
        CHECK(s.attenuation);
        CHECK(s.maxDistance == doctest::Approx(40.0f));
        CHECK(s.volDryAtMax == doctest::Approx(-96.3f));
        CHECK(s.volAuxGameDefAtMax == doctest::Approx(-2.5f));
        CHECK(s.volAuxUserDefAtMax == doctest::Approx(-11.0f));
    }

    SUBCASE("a zeroed struct is an unattenuated voice with no curve, not a silent one") {
        replay::IEntity::AudioPlayingSound s;
        replay::IEntity::AudioDecodePositioningInfo(info, s);
        CHECK_FALSE(s.attenuation);
        CHECK(s.maxDistance == doctest::Approx(0.0f));
        CHECK(s.volDryAtMax == doctest::Approx(0.0f));
    }

    SUBCASE("a null buffer leaves the row untouched") {
        replay::IEntity::AudioPlayingSound s;
        s.maxDistance = 7.0f;
        replay::IEntity::AudioDecodePositioningInfo(nullptr, s);
        CHECK(s.maxDistance == doctest::Approx(7.0f));
    }

    SUBCASE("the struct is exactly what the query zeroes and fills") {
        CHECK(replay::IEntity::kPositioningInfoBytes == 68u);
        CHECK(lpf::kAkPosInfoVolAuxUserOff + sizeof(float) <=
              replay::IEntity::kPositioningInfoBytes);
    }
}

// ---- DESIGN-HOST-SYNC P0: the script-graph seam against the same byte-level rig -----------------

namespace {

namespace sgf = replay::scriptgraph_facts;

struct SeamLog {
    uint32_t setCalls = 0;
    uint64_t setType = 0, setId = 0;
    uint32_t sendCalls = 0;
    const void* sendGraph = nullptr;
    uint8_t event[sgf::kEventSize] = {};
};
SeamLog g_seam;

constexpr uint64_t kSeamSpace = 0x3F9915D3001DC28Eull;
constexpr uint64_t kSeamActor = 0x000000000001003Bull;
constexpr uint64_t kSeamNode = 0x999D680B6B01B67Eull;
// Where the stand-in keeps game_timer inside EXECUTION_DATA (the arena graph's own is +0x78).
constexpr uint32_t kTimerDataOff = 0x78;

uint8_t* SeamData(void* graph) {
    uint8_t* data = nullptr;
    memcpy(&data, static_cast<uint8_t*>(graph) + lpf_script::kScriptGraphDataOff, sizeof data);
    return data;
}

// script_get_variable's shape: type first, then name, nothing written on a miss.
int FakeSeamGet(void* graph, uint64_t name, uint64_t type, void* out) {
    if (type == replay::gametype::SymbolHash("actor") && name == replay::gametype::SymbolHash("self")) {
        uint8_t var[lpf_script::kActorVarSize] = {};
        memcpy(var + lpf_script::kActorVarSpaceOff, &kSeamSpace, 8);
        memcpy(var + lpf_script::kActorVarActorOff, &kSeamActor, 8);
        memcpy(var + lpf_script::kActorVarNodeOff, &kSeamNode, 8);
        memcpy(out, var, sizeof var);
        return 1;
    }
    if (type != sgf::kRealType || name != sgf::kSymGameTimer) return 0;
    memcpy(out, SeamData(graph) + kTimerDataOff, sizeof(float));
    return 1;
}

int FakeSeamSet(void* graph, uint64_t name, uint64_t type, void* value, bool* delegateVariable,
                uint64_t id) {
    ++g_seam.setCalls;
    g_seam.setType = type;
    g_seam.setId = id;
    if (delegateVariable) *delegateVariable = false;
    if (type != sgf::kRealType || (name != sgf::kSymGameTimer && id != sgf::kGameTimerId)) return 0;
    memcpy(SeamData(graph) + kTimerDataOff, value, sizeof(float));
    return 1;
}

int64_t FakeSeamSend(void* graph, void* event) {
    ++g_seam.sendCalls;
    g_seam.sendGraph = graph;
    memcpy(g_seam.event, event, sizeof g_seam.event);
    return 1;
}

uint64_t EventQword(uint32_t off) {
    uint64_t v = 0;
    memcpy(&v, g_seam.event + off, sizeof v);
    return v;
}

}  // namespace

TEST_CASE("SCRIPT-GRAPH: find by module hash and actor; set via +248, read back via +240; send via +512") {
    replay::livesafety::SetForTest(false, true);
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    FakeCs phys, player;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;

    auto rig = std::unique_ptr<ScriptRig>(new ScriptRig());
    rig->Build(space);
    // 0: another script. 1: the arena graph on actor 0x1003B, initialised. 2: the arena graph on
    // a second actor, never initialised. 3: the absent-component sentinel.
    rig->WireInstance(0, kSeamActor, 0, kScriptOther, 5, true);
    rig->WireInstance(1, kSeamActor, 1, sgf::kArenaScriptHash, 5, true);
    rig->WireInstance(2, 0x000000000002003Bull, 2, sgf::kArenaScriptHash, 2, false);
    rig->WireInstance(3, 0x000000000003003Bull, lpf_script::kNoComponent, kScriptOther, 5, true);
    for (uint32_t i = 0; i < ScriptRig::kInstances; ++i) {
        ScriptRig::PutPtr(rig->module[i], lpf_script::kScriptModuleGetOff,
                          reinterpret_cast<const void*>(&FakeSeamGet));
        ScriptRig::PutPtr(rig->module[i], sgf::kModuleSetOff,
                          reinterpret_cast<const void*>(&FakeSeamSet));
        ScriptRig::PutPtr(rig->module[i], sgf::kModuleSendEventOff,
                          reinterpret_cast<const void*>(&FakeSeamSend));
    }
    const float seed = 719.0f;
    memcpy(rig->graphData[1] + kTimerDataOff, &seed, sizeof seed);
    g_seam = SeamLog();

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());

    replay::IEntity::ScriptGraphRef ref;
    const char* why = nullptr;
    REQUIRE(entity->FindScriptGraph(nullptr, sgf::kArenaScriptHash, 0, 0, ref, &why));
    CHECK(why == nullptr);
    CHECK(ref.inst == rig->inst[1]);
    CHECK(ref.graph == rig->Graph(1));
    CHECK(ref.selfValid);
    CHECK(entity->ScriptGraphValid(ref));

    SUBCASE("the value set through the +248 binding is what get reads back") {
        float v = 0.0f;
        REQUIRE(entity->ScriptGetReal(ref, sgf::kSymGameTimer, v));
        CHECK(v == seed);
        REQUIRE(entity->ScriptSetReal(ref, sgf::kSymGameTimer, sgf::kGameTimerId, 3600.0f));
        CHECK(g_seam.setCalls == 1);
        CHECK(g_seam.setType == sgf::kRealType);
        CHECK(g_seam.setId == sgf::kGameTimerId);
        REQUIRE(entity->ScriptGetReal(ref, sgf::kSymGameTimer, v));
        CHECK(v == 3600.0f);
    }
    SUBCASE("sendevent: actor = self, actorname = self.nodeid, component as passed, userdata -1") {
        REQUIRE(entity->ScriptSendEvent(ref, sgf::kSymOnNetTouchGrab, sgf::kSymNetTouchInteract));
        CHECK(g_seam.sendCalls == 1);
        CHECK(g_seam.sendGraph == rig->Graph(1));
        CHECK(EventQword(0) == sgf::kSymOnNetTouchGrab);
        CHECK(EventQword(sgf::kEventActorOff) == kSeamSpace);
        CHECK(EventQword(sgf::kEventActorOff + 8) == kSeamActor);
        CHECK(EventQword(sgf::kEventActorOff + 16) == kSeamNode);
        CHECK(EventQword(sgf::kEventActorNameOff) == kSeamNode);
        CHECK(EventQword(sgf::kEventComponentOff) == sgf::kSymNetTouchInteract);
        CHECK(EventQword(sgf::kEventUserDataOff) == ~0ull);
    }
    SUBCASE("the actor filter is honoured, and each refusal is named") {
        replay::IEntity::ScriptGraphRef other;
        CHECK_FALSE(entity->FindScriptGraph(nullptr, sgf::kArenaScriptHash, 0x000000000002003Bull,
                                            0, other, &why));
        REQUIRE(why != nullptr);
        CHECK(strstr(why, "EXECUTION_DATA") != nullptr);
        CHECK_FALSE(entity->FindScriptGraph(nullptr, sgf::kArenaScriptHash, 0x000000000009003Bull,
                                            0, other, &why));
        REQUIRE(why != nullptr);
        CHECK(strstr(why, "not on this actor") != nullptr);
        REQUIRE(entity->FindScriptGraph(nullptr, sgf::kArenaScriptHash, kSeamActor, 0, other, &why));
        CHECK(other.inst == rig->inst[1]);
        CHECK_FALSE(entity->FindScriptGraph(nullptr, sgf::kDiscFxScriptHash, 0, 0, other, &why));
        REQUIRE(why != nullptr);
        CHECK(strstr(why, "module hash") != nullptr);
    }
    SUBCASE("the actor is the row's first qword; a zero actorHi matches any system's row") {
        // A live row's second qword is the owning system's symbol, never zero.
        const uint64_t kRowSymbol = 0x5C1217ACB5ED2A9Dull;
        rig->actorIndex[1][1] = kRowSymbol;
        replay::IEntity::ScriptGraphRef other;
        REQUIRE(entity->FindScriptGraph(nullptr, sgf::kArenaScriptHash, kSeamActor, 0, other, &why));
        CHECK(other.inst == rig->inst[1]);
        CHECK(other.actorHi == kRowSymbol);
        REQUIRE(entity->FindScriptGraph(nullptr, sgf::kArenaScriptHash, kSeamActor, kRowSymbol,
                                        other, &why));
        CHECK(other.inst == rig->inst[1]);
        CHECK_FALSE(entity->FindScriptGraph(nullptr, sgf::kArenaScriptHash, kSeamActor,
                                            kRowSymbol + 1, other, &why));
        REQUIRE(why != nullptr);
        CHECK(strstr(why, "not on this actor") != nullptr);
    }
    SUBCASE("valid, then stale: the module stops naming the hash and every call refuses") {
        ScriptRig::Put64(rig->module[1], lpf_script::kScriptModuleHashOff, kScriptOther);
        CHECK_FALSE(entity->ScriptGraphValid(ref));
        float v = 0.0f;
        CHECK_FALSE(entity->ScriptGetReal(ref, sgf::kSymGameTimer, v));
        CHECK_FALSE(entity->ScriptSetReal(ref, sgf::kSymGameTimer, sgf::kGameTimerId, 1.0f));
        CHECK_FALSE(entity->ScriptSendEvent(ref, sgf::kSymFxStateRed, ~0ull));
        CHECK(g_seam.setCalls == 0);
        CHECK(g_seam.sendCalls == 0);
    }
    SUBCASE("a live session writes nothing through either binding, and still reads") {
        replay::livesafety::SetForTest(false, false);
        CHECK_FALSE(entity->ScriptSetReal(ref, sgf::kSymGameTimer, sgf::kGameTimerId, 3600.0f));
        CHECK_FALSE(entity->ScriptSendEvent(ref, sgf::kSymFxStateRed, ~0ull));
        CHECK(g_seam.setCalls == 0);
        CHECK(g_seam.sendCalls == 0);
        float v = 0.0f;
        CHECK(entity->ScriptGetReal(ref, sgf::kSymGameTimer, v));
    }
    replay::livesafety::ResetForTest();
}

// RUN-VERIFY-6: the census's parent walk reached the loadout actor, which is mapped but is no space.
// Its "actor registry" qword was 0x10000000A and its "system table" named a component system whose
// actor index was a small integer, and each touch of those was a first-chance fault. Here both land
// in reserved, never-committed memory, which is what makes a touch fault and a query refuse.
TEST_CASE("FIX-N1: the disc-ring census walks a non-space parent without one probe fault") {
    namespace lpf = replay::localpawn_facts;
    Env& e = TheEnv();
    e.Tick(nullptr);

    FakeSpace space;
    space.Construct(space.elements);
    FakeCs phys, player;
    phys.WirePhys();
    player.WirePlayer();
    g_physCs = &phys;
    g_playerCs = &player;

    replay::IEntity* entity = replay::entity::EntityCreateWithLookup(space.bytes, &FakeFind);
    REQUIRE(entity->Available());
    DiscChain c;
    c.Build(0x00070005u, 0x00000002u, /*dense=*/2, /*enabled=*/1);
    replay::IEntity::DiscBind b;
    REQUIRE(entity->BindDisc(c.netGame, b));

    PagePair pages;
    REQUIRE(pages.base != nullptr);
    uint8_t* const hole = pages.base + pages.page;

    static uint8_t notASpace[0x800];
    alignas(16) uint8_t table[lpf::kSystemTableStride] = {};
    alignas(16) uint8_t cs[lpf::kCsActorIndexCountOff + sizeof(uint64_t)] = {};
    memset(notASpace, 0, sizeof notASpace);
    const uint64_t oneSystem = 1, fiveRows = 5;
    DiscChain::PutPtr(notASpace, lpf::kGameSpaceActorRegOff, hole + 0xA);
    DiscChain::PutPtr(notASpace, lpf::kGameSpaceSystemTableOff, table);
    memcpy(notASpace + lpf::kSystemTableCountOff, &oneSystem, sizeof oneSystem);
    DiscChain::PutPtr(notASpace, lpf::kGameSpaceParentOff, hole + 0x40);
    DiscChain::PutPtr(table, lpf::kSystemTableEntryCsOff, cs);
    DiscChain::PutPtr(cs, lpf::kCsActorIndexOff, hole);
    memcpy(cs + lpf::kCsActorIndexCountOff, &fiveRows, sizeof fiveRows);
    DiscChain::PutPtr(c.level, lpf::kGameSpaceParentOff, notASpace);

    const uint64_t faultsBefore = replay::ProbeFaultCount();
    static replay::IEntity::DiscRingReading ring;
    REQUIRE(entity->DiscRingCensus(ring));
    void* spaces[8] = {};
    const uint32_t n = entity->DiscCandidateSpaces(c.netGame, spaces, 8);
    CHECK(replay::ProbeFaultCount() == faultsBefore);

    // The walk must still have reached the object, or the zero above proves nothing.
    const replay::IEntity::DiscRingReading::SpaceScan* scanned = nullptr;
    for (uint32_t i = 0; i < ring.scansWritten; ++i)
        if (ring.scans[i].space == reinterpret_cast<uint64_t>(notASpace)) scanned = &ring.scans[i];
    REQUIRE(scanned != nullptr);
    CHECK(scanned->walked);
    CHECK(scanned->systemsSeen == 1u);
    CHECK(scanned->idxGameFrisbee == lpf::kNoComponent);
    CHECK(std::find(spaces, spaces + n, static_cast<void*>(notASpace)) != spaces + n);
    CHECK(std::find(spaces, spaces + n, static_cast<void*>(hole + 0x40)) != spaces + n);

    entity->DropDisc();
    g_physCs = nullptr;
    g_playerCs = nullptr;
    replay::entity::EntityCreate(nullptr);
}
