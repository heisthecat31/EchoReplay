#include "playback/core/apply.h"

#include <stdio.h>
#include <string.h>

#include <chrono>

#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"
#include "bootstrap/tickcost.h"
#include "engine/gamestate_facts.h"
#include "playback/core/sample.h"

namespace replay {
namespace playback {

namespace {

// Refused > Applied > PresentNotApplied > NoRecords. A rank, not a chain of special cases: the
// merge below has to be total over the states, and the arm that used to be `default:` is what let
// two different facts share one sentence.
uint8_t FidelityRank(NetVarFidelityState s) {
    switch (s) {
        case NetVarFidelityState::NoRecords: return 0;
        case NetVarFidelityState::PresentNotApplied: return 1;
        case NetVarFidelityState::Applied: return 2;
        case NetVarFidelityState::Refused: return 3;
    }
    return 3;   // an unranked state must dominate, never be quietly discarded
}

}  // namespace

const char* NetVarFidelityText(NetVarFidelityState s) {
    switch (s) {
        case NetVarFidelityState::NoRecords:
            return "netvars: no netvar records in this capture (nothing to apply)";
        case NetVarFidelityState::PresentNotApplied:
            return "netvars: records present, none due at this playhead";
        case NetVarFidelityState::Applied: return "netvars: applied";
        case NetVarFidelityState::Refused:
            return "netvars: STORED, NOT APPLIED (no apply path on this build)";
    }
    // No `default:` arm. A state added without a sentence of its own has to be visible here rather
    // than rendered as whichever of the four the default happened to name.
    return "netvars: state not stated";
}

void ApplyStats::Add(const ApplyStats& o) {
    bodies_installed += o.bodies_installed;
    bodies_absent += o.bodies_absent;
    bodies_unhandled += o.bodies_unhandled;
    bodies_locally_controlled += o.bodies_locally_controlled;
    bodies_install_failed += o.bodies_install_failed;
    players_published += o.players_published;
    players_absent += o.players_absent;
    players_skipped_asseen += o.players_skipped_asseen;
    players_unhandled += o.players_unhandled;
    players_unmapped += o.players_unmapped;
    players_refused_owner += o.players_refused_owner;
    netvars_attempted += o.netvars_attempted;
    netvars_applied += o.netvars_applied;
    netvars_refused += o.netvars_refused;
    netvars_unchanged += o.netvars_unchanged;
    disc_sampled = disc_sampled || o.disc_sampled;
    disc_owner_stamps += o.disc_owner_stamps;
    disc_owner_unmapped += o.disc_owner_unmapped;
    disc_owner_fallbacks += o.disc_owner_fallbacks;
    disc_owner_held_over += o.disc_owner_held_over;
    disc_owner_refused += o.disc_owner_refused;
    disc_touch_writes += o.disc_touch_writes;
    disc_touch_refused += o.disc_touch_refused;
    // The three below are a LEVEL, not a total: a summed span holds the most recent one published.
    if (o.disc_owner_stamps) {
        disc_owner_value = o.disc_owner_value;
        disc_owner_recorded_slot = o.disc_owner_recorded_slot;
        disc_owner_engine_slot = o.disc_owner_engine_slot;
    }
    if (o.netvars_present > netvars_present) {
        netvars_present = o.netvars_present;
    }
    netvar_apply_available = netvar_apply_available && o.netvar_apply_available;
    // â˜… Ranked. A summed report must say "something was refused" if anything was, must not let a
    // quiet frame pull an Applied back down, and must not report the span as carrying no records
    // because the last frame's window happened to hold none.
    if (FidelityRank(o.netvar_state) > FidelityRank(netvar_state)) {
        netvar_state = o.netvar_state;
    }
}

void StateApplier::Bind(IEntity* entity, IPosePublisher* pose, INetVar* netvar, OwnerGuard* owners) {
    entity_ = entity;
    pose_ = pose;
    netvar_ = netvar;
    owners_ = owners;
    verify_mismatches_ = 0;
}

bool StateApplier::Rebind(IEntity* entity, IPosePublisher* pose, INetVar* netvar) {
    if (entity == entity_ && pose == pose_ && netvar == netvar_) {
        return false;
    }
    entity_ = entity;
    pose_ = pose;
    netvar_ = netvar;
    return true;
}

// The disc's possession, published as a LEVEL into the one dword three engine predicates read.
//
// CR15NetTouchInteractCS::Update (sub_140D70F40) asks sub_140CEDEB0 who holds the disc, diffs the
// answer against its own latch at touchCI+52 and fires OnNetTouchGrab / OnNetTouchRelease /
// OnNetTouchOwnershipTransfer itself. So nothing here synthesises an edge.
//
// WHEN the engine samples that level is the engine's business and not knowable from here:
// sub_140D70F40 takes no delta time and has no call site -- it is reached through a registered
// vtable slot -- so the update stage decides, and the plugin's pause bit (CClock+0x50 bit 2) only
// makes the heartbeat report a zero delta. What this file guarantees instead is the property that
// makes any sampling cadence correct: the level is the truth for the current playhead, it is
// written only when the recording says the holder changed, and it never passes through an
// intermediate value. Whenever the engine next looks -- next frame, or the first frame after a
// rewind or a paused scrub ends -- it sees one transition to the right holder, never a burst of
// stale edges. Possession VISUALS may therefore lag by a frame while paused or rewinding; they
// cannot be wrong once running resumes.
//
// An owner the roster never mapped -- a spectator, or a player this session did not admit -- is
// published as UNOWNED rather than as a guess. Publishing somebody else's id would hand the disc
// to the wrong avatar with nothing reporting it.
void StateApplier::PublishDiscOwner(Window& w, BodyTrack& tr, uint32_t recorded,
                                    int64_t t_us, ApplyStats& st) {
    SeedTrackFromDiscLevel(tr);
    // The touch pair is a function of the playhead, not of the owner CHANGING, so it is asked on
    // every publish -- including the one the owner dword needs no write for, which is the tick a
    // seek back over a round boundary lands on.
    PublishDiscTouch(w, tr, recorded, t_us, st);
    // A recorded "nobody" never replaces a real holder. The host's UpdateTrackingStatsArena
    // (sub_1401C5130) treats every dword except dword_141FFCEA0 as a holder, so the out-of-table
    // value reads as a player of team 0xFFFF: holder -> 0xFFFE0010 at a goal is a cross-team change
    // and raises nevt_*_intercepted / _disc_stolen / _gains_possession, and 0xFFFE0010 -> the next
    // grab raises another. A live server writes the engine's own none there, which no branch treats
    // as a holder; we cannot (it makes the disc locally controlled), so the last real holder stays
    // named -- the state every thrown disc is already in, since the recorded holder is sticky
    // through flight and only goes none at a goal or a round start. The node's held bit still
    // follows the recording, so the touch system reads nobody (ENG-GOAL-SFX.md).
    if (recorded == kBodyOwnerNone && tr.owner_level_published && tr.owner_level_real) {
        if (!tr.owner_level_held_over) {
            ++st.disc_owner_held_over;
            IEntity::HolderReading hr;
            if (entity_->EngineHolderOf(tr.handle, hr) && hr.read) st.disc_holder = hr;
            NoteOwnerChange(recorded, t_us, "the last real holder stays named over a recorded none");
        }
        tr.owner_level_held_over = true;
        return;
    }
    tr.owner_level_held_over = false;
    if (tr.owner_level_published && recorded == tr.owner_level_slot) {
        NoteOwnerChange(recorded, t_us, "the engine's dword already names this holder");
        return;
    }

    uint16_t engine_slot = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    uint32_t owner = MapRecordedOwner(w, recorded, engine_slot);
    if (recorded != kBodyOwnerNone && recorded < 0xFFFFu && engine_slot == 0xFFFFu)  // facts-ok: NETVAR.SLOT_NONE the u16 range limit, not the engine no-slot field
        ++st.disc_owner_unmapped;

    if (!entity_->SetRecordedOwner(tr.handle, owner)) {
        ++st.disc_owner_refused;
        return;
    }
    // The stamp IS a claim on the engine's dword, so the release half has to know about it even
    // when the value we wrote was the synthetic one.
    tr.synthetic_owner_set = true;

    // ★ The fallback the RE asked for, measured rather than assumed. A real owner is a REMOTE user
    // and must leave the body not-locally-controlled; if this build resolves the entrant into the
    // local band instead, the engine would simulate the disc and overwrite the replay. Put the
    // out-of-table value back and count it: a disc with no grab events still beats a wrong replay.
    if (owner != entity_->SyntheticOwnerValue() && entity_->IsLocallyControlled(tr.handle)) {
        entity_->SetRecordedOwner(tr.handle, entity_->SyntheticOwnerValue());
        ++st.disc_owner_fallbacks;
        owner = entity_->SyntheticOwnerValue();
        engine_slot = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }

    tr.owner_level_slot = recorded;
    tr.owner_level_published = true;
    tr.owner_level_real = engine_slot != 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    disc_level_.handle = tr.handle;
    disc_level_.value = owner;
    disc_level_.recorded_slot = recorded;
    disc_level_.engine_slot = engine_slot;
    disc_level_.real = tr.owner_level_real;
    disc_level_.held = true;
    ++st.disc_owner_stamps;
    st.disc_owner_value = owner;
    st.disc_owner_recorded_slot = recorded;
    st.disc_owner_engine_slot = engine_slot;
    char rec[16] = "none";
    char eng[24] = "none";
    if (recorded != kBodyOwnerNone) snprintf(rec, sizeof rec, "%u", (unsigned)recorded);
    if (engine_slot != 0xFFFFu)  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        snprintf(eng, sizeof eng, "%u/%u", (unsigned)engine_slot, (unsigned)(owner >> 16));
    // ★ THE ONE READ-BACK, and it is the engine's OWN answer, not ours re-read. Our stamp landing
    // proves the store; it says nothing about whether sub_140CEDEB0 hands that dword to the touch
    // system or answers "nobody". Four launch unknowns -- the slot/index namespace, the node's
    // 0x200, the generation, and which arm -- looked identical to the owner before this line, and
    // each of them now names itself. Once per possession change, six per reference capture.
    char seen[128] = "not read";
    IEntity::HolderReading hr;
    if (entity_->EngineHolderOf(tr.handle, hr) && hr.read) {
        st.disc_holder = hr;
        snprintf(seen, sizeof seen,
                 "%s (%s arm; live slot %u in band %u..%u, gen %u %s, held bit %u, node %s%04X)"
                 " -- %s",
                 hr.held ? "HOLDER" : "nobody",
                 hr.local_arm ? "local" : (hr.remote_arm ? "remote" : "no"),
                 (unsigned)hr.live_slot, (unsigned)hr.band_lo, (unsigned)hr.band_hi,
                 (unsigned)hr.live_gen, hr.gen_ok ? "ok" : "MISMATCH", (unsigned)hr.held_bit,
                 hr.node_read ? "" : "unread:", (unsigned)hr.node_state,
                 (hr.held == (engine_slot != 0xFFFFu)) ? "agrees" : "DISAGREES");  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    owner_logged_slot_ = recorded;
    owner_logged_valid_ = true;
    log::Direct(LogLevel::Info, "playback",
                "disc possession at %.3f s: recorded slot %s -> engine slot/gen %s, "
                "PhysCI+0x1C = %08X; engine reads %s", (double)t_us * 1e-6, rec, eng,
                (unsigned)owner, seen);
}

// The engine has taken the disc back. Restore the level that is already published rather than
// deciding a new one: `disc_level_` is the record that our value is still supposed to be out there,
// and it survives the window rebuilds that throw the track away. With nothing published yet the
// out-of-table value is the right claim -- it is what an unowned disc is published as anyway.
//
// No edge is synthesised. The value written is the one the engine's own holder gate last agreed
// with, and SetRecordedOwner does not store when the dword already holds it, so a disc the engine
// has not touched costs one read. OwnerSave keeps the FIRST original, so repeated claims never
// overwrite the engine's own owner: teardown still restores it.
void StateApplier::ReclaimDiscOwner(BodyTrack& tr, ApplyStats& st) {
    SeedTrackFromDiscLevel(tr);
    IEntity::HolderReading hr;
    if (entity_->EngineHolderOf(tr.handle, hr) && hr.read) {
        st.disc_owner_seen = hr.owner;
        st.disc_owner_seen_valid = true;
    }
    const bool ours = disc_level_.held && disc_level_.handle.v == tr.handle.v &&
                      disc_level_.handle.pool == tr.handle.pool;
    if (!ours) {
        // No level is out there yet, so the claim IS the level: the out-of-table value, which is
        // also what an unowned disc is published as. The remote-replay slot surface resolves its
        // CI by pool bounds only (DIAG-POSE-DECOUPLE 4b).
        const bool first = !tr.synthetic_owner_set;
        if (tr.slot_remote) {
            entity_->SetSyntheticOwnerDriven(tr.handle);
        } else {
            entity_->SetSyntheticOwner(tr.handle);
        }
        tr.synthetic_owner_set = true;
        if (first) ++st.bodies_owner_claimed; else ++st.disc_owner_reclaims;
        return;
    }
    const uint32_t level = disc_level_.value;
    if (!entity_->SetRecordedOwner(tr.handle, level)) {
        ++st.disc_owner_refused;
        return;
    }
    tr.synthetic_owner_set = true;
    ++st.disc_owner_reclaims;
    // The engine has been writing this CI, so what we last published is no longer what is there.
    touch_published_ = false;
    // The same measured fallback PublishDiscOwner makes, for the same reason: a real owner that
    // leaves the body locally controlled would have the engine simulate the disc over the replay.
    // The out-of-table value cannot -- IsLocallyControlled refuses any owner whose low word is at
    // or past the user-slot table size before it touches memory (W1-E HP1).
    if (level != entity_->SyntheticOwnerValue() && entity_->IsLocallyControlled(tr.handle)) {
        if (entity_->SetRecordedOwner(tr.handle, entity_->SyntheticOwnerValue())) {
            disc_level_.value = entity_->SyntheticOwnerValue();
            disc_level_.engine_slot = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
            disc_level_.real = false;
            tr.owner_level_real = false;
            ++st.disc_owner_fallbacks;
        }
    }
}

uint32_t StateApplier::MapRecordedOwner(Window& w, uint32_t recorded, uint16_t& engine_slot) const {
    engine_slot = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    uint32_t owner = entity_->SyntheticOwnerValue();
    if (recorded == kBodyOwnerNone || recorded >= 0xFFFFu) return owner;  // facts-ok: NETVAR.SLOT_NONE the u16 range limit, not the engine no-slot field
    for (size_t p = 0; p < w.PlayerCount(); ++p) {
        const PlayerTrack& pt = w.PlayerAt(p);
        if (!pt.user_id_valid || pt.user_slot != (uint16_t)recorded) continue;
        uint32_t id = 0;
        if (entity_->UserIdOf(UserIndex((uint16_t)pt.user_id), id)) {
            owner = id;
            engine_slot = (uint16_t)pt.user_id;
        }
        break;
    }
    return owner;
}

// lastOwner(T): the newest recorded holder at or before T that is still inside T's ROUND.
//
// ★ WHY A ROUND BOUND AT ALL. LastOwner is what the engine's own "if not held, use LastOwner"
// idiom falls back to, so it is what says "this disc has been touched". A real client gets it
// reset when the round is set up again, which is what brings the untouched ring back at a restart.
// The recording carries no reset event -- the whole S_NETVAR stream has no key that moves at the
// first grab -- so the round is taken from the recording's own `game_state`: two samples belong to
// the same round exactly when the resolved `game_state` at their times is the same value. That is
// a level, so it survives a seek, a reverse and a pause with no edge to miss.
//
// The memo is only a memo. It is discarded whenever the playhead has moved back past the sample it
// was taken at, or `game_state` has changed since -- which are exactly the two cases the ring has
// to reappear for -- and the value is then re-derived by walking this track's own samples back.
uint32_t StateApplier::LastOwnerAt(Window& w, const BodyTrack& tr, uint32_t recorded,
                                   int64_t t_us) {
    uint64_t gs = 0;
    const bool gs_known = w.NetVars().ValueAt(gamestate::facts::kVarGameState.symbol, t_us, gs);

    // A holder at T IS the last owner at T, and it is also the newest one the memo can hold.
    if (recorded != kBodyOwnerNone) {
        uint16_t slot = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        last_owner_.owner = MapRecordedOwner(w, recorded, slot);
        last_owner_.recorded_slot = recorded;
        last_owner_.at_us = t_us;
        last_owner_.game_state = gs;
        last_owner_.state_known = gs_known;
        last_owner_.valid = true;
        return last_owner_.owner;
    }

    if (last_owner_.valid && t_us >= last_owner_.at_us &&
        last_owner_.state_known == gs_known && last_owner_.game_state == gs) {
        return last_owner_.owner;
    }

    const BodyState* frames = w.BodyFrames(tr);
    const uint64_t* times = w.BodyTimes(tr);
    uint32_t owner = entity_->SyntheticOwnerValue();
    int64_t at = t_us;
    if (frames && times && tr.count) {
        size_t i = tr.count;
        while (i > 0 && (int64_t)times[i - 1] > t_us) --i;
        for (; i > 0; --i) {
            const size_t k = i - 1;
            uint64_t gs_k = 0;
            const bool known_k =
                w.NetVars().ValueAtBySearch(gamestate::facts::kVarGameState.symbol,
                                            (int64_t)times[k], gs_k);
            if (known_k != gs_known || gs_k != gs) break;   // an earlier round: not this ring's history
            if (frames[k].owner_slot == kBodyOwnerNone) continue;
            uint16_t slot = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
            owner = MapRecordedOwner(w, frames[k].owner_slot, slot);
            at = (int64_t)times[k];
            break;
        }
    }
    last_owner_.valid = true;
    last_owner_.owner = owner;
    last_owner_.at_us = at;
    last_owner_.game_state = gs;
    last_owner_.state_known = gs_known;
    return owner;
}

// The disc's touch-state pair, written as LEVELS in the same place and the same order as the
// possession stamp -- after the history install, so the node and the CI never disagree. Two pure
// functions of the playhead: held(T) is "the recording says somebody holds it at T", and
// lastOwner(T) is above. Pause, reverse and scrub are correct by construction, and the value never
// passes through an intermediate: a hand-over writes one transition, as POSSESSION-2 requires.
void StateApplier::PublishDiscTouch(Window& w, BodyTrack& tr, uint32_t recorded, int64_t t_us,
                                    ApplyStats& st) {
    if (entity_ == nullptr) return;
    if (!livesafety::Allowed(livesafety::Feature::DiscTouchState)) return;
    const bool held = recorded != kBodyOwnerNone;
    // The node bit is reconciled on EVERY publish rather than on the change, because the engine
    // reads the newest node and this plugin pushes a new one each tick: a level written once would
    // be gone by the next frame. It is a function of the playhead alone, so a seek, a paused drag
    // or a reverse lands on the right value without an edge being replayed.
    entity_->SetRecordedNodeHeld(tr.handle, held);
    // A held disc's last owner is its current one: the recorded level answers both, and asking the
    // memo for it here is what keeps the two fields consistent within the tick.
    const uint32_t last = LastOwnerAt(w, tr, recorded, t_us);
    if (touch_published_ && touch_held_ == held && touch_last_owner_ == last) return;
    if (!entity_->SetDiscTouchState(tr.handle, held, last)) {
        ++st.disc_touch_refused;
        return;
    }
    touch_published_ = true;
    touch_held_ = held;
    touch_last_owner_ = last;
    ++st.disc_touch_writes;
    log::Direct(LogLevel::Info, "playback",
                "disc touch-state at %.3f s: PhysCI+0x06 bit1 = %d (held), PhysCI+0x20 = %08X "
                "(last owner %s) => disc reads %s", (double)t_us * 1e-6, (int)held, (unsigned)last,
                last == entity_->SyntheticOwnerValue() ? "none" : "a mapped live user",
                (held || last != entity_->SyntheticOwnerValue()) ? "TOUCHED" : "UNCLAIMED");
}

// The short form, for a change that produced no write. Same dedupe as the rich line above, so a
// hand-over is one line whichever path it took and a steady holder is silent.
void StateApplier::NoteOwnerChange(uint32_t recorded, int64_t t_us, const char* why) {
    if (owner_logged_valid_ && recorded == owner_logged_slot_) return;
    owner_logged_slot_ = recorded;
    owner_logged_valid_ = true;
    char rec[16] = "none";
    if (recorded != kBodyOwnerNone) snprintf(rec, sizeof rec, "%u", (unsigned)recorded);
    log::Direct(LogLevel::Info, "playback",
                "disc possession at %.3f s: recorded slot %s, no write (%s)", (double)t_us * 1e-6,
                rec, why);
}

// A window rebuild throws the track away while our level is still on the engine's dword. Without
// this the fresh track reads as "nothing published", republishes the same value, and -- worse --
// a release scoped to tracks would find nothing to hand back.
void StateApplier::SeedTrackFromDiscLevel(BodyTrack& tr) const {
    if (tr.owner_level_published || !disc_level_.held) return;
    if (tr.handle.v != disc_level_.handle.v || tr.handle.pool != disc_level_.handle.pool) return;
    tr.owner_level_slot = disc_level_.recorded_slot;
    tr.owner_level_real = disc_level_.real;
    tr.owner_level_held_over = false;
    tr.owner_level_published = true;
    tr.synthetic_owner_set = true;
}

size_t StateApplier::ReconcileDiscLevel(Window& w) {
    if (entity_ == nullptr || !disc_level_.held) return 0;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        BodyTrack& tr = w.BodyAt(i);
        if (!tr.claim_owner || !tr.handle_valid) continue;
        if (tr.handle.v == disc_level_.handle.v && tr.handle.pool == disc_level_.handle.pool) {
            SeedTrackFromDiscLevel(tr);
            return 0;
        }
    }
    // ClearSyntheticOwner restores the owner dword AND, from the same save, the touch pair, so the
    // cache of what we published has to go with it or the next publish would skip the re-assert.
    entity_->ClearSyntheticOwner(disc_level_.handle);
    disc_level_ = DiscLevel();
    ForgetDiscTouch();
    return 1;
}

void StateApplier::ForgetDiscLevel() {
    if (entity_ != nullptr && disc_level_.held) entity_->ForgetSyntheticOwner(disc_level_.handle);
    disc_level_ = DiscLevel();
    ForgetDiscTouch();
}

void StateApplier::ForgetDiscTouch() {
    touch_published_ = false;
    touch_held_ = false;
    touch_last_owner_ = 0;
    last_owner_ = LastOwnerMemo();
}

// `recorded->published`, with the engine's invalid-id sentinel spelled rather than printed as a
// dword a reader has to recognise.
static void FormatGrabTarget(uint32_t recorded, uint32_t published, char* out, size_t cap) {
    char rec[12] = "none";
    char pub[12] = "none";
    if (recorded != kHoldNobody) snprintf(rec, sizeof rec, "%08X", (unsigned)recorded);
    if (published != kHoldNobody) snprintf(pub, sizeof pub, "%08X", (unsigned)published);
    snprintf(out, cap, "%s->%s", rec, pub);
}

// ★ THE GRAB INPUT IS WHAT MAKES A GRAB, AND THE HOLD ID IS WHO IT LANDED ON.
//
// The engine re-runs its own per-hand grab state machine from the replicated analog trigger blend
// (sub_140C20DE0 reads PlayerCS+0x1274+4*hand); when it decides to attach it calls AttachHand with
// playSound = 1, which is the ONLY site in the image that posts event_sound_hand_grab_left/right.
// The recorder has always captured trigL/trigR and the publisher has always stopped short of
// writing them, so a replayed player's grab input was a constant 0: no sound, no hand attach, for
// geometry and for players alike. These are level values, not edges -- the engine derives its own
// edges -- so a scrub, a pause or a reverse cannot strand a half-fired grab.
//
// The hold ids are the second half. They name the grabbed player as an SR15NetUserId in the
// RECORDING's slot numbering, and the live session hands out different slots (live 1 <- rec 0,
// 2 <- rec 1, 3 <- rec 3 in the owner's capture), so publishing them verbatim points a hand at
// whoever happens to hold that live slot. `0xFFFFFFFF` is the engine's own invalid-id sentinel
// (dword_141FFCEA0) and must pass through untouched: sub_140CCE580 requires it on both sides of a
// frame to raise the geo-attach delegates at all.
void StateApplier::PublishGrip(Window& w, const PlayerTrack& tr, const ApplyOptions& opt,
                               PlayerState& ps) {
    if (!opt.grip_inputs) {
        // What every build before this one published: the field the engine gates on stays 0, and
        // the hold ids stay in recorded numbering rather than being pointed at a live player.
        ps.trigL = 0.0f;
        ps.trigR = 0.0f;
        return;
    }
    const uint32_t beforeL = ps.holdL;
    const uint32_t beforeR = ps.holdR;
    ps.holdL = RemapHoldTarget(w, ps.holdL);
    ps.holdR = RemapHoldTarget(w, ps.holdR);

    GripReport& seen = grip_[tr.user_slot < kGripTracks ? tr.user_slot : kGripTracks - 1];
    const bool edge = !seen.seen || seen.recL != beforeL || seen.recR != beforeR ||
                      seen.pubL != ps.holdL || seen.pubR != ps.holdR;
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    if (!edge && seen.reported_ms != 0 && now - seen.reported_ms < 1000u) {  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
        return;
    }
    // PERF-1: the periodic repeat is one line per player per second of hand-grab state nobody reads;
    // the EDGE line is the one that says something happened. Below Info by default, so the two
    // FormatGrabTarget calls and the 200-byte format below do not run at all in a steady launch.
    const LogLevel gripLevel = edge ? LogLevel::Info : LogLevel::Trace;
    seen.seen = true;
    seen.recL = beforeL;
    seen.recR = beforeR;
    seen.pubL = ps.holdL;
    seen.pubR = ps.holdR;
    seen.reported_ms = now;
    // `holdL`/`holdR` read as disc possession for a whole launch and cost it its conclusion. These
    // name the PLAYER OR OBJECT this hand is attached to; the disc's holder is published on the
    // `disc possession` line and nowhere else.
    if (gripLevel < log::Level()) {
        return;
    }
    char l[24], r[24];
    FormatGrabTarget(beforeL, ps.holdL, l, sizeof l);
    FormatGrabTarget(beforeR, ps.holdR, r, sizeof r);
    log::Direct(gripLevel, "playback",
                "grip: rec slot %u -> live %u: trigL=%.3f trigR=%.3f grabTargetL %s grabTargetR %s"
                "%s (hand-grab targets, not disc possession)",
                (unsigned)tr.user_slot, (unsigned)tr.user_id, (double)ps.trigL, (double)ps.trigR,
                l, r, edge ? " EDGE" : "");
}

// The recorded hold dword carries a recorded user slot in its low word. The same recorded->live
// resolution the body tracks and possession already use is the track table itself, so this is a
// scan of it rather than a second map that could disagree with the first. A target this window
// never resolved -- a spectator, or a player who had left by the time the capture reached here --
// becomes the sentinel: "not holding" is the honest answer, and pointing the hand at a live slot
// we did not resolve is the bug this exists to prevent.
uint32_t StateApplier::RemapHoldTarget(Window& w, uint32_t recorded) const {
    if (recorded == kHoldNobody) {
        return recorded;   // the engine's invalid id, and the geo delegates test it exactly
    }
    const uint16_t recSlot = (uint16_t)(recorded & 0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    for (size_t i = 0; i < w.PlayerCount(); ++i) {
        const PlayerTrack& other = w.PlayerAt(i);
        if (!other.user_id_valid || other.user_slot != recSlot) {
            continue;
        }
        uint32_t live = 0;
        // The generation comes off CR15NetGame's own per-slot table, never from the recording:
        // the recorded genidx is 0 in every frame of the owner's capture and a live handle need
        // not be.
        if (entity_ != nullptr &&
            entity_->UserIdOf(UserIndex((uint16_t)other.user_id), live)) {
            return live;
        }
        break;
    }
    return kHoldNobody;
}

bool SampleBodyFor(Window& w, BodyTrack& tr, SeriesCursor& cursor, int64_t t_us, BodyState& out,
                   bool reversed, bool hermite, HermiteTally* hermite_tally, const SmoothFrame* smooth) {
    const float weight = smooth != nullptr ? smooth->weight : 0.0f;
    if (!(weight > 0.0f)) {
        return SampleBodyAt(w.BodyFrames(tr), w.BodyTimes(tr), tr.count, cursor, t_us, out, reversed,
                            hermite, hermite_tally);
    }
    if (weight >= 1.0f) {
        return SampleBodySmoothAt(w, tr, cursor, t_us, out, reversed, hermite, smooth->tally);
    }
    BodyState raw, sm;
    memset(&raw, 0, sizeof raw);
    memset(&sm, 0, sizeof sm);
    if (!SampleBodyAt(w.BodyFrames(tr), w.BodyTimes(tr), tr.count, cursor, t_us, raw, reversed,
                      hermite, hermite_tally) ||
        !SampleBodySmoothAt(w, tr, cursor, t_us, sm, reversed, hermite, smooth->tally)) {
        return false;
    }
    BlendBody(raw, sm, weight, out);
    return true;
}

bool SamplePlayerFor(Window& w, PlayerTrack& tr, SeriesCursor& cursor, int64_t t_us, PlayerState& out,
                     bool reversed, const SmoothFrame* smooth) {
    const float weight = smooth != nullptr ? smooth->weight : 0.0f;
    if (!(weight > 0.0f)) {
        return SamplePlayerAt(w.PlayerFrames(tr), w.PlayerTimes(tr), tr.count, cursor, t_us, out,
                              reversed);
    }
    if (weight >= 1.0f) {
        return SamplePlayerSmoothAt(w, tr, cursor, t_us, out, reversed, smooth->tally);
    }
    PlayerState raw, sm;
    memset(&raw, 0, sizeof raw);
    memset(&sm, 0, sizeof sm);
    if (!SamplePlayerAt(w.PlayerFrames(tr), w.PlayerTimes(tr), tr.count, cursor, t_us, raw, reversed) ||
        !SamplePlayerSmoothAt(w, tr, cursor, t_us, sm, reversed, smooth->tally)) {
        return false;
    }
    BlendPlayer(raw, sm, weight, out);
    return true;
}

ApplyStats StateApplier::Apply(Window& w, int64_t t_us, const ApplyOptions& opt) {
    ApplyStats st;
    if (!entity_) {
        return st;
    }

    // ---- bodies ------------------------------------------------------------------------------
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        BodyTrack& tr = w.BodyAt(i);
        if (!tr.handle_valid) {
            ++st.bodies_unhandled;
            continue;
        }

        BodyState s;
        memset(&s, 0, sizeof s);
        if (!SampleBodyFor(w, tr, tr.cursor, t_us, s, opt.reversed, opt.hermite, opt.hermite_tally,
                           opt.smooth)) {
            ++st.bodies_absent;   // E-P4: not yet joined at this playhead. Absent, not frozen.
            if (tr.claim_owner) st.disc_drive = DiscDrive::Absent;
            continue;
        }
        if (tr.claim_owner) {
            st.disc_sampled = true;
        }

        // Claimed, sampled -- and deliberately not installed. See ApplyOptions::hold_disc_pose.
        // DESIGN-HOST-SYNC P3: decided BEFORE the reclaim. While the host parks the disc it alone
        // owns PhysCI+0x1C; a reclaim or a published level here is the FFFE0010/FFFFFFFF flap
        // (LIVE-TRUTH 3.2). The first tick after the hold drops re-asserts the recorded level.
        if (tr.claim_owner && opt.hold_disc_pose) {
            st.disc_drive = DiscDrive::HostHeld;
            continue;
        }

        // â˜… INV-OWN1's OBSERVABLE, asked of the engine rather than derived from the id we think we
        // wrote. If this is true the engine will simulate the body and overwrite whatever we install,
        // so installing anyway would produce a replay that is wrong for a reason nothing reports.
        if (entity_->IsLocallyControlled(tr.handle)) {
            // A body bound through the NET-ACTOR table must never be locally controlled here: the
            // engine would simulate it and overwrite what we install, so installing anyway produces
            // a wrong replay for a reason nothing reports. That is still INV-OWN1's observable.
            // The disc is the third case: bound by the component id the live frisbee vouched for,
            // simulated by this host, and claimed the same way (full identity gate, its CI is a
            // genuine level component).
            if (!tr.bound_by_slot && !tr.claim_owner) {
                ++st.bodies_locally_controlled;
                continue;
            }
            // A body bound through a USER SLOT is the recorder's own pawn, and the viewer's own pawn
            // is locally simulated on THIS machine by construction -- same body, same role, one
            // session later. Refusing it is what left the viewer frozen while everyone else
            // replayed. W1-E's SetSyntheticOwner writes ENTITY.OWNER_SYNTHETIC into PHYS.CI_OWNER_OFF
            // (W2-B spike F1) -- the engine's own "this body is driven from outside" switch, and the
            // only write we make; there is no raw transform poke on any path here. Once per track,
            // per the spike's line 647, not once per frame.
            // ★ The disc's claim is RE-ASSERTED, never latched. The engine's own touch system
            // (CR15NetTouchInteractCS::Update) reacts to the possession level we publish by
            // transferring the body to this host -- which makes IsLocallyControlled true and frees
            // the history nodes we installed. A claim made once per track therefore left the disc
            // engine-simulated, and so parked at arena centre, for the rest of the load: that is
            // DISC-FLIGHT's `drive=locallyControlled` / `read=badSlot` state. Putting the level we
            // already published back is not a hand-over -- it is the same value the holder gate
            // last agreed with -- so possession semantics are untouched.
            if (tr.claim_owner) {
                ReclaimDiscOwner(tr, st);
            } else if (!tr.synthetic_owner_set) {
                // The remote-replay slot surface resolves its CI by pool bounds only, so the stamp
                // lands on the element the engine drives without the stricter-than-engine identity
                // proof; every other body keeps the full gate (DIAG-POSE-DECOUPLE 4b).
                if (tr.slot_remote) {
                    entity_->SetSyntheticOwnerDriven(tr.handle);
                } else {
                    entity_->SetSyntheticOwner(tr.handle);
                }
                tr.synthetic_owner_set = true;
                ++st.bodies_owner_claimed;
            }
            if (entity_->IsLocallyControlled(tr.handle)) {
                // The claim did not take on this build. Do NOT install over a body the engine is
                // still simulating: that is the same wrong replay, one arm further along.
                ++st.bodies_locally_controlled;
                if (tr.claim_owner) st.disc_drive = DiscDrive::LocallyControlled;
                continue;
            }
        }

        // The one-node ring. Time is the playhead itself: the node we install IS the state at t, so
        // the engine has nothing left to interpolate and no sample time to get wrong.
        const uint64_t node_t = t_us < 0 ? 0u : (uint64_t)t_us;
        InstallReading ir;
        // The recorded remote entrant drives its already-present phys CI by pool bounds only: the
        // engine's UPDATE_TARGET_STATE reads the same element by pool index, and demanding it also
        // carry ci[+0].entityId == handle is a proof the engine never makes. This decouples the
        // pose-drive from the model instantiation the entrant is still waiting on (DIAG-POSE-DECOUPLE).
        const bool installed = tr.slot_remote
            ? entity_->SetHistoryWindowDriven(tr.handle, &s, &node_t, 1, ir)
            : entity_->SetHistoryWindowDetailed(tr.handle, &s, &node_t, 1, ir);
        if (!installed) {
            ++st.bodies_install_failed;
            if (ir.why == InstallRefusal::CiIdentityMismatch) {
                ++st.bodies_install_identity_refused;
            }
            if (tr.claim_owner) {
                st.disc_drive = DiscDrive::Refused;
                st.disc_install_refused = ir;
                // The publish below is never reached on this path, so a hand-over that lands on a
                // refused install would otherwise be the one change nothing reports.
                NoteOwnerChange(s.owner_slot, t_us, "the history install was refused");
            }
            continue;
        }
        ++st.bodies_installed;
        if (tr.claim_owner) st.disc_drive = DiscDrive::Installed;

        // ★ AFTER the install, and the order is the whole fix. The holder gate is a PAIR -- the
        // owner dword at PhysCI+0x1C and the node state header's 0x200 -- and the recording changes
        // both on ONE sample (verified over every hand-over in the reference capture: the sample
        // that carries the new sender also carries bit 9). Publishing the owner before installing
        // the node opened a window in which the dword named the new holder while the node still
        // carried the previous tick's state word, so sub_140CEDEB0 answered "nobody" and the touch
        // system saw a release where the recording has a hand-over. Installing first means the pair
        // is only ever old-holder/old-bit or new-holder/new-bit.
        // It sits after the read-back below so the pose verification measures the node this tick
        // installed, not a ring the possession stamp has since been through.

        // ★ UNCONDITIONAL, not behind verify_installed. An install that returned true is a claim
        // about the call; only this says the engine's ring HOLDS the pose. It is one node walk on
        // the same element that was just written, so it costs what the write cost.
        {
            BodyState back;
            uint64_t back_t = 0;
            memset(&back, 0, sizeof back);
            // The remote-replay install resolved its element with the ci[+0] field compare dropped;
            // the read-back must resolve the SAME element or a working install scores installed_unread
            // instead of read_back (DIAG-POSE-DECOUPLE 4a). Every other body keeps the full gate.
            const size_t n = tr.slot_remote
                ? entity_->ReadHistoryWindowDriven(tr.handle, &back, &back_t, 1)
                : entity_->ReadHistoryWindow(tr.handle, &back, &back_t, 1);
            // Compared field by field, never memcmp: BodyState has padding and a memcmp gate goes red
            // or green on uninitialised bytes for reasons unrelated to its subject.
            const bool same = n == 1 && back_t == node_t &&
                              back.com[0] == s.com[0] && back.com[1] == s.com[1] &&
                              back.com[2] == s.com[2] && back.ori[0] == s.ori[0] &&
                              back.ori[1] == s.ori[1] && back.ori[2] == s.ori[2] &&
                              back.ori[3] == s.ori[3] && back.bits11 == s.bits11;
            if (same) {
                ++st.bodies_read_back;
            } else {
                ++st.bodies_installed_unread;
                if (opt.verify_installed) ++verify_mismatches_;
            }
        }

        if (tr.claim_owner) {
            PublishDiscOwner(w, tr, s.owner_slot, t_us, st);
        }
    }

    // ---- players -----------------------------------------------------------------------------
    for (size_t i = 0; i < w.PlayerCount(); ++i) {
        PlayerTrack& tr = w.PlayerAt(i);
        if (!tr.handle_valid) {
            ++st.players_unhandled;
            continue;
        }
        // â˜… An unresolved entrant is NOT published. A capture slot is not a user id, so publishing
        // before W2-C has resolved one to the other would write a recorded pose onto whichever engine
        // entrant happened to land on that handle -- wrong avatar, no error, no log line.
        if (!tr.user_id_valid) {
            ++st.players_unmapped;
            continue;
        }
        if (owners_ && owners_->Validate(tr.user_id) != OwnerVerdict::Ok) {
            ++st.players_refused_owner;
            continue;
        }
        // M1: as-seen is a per-entity SAMPLING TIME and nothing else changes. Ground truth publishes
        // at the playhead exactly, delay removed; as-seen reproduces the recorded offsets. Both are
        // the same publish, three numbers apart -- there is no second implementation.
        if (opt.as_seen && (tr.occluded || !tr.body_updated_this_frame)) {
            ++st.players_skipped_asseen;   // the client did not update this player that frame
            continue;
        }
        const int64_t t_pub = opt.as_seen ? (t_us - tr.render_delay_us + tr.time_offset_us) : t_us;

        PlayerState ps;
        memset(&ps, 0, sizeof ps);
        if (!SamplePlayerFor(w, tr, tr.cursor, t_pub, ps, opt.reversed, opt.smooth)) {
            ++st.players_absent;
            continue;
        }
        if (pose_) {
            PublishGrip(w, tr, opt, ps);
            // PP-B writes the raw block at PlayerCI+60 and the engine's own authority branch fills
            // both slots later in the same frame -- as bit-identical copies, which is the only state a
            // retail offline client ever has (S7.2.4a). So the same value goes to both parameters:
            // forcing them apart would manufacture a configuration the shipped game never produces.
            pose_->Publish(tr.handle, ps, ps);
        }
        ++st.players_published;
    }

    // ---- netvars -----------------------------------------------------------------------------
    // Resolved, never applied as a forward edge stream. value(key, t) is the last write at or before
    // t, so this is identical code forwards, backwards and after a jump.
    KeyedResolver& nv = w.NetVars();


    // ---- pass 1: what is due, and where it goes -------------------------------------------
    // The slot is looked up per key, not assumed. A name the window never carried a slot for is
    // REFUSED by name rather than addressed to slot 0: slot 0 is a real record, so a guess here is a
    // write into the wrong one, which is the failure the whole guard chain exists to prevent.
    struct DueRow {
        size_t k;
        uint64_t key;
        uint64_t value;
        uint16_t slot;
    };
    DueRow due[KeyedResolver::kMaxKeys];
    size_t due_n = 0;

    for (size_t k = 0; k < nv.KeyCount(); ++k) {
        const uint64_t key = nv.KeyAt(k);
        uint64_t value = 0;
        if (!nv.ValueAt(key, t_us, value)) {
            continue;   // no write at or before t: the netvar does not exist yet at this playhead
        }
        uint64_t prev = 0;
        if (w.LastWritten(k, prev) && prev == value) {
            ++st.netvars_unchanged;
            continue;
        }
        uint16_t slot = 0;
        if (!w.NetVarSlotOf(key, slot)) {
            ++st.netvars_attempted;
            ++st.netvars_refused;
            continue;
        }
        if (opt.netvar_owned_elsewhere && opt.netvar_owned_elsewhere(key, slot)) {
            continue;
        }
        if (due_n < KeyedResolver::kMaxKeys) {
            due[due_n].k = k;
            due[due_n].key = key;
            due[due_n].value = value;
            due[due_n].slot = slot;
            ++due_n;
        }
    }

    // ---- pass 2: apply, one rowset per slot, in the engine's own order --------------------------
    // The engine receives a DUV message per (level, slot) and decodes its whole rowset in one call
    // (echovr.exe sub_140D3DF90 -> sub_140F8BCB0). Writing row by row through separate calls would
    // sort the store once per row and fire the change delegates in an order the engine never
    // produces, so replay groups by slot the same way the wire does.
    for (size_t i = 0; i < due_n; ++i) {
        if (due[i].slot == kNetVarSlotNone) continue;   // already consumed into an earlier rowset
        const uint16_t slot = due[i].slot;

        NetVarApplyRow rows[KeyedResolver::kMaxKeys];
        size_t idx[KeyedResolver::kMaxKeys];
        size_t n = 0;
        for (size_t j = i; j < due_n; ++j) {
            if (due[j].slot != slot) continue;
            rows[n].name = due[j].key;
            rows[n].value = due[j].value;
            idx[n] = j;
            ++n;
            due[j].slot = kNetVarSlotNone;
        }

        st.netvars_attempted += (uint32_t)n;
        if (netvar_table_) {
            // The rowset is the unit: the engine decodes one and fires its delegates once, so the
            // whole set lands or the whole set is refused.
            const NetVarApplyStatus s =
                ApplyRowset(netvar_table_, netvar_owner_, slot, rows, n, netvar_apply_);
            if (s == NetVarApplyStatus::Applied) {
                st.netvars_applied += (uint32_t)n;
                // The shadow advances only on a landed write: a shadow that recorded an INTENT
                // would make the next frame skip a write that never happened.
                for (size_t r = 0; r < n; ++r) {
                    w.NoteWritten(due[idx[r]].k, due[idx[r]].value);
                }
            } else {
                st.netvars_refused += (uint32_t)n;
            }
        } else if (netvar_) {
            // ★ No level-backed table on this playback. INetVar answers PER WRITE, so it is asked
            // per row and its bool is READ. Grouping by slot is how a rowset reaches the engine's
            // decoder; it is not a licence to send the first row and drop the rest, nor to call a
            // landed write refused. Both were true of the grouped rewrite and both were caught by
            // the shadow: a write counted refused never advances it, so the next frame re-sends a
            // value the engine already holds and the frame after that reports it unchanged --
            // three counters wrong from one discarded return value.
            for (size_t r = 0; r < n; ++r) {
                NetVarWrite nw;
                nw.name = rows[r].name;
                nw.rawSValue = rows[r].value;
                nw.slot = (uint8_t)slot;
                if (netvar_->Write(nw)) {
                    ++st.netvars_applied;
                    w.NoteWritten(due[idx[r]].k, due[idx[r]].value);
                } else {
                    ++st.netvars_refused;
                }
            }
        } else {
            st.netvars_refused += (uint32_t)n;
        }
    }


    // â˜…â˜… NOT `refused == 0`. With no netvars in the file nothing is attempted, nothing is refused,
    // and that form reported "applying fine" -- a capability claim backed by the absence of evidence.
    // The chain test caught it on a file with no netvar stream. Availability means writes LAND, so it
    // needs a landing: no attempt is "unknown", and defaulting unknown to true is the whole defect.
    st.netvar_apply_available = (st.netvars_attempted > 0 && st.netvars_refused == 0);
    st.netvars_present = (uint32_t)nv.KeyCount();
    // â˜…â˜… The quiet answer is TWO answers. Zero attempts over a window that holds no keys is a fact
    // about the FILE and must not fail a fidelity row; zero attempts over a window that holds keys
    // is a fact about this playhead. Deciding between them needs the key count, which is why it is
    // recorded above rather than re-derived by a reader that no longer has the window.
    st.netvar_state = st.netvars_attempted > 0
                          ? (st.netvars_refused > 0 ? NetVarFidelityState::Refused
                                                    : NetVarFidelityState::Applied)
                      : st.netvars_present > 0 ? NetVarFidelityState::PresentNotApplied
                                               : NetVarFidelityState::NoRecords;

    return st;
}

// entity.h:473: `virtual void ClearSyntheticOwner(PhysHandle) = 0;` -- the release half of the
// claim made at :128 of this file. The flag is OUR record that a claim was made, so it is cleared
// even when the handle no longer resolves: leaving it set would stop the next arm re-claiming.
size_t StateApplier::ReleaseSyntheticOwners(Window& w, OwnerRelease scope) {
    if (entity_ == nullptr) {
        return 0;
    }
    size_t released = 0;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        BodyTrack& tr = w.BodyAt(i);
        if (!tr.synthetic_owner_set) {
            continue;
        }
        // The disc's level is the one claim a discontinuity must NOT hand back: the engine
        // edge-detects the dword, so putting the original owner there between two publishes of
        // the same holder is a release and a grab the owner sees and hears. The next publish
        // swaps old for new in one store, and while the new window decodes the level HOLDS.
#ifndef POSSESSION2_MUTATE_BATTERY_RELEASES_DISC
        if (scope == OwnerRelease::KeepDiscLevel && tr.claim_owner && disc_level_.held &&
            tr.handle.v == disc_level_.handle.v && tr.handle.pool == disc_level_.handle.pool) {
            continue;
        }
#endif
        tr.synthetic_owner_set = false;
        tr.owner_level_published = false;   // nothing is stamped now: the next tick republishes
        // NOT gated on handle_valid. A rebind that refused clears that flag while the claim is
        // still stamped on the engine's object, and entity.cpp addresses the release by the INDEX
        // the claim recorded -- so skipping it here is what froze a claimed disc in mid-air.
        if (tr.claim_owner && disc_level_.held && tr.handle.v == disc_level_.handle.v &&
            tr.handle.pool == disc_level_.handle.pool) {
            disc_level_ = DiscLevel();
        }
        entity_->ClearSyntheticOwner(tr.handle);
        ++released;
    }
    // A rebuild can destroy the disc's track while the level is still stamped; the held handle is
    // then the only record of the claim, and a teardown that skipped it would leave the engine
    // treating the disc as owned by a user who left with the load.
    if (scope == OwnerRelease::All && disc_level_.held) {
        entity_->ClearSyntheticOwner(disc_level_.handle);
        disc_level_ = DiscLevel();
        ++released;
    }
    return released;
}

// entity.h:483. entity.cpp:1768 states the contract from the other side: per-CI growth "is bounded
// by the caller's TrimHistory window rather than by a magic number". The resident window is that
// bound -- it is the span S7.4 seeks inside without a rebuild.
size_t StateApplier::TrimHistories(Window& w, uint64_t keepFromUs, uint64_t keepToUs) {
    if (entity_ == nullptr || keepToUs < keepFromUs) {
        return 0;
    }
    size_t trimmed = 0;
    for (size_t i = 0; i < w.BodyCount(); ++i) {
        BodyTrack& tr = w.BodyAt(i);
        if (!tr.handle_valid) {
            continue;
        }
        entity_->TrimHistory(tr.handle, keepFromUs, keepToUs);
        ++trimmed;
    }
    return trimmed;
}

}  // namespace playback
}  // namespace replay
