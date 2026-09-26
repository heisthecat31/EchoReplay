#include "control/notes.h"

#include "bootstrap/md5.h"
#include "foundation/demofile/format.h"

#include <windows.h>

#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

namespace replay {
namespace control {
namespace notes {
namespace {

const char kSuffix[] = ".notes.json";
// A notes file is a few hundred bookmarks of user text. Anything larger is not one, and reading it
// would only be a way to spend memory on a file we are going to refuse.
constexpr size_t kMaxFileBytes = 1u << 20;

// `.%08X.tmp` -- the process id, at a fixed width so the length of the temporary path is a constant
// this file can count rather than a number that varies with the pid. Two processes marking the same
// capture used to open the same `<sidecar>.tmp` for writing and truncate each other before either
// renamed.
constexpr size_t kTempSuffixChars = 13;

// MAX_PATH, minus the terminator, minus both suffixes. _wfopen_s refuses past MAX_PATH whatever the
// caller's buffer is, and Save appends its temporary suffix AFTER every length check anyone else
// could make -- so the ceiling has to be measured here, once, against the longest name this file
// will ever ask the CRT to open.
// `.stale-` and thirty-two hex digits, appended to the sidecar path when a sidecar is moved aside.
constexpr size_t kAsideSuffixChars = 7 + 32;

const char kHexDigits[] = "0123456789abcdef";

void AppendHex16(std::string* out, const uint8_t v[16]) {
    out->push_back('"');
    for (size_t i = 0; i < 16; ++i) {
        out->push_back(kHexDigits[v[i] >> 4]);
        out->push_back(kHexDigits[v[i] & 0x0F]);
    }
    out->push_back('"');
}

int HexNibble(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return static_cast<int>(ch - L'0');
    if (ch >= L'a' && ch <= L'f') return static_cast<int>(ch - L'a') + 10;
    if (ch >= L'A' && ch <= L'F') return static_cast<int>(ch - L'A') + 10;
    return -1;
}

// Exactly thirty-two digits and then the end. A shorter run left-padded with zeroes would read as a
// different identity that happens to match, which is the one thing this field exists to prevent.
bool ParseHex16(const wchar_t* s, uint8_t out[16]) {
    for (size_t i = 0; i < 16; ++i) {
        const int hi = HexNibble(s[i * 2]);
        const int lo = HexNibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return s[32] == 0;
}

constexpr size_t kMaxDemoChars = MAX_PATH - 1 - (sizeof kSuffix - 1) - kTempSuffixChars;

size_t WideLen(const wchar_t* s) {
    size_t n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}

// ---- UTF-8, both directions ------------------------------------------------------------------
//
// Hand-written rather than WideCharToMultiByte because the same file has to DECODE, and the decoder
// must reject malformed input rather than substitute U+FFFD: a label that silently changes when it
// round trips is a defect nobody would ever report.

void AppendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Reads one code point starting at `p`. False on any malformed sequence.
bool DecodeUtf8(const char* p, const char* end, uint32_t* cp, size_t* used) {
    const auto b0 = static_cast<uint8_t>(*p);
    size_t need = 0;
    uint32_t v = 0;
    if (b0 < 0x80) {
        *cp = b0;
        *used = 1;
        return true;
    }
    if ((b0 & 0xE0) == 0xC0) {
        need = 1;
        v = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2;
        v = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3;
        v = b0 & 0x07u;
    } else {
        return false;   // a continuation byte or an over-long lead
    }
    if (static_cast<size_t>(end - p) < need + 1) {
        return false;
    }
    for (size_t i = 1; i <= need; ++i) {
        const auto b = static_cast<uint8_t>(p[i]);
        if ((b & 0xC0) != 0x80) {
            return false;
        }
        v = (v << 6) | (b & 0x3Fu);
    }
    // Over-long encodings and the surrogate range are refused: both are ways to smuggle a second
    // spelling of the same character past a comparison.
    if ((need == 1 && v < 0x80) || (need == 2 && v < 0x800) || (need == 3 && v < 0x10000)) {
        return false;
    }
    if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        return false;
    }
    *cp = v;
    *used = need + 1;
    return true;
}

// Appends a code point to a UTF-16 buffer, as a surrogate pair when it needs one.
bool AppendUtf16(uint32_t cp, wchar_t* out, size_t cap, size_t* len) {
    if (cp < 0x10000) {
        if (*len + 1 >= cap) {
            return false;
        }
        out[(*len)++] = static_cast<wchar_t>(cp);
        return true;
    }
    if (*len + 2 >= cap) {
        return false;
    }
    const uint32_t v = cp - 0x10000;
    out[(*len)++] = static_cast<wchar_t>(0xD800 + (v >> 10));
    out[(*len)++] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
    return true;
}

void AppendJsonString(std::string* out, const wchar_t* s) {
    out->push_back('"');
    for (size_t i = 0; s[i] != 0; ++i) {
        const auto u = static_cast<uint32_t>(static_cast<uint16_t>(s[i]));
        switch (u) {
            case '"':  out->append("\\\""); continue;
            case '\\': out->append("\\\\"); continue;
            case '\b': out->append("\\b");  continue;
            case '\f': out->append("\\f");  continue;
            case '\n': out->append("\\n");  continue;
            case '\r': out->append("\\r");  continue;
            case '\t': out->append("\\t");  continue;
            default: break;
        }
        if (u < 0x20) {
            char esc[7];
            snprintf(esc, sizeof esc, "\\u%04X", u);
            out->append(esc);
            continue;
        }
        // A high surrogate followed by a low one is one code point, and encoding them separately
        // would produce two invalid UTF-8 sequences that this file's own decoder then refuses.
        if (u >= 0xD800 && u <= 0xDBFF) {
            const auto next = static_cast<uint32_t>(static_cast<uint16_t>(s[i + 1]));
            if (next >= 0xDC00 && next <= 0xDFFF) {
                AppendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (next - 0xDC00));
                ++i;
                continue;
            }
            // An unpaired surrogate is not text. Escaped rather than encoded, so the file stays
            // valid JSON and the damage stays visible.
            char esc[7];
            snprintf(esc, sizeof esc, "\\u%04X", u);
            out->append(esc);
            continue;
        }
        AppendUtf8(out, u);
    }
    out->push_back('"');
}

void AppendInt64(std::string* out, int64_t v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    out->append(buf);
}

// ---- A JSON reader, scoped to this document --------------------------------------------------
//
// Small, but a real parser rather than a pattern match: the point of reading this file is to refuse
// a damaged one, and a scanner that hunts for "tUs" would accept text that is not JSON at all.

class Reader {
public:
    Reader(const char* p, size_t len) : p_(p), end_(p + len) {}

