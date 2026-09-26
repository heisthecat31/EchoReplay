// The ncaActorAlias record table, read.
//
// The engine resolves an actor guid by an EXACT match on {classId, index, generation} in one sorted
// table (sub_140215DC0 @ 0x140215DC0, reached through the +680 virtual on the ncaActorAlias system
// at space + GSPACE.ACTOR_ALIAS_OFF).  A pooled actor's record is not destroyed and recreated across
// generations: the pool free re-keys it in place (sub_140216620 @ 0x140216620, broadcast slot +640
// of sub_1404F1C60), and nothing on the allocation path ever inserts one.  So the two ways a
// recycled pool entry can become unresolvable -- its row is missing, or its row is a generation
// behind the pool's own counter -- look identical from the crash and different from here.
//
// Header-only because both the admission path and the entity surface need it and they do not share
// a link unit.  Every pointer is page-probed before it is followed and an unreadable link becomes a
// word in the rendered line, never a fault: this runs beside a live admission.  Nothing is written.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/pageprobe.h"
#include "engine/localpawn_facts.h"

namespace replay::actoralias {

// How many pool entries the rendered line spells out by hand.  A display bound, not a fact: the
// remoteplayer pools have capacity 15 and nothing is learned past the first sixteen.
inline constexpr uint32_t kEntriesShown = 16u;

namespace detail {

inline const void* At(const void* p, size_t off) {
    return p ? static_cast<const uint8_t*>(p) + off : nullptr;
}

// The content scans below pass `spans`, and so ask without touching: most qwords they follow are not
// pointers. Everything else passes nothing and keeps the touch.
inline bool CanRead(QueriedSpans* spans, const void* p, size_t len) {
    return spans ? spans->Readable(p, len) : replay::Readable(p, len);
}

// Follows a qword at p+off, but only after proving the qword itself is mapped.
inline const void* Deref(const void* p, size_t off, QueriedSpans* spans = nullptr) {
    const void* at = At(p, off);
    if (!CanRead(spans, at, sizeof(void*))) return nullptr;
    const void* v = nullptr;
    memcpy(&v, at, sizeof v);
    return v;
}

struct Line {
    char*  buf;
    size_t cap;
    size_t n = 0;

