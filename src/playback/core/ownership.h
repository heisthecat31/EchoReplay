#pragma once

// W2-B. INV-OWN1: the one assumption the owner-id ruling rests on, with the check behind it.
//
// ---------------------------------------------------------------------------------------------
// ★ THE RULING, and why this file exists rather than a comment.
//
// PhysCI+28 carries the RECORDED ENTRANT'S REAL USER ID -- not a synthetic value. W1-E's enumeration
// of that field found 41 readers in 31 functions and three guard families the codebase actually uses:
//   owner == kInvalid        "nobody owns this"
//   owner == <some user id>  "owned by that user"
//   low word < 0x10          "owned by a slot in the user table"
// The previously shipped 0xFFFE0010 is the one choice with NO branch for it -- not kInvalid, not a
// user, and outside the table -- so every reader took a fall-through arm nobody designed. That is what
// produced this packet's F1 (PeerTime returning 0, hence the interpolator's sample time pinned to 0),
// a corrupted cached time at CI+304, an un-releasable body, and a synthetic id leaking onto the wire.
//
// With a real id, all 41 readers take the branch they take in a normal match: "owned by someone who is
// not me." That is not a workaround; it is the configuration the engine was written for.
//
// ★ AND IT RESTS ON EXACTLY ONE THING, which is why it is checked here and not assumed:
//
//   INV-OWN1. No replayed entrant's user id is the replay session's OWN local user id.
//
// If it ever were, CR15NetPhysicsCS::IsLocallyControlled would return TRUE for that entrant, physics
// would simulate the body instead of taking our installed state, and it would overwrite the replay --
// which is HP1's original failure, arriving through the front door. We author entrants through the
// bot-admission path so we control the allocation, so this is ours to satisfy rather than ours to hope
// for. W2-C owns the allocation half; this is the publication half, asserted every frame we publish.
//
// ★ Both arms are constructible, which is the point (tests/playback):
//   GREEN  entrants allocated by the admission path, none equal to the local id
//   RED    force one entrant's id to the local id -- Validate() refuses it and names it
// A guard whose red is unconstructible is not a guard, and this project has shipped three of those.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace playback {

// The engine's own bound, from ENTITY.USER_SLOT_TABLE_MAX: IsLocallyControlled and PeerTime both
// return early for a low word >= this, before touching memory. It is a MEASURED fact and reaches code
// through facts_generated.h; it is named here only so the reasoning below reads without a second file.
static const uint16_t kUserSlotTableMax = 0x10;

enum class OwnerVerdict : uint8_t {
    Ok = 0,
    IsLocalUser,        // ★ INV-OWN1 violated: this entrant would be locally controlled
    IsInvalidSentinel,  // kInvalid: offline that means "locally controlled", HP1's original defect
    OutOfTable,         // low word >= kUserSlotTableMax: the 0xFFFE0010 class, no branch owns it
    NotRegistered,      // an id no admission call produced -- we did not create this entrant
    // ★★ SEPARATE FROM NotRegistered, and the distinction is the same one W2-F found in its own
    // ContentMismatch: "we asked and this is not an entrant we admitted" and "nobody configured the
    // guard, so it cannot answer" were one value. The first is a finding about the replay; the second
    // is a finding about US, and a caller that saw NotRegistered could not tell which it had.
    GuardUnconfigured,
    Count
};

const char* OwnerVerdictName(OwnerVerdict);

// Holds what the session allocated, so a publish can be checked against it rather than against a rule
// somebody remembered. Small and linear: an Arena match has at most 16 user slots.
class OwnerGuard {
public:
    static const size_t kMaxEntrants = 16;

    // `local_user_id` is the replay session's own id, and `k_invalid` is the engine's kInvalid as read
    // at run time -- passed in rather than baked, because it is a value in .rdata this packet has not
    // re-derived and a wrong constant here would make the guard refuse everything or nothing.
    void Reset(uint32_t local_user_id, uint32_t k_invalid);

    // The same arming, for the session whose local user id has NO SOURCE -- IEntity::LocalUserIndex
    // answered NotAvailable, None or Ambiguous. The guard is configured, and every arm except
    // is-local-user still runs; that one is not asked, because the only way to ask it is to invent
    // a number and the number that used to be invented was 0, which refused a GENUINE index 0 and
    // that player's body never published (AUDIT_WIRING_A OPEN-1). Refusing nobody on one arm is a
    // stated hole; refusing the wrong body is a silently wrong replay.
    //
    // ★ It is a WINDOW, not a life sentence. This state says "there was no
    // answer at the moment we asked", and the engine can be bound and readable and still unable to
    // answer -- the broadcaster pool resolves separately from Available(). The caller re-asks every
    // tick while LocalUserKnown() is false and Reset()s with the real index the moment one exists,
    // which is also why the entrants admitted meanwhile must be re-registered: they were admitted
    // with this arm off.
    void ResetLocalUnknown(uint32_t k_invalid);

    // Called once per entrant as W2-C admits it. Refuses rather than records when the id would break
    // INV-OWN1, so a bad allocation is caught where it happens and not one frame before a wrong replay.
    OwnerVerdict RegisterEntrant(uint32_t user_id);

    // Called on every publish. Cheap -- a compare against the local id plus a membership test over at
    // most 16 entries -- and it is the assertion that turns INV-OWN1 from prose into a runtime fact.
    OwnerVerdict Validate(uint32_t user_id) const;

    size_t EntrantCount() const { return count_; }
    uint32_t EntrantAt(size_t i) const { return i < count_ ? entrants_[i] : 0; }
    // Meaningless unless LocalUserKnown(): a caller that reads the id alone cannot tell the
    // engine's answer 0 from "there was no answer".
    uint32_t LocalUserId() const { return local_; }
    bool LocalUserKnown() const { return local_known_; }

    // Non-zero means a publish was refused. A gate run ending with a non-zero count is a failure, not
    // a statistic: it means some entity was not published and the replay is silently incomplete.
    uint64_t Refusals() const { return refusals_; }
    OwnerVerdict LastRefusal() const { return last_refusal_; }

private:
    uint32_t entrants_[kMaxEntrants] = {};
    size_t count_ = 0;
    uint32_t local_ = 0;
    bool local_known_ = false;
    uint32_t k_invalid_ = 0xFFFFFFFFu;
    bool configured_ = false;
    mutable uint64_t refusals_ = 0;
    mutable OwnerVerdict last_refusal_ = OwnerVerdict::Ok;
};

}  // namespace playback
}  // namespace replay
