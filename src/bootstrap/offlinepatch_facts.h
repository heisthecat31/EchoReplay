#pragma once

// The byte sites of EchoRelay's `-offline` patch, as they read in echovr.exe TimeDateStamp 0x6452DFF6
// (md5 caa05ee3bca2f5df0809994e93e2cc05). Every original was read from that file and every call and
// branch target decoded; tests/bootstrap/test_offlinepatch.cpp re-decodes each one against the registry.
//
// EchoRelay writes seven sites for -offline. Three of them (0x14017F77B `74 12`, 0x14017F817 and
// 0x14017F823 `90 90`) already hold the bytes it writes in this build, so they are not listed: a write
// of identical bytes is not a patch.

#include <stdint.h>

#include "generated/facts_generated.h"

namespace replay::offlinepatch::facts {

inline constexpr uint64_t kPreferredImageBase = ::facts::PE_IMAGE_BASE;
inline constexpr uint32_t kTimeDateStamp      = 0x6452DFF6u;

inline constexpr uint64_t kGameVtable = 0x1416D0FB0ull;  // facts-ok: GAME.VTABLE PC_PRESENT, so gen_facts emits no value
inline constexpr uint64_t kCliApply   = ::facts::CLI_APPLY;  // slot 0 of kGameVtable

inline constexpr uint32_t kGameFlagsOff = ::facts::GAME_FLAGS_OFF;
// The bit EchoRelay's -windowed ORs in before the command line is applied: "not in VR". CLI.APPLY
// itself reads it (0x140117F49), so it has to be set at the slot, not after.
inline constexpr uint64_t kWindowedBit = 0x100000ull;

struct Site {
    const char* name;
    uint64_t    va;
    uint8_t     len;
    const char* original;
    const char* patched;
    const char* purpose;
};

// Byte strings rather than integer arrays: they are opcodes, not positions.
inline constexpr Site kSites[] = {
    {"boot-offline-match", 0x1400FDE0Eull, 5, "\xE8\x5D\xFC\xFF\xFF", "\xE8\xCD\x02\x00\x00",
     "the boot-to-multiplayer arm calls SESSION.CREATE_OFFLINE_MATCH_LH instead of "
     "SESSION.BEGIN_MULTIPLAYER: host the -level locally, no matchmaking"},
    {"offline-host-branch", 0x14017F0B1ull, 2, "\x74\x0A", "\x75\x0A",
     "sub_14017F070: the offline local host takes the normal session path, not sub_14017F520"},
    {"login-failure-skip", 0x1401AC83Eull, 6, "\x0F\x85\x91\x00\x00\x00", "\xE9\x92\x00\x00\x00\x00",
     "sub_1401AC820: always jump past the 'Failed to connect to the login service' handling"},
    {"tutorial-redirect", 0x140A7C685ull, 5, "\xE8\x26\x18\x68\xFF", "\xE8\xD6\x17\x68\xFF",
     "menu action 3 calls sub_1400FDE60 (tutorial level through CREATE_OFFLINE_MATCH_LH) instead of "
     "sub_1400FDEB0, which ends the session and clears the offline mode bit first"},
};

inline constexpr size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);
inline constexpr uint8_t kMaxSiteLen = 6;

}  // namespace replay::offlinepatch::facts
