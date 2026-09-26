// W1-E's module translation unit: the one place that creates the entity
// adapter, the pose publisher and the as-seen reader and publishes them in ReplayServices.
//
// Init publishes the singleton unresolved, so every consumer sees a non-null IEntity whose
// Available() is false until a game space exists. The tick binds it: TickInfo::netGame carries the
// CncaGameSpace at NETGAME.CS_SPACE_OFF, and the space is handed to EntityCreate only after its
// component-system table reads as CONSTRUCTED and SORTED. A null check alone is not a check on the
// table at all (DIAG-CRASH launch-12, AUDIT_W2-F_AVFIX): the engine's lookup indexes the table base
// unguarded once the count is non-zero.
//
// It was IDENT.NETGAME_GAMESPACE_OFF (+0x8) until ARCH-10 section 1 proved that slot is the CR15Game
// back-pointer and not a game space at all -- so CheckSpaceTable refused it on every attempt of
// every recorded run, and this module has never once bound. The guard was right; the object was
// wrong. The live header at +0x2B08 cannot be read from any recorded artifact (ARCH-10 section 8.1),
// so each bind ATTEMPT also emits section 4.2's four-candidate evidence line: which candidate the
// guard would accept, all four side by side, out of the launch itself. That is evidence, not a
// fallback -- this module binds NETGAME.CS_SPACE_OFF or it defers, and never hops.
//
// The binding is re-validated on every tick against CSREGISTER-EPOCH S1.3's TWO signals, and they
// are not symmetric. W2-F REGISTERS a record and asks "is our record still there"; W1-E SEARCHES
// the table, so the decisive question is the searcher's analogue: "does re-asking the search return
// what it returned when we bound?" -- answered from the table's own (symbol, system) entries, which
// tableguard has already proved readable, without calling the engine.
//
//   (a) DECISIVE -- the digest of the count entries the engine's binary search reads. A space
//       rebuilt into the SAME block with the same capacity and count installs newly constructed
//       component systems, so the entries differ even when the header triple does not. Acting on
//       the triple alone left phys_.cs and every saved owner handle addressing freed objects while
//       Available() read true.
//   (b) CORROBORATING -- the header triple (base, capacity, count). Reported when it moves and
//       never acted on alone: a live table that grew or was reallocated still names the same
//       systems, and re-binding on that alone would drop the owner ledger for nothing.
//
// Re-binding is idempotent for a searcher, which is why (a) differing is acted on immediately and
// is not for a registration that appends. A space that stops validating is dropped with no write
// into it. A binding whose entity does not resolve is a DEFERRAL, never a latch: the pools are
// allocated during the load window and a bind that lands inside it is retried, counted, per tick.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/services.h"
#include "bootstrap/tableguard.h"
#include "engine/asseen.h"
#include "engine/entity.h"
#include "engine/entity_emote.h"
#include "engine/entity_subject.h"
#include "engine/localpawn_facts.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"

namespace replay {
namespace entity {
namespace {

const char* const kReplayModuleName = "entity";
namespace lp = localpawn_facts;

// One refusal line per space, then one every kLogEveryDeferrals ticks, so a load window leaves
// evidence without burying the log in it.
const uint64_t kLogEveryDeferrals = 600u;

enum class Space : uint8_t {
    NoNetGame,          // TickInfo::netGame is null: before state 9, or between matches
    NetGameUnreadable,  // the slot holding the space pointer is not committed memory
    NoSpace,            // the slot reads null
    TableNotReady,      // tableguard refused; `why` says how
    TableUnsorted,      // constructed but the engine has not sorted it: its lookup would sort in place
    Ready,
};

// EPOCH (a), DECISIVE. FNV-1a over the `count` entries of the component-system table -- the exact
// bytes CS.LOOKUP_BY_SYMBOL binary-searches, so an equal digest means an equal search result and a
// differing one means our resolved systems are not what this table names any more. The count is
// mixed into the seed rather than only fixing the length, so a table that loses its last entry
// cannot digest as a prefix of itself. tableguard has already proved `capacity` elements readable,
// which is a superset of these bytes; the probe below is belt-and-braces and a failure of it is a
// refusal, never a digest we would compare.
bool EntryDigest(uint64_t base, uint64_t count, uint64_t stride, uint64_t& out) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(base));
    const size_t bytes = static_cast<size_t>(count * stride);
    if (!Readable(p, bytes)) return false;
    uint64_t h = 0xcbf29ce484222325ull ^ count;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    out = h;
    return true;
}

// What the live binding was made against -- EPOCH (a) only. The header triple is deliberately NOT
// in here: it is (b), and this is the struct the re-bind decision is taken on.
struct SpaceIdentity {
    void*    netGame = nullptr;
    void*    space   = nullptr;
    uint64_t entries = 0;   // the digest above; 0 until a table read as Ready

    bool operator==(const SpaceIdentity& o) const {
        return netGame == o.netGame && space == o.space && entries == o.entries;
    }
};

// EPOCH (b), corroborating. Carried and logged, compared only to report that it moved.
struct TableHeader {
    uint64_t base = 0, capacity = 0, count = 0;

