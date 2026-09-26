#include "playback/core/viewer_role.h"

#include <string.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/services.h"
#include "engine/pawn_provision.h"
#include "generated/facts_generated.h"

namespace replay::playback {
namespace {

// The role names, indexed by the value.  Only two entries carry a fact behind them, and the
// static_asserts below say which: `spectator` sits at GAME.TEAM_SPECTATOR because the registry
// puts it there, and every entry the gate accepts is one the table calls a spectating role.  The
// rest are the engine's documented semantics -- 0 and 1 are the playing teams whose counts
// IDENT.SERVER_ASSIGN_TEAM keeps at ng+2*team+230, 3 is a participating non-team role, and 7 is
// the all-ones state every consumer prints as -1 -- and they are labels for a reader, not inputs
// to any decision here.
#ifdef SPEC_MUTATE_LABEL_SHIFT
// The label table rotated by one, which is how a role table drifts in practice: every name is still
// present and spelled correctly, and only the value each one sits at has moved.
const char* const kRoleNames[] = {"out-of-range", "blue",      "orange",       "spectator",
                                  "non-team",     "moderator", "out-of-range", "out-of-range"};
#else
const char* const kRoleNames[] = {"blue",      "orange",       "spectator",    "non-team",
                                  "moderator", "out-of-range", "out-of-range", "no-team"};
#endif

constexpr uint16_t kRoleCount = uint16_t(sizeof kRoleNames / sizeof kRoleNames[0]);

static_assert(facts::GAME_TEAM_SPECTATOR < kRoleCount, "the spectator role is outside the table");
static_assert(facts::IDENT_USER_TEAM_MASK + 1u == kRoleCount, "the table is not the field's width");

// V1's deferral budget and V4's issue bound.  Neither is an engine constant -- they are this
// chain's own patience -- so they are named here and nowhere else.  240 pumps is four seconds of
// stage-0 ticks: long enough for the engine's own auto-pick to land, short enough that a run which
// never seats says so while it is still running.
constexpr unsigned kAutoPickPumpBudget = 240;
constexpr unsigned kIssueBudget = 3;

// The netgame's state word sits at offset 0, which the registry WITHHOLDS: a zero displacement has
// no byte for an anchor to sit on, and pawn_provision.cpp:1179 records that same refusal for the
// same read.  The CONSTANT is the registry's -- NETGAME.STATE_INMATCH, the value CR15NetGame::
// Update's own spectator-rig gate tests `*a1` against -- so the term that could drift is imported.

const uint8_t* EntrantWord(void* ng, uint16_t index) {
    return static_cast<const uint8_t*>(ng) + facts::LOCAL_NETGAME_USER_RECORD_BASE_OFF +
           size_t(index) * facts::USER_STRIDE;
}

}  // namespace

const char* const kViewerLogModule = "identity";

bool SessionInMatch(void* ng) {
    if (!Readable(ng, sizeof(uint32_t))) return false;
    uint32_t state = 0;
    memcpy(&state, ng, sizeof state);
    return state == facts::NETGAME_STATE_INMATCH;
}

bool RoleSpectates(uint16_t role) {
    // Written in 16-bit arithmetic because the engine's is: mov eax,0FFFDh / sub cx,2 / test ax,cx.
    // CAM.SPECTATOR_TEAM_GATE first maps a role of 5 or more to 0xFFFF; that clamp changes no
    // answer for a 3-bit field -- 5, 6 and 7 all fail this test on their own -- so it is not
    // reproduced here, and the mask case checks the two agree over every value.
    const uint16_t delta = uint16_t(role - uint16_t(facts::GAME_TEAM_SPECTATOR));
#ifdef SPEC_MUTATE_MASK_WIDE
    // Bit 1 put back into the mask, which is the whole of what the engine's constant clears: the
    // gate then accepts role 2 alone and refuses the moderator seat, a change that reads as a
    // harmless tightening and silently drops a spectating role.
    return uint16_t(delta & uint16_t(facts::GAME_SPECTATOR_GATE_MASK | 2u)) == 0;
#else
    return uint16_t(delta & uint16_t(facts::GAME_SPECTATOR_GATE_MASK)) == 0;
#endif
}

const char* ViewerRoleName(uint16_t role) {
    return role < kRoleCount ? kRoleNames[role] : "out-of-range";
}

uint16_t RoleNoTeam() {
    return uint16_t(facts::IDENT_USER_TEAM_MASK);
}

bool ReadEntrantRole(void* ng, uint16_t index, uint16_t& out) {
    if (ng == nullptr) return false;
    const uint8_t* const word = EntrantWord(ng, index);
    if (!Readable(word, sizeof(uint16_t))) return false;
    const uint16_t packed = *reinterpret_cast<const uint16_t volatile*>(word);
    out = uint16_t((packed >> facts::IDENT_USER_TEAM_SHIFT) & facts::IDENT_USER_TEAM_MASK);
    return true;
}

void ViewerSeat::Reset() {
    *this = ViewerSeat();
}

SeatStep ViewerSeat::Pump(SeatScope scope, void* ng, const PawnProvision::ActorHops* hops,
                          ILog* log) {
    // ---- BEFORE EVERYTHING, INCLUDING THE SCOPE TERM.  V3 calls IDENT.SERVER_ASSIGN_TEAM for
    // entrant 0, which is the person at the keyboard: in a live public match that is a real team
    // change on a real account, visible to the other nine players. The scope term below refuses in
    // that session too, but only because `g.loaded` cannot become true uncontained -- one hop of
    // indirection, and SEAT_IN_CAPTURE compiles the scope term out entirely.
    if (!livesafety::Allowed(livesafety::Feature::ViewerSeat)) {
        if (log && !uncontained_refused_) {
            uncontained_refused_ = true;
            log->Write(LogLevel::Info, kViewerLogModule,
                       "ViewerSeatInert -- %s; entrant 0 keeps the team the engine gave it",
                       livesafety::VerdictReason(livesafety::Current()));
        }
        return SeatStep::Uncontained;
    }

    // ---- BEFORE V0, AND BEFORE THE PUMP COUNTER.  The seat belongs to a session this module has
    // a demo open in, and to nothing else: L2 #17 seated the RECORDING user of a capture-only
    // session as a spectator at 03:09:05, the physics pool then answered 0 bodies for the
    // remaining 216 s, and the file carries no S_BODY_SAMPLED at all.  That session has nothing
    // open, so it lands here -- a refusal by name rather than a silent skip, spending no pump, no
    // budget and no latch, because a session that was never asked has not deferred anything.
    //
    // A live capture is NOT what refuses: capture records during playback too, so a recording term
    // refused every playback launch there was (L3 RUN-31 F1).
#ifndef SEAT_IN_CAPTURE
    if (scope != SeatScope::Playback) {
        if (log && !not_playback_refused_) {
            not_playback_refused_ = true;
            log->Write(LogLevel::Info, kViewerLogModule, "%s",
                       "ViewerSeatNotPlayback -- no replay is open in this process, so the viewer "
                       "seat is not issued and entrant 0 keeps the team the engine gave it");
        }
        return SeatStep::NotPlaybackSession;
    }
#endif
    const uint64_t window = uint64_t(pumps_++) / kPumpsPerWindow;
    // ---- V0.  Every term is a precondition for READING the record, so none of them may spend a
    // deferral budget: the plugin ticks before the engine has a netgame at all, and a refusal that
    // fires on every session before the game exists is a refusal nobody reads.
    if (ng == nullptr) return SeatStep::NoNetGame;
    const uint8_t* const local_users =
        static_cast<const uint8_t*>(ng) + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF;
    if (!Readable(local_users, sizeof(uint16_t))) return SeatStep::RecordUnreadable;
    if (*reinterpret_cast<const uint16_t volatile*>(local_users) == 0) return SeatStep::NoLocalUser;

    uint16_t role = 0;
    if (!ReadEntrantRole(ng, 0, role)) return SeatStep::RecordUnreadable;

    // The pre-seat measurement AUD-S14 F4/R1-F2 left open: what entrant 0's team bits hold BEFORE
    // anything of ours has touched them.  Taken on the first pump that can read the record and
    // printed once, so a run answers the question V1's "not 7" predicate is provisional on.
    if (!preseat_measured_) {
        preseat_measured_ = true;
        preseat_role_ = role;
    }
    if (!preseat_logged_ && log) {
        preseat_logged_ = true;
        log->Write(LogLevel::Info, kViewerLogModule, "viewer slot 0 role=%u (%s) preseat",
                   unsigned(role), ViewerRoleName(role));
    }

    // ---- NOT YET IN MATCH IS A DEFERRAL, NOT A REFUSAL.  Before state 9 the
    // engine has not been asked for anything: CR15NetGame::Update reaches the rig enable only at
    // `*a1 == 9`, so a viewer on a playing team at state 4/6/7/8 is NOT-YET-SEATED, not refused.
    // Spending a refusal budget here made a session that goes on to seat correctly print the
    // acceptance's own refusal words, and the clause could not then be satisfied by any passing
    // run.  V4's budget, every refusal and the issue counter all start at the first pump in
    // state 9, and this arm says so at Info, once per window.
#ifndef W2P1_MUTATE_BUDGET_BEFORE_INMATCH
    if (!SessionInMatch(ng)) {
        if (log && deferral_logged_window_ != window) {
            deferral_logged_window_ = window;
            log->Write(LogLevel::Info, kViewerLogModule,
                       "viewer seat: deferred (SessionNotInMatch) -- slot 0 is %s (role=%u) and "
                       "the session has not reached NETGAME.STATE_INMATCH, where the engine's own "
                       "rig gate lives; nothing has been asked of it yet",
                       ViewerRoleName(role), unsigned(role));
        }
        return SeatStep::SessionNotInMatch;
    }
#endif
    in_match_seen_ = true;

    // ---- V2.  The engine's own gate already accepts this seat, so there is nothing to issue and
    // no transition to spend.
#if !defined(W2P1_MUTATE_VIEWER_ALWAYS_ISSUE) && !defined(VIEWER_ALWAYS_ISSUE)
    if (RoleSpectates(role)) return SeatStep::AlreadySeated;
#endif

    // ---- V1.  Seating observed, in the same three bits sub_140178090, the entrant dump and
    // IDENT.SERVER_ASSIGN_TEAM read.  The all-ones state is the one value every engine consumer
    // treats as "no team at all" -- the dump prints -1 for it -- so it is the one this waits out.
#if defined(W2P1_MUTATE_VIEWER_SEATED_ROLE0) || defined(VIEWER_ISSUE_BEFORE_SEATED)
    const bool seated = role != 0;
#else
    const bool seated = role != RoleNoTeam();
#endif
    if (!seated) {
        ++awaiting_;
        if (awaiting_ >= kAutoPickPumpBudget && !autopick_refused_ && log) {
            autopick_refused_ = true;
            log->Write(LogLevel::Warn, kViewerLogModule,
                       "ViewerAutoPickNotObserved -- entrant 0 still has no team (role=%u) after "
                       "%u in-match pumps, so there is no seating for this chain to correct",
                       unsigned(role), awaiting_);
        }
        return SeatStep::AwaitingSeat;
    }

    // ---- V4's bound, read BEFORE the call: an engine that keeps re-issuing its own team request
    // must not be answered forever, and the refusal names the role it kept seeing.
#if !defined(W2P1_MUTATE_VIEWER_UNBOUNDED) && !defined(VIEWER_SEAT_UNGATED)
    if (issues_ >= kIssueBudget) {
        if (!budget_refused_ && log) {
            budget_refused_ = true;
            log->Write(LogLevel::Warn, kViewerLogModule,
                       "ViewerSeatRefused -- %u issues spent and slot 0 is %s (role=%u) again; the "
                       "engine is re-seating the local user and this chain will not fight it",
                       issues_, ViewerRoleName(role), unsigned(role));
        }
        return SeatStep::BudgetSpent;
    }
#endif

    // ---- V4's CADENCE.  S14.10's V4 row is "each census window", not each tick:
    // three chances spread over three windows, so a postcondition that fails once does not burn
    // the whole budget inside 50 ms.  The FIRST issue waits for nothing -- kNoWindow is no window.
    if (last_issue_window_ == window) return SeatStep::ReissueWaitsForWindow;

    // ---- V3's preconditions.  Both can still come good -- the Identity re-binds its surface
    // across a load -- so they DEFER first; unlike the state term they cannot come good on their
    // own, so the patience V1 gets buys a named refusal that says which one never did.
    SeatStep blocked_on = SeatStep::Issued;
    const char* why = nullptr;
    if (hops == nullptr) {
        blocked_on = SeatStep::NoHopSurface;
        why = "IIdentity::ActorHops() answered null, so nothing bound the hop surface";
    } else if (hops->assignTeam == nullptr) {
        blocked_on = SeatStep::NoAssignTeamBind;
        why = "the hop surface carries no IDENT.SERVER_ASSIGN_TEAM bind";
    }
    if (why != nullptr) {
        ++blocked_;
        if (blocked_ >= kAutoPickPumpBudget && !not_issued_refused_ && log) {
            not_issued_refused_ = true;
            log->Write(LogLevel::Warn, kViewerLogModule,
                       "ViewerSeatNotIssued -- slot 0 is %s (role=%u) and wants the spectator seat, "
                       "but %s (after %u in-match pumps)",
                       ViewerRoleName(role), unsigned(role), why, blocked_);
        }
        return blocked_on;
    }

    // ---- V3.  The engine's own local-seat call, through the SAME bind A3 uses, wrapped in the
    // crash marker and the ONE re-entrancy latch every engine call from this stage-0 tick shares
    // (pawn_provision.h:744).  A latch already held means another authored transition is on the
    // stack: defer under ITS OWN name -- attributing it to the state term would report the wrong
    // cause on the one occasion it ever fires.
    {
        PawnProvision::EngineCallLatch latch;
        if (!latch.taken) {
            ++blocked_;
            return SeatStep::EngineCallInFlight;
        }
#if defined(W2P1_MUTATE_VIEWER_IDX)
        const uint16_t index = 1;
#else
        const uint16_t index = 0;
#endif
#if defined(W2P1_MUTATE_VIEWER_TEAM_4)
        const uint16_t want = 4;
#else
        const uint16_t want = uint16_t(facts::GAME_TEAM_SPECTATOR);
#endif
        PawnProvision::MarkHop(3, "viewer seat assignTeam", hops->ctx, index);
        ++issues_;
        last_issue_window_ = window;
        const bool issued = hops->assignTeam(hops->ctx, index, want);
#if defined(W2P1_MUTATE_VIEWER_TRUST_RETURN) || defined(VIEWER_TRUSTS_RETURN)
        // The call's own answer taken for the verdict, which is exactly what the two-reads rule
        // forbids: IDENT.SERVER_ASSIGN_TEAM early-outs silently on a session that refuses it.
        if (issued) return SeatStep::Issued;
#else
        (void)issued;
#endif
    }

    // ---- V3's postcondition: the RECORD, re-read.  Never the call's return value.
    uint16_t after = 0;
    if (!ReadEntrantRole(ng, 0, after) || after != uint16_t(facts::GAME_TEAM_SPECTATOR)) {
        if (log) {
            log->Write(LogLevel::Warn, kViewerLogModule,
                       "ViewerSeatRefused -- issue %u called IDENT.SERVER_ASSIGN_TEAM for slot 0 "
                       "and the record still reads %s (role=%u)",
                       issues_, ViewerRoleName(after), unsigned(after));
        }
        return SeatStep::PostconditionFailed;
    }
    return SeatStep::Issued;
}

}  // namespace replay::playback
