#pragma once

// W2-G. Percent-decoding for the HTTP surface's query string.
//
// It lives here rather than in http.cpp because it is pure and it has real failure modes -- a
// truncated escape, a non-hex escape, a value that does not fit -- and http.cpp cannot be linked
// into a test on a machine with no game. Every one of those cases is a way for a command to arrive
// as a DIFFERENT command than the one that was sent, which is worth a test rather than a comment.

#include <stddef.h>

namespace replay {
namespace control {

// Decodes `%XX` and `+`. Writes at most `cap` bytes including the terminator.
//
// * REFUSES RATHER THAN PARTIALLY DECODES. A truncated or non-hex escape, or a result that does not
// fit, returns false and leaves nothing to be acted on. Half a command line is a valid command line
// that says something else -- `load a.echodemo` truncated to `load a` opens a different file, or
// nothing, and the user is told neither.
bool PercentDecode(const char* in, size_t inLen, char* out, size_t cap);

// Finds `key` in a `path?a=1&b=2` query string and decodes its value. False when there is no query,
// no such key, or the value would not decode. Matches the key as a WHOLE name, so `line` does not
// match `linewidth`.
bool QueryValue(const char* uri, const char* key, char* out, size_t cap);

}  // namespace control
}  // namespace replay