    bool operator==(const TableHeader& o) const {
        return base == o.base && capacity == o.capacity && count == o.count;
    }
};

struct Resolution {
    Space          verdict = Space::NoNetGame;
    tableguard::TableWhy why = tableguard::TableWhy::NotChecked;
    SpaceIdentity  id;
    TableHeader    header;
};

const char* SpaceText(const Resolution& r) {
    switch (r.verdict) {
        case Space::NoNetGame:         return "no netGame";
        case Space::NetGameUnreadable: return "netGame unreadable";
        case Space::NoSpace:           return "game space null";
        case Space::TableNotReady:     return tableguard::WhyName(r.why);
        case Space::TableUnsorted:     return "system table unsorted";
        case Space::Ready:             return "ready";
    }
    return "?";
}

// Every field on the way from netGame to a usable space, each asked of the memory manager before
// it is read. Fills the identity with whatever was resolved so a refusal can still name the space.
Resolution ResolveSpace(void* netGame) {
    Resolution r;
    r.id.netGame = netGame;
    if (!netGame) return r;

    const uint8_t* slot = static_cast<const uint8_t*>(netGame) + facts::NETGAME_CS_SPACE_OFF;
    if (!Readable(slot, sizeof(void*))) { r.verdict = Space::NetGameUnreadable; return r; }
    void* space;
    memcpy(&space, slot, sizeof space);
    if (!space) { r.verdict = Space::NoSpace; return r; }
    r.id.space = space;

    const tableguard::TableCheck t = tableguard::CheckSpaceTable(
        space, lp::kGameSpaceSystemTableOff, lp::kSystemTableStride, false);
    r.why = t.why;
    // AUD-F3B F3B-1. tableguard now judges the sort bit itself, so the specific reason arrives as a
    // verdict and must be read before the generic not-Ready arm swallows it. The raw-bit test below
    // stays: it is the same rule for a table this one is not the only reader of.
    if (t.why == tableguard::TableWhy::SortPending) { r.verdict = Space::TableUnsorted; return r; }
    if (t.verdict != tableguard::TableVerdict::Ready) { r.verdict = Space::TableNotReady; return r; }
    if (t.raw.flags & lp::kSystemTableUnsorted) { r.verdict = Space::TableUnsorted; return r; }

    if (!EntryDigest(t.raw.base, t.raw.count, lp::kSystemTableStride, r.id.entries)) {
        r.verdict = Space::TableNotReady;
        r.why = tableguard::TableWhy::ArrayUnreadable;
        r.id.entries = 0;
        return r;
    }

    r.header.base = t.raw.base;
    r.header.capacity = t.raw.capacity;
    r.header.count = t.raw.count;
    r.verdict = Space::Ready;
    return r;
}

struct State {
    ReplayServices* services = nullptr;
    IEntity*        entity   = nullptr;
    bool            bound    = false;
    Resolution      last;              // the binding's, while bound; the last refusal's otherwise
    uint64_t        deferrals = 0;     // ticks a space was seen and refused, since the last change
    // F2. A bind whose entity did not resolve is retried every tick, and both halves of WHY are
    // kept: how many times, and the last reason. Without them a permanent refusal and a genuine
    // facts miss produce the same single line and the launch cannot tell them apart.
    uint64_t        resolveRefusals = 0;
    const char*     lastRefusal = nullptr;
    uint64_t        headerMoved = 0;   // (b) differed while (a) held -- counted, never acted on
};

State g;

void Log(LogLevel level, const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    g.services->log->Write(level, kReplayModuleName, "%s", text);
}

// ---- ARCH-12 section 4.3: the five-candidate evidence line ------------------------------------
//
// FROZEN INTERFACE. W1-E emits it, W2-R parses it, neither may change it without the other.
// ARCH-12 section 4.3 amends ARCH-10 section 4.2's grammar; quoted from the spike verbatim, the
// wrap being the page's and not the line's:
//
//   [entity] cs-space candidates: ng=<PTR16> | +0x8=<CAND> | +0x2B08=<CAND> | +0x2B10=<CAND>
//     | game+0x7AF0=<CAND> | game+0x7AF8=<CAND> | spawnsym=<SYM16> | levelsym=<SYM16>
//
//   <PTR16> ::= 16 uppercase hex digits, zero-padded
//   <CAND>  ::= <PTR16> " " <TABLE> | "null" | "unreadable"
//   <TABLE> ::= tableguard::FormatTable's existing rendering, verbatim and unparenthesised
//   <SYM16> ::= 16 uppercase hex digits | "null" | "unreadable"
//
// The whole line is emitted, symbol fields included: W0-2 merged GAME.SPAWN_GAMESPACE_SYM_OFF and
// SPACE.NAME_SYMBOL_OFF in e31c409, so both constants are in facts_generated.h at HEAD and the
// split ARCH-12 section 5.4 permits is not needed.
//
// ONE detail section 4.3 leaves unstated, chosen minimally and FROZEN here with the rest: a
// <SYM16> whose 8 bytes READ renders 16 hex digits ALWAYS, the all-zero symbol included. `null`
// means the object the symbol would have been read out of was null, `unreadable` that the read was
// refused -- its own, or its parent's, handed down exactly as <CAND> already hands the CR15Game's
// word down to game+0x7AF*. Rendering a zero symbol as `null` would make the null symbol
// indistinguishable from an absent space, which is the one thing these two fields are recorded to
// tell apart.
//
// spawnsym and levelsym are INFORMATION (section 4.3 rule 6, R2's rejected gate). Nothing here
// judges them and no verdict on this line moves on them; recording them is what earns their
// promotion to a gate later.
//
// WHY +0x2B10 is the row's subject now: ARCH-12 section 2.2 proves it holds the netGame's SECOND
// CncaGameSpaceMT (the level space) and section 2.3 that the spectator-camera CS is registered
// there. NETGAME.SPECTATORCAM_CS_OFF keeps its id and the id is a MISNOMER (ARCH-12 R5) -- do not
// read a type from it. The four-column line could not show this: it had no column for the slot
// that was right (ARCH-12 R3, "missing column, not wrong choice").
//
// One line per bind ATTEMPT, on the deferral throttle: never per tick, never per frame. Nothing is
// called into and no engine routine runs -- every slot is asked of the memory manager first, and
// CheckSpaceTable is the only judgement made. It exists because the header at +0x2B08 cannot be read
// from any recorded artifact, so the launch that could refute NETGAME.CS_SPACE_OFF has to be the same
// launch that names the candidate which is right (ARCH-10 section 8.1). It authorises nothing:
// ResolveSpace reads NETGAME.CS_SPACE_OFF and no other slot, whatever this line says.
//
// FormatTable is reused rather than forked. A second spelling of that format here would be a second
// thing for W2-R's parser to drift from, and the log would carry two renderings of one judgement.
const char* const kCandNull = "null";
const char* const kCandUnreadable = "unreadable";

// The two CR15Game slots section 4.2 lists. Neither is a facts.tsv row: they are candidates this line
// exists to judge, not offsets anything binds on.
const uint32_t kGameCandidateAOff = 0x7AF0u;
const uint32_t kGameCandidateBOff = 0x7AF8u;

// The slot labels in the format string are frozen literal text. A fact that moved under one of them
// would put the line's own name for a slot at odds with the slot it read, and W2-R would attribute a
// verdict to the wrong candidate -- so the grammar and the parser move together or not at all, and
// the build stops here rather than mislabelling. Writing the value out is the point: importing the
// constant into its own guard is a comparison that cannot fail.
static_assert(facts::IDENT_NETGAME_GAMESPACE_OFF == 0x8u,
              "section 4.2 froze the +0x8 label; it must still name the slot this line reads");
static_assert(facts::NETGAME_CS_SPACE_OFF == 0x2B08u,  // facts-ok: NETGAME.CS_SPACE_OFF label guard
              "section 4.2 froze the +0x2B08 label; it must still name the slot this line reads");
// The id is a misnomer (ARCH-12 R5): the slot holds the netGame's LEVEL CncaGameSpaceMT, and the
// label on the wire is its offset, not its id. The hatch is on the literal's OWN line -- gatecheck's
// EXEMPT_LINE is per-line, so a hatch on the comment above exempts the comment, not the guard.
static_assert(facts::NETGAME_SPECTATORCAM_CS_OFF == 0x2B10u,  // facts-ok: NETGAME.SPECTATORCAM_CS_OFF label guard
              "ARCH-12 4.3 froze the +0x2B10 label; it must still name the slot this line reads");
// These two carry no offset in their labels, so the frozen thing is which field each name reports:
// a fact that moved under `spawnsym` or `levelsym` would leave W2-R attributing one object's symbol
// to another.
static_assert(facts::GAME_SPAWN_GAMESPACE_SYM_OFF == 0x8A48u,  // facts-ok: GAME.SPAWN_GAMESPACE_SYM_OFF label guard
              "ARCH-12 4.3 froze spawnsym as *(game + 0x8A48)");
static_assert(facts::SPACE_NAME_SYMBOL_OFF == 0x58u,
              "ARCH-12 4.3 froze levelsym as *(*(ng+0x2B10) + 0x58)");

// The pointer in one slot, or null with `word` set to the <CAND> literal standing in for it. A
// parent whose own read failed hands its word down: section 4.2 renders both game+0x7AF* exactly as
// `game` itself read, so a null CR15Game does not silently become an unreadable one.
void* ReadSlot(const void* base, uint32_t off, const char*& word) {
    const uint8_t* slot = static_cast<const uint8_t*>(base) + off;
    if (!Readable(slot, sizeof(void*))) { word = kCandUnreadable; return nullptr; }
    void* p = nullptr;
    memcpy(&p, slot, sizeof p);
    word = p ? nullptr : kCandNull;
    return p;
}

// AUD-C1 F3's counter, and the reason it is not a debug ornament: the defect F3 named is work
// done on every tick that emits NOTHING, so no count of lines can see it and only a count of the
// judgements can. Incremented where CheckSpaceTable is called, read by CandidateTableChecks below.
uint64_t gCandChecks = 0;

void FormatCandidate(void* p, const char* word, char* buf, size_t cap) {
    if (!p) {
        snprintf(buf, cap, "%s", word);
        return;
    }
    const int n = snprintf(buf, cap, "%016llX ",
                           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
    if (n < 0 || static_cast<size_t>(n) >= cap) return;
    ++gCandChecks;
    const tableguard::TableCheck check =
        tableguard::CheckSpaceTable(p, lp::kGameSpaceSystemTableOff, lp::kSystemTableStride, false);
    tableguard::FormatTable(check, buf + n, cap - static_cast<size_t>(n));
}

// A <SYM16>: the CSymbol64 at `owner + off`, or the word standing in for an owner that is not
// there. `ownerWord` is the <CAND> literal the owner's own read produced, handed down for the same
// reason game+0x7AF* takes the CR15Game's -- a null level space must not read as an unreadable one.
void FormatSymbol(const void* owner, uint32_t off, const char* ownerWord, char* buf, size_t cap) {
    if (owner == nullptr) {
        snprintf(buf, cap, "%s", ownerWord != nullptr ? ownerWord : kCandNull);
        return;
    }
    const uint8_t* slot = static_cast<const uint8_t*>(owner) + off;
    if (!Readable(slot, sizeof(uint64_t))) {
        snprintf(buf, cap, "%s", kCandUnreadable);
        return;
    }
    uint64_t sym = 0;
    memcpy(&sym, slot, sizeof sym);
    snprintf(buf, cap, "%016llX", static_cast<unsigned long long>(sym));
}

// Widest <CAND> is 16 hex digits, a space, and FormatTable's longest line (two names plus seven
// fields, the decimal ones 20 digits each) -- 218 bytes, so five of them plus the labels and the
// two <SYM16> overrun Log's 512-byte buffer several times over. Built and written whole here
// instead: a truncated evidence line is one W2-R would misparse rather than reject. The buffer is
// sized from the widest rendering rather than to a round number, so a sixth field cannot be added
// without this arithmetic being redone.
const size_t kCandCap = 288;                                   // >= 16 + 1 + 218 + 1
const size_t kSymCap = 24;                                     // >= 16 + 1
const size_t kLabelsCap = 128;                                 // "cs-space candidates: ng=" + PTR16
                                                               //   + the seven ` | <label>=`
const size_t kLineCap = 5 * kCandCap + 2 * kSymCap + kLabelsCap;

// The post-bind watch waits for a thing that need never arrive -- a launch that never
// loads a level leaves +0x2B10 null for the whole session -- so the window it keeps open must be
// bounded by something other than the arrival it is waiting for. Two ceilings, both per binding:
//
//   kCandChangeLines  full renderings this binding may spend. Five candidates and two symbols
//                     cannot each move more than once in a load that is going anywhere, so eight is
//                     that with room to spare; a ninth means a candidate is flapping, which is
//                     worth ONE line saying so and not one per tick. Spending it is TERMINAL for
//                     the binding: if re-arming refilled the budget a flapping pointer would defeat
//                     the ceiling outright, which is the same unbounded shape in a longer spelling.
//
//   kCandIdleTicks    ticks with no probe movement after which the watch goes dormant. 1800 is
//                     30 s at the engine's 60 Hz frame tick -- a load window that has not moved in
//                     half a minute is not loading. Dormant is not stopped: it drops to the five
//                     slot POINTERS alone, and any of them moving re-arms it, so a level space that
//                     arrives four minutes late is still logged.
const uint64_t kCandChangeLines = 8u;
const uint64_t kCandIdleTicks = 1800u;

// What the watch compares PER TICK, and deliberately not the rendered line: five slot reads, plus
// -- only while armed -- the two real candidates' table headers, read raw. No CheckSpaceTable, no
// FormatTable, no symbol read, no snprintf. The header words are exactly the ones tableguard
// derives its verdict from, so a verdict moving under a pointer that did not is still a change;
// `word` is one of two file-static literals or null, so comparing it by address is comparing which
// word it is.
struct CandProbe {
    uintptr_t            p[5] = {};
    const char*          word[5] = {};
    tableguard::TableRaw hdr[2] = {};  // +0x2B08 and +0x2B10; left zero on a dormant tick
};

bool SameSlots(const CandProbe& a, const CandProbe& b) {
    for (size_t i = 0; i < 5; ++i)
        if (a.p[i] != b.p[i] || a.word[i] != b.word[i]) return false;
    return true;
}

bool SameHeaders(const CandProbe& a, const CandProbe& b) {
    return memcmp(a.hdr, b.hdr, sizeof a.hdr) == 0;
}

// The 0x40-byte header as bytes, with tableguard's own offsets so the two readers cannot drift.
// A header that is not there stays all-zero, which is a state no readable header shares: the
// engine's empty block stores kNoBlock in `base`, never 0 (tableguard.h, kNoBlock).
void ReadTableHeader(const void* space, tableguard::TableRaw& raw) {
    if (!space) return;
    const uint8_t* h = static_cast<const uint8_t*>(space) + lp::kGameSpaceSystemTableOff;
    if (!Readable(h, tableguard::kHeaderBytes)) return;
    memcpy(&raw.base, h + tableguard::kBaseOff, sizeof raw.base);
    memcpy(&raw.bytes, h + tableguard::kBytesOff, sizeof raw.bytes);
    memcpy(&raw.allocator, h + tableguard::kAllocatorOff, sizeof raw.allocator);
    memcpy(&raw.grow, h + tableguard::kGrowOff, sizeof raw.grow);
    memcpy(&raw.capacity, h + tableguard::kCapacityOff, sizeof raw.capacity);
    memcpy(&raw.count, h + tableguard::kCountOff, sizeof raw.count);
    // DIAG-F3-B section 2: the flags slot is a DWORD, and reading it 8 wide pulls the next field in.
    uint32_t flags = 0;
    memcpy(&flags, h + tableguard::kFlagsOff, sizeof flags);
    raw.flags = flags;
}

CandProbe ProbeCandidates(void* netGame, bool slotsOnly) {
    CandProbe pr;
    void* game = ReadSlot(netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, pr.word[0]);
    void* csSpace = ReadSlot(netGame, facts::NETGAME_CS_SPACE_OFF, pr.word[1]);
    void* levelSpace = ReadSlot(netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, pr.word[2]);
    // The parent's word, handed down exactly as the rendering hands it down, so the probe and the
    // line agree about what a null CR15Game's children are.
    pr.word[3] = pr.word[0];
    void* a = game ? ReadSlot(game, kGameCandidateAOff, pr.word[3]) : nullptr;
    pr.word[4] = pr.word[0];
    void* b = game ? ReadSlot(game, kGameCandidateBOff, pr.word[4]) : nullptr;

    void* const slots[5] = {game, csSpace, levelSpace, a, b};
    for (size_t i = 0; i < 5; ++i) pr.p[i] = reinterpret_cast<uintptr_t>(slots[i]);
    if (!slotsOnly) {
        ReadTableHeader(csSpace, pr.hdr[0]);
        ReadTableHeader(levelSpace, pr.hdr[1]);
    }
    return pr;
}

// The evidence line's own state, separate from State because it is the emitter's and not the
// binding's, and because it is reset on a different edge (a new binding, not a new tick). Init
// clears it with the module.
struct CandidateWatch {
    CandProbe probe;
    uint64_t  lines = 0;            // full renderings spent since the bind -- kCandChangeLines caps it
    uint64_t  idle = 0;             // consecutive ticks the probe has not moved
    bool      dormant = false;      // watching the slot pointers only
    bool      sawCsSpace = false;   // +0x2B08 has rendered non-null at least once since the bind
    bool      sawLevelSpace = false;  // +0x2B10 likewise -- ARCH-12 section 5: neither arrives first
};

CandidateWatch gCand;

void RenderCandidates(void* netGame, char* line, size_t cap) {
    const char* wGame = nullptr;
    void* game = ReadSlot(netGame, facts::IDENT_NETGAME_GAMESPACE_OFF, wGame);
    const char* wSpace = nullptr;
    void* csSpace = ReadSlot(netGame, facts::NETGAME_CS_SPACE_OFF, wSpace);
    // ARCH-12 R5's reserved words: the netGame has TWO spaces and neither may be `gameSpace`.
    const char* wLevel = nullptr;
    void* levelSpace = ReadSlot(netGame, facts::NETGAME_SPECTATORCAM_CS_OFF, wLevel);
    const char* wA = wGame;
    void* a = game ? ReadSlot(game, kGameCandidateAOff, wA) : nullptr;
    const char* wB = wGame;
    void* b = game ? ReadSlot(game, kGameCandidateBOff, wB) : nullptr;

    char c1[kCandCap], c2[kCandCap], c3[kCandCap], c4[kCandCap], c5[kCandCap];
    FormatCandidate(game, wGame, c1, sizeof c1);
    FormatCandidate(csSpace, wSpace, c2, sizeof c2);
    FormatCandidate(levelSpace, wLevel, c3, sizeof c3);
    FormatCandidate(a, wA, c4, sizeof c4);
    FormatCandidate(b, wB, c5, sizeof c5);

    char spawnsym[kSymCap], levelsym[kSymCap];
    FormatSymbol(game, facts::GAME_SPAWN_GAMESPACE_SYM_OFF, wGame, spawnsym, sizeof spawnsym);
    FormatSymbol(levelSpace, facts::SPACE_NAME_SYMBOL_OFF, wLevel, levelsym, sizeof levelsym);

    snprintf(line, cap,
             "cs-space candidates: ng=%016llX | +0x8=%s | +0x2B08=%s | +0x2B10=%s"
             " | game+0x7AF0=%s | game+0x7AF8=%s | spawnsym=%s | levelsym=%s",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(netGame)), c1, c2, c3, c4,
             c5, spawnsym, levelsym);
}

// The baseline every later change is measured against, and the latch that closes the window.
void RecordCandidates(const CandProbe& pr) {
    gCand.probe = pr;
    gCand.idle = 0;
    gCand.dormant = false;
    if (pr.p[1]) gCand.sawCsSpace = true;
    if (pr.p[2]) gCand.sawLevelSpace = true;
}

// Emit unconditionally. Every bind ATTEMPT takes this path -- the bind edge and the deferral
// path's throttle -- so the line is never silent about an attempt. The attempt does not spend the
// post-bind budget: kLogEveryDeferrals already bounds this path.
void LogCandidates(void* netGame) {
    char line[kLineCap];
    RenderCandidates(netGame, line, sizeof line);
    RecordCandidates(ProbeCandidates(netGame, false));
    g.services->log->Write(LogLevel::Info, kReplayModuleName, "%s", line);
}

// L5 re-run #4 F11: the line used to stop at the bind, and the level space at +0x2B10 appeared
// 1.4 s LATER -- so a row read `+0x2B10 rendered null` off a sampling window that had already
// closed, while the camera went on to bind to that very space. ARCH-12 section 5 forbids assuming
// either space's arrival order (L5 #3 had the reverse by 1.3 s), so neither a longer window nor a
// single re-check answers it: the stop condition has to BE the thing being waited for.
//
// THE INVARIANT, whole: after a successful bind the line is emitted on every CHANGE of a candidate
// -- a slot going null->non-null, or the table under one moving -- until BOTH real candidates
// (+0x2B08 and +0x2B10) have rendered non-null at least once, or until one of the two ceilings
// above is reached; whichever comes first, and each of the three says so.
//
// AUD-C1 F3 is the cost half of it, and it is the probe that answers it: a steady tick costs five
// slot reads and two header reads and makes NO tableguard judgement at all, a dormant tick five
// slot reads, and the full rendering -- five CheckSpaceTable calls, two symbol reads and a
// kLineCap snprintf -- happens only on a tick where the probe moved, at most kCandChangeLines
// times per binding. The old shape ran the whole rendering every tick and merely stayed quiet
// about it, which is the defect a line count cannot see and CandidateTableChecks can.
void LogCandidateChange(void* netGame) {
    CandidateWatch& w = gCand;
    if (w.sawCsSpace && w.sawLevelSpace) return;
    if (w.lines >= kCandChangeLines) return;

    CandProbe pr = ProbeCandidates(netGame, w.dormant);
    const bool moved = !SameSlots(pr, w.probe) || (!w.dormant && !SameHeaders(pr, w.probe));
    if (!moved) {
        if (!w.dormant && ++w.idle >= kCandIdleTicks) {
            w.dormant = true;
            // silence after this line is DORMANCY, not an answer. Silence after a line whose
            // +0x2B10 is non-null is the closed window, and those are different facts.
            g.services->log->Write(
                LogLevel::Info, kReplayModuleName,
                "cs-space candidate watch dormant: ng=%016llX no candidate moved in %llu ticks;"
                " +0x2B08=%016llX +0x2B10=%016llX; the slot pointers are still read every tick and"
                " one of them moving re-arms it",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(netGame)),
                static_cast<unsigned long long>(w.idle),
                static_cast<unsigned long long>(w.probe.p[1]),
                static_cast<unsigned long long>(w.probe.p[2]));
        }
        return;
    }
    // A dormant tick did not read the headers, so the baseline this change becomes must be a whole
    // one or the next tick would read the zeroes back as a second change.
    if (w.dormant) pr = ProbeCandidates(netGame, false);

