// The config parser reads a file a user edits by hand, so the interesting cases are all the ways a hand
// edit goes wrong - and the requirement that a broken file falls back to declared defaults rather than to
// half a configuration.

#include <windows.h>

#include <cstring>
#include <iterator>
#include <set>
#include <fstream>
#include <string>
#include <vector>

#include <doctest.h>

#include "bootstrap/config.h"
#include "capture/capture_types.h"
#include "bootstrap/log.h"
#include "bootstrap/sc7.h"

namespace {

// The declared default for a bool row, read out of config_keys.inc itself. A case that spells a
// default in its own source is a SECOND declaration of it, which is exactly the drift the table
// exists to stop: W2-F-C3 flipped `ui.gate_scope` to true and the case below went on asserting the
// literal `false`, so it reddened at 258c0e8 instead of following the flip.
// Returns -1 for a key the table does not declare -- an undeclared key is a broken table, not a
// false default, and the caller REQUIREs against that rather than reading it as `off`.
int DeclaredBoolDefault(const char* key) {
#define REPLAY_CONFIG_BOOL(K, D, ROW, STREAM, F, DOC) \
    if (std::strcmp(K, key) == 0) return (D) ? 1 : 0;
#define REPLAY_CONFIG_INT(K, D, ROW, STREAM, F, DOC)
#define REPLAY_CONFIG_DOUBLE(K, D, ROW, STREAM, F, DOC)
#define REPLAY_CONFIG_STRING(K, D, ROW, STREAM, F, DOC)
#include "bootstrap/config_keys.inc"
#undef REPLAY_CONFIG_BOOL
#undef REPLAY_CONFIG_INT
#undef REPLAY_CONFIG_DOUBLE
#undef REPLAY_CONFIG_STRING
    return -1;
}

std::wstring LedgerTempLog() {
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir, n) + L"replay-w1c-ledger-order.log";
}

std::string ReadAllTextForTest(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    std::string body;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof buffer, &read, nullptr) && read != 0) {
        body.append(buffer, read);
    }
    CloseHandle(file);
    return body;
}

struct Doc {
    replay::ConfigDocument doc;
    char                   error[256] = {};

    bool Parse(const std::string& text) {
        return doc.Parse(text.data(), text.size(), error, sizeof error);
    }
};

}  // namespace

TEST_CASE("nested objects and dotted keys are the same document") {
    Doc flat;
    REQUIRE(flat.Parse(R"({"capture.voip": false, "log.level": "trace"})"));
    Doc nested;
    REQUIRE(nested.Parse(R"({"capture": {"voip": false}, "log": {"level": "trace"}})"));

    for (Doc* d : {&flat, &nested}) {
        const replay::ConfigValue* voip = d->doc.Find("capture.voip");
        REQUIRE(voip != nullptr);
        CHECK(voip->type == replay::ConfigType::Bool);
        CHECK(voip->boolValue == false);
        const replay::ConfigValue* level = d->doc.Find("log.level");
        REQUIRE(level != nullptr);
        CHECK(std::string(level->stringValue) == "trace");
        CHECK(d->doc.Count() == 2);
    }
}

TEST_CASE("values keep their type, and numbers keep their exactness") {
    Doc d;
    REQUIRE(d.Parse(R"({"a": 250, "b": -9007199254740993, "c": 0.5, "d": 1e3, "e": "s", "f": true})"));
    CHECK(d.doc.Find("a")->type == replay::ConfigType::Int);
    CHECK(d.doc.Find("a")->intValue == 250);
    // Beyond a double's exact integer range: an int routed through a double comes back changed.
    CHECK(d.doc.Find("b")->type == replay::ConfigType::Int);
    CHECK(d.doc.Find("b")->intValue == -9007199254740993ll);
    CHECK(d.doc.Find("c")->type == replay::ConfigType::Double);
    CHECK(d.doc.Find("c")->doubleValue == doctest::Approx(0.5));
    CHECK(d.doc.Find("d")->type == replay::ConfigType::Double);
    CHECK(d.doc.Find("d")->doubleValue == doctest::Approx(1000.0));
    CHECK(d.doc.Find("f")->boolValue == true);
    CHECK(d.doc.Find("nope") == nullptr);
}

TEST_CASE("comments, trailing commas and null are accepted the way a hand-edited file uses them") {
    Doc d;
    REQUIRE(d.Parse("{\n"
                    "  // the whole point of allowing these\n"
                    "  \"capture.voip\": false,  // trailing\n"
                    "  /* and the block form */\n"
                    "  \"ui.panel\": true,\n"
                    "}"));
    CHECK(d.doc.Count() == 2);

    Doc empty;
    REQUIRE(empty.Parse(""));
    CHECK(empty.doc.Count() == 0);

    // null is "no opinion": the key is not recorded and the declared default stands.
    Doc nulled;
    REQUIRE(nulled.Parse(R"({"log.level": null, "capture.voip": false})"));
    CHECK(nulled.doc.Find("log.level") == nullptr);
    CHECK(nulled.doc.Count() == 1);
}

