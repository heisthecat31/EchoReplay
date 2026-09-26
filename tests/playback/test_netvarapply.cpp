// W2-B. The netvar apply, against a table built to the engine's own shape.
//
// The point of building the engine's CCompactPool rather than mocking the walk: `EngineNetVarTable`
// is the code under test, so the [level+0xD0] slot map, the u16 counts, the bare record-array pointer
// at [level+0x100], the record stride and the payload the store decoder reads all run for real. Only the decoder itself is ours -- and it is
// bound through the same seam the loaded image binds, so the argument shape it receives is the shape
// the engine's own decoder is passed.

#include "doctest.h"

#include <stdint.h>
#include <string.h>

#include "capture/streamcodec.h"
#include "foundation/bitio/bitio.h"
#include "playback/core/netvarapply.h"
#include "playback/core/window.h"

using namespace replay;
using namespace replay::playback;

namespace {

constexpr uint64_t kLevelId = 0x4152454E41303031ull;   // "ARENA001"

// The store the bound decoder writes into. Not the engine's 288-byte layout -- that is behind
// sub_140F8BCB0 and this test is not reimplementing it. What this DOES reproduce is the contract the
// engine's decoder has with its caller: rows arrive as {u64 name, u64 value} at payload+6, an
// unchanged row is not a change, and every changed name is appended to the collector.
struct FakeStore {
    struct Row {
        uint64_t name;
        uint64_t value;
    };
    Row rows[32] = {};
    size_t count = 0;

    bool Find(uint64_t name, uint64_t& out) const {
        for (size_t i = 0; i < count; ++i) {
            if (rows[i].name == name) {
                out = rows[i].value;
                return true;
            }
        }
        return false;
    }
};

// The collector's field offsets, taken from sub_140F8BCB0's own indexing of its fourth argument.
// Spelled here as numbers on purpose: if netvarapply.cpp's struct ever stops matching them, this
// test writes through the wrong words and fails, which is the only way a layout contract can fail.
constexpr uint32_t kCollectorBase = 0x00;
constexpr uint32_t kCollectorCap = 0x28;
constexpr uint32_t kCollectorCount = 0x30;

int __fastcall FakeStoreDecode(void* store, const void* payload, uint64_t bytes, void* changed) {
    FakeStore* s = static_cast<FakeStore*>(store);
    uint16_t n = 0;
    memcpy(&n, static_cast<const uint8_t*>(payload) + 4, 2);
    REQUIRE(bytes == 6u + 16u * n);

    uint64_t* names = nullptr;
    uint64_t cap = 0, wrote = 0;
    memcpy(&names, static_cast<uint8_t*>(changed) + kCollectorBase, sizeof names);
    memcpy(&cap, static_cast<uint8_t*>(changed) + kCollectorCap, sizeof cap);

    for (uint16_t i = 0; i < n; ++i) {
        uint64_t name = 0, value = 0;
        memcpy(&name, static_cast<const uint8_t*>(payload) + 6 + 16 * i, 8);
        memcpy(&value, static_cast<const uint8_t*>(payload) + 6 + 16 * i + 8, 8);
        bool changed_row = true;
        size_t at = s->count;
        for (size_t j = 0; j < s->count; ++j) {
            if (s->rows[j].name == name) {
                at = j;
                changed_row = s->rows[j].value != value;
                break;
            }
        }
        if (at == s->count && s->count < 32) ++s->count;
        s->rows[at].name = name;
        s->rows[at].value = value;
        if (changed_row && wrote < cap) names[wrote++] = name;
    }
    memcpy(static_cast<uint8_t*>(changed) + kCollectorCount, &wrote, sizeof wrote);
    return wrote ? 1 : 0;
}

// A level object shaped the way sub_140D3DF90 reads one: the component owner at +0x80 carrying the
// level id at +0x58, and the CCompactPool at +0xC8 as sub_140CFD730 builds it: the slot->index map at
// +0xD0, u16 capacity / record count at +0xF8 / +0xFA, the bare record-array pointer at +0x100.
struct FakeLevel {
    static constexpr size_t kCapacity = 8;
    static constexpr size_t kRecords = 3;

