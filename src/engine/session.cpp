#include "engine/session.h"

#include <windows.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include "bootstrap/attachhook.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/coexist.h"
#include "bootstrap/egress.h"
#include "bootstrap/offlinepatch.h"
#include "bootstrap/services.h"
#include "engine/session_facts.h"
#include "foundation/gametype.h"

namespace sf = replay::session_facts;

namespace {

const char* const kReplayModuleName = "session";

// ---------------------------------------------------------------------------------------------
// Reaching the engine's memory.
// ---------------------------------------------------------------------------------------------

// facts.tsv records addresses against the image base the PE declares; the loader may map it
// elsewhere. buildguard rebases the same way but does not export it, so this is a local copy
// rather than a widened interface on a header this packet does not own.
const uint8_t* Rebase(uint64_t pcVa) {
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr || pcVa < facts::PE_IMAGE_BASE) {
        return nullptr;
    }
    return base + (pcVa - facts::PE_IMAGE_BASE);
}

// ★ NO local Readable here. buildguard.h publishes replay::Readable (:85) and this file's own
// copy sat in an anonymous namespace at global scope, so the probes inside
// namespace replay::session found the PUBLISHED one by unqualified lookup and silently rebound
// to it - docs/spikes/READABLE-REBIND.md, disposition 1. The copy is gone and every remaining call
// is qualified: one function, chosen by the author rather than by a lookup rule. The published one
// is also the stricter of the two (it additionally refuses a span that wraps).

// ---------------------------------------------------------------------------------------------
// ★ THE ENGINE POINTER WALK OFF THE TICK'S CRITICAL PATH (L2-RUN-2 F1).
//
// The capture tick's `gate` phase is this file's State() and nothing else, and L2 measured it at
// 1793 us per tick -- then at 1685-1941 us per tick for four minutes AFTER the capture was killed,
// with nothing recording. The three replay::Readable probes that used to sit on this path were the
// whole of it: each is a VirtualQuery, i.e. NtQueryVirtualMemory, which takes the address space's
// lock and walks its VAD tree while the render and streaming threads churn it. ENG-W2A-R8 removed
// four of these from ReadWorldOrigin for exactly this reason; these are the rest of them, and
// S6.4's rule for the armed tick is verbatim "zero syscalls while armed".
//
// A probe is also the WEAKER guarantee: VirtualQuery answers about the instant it ran and the read
// it protects happens afterwards, so nothing stops the region being unmapped in between. A load
// under __try answers about the instant of the load itself, costs nothing on x64 when it does not
// fault (the handler is a table entry, not a runtime push), and refuses in every case the probe
// existed to refuse -- a null, a garbage or a not-yet-constructed pointer.
//
// The filter takes the three codes a single load can raise on a page this walk must refuse:
// EXCEPTION_ACCESS_VIOLATION, EXCEPTION_IN_PAGE_ERROR and STATUS_GUARD_PAGE_VIOLATION. Anything else
// raised inside a load is not ours to swallow and continues its search.
//
// ★ THE GUARD PAGE. replay::Readable, the probe these loads replaced, refused a guard
// page BY NAME -- buildguard.cpp:83, `(info.Protect & PAGE_GUARD) != 0`. Without the code below the
// replacement did not: a load landing on a PAGE_GUARD page raises 0x80000001, which is neither of the
// other two, so the filter continued the search and an unhandled exception on the GAME thread ends the
// owner's match. Every thread stack in echovr.exe ends in a guard page, so a stale pointer into
// recycled address space reaches one. The load also CONSUMES the guard on its way past, which is why
// swallowing it is right: the page is now an ordinary page and the next read of it is meaningless, so
// the only correct answer is the one Readable gave -- refuse.
// ---------------------------------------------------------------------------------------------

// STATUS_GUARD_PAGE_VIOLATION. Named here because winnt.h only defines it when WIN32_NO_STATUS is
// unset, and this file must not depend on which header a translation unit reached first. Pinned to
// the SDK's value wherever the SDK does supply one.
constexpr unsigned long kStatusGuardPageViolation = 0x80000001ul;
#ifdef STATUS_GUARD_PAGE_VIOLATION
static_assert(kStatusGuardPageViolation ==
                  static_cast<unsigned long>(STATUS_GUARD_PAGE_VIOLATION),
              "STATUS_GUARD_PAGE_VIOLATION is not 0x80000001 on this SDK");
#endif

int SafeReadFilter(unsigned long code) {
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
            code == kStatusGuardPageViolation)
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool TryLoadPointer(const void* at, uint8_t** out) {
    bool ok = false;
    __try {
        *out = *reinterpret_cast<uint8_t* const*>(at);
        ok = true;
    } __except (SafeReadFilter(GetExceptionCode())) {
        *out = nullptr;
    }
    return ok;
}

bool TryLoadInt32(const void* at, int32_t* out) {
    bool ok = false;
    __try {
        *out = *reinterpret_cast<const int32_t*>(at);
        ok = true;
    } __except (SafeReadFilter(GetExceptionCode())) {
        *out = 0;
    }
    return ok;
}

bool TryTouch(const void* at) {
    bool ok = false;
    __try {
        (void)*static_cast<const volatile uint8_t*>(at);
        ok = true;
    } __except (SafeReadFilter(GetExceptionCode())) {
        ok = false;
    }
    return ok;
}

// The slot is a fixed address inside the main image, so whether it is mapped at all is a property of
// the process and is decided once -- ENG-W2A-R8 cached the same geometry in ReadWorldOrigin for the
// same reason. It also keeps a host that is not echovr.exe (every test binary) off the fault path:
// there the slot address is unmapped, and one kernel exception dispatch per call would cost far more
// than the VirtualQuery this replaced. Published pointer-then-flag so a second thread either does the
// work again or sees a complete answer.
std::atomic<const uint8_t*> g_gamePtrSlot{nullptr};
std::atomic<bool> g_gamePtrSlotResolved{false};

