#include "playback/core/ownership.h"

namespace replay {
namespace playback {

const char* OwnerVerdictName(OwnerVerdict v) {
    switch (v) {
        case OwnerVerdict::Ok: return "ok";
        case OwnerVerdict::IsLocalUser: return "is-local-user";
        case OwnerVerdict::IsInvalidSentinel: return "is-kInvalid";
        case OwnerVerdict::OutOfTable: return "out-of-user-table";
        case OwnerVerdict::NotRegistered: return "not-registered";
        case OwnerVerdict::GuardUnconfigured: return "guard-unconfigured";
        default: return "?";
    }
}

void OwnerGuard::Reset(uint32_t local_user_id, uint32_t k_invalid) {
    count_ = 0;
    local_ = local_user_id;
    local_known_ = true;
    k_invalid_ = k_invalid;
    configured_ = true;
    refusals_ = 0;
    last_refusal_ = OwnerVerdict::Ok;
}

void OwnerGuard::ResetLocalUnknown(uint32_t k_invalid) {
    Reset(0, k_invalid);
    local_known_ = false;
}

// The classification, in one place, so RegisterEntrant and Validate cannot drift about what a bad id
// is. Order matters: kInvalid is checked before the table bound because kInvalid's low word is also
// out of the table, and reporting the wrong reason sends the next reader to the wrong packet.
static OwnerVerdict Classify(uint32_t id, uint32_t local, bool local_known, uint32_t k_invalid) {
    if (id == k_invalid) {
        return OwnerVerdict::IsInvalidSentinel;
    }
    // Asked only when there is an answer to ask against. `local` is 0 when there is not, and 0 is a
    // legal user index -- testing against it there is the OPEN-1 defect, not a conservative guess.
    if (local_known && id == local) {
        return OwnerVerdict::IsLocalUser;
    }
    if ((uint16_t)id >= kUserSlotTableMax) {
        return OwnerVerdict::OutOfTable;
    }
    return OwnerVerdict::Ok;
}

OwnerVerdict OwnerGuard::RegisterEntrant(uint32_t user_id) {
    if (!configured_) {
        return OwnerVerdict::GuardUnconfigured;
    }
    const OwnerVerdict v = Classify(user_id, local_, local_known_, k_invalid_);
    if (v != OwnerVerdict::Ok) {
        return v;   // refused, not recorded: a bad allocation must not become a publishable entrant
    }
    for (size_t i = 0; i < count_; ++i) {
        if (entrants_[i] == user_id) {
            return OwnerVerdict::Ok;   // idempotent; re-admitting the same entrant is not an error
        }
    }
    if (count_ >= kMaxEntrants) {
        return OwnerVerdict::NotRegistered;
    }
    entrants_[count_++] = user_id;
    return OwnerVerdict::Ok;
}

OwnerVerdict OwnerGuard::Validate(uint32_t user_id) const {
    OwnerVerdict v =
        configured_ ? Classify(user_id, local_, local_known_, k_invalid_)
                    : OwnerVerdict::GuardUnconfigured;
    if (v == OwnerVerdict::Ok) {
        bool found = false;
        for (size_t i = 0; i < count_; ++i) {
            if (entrants_[i] == user_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            v = OwnerVerdict::NotRegistered;
        }
    }
    if (v != OwnerVerdict::Ok) {
        ++refusals_;
        last_refusal_ = v;
    }
    return v;
}

}  // namespace playback
}  // namespace replay
