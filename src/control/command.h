#pragma once

// W2-G. The command table and the line parser behind all three control surfaces.
//
// DESIGN 8.5.2 lists one command set and says it is "identical across all three" surfaces. That is
// only true if there is one parser, so this is it: the console window, the ECHOREPLAY_OPEN carrier and
// the HTTP routes all produce a Command through this file and push it onto one queue, which the
// module drains on the tick thread. A surface that parsed its own arguments would drift, and the
// drift would show up as "seek works in the console but not over HTTP" months later.
//
// THIS FILE TOUCHES NOTHING. No windows.h, no engine memory, no transport, no allocation, no CRT
// locale. Every decision it makes is a pure function of the line it was handed, which is why
// tests/control can sweep it on a machine with no game and why it is the T0 half of this packet's
// gate.
//
// * THE PARSER IS WIDE-CHAR NATIVE, and that is a decision rather than a habit. `load` carries a
// filesystem path; narrowing it to char and widening it back is where a path with a non-ASCII
// character stops opening. The console reads wide (ReadConsoleW), and the HTTP surface widens its
// UTF-8 body once, at its own edge, before it gets here.
//
// * NUMBERS ARE PARSED HERE RATHER THAN BY wcstod, and the reason is not taste. wcstod honours
// LC_NUMERIC. We share a process with a shipped game that may call setlocale for its own reasons,
// and in a comma-decimal locale wcstod(L"1.5") returns 1.0 and consumes one character -- so
// `speed 1.5` would silently become `speed 1`. A parser that cannot be reconfigured out from under
// us is worth thirty lines.

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace control {

// DESIGN 8.5.2's set, plus `help` and `list`, which that section's own console description names
// ("with help, list, tp, printpos and friends") and which uiapi.h calls the browser's listing.
enum class CommandId : uint8_t {
    None = 0,
    Help,
    List,
    Load,
    Play,
    Pause,
    Speed,
    Seek,
    Step,
    Round,
    Cam,
    Mode,
    Mark,
    Marks,
    Goto,
    Console,
    Info,
    Diag,
    Unload,
    // ARCH-17 S3.1, the commanded-pose family. APPENDED, so every value above keeps its number.
    // These place the camera; they never fly it (RULING FC-2), and they never touch the transport.
    CamMove,        // cam move <dx> <dy> <dz>  |  cam move to <x> <y> <z>
    CamLook,        // cam look <yaw> <pitch>   |  cam look to <yaw> <pitch>  |  cam look at <n>
    CamRelease,     // cam release -- hand the pose back to the stock free camera
    // ARCH-23 S14.11 (W2-G-32a). APPENDED for the same reason the three above were.
    CamFlight,      // cam flight begin | cam flight end -- bracket the owner's own free flight
    // NAMES-3. APPENDED for the same reason: every value above keeps its number.
    Dress,          // dress [force] -- dress the recorded roster; `force` re-dresses dressed slots
    // DESIGN-UI-2 S7.2. APPENDED for the same reason every family above it was.
    Rate,           // rate <x> -- the signed rate WITHOUT touching pause. `speed` is unchanged.
    Scrub,          // scrub begin | to <t> | end [<t>] -- drive a drag with no mouse
    // DESIGN-UI-2 S7.2 (P2). APPENDED for the same reason: every value above keeps its number.
    Debug,          // debug panel on|off -- the old panel, which the bar demoted to opt-in
    // BAR-FIX. APPENDED for the same reason every family above it was.
    Bar,            // bar on|off|toggle -- the player bar itself, the F7 key's other hand
    // RECORDER-2 part D. APPENDED for the same reason every family above it was.
    Record,         // record start|stop|status -- the manual door when capture.auto_record is off
    // POLISH-2. APPENDED for the same reason every family above it was.
    Reverse,        // reverse -- flip the direction, keeping pause, as the bar's button does
    // MULTI-LOAD. APPENDED for the same reason every family above it was: a value inserted beside
    // `Seek`, where it belongs by meaning, renumbers eight commands and a test pins those numbers.
    SeekFile,       // seekfile <t|pct|+/-t> -- the WHOLE FILE's axis, what plain `seek` used to be
    // PRESENT-G. APPENDED for the same reason every family above it was.
    Census,         // census -- the engine's actor-index table for the component pools, to the log
    // AUDIO-FIX-2. APPENDED for the same reason every family above it was.
    AudioWrites,    // audio writes on|off|status -- the cutoff A/B's two arms, inside one launch
};

// * `cam` CARRIES ITS MODE AS A WORD, NOT AS AN ENUM OF OURS, and that is a correction.
//
// DESIGN 8.5.2 spells the set `free|pov N|follow N|orbit N|level N|asseen`. The engine's set,
// read by W2-E out of CAM.HTTP_CAMERA_MODE_HANDLER, is `pov follow level disc side free api` --
// so the design names two cameras the engine does not have (orbit, asseen) and omits two it does
// (disc, side). camera_control.h states the rule outright: "a second hand-written table is how
// two surfaces end up disagreeing about what 'side' means."
//
// So this file captures the word and validates only its shape. camera_control::ModeFromName
// resolves it, and the per-mode selector rules are enforced where that table lives -- which also
// keeps this translation unit free of any dependency on src/camera.
inline constexpr size_t kMaxCamModeChars = 16;

// ---- The commanded-pose family's bounds (ARCH-17 S3.1) ----------------------------------------
// Each component of `cam move`, in metres. A world coordinate outside the arena is a typo, and
// clamping a typo into a legal-looking teleport is exactly what ParseTimeSpec refuses for `500%`.
inline constexpr double kCamMoveMaxMetres = 10000.0;
// `cam look` pitch, in degrees. REFUSED outside, never clamped: at +/-90 the yaw axis is
// degenerate, so a clamp would silently change which way the camera ends up facing.
inline constexpr double kCamPitchLimitDeg = 89.9;
// Yaw is normalised into (-180, 180] rather than bounded, so `cam look 720 0` means what it says.
// The magnitude limit is only what the normalisation can reduce exactly; nothing a person types
// reaches it, and past it the refusal names the number.
inline constexpr double kCamYawMaxDeg = 1.0e18;
// THE PLAYER SELECTOR. One range, one refusal word, for EVERY arm that takes a player number --
// `cam look at <n>`, `cam pov <n>`, `cam follow <n>`. It is the engine's own 1-based selector;
// tests/control static_asserts the maximum against IReplayCameraControl::kPlayerSelectorMax --
// this file cannot include src/camera without giving up the "touches nothing" contract above, so
// the pin lives where a mismatch is a test failure instead of a silent divergence.
inline constexpr int64_t kCamPlayerSelectorMax = 10;
// And it is 1-BASED, so 0 is refused HERE and never carried. camera_control.h records the engine's
// own packing (read out of SET_CAMERA_MODE_IMPL) and the one thing it says about 0 is that the
// engine's answer for it is bit-identical to its answer for player 10 -- so PackSelector refuses 0,
// and a 0 that got past this parser came back diagnosed `no player occupies that selector`, naming
// the wrong cause for a number that is not a selector at all. `cam look at 0` was fixed with a
// bound private to that arm, and `cam pov 0` went on reaching SetMode with the same ambiguity for
// one more packet: a per-arm copy of this rule is how the second arm gets missed, so there is one.
inline constexpr int64_t kCamPlayerSelectorMin = 1;

// The predicate, and the word every arm refuses by. The word is a VALUE, not a sentence: a reader
// keys on `selector-out-of-range` and never on the prose around it.
inline constexpr const char* kSelectorOutOfRangeWord = "selector-out-of-range";
constexpr bool IsPlayerSelector(int64_t n) {
    return n >= kCamPlayerSelectorMin && n <= kCamPlayerSelectorMax;
}

// Which `cam <mode>` words take a PLAYER number rather than some other index. `cam level <n>` also
// takes a selector, and it is a level index the engine clamps to its own set -- bounding it by the
// roster would refuse a legal camera. This list is the grammar's, not the engine's: ModeFromName
// still owns which words exist, and dispatch still owns which modes accept a selector at all. It
// lives on this side of the wall because dispatch.cpp is not linked into tests/control, so a rule
// stated only there has no case that can fail -- which is exactly how `cam pov 0` survived.
bool IsPlayerSelectorMode(const char* modeWord);

// The one out-of-range sentence, so no arm can invent a second spelling of the same refusal:
//   cam pov: selector-out-of-range, the player selector is 1 to 10, got "0"
// `verbTail` may be null for an arm whose verb is one piece. The range is printed from the
// constants above and never spelled into the text, so it cannot drift from the rule enforced.
void FormatSelectorOutOfRange(char* out, size_t cap, const char* verbHead, const char* verbTail,
                              int64_t got);

// CommandSpec::needs bits.
inline constexpr uint8_t kNeedsNothing   = 0;
inline constexpr uint8_t kNeedsTransport = 1u << 0;
inline constexpr uint8_t kNeedsCamera    = 1u << 1;
// ★ A CAPTURE HAS TO BE OPEN, and this bit exists because its absence was reported as success.
// Transport::Play, Seek, Step and SeekRound all open with `if (!g.loaded) return;`, and the only
// thing the dispatcher tested was whether the transport OBJECT existed -- which it does from module
// init onward. So with nothing loaded, `seek 30` answered "seek to 30000000 us", `play` answered
// "playing at 1x" and `step -5` answered "stepped -5 tick(s) to 0 us", every one of them about a
// call that returned immediately and did nothing. The panel was never vulnerable: it greys the
// whole transport strip on durationUs == 0, which is why the two surfaces disagreed.
inline constexpr uint8_t kNeedsLoaded    = 1u << 2;

const char* CommandName(CommandId);

// 260 is MAX_PATH; the extended-length form reaches 32767. A replay path that long is not
// a case worth 64 KB per queue slot, so this is the practical ceiling and a path over it is REFUSED
// with its length in the message rather than truncated into a path that names a different file.
inline constexpr size_t kMaxPathChars = 520;
// The longest `mark` label the parser accepts, which MUST be the sidecar's own cap. notes.h
// static_asserts the two against each other: the parser used to take 519 characters and the
// sidecar 127, so the refusal the owner saw was a disjunction -- "the sidecar already holds 256
// bookmarks, OR the label is longer than 127 characters" -- where the code knew which.
inline constexpr size_t kMaxMarkLabelChars = 128;
inline constexpr size_t kMaxErrorChars = 192;
// The console's own line buffer and the longest body the HTTP surface will parse.
inline constexpr size_t kMaxLineChars = 1024;

struct Command {
    CommandId id = CommandId::None;

    double  rate = 0.0;             // Speed. Signed: negative is reverse (B.8).
    int64_t whole = 0;              // Seek: microseconds. Step: frames, signed. Round: the number
                                    // the panel labels. Cam: the subject index.
    double  percent = 0.0;          // Seek, when seekIsPercent
    bool    seekIsPercent = false;
    bool    asSeen = false;         // Mode. false = ground truth, matching B.8's SetMode default.
    // Goto: `goto resume` rather than `goto <n>`. A separate flag, not a magic index, because the
    // resume point and bookmark number zero are different things and zero is a real index.
    bool    gotoResume = false;
    // Dress: `dress force`. Off, a slot the ladder already dressed on this load is skipped, so the
    // verb cannot stack a second profile document on a player who is already wearing one.
    bool    dressForce = false;
    // Load: enter from the most recent capture. Set by the ECHOREPLAY_OPEN carrier, never by the
    // parser -- "no argument" and "an empty argument" are different, and only the first means
    // most recent. `load ""` used to reach this behaviour through an empty text field and open a
    // capture the owner had not named.
    bool    loadMostRecent = false;
    // Cam: the mode word, lowercased ASCII. Empty for every other command. `whole` carries the
    // selector and camHasIndex says whether one was typed -- 0 is a real selector value the engine
    // treats specially, so "absent" and "zero" must not share a representation.
    char    camMode[kMaxCamModeChars] = {0};
    bool    camHasIndex = false;

    // Cam pose family. Metres (CamMove) or degrees (CamLook); world space, the same frame the
    // engine stores at viewstate+0x60. Meaningless unless id is CamMove or CamLook.
    float   camVec[3] = {0.0f, 0.0f, 0.0f};
    // false = RELATIVE to the pose the engine currently holds; true = ABSOLUTE world coordinates.
    // Two forms, never one with a magic sentinel: (0,0,0) is a real absolute destination and a
    // real no-op delta, and they are different commands.
    bool    camAbsolute = false;
    // CamLook only: `cam look at <n>` -- aim at the subject `whole` names instead of at an angle.
    bool    camLookAt = false;

    // CamFlight: true is `begin`, false is `end`. Two words, one flag -- there is no third
    // form, and the id already says the command is this one.
    bool    camFlightBegin = false;

    // Seek: the time in `whole` is a DELTA from the playhead, not a position, and it is the one
    // case where `whole` may be negative. `seek +10` / `seek -10`, the same nudge the panel's
    // -10s/+1s buttons already perform. No new CommandId (ARCH-17 S8.1): `seek` is one verb with
    // two operand forms, and a second id would give `help` two rows for one word.
    // A sign and a `%` are refused together -- a relative percentage of a duration is a unit
    // nobody typed.
    bool    seekIsRelative = false;
    // `seek @t`: t is measured from the first RECORDED tick, not from the file's axis origin.
    bool    seekIsSpanRelative = false;
    // Goto: step to the marker nearest after / before the playhead. Flags in gotoResume's shape,
    // never sentinel indices, because 0 and -1 are real bookmark numbers to whoever types them.
    // Both false and gotoResume false means `goto <n>`, in `whole`.
    bool    gotoNext = false;
    bool    gotoPrev = false;

    // Scrub: which of the three phases. A word, for the reason `cam` carries its mode as one --
    // and an enum of three here would be a second spelling of the same three words `help`
    // already prints. The target, when one is typed, arrives in `whole`/`percent` exactly as
    // `seek`'s does, through the same ParseTimeSpec.
    enum class ScrubPhase : uint8_t { Begin, To, End };
    ScrubPhase scrubPhase = ScrubPhase::Begin;
    bool       scrubHasTarget = false;

    // Debug: `debug panel on` / `debug panel off`. One flag and no third form, exactly as
    // camFlightBegin carries `cam flight`'s two words.
    bool       debugOn = false;

    // Bar: `bar on` / `bar off` / `bar toggle`. Three words and no fourth, and `toggle` is its own
    // value rather than a bool plus a flag, because the answer to "on or off?" is not known here.
    enum class BarMode : uint8_t { On, Off, Toggle };
    BarMode    barMode = BarMode::Toggle;
    // RECORDER-2 part D. `record start|stop|status`.
    enum class RecordVerb : uint8_t { Start, Stop, Status };
    RecordVerb recordVerb = RecordVerb::Status;
    // AUDIO-FIX-2. `audio writes on|off|status`. Its own enum rather than a bool plus a flag for
    // the same reason BarMode is: `status` is a third answer, not the absence of the other two.
    enum class AudioWritesVerb : uint8_t { On, Off, Status };
    AudioWritesVerb audioWritesVerb = AudioWritesVerb::Status;

    // Load: the file to open. Mark: the optional bookmark label. One buffer with two documented
    // uses rather than two buffers, because a Command travels through a fixed-size queue slot.
    wchar_t text[kMaxPathChars] = {0};

    // Stamped by dispatch::Submit, never by the parser: the number the finished Result is filed
    // under so the surface that issued the command can collect what actually happened.
    uint64_t seq = 0;
};

struct ParseResult {
    bool    ok = false;
    Command cmd;
    // Always populated, and on failure it names the offending token rather than restating the
    // usage. "speed: expected a number, got \"fast\"" tells the user what happened to THEIR input;
    // "usage: speed <x>" makes them guess which half was wrong.
    char    error[kMaxErrorChars] = {0};
};

// A blank line, or one that is only whitespace, parses OK with id == None and is not an error --
// pressing Enter at a prompt is not a mistake. Every caller checks the id before dispatching.
ParseResult Parse(const wchar_t* line);

// The table, exposed as data so `help` and GET /replay/help render the same text from one source.
struct CommandSpec {
    CommandId   id;
    const char* name;
    const char* usage;
    const char* help;
    // Which services the command cannot run without, as kNeeds* bits. The dispatcher tests this
    // once and produces one honest refusal naming the missing service, instead of fifteen ad-hoc
    // null checks that each phrase it differently. `requires` is a C++20 keyword, hence `needs`.
    uint8_t     needs;
};

size_t             SpecCount();
const CommandSpec& SpecAt(size_t i);
const CommandSpec* SpecFor(CommandId id);

// ---- The pieces worth testing on their own, because each has its own failure modes -------------

// Decimal only, no exponent: nobody types 1e3 for a playback rate, and accepting it would mean
// accepting `inf` and `nan` from the same code path. Rejects both explicitly. `end` receives the
// first character not consumed, so a caller can tell "12abc" from "12".
bool ParseDecimal(const wchar_t* s, double* out, const wchar_t** end);
bool ParseInt64(const wchar_t* s, int64_t* out, const wchar_t** end);

// `seek` accepts `90` (seconds), `1:23.5` ([[h:]m:]s), and `50%`. Seconds is the unit a human
// types; B.8's Seek takes microseconds, so the conversion happens here once rather than in each
// surface. Percent is 0..100 inclusive and anything outside is refused, not clamped: a clamp would
// turn a typed `500%` into a silent jump to the end.
bool ParseTimeSpec(const wchar_t* s, int64_t* outUs, double* outPercent, bool* isPercent);

// ★ WHAT A `step` ACTUALLY DID, decided from the playhead either side of the call rather than from
// the number that was asked for. Transport::StepTicks stops at either end of the tick table and
// returns void, so a dispatcher that echoed the request answered `ok` on a playhead that had not
// moved: L3-RUN-2 F4, "stepped 1 tick(s) to 4022182 us" with 4022182 us -> 4022182 us. A step that
// reaches no new tick is a frame the user cannot get to and must be refused BY NAME. The decision
// lives here, not in dispatch.cpp, because dispatch reaches IReplayTransport and replay::ui and is
// not linked into the tests -- so the fix would otherwise have no case that can fail.
enum class StepOutcome : uint8_t {
    Moved,          // the playhead is on a different tick than the one it was on
    NoTickThatWay,  // the step ran into the end of the tick table and moved nothing
};
// B.8's IReplayTransport::LastStepTicks() answers this when the transport does not count its
// steps. Pinned here rather than included, for the "touches nothing" reason above -- the same
// arrangement kCamPlayerSelectorMax uses: tests/control static_asserts it against
// replay::kStepTicksUncounted, so a divergence is a test failure and not a silent one.
inline constexpr int kStepTicksUncounted = INT_MIN;

// `lastStepTicks` is the transport's own count and settles the question outright when it has one.
// The playhead is the fallback for a transport that does not count, and it is only a fallback: a
// recorded tick table is non-decreasing, so a step across a repeated boundary consumes a real tick
// and leaves the playhead where it was. Inferring from the playhead calls that a refusal.
StepOutcome ClassifyStep(int64_t beforeUs, int64_t afterUs, int lastStepTicks);

// The word that names a refused step on the wire and in a launch row. One spelling, so the console,
// the HTTP surface and the observer cannot each invent their own.
inline constexpr char kStepNoTickThatWay[] = "step-no-tick-that-way";

}  // namespace control
}  // namespace replay
