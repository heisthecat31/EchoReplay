#pragma once

// MULTI-LOAD. Level hash -> the level's own name, for the browser's rows.
//
// ★ HARVESTED FROM THE SHIPPED EXE, NOT GUESSED. The engine stores each level id as the qword
// immediately before its "OlPrEfIx<name>" literal -- the same read tests/session/test_session.cpp's
// IdOfName makes, which is where `mpl_arena_a == 0x576ED3F8428EBC4B` is already pinned against the
// real binary (test_session.cpp:387). Every row below came out of that table in
// echovr.exe md5 caa05ee3bca2f5df0809994e93e2cc05, and tests/ui re-derives the one the whole tree
// already knows so a typo in this list cannot pass.
//
// An id that is not here prints as a hash and says so. A wrong name in this list would be worse
// than no name -- the row is what the owner picks a capture by.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace replay {
namespace ui {

struct LevelNameRow {
    uint64_t id;
    const char* name;
};

inline const LevelNameRow* LevelNameTable(size_t* count) {
    static const LevelNameRow kRows[] = {
        {0x576ED3F8428EBC4Bull, "mpl_arena_a"},
        {0x42670F2BED45703Cull, "mpl_combat_combustion"},
        {0x43E2DA7914642604ull, "mpl_combat_dyson"},
        {0xDF5CA7B7DFA383D4ull, "mpl_combat_fission"},
        {0x43E2DA7A0C623A19ull, "mpl_combat_gauss"},
        {0xDF4AABA6CEA689C9ull, "mpl_combat_pebbles"},
        {0x6DAA00A6D33D44B7ull, "mpl_lobby_b_arena"},
        {0x3C8D747C21EA8027ull, "mpl_tutorial_arena"},
        {0x3C8D74713CED8C3Full, "mpl_tutorial_lobby"},
        {0xA46F74BBC1F3AE6Aull, "mpl_tutorial_master"},
    };
    if (count != nullptr) *count = sizeof kRows / sizeof kRows[0];
    return kRows;
}

// Null when the id is not one this build knows.
inline const char* LevelName(uint64_t id) {
    size_t n = 0;
    const LevelNameRow* rows = LevelNameTable(&n);
    for (size_t i = 0; i < n; ++i) {
        if (rows[i].id == id) return rows[i].name;
    }
    return nullptr;
}

// What a browser row shows. `known` is false for a row whose manifest has not been read: a level
// column that printed "mpl_arena_a" for a file nobody opened would be the same lie the player and
// round counts already refuse to tell.
//
// Strips the `mpl_` prefix every level shares -- the column is short and the prefix carries no
// information -- and falls back to the low 32 bits of the hash, which is what the capture's own
// file name is built from and is therefore recognisable by eye.
inline void FormatLevel(bool known, uint64_t id, char* out, size_t cap) {
    if (cap == 0) return;
    if (!known) {
        snprintf(out, cap, "-");
        return;
    }
    const char* name = LevelName(id);
    if (name != nullptr) {
        const char* shown = name;
        if (name[0] == 'm' && name[1] == 'p' && name[2] == 'l' && name[3] == '_') shown = name + 4;
        snprintf(out, cap, "%s", shown);
        return;
    }
    snprintf(out, cap, "%08X", static_cast<unsigned>(id & 0xFFFFFFFFull));  // facts-ok: PHYS.ID_DEAD a u32 mask over a packed id; PHYS.ID_DEAD is PC_PRESENT and GOAL.USERID_UNSET is unmerged, so neither emits a constant
}

}  // namespace ui
}  // namespace replay
