// The shared table check, driven with the states the engine actually produces and the one it was
// written for: a space whose table header is garbage that happens to look plausible. Every refusal leg
// has a case that goes red when the leg is removed, and the two legitimately-empty states each have a
// near-miss neighbour so "empty" cannot be answered by a null test.

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include <doctest.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/tableguard.h"

namespace {

using namespace replay::tableguard;

// What the engine's append reaches through: an object whose first qword is a vtable of at least
// eight slots (sub_1400D8D40 calls +16, +40 and +56).
void* g_vtable[8] = {};
struct FakeAllocator {
    void* vtable;
};
FakeAllocator g_allocator{g_vtable};

uint64_t Addr(const void* p) { return reinterpret_cast<uint64_t>(p); }
uint64_t Alloc() { return Addr(&g_allocator); }

// Reserved, never committed: unreadable by construction, unlike a guessed address.
uint64_t Uncommitted() {
    static void* reserved = VirtualAlloc(nullptr, 1u << 20, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(reserved != nullptr);
    return Addr(reserved);
}

// A two-page reservation whose FIRST page is committed and whose second is committed only when asked.
// A block placed to end past the first page is readable at its start and unreadable at its end, which is
// what separates a probe of the whole span from a probe of the first byte or of the installed prefix.
struct TwoPages {
    void* region = nullptr;
    uint64_t pageBytes = 0;

    explicit TwoPages(bool commitTail) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        pageBytes = si.dwPageSize;
        region = VirtualAlloc(nullptr, static_cast<size_t>(2 * pageBytes), MEM_RESERVE, PAGE_NOACCESS);
        REQUIRE(region != nullptr);
        const size_t committed = static_cast<size_t>(commitTail ? 2 * pageBytes : pageBytes);
        REQUIRE(VirtualAlloc(region, committed, MEM_COMMIT, PAGE_READWRITE) != nullptr);
    }
    ~TwoPages() { VirtualFree(region, 0, MEM_RELEASE); }

    TwoPages(const TwoPages&) = delete;
    TwoPages& operator=(const TwoPages&) = delete;

    uint64_t FirstPageEnd() const { return Addr(region) + pageBytes; }
};

const uint32_t kCsTableOff = 0x420u;  // the fixture's choice; the product's comes from W2-F's fact row
const uint64_t kCsStride = 16u;
const uint64_t kRecordStride = 72u;

void PutQword(std::vector<uint8_t>& mem, size_t off, uint64_t v) {
    REQUIRE(off + sizeof v <= mem.size());
    std::memcpy(mem.data() + off, &v, sizeof v);
}

struct Space {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x2000, 0xCD);
    std::vector<uint8_t> block;

    void Plant(uint32_t tableOff, const TableRaw& r) {
        PutQword(mem, tableOff + kBaseOff, r.base);
        PutQword(mem, tableOff + kBytesOff, r.bytes);
        PutQword(mem, tableOff + kAllocatorOff, r.allocator);
        PutQword(mem, tableOff + kGrowOff, r.grow);
        PutQword(mem, tableOff + kCapacityOff, r.capacity);
        PutQword(mem, tableOff + kCountOff, r.count);
        PutQword(mem, tableOff + kFlagsOff, r.flags);
    }

    // The fields exactly as CreateComponentSystems leaves them, with a real block behind the base.
    TableRaw Constructed(uint64_t capacity, uint64_t count, uint64_t stride) {
        block.assign(static_cast<size_t>(capacity * stride), 0);
        TableRaw r;
        r.base = Addr(block.data());
        r.bytes = capacity * stride;
        r.allocator = Alloc();
        r.grow = 0;
        r.capacity = capacity;
        r.count = count;
        return r;
    }
};

// Constructor_(this, 0, ...) : bytes 0, base kNoBlock; the table's own bookkeeping zeroed.
TableRaw ConstructedEmpty(uint64_t grow) {
    TableRaw r;
    r.base = kNoBlock;
    r.allocator = Alloc();
    r.grow = grow;
    return r;
}

}  // namespace