    uint8_t bytes[0x200] = {};
    uint64_t owner_obj[16] = {};
    uint16_t slot_map[kCapacity][2] = {};             // stride 4: {u16 index, u16 generation}
    uint8_t records[kCapacity * 0x30] = {};
    FakeStore stores[kRecords];

    FakeLevel() {
        owner_obj[0x58 / 8] = kLevelId;
        void* ownerp = owner_obj;
        memcpy(bytes + 0x80, &ownerp, sizeof ownerp);
        void* mapp = slot_map;
        memcpy(bytes + 0xD0, &mapp, sizeof mapp);
        for (size_t i = 0; i < 8; ++i) slot_map[i][0] = kNetVarSlotNone;
        const uint16_t cap = kCapacity;
        memcpy(bytes + 0xF8, &cap, sizeof cap);
        SetRecordCount(kRecords);
        const uint16_t prefix = 1;   // the active prefix at +0xFC: NOT the record count
        memcpy(bytes + 0xFC, &prefix, sizeof prefix);
        void* recordsp = records;
        memcpy(bytes + 0x100, &recordsp, sizeof recordsp);
        // Past the pool, what a live CR15NetVarCS holds: a pointer and a set bool. Read as a CTable
        // header's `count` and `flags` they refused every live table.
        const uint64_t past = 0x000001C08F7A1230ull;
        memcpy(bytes + 0x130, &past, sizeof past);  // facts-ok: PHYS.CI_TARGET_TIME_OFF a test fixture value this case compares against, not an engine position
        const uint32_t set = 1;
        memcpy(bytes + 0x138, &set, sizeof set);  // facts-ok: ENTITY.REMOTEPLAYER_CS_NCAPHYSICS_OFF a test fixture value this case compares against, not an engine position
        for (size_t i = 0; i < kRecords; ++i) {
            void* sp = &stores[i];
            memcpy(records + i * 0x30 + 0x10, &sp, sizeof sp);
        }
    }

    void MapSlot(uint16_t slot, uint16_t index) { slot_map[slot][0] = index; }

    // An address no page of this process maps, so any read through the slot map is observable as a
    // refusal that names the wrong step (or as a fault, if a future edit drops the Committed check).
    void PoisonSlotMap() {
        void* p = reinterpret_cast<void*>(0xDEAD0000DEAD0000ull);
        memcpy(bytes + 0xD0, &p, sizeof p);
    }

    void SetRecordCount(uint16_t n) { memcpy(bytes + 0xFA, &n, sizeof n); }

    uint32_t RecordFlags(size_t index) const {
        uint32_t f = 0;
        memcpy(&f, records + index * 0x30 + 0x28, sizeof f);
        return f;
    }

    void* Level() { return bytes; }
};

// The synthetic capture: a real S_NETVAR record, encoded by the shipping encoder and read back by
// the shipping decoder, so the rows and the SLOT reach the window through the file's own codec.
void SeedWindowFromEncodedNetvar(Window& w, uint16_t slot, const uint64_t* names,
                                 const uint64_t* values, size_t n, int64_t t_us) {
    capture::NetvarMessage m;
    memset(&m, 0, sizeof m);
    m.owner = kLevelId;
    m.slot = slot;
    m.gen = 0;
    m.pktSeq = 1;
    m.hasRowset = true;
    m.rowsetSeq = 1;
    m.varCount = static_cast<uint16_t>(n);
    for (size_t i = 0; i < n; ++i) {
        m.rows[i].name = names[i];
        m.rows[i].value = values[i];
    }

    static uint8_t buf[4096];
    bitio::BitWriter bw(buf, sizeof buf);
    capture::NetvarEncoder enc;
    REQUIRE(enc.Encode(bw, m));

    bitio::BitReader br(buf, bw.BytesWritten());
    capture::NetvarDecoder dec(true);
    capture::NetvarMessage got;
    memset(&got, 0, sizeof got);
    REQUIRE(dec.Decode(br, got));
    REQUIRE(got.slot == slot);
    REQUIRE(got.varCount == m.varCount);

    KeyedRecord recs[16];
    for (uint16_t i = 0; i < got.varCount; ++i) {
        recs[i].t_us = t_us;
        recs[i].key = got.rows[i].name;
        recs[i].value = got.rows[i].value;
        w.NoteNetVarSlot(got.rows[i].name, got.slot);   // windowbuild's own call
    }
    REQUIRE(w.NetVars().Build(nullptr, 0, recs, got.varCount));
}

const uint64_t kNames[3] = {0x9001, 0x9002, 0x9003};
const uint64_t kValues[3] = {11, 22, 33};

}  // namespace