TEST_CASE("escapes decode, including surrogate pairs") {
    // \u escapes rather than literal UTF-8 bytes, so this file stays ASCII and the test proves the
    // decoder instead of proving the compiler guessed the file's codepage. Ordinary string literals
    // rather than raw ones, because MSVC still expands a universal-character-name inside R"(...)".
    Doc d;
    REQUIRE(d.Parse("{\"a\": \"tab\\there\", \"b\": \"\\u00e9\", "
                    "\"c\": \"\\ud83c\\udfae\", \"d\": \"\\\\ \\\" \\/\"}"));
    CHECK(std::string(d.doc.Find("a")->stringValue) == "tab\there");
    CHECK(std::string(d.doc.Find("b")->stringValue) == "\xC3\xA9");
    CHECK(std::string(d.doc.Find("c")->stringValue) == "\xF0\x9F\x8E\xAE");
    CHECK(std::string(d.doc.Find("d")->stringValue) == "\\ \" /");

    // A lone high surrogate with nothing valid behind it must not swallow the rest of the string.
    REQUIRE(d.Parse("{\"a\": \"\\ud83c!\"}"));
    CHECK(std::string(d.doc.Find("a")->stringValue).size() == 4);
}

TEST_CASE("a malformed document is refused whole, and says where") {
    // Each of these is a real hand-edit mistake. The requirement is the same for all of them: refuse,
    // explain, and leave the caller on declared defaults - never a document that parsed halfway.
    const char* broken[] = {
        R"({"a": )",                       // truncated
        R"({"a" 1})",                      // missing colon
        R"({"a": 1 "b": 2})",              // missing comma
        R"({a: 1})",                       // unquoted key
        R"({"a": tru})",                   // not a keyword
        R"({"a": "unterminated)",          // unterminated string
        R"({"a": [1, 2]})",                // arrays are not a shape this build understands
        R"({"a": 1} trailing)",            // content after the object
        R"([1, 2])",                       // not an object
        R"({"a": "\q"})",                  // unknown escape
        R"({"a": "\u00"})",                // short \u
        R"({"a": 1.2.3})",                 // malformed number
    };
    for (const char* text : broken) {
        Doc d;
        // Something is loaded first, so "refused whole" is a statement about discarding rather than about
        // a document that was empty to begin with.
        REQUIRE(d.Parse(R"({"seed": 1})"));
        REQUIRE(d.doc.Count() == 1);
        CAPTURE(text);
        CHECK_FALSE(d.Parse(text));
        CHECK(d.doc.Count() == 0);
        CHECK(std::strlen(d.error) > 0);
    }
}

TEST_CASE("nesting is bounded, so a hostile file cannot walk off the stack") {
    std::string deep;
    for (int i = 0; i < 200; ++i) {
        deep += "{\"a\":";
    }
    deep += "1";
    for (int i = 0; i < 200; ++i) {
        deep += "}";
    }
    Doc d;
    CHECK_FALSE(d.Parse(deep));
    CHECK(std::string(d.error).find("nested") != std::string::npos);

    std::string shallow;
    for (int i = 0; i < 8; ++i) {
        shallow += "{\"a\":";
    }
    shallow += "1";
    for (int i = 0; i < 8; ++i) {
        shallow += "}";
    }
    CHECK(d.Parse(shallow));
    CHECK(d.doc.Find("a.a.a.a.a.a.a.a") != nullptr);
}

TEST_CASE("a repeated key resolves to the last one rather than to whichever the parser saw first") {
    Doc d;
    REQUIRE(d.Parse(R"({"capture.voip": true, "capture": {"voip": false}})"));
    CHECK(d.doc.Count() == 1);
    CHECK(d.doc.Find("capture.voip")->boolValue == false);
}

TEST_CASE("the key registry has no duplicate key, ledger row or stream") {
    std::set<std::string> keys;
    std::set<std::string> rows;
    std::set<std::string> streams;
    for (size_t i = 0; i < replay::ConfigKeyCount(); ++i) {
        const replay::ConfigKey& k = replay::ConfigKeys()[i];
        CAPTURE(k.key);
        CHECK(keys.insert(k.key).second);
        CHECK(k.doc != nullptr);
        CHECK(std::strlen(k.doc) > 0);
        if (k.ledgerRow != nullptr) {
            CHECK(rows.insert(k.ledgerRow).second);
        }
        if (k.stream != nullptr) {
            CHECK(streams.insert(k.stream).second);
        }
        CHECK(replay::FindConfigKey(k.key) == &k);
    }
    CHECK(keys.size() == replay::ConfigKeyCount());
    CHECK(replay::FindConfigKey("no.such.key") == nullptr);
    CHECK(replay::FindConfigKey(nullptr) == nullptr);
}

TEST_CASE("reads come from the file, then the declared default, and an unknown key is the caller's") {
    replay::config::ResetForTest();
    const replay::IConfig* cfg = replay::config::Interface();

    // Nothing loaded: every declared key reads its declared default.
    CHECK(cfg->GetBool("capture.voip", true) == true);
    CHECK(cfg->GetBool("playback.render_suppress_hook", false) == false);
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "auto");
    CHECK(cfg->GetInt("log.drain_interval_ms", 250) == 250);

    // Has() answers "is this a key this build understands", which is the question a typo needs asked.
    CHECK(cfg->Has("capture.voip"));
    CHECK_FALSE(cfg->Has("capture.voipp"));

    // An undeclared key returns exactly what the caller asked for and does not invent a value.
    CHECK(cfg->GetInt("not.a.key", 41) == 41);
    CHECK(std::string(cfg->GetString("not.a.key", "fallback")) == "fallback");

    const std::string text = R"({"capture.voip": false, "tick.mode": "hook",
                                 "log.drain_interval_ms": 40, "not.declared.at.all": 1})";
    REQUIRE(replay::config::LoadFromTextForTest(text.data(), text.size()));
    CHECK(cfg->GetBool("capture.voip", true) == false);
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "hook");
    CHECK(cfg->GetInt("log.drain_interval_ms", 250) == 40);
    CHECK(replay::config::UnknownFileKeys() == 1);

    replay::config::ResetForTest();
}

