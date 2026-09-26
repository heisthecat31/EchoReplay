// E-8: the S_ASSEEN inputs of §5.4.4.
//
// Every number comes from facts_generated.h by id. The netGame chain below was re-read on PC out
// of CR15NetGame::PeerTime, which is the one function that touches all four of the fields this
// stream needs; derivations in docs/spikes/W1-E.md.

#include "engine/asseen.h"

#include <new>
#include <string.h>

#include "generated/facts_generated.h"

namespace {

template <typename T>
inline T Rd(const void* base, uint32_t off) {
    T v;
    memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

template <typename T>
inline void Wr(void* base, uint32_t off, const T& v) {
    memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof(T));
}

// The -0.04444f render delay, stored in the registry as its IEEE bit pattern so nobody has to
// trust a decimal rendering of it.
inline float RenderDelaySeconds() {
    float f;
    const uint32_t bits = facts::ENTITY_RENDER_DELAY_S;
    memcpy(&f, &bits, sizeof f);
    return f;
}

class AsSeen final : public IAsSeen {
public:
    AsSeen(void* netGame, IEntity* entity)
        : netGame_(netGame),
          playerCs_(entity ? replay::entity::RemotePlayerSystem(entity) : nullptr) {}

    bool Read(AsSeenSample& out) const override {
        out = AsSeenSample{};
        out.renderDelaySec = RenderDelaySeconds();
        out.valid = kAsSeenRenderDelay;

        if (netGame_) {
            // Which register IS the peer time depends on the authority role, and PeerTime picks
            // between them with exactly this test. Capturing the wrong one would be invisible in
            // a host-recorded file and wrong in a client-recorded one.
            out.peerTimeUs      = Rd<uint64_t>(netGame_, IsAuthority() ? facts::NETGAME_PEERTIME_OFF
                                                                       : facts::NETGAME_SERVERTIME_OFF_PC);
            out.serverTimeEstUs = Rd<uint64_t>(netGame_, facts::NETGAME_SERVERTIME_OFF_PC);
            out.valid |= kAsSeenPeerTime | kAsSeenServerTime;
            // serverTimeSlew is deliberately absent: its offset comes from R6's reading of
            // UpdateServerTime on ARM64 and W1-E did not re-derive it on PC. Leaving the bit
            // clear says "not measured"; filling the field with zero would say "measured zero".
        }

        if (playerCs_) {
            out.lodThrottleEnable = Rd<uint32_t>(playerCs_, facts::PLAYER_CS_LOD_ENABLE_OFF);
            out.lodPerFrameBudget = Rd<uint32_t>(playerCs_, facts::PLAYER_CS_LOD_BUDGET_OFF);
            out.lodCursor         = Rd<uint32_t>(playerCs_, facts::PLAYER_CS_LOD_CURSOR_OFF);
            out.valid |= kAsSeenLod;
        }
        return out.valid != 0;
    }

    bool ReadPlayer(UserIndex u, AsSeenPerPlayer& out) const override {
        out = AsSeenPerPlayer{};
        if (!netGame_) return false;
        // PeerTime's own chain: the per-user pawn word carries a 5-bit slot, and the slot indexes
        // the per-slot peer-time table. Two indirections, both re-read on PC.
        const uint32_t pawnOff = facts::NETGAME_PAWN_TABLE_OFF +
                                 facts::NETGAME_PAWN_TABLE_STRIDE * u.v;
        const uint16_t pawn    = Rd<uint16_t>(netGame_, pawnOff);
        const uint32_t slot    = pawn & facts::NETGAME_PAWN_SLOT_MASK;
        out.peerTimeUs = Rd<uint64_t>(netGame_, facts::NETGAME_PEERSLOT_BASE_PC +
                                                facts::NETGAME_PEERSLOT_STRIDE_PC * slot);
        return true;
    }

    void Apply(const AsSeenSample& in) override {
        // Only what was captured is applied. Writing a field whose `valid` bit was clear would
        // stamp a zero over a live register on the strength of a value nobody ever read.
        if (netGame_ && (in.valid & kAsSeenPeerTime)) {
            Wr<uint64_t>(netGame_, facts::NETGAME_PEERTIME_OFF, in.peerTimeUs);
            Wr<uint64_t>(netGame_, facts::NETGAME_SERVERTIME_OFF_PC, in.serverTimeEstUs);
        }
        if (playerCs_ && (in.valid & kAsSeenLod)) {
            Wr<uint32_t>(playerCs_, facts::PLAYER_CS_LOD_ENABLE_OFF, in.lodThrottleEnable);
            Wr<uint32_t>(playerCs_, facts::PLAYER_CS_LOD_BUDGET_OFF, in.lodPerFrameBudget);
            Wr<uint32_t>(playerCs_, facts::PLAYER_CS_LOD_CURSOR_OFF, in.lodCursor);
        }
    }

    void ApplyPlayer(UserIndex u, const AsSeenPerPlayer& in) override {
        if (!netGame_) return;
        const uint32_t pawnOff = facts::NETGAME_PAWN_TABLE_OFF +
                                 facts::NETGAME_PAWN_TABLE_STRIDE * u.v;
        const uint32_t slot = Rd<uint16_t>(netGame_, pawnOff) & facts::NETGAME_PAWN_SLOT_MASK;
        Wr<uint64_t>(netGame_, facts::NETGAME_PEERSLOT_BASE_PC +
                               facts::NETGAME_PEERSLOT_STRIDE_PC * slot, in.peerTimeUs);
    }

private:
    // (flags & 2) || ((flags & 4) == 0 && (flags & 0x40)) -- the same predicate PeerTime,
    // IsLocallyControlled and UpdateInterpolatedPlayerStates all branch on. The flags word is
    // reached through a pointer, not stored inline.
    bool IsAuthority() const {
        const void* p = Rd<void*>(netGame_, facts::NETGAME_ROLEFLAGS_OFF);
        if (!p) return false;
        const uint64_t f = Rd<uint64_t>(p, 0);
        return (f & 2u) != 0u || ((f & 4u) == 0u && (f & 0x40u) != 0u);
    }

    void* netGame_;
    void* playerCs_;
};

AsSeen* g_instance = nullptr;
alignas(AsSeen) unsigned char g_storage[sizeof(AsSeen)];

}  // namespace

namespace replay {
namespace asseen {

IAsSeen* AsSeenCreate(void* netGame, IEntity* entity) {
    g_instance = new (static_cast<void*>(g_storage)) AsSeen(netGame, entity);
    return g_instance;
}

// NOT a destructor, and the name overstates it: the object has static storage duration and
// AsSeenCreate placement-news over the same bytes every time, so this only forgets the instance.
// The consequence is worth stating because it is the opposite of the usual hazard: a consumer that
// cached services->asSeen across a re-bind does not dangle -- it silently follows the NEW reader.
// Nothing in the plugin caches it (the module reassigns services->asSeen on every bind and unbind),
// and the entity module's "dropped with no write into it" holds because this writes nothing into
// the game space, not because the object goes away. A consumer that ever does cache it must
// re-read services->asSeen per tick rather than trusting this call to have invalidated its copy.
void AsSeenDestroy(IAsSeen* p) {
    if (p && p == g_instance) g_instance = nullptr;
}

}  // namespace asseen
}  // namespace replay
