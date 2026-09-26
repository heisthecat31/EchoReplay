#pragma once

// W2-F. The NRadGui facade entry points the panel, the timeline and the browser call, and the
// per-site guard bytes for each. GENERATED from echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05 --
// no byte below was typed. The same table produced the W2-F fact drop and its anchors, so the
// registry drop and this header cannot disagree about a value.
//
// Configure with -DUI_FACTS_MERGED once W0-2 has merged the W2-F fact drop, and the static
// asserts at the foot turn this header from a second copy of the numbers into an assertion against
// the registry -- the shape W1-H used for camera_writepoint_facts.h and W1-D for tick_facts.h.
//
// ---------------------------------------------------------------------------------------------
// * SIX OF THESE HAVE NO DISTINGUISHING PROLOGUE, and a guard that cannot tell two functions apart
// is not an identity check. SetNextWindowPos and SetNextWindowSize are byte-identical for their
// first 20 bytes; so are CalcTextSize, InputReal3, InputText and PlotLines, four ways. The two we
// call from the ambiguous groups therefore carry a DISCRIMINATOR window as well -- the smallest
// 20-byte window at a 4- or 8-byte offset that separates every member of the group -- and
// gui.cpp checks the discriminator where one exists, never the prologue alone. Calling
// SetNextWindowSize believing it was SetNextWindowPos would move a window instead of sizing it and
// look like a layout bug for a week.
//
// * THE VOCABULARY IS THE WHOLE VOCABULARY. echovr.exe linked only the facade functions its own
// four debug windows and its dialogue UI call. There is no slider, no drag, no Selectable, no
// scrolling child and no mouse-position query in this binary -- established from the Begin/End
// balance-token string literals, of which only six survive in the image. That is why the scrub bar
// below is a strip of Buttons and not a slider: a slider is not available to be called.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay {
namespace ui_facts {

inline constexpr uint64_t kImageBase = 0x140000000ull;

// bool Begin(const char*, bool*, const uint32_t* flags)
inline constexpr uint64_t kBeginVA = 0x140F91290ull;
inline constexpr uint8_t kBeginPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x49, 0x8B };

// void End()
inline constexpr uint64_t kEndVA = 0x140F91D00ull;
inline constexpr uint8_t kEndPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x3D, 0x67, 0x9C, 0x14, 0x01, 0x48, 0x85, 0xFF };

// void SetNextWindowPos(const float pos[2], const uint32_t* cond)
inline constexpr uint64_t kSetNextWindowPosVA = 0x140F95E30ull;
inline constexpr uint8_t kSetNextWindowPosPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xF2, 0x48, 0x8B, 0xF1, 0x48 };
// Prologue is shared; this window at entry + 4 is what identifies it.
inline constexpr uint32_t kSetNextWindowPosDiscOffset = 4u;
inline constexpr uint8_t kSetNextWindowPosDisc[20] = { 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xF2, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x3D, 0x2E, 0x5B };

// void SetNextWindowSize(const float size[2], const uint32_t* cond)
inline constexpr uint64_t kSetNextWindowSizeVA = 0x140F95EE0ull;
inline constexpr uint8_t kSetNextWindowSizePrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xF2, 0x48, 0x8B, 0xF1, 0x48 };
// Prologue is shared; this window at entry + 4 is what identifies it.
inline constexpr uint32_t kSetNextWindowSizeDiscOffset = 4u;
inline constexpr uint8_t kSetNextWindowSizeDisc[20] = { 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xF2, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x3D, 0x7E, 0x5A };

// void PushStyleColor(uint32_t guiColorId, const float rgba[4])
inline constexpr uint64_t kPushStyleColorVA = 0x140F94540ull;
inline constexpr uint8_t kPushStyleColorPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x30 };

// void PopStyleColor()
inline constexpr uint64_t kPopStyleColorVA = 0x140F93FA0ull;
inline constexpr uint8_t kPopStyleColorPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x3D, 0xC7, 0x79, 0x14, 0x01, 0x48, 0x85, 0xFF };

