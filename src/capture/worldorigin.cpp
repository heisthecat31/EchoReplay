#include "capture/worldorigin.h"

#include <string.h>

#include "bootstrap/buildguard.h"
#include "generated/facts_generated.h"

namespace replay {
namespace capture {
namespace {

// All three are cited now. The registry edit this file was waiting on has landed: OFF_LO and OFF_HI
// were never a pair, and they are retired in favour of a CUMULATIVE triple (_OFF_X/_Y/_Z, recorded
// here) and a PENDING one (_PENDING_OFF_X/_Y/_Z, which the render thread zeroes on every particle
// compute pass and which this file must never read). Each of the six is anchored to its own disp32
// in echovr.exe.
//
// The two asserts stay, with their subject changed from "the number I derived" to "the three the
// registry hands me are still one contiguous vector, and still clear of the transient one". That is
// the property this code depends on and the one a future registry edit could break.
static_assert(facts::WORLD_REORIGIN_OFF_Y == facts::WORLD_REORIGIN_OFF_X + 4u &&
                  facts::WORLD_REORIGIN_OFF_Z == facts::WORLD_REORIGIN_OFF_X + 8u,
              "the cumulative world offset is no longer three contiguous floats");
static_assert(facts::WORLD_REORIGIN_PENDING_OFF_X >= facts::WORLD_REORIGIN_OFF_Z + 4u,
              "the pending triple overlaps the cumulative one; the two-triple reading is wrong");

constexpr uint32_t kOffX = facts::WORLD_REORIGIN_OFF_X;
constexpr uint32_t kOffY = facts::WORLD_REORIGIN_OFF_Y;
constexpr uint32_t kOffZ = facts::WORLD_REORIGIN_OFF_Z;

// CncaGame + 0x828 bit 9. CR15Game derives from CncaGame, so the flag word is read off the CR15Game
// singleton -- the same object and the same word GAME.FRAME_COUNTER_OFF is read from. The bit is set
// by RunLoop's inlined UpdateBeginTranslation (btr 8 / bts 9 at 0x14050BF05) and cleared by
// UpdateEndTranslation after it has accumulated the delta, so it is exactly "a re-origin is mid-apply".
constexpr uint64_t kTranslatePendingBit = 0x200ull;

// ★ MappedAddress IS A SYSCALL PATH, AND THIS RUNS INSIDE CAPTURE'S PER-TICK COST BRACKET.
//
// buildguard::MappedAddress validates the main image's PE headers through Readable(), and Readable
// calls VirtualQuery -- a syscall that takes the process address-space lock. ReadWorldOrigin resolves
// two cells and each resolution makes two of those calls, so this file was issuing FOUR VirtualQuery
// syscalls per tick, contending with every allocating thread in a game process whose VAD tree is
// enormous. S6.4's rule for the armed tick is "zero heap allocations, zero lock acquisitions, zero
// syscalls", and L2 (2026-09-03) killed a live capture on a window median of 2000-4506 us that the
// gather loops could not account for -- the file it wrote has players: 0, so those loops ran empty.
//
// The main image's base and SizeOfImage are fixed for the life of the process, so the RESOLUTION is
// done once per cell and the steady-state tick keeps only the two pointer loads. A cell that has not
// resolved yet is retried rather than latched, so a call made before the image is readable does not
// cache a permanent null. Both cells are resolved from the tick thread only; a benign racing write of
// the same value is the worst a second caller can do.
uint8_t* DerefSingleton(uint64_t pcVa, const void*& cachedCell) {
    if (cachedCell == nullptr) {
        cachedCell = buildguard::MappedAddress(pcVa);
    }
    const void* cell = cachedCell;
    if (cell == nullptr) {
        return nullptr;
    }
    uint8_t* p = *reinterpret_cast<uint8_t* const volatile*>(cell);
    if (p == nullptr || (reinterpret_cast<uintptr_t>(p) & 7u) != 0) {
        return nullptr;
    }
    return p;
}

float LoadFloat(const uint8_t* base, uint32_t off) {
    return *reinterpret_cast<const volatile float*>(base + off);
}

uint32_t Bits(float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof u);
    return u;
}

}  // namespace

WorldOriginSample ReadWorldOrigin() {
    WorldOriginSample s;

    static const void* rendererCell = nullptr;
    static const void* gameCell = nullptr;

    const uint8_t* renderer = DerefSingleton(facts::WORLD_REORIGIN_GLOBAL, rendererCell);
    if (renderer == nullptr) {
        return s;   // not booted, or not this build. valid stays false.
    }

    // ★ THE TRIPLE IS NOT WRITTEN ATOMICALLY. UpdateEndTranslation stores x, y and z with three
    // separate addss/movss pairs and no fence, and this runs on a different thread, so a read landing
    // between 0x140517FA7 and 0x140517FDF can see a half-updated vector. The window is a few
    // instructions on the one frame per session where a re-origin happens -- rare enough that it would
    // never be reproduced and never be explained, which is what makes it worth detecting rather than
    // hoping about.
    //
    // Two independent detectors, because they fail differently. The latch bit is the engine's own
    // statement that a translate is mid-apply, and it covers a read that happens to observe a
    // consistent-looking but stale triple. The double read catches a tear directly, including one that
    // lands while the bit is momentarily clear. Either one marks the sample suspect; a reader can then
    // distrust one keyframe instead of silently trusting a corrupt world offset.
    const float x0 = LoadFloat(renderer, kOffX);
    const float y0 = LoadFloat(renderer, kOffY);
    const float z0 = LoadFloat(renderer, kOffZ);

    bool latched = false;
    if (const uint8_t* game = DerefSingleton(facts::GAME_SINGLETON_VA, gameCell)) {
        const uint64_t flags =
            *reinterpret_cast<const volatile uint64_t*>(game + facts::WORLD_REORIGIN_FLAG_OFF);
        latched = (flags & kTranslatePendingBit) != 0;
    }

    const float x1 = LoadFloat(renderer, kOffX);
    const float y1 = LoadFloat(renderer, kOffY);
    const float z1 = LoadFloat(renderer, kOffZ);

    // Bit-compare, not float-compare: this asks "did these bytes change under me", and NaN != NaN
    // would report a tear on every frame if the engine ever parked a NaN here.
    const bool torn = Bits(x0) != Bits(x1) || Bits(y0) != Bits(y1) || Bits(z0) != Bits(z1);

    s.x = x1;
    s.y = y1;
    s.z = z1;
    s.valid = true;
    s.suspect = latched || torn;
    return s;
}

}  // namespace capture
}  // namespace replay