    void SkipWhitespace() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) {
            ++p_;
        }
    }

    bool AtEnd() {
        SkipWhitespace();
        return p_ >= end_;
    }

    bool Peek(char* c) {
        SkipWhitespace();
        if (p_ >= end_) {
            return false;
        }
        *c = *p_;
        return true;
    }

    bool Take(char expect) {
        SkipWhitespace();
        if (p_ >= end_ || *p_ != expect) {
            return false;
        }
        ++p_;
        return true;
    }

    bool ReadString(wchar_t* out, size_t cap, bool* tooLong) {
        *tooLong = false;
        SkipWhitespace();
        if (p_ >= end_ || *p_ != '"') {
            return false;
        }
        ++p_;
        size_t len = 0;
        while (p_ < end_ && *p_ != '"') {
            uint32_t cp = 0;
            if (*p_ == '\\') {
                if (!ReadEscape(&cp)) {
                    return false;
                }
            } else if (static_cast<uint8_t>(*p_) < 0x20) {
                return false;   // a raw control character is not legal inside a JSON string
            } else {
                size_t used = 0;
                if (!DecodeUtf8(p_, end_, &cp, &used)) {
                    return false;
                }
                p_ += used;
            }
            if (!AppendUtf16(cp, out, cap, &len)) {
                *tooLong = true;
                return false;
            }
        }
        if (p_ >= end_) {
            return false;   // unterminated
        }
        ++p_;
        out[len] = 0;
        return true;
    }