// void PushButtonDisabled(const float rgba[4])
inline constexpr uint64_t kPushButtonDisabledVA = 0x140F941B0ull;
inline constexpr uint8_t kPushButtonDisabledPrologue[20] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xD1, 0xB9, 0x16, 0x00, 0x00, 0x00, 0xE8, 0x7A, 0x03 };

// void PopButtonDisabled()
inline constexpr uint64_t kPopButtonDisabledVA = 0x140F93950ull;
inline constexpr uint8_t kPopButtonDisabledPrologue[20] = { 0x48, 0x83, 0xEC, 0x28, 0xE8, 0x47, 0x06, 0x00, 0x00, 0xE8, 0x42, 0x06, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xE9, 0x39 };

// void Text(const char*)
inline constexpr uint64_t kTextVA = 0x140F96A80ull;
inline constexpr uint8_t kTextPrologue[20] = { 0x48, 0x8B, 0xD1, 0x48, 0x8D, 0x0D, 0xEA, 0x6E, 0x73, 0x00, 0xE9, 0x81, 0x02, 0x00, 0x00, 0xCC, 0x4C, 0x8B, 0xC2, 0x48 };

// void TextF(const char* fmt, ...)
inline constexpr uint64_t kTextFVA = 0x140F96D10ull;
inline constexpr uint8_t kTextFPrologue[20] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x4C, 0x89, 0x4C, 0x24, 0x20 };

// void SameLine(float posX, float spacingW)
inline constexpr uint64_t kSameLineVA = 0x140F95C20ull;
inline constexpr uint8_t kSameLinePrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0x29, 0x74, 0x24, 0x30, 0x0F, 0x29, 0x7C, 0x24, 0x20 };

// void NewLine()
inline constexpr uint64_t kNewLineVA = 0x140F93720ull;
inline constexpr uint8_t kNewLinePrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x3D, 0x47, 0x82, 0x14, 0x01, 0x48, 0x85, 0xFF };

// void Separator()
inline constexpr uint64_t kSeparatorVA = 0x140F95CC0ull;
inline constexpr uint8_t kSeparatorPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x3D, 0xA7, 0x5C, 0x14, 0x01, 0x48, 0x85, 0xFF };

// void Columns(uint32_t count, const char* id, bool border)
inline constexpr uint64_t kColumnsVA = 0x140F91B80ull;
inline constexpr uint8_t kColumnsPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F };

// void NextColumn(uint32_t n)
inline constexpr uint64_t kNextColumnVA = 0x140F937A0ull;
inline constexpr uint8_t kNextColumnPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xF9, 0x48, 0x8B, 0x35 };

// bool Button(const char* label, const float size[2])
inline constexpr uint64_t kButtonVA = 0x140F916B0ull;
inline constexpr uint8_t kButtonPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xE9, 0x48, 0x8B };

// bool Checkbox_(const char* label, bool* v)
inline constexpr uint64_t kCheckboxVA = 0x140F91AD0ull;
inline constexpr uint8_t kCheckboxPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20 };

// bool InputReal(const char*, float*, float, float, int, const uint32_t*, float, float)
inline constexpr uint64_t kInputRealVA = 0x140F93550ull;
inline constexpr uint8_t kInputRealPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60 };

// bool InputText(const char*, char*, uint64_t, const uint32_t*)
inline constexpr uint64_t kInputTextVA = 0x140F93650ull;
inline constexpr uint8_t kInputTextPrologue[20] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57 };

// void SetCursorEnabled(uint32_t)
inline constexpr uint64_t kSetCursorEnabledVA = 0x140F95D60ull;
inline constexpr uint8_t kSetCursorEnabledPrologue[20] = { 0x89, 0x0D, 0x2A, 0x5C, 0x14, 0x01, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x48, 0x89, 0x5C, 0x24 };


// ---------------------------------------------------------------------------------------------
// * THE GUI SUPPRESSION GATE (ARCH-20 R1). These two are not widgets. They are the engine's own
// pair of setters around byte_1420DAF2C -- the byte NRadGui::Begin's third conjunct tests -- and
// ARCH-20 s0.1 establishes that clearing that one byte is not merely necessary but SUFFICIENT to
// open the gate, because the other leg of the disjunction (dword_1420DB988 <= 0) has one writer in
// the whole image and holds process-wide. s1.2 establishes that both functions are PURE SETTERS:
// neither loads a font, creates a device object, nor enables anything else.
//
// ** THE GUARD WINDOW IS THE FUNCTION, NOT 20 BYTES. ClearSuppressed is 14 bytes and SetSuppressed
// is 8. A 20-byte window on either runs past its `retn` into int3 padding and then into the NEXT
// function, so it would be asserting bytes that do not belong to the function it claims to
// identify and could not fail on a real difference inside it (ARCH-20 s3.1, closing paragraph).

