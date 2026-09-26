#pragma once

// W2-C's product: take one recorded PlayerRecord and make the replayed player wear what the
// captured player wore -- name, profile, loadout, numbers and team colour -- then CHECK it,
// because every failure on this path is silent by default.
//
// The name is checked on the OBSERVABLE: what UserDisplayName renders, not what we stored.
// Three gates decide that answer and the recorded profile is only the third of them, so the
// two write paths (profile and slot table) are both driven and neither is the check.
//
// SC6 in one sentence: the twenty item ids, both numbers and the alignment are compared
// SLOT BY SLOT against what the engine actually applied, and the comparison distinguishes
// three outcomes that a bare "did it match" would collapse into one:
//
//   matching   the engine holds the id we asked for
//   remapped   the engine holds a DIFFERENT non-zero id.  This is not a fault: LoadAndApplyItem
//              rewrites three ids to hard-coded defaults when the `net_standard_chassis` rule
//              var is on, at apply time, and a replay that recorded the raw loadout reproduces
//              that automatically.  Counting a remap as a strip would make every match red.
//   absent     the engine holds nothing where we asked for something.  THIS is the failure
//              DESIGN 9.3 breaker 2 describes, and it is what the diff exists to catch.

#include <stdint.h>
#include <stddef.h>

#include "bootstrap/services.h"
#include "engine/identity_access.h"
#include "foundation/demofile/demotypes.h"
#include "generated/facts_generated.h"

