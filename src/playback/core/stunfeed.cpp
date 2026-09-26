#include "playback/core/stunfeed.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "engine/pawn_provision.h"
#include "foundation/demofile/demotypes.h"
#include "generated/facts_generated.h"
#include "playback/core/recorded_loadout.h"
#include "playback/core/stunfeed_facts.h"

namespace replay {
namespace playback {
namespace stunfeed {

namespace sf = replay::stunfeed_facts;

namespace {

const char kTapName[] = "SR15StunPlayerMsg";

uint32_t U32(const uint8_t* p, uint32_t off) {
    uint32_t v = 0;
    memcpy(&v, p + off, sizeof v);
    return v;
}

float F32(const uint8_t* p, uint32_t off) {
    float v = 0.0f;
    memcpy(&v, p + off, sizeof v);
    return v;
}

uint16_t SlotOf(uint32_t netUserId) {
    return uint16_t(netUserId & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------------------------

void RecordedStuns::Reset() {
    count_ = 0;
    chapter_first_ = 0;
    victim_count_ = 0;
}

bool RecordedStuns::Offer(uint64_t arrivalUs, const void* body, uint32_t len, ScanCensus& census) {
    if (len < kBodyBytes || body == nullptr) {
        ++census.refused_rows;
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(body);
    const float duration = F32(p, sf::kOffDuration);
    // A duration the engine's own timer could not be running is not something this recording means,
    // and it would be written straight into the field that ends the stun.  Counted, named, refused.
    if (!(duration > sf::kMinDuration) || !(duration <= sf::kMaxDuration)) {
        ++census.refused_rows;
        return false;
    }
    if (count_ >= kMaxRows) {
        ++census.dropped_rows;
        if (!census.first_drop) census.first_drop = "more stun rows than the store holds";
        return false;
    }
    Stun& s = rows_[count_++];
    s.t_us = arrivalUs;
    s.victim = U32(p, sf::kOffVictim);
    s.puncher = U32(p, sf::kOffPuncher);
    s.duration = duration;
    s.blocked = U32(p, sf::kOffFlag) != 0;
    memcpy(s.body, p, kBodyBytes);

    const uint16_t slot = SlotOf(s.victim);
    bool seen = false;
    for (uint32_t i = 0; i < victim_count_; ++i) seen = seen || victims_[i] == slot;
    if (!seen && victim_count_ < kMaxVictims) victims_[victim_count_++] = slot;

    ++census.rows;
    return true;
}

void RecordedStuns::BeginChapter() { chapter_first_ = count_; }

void RecordedStuns::EndChapter(uint64_t tBeginUs, uint64_t tEndUs, uint64_t firstArrivalUs,
                               uint64_t lastArrivalUs, ScanCensus& census) {
    if (chapter_first_ >= count_) return;
    const uint64_t span = tEndUs > tBeginUs ? tEndUs - tBeginUs : 0;
    const uint64_t arrivalSpan = lastArrivalUs > firstArrivalUs ? lastArrivalUs - firstArrivalUs : 0;
    for (uint32_t i = chapter_first_; i < count_; ++i) {
        uint64_t t = tBeginUs;
        if (span == 0) {
            // A chapter with no end after its begin cannot place anything; the row keeps the only
            // time the file gives it and the count says so.
            rows_[i].t_us = tBeginUs;
            continue;
        }
        if (arrivalSpan == 0) {
            t = tBeginUs + span / 2;
        } else {
            const uint64_t a = rows_[i].t_us;
            const uint64_t d = a > firstArrivalUs ? a - firstArrivalUs : 0;
            t = tBeginUs + (d >= arrivalSpan ? span - 1 : (span * d) / arrivalSpan);
        }
        if (t >= tEndUs) t = tEndUs - 1;
        if (t != tBeginUs) ++census.placed_rows;
        rows_[i].t_us = t;
    }
    chapter_first_ = count_;
}

uint32_t RecordedStuns::FirstAfter(int64_t tUs) const {
    if (count_ == 0) return 0;
    if (tUs < 0) return 0;
    const uint64_t t = uint64_t(tUs);
    uint32_t lo = 0, hi = count_;  // first index with t_us > t
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (rows_[mid].t_us <= t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int32_t RecordedStuns::LastForVictimAtOrBefore(uint16_t recSlot, int64_t tUs) const {
    if (tUs < 0) return -1;
    const uint32_t end = FirstAfter(tUs);
    for (uint32_t i = end; i > 0; --i) {
        if (SlotOf(rows_[i - 1].victim) == recSlot) return int32_t(i - 1);
    }
    return -1;
}

uint32_t ScanRecordedStuns(IDemoReader& reader, RecordedStuns& into, ScanCensus& census) {
    into.Reset();
    census = ScanCensus{};
    static uint8_t body[capture::kMaxEventBodyBytes];
    static uint8_t keyblob[identity::kTapTableScratchBytes];
    uint32_t stored = 0;
    uint16_t tap = capture::kTapTypeIndexNone;

    for (size_t i = 0; i < reader.ChapterCount(); ++i) {
        DecodedChapter dc;
        if (!reader.DecodeChapter(i, dc)) continue;
        const uint16_t here =
            identity::TapIndexForName(dc, kTapName, sizeof kTapName - 1, keyblob, sizeof keyblob);
        if (here != capture::kTapTypeIndexNone) {
            ++census.tap_chapters;
            tap = here;
        }
        if (tap == capture::kTapTypeIndexNone) continue;

        const StreamView* sv = dc.Find(kStreamEvent);
        if (!sv || !sv->records || sv->bytes <= capture::kBlockPrefixBytes) continue;
        const ChapterKey key = reader.ChapterAt(i);
        into.BeginChapter();
        bitio::BitReader r(sv->records + capture::kBlockPrefixBytes,
                           sv->bytes - capture::kBlockPrefixBytes);
        capture::EventDecoder dec;
        bool haveArrival = false;
        uint64_t firstArrival = 0, lastArrival = 0;
        for (uint32_t k = 0; k < sv->recordCount; ++k) {
            capture::EventMessage m{};
            // A refused row leaves the reader off a record boundary, so the rest of this chapter's
            // events are unreadable: stop rather than decode garbage, as the goal scan does.
            if (!dec.Decode(r, m, body, sizeof body)) break;
            ++census.event_rows;
            // EVERY event row widens the arrival window, not just the stun rows: the window is what
            // locates a stun inside its chapter, and one built from eight rows would be four points.
            if (!haveArrival) {
                firstArrival = lastArrival = m.stampUs;
                haveArrival = true;
            } else if (m.stampUs > lastArrival) {
                lastArrival = m.stampUs;
            }
            if (m.typeIndex != tap) continue;
            if (into.Offer(m.stampUs, m.body, m.bodyBytes, census)) ++stored;
        }
        into.EndChapter(key.tBeginUs, key.tEndUs, firstArrival, lastArrival, census);
    }
    census.tap = tap;
    return stored;
}

RecordedStuns& PlaybackRecordedStuns() {
    static RecordedStuns s;
    return s;
}

ScanCensus& PlaybackStunCensus() {
    static ScanCensus c;
    return c;
}

// ---------------------------------------------------------------------------------------------
// The feed
// ---------------------------------------------------------------------------------------------

namespace {

struct Feed {
    bool armed = false;
    bool have_cursor = false;  // false until the first sweep, and after every seek
    bool seek_pending = false;
    int64_t cursor_us = 0;
    int64_t origin_us = 0;
    uint32_t next = 0;  // first row strictly after the cursor; the steady state's one comparison
    uint32_t logged = 0;
    uint32_t level_logged = 0;
    ApplyCensus census;
    const Handlers* injected = nullptr;
    Handlers bound;
    bool bind_tried = false;
    // The punchable component system, memoised on the pair it was found through: CS.LOOKUP_BY_SYMBOL
    // SORTS the game space's system table in place, so asking it once per tick would be an engine
    // table sort per tick on our thread.
    void* cs_space = nullptr;
    void* cs = nullptr;
};

Feed g;

// Eight stuns is a whole match; a file that somehow produces thousands must not turn the log into
// the payload.  The census counts every fire whether or not it was printed.
inline constexpr uint32_t kLogCap = 64;
inline constexpr uint32_t kLevelLogCap = 32;

template <class Fn>
Fn Bind(const char* factId, uint64_t va, const uint8_t* pro, size_t n) {
    if (!buildguard::VerifySite(factId, va, pro, n)) return nullptr;
    return reinterpret_cast<Fn>(const_cast<void*>(buildguard::MappedAddress(va)));
}

// The two delegate wrappers share their first 76 bytes, so the prologue alone cannot tell them
// apart; the second window is what does, and a wrapper that fails either one does not bind.
DelegateFn BindDelegate(const char* factId, const char* windowFactId, uint64_t va,
                        const uint8_t* pro, size_t proN, const uint8_t* win, size_t winN) {
    DelegateFn fn = Bind<DelegateFn>(factId, va, pro, proN);
    if (fn == nullptr) return nullptr;
    if (!buildguard::VerifySite(windowFactId, va + sf::kWindowOff, win, winN)) return nullptr;
    return fn;
}

// SEH lives in POD-only functions: MSVC C2712 forbids __try beside the EngineCallLatch.
bool CallDelegateGuarded(DelegateFn fn, void* cs, uint32_t componentId) {
    __try {
        fn(cs, componentId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallRaiseGuarded(RaiseEventFn fn, void* space, uint64_t symbol, uint64_t lo, uint64_t hi) {
    __try {
        fn(space, symbol, lo, hi, 1u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallFindGuarded(FindComponentFn fn, void* cs, void* out, uint64_t lo, uint64_t hi) {
    __try {
        fn(cs, out, lo, hi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallLookupGuarded(CsLookupFn fn, void* space, uint64_t symbol, void*& out) {
    __try {
        out = fn(space, symbol);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LoadPtr(const void* at, void*& out) {
    if (!replay::Readable(at, sizeof(void*))) return false;
    __try {
        out = *static_cast<void* const*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LoadU32(const void* at, uint32_t& out) {
    if (!replay::Readable(at, sizeof(uint32_t))) return false;
    __try {
        memcpy(&out, at, sizeof out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LoadU16(const void* at, uint16_t& out) {
    if (!replay::Readable(at, sizeof(uint16_t))) return false;
    __try {
        memcpy(&out, at, sizeof out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool StoreU32(void* at, uint32_t v) {
    __try {
        memcpy(at, &v, sizeof v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LoadU64(const void* at, uint64_t& out) {
    if (!replay::Readable(at, sizeof(uint64_t))) return false;
    __try {
        memcpy(&out, at, sizeof out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const uint8_t* At(const void* base, uint32_t off) {
    return static_cast<const uint8_t*>(base) + off;
}

// The punchable component's CSymbol64, out of the image's own radhash table: the ASCII anchor is
// verified first and the qword immediately before it is the value, which is exactly the
// `lea str-1 / and ~7 / mov` every engine site spells.
uint64_t ReadSymbol(const char* factId, uint64_t anchorVa, const uint8_t* anchor, size_t n) {
    if (!buildguard::VerifySite(factId, anchorVa, anchor, n)) return 0;
    const void* at = buildguard::MappedAddress(anchorVa - 8);
    uint64_t sym = 0;
    if (!at || !LoadU64(at, sym)) return 0;
    return sym;
}

uint64_t ReadPunchableSymbol() {
    return ReadSymbol("STUN.PUNCHABLE_SYM_ANCHOR_VA", sf::kPunchableSymbolAnchorVa,
                      sf::kPunchableSymbolAnchor, sizeof sf::kPunchableSymbolAnchor);
}

const Handlers* Resolve() {
    if (g.injected != nullptr) return g.injected;
    if (!g.bind_tried) {
        g.bind_tried = true;
        g.bound.on_hit =
            BindDelegate("STUN.ONHIT_VA", "STUN.ONHIT_VA.WINDOW48", sf::kOnHitVa,
                         sf::kOnHitPrologue, sizeof sf::kOnHitPrologue, sf::kOnHitWindow,
                         sizeof sf::kOnHitWindow);
        g.bound.on_stun =
            BindDelegate("STUN.ONSTUN_VA", "STUN.ONSTUN_VA.WINDOW48", sf::kOnStunVa,
                         sf::kOnStunPrologue, sizeof sf::kOnStunPrologue, sf::kOnStunWindow,
                         sizeof sf::kOnStunWindow);
        g.bound.lookup = Bind<CsLookupFn>("CS.LOOKUP_BY_SYMBOL", facts::CS_LOOKUP_BY_SYMBOL,
                                          facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
                                          sizeof facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE);
        g.bound.find = Bind<FindComponentFn>(
            "LOCAL.FIND_ACTOR_COMPONENT", facts::LOCAL_FIND_ACTOR_COMPONENT,
            facts::LOCAL_FIND_ACTOR_COMPONENT_PROLOGUE,
            sizeof facts::LOCAL_FIND_ACTOR_COMPONENT_PROLOGUE);
        g.bound.symbol = ReadPunchableSymbol();
        g.bound.raise = Bind<RaiseEventFn>("STUN.RAISE_EVENT_VA", sf::kRaiseEventVa,
                                           sf::kRaiseEventPrologue, sizeof sf::kRaiseEventPrologue);
        g.bound.on_hit_symbol = ReadSymbol("STUN.ONHIT_SYM_ANCHOR_VA", sf::kOnHitSymbolAnchorVa,
                                           sf::kOnHitSymbolAnchor, sizeof sf::kOnHitSymbolAnchor);
        g.bound.on_stun_symbol = ReadSymbol("STUN.ONSTUN_SYM_ANCHOR_VA", sf::kOnStunSymbolAnchorVa,
                                            sf::kOnStunSymbolAnchor, sizeof sf::kOnStunSymbolAnchor);
        if (g.bound.on_hit_symbol == 0 || g.bound.on_stun_symbol == 0) g.bound.raise = nullptr;
        log::Direct(LogLevel::Info, "playback",
                    "stuns: bind: OnHit %s, OnStun %s, cs-lookup %s, find-component %s, "
                    "R14NetPunchable symbol 0x%016llX, actor-raise fallback %s "
                    "(OnHit 0x%016llX, OnStun 0x%016llX)",
                    g.bound.on_hit ? "ok" : "REFUSED", g.bound.on_stun ? "ok" : "REFUSED",
                    g.bound.lookup ? "ok" : "REFUSED", g.bound.find ? "ok" : "REFUSED",
                    (unsigned long long)g.bound.symbol, g.bound.raise ? "ok" : "REFUSED",
                    (unsigned long long)g.bound.on_hit_symbol,
                    (unsigned long long)g.bound.on_stun_symbol);
    }
    return &g.bound;
}

// The component system, found once per game space.  The vtable slot is PROVED to hold the entry we
// bound before anything is called through it -- this is a fourth object and the slot check is per
// object, never per process.
void* PunchableCs(const Handlers* h, void* netGame) {
    void* space = nullptr;
    if (!LoadPtr(At(netGame, facts::NETGAME_CS_SPACE_OFF), space) || space == nullptr) return nullptr;
    if (g.cs != nullptr && g.cs_space == space) return g.cs;
    g.cs_space = space;
    g.cs = nullptr;
    if (h->lookup == nullptr || h->find == nullptr || h->symbol == 0) return nullptr;
    void* cs = nullptr;
    if (!CallLookupGuarded(h->lookup, space, h->symbol, cs) || cs == nullptr) return nullptr;
    void* vtbl = nullptr;
    if (!LoadPtr(cs, vtbl) || vtbl == nullptr) return nullptr;
    void* slot = nullptr;
    if (!LoadPtr(At(vtbl, facts::LOCAL_CS_VTSLOT_FIND_COMPONENT), slot)) return nullptr;
    if (slot != reinterpret_cast<void*>(h->find)) return nullptr;
    g.cs = cs;
    return cs;
}

// actor -> SComponentID, through the engine's own dispatcher.  The engine's own wrapper hands this
// a 40-byte scratch even though only the first dword comes back; matching that costs nothing and
// does not depend on a belief about how much the callee touches.
// ★ THE TWO ARGUMENTS ARE (ACTOR ID, GAME SPACE), NOT THE TWO HALVES OF THE GUID. The searcher
// behind the vtable slot substitutes its own space when the second half is -1 and then tiebreaks
// every candidate row against it, so a stored GUID fed in as a pair misses on every actor. Which
// half is the actor id was measured by PRESENT-G's census (see Entity::ComponentOnActor); the other
// half is tried only after a miss, and a miss is the engine saying "not this actor".
bool ComponentOnActor(const Handlers* h, void* cs, const LiveUser& u, uint32_t& componentId) {
    if (u.actor_lo == 0 && u.actor_hi == 0) return false;  // a zero GUID is not an actor
    const uint64_t halves[2] = {u.actor_hi, u.actor_lo};
    for (uint64_t actorId : halves) {
        uint8_t scratch[40] = {};
        if (!CallFindGuarded(h->find, cs, scratch, actorId, sf::kThisGameSpace)) return false;
        uint32_t id = 0;
        memcpy(&id, scratch, sizeof id);
        if (uint16_t(id & 0xFFFFu) != sf::kNoComponent) {  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
            componentId = id;
            return true;
        }
    }
    return false;
}

// SComponentID -> the 72-byte punchable record, off the unshifted pool group: the handle map's
// 4-byte entry gives the element index and the element base is strided by 72.
uint8_t* RecordFor(void* cs, uint32_t componentId) {
    void* map = nullptr;
    void* base = nullptr;
    if (!LoadPtr(At(cs, sf::kCsHandleMapOff), map) || map == nullptr) return nullptr;
    if (!LoadPtr(At(cs, sf::kCsPoolBaseOff), base) || base == nullptr) return nullptr;
    const uint16_t handle = uint16_t(componentId & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    uint16_t element = 0;
    if (!LoadU16(At(map, 4u * handle), element)) return nullptr;
    uint8_t* rec = const_cast<uint8_t*>(At(base, sf::kRecordStride * uint32_t(element)));
    if (!replay::Readable(rec, sf::kRecordStride)) return nullptr;
    return rec;
}

// Diagnostic only, and never on a per-frame path.  sub_140D53DA0 resolves an actor to a punchable
// by binary-searching the array at cs+0x110 whose element count is the qword at cs+0x118, so a
// count of 0 says outright that no pawn in this session carries the component, while a healthy
// count alongside a refusal says the component exists on an actor the feed is not naming.
uint32_t PunchableCount(void* cs) {
    void* p = nullptr;
    if (!LoadPtr(At(cs, 0x118u), p)) return 0xFFFFFFFFu;  // facts-ok: GSPACE.ALIAS_COUNT_OFF cs+0x118 is CR15NetPunchableCS's own element count (sub_140D53DA0) and 0xFFFFFFFFu is this function's "could not read" answer; neither is a registry position
    return uint32_t(reinterpret_cast<uintptr_t>(p));
}

// OnHit then OnStun on the victim's own actor GUID, in the engine's own order (sub_140D53DA0's).
// The space comes off the component system the same way sub_140CCE580 takes it off its own CS.
bool RaiseOnActor(const Handlers* h, void* cs, const LiveUser& victim) {
    if (h->raise == nullptr) return false;
    if (victim.actor_lo == 0 && victim.actor_hi == 0) return false;
    void* space = nullptr;
    if (!LoadPtr(At(cs, sf::kCsSpaceOff), space) || space == nullptr) return false;
    if (!CallRaiseGuarded(h->raise, space, h->on_hit_symbol, victim.actor_lo, victim.actor_hi))
        return false;
    return CallRaiseGuarded(h->raise, space, h->on_stun_symbol, victim.actor_lo, victim.actor_hi);
}

void TurnOffAfterFault(const char* what) {
    ++g.census.faulted;
    g.armed = false;
    g.census.refusal = "the engine's stun delegate raised";
    log::Direct(LogLevel::Error, "playback",
                "stuns: the engine's %s raised; the stun feed is off for the rest of this session",
                what);
}

}  // namespace

void InjectHandlersForTest(const Handlers* h) {
    g.injected = h;
    g.bind_tried = false;
    g.bound = Handlers{};
    g.cs = nullptr;
    g.cs_space = nullptr;
}

void Arm(bool on) {
    g.armed = on;
    g.logged = 0;
    g.level_logged = 0;
    g.census.refusal = nullptr;
    g.cs = nullptr;
    g.cs_space = nullptr;
    Forget();
}

bool Armed() { return g.armed; }

void SetPlayheadOriginUs(int64_t originUs) { g.origin_us = originUs; }

void NoteSeek() { g.seek_pending = true; }

void Forget() {
    g.have_cursor = false;
    g.seek_pending = false;
    g.cursor_us = 0;
    g.next = 0;
}

const ApplyCensus& Census() { return g.census; }

void ResetCensusForTest() {
    g.census = ApplyCensus{};
    g.logged = 0;
    g.level_logged = 0;
}

namespace {

// The three stores StunPlayerCB makes, in its order and with its masks.  `pulse` is the one-frame
// bit0 the engine's own Update clears every frame: an EDGE sets it because the handler does, a
// level restore does not because re-asserting an edge at a seek destination buys nothing.
bool WriteStunned(uint8_t* rec, float duration, float elapsed, bool pulse) {
    uint32_t flags = 0;
    if (!LoadU32(rec + sf::kFlagsOff, flags)) return false;
    uint32_t want = (flags & ~sf::kFlagInvulnerable) | sf::kFlagStunned;
    if (pulse) want |= sf::kFlagStunPulse;
    uint32_t elapsedBits = 0, lengthBits = 0;
    memcpy(&elapsedBits, &elapsed, sizeof elapsedBits);
    memcpy(&lengthBits, &duration, sizeof lengthBits);
    return StoreU32(rec + sf::kElapsedOff, elapsedBits) &&
           StoreU32(rec + sf::kFlagsOff, want) && StoreU32(rec + sf::kLengthOff, lengthBits);
}

// Outside every recorded span.  bit2 is left exactly as the engine left it: how long the
// post-stun invulnerability lasts is a punch-settings field this recording does not carry, so
// asserting or clearing it would be a value the plugin invented rather than replayed.
bool ClearStunned(uint8_t* rec, bool& changed) {
    changed = false;
    uint32_t flags = 0;
    if (!LoadU32(rec + sf::kFlagsOff, flags)) return false;
    const uint32_t want = flags & ~(sf::kFlagStunned | sf::kFlagStunPulse);
    if (want == flags) return true;
    if (!StoreU32(rec + sf::kFlagsOff, want)) return false;
    changed = true;
    return true;
}

// The LEVEL: what every recorded victim's punchable record should hold at this playhead.  A pure
// function of `tUs`, which is what makes reverse play and seeking free -- and it never raises.
void RestoreLevel(const Handlers* h, void* cs, int64_t tUs, MapUserFn map, void* mapCtx) {
    const RecordedStuns& src = PlaybackRecordedStuns();
    uint32_t wrote = 0, cleared = 0;
    for (uint32_t i = 0; i < src.VictimCount(); ++i) {
        const uint16_t slot = src.VictimAt(i);
        LiveUser live;
        if (map == nullptr || !map(mapCtx, slot, live)) continue;
        uint32_t componentId = 0;
        if (!ComponentOnActor(h, cs, live, componentId)) continue;
        uint8_t* rec = RecordFor(cs, componentId);
        if (rec == nullptr) continue;

        const int32_t at = src.LastForVictimAtOrBefore(slot, tUs);
        const Stun* row = at >= 0 ? &src.At(uint32_t(at)) : nullptr;
        const double elapsed = row ? double(tUs - int64_t(row->t_us)) / 1e6 : 0.0;
        if (row != nullptr && elapsed < double(row->duration)) {
            if (WriteStunned(rec, row->duration, float(elapsed), /*pulse=*/false)) ++wrote;
        } else {
            bool changed = false;
            if (ClearStunned(rec, changed) && changed) {
                ++wrote;
                ++cleared;
            }
        }
    }
    g.census.level_writes += wrote;
    g.census.level_clears += cleared;
    if (wrote != 0 && g.level_logged < kLevelLogCap) {
        ++g.level_logged;
        log::Direct(LogLevel::Info, "playback",
                    "stuns: level restore at playhead %lld us: %u punchable record(s) written, %u "
                    "of them cleared; no delegate raised",
                    (long long)(tUs - g.origin_us), wrote, cleared);
    }
}

}  // namespace

void Sweep(void* netGame, int64_t tUs, bool hold, MapUserFn map, void* mapCtx) {
    if (!g.armed) {
        g.census.refusal = "not armed";
        return;
    }
    const RecordedStuns& src = PlaybackRecordedStuns();

    // WHAT MAKES A CROSSING A CROSSING.  The cursor is the playhead the last sweep saw.  A jump the
    // user caused, the first sweep of a load and every tick the pre-roll owns the clock all re-base
    // it instead of sweeping the span -- so a forward seek across a stun, a load that opens past one
    // and the pre-roll's fast wind are all silent, and a pause leaves the cursor where it is so
    // resuming on top of a row cannot fire it twice.
    const bool rebase = !g.have_cursor || g.seek_pending || hold;
    const bool backward = !rebase && tUs < g.cursor_us;
    if (!rebase && !backward &&
        (g.next >= src.Count() || tUs < int64_t(src.At(g.next).t_us))) {
        // The steady state, and it is one comparison: nothing to cross and no discontinuity.
        g.cursor_us = tUs;
        return;
    }
    if (hold) {
        // The pre-roll owns the clock and winds through the file fast.  Re-base and touch nothing:
        // the level it would restore is about to be restored again by the tick that takes over.
        ++g.census.holds;
        g.next = src.FirstAfter(tUs);
        g.cursor_us = tUs;
        g.have_cursor = true;
        return;
    }

    if (!livesafety::Allowed(livesafety::Feature::StunEvents)) {
        g.census.refusal = "containment";
        return;
    }
    if (netGame == nullptr) {
        g.census.refusal = "no netGame";
        return;
    }
    const Handlers* h = Resolve();
    if (h->on_hit == nullptr || h->on_stun == nullptr || h->find == nullptr) {
        g.census.refusal = "the stun delegates did not bind";
        return;
    }
    void* cs = PunchableCs(h, netGame);
    if (cs == nullptr) {
        ++g.census.refused_cs;
        g.census.refusal = "the R14NetPunchable component system did not resolve";
        return;
    }
    g.census.refusal = nullptr;

    if (g.seek_pending) {
        ++g.census.seeks_suppressed;
        g.seek_pending = false;
    }
    if (backward) ++g.census.reverse_levels;

    if (!rebase && !backward) {
        for (uint32_t i = g.next; i < src.Count() && int64_t(src.At(i).t_us) <= tUs; ++i) {
            const Stun& s = src.At(i);
            LiveUser victim;
            if (map == nullptr || !map(mapCtx, SlotOf(s.victim), victim)) {
                ++g.census.unmapped_victim;
                if (g.logged < kLogCap) {
                    ++g.logged;
                    log::Direct(LogLevel::Info, "playback",
                                "stuns: refused at playhead %lld us: recorded victim slot %u is not "
                                "on this session's field",
                                (long long)(tUs - g.origin_us), unsigned(SlotOf(s.victim)));
                }
                continue;
            }
            LiveUser puncher;
            const bool havePuncher = map(mapCtx, SlotOf(s.puncher), puncher);
            if (!havePuncher) ++g.census.unmapped_puncher;

            uint32_t componentId = 0;
            if (!ComponentOnActor(h, cs, victim, componentId)) {
                ++g.census.refused_handle;
                // No punchable on that actor, so there is no record to write and no component to
                // hand a wrapper -- but the FX graphs listen on the ACTOR, so the delegates can
                // still be raised where they are addressed. Forward crossing only, exactly like the
                // wrapper path, so a paused drag or a reverse step raises nothing.
                bool raised = false;
                {
                    PawnProvision::EngineCallLatch latch;
                    raised = RaiseOnActor(h, cs, victim);
                }
                if (raised) ++g.census.actor_raises;
                if (g.logged < kLogCap) {
                    ++g.logged;
                    log::Direct(LogLevel::Info, "playback",
                                "stuns: refused at playhead %lld us: victim rec slot %u -> live "
                                "%u/gen %u carries no R14NetPunchable component; %u punchable(s) "
                                "on this system; OnHit/OnStun raised on the victim's actor: %s",
                                (long long)(tUs - g.origin_us), unsigned(SlotOf(s.victim)),
                                unsigned(SlotOf(victim.net_user_id)),
                                unsigned(victim.net_user_id >> 16), PunchableCount(cs),
                                raised ? "yes" : "no");
                }
                continue;
            }
            uint8_t* rec = RecordFor(cs, componentId);
            if (rec == nullptr) {
                ++g.census.refused_record;
                continue;
            }
            // State first, then the two raises: the engine's own order, and the FX script reads the
            // record it is about.
            if (!WriteStunned(rec, s.duration, 0.0f, /*pulse=*/true)) {
                ++g.census.refused_record;
                continue;
            }
            {
                PawnProvision::EngineCallLatch latch;
                if (!CallDelegateGuarded(h->on_hit, cs, componentId)) {
                    TurnOffAfterFault("delegate_OnHit wrapper");
                    return;
                }
                if (!CallDelegateGuarded(h->on_stun, cs, componentId)) {
                    TurnOffAfterFault("delegate_OnStun wrapper");
                    return;
                }
            }
            ++g.census.fires;

            if (g.logged < kLogCap) {
                ++g.logged;
                log::Direct(LogLevel::Info, "playback",
                            "stuns: fire at playhead %lld us: puncher rec slot %u -> live %u/gen %u, "
                            "victim rec slot %u -> live %u/gen %u, blocked %s, %.3f s, component "
                            "0x%08X",
                            (long long)(tUs - g.origin_us), unsigned(SlotOf(s.puncher)),
                            unsigned(SlotOf(puncher.net_user_id)),
                            unsigned(puncher.net_user_id >> 16), unsigned(SlotOf(s.victim)),
                            unsigned(SlotOf(victim.net_user_id)),
                            unsigned(victim.net_user_id >> 16), s.blocked ? "yes" : "no",
                            double(s.duration), componentId);
            }
        }
        g.next = src.FirstAfter(tUs);
        g.cursor_us = tUs;
        g.have_cursor = true;
        return;
    }

    // A discontinuity or a backward step: restore what the playhead implies and raise nothing.
    RestoreLevel(h, cs, tUs, map, mapCtx);
    g.next = src.FirstAfter(tUs);
    g.cursor_us = tUs;
    g.have_cursor = true;
}

}  // namespace stunfeed
}  // namespace playback
}  // namespace replay
