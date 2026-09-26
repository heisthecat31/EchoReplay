#include "bootstrap/tableguard.h"

#include <stdio.h>

#include "bootstrap/buildguard.h"

namespace replay {
namespace tableguard {
namespace {

static_assert(sizeof(size_t) == 8, "the spans below are engine-sized; this is an x64-only plugin");

TableCheck Answer(const TableRaw& raw, TableVerdict verdict, TableWhy why) {
    TableCheck c;
    c.verdict = verdict;
    c.why = why;
    c.raw = raw;
    return c;
}

const void* Ptr(uint64_t address) {
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(address));
}

uint64_t ReadQword(const uint8_t* header, uint32_t off) {
    return *reinterpret_cast<const volatile uint64_t*>(header + off);
}

// DIAG-F3-B section 2: the flags slot is a dword. Reading it 8 wide pulled the next field in with it,
// which is why L5's log line says `flags=100000000` for a header whose flags are in fact 0 -- bit 32
// of the misread, not a bit of this field at all. Harmless for the bit-0 test either way; wrong in
// every artifact that prints the value, and this reader is where the value comes from.
uint64_t ReadDword(const uint8_t* header, uint32_t off) {
    return *reinterpret_cast<const volatile uint32_t*>(header + off);
}

// The engine's append calls the allocator through its vtable (slots +8/+40/+48/+56 in sub_1400D4B90 --
// ARCH-10 section 5; the earlier comment named sub_1400D8D40, a different call in the same function),
// so an append target needs an allocator object whose vtable is at least that long and readable. The
// 8-qword probe below covers every one of those slots.
bool AllocatorUsable(uint64_t allocator) {
    if (allocator == 0 || !Readable(Ptr(allocator), sizeof(uint64_t))) {
        return false;
    }
    const uint64_t vtable = *reinterpret_cast<const volatile uint64_t*>(Ptr(allocator));
    return vtable != 0 && Readable(Ptr(vtable), 8 * sizeof(uint64_t));
}

TableCheck JudgeFields(const TableRaw& raw, uint64_t stride, bool emptyIsReady) {
    if (stride == 0) {
        return Answer(raw, TableVerdict::Unreadable, TableWhy::StrideZero);
    }
    if (raw.count > raw.capacity) {
        return Answer(raw, TableVerdict::NotReady, TableWhy::CountOverCapacity);
    }
    if (raw.capacity > UINT64_MAX / stride || raw.capacity * stride > raw.bytes) {
        return Answer(raw, TableVerdict::NotReady, TableWhy::CapacityOverBlock);
    }

    if (raw.base == 0 || raw.base == kNoBlock) {
        if (raw.bytes != 0) {
            return Answer(raw, TableVerdict::NotReady, TableWhy::NoBlock);
        }
        // bytes == 0 forced capacity == 0 and count == 0 above: the constructor's empty state.
        if (!emptyIsReady) {
            return Answer(raw, TableVerdict::Empty, TableWhy::Empty);
        }
        if (!AllocatorUsable(raw.allocator)) {
            return Answer(raw, TableVerdict::NotReady, TableWhy::NoAllocator);
        }
        // sub_14016F730 grows a full table by max(grow, capacity) elements; both zero is a zero-byte
        // reallocation that leaves base at kNoBlock and then writes the element there.
        if (raw.grow == 0) {
            return Answer(raw, TableVerdict::NotReady, TableWhy::CannotGrow);
        }
        return Answer(raw, TableVerdict::Ready, TableWhy::Ready);
    }

    // A real pointer to zero elements is a state neither constructor produces.
    if (raw.capacity == 0) {
        return Answer(raw, TableVerdict::NotReady, TableWhy::NoBlock);
    }
    // Probed before any count test: a zero count with a garbage base is the unbuilt state, and an append
    // would write the first element through it.
    if (!Readable(Ptr(raw.base), static_cast<size_t>(raw.capacity * stride))) {
        return Answer(raw, TableVerdict::NotReady, TableWhy::ArrayUnreadable);
    }
    if (emptyIsReady && !AllocatorUsable(raw.allocator)) {
        return Answer(raw, TableVerdict::NotReady, TableWhy::NoAllocator);
    }
    if (raw.count == 0 && !emptyIsReady) {
        return Answer(raw, TableVerdict::Empty, TableWhy::Empty);
    }
    return Answer(raw, TableVerdict::Ready, TableWhy::Ready);
}

}  // namespace

// AUDIT_F3B_CONSUMERS F3B-1. The one authoring of the unsorted rule, so no consumer has to remember
// it: while flags bit 0 is set the engine's lookup sorts the block IN PLACE before it searches, which
// on a plugin tick is an engine write on our thread and, on a stale block, a write into freed memory
// (src/ui/csregister.cpp:280-288, ARCH-10 S7). Applied to the two dispositions that mean "hand it to
// the engine" and to nothing else: a table that is already NotReady or Unreadable keeps its own, more
// specific, reason, and a bit that is clear leaves every verdict exactly as it was.
TableCheck JudgeTable(const TableRaw& raw, uint64_t stride, bool emptyIsReady) {
    const TableCheck c = JudgeFields(raw, stride, emptyIsReady);
    const bool handedToEngine = c.verdict == TableVerdict::Ready || c.verdict == TableVerdict::Empty;
    if (handedToEngine && (raw.flags & 1u) != 0) {
        return Answer(raw, TableVerdict::NotReady, TableWhy::SortPending);
    }
    return c;
}

SearchBound BoundForSearch(const TableCheck& check, uint64_t stride) {
    SearchBound b;
    if (stride == 0) return b;
    if (check.verdict != TableVerdict::Ready) {
        // The only two objections a read-only search bounded by `count` is immune to. Everything
        // else -- the fields disagreeing, an unreadable header, a table with nothing in it -- still
        // means there is no trustworthy row to read.
        if (check.verdict != TableVerdict::NotReady) return b;
        if (check.why != TableWhy::SortPending && check.why != TableWhy::ArrayUnreadable &&
            check.why != TableWhy::CapacityOverBlock)
            return b;
    }
    const TableRaw& raw = check.raw;
    if (raw.base == 0 || raw.base == kNoBlock) return b;
    // The three verdicts above all come from past JudgeFields' count/capacity test, so this repeats
    // it. It is stated anyway because `rows` is a bound other code reads memory through, and a bound
    // must be checked where it is produced and not inferred from how the caller got here.
    if (raw.count == 0 || raw.count > raw.capacity) return b;
    if (raw.count > UINT64_MAX / stride) return b;
    const uint64_t span = raw.count * stride;
    if (!Readable(Ptr(raw.base), static_cast<size_t>(span))) return b;
    b.rows = raw.count;
    b.sorted = (raw.flags & 1u) == 0;
    b.ok = true;
    return b;
}

TableCheck CheckSpaceTable(const void* space, uint32_t tableOff, uint64_t stride, bool emptyIsReady) {
    TableRaw raw;
    if (space == nullptr) {
        return Answer(raw, TableVerdict::Unreadable, TableWhy::NoSpace);
    }
    const uint8_t* header = static_cast<const uint8_t*>(space) + tableOff;
    if (!Readable(header, kHeaderBytes)) {
        return Answer(raw, TableVerdict::Unreadable, TableWhy::HeaderUnreadable);
    }
    raw.base = ReadQword(header, kBaseOff);
    raw.bytes = ReadQword(header, kBytesOff);
    raw.allocator = ReadQword(header, kAllocatorOff);
    raw.grow = ReadQword(header, kGrowOff);
    raw.capacity = ReadQword(header, kCapacityOff);
    raw.count = ReadQword(header, kCountOff);
    raw.flags = ReadDword(header, kFlagsOff);
    return JudgeTable(raw, stride, emptyIsReady);
}

const char* VerdictName(TableVerdict v) {
    switch (v) {
        case TableVerdict::NotChecked: return "NotChecked";
        case TableVerdict::Ready:      return "Ready";
        case TableVerdict::Empty:      return "Empty";
        case TableVerdict::NotReady:   return "NotReady";
        case TableVerdict::Unreadable: return "Unreadable";
    }
    return "<invalid>";
}

const char* WhyName(TableWhy w) {
    switch (w) {
        case TableWhy::NotChecked:        return "NotChecked";
        case TableWhy::Ready:             return "Ready";
        case TableWhy::NoSpace:           return "NoSpace";
        case TableWhy::HeaderUnreadable:  return "HeaderUnreadable";
        case TableWhy::StrideZero:        return "StrideZero";
        case TableWhy::CountOverCapacity: return "CountOverCapacity";
        case TableWhy::CapacityOverBlock: return "CapacityOverBlock";
        case TableWhy::NoBlock:           return "NoBlock";
        case TableWhy::Empty:             return "Empty";
        case TableWhy::NoAllocator:       return "NoAllocator";
        case TableWhy::CannotGrow:        return "CannotGrow";
        case TableWhy::ArrayUnreadable:   return "ArrayUnreadable";
        case TableWhy::SortPending:       return "SortPending";
    }
    return "<invalid>";
}

size_t FormatTable(const TableCheck& c, char* buf, size_t cap) {
    if (buf == nullptr || cap == 0) {
        return 0;
    }
    const int n = snprintf(buf, cap,
                           "%s/%s base=%llx bytes=%llu alloc=%llx grow=%llu cap=%llu count=%llu flags=%llx",
                           VerdictName(c.verdict), WhyName(c.why),
                           static_cast<unsigned long long>(c.raw.base),
                           static_cast<unsigned long long>(c.raw.bytes),
                           static_cast<unsigned long long>(c.raw.allocator),
                           static_cast<unsigned long long>(c.raw.grow),
                           static_cast<unsigned long long>(c.raw.capacity),
                           static_cast<unsigned long long>(c.raw.count),
                           static_cast<unsigned long long>(c.raw.flags));
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

}  // namespace tableguard
}  // namespace replay