// void ClearSuppressed(char hotkeyEnable):  GUI.HOTKEY_ENABLE_BYTE = a1;  GUI.SUPPRESS_BYTE = 0.
// mov cs:byte_142025F6B, cl / mov cs:byte_1420DAF2C, 0 / retn -- the clear is UNCONDITIONAL and a1
// only reaches the hotkey-enable byte, which is why a1 must be the OBSERVED value (s1.2.2).
inline constexpr uint64_t kClearSuppressedVA = 0x140F95D40ull;  // facts-ok: GUI.CLEAR_SUPPRESSED ARCH-20 s3.1 setter; the literal stays because tests/ui/check.ps1 parses THIS line to re-read the byte out of echovr.exe, and the static_assert below binds it to the registry
inline constexpr uint8_t kClearSuppressedPrologue[14] = { 0x88, 0x0D, 0x25, 0x02, 0x09, 0x01, 0xC6, 0x05, 0xDF, 0x51, 0x14, 0x01, 0x00, 0xC3 };

// void SetSuppressed():  GUI.SUPPRESS_BYTE = 1. The whole function, both instructions.
inline constexpr uint64_t kSetSuppressedVA = 0x140F922A0ull;  // facts-ok: GUI.SET_SUPPRESSED ARCH-20 s3.1 setter; the literal stays because tests/ui/check.ps1 parses THIS line to re-read the byte out of echovr.exe, and the static_assert below binds it to the registry
inline constexpr uint8_t kSetSuppressedPrologue[8] = { 0xC6, 0x05, 0x85, 0x8C, 0x14, 0x01, 0x01, 0xC3 };

// The registry drop (W0-2 93d97c1) declares GUI.CLEAR_SUPPRESSED.PROLOGUE as EIGHT bytes; the
// window above is the whole fourteen-byte function. Widening ours to match would be a check on
// less, so the registry's row is asserted as a PREFIX of it instead: the two cannot disagree about
// a byte they both hold, and this header keeps the stronger window.
constexpr bool IsPrefix(const uint8_t* whole, const uint8_t* prefix, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (whole[i] != prefix[i]) return false;
    }
    return true;
}
static_assert(IsPrefix(kClearSuppressedPrologue, facts::GUI_CLEAR_SUPPRESSED_PROLOGUE, 8),
              "GUI.CLEAR_SUPPRESSED.PROLOGUE drifted from this header's whole-function window");
static_assert(IsPrefix(kSetSuppressedPrologue, facts::GUI_SET_SUPPRESSED_PROLOGUE, 8),
              "GUI.SET_SUPPRESSED.PROLOGUE drifted from this header's whole-function window");

// The four gate globals ARCH-20 s2.1 SAMPLES and never writes. Data, so there is no prologue to
// guard and MappedAddress is the whole check; an address that does not resolve makes the sample
// UNRESOLVED rather than zero. kWindowBudgetVA and kReentryDepthVA are read and published but
// never acted on -- acting on a single-writer constant would be acting on an unmeasured one.
inline constexpr uint64_t kSuppressByteVA = facts::GUI_SUPPRESS_BYTE;
inline constexpr uint64_t kReentryDepthVA = facts::GUI_REENTRY_DEPTH;
inline constexpr uint64_t kWindowBudgetVA = facts::GUI_WINDOW_BUDGET;
inline constexpr uint64_t kHotkeyEnableByteVA = facts::GUI_HOTKEY_ENABLE_BYTE;

