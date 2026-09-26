#pragma once

// An IEntity whose ownership is the engine's: one owner dword per body, the host simulating every
// body whose owner's low word is a user slot (< 0x10) or the invalid id. The claim writes the
// synthetic remote owner over it and the release puts the original back, so a test can tell a
// driven body from a released one by the word the engine would read, not by a call count.

#include <stdint.h>
#include <string.h>

#include <map>
#include <vector>

#include "engine/entity.h"

namespace disctest {

struct DiscEntity : replay::IEntity {
    static constexpr size_t   kBodies = 32;
    static constexpr uint32_t kSyntheticOwner = 0xFFFE0010u;  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value; this is the fixture's expected owner word
    static constexpr uint32_t kHostOwner = 0xFFFFFFFFu;   // kInvalid on the authority: simulated  // facts-ok: PHYS.ID_DEAD PC_PRESENT so gen_facts emits no value, and GOAL.USERID_UNSET is still an unmerged pending drop

    uint32_t owner[kBodies];
    uint32_t saved[kBodies] = {};
    bool     claimed[kBodies] = {};
    struct Ring {
        size_t depth = 0;
        replay::BodyState s{};
        uint64_t t = 0;
        size_t installs = 0;
    } ring[kBodies];
    replay::InstallRefusal refuse_install = replay::InstallRefusal::None;

    // component id -> handle, the answer PhysOfComponentId gives; anything else is absent.
    std::map<uint32_t, uint16_t> components;
    uint32_t  ci_flags = 0x0800u;

    // The rules chain's answer. It names ONE disc or none -- the engine keeps a single validated
    // ref -- so a fixture sets it rather than filling a list.
    bool        chain_ok = false;
    const char* chain_refused = "no frisbee in this fixture";
    uint32_t    disc_component = 0;
    uint16_t    disc_handle = 0;
    bool        disc_stands = true;    // the per-tick revalidation's answer
    void*       phys_cs = reinterpret_cast<void*>(0x1000);
    void*       disc_phys_cs = reinterpret_cast<void*>(0x2000);  // facts-ok: PHYS.CI_PAUSED_BIT a test fixture value this case compares against, not an engine position