namespace replay {
namespace identity {

struct ItemDiff {
    uint32_t compared;
    uint32_t matching;
    uint32_t remapped;
    uint32_t absent;
};

// Pure: no engine, no state.  `want` and `got` are both twenty entries; `compared` counts the
// positions where the recording asked for a non-zero id, because a recording that carries
// fewer than twenty equipped slots must not be scored against empty ones.
ItemDiff DiffItems(const Symbol64 want[20], const Symbol64 got[20]);

// Which of the engine's identity fields a dress actually wrote, in the order Dress writes them.
// This is not bookkeeping for its own sake: it is the residue's contents.  Vacating a field we
// never wrote would undress an entrant this session never dressed, and skipping one we did write
// leaves the previous replay's identity on screen -- so the vacate is driven from what happened,
// never from what was intended.
struct IdentityFields {
    bool profile, slotName, team, loadout, alignment;
};

struct DressResult {
    bool     ok;
    // EVERY engine write this dress owes the slot was issued -- the ladder reached the read-back.
    // It is deliberately NOT `ok`: `ok` is the VERDICT (the engine renders the recorded name and
    // holds every recorded item), and a verdict can stay false for reasons a second pass cannot
    // mend -- an item the engine remapped to nothing, a name gate we do not control.  Re-running
    // the ladder for those would re-apply the loadout to the actor a few times a second for the
    // rest of the load.  `applied` is what the retry latches on: once it is true this slot is
    // done for this load and later passes skip it.
    bool     applied;
    ItemDiff items;
    uint16_t numberWanted, numberBodyWanted;
    uint16_t alignmentWanted;
    uint32_t profileScalars;      // scalar leaves of the recorded profile that were carried
    uint32_t uncarriedArrays;     // arrays that could not be -- `unlocks` is one of these
    char     firstUncarried[256];
    // THE name check.  `nameShown` is what CR15NetGame::UserDisplayName returned for this
    // entrant -- the observable, not either of the two sources we wrote -- and it is carried
    // out of Dress so a mismatch can be logged as the string the player will actually see.
    // "Unknown" here is the engine telling us the whole name path failed.
    bool     nameResolved;        // UserDisplayName answered at all
    char     nameShown[64];
    bool     nameMatches;         // ...and what it answered is the recorded displayname
    identity_access::Refusal refusal;
    const char* stage;            // which step refused, for the log line
    char     detail[192];
    // THE seam W2-B needs, and it is deliberately NOT gated on `ok`.  Step 1 answers a different
    // question from steps 2..7: `indexResolved` says the engine gave this recorded slot a live
    // UserIndex, while `ok` says the avatar it is wearing is right.  A player whose loadout came
    // back short still has a real identity, and refusing to publish its body over a wrong skin
    // would freeze it -- which is the failure this seam exists to remove, arriving from the
    // other side.  `userIndex` is meaningless unless `indexResolved`.
    bool     indexResolved;
    uint16_t userIndex;
    // WHERE the engine put the entrant this dress admitted, which is not where the file recorded
    // it: ARCH-23 S7.2's step 1 asks for the recorded slot and the engine answers with the slot it
    // had free (the viewer's own user holds slot 0 with an empty profile, so a recorded slot 0 is
    // routinely remapped).  Every write below that addresses the engine's USER is keyed on this;
    // the names, loadout, skins and colours still come from the recorded PlayerRecord.  It is the
    // engine's own answer -- SlotOf on the index the admission returned -- and never our arithmetic.
    uint16_t slotResolved;
    // Step 1's other refusal vocabulary.  A provision that refused did not reach any of the five
    // predicates, so reporting an identity_access::Refusal for it would name a step that was never
    // run -- which is how L3 #11 came to read "the entrant's profile CJson is absent or empty" for
    // a player the session had never admitted.
    PawnProvision::Refusal provisionRefusal;
    // Step 3's own word, and it is separate from `refusal` because it is NOT a failure of the
    // dress: a recording that never captured a team costs the team and nothing else.  `None`
    // means the step ran -- `wrote.team` then says whether the engine took it.
    identity_access::Refusal teamRefusal;
    // The same shape as `teamRefusal`, for the two other steps that can have no INPUT rather than
    // be refused: a row the capture recorded no profile document for, and one whose twenty item
    // ids are all the no-model sentinel.  `None` means the step ran.
    identity_access::Refusal profileRefusal;
    identity_access::Refusal loadoutRefusal;
    // Not a player at all: the recorded team is at or above IDENT.ALIGN_TEAM_LIMIT.  The engine
    // gives such an entrant no actor, so it is never admitted and never dressed.
    bool     spectator;
    // Step 1 re-bound to an entrant an earlier load in this session provisioned, instead of
    // admitting a new one.  The launch reads this to tell "three players" from "three players
    // twice": every adopted row is a row that would otherwise have been a duplicate.
    bool     adopted;
    IdentityFields wrote;
};

// WHAT THE CAPTURE SAYS THIS ENTRANT'S PROFILE DOCUMENT IS, asked of the caller rather than read
// here: the document lives in the recording's identity chapter, which src/playback owns, and this
// module has to stay linkable with no game and no reader present.  `recordedSlot` is the RECORDED
// user slot.  Null provider, or a provider that answers false, means the row has no document and
// the ladder falls back to the one it can author itself.
struct DressEnv {
    bool (*recordedDocument)(uint16_t recordedSlot, Blob& out) = nullptr;
    // playback.dress_loadout. False skips step 4 -- the twenty item ids, both numbers -- and
    // nothing else: the name, the number the slot table carries, the team and the colour still go
    // on. Defaulted true so every existing caller and every test keeps the shipped ladder.
    bool applyLoadout = true;
};

// No slot maps here.  0xFFFF is not a user index on this build -- the engine's own bounds-checked
// slot accessors return it past the count -- and index 0 IS valid, so a zero-filled mapping must
// never read as "slot 0 is dressed as index 0" (DESIGN 9.2: a missing value may not default to a
// confident answer).
inline constexpr uint16_t kNotDressed = 0xFFFFu;

// The verdict, factored out so it can be checked without an engine.  Dress's ladder needs a
// live CR15NetGame end to end, so the rule that decides pass/fail would otherwise have no
// offline coverage at all -- and it is the rule C-2 defeated: an items-only verdict reports
// success for a player rendering "Unknown".
//
//   every position the recording filled is present  (items.absent == 0)
//   at least one was compared                       (items.compared > 0)
//   the engine RENDERS the recorded name            (nameMatches)
//
// All three, and none of them is redundant: drop the third and the owner's first requirement
// stops being checked while the gate stays green.
bool Verdict(const DressResult&);

// STEP 3, on its own so its two answers are drivable without a live netgame.
// True means the dress goes on: either the engine took the recorded team, or the recording had no
// team to apply and `out.teamRefusal` names that.  False means the engine refused a team we DID
// record, which is the only case that costs the rest of the dress.
bool ApplyTeamStep(IIdentity& id, const PlayerRecord& rec, UserIndex index, DressResult& out);

// Drives the six steps in the order the engine requires: entrant, profile, team, loadout,
// alignment, then the read-back.  Stops at the first refusal and names the stage.
// `adoptEngineSlot` is the ADOPT-OR-ADMIT seam.  ReleaseReplayUser refuses on this build, so an
// entrant an earlier load in this session provisioned is still in the engine's tables when the next
// load starts.  Admitting a second one for the same player is what doubled the roster; handed the
// engine slot the earlier load resolved, step 1 RE-BINDS to that entrant instead -- same live slot,
// this file's identity written over it.  kNotDressed, or a slot the engine no longer holds an
// entrant in, falls through to the admission.
bool Dress(IIdentity& id, const PlayerRecord& rec, DressResult& out,
           const DressEnv& env = DressEnv{}, uint16_t adoptEngineSlot = kNotDressed);

// Dress, then log the outcome whether it passed or failed.  The three things that otherwise
// leave no trace -- an absent item, an uncarried profile array, a display name that did not
// take -- are logged on the SUCCESS path too.  Takes its two dependencies explicitly so the
// caller (W2-B's load path) and an offline test drive the same function.
bool DressAndReport(IIdentity& id, ILog& log, const PlayerRecord& rec, DressResult& out);

// How loud a dress pass is allowed to be.  A load's first pass reports everything, as it always
// has.  A RETRY pass runs a few times a second for as long as the engine keeps deferring, so it
// reports only what CHANGED -- a slot that was deferred and now is not -- because five players
// times four lines times a few hertz buries the one line that says the wait ended.
enum class DressPass { First, Retry };

// What the driver does after a pass, as a pure rule: no engine, no clock of its own, so the thing
// that decides when a wait ENDS can be driven offline instead of only by a thirty-second launch.
//
// `since_ms` is the start of the current CONTINUOUS run of deferral and belongs to the caller; zero
// means nothing was deferring.  The run restarts whenever a player stops deferring, so a roster the
// engine admits one slot at a time is never given up on for taking its time -- only one that makes
// no progress at all for the whole bound.
enum class DressAction { Settled, KeepWaiting, GiveUp };
DressAction NextDressAction(uint32_t deferredNow, uint32_t deferredBefore, bool firstPass,
                            uint64_t now_ms, uint64_t boundMs, uint64_t& since_ms);

struct RosterReport {
    uint32_t players;   // records in the manifest
    uint32_t dressed;   // Verdict() true -- name rendered, every recorded item present
    uint32_t failed;    // refused at some stage, or the verdict said no; each is logged by name
    // THE FIELD THE RETRY TURNS ON, and it is separate from `failed` because a deferral is not a
    // decision: the entrant-add was queued and the engine answered "not in that phase yet", which
    // its own headers call a DEFERRAL twice and in capitals.  Counted into `failed` it read as a
    // permanent refusal, the driver cleared `pending`, and the roster was never dressed at all --
    // `engine held 0 of 20`, for the whole nine-minute session.
    uint32_t deferred;
    // Slots a PREVIOUS pass in this load already applied, skipped here.  Their verdict is carried
    // forward into `dressed`/`failed`, so those two still sum to `players` on every pass.
    uint32_t alreadyApplied;
    // Rows that are NOT players: the recorded team is at or above IDENT.ALIGN_TEAM_LIMIT.  They are
    // not admitted, not dressed and not deferred, so they are counted out of `players` too -- the
    // pass must not spend thirty seconds waiting for an entrant the engine will never give a body.
    uint32_t spectators;
    // The deferred slots by name, so the caller's give-up can say WHICH player is still waiting
    // and on WHAT without dressing the roster a second time to find out.  `deferred` is its length.
    struct Deferred {
        uint16_t                userSlot;
        PawnProvision::Refusal  why;
    };
    Deferred deferredSlots[facts::ENTITY_USER_SLOT_TABLE_MAX];
    // ...and the slots that left the deferred set WITHOUT being dressed.  A deferral ends in one of
    // three ways -- dressed, refused by name, or never a player -- and this is the middle one.  It
    // is a separate list from `deferredSlots` because the give-up report names who is still waiting
    // and this names who stopped waiting and is still undressed; collapsing them into `failed`
    // is how three players failed with nothing in the log naming them together.
    struct Stuck {
        uint16_t                 userSlot;
        identity_access::Refusal why;
    };
    uint32_t stuck;
    Stuck    stuckSlots[facts::ENTITY_USER_SLOT_TABLE_MAX];
    // The vacate obligation.  Four numbers because there are four outcomes and one of them --
    // vacateRefused -- is a slot still wearing the previous replay's identity, which is the thing
    // this whole mechanism exists to prevent.  Collapsing it into `failed` would hide it behind a
    // number that is about THIS roster's dressing.
    uint32_t residue;        // slots carried in from earlier loads in this session
    uint32_t vacated;        // residue slots this roster does not use, undressed here
    uint32_t staleSlot;      // the entrant is gone (a netGame teardown): nothing to undress
    uint32_t vacateRefused;  // a write refused; the slot is NOT clean and is kept for the next load
    // A residue slot THIS manifest names whose dress wrote nothing this pass (the provision
    // refused, say, on the re-dress after an entity re-bind).  It is deliberately NOT vacated --
    // the loaded replay owns the slot, and vacating it would strip the currently playing replay's
    // own player -- but it is not silently clean either: it still carries whatever an earlier pass
    // wrote, so it gets its own number and its own line.
    uint32_t dressRefused;
    // Players step 1 re-bound to an entrant this session already had, rather than admitting a
    // second one.  Zero on a first load and equal to the overlap on every load after it.
    uint32_t adopted;
};

// Capture user slot -> LIVE engine user index, for one loaded roster.
//
// ★ It is keyed on the SLOT, and it is an array OF slots rather than of records, for a reason
// that is not stylistic: the engine's user-slot table has ENTITY.USER_SLOT_TABLE_MAX entries and
// step 1 refuses anything past it, so a record this table cannot hold is a record that
// could not have been dressed either.  Sizing it any other way would introduce an overflow arm
// whose only possible behaviour is to attribute one player's index to another.
//
// ★ And the value is the LIVE index, never PlayerRecord::userIndex.  The recorded index belongs
// to the capture session's CR15NetGame; using it here would put a number from another process
// where INV-OWN1 expects this one's, and the guard would validate it and pass.
// ★ It also carries the RESIDUE, and that is why it is passed to both DressRoster and
// ReleaseRoster and why Clear() does not empty it.  IIdentity::ReleaseReplayUser refuses on this
// build (Refusal::NoReleasePath: the design's one named release mechanism, SNSLobbyEntrantRemoved,
// has no reachable consumer here), so an unreleased entrant
// is not a leak -- what survives is IDENTITY.  A slot replay A dressed keeps A's name, loadout and
// colours until something overwrites them, so a second replay that does not use that slot plays it
// back wearing A's face: a wrong replay that looks right.  The residue is this session's record of
// which slots it dressed and which fields it wrote, and the next DressRoster owes every one of them
// an outcome.  It survives Clear() because an entity re-bind invalidates the engine INDICES while
// leaving our own slot numbers -- and the entrants we dressed -- exactly as valid as before.
class RosterMapping {
public:
    static constexpr size_t kSlots = facts::ENTITY_USER_SLOT_TABLE_MAX;

