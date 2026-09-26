#include "playback/core/netvarguard.h"

#include <atomic>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/log.h"
#include "playback/core/netvarsite.h"

namespace replay {
namespace playback {
namespace netvarguard {

namespace {

constexpr uint64_t kSetFlagVa          = 0x140D45820ull;  // facts-ok: NETVAR.SET_FLAG_HANDLE pending; CR15NetVarCS::SetFlag(SCompactPoolHandle, CSymbol64, bool)
constexpr uint64_t kSetIntVa           = 0x140D46790ull;  // facts-ok: NETVAR.SET_INT_HANDLE pending; CR15NetVarCS::SetInt(SCompactPoolHandle, CSymbol64, int)
constexpr uint64_t kSetRealVa          = 0x140D47D60ull;  // facts-ok: NETVAR.SET_REAL_HANDLE pending; CR15NetVarCS::SetReal(SCompactPoolHandle, CSymbol64, float)
constexpr uint64_t kSetRealNetGameVa   = 0x140D47E50ull;  // facts-ok: NETVAR.SET_REAL_NETGAME pending; CR15NetVarCS::SetReal(CR15NetGameMT&, CSymbol64, float)
constexpr uint64_t kSetSymbolVa        = 0x140D48DD0ull;  // facts-ok: NETVAR.SET_SYMBOL_HANDLE pending; CR15NetVarCS::SetSymbol(SCompactPoolHandle, CSymbol64, CSymbol64, float)
constexpr uint64_t kSetSymbolNetGameVa = 0x140D48EC0ull;  // facts-ok: NETVAR.SET_SYMBOL_NETGAME pending; CR15NetVarCS::SetSymbol(CR15NetGameMT&, CSymbol64, CSymbol64, float)
constexpr uint8_t kSetFlagPrologue[24] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x45, 0x8B, 0xD1, 0x49, 0x8B, 0xD8, 0x8B, 0xFA, 0x48};  // facts-ok: NETVAR.SET_FLAG_HANDLE prologue
constexpr uint8_t kSetIntPrologue[24] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x44, 0x89, 0x4C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xD8, 0x8B};  // facts-ok: NETVAR.SET_INT_HANDLE prologue
constexpr uint8_t kSetRealPrologue[24] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0xF3, 0x0F, 0x11, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xD8};  // facts-ok: NETVAR.SET_REAL_HANDLE prologue
constexpr uint8_t kSetRealNetGamePrologue[24] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xA0};  // facts-ok: NETVAR.SET_REAL_NETGAME prologue
constexpr uint8_t kSetSymbolPrologue[24] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xD8, 0x8B};  // facts-ok: NETVAR.SET_SYMBOL_HANDLE prologue
constexpr uint8_t kSetSymbolNetGamePrologue[24] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xA0, 0x00};  // facts-ok: NETVAR.SET_SYMBOL_NETGAME prologue

const SiteFact kSites[kSiteCount] = {
    {"NETVAR.SET_FLAG_HANDLE", kSetFlagVa, kSetFlagPrologue, sizeof kSetFlagPrologue},
    {"NETVAR.SET_INT_HANDLE", kSetIntVa, kSetIntPrologue, sizeof kSetIntPrologue},
    {"NETVAR.SET_REAL_HANDLE", kSetRealVa, kSetRealPrologue, sizeof kSetRealPrologue},
    {"NETVAR.SET_REAL_NETGAME", kSetRealNetGameVa, kSetRealNetGamePrologue, sizeof kSetRealNetGamePrologue},
    {"NETVAR.SET_SYMBOL_HANDLE", kSetSymbolVa, kSetSymbolPrologue, sizeof kSetSymbolPrologue},
    {"NETVAR.SET_SYMBOL_NETGAME", kSetSymbolNetGameVa, kSetSymbolNetGamePrologue, sizeof kSetSymbolNetGamePrologue},
};

struct OwnedCell {
    std::atomic<uint64_t> symbol{0};
    std::atomic<uint32_t> slot{0};
};

