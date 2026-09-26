#include "audio/voip/voipsite.h"

#include "audio/memprobe.h"
#include "audio/voip/voip_facts.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/tableguard.h"
#include "generated/facts_generated.h"

namespace replay {
namespace audio {
namespace voip {
namespace {

// The component-system table is indexed in 16-byte {symbol, system} pairs -- the same element size
// csregister.cpp:333 and camera_control.cpp:69 judge it by. The guard needs it to decide whether the
// block holds the elements its header claims.
//
// ★ THIS IS THE THIRD PRIVATE COPY, AND IT IS ONE BECAUSE THERE IS NOTHING TO READ (AUD-F3L F3L-4).
// The earlier claim here that a stride "is not a fact row" was wrong: facts.tsv carries
// WORLD.NETACTOR_TABLE_ENTRY_STRIDE = 16 with an anchor and an evidence site. But that row is
// PC_PRESENT/U, so facts_generated.h emits it into `withheld_not_usable_on_pc` as an INCOMPLETE
// TYPE -- there is no facts:: name with a value to read, and writing one would be inventing the
// promotion rather than reading it. The CS table's stride has no row of its own at all. Promoting
// CS.TABLE_ENTRY_STRIDE is a facts-packet change and the correction ARCH-10 3.3 actually asks for;
// until it lands, all three sites are copies and this comment says so instead of excusing it.
const uint64_t kCsEntryStride = 16u;

ComponentLookupFn g_lookupForTest = nullptr;
ConnectFn g_connectForTest = nullptr;
DisconnectFn g_disconnectForTest = nullptr;
const uint32_t* g_sentinelForTest = nullptr;

const uint8_t* At(const void* base, uint32_t off) {
    return static_cast<const uint8_t*>(base) + off;
}

// Every engine dereference in this file goes through one of these three. A pointer we did not
// allocate is a claim, and the design's rule is a clean refusal rather than a corruption.
bool ReadU16(const void* p, uint16_t* out) {
    if (!Readable(p, sizeof(uint16_t))) {
        return false;
    }
    *out = *static_cast<const uint16_t*>(p);
    return true;
}

bool ReadU32(const void* p, uint32_t* out) {
    if (!Readable(p, sizeof(uint32_t))) {
        return false;
    }
    *out = *static_cast<const uint32_t*>(p);
    return true;
}

bool ReadPtr(const void* p, void** out) {
    if (!Readable(p, sizeof(void*))) {
        return false;
    }
    *out = *reinterpret_cast<void* const*>(p);
    return true;
}

// The three pointers the walk needs off the component system, gathered in one place so a partial
// read cannot leave half of them stale.
struct CsTables {
    const uint8_t* ciBase;
    const uint8_t* poolTable;
    const uint8_t* denseMap;
    uint16_t count;
};

bool ReadTables(const VoipSite& site, CsTables* out) {
    void* ciBase = nullptr;
    void* poolTable = nullptr;
    void* denseMap = nullptr;
    uint16_t count = 0;
    if (!ReadU16(At(site.cs, voip_facts::kCsCountOff), &count) ||
        !ReadPtr(At(site.cs, voip_facts::kCsCiBaseOff), &ciBase) ||
        !ReadPtr(At(site.cs, voip_facts::kCsPoolTableOff), &poolTable) ||
        !ReadPtr(At(site.cs, voip_facts::kCsDenseMapOff), &denseMap)) {
        return false;
    }
    if (ciBase == nullptr || poolTable == nullptr || denseMap == nullptr) {
        return false;
    }
    out->ciBase = static_cast<const uint8_t*>(ciBase);
    out->poolTable = static_cast<const uint8_t*>(poolTable);
    out->denseMap = static_cast<const uint8_t*>(denseMap);
    out->count = count;
    return true;
}

// The engine's own "nobody is in this slot" id. Read, never transcribed -- and an unreadable one
// is an absence, not a licence to assume 0xFFFFFFFF: guessing it wrong makes a TAKEN slot look
// free, and binding over a live talker is the one mistake here that damages the session.
bool ReadNoUserSentinel(uint32_t* out) {
    if (g_sentinelForTest != nullptr) {
        *out = *g_sentinelForTest;
        return true;
    }
    const void* p = buildguard::MappedAddress(voip_facts::kNoUserSentinelVa);
    return p != nullptr && ReadU32(p, out);
}

BindEpochFn g_bindEpoch = nullptr;

void Set(Resolve* why, Resolve v) {
    if (why != nullptr) {
        *why = v;
    }
}

}  // namespace

void SetComponentLookupForTest(ComponentLookupFn fn) { g_lookupForTest = fn; }

void SetBindEpochSource(BindEpochFn fn) { g_bindEpoch = fn; }

const char* ResolveText(Resolve r) {
    switch (r) {
        case Resolve::Ok: return "ok";
        case Resolve::NoNetGame: return "no CR15NetGame -- there is no session";
        case Resolve::NoGameSpace: return "the netGame carries no game space at NETGAME.CS_SPACE_OFF";
        case Resolve::GameSpaceUnreadable: return "the netGame's game space field is not mapped";
        case Resolve::BuildMismatch: return "CS.LOOKUP_BY_SYMBOL does not hold the recorded bytes";
        case Resolve::NoVoipCs: return "this gamespace has no R14NetVoipReceiver component system";
        case Resolve::UnreadableCs: return "the receiver system's own fields are not mapped";
        case Resolve::NoInstances: return "the receiver system holds no instances";
        case Resolve::SlotOutOfRange: return "the user slot is past the netGame slot tables";
        case Resolve::NoGeneration: return "the generation table holds no entrant in that slot";
        case Resolve::NoSuchTalker: return "no live receiver instance carries that user id";
        case Resolve::TableDisagree: return "the two pool maps disagree; this is not the recorded layout";
    }
    return "unknown";
}

const char* GateText(Gate g) {
    switch (g) {
        case Gate::Open: return "open (gate 4, the engine's own mute rule, is left to the engine)";
        case Gate::NoProvider: return "gate 1: the voice provider is null, so ReceiveVoip returns at once";
        case Gate::NoNetGameSub: return "gate 2: the netGame sub-object ReceiveVoip reads is null";
        case Gate::NotConnected: return "gate 3: this instance is not connected to a talker";
        case Gate::Unreadable: return "a gate's own memory is not mapped, so the answer is unknown";
    }
    return "unknown";
}

// The same field ResolveVoipSite reads first, read the same guarded way and nothing further. It is
// deliberately not folded into ResolveVoipSite's own output: the caller needs this WITHOUT making
// the engine call, and a function that answered it by resolving would defeat its own purpose.
VoipSpaceKey VoipSpaceKeyOf(void* netGame) {
    VoipSpaceKey key;
    if (g_bindEpoch != nullptr) {
        key.bind = g_bindEpoch();
    }
    key.netGame = netGame;
    if (netGame == nullptr) {
        return key;
    }
    void* space = nullptr;
    if (!ReadPtr(At(netGame, facts::NETGAME_CS_SPACE_OFF), &space)) {
        return key;
    }
    key.readable = true;
    key.space = space;
    // The table block's base, guarded like everything else here. A null space carries no header, so
    // `tableRead` stays false and two such keys still compare equal -- absence is a state, not a
    // value that happens to be zero.
    if (space != nullptr) {
        void* base = nullptr;
        if (ReadPtr(At(space, facts::CS_TABLE_OFF), &base)) {
            key.tableRead = true;
            key.tableBase = reinterpret_cast<uint64_t>(base);
        }
    }
    return key;
}

bool ResolveVoipSite(void* netGame, VoipSite* out, Resolve* why) {
    if (out == nullptr) {
        return false;
    }
    *out = VoipSite{};
    if (netGame == nullptr) {
        Set(why, Resolve::NoNetGame);
        return false;
    }
    // ★ THE COMPONENT-SYSTEM SPACE IS netGame+0x2B08, NOT netGame+0x8. The +0x8 pointer is the
    // CR15Game the netGame belongs to -- an argument to the identity and actor routines and never a
    // CncaGameSpace -- so a lookup through it could not have found this system and never did
    // (ARCH-10 S1.1/S1.3). W3-C hands nothing to those routines, so nothing here stays on +0x8.
    void* gameSpace = nullptr;
    if (!ReadPtr(At(netGame, facts::NETGAME_CS_SPACE_OFF), &gameSpace)) {
        Set(why, Resolve::GameSpaceUnreadable);
        return false;
    }
    if (gameSpace == nullptr) {
        Set(why, Resolve::NoGameSpace);
        return false;
    }
    ComponentLookupFn lookup = g_lookupForTest;
    if (lookup == nullptr) {
        if (!buildguard::VerifySite("CS.LOOKUP_BY_SYMBOL", voip_facts::kLookupBySymbolVa,
                                    voip_facts::kLookupBySymbolPrologue, 20)) {
            Set(why, Resolve::BuildMismatch);
            return false;
        }
        const void* mapped = buildguard::MappedAddress(voip_facts::kLookupBySymbolVa);
        if (mapped == nullptr) {
            Set(why, Resolve::BuildMismatch);
            return false;
        }
        lookup = reinterpret_cast<ComponentLookupFn>(const_cast<void*>(mapped));
    }
    void* cs = lookup(gameSpace, voip_facts::kReceiverCsSymbol);
    if (cs == nullptr) {
        Set(why, Resolve::NoVoipCs);
        return false;
    }
    CsTables tables{};
    VoipSite candidate{cs, netGame};
    if (!ReadTables(candidate, &tables)) {
        Set(why, Resolve::UnreadableCs);
        return false;
    }
    *out = candidate;
    Set(why, Resolve::Ok);
    return true;
}

bool SameReceiverStillLive(void* liveNetGame, const VoipSite& bound, bool* outRetryable) {
    if (outRetryable != nullptr) {
        *outRetryable = false;
    }
    if (liveNetGame == nullptr || !bound.Valid() || liveNetGame != bound.netGame) {
        return false;
    }
    void* space = nullptr;
    if (!ReadPtr(At(liveNetGame, facts::NETGAME_CS_SPACE_OFF), &space) || space == nullptr) {
        return false;
    }
    // Judged before the lookup, because the lookup is what would sort it. NotReady and Unreadable
    // are both "do not enter the engine here", and on this path both mean abandon: a teardown that
    // cannot verify what it is about to write into does not write.
    const tableguard::TableCheck table =
        tableguard::CheckSpaceTable(space, facts::CS_TABLE_OFF, kCsEntryStride, false);
    if (table.verdict != tableguard::TableVerdict::Ready) {
        // The one refusal here that says "not yet" rather than "not ours": bit 0 is set because a
        // system registered since the last lookup, and the next lookup by anyone clears it.
        if (outRetryable != nullptr && table.why == tableguard::TableWhy::SortPending) {
            *outRetryable = true;
        }
        return false;
    }
    VoipSite live;
    if (!ResolveVoipSite(liveNetGame, &live, nullptr)) {
        return false;
    }
    // Pointer equality and nothing weaker. The whole point is that a rebuilt system answers a
    // DIFFERENT address out of an identical-looking space.
    return live.cs == bound.cs && live.netGame == bound.netGame;
}

bool UserIdOf(void* netGame, uint16_t userSlot, uint32_t* outUserId, Resolve* why) {
    if (outUserId == nullptr) {
        return false;
    }
    *outUserId = 0;
    if (netGame == nullptr) {
        Set(why, Resolve::NoNetGame);
        return false;
    }
    if (userSlot >= voip_facts::kUserSlotTableMax) {
        Set(why, Resolve::SlotOutOfRange);
        return false;
    }
    const uint32_t stride = 4u * userSlot;
    uint16_t generation = 0;
    if (!ReadU16(At(netGame, voip_facts::kGenTableOff + stride), &generation)) {
        Set(why, Resolve::NoGeneration);
        return false;
    }
    // 0xFFFF is the engine's own "nobody": ENTITY.IS_LOCALLY_CONTROLLED rejects an owner whose
    // HIWORD is 0xFFFF before it reads the table at all.
    if (generation == 0xFFFFu) {
        Set(why, Resolve::NoGeneration);
        return false;
    }
    *outUserId = MakeUserId(userSlot, generation);
    Set(why, Resolve::Ok);
    return true;
}

bool InstanceCount(const VoipSite& site, uint16_t* out) {
    if (out == nullptr || !site.Valid()) {
        return false;
    }
    return ReadU16(At(site.cs, voip_facts::kCsCountOff), out);
}

bool ReadInstance(const VoipSite& site, uint16_t dense, VoipInstanceRow* out) {
    if (out == nullptr || !site.Valid()) {
        return false;
    }
    *out = VoipInstanceRow{};
    out->dense = dense;
    CsTables t{};
    if (!ReadTables(site, &t) || dense >= t.count) {
        return false;
    }
    const uint8_t* ci = t.ciBase + static_cast<size_t>(voip_facts::kCiStride) * dense;
    uint16_t poolIndex = 0;
    if (!ReadU16(t.denseMap + 2u * dense, &poolIndex)) {
        return false;
    }
    out->poolIndex = poolIndex;
    const uint8_t* entry = t.poolTable + static_cast<size_t>(voip_facts::kPoolEntryStride) * poolIndex;
    if (!ReadU16(entry + voip_facts::kPoolGenOff, &out->generation)) {
        return false;
    }
    out->readable = ReadU32(ci + voip_facts::kCiUserIdOff, &out->userId) &&
                    ReadU32(ci + voip_facts::kCiTalkerAOff, &out->talkerA) &&
                    ReadU32(ci + voip_facts::kCiTalkerBOff, &out->talkerB) &&
                    ReadU32(ci + voip_facts::kCiDecoderStateOff, &out->decoderState);
    return out->readable;
}

bool HandleOf(const VoipSite& site, uint32_t userId, uint32_t* outHandle, const uint8_t** outCi,
              Resolve* why) {
    if (outHandle == nullptr || outCi == nullptr) {
        return false;
    }
    *outHandle = 0;
    *outCi = nullptr;
    if (!site.Valid()) {
        Set(why, Resolve::NoNetGame);
        return false;
    }
    CsTables t{};
    if (!ReadTables(site, &t)) {
        Set(why, Resolve::UnreadableCs);
        return false;
    }
    if (t.count == 0) {
        Set(why, Resolve::NoInstances);
        return false;
    }

    // The dispatcher's own scan: the first live instance whose SR15NetUserId matches.
    uint16_t dense = 0;
    bool found = false;
    for (uint16_t i = 0; i < t.count; ++i) {
        uint32_t id = 0;
        const uint8_t* ci = t.ciBase + static_cast<size_t>(voip_facts::kCiStride) * i;
        if (!ReadU32(ci + voip_facts::kCiUserIdOff, &id)) {
            Set(why, Resolve::UnreadableCs);
            return false;
        }
        if (id == userId) {
            dense = i;
            found = true;
            break;
        }
    }
    if (!found) {
        Set(why, Resolve::NoSuchTalker);
        return false;
    }

    uint16_t poolIndex = 0;
    if (!ReadU16(t.denseMap + 2u * dense, &poolIndex)) {
        Set(why, Resolve::UnreadableCs);
        return false;
    }
    const uint8_t* entry = t.poolTable + static_cast<size_t>(voip_facts::kPoolEntryStride) * poolIndex;
    uint16_t backDense = 0;
    uint16_t generation = 0;
    if (!ReadU16(entry, &backDense) || !ReadU16(entry + voip_facts::kPoolGenOff, &generation)) {
        Set(why, Resolve::UnreadableCs);
        return false;
    }
    // ★ The round trip. VOIP.RECEIVE resolves the handle by exactly this reverse hop, so a pool
    // table that does not lead back is a layout we do not have, and the honest answer is a refusal
    // rather than a handle whose low word happens to index a live instance.
    if (backDense != dense) {
        Set(why, Resolve::TableDisagree);
        return false;
    }

    *outHandle = static_cast<uint32_t>(poolIndex) | (static_cast<uint32_t>(generation) << 16);
    *outCi = t.ciBase + static_cast<size_t>(voip_facts::kCiStride) * dense;
    Set(why, Resolve::Ok);
    return true;
}

const char* BindText(Bind b) {
    switch (b) {
        case Bind::AlreadyBound: return "already bound by the session; not ours to release";
        case Bind::Bound: return "bound through the engine's own Connect";
        case Bind::NoSite: return "no receiver system";
        case Bind::NoFreeInstance: return "every receiver instance is spoken for";
        case Bind::BuildMismatch: return "VOIP.CONNECT does not hold its recorded prologue";
        case Bind::Unreadable: return "the receiver system's tables are not mapped";
        case Bind::NoSentinel: return "the no-user id could not be read, so free could not be told from taken";
    }
    return "unknown";
}

void SetBindCallsForTest(ConnectFn connect, DisconnectFn disconnect, const uint32_t* sentinel) {
    g_connectForTest = connect;
    g_disconnectForTest = disconnect;
    g_sentinelForTest = sentinel;
}

Bind BindTalker(const VoipSite& site, uint32_t userId, uint16_t* outPoolIndex) {
    if (outPoolIndex != nullptr) {
        *outPoolIndex = 0;
    }
    if (!site.Valid()) {
        return Bind::NoSite;
    }
    CsTables t{};
    if (!ReadTables(site, &t)) {
        return Bind::Unreadable;
    }
    uint32_t noUser = 0;
    if (!ReadNoUserSentinel(&noUser)) {
        return Bind::NoSentinel;
    }

    // ★ TAKEN BEFORE FREE, IN TWO PASSES, DELIBERATELY. A single pass that bound the first free
    // slot it met would connect a second instance for a user who already has one whenever the
    // free slot came first -- two instances carrying one id, and the dispatcher's own scan takes
    // whichever it reaches first. The engine would not complain and the voice would be routed by
    // array order.
    int freeDense = -1;
    for (uint16_t i = 0; i < t.count; ++i) {
        uint32_t id = 0;
        if (!ReadU32(t.ciBase + static_cast<size_t>(voip_facts::kCiStride) * i +
                         voip_facts::kCiUserIdOff,
                     &id)) {
            return Bind::Unreadable;
        }
        if (id == userId) {
            uint16_t poolIndex = 0;
            if (!ReadU16(t.denseMap + 2u * i, &poolIndex)) {
                return Bind::Unreadable;
            }
            if (outPoolIndex != nullptr) {
                *outPoolIndex = poolIndex;
            }
            return Bind::AlreadyBound;
        }
        if (freeDense < 0 && id == noUser) {
            freeDense = static_cast<int>(i);
        }
    }
    if (freeDense < 0) {
        return Bind::NoFreeInstance;
    }

    const uint16_t dense = static_cast<uint16_t>(freeDense);
    uint16_t poolIndex = 0;
    if (!ReadU16(t.denseMap + 2u * dense, &poolIndex)) {
        return Bind::Unreadable;
    }
    // The same round trip HandleOf makes, and for a sharper reason: Connect indexes CS+0xD0 with
    // this pool index, so a wrong one binds a DIFFERENT instance -- and binding is a write.
    const uint8_t* entry =
        t.poolTable + static_cast<size_t>(voip_facts::kPoolEntryStride) * poolIndex;
    uint16_t backDense = 0;
    if (!ReadU16(entry, &backDense) || backDense != dense) {
        return Bind::Unreadable;
    }

    ConnectFn connect = g_connectForTest;
    if (connect == nullptr) {
        if (!buildguard::VerifySite("VOIP.CONNECT", voip_facts::kConnectVa,
                                    voip_facts::kConnectPrologue, 20)) {
            return Bind::BuildMismatch;
        }
        const void* mapped = buildguard::MappedAddress(voip_facts::kConnectVa);
        if (mapped == nullptr) {
            return Bind::BuildMismatch;
        }
        connect = reinterpret_cast<ConnectFn>(const_cast<void*>(mapped));
    }
    connect(site.cs, poolIndex, userId);

    // ★ ADVANCE ON EFFECT, NEVER ON INTENT. Connect returns void, so the only way to know it took
    // is to read back the field it writes. A caller that recorded ownership on the strength of
    // having made the call would try to release a slot it never held.
    uint32_t after = 0;
    if (!ReadU32(t.ciBase + static_cast<size_t>(voip_facts::kCiStride) * dense +
                     voip_facts::kCiUserIdOff,
                 &after) ||
        after != userId) {
        return Bind::Unreadable;
    }
    if (outPoolIndex != nullptr) {
        *outPoolIndex = poolIndex;
    }
    return Bind::Bound;
}

// poolIndex -> dense -> the CI's SR15NetUserId. The same three reads the post-hoc check made; they
// are a function now because the whole point of F3L-1 is that they run on BOTH sides of the write.
//
// ★ AND ITS TWO FAILURES ARE NOT ONE FACT (AUD-F3M F3M-3). A dense index outside the live range
// means the CI entry the slot named is GONE; memory that will not read means we could not look.
// Before the write the two refuse alike -- neither is proof the slot is still ours. After it they
// are opposites: the first is what a release LOOKS like when Disconnect compacts the dense array,
// and folding it into the second files a slot that came back under the counter for leaks.
enum class PoolRead { Ok, DenseOutOfRange, Unreadable };

static PoolRead UserIdAtPool(const CsTables& t, uint16_t poolIndex, uint32_t* outUserId) {
    const uint8_t* entry =
        t.poolTable + static_cast<size_t>(voip_facts::kPoolEntryStride) * poolIndex;
    uint16_t dense = 0;
    if (!ReadU16(entry, &dense)) {
        return PoolRead::Unreadable;
    }
    if (dense >= t.count) {
        return PoolRead::DenseOutOfRange;
    }
    return ReadU32(t.ciBase + static_cast<size_t>(voip_facts::kCiStride) * dense +
                       voip_facts::kCiUserIdOff,
                   outUserId)
               ? PoolRead::Ok
               : PoolRead::Unreadable;
}

Release UnbindTalker(const VoipSite& site, uint16_t poolIndex, uint32_t ownedUserId) {
    if (!site.Valid()) {
        return Release::NoSite;
    }
    CsTables t{};
    uint32_t noUser = 0;
    if (!ReadTables(site, &t) || !ReadNoUserSentinel(&noUser)) {
        return Release::Unreadable;
    }

    // ★ THE PRE-CHECK, AND IT IS THE POINT OF THIS FUNCTION (AUD-F3L F3L-1). The receiver surviving
    // says nothing about the slot: the engine validates no pool index, so a cached one written into
    // Disconnect lands on whoever holds that slot NOW. A player who left and one who joined is
    // enough -- no allocator reuse and no rebuild required -- and the victim is a live talker in the
    // owner's real match. Refuse instead, and say which refusal it was.
    uint32_t before = 0;
    if (UserIdAtPool(t, poolIndex, &before) != PoolRead::Ok) {
        // Alike on this side on purpose: a dense index that has left the range is no more a proof
        // the slot still carries the talker we bound than an unmapped page is, and this is the side
        // where anything short of that proof must refuse.
        return Release::Unreadable;
    }
    if (before != ownedUserId) {
        return Release::PoolMoved;
    }

    DisconnectFn disconnect = g_disconnectForTest;
    if (disconnect == nullptr) {
        if (!buildguard::VerifySite("VOIP.DISCONNECT", voip_facts::kDisconnectVa,
                                    voip_facts::kDisconnectPrologue, 20)) {
            return Release::BuildMismatch;
        }
        const void* mapped = buildguard::MappedAddress(voip_facts::kDisconnectVa);
        if (mapped == nullptr) {
            return Release::BuildMismatch;
        }
        disconnect = reinterpret_cast<DisconnectFn>(const_cast<void*>(mapped));
    }
    disconnect(site.cs, poolIndex);

    // Read back, for the same reason the pre-check exists: a leaked slot is the failure this
    // prevents, and "we called Disconnect" is not evidence that the slot came back. Re-read the
    // tables -- Disconnect may compact them -- rather than trusting the copy taken above.
    CsTables after{};
    if (!ReadTables(site, &after)) {
        return Release::Unreadable;
    }
    uint32_t now = 0;
    switch (UserIdAtPool(after, poolIndex, &now)) {
        case PoolRead::Ok:
            return now == noUser ? Release::Released : Release::Unreadable;
        case PoolRead::DenseOutOfRange:
            // The instance was compacted out from under the slot, and that IS the release: the
            // engine cannot both have dropped the CI entry and still be holding our talker in it.
            return Release::Released;
        default:
            return Release::Unreadable;
    }
}

const char* ReleaseText(Release r) {
    switch (r) {
        case Release::Released:
            return "released";
        case Release::PoolMoved:
            return "pool slot no longer carries the talker we bound; not ours to disconnect";
        case Release::Unreadable:
            return "the receiver's tables could not be read";
        case Release::NoSite:
            return "no receiver";
        case Release::BuildMismatch:
            return "VOIP.DISCONNECT does not hold the recorded prologue";
    }
    return "unknown";
}

Gate InspectGates(const VoipSite& site, const uint8_t* ci, const void* provider) {
    if (!site.Valid() || ci == nullptr) {
        return Gate::Unreadable;
    }
    if (provider == nullptr) {
        return Gate::NoProvider;
    }
    void* netGameField = nullptr;
    if (!ReadPtr(At(site.cs, voip_facts::kCsNetGameOff), &netGameField) || netGameField == nullptr) {
        return Gate::Unreadable;
    }
    void* sub = nullptr;
    if (!ReadPtr(At(netGameField, voip_facts::kNetGameSubOff), &sub)) {
        return Gate::Unreadable;
    }
    if (sub == nullptr) {
        return Gate::NoNetGameSub;
    }
    uint32_t decoderState = 0;
    if (!ReadU32(ci + voip_facts::kCiDecoderStateOff, &decoderState)) {
        return Gate::Unreadable;
    }
    if (decoderState == 0) {
        return Gate::Open;   // the engine skips the two talker checks entirely in this arm
    }
    uint32_t talkerA = 0;
    uint32_t talkerB = 0;
    if (!ReadU32(ci + voip_facts::kCiTalkerAOff, &talkerA) ||
        !ReadU32(ci + voip_facts::kCiTalkerBOff, &talkerB)) {
        return Gate::Unreadable;
    }
    if (talkerA == voip_facts::kTalkerUnset || talkerB == voip_facts::kTalkerUnset) {
        return Gate::NotConnected;
    }
    return Gate::Open;
}

}  // namespace voip
}  // namespace audio
}  // namespace replay
