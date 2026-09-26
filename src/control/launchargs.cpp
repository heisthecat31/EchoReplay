#include "control/launchargs.h"

#include <windows.h>

namespace replay {
namespace control {
namespace {

// A quote toggles literal mode and is not itself part of the token. That is the engine's own
// tokeniser's shape rather than CommandLineToArgvW's: CMDLINE.TOKENIZE splits on 0x20, and the
// argv rules' backslash-before-quote escaping would mangle a directory path ending in a separator.
struct RawToken {
    // Two spare over the path limit so a token one character too long is still seen in full.
    wchar_t text[kMaxPathChars + 8];
    size_t  len = 0;
    bool    unterminated = false;
};

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

wchar_t LowerAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// '-' and '+' are the two a user types. 0x96 is the third the engine's tokeniser accepts, and
// session::HasToken already honours all three -- a launcher passing the switch the way the engine
// would have accepted it must not slip past the warning because only our reader disagrees.
bool IsSwitchPrefix(wchar_t c) {
    return c == L'-' || c == L'+' || c == 0x0096;
}

bool NextToken(const wchar_t** cursor, RawToken* out) {
    *out = RawToken();
    const wchar_t* p = *cursor;
    while (*p != 0 && IsSpace(*p)) {
        ++p;
    }
    if (*p == 0) {
        *cursor = p;
        return false;
    }
    bool inQuotes = false;
    const size_t cap = sizeof out->text / sizeof out->text[0];
    while (*p != 0) {
        if (*p == L'"') {
            inQuotes = !inQuotes;
            ++p;
            continue;
        }
        if (!inQuotes && IsSpace(*p)) {
            break;
        }
        if (out->len + 1 < cap) {
            out->text[out->len++] = *p;
        }
        ++p;
    }
    out->text[out->len] = 0;
    out->unterminated = inQuotes;
    *cursor = p;
    return true;
}

// Case-insensitive over ASCII, which is all a switch name is.
bool EqualsIgnoringCase(const wchar_t* a, size_t aLen, const wchar_t* b) {
    size_t i = 0;
    for (; i < aLen; ++i) {
        if (b[i] == 0 || LowerAscii(a[i]) != LowerAscii(b[i])) {
            return false;
        }
    }
    return b[i] == 0;
}

size_t Length(const wchar_t* s) {
    size_t n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}

bool EndsWithIgnoringCase(const wchar_t* s, size_t len, const wchar_t* suffix) {
    const size_t sufLen = Length(suffix);
    if (len < sufLen) {
        return false;
    }
    const wchar_t* tail = s + (len - sufLen);
    for (size_t i = 0; i < sufLen; ++i) {
        if (LowerAscii(tail[i]) != LowerAscii(suffix[i])) {
            return false;
        }
    }
    return true;
}

// Rooted, not merely rooted-looking: `C:file.echodemo` is drive-relative and resolves against
// whatever that drive's current directory happens to be, which for a game launched by a shortcut is
// not something the owner chose.
bool IsAbsolute(const wchar_t* s, size_t len) {
    if (len >= 2 && (s[0] == L'\\' || s[0] == L'/') && (s[1] == L'\\' || s[1] == L'/')) {
        return true;   // UNC
    }
    const bool drive = (s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z');
    return len >= 3 && drive && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/');
}

const wchar_t kLegacySwitchName[] = L"replayviewer";

// Trims ASCII whitespace and one matched pair of surrounding quotes. A launcher that interpolates a
// path into a batch file leaves both behind, and neither can occur in a Windows file name, so
// removing them cannot change which file is named.
void Tidy(const wchar_t** begin, size_t* len) {
    const wchar_t* p = *begin;
    size_t n = *len;
    while (n > 0 && IsSpace(*p)) {
        ++p;
        --n;
    }
    while (n > 0 && IsSpace(p[n - 1])) {
        --n;
    }
    if (n >= 2 && p[0] == L'"' && p[n - 1] == L'"') {
        ++p;
        n -= 2;
        while (n > 0 && IsSpace(*p)) {
            ++p;
            --n;
        }
        while (n > 0 && IsSpace(p[n - 1])) {
            --n;
        }
    }
    *begin = p;
    *len = n;
}

void Refuse(OpenRequest* out, OpenRefusal reason) {
    out->hasPath = false;
    out->mostRecent = false;
    out->refusal = reason;
}

}  // namespace

const char* OpenRefusalText(OpenRefusal reason) {
    switch (reason) {
        case OpenRefusal::None:           return "no refusal";
        case OpenRefusal::TooLong:        return "the path is longer than this build can hold";
        case OpenRefusal::NotAbsolute:    return "the path is not absolute";
        case OpenRefusal::WrongExtension: return "the path does not name a replay (.echodemo, "
                                                 ".echoreplay or .tape)";
        case OpenRefusal::Missing:        return "no such file";
        case OpenRefusal::NotAFile:       return "that path is a directory, not a capture";
        case OpenRefusal::Unreadable:     return "the file is there but could not be opened for "
                                                 "reading";
    }
    return "unknown";
}

bool ReadOpenRequest(OpenRequest* out) {
    if (out == nullptr) {
        return false;
    }
    *out = OpenRequest();

    wchar_t value[kMaxPathChars + 8];
    const DWORD cap = static_cast<DWORD>(sizeof value / sizeof value[0]);
    const DWORD n = ::GetEnvironmentVariableW(kOpenEnvVar, value, cap);
    if (n == 0) {
        return false;   // not set, or set to nothing, which this treats the same way
    }
    out->present = true;
    if (n >= cap) {
        // n is the size the value needs, including its terminator: it did not fit, and a value that
        // did not fit names a different file than the one that was meant.
        Refuse(out, OpenRefusal::TooLong);
        return true;
    }
    value[n] = 0;

    const wchar_t* text = value;
    size_t len = n;
    Tidy(&text, &len);
    if (len == 0) {
        Refuse(out, OpenRefusal::Missing);
        return true;
    }
    if (len + 1 > kMaxPathChars) {
        Refuse(out, OpenRefusal::TooLong);
        return true;
    }
    for (size_t i = 0; i < len; ++i) {
        out->path[i] = text[i];
    }
    out->path[len] = 0;

    if (EqualsIgnoringCase(out->path, len, kOpenMostRecent)) {
        out->mostRecent = true;
        return true;
    }
    if (!IsAbsolute(out->path, len)) {
        Refuse(out, OpenRefusal::NotAbsolute);
        return true;
    }
    if (!EndsWithIgnoringCase(out->path, len, kCaptureExtension) &&
        !EndsWithIgnoringCase(out->path, len, kLegacyReplayExtension) &&
        !EndsWithIgnoringCase(out->path, len, kLegacyTapeExtension)) {
        Refuse(out, OpenRefusal::WrongExtension);
        return true;
    }
    const DWORD attrs = ::GetFileAttributesW(out->path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        Refuse(out, OpenRefusal::Missing);
        return true;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        Refuse(out, OpenRefusal::NotAFile);
        return true;
    }
    // Readable is asked, not assumed. FILE_SHARE_WRITE and _DELETE are granted because this only
    // asks the question -- the transport opens the file for itself later, and holding it exclusively
    // for the length of one test would be this reader deciding who else may touch it.
    const HANDLE h = ::CreateFileW(out->path, GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Refuse(out, OpenRefusal::Unreadable);
        return true;
    }
    ::CloseHandle(h);
    out->hasPath = true;
    return true;
}

bool CommandLineMentionsReplayViewer(const wchar_t* commandLine) {
    if (commandLine == nullptr) {
        return false;
    }
    const wchar_t* cursor = commandLine;
    RawToken tok;
    while (NextToken(&cursor, &tok)) {
        if (tok.len >= 2 && IsSwitchPrefix(tok.text[0])) {
            const wchar_t* body = tok.text + 1;
            size_t bodyLen = tok.len - 1;
            for (size_t i = 0; i < bodyLen; ++i) {
                if (body[i] == L'=') {
                    bodyLen = i;   // the attached form carries its value on the same token
                    break;
                }
            }
            if (EqualsIgnoringCase(body, bodyLen, kLegacySwitchName)) {
                return true;
            }
        }
        if (tok.unterminated) {
            return false;   // an unterminated quote swallowed the rest of the line into this token
        }
    }
    return false;
}

}  // namespace control
}  // namespace replay