// What a launch with no ECHOREPLAY_* variable set gets. The caller's default is the
// OPPOSITE of the expected answer on every line, so only the declared default can satisfy it.
TEST_CASE("the shipped defaults are the launch-verified body route, and nothing unverified") {
    replay::config::ResetForTest();
    const replay::IConfig* cfg = replay::config::Interface();

    CHECK(cfg->GetBool("pawn.bot_body", true) == false);
    CHECK(cfg->GetBool("pawn.equipment_recv", false) == true);
    CHECK(cfg->GetBool("session.gametype_override", false) == true);
    CHECK(std::string(cfg->GetString("session.gametype_override_name", "")) == "echo_arena");

    CHECK(cfg->GetBool("pawn.bot_publish_loadout", true) == false);
    CHECK(cfg->GetBool("pawn.participate_backstop", true) == false);
    // SHIP-DEFAULTS. These four were launch-verified only with ECHOREPLAY_* set by the launch
    // agent, and the owner launches through tools\watch_replay.ps1 with no variable at all -- so
    // shipping them off shipped him a viewer with no clock, no score and no disc. Each is inert
    // outside a contained process (netvarhud's `contained` argument, g.disc_drive_on's
    // ProcessContained(), and rows 2.8/2.9 resolving to `off` for a live match).
    CHECK(cfg->GetBool("playback.gamestate_clock", false) == true);
    CHECK(cfg->GetBool("playback.gamestate_score", false) == true);
    CHECK(cfg->GetBool("playback.host_netvar_filter", false) == true);
    CHECK(cfg->GetBool("playback.disc_drive", false) == true);
    // The gate is on whenever the drive is: a moving disc the host can score off is a bug.
    CHECK(cfg->GetBool("playback.disc_score_gate", false) == true);
    // The pre-roll ships on and now has to be steerable from a launch, because it is the thing a
    // launch suspects when the disc does not bind.
    CHECK(cfg->GetBool("playback.preroll", false) == true);
    // 65 s of host time (45 s pre-match + 20 s launch) at 2x is 32.5 s, and the admission grace can
    // hold the fast clock off for 8 s before it starts.
    CHECK(cfg->GetInt("playback.preroll_bound_ms", 1) == 55000);
    CHECK(cfg->GetDouble("playback.preroll_rate", 1.0) == 2.0);

    // The pair stays steerable from a launch without a rebuild.
    for (const char* key : {"pawn.bot_body", "pawn.equipment_recv", "session.gametype_override",
                            "playback.disc_drive", "playback.disc_score_gate",
                            "playback.gamestate_clock", "playback.gamestate_score",
                            "playback.host_netvar_filter", "playback.preroll",
                            "playback.preroll_rate", "playback.preroll_bound_ms"}) {
        const replay::ConfigKey* k = replay::FindConfigKey(key);
        REQUIRE(k != nullptr);
        CHECK((k->flags & replay::kConfigEnvOverride) != 0);
    }
    // Every declared override row has a cell: a row past the capacity keeps its file value
    // whatever its variable says, which is how the last declared key once ignored its own.
    size_t overridable = 0;
    for (size_t i = 0; i < replay::ConfigKeyCount(); ++i) {
        overridable += (replay::ConfigKeys()[i].flags & replay::kConfigEnvOverride) != 0;
    }
    CHECK(overridable >= 18);
    CHECK(overridable <= replay::kEnvOverrideCapacity);
    replay::config::ResetForTest();
}

TEST_CASE("a key read as the wrong type returns the CALLER's default, not one of the wrong type") {
    replay::config::ResetForTest();
    const replay::IConfig* cfg = replay::config::Interface();

    // W1-D's probe. tick.mode is declared a string; asked for as a bool it used to fall through to the
    // string row's unset boolDefault - a confident `false` nobody wrote, which read as "the fallback
    // tick is not declared" in every configuration including tick.mode=hook. A `bool` return has no
    // refusal channel, so the caller's default is the answer and the Warn line is the loud part; a
    // caller that needs the tri-state asks config::RowDeclared, which can read the string.
    CHECK(cfg->GetBool("tick.mode", true) == true);
    CHECK(cfg->GetBool("tick.mode", false) == false);

    // The same fallthrough in the other three readers: one Resolve, four callers.
    CHECK(cfg->GetInt("tick.mode", 7) == 7);
    CHECK(cfg->GetInt("capture.voip", 41) == 41);
    CHECK(cfg->GetDouble("capture.voip", 1.5) == 1.5);
    CHECK(std::string(cfg->GetString("capture.voip", "caller")) == "caller");

    // The one widening Resolve does allow is not a mismatch and must survive the fix.
    CHECK(cfg->GetDouble("log.drain_interval_ms", 250.0) == 250.0);

    // A value in the file cannot resurrect the wrong-typed read either.
    const std::string text = R"({"tick.mode": "hook", "capture.voip": false})";
    REQUIRE(replay::config::LoadFromTextForTest(text.data(), text.size()));
    CHECK(cfg->GetBool("tick.mode", true) == true);
    CHECK(std::string(cfg->GetString("capture.voip", "caller")) == "caller");

    replay::config::ResetForTest();
}

