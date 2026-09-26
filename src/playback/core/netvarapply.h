#pragma once

// W2-B. Applying a replayed netvar rowset the way the engine applies a received one.
//
// ---------------------------------------------------------------------------------------------
// ★ THIS MIRRORS ONE ENGINE FUNCTION, STEP FOR STEP: `echovr.exe` `sub_140D3DF90`, the listener the
// level registers against `SR15NetReplicatedDeltaUnreliableVars` (registered at `sub_140D048D0`
// +0x0E2, the "[NETVAR] Level '%s' waiting for initial vars" gate). It is the only code on this
// build that turns a DUV message into netvar state, so it is the only thing worth mirroring: a
// replay that wrote a store offset of its own would be guessing at a layout the engine owns.
//
// The engine's sequence, read off the disassembly (offsets are into the message body, which is what
// `NETVAR.DUV_*` names, and each one is anchored in the W2-B_netvarapply fact drop):
//
//   140D3DFAD  mov  r8, [rcx+80h] / mov rax,[r8+58h]    the level id this component belongs to
//   140D3DFB8  mov  rcx, [rdx+10h]                      the message body
//   140D3DFBC  cmp  [rcx], rax                          body[0x00] == level id, else DROP THE MESSAGE
//   140D3DFC5  movzx r15d, word ptr [rcx+8]             body[0x08] = the netvar slot
//   140D3DFD3  movzx eax,  word ptr [rcx+0Ah]           body[0x0A] = the component generation
//   140D3DFE5  cmp  r15w, 0FFFFh / jz                   0xFFFF is the engine's own "no slot" value
//   140D3DFEF  mov  rax, [rsi+0D0h]                     slot -> dense record index (u16, stride 4)
//   140D3DFF6  movzx ecx, word ptr [rax+r15*4]
//   140D3DFFB  lea  r14, [rcx+rcx*2] / shl r14,4        record = index * 0x30 ...
//   140D3E003  add  r14, [rsi+100h]                     ... into the update-record table
//   140D3E06C  call sub_140F8BCB0([r14+10h], payload)   the store decode -- THE WRITE
//   140D3E080  or   dword ptr [r14+28h], 1              "initial vars have been received"
//   140D3E0C1  call sub_140CCF9E0(this, slot|gen<<16, row)   delegate_OnNetVarChanged, per changed row
//
// Two things follow from that, and they are the reason this file exists rather than a store poke:
//
//  1. THE OWNER CHECK IS NOT CEREMONY. The engine drops a whole message whose body[0x00] is not the
//     loaded level. Replaying an Arena capture into a lobby level must refuse for exactly that
//     reason, by name (`netvars-owner-mismatch`), and not quietly write scores into the wrong level.
//  2. THE RECORD ARRAY AT level+0x100 IS NOT A CTable. It is the bare base pointer of the record
//     array of the CCompactPool embedded at level+0xC8 (built by sub_140CFD730, appended to by
//     sub_140C80660); its count is the pool's u16 at +0xFA and it has no sort bit -- the pool is
//     never sorted, and the STORE's own unsorted bit is the decoder's to handle. `tableguard` reads
//     a 0x40-byte header there and lands on members past the pool, so it must not be pointed at
//     this (DIAG-NETVAR-TABLEGUARD S1.4). The walk is bounded by the pool's own counts instead.
//
// ★ WHAT IS NOT ESTABLISHED, STATED AS A CONDITION AND NOT AS A SHRUG. `sub_140D3DF90` takes the
// component object in `rcx`; nothing in the fact registry names how to reach it from a plugin tick,
// and `sub_140F8BCB0`'s fourth argument is an `ATL::CRowset` built through three engine calls and a
// TLS accessor. So `EngineNetVarTable` performs every step above that is derivable offline and
// refuses the last one BY NAME (`netvars-dispatch-unbound`) until `BindDispatch` is given the
// component pointer and the receive entry. That refusal is CONDITIONAL on a binding that a live
// session supplies; it is not the unconditional `return false` this replaced.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay {
namespace playback {

// The engine's own "no slot" value at body[0x08] (140D3DFDF: mov r12d, 0FFFFh), which is the
// instruction NETVAR.SLOT_NONE is anchored at.
inline constexpr uint16_t kNetVarSlotNone =
    static_cast<uint16_t>(facts::NETVAR_SLOT_NONE);

enum class NetVarApplyStatus : uint8_t {
    NotAttempted = 0,
    Applied,
    NoTable,            // nothing to write into: no engine table is bound on this playback
    OwnerUnresolvable,  // the level id the engine compares body[0x00] against could not be read
    OwnerMismatch,      // the capture's level is not the level loaded -- the engine drops these too
    SlotInvalid,        // body[0x08] is 0xFFFF
    TableUnsorted,      // no longer produced: the pool has no sort bit, and the decoder sorts its store
    TableNotReady,      // the component's pool, the slot's record or its store could not be read
    SchemaMismatch,     // the slot has no update record in this level's table
    DispatchUnbound,    // every precondition held; the engine receive entry is not bound
    RowRejected,        // the table accepted the record and refused a row
};

// The refusal's NAME, which is what a report and a gate row carry. `Applied` and `NotAttempted` have
// no refusal name and answer nullptr, so a caller cannot print a refusal for a write that landed.
const char* NetVarApplyRefusalName(NetVarApplyStatus);
// One sentence, for a log line.
const char* NetVarApplyText(NetVarApplyStatus);

struct NetVarApplyRow {
    uint64_t name;   // CSymbol64
    uint64_t value;  // the raw SValue qword, never re-encoded (DESIGN S5.4.5 rule 2)
};

// One 48-byte update record, as `sub_140D3DF90` resolves it. `store` is `[record+0x10]`, the object
// the engine hands its decoder; `index` is the dense index the slot mapped to, kept because a wrong
// one is the failure this whole walk exists to avoid and a report that cannot name it cannot show it.
struct NetVarRecord {
    uint16_t slot = kNetVarSlotNone;
    uint16_t index = 0;
    void* store = nullptr;
    bool initial_vars_seen = false;
};

// The seam. The engine implements it against a live level; a test implements it against an array,
// and both run the same sequence in ApplyRowset -- there is no second ordering.
class INetVarTable {
public:
    // `[[rcx+0x80]+0x58]`: the level id body[0x00] is checked against.
    virtual bool OwnerId(uint64_t& out) const = 0;
    // The `[level+0xD0]` slot map into the `[level+0x100]` record array. Answers `Applied` when
    // `out` is usable, and TableNotReady / SchemaMismatch otherwise.
    virtual NetVarApplyStatus ResolveSlot(uint16_t slot, NetVarRecord& out) = 0;
    // The decode into `[record+0x10]`. `written` counts rows whose stored value CHANGED, which is
    // what the engine's own per-row delegate loop counts -- an unchanged row is not a write.
    virtual NetVarApplyStatus ApplyRows(const NetVarRecord& rec, const NetVarApplyRow* rows,
                                        size_t count, uint32_t& written) = 0;
    // `or dword ptr [record+0x28], 1`. The engine sets it after the decode whether or not the decode
    // changed anything, and the "waiting for initial vars" gate reads exactly this bit.
    virtual void MarkInitialVarsSeen(const NetVarRecord& rec) = 0;

protected:
    ~INetVarTable() = default;
};

struct NetVarApplyStats {
    uint32_t rows_offered = 0;
    uint32_t rows_written = 0;
    uint32_t rows_refused = 0;
    uint32_t rowsets_applied = 0;
    uint32_t rowsets_refused = 0;
    NetVarApplyStatus last_refusal = NetVarApplyStatus::NotAttempted;
};

// Applies one rowset -- one slot's worth of rows at one playhead -- in the engine's own order.
// `owner` is the level id the capture recorded in body[0x00]; it is compared, never rewritten.
NetVarApplyStatus ApplyRowset(INetVarTable* table, uint64_t owner, uint16_t slot,
                              const NetVarApplyRow* rows, size_t count, NetVarApplyStats& st);

// The engine-backed table. `level` is the object `sub_140D3DF90` receives in `rcx`.
class EngineNetVarTable final : public INetVarTable {
public:
    // `sub_140F8BCB0`, the store decoder `sub_140D3DF90` calls at 140D3E06C, as the caller resolved
    // it from the loaded image (RVA 0xF8BCB0 on the md5-pinned build). Its arguments, read off its
    // own body: the store `[record+0x10]`, a payload that begins {u32 rowset_seq, u16 var_count}
    // followed by 16-byte {name, value} rows, the payload length, and an optional collector that it
    // appends the CHANGED names to. It sorts the store itself when the store's own unsorted bit is
    // set (140F8BD5x -> sub_140C8A380), which is why nothing here sorts anything.
    using StoreDecodeFn = int(__fastcall*)(void* store, const void* payload, uint64_t bytes,
                                           void* changed_names);