    bool ReadInt64(int64_t* out) {
        SkipWhitespace();
        const char* start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) {
            ++p_;
        }
        if (p_ >= end_ || *p_ < '0' || *p_ > '9') {
            return false;
        }
        const bool neg = (*start == '-');
        const uint64_t limit = neg ? 9223372036854775808ull : 9223372036854775807ull;
        uint64_t v = 0;
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
            const uint64_t d = static_cast<uint64_t>(*p_ - '0');
            if (v > (limit - d) / 10) {
                return false;
            }
            v = v * 10 + d;
            ++p_;
        }
        // These fields are microsecond counts. A fractional or exponent form is a different
        // document than the one we write, and accepting it would mean guessing what it meant.
        if (p_ < end_ && (*p_ == '.' || *p_ == 'e' || *p_ == 'E')) {
            return false;
        }
        *out = neg ? static_cast<int64_t>(0u - v) : static_cast<int64_t>(v);
        return true;
    }

    // Consumes any well-formed value and rejects anything that is not one, so an unknown key
    // written by a future version is skipped without weakening the "is this JSON" question.
    bool SkipValue(int depth = 0) {
        if (depth > 32) {
            return false;
        }
        char c = 0;
        if (!Peek(&c)) {
            return false;
        }
        if (c == '"') {
            wchar_t scratch[64];
            bool tooLong = false;
            if (ReadString(scratch, 64, &tooLong)) {
                return true;
            }
            // A string longer than the scratch is still a valid string; re-scan it as raw text.
            return !tooLong ? false : SkipRawString();
        }
        if (c == '{' || c == '[') {
            const char open = c;
            const char close = (open == '{') ? '}' : ']';
            Take(open);
            if (Take(close)) {
                return true;
            }
            for (;;) {
                if (open == '{') {
                    wchar_t key[128];
                    bool tooLong = false;
                    if (!ReadString(key, 128, &tooLong) && !tooLong) {
                        return false;
                    }
                    if (!Take(':')) {
                        return false;
                    }
                }
                if (!SkipValue(depth + 1)) {
                    return false;
                }
                if (Take(',')) {
                    continue;
                }
                return Take(close);
            }
        }
        if (c == 't') {
            return Literal("true");
        }
        if (c == 'f') {
            return Literal("false");
        }
        if (c == 'n') {
            return Literal("null");
        }
        return SkipNumber();
    }

private:
    bool Literal(const char* lit) {
        SkipWhitespace();
        for (size_t i = 0; lit[i] != 0; ++i) {
            if (p_ >= end_ || *p_ != lit[i]) {
                return false;
            }
            ++p_;
        }
        return true;
    }

    bool SkipNumber() {
        SkipWhitespace();
        const char* start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) {
            ++p_;
        }
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' ||
                             *p_ == '-' || *p_ == '+')) {
            ++p_;
        }
        return p_ != start;
    }

    bool SkipRawString() {
        SkipWhitespace();
        if (p_ >= end_ || *p_ != '"') {
            return false;
        }
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            p_ += (*p_ == '\\' && p_ + 1 < end_) ? 2 : 1;
        }
        if (p_ >= end_) {
            return false;
        }
        ++p_;
        return true;
    }

    bool ReadEscape(uint32_t* cp) {
        ++p_;   // the backslash
        if (p_ >= end_) {
            return false;
        }
        const char e = *p_++;
        switch (e) {
            case '"':  *cp = '"';  return true;
            case '\\': *cp = '\\'; return true;
            case '/':  *cp = '/';  return true;
            case 'b':  *cp = '\b'; return true;
            case 'f':  *cp = '\f'; return true;
            case 'n':  *cp = '\n'; return true;
            case 'r':  *cp = '\r'; return true;
            case 't':  *cp = '\t'; return true;
            case 'u':  break;
            default:   return false;
        }
        uint32_t hi = 0;
        if (!ReadHex4(&hi)) {
            return false;
        }
        if (hi >= 0xD800 && hi <= 0xDBFF) {
            // A high surrogate must be followed by its low half, or the two would decode to
            // different text than they encoded.
            if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') {
                return false;
            }
            p_ += 2;
            uint32_t lo = 0;
            if (!ReadHex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) {
                return false;
            }
            *cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
            return true;
        }
        if (hi >= 0xDC00 && hi <= 0xDFFF) {
            return false;   // a low surrogate with no high half
        }
        *cp = hi;
        return true;
    }

    bool ReadHex4(uint32_t* out) {
        if (end_ - p_ < 4) {
            return false;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            uint32_t d;
            if (c >= '0' && c <= '9') {
                d = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
            v = (v << 4) | d;
        }
        *out = v;
        return true;
    }

    const char* p_;
    const char* end_;
};

bool KeyIs(const wchar_t* key, const char* lit) {
    size_t i = 0;
    for (; key[i] != 0; ++i) {
        if (lit[i] == 0 || key[i] != static_cast<wchar_t>(lit[i])) {
            return false;
        }
    }
    return lit[i] == 0;
}