    template <typename... A>
    void Put(const char* fmt, A... args) {
        if (buf == nullptr || n + 1 >= cap) return;
        const int w = snprintf(buf + n, cap - n, fmt, args...);
        if (w < 0) { buf[n] = '\0'; return; }
        n = (size_t(w) >= cap - n) ? cap - 1 : n + size_t(w);
    }
};

// One subsystem's record table, as sub_140215DC0 reads it: the array at +272 and its length at
// +280, relative to the subsystem pointer the space publishes.
struct Table {
    const uint8_t* rows = nullptr;
    uint64_t       count = 0;
    uint32_t       storeOff = 0;        // filled by ReadStore: the `lea` displacement of the thunk
    const uint8_t* rekey = nullptr;     // filled by ReadStore: the +640 target its bytes anchored
};

// ONE COMPONENT SYSTEM'S ACTOR-RECORD STORE, FOUND THE ONLY WAY THAT PROVES ITSELF.
//
// The store is not at a fixed place in every system, so "read +272/+280 off any subsystem pointer"
// reads garbage from most of them.  What IS fixed is the shape of the system's own +640 re-key
// thunk: `mov r8,rdx` / `lea rdx,[rcx+STORE]` / `jmp sub_140216620`.  Decoding that `lea` gives the
// store this system will re-key, so the table read here and the table the write lands in are the
// same object by construction, and a system whose +640 slot is not one of these thunks -- which is
// most of the space -- is skipped instead of guessed at.
inline bool ReadStore(const void* system, Table& out, QueriedSpans* spans = nullptr) {
    namespace lp = replay::localpawn_facts;
    if (!CanRead(spans, system, sizeof(void*))) return false;
    const uint8_t* vtbl = nullptr;
    memcpy(&vtbl, system, sizeof vtbl);
    if (!CanRead(spans, vtbl, lp::kRekeyVtableOff + sizeof(void*))) return false;
    const uint8_t* fn = nullptr;
    memcpy(&fn, vtbl + lp::kRekeyVtableOff, sizeof fn);
    if (!CanRead(spans, fn, 16)) return false;
    if (fn[0] != 0x4C || fn[1] != 0x8B || fn[2] != 0xC2) return false;  // mov r8, rdx
    if (fn[3] != 0x48 || fn[4] != 0x8D) return false;
    int32_t disp = 0;
    if (fn[5] == 0x91) {                                               // lea rdx, [rcx+disp32]
        memcpy(&disp, fn + 6, sizeof disp);
        if (fn[10] != 0xE9) return false;                              // jmp rel32
    } else if (fn[5] == 0x51) {                                        // lea rdx, [rcx+disp8]
        disp = int32_t(int8_t(fn[6]));
        if (fn[7] != 0xE9) return false;
    } else {
        return false;
    }
    if (disp < 0 || disp > 0x10000) return false;
    const size_t store = size_t(uint32_t(disp));
    const void* const at = At(system, store + lp::kStoreCountOff);
    if (!CanRead(spans, at, sizeof(uint64_t))) return false;
    memcpy(&out.count, at, sizeof out.count);
    out.rows = static_cast<const uint8_t*>(Deref(system, store + lp::kStoreRowsOff, spans));
    out.storeOff = uint32_t(disp);
    out.rekey = fn;
    if (out.rows == nullptr || out.count == 0 || out.count > lp::kAliasCountSane) return false;
    // The whole row span, not its first row: a count that runs past the committed end is not this
    // store, and every row loop after this one trusts the count.
    return CanRead(spans, out.rows, size_t(lp::kAliasRowStride) * out.count);
}

inline bool ReadTable(const void* system, Table& out, QueriedSpans* spans = nullptr) {
    namespace lp = replay::localpawn_facts;
    if (!CanRead(spans, system, lp::kAliasCountOff + sizeof(uint64_t))) return false;
    memcpy(&out.count, At(system, lp::kAliasCountOff), sizeof out.count);
    out.rows = static_cast<const uint8_t*>(Deref(system, lp::kAliasRowsOff, spans));
    if (out.rows == nullptr || out.count == 0 || out.count > lp::kAliasCountSane) return false;
    return CanRead(spans, out.rows, size_t(lp::kAliasRowStride) * out.count);
}

// The pool descriptor for one class, as sub_1404F4420 pops it: the LIFO of free entries, its depth
// (which IS the pool's free count) and the per-entry generation counter the free advances.
struct Pool {
    void*           desc = nullptr;
    uint16_t*       stack = nullptr;
    const uint16_t* gens = nullptr;
    uint16_t        capacity = 0;
    uint64_t        freeCount = 0;
};

inline bool ReadPool(const void* gameSpace, uint16_t classId, Pool& out) {
    namespace lp = replay::localpawn_facts;
    const void* const poolTable = Deref(gameSpace, lp::kSpacePoolTableOff);
    void* const desc = const_cast<void*>(At(poolTable, lp::kPoolDescStride * classId));
    if (poolTable == nullptr || !replay::Readable(desc, lp::kPoolDescCapOff + sizeof(uint16_t)))
        return false;
    memcpy(&out.capacity, At(desc, lp::kPoolDescCapOff), sizeof out.capacity);
    memcpy(&out.freeCount, At(desc, lp::kPoolDescFreeOff), sizeof out.freeCount);
    if (out.capacity == 0 || out.freeCount > out.capacity) return false;
    out.stack = static_cast<uint16_t*>(const_cast<void*>(Deref(desc, lp::kPoolDescFreeStackOff)));
    out.gens = static_cast<const uint16_t*>(Deref(desc, lp::kPoolDescGenTableOff));
    if (!replay::Readable(out.gens, sizeof(uint16_t) * size_t(out.capacity))) return false;
    out.desc = desc;
    return true;
}

inline bool HoldsClass(const Table& t, uint16_t classId) {
    namespace lp = replay::localpawn_facts;
    // A display bound on a scan over pointers that are only probably tables; the real one is a few
    // hundred rows and a candidate that needs more than this to prove itself is not the one.
    const uint64_t scan = t.count < 4096 ? t.count : 4096;
    for (uint64_t i = 0; i < scan; ++i) {
        const uint8_t* const r = t.rows + lp::kAliasRowStride * i;
        if (!replay::Readable(r, lp::kAliasRowStride)) return false;
        uint16_t cls = 0;
        memcpy(&cls, r, sizeof cls);
        if (cls == classId) return true;
    }
    return false;
}

}  // namespace detail

// The space offset of the subsystem whose record table holds `classId`, or 0. The hatched
// GSPACE.ACTOR_ALIAS_OFF is one number read out of one build; a live actor of a known class is a
// fact about the table itself, so when the hatched offset disagrees this says which offset the
// engine is actually using rather than leaving a wrong table rendered as if it were right.
inline uint32_t FindTableOffset(const void* gameSpace, uint16_t classId, uint32_t* candidates) {
    // The component-system pointer block the space publishes. GSPACE.ACTOR_ALIAS_OFF (3144) and its
    // ncaActorJuggler neighbour (3152) sit inside this band on the build the offsets were read from.
    constexpr uint32_t kScanLo = 2048u, kScanHi = 4608u;
    uint32_t seen = 0;
    QueriedSpans spans;
    for (uint32_t off = kScanLo; off <= kScanHi; off += 8) {
        detail::Table t;
        if (!detail::ReadTable(detail::Deref(gameSpace, off, &spans), t, &spans)) continue;
        ++seen;
        if (detail::HoldsClass(t, classId)) {
            if (candidates != nullptr) *candidates = seen;
            return off;
        }
    }
    if (candidates != nullptr) *candidates = seen;
    return 0;
}

// The record table that holds `classId`, and the space offset it was found at. The hatched offset
// is tried first and the scan is what settles it.
//
// OWED. This and FindTableOffset still reach the store through `detail::ReadTable`
// -- the fixed +80/+88 row/count offsets that ENG-RELOAD-5 §1.2 measured are right only for the
// 0xC0 systems, so on any other subsystem it reads whatever sits there. `detail::ReadStore`, which
// decodes each system's own +640 thunk, is the correct reader and is what RepairLaggingRecords and
// the reload census already use. Moving these two onto it also needs the executable-memory thunk
// fixture the reload5 cases build, in place of the planted +3144 table the steer cases plant now.
inline bool AcquireTable(const void* gameSpace, uint16_t classId, detail::Table& table,
                         uint32_t& offset) {
    offset = replay::localpawn_facts::kSpaceActorAliasOff;
    if (detail::ReadTable(detail::Deref(gameSpace, offset), table) &&
        detail::HoldsClass(table, classId))
        return true;
    const uint32_t hit = FindTableOffset(gameSpace, classId, nullptr);
    if (hit == 0) return false;
    offset = hit;
    return detail::ReadTable(detail::Deref(gameSpace, offset), table);
}

// The engine's own final test, spelled as the exact match sub_140215DC0 makes on all three words
// of the key (R4/R5/R6 of RE-RELOAD-ENTRY5). Walked rather than binary-searched: the comparator
// the engine sorts with orders on the record's space id as well, and a linear pass over a few
// hundred rows off a frame path needs none of that to answer "is this handle in the table".
inline bool Resolves(const detail::Table& table, uint16_t classId, uint16_t index,
                     uint16_t generation) {
    namespace lp = replay::localpawn_facts;
    for (uint64_t i = 0; i < table.count; ++i) {
        const uint8_t* const r = table.rows + lp::kAliasRowStride * i;
        if (!replay::Readable(r, lp::kAliasRowStride)) return false;
        uint16_t cls = 0, idx = 0, gen = 0;
        memcpy(&cls, r, sizeof cls);
        memcpy(&idx, r + 2, sizeof idx);
        memcpy(&gen, r + 4, sizeof gen);
        if (cls == classId && idx == index && gen == generation) return true;
    }
    return false;
}

// The highest generation any row of `classId`/`index` carries, or 0xFFFF when the class/index has no
// row at all.  "No row" and "a row a generation behind" are different faults with the same symptom,
// and only the second one is repairable by a re-key.
inline uint16_t HighestRowGeneration(const detail::Table& table, uint16_t classId, uint16_t index) {
    namespace lp = replay::localpawn_facts;
    uint16_t best = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this function's own "no row seen" u16 seed, not the engine's netvar slot field
    bool any = false;
    for (uint64_t i = 0; i < table.count; ++i) {
        const uint8_t* const r = table.rows + lp::kAliasRowStride * i;
        if (!replay::Readable(r, lp::kAliasRowStride)) break;
        uint16_t cls = 0, idx = 0, gen = 0;
        memcpy(&cls, r, sizeof cls);
        memcpy(&idx, r + 2, sizeof idx);
        memcpy(&gen, r + 4, sizeof gen);
        if (cls != classId || idx != index) continue;
        if (!any || gen > best) { best = gen; any = true; }
    }
    return any ? best : uint16_t(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this function's own "no such row" answer, documented at :223, not the engine's netvar slot field
}

// The actor guid key the engine packs into one qword: class, index, generation, low to high.
inline uint64_t ActorKey(uint16_t classId, uint16_t index, uint16_t generation) {
    return uint64_t(classId) | (uint64_t(index) << 16) | (uint64_t(generation) << 32);
}

enum class LagResult : uint8_t {
    NoPool,    // the class publishes no pool
    NoTable,   // nothing in this object keeps a record table for the class
    InSync,    // every allocatable entry's row is at the generation the pool would stamp
    Lagging,   // at least one entry's newest row is behind the pool's generation counter
};

struct LagReport {
    LagResult result = LagResult::NoPool;
    uint32_t tables = 0;    // subsystem record tables that carry the class
    uint32_t lagging = 0;   // (table, entry) pairs whose row generation was behind the pool's
    uint16_t firstIndex = 0xFFFF;    // facts-ok: NETVAR.SLOT_NONE this report's own "no lagging entry recorded" marker, not the engine's netvar slot field
    uint16_t firstRowGen = 0xFFFF;   // facts-ok: NETVAR.SLOT_NONE this report's own "no lagging entry recorded" marker, not the engine's netvar slot field
    uint16_t firstPoolGen = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this report's own "no lagging entry recorded" marker, not the engine's netvar slot field
};

// HOW MANY POOLED-ACTOR RECORDS THE FREE'S OWN RE-KEY LEFT BEHIND.  A READING; it writes nothing.
//
// The unload's engine free advances the pool's generation counter for every entry it releases and
// then broadcasts the old guid to each component system's +640 re-key.  Under the bottom-up burst
// that ENG-RELOAD-5 replaced, exactly one entry came back with its record still at the old
// generation, in six stores at once; under the top-down one-per-pass leave it is zero on every
// unload.  So this is the instrument that says the fix is still holding, and it is the only correct
// reading of these stores anyone has taken.
//
// It is NOT a repair.  Carrying the lagging record forward with the engine's own re-key -- F-A, the
// RE holder's top pick -- was built, measured twice, and killed the process at the UNLOAD both
// times: a live consumer still holds the OLD key (r14 = {class 59, entry 5, generation ZERO} in
// launch g1), so the record cannot satisfy both holders and moving it breaks the one that is still
// there.  ENG-STABILIZE-1 removed the write arm; the reading stays.
inline LagReport ReportLaggingRecords(void* space, const detail::Pool& pool, uint16_t classId) {
    LagReport rep;
    if (space == nullptr || pool.gens == nullptr || pool.capacity == 0) return rep;
    rep.result = LagResult::NoTable;

    constexpr uint32_t kScanLo = 2048u, kScanHi = 4608u;
    QueriedSpans spans;
    for (uint32_t off = kScanLo; off <= kScanHi; off += 8) {
        void* const system = const_cast<void*>(detail::Deref(space, off, &spans));
        detail::Table table;
        if (!detail::ReadStore(system, table, &spans) || !detail::HoldsClass(table, classId)) continue;
        ++rep.tables;
        if (rep.result == LagResult::NoTable) rep.result = LagResult::InSync;

        for (uint16_t idx = 0; idx < pool.capacity; ++idx) {
            const uint16_t poolGen = pool.gens[idx];
            if (Resolves(table, classId, idx, poolGen)) continue;
            const uint16_t rowGen = HighestRowGeneration(table, classId, idx);
            if (rowGen == 0xFFFFu || rowGen >= poolGen) continue;  // absent, or not a lag  facts-ok: NETVAR.SLOT_NONE HighestRowGeneration's own "no such row" answer, not the engine's netvar slot field
            ++rep.lagging;
            rep.result = LagResult::Lagging;
            if (rep.firstIndex == 0xFFFFu) {  // facts-ok: NETVAR.SLOT_NONE LagReport's own "nothing recorded yet" marker from :259, not the engine's netvar slot field
                rep.firstIndex = idx;
                rep.firstRowGen = rowGen;
                rep.firstPoolGen = poolGen;
            }
        }
    }
    return rep;
}

inline const char* LagResultName(LagResult r) {
    switch (r) {
        case LagResult::NoPool: return "no-pool";
        case LagResult::NoTable: return "no-table";
        case LagResult::InSync: return "in-sync";
        case LagResult::Lagging: return "lagging";
    }
    return "?";
}

enum class SteerResult : uint8_t {
    NoPool,      // the class publishes no pool, or its free stack could not be read
    NoTable,     // no record table in this space holds the class at all
    Resolvable,    // the entry the allocation will pop has a row at the generation it will be stamped
    Unresolvable,  // it does not, and the handle the engine stamps for it will resolve to nothing
};

struct Steer {
    SteerResult result = SteerResult::NoPool;
    uint16_t    index = 0xFFFF;        // the entry the next allocation would pop  facts-ok: NETVAR.SLOT_NONE this struct's own "no entry read" marker, not the engine's netvar slot field
    uint16_t    generation = 0xFFFF;   // the generation the pool would stamp on it  facts-ok: NETVAR.SLOT_NONE this struct's own "no generation read" marker, not the engine's netvar slot field
    uint32_t    freeCount = 0;         // the free count it read
};

// ASK THE ENGINE'S OWN QUESTION ABOUT THE ENTRY THE NEXT ALLOCATION WILL POP.  A READING; it writes
// nothing.
//
// A pooled actor's registry record is re-keyed in place by the free (sub_140216620, broadcast slot
// +640 of sub_1404F1C60), while the same free advances the pool's own generation counter
// unconditionally.  Under the removal order ENG-RELOAD-4 measured, entry 5 of `remoteplayer_arena`
// came back with genTable=1 and its row still at generation 0: the next allocation stamps
// {class,5,1}, the resolve inside sub_140CB89B0 finds nothing, and the engine dereferences the null.
//
// ENG-RELOAD-5's paced top-down leave makes the free's own re-key land for every entry, and this has
// answered `Resolvable` on every admission of every launch since.  It is kept as the LAST LINE OF
// DEFENCE and nothing more: the retirement arm ENG-RELOAD-4 shipped had nothing left to retire, and
// an unasked-for write into the engine's free stack is not what a check that never fires should be
// holding.  An unresolvable entry is now reported, loudly, and the admission proceeds -- which is
// what it did anyway, since the retirement never ran.
inline Steer SteerAwayFromUnresolvable(void* gameSpace, uint16_t classId) {
    namespace lp = replay::localpawn_facts;
    Steer s;
    if (gameSpace == nullptr) return s;

    detail::Pool pool;
    if (!detail::ReadPool(gameSpace, classId, pool) || pool.freeCount == 0) return s;
    const uint16_t capacity = pool.capacity;
    const uint64_t freeCount = pool.freeCount;
    const uint16_t* const gens = pool.gens;
    uint16_t* const stack = pool.stack;
    void* const desc = pool.desc;
    s.freeCount = uint32_t(freeCount);
    if (!replay::Readable(stack, sizeof(uint16_t) * size_t(freeCount))) return s;

    detail::Table table;
    uint32_t offset = 0;
    if (!AcquireTable(gameSpace, classId, table, offset)) {
        s.result = SteerResult::NoTable;
        return s;
    }

    const uint16_t top = stack[freeCount - 1];
    const uint16_t gen = top < capacity ? gens[top] : uint16_t(0xFFFF);  // facts-ok: NETVAR.SLOT_NONE this reader's own "generation unreadable" marker for an out-of-range top, not the engine's netvar slot field
    s.index = top;
    s.generation = gen;
    s.result = top < capacity && Resolves(table, classId, top, gen) ? SteerResult::Resolvable
                                                                   : SteerResult::Unresolvable;
    return s;
}

inline const char* SteerResultName(SteerResult r) {
    switch (r) {
        case SteerResult::NoPool: return "no-pool";
        case SteerResult::NoTable: return "no-table";
        case SteerResult::Resolvable: return "resolvable";
        case SteerResult::Unresolvable: return "unresolvable";
    }
    return "?";
}

// How many subsystem tables and rows one object contributes to the reload line.  Display bounds.
inline constexpr uint32_t kTablesShown = 4u;
inline constexpr uint32_t kRowsShown   = 24u;

// Every subsystem table of `object` that carries a record of `classId`, named by the offset it sits
// at, with each row's generation, local index and SPACE ID -- the qword the engine's own lookup
// orders on beside the key, and the one field of entry 5's record nobody had read.  Returns the
// number of tables rendered.
inline uint32_t DescribeTablesOf(const void* object, const char* label, uint16_t classId,
                                 detail::Line& line, QueriedSpans& spans) {
    namespace lp = replay::localpawn_facts;
    if (object == nullptr) {
        line.Put(" %s=<null>", label);
        return 0;
    }
    constexpr uint32_t kScanLo = 2048u, kScanHi = 4608u;
    uint32_t tables = 0, systems = 0;
    for (uint32_t off = kScanLo; off <= kScanHi && tables < kTablesShown; off += 8) {
        detail::Table t;
        if (!detail::ReadStore(detail::Deref(object, off, &spans), t, &spans)) continue;
        ++systems;
        if (!detail::HoldsClass(t, classId)) continue;
        ++tables;
        line.Put(" %s@%u+%u n=%llu:", label, unsigned(off), unsigned(t.storeOff),
                 (unsigned long long)t.count);
        uint32_t shown = 0, held = 0;
        for (uint64_t i = 0; i < t.count; ++i) {
            const uint8_t* const r = t.rows + lp::kAliasRowStride * i;
            if (!replay::Readable(r, lp::kAliasRowStride)) break;
            uint16_t cls = 0, idx = 0, gen = 0, loc = 0;
            uint64_t sid = 0;
            memcpy(&cls, r, sizeof cls);
            if (cls != classId) continue;
            ++held;
            if (shown >= kRowsShown) continue;
            memcpy(&idx, r + 2, sizeof idx);
            memcpy(&gen, r + 4, sizeof gen);
            memcpy(&sid, r + 8, sizeof sid);
            memcpy(&loc, r + 16, sizeof loc);
            ++shown;
            line.Put("[%u g%u l%u s%08X]", unsigned(idx), unsigned(gen), unsigned(loc),
                     unsigned(sid & 0xFFFFFFFFull));  // facts-ok: GOAL.USERID_UNSET a low-32 mask narrowing a u64 id for the %08X log field, not the unset-user sentinel
        }
        line.Put("(%u)", unsigned(held));
    }
    if (tables == 0) line.Put(" %s=<no class-%u store in %u system(s)>", label, unsigned(classId),
                              unsigned(systems));
    return tables;
}

// ENG-RELOAD-5's measurement, one log line.  The child game space the plugin holds
// (*(ng+NETGAME.CS_SPACE_OFF)) is NOT where the engine's actor resolve reads: every site in the
// image hands the lookup *(ng+8).  RELOAD-4's reader found its table by content and settled on a
// subsystem of the child, so both objects are rendered here, each with the offset it was found at,
// and the pool's own counters once at the end.  Read-only.
inline void DescribeReload(const void* root, const void* child, uint16_t classId, char* out,
                           size_t cap) {
    detail::Line line{out, cap};
    if (out != nullptr && cap != 0) out[0] = '\0';
    line.Put("cls=%u", unsigned(classId));
    QueriedSpans spans;
    DescribeTablesOf(root, "root", classId, line, spans);
    DescribeTablesOf(child, "child", classId, line, spans);

    detail::Pool pool;
    if (detail::ReadPool(child, classId, pool)) {
        const uint32_t show = pool.capacity < kEntriesShown ? pool.capacity : kEntriesShown;
        line.Put(" | free=%llu/%u genTable=", (unsigned long long)pool.freeCount,
                 unsigned(pool.capacity));
        for (uint32_t k = 0; k < show; ++k) line.Put("%u,", unsigned(pool.gens[k]));
    } else {
        line.Put(" | <no pool>");
    }
}

// Renders every record of `classId` in the space's alias table, followed by the two numbers those
// records only mean anything against: the class pool's own generation table (advanced by the free
// unconditionally, where the re-key above it can silently do nothing) and sub_1404ECD40's sticky
// per-(class, index) mark-for-destroy byte, whose already-set arm skips the whole destroy
// broadcast.  Returns how many records of that class were found.  `out` is always NUL-terminated.
inline uint32_t Describe(const void* gameSpace, uint16_t classId, char* out, size_t cap) {
    namespace lp = replay::localpawn_facts;
    detail::Line line{out, cap};
    if (out != nullptr && cap != 0) out[0] = '\0';
    if (gameSpace == nullptr) { line.Put("no game space"); return 0; }

    detail::Table table;
    uint32_t offset = 0;
    const bool have = AcquireTable(gameSpace, classId, table, offset);
    line.Put("cls=%u @space+%u n=%llu rows:", unsigned(classId), unsigned(offset),
             have ? (unsigned long long)table.count : 0ull);
    uint32_t found = 0;
    if (!have) {
        line.Put(" <table empty or out of band>");
    } else {
        for (uint64_t i = 0; i < table.count; ++i) {
            const uint8_t* const r = table.rows + lp::kAliasRowStride * i;
            if (!replay::Readable(r, lp::kAliasRowStride)) {
                line.Put(" <row %llu unreadable>", (unsigned long long)i);
                break;
            }
            uint16_t cls = 0, idx = 0, gen = 0, loc = 0;
            memcpy(&cls, r, sizeof cls);
            memcpy(&idx, r + 2, sizeof idx);
            memcpy(&gen, r + 4, sizeof gen);
            memcpy(&loc, r + 16, sizeof loc);
            if (cls != classId) continue;
            ++found;
            line.Put(" [%u g%u l%u]", unsigned(idx), unsigned(gen), unsigned(loc));
        }
        if (found == 0) line.Put(" <none of this class>");
    }

    const void* const poolTable = detail::Deref(gameSpace, lp::kSpacePoolTableOff);
    const void* const desc = detail::At(poolTable, lp::kPoolDescStride * classId);
    if (poolTable != nullptr && replay::Readable(desc, lp::kPoolDescCapOff + sizeof(uint16_t))) {
        uint16_t capacity = 0;
        uint64_t freeSlots = 0;
        memcpy(&capacity, detail::At(desc, lp::kPoolDescCapOff), sizeof capacity);
        memcpy(&freeSlots, detail::At(desc, lp::kPoolDescFreeOff), sizeof freeSlots);
        const uint32_t show = capacity < kEntriesShown ? capacity : kEntriesShown;
        line.Put(" | free=%llu/%u genTable=", (unsigned long long)freeSlots, unsigned(capacity));
        const uint16_t* const gens =
            static_cast<const uint16_t*>(detail::Deref(desc, lp::kPoolDescGenTableOff));
        for (uint32_t k = 0; k < show; ++k) {
            if (gens != nullptr && replay::Readable(gens + k, sizeof(uint16_t)))
                line.Put("%u,", unsigned(gens[k]));
            else
                line.Put("?,");
        }
        const uint8_t* const dead = static_cast<const uint8_t*>(detail::Deref(
            gameSpace, size_t(lp::kSpaceDestroyFlagTableOff) + lp::kSpaceDestroyFlagStride * classId));
        line.Put(" destroyFlag=");
        for (uint32_t k = 0; k < show; ++k) {
            if (dead != nullptr && replay::Readable(dead + k, sizeof(uint8_t)))
                line.Put("%u,", unsigned(dead[k]));
            else
                line.Put("?,");
        }
    }
    return found;
}

}  // namespace replay::actoralias