TEST_CASE("RowDeclared: a row is declared exactly when the config asks for that row's detour") {
    replay::config::ResetForTest();

    // A bool row is its own key, against the declared default.
    CHECK(replay::config::RowDeclared("capture.voip") == true);

    // Row 1 is a string. S4.4: auto | slot | hook, and only `hook` asks for a detour - but the value
    // that answers is the one the tick module PUBLISHES, not the one the file asked it for, so before
    // that publish this predicate's false means could-not-ask (see RowDeclarationState below).
    CHECK(replay::config::RowDeclared("tick.mode") == false);

    // Could-not-ask is a refusal, not a shrug.
    CHECK(replay::config::RowDeclared("no.such.key") == false);
    CHECK(replay::config::RowDeclared(nullptr) == false);
    CHECK(replay::config::RowDeclared("log.drain_interval_ms") == false);

    const std::string on = R"({"tick.mode": "hook", "capture.voip": false})";
    REQUIRE(replay::config::LoadFromTextForTest(on.data(), on.size()));
    CHECK(replay::config::RowDeclared("tick.mode") == false);   // asked for, not yet resolved
    REQUIRE(replay::config::SetEffectiveString("tick.mode", "hook"));
    CHECK(replay::config::RowDeclared("tick.mode") == true);
    CHECK(replay::config::RowDeclared("capture.voip") == false);

    replay::config::ResetForTest();
    const std::string slot = R"({"tick.mode": "slot"})";
    REQUIRE(replay::config::LoadFromTextForTest(slot.data(), slot.size()));
    REQUIRE(replay::config::SetEffectiveString("tick.mode", "slot"));
    CHECK(replay::config::RowDeclared("tick.mode") == false);

    replay::config::ResetForTest();
}

// DIAG-DRAIN-NOOP Q3: the reply-drain detour on NETGAME.UPDATE (RVA 0x1BBDB0) must be attributed to a
// ledger row so SC7 does not FAIL "1 to nobody" -- but it is byte-anchored and installs ONLY in
// tick.mode=slot, refusing cleanly in hook mode where tick's own fallback owns the site.  So its row
// (2.7) is the COMPLEMENT of tick.mode's own hook-only row 1: Declared iff tick resolves to slot, and
// NotDeclared in hook mode (declaring it there would be the 2.5b "declared row with no detour" trap).
TEST_CASE("DIAG-DRAIN-NOOP Q3: pawn.reply_drain carries row 2.7, Declared only in slot mode") {
    using RowState = replay::config::RowState;
    replay::config::ResetForTest();

    // The key SC7 reads for attribution: RowPatchesText(2.7) is true iff the key carries kConfigTextDetour,
    // and hookmgr::SiteAt reads ledgerRow off it -- so both must be exactly these values, or the 0x1BBDB0
    // patch is attributed to nobody (the FAIL-before this row exists to fix).
    const replay::ConfigKey* drain = replay::FindConfigKey("pawn.reply_drain");
    REQUIRE(drain != nullptr);
    CHECK(std::string(drain->ledgerRow ? drain->ledgerRow : "") == "2.7");
    CHECK((drain->flags & replay::kConfigTextDetour) != 0);

    // BEFORE tick resolves: the provider is still deciding, so the row is Unresolved -- SC7's DeclaredRows
    // defers (excuses) it rather than expecting or dropping it on a guess.
    CHECK(replay::config::RowDeclarationState("pawn.reply_drain") == RowState::Unresolved);

    // SLOT mode (the real launch): the site is pristine, the detour installs, so the row is Declared and
    // SC7 expects exactly the one 0x1BBDB0 patch -> row 2.7 attributed -> PASS.
    replay::config::ResetForTest();
    REQUIRE(replay::config::SetEffectiveString("tick.mode", "slot"));
    CHECK(replay::config::RowDeclarationState("pawn.reply_drain") == RowState::Declared);
    CHECK(replay::config::RowDeclared("pawn.reply_drain") == true);

    // HOOK mode: tick's fallback already owns NETGAME.UPDATE and our Create refuses, so there is NO detour
    // to match -- the row must be NotDeclared, or SC7 expects a site that does not exist (the 2.5b trap).
    replay::config::ResetForTest();
    REQUIRE(replay::config::SetEffectiveString("tick.mode", "hook"));
    CHECK(replay::config::RowDeclarationState("pawn.reply_drain") == RowState::NotDeclared);
    CHECK(replay::config::RowDeclared("pawn.reply_drain") == false);

    // ROUTE OFF: no drain installs in any mode, so the row is NotDeclared even in slot mode.  The route's
    // kill switch disarms its driver, and the ledger must not expect a site the switch turned off.
    replay::config::ResetForTest();
    const std::string off = R"({"pawn.reply_route": false})";
    REQUIRE(replay::config::LoadFromTextForTest(off.data(), off.size()));
    REQUIRE(replay::config::SetEffectiveString("tick.mode", "slot"));
    CHECK(replay::config::RowDeclarationState("pawn.reply_drain") == RowState::NotDeclared);

    replay::config::ResetForTest();
}