    char line[kLineCap];
    RenderCandidates(netGame, line, sizeof line);
    RecordCandidates(pr);
    ++w.lines;
    g.services->log->Write(LogLevel::Info, kReplayModuleName, "%s", line);

    if (w.lines >= kCandChangeLines && !(w.sawCsSpace && w.sawLevelSpace)) {
        g.services->log->Write(
            LogLevel::Info, kReplayModuleName,
            "cs-space candidate watch stopped: ng=%016llX %llu change lines spent and +0x2B08/"
            "+0x2B10 have not both rendered; a candidate is flapping and this binding says no more",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(netGame)),
            static_cast<unsigned long long>(w.lines));
    }
}

// The emote walk's three inputs, installed at Init and re-aimed wherever the binding moves.  The
// netGame is recorded now that a component system is: a system resolved out of one game space,
// asked about an entrant of another netGame, names a different player and comes back as a clean
// success, so the seam refuses that pairing instead of answering it.
EmoteChain g_emoteChain;

// The component-system input, through the engine's own CncaGameSpace::FindComponentSystem on the
// space this binding validated -- once per binding.  A system can be registered AFTER its space
// validates (the lateness RetryResolve exists for), so a binding that answers null is asked again
// on the ticks that follow: the ask is a binary search over the space's own table, it issues no
// VirtualQuery, and it stops the moment it answers.  Only a lookup that itself answers null leaves
// the chain empty, which is the state the seam names EmoteNoSystem.
void ResolveEmoteCs(const Resolution& r) {
    void* const cs = ResolveCustomizationCs(r.id.space, nullptr);
    if (!cs) return;
    g_emoteChain.customizationCs = cs;
    Log(LogLevel::Info,
        "emote component system %p resolved on space %p (netGame %p): scoreboard bit 14 is live",
        cs, r.id.space, r.id.netGame);
}

