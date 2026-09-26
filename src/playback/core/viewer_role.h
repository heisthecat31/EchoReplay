#pragma once

#include <stdint.h>

namespace replay {
class ILog;
namespace PawnProvision { struct ActorHops; }
}  // namespace replay

// The VIEWER's role in a playback session, read back out of the engine's own record -- and, since
// ARCH-23 S14.10, SEATED there by the engine's own local-seat call.
//
// A replay viewer that is seated as a player gets hands, a HUD and a body, and cannot fly the
// camera; the engine enables its shipping spectator rig only when entrant 0's team satisfies
// CAM.SPECTATOR_TEAM_GATE.  `-lobbyteam 2` stays on the engine line and is the request; the
// in-client offline host does not honour it, so ViewerSeat issues IDENT.SERVER_ASSIGN_TEAM the way
// the engine's own UserAssignTeamRequestCB does, and believes only the record it re-reads after.
namespace replay::playback {

// The engine's own predicate, not an enumeration of the values it happens to accept:
// ((role - GAME.TEAM_SPECTATOR) & GAME.SPECTATOR_GATE_MASK) == 0, which is true for exactly
// {2, 4} -- spectator and moderator.  Registering the mask instead of the number 4 is what makes
// this the engine's set rather than our memory of it.
// The module string every line in this chain goes out under, so the acceptance's `[identity]`
// anchor has ONE definition and the census line in playback_module.cpp cannot drift from it.
extern const char* const kViewerLogModule;

bool RoleSpectates(uint16_t role);

// The netgame's session state word tested against NETGAME.STATE_INMATCH -- the value, and the only
// value, at which CR15NetGame::Update reaches the spectator-rig enable (`*a1 == 9`, sub_1401BBDB0
// :235).  Published because the census line has to render a pre-match session differently: before
// state 9 a viewer on a playing team is NOT YET SEATED, and calling that a refusal makes the
// acceptance unsatisfiable by a run that goes on to succeed.
bool SessionInMatch(void* netGame);

// A word for each of the eight values the 3-bit team field can hold.  Never null.
const char* ViewerRoleName(uint16_t role);

// The all-ones state of the 3-bit team field, which is what every engine consumer treats as "no
// team at all": the entrant dump prints -1 for it and CAM.SPECTATOR_TEAM_GATE maps it to -1.  It
// is IDENT.USER_TEAM_MASK itself and is written that way so no second copy of 7 exists.
uint16_t RoleNoTeam();

// Entrant `index`'s packed role, out of the record at NETGAME + LOCAL_NETGAME_USER_RECORD_BASE_OFF
// + index * USER_STRIDE, through IDENT.USER_TEAM_SHIFT / IDENT.USER_TEAM_MASK -- the same three
// bits sub_140178090, the entrant dump and IDENT.SERVER_ASSIGN_TEAM itself read.  False when the
// netgame is null or the word is not readable; `out` is untouched then.

bool ReadEntrantRole(void* netGame, uint16_t index, uint16_t& out);

// * THE SESSION-MODE PREDICATE (AUD-SEAT-SCOPE F1/F2).  Two facts, each read from the module that
// owns it, and they are exclusive by construction: the seat is issued only when the PLAYBACK module
// owns this session AND the CAPTURE module is not recording.
//
// It is NOT the launch carrier.  Nothing in src/ ever sets ECHOREPLAY_OPEN, so a demo opened at
// RUNTIME -- the panel's browser click (ui::LoadPath) or the console `load` -- was refused for the
// whole life of the process (F1).
//
// Nor is it "and no capture is recording".  Capture is ALWAYS ON -- locked constraint 6, the
// plugin records during playback sessions too -- so that term refused the seat on the state-9
// tick of every playback launch there is (L3 RUN-31 F1, 0 x role=2).  The capture-only session
// L2 #17 named has nothing open, and `loaded` already answers it.
//
// `Playback` is the playback module's own `loaded`: the fact a successful open sets, whether the
// open came from the launch or from a click.  The definition lives in playback_module.cpp -- the
// one translation unit that holds that fact -- and there is no second statement of it anywhere.
enum class SeatScope : uint8_t {
    Playback,     // a demo is open here: the seat is ours to issue
    NoDemoOwned,  // this module has nothing open -- the session is not a playback at all
};
SeatScope SessionSeatScope();

// The same rule as one bit, for callers that need only the verdict.
bool PlaybackOwnsSession();

// Where one pump of the V-chain ended.  The verdict is never the call's return value.
enum class SeatStep : uint8_t {
    NotPlaybackSession, // no demo is open here -- nothing is asked of the engine at all
    NoNetGame,          // V0
    RecordUnreadable,   // V0
    NoLocalUser,        // V0
    AwaitingSeat,       // V1 -- the record still says "no team at all"
    AlreadySeated,      // V2 -- role is in the gate's set, so no call is made
    SessionNotInMatch,  // NOT YET in match -- a DEFERRAL that spends no budget and names no refusal
    ReissueWaitsForWindow,  // V4 -- one issue per census window, never one per tick
    EngineCallInFlight, // the shared stage-0 latch is held by another authored transition
    NoHopSurface,       // V3 -- IIdentity::ActorHops() answered null
    NoAssignTeamBind,   // V3 -- the surface carries no IDENT.SERVER_ASSIGN_TEAM
    Issued,             // V3 -- called, and the RE-READ record says spectator
    PostconditionFailed,// V3 -- called, and the record did not move
    BudgetSpent,        // V4 -- three issues have been made and the role keeps leaving the set
    Uncontained,        // this process may not write engine state at all (livesafety::ViewerSeat)
};

// V0..V4 of ARCH-23 S14.10.  One instance per session; `Pump` once per tick from the stage-0
// provider slot, which is the first call of the frame and the only place an authored transition
// may be issued from (S14.6, P1-18).
//
// Nothing here is stored across a pump except the counters and the latches: the hop surface is the
// Identity's, whose owner re-binds it across a load, so it is taken as an argument every time.
// One census window, in pumps.  The chain counts its own so that a window is a property of the
// chain rather than of whatever counter a caller happens to hold; it is the same 60 ticks the
// census line and W2-E-31c's rig line use.
constexpr unsigned kPumpsPerWindow = 60;
constexpr uint64_t kNoWindow = ~uint64_t(0);

class ViewerSeat {
public:
    // `scope` is SessionSeatScope() at the call site: anything but `Playback` refuses BY NAME
    // before V0 and spends nothing -- no pump, no budget, no engine call.  A parameter rather than
    // a read inside, so a test drives both answers without the process environment and the
    // wiring is one grep-able line in the caller.
    // `hops` is IIdentity::ActorHops() -- may be null, and says so by name when it is.
    SeatStep Pump(SeatScope scope, void* netGame, const PawnProvision::ActorHops* hops,
                  ILog* log);