TEST_CASE("an effective value is published once, before the first read, or not at all") {
    replay::config::ResetForTest();
    const replay::IConfig* cfg = replay::config::Interface();

    // Set, then read: every reader is served the resolution instead of the `auto` that was asked for,
    // including the row-declaration rule SC7 and hookmgr share.
    CHECK(std::string(replay::config::RequestedString("tick.mode")) == "auto");
    CHECK(replay::config::SetEffectiveString("tick.mode", "hook") == true);
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "hook");
    CHECK(replay::config::RowDeclared("tick.mode") == true);

    // Set twice: refused, and the value already published stands.
    CHECK(replay::config::SetEffectiveString("tick.mode", "slot") == false);
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "hook");

    // Read, then set: refused. This is the whole contract - a consumer served "auto" must never see
    // "hook" on its next read.
    replay::config::ResetForTest();
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "auto");
    CHECK(replay::config::SetEffectiveString("tick.mode", "hook") == false);
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "auto");
    CHECK(replay::config::RowDeclared("tick.mode") == false);

    // A read of any type closes the window, not just a read of the right one.
    replay::config::ResetForTest();
    CHECK(cfg->GetBool("tick.mode", true) == true);
    CHECK(replay::config::SetEffectiveString("tick.mode", "hook") == false);

    // RequestedString does not: it is the input to a resolution, so a provider can ask what it was
    // asked for and still publish what it resolved.
    replay::config::ResetForTest();
    CHECK(std::string(replay::config::RequestedString("tick.mode")) == "auto");
    CHECK(std::string(replay::config::RequestedString("tick.mode")) == "auto");
    CHECK(replay::config::SetEffectiveString("tick.mode", "slot") == true);
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "slot");
    CHECK(replay::config::RowDeclared("tick.mode") == false);

    // Declared string keys and real values only - and none of those refusals may consume the one publish.
    replay::config::ResetForTest();
    CHECK(replay::config::SetEffectiveString("no.such.key", "hook") == false);
    CHECK(replay::config::SetEffectiveString(nullptr, "hook") == false);
    CHECK(replay::config::SetEffectiveString("capture.voip", "hook") == false);
    CHECK(replay::config::SetEffectiveString("tick.mode", nullptr) == false);
    CHECK(replay::config::SetEffectiveString("tick.mode", "") == false);
    CHECK(replay::config::RequestedString("capture.voip") == nullptr);
    CHECK(replay::config::RequestedString("no.such.key") == nullptr);
    CHECK(replay::config::RequestedString(nullptr) == nullptr);
    CHECK(replay::config::SetEffectiveString("tick.mode", "hook") == true);

    // The file's value is what was REQUESTED; a resolution outranks it for readers.
    replay::config::ResetForTest();
    const std::string text = R"({"tick.mode": "slot"})";
    REQUIRE(replay::config::LoadFromTextForTest(text.data(), text.size()));
    CHECK(std::string(replay::config::RequestedString("tick.mode")) == "slot");
    CHECK(replay::config::SetEffectiveString("tick.mode", "hook") == true);
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "hook");

    replay::config::ResetForTest();
}

TEST_CASE("a pinned row cannot be switched off from the file") {
    // Ruling Q-F and S12.0 rule 0: the UI row is not refusable, and MANIFEST / CORE / S_TICK / S_KEY are
    // required to play. A gate whose cheapest green is turning the panel off is a gate that will
    // eventually be paid, so the file simply does not get to make that change.
    replay::config::ResetForTest();
    const std::string text = R"({"ui.panel": false, "capture.section.core": false,
                                 "capture.stream.key": false, "capture.voip": false})";
    REQUIRE(replay::config::LoadFromTextForTest(text.data(), text.size()));
    const replay::IConfig* cfg = replay::config::Interface();

    CHECK(cfg->GetBool("ui.panel", true) == true);
    CHECK(cfg->GetBool("capture.section.core", true) == true);
    CHECK(cfg->GetBool("capture.stream.key", true) == true);
    // ... while a row that IS refusable really is refused, or the pin above would prove nothing.
    CHECK(cfg->GetBool("capture.voip", true) == false);

    // ★ The exact set, not a floor. A `pinned >= 4` floor over exactly five keys let two of them -
    // capture.section.manifest and capture.stream.tick - be unpinned with every check in the tree still
    // green, which is the failure mode kConfigPinnedTrue exists to prevent, one level up.
    //
    // tests/ledgerkeys derives three of these from the design and fails if they lose the pin. The other
    // two are this packet's own judgement and the design does not state them, so they are named here with
    // the reason - which is what makes a later removal a deliberate edit rather than a silent one.
    const char* const kExpectedPins[] = {
        "capture.section.core",      // DESIGN S5.4: required to play
        "capture.section.manifest",  // DESIGN S5.4: required to play
        "capture.stream.key",        // W1-C: without keyframes a file cannot be seeked, so SC5 is unmeetable
        "capture.stream.tick",       // W1-C: the spine. A file with no tick stream has no timeline at all
        "ui.panel",                  // DESIGN S4.4 row 0.20, tied to SC11 by S12.0 RULE 0
    };
    std::set<std::string> expected(std::begin(kExpectedPins), std::end(kExpectedPins));

    std::set<std::string> actual;
    for (size_t i = 0; i < replay::ConfigKeyCount(); ++i) {
        const replay::ConfigKey& k = replay::ConfigKeys()[i];
        if ((k.flags & replay::kConfigPinnedTrue) != 0) {
            actual.insert(k.key);
            CAPTURE(k.key);
            CHECK(k.type == replay::ConfigType::Bool);
            CHECK(k.boolDefault == true);
            CHECK(cfg->GetBool(k.key, true) == true);
        }
    }
    CHECK(actual == expected);
    for (const char* name : kExpectedPins) {
        CAPTURE(name);
        CHECK(actual.count(name) == 1);
    }

    replay::config::ResetForTest();
}

TEST_CASE("a value of the wrong type in the file does not become a value of the right type") {
    replay::config::ResetForTest();
    const std::string text = R"({"capture.voip": "yes", "log.drain_interval_ms": "soon",
                                 "tick.mode": 3})";
    REQUIRE(replay::config::LoadFromTextForTest(text.data(), text.size()));
    const replay::IConfig* cfg = replay::config::Interface();
    CHECK(cfg->GetBool("capture.voip", true) == true);              // declared default, not "yes"
    CHECK(cfg->GetInt("log.drain_interval_ms", 250) == 250);
    CHECK(std::string(cfg->GetString("tick.mode", "auto")) == "auto");
    replay::config::ResetForTest();
}