void Bind(const Resolution& r) {
    // A new binding is a new pair of candidates to wait on, so the window re-opens here and
    // LogCandidates below records the baseline every later change is measured against.
    gCand = CandidateWatch();
    // ARCH-12 4.3's line on the attempt that succeeded, too: rule 2's asked-pass is "+0x2B10
    // judged Ready or Empty", which the absence of a refusal cannot say.
    LogCandidates(r.id.netGame);
    // EntityBind, not EntityCreate: this function is reached only when the resolved SpaceIdentity
    // differs from the bound one, so it is exactly the edge IEntity's bind epoch has to mark for
    // the consumers that cached a user index issued by the space we are leaving.
    g.entity = EntityBind(r.id.space);
    g.services->entity = g.entity;
    SetSubjectEntity(g.entity, r.id.netGame);
    asseen::AsSeenDestroy(g.services->asSeen);
    g.services->asSeen = asseen::AsSeenCreate(r.id.netGame, g.entity);
    g.bound = true;
    g.last = r;
    // A new binding is a new space and a new netGame; the system the old one named belongs to
    // neither, so it is dropped before the new one is asked for.
    g_emoteChain.customizationCs = nullptr;
    g_emoteChain.netGame = r.id.netGame;
    ResolveEmoteCs(r);
    g.resolveRefusals = g.entity->Available() ? 0u : 1u;
    g.lastRefusal = g.entity->Available() ? nullptr : g.entity->Diagnostic();
    Log(LogLevel::Info,
        "bound game space %p (netGame %p, systems %llx cap=%llu count=%llu entries %llx): entity %s; "
        "local pawn %s; publisher %s",
        r.id.space, r.id.netGame, static_cast<unsigned long long>(r.header.base),
        static_cast<unsigned long long>(r.header.capacity),
        static_cast<unsigned long long>(r.header.count),
        static_cast<unsigned long long>(r.id.entries),
        g.entity->Available() ? "available" : g.entity->Diagnostic(), LocalPawnDiagnostic(g.entity),
        g.services->pose ? g.services->pose->Strategy() : "none");
}