TEST_CASE("tableguard: a populated component-system table is Ready to search") {
    Space s;
    const TableRaw r = s.Constructed(8, 3, kCsStride);
    const TableCheck c = JudgeTable(r, kCsStride, false);
    CHECK(c.verdict == TableVerdict::Ready);
    CHECK(c.why == TableWhy::Ready);
    CHECK(c.raw.count == 3);
}

TEST_CASE("tableguard: the header is read at the derived offsets, each field from its own slot") {
    // Planted by literal offset, not through the header's constants, so a constant that moved moves
    // the reader and not the fixture.
    Space s;
    for (uint64_t i = 0; i < 8; ++i) {
        PutQword(s.mem, kCsTableOff + 8 * i, 0x1111 * (i + 1));
    }
    const TableCheck c = CheckSpaceTable(s.mem.data(), kCsTableOff, kCsStride, false);
    CHECK(c.raw.base == 0x1111);
    CHECK(c.raw.bytes == 0x2222);
    CHECK(c.raw.allocator == 0x3333);
    CHECK(c.raw.grow == 0x5555);
    CHECK(c.raw.capacity == 0x6666);
    CHECK(c.raw.count == 0x7777);
    CHECK(c.raw.flags == 0x8888);
    CHECK(c.verdict == TableVerdict::NotReady);
}

TEST_CASE("tableguard: the byte size is +0x08, not the allocator pointer at +0x10 (AUDIT F1)") {
    // A live block's +0x10 holds a pointer, which read as a size would be large enough for any
    // capacity: a check reading that slot never refuses. Here +0x08 says 8 bytes and +0x10 holds a
    // pointer; capacity 8 * stride 16 = 128 bytes is more than the block has.
    Space s;
    std::vector<uint8_t> block(128, 0);
    PutQword(s.mem, kCsTableOff + 0x00, Addr(block.data()));
    PutQword(s.mem, kCsTableOff + 0x08, 8);
    PutQword(s.mem, kCsTableOff + 0x10, Alloc());
    PutQword(s.mem, kCsTableOff + 0x20, 0);
    PutQword(s.mem, kCsTableOff + 0x28, 8);
    PutQword(s.mem, kCsTableOff + 0x30, 3);
    PutQword(s.mem, kCsTableOff + 0x38, 0);
    const TableCheck c = CheckSpaceTable(s.mem.data(), kCsTableOff, kCsStride, false);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::CapacityOverBlock);
}

TEST_CASE("tableguard: count above capacity is refused") {
    Space s;
    TableRaw r = s.Constructed(8, 3, kCsStride);
    r.count = 9;
    const TableCheck c = JudgeTable(r, kCsStride, false);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::CountOverCapacity);
    // Control: count == capacity is the full table, which is fine to search.
    r.count = 8;
    CHECK(JudgeTable(r, kCsStride, false).verdict == TableVerdict::Ready);
}

TEST_CASE("tableguard: a block too small for its capacity is refused") {
    Space s;
    TableRaw r = s.Constructed(8, 3, kCsStride);
    r.bytes = 8 * kCsStride - 1;
    const TableCheck c = JudgeTable(r, kCsStride, false);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::CapacityOverBlock);
    // Control: the leg is capacity * stride <= bytes, and the slack is accepted. Growth is NOT where the
    // slack comes from - sub_14016F730 reallocates to exactly 72 * (cap + grow) and then adds grow to
    // cap, so bytes == stride * capacity holds after an append grows the table exactly as it does after
    // construction. The leg stays loose because no derivation covers every path that could leave a block
    // oversized; the deferred L4 line (bytes == 16 * cap at the live CS table) is the observation that
    // would justify tightening it to equality.
    r.bytes = 8 * kCsStride + 64;
    CHECK(JudgeTable(r, kCsStride, false).verdict == TableVerdict::Ready);
}

