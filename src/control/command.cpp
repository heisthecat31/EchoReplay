#include "control/command.h"

// W2-G. See command.h for why this file has no Windows header, no locale dependency and no
// allocation in it.

namespace replay {
namespace control {
namespace {

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

wchar_t LowerAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// Byte-equal over two NUL-terminated ASCII words. <string.h> is a dependency this file exists
// without; the mode word reaching here is already lowercased by CopyModeWord.
bool WordIs(const char* a, const char* b) {
    while (*a != 0 && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

const wchar_t* SkipSpace(const wchar_t* p) {
    while (*p != 0 && IsSpace(*p)) {
        ++p;
    }
    return p;
}

// Case-insensitive over ASCII only. Command words are ASCII by construction; folding beyond that
// would need a locale, which is the dependency this file exists without.
bool TokenIs(const wchar_t* tok, size_t len, const char* literal) {
    size_t i = 0;
    for (; i < len; ++i) {
        const char lit = literal[i];
        if (lit == 0) {
            return false;
        }
        if (LowerAscii(tok[i]) != LowerAscii(static_cast<wchar_t>(lit))) {
            return false;
        }
    }
    return literal[i] == 0;
}

// Builds an error line. Every write is bounded and the buffer is always terminated, including when
// the message is truncated -- an error path that can overrun is worse than the error it reports.
class Msg {
public:
    Msg(char* buf, size_t cap) : buf_(buf), cap_(cap), len_(0) {
        if (cap_ != 0) {
            buf_[0] = 0;
        }
    }

    void Str(const char* s) {
        while (*s != 0 && len_ + 1 < cap_) {
            buf_[len_++] = *s++;
        }
        Terminate();
    }

    void Ch(char c) {
        if (len_ + 1 < cap_) {
            buf_[len_++] = c;
        }
        Terminate();
    }

    // The offending token, quoted. Anything outside printable ASCII becomes '?': this buffer is
    // narrow, the token may not be, and a mojibake fragment in an error message is noise. The
    // token stays delimited, so an empty or all-'?' token is visibly the thing that was typed.
    void Quoted(const wchar_t* tok, size_t len) {
        Ch('"');
        const size_t shown = len < 48 ? len : 48;
        for (size_t i = 0; i < shown; ++i) {
            const wchar_t c = tok[i];
            Ch((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?');
        }
        if (len > shown) {
            Str("...");
        }
        Ch('"');
    }

    void Int(int64_t v) {
        char tmp[24];
        size_t n = 0;
        const bool neg = v < 0;
        // Negated in unsigned so INT64_MIN has somewhere to go.
        uint64_t u = neg ? (0u - static_cast<uint64_t>(v)) : static_cast<uint64_t>(v);
        do {
            tmp[n++] = static_cast<char>('0' + (u % 10));
            u /= 10;
        } while (u != 0 && n < sizeof tmp);
        if (neg) {
            Ch('-');
        }
        while (n != 0) {
            Ch(tmp[--n]);
        }
    }

private:
    void Terminate() {
        if (cap_ != 0) {
            buf_[len_] = 0;
        }
    }

    char*  buf_;
    size_t cap_;
    size_t len_;
};

// A token is either bare (runs to whitespace) or double-quoted (runs to the closing quote, which is
// not part of it). `quoted` matters to the callers that take a whole argument: "no argument" and
// "an empty argument" are different requests, and only a quote can express the second.
struct Token {
    const wchar_t* begin = nullptr;
    size_t         len = 0;
    bool           present = false;
    bool           quoted = false;
};

// ★ ONE GUARD FOR EVERY VERB, because the same typo used to get two verdicts. The unterminated
// quote check was applied at three call sites (the command word, `load`, `mark`) and absent from
// six: `load "a` was refused while `speed "1.5` was ACCEPTED at rate 1.5, and `seek "50%`,
// `cam "pov` and `cam pov "3` were all accepted. The values produced were right, which is what made
// it survive -- so the check moved to the one place every line passes through instead of being
// added to six more.
bool HasUnterminatedQuote(const wchar_t* line) {
    bool open = false;
    for (const wchar_t* p = line; *p != 0; ++p) {
        if (*p == L'"') {
            open = !open;
        }
    }
    return open;
}

Token NextToken(const wchar_t** cursor) {
    Token t;
    const wchar_t* p = SkipSpace(*cursor);
    if (*p == 0) {
        *cursor = p;
        return t;
    }
    if (*p == L'"') {
        ++p;
        t.begin = p;
        while (*p != 0 && *p != L'"') {
            ++p;
        }
        t.len = static_cast<size_t>(p - t.begin);
        t.present = true;
        t.quoted = true;
        if (*p == L'"') {
            ++p;
        }
        *cursor = p;
        return t;
    }
    t.begin = p;
    while (*p != 0 && !IsSpace(*p)) {
        ++p;
    }
    t.len = static_cast<size_t>(p - t.begin);
    t.present = t.len != 0;
    *cursor = p;
    return t;
}

// `load` and `mark` take everything left on the line, so an unquoted path with spaces works and a
// quoted one has its quotes removed. Nothing may follow a quoted value: `load "a" b` is a typo,
// and silently opening "a" would hide it.
Token TakeRest(const wchar_t** cursor, bool* trailingJunk) {
    *trailingJunk = false;
    const wchar_t* p = SkipSpace(*cursor);
    if (*p == 0) {
        *cursor = p;
        return Token();
    }
    if (*p == L'"') {
        Token t = NextToken(&p);
        if (*SkipSpace(p) != 0) {
            *trailingJunk = true;
        }
        *cursor = p;
        return t;
    }
    Token t;
    t.begin = p;
    const wchar_t* end = p;
    while (*end != 0) {
        ++end;
    }
    while (end > p && IsSpace(*(end - 1))) {
        --end;
    }
    t.len = static_cast<size_t>(end - p);
    t.present = t.len != 0;
    *cursor = end;
    return t;
}

// A token made entirely of whitespace, or of nothing at all. `load ""` and `load "  "` are the two
// spellings that reach it.
bool AllSpace(const Token& t) {
    for (size_t i = 0; i < t.len; ++i) {
        if (!IsSpace(t.begin[i])) {
            return false;
        }
    }
    return true;
}

bool CopyText(const Token& t, wchar_t* out, size_t cap) {
    if (t.len + 1 > cap) {
        return false;
    }
    for (size_t i = 0; i < t.len; ++i) {
        out[i] = t.begin[i];
    }
    out[t.len] = 0;
    return true;
}

// A sanity bound, NOT the engine's fast-forward ceiling. The real ceiling is
// ui::RateCeiling, which depends on the machine's frame period and is reported by the panel;
// a number here would be a second, wrong answer to the same question.
constexpr double kRateSanityLimit = 100.0;
// 31 years, which keeps every `seconds * 1e6` inside the int64 range with room to spare.
constexpr double kMaxSeconds = 1.0e9;
constexpr int64_t kInt32Min = -2147483647 - 1;
constexpr int64_t kInt32Max = 2147483647;

const CommandSpec kSpecs[] = {
    {CommandId::Help,   "help",   "help",
     "list every command and what it takes", kNeedsNothing},
    {CommandId::List,   "list",   "list",
     "open the replay browser and rescan the replay folder", kNeedsNothing},
    {CommandId::Load,   "load",   "load <file>",
     "load a capture; quote the path only if it has trailing spaces", kNeedsNothing},
    {CommandId::Play,   "play",   "play",
     "resume at 1x; use speed for any other rate", kNeedsTransport | kNeedsLoaded},
    {CommandId::Pause,  "pause",  "pause",
     "hold the playhead where it is", kNeedsTransport | kNeedsLoaded},
    {CommandId::Speed,  "speed",  "speed <x>",
     "playback rate; negative runs backwards, 0 pauses", kNeedsTransport | kNeedsLoaded},
    {CommandId::Reverse, "reverse", "reverse",
     "flip the direction and keep the transport state: paused stays paused, exactly as the "
     "player bar's Reverse button does. `speed -1` and `rate -1` are unchanged",
     kNeedsTransport | kNeedsLoaded},
    {CommandId::Rate,   "rate",   "rate <x>",
     "set the signed rate WITHOUT resuming; a paused viewer stays paused. `speed` is unchanged",
     kNeedsTransport | kNeedsLoaded},
    {CommandId::Scrub,  "scrub",  "scrub begin | to <t|pct> | end [<t|pct>]",
     "drive a timeline drag with no mouse: begin pauses and mutes, `to` overwrites the one pending "
     "target, end settles and restores the pre-drag state",
     kNeedsTransport | kNeedsLoaded},
    {CommandId::Debug,  "debug",  "debug panel on|off",
     "show or hide the old detail panel; the player bar is the default UI and is unaffected",
     kNeedsNothing},
    {CommandId::Bar,    "bar",    "bar on|off|toggle",
     "show or dismiss the player bar. F7 does the same in the game window. `off` stays off until "
     "it is turned back on, including through a live capture",
     kNeedsNothing},
    {CommandId::Seek,   "seek",   "seek <t|pct|+/-t|@t>",
     "jump INTO THE PLAYABLE SPAN: 90 (seconds), 1:23.5 (m:s), 1:02:03 (h:m:s) or 50% are all "
     "measured from where the recorded content starts, the same axis the timeline draws. `@t` is "
     "the same thing said explicitly; +10 or -1:30 nudge from the playhead. `seekfile` is the "
     "whole-file axis",
     kNeedsTransport | kNeedsLoaded},
    {CommandId::SeekFile, "seekfile", "seekfile <t|pct|+/-t>",
     "the same jump on the WHOLE FILE's axis, including the dead air before the content starts -- "
     "what plain `seek` used to mean. `seekfile 50%` is the middle of the file, not of the match",
     kNeedsTransport | kNeedsLoaded},
    {CommandId::Step,   "step",   "step [+/-n]",
     "advance or rewind n recorded ticks; one tick if n is omitted", kNeedsTransport | kNeedsLoaded},
    {CommandId::Round,  "round",  "round <n>",
     "jump to a round, numbered as the timeline labels it", kNeedsTransport | kNeedsLoaded},
    {CommandId::Cam,    "cam",    "cam <pov N|follow N|level N|disc|side|free>",
     "select the replay camera; the modes are the engine's own", kNeedsCamera},
    {CommandId::Mode,   "mode",   "mode <ground|asseen>",
     "play ground truth, or what a player actually saw", kNeedsTransport},
    {CommandId::Mark,   "mark",   "mark [label]",
     "bookmark the current playhead in the sidecar beside the file",
     kNeedsTransport | kNeedsLoaded},
    {CommandId::Marks,  "marks",  "marks [page]",
     "list the bookmarks in this capture's sidecar, 20 to a page", kNeedsLoaded},
    {CommandId::Goto,   "goto",   "goto <n|next|prev|resume>",
     "jump to bookmark n as `marks` numbers it, to the nearest one after or before the "
     "playhead, or to where you left off",
     kNeedsTransport | kNeedsLoaded},
    {CommandId::Info,   "info",   "info",
     "one line: what the transport and the UI are doing", kNeedsNothing},
    {CommandId::Diag,   "diag",   "diag",
     "the degradation list, the control surfaces, and what each refused", kNeedsNothing},
    {CommandId::Console, "console", "console close",
     "shut this console window down; Echo VR and the panel are unaffected", kNeedsNothing},
    {CommandId::Unload, "unload", "unload",
     "close the capture and release it", kNeedsTransport | kNeedsLoaded},
    {CommandId::Dress,  "dress",  "dress [force]",
     "dress the recorded roster: names, numbers, teams, colours and loadouts. `force` runs the "
     "ladder over players it already dressed too",
     kNeedsLoaded},
    // ARCH-17's commanded-pose family. The NAME carries the sub-word, so `help` and
    // GET /replay/help render these three from the one table like every other row. A name with a
    // space is never the line's first word, so the command-word lookup below skips these rows and
    // the `cam` arm claims them -- see the guard there.
    //
    // kNeedsCamera ONLY. Not kNeedsTransport and not kNeedsLoaded: moving the camera around an
    // idle spectator view is a legitimate thing to do, and ARCH-17 S2.1 rules the transport is not
    // this family's business.
    {CommandId::CamMove, "cam move", "cam move <dx dy dz | to x y z>",
     "place the camera: an offset in metres, or `to` for world coordinates", kNeedsCamera},
    {CommandId::CamLook, "cam look", "cam look <yaw pitch | to yaw pitch | at n>",
     "aim the camera in degrees, or at the subject a selector names", kNeedsCamera},
    {CommandId::CamRelease, "cam release", "cam release",
     "hand the pose back to the stock free camera", kNeedsCamera},
    // ARCH-23 S14.11's flight window. kNeedsCamera, and NOT kNeedsTransport or kNeedsLoaded:
    // the marker is queued for the camera module to stamp inside its next census window, so with
    // no module bound nothing would ever drain it and a cheerful "window is open" would be an
    // answer to a question nobody could ask. Flying an idle spectator view is legitimate and the
    // transport is not this verb's business, which is the same rule the pose family follows.
    {CommandId::CamFlight, "cam flight", "cam flight <begin|end>",
     "bracket a stretch of your own free flight so the run can score it", kNeedsCamera},
    {CommandId::Census, "census", "census",
     "write the engine's own actor-component index to the log: which actors carry an EchoUnit, an "
     "ncaModel or a body, beside the actor each user slot resolves to",
     kNeedsLoaded},
    {CommandId::Record, "record", "record start|stop|status",
     "start or stop a recording by hand. With capture.auto_record on, live matches record "
     "themselves and this only stops one early",
     kNeedsNothing},
    // kNeedsNothing, and deliberately: the arm this turns off is the recorded-state reconcile, and
    // the whole point of the verb is to flip it between two passes of the same loaded session.
    {CommandId::AudioWrites, "audio", "audio writes on|off|status",
     "turn the recorded Wwise state, switch, memo, stop and loop re-arm writes off and on again "
     "without reloading, so the audio-cutoff A/B runs inside one session",
     kNeedsNothing},
};

constexpr size_t kSpecCount = sizeof kSpecs / sizeof kSpecs[0];

bool Fail(ParseResult* r) {
    r->ok = false;
    return false;
}

// Every refusal names the command, what was wrong, and the input that caused it.
bool FailToken(ParseResult* r, const char* cmd, const char* what, const Token& t) {
    Msg m(r->error, kMaxErrorChars);
    m.Str(cmd);
    m.Str(": ");
    m.Str(what);
    if (t.present) {
        m.Str(", got ");
        m.Quoted(t.begin, t.len);
    }
    return Fail(r);
}

void FailSelectorRange(ParseResult* r, const char* verbHead, const char* verbTail,
                       int64_t got) {
    FormatSelectorOutOfRange(r->error, kMaxErrorChars, verbHead, verbTail, got);
    Fail(r);
}

bool FailPlain(ParseResult* r, const char* cmd, const char* what) {
    Msg m(r->error, kMaxErrorChars);
    m.Str(cmd);
    m.Str(": ");
    m.Str(what);
    return Fail(r);
}

// Extra words are refused rather than ignored. `pause now` is a typo, and a parser that drops the
// second word teaches the user it was accepted.
bool ExpectEnd(ParseResult* r, const char* cmd, const wchar_t* cursor) {
    const wchar_t* p = cursor;
    const Token extra = NextToken(&p);
    if (extra.present) {
        return FailToken(r, cmd, "takes no further arguments", extra);
    }
    return true;
}

// Lowercases into `out`. False when the word does not fit, which is refused rather than truncated:
// a shortened mode word could name a different mode.
bool CopyModeWord(const Token& t, char* out, size_t cap) {
    if (t.len + 1 > cap) {
        return false;
    }
    for (size_t i = 0; i < t.len; ++i) {
        const wchar_t c = LowerAscii(t.begin[i]);
        if (c > 0x7E) {
            return false;   // no engine mode name is non-ASCII
        }
        out[i] = static_cast<char>(c);
    }
    out[t.len] = 0;
    return true;
}

// Reduces a yaw into (-180, 180]. No fmod, because this file has no math.h, and no subtract loop,
// because a typed 1000000 degrees would spin it. The int64 truncation is exact for every value
// kCamYawMaxDeg admits.
double NormaliseYawDeg(double deg) {
    const double turns  = deg / 360.0;
    const double whole  = static_cast<double>(static_cast<int64_t>(turns));
    double       y      = deg - whole * 360.0;
    if (y <= -180.0) {
        y += 360.0;
    }
    if (y > 180.0) {
        y -= 360.0;
    }
    return y;
}

// One number of a commanded pose. The range test is a negated conjunction rather than a pair of
// `>` tests, so a value that is not ordered against anything takes the refusal instead of slipping
// past a comparison that is false for it. ParseDecimal refuses inf, nan and anything over 18
// digits before this sees it; this is the second lock on the same door, not the first.
bool TakeNumber(ParseResult* r, const char* cmd, const wchar_t** cursor, const char* missing,
                double lo, double hi, const char* rangeWhat, double* out) {
    const Token t = NextToken(cursor);
    if (!t.present) {
        FailPlain(r, cmd, missing);
        return false;
    }
    double         v   = 0.0;
    const wchar_t* end = nullptr;
    if (!ParseDecimal(t.begin, &v, &end) || end != t.begin + t.len) {
        FailToken(r, cmd, "expected a number", t);
        return false;
    }
    if (!(v >= lo && v <= hi)) {
        FailToken(r, cmd, rangeWhat, t);
        return false;
    }
    *out = v;
    return true;
}

// True if the next token is `word`, consuming it. Used for the `to` / `at` sub-forms, which are
// optional: a peek that does not match must leave the cursor where a number is expected.
bool TakeWord(const wchar_t** cursor, const char* word) {
    const wchar_t* probe = *cursor;
    const Token    t     = NextToken(&probe);
    if (!t.present || !TokenIs(t.begin, t.len, word)) {
        return false;
    }
    *cursor = probe;
    return true;
}

bool ParseCamMove(ParseResult* r, const wchar_t** cursor) {
    const char* const name = "cam move";
    r->cmd.camAbsolute     = TakeWord(cursor, "to");
    const char* const missing =
        r->cmd.camAbsolute ? "needs three world coordinates in metres, as in `cam move to 4 2 -1`"
                           : "needs three offsets in metres, as in `cam move 1 0 0`";
    for (int i = 0; i < 3; ++i) {
        double v = 0.0;
        if (!TakeNumber(r, name, cursor, missing, -kCamMoveMaxMetres, kCamMoveMaxMetres,
                        "a component is out of range; metres must be within +/-10000", &v)) {
            return false;
        }
        r->cmd.camVec[i] = static_cast<float>(v);
    }
    return ExpectEnd(r, name, *cursor);
}

bool ParseCamLook(ParseResult* r, const wchar_t** cursor) {
    const char* const name = "cam look";

    if (TakeWord(cursor, "at")) {
        r->cmd.camLookAt = true;
        const Token sel  = NextToken(cursor);
        if (!sel.present) {
            FailPlain(r, name, "needs a subject index, as in `cam look at 3`");
            return false;
        }
        int64_t        n   = 0;
        const wchar_t* end = nullptr;
        if (!ParseInt64(sel.begin, &n, &end) || end != sel.begin + sel.len) {
            FailToken(r, name, "expected a subject index", sel);
            return false;
        }
        if (!IsPlayerSelector(n)) {
            FailSelectorRange(r, name, "at", n);
            return false;
        }
        r->cmd.whole = n;
        return ExpectEnd(r, name, *cursor);
    }

    r->cmd.camAbsolute        = TakeWord(cursor, "to");
    const char* const missing = "needs a yaw and a pitch in degrees, as in `cam look 90 0`";
    double            yaw     = 0.0;
    double            pitch   = 0.0;
    if (!TakeNumber(r, name, cursor, missing, -kCamYawMaxDeg, kCamYawMaxDeg,
                    "yaw must be a finite number of degrees", &yaw)) {
        return false;
    }
    if (!TakeNumber(r, name, cursor, missing, -kCamPitchLimitDeg, kCamPitchLimitDeg,
                    "pitch is out of range; +/-90 is degenerate, so this is refused not clamped",
                    &pitch)) {
        return false;
    }
    r->cmd.camVec[0] = static_cast<float>(NormaliseYawDeg(yaw));
    r->cmd.camVec[1] = static_cast<float>(pitch);
    return ExpectEnd(r, name, *cursor);
}

// A spec name with a space in it is a sub-word form (`cam move`), which the line's first word can
// never be -- except through a quoted `"cam move"`, and that must not become a second spelling of
// the command. The `cam` arm claims those three words itself.
bool IsSubWordSpec(const char* specName) {
    for (const char* p = specName; *p != 0; ++p) {
        if (*p == ' ') {
            return true;
        }
    }
    return false;
}

}  // namespace

const char* CommandName(CommandId id) {
    const CommandSpec* s = SpecFor(id);
    return s != nullptr ? s->name : "none";
}

size_t SpecCount() { return kSpecCount; }

const CommandSpec& SpecAt(size_t i) { return kSpecs[i < kSpecCount ? i : 0]; }

const CommandSpec* SpecFor(CommandId id) {
    for (size_t i = 0; i < kSpecCount; ++i) {
        if (kSpecs[i].id == id) {
            return &kSpecs[i];
        }
    }
    return nullptr;
}

bool ParseDecimal(const wchar_t* s, double* out, const wchar_t** end) {
    if (s == nullptr || out == nullptr) {
        return false;
    }
    const wchar_t* p = s;
    bool neg = false;
    if (*p == L'+' || *p == L'-') {
        neg = (*p == L'-');
        ++p;
    }
    double v = 0.0;
    size_t intDigits = 0;
    bool   sawInt = false;
    while (IsDigit(*p)) {
        sawInt = true;
        // 18 digits keeps the value inside the range where a later multiply cannot reach infinity.
        // Beyond that we refuse rather than accumulate, because an infinite rate would propagate
        // into the transport and the clock.
        //
        // ★ MAGNITUDE, NOT KEYSTROKES. This counted every digit, leading zeros included, so a
        // zero-padded value from a script or a paste -- `speed 000000000000000000001` -- was
        // reported as "expected a number". Zeros before the first significant digit change nothing
        // about the value, so they cost nothing against the limit.
        if (v != 0.0 || *p != L'0') {
            if (++intDigits > 18) {
                return false;
            }
        }
        v = v * 10.0 + static_cast<double>(*p - L'0');
        ++p;
    }
    size_t fracDigits = 0;
    if (*p == L'.') {
        ++p;
        double scale = 0.1;
        while (IsDigit(*p)) {
            ++fracDigits;
            if (fracDigits <= 18) {
                v += static_cast<double>(*p - L'0') * scale;
                scale *= 0.1;
            }
            ++p;
        }
    }
    if (!sawInt && fracDigits == 0) {
        return false;
    }
    *out = neg ? -v : v;
    if (end != nullptr) {
        *end = p;
    }
    return true;
}

bool ParseInt64(const wchar_t* s, int64_t* out, const wchar_t** end) {
    if (s == nullptr || out == nullptr) {
        return false;
    }
    const wchar_t* p = s;
    bool neg = false;
    if (*p == L'+' || *p == L'-') {
        neg = (*p == L'-');
        ++p;
    }
    if (!IsDigit(*p)) {
        return false;
    }
    const uint64_t limit = neg ? 9223372036854775808ull : 9223372036854775807ull;
    uint64_t v = 0;
    while (IsDigit(*p)) {
        const uint64_t d = static_cast<uint64_t>(*p - L'0');
        if (v > (limit - d) / 10) {
            return false;
        }
        v = v * 10 + d;
        ++p;
    }
    // Negation in unsigned, then one conversion, so INT64_MIN is representable without overflowing
    // a signed negate. C++20 fixes that conversion as two's complement.
    *out = neg ? static_cast<int64_t>(0u - v) : static_cast<int64_t>(v);
    if (end != nullptr) {
        *end = p;
    }
    return true;
}

StepOutcome ClassifyStep(int64_t beforeUs, int64_t afterUs, int lastStepTicks) {
    if (lastStepTicks != kStepTicksUncounted) {
        return lastStepTicks == 0 ? StepOutcome::NoTickThatWay : StepOutcome::Moved;
    }
    return beforeUs == afterUs ? StepOutcome::NoTickThatWay : StepOutcome::Moved;
}

bool ParseTimeSpec(const wchar_t* s, int64_t* outUs, double* outPercent, bool* isPercent) {
    if (s == nullptr || outUs == nullptr || outPercent == nullptr || isPercent == nullptr) {
        return false;
    }
    *outUs = 0;
    *outPercent = 0.0;
    *isPercent = false;

    // ★ THE SIGN IS CHECKED BEFORE IT CAN BE DROPPED. The clock form used to validate each leading
    // field with `lead[i] < 0`, which cannot fire for a field that parsed to 0 -- so `seek -5` was
    // refused and `seek -0:30` gave +30 s. There is no negative time on a capture, so a leading
    // minus is refused here once, for every form.
    if (s[0] == L'-') {
        return false;
    }

    int colons = 0;
    for (const wchar_t* q = s; *q != 0; ++q) {
        if (*q == L':') {
            ++colons;
        }
    }
    if (colons > 2) {
        return false;
    }

    if (colons > 0) {
        int64_t lead[2] = {0, 0};
        const wchar_t* p = s;
        for (int i = 0; i < colons; ++i) {
            const wchar_t* end = nullptr;
            if (!ParseInt64(p, &lead[i], &end) || *end != L':' || lead[i] < 0) {
                return false;
            }
            p = end + 1;
        }
        double secs = 0.0;
        const wchar_t* end = nullptr;
        if (!ParseDecimal(p, &secs, &end) || *end != 0) {
            return false;
        }
        // The leading field is the largest unit and does not wrap, so 90:30 is ninety minutes.
        // Every field below it must be a real clock field: 1:90 is a typo, not 150 seconds.
        if (secs < 0.0 || secs >= 60.0) {
            return false;
        }
        const int64_t hours = (colons == 2) ? lead[0] : 0;
        const int64_t minutes = (colons == 2) ? lead[1] : lead[0];
        if (colons == 2 && minutes >= 60) {
            return false;
        }
        const double total = static_cast<double>(hours) * 3600.0 +
                             static_cast<double>(minutes) * 60.0 + secs;
        if (total > kMaxSeconds) {
            return false;
        }
        *outUs = static_cast<int64_t>(total * 1000000.0 + 0.5);
        return true;
    }

    double v = 0.0;
    const wchar_t* end = nullptr;
    if (!ParseDecimal(s, &v, &end)) {
        return false;
    }
    if (*end == L'%') {
        if (*(end + 1) != 0) {
            return false;
        }
        // Refused, not clamped. A typed 500% is a mistake, and jumping silently to the end is the
        // worst available interpretation of it.
        if (v < 0.0 || v > 100.0) {
            return false;
        }
        *outPercent = v;
        *isPercent = true;
        return true;
    }
    if (*end != 0 || v < 0.0 || v > kMaxSeconds) {
        return false;
    }
    *outUs = static_cast<int64_t>(v * 1000000.0 + 0.5);
    return true;
}

ParseResult Parse(const wchar_t* line) {
    ParseResult r;
    if (line == nullptr) {
        Msg m(r.error, kMaxErrorChars);
        m.Str("no command");
        r.ok = false;
        return r;
    }

    if (HasUnterminatedQuote(line)) {
        Msg m(r.error, kMaxErrorChars);
        m.Str("parse: unterminated quote");
        r.ok = false;
        return r;
    }

    const wchar_t* cursor = line;
    const Token word = NextToken(&cursor);
    if (!word.present) {
        // A blank line is not an error. It is what Enter does.
        r.ok = true;
        r.cmd.id = CommandId::None;
        return r;
    }

    const CommandSpec* spec = nullptr;
    for (size_t i = 0; i < kSpecCount; ++i) {
        if (IsSubWordSpec(kSpecs[i].name)) {
            continue;
        }
        if (TokenIs(word.begin, word.len, kSpecs[i].name)) {
            spec = &kSpecs[i];
            break;
        }
    }
    if (spec == nullptr) {
        Msg m(r.error, kMaxErrorChars);
        m.Str("unknown command ");
        m.Quoted(word.begin, word.len);
        m.Str("; type `help`");
        r.ok = false;
        return r;
    }

    r.cmd.id = spec->id;
    const char* const name = spec->name;

    switch (spec->id) {
        case CommandId::Help:
        case CommandId::List:
        case CommandId::Play:
        case CommandId::Pause:
        case CommandId::Reverse:
        case CommandId::Info:
        case CommandId::Diag:
        case CommandId::Census:
        case CommandId::Unload: {
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            if (spec->id == CommandId::Play) {
                r.cmd.rate = 1.0;
            }
            break;
        }

        // `dress` dresses what is not dressed yet; `dress force` runs the ladder over every slot
        // again. The two are separate words because a re-dress is not free: each pass over an
        // already-dressed player attaches a fresh profile document and the engine abandons the
        // previous one, so repeating it costs memory the plugin cannot get back.
        case CommandId::Dress: {
            const Token t = NextToken(&cursor);
            if (t.present) {
                char sub[8] = {0};
                static const char kForce[] = "force";
                bool isForce = CopyModeWord(t, sub, sizeof sub);
                for (size_t i = 0; isForce && i < sizeof kForce; ++i) isForce = sub[i] == kForce[i];
                if (!isForce) {
                    FailToken(&r, name, "takes nothing, or the word `force`", t);
                    return r;
                }
                r.cmd.dressForce = true;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Load: {
            bool junk = false;
            const Token t = TakeRest(&cursor, &junk);
            if (!t.present) {
                FailPlain(&r, name, "needs a file, as in `load match.echodemo`");
                return r;
            }
            // ★ AN EMPTY NAME IS NOT A REQUEST FOR THE MOST RECENT CAPTURE. `load ""` produced a
            // present token of length zero, RunLoad read the empty text as "no argument", and the
            // owner who asked for a file he had named got a different capture opened and was told
            // it worked. `load "  "` is the same shape one step along.
            if (AllSpace(t)) {
                FailPlain(&r, name,
                          t.len == 0
                              ? "an empty name is not a file. Name the capture, or use the browser."
                              : "that name is only spaces. Name the capture, or use the browser.");
                return r;
            }
            if (junk) {
                FailPlain(&r, name, "a quoted path must be the whole argument");
                return r;
            }
            if (!CopyText(t, r.cmd.text, kMaxPathChars)) {
                Msg m(r.error, kMaxErrorChars);
                m.Str("load: path is ");
                m.Int(static_cast<int64_t>(t.len));
                m.Str(" characters; the limit is ");
                m.Int(static_cast<int64_t>(kMaxPathChars - 1));
                r.ok = false;
                return r;
            }
            break;
        }

        case CommandId::Mark: {
            bool junk = false;
            const Token t = TakeRest(&cursor, &junk);
            if (junk) {
                FailPlain(&r, name, "a quoted label must be the whole argument");
                return r;
            }
            // Against the sidecar's own cap rather than the path buffer: a label accepted here and
            // refused there produced a refusal naming two possible causes.
            if (t.present && !CopyText(t, r.cmd.text, kMaxMarkLabelChars)) {
                Msg m(r.error, kMaxErrorChars);
                m.Str("mark: the label is ");
                m.Int(static_cast<int64_t>(t.len));
                m.Str(" characters; the limit is ");
                m.Int(static_cast<int64_t>(kMaxMarkLabelChars - 1));
                r.ok = false;
                return r;
            }
            break;
        }

        case CommandId::Marks: {
            const Token t = NextToken(&cursor);
            r.cmd.whole = 1;
            if (t.present) {
                int64_t v = 0;
                const wchar_t* end = nullptr;
                if (!ParseInt64(t.begin, &v, &end) || end != t.begin + t.len) {
                    FailToken(&r, name, "expected a page number", t);
                    return r;
                }
                if (v < 1 || v > kInt32Max) {
                    FailToken(&r, name, "pages start at 1", t);
                    return r;
                }
                r.cmd.whole = v;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Goto: {
            const Token t = NextToken(&cursor);
            if (!t.present) {
                FailPlain(&r, name,
                          "needs a bookmark number as `marks` lists them, `next` or `prev` for "
                          "the nearest one, or `resume` for where you left off");
                return r;
            }
            if (TokenIs(t.begin, t.len, "resume")) {
                r.cmd.gotoResume = true;
            } else if (TokenIs(t.begin, t.len, "next")) {
                r.cmd.gotoNext = true;
            } else if (TokenIs(t.begin, t.len, "prev")) {
                r.cmd.gotoPrev = true;
            } else {
                int64_t v = 0;
                const wchar_t* end = nullptr;
                if (!ParseInt64(t.begin, &v, &end) || end != t.begin + t.len) {
                    FailToken(&r, name, "expected a bookmark number, `next`, `prev` or `resume`",
                              t);
                    return r;
                }
                if (v < 1 || v > kInt32Max) {
                    FailToken(&r, name, "bookmarks are numbered from 1", t);
                    return r;
                }
                r.cmd.whole = v;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Speed: {
            const Token t = NextToken(&cursor);
            if (!t.present) {
                FailPlain(&r, name, "needs a rate, as in `speed 0.5`");
                return r;
            }
            double v = 0.0;
            const wchar_t* end = nullptr;
            if (!ParseDecimal(t.begin, &v, &end) || end != t.begin + t.len) {
                FailToken(&r, name, "expected a number", t);
                return r;
            }
            if (v < -kRateSanityLimit || v > kRateSanityLimit) {
                FailToken(&r, name, "rate is outside -100..100", t);
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            r.cmd.rate = v;
            break;
        }

        case CommandId::Rate: {
            const Token t = NextToken(&cursor);
            if (!t.present) {
                FailPlain(&r, name, "needs a rate, as in `rate -2`");
                return r;
            }
            double v = 0.0;
            const wchar_t* end = nullptr;
            if (!ParseDecimal(t.begin, &v, &end) || end != t.begin + t.len) {
                FailToken(&r, name, "expected a number", t);
                return r;
            }
            if (v < -kRateSanityLimit || v > kRateSanityLimit) {
                FailToken(&r, name, "rate is outside -100..100", t);
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            r.cmd.rate = v;
            break;
        }

        case CommandId::Scrub: {
            const Token phase = NextToken(&cursor);
            if (!phase.present) {
                FailPlain(&r, name, "needs begin, to or end");
                return r;
            }
            bool wantsTarget = false;
            bool allowsTarget = false;
            if (TokenIs(phase.begin, phase.len, "begin")) {
                r.cmd.scrubPhase = Command::ScrubPhase::Begin;
            } else if (TokenIs(phase.begin, phase.len, "to")) {
                r.cmd.scrubPhase = Command::ScrubPhase::To;
                wantsTarget = allowsTarget = true;
            } else if (TokenIs(phase.begin, phase.len, "end")) {
                r.cmd.scrubPhase = Command::ScrubPhase::End;
                allowsTarget = true;
            } else {
                FailToken(&r, name, "expected begin, to or end", phase);
                return r;
            }
            const Token target = NextToken(&cursor);
            if (!target.present) {
                if (wantsTarget) {
                    FailPlain(&r, name,
                              "`scrub to` needs a time or a percentage, as in `scrub to 50%`");
                    return r;
                }
                if (!ExpectEnd(&r, name, cursor)) {
                    return r;
                }
                break;
            }
            if (!allowsTarget) {
                FailToken(&r, name, "takes no argument", target);
                return r;
            }
            // ★ THE RELATIVE FORM IS REFUSED BY ITS SIGN, HERE. ParseTimeSpec takes `+10` as ten
            // seconds, so without this `scrub to +10` would silently mean a position rather than
            // the nudge it reads as -- and a nudge into a slot that coalesces would compound
            // however many times it survived a tick.
            if (target.len > 0 && (target.begin[0] == L'+' || target.begin[0] == L'-')) {
                FailToken(&r, name, "takes a position, not a relative nudge -- use `seek`", target);
                return r;
            }
            wchar_t sbuf[64];
            if (!CopyText(target, sbuf, sizeof sbuf / sizeof sbuf[0])) {
                FailToken(&r, name, "is too long to be a time", target);
                return r;
            }
            // The same ParseTimeSpec `seek` uses, so `50%` and `1:23.5` mean here what they mean
            // there. The relative form is deliberately NOT accepted: a drag target is a position,
            // and `+10` into a slot that coalesces would compound however many times it survived.
            if (!ParseTimeSpec(sbuf, &r.cmd.whole, &r.cmd.percent, &r.cmd.seekIsPercent)) {
                FailToken(&r, name, "expected a time like 90, 1:23.5 or 50%", target);
                return r;
            }
            r.cmd.scrubHasTarget = true;
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Debug: {
            // One subject today, and it is spelled out rather than made optional: `debug` alone
            // would have to mean something, and every meaning it could take is a surprise.
            const Token subject = NextToken(&cursor);
            if (!subject.present) {
                FailPlain(&r, name, "needs a subject: `debug panel on` or `debug panel off`");
                return r;
            }
            if (!TokenIs(subject.begin, subject.len, "panel")) {
                FailToken(&r, name, "expected panel", subject);
                return r;
            }
            const Token state = NextToken(&cursor);
            if (!state.present) {
                FailPlain(&r, name, "needs on or off");
                return r;
            }
            if (TokenIs(state.begin, state.len, "on")) {
                r.cmd.debugOn = true;
            } else if (TokenIs(state.begin, state.len, "off")) {
                r.cmd.debugOn = false;
            } else {
                FailToken(&r, name, "expected on or off", state);
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Record: {
            const Token verb = NextToken(&cursor);
            if (!verb.present) {
                FailPlain(&r, name, "needs start, stop or status");
                return r;
            }
            if (TokenIs(verb.begin, verb.len, "start")) {
                r.cmd.recordVerb = Command::RecordVerb::Start;
            } else if (TokenIs(verb.begin, verb.len, "stop")) {
                r.cmd.recordVerb = Command::RecordVerb::Stop;
            } else if (TokenIs(verb.begin, verb.len, "status")) {
                r.cmd.recordVerb = Command::RecordVerb::Status;
            } else {
                FailToken(&r, name, "expected start, stop or status", verb);
                return r;
            }
            break;
        }

        case CommandId::AudioWrites: {
            // `audio` is the family, `writes` the only member of it today. Spelt out rather than
            // folded into the verb so a second `audio <something>` costs a branch here and not a
            // second top-level word for a subject that already has one.
            const Token family = NextToken(&cursor);
            if (!family.present || !TokenIs(family.begin, family.len, "writes")) {
                FailPlain(&r, name, "needs `writes on`, `writes off` or `writes status`");
                return r;
            }
            const Token state = NextToken(&cursor);
            if (!state.present) {
                FailPlain(&r, name, "needs on, off or status after `writes`");
                return r;
            }
            if (TokenIs(state.begin, state.len, "on")) {
                r.cmd.audioWritesVerb = Command::AudioWritesVerb::On;
            } else if (TokenIs(state.begin, state.len, "off")) {
                r.cmd.audioWritesVerb = Command::AudioWritesVerb::Off;
            } else if (TokenIs(state.begin, state.len, "status")) {
                r.cmd.audioWritesVerb = Command::AudioWritesVerb::Status;
            } else {
                FailToken(&r, name, "expected on, off or status", state);
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Bar: {
            const Token state = NextToken(&cursor);
            if (!state.present) {
                FailPlain(&r, name, "needs on, off or toggle");
                return r;
            }
            if (TokenIs(state.begin, state.len, "on")) {
                r.cmd.barMode = Command::BarMode::On;
            } else if (TokenIs(state.begin, state.len, "off")) {
                r.cmd.barMode = Command::BarMode::Off;
            } else if (TokenIs(state.begin, state.len, "toggle")) {
                r.cmd.barMode = Command::BarMode::Toggle;
            } else {
                FailToken(&r, name, "expected on, off or toggle", state);
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Seek:
        case CommandId::SeekFile: {
            const bool wholeFile = r.cmd.id == CommandId::SeekFile;
            const Token t = NextToken(&cursor);
            if (!t.present) {
                FailPlain(&r, name, "needs a time or a percentage, as in `seek 1:23` or `seek 50%`");
                return r;
            }
            wchar_t buf[64];
            if (!CopyText(t, buf, sizeof buf / sizeof buf[0])) {
                FailToken(&r, name, "is too long to be a time", t);
                return r;
            }
            // The sign is stripped HERE and the magnitude goes through the one ParseTimeSpec every
            // other surface uses, rather than teaching it a fifth out-parameter. That also keeps
            // `--30` and `+-30` refused for free: the second sign reaches ParseTimeSpec, which
            // refuses a leading minus for every form.
            const wchar_t* mag = buf;
            bool negative = false;
            // ★ BARE `seek 12.5` IS 12.5 s INTO THE RECORDED CONTENT, not into the file. On a
            // capture whose content starts 13 minutes into its own axis the file axis is a number
            // nobody can read off the timeline, and `seek 19` silently landing on 0:00 because 19 s
            // is before the content starts is not a jump, it is a clamp (LAUNCH-LIVEFIX 4.3). The
            // window, the bar and `seek @t` were already on the span; this puts the console on the
            // same axis as the thing the owner is looking at. `seekfile` keeps the file axis for
            // anything written against it.
            if (!wholeFile) r.cmd.seekIsSpanRelative = true;
            if (buf[0] == L'@') {
                if (wholeFile) {
                    FailToken(&r, name,
                              "has no `@` form: it IS the whole-file axis. Type `seek @12.5` for "
                              "the span", t);
                    return r;
                }
                r.cmd.seekIsSpanRelative = true;
                ++mag;
                if (*mag == 0) {
                    FailToken(&r, name, "needs a time after the @, as in `seek @12.5`", t);
                    return r;
                }
            }
            if (mag[0] == L'+' || mag[0] == L'-') {
                r.cmd.seekIsRelative = true;
                negative = mag[0] == L'-';
                ++mag;
                if (*mag == 0) {
                    FailToken(&r, name, "needs a time after the sign, as in `seek +10`", t);
                    return r;
                }
                // A nudge has no axis -- it is a delta from the playhead -- so it never takes the
                // span arm, whichever spelling asked for it.
                r.cmd.seekIsSpanRelative = false;
            }
            if (!ParseTimeSpec(mag, &r.cmd.whole, &r.cmd.percent, &r.cmd.seekIsPercent)) {
                FailToken(&r, name, "expected seconds, m:s, h:m:s or a percentage", t);
                return r;
            }
            if (r.cmd.seekIsRelative && r.cmd.seekIsPercent) {
                // A relative percentage is a unit nobody typed: 10% of the file, or 10% of where
                // you are? Refused naming the token instead of picking one (ARCH-17 S8.1).
                FailToken(&r, name, "cannot nudge by a percentage; type `seek +10` or `seek 50%`",
                          t);
                return r;
            }
            if (negative) {
                r.cmd.whole = -r.cmd.whole;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Step: {
            const Token t = NextToken(&cursor);
            if (!t.present) {
                r.cmd.whole = 1;
                break;
            }
            int64_t v = 0;
            const wchar_t* end = nullptr;
            if (!ParseInt64(t.begin, &v, &end) || end != t.begin + t.len) {
                FailToken(&r, name, "expected a whole number of ticks", t);
                return r;
            }
            if (v == 0) {
                FailPlain(&r, name, "0 ticks is not a step");
                return r;
            }
            // B.8's Step takes an int, so a wider value would be truncated into a different jump.
            if (v < kInt32Min || v > kInt32Max) {
                FailToken(&r, name, "tick count is out of range", t);
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            r.cmd.whole = v;
            break;
        }

        case CommandId::Round: {
            const Token t = NextToken(&cursor);
            if (!t.present) {
                FailPlain(&r, name, "needs a round number, as in `round 2`");
                return r;
            }
            int64_t v = 0;
            const wchar_t* end = nullptr;
            if (!ParseInt64(t.begin, &v, &end) || end != t.begin + t.len) {
                FailToken(&r, name, "expected a round number", t);
                return r;
            }
            if (v < 0 || v > kInt32Max) {
                FailToken(&r, name, "round number is out of range", t);
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            r.cmd.whole = v;
            break;
        }

        case CommandId::Cam: {
            const Token t = NextToken(&cursor);
            if (!t.present) {
                FailPlain(&r, name,
                          "needs a mode. The engine's own set is pov, follow, level, disc, side "
                          "and free");
                return r;
            }
            // ARCH-17 S3.1: the commanded-pose family claims its sub-words BEFORE the word is
            // taken as a mode name. The engine's set is `pov follow level disc side free api`, so
            // there is no collision today -- and tests/control checks these three against
            // camera_control::ModeFromName, so a future engine mode called `move` is a test
            // failure rather than a silent shadow.
            if (TokenIs(t.begin, t.len, "move")) {
                r.cmd.id = CommandId::CamMove;
                if (!ParseCamMove(&r, &cursor)) {
                    return r;
                }
                break;
            }
            if (TokenIs(t.begin, t.len, "look")) {
                r.cmd.id = CommandId::CamLook;
                if (!ParseCamLook(&r, &cursor)) {
                    return r;
                }
                break;
            }
            if (TokenIs(t.begin, t.len, "release")) {
                r.cmd.id = CommandId::CamRelease;
                if (!ExpectEnd(&r, "cam release", cursor)) {
                    return r;
                }
                break;
            }
#ifndef FLIGHT_VERB_UNROUTED
            if (TokenIs(t.begin, t.len, "flight")) {
                r.cmd.id = CommandId::CamFlight;
                const Token w = NextToken(&cursor);
                if (!w.present) {
                    FailPlain(&r, "cam flight", "needs begin or end");
                    return r;
                }
                if (TokenIs(w.begin, w.len, "begin")) {
                    r.cmd.camFlightBegin = true;
                } else if (TokenIs(w.begin, w.len, "end")) {
                    r.cmd.camFlightBegin = false;
                } else {
                    FailToken(&r, "cam flight", "expected begin or end", w);
                    return r;
                }
                if (!ExpectEnd(&r, "cam flight", cursor)) {
                    return r;
                }
                break;
            }
#endif
            if (!CopyModeWord(t, r.cmd.camMode, kMaxCamModeChars)) {
                FailToken(&r, name, "is not a mode name", t);
                return r;
            }
            // The selector is optional HERE because which modes take one is a property of the
            // engine's table, not of this grammar. camera_control resolves the word and enforces
            // the per-mode rule, so there is one place that knows and it is the one reading the
            // binary.
            const Token sel = NextToken(&cursor);
            if (sel.present) {
                int64_t v = 0;
                const wchar_t* end = nullptr;
                if (!ParseInt64(sel.begin, &v, &end) || end != sel.begin + sel.len) {
                    FailToken(&r, name, "expected a selector index", sel);
                    return r;
                }
                // The player-selector rule first, so `cam pov 0` and `cam pov 11` are
                // refused by the same word and the same range as `cam look at 0`. Only a mode
                // whose selector is NOT a player number falls through to the shape bound below.
                if (IsPlayerSelectorMode(r.cmd.camMode) && !IsPlayerSelector(v)) {
                    FailSelectorRange(&r, name, r.cmd.camMode, v);
                    return r;
                }
                if (v < 0 || v > kInt32Max) {
                    FailToken(&r, name, "selector index is out of range", sel);
                    return r;
                }
                r.cmd.whole = v;
                r.cmd.camHasIndex = true;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Console: {
            // One subcommand, and it is spelled out rather than implied. `console` on its own would
            // be an ambiguous request for a window the owner is looking at, and the close box is
            // deliberately disabled -- so this is the deliberate way out, and it has to be
            // unmistakable.
            const Token t = NextToken(&cursor);
            if (!t.present || !TokenIs(t.begin, t.len, "close")) {
                FailPlain(&r, name,
                          "the only thing it does is `console close`, which closes this window and "
                          "leaves the game running");
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        case CommandId::Mode: {
            const Token t = NextToken(&cursor);
            if (!t.present) {
                FailPlain(&r, name, "needs ground or asseen");
                return r;
            }
            if (TokenIs(t.begin, t.len, "ground")) {
                r.cmd.asSeen = false;
            } else if (TokenIs(t.begin, t.len, "asseen")) {
                r.cmd.asSeen = true;
            } else {
                FailToken(&r, name, "expected ground or asseen", t);
                return r;
            }
            if (!ExpectEnd(&r, name, cursor)) {
                return r;
            }
            break;
        }

        // Reached only through the `cam` arm above, which rewrites the id and breaks out of the
        // switch; the line's first word is never `cam move`. Listed so the switch stays exhaustive
        // and a fourth family member cannot be added without landing here.
        case CommandId::CamMove:
        case CommandId::CamLook:
        case CommandId::CamRelease:
        case CommandId::CamFlight:
        case CommandId::None:
            break;
    }

    r.ok = true;
    return r;
}

bool IsPlayerSelectorMode(const char* modeWord) {
    return WordIs(modeWord, "pov") || WordIs(modeWord, "follow");
}

void FormatSelectorOutOfRange(char* out, size_t cap, const char* verbHead, const char* verbTail,
                              int64_t got) {
    Msg m(out, cap);
    m.Str(verbHead);
    if (verbTail != nullptr && verbTail[0] != 0) {
        m.Ch(' ');
        m.Str(verbTail);
    }
    m.Str(": ");
    m.Str(kSelectorOutOfRangeWord);
    m.Str(", the player selector is ");
    m.Int(kCamPlayerSelectorMin);
    m.Str(" to ");
    m.Int(kCamPlayerSelectorMax);
    m.Str(", got \"");
    m.Int(got);
    m.Ch('"');
}

}  // namespace control
}  // namespace replay
