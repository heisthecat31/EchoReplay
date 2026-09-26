#include "playback/core/netvarsite.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "generated/facts_generated.h"
#include "playback/core/apply.h"

namespace replay {
namespace playback {
namespace {

const uint8_t* At(const void* base, uint32_t off) {
    return static_cast<const uint8_t*>(base) + off;
}

// Every read of engine memory in this file goes through here. A slot we cannot read is not a slot
// that holds null: the two are different answers and only this one is honest about which.
void* Deref(const void* base, uint32_t off) {
    if (!base) return nullptr;
    const uint8_t* p = At(base, off);
    if (!Readable(p, sizeof(void*))) return nullptr;
    void* v = nullptr;
    memcpy(&v, p, sizeof v);
    return v;
}

bool ReadU64(const void* base, uint32_t off, uint64_t& out) {
    if (!base) return false;
    const uint8_t* p = At(base, off);
    if (!Readable(p, sizeof out)) return false;
    memcpy(&out, p, sizeof out);
    return true;
}

// The bound table. A static rather than a caller-owned object because the applier holds the pointer
// across ticks and nothing else in this packet has a lifetime long enough to own it. Re-binding
// re-constructs in place: EngineNetVarTable owns no resource, so the previous one needs no teardown
// and the applier's pointer stays valid across the swap.
alignas(EngineNetVarTable) unsigned char g_tableStorage[sizeof(EngineNetVarTable)];
EngineNetVarTable* g_table = nullptr;

}  // namespace

const char* NetVarSiteBindText(NetVarSiteBind b) {
    switch (b) {
        case NetVarSiteBind::NoNetGame:        return "no CR15NetGame -- no session is loaded";
        case NetVarSiteBind::NoGame:
            return "the netGame's global netvar handle is empty -- no global_vars component exists";
        case NetVarSiteBind::NoNetVarCs:
            return "the netGame's global netvar handle carries no component or no slot";
        case NetVarSiteBind::NetGameMismatch:
            return "the handle's component names another CR15NetGame at +0xB8 -- refused rather "
                   "than bound";
        case NetVarSiteBind::SpaceMismatch:
            return "the handle's component is not in the netGame's current level space";
        case NetVarSiteBind::TableNotReady:
            return "the update-record table at cs+0x100 cannot be walked yet";
        case NetVarSiteBind::NoRecord:
            return "the component's pool holds no update record for the handle's slot";
        case NetVarSiteBind::PoolUnreadable:
            return "the component's pool at cs+0xC8 could not be read";
        case NetVarSiteBind::GenerationMismatch:
            return "the handle's slot was recycled -- the pool's generation for it is not the "
                   "handle's";
        case NetVarSiteBind::StoreUnreadable:
            return "the route holds, but the slot's update record or its store could not be read";
        case NetVarSiteBind::NoCaptureLevelId:
            return "the capture records no level id, so no rowset owner could be compared";
        case NetVarSiteBind::Ok:               return "bound";
    }
    return "unstated";
}

NetVarSite LocateNetVarSite(void* netGame) {
    NetVarSite s;
    if (!netGame) return s;

    s.game = Deref(netGame, kNetGameNetVarGameOff);
    if (!s.game) {
        s.bind = NetVarSiteBind::NoGame;
        return s;
    }
    // Logged, never compared: the snapshot is taken once and the counter moves with every spawn.
    ReadU64(netGame, kNetGameNetVarGenerationOff, s.cachedGeneration);
    ReadU64(s.game, kGameGenerationOff, s.liveGeneration);

    void* const cs = Deref(netGame, kNetGameNetVarCsOff);
    uint32_t handle = 0;
    const uint8_t* hp = At(netGame, kNetGameNetVarHandleOff);
    if (!cs || !Readable(hp, sizeof handle)) {
        s.bind = NetVarSiteBind::NoNetVarCs;
        return s;
    }
    memcpy(&handle, hp, sizeof handle);
    s.handle = handle;
    if (s.Slot() == kNetVarSlotNone) {
        s.bind = NetVarSiteBind::NoNetVarCs;
        return s;
    }

    // InitNetVarCI hands the handle to `[cs+0xB8]` (0x140CF7269), so a component whose +0xB8 is not
    // the netGame we walked from did not fill this handle.
    if (Deref(cs, facts::NETVAR_LEVEL_NETGAME_OFF) != netGame) {
        s.bind = NetVarSiteBind::NetGameMismatch;
        return s;
    }

    // A component of a space the netGame has left keeps its handle filled until RemoveReferences
    // runs. Where the component lives tells it apart; the generation never could.
    s.space = Deref(cs, kNetVarOwnerPtrOff);
    s.levelSpace = Deref(netGame, kNetGameLevelSpaceOff);
    if (!s.space || s.space != s.levelSpace) {
        s.bind = NetVarSiteBind::SpaceMismatch;
        return s;
    }
    s.netVarCs = cs;

    s.pool = WalkNetVarPool(cs, s.Slot());
    s.records = s.pool.count;
    switch (s.pool.step) {
        case NetVarPoolStep::NoComponent:
        case NetVarPoolStep::HeaderUnreadable:
        case NetVarPoolStep::SlotOverCapacity:
        case NetVarPoolStep::SlotMapUnreadable:
            s.bind = NetVarSiteBind::PoolUnreadable;
            return s;
        case NetVarPoolStep::SlotUnmapped:
        case NetVarPoolStep::IndexOverCount:
            s.bind = NetVarSiteBind::NoRecord;
            return s;
        case NetVarPoolStep::Walked:
        case NetVarPoolStep::RecordUnreadable:
        case NetVarPoolStep::StoreUnreadable:
            break;
    }
    // 140C80981: the appender packs the slot map's generation into the handle's high word, and a
    // recycled slot gets a new one. The cs-form SetInt indexes by slot alone, so this is its guard.
    if (s.pool.generation != s.HandleGeneration()) {
        s.bind = NetVarSiteBind::GenerationMismatch;
        return s;
    }
    s.route = true;

    if (s.pool.step != NetVarPoolStep::Walked) {
        s.bind = NetVarSiteBind::StoreUnreadable;
        return s;
    }
    s.store = s.pool.store;
    s.bind = NetVarSiteBind::Ok;
    return s;
}

void FormatNetVarSite(const NetVarSite& s, char* line, size_t cap) {
    if (!line || !cap) return;
    snprintf(line, cap,
             "%s [pool walk: %s] cs=%p handle=0x%08X slot=%u generation=%u; pool capacity=%u "
             "count=%u prefix=%u slotmap=%p records=%p -> index=%u generation=%u record=%p store=%p",
             NetVarSiteBindText(s.bind), NetVarPoolStepText(s.pool.step), s.netVarCs,
             (unsigned)s.handle, (unsigned)s.Slot(), (unsigned)s.HandleGeneration(),
             (unsigned)s.pool.capacity, (unsigned)s.pool.count, (unsigned)s.pool.prefix,
             (const void*)s.pool.slotMap, (const void*)s.pool.records, (unsigned)s.pool.index,
             (unsigned)s.pool.generation, (const void*)s.pool.record, s.pool.store);
}

bool ReadNetGameRoleWord(void* netGame, uint64_t& word) {
    return ReadU64(Deref(netGame, facts::NETGAME_ROLEFLAGS_OFF), 0, word);
}

NetVarSite BindNetVarTableOn(StateApplier& applier, void* netGame, uint64_t captureLevelId,
                            EngineNetVarTable::StoreDecodeFn decode, char* line, size_t lineCap) {
    NetVarSite s = LocateNetVarSite(netGame);

    // Located, and still not bindable: the manifest's levelId is what ApplyRowset compares each
    // rowset's owner field against, and captures written before that field was filled carry the
    // zero-initialised default. Binding it would turn the engine's own owner check -- the check
    // that stops one level's scores being written into another -- into a guard that refuses
    // everything while looking like it passed.
    if (s.bind == NetVarSiteBind::Ok && captureLevelId == 0) {
        s.bind = NetVarSiteBind::NoCaptureLevelId;
    }

    if (s.StoreOk()) {
        g_table = new (g_tableStorage) EngineNetVarTable(s.netVarCs);
        if (decode) g_table->BindStoreDecode(decode);
        applier.BindNetVarTable(g_table, captureLevelId);
    }

    if (line && lineCap) {
        if (s.StoreOk()) {
            snprintf(line, lineCap,
                     "netvar table: bound at %p via the netGame's global handle (netgame state %d, "
                     "level space %p, slot %u, vars=%u; generation cached %llu live %llu, not a "
                     "gate)",
                     s.netVarCs, (int)NetGameStateOf(netGame), s.space, (unsigned)s.Slot(),
                     s.records, (unsigned long long)s.cachedGeneration,
                     (unsigned long long)s.liveGeneration);
        } else if (s.netVarCs) {
            char detail[512];
            FormatNetVarSite(s, detail, sizeof detail);
            snprintf(line, lineCap,
                     "netvar table: generic store decoder not bound -- %s. The HUD setter path %s",
                     detail,
                     s.RouteOk() ? "does not need the store and is unaffected"
                                 : "needs this route too and is not writing");
        } else if (s.bind == NetVarSiteBind::SpaceMismatch) {
            snprintf(line, lineCap,
                     "netvar table: not located (netgame state %d; %s: component space %p, level "
                     "space %p)",
                     (int)NetGameStateOf(netGame), NetVarSiteBindText(s.bind), s.space,
                     s.levelSpace);
        } else {
            snprintf(line, lineCap, "netvar table: not located (netgame state %d; %s)",
                     (int)NetGameStateOf(netGame), NetVarSiteBindText(s.bind));
        }
    }
    return s;
}

void UnbindNetVarTable(StateApplier& applier) {
    applier.BindNetVarTable(nullptr, 0);
    g_table = nullptr;
}

void* LiveNetGame() {
    // The SLOT is resolved once and held; the CR15Game in it is exactly the pointer that changes,
    // so it is re-read every call. Same fact, same rebasing point as W2-B's residue walk.
    static const void* slot = nullptr;
    if (!slot) slot = buildguard::MappedAddress(facts::GAME_SINGLETON_VA);
    if (!slot || !Readable(slot, sizeof(void*))) return nullptr;
    void* game = nullptr;
    memcpy(&game, slot, sizeof game);
    return Deref(game, facts::GAME_NETGAME_PTR_OFF);
}

EngineNetVarTable::StoreDecodeFn LiveStoreDecode() {
    if (!buildguard::VerifySite("NETVAR.STORE_DECODE", facts::NETVAR_STORE_DECODE,
                                facts::NETVAR_STORE_DECODE_PROLOGUE,
                                sizeof facts::NETVAR_STORE_DECODE_PROLOGUE)) {
        return nullptr;
    }
    return reinterpret_cast<EngineNetVarTable::StoreDecodeFn>(
        const_cast<void*>(buildguard::MappedAddress(facts::NETVAR_STORE_DECODE)));
}

int32_t NetGameStateOf(void* netGame) {
    if (!netGame || !Readable(netGame, sizeof(int32_t))) return -1;
    int32_t state = 0;
    memcpy(&state, netGame, sizeof state);
    return state;
}

void ResetNetVarSiteForTest() { g_table = nullptr; }

}  // namespace playback
}  // namespace replay
