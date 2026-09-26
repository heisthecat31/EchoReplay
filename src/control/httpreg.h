#pragma once

// MULTI-LOAD. The identity half of the HTTP route registration, with no engine in it.
//
// The routes live on a listener EMBEDDED in CR15NetGame. A cross-level load runs
// SESSION.CREATE_OFFLINE_MATCH, which tears the session down and builds a new one -- and the
// registration used to latch on a bool, so the four routes stayed on the dead object and the
// engine answered 404 for every one of our URIs while Running() still said true
// (DIAG-LEVEL-MISMATCH-404 s1.3). Every other per-session binding in this plugin re-validates
// itself every tick; this is that validation for the routes.
//
// The identity is the PAIR -- the netGame AND the evhttp pointer inside its listener -- because
// src/ui/csregister.cpp:479 documents the engine rebuilding a space under the SAME pointer. One
// pointer compare would miss exactly that case.
//
// Header-only and free of engine, log and dispatch, so tests/control can drive the whole thing
// against a fake listener: replay_control_core does not carry http.cpp and never will.

#include <stddef.h>

namespace replay {
namespace control {
namespace http {

// The registration's own record of what it registered on.
struct Registration {
    bool        running = false;
    const void* netGame = nullptr;
    const void* evhttp = nullptr;
    void*       listener = nullptr;
};

// The whole decision, as a pure function of the two identities.
//
// A null live netGame is "there is nothing to register on yet", not "re-register": the tick runs
// before the session exists and would otherwise ask every frame. A null live evhttp while we are
// running IS a re-register, because the listener we hold has gone away and the state must stop
// claiming it is up -- Start() then refuses with its own reason and the panel goes dark.
inline bool NeedsReregister(bool running, const void* boundNetGame, const void* boundEvhttp,
                            const void* liveNetGame, const void* liveEvhttp) {
    if (liveNetGame == nullptr) return false;
    if (!running) return true;
    return liveNetGame != boundNetGame || liveEvhttp != boundEvhttp;
}

// Drops the latch WITHOUT unregistering. After a rebuild the old listener may be freed or reused,
// and calling the engine's unregister through it is a call into dead memory (s1.4).
inline void DropLatch(Registration* reg) { *reg = Registration{}; }

// Stop() may only unregister through the identity it registered against.
inline bool CanUnregister(const Registration& reg, const void* liveNetGame,
                          const void* liveEvhttp) {
    if (!reg.running || reg.listener == nullptr) return false;
    // A null live netGame at shutdown means the session is already gone; the listener went with it.
    if (liveNetGame == nullptr) return false;
    return liveNetGame == reg.netGame && liveEvhttp == reg.evhttp;
}

// The engine's own prototypes, five arguments and five: the register call's fifth lands on the
// stack and the unregister call returns an int nothing reads. Both are declared here exactly as
// http.cpp resolves them, so a fake in a test is the same shape as the real one.
using RouteRegisterFn = int (*)(void* listener, const char* uri, void (*cb)(void*, void*),
                                void* cbdata, unsigned methods);
using RouteUnregisterFn = int (*)(void* listener, const char* uri);

struct Route {
    const char* uri;
    void (*handler)(void*, void*);
    unsigned methods;
};

// Registers every route and records the identity when at least one took. Returns how many took.
//
// rc != 0 is "a callback for that exact URI already exists", and the callee does NOT replace it --
// so a redundant attempt is a no-op, and a fresh route table is exactly the case where every call
// returns 0. `already` counts the refusals so a caller can tell "I just re-armed a rebuilt table"
// from "these were still live".
inline size_t Apply(Registration* reg, RouteRegisterFn fn, void* listener, const void* netGame,
                    const void* evhttp, const Route* routes, size_t count, size_t* already) {
    size_t registered = 0;
    size_t refused = 0;
    for (size_t i = 0; i < count; ++i) {
        if (fn(listener, routes[i].uri, routes[i].handler, nullptr, routes[i].methods) != 0) {
            ++refused;
            continue;
        }
        ++registered;
    }
    if (already != nullptr) *already = refused;
    if (registered == 0) return 0;
    reg->running = true;
    reg->listener = listener;
    reg->netGame = netGame;
    reg->evhttp = evhttp;
    return registered;
}

// The other end. Unregisters only when the identity still matches, and clears the record either
// way -- a caller that cannot prove the listener is the one it registered on must still forget it.
inline bool Release(Registration* reg, RouteUnregisterFn fn, const void* liveNetGame,
                    const void* liveEvhttp, const char* const* uris, size_t count) {
    const bool safe = CanUnregister(*reg, liveNetGame, liveEvhttp) && fn != nullptr;
    if (safe) {
        for (size_t i = 0; i < count; ++i) fn(reg->listener, uris[i]);
    }
    DropLatch(reg);
    return safe;
}

}  // namespace http
}  // namespace control
}  // namespace replay