const uint8_t* GamePtrSlot() {
    if (g_gamePtrSlotResolved.load(std::memory_order_acquire)) {
        return g_gamePtrSlot.load(std::memory_order_relaxed);
    }
    const uint8_t* at = Rebase(sf::kGamePtrVa);
    if (at != nullptr && !replay::Readable(at, sizeof(void*))) {
        at = nullptr;
    }
    g_gamePtrSlot.store(at, std::memory_order_relaxed);
    g_gamePtrSlotResolved.store(true, std::memory_order_release);
    return at;
}

uint8_t* GameBase() {
    const uint8_t* slot = GamePtrSlot();
    if (slot == nullptr) {
        return nullptr;
    }
    uint8_t* game = nullptr;
    if (!TryLoadPointer(slot, &game) || game == nullptr) {
        return nullptr;
    }
    // CR15Game is far larger than this, but every field this file touches lives below 0x8A78, so
    // touching the far end of that span catches a half-constructed object here rather than by a
    // fault three frames later. One allocation is one committed region, so the endpoint stands for
    // the span the VirtualQuery walk used to cover.
    if (!TryTouch(game + sf::kSessionFlagsOff + sizeof(uint64_t) - 1)) {
        return nullptr;
    }
    return game;
}

uint64_t* SessionFlagsPtr(uint8_t* game) {
    return reinterpret_cast<uint64_t*>(game + sf::kSessionFlagsOff);
}

uint64_t* ModeFlagsPtr(uint8_t* game) {
    return reinterpret_cast<uint64_t*>(game + sf::kGameModeFlagsOff);
}

// ---------------------------------------------------------------------------------------------
// Name ids, read out of the image rather than recomputed.
// ---------------------------------------------------------------------------------------------
//
// Every engine name constant sits in .rdata as an 8-byte id immediately followed by the literal
// "OlPrEfIx<name>", 8-byte aligned. ApplyCommandLine reads exactly that shape: its echo_arena
// check is *(qword*)((&"OlPrEfIxecho_arena"[-1]) & ~7). So finding the name finds its own id, and
// because the two are adjacent the lookup validates itself - there is no hash function to port,
// no prototype to guess, and a renamed constant in some other build fails to match instead of
// handing back a plausible wrong number.
//
// This is a linear scan of the initialised-data sections. Callers that need many ids should cache
// them; the shipping path takes them from the demo's MANIFEST and never calls this at all.
constexpr char kNamePrefix[] = "OlPrEfIx";

const uint8_t* FindBytes(const uint8_t* haystack, size_t haystackLen, const uint8_t* needle,
                         size_t needleLen) {
    if (needleLen == 0 || haystackLen < needleLen) {
        return nullptr;
    }
    const uint8_t* last = haystack + (haystackLen - needleLen);
    for (const uint8_t* at = haystack; at <= last; ++at) {
        if (*at == *needle && memcmp(at, needle, needleLen) == 0) {
            return at;
        }
    }
    return nullptr;
}

uint64_t LookupNameId(const char* name) {
    if (name == nullptr || *name == '\0') {
        return 0;
    }
    char pattern[192];
    const int written =
        _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s%s", kNamePrefix, name);
    if (written <= 0) {
        return 0;
    }
    // The terminator is part of the pattern, so "echo_arena" cannot match "echo_arena_private".
    const size_t patternLen = static_cast<size_t>(written) + 1;

    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr || !replay::Readable(base, sizeof(IMAGE_DOS_HEADER))) {
        return 0;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!replay::Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) == 0 ||
            (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            continue;
        }
        const uint8_t* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (!replay::Readable(start, size)) {
            continue;
        }
        const uint8_t* hit =
            FindBytes(start, size, reinterpret_cast<const uint8_t*>(pattern), patternLen);
        if (hit == nullptr) {
            continue;
        }
        // A misaligned hit is not the layout this was derived from, and reading eight bytes back
        // from it would return a shifted number that still looks like an id. Refuse instead.
        if ((reinterpret_cast<uintptr_t>(hit) % 8) != 0 || hit - 8 < start) {
            return 0;
        }
        return *reinterpret_cast<const uint64_t*>(hit - 8);
    }
    return 0;
}

using GetCommandLineAFn = LPSTR(WINAPI*)();
GetCommandLineAFn g_realGetCommandLineA = nullptr;

// ---------------------------------------------------------------------------------------------
// Command-line tokens (SE-A). Pure string work, so it is testable with no game present.
// ---------------------------------------------------------------------------------------------

// CMDLINE.TOKENIZE accepts '-', '+' and 0x96 as switch prefixes.
bool IsSwitchPrefix(char c) {
    return c == '-' || c == '+' || c == static_cast<char>(0x96);
}

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// The comparable part of a token: prefixes stripped, stopped at '=' or a separator.
size_t TokenName(const char* at, const char** nameOut) {
    while (*at != '\0' && IsSwitchPrefix(*at)) {
        ++at;
    }
    *nameOut = at;
    size_t len = 0;
    while (at[len] != '\0' && at[len] != '=' && static_cast<unsigned char>(at[len]) > 0x20) {
        ++len;
    }
    return len;
}

