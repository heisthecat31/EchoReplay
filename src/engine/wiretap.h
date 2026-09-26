#pragma once

// The stock per-symbol tap on the engine's game-message bus (NRadEngine::CBroadcaster).
// Level 0: one stock call in, one stock call out, no patched bytes, no detour.
//
// Proposed as an Appendix B contract; W1-L owns it, W2-A consumes it for S_NETVAR and
// W3-E for S_EVENT.  See docs/spikes/W1-L.md.

#include <cstdint>

namespace replay {

// Microseconds on the tap's own arrival clock: one monotonic basis for the whole process, so two
// taps' stamps are comparable and a consumer can hold them on a single axis.  Zero only on a
// machine with no performance counter.  Read it on the tick thread to relate an arrival stamp to
// whatever clock the caller keeps its own records on.
uint64_t WiretapArrivalNowUs();

// One dispatch, exactly as CBroadcaster hands it over.  `data` is borrowed from the engine
// and is valid only for the duration of the call: copy what you keep.
struct WireMessage {
    // Every field is a constructor argument on purpose.  The tap that fills this in is the only
    // place `arrivalUs` can come from, and a message that reached a consumer without one recorded
    // an engine register as a timestamp for the life of the file.
    WireMessage(const void* dataIn, uint64_t bytesIn, uint64_t arrivalUsIn, uint64_t stampIn,
                uint64_t peerIn)
        : data(dataIn), bytes(bytesIn), arrivalUs(arrivalUsIn), stamp(stampIn), peer(peerIn) {}

    const void* data;
    uint64_t    bytes;

    // When the tap saw this dispatch, in microseconds on WiretapArrivalNowUs()'s basis.  Set for
    // every message of every registered kind, at the one point they all pass through, and
    // non-decreasing across the process.  This is the only time value on this struct.
    uint64_t arrivalUs;

    // The engine's per-dispatch qword, forwarded verbatim.  It is NOT a time and must never be
    // read as one: ReceiveLocalEvent passes all ones for it and the network path's value was never
    // traced to a definition (docs/spikes/W1-L.md S9.2 leaves it uninterpreted).  Stored as
    // microseconds by an earlier consumer it produced arrival times of 6, 34, 36, 65, 66,
    // 16369959894457074595 and 0xFFFFFFFFFFFFFFFF in one recorded match.
    uint64_t stamp;

    uint64_t peer;  // SPeer
};

// May run on a thread other than the tick, so it must not block.  §6.1's ring push is the
// intended body; anything that allocates, locks or touches the filesystem belongs on the
// worker side of that ring.
using WiretapFn = void (*)(const WireMessage&, void* ctx);

class IWiretap {
public:
    static constexpr uint16_t kInvalidHandle = 0xFFFF;

    virtual ~IWiretap() = default;

    // False when the engine did not check out.  Nothing else on this interface does
    // anything while it is false, and Diagnostic() says which check refused.
    virtual bool        Available() const = 0;
    virtual const char* Diagnostic() const = 0;

    // CMatSym for a message struct named as the binary spells it, leading 'S' included:
    // "SR15NetComponentEvent".  Zero if unavailable.
    virtual uint64_t Symbol(const char* structName) const = 0;

    // kInvalidHandle on failure, including when called from inside a callback: the engine
    // has no deferral for registration the way it does for removal.
    virtual uint16_t Listen(const char* structName, WiretapFn fn, void* ctx) = 0;
    virtual uint16_t ListenSymbol(uint64_t symbol, WiretapFn fn, void* ctx) = 0;

    // Safe during a dispatch: the engine marks the node skipped rather than unlinking it.
    // Still prefer the tick, because a node retired that way is never returned to the pool.
    virtual void Unlisten(uint16_t handle) = 0;
    virtual void UnlistenAll() = 0;

    virtual uint32_t ActiveCount() const = 0;

    // How many registrations were refused because the engine's CR15NetGame listener lock was still
    // held when the deadline passed. Every one of them left the engine exactly as it was, so the
    // caller's answer is to try again on a later tick; Diagnostic() then reads `wiretap-lock-timeout`.
    virtual uint32_t LockTimeouts() const = 0;
};

// `netGame` is TickInfo::netGame (CR15NetGame*).  Null only if the allocation failed;
// otherwise always check Available() before using the result.
IWiretap* WiretapCreate(void* netGame);

// The same thing against an engine image at a stated base, rather than the one this DLL is
// loaded into.  tests/wiretap's offline host maps echovr.exe itself and passes that base, so
// registration runs through the engine's own CBroadcaster::Listen with no game running.  The
// entry-point and known-answer checks are the same ones WiretapCreate applies.
IWiretap* WiretapCreateAt(void* netGame, const void* engineBase);

void WiretapDestroy(IWiretap* tap);

}  // namespace replay