// True when the label holds a surrogate that is not part of a pair. AppendJsonString writes such a
// unit as a bare \uXXXX escape and ReadEscape refuses exactly that, so a label like this would save
// and then make its own sidecar unreadable for good. Refused at the door instead.
bool HasUnpairedSurrogate(const wchar_t* s) {
    for (size_t i = 0; s[i] != 0; ++i) {
        const auto u = static_cast<uint32_t>(static_cast<uint16_t>(s[i]));
        if (u >= 0xDC00 && u <= 0xDFFF) {
            return true;   // a low half with no high half before it
        }
        if (u >= 0xD800 && u <= 0xDBFF) {
            const auto next = static_cast<uint32_t>(static_cast<uint16_t>(s[i + 1]));
            if (next < 0xDC00 || next > 0xDFFF) {
                return true;
            }
            ++i;
        }
    }
    return false;
}

}  // namespace

const char* AddResultName(AddResult r) {
    switch (r) {
        case AddResult::Ok:            return "added";
        case AddResult::Full:          return "the sidecar already holds its maximum bookmarks";
        case AddResult::LabelTooLong:  return "the label is longer than this sidecar allows";
        case AddResult::LabelMalformed:
            return "the label contains half of a surrogate pair, which this file's own reader "
                   "would refuse once it was written";
    }
    return "refused";
}

const char* LoadResultName(LoadResult r) {
    switch (r) {
        case LoadResult::Ok:             return "read";
        case LoadResult::Absent:         return "there is no sidecar yet";
        case LoadResult::Unreadable:     return "it exists and could not be read";
        case LoadResult::Damaged:        return "it exists and does not parse";
        case LoadResult::UnknownVersion: return "it is a version of the format this build cannot read";
        case LoadResult::AbsoluteAxisVersion:
            return "it was written before the playback axis became 0-based, so its bookmark times "
                   "are absolute capture time and would land in the wrong place. Nothing can "
                   "convert them: no seam publishes a capture's start time any more";
        case LoadResult::Unidentified:
            return "notes-stale-identity: it names no capture, so nothing can show it belongs to "
                   "this one rather than to a file that used to be at this path";
        case LoadResult::StaleIdentity:
            return "notes-stale-identity: it belongs to a different capture. It has been renamed "
                   "aside rather than deleted, and this capture starts with no bookmarks";
        case LoadResult::StaleIdentityHeld:
            return "notes-stale-identity: it belongs to a different capture and could not be "
                   "renamed aside, so nothing was written and it is exactly as it was";
    }
    return "not read";
}

const char* SaveResultName(SaveResult r) {
    switch (r) {
        case SaveResult::Ok:            return "written";
        case SaveResult::PathTooLong:   return "the sidecar path does not fit";
        case SaveResult::OpenFailed:    return "the sidecar could not be opened for writing";
        case SaveResult::WriteFailed:   return "the write did not complete";
        case SaveResult::ReplaceFailed: return "the finished file could not replace the old one";
    }
    return "not written";
}

size_t MaxDemoPathChars() { return kMaxDemoChars; }

bool CaptureIdentity::IsLegacy() const {
    for (size_t i = 0; i < 16; ++i) {
        if (uuid[i] != 0) {
            return false;
        }
    }
    return true;
}

bool CaptureIdentity::Matches(const CaptureIdentity& other) const {
    if (sizeBytes != other.sizeBytes || t0Us != other.t0Us) {
        return false;
    }
    if (memcmp(uuid, other.uuid, sizeof uuid) != 0) {
        return false;
    }
    // A capture minted before ENG-W2A-R10 F7 has no uuid to compare, so the content answers
    // instead. A capture WITH a uuid never consults the digest: it is not computed for those, and
    // comparing a field neither side fills would be a comparison that cannot fail.
    return IsLegacy() ? memcmp(headDigest, other.headDigest, sizeof headDigest) == 0 : true;
}

bool IdentityForCapture(const wchar_t* demoPath, CaptureIdentity* out) {
    if (demoPath == nullptr || out == nullptr) {
        return false;
    }
    *out = CaptureIdentity();
    if (WideLen(demoPath) + 1 > MAX_PATH) {
        return false;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, demoPath, L"rb") != 0 || f == nullptr) {
        return false;
    }
    __int64 size = 0;
    if (_fseeki64(f, 0, SEEK_END) != 0 || (size = _ftelli64(f)) < 0 ||
        _fseeki64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    // The loader's own struct and the loader's own magic, never a hand-counted offset: a reader
    // with its own copy of a format certifies files the real reader would refuse.
    FileHeaderDisk head;
    if (static_cast<uint64_t>(size) < sizeof head ||
        fread(&head, 1, sizeof head, f) != sizeof head ||
        memcmp(head.magic, kFileMagic, sizeof head.magic) != 0) {
        fclose(f);
        return false;
    }
    memcpy(out->uuid, head.demoUuid, sizeof out->uuid);
    out->sizeBytes = size;
    out->t0Us = static_cast<int64_t>(head.t0Us);
    if (out->IsLegacy()) {
        if (_fseeki64(f, 0, SEEK_SET) != 0) {
            fclose(f);
            return false;
        }
        Md5    md5;
        uint8_t buf[4096];
        size_t  total = 0;
        while (total < kLegacyDigestBytes) {
            size_t want = kLegacyDigestBytes - total;
            if (want > sizeof buf) {
                want = sizeof buf;
            }
            const size_t got = fread(buf, 1, want, f);
            if (got == 0) {
                break;
            }
            md5.Update(buf, got);
            total += got;
        }
        if (ferror(f) != 0) {
            fclose(f);
            return false;
        }
        md5.Final(out->headDigest);
    }
    fclose(f);
    return true;
}

