#pragma once

// A readability probe, and nothing else.
//
// Every pointer this subsystem follows into the engine -- the sink list, CSoundPlayer, the memo
// containers, an XAudio2 vtable -- is a pointer whose target we did not allocate and whose validity
// is a claim rather than a guarantee. The design's rule is that a mismatch is a clean refusal
// with a log line, never a corruption; a refusal is only possible if the read can be attempted
// safely, so every dereference is gated on this.
//
// It contains no engine knowledge at all -- no address, no offset, no struct -- which is why an L2
// binding (src/engine/audiostate.cpp) including it does not make that file depend on the L3 audio
// module. It is a Win32 helper that happens to live beside its first caller.
//
// The probe itself is bootstrap/pageprobe.h's touch, not a VirtualQuery. PERF-4 measured the query
// at about 100 us in this process and ~10,000 calls a second across the plugin; the touch answers
// the same question in nanoseconds, and about a nearer instant -- a query describes the map as it
// was when the query ran, and the caller's read happens afterwards either way.

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include "bootstrap/pageprobe.h"
#endif

namespace replay {
namespace audio {

// True when [p, p+bytes) is entirely committed, readable, and not a guard page. A zero-length span
// over a null pointer is false: "nothing to read" must not read as "safe to read".
inline bool Readable(const void* p, size_t bytes) {
#if defined(_WIN32)
    if (p == nullptr || bytes == 0) {
        return false;
    }
    return PageSpanReadable(p, bytes);
#else
    (void)p;
    (void)bytes;
    return false;
#endif
}

// Reads a T out of engine memory, or leaves `out` untouched and returns false. The value is copied
// rather than referenced so no caller ends up holding a pointer into a page that was readable once.
template <class T>
inline bool ReadAt(uintptr_t address, T* out) {
    if (out == nullptr || !Readable(reinterpret_cast<const void*>(address), sizeof(T))) {
        return false;
    }
    *out = *reinterpret_cast<const T*>(address);
    return true;
}

}  // namespace audio
}  // namespace replay