TEST_CASE("tableguard: a capacity whose byte product overflows is refused, not wrapped to fit") {
    Space s;
    TableRaw r = s.Constructed(8, 3, kCsStride);
    r.capacity = 1ull << 60;  // * 16 wraps to exactly 0, which any block size would satisfy
    r.count = 3;
    const TableCheck c = JudgeTable(r, kCsStride, false);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::CapacityOverBlock);
}

TEST_CASE("tableguard: the constructor's empty state is Empty for a search and Ready for an append") {
    const TableRaw r = ConstructedEmpty(4);
    const TableCheck search = JudgeTable(r, kRecordStride, false);
    CHECK(search.verdict == TableVerdict::Empty);
    CHECK(search.why == TableWhy::Empty);
    const TableCheck append = JudgeTable(r, kRecordStride, true);
    CHECK(append.verdict == TableVerdict::Ready);
    CHECK(append.why == TableWhy::Ready);

    // A never-constructed but zeroed table with an allocator installed reaches the same alloc path
    // in sub_1400D8D40 (old bytes 0, so nothing is copied or freed) and is a legal target too.
    TableRaw zeroed = r;
    zeroed.base = 0;
    CHECK(JudgeTable(zeroed, kRecordStride, true).verdict == TableVerdict::Ready);
}

TEST_CASE("tableguard: an empty append target that cannot grow is refused") {
    // sub_14016F730 grows by max(grow, capacity); both zero is a zero-byte reallocation that leaves the
    // base at kNoBlock and then writes the record there.
    const TableRaw r = ConstructedEmpty(0);
    const TableCheck c = JudgeTable(r, kRecordStride, true);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::CannotGrow);
}

TEST_CASE("tableguard: an append target without a reachable allocator is refused") {
    TableRaw r = ConstructedEmpty(4);
    r.allocator = 0;
    CHECK(JudgeTable(r, kRecordStride, true).why == TableWhy::NoAllocator);
    r.allocator = Uncommitted();
    CHECK(JudgeTable(r, kRecordStride, true).why == TableWhy::NoAllocator);
    // An allocator object whose vtable pointer is unreadable is refused too: the append calls through it.
    FakeAllocator broken{reinterpret_cast<void*>(static_cast<uintptr_t>(Uncommitted()))};
    r.allocator = Addr(&broken);
    const TableCheck c = JudgeTable(r, kRecordStride, true);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::NoAllocator);
    // A search never allocates, so the same fields are merely Empty for one.
    CHECK(JudgeTable(r, kRecordStride, false).verdict == TableVerdict::Empty);
}

TEST_CASE("tableguard: a full table about to be appended to needs its allocator; searching it does not") {
    Space s;
    TableRaw r = s.Constructed(8, 8, kRecordStride);
    CHECK(JudgeTable(r, kRecordStride, true).verdict == TableVerdict::Ready);
    r.allocator = 0;
    const TableCheck c = JudgeTable(r, kRecordStride, true);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::NoAllocator);
    CHECK(JudgeTable(r, kRecordStride, false).verdict == TableVerdict::Ready);
}

TEST_CASE("tableguard: a base that says no-block beside a byte count is refused") {
    TableRaw r = ConstructedEmpty(4);
    r.bytes = 64;
    CHECK(JudgeTable(r, kRecordStride, true).why == TableWhy::NoBlock);
    r.base = 0;
    CHECK(JudgeTable(r, kRecordStride, true).why == TableWhy::NoBlock);
    CHECK(JudgeTable(r, kRecordStride, false).verdict == TableVerdict::NotReady);
}

TEST_CASE("tableguard: a real pointer to zero elements is refused") {
    Space s;
    TableRaw r = s.Constructed(8, 0, kRecordStride);
    r.capacity = 0;
    r.bytes = 0;
    const TableCheck c = JudgeTable(r, kRecordStride, true);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::NoBlock);
}

