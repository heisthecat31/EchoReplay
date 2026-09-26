// W2-G. The offline half of the control gate: the command table and its parser, which touch no
// engine memory, no facade and no socket and therefore run on a machine with no game.
//
// WHAT THIS COVERS, stated because a pass count with no scope is a claim and not a result: the
// number parsers, the time-spec grammar, quoting and arity for every command in DESIGN 8.5.2, the
// ECHOREPLAY_OPEN carrier (against the real environment and real files on disk), the command
// queue under concurrent producers, and which services each command declares it cannot run
// without.
//
// The sidecar is in test_notes_io.cpp, ON DISK. It used to be here and it stopped at Serialise and
// Parse -- Save and Load, the two functions that could destroy the owner's bookmarks, were never
// called by anything in this gate.
//
// WHAT IT DOES NOT COVER: registering an HTTP route, AllocConsole, and any dispatch that reaches
// IReplayTransport or replay::ui -- including every kNeedsLoaded refusal, which needs a transport
// and a UI to refuse against. Those need a launch and are parked in
// runs/queue/payload/W2-G.json. This gate being green is not the control surface working.

#include "doctest.h"

#include <windows.h>

#include <locale.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "control/campose.h"
#include "control/cmdqueue.h"
#include "playback/core/transport.h"   // kStepTicksUncounted, pinned against control own copy
#include "camera/camera_control.h"
#include "control/command.h"
#include "control/httpreg.h"
#include "control/launchargs.h"
#include "control/launchgate.h"
#include "control/loadgate.h"
#include "control/notes.h"
#include "control/seekgoto.h"
#include "engine/session.h"
#include "control/urlquery.h"

using namespace replay::control;

