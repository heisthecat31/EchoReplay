// B-6: bin\win10\echoreplay\config.json.
//
// The design - "Every row is disableable by a config key, and the log prints the enabled set at
// startup" - and S4.2 B-6 - "every ledger row and every stream has a key". config_keys.inc is that
// promise written down once, in a form both the C++ and tests/ledgerkeys can read: the check parses the
// ledger rows out of S4.4 and the streams out of S5.4 and fails if any of them has no key here.
//
// The snapshot is immutable after first read; effective values may be published once before. IConfig's
// contract is that a value cannot change under a packet mid-session, so there is no reload path and none
// is coming; a setting change needs a restart. The single exception is config::SetEffectiveString, which
// RECORDS a resolution instead of changing a setting - a provider asked for `auto` publishes the mode it
// actually resolved - and is refused the moment anything has read that key, so no consumer can be served
// two values for one key.
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "bootstrap/services.h"

namespace replay {

enum class ConfigType : uint8_t { Bool, Int, Double, String };

enum ConfigFlags : uint32_t {
    kConfigNone = 0,
    // Ruling Q-F and S12.0 RULE 0: a gate must not be satisfiable by removing a required feature. The UI
    // row is declared not refusable (SC11), and MANIFEST / CORE are required to play. These keys exist so a
    // configuration-aware gate can enumerate them, but a `false` in the file is refused and logged rather
    // than honoured - otherwise the cheapest green is a regression.
    kConfigPinnedTrue = 1u << 0,

    // This bool row also answers to an environment variable, whose name is DERIVED from the key:
    // `ui.gate_scope` -> `ECHOREPLAY_UI_GATE_SCOPE`. It exists for a launch profile that cannot
    // carry a config file, and it is resolved HERE rather than beside the feature so that every
    // reader -- the feature, the ledger dump, the launch record -- gets the same answer. A feature
    // that parsed its own variable gave the launch record a second source of truth, and the record
    // printed `ui.gate_scope disabled` about a run that had the scope armed for 5102 frames
    // An unregistered command-line switch is never the mechanism: it would
    // void the engine's whole command line.
    kConfigEnvOverride = 1u << 1,

    // WHAT THE ROW PATCHES, declared here rather than inferred from how its number is spelled. SC7
    // compares two populations -- the hookmgr sites standing in echovr.exe's `.text`, and the config
    // rows that predict them -- and both have to be selected by one fact, or the gate compares two
    // different questions. Row 2.5b is the case that proved it: a Level-2 row that installs no detour
    // at all (it opens the engine's gate through the engine's own setters and restores a `.data`
    // byte), so a `"2."`-prefix test put it in the denominator where no site can ever match it, and
    // SC7 published expected_rows 4 against declared_sites 3 on every launch with the scope on.
    kConfigTextDetour = 1u << 2,
};

// How many kConfigEnvOverride rows one process can hold overridden at once. A row past it keeps
// its file value whatever its variable says, so it stays above the number config_keys.inc declares.
inline constexpr size_t kEnvOverrideCapacity = 96;

struct ConfigKey {
    const char* key;
    ConfigType  type;
    bool        boolDefault;
    int64_t     intDefault;
    double      doubleDefault;
    const char* stringDefault;
    const char* ledgerRow;   // "0.1", "1X.2", "2.3" ... or nullptr
    const char* stream;      // "CORE", "S_TICK" ... or nullptr
    uint32_t    flags;
    const char* doc;
};

const ConfigKey* ConfigKeys();
size_t           ConfigKeyCount();
const ConfigKey* FindConfigKey(const char* key);

// One parsed value out of the file. Exposed so the parser can be tested on its own.
struct ConfigValue {
    ConfigType  type;
    bool        boolValue;
    int64_t     intValue;
    double      doubleValue;
    const char* stringValue;   // owned by the ConfigDocument that produced it
};

// A flat view of a JSON object: nested objects are flattened to dotted paths, so
// {"capture":{"voip":false}} and {"capture.voip":false} are the same document.
class ConfigDocument {
public:
    ConfigDocument();
    ~ConfigDocument();
    ConfigDocument(const ConfigDocument&) = delete;
    ConfigDocument& operator=(const ConfigDocument&) = delete;