TEST_CASE("tableguard: the audit's fail-open case - count 0, garbage base, huge capacity, appending") {
    // AUDIT W2-F/AVFIX F2: the rejected fix answered Ready here and the registrar then wrote 72 bytes
    // at the base. With the header internally consistent, only the memory manager can refuse it.
    TableRaw r;
    r.base = Uncommitted();
    r.bytes = 0x100000ull * kRecordStride;
    r.allocator = Alloc();
    r.grow = 4;
    r.capacity = 0x100000;
    r.count = 0;
    const TableCheck c = JudgeTable(r, kRecordStride, true);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::ArrayUnreadable);

    // The audit's literal values: bytes 0, so the block leg refuses it before the probe does.
    TableRaw literal;
    literal.base = 0x400000002000ull;
    literal.capacity = 0x100000;
    literal.count = 0;
    const TableCheck l = JudgeTable(literal, kRecordStride, true);
    CHECK(l.verdict == TableVerdict::NotReady);
    CHECK(l.why == TableWhy::CapacityOverBlock);
}

TEST_CASE("tableguard: a populated table whose base is not mapped is the recorded crash") {
    // DIAG-CRASH launch-12: count 0x1E, base into nothing. The lookup binary-searches it.
    TableRaw r;
    r.base = Uncommitted();
    r.bytes = 0x40 * kCsStride;
    r.allocator = Alloc();
    r.capacity = 0x40;
    r.count = 0x1E;
    const TableCheck c = JudgeTable(r, kCsStride, false);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::ArrayUnreadable);
}

TEST_CASE("tableguard: the whole block must be mapped, not only the installed elements") {
    // An append writes at base + count * stride, which the installed prefix does not cover. The block
    // here STARTS in mapped memory and runs off the end of it: the two installed records fill the
    // committed page exactly, and the two the capacity claims lie in the uncommitted tail. A probe of
    // the first byte reads a committed page, and so does a probe of count * stride (the regression
    // b4f3544 shipped); only a probe of capacity * stride sees the hole.
    const uint64_t capacity = 4, count = 2;
    TableRaw r;
    r.bytes = capacity * kRecordStride;
    r.allocator = Alloc();
    r.grow = 4;
    r.capacity = capacity;
    r.count = count;

    const TwoPages split(false);
    r.base = split.FirstPageEnd() - count * kRecordStride;
    const TableCheck c = JudgeTable(r, kRecordStride, true);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::ArrayUnreadable);

    // Near miss: the same header over the same shape of block, with the tail committed too. Nothing but
    // the mapping of the last two records differs, so this leg is the only thing the case can be
    // measuring.
    const TwoPages whole(true);
    r.base = whole.FirstPageEnd() - count * kRecordStride;
    const TableCheck ok = JudgeTable(r, kRecordStride, true);
    CHECK(ok.verdict == TableVerdict::Ready);
    CHECK(ok.why == TableWhy::Ready);
}

TEST_CASE("tableguard: a constructed but empty table is Empty for a search, with a real block behind it") {
    // Near miss for the crash case above: same count of zero, but this base is mapped. A search still has
    // nothing to find; an append is fine.
    Space s;
    const TableRaw r = s.Constructed(8, 0, kCsStride);
    const TableCheck search = JudgeTable(r, kCsStride, false);
    CHECK(search.verdict == TableVerdict::Empty);
    CHECK(search.why == TableWhy::Empty);
    CHECK(JudgeTable(r, kCsStride, true).verdict == TableVerdict::Ready);
}

TEST_CASE("tableguard: a stride of zero is a question that cannot be asked") {
    Space s;
    const TableRaw r = s.Constructed(8, 3, kCsStride);
    const TableCheck c = JudgeTable(r, 0, false);
    CHECK(c.verdict == TableVerdict::Unreadable);
    CHECK(c.why == TableWhy::StrideZero);
}