namespace {

Command Ok(const wchar_t* line) {
    const ParseResult r = Parse(line);
    REQUIRE_MESSAGE(r.ok, "expected to parse: ", r.error);
    return r.cmd;
}

std::string Err(const wchar_t* line) {
    const ParseResult r = Parse(line);
    REQUIRE_FALSE(r.ok);
    return std::string(r.error);
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------------------------

TEST_CASE("every command id has exactly one spec, and its name parses back to it") {
    // EXACT, not a floor. A new command is a deliberate edit and must be a deliberate edit here
    // too -- a floor would let an enumerator be added with no spec and no test, and SpecFor would
    // return null at the first dispatch instead of at build time.
    // 23 since NAMES-3 added `dress`, which landed without this line being moved with it.
    // 25 since DESIGN-UI-2 S7.2 added `rate` and `scrub`.
    // 26 since UI-2 P2 added `debug`, the runtime switch for the panel the bar demoted.
    // 27 since BAR-FIX added `bar`, the switch that dismisses the bar itself.
    // 28 since RECORDER-2 added `record`, the manual door for capture.auto_record=false.
    // 29 since POLISH-2 added `reverse`, the player bar's direction button as a verb.
    // 30 since MULTI-LOAD added `seekfile`: `seek` moved onto the playable span, and the file axis
    // it used to mean needs a name of its own.
    // 31 since PRESENT-G added `census`, the engine's own actor-component index as a verb.
    // 32 since AUDIO-FIX-2 added `audio`, whose one member `writes` is the cutoff A/B's switch.
    CHECK(SpecCount() == 32);

    // To the LAST enumerator, not to CamFlight: the bound stopped moving when the family after it
    // was appended, so `dress` had a spec nothing here ever asked for.
    for (uint8_t v = static_cast<uint8_t>(CommandId::Help);
         v <= static_cast<uint8_t>(CommandId::Bar); ++v) {
        const CommandId id = static_cast<CommandId>(v);
        const CommandSpec* spec = SpecFor(id);
        REQUIRE_MESSAGE(spec != nullptr, "no spec for command id ", static_cast<int>(v));
        CHECK(spec->id == id);
        REQUIRE(spec->name != nullptr);
        REQUIRE(spec->usage != nullptr);
        REQUIRE(spec->help != nullptr);
        CHECK(spec->name[0] != 0);

        // The name must be lowercase ASCII, or the case-folding compare would be doing nothing.
        // A single space is allowed because ARCH-17's three rows carry a sub-word (`cam move`),
        // and the command-word lookup skips exactly those rows.
        for (const char* p = spec->name; *p != 0; ++p) {
            CHECK((*p == ' ' || (*p >= 'a' && *p <= 'z')));
        }

        // Round trip: feeding a spec's own name back through the parser must reach that command.
        // A command whose word the tokenizer cannot produce is unreachable, and nothing else in
        // this suite would notice.
        std::wstring line(spec->name, spec->name + strlen(spec->name));
        const ParseResult r = Parse(line.c_str());
        CHECK(r.cmd.id == id);
        if (!r.ok) {
            // Failing for want of an argument is fine; failing as unknown means it is unreachable.
            CHECK_FALSE(Contains(std::string(r.error), "unknown command"));
        }
    }
}

TEST_CASE("command names are unique") {
    for (size_t i = 0; i < SpecCount(); ++i) {
        for (size_t j = i + 1; j < SpecCount(); ++j) {
            CHECK(strcmp(SpecAt(i).name, SpecAt(j).name) != 0);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// ParseDecimal / ParseInt64
// ---------------------------------------------------------------------------------------------

TEST_CASE("ParseDecimal accepts the forms a person types and refuses the rest") {
    double v = 0.0;
    const wchar_t* end = nullptr;

    CHECK(ParseDecimal(L"0", &v, &end));       CHECK(v == 0.0);       CHECK(*end == 0);
    CHECK(ParseDecimal(L"12", &v, &end));      CHECK(v == 12.0);
    CHECK(ParseDecimal(L"-3.5", &v, &end));    CHECK(v == doctest::Approx(-3.5));
    CHECK(ParseDecimal(L"+2", &v, &end));      CHECK(v == 2.0);
    CHECK(ParseDecimal(L".5", &v, &end));      CHECK(v == doctest::Approx(0.5));
    CHECK(ParseDecimal(L"5.", &v, &end));      CHECK(v == 5.0);

    // `end` is what lets a caller tell "12" from "12abc"; without it both parse and the second
    // silently becomes 12.
    CHECK(ParseDecimal(L"12abc", &v, &end));   CHECK(v == 12.0);      CHECK(*end == L'a');

    CHECK_FALSE(ParseDecimal(L"", &v, &end));
    CHECK_FALSE(ParseDecimal(L".", &v, &end));
    CHECK_FALSE(ParseDecimal(L"-", &v, &end));
    CHECK_FALSE(ParseDecimal(L"abc", &v, &end));
    // No exponent, deliberately: accepting it would mean accepting inf and nan from the same path.
    CHECK(ParseDecimal(L"1e3", &v, &end));     CHECK(v == 1.0);       CHECK(*end == L'e');
    // Nineteen integer digits is refused rather than accumulated toward infinity.
    CHECK_FALSE(ParseDecimal(L"12345678901234567890", &v, &end));
}

TEST_CASE("ParseDecimal is unaffected by LC_NUMERIC, which is the whole reason it exists") {
    // The control comes first: if the CRT still reads 1.5 correctly under this locale then the
    // locale did not take, and asserting our parser agrees with it would prove nothing at all.
    const char* applied = setlocale(LC_NUMERIC, "de-DE");
    if (applied == nullptr) {
        applied = setlocale(LC_NUMERIC, "German");
    }
    if (applied == nullptr) {
        MESSAGE("no comma-decimal locale on this machine; the discriminating half did not run");
        return;
    }

    const double viaCrt = wcstod(L"1.5", nullptr);
    if (viaCrt == 1.5) {
        MESSAGE("LC_NUMERIC applied but wcstod still reads 1.5; control did not arm");
    } else {
        // This is the failure the parser exists to prevent: `speed 1.5` becoming `speed 1`.
        CHECK(viaCrt == doctest::Approx(1.0));
        double mine = 0.0;
        CHECK(ParseDecimal(L"1.5", &mine, nullptr));
        CHECK(mine == doctest::Approx(1.5));
        CHECK(Ok(L"speed 1.5").rate == doctest::Approx(1.5));
    }
    setlocale(LC_NUMERIC, "C");
}

TEST_CASE("ParseInt64 covers both ends of the range without overflowing a negate") {
    int64_t v = 0;
    const wchar_t* end = nullptr;

    CHECK(ParseInt64(L"0", &v, &end));                     CHECK(v == 0);
    CHECK(ParseInt64(L"-1", &v, &end));                    CHECK(v == -1);
    CHECK(ParseInt64(L"9223372036854775807", &v, &end));   CHECK(v == INT64_MAX);
    CHECK(ParseInt64(L"-9223372036854775808", &v, &end));  CHECK(v == INT64_MIN);

    CHECK_FALSE(ParseInt64(L"9223372036854775808", &v, &end));
    CHECK_FALSE(ParseInt64(L"-9223372036854775809", &v, &end));
    CHECK_FALSE(ParseInt64(L"", &v, &end));
    CHECK_FALSE(ParseInt64(L"-", &v, &end));
    // Not an error: it parses the integer part and `end` reports where it stopped. That is the whole
    // point of `end`, and it is how every caller distinguishes "1" from "1.5" -- the command parser
    // requires end == token end, so `step 1.5` is refused there rather than silently becoming 1.
    CHECK(ParseInt64(L"1.5", &v, &end));
    CHECK(v == 1);
    CHECK(*end == L'.');
}

// ---------------------------------------------------------------------------------------------
// ParseTimeSpec
// ---------------------------------------------------------------------------------------------

TEST_CASE("ParseTimeSpec reads seconds, clock time and percentages") {
    int64_t us = 0;
    double pct = 0.0;
    bool isPct = true;

    CHECK(ParseTimeSpec(L"90", &us, &pct, &isPct));       CHECK(us == 90000000);  CHECK_FALSE(isPct);
    CHECK(ParseTimeSpec(L"0", &us, &pct, &isPct));        CHECK(us == 0);
    CHECK(ParseTimeSpec(L"83.5", &us, &pct, &isPct));     CHECK(us == 83500000);
    CHECK(ParseTimeSpec(L"1:23", &us, &pct, &isPct));     CHECK(us == 83000000);
    CHECK(ParseTimeSpec(L"1:23.5", &us, &pct, &isPct));   CHECK(us == 83500000);
    CHECK(ParseTimeSpec(L"1:02:03", &us, &pct, &isPct));  CHECK(us == 3723000000ll);
    // The leading field is the largest unit and does not wrap: 90:30 is ninety minutes.
    CHECK(ParseTimeSpec(L"90:30", &us, &pct, &isPct));    CHECK(us == 5430000000ll);

    CHECK(ParseTimeSpec(L"50%", &us, &pct, &isPct));
    CHECK(isPct);
    CHECK(pct == doctest::Approx(50.0));
    CHECK(ParseTimeSpec(L"0%", &us, &pct, &isPct));       CHECK(pct == doctest::Approx(0.0));
    CHECK(ParseTimeSpec(L"100%", &us, &pct, &isPct));     CHECK(pct == doctest::Approx(100.0));
}

TEST_CASE("ParseTimeSpec refuses the forms that would silently seek somewhere else") {
    int64_t us = 0;
    double pct = 0.0;
    bool isPct = false;

    CHECK_FALSE(ParseTimeSpec(L"", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"-5", &us, &pct, &isPct));       // no negative absolute time
    CHECK_FALSE(ParseTimeSpec(L"1:90", &us, &pct, &isPct));     // a typo, not 150 seconds
    CHECK_FALSE(ParseTimeSpec(L"1:70:00", &us, &pct, &isPct));  // minutes must be a clock field
    CHECK_FALSE(ParseTimeSpec(L"1:2:3:4", &us, &pct, &isPct));  // three colons is not a time
    CHECK_FALSE(ParseTimeSpec(L"1:", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L":30", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"12s", &us, &pct, &isPct));
    // Refused, not clamped. Clamping would turn a typed 500% into a jump to the end.
    CHECK_FALSE(ParseTimeSpec(L"500%", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"-1%", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"50%x", &us, &pct, &isPct));
}

// ---------------------------------------------------------------------------------------------
// Parse: DESIGN 8.5.2's command set
// ---------------------------------------------------------------------------------------------

TEST_CASE("a blank line is not an error") {
    for (const wchar_t* line : {L"", L"   ", L"\t \r\n"}) {
        const ParseResult r = Parse(line);
        CHECK(r.ok);
        CHECK(r.cmd.id == CommandId::None);
    }
    const ParseResult n = Parse(nullptr);
    CHECK_FALSE(n.ok);
}

TEST_CASE("the no-argument commands parse, and refuse arguments rather than ignoring them") {
    CHECK(Ok(L"play").id == CommandId::Play);
    CHECK(Ok(L"play").rate == doctest::Approx(1.0));
    CHECK(Ok(L"pause").id == CommandId::Pause);
    CHECK(Ok(L"info").id == CommandId::Info);
    CHECK(Ok(L"diag").id == CommandId::Diag);
    CHECK(Ok(L"census").id == CommandId::Census);
    CHECK(Contains(Err(L"census echo"), "takes no further arguments"));
    // A census walks three whole component tables, so it is refused with nothing loaded rather
    // than answering about an empty session.
    CHECK(SpecFor(CommandId::Census)->needs == kNeedsLoaded);
    CHECK(Ok(L"unload").id == CommandId::Unload);
    CHECK(Ok(L"list").id == CommandId::List);
    CHECK(Ok(L"help").id == CommandId::Help);

    // The failing input for the same check: an extra word must be an error. A parser that dropped
    // it would teach the user that `pause now` was understood.
    CHECK(Contains(Err(L"pause now"), "takes no further arguments"));
    CHECK(Contains(Err(L"pause now"), "\"now\""));
    CHECK(Contains(Err(L"play 2"), "takes no further arguments"));
}

TEST_CASE("leading and trailing whitespace and case do not matter to a command word") {
    CHECK(Ok(L"  pause  ").id == CommandId::Pause);
    CHECK(Ok(L"PAUSE").id == CommandId::Pause);
    CHECK(Ok(L"PaUsE").id == CommandId::Pause);
    CHECK(Ok(L"SEEK 1:23").whole == 83000000);
    CHECK(Ok(L"MODE AsSeen").asSeen);
}

TEST_CASE("an unknown command names what was typed and points at help") {
    const std::string e = Err(L"tp 1 2 3");
    CHECK(Contains(e, "unknown command"));
    CHECK(Contains(e, "\"tp\""));
    CHECK(Contains(e, "help"));
}

TEST_CASE("load takes the rest of the line, so an unquoted path with spaces works") {
    CHECK(wcscmp(Ok(L"load match.echodemo").text, L"match.echodemo") == 0);
    CHECK(wcscmp(Ok(L"load C:\\Replays\\a b\\match.echodemo").text,
                 L"C:\\Replays\\a b\\match.echodemo") == 0);
    // Trailing whitespace is not part of a path.
    CHECK(wcscmp(Ok(L"load match.echodemo   ").text, L"match.echodemo") == 0);
    // Quotes are removed, and only quoting can preserve a trailing space.
    CHECK(wcscmp(Ok(L"load \"a b.echodemo\"").text, L"a b.echodemo") == 0);
    CHECK(wcscmp(Ok(L"load \"trailing \"").text, L"trailing ") == 0);
    // A non-ASCII path survives, which is the reason this parser is wide-char native.
    CHECK(wcscmp(Ok(L"load \x00e9t\x00e9.echodemo").text, L"\x00e9t\x00e9.echodemo") == 0);

    CHECK(Contains(Err(L"load"), "needs a file"));
    CHECK(Contains(Err(L"load \"unclosed"), "unterminated quote"));
    CHECK(Contains(Err(L"load \"a\" b"), "whole argument"));

    std::wstring tooLong = L"load ";
    tooLong.append(kMaxPathChars, L'x');
    CHECK(Contains(Err(tooLong.c_str()), "the limit is"));
}

TEST_CASE("speed is signed and bounded") {
    CHECK(Ok(L"speed 0.5").rate == doctest::Approx(0.5));
    CHECK(Ok(L"speed -1").rate == doctest::Approx(-1.0));
    CHECK(Ok(L"speed 0").rate == doctest::Approx(0.0));
    CHECK(Ok(L"speed 100").rate == doctest::Approx(100.0));

    CHECK(Contains(Err(L"speed"), "needs a rate"));
    CHECK(Contains(Err(L"speed fast"), "expected a number"));
    // "12abc" must not become 12: the end-of-token check is what stops it.
    CHECK(Contains(Err(L"speed 12abc"), "expected a number"));
    CHECK(Contains(Err(L"speed 101"), "outside"));
    CHECK(Contains(Err(L"speed -101"), "outside"));
    CHECK(Contains(Err(L"speed 1 2"), "takes no further arguments"));
}

TEST_CASE("seek carries either a time or a percentage, and says which") {
    CHECK(Ok(L"seek 90").whole == 90000000);
    CHECK_FALSE(Ok(L"seek 90").seekIsPercent);
    CHECK(Ok(L"seek 50%").seekIsPercent);
    CHECK(Ok(L"seek 50%").percent == doctest::Approx(50.0));
    CHECK(Ok(L"seek 1:23.5").whole == 83500000);

    CHECK(Contains(Err(L"seek"), "needs a time"));
    CHECK(Contains(Err(L"seek soon"), "expected seconds"));
    CHECK(Contains(Err(L"seek 1:90"), "expected seconds"));
    CHECK(Contains(Err(L"seek 50% 1"), "takes no further arguments"));
}

TEST_CASE("step defaults to one tick, is signed, and refuses a zero step") {
    CHECK(Ok(L"step").whole == 1);
    CHECK(Ok(L"step 5").whole == 5);
    CHECK(Ok(L"step +5").whole == 5);
    CHECK(Ok(L"step -5").whole == -5);
    CHECK(Ok(L"step 2147483647").whole == 2147483647);
    CHECK(Ok(L"step -2147483648").whole == -2147483648ll);

    CHECK(Contains(Err(L"step 0"), "not a step"));
    CHECK(Contains(Err(L"step half"), "whole number"));
    // B.8's Step takes an int; a wider value truncates into a different jump, so it is refused.
    CHECK(Contains(Err(L"step 2147483648"), "out of range"));
    CHECK(Contains(Err(L"step -2147483649"), "out of range"));
}

TEST_CASE("round takes a number and passes it through unchanged") {
    // Unchanged is the point: the panel labels rounds with the manifest's own round index, so the
    // console must speak the same number. A silent +/-1 here would make the two surfaces disagree
    // about which round is which, and it would look like a transport bug.
    CHECK(Ok(L"round 0").whole == 0);
    CHECK(Ok(L"round 2").whole == 2);

    CHECK(Contains(Err(L"round"), "needs a round number"));
    CHECK(Contains(Err(L"round -1"), "out of range"));
    CHECK(Contains(Err(L"round two"), "expected a round number"));
}

TEST_CASE("cam captures the mode word and its optional selector, and validates only their shape") {
    // The VOCABULARY is deliberately not checked here. camera_control.h reads the engine's seven
    // names out of CAM.HTTP_CAMERA_MODE_HANDLER and says why a second hand-written table is a
    // defect: "that is how two surfaces end up disagreeing about what 'side' means." So this parser
    // captures the word and the dispatcher resolves it against the binary.
    //
    // Worth recording because it is a correction: DESIGN 8.5.2 lists `orbit` and `asseen`, which
    // the engine does not have, and omits `disc` and `side`, which it does.
    CHECK(strcmp(Ok(L"cam free").camMode, "free") == 0);
    CHECK_FALSE(Ok(L"cam free").camHasIndex);
    CHECK(strcmp(Ok(L"cam disc").camMode, "disc") == 0);
    CHECK(strcmp(Ok(L"cam side").camMode, "side") == 0);

    const Command pov = Ok(L"cam pov 3");
    CHECK(strcmp(pov.camMode, "pov") == 0);
    CHECK(pov.camHasIndex);
    CHECK(pov.whole == 3);

    // "absent" and "zero" must not share a representation -- but 0 is no longer a selector any
    // arm can carry (see the player-selector case below), so the distinction is pinned with 1.
    const Command one = Ok(L"cam pov 1");
    CHECK(one.camHasIndex);
    CHECK(one.whole == 1);
    CHECK_FALSE(Ok(L"cam pov").camHasIndex);

    // The word is lowercased so the dispatcher's lookup sees one spelling.
    CHECK(strcmp(Ok(L"cam POV 3").camMode, "pov") == 0);
    CHECK(strcmp(Ok(L"CAM Follow 1").camMode, "follow") == 0);

    // A word the engine does not define still PARSES; it is refused by the engine's own table,
    // with the engine's own list in the message.
    CHECK(strcmp(Ok(L"cam orbit").camMode, "orbit") == 0);

    CHECK(Contains(Err(L"cam"), "needs a mode"));
    CHECK(Contains(Err(L"cam pov x"), "expected a selector index"));
    CHECK(Contains(Err(L"cam pov -1"), kSelectorOutOfRangeWord));
    CHECK(Contains(Err(L"cam pov 3 4"), "takes no further arguments"));

    std::wstring longMode = L"cam ";
    longMode.append(kMaxCamModeChars + 2, L'z');
    CHECK(Contains(Err(longMode.c_str()), "is not a mode name"));
}

// ---------------------------------------------------------------------------------------------
// ARCH-17 -- the commanded-pose family: `cam move`, `cam look`, `cam release`.
//
// The parser PLACES the camera and never flies it (RULING FC-2), so everything below is a pure
// function of the line. What is NOT here: whether the pose lands. That is camera_control's and the
// dispatcher's, and no case in this file may be read as evidence for it.
// ---------------------------------------------------------------------------------------------

// The bound the parser enforces for `cam look at <n>` is the engine's own selector maximum.
// command.h cannot include src/camera without giving up the "touches nothing" contract at its
// head, so the two values are pinned HERE, where a divergence stops this gate at build time.
static_assert(kCamPlayerSelectorMax == replay::IReplayCameraControl::kPlayerSelectorMax,
              "every player-selector arm must enforce the engine's own player-selector bound");

TEST_CASE("the pose sub-words are not engine mode names, so the family shadows nothing") {
    // ARCH-17 S3.1 requires this check against the engine's OWN table rather than a second list:
    // if a future build adds a mode called `move`, the parser would shadow it silently and this
    // gate is where that becomes visible.
    replay::CameraMode m{};
    CHECK_FALSE(replay::camera_control::ModeFromName("move", &m));
    CHECK_FALSE(replay::camera_control::ModeFromName("look", &m));
    CHECK_FALSE(replay::camera_control::ModeFromName("release", &m));

    // The control, without which the three above would pass against a ModeFromName that answered
    // false for everything.
    CHECK(replay::camera_control::ModeFromName("free", &m));
    CHECK(replay::camera_control::ModeFromName("api", &m));
}

// W2-G. ONE selector-range rule for every arm that takes a player number. `cam look at 0` was
// fixed at 5433599 with a bound private to that arm; `cam pov 0` and `cam follow 0` went on
// parsing and reaching SetMode, where the engine packs 0 as the word it packs for player 10 --
// the same ambiguity, one arm over. The sweep below is over the ARMS, so an arm that grows its
// own copy of the rule (or a new arm added without one) is a failure here and not a discovery.
TEST_CASE("W2-G: every player-selector arm refuses 0 and max+1 by the same name and range") {
    // Which mode words take a player number, pinned against the engine's own table: every word
    // here must be one ModeFromName defines, or the grammar is bounding a mode that does not
    // exist and leaving the real one unbounded.
    replay::CameraMode m{};
    CHECK(replay::camera_control::ModeFromName("pov", &m));
    CHECK(replay::camera_control::ModeFromName("follow", &m));
    CHECK(IsPlayerSelectorMode("pov"));
    CHECK(IsPlayerSelectorMode("follow"));
    // The discriminating controls, without which a predicate returning true for everything --
    // and one returning false for everything -- would both pass. `level` is the one that matters:
    // it takes a selector too, but it is a LEVEL index the engine clamps to its own set, so
    // bounding it by the roster would refuse a legal camera.
    CHECK_FALSE(IsPlayerSelectorMode("level"));
    CHECK_FALSE(IsPlayerSelectorMode("free"));
    CHECK_FALSE(IsPlayerSelectorMode("disc"));
    CHECK_FALSE(IsPlayerSelectorMode("side"));
    CHECK_FALSE(IsPlayerSelectorMode("orbit"));
    CHECK_FALSE(IsPlayerSelectorMode(""));
    CHECK_FALSE(IsPlayerSelectorMode("po"));
    CHECK_FALSE(IsPlayerSelectorMode("povx"));

    const char* const verbs[] = {"cam look at", "cam pov", "cam follow"};
    for (const char* const verb : verbs) {
        const std::string v(verb);
        CAPTURE(v);
        const std::wstring w(v.begin(), v.end());

        // Both ends of the range are accepted and carry the number they were given. Without
        // these the rule could refuse every selector and the refusals below would still pass.
        CHECK(Ok((w + L" 1").c_str()).whole == kCamPlayerSelectorMin);
        CHECK(Ok((w + L" 10").c_str()).whole == kCamPlayerSelectorMax);

        // 0, max+1 and a negative are refused BY NAME, by the arm that was asked, printing the
        // range and the number. 0 is the one that mattered: it used to reach the engine.
        const int64_t bad[] = {0, kCamPlayerSelectorMax + 1, -1, 99};
        for (const int64_t n : bad) {
            CAPTURE(n);
            const std::string err = Err((w + L" " + std::to_wstring(n)).c_str());
            CAPTURE(err);
            CHECK(Contains(err, kSelectorOutOfRangeWord));
            CHECK(Contains(err, "the player selector is 1 to 10"));
            CHECK(Contains(err, ("got \"" + std::to_string(n) + "\"").c_str()));
            // The refusal names the arm that was asked, so three arms cannot answer with one
            // anonymous sentence.
            CHECK(Contains(err, (v + ":").c_str()));
        }
    }

    // The rule is SCOPED. `cam level` takes a selector the engine clamps itself, and it still
    // parses at values no player selector would accept -- so the sweep above is bounding player
    // arms and not simply everything with a number after it.
    CHECK(Ok(L"cam level 0").whole == 0);
    CHECK(Ok(L"cam level 0").camHasIndex);
    CHECK(Ok(L"cam level 99").whole == 99);

    // And the shape bound is still there underneath it, for the modes the rule does not cover.
    CHECK(Contains(Err(L"cam level -1"), "selector index is out of range"));
}

TEST_CASE("cam move places the camera, relative by default and absolute on `to`") {
    const Command d = Ok(L"cam move 1 0 -2.5");
    CHECK(d.id == CommandId::CamMove);
    CHECK_FALSE(d.camAbsolute);
    CHECK(d.camVec[0] == 1.0f);
    CHECK(d.camVec[1] == 0.0f);
    CHECK(d.camVec[2] == -2.5f);

    const Command a = Ok(L"cam move to 4 2 -1");
    CHECK(a.id == CommandId::CamMove);
    CHECK(a.camAbsolute);
    CHECK(a.camVec[0] == 4.0f);
    CHECK(a.camVec[1] == 2.0f);
    CHECK(a.camVec[2] == -1.0f);

    // (0,0,0) is a real no-op delta AND a real absolute destination. They are different commands,
    // which is the whole reason camAbsolute is a flag and not a sentinel value.
    CHECK_FALSE(Ok(L"cam move 0 0 0").camAbsolute);
    CHECK(Ok(L"cam move to 0 0 0").camAbsolute);
    CHECK(Ok(L"cam move -0 0 0").camVec[0] == 0.0f);

    // The metre bound is inclusive: the last legal coordinate is not refused.
    CHECK(Ok(L"cam move 10000 -10000 0").camVec[0] == 10000.0f);
    CHECK(Ok(L"cam move 10000 -10000 0").camVec[1] == -10000.0f);
    CHECK(Ok(L"cam move to 10000 0 0").camVec[0] == 10000.0f);

    // Case folds for the sub-word and for `to`, as it does for every other command word.
    CHECK(Ok(L"CAM MOVE TO 1 2 3").camAbsolute);
    CHECK(Ok(L"CAM MOVE TO 1 2 3").id == CommandId::CamMove);

    // The mode word is untouched by this arm: `move` never becomes a mode name.
    CHECK(Ok(L"cam move 1 0 0").camMode[0] == 0);
}

TEST_CASE("cam move refuses a malformed pose instead of taking the numbers it did get") {
    CHECK(Contains(Err(L"cam move"), "needs three offsets"));
    CHECK(Contains(Err(L"cam move 1"), "needs three offsets"));
    CHECK(Contains(Err(L"cam move 1 2"), "needs three offsets"));
    CHECK(Contains(Err(L"cam move to"), "needs three world coordinates"));
    CHECK(Contains(Err(L"cam move to 1"), "needs three world coordinates"));
    CHECK(Contains(Err(L"cam move to 1 2"), "needs three world coordinates"));
    CHECK(Contains(Err(L"cam move 1 2 3 4"), "takes no further arguments"));
    CHECK(Contains(Err(L"cam move to 1 2 3 4"), "takes no further arguments"));

    // A non-number names itself, so the operator sees which of the three was wrong.
    CHECK(Contains(Err(L"cam move 1 x 3"), "expected a number"));
    CHECK(Contains(Err(L"cam move 1 x 3"), "\"x\""));
    CHECK(Contains(Err(L"cam move 1 2 3x"), "expected a number"));

    // Out of range is REFUSED with the value printed, never clamped into a legal-looking teleport.
    CHECK(Contains(Err(L"cam move 10000.5 0 0"), "out of range"));
    CHECK(Contains(Err(L"cam move 10000.5 0 0"), "\"10000.5\""));
    CHECK(Contains(Err(L"cam move 0 -20000 0"), "out of range"));
    CHECK(Contains(Err(L"cam move 0 0 20000"), "out of range"));
    CHECK(Contains(Err(L"cam move to 99999 0 0"), "out of range"));

    // ParseDecimal's own refusals reach this grammar, so no exponent, no inf and no nan.
    CHECK(Contains(Err(L"cam move 1e3 0 0"), "expected a number"));
    CHECK(Contains(Err(L"cam move inf 0 0"), "expected a number"));
    CHECK(Contains(Err(L"cam move nan 0 0"), "expected a number"));

    // A twenty-digit coordinate is past ParseDecimal's own 18-digit ceiling, so it is refused
    // there rather than reaching the metre bound -- but it IS refused, and the refusal names the
    // token, which is the property that matters: no long digit string becomes a position.
    CHECK(Contains(Err(L"cam move 1 0 99999999999999999999"), "expected a number"));
    CHECK(Contains(Err(L"cam move 1 0 99999999999999999999"), "\"99999999999999999999\""));
}

TEST_CASE("cam move and cam look are unaffected by LC_NUMERIC, which is why they avoid wcstod") {
    const char* saved = setlocale(LC_NUMERIC, nullptr);
    const std::string keep(saved != nullptr ? saved : "C");
    // Best effort: if the comma-decimal locale is not installed the checks still run, against the
    // C locale, so this case is never vacuous.
    if (setlocale(LC_NUMERIC, "German_Germany.1252") == nullptr) {
        setlocale(LC_NUMERIC, "de_DE.UTF-8");
    }
    CHECK(Ok(L"cam move 1.5 0 0").camVec[0] == 1.5f);
    CHECK(Ok(L"cam look 1.5 2.5").camVec[0] == 1.5f);
    CHECK(Ok(L"cam look 1.5 2.5").camVec[1] == 2.5f);
    setlocale(LC_NUMERIC, keep.c_str());
}

TEST_CASE("cam look takes angles, an absolute aim, or a subject") {
    const Command l = Ok(L"cam look 90 -10");
    CHECK(l.id == CommandId::CamLook);
    CHECK(l.camVec[0] == 90.0f);
    CHECK(l.camVec[1] == -10.0f);
    CHECK(l.camVec[2] == 0.0f);
    CHECK_FALSE(l.camAbsolute);
    CHECK_FALSE(l.camLookAt);

    const Command t = Ok(L"cam look to 90 -10");
    CHECK(t.id == CommandId::CamLook);
    CHECK(t.camAbsolute);
    CHECK(t.camVec[0] == 90.0f);
    CHECK(t.camVec[1] == -10.0f);

    // Yaw normalises into (-180, 180], so a full turn is not a different command from none.
    CHECK(Ok(L"cam look 720 0").camVec[0] == 0.0f);
    CHECK(Ok(L"cam look 270 0").camVec[0] == -90.0f);
    CHECK(Ok(L"cam look -450 0").camVec[0] == -90.0f);
    CHECK(Ok(L"cam look 180 0").camVec[0] == 180.0f);
    CHECK(Ok(L"cam look -180 0").camVec[0] == 180.0f);
    CHECK(Ok(L"cam look -179.5 0").camVec[0] == -179.5f);
    CHECK(Ok(L"cam look to 720 0").camVec[0] == 0.0f);

    // The pitch bound is inclusive at the last angle the yaw axis still resolves.
    CHECK(Ok(L"cam look 0 89.9").camVec[1] == doctest::Approx(89.9));
    CHECK(Ok(L"cam look 0 -89.9").camVec[1] == doctest::Approx(-89.9));

    const Command at = Ok(L"cam look at 3");
    CHECK(at.id == CommandId::CamLook);
    CHECK(at.camLookAt);
    CHECK(at.whole == 3);
    CHECK_FALSE(at.camAbsolute);
    // The selector is 1-BASED here (kCamPlayerSelectorMin). It used to accept 0, and 0 is the one
    // value the engine cannot tell apart from player 10 -- camera_control.h carries the engine's
    // packing and says so -- which is why camera_control::PackSelector refuses it. A 0 that got
    // past the parser came back diagnosed `no player occupies that selector`, naming the wrong
    // cause for a number that is not a selector at all.
    CHECK(Ok(L"cam look at 1").whole == 1);
    CHECK(Ok(L"cam look at 10").whole == 10);
    CHECK(Ok(L"cam look at 1").camLookAt);
    CHECK(Ok(L"cam look at 3").camMode[0] == 0);
    // An angle form never sets the look-at flag, and the look-at form never sets an angle.
    CHECK_FALSE(Ok(L"cam look 90 0").camLookAt);
    CHECK(Ok(L"cam look at 3").camVec[0] == 0.0f);
}

TEST_CASE("cam look refuses the degenerate pitch and an out-of-range subject, naming the number") {
    CHECK(Contains(Err(L"cam look"), "needs a yaw and a pitch"));
    CHECK(Contains(Err(L"cam look 90"), "needs a yaw and a pitch"));
    CHECK(Contains(Err(L"cam look to"), "needs a yaw and a pitch"));
    CHECK(Contains(Err(L"cam look to 90"), "needs a yaw and a pitch"));
    CHECK(Contains(Err(L"cam look 90 0 0"), "takes no further arguments"));
    CHECK(Contains(Err(L"cam look up 0"), "expected a number"));
    CHECK(Contains(Err(L"cam look 0 down"), "expected a number"));

    // +/-90 is the degenerate yaw axis. REFUSED, never clamped -- a clamp would silently change
    // which way the camera ends up facing.
    CHECK(Contains(Err(L"cam look 0 90"), "pitch is out of range"));
    CHECK(Contains(Err(L"cam look 0 90"), "refused not clamped"));
    CHECK(Contains(Err(L"cam look 0 -90"), "pitch is out of range"));
    CHECK(Contains(Err(L"cam look 0 89.95"), "pitch is out of range"));
    CHECK(Contains(Err(L"cam look 0 89.95"), "\"89.95\""));
    CHECK(Contains(Err(L"cam look to 0 91"), "pitch is out of range"));

    CHECK(Contains(Err(L"cam look at"), "needs a subject index"));
    CHECK(Contains(Err(L"cam look at x"), "expected a subject index"));
    // The bound is printed from kCamPlayerSelectorMax, so the sentence cannot drift from the
    // rule. The shared word and the sweep over every arm are the player-selector case below.
    CHECK(Contains(Err(L"cam look at -1"), "the player selector is 1 to 10"));
    CHECK(Contains(Err(L"cam look at 0"), "the player selector is 1 to 10"));
    CHECK(Contains(Err(L"cam look at 0"), "\"0\""));
    CHECK(Contains(Err(L"cam look at 11"), "the player selector is 1 to 10"));
    CHECK(Contains(Err(L"cam look at 11"), "\"11\""));
    CHECK(Contains(Err(L"cam look at 3 4"), "takes no further arguments"));
}


// ---------------------------------------------------------------------------------------------
// ARCH-23 S14.11 -- `cam flight begin|end`, the marker the run scores free flight by (W2-G-32a).
// ---------------------------------------------------------------------------------------------


TEST_CASE("cam flight brackets a stretch of free flight, and only begin or end will do") {
    const Command b = Ok(L"cam flight begin");
    CHECK(b.id == CommandId::CamFlight);
    CHECK(b.camFlightBegin);
    // Not taken as a mode word: `flight` must be claimed by this arm before CopyModeWord sees it,
    // or `cam flight begin` becomes a camera mode nobody has and a selector that will not parse.
    CHECK(b.camMode[0] == 0);

    const Command e = Ok(L"cam flight end");
    CHECK(e.id == CommandId::CamFlight);
    CHECK_FALSE(e.camFlightBegin);
    CHECK(Ok(L"CAM FLIGHT BEGIN").id == CommandId::CamFlight);
    CHECK(Ok(L"CAM FLIGHT BEGIN").camFlightBegin);

    CHECK(Contains(Err(L"cam flight"), "cam flight:"));
    CHECK(Contains(Err(L"cam flight"), "needs begin or end"));
    CHECK(Contains(Err(L"cam flight sideways"), "cam flight:"));
    CHECK(Contains(Err(L"cam flight sideways"), "expected begin or end"));
    CHECK(Contains(Err(L"cam flight sideways"), "sideways"));
    CHECK(Contains(Err(L"cam flight begin now"), "cam flight:"));
    CHECK(Contains(Err(L"cam flight begin now"), "takes no further arguments"));
    CHECK(Contains(Err(L"flight begin"), "unknown command"));

    const CommandSpec* spec = SpecFor(CommandId::CamFlight);
    REQUIRE(spec != nullptr);
    CHECK(strcmp(spec->name, "cam flight") == 0);
    // The rig, and only the rig: nothing drains the mark without the camera module, and neither
    // the transport nor a loaded capture has anything to do with the owner flying.
    CHECK((spec->needs & kNeedsCamera) != 0);
    CHECK((spec->needs & (kNeedsTransport | kNeedsLoaded)) == 0);
}

TEST_CASE("a flight mark is queued for the camera module, one per invocation, oldest first") {
    // The mark is QUEUED, not logged here: camera_module drains it inside its next census window
    // and stamps it on the clock the pose samples carry. Draining first, because another case in
    // this binary may have left one behind and a queue read that started dirty proves nothing.
    bool mark = false;
    while (replay::camera_control::TakeFlightMark(&mark)) {
    }
    CHECK_FALSE(replay::camera_control::TakeFlightMark(&mark));

    replay::camera_control::MarkFlightWindow(true);
    replay::camera_control::MarkFlightWindow(false);
    REQUIRE(replay::camera_control::TakeFlightMark(&mark));
    CHECK(mark);
    REQUIRE(replay::camera_control::TakeFlightMark(&mark));
    CHECK_FALSE(mark);
    CHECK_FALSE(replay::camera_control::TakeFlightMark(&mark));

    // A second begin is a second mark, not a swallowed one -- that is what `re-stamps` means on
    // the wire, and the scorer takes the last begin before the end.
    replay::camera_control::MarkFlightWindow(true);
    replay::camera_control::MarkFlightWindow(true);
    REQUIRE(replay::camera_control::TakeFlightMark(&mark));
    CHECK(mark);
    REQUIRE(replay::camera_control::TakeFlightMark(&mark));
    CHECK(mark);
    CHECK_FALSE(replay::camera_control::TakeFlightMark(&mark));

}

TEST_CASE("a second `cam flight begin` re-stamps and says so, and every outcome names itself") {
    char text[256];
    bool open = false;

    FlightMarkOutcome o = ClassifyFlightMark(true, open);
    CHECK(o == FlightMarkOutcome::Opened);
    open = FlightWindowAfter(o);
    CHECK(open);
    FormatFlightMark(o, text, sizeof text);
    CHECK(Contains(std::string(text), "cam flight begin:"));

    o = ClassifyFlightMark(true, open);
    CHECK(o == FlightMarkOutcome::Reopened);
    open = FlightWindowAfter(o);
    CHECK(open);
    FormatFlightMark(o, text, sizeof text);
    CHECK(Contains(std::string(text), "RE-STAMPED"));

    o = ClassifyFlightMark(false, open);
    CHECK(o == FlightMarkOutcome::Closed);
    open = FlightWindowAfter(o);
    CHECK_FALSE(open);
    FormatFlightMark(o, text, sizeof text);
    CHECK(Contains(std::string(text), "cam flight end:"));

    o = ClassifyFlightMark(false, open);
    CHECK(o == FlightMarkOutcome::ClosedUnopened);
    FormatFlightMark(o, text, sizeof text);
    CHECK(Contains(std::string(text), "brackets nothing"));

    // Every outcome says which of the four it was, and no two say the same thing.
    std::vector<std::string> said;
    for (FlightMarkOutcome each : {FlightMarkOutcome::Opened, FlightMarkOutcome::Reopened,
                                   FlightMarkOutcome::Closed,
                                   FlightMarkOutcome::ClosedUnopened}) {
        FormatFlightMark(each, text, sizeof text);
        CHECK_FALSE(Contains(std::string(text), "unhandled"));
        said.push_back(text);
    }
    std::sort(said.begin(), said.end());
    CHECK(std::unique(said.begin(), said.end()) == said.end());
}

TEST_CASE("cam release takes nothing, and a trailing word is refused rather than ignored") {
    const Command r = Ok(L"cam release");
    CHECK(r.id == CommandId::CamRelease);
    CHECK_FALSE(r.camAbsolute);
    CHECK_FALSE(r.camLookAt);
    CHECK(r.camMode[0] == 0);
    CHECK(Ok(L"CAM RELEASE").id == CommandId::CamRelease);
    CHECK(Contains(Err(L"cam release now"), "cam release:"));
    CHECK(Contains(Err(L"cam release now"), "takes no further arguments"));
}

TEST_CASE("the pose family is additive: the eighteen commands before it are untouched") {
    // The ids keep their numbers, which is what lets a queued Command survive an in-place upgrade.
    CHECK(static_cast<uint8_t>(CommandId::Unload) == 18);
    CHECK(static_cast<uint8_t>(CommandId::CamMove) == 19);
    CHECK(static_cast<uint8_t>(CommandId::CamLook) == 20);
    CHECK(static_cast<uint8_t>(CommandId::CamRelease) == 21);
    CHECK(static_cast<uint8_t>(CommandId::CamFlight) == 22);
    CHECK(static_cast<uint8_t>(CommandId::Cam) == 10);
    CHECK(static_cast<uint8_t>(CommandId::Help) == 1);

    // `cam <mode> [N]` still parses to Cam, with the mode word and the selector where they were.
    CHECK(Ok(L"cam free").id == CommandId::Cam);
    CHECK(strcmp(Ok(L"cam free").camMode, "free") == 0);
    CHECK(Ok(L"cam pov 3").id == CommandId::Cam);
    CHECK(Ok(L"cam pov 3").whole == 3);
    CHECK(Ok(L"cam pov 3").camHasIndex);
    CHECK(strcmp(Ok(L"cam orbit").camMode, "orbit") == 0);

    // A quoted `"cam move"` is not a second spelling of the command: a spec name with a space is
    // skipped by the command-word lookup, so the family has exactly one surface syntax.
    CHECK(Contains(Err(L"\"cam move\" 1 0 0"), "unknown command"));

    // Every command that is not this family keeps its parse, including the ones whose words are
    // near-misses for the sub-words.
    CHECK(Ok(L"play").id == CommandId::Play);
    CHECK(Ok(L"mode asseen").id == CommandId::Mode);
    CHECK(Contains(Err(L"move 1 0 0"), "unknown command"));
    CHECK(Contains(Err(L"release"), "unknown command"));
}

TEST_CASE("the pose family declares a camera and nothing else") {
    const CommandId kFamily[] = {CommandId::CamMove, CommandId::CamLook, CommandId::CamRelease};
    for (CommandId id : kFamily) {
        const CommandSpec* s = SpecFor(id);
        REQUIRE(s != nullptr);
        // ARCH-17 S3.1: NOT kNeedsTransport and NOT kNeedsLoaded. Moving the camera around an idle
        // spectator view is legitimate, and the transport is not this family's business.
        CHECK(s->needs == kNeedsCamera);
        CHECK(s->usage != nullptr);
        CHECK(s->help != nullptr);
        // help renders from this table, so a row with no text is a command with no help.
        CHECK(s->usage[0] != 0);
        CHECK(s->help[0] != 0);
    }
}

TEST_CASE("mode selects ground truth or as-seen, and ground is the default value") {
    CHECK_FALSE(Ok(L"mode ground").asSeen);
    CHECK(Ok(L"mode asseen").asSeen);
    CHECK(Contains(Err(L"mode"), "needs ground or asseen"));
    CHECK(Contains(Err(L"mode both"), "expected ground or asseen"));
}

TEST_CASE("mark takes an optional label") {
    CHECK(Ok(L"mark").id == CommandId::Mark);
    CHECK(Ok(L"mark").text[0] == 0);
    CHECK(wcscmp(Ok(L"mark great save").text, L"great save") == 0);
    CHECK(wcscmp(Ok(L"mark \"great save\"").text, L"great save") == 0);
    CHECK(Contains(Err(L"mark \"a\" b"), "whole argument"));
}

TEST_CASE("the command needs-bits name the service each command cannot run without") {
    CHECK((SpecFor(CommandId::Play)->needs & kNeedsTransport) != 0);
    CHECK((SpecFor(CommandId::Cam)->needs & kNeedsCamera) != 0);
    // load goes through replay::ui, which reports its own reason when no transport has registered,
    // so it must NOT carry the bit -- two refusals for one cause is how the surfaces disagree.
    CHECK(SpecFor(CommandId::Load)->needs == kNeedsNothing);
    CHECK(SpecFor(CommandId::Help)->needs == kNeedsNothing);
    CHECK(SpecFor(CommandId::Diag)->needs == kNeedsNothing);
}

// ---------------------------------------------------------------------------------------------
// ECHOREPLAY_OPEN -- the carrier that replaced -replayviewer
//
// These run against the REAL environment block and REAL files, not against a string handed to a
// parser. That is the point: the defect being prevented is a value the process never actually
// reads, and a test that passes its own value in cannot see that.
// ---------------------------------------------------------------------------------------------

namespace {

// Sets kOpenEnvVar for the length of a scope and puts it back afterwards, so one case cannot decide
// another's answer.
class ScopedOpenEnv {
public:
    explicit ScopedOpenEnv(const wchar_t* value) {
        ::SetEnvironmentVariableW(kOpenEnvVar, value);
    }
    ~ScopedOpenEnv() { ::SetEnvironmentVariableW(kOpenEnvVar, nullptr); }
    ScopedOpenEnv(const ScopedOpenEnv&) = delete;
    ScopedOpenEnv& operator=(const ScopedOpenEnv&) = delete;
};

// A real file on disk with a real extension, because "exists and is readable" is asked of the
// filesystem and cannot be faked.
class TempCapture {
public:
    explicit TempCapture(const wchar_t* suffix) {
        wchar_t dir[MAX_PATH];
        const DWORD n = ::GetTempPathW(MAX_PATH, dir);
        REQUIRE(n != 0);
        path_.assign(dir, n);
        path_ += L"w2g_open_";
        path_ += std::to_wstring(::GetCurrentProcessId());
        path_ += L"_";
        path_ += std::to_wstring(++counter_);
        path_ += suffix;
        const HANDLE h = ::CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        ::WriteFile(h, "ECHODEMO", 8, &written, nullptr);
        ::CloseHandle(h);
    }
    ~TempCapture() { ::DeleteFileW(path_.c_str()); }
    const wchar_t* Path() const { return path_.c_str(); }

private:
    std::wstring path_;
    static int counter_;
};
int TempCapture::counter_ = 0;

}  // namespace

TEST_CASE("the launch path comes from the environment, and it is the path that was set") {
    // ★ THE ARM. Make ReadOpenRequest stop calling GetEnvironmentVariableW -- return false, or read
    // some other name -- and this case fails: there is no other way for the path to arrive.
    TempCapture cap(L".echodemo");
    ScopedOpenEnv env(cap.Path());

    OpenRequest req;
    REQUIRE(ReadOpenRequest(&req));
    CHECK(req.present);
    CHECK(req.hasPath);
    CHECK_FALSE(req.mostRecent);
    CHECK(req.refusal == OpenRefusal::None);
    CHECK(wcscmp(req.path, cap.Path()) == 0);
}

TEST_CASE("an unset carrier is not a request and not an error") {
    ::SetEnvironmentVariableW(kOpenEnvVar, nullptr);
    OpenRequest req;
    CHECK_FALSE(ReadOpenRequest(&req));
    CHECK_FALSE(req.present);
    CHECK_FALSE(req.hasPath);
    CHECK(req.refusal == OpenRefusal::None);

    // An empty value is indistinguishable from an unset one at the Win32 API -- both answer 0 -- so
    // it is read as absent, and the "most recent" request is carried by an explicit token instead.
    ScopedOpenEnv env(L"");
    CHECK_FALSE(ReadOpenRequest(&req));
    CHECK_FALSE(req.present);
}

TEST_CASE("a carrier naming a file that is not there is refused, and the refusal says which file") {
    std::wstring gone;
    {
        TempCapture cap(L".echodemo");
        gone = cap.Path();
    }   // deleted here, so the path is well-formed and names nothing

    ScopedOpenEnv env(gone.c_str());
    OpenRequest req;
    REQUIRE(ReadOpenRequest(&req));
    CHECK(req.present);            // the owner DID ask for something
    CHECK_FALSE(req.hasPath);      // and it cannot be served
    CHECK(req.refusal == OpenRefusal::Missing);
    CHECK(strcmp(OpenRefusalText(req.refusal), "no such file") == 0);
    // The refused path is kept: a refusal that cannot name what it refused is not diagnosable.
    CHECK(wcscmp(req.path, gone.c_str()) == 0);
}

TEST_CASE("every other way the value can be wrong is a named refusal, never a guess") {
    OpenRequest req;

    {   // relative, so it would resolve against whatever directory the game was started in
        ScopedOpenEnv env(L"match.echodemo");
        REQUIRE(ReadOpenRequest(&req));
        CHECK(req.refusal == OpenRefusal::NotAbsolute);
        CHECK_FALSE(req.hasPath);
    }
    {   // drive-relative is not absolute either
        ScopedOpenEnv env(L"C:match.echodemo");
        REQUIRE(ReadOpenRequest(&req));
        CHECK(req.refusal == OpenRefusal::NotAbsolute);
    }
    {   // a real, readable file that is not a capture
        TempCapture cap(L".txt");
        ScopedOpenEnv env(cap.Path());
        REQUIRE(ReadOpenRequest(&req));
        CHECK(req.refusal == OpenRefusal::WrongExtension);
        CHECK_FALSE(req.hasPath);
    }
    // FORMATS-P6: the legacy replays the viewer converts on open are accepted, in any case.
    for (const wchar_t* legacyExt : {L".echoreplay", L".TAPE"}) {
        TempCapture cap(legacyExt);
        ScopedOpenEnv env(cap.Path());
        REQUIRE(ReadOpenRequest(&req));
        CHECK(req.refusal == OpenRefusal::None);
        CHECK(req.hasPath);
    }
    {   // a directory whose name ends in the extension
        wchar_t dir[MAX_PATH];
        const DWORD n = ::GetTempPathW(MAX_PATH, dir);
        REQUIRE(n != 0);
        std::wstring d(dir, n);
        d += L"w2g_open_dir.echodemo";
        const bool dirReady =
            ::CreateDirectoryW(d.c_str(), nullptr) != 0 || ::GetLastError() == ERROR_ALREADY_EXISTS;
        REQUIRE(dirReady);
        {
            ScopedOpenEnv env(d.c_str());
            REQUIRE(ReadOpenRequest(&req));
            CHECK(req.refusal == OpenRefusal::NotAFile);
            CHECK_FALSE(req.hasPath);
        }
        ::RemoveDirectoryW(d.c_str());
    }
    {   // longer than this build can hold: refused, never truncated into a different file's name
        std::wstring longPath = L"C:\\";
        longPath.append(kMaxPathChars + 4, L'x');
        longPath += kCaptureExtension;
        ScopedOpenEnv env(longPath.c_str());
        REQUIRE(ReadOpenRequest(&req));
        CHECK(req.refusal == OpenRefusal::TooLong);
        CHECK_FALSE(req.hasPath);
    }
}

TEST_CASE("the browser sentinel is the one value that is not a path") {
    ScopedOpenEnv env(kOpenMostRecent);
    OpenRequest req;
    REQUIRE(ReadOpenRequest(&req));
    CHECK(req.present);
    CHECK(req.mostRecent);
    CHECK_FALSE(req.hasPath);
    CHECK(req.refusal == OpenRefusal::None);
}

TEST_CASE("a quoted or padded value names the same file as a bare one") {
    // A launcher that interpolates a path into a batch file leaves both behind, and neither
    // character can occur in a Windows file name.
    TempCapture cap(L".echodemo");
    std::wstring quoted = L"  \"";
    quoted += cap.Path();
    quoted += L"\"  ";
    ScopedOpenEnv env(quoted.c_str());
    OpenRequest req;
    REQUIRE(ReadOpenRequest(&req));
    CHECK(req.hasPath);
    CHECK(wcscmp(req.path, cap.Path()) == 0);
}

TEST_CASE("the old switch on the command line is still recognised -- for the warning, and nothing else") {
    // The engine has already thrown the entire line away by the time this is asked (F2:
    // ApplyCommandLine jumps to its epilogue on the first unregistered token), so -offline and all
    // four host pins are gone with it. The only thing left to do is say so.
    CHECK(CommandLineMentionsReplayViewer(L"echovr.exe -offline -replayviewer x.echodemo"));
    CHECK(CommandLineMentionsReplayViewer(L"echovr.exe -replayviewer"));
    CHECK(CommandLineMentionsReplayViewer(L"echovr.exe -replayviewer=x.echodemo"));
    CHECK(CommandLineMentionsReplayViewer(L"echovr.exe -ReplayViewer x"));
    // Under every prefix the engine's own tokeniser honours, or a launcher could pass one that
    // voids the line and get no warning for it.
    CHECK(CommandLineMentionsReplayViewer(L"g.exe +replayviewer x"));
    CHECK(CommandLineMentionsReplayViewer(L"g.exe \x0096replayviewer x"));

    CHECK_FALSE(CommandLineMentionsReplayViewer(nullptr));
    CHECK_FALSE(CommandLineMentionsReplayViewer(L""));
    CHECK_FALSE(CommandLineMentionsReplayViewer(L"echovr.exe -offline -level mpl_arena_a"));
    CHECK_FALSE(CommandLineMentionsReplayViewer(L"g.exe -replayviewerx foo"));
    CHECK_FALSE(CommandLineMentionsReplayViewer(L"g.exe -notreplayviewer foo"));
    // A value that merely contains the word is not a switch: this must not warn about a launch that
    // is in fact clean.
    CHECK_FALSE(CommandLineMentionsReplayViewer(L"g.exe -logpath C:\\-replayviewer\\log.txt"));

    // The wording is pinned because it is the only evidence a mistaken launch leaves, and because
    // the audit reference in it is what tells the reader why the launch went online.
    CHECK(strstr(kLegacySwitchWarning, "-replayviewer") != nullptr);
    CHECK(strstr(kLegacySwitchWarning, "engine ignored the command line") != nullptr);
    CHECK(strstr(kLegacySwitchWarning, "AUDIT_W1-F_R4 F2") != nullptr);
}

// ---------------------------------------------------------------------------------------------
// L3-RUN-2 F4 -- `step +1` answered ok ("stepped 1 tick(s) to 4022182 us") with the playhead on
// 4022182 us before and after. StepTicks stops at either end of the tick table and returns void, so
// the only honest source for the answer is the playhead either side of the call.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a step that reaches no new tick is not a step") {
    // One spelling of "this transport does not count", shared with B.8 rather than copied from it.
    static_assert(replay::control::kStepTicksUncounted == replay::kStepTicksUncounted,
                  "control's pin drifted from IReplayTransport::LastStepTicks()");

    // The recorded values, both directions, from the launch that filed F4, through a transport that
    // does not count -- the playhead is all there is, and it is enough for these five.
    const int nc = replay::control::kStepTicksUncounted;
    CHECK(ClassifyStep(4022182, 4022182, nc) == StepOutcome::NoTickThatWay);
    CHECK(ClassifyStep(4022182, 4011071, nc) == StepOutcome::Moved);
    CHECK(ClassifyStep(4011071, 4022182, nc) == StepOutcome::Moved);
    // 0 us is a playhead, not an absence: a step off the first tick and back onto it is the one
    // case where "did not move" and "start of file" coincide, and both are still refusals.
    CHECK(ClassifyStep(0, 0, nc) == StepOutcome::NoTickThatWay);
    CHECK(ClassifyStep(0, 11111, nc) == StepOutcome::Moved);

    // The same two playheads, now with a count behind them. 0 is a refusal whatever the playhead
    // did; a non-zero count is a step whatever the playhead did.
    CHECK(ClassifyStep(4022182, 4022182, 0) == StepOutcome::NoTickThatWay);
    CHECK(ClassifyStep(4022182, 4011071, -1) == StepOutcome::Moved);
    // A partial step is still a step: `step 5` that reached two ticks moved.
    CHECK(ClassifyStep(4011071, 4033293, 2) == StepOutcome::Moved);

    // ★ THE BLIND SPOT IN THE PLAYHEAD, AND THE WHOLE REASON FOR THE COUNT. A recorded tick table is
    // NON-decreasing: a stalled source records the same boundary twice, and StepTicks discards the
    // accumulator, so a step that consumed a real recorded tick can land on the microsecond it
    // started from. Inferred from the playhead this reads as `step-no-tick-that-way` and the user is
    // refused a frame that exists. RED against the inference, green against the count.
    CHECK(ClassifyStep(22222, 22222, 1) == StepOutcome::Moved);
    CHECK(ClassifyStep(22222, 22222, -1) == StepOutcome::Moved);
}

// ---------------------------------------------------------------------------------------------
// The queue
// ---------------------------------------------------------------------------------------------

TEST_CASE("the queue is first in, first out, and reports its own overflow") {
    CommandQueue q;
    Command c;
    CHECK_FALSE(q.Pop(&c));

    for (int i = 0; i < 8; ++i) {
        Command in;
        in.id = CommandId::Step;
        in.whole = i;
        CHECK(q.Push(in));
    }
    for (int i = 0; i < 8; ++i) {
        REQUIRE(q.Pop(&c));
        CHECK(c.whole == i);
    }
    CHECK_FALSE(q.Pop(&c));
    // Nothing was refused: a queue drained as fast as it is filled never reaches its cap.
    CHECK(q.Refused() == 0);
}

TEST_CASE("a full queue refuses the new command and counts it, never silently drops it") {
    CommandQueue q;
    Command in;
    in.id = CommandId::Pause;
    size_t accepted = 0;
    while (q.Push(in)) {
        ++accepted;
        REQUIRE(accepted <= CommandQueue::kCapacity + 1);
    }
    CHECK(accepted == CommandQueue::kCapacity);
    CHECK(q.Refused() == 1);

    // And it recovers: draining one slot makes room for exactly one more.
    Command out;
    REQUIRE(q.Pop(&out));
    CHECK(q.Push(in));
    CHECK_FALSE(q.Push(in));
    CHECK(q.Refused() == 2);
}

TEST_CASE("concurrent producers lose nothing and duplicate nothing") {
    // The console thread and the HTTP handler are different threads, so this is the real shape.
    // A queue that lost a command under contention would look like "the console sometimes ignores
    // me", which is the hardest kind of defect to report.
    constexpr int kProducers = 4;
    constexpr int kEach = 500;
    CommandQueue q;
    std::atomic<int> pushed{0};
    std::vector<int> seen(kProducers * kEach, 0);
    std::atomic<bool> go{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kEach; ++i) {
                Command c;
                c.id = CommandId::Step;
                c.whole = p * kEach + i;
                while (!q.Push(c)) {
                    std::this_thread::yield();
                }
                pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    int drained = 0;
    go.store(true, std::memory_order_release);
    while (drained < kProducers * kEach) {
        Command c;
        if (q.Pop(&c)) {
            REQUIRE(c.whole >= 0);
            REQUIRE(c.whole < kProducers * kEach);
            ++seen[static_cast<size_t>(c.whole)];
            ++drained;
        } else {
            std::this_thread::yield();
        }
    }
    for (auto& t : producers) {
        t.join();
    }

    // Refusals are EXPECTED here and are not losses: these producers retry, so a refusal is
    // backpressure from a full queue. The invariant that matters is below -- every command was
    // pushed exactly once, drained exactly once, and none was duplicated or lost. (Submit does not
    // retry, so in the product a refusal really is a command that did not run, and the user is told
    // so on the spot.)
    CHECK(pushed.load() == kProducers * kEach);
    CHECK(drained == kProducers * kEach);
    for (size_t i = 0; i < seen.size(); ++i) {
        CAPTURE(i);
        REQUIRE(seen[i] == 1);
    }
}

// The notes sidecar's own cases live in test_notes_io.cpp, in memory AND on disk. They were here,
// and every one of them stopped at Serialise/Parse -- so Save and Load, the two functions that can
// destroy the owner's bookmarks, were never called by anything in this gate.


// ---------------------------------------------------------------------------------------------
// The unterminated-quote guard, which used to be applied at three call sites out of nine
// ---------------------------------------------------------------------------------------------

TEST_CASE("the same typo is refused by every verb, not by three of them") {
    // Measured before the fix: `load "a` was refused while `speed "1.5` was ACCEPTED at rate 1.5,
    // and `seek "50%`, `cam "pov` and `cam pov "3` were all accepted. The values produced happened
    // to be right, which is exactly why it survived a review.
    const wchar_t* const lines[] = {
        L"load \"a",     L"mark \"x",   L"speed \"1.5", L"seek \"50%", L"seek \"1:30",
        L"step \"1",     L"round \"2",  L"cam \"pov",   L"cam pov \"3", L"mode \"ground",
        L"marks \"1",    L"goto \"1",   L"console \"close",
    };
    for (const wchar_t* line : lines) {
        CAPTURE(line);
        const std::string err = Err(line);
        CHECK(Contains(err, "unterminated quote"));
    }

    // And the guard does not refuse a line that closes its quotes.
    CHECK(Ok(L"speed \"1.5\"").rate == doctest::Approx(1.5));
    CHECK(Ok(L"cam \"pov\" 3").camHasIndex);
}

// ---------------------------------------------------------------------------------------------
// `load ""` -- an empty argument is not "no argument"
// ---------------------------------------------------------------------------------------------

TEST_CASE("an empty or blank file name is refused rather than treated as `most recent`") {
    // Measured before the fix: `load ""` parsed ok with an empty text, and RunLoad read that as a
    // bare `load` and called ui::LoadMostRecent() -- the owner named a file, got a different
    // capture, and was told it worked.
    const std::string empty = Err(L"load \"\"");
    CHECK(Contains(empty, "empty name"));
    const std::string blank = Err(L"load \"  \"");
    CHECK(Contains(blank, "only spaces"));

    // A real path still loads, including one with spaces in it.
    const Command cmd = Ok(L"load \"C:\\replays\\my match.echodemo\"");
    CHECK(wcscmp(cmd.text, L"C:\\replays\\my match.echodemo") == 0);
    CHECK_FALSE(cmd.loadMostRecent);

    // Nothing the parser produces asks for the most recent capture. Only the launch switch does,
    // and it says so with a flag rather than with an empty string.
    CHECK_FALSE(Ok(L"list").loadMostRecent);
}

// ---------------------------------------------------------------------------------------------
// Signs and leading zeros
// ---------------------------------------------------------------------------------------------

TEST_CASE("a negative time is refused in every form ParseTimeSpec can be written") {
    // `lead[i] < 0` could not fire for a field that had already parsed to 0, so `seek -5` was
    // refused and `seek -0:30` gave +30 s. ParseTimeSpec still refuses a leading minus for every
    // form -- and that is now where this property is pinned, because ARCH-17 S8.1 gives `seek`'s
    // own arm ONE leading sign as the relative-nudge operand. The magnitude below it is still a
    // time with no sign in it, which is what keeps `--5` and `-0:30`-means-plus-30 refused.
    int64_t us = 0;
    double pct = 0.0;
    bool isPct = false;
    CHECK_FALSE(ParseTimeSpec(L"-5", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"-0:30", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"-0:0:30", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"-1:30", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"-0", &us, &pct, &isPct));
    CHECK_FALSE(ParseTimeSpec(L"-50%", &us, &pct, &isPct));

    // At the command, one sign is a nudge backwards -- and the original defect is still caught:
    // `-0:30` is minus thirty seconds, never plus thirty.
    CHECK(Ok(L"seek -0:30").whole == -30000000);
    CHECK(Ok(L"seek -0:0:30").whole == -30000000);
    CHECK(Ok(L"seek -5").whole == -5000000);
    CHECK(Ok(L"seek -0").seekIsRelative);
    CHECK(Ok(L"seek -0").whole == 0);
    // A second sign has no arm to strip it and reaches the function above.
    CHECK_FALSE(Parse(L"seek --5").ok);
    CHECK_FALSE(Parse(L"seek -50%").ok);

    // The positive forms are untouched.
    CHECK(Ok(L"seek 0:30").whole == 30000000);
    CHECK(Ok(L"seek 0:0:30").whole == 30000000);
    CHECK(Ok(L"seek 1:30").whole == 90000000);
}

TEST_CASE("a zero-padded number is a number") {
    // ParseDecimal counted keystrokes rather than magnitude, so a value from a script or a paste
    // with leading zeros was reported as "expected a number".
    CHECK(Ok(L"speed 000000000000000000001").rate == doctest::Approx(1.0));
    CHECK(Ok(L"speed 0000000000000000000000000.5").rate == doctest::Approx(0.5));
    CHECK(Ok(L"speed 01.5").rate == doctest::Approx(1.5));

    // The limit it exists to enforce still holds: nineteen SIGNIFICANT digits is still too many.
    CHECK_FALSE(Parse(L"speed 1234567890123456789").ok);
}

// ---------------------------------------------------------------------------------------------
// The label cap, which used to be two numbers
// ---------------------------------------------------------------------------------------------

TEST_CASE("a mark label is measured against the sidecar's own cap, and the refusal says which") {
    std::wstring line(L"mark ");
    line.append(kMaxMarkLabelChars - 1, L'x');
    const Command ok = Ok(line.c_str());
    CHECK(wcslen(ok.text) == kMaxMarkLabelChars - 1);

    std::wstring tooLong(L"mark ");
    tooLong.append(kMaxMarkLabelChars, L'x');
    const std::string err = Err(tooLong.c_str());
    // One reason, with both numbers in it. The old refusal came from the sidecar and offered the
    // reader a choice between the bookmark cap and the label cap.
    CHECK(Contains(err, "the label is"));
    CHECK(Contains(err, "127"));
}

// ---------------------------------------------------------------------------------------------
// The commands that make a bookmark reachable
// ---------------------------------------------------------------------------------------------

TEST_CASE("marks and goto parse the forms the help text advertises") {
    CHECK(Ok(L"marks").whole == 1);          // page 1 by default
    CHECK(Ok(L"marks 3").whole == 3);
    CHECK_FALSE(Parse(L"marks 0").ok);
    CHECK_FALSE(Parse(L"marks -1").ok);
    CHECK_FALSE(Parse(L"marks two").ok);

    const Command byNumber = Ok(L"goto 4");
    CHECK(byNumber.whole == 4);
    CHECK_FALSE(byNumber.gotoResume);

    const Command resume = Ok(L"goto resume");
    CHECK(resume.gotoResume);
    // `goto 0` and `goto resume` must not share a representation: bookmarks are numbered from 1
    // and zero is not a spelling of "where I left off".
    CHECK_FALSE(Parse(L"goto 0").ok);
    CHECK_FALSE(Parse(L"goto").ok);
    CHECK_FALSE(Parse(L"goto later").ok);
}

TEST_CASE("console close is spelled out, and nothing else is accepted") {
    CHECK(Parse(L"console close").ok);
    CHECK_FALSE(Parse(L"console").ok);
    CHECK_FALSE(Parse(L"console open").ok);
    CHECK_FALSE(Parse(L"console close now").ok);
    CHECK(Contains(Err(L"console"), "console close"));
}

// ---------------------------------------------------------------------------------------------
// What each command cannot run without
// ---------------------------------------------------------------------------------------------

TEST_CASE("every command that drives the playhead declares that it needs a capture open") {
    // The dispatcher tested only whether the transport OBJECT existed, which it does from module
    // init onward -- so with nothing loaded, `seek 30` answered "seek to 30000000 us" over a
    // Seek() that returned at its first line. The bit is what MissingFor now tests.
    const CommandId needLoaded[] = {CommandId::Play,  CommandId::Pause, CommandId::Speed,
                                    CommandId::Seek,  CommandId::Step,  CommandId::Round,
                                    CommandId::Mark,  CommandId::Marks, CommandId::Goto,
                                    CommandId::Unload};
    for (CommandId id : needLoaded) {
        const CommandSpec* spec = SpecFor(id);
        REQUIRE(spec != nullptr);
        CAPTURE(spec->name);
        CHECK((spec->needs & kNeedsLoaded) != 0);
    }

    // And the ones that must work with nothing open do not carry it, or the owner could never
    // reach a capture in the first place.
    const CommandId needNothingLoaded[] = {CommandId::Help, CommandId::List, CommandId::Load,
                                           CommandId::Info, CommandId::Diag, CommandId::Console};
    for (CommandId id : needNothingLoaded) {
        const CommandSpec* spec = SpecFor(id);
        REQUIRE(spec != nullptr);
        CAPTURE(spec->name);
        CHECK((spec->needs & kNeedsLoaded) == 0);
    }
}

// ---------------------------------------------------------------------------------------------
// The HTTP surface's query decoder
// ---------------------------------------------------------------------------------------------

TEST_CASE("PercentDecode handles the forms a client library emits") {
    char out[64];
    REQUIRE(PercentDecode("hello", 5, out, sizeof out));
    CHECK(strcmp(out, "hello") == 0);
    REQUIRE(PercentDecode("seek+1%3A23", 11, out, sizeof out));
    CHECK(strcmp(out, "seek 1:23") == 0);
    REQUIRE(PercentDecode("load%20a%20b.echodemo", 21, out, sizeof out));
    CHECK(strcmp(out, "load a b.echodemo") == 0);
    // Lowercase hex is as legal as uppercase, and clients emit both.
    REQUIRE(PercentDecode("%2f%2F", 6, out, sizeof out));
    CHECK(strcmp(out, "//") == 0);
    REQUIRE(PercentDecode("", 0, out, sizeof out));
    CHECK(out[0] == 0);
}

TEST_CASE("PercentDecode refuses rather than decoding half a command") {
    char out[64];
    // A truncated escape is not a literal percent sign: taking it as one would turn `100%` at the
    // end of a line into a different value than the sender wrote.
    CHECK_FALSE(PercentDecode("abc%", 4, out, sizeof out));
    CHECK_FALSE(PercentDecode("abc%2", 5, out, sizeof out));
    CHECK_FALSE(PercentDecode("abc%zz", 6, out, sizeof out));
    CHECK_FALSE(PercentDecode("abc%2z", 6, out, sizeof out));

    // And a value that does not fit is refused whole. `load a.echodemo` truncated to `load a`
    // opens a different file, or nothing, and says neither.
    char tiny[8];
    CHECK_FALSE(PercentDecode("load abcdefghij", 15, tiny, sizeof tiny));
}

TEST_CASE("QueryValue finds its key, and only its key") {
    char out[64];
    REQUIRE(QueryValue("/replay/command?line=pause", "line", out, sizeof out));
    CHECK(strcmp(out, "pause") == 0);
    REQUIRE(QueryValue("/replay/command?a=1&line=play&b=2", "line", out, sizeof out));
    CHECK(strcmp(out, "play") == 0);
    REQUIRE(QueryValue("/replay/command?line=", "line", out, sizeof out));
    CHECK(out[0] == 0);

    // Whole-name matching. A prefix match would make `linewidth=3` answer a request for `line`.
    CHECK_FALSE(QueryValue("/replay/command?linewidth=3", "line", out, sizeof out));
    CHECK_FALSE(QueryValue("/replay/command?myline=3", "line", out, sizeof out));
    CHECK_FALSE(QueryValue("/replay/command", "line", out, sizeof out));
    CHECK_FALSE(QueryValue("/replay/command?other=1", "line", out, sizeof out));
    CHECK_FALSE(QueryValue(nullptr, "line", out, sizeof out));

    // A key whose value will not decode is a refusal, not an empty string.
    CHECK_FALSE(QueryValue("/replay/command?line=%zz", "line", out, sizeof out));
}

// ---------------------------------------------------------------------------------------------
// What is deliberately NOT tested here, and why
// ---------------------------------------------------------------------------------------------
//
// ★★ THE CONSOLE CLOSE. A control_closeprobe target used to spawn a child that allocated a real
// console window, so that what Windows does to a process when its console closes could be measured
// rather than assumed. The owner has ruled the behaviour is not a defect -- he knows the console
// belongs to the game process -- so the requirement is gone and so is the probe. The measurements
// survive in docs/spikes/W2-G.md. Nothing in this directory opens a window: the probe ran on the
// owner's own desktop, and a mutation loop re-running this binary painted about a hundred and fifty
// console windows over whatever he was doing.

// =================================================================================================
// The load gate. `Execute` is not linked here (it reaches IReplayTransport and replay::ui), so the
// decision it makes lives in control/loadgate.h and every cell of it is swept from this side.
// =================================================================================================

TEST_CASE("a capture is open only when the panel and the transport both say so") {
    using replay::control::CaptureIsOpen;
    const replay::TransportState kEvery[] = {replay::TransportState::Pause,  replay::TransportState::Play,
                                     replay::TransportState::Scrub,  replay::TransportState::Seek,
                                     replay::TransportState::Step,   replay::TransportState::NotLoaded};

    for (replay::TransportState st : kEvery) {
        const bool holdsFile = st != replay::TransportState::NotLoaded;
        CHECK(CaptureIsOpen(true, true, st) == holdsFile);
        // No path in the panel, and no transport at all: neither can be talked into a yes.
        CHECK_FALSE(CaptureIsOpen(false, true, st));
        CHECK_FALSE(CaptureIsOpen(true, false, st));
        CHECK_FALSE(CaptureIsOpen(false, false, st));
    }

    // The cell this exists for. Before W2-B there was no word for "no file" and the transport
    // answered Pause, so the panel's path alone decided -- and a fileless transport was driven.
    CHECK_FALSE(CaptureIsOpen(true, true, replay::TransportState::NotLoaded));
    CHECK(CaptureIsOpen(true, true, replay::TransportState::Pause));
}

TEST_CASE("every transport state has its own word, and NotLoaded does not borrow one") {
    using replay::control::TransportRunWord;
    CHECK(strcmp(TransportRunWord(replay::TransportState::NotLoaded), "nothing loaded") == 0);
    CHECK(strcmp(TransportRunWord(replay::TransportState::Pause), "paused") == 0);
    CHECK(strcmp(TransportRunWord(replay::TransportState::Play), "playing") == 0);
    CHECK(strcmp(TransportRunWord(replay::TransportState::Scrub), "scrubbing") == 0);
    CHECK(strcmp(TransportRunWord(replay::TransportState::Seek), "seeking") == 0);
    CHECK(strcmp(TransportRunWord(replay::TransportState::Step), "stepping") == 0);

    // The two-valued form said "playing" for four of the six. Every word is distinct, so no state
    // can be reported as another one.
    const replay::TransportState kEvery[] = {replay::TransportState::Pause,  replay::TransportState::Play,
                                     replay::TransportState::Scrub,  replay::TransportState::Seek,
                                     replay::TransportState::Step,   replay::TransportState::NotLoaded};
    for (int i = 0; i < 6; ++i) {
        for (int j = i + 1; j < 6; ++j) {
            CHECK(strcmp(TransportRunWord(kEvery[i]), TransportRunWord(kEvery[j])) != 0);
        }
    }
}

// ==============================================================================================
// L3 F1 -- the ECHOREPLAY_OPEN launch gate, driven by the NetGame sequence L3 actually recorded.
//
// The load was submitted at NetGame state 1 (loading root), the entry tore the half-built session
// down, and the engine sat in -94 (login failed) for the rest of the launch. So the test the gate
// has to pass is not "does it eventually submit" -- it is "over the exact sequence of states L3
// saw, does it submit ZERO times".
// ==============================================================================================
namespace {

// The module's own bookkeeping, so a sequence exercises the retry rules and not just one call.
struct GateSim {
    uint32_t attempts = 0;
    uint32_t submits = 0;
    uint32_t gaveUp = 0;
    bool     refusalPending = false;
    int32_t  refusedAtState = 0;
    bool     done = false;
    std::string lastWhy;

    // `refuseIt` models W1-F's precondition turning the submitted load down on the same tick.
    LaunchStep Tick(bool transport, bool sessionPresent, int32_t state, bool outOfTime,
                    bool retryDue, bool refuseIt) {
        if (done) {
            return LaunchStep::GiveUp;
        }
        LaunchGateInput in;
        in.transportPresent = transport;
        in.sessionPresent = sessionPresent;
        in.netGameState = state;
        in.attempts = attempts;
        in.maxAttempts = 3;
        in.refusalPending = refusalPending;
        in.refusedAtState = refusedAtState;
        in.retryDue = retryDue;
        in.outOfTime = outOfTime;
        const char* why = nullptr;
        const LaunchStep step = DecideLaunchStep(in, &why);
        REQUIRE(why != nullptr);
        lastWhy = why;
        if (step == LaunchStep::Submit) {
            ++submits;
            ++attempts;
            refusalPending = refuseIt;
            refusedAtState = state;
            // An ACCEPTED load ends the service: the module latches LaunchState::Done and never
            // asks the gate again. Modelling that is what makes "submitted once" mean once.
            if (!refuseIt) {
                done = true;
            }
        } else if (step == LaunchStep::GiveUp) {
            ++gaveUp;
            done = true;
        }
        return step;
    }
};

// echoreplay.log, L3, in order. State 1 is where the load actually went out.
const int32_t kL3States[] = {0, 1, 1, 1, 2, -94, -94, -94, -94, -94};
// A healthy launch: the sequence L2 and L5 both walked, ending in a level.
const int32_t kHealthyStates[] = {0, 1, 2, 3, 5, 7, 8, 9, 9, 9};

}  // namespace

TEST_CASE("launch gate: the recorded L3 sequence submits no load at all") {
    GateSim sim;
    for (int32_t st : kL3States) {
        sim.Tick(true, true, st, false, true, false);
    }
    CHECK(sim.submits == 0);
    CHECK(sim.gaveUp == 0);
    // The reason names the engine's state, not the transport, so the log says which.
    CHECK(sim.lastWhy.find("NetGame") != std::string::npos);

    // The one line that fails against the pre-fix code: state 1 with a transport was a submit.
    const char* why = nullptr;
    LaunchGateInput at1;
    at1.transportPresent = true;
    at1.sessionPresent = true;
    at1.netGameState = 1;
    at1.maxAttempts = 3;
    CHECK(DecideLaunchStep(at1, &why) == LaunchStep::Wait);
}

TEST_CASE("launch gate: every state below level load waits, 7/8/9 submit") {
    for (int32_t st = -100; st < 7; ++st) {
        LaunchGateInput in;
        in.transportPresent = true;
        in.sessionPresent = true;
        in.netGameState = st;
        in.maxAttempts = 3;
        const char* why = nullptr;
        CAPTURE(st);
        CHECK(DecideLaunchStep(in, &why) == LaunchStep::Wait);
    }
    const int32_t kReady[] = {7, 8, 9};
    for (int32_t st : kReady) {
        LaunchGateInput in;
        in.transportPresent = true;
        in.sessionPresent = true;
        in.netGameState = st;
        in.maxAttempts = 3;
        const char* why = nullptr;
        CAPTURE(st);
        CHECK(DecideLaunchStep(in, &why) == LaunchStep::Submit);
    }
    CHECK(kLoadReadyState == 7);
    CHECK(static_cast<int32_t>(replay::NetGameState::LoadingLevel) == kLoadReadyState);
}

TEST_CASE("launch gate: a healthy sequence submits exactly once, at state 7") {
    GateSim sim;
    bool    sawSubmit = false;
    int32_t submittedAt = 0;
    for (int32_t st : kHealthyStates) {
        if (sim.Tick(true, true, st, false, true, false) == LaunchStep::Submit) {
            if (!sawSubmit) {
                sawSubmit = true;
                submittedAt = st;
            }
        }
    }
    CHECK(sim.submits == 1);
    CHECK(sawSubmit);
    CHECK(submittedAt == 7);
    CHECK(sim.gaveUp == 0);
}

TEST_CASE("launch gate: a refusal is retried on the next state change, not on the clock") {
    GateSim sim;
    // Submitted at 7 and refused there.
    CHECK(sim.Tick(true, true, 7, false, true, true) == LaunchStep::Submit);
    CHECK(sim.refusalPending);

    // The clock says retry; the engine has not moved. Asking again asks the same question.
    for (int i = 0; i < 5; ++i) {
        CHECK(sim.Tick(true, true, 7, false, true, false) == LaunchStep::Wait);
    }
    CHECK(sim.submits == 1);
    CHECK(sim.lastWhy.find("has not changed state") != std::string::npos);

    // The engine moves. That, and only that, re-offers the request.
    CHECK(sim.Tick(true, true, 8, false, false, false) == LaunchStep::Submit);
    CHECK(sim.submits == 2);
}

TEST_CASE("launch gate: bounded -- a state that keeps refusing ends at the browser, with a reason") {
    GateSim sim;
    int32_t st = 7;
    for (int i = 0; i < 40 && !sim.done; ++i) {
        sim.Tick(true, true, st, false, true, true);
        ++st;  // a new state every tick, so the refusal retry is never what stops it
    }
    CHECK(sim.submits == 3);          // maxAttempts, and not one more
    CHECK(sim.gaveUp == 1);
    CHECK(sim.lastWhy.find("refused on every attempt") != std::string::npos);
}

TEST_CASE("launch gate: the request is never dropped -- out of time always names its blocker") {
    struct Case { bool transport; bool session; int32_t state; const char* word; };
    const Case kCases[] = {
        {false, false, 0, "transport"},
        {true, false, 9, "session service"},
        {true, true, 1, "NetGame"},
        {true, true, 9, "wait limit"},
    };
    for (const Case& c : kCases) {
        LaunchGateInput in;
        in.transportPresent = c.transport;
        in.sessionPresent = c.session;
        in.netGameState = c.state;
        in.maxAttempts = 3;
        in.outOfTime = true;
        const char* why = nullptr;
        CAPTURE(c.word);
        CHECK(DecideLaunchStep(in, &why) == LaunchStep::GiveUp);
        REQUIRE(why != nullptr);
        CHECK(std::string(why).find(c.word) != std::string::npos);
    }
    // The same four inputs while there is still time are waits, not give-ups: the bound is the
    // clock's and nothing else's.
    for (const Case& c : kCases) {
        LaunchGateInput in;
        in.transportPresent = c.transport;
        in.sessionPresent = c.session;
        in.netGameState = c.state;
        in.maxAttempts = 3;
        const char* why = nullptr;
        CAPTURE(c.word);
        CHECK(DecideLaunchStep(in, &why) != LaunchStep::GiveUp);
    }
}

TEST_CASE("launch gate: an unregistered session service is not state zero") {
    // "the engine is at state 0" and "nobody can tell us the state" must not share an answer.
    LaunchGateInput noSession;
    noSession.transportPresent = true;
    noSession.sessionPresent = false;
    noSession.netGameState = 9;
    noSession.maxAttempts = 3;
    const char* whyA = nullptr;
    CHECK(DecideLaunchStep(noSession, &whyA) == LaunchStep::Wait);

    LaunchGateInput atZero = noSession;
    atZero.sessionPresent = true;
    atZero.netGameState = 0;
    const char* whyB = nullptr;
    CHECK(DecideLaunchStep(atZero, &whyB) == LaunchStep::Wait);
    REQUIRE(whyA != nullptr);
    REQUIRE(whyB != nullptr);
    CHECK(strcmp(whyA, whyB) != 0);
}

// ---------------------------------------------------------------------------------------------
// P17-B: relative seek, and `goto next` / `goto prev`
// ---------------------------------------------------------------------------------------------

namespace {
// Ten seconds of microseconds is a value in the fact registry (CLOCK.CTOR_MAXELAPSED_INIT), and a
// fixture that spells it as a literal trips the gatecheck ratchet. Here it is a unit, not a fact,
// so it is built from one.
constexpr int64_t kSecUs = 1000000;
}  // namespace

TEST_CASE("seek carries a sign as a relative nudge, and the magnitude still goes through "
          "ParseTimeSpec") {
    // The control: no sign, no flag. If this ever goes red, the flag is being set by something
    // other than the sign and every absolute seek has become a nudge.
    const Command abs = Ok(L"seek 10");
    CHECK_FALSE(abs.seekIsRelative);
    CHECK(abs.whole == 10 * kSecUs);

    const Command fwd = Ok(L"seek +10");
    CHECK(fwd.seekIsRelative);
    CHECK_FALSE(fwd.seekIsPercent);
    CHECK(fwd.whole == 10 * kSecUs);

    const Command back = Ok(L"seek -10");
    CHECK(back.seekIsRelative);
    CHECK(back.whole == -10 * kSecUs);

    // The two signs must not produce the same command. A dropped negation is the mutant this line
    // exists for, and it is invisible to a test that only checks the flag.
    CHECK(fwd.whole == -back.whole);
    CHECK(fwd.whole != back.whole);

    // Every form the absolute grammar takes, signed.
    CHECK(Ok(L"seek -1:30").whole == -90000000);
    CHECK(Ok(L"seek +1:00:00").whole == 3600000000LL);
    CHECK(Ok(L"seek -0.5").whole == -500000);

    // Case: the sign is a prefix of the token, not a separate word.
    CHECK(Contains(Err(L"seek +"), "after the sign"));
    CHECK(Contains(Err(L"seek + 30"), "after the sign"));
    // A second sign reaches ParseTimeSpec, which refuses a leading minus for every form.
    CHECK(Contains(Err(L"seek --30"), "--30"));
    CHECK(Contains(Err(L"seek +-30"), "+-30"));
    // Still one operand.
    CHECK_FALSE(Parse(L"seek +10 20").ok);
    // The magnitude is still bounded by kMaxSeconds, sign or no sign.
    CHECK_FALSE(Parse(L"seek +1000000001").ok);
}

TEST_CASE("a relative percentage is refused, naming the token, and absolute percent still works") {
    const std::string plus = Err(L"seek +50%");
    CHECK(Contains(plus, "+50%"));
    CHECK(Contains(plus, "percentage"));
    CHECK(Contains(Err(L"seek -50%"), "-50%"));

    const Command pct = Ok(L"seek 50%");
    CHECK(pct.seekIsPercent);
    CHECK_FALSE(pct.seekIsRelative);
    CHECK(pct.percent == 50.0);
    // And the refusal is about the SIGN, not about percentages: 500% still fails for its own
    // reason and 50% still passes, so this arm cannot be the thing that broke either.
    CHECK_FALSE(Parse(L"seek 500%").ok);
}

TEST_CASE("goto next / prev are flags in gotoResume's shape, never sentinel indices") {
    const Command next = Ok(L"goto next");
    CHECK(next.gotoNext);
    CHECK_FALSE(next.gotoPrev);
    CHECK_FALSE(next.gotoResume);
    CHECK(next.whole == 0);

    const Command prev = Ok(L"goto prev");
    CHECK(prev.gotoPrev);
    CHECK_FALSE(prev.gotoNext);
    CHECK_FALSE(prev.gotoResume);

    // The three words are three different commands, and `goto <n>` is none of them.
    const Command resume = Ok(L"goto resume");
    CHECK(resume.gotoResume);
    CHECK_FALSE(resume.gotoNext);
    CHECK_FALSE(resume.gotoPrev);

    const Command byNumber = Ok(L"goto 3");
    CHECK(byNumber.whole == 3);
    CHECK_FALSE(byNumber.gotoNext);
    CHECK_FALSE(byNumber.gotoPrev);
    CHECK_FALSE(byNumber.gotoResume);

    // Case-folded like `resume`, because the parser folds every keyword.
    CHECK(Ok(L"goto NEXT").gotoNext);
    CHECK(Ok(L"goto Prev").gotoPrev);

    // A near miss is refused and the refusal lists what it could have been.
    const std::string bad = Err(L"goto nxt");
    CHECK(Contains(bad, "nxt"));
    CHECK(Contains(bad, "next"));
    CHECK(Contains(bad, "prev"));
    CHECK_FALSE(Parse(L"goto next 2").ok);
}

TEST_CASE("the help table advertises both new operand forms, from the one table help renders") {
    const CommandSpec* seek = SpecFor(CommandId::Seek);
    REQUIRE(seek != nullptr);
    CHECK(Contains(std::string(seek->usage), "+/-"));
    const CommandSpec* go = SpecFor(CommandId::Goto);
    REQUIRE(go != nullptr);
    CHECK(Contains(std::string(go->usage), "next"));
    CHECK(Contains(std::string(go->usage), "prev"));
    // No new id: `seek` and `goto` are one verb each with two more operand forms, so `help` must
    // still print exactly one row per word. SpecCount()'s exact pin above is the other half.
    size_t seekRows = 0;
    size_t gotoRows = 0;
    for (size_t i = 0; i < SpecCount(); ++i) {
        if (strcmp(SpecAt(i).name, "seek") == 0) {
            ++seekRows;
        }
        if (strcmp(SpecAt(i).name, "goto") == 0) {
            ++gotoRows;
        }
    }
    CHECK(seekRows == 1);
    CHECK(gotoRows == 1);
}

TEST_CASE("ResolveRelativeSeekUs clamps to the file and says which of the three happened") {
    const int64_t duration = 120000000;  // 120 s
    int64_t land = -7;

    CHECK(ResolveRelativeSeekUs(30000000, 10 * kSecUs, duration, &land) == RelativeSeek::Landed);
    CHECK(land == 40000000);
    CHECK(ResolveRelativeSeekUs(30000000, -10 * kSecUs, duration, &land) == RelativeSeek::Landed);
    CHECK(land == 20000000);

    // The panel's own behaviour, and the reason S8.1 rules a clamp right here: -10s at t=3s means
    // the start, not an error.
    CHECK(ResolveRelativeSeekUs(3000000, -10 * kSecUs, duration, &land) == RelativeSeek::Clamped);
    CHECK(land == 0);
    // The other end, which is the mutant target: drop the upper clamp and this line goes red while
    // the lower one stays green.
    CHECK(ResolveRelativeSeekUs(115000000, 10 * kSecUs, duration, &land) == RelativeSeek::Clamped);
    CHECK(land == duration);

    // Both boundaries are INSIDE. Landing exactly on the end of the file is not a clamp, and
    // calling it one would make the reply say "clamped" for a seek that went exactly where asked.
    CHECK(ResolveRelativeSeekUs(110000000, 10 * kSecUs, duration, &land) == RelativeSeek::Landed);
    CHECK(land == duration);
    CHECK(ResolveRelativeSeekUs(10 * kSecUs, -10 * kSecUs, duration, &land) == RelativeSeek::Landed);
    CHECK(land == 0);

    // The sign has to survive: +d and -d from the same playhead cannot land in the same place.
    int64_t up = 0;
    int64_t down = 0;
    ResolveRelativeSeekUs(60000000, 5000000, duration, &up);
    ResolveRelativeSeekUs(60000000, -5000000, duration, &down);
    CHECK(up == 65000000);
    CHECK(down == 55000000);
    CHECK(up != down);
}

TEST_CASE("a duration of zero is a refusal, not a clamp to the start") {
    int64_t land = -7;
    CHECK(ResolveRelativeSeekUs(0, 10 * kSecUs, 0, &land) == RelativeSeek::NoRange);
    // Untouched: a caller that prints the landing after ignoring the refusal prints its own
    // sentinel, not a plausible-looking 0 us that never came from this function.
    CHECK(land == -7);
    CHECK(ResolveRelativeSeekUs(5000000, -1000000, -1, &land) == RelativeSeek::NoRange);
    CHECK(land == -7);
    // One microsecond of range is a range.
    CHECK(ResolveRelativeSeekUs(0, 10 * kSecUs, 1, &land) == RelativeSeek::Clamped);
    CHECK(land == 1);

    // The three words are three words.
    CHECK(strcmp(RelativeSeekName(RelativeSeek::Landed),
                 RelativeSeekName(RelativeSeek::Clamped)) != 0);
    CHECK(strcmp(RelativeSeekName(RelativeSeek::Clamped),
                 RelativeSeekName(RelativeSeek::NoRange)) != 0);
}

TEST_CASE("the relative sum saturates instead of wrapping into the wrong end of the file") {
    const int64_t duration = 120000000;
    int64_t land = -7;
    // A wrapped sum would come out negative and clamp to 0 -- a `seek +10` that lands at the START
    // of the file, which is the one wrong answer that still looks like a legal one.
    CHECK(ResolveRelativeSeekUs(INT64_MAX - 5, 1000000, duration, &land) == RelativeSeek::Clamped);
    CHECK(land == duration);
    CHECK(ResolveRelativeSeekUs(INT64_MIN + 5, -1000000, duration, &land) == RelativeSeek::Clamped);
    CHECK(land == 0);
    CHECK(SaturatingAddUs(INT64_MAX, 1) == INT64_MAX);
    CHECK(SaturatingAddUs(INT64_MIN, -1) == INT64_MIN);
    CHECK(SaturatingAddUs(-5, 5) == 0);
}

TEST_CASE("StepMarkerUs steps strictly, does not wrap, and keeps its two refusals apart") {
    // Deliberately NOT in time order -- notes.cpp appends at count_ in the order `mark` was typed
    // and never sorts. A neighbour-by-index step would answer 90 for `next` from 35 us.
    const int64_t marks[] = {50000000, 10 * kSecUs, 90000000, 30000000, 70000000};
    const size_t n = sizeof marks / sizeof marks[0];
    int64_t land = -7;

    REQUIRE(StepMarkerUs(marks, n, 35000000, true, &land) == MarkerStep::Found);
    CHECK(land == 50000000);
    REQUIRE(StepMarkerUs(marks, n, 35000000, false, &land) == MarkerStep::Found);
    CHECK(land == 30000000);

    // Strictly. Standing exactly on a marker, `next` goes to the one after it, so two `goto next`
    // in a row advance twice instead of sticking.
    REQUIRE(StepMarkerUs(marks, n, 30000000, true, &land) == MarkerStep::Found);
    CHECK(land == 50000000);
    REQUIRE(StepMarkerUs(marks, n, land, true, &land) == MarkerStep::Found);
    CHECK(land == 70000000);
    REQUIRE(StepMarkerUs(marks, n, 30000000, false, &land) == MarkerStep::Found);
    CHECK(land == 10 * kSecUs);

    // No wrap, at either end.
    CHECK(StepMarkerUs(marks, n, 90000000, true, &land) == MarkerStep::AtEdge);
    CHECK(StepMarkerUs(marks, n, 91000000, true, &land) == MarkerStep::AtEdge);
    CHECK(StepMarkerUs(marks, n, 10 * kSecUs, false, &land) == MarkerStep::AtEdge);
    CHECK(StepMarkerUs(marks, n, 0, false, &land) == MarkerStep::AtEdge);
    // AtEdge left the landing alone, so the caller cannot print a stale one as a success.
    CHECK(land == 10 * kSecUs);

    // "no bookmarks" and "you are at the last one" are answered by different things -- `mark`
    // versus moving the playhead -- so they must not share a word.
    CHECK(StepMarkerUs(marks, 0, 35000000, true, &land) == MarkerStep::NoMarkers);
    CHECK(StepMarkerUs(nullptr, n, 35000000, true, &land) == MarkerStep::NoMarkers);
    CHECK(strcmp(MarkerStepName(MarkerStep::NoMarkers), MarkerStepName(MarkerStep::AtEdge)) != 0);
    CHECK(strcmp(MarkerStepName(MarkerStep::Found), MarkerStepName(MarkerStep::AtEdge)) != 0);

    // Duplicates are one destination, not two: stepping off a doubled marker skips both copies.
    const int64_t dupes[] = {40000000, 20000000, 40000000, 60000000};
    REQUIRE(StepMarkerUs(dupes, 4, 40000000, true, &land) == MarkerStep::Found);
    CHECK(land == 60000000);
    REQUIRE(StepMarkerUs(dupes, 4, 40000000, false, &land) == MarkerStep::Found);
    CHECK(land == 20000000);
}

TEST_CASE("StepMarkerUs agrees with a brute-force oracle over a full unordered sidecar") {
    // A whole sidecar's worth, in an order no `mark` session would ever produce, swept from every
    // position between and on the markers. The oracle is written the other way round -- a full
    // min/max scan with its own comparisons -- so a shared mistake is not shared code.
    const size_t n = notes::kMaxBookmarks;
    std::vector<int64_t> marks(n);
    for (size_t i = 0; i < n; ++i) {
        marks[i] = static_cast<int64_t>(((i * 97) % n) + 1) * 1000000;
    }

    size_t found = 0;
    size_t edges = 0;
    for (int64_t now = 0; now <= static_cast<int64_t>(n + 1) * 1000000; now += 500000) {
        for (int pass = 0; pass < 2; ++pass) {
            const bool forward = pass == 0;
            bool haveWant = false;
            int64_t want = 0;
            for (size_t i = 0; i < n; ++i) {
                const int64_t t = marks[i];
                if (forward ? (t > now) : (t < now)) {
                    if (!haveWant || (forward ? (t < want) : (t > want))) {
                        want = t;
                        haveWant = true;
                    }
                }
            }
            int64_t got = -7;
            const MarkerStep step = StepMarkerUs(marks.data(), n, now, forward, &got);
            if (haveWant) {
                REQUIRE(step == MarkerStep::Found);
                CHECK(got == want);
                ++found;
            } else {
                REQUIRE(step == MarkerStep::AtEdge);
                ++edges;
            }
        }
    }
    // The sweep reached both answers -- a sweep that only ever saw one of them would pass with
    // half the function deleted.
    CHECK(found > 0);
    CHECK(edges > 0);
}

// ---------------------------------------------------------------------------------------------
// W2-G P17-E. The commanded-pose family: `cam move`, `cam look` and `cam release`.
//
// dispatch.cpp is on the echoreplay target only, so the arms themselves cannot be linked here.
// What CAN be linked is every decision they make (control/campose.h) and the shipped
// IReplayCameraControl (camera_control.cpp is compiled into this target), so the cases below drive
// the arm's own sequence against a fake control that records what it was handed, and a source scan
// pins the wiring the fake cannot see.
// ---------------------------------------------------------------------------------------------

namespace {

// Records what it was asked, and answers whatever the case needs it to. Nothing here computes a
// pose: a fixture that did the work would be the thing under test.
class RecordingCam : public replay::IReplayCameraControl {
public:
    int                   commandPoseCalls = 0;
    int                   releasePoseCalls = 0;
    replay::CommandedPose lastCommanded;
    bool                  acceptCommand = true;
    bool                  acceptRelease = true;
    replay::ControlRefusal refusalWord = replay::ControlRefusal::None;
    replay::PoseRegime     regime      = replay::PoseRegime::Free;
    replay::camera::ViewPoseRead readBack = replay::camera::ViewPoseRead::Ok;
    replay::ViewPose            landedPose;
    // What camera_control::Diagnostic() would carry. Settable because the look-at refusals are
    // told apart by their SubjectResolve sentence while sharing one ControlRefusal word.
    std::string diagnostic = "diag";

    bool CommandPose(const replay::CommandedPose& p) override {
        ++commandPoseCalls;
        lastCommanded = p;
        if (acceptCommand) {
            regime = replay::PoseRegime::Commanded;
        }
        return acceptCommand;
    }
    bool ReleasePose() override {
        ++releasePoseCalls;
        if (acceptRelease) {
            regime = replay::PoseRegime::Free;
        }
        return acceptRelease;
    }
    replay::camera::ViewPoseRead GetViewPose(replay::ViewPose* out) const override {
        if (readBack == replay::camera::ViewPoseRead::Ok) {
            *out = landedPose;
        }
        return readBack;
    }
    replay::PoseRegime     Regime() const override { return regime; }
    replay::ControlRefusal LastRefusal() const override { return refusalWord; }

    bool SetMode(replay::CameraMode, int32_t) override { return false; }
    replay::CameraMode RequestedMode() const override { return replay::CameraMode::Free; }
    bool ObservedMode(replay::CameraMode*) const override { return false; }
    bool ObservedSelector(int32_t*, uint16_t*) const override { return false; }
    bool ActiveLevelCameraSet(uint16_t*) const override { return false; }
    bool BeginOracleTrack() override { return false; }
    void EndOracleTrack() override {}
    bool OracleTrackActive() const override { return false; }
    bool Available() const override { return true; }
};

// The arm, minus the two lines of dispatcher that fetch the control and wrap the text in a Result.
// Written once here so no case can drift into testing a different order of operations than the one
// dispatch.cpp performs.
struct CamPoseReply {
    bool        ok = false;
    std::string text;
    CamPoseOutcome outcome = CamPoseOutcome::Refused;
};

CamPoseReply DriveCamPose(RecordingCam* cam, const char* verb, const Command& cmd) {
    replay::CommandedPose p;
    PoseFromCommand(cmd, &p);
    const bool accepted = cam->CommandPose(p);
    // Through the published sampler, exactly as the arm does -- not cam->GetViewPose directly.
    replay::camera_control::ViewPoseSample sample;
    replay::camera::ViewPoseRead read = replay::camera::ViewPoseRead::NotBound;
    if (accepted) {
        read = replay::camera_control::SampleViewPose(cam, &sample);
    }
    char buf[2048];
    CamPoseReply out;
    out.outcome = ClassifyCamPose(accepted, read);
    switch (out.outcome) {
        case CamPoseOutcome::Refused:
            FormatCamPoseRefused(verb, cam->LastRefusal(), cam->diagnostic.c_str(), buf,
                                 sizeof buf);
            break;
        case CamPoseOutcome::Landed:
            if (p.lookAt) {
                FormatCamLookAtLanded(verb, p.lookAtSelector, sample.pose, cam->Regime(), buf,
                                      sizeof buf);
            } else {
                FormatCamPoseLanded(verb, sample.pose, cam->Regime(), buf, sizeof buf);
            }
            break;
        case CamPoseOutcome::CommandedNotReadBack:
            FormatCamPoseNotReadBack(verb, read, "read refusal", buf, sizeof buf);
            break;
    }
    out.ok   = CamPoseOk(out.outcome);
    out.text = buf;
    return out;
}

// Facing +Z, i.e. `cam look to 0 0`: the identity quaternion under ARCH-19 E2's convention.
const float kIdentityQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};

std::string ReadRepoFile(const char* relative) {
    std::ifstream in(std::filesystem::path(REPLAY_REPO_ROOT) / relative);
    REQUIRE_MESSAGE(in.good(), "cannot read ", relative);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Code only: a prose mention of a name is not a call site, and this whole family is discussed in
// comments in both files the scan reads.
std::string CodeOnly(const std::string& text) {
    std::string out;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t eol = text.find('\n', at);
        const std::string line = text.substr(at, eol == std::string::npos ? eol : eol - at);
        std::string bare = line.substr(0, line.find("//"));
        while (!bare.empty() && (bare.back() == '\r' || bare.back() == ' ')) {
            bare.pop_back();
        }
        out += bare;
        out += '\n';
        if (eol == std::string::npos) break;
        at = eol + 1;
    }
    return out;
}

// The S9 criterion, anchored: the member name preceded by `->` and followed by its call paren.
// Unanchored, `ReleasePose` matches `ReleasePoseOwner` and the criterion is
// already non-empty at HEAD with no shipping caller at all.
int ArrowCallSites(const std::string& code, const std::string& name) {
    const std::string tok = "->" + name + "(";
    int n = 0;
    for (size_t at = code.find(tok); at != std::string::npos; at = code.find(tok, at + 1)) {
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("P17-E: PoseFromCommand carries the parser's FLAGS, and puts degrees where degrees go") {
    // cam move, relative: a position delta and nothing else. setOrientation false is the R6 row
    // that means "do not touch the facing", not "face 0,0".
    replay::CommandedPose p;
    PoseFromCommand(Ok(L"cam move 1 -2 3.5"), &p);
    CHECK(p.setPosition);
    CHECK_FALSE(p.setOrientation);
    CHECK_FALSE(p.absolute);
    CHECK(p.pos[0] == doctest::Approx(1.0f));
    CHECK(p.pos[1] == doctest::Approx(-2.0f));
    CHECK(p.pos[2] == doctest::Approx(3.5f));
    CHECK(p.yawDeg == doctest::Approx(0.0f));
    CHECK(p.pitchDeg == doctest::Approx(0.0f));
    CHECK_FALSE(p.lookAt);

    // `cam move to` is the SAME three numbers with one flag flipped. A sentinel would make
    // `cam move to 0 0 0` and `cam move 0 0 0` the same command; they are not.
    PoseFromCommand(Ok(L"cam move to 0 0 0"), &p);
    CHECK(p.setPosition);
    CHECK(p.absolute);
    CHECK(p.pos[0] == doctest::Approx(0.0f));
    PoseFromCommand(Ok(L"cam move 0 0 0"), &p);
    CHECK(p.setPosition);
    CHECK_FALSE(p.absolute);

    // cam look: the DEGREES the parser produced, in the degree fields. This is the mutant the
    // packet names -- an angle handed over un-decomposed, or written into pos[] because both are
    // carried by camVec on the way in.
    PoseFromCommand(Ok(L"cam look 90 -10"), &p);
    CHECK(p.setOrientation);
    CHECK_FALSE(p.setPosition);
    CHECK_FALSE(p.absolute);
    CHECK(p.yawDeg == doctest::Approx(90.0f));
    CHECK(p.pitchDeg == doctest::Approx(-10.0f));
    CHECK(p.pos[0] == doctest::Approx(0.0f));
    CHECK(p.pos[1] == doctest::Approx(0.0f));
    CHECK(p.pos[2] == doctest::Approx(0.0f));

    PoseFromCommand(Ok(L"cam look to 45 0"), &p);
    CHECK(p.setOrientation);
    CHECK(p.absolute);
    CHECK(p.yawDeg == doctest::Approx(45.0f));

    // `cam look at <n>` is ARCH-19 R6's true/true row: an absolute facing, no angles of its own,
    // and the selector the parser bounded.
    PoseFromCommand(Ok(L"cam look at 3"), &p);
    CHECK(p.lookAt);
    CHECK(p.lookAtSelector == 3);
    CHECK(p.setOrientation);
    CHECK(p.absolute);
    CHECK_FALSE(p.setPosition);
}

TEST_CASE("P17-E: the three outcomes are three, and only a refusal is a failed command") {
    using replay::camera::ViewPoseRead;
    CHECK(ClassifyCamPose(false, ViewPoseRead::Ok) == CamPoseOutcome::Refused);
    CHECK(ClassifyCamPose(true, ViewPoseRead::Ok) == CamPoseOutcome::Landed);
    // The one ARCH-19 E3 exists for: the write landed and we cannot say where. It is NOT a refusal
    // and it is NOT a landing, and a two-valued classifier would have to call it one of them.
    CHECK(ClassifyCamPose(true, ViewPoseRead::Unreadable) == CamPoseOutcome::CommandedNotReadBack);
    CHECK(ClassifyCamPose(true, ViewPoseRead::NotBound) == CamPoseOutcome::CommandedNotReadBack);
    CHECK(ClassifyCamPose(true, ViewPoseRead::NoViewState) == CamPoseOutcome::CommandedNotReadBack);

    CHECK_FALSE(CamPoseOk(CamPoseOutcome::Refused));
    CHECK(CamPoseOk(CamPoseOutcome::Landed));
    CHECK(CamPoseOk(CamPoseOutcome::CommandedNotReadBack));
}

TEST_CASE("P17-E: `cam move` reports the pose that LANDED, not the one that was asked for") {
    RecordingCam cam;
    cam.landedPose.pos[0] = 12.4f;
    cam.landedPose.pos[1] = 3.1f;
    cam.landedPose.pos[2] = -8.75f;
    memcpy(cam.landedPose.quat, kIdentityQuat, sizeof kIdentityQuat);
    cam.landedPose.source = replay::ViewPoseSource::ViewState;

    // The request and the landing disagree ON PURPOSE (ARCH-17 S3.4): this fake's engine put the
    // camera somewhere else. A reply echoing the request would be indistinguishable from a reply
    // that read the engine, which is the whole point of the read-back.
    const CamPoseReply r = DriveCamPose(&cam, "cam move", Ok(L"cam move to 1 1 1"));
    CHECK(cam.commandPoseCalls == 1);
    CHECK(cam.releasePoseCalls == 0);      // the wrong-function mutant
    CHECK(cam.lastCommanded.absolute);
    CHECK(cam.lastCommanded.pos[0] == doctest::Approx(1.0f));
    CHECK(r.ok);
    CHECK(r.outcome == CamPoseOutcome::Landed);
    CHECK(r.text.find("12.400") != std::string::npos);
    CHECK(r.text.find("-8.750") != std::string::npos);
    CHECK(r.text.find("regime commanded") != std::string::npos);
    CHECK(r.text.find("source view-state") != std::string::npos);
    // The identity quaternion is `cam look to 0 0` under the one convention, and the angles are
    // printed from it rather than from the request -- which named no angles at all.
    CHECK(r.text.find("yaw 0.0 pitch 0.0") != std::string::npos);
    CHECK(r.text.find("1.000") == std::string::npos);   // nothing of the request survives
}

TEST_CASE("P17-E: a refusal is a NAMED word and never an ok") {
    RecordingCam cam;
    cam.acceptCommand = false;
    cam.refusalWord   = replay::ControlRefusal::FacingDegenerate;
    const CamPoseReply r = DriveCamPose(&cam, "cam look", Ok(L"cam look 10 0"));
    CHECK(cam.commandPoseCalls == 1);
    CHECK_FALSE(r.ok);                                    // the swallowed-refusal mutant
    CHECK(r.outcome == CamPoseOutcome::Refused);
    CHECK(r.text.find("facing-degenerate") != std::string::npos);
    CHECK(r.text.find("diag") != std::string::npos);
    // A refused command never read the engine back, so it carries no pose and cannot be mistaken
    // for one.
    CHECK(r.text.find("regime") == std::string::npos);
    CHECK(r.text.find("yaw") == std::string::npos);

    // Every appended ARCH-19 R4 value has a word of its own here, because a probe row scores on it.
    for (replay::ControlRefusal word :
         {replay::ControlRefusal::PoseUnreadable, replay::ControlRefusal::WritePointRefused,
          replay::ControlRefusal::FacingDegenerate, replay::ControlRefusal::PoseNotFinite,
          replay::ControlRefusal::LookAtUnresolvable}) {
        char buf[256];
        FormatCamPoseRefused("cam look", word, "why", buf, sizeof buf);
        const std::string said(buf);
        const std::string expected(replay::ControlRefusalName(word));
        CHECK(expected != "unknown");
        CHECK_MESSAGE(said.find(expected) != std::string::npos, said);
    }
}

TEST_CASE("P17-E: commanded-but-unreadable is its own sentence with no pose in it") {
    RecordingCam cam;
    cam.readBack = replay::camera::ViewPoseRead::Unreadable;
    cam.landedPose.pos[0] = 999.0f;    // never printed: the read did not return Ok
    const CamPoseReply r = DriveCamPose(&cam, "cam move", Ok(L"cam move 1 0 0"));
    CHECK(cam.commandPoseCalls == 1);
    CHECK(r.outcome == CamPoseOutcome::CommandedNotReadBack);
    CHECK(r.ok);                       // the command DID what it said -- the Result stays true
    CHECK(r.text.find("could not be read back") != std::string::npos);
    CHECK(r.text.find("read refusal") != std::string::npos);
    CHECK(r.text.find("999") == std::string::npos);
    CHECK(r.text.find("at (") == std::string::npos);
    CHECK(r.text.find("regime") == std::string::npos);
    // The four-word reason, from the engine's own table.
    CHECK(r.text.find(replay::camera::ViewPoseReadName(replay::camera::ViewPoseRead::Unreadable)) !=
          std::string::npos);
}

TEST_CASE("P17-E: a vertical landing says `facing vertical`, never yaw 0 pitch 0") {
    // Straight down. DecomposeFacing refuses here and leaves its out-parameters untouched, so a
    // sentence that printed them would print two zeros -- a DIFFERENT pose, and one the camera is
    // not in.
    replay::ViewPose pose;
    replay::camera_control::BuildFacing(0.0f, 90.0f, pose.quat);
    float yaw = 0.0f, pitch = 0.0f;
    // The premise, asserted rather than assumed: if BuildFacing/DecomposeFacing ever round-trip
    // this, the case below stops being about a degenerate facing and must be re-chosen.
    REQUIRE_FALSE(replay::camera_control::DecomposeFacing(pose.quat, &yaw, &pitch));

    pose.pos[0] = 1.5f;
    char buf[512];
    FormatCamPoseLanded("cam look", pose, replay::PoseRegime::Commanded, buf, sizeof buf);
    const std::string said(buf);
    CHECK(said.find("facing vertical") != std::string::npos);
    CHECK(said.find("yaw") == std::string::npos);
    CHECK(said.find("pitch") == std::string::npos);
    CHECK(said.find("1.500") != std::string::npos);
    CHECK(said.find("regime commanded") != std::string::npos);

    // The control: a horizontal facing DOES print its angles, so the branch above is a branch and
    // not the only thing this function can say. The expected angles are taken from DecomposeFacing
    // itself, because what this function must do is PRINT WHAT THE DECOMPOSITION SAID -- asserting
    // the numbers that went into BuildFacing instead would be asserting a property of W2-E's pair,
    // and that property does not hold today (see the round-trip case below).
    replay::camera_control::BuildFacing(41.25f, -6.0f, pose.quat);
    REQUIRE(replay::camera_control::DecomposeFacing(pose.quat, &yaw, &pitch));
    FormatCamPoseLanded("cam look", pose, replay::PoseRegime::Commanded, buf, sizeof buf);
    const std::string level(buf);
    CHECK(level.find("facing vertical") == std::string::npos);
    char expect[64];
    snprintf(expect, sizeof expect, "yaw %.1f pitch %.1f", yaw, pitch);
    CHECK_MESSAGE(level.find(expect) != std::string::npos, level);
    CHECK_MESSAGE(level.find("41.2") != std::string::npos, level);
}

TEST_CASE("P17-E: `cam release` says which of the three things it did") {
    RecordingCam cam;
    cam.regime = replay::PoseRegime::Commanded;
    const replay::PoseRegime before = cam.Regime();
    REQUIRE(cam.ReleasePose());
    CHECK(cam.releasePoseCalls == 1);
    CHECK(cam.commandPoseCalls == 0);          // the wrong-function mutant, other direction
    char buf[512];
    FormatCamRelease(before, cam.Regime(), buf, sizeof buf);
    CHECK(std::string(buf).find("is back with the stock free camera") != std::string::npos);
    CHECK(std::string(buf).find("regime free") != std::string::npos);

    // Already free: a success that did nothing, and it says so rather than claiming a hand-back.
    FormatCamRelease(replay::PoseRegime::Free, replay::PoseRegime::Free, buf, sizeof buf);
    CHECK(std::string(buf).find("was already with the stock free camera") != std::string::npos);

    // No rig at all is a third state, not a quieter version of the second.
    FormatCamRelease(replay::PoseRegime::Unavailable, replay::PoseRegime::Unavailable, buf,
                     sizeof buf);
    CHECK(std::string(buf).find("nothing to hand back") != std::string::npos);
    CHECK(std::string(buf).find("regime unavailable") != std::string::npos);
}

TEST_CASE("P17-E S9: the family has a shipping non-test caller, and it is the dispatcher") {
    // ARCH-19 E4, BOTH directions. The frozen grep is already non-empty at
    // HEAD on `ReleasePoseOwner`, and its `must name dispatch.cpp` half is satisfiable by a
    // comment. This case anchors on the call form and strips comments, so neither false pass is
    // available to it -- and it asserts the OTHER direction too, which a one-sided grep cannot see.
    const std::string cmdName = std::string("Command") + "Pose";
    const std::string relName = std::string("Release") + "Pose";

    // The known-answer control, because a matcher nobody has seen say NO has not been shown able
    // to. Both strings are assembled so this file cannot match itself.
    CHECK(ArrowCallSites(relName + "Owner(PoseOwner::WritePoint);", relName) == 0);
    CHECK(ArrowCallSites("camera::" + relName + "();", relName) == 0);   // not through a `->`
    CHECK(ArrowCallSites("cam->" + relName + "();", relName) == 1);

    const std::string dispatch = CodeOnly(ReadRepoFile("src/control/dispatch.cpp"));
    const int commanded = ArrowCallSites(dispatch, cmdName);
    const int released  = ArrowCallSites(dispatch, relName);
    MESSAGE("dispatch.cpp anchored call sites: " << cmdName << "=" << commanded << " " << relName
                                                 << "=" << released);
    CHECK(commanded > 0);       // non-empty, for a real reason
    CHECK(released > 0);

    // Direction two, which S9 did not state: the panel reaches this family only through
    // dispatch::Submit (ARCH-17 S3.4). One that called the interface itself would sit below the
    // command gate -- the RES-TRANSPORT-CMDS S2 defect.
    const std::string panel = CodeOnly(ReadRepoFile("src/ui/panel.cpp"));
    CHECK(ArrowCallSites(panel, cmdName) == 0);
    CHECK(ArrowCallSites(panel, relName) == 0);
    // And the scan read a real file rather than an empty string, which would make the two zeros
    // above vacuous. It contains no `Submit` yet -- P17-F is the packet that adds one (ARCH-19 E5),
    // and that it has none is itself the state E5 describes.
    CHECK(panel.size() > 500);
    CHECK(panel.find("Submit") == std::string::npos);

    // The routing, which the fake above cannot see: each id reaches the arm that calls the member
    // the contract names for it.
    const size_t poseArm    = dispatch.find("Result RunCamPose(");
    const size_t releaseArm = dispatch.find("Result RunCamRelease(");
    REQUIRE(poseArm != std::string::npos);
    REQUIRE(releaseArm != std::string::npos);
    REQUIRE(releaseArm > poseArm);
    const std::string poseBody = dispatch.substr(poseArm, releaseArm - poseArm);
    CHECK(ArrowCallSites(poseBody, cmdName) == 1);
    CHECK(ArrowCallSites(poseBody, relName) == 0);

    // THE NEW CALL SITE (`cam look at`). The read-back leaves the arm through the PUBLISHED
    // sampler camera_control::SampleViewPose -- the same body ARCH-17 S7.4 points p10-cam-7/9 at
    // -- and no longer through cam->GetViewPose, so the row that scores a landing and the sentence
    // that reports it cannot read two different poses. Known-answer controls first, both ways.
    auto countOf = [](const std::string& hay, const std::string& needle) {
        size_t n = 0;
        for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
            ++n;
        }
        return n;
    };
    const std::string sampler = std::string("Sample") + "ViewPose(";
    const std::string getter  = std::string("Get") + "ViewPose";
    CHECK(countOf("camera_control::" + sampler, sampler) == 1);
    CHECK(countOf("nothing of the sort here", sampler) == 0);
    CHECK(ArrowCallSites("cam->" + getter + "(&back);", getter) == 1);
    CHECK(countOf(poseBody, sampler) == 1);
    CHECK(ArrowCallSites(dispatch, getter) == 0);
    // And the arm formats the look-at landing with the formatter that names the subject.
    CHECK(countOf(poseBody, std::string("FormatCamLookAt") + "Landed(") == 1);
    const size_t afterRelease = dispatch.find("Result Execute(", releaseArm);
    REQUIRE(afterRelease != std::string::npos);
    const std::string releaseBody = dispatch.substr(releaseArm, afterRelease - releaseArm);
    CHECK(ArrowCallSites(releaseBody, relName) == 1);
    CHECK(ArrowCallSites(releaseBody, cmdName) == 0);

    for (const char* arm : {"case CommandId::CamMove:", "case CommandId::CamLook:",
                            "case CommandId::CamRelease:"}) {
        CHECK_MESSAGE(dispatch.find(arm) != std::string::npos, arm);
    }
    // Each label reaches the arm the contract names for it: the routing mutant is a label that
    // falls into the neighbouring case body.
    for (const char* label : {"CamMove:", "CamLook:"}) {
        const size_t at = dispatch.find(label);
        REQUIRE(at != std::string::npos);
        const std::string body = dispatch.substr(at, 60);
        CHECK_MESSAGE(body.find("RunCamPose(cmd)") != std::string::npos, body);
        CHECK_MESSAGE(body.find("RunCamRelease(cmd)") == std::string::npos, body);
    }
    const size_t relLabel = dispatch.find("CamRelease:");
    REQUIRE(relLabel != std::string::npos);
    CHECK(dispatch.substr(relLabel, 60).find("RunCamRelease(cmd)") != std::string::npos);
}

TEST_CASE("P17-CAMCMD: the cam family cannot reach the transport, so a paused view moves alone") {
    // The owner's clause is "freely move around", and the probe row it is scored on
    // (w2e-4b-view-moves-while-paused) watches the view across a span in which the PLAYHEAD must
    // not have moved. Nothing offline pinned that: the family's independence from the transport
    // lived in a comment on the spec table and in the shape of two dispatcher arms, and an edit
    // that reached for `t->` inside either arm would have been caught by no assertion at all.
    //
    // Two legs, because either alone can be satisfied while the other is broken: the arm bodies
    // carry no transport reference, AND the gate that runs them demands no transport service (a
    // family that named kNeedsTransport would refuse `cam move` on an idle rig, which is the
    // opposite defect and equally a failure of the clause).
    auto transportTouches = [](const std::string& body) {
        size_t n = 0;
        for (const char* tok : {"t->", "g_services->transport", "transport->", "IReplayTransport"}) {
            for (size_t at = body.find(tok); at != std::string::npos; at = body.find(tok, at + 1)) {
                ++n;
            }
        }
        return n;
    };
    // Known-answer control, both ways, before the scanner is trusted on the arms it is aimed at.
    CHECK(transportTouches("cam->CommandPose(p);") == 0);
    CHECK(transportTouches("g_services->transport->Pause();") > 0);

    const std::string dispatch = CodeOnly(ReadRepoFile("src/control/dispatch.cpp"));
    const size_t speedArm   = dispatch.find("Result RunSpeed(");
    const size_t poseArm    = dispatch.find("Result RunCamPose(");
    const size_t releaseArm = dispatch.find("Result RunCamRelease(");
    const size_t executeArm = dispatch.find("Result Execute(");
    REQUIRE(speedArm != std::string::npos);
    REQUIRE(poseArm != std::string::npos);
    REQUIRE(releaseArm > poseArm);
    REQUIRE(executeArm > releaseArm);

    // The control is a REAL arm of the same file, not a literal: a scanner that scored zero on
    // every body in dispatch.cpp would pass the two checks below while seeing nothing.
    const std::string speedBody = dispatch.substr(speedArm, dispatch.find("Result ", speedArm + 8) - speedArm);
    CHECK(transportTouches(speedBody) > 0);

    const std::string poseBody    = dispatch.substr(poseArm, releaseArm - poseArm);
    const std::string releaseBody = dispatch.substr(releaseArm, executeArm - releaseArm);
    CHECK(poseBody.size() > 200);       // a slice that came back empty proves nothing
    CHECK(releaseBody.size() > 200);
    CHECK_MESSAGE(transportTouches(poseBody) == 0, "RunCamPose reaches the transport");
    CHECK_MESSAGE(transportTouches(releaseBody) == 0, "RunCamRelease reaches the transport");

    // Leg two: the gate. Not kNeedsTransport and not kNeedsLoaded -- and the control is Play,
    // whose bit is set, so a mask read that returned zero for everything cannot pass this.
    CHECK((SpecFor(CommandId::Play)->needs & kNeedsTransport) != 0);
    for (CommandId id : {CommandId::CamMove, CommandId::CamLook, CommandId::CamRelease}) {
        const CommandSpec* s = SpecFor(id);
        REQUIRE(s != nullptr);
        CHECK_MESSAGE((s->needs & (kNeedsTransport | kNeedsLoaded)) == 0, s->name);
        CHECK((s->needs & kNeedsCamera) != 0);
    }
}

TEST_CASE("P17-CAMCMD: the `diag` text an operator reads is the one ui::WriteDiagnostics wrote") {
    // uiapi.h publishes the dump; dispatch.cpp is its only consumer, and the trip is two hops --
    // the tick copies it into the snapshot, the `diag` renderer prints the copy. Either hop can be
    // deleted without breaking a build, and the surviving hop leaves a plausible `diag` that is
    // simply missing a section. Both are asserted, with the tokens built so this file cannot match
    // itself.
    const std::string call  = std::string("ui::Write") + "Diagnostics(";
    const std::string field = std::string("s.ui") + "Diagnostics";
    auto countOf = [](const std::string& hay, const std::string& needle) {
        size_t n = 0;
        for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
            ++n;
        }
        return n;
    };
    CHECK(countOf("nothing of the sort here", call) == 0);
    CHECK(countOf(call + call, call) == 2);

    const std::string dispatch = CodeOnly(ReadRepoFile("src/control/dispatch.cpp"));
    // Hop one: read once, on the tick, into the buffer the snapshot is filled from -- not into a local
    // that is dropped. PERF-1 put a 250 ms cadence between the read and the snapshot, because the read
    // walks two 512-bucket histograms and nothing consumes the result until an operator asks for it,
    // so the assertion is now three links rather than two: one read, one named buffer, one copy.
    const std::string buffer = std::string("g_ui") + "DiagText";
    CHECK(countOf(dispatch, call) == 1);
    CHECK(countOf(dispatch, call + buffer + ", sizeof " + buffer + ")") == 1);
    CHECK(countOf(dispatch, "CopyStr(" + field + ", sizeof " + field + ", " + buffer + ")") == 1);

    // Hop two: the renderer prints that field. Sliced from the definition, so a decl or the call
    // site cannot stand in for the body.
    const size_t diagDef = dispatch.find("void FormatDiag(");
    REQUIRE(diagDef != std::string::npos);
    const std::string diagBody = dispatch.substr(diagDef);
    CHECK(diagBody.size() > 400);  // facts-ok: AUDIO.LISTENER_COUNT_OFF a minimum length in characters for a function body read out of this file, not an engine offset
    CHECK_MESSAGE(countOf(diagBody, field) > 0, "FormatDiag never prints the ui dump");

    // And `diag` is a command the surfaces can actually reach, answered immediately rather than
    // queued: a route that never runs renders nothing however well the two hops are joined.
    const CommandSpec* spec = SpecFor(CommandId::Diag);
    REQUIRE(spec != nullptr);
    CHECK(std::string(spec->name) == "diag");
    CHECK(Ok(L"diag").id == CommandId::Diag);
    CHECK(countOf(dispatch, "FormatDiag(snap") == 1);
}

TEST_CASE("P17-E: the dispatcher does not carry a second facing convention") {
    // ARCH-19 E2: there is ONE quaternion<->facing pair in the tree. A dispatcher that grew its own
    // trigonometry would be two surfaces disagreeing about which way the camera points, which is
    // the defect the pair was published to prevent. The tokens are BUILT so this scan cannot count
    // the file it lives in.
    const std::string dispatch = CodeOnly(ReadRepoFile("src/control/dispatch.cpp"));
    for (const std::string fn : {std::string("atan") + "2", std::string("asin"),
                                 std::string("acos"), std::string("quat")}) {
        CHECK_MESSAGE(dispatch.find(fn) == std::string::npos, fn);
    }
    // The control: those tokens ARE findable in the file that legitimately owns the convention, so
    // an empty result above is a property of dispatch.cpp and not of this matcher.
    const std::string owner = CodeOnly(ReadRepoFile("src/camera/camera_control.cpp"));
    CHECK(owner.find(std::string("atan") + "2") != std::string::npos);
    CHECK(owner.find(std::string("quat")) != std::string::npos);
}

TEST_CASE("L3-RUN-3 F1: a seek that names a target must be classified against the playhead") {
    // Three answers, not two. The absolute arm has read the playhead back since 6e18393; the
    // relative, marker-step and bookmark arms printed their own arithmetic, so `seek +10` answered
    // "to 12133312 us" having landed on 2255533 us.
    // The tolerance is the CALLER'S since AUD-P17 F4, so this case names one: 16666 us, the frame
    // period a 60 Hz machine measures. seekgoto.h holds no period of its own any more.
    const int64_t toleranceUs = 16666;
    CHECK(ClassifySeekLandingUs(2133312, 2133312, toleranceUs) == SeekLanding::OnTarget);
    CHECK(ClassifySeekLandingUs(2133312, 2133313, toleranceUs) == SeekLanding::NearestTick);
    CHECK(ClassifySeekLandingUs(2133312, 2133312 - toleranceUs, toleranceUs) ==
          SeekLanding::NearestTick);
    CHECK(ClassifySeekLandingUs(2133312, 2133312 + toleranceUs, toleranceUs) ==
          SeekLanding::NearestTick);
    CHECK(ClassifySeekLandingUs(2133312, 2133312 + toleranceUs + 1, toleranceUs) ==
          SeekLanding::OffTarget);

    // The two misses F1 actually recorded, which is what makes this a retroactive check and not a
    // fixture chosen to pass.
    CHECK(ClassifySeekLandingUs(12133312, 2255533, toleranceUs) == SeekLanding::OffTarget);
    CHECK(ClassifySeekLandingUs(2133312, 2255533, toleranceUs) == SeekLanding::OffTarget);
    CHECK(std::string(SeekLandingName(ClassifySeekLandingUs(12133312, 2255533, toleranceUs))) ==
          "landed-off-target");

    // The magnitude is taken in the unsigned domain, so no pair of int64 times can wrap it into a
    // false OnTarget.
    CHECK(ClassifySeekLandingUs(INT64_MIN, INT64_MAX, toleranceUs) == SeekLanding::OffTarget);
    CHECK(ClassifySeekLandingUs(INT64_MAX, INT64_MIN, toleranceUs) == SeekLanding::OffTarget);
    CHECK(ClassifySeekLandingUs(INT64_MIN, INT64_MIN, toleranceUs) == SeekLanding::OnTarget);

    // And the arms actually read it back. This is the half a pure-function sweep cannot see, and
    // it is the half that was broken: every arm that names a target time now calls PlayheadUs()
    // AFTER its seek and classifies what it finds.
    // Since F4 the arms route through one helper that ASKS for the tolerance before it classifies,
    // so the call sites to count are that helper's.
    const std::string dispatch = CodeOnly(ReadRepoFile("src/control/dispatch.cpp"));
    size_t classifies = 0;
    for (size_t at = dispatch.find("bad = SeekLandingRefusal("); at != std::string::npos;
         at = dispatch.find("bad = SeekLandingRefusal(", at + 1)) {
        ++classifies;
    }
    // goto resume, goto next/prev, goto <n>, the relative seek, and the percentage seek.
    CHECK(classifies == 5);
    size_t offTarget = 0;
    for (size_t at = dispatch.find("SeekLanding::OffTarget"); at != std::string::npos;
         at = dispatch.find("SeekLanding::OffTarget", at + 1)) {
        ++offTarget;
    }
    // ZERO in the dispatcher, ONE in seekgoto.h. An arm testing the verdict itself would be an arm
    // that could report `on-target` on a capture that published no cadence. The count moved out of
    // this file with the decision: it used to be one HERE, the helper's own, and
    // the helper's body is now next to the classifier so the offline suite can execute it. Both
    // halves are asserted, so the occurrence cannot simply go missing.
    CHECK(offTarget == 0);
    const std::string seekgotoSrc = CodeOnly(ReadRepoFile("src/control/seekgoto.h"));
    size_t offTargetInHelper = 0;
    for (size_t at = seekgotoSrc.find("SeekLanding::OffTarget"); at != std::string::npos;
         at = seekgotoSrc.find("SeekLanding::OffTarget", at + 1)) {
        ++offTargetInHelper;
    }
    // Four: SeekLandingName's case label, the classifier's two returns, and the helper's one
    // comparison. Nothing that names an ARM.
    CHECK(offTargetInHelper == 4);
}

TEST_CASE("The landing tolerance is the interval MEASURED, never a compiled-in 33334") {
    // The fixture is a machine whose frame period is 40000 us. A landing exactly one such interval
    // away is a correct neighbouring tick THERE -- and the constant this replaces (33334 us, one
    // 30 Hz frame, asked of nobody) calls the same landing a miss. That disagreement is the whole
    // finding, so both readings are asserted on the same pair of times.
    const int64_t target = 2133312;
    const int64_t measuredUs = 40000;
    const int64_t retiredConstantUs = 33334;
    REQUIRE(measuredUs > retiredConstantUs);
    CHECK(ClassifySeekLandingUs(target, target + measuredUs, measuredUs) ==
          SeekLanding::NearestTick);
    CHECK(ClassifySeekLandingUs(target, target - measuredUs, measuredUs) ==
          SeekLanding::NearestTick);
    CHECK(ClassifySeekLandingUs(target, target + measuredUs, retiredConstantUs) ==
          SeekLanding::OffTarget);
    CHECK(ClassifySeekLandingUs(target, target + measuredUs + 1, measuredUs) ==
          SeekLanding::OffTarget);
    // An interval of zero is not a licence to accept anything: only an exact landing passes.
    CHECK(ClassifySeekLandingUs(target, target, 0) == SeekLanding::OnTarget);
    CHECK(ClassifySeekLandingUs(target, target + 1, 0) == SeekLanding::OffTarget);

    // The number is ASKED FOR, and the fourth answer exists. `could-not-ask` is not a landing
    // verdict and SeekLanding cannot express it, so it lives beside the classifier; neither file
    // may carry a period of its own again.
    //
    // ★ WHICH CADENCE IS ASKED FOR CHANGED, and this case used to assert the wrong one. It required
    // `TickIntervalUs` in dispatch.cpp -- the RENDER frame period -- which is a fact about this
    // machine and not about the file. The assertion is inverted here rather than deleted: the
    // render period must never come back as the answer to a question about a capture.
    const std::string dispatch = CodeOnly(ReadRepoFile("src/control/dispatch.cpp"));
    CHECK(dispatch.find("TickIntervalUs") == std::string::npos);
    CHECK(dispatch.find("SeekLandingRefusalFor") != std::string::npos);
    CHECK(dispatch.find("33334") == std::string::npos);
    const std::string seekgoto = CodeOnly(ReadRepoFile("src/control/seekgoto.h"));
    CHECK(seekgoto.find("TickSpacingUs") != std::string::npos);
    CHECK(seekgoto.find("could-not-ask") != std::string::npos);
    CHECK(seekgoto.find("TickIntervalUs") == std::string::npos);
    CHECK(seekgoto.find("kSeekLandingToleranceUs") == std::string::npos);
    CHECK(seekgoto.find("33334") == std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// The landing tolerance is the CAPTURE's sample spacing, not the render period.
//
// A transport that publishes a spacing and counts how often it was asked. Everything else is the
// minimum B.8 requires; none of it is read by the case below, and PlayheadUs deliberately returns
// a value no assertion depends on so nothing can pass by reading the wrong member.
// ---------------------------------------------------------------------------------------------
namespace {

class ToleranceTransport final : public replay::IReplayTransport {
public:
    int64_t spacingUs = 0;              // 0 == this capture publishes no cadence
    mutable int spacingAsked = 0;

    bool TickSpacingUs(int64_t* outUs) const override {
        ++spacingAsked;
        if (spacingUs <= 0) {
            return false;
        }
        *outUs = spacingUs;
        return true;
    }

    bool Load(const wchar_t*) override { return false; }
    void Unload() override {}
    void Play(float) override {}
    void Pause() override {}
    void Step(int) override {}
    void Seek(int64_t) override {}
    bool SeekRound(replay::RoundNumber) override { return false; }
    void SetMode(bool) override {}
    replay::TransportState State() const override { return replay::TransportState::NotLoaded; }
    float Rate() const override { return 0.0f; }
    int64_t PlayheadUs() const override { return -1; }
    uint64_t DurationUs() const override { return 0; }
    const char* ActiveModeLabel() const override { return "tolerance-fixture"; }
};

}  // namespace

TEST_CASE("A landing is judged against the capture's tick spacing, not the frame "
          "period") {
    // A 30 Hz capture on a 90 Hz machine -- the two cadences that are independent and were being
    // confused. Both are derived from their rates here rather than typed, so neither can be edited
    // into agreement without the arithmetic saying so.
    const int64_t captureSpacingUs = 1000000 / 30;   // 33333: this FILE's sample spacing
    const int64_t renderPeriodUs = 1000000 / 90;     // 11111: how fast this MACHINE draws
    REQUIRE(captureSpacingUs > renderPeriodUs);

    const int64_t target = 2133312;
    // Inside the file's spacing, outside the machine's frame period: a correct landing on the
    // neighbouring recorded tick that the render period called a miss.
    const int64_t landedUs = target + 16000;
    REQUIRE(landedUs - target > renderPeriodUs);
    REQUIRE(landedUs - target < captureSpacingUs / 2);

    // THE DEFECT, stated as an executed fact: judged against the render period the same landing is
    // refused. This is the reading that shipped, and it must stay false of the capture's spacing.
    CHECK(ClassifySeekLandingUs(target, landedUs, renderPeriodUs) == SeekLanding::OffTarget);

    ToleranceTransport t;
    t.spacingUs = captureSpacingUs;

    int64_t askedUs = 0;
    REQUIRE(SeekToleranceUs(&t, &askedUs) == SeekTolerance::FromCapture);
    CHECK(askedUs == captureSpacingUs);
    CHECK(t.spacingAsked == 1);

    SeekLanding landing = SeekLanding::OffTarget;
    CHECK(SeekLandingRefusalFor(&t, target, landedUs, &landing) == nullptr);
    CHECK(landing == SeekLanding::NearestTick);

    // A landing wider than HALF the spacing is still the neighbouring tick. Without this row a
    // tolerance of spacing/2 -- an easy and wrong "nearest" reading -- classifies every assertion
    // above identically.
    const int64_t farNeighbour = target + 30000;
    REQUIRE(farNeighbour - target > captureSpacingUs / 2);
    REQUIRE(farNeighbour - target < captureSpacingUs);
    landing = SeekLanding::OffTarget;
    CHECK(SeekLandingRefusalFor(&t, target, farNeighbour, &landing) == nullptr);
    CHECK(landing == SeekLanding::NearestTick);

    // And it is still a BOUND. One microsecond past a whole spacing is a miss, reported by name.
    landing = SeekLanding::NearestTick;
    const char* bad = SeekLandingRefusalFor(&t, target, target + captureSpacingUs + 1, &landing);
    REQUIRE(bad != nullptr);
    CHECK(std::string(bad) == "landed-off-target");
    CHECK(landing == SeekLanding::OffTarget);

    // Backwards is the same distance.
    landing = SeekLanding::OffTarget;
    CHECK(SeekLandingRefusalFor(&t, target, target - 30000, &landing) == nullptr);
    CHECK(landing == SeekLanding::NearestTick);

    SUBCASE("a capture that publishes no cadence is could-not-ask, never a verdict") {
        ToleranceTransport quiet;   // spacingUs stays 0
        int64_t unwritten = -12345;
        CHECK(SeekToleranceUs(&quiet, &unwritten) == SeekTolerance::Unknown);
        CHECK(unwritten == -12345);   // untouched on a refusal

        SeekLanding verdict = SeekLanding::OnTarget;
        const char* why = SeekLandingRefusalFor(&quiet, target, landedUs, &verdict);
        REQUIRE(why != nullptr);
        CHECK(std::string(why).find("could-not-ask") == 0);
        // Not a landing verdict: the out-parameter is left as the caller set it, so no arm can
        // print `on-target` or `landed-off-target` off a cadence nobody published.
        CHECK(verdict == SeekLanding::OnTarget);
    }

    SUBCASE("no transport at all is could-not-ask, not a crash and not a pass") {
        SeekLanding verdict = SeekLanding::OnTarget;
        CHECK(SeekLandingRefusalFor(nullptr, target, landedUs, &verdict) != nullptr);
        CHECK(verdict == SeekLanding::OnTarget);
    }
}

TEST_CASE("`seek <pct>` resolves a time, and no range is a refusal and not 0") {
    // ui::SeekFraction ends on a void SeekTo and returns true whatever the transport did, so the
    // percentage arm could not tell a landing from a no-op -- it answered "seek to 50%" either
    // way. The arithmetic is here, where the offline suite can sweep it.
    int64_t land = -1;
    REQUIRE(ResolvePercentSeekUs(50.0, 4000000, &land) == PercentSeek::Resolved);
    CHECK(land == 2000000);
    REQUIRE(ResolvePercentSeekUs(0.0, 4000000, &land) == PercentSeek::Resolved);
    CHECK(land == 0);
    REQUIRE(ResolvePercentSeekUs(100.0, 4000000, &land) == PercentSeek::Resolved);
    CHECK(land == 4000000);
    // Out of range clamps to the file, the rule `seek +<t>` already follows.
    REQUIRE(ResolvePercentSeekUs(140.0, 4000000, &land) == PercentSeek::Resolved);
    CHECK(land == 4000000);
    REQUIRE(ResolvePercentSeekUs(-3.0, 4000000, &land) == PercentSeek::Resolved);
    CHECK(land == 0);

    // A percentage of a span nobody knows is NOT the start of the file. `seek 0` is the command
    // that means the start, and nothing is written on the refusal.
    land = -1;
    CHECK(ResolvePercentSeekUs(50.0, 0, &land) == PercentSeek::NoRange);
    CHECK(ResolvePercentSeekUs(50.0, -1, &land) == PercentSeek::NoRange);
    CHECK(land == -1);

    // And the arm: the dispatcher no longer calls ui::SeekFraction at all -- it resolves the time
    // here and finishes on the absolute arm's Seek, read-back and classification.
    const std::string dispatch = CodeOnly(ReadRepoFile("src/control/dispatch.cpp"));
    CHECK(dispatch.find("SeekFraction") == std::string::npos);
    CHECK(dispatch.find("ResolvePercentSeekUs(") != std::string::npos);
    CHECK(dispatch.find("PercentSeek::NoRange") != std::string::npos);
}

TEST_CASE("W2-E finding: BuildFacing and DecomposeFacing do not round-trip a signed pitch") {
    // NOT this packet's code and NOT asserted as correct. ARCH-19 E2 publishes the two as a pair --
    // "the inverse" -- and every surface that prints or commands a facing goes through them, so a
    // pitch that comes back with the opposite sign means `cam look 0 -10` and the sentence
    // reporting where it landed disagree about which way is up. The yaw round-trips; the pitch is
    // negated. Recorded here as a MEASUREMENT, with the numbers, so W2-E has a repro and no reader
    // of this suite has to rediscover it -- pinning the observed sign as expected would be
    // codifying the defect.
    float quat[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    for (float want : {-30.0f, -6.0f, 0.0f, 6.0f, 30.0f}) {
        replay::camera_control::BuildFacing(41.25f, want, quat);
        REQUIRE(replay::camera_control::DecomposeFacing(quat, &yaw, &pitch));
        MESSAGE("BuildFacing(41.25, " << want << ") decomposes to yaw " << yaw << " pitch "
                                      << pitch);
        CHECK(yaw == doctest::Approx(41.25f).epsilon(0.001));
        // The magnitude survives; the sign is the open question. This assertion holds under EITHER
        // sign convention, so it does not take a side, and the MESSAGE above carries the evidence.
        CHECK(fabsf(pitch) == doctest::Approx(fabsf(want)).epsilon(0.001));
    }
}

TEST_CASE("W2-G: `cam look at <n>` aims at the subject and the sentence names who it landed on") {
    using replay::camera_control::BuildFacing;
    using replay::camera_control::DecomposeFacing;
    using replay::camera_control::FacingToward;

    // The camera stands at `from`; player 7's body centre is at `to`. The aim is asked of
    // camera_control::FacingToward -- the one direction body -- rather than recomputed here, so
    // this fixture cannot pass by agreeing with a second copy of the arithmetic.
    const float from[3] = {2.0f, 3.0f, -4.0f};
    const float to[3]   = {12.5f, 4.25f, 6.0f};
    float       yawWant = 0.0f;
    float       pitchWant = 0.0f;
    REQUIRE(FacingToward(from, to, &yawWant, &pitchWant));

    // ...and those two angles really do point AT the subject. ARCH-19 R5's convention says
    // BuildFacing(yaw, pitch) faces (sin y cos p, sin p, cos y cos p); that vector must be the
    // normalised (to - from). Without this the case would only prove the formatter can print
    // whatever FacingToward returned, whatever that was.
    const float rad = 3.14159265358979323846f / 180.0f;
    const float fwd[3] = {sinf(yawWant * rad) * cosf(pitchWant * rad), sinf(pitchWant * rad),
                          cosf(yawWant * rad) * cosf(pitchWant * rad)};
    const float d[3]   = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float len    = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) {
        CHECK(fwd[i] == doctest::Approx(d[i] / len).epsilon(1e-4));
    }

    float quat[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    BuildFacing(yawWant, pitchWant, quat);

    RecordingCam cam;
    memcpy(cam.landedPose.pos, from, sizeof from);
    memcpy(cam.landedPose.quat, quat, sizeof quat);
    cam.landedPose.source = replay::ViewPoseSource::ViewState;

    const CamPoseReply r = DriveCamPose(&cam, "cam look at 7", Ok(L"cam look at 7"));
    CHECK(cam.commandPoseCalls == 1);
    CHECK(cam.releasePoseCalls == 0);            // the wrong-function mutant
    CHECK(cam.lastCommanded.lookAt);
    CHECK(cam.lastCommanded.absolute);           // ARCH-19 R6's true/true row
    CHECK_FALSE(cam.lastCommanded.setPosition);
    CHECK(cam.lastCommanded.lookAtSelector == 7);  // the selector off-by-one mutant
    CHECK(r.ok);
    CHECK(r.outcome == CamPoseOutcome::Landed);
    CHECK_MESSAGE(r.text.find("aimed at player 7") != std::string::npos, r.text);

    // The angles in the sentence are the LANDED facing decomposed by the one pair, not the aim we
    // asked for -- and here the two agree to 1e-4 because this fake's engine landed on it exactly.
    float yawGot   = 0.0f;
    float pitchGot = 0.0f;
    REQUIRE(DecomposeFacing(cam.landedPose.quat, &yawGot, &pitchGot));
    CHECK(yawGot == doctest::Approx(yawWant).epsilon(1e-4));
    CHECK(pitchGot == doctest::Approx(pitchWant).epsilon(1e-4));
    char want[96];
    snprintf(want, sizeof want, "yaw %.1f pitch %.1f", yawGot, pitchGot);
    const std::string wantMsg = r.text + " -- wanted " + want;
    CHECK_MESSAGE(r.text.find(want) != std::string::npos, wantMsg);
    CHECK(r.text.find("from (2.000, 3.000, -4.000)") != std::string::npos);
    CHECK(r.text.find("regime commanded") != std::string::npos);
    CHECK(r.text.find("source view-state") != std::string::npos);

    // Every selector the parser admits reaches CommandPose as itself and is named as itself. A
    // shift of one anywhere between the token and the interface reds twice over.
    for (int64_t n = kCamPlayerSelectorMin; n <= kCamPlayerSelectorMax; ++n) {
        const std::wstring line = L"cam look at " + std::to_wstring(n);
        const std::string  verb = "cam look at " + std::to_string(n);
        RecordingCam       c2;
        memcpy(c2.landedPose.quat, quat, sizeof quat);
        const CamPoseReply rn = DriveCamPose(&c2, verb.c_str(), Ok(line.c_str()));
        CHECK(c2.lastCommanded.lookAtSelector == static_cast<int32_t>(n));
        const std::string who = "aimed at player " + std::to_string(n);
        CHECK_MESSAGE(rn.text.find(who) != std::string::npos, rn.text);
    }

    // Straight up at the subject is a real aim with no yaw. It says so; it never says yaw 0, which
    // is a different pose (ARCH-19 E3, and DecomposeFacing leaves its outputs untouched for it).
    RecordingCam vert;
    BuildFacing(0.0f, 90.0f, vert.landedPose.quat);
    const CamPoseReply rv = DriveCamPose(&vert, "cam look at 4", Ok(L"cam look at 4"));
    CHECK(rv.ok);
    CHECK_MESSAGE(rv.text.find("aimed at player 4") != std::string::npos, rv.text);
    CHECK_MESSAGE(rv.text.find("facing vertical") != std::string::npos, rv.text);
    CHECK(rv.text.find("yaw") == std::string::npos);
    CHECK(rv.text.find("pitch") == std::string::npos);
}

TEST_CASE("W2-G: every way `cam look at` fails is a named VALUE, and no-seam is today's answer") {
    using replay::camera_control::SubjectResolve;
    using replay::camera_control::SubjectResolveName;

    // Each SubjectResolve reaches this layer as ONE ControlRefusal word carrying its OWN sentence,
    // and both halves have to survive: the word is what a P17-G row scores, the sentence is what
    // tells the operator which of five different things went wrong.
    std::string previous;
    for (SubjectResolve sr : {SubjectResolve::NoSeam, SubjectResolve::RosterUnreadable,
                              SubjectResolve::NoSuchSubject, SubjectResolve::NoBody,
                              SubjectResolve::DerivationUncertified}) {
        RecordingCam cam;
        cam.acceptCommand = false;
        cam.refusalWord   = replay::ControlRefusal::LookAtUnresolvable;
        cam.diagnostic    = std::string("cam look at 3 refused: ") + SubjectResolveName(sr);
        const CamPoseReply r = DriveCamPose(&cam, "cam look at 3", Ok(L"cam look at 3"));
        CHECK(cam.commandPoseCalls == 1);
        CHECK(cam.releasePoseCalls == 0);
        CHECK_FALSE(r.ok);                           // the refusal-swallowed-as-ok mutant
        CHECK(r.outcome == CamPoseOutcome::Refused);
        CHECK_MESSAGE(r.text.find("look-at-unresolvable") != std::string::npos, r.text);
        CHECK_MESSAGE(r.text.find(SubjectResolveName(sr)) != std::string::npos, r.text);
        // A refused command never read the engine back: nothing here can be scored as a landing.
        CHECK(r.text.find("aimed at player") == std::string::npos);
        CHECK(r.text.find("regime") == std::string::npos);
        CHECK(r.text.find("yaw") == std::string::npos);
        // Five distinct sentences, so the assertion above is not five copies of one string.
        CHECK(r.text != previous);
        previous = r.text;
    }

    // THE WORD COMES FROM THE VALUE, NEVER FROM THE SENTENCE. Two refusals sharing one diagnostic
    // must still print different words, and a diagnostic that names every wrong word must not
    // change the right one. A dispatcher that keyed on the sentence fails both halves.
    RecordingCam a;
    RecordingCam b;
    a.acceptCommand = false;
    b.acceptCommand = false;
    a.diagnostic    = "the same sentence for both";
    b.diagnostic    = "the same sentence for both";
    a.refusalWord   = replay::ControlRefusal::LookAtUnresolvable;
    b.refusalWord   = replay::ControlRefusal::FacingDegenerate;
    const std::string ta = DriveCamPose(&a, "cam look at 3", Ok(L"cam look at 3")).text;
    const std::string tb = DriveCamPose(&b, "cam look at 3", Ok(L"cam look at 3")).text;
    CHECK(ta != tb);
    CHECK(ta.find("look-at-unresolvable") != std::string::npos);
    CHECK(tb.find("facing-degenerate") != std::string::npos);
    CHECK(tb.find("look-at-unresolvable") == std::string::npos);

    RecordingCam lying;
    lying.acceptCommand = false;
    lying.refusalWord   = replay::ControlRefusal::LookAtUnresolvable;
    lying.diagnostic    = "facing-degenerate, pose-not-finite, resolved, ok";
    const std::string lt = DriveCamPose(&lying, "cam look at 3", Ok(L"cam look at 3")).text;
    CHECK_MESSAGE(lt.find("look-at-unresolvable") != std::string::npos, lt);

    // TODAY'S LIVE ANSWER, PINNED. W1-E has not installed the subject-pose composer, so the
    // shipping image resolves nothing and every `cam look at <n>` refuses with NoSeam's sentence.
    // That is the EXPECTED live result, not a defect -- and this assertion is what tells the next
    // agent the seam arrived, instead of leaving a stale expectation to be discovered by a probe.
    CHECK_MESSAGE(replay::camera_control::InstalledSubjectPose() == nullptr,
                  "W1-E's composer is installed -- the no-seam expectation here is now stale");
    CHECK(std::string(SubjectResolveName(SubjectResolve::NoSeam)) ==
          "no subject-pose seam in this image");
}

TEST_CASE("W2-G-32a: the cam flight label reaches its own arm, and that arm takes no clock") {
    const std::string dispatch = CodeOnly(ReadRepoFile("src/control/dispatch.cpp"));
    const size_t label = dispatch.find("case CommandId::CamFlight:");
    REQUIRE(label != std::string::npos);
    CHECK(dispatch.substr(label, 70).find("RunCamFlight(cmd)") != std::string::npos);

    const size_t arm = dispatch.find("Result RunCamFlight(");
    REQUIRE(arm != std::string::npos);
    const size_t next = dispatch.find("Result RunCamRelease(", arm);
    REQUIRE(next != std::string::npos);
    const std::string body = dispatch.substr(arm, next - arm);
    CHECK(body.size() > 200);

    size_t marks = 0;
    for (size_t at = body.find("MarkFlightWindow("); at != std::string::npos;
         at = body.find("MarkFlightWindow(", at + 1)) {
        ++marks;
    }
    CHECK(marks == 1);

    // The marker has to sit on the same clock as the pose samples, which is the camera module's.
    // A clock read in this arm would be a second timeline and the run would score the wrong stretch.
    for (const char* clock : {"GetTickCount", "QueryPerformanceCounter", "GetSystemTime",
                              "std::chrono"}) {
        CHECK_MESSAGE(body.find(clock) == std::string::npos, clock);
    }
}

// ---------------------------------------------------------------------------------------------
// DESIGN-UI-2 S7.2 -- `rate` and `scrub`
// ---------------------------------------------------------------------------------------------

TEST_CASE("rate takes a signed decimal and nothing else") {
    ParseResult r = Parse(L"rate -2");
    REQUIRE(r.ok);
    CHECK(r.cmd.id == CommandId::Rate);
    CHECK(r.cmd.rate == doctest::Approx(-2.0));

    CHECK(Parse(L"rate 0.25").ok);
    CHECK_FALSE(Parse(L"rate").ok);
    CHECK_FALSE(Parse(L"rate fast").ok);
    CHECK_FALSE(Parse(L"rate 1 2").ok);
    CHECK_FALSE(Parse(L"rate 1000").ok);       // the same sanity band `speed` refuses
    CHECK_FALSE(Parse(L"rate 1e3").ok);        // no exponent, for the reason `speed` has none
}

TEST_CASE("debug takes its subject and its state as words, and refuses anything else") {
    ParseResult on = Parse(L"debug panel on");
    REQUIRE(on.ok);
    CHECK(on.cmd.id == CommandId::Debug);
    CHECK(on.cmd.debugOn);

    ParseResult off = Parse(L"debug panel off");
    REQUIRE(off.ok);
    CHECK_FALSE(off.cmd.debugOn);

    CHECK_FALSE(Parse(L"debug").ok);
    CHECK_FALSE(Parse(L"debug panel").ok);
    CHECK_FALSE(Parse(L"debug panel yes").ok);
    CHECK_FALSE(Parse(L"debug bar on").ok);
    CHECK_FALSE(Parse(L"debug panel on off").ok);
}

TEST_CASE("bar takes on, off or toggle and nothing else") {
    ParseResult on = Parse(L"bar on");
    REQUIRE(on.ok);
    CHECK(on.cmd.id == CommandId::Bar);
    CHECK(on.cmd.barMode == Command::BarMode::On);

    CHECK(Parse(L"bar off").cmd.barMode == Command::BarMode::Off);
    CHECK(Parse(L"bar toggle").cmd.barMode == Command::BarMode::Toggle);

    CHECK_FALSE(Parse(L"bar").ok);
    CHECK_FALSE(Parse(L"bar yes").ok);
    CHECK_FALSE(Parse(L"bar on off").ok);
}

TEST_CASE("scrub carries its phase as a word and its target through the one time parser") {
    ParseResult begin = Parse(L"scrub begin");
    REQUIRE(begin.ok);
    CHECK(begin.cmd.id == CommandId::Scrub);
    CHECK(begin.cmd.scrubPhase == Command::ScrubPhase::Begin);
    CHECK_FALSE(begin.cmd.scrubHasTarget);

    ParseResult to = Parse(L"scrub to 1:23.5");
    REQUIRE(to.ok);
    CHECK(to.cmd.scrubPhase == Command::ScrubPhase::To);
    CHECK(to.cmd.scrubHasTarget);
    CHECK_FALSE(to.cmd.seekIsPercent);
    CHECK(to.cmd.whole == 83500000);

    ParseResult pct = Parse(L"scrub to 50%");
    REQUIRE(pct.ok);
    CHECK(pct.cmd.seekIsPercent);
    CHECK(pct.cmd.percent == doctest::Approx(50.0));

    ParseResult end = Parse(L"scrub end");
    REQUIRE(end.ok);
    CHECK(end.cmd.scrubPhase == Command::ScrubPhase::End);
    CHECK_FALSE(end.cmd.scrubHasTarget);

    ParseResult endAt = Parse(L"scrub end 75%");
    REQUIRE(endAt.ok);
    CHECK(endAt.cmd.scrubPhase == Command::ScrubPhase::End);
    CHECK(endAt.cmd.scrubHasTarget);

    // `begin` takes no argument, `to` requires one, and a relative form is refused outright: a
    // nudge into a slot that coalesces would compound however many times it survived a tick.
    CHECK_FALSE(Parse(L"scrub").ok);
    CHECK_FALSE(Parse(L"scrub begin 10").ok);
    CHECK_FALSE(Parse(L"scrub to").ok);
    CHECK_FALSE(Parse(L"scrub to +10").ok);
    CHECK_FALSE(Parse(L"scrub sideways").ok);
    CHECK_FALSE(Parse(L"scrub to 50% extra").ok);
    CHECK_FALSE(Parse(L"scrub to 500%").ok);
}

// POLISH-2. The player bar has a Reverse button and the console answered HTTP 400 for the word.
TEST_CASE("POLISH2: `reverse` is a verb, takes no argument, and leaves speed and rate alone") {
    CHECK(Ok(L"reverse").id == CommandId::Reverse);
    CHECK(Contains(Err(L"reverse -1"), "reverse"));      // it flips; it does not take a rate

    // The two verbs it must not have displaced.
    CHECK(Ok(L"speed -1").id == CommandId::Speed);
    CHECK(Ok(L"speed -1").rate == -1.0);
    CHECK(Ok(L"rate -1").id == CommandId::Rate);

    const CommandSpec* spec = SpecFor(CommandId::Reverse);
    REQUIRE(spec != nullptr);
    // The same needs as `rate`: a direction flip is a transport write against a loaded capture.
    CHECK((spec->needs & kNeedsTransport) != 0);
    CHECK((spec->needs & kNeedsLoaded) != 0);
}

// ---------------------------------------------------------------------------------------------
// MULTI-LOAD. The HTTP routes' identity, and the seek axis.
//
// The routes live on a listener embedded in CR15NetGame. A cross-level load rebuilds the session,
// and the registration used to latch on a bool -- so the four routes stayed on the dead object and
// the engine answered 404 for every one of our URIs while Running() still said true. Nothing in
// this tree touched http::Start or http::Running before this block existed, which is why it
// shipped.
//
// http.cpp itself needs the game (it resolves engine entry points out of the mapped image), so the
// part that can be wrong without a game -- the identity decision and the register/unregister
// bookkeeping -- lives in control/httpreg.h and is driven here against a fake listener.
// ---------------------------------------------------------------------------------------------

namespace {

int    g_fakeRegisterCalls = 0;
int    g_fakeUnregisterCalls = 0;
void*  g_fakeUnregisteredOn = nullptr;

// A fake route table per listener: register refuses a URI that is already on THIS listener, which
// is what the engine's own call does and is the whole reason a redundant Start is a no-op.
std::vector<std::pair<void*, std::string>> g_fakeRoutes;

void FakeHandler(void*, void*) {}

int FakeRegister(void* listener, const char* uri, void (*)(void*, void*), void*, unsigned) {
    ++g_fakeRegisterCalls;
    for (const auto& r : g_fakeRoutes) {
        if (r.first == listener && r.second == uri) return -1;
    }
    g_fakeRoutes.emplace_back(listener, std::string(uri));
    return 0;
}

int FakeUnregister(void* listener, const char* uri) {
    ++g_fakeUnregisterCalls;
    g_fakeUnregisteredOn = listener;
    for (size_t i = 0; i < g_fakeRoutes.size(); ++i) {
        if (g_fakeRoutes[i].first == listener && g_fakeRoutes[i].second == uri) {
            g_fakeRoutes.erase(g_fakeRoutes.begin() + static_cast<ptrdiff_t>(i));
            return 0;
        }
    }
    return -1;
}

size_t FakeRoutesOn(void* listener) {
    size_t n = 0;
    for (const auto& r : g_fakeRoutes) {
        if (r.first == listener) ++n;
    }
    return n;
}

void ResetFake() {
    g_fakeRegisterCalls = 0;
    g_fakeUnregisterCalls = 0;
    g_fakeUnregisteredOn = nullptr;
    g_fakeRoutes.clear();
}

const replay::control::http::Route kFakeRoutes[] = {
    {"/replay/status", &FakeHandler, 1},
    {"/replay/help", &FakeHandler, 1},
    {"/replay/result", &FakeHandler, 1},
    {"/replay/command", &FakeHandler, 2},
};
const char* const kFakeUris[] = {"/replay/status", "/replay/help", "/replay/result",
                                 "/replay/command"};

}  // namespace

TEST_CASE("MULTI-LOAD: NeedsReregister is true exactly when the identity moved") {
    using replay::control::http::NeedsReregister;
    // Four addresses distinct BY CONSTRUCTION. Four `int` locals are distinct too, but a pure
    // inline function over addresses that never escape is exactly the shape an optimiser may fold,
    // and a fixture that folds makes this case pass on nothing.
    char space[4] = {};
    const void* const netA = &space[0];
    const void* const netB = &space[1];
    const void* const httpA = &space[2];
    const void* const httpB = &space[3];

    // Not running yet, on a live session: register.
    CHECK(NeedsReregister(false, nullptr, nullptr, netA, httpA));
    // The same pair we registered against: nothing to do, every tick, forever.
    CHECK_FALSE(NeedsReregister(true, netA, httpA, netA, httpA));
    // A new CR15NetGame -- the ordinary cross-level rebuild.
    CHECK(NeedsReregister(true, netA, httpA, netB, httpB));
    // THE CASE A POINTER COMPARE MISSES, and csregister was bitten by it first: the engine rebuilt
    // under the SAME pointer, so only the embedded evhttp names the new object.
    CHECK(NeedsReregister(true, netA, httpA, netA, httpB));
    // The listener went away under a live netGame: re-register, which fails politely and takes the
    // panel's httpUp down with it. Claiming to be up is the defect, not failing to register.
    CHECK(NeedsReregister(true, netA, httpA, netA, nullptr));
    // No session at all: there is nothing to register on and nothing to report.
    CHECK_FALSE(NeedsReregister(false, nullptr, nullptr, nullptr, nullptr));
    CHECK_FALSE(NeedsReregister(true, netA, httpA, nullptr, nullptr));
}

TEST_CASE("MULTI-LOAD: a rebuilt session re-registers on the NEW listener and never touches the old") {
    using namespace replay::control::http;
    ResetFake();

    char space[6] = {};
    void* const netA = &space[0];
    void* const netB = &space[1];
    void* const listenerA = &space[2];   // the listener is embedded in the netGame; a distinct
    void* const listenerB = &space[3];   // address is all this bookkeeping needs
    void* const httpA = &space[4];
    void* const httpB = &space[5];

    Registration reg;
    size_t already = 0;
    CHECK(Apply(&reg, &FakeRegister, listenerA, netA, httpA, kFakeRoutes, 4, &already) == 4);
    CHECK(already == 0);
    CHECK(FakeRoutesOn(listenerA) == 4);
    CHECK(reg.running);

    // A second Start on the same live session is a no-op: the decision refuses before a single
    // engine call is made.
    CHECK_FALSE(NeedsReregister(reg.running, reg.netGame, reg.evhttp, netA, httpA));
    const int callsBefore = g_fakeRegisterCalls;

    // ---- the cross-level load: a new CR15NetGame, and the old one is gone ----
    CHECK(NeedsReregister(reg.running, reg.netGame, reg.evhttp, netB, httpB));
    DropLatch(&reg);
    CHECK_FALSE(reg.running);
    // NOT ONE UNREGISTER AGAINST THE OLD LISTENER. That object may be freed; the call would be
    // into dead memory, and it is the second half of this defect.
    CHECK(g_fakeUnregisterCalls == 0);
    CHECK(g_fakeRegisterCalls == callsBefore);

    CHECK(Apply(&reg, &FakeRegister, listenerB, netB, httpB, kFakeRoutes, 4, &already) == 4);
    CHECK(already == 0);          // a fresh table takes every route: positive proof it was rebuilt
    CHECK(FakeRoutesOn(listenerB) == 4);
    CHECK(FakeRoutesOn(listenerA) == 4);   // untouched, not unregistered
    CHECK(reg.netGame == netB);

    // ---- shutdown hands back only what it owns ----
    CHECK(Release(&reg, &FakeUnregister, netB, httpB, kFakeUris, 4));
    CHECK(FakeRoutesOn(listenerB) == 0);
    CHECK(g_fakeUnregisteredOn == listenerB);
    CHECK_FALSE(reg.running);
}

TEST_CASE("MULTI-LOAD: Stop refuses to unregister through a listener that is not ours") {
    using namespace replay::control::http;
    ResetFake();
    char space[5] = {};
    void* const netA = &space[0];
    void* const netB = &space[1];
    void* const httpA = &space[2];
    void* const httpB = &space[3];
    void* const listenerA = &space[4];

    Registration reg;
    size_t already = 0;
    REQUIRE(Apply(&reg, &FakeRegister, listenerA, netA, httpA, kFakeRoutes, 4, &already) == 4);

    // Shutdown after a rebuild nobody noticed: the live session is B, the record says A.
    CHECK_FALSE(CanUnregister(reg, netB, httpB));
    CHECK_FALSE(Release(&reg, &FakeUnregister, netB, httpB, kFakeUris, 4));
    CHECK(g_fakeUnregisterCalls == 0);
    CHECK(g_fakeUnregisteredOn == nullptr);
    // The record is dropped either way: a registration nobody can prove is not one to keep.
    CHECK_FALSE(reg.running);
    CHECK(reg.listener == nullptr);

    // A redundant Apply over a table that still holds the routes registers nothing and says so,
    // which is what makes a confirming re-register safe.
    Registration again;
    CHECK(Apply(&again, &FakeRegister, listenerA, netA, httpA, kFakeRoutes, 4, &already) == 0);
    CHECK(already == 4);
    CHECK_FALSE(again.running);
}

TEST_CASE("MULTI-LOAD: the tick asks whether it is registered on THIS netGame") {
    // control_module.cpp needs a game and cannot be linked here, so its wiring is read out of the
    // tree -- the same mechanism the S9 case above uses. Three things have to be true together:
    // the gate is identity-keyed, the latch is dropped without unregistering, and the panel is
    // told the surface is down. Dropping any one of them is a live defect nothing else would see.
    const std::string src = CodeOnly(ReadRepoFile("src/control/control_module.cpp"));
    CHECK(Contains(src, "!http::RegisteredOn(info.netGame)"));
    CHECK(Contains(src, "http::Invalidate("));
    CHECK(Contains(src, "dispatch::SetHttpState(false, http::Diagnostic())"));
    // The old gate, which is the defect: registered at all, rather than registered on this one.
    CHECK_FALSE(Contains(src, "!http::Running() && info.netGame"));
    // Shutdown hands the live netGame to Stop so it can refuse a listener that is not ours.
    CHECK(Contains(src, "http::Stop(g_lastNetGame)"));
}

// ---------------------------------------------------------------------------------------------
// MULTI-LOAD. `seek <t>` is the PLAYABLE SPAN, and `seekfile` is the file.
//
// `seek 19`, `seek 42` and `seek 21.5` all landed on 0:00.000 on the owner's reference capture,
// whose content starts 822.9 s into its own axis: every one of them was before the content and
// clamped (LAUNCH-LIVEFIX 4.3). The bar, the readout and `seek @t` were already on the span.
// ---------------------------------------------------------------------------------------------

TEST_CASE("MULTI-LOAD: bare `seek` is span-relative and `seekfile` is the whole file") {
    // Bare seconds: the span, without the owner having to type an @.
    CHECK(Ok(L"seek 19").seekIsSpanRelative);
    CHECK(Ok(L"seek 19").whole == 19 * 1000000);
    CHECK(Ok(L"seek 1:23.5").seekIsSpanRelative);
    // A percentage under `seek` is a percentage OF THE SPAN, which is what the bar's own click
    // mapping has always been: two surfaces that disagreed about 50% is the defect.
    CHECK(Ok(L"seek 50%").seekIsSpanRelative);
    CHECK(Ok(L"seek 50%").seekIsPercent);
    CHECK(Ok(L"seek 50%").percent == 50.0);
    // The explicit spelling still means exactly the same thing.
    CHECK(Ok(L"seek @12.5").seekIsSpanRelative);
    CHECK(Ok(L"seek @12.5").whole == 12500000);

    // A nudge has no axis: it is a delta from the playhead, so it must not take the span arm --
    // which resolves an absolute time and would turn `seek +10` into "10 s into the content".
    CHECK(Ok(L"seek +10").seekIsRelative);
    CHECK_FALSE(Ok(L"seek +10").seekIsSpanRelative);
    CHECK(Ok(L"seek -1:30").seekIsRelative);
    CHECK_FALSE(Ok(L"seek -1:30").seekIsSpanRelative);

    // The named form for the old behaviour, for anything written against the file axis.
    CHECK(Ok(L"seekfile 19").id == CommandId::SeekFile);
    CHECK_FALSE(Ok(L"seekfile 19").seekIsSpanRelative);
    CHECK(Ok(L"seekfile 19").whole == 19 * 1000000);
    CHECK(Ok(L"seekfile 40%").seekIsPercent);
    CHECK_FALSE(Ok(L"seekfile 40%").seekIsSpanRelative);
    CHECK(Ok(L"seekfile +10").seekIsRelative);
    CHECK_FALSE(Ok(L"seekfile +10").seekIsSpanRelative);

    // `seekfile @t` is a contradiction and is refused by name rather than silently picking an axis.
    CHECK_FALSE(Parse(L"seekfile @12.5").ok);
    CHECK(Contains(Err(L"seekfile @12.5"), "whole-file axis"));

    const CommandSpec* spec = SpecFor(CommandId::SeekFile);
    REQUIRE(spec != nullptr);
    CHECK((spec->needs & kNeedsTransport) != 0);
    CHECK((spec->needs & kNeedsLoaded) != 0);
    // The help has to name the axis, because the axis is the whole difference between the two.
    CHECK(Contains(std::string(SpecFor(CommandId::Seek)->help), "PLAYABLE SPAN"));
    CHECK(Contains(std::string(spec->help), "WHOLE FILE"));
}

// BACKLOG-1. The recorder surface's Record/Stop buttons reach the recorder through THIS module's
// command layer and nothing else. The wiring lives in control_module.cpp, which is on the shipped
// DLL and cannot be linked here (tests/control/CMakeLists.txt), so it is asserted from the source
// the way the S9 case above asserts dispatch's -- and the arm it registers into, `record`, is
// driven for real by the CommandId::Record cases in this file.
TEST_CASE("the ui command sink is registered from control's init and cleared on shutdown") {
    const std::string module = CodeOnly(ReadRepoFile("src/control/control_module.cpp"));
    REQUIRE(module.size() > 500);   // the scan read a file, so a zero below means something

    // Registered, cleared, and pointed at a function that goes through the parser and the queue.
    CHECK(module.find("ui::SetCommandSink(&SubmitUiCommand)") != std::string::npos);
    CHECK(module.find("ui::SetCommandSink(nullptr)") != std::string::npos);
    const size_t sink = module.find("bool SubmitUiCommand(");
    REQUIRE(sink != std::string::npos);
    const std::string body = module.substr(sink, 900);
    CHECK(body.find("Parse(line)") != std::string::npos);
    CHECK(body.find("dispatch::Submit(") != std::string::npos);

    // ★ AND IT REACHES THE RECORDER THROUGH NOTHING ELSE. A sink that called
    // RequestManualRecordStart itself would skip the session gate, the "already recording" refusal
    // and the status line -- which is the whole reason the seam is command text.
    CHECK(module.find("RequestManualRecord") == std::string::npos);
    CHECK(module.find("CaptureIsRecording") == std::string::npos);

    // The command text the UI issues is a command this build parses.
    CHECK(Ok(L"record start").id == CommandId::Record);
    CHECK(Ok(L"record start").recordVerb == Command::RecordVerb::Start);
    CHECK(Ok(L"record stop").recordVerb == Command::RecordVerb::Stop);
}

TEST_CASE("AUDIO-FIX-2: `audio writes` parses its three arms and refuses everything else") {
    CHECK(Ok(L"audio writes on").id == CommandId::AudioWrites);
    CHECK(Ok(L"audio writes on").audioWritesVerb == Command::AudioWritesVerb::On);
    CHECK(Ok(L"audio writes off").audioWritesVerb == Command::AudioWritesVerb::Off);
    CHECK(Ok(L"audio writes status").audioWritesVerb == Command::AudioWritesVerb::Status);
    // `audio` alone is not a toggle, and neither is `audio off` -- a verb whose family word is
    // optional would turn a typo into a silent arm change in the middle of a measurement.
    CHECK(Contains(Err(L"audio"), "writes"));
    CHECK(Contains(Err(L"audio off"), "writes"));
    CHECK(Contains(Err(L"audio writes"), "on, off or status"));
    CHECK(!Err(L"audio writes on off").empty());
}

// =================================================================================================
// The four control routes ride the GAME'S OWN HTTP listener, and
// POST /replay/command is unauthenticated: in a live match `record stop` ends the owner's recording
// of it and `list` enumerates his replay folder. Outside contained offline playback they are not
// registered at all unless control.http_routes_live says so.
// =================================================================================================

#include "bootstrap/livesafety.h"

TEST_CASE("the HTTP route gate: contained offline playback, or the owner said so, or nothing") {
    using replay::livesafety::HttpRoutesAllowed;

    // A live/online session. The routes are not the plugin's socket -- they are four callbacks on
    // ECHOVR'S listener, one of them an unauthenticated POST that can stop the owner's recording of
    // the match he is playing and list his replay folder.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/true, /*offlineLockHeld=*/false);
    CHECK_FALSE(HttpRoutesAllowed(false));
    CHECK(HttpRoutesAllowed(true));   // control.http_routes_live, and only that

    // The offline lock not held is the other non-contained shape, and it refuses the same way.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/false);
    CHECK_FALSE(HttpRoutesAllowed(false));

    // Contained offline playback: up without the key, which is what the launch agents drive.
    replay::livesafety::SetForTest(/*liveOnlineExempt=*/false, /*offlineLockHeld=*/true);
    CHECK(HttpRoutesAllowed(false));
    CHECK(HttpRoutesAllowed(true));
    replay::livesafety::ResetForTest();
}

// http.cpp is on the echoreplay target and cannot be linked here -- the same situation P17-E's S9
// case is in, and asserted the same way: from the source. What the case above proves is the
// DECISION; what this one proves is that the one door into route registration asks it, before it
// touches the listener.
TEST_CASE("S9: http::Start asks the route gate before it reaches the engine's listener") {
    const std::string src = std::string(REPLAY_REPO_ROOT) + "/src/control/http.cpp";
    FILE* f = fopen(src.c_str(), "rb");
    REQUIRE_MESSAGE(f != nullptr, src);
    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);

    const size_t start = text.find("bool Start(void* netGame) {");
    REQUIRE(start != std::string::npos);
    const size_t gate = text.find("livesafety::HttpRoutesAllowed(g_liveAllowed)", start);
    const size_t listener = text.find("facts::HTTP_LISTENER_OFF", start);
    const size_t apply = text.find("g_fn.routeRegister", start);
    REQUIRE(gate != std::string::npos);
    REQUIRE(listener != std::string::npos);
    REQUIRE(apply != std::string::npos);
    CHECK(gate < listener);
    CHECK(gate < apply);

    // And the key it reads is declared, default FALSE. A gate whose override defaults on is not one.
    const std::string keys = std::string(REPLAY_REPO_ROOT) + "/src/bootstrap/config_keys.inc";
    FILE* kf = fopen(keys.c_str(), "rb");
    REQUIRE(kf != nullptr);
    std::string ktext;
    while ((n = fread(buf, 1, sizeof buf, kf)) > 0) ktext.append(buf, n);
    fclose(kf);
    CHECK(ktext.find("REPLAY_CONFIG_BOOL(\"control.http_routes_live\", false,") != std::string::npos);
}
