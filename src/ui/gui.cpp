#include "ui/gui.h"

#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/log.h"
#include "ui/ui_facts.h"

namespace replay {
namespace ui {
namespace gui {
namespace {

const char* const kReplayModuleName = "ui";

// One row per entry point. `disc` is null for the entries whose prologue is unique; where it is
// set, THAT is what the guard compares, at `discOffset` past the entry, because the prologue alone
// cannot tell the entry apart from the others that share it.
struct Site {
    const char* factId;
    uint64_t va;
    const uint8_t* guardBytes;
    uint32_t guardOffset;
    bool required;
    // The guard window, in bytes. 20 for every facade entry point, because that is the window the
    // registry recorded. The two ARCH-20 setters are 14 and 8 bytes long and are declared short:
    // a 20-byte window on either would read past its `retn` into padding and the next function,
    // and a guard that asserts somebody else's bytes cannot fail on a difference in its own.
    uint32_t guardLen = 20;
};

// Order must match `enum class Entry`. Checked by a static_assert on the count below and by
// tests/ui, which walks both and compares names.
constexpr Site kSites[] = {
    {"GUI.BEGIN", ui_facts::kBeginVA, ui_facts::kBeginPrologue, 0, true},
    {"GUI.END", ui_facts::kEndVA, ui_facts::kEndPrologue, 0, true},
    {"GUI.SETNEXTWINDOWPOS.DISCRIMINATOR", ui_facts::kSetNextWindowPosVA,
     ui_facts::kSetNextWindowPosDisc, ui_facts::kSetNextWindowPosDiscOffset, false},
    {"GUI.SETNEXTWINDOWSIZE.DISCRIMINATOR", ui_facts::kSetNextWindowSizeVA,
     ui_facts::kSetNextWindowSizeDisc, ui_facts::kSetNextWindowSizeDiscOffset, false},
    {"GUI.TEXT", ui_facts::kTextVA, ui_facts::kTextPrologue, 0, true},
    {"GUI.SAMELINE", ui_facts::kSameLineVA, ui_facts::kSameLinePrologue, 0, true},
    {"GUI.NEWLINE", ui_facts::kNewLineVA, ui_facts::kNewLinePrologue, 0, false},
    {"GUI.SEPARATOR", ui_facts::kSeparatorVA, ui_facts::kSeparatorPrologue, 0, false},
    {"GUI.COLUMNS", ui_facts::kColumnsVA, ui_facts::kColumnsPrologue, 0, false},
    {"GUI.NEXTCOLUMN", ui_facts::kNextColumnVA, ui_facts::kNextColumnPrologue, 0, false},
    {"GUI.BUTTON", ui_facts::kButtonVA, ui_facts::kButtonPrologue, 0, true},
    {"GUI.CHECKBOX", ui_facts::kCheckboxVA, ui_facts::kCheckboxPrologue, 0, false},
    {"GUI.INPUTREAL", ui_facts::kInputRealVA, ui_facts::kInputRealPrologue, 0, false},
    {"GUI.INPUTTEXT", ui_facts::kInputTextVA, ui_facts::kInputTextPrologue, 0, false},
    {"GUI.PUSHBUTTONDISABLED", ui_facts::kPushButtonDisabledVA,
     ui_facts::kPushButtonDisabledPrologue, 0, false},
    {"GUI.POPBUTTONDISABLED", ui_facts::kPopButtonDisabledVA,
     ui_facts::kPopButtonDisabledPrologue, 0, false},
    {"GUI.SETCURSORENABLED", ui_facts::kSetCursorEnabledVA, ui_facts::kSetCursorEnabledPrologue, 0,
     false},
    {"GUI.CLEAR_SUPPRESSED", ui_facts::kClearSuppressedVA, ui_facts::kClearSuppressedPrologue, 0,
     false, sizeof ui_facts::kClearSuppressedPrologue},
    {"GUI.SET_SUPPRESSED", ui_facts::kSetSuppressedVA, ui_facts::kSetSuppressedPrologue, 0, false,
     sizeof ui_facts::kSetSuppressedPrologue},
};

static_assert(sizeof kSites / sizeof kSites[0] == static_cast<size_t>(Entry::Count),
              "kSites and enum class Entry must stay in step");

const char* const kEntryNames[] = {
    "Begin", "End", "SetNextWindowPos", "SetNextWindowSize", "Text",
    "SameLine", "NewLine", "Separator", "Columns", "NextColumn",
    "Button", "Checkbox", "InputReal", "InputText", "PushButtonDisabled",
    "PopButtonDisabled", "SetCursorEnabled", "ClearSuppressed", "SetSuppressed",
};
static_assert(sizeof kEntryNames / sizeof kEntryNames[0] == static_cast<size_t>(Entry::Count),
              "kEntryNames and enum class Entry must stay in step");

// The facade's C prototypes. C2Vector/C4Vector/CFlagsT are all passed by const reference, which on
// x64 is a pointer, so each is a pointer to the right number of floats or one u32.
using FnBegin = bool (*)(const char*, bool*, const uint32_t*);
using FnVoid = void (*)();
using FnWindowVec = void (*)(const float*, const uint32_t*);
using FnText = void (*)(const char*);
using FnSameLine = void (*)(float, float);
using FnColumns = void (*)(uint32_t, const char*, bool);
using FnNextColumn = void (*)(uint32_t);
using FnButton = bool (*)(const char*, const float*);
using FnCheckbox = bool (*)(const char*, bool*);
using FnInputReal = bool (*)(const char*, float*, float, float, int, const uint32_t*, float, float);
using FnInputText = bool (*)(const char*, char*, uint64_t, const uint32_t*);
using FnColour = void (*)(const float*);
using FnU32 = void (*)(uint32_t);
using FnChar = void (*)(char);

struct State {
    void* fn[static_cast<size_t>(Entry::Count)] = {};
    void** gate = nullptr;
    bool bound = false;
    bool ready = false;
    uint32_t boundCount = 0;
    char diagnostic[512] = {};
};

State g;

void* Resolve(const Site& s) {
    const void* mapped = buildguard::MappedAddress(s.va);
    if (mapped == nullptr) return nullptr;
    if (!buildguard::VerifySite(s.factId, s.va + s.guardOffset, s.guardBytes, s.guardLen)) {
        return nullptr;
    }
    return const_cast<void*>(mapped);
}

void AppendDiagnostic(const char* fmt, const char* a) {
    const size_t used = strlen(g.diagnostic);
    if (used + 2 >= sizeof g.diagnostic) return;
    char line[192];
    snprintf(line, sizeof line, fmt, a);
    if (used > 0) {
        snprintf(g.diagnostic + used, sizeof g.diagnostic - used, "; %s", line);
    } else {
        snprintf(g.diagnostic, sizeof g.diagnostic, "%s", line);
    }
}

template <typename Fn>
Fn At(Entry e) {
    return reinterpret_cast<Fn>(g.fn[static_cast<size_t>(e)]);
}

}  // namespace

const char* EntryName(Entry e) {
    const size_t i = static_cast<size_t>(e);
    return i < static_cast<size_t>(Entry::Count) ? kEntryNames[i] : "<invalid>";
}

bool Bind() {
    if (g.bound) return g.ready;
    g.bound = true;
    g.ready = true;
    g.diagnostic[0] = '\0';

    for (size_t i = 0; i < static_cast<size_t>(Entry::Count); ++i) {
        g.fn[i] = Resolve(kSites[i]);
        if (g.fn[i] != nullptr) {
            ++g.boundCount;
            continue;
        }
        if (kSites[i].required) {
            g.ready = false;
            AppendDiagnostic("%s refused: the panel cannot draw at all without it", kEntryNames[i]);
        } else {
            AppendDiagnostic("%s unavailable", kEntryNames[i]);
        }
    }

    // ★ A HALF-BOUND PAIR IS WORSE THAN NEITHER. PushButtonDisabled pushes a style colour that
    // PopButtonDisabled pops; if only the push resolved we would leak one stack entry per browser
    // row per frame, which the facade's own stack checker would then report against whatever
    // unlucky window closed next. Columns/NextColumn is the same shape. So a pair is all or
    // nothing, and the drop is named.
    struct Pair {
        Entry a, b;
    };
    constexpr Pair kPairs[] = {{Entry::PushButtonDisabled, Entry::PopButtonDisabled},
                               {Entry::Columns, Entry::NextColumn}};
    for (const Pair& p : kPairs) {
        void*& a = g.fn[static_cast<size_t>(p.a)];
        void*& b = g.fn[static_cast<size_t>(p.b)];
        if ((a == nullptr) == (b == nullptr)) continue;
        AppendDiagnostic("%s dropped: its partner did not bind and an unbalanced stack is worse",
                         EntryName(a == nullptr ? p.b : p.a));
        if (a != nullptr) --g.boundCount;
        if (b != nullptr) --g.boundCount;
        a = nullptr;
        b = nullptr;
    }

    const void* gate = buildguard::MappedAddress(ui_facts::kWindowGateVA);
    g.gate = const_cast<void**>(reinterpret_cast<const void* const*>(gate));
    if (g.gate == nullptr) {
        g.ready = false;
        AppendDiagnostic("%s refused: no display object to test", "GUI.WINDOW_GATE");
    }

    if (g.diagnostic[0] == '\0') {
        snprintf(g.diagnostic, sizeof g.diagnostic,
                 "all %u facade entry points resolved and passed their guard", g.boundCount);
    }
    log::Direct(g.ready ? LogLevel::Info : LogLevel::Error, kReplayModuleName,
                "facade bind: %u/%u entry points, %s -- %s", g.boundCount,
                static_cast<uint32_t>(Entry::Count), g.ready ? "usable" : "UNUSABLE", g.diagnostic);
    return g.ready;
}

bool Bound() { return g.bound; }
bool Ready() { return g.ready; }
uint32_t BoundCount() { return g.boundCount; }
const char* Diagnostic() { return g.diagnostic[0] != '\0' ? g.diagnostic : "not bound yet"; }

bool Have(Entry e) {
    const size_t i = static_cast<size_t>(e);
    return i < static_cast<size_t>(Entry::Count) && g.fn[i] != nullptr;
}

bool WindowGateOpen() { return g.gate != nullptr && *g.gate != nullptr; }

bool Begin(const char* name, bool* open, uint32_t flags) {
    FnBegin f = At<FnBegin>(Entry::Begin);
    if (f == nullptr) return false;
    return f(name, open, &flags);
}

void End() {
    FnVoid f = At<FnVoid>(Entry::End);
    if (f != nullptr) f();
}

void SetNextWindowPos(const Vec2& pos, uint32_t cond) {
    FnWindowVec f = At<FnWindowVec>(Entry::SetNextWindowPos);
    if (f != nullptr) f(&pos.x, &cond);
}

void SetNextWindowSize(const Vec2& size, uint32_t cond) {
    FnWindowVec f = At<FnWindowVec>(Entry::SetNextWindowSize);
    if (f != nullptr) f(&size.x, &cond);
}

void Text(const char* s) {
    FnText f = At<FnText>(Entry::Text);
    if (f != nullptr && s != nullptr) f(s);
}

void SameLine(float posX, float spacingW) {
    FnSameLine f = At<FnSameLine>(Entry::SameLine);
    if (f != nullptr) f(posX, spacingW);
}

void NewLine() {
    FnVoid f = At<FnVoid>(Entry::NewLine);
    if (f != nullptr) f();
}

void Separator() {
    FnVoid f = At<FnVoid>(Entry::Separator);
    if (f != nullptr) f();
}

void Columns(uint32_t count, const char* id, bool border) {
    FnColumns f = At<FnColumns>(Entry::Columns);
    if (f != nullptr) f(count, id, border);
}

void NextColumn(uint32_t n) {
    FnNextColumn f = At<FnNextColumn>(Entry::NextColumn);
    if (f != nullptr) f(n);
}

bool Button(const char* label, const Vec2& size) {
    FnButton f = At<FnButton>(Entry::Button);
    if (f == nullptr || label == nullptr) return false;
    return f(label, &size.x);
}

bool Checkbox(const char* label, bool* v) {
    FnCheckbox f = At<FnCheckbox>(Entry::Checkbox);
    if (f == nullptr || label == nullptr || v == nullptr) return false;
    return f(label, v);
}

bool InputReal(const char* label, float* v, float step, float stepFast, int precision,
               uint32_t flags, float minV, float maxV) {
    FnInputReal f = At<FnInputReal>(Entry::InputReal);
    if (f == nullptr || label == nullptr || v == nullptr) return false;
    return f(label, v, step, stepFast, precision, &flags, minV, maxV);
}

bool InputText(const char* label, char* buf, uint64_t cap, uint32_t flags) {
    FnInputText f = At<FnInputText>(Entry::InputText);
    if (f == nullptr || label == nullptr || buf == nullptr || cap == 0) return false;
    return f(label, buf, cap, &flags);
}

void PushButtonDisabled(const Vec4& colour) {
    FnColour f = At<FnColour>(Entry::PushButtonDisabled);
    if (f != nullptr) f(&colour.r);
}

void PopButtonDisabled() {
    FnVoid f = At<FnVoid>(Entry::PopButtonDisabled);
    if (f != nullptr) f();
}

void SetCursorEnabled(uint32_t enabled) {
    FnU32 f = At<FnU32>(Entry::SetCursorEnabled);
    if (f != nullptr) f(enabled);
}

bool ClearSuppressed(uint8_t hotkeyEnable) {
    FnChar f = At<FnChar>(Entry::ClearSuppressed);
    if (f == nullptr) return false;
    f(static_cast<char>(hotkeyEnable));
    return true;
}

bool SetSuppressed() {
    FnVoid f = At<FnVoid>(Entry::SetSuppressed);
    if (f == nullptr) return false;
    f();
    return true;
}

void ResetForTest() { g = State(); }

}  // namespace gui
}  // namespace ui
}  // namespace replay