TEST_CASE("netvarapply: an S_NETVAR rowset reaches the engine's slot-1 record") {
    Window w;
    SeedWindowFromEncodedNetvar(w, /*slot=*/1, kNames, kValues, 3, 1009);

    // The slot survived the codec and the window. Before this change the window dropped it and every
    // row was addressed to slot 0.
    uint16_t slot = kNetVarSlotNone;
    REQUIRE(w.NetVarSlotOf(kNames[0], slot));
    CHECK(slot == 1);

    FakeLevel level;
    level.MapSlot(1, 2);   // slot 1 lives in update record 2
    EngineNetVarTable table(level.Level());
    table.BindStoreDecode(&FakeStoreDecode);

    NetVarApplyRow rows[3];
    for (size_t i = 0; i < 3; ++i) {
        uint64_t v = 0;
        REQUIRE(w.NetVars().ValueAt(kNames[i], 2000, v));
        rows[i].name = kNames[i];
        rows[i].value = v;
    }

    NetVarApplyStats st;
    const NetVarApplyStatus s = ApplyRowset(&table, kLevelId, 1, rows, 3, st);
    REQUIRE(s == NetVarApplyStatus::Applied);
    CHECK(NetVarApplyRefusalName(s) == nullptr);
    CHECK(st.rows_written == 3);
    CHECK(st.rowsets_applied == 1);

    // THE TABLE'S ROWS CHANGED -- and in record 2, not record 0 or 3.
    for (size_t i = 0; i < 3; ++i) {
        uint64_t got = 0;
        REQUIRE(level.stores[2].Find(kNames[i], got));
        CHECK(got == kValues[i]);
    }
    CHECK(level.stores[0].count == 0);
    CHECK(level.stores[1].count == 0);
    // 140D3E080: the engine's "initial vars received" bit, set after the decode.
    CHECK((level.RecordFlags(2) & 1u) == 1u);
}

TEST_CASE("netvarapply: an index at or past the pool's record count refuses and writes nothing") {
    FakeLevel level;
    level.MapSlot(1, 2);
    level.SetRecordCount(2);   // record 2 has not been appended
    EngineNetVarTable table(level.Level());
    table.BindStoreDecode(&FakeStoreDecode);

    NetVarApplyRow rows[1] = {{kNames[0], kValues[0]}};
    NetVarApplyStats st;
    const NetVarApplyStatus s = ApplyRowset(&table, kLevelId, 1, rows, 1, st);

    // REQUIRE, not CHECK: every assertion below is predicated on the write NOT having happened.
    REQUIRE(s == NetVarApplyStatus::SchemaMismatch);
    CHECK(table.LastIndex() == 2);
    CHECK(st.rows_written == 0);
    CHECK(st.rows_refused == 1);
    CHECK(level.stores[2].count == 0);
    CHECK((level.RecordFlags(2) & 1u) == 0u);

    level.SetRecordCount(3);
    CHECK(ApplyRowset(&table, kLevelId, 1, rows, 1, st) == NetVarApplyStatus::Applied);
}

