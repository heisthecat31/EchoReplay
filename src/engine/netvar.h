#pragma once

// B.11.2 `INetVar` (E-5) -- the replicated-variable surface. Owned by W2-A; consumed by W2-B.
//
// ---------------------------------------------------------------------------------------------
// ★ EVENT-SOURCED, NEVER POLLED, AND THAT IS A CORRECTNESS REQUIREMENT.
//
// The measured DUV traffic is 118 Hz combined against a 72-120 Hz frame rate (R4 S7), so a capture that
// polled the store each tick would be STRUCTURALLY unable to observe every change -- and S5.4.5 declares
// S_NETVAR retention ALL, which a poller cannot honour. S6.2.1 is explicit: "a stream declaring
// retention ALL may not be polled, because a poller cannot observe what it does not sample."
//
// So this binds W1-L's CBroadcaster tap to SR15NetReplicatedDeltaUnreliableVars and every value it ever
// reports came off a message. Poll() below drains what the tap delivered; it does not read the store.
//
// ★ THE THREE THINGS THIS CLASS WILL NOT DO, each because doing it would be a guess:
//
//  1. It never RE-ENCODES an SValue. The client's typed getter branches on the encoding rather than on
//     the value, and re-encoding an "equivalent" is the documented cause of a pause-overlay regression
//     (R4 S1.1). Values are 8-byte qwords in and 8-byte qwords out.
//  2. Write() REFUSES on this build. Applying a netvar means either a store setter or a way to
//     re-dispatch a DUV message, and neither is established on PC -- no fact registry row names one.
//     Writing to a guessed address inside a running game is the one experiment this packet will not
//     run. Diagnostic() says exactly that, and the capture stores every DUV message VERBATIM so the
//     mechanism can land later against files that already exist.
//  3. Read() answers from a MIRROR of the messages the tap delivered, not from engine memory. That is
//     the value the client holds -- the engine applied the same message -- but it is only complete once
//     the baseline rowset has arrived, and BaselineSeen() says whether it has. A reader that treats an
//     absent name as a zero would be inventing state; Read() returns false instead.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

namespace replay {

class IWiretap;

using Symbol64 = uint64_t;

struct NetVarWrite {
    Symbol64 name;
    uint64_t rawSValue;   // verbatim, never re-encoded
    uint8_t slot;
};

class INetVar {
public:
    // Changes since the last Poll, in arrival order. Returns how many were written.
    virtual size_t Poll(NetVarWrite* out, size_t cap) = 0;
    // Returns whether the write LANDED, not whether it was attempted. W2-B asked for this, and the
    // reason is the important part: a void return makes success unobservable through the interface,
    // so a caller can only count attempts -- which is why the one netvar observable reported success
    // for a no-op.
    //
    // On this build it always returns false. No apply path is established on PC: no fact row names a
    // netvar store setter or a way to re-dispatch a DUV message, and a write to a guessed offset
    // inside a running game corrupts state that looks fine until a HUD gate reads it.
    virtual bool Write(const NetVarWrite&) = 0;
    virtual bool Read(Symbol64 name, uint8_t slot, uint64_t& rawSValue) const = 0;
    // The engine's own numbering: the DUV packet sequence, counting packets the engine numbered
    // whether or not we saw them. NEVER our own delivery tally -- that is S5.6.2's hole 2.
    virtual size_t ObservedCount() const = 0;

protected:
    ~INetVar() = default;
};

namespace netvar {

// Why the surface is not fully live. "It did not work" is not a diagnosis, so every refusal that can
// happen has its own value, as tick::ClaimResult and entity::Resolve do.
enum class Status : uint8_t {
    Ok = 0,
    NoWiretap,        // the caller passed null
    TapUnavailable,   // IWiretap::Available() is false; its Diagnostic() says why
    ListenPending,    // the one Listen has not been attempted yet: never asked, which is not a refusal
    ListenRefused,    // BroadcasterListen returned kInvalidHandle
    WriteUnsupported, // Write() was called; there is no established apply path on this build
};
const char* StatusText(Status);

struct Stats {
    uint64_t messages;        // DUV messages the tap delivered
    uint64_t rows;            // rows inside them; every one is stored
    uint64_t malformed;       // bodies that did not parse; stored verbatim regardless
    uint64_t observedPackets; // from the packet sequence: delivered plus provably missing
    uint64_t missingPackets;  // sequence jumps -- the engine numbered them, we never saw them
    uint64_t duplicateSeq;    // two rowsets in one packet, which the baseline pair does
    uint64_t regressions;     // the sequence went backwards; non-zero means it is not what we think
    uint64_t dropped;         // rows the internal queue could not hold; counted, never silent
    uint64_t writeRefusals;
    uint32_t callbackThreadId;  // measured, because W1-L could not establish it statically
    uint32_t tickThreadId;
};

// The raw message, handed on before anything is interpreted. This is what the capture stores: the
// completeness of S_NETVAR rests on the packet number inside the body, so the body travels whole.
using MessageFn = void (*)(const void* body, uint32_t bytes, uint64_t stampUs, void* ctx);

// `tap` is W1-L's IWiretap, already created against TickInfo::netGame. Create does NOT register the
// listener -- see TryListen -- and Destroy removes it; Listen from inside a callback is refused by the
// tap itself, so registration happens on the tick like every other teardown-sensitive call.
INetVar* Create(IWiretap* tap, MessageFn onMessage, void* ctx);
void Destroy(INetVar*);

// Attempts the single Listen this surface needs and answers what happened. A no-op once the answer is
// final (Ok, NoWiretap, TapUnavailable); ListenRefused is NOT final, so the owner calls again on a
// later tick.
//
// WHY THIS IS NOT DONE IN Create. That Listen takes the same CR15NetGame listener lock the tap walk
// takes, with the same 2000 us deadline, on the tick this surface is built -- the state-9 transition,
// the most contended tick of a session. One busy frame there lost S_NETVAR for the whole match, and a
// surface built during a walk that had already spent the frame's deadline would spend a second one.
// Both go away when one owner drives every registration on the tick: capture's RegisterSurfaces.
Status TryListen(INetVar*);

Status   StatusOf(const INetVar*);
void     GetStats(const INetVar*, Stats* out);
bool     BaselineSeen(const INetVar*);
// Records which thread the tap dispatched on, so the capture can report it rather than assume it.
void     NoteTickThread(INetVar*, uint32_t threadId);

}  // namespace netvar
}  // namespace replay