// F2. The held binding's entity refused below the table check -- Resolve::PoolNotReady is the
// reachable one: Pool::Ready() needs the handle map and the element base dereferenced, and the
// system table's count tracks neither, so a system installed before its pool is allocated is a
// miss the next tick can clear. Ask again rather than latching, and say so at the same throttle
// the deferral path uses so a load window does not bury the log.
void RetryResolve() {
    g.entity = EntityCreate(g.last.id.space);
    g.services->entity = g.entity;
    SetSubjectEntity(g.entity, g.last.id.netGame);
    if (g.entity->Available()) {
        asseen::AsSeenDestroy(g.services->asSeen);
        g.services->asSeen = asseen::AsSeenCreate(g.last.id.netGame, g.entity);
        Log(LogLevel::Info, "entity resolved on space %p after %llu refusals (last %s); local pawn %s",
            g.last.id.space, static_cast<unsigned long long>(g.resolveRefusals),
            g.lastRefusal ? g.lastRefusal : "none", LocalPawnDiagnostic(g.entity));
        g.resolveRefusals = 0;
        g.lastRefusal = nullptr;
        return;
    }
    ++g.resolveRefusals;
    g.lastRefusal = g.entity->Diagnostic();
    if (g.resolveRefusals % kLogEveryDeferrals == 0) {
        Log(LogLevel::Info, "space %p still unresolved after %llu attempts: %s", g.last.id.space,
            static_cast<unsigned long long>(g.resolveRefusals), g.lastRefusal);
    }
}