TEST_CASE("netvarapply: an unreadable slot map or record array refuses by name, never faults") {
    NetVarApplyRow rows[1] = {{kNames[0], kValues[0]}};
    {
        FakeLevel level;
        level.MapSlot(1, 2);
        level.PoisonSlotMap();
        EngineNetVarTable table(level.Level());
        table.BindStoreDecode(&FakeStoreDecode);
        NetVarApplyStats st;
        REQUIRE(ApplyRowset(&table, kLevelId, 1, rows, 1, st) == NetVarApplyStatus::SchemaMismatch);
        CHECK(st.rowsets_refused == 1);
        CHECK(level.stores[2].count == 0);
    }
    {
        FakeLevel level;
        level.MapSlot(1, 2);
        void* nowhere = reinterpret_cast<void*>(0xDEAD0000DEAD0000ull);
        memcpy(level.bytes + 0x100, &nowhere, sizeof nowhere);
        EngineNetVarTable table(level.Level());
        table.BindStoreDecode(&FakeStoreDecode);
        NetVarApplyStats st;
        const NetVarApplyStatus s = ApplyRowset(&table, kLevelId, 1, rows, 1, st);
        REQUIRE(s == NetVarApplyStatus::TableNotReady);
        CHECK(strcmp(NetVarApplyRefusalName(s), "netvars-table-not-ready") == 0);
        CHECK(level.stores[2].count == 0);
    }
    {
        FakeLevel level;   // a slot past the map's `capacity` entries is never read through
        EngineNetVarTable table(level.Level());
        table.BindStoreDecode(&FakeStoreDecode);
        NetVarApplyStats st;
        CHECK(ApplyRowset(&table, kLevelId, FakeLevel::kCapacity, rows, 1, st) ==
              NetVarApplyStatus::SchemaMismatch);
    }
}

TEST_CASE("netvarapply: a capture from another level refuses by name") {
    FakeLevel level;
    level.MapSlot(1, 2);
    EngineNetVarTable table(level.Level());
    table.BindStoreDecode(&FakeStoreDecode);

    NetVarApplyRow rows[1] = {{kNames[0], kValues[0]}};
    NetVarApplyStats st;
    const NetVarApplyStatus s = ApplyRowset(&table, kLevelId ^ 1, 1, rows, 1, st);
    CHECK(s == NetVarApplyStatus::OwnerMismatch);
    CHECK(strcmp(NetVarApplyRefusalName(s), "netvars-owner-mismatch") == 0);
    CHECK(level.stores[2].count == 0);
}

TEST_CASE("netvarapply: a slot with no update record refuses schema, not slot 0") {
    FakeLevel level;   // slot map left at 0xFFFF everywhere
    EngineNetVarTable table(level.Level());
    table.BindStoreDecode(&FakeStoreDecode);

    NetVarApplyRow rows[1] = {{kNames[0], kValues[0]}};
    NetVarApplyStats st;
    const NetVarApplyStatus s = ApplyRowset(&table, kLevelId, 1, rows, 1, st);
    CHECK(s == NetVarApplyStatus::SchemaMismatch);
    CHECK(strcmp(NetVarApplyRefusalName(s), "netvars-schema-mismatch") == 0);
    CHECK(level.stores[0].count == 0);
}

TEST_CASE("netvarapply: with no decoder bound the walk still runs and names the missing step") {
    FakeLevel level;
    level.MapSlot(0, 1);
    EngineNetVarTable table(level.Level());   // BindStoreDecode deliberately not called

    NetVarApplyRow rows[1] = {{kNames[0], kValues[0]}};
    NetVarApplyStats st;
    const NetVarApplyStatus s = ApplyRowset(&table, kLevelId, 0, rows, 1, st);
    // Not NoTable, not OwnerMismatch: every derivable precondition passed, and the refusal names the
    // one step that a loaded image supplies.
    CHECK(s == NetVarApplyStatus::DispatchUnbound);
    CHECK(strcmp(NetVarApplyRefusalName(s), "netvars-dispatch-unbound") == 0);
    CHECK((level.RecordFlags(1) & 1u) == 0u);
}