bool SidecarPathFor(const wchar_t* demoPath, wchar_t* out, size_t cap) {
    if (demoPath == nullptr || out == nullptr || demoPath[0] == 0) {
        return false;
    }
    const size_t base = WideLen(demoPath);
    const size_t suffix = sizeof kSuffix - 1;
    if (base > kMaxDemoChars || base + suffix + 1 > cap) {
        return false;
    }
    for (size_t i = 0; i < base; ++i) {
        out[i] = demoPath[i];
    }
    for (size_t i = 0; i < suffix; ++i) {
        out[base + i] = static_cast<wchar_t>(kSuffix[i]);
    }
    out[base + suffix] = 0;
    return true;
}

void Sidecar::Clear() {
    identity_ = CaptureIdentity();
    count_ = 0;
    lastPlayheadUs_ = 0;
}

AddResult Sidecar::AddBookmark(int64_t tUs, const wchar_t* label) {
    if (count_ >= kMaxBookmarks) {
        return AddResult::Full;
    }
    if (label != nullptr) {
        if (WideLen(label) + 1 > kMaxLabelChars) {
            return AddResult::LabelTooLong;
        }
        if (HasUnpairedSurrogate(label)) {
            return AddResult::LabelMalformed;
        }
    }
    Bookmark& b = bookmarks_[count_];
    b.tUs = tUs;
    b.label[0] = 0;
    if (label != nullptr) {
        for (size_t i = 0; label[i] != 0; ++i) {
            b.label[i] = label[i];
            b.label[i + 1] = 0;
        }
    }
    ++count_;
    return AddResult::Ok;
}

const Bookmark* Sidecar::BookmarkAt(size_t i) const {
    return i < count_ ? &bookmarks_[i] : nullptr;
}

void Sidecar::Serialise(std::string* out) const {
    if (out == nullptr) {
        return;
    }
    out->clear();
    out->append("{\n  \"version\": ");
    AppendInt64(out, kFormatVersion);
    // Written on every save, including the very first: a sidecar with no identity is refused on the
    // next open, so a writer that could omit this block would be a writer that produces files its
    // own reader throws away.
    out->append(",\n  \"identity\": {\"uuid\": ");
    AppendHex16(out, identity_.uuid);
    out->append(", \"head\": ");
    AppendHex16(out, identity_.headDigest);
    out->append(", \"sizeBytes\": ");
    AppendInt64(out, identity_.sizeBytes);
    out->append(", \"t0Us\": ");
    AppendInt64(out, identity_.t0Us);
    out->append("}");
    out->append(",\n  \"lastPlayheadUs\": ");
    AppendInt64(out, lastPlayheadUs_);
    out->append(",\n  \"bookmarks\": [");
    for (size_t i = 0; i < count_; ++i) {
        out->append(i == 0 ? "\n    {\"tUs\": " : ",\n    {\"tUs\": ");
        AppendInt64(out, bookmarks_[i].tUs);
        out->append(", \"label\": ");
        AppendJsonString(out, bookmarks_[i].label);
        out->append("}");
    }
    out->append(count_ != 0 ? "\n  ]\n}\n" : "]\n}\n");
}