// ** THE TWO SETTER ADDRESSES ABOVE ARE WRITTEN OUT AND THEN ASSERTED, and that is not a second
// copy of the answer. tests/ui/check.ps1 PARSES this header for `k<Name>VA = 0x...` beside
// `k<Name>Prologue`, walks each address in the PE and re-reads the bytes out of echovr.exe; naming
// the registry symbol on those two lines made the parser drop both rows -- 20 extracted where the
// header declares 22 -- and the guard stopped reading what it claimed to read. The literal is what
// the guard needs; the static_assert is what stops the literal from drifting. The four gate globals
// have no prologue and no parser row, so they keep the symbol.
static_assert(kClearSuppressedVA == facts::GUI_CLEAR_SUPPRESSED, "GUI.CLEAR_SUPPRESSED drifted");
static_assert(kSetSuppressedVA == facts::GUI_SET_SUPPRESSED, "GUI.SET_SUPPRESSED drifted");

// ---------------------------------------------------------------------------------------------
// * ARCH-20 S5's READ SIDE. Two more engine dwords the probe samples and never writes,
// the ImGui context POINTER SLOT, the three offsets that walk from it, and the two .rdata
// constants term 3 of the S5.1 base control compares against.
//
// ** 0x142029880 HOLDS A POINTER; IT IS NOT THE CONTEXT. Every context-relative field below is an
// OFFSET against the value LOADED from that slot, and no sum of the two is ever written down --
// AUDIT_ARCH_19_20 B1 read an unrelated live global as the context and got plausible numbers back.
inline constexpr uint64_t kViewportsEnabledVA = facts::GUI_VIEWPORTS_ENABLED;
inline constexpr uint64_t kBeginBalanceVA = facts::GUI_BEGIN_BALANCE;
inline constexpr uint64_t kImguiContextSlotVA = facts::GUI_IMGUI_CONTEXT;
inline constexpr uint64_t kAtlasConstAVA = facts::GUI_ATLAS_CONST_A;
inline constexpr uint64_t kAtlasConstBVA = facts::GUI_ATLAS_CONST_B;
inline constexpr uint32_t kImguiIoOff = facts::GUI_IMGUI_IO_OFF;
inline constexpr uint32_t kImguiFontsIoOff = facts::GUI_IMGUI_FONTS_IO_OFF;
inline constexpr uint32_t kImguiFrameCountOff = facts::GUI_IMGUI_FRAME_COUNT_OFF;
inline constexpr uint32_t kImguiRenderDrawListsFnOff = facts::GUI_IMGUI_RENDER_DRAW_LISTS_FN_OFF;
inline constexpr uint32_t kImguiCmdListsCountOff = facts::GUI_IMGUI_CMDLISTS_COUNT_OFF;
inline constexpr uint32_t kImguiTotalVtxOff = facts::GUI_IMGUI_TOTAL_VTX_OFF;
// Where sub_140F922D0 stores the two constants inside the atlas (ARCH-20 S5.1, the W2-R rows for
// GUI.ATLAS_CONST_A/B name the stores). Atlas-relative, so there is no registry id to take.
inline constexpr uint32_t kAtlasDwordAOff = 0x20u;
inline constexpr uint32_t kAtlasDwordBOff = 0x24u;

// ---- ARCH-20 S6.3(a): the ImDrawData the engine's EndFrame fills ------------------------------
//
// CORRECTED 2026-09-04 AGAINST THE BINARY. W2-F-C3 read this struct as the 1.86+ ImGui
// layout -- CmdListsCount at +0x04, TotalVtxCount at +0x0C -- and called the two ratified registry
// rows the CmdLists ImVector header. That is refuted by the engine's own Render epilogue: a scan of
// 0x141378100-0x141378800 for r15-relative accesses finds NO access at ImDrawData+0x04 or +0x0C at
// all, and finds a REX.W QWORD store at +0x08 (0x141378710 mov [r15+1890h], r12) of the pointer
// loaded from ctx+0x18B0. A pointer slot there is the pre-1.71 layout -- which is also the only
// layout an engine that still CALLS io.RenderDrawListsFn can have, and it is called, at
// 0x14137874B, with lea rcx,[r15+1888h] as its argument.
//
//     +0x00  bool          Valid          -- EndFrame sets it, NewFrame clears the struct
//     +0x08  ImDrawList**  CmdLists       -- GUI.IMDRAWDATA_CMDLISTS_PTR_OFF
//     +0x10  int           CmdListsCount  -- GUI.IMGUI_CMDLISTS_COUNT_OFF, 0x141378740
//     +0x14  int           TotalVtxCount  -- GUI.IMGUI_TOTAL_VTX_OFF, S6.3(a)'s number, 0x14137872B
//     +0x18  int           TotalIdxCount  -- GUI.IMDRAWDATA_TOTAL_IDX_OFF, 0x141378739
//
// Every field offset below is a difference of two registry rows; no offset is written down here.
// The four static_asserts state the layout where it can fail: re-derive any one row and the build
// stops rather than the reads quietly re-aiming.
inline constexpr uint32_t kImguiDrawDataOff = facts::GUI_IMDRAWDATA_OFF;
inline constexpr uint32_t kDrawDataValidOff = 0x00u;   // the struct's first byte IS Valid
inline constexpr uint32_t kDrawDataCmdListsPtrOff =
    facts::GUI_IMDRAWDATA_CMDLISTS_PTR_OFF - facts::GUI_IMDRAWDATA_OFF;
