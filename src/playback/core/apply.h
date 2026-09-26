#pragma once

// W2-B. ApplyStateAt -- the one function in this packet that writes engine state.
//
// ---------------------------------------------------------------------------------------------
// ★ IT TAKES NO DISCONTINUITY PARAMETER, AND THAT IS THE DESIGN.
//
// The design specifies seek as a nine-step procedure that applies keyframe state, fires a
// battery, rebuilds rings and pre-rolls -- running BESIDE S7.1's nine-step frame, which also applies
// state. Two code paths that must agree about "the complete state at time T" drift, and the symptom
// is a replay that looks right when you play into a moment and wrong when you scrub to it.
//
// So there is one path. Linear play, scrub, reverse, fast-forward and seek all call Apply(w, t, mode),
// and it CANNOT know which one it is -- there is no channel through which to find out. Path
// independence is therefore something an implementation would have to work to violate, rather than a
// property a test has to keep catching. What a jump costs is elsewhere: the window's cursors are
// invalidated, and the residue battery fires. Neither is a second way to apply state.
//
// ★ ONE NODE PER BODY, AND IT IS CORRECT ON BOTH ENGINE ARMS. F5: offline, ENTITY.UPDATE_TARGET_STATE
// skips both the interpolate and the extrapolate branch (four IS_HOST tests gate them) and copies the
// head history node VERBATIM. So the ring needs exactly one node, holding the state WE interpolated at
// the playhead. And if the offline bit were ever cleared, ENTITY.INTERPOLATE_PHYS_STATE's second
// early-out -- `if (!head->next) copy head; return` -- produces the same value. One node is not a
// simplification of a straddling window; it is the shape that has no wrong answer on either arm, and
// it makes an out-of-order list (F2) and a bad sample time (F1) unrepresentable rather than guarded.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include "engine/entity.h"
#include "engine/netvar.h"
#include "playback/core/netvarapply.h"
#include "playback/core/ownership.h"
#include "playback/core/sample.h"
#include "playback/core/window.h"

