#include "engine/entity_emote.h"

#include <string.h>

#include <windows.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/pageprobe.h"
#include "generated/facts_generated.h"

namespace replay {
namespace entity {
namespace {

using identity_access::EmoteSource;
using identity_access::Refusal;

const EmoteChain* g_chain = nullptr;

bool Committed(const void* p, size_t n) {
    return PageSpanReadable(p, n);
}

template <typename T>
T Rd(const void* base, uint32_t off) {
    T v;
    memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

// Both engine slots, spelled the way the disassembly calls them.  The record call returns its own
// out pointer in rax (the hidden-return-object convention MSVC compiled it under), which is why the
// engine reads the id back through rax and not through the buffer it passed.
using EmoteRecordFn = void* (*)(void* self, void* out, uint64_t key, uint64_t sentinel);
using EmoteOwnsFn   = int   (*)(void* self, uint32_t emoteId);

void* SlotAt(const void* element, uint32_t byteOff) {
    const void* const vtable = Rd<const void*>(element, 0);
    if (!Committed(vtable, byteOff + sizeof(void*))) return nullptr;
    return Rd<void*>(vtable, byteOff);
}

bool SeamResolve(void* ng, UserIndex entrant, EmoteSource* out) {
    if (!out || !g_chain) return false;
    if (g_chain->netGame && g_chain->netGame != ng) return false;
    if (!g_chain->actorKey) return false;
    uint64_t key = 0;
    if (!g_chain->actorKey(ng, entrant, key)) return false;
    return ResolveEmoteOnSystem(g_chain->customizationCs, key, *out, nullptr);
}

}  // namespace

const identity_access::EmoteSourceSeam kEmoteSource = { &SeamResolve };

bool EmoteActorGuidKey(void* netGame, UserIndex entrant, uint64_t& keyOut) {
    keyOut = 0;
    // UserTableBound is the entrant count AND the span check over the user array this read is
    // about to index -- the record at USER_ARRAY_OFF + USER.STRIDE*slot, which is where the
    // SActorGUID lives.  Passing the check makes the load below in-bounds by construction.
    uint16_t entrants = 0;
    if (!identity_access::UserTableBound(netGame, entrants)) return false;
    if (entrant.v >= entrants) return false;
    const uint64_t off = uint64_t(facts::IDENT_USER_ACTOR_GUID_OFF) + sizeof(uint64_t) +
                         uint64_t(facts::USER_STRIDE) * entrant.v;
    keyOut = Rd<uint64_t>(netGame, static_cast<uint32_t>(off));
    return true;
}

CsLookupFn EngineCsLookup() {
    // Every fact address is a PREFERRED-BASE VA and echovr.exe is DYNAMICBASE: buildguard is the
    // one place that rebases, and calling a fact without it is a wild call on every slid run.
    const void* const at = buildguard::MappedAddress(facts::CS_LOOKUP_BY_SYMBOL);
    if (!Committed(at, sizeof(facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE))) return nullptr;
    if (memcmp(at, facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
               sizeof(facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE)) != 0) {
        return nullptr;
    }
    return reinterpret_cast<CsLookupFn>(const_cast<void*>(at));
}

void* ResolveCustomizationCs(void* gameSpace, CsLookupFn find) {
    if (!gameSpace) return nullptr;
    if (!find) find = EngineCsLookup();
    if (!find) return nullptr;
    return find(gameSpace, facts::IDENT_SYM_R15NETCUSTOMIZATION);
}

void SetEmoteChain(const EmoteChain* c) { g_chain = c; }
const EmoteChain* InstalledEmoteChain() { return g_chain; }

bool ResolveEmoteOnSystem(void* customizationCs, uint64_t entrantKey, EmoteSource& out,
                          Refusal* why) {
    const uint32_t kCountOff = facts::IDENT_EMOTE_CS_COUNT_OFF;
    if (!Committed(customizationCs, kCountOff + sizeof(uint64_t))) {
        if (why) *why = Refusal::EmoteNoSystem;
        return false;
    }
    const void* const array = Rd<const void*>(customizationCs, 0);
    const uint64_t    count = Rd<uint64_t>(customizationCs, kCountOff);
    if (!array || count == 0 || !Committed(array, static_cast<size_t>(count) * sizeof(void*))) {
        if (why) *why = Refusal::EmoteNoSystem;
        return false;
    }

    // The engine's buffer is a frame local at rbp+1C8 whose size no instruction publishes; only
    // its first dword is ever read back.  256 bytes is a bound, not a derived size, so it is
    // zeroed and the id is taken from the returned pointer the way the engine takes it.
    uint64_t record[32];
    for (uint64_t i = 0; i < count; ++i) {
        void* const element = Rd<void*>(array, static_cast<uint32_t>(i * sizeof(void*)));
        if (!Committed(element, sizeof(void*))) {
            if (why) *why = Refusal::EmoteVtableUnreadable;
            return false;
        }
        const EmoteRecordFn record_fn =
            reinterpret_cast<EmoteRecordFn>(SlotAt(element, facts::IDENT_EMOTE_VTSLOT_OFF));
        const EmoteOwnsFn owns_fn =
            reinterpret_cast<EmoteOwnsFn>(SlotAt(element, facts::STATS_EMOTE_VTSLOT));
        if (!record_fn || !owns_fn) {
            if (why) *why = Refusal::EmoteVtableUnreadable;
            return false;
        }
        memset(record, 0, sizeof(record));
        void* const returned = record_fn(element, record, entrantKey, ~0ull);
        if (!Committed(returned, sizeof(uint32_t))) {
            if (why) *why = Refusal::EmoteUnreadable;
            return false;
        }
        const uint32_t emoteId = Rd<uint32_t>(returned, 0);
        if (owns_fn(element, emoteId) != 0) {
            out.componentSystem = element;
            out.emoteId = emoteId;
            return true;
        }
    }
    // Every element answered, none claimed this entrant's id: the walk fell out of its bound the
    // same way the engine's does, and that is not "no emote is playing".
    if (why) *why = Refusal::EmoteNoSystem;
    return false;
}

}  // namespace entity
}  // namespace replay
