#include "fake_gui.h"

#include <string.h>

#include <map>

namespace {

struct FakeState {
    bool ready = true;
    bool windowGate = true;
    bool have[static_cast<size_t>(replay::ui::gui::Entry::Count)] = {};
    bool disabledDepth = false;

    std::vector<std::string> texts;
    std::vector<std::string> buttons;
    std::vector<std::string> disabledButtons;
    std::string clickPending;

    uint64_t items = 0;
    uint64_t glyphs = 0;
    uint64_t calls = 0;

    uint32_t cursorCalls = 0;
    uint32_t windowPosCalls = 0;
    uint32_t windowSizeCalls = 0;
    replay::ui::gui::Vec2 lastWindowPos;
    replay::ui::gui::Vec2 lastWindowSize;
    bool posPending = false;
    replay::ui::gui::Vec2 pendingPos;
    std::map<std::string, replay::ui::gui::Vec2> windowAt;
    int lastCursorArg = -1;
    uint32_t beginCount = 0;
    uint32_t endCount = 0;
    uint32_t beginRefusals = 0;
    bool windowOpened = false;
    // Null means "the frame is always open", which is what every case that is not about the frame
    // byte wants. A counter case binds this to the same byte inframe::Sample reads.
    const uint8_t* frameByte = nullptr;
};

FakeState f;

bool Contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool AnyContains(const std::vector<std::string>& v, const char* needle) {
    for (const std::string& s : v) {
        if (Contains(s, needle)) return true;
    }
    return false;
}

// ImGui measures a label up to "##" and hashes the rest; only the measured part is glyph work.
void NoteItem(const char* label, bool hidesAfterHashes) {
    ++f.items;
    ++f.calls;
    if (label == nullptr) return;
    const char* end = nullptr;
    if (hidesAfterHashes) end = strstr(label, "##");
    f.glyphs += end != nullptr ? static_cast<uint64_t>(end - label) : strlen(label);
}

}  // namespace

namespace uifake {

uint64_t ItemsDrawn() { return f.items; }
uint64_t GlyphsDrawn() { return f.glyphs; }
uint64_t FacadeCallsDrawn() { return f.calls; }

void Reset() {
    const bool ready = f.ready;
    const bool gate = f.windowGate;
    // Survives the reset for the same reason ready and gate do: it is a property of the engine the
    // test is standing up, not of the frame just drawn. A Reset between Sample and Draw would
    // otherwise re-open a frame the test deliberately shut.
    const uint8_t* const frameByte = f.frameByte;
    std::map<std::string, replay::ui::gui::Vec2> windowAt;
    windowAt.swap(f.windowAt);
    bool have[static_cast<size_t>(replay::ui::gui::Entry::Count)];
    memcpy(have, f.have, sizeof have);
    f = FakeState();
    f.ready = ready;
    f.windowGate = gate;
    f.frameByte = frameByte;
    f.windowAt.swap(windowAt);
    memcpy(f.have, have, sizeof have);
}

void SetReady(bool ready) { f.ready = ready; }
void SetWindowGate(bool open) { f.windowGate = open; }
void SetFrameByte(const uint8_t* frameOpenByte) { f.frameByte = frameOpenByte; }
uint32_t BeginRefusals() { return f.beginRefusals; }

void SetHave(replay::ui::gui::Entry e, bool have) {
    f.have[static_cast<size_t>(e)] = have;
}

void ClickOnce(const char* needle) { f.clickPending = needle != nullptr ? needle : ""; }

const std::vector<std::string>& Texts() { return f.texts; }
const std::vector<std::string>& ButtonLabels() { return f.buttons; }
bool TextContains(const char* needle) { return AnyContains(f.texts, needle); }
bool ButtonDrawn(const char* needle) { return AnyContains(f.buttons, needle); }
bool ButtonDisabled(const char* needle) { return AnyContains(f.disabledButtons, needle); }

uint32_t CursorCalls() { return f.cursorCalls; }
uint32_t WindowPosCalls() { return f.windowPosCalls; }
uint32_t WindowSizeCalls() { return f.windowSizeCalls; }
replay::ui::gui::Vec2 LastWindowPos() { return f.lastWindowPos; }
replay::ui::gui::Vec2 LastWindowSize() { return f.lastWindowSize; }
int LastCursorArg() { return f.lastCursorArg; }

void DragWindowBody(const char* name, float dx, float dy) {
    replay::ui::gui::Vec2& p = f.windowAt[name];
    p.x += dx;
    p.y += dy;
}

replay::ui::gui::Vec2 WindowAt(const char* name) {
    const auto it = f.windowAt.find(name);
    return it != f.windowAt.end() ? it->second : replay::ui::gui::Vec2();
}
bool WindowOpened() { return f.windowOpened; }
uint32_t BeginCount() { return f.beginCount; }
uint32_t EndCount() { return f.endCount; }

}  // namespace uifake