inline constexpr uint32_t kDrawDataCmdListsCountOff =
    facts::GUI_IMGUI_CMDLISTS_COUNT_OFF - facts::GUI_IMDRAWDATA_OFF;
inline constexpr uint32_t kDrawDataTotalVtxOff =
    facts::GUI_IMGUI_TOTAL_VTX_OFF - facts::GUI_IMDRAWDATA_OFF;
inline constexpr uint32_t kDrawDataTotalIdxOff =
    facts::GUI_IMDRAWDATA_TOTAL_IDX_OFF - facts::GUI_IMDRAWDATA_OFF;

static_assert(kDrawDataCmdListsPtrOff == 0x08u,
              "ImDrawData::CmdLists is the qword at +0x08 (0x141378710 mov [r15+1890h], r12)");
static_assert(kDrawDataCmdListsCountOff == 0x10u,
              "ImDrawData::CmdListsCount is at +0x10 (0x141378740 mov [r15+1898h], edx, then "
              "test edx,edx / jle around the RenderDrawListsFn call)");
static_assert(kDrawDataTotalVtxOff == 0x14u,
              "ImDrawData::TotalVtxCount is at +0x14 (0x14137872B mov [r15+189Ch], eax)");
static_assert(kDrawDataTotalIdxOff == 0x18u,
              "ImDrawData::TotalIdxCount is at +0x18 (0x141378739 mov [r15+18A0h], eax)");

// No frame this engine draws has this many command lists; a read of unrelated memory usually does.
inline constexpr int32_t kDrawDataMaxCmdLists = 4096;

// The minimum set without which there is no panel at all. Everything else degrades to a named
// absence in the UI; these five do not, because a window that cannot open cannot report anything.
inline constexpr uint64_t kRequired[] = {
    kBeginVA, kEndVA, kTextVA, kButtonVA, kSameLineVA,
};

// Already in the registry, named here so gui.cpp reads one header rather than two vocabularies.
inline constexpr uint64_t kWindowGateVA = facts::GUI_WINDOW_GATE;

#ifdef UI_FACTS_MERGED
// After the merge these become assertions rather than a second copy. A value that drifted between
// this header and facts/facts.tsv fails the build instead of shipping two answers.
static_assert(kBeginVA == facts::GUI_BEGIN, "GUI.BEGIN drifted");
static_assert(kEndVA == facts::GUI_END, "GUI.END drifted");
static_assert(kTextVA == facts::GUI_TEXT, "GUI.TEXT drifted");
static_assert(kButtonVA == facts::GUI_BUTTON, "GUI.BUTTON drifted");
static_assert(kSameLineVA == facts::GUI_SAMELINE, "GUI.SAMELINE drifted");
static_assert(kSetNextWindowPosVA == facts::GUI_SETNEXTWINDOWPOS, "GUI.SETNEXTWINDOWPOS drifted");
static_assert(kSetNextWindowSizeVA == facts::GUI_SETNEXTWINDOWSIZE, "GUI.SETNEXTWINDOWSIZE drifted");
#endif

}  // namespace ui_facts
}  // namespace replay
