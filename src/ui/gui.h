#pragma once

// W2-F. The binding to the game's own NRadGui facade: resolve, guard, and call.
//
// ---------------------------------------------------------------------------------------------
// * EVERY ADDRESS IS RESOLVED THROUGH THE MAPPED IMAGE, never as a literal. echovr.exe is
// DYNAMICBASE + HIGH_ENTROPY_VA and the slide is real on this machine, so every entry point goes
// through buildguard::MappedAddress, which is the one place that rebases.
//
// * EVERY ENTRY POINT IS GUARDED BEFORE IT IS EVER CALLED, and two of them cannot be guarded by
// their prologue. SetNextWindowPos and SetNextWindowSize are byte-identical for their first twenty
// bytes; so are four others we do not call. For those two the guard reads the DISCRIMINATOR window
// at entry+4 instead -- the bytes where they load their different globals. A prologue guard there
// would pass whichever of the two we had actually resolved, and calling Size believing it was Pos
// would size the window instead of placing it and read as a layout bug for a week.
//
// * A FAILED GUARD DISABLES ONE WIDGET, NOT THE PLUGIN. buildguard's own header makes the same
// split for hooks: "is this the right build" gates loading, "is this site safe" gates one site.
// Another mod's detour on a facade function, or a prologue that drifted, costs us that widget and
// is NAMED in the panel and the log. Five entry points are required -- Begin, End, Text, Button,
// SameLine -- because a window that cannot open cannot report its own degradation.
//
// * WHAT IS DELIBERATELY NOT BOUND, so that a reader does not think it was forgotten.
//   TextF is variadic and formats through the engine's own printf; we format into our own buffer
//   and call Text, which removes an entire class of format-string behaviour we would be inheriting
//   from a function we have not read.
//   PushStyleColor/PopStyleColor exist and are resolved in ui_facts.h, but PushStyleColor's first
//   argument is an EGuiColors enum whose values this packet has NOT derived. Calling it with a
//   guessed index would recolour an arbitrary element. The timeline therefore encodes the playhead
//   and the round markers in cell HEIGHT, which needs no enum. PushButtonDisabled takes its colour
//   directly as four floats and IS bound -- it is how the browser greys a file the session cannot
//   load, which is a behaviour DESIGN 8.4.3 asks for by name.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

namespace replay {
namespace ui {
namespace gui {

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};
struct Vec4 {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

enum class Entry : uint8_t {
    Begin = 0,
    End,
    SetNextWindowPos,
    SetNextWindowSize,
    Text,
    SameLine,
    NewLine,
    Separator,
    Columns,
    NextColumn,
    Button,
    Checkbox,
    InputReal,
    InputText,
    PushButtonDisabled,
    PopButtonDisabled,
    SetCursorEnabled,
    // The ARCH-20 gate setters. Bound through the same table and the same prologue discipline as
    // every entry above, and NEVER required: a build where they do not resolve draws exactly as it
    // does today, it just cannot open the gate.
    ClearSuppressed,
    SetSuppressed,
    Count,
};

const char* EntryName(Entry);

// Resolves and guards every entry point. Idempotent; the first call decides. Returns Ready().
bool Bind();
bool Bound();          // Bind() has run, whatever it concluded
bool Ready();          // the five required entry points are live
bool Have(Entry);
uint32_t BoundCount();  // how many of Entry::Count resolved and passed their guard

// Never empty. When Ready() it names what degraded, or says nothing did; when !Ready() it names
// which required entry point refused and why. "The UI is unavailable" is not a diagnosis.
const char* Diagnostic();

// GUI.WINDOW_GATE: the display object CR15NetGame::ProcessUI tests before it draws anything. Null
// means headless, and every stock window returns without drawing. So do we.
bool WindowGateOpen();

// ---- the facade, with a no-op for every entry point that did not bind ----------------------
// Begin must ALWAYS be paired with End, including when Begin returns false: the facade runs a
// Begin/End balance checker (0x140F93A10, "Missing call to \"%s\" before \"%s\"") that would put
// our window name into the game's own log every frame.
bool Begin(const char* name, bool* open, uint32_t flags);
void End();
void SetNextWindowPos(const Vec2& pos, uint32_t cond);
void SetNextWindowSize(const Vec2& size, uint32_t cond);
void Text(const char* s);
// Absolute x when posX > 0, current-line continuation when posX == 0 -- the facade's own meaning.
void SameLine(float posX, float spacingW);
void NewLine();
void Separator();
void Columns(uint32_t count, const char* id, bool border);
void NextColumn(uint32_t n);
bool Button(const char* label, const Vec2& size);
bool Checkbox(const char* label, bool* v);
bool InputReal(const char* label, float* v, float step, float stepFast, int precision,
               uint32_t flags, float minV, float maxV);
bool InputText(const char* label, char* buf, uint64_t cap, uint32_t flags);
void PushButtonDisabled(const Vec4& colour);
void PopButtonDisabled();
void SetCursorEnabled(uint32_t enabled);

// ---- the GUI suppression gate (ARCH-20 R1) ----------------------------------------------------
// The engine's own two setters around byte_1420DAF2C, exposed here so that inframe's GateScope
// calls them the way the panel calls Begin -- through a bound, guarded entry and nothing else. The
// panel does not call either.
//
// ** EACH RETURNS FALSE WHEN ITS ENTRY DID NOT BIND, and that answer is load-bearing. An unmapped
// or mutated setter must be a REFUSAL BY NAME with no scope entered, never a silent no-op: a
// ClearSuppressed that quietly did nothing would leave the byte set while its partner restored a
// value nobody had observed.
//
// `hotkeyEnable` must be the byte just read out of GUI.HOTKEY_ENABLE_BYTE. The engine runs an
// edge-toggle of the suppress byte at 0x140F9293A gated on that same byte, so writing anything
// else hands the operator's keyboard a GUI toggle they did not have (ARCH-20 s1.2.2).
bool ClearSuppressed(uint8_t hotkeyEnable);
bool SetSuppressed();

void ResetForTest();

}  // namespace gui
}  // namespace ui
}  // namespace replay
