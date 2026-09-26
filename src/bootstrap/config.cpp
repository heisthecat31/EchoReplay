#include "bootstrap/config.h"

#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bootstrap/log.h"
#include "bootstrap/paths.h"

namespace replay {
namespace {

constexpr int64_t kNoInt = 0;
constexpr double kNoDouble = 0.0;
constexpr bool kNoBool = false;

#define REPLAY_CONFIG_BOOL(K, D, ROW, STREAM, F, DOC) \
    {K, ConfigType::Bool, D, kNoInt, kNoDouble, nullptr, ROW, STREAM, F, DOC},
#define REPLAY_CONFIG_INT(K, D, ROW, STREAM, F, DOC) \
    {K, ConfigType::Int, kNoBool, D, kNoDouble, nullptr, ROW, STREAM, F, DOC},
#define REPLAY_CONFIG_DOUBLE(K, D, ROW, STREAM, F, DOC) \
    {K, ConfigType::Double, kNoBool, kNoInt, D, nullptr, ROW, STREAM, F, DOC},
#define REPLAY_CONFIG_STRING(K, D, ROW, STREAM, F, DOC) \
    {K, ConfigType::String, kNoBool, kNoInt, kNoDouble, D, ROW, STREAM, F, DOC},

const ConfigKey kKeys[] = {
#include "bootstrap/config_keys.inc"
};

#undef REPLAY_CONFIG_BOOL
#undef REPLAY_CONFIG_INT
#undef REPLAY_CONFIG_DOUBLE
#undef REPLAY_CONFIG_STRING

constexpr size_t kKeyCount = sizeof kKeys / sizeof kKeys[0];

}  // namespace

const ConfigKey* ConfigKeys() { return kKeys; }
size_t ConfigKeyCount() { return kKeyCount; }

const ConfigKey* FindConfigKey(const char* key) {
    if (key == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < kKeyCount; ++i) {
        if (std::strcmp(kKeys[i].key, key) == 0) {
            return &kKeys[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------
// The document: JSON, with nested objects flattened to dotted paths so {"capture":{"voip":false}} and
// {"capture.voip":false} mean the same thing. `//` line comments are accepted, because a settings file
// people edit by hand is worth more with its documentation next to the values, and the default file this
// packet writes uses them.
// ---------------------------------------------------------------------------------------------------

struct ConfigDocument::Impl {
    struct Entry {
        std::string key;
        ConfigType  type;
        bool        boolValue;
        int64_t     intValue;
        double      doubleValue;
        std::string stringValue;
    };
    std::vector<Entry>       entries;
    std::vector<ConfigValue> view;   // parallel to entries; pointers fixed up after parsing
    size_t                   duplicates = 0;
};

namespace {

// A hand-written config can nest, and a hostile one can nest a great deal. The limit is well past any
// real settings file and well short of the stack.
constexpr int kMaxDepth = 32;

class Parser {
public:
    Parser(const char* text, size_t len, ConfigDocument::Impl* out, char* error, size_t errorCap)
        : begin_(text), p_(text), end_(text + len), out_(out), error_(error), errorCap_(errorCap) {
        if (errorCap_ > 0) {
            error_[0] = '\0';
        }
    }

    bool ParseDocument() {
        SkipTrivia();
        if (p_ == end_) {
            return true;   // an empty file is a valid configuration: everything takes its default
        }
        if (*p_ != '{') {
            return Fail("the document must be a JSON object");
        }
        std::string prefix;
        if (!ParseObject(prefix, 0)) {
            return false;
        }
        SkipTrivia();
        if (p_ != end_) {
            return Fail("trailing content after the closing brace");
        }
        return true;
    }

private:
    const char* begin_;
    const char* p_;
    const char* end_;
    ConfigDocument::Impl* out_;
    char* error_;
    size_t errorCap_;

    bool Fail(const char* what) {
        if (errorCap_ > 0) {
            std::snprintf(error_, errorCap_, "%s (at byte %llu)", what,
                          static_cast<unsigned long long>(p_ - begin_));
        }
        return false;
    }

    void SkipTrivia() {
        for (;;) {
            while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) {
                ++p_;
            }
            if (end_ - p_ >= 2 && p_[0] == '/' && p_[1] == '/') {
                while (p_ != end_ && *p_ != '\n') {
                    ++p_;
                }
                continue;
            }
            if (end_ - p_ >= 2 && p_[0] == '/' && p_[1] == '*') {
                p_ += 2;
                while (end_ - p_ >= 2 && !(p_[0] == '*' && p_[1] == '/')) {
                    ++p_;
                }
                if (end_ - p_ >= 2) {
                    p_ += 2;
                }
                continue;
            }
            return;
        }
    }

    bool ParseObject(std::string& prefix, int depth) {
        if (depth >= kMaxDepth) {
            return Fail("objects are nested deeper than this parser will follow");
        }
        ++p_;   // '{'
        SkipTrivia();
        if (p_ != end_ && *p_ == '}') {
            ++p_;
            return true;
        }
        for (;;) {
            SkipTrivia();
            if (p_ == end_ || *p_ != '"') {
                return Fail("expected a quoted key");
            }
            std::string name;
            if (!ParseString(name)) {
                return false;
            }
            SkipTrivia();
            if (p_ == end_ || *p_ != ':') {
                return Fail("expected ':' after a key");
            }
            ++p_;

            const size_t mark = prefix.size();
            if (!prefix.empty()) {
                prefix += '.';
            }
            prefix += name;
            const bool ok = ParseMember(prefix, depth);
            prefix.resize(mark);
            if (!ok) {
                return false;
            }

            SkipTrivia();
            if (p_ != end_ && *p_ == ',') {
                ++p_;
                SkipTrivia();
                if (p_ != end_ && *p_ == '}') {
                    ++p_;   // a trailing comma before '}' is tolerated
                    return true;
                }
                continue;
            }
            if (p_ != end_ && *p_ == '}') {
                ++p_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseMember(std::string& key, int depth) {
        SkipTrivia();
        if (p_ == end_) {
            return Fail("expected a value");
        }
        if (*p_ == '{') {
            return ParseObject(key, depth + 1);
        }

        ConfigDocument::Impl::Entry e;
        e.key = key;
        if (*p_ == '"') {
            e.type = ConfigType::String;
            if (!ParseString(e.stringValue)) {
                return false;
            }
        } else if (*p_ == 't' || *p_ == 'f') {
            e.type = ConfigType::Bool;
            if (!ParseKeyword(&e.boolValue)) {
                return false;
            }
        } else if (*p_ == 'n') {
            // null means "no opinion": the declared default stands, and the key is not recorded.
            if (end_ - p_ < 4 || std::strncmp(p_, "null", 4) != 0) {
                return Fail("expected null");
            }
            p_ += 4;
            return true;
        } else if (*p_ == '[') {
            return Fail("array values are not a configuration shape this build understands");
        } else {
            if (!ParseNumber(e)) {
                return false;
            }
        }
        Add(e);
        return true;
    }

    void Add(ConfigDocument::Impl::Entry& e) {
        for (auto& existing : out_->entries) {
            if (existing.key == e.key) {
                ++out_->duplicates;
                existing = e;   // last one wins, and the count is reported
                return;
            }
        }
        out_->entries.push_back(e);
    }

    bool ParseKeyword(bool* value) {
        if (end_ - p_ >= 4 && std::strncmp(p_, "true", 4) == 0) {
            *value = true;
            p_ += 4;
            return true;
        }
        if (end_ - p_ >= 5 && std::strncmp(p_, "false", 5) == 0) {
            *value = false;
            p_ += 5;
            return true;
        }
        return Fail("expected true or false");
    }

    bool ParseNumber(ConfigDocument::Impl::Entry& e) {
        const char* start = p_;
        if (p_ != end_ && (*p_ == '-' || *p_ == '+')) {
            ++p_;
        }
        bool isDouble = false;
        while (p_ != end_) {
            const char c = *p_;
            if (c >= '0' && c <= '9') {
                ++p_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                isDouble = isDouble || (c == '.' || c == 'e' || c == 'E');
                ++p_;
            } else {
                break;
            }
        }
        if (p_ == start) {
            return Fail("expected a number");
        }
        // strtod / strtoll need a terminated buffer, and the document is not ours to modify.
        char buf[64];
        const size_t n = static_cast<size_t>(p_ - start);
        if (n + 1 > sizeof buf) {
            return Fail("numeric literal is implausibly long");
        }
        std::memcpy(buf, start, n);
        buf[n] = '\0';

        char* stop = nullptr;
        if (isDouble) {
            e.type = ConfigType::Double;
            e.doubleValue = std::strtod(buf, &stop);
        } else {
            e.type = ConfigType::Int;
            e.intValue = std::strtoll(buf, &stop, 10);
        }
        if (stop == nullptr || *stop != '\0') {
            return Fail("malformed number");
        }
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned long cp) {
        if (cp < 0x80ul) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800ul) {
            out += static_cast<char>(0xC0ul | (cp >> 6));
            out += static_cast<char>(0x80ul | (cp & 0x3Ful));
        } else if (cp < 0x10000ul) {
            out += static_cast<char>(0xE0ul | (cp >> 12));
            out += static_cast<char>(0x80ul | ((cp >> 6) & 0x3Ful));
            out += static_cast<char>(0x80ul | (cp & 0x3Ful));
        } else {
            out += static_cast<char>(0xF0ul | (cp >> 18));
            out += static_cast<char>(0x80ul | ((cp >> 12) & 0x3Ful));
            out += static_cast<char>(0x80ul | ((cp >> 6) & 0x3Ful));
            out += static_cast<char>(0x80ul | (cp & 0x3Ful));
        }
    }

    bool ParseHex4(unsigned long* out) {
        if (end_ - p_ < 4) {
            return false;
        }
        unsigned long v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = p_[i];
            unsigned long d;
            if (c >= '0' && c <= '9') {
                d = static_cast<unsigned long>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<unsigned long>(c - 'a') + 10ul;
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<unsigned long>(c - 'A') + 10ul;
            } else {
                return false;
            }
            v = (v << 4) | d;
        }
        p_ += 4;
        *out = v;
        return true;
    }

    bool ParseString(std::string& out) {
        ++p_;   // opening quote
        out.clear();
        while (p_ != end_) {
            const char c = *p_++;
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ == end_) {
                break;
            }
            const char esc = *p_++;
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned long cp = 0;
                    if (!ParseHex4(&cp)) {
                        return Fail("malformed \\u escape");
                    }
                    if (cp >= 0xD800ul && cp <= 0xDBFFul && end_ - p_ >= 6 && p_[0] == '\\' &&
                        p_[1] == 'u') {
                        const char* save = p_;
                        p_ += 2;
                        unsigned long lo = 0;
                        if (ParseHex4(&lo) && lo >= 0xDC00ul && lo <= 0xDFFFul) {
                            cp = 0x10000ul + ((cp - 0xD800ul) << 10) + (lo - 0xDC00ul);
                        } else {
                            p_ = save;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    return Fail("unknown escape sequence");
            }
        }
        return Fail("unterminated string");
    }
};

}  // namespace

ConfigDocument::ConfigDocument() : impl_(new Impl()) {}
ConfigDocument::~ConfigDocument() { delete impl_; }

bool ConfigDocument::Parse(const char* text, size_t len, char* error, size_t errorCap) {
    impl_->entries.clear();
    impl_->view.clear();
    impl_->duplicates = 0;

    Parser parser(text, len, impl_, error, errorCap);
    if (!parser.ParseDocument()) {
        // Half a config is a configuration nobody wrote; the caller falls back to the declared defaults.
        impl_->entries.clear();
        return false;
    }
    impl_->view.reserve(impl_->entries.size());
    for (const auto& e : impl_->entries) {
        ConfigValue v{};
        v.type = e.type;
        v.boolValue = e.boolValue;
        v.intValue = e.intValue;
        v.doubleValue = e.doubleValue;
        v.stringValue = e.stringValue.c_str();
        impl_->view.push_back(v);
    }
    return true;
}

size_t ConfigDocument::Count() const { return impl_->entries.size(); }

const char* ConfigDocument::KeyAt(size_t i) const {
    return i < impl_->entries.size() ? impl_->entries[i].key.c_str() : nullptr;
}

const ConfigValue* ConfigDocument::Find(const char* key) const {
    if (key == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < impl_->entries.size(); ++i) {
        if (impl_->entries[i].key == key) {
            return &impl_->view[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------
// The snapshot every module reads through.
// ---------------------------------------------------------------------------------------------------

namespace config {
namespace {

constexpr size_t kUnknownKeyReports = 32;
constexpr size_t kEffectiveKeys = 4;
constexpr size_t kEffectiveValueCap = 64;

// A resolution published by the provider that made it. `ready` is written last, so a reader racing the
// publisher sees either no slot or a whole one.
struct EffectiveValue {
    const ConfigKey* decl;
    LONG             ready;
    char             value[kEffectiveValueCap];
};

struct State {
    ConfigDocument doc;
    bool           loaded;
    bool           filePresent;
    bool           fileWritten;
    size_t         unknownFileKeys;
    char           parseError[256];
    // Log-once bookkeeping. Both arrays are fixed: reporting is a diagnostic, and a diagnostic that can
    // grow without bound on hostile input is a second defect.
    LONG           reported[512];
    const char*    unknownReported[kUnknownKeyReports];
    LONG           unknownReportedCount;
    // A key that has answered a reader can no longer be published to. Indexed like `reported`, by the
    // key's position in the registry.
    LONG           served[512];
    EffectiveValue effective[kEffectiveKeys];
};

// Namespace scope, not a function-local static: a magic static's first call takes a guard lock, and the
// first read of a key can come from any thread. This object is dynamically initialised by the CRT before
// DllMain's body runs, so it is simply there.
State g_state;
State& S() { return g_state; }

// Reports `key` once. Declared keys are indexed by their registry position; anything else goes into a
// small first-come table so a typo is named exactly once and a fuzzed key list cannot flood the log.
bool ClaimReport(const ConfigKey* decl, const char* key) {
    State& s = S();
    if (decl != nullptr) {
        const size_t index = static_cast<size_t>(decl - ConfigKeys());
        if (index >= sizeof s.reported / sizeof s.reported[0]) {
            return false;
        }
        return InterlockedExchange(&s.reported[index], 1) == 0;
    }
    for (LONG i = 0; i < s.unknownReportedCount; ++i) {
        if (s.unknownReported[i] != nullptr && std::strcmp(s.unknownReported[i], key) == 0) {
            return false;
        }
    }
    const LONG slot = InterlockedIncrement(&s.unknownReportedCount) - 1;
    if (slot < 0 || static_cast<size_t>(slot) >= kUnknownKeyReports) {
        return false;
    }
    s.unknownReported[slot] = key;
    return true;
}

// A read that was served an answer for `decl`. This is what closes the publish window.
void MarkServed(const ConfigKey* decl) {
    State& s = S();
    const size_t index = static_cast<size_t>(decl - ConfigKeys());
    if (index < sizeof s.served / sizeof s.served[0]) {
        InterlockedExchange(&s.served[index], 1);
    }
}

bool WasServed(const ConfigKey* decl) {
    State& s = S();
    const size_t index = static_cast<size_t>(decl - ConfigKeys());
    // A key past the end of the table cannot be tracked, and untracked is not "never read": missing
    // data must not answer confidently, so it closes the window rather than opening it.
    if (index >= sizeof s.served / sizeof s.served[0]) {
        return true;
    }
    return InterlockedCompareExchange(&s.served[index], 0, 0) != 0;
}

const char* FindEffective(const ConfigKey* decl) {
    State& s = S();
    for (size_t i = 0; i < kEffectiveKeys; ++i) {
        if (InterlockedCompareExchange(&s.effective[i].ready, 0, 0) != 0 &&
            s.effective[i].decl == decl) {
            return s.effective[i].value;
        }
    }
    return nullptr;
}

const char* TypeName(ConfigType t) {
    switch (t) {
        case ConfigType::Bool:   return "bool";
        case ConfigType::Int:    return "int";
        case ConfigType::Double: return "double";
        case ConfigType::String: return "string";
    }
    return "?";
}

// Resolves a read: the declared key, the file's value if it has one of the right type, and the
// diagnostics for everything that did not line up. Returns nullptr when the declared default stands.
//
// `*declOut` is the declaration THIS read may use, so it stays null whenever the declaration cannot
// answer the question asked - an unknown key, or a key of another type. A string row's boolDefault is
// not a value anybody wrote; handing it back as an answer turned a caller's `true` into `false`.
const ConfigValue* Resolve(const char* key, ConfigType wanted, const ConfigKey** declOut) {
    *declOut = nullptr;
    const ConfigKey* decl = FindConfigKey(key);
    if (decl == nullptr) {
        if (ClaimReport(nullptr, key)) {
            log::Interface()->Write(LogLevel::Warn, "config",
                        "no such key '%s' - returning the caller's default. A key this build does not "
                        "declare is a typo, not a setting", key);
        }
        return nullptr;
    }
    MarkServed(decl);
    if (decl->type != wanted && !(decl->type == ConfigType::Int && wanted == ConfigType::Double)) {
        if (ClaimReport(decl, key)) {
            log::Interface()->Write(LogLevel::Warn, "config",
                        "key '%s' is declared %s but was read as %s - returning the caller's default",
                        key, TypeName(decl->type), TypeName(wanted));
        }
        return nullptr;
    }
    *declOut = decl;
    const ConfigValue* v = S().doc.Find(key);
    if (v == nullptr) {
        return nullptr;
    }
    if (v->type != decl->type && !(v->type == ConfigType::Int && decl->type == ConfigType::Double)) {
        if (ClaimReport(decl, key)) {
            log::Interface()->Write(LogLevel::Warn, "config",
                        "config.json gives '%s' a %s; it is declared %s. Using the default", key,
                        TypeName(v->type), TypeName(decl->type));
        }
        return nullptr;
    }
    return v;
}

// `ui.gate_scope` -> `ECHOREPLAY_UI_GATE_SCOPE`. Derived, never a second literal: a name typed out
// beside the feature is how the feature and the ledger came to read different sources.
bool EnvVarNameFor(const char* key, char* out, size_t cap) {
    static const char kPrefix[] = "ECHOREPLAY_";
    size_t n = 0;
    for (; kPrefix[n] != '\0'; ++n) {
        if (n + 1 >= cap) return false;
        out[n] = kPrefix[n];
    }
    for (size_t i = 0; key[i] != '\0'; ++i) {
        if (n + 1 >= cap) return false;
        const char c = key[i];
        out[n++] = c == '.' ? '_' : (c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    out[n] = '\0';
    return true;
}

// Every kConfigEnvOverride row, resolved ONCE for the process. A magic static, so the resolution
// happens before any reader sees the table and happens exactly once however many threads ask.
struct EnvOverrides {
    struct Row {
        const ConfigKey* decl;
        bool             boolValue;
        int64_t          intValue;
        double           doubleValue;
    };
    // Sized above the count of kConfigEnvOverride rows config_keys.inc declares, with headroom.
    // The loop below fills in declaration order and STOPS at capacity, so an array shorter than the
    // set silently drops the env override of every row past the bound -- which is how the last
    // kConfigEnvOverride key came to ignore its variable. Kept comfortably above the real count.
    Row   rows[kEnvOverrideCapacity];
    size_t count;

    EnvOverrides() : rows{}, count(0) {
        for (size_t i = 0; i < ConfigKeyCount() && count < _countof(rows); ++i) {
            const ConfigKey& k = ConfigKeys()[i];
            if ((k.flags & kConfigEnvOverride) == 0) continue;
            // Strings resolve through providers (tick.mode, the two hook rows), not through here.
            if (k.type == ConfigType::String) continue;
            char name[96];
            char value[32];
            if (!EnvVarNameFor(k.key, name, sizeof name)) continue;
            const DWORD n = ::GetEnvironmentVariableA(name, value, static_cast<DWORD>(sizeof value));
            // Absent, empty, or too long to be a value this build can read: the row stands. A value
            // nobody can spell must not silently decide a feature the owner's screen depends on.
            if (n == 0 || n >= sizeof value) continue;
            Row row{&ConfigKeys()[i], false, 0, 0.0};
            if (!ParseEnvValue(k.type, value, row)) {
                log::Interface()->Write(LogLevel::Warn, "config",
                            "%s is set to something this build does not understand; '%s' stays at "
                            "its configured value", name, k.key);
                continue;
            }
            rows[count] = row;
            ++count;
        }
    }

    // One numeric spelling for the whole table: strtoll / strtod over the WHOLE string, so a
    // trailing character is a refusal rather than a silently truncated number.
    static bool ParseEnvValue(ConfigType type, const char* value, Row& row) {
        if (type == ConfigType::Bool) {
            if (_stricmp(value, "1") == 0 || _stricmp(value, "true") == 0) {
                row.boolValue = true;
                return true;
            }
            if (_stricmp(value, "0") == 0 || _stricmp(value, "false") == 0) {
                row.boolValue = false;
                return true;
            }
            return false;
        }
        char* end = nullptr;
        errno = 0;
        if (type == ConfigType::Int) {
            const long long parsed = std::strtoll(value, &end, 10);
            if (end == value || *end != '\0' || errno == ERANGE) return false;
            row.intValue = static_cast<int64_t>(parsed);
            return true;
        }
        const double parsed = std::strtod(value, &end);
        if (end == value || *end != '\0' || errno == ERANGE) return false;
        row.doubleValue = parsed;
        return true;
    }

    const Row* Find(const ConfigKey* decl) const {
        for (size_t i = 0; i < count; ++i) {
            if (rows[i].decl == decl) return &rows[i];
        }
        return nullptr;
    }
};

const EnvOverrides& Env() {
    static const EnvOverrides resolved;
    return resolved;
}

class ConfigImpl final : public IConfig {
public:
    bool GetBool(const char* key, bool dflt) const override {
        const ConfigKey* decl = nullptr;
        const ConfigValue* v = Resolve(key, ConfigType::Bool, &decl);
        if (decl == nullptr) {
            return dflt;
        }
        WarnOnDefaultDrift(decl, dflt == decl->boolDefault);
        if ((decl->flags & kConfigPinnedTrue) != 0) {
            if (v != nullptr && !v->boolValue && ClaimReport(decl, key)) {
                log::Interface()->Write(LogLevel::Warn, "config",
                            "'%s' is set false but this row is not refusable (ruling Q-F, SC11, S12.0 rule 0); "
                            "the feature stays on", key);
            }
            return true;
        }
        // The runner's override, ahead of the file and the default and BEHIND the pin: a row that
        // is not refusable is not refusable by an environment variable either.
        if ((decl->flags & kConfigEnvOverride) != 0) {
            if (const EnvOverrides::Row* env = Env().Find(decl)) return env->boolValue;
        }
        return v != nullptr ? v->boolValue : decl->boolDefault;
    }

    int64_t GetInt(const char* key, int64_t dflt) const override {
        const ConfigKey* decl = nullptr;
        const ConfigValue* v = Resolve(key, ConfigType::Int, &decl);
        if (decl == nullptr) {
            return dflt;
        }
        WarnOnDefaultDrift(decl, dflt == decl->intDefault);
        if ((decl->flags & kConfigEnvOverride) != 0) {
            if (const EnvOverrides::Row* env = Env().Find(decl)) return env->intValue;
        }
        return v != nullptr ? v->intValue : decl->intDefault;
    }

    double GetDouble(const char* key, double dflt) const override {
        const ConfigKey* decl = nullptr;
        const ConfigValue* v = Resolve(key, ConfigType::Double, &decl);
        if (decl == nullptr) {
            return dflt;
        }
        WarnOnDefaultDrift(decl, dflt == decl->doubleDefault);
        if ((decl->flags & kConfigEnvOverride) != 0) {
            if (const EnvOverrides::Row* env = Env().Find(decl)) {
                return decl->type == ConfigType::Int ? static_cast<double>(env->intValue)
                                                     : env->doubleValue;
            }
        }
        if (v == nullptr) {
            return decl->type == ConfigType::Int ? static_cast<double>(decl->intDefault)
                                                 : decl->doubleDefault;
        }
        return v->type == ConfigType::Int ? static_cast<double>(v->intValue) : v->doubleValue;
    }

    const char* GetString(const char* key, const char* dflt) const override {
        const ConfigKey* decl = nullptr;
        const ConfigValue* v = Resolve(key, ConfigType::String, &decl);
        if (decl == nullptr) {
            return dflt;
        }
        WarnOnDefaultDrift(decl, dflt != nullptr && decl->stringDefault != nullptr &&
                                     std::strcmp(dflt, decl->stringDefault) == 0);
        // A published resolution outranks the file: `auto` is the question, not the answer.
        if (const char* effective = FindEffective(decl)) {
            return effective;
        }
        return v != nullptr ? v->stringValue : decl->stringDefault;
    }

    bool Has(const char* key) const override { return FindConfigKey(key) != nullptr; }

private:
    // Two places asserting a default is exactly the drift config_keys.inc exists to stop, so a caller
    // whose fallback disagrees with the declared one is named rather than quietly overruled.
    static void WarnOnDefaultDrift(const ConfigKey* decl, bool agrees) {
        if (!agrees && ClaimReport(decl, decl->key)) {
            log::Interface()->Write(LogLevel::Warn, "config",
                        "the caller's default for '%s' disagrees with config_keys.inc; the declared "
                        "default is authoritative", decl->key);
        }
    }
};

ConfigImpl g_interface;

std::string DefaultDocument() {
    std::string out;
    out += "{\n";
    out += "  // echoreplay configuration. Written on first launch; every key this build understands is\n";
    out += "  // listed with its default. Delete the file to get it back.\n";
    out += "  //\n";
    out += "  // Keys are dotted. Nested objects work too: \"capture\": { \"voip\": false } is the same\n";
    out += "  // as \"capture.voip\": false. Line comments are an extension of this reader, not JSON.\n";
    for (size_t i = 0; i < ConfigKeyCount(); ++i) {
        const ConfigKey& k = ConfigKeys()[i];
        out += "\n  // ";
        out += k.doc;
        if (k.ledgerRow != nullptr) {
            out += "  [ledger row ";
            out += k.ledgerRow;
            out += "]";
        }
        if (k.stream != nullptr) {
            out += "  [";
            out += k.stream;
            out += "]";
        }
        if ((k.flags & kConfigPinnedTrue) != 0) {
            out += "  [not refusable]";
        }
        out += "\n  \"";
        out += k.key;
        out += "\": ";
        char scratch[64];
        switch (k.type) {
            case ConfigType::Bool:
                out += k.boolDefault ? "true" : "false";
                break;
            case ConfigType::Int:
                std::snprintf(scratch, sizeof scratch, "%lld",
                              static_cast<long long>(k.intDefault));
                out += scratch;
                break;
            case ConfigType::Double:
                std::snprintf(scratch, sizeof scratch, "%g", k.doubleDefault);
                out += scratch;
                break;
            case ConfigType::String:
                out += '"';
                out += k.stringDefault != nullptr ? k.stringDefault : "";
                out += '"';
                break;
        }
        if (i + 1 < ConfigKeyCount()) {
            out += ',';
        }
    }
    out += "\n}\n";
    return out;
}

bool WriteWholeFile(const wchar_t* path, const char* bytes, size_t n) {
    if (!IsWritablePath(path)) {
        return false;
    }
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && written == n;
}

bool ReadWholeFile(const wchar_t* path, std::string* out) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > (4ll << 20)) {
        CloseHandle(file);
        return false;   // a settings file this large is not a settings file
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out->empty() ||
                    ReadFile(file, &(*out)[0], static_cast<DWORD>(out->size()), &read, nullptr);
    CloseHandle(file);
    if (ok == FALSE || read != out->size()) {
        return false;
    }
    return true;
}

void ReportUnknownFileKeys() {
    State& s = S();
    s.unknownFileKeys = 0;
    for (size_t i = 0; i < s.doc.Count(); ++i) {
        const char* key = s.doc.KeyAt(i);
        if (FindConfigKey(key) == nullptr) {
            ++s.unknownFileKeys;
            log::Direct(LogLevel::Warn, "config",
                        "config.json sets '%s', which this build does not declare - it does nothing", key);
        }
    }
}

}  // namespace

bool LoadForAttach() {
    State& s = S();
    if (s.loaded || !Resolved()) {
        return false;
    }
    std::string text;
    if (!ReadWholeFile(ConfigPath(), &text)) {
        return false;   // no file yet: every key stands at its declared default, which is correct
    }
    s.filePresent = true;
    if (!s.doc.Parse(text.c_str(), text.size(), s.parseError, sizeof s.parseError)) {
        return false;   // the worker reports the parse error properly; here we just stay on defaults
    }
    s.loaded = true;
    return true;
}

bool Load() {
    State& s = S();
    s.parseError[0] = '\0';
    s.filePresent = false;
    s.fileWritten = false;

    if (!Resolved()) {
        return false;
    }
    if (!IsWritablePath(StateDir())) {
        return false;
    }
    CreateDirectoryW(StateDir(), nullptr);   // already-exists is the normal case and not an error

    std::string text;
    if (s.loaded) {
        // LoadForAttach already parsed it at DllMain time; report on it rather than parsing twice.
        ReportUnknownFileKeys();
    } else if (ReadWholeFile(ConfigPath(), &text)) {
        s.filePresent = true;
        if (!s.doc.Parse(text.c_str(), text.size(), s.parseError, sizeof s.parseError)) {
            log::Direct(LogLevel::Error, "config",
                        "config.json did not parse: %s. Every key falls back to its declared default; "
                        "the file is left alone so the mistake can be fixed", s.parseError);
        } else {
            ReportUnknownFileKeys();
        }
    } else {
        const std::string defaults = DefaultDocument();
        s.fileWritten = WriteWholeFile(ConfigPath(), defaults.c_str(), defaults.size());
    }

    const char* level = Interface()->GetString("log.level", "info");
    if (std::strcmp(level, "trace") == 0) {
        log::SetLevel(LogLevel::Trace);
    } else if (std::strcmp(level, "warn") == 0) {
        log::SetLevel(LogLevel::Warn);
    } else if (std::strcmp(level, "error") == 0) {
        log::SetLevel(LogLevel::Error);
    } else {
        log::SetLevel(LogLevel::Info);
    }
    const int64_t chatter = Interface()->GetInt("log.chatter_period_ms", 10000);
    log::SetChatterPeriodMs(chatter < 0 ? 10000u
                                        : (chatter > 600000 ? 600000u : uint32_t(chatter)));

    s.loaded = true;
    return true;
}

const IConfig* Interface() { return &g_interface; }

RowState RowDeclarationState(const char* key) {
    const ConfigKey* decl = key != nullptr ? FindConfigKey(key) : nullptr;
    if (decl == nullptr) {
        if (ClaimReport(nullptr, key != nullptr ? key : "<null>")) {
            log::Interface()->Write(LogLevel::Warn, "config",
                        "'%s' declares no row: this build has no such key, so no detour is asked for",
                        key != nullptr ? key : "<null>");
        }
        return RowState::NotDeclared;
    }
    // The reply-drain detour is byte-anchored and mutually exclusive with tick's own hook-mode fallback
    // on NETGAME.UPDATE: it installs ONLY in tick.mode=slot and refuses cleanly in hook mode where tick
    // already owns the site. So its row is Declared only in slot mode -- the complement of tick.mode's
    // own row, which is Declared only in "hook" (the switch below). Gate first on the route: with
    // pawn.reply_route off the drain is never installed, so no row is wanted. Then read the EFFECTIVE
    // tick mode the way the String case does -- Unresolved while the provider is still deciding (excused
    // as a deferral), Declared iff it settled on "slot". This makes SC7 expect the row iff the detour
    // actually installs, and hookmgr's own Create-gate (RowDeclared on this key) then creates it iff so.
    if (std::strcmp(decl->key, "pawn.reply_drain") == 0) {
        if (!RowDeclared("pawn.reply_route")) return RowState::NotDeclared;
        const ConfigKey* tick = FindConfigKey("tick.mode");
        if (tick == nullptr) return RowState::NotDeclared;
        if (FindEffective(tick) == nullptr) return RowState::Unresolved;
        const char* mode = Interface()->GetString(tick->key, tick->stringDefault);
        return mode != nullptr && std::strcmp(mode, "slot") == 0 ? RowState::Declared
                                                                 : RowState::NotDeclared;
    }
    const IConfig* cfg = Interface();
    switch (decl->type) {
        case ConfigType::Bool:
            return cfg->GetBool(decl->key, decl->boolDefault) ? RowState::Declared
                                                              : RowState::NotDeclared;
        case ConfigType::String: {
            // Before the read, and returning before it: the provider's resolution is what declares a
            // string row, and reading the key here would close the window it has to publish through.
            if (FindEffective(decl) == nullptr) {
                return RowState::Unresolved;
            }
            const char* mode = cfg->GetString(decl->key, decl->stringDefault);
            return mode != nullptr && std::strcmp(mode, "hook") == 0 ? RowState::Declared
                                                                     : RowState::NotDeclared;
        }
        default:
            break;
    }
    if (ClaimReport(decl, decl->key)) {
        log::Interface()->Write(LogLevel::Warn, "config",
                    "'%s' is declared %s, which cannot say whether a detour is wanted; no row declared",
                    decl->key, TypeName(decl->type));
    }
    return RowState::NotDeclared;
}

bool RowDeclared(const char* key) { return RowDeclarationState(key) == RowState::Declared; }

const char* RequestedString(const char* key) {
    const ConfigKey* decl = key != nullptr ? FindConfigKey(key) : nullptr;
    if (decl == nullptr || decl->type != ConfigType::String) {
        if (ClaimReport(decl, key != nullptr ? key : "<null>")) {
            log::Interface()->Write(LogLevel::Warn, "config",
                        "no requested value for '%s': it is %s", key != nullptr ? key : "<null>",
                        decl == nullptr ? "not a key this build declares" : "not a string key");
        }
        return nullptr;
    }
    const ConfigValue* v = S().doc.Find(decl->key);
    if (v != nullptr && v->type == ConfigType::String) {
        return v->stringValue;
    }
    return decl->stringDefault;
}

bool SetEffectiveString(const char* key, const char* value) {
    const ConfigKey* decl = key != nullptr ? FindConfigKey(key) : nullptr;
    const char* name = key != nullptr ? key : "<null>";
    if (decl == nullptr) {
        log::Interface()->Write(LogLevel::Warn, "config",
                    "refusing to publish '%s': this build declares no such key", name);
        return false;
    }
    if (decl->type != ConfigType::String) {
        log::Interface()->Write(LogLevel::Warn, "config",
                    "refusing to publish an effective value for '%s': it is declared %s, and only a "
                    "string key carries a resolved mode", name, TypeName(decl->type));
        return false;
    }
    if (value == nullptr || value[0] == '\0') {
        log::Interface()->Write(LogLevel::Warn, "config",
                    "refusing to publish an empty effective value for '%s': an empty resolution is not "
                    "a resolution", name);
        return false;
    }
    const size_t len = std::strlen(value);
    if (len >= kEffectiveValueCap) {
        log::Interface()->Write(LogLevel::Warn, "config",
                    "refusing to publish '%s' for '%s': %zu bytes exceeds the %zu this table holds",
                    value, name, len, kEffectiveValueCap - 1);
        return false;
    }
    State& s = S();
    if (const char* already = FindEffective(decl)) {
        log::Interface()->Write(LogLevel::Warn, "config",
                    "'%s' was already resolved to '%s'; refusing to republish it as '%s'. An effective "
                    "value is written once", name, already, value);
        return false;
    }
    if (WasServed(decl)) {
        log::Interface()->Write(LogLevel::Warn, "config",
                    "'%s' has already been read; refusing to publish '%s' after the fact. A consumer "
                    "must never observe two values for one key", name, value);
        return false;
    }
    // AUDIT_G G-1. Claiming the slot IS the duplicate test. A counter incremented first and
    // bound-tested second advanced permanently on a table-full refusal, and testing FindEffective
    // first left a window in which two threads publishing the same key each took a slot. Claiming
    // `decl` closes both: `ready` is still written last, and FindEffective ignores a slot until
    // `ready`, so a claimed-but-unfilled slot is invisible to every reader.
    size_t slot = kEffectiveKeys;
    for (size_t i = 0; i < kEffectiveKeys; ++i) {
        const void* const held = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(const_cast<ConfigKey**>(&s.effective[i].decl)),
            const_cast<ConfigKey*>(decl), nullptr);
        if (held == nullptr) { slot = i; break; }
        if (held == static_cast<const void*>(decl)) {
            log::Interface()->Write(LogLevel::Warn, "config",
                        "'%s' is already being published on another thread; refusing to publish it as "
                        "'%s'. An effective value is written once", name, value);
            return false;
        }
    }
    if (slot == kEffectiveKeys) {
        log::Interface()->Write(LogLevel::Warn, "config",
                    "refusing to publish '%s' for '%s': the effective-value table holds %zu keys",
                    value, name, kEffectiveKeys);
        return false;
    }
    std::memcpy(s.effective[slot].value, value, len + 1);
    InterlockedExchange(&s.effective[slot].ready, 1);
    log::Interface()->Write(LogLevel::Info, "config", "'%s' resolved to '%s'", decl->key, value);
    return true;
}

size_t FileKeyCount() { return S().doc.Count(); }
size_t UnknownFileKeys() { return S().unknownFileKeys; }
bool FileWasPresent() { return S().filePresent; }
bool FileWasWritten() { return S().fileWritten; }
const char* ParseError() { return S().parseError; }

bool LoadFromTextForTest(const char* text, size_t len) {
    State& s = S();
    s.parseError[0] = '\0';
    const bool ok = s.doc.Parse(text, len, s.parseError, sizeof s.parseError);
    if (ok) {
        ReportUnknownFileKeys();
    }
    s.loaded = ok;
    return ok;
}

void ResetForTest() {
    State& s = S();
    s.doc.Parse("", 0, s.parseError, sizeof s.parseError);
    s.loaded = false;
    s.filePresent = false;
    s.fileWritten = false;
    s.unknownFileKeys = 0;
    std::memset(s.reported, 0, sizeof s.reported);
    s.unknownReportedCount = 0;
    std::memset(s.unknownReported, 0, sizeof s.unknownReported);
    std::memset(s.served, 0, sizeof s.served);
    std::memset(s.effective, 0, sizeof s.effective);
}

}  // namespace config
}  // namespace replay
