#pragma once

// The subject-pose composer behind W2-E's `cam look at <n>`.
//
// WHY IT LIVES HERE AND NOT IN src/camera/.  Resolving a selector to a point means dereferencing
// the roster (identity_access) and the physics pools (IEntity), and W1-E is the packet that owns
// both.  ARCH-19 R3 puts the SEAM and the consumer in W2-E and the composer here, so the
// dependency runs one way only: the entity module calls camera_control::SetSubjectPose, and
// nothing in src/camera/ names this header.  The camera library is linked into images that do not
// carry the entity module, and a camera_control that called back into the entity module would
// stop those images linking.

#include <stdint.h>

#include "camera/camera_control.h"
#include "engine/entity.h"

namespace replay {
namespace entity {

// Which UserIndex the engine's 1-based selector names, via (teamId, teamSlot).
//
// The n -> (teamId, teamSlot) split is camera_control::PackSelector's -- read out of
// CAM.SET_CAMERA_MODE_IMPL, and deliberately NOT re-derived here.  A second hand-written copy of
// the packing is the defect the PackSelector/PackedSelectorWord pair exists to prevent, and it
// would be a copy that no test of either surface could catch disagreeing.
//
// Resolved (`userIndexOut` written), or which step declined:
//   RosterUnreadable -- `ng` is null, or the entrant count or the user array it bounds is not in
//                       committed memory, so TeamOf/TeamSlotOf could not be asked at all;
//   NoSuchSubject    -- the selector is outside 1..kPlayerSelectorMax, or the roster is readable
//                       and nobody occupies that (teamId, teamSlot).
camera_control::SubjectResolve UserOfSelector(void* ng, int32_t selector, uint16_t* userIndexOut);

// That user's body centre of mass, in the engine's world frame -- the frame viewstate+0x60 is in.
// False when no entity has been published, when `ng` is not the netGame the published entity's
// pools belong to, or when the user carries no player or no physics body this tick.
bool WorldPointOfUser(void* ng, uint16_t userIndex, float outPos[3]);

// The two of them as the seam camera_control::SetSubjectPose takes.
extern const camera_control::SubjectPoseSeam kSubjectPose;

// The pools half of the seam's state.  The roster half reads `ng` directly and needs nothing; the
// body half needs the IEntity this module published AND the netGame that entity is bound to,
// because a point read out of one session's pools under another session's roster index is the
// wrong player rendered with no complaint.  entity_module.cpp is the shipping caller -- Init,
// Bind, Unbind and Shutdown, wherever `g.entity` moves -- and the test drives a fake.
void     SetSubjectEntity(IEntity*, void* netGame);
IEntity* SubjectEntity();

}  // namespace entity
}  // namespace replay
