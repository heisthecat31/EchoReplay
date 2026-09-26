#pragma once

#include <cstddef>
#include <cstring>

// One JSON string escape for every emitter in this build.
//
// L5 (`20260902-081617Z`) wrote `sc7_text_diff.json` with a module path interpolated raw, so the
// document carried `\P` at offset 2156 and no parser would read it. The dangerous half is the rest of
// that same path: `\r` in `ready-at-dawn` and `\b` in `bin` are VALID escapes, so a lenient reader
// would not have refused the file - it would have handed its consumer a silently corrupted path. Any
// field that can hold a path, a filename, a module name or any other text this process did not author
// goes through here.

namespace replay {
namespace json {

// Policy - one document encoding, stated once:
//   * `\` and `"` are backslash-escaped;
//   * 0x08 0x09 0x0A 0x0C 0x0D take their short forms `\b \t \n \f \r`;
//   * every other byte below 0x20 becomes `\u00xx` - including 0x00, which is a control character and
//     not a terminator once the length is known (see AppendEscapedN);
//   * 0x7F is copied verbatim, which JSON permits;
//   * a WELL-FORMED UTF-8 sequence (Unicode 15 table 3-7: no overlong form, no surrogate, nothing
//     above U+10FFFF) is copied VERBATIM, so a multi-byte character survives byte-exact rather than
//     being split into `\u00xx` halves no reader can put back together;
//   * an ILL-FORMED byte sequence becomes one U+FFFD per maximal subpart, written as the six ASCII
//     bytes `\ufffd` so the document stays valid whatever the input was. AUDIT_L5FIX W1C-3: the
//     old policy copied every byte >= 0x80 through, so an invalid sequence produced a document that
//     parsed and handed its consumer a mangled value - the exact harm this routine exists to stop.
//     Replacement is lossy and says so in the document; passthrough was lossy and silent. Callers that
//     narrow with `CP_UTF8` cannot produce an ill-formed sequence, so no live caller is affected.
//   * `/` is not escaped, which JSON does not require.
//
// SATURATION. AUDIT_L5FIX W1C-2: clamping to `cap - 1` and reporting success writes a truncated
// document, and a truncated JSON document is invalid JSON. So a value that does not fit is not
// written short - the routine returns `cap`, which no in-range cursor can equal (a document that fits
// ends at `cap - 1` at the very most, one byte of which is the terminator). Every append here and in
// the emitters is a no-op once `at == cap`, so the signal survives to the end of the document, where
// the emitter turns it into a refusal. The buffer's tail is undefined after saturation; the caller
// must not write it out.

// The `\u00xx` and `\ufffd` forms - the widest thing this routine emits for one input character.
constexpr size_t kMaxEscapeBytes = 6;

// Escapes `len` bytes of `s` into `buf` at `at` and returns the new offset, or `cap` if the value did
// not fit. The quotes are the caller's. Use this form whenever the value's length is known
// independently of a terminator: `AppendEscaped` below cannot see a byte past the first NUL, because
// that is what `const char*` means, and a caller holding a byte range must not lose the tail silently.
inline size_t AppendEscapedN(char* buf, size_t cap, size_t at, const char* s, size_t len) {
    if (buf == nullptr || cap == 0) {
        return at;
    }
    if (at >= cap) {
        return cap;
    }
    static const char kHex[] = "0123456789abcdef";
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s != nullptr ? s : "");
    const size_t room = cap - 1;   // the last byte belongs to the terminator
    if (s == nullptr) {
        len = 0;
    }
    for (size_t i = 0; i < len;) {
        char esc[kMaxEscapeBytes];
        size_t n = 0;
        size_t consumed = 1;
        const unsigned char b = p[i];
        switch (b) {
            case '\\': esc[0] = '\\'; esc[1] = '\\'; n = 2; break;
            case '"':  esc[0] = '\\'; esc[1] = '"';  n = 2; break;
            case '\b': esc[0] = '\\'; esc[1] = 'b';  n = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  n = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  n = 2; break;
            case '\f': esc[0] = '\\'; esc[1] = 'f';  n = 2; break;
            case '\r': esc[0] = '\\'; esc[1] = 'r';  n = 2; break;
            default:
                if (b < 0x20) {
                    esc[0] = '\\';
                    esc[1] = 'u';
                    esc[2] = '0';
                    esc[3] = '0';
                    esc[4] = kHex[b >> 4];
                    esc[5] = kHex[b & 0x0F];
                    n = 6;
                } else if (b < 0x80) {
                    esc[0] = static_cast<char>(b);
                    n = 1;
                } else {
                    // Unicode 15 table 3-7, the only byte sequences that are well-formed UTF-8. The
                    // second byte's range is what rejects the overlongs (C0 C1, E0 80, F0 80), the
                    // surrogates (ED A0) and everything above U+10FFFF (F4 90, F5..FF).
                    unsigned char lo = 0x80;
                    unsigned char hi = 0xBF;
                    size_t trail = 0;
                    if (b >= 0xC2 && b <= 0xDF) { trail = 1; }
                    else if (b == 0xE0) { trail = 2; lo = 0xA0; }
                    else if (b >= 0xE1 && b <= 0xEC) { trail = 2; }
                    else if (b == 0xED) { trail = 2; hi = 0x9F; }
                    else if (b >= 0xEE && b <= 0xEF) { trail = 2; }
                    else if (b == 0xF0) { trail = 3; lo = 0x90; }
                    else if (b >= 0xF1 && b <= 0xF3) { trail = 3; }
                    else if (b == 0xF4) { trail = 3; hi = 0x8F; }

                    size_t ok = 0;
                    while (ok < trail && i + 1 + ok < len) {
                        const unsigned char c = p[i + 1 + ok];
                        const unsigned char clo = (ok == 0) ? lo : 0x80;
                        const unsigned char chi = (ok == 0) ? hi : 0xBF;
                        if (c < clo || c > chi) {
                            break;
                        }
                        ++ok;
                    }
                    if (trail != 0 && ok == trail) {
                        for (size_t k = 0; k <= trail; ++k) {
                            esc[k] = static_cast<char>(p[i + k]);
                        }
                        n = trail + 1;
                        consumed = trail + 1;
                    } else {
                        // One replacement per maximal subpart: a truncated three-byte character is one
                        // ruined character, not two or three.
                        esc[0] = '\\'; esc[1] = 'u'; esc[2] = 'f'; esc[3] = 'f'; esc[4] = 'f';
                        esc[5] = 'd';
                        n = 6;
                        consumed = (trail != 0 && ok != 0) ? ok + 1 : 1;
                    }
                }
                break;
        }
        if (at + n > room) {
            buf[at] = '\0';
            return cap;   // did not fit - never a prefix, never a split escape
        }
        for (size_t k = 0; k < n; ++k) {
            buf[at++] = esc[k];
        }
        i += consumed;
    }
    buf[at] = '\0';
    return at;
}

// The NUL-terminated form. A `const char*` value ends at its first NUL by definition of the type, so
// this cannot report a truncation the caller did not already accept when it chose that type; a caller
// holding a byte range calls AppendEscapedN instead.
inline size_t AppendEscaped(char* buf, size_t cap, size_t at, const char* s) {
    return AppendEscapedN(buf, cap, at, s, s != nullptr ? std::strlen(s) : 0);
}

}  // namespace json
}  // namespace replay