namespace replay {
namespace ui {
namespace gui {

const char* EntryName(Entry) { return "fake"; }

bool Bind() { return f.ready; }
bool Bound() { return f.ready; }
bool Ready() { return f.ready; }
bool Have(Entry e) { return f.have[static_cast<size_t>(e)]; }

uint32_t BoundCount() {
    uint32_t n = 0;
    for (size_t i = 0; i < static_cast<size_t>(Entry::Count); ++i) {
        if (f.have[i]) ++n;
    }
    return n;
}

const char* Diagnostic() { return "fake facade"; }
bool WindowGateOpen() { return f.windowGate; }

bool Begin(const char* name, bool* open, uint32_t) {
    ++f.beginCount;
    ++f.calls;
    if (f.posPending && name != nullptr) f.windowAt[name] = f.pendingPos;
    f.posPending = false;
    // Refused before anything is recorded, because the real one records nothing either: a caller
    // that ignores the answer must see an empty window, not a window it merely failed to notice.
    // `open` is left alone -- the engine's Begin does not write it on this path, and writing true
    // here would hand the panel a close-box answer it was never given.
    if (f.frameByte != nullptr && *f.frameByte == 0) {
        ++f.beginRefusals;
        return false;
    }
    if (open != nullptr) *open = true;
    f.windowOpened = true;
    f.texts.push_back(std::string("[begin]") + (name != nullptr ? name : ""));
    return true;
}

void End() { ++f.endCount; ++f.calls; }
void SetNextWindowPos(const Vec2& p, uint32_t) {
    ++f.calls;
    ++f.windowPosCalls;
    f.lastWindowPos = p;
    f.posPending = true;
    f.pendingPos = p;
}
void SetNextWindowSize(const Vec2& p, uint32_t) {
    ++f.calls;
    ++f.windowSizeCalls;
    f.lastWindowSize = p;
}

void Text(const char* s) {
    // Text has no ID, so nothing in it is hidden: every character is measured and drawn.
    NoteItem(s, false);
    f.texts.push_back(s != nullptr ? s : "");
}

// ** LAYOUT IS NOT A WIDGET, and counting it as one made the stream unreadable. SameLine moves the
// cursor; NewLine, Columns and NextColumn move it too. None of them runs ItemAdd, hashes an ID or
// measures a string, which is what Items is defined to count and what the engine actually charges
// for. They are still facade calls through a resolved function pointer, so they are counted -- in
// the counter that says so.
void SameLine(float, float) { ++f.calls; }
void NewLine() { ++f.calls; }
void Separator() { NoteItem(nullptr, false); }
void Columns(uint32_t, const char*, bool) { ++f.calls; }
void NextColumn(uint32_t) { ++f.calls; }

bool Button(const char* label, const Vec2&) {
    NoteItem(label, true);
    const std::string l = label != nullptr ? label : "";
    f.buttons.push_back(l);
    if (f.disabledDepth) f.disabledButtons.push_back(l);
    if (!f.clickPending.empty() && l.find(f.clickPending) != std::string::npos) {
        f.clickPending.clear();
        return true;
    }
    return false;
}

bool Checkbox(const char* label, bool*) {
    NoteItem(label, true);
    return false;
}
bool InputReal(const char* label, float*, float, float, int, uint32_t, float, float) {
    NoteItem(label, true);
    return false;
}
bool InputText(const char* label, char*, uint64_t, uint32_t) {
    NoteItem(label, true);
    return false;
}

void PushButtonDisabled(const Vec4&) { f.disabledDepth = true; ++f.calls; }
void PopButtonDisabled() { f.disabledDepth = false; ++f.calls; }

void SetCursorEnabled(uint32_t enabled) {
    ++f.cursorCalls;
    f.lastCursorArg = static_cast<int>(enabled);
}

void ResetForTest() { uifake::Reset(); }

}  // namespace gui
}  // namespace ui
}  // namespace replay

namespace uifake {
namespace {
std::string g_gateEvents;
bool g_clearBound = true;
bool g_setBound = true;
int g_lastHotkey = -1;
}  // namespace

const std::string& GateEvents() { return g_gateEvents; }
void NoteGateEvent(char c) { g_gateEvents.push_back(c); }
int LastHotkeyArg() { return g_lastHotkey; }

void SetSuppressSettersBound(bool clearBound, bool setBound) {
    g_clearBound = clearBound;
    g_setBound = setBound;
}

void ResetGate() {
    g_gateEvents.clear();
    g_clearBound = true;
    g_setBound = true;
    g_lastHotkey = -1;
}

}  // namespace uifake

namespace replay {
namespace ui {
namespace gui {

// The real pair calls the engine's two setters. Here they only record, which is the whole point:
// what the tests score is the ORDER and the ARGUMENT, and both are decided on our side of the call.
bool ClearSuppressed(uint8_t hotkeyEnable) {
    if (!uifake::g_clearBound) return false;
    uifake::g_lastHotkey = static_cast<int>(hotkeyEnable);
    uifake::NoteGateEvent('C');
    return true;
}

bool SetSuppressed() {
    if (!uifake::g_setBound) return false;
    uifake::NoteGateEvent('S');
    return true;
}

}  // namespace gui
}  // namespace ui
}  // namespace replay