// The old space is gone, or never finished building: nothing is written into it. Restoring owners
// is only for a space that is still there (Shutdown).
void Unbind(const Resolution& now) {
    g.entity = EntityUnbind();
    g.services->entity = g.entity;
    SetSubjectEntity(g.entity, nullptr);
    asseen::AsSeenDestroy(g.services->asSeen);
    g.services->asSeen = nullptr;
    g.bound = false;
    g_emoteChain.customizationCs = nullptr;
    g_emoteChain.netGame = nullptr;
    g.resolveRefusals = 0;
    g.lastRefusal = nullptr;
    Log(LogLevel::Info, "unbound game space %p (netGame %p): %s", g.last.id.space, g.last.id.netGame,
        SpaceText(now));
}

bool Init(ReplayServices* s) {
    REPLAY_REQUIRE(s, log);
    g = State();
    gCand = CandidateWatch();
    g.services = s;
    // EntityUnbind rather than EntityCreate: the epoch is process-global (the implementation is a
    // singleton), so a module Init that followed an earlier bind in the same process would
    // otherwise publish an entity still answering Ok with the previous binding's generation.
    g.entity = EntityUnbind();
    // F5. Published only once this Init can no longer fail. modreg marks a module whose Init
    // returned false not-live and never calls its Shutdown, so anything assigned before the last
    // failure point would stay in ReplayServices for the process lifetime, owned by a module that
    // refused -- and DESIGN B.12's rule is that a slot is null until its packet has registered.
    IPosePublisher* const pose = PosePublisherCreate(g.entity, PoseStrategy::PpB);
    if (!pose) {
        Log(LogLevel::Error,
            "PP-B publisher refused for the entity singleton; nothing can publish a pose. "
            "entity/pose/asSeen left unpublished");
        g = State();
        return false;
    }
    s->entity = g.entity;
    s->pose = pose;
    s->asSeen = nullptr;
    // The `cam look at <n>` composer, installed the way SetWritePoint is: from HERE, because
    // camera_control must not name the entity module.  Until this line runs camera_control has no
    // seam and refuses the command by name (SubjectResolve::NoSeam) rather than aiming at nothing.
    // The netGame is null until a bind: an unbound entity's pools refuse for themselves.
    SetSubjectEntity(g.entity, nullptr);
    camera_control::SetSubjectPose(&kSubjectPose);
    // Bit 14 of the scoreboard mask.  identity_access reads is_emote_playing through a seam
    // because it is the one scoreboard field that is NOT on CR15NetGame; until this line runs
    // ResolveEmoteSource refuses EmoteNoSeam -- nobody could be asked -- and never publishes
    // a cleared bit as "not playing".
    // The key half of the walk: the entrant SActorGUID second qword, read off
    // the netGame the seam is asked about.  The component-system half arrives at the first Bind;
    // until then the chain names no system and the seam refuses EmoteNoSystem, which is why the
    // chain is reset here -- a module re-Init in the same process must not inherit the system a
    // previous binding resolved out of a space that is gone.
    g_emoteChain = EmoteChain();
    g_emoteChain.actorKey = &EmoteActorGuidKey;
    SetEmoteChain(&g_emoteChain);
    identity_access::SetEmoteSource(&kEmoteSource);
    Log(LogLevel::Info, "entity published unresolved (%s); publisher %s; binding waits for a constructed game space",
        g.entity->Diagnostic(), s->pose->Strategy());
    return true;
}