    struct ResidueEntry {
        bool           used = false;
        uint16_t       userIndex = kNotDressed;  // at dress time; advisory -- re-resolved before any
                                                 // vacate write
        // The slot the ENGINE holds this entrant in, which is what every vacate write addresses.
        // Keyed on the recorded slot instead, a vacate would empty the profile and slot name of
        // whoever the engine has in the recorded slot -- the viewer's own user, in the shape L3 #11
        // ran.  Undressing the person watching the replay is not a lesser failure than leaving the
        // previous replay's face on screen.
        uint16_t       slotResolved = kNotDressed;
        IdentityFields wrote{};
    };

    RosterMapping() { Clear(); ClearResidue(); }

    // The mapping only.  The residue is deliberately untouched: see the class comment.
    void Clear() {
        for (size_t i = 0; i < kSlots; ++i) index_[i] = kNotDressed;
        mapped_ = 0;
    }

    void ClearResidue() {
        for (size_t i = 0; i < kSlots; ++i) residue_[i] = ResidueEntry{};
    }

    // Merged, never replaced: a load that got as far as the profile does not erase the record of an
    // earlier load's slot-name write, which is still on the engine and still has to be vacated.
    void NoteDressed(uint16_t userSlot, uint16_t slotResolved, uint16_t userIndex,
                     const IdentityFields& wrote) {
        if (userSlot >= kSlots) return;
        ResidueEntry& e = residue_[userSlot];
        // THE ENGINE SLOT IS RECORDED FOR EVERY ENTRANT THIS SESSION PROVISIONED, written into or
        // not: it is the number Unload hands back and the one every vacate write addresses, and a
        // pass that resolved a slot and then refused a cosmetic step still created that entrant.
        // `used` -- the obligation to UNDRESS the slot on the next load -- is only ever what this
        // replay actually wrote.
        e.userIndex = userIndex;
        e.slotResolved = slotResolved;
        e.used |= wrote.profile || wrote.slotName || wrote.team || wrote.loadout ||
                  wrote.alignment;
        e.wrote.profile   |= wrote.profile;
        e.wrote.slotName  |= wrote.slotName;
        e.wrote.team      |= wrote.team;
        e.wrote.loadout   |= wrote.loadout;
        e.wrote.alignment |= wrote.alignment;
    }

