#pragma once

// W2-F. "Is the offline-bit lock held in this PROCESS?" as one predicate, so the probe that answers
// it in the game and the offline test that proves the answer can fail are reading the same rule.
//
// ★ TWO STATES ARE HELD AND ONLY ONE OF THEM INSTALLED ANYTHING. Every probe DLL links
// replay_bootstrap, so a multi-probe launch runs offlinelock::Apply() once per module and only the
// first can hold the trampoline; MinHook refuses the rest. A module reporting AlreadyLockedInProcess
// is describing a process that IS locked. Treating it as a failure would refuse a protected launch;
// treating NotAttempted as held would let an unprotected one through. The second is the direction
// that costs something, and it is the one this predicate exists to make unrepresentable.
//
// ★ AND IT IS DELIBERATELY NOT A `state != NotAttempted` TEST. Every RefusedBy* value is a state in
// which the lock was attempted and IS NOT HELD -- RefusedNoSites, RefusedSelfTest and
// RefusedByConfig all mean the bit is unprotected. An inequality would pass all of them.
//
// It lives in tests/ui because src/bootstrap/offlinelock.h is W1-C's. Four call sites now open-code
// this same disjunction (probe_audio, probe_teamgate, probe_offlinelock, and this probe); the right
// long-term home is offlinelock.h itself, which is a note for W1-C rather than an edit from here.

#include "bootstrap/offlinelock.h"

namespace replay {
namespace uiprobe {

inline bool OfflineLockHeld(offlinelock::LockState state) {
    return state == offlinelock::LockState::Locked ||
           state == offlinelock::LockState::AlreadyLockedInProcess;
}

}  // namespace uiprobe
}  // namespace replay
