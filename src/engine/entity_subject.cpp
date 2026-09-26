#include "engine/entity_subject.h"

#include <string.h>

#include "engine/identity_access.h"

namespace replay {
namespace entity {
namespace {

IEntity* gEntity  = nullptr;
void*    gNetGame = nullptr;

// B.4's documented "no such handle" answer -- PhysOfNetId and LocalPlayerAt both return it.
constexpr uint16_t kNoHandle = 0xFFFFu;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value

}  // namespace

void     SetSubjectEntity(IEntity* e, void* netGame) { gEntity = e; gNetGame = netGame; }
IEntity* SubjectEntity() { return gEntity; }

camera_control::SubjectResolve UserOfSelector(void* ng, int32_t selector, uint16_t* userIndexOut) {
    using camera_control::SubjectResolve;
    if (!userIndexOut) return SubjectResolve::RosterUnreadable;

    uint8_t teamId = 0, teamSlot = 0;
    if (!camera_control::PackSelector(selector, &teamId, &teamSlot)) {
        return SubjectResolve::NoSuchSubject;
    }

    // The bound is identity_access's, taken from the engine's own entrant count and checked
    // against committed memory there.  Without it this scan has no end: the user array carries no
    // terminator, and an unoccupied entry reads as a team and a slot like any other.
    uint16_t entrants = 0;
    if (!identity_access::UserTableBound(ng, entrants)) return SubjectResolve::RosterUnreadable;

    for (uint16_t i = 0; i < entrants; ++i) {
        const UserIndex u(i);
        if (identity_access::TeamOf(ng, u) == teamId &&
            identity_access::TeamSlotOf(ng, u) == teamSlot) {
            *userIndexOut = i;
            return SubjectResolve::Resolved;
        }
    }
    return SubjectResolve::NoSuchSubject;
}

bool WorldPointOfUser(void* ng, uint16_t userIndex, float outPos[3]) {
    if (!outPos) return false;
    IEntity* const e = gEntity;
    if (!e) return false;
    // A roster index resolved out of one netGame, read against pools bound to another, names a
    // different player and comes back as a clean success.  A known mismatch is refused; an
    // unrecorded binding falls through to the pools, which are unresolved and refuse for
    // themselves.
    if (gNetGame && ng != gNetGame) return false;

    const PlayerHandle player = e->PlayerOf(UserIndex(userIndex));
    if (player.v == kNoHandle) return false;
    const PhysHandle phys = e->PhysOf(player);
    if (phys.v == kNoHandle) return false;

    BodyState body;
    memset(&body, 0, sizeof body);
    if (!e->ReadBody(phys, body)) return false;

    // com, with no offset.  The engine's own follow-player camera (CR15NetFollowPlayerCS) requires
    // Transform and Physics and no head component, so a `com + k` would be a constant nothing in
    // the binary supplies.
    memcpy(outPos, body.com, sizeof body.com);
    return true;
}

const camera_control::SubjectPoseSeam kSubjectPose = { &UserOfSelector, &WorldPointOfUser };

}  // namespace entity
}  // namespace replay