TEST_CASE("a row nobody can answer yet is its own word, not a false") {
    replay::config::ResetForTest();

    // A bool row always answers: it is its own key, read against the default this build declares.
    CHECK(replay::config::RowDeclarationState("capture.voip") == replay::config::RowState::Declared);
    REQUIRE(replay::config::LoadFromTextForTest("{\"capture.voip\": false}", 23));
    CHECK(replay::config::RowDeclarationState("capture.voip") == replay::config::RowState::NotDeclared);
    replay::config::ResetForTest();

    // A string row is answered by its provider's published resolution. Until then the outcome does not
    // exist, and the file's value is the request rather than the answer.
    const char* asked = "{\"tick.mode\": \"hook\"}";
    REQUIRE(replay::config::LoadFromTextForTest(asked, std::strlen(asked)));
    CHECK(replay::config::RowDeclarationState("tick.mode") == replay::config::RowState::Unresolved);
    CHECK_FALSE(replay::config::RowDeclared("tick.mode"));

    // ★ And asking must not be what closes the window: the publish still succeeds after both calls.
    REQUIRE(replay::config::SetEffectiveString("tick.mode", "hook"));
    CHECK(replay::config::RowDeclarationState("tick.mode") == replay::config::RowState::Declared);
    CHECK(replay::config::RowDeclared("tick.mode"));
    replay::config::ResetForTest();

    // The other resolution, asserted rather than merely absent.
    REQUIRE(replay::config::SetEffectiveString("tick.mode", "slot"));
    CHECK(replay::config::RowDeclarationState("tick.mode") == replay::config::RowState::NotDeclared);
    replay::config::ResetForTest();

    // Refusals keep their old word: an undeclarable row is not a row waiting on anybody.
    CHECK(replay::config::RowDeclarationState("no.such.key") == replay::config::RowState::NotDeclared);
    CHECK(replay::config::RowDeclarationState(nullptr) == replay::config::RowState::NotDeclared);
    CHECK(replay::config::RowDeclarationState("log.drain_interval_ms") == replay::config::RowState::NotDeclared);
}

// AUDIT_L5_RUN3 M3 (§12 item 5). The recorded init order, replayed in the order the launch ran it:
// the startup ledger dump (bootstrap.cpp:357 -> log.cpp DumpLedger) looks at row 1 first, the tick
// module claims the free CNSProvider slot and publishes what it resolved to second
// (tick_policy.cpp:132, `config::SetEffectiveString("tick.mode", EffectiveModeName(mode))`), and
// SC7's late-tick pass builds its denominator third. On L5 #3 step 1 read row 1's EFFECTIVE value,
// which closed the publish window, so step 2 was refused ("'tick.mode' has already been read")
// and step 3 deferred row 1 -- for the life of the process, since the publish never comes twice.
//
// THE INVARIANT, and the reason this is fixed by construction rather than by scheduling: no reader
// of a string row's effective value runs before that row's provider publishes, because the only
// pre-provider reader there was now asks for the REQUEST instead, and a request read does not close
// the window. Ordering nothing has to be arranged cannot be got wrong later.
TEST_CASE("the recorded init order leaves tick.mode's publish window open for its provider") {
    const std::wstring path = LedgerTempLog();
    DeleteFileW(path.c_str());
    replay::log::RedirectForTest(path.c_str());
    replay::log::SetLevel(replay::LogLevel::Info);

    replay::config::ResetForTest();
    const std::string file = R"({"tick.mode": "auto"})";
    REQUIRE(replay::config::LoadFromTextForTest(file.data(), file.size()));

    // 0. Bootstrap resolves row 2.8 from containment BEFORE the dump (WorkerMain), so its window is
    // closed by its own provider and never by a reader.
    REQUIRE(replay::config::SetEffectiveString("playback.host_netvar_hook", "off") == true);
    REQUIRE(replay::config::SetEffectiveString("playback.host_score_hook", "off") == true);

    // 1. The startup ledger dump. The real shipped reader, not a stand-in.
    replay::log::Interface()->DumpLedger();
    replay::log::Drain();
    const std::string dumped = ReadAllTextForTest(path);
    // Non-vacuity: the dump really ran and really printed row 1, so the publish below succeeds
    // AFTER a look at the row rather than because nothing looked.
    REQUIRE(dumped.find("tick.mode") != std::string::npos);
    CHECK(dumped.find("requested = auto") != std::string::npos);

    // 2. The provider publishes the mode the slot claim carried.
    REQUIRE(replay::config::SetEffectiveString("tick.mode", "slot") == true);
    CHECK(std::string(replay::config::Interface()->GetString("tick.mode", "auto")) == "slot");

    // 3. The late-tick pass. DEFERRED-CHECKS D-2 requires deferred_rows: [].
    replay::sc7::Sc7Result late{};
    replay::sc7::DeclaredRows(late);
    CHECK(late.deferredCount == 0);
    // ... and row 1 is not in the denominator either, which is what `slot` MEANS: zero hooks. A row
    // that resolved and a row that never answered are two different absences and only one is a pass.
    CHECK(replay::config::RowDeclarationState("tick.mode") == replay::config::RowState::NotDeclared);
    CHECK(replay::config::RowDeclared("tick.mode") == false);

    // The control, which is also the L5 #3 record retroactively: put ONE effective-value read back
    // in front of the publish -- what the ledger dump used to do -- and every assertion above
    // inverts. Without this arm the case above could pass for reasons that have nothing to do with
    // ordering.
    replay::config::ResetForTest();
    REQUIRE(replay::config::LoadFromTextForTest(file.data(), file.size()));
    CHECK(std::string(replay::config::Interface()->GetString("tick.mode", "auto")) == "auto");
    CHECK(replay::config::SetEffectiveString("tick.mode", "slot") == false);
    CHECK(std::string(replay::config::Interface()->GetString("tick.mode", "auto")) == "auto");
    replay::sc7::Sc7Result stuck{};
    replay::sc7::DeclaredRows(stuck);
    // TWO rows defer on an unpublished tick.mode now: row 1 (tick.mode itself) and row 2.7
    // (pawn.reply_drain, whose declaration is value-gated on the resolved tick mode -- DIAG-DRAIN-NOOP
    // Q3).  Both are excused deferrals, not FAILs, until the provider publishes. Row 2.8 is a third
    // here only because this arm never ran its provider either.
    REQUIRE(stuck.deferredCount == 4);
    CHECK(std::string(stuck.deferred[3].id) == "2.9");
    CHECK(std::string(stuck.deferred[3].key) == "playback.host_score_hook");
    CHECK(std::string(stuck.deferred[3].reason) == "playback-host_score_hook-not-yet-published");
    CHECK(std::string(stuck.deferred[2].id) == "2.8");
    CHECK(std::string(stuck.deferred[2].key) == "playback.host_netvar_hook");
    CHECK(std::string(stuck.deferred[2].reason) == "playback-host_netvar_hook-not-yet-published");
    CHECK(std::string(stuck.deferred[0].id) == "1");
    CHECK(std::string(stuck.deferred[0].key) == "tick.mode");
    CHECK(std::string(stuck.deferred[0].reason) == "tick-mode-not-yet-published");
    CHECK(std::string(stuck.deferred[1].id) == "2.7");
    CHECK(std::string(stuck.deferred[1].key) == "pawn.reply_drain");
    CHECK(std::string(stuck.deferred[1].reason) == "pawn-reply_drain-not-yet-published");

    replay::config::ResetForTest();
    replay::log::EndRedirectForTest();
    DeleteFileW(path.c_str());
}


