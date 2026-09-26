// W2-B. The playback module: IReplayTransport, the frame loop, and the registration that makes all
// of it exist inside the DLL.
//
// ---------------------------------------------------------------------------------------------
// ★★★ WHY THIS FILE IS THE MOST IMPORTANT ONE IN THE PACKET DESPITE BEING THE SMALLEST.
//
// Everything behind it -- the transport arithmetic, the resolver, the window, ApplyStateAt, the chain
// -- was verified by 72,000 assertions and linked as a static library the DLL never pulled in, with
// ReplayServices::transport null. Four packets consume that pointer. W2-F's panel had a branch that
// rendered "No replay transport has registered yet", and that branch was the only thing in the whole
// tree that noticed.
//
// ★★ Verified and wired are different properties, and every test in this packet drove the library
// directly, so none of them could tell the difference. That is the lesson, and it is why the
// registration below is not boilerplate: it is the step that turns a proven library into a product.
//
// ★ ORDERING, NOT SIMPLIFICATION. Load / Play / Pause / Seek / Step / SeekRound land here complete;
// the pieces still missing are named in ActiveModeLabel and reported, never stubbed to look present.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <chrono>

#include "audio/audioresidue.h"
#include "bootstrap/buildguard.h"
#include "bootstrap/config.h"
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/module_order.h"
#include "bootstrap/offlinelock.h"
#include "bootstrap/mainprof.h"
#include "bootstrap/tickcost.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/services.h"
#include "capture/capturestop.h"
#include "camera/camera_control.h"
#include "engine/audiorate.h"
#include "engine/clock.h"
#include "engine/actor_alias.h"
#include "engine/entity.h"
#include "engine/disccensus.h"
#include "engine/gamestate_facts.h"
#include "engine/scriptgraph_facts.h"
#include "capture/markers.h"
#include "foundation/gametype.h"
#include "engine/identity_access.h"
#include "engine/wiretap.h"
#include "engine/netvar.h"
#include "engine/pawn_provision.h"
#include "engine/session.h"
#include "engine/session_watch.h"
#include "engine/tick.h"
#include "foundation/demofile/demofile.h"
#include "foundation/demofile/format.h"
#include "identity/dresser.h"
#include "playback/core/apply.h"
#include "playback/core/discontinuity.h"
#include "playback/core/netvarguard.h"
#include "playback/core/scoregate.h"
#include "playback/core/hosthold.h"
#include "playback/core/clockpin.h"
#include "playback/core/levelspace.h"
#include "playback/core/shellfeed.h"
#include "playback/core/shellplan.h"
#include "playback/core/netvarhud.h"
#include "playback/core/netvarsite.h"
#include "playback/core/recorded_loadout.h"
#include "playback/core/componentfeed.h"
#include "playback/core/goalfeed.h"
#include "playback/core/stunfeed.h"
#include "playback/core/statsfeed.h"
#include "playback/core/reload_cycle.h"
#include "playback/core/viewer_role.h"
#include "playback/core/ownership.h"
#include "playback/core/sample.h"
#include "playback/core/playback.h"
#include "playback/core/transport.h"
#include "playback/core/window.h"
#include "playback/core/windowbuild.h"
#include "playback/core/windowprefetch.h"
#include "playback/residue/residue.h"

#include <memory>

static const char* const kReplayModuleName = "playback";

namespace replay {
namespace playback {
namespace {

// ---------------------------------------------------------------------------------------------
// The residue rows NOBODY SERVES, registered as UNMAPPED.
//
// ★★★ AN EMPTY UNAVAILABLE-LIST READS AS "EVERYTHING IS AVAILABLE". The battery was built to report
// which category-N primitives a build cannot run, and with nothing registered UnavailableIds()
// returned zero -- indistinguishable from a build where every primitive works. That is the fails=0
// shape again: absence of evidence rendering as evidence of absence.
//
// ★★ AND A PLACEHOLDER THAT OUTLIVES ITS SUBJECT IS THE SAME DEFECT INVERTED. Seven rows were
// deleted from this table in the change that armed W2-H's and W3-A's real ones, because N-2, N-3,
// N-4, N-7, N-8, N-16 and N-17 now have owners that register them. Two of those ids -- N-2 and N-4 --
// collided EXACTLY with W3-A's, and ResidueDiagnostic() answers with the first match: the panel would
// have shown a working primitive as unmapped, quoting a reason that stopped being true, while
// UnavailableCount() counted it twice. A row here is a statement that this build runs nothing for
// that id, so it has to be withdrawn the moment that stops being so.
//
// What is left is the design's genuinely unserved rows, and W3-A's header names the same six
// with a reason apiece in docs/spikes/W3-A.md.
// ---------------------------------------------------------------------------------------------
class UnmappedResidue final : public IResidue {
public:
    UnmappedResidue(const char* id, const char* why, bool reverse_too)
        : id_(id), why_(why), reverse_(reverse_too) {}

    const char* Id() const override { return id_; }
    Availability Avail() const override { return Availability::Unmapped; }
    const char* Diagnostic() const override { return why_; }
    bool RunsOn(Cause c) const override {
        if (c == Cause::ReverseTick) {
            return reverse_;   // S7.5.5's per-tick subset; the two of it left here are N-5 and N-13
        }
        return c == Cause::Seek || c == Cause::MicroSeek || c == Cause::GapCross;
    }
    void Fire(const Discontinuity&) override {}   // never called: the battery skips a non-Ready row

private:
    const char* id_;
    const char* why_;
    bool reverse_;
};

// S7.4.3's table. The `effect` column is why each one matters, in the words the design uses.
UnmappedResidue g_residues[] = {
    {"N-5", "stat/score accumulators compute clockTime - base; a backwards seek makes them negative",
     true},
    {"N-9", "particle pools and per-particle age: ResetPools is LIBR15_VA", false},
    {"N-10", "particle RNG seed: the emitter Reset(CSimpleRandom&) is LIBR15_VA", false},
    {"N-12", "decals: ResetActorGenerations is LIBR15_VA", false},
    {"N-13", "animation clocks hold an absolute UTime; pre-roll cannot recover it", true},
    {"N-19", "mission-script fibers: UpdateFastForward semantics are [U]", false},
};

// ---------------------------------------------------------------------------------------------

// One second, in the microseconds every playhead, bias and stamp in this module is expressed in.
// The wall-clock conversion above and the periodic status cadence below are the same number by
// construction: a status line that drifts from the clock it reports is worse than no line.
constexpr uint64_t kUsPerSecond = 1000000ull;

// The module string the provisioner's own census writes under, so a grep for [pawn] finds the
// ledger line beside the census rows it summarises.
const char* const kPawnModuleName = "pawn";

// The viewer's own role goes out under its own name, so a grep for [identity] finds the one line
// that says whether this session can spectate at all without wading through the roster census.
// TAKEN from viewer_role.h, never spelled again: the seat's lines and this
// census have to carry the same anchor, and two definitions of the same string are two things
// to rename.
const char* const kViewerModuleName = kViewerLogModule;

constexpr uint16_t kNoNetVarRefusal = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids

// ★ DESIGN-UI-2 S5.3.1. EXACTLY ONE HOLD, EVER, PER SCRUB. audio::HoldAudioSeekMute is a refcount
// (audioresidue.cpp:28), so paired calls scattered across five exit paths cannot be checked by
// anything: one unbalanced take and the session is silent with no error anywhere. This object is
// the only thing in the module that calls it for a drag, and its destructor is the backstop for
// module teardown.
class ScrubMuteHold {
public:
    void Take()    { if (!held_) { audio::HoldAudioSeekMute(true);  held_ = true;  } }
    void Release() { if ( held_) { audio::HoldAudioSeekMute(false); held_ = false; } }
    bool Held() const { return held_; }
    ~ScrubMuteHold() { Release(); }

private:
    bool held_ = false;
};

struct Global {
    // Slot 0 is built here, at DLL load, because that is exactly when the `Window window;` this
    // replaced was built. A window has always existed for the whole life of the process and a
    // handful of diagnostics read it above the load gate; making it appear at Load instead would
    // have been a behaviour change smuggled in under an ownership change.
    Global() {
        win_store[0].reset(new Window());
        cur = win_store[0].get();
    }

    ReplayServices* svc = nullptr;
    ILog* log = nullptr;

    DemoReader reader;
    bool loaded = false;
    // Set by Shutdown before it unloads. Unload is shared between "the owner closed the replay" and
    // "the process is going away", and only the first of those may drive the engine.
    bool exiting = false;
    // When the leave feed was filled, and whether its wait has already been reported once.
    uint64_t leave_wait_since_ms = 0;
    bool     leave_wait_said = false;
    // RELOAD-6: a mid-session Open re-enters the level before the new roster is admitted.
    reloadcycle::Machine reload_cycle;
    uint32_t reload_cycles = 0;          // cycles armed this process, for the log

    // ★★★ THE WINDOW IS HELD BY POINTER, AND THAT IS THE WHOLE OF WINDOW-DECODE-1's OWNERSHIP
    // CHANGE. It used to be a Window BY VALUE here, which made "adopt a window somebody else
    // decoded" impossible without copying megabytes -- and Window is deliberately neither copyable
    // nor movable. Three heap slots instead, and adoption is two pointer assignments.
    //
    //   win_store[0]  always allocated; it is `cur` at load.
    //   win_store[1]  allocated only when playback.async_window_decode is ON.
    //   win_store[2]  ditto.
    //
    // Roles are raw pointers INTO win_store, never owners, and every Window object outlives every
    // role it is given:
    //   cur       the window the tick samples. Never null.
    //   ready     decoded by the worker and waiting for the playhead to want it, or null.
    //   free_win  the buffers with no job. At most two, and the array IS the memory bound: there is
    //             nowhere to put a third, so nothing can quietly grow the resident set.
    // A buffer lent to the worker is in none of these -- that is what "borrowed" means here, and it
    // is why the tick cannot touch it. cur + ready + free + borrowed == 3 with the flag on, and the
    // slot count never moves with the length of the file or the size of a seek storm.
    std::unique_ptr<Window> win_store[3];
    Window* cur = nullptr;
    Window* ready = nullptr;
    Window* free_win[2] = {nullptr, nullptr};
    // What `ready` holds, and which load it belongs to. A result whose serial is not the live
    // load_serial is a window decoded for a file that is no longer open.
    size_t   ready_centre = (size_t)-1;
    uint32_t ready_serial = 0;
    BuildStats ready_build;
    uint64_t ready_since_us = 0;   // when it landed, for the adoption-latency field

    // playback.async_window_decode, read once per load. The worker is started only when this is on,
    // and with it off nothing below this line ever runs.
    bool async_window_decode = false;
    // ★★ SET BY A JOIN THAT TIMED OUT, AND NEVER CLEARED AGAIN IN THIS PROCESS. A worker that did
    // not come out of its decode still owns a buffer this module cannot name, so every window it
    // could be holding is abandoned rather than reused -- and a later Load must not start a second
    // worker over an abandoned first. Per PROCESS, not per load: that is what makes it safe.
    bool prefetch_abandoned = false;
    bool prefetch_abandoned_said = false;
    // The centre last posted to the worker, so the threshold does not re-post every frame.
    size_t   prefetch_posted_centre = (size_t)-1;
    uint32_t prefetch_posted_serial = 0;

    // ★ THE INSTRUMENT THE OPERATOR READS. `tick_decodes` is the defect itself: a window decoded ON
    // the tick thread. Steady playback with the worker on must hold it at zero and a seek must cost
    // exactly one, so a rate that is not ~0/min is the feature not working, whatever the frame times
    // say.
    uint64_t tick_decodes = 0;
    uint64_t adoptions = 0;
    uint64_t adopt_us_last = 0;
    uint64_t adopt_us_max = 0;
    uint64_t prefetch_late = 0;     // the playhead wanted a window the worker had not finished
    uint64_t prefetch_stale = 0;    // results dropped: wrong load, or a centre nobody wants any more
    uint64_t prefetch_fallbacks = 0;   // boundaries the worker lost and the tick decoded instead
    uint64_t tick_decodes_at_line = 0;
    uint64_t async_line_at_us = 0;

    BuildStats build;
    // Measured once per load: the wire senders' clock offsets every window build places wire
    // records with, and the deepest chapter the pools are sized for.
    TrackScan track_scan;
    // The admission side of the dress ladder.  Armed for the life of a LOAD, not of an epoch: an
    // entity re-bind re-dresses, and the re-dress has to reuse the mapping this load already made.
    PawnProvisioner pawn;
    void* tick_net_game = nullptr;
    // Per load: a match that ended before this file was opened is not this load's ended session.
    SessionWatch session;
    // Tried once per process, not once per tick: a registration that the engine refused is refused
    // for a reason a retry on the next frame cannot change, and re-asking every frame would hold
    // the netgame's listener lock sixty times a second for nothing.
    bool reply_route_tried = false;
    bool reply_route_armed = false;
    // The tap the reply route registers through. Owned here because this module owns the tick that
    // created it; identity_access holds the interface and never the lifetime.
    IWiretap* reply_tap = nullptr;
    // Asked only for the [pawn] ledger line, so the bind's own resolver is not perturbed by it.
    PawnResolver pawn_census;
    // W2-P1's recorded -> provisioned slot map, copied out of PawnProvisioner's ledger, which is
    // the ONE place a recorded slot's live slot is decided. The bind and the pose line both read
    // it from here; neither keeps a second copy. `slot_map_n` 0 is the identity map.
    uint32_t load_serial = 0;   // IReplayTransport::LoadSerial
    uint16_t slot_map[PawnResolver::kSlots];
    size_t   slot_map_n = 0;
    BindResult bind;
    // Wall microseconds since the last periodic position line.
    uint64_t position_accum_us = 0;
    // The pacing measure the periodic line reports: where the playhead was a second ago, and the
    // REAL microseconds since. Their ratio against the rate is the whole of "does the replay run at
    // the speed it says", which no log line could answer before.
    int64_t pace_playhead_us = 0;
    uint64_t pace_wall_us = 0;
    bool pace_armed = false;
    // Sign of the rate the last tick ran at; 0 until one has run. A change of it is a discontinuity
    // for every filtered consumer downstream, even though the playhead did not jump.
    int  last_rate_sign = 0;
    // Per-second, not cumulative: a machine that is simply slow clamps EVERY frame of EVERY second,
    // a hitch clamps one frame of one second, and the running totals cannot tell the two apart.
    uint32_t pace_frames = 0;
    uint32_t pace_clamps = 0;
    uint64_t pace_dropped_us = 0;
    // Which interpolant the apply pass gave each bracket this second, and why the line won where
    // it did. Cleared with the pacing counters on the line that reports them.
    HermiteTally hermite_tally;
    EndState end_state_said = EndState::None;   // edge-triggered: the line is a transition, not a state
    // A discontinuity owes the reader a position line NOW, not up to a second later: a bind or a
    // seek is exactly when where-the-bodies-are is the question.
    bool position_line_pending = false;
    // PER-WINDOW, not latched (ARCH-23 S14.10): V4 re-reads the record every census window and
    // re-issues the seat if the role has left the gate's set, so a line printed once could not say
    // what the session's role IS -- only what it was the first time anybody looked.  The tick this
    // window's line is due at; 0 means the first pump that can read the record.
    uint64_t viewer_census_due_tick = 0;
    // The role line answers the question every window, but it is the same answer every window: at
    // the census cadence it was 2.9 lines a second and 6% of a steady log. Spaced by
    // `log.chatter_period_ms`, and printed immediately whenever the answer CHANGES -- so a session
    // that is wrong from its first frame still says so on its first window.
    uint64_t viewer_role_chatter_at = 0;
    bool viewer_role_seen = false;
    uint32_t viewer_role_said = 0;
    // V0..V4.  One per session; Reset() with the load, because a second load assigns its own
    // entrants and the issue budget belongs to the session that spent it.
    ViewerSeat viewer_seat;
    Transport transport;
    StateApplier applier;
    OwnerGuard owners;
    ResidueBattery battery;

    bool as_seen = false;
    bool window_valid = false;
    // ★ Once ANY window built for this file has yielded netvar keys, the file carries netvar
    // records. Sticky for the life of the load: a later quiet window must not let the panel report
    // the capture as carrying none, which is a claim about the file made from one playhead.
    bool netvar_records_seen = false;
    // The netvar bind follows the engine's cached route, which is re-validated every tick: bound
    // means `netvar_site` still names the live component instance.
    bool netvar_table_bound = false;
    bool netvar_bind_wanted = false;   // loaded, contained, and a gamestate flag is on
    bool host_netvar_filter_on = false;   // playback.host_netvar_filter
    bool disc_drive_on = false;           // playback.disc_drive, and the score gate is in place
    bool live_safety_said = false;        // the apply-pass refusal is named once, not per tick
    bool disc_score_gate_on = true;       // playback.disc_score_gate
    // The last mode the score gate published and the term that decided it, so the gate's state is
    // an edge line instead of a per-tick silence. `why` is a string literal, compared by pointer.
    bool score_gate_said = false;
    scoregate::Mode score_gate_mode = scoregate::Mode::PassThrough;
    const char* score_gate_why = "";
    bool grip_inputs_on = true;           // playback.grip_inputs
    uint32_t voicewatch_ms = 0;           // audio.voicewatch_ms, 0 = off
    uint32_t foley_census_ms = 0;         // audio.foley_census_ms, 0 = off
    uint32_t foley_deep_at_ms = 60000;    // audio.foley_deep_at_s, in ms
    uint32_t disc_census_ms = 0;          // disc.diff_census_ms, 0 = off
    bool host_hold_on = true;             // playback.host_hold
    bool host_hold_pause_on = false;      // playback.host_hold_pause
    // DESIGN-HOST-SYNC P1: the host arena graph's own game_timer, pinned while loaded.
    bool host_clock_pin_on = true;        // playback.host_clock_pin
    clockpin::Pin host_clock_pin;
    uint64_t host_clock_line_ms = 0;      // the once-a-second `host clock:` line
    const char* host_clock_why_said = nullptr;
    uint32_t host_clock_misses = 0;      // arena-graph walks that found nothing since the last say
    uint64_t host_clock_miss_said_ms = 0;
    // DESIGN-HOST-SYNC P4: the disc FX graph's shell, reconciled to the recording.
    bool disc_shell_on = true;            // playback.disc_shell
    shellplan::Plan shell_plan;
    shellfeed::Reconciler shell_feed;
    uint32_t shell_epoch = 0;             // bumped by every user seek
    bool shell_transport_set = false;     // the playhead was last placed by seek, scrub or reverse
    int32_t shell_last_k = -1;
    uint32_t shell_last_epoch = 0;
    int64_t shell_last_t = 0;
    bool shell_prev_on = false;
    shellfeed::GrabClearLatch shell_grab_said;
    int32_t shell_prev_w6 = 0, shell_prev_w1 = 0;
    IEntity::ScriptGraphRef shell_fx;
    uint32_t shell_fx_phys = 0;           // the disc component id the FX graph was matched through
    bool shell_fx_ok = false;
    uint64_t shell_fx_space = 0;          // levelspace::Key the FX graph was found under
    bool shell_grabs_settled = false;     // span grabs filled for the disc track id below
    uint64_t shell_grabs_id = 0;
    uint64_t shell_fx_walk_ms = 0;
    const char* shell_fx_why_said = nullptr;
    bool shell_send_refused_said = false;
    uint64_t host_phase_accum_us = 0;     // the once-a-minute host-phase line
    uint32_t host_phase_minutes = 0;
    const char* host_phase_said = nullptr;   // the phase the edge line last named; a kPhaseFacts row
    bool hermite_on = true;               // playback.hermite
    // Motion smoothing (DESIGN-SMOOTHING.md). Mode and strength come from config at load and from
    // the menu after it; `smooth_w` is the 200 ms crossfade toward what the mode wants this frame.
    SmoothMode smooth_mode = SmoothMode::SlowMo;   // playback.motion_smoothing
    uint8_t smooth_strength = kSmoothStrengthDefault;   // playback.motion_smoothing_strength
    float smooth_w = 0.0f;
    bool smooth_snap = true;              // land the weight on its target: a load, seek or scrub
    SmoothTally smooth_tally;
    bool smooth_trace_on = false;         // playback.motion_smoothing_trace
    uint32_t smooth_trace_lines = 0;
    bool disc_probe_pending = false;      // the once-per-bind disc line is still owed
    // The one claim_owner track of the window, found at bind time. The gate, the revalidation and
    // the probe all ask about it every tick, and none of them may walk the track list to do it.
    static constexpr size_t kNoDiscTrack = (size_t)-1;
    size_t disc_track = kNoDiscTrack;
    uint64_t disc_retry_accum_us = 0;     // the unbound re-attempt's rate limit
    uint64_t disc_fresh_accum_us = 0;     // and the BOUND chain's re-walk, on the same cadence
    BindResult::DiscVerdict disc_logged_verdict = BindResult::DiscVerdict::NotAsked;
    uint32_t disc_logged_id = 0;
    // One line per distinct verdict per load, and a count of the changes that did not get one.
    // The chain can alternate at 2 Hz around a round reset -- the ref goes stale, then resolves
    // before the phys component is re-created -- and a line per change buries the log in it.
    uint32_t disc_logged_mask = 0;
    uint32_t disc_verdict_repeats = 0;
    // The early path: which of its hops we have already named, and whether the hand-over to the
    // engine's own ref has been reported. Both are once-per-load lines -- the early path is asked
    // again every 500 ms while it refuses, and one line per attempt is 200 lines of pre_match.
    const char* disc_early_refusal_logged = nullptr;
    bool disc_early_bound_logged = false;
    bool disc_early_agree_logged = false;
    // The registry head is a bounded, once-per-load dump; the candidate line beside it is not.
    bool disc_early_symbols_logged = false;
    // ★ THE CANDIDATE LINES ARE KEYED ON WHAT THE CANDIDATES SAID, NOT ON THE REFUSAL STRING. Every
    // failed poll produced the same literal, so ~100 polls per launch collapsed into one line and
    // nobody could say whether the level space ever filled. The digest covers every candidate's
    // root, alias, the registry state and count AT EVERY DEPTH, sym= and live=.
    uint64_t disc_early_digest = 0;
    bool     disc_early_digest_valid = false;
    uint64_t disc_early_quiet_us = 0;       // since the last set of lines
    uint32_t disc_early_changes_held = 0;   // changes the rate limit swallowed since then
    bool     disc_early_change_pending = false;

    // ---- THE PRE-ROLL ----------------------------------------------------------------------
    // Once per load, at load: the host's 45 s pre-match script is metered by THIS module's clock
    // (RE-PREMATCH-LEVERS §3b), so with the playhead pinned the engine clock runs it out fast and
    // the disc is bound before the first second the owner watches.
    enum PreRoll : uint8_t { kPreRollUnstarted, kPreRollWaiting, kPreRollRunning, kPreRollOver };
    PreRoll preroll = kPreRollUnstarted;
    bool     preroll_enabled = false;        // playback.preroll
    float    preroll_rate = 2.0f;            // playback.preroll_rate -- the LAUNCH-sequence scale
    float    preroll_scale = 4.0f;           // playback.preroll_clock_scale -- the PRE-MATCH scale
    uint64_t preroll_bound_us = 55000000;    // playback.preroll_bound_ms
    uint64_t preroll_wall_us = 0;            // wall since the load, across the wait AND the run
    uint64_t preroll_run_us = 0;             // wall with the engine clock actually running fast
    // The timeline, three phases deep, so the next launch measures itself instead of being argued
    // about: wall is ours (QPC), engine is the SIM clock differenced, which is the only honest
    // answer to "did the scale we asked for actually happen".
    uint64_t preroll_wait_sim_us = 0;        // admission, dressing and the engine's own level load
    uint64_t preroll_fast_wall_us = 0;       // the pre-match countdown, at preroll_scale
    uint64_t preroll_fast_sim_us = 0;
    uint64_t preroll_ramp_wall_us = 0;       // the launch sequence, at preroll_rate
    uint64_t preroll_ramp_sim_us = 0;
    uint64_t preroll_prev_sim_us = 0;        // last tick's engine sim stamp, 0 = none yet
    bool     preroll_ramped = false;         // the ramp-down edge has been logged
    bool     preroll_bound_disc_logged = false;  // "bound, still holding for the launch", once
    bool     disc_host_held = false;         // last tick's hold_disc_pose, so the line prints on edges
    bool     preroll_play_requested = false; // Play was pressed while it prepared
    float    preroll_play_rate = 1.0f;
    // LAUNCH-FIX3 counted 425 changes of answer from the rules chain while the disc was unbound,
    // and the bind line names only the first of them: the verdict enum is the same for all of
    // them, so the coalescer counts the repeats and throws the hop away. These name each distinct
    // hop once and count the transitions between them.
    static constexpr size_t kDiscHopsRemembered = 8;
    const char* disc_rules_hops[kDiscHopsRemembered] = {};
    size_t disc_rules_hops_n = 0;
    const char* disc_rules_hop_last = nullptr;
    uint32_t disc_rules_hop_changes = 0;
    bool netvar_spread_warn_pending = false;   // owed until a window build has the real numbers
    bool hud_clock_on = false;
    bool hud_score_on = false;
    bool stats_on = false;
    bool goals_on = false;
    bool stuns_on = false;
    bool component_events_on = false;
    NetVarSite netvar_site;
    uint16_t netvar_last_refusal = kNoNetVarRefusal;   // NetVarSite::RefusalKey
    size_t window_centre = (size_t)-1;
    // ★★★ THE FILE'S SEEKABLE SPAN, IN ABSOLUTE SOURCE MICROSECONDS, TAKEN FROM THE CHAPTER TABLE.
    // The seekable range and the duration the scrub bar is drawn against are the SAME two numbers
    // read off the SAME table -- computed once here rather than twice at two call sites, which is
    // how a timeline comes to disagree with the thing it scrubs.
    int64_t core_first_us = 0;
    int64_t core_last_us = 0;
    // Where the recorded content starts, as against where the AXIS starts. Separate from
    // core_first_us on purpose: core_first_us anchors the axis every other module already uses, and
    // moving it would renumber every seek in the tree. This one is published on its own accessor.
    int64_t core_data_first_us = 0;
    // The other end of the same span: the last recorded tick, which is not the last chapter's end.
    int64_t core_data_last_us = 0;
    // "ticks" or "chapters" -- which measurement answered the span, for the load's own log line.
    const char* span_source = "none";
    // What every driven netvar key opens this file on, read once at load. See NetVarPrimer.
    NetVarPrimer netvar_primer;
    // The cadence MeasureCadence read off this file at Load, kept so TickSpacingUs can publish it.
    // ONE holder: the number that sized the window and the number a seek landing is judged against
    // are the same measurement, not two derivations of it. 0 means the file carries no measurable
    // cadence, which is a refusal to answer and never a spacing of zero.
    int64_t tick_spacing_us = 0;

    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_last{};
    bool qpc_started = false;

    // ---- DESIGN-UI-2 S5.3, the drag. One pending target, overwritten in place: older targets are
    // discarded, never queued, because replaying every intermediate position IS the lag. Times are
    // ABSOLUTE source time, like everything else below the interface.
    struct ScrubState {
        bool     active = false;
        bool     light = true;              // playback.scrub_light, latched per scrub
        bool     resume_on_release = false; // the pre-drag run state
        float    resume_rate = 1.0f;        // the pre-drag signed rate
        uint32_t load_serial = 0;           // the file the drag started on
        int64_t  pending_us = 0;
        int64_t  applied_us = 0;
        bool     pending_dirty = false;     // a target is waiting for the next tick
        uint32_t skipped = 0;               // targets coalesced away since BeginScrub
        uint64_t pending_at_us = 0;         // QPC of the ScrubTo that wrote pending_us
        uint32_t lines = 0;                 // step lines emitted this drag (rate limit)
        uint32_t lines_dropped = 0;
        uint32_t steps = 0;
        // ★ THE ONLY WITNESS THIS LOG HAS FOR A BODY. `read=`/`read_next=` on the pawn line come
        // out of the history ring we just installed, so a frozen body and a moving one are
        // byte-identical there (DIAG-SCRUB-BODIES S1). A body re-latches on a BEAT; these are the
        // clock's own counts of the beats this drag earned.
        uint64_t beats_at_begin = 0;
        uint64_t refused_at_begin = 0;
        uint64_t beats_at_line = 0;
        // Re-binds this drag made without saying so. A drag that crossed a chapter and printed
        // binds=0 is the G3 defect back.
        uint32_t light_binds = 0;
    };
    // ★ THE EVIDENCE LINE IS THE TICK'S, NOT THE SEEK'S. RebuildWindow runs at two sites, so a
    // line written from SeekAbsolute would print rebuilt=0 in the very frame the tick rebuilt --
    // and AC-1.3/AC-1.6 would report PASS over an unremoved stall. This record is filled as the
    // tick walks past both of them and emitted on the way out, including out of an early return.
    struct ScrubStepRecord {
        bool     armed = false;
        int64_t  target_us = 0;         // playback-relative, as the log prints it
        bool     covered = false;       // the resident span already held it
        uint64_t seek_builds = 0;
        uint64_t tick_builds = 0;
        uint32_t skipped = 0;
        bool     refused = false;
        double   build_ms = 0.0;
        double   apply_ms = 0.0;
        uint64_t issued_at_us = 0;      // WallNowUs of the ScrubTo that handed this target in
    };
    static constexpr uint32_t kScrubStepLinesMax = 600;   // ~20 s of dragging at 30 fps
    ScrubState scrub;
    ScrubStepRecord scrub_step;
    // Sticky across the abort a refusal causes, cleared by the next BeginScrub: the bar needs
    // to know the playhead did not move even though the drag is already over.
    bool scrub_last_refused = false;
    // S5.3.1: the ONE owner of the drag's audio hold. A refcount does not fail loudly, so an
    // unbalanced take leaves audio dead for the session with nothing reporting it.
    ScrubMuteHold scrub_mute;
    // Every entry into BuildWindowRange, refused builds included -- `rebuilds` counts successes
    // only, and a refusal costs the same decode the evidence line exists to see.
    uint64_t window_builds = 0;
    uint64_t recentres = 0;

    ApplyStats last_apply;
    uint64_t frames = 0;
    uint64_t rebuilds = 0;
    uint64_t apply_failures = 0;
    uint64_t rebinds = 0;
    bool entity_available = false;
    IEntity::BindEpoch entity_bind_state = IEntity::BindEpoch::NotAvailable;
    uint64_t entity_bind_epoch = 0;
    // PERF-3: what the bound netvar route was last re-validated against, and when it is due again.
    uint64_t netvar_revalidate_key = ~0ull;
    uint64_t netvar_revalidate_tick = 0;
    uint64_t owner_resets = 0;
    // The last answer IEntity::LocalUserIndex gave, and how many ticks the guard has been unable to
    // arm its is-local-user arm since the last reset. Held because the reason is the actionable half
    // -- `ambiguous` on a bot host is a different problem from `none` one tick after a bind.
    IEntity::LocalUser local_user_state = IEntity::LocalUser::NotAvailable;
    uint64_t local_unknown_ticks = 0;

    // The audio residue rows arm when a demo loads, which is BEFORE the game has created
    // CSoundPlayer on every host measured so far, and the audio module resolves it late. So
    // whether those rows can reach an audio state is a fact that changes mid-session, and a stop
    // line that did not carry it made a session whose seek reconcile never worked read exactly
    // like one where it did.
    uint64_t ticks = 0;
    bool audio_residue_bound = false;
    uint64_t audio_residue_bound_tick = 0;

    IdentityStatus identity;
    // The dress retry's own clock, in GetTickCount64 milliseconds. `dress_defer_since_ms` is when
    // the CURRENT continuous run of deferral began and is zero when nothing is deferring, so a
    // roster that admits one player at a time restarts the bound instead of spending it.
    uint64_t dress_next_ms = 0;
    uint64_t dress_defer_since_ms = 0;
    uint64_t dress_retry_ms = 250;
    uint64_t dress_bound_ms = 30000;
    // playback.dress / playback.dress_loadout, read once per LOAD like the two timeouts above: a
    // config lookup is a string walk and this pass runs a few times a second.
    bool dress_enabled = true;
    bool dress_loadout = true;
    // ★ THE BOUND IS MEASURED ON A CLOCK THAT ONLY RUNS WHILE THE ENGINE CAN MAKE PROGRESS.  The
    // admission is published by the engine's own NETGAME.UPDATE drain, and the plugin masters that
    // clock: while the session gate is shut -- level loading, containment not locked yet, no
    // netgame -- or while playback is paused, nothing can advance and a wall-clock bound would give
    // up on a dress that was never given a chance.  A viewer who loads a file and walks away must
    // find his players dressed when he presses play, not a give-up line from thirty seconds in.
    uint64_t dress_clock_ms = 0;        // accumulated progress time
    uint64_t dress_clock_last_ms = 0;   // wall time the last progressing pass was seen at
    // The containment/session deferral's own bound: one Warn then silence is how the last one
    // survived a nine-minute session.
    uint64_t shut_since_ms = 0;
    bool     shut_escalated = false;
    PresentSnapshot present;
    // Capture slot -> live engine user index, for the roster currently loaded. Held here rather
    // than inside the Window because a seek rebuilds the Window and the mapping outlives it: the
    // engine's entrants do not change when the playhead does.
    identity::RosterMapping roster_map;

    // What this module has DRIVEN on other packets' surfaces. Counted per call rather than read
    // back as a state, because a state a seam sets can also be reached without the seam.
    uint64_t clock_steps_pushed = 0;
    uint64_t owner_releases = 0;
    uint64_t history_trims = 0;
    uint64_t audio_pins_pushed = 0;
    uint64_t audio_pins_absent = 0;
    uint64_t audio_pins_refused = 0;
    uint64_t lod_throttle_writes = 0;
    bool lod_throttle_suppressed = false;
    bool lod_pin_refused = false;

    // S12.5.1's scripted camera track. `oracle_holder` is the control the bracket was OPENED on and
    // is what closes it: camera_control::Get() answers null between a Release() and the next
    // Create(), and a bracket that could not be closed because the accessor went quiet for one
    // frame would leave SetMode refused for the rest of the session.
    bool                  oracle_asked = false;
    bool                  oracle_active = false;
    IReplayCameraControl* oracle_holder = nullptr;
    uint64_t              oracle_begins = 0;
    uint64_t              oracle_ends = 0;
    uint64_t              oracle_poses = 0;
    const char*           oracle_refusal = "";
    IReplayCameraControl* camera_control_override = nullptr;
    const IEntity* lod_throttle_target = nullptr;

    // What Load() found and what it means. Held so ActiveModeLabel can say it rather than only the log.
    bool build_mismatch = false;
    bool content_mismatch = false;
    bool build_checked = false;
    char refusal[192] = {};
    char label[384] = {};  // facts-ok: IDENT.USER_ACTOR_GUID_OFF a local buffer size, not the engine offset
};

Global g;

// ★★ THE ONE PLACE THE CURRENT WINDOW IS NAMED. Every reader goes through here, so an adoption is
// one assignment to `g.cur` and not a search for borrowers -- and the compiler, not a reviewer, is
// what found the fifty-one call sites when the member stopped being a Window by value.
//
// ★ WHAT A CALLER MAY NOT DO WITH IT: keep the reference, or anything pointing into it, across the
// window step of the tick. Exactly one thing in the tree ever did -- Transport's TickTable, which
// borrows a raw pointer into the window's tick storage -- and AdoptWindowTail re-points it on every
// change, synchronous or adopted, for the same reason it always did.
Window& Win() { return *g.cur; }

// The free list. Refusing to push is not an error to swallow: it would mean a fourth buffer exists,
// which is a bookkeeping bug and not a memory pressure, so it is asserted by the tests rather than
// handled here.
bool PushFreeWindow(Window* w) {
    if (w == nullptr) {
        return true;
    }
    for (Window*& slot : g.free_win) {
        if (slot == nullptr) {
            slot = w;
            return true;
        }
    }
    return false;
}

Window* PopFreeWindow() {
    for (Window*& slot : g.free_win) {
        if (slot != nullptr) {
            Window* w = slot;
            slot = nullptr;
            return w;
        }
    }
    return nullptr;
}

// The file's own origin, the one answer. ReplayTransportImpl::Origin() forwards to this rather
// than recomputing it -- INV-AXIS is about there being exactly one conversion, not about where it
// is spelled, and the log helpers above the class need it too.
int64_t SourceOriginUs() {
    return g.loaded ? (int64_t)g.reader.GetManifest().t0Us : 0;
}

// A monotonic wall stamp, for measuring how long something in this module took. Separate from
// WallDeltaUs because that one CONSUMES the delta -- calling it to time a span would eat the
// frame's own tick delta.
uint64_t WallNowUs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g.qpc_freq.QuadPart <= 0) {
        return 0;
    }
    const long long whole = now.QuadPart / g.qpc_freq.QuadPart;
    const long long rem = now.QuadPart % g.qpc_freq.QuadPart;
    return (uint64_t)whole * kUsPerSecond +
           (uint64_t)((rem * (int64_t)kUsPerSecond) / g.qpc_freq.QuadPart);
}

// ---------------------------------------------------------------------------------------------
// The wall clock. Read ourselves, never from the sim clock we are about to drive (S7.3.1).
uint64_t WallDeltaUs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g.qpc_started) {
        g.qpc_last = now;
        g.qpc_started = true;
        return 0;
    }
    const long long ticks = now.QuadPart - g.qpc_last.QuadPart;
    g.qpc_last = now;
    if (ticks <= 0 || g.qpc_freq.QuadPart <= 0) {
        return 0;
    }
    // 64-bit safe: multiply before divide would overflow on a long stall, so scale in two steps.
    const long long whole = ticks / g.qpc_freq.QuadPart;
    const long long rem = ticks % g.qpc_freq.QuadPart;
    return (uint64_t)whole * kUsPerSecond +
           (uint64_t)((rem * (int64_t)kUsPerSecond) / g.qpc_freq.QuadPart);
}


// The engine's kInvalid as INV-OWN1 reads it. Named rather than spelled at the call sites so the
// two arming paths cannot drift, and so the one place it would have to change is one place.
const uint32_t kEngineInvalidUserId = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

const char* LocalUserText(IEntity::LocalUser lu) {
    switch (lu) {
        case IEntity::LocalUser::NotAvailable: return "the entity surface has no broadcaster pool";
        case IEntity::LocalUser::None:         return "this machine authors nobody";
        case IEntity::LocalUser::Ambiguous:    return "this machine authors more than one";
        case IEntity::LocalUser::Ok:           return "ok";
    }
    return "?";
}

// The same four states in one word, for the status line the operator reads. Separate from the
// sentences above because a panel has 80 columns and because `ambiguous` is the word AUDIT_WIRING_C
// C2 asks the sanctioned -offline host to be checked for: it is the state a bot-authoring host is
// expected to sit in, and it must be legible without opening the log.
const char* LocalUserWord(IEntity::LocalUser lu) {
    switch (lu) {
        case IEntity::LocalUser::NotAvailable: return "not-available";
        case IEntity::LocalUser::None:         return "none";
        case IEntity::LocalUser::Ambiguous:    return "ambiguous";
        case IEntity::LocalUser::Ok:           return "ok";
    }
    return "?";
}

IEntity::LocalUser AskLocalUser(UserIndex& out) {
    IEntity* const ent = g.svc ? g.svc->entity : nullptr;
    return ent ? ent->LocalUserIndex(out) : IEntity::LocalUser::NotAvailable;
}

// ★ INV-OWN1's local id comes from the engine, every time the binding changes, and from nowhere
// else. It used to be the literal 0: OwnerGuard::Classify refuses `id == local_`, so a genuine
// engine UserIndex of 0 was refused as is-local-user and that player's body never published
// (AUDIT_WIRING_A OPEN-1). Only LocalUser::Ok yields a number -- the other three are "no source",
// and the guard is told that rather than handed a fabricated one (S9.2).
void ArmOwnerGuard() {
    UserIndex local(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const IEntity::LocalUser lu = AskLocalUser(local);
    ++g.owner_resets;
    g.local_user_state = lu;
    g.local_unknown_ticks = 0;
    if (lu == IEntity::LocalUser::Ok) {
        g.owners.Reset(local.v, kEngineInvalidUserId);
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "INV-OWN1 armed against this session's own local user index %u",
                     (unsigned)local.v);
        return;
    }
    g.owners.ResetLocalUnknown(kEngineInvalidUserId);
    g.log->Write(LogLevel::Warn, kReplayModuleName,
                 "INV-OWN1 armed with NO local user index (%s): the is-local-user arm refuses "
                 "nobody until one exists; every other arm still runs. A replay body published onto "
                 "this machine's own entrant would not be caught here. The tick asks again every "
                 "frame and re-arms on the first answer",
                 LocalUserText(lu));
}

// ★ THE SEAM. W2-C resolved the capture's user slots to this session's user indices; this is where
// that answer reaches the value INV-OWN1 validates and the pose publisher owns by.
//
// It runs after dressing AND after every window build, because a seek builds fresh PlayerTracks and
// a track constructed with user_id_valid == false is a player that does not move. Registering with
// OwnerGuard here, rather than at publish time, keeps a bad allocation a load-time event instead of
// a per-frame one.
// The stat feed's mapping, taken from the STAT STORE's own slot list rather than from the window's
// player tracks: a recorded slot whose rows the file carries is owed its numbers whether or not the
// three resident chapters happen to hold a pose for it.
size_t CollectStatMapping(statsfeed::MappedPlayer* out, size_t cap) {
    const statsfeed::RecordedStats& src = statsfeed::PlaybackRecordedStats();
    size_t n = 0;
    for (uint16_t i = 0; i < src.PlayerSlotCount() && n < cap; ++i) {
        const uint16_t slot = src.PlayerSlotAt(i);
        UserIndex index(0);
        if (!g.roster_map.Lookup(slot, index)) continue;
        out[n].rec_slot = slot;
        out[n].live_index = index.v;
        ++n;
    }
    return n;
}

void ApplyRosterMapping() {
    g.identity.mapped = 0;
    g.identity.owner_refused = 0;
    // The player HANDLE resolves from the same number: IEntity::PlayerOf takes a UserIndex, which
    // is why player tracks stayed unhandled while body tracks bound -- bodies key on a durable net
    // id and had a resolver, players key on a live user index and until now nothing produced one.
    IEntity* const ent = g.svc->entity && g.svc->entity->Available() ? g.svc->entity : nullptr;
    for (size_t i = 0; i < Win().PlayerCount(); ++i) {
        PlayerTrack& tr = Win().PlayerAt(i);
        tr.user_id_valid = false;
        tr.handle_valid = false;
        UserIndex index(0);
        // The roster map is keyed by RECORDED user slot, and so is the track -- for wire rows too,
        // whose slot comes off the message header rather than the stream's dense index.
        if (!g.roster_map.Lookup(tr.user_slot, index)) {
            continue;
        }
        const OwnerVerdict v = g.owners.RegisterEntrant(index.v);
        if (v != OwnerVerdict::Ok) {
            ++g.identity.owner_refused;
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "recorded slot %u resolved to user index %u, REFUSED by INV-OWN1: %s%s",
                         (unsigned)tr.user_slot, (unsigned)index.v, OwnerVerdictName(v),
                         v == OwnerVerdict::IsLocalUser
                             ? "; that IS this session's own local user index, read from the engine"
                             : "");
            continue;
        }
        tr.user_id = index.v;
        tr.user_id_valid = true;
        ++g.identity.mapped;
        if (ent) {
            const PlayerHandle h = ent->PlayerOf(index);
            // 0xFFFF is the contract's "this session does not carry that one", the same sentinel
            // BindBodyHandles honours; using it as a handle indexes past the end of the pool.
            if (h.v != 0xFFFFu) {  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
                tr.handle = h;
                tr.handle_valid = true;
            }
        }
    }
}

// ★ ArmOwnerGuard is reached from Load and from the rebind edge, and the window
// in which the engine cannot answer is an ORDINARY part of a session, not a corner: Entity's
// Available() is `resolve_ == Ok`, which is decided by the physics CS, the remote-player CS and the
// pool -- the broadcaster pool is not in that chain and resolves separately -- so a bound, readable
// space whose local player is not yet seated answers LocalUser::None. A Load in that window used to
// arm local-unknown for the whole session: the space never changes again, so neither the epoch nor
// availability moves and nothing asked a third time, and INV-OWN1's primary arm stayed off.
//
// So the tick asks again, but only while the answer is missing. The ask is a count read and one
// element read behind a validated pointer; in the ordinary case (armed at Load) this function
// returns on its first line.
void RetryArmOwnerGuard() {
    if (g.owners.LocalUserKnown()) {
        return;
    }
    ++g.local_unknown_ticks;
    UserIndex local(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    const IEntity::LocalUser lu = AskLocalUser(local);
    g.local_user_state = lu;
    if (lu != IEntity::LocalUser::Ok) {
        return;
    }
    g.owners.Reset(local.v, kEngineInvalidUserId);
    ++g.owner_resets;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "INV-OWN1 armed against local index %u after %llu ticks",
                 (unsigned)local.v, (unsigned long long)g.local_unknown_ticks);
    // ★ RE-ARMED, AND THE ROSTER RE-VALIDATED. Every entrant admitted while the arm was off was
    // admitted without it, and Reset() has just cleared the registry. Re-running the mapping puts
    // each capture slot back through RegisterEntrant against the REAL index, so a replay player
    // that resolves to this machine's own entrant is refused at this moment -- counted in
    // owner_refused, named in the Error line, and its track left user_id_valid == false so it
    // cannot publish -- instead of being kept because it happened to be admitted one tick before
    // the engine could answer. Recording the id without re-validating would leave exactly the body
    // INV-OWN1 exists to stop.
    ApplyRosterMapping();
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "INV-OWN1 re-validated the loaded roster against local index %u: %u/%u mapped, "
                 "%u refused",
                 (unsigned)local.v, g.identity.mapped, g.identity.players,
                 g.identity.owner_refused);
}

// The end of the last CORE chapter, over the whole chapter table. Chapters of other sections sit
// between the core ones, so the last chapter is not necessarily a core chapter.
// Where the recorded content BEGINS, which is not where the axis begins: t0 is the manifest's
// declared origin and a capture can carry hundreds of seconds of nothing before its first core
// chapter. `first_us` is the fallback, so a file with no core chapter reads as a zero-length span
// rather than as a span running backwards.
int64_t CoreBeginUs(DemoReader& r, int64_t first_us) {
    bool found = false;
    int64_t begin = first_us;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section != kSectionCore || k.tEndUs <= k.tBeginUs) continue;
        if (!found || (int64_t)k.tBeginUs < begin) {
            begin = (int64_t)k.tBeginUs;
            found = true;
        }
    }
    return begin < first_us ? first_us : begin;
}

int64_t CoreEndUs(DemoReader& r, int64_t first_us) {
    int64_t end = first_us;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section == kSectionCore && (int64_t)k.tEndUs > end) {
            end = (int64_t)k.tEndUs;
        }
    }
    return end;
}

// ★★★ THE WINDOW IS SIZED BY THE FILE, AT LOAD.
//
// The pool used to be a constant, chosen against one-second chapters that no capture has. A chapter
// too deep for it makes BuildWindow refuse the whole range, so the number silently decided which
// parts of a match were reachable -- and it decided it at the SEEK, where the only thing the owner
// sees is a jump that did not happen. Both terms come from the capture itself: the widest CORE
// chapter span in the index, and the tick period measured off the first chapter that carries ticks.
//
// `spanOut`/`periodOut` are written whenever they could be answered, so the caller's refusal
// sentence can name what it did read. False means the file has CORE chapters and no measurable
// cadence -- not a default, a refusal.
constexpr size_t kSizingProbeChapters = 8;

bool MeasureCadence(DemoReader& r, uint64_t& spanOut, uint64_t& periodOut) {
    spanOut = 0;
    periodOut = 0;
    size_t coreChapters = 0;
    for (size_t i = 0; i < r.ChapterCount(); ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section != kSectionCore || k.tEndUs <= k.tBeginUs) {
            continue;
        }
        ++coreChapters;
        const uint64_t span = k.tEndUs - k.tBeginUs;
        if (span > spanOut) {
            spanOut = span;
        }
    }
    if (coreChapters == 0) {
        return true;   // nothing to build a window over; the default pools are never asked for
    }

    size_t probed = 0;
    for (size_t i = 0; i < r.ChapterCount() && probed < kSizingProbeChapters; ++i) {
        const ChapterKey k = r.ChapterAt(i);
        if (k.section != kSectionCore || k.tEndUs <= k.tBeginUs) {
            continue;
        }
        ++probed;
        DecodedChapter dc;
        if (!r.DecodeChapter(i, dc)) {
            continue;
        }
        const StreamView* tv = dc.Find(kStreamTick);
        if (tv == nullptr || tv->recordCount == 0) {
            continue;
        }
        // The MEAN spacing across the chapter, floored -- floored, so an uneven cadence rounds
        // towards MORE ticks per chapter and the pool is sized over rather than under. A one-tick
        // chapter yields the chapter span itself, which is the widest honest period it can claim.
        const uint64_t period = (k.tEndUs - k.tBeginUs) / tv->recordCount;
        periodOut = period > 0 ? period : 1;
        return true;
    }
    return false;
}

// ★★★ ONE BIND SITE, TWO EDGES, EITHER ORDER. Body handles need a live AVAILABLE entity surface
// AND a valid window, and those two arrive in whichever order the session gives them: loading into a
// session that has already bound its game space spends the entity edge before a window exists, and
// loading ahead of the space gives a valid window before the entity resolves. Binding on one edge
// only leaves the other order permanently unbound. It also has to SAY so on both paths -- the
// rebuild path used to bind silently, so "were the handles bound?" was unanswerable on exactly the
// order a real session takes.
// The provisioner's mapping, snapshotted. Entries it has not assigned say so (kSlotUnassigned):
// the resolver reads them as identity, and the bind refuses a wire body on one rather than aim
// recorded slot 0 at the viewer's own. Returns true when the map CHANGED, because that is the
// moment a bind taken earlier asked the wrong slot and owes the session a re-bind.
//
// Deliberately NOT gated on `provisioned`. The mapping is final at ASSIGNMENT: AssignMappedSlot
// proved the live slot free and the provisioner reuses it for the life of the load. `provisioned`
// reports whether the ENTRANT has arrived, a later question the bind answers in its own words
// (`no player ci` vs `no phys ci`). L3 #27 settled its only slot on the last line it ever wrote;
// a map gated on that would have asked the viewer's slot for the whole session.
bool RefreshSlotMap() {
    uint16_t next[PawnResolver::kSlots];
    for (uint16_t i = 0; i < PawnResolver::kSlots; ++i) next[i] = PawnResolver::kSlotUnassigned;
#ifndef W2P1_MUTATE_SLOTMAP_UNWIRED
    const PawnLedger led = g.pawn.Ledger();
    const uint32_t entries =
        led.mapped < PawnResolver::kSlots ? led.mapped : (uint32_t)PawnResolver::kSlots;
    for (uint32_t i = 0; i < entries; ++i) {
        const uint16_t recorded = led.map[i].slot_recorded;
        const uint16_t live     = led.map[i].slot_resolved;
        // kSlotUnmapped at either end is a row the provisioner never filled, not slot 65535.
        if (recorded < PawnResolver::kSlots && live < PawnResolver::kSlots) next[recorded] = live;
    }
#endif
    if (g.slot_map_n == PawnResolver::kSlots && memcmp(g.slot_map, next, sizeof next) == 0) {
        return false;
    }
    memcpy(g.slot_map, next, sizeof next);
    g.slot_map_n = PawnResolver::kSlots;
    return true;
}

// The window's one claimed track, or null. Found once per bind; never searched for per frame.
BodyTrack* DiscTrack() {
    if (g.disc_track >= Win().BodyCount()) return nullptr;
    BodyTrack& tr = Win().BodyAt(g.disc_track);
    return tr.claim_owner ? &tr : nullptr;
}

// A bind walks the window and the answer does not move between frames, so the unbound re-attempt
// is rate-limited rather than run per tick.
constexpr uint64_t kDiscRetryPeriodUs = 500000;

// The disc alone. The level game space the frisbee lives in is created EMPTY at map load and filled
// later -- the first launch measured it at 0 systems while the persistent space held 99 -- so a bind
// taken at load time can only miss, and this is what closes that gap without disturbing the player
// binds. It also runs after every drop, so a round reset re-binds rather than ending the drive.
// Whether this answer has had a line of its own since the file was loaded. A verdict already
// printed at the same bound id is a repeat however many times the chain has changed its mind in
// between, and the repeats are counted onto the next line that does print.
bool DiscVerdictWorthALine(BindResult::DiscVerdict v, uint32_t id) {
    static_assert((unsigned)BindResult::DiscVerdict::HandleInUse < 32u,
                  "the printed-verdict set is a 32-bit mask");
    const uint32_t bit = 1u << (unsigned)v;
    if ((g.disc_logged_mask & bit) != 0 && id == g.disc_logged_id) {
        ++g.disc_verdict_repeats;
        return false;
    }
    g.disc_logged_mask |= bit;
    g.disc_logged_verdict = v;
    g.disc_logged_id = id;
    return true;
}

// The early path's own line. `disc bind %s` is formatted by windowbuild out of BindResult, which
// says nothing about WHICH chain answered, and that is the one thing a launch log needs to settle
// the 100 s question. One line per distinct hop, by pointer: the refusals are literals, the path
// is re-asked every 500 ms while it refuses, and a line per attempt is 200 lines of pre_match.
// The refusal names the hop; this names what each candidate space actually held, so a wrong space
// or a wrong actor symbol is visible without a second launch. It rides on the refusal line, which
// prints once per distinct hop -- never once per 500 ms retry.
using EarlyDiag = replay::IEntity::DiscBind::EarlyDiag;

// EMPTY is not UNREADABLE. A level space is created empty at `[NETGAME] Loading map` and filled by
// streaming seconds later, and one word for both is what made the first three launches unable to
// say whether it ever fills.
const char* RegWord(EarlyDiag::Reg r) {
    switch (r) {
        case EarlyDiag::Reg::NoReg:      return "no-registry";
        case EarlyDiag::Reg::Unreadable: return "unreadable";
        case EarlyDiag::Reg::Empty:      return "empty";
        case EarlyDiag::Reg::Populated:  return "";   // the count says it
        case EarlyDiag::Reg::Unseen:     break;
    }
    return "-";
}

void Mix(uint64_t& h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;   // FNV-1a, over the fields a launch has to watch move
}

// Everything the candidate lines print, in one comparable word. What the DIAGNOSTIC says is the
// key, so a poll whose answer changed under an unchanged refusal literal still prints.
uint64_t DiscEarlyDigest(const EarlyDiag& d) {
    uint64_t h = 1469598103934665603ull;
    Mix(h, uint64_t(d.n));
    for (size_t i = 0; i < d.n && i < EarlyDiag::kCandidates; ++i) {
        const EarlyDiag::Cand& c = d.cand[i];
        Mix(h, uint64_t(reinterpret_cast<uintptr_t>(c.root)));
        Mix(h, uint64_t(int64_t(c.aliasOf)));
        Mix(h, uint64_t(c.depth));
        Mix(h, uint64_t(c.symbolMatched) | (uint64_t(c.slotLive) << 1) |
                   (uint64_t(c.registryRead) << 2));
        for (size_t dep = 0; dep < EarlyDiag::kDepths; ++dep) {
            Mix(h, c.countAt[dep]);
            Mix(h, uint64_t(c.regAt[dep]));
        }
    }
    return h;
}

// One line per candidate rather than one for all three: eight depths of `dN=` per candidate is more
// than the log's 512-char line holds, and a truncated tail is exactly where the parent spaces are.
void LogDiscEarlyCandidates(const replay::IEntity& e, bool force) {
    const EarlyDiag* const d = e.DiscEarlyDiagnostics();
    if (!d || d->n == 0) return;

    const uint64_t digest = DiscEarlyDigest(*d);
    if (!g.disc_early_digest_valid || digest != g.disc_early_digest) {
        g.disc_early_digest = digest;
        g.disc_early_digest_valid = true;
        g.disc_early_change_pending = true;
    }
    if (!g.disc_early_change_pending && !force) return;
    // At 2 Hz a state that flickers would still be two lines a second. One set per second, and the
    // swallowed changes are counted onto the set that does print so none of them goes unrecorded.
    if (!force && g.disc_early_quiet_us < kUsPerSecond) {
        ++g.disc_early_changes_held;
        return;
    }
    const uint32_t held = g.disc_early_changes_held;
    g.disc_early_changes_held = 0;
    g.disc_early_change_pending = false;
    g.disc_early_quiet_us = 0;

    for (size_t i = 0; i < d->n && i < EarlyDiag::kCandidates; ++i) {
        const EarlyDiag::Cand& c = d->cand[i];
        const char* const name = c.name ? c.name : "?";
        char held_note[48];
        held_note[0] = '\0';
        if (held != 0 && i == 0) {
            _snprintf_s(held_note, sizeof held_note, _TRUNCATE,
                        " (+%u change(s) not printed)", held);
        }
        if (!c.root) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "disc bind early path candidate [%u] %s=null%s", unsigned(i), name,
                         held_note);
            continue;
        }
        if (c.aliasOf >= 0) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "disc bind early path candidate [%u] %s=%p same as [%d]%s", unsigned(i),
                         name, c.root, int(c.aliasOf), held_note);
            continue;
        }
        // The parent walk, depth by depth: `d0=empty d1=99` is the difference between "the space is
        // wrong" and "the space has not been streamed into yet".
        char depths[300];  // facts-ok: PHYS.CI_EXTRAP_CAP_S_OFF plain 300 as a count, size or pixel value, not the extrapolation-cap offset
        depths[0] = '\0';
        size_t at = 0;
        for (uint32_t dep = 0; dep < c.depth && dep < EarlyDiag::kDepths && at + 1 < sizeof depths;
             ++dep) {
            const EarlyDiag::Reg r = c.regAt[dep];
            char what[24];
            if (r == EarlyDiag::Reg::Populated) {
                _snprintf_s(what, sizeof what, _TRUNCATE, "%llu",
                            (unsigned long long)c.countAt[dep]);
            } else {
                _snprintf_s(what, sizeof what, _TRUNCATE, "%s", RegWord(r));
            }
            const int w = _snprintf_s(depths + at, sizeof depths - at, _TRUNCATE, "%sd%u=%s",
                                      dep != 0 ? " " : "", dep, what);
            if (w < 0) break;
            at += size_t(w);
        }
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc bind early path candidate [%u] %s=%p depth=%u %s sym=%s live=%s%s",
                     unsigned(i), name, c.root, c.depth, depths,
                     c.symbolMatched ? "found" : "no", c.slotLive ? "yes" : "no", held_note);
    }

    // The registry head, for the case where the space is right and the actor carries a symbol we
    // do not know. Once per load, eight to a line so the 512-char limit is never the thing that
    // decides what a launch can settle.
    if (d->sampleN == 0 || g.disc_early_symbols_logged) return;
    g.disc_early_symbols_logged = true;
    for (size_t base = 0; base < d->sampleN; base += 8) {
        char syms[320];  // facts-ok: ENTITY.REMOTEPLAYER_CS_NCATRANSFORM_OFF a local buffer size, not the engine offset
        syms[0] = '\0';
        size_t s = 0;
        size_t i = base;
        for (; i < d->sampleN && i < base + 8 && s + 1 < sizeof syms; ++i) {
            const int w = _snprintf_s(syms + s, sizeof syms - s, _TRUNCATE, "%s%016llX",
                                      i != base ? " " : "",
                                      static_cast<unsigned long long>(d->sample[i]));
            if (w < 0) break;
            s += size_t(w);
        }
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc bind early path actor symbols in %p [%u..%u of %u]: %s", d->sampleFrom,
                     unsigned(base), unsigned(i - 1), unsigned(d->sampleN), syms);
    }
}

void LogDiscEarlyPath() {
    if (!g.log || !g.svc || !g.svc->entity) return;
    const IEntity& e = *g.svc->entity;
    if (e.DiscBoundEarly()) {
        if (g.disc_early_bound_logged) return;
        g.disc_early_bound_logged = true;
        g.disc_early_refusal_logged = nullptr;
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc bind early path: the engine has not resolved its frisbee ref yet; the "
                     "game_frisbee actor was resolved in candidate space %s instead",
                     e.DiscEarlySpaceName());
        // Which of the candidates answered is the one thing a successful launch still has to
        // settle, so the candidate line prints on the bind too, not only on a refusal.
        LogDiscEarlyCandidates(e, true);
        return;
    }
    const char* const why = e.DiscEarlyRefusal();
    if (why && why != g.disc_early_refusal_logged) {
        g.disc_early_refusal_logged = why;
        g.log->Write(LogLevel::Info, kReplayModuleName, "disc bind early path refused: %s", why);
    }
    // ★ AND NOT ONLY WHEN THAT LITERAL CHANGES. The refusal is the same string for every poll of a
    // space that is still streaming; what moves is the candidates' own answer.
    LogDiscEarlyCandidates(e, false);

    // And the hop the ENGINE's own chain is stopping at, whenever that answer changes. The
    // refusals are literals, so identity is the comparison and the memory is eight of them.
    const char* const hop = g.bind.disc_oracle_unreadable;
    if (hop == g.disc_rules_hop_last) return;
    if (g.disc_rules_hop_last) ++g.disc_rules_hop_changes;
    g.disc_rules_hop_last = hop;
    if (!hop) return;
    for (size_t i = 0; i < g.disc_rules_hops_n; ++i) {
        if (g.disc_rules_hops[i] == hop) return;
    }
    if (g.disc_rules_hops_n < Global::kDiscHopsRemembered)
        g.disc_rules_hops[g.disc_rules_hops_n++] = hop;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "disc bind rules ref stops at a new hop: %s (%u changes of hop so far)", hop,
                 g.disc_rules_hop_changes);
}

void RetryDiscBind() {
    if (!g.window_valid || !g.svc || !g.svc->entity || !g.svc->entity->Available()) return;
    void* const netGame = g.tick_net_game ? g.tick_net_game : LiveNetGame();
    const size_t before = g.bind.bound_disc;
    BindDiscTracks(*g.svc->entity, netGame, Win(), g.bind);
    g.bind.bound = g.bind.bound - before + g.bind.bound_disc;
    g.disc_track = Global::kNoDiscTrack;
    for (size_t i = 0; i < Win().BodyCount(); ++i) {
        if (Win().BodyAt(i).claim_owner) { g.disc_track = i; break; }
    }
    g.owner_releases += g.applier.ReconcileDiscLevel(Win());
    if (g.bind.bound_disc > 0 && before == 0) g.disc_probe_pending = true;
    LogDiscEarlyPath();
    // An unbound retry at 2 Hz that logged every attempt would bury the launch log in the one line
    // that says nothing happened.
    if (g.log) {
        char summary[kLogLineMax];
        if (FormatDiscBind(g.bind, summary, sizeof summary) &&
            DiscVerdictWorthALine(g.bind.disc_verdict, g.bind.disc_bound_id)) {
            const uint32_t repeats = g.disc_verdict_repeats;
            g.disc_verdict_repeats = 0;
            if (repeats == 0) {
                g.log->Write(LogLevel::Info, kReplayModuleName, "disc bind %s (retry)", summary);
            } else {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "disc bind %s (retry, and %u earlier changes of answer not printed)",
                             summary, repeats);
            }
        }
    }
}

// One compare per drive tick against the engine's own validity predicate. A round reset destroys
// the frisbee's physics component and the engine recycles the pool slot, after which the bound
// handle addresses a different body -- so the claim is dropped WITHOUT a write, because the owner
// dword we saved belongs to whatever now occupies that slot.
void MaintainDiscBind(uint64_t wall) {
    if (!g.disc_drive_on || !g.window_valid || !g.svc || !g.svc->entity) return;
    BodyTrack* disc = DiscTrack();
    if (disc && disc->handle_valid) {
        const char* reason = nullptr;
        if (!g.svc->entity->DiscStands(disc->disc_component_id)) {
            reason = "the engine no longer vouches for component";
        } else {
            // The bound pool is a pointer into the level game space and DiscStands reads it
            // without asking whether that space is still there. Re-walking the rules chain is what
            // answers that, and it costs syscalls -- so it runs HERE, at 2 Hz, never per tick.
            g.disc_fresh_accum_us += wall;
            if (g.disc_fresh_accum_us >= kDiscRetryPeriodUs) {
                g.disc_fresh_accum_us = 0;
                void* const netGame = g.tick_net_game ? g.tick_net_game : LiveNetGame();
                IEntity::DiscChainReading chain;
                if (!g.svc->entity->DiscChainStands(netGame, &chain)) {
                    // The host reaching round start is the expected case here, not a fault: the
                    // engine's ref resolves and, if it names another body, it wins.
                    reason = chain.refResolved
                                 ? "the engine's own frisbee ref now names a different disc than"
                                 : (chain.early ? "the level space no longer names the bound disc "
                                                  "pool for component"
                                                : "the rules chain no longer names the bound disc "
                                                  "pool for component");
                } else if (chain.early && chain.agreed && !g.disc_early_agree_logged) {
                    g.disc_early_agree_logged = true;
                    if (g.log)
                        g.log->Write(LogLevel::Info, kReplayModuleName,
                                     "disc bind early path agrees with the engine's frisbee ref "
                                     "(physCs=%p physId=0x%08X); the ref drives it from here",
                                     chain.refPhysCs, chain.refPhysId);
                }
            }
        }
        if (!reason) return;
        if (disc->synthetic_owner_set) g.svc->entity->ForgetSyntheticOwner(disc->handle);
        // The component is gone, so the held level goes with it WITHOUT a restore: its old owner
        // dword now belongs to whatever the engine recycled into that pool slot.
        g.applier.ForgetDiscLevel();
        g.svc->entity->DropDisc();
        disc->synthetic_owner_set = false;
        disc->owner_level_published = false;
        disc->handle_valid = false;
        disc->claim_owner = false;
        g.disc_track = Global::kNoDiscTrack;
        // `bound` counts what is bound NOW: leaving the disc in it inflates the count by one on
        // every drop/re-bind cycle, and that field is what an operator reads off the bind line.
        g.bind.bound = g.bind.bound > g.bind.bound_disc ? g.bind.bound - g.bind.bound_disc : 0;
        g.bind.bound_disc = 0;
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "disc bind dropped: %s 0x%08X; released without a write, re-binding",
                         reason, disc->disc_component_id);
        }
        g.disc_retry_accum_us = kDiscRetryPeriodUs;   // re-bind on the next tick
        return;
    }
    g.disc_fresh_accum_us = 0;
    g.disc_retry_accum_us += wall;
    if (g.disc_retry_accum_us < kDiscRetryPeriodUs) return;
    g.disc_retry_accum_us = 0;
    RetryDiscBind();
}

// ---- THE PRE-ROLL -----------------------------------------------------------------------------
// The offline host will not resolve its own frisbee ref until its pre_match script counts 45 s
// down, and that countdown is metered by the engine clock THIS module masters (three launches:
// play->bind 48.7 / 45.4 / 50.8 s, load->bind 552 / 119 / 51 s -- RE-PREMATCH-LEVERS §3b). So the
// playhead is pinned where the owner left it and the engine clock is run forward until the disc
// binds. Nothing is seeked and the first frame he watches is still the one he asked for.
constexpr double kHostPreMatchS = 45.0;
// ...and the disc binds at that round_start, but the host is then in PRE-LAUNCH: players in tubes,
// "LAUNCH" on screen, and the script parking the disc at the arena origin every frame. Waiting only
// for the bind hands the owner that fight (DIAG-HOST-ROUNDSTART §Q4: round_start -0s->
// team_room_wait -> transition_to_launch -5s-> pre_launch_enter -15s-> main_loop_enter). So the
// hold runs on to main_loop, where the host stops writing the disc and the drive has it alone.
constexpr double kHostRoundStartToPlayingS = 20.0;
constexpr double kHostToPlayingS = kHostPreMatchS + kHostRoundStartToPlayingS;   // 65 s of host time
// Admission first: a pre-roll over a half-populated host prepares the wrong session. Bounded, so a
// provisioner that never settles costs the wait and not the pre-roll.
constexpr uint64_t kPreRollAdmitGraceUs = 8000000;
// A sim-clock difference larger than this is not one frame of anything: the engine clamps each
// heartbeat to CLOCK.MAXELAPSED_US (66666 us) and there are three beats to a frame, so a whole
// second is already fifty times the largest frame it can produce. Rejected, never clamped -- a
// clamped sample would land in the timeline looking like a measurement.
constexpr uint64_t kPreRollSimStepMaxUs = 1000000;

// THE HOST'S PHASE, AS THE HOST PUBLISHES IT. The replay drives `game_state`, so the netvar store
// answers with the RECORDED phase -- reading it back would ask our own writes where the host is.
// HOST-NV sees the host's write before it swallows it, and that is the value used here.
struct HostPhase {
    bool             known = false;
    gamestate::Phase phase = gamestate::Phase::Unknown;
    const char*      name = "not published";
};

HostPhase HostPhaseNow() {
    HostPhase out;
    const netvarguard::HostSymbol seen = netvarguard::LastHostSymbol();
    if (seen.writes == 0) return out;   // never heard from: not "pre-match", not anything
    out.name = "unknown symbol";
    for (uint32_t i = 0; i < gamestate::facts::kPhaseFactCount; ++i) {
        const gamestate::facts::PhaseFact& row = gamestate::facts::kPhaseFacts[i];
        if (row.symbol != seen.value) continue;
        out.known = true;
        out.phase = row.phase;
        out.name = row.name;
        return out;
    }
    // The four the arena script owns and the exe has never heard of (gamestate_facts.h).
    // transition_to_launch_* is in here, and it is the phase that caused the alternation.
    for (uint32_t i = 0; i < gamestate::facts::kScriptPhaseFactCount; ++i) {
        const gamestate::facts::ScriptPhaseFact& row = gamestate::facts::kScriptPhaseFacts[i];
        if (row.symbol != seen.value) continue;
        out.known = true;
        out.phase = row.phase;
        out.name = row.name;
        return out;
    }
    return out;
}

const char* HostPhaseName() { return HostPhaseNow().name; }

// main_loop_enter / main_loop_update -- the host has launched and is playing.
bool HostPlaying() {
    const HostPhase now = HostPhaseNow();
    return now.known && now.phase == gamestate::Phase::Playing;
}

// ★ R1. The disc hold is a LEVEL, not a latch: the host leaves main_loop again on its own round
// flow (round_over -> sudden_death -> transition_to_launch -> pre_launch -> main_loop, every ~5 min
// of host playing time) and its script parks the frisbee every graph update in those phases. A
// latch that never handed the disc back brought the alternation straight back there.
// An unreadable or unnamed phase means DRIVE: the mute arm has to keep its old behaviour, or a
// build where HOST-NV did not install would never move the disc at all.
bool HostParksDisc() {
    const HostPhase now = HostPhaseNow();
    return now.known && now.phase != gamestate::Phase::Playing;
}

// Nothing the host wrote has ever been seen, so its phase cannot be waited on. True on a build
// where HOST-NV refused to install, and on any run before the script's first publish.
bool HostPhaseMute() { return netvarguard::LastHostSymbol().writes == 0; }

// ---- THE PRE-ROLL'S TWO SCALES ----------------------------------------------------------------
// ★ WHY THE PRE-MATCH RUNS FASTER THAN THE LAUNCH BEHIND IT. The 45 s pre-match is an empty
// countdown -- one script node subtracting `graph->deltatime` from a float whose default the node
// itself carries (`execute_node_5d5f35f3` @ 0x180017A20, a R15NetBalanceQueryExpression defaulting
// to 45.0). The 20 s behind it is not empty: it teleports eight bodies into tubes and launches
// them. So the countdown is run out at `playback.preroll_clock_scale` and the launch sequence at
// `playback.preroll_rate`, which is the only rate a launch has ever actually been watched at.
//
// ★ AND WHY RAISING THE SCALE IS SAFE. The engine clamps every heartbeat's scaled delta to
// CLOCK.MAXELAPSED_US -- 66666 us, read live -- so the largest step it will EVER integrate is
// fixed, and this module's own PlanTimescale caps the commanded rate at maxElapsed/framePeriod
// (clock_policy.h `Ceiling`). At the ~240 tick/s this host runs windowed, 4x is ~16 ms of sim per
// frame: exactly the step the engine takes on every frame of an ordinary 60 Hz session. Asking for
// more than the ceiling is not dangerous, it is discarded inside the cap -- which is what makes
// this key safe for an operator to raise from a launch.
constexpr float kPreRollScaleMin = 1.0f;
constexpr float kPreRollScaleMax = 8.0f;

// The host's launch sequence: everything from its round_start to main_loop. The bind IS the
// round_start (DIAG-HOST-ROUNDSTART §Q4), so either half answers on its own -- a host that has not
// published a phase cannot hide a launch that has already bound the disc, and a launch phase seen
// before the bind lands must not be run out at the pre-match scale either.
bool PreRollInLaunchSequence() {
    if (g.bind.bound_disc > 0) return true;
    const HostPhase now = HostPhaseNow();
    return now.known && now.phase != gamestate::Phase::PreMatch;
}

// What the whole hold is expected to cost in REAL seconds: the 45 s countdown at the pre-match
// scale plus the 20 s launch at the ramped one. Both divisors are clamped at load, so neither can
// be zero here.
double PreRollExpectedWallS() {
    return kHostPreMatchS / (double)g.preroll_scale +
           kHostRoundStartToPlayingS / (double)g.preroll_rate;
}

bool PreRollActive() { return g.preroll == Global::kPreRollRunning; }
bool PreRollPreparing() {
    return g.preroll == Global::kPreRollWaiting || g.preroll == Global::kPreRollRunning;
}

// The recorded players are in, or the provisioner has answered for all of them.
bool AdmissionSettled() {
    const PawnLedger led = g.pawn.Ledger();
    if (led.requested == 0) return true;   // nothing to admit: there is nothing to wait for
    return led.provisioned + led.failed >= led.requested;
}

// ★ THE TIMELINE, SO THE NEXT LAUNCH MEASURES ITSELF. Four lines, one per phase and a total, every
// one of them `preroll:` so a single grep answers "where did the wait go" without anybody having to
// difference log timestamps by hand. Wall is ours; ENGINE is the sim clock differenced across each
// tick, so the ratio is what the engine ACHIEVED and not what we asked it for -- the two differ
// whenever the frame rate puts the ceiling below the configured scale, and that difference is
// exactly what an operator raising the key needs to see.
void LogPreRollTimeline() {
    if (!g.log) return;
    const double fastW = double(g.preroll_fast_wall_us) / 1e6;
    const double fastE = double(g.preroll_fast_sim_us) / 1e6;
    const double rampW = double(g.preroll_ramp_wall_us) / 1e6;
    const double rampE = double(g.preroll_ramp_sim_us) / 1e6;
    // Derived, not accumulated: every tick's slice is charged to exactly one of the three, so the
    // remainder IS the wait and the three cannot fail to add up to the total.
    const uint64_t waitUs = g.preroll_wall_us > (g.preroll_fast_wall_us + g.preroll_ramp_wall_us)
                                ? g.preroll_wall_us - g.preroll_fast_wall_us - g.preroll_ramp_wall_us
                                : 0;
    const double waitW = double(waitUs) / 1e6;
    const double waitE = double(g.preroll_wait_sim_us) / 1e6;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "preroll: timeline | wait      %6.1f s wall, %6.1f s engine (%.2fx) -- level load, "
                 "admission and dressing, on the engine's own clock",
                 waitW, waitE, waitW > 0.0 ? waitE / waitW : 0.0);
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "preroll: timeline | pre-match  %6.1f s wall, %6.1f s engine (%.2fx achieved, "
                 "%.2fx asked -- playback.preroll_clock_scale)",
                 fastW, fastE, fastW > 0.0 ? fastE / fastW : 0.0, (double)g.preroll_scale);
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "preroll: timeline | launch     %6.1f s wall, %6.1f s engine (%.2fx achieved, "
                 "%.2fx asked -- playback.preroll_rate)",
                 rampW, rampE, rampW > 0.0 ? rampE / rampW : 0.0, (double)g.preroll_rate);
    // ★ THE REASON IS NOT REPEATED HERE. The `ended after ... -- <why>` line above carries it, and
    // a second line quoting the same words makes every "did this happen once" count answer twice.
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "preroll: timeline | TOTAL     %6.1f s wall, %6.1f s engine -- load to the first "
                 "frame shown",
                 double(g.preroll_wall_us) / 1e6, waitE + fastE + rampE);
}

// One exit, so "the engine clock goes back to mirroring the transport" is one statement and not
// five. Idempotent, and safe on a module that never started a pre-roll.
void EndPreRoll(const char* why, bool resume) {
    if (g.preroll == Global::kPreRollOver) return;
    // ★ EVERY ANNOUNCED PRE-ROLL GETS AN END LINE, not only one that reached the fast clock. A
    // reload binds the disc during the dress tick, so the hold ends while it is still WAITING --
    // which used to leave a `pre-roll ON` line with nothing after it and a third load's pre-roll
    // unaccounted for (LAUNCH-MULTILOAD §5.3).
    const bool ran = PreRollActive();
    const bool announced = PreRollPreparing();
    g.preroll = Global::kPreRollOver;
    // ★ ALWAYS, on every path including the refused ones: the clock stays ours until it is handed
    // back, and a pre-roll that ends without this leaves the host running while the viewer pauses.
    if (g.svc && g.svc->clock) g.svc->clock->SetPreRoll(false, 1.0f);
    if (announced && g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "pre-roll: ended after %.1f s of real time (%.1f s of it on the fast clock)%s "
                     "-- %s",
                     double(g.preroll_wall_us) / 1e6, double(g.preroll_run_us) / 1e6,
                     ran ? "" : ", never reaching the fast clock", why);
        LogPreRollTimeline();
    }
    const bool play = g.preroll_play_requested;
    const float rate = g.preroll_play_rate;
    g.preroll_play_requested = false;
    if (!resume || !play) return;
    // The Play pressed while it prepared. Honoured here rather than dropped, so the owner's one
    // press does what he meant by it.
    g.transport.SetRate((double)rate);
    g.transport.SetPaused(false);
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "pre-roll: the play pressed while it prepared starts now, at %.2fx",
                     (double)rate);
    }
}

// Every loaded tick, above every early return the window gates below take. `simNowUs` is the
// engine's own sim clock as the tick provider sampled it -- CClock+0x28, the counter every
// heartbeat accumulates -- and 0 means no sample, which is what a rig with no engine behind it
// hands in.
void UpdatePreRoll(uint64_t wall, uint64_t simNowUs) {
    if (g.preroll == Global::kPreRollOver) return;
    if (g.preroll == Global::kPreRollUnstarted) {
        // Decided once per load. `disc_drive_on` already carries ProcessContained(), so a
        // non-contained process never reaches the clock through here.
        if (!g.preroll_enabled || !g.disc_drive_on || !g.svc || !g.svc->clock) {
            g.preroll = Global::kPreRollOver;
            return;
        }
        // ★ A CAPTURE WITH NO RECORDED PLAYERS HAS NOTHING THE PRE-ROLL IS WAITING FOR. The wait
        // ends on a disc bind, and a disc is bound to a body somebody carries: a solo or lobby
        // session records neither, so the wait can only end on the wall bound -- 40 s of a frozen
        // playhead before a capture that was never going to need it (LAUNCH-STATS1:188-193).
        if (g.reader.GetManifest().playerCount == 0) {
            if (g.log) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "pre-roll skipped: this capture records no players, so there is no "
                             "roster to dress and no disc bind to wait for; playback starts now "
                             "instead of spending the %.0f s bound (playback.preroll_bound_ms)",
                             double(g.preroll_bound_us) / 1e6);
            }
            g.preroll = Global::kPreRollOver;
            return;
        }
        // ★ AND A CAPTURE WITH NO BODY TRACK HAS NO DISC TO BIND EITHER. The disc is the only
        // non-player body a capture carries (players ride S_PLAYER_*), so zero body records over
        // every core chapter means the wait can only end on the wall bound. A 1-player solo capture
        // records a roster and no disc, so the playerCount test above does not catch it and the
        // owner spends the full 40 s frozen (LAUNCH-MULTILOAD §3.3).
        if (g.track_scan.max_body_records == 0) {
            if (g.log) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "pre-roll skipped: this capture records no body track, so there is no "
                             "disc to bind and nothing the wait could end on; playback starts now "
                             "instead of spending the %.0f s bound (playback.preroll_bound_ms)",
                             double(g.preroll_bound_us) / 1e6);
            }
            g.preroll = Global::kPreRollOver;
            return;
        }
        g.preroll = Global::kPreRollWaiting;
    }
    g.preroll_wall_us += wall;
    // ★ THE SLICE THAT JUST ELAPSED IS CHARGED HERE, ABOVE EVERY EXIT, to the phase the module was
    // in DURING it. Booking it at the bottom instead would lose the last slice of every hold that
    // ends -- which is the one tick the disc bind lands in.
    {
        uint64_t simDelta = 0;
        if (simNowUs != 0 && g.preroll_prev_sim_us != 0 && simNowUs > g.preroll_prev_sim_us) {
            const uint64_t d = simNowUs - g.preroll_prev_sim_us;
            if (d <= kPreRollSimStepMaxUs) simDelta = d;
        }
        g.preroll_prev_sim_us = simNowUs;
        if (g.preroll != Global::kPreRollRunning) {
            g.preroll_wait_sim_us += simDelta;
        } else if (g.preroll_ramped) {
            g.preroll_ramp_wall_us += wall;
            g.preroll_ramp_sim_us += simDelta;
        } else {
            g.preroll_fast_wall_us += wall;
            g.preroll_fast_sim_us += simDelta;
        }
    }
    if (g.bind.bound_disc > 0) {
        // Both halves, or neither is worth anything: a bound disc the host is still parking at the
        // origin every frame is the alternation the owner watched.
        if (HostPlaying()) {
            EndPreRoll("the disc is bound and the host is playing", true);
            return;
        }
        if (HostPhaseMute()) {
            // The phase cannot be waited on, so waiting for it would only burn the bound. End on
            // the bind, as before -- and say why, because this is the arm that brings the flicker
            // back.
            if (g.log) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "pre-roll: the host has never published game_state through HOST-NV, so "
                             "its phase cannot be waited on; ending on the disc bind alone, which "
                             "may leave the host in pre-launch still writing the disc");
            }
            EndPreRoll("the disc is bound, the host's phase unobservable", true);
            return;
        }
        if (!g.preroll_bound_disc_logged) {
            g.preroll_bound_disc_logged = true;
            if (g.log) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "pre-roll: the disc bound after %.1f s of real time, but the host is in "
                             "'%s' -- holding on for its launch (~%.0f s of host time, ~%.1f s more "
                             "at %.2fx)",
                             double(g.preroll_wall_us) / 1e6, HostPhaseName(),
                             kHostRoundStartToPlayingS,
                             kHostRoundStartToPlayingS / (double)g.preroll_rate,
                             (double)g.preroll_rate);
            }
        }
    }
    if (g.preroll_wall_us >= g.preroll_bound_us) {
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "PRE-ROLL GAVE UP after %.1f s of real time: disc %s, host phase '%s'. "
                         "Playback starts now and the disc settles when the host launches, as it "
                         "did before (playback.preroll_bound_ms)",
                         double(g.preroll_wall_us) / 1e6,
                         g.bind.bound_disc > 0 ? "bound" : "still not bound", HostPhaseName());
        }
        EndPreRoll("the wall bound", true);
        return;
    }
    if (g.preroll == Global::kPreRollWaiting) {
        if (g.identity.pending) return;   // the roster is still being dressed
        const bool settled = AdmissionSettled();
        if (!settled && g.preroll_wall_us < kPreRollAdmitGraceUs) return;
        g.preroll = Global::kPreRollRunning;
        if (g.log) {
            const PawnLedger led = g.pawn.Ledger();
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "pre-roll: holding the playhead at %lld us and running the engine clock at "
                         "%.2fx so the host's ~%.0f s pre-match AND launch elapse (expect ~%.0f s "
                         "of real time, bound %.0f s); admission %s (%u of %u provisioned, %u failed)",
                         (long long)(g.transport.DisplayPlayheadUs() -
                                     (int64_t)g.reader.GetManifest().t0Us),
                         (double)g.preroll_scale, kHostToPlayingS,
                         PreRollExpectedWallS(),
                         double(g.preroll_bound_us) / 1e6, settled ? "settled" : "STILL OPEN",
                         led.provisioned, led.requested, led.failed);
        }
    }
    // ★ THE RAMP. One edge, one line, and it latches -- the host does not go back into its
    // pre-match, and a scale that flapped with a phase republish would be a rate change per frame.
    if (!g.preroll_ramped && PreRollInLaunchSequence()) {
        g.preroll_ramped = true;
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "pre-roll: the host is past its pre-match after %.1f s of real time -- the "
                         "engine clock goes from %.2fx to %.2fx (playback.preroll_rate) for the "
                         "launch sequence, which teleports and launches the bodies",
                         double(g.preroll_wall_us) / 1e6, (double)g.preroll_scale,
                         (double)g.preroll_rate);
        }
    }
    // Re-asserted every frame: it is two field writes, and it is what keeps the transport mirror
    // from re-freezing the engine between our tick and the clock's.
    g.svc->clock->SetPreRoll(true, g.preroll_ramped ? g.preroll_rate : g.preroll_scale);
    g.preroll_run_us += wall;
}

void BindBodyHandlesIfReady(const char* trigger, bool light = false) {
    if (!g.window_valid || !g.svc || !g.svc->entity || !g.svc->entity->Available()) {
        return;
    }
    // EVERY rebind drops handle_valid on every track, so a claim that is still stamped on the
    // engine's object has to be handed back BEFORE the handles that name it are thrown away.
    // RebuildWindow already released; this finds nothing left to do there and is the whole fix on
    // the slot-map and entity-rebind paths, where a refused re-bind used to freeze the disc. The disc's
    // level is kept over the bind and settled by ReconcileDiscLevel below, which releases it only
    // when the bind landed on a different body or on none.
    g.owner_releases += g.applier.ReleaseSyntheticOwners(
        Win(), StateApplier::OwnerRelease::KeepDiscLevel);
    const size_t had_disc = g.bind.bound_disc;
    void* const netGame = g.tick_net_game ? g.tick_net_game : LiveNetGame();
    const BindResult b = BindBodyHandles(*g.svc->entity, Win(), g.slot_map, g.slot_map_n,
                                         g.disc_drive_on, netGame);
    g.bind = b;
    g.disc_track = Global::kNoDiscTrack;
    for (size_t i = 0; i < Win().BodyCount(); ++i) {
        if (Win().BodyAt(i).claim_owner) { g.disc_track = i; break; }
    }
    g.owner_releases += g.applier.ReconcileDiscLevel(Win());
    // The probe is owed again whenever a bind that had no disc lands one: the first launch's bind
    // refused, and a retry that succeeds minutes later is exactly the event worth a line.
    if (b.bound_disc > 0 && had_disc == 0) g.disc_probe_pending = true;
    LogDiscEarlyPath();
    // A light bind BINDS and says nothing: the position line costs a pre-apply CI walk per body
    // and the summary would print once per rebuild for the length of a drag. Both wait for the
    // settle, which is the only deferral S5.4 ever had a reason for here.
    if (light) {
        ++g.scrub.light_binds;
        return;
    }
    g.position_line_pending = true;
    if (g.log) {
        // Formatted by windowbuild, beside the counters, so a field added there reaches this
        // line without anyone remembering to widen it.
        // Sized from the log's own line rather than from an observed maximum: every counter here
        // is a size_t and the entity key column widened the sentence once already, so a buffer
        // picked from today's longest line silently deletes the tail the summary exists to print.
        char summary[kLogLineMax];
        FormatBindSummary(b, summary, sizeof summary);
        g.log->Write(LogLevel::Info, kReplayModuleName, "body handles re-bound: %s (%s)", summary,
                     trigger);
        // Once per load and again only when the answer changes: a window rebuild re-binds every
        // few seconds and the disc's identity does not move with it.
        if (FormatDiscBind(b, summary, sizeof summary) &&
            DiscVerdictWorthALine(b.disc_verdict, b.disc_bound_id)) {
            g.disc_verdict_repeats = 0;
            g.log->Write(LogLevel::Info, kReplayModuleName, "disc bind %s", summary);
        }
    }
}

// The load's statement of where each wire sender sits against the capture axis. A spectator capture
// has nothing BUT wire poses, so a sender missing from these lines is a player who will not move.
void LogWireClocks() {
    if (!g.log) return;
    const TrackScan& s = g.track_scan;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "track scan: %u wire records from %zu senders (%u past the sender table); deepest "
                 "chapter holds %u body records, %u player records, %u ticks",
                 unsigned(s.wire_records), s.clocks.count, unsigned(s.clocks.senders_overflowed),
                 unsigned(s.max_body_records), unsigned(s.max_player_records),
                 unsigned(s.max_ticks));
    for (size_t i = 0; i < s.clocks.count; ++i) {
        const uint32_t who = s.clocks.sender[i];
        char name[32];
        if (who == kEngineInvalidUserId) {
            _snprintf_s(name, sizeof name, _TRUNCATE, "server (unowned)");
        } else {
            _snprintf_s(name, sizeof name, _TRUNCATE, "slot %u gen %u", unsigned(who & 0xFFFFu),  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
                        unsigned(who >> 16));
        }
        char line[256];
        FormatWireClock(s.clocks, i, name, line, sizeof line);
        const bool wide = s.clocks.SpreadUs(i) > WireClocks::kSpreadWarnUs;
        g.log->Write(wide ? LogLevel::Warn : LogLevel::Info, kReplayModuleName, "%s", line);
    }
}

// The netvar bind, through the netGame's global netvar handle (netvarsite.h).
//
// Retried EVERY TICK while a replay is loaded. The handle is first filled when the level's
// global_vars component is initialised; every earlier attempt in a launch ran at state 7 or 4 and
// logged "not located", and nothing asked again. The check is a handful of guarded reads and no
// engine call, so it also runs every tick after the bind: a level re-entry changes the component,
// its slot or its space, the table is taken back from the applier, and the next valid tick binds
// the new one.
//
// A refusal is logged once per distinct reason, not once per tick, with the values it was made from.
//
// This binds the GENERIC store decoder only. The HUD keys go through the typed setters, which need
// the route and not the store (netvarhud.h), so nothing here says whether the clock is being driven.
// A second is the backstop; the epoch and the load serial are the real trigger. Ticks, not
// milliseconds, because the tick counter is already there and a slower frame wants a slower check.
constexpr uint64_t kNetVarRevalidateTicks = 60;

void MaintainNetVarBind() {
    if (!g.loaded || !g.netvar_bind_wanted) {
        return;
    }
    void* const netGame = g.tick_net_game ? g.tick_net_game : LiveNetGame();
    if (g.netvar_table_bound) {
        // ★ NOT EVERY TICK. Producing the four pointers `SameInstanceAs` compares costs a dozen
        // guarded reads, and every guarded read is a VirtualQuery: PERF-3 measured this one call at
        // 1.8 ms of every rendered frame, the largest single item in the plugin's own cost. What
        // can invalidate the bind is named two comments above -- a level re-entry that changes the
        // component, its slot or its space -- so the re-validation rides the entity bind epoch and
        // the load serial, with a one-second backstop for a teardown neither of them shows.
        const uint64_t bind_key = (uint64_t(g.entity_bind_epoch) << 32) | uint64_t(g.load_serial);
        if (bind_key == g.netvar_revalidate_key && g.ticks < g.netvar_revalidate_tick) {
            return;
        }
        g.netvar_revalidate_key = bind_key;
        g.netvar_revalidate_tick = g.ticks + kNetVarRevalidateTicks;
        const NetVarSite now = LocateNetVarSite(netGame);
        if (now.StoreOk() && now.SameInstanceAs(g.netvar_site)) {
            return;
        }
        UnbindNetVarTable(g.applier);
        g.netvar_table_bound = false;
        g.netvar_last_refusal = kNoNetVarRefusal;
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "netvar table: the bound route no longer validates at netgame state %d "
                         "(%s); unbound, re-binding when it does",
                         (int)NetGameStateOf(netGame),
                         now.StoreOk() ? "a different component instance"
                                       : NetVarSiteBindText(now.bind));
        }
    }
    char line[768];  // facts-ok: WORLD.REORIGIN_FLAG_BITS a local buffer size, not the engine offset
    const NetVarSite site = BindNetVarTableOn(g.applier, netGame, g.reader.GetManifest().levelId,
                                              LiveStoreDecode(), line, sizeof line);
    if (site.StoreOk()) {
        g.netvar_table_bound = true;
        g.netvar_site = site;
        g.netvar_last_refusal = kNoNetVarRefusal;
        if (g.log) g.log->Write(LogLevel::Info, kReplayModuleName, "%s", line);
    } else if (site.RefusalKey() != g.netvar_last_refusal) {
        g.netvar_last_refusal = site.RefusalKey();
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName, "%s; retrying every tick", line);
        }
    }
}

// Gamestate playback is for a contained process only: on a live match the host owns these netvars
// and the setters would be a client's silent no-op at best.
// RECORDER-2 part A: the two terms now live in one place (bootstrap/livesafety.h) so that the paths
// which never asked -- clock, grip, pose, audio rate, scrub, cursor, band-3 registration -- ask the
// SAME question as the ones that did. This name stays because it is what the gated callers read.
bool ProcessContained() { return livesafety::Allowed(livesafety::Feature::DiscDrive); }

void ArmGameStateNetVars() {
    const bool contained = ProcessContained();
    g.netvar_bind_wanted = contained && (g.hud_clock_on || g.hud_score_on);
    const HudNetVarSetters setters =
        g.netvar_bind_wanted ? LiveHudNetVarSetters() : HudNetVarSetters{};
    HudNetVarsArm(contained, g.hud_clock_on, g.hud_score_on, g.host_netvar_filter_on, setters);
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "gamestate netvars: %s (clock=%s score=%s contained=%s; setters real=%s "
                     "symbol=%s int=%s; read-back getters real=%s symbol=%s int=%s)",
                     g.netvar_bind_wanted ? "ARMED" : "off", g.hud_clock_on ? "on" : "off",
                     g.hud_score_on ? "on" : "off", contained ? "yes" : "no",
                     setters.setReal ? "ok" : "none", setters.setSymbol ? "ok" : "none",
                     setters.setInt ? "ok" : "none", setters.getReal ? "ok" : "none",
                     setters.getSymbol ? "ok" : "none", setters.getInt ? "ok" : "none");
    }
}

void DisarmGameStateNetVars() {
    HudNetVarsDisarm();
    UnbindNetVarTable(g.applier);
    g.netvar_table_bound = false;
    g.netvar_bind_wanted = false;
    g.netvar_last_refusal = kNoNetVarRefusal;
}

size_t CollectStatMapping(statsfeed::MappedPlayer* out, size_t cap);

// The goal feed's identity rewrite, through the SAME two steps the stat feed's header rewrite uses:
// the roster map turns a recorded slot into a live user index, and the engine's own slot table turns
// that index into the slot|generation pair its handlers validate against. `ctx` is the netGame.
bool MapRecSlotToLive(void* ctx, uint16_t recSlot, uint32_t& liveNetUserId) {
    UserIndex index(0);
    if (!g.roster_map.Lookup(recSlot, index)) return false;
    return statsfeed::LiveNetUserId(ctx, index.v, liveNetUserId);
}

// The stun feed's rewrite, and it needs one thing the goal feed's does not: the victim's live
// SActorGUID. A goal is delivered to the netgame, but a stun is delivered to a COMPONENT, and the
// actor is what finds that component. Defined below ProvisionedIdentity, which owns the hop surface.
bool MapRecSlotToLiveStun(void* ctx, uint16_t recSlot, stunfeed::LiveUser& out);

// The engine's own headcount, printed only when it moves. A leave that freed nothing and an
// admission that leaked one look identical in our counters and different in these two.
void LogEngineUserCounts(void* netGame) {
    if (netGame == nullptr || !g.log) return;
    static uint16_t last_total = 0xFFFF, last_local = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    uint16_t total = 0, local = 0;
    __try {
        const uint8_t* base = static_cast<const uint8_t*>(netGame);
        memcpy(&total, base + facts::IDENT_ENTRANT_COUNT_OFF, sizeof total);
        memcpy(&local, base + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF, sizeof local);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (total == last_total && local == last_local) return;
    last_total = total;
    last_local = local;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "engine users: total=%u local=%u (the engine's own counts, at ng+0x%X and ng+0x%X)",
                 unsigned(total), unsigned(local), unsigned(facts::IDENT_ENTRANT_COUNT_OFF),
                 unsigned(facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF));
}

double ContentSeconds(int64_t abs_us);

// ---- DESIGN-HOST-SYNC P1: the host's round clock -------------------------------------------------

constexpr uint64_t kOncePerSecondMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 ms, not the CR15Game vtable slot

// The sweeps run from CR15NetGame::Update, which the level cycle's hold in
// Tick does not cover, so they key every graph they hold to this and touch nothing while it is 0.
uint64_t LevelSpaceNow(IEntity& ent, int32_t netGameState) {
    uint64_t epoch = 0;
    // NotAvailable (an entity with no epoch) still keys on the hold and the state.
    const bool bound = ent.BindEpochNow(epoch) != IEntity::BindEpoch::Unbound;
    return levelspace::Key(g.reload_cycle.Holds(), netGameState == reloadcycle::kStateInGame,
                           bound, epoch);
}

// A walk that keeps missing is re-said this often, with how many walks missed, so a pin that never
// arms is visible for the whole session and not only in its first line.
constexpr uint64_t kClockMissResayMs = 30000;

void HostClockSweep(IEntity& ent, void* netGame, uint64_t nowMs, uint64_t space) {
    clockpin::Policy p;
    p.loaded = g.loaded;
    p.allowed = livesafety::Allowed(livesafety::Feature::HostClockPin);
    p.enabled = g.host_clock_pin_on;
    p.space = space;
    p.netGame = netGame;
    const clockpin::Report r = g.host_clock_pin.Step(ent, p, nowMs);
    if (!g.log) return;
    switch (r.event) {
        case clockpin::Event::Idle:
        case clockpin::Event::Waiting:
            return;
        case clockpin::Event::NotFound:
            ++g.host_clock_misses;
            if (r.why != g.host_clock_why_said ||
                nowMs - g.host_clock_miss_said_ms >= kClockMissResayMs) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "host clock pin: no arena graph %016llX in %u space(s) (last: %s); "
                             "%u walk(s) missed; re-walking at most once a second",
                             (unsigned long long)scriptgraph_facts::kArenaScriptHash,
                             r.spacesWalked, r.why ? r.why : "no reason given",
                             g.host_clock_misses);
                g.host_clock_why_said = r.why;
                g.host_clock_miss_said_ms = nowMs;
                g.host_clock_misses = 0;
            }
            return;
        case clockpin::Event::Released:
            g.host_clock_why_said = nullptr;
            g.host_clock_line_ms = 0;
            if (r.haveBefore) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "host clock pin RELEASED at unload -- left at %.2f so the idle host "
                             "stays in main_loop",
                             r.before);
            } else {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "host clock pin RELEASED at unload -- nothing written; the arena graph "
                             "did not read back on this tick");
            }
            return;
        case clockpin::Event::Refused:
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "host clock pin: REFUSED -- set answered %d, get reads %.2f (module+%u "
                         "set binding, arena graph %016llX inst %p); no further write this load",
                         r.setAnswered ? 1 : 0, r.haveAfter ? r.after : -1.0f,
                         unsigned(scriptgraph_facts::kModuleSetOff),
                         (unsigned long long)scriptgraph_facts::kArenaScriptHash, r.ref.inst);
            break;
        case clockpin::Event::Armed:
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "host clock pin ARMED: game_timer %.2f -> %.0f (arena graph %016llX inst "
                         "%p, space %u of the walk)",
                         r.haveBefore ? r.before : -1.0f, double(clockpin::kPinValue),
                         (unsigned long long)scriptgraph_facts::kArenaScriptHash, r.ref.inst,
                         r.spacesWalked);
            break;
        case clockpin::Event::Observed:
        case clockpin::Event::Pinned:
            break;
    }
    if (!r.haveBefore) return;
    if (!r.firstSight && nowMs - g.host_clock_line_ms < kOncePerSecondMs) return;
    g.host_clock_line_ms = nowMs;
    const HostPhase phase = HostPhaseNow();
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "host clock: game_timer %.2f (%s) arena graph %016llX inst %p phase '%s' pin %s",
                 r.before, r.firstSight ? "first sight" : "before pin",
                 (unsigned long long)scriptgraph_facts::kArenaScriptHash, r.ref.inst,
                 phase.name ? phase.name : "unknown", clockpin::EventName(r.event));
}

// ---- DESIGN-HOST-SYNC P4: the disc shell ------------------------------------------------------------

// The FX graph handle and everything the feed learned by talking to that graph; the plan and the
// span grabs come from the file and stay.
void ForgetShellFx() {
    g.shell_fx_ok = false;
    g.shell_fx_space = 0;
    g.shell_fx_phys = 0;
    g.shell_fx_walk_ms = 0;
    g.shell_fx_why_said = nullptr;
    g.shell_feed.Reset();
    g.shell_prev_on = false;
    g.shell_grab_said.Reset();
    g.shell_prev_w6 = g.shell_prev_w1 = 0;
    g.shell_send_refused_said = false;
}

void ResetShellFeed() {
    g.shell_plan.Clear();
    ForgetShellFx();
    g.shell_transport_set = false;
    g.shell_last_k = -1;
    g.shell_grabs_settled = false;
    g.shell_grabs_id = 0;
}

// Every handle the sweeps hold into a level space. The keys drop them on their own at the next
// sweep; this drops them at the edges the module sees first.
void ForgetLevelHandles() {
    g.host_clock_pin.Forget();
    ForgetShellFx();
}

// R[k] from the recorded game_state edges the file's own marker table carries (kMarkPhase, payload
// = the state symbol), the colour from the recorded goal rows.
void BuildShellPlan() {
    ResetShellFeed();
    static shellplan::GoalIn goals[goalfeed::kMaxRows];
    static shellplan::PhaseIn phases[1024];
    const goalfeed::RecordedGoals& rg = goalfeed::PlaybackRecordedGoals();
    uint32_t nGoals = 0;
    for (uint32_t i = 0; i < rg.Count() && nGoals < goalfeed::kMaxRows; ++i) {
        goals[nGoals].t_us = (int64_t)rg.At(i).t_us;
        goals[nGoals].team = rg.At(i).team;
        ++nGoals;
    }
    const Manifest& m = g.reader.GetManifest();
    uint32_t nPhases = 0;
    for (uint16_t i = 0; i < m.roundCount && nPhases < 1024; ++i) {
        if (m.rounds[i].kind != capture::kMarkPhase) continue;
        const uint64_t sym = m.rounds[i].payload;
        bool preLaunch = false;
        for (const gamestate::facts::PhaseFact& f : gamestate::facts::kPhaseFacts) {
            if (f.symbol != sym) continue;
            preLaunch = strcmp(f.name, "pre_launch_enter") == 0 ||
                        strcmp(f.name, "pre_launch_update") == 0;
            break;
        }
        phases[nPhases].t_us = (int64_t)m.rounds[i].tUs;
        phases[nPhases].pre_launch = preLaunch;
        ++nPhases;
    }
    g.shell_plan.Build(g.core_data_first_us, goals, nGoals, phases, nPhases);
    if (!g.log) return;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "disc shell: %u span(s) from %u goal row(s) and %u recorded game_state edge(s); "
                 "feed %s (playback.disc_shell)",
                 g.shell_plan.Count(), nGoals, nPhases, g.disc_shell_on ? "ON" : "OFF");
    for (uint32_t k = 1; k < g.shell_plan.Count(); ++k) {
        const shellplan::Span& sp = g.shell_plan.At(k);
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc shell: restart #%u at %.3fs after goal %d (team %u) -> %s", k,
                     ContentSeconds(sp.r_us), sp.goal,
                     unsigned(rg.At(uint32_t(sp.goal)).team), shellplan::ColourName(sp.colour));
    }
}

// The recorded holder of the bound disc at t: the newest sample at or before t.
bool DiscOwnedAt(int64_t t) {
    BodyTrack* tr = DiscTrack();
    if (!tr || !tr->count) return false;
    const BodyState* frames = Win().BodyFrames(*tr);
    const uint64_t* times = Win().BodyTimes(*tr);
    size_t i = tr->count;
    while (i > 0 && (int64_t)times[i - 1] > t) --i;
    return i > 0 && frames[i - 1].owner_slot != kBodyOwnerNone;
}

// Every span's first grab, from the load scan's whole-file possession edges of the track the disc
// is bound to -- settled when the disc binds, since the track is not known before, and again if it
// binds another track.
void SettleShellGrabs() {
    BodyTrack* const tr = DiscTrack();
    if (!tr || (g.shell_grabs_settled && g.shell_grabs_id == tr->id.value)) return;
    g.shell_grabs_settled = true;
    g.shell_grabs_id = tr->id.value;
    g.shell_plan.ClearGrabs();
    const TrackScan& scan = g.track_scan;
    if (scan.holder_edges_incomplete) {
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "disc shell: first grabs REFUSED -- the load scan's possession index is "
                         "incomplete; only a disc owned at the playhead settles a span");
        }
        return;
    }
    static shellplan::HolderEdgeIn edges[TrackScan::kMaxHolderEdges];
    uint32_t n = 0;
    for (const TrackScan::HolderEdge& e : scan.holder_edges) {
        if (e.id_value == tr->id.value && n < TrackScan::kMaxHolderEdges) {
            edges[n++] = {e.t_us, e.owned};
        }
    }
    if (n == 0) {
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "disc shell: first grabs REFUSED -- the load scan holds no possession rows "
                         "for disc track %016llX (%u edge(s) of other bodies, %u unplaced)",
                         (unsigned long long)tr->id.value, unsigned(scan.holder_edges.size()),
                         scan.holder_edges_unplaced);
        }
        return;
    }
    g.shell_plan.FillGrabs(edges, n);
    if (!g.log) return;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "disc shell: first grabs from %u possession edge(s) of disc track %016llX over the "
                 "whole file (%u unplaced)",
                 n, (unsigned long long)tr->id.value, scan.holder_edges_unplaced);
    for (uint32_t k = 0; k < g.shell_plan.Count(); ++k) {
        const shellplan::Span& sp = g.shell_plan.At(k);
        if (sp.grab_none) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "disc shell: span #%u (restart %.3fs) has no recorded grab", k,
                         ContentSeconds(sp.r_us));
        } else {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "disc shell: span #%u (restart %.3fs) first grab at %.3fs", k,
                         ContentSeconds(sp.r_us), ContentSeconds(sp.g_us));
        }
    }
}

// A span the index could not settle is still settled by a disc owned at t: a grab happened.
shellplan::Want ShellWantAt(int64_t t) {
    SettleShellGrabs();
    shellplan::Want w = g.shell_plan.WantAt(t);
    if (!w.known && w.k >= 0 && DiscOwnedAt(t)) {
        w.known = true;
        w.show = false;
    }
    return w;
}

// The FX graph on the actor the disc track drives, never merely the first one found: the census
// names the bound actor, and the graph walk is filtered to it.
bool FindShellFx(IEntity& ent, void* netGame, uint64_t nowMs, uint64_t levelSpace) {
    if (g.shell_fx_ok && g.shell_fx_space != levelSpace) ForgetShellFx();
    BodyTrack* const tr = DiscTrack();
    const uint32_t physId = tr ? tr->disc_component_id : 0;
    if (g.shell_fx_ok && tr && physId == g.shell_fx_phys && ent.ScriptGraphValid(g.shell_fx)) {
        return true;
    }
    g.shell_fx_ok = false;
    if (g.shell_fx_walk_ms != 0 && nowMs - g.shell_fx_walk_ms < kOncePerSecondMs) return false;
    g.shell_fx_walk_ms = nowMs;
    const char* why = "the disc track is not bound";
    uint64_t actorId = 0;
    bool haveActor = false;
    if (tr) {
        static IEntity::DiscRingReading ring;
        if (!ent.DiscRingCensus(ring)) {
            why = ring.refused ? ring.refused : "the disc-ring census refused";
        } else {
            haveActor = shellfeed::BoundDiscActor(ring, physId, actorId, &why);
        }
    }
    void* spaces[8] = {};
    const uint32_t n = haveActor ? ent.DiscCandidateSpaces(netGame, spaces, 8) : 0;
    for (uint32_t i = 0; haveActor && i <= n && i < 8; ++i) {
        void* const space = i < n ? spaces[i] : nullptr;
        if (!ent.FindScriptGraph(space, scriptgraph_facts::kDiscFxScriptHash, actorId, 0,
                                 g.shell_fx, &why)) {
            continue;
        }
        if (!g.shell_fx.selfValid) {
            why = "the FX graph's `self` did not read back";
            continue;
        }
        g.shell_fx_ok = true;
        g.shell_fx_space = levelSpace;
        g.shell_fx_phys = physId;
        g.shell_fx_why_said = nullptr;
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "disc shell: FX graph %016llX found on the bound disc actor %016llX "
                         "(component %u) inst %p",
                         (unsigned long long)scriptgraph_facts::kDiscFxScriptHash,
                         (unsigned long long)g.shell_fx.actorLo, physId, g.shell_fx.inst);
        }
        return true;
    }
    if (g.log && why != g.shell_fx_why_said) {
        g.shell_fx_why_said = why;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "disc shell: feed idle -- no FX graph %016llX on the bound disc actor (%s)",
                     (unsigned long long)scriptgraph_facts::kDiscFxScriptHash, why);
    }
    return false;
}

void DiscShellSweep(IEntity& ent, void* netGame, uint64_t nowMs, uint64_t space) {
    if (space == 0) {
        if (g.shell_fx_ok) ForgetShellFx();
        return;
    }
    if (!g.loaded || g.shell_plan.Count() == 0) return;
    if (!FindShellFx(ent, netGame, nowMs, space)) return;
    const int64_t t = g.present.presented_us;

    int32_t w1 = 0, w6 = 0;
    const bool read = ent.ScriptDataWord(g.shell_fx, scriptgraph_facts::kFxWordSpawnBlue, w1) &&
                      ent.ScriptDataWord(g.shell_fx, scriptgraph_facts::kFxWordSpawnOrange, w6);
    shellfeed::Input in;
    in.want = ShellWantAt(t);
    in.obs = shellfeed::ObserveWords(read, w1, w6);
    in.allowed = g.disc_shell_on && livesafety::Allowed(livesafety::Feature::DiscShellFeed);
    // A restart crossed by forward play is the ordinary case again: nothing was set by hand.
    if (in.want.k != g.shell_last_k && g.shell_epoch == g.shell_last_epoch && t > g.shell_last_t) {
        g.shell_transport_set = false;
    }
    in.transport_set = g.shell_transport_set;
    in.disc_owned = DiscOwnedAt(t);
    in.epoch = g.shell_epoch;
    in.now_ms = nowMs;
    in.clock_running = !g.transport.Paused();

    if (read && g.log && g.shell_prev_on && !in.obs.on && !in.want.show && in.want.known &&
        g.shell_grab_said.Fall(in.want.k, g.shell_epoch)) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc shell: first grab at %.3fs -- cleared by the engine's OnNetTouchGrab arm "
                     "(w6 %d->%d, w1 %d->%d)",
                     ContentSeconds(t), g.shell_prev_w6, w6, g.shell_prev_w1, w1);
    }
    if (read) {
        g.shell_prev_on = in.obs.on;
        g.shell_prev_w1 = w1;
        g.shell_prev_w6 = w6;
    }
    g.shell_last_k = in.want.k;
    g.shell_last_epoch = g.shell_epoch;
    g.shell_last_t = t;

    const shellfeed::Step step = g.shell_feed.Next(in);
    if (step.action == shellfeed::Action::None) return;
    if (step.action == shellfeed::Action::Refused) {
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "disc shell: span #%d REFUSED after %u deliveries -- the FX graph did not "
                         "reach the wanted state (w1 %d w6 %d); no further delivery this seek",
                         step.k, step.attempts, w1, w6);
        }
        return;
    }
    const bool show = step.action == shellfeed::Action::Show;
    const shellfeed::ShowEvents events = shellfeed::ShowEventsFor(step.colour);
    const bool sent =
        show ? ent.ScriptSendEvent(g.shell_fx, events.fxState, ~0ull) &&
                   ent.ScriptSendEvent(g.shell_fx, events.spawn, ~0ull)
             : ent.ScriptSendEvent(g.shell_fx, scriptgraph_facts::kSymOnNetTouchGrab,
                                   scriptgraph_facts::kSymNetTouchInteract);
    if (!sent) {
        if (g.log && !g.shell_send_refused_said) {
            g.shell_send_refused_said = true;
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "disc shell: sendevent REFUSED -- the module+%u binding is outside the "
                         "script image, `self` is unread, or this session is live",
                         unsigned(scriptgraph_facts::kModuleSendEventOff));
        }
        return;
    }
    if (!g.log) return;
    if (show) {
        const shellplan::Span& sp = g.shell_plan.At(uint32_t(step.k));
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc shell: restart #%d at %.3fs (goal team %d) -> forceSetFXState_%s + %s "
                     "delivered (sendevent)%s, attempt %u, playhead %.3fs",
                     step.k, ContentSeconds(sp.r_us),
                     sp.goal >= 0 ? int(goalfeed::PlaybackRecordedGoals().At(uint32_t(sp.goal)).team)
                                  : -1,
                     shellplan::ColourName(step.colour),
                     step.colour == shellplan::Colour::Red ? "spawn_orange_particles"
                                                           : "spawn_blue_particles",
                     step.retry ? " RETRY" : "", step.attempts,
                     ContentSeconds(t));
    } else if (step.residual) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc shell: seek-set playhead in an unowned post-grab span -> OnNetTouchGrab "
                     "delivered to the FX graph only (span #%d, playhead %.3fs)",
                     step.k, ContentSeconds(t));
    } else {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc shell: shell shows the other colour than span #%d wants (%s) -> "
                     "OnNetTouchGrab delivered to the FX graph only; the show follows",
                     step.k, shellplan::ColourName(step.colour));
    }
}

void HudNetVarStageSweep(void* /*ctx*/, void* netGame) {
    // The engine drives this from CR15NetGame::Update, about four times a rendered frame, so it is a
    // cost root of its own and not part of the playback tick's total.
    const tickcost::Scope cost_root(tickcost::kRootSweep);
    {
        // The prologue every sweep pays whatever the session is doing. Scoped so that the sweep's
        // fixed cost is a named phase rather than the `other` the whole of PERF-2 went looking in.
        const tickcost::Scope cost_tail(tickcost::kSweepTail);
        // The stat feed rides the same tail. It is serviced ahead of the session blocker because the
        // RELEASE is exactly what a session that is going away owes the next load.
        statsfeed::ServiceRelease(netGame);
        // ...and the LEAVE FEED rides it after the stat release, never before: the stat release
        // writes into the entrant's own record, and the leave destroys that record. Same frame,
        // that order.
        if (identity::RosterLeavePending() && g.svc && g.svc->identity && g.log) {
            identity::ServiceRosterLeave(*g.svc->identity, *g.log, g.roster_map);
            const identity::LeaveReport& t = identity::RosterLeaveTotals();
            g.identity.released = t.left;
            g.identity.release_refused = t.refused;
        }
        LogEngineUserCounts(netGame);
    }
    if (statsfeed::Armed() && g.loaded) {
        const tickcost::Scope cost(tickcost::kStats);
        statsfeed::MappedPlayer mapped[statsfeed::kMaxSlots];
        const size_t n = CollectStatMapping(mapped, statsfeed::kMaxSlots);
        statsfeed::Sweep(netGame, g.present.presented_us, mapped, n, GetTickCount64());
    }
    // The goal feed rides the same tail, after the stats: a goal's banner reads the scorer's name
    // and the scoreboard beside it, and the numbers should already be this instant's when it opens.
    if (goalfeed::Armed() && g.loaded) {
        const tickcost::Scope cost(tickcost::kGoals);
        // The flash, the accrual SFX and the goal_fx explosion are the engine's own reaction to a
        // score-netvar change, so the goal feed reports the HUD writer's state as their reason.
        // ONE AXIS. The probe line sits beside `goals: fire`, which speaks content-relative
        // seconds; handing it the file's absolute stamp would put the two lines 16 s apart on the
        // owner's capture and read as a placement defect that is not there.
        HudNetVarsSetPlayheadUs(g.present.presented_us - g.core_data_first_us);
        goalfeed::SetScoreTriggerRefusal(HudNetVarsScoreTriggerRefusal());
        goalfeed::Sweep(netGame, g.present.presented_us, PreRollPreparing(), &MapRecSlotToLive,
                        netGame);
    }
    // The stun feed rides the same tail. It is last of the three because it is the only one that
    // writes a PLAYER'S state: the pose pass has already re-driven every body this tick, so the
    // record it touches is the one the engine's own Update will read next -- and the pose stream
    // carries no stun field, so nothing overwrites it back.
    if (stunfeed::Armed() && g.loaded) {
        const tickcost::Scope cost(tickcost::kStuns);
        stunfeed::Sweep(netGame, g.present.presented_us, PreRollPreparing(), &MapRecSlotToLiveStun,
                        netGame);
    }
    // The component feed rides the same tail, and it has to: the raise takes the event DB's lock
    // through a per-thread counter read out of ENGINE TLS, so the engine's own update stack is the
    // only place this call is defined at all.
    if (componentfeed::Armed() && g.loaded) {
        const tickcost::Scope cost(tickcost::kEvents);
        componentfeed::Sweep(netGame, g.present.presented_us, PreRollPreparing());
    }
    // DESIGN-HOST-SYNC P1 and P4, above the session blocker's return: the unload tick is one of the
    // ticks it returns on, and the pin owes that tick its release line.
    if (g.svc && g.svc->entity) {
        const uint64_t nowMs = GetTickCount64();
        const uint64_t space = LevelSpaceNow(*g.svc->entity, NetGameStateOf(netGame));
        HostClockSweep(*g.svc->entity, netGame, nowMs, space);
        {
            const tickcost::Scope cost(tickcost::kEvents);
            DiscShellSweep(*g.svc->entity, netGame, nowMs, space);
        }
    }
    // The detour hands over every CR15NetGame::Update, whatever state the session is in; the typed
    // setters are only called into a running match.
    if (PawnProvision::SessionBlocker(netGame) != PawnProvision::Refusal::None) {
        // ★★★ THE END OF A MATCH IS NOT THE END OF THE REPLAY. Idling drops the guard to
        // PassThrough, which hands every driven key back to the host mid-file -- the "frozen at
        // EOF" reading, where the clock stops being the recording's and becomes whatever the host
        // left in the record. While a file is loaded the setter sweep is skipped (the typed setters
        // are only safe inside a running match) but the owned set and the Filter mode stay exactly
        // as they were, so the last published value holds. PassThrough belongs to unload, and
        // DisarmGameStateNetVars is where unload does it.
        if (!g.loaded) HudNetVarsIdle();
        return;
    }
    const tickcost::Scope cost_hud(tickcost::kHud);
    HudNetVarsSweep(netGame, GetTickCount64());
}

// The panel answers for the FILE, not for the frame. ApplyStats sees one window, so a playhead in a
// chapter with no netvar writes reads NoRecords even in a capture full of them; the sticky bit is
// what stops that frame from publishing a property of the file it cannot see.
//
// ApplyStats also sees only the GENERIC decoder path: the HUD keys are withheld from it and written
// by the setter sweep, so its "refused" and "none due" are reported beside what that sweep is doing.
const char* NetVarFidelityLine() {
    NetVarFidelityState state = g.last_apply.netvar_state;
    if (state == NetVarFidelityState::NoRecords && g.netvar_records_seen) {
        state = NetVarFidelityState::PresentNotApplied;
    }
    const bool hud = HudNetVarsApplying();
    if (state == NetVarFidelityState::Refused && g.netvar_bind_wanted) {
        if (g.netvar_table_bound) {
            return hud ? "netvars: HUD clock/score keys APPLIED via engine setters; other keys "
                         "STORED, NOT APPLIED (the generic decoder refused them)"
                       : "netvars: STORED, NOT APPLIED (the generic decoder refused them; HUD "
                         "setters not writing)";
        }
        return hud ? "netvars: HUD clock/score keys APPLIED via engine setters; other keys STORED, "
                     "NOT APPLIED (generic decoder not bound)"
                   : "netvars: STORED, NOT APPLIED (generic decoder not bound; HUD setters not "
                     "writing)";
    }
    if (state == NetVarFidelityState::PresentNotApplied && hud) {
        return "netvars: HUD clock/score keys APPLIED via engine setters; no other key due at this "
               "playhead";
    }
    return NetVarFidelityText(state);
}

// entity.h:650: `virtual void SetLodThrottle(bool enabled) = 0;  // CR15RemotePlayerCS + 520`.
// The live policy round-robins remote bodies because a live player is looking through one head; a
// replay flies a free camera, so the body the policy would have skipped this frame is the one being
// watched. Suppressed while a file is loaded and PUT BACK when it is not.
//
// Written only on a change, and the target pointer is part of the comparison: W1-E publishes one
// entity singleton whose Available() flips when a game space binds, and the write we made through
// an unbound surface reached nothing.
// This used to run above the load gate and wrote a 1 into the enable dword
// of EVERY session -- a value it had never read, in sessions that had suppressed nothing, with no
// path back.  Two things changed.  It is called only from the loaded half of the tick and only
// when the predicate says this process is contained, so `suppress` is no longer a variable: a
// loaded replay always wants the round-robin off.  And the un-suppress direction is not a write of
// 1 any more -- ReleaseLodThrottle below puts back the value the engine itself had.
void ApplyLodThrottle() {
    if (!livesafety::Allowed(livesafety::Feature::LodThrottle)) return;
    IEntity* const ent = g.svc ? g.svc->entity : nullptr;
    if (ent == nullptr || !ent->Available()) {
        // Nothing to write through, so nothing is recorded as written: the next tick that finds a
        // bound surface must still make the write.
        g.lod_throttle_target = nullptr;
        return;
    }
    if (g.lod_throttle_target == ent) return;
    // ★ PERF-1. Ledger row 0.14 was declared in W2-B and never read by anything: the pin happened
    // whatever the config said, so no launch in this project's history has run without it. That matters
    // now, because zeroing the enable dword makes the engine call UpdateRemoteBody instead of
    // UpdateRemoteBodyThrottled on EVERY remote player EVERY frame, and DIAG-PERF-27FPS measured a cost
    // linear in recorded players (~2.4 ms each) that survives a pause. The default is unchanged -- the
    // pin is what a free camera was designed to want -- but ECHOREPLAY_PLAYBACK_LOD_PIN=0 now makes that
    // an A/B rather than an axiom. Read below the idempotence latch: GetBool walks the key table, so on
    // the pinned path it runs once per load, not once per tick.
    if (g.svc->config == nullptr || !g.svc->config->GetBool("playback.lod_pin", true)) {
        if (g.log && !g.lod_pin_refused) {
            g.lod_pin_refused = true;
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "lod throttle NOT claimed: playback.lod_pin is off, so the engine keeps its own "
                         "remote-player round-robin for this load");
        }
        return;
    }
    if (!ent->ClaimLodThrottle(false)) {
        // The claim could not read the field, so it did not write it either. Nothing to release.
        g.lod_throttle_target = nullptr;
        return;
    }
    g.lod_throttle_suppressed = true;
    g.lod_throttle_target = ent;
    ++g.lod_throttle_writes;
    // One line per claim, because the claim/release contract was otherwise unobservable at runtime
    // and the last launch could only INFER the hand-back (LAUNCH-LIVEFIX 4.1).
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "lod throttle CLAIMED on entity surface %p (write %llu): the remote-player "
                     "round-robin is off for the duration of this load", (const void*)ent,
                     (unsigned long long)g.lod_throttle_writes);
    }
}

// The other end of the claim: unload, the end of the match this load was playing in, and shutdown.
// Safe to call having never claimed -- the entity answers false and nothing is written.
void ReleaseLodThrottle() {
    IEntity* const ent = g.svc ? g.svc->entity : nullptr;
    const bool held = ent != nullptr && g.lod_throttle_target == ent;
    if (held) ent->ReleaseLodThrottle();
    // Printed only where something was actually handed back: a line on every unload, claimed or
    // not, would say the release ran on loads that never claimed.
    if (held && g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "lod throttle RELEASED on entity surface %p: the value the engine itself had "
                     "is back", (const void*)ent);
    }
    g.lod_throttle_target = nullptr;
    g.lod_throttle_suppressed = false;
}


// ---------------------------------------------------------------------------------------------
// S12.5.1 -- THE SCRIPTED CAMERA TRACK, AND WHY IT IS A TABLE IN THIS FILE.
//
// The image gate needs a camera pose that is an exact function of source time; a hand-flown camera
// and a wall-clock integrator both fail that by construction (S8.1.5). S5.4.7's ORACLE table names
// the source of the track "our own transport", and this module is it: a fixed set of waypoints
// with a fixed interpolation, evaluated identically in both passes.
//
// The loop closes -- the last segment returns to the first waypoint -- so the function is total.
// A track that HELD its last pose would leave every frame after 30 s of source time comparing one
// static image, which is a gate that passes because it stopped looking.
struct OracleWaypoint {
    int64_t tUs;
    float   pos[3];
    float   quat[4];      // (x, y, z, w), the order camera_writepoint.h states
    float   fovY;         // vertical, radians
};

// Four legs of ten seconds. The leg is derived from the duration it is rather than written
// out in microseconds: that digit run is also the value of CLOCK.CTOR_MAXELAPSED_INIT, and a
// literal scan cannot tell an unrelated coincidence from a transcribed engine fact.
constexpr int64_t kOracleTrackLegUs =
    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds(10)).count();
const int64_t kOracleTrackPeriodUs = 4 * kOracleTrackLegUs;   // the loop, in source time

// 70 degrees, written as the angle it is rather than as its radian value: the number a
// reader checks is the one in the design, and a transcribed constant is a number nobody
// can check at all.
const float kOracleTrackFovY = 70.0f * (3.1415927f / 180.0f);

const OracleWaypoint kOracleTrack[] = {
    {                 0, {  0.0f, 4.0f, -16.0f}, {0.0f,  0.000000f, 0.0f, 1.000000f}, kOracleTrackFovY},
    {     kOracleTrackLegUs, { 12.0f, 6.0f,  -8.0f}, {0.0f,  0.258819f, 0.0f, 0.965926f}, kOracleTrackFovY},
    { 2 * kOracleTrackLegUs, {  0.0f, 8.0f,  16.0f}, {0.0f,  1.000000f, 0.0f, 0.000000f}, kOracleTrackFovY},
    { 3 * kOracleTrackLegUs, {-12.0f, 6.0f,  -8.0f}, {0.0f, -0.258819f, 0.0f, 0.965926f}, kOracleTrackFovY},
};

}  // namespace

void OracleTrackPoseAt(int64_t sourceTimeUs, float pos[3], float quat[4], float* fovYRadians) {
    const size_t n = sizeof kOracleTrack / sizeof kOracleTrack[0];
    const int64_t t = sourceTimeUs <= 0 ? 0 : sourceTimeUs % kOracleTrackPeriodUs;
    size_t i = 0;
    while (i + 1 < n && t >= kOracleTrack[i + 1].tUs) {
        ++i;
    }
    const OracleWaypoint& a = kOracleTrack[i];
    const OracleWaypoint& b = kOracleTrack[(i + 1) % n];
    const int64_t endUs = (i + 1 < n) ? kOracleTrack[i + 1].tUs : kOracleTrackPeriodUs;
    const double span = (double)(endUs - a.tUs);
    const float u = span > 0.0 ? (float)((double)(t - a.tUs) / span) : 0.0f;

    for (size_t k = 0; k < 3; ++k) {
        pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * u;
    }
    // The shortest arc, not the shortest numbers: q and -q are the same orientation, so a segment
    // whose endpoints have a negative dot interpolates the long way round and the camera swings
    // through the arena between two adjacent waypoints.
    float dot = 0.0f;
    for (size_t k = 0; k < 4; ++k) {
        dot += a.quat[k] * b.quat[k];
    }
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    float len2 = 0.0f;
    for (size_t k = 0; k < 4; ++k) {
        quat[k] = a.quat[k] + (sign * b.quat[k] - a.quat[k]) * u;
        len2 += quat[k] * quat[k];
    }
    if (len2 > 0.0f) {
        const float inv = 1.0f / sqrtf(len2);
        for (size_t k = 0; k < 4; ++k) {
            quat[k] *= inv;
        }
    }
    *fovYRadians = a.fovY + (b.fovY - a.fovY) * u;
}

namespace {

IReplayCameraControl* CameraControl() {
    return g.camera_control_override != nullptr ? g.camera_control_override : camera_control::Get();
}

void EndOracleTrackIfActive(const char* why) {
    if (!g.oracle_active) {
        return;
    }
    if (g.oracle_holder != nullptr) {
        g.oracle_holder->EndOracleTrack();
    }
    g.oracle_active = false;
    g.oracle_holder = nullptr;
    ++g.oracle_ends;
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "oracle camera track: mode api left (%s), %llu poses driven; the stock camera "
                     "has the rig back",
                     why, (unsigned long long)g.oracle_poses);
    }
}

// Entered at Load and only at Load: the bracket's whole value is that the pose is a function of
// source time for the WHOLE pass, and a track that could start mid-pass would produce two files
// the image gate cannot compare.
void ArmOracleTrackIfAsked() {
    g.oracle_asked = g.svc != nullptr && g.svc->config != nullptr &&
                     g.svc->config->GetBool("playback.oracle_track", false);
    g.oracle_refusal = "";
    g.oracle_poses = 0;
    if (!g.oracle_asked) {
        return;
    }
    // A track that takes the camera and cannot write a pose is worse than no track at all: the
    // human's free camera is held by us and nothing drives it. Refuse before Begin, never after.
    if (g.svc->camera == nullptr) {
        g.oracle_refusal = "no-pose-sink";
    } else {
        IReplayCameraControl* cam = CameraControl();
        if (cam == nullptr) {
            g.oracle_refusal = "camera-control-unavailable";
        } else if (!cam->BeginOracleTrack()) {
            g.oracle_refusal = ControlRefusalName(cam->LastRefusal());
        } else {
            g.oracle_active = true;
            g.oracle_holder = cam;
            ++g.oracle_begins;
        }
    }
    if (!g.log) {
        return;
    }
    if (g.oracle_active) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "oracle camera track: mode api entered, pose is an exact function of source "
                     "time (S12.5.1); SetMode is refused with oracle-track-active until unload");
    } else {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "oracle camera track refused: %s; this pass renders whatever camera it was "
                     "flown with and is not an image-gate reference",
                     g.oracle_refusal);
    }
}

// Per frame while the bracket is open (S5.4.7: "per frame during a reference pass only").
//
// Driven from the PRESENTED source time, which is the instant this frame's content comes from and
// the number the S12.5.2 sidecar records -- so two passes matched by source_time_us are matched at
// equal camera poses whatever display bias either pass measured.
void DriveOracleTrack(int64_t presentedUs) {
    if (!g.oracle_active || g.svc == nullptr || g.svc->camera == nullptr) {
        return;
    }
    const int64_t origin = (int64_t)g.reader.GetManifest().t0Us;
    float pos[3];
    float quat[4];
    float fovY = 0.0f;
    OracleTrackPoseAt(presentedUs - origin, pos, quat, &fovY);
    g.svc->camera->SetPose(pos, quat, fovY);
    ++g.oracle_poses;
}

// The pins the last resolved build carries. Factored out because a drag defers it and EndScrub
// then owes it from the SAME BuildStats -- a second copy of this block is how two paths come to
// disagree about what an absent pin block means.
void PushAudioPinsFromBuild() {
    if (!g.build.keyframe_present) {
        return;
    }
    audio::AudioPins pins;
    if (!g.build.audio_pins_present) {
        ++g.audio_pins_absent;
    } else if (audio::DecodePins(g.build.audio_pins_raw, g.build.audio_pins_bytes, &pins,
                                 nullptr) != audio::PinDecode::Ok) {
        // Present and unreadable is a third answer, and it is not absence: the file MEANT to
        // say something about the sustained audio here and this build could not read it.
        ++g.audio_pins_refused;
    } else {
        audio::SetAudioPins(pins);
        ++g.audio_pins_pushed;
    }
}

void AdoptWindowTail(bool light);

// ★ `light` is DESIGN-UI-2 S5.4's per-drag-step profile: the decode and everything the picture is
// a function of still runs, and only the bookkeeping a drag cannot see is deferred to EndScrub --
// the three build log lines, the HUD netvar invalidate, the netvar warn, the audio pin decode (the
// mix is muted for the whole drag anyway), the dress re-arm and TrimHistories, which is a bound and
// not a correctness condition.
// ★★ NOTHING POSITIONAL IS DEFERRED, and the body BIND is positional: S5.4 put the player re-bind
// on this list and that is what froze the bodies (G3). It now runs on every build, light or not;
// only the two lines it used to print wait for the settle.
bool RebuildWindow(int64_t t_us, bool light = false) {
    if (!g.loaded) {
        return false;
    }
    // INVARIANT: every SetSyntheticOwner (apply.cpp:128) has exactly one matching
    // ClearSyntheticOwner before the handle can be re-bound or the file unloaded.
    // A bind constructs each BodyTrack with synthetic_owner_set == false
    // (windowbuild.cpp:334), so the build below is the moment OUR record of a claim is
    // destroyed -- and every rebuild in this module funnels through this function (load,
    // SeekAbsolute, Tick step 3). Releasing HERE, ahead of the build, is what makes the
    // pairing hold by construction: no claim can outlive the bookkeeping that names it.
    // Tick's release on a discontinuity is still required and still runs -- a seek inside
    // the resident span hands authority back with no rebuild at all -- and finds nothing
    // left to do when a rebuild already ran. A build that REFUSES has released too, and
    // deliberately: handing a body back to the engine we are about to stop driving is the
    // safe direction, and the next successful build re-binds and re-claims it.
    //
    // ...with ONE exception, and it is the whole of P-1: the disc's possession level is not
    // authority scoped to the old span. Its handle does not move across a seek and its level is a
    // pure function of the new playhead, while the engine EDGE-DETECTS the dword it lives in --
    // so handing it back here fired a release, and the republish a decode later fired a grab, for
    // every seek and every scrub step. It is kept across the build and settled against what the
    // re-bind below resolves, which is where a disc that really moved gets its owner back.
    g.owner_releases += g.applier.ReleaseSyntheticOwners(
        Win(), StateApplier::OwnerRelease::KeepDiscLevel);
    const size_t centre = ChapterForTime(g.reader, t_us);
    ++g.window_builds;
    // ★ THE DEFECT, COUNTED WHERE IT HAPPENS. This is a three-chapter decode on the game's tick
    // thread: 9-13 ms, one dropped frame. With the worker on it must happen once per SEEK and never
    // on a boundary crossing, and `window async:` reports the rate so an operator can see whether
    // that is what the build is doing rather than infer it from frame times.
    ++g.tick_decodes;
    const bool built = BuildWindowRange(g.reader, centre, Win(), g.build, &g.track_scan.clocks,
                                        &g.netvar_primer);
    if (g.log && !light) {
        // Every rebuild, refused ones included: a record that reached no track names its reason
        // here, and a refused build is exactly when the drop counts are the question.
        // One line per part: the whole summary is wider than the log writer's line, and the half it
        // dropped was the netvar accounts.
        char tracks[kLogLineMax];
        for (size_t part = 0; part < kBuildSummaryParts; ++part) {
            FormatBuildSummary(g.build, part, tracks, sizeof tracks);
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "window tracks %zu/%zu (chapters %zu..%zu, %zu bodies, %zu players): %s",
                         part + 1, kBuildSummaryParts, g.build.first_chapter, g.build.last_chapter,
                         Win().BodyCount(), Win().PlayerCount(), tracks);
        }
    }
    if (!built) {
        g.window_valid = false;
        if (g.log) {
            g.log->Write(LogLevel::Error, kReplayModuleName, "window build refused: %s",
                         g.build.refusal ? g.build.refusal : "unstated");
        }
        return false;
    }
    g.window_centre = centre;
    g.window_valid = true;
    ++g.rebuilds;
    AdoptWindowTail(light);
    return true;
}

// Everything a window CHANGE owes the rest of the module, decoded here or decoded by the worker.
// Split out of RebuildWindow so the adopted path cannot drift from the synchronous one: there is
// one list of consequences and both callers run all of it.
void AdoptWindowTail(bool light) {
    // The cells were resolved against the window that just went away. Cleared here, re-published at
    // the current T by the tick's own publish; the sweep carries ownership across the gap. NOT a
    // forced redraw: a window roll is sequential playback crossing a chapter seam, and nothing
    // about it owes the widgets a value they are already showing.
    if (!light) {
        HudNetVarsInvalidate(false);
    }

    // ★ Once per load, on the first build that has the numbers. A file recorded before the netvar
    // stamp landed can still be watched and its clock still moves, but every netvar time in it is
    // this build's even spread over the chapter rather than a time the capture wrote down -- and a
    // viewer comparing two files has to be able to tell which one he is looking at. With the
    // numbers, because "interpolated" does not tell him how far off the HUD can be: a key with one
    // row in a chapter -- every score and state change is one -- is placed at that chapter's
    // midpoint, so its true instant is anywhere within half a chapter either side.
    // The flag survives a light build deliberately: the warning is once per load, and a drag must
    // not be the thing that consumes it.
    if (g.netvar_spread_warn_pending && !light) {
        g.netvar_spread_warn_pending = false;
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "this file carries no per-message netvar time (4-field netvar schema); "
                         "netvar rows are spread evenly across each chapter (widest chapter %.3f s, "
                         "%u rows placed). The error is PER KEY: the sparsest key here had %u row(s) "
                         "in a chapter and can be up to +/-%.3f s from when it was recorded, and %u "
                         "key(s) had a single row. A key the tap delivers every frame is placed to "
                         "within its own period",
                         double(g.build.netvar_spread_span_us) / 1e6,
                         unsigned(g.build.netvar_rows_spread),
                         unsigned(g.build.netvar_spread_sparsest_rows),
                         double(g.build.netvar_spread_worst_us) / 1e6,
                         unsigned(g.build.netvar_spread_one_row_keys));
        }
    }

    // audioresidue.h:99: "The pins the next stop-and-re-arm should restore. W2-B calls this when
    // the window resolves a keyframe". This is that moment, and it runs on every resolve -- so a
    // BACKWARDS seek re-pins from the keyframe it landed in rather than keeping the sustained loops
    // of the future. An absent block is counted, never turned into a claim of zero loops.
    if (!light) {
        PushAudioPinsFromBuild();
    }
    if (Win().NetVars().KeyCount() > 0) {
        g.netvar_records_seen = true;
    }
    // An unresolved entity answers every lookup with "absent"; leaving the handles unbound keeps
    // those bodies counted as unhandled, and the tick binds them the moment the surface resolves.
    // ★★★ THE BIND IS POSITIONAL, SO IT IS NOT DEFERRABLE. BuildWindowRange above constructs every
    // BodyTrack with handle_valid == false, and apply.cpp:481 skips an unbound track outright --
    // so a drag that deferred this to EndScrub left every PLAYER BODY with no com written for the
    // rest of the drag, while RetryDiscBind kept the disc moving and ApplyRosterMapping below kept
    // the PlayerCI pose (hands, HMD) published. That asymmetry is exactly what the owner saw and
    // what LAUNCH-QUALIFY-2100 G3 measured: disc and hands follow the drag, bodies and the minimap
    // dots they feed do not. Only the bind's LOGGING is light now.
    BindBodyHandlesIfReady(light ? "scrub step" : "window rebuilt", light);

    // entity.h:483, at the window's cadence. The resident span is the bound: entity.cpp:1768 says
    // per-CI growth "is bounded by the caller's TrimHistory window rather than by a magic number",
    // and this is the window S7.4 seeks inside without a rebuild. After the bind above, so the
    // handles it just resolved are the ones trimmed.
    if (!light) {
        g.history_trims +=
            g.applier.TrimHistories(Win(), Win().OuterBeginUs(), Win().OuterEndUs());
    }
    // ★★★ THE TRANSPORT STEPS OVER THIS WINDOW'S TICK TABLE, AND THE BUILD JUST REFILLED ITS
    // STORAGE IN PLACE. Without this the transport kept the count it was handed at load over a
    // different three chapters' boundaries: its index named a row of a table that no longer
    // existed. That is the mechanism behind L3-RUN-3 F1 -- sequential playback crossed chapter
    // seams by accident, and no seek could leave the load-time window at all.
    // Stated BEFORE the retable, so the first Advance after a rebuild already knows whether running
    // out of table in either direction is the end of the recording or a chapter still to decode.
    g.transport.SetTableEdges((int64_t)Win().OuterBeginUs() <= g.core_first_us,
                              (int64_t)Win().OuterEndUs() >= g.core_last_us);
    g.transport.Retable(Win().Ticks());
    // A rebuild constructs fresh PlayerTracks with user_id_valid == false. Without this the first
    // seek would silently un-own every player the load had just mapped.
    ApplyRosterMapping();
}

// ---------------------------------------------------------------------------------------------
// playback.async_window_decode: the boundary crossing that costs nothing.
//
// ★★★ THE SHAPE, in the order a frame runs it.
//   CollectPrefetch   takes a finished decode out of the worker, into `ready` or back to `spare`.
//   AdoptPrefetched   swaps `ready` into `cur` when the playhead wants that centre. Two pointer
//                     assignments and AdoptWindowTail; no decode, no allocation, no lock.
//   RequestPrefetch   posts the NEXT centre once the playhead is half way through the current one.
// With the key off not one of them does anything, and RebuildWindow is reached exactly as before.
// ---------------------------------------------------------------------------------------------

// How long Unload waits for the worker to come out of a decode. A three-chapter decode is 9-13 ms
// measured, so this is three orders of magnitude of headroom -- long enough that a timeout means
// something is genuinely wrong, short enough that a quit does not appear to hang.
constexpr unsigned kPrefetchJoinMs = 5000;

bool AsyncWindowsOn() { return g.async_window_decode && winprefetch::Running(); }

// A free buffer goes back to the worker as soon as there is one, so the request that follows is
// never refused for want of somewhere to decode into.
void OfferPrefetchBuffer() {
    if (winprefetch::HasBuffer() || !winprefetch::Running()) {
        return;
    }
    Window* w = PopFreeWindow();
    if (w == nullptr) {
        return;
    }
    if (!winprefetch::GiveBuffer(w)) {
        PushFreeWindow(w);
    }
}

// ★ WHAT MAKES A RESULT STALE, and both halves matter. A different `load_serial` is a window
// decoded for a file that is no longer open -- adopting it would sample another recording. A centre
// nobody wants any more is the direction-flip and seek-storm case: the material is sound, it is
// simply not where the playhead went, and keeping it would hold the only spare buffer hostage.
void CollectPrefetch() {
    winprefetch::Result r;
    if (!winprefetch::Collect(r)) {
        return;
    }
    const bool usable = r.built && r.window != nullptr && r.load_serial == g.load_serial;
    if (!usable) {
        ++g.prefetch_stale;
        PushFreeWindow(r.window);
        OfferPrefetchBuffer();
        return;
    }
    // A second result while one is already waiting cannot happen -- the worker holds one buffer --
    // but if `ready` is somehow occupied the OLDER one is the one to drop: the newer request is the
    // one the playhead asked for.
    if (g.ready != nullptr) {
        ++g.prefetch_stale;
        PushFreeWindow(g.ready);
    }
    g.ready = r.window;
    g.ready_centre = r.centre;
    g.ready_serial = r.load_serial;
    g.ready_build = r.stats;
    g.ready_since_us = WallNowUs();
    OfferPrefetchBuffer();
}

// The adoption. Returns false when nothing suitable is waiting, and the caller then falls back to
// the synchronous rebuild exactly as it always did.
bool AdoptPrefetched(int64_t t_us) {
    if (!AsyncWindowsOn() || g.ready == nullptr) {
        return false;
    }
    if (g.ready_serial != g.load_serial || g.ready_centre != ChapterForTime(g.reader, t_us) ||
        !g.ready->CoversForSampling(t_us)) {
        return false;
    }
    const uint64_t t0 = WallNowUs();
    // Released against the window that is going, exactly where RebuildWindow releases it and for
    // the same invariant: no synthetic-owner claim may outlive the tracks that name it.
    g.owner_releases += g.applier.ReleaseSyntheticOwners(
        Win(), StateApplier::OwnerRelease::KeepDiscLevel);

    // ★★ THE SWAP. `cur` becomes the free buffer and `ready` becomes current. Every borrower of a
    // pointer INTO a window is re-pointed by AdoptWindowTail below -- the transport's TickTable by
    // Retable, the disc track index by the re-bind -- and nothing else in this module holds one
    // across the window step of a tick.
    // ★ CAPTURED BEFORE THE SLOT IS CLEARED. Reading `ready_centre` after the clear set the centre
    // to SIZE_MAX on every adoption, which stopped RequestPrefetch dead at its own bounds guard --
    // one boundary adopted, then no prefetch ever again and the full decode at the coverage edge.
    const size_t adopted_centre = g.ready_centre;
    Window* going = g.cur;
    g.cur = g.ready;
    g.ready = nullptr;
    g.ready_centre = (size_t)-1;
    PushFreeWindow(going);
    g.build = g.ready_build;
    g.window_centre = adopted_centre;
    g.window_valid = true;
    ++g.rebuilds;
    ++g.adoptions;
    ++g.window_builds;
    AdoptWindowTail(false);
    OfferPrefetchBuffer();

    const uint64_t dt = WallNowUs() - t0;
    g.adopt_us_last = dt;
    if (dt > g.adopt_us_max) {
        g.adopt_us_max = dt;
    }
    return true;
}

// How much wall time a synchronous decode needs to still be an improvement on stalling at the
// seam. The decode is 9-13 ms measured; this is four of those, which is a frame at 20 fps.
constexpr uint64_t kSyncFallbackLeadUs = 50000;

// ★★ LOSING THE RACE GRACEFULLY. Adopt-or-count leaves exactly one outcome when the worker is
// genuinely slower than the playhead -- 4x, a cold disk, a chapter far bigger than the measured
// 9-13 ms: the module coasts to the outer edge and takes the full decode THERE, with the playhead
// already at the seam, which is strictly worse for the viewer than the synchronous re-centre the
// OFF path takes a whole chapter early. So the remaining coverage is measured in WALL time -- the
// only unit a decode cost can be compared against, and the one that does not change meaning
// between 0.1x and 4x -- and the tick decodes while the decode still fits inside it.
bool CoverageRunningOut(int64_t t_us, int dir) {
    const double rate = g.transport.Rate();
    const double speed = rate < 0.0 ? -rate : rate;
    if (dir == 0 || speed <= 0.0) {
        return false;
    }
    const int64_t edge = dir < 0 ? (int64_t)Win().OuterBeginUs() : (int64_t)Win().OuterEndUs();
    const int64_t left = dir < 0 ? t_us - edge : edge - t_us;
    if (left <= 0) {
        return true;
    }
    return (double)left / speed <= (double)kSyncFallbackLeadUs;
}

// ★★ THE THRESHOLD, and it is deliberately NOT RecentreMargin. RecentreMargin goes true when the
// playhead has already LEFT the centre chapter -- which is the instant the new window is wanted, so
// a request posted there would have no lead at all and the worker would lose every race. Half way
// through the centre chapter is one chapter of lead: a second at 1x, a quarter of a second at 4x,
// ten seconds at 0.1x, against a decode measured at 9-13 ms.
//
// The target is the next CORE chapter in the direction of travel. At either end of the file
// Prev/NextCoreChapter answer the centre itself, which is how this stops asking.
void RequestPrefetch(int64_t t_us) {
    if (!AsyncWindowsOn() || !g.window_valid || !g.loaded) {
        return;
    }
    if (g.window_centre >= g.reader.ChapterCount()) {
        return;
    }
    const double rate = g.transport.Rate();
    const int dir = g.transport.Paused() ? 0 : (rate < 0.0 ? -1 : (rate > 0.0 ? 1 : 0));
    if (dir == 0) {
        return;   // paused: no direction of travel, and nothing to warm towards
    }
    const ChapterKey ck = g.reader.ChapterAt(g.window_centre);
    if (ck.tEndUs <= ck.tBeginUs) {
        return;
    }
    const uint64_t half = ck.tBeginUs + (ck.tEndUs - ck.tBeginUs) / 2;
    size_t want = g.window_centre;
    if (dir > 0 && t_us >= (int64_t)half) {
        want = NextCoreChapter(g.reader, g.window_centre);
    } else if (dir < 0 && t_us <= (int64_t)half) {
        want = PrevCoreChapter(g.reader, g.window_centre);
    }
    if (want == g.window_centre) {
        return;
    }

    // A window already decoded for the wrong direction is discarded HERE rather than kept: it is
    // holding the only free buffer, and the one the playhead is travelling towards cannot be
    // requested until it lets go. This is the speed/direction-flip case.
    if (g.ready != nullptr && g.ready_centre != want) {
        ++g.prefetch_stale;
        PushFreeWindow(g.ready);
        g.ready = nullptr;
        g.ready_centre = (size_t)-1;
    }
    if (g.ready != nullptr) {
        return;   // already decoded and waiting: nothing to ask for
    }
    if (g.prefetch_posted_centre == want && g.prefetch_posted_serial == g.load_serial &&
        winprefetch::Busy()) {
        return;   // the worker is already on it
    }
    OfferPrefetchBuffer();
    if (winprefetch::Request(want, g.load_serial)) {
        g.prefetch_posted_centre = want;
        g.prefetch_posted_serial = g.load_serial;
    }
}

// ★ DESIGN-UI-2 S5.1's LOOKAHEAD HALF. Once both rebuild guards read CoversForSampling, nothing
// rebuilds until the playhead reaches the OUTER edge -- zero decoded material ahead, i.e. a stall
// guaranteed at the crossing, forwards and backwards. That is a regression, not a win, and this is
// what pays for it: on an idle tick, a whole chapter before the edge, the window re-centres and
// playback keeps strictly more lookahead than it had before the change. Never during a drag.
//
// The centre chapter not moving is the only anti-thrash guard needed: at either end of the file
// ChapterForTime answers the same chapter it already holds, so this cannot loop.
void RecentreWindow(const char* reason) {
    if (!g.loaded || !g.window_valid) {
        return;
    }
    const int64_t t = g.transport.DisplayPlayheadUs();
    if (ChapterForTime(g.reader, t) == g.window_centre) {
        return;
    }
    const int64_t from = (int64_t)Win().OuterBeginUs();
    const uint64_t t0 = WallNowUs();
    if (!RebuildWindow(t, false)) {
        return;
    }
    ++g.recentres;
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "[scrub] recentre from=%lld to=%lld reason=%s build_ms=%.3f",
                     (long long)(from - SourceOriginUs()),
                     (long long)((int64_t)Win().OuterBeginUs() - SourceOriginUs()), reason,
                     double(WallNowUs() - t0) / 1000.0);
    }
}

struct ClockBeats {
    uint64_t armed;
    uint64_t refused;
};

ClockBeats ReadClockBeats() {
    ClockBeats b = {0, 0};
    if (g.svc != nullptr && g.svc->clock != nullptr) {
        const ClockStatus cs = g.svc->clock->Status();
        b.armed = cs.stepsArmed;
        b.refused = cs.stepsRefusedBusy;
    }
    return b;
}

void EmitScrubStepLine() {
    Global::ScrubStepRecord& r = g.scrub_step;
    if (!r.armed) {
        return;
    }
    r.armed = false;
    // A drag is a bounded human action, so the limit is per drag rather than per second: capping
    // by time would thin exactly the sample AC-1.3's p95 is computed from.
    if (!g.log || g.scrub.lines >= Global::kScrubStepLinesMax) {
        ++g.scrub.lines_dropped;
        return;
    }
    ++g.scrub.lines;
    // ONE FRAME BEHIND, on purpose rather than by accident: the clock module mirrors and arms
    // after this module has ticked, so `beats=` counts the beats written since the PREVIOUS step
    // line -- i.e. the one this step's predecessor earned. Over a drag the totals are what matter,
    // and the settle line prints those.
    const ClockBeats beats = ReadClockBeats();
    const uint64_t since = beats.armed - g.scrub.beats_at_line;
    g.scrub.beats_at_line = beats.armed;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "[scrub] step target=%lld covered=%d seek_rebuilt=%d tick_rebuilt=%d refused=%d "
                 "skipped=%u beats=%llu bound=%zu build_ms=%.3f apply_ms=%.3f total_ms=%.3f",
                 (long long)r.target_us, r.covered ? 1 : 0, r.seek_builds != 0 ? 1 : 0,
                 r.tick_builds != 0 ? 1 : 0, r.refused ? 1 : 0, r.skipped,
                 (unsigned long long)since, g.bind.bound, r.build_ms, r.apply_ms,
                 r.issued_at_us ? double(WallNowUs() - r.issued_at_us) / 1000.0 : 0.0);
}

// The tick has five early returns between the window gate and the apply, and a refused window and
// an uncovered playhead are precisely the frames the line exists to show. So it leaves on the way
// out, whichever way out that is.
struct ScrubStepGuard {
    ~ScrubStepGuard() { EmitScrubStepLine(); }
};

// Names, profiles, loadouts and colours are W2-C's, applied once per loaded file. IIdentity writes
// into the same CR15NetGame the entity surface validates, so the roster is not dressed until W1-E
// has bound one: before that it is DEFERRED and counted, never dressed against a guess and never
// silently skipped. Called from Load and then from every tick while the gate is shut.
// The provider the dress ladder's step 1 has to be given, and the reason it lives HERE rather than
// beside PawnProvisioner: identity_access.h includes pawn_provision.h for JoinDesc, so the
// provisioner's own header cannot see IIdentity.  The load path is where both are complete.
//
// Identity::ProvisionReplayUser REFUSES an occupied recorded slot; the REMAP lives in
// PawnProvisioner.  A dresser handed the shipped singleton therefore dresses nobody on any lobby
// the viewer is already seated in -- which is every lobby.  Every other IIdentity call forwards
// unchanged: the singleton stays the one that talks to the engine.
//
// INNER MUST BE THE SHIPPED IDENTITY.  PawnProvisioner admits through ProvisionEnv::identity, so
// arming the provisioner with a ProvisionedIdentity would call back into here without end.
class ProvisionedIdentity final : public IIdentity {
public:
    ProvisionedIdentity(IIdentity& inner, PawnProvisioner& prov) : inner_(inner), prov_(prov) {}

    // The admission, remapped. `out` is the index the engine issued for the slot the provisioner
    // mapped this recorded slot to; the dresser then asks SlotOf(out) for where that landed, so
    // nothing here has to tell it a slot number it could assume wrongly.
    bool ProvisionReplayUser(UserSlot recorded, const JoinDesc& desc, UserIndex& out) override {
        UserSlot  slot(kSlotUnmapped);
        UserIndex index(kSlotUnmapped);
        if (!prov_.Admit(recorded, desc, slot, index)) return false;
        out = index;
        return true;
    }
    PawnProvision::Refusal LastProvisionRefusal() const override {
        return prov_.LastProvisionRefusal();
    }

    // THE HOP SURFACE IS THE INNER IDENTITY'S, and dropping this forward is silent: IIdentity
    // defaults ActorHops() to nullptr (identity_access.h:113) for the frozen fakes that predate
    // it, so a wrapper that forgets it answers "this build bound no hop read" for a build that
    // bound every one -- and a provisioner armed with that answer settles each entrant at the
    // admission and never drives a hop or a dress.  Pointer-identical, never a copy: `hops.ctx`
    // names the surface object itself.
#ifndef W2P1_MUTATE_ACTORHOPS_NOT_FORWARDED
    const PawnProvision::ActorHops* ActorHops() const override { return inner_.ActorHops(); }
#endif

    // THE ONE THE identity_access:: FREE FUNCTIONS HAVE TO SEE.  They key on the shipped
    // singleton's address; handed this wrapper they all answered "not mine", which is what made
    // SetSlotName refuse NoSlotNameTable and every dress failure print "None: none".
#ifndef W2P1_MUTATE_WRAPPER_HIDES_INNER
    const IIdentity* Underlying() const override { return inner_.Underlying(); }
#endif

    bool AddReplayUser(UserSlot s, UserIndex& out) override { return inner_.AddReplayUser(s, out); }
    bool ReleaseReplayUser(UserSlot s) override { return inner_.ReleaseReplayUser(s); }
    bool ResolveReplayUser(UserSlot s, UserIndex& out) override {
        return inner_.ResolveReplayUser(s, out);
    }
    bool SetProfileJson(UserSlot s, const Blob& json) override {
        return inner_.SetProfileJson(s, json);
    }
    bool SetTeam(UserIndex i, uint8_t team, uint8_t teamSlot) override {
        return inner_.SetTeam(i, team, teamSlot);
    }
    bool ApplyLoadout(UserIndex i, const Symbol64* slots, uint16_t number,
                      uint16_t numberBody) override {
        return inner_.ApplyLoadout(i, slots, number, numberBody);
    }
    bool OverrideAlignment(UserIndex i, uint16_t alignment) override {
        return inner_.OverrideAlignment(i, alignment);
    }
    bool ReadAppliedItems(UserIndex i, Symbol64* out) const override {
        return inner_.ReadAppliedItems(i, out);
    }
    UserSlot  SlotOf(UserIndex i) const override { return inner_.SlotOf(i); }
    UserIndex IndexOf(UserSlot s) const override { return inner_.IndexOf(s); }

private:
    IIdentity&       inner_;
    PawnProvisioner& prov_;
};

// The CR15NetGame the last tick carried. A function because ProvisionEnv takes one, and the
// value changes with the session while the arming does not.
void* TickNetGame() { return g.tick_net_game; }

// The state WORD the post-provision census reports.  This module has no reader for it; what it has
// is the gate's own answer, which is that same word tested against NETGAME.STATE_INMATCH.  So a
// session the gate passed reports the constant the gate just proved, and one it did not reports
// UNKNOWN rather than a number nobody read -- the census prints no `stateRaw=` line for that.
int32_t TickStateRaw() {
    return PawnProvision::SessionBlocker(TickNetGame()) == PawnProvision::Refusal::None
               ? int32_t(facts::NETGAME_STATE_INMATCH)
               : kCensusStateUnknown;
}

// The provisioner's own ledger, at the epoch edge -- once per dress pass, which is once per load
// and once per entity re-bind. INV-PP1's `adopted` is structurally 0 and unobservable until
// something prints it; `resolved` is asked of the entity surface per mapped slot rather than taken
// from `provisioned`, because an admitted entrant with no pawn is the failure this line exists to
// show. `class=none` when nothing resolved and `mixed` when two slots disagree -- one word for one
// class would report the first slot's answer as the roster's.
// `deferred` is ONE number with ONE meaning: provisioning attempts not yet made. The provisioner's
// own budget deferrals and the dress gate's session deferrals are both exactly that, and two fields
// would let a reader take either one for the whole.
// THE VIEWER'S OWN ROLE, from the engine's record and not from the arguments we believe we passed.
//
// Every playback run so far has spawned the viewer as a PLAYER, which is the owner's complaint.
// The engine decides that on entrant 0's team: CAM.SPECTATOR_TEAM_GATE reads the packed word at
// netGame+0x178 and CR15NetGame::Update enables the shipping spectator rig only for a team that
// gate accepts. With no -lobbyteam on the line CR15Game+GAME.TEAM_OFF stays -1, the admit path
// reads 0xFFFF and auto-picks a playing team, and the viewer gets hands and a HUD instead of a
// free camera.
//
// TWO READS, and the second one is no longer `lobbyteam=` (ARCH-23 S14.10, upheld). GAME.TEAM_OFF
// is reset to -1 before the level loads, so that instrument could only ever print -1 whatever was
// on the line: a field that cannot vary cannot corroborate anything, and it was read as evidence.
// It is DELETED. The two reads that remain are real ones and they are V3's: the record BEFORE the
// call and the record AFTER it, in viewer_role.cpp. The harness checks independently that
// `-lobbyteam 2` reached the rendered engine line, which is the reading this one pretended to be.
//
// The local-user count is the gate's OWN precondition, and it is what "readable" means here: at
// zero there is no entrant 0 to read and the word at netGame+0x178 is whatever was last there, so
// the line is withheld rather than reporting that as the viewer's role. Nothing below writes the
// engine's team -- that is set on the command line and by nothing else.
// One census window, matching W2-E-31c's rig-state line so the two readings of the same session
// land at the same cadence and a reader can pair them.
constexpr uint64_t kViewerCensusTicks = 60;

// ARCH-23 S14.10 V0..V4, driven from the stage-0 provider slot -- the first call of the frame,
// which is where P1-18 puts every authored engine transition and the only position from which the
// spectating arm's component teardown lands on an untorn frame (S14.10 F5).
//
// The hop surface comes off the identity every pump rather than being cached: the Identity that
// owns it re-binds it across a load (RebindHops), and a pointer we kept would name the surface of
// the object it was copied from. A null one refuses by name inside Pump.
//
// ★ AND IT IS ASKED THROUGH `ProvisionedIdentity`, which is the whole of AUD-S14 R1-F1 and the
// answer to AUD-W2-P1-30-part2 F7. The wrapper forwards fifteen methods; ActorHops() was the
// sixteenth, and a decorator that drops a method silently substitutes IIdentity's base
// `return nullptr` for the surface the inner object has. Arm() is handed the INNER identity by
// design (:1144, "the wrapper must never be armed"), so the seat is the forward's shipped
// consumer: ask the inner object here and the forward becomes dead code whose mutant only a unit
// test can see. The wrapper is two references on the stack -- it costs nothing to be the receiver
// the design says it is.
void SeatViewer(SeatScope scope) {
    // The identity is not even asked in a capture: the seat's own refusal is the whole of what a
    // non-playback session gets from this chain, and asking for a hop surface we may not use is
    // work the stage-0 slot does not owe.
    const PawnProvision::ActorHops* hops = nullptr;
    if (scope == SeatScope::Playback && g.svc && g.svc->identity) {
        ProvisionedIdentity receiver(*g.svc->identity, g.pawn);
        hops = static_cast<IIdentity&>(receiver).ActorHops();
    }
    g.viewer_seat.Pump(scope, TickNetGame(), hops, g.log);
}

bool MapRecSlotToLiveStun(void* ctx, uint16_t recSlot, stunfeed::LiveUser& out) {
    UserIndex index(0);
    if (!g.roster_map.Lookup(recSlot, index)) return false;
    if (!statsfeed::LiveNetUserId(ctx, index.v, out.net_user_id)) return false;
    if (!g.svc || !g.svc->identity) return false;
    ProvisionedIdentity receiver(*g.svc->identity, g.pawn);
    const PawnProvision::ActorHops* hops = static_cast<IIdentity&>(receiver).ActorHops();
    if (!hops || !hops->actorGuid) return false;
    // A guid the engine refuses to hand over is a user who has no actor yet, which is a WAIT and
    // never a guess: the stun feed counts the refusal and presents nothing for him.
    return hops->actorGuid(hops->ctx, index.v, &out.actor_lo, &out.actor_hi);
}

// The census is the seat's own verdict, so it is scoped exactly as the seat is (L2 #17).
// `ViewerNotSpectator` says "this session cannot free-fly", which is a defect in a playback and
// the CORRECT state of a capture -- printed in a capture it warns about the thing that was
// working, and read back later it makes a recording session look like a broken viewer one.
void LogViewerRole(SeatScope scope) {
    if (!g.log || scope != SeatScope::Playback) return;
    void* const ng = TickNetGame();
    if (ng == nullptr) return;
    const uint8_t* const local_users =
        static_cast<const uint8_t*>(ng) + facts::LOCAL_NETGAME_LOCAL_USER_COUNT_OFF;
    // ★ THE RATE LIMIT FIRST. `Readable` is a VirtualQuery, and under this engine's address-space
    // churn a VirtualQuery is expensive enough to see in a frame time (PERF-3). Probing above the
    // gate spent one on 59 of every 60 ticks to reach a `return`. The census refuses an unreadable
    // record either way, so the order is free.
    if (g.viewer_census_due_tick != 0 && g.ticks < g.viewer_census_due_tick) return;
    if (!Readable(local_users, sizeof(uint16_t))) return;
    if (*reinterpret_cast<const uint16_t volatile*>(local_users) == 0) return;
    g.viewer_census_due_tick = g.ticks + kViewerCensusTicks;

    const uint16_t role = identity_access::TeamOf(ng, UserIndex(0));
    // ★ NOT-YET-SEATED IS NOT A REFUSAL. CR15NetGame::Update reaches the
    // spectator-rig gate only at NETGAME.STATE_INMATCH, so before that state the engine has not
    // been asked for a seat and entrant 0 being on a playing team is not a verdict about one.
    // ViewerNotSpectator printed there made every correctly-seating session emit the acceptance's
    // own refusal word, and no passing run could satisfy the clause. The line still prints -- the
    // question is asked every window and answered every window -- but with the role RENDERED as
    // what it is: a reading taken before the seat was due.
    const bool in_match = SessionInMatch(ng);
#ifdef W2P1_MUTATE_PREMATCH_REFUSAL_PRINTED
    // The build F1 rejected: the pre-match reading rendered as a seat verdict, and the refusal
    // raised on a session that has not been asked for anything yet.
    g.log->Write(LogLevel::Info, kViewerModuleName, "viewer slot 0 role=%u (%s)", unsigned(role),
                 ViewerRoleName(role));
#else
    const uint32_t said = (uint32_t(role) << 1) | (in_match ? 1u : 0u);
    if (!g.viewer_role_seen || said != g.viewer_role_said ||
        log::ChatterDue(g.viewer_role_chatter_at)) {
        g.viewer_role_seen = true;
        g.viewer_role_said = said;
        g.log->Write(LogLevel::Info, kViewerModuleName, "viewer slot 0 role=%u (%s)", unsigned(role),
                     in_match ? ViewerRoleName(role) : "not yet seated");
    }
    if (!in_match) return;
#endif
    // The refusal is NAMED and on its own line: "the viewer is a player" is the failure, and a
    // reader scanning for it should not have to parse the line above to find it. Unconditional per
    // window, never a transition -- a verdict that prints only when it changes is silent on exactly
    // the session that was wrong from its first frame.
    if (!RoleSpectates(role)) {
        g.log->Write(LogLevel::Warn, kViewerModuleName,
                     "ViewerNotSpectator -- slot 0 is %s (role=%u), so CAM.SPECTATOR_TEAM_GATE is "
                     "false and the spectator camera stays disabled: this session cannot free-fly",
                     ViewerRoleName(role), unsigned(role));
    }
}

void LogPawnLedger(const char* waiting) {
    if (!g.log) return;
    const PawnLedger led = g.pawn.Ledger();
    const uint32_t entries = led.mapped < identity::RosterMapping::kSlots
                                 ? led.mapped
                                 : (uint32_t)identity::RosterMapping::kSlots;

    uint32_t resolved = 0;
    PawnClass agreed = PawnClass::Unknown;
    uint32_t agreed_ent = kEntityKeyUnknown;
    bool mixed = false;
    for (uint32_t i = 0; i < entries; ++i) {
        PawnRef ref{PlayerHandle(kPawnAbsent), PhysHandle(kPawnAbsent), kEntityKeyUnknown,
                    PawnClass::Unknown, false};
        if (!g.svc->entity || !g.pawn_census.ResolvePawn(*g.svc->entity, led.map[i].slot_resolved,
                                                         PawnClass::Unknown, ref)) {
            continue;
        }
        if (resolved && ref.cls != agreed) mixed = true;
        agreed = ref.cls;
        agreed_ent = ref.entity;
        ++resolved;
    }
    const char* cls = resolved == 0 ? "none" : mixed ? "mixed" : PawnClassName(agreed);
    // One resolved pawn has one entity key; two have two, and printing either would name the wrong
    // pawn, so the summary says it has none and the per-entry lines below carry them.
    char ent[kEntityKeyTextMax];
    FormatEntityKey(resolved == 1 ? agreed_ent : kEntityKeyUnknown, ent, sizeof ent);
    g.log->Write(LogLevel::Info, kPawnModuleName,
                 "provisioned %u, adopted %u, deferred %u, resolved %u (class=%s, ent=%s)%s%s",
                 led.provisioned, led.adopted, led.deferred + g.identity.deferred, resolved, cls,
                 ent, waiting ? " waiting: " : "", waiting ? waiting : "");
    // The reply route's own numbers, because `provisioned 0` has two very different causes on this
    // path: the engine never called our listener (pump=0 -- nobody was ever asked), or it did and
    // the publish or the seed refused (pump>0 with published=0). One line separates them.
    {
        const identity_access::ReplyRouteCensus rc = identity_access::ReplyRouteStatus();
        // `armed` FIRST and spelled separately from the provisioner's own arm: they are two arms
        // with two owners, and L3 #26 was read for a whole cycle as though `Armed()` meant this
        // one. `not tried` is a third answer -- no tick has carried a netGame yet -- because a
        // route that was never asked and a route that refused are not the same failure.
        g.log->Write(LogLevel::Info, kPawnModuleName,
                     "reply route: %s -- promoted=%s listeners=%u driverCalls=%u pump=%u queued=%u "
                     "drained=%u published=%u wrongphase=%u seedrefusals=%u (%s)",
                     !g.reply_route_tried ? "not tried"
                                          : g.reply_route_armed ? "armed" : "not armed",
                     rc.promoted ? "yes" : "no", unsigned(rc.listeners), unsigned(rc.driverCalls),
                     unsigned(rc.pumpCallbacks), unsigned(rc.queued), unsigned(rc.drained),
                     unsigned(rc.published), unsigned(rc.wrongPhase), unsigned(rc.seedRefusals),
                     identity_access::RefusalText(rc.lastSeed));
    }
    for (uint32_t i = 0; i < entries; ++i) {
        PawnRef ref{PlayerHandle(kPawnAbsent), PhysHandle(kPawnAbsent), kEntityKeyUnknown,
                    PawnClass::Unknown, false};
        const bool ok = g.svc->entity && g.pawn_census.ResolvePawn(*g.svc->entity,
                                                                   led.map[i].slot_resolved,
                                                                   PawnClass::Unknown, ref);
        char rent[kEntityKeyTextMax];
        FormatEntityKey(ok ? ref.entity : kEntityKeyUnknown, rent, sizeof rent);
        g.log->Write(LogLevel::Info, kPawnModuleName,
                     "recorded slot %u -> slot %u (class=%s, ent=%s)",
                     unsigned(led.map[i].slot_recorded), unsigned(led.map[i].slot_resolved),
                     ok ? PawnClassName(ref.cls) : "none", rent);
    }
}

// ★ ARMING IS THE LOAD'S, NOT THE DRESS PASS'S (L3 #26 F1).  This block used to sit BELOW the
// dress gate in TryDressRoster, so the provisioner was armed only on a tick that had already
// cleared NoNetGame, SessionNotInMatch and "entity surface not bound to a game space" -- and
// `Tick` pumps only what is armed.  RUN-26 never cleared that gate: the dresser's last deferral is
// stamped 23:15:58.994, the engine seated slot 1 at 23:16:03, and in between the pump -- the only
// thing that re-reads the entrant record, prints the window line and walks the actor hops -- had
// never once been entered.  The session's state is what the PUMP is there to report on; it cannot
// also be the reason there is no pump.  So the arm keys on the env the provisioner actually needs
// and on nothing else, and `Armed()` means exactly "provisioning is armed" -- not "the roster was
// dressed", and not "the reply route arm succeeded", which is one PATH into this table and has its
// own state on its own line.
// WHAT THE FILE SAYS THIS ENTRANT WAS WEARING, handed to the dress as a slot and not as a call:
// PawnProvisioner lives in src/engine, which links nothing of src/playback, and the dress has to be
// exercisable with no game present. The source is re-asked on EVERY call rather than captured at
// arm time -- it is the same singleton LogRecordedLoadouts prints and the tick moves its horizon,
// so a mid-match loadout swap is answered from the instant being played, and a backward seek
// answers the older block again. `recordedSlot` is the RECORDED user slot, which is the index
// ReadRecordedLoadout keys on; the provisioner drives with the recorded slot, never the live one.
bool RecordedLoadoutForProvision(uint32_t recordedSlot, identity::RecordedLoadout& out,
                                 const char** refusal) {
    return identity::ReadRecordedLoadout(identity::PlaybackRecordedIdentity(), recordedSlot, out,
                                         refusal);
}

void ArmProvisioner() {
#ifdef W2P1_MUTATE_ARM_BELOW_DRESS_GATE
    // L3 #26's arm, restored: armed only once the DRESS pass's own deferral gate has cleared, so a
    // load that spends its first seconds waiting for the level to come up never pumps at all.
    if (!g.loaded || g.svc == nullptr || g.svc->entity == nullptr || !g.svc->entity->Available() ||
        PawnProvision::SessionBlocker(TickNetGame()) != PawnProvision::Refusal::None) {
        return;
    }
#endif
    if (g.pawn.Armed() || !g.loaded || g.svc == nullptr || g.svc->identity == nullptr ||
        g.svc->entity == nullptr) {
        return;
    }
    ProvisionEnv env;
    env.identity = g.svc->identity;   // the SHIPPED identity: the wrapper must never be armed
    env.netGame = &TickNetGame;
    // The census's three inputs.  Unset, Pump censuses nothing at all and the run cannot say
    // which surface holds the pawn it provisioned (L3 #12's `w2p-pawns-provisioned`
    // could-not-ask).  The entity surface is taken by POINTER, not by Available(): W1-E's entity
    // is a process-lifetime singleton whose availability comes and goes with the game space, and
    // waiting for it to be available is the deferral that kept the pump from existing.
    env.entity   = g.svc->entity;
    env.log      = g.log;
    env.stateRaw = &TickStateRaw;
    // The loadout's recording side. `loadoutEngine` stays null on purpose: null there means "bind
    // this image at its recorded VAs", which is what a launch wants, while null HERE would mean the
    // dress can only ever answer RecordingNotAsked.
#ifndef W2P1_MUTATE_RECORDED_LOADOUT_PROVIDER_UNWIRED
    env.recordedLoadout = &RecordedLoadoutForProvision;
#endif
    // ENG-NATIVE-ADMIT (echo-101): on the engine's own remote reply route (pawn.bot_body OFF),
    // nothing fills the per-user loadout SOURCE the add-entrant handler bridges into the live table,
    // so the entrant is admitted over a zero-length instance array and the framework's phase-16
    // model-build deadlocks.  Pre-fill that SOURCE from the recording before admission.  Coupled to
    // !bot_body so the shipped bot-body path -- which fills its own SOURCE and has no bridge on its
    // direct insert -- is unaffected; default-on within the remote route, A/B off with
    // ECHOREPLAY_PAWN_REMOTE_SOURCE_LOADOUT=0.
    env.sourceLoadoutBeforeAdmit =
        config::Interface()->GetBool("pawn.remote_source_loadout", true) &&
        !config::Interface()->GetBool("pawn.bot_body", false);
    // ENG-BOTROUTE-FIX (Change A): on the bot body-build route, carry the identity-block loadout
    // document (the bytes the remote route parses to a non-empty loadout) into the admission so
    // HookSourceLoadout's SOURCE parse no longer feeds the engine parser the manifest entrantJson that
    // parses to zero.  Coupled to the SAME config that wires HookSourceLoadout in the identity layer
    // (pawn.bot_body_source_loadout && pawn.bot_body), so the shipped default (either flag off) never
    // fetches or carries it and the admission flow is byte-for-byte unchanged.
    env.botBodySourceLoadout =
        config::Interface()->GetBool("pawn.bot_body_source_loadout", false) &&
        config::Interface()->GetBool("pawn.bot_body", false);
    // Author a profile document for an entrant the capture recorded none for, so the
    // engine has something to build a loadout instance from.  Read here rather than taken from
    // g.dress_loadout because the arm does not depend on a dress pass having run first; the key is
    // the same one ArmDressPass reads (kDressLoadoutKey), and it is the whole switch: off, nothing
    // is authored and the admission is byte-for-byte what it was.
    env.authorLoadoutDocument = config::Interface()->GetBool("playback.dress_loadout", true);

    // ENG-EQUIPMENT-RECV (DIAG-MODEL-COMPONENT-LEVER): on the engine's own remote reply route
    // (pawn.bot_body OFF), drive the engine's R15NetPlayerEquipmentUpdate RECV with the recorded chassis so
    // the engine installs the net-model component and the framework's phase-16 rebuild seats a visible body.
    // Coupled to !bot_body so the bot-body route never fabricates a RECV; default ON.
    env.equipmentRecv =
        config::Interface()->GetBool("pawn.equipment_recv", true) &&
        !config::Interface()->GetBool("pawn.bot_body", false);
    // ENG-SPAWNED-SEED backstop: same coupling -- it is a lever of the reply route only.
    env.participateBackstop =
        config::Interface()->GetBool("pawn.participate_backstop", false) &&
        !config::Interface()->GetBool("pawn.bot_body", false);
    g.pawn.Arm(env);
    // THE HOP SURFACE, RE-ASKED AND READ BACK ON EVERY ARM.  The bind itself is Arm's own
    // (pawn_provision.cpp:1209 takes it from this same `env.identity`), so this module does not
    // hold a second copy of one fact; what it holds is the READING, because the alternative is
    // silent by construction: Settle takes an entrant with no surface straight to Provisioned and
    // never enters AdvanceSlot -- the only place a hop is advanced or a loadout is dressed from --
    // and the census then prints `provisioned N` about pawns nothing ever enabled, placed or
    // dressed.  A build that bound no hop says so BY NAME here, on the arm, before any slot exists
    // for the pawn census to report on.
    if (g.log != nullptr && g.pawn.Env().hops == nullptr) {
        g.log->Write(LogLevel::Warn, kPawnModuleName,
                     "hops: HopsUnbound -- the identity answered no actor-hop surface, so every "
                     "admitted slot settles at the admission: no enable, no place, no physics CI "
                     "and no loadout dress will be driven for any entrant this load");
    }
}

// What the FILE says each entrant was wearing, once per load, under the viewer's own module name so
// a grep for [identity] finds it beside the roles. Three states are printed as three words: a count
// of UNKNOWN is a capture that could not ask, and it must not read as zero pairs.
void LogRecordedLoadouts() {
    if (!g.log) return;
    const identity::RecordedIdentitySource& src = identity::PlaybackRecordedIdentity();
    if (src.SlotCount() == 0) {
        // The scan's own line, not a restatement of two of its counters: no entrant slot can mean
        // the file carried no identity chapter, or that one was there and every row refused, and
        // W3-E's census is the only reading that tells those apart (rows, tap chapters, tap index,
        // offered/decoded/refused and the per-version breakdown).
        g.log->Write(LogLevel::Info, kViewerModuleName, "%s", src.CensusLine());
        return;
    }
    for (uint16_t i = 0; i < src.SlotCount(); ++i) {
        const uint32_t slot = src.SlotAt(i);
        identity::RecordedLoadout rec;
        const char* why = nullptr;
        if (!identity::ReadRecordedLoadout(src, slot, rec, &why)) {
            g.log->Write(LogLevel::Warn, kViewerModuleName, "recorded loadout slot %u: refused: %s",
                         slot, why ? why : "unstated");
            continue;
        }
        char pairs[16];
        if (rec.filledPairCount == identity::kRecordedPairCountUnknown) {
            _snprintf_s(pairs, sizeof pairs, _TRUNCATE, "UNKNOWN");
        } else {
            _snprintf_s(pairs, sizeof pairs, _TRUNCATE, "%u", (unsigned)rec.filledPairCount);
        }
        const char* profile = rec.profile == identity::RecordedProfileState::Present  ? "present"
                              : rec.profile == identity::RecordedProfileState::Absent ? "absent"
                              : rec.profile == identity::RecordedProfileState::Refused
                                  ? "refused"
                                  : "not asked";
        const identity::RecordedIdentityEntry* e = src.Current(slot);
        g.log->Write(LogLevel::Info, kViewerModuleName,
                     "recorded loadout slot %u: v%u pairs %s/%u keys %s profile %s",
                     slot, e ? (unsigned)e->version : 0u, pairs,
                     (unsigned)identity::kRecordedLoadoutPairCap, rec.hasSlotKeys ? "yes" : "no",
                     profile);
    }
}

// THE RETRY CADENCE.  A dress pass is not free: for every undressed slot it re-asks the engine to
// admit the entrant and, once that lands, writes a profile, a slot name, a team, twenty item ids,
// two numbers and a colour, and makes the engine re-apply the loadout to the actor.  Its cost is
// not measurable offline -- every one of those calls is an engine call -- so it is PACED rather
// than run per frame: 4 Hz covers the engine's admission pump, which drains in milliseconds, with
// two orders of magnitude of margin, and costs 4 passes a second instead of 90.  It also stops
// entirely once nothing is deferred, so the steady state is zero.
constexpr uint64_t kDressRetryPeriodMs = 250;
// Both numbers below are timeouts, never gates: a harness sets them to zero to drive a retry and a
// give-up inside one test run, and no value of either lets the dress write anything it could not
// write at the shipped values.
constexpr const char* kDressRetryKey = "playback.dress_retry_ms";
constexpr const char* kDressKey = "playback.dress";
constexpr const char* kDressLoadoutKey = "playback.dress_loadout";
// Default ON: the leave is what empties the arena between captures. Off is the escape hatch --
// adoption, which is what every build before the leave path did.
constexpr const char* kLeaveOnUnloadKey = "playback.leave_on_unload";

// ...and the retry is BOUNDED.  A deferral that never ends is a defect, and the way the last one
// survived a nine-minute session was silence.  Thirty seconds of CONTINUOUS deferral -- the clock
// restarts whenever a slot stops deferring -- then one line per still-deferred player, by name and
// by reason, and the pass stops asking.
constexpr uint64_t kDressDeferralBoundMs = 30000;
constexpr const char* kDressBoundKey = "playback.dress_deferral_bound_ms";

// Read once per load, not per tick: a config lookup is a string walk and this runs a few times a
// second for as long as the engine keeps deferring.
bool ConfigBool(const char* key, bool dflt) {
    const IConfig* cfg = g.svc ? g.svc->config : nullptr;
    return cfg ? cfg->GetBool(key, dflt) : dflt;
}

uint64_t ConfigMs(const char* key, uint64_t dflt) {
    const IConfig* cfg = g.svc ? g.svc->config : nullptr;
    if (!cfg) return dflt;
    const int64_t v = cfg->GetInt(key, (int64_t)dflt);
    return v < 0 ? dflt : (uint64_t)v;
}

// Every player the engine is still deferring, named, with the refusal that names why.  This is the
// end of the retry and it is the loudest thing this module says: past here those players keep the
// engine's placeholder name and number for the rest of the file.
void ReportDressGiveUp(const identity::RosterReport& report, uint64_t waited_ms) {
    const Manifest& m = g.reader.GetManifest();
    for (uint32_t i = 0; i < report.deferred; ++i) {
        const uint16_t slot = report.deferredSlots[i].userSlot;
        const char* name = "(not in the manifest)";
        for (uint32_t p = 0; p < m.playerCount; ++p) {
            if (m.players[p].userSlot == slot) {
                name = m.players[p].displayName;
                break;
            }
        }
        g.log->Write(LogLevel::Error, kReplayModuleName,
                     "identity dressing GIVEN UP for player '%s' slot %u after %llu s of continuous "
                     "deferral: %s -- this player keeps the engine's placeholder name and number "
                     "for the rest of this file",
                     name, unsigned(slot), (unsigned long long)(waited_ms / 1000),  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
                     PawnProvision::RefusalText(report.deferredSlots[i].why));
    }
    g.log->Write(LogLevel::Error, kReplayModuleName,
                 "identity dressing stopped retrying: %u of %u player(s) were still deferred after "
                 "%llu s and %u pass(es); %u are dressed",
                 report.deferred, report.players, (unsigned long long)(waited_ms / 1000),  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
                 g.identity.passes, report.dressed);
}

// A load, or an entity re-bind: everything the retry remembers about the roster it was dressing is
// about a set of user indices that no longer means anything.
// WHAT THE DRESS READS OUT OF EACH ROW, hashed: the slot it dresses, the team that decides
// alignment and spectator-ness, and the name it writes. FNV-1a over at most a few short rows, no
// allocation and no engine call, so it can run on the tick that asks whether the roster moved.
uint64_t RosterDigest(const Manifest& m) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const uint32_t rows = m.playerCount;
    mix(&rows, sizeof rows);
    for (uint32_t i = 0; i < m.playerCount; ++i) {
        const PlayerRecord& p = m.players[i];
        mix(&p.userSlot, sizeof p.userSlot);
        mix(&p.teamId16, sizeof p.teamId16);
        mix(&p.teamId, sizeof p.teamId);
        mix(p.displayName, strnlen(p.displayName, sizeof p.displayName));
    }
    return h;
}

void ArmDressPass(uint32_t players, bool keepDressed = false) {
    g.identity = IdentityStatus();
    g.identity.pending = true;
    g.identity.players = players;
    if (keepDressed) {
        g.roster_map.ArmDressPassKeepingApplied();
    } else {
        g.roster_map.ArmDressPass();
    }
    g.dress_next_ms = 0;
    g.dress_defer_since_ms = 0;
    // Starts at 1, never 0: NextDressAction reads a zero `since_ms` as "the wait has not started",
    // so a clock whose own zero were a real instant would restart the bound on every pass.
    g.dress_clock_ms = 1;
    g.dress_clock_last_ms = 0;
    g.shut_since_ms = 0;
    g.shut_escalated = false;
    g.dress_retry_ms = ConfigMs(kDressRetryKey, kDressRetryPeriodMs);
    g.dress_bound_ms = ConfigMs(kDressBoundKey, kDressDeferralBoundMs);
    g.dress_enabled = ConfigBool(kDressKey, true) &&
                      livesafety::Allowed(livesafety::Feature::Dress);
    g.dress_loadout = ConfigBool(kDressLoadoutKey, true);
    identity::SetLeaveOnUnload(ConfigBool(kLeaveOnUnloadKey, true));
    identity::SetLeaveOrder(ConfigBool("playback.leave_reverse_order", true),
                            ConfigBool("playback.leave_one_per_pass", true));
}

// GIVING UP IS NOT TERMINAL.  The pass stops asking so it cannot spend the session retrying, but
// every event that changes the question re-asks it: a new file, a seek, a roster change, or the
// viewer saying so.  Without this a dress that gave up while the level was still coming up stayed
// given up for the rest of the launch, with the players wearing the engine's placeholders.
bool ReArmDressAfterGiveUp(const char* why) {
    if (!g.loaded || !g.identity.gave_up) return false;
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "identity dressing re-armed (%s): the roster is dressed again from scratch",
                     why);
    }
    ArmDressPass(g.identity.players);
    return true;
}

// The identity block's own loadout document -- the bytes the engine's remote route parses to one
// real loadout instance, and the only profile document most captured rows have: the manifest's
// entrant JSON is optional and usually absent.  Re-asked per pass rather than captured, because
// the recorded-identity horizon moves with the playhead.
bool RecordedProfileDocument(uint16_t recordedSlot, Blob& out) {
    identity::RecordedLoadout rl;
    if (!identity::ReadRecordedLoadout(identity::PlaybackRecordedIdentity(), recordedSlot, rl,
                                       nullptr))
        return false;
    if (rl.document == nullptr || rl.documentBytes == 0) return false;
    out = Blob{reinterpret_cast<const uint8_t*>(rl.document), rl.documentBytes};
    return true;
}

// The leave feed drains on the engine's own update. This is how long a load waits for it before
// giving up and adopting instead: three seconds is hundreds of engine frames, and an engine that has
// not produced one in that time is not about to produce eight.
constexpr uint64_t kLeaveWaitMs = 3000;

bool LeaveFeedStillBlocking() {
    if (!identity::RosterLeavePending()) return false;
    const uint64_t now = GetTickCount64();
    if (g.leave_wait_since_ms == 0) g.leave_wait_since_ms = now;
    if (now - g.leave_wait_since_ms < kLeaveWaitMs) return true;
    if (!g.leave_wait_said) {
        g.leave_wait_said = true;
        identity::ForgetRosterLeave();
        if (g.log)
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "the previous load's replay entrants have not left the engine after %llu ms "
                         "-- the engine update that feeds the removals has not run. The queue is "
                         "dropped and this load re-binds to them instead, as it did before the leave "
                         "path existed",
                         (unsigned long long)kLeaveWaitMs);
    }
    return false;
}

// RELOAD-6. The previous capture's entrants are freed by the leave, but everything the engine
// attached to them -- the heraldry nametag actor and its number/decal/border/pip, and their script
// graphs -- lives until the game space is torn down. So a reload leaves the level and enters it
// again through the stock offline-match entry, and nothing of the new load touches the engine
// until the fresh space is in game. Called with the reload's own Unload already done.
void ArmLevelCycle(bool reloading) {
    ISessionEntry* const s = g.svc ? g.svc->session : nullptr;
    if (!reloading || s == nullptr) return;
    if (!ConfigBool("playback.reload_via_level_cycle", true)) {
        if (g.log)
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "reload: in place (playback.reload_via_level_cycle is off): the previous "
                         "roster's attached nametags and script graphs stay in this level");
        return;
    }
    // The load funnel asks for a level change itself when this capture is on another level; that
    // entry IS the cycle, so it is waited out rather than asked for twice.
    const bool entryQueued = s->EntryPending();
    const int32_t state = static_cast<int32_t>(s->State());
    if (!entryQueued && state != reloadcycle::kStateInGame) {
        if (g.log)
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "reload: no level cycle -- NetGame is at state %d, not in game, so this "
                         "load waits for the level like a first load",
                         state);
        return;
    }
    ++g.reload_cycles;
    g.reload_cycle.Arm(GetTickCount64(), entryQueued);
    ForgetLevelHandles();
    if (g.log)
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "reload: level cycle #%u armed (%s): admissions and engine writes for this "
                     "load are held until the level has been left and entered again",
                     g.reload_cycles,
                     entryQueued ? "the load funnel already asked for this capture's level"
                                 : "after the previous roster has left");
}

// The space the previous load bound into is gone, or may be: every fact taken from it is dropped
// so the first ticks in the new space derive them again, as a first load does.
void ForgetLevelSpaceFacts() {
    g.session = SessionWatch{};
    g.viewer_seat.Reset();
    if (g.svc && g.svc->entity) g.svc->entity->DropDisc();
    g.disc_track = Global::kNoDiscTrack;
    g.disc_probe_pending = g.disc_drive_on;
    g.bind = BindResult();
    g.slot_map_n = 0;
    ReleaseLodThrottle();
    identity_access::ForgetEntrantActorClass();
}

void FinishLevelCycle(uint64_t now) {
    const reloadcycle::Machine& m = g.reload_cycle;
    const reloadcycle::Outcome o = m.outcome();
    const bool leftLevel = m.LeftLevel();
    if (leftLevel) ForgetLevelSpaceFacts();
    ForgetLevelHandles();
    if (!g.log) return;
    const double totalS = double(m.ElapsedMs(now)) / 1000.0;
    if (o == reloadcycle::Outcome::Cycled) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "reload: level cycle #%u done in %.1f s (out of the level after %.1f s): the "
                     "new replay is admitted into a fresh level space, with its pre-roll",
                     g.reload_cycles, totalS, double(m.ExitAtMs() - m.ArmedAtMs()) / 1000.0);
        return;
    }
    if (o == reloadcycle::Outcome::Cancelled) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "reload: level cycle #%u cancelled after %.1f s (the replay was unloaded)",
                     g.reload_cycles, totalS);
        return;
    }
    ISessionEntry* const s = g.svc ? g.svc->session : nullptr;
    const char* why = s != nullptr ? s->FailureReason() : nullptr;
    g.log->Write(LogLevel::Error, kReplayModuleName,
                 "reload: level cycle #%u FAILED (%s) after %.1f s at NetGame state %d; the "
                 "session's last word: %s. %s",
                 g.reload_cycles, reloadcycle::OutcomeName(o), totalS, m.LastState(),
                 why != nullptr && why[0] != 0 ? why : "none",
                 leftLevel ? "The level was left: this load admits when the session is in game "
                             "again"
                           : "The replay opens in the level as it is, and the previous roster's "
                             "attached nametags may remain, as before RELOAD-6");
}

// True while the cycle holds this load. Performs the one engine request the machine asks for.
bool HoldForLevelCycle() {
    reloadcycle::Machine& m = g.reload_cycle;
    if (!m.Holds()) return false;
    ISessionEntry* const s = g.svc ? g.svc->session : nullptr;
    const uint64_t now = GetTickCount64();
    const reloadcycle::Phase before = m.phase();
    const reloadcycle::Observation o{now, identity::RosterLeavePending(),
                                     s != nullptr ? static_cast<int32_t>(s->State()) : -1};
    if (m.Step(o) == reloadcycle::Action::Request && s != nullptr) {
        const EntryRefusal r = s->RequestLevelCycle();
        m.Answer(r == EntryRefusal::Accepted        ? reloadcycle::RequestAnswer::Accepted
                 : r == EntryRefusal::NotInLevelYet ? reloadcycle::RequestAnswer::NotYet
                                                    : reloadcycle::RequestAnswer::Refused,
                 now);
    }
    if (m.phase() != before && m.Holds() && g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "reload: level cycle #%u %s -> %s at +%llu ms (NetGame state %d)",
                     g.reload_cycles, reloadcycle::PhaseName(before),
                     reloadcycle::PhaseName(m.phase()), (unsigned long long)m.ElapsedMs(now),
                     o.netGameState);
    }
    if (m.FinishedNow()) FinishLevelCycle(now);
    return m.Holds();
}

void TryDressRoster() {
    if (!g.identity.pending || !g.loaded) {
        return;
    }
    // playback.dress off: no pass runs at all, so no entrant is admitted by the dresser and no
    // identity field is written. Admission through the provisioner's own pump and the body drive
    // are untouched -- this switch exists so a launch that crashes can be repeated without the one
    // subsystem that writes another user's identity.
    if (!g.dress_enabled) {
        // ★ AND IT CLEARS `pending`, because ArmDressPass set it before anyone asked whether the
        // dresser was on. Left latched it is a promise nothing below will ever keep: the pre-roll's
        // Waiting gate (UpdatePreRoll) and the panel's dressing state both wait on it, so
        // playback.dress=false -- the switch a crashing launch is repeated with -- used to buy a
        // 40 s dead wait and a "PRE-ROLL GAVE UP" that blamed the disc.
        g.identity.pending = false;
        g.identity.entrant_deferred = 0;
        return;
    }
    // ★ LIVE SAFETY, and it is first because everything below it WRITES ENGINE STATE: the dress
    // admits entrants and overwrites another user's name, profile, team, loadout and colour.  In a
    // live online match those are the host's to own and ours to record, never to author -- so the
    // pass runs only where the offline bit is locked, which is every replay launch and no capture
    // of a live session (bootstrap.cpp: the lock is applied unless IsLiveOnlineExempt).  It is a
    // deferral rather than a hard stop for the same reason the others are: the lock is applied
    // early in the launch and a Load that raced it is not a Load that should be refused forever.
    const bool contained = ProcessContained();

    // Before the deferral gate below, never after it: Identity::ProvisionReplayUser refuses an
    // occupied recorded slot and the remap lives in PawnProvisioner, so a dresser handed the
    // shipped singleton dresses nobody -- the viewer holds slot 0 on every live lobby -- and the
    // in-Load dress pass runs before any tick has called ArmProvisioner.  Armed once per LOAD: an
    // entity re-bind must find this mapping still here, or the re-dress asks again for a slot its
    // own first pass filled and is refused.
    ArmProvisioner();
    // NoNetGame and SessionNotInMatch are DEFERRALS, not refusals -- PawnProvision::Refusal says so
    // in as many words. This pass first runs inside Load, before any tick has carried a netGame, and
    // it clears `pending` unconditionally: read as refusals they failed the roster PERMANENTLY on
    // every load, and the dresser dressed nobody. So they join the three the gate already holds --
    // the pass retries on later ticks, the provisioner's slots stay Pending, and a pass still
    // waiting when the file closes is reported by name in Unload.
    const PawnProvision::Refusal session = PawnProvision::SessionBlocker(TickNetGame());
    const char* shut = !contained ? "the process is not contained (the offline bit is not locked); "
                                    "dressing writes another user's identity and must never run in "
                                    "a live session"
                       : !g.svc->identity               ? "no IIdentity published"
                       : !g.svc->entity                 ? "no IEntity published"
                       : !g.svc->entity->Available()    ? "entity surface not bound to a game space"
                       // Dressing into a slot whose entrant is mid-teardown would adopt something
                       // about to be destroyed, so the previous load's entrants finish leaving
                       // first. BOUNDED: the feed drains from the engine's own update, and an
                       // engine that has stopped updating -- a match that ended between the unload
                       // and the load -- would otherwise hold every later load off the roster for
                       // the rest of the session. Past the bound the dress runs and adopts, which
                       // is the same fallback a refused leave takes.
                       : LeaveFeedStillBlocking()
                             ? "the previous load's replay entrants are still leaving the engine"
                       : g.reload_cycle.Holds()
                             ? "the level is being left and entered again for this load (RELOAD-6)"
                       : session != PawnProvision::Refusal::None
                             ? PawnProvision::RefusalText(session)
                                                        : nullptr;
    if (shut) {
        ++g.identity.deferred;
        const uint64_t shut_now = GetTickCount64();
        // Once per distinct blocker, not once per tick: waiting out a level load is normal and
        // lasts thousands of frames, and a per-frame line would bury the one that says it ended.
        if (shut != g.identity.defer_why) {
            g.identity.defer_why = shut;
            g.shut_since_ms = shut_now;
            g.shut_escalated = false;
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "identity dressing deferred: %s; the roster will be dressed when it is",
                         shut);
            LogPawnLedger(shut);
        } else if (!g.shut_escalated && g.dress_bound_ms != 0 &&
                   shut_now - g.shut_since_ms >= g.dress_bound_ms) {
            // ...and BOUNDED, because one Warn and then silence is how the last one survived a
            // nine-minute session.  The pass keeps asking -- this blocker is the one thing that can
            // end on its own -- but it says so once, loudly, at the same bound the dress uses.
            g.shut_escalated = true;
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "identity dressing has been deferred on the SAME blocker for %llu s and "
                         "no player has been dressed: %s. The pass keeps asking; every name, "
                         "number, team and loadout stays at the engine's placeholder until it "
                         "clears",
                         (unsigned long long)((shut_now - g.shut_since_ms) / 1000), shut);  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
            LogPawnLedger(shut);
        }
        return;
    }

    const Manifest& m = g.reader.GetManifest();
    // The recorded roster changed under us -- a mid-file join or leave republished the manifest --
    // so the slots this load already dressed are answers to a different question.
    //
    // ★ COMPARED AGAINST A DIGEST OF THE SAME ROWS, NOT AGAINST A COUNT OF A DIFFERENT SET
    // This used to test `m.playerCount != g.identity.players`, and
    // `g.identity.players` is the DRESSABLE count -- spectators removed -- so on any capture with a
    // spectator row the test was true on every pass forever: every frame wiped the latches, reset
    // the pacing and the progress clock, and re-ran the whole ladder for every player.  A row count
    // would compare like with like and still miss the change that matters most, five rows out and
    // five different rows in, so the subject is the rows themselves.
    const uint64_t digest = RosterDigest(m);
#ifdef NAMES3_MUTATE_ROSTER_COUNT_COMPARE
    if (g.identity.passes != 0 && m.playerCount != g.identity.players) {
#else
    if (g.identity.passes != 0 && digest != g.identity.roster_digest) {
#endif
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "the recorded roster changed mid-file (%u row(s) now, %u dressable last pass); "
                     "the dress pass is re-armed and every slot is dressed again",
                     (unsigned)m.playerCount, g.identity.players);
        ArmDressPass((uint32_t)m.playerCount);
    }

    const uint64_t now = GetTickCount64();
    // ★ THE PROGRESS CLOCK, and the bound below is measured on it rather than on the wall.  It only
    // runs between two passes that both got past the gate above -- shut means the engine cannot
    // publish at all -- and it stops while playback is paused, because the admission is published
    // by the engine update this module paces.  A viewer who loads a file and leaves it paused at T0
    // used to burn the whole thirty seconds and get a give-up line for a dress nothing had refused.
    // ★ AND A PRE-ROLL IS PROGRESS. The playhead is pinned on purpose while the engine clock runs
    // the host forward -- the engine update this pass waits on is being paced faster, not stopped --
    // so a clock stopped by `Paused()` here would give the dress its full budget of frozen frames.
    if (g.dress_clock_last_ms != 0 && now > g.dress_clock_last_ms &&
        (!g.transport.Paused() || PreRollActive()))
        g.dress_clock_ms += now - g.dress_clock_last_ms;
    g.dress_clock_last_ms = now;

    const bool retry = g.identity.passes != 0;
    // Paced, not per-frame: see kDressRetryPeriodMs. The FIRST pass is never paced -- it runs
    // inside Load, and the whole point of it is to dress the roster before the first frame.
    if (retry && now < g.dress_next_ms) {
        return;
    }
    g.dress_next_ms = now + g.dress_retry_ms;

    ProvisionedIdentity admitting(*g.svc->identity, g.pawn);

    const uint32_t deferred_before = g.identity.entrant_deferred;
    identity::RosterReport report;
    // The profile document the attach step needs.  Wired here because the recording is src/playback's
    // and the dresser links none of it.
    identity::DressEnv denv;
#ifndef W2P1_MUTATE_DRESS_DOCUMENT_UNWIRED
    denv.recordedDocument = &RecordedProfileDocument;
#endif
    denv.applyLoadout = g.dress_loadout;
    identity::DressRoster(admitting, *g.log, m.players, m.playerCount, report, g.roster_map,
                          retry ? identity::DressPass::Retry : identity::DressPass::First, denv);
    ++g.identity.passes;
    g.identity.defer_why = nullptr;
    // The rows THIS pass answered, so the next one compares against them and not against a count
    // of a different set. Written on every pass, including the first: a digest left at zero would
    // make the second pass report a change nothing made.
    g.identity.roster_digest = digest;
    g.identity.players = report.players;
    g.identity.dressed = report.dressed;
    g.identity.failed = report.failed;
    g.identity.entrant_deferred = report.deferred;

    // ★ THE DEFECT, IN ONE STATEMENT.  This used to be `g.identity.pending = false;`
    // unconditionally.  Every slot of the subject capture refused its admission with
    // ReplyWrongPhase -- which the provisioner's own header calls a DEFERRAL, not a failure --
    // the flag was cleared anyway, and the per-tick driver that calls this function every frame
    // no-opped for the next nine minutes.  The queue drained milliseconds later, the engine
    // admitted all five players, and nothing ever wrote their name, number, team or loadout:
    // `engine held 0` of 20 recorded rows.
    switch (identity::NextDressAction(report.deferred, deferred_before, !retry, g.dress_clock_ms,
                                      g.dress_bound_ms, g.dress_defer_since_ms)) {
        case identity::DressAction::Settled:
            g.identity.pending = false;
            break;
        case identity::DressAction::KeepWaiting:
            break;
        case identity::DressAction::GiveUp:
            ReportDressGiveUp(report, g.dress_clock_ms - g.dress_defer_since_ms);
            g.identity.pending = false;
            g.identity.gave_up = true;
            break;
    }

    LogPawnLedger(nullptr);
    ApplyRosterMapping();
    // The window was bound before this pass ran, so that bind asked the RECORDED slot -- the
    // viewer's own on every live lobby. The mapping exists now; the bind is re-taken through it.
    if (RefreshSlotMap()) BindBodyHandlesIfReady("slot map installed");
    // Once, when the roster finally settles -- not on every pass that got past the session gate.
    if (!g.identity.pending && g.identity.deferred) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "identity dressing ran after %u deferred tick(s)", g.identity.deferred);
    }
}

// The applier's pointers are taken from ReplayServices every tick, not once at Init. W2-A publishes
// INetVar from its first tick, after every Init has run, so the Init-time binding held null for the
// life of the process and every netvar write counted refused (S9.14).
void RebindServices() {
    IEntity* const had_entity = g.applier.Entity();
    const bool pointers_changed = g.applier.Rebind(g.svc->entity, g.svc->pose, g.svc->netVar);
    const bool available = g.svc->entity && g.svc->entity->Available();
    const bool availability_changed = available != g.entity_available;
    g.entity_available = available;

    // ★ WHAT ACTUALLY MARKS A REBIND (AUDIT_WIRING_A F-A). Neither term beside it can: W1-E's
    // entity is a process-lifetime singleton, so the POINTER only ever changes when a test
    // substitutes a fake, and its Tick re-binds a changed space by calling Bind() with no Unbind,
    // so a space rebuilt inside one engine call reads AVAILABLE before and after. The epoch
    // advances on exactly the edge that invalidates every index, registration and handle we hold.
    // The other two are kept beside it: they are free, and an entity answering NotAvailable --
    // any implementation older than the epoch -- still has to be handled by something.
    uint64_t epoch = 0;
    const IEntity::BindEpoch bind_state =
        g.svc->entity ? g.svc->entity->BindEpochNow(epoch) : IEntity::BindEpoch::NotAvailable;
    const bool epoch_changed = bind_state != g.entity_bind_state || epoch != g.entity_bind_epoch;
    g.entity_bind_state = bind_state;
    g.entity_bind_epoch = epoch;

    if (!pointers_changed && !availability_changed && !epoch_changed) {
        return;
    }
    ++g.rebinds;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "services re-bound: entity %s (%s, bind epoch %llu%s), pose %s, netvar %s",
                 g.svc->entity ? "live" : "null", available ? "available" : "unavailable",
                 (unsigned long long)epoch,
                 bind_state == IEntity::BindEpoch::Ok        ? ""
                 : bind_state == IEntity::BindEpoch::Unbound ? " unbound"
                                                             : " not tracked",
                 g.svc->pose ? "live" : "null", g.svc->netVar ? "live" : "null");
    const bool entity_changed = epoch_changed || g.svc->entity != had_entity || availability_changed;
    // ★ A new game space is a new CR15NetGame, and every user index we hold was issued by the old
    // one. Keeping them would publish recorded poses onto whatever entrants happen to occupy those
    // indices now -- the wrong-avatar failure the two index spaces exist to prevent, arriving from
    // the one direction the load-time gate cannot see. So the mapping is dropped and the roster is
    // re-dressed; re-dressing is idempotent and costs one pass, holding a stale index costs a
    // silently wrong replay.
    if (entity_changed && g.loaded) {
        g.roster_map.Clear();
        // Re-ARMED, not merely re-derived. OwnerGuard::entrants_ holds every index admitted under
        // the previous game space and RegisterEntrant is idempotent, so without this the registry
        // becomes the union of two sessions' indices and INV-OWN1 would admit one the new space
        // never issued. The local user id is re-read here for the same reason, and because a Load
        // that happened before W1-E bound anything had no answer to arm against at all.
        ArmOwnerGuard();
        ApplyRosterMapping();
        ArmDressPass((uint32_t)g.reader.GetManifest().playerCount);
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "entity surface re-bound: the roster's user indices belonged to the previous "
                     "game space and have been dropped; re-dressing");
        TryDressRoster();
    }
    if (entity_changed) {
        BindBodyHandlesIfReady("entity re-bound");
    }
}

// ---------------------------------------------------------------------------------------------

class ReplayTransportImpl final : public IReplayTransport {
public:
    bool Load(const wchar_t* path) override {
        const bool reloading = g.loaded;
        Unload();
        g.refusal[0] = 0;

        // ★ RECORDER-2 part A. A demo loaded in a live session is the one event that would arm
        // every playback write path at once, so the load is refused BY NAME before the file is even
        // opened. Each of those paths still carries its own gate; this is the front door.
        if (!livesafety::Allowed(livesafety::Feature::ReplayLoad)) {
            _snprintf_s(g.refusal, sizeof g.refusal, _TRUNCATE,
                        "load refused: %s. Playback writes engine state and this process is not a "
                        "contained offline session",
                        livesafety::VerdictReason(livesafety::Current()));
            if (g.log) {
                g.log->Write(LogLevel::Error, kReplayModuleName, "%s", g.refusal);
            }
            return false;
        }

        if (!g.reader.Open(path)) {
            const char* why = g.reader.RefusalReason();
            _snprintf_s(g.refusal, sizeof g.refusal, _TRUNCATE, "open refused: %s",
                        why ? why : "unstated");
            if (g.log) {
                g.log->Write(LogLevel::Error, kReplayModuleName, "%s", g.refusal);
            }
            return false;
        }

        const Manifest& m = g.reader.GetManifest();

        // ★ E-P8. An unknown format_major is REFUSED and named. A reader that guessed would parse a
        // future layout as the current one and render plausible nonsense.
        if (m.formatMajor != kFormatMajor) {
            _snprintf_s(g.refusal, sizeof g.refusal, _TRUNCATE,
                        "format_major %u is not readable by this build (expects %u)",
                        (unsigned)m.formatMajor, (unsigned)kFormatMajor);
            if (g.log) {
                g.log->Write(LogLevel::Error, kReplayModuleName, "%s", g.refusal);
            }
            g.reader.Close();
            return false;
        }

        // ★★ E-P7. A build mismatch WARNS and loads; it does not refuse, because the owner may well
        // want to watch a file recorded against another build and be told what differs. Today this
        // check did not exist at all, so such a file loaded silently and every wrong-looking avatar
        // was unexplained. It is surfaced in ActiveModeLabel as well as the log, because a warning
        // only in a log file is a warning the person watching never sees.
        g.build_checked = false;
        g.build_mismatch = false;
        g.content_mismatch = false;
        if (const char* expect = ::replay::ExpectedMd5()) {
            char have[33];
            for (int i = 0; i < 16; ++i) {
                _snprintf_s(have + i * 2, 3, _TRUNCATE, "%02x", m.echovrMd5[i]);
            }
            have[32] = 0;
            g.build_checked = true;
            g.build_mismatch = _stricmp(have, expect) != 0;
            if (g.build_mismatch && g.log) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "this file was recorded against echovr.exe md5 %s; this build guards %s. "
                             "Cosmetics and content ids may not resolve.",
                             have, expect);
            }
        }

        // ★ Once per load, not per window build. A file recorded before the netvar stamp landed can
        // still be watched and its clock still moves, but every netvar time in it is this build's
        // even spread over the chapter rather than a time the capture wrote down -- and a viewer
        // comparing two files has to be able to tell which one he is looking at.
        // Owed, not written: the line quotes the widest chapter and the worst placement error, and
        // no window has been built yet, so writing it here prints 0.000 s over 0 rows. RebuildWindow
        // pays it on the first build that has measured them.
        g.netvar_spread_warn_pending = !NetvarStampsPresent(m);

        g.loaded = true;
        // The other half of Unload's SuspendReplyDrainBody. The driver's only install site runs
        // once per session (`reply_route_tried`), so without this a second load left the detour
        // passing straight through: driverCalls froze, the reply route never published, and every
        // entrant of the reload stayed deferred at ReplyWrongPhase. Resume never creates a hook.
        identity_access::ResumeReplyDrainBody();
        ++g.load_serial;
        g.session = SessionWatch{};
        g.disc_probe_pending = g.disc_drive_on;
        g.disc_track = Global::kNoDiscTrack;
        g.disc_retry_accum_us = 0;
        g.disc_fresh_accum_us = 0;
        g.disc_logged_verdict = BindResult::DiscVerdict::NotAsked;
        g.disc_logged_mask = 0;
        g.disc_verdict_repeats = 0;
        g.disc_early_refusal_logged = nullptr;
        g.disc_early_digest_valid = false;
        g.disc_early_change_pending = false;
        g.disc_early_changes_held = 0;
        // The first evaluation of a load always prints: it is the one the rate limit must not eat.
        g.disc_early_quiet_us = kUsPerSecond;
        // Once per load, at load. A seek later in the session does not buy a second one.
        // The three knobs are read HERE for the reason the dress timeouts are (ConfigMs): a
        // harness that could not shorten the bound could only test the give-up by waiting it out.
        g.preroll = Global::kPreRollUnstarted;
        g.preroll_wall_us = 0;
        g.preroll_run_us = 0;
        g.preroll_wait_sim_us = 0;
        g.preroll_fast_wall_us = 0;
        g.preroll_fast_sim_us = 0;
        g.preroll_ramp_wall_us = 0;
        g.preroll_ramp_sim_us = 0;
        g.preroll_prev_sim_us = 0;
        g.preroll_ramped = false;
        g.preroll_bound_disc_logged = false;
        g.disc_host_held = false;
        g.preroll_play_requested = false;
        // The host's phase is read out of HOST-NV, which only keeps a value for a symbol somebody
        // asked it to keep. Asked once per load; the detours are installed long before this.
        netvarguard::WatchHostSymbol(gamestate::facts::kVarGameState.symbol);
        // The pre-roll drives the engine clock fast over a span of real
        // match time; it was protected only by sitting downstream of ReplayLoad. It asks for
        // itself here, which is what makes its startup row true.
        g.preroll_enabled = livesafety::Allowed(livesafety::Feature::PreRoll) &&
                            (g.svc && g.svc->config
                                 ? g.svc->config->GetBool("playback.preroll", true)
                                 : true);
        {
            const double rate =
                g.svc && g.svc->config ? g.svc->config->GetDouble("playback.preroll_rate", 2.0)
                                       : 2.0;
            // Under 1x it would run the host SLOWER than playing does, and the engine discards
            // whatever a step above its own 66666 us clamp asks for -- so 8 is already generous.
            g.preroll_rate = (float)(rate < kPreRollScaleMin
                                         ? kPreRollScaleMin
                                         : (rate > kPreRollScaleMax ? kPreRollScaleMax : rate));
            const double scale = g.svc && g.svc->config
                                     ? g.svc->config->GetDouble("playback.preroll_clock_scale", 4.0)
                                     : 4.0;
            g.preroll_scale = (float)(scale < kPreRollScaleMin
                                          ? kPreRollScaleMin
                                          : (scale > kPreRollScaleMax ? kPreRollScaleMax : scale));
            // 65 s of host time -- 45 at the pre-match scale, 20 at the launch rate -- plus up to
            // 8 s of admission grace before the fast clock starts. At the 4x/2x defaults that is
            // 21.3 s of wall, and the bound stays where PREROLL-2 put it: it is a give-up, not a
            // schedule, and leaving slack in it costs a run that reaches its exit nothing.
            g.preroll_bound_us = ConfigMs("playback.preroll_bound_ms", 55000) * 1000ull;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        }
        if (g.log) {
            // Four conditions can refuse this, and a bare "off" sent the last launch hunting the
            // wrong one. Name the first that refused, in the order UpdatePreRoll tests them.
            const char* why = "";
            if (!g.preroll_enabled) {
                why = " (playback.preroll is off)";
            } else if (!g.disc_drive_on && !ProcessContained()) {
                why = " (this process is not contained, so the disc drive is off)";
            } else if (!g.disc_drive_on && g.svc && g.svc->config &&
                       !g.svc->config->GetBool("playback.disc_drive", true)) {
                why = " (playback.disc_drive is off)";
            } else if (!g.disc_drive_on) {
                why = " (the host score gate did not install, so the disc drive is off)";
            } else if (!g.svc || !g.svc->clock) {
                why = " (no engine clock service)";
            }
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "pre-roll %s%s at %.2fx then %.2fx, bound %.1f s: the host's ~%.0f s of "
                         "pre-match and launch is run out on the engine clock with the playhead "
                         "held (expect ~%.1f s of real time), so the recorded disc is driven from "
                         "the first second watched",
                         (g.preroll_enabled && g.disc_drive_on && g.svc && g.svc->clock) ? "ON"
                                                                                         : "off",
                         why, (double)g.preroll_scale, (double)g.preroll_rate,
                         double(g.preroll_bound_us) / 1e6, kHostToPlayingS,
                         PreRollExpectedWallS());
        }
        g.disc_early_bound_logged = false;
        g.disc_early_agree_logged = false;
        g.disc_early_symbols_logged = false;
        g.disc_rules_hops_n = 0;
        g.disc_rules_hop_last = nullptr;
        g.disc_rules_hop_changes = 0;
        ArmGameStateNetVars();
        g.as_seen = false;
        ArmOwnerGuard();


        // ★ SIZE THE WINDOW BEFORE THE FIRST BUILD, AND REFUSE HERE IF IT CANNOT BE SIZED. A file
        // whose chapters are deeper than the pool can hold used to load, look fine, and then refuse
        // every seek into a full part of the match with nothing said at the seek but a playhead that
        // did not move. The owner learns at LOAD instead, by name.
        uint64_t spanUs = 0, periodUs = 0;
        if (!MeasureCadence(g.reader, spanUs, periodUs)) {
            _snprintf_s(g.refusal, sizeof g.refusal, _TRUNCATE,
                        "this capture's first %u core chapters carry no ticks, so the playback "
                        "window cannot be sized for it",
                        (unsigned)kSizingProbeChapters);
            if (g.log) {
                g.log->Write(LogLevel::Error, kReplayModuleName, "%s", g.refusal);
            }
            g.loaded = false;
            g.reader.Close();
            return false;
        }
        g.tick_spacing_us = periodUs <= (uint64_t)INT64_MAX ? (int64_t)periodUs : 0;

        // Walks every chapter, so it runs here with the other load-time scans and never on a
        // rebuild. A refusal is a chapter that does not decode, which no window could be built
        // across either.
        const char* scan_why = "unstated";
        if (!ScanTrackSources(g.reader, g.track_scan, &scan_why)) {
            _snprintf_s(g.refusal, sizeof g.refusal, _TRUNCATE, "track scan: %s", scan_why);
            if (g.log) {
                g.log->Write(LogLevel::Error, kReplayModuleName, "%s", g.refusal);
            }
            g.loaded = false;
            g.reader.Close();
            return false;
        }
        LogWireClocks();

        // Read once per load, above the sizing that acts on it. Default OFF: this path has never
        // been driven in game, and the key is what an operator flips to drive it.
        g.async_window_decode =
            g.svc && g.svc->config && g.svc->config->GetBool("playback.async_window_decode", false);
        // ★ AND ONE TIMED-OUT JOIN ENDS IT FOR THE PROCESS. A worker abandoned mid-decode is still
        // writing into memory this module gave up naming; starting a second one, or sizing a
        // window beside it, is how a leak becomes a corruption. Said once, as a WARN, because an
        // operator who turned the key on deserves to know why it stopped answering.
        if (g.async_window_decode && g.prefetch_abandoned) {
            g.async_window_decode = false;
            if (g.log && !g.prefetch_abandoned_said) {
                g.prefetch_abandoned_said = true;
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "async window decode is off for the rest of this process: an earlier "
                             "unload left a prefetch worker running and its windows abandoned");
            }
        }

        if (periodUs != 0) {
            const WindowCaps want = CapsForScan(CapsForCadence(spanUs, periodUs), g.track_scan);
            const char* why = "unstated";
            if (!Win().Reserve(want, &why)) {
                _snprintf_s(g.refusal, sizeof g.refusal, _TRUNCATE,
                            "%s (chapters of %llu us at one tick per %llu us need %zu body samples; "
                            "the ceiling is %zu)",
                            why, (unsigned long long)spanUs, (unsigned long long)periodUs,
                            want.max_body_samples, kWindowSampleCeiling);
                if (g.log) {
                    g.log->Write(LogLevel::Error, kReplayModuleName, "%s", g.refusal);
                }
                g.loaded = false;
                g.reader.Close();
                return false;
            }

            // ★★ THE OTHER TWO SLOTS ARE ALLOCATED ONLY WITH THE KEY ON, and that is the whole of
            // "the synchronous path is untouched when OFF": no second reader, no worker thread, no
            // second and third window, not one byte of resident memory that the build before this
            // packet did not also hold. Sized with the SAME caps as the current window, because a
            // buffer the worker could not fill to the same depth would refuse at the boundary
            // instead of at load, which is exactly the failure the block above exists to prevent.
            if (g.async_window_decode) {
                for (size_t i = 1; i < 3; ++i) {
                    // Freshly constructed, never reused: a slot that survived a previous load is
                    // one this module could not prove nobody else is writing into, and Reserve's
                    // first act on a reused slot is to delete[] its pools.
                    g.win_store[i].reset(new (std::nothrow) Window());
                    const char* slot_why = "unstated";
                    if (!g.win_store[i] || !g.win_store[i]->Reserve(want, &slot_why)) {
                        // Not a refusal of the LOAD. The file is readable and the synchronous path
                        // can play it; only the prefetch is unavailable, and saying so by name
                        // beats a load the owner cannot explain.
                        g.win_store[1].reset();
                        g.win_store[2].reset();
                        g.free_win[0] = g.free_win[1] = nullptr;
                        g.async_window_decode = false;
                        if (g.log) {
                            g.log->Write(LogLevel::Warn, kReplayModuleName,
                                         "async window decode is off for this file: a prefetch "
                                         "window could not be sized (%s)", slot_why);
                        }
                        break;
                    }
                    g.free_win[i - 1] = g.win_store[i].get();
                }
            }
        }

        // The playhead starts at the file's origin, and the first window is built there so the panel
        // has something to show before anything is played.
        const int64_t t0 = (int64_t)m.t0Us;
        g.core_first_us = t0;
        g.core_last_us = CoreEndUs(g.reader, t0);
        // ★ THE SPAN THE TIMELINE IS DRAWN OVER COMES FROM THE TICKS, NOT THE CHAPTER TABLE.
        // CoreBeginUs answers the first core chapter's start, and on a `partial_start` capture that
        // chapter begins at the manifest origin and runs for minutes before its single first tick
        // (LAUNCH-UI3 §6: 871 s of axis for 48.5 s of content). The track scan has already walked
        // every core chapter; its tick span is where the owner can actually go.
        g.core_data_first_us = g.track_scan.have_tick_span ? g.track_scan.first_tick_us
                                                           : CoreBeginUs(g.reader, t0);
        g.core_data_last_us = g.track_scan.have_tick_span && g.track_scan.last_tick_us > g.core_data_first_us
                                  ? g.track_scan.last_tick_us
                                  : g.core_last_us;
        g.span_source = g.track_scan.have_tick_span ? "ticks" : "chapters";
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "playable span: file=[0..%lld] playable=[%lld..%lld] us (source=%s)",
                         (long long)(g.core_last_us - t0), (long long)(g.core_data_first_us - t0),
                         (long long)(g.core_data_last_us - t0), g.span_source);
        }

        // ★ THE RECORDED IDENTITY, OFF DISK, BEFORE ANYTHING DRESSES ANYONE. Blocks are written ON
        // CHANGE, so an entrant's defining block sits wherever it changed -- usually near the match
        // start and outside whatever window the playhead is in. A per-window fill would therefore
        // answer "not recorded" for every seek past the change, so the file is scanned once here and
        // the playhead selects out of it. Before RebuildWindow, because a DecodedChapter's pointers
        // survive exactly one further DecodeChapter and this walks every chapter in the file.
        // Before the first window, and for the same reason the identity scan is: a key's opening
        // value lives wherever the recorder first wrote it, which is almost never in the three
        // chapters the playhead starts in. One walk of the file, once per load.
        ScanNetVarPrimer(g.reader, g.netvar_primer);
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "netvar primer: %zu key(s) from %u keyframe(s) over %u CORE chapter(s) -- "
                         "the opening value of a key no window carries",
                         g.netvar_primer.count, g.netvar_primer.keyframes,
                         g.netvar_primer.chapters);
        }

        identity::ScanRecordedIdentity(g.reader, identity::PlaybackRecordedIdentity());
        identity::PlaybackRecordedIdentity().SetHorizonUs((uint64_t)t0);
        LogRecordedLoadouts();

        // Same one-walk-per-load reason as the two scans above, and one more: a stat row is keyed
        // by a slot, not by a chapter, and the newest row at or before the playhead is almost never
        // in the chapters the playhead starts in.
        statsfeed::ScanCensus& sc = statsfeed::PlaybackStatsCensus();
        statsfeed::ScanRecordedStats(g.reader, statsfeed::PlaybackRecordedStats(), sc);
        if (g.log) {
            // NO MAPPED COUNT HERE. The roster is dressed seconds after this line, so a census
            // taken now can only ever say zero -- which is what LAUNCH-STATS2 read as a defect.
            // The feed reports what it can actually reach, from the sweep that reaches it.
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "stats: %u player rows / %u team rows decoded from "
                         "%u S_EVENT row(s) over %u chapter(s) with a tap table; taps player=%u "
                         "team=%u; %u refused, %u dropped%s%s. Rows carry the tap's ARRIVAL stamp, "
                         "which is not the playhead's axis, so %u of them are SPREAD evenly inside "
                         "the chapter that holds them -- an interpolation, bounded by the chapter",
                         sc.player_rows, sc.team_rows, sc.event_rows, sc.tap_chapters,
                         unsigned(sc.player_tap), unsigned(sc.team_tap), sc.refused_rows,
                         sc.dropped_rows, sc.first_drop ? ": " : "",
                         sc.first_drop ? sc.first_drop : "", sc.spread_rows);
        }
        statsfeed::SetPlayheadOriginUs(t0);
        statsfeed::Arm(g.stats_on);

        // The goal rows, for the same one-walk-per-load reason: a goal is an EDGE whose row sits
        // wherever it happened, which is almost never in the chapters a load opens in.
        goalfeed::ScanCensus& gc = goalfeed::PlaybackGoalCensus();
        goalfeed::ScanRecordedGoals(g.reader, goalfeed::PlaybackRecordedGoals(), gc);
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "goals: %u rows decoded from %u S_EVENT row(s) over %u chapter(s) with a "
                         "tap table; tap=%u; %u refused, %u dropped%s%s; %u placed inside their "
                         "chapter from the arrival stamps around them (an interpolation, bounded by "
                         "the chapter). Feed %s (playback.goal_events), horn %s "
                         "(playback.goal_horn), explosion %s (playback.goal_explosion)",
                         gc.rows, gc.event_rows, gc.tap_chapters, unsigned(gc.tap), gc.refused_rows,
                         gc.dropped_rows, gc.first_drop ? ": " : "",
                         gc.first_drop ? gc.first_drop : "", gc.placed_rows,
                         g.goals_on ? "ON" : "OFF", goalfeed::HornEnabled() ? "ON" : "off",
                         goalfeed::ExplosionEnabled() ? "ON" : "off");
        }
        goalfeed::SetPlayheadOriginUs(t0);
        goalfeed::Arm(g.goals_on);
        BuildShellPlan();

        // The stun rows, same one-walk-per-load. A stun is an edge AND a two-second span, so the
        // scan also has to be complete before the first seek: a file opened inside a stun has to
        // know which one it is inside of.
        stunfeed::ScanCensus& stc = stunfeed::PlaybackStunCensus();
        stunfeed::ScanRecordedStuns(g.reader, stunfeed::PlaybackRecordedStuns(), stc);
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "stuns: %u rows decoded from %u S_EVENT row(s) over %u chapter(s) with a "
                         "tap table; tap=%u; %u refused, %u dropped%s%s; %u placed inside their "
                         "chapter from the arrival stamps around them (an interpolation, bounded by "
                         "the chapter). Feed %s (playback.stun_events)",
                         stc.rows, stc.event_rows, stc.tap_chapters, unsigned(stc.tap), stc.refused_rows,
                         stc.dropped_rows, stc.first_drop ? ": " : "",
                         stc.first_drop ? stc.first_drop : "", stc.placed_rows,
                         g.stuns_on ? "ON" : "OFF");
        }
        stunfeed::SetPlayheadOriginUs(t0);
        stunfeed::Arm(g.stuns_on);

        // The component-event rows, on the same one-walk-per-load. Only the allow-listed pairs are
        // stored; everything else is tallied per (script, event) so a later widening of the list is
        // argued from this capture's own inventory rather than from what a script is named.
        componentfeed::ScanCensus& cc = componentfeed::PlaybackEventCensus();
        componentfeed::ScanRecordedEvents(g.reader, componentfeed::PlaybackRecordedEvents(), cc);
        if (g.log) {
            const uint32_t inSpan = componentfeed::PlaybackRecordedEvents().CountInSpan(
                g.core_data_first_us, g.core_data_last_us);
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "component events: %u rows, %u allow-listed (%u inside the playable span) "
                         "-- from %u S_EVENT row(s) over %u chapter(s) with a tap table; tap=%u; %u "
                         "off the list, %u refused, %u dropped%s%s, %u unpaired; %u placed inside "
                         "their chapter from the arrival stamps around them. Feed %s "
                         "(playback.component_events)",
                         cc.component_rows, cc.rows, inSpan, cc.event_rows, cc.tap_chapters,
                         unsigned(cc.tap), cc.denied_rows, cc.refused_rows, cc.dropped_rows,
                         cc.first_drop ? ": " : "", cc.first_drop ? cc.first_drop : "",
                         cc.unpaired_rows, cc.placed_rows,
                         g.component_events_on ? "ON" : "OFF");
            componentfeed::LogPairCensus(cc);
        }
        componentfeed::SetPlayheadOriginUs(t0);
        componentfeed::Arm(g.component_events_on);

        if (!RebuildWindow(t0)) {
            _snprintf_s(g.refusal, sizeof g.refusal, _TRUNCATE, "first window: %s",
                        g.build.refusal ? g.build.refusal : "unstated");
            g.loaded = false;
            g.reader.Close();
            return false;
        }
        TransportTuning tune;
        g.transport.Reset(Win().Ticks(), tune);
        // After Reset, which clears any previous file's span. The transport clamps the DISPLAY
        // playhead into this and nothing wider: the end of the resident table is not the end of the
        // recording, and only this span can tell the two apart.
        g.transport.SetFileSpan(g.core_first_us, g.core_last_us);
        g.transport.SeekTo(t0);
        int64_t f = 0, t = 0;
        Cause c = Cause::Seek;
        g.transport.TakeDiscontinuity(f, t, c);   // consume the load's own discontinuity
        g.transport.SetPaused(true);

        ArmDressPass((uint32_t)m.playerCount);
        ForgetLevelHandles();
        ArmLevelCycle(reloading);
        TryDressRoster();

        // ★ STARTED AFTER THE FIRST WINDOW IS BUILT AND THE LOAD CANNOT STILL REFUSE, so there is
        // no path where a worker outlives a load that did not happen. Its reader is its own, opened
        // on the same path; the clocks and the primer it borrows are load-time scans that never
        // change while a file is open.
        if (g.async_window_decode) {
            const char* why = "unstated";
            if (!winprefetch::Start(path, &g.track_scan.clocks, &g.netvar_primer, &why)) {
                g.async_window_decode = false;
                if (g.log) {
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "async window decode is off for this file: %s", why);
                }
            } else {
                OfferPrefetchBuffer();
                if (g.log) {
                    g.log->Write(LogLevel::Info, kReplayModuleName,
                                 "async window decode is ON: the next window is decoded on a worker "
                                 "thread, half a chapter ahead of the boundary. Watch `window "
                                 "async:` -- tick_decodes/min must be ~0 in steady playback and one "
                                 "per seek");
                }
            }
        }

        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "loaded: %u chapters, %u players, duration %llu us%s",
                         (unsigned)g.reader.ChapterCount(), (unsigned)m.playerCount,
                         (unsigned long long)DurationUs(),
                         g.reader.WasRecovered() ? " (recovered)" : "");
            // L3-RUN-7 F4: the panel painted the netvar fidelity state and the log never carried
            // it, so a reader watching the log scored "the state was never published" over a file
            // that had already answered. Same function the panel calls, so the two cannot drift.
            g.log->Write(LogLevel::Info, kReplayModuleName, "%s", NetVarFidelityLine());
            // The config value and whether the interpolant will actually receive it are two
            // different facts, and the key on its own has never been able to say the second: the
            // curve only runs where the apply pass reaches a body, so a load with no entity
            // surface -- or one the live-safety gate holds inert -- replays every bracket on the
            // straight line with `playback.hermite=true` in the config.
            const bool pose_allowed = livesafety::Allowed(livesafety::Feature::PlayerPose);
            const bool bound = g.applier.Entity() != nullptr;
            const bool armed = g.hermite_on && bound && pose_allowed;
            char interpolant[192];
            if (armed) {
                _snprintf_s(interpolant, sizeof interpolant, _TRUNCATE,
                            "the cubic through the recorded velocities");
            } else if (!g.hermite_on) {
                _snprintf_s(interpolant, sizeof interpolant, _TRUNCATE,
                            "the straight line, because the key is off");
            } else if (!bound) {
                _snprintf_s(interpolant, sizeof interpolant, _TRUNCATE,
                            "the straight line, because no entity surface is bound and the apply "
                            "pass samples nothing");
            } else {
                _snprintf_s(interpolant, sizeof interpolant, _TRUNCATE,
                            "the straight line, because the apply pass is inert (%s)",
                            livesafety::VerdictReason(livesafety::Current()));
            }
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "hermite: playback.hermite=%s, path %s: body position takes %s. The "
                         "transport pacing line counts the brackets each way, once a second",
                         g.hermite_on ? "true" : "false", armed ? "ARMED" : "NOT armed",
                         interpolant);
        }
        // Last, and only on a load that succeeded: the bracket must never be open over a file this
        // module refused. Every failure path above returns through Unload(), which closes it.
        ArmOracleTrackIfAsked();
        return true;
    }

    void Unload() override {
        g.reload_cycle.Cancel();
        if (g.reload_cycle.FinishedNow()) FinishLevelCycle(GetTickCount64());
        // FIRST, and unconditionally: unload always ends the track. Load() begins with an Unload(),
        // so this is also what stops two loads from opening the bracket twice, and it runs on a
        // module that never loaded because a half-armed load is exactly when it matters.
        EndOracleTrackIfActive("unload");
        // S9: an unload during a drag ends it. No seek and no resume -- there is nothing left to
        // seek in -- but the audio hold is released, which is the exit that leaks if it is not.
        AbortScrub("the replay was unloaded");
        // Before the file goes: there is nothing left to prepare for, and the engine clock goes
        // back to mirroring the transport whatever state the pre-roll was in.
        EndPreRoll("the replay was unloaded", false);
        // ★★★ BEFORE THE READER CLOSES AND BEFORE A SINGLE WINDOW IS FREED. The worker is inside a
        // decode that reads its own reader, its own scratch and a window this object owns; joining
        // it here is what makes "no use after close" a property of the order rather than of luck.
        // Its reader is its own, but the clocks and the primer it borrows are g's and go below.
        //
        // ★★ A JOIN THAT TIMES OUT FREES NOTHING. Stop() returning false means the worker is still
        // running and still holding all of it, so the windows are deliberately LEFT ALLOCATED and
        // the load's scans left standing. A leaked 7 MB buffer on a path that has never been seen
        // is the right trade against a decode thread writing into freed memory.
        const bool worker_joined = winprefetch::Running() ? winprefetch::Stop(kPrefetchJoinMs) : true;
        if (!worker_joined) {
            // ★★ AND THEY ARE ABANDONED, NOT MERELY LEFT ALLOCATED. Leaving them reachable was the
            // hazard: the next Load kept the surviving slots and called Window::Reserve on them,
            // which delete[]s the pools a live worker is writing into. The worker's buffer is any
            // one of the three -- the roles rotate on every adoption -- so all three are released
            // and the module starts again on storage nothing outside it has ever seen.
            std::unique_ptr<Window> fresh(new (std::nothrow) Window());
            g.prefetch_abandoned = true;
            if (fresh) {
                for (std::unique_ptr<Window>& slot : g.win_store) {
                    (void)slot.release();
                }
                g.win_store[0] = std::move(fresh);
                g.cur = g.win_store[0].get();
            }
            if (g.log) {
                g.log->Write(LogLevel::Error, kReplayModuleName,
                             "the window prefetch worker did not finish inside %u ms; its %s and "
                             "async window decode is off for the rest of this process",
                             (unsigned)kPrefetchJoinMs,
                             fresh ? "windows are abandoned rather than freed under it"
                                   : "windows are left allocated (no memory for a fresh one)");
            }
        }
        g.async_window_decode = false;
        g.ready = nullptr;
        g.ready_centre = (size_t)-1;
        g.ready_serial = 0;
        g.ready_build = BuildStats();
        g.free_win[0] = g.free_win[1] = nullptr;
        g.prefetch_posted_centre = (size_t)-1;
        g.prefetch_posted_serial = 0;
        // Per load, not per process: a second file's boundary decodes counted into the first one's
        // rate would report one recording's hitching against another's.
        g.tick_decodes = 0;
        g.adoptions = 0;
        g.adopt_us_last = 0;
        g.adopt_us_max = 0;
        g.prefetch_late = 0;
        g.prefetch_stale = 0;
        g.prefetch_fallbacks = 0;
        g.tick_decodes_at_line = 0;
        g.async_line_at_us = 0;
        if (worker_joined) {
            // ★★ THE CURRENT WINDOW MAY BE ANY OF THE THREE. An adoption swaps roles, not storage,
            // so after one `cur` points into slot 1 or 2 -- and freeing those by index would free
            // the window the Clear() below is about to walk. The slot `cur` names is moved into
            // slot 0 (which destroys the one that was there, and that one is on the free list, not
            // in use) and the rest go. `cur` itself never moves: it is the same object.
            for (size_t i = 1; i < 3; ++i) {
                if (g.win_store[i].get() == g.cur) {
                    g.win_store[0] = std::move(g.win_store[i]);
                    break;
                }
            }
            g.win_store[1].reset();
            g.win_store[2].reset();
            g.cur = g.win_store[0].get();
        }
        if (g.loaded) {
            g.reader.Close();
        }
        // The recorded identity belongs to the file that carried it: a store left standing would
        // dress the next load's entrants from the previous match's blocks.
        identity::PlaybackRecordedIdentity().Reset();
        // And so do the stats -- with the extra step that they were WRITTEN into engine records
        // the next load may adopt the very same entrants for. The zeroing needs a netGame this
        // thread does not hold, so it is requested and the next stage-3 tail performs it; the
        // store and the applied memory go now.
        {
            statsfeed::MappedPlayer mapped[statsfeed::kMaxSlots];
            const size_t n = CollectStatMapping(mapped, statsfeed::kMaxSlots);
            statsfeed::RequestRelease(mapped, n);
        }
        statsfeed::Arm(false);
        statsfeed::PlaybackRecordedStats().Reset();
        statsfeed::PlaybackStatsCensus() = statsfeed::ScanCensus{};
        // The goal feed needs no release. What it wrote is the gameplay object's last-score DISPLAY
        // fields, which nothing reads until something raises the banner event -- and with the feed
        // disarmed nothing does. The engine clears them itself at its next round start.
        goalfeed::Arm(false);
        goalfeed::PlaybackRecordedGoals().Reset();
        ResetShellFeed();
        goalfeed::PlaybackGoalCensus() = goalfeed::ScanCensus{};
        // The stun feed DOES leave state behind -- a victim stunned at the moment of unload would
        // keep his bit1 until the engine's own timer ran it out. Disarming first and clearing the
        // store second is enough: the engine's Update owns the decay and finishes it without us.
        stunfeed::Arm(false);
        stunfeed::PlaybackRecordedStuns().Reset();
        stunfeed::PlaybackStunCensus() = stunfeed::ScanCensus{};
        // The component feed needs no release either. What it raised ran inside the level's own
        // script graphs, which own their state and their own reset event; the plugin holds nothing
        // the engine would want back.
        componentfeed::Arm(false);
        componentfeed::PlaybackRecordedEvents().Reset();
        componentfeed::PlaybackEventCensus() = componentfeed::ScanCensus{};
        g.loaded = false;
        g.window_valid = false;
        g.netvar_records_seen = false;
        // No file, no timeline: the stage-3 tail writes nothing until the next load arms it.
        DisarmGameStateNetVars();
        // The NETGAME.UPDATE detour has no removal path; disarming its body is the removal.
        identity_access::SuspendReplyDrainBody();
        scoregate::Publish(scoregate::Mode::PassThrough, 0);
        g.disc_probe_pending = false;
        g.window_centre = (size_t)-1;
        g.core_first_us = 0;
        g.core_last_us = 0;
        g.core_data_first_us = 0;
        g.core_data_last_us = 0;
        g.span_source = "none";
        g.netvar_primer = NetVarPrimer();
        g.tick_spacing_us = 0;
        // Before Clear(): afterwards no track remains to say which handles were claimed, and the
        // engine would keep treating those bodies as locally owned for the rest of the session.
        g.owner_releases += g.applier.ReleaseSyntheticOwners(Win());
        // The claims are handed back first; the pool that named them is a level-space pointer and
        // nothing after this load may read it again.
        if (g.svc && g.svc->entity) g.svc->entity->DropDisc();
        g.disc_track = Global::kNoDiscTrack;
        g.disc_fresh_accum_us = 0;
        g.netvar_spread_warn_pending = false;
        Win().Clear();
        g.build = BuildStats();
        g.last_apply = ApplyStats();
        // The admissions belonged to this load. A second load in the same session assigns its own
        // slots, and keeping the map would hand the new roster the old one's mapping.
        g.pawn.Disarm();
        g.pawn_census.Reset();
        g.slot_map_n = 0;
        g.viewer_census_due_tick = 0;
        g.viewer_seat.Reset();
        g.bind = BindResult();
        g.track_scan = TrackScan();
        g.position_accum_us = 0;
        g.position_line_pending = false;
        // The brackets belonged to the file that is going: counting them into the next load's
        // first second would report one recording's interpolants against another's.
        g.hermite_tally = HermiteTally();
        g.smooth_tally = SmoothTally();
        g.smooth_snap = true;
        g.smooth_trace_lines = 0;
        // A roster that was never dressed is a REPORTED outcome. Silence is the one thing a
        // deferral gate must not end in: the pass waited out the whole load, and what it waited on
        // last is the only thing that explains why every avatar was wrong.
        if (g.identity.pending && g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "identity dressing never ran: still deferred after %u tick(s): %s",
                         g.identity.deferred,
                         g.identity.defer_why ? g.identity.defer_why : "unstated");
        }
        if (g.svc && g.svc->identity && g.log && !g.exiting) {
            identity::ReleaseReport rel;
            identity::ReleaseRoster(*g.svc->identity, *g.log, g.roster_map, rel);
            g.identity.released = rel.released;
            g.identity.release_refused = rel.refused;
            g.leave_wait_since_ms = GetTickCount64();
            g.leave_wait_said = false;
        } else if (g.exiting && g.log && g.roster_map.Mapped()) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "the process is exiting: the replay entrants are left where they are and "
                         "no leave is fed to the engine; the OS reclaims them with the process");
        }
        // Cleared even when there was no IIdentity to release through: an index this session
        // cannot hand back is still an index it must not reuse.
        g.roster_map.Clear();
        // ...and the per-load dress latch with it, so the next load dresses every slot from the
        // start rather than skipping the ones this one happened to finish.
        g.roster_map.ArmDressPass();
        const IdentityStatus prev = g.identity;
        g.identity = IdentityStatus();
        g.identity.released = prev.released;
        g.identity.release_refused = prev.release_refused;
        g.dress_next_ms = 0;
        g.dress_defer_since_ms = 0;

        // The live LOD policy comes back the moment the file goes: a viewer who unloaded a replay is
        // in the live game again, and leaving remote bodies at full rate is our cost, not theirs.
        // It comes back as the value the ENGINE had, not as a 1 this module guessed (M2), and
        // Shutdown() reaches this through Unload(), so this is the one release site.
        ReleaseLodThrottle();
    }

    void Play(float rate) override {
        if (!g.loaded) {
            return;
        }
        // A play from elsewhere ends a drag in flight, for the same reason Seek does: otherwise the
        // next tick re-seeks to the drag's stale target over the top of it. EndScrub's own resume
        // call reaches here with the state already cleared, so this is a no-op for that path.
        AbortScrub("a play arrived from elsewhere");
        if (rate == 0.0f) {
            Pause();
            return;
        }
        // ★ A PLAY PRESSED WHILE THE LOAD IS PREPARING IS REMEMBERED, NOT REFUSED AND NOT OBEYED.
        // Obeying it starts the replay with an undriven disc, which is the whole defect; refusing
        // it makes the owner press play at a picture that never moves. EndPreRoll starts it.
        if (PreRollPreparing()) {
            g.preroll_play_requested = true;
            g.preroll_play_rate = rate;
            if (g.log) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "play at %.2fx while the replay is preparing: it starts the moment "
                             "pre-roll ends",
                             (double)rate);
            }
            return;
        }
        // ★★ PLAY AT THE END OF THE RECORDING STARTS IT AGAIN, which is what every demo player does
        // and the only reading of "play" that does anything at all from there: the playhead is on
        // the last recorded sample and forward is out of file. The same in reverse from the front.
        // A seek is a real jump and goes through the one function that moves the window with it.
        const EndState end = g.transport.Ended();
        if (rate > 0.0f && end == EndState::AtEnd) {
            if (!SeekAbsolute(g.core_first_us)) {
                return;   // refused: no window there. Staying put beats playing from nowhere.
            }
            if (g.log) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "play from the end of the recording: restarted at the beginning");
            }
        } else if (rate < 0.0f && end == EndState::AtStart) {
            // ★ CLAMPED, NOT WRAPPED. Backwards from the first sample has nothing behind it, and
            // jumping to the end is a seek across the whole recording that the owner did not ask
            // for -- he asked to run backwards from where he is standing.
            if (g.log) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "play backwards at the start of the recording: nothing to play, so "
                             "the playhead stays where it is");
            }
            return;
        }
        g.transport.SetRate((double)rate);
        g.transport.SetPaused(false);
    }

    // ★ RATE ONLY. No SetPaused, and deliberately no end-of-recording restart-seek: both belong to
    // Play. A paused viewer who picks 0.5x or Reverse stays exactly where he is and gets that rate
    // the moment he presses Play.
    void SetRate(float rate) override {
        if (!g.loaded) {
            return;
        }
        if (rate == 0.0f) {
            Pause();
            return;
        }
        if (PreRollPreparing()) {
            g.preroll_play_rate = rate;
            return;
        }
        g.transport.SetRate((double)rate);
    }

    // ---- DESIGN-UI-2 S5.3, the drag ----------------------------------------------------------

    void BeginScrub() override {
        // S9: during the pre-roll the track is inert and a click is ignored, not queued. Refusing
        // here also keeps Pause() from eating a remembered play the owner did press.
        if (!g.loaded || PreRollPreparing()) {
            return;
        }
        // L4: its engine-visible effects are the clock and the apply pass, both gated -- but a
        // future path must not be able to reach a seek sideways in a live session.
        if (!livesafety::Allowed(livesafety::Feature::Scrub)) {
            return;
        }
        if (g.scrub.active) {
            return;   // idempotent: no second hold, and the resume state stays the PRE-drag one
        }
        Global::ScrubState s;
        s.active = true;
        s.light = g.svc && g.svc->config ? g.svc->config->GetBool("playback.scrub_light", true)
                                         : true;
        s.resume_on_release = !g.transport.Paused();
        const double r = g.transport.Rate();
        s.resume_rate = r != 0.0 ? (float)r : 1.0f;
        s.load_serial = g.load_serial;
        s.pending_us = s.applied_us = g.transport.DisplayPlayheadUs();
        const ClockBeats beats0 = ReadClockBeats();
        s.beats_at_begin = s.beats_at_line = beats0.armed;
        s.refused_at_begin = beats0.refused;
        g.scrub = s;
        g.scrub_step = Global::ScrubStepRecord();
        g.scrub_last_refused = false;
        Pause();
        g.scrub_mute.Take();
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "[scrub] begin at %lld us (%s, rate %.2fx, %s seeks)",
                         (long long)(s.applied_us - Origin()),
                         s.resume_on_release ? "was playing" : "was paused", (double)s.resume_rate,
                         s.light ? "light" : "full");
        }
    }

    void ScrubTo(int64_t us) override {
        if (!g.scrub.active) {
            return;
        }
        if (!g.loaded || g.load_serial != g.scrub.load_serial) {
            AbortScrub("the loaded file changed");
            return;
        }
        const int64_t dur = (int64_t)DurationUs();
        if (us < 0) {
            us = 0;
        }
        if (dur > 0 && us > dur) {
            us = dur;
        }
        const int64_t abs_us = us + Origin();
        // ★ ONE SLOT. A target that never reached a tick is coalesced away and counted -- the
        // count is the proof the drag is not replaying every intermediate position.
        if (g.scrub.pending_dirty) {
            ++g.scrub.skipped;
        }
        if (abs_us != g.scrub.pending_us || !g.scrub.pending_dirty) {
            g.scrub.pending_at_us = WallNowUs();
        }
        g.scrub.pending_us = abs_us;
        g.scrub.pending_dirty = (abs_us != g.scrub.applied_us);
    }

    void EndScrub(int64_t us) override {
        if (!g.scrub.active) {
            g.scrub_mute.Release();   // a hold with no scrub behind it is the leak S5.3.1 is for
            return;
        }
        if (!g.loaded || g.load_serial != g.scrub.load_serial) {
            AbortScrub(!g.loaded ? "the file was unloaded" : "a different file was loaded");
            return;
        }
        // ★ RELEASED ON EVERY EXIT, INCLUDING THE ONES NOT WRITTEN YET. The refcount does not fail
        // loudly, so the release is owned by scope rather than by remembering to write it before
        // each return. It runs after the settle below, which is the order S5.3 asks for.
        struct Releaser {
            ~Releaser() { g.scrub_mute.Release(); }
        } releaser;

        const Global::ScrubState s = g.scrub;
        const int64_t target = us >= 0 ? us + Origin()
                                       : (s.pending_dirty ? s.pending_us : s.applied_us);
        // Cleared BEFORE the settle: the seek below must take the full profile, and State() owes
        // every reader Pause/Play again from here on.
        g.scrub = Global::ScrubState();
        g.scrub_step = Global::ScrubStepRecord();

        const uint64_t t0 = WallNowUs();
        const uint64_t builds0 = g.window_builds;
        const bool ok = SeekAbsolute(target, false);
        const double build_ms = double(WallNowUs() - t0) / 1000.0;
        // A settle that landed inside the resident span rebuilt nothing, so nothing ran the work
        // every light step deferred. It is owed either way.
        const uint64_t deferred0 = WallNowUs();
        if (ok && g.window_builds == builds0) {
            HudNetVarsInvalidate(true);
            PushAudioPinsFromBuild();
            BindBodyHandlesIfReady("scrub ended");
            g.history_trims += g.applier.TrimHistories(Win(), Win().OuterBeginUs(),
                                                       Win().OuterEndUs());
        }
        const double dress_ms = double(WallNowUs() - deferred0) / 1000.0;
        const uint64_t recentre0 = WallNowUs();
        RecentreWindow("endscrub");
        const double own_ms = double(WallNowUs() - recentre0) / 1000.0;

        if (s.resume_on_release && ok) {
            Play(s.resume_rate);
        }
        if (g.log) {
            // beats vs steps is the drag's verdict: one beat per step is what makes player bodies
            // land. The settle's own beat arms on the next clock tick and is not in this count.
            const ClockBeats bend = ReadClockBeats();
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "[scrub] settle target=%lld steps=%u skipped=%u refused=%d beats=%llu "
                         "busy=%llu binds=%u build_ms=%.3f hud_ms=%.3f own_ms=%.3f mute_held=%d "
                         "dropped_lines=%u total_ms=%.3f",
                         (long long)(target - Origin()), s.steps, s.skipped, ok ? 0 : 1,
                         (unsigned long long)(bend.armed - s.beats_at_begin),
                         (unsigned long long)(bend.refused - s.refused_at_begin), s.light_binds,
                         build_ms,
                         dress_ms, own_ms, g.scrub_mute.Held() ? 1 : 0, s.lines_dropped,
                         double(WallNowUs() - t0) / 1000.0);
        }
    }

    ScrubStats Scrub() const override {
        ScrubStats st;
        st.active = g.scrub.active;
        st.pendingUs = g.scrub.pending_us - Origin();
        st.appliedUs = g.scrub.applied_us - Origin();
        st.skipped = g.scrub.skipped;
        st.lastRefused = g.scrub_last_refused;
        return st;
    }

    // S9's five non-mouse-up exits land here: the hold goes, the pre-drag run state comes back,
    // and NO seek is issued -- there is either nothing left to seek in, or the seek is what just
    // refused.
    void AbortScrub(const char* why) {
        if (!g.scrub.active) {
            g.scrub_mute.Release();
            return;
        }
        const Global::ScrubState s = g.scrub;
        g.scrub = Global::ScrubState();
        g.scrub_step = Global::ScrubStepRecord();
        g.scrub_mute.Release();
        if (g.loaded && s.load_serial == g.load_serial && s.resume_on_release) {
            Play(s.resume_rate);
        }
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "[scrub] abort after %u step(s): %s", s.steps, why);
        }
    }

    // Once per tick, from Tick, above the discontinuity it raises -- so the residue battery that
    // makes the bodies snap fires on the SAME frame as the step. Returns whether a step was made.
    bool ApplyPendingScrub() {
        if (!g.scrub.active || !g.scrub.pending_dirty) {
            return false;
        }
        if (!g.loaded || g.load_serial != g.scrub.load_serial) {
            AbortScrub("the loaded file changed");
            return false;
        }
        const int64_t target = g.scrub.pending_us;
        g.scrub.pending_dirty = false;
        ++g.scrub.steps;

        Global::ScrubStepRecord& rec = g.scrub_step;
        rec = Global::ScrubStepRecord();
        rec.armed = true;
        rec.target_us = target - Origin();
        rec.covered = g.window_valid && Win().CoversForSampling(target);
        rec.skipped = g.scrub.skipped;
        rec.issued_at_us = g.scrub.pending_at_us;

        const uint64_t builds0 = g.window_builds;
        const uint64_t t0 = WallNowUs();
        const bool ok = SeekAbsolute(target, g.scrub.light);
        rec.build_ms = double(WallNowUs() - t0) / 1000.0;
        rec.seek_builds = g.window_builds - builds0;
        if (!ok) {
            // S9: the playhead did not move, so the drawn one must snap back -- and the drag is
            // over, which is also how the hold gets released.
            rec.refused = true;
            g.scrub_last_refused = true;
            // Emitted BEFORE the abort: the abort resets the record, and a refusal is the step
            // whose line matters most.
            EmitScrubStepLine();
            AbortScrub("the window build refused");
            return false;
        }
        g.scrub.applied_us = target;
        return true;
    }

    // A pause during the wait is the owner saying he does not want it to start: the remembered
    // play goes with it. The pre-roll itself keeps running -- it is not what he paused.
    void Pause() override {
        g.preroll_play_requested = false;
        g.transport.SetPaused(true);
    }

    void Step(int frames) override {
        // Nothing was consumed until the transport says it was: an unloaded module and a zero step
        // are counted refusals, not absences.
        lastStepTicks_ = 0;
        if (!g.loaded || frames == 0) {
            return;
        }
        g.preroll_play_requested = false;
        g.transport.SetPaused(true);
        lastStepTicks_ = g.transport.StepTicks(frames);
    }

    int LastStepTicks() const override { return lastStepTicks_; }

    // ★★★ THE SEEKABLE RANGE IS THE DECODED RANGE, BY THE SAME TABLE.
    //
    // The transport can only land on a boundary of the RESIDENT window -- three chapters -- and
    // SnapToTarget clamps a target past it to the last tick it holds. So every seek beyond the
    // load-time window landed on the edge of that window and the reply named a position the playhead
    // never reached: on an 80 s, 102-chapter file the whole timeline was 2.26 s wide (L3-RUN-3 F1),
    // while sequential playback traversed all of it. The window is therefore moved to the target
    // BEFORE the transport is asked to land on it, and the range this accepts is
    // [core_first_us, core_last_us] -- the same chapter table DurationUs() is computed from, so
    // "seekable" and "has a decodable window" cannot drift apart.
    //
    // ★ A build that refuses is a REFUSAL, not a clamp to wherever the old window happened to end.
    // Returns false having moved nothing, and says so by name: a jump that silently lands somewhere
    // else is exactly the failure this packet spent two launches unable to see.
    // ★ G1 OF DESIGN-UI-2 S5.1, AND IT IS WORTH NOTHING WITHOUT G2. The cheap in-window path was
    // guarded on the CENTRE CHAPTER (NeedsRebuild) while what a sample actually needs is the whole
    // resident span (CoversForSampling). A seek landing inside the outer span but outside the
    // centre chapter decoded three chapters for data already in memory. Moving this guard alone
    // moves the cost by one tick and removes none of it -- the tick's step-3 guard reaches the same
    // wrong answer on the very next frame -- so the two move together or not at all.
    bool SeekAbsolute(int64_t t_us, bool light = false) {
        if (t_us < g.core_first_us) {
            t_us = g.core_first_us;
        }
        if (t_us > g.core_last_us) {
            t_us = g.core_last_us;
        }
        if (!g.window_valid || !Win().CoversForSampling(t_us)) {
            const int64_t held = g.transport.DisplayPlayheadUs();
            if (!RebuildWindow(t_us, light)) {
                if (g.log) {
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "seek to %lld us refused: no window could be built there (%s). The "
                                 "playhead stays at %lld us.",
                                 (long long)(t_us - Origin()),
                                 g.build.refusal ? g.build.refusal : "unstated",
                                 (long long)(held - Origin()));
                }
                return false;
            }
        }
        // The other discontinuity worth a position line: where the bodies landed is the whole
        // question a seek asks, and waiting out the cadence answers it about the wrong instant.
        // A drag step asks it 300 times and the answer is a pose line plus a pre-apply CI walk per
        // body, per frame, for a position nobody will read: S5.4 defers it to the settle.
        if (!light) {
            g.position_line_pending = true;
        }
        g.transport.SeekTo(t_us);
        // A seek is one of the events that changes the dress's question: the recorded identity
        // horizon moves with the playhead, and a give-up that happened while the level was still
        // coming up must not outlive the rest of the session.
        if (!light) {
            ReArmDressAfterGiveUp("the playhead moved");
        }
        return true;
    }

    // ★★★ THE ONE CONVERSION. Public positions are playback-relative; the tick table, the window and
    // everything else below this line are absolute source time. This function and PlayheadUs() are
    // the only two places the two axes meet, which is the whole content of INV-AXIS: a caller that
    // had to subtract t0 itself is a caller that can get it wrong, and there were four of them.
    void Seek(int64_t tUs) override {
        if (!g.loaded) {
            return;
        }
        // A seek from anywhere else wins over a drag in flight, and the drag must not then keep
        // re-seeking to its own stale target on the next tick.
        AbortScrub("a seek arrived from elsewhere");
        SeekAbsolute(tUs + Origin());
    }

    // ★ The round NUMBER, and false when the file has no such round -- refused, never clamped to a
    // neighbour. Jumping to a plausible wrong round is the failure the RoundNumber type exists for.
    bool SeekRound(RoundNumber n) override {
        if (!g.loaded) {
            return false;
        }
        const Manifest& m = g.reader.GetManifest();
        for (uint16_t i = 0; i < m.roundCount; ++i) {
            if (m.rounds[i].index == n.v) {
                // The one reader-side correction, and it is named rather than silent: a mark
                // stamped before this file's origin is clamped to the origin and reported
                // PARTIAL-START (playback.h, ClampRoundSeekUs). Old captures carry such marks and
                // no writer fix can reach bytes already written.
                bool partialStart = false;
                const int64_t target =
                    ClampRoundSeekUs((int64_t)m.rounds[i].tUs, Origin(), &partialStart);
                if (partialStart && g.log) {
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "round %u is stamped %lld us before this file's origin; seeking to "
                                 "the start and reporting PARTIAL-START (kRoundPartialStart)",
                                 (unsigned)n.v,
                                 (long long)((int64_t)m.rounds[i].tUs - Origin()));
                }
                // The same path an ordinary seek takes: a round marker past the resident window is
                // no more reachable than any other target, and it was the same clamp.
                return SeekAbsolute(target);
            }
        }
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "no round numbered %u in this file; refused rather than clamped",
                         (unsigned)n.v);
        }
        return false;
    }

    void SetMode(bool asSeen) override { g.as_seen = asSeen; }

    void SetMotionSmoothing(uint8_t mode, uint8_t strength) override {
        if (mode > (uint8_t)SmoothMode::On) return;
        const SmoothMode m = (SmoothMode)mode;
        const uint8_t st = ParamsForStrength(strength).strength;
        if (m == g.smooth_mode && st == g.smooth_strength) return;
        g.smooth_mode = m;
        g.smooth_strength = st;
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName, "smoothing: %s strength %s (menu)",
                         SmoothModeName(m), SmoothStrengthName(st));
        }
    }

    uint16_t MotionSmoothing() const override {
        return (uint16_t)((uint16_t)g.smooth_strength << 8 | (uint16_t)g.smooth_mode);
    }

    bool LiveUserSlot(uint16_t recordedSlot, uint16_t* outLiveSlot) const override {
        UserIndex index(0);
        if (!g.loaded || outLiveSlot == nullptr || !g.roster_map.Lookup(recordedSlot, index)) {
            return false;
        }
        *outLiveSlot = (uint16_t)index.v;
        return true;
    }

    // ★ State() still answers Pause throughout a pre-roll, because that is what the transport IS:
    // the playhead is not moving. Everything downstream of it -- the audio rate drive, the netvar
    // publish, the pacing line's 1.000x contract -- is a statement about the PLAYHEAD and stays
    // true. This is the separate answer to "why", and it is the only thing that tells a viewer
    // waiting on purpose from a viewer that hung.
    PrepareStatus Preparing() const override {
        PrepareStatus p;
        if (!g.loaded || !PreRollPreparing()) return p;
        p.preparing = true;
        p.playRequested = g.preroll_play_requested;
        p.elapsedS = double(g.preroll_wall_us) / 1e6;
        p.boundS = double(g.preroll_bound_us) / 1e6;
        // Against the host time expected to be needed, not against the give-up bound: the bar is
        // how far the pre-match has got, and it is capped at the bound it can actually run to.
        // Over the WHOLE wait -- pre-match AND the launch sequence behind it. Measuring the bar
        // against the 45 s alone would run it to 100%, then sit there for the ten seconds the host
        // spends in the tubes, which is the one thing a progress bar must not do.
        // Both scales, each over its own stretch of host time: at 4x pre-match and 2x launch the
        // bar spans 11.25 + 10 s, and a bar measured against one of the two would stall or finish
        // early at the ramp.
        const double expected = PreRollExpectedWallS();
        const double span = expected < p.boundS ? expected : p.boundS;
        p.progress = span > 0.0 ? p.elapsedS / span : 0.0;
        if (p.progress > 1.0) p.progress = 1.0;
        // An ESTIMATE: the script's own copy of the countdown is not readable (the netvar is a
        // one-way mirror), so this is 65 s less the host time the clock has been able to run out.
        // Measured off the engine's own sim clock where there is one, so it does not inherit a
        // scale the ceiling refused; the commanded product is the fallback for a process with no
        // engine behind it.
        const double ranHostS =
            g.preroll_fast_sim_us + g.preroll_ramp_sim_us > 0
                ? double(g.preroll_fast_sim_us + g.preroll_ramp_sim_us) / 1e6
                : double(g.preroll_fast_wall_us) / 1e6 * (double)g.preroll_scale +
                      double(g.preroll_ramp_wall_us) / 1e6 * (double)g.preroll_rate;
        p.hostRemainingS = kHostToPlayingS - ranHostS;
        if (p.hostRemainingS < 0.0) p.hostRemainingS = 0.0;
        return p;
    }

    TransportState State() const override {
        if (!g.loaded) {
            // ★ NOT `Pause`. This transport is published from Init before any file is opened, so
            // this is the state the whole session reports until a replay is loaded -- and reporting
            // it as a pause COMMAND is what stopped the engine's clock in launch 1.
            return TransportState::NotLoaded;
        }
        // S5.3: Scrub and Seek were declared at transport.h:54 and never produced. A drag now
        // produces one, which is also what lets `info` report a scrub to an agent with no mouse.
        if (g.scrub.active) {
            return TransportState::Scrub;
        }
        return g.transport.Paused() ? TransportState::Pause : TransportState::Play;
    }

    EndState Ended() const override {
        return g.loaded ? g.transport.Ended() : EndState::None;
    }

    float Rate() const override { return (float)g.transport.Rate(); }
    // The other half of the conversion. Signed, and deliberately not clamped at zero: a reverse step
    // past the first recorded tick is a real transport state, and reporting it as 0 would hide the
    // one case B.8's signed return type exists for.
    int64_t PlayheadUs() const override { return g.transport.DisplayPlayheadUs() - Origin(); }

    // The boundary half of the conversion PlayheadUs() does, and the only other place the two axes
    // meet. Absolute source time does not leave this class: what comes back is directly comparable
    // to PlayheadUs(), which is the entire contract W2-R's row 26 asks against.
    bool NearestTickUs(int64_t playheadUs, int64_t* outTickUs) const override {
        if (!g.loaded || outTickUs == nullptr) {
            return false;
        }
        int64_t absUs = 0;
        if (!g.transport.TickAtOrBelow(playheadUs + Origin(), &absUs)) {
            return false;
        }
        *outTickUs = absUs - Origin();
        return true;
    }

    // The recorded content's own span, converted to the playhead axis by the one subtraction
    // NearestTickUs uses. It reads the same chapter table DurationUs() is computed from, so the
    // timeline and the thing it scrubs cannot disagree about where the file's content is.
    bool PlayableSpanUs(int64_t* firstUs, int64_t* lastUs) const override {
        if (!g.loaded || firstUs == nullptr || lastUs == nullptr ||
            g.core_data_last_us <= g.core_data_first_us) {
            return false;
        }
        *firstUs = g.core_data_first_us - Origin();
        *lastUs = g.core_data_last_us - Origin();
        return true;
    }

    uint32_t LoadSerial() const override { return g.load_serial; }

    uint64_t DurationUs() const override {
        if (!g.loaded) {
            return 0;
        }
        // The window covers three chapters, so its last tick is not the file's end. The chapter table
        // is; using the window's would have shortened the scrub bar to whatever is resident. Read off
        // the span Load computed, so the number the scrub bar is drawn against and the number
        // SeekAbsolute accepts up to are one value and not two computations of it.
        return g.core_last_us > g.core_first_us
                   ? (uint64_t)(g.core_last_us - g.core_first_us)
                   : 0;
    }

    // The file's cadence, measured at Load off the first core chapter that carries ticks. The
    // manifest carries no tick-spacing field (demotypes.h: keyframeIntervalUs is the KEYFRAME
    // interval, a different quantity), so this is the measurement rather than a declared value --
    // and it is the SAME measurement that sized the window, not a second opinion about it.
    //
    // It is a mean over a chapter, not a median over the window. A chapter containing a capture gap
    // therefore reads WIDER than the steady cadence -- which for a landing tolerance is the safe
    // direction: it errs towards accepting a correct landing, never towards refusing one, and
    // refusing correct landings is the defect this accessor exists to end.
    bool TickSpacingUs(int64_t* outUs) const override {
        if (!g.loaded || g.tick_spacing_us <= 0 || outUs == nullptr) {
            return false;
        }
        *outUs = g.tick_spacing_us;
        return true;
    }

    // ★★ THE UI NEVER DEGRADES SILENTLY. Every honest gap this packet knows about is in this string:
    // the mode, the rate, whether netvars land, how many residue primitives are unmapped, and whether
    // the file was recorded against a different build. A fidelity statement that lives only in a log
    // is one the person watching the replay never sees.
    // The file's own origin. Private: nothing above this class needs it, and anything that took it
    // would be building the second answer INV-AXIS exists to prevent.
    static int64_t Origin() { return SourceOriginUs(); }

    const char* ActiveModeLabel() const override {
        if (!g.loaded) {
            _snprintf_s(g.label, sizeof g.label, _TRUNCATE, "no file loaded%s%s",
                        g.refusal[0] ? " - " : "", g.refusal);
            return g.label;
        }
        char rate[32];
        if (g.transport.Paused()) {
            _snprintf_s(rate, sizeof rate, _TRUNCATE, "paused");
        } else {
            _snprintf_s(rate, sizeof rate, _TRUNCATE, "%.2fx", g.transport.Rate());
        }
        const size_t unavailable = g.battery.UnavailableCount();
        char identity[64];
        if (g.identity.pending) {
            _snprintf_s(identity, sizeof identity, _TRUNCATE, "identity: DEFERRED (%u ticks)",
                        g.identity.deferred);
        } else {
            _snprintf_s(identity, sizeof identity, _TRUNCATE, "identity: %u/%u dressed",
                        g.identity.dressed, g.identity.players);
        }
        // Separate from `identity` because it answers a different question. Dressing says whether
        // the avatars are right; this says whether the bodies can move at all, and a panel showing
        // only the first would read "identity: 0/2 dressed" for a world that is playing correctly
        // in grey, and for one that is frozen solid.
        // ★ When the is-local-user arm is off, the panel says so AND says why:
        // `ambiguous` (a host authoring more than one player -- the shape the sanctioned -offline
        // in-client host is expected to take) is a different finding from `none` (nobody seated
        // yet) or `not-available` (no broadcaster pool at all). Without the reason on the line, a
        // permanently-half-armed session reads identical to a healthy one.
        char owners[128];
        _snprintf_s(owners, sizeof owners, _TRUNCATE, "owners: %u/%u mapped%s%s%s",
                    g.identity.mapped, g.identity.players,
                    g.identity.owner_refused ? " | INV-OWN1 REFUSED" : "",
                    g.owners.LocalUserKnown() ? "" : " | INV-OWN1 local-unknown: ",
                    g.owners.LocalUserKnown() ? "" : LocalUserWord(g.local_user_state));
        // W2-H measures the rate plan and names it; S10.4.5 wants the UI to say so, and until now
        // the string existed with no printer.
        const char* rateLabel =
            g.svc && g.svc->audioRate ? g.svc->audioRate->RateLabel() : nullptr;
        if (!rateLabel) rateLabel = "";
        _snprintf_s(g.label, sizeof g.label, _TRUNCATE,
                    "%s%s%s | %s%s | %s | %s | %s | residue: %u/%u unmapped%s%s",
                    rate, rateLabel[0] ? " | " : "", rateLabel,
                    g.as_seen ? "as-seen" : "ground truth",
                    g.transport.GapsCrossed() ? " | gaps crossed" : "",
                    // The full string, not a slice of it: pointer arithmetic to skip a prefix
                    // breaks silently the day the wording changes, and the wording is the point.
                    NetVarFidelityLine(), identity, owners,
                    (unsigned)unavailable, (unsigned)g.battery.Count(),
                    g.build_mismatch ? " | DIFFERENT GAME BUILD" : "",
                    g.build_checked ? "" : " | build not checked");
        return g.label;
    }

private:
    // The transport's own count of the last step, so a caller never has to difference playheads
    // for it. Reset by every Step(), including the ones that return early.
    int lastStepTicks_ = 0;
};

ReplayTransportImpl g_transport;

// ---------------------------------------------------------------------------------------------
// The frame loop. S7.1's steps, in order, with the two this packet does not own left to their owners.

// ★ TickFn is void(*)(const TickInfo&, void*). The ctx parameter is unused here because this
// module keeps one global -- but it is in the signature, and a one-argument function silently is not
// a TickFn.
//
// TWO READS, NEVER ONE. `driven=` is our own source sample at the presented playhead -- the same
// interpolation SetHistoryWindow was given, not the nearest recorded sample, which is a different
// pose at every instant between two stamps and would let a frozen replay read as a moving one.
// `read=` is the ENGINE's answer for the SAME handle, asked AFTER the applier's install through the
// same ReadBodyDetailed gates that install now passes. Until DIAG-PAWN-STATIC this line printed only
// the first, and 286 identical poses were read as a motionless PAWN when they were a motionless
// FILE. One number that is silently the source cannot answer "does the pawn move".
//
// The cursor is a COPY: the applier owns the track's, and an instrument that advances it perturbs
// the thing it measures.
// THE ENGINE'S OWN BODY, READ BEFORE THIS TICK'S WRITE.
//
// AUD-W2-B-POSEREAD major 3: LogBoundBodyPositions runs in the same tick callback as
// StateApplier::Apply, one statement later, and ReadBodyDetailed takes the history ring's head --
// the node the applier installed a moment earlier. So `read=` proves the ring HOLDS our pose and
// nothing else: src=history and delta=0.000 are structural on every successful install, and two
// `read=` values a second apart differ exactly when the two `driven=` values do. That cannot answer
// "does the pawn move".
//
// `read_next=` is the same read taken BEFORE Apply on the tick that prints -- the engine's body as
// it stands after the previous window's install AND every physics step since. It is the engine's
// answer, not our echo. MOTION is `read_next=` CHANGING from one window to the next; `delta_next`
// is how far the engine's body sits from what this window drove.
struct EnginePose {
    bool  taken = false;
    bool  ok = false;
    float com[3] = {0.0f, 0.0f, 0.0f};
    BodySource  src = BodySource::NotAsked;
    BodyRefusal why = BodyRefusal::None;
};
constexpr size_t kMaxPoseProbes = 8;
EnginePose g_pre_apply[kMaxPoseProbes];

// ★ THE ONE AXIS A HUMAN READS. Seconds from the START OF THE PLAYABLE SPAN -- what `seek @N`
// takes, what the ui bar draws, and what "content 30 s" means. The file's own stamps run from an
// origin 822 s earlier on a partial-start capture, so a bare absolute printed beside a seek reads
// as a 20.9 s error that is not there (DIAG-DISC-FROZEN-REGRESSION §1).
double ContentSeconds(int64_t abs_us) { return double(abs_us - g.core_data_first_us) / 1e6; }

void SnapshotPreApplyPoses() {
    for (size_t i = 0; i < kMaxPoseProbes; ++i) g_pre_apply[i] = EnginePose();
    IEntity* const ent =
        g.svc && g.svc->entity && g.svc->entity->Available() ? g.svc->entity : nullptr;
    if (!ent) return;
    for (size_t i = 0; i < Win().BodyCount() && i < kMaxPoseProbes; ++i) {
        const BodyTrack& tr = Win().BodyAt(i);
        if (!tr.handle_valid) continue;
        BodyState s;
        memset(&s, 0, sizeof s);
        BodyReading r;
        EnginePose& p = g_pre_apply[i];
        p.taken = true;
        // The remote-replay slot surface reads its driven element with the ci[+0] field compare
        // dropped, so a working install reports src=history not ciEntityMismatch (DIAG-POSE-DECOUPLE).
        p.ok = tr.slot_remote ? ent->ReadBodyDetailedDriven(tr.handle, s, r)
                              : ent->ReadBodyDetailed(tr.handle, s, r);
        p.src = r.source;
        p.why = r.refusedAt;
        if (p.ok) { p.com[0] = s.com[0]; p.com[1] = s.com[1]; p.com[2] = s.com[2]; }
    }
}

// One line per bound body per status pass -- 1.65 MB of the auditor's log.
// The whole block is gated, reads included: the engine reads below are the expensive half.
uint64_t g_bodyPosChatterAt = 0;

// This frame's smoothing weight: 1 where the mode wants smoothing at the current rate, else 0. A click
// that moves the target crossfades over 200 ms of wall time; a load, seek or scrub lands on it.
SmoothFrame SmoothFrameFor(int64_t wall_us) {
    Win().SetSmoothing(ParamsForStrength(g.smooth_strength));
    const float target = SmoothWanted(g.smooth_mode, g.transport.Rate()) ? 1.0f : 0.0f;
    g.smooth_w = StepSmoothWeight(g.smooth_w, target, wall_us, g.smooth_snap || g.scrub.active);
    g.smooth_snap = false;
    SmoothFrame f;
    f.weight = g.smooth_w;
    f.tally = &g.smooth_tally;
    return f;
}

// `playback.motion_smoothing_trace`: the lowest bound player's pose as this frame drove it, one line
// per frame, for tools/smoothtrace.ps1. Sampled at the playhead (ground truth), capped per load.
void TraceSmoothing(int64_t t, int64_t wall_us) {
    constexpr uint32_t kTraceCap = 20000;
    if (!g.log || g.smooth_trace_lines >= kTraceCap) return;
    PlayerTrack* who = nullptr;
    for (size_t i = 0; i < Win().PlayerCount(); ++i) {
        PlayerTrack& tr = Win().PlayerAt(i);
        if (tr.handle_valid && (who == nullptr || tr.user_slot < who->user_slot)) who = &tr;
    }
    if (who == nullptr) return;
    SmoothFrame smooth;
    smooth.weight = g.smooth_w;
    PlayerState ps;
    SeriesCursor pc = who->cursor;
    if (!SamplePlayerFor(Win(), *who, pc, t, ps, false, &smooth)) return;
    BodyState bs;
    memset(&bs, 0, sizeof bs);
    for (size_t i = 0; i < Win().BodyCount(); ++i) {
        BodyTrack& tr = Win().BodyAt(i);
        if (!tr.handle_valid || !tr.bound_by_slot || tr.user_slot != who->user_slot) continue;
        SeriesCursor bc = tr.cursor;
        SampleBodyFor(Win(), tr, bc, t, bs, false, g.hermite_on, nullptr, &smooth);
        break;
    }
    ++g.smooth_trace_lines;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "[smooth-trace] n=%u wall_us=%lld t=%lld rate=%.3f w=%.2f com=%.5f,%.5f,%.5f "
                 "lh=%.5f,%.5f,%.5f rh=%.5f,%.5f,%.5f hmd=%.6f,%.6f,%.6f,%.6f",
                 g.smooth_trace_lines, (long long)wall_us, (long long)t, g.transport.Rate(),
                 double(g.smooth_w), bs.com[0], bs.com[1], bs.com[2], ps.lHandOff[0], ps.lHandOff[1],
                 ps.lHandOff[2], ps.rHandOff[0], ps.rHandOff[1], ps.rHandOff[2], ps.hmdOri[0],
                 ps.hmdOri[1], ps.hmdOri[2], ps.hmdOri[3]);
}

void LogBoundBodyPositions(int64_t t) {
    if (!g.log) return;
    if (!replay::log::ChatterDue(g_bodyPosChatterAt)) return;
    IEntity* const ent =
        g.svc && g.svc->entity && g.svc->entity->Available() ? g.svc->entity : nullptr;
    for (size_t i = 0; i < Win().BodyCount(); ++i) {
        BodyTrack& tr = Win().BodyAt(i);
        if (!tr.handle_valid) continue;
        SeriesCursor cursor = tr.cursor;
        BodyState s;
        // Same interpolant as the drive, or this line's `driven == read, delta` measures the
        // difference between two interpolants instead of the thing it is about. No tally: this
        // sample is the diagnostic's own re-read, not a bracket the replay drove a body through.
        SmoothFrame smooth;
        smooth.weight = g.smooth_w;
        if (!SampleBodyFor(Win(), tr, cursor, t, s, false, g.hermite_on, nullptr, &smooth)) {
            continue;
        }
        // THE LIVE SLOT, through the same map the bind asked through: the number here has to be
        // the one the engine's own `added to slot N` names, or the two logs cannot be correlated.
        const bool mapped = tr.bound_by_slot && tr.user_slot < g.slot_map_n &&
                            g.slot_map[tr.user_slot] != PawnResolver::kSlotUnassigned;
        const uint16_t slot = mapped ? g.slot_map[tr.user_slot] : tr.user_slot;
        char ent_text[kEntityKeyTextMax];
        // Per track: a multi-player capture binds several bodies by slot, each to its own pawn.
        FormatEntityKey(tr.bound_by_slot ? tr.slot_entity : kEntityKeyUnknown, ent_text,
                        sizeof ent_text);

        // A REFUSAL TAKES THE PLACE OF THE POSE. Printing (0.00, 0.00, 0.00) for a read that never
        // happened would put a pose-shaped number where there is no reading, and a delta measured
        // against it would be a distance from nothing.
        BodyState back;
        memset(&back, 0, sizeof back);
        BodyReading r;
        const bool answered = ent && (tr.slot_remote
                                          ? ent->ReadBodyDetailedDriven(tr.handle, back, r)
                                          : ent->ReadBodyDetailed(tr.handle, back, r));
        char read_text[64];
        char delta_text[24];
        if (answered) {
            _snprintf_s(read_text, sizeof read_text, _TRUNCATE, "(%.2f, %.2f, %.2f)",
                        back.com[0], back.com[1], back.com[2]);
            const float dx = back.com[0] - s.com[0];
            const float dy = back.com[1] - s.com[1];
            const float dz = back.com[2] - s.com[2];
            _snprintf_s(delta_text, sizeof delta_text, _TRUNCATE, "%.3f",
                        sqrtf(dx * dx + dy * dy + dz * dz));
        } else {
            // BodyRefusalName(None) is "none", which reads as health -- so a read that failed
            // without naming a gate says so instead of borrowing the healthy word.
            const char* why = !ent                               ? "noEntitySurface"
                              : r.refusedAt != BodyRefusal::None ? BodyRefusalName(r.refusedAt)
                                                                 : "noReading";
            _snprintf_s(read_text, sizeof read_text, _TRUNCATE, "%s", why);
            _snprintf_s(delta_text, sizeof delta_text, _TRUNCATE, "n/a");
        }
        // `src` is the engine's own account of WHICH source answered, and it stays on the line when
        // the read refused: `notAsked` (we never got to ask) is not `none` (both chains were asked
        // and neither had a body).
        const char* src = ent ? BodySourceName(r.source) : BodySourceName(BodySource::NotAsked);

        // The pre-apply reading for THIS track, if one was taken this tick.
        char next_text[64];
        char next_delta[24];
        const char* next_src = BodySourceName(BodySource::NotAsked);
        const EnginePose& p = i < kMaxPoseProbes ? g_pre_apply[i] : EnginePose();
        if (!p.taken) {
            _snprintf_s(next_text, sizeof next_text, _TRUNCATE, "notTaken");
            _snprintf_s(next_delta, sizeof next_delta, _TRUNCATE, "n/a");
        } else if (p.ok) {
            _snprintf_s(next_text, sizeof next_text, _TRUNCATE, "(%.2f, %.2f, %.2f)", p.com[0],
                        p.com[1], p.com[2]);
            const float nx = p.com[0] - s.com[0];
            const float ny = p.com[1] - s.com[1];
            const float nz = p.com[2] - s.com[2];
            _snprintf_s(next_delta, sizeof next_delta, _TRUNCATE, "%.3f",
                        sqrtf(nx * nx + ny * ny + nz * nz));
            next_src = BodySourceName(p.src);
        } else {
            _snprintf_s(next_text, sizeof next_text, _TRUNCATE, "%s",
                        p.why != BodyRefusal::None ? BodyRefusalName(p.why) : "noReading");
            _snprintf_s(next_delta, sizeof next_delta, _TRUNCATE, "n/a");
            next_src = BodySourceName(p.src);
        }

        // WHY the engine did or did not get this tick's pose, for the one track that carries the
        // claim. `read=badSlot` with nothing beside it read as an engine defect for a whole
        // launch; an install that never ran and one that ran and refused are the same silence.
        char drive_text[160];
        drive_text[0] = '\0';
        if (tr.claim_owner) {
            const InstallReading& ir = g.last_apply.disc_install_refused;
            // The owner dword the engine was holding when the claim had to be re-made. Printed on
            // the reclaim ticks only: it is the one number that separates "the engine took the
            // body back" from "our own level was never stamped".
            char owner_text[48];
            owner_text[0] = '\0';
            if (g.last_apply.disc_owner_seen_valid) {
                _snprintf_s(owner_text, sizeof owner_text, _TRUNCATE, " engineOwner=%08X reclaims=%u",
                            unsigned(g.last_apply.disc_owner_seen),
                            unsigned(g.last_apply.disc_owner_reclaims));
            }
            if (g.last_apply.disc_drive == DiscDrive::Refused) {
                _snprintf_s(drive_text, sizeof drive_text, _TRUNCATE,
                            " drive=refused(%s via %s)%s",
                            InstallRefusalName(ir.why), CiPathName(ir.from), owner_text);
            } else {
                _snprintf_s(drive_text, sizeof drive_text, _TRUNCATE, " drive=%s%s",
                            DiscDriveName(g.last_apply.disc_drive), owner_text);
            }
        }
        // ONE AXIS. `playhead=` is content-relative seconds, the axis every span, seek and ui line
        // speaks; `abs_us=` is the file's own stamp, named so it can never be read as the other.
        // Printing the raw absolute here cost a launch to "2.386 s past the end of the span".
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "pawn slot %u ent=%s driven=(%.2f, %.2f, %.2f) read=%s src=%s delta=%s "
                     "read_next=%s src_next=%s delta_next=%s%s playhead=%.3fs abs_us=%lld",
                     unsigned(slot), ent_text, s.com[0], s.com[1], s.com[2], read_text, src,
                     delta_text, next_text, next_src, next_delta, drive_text, ContentSeconds(t),
                     (long long)t);
    }
}

// The host's OnScoreCB is swallowed for as long as a replay is loaded in contained offline
// playback: whatever the disc is doing, a host goal then is its own script scoring a replayed
// scene (scoregate.h). Each swallowed score is logged against the playhead it arrived at.
// Unloading publishes PassThrough directly.
void DriveScoreGate(int64_t t) {
    if (!scoregate::Installed()) return;
    scoregate::GateState s;
    s.contained = ProcessContained();
    s.loaded = g.loaded;
    s.gate_on = g.disc_score_gate_on;
    const scoregate::Decision r = scoregate::Decide(s);
    scoregate::Publish(r.mode, t);

    // ONE LINE PER EDGE, never per tick. The reason moving inside one mode is an edge too: it is
    // the term that would have opened the gate under the old condition.
    if (g.log && (!g.score_gate_said || r.mode != g.score_gate_mode || r.why != g.score_gate_why)) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "host score gate: %s -> %s at playhead %.3fs abs_us=%lld -- %s",
                     g.score_gate_said ? scoregate::ModeName(g.score_gate_mode) : "unpublished",
                     scoregate::ModeName(r.mode), ContentSeconds(t), (long long)t, r.why);
        g.score_gate_said = true;
        g.score_gate_mode = r.mode;
        g.score_gate_why = r.why;
    }

    scoregate::Counts c;
    scoregate::DrainCounts(c);
    // A pass means the host's IncrementScore has just written its own team and point count over
    // the recorded ones. The goal feed re-applies the recording within the tick.
    if (c.passed) goalfeed::NotePassSinceLastSweep();
    if (!g.log) return;
    if (c.swallowed) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "host score gate: swallowed %u OnScoreCB call(s) at playhead %.3fs "
                     "abs_us=%lld -- no stats, no score netvars, no rules state change, no send",
                     unsigned(c.swallowed), ContentSeconds(c.lastSwallowedPlayheadUs),
                     (long long)c.lastSwallowedPlayheadUs);
    }
    if (c.passed) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "host score gate: %u OnScoreCB call(s) PASSED to the host at playhead %.3fs "
                     "abs_us=%lld (%s)",
                     unsigned(c.passed), ContentSeconds(t), (long long)t, r.why);
    }
}

// DISC-BIND probes P2, P3, P4 and P7, once per load, on the first frame an apply reached the disc.
void LogDiscProbe(int64_t t, void* netGame) {
    // ONE SHOT, whatever it finds. Returning with the flag still set cost a track walk every frame
    // for the rest of the load, and the healthy case -- claim not attempted because the engine does
    // not report the disc locally controlled -- printed nothing at all.
    g.disc_probe_pending = false;
    if (!g.log || !g.svc || !g.svc->entity) return;
    const BodyTrack* disc = DiscTrack();
    if (!disc || !disc->handle_valid) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc drive probe: no disc bound, so nothing is driven or swallowed");
        return;
    }
    const InstallReading& refused = g.last_apply.disc_install_refused;
    const bool install_refused = refused.why != InstallRefusal::None;

    BodyState ring;
    uint64_t ring_t = 0;
    memset(&ring, 0, sizeof ring);
    const size_t nodes = g.svc->entity->ReadHistoryWindow(disc->handle, &ring, &ring_t, 1);
    uint64_t clock_us = 0;
    const uint8_t* const clock =
        netGame ? static_cast<const uint8_t*>(netGame) + facts::NETGAME_PEERTIME_OFF : nullptr;
    const bool clock_read = clock && Readable(clock, sizeof clock_us);
    if (clock_read) memcpy(&clock_us, clock, sizeof clock_us);

    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "disc drive probe: handle=%u/pool%u cid=0x%08X owner claim %s, engine still "
                 "simulates it=%s; "
                 // `ring holds 1 node(s)` was this call's CAPACITY argument, hard-coded 1, and it
                 // printed 1 in every build it ever shipped in. It says what it is now.
                 "install %s (history container %s, identity gate %s, ci via %s); read %u of the 1 "
                 "node this probe asks for, newest at playhead=%.3fs (abs_us=%llu) against "
                 "playhead=%.3fs (abs_us=%lld) and netgame clock %s%llu us",
                 unsigned(disc->handle.v), unsigned(disc->handle.pool), disc->disc_component_id,
                 disc->synthetic_owner_set
                     ? "made"
                     : "NOT made (the engine did not report the disc locally controlled)",
                 g.svc->entity->IsLocallyControlled(disc->handle) ? "YES" : "no",
                 install_refused ? InstallRefusalName(refused.why) : "ok",
                 refused.why == InstallRefusal::NoHistoryList ? "ABSENT" : "present",
                 refused.why == InstallRefusal::CiIdentityMismatch ? "REFUSED" : "passed",
                 CiPathName(refused.from), unsigned(nodes),
                 ContentSeconds((int64_t)ring_t), (unsigned long long)ring_t, ContentSeconds(t),
                 (long long)t,
                 clock_read ? "" : "(unread) ", (unsigned long long)clock_us);
}

// ★ FAR-SOUND-2's instrument. A census answers "how many voices now"; only a LEDGER separates a
// stack of short one-shots from loops that were started and never stopped, and the age of the
// oldest live voice is the whole of that distinction. Read-only -- the same four Wwise Query entry
// points the `census` verb already binds, nothing posted, nothing stopped, nothing re-mixed.
struct VoiceLedger {
    struct Row {
        uint32_t playingId = 0;
        uint32_t eventId = 0;
        uint32_t emitterId = 0;
        uint64_t firstMs = 0;
        uint32_t lastSeq = 0;
    };
    // Twice the census's own playing cap, so the ledger cannot be the thing that loses a voice.
    static constexpr uint32_t kCap = 128;
    Row      rows[kCap];
    uint32_t used = 0;
    uint32_t seq = 0;
    uint64_t next_ms = 0;
    uint64_t digest_ms = 0;
    uint64_t starts = 0;
    uint64_t ends = 0;
    uint64_t overflow = 0;   // voices seen that the ledger had no room to track
    bool     refused_said = false;
};
VoiceLedger g_voices;

void SampleVoices(uint64_t now_ms) {
    IEntity* const ent = g.svc ? g.svc->entity : nullptr;
    if (ent == nullptr || !ent->Available() || g.log == nullptr) return;

    IEntity::AudioMix mix;
    if (!ent->AudioMixCensus(mix) || mix.playingRefused != nullptr) {
        if (!g_voices.refused_said) {
            g_voices.refused_said = true;
            g.log->Write(LogLevel::Warn, kReplayModuleName, "voicewatch: refused -- %s",
                         mix.playingRefused ? mix.playingRefused
                                            : (mix.refused ? mix.refused : "unknown"));
        }
        return;
    }
    const bool haveL = mix.listenersWritten != 0;
    const float lx = haveL ? mix.listeners[0].pos[0] : 0.0f;
    const float ly = haveL ? mix.listeners[0].pos[1] : 0.0f;
    const float lz = haveL ? mix.listeners[0].pos[2] : 0.0f;

    ++g_voices.seq;
    for (uint32_t i = 0; i < mix.playingWritten; ++i) {
        const IEntity::AudioPlayingSound& s = mix.playing[i];
        uint32_t at = g_voices.kCap;
        for (uint32_t r = 0; r < g_voices.used; ++r) {
            if (g_voices.rows[r].playingId == s.playingId) { at = r; break; }
        }
        if (at != g_voices.kCap) {
            g_voices.rows[at].lastSeq = g_voices.seq;
            continue;
        }
        if (g_voices.used >= g_voices.kCap) { ++g_voices.overflow; continue; }
        VoiceLedger::Row& row = g_voices.rows[g_voices.used++];
        row.playingId = s.playingId;
        row.eventId   = s.eventId;
        row.emitterId = s.emitterId;
        row.firstMs   = now_ms;
        row.lastSeq   = g_voices.seq;
        ++g_voices.starts;
        // How many voices of THIS event already sit on THIS emitter is the number the hypothesis
        // turns on, so it is on the start line rather than only in the digest.
        uint32_t same = 0;
        for (uint32_t r = 0; r < g_voices.used; ++r) {
            if (g_voices.rows[r].emitterId == s.emitterId && g_voices.rows[r].eventId == s.eventId)
                ++same;
        }
        const float dx = s.emitterPos[0] - lx, dy = s.emitterPos[1] - ly, dz = s.emitterPos[2] - lz;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "voicewatch start: play %u event %u emitter %u d=%.1f radius %.1f | %u of this "
                     "event on this emitter, %u live",
                     s.playingId, s.eventId, s.emitterId,
                     haveL ? sqrtf(dx * dx + dy * dy + dz * dz) : -1.0f, (double)s.maxRadius, same,
                     g_voices.used);
    }
    // Sweep: a row this pass did not see has ended, and its age is what the whole instrument is for.
    for (uint32_t r = 0; r < g_voices.used;) {
        if (g_voices.rows[r].lastSeq == g_voices.seq) { ++r; continue; }
        const VoiceLedger::Row gone = g_voices.rows[r];
        g_voices.rows[r] = g_voices.rows[--g_voices.used];
        ++g_voices.ends;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "voicewatch end: play %u event %u emitter %u age %.1f s | %u live",
                     gone.playingId, gone.eventId, gone.emitterId,
                     double(now_ms - gone.firstMs) / 1000.0, g_voices.used);
    }
    if (now_ms - g_voices.digest_ms < 5000u) return;
    g_voices.digest_ms = now_ms;
    uint32_t oldest = g_voices.kCap, busiest = 0, busiestOn = 0;
    for (uint32_t r = 0; r < g_voices.used; ++r) {
        if (oldest == g_voices.kCap || g_voices.rows[r].firstMs < g_voices.rows[oldest].firstMs)
            oldest = r;
        uint32_t n = 0;
        for (uint32_t q = 0; q < g_voices.used; ++q)
            if (g_voices.rows[q].emitterId == g_voices.rows[r].emitterId) ++n;
        if (n > busiest) { busiest = n; busiestOn = g_voices.rows[r].emitterId; }
    }
    g.log->Write(LogLevel::Warn, kReplayModuleName,
                 "voicewatch: %u live on %u of %u emitter(s) (%u seen, %u written); oldest %.1f s "
                 "(play %u event %u emitter %u); busiest emitter %u with %u; starts %llu ends %llu "
                 "overflow %llu",
                 g_voices.used, mix.emittersActive, mix.emittersWritten, mix.playingSeen,
                 mix.playingWritten,
                 oldest == g_voices.kCap ? 0.0
                                         : double(now_ms - g_voices.rows[oldest].firstMs) / 1000.0,
                 oldest == g_voices.kCap ? 0u : g_voices.rows[oldest].playingId,
                 oldest == g_voices.kCap ? 0u : g_voices.rows[oldest].eventId,
                 oldest == g_voices.kCap ? 0u : g_voices.rows[oldest].emitterId, busiestOn, busiest,
                 (unsigned long long)g_voices.starts, (unsigned long long)g_voices.ends,
                 (unsigned long long)g_voices.overflow);
}

// ★ AUDIO-FIX-2 item 3. THE CUTOFF NUMBER, PRINTED THE SAME WAY IN BOTH KINDS OF SESSION.
//
// The owner's live client stops sounding foley past some distance; this one does not. Nobody has
// the number, and it is not derivable offline -- it lives in bank data. The farthest emitter that
// is CARRYING A VOICE, measured to the listener the mix actually attenuates against, IS that
// number: in a live match it is the cutoff radius, and in a replay it is what we have instead of
// one. So both sessions print the same fields and the answer is a subtraction.
//
// EVERY TERM IS A READ -- the engine's own emitter and listener tables plus AK::Query -- so this
// takes no livesafety::Feature, and it must not take one: an enumerator there is a promise that
// the path goes inert in a live session, and running in a live session is this path's entire
// purpose. It posts nothing, stops nothing, registers nothing and opens no socket.
struct FoleyCensus {
    static constexpr uint32_t kEventCap = 8;
    uint64_t next_ms = 0;
    uint64_t match_since_ms = 0;   // 0 = not in a match right now
    bool     in_match = false;
    bool     deep_done = false;    // this match has had its one deep census
    // The AkExports snapshot tables. Statics, not stack: the compact line runs on the game thread
    // and AudioMix alone is tens of kilobytes.
    IEntity::AudioMix mix;
    AudioStatePair  states[64];
    AudioSwitchTriple switches[512];
};
FoleyCensus g_foley;

// ★ THE DISC DIFF CENSUS (ENG-LIVE-CENSUS-2 item 1). Seven packets tried to work out what hides
// the disc's spinning shell at the first grab by reading the offline image and our own replay
// sessions, where the transition never happens. It happens in a LIVE match, the owner loads this
// same DLL to record live matches, and so the transition can be measured. Everything the census
// reads is a probed load -- no engine call, no write, no livesafety::Feature -- and it runs on a
// TRIGGER, never per frame: first sight of a disc, a change of holder, a change of the disc
// population (a goal respawn re-creates the actor), and otherwise at most once per
// `disc.diff_census_ms`. Between triggers the cost is one probed dword per disc.
struct DiscDiffCensus {
    uint64_t next_ms = 0;
    uint64_t match_since_ms = 0;
    bool     in_match = false;
    bool     have = false;       // `last` holds a snapshot to diff against
    uint64_t space = 0;          // levelspace::Key `last` was taken under; its disc pointers live there
    // Two whole snapshots, as statics: they are a few kilobytes each and this runs on the game
    // thread, where the stack is not ours to spend.
    disccensus::Snapshot last;   // what the previous trigger saw
    disccensus::Snapshot fresh;  // what this trigger sees, before it becomes `last`
};
DiscDiffCensus g_disc;

// One line per disc per trigger, and the diff line is by far the longest: a first grab can move
// several EXECUTION_DATA words at once and each costs 26 characters. Long enough to carry them,
// bounded so the sink's own line handling is never the thing that decides what we learn.
constexpr size_t kDiscLineCap = 1024;

// The disc census is what this key gates; 0 turns it off outright.
constexpr uint64_t kDiscCensusDefaultMs = 10000;
constexpr double kDiscClockMs = 1000.0;  // the disc line prints seconds; every other clock here is ms

void LogDiscCensus(const TickInfo& info, bool inMatch) {
    IEntity* const ent = g.svc ? g.svc->entity : nullptr;
    if (ent == nullptr || !ent->Available() || g.log == nullptr) return;

    disccensus::Input in;
    in.nSpaces = ent->DiscCandidateSpaces(info.netGame, in.spaces, disccensus::Input::kSpaceCap);
    if (in.nSpaces == 0) return;

    const uint64_t now_ms = GetTickCount64();
    // The clock the owner can line a log line up against: seconds since this instrument first saw
    // the netgame in a match. Not the engine's match clock -- that would be one more fact to
    // derive, and a stopwatch started at the same edge answers "when in the match" just as well.
    const double t = g_disc.match_since_ms != 0
                         ? (double)(now_ms - g_disc.match_since_ms) / kDiscClockMs
                         : 0.0;

    disccensus::Snapshot& fresh = g_disc.fresh;
    if (!disccensus::Take(in, fresh)) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "disc census: refused -- none of %u candidate space(s) walked",
                     (unsigned)in.nSpaces);
        return;
    }
    // A full snapshot on first sight and whenever the set of disc actors itself changed -- a goal
    // respawn re-creates the actor, and a diff against a disc that no longer exists is fiction.
    const bool full = !g_disc.have || disccensus::PopulationChanged(g_disc.last, fresh);
    const char* const why = !g_disc.have ? "first sight" : full ? "respawn" : "diff";
    char line[kDiscLineCap];
    for (uint32_t i = 0; i < fresh.discsWritten; ++i) {
        const size_t n = full ? disccensus::Describe(fresh, i, line, sizeof line)
                              : disccensus::Diff(g_disc.last, fresh, i, line, sizeof line);
        if (n == 0) continue;   // nothing moved on this disc: say nothing rather than say so
        g.log->Write(LogLevel::Warn, kReplayModuleName, "disc %s: t=%.1f %s%s", why, t,
                     inMatch ? "" : "(no match) ", line);
    }
    if (full) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "disc census (%s): t=%.1f %u disc(s) in %u of %u space(s), %u system(s) "
                     "walked, %u script(s), %u node(s)",
                     why, t, fresh.discsWritten, fresh.spacesWalked, (unsigned)in.nSpaces,
                     fresh.systemsSeen, fresh.scriptsWritten, fresh.nodesWritten);
    }
    g_disc.last = fresh;
    g_disc.have = fresh.discsWritten != 0;
}

// The residue line's own cadence, deliberately not shared with the foley one: they are the same
// 30 s but they answer to different gates, and one key switching off both would be a surprise.
constexpr uint64_t kResidueLineMs = 30000;
uint64_t g_residue_line_ms = 0;

// The compact 30 s line, from a census already taken. Returns false when nothing could be read.
bool LogFoleyCompact(const char* why) {
    IEntity* const ent = g.svc ? g.svc->entity : nullptr;
    if (ent == nullptr || !ent->Available() || g.log == nullptr) return false;
    IEntity::AudioMix& mix = g_foley.mix;
    if (!ent->AudioMixCensus(mix) || mix.playingRefused != nullptr) {
        g.log->Write(LogLevel::Warn, kReplayModuleName, "foley: refused (%s) -- %s", why,
                     mix.playingRefused ? mix.playingRefused
                                        : (mix.refused ? mix.refused : "unknown"));
        return false;
    }
    const bool haveL = mix.listenersWritten != 0;
    const float lx = haveL ? mix.listeners[0].pos[0] : 0.0f;
    const float ly = haveL ? mix.listeners[0].pos[1] : 0.0f;
    const float lz = haveL ? mix.listeners[0].pos[2] : 0.0f;
    float nearest = -1.0f, farthest = -1.0f;
    uint32_t eventId[FoleyCensus::kEventCap] = {};
    uint32_t eventN[FoleyCensus::kEventCap] = {};
    uint32_t events = 0, eventsSeen = 0;
    for (uint32_t i = 0; i < mix.playingWritten; ++i) {
        const IEntity::AudioPlayingSound& s = mix.playing[i];
        if (haveL) {
            const float dx = s.emitterPos[0] - lx, dy = s.emitterPos[1] - ly,
                        dz = s.emitterPos[2] - lz;
            const float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (nearest < 0.0f || d < nearest) nearest = d;
            if (d > farthest) farthest = d;
        }
        uint32_t at = FoleyCensus::kEventCap;
        for (uint32_t k = 0; k < events; ++k)
            if (eventId[k] == s.eventId) { at = k; break; }
        if (at != FoleyCensus::kEventCap) {
            ++eventN[at];
        } else if (events < FoleyCensus::kEventCap) {
            eventId[events] = s.eventId;
            eventN[events++] = 1;
        } else {
            ++eventsSeen;
        }
    }
    char ev[256];
    int n = 0;
    for (uint32_t k = 0; k < events && n >= 0 && n < (int)sizeof ev - 1; ++k)
        n += snprintf(ev + n, sizeof ev - (size_t)n, " %u x%u", eventId[k], eventN[k]);
    if (n == 0) snprintf(ev, sizeof ev, " none");
    g.log->Write(LogLevel::Warn, kReplayModuleName,
                 "foley (%s): %u voice(s) on %u of %u emitter(s); playing-voice distance to "
                 "listener[0] min %.1f max %.1f m; %u listener(s); %u emitter(s) on the 2D "
                 "listener; sounds %llu of %llu (%llu active), emitters free %llu; wwise voices "
                 "%u of %u cap, threshold %.1f dB%s; event(s)%s%s",
                 why, mix.playingSeen, mix.emittersActive, mix.emittersWritten, (double)nearest,
                 (double)farthest, mix.listenersWritten, mix.listener2dEmitters,
                 (unsigned long long)(mix.soundPoolCap - mix.soundPoolFree),
                 (unsigned long long)mix.soundPoolCap, (unsigned long long)mix.soundsActive,
                 (unsigned long long)mix.emitterPoolFree,
                 mix.voiceHigh - mix.voiceLow, mix.maxVoices, (double)mix.volumeThresholdDb,
                 mix.poolRead && mix.limitsRead ? "" : " (some gauges unread)", ev,
                 eventsSeen != 0 ? " (+more, event table full)" : "");
    return true;
}

// The deep read: the game-sync context every switch or state container resolves against at post
// time (RE-AUDIO-CUTOFF R1's discriminator), then the whole object sub-tree of every playing voice
// (R2's). The counts go LAST because the log sink keeps the newest lines of a burst, and the counts
// are what the answer turns on -- "our pawn emitters carry no switch groups at all" is a verdict a
// dropped row would have hidden.
uint32_t LogFoleySyncCensus(const char* why) {
    if (g.log == nullptr) return 0;
    IAudioState* const st = g.svc ? g.svc->audioState : nullptr;
    if (st == nullptr) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "foley sync (%s): refused -- no CSoundPlayer was resolved, so there is no "
                     "switch or state memo to walk", why);
        return 1;
    }
    uint32_t stateTotal = 0, switchTotal = 0;
    const size_t nStates = st->SnapshotStates(g_foley.states,
                                              sizeof g_foley.states / sizeof g_foley.states[0],
                                              &stateTotal);
    const size_t nSwitches = st->SnapshotSwitches(
        g_foley.switches, sizeof g_foley.switches / sizeof g_foley.switches[0], &switchTotal);
    uint32_t lines = 0;
    for (size_t i = 0; i < nStates; ++i) {
        g.log->Write(LogLevel::Warn, kReplayModuleName, "foley sync state[%u]: group %u value %u",
                     (unsigned)i, g_foley.states[i].group, g_foley.states[i].value);
        ++lines;
    }
    // Per game object, in the order the emitter map holds them, so a run of rows with one gameObj
    // is one emitter's whole context and the groups a pawn emitter is MISSING are countable.
    for (size_t i = 0; i < nSwitches; ++i) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "foley sync switch[%u]: obj %u group %u value %u", (unsigned)i,
                     g_foley.switches[i].gameObj, g_foley.switches[i].group,
                     g_foley.switches[i].value);
        ++lines;
    }
    uint32_t objects = 0;
    for (size_t i = 0; i < nSwitches; ++i) {
        bool seen = false;
        for (size_t k = 0; k < i && !seen; ++k) seen = g_foley.switches[k].gameObj ==
                                                       g_foley.switches[i].gameObj;
        if (!seen) ++objects;
    }
    g.log->Write(LogLevel::Warn, kReplayModuleName,
                 "foley sync (%s): %u global state group(s) of %u; %u per-object switch row(s) of "
                 "%u across %u game object(s). Every value is Wwise's own answer, not the engine's "
                 "cache. A pawn emitter with NO row here resolves every container it posts to the "
                 "bank default",
                 why, (unsigned)nStates, stateTotal, (unsigned)nSwitches, switchTotal, objects);
    return lines + 1;
}

void Tick(const TickInfo& info, void* /*ctx*/) {
    // ★ PERF-1's instrument. `players_published` is the previous tick's figure, which is what makes
    // the per-player arithmetic in the `cost:` line readable without a second source of truth.
    const tickcost::Scope cost_root(tickcost::kRootTick);
    tickcost::NoteFrame(g.last_apply.players_published);
    mainprof::NoteFrameThread();
    // The provisioner admits through the CR15NetGame THIS TICK was handed, not through a second
    // read of the singleton: the tick is the one place the session's netGame is already known to
    // be the live one, and it is the surface a test can drive.
    g.tick_net_game = info.netGame;
    RebindServices();
    ++g.ticks;
    if (!g.audio_residue_bound && audio::AudioResidueStateBound()) {
        g.audio_residue_bound = true;
        g.audio_residue_bound_tick = g.ticks;
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName, "audio residue: bound at tick %llu",
                         (unsigned long long)g.ticks);
        }
    }
    // ★ NOT BEHIND `loaded`.  The seat is decided by ApplyCommandLine
    // and by the admit path, in the lobby, before any .echodemo is opened and whether or not one
    // ever is.  Behind the load gate, a session whose fixture failed to open emitted NEITHER the
    // census line NOR ViewerNotSpectator, so the acceptance's "no ViewerNotSpectator anywhere"
    // clause was satisfied by a run that never asked -- asked-and-passed and never-asked are not
    // the same answer.  LogViewerRole is self-gated (log, latch, netGame, readable, non-zero local
    // user count), so the load gate never added a precondition here; it only added silence.
#ifndef VIEWER_LINE_BEHIND_LOAD
    // The SEAT before the census, so the line reports the record as it stands after this
    // frame's issue rather than the one that provoked it.
    {
        const tickcost::Scope cost(tickcost::kSeat);
        const SeatScope seat_scope = SessionSeatScope();
        SeatViewer(seat_scope);
        LogViewerRole(seat_scope);
    }
#endif
    // ★ AUDIO-FIX-2 item 3, and ABOVE the load gate for the same reason the seat census is: the
    // session this reading exists to compare against is a LIVE spectate, where nothing is loaded
    // and the gate below would make the instrument silent in exactly the one place it is needed.
    if (g.foley_census_ms != 0) {
        const uint64_t now_ms = GetTickCount64();
        const bool inMatch = NetGameStateOf(info.netGame) == (int32_t)NetGameState::InGame;
        if (inMatch && !g_foley.in_match) {
            g_foley.match_since_ms = now_ms;
            g_foley.deep_done = false;
        }
        if (!inMatch && g_foley.in_match) {
            // Match end. The deep read is taken here whether or not the 60 s one fired, because a
            // match that ended before then still carried the context we came for.
            LogFoleyCompact("match end");
            LogFoleySyncCensus("match end");
        }
        g_foley.in_match = inMatch;
        if (now_ms - g_foley.next_ms >= g.foley_census_ms) {
            g_foley.next_ms = now_ms;
            LogFoleyCompact(inMatch ? "in match" : "no match");
        }
        if (inMatch && !g_foley.deep_done &&
            now_ms - g_foley.match_since_ms >= g.foley_deep_at_ms) {
            g_foley.deep_done = true;
            LogFoleySyncCensus("60 s into the match");
        }
    }
    // ★ The disc diff census, ABOVE the load gate for the same reason: the session it exists to
    // measure is a live spectate, where nothing is loaded.
    if (g.disc_census_ms != 0) {
        const uint64_t now_ms = GetTickCount64();
        const int32_t state = NetGameStateOf(info.netGame);
        const bool inMatch = state == (int32_t)NetGameState::InGame;
        if (inMatch && !g_disc.in_match) g_disc.match_since_ms = now_ms;
        g_disc.in_match = inMatch;
        // `last` resolved its disc pointers in one space; outside a keyed space they are not
        // followed, and a new space takes a first-sight snapshot of its own.
        const uint64_t space =
            g.svc && g.svc->entity ? LevelSpaceNow(*g.svc->entity, state) : 0;
        if (space == 0 || space != g_disc.space) g_disc.have = false;
        g_disc.space = space;
        // The per-tick term, and the only one: one probed dword per disc, through the pointer the
        // last snapshot resolved. A holder edge is what the owner's sentence is about, so it takes
        // a snapshot immediately rather than waiting for the interval.
        uint32_t holders[disccensus::Snapshot::kDiscCap] = {};
        uint32_t nHolders = 0;
        const bool moved =
            g_disc.have && disccensus::HolderMoved(g_disc.last, holders,
                                                   disccensus::Snapshot::kDiscCap, nHolders);
        // No `|| !have` term: with no disc in the level -- a lobby, a loading screen -- `have`
        // stays false forever, and a trigger that reads it would walk every space every frame.
        if (moved || now_ms - g_disc.next_ms >= g.disc_census_ms) {
            g_disc.next_ms = now_ms;
            LogDiscCensus(info, inMatch);
        }
    }
    if (!g.loaded) {
        return;
    }
    ++g.frames;
    // RELOAD-6: nothing of this load reaches the engine while its level is being re-entered, and
    // the held span is not played.
    if (HoldForLevelCycle()) {
        (void)WallDeltaUs();
        return;
    }
    tickcost::Scope cost_prov(tickcost::kProvision);
    // The seek battery's own account, so a census log shows what a jump did to the mix. Since
    // FIX-AUDIO-B a jump stops and re-posts nothing -- the engine keeps every voice it started -- so
    // this line carries only what the battery does do, beside the live loop count to compare with.
    // BELOW the load gate: there is no seek in a live spectate.
    if (g.foley_census_ms != 0) {
        const uint64_t now_ms = GetTickCount64();
        if (now_ms - g_residue_line_ms >= kResidueLineMs) {
            g_residue_line_ms = now_ms;
            const audio::AudioResidueStats& rs = audio::AudioResidueStatsRef();
            uint32_t live = 0;
            if (IAudioState* const st = g.svc ? g.svc->audioState : nullptr) {
                AudioLoopPin none{};
                st->SnapshotLoops(&none, 0, &live);   // cap 0: the total is the whole question
            }
            if (g.log) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "residue: stopped 0, re-posted 0, live %u (%u mute(s), %u pin "
                             "restore(s), %u reconcile(s), %u clock rebase(s))",
                             live, rs.mutes, rs.pinsRestored, rs.reconciles, rs.clockRebases);
            }
        }
    }
    // BELOW the load gate, and self-gated on the predicate.
    ApplyLodThrottle();
    // Asked every loaded tick, not at the gate below: the watch has to see the match to see it end.
    const bool in_match =
        PawnProvision::SessionBlocker(info.netGame) == PawnProvision::Refusal::None;
    const bool session_over = g.session.Ended(in_match);
    MaintainNetVarBind();
#ifdef VIEWER_LINE_BEHIND_LOAD
    const SeatScope seat_scope = SessionSeatScope();
    SeatViewer(seat_scope);
    LogViewerRole(seat_scope);
#endif
    // Before dressing and before any publish this frame: if the arm can be turned on now, the
    // roster this frame publishes must already have been checked against it.
    RetryArmOwnerGuard();
    // ★ AND BEFORE THE DRESS PASS, not inside it (L3 #26 F1).  The dress pass defers for as long
    // as the level takes to come up; the provisioner must be pumping across that whole wait, which
    // is exactly the span the entrant lands in.
    ArmProvisioner();
    cost_prov.Close();
    {
        const tickcost::Scope cost(tickcost::kDress);
        TryDressRoster();
    }
    tickcost::Scope cost_prov2(tickcost::kProvision);
    // The pump the provisioner's retry budget and its census both wait on. Nothing else in
    // src/ calls Pump(), so before this the ProvisionEnv census inputs wired on the dress
    // path could not print on any shipped run, and a slot deferred by BudgetExceeded stayed
    // deferred for the life of the load. Armed-only: an unarmed provisioner has no env.
    // ★ THE CALL SITE THE REPLY ROUTE NEEDS, AND IT IS NOT THIS ONE.  ReplyRouteArm registers a
    // listener on the netgame's own broadcaster; the engine invokes it from its receive pump, and
    // that callback -- not the Pump below -- is where the entrant-added message is published.  All
    // the tick does is arm it once and let the provisioner queue.  `pawn.reply_route` false disarms
    // the whole route (the direct insert is then the shipping path, unchanged); it is a declared
    // config row, so the ledger dump and the launch record read the same answer this does, and its
    // derived environment name ECHOREPLAY_PAWN_REPLY_ROUTE is the kill switch a launch can carry.
    // WHICH messages an offline solo host publishes on that node is a registry fact, not an
    // operator knob: the listen set is identity_access's own, and nullptr asks for it.
    if (!g.reply_route_tried && info.netGame != nullptr) {
        g.reply_route_tried = true;
        if (config::Interface()->GetBool("pawn.reply_route", true)) {
            g.reply_tap = WiretapCreate(info.netGame);
        }
        g.reply_route_armed =
            g.reply_tap != nullptr &&
            identity_access::ReplyRouteArmWith(info.netGame, g.reply_tap, nullptr);
        // Install the dedicated always-armed NETGAME.UPDATE drain driver HERE, not at module init:
        // this runs once a session is up, so the tick has already resolved its mode.  Under
        // tick.mode=slot -- the run in which the entrant parks -- NETGAME.UPDATE is pristine and this
        // installs; under tick.mode=hook, tick's own fallback took the site first and this refuses by
        // prologue rather than colliding.  The drain is what runs the engine's own admission
        // (SNSLobbyEntrantAdded -> AddRemoteUser) offline, so it must be driven from an engine-update
        // stack that fires in-match, which the GUI heartbeat did not.
        if (g.reply_route_armed) identity_access::InstallReplyDrainDriver();
        if (g.log) {
            const identity_access::ReplyRouteCensus c = identity_access::ReplyRouteStatus();
            g.log->Write(LogLevel::Info, kPawnModuleName,
                         "reply route: %s (promoted=%s, listeners=%u, symbols=%s)%s%s",
                         g.reply_route_armed ? "armed" : "not armed", c.promoted ? "yes" : "no",
                         unsigned(c.listeners), identity_access::ReplyRouteDefaultSymbols(),
                         c.diagnostic[0] ? " -- " : "", c.diagnostic);
        }
    }
    // ★ AND THE PUMP IS NOT DOWNSTREAM OF THE ROUTE ABOVE.  The reply route is one PATH into the
    // provisioner's table; whether the engine's broadcaster took our listener says nothing about
    // whether the table has work, and on a solo offline host the arm can fail outright while the
    // direct insert still seats an entrant.  A pump gated on it would go quiet on exactly the
    // host this viewer is built for.
#ifdef W2P1_MUTATE_PUMP_GATED_ON_REPLY_ARM
    if (g.reply_route_armed && g.pawn.Armed()) {
#else
    if (g.pawn.Armed()) {
#endif
        g.pawn.Pump();
    }

    // ★ EVERY TICK, BESIDE THE PUMP -- not once per load from the dress pass.  The dress pass is
    // one producer of mappings and it runs exactly once; `Provision` assigns a slot's live number
    // wherever it is first reached, and a slot that is queued (`Request`) and mapped by a later
    // `Pump` would otherwise stay on the identity map for the life of the session -- the bind then
    // asks that player's RECORDED slot, which is the whole defect this map exists to close.  The
    // refresh is a compare against what is already installed and returns false on every tick that
    // mapped nothing, so the re-bind fires only when the map actually moved.
#ifndef W2P1_MUTATE_SLOTMAP_REFRESH_ONCE
    if (RefreshSlotMap()) BindBodyHandlesIfReady("slot map installed");
#endif
    cost_prov2.Close();

    // 1. the playhead advance, from REAL time
    const uint64_t wall = WallDeltaUs();
    const StepResult step = g.transport.Advance(wall);
    // clock.h:123: "Advance the sim by exactly `stepUs` microseconds on the next heartbeat.
    // NEVER 0." transport.h:235 names the caller: "The caller writes `commanded_step_us` to the
    // clock". Pushed EVERY frame, playing or paused -- Advance answers 1 us while paused
    // (S7.3.3), and a paused clock with no step armed reads a raw delta of 0, which clock.cpp
    // measured turning into +inf at 0x1400CF6FB. Pause is the engine's state, not the transport's.
    // ★ ABOVE THE WINDOW GATES BELOW: a refused window and a still-dressing roster are exactly the
    // frames the pre-roll spans, and its wall bound has to keep running through them.
    UpdatePreRoll(wall, info.simTimeUs);
    // ★ AND THE PER-FRAME STEP PUSH IS SUPPRESSED WHILE IT RUNS. Advance answers 1 us while the
    // transport is paused; pushed during a pre-roll it is a 1 us step request every frame against a
    // clock that is deliberately not paused, which is the one write that could make the pre-roll
    // fictional. The transport is still Advanced above -- only the push is held.
    // ★ AND WHILE A DRAG RUNS, for the mirror image of that reason. The clock's mirror arms one
    // beat per scrub step -- the beat the player bodies re-latch on -- and a 1 us push landing in
    // the same slot would overwrite it, or overwrite a beat the every-other-frame guard is still
    // carrying. The engine is pause-bit held throughout, so the minElapsed floor, not this push, is
    // what keeps its raw delta off zero.
    if (g.svc->clock != nullptr && !PreRollActive() && !g.scrub.active) {
        g.svc->clock->SetStepUs(step.commanded_step_us);
        ++g.clock_steps_pushed;
    }

    // 1a. THE END OF THE RECORDING, SAID ONCE. "The decoder is behind" and "there is nothing left
    // to decode" looked identical in the log -- one hold per frame, forever -- and they are the two
    // things an operator watching a stopped picture most needs told apart.
    if (step.end_state != g.end_state_said) {
        g.end_state_said = step.end_state;
        if (g.log && step.end_state != EndState::None) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "%s of the recording at %lld us: the playhead is clamped there and playback "
                         "is paused. This is the end of the file, not a window waiting to decode. "
                         "Play restarts from the other end; seek moves away from it.",
                         step.end_state == EndState::AtEnd ? "END" : "START",
                         (long long)(g.transport.DisplayPlayheadUs() -
                                     (int64_t)g.reader.GetManifest().t0Us));
        }
    }

    // 1b. ONCE A SECOND, WHAT THE PLAYHEAD DID PER REAL SECOND. Above every early return, because
    // a refused window and a session-over tick are exactly when a viewer wants to know whether the
    // transport is still keeping time. Reported as the measured ratio against the commanded rate:
    // 1.00x of 1.00x is the whole assertion, and a sprint shows as a ratio nobody can read past.
    if (g.loaded) {
        const int64_t here = g.transport.DisplayPlayheadUs();
        if (!g.pace_armed) {
            g.pace_armed = true;
            g.pace_playhead_us = here;
            g.pace_wall_us = 0;
            g.pace_frames = g.pace_clamps = 0;
            g.pace_dropped_us = 0;
        }
        g.pace_wall_us += wall;
        ++g.pace_frames;
        if (step.wall_dropped_us != 0) {
            ++g.pace_clamps;
            g.pace_dropped_us += step.wall_dropped_us;
        }
        // ★ THE HITCH RECORDER. A 1 Hz frame count cannot tell nine 111 ms frames from one 900 ms
        // stall, and it charged nothing to the audio module at all -- which is why 22 seconds of
        // 8 fps could be correlated with the audio module's degraded loop and never attributed.
        // This line names what the plugin was doing on the frame that was late.
        const double dt_ms = double(wall) / 1000.0;
        tickcost::NoteFrameDt(dt_ms);
        uint32_t hitch_suppressed = 0;
        if (g.log && tickcost::HitchDue(dt_ms, g.pace_clamps, &hitch_suppressed)) {
            char frame_cost[256];
            tickcost::FormatFrameCost(frame_cost, sizeof frame_cost);
            char audio_state[256];
            audio_state[0] = '\0';
            if (g.svc != nullptr && g.svc->audioRate != nullptr) {
                g.svc->audioRate->DescribeState(audio_state, sizeof audio_state);
            }
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "hitch: dt=%.1fms frames_this_second=%u clamps=%u | %s | rate=%.2fx "
                         "state=%s playhead=%.3fs holds=%llu microseeks=%llu | audio: %s | "
                         "suppressed=%u",
                         dt_ms, g.pace_frames, g.pace_clamps, frame_cost, g.transport.Rate(),
                         g.transport.Paused() ? "pause" : "play",
                         double(g.transport.DisplayPlayheadUs()) / 1e6,
                         (unsigned long long)g.transport.CoverageHolds(),
                         (unsigned long long)g.transport.MicroSeeks(), audio_state,
                         hitch_suppressed);
        }
        if (g.pace_wall_us >= kUsPerSecond) {
            if (g.log) {
                const double secs = double(g.pace_wall_us) / 1e6;
                const char* endword = "";
                switch (g.transport.Ended()) {
                    case EndState::AtEnd:   endword = " END-OF-RECORDING"; break;
                    case EndState::AtStart: endword = " START-OF-RECORDING"; break;
                    case EndState::None:    endword = step.held_for_coverage ? " waiting-for-decode" : ""; break;
                }
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "transport pacing: playhead %+.3f s per %.3f s real = %.3fx "
                             "(rate %.2fx%s%s) | holds=%llu ends=%llu retables=%llu | clamps %u of "
                             "%u frame(s) this second, %.3f s dropped (total %llu, %.3f s) | "
                             "microseeks=%llu gaps=%llu | hermite %s: cubic=%u lerp=%u "
                             "(teleport=%u gap=%u velocity=%u clamp=%u) | smooth %s/%s w=%.2f "
                             "brackets=%u fits=%u degen=%u edge_raw=%u held=%u qslerp=%u",
                             double(here - g.pace_playhead_us) / 1e6, secs,
                             double(here - g.pace_playhead_us) / 1e6 / secs,
                             g.transport.Rate(),
                             PreRollActive() ? " PRE-ROLL (the playhead is held on purpose)"
                                             : (g.transport.Paused() ? " paused" : ""),
                             endword,
                             (unsigned long long)g.transport.CoverageHolds(),
                             (unsigned long long)g.transport.EndClamps(),
                             (unsigned long long)g.transport.RetableJumps(),
                             g.pace_clamps, g.pace_frames, double(g.pace_dropped_us) / 1e6,
                             (unsigned long long)g.transport.WallClamps(),
                             double(g.transport.WallDroppedUs()) / 1e6,
                             (unsigned long long)g.transport.MicroSeeks(),
                             (unsigned long long)g.transport.GapsCrossed(),
                             g.hermite_on ? "on" : "off", g.hermite_tally.cubic,
                             g.hermite_tally.Lerped(), g.hermite_tally.teleport,
                             g.hermite_tally.gap, g.hermite_tally.velocity,
                             g.hermite_tally.clamp, SmoothModeName(g.smooth_mode),
                             SmoothStrengthName(g.smooth_strength), double(g.smooth_w),
                             g.smooth_tally.brackets, g.smooth_tally.fits,
                             g.smooth_tally.degenerate, g.smooth_tally.edge_raw,
                             g.smooth_tally.held, g.smooth_tally.quat_slerp);
                // ★ ONLY WITH THE KEY ON. With it off this is a line about a mechanism that is not
                // running, and the log budget is already the thing the last packet could not close.
                //
                // ★★ WHAT TO READ, IN ORDER. `tick_decodes/min` IS the defect: a three-chapter
                // decode on the game's own thread. In steady playback at any rate it must be ~0 and
                // a seek must cost exactly one, so a rate that climbs while nobody is seeking means
                // the worker is losing the race and the hitch is still there. `late` counts frames
                // the playhead wanted the next window and the worker had not landed it -- a few at
                // 4x is the threshold being tight, a steady stream is the worker being too slow.
                // `adopt_us` is what an adoption costs the tick: the swap and the re-bind, with no
                // decode in it, and it is the number that replaces the 9-13 ms.
                if (AsyncWindowsOn()) {
                    const winprefetch::Counters wc = winprefetch::ReadCounters();
                    const uint64_t now_us = WallNowUs();
                    const double span_s = g.async_line_at_us != 0
                                              ? double(now_us - g.async_line_at_us) / 1e6
                                              : 0.0;
                    const uint64_t decodes_this_span = g.tick_decodes - g.tick_decodes_at_line;
                    g.log->Write(LogLevel::Info, kReplayModuleName,
                                 "window async: tick_decodes=%llu (%.1f/min) adopted=%llu "
                                 "adopt_us last=%llu max=%llu | late=%llu stale=%llu | worker "
                                 "requests=%llu coalesced=%llu decodes=%llu refused=%llu "
                                 "mean_decode_ms=%.1f | ready=%s free=%u",
                                 (unsigned long long)g.tick_decodes,
                                 span_s > 0.0 ? double(decodes_this_span) * 60.0 / span_s : 0.0,
                                 (unsigned long long)g.adoptions,
                                 (unsigned long long)g.adopt_us_last,
                                 (unsigned long long)g.adopt_us_max,
                                 (unsigned long long)g.prefetch_late,
                                 (unsigned long long)g.prefetch_stale,
                                 (unsigned long long)wc.requests,
                                 (unsigned long long)wc.coalesced,
                                 (unsigned long long)wc.decodes,
                                 (unsigned long long)wc.refused,
                                 wc.decodes ? double(wc.decode_us) / double(wc.decodes) / 1000.0
                                            : 0.0,
                                 g.ready != nullptr ? "yes" : "no",
                                 unsigned((g.free_win[0] != nullptr ? 1u : 0u) +
                                          (g.free_win[1] != nullptr ? 1u : 0u)));
                    g.tick_decodes_at_line = g.tick_decodes;
                    g.async_line_at_us = now_us;
                }
            }
            // Beside the fps meter on purpose: the pacing line says how many frames the second had,
            // and this one says how many microseconds of each were ours.
            char cost_line[448];
            if (g.log && tickcost::FormatAndReset(cost_line, sizeof cost_line)) {
                g.log->Write(LogLevel::Info, kReplayModuleName, "%s", cost_line);
            }
            // The distribution behind that frame count, on its own line so the pacing format nobody
            // parses stays the shape it was.
            const tickcost::DtSummary dt = tickcost::TakeDtSummary();
            if (g.log && dt.frames != 0) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "frame dt: p50=%.1f p95=%.1f p99=%.1f max=%.1f ms over %u frame(s)",
                             dt.p50, dt.p95, dt.p99, dt.max, dt.frames);
            }
            g.pace_playhead_us = here;
            g.pace_wall_us = 0;
            g.pace_frames = g.pace_clamps = 0;
            g.pace_dropped_us = 0;
            g.hermite_tally = HermiteTally();
            g.smooth_tally = SmoothTally();
        }
    } else {
        g.pace_armed = false;
    }

    // 1b. DESIGN-UI-2 S5.3 step 7: the newest drag target, applied once, ABOVE the discontinuity
    // it raises -- so the residue battery that collapses the engine's time blend fires on the same
    // frame the bodies moved, which is the whole of "it snaps".
    g_transport.ApplyPendingScrub();
    ScrubStepGuard scrub_step_guard;

    // 2. the discontinuity, if any -- consumed once, and it is what a jump costs
    int64_t from = 0, to = 0;
    Cause cause = Cause::Seek;
    bool jumped = g.transport.TakeDiscontinuity(from, to, cause);
    // A user's jump lands exactly: the mode crossfade is for a click the owner watches, not a seek.
    if (jumped && cause == Cause::Seek) g.smooth_snap = true;

    // ★ A DIRECTION FLIP IS A DISCONTINUITY. The playhead does not jump, but every dt-driven filter
    // downstream -- the engine's net-physics time blend above all -- is now holding history that
    // points the wrong way, and left alone it converges out of it over seconds with the disc sitting
    // beside the hand holding it. N-4 collapses that blend, so the flip is routed through the same
    // battery a seek is. Not raised when a real jump is already pending: one battery pass per tick,
    // and the seek is the stronger statement.
    const double rate_now = g.transport.Rate();
    const int rate_sign = rate_now < 0.0 ? -1 : (rate_now > 0.0 ? 1 : 0);
    if (rate_sign != 0) {
        if (!jumped && g.last_rate_sign != 0 && rate_sign != g.last_rate_sign) {
            from = to = g.transport.SolverPlayheadUs();
            cause = Cause::ReverseTick;
            jumped = true;
        }
        g.last_rate_sign = rate_sign;
    }

    // S10.4.1a. The audio sink carries about 85 ms of buffering, FLAT IN SCENE TIME -- so the wall
    // offset a viewer perceives is 85/r ms and reaches 853 ms at 0.1x, the mode the owner asked for
    // by name. Presenting from `display - bias` delays the picture by exactly what the audio is
    // already carrying, in the units the buffer is constant in, so one subtraction serves every
    // rate and no per-rate table exists to get wrong. Zero until W2-H's measurement latches.
    //
    // ★ The subtraction is on the DISPLAY playhead only; the transport's own position and the
    // solver playhead are untouched, so seeking and the timeline still report where the user is.
    // The window follows the biased time deliberately: the window exists to cover what is
    // presented, and choosing its centre from an instant we do not present is how a boundary seek
    // builds a span that does not contain its own sample point. W2-H S7.3 words this as "the
    // window centre is untouched"; DESIGN S10.4.1a's rule is display-not-solver, and that is the
    // one followed here.
    int64_t t = g.transport.DisplayPlayheadUs();
    g.present = PresentSnapshot();
    g.present.transport_us = t;
    if (g.svc->audioRate) {
        g.present.bias_us = g.svc->audioRate->DisplayBiasSceneUs();
        const int64_t origin = (int64_t)g.reader.GetManifest().t0Us;
        const int64_t biased = t - g.present.bias_us;
        // Never before the recording begins: there is no picture there, and a playhead outside the
        // file would count an apply failure every frame for the first 85 ms of every load.
        t = biased < origin ? origin : biased;
    }
    g.present.presented_us = t;
    tickcost::Scope cost_drive(tickcost::kDrive);
    DriveScoreGate(t);

    // Before the window and whatever the window says: the pose is a function of source time, so a
    // frame whose window refused still renders from the pose its source instant asks for. A camera
    // that stalled with the picture would make the image gate compare two viewpoints and read the
    // difference as a fidelity defect.
    DriveOracleTrack(t);
    cost_drive.Close();

    // 3. the window: rebuilt when the playhead leaves the centre chapter, or after a jump that left
    //    the resident span entirely. The verdict is held rather than returned on, because step 4 owes
    //    work whichever way it went.
    // ★ G2 OF DESIGN-UI-2 S5.1, AND IT IS THE HALF THAT MATTERS. This runs EVERY FRAME, so a seek
    // that moved G1 alone only moved the three-chapter decode one tick later: the stall was never
    // removed. Both guards read the span a sample actually needs -- the resident OUTER span -- and
    // the lookahead the centre-chapter test used to provide is paid back by the idle re-centre
    // below, which runs a whole chapter earlier than this would have stalled.
    const uint64_t builds_before_window = g.window_builds;
    bool window_ready;
    {
        const tickcost::Scope cost(tickcost::kWindow);
        // One acquire-load when there is nothing to take, which is every frame but one per window.
        if (AsyncWindowsOn()) {
            CollectPrefetch();
        }
        window_ready = (g.window_valid && Win().CoversForSampling(t));
        // ★ THE COVERING GUARD'S ASYNC ARM. A window the worker already decoded is adopted rather
        // than decoded again -- this is the arm a SEEK's neighbour warm lands on, and the one that
        // catches a boundary the threshold below was too late to cover.
        if (!window_ready && AdoptPrefetched(t)) {
            window_ready = true;
        }
        if (!window_ready) {
            window_ready = RebuildWindow(t, g.scrub.active && g.scrub.light);
        }
        g.scrub_step.tick_builds = g.window_builds - builds_before_window;

        // The lookahead half, never inside a drag and at most once per tick. Paused or scrubbing
        // means both edges: there is no direction of travel to look ahead in.
        if (window_ready && !g.scrub.active) {
            const double rate_for_dir = g.transport.Rate();
            const int dir = g.transport.Paused()
                                ? 0
                                : (rate_for_dir < 0.0 ? -1 : (rate_for_dir > 0.0 ? 1 : 0));
            if (Win().RecentreMargin(t, dir)) {
                // ★★ WITH THE WORKER ON, A RE-CENTRE THAT IS NOT READY IS SKIPPED, NOT DECODED.
                // RecentreMargin is lookahead, not coverage: the playhead is still inside a span
                // this window can answer for, so waiting one more frame costs the viewer nothing,
                // while decoding here is the 9-13 ms hitch this packet exists to delete. The
                // covering guard above is the backstop if the worker never lands it, and `late`
                // counts every frame that happened so the operator can see it.
                if (AsyncWindowsOn()) {
                    if (!AdoptPrefetched(t)) {
                        ++g.prefetch_late;
                        if (CoverageRunningOut(t, dir)) {
                            ++g.prefetch_fallbacks;
                            RecentreWindow("late");
                        }
                    }
                } else {
                    RecentreWindow("idle");
                }
            }
            RequestPrefetch(t);
        }
    }

    // The recorded identity is a function of the PRESENTED instant, exactly like the pose: an
    // entrant who changed loadout mid-match wears what the file recorded before this moment, in
    // both directions, and the read is a search rather than a state a backward seek would have to
    // undo. Outside the window build deliberately -- a block defining an entrant sits wherever it
    // changed, usually chapters away from the one being played.
    identity::PlaybackRecordedIdentity().SetHorizonUs(t < 0 ? 0 : (uint64_t)t);

    // 4. INV-P: a cache is invalidated on a discontinuity, never updated.
    //
    // ★★★ INV-D: A DISCONTINUITY TAKEN FROM THE TRANSPORT IS FIRED ON THE TICK IT WAS TAKEN, WHETHER
    // OR NOT THE WINDOW SURVIVED.
    //
    // TakeDiscontinuity CONSUMES -- the transport has no second copy and nothing can ask again -- so
    // the `return` that used to sit inside step 3 dropped the entire residue pass for that jump on the
    // floor, permanently and silently. It cost nothing while every row was an unmapped no-op, which is
    // why it survived review. It costs the session now: the rows that stop the sounds from before the
    // jump, reconcile the Wwise state memo and re-arm the loops all live in this battery, and the
    // symptom of skipping them is audio quietly wrong for the rest of the session with nothing
    // failing anywhere.
    //
    // ★ A REFUSED WINDOW IS A REASON TO RUN THEM, NOT TO SKIP THEM. No residue row reads the Window:
    // W2-H's operate on Wwise and on this packet's own clocks, W3-A's on engine component systems. A
    // build that refused means the picture is about to stop at the old frame -- exactly when leaving
    // the old audio running is worst.
    if (jumped) {
        const tickcost::Scope cost(tickcost::kJump);
        Win().InvalidateCursors();
        // The stat feed answers from a search, so a jump costs it nothing but the memo of what it
        // last wrote -- which is exactly what must not survive a seek that moved the answer.
        statsfeed::Invalidate();
        // The goal feed answers from a search too, but a jump costs it more than a memo: the span
        // it would otherwise sweep is a span the playhead did not travel, and firing across it
        // would put a banner on screen for a goal the viewer seeked PAST. The cursor re-bases and
        // the level restore -- the banner DATA, without the raise -- is what the next sweep does.
        goalfeed::NoteSeek();
        // The stun feed has the biggest level half of the three: about a seventh of the owner's
        // 105 s is inside a stun span, so a seek landing mid-stun is common, not exotic. The next
        // sweep restores every victim's punchable record to what the playhead implies and raises
        // nothing -- a stun sound at a seek destination is the artefact, not the stun.
        stunfeed::NoteSeek();
        // Same rule, harder: an event feed has no level half to restore, so a jump consumes the
        // span and raises nothing. A tube that launches at a seek destination is worse than a tube
        // that stays still.
        componentfeed::NoteSeek();
        // Same reason as the rebuild's: a seek that lands inside the resident span rebuilds nothing,
        // and the cells would otherwise keep asserting the value from where the user seeked FROM.
        // The forced redraw is for the USER'S jump only -- a catch-up micro-seek and a capture-gap
        // crossing are the transport reporting on itself, and re-writing `game_state` for them
        // pushed a state change at the host's own scripts several times a minute.
        HudNetVarsInvalidate(cause == Cause::Seek);
        if (cause == Cause::Seek) ++g.shell_epoch;
        if (cause == Cause::Seek || cause == Cause::ReverseTick) g.shell_transport_set = true;
        // The authority claim does not survive a jump. The tracks the next apply re-arms are at a
        // different time, so a handle claimed for the old span is authority we no longer write --
        // released BEFORE the re-arm, which is the pairing entity.h:473 asks for. The disc is exempt, for the reason RebuildWindow states:
        // its level is a function of the new playhead, and releasing it published a momentary
        // "nobody" that CR15NetTouchInteractCS reads as a real release and a real grab.
        g.owner_releases += g.applier.ReleaseSyntheticOwners(
            Win(), StateApplier::OwnerRelease::KeepDiscLevel);
        Discontinuity d;
        d.from_us = from;
        d.to_us = to;
        d.cause = cause;
        d.needs_preroll = (cause == Cause::Seek);
        g.battery.Fire(d);
    }

    // ★★★ THE HUD NETVARS ARE PUBLISHED ABOVE EVERY EARLY RETURN, AND THEY READ NOTHING THE
    // APPLIER OWNS. They are a function of the window and T alone, so a refused build, a
    // session-over tick and a playhead the window does not cover are all frames where the clock
    // must still say what the recording says -- they are, in fact, exactly the frames where it used
    // to fall back to the host's `05:00`. Publishing here also pairs the invalidations above with
    // their re-assert inside one tick, so no frame passes with a cleared cell.
    // ★ HOST-HOLD, and it is published above every early return for the reason the HUD netvars are:
    // the hold has to be RELEASED on the tick the replay unloads, and that tick takes one of them.
    {
        const tickcost::Scope cost(tickcost::kDrive);
        hosthold::Policy hp;
        hp.loaded = g.loaded && livesafety::Allowed(livesafety::Feature::HostHold);
        hp.round_time_on = g.host_hold_on;
        hp.pause_on = g.host_hold_pause_on;
        const HostPhase phase = HostPhaseNow();
        hp.playing = phase.known && phase.phase == gamestate::Phase::Playing;
        hp.phase_name = phase.name;
        const hosthold::Want want = hosthold::Step(hp, wall);
        HudNetVarsSetHostHold(want.round_time, want.pause);

        // ONE LINE PER HOST PHASE EDGE. The once-a-minute line below samples; it cannot show a
        // `point_score` the host entered and left between two samples, and it is the sampling that
        // left open whether the host reaches `round_over` at all under the netvar filter. The name
        // strings are program-lifetime rows of kPhaseFacts, so the edge is a pointer compare.
        if (g.log && g.loaded && phase.name != g.host_phase_said) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "host: phase '%s' -> '%s' at playhead %.3f s",
                         g.host_phase_said ? g.host_phase_said : "not published", phase.name,
                         ContentSeconds(t));
        }
        g.host_phase_said = phase.name;

        // Once a minute, so a launch can prove the host stayed in main_loop for longer than its own
        // round -- the one thing no offline test can show.
        g.host_phase_accum_us += wall;
        if (g.host_phase_accum_us >= 60 * kUsPerSecond) {
            g.host_phase_accum_us = 0;
            ++g.host_phase_minutes;
            if (g.log && g.loaded) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "host: phase '%s' at minute %u of this load -- %s %s, %s %s",
                             phase.name, g.host_phase_minutes, HudNetVarsHostHoldName(0),
                             HudNetVarsHostHoldHeld(0) ? "HELD" : "not held",
                             HudNetVarsHostHoldName(1),
                             HudNetVarsHostHoldHeld(1) ? "HELD" : "not held");
            }
        }
    }

    if (g.netvar_bind_wanted) {
        const tickcost::Scope cost(tickcost::kNetVar);
        HudNetVarFrame hud;
        ResolveHudNetVars(Win(), t, hud);
        // The decoder is the frisbee owner's only writer here; unbound, the host keeps the key.
        //
        // ★ AND WHILE A REAL RECORDED HOLDER IS STAMPED, THE HOST'S OWN VALUE IS THE RIGHT ONE.
        // sub_1401C6A40 republishes game_frisbee_ownerid from PhysCI+0x1C every stage-3 tick, and
        // P1 now makes that dword truthful, so swallowing the host write would suppress the correct
        // answer in favour of recorded rows that miss one transition outright and are 15 s stale on
        // another (RE-GRAB-POSSESSION S3.3). The key is only kept during UNOWNED spans, where the
        // dword still carries the out-of-table value the drive needs and the host would republish
        // a user id nobody has.
        const BodyTrack* const owned_disc = DiscTrack();
        const bool host_owner_truthful =
            owned_disc && owned_disc->owner_level_published && owned_disc->owner_level_real &&
            !owned_disc->owner_level_held_over;
        hud.frisbee_owner.due =
            hud.frisbee_owner.due && g.netvar_table_bound && !host_owner_truthful;
        HudNetVarsPublish(hud);
    }

    if (!window_ready) {
        return;
    }

    // The applier installs history nodes and raw pose blocks through component-system pointers
    // cached at bind time; once the match this load was playing in is over, those belong to a
    // session the engine is tearing down. The transport, the clock step and the residue pass above
    // keep running -- they are this module's own state.
    if (session_over) {
        // The match the pre-roll was preparing is gone; there is no host left to run forward.
        EndPreRoll("the match ended", false);
        // Once, on the edge, while the engine still owns the bodies: whatever this load claimed
        // goes back to the host's own simulation.
        if (g.session.ReleaseDue()) {
            g.owner_releases += g.applier.ReleaseSyntheticOwners(Win());
            // The level the disc pool points into goes with the match. A later tick must re-bind
            // rather than read a pointer this session left behind.
            if (g.svc->entity) g.svc->entity->DropDisc();
            // The component system the LOD claim was taken against belongs to that match too.
            ReleaseLodThrottle();
            g.disc_track = Global::kNoDiscTrack;
            g.bind.bound = g.bind.bound > g.bind.bound_disc ? g.bind.bound - g.bind.bound_disc : 0;
            g.bind.bound_disc = 0;
        }
        return;
    }

    // The candidate diagnostic's rate limit runs on real time, like every other cadence here.
    g.disc_early_quiet_us += wall;
    {
        const tickcost::Scope cost(tickcost::kDrive);
        MaintainDiscBind(wall);
    }

    // 5. the only writer of engine state
    //
    // ★ RECORDER-2 part A / L2+L3. The apply pass stamps synthetic ownership on player bodies,
    // publishes their poses and writes their grips -- in a live match those bodies belong to real
    // people. StateApplier stays a pure object driven by its fake entity in tests; the gate goes
    // here, at the one site that hands it the live engine.
    if (!livesafety::Allowed(livesafety::Feature::PlayerPose)) {
        if (!g.live_safety_said) {
            g.live_safety_said = true;
            if (g.log) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "apply pass inert: %s -- no pose, grip, owner or disc write is made",
                             livesafety::VerdictReason(livesafety::Current()));
            }
        }
        return;
    }
    if (!Win().CoversForSampling(t)) {
        ++g.apply_failures;
        return;
    }
    // The pre-apply reading has to be taken BEFORE the install overwrites the ring, and only on
    // the tick that will print -- an unconditional read would cost a CI walk per body per frame
    // for a line emitted once a second.
    const bool will_print =
        (g.position_accum_us + wall) >= kUsPerSecond || g.position_line_pending;
    if (will_print) SnapshotPreApplyPoses();

    ApplyOptions opt;
    opt.as_seen = g.as_seen;
    opt.reversed = g.transport.Rate() < 0.0;
    opt.netvar_owned_elsewhere = &HudNetVarsOwns;
    opt.grip_inputs = g.grip_inputs_on;
    opt.hermite = g.hermite_on;
    opt.hermite_tally = &g.hermite_tally;
    const SmoothFrame smooth = SmoothFrameFor(wall);
    opt.smooth = &smooth;
    opt.hold_disc_pose = HostParksDisc();
    if (opt.hold_disc_pose != g.disc_host_held && g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "disc: %s -- the host is in '%s'",
                     opt.hold_disc_pose
                         ? "leaving the pose to the host while its script parks the frisbee, so "
                           "only one writer moves it"
                         : "driving the recorded pose again",
                     HostPhaseName());
    }
    g.disc_host_held = opt.hold_disc_pose;
    const uint64_t apply0 = g.scrub_step.armed ? WallNowUs() : 0;
    {
        const tickcost::Scope cost(tickcost::kApply);
        g.last_apply = g.applier.Apply(Win(), t, opt);
    }
    if (g.smooth_trace_on) TraceSmoothing(t, wall);
    if (g.scrub_step.armed) {
        g.scrub_step.apply_ms = double(WallNowUs() - apply0) / 1000.0;
    }
    if (g.disc_probe_pending && (g.last_apply.disc_sampled || !DiscTrack()))
        LogDiscProbe(t, info.netGame);

    // 6. the periodic proof that a bound body is somewhere, and somewhere that moves.
    g.position_accum_us += wall;
    if (g.position_accum_us >= kUsPerSecond || g.position_line_pending) {
        g.position_accum_us = 0;
        g.position_line_pending = false;
        LogBoundBodyPositions(t);
    }

    // Last in the tick, so a voice started by this frame's delegate edges is already in the set.
    if (g.voicewatch_ms != 0) {
        const uint64_t now_ms = GetTickCount64();
        if (now_ms - g_voices.next_ms >= g.voicewatch_ms) {
            g_voices.next_ms = now_ms;
            SampleVoices(now_ms);
        }
    }
}

bool Init(ReplayServices* s) {
    REPLAY_REQUIRE(s, log);
    REPLAY_REQUIRE(s, config);
    REPLAY_REQUIRE(s, tick);
    g.svc = s;
    g.log = s->log;

    QueryPerformanceFrequency(&g.qpc_freq);

    size_t placeholders = 0;
    for (size_t i = 0; i < sizeof g_residues / sizeof g_residues[0]; ++i) {
        placeholders += g.battery.Register(&g_residues[i]) ? 1u : 0u;
    }

    // ★★★ THE TWO CALLS TWELVE VERIFIED PRIMITIVES WERE WAITING ON.
    //
    // The battery is a member of a Global in this file's anonymous namespace, so no other packet can
    // reach it. Both suppliers therefore hand up an Arm seam instead of registering themselves, and
    // until these two lines existed neither seam had a caller anywhere in src/ or tests/: W2-H's
    // N-5a/N-16/N-17a/N-17b and W3-A's eight rows were compiled, linked, tested green and never
    // fired. The cost of the audio half is the one the owner would have met first -- a backwards seek
    // never reconciles the Wwise memo, so music and atmosphere stay wrong for the rest of the
    // session, with no error and no log line.
    //
    // ★ This is "verified and wired are different properties", the sentence at the head of this file,
    // arriving a second time for the same reason: every test in both packets drove its library
    // directly, so not one of them could tell the difference. The registration counts below are the
    // check that survives a refactor.
    const size_t audioRows = audio::ArmAudioResidue(g.battery);
    const size_t engineRows = residue::ArmPlaybackResidue(g.battery, g.log);

    // entity and pose may be null on a build where W1-E's spike refused; the applier checks, and the
    // transport still loads a file and drives a scrub bar, which is what W2-F needs.
    g.applier.Bind(s->entity, s->pose, s->netVar, &g.owners);

    if (!s->tick->RegisterEarly(&Tick, nullptr)) {
        g.log->Write(LogLevel::Error, kReplayModuleName,
                     "the tick callback table refused this module; playback will not run");
        return false;
    }

    // The HUD netvar writer rides the NETGAME.UPDATE detour's stage-3 tail
    // (InstallReplyDrainDriver / pawn.reply_route) -- a dependency, not a second hook. Registered
    // only when a flag is on, so the shipped default leaves the tail a null check.
    g.hud_clock_on = s->config->GetBool("playback.gamestate_clock", false);
    g.hud_score_on = s->config->GetBool("playback.gamestate_score", false);
    // False at the call site like its neighbours: a key that somehow is not registered must not
    // turn an engine-writing arm on.
    g.stats_on = s->config->GetBool("playback.stats", false);
    g.goals_on = s->config->GetBool("playback.goal_events", false);
    goalfeed::SetHornEnabled(s->config->GetBool("playback.goal_horn", false));
    // Default TRUE where its neighbours default false: this raise reaches one FX-only listener and
    // its failure mode is a no-op, so the owner should not have to find a key to see a goal.
    goalfeed::SetExplosionEnabled(s->config->GetBool("playback.goal_explosion", true));
    // Not one of the neighbours' engine-writing arms: this writes a log line and nothing else, so
    // an unregistered key should leave the evidence on rather than take it away.
    HudNetVarsSetScoreProbe(s->config->GetBool("playback.goal_score_probe", true));
    // Both default ON: a stun the viewer cannot see and a goal that does not explode are missing
    // playback, not optional extras. Either key still turns its feed off, which is what an A/B
    // needs.
    g.stuns_on = s->config->GetBool("playback.stun_events", true);
    g.component_events_on = s->config->GetBool("playback.component_events", true);
    entity::SetDelegateEmitEnabled(s->config->GetBool("playback.delegate_events", true));
    entity::SetDiscNodeHeldEnabled(s->config->GetBool("playback.disc_net_touch_held", false));
    {
        const int64_t ms = s->config->GetInt("audio.voicewatch_ms", 0);
        g.voicewatch_ms = ms > 0 ? (uint32_t)(ms < 60000 ? ms : 60000) : 0u;
        if (g.voicewatch_ms != 0) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "voicewatch: on, sampling Wwise's playing set every %u ms. Read-only: "
                         "nothing is posted, stopped or re-mixed",
                         g.voicewatch_ms);
        }
    }
    {
        // Clamped to a minute for the same reason voicewatch is: a cadence longer than that is a
        // key somebody meant in seconds, and a census that fires once an hour is off with extra
        // steps. A deep read later than ten minutes in is the same mistake.
        const int64_t ms = s->config->GetInt("audio.foley_census_ms", 30000);
        g.foley_census_ms = ms > 0 ? (uint32_t)(ms < 60000 ? ms : 60000) : 0u;
        const int64_t deep = s->config->GetInt("audio.foley_deep_at_s", 60);
        // The key is in seconds because that is how a person says "a minute into the match"; every
        // other clock in this block is milliseconds.
        static constexpr int64_t kMsPerSecond = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF milliseconds in a second, not a vtable slot
        g.foley_deep_at_ms =
            (uint32_t)((deep > 0 ? (deep < 600 ? deep : 600) : 0) * kMsPerSecond);
        if (g.foley_census_ms != 0) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "foley census: on, every %u ms, with the deep switch/state read %u s into "
                         "each match and at match end. READ-ONLY: AK::Query and log lines only, "
                         "which is why it also runs in a live spectate",
                         g.foley_census_ms, (unsigned)(g.foley_deep_at_ms / kMsPerSecond));
        }
    }
    {
        // Same clamp and the same reasoning as the two above. The floor matters more here: this
        // interval is the CEILING on a space walk, and a key of 1 would turn a triggered census
        // into a per-second one for no extra evidence.
        const int64_t ms = s->config->GetInt("disc.diff_census_ms", (int64_t)kDiscCensusDefaultMs);
        const int64_t floorMs = 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF one second, not a vtable slot
        g.disc_census_ms =
            ms > 0 ? (uint32_t)(ms < 60000 ? (ms > floorMs ? ms : floorMs) : 60000) : 0u;
        if (g.disc_census_ms != 0) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "disc diff census: on, at most every %u ms plus every holder change. "
                         "READ-ONLY: probed memory reads and log lines only, no engine call, which "
                         "is why it also runs in a live spectate",
                         g.disc_census_ms);
        }
    }
    const bool minimal = s->config->GetBool("bootstrap.minimal", false);
    if (minimal) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "bootstrap.minimal: no stage sweep, no host-netvar guard and no score gate are "
                     "armed. Diagnostic only -- the HUD clock, the score and the disc gate do not "
                     "run in this configuration");
    }
    if (!minimal && (g.hud_clock_on || g.hud_score_on || g.stats_on || g.goals_on || g.stuns_on ||
                     g.component_events_on)) {
        identity_access::RegisterGameStateStageSweep(&HudNetVarStageSweep, nullptr);
    }
    // HOST-NV. Here because Init runs on the bootstrap worker, where a MinHook arm belongs, and
    // before SC7's post-init pass. Whether it installs is the ledger key's answer alone, which
    // bootstrap resolved from containment and the two gamestate flags.
    if (!minimal) netvarguard::Install();
    g.host_netvar_filter_on = s->config->GetBool("playback.host_netvar_filter", false);
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "gamestate netvars: clock %s (playback.gamestate_clock), score %s "
                 "(playback.gamestate_score); HUD keys are written through the engine's typed "
                 "setters on the NETGAME.UPDATE stage-3 tail while a replay is loaded. Host netvar "
                 "guard %s, host writes to the driven keys are %s (playback.host_netvar_filter)",
                 g.hud_clock_on ? "on" : "off", g.hud_score_on ? "on" : "off",
                 netvarguard::Installed() ? "installed" : "NOT installed",
                 g.host_netvar_filter_on ? "SWALLOWED" : "counted only");

    // DISC-BIND. Same thread, same reason. Never in a process that is not contained; and a driven
    // disc the host can still score off is a bug, so with the gate wanted and not in place the
    // disc stays undriven.
    if (!minimal) scoregate::Install();
    g.disc_score_gate_on = s->config->GetBool("playback.disc_score_gate", true);
    g.grip_inputs_on = s->config->GetBool("playback.grip_inputs", true);
    // False at the call site like every other engine-writing arm: a key that somehow did not
    // register must not start writing the HOST's own netvars.
    g.host_hold_on = s->config->GetBool("playback.host_hold", false);
    g.host_hold_pause_on = s->config->GetBool("playback.host_hold_pause", false);
    g.host_clock_pin_on = s->config->GetBool("playback.host_clock_pin", false);
    g.disc_shell_on = s->config->GetBool("playback.disc_shell", false);
    g.hermite_on = s->config->GetBool("playback.hermite", true);
    {
        const char* mode = s->config->GetString("playback.motion_smoothing", "slowmo");
        if (!ParseSmoothMode(mode, &g.smooth_mode)) {
            // Unknown means OFF: the lossless path is the only safe reading of a word we do not know.
            g.smooth_mode = SmoothMode::Off;
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "smoothing: playback.motion_smoothing='%s' is not off|slowmo|on -- running OFF",
                         mode != nullptr ? mode : "(null)");
        }
        const int64_t strength = s->config->GetInt("playback.motion_smoothing_strength",
                                                   kSmoothStrengthDefault);
        g.smooth_strength = ParamsForStrength((uint8_t)(strength < 1 ? 1 : strength > 3 ? 3 : strength)).strength;
        if (strength != (int64_t)g.smooth_strength) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "smoothing: playback.motion_smoothing_strength=%lld is outside 1..3 -- using %u",
                         (long long)strength, unsigned(g.smooth_strength));
        }
        g.smooth_trace_on = s->config->GetBool("playback.motion_smoothing_trace", false);
        g.log->Write(LogLevel::Info, kReplayModuleName, "smoothing: %s, strength %s%s",
                     SmoothModeName(g.smooth_mode), SmoothStrengthName(g.smooth_strength),
                     g.smooth_trace_on ? ", per-frame trace ON" : "");
    }
    const bool disc_wanted = s->config->GetBool("playback.disc_drive", false);
    g.disc_drive_on = disc_wanted && ProcessContained() &&
                      (scoregate::Installed() || !g.disc_score_gate_on);
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "disc drive %s (playback.disc_drive %s, contained=%s); host score gate %s, a "
                 "score while a replay is in a match is %s (playback.disc_score_gate)",
                 g.disc_drive_on ? "ON" : "off", disc_wanted ? "on" : "off",
                 ProcessContained() ? "yes" : "no",
                 scoregate::Installed() ? "installed" : "NOT installed",
                 !scoregate::Installed() ? "the host's" : g.disc_score_gate_on ? "SWALLOWED"
                                                                              : "counted only");

    // ★ The registration itself. Four packets read this pointer, and until this line existed every
    // one of them saw null.
    s->transport = &g_transport;

    // Three MEASURED counts rather than the three table sizes, because Register() refuses silently
    // once the battery is full: a sum that stops matching the tables is the only way that shows.
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "registered: transport live, %u residue primitives (%u unmapped) = %u audio + %u "
                 "engine + %u unserved, tick=%s",
                 (unsigned)g.battery.Count(), (unsigned)g.battery.UnavailableCount(),
                 (unsigned)audioRows, (unsigned)engineRows, (unsigned)placeholders,
                 s->tick->Mode());
    return true;
}

void Shutdown() {
    if (g.log) {
        if (g.audio_residue_bound) {
            g.log->Write(LogLevel::Info, kReplayModuleName, "audio residue: bound at tick %llu",
                         (unsigned long long)g.audio_residue_bound_tick);
        } else {
            // Not an aside: every seek in this session left the Wwise memo unreconciled.
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "audio residue: unbound (CSoundPlayer not resolved)");
        }
    }
    // ★ NOTHING MAY BE FED TO THE ENGINE FROM HERE ON. Shutdown reaches Unload, and Unload is what
    // queues the leave feed; the engine is tearing itself down around us and an entrant teardown
    // issued into that is the crash-on-exit this plugin's whole detach design avoids. Two
    // independent reasons hold: the stage sweep that drains the queue is unregistered on the line
    // below, and Unload is told not to queue in the first place.
    g.exiting = true;
    identity_access::ClearGameStateStageSweep();
    g_transport.Unload();
    identity::ForgetRosterLeave();
    if (g.svc) {
        g.svc->transport = nullptr;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The public surface. Outside the anonymous namespace on purpose: the battery lives in a Global that
// nothing outside this file can name, which is why W2-F could render the COUNT and not the IDS.

ResidueStatus ResidueSnapshot() {
    ResidueStatus st;
    // ★★ `consulted` is the whole point. Before Init runs, the battery is empty -- and an
    // unavailable-count of zero from an empty battery reads as "everything is available", which is the
    // false all-clear the classifier exists to prevent. g.svc is non-null only after Init succeeded,
    // so it is the honest witness for "somebody actually asked".
    st.consulted = (g.svc != nullptr) && g.battery.Count() > 0;
    st.registered = g.battery.Count();
    st.unavailable = g.battery.UnavailableCount();
    return st;
}

size_t UnavailableResidueIds(const char** out, size_t cap) {
    if (g.svc == nullptr) {
        return 0;   // not consulted; ResidueSnapshot().consulted is what separates this from "none"
    }
    return g.battery.UnavailableIds(out, cap);
}

const char* ResidueDiagnostic(const char* id) {
    if (!id) {
        return nullptr;
    }
    for (size_t i = 0; i < g.battery.Count(); ++i) {
        IResidue* r = g.battery.At(i);
        if (r && strcmp(r->Id(), id) == 0) {
            return r->Diagnostic();
        }
    }
    return nullptr;
}

SeamStatus SeamSnapshot() {
    SeamStatus st{};
    st.clock_steps_pushed = g.clock_steps_pushed;
    st.owner_releases = g.owner_releases;
    st.history_trims = g.history_trims;
    st.audio_pins_pushed = g.audio_pins_pushed;
    st.audio_pins_absent = g.audio_pins_absent;
    st.audio_pins_refused = g.audio_pins_refused;
    st.lod_throttle_writes = g.lod_throttle_writes;
    st.lod_throttle_suppressed = g.lod_throttle_suppressed;
    return st;
}


BindingStatus BindingSnapshot() {
    BindingStatus st;
    st.entity = g.applier.Entity();
    st.pose = g.applier.Pose();
    st.netvar = g.applier.NetVar();
    st.entity_available = g.entity_available;
    st.rebinds = g.rebinds;
    st.entity_bind_state = g.entity_bind_state;
    st.entity_bind_epoch = g.entity_bind_epoch;
    st.owner_resets = g.owner_resets;
    st.entrants = (uint32_t)g.owners.EntrantCount();
    st.local_user_known = g.owners.LocalUserKnown();
    st.local_user_id = g.owners.LocalUserId();
    st.local_user_state = g.local_user_state;
    st.local_unknown_ticks = g.local_unknown_ticks;
    return st;
}

PresentSnapshot LastPresent() {
    return g.present;
}

ApplyStats ApplyStatsSnapshot() {
    return g.last_apply;
}

BuildStats BuildStatsSnapshot() {
    return g.build;
}

OracleTrackStatus OracleTrackSnapshot() {
    OracleTrackStatus s;
    s.asked = g.oracle_asked;
    s.active = g.oracle_active;
    s.begins = g.oracle_begins;
    s.ends = g.oracle_ends;
    s.poses = g.oracle_poses;
    s.refusal = g.oracle_refusal;
    return s;
}

void SetCameraControlForTest(IReplayCameraControl* cam) {
    g.camera_control_override = cam;
}

WindowDecodeStats WindowDecodeStatsForTest() {
    WindowDecodeStats s{};
    s.tickDecodes = g.tick_decodes;
    s.adoptions = g.adoptions;
    s.prefetchLate = g.prefetch_late;
    s.prefetchStale = g.prefetch_stale;
    s.prefetchFallbacks = g.prefetch_fallbacks;
    s.windowCentre = g.window_centre;
    for (const std::unique_ptr<Window>& slot : g.win_store) {
        if (slot) {
            ++s.windowsAllocated;
        }
    }
    s.current = g.cur;
    s.asyncOn = AsyncWindowsOn();
    return s;
}

void ServiceLeaveFeedForTest() {
    if (!identity::RosterLeavePending() || !g.svc || !g.svc->identity || !g.log) return;
    identity::ServiceRosterLeave(*g.svc->identity, *g.log, g.roster_map);
    const identity::LeaveReport& t = identity::RosterLeaveTotals();
    g.identity.released = t.left;
    g.identity.release_refused = t.refused;
}

void ForgetSessionEntrantsForTest() {
    g.roster_map.ClearResidue();
}

bool RequestPawnSlotForTest(uint16_t recorded) {
    return g.pawn.Request(UserSlot(recorded), JoinDesc{});
}

RecordedLoadoutProviderFn ArmedRecordedLoadoutProviderForTest() {
    return g.pawn.Env().recordedLoadout;
}

const void* ArmedActorHopsForTest() {
    return g.pawn.Env().hops;
}

ArmedBodyRoute ArmedBodyRouteForTest() {
    const auto& env = g.pawn.Env();
    return {env.equipmentRecv, env.sourceLoadoutBeforeAdmit, env.botBodySourceLoadout,
            env.participateBackstop};
}

const void* ProvisionedIdentityHopsForTest(IIdentity& inner) {
    ProvisionedIdentity wrapper(inner, g.pawn);
    return static_cast<const IIdentity&>(wrapper).ActorHops();
}

IdentityStatus IdentitySnapshot() {
    return g.identity;
}

// ★ THE ONE-SHOT THAT SEPARATES "THE PAWNS ARE UNPROVISIONED" FROM "THE BIND RESOLVES THE WRONG
// ACTOR". PRESENT-F measured 22 EchoUnit components in a session whose replayed pawns' actors
// carried none, and had no way to ask whose those 22 were. This walks the engine's own actor index
// for the three pools, then prints the actor GUID the plugin resolves for every user slot beside
// them, so the two sets can simply be compared. Typed by hand, never on a frame path.
uint32_t LogComponentCensus() {
    IEntity* const ent = g.svc && g.svc->entity && g.svc->entity->Available() ? g.svc->entity
                                                                             : nullptr;
    if (!ent || !g.log) return 0;
    // 22 EchoUnit + 52 punchable was the session PRESENT-F measured; 256 rows is room for an arena
    // several times that busy, and a census that hits the cap says so rather than lying by silence.
    static constexpr uint32_t kCap = 256;
    static IEntity::CensusRow rows[kCap];
    const uint32_t n = ent->ComponentCensus(rows, kCap);
    uint32_t lines = 1;
    g.log->Write(LogLevel::Warn, kReplayModuleName,
                 "census: %u actor-index row(s) across EchoUnit/ncaModel/CR15Body%s", n,
                 n == kCap ? " (CAPPED -- there are more)" : "");
    static const char* const kPoolName[] = {"echo", "model", "body"};
    for (uint32_t i = 0; i < n; ++i) {
        const IEntity::CensusRow& r = rows[i];
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census %s[%u]: actor %016llX%016llX comp %u pos %s(%.2f %.2f %.2f)",
                     kPoolName[(unsigned)r.pool], i, (unsigned long long)r.actorHi,
                     (unsigned long long)r.actorLo, (unsigned)r.component,
                     r.havePos ? "" : "UNREAD ", (double)r.pos[0], (double)r.pos[1],
                     (double)r.pos[2]);
        ++lines;
    }
    // The pooled-actor classes. `free` is what decides whether the NEXT admission gets an actor or
    // the invalid guid that kills the process (entity.h, ActorPoolRow), so this is printed before
    // and after an unload to say whether the engine took its actors back.
    static constexpr uint32_t kPoolCap = 64;
    static IEntity::ActorPoolRow pools[kPoolCap];
    const uint32_t np = ent->ActorPoolCensus(pools, kPoolCap);
    g.log->Write(LogLevel::Warn, kReplayModuleName, "census: %u actor pool(s)%s", np,
                 np == 0 ? " -- the game space published none, so the pool guard cannot arm" : "");
    ++lines;
    for (uint32_t i = 0; i < np; ++i) {
        const IEntity::ActorPoolRow& p = pools[i];
        const char* named = replay::gametype::NameOfActorPrefab(p.prefabId);
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census pool class %u: prefab %016llX (%s) free %u of %u", (unsigned)p.classId,
                     (unsigned long long)p.prefabId, named ? named : "?", p.freeSlots, p.capacity);
        ++lines;
    }

    // The ncaActorAlias rows for the pool the replayed entrants allocate from, taken
    // at whatever moment the census is asked for.  The admission and the leave take the same reading
    // at their own two moments; this is the one that can be asked at a playhead, which is what says
    // WHEN a recycled entry's record stops being resolvable rather than only that it has.
    {
        void* const netGame = g.tick_net_game ? g.tick_net_game : LiveNetGame();
        void* space = nullptr;
        if (netGame != nullptr) {
            const uint8_t* const at =
                static_cast<const uint8_t*>(netGame) + facts::NETGAME_CS_SPACE_OFF;
            if (Readable(at, sizeof space)) memcpy(&space, at, sizeof space);
        }
        for (uint32_t i = 0; i < np; ++i) {
            if (pools[i].prefabId != replay::gametype::kRemotePlayerArenaPrefab) continue;
            char alias[2048];
            replay::actoralias::Describe(space, static_cast<uint16_t>(pools[i].classId), alias,
                                         sizeof alias);
            g.log->Write(LogLevel::Warn, kReplayModuleName, "census alias: %s", alias);
            ++lines;
        }
    }

    // P0 reading 6, first because it is the cheapest and the owner's most visible symptom, and
    // because sampling it across repeated censuses is what says whether the timer ever resets.
    IEntity::DiscRing ring;
    if (!ent->ReadDiscRing(ring)) {
        g.log->Write(LogLevel::Warn, kReplayModuleName, "census disc: refused -- %s",
                     ring.refused ? ring.refused : "unknown");
    } else if (!ring.touchRead) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census disc: timesincelastheld %.3f s (enabled %u), touch handle %u, "
                     "touch CI UNREAD",
                     (double)ring.timeSinceLastHeld, (unsigned)ring.enabled,
                     (unsigned)ring.touchHandle);
    } else {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census disc: timesincelastheld %.3f s (enabled %u), touch handle %u, "
                     "holders %08X/%08X, flags %04X ask-parent %d => the reset gate reads %s",
                     (double)ring.timeSinceLastHeld, (unsigned)ring.enabled,
                     (unsigned)ring.touchHandle,
                     (unsigned)ring.holderA, (unsigned)ring.holderB, (unsigned)ring.flags,
                     (int)ring.askParent, ring.heldByOwnCi ? "HELD" : "not held");
    }
    ++lines;

    // P0 reading 3. Which component systems carry rows for the replayed players' actor class, and
    // for the classes that DO carry an EchoUnit -- the comparison that separates "the prefab has no
    // script graph" from "the delegate lands somewhere else". The four classes are taken from the
    // pool census above so nothing here hardcodes a class id.
    uint16_t classes[IEntity::SystemRow::kClasses];
    uint32_t nClasses = 0;
    for (uint32_t i = 0; i < np && nClasses < IEntity::SystemRow::kClasses; ++i) {
        if (pools[i].prefabId == replay::gametype::kRemotePlayerArenaPrefab)
            classes[nClasses++] = pools[i].classId;
    }
    for (uint32_t i = 0; i < np && nClasses < IEntity::SystemRow::kClasses; ++i) {
        if (pools[i].prefabId != replay::gametype::kRemotePlayerArenaPrefab)
            classes[nClasses++] = pools[i].classId;
    }
    if (nClasses > 0) {
        static constexpr uint32_t kSysCap = 512;
        static IEntity::SystemRow systems[kSysCap];
        const uint32_t ns = ent->SystemCensus(classes, nClasses, systems, kSysCap);
        char header[128] = {};
        int at = snprintf(header, sizeof header, "census systems: %u walked%s, classes", ns,
                          ns == kSysCap ? " (CAPPED -- there are more)" : "");
        for (uint32_t k = 0; k < nClasses && at > 0 && at < (int)sizeof header; ++k)
            at += snprintf(header + at, sizeof header - (size_t)at, " %u", (unsigned)classes[k]);
        g.log->Write(LogLevel::Warn, kReplayModuleName, "%s", header);
        ++lines;
        for (uint32_t i = 0; i < ns; ++i) {
            const IEntity::SystemRow& s = systems[i];
            uint32_t hits = 0;
            for (uint32_t k = 0; k < nClasses; ++k) hits += s.perClass[k];
            if (hits == 0) continue;
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "census system %016llX: %u row(s); per class %u %u %u %u",
                         (unsigned long long)s.symbol, s.rows, s.perClass[0], s.perClass[1],
                         s.perClass[2], s.perClass[3]);
            ++lines;
        }
    }

    // Restated at the end of the census, where a log burst cannot drop it.
    uint32_t discScriptInstances = 0, discScriptSelf = 0;
    bool discScriptCapped = false, discScriptRead = false;

    // P1C M1 and D5. `self` on every graph of the three scripts the owner's three complaints run
    // through, beside a roll-call of every script the level instantiated. The three gates are the
    // same gate, so one walk answers all of them: a delegate whose event actor is not the graph's
    // own `self` is dropped before its first node runs, silently.
    {
        static constexpr uint64_t kWanted[] = {
            0xF39961BE341AABB7ull,  // remote hands + wrist thrusters, PlaySound only
            0xACD35BDC14481483ull,  // boost / brake / backpack booster
            0x38965D90A823F03Full,  // the disc, whose `bubble` actor is the "untouched" ring
        };
        static constexpr uint32_t kScriptRowCap = 256;
        static constexpr uint32_t kScriptHashCap = 192;
        static IEntity::ScriptSelfRow scriptRows[kScriptRowCap];
        static IEntity::ScriptHashRow scriptHashes[kScriptHashCap];
        IEntity::ScriptCensus sc;
        sc.wanted = kWanted;
        sc.nWanted = (uint32_t)(sizeof kWanted / sizeof kWanted[0]);
        sc.rows = scriptRows;
        sc.rowCap = kScriptRowCap;
        sc.hashes = scriptHashes;
        sc.hashCap = kScriptHashCap;
        if (!ent->ScriptSelfCensus(sc)) {
            g.log->Write(LogLevel::Warn, kReplayModuleName, "census script: refused -- %s",
                         sc.refused ? sc.refused : "unknown");
        } else {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "census script: %u ncaScript instance(s) walked, %u unreadable, %u "
                         "distinct script(s)%s, %u detail row(s)%s",
                         sc.walked, sc.unreadable, sc.hashesWritten,
                         sc.hashesWritten == kScriptHashCap ? " (CAPPED)" : "", sc.rowsWritten,
                         sc.rowsWritten == kScriptRowCap ? " (CAPPED)" : "");
            discScriptRead = true;
            discScriptCapped = sc.hashesWritten == kScriptHashCap;
            for (uint32_t i = 0; i < sc.hashesWritten; ++i) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census script hash %016llX: %u instance(s), %u answered `self`",
                             (unsigned long long)sc.hashes[i].hash, sc.hashes[i].instances,
                             sc.hashes[i].withSelf);
                if (sc.hashes[i].hash == 0x38965D90A823F03Full) {
                    discScriptInstances = sc.hashes[i].instances;
                    discScriptSelf = sc.hashes[i].withSelf;
                }
            }
            for (uint32_t i = 0; i < sc.rowsWritten; ++i) {
                const IEntity::ScriptSelfRow& s = sc.rows[i];
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census script[%u] %016llX on actor %016llX%016llX ci %u state %d: "
                             "self %s space %016llX actor %016llX node %016llX",
                             i, (unsigned long long)s.scriptHash, (unsigned long long)s.actorHi,
                             (unsigned long long)s.actorLo, (unsigned)s.ciIndex, (int)s.state,
                             s.answered ? "READ" : "UNANSWERED", (unsigned long long)s.selfSpace,
                             (unsigned long long)s.selfActor, (unsigned long long)s.selfNode);
                // The sentinel block, raw. Any word above zero is a delegate whose previous thread
                // is still alive, and every further edge of THAT delegate is dropped in silence.
                char words[8 * IEntity::ScriptSelfRow::kDataWords + 1] = {};
                int at = 0;
                for (uint32_t w = 0; w < IEntity::ScriptSelfRow::kDataWords &&
                                     at >= 0 && at < (int)sizeof words - 1;
                     ++w) {
                    at += snprintf(words + at, sizeof words - (size_t)at, "%08X",
                                   s.dataRead ? s.data[w] : 0u);
                }
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census script[%u] data %s gametime %lld: %s", i,
                             s.dataRead ? "read" : "UNREAD", (long long)s.graphTime, words);
            }
        }
        lines += 1 + sc.hashesWritten + 2 * sc.rowsWritten;
    }

    // ★ THE RING. The engine's own witness for the disc's grab edge: CR15NetTouchInteractCS::Update
    // raises `delegate_OnNetTouchGrab` only when the holder it reads disagrees with the latch bit
    // it keeps, and only while the netgame state word reads 9. Latch clear with a holder published
    // means the update is not seeing our owner; latch set means the edge WAS raised and the ring is
    // a downstream problem. `last owner` is rewritten every pass, so it also says the loop ran.
    {
        IEntity::NetTouchReading nt;
        if (!ent->NetTouchCensus(nt)) {
            // With the control found, a miss is the level's answer and not the walk's: the same
            // pass over the same table saw a symbol the plugin binds a system by.
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "census nettouch: refused -- %s (%u system(s) walked, R14NetPhysics %s "
                         "in the same pass)", nt.refused ? nt.refused : "unknown", nt.systemsSeen,
                         nt.controlFound ? "FOUND" : "missed too -- the walk is the suspect");
            ++lines;
        } else {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "census nettouch: cs %016llX, %u component(s), %u row(s); netgame state %s%u"
                         " (9 = in match, the only state the grab edge is raised in), remote band %u..%u",
                         (unsigned long long)nt.cs, nt.ciCount, nt.rowsWritten,
                         nt.netGameRead ? "" : "UNREAD ", nt.netGameState, nt.bandLo, nt.bandHi);
            ++lines;
            for (uint32_t i = 0; i < nt.rowsWritten; ++i) {
                const IEntity::NetTouchRow& r = nt.rows[i];
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census nettouch[%u]: entity %u phys %u touch %u; last owner %08X, "
                             "flags %02X, held latch %u; body %s owner %08X flags %04X",
                             i, (unsigned)r.entity, (unsigned)(r.physId & 0xFFFFu),
                             (unsigned)(r.touchId & 0xFFFFu), (unsigned)r.lastOwner,
                             (unsigned)r.flags, r.heldLatch ? 1u : 0u,
                             r.physRead ? "read" : "UNREAD", (unsigned)r.physOwner,
                             (unsigned)r.physFlags);
                ++lines;
            }
        }
    }

    // ★ ITEM C. The listener set Wwise attenuates against, beside the pose the engine is about to
    // render from. One line per listener, because "how many" is half the finding and "where" is the
    // other half: a second listener parked mid-arena keeps every emitter audible from any camera.
    {
        IEntity::AudioMix mix;
        if (!ent->AudioMixCensus(mix)) {
            g.log->Write(LogLevel::Warn, kReplayModuleName, "census audio: refused -- %s",
                         mix.refused ? mix.refused : "unknown");
            ++lines;
        } else {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "census audio: sound player %016llX, %llu listener(s)%s, spatialized id "
                         "%u, %llu view(s), active view %u",
                         (unsigned long long)mix.system, (unsigned long long)mix.listenerCount,
                         mix.listenersWritten < mix.listenerCount ? " (CAPPED)" : "",
                         (unsigned)mix.spatializedId, (unsigned long long)mix.viewCount,
                         (unsigned)mix.activeViewId);
            for (uint32_t i = 0; i < mix.listenersWritten; ++i) {
                const IEntity::AudioListener& l = mix.listeners[i];
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census audio listener[%u]: id %u pos (%.2f %.2f %.2f) quat "
                             "(%.3f %.3f %.3f %.3f)",
                             i, (unsigned)l.id, l.pos[0], l.pos[1], l.pos[2], l.quat[0], l.quat[1],
                             l.quat[2], l.quat[3]);
            }
            for (uint32_t i = 0; i < mix.viewsWritten; ++i) {
                const IEntity::AudioView& v = mix.views[i];
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census audio view[%u]: id %u pos (%.2f %.2f %.2f)%s", i,
                             (unsigned)v.id, v.pos[0], v.pos[1], v.pos[2],
                             v.id == mix.activeViewId ? " ACTIVE" : "");
            }
            // ★ ITEM 2. Where Wwise believes every postable game object is, with its distance to the
            // listener the line above printed -- the whole of "is this sound being attenuated over a
            // distance at all", read out of the engine's own cache of what it last sent SetPosition.
            const bool haveL = mix.listenersWritten != 0;
            const float lx = haveL ? mix.listeners[0].pos[0] : 0.0f;
            const float ly = haveL ? mix.listeners[0].pos[1] : 0.0f;
            const float lz = haveL ? mix.listeners[0].pos[2] : 0.0f;
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "census audio emitters: %llu registered%s, distances are to listener[0]",
                         (unsigned long long)mix.emitterCount,
                         mix.emittersWritten < mix.emitterCount ? " (CAPPED)" : "");
            for (uint32_t i = 0; i < mix.emittersWritten; ++i) {
                const IEntity::AudioEmitter& e = mix.emitters[i];
                const float dx = e.pos[0] - lx, dy = e.pos[1] - ly, dz = e.pos[2] - lz;
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census audio emitter[%u]: id %u flags %08X%s pos (%.2f %.2f %.2f) "
                             "d=%.1f", i, (unsigned)e.id, (unsigned)e.flags,
                             (e.flags & 0x20u) ? " POSTABLE" : "", e.pos[0], e.pos[1], e.pos[2],
                             haveL ? sqrtf(dx * dx + dy * dy + dz * dz) : -1.0f);
                // FAR-SOUND-2. Only the emitters carrying a voice are read back out of Wwise, so
                // only those get this second line. The three AkTransform vectors are printed
                // whole: the one that tracks the pawn is the position, and if NONE of them does,
                // the game's cache above is a claim Wwise never received.
                if (!e.wwiseRead && !e.dryRead && !e.auxRead) continue;
                char aux[128];
                int n = 0;
                for (uint32_t s = 0; s < 4 && s < e.auxCount; ++s) {
                    n += snprintf(aux + n, sizeof aux - (size_t)n, " bus %u=%.3f",
                                  (unsigned)e.auxBus[s], (double)e.auxValue[s]);
                }
                if (n == 0) snprintf(aux, sizeof aux, " none");
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census audio emitter[%u] wwise: %s front (%.2f %.2f %.2f) top "
                             "(%.2f %.2f %.2f) pos (%.2f %.2f %.2f) | dry %s%.3f | %u aux send(s)%s",
                             i, e.wwiseRead ? "read" : "UNREAD", e.wwise[0], e.wwise[1], e.wwise[2],
                             e.wwise[3], e.wwise[4], e.wwise[5], e.wwise[6], e.wwise[7], e.wwise[8],
                             e.dryRead ? "" : "UNREAD ", (double)e.dryLevel,
                             (unsigned)e.auxCount, e.auxRead ? aux : " UNREAD");
                ++lines;
            }
            // ★ The Wwise side. The game keeps no copy of the listener set an emitter answers to,
            // so a listener that was in the table when SetDefaultListeners was last called and has
            // not moved since is invisible from the game side and audible from every camera.
            if (mix.wwiseRefused != nullptr) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census audio wwise: refused -- %s", mix.wwiseRefused);
                ++lines;
            } else {
                for (uint32_t i = 0; i < mix.setsWritten; ++i) {
                    const IEntity::AudioListenerSet& s = mix.sets[i];
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census audio listener set[%u]: %u emitter(s) answer to %u "
                                 "listener(s) %llu %llu %llu %llu", i, (unsigned)s.emitters,
                                 (unsigned)s.size, (unsigned long long)s.ids[0],
                                 (unsigned long long)s.ids[1], (unsigned long long)s.ids[2],
                                 (unsigned long long)s.ids[3]);
                }
                for (uint32_t i = 0; i < mix.wwiseListenersWritten; ++i) {
                    const IEntity::AudioWwiseListener& l = mix.wwiseListeners[i];
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census audio wwise listener[%u]: id %llu %s (%.2f %.2f %.2f)", i,
                                 (unsigned long long)l.id, l.read ? "pos" : "UNREADABLE",
                                 l.pos[0], l.pos[1], l.pos[2]);
                }
                lines += mix.setsWritten + mix.wwiseListenersWritten;
            }
            // FINAL-FIX-1 item 1(c). What is PLAYING right now and how far its curve reaches.
            // `dry`/`auxGame` are the curve's last point in dB: a dry end at silence with a
            // game-defined reverb end near 0 is a sound that survives distance as reverb only.
            if (mix.playingRefused != nullptr) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census audio playing: refused -- %s", mix.playingRefused);
                ++lines;
            } else {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census audio playing: %u voice(s) on %u of %u emitter(s)%s",
                             mix.playingSeen, mix.emittersActive, mix.emittersWritten,
                             mix.playingSeen > mix.playingWritten ? " -- TABLE FULL" : "");
                for (uint32_t i = 0; i < mix.playingWritten; ++i) {
                    const IEntity::AudioPlayingSound& s = mix.playing[i];
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census audio playing[%u]: emitter %u pos (%.2f %.2f %.2f) "
                                 "playing %u event %u obj %u of %u radius %.1f | %s maxDist %.1f "
                                 "dry %.1f dB auxGame %.1f dB auxUser %.1f dB%s",
                                 i, s.emitterId, s.emitterPos[0], s.emitterPos[1], s.emitterPos[2],
                                 s.playingId, s.eventId, s.objId, s.objCount, s.maxRadius,
                                 !s.info ? "NO POSITIONING INFO"
                                         : (s.attenuation ? "attenuated" : "NOT ATTENUATED"),
                                 s.maxDistance, s.volDryAtMax, s.volAuxGameDefAtMax,
                                 s.volAuxUserDefAtMax, s.info ? "" : " (query refused)");
                    // ★ AUDIO-FIX-2 C.2. The WHOLE sub-tree behind that one pick. The line above
                    // names one node because one number has to be printed somewhere; these name
                    // every node the event references, so a container whose children carry
                    // different curves is visible as the several curves it is, and "the floor is
                    // -19 dB" can be checked against the child that carries it rather than
                    // against whichever child came back first.
                    for (uint32_t k = 0; k < s.nodeCount; ++k) {
                        const IEntity::AudioObjNode& n = mix.objNodes[s.nodeFirst + k];
                        g.log->Write(LogLevel::Warn, kReplayModuleName,
                                     "census audio playing[%u] node[%u]: obj %u parent %u depth %d "
                                     "| %s maxDist %.1f dry %.1f dB auxGame %.1f dB auxUser %.1f dB",
                                     i, k, n.objId, n.parentId, (int)n.depth,
                                     !n.info ? "NO POSITIONING INFO"
                                             : (n.attenuation ? "attenuated" : "NOT ATTENUATED"),
                                     n.maxDistance, n.volDryAtMax, n.volAuxGameDefAtMax,
                                     n.volAuxUserDefAtMax);
                        ++lines;
                    }
                }
                lines += 1 + mix.playingWritten;
                // LAST in the census, because the sink keeps the newest lines of a burst and this
                // is the block the packet turns on. One row per distinct event, at most four.
                uint32_t shownEvents[4] = {};
                uint32_t shown = 0;
                for (uint32_t i = 0; i < mix.playingWritten && shown < 4; ++i) {
                    const IEntity::AudioPlayingSound& s = mix.playing[i];
                    if (!s.info) continue;
                    bool seen = false;
                    for (uint32_t k = 0; k < shown; ++k) seen = seen || shownEvents[k] == s.eventId;
                    if (seen) continue;
                    shownEvents[shown++] = s.eventId;
                    char floats[512];
                    int n = 0;
                    for (uint32_t o = 0; o + 4 <= sizeof s.raw; o += 4) {
                        float f = 0.0f;
                        uint32_t u = 0;
                        memcpy(&f, s.raw + o, sizeof f);
                        memcpy(&u, s.raw + o, sizeof u);
                        const bool sane = f == f && f > -1.0e6f && f < 1.0e6f;
                        n += snprintf(floats + n, sizeof floats - (size_t)n, " +%u=%s",
                                      (unsigned)o, sane ? "" : "");
                        n += sane ? snprintf(floats + n, sizeof floats - (size_t)n, "%.3f",
                                             (double)f)
                                  : snprintf(floats + n, sizeof floats - (size_t)n, "0x%08X",
                                             (unsigned)u);
                    }
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census audio posinfo event %u obj %u radius %.1f raw:%s",
                                 s.eventId, s.objId, (double)s.maxRadius, floats);
                    ++lines;
                }
            }
            lines += 2 + mix.listenersWritten + mix.viewsWritten + mix.emittersWritten;
        }
        // The camera pose to compare these against is NOT read here: `cam` reads it back through
        // camera_control::SampleViewPose, which this module does not link, and a second reader
        // would be a second answer to one question. The operator asks `cam` in the same window.
    }

    // Every user slot the net game can hold, not just the mapped ones: a pawn whose components sit
    // on a NEIGHBOURING slot's actor is exactly what H-A predicts, and only the full table shows it.
    for (uint16_t slot = 0; slot < facts::ENTITY_USER_SLOT_TABLE_MAX; ++slot) {
        IEntity::EchoBind b;
        ent->ProbeEchoUnitBind(UserIndex(slot), b);
        if (b.actorLo == 0 && b.actorHi == 0 && !b.haveKeys) continue;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census user[%u]: user-actor %016llX%016llX keys %016llX%016llX (%s) echo %u "
                     "model %u body %u",
                     (unsigned)slot, (unsigned long long)b.actorHi, (unsigned long long)b.actorLo,
                     (unsigned long long)b.keysHi, (unsigned long long)b.keysLo,
                     b.haveKeys ? "present" : "absent", (unsigned)b.echoHandle,
                     (unsigned)b.modelHandle, (unsigned)b.bodyHandle);
        ++lines;
    }
    for (size_t i = 0; i < Win().PlayerCount(); ++i) {
        const PlayerTrack& tr = Win().PlayerAt(i);
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census track[%u]: rec slot %u -> user %u (%s) remote-player handle %u",
                     (unsigned)i, (unsigned)tr.user_slot, (unsigned)tr.user_id,
                     tr.user_id_valid ? "mapped" : "UNMAPPED", (unsigned)tr.handle.v);
        ++lines;

        // P0 readings 4 and 5. MATCH is the whole question: the actor the engine resolves for the
        // delegate handle, against the actor the net game has bound to this user's record. The
        // partition and the history head ride along because they are two more reads on the same CD.
        IEntity::RemoteTrackProbe p;
        ent->RemoteTrackProbe_(UserIndex(tr.user_id), p);
        IEntity::EchoBind b;
        ent->ProbeEchoUnitBind(UserIndex(tr.user_id), b);
        // The resolve answers {actor id, component symbol}, not an SActorGUID: the second qword
        // came back as the CSymbol64 of R14RemotePlayer for every track, where the user record's
        // second qword is the gamespace name. Only the actor id is comparable, and it is the whole
        // question -- it is what sub_14050FED0 dispatches the delegate on.
        const bool match = p.targetRead && p.targetLo == b.actorHi;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census delegate[%u]: handle 0x%08X cd %u of %u enabled (%u total), history "
                     "head %s, target actor %016llX sym %016llX vs user-actor %016llX space "
                     "%016llX => %s%s%s",
                     (unsigned)i, p.compact, (unsigned)p.cdIndex, (unsigned)p.enabledCds,
                     (unsigned)p.totalCds,
                     !p.historyRead ? "UNREAD" : (p.historyEmpty ? "NULL" : "NON-NULL -- INVARIANT "
                                                                           "BROKEN"),
                     (unsigned long long)p.targetLo, (unsigned long long)p.targetHi,
                     (unsigned long long)b.actorHi, (unsigned long long)b.actorLo,
                     !p.targetRead ? "UNRESOLVED" : (match ? "MATCH" : "MISMATCH"),
                     p.refused ? " -- " : "", p.refused ? p.refused : "");
        ++lines;

        // P1C M0. The two hand fields beside the sentinel they are compared against: the geo-attach
        // delegates are raised only while the hand equals it both before and after the diff, so a
        // hand that never reads as the sentinel means those two delegates were never raised at all
        // and the plugin's own count of bit-5/6 transitions was never evidence that they were.
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census action[%u]: bits28 %08X holdL %08X holdR %08X (%s) vs sentinel %08X "
                     "(%s) => L %s, R %s",
                     (unsigned)i, p.bits28, p.holdL, p.holdR, p.actionRead ? "read" : "UNREAD",
                     p.holdSentinel, p.sentinelRead ? "read" : "UNREAD",
                     p.holdL == p.holdSentinel ? "free" : "HOLDING",
                     p.holdR == p.holdSentinel ? "free" : "HOLDING");
        ++lines;
    }

    // ★ DISC-RING-1's two answers, restated LAST. One census writes several hundred lines inside a
    // single tick and the log sink keeps the newest of a burst, so a block written in the middle of
    // it can be gone by the time the file is read -- which is exactly what happened to ENG-P1C's
    // `census script:` summary. These two lines are the ones the ring question turns on, so they
    // are written where a drop cannot reach them.
    {
        IEntity::NetTouchReading nt;
        const bool ntRead = ent->NetTouchCensus(nt);
        uint32_t held = 0;
        for (uint32_t i = 0; i < nt.rowsWritten; ++i) held += nt.rows[i].heldLatch ? 1u : 0u;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census ring: net-touch %s -- %u system(s) walked, R14NetPhysics %s; %u "
                     "component(s), %u with the update's held latch set; netgame state %s%u",
                     ntRead ? "PRESENT" : (nt.refused ? nt.refused : "refused"), nt.systemsSeen,
                     nt.controlFound ? "found" : "MISSED", nt.rowsWritten, held,
                     nt.netGameRead ? "" : "UNREAD ", nt.netGameState);
        ++lines;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "census ring: disc script 38965D90A823F03F -- %s%u instance(s), %u answered "
                     "`self`%s", discScriptRead ? "" : "walk refused, ", discScriptInstances,
                     discScriptSelf, discScriptCapped ? " (the hash roll-call was CAPPED, so a "
                                                        "zero here is not yet an absence)" : "");
        ++lines;
    }

    // ★ DISC-RING-2. The lines above walk the space the PAWNS live in. These walk the space the
    // DISC was bound in, which need not be the same one, and they name every actor the frisbee
    // system holds a component for -- the whole of RE-DISC-RING-2 §5's discrimination table.
    {
        IEntity::DiscRingReading dr;
        if (!ent->DiscRingCensus(dr)) {
            g.log->Write(LogLevel::Warn, kReplayModuleName, "census discactor: refused -- %s",
                         dr.refused ? dr.refused : "unknown");
            ++lines;
        } else {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "census discactor: disc space %016llX %s pawn space %016llX; %u live "
                         "frisbee component(s) in the bound system; bind slot %u",
                         (unsigned long long)dr.discSpace,
                         dr.spacesDiffer ? "DIFFERS from" : "is the same as",
                         (unsigned long long)dr.pawnSpace, (unsigned)dr.frisbeeLive,
                         (unsigned)dr.bindActorIndex);
            ++lines;
            for (uint32_t i = 0; i < dr.scansWritten; ++i) {
                const IEntity::DiscRingReading::SpaceScan& s = dr.scans[i];
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census discactor space[%u] %016llX: %s, %u system(s), R14NetPhysics "
                             "%s, R14Frisbee %016llX%s, %u ncaScript component(s) walked, %u of "
                             "them the disc script; registry game_frisbee slot %u (%s), "
                             "practice_disc slot %u (%s)",
                             i, (unsigned long long)s.space, s.walked ? "walked" : "NOT WALKABLE",
                             s.systemsSeen, s.controlFound ? "found" : "MISSED",
                             (unsigned long long)s.frisbeeCs, s.isBoundCs ? " (THE BOUND ONE)" : "",
                             s.scriptsWalked, s.discScriptInstances, (unsigned)s.idxGameFrisbee,
                             s.liveGameFrisbee ? "live" : "not live", (unsigned)s.idxPracticeDisc,
                             s.livePracticeDisc ? "live" : "not live");
                ++lines;
            }
            for (uint32_t i = 0; i < dr.actorsWritten; ++i) {
                const IEntity::DiscRingReading::Actor& a = dr.actors[i];
                // The disc's own body, through the pool the track drives, so a second frisbee actor
                // parked elsewhere is visible as a position and not only as a row.
                BodyState st;
                memset(&st, 0, sizeof st);
                BodyReading rd;
                // 0 is component index 0, which is where the BOUND disc's body actually sits; the
                // "no component" value is all-ones. Rejecting 0 printed UNREAD for the one row that
                // could be read (ENG-DISC-RING-2 §5's loose end).
                const bool posRead =
                    a.physId != 0xFFFFu &&  // facts-ok: NETVAR.SLOT_NONE the census's own "this actor has no phys component" value, not the engine's netvar slot field
                    ent->ReadBodyDetailed(PhysHandle{(uint16_t)a.physId, PhysHandle::kPoolDisc}, st,
                                          rd);
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census discactor[%u]%s: actor %016llX (row sym %016llX) frisbeeCi %u "
                             "phys %u untouched %.1fs; %u system(s); R14Frisbee=%c "
                             "R14TouchInteract=%c ncaPhysics=%c ncaScript=%u R14NetPhysics=%c "
                             "R14NetTouchInteract=%c R15NetFrisbeeTrail=%c; scripts %016llX %016llX "
                             "%016llX %016llX; pos %s (%.2f %.2f %.2f)",
                             i, a.bound ? " BOUND" : "", (unsigned long long)a.actorId,
                             (unsigned long long)a.rowSymbol, (unsigned)a.frisbeeCi,
                             (unsigned)(a.physId & 0xFFFFu), a.timeSinceHeld, a.systems,
                             (a.has & IEntity::DiscRingReading::kFrisbee) ? 'y' : 'n',
                             (a.has & IEntity::DiscRingReading::kTouch) ? 'y' : 'n',
                             (a.has & IEntity::DiscRingReading::kNcaPhysics) ? 'y' : 'n', a.scripts,
                             (a.has & IEntity::DiscRingReading::kNetPhysics) ? 'y' : 'n',
                             (a.has & IEntity::DiscRingReading::kNetTouch) ? 'y' : 'n',
                             (a.has & IEntity::DiscRingReading::kTrail) ? 'y' : 'n',
                             (unsigned long long)a.scriptHash[0],
                             (unsigned long long)a.scriptHash[1],
                             (unsigned long long)a.scriptHash[2],
                             (unsigned long long)a.scriptHash[3],
                             posRead ? "read"
                                     : (a.physId == 0xFFFFu ? "NO PHYS COMPONENT"  // facts-ok: NETVAR.SLOT_NONE the census's own "no phys component" value, not the engine's netvar slot field
                                                            : BodyRefusalName(rd.refusedAt)),
                             posRead ? st.com[0] : 0.f, posRead ? st.com[1] : 0.f,
                             posRead ? st.com[2] : 0.f);
                ++lines;
            }
            for (uint32_t i = 0; i < dr.sitesWritten; ++i) {
                const IEntity::DiscRingReading::ScriptSite& s = dr.sites[i];
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census discactor script[%u]: 38965D90A823F03F on actor %016llX in "
                             "space %016llX, ci %u state %d", i, (unsigned long long)s.actorId,
                             (unsigned long long)s.space, (unsigned)s.ciIndex, (int)s.state);
                ++lines;
            }
            // Last, so a burst cannot drop it: the one sentence the verdict turns on.
            uint32_t instances = 0, walkedScripts = 0;
            for (uint32_t i = 0; i < dr.scansWritten; ++i) {
                instances += dr.scans[i].discScriptInstances;
                walkedScripts += dr.scans[i].scriptsWalked;
            }
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "census discactor: %u actor(s) carry the bound R14Frisbee system; disc "
                         "script 38965D90A823F03F has %u instance(s) across %u space(s) (%u "
                         "ncaScript component(s) walked)", dr.actorsWritten, instances,
                         dr.scansWritten, walkedScripts);
            ++lines;

            // ★ RING-1's two measurements, re-taken in the space the disc actually lives in. That
            // is the whole correction: both were taken in the pawns' space, where neither the
            // net-touch system nor the disc's graph has any reason to exist.
            void* const discSpace = reinterpret_cast<void*>((uintptr_t)dr.discSpace);
            if (discSpace && dr.spacesDiffer) {
                IEntity::NetTouchReading nt;
                nt.space = discSpace;
                const bool ok = ent->NetTouchCensus(nt);
                uint32_t held = 0;
                for (uint32_t i = 0; i < nt.rowsWritten; ++i) held += nt.rows[i].heldLatch ? 1u : 0u;
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "census ring2: net-touch in the DISC's space %s -- %u system(s) "
                             "walked, R14NetPhysics %s; %u component(s), %u with the update's held "
                             "latch set; netgame state %s%u",
                             ok ? "PRESENT" : (nt.refused ? nt.refused : "refused"), nt.systemsSeen,
                             nt.controlFound ? "found" : "MISSED", nt.rowsWritten, held,
                             nt.netGameRead ? "" : "UNREAD ", nt.netGameState);
                ++lines;
                for (uint32_t i = 0; i < nt.rowsWritten; ++i) {
                    const IEntity::NetTouchRow& r = nt.rows[i];
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census ring2 nettouch[%u]: entity %u phys %u touch %u; last owner "
                                 "%08X, flags %02X, held latch %u; body %s owner %08X flags %04X",
                                 i, (unsigned)r.entity, (unsigned)(r.physId & 0xFFFFu),
                                 (unsigned)(r.touchId & 0xFFFFu), (unsigned)r.lastOwner,
                                 (unsigned)r.flags, r.heldLatch ? 1u : 0u,
                                 r.physRead ? "read" : "UNREAD", (unsigned)r.physOwner,
                                 (unsigned)r.physFlags);
                    ++lines;
                    // Only the row that carries a body can answer the gate question, and printing
                    // the ten empty ones would push this one out of the sink's window.
                    if (!r.physRead) continue;
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census ring3 gate[%u]: physCS %016llX ci %s (owner %08X), node %s "
                                 "%04X held-bit %u; touchCS %016llX %s index %u of %u %s, holders "
                                 "%08X/%08X hand %08X flags %04X",
                                 i, (unsigned long long)nt.csPhys,
                                 r.engSameCi ? "SAME as the pool's" : "DIFFERS from the pool's",
                                 (unsigned)r.engOwner, r.nodeRead ? "read" : "UNREAD",
                                 (unsigned)r.nodeState, (r.nodeState & 0x200u) ? 1u : 0u,  // facts-ok: PHYS.NODE_HELD_BIT the remote arm's last term, printed beside the word it is read from
                                 (unsigned long long)nt.csTouch,
                                 r.touchCsRead ? "read" : "UNREAD", (unsigned)r.touchIndex,
                                 (unsigned)r.touchCount, r.touchLive ? "LIVE" : "not live",
                                 (unsigned)r.touchHolderA, (unsigned)r.touchHolderB,
                                 (unsigned)r.touchHand, (unsigned)r.touchFlags);
                    ++lines;
                }

                // DISC-RING-5: the same bit counted on the PLUGIN's side of the same node, so a
                // gate that reads held-bit 0 separates "never written" from "written and gone".
                {
                    const entity::NodeHeldDiag d = entity::NodeHeldDiagRead();
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census ring4 node: key %s, level %s; publishes %u, resolve "
                                 "refused %u, no head node %u, stamped %u, pushed onto %u; hist "
                                 "count %llu, last word %04X",
                                 entity::DiscNodeHeldEnabled() ? "ON" : "off",
                                 d.lastHeld ? "held" : "free", d.calls, d.resolveFail, d.headNull,
                                 d.stamped, d.pushes, (unsigned long long)d.histCount,
                                 (unsigned)d.lastState);
                    ++lines;
                }

                static constexpr uint64_t kDiscScript[] = {0x38965D90A823F03Full};
                static IEntity::ScriptSelfRow discRows[4];
                static IEntity::ScriptHashRow discHashes[8];
                IEntity::ScriptCensus sc2;
                sc2.space = discSpace;
                sc2.wanted = kDiscScript;
                sc2.nWanted = 1;
                sc2.rows = discRows;
                sc2.rowCap = (uint32_t)(sizeof discRows / sizeof discRows[0]);
                sc2.hashes = discHashes;
                sc2.hashCap = (uint32_t)(sizeof discHashes / sizeof discHashes[0]);
                if (!ent->ScriptSelfCensus(sc2)) {
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census ring2: disc script walk refused -- %s",
                                 sc2.refused ? sc2.refused : "unknown");
                    ++lines;
                } else {
                    for (uint32_t i = 0; i < sc2.rowsWritten; ++i) {
                        const IEntity::ScriptSelfRow& s = sc2.rows[i];
                        char words[8 * IEntity::ScriptSelfRow::kDataWords + 1] = {};
                        int at = 0;
                        for (uint32_t w = 0; w < IEntity::ScriptSelfRow::kDataWords &&
                                             at >= 0 && at < (int)sizeof words - 1;
                             ++w)
                            at += snprintf(words + at, sizeof words - (size_t)at, "%08X",
                                           s.dataRead ? s.data[w] : 0u);
                        g.log->Write(LogLevel::Warn, kReplayModuleName,
                                     "census ring2 script[%u]: actor %016llX ci %u state %d; self %s "
                                     "space %016llX actor %016llX node %016llX; gametime %lld; data "
                                     "%s %s",
                                     i, (unsigned long long)s.actorLo, (unsigned)s.ciIndex,
                                     (int)s.state, s.answered ? "READ" : "UNANSWERED",
                                     (unsigned long long)s.selfSpace,
                                     (unsigned long long)s.selfActor,
                                     (unsigned long long)s.selfNode, (long long)s.graphTime,
                                     s.dataRead ? "read" : "UNREAD", words);
                        ++lines;
                        // ★ RE-DISC-RING-3 §6's R1, on the row that just answered. `bubble` is the
                        // only actor variable the graph compiles in with nodeid -1, so an unbound
                        // one is not a failure of this read -- it is the reading that retires the
                        // whole grab-ring thesis. The flags say which disc this instance thinks it
                        // is, which is the other half of the same table.
                        if (!s.discRead) continue;
                        char flags[160];
                        int fa = 0;
                        for (uint32_t f = 0; f < IEntity::kDiscFlagCount && fa >= 0 &&
                                             fa < (int)sizeof flags - 1; ++f) {
                            const bool answered = (s.flagsAnswered & (1u << f)) != 0;
                            fa += snprintf(flags + fa, sizeof flags - (size_t)fa, " %s=%s",
                                           IEntity::kDiscFlagNames[f],
                                           !answered ? "UNANSWERED"
                                                     : ((s.flagsValue & (1u << f)) ? "1" : "0"));
                        }
                        g.log->Write(LogLevel::Warn, kReplayModuleName,
                                     "census ring2 script[%u] R1: bubble %s space %016llX actor "
                                     "%016llX node %016llX;%s; model_component %s%016llX",
                                     i, s.bubbleAnswered ? "READ" : "UNANSWERED",
                                     (unsigned long long)s.bubbleSpace,
                                     (unsigned long long)s.bubbleActor,
                                     (unsigned long long)s.bubbleNode, flags,
                                     s.modelComponentAnswered ? "" : "UNANSWERED ",
                                     (unsigned long long)s.modelComponent);
                        ++lines;
                    }
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "census ring2: disc script 38965D90A823F03F in the DISC's space -- "
                                 "%u instance(s) detailed%s, %u ncaScript component(s) walked",
                                 sc2.rowsWritten,
                                 sc2.rowsWritten == sc2.rowCap ? " (CAPPED)" : "", sc2.walked);
                    ++lines;
                }
            }
        }
    }

    // ★ LAST, AND DELIBERATELY. The sink keeps the newest lines of a burst and this census writes
    // several hundred inside one tick, so the two blocks the cutoff question turns on are written
    // where a drop cannot reach them: the game-sync context every container resolves against, and
    // the one line carrying the distance at which sound is still playing.
    lines += LogFoleySyncCensus("census");
    if (LogFoleyCompact("census")) ++lines;
    return lines;
}

bool ReArmIdentityDress(bool force) {
    // Asked for by hand, so it re-arms whether or not the pass gave up -- a viewer who types
    // `dress` wants the roster dressed again, and a pass that is still retrying has nothing to
    // re-ask.  The two gates that always hold: a file has to be loaded, and the process has to be
    // contained, because past here the ladder writes another user's identity.
    if (!g.loaded || !g.dress_enabled || !livesafety::Allowed(livesafety::Feature::Dress)) {
        return false;
    }
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "identity dressing re-armed (asked to): %s",
                     force ? "every recorded player is dressed again from scratch"
                           : "the players this load has not dressed yet are dressed; the dressed "
                             "ones are left alone (`dress force` re-dresses them)");
    }
    ArmDressPass((uint32_t)g.reader.GetManifest().playerCount, !force);
    return true;
}

// * ONE DEFINITION OF THE SEAT'S SESSION MODE.  Here rather than in
// viewer_role.cpp because this is the translation unit that owns `g.loaded` -- the fact a
// successful open sets, from the launch arm and from ui::LoadPath alike -- so the predicate reads
// the state the module already holds instead of a second copy of it.
//
// A LIVE CAPTURE IS NOT A TERM (L3 RUN-31 F1).  The plugin records during playback sessions too --
// locked constraint 6, capture is always on -- so a capture is recording on the state-9 tick of
// EVERY playback launch there is: a recording term refuses the seat in all of them, which is the
// 0 x role=2 and the first-person view with hands and HUD that RUN-31 measured.  The session that
// must not be seated is the capture-ONLY one (L2 #17), and `g.loaded` is false there, so the one
// term below is the whole rule.
SeatScope SessionSeatScope() {
#if defined(SEAT_BLOCKED_BY_RECORDING)
    // Mutation control: the pre-fix term, refusing the seat while a capture is recording.
    if (capture::CaptureIsRecording()) return SeatScope::NoDemoOwned;
#endif
#if defined(SEAT_WITHOUT_DEMO)
    // Mutation control: drop the only term, seating a capture-only session as well.
    return SeatScope::Playback;
#else
    return g.loaded ? SeatScope::Playback : SeatScope::NoDemoOwned;
#endif
}

bool PlaybackOwnsSession() { return SessionSeatScope() == SeatScope::Playback; }

}  // namespace playback
}  // namespace replay

// ★★ THE MODULE TICK IS NULL ON PURPOSE, and the macro permits it. This module drives its frame
// from ITick::RegisterEarly, which is the stage-0 slot W1-D established and the position S7.1's nine
// steps are specified at. Registering BOTH would tick the playhead twice per frame -- the transport
// would advance at 2x with every rate reported as 1x, which is the kind of defect that looks like a
// clock bug for a week.
REPLAY_MODULE("playback", ::replay::kModuleOrderPlayback, ::replay::playback::Init, nullptr,
              ::replay::playback::Shutdown);