    void DropResidue(uint16_t userSlot) {
        if (userSlot < kSlots) residue_[userSlot] = ResidueEntry{};
    }

    const ResidueEntry& ResidueAt(size_t slot) const {
        static const ResidueEntry kNone{};
        return slot < kSlots ? residue_[slot] : kNone;
    }

    uint32_t ResidueCount() const {
        uint32_t n = 0;
        for (size_t i = 0; i < kSlots; ++i) if (residue_[i].used) ++n;
        return n;
    }

    // False for a slot the table cannot hold; the caller counts it rather than losing it.
    bool Set(uint16_t userSlot, UserIndex index) {
        if (userSlot >= kSlots || index.v == kNotDressed) return false;
        if (index_[userSlot] == kNotDressed) ++mapped_;
        index_[userSlot] = index.v;
        return true;
    }

    bool Lookup(uint16_t userSlot, UserIndex& out) const {
        if (userSlot >= kSlots || index_[userSlot] == kNotDressed) return false;
        out = UserIndex(index_[userSlot]);
        return true;
    }

    uint32_t Mapped() const { return mapped_; }
    uint16_t IndexAt(size_t slot) const { return slot < kSlots ? index_[slot] : kNotDressed; }

    // ---- the per-LOAD dress latch ---------------------------------------------------------
    // What makes the retry idempotent: a slot whose write ladder has run once is not run again on
    // this load.  Held here rather than in the residue because the two answer different questions
    // -- the residue is "what does this slot still WEAR, across loads", the latch is "did THIS
    // load already dress it" -- and because Clear(), which DressRoster calls on every pass, must
    // not touch either.
    //
    // ★ It is dropped by ArmDressPass(), which the load and the entity RE-BIND both call.  The
    // re-bind is the case that makes a latch dangerous: it issues a whole new set of user indices,
    // so a latch surviving it would skip every slot and leave the mapping holding numbers from the
    // previous game space.
    // The coarse outcome of the last pass over a slot.  A retry reports CHANGE, not repetition:
    // without this, a slot still waiting on the engine's phase gate would print its four-line
    // "NOT dressed" report a few times a second for the whole wait.
    enum class Outcome : uint8_t { NotAsked, Deferred, Failed, Applied };