namespace replay {
namespace playback {

// The four answers netvar replay can give. The two quiet ones are NOT one state: a capture that
// carries no netvar records has nothing to apply -- a solo -offline host produces none by
// construction (ARCH-21) -- and reporting that as a playback which failed to apply what it held
// fails a file for a property of the file. The counters cannot separate them: both leave attempted,
// applied, refused and unchanged at zero. netvars_present is what does.
enum class NetVarFidelityState : uint8_t {
    NoRecords = 0,        // the window carried no netvar keys at all: there was nothing to apply
    PresentNotApplied,    // keys are present; none of them changed at this playhead
    Applied,              // every write returned true
    Refused,              // at least one write returned false -- no apply path on this build
};

const char* NetVarFidelityText(NetVarFidelityState);

enum class DiscDrive : uint8_t {
    NotReached,         // no claimed track in this window, or the apply never reached it
    Absent,             // no sample at or before this playhead: the disc has not joined yet
    LocallyControlled,  // the claim did not take, so the engine still simulates it: not installed
    Refused,            // the install ran and refused; disc_install_refused names the gate
    Installed,          // the pose reached the engine's ring
    HostHeld,           // deliberately not installed: the host is still parking the disc each frame
};

inline const char* DiscDriveName(DiscDrive d) {
    switch (d) {
        case DiscDrive::Absent:            return "absent";
        case DiscDrive::LocallyControlled: return "locallyControlled";
        case DiscDrive::Refused:           return "refused";
        case DiscDrive::Installed:         return "installed";
        case DiscDrive::HostHeld:          return "hostHeld";
        case DiscDrive::NotReached:        break;
    }
    return "notReached";
}

struct ApplyStats {
    uint32_t bodies_installed = 0;
    uint32_t bodies_absent = 0;          // no sample at or before t: E-P4, correctly not published
    uint32_t bodies_unhandled = 0;       // the file has it, this session has no handle for it yet
    uint32_t bodies_locally_controlled = 0;  // ★ INV-OWN1's observable. Non-zero is a real defect.
    uint32_t bodies_install_failed = 0;  // the install refused: bad window, no element, the
                                         // identity gate declined, or the ring came up short
    // ★ THE WRITE AND THE READ-BACK ARE TWO FACTS. `bodies_installed` counts what the install
    // RETURNED; these count what the engine's ring HOLDS afterwards. A session reporting installs
    // with nothing read back has driven a body nothing can observe -- the shape DIAG-PAWN-STATIC
    // found, where a pose line printed our own source sample and read as motion.
    uint32_t bodies_read_back = 0;        // installed, and the ring reads the pose back equal
    uint32_t bodies_installed_unread = 0; // installed, and the read-back differs or refuses
    // The install's own identity gate, named. A subset of bodies_install_failed, kept apart
    // because "the element is not there" and "the element is there and unvouched" are different
    // findings and only the second says we were about to drive an unvouched CI.
    uint32_t bodies_install_identity_refused = 0;
    // A slot-bound body whose engine counterpart this machine simulates, claimed this call through
    // SetSyntheticOwner. Once per track per session: after the claim the engine answers "not locally
    // controlled" and the branch is not reached again, so a non-zero count on every frame would mean
    // the claim is not sticking.
    uint32_t bodies_owner_claimed = 0;
    // ---- the disc's possession level (RE-GRAB-POSSESSION P1) --------------------------------
    // A LEVEL change, not an edge: the engine's own touch CS derives grab and release by diffing
    // this against its latch. On the reference capture the disc changes hands 6 times in 48.5 s,
    // so a session with hundreds of stamps is a level that is not sticking, not a busy match.
    uint32_t disc_owner_stamps = 0;
    // The recording named a holder this session has no entrant for -- a spectator, or a slot the
    // roster never mapped. Published as unowned; never as a guess at somebody else's id.
    uint32_t disc_owner_unmapped = 0;
    // The engine answered "locally controlled" for a real recorded owner, so the out-of-table value
    // went back. Non-zero means the admitted entrant landed in the LOCAL band, which is the one
    // premise P1 rests on; a launch that reports it has refuted that premise and says so.
    uint32_t disc_owner_fallbacks = 0;
    // A recorded "nobody" span began while a real holder was published; the holder stayed named.
    uint32_t disc_owner_held_over = 0;
    uint32_t disc_owner_refused = 0;    // the element did not resolve, or the store refused
    // The engine took the disc back -- its own touch handling transfers the body to this host on
    // the grab/release edges our level publishes, and frees the history nodes as it goes. This
    // counts the ticks that put our level back on the dword so the drive could continue.
    uint32_t disc_owner_reclaims = 0;
    uint32_t disc_owner_seen = 0;        // what PhysCI+0x1C held when that happened
    bool     disc_owner_seen_valid = false;
    uint32_t disc_owner_value = 0;          // the dword last stamped into PhysCI+0x1C
    uint32_t disc_owner_recorded_slot = 0;  // the recorded user slot it came from
    uint16_t disc_owner_engine_slot = 0xFFFF;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    // What the engine's own holder gates answer for the disc, read back once per level change.
    IEntity::HolderReading disc_holder;
    // The disc track's install refusal this frame, by name; `why` None when it installed or was
    // not attempted. Written on the refusal path only, so read `disc_drive` to tell the two apart.
    InstallReading disc_install_refused;
    // WHICH of the four ways the claimed track's pose did or did not reach the engine. Without it
    // an install that never ran and one that ran and refused leave the same silence, and the only
    // surviving symptom is the read-back's `badSlot` -- a refusal about the LOCAL physics CS that
    // says nothing about why the driven ring is empty (DIAG-DISC-FROZEN-REGRESSION §3.4).
    DiscDrive disc_drive = DiscDrive::NotReached;
    // The claimed track produced a sample at this playhead, so whatever was going to happen to the
    // claim has happened. The one-shot disc probe waits for this rather than for a claim that took.
    bool disc_sampled = false;
    uint32_t players_published = 0;
    uint32_t players_absent = 0;
    uint32_t disc_touch_writes = 0;     // IsHeld / LastOwner level changes
    uint32_t disc_touch_refused = 0;
    uint32_t players_skipped_asseen = 0; // occluded / not updated that frame -- M1, deliberate
    uint32_t players_unhandled = 0;
    // ★★ TWO COUNTS, because they were one and the one could not be read.
    //   players_unmapped        W2-C has not resolved this capture slot to a live user index, so
    //                           there is no owner to publish under.  A wiring state: the world
    //                           does not move and nothing is wrong with the file.
    //   players_refused_owner   the mapping IS there and OwnerGuard refused the id it holds.
    //                           An INV-OWN1 finding about this replay, and a much louder event.
    // Collapsed together, "every player refused" read as an ownership violation for as long as the
    // seam was unwritten -- and a permanent unwired state wearing a guard's name is the failure
    // this project keeps rediscovering.
    uint32_t players_unmapped = 0;
    uint32_t players_refused_owner = 0;
    // ★★★ THREE COUNTS, BECAUSE ONE OF THEM WAS A LIE.
    //
    // netvars_written used to increment on the CALL. INetVar::Write() unconditionally refuses on this
    // build -- there is no established apply path, and W2-A refuses loudly rather than writing to a
    // guessed store offset inside a running game -- so the only netvar observable this packet had
    // reported success for a no-op, and score, clock and round state do not replay.
    //
    // That is the same disease as the bit-length finding one round earlier: a counter that increments
    // on the attempt rather than on the effect. So `applied` is now measured from INetVar's own
    // refusal counter across the loop, and it is zero on this build -- which is the truth.
    uint32_t netvars_attempted = 0;
    // ★★ MEASURED FROM INetVar::Write's RETURN VALUE, which W2-A added at this packet's request
    // for the reason that matters: a void return makes success unobservable through the interface, so
    // a caller can only count attempts -- which is how the one netvar observable came to report
    // success for a no-op. It is ZERO on this build, because no apply path is established on PC, and
    // zero is the honest number rather than a gap.
    uint32_t netvars_applied = 0;
    uint32_t netvars_refused = 0;
    uint32_t netvars_unchanged = 0;
    // ★ The window's netvar key count at this playhead -- the ONLY field that separates "this
    // capture has no netvar records" from "it has them and none was due here". Summed reports keep
    // the largest key set seen rather than a total, because a total over frames counts one key once
    // per frame and would read as a record count it is not.
    uint32_t netvars_present = 0;
    // ★★★ THREE STATES, AND IT WAS A BOOL. `netvar_apply_available` answered FALSE both when writes
    // were REFUSED and when none were ATTEMPTED -- so "we tried and the engine has no apply path" and
    // "this frame had no netvars to write" rendered identically, as one string on screen.
    //
    // That is the defect W2-F found in its own ContentMismatch one seam away, and mine was worse
    // because ActiveModeLabel renders it: a file with no netvar stream would have told the owner his
    // score and clock were STORED, NOT APPLIED, which is a claim about the world that is not true.
    // A caller cannot recover the third state from the bool, so the bool is gone.
    bool netvar_apply_available = true;   // kept for callers that only want "is anything wrong"
    NetVarFidelityState netvar_state = NetVarFidelityState::NoRecords;