    // A new load assigns its own entrants; the pre-seat measurement and the issue budget belong to
    // the session that took them.
    void Reset();

    unsigned Issues() const { return issues_; }
    unsigned Pumps() const { return pumps_; }
    // True once a pump has seen NETGAME.STATE_INMATCH.  V4's budget and every refusal in this
    // chain start there: before it the session is not refusing the seat, it has not been asked.
    unsigned AwaitingPumps() const { return awaiting_; }
    unsigned BlockedPumps() const { return blocked_; }
    // The role the record held on the FIRST pump that could read it, before any issue -- the
    // measurement AUD-S14 F4/R1-F2 left open.  Meaningless until PreseatMeasured() is true.
    uint16_t PreseatRole() const { return preseat_role_; }
    bool PreseatMeasured() const { return preseat_measured_; }
    bool InMatchSeen() const { return in_match_seen_; }

private:
    unsigned pumps_ = 0;
    bool in_match_seen_ = false;
    // The window of the last issue, so V4 re-issues once per census window rather than once per
    // tick: three chances spread over three windows, which is what S14.10's V4 row specifies.  No
    // issue has been made while this holds kNoWindow.
    uint64_t last_issue_window_ = kNoWindow;
    uint64_t deferral_logged_window_ = kNoWindow;
    // The V1 deferral budget: pumps counted only where the record WAS read and said "no team",
    // so a netgame that does not exist yet cannot spend it.
    unsigned awaiting_ = 0;
    // The same for a pump that wanted to issue and could not -- an unbound hop surface, or a
    // session that has not reached the state the engine's own rig gate needs.
    unsigned blocked_ = 0;
    unsigned issues_ = 0;
    uint16_t preseat_role_ = 0;
    bool preseat_measured_ = false;
    bool preseat_logged_ = false;
    bool autopick_refused_ = false;
    bool not_issued_refused_ = false;
    bool budget_refused_ = false;
    bool not_playback_refused_ = false;
    bool uncontained_refused_ = false;
};

}  // namespace replay::playback