void Tick(const TickInfo& info) {
    const Resolution r = ResolveSpace(info.netGame);

    if (r.verdict == Space::Ready) {
        // (a) decides. Equal entries mean the search we bound on still returns what it returned;
        // anything else -- a new netGame, a new space, or the same block carrying a rebuilt table
        // with the same capacity and count -- is a different table and a re-bind.
        if (g.bound && g.last.id == r.id) {
            if (!(g.last.header == r.header)) {
                ++g.headerMoved;
                Log(LogLevel::Info,
                    "system table moved on space %p (%llx cap=%llu count=%llu -> %llx cap=%llu count=%llu); "
                    "it names the same systems, so the binding stands (headerMoved %llu)",
                    r.id.space, static_cast<unsigned long long>(g.last.header.base),
                    static_cast<unsigned long long>(g.last.header.capacity),
                    static_cast<unsigned long long>(g.last.header.count),
                    static_cast<unsigned long long>(r.header.base),
                    static_cast<unsigned long long>(r.header.capacity),
                    static_cast<unsigned long long>(r.header.count),
                    static_cast<unsigned long long>(g.headerMoved));
                g.last.header = r.header;
            }
            if (!g_emoteChain.customizationCs) ResolveEmoteCs(r);
            if (!g.entity->Available()) RetryResolve();
            g.deferrals = 0;
            // The binding stands, but the OTHER real candidate may still be arriving: this is the
            // only path a late +0x2B10 is reachable from, and Bind recorded the baseline it is
            // compared against (the bound path is reachable only through Bind).
            if (info.netGame) LogCandidateChange(info.netGame);
            return;
        }
        if (g.bound) {
            Log(LogLevel::Info,
                "game space changed %p -> %p (systems %llx -> %llx, entries %llx -> %llx); re-binding",
                g.last.id.space, r.id.space, static_cast<unsigned long long>(g.last.header.base),
                static_cast<unsigned long long>(r.header.base),
                static_cast<unsigned long long>(g.last.id.entries),
                static_cast<unsigned long long>(r.id.entries));
        }
        Bind(r);
        g.deferrals = 0;
        return;
    }

    if (g.bound) {
        Unbind(r);
        g.deferrals = 0;
    }

    // A space that exists but does not validate yet is the load window; say so once per distinct
    // refusal and then sparsely, and never latch -- the next tick asks again.
    const bool changed = r.verdict != g.last.verdict || r.why != g.last.why || !(g.last.id == r.id);
    if (changed) g.deferrals = 0;
    const bool speak = changed || ++g.deferrals % kLogEveryDeferrals == 0;
    if (r.id.space && speak) {
        Log(LogLevel::Info, "game space %p (netGame %p) deferred: %s", r.id.space, r.id.netGame, SpaceText(r));
    }
    // On the same throttle, and NOT gated on r.id.space: a null at NETGAME.CS_SPACE_OFF is exactly
    // the refusal whose answer would have to name another candidate, so it is the one attempt the
    // evidence line must never be silent about. The refusal line above stays gated -- it has no
    // space to name.
    if (info.netGame && speak) LogCandidates(info.netGame);
    g.last = r;
}