    void Add(const ApplyStats& o);
    // One line for a log or a UI label. Never claims applied when nothing landed, and never claims
    // NOT applied when nothing was asked of it.
    const char* NetVarFidelity() const { return NetVarFidelityText(netvar_state); }
};

// Optional read-back verification. Off by default: it doubles the per-body engine traffic, so it is
// for a gate run and a bring-up launch, not for the shipping tick.
struct ApplyOptions {
    bool as_seen = false;
    bool verify_installed = false;
    // The playhead is running backwards this frame. Only the sampled velocities care: see
    // SampleBodyAt in sample.h for why they are negated rather than zeroed.
    bool reversed = false;
    // True for a (name, slot) another writer drives this frame (netvarhud's typed setters). Such a
    // row is not attempted here, so it is neither written twice nor counted refused.
    bool (*netvar_owned_elsewhere)(uint64_t name, uint16_t slot) = nullptr;

    // Publish the recorded per-hand grab input, so the engine's own grab state machine runs for
    // replayed players -- which is what makes a wall grab or a player grab sound and attach. Off
    // leaves the input at 0, which is what every build before this one published. `playback.grip_inputs`.
    bool grip_inputs = true;

    // ONE WRITER FOR THE DISC. Before its first launch of this load the offline host's arena script
    // parks the frisbee at the arena origin every frame, and our install goes in at engine stage 0 --
    // earlier in the frame than the script's own pass -- so the two writes render on alternate
    // frames. While this is set the disc's pose is left to the host; the owner claim is still
    // re-asserted, so the engine does not take the body over in the meantime.
    bool hold_disc_pose = false;

