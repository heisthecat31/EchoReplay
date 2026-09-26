#pragma once

// S5.4.7's ORACLE producers, AS THE REGISTRY PUBLISHES THEM -- owned by W2-A, read by capture.cpp.
//
// The producers are engine entry points, not modules under src/, so the only thing that can decide
// whether the capture may call one is the registry (facts/facts.tsv -> generated/facts_generated.h).
// One row per producer, carrying the address, the 20-byte prologue guard the registry recorded for
// it, and the CALL SHAPE it may be invoked through.
//
// ~ A ROW IS CALLABLE ONLY WHEN ITS CALL SHAPE IS PROMOTED. `argMove` is the argument-move bytes
// Guarded<> checks at the entry point before the address is ever called -- IDENT.ADMIT_USER carries
// exactly that pair (ARGMOVE + ARGMOVE_OFF) because a wrong prototype killed the client once already.
// These three rows do not carry it: promoting them is DL-22 / V-I-W2I1, owned by W2-I. So today every
// row refuses, by NAME and with the TIER it refused at, and the day those argmove rows merge this
// path binds with no edit here. The decision is never taken from a test global: a test substitutes a
// registry VIEW -- a registry whose rows are promoted -- and never a builder pointer, so "bound" is
// reachable by promoting a row and by nothing else.

#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/pageprobe.h"
#include "engine/oracletap.h"
#include "generated/facts_generated.h"

namespace replay {
namespace capture {

// The /session handler has two arms and the registry files them as two rows. They are ONE producer:
// a tap bound to one arm returns an empty document whenever the other predicate selects, and that
// reads as a clean snapshot rather than an unhandled mode.
enum class OracleRole : uint8_t { SessionPrimary = 0, SessionAlt = 1, Bones = 2 };
inline constexpr size_t kOracleRoleCount = 3;

struct OracleRegistryRow {
    const char*    factId;
    OracleRole     role;
    uint64_t       va;
    const uint8_t* prologue;
    uint32_t       prologueBytes;
    // Null until the registry carries <factId>.ARGMOVE. Without it Guarded<> has nothing to verify
    // the entry point against, so an unpromoted prototype is a refusal and never a cast.
    const uint8_t* argMove;
    uint32_t       argMoveBytes;
    uint32_t       argMoveOff;
};

inline constexpr OracleRegistryRow kOracleRegistry[kOracleRoleCount] = {
    {"ORACLE.SESSION_BUILDER", OracleRole::SessionPrimary, facts::ORACLE_SESSION_BUILDER,
     facts::ORACLE_SESSION_BUILDER_PROLOGUE,
     uint32_t(sizeof facts::ORACLE_SESSION_BUILDER_PROLOGUE), nullptr, 0, 0},
    {"ORACLE.SESSION_BUILDER_ALT", OracleRole::SessionAlt, facts::ORACLE_SESSION_BUILDER_ALT,
     facts::ORACLE_SESSION_BUILDER_ALT_PROLOGUE,
     uint32_t(sizeof facts::ORACLE_SESSION_BUILDER_ALT_PROLOGUE), nullptr, 0, 0},
    {"ORACLE.BONES_BUILDER", OracleRole::Bones, facts::ORACLE_BONES_BUILDER,
     facts::ORACLE_BONES_BUILDER_PROLOGUE, uint32_t(sizeof facts::ORACLE_BONES_BUILDER_PROLOGUE),
     nullptr, 0, 0},
};

// A resolver turns one promoted row into the pointer it will be called through. The shipped one reads
// the loaded image; a test host has no echovr.exe in it, so a view brings its own.
using OracleResolveFn = BuilderFn (*)(const OracleRegistryRow&);

struct OracleRegistryView {
    const OracleRegistryRow* rows;
    size_t                   count;
    OracleResolveFn          resolve;
    void*                    sessionCtx;
    void*                    bonesCtx;
};

// The tier this path may act on the row at. `U` is read from the registry's OWN list of unverified
// ids (S4.6 rule 5) rather than from a copy of that list kept here, so a row demoted upstream is
// refused here without an edit.
inline const char* OracleRowTier(const OracleRegistryRow& r) {
    for (size_t i = 0; i < facts::UNVERIFIED_FACT_COUNT; ++i) {
        if (strcmp(facts::UNVERIFIED_FACT_IDS[i], r.factId) == 0) {
            return "U";
        }
    }
    return r.argMove == nullptr ? "PROTO-UNPROMOTED" : "CALLABLE";
}

inline bool OracleBytesReadable(const uint8_t* at, uint32_t n) {
    if (n == 0) {
        return true;
    }
    return PageSpanReadable(at, n);
}

// identity_access.cpp's Guarded<Sig>, over a row whose guard bytes are sized at runtime.
// reinterpret_cast turns any address into any function type without a word, so nothing is returned
// until the registry's own bytes are found at the registry's own address.
inline BuilderFn GuardedOracleBuilder(const OracleRegistryRow& r) {
    const void* at = buildguard::MappedAddress(r.va);
    if (at == nullptr) {
        return nullptr;
    }
    const uint8_t* p = static_cast<const uint8_t*>(at);
    if (!OracleBytesReadable(p, r.prologueBytes) || memcmp(p, r.prologue, r.prologueBytes) != 0) {
        return nullptr;
    }
    const uint8_t* moves = p + r.argMoveOff;
    if (!OracleBytesReadable(moves, r.argMoveBytes) ||
        memcmp(moves, r.argMove, r.argMoveBytes) != 0) {
        return nullptr;
    }
    return reinterpret_cast<BuilderFn>(const_cast<void*>(static_cast<const void*>(p)));
}

}  // namespace capture
}  // namespace replay
