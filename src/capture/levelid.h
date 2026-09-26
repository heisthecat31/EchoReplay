#pragma once

// W2-A. THE LEVEL ID THE CAPTURE HAS TO RECORD, AND WHY IT IS THIS ONE AND NOT ANY OTHER.
//
// `manifest.levelId` is not decoration. Playback's `ApplyRowset` compares every netvar rowset's
// owner word -- body[NETVAR.DUV_OWNER_OFF] -- against the id the capture recorded, and refuses the
// rowset when they differ; that comparison is the engine's own check, the one that stops one
// level's scores being written into another. So the value recorded here must be bit-for-bit the
// value the engine itself compares against, read from the same place the engine reads it:
//
//   NETVAR.RECEIVE_PATH (sub_140D3DF90), on the R15NetVar component system it receives in rcx:
//     0x140D3DFAD   mov r8, [rcx+80h]     <- kOwnerPtrOff, the component's owner object
//     0x140D3DFB4   mov rax, [r8+58h]     <- kOwnerIdOff, the loaded level id
//     0x140D3DFBC   cmp [rcx], rax        <- NETVAR.DUV_OWNER_OFF against exactly that u64
//
// and playback's EngineNetVarTable::OwnerId walks the same two hops (netvarapply.h
// kNetVarOwnerPtrOff / kNetVarOwnerIdOff). A capture that records anything else -- a pointer, a
// hash, a counter -- records a value every rowset will be refused against, and the refusal would
// look exactly like a bound table doing its job.
//
// The component system itself is found the way every component system in this plugin is found, and
// the answer is CHECKED: `CS.LOOKUP_BY_SYMBOL(space, NETVAR.CS_SYMBOL)` in the LEVEL space
// (netGame+NETGAME_SPECTATORCAM_CS_OFF) first, then the CS space, and a candidate whose
// NETVAR_LEVEL_NETGAME_OFF slot is not the netGame we walked from is refused by name rather than
// read. Same order, same check, same refusal vocabulary as playback's LocateNetVarSite -- if the
// two walks disagreed about which component is this session's, the id would be read off one object
// and compared against another.
//
// EVERY hop is span-proved (`Readable`) before it is dereferenced, and NOTHING here refuses a
// capture: an unreadable chain records 0 and says which hop failed. A capture with no level id is
// a capture whose netvar rowsets playback will leave unbound; a capture that never happened is a
// match lost.

#include <stdint.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "generated/facts_generated.h"

