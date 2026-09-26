#include "capture/capturesecs.h"

#include <windows.h>

namespace replay {
namespace capture {
namespace {

// The same wide spelling as the narrow one, built from the single literal in the header so the two
// cannot drift apart.
constexpr wchar_t kLegacySwitchToken[] = L"" REPLAY_LEGACY_CAPTURE_SECS_SWITCH;
constexpr size_t kLegacySwitchTokenLen = sizeof kLegacySwitchToken / sizeof(wchar_t) - 1;

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

// The three prefixes the engine's tokeniser accepts for a switch. 0x96 is the en dash a launcher
// picks up when a command line has been through a word processor, and the engine honours it, so a
// diagnostic that missed it would miss the launch most likely to have been hand-edited.
bool IsSwitchPrefix(wchar_t c) {
    return c == L'-' || c == L'+' || c == static_cast<wchar_t>(0x96);
}

wchar_t Lower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// Trims ASCII whitespace and one matched pair of surrounding quotes. A launcher that interpolates a
// number into a batch file leaves both behind, and neither can be part of a number, so removing
// them cannot change which value is named.
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

}  // namespace

const char* CaptureSecsRefusalText(CaptureSecsRefusal reason) {
    switch (reason) {
        case CaptureSecsRefusal::None:       return "no refusal";
        case CaptureSecsRefusal::TooLong:    return "the value is longer than a capture length can "
                                                    "be";
        case CaptureSecsRefusal::NotANumber: return "the value is not a whole number of seconds";
        case CaptureSecsRefusal::OutOfRange: return "the value is outside 30..86400 seconds";
    }
    return "unknown";
}

CaptureSecsRefusal ParseCaptureSeconds(const wchar_t* text, size_t len, uint32_t* seconds) {
    if (seconds != nullptr) {
        *seconds = 0;
    }
    if (text == nullptr || seconds == nullptr) {
        return CaptureSecsRefusal::NotANumber;
    }
    Tidy(&text, &len);
    if (len == 0) {
        return CaptureSecsRefusal::NotANumber;
    }
    // ★ NO SIGN, NOT EVEN A LEADING '+'. A signed value is a value written by something that thinks
    // this field can be negative, and accepting the plus while refusing the minus would hide that.
    uint64_t acc = 0;
    for (size_t i = 0; i < len; ++i) {
        const wchar_t c = text[i];
        if (c < L'0' || c > L'9') {
            return CaptureSecsRefusal::NotANumber;
        }
        acc = acc * 10 + static_cast<uint64_t>(c - L'0');
        // Stops the accumulator well below any overflow, and every value that reaches this point is
        // already past the ceiling, so the answer cannot change by reading further digits.
        if (acc > kCaptureSecsMax) {
            return CaptureSecsRefusal::OutOfRange;
        }
    }
    if (acc < kCaptureSecsMin) {
        return CaptureSecsRefusal::OutOfRange;
    }
    *seconds = static_cast<uint32_t>(acc);
    return CaptureSecsRefusal::None;
}

bool ReadCaptureSecondsRequest(CaptureSecsRequest* out) {
    if (out == nullptr) {
        return false;
    }
    *out = CaptureSecsRequest();

    wchar_t value[kMaxCaptureSecsChars + 8];
    const DWORD cap = static_cast<DWORD>(sizeof value / sizeof value[0]);
    const DWORD n = ::GetEnvironmentVariableW(kCaptureSecsEnvVar, value, cap);
    if (n == 0) {
        return false;   // not set, or set to nothing, which this treats the same way
    }
    out->present = true;
    if (n >= cap) {
        // n is then the size the value needs including its terminator: it did not fit, and a value
        // that did not fit is a different number from the one that was meant.
        out->refusal = CaptureSecsRefusal::TooLong;
        return true;
    }
    value[n] = 0;

    const wchar_t* text = value;
    size_t len = n;
    Tidy(&text, &len);
    if (len + 1 > kMaxCaptureSecsChars) {
        out->refusal = CaptureSecsRefusal::TooLong;
        return true;
    }
    for (size_t i = 0; i < len; ++i) {
        out->value[i] = text[i];
    }
    out->value[len] = 0;

    uint32_t seconds = 0;
    const CaptureSecsRefusal why = ParseCaptureSeconds(text, len, &seconds);
    if (why != CaptureSecsRefusal::None) {
        out->refusal = why;
        return true;
    }
    out->seconds = seconds;
    out->hasSeconds = true;
    return true;
}

bool CommandLineMentionsCaptureSecs(const wchar_t* commandLine) {
    if (commandLine == nullptr) {
        return false;
    }
    for (const wchar_t* p = commandLine; *p != 0;) {
        while (*p != 0 && IsSpace(*p)) {
            ++p;
        }
        const wchar_t* token = p;
        while (*p != 0 && !IsSpace(*p)) {
            ++p;
        }
        size_t len = static_cast<size_t>(p - token);
        // A launcher that quoted the whole switch still put it on the line.
        if (len >= 2 && token[0] == L'"' && token[len - 1] == L'"') {
            ++token;
            len -= 2;
        }
        if (len < kLegacySwitchTokenLen + 1 || !IsSwitchPrefix(token[0])) {
            continue;
        }
        bool match = true;
        for (size_t i = 0; i < kLegacySwitchTokenLen; ++i) {
            if (Lower(token[1 + i]) != kLegacySwitchToken[i]) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        // The token has to END here, so a longer switch that merely starts the same way is not
        // reported as this one. Both attached forms count: the value can follow '=' or ':'.
        const size_t after = 1 + kLegacySwitchTokenLen;
        if (after == len || token[after] == L'=' || token[after] == L':') {
            return true;
        }
    }
    return false;
}

}  // namespace capture
}  // namespace replay