    // Cubic Hermite through the recorded velocities for body POSITION between the 45 Hz samples,
    // instead of a straight line. `playback.hermite`.
    bool hermite = true;
    // Where the per-bracket interpolant counts go. The caller owns the counters and decides when
    // to read and clear them, so this pass allocates nothing and keeps no state of its own.
    HermiteTally* hermite_tally = nullptr;
    // Motion smoothing for this frame. nullptr, or a weight of 0, is the 1:1 path untouched.
    const SmoothFrame* smooth = nullptr;
};

// The one dispatch between the 1:1 samplers and the smoothed ones, shared by Apply and every
// diagnostic that has to read back the interpolant that was actually driven. `cursor` is the caller's.
bool SampleBodyFor(Window& w, BodyTrack& tr, SeriesCursor& cursor, int64_t t_us, BodyState& out,
                   bool reversed, bool hermite, HermiteTally* hermite_tally, const SmoothFrame* smooth);
bool SamplePlayerFor(Window& w, PlayerTrack& tr, SeriesCursor& cursor, int64_t t_us, PlayerState& out,
                     bool reversed, const SmoothFrame* smooth);

// The engine's invalid SR15NetUserId, dword_141FFCEA0 -- "this hand is holding nothing". Read out
// of the image rather than invented: sub_140CCE580 compares the hold dword against it exactly, on
// both sides of a frame, before it will raise the hand-attached-to-geo delegates.
inline constexpr uint32_t kHoldNobody = 0xFFFFFFFFu;  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

class StateApplier {
public:
    void Bind(IEntity* entity, IPosePublisher* pose, INetVar* netvar, OwnerGuard* owners);

    // The engine's netvar update-record table, and the level id the engine compares a rowset's
    // owner field against. Separate from Bind because it is the ONE surface that is bound from the
    // live level rather than from the service registry, and a playback with no level bound must
    // still run every other write -- it refuses netvars by name (`netvars-no-table`), not silently.
    void BindNetVarTable(INetVarTable* table, uint64_t owner) {
        netvar_table_ = table;
        netvar_owner_ = owner;
    }
    INetVarTable* NetVarTable() const { return netvar_table_; }
    const NetVarApplyStats& NetVarApplyStatsRef() const { return netvar_apply_; }

    // Re-take the three published pointers; true when any of them changed. A binding taken at Init
    // is stale on the shipping path: W2-A publishes INetVar from its FIRST TICK, after every Init
    // has run, and W1-E re-publishes when the game space changes (S9.14: source order is not
    // execution order). Called every tick -- three pointer compares.
    bool Rebind(IEntity* entity, IPosePublisher* pose, INetVar* netvar);

