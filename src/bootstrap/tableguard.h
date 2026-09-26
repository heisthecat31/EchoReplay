// The one place that knows what a constructed NRadEngine::CTable looks like, so that every site which
// hands a game-space table to an engine routine asks the same question before the engine indexes it.
//
// Why this exists: the engine's lookups (CS.LOOKUP_BY_SYMBOL and the actor-table searches under
// IDENT.ACTOR_COMPONENT_REF) guard on the element COUNT and then index the base pointer unguarded. The
// engine is safe because the code that constructs a table is the code that later uses it - ordering,
// not a built-flag (CS.GAMESPACE_HAS_NO_BUILT_FLAG). A plugin tick does not inherit that ordering, and
// one null-check on the space pointer is not a check on the table at all (DIAG-CRASH launch-12; audit
// W2-F/AVFIX).
//
// ★ WHAT THE OBSERVED REFUSAL ACTUALLY WAS (ARCH-10 section 5, from DIAG-F3-A/B). The refusal this
// check reported was NOT an allocated-but-unconstructed space carrying garbage. The pointer handed to
// it was `CR15NetGame+0x8`, which is the CR15Game and not a game space at all, so `+0x420` was a
// perfectly well-initialised CR15Game member: all fourteen dwords equal, in order, the runtime-lowered
// render-target / memory-budget block at `echovr.exe` .data RVA 0x020208F0. That is why
// `CapacityOverBlock` was stable across three processes and three ASLR bases - a stable graphics
// budget read as a CMemBlock header, not uninitialised heap. The CncaGameSpace is `CR15NetGame+0x2B08`
// (ARCH-10 section 1.3). The predicate below and every offset in it were CORRECT throughout
// (DIAG-F3-B section 2); only the diagnosis attached to the refusal was wrong. The check still earns
// its place: a wrong-object read is exactly what it must refuse, and it did.
//
// The predicate is the constructor's own postcondition, read back out of the space: the fields that
// CMemBlock::Constructor_ and CTable's element bookkeeping write together must still agree with each
// other and with the memory manager. Derived twice, offsets relative to the table:
//
//   libr15 (names kept)                                       PC echovr.exe
//   CMemBlock::Constructor_(this, bytes, align):               sub_1400D4B90(a1, bytes, keep):
//     this+0x08 = bytes; this+0x00 = alloc(bytes) or -1          a1[1] = bytes; *a1 = alloc(..) or -1
//     this+0x10 = CAllocator*, called through its vtable        a1[2] called via vtable +8/+40/+48/+56
//     this+0x18 = align (dword), this+0x1C |= 1                 dword a1+0x18 read as the alignment
//   CncaGameSpace::CreateComponentSystems (0x1404EA030,        sub_14016F730 (append into the table):
//   sequence at 0x1404EA696-0x1404EA6EB):
//     SetAllocator(space+0x420, *(space+0));                      grow = a1[4]      (+0x20)
//     Constructor_(space+0x420, 16*capacity, 0);                  capacity = a1[5] (+0x28)
//     space+0x448 = capacity; +0x450 = 0; +0x440 = 0              count = a1[6]    (+0x30)
//   CncaGameSpace::FindComponentSystemMT:                      sub_140105510 (CS.LOOKUP_BY_SYMBOL):
//     count at +1104 (+0x30 of 0x420), flags +1112 (+0x38),      same three reads, same order,
//     base +1056 (+0x00) indexed by 16 with no check              `if (!count) return 0;` is the only guard
//
// the W2-F fact drop row CS.TABLE_BYTES_OFF is 0x428 (= table + 0x08), corrected in b021d2c from
// 0x430, which is the allocator pointer. A check that read the allocator as the size compared a capacity
// against a pointer and never refused (audit W2-F/AVFIX F1).
//
// WHAT THIS CHECK CANNOT SEE: liveness. It proves shape and mapping - the fields agree with each other
// and the memory manager still maps the block - and a PREVIOUS occupant's postcondition satisfies both.
// A block released with HeapFree leaves its page committed and the header untouched, so a table the
// engine has already freed and handed to someone else answers Ready (audit W1-C/TABLEGUARD 3). Ready
// therefore means "safe to index", NEVER "not stale": a caller that needs the table to still be the one
// it was built for must re-read the engine's construction signal per tick, which is
// docs/spikes/CSREGISTER-EPOCH.md's job and deliberately not this check's.

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace replay {
namespace tableguard {

inline constexpr uint32_t kBaseOff      = 0x00u;  // element array, or kNoBlock after a zero-byte construction
inline constexpr uint32_t kBytesOff     = 0x08u;  // bytes the block was constructed with
inline constexpr uint32_t kAllocatorOff = 0x10u;  // CAllocator*, every (re)allocation goes through its vtable
inline constexpr uint32_t kGrowOff      = 0x20u;  // elements added when an append finds the table full
inline constexpr uint32_t kCapacityOff  = 0x28u;  // elements the block holds
inline constexpr uint32_t kCountOff     = 0x30u;  // elements installed
inline constexpr uint32_t kFlagsOff     = 0x38u;  // DWORD. bit 0: unsorted, the lookup sorts in place first
inline constexpr uint32_t kHeaderBytes  = 0x40u;

// What both constructors store as the base of a block that holds no bytes. It is not null, so a null
// test on the base never sees the engine's own empty state.
inline constexpr uint64_t kNoBlock = ~0ull;

struct TableRaw {
    uint64_t base = 0;
    uint64_t bytes = 0;
    uint64_t allocator = 0;
    uint64_t grow = 0;
    uint64_t capacity = 0;
    uint64_t count = 0;
    uint64_t flags = 0;
};

// AUDIT_F3B_CONSUMERS F3B-1. `Ready` also means the engine will not MUTATE the table when it is
// handed it: flags bit 0 says the engine's lookup sorts the block in place before searching, and on a
// plugin tick that is an O(n log n) engine write on our thread - into freed memory if the block is
// stale (src/ui/csregister.cpp:280-288 refuses on exactly that, ARCH-10 S7). That rule used to live in
// each consumer, and only one of the three had it. It lives here now, so there is ONE authoring of it:
// a table that would otherwise be Ready or Empty answers NotReady/SortPending while the bit is set.
//
// Four answers, never a bool. "Not ready" and "could not look" must never share a value: the first is
// a fact about the table, the second a fact about our own read. Empty is its own answer because what
// to do with a constructed table that holds nothing is the caller's policy, not the table's: one site
// waits for the loading window to fill it, another falls through to a second table. NotChecked is 0 so
// an unfilled result cannot read as any of them.
enum class TableVerdict : uint8_t {
    NotChecked = 0,
    Ready,        // consistent, mapped, and (for a search) holding something to find
    Empty,        // consistent, nothing installed: safe to search, and the search finds nothing
    NotReady,     // the fields disagree with each other or with the memory manager - see why
    Unreadable,   // we could not read the fields at all - see why
};

// AUDIT_F3B_CONSUMERS F3B-1. Appended, never renumbered. `SortPending` is the last value because the
// enum's numbers are already in recorded artifacts.
enum class TableWhy : uint8_t {
    NotChecked = 0,
    Ready,
    NoSpace,             // Unreadable: the space pointer is null
    HeaderUnreadable,    // Unreadable: the 0x40-byte header is not committed memory
    StrideZero,          // Unreadable: the caller asked about elements of no size
    CountOverCapacity,   // NotReady: written together by the constructor, now disagree
    CapacityOverBlock,   // NotReady: the block is too small for the elements it claims (or the product overflows)
    NoBlock,             // NotReady: base says "no block" while bytes says otherwise, or a block of zero elements
    Empty,               // Empty: constructed, nothing installed - nothing for a search to find
    NoAllocator,         // NotReady: an append may reallocate, and the allocator it would call is not there
    CannotGrow,          // NotReady: empty with grow == 0, so the engine's append computes a zero-byte block
    ArrayUnreadable,     // NotReady: capacity elements from base are not committed memory - THE observed crash
    SortPending,         // NotReady: flags bit 0 - the engine's lookup would sort this table IN PLACE
};

struct TableCheck {
    TableVerdict verdict = TableVerdict::NotChecked;
    TableWhy why = TableWhy::NotChecked;
    TableRaw raw;
};

// The entry every consumer uses. `tableOff` is the table's offset on the space (0x420 for the
// component-system table; identity's actor tables sit at +0x1EE0/+0x1F50/+0x1F90), `stride` the
// element size the engine indexes by. `emptyIsReady` is false for a table about to be SEARCHED (an empty
// one answers Empty) and true for one about to be APPENDED to (the engine's post-construction empty
// state answers Ready, provided it can grow and has an allocator to grow through).
TableCheck CheckSpaceTable(const void* space, uint32_t tableOff, uint64_t stride, bool emptyIsReady);

// The judgement on fields already read - a minidump's, a test's. Still probes the base through the
// memory manager: a header that is internally consistent says nothing about where its base points.
TableCheck JudgeTable(const TableRaw& raw, uint64_t stride, bool emptyIsReady);

// What a caller may touch when it searches the rows ITSELF, read-only, and never
// past the table's own count. Two of JudgeTable's NotReady answers are about what the ENGINE would do
// to the block and not about whether the rows are there to be read: SortPending is its lookup sorting
// in place, and ArrayUnreadable/CapacityOverBlock are the block being too small for the CAPACITY an
// append may write into. Neither happens on a read bounded by `count`, so neither is a refusal here --
// the sort mark costs the caller its bisection and nothing else. Every other verdict still refuses,
// and the bound comes from THIS table's header, so a space with 68 systems and one with 99 are each
// searched to their own extent. The base is re-probed for exactly `rows * stride` before `ok` is set.
struct SearchBound {
    uint64_t rows = 0;    // elements the caller may read
    bool sorted = false;  // false: the rows are not in key order, so a binary search would miss them
    bool ok = false;
};
SearchBound BoundForSearch(const TableCheck& check, uint64_t stride);

const char* VerdictName(TableVerdict);
const char* WhyName(TableWhy);

// One line, the same at every site: verdict, reason, and the seven raw fields as the engine holds them.
// Returns the length written (excluding the terminator), truncated to `cap`.
size_t FormatTable(const TableCheck& check, char* buf, size_t cap);

}  // namespace tableguard
}  // namespace replay
