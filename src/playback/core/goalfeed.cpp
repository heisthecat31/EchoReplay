#include "playback/core/goalfeed.h"

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
#include "playback/core/goalfeed_facts.h"
#include "playback/core/recorded_loadout.h"

namespace replay {
namespace playback {
namespace goalfeed {

namespace gf = replay::goalfeed_facts;

namespace {

const char kTapName[] = "SR15NetReliablePostScoreUpdate";

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

float F32(const uint8_t* p, uint32_t off) {
    float v = 0.0f;
    memcpy(&v, p + off, sizeof v);
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------------------------

void RecordedGoals::Reset() {
    count_ = 0;
    chapter_first_ = 0;
}

bool RecordedGoals::Offer(uint64_t arrivalUs, const void* body, uint32_t len, ScanCensus& census) {
    if (len < kBodyBytes || body == nullptr) {
        ++census.refused_rows;
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(body);
    const uint16_t goalType = U16(p, gf::kOffGoalType);
    if (goalType > gf::kMaxGoalType) {
        // The banner's string switch has no arm for it, so what a row like this would put on screen
        // is not something the recording contains.  Counted, named, never shown.
        ++census.refused_rows;
        return false;
    }
    if (count_ >= kMaxRows) {
        ++census.dropped_rows;
        if (!census.first_drop) census.first_drop = "more goal rows than the store holds";
        return false;
    }
    Goal& g = rows_[count_++];
    g.t_us = arrivalUs;
    g.team = U16(p, gf::kOffTeam);
    g.points = U16(p, gf::kOffPoints);
    g.goal_type = goalType;
    g.scorer = U32(p, gf::kOffScorer);
    g.assist = U32(p, gf::kOffAssist);
    g.speed = F32(p, gf::kOffSpeed);
    g.distance = F32(p, gf::kOffDistance);
    g.possession_s = F32(p, gf::kOffPossession);
    // The bytes the engine is handed are the recording's, and only the two ids are ever rewritten
    // -- off a copy of this, per fire, so the store stays the file.
    memcpy(g.body, p, kBodyBytes);
    ++census.rows;
    return true;
}

void RecordedGoals::BeginChapter() { chapter_first_ = count_; }

void RecordedGoals::EndChapter(uint64_t tBeginUs, uint64_t tEndUs, uint64_t firstArrivalUs,
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

int32_t RecordedGoals::IndexAtOrBefore(int64_t tUs) const {
    if (count_ == 0 || tUs < 0) return -1;
    const uint64_t t = uint64_t(tUs);
    uint32_t lo = 0, hi = count_;   // first index with t_us > t
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (rows_[mid].t_us <= t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo == 0 ? -1 : int32_t(lo - 1);
}

uint32_t RecordedGoals::FirstAfter(int64_t tUs) const {
    if (tUs < 0) return 0;
    const int32_t at = IndexAtOrBefore(tUs);
    return at < 0 ? 0 : uint32_t(at) + 1;
}

uint32_t ScanRecordedGoals(IDemoReader& reader, RecordedGoals& into, ScanCensus& census) {
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
            // events are unreadable: stop rather than decode garbage, exactly as the stat scan does.
            if (!dec.Decode(r, m, body, sizeof body)) break;
            ++census.event_rows;
            // EVERY event row widens the arrival window, not just the goal rows: the window is what
            // locates a goal inside its chapter, and one built from goal rows alone would be a
            // single point.
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

RecordedGoals& PlaybackRecordedGoals() {
    static RecordedGoals s;
    return s;
}

ScanCensus& PlaybackGoalCensus() {
    static ScanCensus c;
    return c;
}

// ---------------------------------------------------------------------------------------------
// The feed
// ---------------------------------------------------------------------------------------------

namespace {

struct Feed {
    bool armed = false;
    bool horn = false;
    bool explosion = false;
    bool have_cursor = false;   // false until the first sweep, and after every seek
    bool seek_pending = false;
    int64_t cursor_us = 0;
    int64_t origin_us = 0;
    bool level_valid = false;
    uint32_t level_row = 0;
    // The host's own OnScoreCB reached the engine since the last sweep, so its IncrementScore has
    // overwritten the scorer fields with the host's team and its own point count. The next level
    // restore re-applies the RECORDED fill over them even though its row has not changed.
    bool pass_since_sweep = false;
    bool probed_role = false;
    // Reporting only: why a score-netvar change would not reach the engine's change notifier.
    const char* score_trigger_refusal = nullptr;
    uint32_t logged = 0;
    ApplyCensus census;
    const Handlers* injected = nullptr;
    Handlers bound;
    bool bind_tried = false;
};

Feed g;

// A match produces a handful of goals; a file that somehow produces thousands must not turn the log
// into the payload.  The census counts every fire whether or not it was printed.
inline constexpr uint32_t kLogCap = 64;

template <class Fn>
Fn Bind(const char* factId, uint64_t va, const uint8_t* pro, size_t n) {
    if (!buildguard::VerifySite(factId, va, pro, n)) return nullptr;
    return reinterpret_cast<Fn>(const_cast<void*>(buildguard::MappedAddress(va)));
}

const Handlers* Resolve() {
    if (g.injected != nullptr) return g.injected;
    if (!g.bind_tried) {
        g.bind_tried = true;
        g.bound.fill = Bind<FillFn>("GOAL.GAMEPLAY_FILL_VA", gf::kGameplayFillVa,
                                    gf::kGameplayFillPrologue, sizeof gf::kGameplayFillPrologue);
        g.bound.receive = Bind<ReceiveFn>("GOAL.RECEIVE_VA", gf::kReceiveVa, gf::kReceivePrologue,
                                          sizeof gf::kReceivePrologue);
        g.bound.sfx = Bind<GoalSfxFn>("GOAL.SFX_VA", gf::kGoalSfxVa, gf::kGoalSfxPrologue,
                                      sizeof gf::kGoalSfxPrologue);
        g.bound.raise = Bind<RaiseFn>("GOAL.EVENT_RAISE_VA", gf::kEventRaiseVa,
                                      gf::kEventRaisePrologue, sizeof gf::kEventRaisePrologue);
    }
    return &g.bound;
}

// SEH lives in POD-only functions: MSVC C2712 forbids __try beside the EngineCallLatch.
bool CallFillGuarded(FillFn fn, void* gameplay, const EngineGoalMsg* msg) {
    __try {
        fn(gameplay, msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallReceiveGuarded(ReceiveFn fn, void* netGame, const EngineGoalMsg* msg) {
    __try {
        fn(netGame, msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSfxGuarded(GoalSfxFn fn, void* netGame, uint32_t id) {
    __try {
        fn(netGame, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallRaiseGuarded(RaiseFn fn, void* netGame, uint64_t symbol) {
    __try {
        fn(netGame, symbol);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadPtrGuarded(const void* at, void*& out) {
    __try {
        out = *static_cast<void* const*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadRoleWordGuarded(const void* netGame, uint32_t& out) {
    __try {
        const uint32_t* const* at = reinterpret_cast<const uint32_t* const*>(
            static_cast<const uint8_t*>(netGame) + gf::kRoleWordPtrOff);
        if (*at == nullptr) return false;
        out = **at;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void TurnOffAfterFault(const char* what) {
    ++g.census.faulted;
    g.armed = false;
    g.census.refusal = "the engine's goal handler raised";
    log::Direct(LogLevel::Error, "playback",
                "goals: the engine's %s raised; the goal feed is off for the rest of this session",
                what);
}

// P-B0.  The role word decides whether the SCRIPT path to the fanfare is dead in this process, and
// therefore whether calling PlayGoalSFX directly can double-fire.  One line, once, never acted on.
void ProbeRoleWord(void* netGame) {
    if (g.probed_role) return;
    g.probed_role = true;
    uint32_t w = 0;
    if (!ReadRoleWordGuarded(netGame, w)) {
        log::Direct(LogLevel::Info, "playback",
                    "goals: probe P-B0: the role word at ng+0x%X could not be read",
                    unsigned(gf::kRoleWordPtrOff));
        return;
    }
    log::Direct(LogLevel::Info, "playback",
                "goals: probe P-B0: role word 0x%08X, client bit (&%u) %s -- the script fanfare "
                "node is %s in this process, so playback.goal_horn %s double-fire",
                w, unsigned(gf::kRoleClientBit), (w & gf::kRoleClientBit) ? "SET" : "clear",
                (w & gf::kRoleClientBit) ? "REACHABLE" : "refused",
                (w & gf::kRoleClientBit) ? "may" : "cannot");
}

const char* GoalTypeName(uint16_t t) {
    // The engine's own switch, for the LOG ONLY.  Nothing on screen is drawn from this: the banner
    // text comes from CR15NetGoalTypeStringExpression reading the field the fill handler wrote.
    switch (t) {
        case 0: return "no-goal";
        case 1: return "slam-dunk";
        case 2: return "inside-shot";
        case 3: return "long-shot";
        case 4: return "bounce-shot";
        case 5: return "long-bounce-shot";
        case 6: return "headbutt";
        case 7: return "long-headbutt";
        case 8: return "bumper-shot";
        case 9: return "long-bumper-shot";
        case 10: return "self-goal";
        default: return "?";
    }
}

}  // namespace

void InjectHandlersForTest(const Handlers* h) {
    g.injected = h;
    g.bind_tried = false;
    g.bound = Handlers{};
}

void Arm(bool on) {
    g.armed = on;
    g.logged = 0;
    g.probed_role = false;
    g.census.refusal = nullptr;
    Forget();
}

bool Armed() { return g.armed; }

void SetHornEnabled(bool on) { g.horn = on; }
bool HornEnabled() { return g.horn; }

void SetExplosionEnabled(bool on) { g.explosion = on; }
bool ExplosionEnabled() { return g.explosion; }

void SetPlayheadOriginUs(int64_t originUs) { g.origin_us = originUs; }

void SetScoreTriggerRefusal(const char* why) { g.score_trigger_refusal = why; }

void NoteSeek() { g.seek_pending = true; }

void NotePassSinceLastSweep() { g.pass_since_sweep = true; }

void Forget() {
    g.have_cursor = false;
    g.seek_pending = false;
    g.cursor_us = 0;
    g.level_valid = false;
    g.level_row = 0;
    g.pass_since_sweep = false;
}

const ApplyCensus& Census() { return g.census; }

void ResetCensusForTest() {
    g.census = ApplyCensus{};
    g.logged = 0;
}

namespace {

// The recorded generation belongs to the recording's lobby and is never carried across; the
// recorded SLOT is the key the roster mapping and the stat feed are both keyed by.
uint32_t RewriteId(uint32_t recorded, MapUserFn map, void* ctx, uint32_t& unmapped) {
    if (recorded == gf::kUnsetUserId) return recorded;
    uint32_t live = 0;
    if (map != nullptr && map(ctx, uint16_t(recorded & 0xFFFFu), live)) return live;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    ++unmapped;
    return gf::kNoUserId;
}

// Builds the 28 bytes the engine is handed: the recording's, with the two SR15NetUserIds rewritten
// from the recording's lobby to this session's.  `liveScorer` comes back so the horn and the log
// speak the id the engine was actually given.
void BuildBody(const Goal& src, MapUserFn map, void* ctx, uint8_t (&out)[kBodyBytes],
               uint32_t& liveScorer, uint32_t& liveAssist) {
    memcpy(out, src.body, kBodyBytes);
    // A recorded id that is the engine's own UNSET value is passed through untouched: a goal with
    // no assist carries it, and what the engine does with it is what a real client does with it.
    // It is the recording saying there was nobody, not a user we failed to map.
    liveScorer = RewriteId(src.scorer, map, ctx, g.census.unmapped_scorer);
    liveAssist = RewriteId(src.assist, map, ctx, g.census.unmapped_assist);
    memcpy(out + gf::kOffScorer, &liveScorer, sizeof liveScorer);
    memcpy(out + gf::kOffAssist, &liveAssist, sizeof liveAssist);
}

}  // namespace

void Sweep(void* netGame, int64_t tUs, bool hold, MapUserFn map, void* mapCtx) {
    if (!g.armed) {
        g.census.refusal = "not armed";
        return;
    }
    if (!livesafety::Allowed(livesafety::Feature::GoalEvents)) {
        g.census.refusal = "containment";
        return;
    }
    if (netGame == nullptr) {
        g.census.refusal = "no netGame";
        return;
    }
    const Handlers* h = Resolve();
    if (h->fill == nullptr || h->receive == nullptr) {
        g.census.refusal = "the goal handlers did not bind";
        return;
    }
    // netGame -> CR15NetGameplay is a LOAD.  A null here is a session that has no gameplay object
    // yet, which is a wait, not a fault.
    void* gameplay = nullptr;
    if (!ReadPtrGuarded(static_cast<const uint8_t*>(netGame) + gf::kGameplayPtrOff, gameplay) ||
        gameplay == nullptr) {
        g.census.refusal = "no gameplay object";
        return;
    }
    g.census.refusal = nullptr;

    const RecordedGoals& src = PlaybackRecordedGoals();

    // WHAT MAKES A CROSSING A CROSSING.  The cursor is the playhead the last sweep saw.  A jump the
    // user caused, the first sweep of a load and every tick the pre-roll owns the clock all re-base
    // it instead of sweeping the span -- so a forward seek across a goal, a load that opens past
    // one and the pre-roll's fast wind are all silent, and a pause leaves the cursor where it is so
    // resuming on top of a row cannot fire it twice.
    const bool rebase = !g.have_cursor || g.seek_pending || hold;
    if (g.seek_pending) {
        ++g.census.seeks_suppressed;
        g.seek_pending = false;
    }
    if (hold) ++g.census.holds;

    if (!rebase && tUs > g.cursor_us) {
        for (uint32_t i = src.FirstAfter(g.cursor_us);
             i < src.Count() && int64_t(src.At(i).t_us) <= tUs; ++i) {
            const Goal& goal = src.At(i);
            uint8_t body[kBodyBytes];
            uint32_t liveScorer = 0, liveAssist = 0;
            BuildBody(goal, map, mapCtx, body, liveScorer, liveAssist);

            EngineGoalMsg msg;
            msg.body = uint64_t(uintptr_t(body));

            ProbeRoleWord(netGame);
            {
                // FILL FIRST, THEN RAISE -- and this is NOT the order a real client dispatches in.
                // Both listeners append to one list and the dispatcher walks it head to tail, and
                // the receive handler is registered before the CR15NetGameplay ctor runs, so a
                // client raises first and fills second.  It gets away with it because the raise is
                // DEFERRED (sub_140510060's last argument), so the banner graph runs after both
                // stores either way.  We fill first anyway: the two orders are equivalent while
                // the raise stays deferred, and only this one is still correct if it ever is not.
                PawnProvision::EngineCallLatch latch;
                if (!CallFillGuarded(h->fill, gameplay, &msg)) {
                    TurnOffAfterFault("gameplay fill handler");
                    return;
                }
                if (!CallReceiveGuarded(h->receive, netGame, &msg)) {
                    TurnOffAfterFault("post-score receive handler");
                    return;
                }
            }
            ++g.census.fires;
            g.level_valid = true;
            g.level_row = i;

            const char* horn_why = nullptr;
            if (!g.horn) {
                horn_why = "off (playback.goal_horn)";
            } else if (h->sfx == nullptr) {
                horn_why = "PlayGoalFanfareSFX did not bind in this image";
            } else if (liveScorer == gf::kNoUserId || liveScorer == gf::kUnsetUserId) {
                horn_why = "the recorded scorer did not map to a live user";
            } else {
                PawnProvision::EngineCallLatch latch;
                if (CallSfxGuarded(h->sfx, netGame, liveScorer)) {
                    ++g.census.horn_calls;
                } else {
                    horn_why = "the guarded call into PlayGoalFanfareSFX refused";
                }
            }

            // THE EXPLOSION.  evt_glb_scored_matanimation, raised through the same deferred entry
            // the receive handler above tail-jumps to, so the rig's graph runs after both stores
            // exactly as it does behind a real goal.  Nothing about the rig is replicated here: the
            // raise is the whole change, and its one listener does the animating.
            const char* boom_why = nullptr;
            const bool blue_point = goal.team == gf::kTeamBlue;
            if (!g.explosion) {
                boom_why = "off (playback.goal_explosion)";
            } else if (!livesafety::Allowed(livesafety::Feature::GoalExplosion)) {
                boom_why = "containment (live-safety goal-explosion)";
            } else if (h->raise == nullptr) {
                boom_why = "GOAL.EVENT_RAISE_VA did not bind in this image";
            } else {
                PawnProvision::EngineCallLatch latch;
                // Two raises, because they are two different effects. The matanimation is the
                // rig's MATERIAL FLASH and its one listener owns no particle node, which is why a
                // goal that raised it was heard and not seen. fx_blue/red_score_event is the
                // explosion itself: three listeners, all cosmetic, NONE of them carrying an actor
                // filter, so a level-space raise reaches them. The recorded team (the one the point
                // went to, own goals included) picks the side. The side event only latches the
                // rig's colour flag and the send's burst reads it, and the deferred entry runs
                // them in raise order, so the side goes FIRST: sent the other way round, every
                // explosion wore the previous goal's colour (DIAG-GAMEMODE-GOALCOLOR B.2).
                const uint64_t fx = blue_point ? gf::kScoreFxBlueSymbol : gf::kScoreFxRedSymbol;
                if (CallRaiseGuarded(h->raise, netGame, gf::kExplosionSymbol) &&
                    CallRaiseGuarded(h->raise, netGame, fx) &&
                    CallRaiseGuarded(h->raise, netGame, gf::kScoreFxSendSymbol)) {
                    ++g.census.explosion_raises;
                } else {
                    boom_why = "the guarded call into the named-event raise refused";
                }
            }

            // WHY EACH ELEMENT DID OR DID NOT HAPPEN, at the one instant any of them could. The
            // owner saw a recorded goal with no effects and nothing in any log said which of the
            // four was missing or why. The flash and the accrual SFX hang off the engine's own
            // change notifier for blue_score/red_score, so their reason is the score writer's. The
            // explosion is not downstream of any of that: its trigger is the rig's own event, and
            // ENG-PRESENT-B2 established that raising it reaches one cosmetic listener -- unlike
            // nevt_increment_score_*, whose only listener is the match state machine.
            if (g.logged < kLogCap) {
                const char* trig = g.score_trigger_refusal;
                log::Direct(LogLevel::Info, "playback",
                            "goals: elements at playhead %lld us -- banner: raised "
                            "(nevt_post_score_update_received); score flash + accrual sfx: %s; "
                            "explosion: %s; horn: %s",
                            (long long)(tUs - g.origin_us),
                            trig ? trig : "armed on the blue_score/red_score change",
                            boom_why ? boom_why
                                     : "evt_glb_scored_matanimation raised (deferred) -- one "
                                       "listener, the goal rig 38965d90ab25e230, whose node set is "
                                       "FX/sound/material only; the raise is unaddressed, so a rig "
                                       "entry node that filters on the scoring actor ignores it",
                            horn_why ? horn_why
                                     : "PlayGoalFanfareSFX called for the live scorer -- the engine "
                                       "resolves goal_fx from the SCORER'S Customization component "
                                       "and with none it refuses silently");
            }

            if (g.logged < kLogCap) {
                ++g.logged;
                log::Direct(LogLevel::Info, "playback",
                            "goals: fire at playhead %lld us: team %u (%s point) +%u %s, explosion "
                            "%s, scorer rec slot %u -> live %u/gen %u, assist rec slot %u -> live "
                            "%u/gen %u, speed %.2f distance %.2f held %.2f s",
                            (long long)(tUs - g.origin_us), unsigned(goal.team),
                            blue_point ? "blue" : "orange", unsigned(goal.points),
                            GoalTypeName(goal.goal_type),
                            boom_why ? "none" : blue_point ? "blue" : "orange",
                            unsigned(goal.scorer & 0xFFFFu), unsigned(liveScorer & 0xFFFFu),  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
                            unsigned(liveScorer >> 16), unsigned(goal.assist & 0xFFFFu),  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
                            unsigned(liveAssist & 0xFFFFu), unsigned(liveAssist >> 16),  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
                            double(goal.speed), double(goal.distance), double(goal.possession_s));
                if (g.census.fires == 1) {
                    // P-B2.  The netvar guard already prints one `hud netvar host writes` line per
                    // second; the raise happened inside this second, so that line and the next one
                    // ARE the measurement.  Nothing is drained here -- a probe that consumed the
                    // counters would take them from the reader they belong to.
                    log::Direct(LogLevel::Info, "playback",
                                "goals: probe P-B2: nevt_post_score_update_received raised at "
                                "playhead %lld us -- the next two `hud netvar host writes` lines "
                                "are what any graph-driven netvar write would appear in",
                                (long long)(tUs - g.origin_us));
                }
            }
        }
    }

    g.cursor_us = tUs;
    g.have_cursor = true;

    // THE LEVEL.  Whatever the playhead is on, the gameplay object should hold the newest goal at
    // or before it -- that is what the banner's expression nodes read when a widget next evaluates,
    // and it is the half a seek must restore.  A fire above already left it there; this is what
    // covers a load, a seek and a backward scrub.  With no row at or before the playhead, the
    // engine's own fields are left exactly as it left them: there is no recorded "no goal yet" to
    // write, and zeroing would be a value we invented.
    // A host score that reached the engine since the last sweep left ITS scorer/team/points in the
    // gameplay block, so the answer being unchanged no longer means the block still holds it.
    const bool reapply = g.pass_since_sweep;
    g.pass_since_sweep = false;

    const int32_t li = src.IndexAtOrBefore(tUs);
    const bool same_row = g.level_valid && li >= 0 && g.level_row == uint32_t(li);
    if (li < 0 || (same_row && !reapply)) return;
    if (same_row) ++g.census.pass_reapplies;

    const Goal& goal = src.At(uint32_t(li));
    uint8_t body[kBodyBytes];
    uint32_t liveScorer = 0, liveAssist = 0;
    BuildBody(goal, map, mapCtx, body, liveScorer, liveAssist);
    EngineGoalMsg msg;
    msg.body = uint64_t(uintptr_t(body));
    {
        // FILL ONLY.  The receive handler raises unconditionally and cannot be asked not to, so the
        // level restore does not call it: a banner at a seek destination is the one thing about
        // this feed the owner would read as broken.
        PawnProvision::EngineCallLatch latch;
        if (!CallFillGuarded(h->fill, gameplay, &msg)) {
            TurnOffAfterFault("gameplay fill handler");
            return;
        }
    }
    ++g.census.level_writes;
    g.level_valid = true;
    g.level_row = uint32_t(li);
}

}  // namespace goalfeed
}  // namespace playback
}  // namespace replay