namespace replay {
namespace capture {

// `CS.LOOKUP_BY_SYMBOL`. Injected rather than resolved inside the walk so a test can drive the
// shipped code over its own memory.
using FindCsFn = void* (*)(void* space, uint64_t symbol);

// Which hop answered, so a zero id is never just "unavailable".
enum class LevelIdWhy : uint8_t {
    NoNetGame = 0,     // no CR15NetGame -- no session to read a level out of
    NoSpace,           // the CR15NetGame carries neither game space yet
    NoLookup,          // CS.LOOKUP_BY_SYMBOL failed its prologue guard, so it was not called
    NoNetVarCs,        // the lookup answered null in every space asked
    NetGameMismatch,   // a component answered, and its +0xB8 is not the netGame we walked from
    NoOwner,           // the component carries no owner object at +0x80 yet
    OwnerUnreadable,   // the owner is there and its +0x58 is not committed memory
    Ok,
};

struct LevelIdRead {
    uint64_t id = 0;
    LevelIdWhy why = LevelIdWhy::NoNetGame;
    void* netVarCs = nullptr;
    bool fromLevelSpace = false;
};

inline const char* LevelIdWhyText(LevelIdWhy w) {
    switch (w) {
        case LevelIdWhy::NoNetGame:       return "no CR15NetGame -- no session is loaded";
        case LevelIdWhy::NoSpace:         return "the CR15NetGame carries no game space yet";
        case LevelIdWhy::NoLookup:        return "CS.LOOKUP_BY_SYMBOL failed its prologue guard";
        case LevelIdWhy::NoNetVarCs:      return "no component answered NETVAR.CS_SYMBOL";
        case LevelIdWhy::NetGameMismatch: return "the component that answered belongs to another "
                                                 "CR15NetGame";
        case LevelIdWhy::NoOwner:         return "the netvar component carries no owner object yet";
        case LevelIdWhy::OwnerUnreadable: return "the owner object's level id is not readable";
        case LevelIdWhy::Ok:              return "read";
    }
    return "unstated";
}

// The two hops off the R15NetVar component system, named here rather than spelled at the use site.
// Both are read off NETVAR.RECEIVE_PATH's own owner compare (the VAs in the header comment) and
// both are the offsets playback already ships in netvarapply.h.
inline constexpr uint32_t kOwnerPtrOff = facts::NETVAR_OWNER_PTR_OFF;
inline constexpr uint32_t kOwnerIdOff = facts::NETVAR_OWNER_ID_OFF;

inline void* DerefGuarded(void* base, uint32_t off) {
    if (!base) return nullptr;
    const uint8_t* p = static_cast<const uint8_t*>(base) + off;
    if (!Readable(p, sizeof(void*))) return nullptr;
    void* v = nullptr;
    memcpy(&v, p, sizeof v);
    return v;
}

// The walk. Pure: everything it touches arrives in its arguments.
inline LevelIdRead ReadLevelIdFrom(void* netGame, FindCsFn find) {
    LevelIdRead r;
    if (!netGame) return r;

    void* const spaces[2] = {DerefGuarded(netGame, facts::NETGAME_SPECTATORCAM_CS_OFF),
                             DerefGuarded(netGame, facts::NETGAME_CS_SPACE_OFF)};
    if (!spaces[0] && !spaces[1]) {
        r.why = LevelIdWhy::NoSpace;
        return r;
    }
    if (!find) {
        r.why = LevelIdWhy::NoLookup;
        return r;
    }

    r.why = LevelIdWhy::NoNetVarCs;
    for (int i = 0; i < 2; ++i) {
        void* const space = spaces[i];
        if (!space) continue;
        if (i == 1 && space == spaces[0]) continue;  // one space in both slots: already asked

        void* const cs = find(space, facts::NETVAR_CS_SYMBOL);
        if (!cs) continue;

        if (DerefGuarded(cs, facts::NETVAR_LEVEL_NETGAME_OFF) != netGame) {
            r.why = LevelIdWhy::NetGameMismatch;
            continue;
        }

        r.netVarCs = cs;
        r.fromLevelSpace = (i == 0);

        void* const owner = DerefGuarded(cs, kOwnerPtrOff);
        if (!owner) {
            r.why = LevelIdWhy::NoOwner;
            return r;
        }
        const uint8_t* const idAt = static_cast<const uint8_t*>(owner) + kOwnerIdOff;
        if (!Readable(idAt, sizeof(uint64_t))) {
            r.why = LevelIdWhy::OwnerUnreadable;
            return r;
        }
        memcpy(&r.id, idAt, sizeof r.id);
        r.why = LevelIdWhy::Ok;
        return r;
    }
    return r;
}

// Resolves CS.LOOKUP_BY_SYMBOL against the loaded image, once. A site that fails its prologue guard
// yields a null find, which the walk reports as NoLookup rather than calling into whatever is there.
inline FindCsFn LiveFindCs() {
    static bool asked = false;
    static const void* site = nullptr;
    if (!asked) {
        asked = true;
        if (buildguard::VerifySite("CS.LOOKUP_BY_SYMBOL", facts::CS_LOOKUP_BY_SYMBOL,
                                   facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE,
                                   sizeof facts::CS_LOOKUP_BY_SYMBOL_PROLOGUE)) {
            site = buildguard::MappedAddress(facts::CS_LOOKUP_BY_SYMBOL);
        }
    }
    return reinterpret_cast<FindCsFn>(const_cast<void*>(site));
}

// What the open path calls: fills `out` with the id, or leaves it 0 and hands back the hop that
// refused so the caller can say so in one line. Returns true when `out` carries a real id.
inline bool FillLevelId(uint64_t& out, void* netGame, FindCsFn find, LevelIdRead& r) {
    r = ReadLevelIdFrom(netGame, find);
    if (r.why != LevelIdWhy::Ok) return false;
#if !defined(W2A_MUTATE_NO_LEVELID)
    // Mutation control: dropping this one assignment is exactly the pre-fix state -- every file on
    // disk carrying manifest.levelId == 0 with nothing else looking wrong.
    out = r.id;
#endif
    return true;
}

}  // namespace capture
}  // namespace replay