void Shutdown() {
    if (!g.services) return;
    // Owners are restored only into a space that is still the one they were taken from.
    if (g.bound) {
        const Resolution r = ResolveSpace(g.last.id.netGame);
        if (r.verdict == Space::Ready && r.id == g.last.id) EntityDestroy(g.entity);
    }
    // The seam points at this module's own bodies; leaving it installed past Shutdown would leave
    // camera_control calling into a module the registry has already stopped.
    camera_control::SetSubjectPose(nullptr);
    identity_access::SetEmoteSource(nullptr);
    SetEmoteChain(nullptr);
    SetSubjectEntity(nullptr, nullptr);
    asseen::AsSeenDestroy(g.services->asSeen);
    g.services->asSeen = nullptr;
    g.services->entity = nullptr;
    g.services->pose = nullptr;
    g = State();
    gCand = CandidateWatch();
}

}  // namespace

// Test seam, AUD-C1 F3: the tableguard judgements the candidate line has made since the module was
// loaded. tests/entity/test_entity_module.cpp is the only caller. It exists because F3's defect is
// per-tick WORK that emits nothing -- a count of emitted lines is green under it, and a count of
// judgements is not; a budget stated in the code and unmeasurable from outside is not a budget.
uint64_t CandidateTableChecks() { return gCandChecks; }

}  // namespace entity
}  // namespace replay

REPLAY_MODULE("entity", 15, ::replay::entity::Init, ::replay::entity::Tick, ::replay::entity::Shutdown);