struct State {
    std::atomic<uint32_t> mode{uint32_t(Mode::PassThrough)};
    std::atomic<uint32_t> owned_seq{0};   // odd while PublishOwned is rewriting the cells
    OwnedCell owned[kMaxOwned];
    std::atomic<uint32_t> swallowed[kMaxOwned] = {};
    std::atomic<uint32_t> passed[kMaxOwned] = {};
    std::atomic<uint8_t>  installed{0};
    // The one symbol whose HOST value is kept. Written on the way in, before the filter, so it is
    // recorded whether the write is swallowed or passed.
    std::atomic<uint64_t> watch_symbol{0};
    std::atomic<uint64_t> watch_value{0};
    std::atomic<uint32_t> watch_writes{0};
    Setters originals;
};
State g;

HookCreateFn g_hookCreate = &hookmgr::Create;
HookArmFn    g_hookArm = &hookmgr::Arm;
VerifySiteFn g_verifySite = &buildguard::VerifySite;

// A read that raced a publish owns nothing: the write passes, which is what the host did anyway.
size_t OwnedIndex(uint16_t slot, uint64_t symbol) {
    if (symbol == 0) return kMaxOwned;
    const uint32_t before = g.owned_seq.load(std::memory_order_acquire);
    if (before & 1u) return kMaxOwned;
    size_t found = kMaxOwned;
    for (size_t i = 0; i < kMaxOwned; ++i) {
        if (g.owned[i].symbol.load(std::memory_order_relaxed) == symbol &&
            g.owned[i].slot.load(std::memory_order_relaxed) == slot) {
            found = i;
            break;
        }
    }
    return g.owned_seq.load(std::memory_order_acquire) == before ? found : kMaxOwned;
}

