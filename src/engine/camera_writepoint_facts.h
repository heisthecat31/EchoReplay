#pragma once

// The numbers W1-H needs that facts/facts.tsv does not carry yet. All 84 rows are submitted in
// the W1-H fact drop for W0-2 to merge; docs/spikes/W1-H.md carries the derivation of each.
//
// The static_asserts at the foot are unconditional: they turn this header from a second copy of the
// merged values into an assertion against the registry -- the same shape W1-L used for
// wiretap_facts.h and W1-D for tick_facts.h.
//
// Everything else this module touches is already in the registry and is used through facts::
// directly: GAME_SINGLETON_RVA, GAME_NETGAME_PTR_OFF, CS_LOOKUP_BY_SYMBOL (+_PROLOGUE), and the
// two offsets the rig is resolved through --
//
//   facts::NETGAME_SPECTATORCAM_CS_OFF   0x2B10  V-PRIMARY, facts.tsv:443   the LEVEL space
//   facts::NETGAME_CS_SPACE_OFF          0x2B08  V-PRIMARY, facts.tsv:1127  the CncaGameSpace
//
// ★ facts::IDENT_NETGAME_GAMESPACE_OFF (0x08) is NOT one of them and this module never reads it.
// Despite the id, that slot holds the CR15Game singleton -- CR15NetGame's back-pointer to the game
// that owns it (facts.tsv:881). CS.TABLE_OFF applied to it reads a render-target / memory-budget
// block, which is the permanent CapacityOverBlock the guard kept reporting. ARCH-10 S1.1/S1.2, and
// ARCH-12 R5's reserved word: +0x8 is `game`, +0x2B08 is `csSpace`, +0x2B10 is `levelSpace`. Never
// bare "game space", and never "game space" for +0x2B10.
//
// ★ RESTORED READ, and the correction owed for having removed it (ARCH-12 R5). This header once
// carried `kNetGameGamespaceOff = 0x2B10` under the name NETGAME.GAMESPACE_MT_OFF and handed that
// pointer to the lookup as a space. That was right, and the name was right: the object is a
// `CncaGameSpaceMT`. It was deleted on the reasoning that +0x2B10 "is not a space; it is the
// spectator component system" -- taken from the id NETGAME.SPECTATORCAM_CS_OFF and from ARCH-10
// S3.5 item 3, which called it "a third object wearing the name". Both are now struck. ARCH-12 S2.2
// proves the slot is written with the return of CR15Game::vtbl[+0x330] called on the level symbol
// the engine has just logged as "[NETGAME] Loading map '%s'", and handed straight to
// `SetCurrentSpawnGameSpace(CncaGameSpaceMT*)`; S2.3 proves both of the engine's own camera
// consumers pass it, unchanged, as argument 1 of CS.LOOKUP_BY_SYMBOL. **The id is a misnomer: do
// not read a type from it.** The number 0x2B10 was always correct; only the type was wrong, and the
// registry row's own comment records where the error entered ("libr15 was consulted for the TYPE
// NAME only").
//
// The two slots are adjacent members of one CR15NetGame but hold two DIFFERENT spaces, so the old
// demand that they AGREE was comparing a space to a component system and could only ever fail.
// ARCH-12 R2 replaces it with a differential across the two. docs/spikes/W1-H.md S13, ARCH-12 S2/S3.

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay {
namespace camera_facts {

inline constexpr uint64_t kImageBase = 0x140000000ull;

// --- the commit point, and the integrator it invokes -----------------------------------------
inline constexpr uint64_t kViewCommitVA          = 0x1405179D0ull;  // CAM.VIEW_COMMIT
inline constexpr uint64_t kIntegrateFreeVA       = 0x140713A10ull;  // CAM.VIEWSTATE_INTEGRATE_FREE
inline constexpr uint64_t kIntegrateBaseVA       = 0x140714800ull;  // CAM.VIEWSTATE_INTEGRATE_BASE

// --- view-state array and object layout -------------------------------------------------------
inline constexpr uint32_t kViewStateArrayOff     = 0x7688u;  // VIEWSTATE.ARRAY_OFF, off CR15Game
inline constexpr uint32_t kViewStateQuatOff      = 0x50u;    // VIEWSTATE.QUAT_OFF     16 B, xyzw
inline constexpr uint32_t kViewStatePosOff       = 0x60u;    // VIEWSTATE.POS_OFF      12 B
inline constexpr uint32_t kViewStateFovYOff      = 0x88u;    // VIEWSTATE.FOVY_OFF     radians
inline constexpr uint32_t kViewStateInputEnaOff  = 0x9Cu;    // VIEWSTATE.INPUT_ENABLE_OFF ★
inline constexpr uint32_t kMaxViews              = 8u;       // VIEWSTATE.MAX_VIEWS
inline constexpr uint64_t kViewCountGlobalVA     = 0x1420207B0ull;  // VIEW.COUNT_GLOBAL

// --- CCameraNode ------------------------------------------------------------------------------
inline constexpr uint64_t kNodeMgrGlobalVA       = 0x1420A0470ull;  // CAM.NODEMGR_GLOBAL
inline constexpr uint32_t kNodeArrayOff          = 0x2CE80u;        // CAM.NODE_ARRAY_OFF
inline constexpr uint32_t kNodeXformOwnerOff     = 0x70u;           // CAM.NODE_XFORM_OWNER_OFF
inline constexpr uint32_t kNodeXformPoolOff      = 0x28u;           // CAM.NODE_XFORM_POOL_OFF
inline constexpr uint32_t kNodeXformIndexOff     = 0x78u;           // CAM.NODE_XFORM_INDEX_OFF
inline constexpr uint32_t kNodeXformStride       = 0x30u;           // CAM.NODE_XFORM_STRIDE
inline constexpr uint32_t kNodeXformPosOff       = 0x10u;           // CAM.NODE_XFORM_POS_OFF
inline constexpr uint32_t kNodeFovHOff           = 0x98u;           // CAM.NODE_FOVH_OFF

// --- the spectator camera component ------------------------------------------------------------
inline constexpr uint64_t kSpectatorSymbol       = 0x66845D16A79233BDull;  // CAM.SPECTATOR_SYMBOL
inline constexpr uint64_t kSetCameraApiImplVA    = 0x140D44D80ull;  // CAM.SETCAMERA_API_IMPL

// The component-system table on the game space. CS.LOOKUP_BY_SYMBOL guards on the element count at
// +0x30 of this table and then indexes the base at +0x00 with NO further check, so the table has to
// be shown indexable before the space is handed to it. Both numbers are derived twice in
// src/bootstrap/tableguard.h's header comment (libr15 names, PC bytes) and are the same pair
// src/camera/camera_control.cpp resolves the shipping rig through.
// ★ CS.TABLE_OFF has merged (facts.tsv:1084, V-PRIMARY), so the offset below is an alias of the
// registry value rather than a second copy of it. ARCH-10 S3.3 -- a private literal equal to a
// V-PRIMARY value is a number that cannot be corrected when the registry corrects itself.
inline constexpr uint32_t kSpaceCsTableOff       = facts::CS_TABLE_OFF;  // the CncaGameSpace's CTable
inline constexpr uint64_t kSpaceCsTableStride    = 16u;     // element size the lookup indexes by

inline constexpr uint32_t kCsInstanceCountOff    = 0xFCu;   // SPECTATOR.CS_INSTANCE_COUNT_OFF  u16
inline constexpr uint32_t kCsInstanceBaseOff     = 0x100u;  // SPECTATOR.CS_INSTANCE_BASE_OFF
inline constexpr uint32_t kCsLocalIndexOff       = 0x1E8u;  // SPECTATOR.CS_LOCAL_INDEX_OFF     u16
inline constexpr uint32_t kCiModeOff             = 0x3Cu;   // SPECTATOR.CI_MODE_OFF
inline constexpr uint32_t kCiFovYOff             = 0x50u;   // SPECTATOR.CI_FOVY_OFF
inline constexpr uint32_t kCiQuatOff             = 0x1C8u;  // SPECTATOR.CI_QUAT_OFF   16 B
inline constexpr uint32_t kCiPosOff              = 0x1D8u;  // SPECTATOR.CI_POS_OFF    16 B store ★
inline constexpr uint32_t kCiScaleOff            = 0x1E4u;  // SPECTATOR.CI_SCALE_OFF
inline constexpr uint32_t kCiFreeCamActiveOff    = 0x274u;  // SPECTATOR.CI_FREECAM_ACTIVE_OFF
inline constexpr int32_t  kModeFree              = 5;       // SPECTATOR.MODE_FREE
inline constexpr int32_t  kModeApi               = 6;       // SPECTATOR.MODE_API

// --- the CI stride, computed the way the engine computes it -------------------------------------
// 640 is not an immediate anywhere in echovr.exe: .text carries no `imul r,r/m,640` (69 /r 80 02
// 00 00), which is why SPECTATOR.CI_STRIDE is withheld as unanchored and why a literal here would
// be a number with nothing to check it against. Both engine sites scale the instance index with a
// lea that multiplies by 5 and a shl of 7, and those eight bytes ARE registry facts:
//   SPECTATOR.CI_STRIDE_MUL_SITE      0x140D6A536  lea r13,[rax+rax*4] ; shl r13,7   the update
//   SPECTATOR.CI_STRIDE_MUL_SITE_API  0x140D44DA5  lea rcx,[rax+rax*4] ; shl rcx,7   SetCameraAPI
// The two are independent instructions, so asserting they agree is a check, not a restatement.

// Register numbers, not register FIELDS. The REX prefix supplies the high bit of each one, so the
// three-bit fields alone do not name a register: with REX.X set, a SIB of 0x80 is [rax+r8*4], which
// is base + index*4 and not index*5. AUD-CAMERA-STRIDE-GUARD F1 -- the earlier predicate compared
// the fields and passed that encoding.
constexpr uint32_t LeaDest(const uint8_t (&s)[8]) {   // REX.R + modrm.reg
    return (((s[0] >> 2) & 1u) << 3) | ((s[2] >> 3) & 7u);
}
constexpr uint32_t LeaIndex(const uint8_t (&s)[8]) {  // REX.X + sib.index
    return (((s[0] >> 1) & 1u) << 3) | ((s[3] >> 3) & 7u);
}
constexpr uint32_t LeaBase(const uint8_t (&s)[8]) {   // REX.B + sib.base
    return ((s[0] & 1u) << 3) | (s[3] & 7u);
}
constexpr uint32_t ShlDest(const uint8_t (&s)[8]) {   // REX.B + modrm.rm
    return ((s[4] & 1u) << 3) | (s[6] & 7u);
}

// True when the eight bytes are `lea r64,[r+r*scale]` followed by `shl r64,imm8` on the register the
// lea just wrote -- the only shape the decoders below may read.
constexpr bool IsIndexScaleShl(const uint8_t (&s)[8]) {
    return (s[0] & 0xF8u) == 0x48u &&                   // REX.W; R/X/B are decoded, not ignored
           s[1] == 0x8Du && (s[2] & 0xC7u) == 0x04u &&  // lea r64,[SIB], mod 00
           (s[3] & 7u) != 5u &&                         // mod 00 with base 101 is disp32, no base
           LeaIndex(s) == LeaBase(s) &&                 // ONE register, so the lea is index*(1+scale)
           (s[4] & 0xF8u) == 0x48u && s[5] == 0xC1u &&
           (s[6] & 0xC0u) == 0xC0u &&                   // mod 11: the shl operand is a register
           ((s[6] >> 3) & 7u) == 4u &&                  // /4 -- shl, not one of the seven neighbours
           ShlDest(s) == LeaDest(s);                    // and it scales what the lea just computed
}
// s[3] is the lea's SIB byte: scale in bits 7:6. [r+r*4] is scale 2, and the base adds the extra 1.
constexpr uint32_t LeaIndexFactor(const uint8_t (&s)[8]) { return 1u + (1u << ((s[3] >> 6) & 3u)); }
constexpr uint32_t ShlCount(const uint8_t (&s)[8]) { return s[7]; }

static_assert(IsIndexScaleShl(facts::SPECTATOR_CI_STRIDE_MUL_SITE),
              "SPECTATOR.CI_STRIDE_MUL_SITE is no longer lea r,[r+r*s] / shl r,imm8");
static_assert(IsIndexScaleShl(facts::SPECTATOR_CI_STRIDE_MUL_SITE_API),
              "SPECTATOR.CI_STRIDE_MUL_SITE_API is no longer lea r,[r+r*s] / shl r,imm8");
static_assert(ShlCount(facts::SPECTATOR_CI_STRIDE_MUL_SITE) == facts::SPECTATOR_CI_STRIDE_SHL,
              "SPECTATOR.CI_STRIDE_SHL disagrees with the shl it was read from");

inline constexpr uint32_t kCiStrideLeaFactor = LeaIndexFactor(facts::SPECTATOR_CI_STRIDE_MUL_SITE);
inline constexpr uint32_t kCiStride = kCiStrideLeaFactor << facts::SPECTATOR_CI_STRIDE_SHL;

static_assert(kCiStride == (LeaIndexFactor(facts::SPECTATOR_CI_STRIDE_MUL_SITE_API)
                            << ShlCount(facts::SPECTATOR_CI_STRIDE_MUL_SITE_API)),
              "the engine's two spectator-CI multiply sites disagree on the stride");

// --- prologue guards ---------------------------------------------------------------------------
// Read from the real echovr.exe. A prologue is seeded from the address it guards, so each one
// proves the bytes at that address have not drifted; it does NOT prove the address is the right
// function. The identity evidence for each site is in the W1-H fact drop's `evidence` column.
inline constexpr uint8_t kViewCommitPrologue[20] = {
    0x48, 0x83, 0xFA, 0x03, 0x0F, 0x85, 0x5B, 0x02, 0x00, 0x00,
    0x48, 0x8B, 0xC4, 0x55, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00 };
inline constexpr uint8_t kSetCameraApiImplPrologue[20] = {
    0x45, 0x33, 0xC0, 0x4C, 0x8B, 0xC9, 0x66, 0x44, 0x3B, 0x81,
    0xFC, 0x00, 0x00, 0x00, 0x73, 0x4C, 0x66, 0x45, 0x3B, 0x81 };
// The two integrator gates. These are not call targets for us -- we never invoke them -- but the
// whole write point rests on `cmp dword ptr [reg+9Ch], 0` still being the first thing each one
// does. Guarding them is what stops a silent behaviour change from looking like a working camera.
// VIEWSTATE.INPUT_GATE_FREE_SITE / VIEWSTATE.INPUT_GATE_BASE_SITE, at the offsets below.
inline constexpr uint32_t kIntegrateFreeGateOff = 0x4Au;  // 0x140713A5A - 0x140713A10
inline constexpr uint32_t kIntegrateBaseGateOff = 0xC7u;  // 0x1407148C7 - 0x140714800
inline constexpr uint8_t  kIntegrateFreeGate[20] = {
    0x83, 0xBF, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x87,
    0x40, 0x01, 0x00, 0x00, 0x0F, 0x84, 0x7C, 0x0D, 0x00, 0x00 };
inline constexpr uint8_t  kIntegrateBaseGate[20] = {
    0x83, 0xBE, 0x9C, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x84, 0x87,
    0x08, 0x00, 0x00, 0x48, 0x89, 0x9C, 0x24, 0xA0, 0x01, 0x00 };

// CAM.SETCAMERA_API_STORES, at 0x140D44DB4. The three stores that prove SetCameraAPI writes the
// CI rather than reading it, and that it forces the mode to 6 as a side effect. Guarding the
// stores rather than only the prologue is what rule 9 asks for.
inline constexpr uint32_t kSetCameraApiStoresOff = 0x34u;  // 0x140D44DB4 - 0x140D44D80
inline constexpr uint8_t  kSetCameraApiStores[20] = {
    0x0F, 0x11, 0x81, 0xC8, 0x01, 0x00, 0x00, 0xC7, 0x41, 0x3C,
    0x06, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x89, 0xD8, 0x01, 0x00 };

}  // namespace camera_facts
}  // namespace replay

// Unconditional since CAMERA-STRIDE-GUARD: these were behind -DCAMERA_WRITEPOINT_FACTS_MERGED,
// which no target in this tree defines, so every assertion below was dead and a drifted value would
// have compiled. The kCiStride line that stood here is gone with the literal it guarded -- the
// stride is derived from facts:: above, and its own assertions are at that derivation.
static_assert(replay::camera_facts::kViewStateArrayOff    == facts::VIEWSTATE_ARRAY_OFF, "drifted");
static_assert(replay::camera_facts::kViewStateInputEnaOff == facts::VIEWSTATE_INPUT_ENABLE_OFF, "drifted");
static_assert(replay::camera_facts::kCiFreeCamActiveOff   == facts::SPECTATOR_CI_FREECAM_ACTIVE_OFF, "drifted");
static_assert(replay::camera_facts::kCiModeOff            == facts::SPECTATOR_CI_MODE_OFF, "drifted");
static_assert(replay::camera_facts::kSetCameraApiImplVA   == facts::CAM_SETCAMERA_API_IMPL, "drifted");
static_assert(replay::camera_facts::kSpectatorSymbol      == facts::CAM_SPECTATOR_SYMBOL, "drifted");