TEST_CASE("tableguard: a null or unmapped space is Unreadable, never NotReady") {
    const TableCheck none = CheckSpaceTable(nullptr, kCsTableOff, kCsStride, false);
    CHECK(none.verdict == TableVerdict::Unreadable);
    CHECK(none.why == TableWhy::NoSpace);
    const void* unmapped = reinterpret_cast<const void*>(static_cast<uintptr_t>(Uncommitted()));
    const TableCheck header = CheckSpaceTable(unmapped, kCsTableOff, kCsStride, false);
    CHECK(header.verdict == TableVerdict::Unreadable);
    CHECK(header.why == TableWhy::HeaderUnreadable);
    // Control: a mapped space whose table was planted goes on to be judged.
    Space s;
    s.Plant(kCsTableOff, s.Constructed(8, 3, kCsStride));
    const TableCheck live = CheckSpaceTable(s.mem.data(), kCsTableOff, kCsStride, false);
    CHECK(live.verdict == TableVerdict::Ready);
    CHECK(live.raw.count == 3);
}

TEST_CASE("tableguard: a lookup that falls through several tables can tell Empty from NotReady") {
    // IDENT.ACTOR_COMPONENT_REF searches +0x1F50, then +0x1F90, then walks +0x1EE0. An empty first
    // table is the engine's normal path to the second; a garbage first table is the crash.
    Space s;
    s.Plant(0x1F50u, s.Constructed(16, 0, kCsStride));
    const TableCheck empty = CheckSpaceTable(s.mem.data(), 0x1F50u, kCsStride, false);
    CHECK(empty.verdict == TableVerdict::Empty);
    TableRaw garbage = ConstructedEmpty(0);
    garbage.base = Uncommitted();
    garbage.bytes = 16 * kCsStride;
    garbage.capacity = 16;
    s.Plant(0x1F90u, garbage);
    const TableCheck bad = CheckSpaceTable(s.mem.data(), 0x1F90u, kCsStride, false);
    CHECK(bad.verdict == TableVerdict::NotReady);
    CHECK(bad.why == TableWhy::ArrayUnreadable);
}

TEST_CASE("tableguard: the same entry covers a table at another offset with another stride") {
    // identity_access's actor tables: +0x1EE0 with 8-byte elements.
    Space s;
    s.Plant(0x1EE0u, s.Constructed(16, 5, 8));
    const TableCheck c = CheckSpaceTable(s.mem.data(), 0x1EE0u, 8, false);
    CHECK(c.verdict == TableVerdict::Ready);
    CHECK(c.raw.count == 5);
    CHECK(c.raw.bytes == 128);
}

TEST_CASE("tableguard: a default result reads as NotChecked, and every enumerator has a name") {
    const TableCheck none;
    CHECK(none.verdict == TableVerdict::NotChecked);
    CHECK(none.why == TableWhy::NotChecked);
    for (uint8_t i = 0; i <= static_cast<uint8_t>(TableWhy::ArrayUnreadable); ++i) {
        CHECK(std::string(WhyName(static_cast<TableWhy>(i))) != "<invalid>");
    }
    CHECK(std::string(WhyName(static_cast<TableWhy>(200))) == "<invalid>");
    CHECK(std::string(VerdictName(TableVerdict::Unreadable)) == "Unreadable");
    CHECK(std::string(VerdictName(TableVerdict::Empty)) == "Empty");
    CHECK(std::string(VerdictName(static_cast<TableVerdict>(200))) == "<invalid>");
}

TEST_CASE("tableguard: the log line carries the verdict, the reason and the raw fields") {
    TableRaw r;
    r.base = 0x400000002000ull;
    r.capacity = 0x100000;
    const TableCheck c = JudgeTable(r, kRecordStride, true);
    char line[256];
    const size_t n = FormatTable(c, line, sizeof line);
    const std::string s(line);
    CHECK(n == s.size());
    CHECK(s.find("NotReady/CapacityOverBlock") == 0);
    CHECK(s.find("base=400000002000") != std::string::npos);
    CHECK(s.find("cap=1048576") != std::string::npos);
    char tiny[8];
    CHECK(FormatTable(c, tiny, sizeof tiny) == 7);
    CHECK(tiny[7] == '\0');
    CHECK(FormatTable(c, nullptr, 0) == 0);
}