    DiscEntity() {
        for (uint32_t& o : owner) o = kHostOwner;
        for (uint16_t& g : generation) g = 0xFFFFu;   // every slot empty until a test fills it  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        for (uint16_t& sl : live_slot) sl = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }
    void PlaceDisc(uint32_t componentId, uint16_t handle) {
        components[componentId] = handle;
        chain_ok = true;
        chain_refused = nullptr;
        disc_component = componentId;
        disc_handle = handle;
    }

    // ★ TWO NAMESPACES, AND THEY MUST NOT BE THE SAME NUMBER. An SR15NetUserId's low word is an
    // INDEX into CR15NetGame's per-slot tables; what the two holder gates then compare against the
    // local/remote band is slotTable[index], a different value. A fixture that set them equal --
    // this one did -- could not fail when production composed an id out of the wrong namespace,
    // because both readings named the same table row.
    uint16_t generation[16];                  // genTable[index]
    uint16_t live_slot[16];                   // slotTable[index]: the band value, NOT the index
    uint16_t band_lo = 0;                     // netGame+224: the first remote slot
    uint16_t band_hi = 0;                     // netGame+220: one past the last
    // The head node's 0x200 is NOT a fixture flag: the engine reads it out of the node the applier
    // installed, so here it is read out of the ring the same way. A fixture that let the test set
    // it by hand could not fail when the owner dword was published a tick ahead of the node.
    bool NodeRead(size_t i) const { return ring[i].depth != 0; }
    uint16_t NodeState(size_t i) const { return (uint16_t)(ring[i].s.bits11 & 0x7FFu); }
    bool     sim_bit = false;                 // PhysCI+4 & 0x4000, the local arm's last term
    bool     holder_readable = true;          // false models an element the walk cannot reach
    uint32_t recorded_stamps = 0;
    bool     refuse_recorded_stamp = false;
    // ★ THE ENGINE TAKING THE BODY BACK, which is what DISC-FLIGHT found in the field.
    // CR15NetTouchInteractCS::Update diffs the holder answer against its own latch and fires
    // OnNetTouchGrab / OnNetTouchRelease itself; the handler transfers physics ownership to this
    // host -- the owner dword becomes the host's and the history nodes whose stored owner differs
    // are freed with it (W1-E §2.1). Off by default so the existing cases keep their subject; a
    // case that wants the engine's real behaviour calls RunEngineUpdate() between applies.
    bool     takes_back_on_possession_edge = false;
    uint32_t touch_latch = kHostOwner;   // what the engine last saw the owner dword be
    size_t   take_backs = 0;

    // The engine's own frame, between two of the plugin's ticks.
    void RunEngineUpdate() {
        if (!takes_back_on_possession_edge) return;
        const size_t i = disc_handle % kBodies;
        if (owner[i] == touch_latch) return;
        touch_latch = owner[i];
        owner[i] = kHostOwner;
        ring[i].depth = 0;
        ++take_backs;
    }
    // Every value the engine's owner dword takes, in order. The disc's possession is a LEVEL the
    // engine edge-detects, so what a seek must not do is only visible as a SEQUENCE.
    std::vector<uint32_t> owner_writes;

    // The PHYS CI as two real fields rather than as two booleans: the held bit shares its u16 with
    // fifteen others the engine owns, so a fixture that stored it alone could not catch a writer
    // that clobbered them.
    uint16_t ci_flags6[kBodies] = {};
    uint32_t ci_prev_owner[kBodies] = {};
    bool     touch_refuses = false;
    size_t   touch_writes = 0;
    bool IsHeldAt(size_t body) const { return (ci_flags6[body] >> 1) & 1u; }
    bool SetDiscTouchState(replay::PhysHandle h, bool held, uint32_t lastOwner) override {
        if (touch_refuses) return false;
        const size_t body = h.v % kBodies;
        ci_flags6[body] = (uint16_t)((ci_flags6[body] & (uint16_t)~0x0002u) | (held ? 0x0002u : 0u));
        ci_prev_owner[body] = lastOwner;
        ++touch_writes;
        return true;
    }

    size_t PhysCount() const override { return kBodies; }
    bool IsLocallyControlled(replay::PhysHandle h) const override {
        const uint32_t o = owner[h.v % kBodies];
        const uint16_t index = static_cast<uint16_t>(o);
        if (o == kHostOwner) return true;
        if (index >= 0x10u) return false;             // the out-of-table sentinel
        const uint16_t slot = live_slot[index];
        return slot == 0xFFFFu || slot < band_lo;     // the LOCAL band, or nobody in that row  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    }

    // sub_140CEDEB0's two gates, to the same terms the real one walks.
    bool EngineHolderOf(replay::PhysHandle h, HolderReading& out) const override {
        out = HolderReading();
        if (!holder_readable) return false;
        const size_t body = h.v % kBodies;
        out.read = true;
        out.node_read = NodeRead(body);
        out.node_state = NodeState(body);
        out.owner = owner[body];
        out.band_lo = band_lo;
        out.band_hi = band_hi;
        out.sim_bit = sim_bit;
        const uint16_t index = static_cast<uint16_t>(out.owner);
        const uint16_t gen = static_cast<uint16_t>(out.owner >> 16);
        if (index >= 16u || gen == 0xFFFFu) return true;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        out.live_gen = generation[index];
        out.live_slot = live_slot[index];
        out.gen_ok = (out.live_gen == gen);
        if (!out.gen_ok || out.live_slot == 0xFFFFu) return true;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        if (out.live_slot < band_lo) {
            out.local_arm = out.sim_bit;
        } else if (out.live_slot < band_hi) {
            out.held_bit = out.node_read && (out.node_state & 0x200u) != 0;
            out.remote_arm = out.held_bit;
        }
        out.held = out.local_arm || out.remote_arm;
        return true;
    }
    uint32_t SyntheticOwnerValue() const override { return kSyntheticOwner; }
    bool SetRecordedOwner(replay::PhysHandle h, uint32_t value) override {
        if (refuse_recorded_stamp) return false;
        const size_t i = h.v % kBodies;
        if (owner[i] == value) return true;
        if (!claimed[i]) {
            saved[i] = owner[i];
            claimed[i] = true;
        }
        owner[i] = value;
        owner_writes.push_back(value);
        ++recorded_stamps;
        return true;
    }
    bool UserIdOf(replay::UserIndex u, uint32_t& out) const override {
        if (u.v >= 16u || generation[u.v] == 0xFFFFu) return false;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        out = static_cast<uint32_t>(u.v) | (static_cast<uint32_t>(generation[u.v]) << 16);
        return true;
    }
    void SetSyntheticOwner(replay::PhysHandle h) override {
        const size_t i = h.v % kBodies;
        if (owner[i] == kSyntheticOwner) return;
        saved[i] = owner[i];
        claimed[i] = true;
        owner[i] = kSyntheticOwner;
        owner_writes.push_back(kSyntheticOwner);
    }
    void ClearSyntheticOwner(replay::PhysHandle h) override {
        const size_t i = h.v % kBodies;
        if (!claimed[i]) return;
        owner[i] = saved[i];
        owner_writes.push_back(saved[i]);
        claimed[i] = false;
    }
    bool ReadBody(replay::PhysHandle, replay::BodyState&) const override { return false; }
    void PushHistory(replay::PhysHandle, const replay::BodyState&, uint64_t) override {}
    void TrimHistory(replay::PhysHandle, uint64_t, uint64_t) override {}
    bool SetHistoryWindow(replay::PhysHandle h, const replay::BodyState* s, const uint64_t* t,
                          size_t n) override {
        if (refuse_install != replay::InstallRefusal::None || n == 0) return false;
        Ring& r = ring[h.v % kBodies];
        r.depth = n;
        r.s = s[0];
        r.t = t[0];
        ++r.installs;
        return true;
    }
    bool SetHistoryWindowDetailed(replay::PhysHandle h, const replay::BodyState* s,
                                  const uint64_t* t, size_t n,
                                  replay::InstallReading& r) override {
        r = replay::InstallReading();
        const bool ok = SetHistoryWindow(h, s, t, n);
        r.why = ok ? replay::InstallRefusal::None : refuse_install;
        return ok;
    }
    size_t ReadHistoryWindow(replay::PhysHandle h, replay::BodyState* s, uint64_t* t,
                             size_t cap) const override {
        const Ring& r = ring[h.v % kBodies];
        if (!r.depth || cap == 0) return 0;
        s[0] = r.s;
        t[0] = r.t;
        return 1;
    }
    float GetTimeOffset(replay::PhysHandle) const override { return 0.0f; }
    void SetTimeOffset(replay::PhysHandle, float) override {}

    // Players: user slot u's body is handle kPlayerBodyBase + u.
    static constexpr uint16_t kPlayerBodyBase = 16;
    size_t PlayerCount() const override { return 16; }
    bool ReadPlayer(replay::PlayerHandle, replay::PlayerState&) const override { return false; }
    void PushPlayerHistory(replay::PlayerHandle, const replay::PlayerState&, uint64_t) override {}
    replay::PhysHandle PhysOf(replay::PlayerHandle p) const override {
        return replay::PhysHandle(static_cast<uint16_t>(kPlayerBodyBase + p.v));
    }
    replay::PlayerHandle PlayerOf(replay::UserIndex u) const override {
        return replay::PlayerHandle(u.v);
    }

    replay::PhysHandle PhysOfComponentId(uint32_t id, ComponentIdReading* seen) const override {
        if (seen) *seen = ComponentIdReading();
        const auto at = components.find(id);
        if (at == components.end()) return replay::PhysHandle(0xFFFFu);  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        if (seen) {
            seen->mapRead = true;
            seen->poolIndex = at->second;
            seen->generation = static_cast<uint16_t>(id >> 16);
            seen->ciFlagsRead = true;
            seen->ciFlags = ci_flags;
        }
        return replay::PhysHandle(at->second);
    }
    bool BindDisc(void*, DiscBind& out) override {
        out = DiscBind();
        out.playerPhysCs = phys_cs;
        if (!chain_ok) { out.refused = chain_refused; return false; }
        out.physCs = disc_phys_cs;
        out.frisbeeCs = reinterpret_cast<void*>(0x3000);
        out.physId = disc_component;
        out.shortcutAgreed = true;
        const auto at = components.find(disc_component);
        if (at == components.end()) {
            out.physPoolRefused = true;
            out.refused = "the net-physics system does not vouch for the disc body";
            return false;
        }
        out.reading.mapRead = true;
        out.reading.poolIndex = at->second;
        out.reading.generation = static_cast<uint16_t>(disc_component >> 16);
        out.reading.ciFlagsRead = true;
        out.reading.ciFlags = ci_flags;
        out.handle = replay::PhysHandle(at->second, replay::PhysHandle::kPoolDisc);
        out.refused = nullptr;
        return true;
    }
    bool DiscStands(uint32_t physId) const override {
        return chain_ok && disc_stands && physId == disc_component;
    }
    void ForgetSyntheticOwner(replay::PhysHandle h) override { claimed[h.v % kBodies] = false; }

    void SetLodThrottle(bool) override {}
    bool Available() const override { return true; }
    const char* Diagnostic() const override { return "disc fixture"; }
};

}  // namespace disctest