TEST_CASE("ui.gate_scope resolves from the ledger, so the ARCH-20 gate scope is reachable") {
    replay::config::ResetForTest();
    const replay::IConfig* cfg = replay::config::Interface();

    // W2-F-C2 built the whole ARCH-20 R1 scope behind this key and the config_keys.inc row was
    // held back over packet ownership, which left the feature reachable only through a test seam:
    // GetBool fell through to the caller's default and nothing an operator could write turned it
    // on. This case is the retroactive proof against that state -- without the row an undeclared
    // key cannot be set from a file at all, and the first CHECK below reads `false`.
    const std::string text = R"({"ui.gate_scope": true})";
    REQUIRE(replay::config::LoadFromTextForTest(text.data(), text.size()));
    CHECK(cfg->GetBool("ui.gate_scope", false) == true);

    // ** AND WITH THE FILE GONE, THE ANSWER IS THE ONE config_keys.inc DECLARES -- whichever way
    // that row currently points. Both caller defaults are tried on purpose: a declared row must
    // outrank either of them, so this stays a check on the resolution path and not on the literal.
    const int declared = DeclaredBoolDefault("ui.gate_scope");
    REQUIRE(declared >= 0);
    replay::config::ResetForTest();
    CHECK(replay::config::Interface()->GetBool("ui.gate_scope", true) == (declared == 1));
    CHECK(replay::config::Interface()->GetBool("ui.gate_scope", false) == (declared == 1));

    replay::config::ResetForTest();
}