TEST_CASE("tableguard: Readable refuses nothing-asked, the unmapped, and a span that wraps") {
    uint8_t local[64] = {};
    CHECK(replay::Readable(local, sizeof local));
    CHECK_FALSE(replay::Readable(nullptr, 8));
    CHECK_FALSE(replay::Readable(local, 0));
    CHECK_FALSE(replay::Readable(reinterpret_cast<const void*>(static_cast<uintptr_t>(Uncommitted())), 8));
    // Wrapping puts the end below the start; a loop that walks from start to end then walks nothing.
    CHECK_FALSE(replay::Readable(reinterpret_cast<const void*>(~static_cast<uintptr_t>(0xFFF)), 0x2000));
}

TEST_CASE("tableguard: the page-touch probe answers about every page of the span, not just the first") {
    const size_t page = 0x1000;
    uint8_t* const region = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, page * 4, MEM_RESERVE, PAGE_NOACCESS));
    REQUIRE(region != nullptr);
    REQUIRE(VirtualAlloc(region, page * 2, MEM_COMMIT, PAGE_READWRITE) != nullptr);

    CHECK(replay::Readable(region, page * 2));
    CHECK(replay::Readable(region + page * 2 - 1, 1));
    // The third page is reserved, not committed: a probe that only looked at the first page of the
    // span would say yes here, and the caller's read would take the process down.
    CHECK_FALSE(replay::Readable(region, page * 3));
    CHECK_FALSE(replay::Readable(region + page * 2, 1));

    DWORD old = 0;
    REQUIRE(VirtualProtect(region, page, PAGE_NOACCESS, &old));
    CHECK_FALSE(replay::Readable(region, 8));
    REQUIRE(VirtualProtect(region, page, PAGE_READWRITE | PAGE_GUARD, &old));
    CHECK_FALSE(replay::Readable(region, 8));

    VirtualFree(region, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------------------------------------------
// AUDIT_F3B_CONSUMERS F3B-1: flags bit 0, the engine's in-place sort
// ---------------------------------------------------------------------------------------------------
//
// The hazard is not that the table is unsafe to READ - it is that handing it to CS.LOOKUP_BY_SYMBOL
// makes the ENGINE write to it, sorting the block in place on our tick thread, and on a stale block
// that is a write into freed memory (src/ui/csregister.cpp:280-288 refuses on exactly this; ARCH-10
// S7). Until now `tableguard` had no word for it and each consumer was expected to test `raw.flags & 1`
// itself: of the three, only `csregister` did. The rule is authored here once, so the answer is the
// same at every site.

TEST_CASE("tableguard: an unsorted table is NotReady - the engine would sort it on our thread") {
    Space s;
    TableRaw r = s.Constructed(8, 3, kCsStride);

    // The control first: the same table, same everything, bit clear. Without it "always NotReady"
    // would satisfy the case below.
    CHECK(JudgeTable(r, kCsStride, false).verdict == TableVerdict::Ready);

    r.flags = 1;
    const TableCheck c = JudgeTable(r, kCsStride, false);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::SortPending);
    CHECK(c.raw.flags == 1);

    // An append target is handed to the engine just the same.
    CHECK(JudgeTable(r, kCsStride, true).why == TableWhy::SortPending);

    // Only bit 0 is the sort bit; the rest of the field is not ours to interpret.
    r.flags = 0xFFFFFFFEu;
    CHECK(JudgeTable(r, kCsStride, false).verdict == TableVerdict::Ready);
    r.flags = 0xFFFFFFFFu;
    CHECK(JudgeTable(r, kCsStride, false).why == TableWhy::SortPending);
}

TEST_CASE("tableguard: an EMPTY table with the sort bit set is refused too") {
    // The engine's own guard is `if (!count) return 0;` and the sort is not proved to come after it,
    // so an empty table with the bit set is still a table we would be asking the engine to sort.
    // csregister's guard does not consult the count either.
    Space s;
    TableRaw searchable = s.Constructed(8, 0, kCsStride);
    CHECK(JudgeTable(searchable, kCsStride, false).verdict == TableVerdict::Empty);
    searchable.flags = 1;
    const TableCheck c = JudgeTable(searchable, kCsStride, false);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::SortPending);

    TableRaw fresh = ConstructedEmpty(4);
    CHECK(JudgeTable(fresh, kRecordStride, true).verdict == TableVerdict::Ready);
    fresh.flags = 1;
    CHECK(JudgeTable(fresh, kRecordStride, true).why == TableWhy::SortPending);
}

