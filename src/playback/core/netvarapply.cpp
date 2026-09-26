#include "playback/core/netvarapply.h"

#include <windows.h>

#include <string.h>
#include "bootstrap/pageprobe.h"

namespace replay {
namespace playback {
namespace {

bool Committed(const void* p, size_t bytes) { return PageSpanReadable(p, bytes); }

const uint8_t* At(void* obj, uint32_t off) {
    return reinterpret_cast<const uint8_t*>(obj) + off;
}

// {u32 rowset_seq, u16 var_count}: `sub_140F8BCB0` reads `*a2` and `*((_WORD*)a2 + 2)`, then walks
// rows from `(char*)a2 + 6` in 16-byte steps (`v49 = v8 + 2`).
constexpr size_t kPayloadHeaderBytes = 6;
// capture::NetvarMessage::kMaxRows. The biggest real rowset is the 56-row slot-0 baseline.
constexpr size_t kMaxRowsPerCall = 128;

// The decoder's fourth argument, in the layout its own body indexes: base at +0x00, a flag word at
// +0x1C that it ORs 8 into, two capacities at +0x20/+0x28 that the grow path compares against, and
// the count at +0x30 that it post-increments per appended name.
struct ChangedNames {
    uint64_t* base;
    uint64_t reserved08;
    uint64_t reserved10;
    uint32_t reserved18;
    uint32_t flags;
    uint64_t cap_a;
    uint64_t cap_b;
    uint64_t count;
};
static_assert(sizeof(ChangedNames) == 0x38, "the decoder indexes this by absolute offset");

}  // namespace

const char* NetVarApplyRefusalName(NetVarApplyStatus s) {
    switch (s) {
        case NetVarApplyStatus::NotAttempted:
        case NetVarApplyStatus::Applied:
            return nullptr;
        case NetVarApplyStatus::NoTable: return "netvars-no-table";
        case NetVarApplyStatus::OwnerUnresolvable: return "netvars-owner-unresolvable";
        case NetVarApplyStatus::OwnerMismatch: return "netvars-owner-mismatch";
        case NetVarApplyStatus::SlotInvalid: return "netvars-slot-invalid";
        case NetVarApplyStatus::TableUnsorted: return "netvars-table-unsorted";
        case NetVarApplyStatus::TableNotReady: return "netvars-table-not-ready";
        case NetVarApplyStatus::SchemaMismatch: return "netvars-schema-mismatch";
        case NetVarApplyStatus::DispatchUnbound: return "netvars-dispatch-unbound";
        case NetVarApplyStatus::RowRejected: return "netvars-row-rejected";
    }
    return "netvars-status-unknown";
}

const char* NetVarApplyText(NetVarApplyStatus s) {
    switch (s) {
        case NetVarApplyStatus::NotAttempted: return "no netvar rowset has been offered";
        case NetVarApplyStatus::Applied: return "applied";
        case NetVarApplyStatus::NoTable: return "no engine netvar table is bound to this playback";
        case NetVarApplyStatus::OwnerUnresolvable:
            return "the loaded level id could not be read, so the engine's owner check cannot be made";
        case NetVarApplyStatus::OwnerMismatch:
            return "this capture's netvars belong to a different level than the one loaded";
        case NetVarApplyStatus::SlotInvalid: return "the record carries the engine's no-slot value";
        case NetVarApplyStatus::TableUnsorted:
            return "the update-record table is flagged unsorted; the engine's lookup would sort it in place";
        case NetVarApplyStatus::TableNotReady:
            return "the component's pool, the slot's record or its store could not be read";
        case NetVarApplyStatus::SchemaMismatch:
            return "this level has no update record for that netvar slot";
        case NetVarApplyStatus::DispatchUnbound:
            return "every precondition held; the engine's netvar receive entry is not bound";
        case NetVarApplyStatus::RowRejected: return "the table refused a row";
    }
    return "status not stated";
}

NetVarApplyStatus ApplyRowset(INetVarTable* table, uint64_t owner, uint16_t slot,
                              const NetVarApplyRow* rows, size_t count, NetVarApplyStats& st) {
    st.rows_offered += static_cast<uint32_t>(count);

    auto refuse = [&](NetVarApplyStatus s) {
        st.rows_refused += static_cast<uint32_t>(count);
        ++st.rowsets_refused;
        st.last_refusal = s;
        return s;
    };

    if (!table) return refuse(NetVarApplyStatus::NoTable);

    // 140D3DFAD/140D3DFBC. The engine drops a message whose body[0x00] is not the loaded level, and
    // so do we: writing another level's scores into this one is the failure mode, not the safeguard.
    uint64_t live = 0;
    if (!table->OwnerId(live)) return refuse(NetVarApplyStatus::OwnerUnresolvable);
    if (live != owner) return refuse(NetVarApplyStatus::OwnerMismatch);

    // 140D3DFE5. Checked before any table read, in that order, because the index walk below reads
    // [level+0xD0 + 4*slot] and 0xFFFF would index 256 KB past the array.
    if (slot == kNetVarSlotNone) return refuse(NetVarApplyStatus::SlotInvalid);

    NetVarRecord rec;
    const NetVarApplyStatus resolved = table->ResolveSlot(slot, rec);
    if (resolved != NetVarApplyStatus::Applied) return refuse(resolved);

    uint32_t written = 0;
    const NetVarApplyStatus wrote = table->ApplyRows(rec, rows, count, written);
    st.rows_written += written;
    if (wrote != NetVarApplyStatus::Applied) {
        // Partial is real: the engine's decoder stops where the payload stops. Rows that landed are
        // counted as landed, and only the remainder is refused -- a refusal that swallowed the whole
        // rowset would understate what is now in the engine.
        const uint32_t remainder = static_cast<uint32_t>(count) - written;
        st.rows_refused += remainder;
        ++st.rowsets_refused;
        st.last_refusal = wrote;
        return wrote;
    }

    // 140D3E080, after the decode and whether or not anything changed.
    table->MarkInitialVarsSeen(rec);
    ++st.rowsets_applied;
    return NetVarApplyStatus::Applied;
}

bool EngineNetVarTable::OwnerId(uint64_t& out) const {
    if (!level_) return false;
    const uint8_t* p = At(level_, kNetVarOwnerPtrOff);
    if (!Committed(p, sizeof(void*))) return false;
    void* ownerObj = nullptr;
    memcpy(&ownerObj, p, sizeof ownerObj);
    if (!ownerObj) return false;
    const uint8_t* q = At(ownerObj, kNetVarOwnerIdOff);
    if (!Committed(q, sizeof(uint64_t))) return false;
    memcpy(&out, q, sizeof out);
    return true;
}

const char* NetVarPoolStepText(NetVarPoolStep s) {
    switch (s) {
        case NetVarPoolStep::Walked:            return "walked";
        case NetVarPoolStep::NoComponent:       return "no component";
        case NetVarPoolStep::HeaderUnreadable:  return "the pool members at cs+0xD0..0x108 are unreadable";
        case NetVarPoolStep::SlotOverCapacity:  return "the slot is past the pool's capacity";
        case NetVarPoolStep::SlotMapUnreadable: return "the slot map entry is unreadable";
        case NetVarPoolStep::SlotUnmapped:      return "the slot map holds 0xFFFF for the slot";
        case NetVarPoolStep::IndexOverCount:    return "the slot's record index is not below the record count";
        case NetVarPoolStep::RecordUnreadable:  return "the slot's 48-byte record is unreadable";
        case NetVarPoolStep::StoreUnreadable:   return "the record's store pointer is null or unreadable";
    }
    return "unstated";
}

NetVarPoolWalk WalkNetVarPool(void* level, uint16_t slot) {
    NetVarPoolWalk w;
    if (!level) return w;

    w.step = NetVarPoolStep::HeaderUnreadable;
    if (!Committed(At(level, kNetVarSlotIndexOff),
                   kNetVarRecordTblOff + sizeof(void*) - kNetVarSlotIndexOff)) {
        return w;
    }
    memcpy(&w.slotMap, At(level, kNetVarSlotIndexOff), sizeof w.slotMap);
    memcpy(&w.capacity, At(level, kNetVarPoolCapacityOff), sizeof w.capacity);
    memcpy(&w.count, At(level, kNetVarPoolCountOff), sizeof w.count);
    memcpy(&w.prefix, At(level, kNetVarPoolPrefixOff), sizeof w.prefix);
    memcpy(&w.records, At(level, kNetVarRecordTblOff), sizeof w.records);

    if (slot >= w.capacity) {
        w.step = NetVarPoolStep::SlotOverCapacity;
        return w;
    }
    const uint8_t* entry = w.slotMap + kNetVarSlotIndexStride * slot;
    if (!Committed(entry, kNetVarSlotIndexStride)) {
        w.step = NetVarPoolStep::SlotMapUnreadable;
        return w;
    }
    memcpy(&w.index, entry, sizeof w.index);
    memcpy(&w.generation, entry + kNetVarSlotGenerationOff, sizeof w.generation);
    if (w.index == kNetVarIndexNone) {
        w.step = NetVarPoolStep::SlotUnmapped;
        return w;
    }
    if (w.index >= w.count) {
        w.step = NetVarPoolStep::IndexOverCount;
        return w;
    }

    uint8_t* record = w.records + static_cast<uint64_t>(w.index) * kNetVarRecordStride;
    if (!Committed(record, kNetVarRecordStride)) {
        w.step = NetVarPoolStep::RecordUnreadable;
        return w;
    }
    w.record = record;
    memcpy(&w.store, record + kNetVarRecordStoreOff, sizeof w.store);
    if (!Committed(w.store, kNetVarStoreHeaderBytes)) {
        w.step = NetVarPoolStep::StoreUnreadable;
        return w;
    }
    w.step = NetVarPoolStep::Walked;
    return w;
}

NetVarApplyStatus EngineNetVarTable::ResolveSlot(uint16_t slot, NetVarRecord& out) {
    const NetVarPoolWalk walk = WalkNetVarPool(level_, slot);
    last_index_ = walk.index;
    switch (walk.step) {
        case NetVarPoolStep::Walked:
            break;
        case NetVarPoolStep::SlotOverCapacity:
        case NetVarPoolStep::SlotMapUnreadable:
        case NetVarPoolStep::SlotUnmapped:
        case NetVarPoolStep::IndexOverCount:
            return NetVarApplyStatus::SchemaMismatch;
        case NetVarPoolStep::NoComponent:
        case NetVarPoolStep::HeaderUnreadable:
        case NetVarPoolStep::RecordUnreadable:
        case NetVarPoolStep::StoreUnreadable:
            return NetVarApplyStatus::TableNotReady;
    }

    uint32_t flags = 0;
    memcpy(&flags, walk.record + kNetVarRecordFlagsOff, sizeof flags);
    out.slot = slot;
    out.index = walk.index;
    out.store = walk.store;
    out.initial_vars_seen = (flags & kNetVarRecordInitialBit) != 0;
    return NetVarApplyStatus::Applied;
}

NetVarApplyStatus EngineNetVarTable::ApplyRows(const NetVarRecord& rec, const NetVarApplyRow* rows,
                                               size_t count, uint32_t& written) {
    written = 0;
    // Everything the offline image establishes has been done by the time control reaches here: the
    // owner matched, the slot resolved, the table passed the guard and the record is mapped. What is
    // left is one address that only the loaded image can supply. Refusing HERE, by name, says which
    // step is missing; refusing at the top would have said only that netvars do not replay.
    if (!decode_) return NetVarApplyStatus::DispatchUnbound;
    if (!rec.store || !rows || !count) return NetVarApplyStatus::RowRejected;
    if (count > kMaxRowsPerCall) return NetVarApplyStatus::RowRejected;

    // The payload the engine's decoder reads, rebuilt in its own layout: a rowset header of
    // {u32 seq, u16 var_count} and then 16-byte rows. The recorded rows go in verbatim -- the values
    // are raw SValue qwords and re-encoding one is the documented pause-overlay regression.
    uint8_t payload[kPayloadHeaderBytes + kMaxRowsPerCall * 16] = {};
    const uint32_t seq = rec.index;   // the decoder stores it beside the row; the engine's is per-rowset
    const uint16_t n = static_cast<uint16_t>(count);
    memcpy(payload, &seq, sizeof seq);
    memcpy(payload + 4, &n, sizeof n);
    for (size_t i = 0; i < count; ++i) {
        memcpy(payload + kPayloadHeaderBytes + 16 * i, &rows[i].name, 8);
        memcpy(payload + kPayloadHeaderBytes + 16 * i + 8, &rows[i].value, 8);
    }

    // The collector the engine passes as the fourth argument: it appends every CHANGED name and
    // grows only when the count reaches the capacity at +0x28, so a capacity of `count` cannot make
    // it call the engine's allocator on our thread.
    uint64_t names[kMaxRowsPerCall] = {};
    ChangedNames collector = {};
    collector.base = names;
    collector.cap_a = count;
    collector.cap_b = count;
    collector.count = 0;

    decode_(rec.store, payload, kPayloadHeaderBytes + 16u * count, &collector);
    if (collector.count > count) return NetVarApplyStatus::RowRejected;   // it grew: our shape is wrong
    written = static_cast<uint32_t>(collector.count);
    return NetVarApplyStatus::Applied;
}

void EngineNetVarTable::MarkInitialVarsSeen(const NetVarRecord& rec) {
    // 140D3E080: `or dword ptr [record+0x28], 1`. Only meaningful once a decode actually ran, so it
    // is written only when one is bound -- setting it after a refusal would tell the "waiting for
    // initial vars" gate a thing that did not happen.
    if (!decode_) return;
    const NetVarPoolWalk walk = WalkNetVarPool(level_, rec.slot);
    if (walk.step != NetVarPoolStep::Walked || walk.index != rec.index) return;
    uint32_t flags = 0;
    memcpy(&flags, walk.record + kNetVarRecordFlagsOff, sizeof flags);
    flags |= kNetVarRecordInitialBit;
    memcpy(walk.record + kNetVarRecordFlagsOff, &flags, sizeof flags);
}

}  // namespace playback
}  // namespace replay