bool NameEquals(const char* a, size_t aLen, const char* b, size_t bLen) {
    if (aLen != bLen) {
        return false;
    }
    for (size_t i = 0; i < aLen; ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace replay {
namespace session {

bool ProbeReadableByte(const void* at) {
    return TryTouch(at);
}

bool HasToken(const char* line, const char* token) {
    if (line == nullptr || token == nullptr) {
        return false;
    }
    const char* wantName = nullptr;
    const size_t wantLen = TokenName(token, &wantName);
    if (wantLen == 0) {
        return false;
    }

    const char* at = line;
    while (*at != '\0') {
        while (*at != '\0' && static_cast<unsigned char>(*at) <= 0x20) {
            ++at;
        }
        if (*at == '\0') {
            break;
        }
        // A quoted run is a value, never a switch, so a path that happens to contain "-noovr"
        // cannot read as the switch.
        if (*at == '"' || *at == '[') {
            const char closer = (*at == '"') ? '"' : ']';
            ++at;
            while (*at != '\0' && *at != closer) {
                ++at;
            }
            if (*at != '\0') {
                ++at;
            }
            continue;
        }
        const char* haveName = nullptr;
        const size_t haveLen = TokenName(at, &haveName);
        if (NameEquals(haveName, haveLen, wantName, wantLen)) {
            return true;
        }
        while (*at != '\0' && *at != ' ') {
            ++at;
        }
    }
    return false;
}

size_t AugmentCommandLine(const char* original, const char* const* tokens, size_t tokenCount,
                          char* out, size_t outCap) {
    if (out == nullptr || outCap == 0) {
        return 0;
    }
    const char* source = (original != nullptr) ? original : "";
    const size_t sourceLen = strlen(source);
    if (sourceLen + 1 > outCap) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, source, sourceLen + 1);

    size_t len = sourceLen;
    for (size_t i = 0; i < tokenCount; ++i) {
        const char* token = tokens[i];
        if (token == nullptr || *token == '\0' || HasToken(out, token)) {
            continue;
        }
        const size_t tokenLen = strlen(token);
        // The separator must be a space: CMDLINE.TOKENIZE splits unquoted tokens on 0x20 alone,
        // so a tab here would glue our token onto the previous one.
        const size_t need = (len == 0 ? 0 : 1) + tokenLen;
        if (len + need + 1 > outCap) {
            memcpy(out, source, sourceLen + 1);
            return 0;
        }
        if (len != 0) {
            out[len++] = ' ';
        }
        memcpy(out + len, token, tokenLen);
        len += tokenLen;
        out[len] = '\0';
    }
    return len;
}

int32_t StateRaw() {
    uint8_t* game = GameBase();
    if (game == nullptr) {
        return 0;
    }
    uint8_t* netGame = nullptr;
    if (!TryLoadPointer(game + sf::kGameNetGameOff, &netGame) || netGame == nullptr) {
        return 0;
    }
    int32_t state = 0;
    return TryLoadInt32(netGame + sf::kNetGameStateOff, &state) ? state : 0;
}

const char* StateName(int32_t state) {
    switch (state) {
        case -100: return "os needs update";
        case -99:  return "obb missing";
        case -98:  return "no network";
        case -97:  return "broadcaster error";
        case -96:  return "certificate error";
        case -95:  return "service unavailable";
        case -94:  return "login failed";
        case -93:  return "login replaced";
        case -92:  return "lobby booted";
        case -91:  return "load failed";
        case 0:    return "logged out";
        case 1:    return "loading root";
        case 2:    return "logging in";
        case 3:    return "logged in";
        case 4:    return "loading global";
        case 5:    return "lobby";
        case 6:    return "server loading";
        case 7:    return "loading level";
        case 8:    return "ready for game";
        case 9:    return "in game";
        default:   return "unknown";
    }
}

uint64_t SessionFlags() {
    uint8_t* game = GameBase();
    return (game == nullptr) ? 0 : *SessionFlagsPtr(game);
}

uint64_t ModeFlags() {
    uint8_t* game = GameBase();
    return (game == nullptr) ? 0 : *ModeFlagsPtr(game);
}

const char* SessionNetworkName(SessionNetwork state) {
    switch (state) {
        case SessionNetwork::NotYetKnown:     return "not-yet-known";
        case SessionNetwork::Offline:         return "offline";
        case SessionNetwork::LiveOnlineMatch: return "live-online-match";
    }
    return "unknown";
}

SessionNetwork NetworkState() {
    uint8_t* game = GameBase();
    if (game == nullptr) {
        return SessionNetwork::NotYetKnown;
    }
    const uint64_t flags = *SessionFlagsPtr(game);
    if (sf::IsOfflineFlags(flags)) {
        return SessionNetwork::Offline;
    }
    auto* netGame = *reinterpret_cast<uint8_t* const*>(game + sf::kGameNetGameOff);
    if (!replay::Readable(netGame, sf::kNetGameStateOff + sizeof(int32_t))) {
        return SessionNetwork::NotYetKnown;
    }
    const int32_t state = *reinterpret_cast<const int32_t*>(netGame + sf::kNetGameStateOff);
    if (state >= sf::kStateReadyForGame) {
        return SessionNetwork::LiveOnlineMatch;
    }
    return SessionNetwork::NotYetKnown;
}

uint16_t HttpPortOverride() {
    uint8_t* game = GameBase();
    return (game == nullptr) ? 0 : *reinterpret_cast<const uint16_t*>(game + sf::kGameHttpPortOff);
}

bool GamespacesCreated() {
    uint8_t* game = GameBase();
    return game != nullptr && (*ModeFlagsPtr(game) & sf::kModeBitGamespaces) != 0;
}

bool MultiplayerBootSeen() {
    uint8_t* game = GameBase();
    return game != nullptr &&
           *reinterpret_cast<const uint32_t*>(game + sf::kGameBootStateOff) == 1;
}

const char* BootSuppressionName(BootSuppression result) {
    switch (result) {
        case BootSuppression::AlreadyClear:      return "already-clear";
        case BootSuppression::ClearedTooLate:    return "cleared-too-late";
        case BootSuppression::ClearedUnverified: return "cleared-unverified";
        case BootSuppression::NoGame:            return "no-game";
        case BootSuppression::RefusedByPolicy:   return "refused-by-policy";
        case BootSuppression::RefusedUncontained: return "refused-uncontained";
    }
    return "unknown";
}

uint64_t HashName(const char* name) {
    return LookupNameId(name);
}

}  // namespace session
}  // namespace replay

namespace {

// ---------------------------------------------------------------------------------------------
// The strategy.
// ---------------------------------------------------------------------------------------------

using EnterOfflineMatchFn = void (*)(void* game, void* params);

// SESSION.SPECTATE_CREATE builds exactly this and hands it to NETGAME_INIT. Reproducing that
// construction is what closes R2 S9's [U] on the layout: +0x20..+0x3F are left as stack garbage
// by three of the engine's own four construction sites, so they carry nothing.
struct SessionParams {
    uint32_t zero;       // +0x00  always 0
    uint32_t three;      // +0x04  always 3
    uint64_t gametype;   // +0x08  -> lobby+1696
    uint64_t level;      // +0x10  -> lobby+1704
    uint64_t minusOne;   // +0x18  always -1
    uint64_t unused[4];  // +0x20  not written by the engine's own constructors
    uint64_t tail;       // +0x40  always 0
};
static_assert(sizeof(SessionParams) == sf::kParamsSize, "SESSION.PARAMS_SIZE");
static_assert(offsetof(SessionParams, gametype) == sf::kParamsGametypeOff, "params+8 gametype");
static_assert(offsetof(SessionParams, level) == sf::kParamsLevelOff, "params+0x10 level");

// The id ApplyCommandLine itself compares -spectatorstream's value against at cr15game.cpp:1709.
// Used as a known-answer control on the name lookup: if this disagrees, the resolver is reading
// the wrong side of the string and every id it returns is wrong in the same silent way.
constexpr uint64_t kEchoArenaId = 0xCB60A4DE7E1CAF73ull;

struct State {
    replay::ReplayServices* services;
    const char*     strategy;
    char            reason[224];
    bool            sitesOk;
    bool            enabled;

    std::atomic<bool>     pending;
    std::atomic<bool>     cycleQueued;      // the pending request is a level cycle
    std::atomic<uint64_t> cycleEnteredMs;   // a performed cycle not yet out of the level; 0 = none
    std::atomic<uint64_t> wantLevel;
    std::atomic<uint64_t> wantGametype;

    uint32_t entryAttempts;
    uint32_t suppressions;
    int32_t  lastState;

    replay::EntryRefusal lastRefusal;
    int32_t              lastRefusedState;
    bool                 refusalLogged;

    replay::session::BootBitPolicy bootPolicy;
    bool                           bootPolicyKnown;

    // ENG-GAMETYPE-ENABLE (F2). session.gametype_override, and the gametype its fallback name
    // (session.gametype_override_name) resolves to. overrideGametypeId is 0 when the name did not
    // resolve (no image / misaligned symbol) -- the fallback rule is then inert.
    bool     gametypeOverride;
    uint64_t overrideGametypeId;
};

State g_state{};

void SetReason(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(g_state.reason, sizeof(g_state.reason), _TRUNCATE, fmt, args);
    va_end(args);
}

void Log(replay::LogLevel level, const char* fmt, ...) {
    if (g_state.services == nullptr || g_state.services->log == nullptr) {
        return;
    }
    char line[480];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
    va_end(args);
    g_state.services->log->Write(level, kReplayModuleName, "%s", line);
}

bool StateIsInGame() {
    return replay::session::StateRaw() == static_cast<int32_t>(sf::kStateInGame);
}

// The gametype the offline entry should actually use. The panel's load funnel resolves it first,
// with the capture's own events in hand, and hands over a whole gametype that passes through here
// untouched; this is the same rule without the file, for a caller that did not. Switched off, or
// for a recorded 0, the value is returned as-is.
replay::gametype::Resolved EffectiveGametype(uint64_t gametypeId) {
    if (!g_state.gametypeOverride) {
        return {gametypeId, replay::gametype::Rule::Unresolved};
    }
    return replay::gametype::Resolve(gametypeId, 0, g_state.overrideGametypeId);
}

class SessionEntry final : public replay::ISessionEntry {
public:
    bool CanChangeLevelMidSession() const override {
        // Only the stock call can. SE-A writes tokens the parser reads once, at startup.
        return g_state.sitesOk && g_state.enabled;
    }

    bool Matches(uint64_t levelId, uint64_t gametypeId) const override {
        // The gametype the entry will actually seed CR15Game with, so "am I already there?" is asked
        // against the value that will be written -- not the recorded one the override replaces.
        gametypeId = EffectiveGametype(gametypeId).id;
        uint8_t* game = GameBase();
        if (game == nullptr) {
            return false;
        }
        const uint64_t level = *reinterpret_cast<const uint64_t*>(game + sf::kGameLevelHashOff);
        const uint64_t gametype = *reinterpret_cast<const uint64_t*>(game + sf::kGameGametypeOff);
        // A zero id means "whatever is loaded": the browser passes it for a file that recorded
        // none, and a wildcard must not read as a mismatch.
        return (levelId == 0 || levelId == level) && (gametypeId == 0 || gametypeId == gametype);
    }

    bool RequestEntry(uint64_t levelId, uint64_t gametypeId) override {
        // ENG-GAMETYPE-ENABLE (F2). Replace an unrecognizable recorded gametype BEFORE anything
        // reads it: the alreadyThere check below, the wantGametype store and PerformEntry's write to
        // CR15Game all then use the same value, so the inner session enters recognized and the
        // landed-check (Matches) settles.
        const uint64_t requestedGametype = gametypeId;
        const replay::gametype::Resolved resolved = EffectiveGametype(gametypeId);
        gametypeId = resolved.id;
        if (gametypeId != requestedGametype) {
            Log(replay::LogLevel::Info,
                "gametype override: rule %s, recorded 0x%016llX -> recognized 0x%016llX",
                replay::gametype::RuleName(resolved.rule),
                static_cast<unsigned long long>(requestedGametype),
                static_cast<unsigned long long>(gametypeId));
        }
        const bool gameConstructed = GameBase() != nullptr;
        const int32_t state = replay::session::StateRaw();
        const bool alreadyThere = Matches(levelId, gametypeId) && StateIsInGame();
        const replay::EntryRefusal verdict = replay::session::EntryVerdict(
            g_state.enabled, g_state.sitesOk, gameConstructed, alreadyThere, state);
        g_state.lastRefusal = verdict;

        if (verdict == replay::EntryRefusal::NotInLevelYet) {
            SetReason("%s: NetGame is at state %d (%s) and entering a replay needs 7/8/9 (in "
                      "level). Nothing was torn down; ask again on a later tick.",
                      replay::EntryRefusalName(verdict), state,
                      replay::session::StateName(state));
            // Callers poll, so say it once per state rather than once per ask.
            if (!g_state.refusalLogged || g_state.lastRefusedState != state) {
                Log(replay::LogLevel::Info, "entry request refused: %s", g_state.reason);
                g_state.refusalLogged = true;
                g_state.lastRefusedState = state;
            }
            return false;
        }
        if (verdict == replay::EntryRefusal::CannotEnter) {
            // When the strategy itself is off the reason Init set is the true one; keep it.
            if (g_state.enabled && g_state.sitesOk) {
                SetReason("%s: CR15Game is not constructed yet; nothing to enter a session on",
                          replay::EntryRefusalName(verdict));
            }
            return false;
        }
        g_state.refusalLogged = false;
        if (alreadyThere) {
            return true;  // already where the caller wants to be
        }
        // Performed on the tick, never here: the stock call tears the live session down and
        // builds a new one, and doing that underneath a running frame is a crash, not a race.
        g_state.wantLevel.store(levelId, std::memory_order_relaxed);
        g_state.wantGametype.store(gametypeId, std::memory_order_relaxed);
        g_state.cycleQueued.store(false, std::memory_order_relaxed);
        g_state.pending.store(true, std::memory_order_release);
        return true;
    }

    replay::NetGameState State() const override {
        return static_cast<replay::NetGameState>(replay::session::StateRaw());
    }

    void* NetGame() const override {
        uint8_t* game = GameBase();
        if (game == nullptr) {
            return nullptr;
        }
        return *reinterpret_cast<void* const*>(game + sf::kGameNetGameOff);
    }

    // ★ Safe before SessionInit has run, which under a refused instance claim it never does: modreg
    // is downstream of bootstrap::Attach. A null here would fault the first caller that strcmp'd it.
    const char* Strategy() const override {
        return g_state.strategy != nullptr ? g_state.strategy : "SE-C";
    }
    const char* FailureReason() const override {
        return g_state.strategy != nullptr ? g_state.reason
                                           : "session module never initialised (bootstrap refused?)";
    }

    replay::EntryRefusal LastRefusal() const override { return g_state.lastRefusal; }

    replay::EntryRefusal RequestLevelCycle() override {
        if (!replay::livesafety::Allowed(replay::livesafety::Feature::LevelCycle)) {
            SetReason("level cycle refused: this process is not a contained offline session");
            return replay::EntryRefusal::CannotEnter;
        }
        if (!g_state.enabled || !g_state.sitesOk || GameBase() == nullptr) {
            return replay::EntryRefusal::CannotEnter;
        }
        switch (replay::session::CycleRequestVerdict(
            g_state.pending.load(std::memory_order_acquire),
            g_state.cycleEnteredMs.load(std::memory_order_acquire), GetTickCount64(),
            StateIsInGame())) {
            case replay::session::CycleRequest::InFlight:
                return replay::EntryRefusal::Accepted;
            case replay::session::CycleRequest::NotInLevel:
                return replay::EntryRefusal::NotInLevelYet;
            case replay::session::CycleRequest::Queue:
                break;
        }
        // Zero is "leave what CR15Game holds": the params are rebuilt from the level and gametype
        // the running match was entered with, so nothing but the stock call is written.
        g_state.wantLevel.store(0, std::memory_order_relaxed);
        g_state.wantGametype.store(0, std::memory_order_relaxed);
        g_state.cycleQueued.store(true, std::memory_order_relaxed);
        g_state.pending.store(true, std::memory_order_release);
        Log(replay::LogLevel::Info,
            "level cycle requested: the running level is left and entered again through "
            "SESSION.CREATE_OFFLINE_MATCH on the next tick");
        return replay::EntryRefusal::Accepted;
    }

    bool EntryPending() const override { return g_state.pending.load(std::memory_order_acquire); }
};

SessionEntry g_entry;

// Tick thread. One stock call, ledger row 0.3.
void PerformEntry() {
    uint8_t* game = GameBase();
    if (game == nullptr) {
        SetReason("CR15Game vanished between the request and the tick");
        return;
    }

    // ★ ARM BEFORE ENTERING, because entering IS what makes this a replay session.
    //
    // bootstrap classifies the session at boot from -replayviewer on the command line, and W2-G's
    // dispatch arms again when playback begins. Both are right, and neither covers a replay entered
    // through THIS function by a path that did not go through either - the browser loading a second
    // file, a config-driven entry, a future caller. The design's rule is that a wrongly-armed
    // capture is loud and a wrongly-disarmed viewer is a silent breach, so the entry point closes
    // the class itself rather than trusting every caller to have done it. Arm is idempotent and
    // reports the FIRST call's trigger, so this never overwrites a sweep override.
    const replay::egress::ArmState armed =
        replay::egress::Arm(replay::egress::ArmTrigger::ReplayMode);
    if (armed != replay::egress::ArmState::Armed) {
        Log(replay::LogLevel::Warn,
            "entering a replay session with the network blocker %s (trigger %s)",
            replay::egress::ArmStateName(armed),
            replay::egress::ArmTriggerName(replay::egress::Current().trigger));
    }
    const uint64_t wantLevel = g_state.wantLevel.load(std::memory_order_relaxed);
    const uint64_t wantGametype = g_state.wantGametype.load(std::memory_order_relaxed);

    // These have to be right in CR15Game as well as in our struct: on the team-2 path
    // SESSION.BEGIN_MULTIPLAYER discards our params and SESSION.SPECTATE_CREATE rebuilds them
    // from these two fields. Zero means "leave whatever the command line set".
    if (wantLevel != 0) {
        *reinterpret_cast<uint64_t*>(game + sf::kGameLevelHashOff) = wantLevel;
    }
    if (wantGametype != 0) {
        *reinterpret_cast<uint64_t*>(game + sf::kGameGametypeOff) = wantGametype;
    }

    SessionParams params{};
    params.three = 3;
    params.gametype = *reinterpret_cast<const uint64_t*>(game + sf::kGameGametypeOff);
    params.level = *reinterpret_cast<const uint64_t*>(game + sf::kGameLevelHashOff);
    params.minusOne = ~0ull;

    const auto enter =
        reinterpret_cast<EnterOfflineMatchFn>(const_cast<uint8_t*>(Rebase(sf::kEnterOfflineMatch)));
    ++g_state.entryAttempts;
    Log(replay::LogLevel::Info, "entering offline match: level 0x%016llX gametype 0x%016llX team %u",
        static_cast<unsigned long long>(params.level),
        static_cast<unsigned long long>(params.gametype),
        static_cast<unsigned>(*reinterpret_cast<const uint16_t*>(game + sf::kGameTeamOff)));
    enter(game, &params);

    const uint64_t flags = *SessionFlagsPtr(game);
    if ((flags & sf::kFlagBitOffline) == 0) {
        SetReason("the stock call returned with SessionFlags 0x%llX; the offline bit is not set",
                  static_cast<unsigned long long>(flags));
        Log(replay::LogLevel::Error, "%s", g_state.reason);
        return;
    }
    Log(replay::LogLevel::Info, "offline bit set; SessionFlags 0x%llX host=%d",
        static_cast<unsigned long long>(flags), sf::IsHostFlags(flags) ? 1 : 0);
}

void ReportStateChanges() {
    const int32_t now = replay::session::StateRaw();
    if (now == g_state.lastState) {
        return;
    }
    Log(replay::LogLevel::Info, "NetGame state %d (%s) -> %d (%s)", g_state.lastState,
        replay::session::StateName(g_state.lastState), now, replay::session::StateName(now));
    g_state.lastState = now;
}

void SessionTick(const replay::TickInfo&) {
    replay::session::PumpOnEngineThread();
}

struct GuardedSite {
    const char*    id;
    uint64_t       va;
    const uint8_t* prologue;
};

bool VerifySites() {
    const GuardedSite sites[] = {
        {"SESSION.CREATE_OFFLINE_MATCH", sf::kEnterOfflineMatch, sf::kEnterOfflinePrologue},
        {"SESSION.BEGIN_MULTIPLAYER", sf::kBeginMultiplay, sf::kBeginMultiplayPrologue},
        {"SESSION.SPECTATE_CREATE", sf::kSpectateCreate, sf::kSpectateCreatePrologue},
        {"GAME.BOOT_BRANCH", sf::kBootBranch, sf::kBootBranchPrologue},
    };
    for (const GuardedSite& site : sites) {
        if (!replay::buildguard::VerifySite(site.id, site.va, site.prologue, sf::kPrologueLen)) {
            SetReason("%s does not hold its recorded prologue; refusing to call into it", site.id);
            return false;
        }
    }
    return true;
}

bool SessionInit(replay::ReplayServices* services) {
    REPLAY_REQUIRE(services, log);
    REPLAY_REQUIRE(services, config);
    g_state.services = services;
    g_state.strategy = "SE-C";
    g_state.lastState = replay::session::StateRaw();
    services->session = &g_entry;
    SetReason("");

    g_state.gametypeOverride = services->config->GetBool("session.gametype_override", true);
    g_state.overrideGametypeId = 0;
    if (g_state.gametypeOverride) {
        const char* name =
            services->config->GetString("session.gametype_override_name", "echo_arena");
        g_state.overrideGametypeId = LookupNameId(name);
        Log(replay::LogLevel::Info,
            "gametype override ON (session.gametype_override): a recorded gametype nothing else "
            "resolves -> '%s' = 0x%016llX%s",
            name, static_cast<unsigned long long>(g_state.overrideGametypeId),
            g_state.overrideGametypeId == 0 ? " (name did not resolve -- override inert)" : "");
    }

    g_state.enabled = services->config->GetBool("session.enter_offline_match", true);
    if (!g_state.enabled) {
        SetReason("ledger row 0.3 (session.enter_offline_match) is switched off in config");
    } else if (!VerifySites()) {
        // reason set by VerifySites
    } else {
        const uint64_t control = LookupNameId("echo_arena");
        if (control != kEchoArenaId) {
            SetReason("name-id lookup control failed: echo_arena resolved to 0x%016llX",
                      static_cast<unsigned long long>(control));
        } else {
            g_state.sitesOk = true;
            g_state.strategy = "SE-B";
        }
    }

    if (g_state.sitesOk) {
        Log(replay::LogLevel::Info,
            "SE-B ready: stock call guarded, name-id control passed, CR15Game %s",
            GameBase() != nullptr ? "already constructed" : "not constructed yet");
    } else {
        Log(replay::LogLevel::Warn, "%s; entry falls back to SE-C (launcher-supplied switches)",
            g_state.reason);
    }
    return true;
}

void SessionShutdown() {
    g_state.pending.store(false, std::memory_order_relaxed);
    g_state.services = nullptr;
}

REPLAY_MODULE("session", 20, SessionInit, SessionTick, SessionShutdown);

}  // namespace

namespace replay {

const char* EntryRefusalName(EntryRefusal refusal) {
    switch (refusal) {
        case EntryRefusal::NotAsked:      return "not-asked";
        case EntryRefusal::Accepted:      return "accepted";
        case EntryRefusal::NotInLevelYet: return "not-in-level-yet";
        case EntryRefusal::CannotEnter:   return "cannot-enter";
    }
    return "unknown";
}

namespace session {

bool NeedsGametypeOverride(uint64_t gametypeId) {
    return gametypeId == ~0ull || (gametypeId != 0 && (gametypeId >> 32) == 0);
}

EntryRefusal EntryVerdict(bool enabled, bool sitesOk, bool gameConstructed, bool alreadyThere,
                          int32_t netGameState) {
    if (!enabled || !sitesOk || !gameConstructed) {
        return EntryRefusal::CannotEnter;
    }
    if (netGameState != sf::kStateLoadingLevel && netGameState != sf::kStateReadyForGame &&
        netGameState != static_cast<int32_t>(sf::kStateInGame)) {
        return EntryRefusal::NotInLevelYet;
    }
    (void)alreadyThere;  // the caller's short-circuit; it cannot make a non-level state enterable
    return EntryRefusal::Accepted;
}

CycleRequest CycleRequestVerdict(bool pending, uint64_t enteredAtMs, uint64_t nowMs, bool inGame) {
    if (pending) return CycleRequest::InFlight;
    if (enteredAtMs != 0 && nowMs - enteredAtMs < kCycleLeaveGraceMs) return CycleRequest::InFlight;
    return inGame ? CycleRequest::Queue : CycleRequest::NotInLevel;
}

EntryRefusal LastEntryRefusal() { return g_state.lastRefusal; }

bool EntryRequestPending() { return g_state.pending.load(std::memory_order_acquire); }

uint32_t EntryAttemptCount() { return g_state.entryAttempts; }

void PumpOnEngineThread() {
    if (!g_state.enabled) {
        return;
    }
    // Unconditional on purpose: the policy lives INSIDE the callee, so there is exactly one place
    // that decides and no call site can forget it.
    SuppressBootToMultiplayer();
    ReportStateChanges();
    const bool inGame = StateIsInGame();
    if (!inGame) g_state.cycleEnteredMs.store(0, std::memory_order_release);
    if (!g_state.pending.load(std::memory_order_acquire)) {
        return;
    }
    const bool cycle = g_state.cycleQueued.exchange(false, std::memory_order_relaxed);
    // A cycle leaves the level it was asked from. Once NetGame is out of it, a transition is
    // already under way and running the stock call again would only restart it.
    if (cycle && !inGame) {
        g_state.pending.store(false, std::memory_order_release);
        Log(replay::LogLevel::Warn,
            "level cycle dropped: NetGame left the level before the entry ran (state %d)",
            replay::session::StateRaw());
        return;
    }
    // Latched before `pending` clears, so a request in between folds into this entry.
    if (cycle) g_state.cycleEnteredMs.store(GetTickCount64(), std::memory_order_release);
    if (g_state.pending.exchange(false, std::memory_order_acq_rel)) {
        PerformEntry();
    }
}

bool ArmOfflineBeforeLogin(bool forceNoNetwork) {
    uint8_t* game = GameBase();
    if (game == nullptr) {
        return false;
    }
    uint64_t* flags = SessionFlagsPtr(game);
    const uint64_t before = *flags;
    *flags |= sf::kFlagBitOffline;
    if (forceNoNetwork) {
        *flags |= sf::kFlagBitNoNetwork;
    }
    if (*flags != before) {
        Log(replay::LogLevel::Info, "armed offline before login: SessionFlags 0x%llX -> 0x%llX",
            static_cast<unsigned long long>(before), static_cast<unsigned long long>(*flags));
    }
    return true;
}

const char* BootBitPolicyName(BootBitPolicy policy) {
    switch (policy) {
        case BootBitPolicy::LeaveIntactOfflineHost:   return "left-intact-offline-local-host";
        case BootBitPolicy::SuppressForReplaySession: return "cleared-replay-session";
        case BootBitPolicy::LeaveIntactNoReplay:      return "left-intact-no-replay-session";
    }
    return "unknown";
}

BootBitPolicy ClassifyBootBitPolicy(replay::bootstrap::SessionMode mode, bool offlineHost) {
    if (offlineHost) {
        return BootBitPolicy::LeaveIntactOfflineHost;
    }
    return mode == replay::bootstrap::SessionMode::Replay
               ? BootBitPolicy::SuppressForReplaySession
               : BootBitPolicy::LeaveIntactNoReplay;
}

BootBitPolicy BootBitPolicyForThisLaunch() {
    if (g_state.bootPolicyKnown) {
        return g_state.bootPolicy;
    }
    // Our own -offline (offlinepatch.h) is settled inside CLI.APPLY, and the arm this bit selects runs
    // after it. Until then the bit is left alone and nothing is latched, so the decision below sees the
    // patch's real outcome rather than a guess.
    const replay::offlinepatch::Status patch = replay::offlinepatch::Current();
    if (patch == replay::offlinepatch::Status::Pending) {
        return BootBitPolicy::LeaveIntactOfflineHost;
    }
    // GetCommandLineW, not the A variant this module augments: rlay reads W and the engine reads A,
    // and the switches that decide this - -offline, -region - are on the original either way.
    const wchar_t* line = GetCommandLineW();
    const replay::bootstrap::SessionMode mode =
        replay::bootstrap::ClassifySession(line, replay::bootstrap::OpenRequestPresent(),
                                           replay::coexist::IsModuleLoaded("rlay.dll"));
    const bool offlineSwitchSeen = replay::bootstrap::HasSwitch(line, L"-offline");
    const bool offlinePatched = replay::offlinepatch::OfflineHostExpected(patch);
    g_state.bootPolicy = ClassifyBootBitPolicy(mode, offlineSwitchSeen || offlinePatched);
    g_state.bootPolicyKnown = true;

    switch (g_state.bootPolicy) {
        case BootBitPolicy::LeaveIntactOfflineHost:
            Log(replay::LogLevel::Info,
                "boot bit left intact: offline local host (-offline on the engine line: %s; offline "
                "patch: %s; session mode '%s'). CR15Game+0x7AE0 bit 0x2000000 selects the arm the "
                "-offline patch redirects to sub_1400FE0E0, which is what loads this level",
                offlineSwitchSeen ? "yes" : "no", replay::offlinepatch::StatusName(patch),
                replay::bootstrap::SessionModeName(mode));
            break;
        case BootBitPolicy::SuppressForReplaySession:
            Log(replay::LogLevel::Info,
                "boot bit cleared: replay session (%ls names a replay to open and no -offline host "
                "needs the bit; offline patch: %s)",
                replay::bootstrap::OpenEnvVarName(), replay::offlinepatch::StatusName(patch));
            break;
        case BootBitPolicy::LeaveIntactNoReplay:
            Log(replay::LogLevel::Info,
                "boot bit left intact: no replay session requested (session mode '%s')",
                replay::bootstrap::SessionModeName(mode));
            break;
    }
    return g_state.bootPolicy;
}

uint32_t BootSuppressionCount() { return g_state.suppressions; }

BootSuppression SuppressBootToMultiplayer() {
    return SuppressBootToMultiplayer(BootBitPolicyForThisLaunch());
}

BootSuppression SuppressBootToMultiplayer(BootBitPolicy policy) {
    // ★ FIRST, and before any engine read. On every policy but one the bit is the launch's own
    // boot path and touching it is the L5 defect.
    if (policy != BootBitPolicy::SuppressForReplaySession) {
        return BootSuppression::RefusedByPolicy;
    }
    // The policy above already refuses every live-online launch -- ClassifySession returns one
    // mode, the exemption needs LiveOnlineMatch and this arm needs Replay -- but that is the
    // command line answering the question twice in two schemes. Ask the process predicate itself,
    // so the clear is governed by the same fact as every other engine write here.
    if (!livesafety::Allowed(livesafety::Feature::BootModeBits)) {
        return BootSuppression::RefusedUncontained;
    }
    uint8_t* game = GameBase();
    if (game == nullptr) {
        return BootSuppression::NoGame;
    }
    uint64_t* mode = ModeFlagsPtr(game);
    if ((*mode & sf::kModeBitBootMp) == 0) {
        // Nothing set it: -level was given and none of the seven setters was. An earlier comment
        // here read "the one -level produces: nothing ever set the bit", which is FALSE - -level
        // beside -region/-mp/-mpappid/-lobbyid/-moderator*/-spectatorstream leaves the bit set by
        // correct engine behaviour. tests/session/boot_flag_expectation.h carries the census.
        return BootSuppression::AlreadyClear;
    }

    // ★ The bit is set, so we are about to change engine state we cannot prove is unread. Say what
    // the witnesses actually support and nothing more. There is no flag meaning "the branch has not
    // run yet"; GamespacesCreated() only ever becomes true on the arm this bit prevents.
    const bool tooLate = MultiplayerBootSeen();
    *mode &= ~sf::kModeBitBootMp;
    ++g_state.suppressions;
    if (tooLate) {
        SetReason("boot-to-multiplayer was set and CR15Game+0x7B14 == 1: sub_140100B90 already ran");
        Log(replay::LogLevel::Error, "%s; cleared the bit anyway, but too late to matter",
            g_state.reason);
        return BootSuppression::ClearedTooLate;
    }
    Log(replay::LogLevel::Warn,
        "cleared CR15Game+0x7AE0 bit 0x2000000 (boot into multiplayer); ordering UNVERIFIED - no "
        "witness proves GAME.BOOT_BRANCH had not already run. Only a launch with no -offline and a "
        "replay open request reaches this line");
    return BootSuppression::ClearedUnverified;
}

// Nothing is augmented any more. Ruling Q-D is retired on its own premise: it reasoned that the
// switch "supplies the mode, not the value" for an external step written to CClock+0x90, but
// clock.cpp:12-16 retired that path (TICK.UPDATE_CLOCK rewrites +0x90 every frame at 0x140127A04)
// and Apply() writes only +0x38, +0x98 and FLAGS bits 1-2, none of which GAME.FIXEDTIMESTEP_BIT
// touches. What the switch DID cost is CLOCK.LIMITER_BIT: CClock::HeartbeatBefore sleeps to
// 33.3 ms (sub_1400CF3B0 @0x1400CF416), pinning every launch -- capture included -- to 30 fps, and
// capping fast-forward at 66666/33333 = 2.0x. The hook stays registered as a pass-through.
const char* const* AugmentTokens(size_t* count) {
    *count = 0;
    return nullptr;
}

extern "C" LPSTR WINAPI ReplayGetCommandLineA() {
    // Built once and never freed: S8.4.2 requires a pointer stable for the process lifetime, and
    // the engine keeps parsing from it after we return. 8 KB is four times the Windows command-line
    // limit, so the augmentation cannot be the thing that overflows it.
    static char augmented[8192];  // facts-ok: PHYS.CI_PAUSED_BIT a command-line buffer size, not the pause bit
    static LONG built = 0;
    if (InterlockedCompareExchange(&built, 1, 0) == 0) {
        size_t count = 0;
        const char* const* tokens = AugmentTokens(&count);
        const char* original = g_realGetCommandLineA();
        if (replay::session::AugmentCommandLine(original, tokens, count, augmented,
                                                sizeof(augmented)) == 0) {
            // Did not fit, or nothing to do. Either way the original is the correct answer, and
            // returning a truncated command line would be far worse than not augmenting.
            InterlockedExchange(&built, 2);
            return const_cast<LPSTR>(original);
        }
        InterlockedExchange(&built, 2);
    }
    while (InterlockedCompareExchange(&built, 2, 2) != 2) {
        Sleep(0);
    }
    return augmented;
}

bool InstallCommandLineHook() {
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32 == nullptr) {
        return false;
    }
    auto* real = reinterpret_cast<GetCommandLineAFn>(GetProcAddress(kernel32, "GetCommandLineA"));
    if (real == nullptr) {
        return false;
    }

    auto* slot = reinterpret_cast<GetCommandLineAFn*>(
        const_cast<uint8_t*>(Rebase(sf::kCmdlineIatGetCommandLineA)));
    if (slot == nullptr || !replay::Readable(slot, sizeof(*slot))) {
        return false;
    }
    // S4.5.3 rule 4. A slot that does not hold kernel32's function is somebody else's redirect,
    // and overwriting it would silently unhook them.
    if (*slot != real) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    g_realGetCommandLineA = real;
    *slot = &ReplayGetCommandLineA;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtect, &ignored);
    return true;
}

replay::ISessionEntry* Interface() {
    return &g_entry;
}

// Ledger row 1X.1. bootstrap::Attach runs this under the loader lock, gated on its config key,
// before the worker starts - which is the only place early enough.
REPLAY_ATTACH_HOOK("session-cmdline", "session.cmdline_hook", InstallCommandLineHook);

}  // namespace session
}  // namespace replay