// The disc's game space answered NotReady for its component-system table on
// every sample of every launch, so SystemInSpace -- which searches the rows itself -- answered null
// for the one space the disc bind validates in, silently. A read that never touches the engine and
// never passes `count` is not the transaction those refusals are about.
TEST_CASE("tableguard: a read-only search is bounded by the table's own count, not by an append's rules") {
    // One Space per table: Constructed() re-assigns the one block a Space owns, so two tables taken
    // from the same Space cannot both be alive.
    Space disc;
    const TableRaw sorted = disc.Constructed(128, 68, kCsStride);

    const SearchBound ready = BoundForSearch(JudgeTable(sorted, kCsStride, false), kCsStride);
    CHECK(ready.ok);
    CHECK(ready.rows == 68);      // this space's extent, not the 99 of the space next to it
    CHECK(ready.sorted);
    CHECK_FALSE(BoundForSearch(JudgeTable(sorted, kCsStride, false), 0).ok);   // stride 0 asks nothing

    // The sort mark: still searchable, but not by bisection.
    TableRaw unsorted = sorted;
    unsorted.flags = 1;
    const TableCheck refused = JudgeTable(unsorted, kCsStride, false);
    REQUIRE(refused.why == TableWhy::SortPending);          // the control on what is being relaxed
    const SearchBound b = BoundForSearch(refused, kCsStride);
    CHECK(b.ok);
    CHECK(b.rows == 68);
    CHECK_FALSE(b.sorted);

    // A capacity that overhangs the block is an APPEND's problem: the 68 installed rows are there.
    TableRaw overhang = sorted;
    overhang.capacity = 1u << 20;
    const TableCheck over = JudgeTable(overhang, kCsStride, false);
    REQUIRE(over.why == TableWhy::CapacityOverBlock);
    const SearchBound ob = BoundForSearch(over, kCsStride);
    CHECK(ob.ok);
    CHECK(ob.rows == 68);

    // ...but a count that overhangs the committed memory is not: those rows are not there to read.
    TwoPages pages(false);
    TableRaw past;
    past.base = pages.FirstPageEnd() - kCsStride;   // one row in, the rest off the committed page
    past.bytes = 64 * kCsStride;
    past.allocator = Alloc();
    past.capacity = 64;
    past.count = 64;
    REQUIRE(JudgeTable(past, kCsStride, false).why == TableWhy::ArrayUnreadable);
    CHECK_FALSE(BoundForSearch(JudgeTable(past, kCsStride, false), kCsStride).ok);

    // Every other answer is still a refusal: nothing installed, fields that disagree, no header.
    Space empty;
    CHECK_FALSE(BoundForSearch(JudgeTable(empty.Constructed(8, 0, kCsStride), kCsStride, false),
                               kCsStride).ok);
    Space small;
    TableRaw disagree = small.Constructed(8, 3, kCsStride);
    disagree.count = 9;
    REQUIRE(JudgeTable(disagree, kCsStride, false).why == TableWhy::CountOverCapacity);
    CHECK_FALSE(BoundForSearch(JudgeTable(disagree, kCsStride, false), kCsStride).ok);
    CHECK_FALSE(BoundForSearch(CheckSpaceTable(nullptr, kCsTableOff, kCsStride, false), kCsStride).ok);
    CHECK_FALSE(BoundForSearch(TableCheck{}, kCsStride).ok);
}