TEST_CASE("ui.gate_scope=false stays reachable, so the retail-behaviour arm is one row away") {
    // ARCH-20 S6.0 is the normative statement and it did not move when the default did: with the
    // scope off the build is retail in behaviour, no engine setter is called and no .text byte
    // differs. The default shipping true only makes that the arm an operator opts into,
    // so what has to be pinned now is that they still CAN -- this reddens if the row is ever made
    // unrefusable (kConfigPinnedTrue) or if the file value stops outranking the declared default.
    replay::config::ResetForTest();
    const std::string off = R"({"ui.gate_scope": false})";
    REQUIRE(replay::config::LoadFromTextForTest(off.data(), off.size()));
    CHECK(replay::config::Interface()->GetBool("ui.gate_scope", true) == false);
    CHECK(replay::config::Interface()->GetBool("ui.gate_scope", false) == false);
    replay::config::ResetForTest();
}
// ------------------------------------------------------------------------------------------------
// The stream cross-check.  Three tables name the same streams and no two of them can see each other:
// capture.cpp's kStreamKeys (the ONLY thing that ever sets builder.streamEnabled[], so a stream
// missing from it can never be produced), this registry (what a user can set and what the startup
// print lists), and DESIGN S5.4 (what a stream id is allowed to mean).  S_GAMESTATE was in the writer
// and in neither of the other two for eleven rounds -- L2-RUN-5 F3 -- and nothing said so, because
// each table was only ever checked against itself.  So these cases read the other two AS TEXT rather
// than keeping a copy here: a copy is the third declaration that drifts.
namespace {

std::string ReadRepoTextFile(const char* rel) {
    std::ifstream in(std::string(REPLAY_REPO_ROOT) + "/" + rel, std::ios::binary);
    if (!in) return std::string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Every key literal in capture.cpp's `kStreamKeys[] = { {id, "key"}, ... }`.  A row is a line that
// opens a brace and carries a quoted string; the table's comments carry neither, so they contribute
// nothing and are not skipped by pattern-matching their text.
std::set<std::string> StreamKeysFromCaptureSource(const std::string& src) {
    std::set<std::string> keys;
    const size_t table = src.find("kStreamKeys[] = {");
    if (table == std::string::npos) return keys;
    const size_t end = src.find("\n    };", table);
    if (end == std::string::npos) return keys;
    size_t line = table;
    while (line < end) {
        size_t stop = src.find('\n', line);
        if (stop == std::string::npos) stop = end;
        if (stop > end) stop = end;
        const std::string text = src.substr(line, stop - line);
        const size_t brace = text.find('{');
        const size_t open = text.find('"');
        if (brace != std::string::npos) {
            if (open != std::string::npos) {
                if (open > brace) {
                    const size_t close = text.find('"', open + 1);
                    if (close != std::string::npos) keys.insert(text.substr(open + 1, close - open - 1));
                }
            }
        }
        line = stop + 1;
    }
    return keys;
}

// Every stream id the demo format defines, section streams and class tables alike.
const std::set<std::string> kFormatStreamIds = {
    "S_ASSEEN", "S_BODY_SAMPLED", "S_BODY_WIRE", "S_DEGRADED", "S_DROPPED", "S_EVENT", "S_GAMESTATE",
    "S_GAP", "S_INDEX", "S_KEY", "S_MANIFEST", "S_MARK", "S_NETVAR", "S_ORACLE", "S_PLAYER_SAMPLED",
    "S_PLAYER_STATS", "S_PLAYER_WIRE", "S_TICK", "S_VOIP", "S_WORLD",
};

struct RegistryRow {
    const char* key;
    const char* stream;
};

// Every row the registry declares, of every type: a key declared as the wrong type is still declared,
// and reporting it as missing would send the reader hunting for a row that is right there.
std::vector<RegistryRow> AllRegistryRows() {
    std::vector<RegistryRow> rows;
#define REPLAY_CONFIG_BOOL(K, D, ROW, STREAM, F, DOC) rows.push_back(RegistryRow{K, STREAM});
#define REPLAY_CONFIG_INT(K, D, ROW, STREAM, F, DOC) rows.push_back(RegistryRow{K, STREAM});
#define REPLAY_CONFIG_DOUBLE(K, D, ROW, STREAM, F, DOC) rows.push_back(RegistryRow{K, STREAM});
#define REPLAY_CONFIG_STRING(K, D, ROW, STREAM, F, DOC) rows.push_back(RegistryRow{K, STREAM});
#include "bootstrap/config_keys.inc"
#undef REPLAY_CONFIG_BOOL
#undef REPLAY_CONFIG_INT
#undef REPLAY_CONFIG_DOUBLE
#undef REPLAY_CONFIG_STRING
    return rows;
}

}  // namespace

TEST_CASE("every stream key capture.cpp switches on is declared in the registry") {
    const std::string src = ReadRepoTextFile("src/capture/capture.cpp");
    REQUIRE_MESSAGE(!src.empty(), "cannot read src/capture/capture.cpp through REPLAY_REPO_ROOT -- "
                                 "refusing to report a pass over a file this case never opened");

    const std::set<std::string> table = StreamKeysFromCaptureSource(src);
    // THE COUNT COMES FROM THE PRODUCT'S OWN LIST, NOT FROM A FLOOR (L2-RUN-8 F5). It used to be
    // `>= 10` over a fourteen-row table, so a parse that read only ten still passed and the four
    // rows it never saw were never checked against the registry at all. kProducibleStreams is the
    // same list capture.cpp static_asserts its own table against, so parsing one row fewer than
    // the capture can produce is now a refusal rather than a quieter pass.
    REQUIRE_MESSAGE(table.size() == size_t(replay::capture::kProducibleStreamCount),
                    "src/capture/capture.cpp's kStreamKeys parsed to " << table.size() <<
                    " key(s) but the capture produces " <<
                    size_t(replay::capture::kProducibleStreamCount) <<
                    " stream(s): the table moved or changed shape, and a case that read only some "
                    "of its rows certifies only those rows");

    std::set<std::string> declared;
    for (const RegistryRow& row : AllRegistryRows()) declared.insert(row.key);
    REQUIRE(declared.size() >= 40);

    for (const std::string& key : table) {
        const std::string why = "capture.cpp's kStreamKeys switches a stream on `" + key +
                                "`, but src/bootstrap/config_keys.inc declares no such key: it "
                                "cannot be set from a file, the startup print does not list it, and "
                                "no ledger or DESIGN check can see the stream at all";
        CHECK_MESSAGE(declared.count(key) == 1, why);
    }
}

TEST_CASE("every declared capture.stream key names a stream the format defines") {
    const std::set<std::string>& defined = kFormatStreamIds;

    int checked = 0;
    for (const RegistryRow& row : AllRegistryRows()) {
        if (std::strncmp(row.key, "capture.stream.", 15) != 0) continue;
        ++checked;
        const std::string key(row.key);
        const std::string unclaimed = "config_keys.inc row `" + key +
                                      "` claims no stream at all, so nothing connects the key to the "
                                      "format";
        REQUIRE_MESSAGE(row.stream != nullptr, unclaimed);
        const std::string claimed(row.stream);
        const std::string why = "key " + key + " claims stream " + claimed +
                                ", which the demo format does not define: a key "
                                "claiming such an id is a knob for a stream that does not exist";
        CHECK_MESSAGE(defined.count(claimed) == 1, why);
    }
    CHECK(checked >= 10);
}
