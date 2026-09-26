#include "playback/core/componentfeed.h"

#include <windows.h>

#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "capture/keyframe.h"
#include "capture/manifestbuild.h"
#include "capture/streamcodec.h"
#include "engine/pawn_provision.h"
#include "foundation/demofile/demotypes.h"
#include "playback/core/componentfeed_facts.h"
#include "engine/scriptgraph_facts.h"
#include "playback/core/recorded_loadout.h"   // identity::TapIndexForName, the file's own tap table

namespace replay {
namespace playback {
namespace componentfeed {

namespace cf = replay::componentfeed_facts;

namespace {

const char kTapName[] = "SR15NetComponentEvent";

uint16_t U16(const uint8_t* p, uint32_t off) {
    uint16_t v = 0;
    memcpy(&v, p + off, sizeof v);
    return v;
}

uint32_t U32(const uint8_t* p, uint32_t off) {
    uint32_t v = 0;
    memcpy(&v, p + off, sizeof v);
    return v;
}

uint64_t U64(const uint8_t* p, uint32_t off) {
    uint64_t v = 0;
    memcpy(&v, p + off, sizeof v);
    return v;
}

// THE ALLOW-LIST.  Keyed on the PAIR, and every entry names the listener it was cleared against.
// The rule for adding a row is not "does this handler look cosmetic" but "does EVERY listener of
// this symbol have a node set with no R15NET*SENDEVENT*, no R15NETSET*, no physics/teleport/
// velocity node and no enable/platform/ownership node" -- a mechanical test over the script DLL's
// linked node classes.  RE-COMPONENT-EVENT-ENTRY.md S3.4a/S3.5 records the audit and three traps
// that each read as harmless.
struct AllowEntry {
    uint64_t script_id;
    uint64_t symbol;
    bool resets;
    const char* what;
};

constexpr AllowEntry kAllow[] = {
    // Reaches only 039b5a245c43ed2e, the tube light rig, whose entire node set is lights, models,
    // transforms and lists: zero send-event, zero netvar write, zero physics.
    {0x6aea99eeb2bab6c4ull, 0x6e0a199402b8e9afull, false, "tube light rig: nevt_launch_sequence"},
    // Same listener, and it is the rig's own return-to-base -- the reset that rebuilds its segment
    // list.  Pairing a launch with it is what stops a rig being stranded lit and parented.
    {0x6aea99eeb2bab6c4ull, 0x627e045b095b618full, true, "tube light rig: nevt_return_sequence"},
    // The goal explosion.  `scriptId` names the RAISER (0x14015D810 writes body+8 into
    // event->actor.nodeid), not the listener, so these three pairs are the scoring rig asking the
    // two goal-FX graphs to run.  Cleared against EVERY listener of each symbol -- 676dd8d22f877d73
    // (particles/models/anim), 4d027c6e7133f91b (particles/beams/materials/anim) and
    // 12220d6560c7f5d3 (Wwise 2-D + sound state): no R15NETSET*, no R15NET*SENDEVENT* other than
    // one SENDEVENTGLOBALNODE that re-raises only fx_send_score_event and AA3E7D5A, whose listener
    // sets are those same two FX graphs, so the transitive closure is closed and cosmetic.  Both
    // graphs self-limit on `event_* < 1`, so a re-feed while the animation runs is dropped engine
    // side.  RE-PRESENT-FALLBACKS.md B2-#1.
    {scriptgraph_facts::kArenaScriptHash, 0xe14549f2f4c40266ull, false, "goal fx: fx_blue_score_event"},
    {scriptgraph_facts::kArenaScriptHash, 0xf7b1de946009b29eull, false, "goal fx: fx_red_score_event"},
    {scriptgraph_facts::kArenaScriptHash, 0xfa9415c11a6a3b6cull, false, "goal fx: fx_send_score_event"},
};

// The goal call.  A live client hears "Goal!" / "3 points never looked so pretty!" because the
// server's announcer (e28785a6428603f9, R15NETSENDEVENTNODE) sends it one of these symbols from
// component 949c090e2472d440, and the symbol IS the dialogue trigger: the only script subscribing to
// any of them is a1991b50876efaa1, whose node set is DIALOGUESCENE / DIALOGUESCENEABORT /
// PLAYDIALOGUE / PLAYSOUND / DELAY / CUE plus three SENDEVENTGLOBALNODEs.  Those three raise
// fx_blue_wins / fx_red_wins / fx_draw, which reach 8fa489f60201fe41 (R15NETSETPROFILEFLAG,
// R15NETSENDEVENT), so every subscription whose thread reaches one of them is left out; the 37 that
// remain close inside dialogue and sound.  Reachability comes from calling the script DLL's own
// next_node on every node and pin: body-less pass-through nodes hide edges from a pseudocode walk.  The host never raises these itself -- its own goals are
// swallowed -- so nothing doubles.  ENG-GOAL-SFX.md has the listener scan and the edge walk.
constexpr uint64_t kAnnouncerScript = 0x949c090e2472d440ull;
constexpr uint64_t kGoalCallSymbols[] = {
    0x00683b01badeedebull, 0x0b806fb3d15abfbcull, 0x1ec8750e6e47257cull,
    0x28a2cc0b74cb01a4ull, 0x3b63bc6cbedbe0edull, 0x3b63bc6cbedbe3edull,
    0x3b63bc6cbedbe4edull, 0x3b63bc6cbedbeeedull, 0x452fcfebcf5cf578ull,
    0x45d6c267c43f1f38ull, 0x47a24a8203460acdull, 0x47a24a8203460fcdull,
    0x588766ed78a8935cull, 0x66f461df61aea120ull, 0x6cfb6cd15bd190a7ull,
    0x6cfb6cd15bd192a7ull, 0x6cfb6cd15bd19aa7ull, 0x6cfb6cd15bd19ba7ull,
    0x6cfb6cd15bd19ca7ull, 0x719813fc352d71b7ull, 0x719813fc352d71b8ull,
    0x719813fc352d71bbull, 0x719813fc352d71bdull, 0x719813fc352d71bfull,
    0x7b5cc8d9d65ac704ull, 0x7ba5c555dd392d44ull, 0x914dca5bfa62213aull,
    0xaf3ecd69e3641346ull, 0xb913daae32c2895bull, 0xb913daae32c28a5bull,
    0xb913daae32c28b5bull, 0xbbb5e1eecb89ad39ull, 0xbbb5e1eecb89ad3aull,
    0xbbb5e1eecb89ad3bull, 0xc24ac30553900ddaull, 0xf4d9778c427fc342ull,
    0xfcc0c9bb41f5d5e6ull,
};

}  // namespace

Verdict Classify(uint64_t scriptId, uint64_t symbol) {
    for (const AllowEntry& e : kAllow) {
        if (e.script_id == scriptId && e.symbol == symbol) return Verdict{true, e.resets, e.what};
    }
    if (scriptId == kAnnouncerScript) {
        for (uint64_t s : kGoalCallSymbols) {
            if (s == symbol) return Verdict{true, false, "announcer: goal call (a1991b50876efaa1)"};
        }
    }
    return Verdict{};
}

// ---------------------------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------------------------

namespace {

void Tally(ScanCensus& census, uint64_t scriptId, uint64_t symbol, bool allowed) {
    for (uint32_t i = 0; i < census.pair_count; ++i) {
        PairTally& p = census.pairs[i];
        if (p.script_id == scriptId && p.symbol == symbol) {
            ++p.rows;
            return;
        }
    }
    if (census.pair_count >= kMaxPairs) {
        ++census.pair_overflow;
        return;
    }
    PairTally& p = census.pairs[census.pair_count++];
    p.script_id = scriptId;
    p.symbol = symbol;
    p.rows = 1;
    p.allowed = allowed;
}

}  // namespace

void RecordedEvents::Reset() {
    count_ = 0;
    chapter_first_ = 0;
}

bool RecordedEvents::Offer(uint64_t arrivalUs, const void* body, uint32_t len, ScanCensus& census) {
    if (len < kBodyBytes || body == nullptr) {
        ++census.refused_rows;
        return false;
    }
    ++census.component_rows;
    const uint8_t* p = static_cast<const uint8_t*>(body);
    const uint64_t symbol = U64(p, cf::kOffSymbol);
    const uint64_t scriptId = U64(p, cf::kOffScriptId);
    const Verdict v = Classify(scriptId, symbol);
    Tally(census, scriptId, symbol, v.allowed);
    if (!v.allowed) {
        // Not a failure and not silence: the pair tally above is what makes widening the list later
        // an evidence-driven decision.
        ++census.denied_rows;
        return false;
    }
    if (count_ >= kMaxRows) {
        ++census.dropped_rows;
        if (!census.first_drop) census.first_drop = "more allow-listed rows than the store holds";
        return false;
    }
    Row& r = rows_[count_++];
    r.t_us = arrivalUs;
    r.symbol = symbol;
    r.script_id = scriptId;
    r.handle = U16(p, cf::kOffHandle);
    r.flags = U16(p, cf::kOffFlags);
    r.param = U32(p, cf::kOffParam);
    // The bytes the engine is handed are the recording's; the only edit is the relay bit, made on a
    // copy at fire time so the store stays the file.
    memcpy(r.body, p, kBodyBytes);
    ++census.rows;
    return true;
}

void RecordedEvents::BeginChapter() { chapter_first_ = count_; }

void RecordedEvents::EndChapter(uint64_t tBeginUs, uint64_t tEndUs, uint64_t firstArrivalUs,
                                uint64_t lastArrivalUs, ScanCensus& census) {
    if (chapter_first_ >= count_) return;
    const uint64_t span = tEndUs > tBeginUs ? tEndUs - tBeginUs : 0;
    const uint64_t arrivalSpan = lastArrivalUs > firstArrivalUs ? lastArrivalUs - firstArrivalUs : 0;
    for (uint32_t i = chapter_first_; i < count_; ++i) {
        if (span == 0) {
            // A chapter with no end after its begin cannot place anything; the row keeps the only
            // time the file gives it and the count says so.
            rows_[i].t_us = tBeginUs;
            continue;
        }
        uint64_t t;
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

void RecordedEvents::DropUnpairedRows(ScanCensus& census) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count_; ++i) {
        const Row& r = rows_[i];
        const Verdict v = Classify(r.script_id, r.symbol);
        bool paired = false;
        if (v.resets) {
            for (uint32_t j = 0; j < i && !paired; ++j) {
                paired = rows_[j].handle == r.handle &&
                         !Classify(rows_[j].script_id, rows_[j].symbol).resets;
            }
        } else {
            for (uint32_t j = i + 1; j < count_ && !paired; ++j) {
                paired = rows_[j].handle == r.handle &&
                         Classify(rows_[j].script_id, rows_[j].symbol).resets;
            }
        }
        if (!paired) {
            ++census.unpaired_rows;
            --census.rows;
            continue;
        }
        rows_[kept++] = r;
    }
    count_ = kept;
    chapter_first_ = count_;
}

uint32_t RecordedEvents::CountInSpan(int64_t beginUs, int64_t endUs) const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count_; ++i) {
        const int64_t t = int64_t(rows_[i].t_us);
        if (t >= beginUs && t <= endUs) ++n;
    }
    return n;
}

uint32_t RecordedEvents::FirstAfter(int64_t tUs) const {
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

uint32_t ScanRecordedEvents(IDemoReader& reader, RecordedEvents& into, ScanCensus& census) {
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
            // events are unreadable: stop rather than decode garbage, exactly as the goal scan does.
            if (!dec.Decode(r, m, body, sizeof body)) break;
            ++census.event_rows;
            // EVERY event row widens the arrival window, not just this tap's: the window is what
            // locates a row inside its chapter, and one built from four rows would be a single point.
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
    into.DropUnpairedRows(census);
    census.tap = tap;
    return into.Count();
}

RecordedEvents& PlaybackRecordedEvents() {
    static RecordedEvents s;
    return s;
}

ScanCensus& PlaybackEventCensus() {
    static ScanCensus c;
    return c;
}

void LogPairCensus(const ScanCensus& census) {
    for (uint32_t i = 0; i < census.pair_count; ++i) {
        const PairTally& p = census.pairs[i];
        if (p.allowed) continue;
        log::Direct(LogLevel::Info, "playback",
                    "component events:   refused script=0x%016llX sym=0x%016llX rows=%u",
                    (unsigned long long)p.script_id, (unsigned long long)p.symbol, p.rows);
    }
    if (census.pair_overflow != 0) {
        log::Direct(LogLevel::Info, "playback",
                    "component events:   %u further row(s) carried a pair past the %u the table "
                    "holds and are counted but not named",
                    census.pair_overflow, kMaxPairs);
    }
}

// ---------------------------------------------------------------------------------------------
// The feed
// ---------------------------------------------------------------------------------------------

namespace {

// One tube per slot, and the capture in hand uses two.  The launch latch is what stops a return
// sequence firing for a launch this forward pass never made.
inline constexpr uint32_t kMaxHandles = 16;

struct HandleState {
    bool used = false;
    uint16_t handle = 0;
    bool launched = false;
    bool pinned = false;
    uint64_t space = 0;
    uint64_t actor_ref = 0;
};

struct Feed {
    bool armed = false;
    bool have_cursor = false;  // false until the first sweep, and after every seek
    bool seek_pending = false;
    int64_t cursor_us = 0;
    int64_t origin_us = 0;
    bool probed = false;
    uint32_t logged = 0;
    ApplyCensus census;
    const Handlers* injected = nullptr;
    Handlers bound;
    bool bind_tried = false;
    ScriptIdOfActorFn script_id_fn = nullptr;
    void* script_id_ctx = nullptr;
    HandleState handles[kMaxHandles];
};

Feed g;

// Four rows in a whole capture; the cap is what stops a widened list turning the log into the
// payload.  The census counts every fire whether or not it was printed.
inline constexpr uint32_t kLogCap = 64;

template <class Fn>
Fn Bind(const char* factId, uint64_t va, const uint8_t* pro, size_t n) {
    if (!buildguard::VerifySite(factId, va, pro, n)) return nullptr;
    return reinterpret_cast<Fn>(const_cast<void*>(buildguard::MappedAddress(va)));
}

// The two data globals are bound through CODE anchors, not taken on trust.  The pool address is the
// rip-relative operand inside CR15NetIdCS::Actor's own prologue (the facts header static_asserts
// that derivation), and the local peer id is the operand of the `cmp rcx,[rip+disp]` this handler
// makes at +0xCA -- so verifying those bytes is what proves the address.
const uint64_t* BindGlobal(const char* factId, uint64_t anchorVa, const uint8_t* anchor, size_t n,
                           uint64_t va) {
    if (!buildguard::VerifySite(factId, anchorVa, anchor, n)) return nullptr;
    return static_cast<const uint64_t*>(buildguard::MappedAddress(va));
}

const Handlers* Resolve() {
    if (g.injected != nullptr) return g.injected;
    if (!g.bind_tried) {
        g.bind_tried = true;
        g.bound.entry = Bind<EntryFn>("CEVT.ENTRY_VA", cf::kEntryVa, cf::kEntryPrologue,
                                      sizeof cf::kEntryPrologue);
        g.bound.resolve =
            Bind<ResolveActorFn>("CEVT.RESOLVE_ACTOR_VA", cf::kResolveActorVa,
                                 cf::kResolveActorPrologue, sizeof cf::kResolveActorPrologue);
        g.bound.self_peer =
            BindGlobal("CEVT.SELF_PEER_VA", cf::kSelfPeerAnchorVa, cf::kSelfPeerAnchor,
                       sizeof cf::kSelfPeerAnchor, cf::kSelfPeerVa);
        g.bound.pool =
            BindGlobal("CEVT.POOL_PTR_VA", cf::kResolveActorVa, cf::kResolveActorPrologue,
                       sizeof cf::kResolveActorPrologue, cf::kPoolPtrVa);
    }
    return &g.bound;
}

// SEH lives in POD-only functions: MSVC C2712 forbids __try beside the EngineCallLatch.
bool CallEntryGuarded(EntryFn fn, void* netGame, void* smsg) {
    __try {
        fn(netGame, smsg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallResolveGuarded(ResolveActorFn fn, uint16_t handle, uint64_t (&out)[2]) {
    __try {
        fn(out, handle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU64Guarded(const uint64_t* at, uint64_t& out) {
    __try {
        out = *at;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPoolSlotGuarded(uint64_t pool, uint16_t idx, uint16_t& count, uint16_t& gen, bool& live) {
    __try {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(pool);
        memcpy(&count, base + cf::kPoolCountOff, sizeof count);
        memcpy(&gen, base + cf::kPoolGenOff + 2u * idx, sizeof gen);
        uint64_t word = 0;
        memcpy(&word, base + cf::kPoolLiveOff + 8u * (idx >> 6), sizeof word);
        live = (word >> (idx & 63u)) & 1u;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadLevelSpaceGuarded(const void* netGame, const void*& out) {
    __try {
        out = *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(netGame) +
                                                   cf::kLevelSpacePtrOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TurnOffAfterFault(const char* what) {
    ++g.census.faulted;
    g.armed = false;
    g.census.refusal = "the engine's component-event path raised";
    log::Direct(LogLevel::Error, "playback",
                "component events: the engine's %s raised; the component-event feed is off for the "
                "rest of this session",
                what);
}

HandleState& StateFor(uint16_t handle) {
    for (uint32_t i = 0; i < kMaxHandles; ++i) {
        if (g.handles[i].used && g.handles[i].handle == handle) return g.handles[i];
    }
    for (uint32_t i = 0; i < kMaxHandles; ++i) {
        if (!g.handles[i].used) {
            g.handles[i] = HandleState{};
            g.handles[i].used = true;
            g.handles[i].handle = handle;
            return g.handles[i];
        }
    }
    // More distinct handles than the table holds: the last slot is reused rather than growing, and
    // a reused slot loses only its launch latch, which fails CLOSED (the return does not fire).
    g.handles[kMaxHandles - 1] = HandleState{};
    g.handles[kMaxHandles - 1].used = true;
    g.handles[kMaxHandles - 1].handle = handle;
    return g.handles[kMaxHandles - 1];
}

// RE S Q5.  One line, before the first raise, printing everything the offline host has to agree with
// for the recorded handle to mean the same actor it meant on the recording server.  Nothing is acted
// on: if the handle does not resolve, the raise is refused a few lines below anyway.
void Probe(const Handlers* h, void* netGame, const Row& row, const uint64_t (&actor)[2]) {
    if (g.probed) return;
    g.probed = true;
    const uint16_t idx = row.handle & cf::kHandleIndexMask;
    uint16_t count = 0, gen = 0;
    bool live = false;
    uint64_t pool = 0;
    const bool havePool =
        h->pool != nullptr && ReadU64Guarded(h->pool, pool) && pool != 0 &&
        ReadPoolSlotGuarded(pool, idx, count, gen, live);
    const void* levelSpace = nullptr;
    ReadLevelSpaceGuarded(netGame, levelSpace);
    log::Direct(LogLevel::Info, "playback",
                "component events: probe handle=0x%04X idx=%u gen=%u count=%u live=%d "
                "space=0x%016llX actorRef=0x%016llX script=0x%016llX sym=0x%016llX levelSpace=%p%s",
                unsigned(row.handle), unsigned(idx), unsigned(gen), unsigned(count), live ? 1 : 0,
                (unsigned long long)actor[0], (unsigned long long)actor[1],
                (unsigned long long)row.script_id, (unsigned long long)row.symbol, levelSpace,
                havePool ? "" : " (pool unreadable: idx/gen/count/live are not measurements)");
}

enum class Outcome { Fired, Refused, Fault };

Outcome DispatchRow(const Handlers* h, void* netGame, const Row& row, int64_t tUs) {
    // Defense in depth: the store only holds allow-listed rows, but the list is the whole safety
    // argument and it is cheap to re-ask at the one place that reaches the engine.
    const Verdict v = Classify(row.script_id, row.symbol);
    if (!v.allowed) {
        ++g.census.refused_pair;
        return Outcome::Refused;
    }

    HandleState& st = StateFor(row.handle);
    if (v.resets && !st.launched) {
        // A return whose launch this forward pass never fired would rebuild a segment list that was
        // never taken apart.  Skipped, counted, never guessed at.
        ++g.census.unpaired_skipped;
        return Outcome::Refused;
    }

    const uint16_t idx = row.handle & cf::kHandleIndexMask;
    uint64_t actor[2] = {cf::kInvalidActorSpace, cf::kInvalidActorRef};
    const bool targeted = idx != cf::kHandleIndexNone;
    if (targeted) {
        if (!CallResolveGuarded(h->resolve, row.handle, actor)) {
            TurnOffAfterFault("component handle resolver");
            return Outcome::Fault;
        }
        Probe(h, netGame, row, actor);
        if (actor[0] == cf::kInvalidActorSpace ||
            uint16_t(actor[1] & 0xFFFFu) == cf::kActorRefNone) {  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
            // index >= count, a recycled generation, or a dead slot: the recorded handle does not
            // name a live actor on THIS host.  The known residual risk is a base-index shift
            // between the recording server and the offline host; this is where it surfaces.
            ++g.census.refused_handle;
            if (g.logged < kLogCap) {
                ++g.logged;
                log::Direct(LogLevel::Info, "playback",
                            "component events: refused at playhead %lld us: handle 0x%04X (idx %u "
                            "gen %u) does not resolve on this host",
                            (long long)(tUs - g.origin_us), unsigned(row.handle), unsigned(idx),
                            unsigned(row.handle >> cf::kHandleGenShift));
            }
            return Outcome::Refused;
        }
        if (!st.pinned) {
            st.pinned = true;
            st.space = actor[0];
            st.actor_ref = actor[1];
        } else if (st.space != actor[0] || st.actor_ref != actor[1]) {
            // The same handle resolving to a different actor mid-load means the pool was recycled
            // under us; the second half of a launch would then land on something else.
            ++g.census.refused_identity;
            return Outcome::Refused;
        }
        if (g.script_id_fn != nullptr) {
            uint64_t targetScript = 0;
            if (!g.script_id_fn(g.script_id_ctx, actor[0], actor[1], targetScript) ||
                targetScript != row.script_id) {
                ++g.census.refused_script_id;
                return Outcome::Refused;
            }
        }
    }

    uint8_t body[kBodyBytes];
    memcpy(body, row.body, kBodyBytes);
    // THE ONE EDITED BIT.  Set, and on a host, it would make the handler re-broadcast this row to
    // every peer.  Cleared here so sub_14012BB70 is unreachable whatever the host's role flags say:
    // declining to originate network traffic is not replicating a behaviour.
    body[cf::kOffFlags] &= uint8_t(~cf::kRelayBit);

    cf::EngineComponentMsg msg;
    if (h->self_peer != nullptr) {
        uint64_t peer = 0;
        if (ReadU64Guarded(h->self_peer, peer)) msg.senderPeerId = peer;
    }
    msg.body = uint64_t(uintptr_t(body));

    {
        // The shared re-entry latch every engine call this plugin issues from the tick takes: a
        // graph that re-entered us cannot start a second raise inside the first.
        PawnProvision::EngineCallLatch latch;
        if (!latch.taken) {
            ++g.census.reentrant;
            return Outcome::Refused;
        }
        if (!CallEntryGuarded(h->entry, netGame, &msg)) {
            TurnOffAfterFault("component event handler");
            return Outcome::Fault;
        }
    }

    ++g.census.fires;
    st.launched = !v.resets;

    if (g.logged < kLogCap) {
        ++g.logged;
        log::Direct(LogLevel::Info, "playback",
                    "component events: fire at playhead %lld us: script=0x%016llX sym=0x%016llX "
                    "(%s) handle=0x%04X idx=%u gen=%u param=%u -- handle %s, relay bit cleared",
                    (long long)(tUs - g.origin_us), (unsigned long long)row.script_id,
                    (unsigned long long)row.symbol, v.what, unsigned(row.handle), unsigned(idx),
                    unsigned(row.handle >> cf::kHandleGenShift), row.param,
                    targeted ? "resolved" : "is the engine's own no-target form");
    }
    return Outcome::Fired;
}

}  // namespace

void InjectHandlersForTest(const Handlers* h) {
    g.injected = h;
    g.bind_tried = false;
    g.bound = Handlers{};
}

void InjectScriptIdResolverForTest(ScriptIdOfActorFn fn, void* ctx) {
    g.script_id_fn = fn;
    g.script_id_ctx = ctx;
}

void Arm(bool on) {
    g.armed = on;
    g.logged = 0;
    g.probed = false;
    g.census.refusal = nullptr;
    Forget();
}

bool Armed() { return g.armed; }

void SetPlayheadOriginUs(int64_t originUs) { g.origin_us = originUs; }

void NoteSeek() { g.seek_pending = true; }

void Forget() {
    g.have_cursor = false;
    g.seek_pending = false;
    g.cursor_us = 0;
    for (uint32_t i = 0; i < kMaxHandles; ++i) g.handles[i] = HandleState{};
}

const ApplyCensus& Census() { return g.census; }

void ResetCensusForTest() {
    g.census = ApplyCensus{};
    g.logged = 0;
    g.probed = false;
}

void Sweep(void* netGame, int64_t tUs, bool hold) {
    if (!g.armed) {
        g.census.refusal = "not armed";
        return;
    }
    if (!livesafety::Allowed(livesafety::Feature::ComponentEvents)) {
        g.census.refusal = "containment";
        return;
    }
    if (netGame == nullptr) {
        g.census.refusal = "no netGame";
        return;
    }
    const Handlers* h = Resolve();
    if (h->entry == nullptr || h->resolve == nullptr) {
        g.census.refusal = "the component-event entry points did not bind";
        return;
    }
    g.census.refusal = nullptr;

    // WHAT MAKES A CROSSING A CROSSING.  The cursor is the playhead the last sweep saw.  The first
    // sweep of a load, a jump the user caused and every tick the pre-roll owns the clock all re-base
    // it instead of sweeping the span -- so a seek across a launch, a load that opens past one and
    // the pre-roll's fast wind are all silent.  A pause leaves the cursor where it is, so resuming
    // on top of a row cannot fire it twice, and reverse never satisfies `tUs > cursor`.
    const bool rebase = !g.have_cursor || g.seek_pending || hold;
    if (g.seek_pending) {
        ++g.census.seeks_suppressed;
        g.seek_pending = false;
    }
    if (hold) ++g.census.holds;
    if (!rebase && tUs < g.cursor_us) ++g.census.reverse_skipped;

    const RecordedEvents& src = PlaybackRecordedEvents();
    if (!rebase && tUs > g.cursor_us) {
        uint32_t i = src.FirstAfter(g.cursor_us);
        uint32_t processed = 0;
        int64_t reached = g.cursor_us;
        for (; i < src.Count() && int64_t(src.At(i).t_us) <= tUs; ++i) {
            if (processed == kMaxFiresPerTick) break;
            if (DispatchRow(h, netGame, src.At(i), tUs) == Outcome::Fault) return;
            ++processed;
            reached = int64_t(src.At(i).t_us);
        }
        if (i < src.Count() && int64_t(src.At(i).t_us) <= tUs) {
            // The rate limit.  The remainder is CARRIED, in order, not dropped: half a launch
            // sequence is the one visible residue this feed can leave.
            for (uint32_t j = i; j < src.Count() && int64_t(src.At(j).t_us) <= tUs; ++j) {
                ++g.census.deferred;
            }
            g.cursor_us = reached;
            g.have_cursor = true;
            return;
        }
    }

    g.cursor_us = tUs;
    g.have_cursor = true;
}

}  // namespace componentfeed
}  // namespace playback
}  // namespace replay