TEST_CASE("tableguard: the sort bit does not mask a table that is broken for another reason") {
    // A more specific reason is worth more than this one: SortPending says "the engine would write to
    // it", the others say "it is not the shape it claims". A caller that reads SortPending and waits
    // for a sort would wait forever on a table whose capacity never fit its block.
    Space s;
    TableRaw over = s.Constructed(8, 3, kCsStride);
    over.capacity = 1u << 20;
    over.flags = 1;
    CHECK(JudgeTable(over, kCsStride, false).why == TableWhy::CapacityOverBlock);

    TableRaw counts = s.Constructed(8, 3, kCsStride);
    counts.count = 9;
    counts.flags = 1;
    CHECK(JudgeTable(counts, kCsStride, false).why == TableWhy::CountOverCapacity);

    TableRaw unreadable = s.Constructed(8, 3, kCsStride);
    unreadable.base = Uncommitted();
    unreadable.flags = 1;
    CHECK(JudgeTable(unreadable, kCsStride, false).why == TableWhy::ArrayUnreadable);

    CHECK(JudgeTable(TableRaw(), 0, false).why == TableWhy::StrideZero);
}

TEST_CASE("tableguard: the flags slot is read as the dword it is, not eight bytes wide") {
    // DIAG-F3-B section 2. Reading 8 wide pulls the NEXT field in with it. The recorded L5 header is
    // the case that proves it matters: its 8-wide value was 0x100000000, which is bit 32 of a misread
    // and bit 0 of nothing. Planted as the qword L5 actually held, the true dword is 0.
    Space s;
    const TableRaw r = s.Constructed(8, 3, kCsStride);
    s.Plant(kCsTableOff, r);
    PutQword(s.mem, kCsTableOff + kFlagsOff, 0x100000000ull);

    const TableCheck wide = CheckSpaceTable(s.mem.data(), kCsTableOff, kCsStride, false);
    CHECK(wide.raw.flags == 0);
    CHECK(wide.verdict == TableVerdict::Ready);

    // ... and the bit that IS in the field is seen, so this is not a read that returns zero.
    PutQword(s.mem, kCsTableOff + kFlagsOff, 0x100000001ull);
    const TableCheck set = CheckSpaceTable(s.mem.data(), kCsTableOff, kCsStride, false);
    CHECK(set.raw.flags == 1);
    CHECK(set.why == TableWhy::SortPending);
}

TEST_CASE("tableguard: the recorded L5 header still judges NotReady/CapacityOverBlock") {
    // The retroactive control (ARCH-10 section 4.2's row, the fields exactly as L5 logged them). The
    // sort-bit rule must not change this verdict: bit 0 of the true dword is clear, and the header is
    // refused for the reason it was always refused for.
    TableRaw l5;
    l5.base = 0x400000002000ull;
    l5.bytes = 2199023256064ull;
    l5.allocator = 0x8000000080ull;
    l5.grow = 70368744179712ull;
    l5.capacity = 54043196648849408ull;
    l5.count = 1048576ull;
    l5.flags = 0x100000000ull;   // as logged, by the 8-wide read this commit narrows
    const TableCheck c = JudgeTable(l5, kCsStride, false);
    CHECK(c.verdict == TableVerdict::NotReady);
    CHECK(c.why == TableWhy::CapacityOverBlock);

    // The same bytes through the reader that now narrows: the dword is 0, and the verdict is the same.
    Space s;
    s.Plant(kCsTableOff, l5);
    const TableCheck read = CheckSpaceTable(s.mem.data(), kCsTableOff, kCsStride, false);
    CHECK(read.raw.flags == 0);
    CHECK(read.verdict == TableVerdict::NotReady);
    CHECK(read.why == TableWhy::CapacityOverBlock);
}

TEST_CASE("tableguard: SortPending has a name, and it reaches the one-line rendering") {
    Space s;
    TableRaw r = s.Constructed(8, 3, kCsStride);
    r.flags = 1;
    char line[256];
    const size_t n = FormatTable(JudgeTable(r, kCsStride, false), line, sizeof line);
    REQUIRE(n > 0);
    const std::string text(line, n);
    CHECK(text.compare(0, 21, "NotReady/SortPending ") == 0);
    CHECK(text.find("flags=1") != std::string::npos);
    CHECK(std::string(WhyName(TableWhy::SortPending)) == "SortPending");
}