    IEntity* Entity() const { return entity_; }
    IPosePublisher* Pose() const { return pose_; }
    INetVar* NetVar() const { return netvar_; }

    // THE function. Writes bodies, then players, then netvars -- in that order, because a netvar
    // handler can read player state and a handler that ran against last frame's pose would be a
    // carried dependency, which INV-P forbids.
    ApplyStats Apply(Window& w, int64_t t_us, const ApplyOptions& opt);

    // Which claims a release hands back. The disc's is different in kind from every other one: the
    // engine EDGE-DETECTS PhysCI+0x1C, so restoring the original owner between two identical levels
    // is a release and a grab the owner sees and hears -- un-tint, spin, catch sound -- for a seek
    // that did not change who is holding the disc. A transport discontinuity therefore keeps the
    // disc's level and lets the next publish swap it in one store; only a real teardown drops it.
    enum class OwnerRelease {
        All,           // unload, session end, re-bind to a different disc, disc drive off
        KeepDiscLevel  // seek, micro-seek, gap cross, direction flip, scrub step, window rebuild
    };

    // The release half of the claim this file makes at apply.cpp:128. entity.h:473 declares
    // `virtual void ClearSyntheticOwner(PhysHandle) = 0;`, and an owner that outlives the window
    // keeps the engine treating a replayed body as locally owned across stop, unload and seek.
    // Returns how many tracks were released.
    size_t ReleaseSyntheticOwners(Window& w, OwnerRelease scope = OwnerRelease::All);

    // After a re-bind: a held disc level survives only while the handle carrying it is still the
    // disc. Re-seeds the new track from the held level when it is, and hands the engine's original
    // owner back when the bind landed elsewhere or landed nothing. Returns 1 if it released.
    size_t ReconcileDiscLevel(Window& w);

    // The component is gone: drop the held level without restoring it, for the same reason
    // ForgetSyntheticOwner exists.
    void ForgetDiscLevel();

    // True while the disc's possession level is stamped on the engine's dword.
    bool DiscLevelHeld() const { return disc_level_.held; }

    // entity.h:483. Bounds the engine's per-CI pose history to the seek window. Called at the
    // WINDOW's cadence, never per record: TrimHistory walks from the tail, so a per-record call
    // would rewalk the same list once for every sample in it.
    size_t TrimHistories(Window& w, uint64_t keepFromUs, uint64_t keepToUs);

    // Non-zero means a read-back disagreed with what we installed. Only meaningful with
    // verify_installed; it is the offline half of G4.
    uint64_t VerifyMismatches() const { return verify_mismatches_; }

private:
    // The disc's recorded holder at the playhead, stamped into PhysCI+0x1C when it changes.
    void PublishDiscOwner(Window& w, BodyTrack& tr, uint32_t recorded, int64_t t_us,
                          ApplyStats& st);

    // Put the level that IS published back on the dword after the engine has overwritten it.
    void ReclaimDiscOwner(BodyTrack& tr, ApplyStats& st);

    // The per-hand grab input and the hold targets, made publishable: the trigger blends the
    // engine's own grab machine runs on, and the hold ids translated out of recorded slot
    // numbering into this session's.
    void PublishGrip(Window& w, const PlayerTrack& tr, const ApplyOptions& opt, PlayerState& ps);
    // The disc's IsHeld / LastOwner pair, beside the possession stamp and in the same order.
    void PublishDiscTouch(Window& w, BodyTrack& tr, uint32_t recorded, int64_t t_us,
                          ApplyStats& st);
    uint32_t LastOwnerAt(Window& w, const BodyTrack& tr, uint32_t recorded, int64_t t_us);
    void ForgetDiscTouch();
    // The recorded slot mapped to the live owner dword the possession stamp publishes, with the
    // live slot in `engine_slot` or 0xFFFF when this window never mapped it.
    uint32_t MapRecordedOwner(Window& w, uint32_t recorded, uint16_t& engine_slot) const;
    uint32_t RemapHoldTarget(Window& w, uint32_t recorded) const;