LoadResult Sidecar::Parse(const char* text, size_t len) {
    Clear();
    if (text == nullptr || len == 0) {
        return LoadResult::Damaged;
    }
    Reader r(text, len);
    if (!r.Take('{')) {
        return LoadResult::Damaged;
    }

    // ★ THE VERSION IS READ, not skipped as an unknown key. It was written on every save and then
    // fell through to SkipValue on every load, so `{"version":99,...}` was accepted as version 1 --
    // a future format with its times in nanoseconds would have been read as microseconds and every
    // bookmark would have landed a thousand times off. A document with NO version is not ours
    // either: adopting a hand-written or foreign file silently is the same mistake.
    //
    // That was hypothetical when it was written and it is not any more: format 1 holds absolute
    // capture times and format 2 holds 0-based playback times (kFormatVersion). This field is the
    // only difference between the two documents, so it is the only thing that can stop a format-1
    // bookmark being drawn t0 microseconds away from the moment it marks.
    //
    // ★ AND IT IS READ ONCE. With last-wins semantics, `"version": 1, ..., "version": 2` reads as
    // format 2 and adopts absolute times onto the 0-based axis -- the one field standing between
    // the two formats, defeated by a second copy of itself. A key this reader has already taken is
    // input it does not understand, and the answer to that is Damaged, for every key, not a shrug.
    bool    sawVersion = false;
    bool    sawIdentity = false;
    bool    sawLastPlayhead = false;
    bool    sawBookmarks = false;
    int64_t version = 0;

    Sidecar staged;
    if (!r.Take('}')) {
        for (;;) {
            wchar_t key[64];
            bool tooLong = false;
            if (!r.ReadString(key, 64, &tooLong)) {
                return LoadResult::Damaged;
            }
            if (!r.Take(':')) {
                return LoadResult::Damaged;
            }
            if (KeyIs(key, "version")) {
                if (sawVersion || !r.ReadInt64(&version)) {
                    return LoadResult::Damaged;
                }
                sawVersion = true;
            } else if (KeyIs(key, "identity")) {
                if (sawIdentity || !r.Take('{')) {
                    return LoadResult::Damaged;
                }
                sawIdentity = true;
                bool sawUuid = false;
                bool sawHead = false;
                bool sawSize = false;
                bool sawT0 = false;
                for (;;) {
                    wchar_t field[64];
                    if (!r.ReadString(field, 64, &tooLong) || !r.Take(':')) {
                        return LoadResult::Damaged;
                    }
                    if (KeyIs(field, "uuid")) {
                        wchar_t hex[64];
                        if (sawUuid || !r.ReadString(hex, 64, &tooLong) ||
                            !ParseHex16(hex, staged.identity_.uuid)) {
                            return LoadResult::Damaged;
                        }
                        sawUuid = true;
                    } else if (KeyIs(field, "head")) {
                        wchar_t hex[64];
                        if (sawHead || !r.ReadString(hex, 64, &tooLong) ||
                            !ParseHex16(hex, staged.identity_.headDigest)) {
                            return LoadResult::Damaged;
                        }
                        sawHead = true;
                    } else if (KeyIs(field, "sizeBytes")) {
                        if (sawSize || !r.ReadInt64(&staged.identity_.sizeBytes)) {
                            return LoadResult::Damaged;
                        }
                        sawSize = true;
                    } else if (KeyIs(field, "t0Us")) {
                        if (sawT0 || !r.ReadInt64(&staged.identity_.t0Us)) {
                            return LoadResult::Damaged;
                        }
                        sawT0 = true;
                    } else if (!r.SkipValue()) {
                        return LoadResult::Damaged;
                    }
                    if (r.Take(',')) {
                        continue;
                    }
                    if (!r.Take('}')) {
                        return LoadResult::Damaged;
                    }
                    break;
                }
                // A half-written identity is not a weaker identity, it is a document claiming to
                // bind itself to a capture without saying which one.
                if (!sawUuid || !sawHead || !sawSize || !sawT0) {
                    return LoadResult::Damaged;
                }
            } else if (KeyIs(key, "lastPlayheadUs")) {
                if (sawLastPlayhead || !r.ReadInt64(&staged.lastPlayheadUs_)) {
                    return LoadResult::Damaged;
                }
                sawLastPlayhead = true;
            } else if (KeyIs(key, "bookmarks")) {
                if (sawBookmarks) {
                    return LoadResult::Damaged;
                }
                sawBookmarks = true;
                if (!r.Take('[')) {
                    return LoadResult::Damaged;
                }
                if (!r.Take(']')) {
                    for (;;) {
                        if (!r.Take('{')) {
                            return LoadResult::Damaged;
                        }
                        int64_t tUs = 0;
                        wchar_t label[kMaxLabelChars] = {0};
                        bool sawTUs = false;
                        bool sawLabel = false;
                        if (!r.Take('}')) {
                            for (;;) {
                                wchar_t field[64];
                                if (!r.ReadString(field, 64, &tooLong) || !r.Take(':')) {
                                    return LoadResult::Damaged;
                                }
                                if (KeyIs(field, "tUs")) {
                                    if (sawTUs || !r.ReadInt64(&tUs)) {
                                        return LoadResult::Damaged;
                                    }
                                    sawTUs = true;
                                } else if (KeyIs(field, "label")) {
                                    if (sawLabel || !r.ReadString(label, kMaxLabelChars, &tooLong)) {
                                        return LoadResult::Damaged;
                                    }
                                    sawLabel = true;
                                } else if (!r.SkipValue()) {
                                    return LoadResult::Damaged;
                                }
                                if (r.Take(',')) {
                                    continue;
                                }
                                if (!r.Take('}')) {
                                    return LoadResult::Damaged;
                                }
                                break;
                            }
                        }
                        // Past the cap the file is refused rather than half-read: silently keeping
                        // the first 256 of 300 bookmarks would lose 44 of a user's own notes.
                        if (staged.AddBookmark(tUs, label) != AddResult::Ok) {
                            return LoadResult::Damaged;
                        }
                        if (r.Take(',')) {
                            continue;
                        }
                        if (!r.Take(']')) {
                            return LoadResult::Damaged;
                        }
                        break;
                    }
                }
            } else if (!r.SkipValue()) {
                return LoadResult::Damaged;
            }
            if (r.Take(',')) {
                continue;
            }
            if (!r.Take('}')) {
                return LoadResult::Damaged;
            }
            break;
        }
    }
    if (!r.AtEnd()) {
        return LoadResult::Damaged;   // trailing text means this is not the document it claims to be
    }

    if (sawVersion && version == kFormatVersion1Absolute) {
        return LoadResult::AbsoluteAxisVersion;
    }
    if (!sawVersion || version != kFormatVersion) {
        return LoadResult::UnknownVersion;
    }

    // ★ AFTER the version, because a format-1 document is refused on its axis and saying
    // "unidentified" about it would name the second-most-interesting thing wrong with it.
    if (!sawIdentity) {
        return LoadResult::Unidentified;
    }

    count_ = staged.count_;
    lastPlayheadUs_ = staged.lastPlayheadUs_;
    identity_ = staged.identity_;
    for (size_t i = 0; i < count_; ++i) {
        bookmarks_[i] = staged.bookmarks_[i];
    }
    return LoadResult::Ok;
}