    explicit EngineNetVarTable(void* level) : level_(level) {}

    // Supplies the one address that cannot be derived without the loaded image. Until a caller binds
    // it, ApplyRows refuses by name AFTER every other precondition has been checked -- so the refusal
    // reports how far the walk got, not that it never started.
    void BindStoreDecode(StoreDecodeFn fn) { decode_ = fn; }
    bool DecodeBound() const { return decode_ != nullptr; }

    bool OwnerId(uint64_t& out) const override;
    NetVarApplyStatus ResolveSlot(uint16_t slot, NetVarRecord& out) override;
    NetVarApplyStatus ApplyRows(const NetVarRecord& rec, const NetVarApplyRow* rows, size_t count,
                                uint32_t& written) override;
    void MarkInitialVarsSeen(const NetVarRecord& rec) override;

    // How far the last ResolveSlot got, for a report that has to say which step refused.
    uint16_t LastIndex() const { return last_index_; }

private:
    void* level_ = nullptr;
    StoreDecodeFn decode_ = nullptr;
    uint16_t last_index_ = 0;
};

// Offsets on the level object, read off `sub_140D3DF90`. Named here rather than spelled at the use
// site so the one place that has to change when a build moves them is the one place a reader looks.
inline constexpr uint32_t kNetVarOwnerPtrOff  = facts::NETVAR_OWNER_PTR_OFF;  // level+0x80 -> component owner
inline constexpr uint32_t kNetVarOwnerIdOff   = facts::NETVAR_OWNER_ID_OFF;   // owner+0x58 -> the level id
//
// +0xD0 .. +0x128 is a CCompactPool embedded at level+0xC8 (sub_140D03C60 `lea r8,[rbx+0C8h]`), so
// +0x100 is the BARE base pointer of the record array and the counts are the pool's own u16s. It is
// not a CTable header and tableguard does not apply to it.
inline constexpr uint32_t kNetVarSlotIndexOff = 0xD0u;  // level+0xD0 -> {u16 index, u16 generation}[] by slot
inline constexpr uint32_t kNetVarSlotIndexStride = 4u;  // 140D3666F: movzx edx, word ptr [rax+r9*4]
inline constexpr uint32_t kNetVarSlotGenerationOff = 2u;  // 140C80981: movzx ecx, word ptr [r14+rax+2] -> handle hi word
inline constexpr uint32_t kNetVarPoolCapacityOff = 0xF8u; // facts-ok: NETVAR.POOL_CAPACITY_OFF pending; u16, 140CFD93C: mov [rbx+30h], r13w
inline constexpr uint32_t kNetVarPoolCountOff = 0xFAu;    // facts-ok: NETVAR.POOL_COUNT_OFF pending; u16, 140C80780: movzx r15d, word ptr [rdi+32h]
inline constexpr uint32_t kNetVarPoolPrefixOff = 0xFCu;   // facts-ok: NETVAR.POOL_PREFIX_OFF pending; u16, 140C809CC: inc word ptr [rdi+34h]
inline constexpr uint32_t kNetVarRecordTblOff = 0x100u; // level+0x100 -> record array; 140D3667C: add r9,[rcx+100h]
inline constexpr uint32_t kNetVarRecordStride = 0x30u;  // 48 bytes: lea r14,[rcx+rcx*2]; shl r14,4
inline constexpr uint32_t kNetVarRecordStoreOff = 0x10u;
inline constexpr uint32_t kNetVarRecordFlagsOff = 0x28u;
inline constexpr uint32_t kNetVarRecordInitialBit = 0x1u;
inline constexpr uint16_t kNetVarIndexNone = 0xFFFFu;   // 140CFD952: every slot -> 0xFFFF at construction  // facts-ok: NETVAR.SLOT_NONE the u16 range limit, not the engine no-slot field
inline constexpr uint32_t kNetVarStoreHeaderBytes = 0x48u;  // through the store's flags qword at +0x40

// Where the walk from a slot to its store stopped. Every step is a guarded read of the pool the
// engine's own accessors walk unguarded (sub_140D36660).
enum class NetVarPoolStep : uint8_t {
    Walked = 0,
    NoComponent,
    HeaderUnreadable,   // cs+0xD0 .. cs+0x108 is not committed memory
    SlotOverCapacity,   // the slot map holds `capacity` entries and the slot is past them
    SlotMapUnreadable,
    SlotUnmapped,       // the slot's index is 0xFFFF: no record was ever appended for it
    IndexOverCount,     // the slot's index is not below the pool's record count
    RecordUnreadable,
    StoreUnreadable,    // [record+0x10] is null or not committed
};
const char* NetVarPoolStepText(NetVarPoolStep);

struct NetVarPoolWalk {
    NetVarPoolStep step = NetVarPoolStep::NoComponent;
    uint16_t capacity = 0;
    uint16_t count = 0;
    uint16_t prefix = 0;
    uint16_t index = kNetVarIndexNone;
    uint16_t generation = 0;
    const uint8_t* slotMap = nullptr;
    uint8_t* records = nullptr;
    uint8_t* record = nullptr;
    void* store = nullptr;

    // The slot map names a live record: everything the typed setters and getters rely on.
    bool SlotMapped() const {
        return step == NetVarPoolStep::Walked || step == NetVarPoolStep::RecordUnreadable ||
               step == NetVarPoolStep::StoreUnreadable;
    }
};
NetVarPoolWalk WalkNetVarPool(void* level, uint16_t slot);

}  // namespace playback
}  // namespace replay