    // ★ ONE THROTTLE PER TRACK, AND NO EDGE EVER DROPPED. The throttle used to be a single
    // function-level `static` shared by every track and both hands, so the whole session got about
    // one grip line per second handed to whichever track ran first after the second rolled over,
    // and a grab shorter than a second could not be sampled at all
    // (DIAG-DISC-FROZEN-REGRESSION §3.3b). Every CHANGE of either hand's target now prints; only
    // an unchanged line is throttled, and only against its own track's last line.
    struct GripReport {
        bool     seen = false;
        uint32_t recL = kHoldNobody, recR = kHoldNobody;
        uint32_t pubL = kHoldNobody, pubR = kHoldNobody;
        uint64_t reported_ms = 0;
    };
    // The recorded user slot indexes this directly: ENTITY.USER_SLOT_TABLE_MAX is the engine's own
    // table bound and the recorded slots come from that same table.
    static constexpr size_t kGripTracks = 16;
    GripReport grip_[kGripTracks];

    // The level that is stamped on the engine's dword RIGHT NOW. Held here and not on the
    // BodyTrack because a window rebuild destroys every track while the dword keeps our value:
    // the track is where the level came from, this is the record that it is still out there.
    struct DiscLevel {
        PhysHandle handle{0xFFFFu};  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        uint32_t   value = 0;
        uint32_t   recorded_slot = 0;
        uint16_t   engine_slot = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        bool       held = false;
        bool       real = false;
    };
    DiscLevel disc_level_;
    void SeedTrackFromDiscLevel(BodyTrack& tr) const;

    // ★ LastOwner is a pure function of the playhead, and this is only the memo that keeps it from
    // being re-derived every tick. `lastOwner(T)` is the newest recorded non-empty holder at or
    // before T that is still inside T's round, so the memo is valid only while the playhead moves
    // forward from the sample it was taken at AND the recording's `game_state` has not changed
    // since. Either of those failing re-derives it by a backward scan -- a seek back past the last
    // touch, or a round boundary, are exactly the two cases the owner's ring has to come back for.
    struct LastOwnerMemo {
        bool     valid = false;
        uint32_t owner = 0;          // the live owner dword, not the recorded slot
        uint32_t recorded_slot = 0;
        int64_t  at_us = 0;          // the sample it came from
        uint64_t game_state = 0;     // the round it belongs to
        bool     state_known = false;
    };
    LastOwnerMemo last_owner_;
    // The disc touch pair as last published, so a steady possession costs one compare.
    bool     touch_published_ = false;
    bool     touch_held_ = false;
    uint32_t touch_last_owner_ = 0;

    // ★ THE LOG'S OWN DEDUPE, kept apart from the write's. The write is skipped when the engine's
    // dword already names this holder, which is right -- but it also silenced the owner's only
    // read-out for "did the disc change hands correctly": one launch crossed seventeen hand-overs
    // and printed three lines (LAUNCH-LIVEFIX 4.2). Every CHANGE of the recorded holder prints
    // exactly one line now, whether or not it cost a write.
    void NoteOwnerChange(uint32_t recorded, int64_t t_us, const char* why);
    uint32_t owner_logged_slot_ = 0;
    bool     owner_logged_valid_ = false;

    IEntity* entity_ = nullptr;
    IPosePublisher* pose_ = nullptr;
    INetVar* netvar_ = nullptr;
    INetVarTable* netvar_table_ = nullptr;
    uint64_t netvar_owner_ = 0;
    NetVarApplyStats netvar_apply_;
    OwnerGuard* owners_ = nullptr;
    uint64_t verify_mismatches_ = 0;
};

}  // namespace playback
}  // namespace replay