LoadResult Sidecar::Load(const wchar_t* path) {
    Clear();
    if (path == nullptr) {
        return LoadResult::Unreadable;
    }
    if (WideLen(path) + 1 > MAX_PATH) {
        // Not "absent". A path the CRT will not open is a path we cannot say anything about, and
        // answering Absent here would authorise the caller to write a new file over it.
        return LoadResult::Unreadable;
    }
    FILE* f = nullptr;
    // ★★★ THE ERRNO IS THE WHOLE POINT. This used to be `!= 0 || f == nullptr` into one
    // fileMissing_ flag, so a sharing violation, a deny-read ACE and descriptor exhaustion all
    // reported "there is no sidecar yet" -- and `mark`, whose only protection was that flag, built
    // a fresh one-bookmark file and replaced a week of the owner's marks with it. ENOENT, and
    // nothing else, is absence.
    const errno_t err = _wfopen_s(&f, path, L"rb");
    if (err != 0 || f == nullptr) {
        return err == ENOENT ? LoadResult::Absent : LoadResult::Unreadable;
    }
    std::string text;
    char buf[4096];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof buf, f)) != 0) {
        if (text.size() + got > kMaxFileBytes) {
            fclose(f);
            return LoadResult::Damaged;   // too big to be a notes file
        }
        text.append(buf, got);
    }
    const bool readFailed = ferror(f) != 0;
    fclose(f);
    if (readFailed) {
        return LoadResult::Unreadable;
    }
    return Parse(text.c_str(), text.size());
}