bool Swallow(uint16_t slot, uint64_t symbol) {
    const size_t i = OwnedIndex(slot, symbol);
    if (i == kMaxOwned) return false;
    if (g.mode.load(std::memory_order_relaxed) == uint32_t(Mode::Filter)) {
        g.swallowed[i].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g.passed[i].fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Only the host reaches a detour -- our own writes call the trampolines -- so whatever lands here
// is the host's value, for any slot. The value is published before the count, so a reader that sees
// a count has a value to go with it.
void NoteHostSymbol(uint64_t symbol, uint64_t value) {
    if (symbol == 0 || g.watch_symbol.load(std::memory_order_relaxed) != symbol) return;
    g.watch_value.store(value, std::memory_order_release);
    g.watch_writes.fetch_add(1, std::memory_order_release);
}

bool PassThrough() { return g.mode.load(std::memory_order_relaxed) == uint32_t(Mode::PassThrough); }

uint16_t HandleSlot(uint32_t handle) { return uint16_t(handle & 0xFFFFu); }  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field

// The netGame forms address the netGame's global netvar handle; the original's first act is the same
// unguarded read.
uint16_t GlobalSlot(const void* netGame) {
    uint32_t handle;
    memcpy(&handle, static_cast<const uint8_t*>(netGame) + kNetGameNetVarHandleOff, sizeof handle);
    return HandleSlot(handle);
}

void __fastcall SetFlagDetour(void* cs, uint32_t handle, uint64_t symbol, uint8_t value) {
    if (!PassThrough() && Swallow(HandleSlot(handle), symbol)) return;
    g.originals.setFlag(cs, handle, symbol, value);
}

void __fastcall SetIntDetour(void* cs, uint32_t handle, uint64_t symbol, uint32_t value) {
    if (!PassThrough() && Swallow(HandleSlot(handle), symbol)) return;
    g.originals.setInt(cs, handle, symbol, value);
}

void __fastcall SetRealDetour(void* cs, uint32_t handle, uint64_t symbol, float value) {
    if (!PassThrough() && Swallow(HandleSlot(handle), symbol)) return;
    g.originals.setReal(cs, handle, symbol, value);
}

void __fastcall SetRealNetGameDetour(void* netGame, uint64_t symbol, float value) {
    if (!PassThrough() && Swallow(GlobalSlot(netGame), symbol)) return;
    g.originals.setRealNetGame(netGame, symbol, value);
}

void __fastcall SetSymbolDetour(void* cs, uint32_t handle, uint64_t symbol, uint64_t value,
                                float lifetime) {
    NoteHostSymbol(symbol, value);
    if (!PassThrough() && Swallow(HandleSlot(handle), symbol)) return;
    g.originals.setSymbol(cs, handle, symbol, value, lifetime);
}

void __fastcall SetSymbolNetGameDetour(void* netGame, uint64_t symbol, uint64_t value,
                                       float lifetime) {
    NoteHostSymbol(symbol, value);
    if (!PassThrough() && Swallow(GlobalSlot(netGame), symbol)) return;
    g.originals.setSymbolNetGame(netGame, symbol, value, lifetime);
}

const Setters kDetours = {&SetFlagDetour,        &SetIntDetour,    &SetRealDetour,
                          &SetRealNetGameDetour, &SetSymbolDetour, &SetSymbolNetGameDetour};

void* DetourAt(size_t i) {
    switch (i) {
        case kSiteSetFlag: return reinterpret_cast<void*>(kDetours.setFlag);
        case kSiteSetInt: return reinterpret_cast<void*>(kDetours.setInt);
        case kSiteSetReal: return reinterpret_cast<void*>(kDetours.setReal);
        case kSiteSetRealNetGame: return reinterpret_cast<void*>(kDetours.setRealNetGame);
        case kSiteSetSymbol: return reinterpret_cast<void*>(kDetours.setSymbol);
        default: return reinterpret_cast<void*>(kDetours.setSymbolNetGame);
    }
}

void** OriginalAt(Setters& s, size_t i) {
    switch (i) {
        case kSiteSetFlag: return reinterpret_cast<void**>(&s.setFlag);
        case kSiteSetInt: return reinterpret_cast<void**>(&s.setInt);
        case kSiteSetReal: return reinterpret_cast<void**>(&s.setReal);
        case kSiteSetRealNetGame: return reinterpret_cast<void**>(&s.setRealNetGame);
        case kSiteSetSymbol: return reinterpret_cast<void**>(&s.setSymbol);
        default: return reinterpret_cast<void**>(&s.setSymbolNetGame);
    }
}

}  // namespace

const SiteFact& SiteAt(Site site) { return kSites[site < kSiteCount ? site : 0]; }

bool Install() {
    if (Installed()) return true;

    const char* resolved = config::Interface()->GetString(kLedgerKey, "auto");
    if (resolved == nullptr || strcmp(resolved, "hook") != 0) {
        log::Direct(LogLevel::Info, "playback",
                    "host netvar guard: not installed (%s is '%s'; bootstrap resolves it to 'hook' "
                    "only for a contained process)",
                    kLedgerKey, resolved != nullptr ? resolved : "");
        return false;
    }

    for (const SiteFact& site : kSites) {
        if (!g_verifySite(site.factId, site.va, site.prologue, site.prologueLen)) {
            log::Direct(LogLevel::Warn, "playback",
                        "host netvar guard: %s does not hold its recorded prologue; none of the %u "
                        "setter detours is installed and host netvar writes pass unfiltered",
                        site.factId, unsigned(kSiteCount));
            return false;
        }
    }

    Setters trampolines;
    for (size_t i = 0; i < kSiteCount; ++i) {
        const SiteFact& site = kSites[i];
        const HookStatus created =
            g_hookCreate(site.factId, site.va, site.prologue, site.prologueLen, DetourAt(i),
                         OriginalAt(trampolines, i), kLedgerKey);
        if (created != HookStatus::Created) {
            log::Direct(LogLevel::Warn, "playback",
                        "host netvar guard: %s not created (%s); no setter detour is armed",
                        site.factId, HookStatusName(created));
            return false;
        }
    }
    // Before the first Arm: an armed detour forwards through these on its very next call.
    g.originals = trampolines;

    size_t armed = 0;
    for (const SiteFact& site : kSites) {
        const HookStatus status = g_hookArm(site.factId);
        if (status != HookStatus::Armed) {
            log::Direct(LogLevel::Warn, "playback", "host netvar guard: %s not armed (%s)",
                        site.factId, HookStatusName(status));
            continue;
        }
        ++armed;
    }
    if (armed != kSiteCount) {
        // A filter with a hole swallows some of the host's writers and not the others; the armed
        // detours stay pass-through for the life of the process.
        log::Direct(LogLevel::Error, "playback",
                    "host netvar guard: %u of %u setter detours armed; the filter stays off",
                    unsigned(armed), unsigned(kSiteCount));
        return false;
    }

    g.installed.store(1, std::memory_order_release);
    log::Direct(LogLevel::Info, "playback",
                "host netvar guard: %u CR15NetVarCS setter detours armed (ledger key %s), "
                "pass-through until a replay drives a match",
                unsigned(kSiteCount), kLedgerKey);
    return true;
}

bool Installed() { return g.installed.load(std::memory_order_acquire) != 0; }

const Setters& Originals() { return g.originals; }

void PublishOwned(const OwnedKey keys[kMaxOwned]) {
    g.owned_seq.fetch_add(1, std::memory_order_release);
    for (size_t i = 0; i < kMaxOwned; ++i) {
        g.owned[i].symbol.store(keys[i].symbol, std::memory_order_relaxed);
        g.owned[i].slot.store(keys[i].slot, std::memory_order_relaxed);
    }
    g.owned_seq.fetch_add(1, std::memory_order_release);
}

void SetMode(Mode mode) {
    if (!Installed()) mode = Mode::PassThrough;
    g.mode.store(uint32_t(mode), std::memory_order_release);
}

Mode CurrentMode() { return Mode(g.mode.load(std::memory_order_relaxed)); }

void DrainCounts(Counts& out) {
    for (size_t i = 0; i < kMaxOwned; ++i) {
        out.swallowed[i] = g.swallowed[i].exchange(0, std::memory_order_relaxed);
        out.passed[i] = g.passed[i].exchange(0, std::memory_order_relaxed);
    }
}

void SetHookAccessForTest(HookCreateFn create, HookArmFn arm, VerifySiteFn verify) {
    g_hookCreate = create != nullptr ? create : &hookmgr::Create;
    g_hookArm = arm != nullptr ? arm : &hookmgr::Arm;
    g_verifySite = verify != nullptr ? verify : &buildguard::VerifySite;
}

void WatchHostSymbol(uint64_t symbol) {
    g.watch_symbol.store(symbol, std::memory_order_release);
}

HostSymbol LastHostSymbol() {
    HostSymbol out;
    out.writes = g.watch_writes.load(std::memory_order_acquire);
    out.value = g.watch_value.load(std::memory_order_acquire);
    return out;
}

void NoteHostSymbolForTest(uint64_t symbol, uint64_t value) {
    if (symbol == 0) {
        g.watch_value.store(0, std::memory_order_release);
        g.watch_writes.store(0, std::memory_order_release);
        return;
    }
    NoteHostSymbol(symbol, value);
}

const Setters& DetoursForTest() { return kDetours; }

void SetOriginalsForTest(const Setters& originals) {
    g.originals = originals;
    g.installed.store(1, std::memory_order_release);
}

void ResetForTest() {
    g.mode.store(uint32_t(Mode::PassThrough), std::memory_order_release);
    g.installed.store(0, std::memory_order_release);
    g.originals = Setters{};
    g.watch_symbol.store(0, std::memory_order_release);
    g.watch_value.store(0, std::memory_order_release);
    g.watch_writes.store(0, std::memory_order_release);
    const OwnedKey none[kMaxOwned] = {};
    PublishOwned(none);
    Counts discard;
    DrainCounts(discard);
    SetHookAccessForTest(nullptr, nullptr, nullptr);
}

}  // namespace netvarguard
}  // namespace playback
}  // namespace replay