    // Returns false and fills `error` on malformed input. A partially parsed document is discarded:
    // half a config is a configuration nobody wrote.
    bool Parse(const char* text, size_t len, char* error, size_t errorCap);

    size_t             Count() const;
    const char*        KeyAt(size_t i) const;
    const ConfigValue* Find(const char* key) const;

    // Opaque by name only: the parser in config.cpp fills it directly, and hiding the definition from
    // the header is what keeps <string> and <vector> out of every consumer's translation unit.
    struct Impl;

private:
    Impl* impl_;
};

namespace config {

// Worker thread. Reads config.json if it is there, writes a fully-commented default if it is not, and
// snapshots the result. Never called from DllMain: it touches the filesystem.
bool Load();

// DllMain time, and read-only. An attach hook is gated on its ledger row's config key, and that key has
// to mean something before the worker exists - S4.4 says every row is disableable, and a key that is only
// consulted after the decision has been taken is not a switch.
//
// Safe under the loader lock for the same reason the attach log line is: one CreateFileW / ReadFile /
// CloseHandle and a bounded parse. No DLL is loaded, no thread is waited on, and the process heap is not
// the loader lock. It never WRITES - producing the default file stays on the worker.
bool LoadForAttach();

// The IConfig every module reads through. Never null; before Load() it serves the declared defaults.
const IConfig* Interface();

// Does the configuration DECLARE this ledger row's intervention? ONE rule, asked by hookmgr's gate
// before it creates a detour and by SC7's expected-row denominator, so the switch and the audit can
// never disagree about which detours were asked for.
//
// A bool row is its own key, read against the default config_keys.inc declares rather than a caller's
// guess at it. A string row is declared by the value that names a detour - S4.4's tick.mode is
// auto | slot | hook, and only `hook` asks for one; `auto` prefers the zero-hook slot claim, so it
// declares nothing and SC7 must not expect a row the shipping default never installs.
//
// A key this build does not declare, and a key of a type that cannot answer, are REFUSED (false) and
// named in the log. An undeclarable row is not a row somebody switched on.
//
// A string row whose provider has not published its resolution yet is UNRESOLVED, not undeclared - see
// RowDeclarationState. This predicate collapses that third state into false, which is the right answer
// for a gate deciding whether to install a detour and the WRONG one for an audit's denominator.
bool RowDeclared(const char* key);

// The same question with the third answer intact, for a caller that has to distinguish `nobody asked for
// this row` from `nobody can answer yet`. A bool row always answers. A string row is answered by the
// provider's PUBLISHED resolution - `tick.mode` is decided on the claim thread and published through
// SetEffectiveString, so before that publish the row's outcome does not exist and scoring it either way
// is a confident answer to missing data.
//
// Unresolved is returned WITHOUT reading the key, which is load-bearing: the read would close the publish
// window and make the resolution it is waiting for impossible.
enum class RowState : uint8_t { NotDeclared, Declared, Unresolved };
RowState RowDeclarationState(const char* key);

// The value the configuration REQUESTED for a declared string key: the file's, or the declared default.
// A read that does not count as one, because it is the INPUT to a resolution rather than the answer to
// one - a provider has to know it was asked for `auto` before it can publish what `auto` resolved to,
// and an ordinary read would close the publish window before the provider could use it. Returns nullptr
// for a key this build does not declare and for one that is not a string.
const char* RequestedString(const char* key);

// Records the value a provider RESOLVED for a declared string key, so every later reader is served the
// mode that is actually running rather than the `auto` that was asked for. Refused, and logged, unless
// the key is declared and a string, the value is non-empty, nothing has read the key yet, and nothing
// has been published for it before. Those refusals are what keep the contract at the top of this file
// true. Returns true only when the value was recorded.
bool SetEffectiveString(const char* key, const char* value);

// Diagnostics for the startup log.
size_t FileKeyCount();
size_t UnknownFileKeys();
bool   FileWasPresent();
bool   FileWasWritten();
const char* ParseError();      // empty when the file parsed, or was absent

// Test seam: load from this text instead of from disk.
bool LoadFromTextForTest(const char* text, size_t len);
void ResetForTest();

}  // namespace config
}  // namespace replay