namespace {

// `<sidecar>.stale-<32 hex>`, named after the capture the file says it belongs to so two aside
// files from two different captures do not collide. Never a delete: these are the owner's marks and
// the capture they belong to may still be on disk.
bool MoveStaleAside(const wchar_t* path, const CaptureIdentity& was, bool identified) {
    const size_t pathLen = WideLen(path);
    if (pathLen + kAsideSuffixChars + 1 > MAX_PATH) {
        return false;
    }
    const uint8_t* token = was.IsLegacy() ? was.headDigest : was.uuid;
    wchar_t suffix[kAsideSuffixChars + 1];
    size_t  at = 0;
    for (const wchar_t* p = L".stale-"; *p != 0; ++p) {
        suffix[at++] = *p;
    }
    for (size_t i = 0; i < 16; ++i) {
        // An unidentified sidecar has no token of its own; zeroes are what it recorded and zeroes
        // are what it is filed under.
        const uint8_t b = identified ? token[i] : 0;
        suffix[at++] = static_cast<wchar_t>(kHexDigits[b >> 4]);
        suffix[at++] = static_cast<wchar_t>(kHexDigits[b & 0x0F]);
    }
    suffix[at] = 0;

    std::wstring aside(path);
    aside.append(suffix);
    // Not MOVEFILE_REPLACE_EXISTING: an aside file already at that name holds bookmarks too.
    if (MoveFileExW(path, aside.c_str(), 0)) {
        return true;
    }
    for (wchar_t n = L'2'; n <= L'9'; ++n) {
        std::wstring numbered(aside);
        numbered.push_back(L'-');
        numbered.push_back(n);
        if (MoveFileExW(path, numbered.c_str(), 0)) {
            return true;
        }
    }
    return false;
}

}  // namespace

LoadResult Sidecar::LoadFor(const wchar_t* path, const CaptureIdentity& expect) {
    const LoadResult read = Load(path);
    if (read != LoadResult::Ok && read != LoadResult::Unidentified) {
        return read;
    }
    if (read == LoadResult::Ok && identity_.Matches(expect)) {
        return LoadResult::Ok;
    }
    // ★ EMPTY, ALWAYS, BEFORE ANYTHING ELSE CAN GO WRONG. L3-RUN-3 F3 is what happens when a
    // mismatch leaves the bookmarks in place: another capture's marks are listed as this one's and
    // `goto resume` seeks to a moment this file does not contain. The rename can fail; handing back
    // a foreign bookmark set must not depend on whether it did.
    const CaptureIdentity was = identity_;
    const bool identified = read == LoadResult::Ok;
    Clear();
    return MoveStaleAside(path, was, identified) ? LoadResult::StaleIdentity
                                                 : LoadResult::StaleIdentityHeld;
}

SaveResult Sidecar::Save(const wchar_t* path) const {
    if (path == nullptr) {
        return SaveResult::PathTooLong;
    }
    const size_t pathLen = WideLen(path);
    if (pathLen + kTempSuffixChars + 1 > MAX_PATH) {
        // Measured before anything is opened, because the failure this replaces looked like a
        // permission problem and sent the owner to inspect ACLs on a path that was simply too long.
        return SaveResult::PathTooLong;
    }

    std::string text;
    Serialise(&text);

    // Through a temporary, then one atomic replace. Writing in place means a crash or a full disk
    // leaves a truncated file that the reader then refuses -- which loses every bookmark, in a way
    // the user only discovers on the next launch.
    //
    // The temporary carries this process's id: two Echo VR instances marking the same capture used
    // to open one `<sidecar>.tmp` for writing and truncate each other before either renamed.
    wchar_t suffix[kTempSuffixChars + 1];
    _snwprintf_s(suffix, _TRUNCATE, L".%08X.tmp", static_cast<unsigned>(GetCurrentProcessId()));
    std::wstring temp(path);
    temp.append(suffix);

    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || f == nullptr) {
        return SaveResult::OpenFailed;
    }
    const size_t written = text.empty() ? 0 : fwrite(text.data(), 1, text.size(), f);
    // ★★★ THREE SEPARATE ANSWERS, AND THE LAST TWO USED TO BE THROWN AWAY.
    //
    // fwrite says the bytes reached the CRT buffer. fflush says they reached the OS. NEITHER says
    // they reached the disk -- and on a network path, which is an entirely ordinary place to keep a
    // capture archive, a delayed-write failure surfaces at CLOSE. fclose's return was discarded, so
    // Save reported success for a write that did not happen and the owner was told "bookmarked ...
    // (4 in this file)" over a file that still held three.
    //
    // _commit is the durability barrier the atomic-replace design was already assuming: without it
    // the rename's metadata can commit ahead of the data, which is exactly the zero-length sidecar
    // this shape exists to prevent.
    bool complete = (written == text.size()) && (fflush(f) == 0) && (_commit(_fileno(f)) == 0);
    if (fclose(f) != 0) {
        complete = false;
    }
    if (!complete) {
        DeleteFileW(temp.c_str());
        return SaveResult::WriteFailed;
    }
    if (!MoveFileExW(temp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return SaveResult::ReplaceFailed;
    }
    return SaveResult::Ok;
}

}  // namespace notes
}  // namespace control
}  // namespace replay