    struct DressLatch {
        bool     applied = false;      // the ladder ran; do not run it again this load
        bool     verdict = false;      // ...and what it concluded, carried into later reports
        uint16_t slotResolved = kNotDressed;
        uint16_t userIndex = kNotDressed;
        Outcome  last = Outcome::NotAsked;
    };

    void ArmDressPass() {
        for (size_t i = 0; i < kSlots; ++i) latch_[i] = DressLatch{};
    }

    // The hand-asked re-arm's version: a slot the ladder already applied keeps its latch, so this
    // pass dresses the ones that are still undressed and leaves the dressed ones alone. Running the
    // ladder over a dressed player attaches a second profile document over the first and the engine
    // abandons the one it was holding, which is memory nothing gives back.
    void ArmDressPassKeepingApplied() {
        for (size_t i = 0; i < kSlots; ++i) {
            if (!latch_[i].applied) latch_[i] = DressLatch{};
        }
    }

    const DressLatch& LatchAt(size_t slot) const {
        static const DressLatch kNone{};
        return slot < kSlots ? latch_[slot] : kNone;
    }

    // One call, because the two facts are written together or not at all: a pass that applied the
    // ladder is also the pass whose outcome the next one compares against.
    void NoteDressPass(uint16_t userSlot, Outcome now, bool verdict, uint16_t slotResolved,
                       uint16_t userIndex) {
        if (userSlot >= kSlots) return;
        DressLatch& l = latch_[userSlot];
        l.last = now;
        if (now != Outcome::Applied) return;
        l.applied = true;
        l.verdict = verdict;
        l.slotResolved = slotResolved;
        l.userIndex = userIndex;
    }

private:
    uint16_t index_[kSlots];
    uint32_t mapped_;
    ResidueEntry residue_[kSlots];
    DressLatch latch_[kSlots];
};

// The shipped entry point: every recorded player, in manifest order.  One player's failure does
// not stop the roster -- the players after it are still dressed -- and the summary line says how
// many were not, so "N dressed == roster size" is a question one log line answers.  Returns
// dressed == players.
//
// ★ It also discharges the VACATE OBLIGATION.  Every slot in `residue ∪ this roster` leaves this
// call in a defined state: the slots this manifest NAMES are its own, and every residue slot it
// does not name is undressed here -- because a release that cannot happen (ReleaseReplayUser
// refuses on this build) becomes an overwrite that can.  Load is the place for it: it is the one
// moment both sets are known, and the writes land on a live game space with the same engine calls
// that dressed them.  The five vacate numbers are separate from `dressed`/`failed` in both the
// report and the log.
//
// ★ The claim is MANIFEST MEMBERSHIP, not "this pass wrote a field".  W2-B re-runs this call with
// the SAME manifest after an entity re-bind (playback_module.cpp, `entity_changed && g.loaded`);
// keyed on what the pass managed to write, a player whose step 1 refused on the second pass
// -- an entrant that still exists, so the slot still resolves -- would be vacated, stripping the
// name, loadout and colour of a player belonging to the replay that is playing right now.  Such a
// slot is `dressRefused`: counted, logged, left alone.
//
// `map` is cleared and refilled.  It carries every player whose ENTRANT step resolved, which is a
// wider set than `dressed`: see DressResult::indexResolved.  There is no mapping-free overload,
// because a caller that took one would compile, run, publish nothing, and look wired.
//
// ★ IT IS CALLED AGAIN while anything is deferred, a few times a second, until nothing is.  A slot
// whose ladder has already run is SKIPPED (RosterMapping::DressLatch), so "exactly one successful
// apply per slot per load" is a property of the code and not of how often the driver calls it.  A
// `Retry` pass logs only what changed; the load's first pass reports everything, as it always has.
bool DressRoster(IIdentity& id, ILog& log, const PlayerRecord* players, size_t count,
                 RosterReport& out, RosterMapping& map, DressPass pass = DressPass::First,
                 const DressEnv& env = DressEnv{});

// Hand every replay user this roster resolved back to the engine, then empty the mapping.
//
// The engine-side half is IIdentity::ReleaseReplayUser, which authors the SNSLobbyEntrantRemoved
// body the client's own EntrantRemovedCB consumes.  Whether that callback can be reached on this
// build is that function's answer to give, not this one's: `out` counts released and refused
// separately and the summary line says which, because "nothing was released" and "there was
// nothing to release" are two states and a single number cannot hold both.
struct ReleaseReport {
    uint32_t mapped;     // slots the roster had resolved
    uint32_t released;   // the engine accepted the release
    uint32_t refused;    // it did not -- each one names its refusal in the log
    uint32_t residue;    // ...and therefore still carry this replay's identity, for the next load
};
void ReleaseRoster(IIdentity& id, ILog& log, RosterMapping& map, ReleaseReport& out);

// ---- the leave feed ---------------------------------------------------------------------------
// ★ WHY THE RELEASE IS TWO CALLS AND NOT ONE.  ReleaseReplayUser drives the engine's own
// NSLobbyRemovingEntrant handler, so it MUST run on the engine thread with a live netGame in hand
// -- and Unload runs on neither.  So ReleaseRoster RECORDS which engine slots are to leave, and
// ServiceLeaveFeed, called from the same stage-3 tail the stat release rides, performs them.  It is
// the identical split statsfeed::RequestRelease / ServiceRelease already uses, and for the identical
// reason.
//
// ★ ORDER.  Everything that DRIVES a replay body -- synthetic physics owners, the disc pool, the
// HUD netvar ownership, the stat records -- is handed back by Unload before the queue is filled,
// and the stat release is serviced ahead of this one inside the tail.  No plugin write can therefore
// land on a body between the leave and the frame that destroys it.
//
// ★ BOUNDED.  Each slot gets kLeaveAttempts passes.  A slot the engine will not free is dropped
// from the queue with a named error rather than retried forever, because a queue that never empties
// would hold the next load's dress pass off the roster for the rest of the session.
struct LeaveReport {
    uint32_t queued;     // engine slots this unload asked to leave
    uint32_t left;       // the engine freed the slot: nothing in its user table names it back
    uint32_t refused;    // the engine did not, after kLeaveAttempts passes -- each one named
};

// Fills the queue from the mapping's residue.  Nothing is called on the engine here.
void RequestRosterLeave(const RosterMapping& map, LeaveReport& out);

// `playback.leave_on_unload`, read once per load. Off, Unload queues nothing and the next load
// adopts the entrants it left behind -- the behaviour of every build before the leave path.
void SetLeaveOnUnload(bool on);
bool LeaveOnUnload();

// `playback.leave_reverse_order` / `playback.leave_one_per_pass`, read once per load.  Pure
// ordering: the same slots leave through the same engine call, highest first instead of lowest, and
// at most one per pass instead of all in one tick.  They exist because the unload's engine free
// leaves exactly one pooled actor record un-re-keyed, always the same entry, and whether that
// follows the ORDER of the removals or the entry itself has never been separated.
void SetLeaveOrder(bool reverse, bool onePerPass);
bool LeaveReverseOrder();
bool LeaveOnePerPass();

// True while any slot is still waiting to leave. The next load's dress pass defers on this: dressing
// into a slot whose entrant is mid-teardown would adopt something about to be destroyed.
bool RosterLeavePending();

// One pass. Drops each slot that left from the residue, so the next load admits a fresh entrant for
// it; a slot that refused KEEPS its residue and is adopted by the next load exactly as before, which
// is the fallback that stops a refused leave turning back into the duplicated roster.
void ServiceRosterLeave(IIdentity& id, ILog& log, RosterMapping& map);

// Drops the queue without calling anything. The exit path: see the comment at its one caller.
void ForgetRosterLeave();

const LeaveReport& RosterLeaveTotals();

// ---- the vacate -------------------------------------------------------------------------------
// Undress one residue slot: re-write the identity fields this session wrote to the state the engine
// itself holds for an entrant nobody is using.  Every value below is the engine's own, not a
// convenient zero:
//
//   name        the empty string -- IDENT.SLOTNAME_GET returns "" for a slot at or past the
//               vector's bound (identity_access.cpp SlotNameField), i.e. "" is what the engine
//               answers for a slot it does not have a name for.
//   profile     `displayname` set to the empty string.  ★ The engine's CJson exposes typed
//               SETTERS and no remove, so the document cannot be emptied; the leaf UserDisplayName
//               reads is driven empty and the leaves it does not read (stats, level) keep the
//               previous replay's values.  That is stated in the log line rather than passed over:
//               what the client RENDERS as this player is vacated, what it stores behind it is not.
//   team        IDENT.ALIGN_TEAM_LIMIT -- the first team id the engine's own alignment table does
//               not cover (GAME.TEAM_OFF records 2 = SPECTATOR as a value the shipping client
//               writes), so CR15NetGame::Alignment answers "none" for it by the engine's rule and
//               not only by our override.
//   loadout     twenty item ids of 0 and both numbers 0.  Zero is the engine's empty item id: it
//               is what ReadAppliedItems returns where nothing is applied, which is exactly what
//               DiffItems scores as `absent`.
//   alignment   0xFFFF -- what CR15NetGame::Alignment (IDENT.NETGAME_ALIGNMENT, whose filed
//               prologue loads FF FF into its default) returns for an entrant with no side.
//
// Three outcomes, because they are three different facts and a bool would collapse the one that
// matters into the one that does not.
enum class VacateOutcome : uint8_t {
    Vacated,     // every field this session wrote is back to the engine's no-user state
    StaleSlot,   // the entrant no longer exists -- there is nothing left wearing anything
    Refused,     // a write refused; the slot still carries the previous replay's identity
};

VacateOutcome VacateSlot(IIdentity& id, ILog& log, uint16_t userSlot,
                         const RosterMapping::ResidueEntry& e);

}  // namespace identity
}  // namespace replay
