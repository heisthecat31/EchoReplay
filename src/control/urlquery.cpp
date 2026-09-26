#include "control/urlquery.h"

#include <string.h>

namespace replay {
namespace control {

bool PercentDecode(const char* in, size_t inLen, char* out, size_t cap) {
    if (in == nullptr || out == nullptr || cap == 0) {
        return false;
    }
    size_t w = 0;
    for (size_t i = 0; i < inLen; ++i) {
        char c = in[i];
        if (c == '+') {
            c = ' ';   // the form-encoding convention, which every client library emits
        } else if (c == '%') {
            if (i + 2 >= inLen) {
                return false;   // a truncated escape, not a literal percent
            }
            int hi = 0, lo = 0;
            const char a = in[i + 1];
            const char b = in[i + 2];
            if (a >= '0' && a <= '9') hi = a - '0';
            else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
            else return false;
            if (b >= '0' && b <= '9') lo = b - '0';
            else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
            else return false;
            c = static_cast<char>((hi << 4) | lo);
            i += 2;
        }
        // Refused rather than truncated: half a command is a different command.
        if (w + 1 >= cap) {
            return false;
        }
        out[w++] = c;
    }
    out[w] = 0;
    return true;
}

bool QueryValue(const char* uri, const char* key, char* out, size_t cap) {
    if (uri == nullptr || key == nullptr || out == nullptr || cap == 0) {
        return false;
    }
    const char* q = strchr(uri, '?');
    if (q == nullptr) {
        return false;
    }
    const size_t keyLen = strlen(key);
    const char* p = q + 1;
    while (*p != 0) {
        const char* amp = strchr(p, '&');
        const char* pairEnd = amp != nullptr ? amp : p + strlen(p);
        const char* eq = static_cast<const char*>(memchr(p, '=', static_cast<size_t>(pairEnd - p)));
        if (eq != nullptr && static_cast<size_t>(eq - p) == keyLen &&
            memcmp(p, key, keyLen) == 0) {
            return PercentDecode(eq + 1, static_cast<size_t>(pairEnd - eq - 1), out, cap);
        }
        if (amp == nullptr) {
            break;
        }
        p = amp + 1;
    }
    return false;
}

}  // namespace control
}  // namespace replay
