#pragma once

// THE DISC DIFF CENSUS -- the instrument for the owner's oldest complaint, taken in a LIVE match.
//
// "The initial spinning ring around the disc still is there for the full match. In a normal match
// it's there until the first grab then isn't there until reset."
//
// Seven packets tried to answer that from the offline image and from replay sessions alone, and
// every one of them ended at the same wall: in OUR session nothing ever hides the ring, so there is
// no transition to read. The live game has that transition. The same plugin DLL is loaded when the
// owner spectates a live match to record it, so the transition can be MEASURED instead of guessed
// -- provided the measurement is one a live session is allowed to make.
//
// ==============================================================================================
// WHY EVERY LINE OF THIS FILE IS A READ
// ==============================================================================================
// The binding live rule is that in a live session the plugin emits nothing special and attempts no
// playback: no engine writes, no engine function calls that can change state, no HTTP routes. This
// census therefore:
//
//   * calls NOTHING in the engine. Not one function pointer is taken, not one vtable slot is read
//     as a call target. Every field it reports is a load from a page that `PageSpanReadable` said
//     was readable a moment earlier. That is why this walk lives in its own file with its own
//     narrow interface instead of inside entity.cpp: a file with no engine calls in it can be
//     TESTED for having no engine calls in it, and tests/bootstrap does exactly that.
//   * takes no `livesafety::Feature`. An enumerator there is a promise that the path goes INERT in
//     a live session, and running in a live session is this path's entire purpose -- printing
//     `disccensus=inert(live-online)` for a path that still runs would be the vacuous row
//     tests/bootstrap/test_livesafety.cpp exists to forbid. This is the same reasoning, and the
//     same conclusion, as the live-safe `foley` line (ENG-AUDIO-FIX-2 item 3).
//   * asks the disc's SCRIPT nothing. The script variables `bubble` / `is_local_disc` /
//     `is_game_disc` / `fx_active` / `in_score_state` are reachable only through the script
//     module's own getter at module+240, which is a call into game code. The existing console
//     census makes that call and may keep making it, because a replay session is ours to
//     experiment in. A live match is not, so the getter is not called from here at any time, and
//     the script state this file reports is the graph's raw EXECUTION_DATA words -- copied, not
//     interpreted. A variable that changes changes a word, and the DIFF is what this census is
//     for: naming which word moved at the first grab is the finding, and the script DB can name
//     the word afterwards, offline, for free.
//
// Everything here is bounded: every loop has a hard cap that does not come from the data, every
// pointer hop is probed before it is followed, and no hop chain is longer than the four written
// down in Take(). The whole snapshot is taken on a TRIGGER -- first sight, a holder change, a disc
// respawn, or once per ten seconds -- never per frame. The per-frame cost is one probed dword per
// disc, read through a pointer the last snapshot resolved.

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace disccensus {

// R14NetPhysics CI+0x1C when nobody holds the body. RE-SWITCH-AND-DISC part 2 §3 reads the same
// dword through the engine's own comparison against dword_141FFCEA0.
inline constexpr uint32_t kUnowned = 0xFFFE0010u;  // facts-ok: ENTITY.OWNER_SYNTHETIC PC_PRESENT, so gen_facts emits no value

// One ncaScript component on a disc actor. The hash names the graph; the data words are the head of
// EXECUTION_DATA, which is where the graph's own variables and its per-delegate re-entrancy
// sentinels live. The layout is per-script and this plugin does not pretend to know it -- it
// reports the words and the diff reports which of them moved.
struct ScriptRow {
    uint64_t hash = 0;
    uint16_t ciIndex = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
    int32_t  state = -1;         // *(int*)(inst+224); 5 is what a checkpointed graph holds
    int64_t  graphTime = 0;      // EXECUTION_GRAPH::gametime
    static constexpr uint32_t kDataWords = 26;
    uint32_t data[kDataWords] = {};
    bool     dataRead = false;
};

// One actor node under a disc actor: what it is called and whether it is switched on. `bubble` --
// the spinning shell the owner is describing -- is one of these, on the actor the disc script's
// `bubble` variable points at.
//
// `enabled` is the READING, not the byte: the engine stores a DISABLE mask, so a flag byte of 0 is
// a node that is ON and this field is 1 for it. A ring that vanishes at the first grab is therefore
// `enabled 1->0` in a diff, which is the sentence the owner's report has to be answerable in.
struct NodeRow {
    uint64_t symbol = 0;       // the node's CSymbol64; SymbolHash("bubble") = 0x41D2C52818210B1B
    uint16_t index = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the node-absent sentinel
    uint8_t  enabled = 0;
    bool     read = false;
};

// One actor carrying an R14Frisbee component: the game disc and the local disc are both here, each
// in whichever game space it lives in.
struct DiscRow {
    uint64_t space = 0;
    uint64_t actorId = 0;          // {slot, pool, generation, pad} as the actor-index row holds it
    uint16_t frisbeeCi = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent sentinel
    uint32_t physId = 0;           // SR15FrisbeeCI+0x18
    float    timeSinceHeld = 0.f;  // SR15FrisbeeCI+120, the engine's own untouched timer
    // The holder, and the pointer the per-tick poll re-reads it through. `physCi` is a raw pointer
    // into the level's game space: it is revalidated by probe on every use and re-resolved by the
    // next snapshot, and it is never dereferenced across an unload without a probe.
    uint64_t physCi = 0;
    bool     holderRead = false;
    uint32_t holder = kUnowned;    // R14NetPhysics CI+0x1C
    uint16_t physFlags = 0;        // CI+6; bit 1 is the engine's IsHeld
    bool     nodeStateRead = false;
    uint16_t nodeState = 0;        // newest history node's state word; 0x200 is the remote held bit
    // The actor the id resolved to, and its node tree. `actor` is a raw pointer into the level, held
    // for the diff's benefit only -- it is never dereferenced outside the snapshot that resolved it.
    uint64_t actor = 0;
    uint16_t nodeCount = 0;        // nodes the actor definition declares; `nodes` is what fitted
    uint32_t nodes = 0;            // NodeRow entries for this disc, from `nodeFirst`
    uint32_t nodeFirst = 0;
    uint32_t scripts = 0;          // ScriptRow entries for this disc, from `scriptFirst`
    uint32_t scriptFirst = 0;
    // `bubble`: the actor+node the disc script (38965D90A823F03F) points at. RUN-VERIFY-2 §e read it
    // through the script module's getter and found it BOUND; this census reads the variable's own
    // storage in the graph's data instead, because a getter is a call and this path makes none.
    bool     refFound = false;
    uint64_t refActor = 0;         // the referenced actor id, in the same packed form as `actorId`
    uint64_t refNode = 0;          // the referenced node's CSymbol64
    uint16_t refIndex = 0xFFFFu;   // facts-ok: LOCAL.NO_COMPONENT the node-absent sentinel
    uint8_t  refEnabled = 0;
    bool     refEnabledRead = false;
};

// What Take() was given to walk. The caller names the game spaces; this file resolves nothing
// through the engine and therefore cannot find a space by itself, which is the point -- the one
// engine-dependent step stays with the entity surface that already owns it.
struct Input {
    static constexpr uint32_t kSpaceCap = 6;
    void*    spaces[kSpaceCap] = {};
    uint32_t nSpaces = 0;
};

struct Snapshot {
    static constexpr uint32_t kDiscCap   = 4;
    static constexpr uint32_t kScriptCap = 48;   // 12 per disc, which is the observed ten plus room
    static constexpr uint32_t kNodeCap   = 96;   // 24 per disc
    uint32_t  spacesWalked = 0;
    uint32_t  systemsSeen = 0;
    uint32_t  discsWritten = 0;
    uint32_t  scriptsWritten = 0;
    uint32_t  nodesWritten = 0;
    uint32_t  actorsResolved = 0;  // disc actor ids that became a CActor* by pure reads
    bool      taken = false;     // the walk ran; false means it never got a readable space
    DiscRow   discs[kDiscCap];
    ScriptRow scripts[kScriptCap];
    NodeRow   nodes[kNodeCap];
    // Why a disc's node rows are empty when they are. A census that prints nothing for a question it
    // never asked is indistinguishable from one that asked and found nothing, so the line carries
    // the reason the LAST disc gave rather than falling silent. Null once every disc walked.
    //
    // ★ THE CHAIN, all of it reads, so the safety review has one place to check it against.
    //   actorId -> CActor*  (ENG-LIVE-CENSUS-2B; the hop the first packet could not derive)
    //     rows  = *(u64*)(space+0x1F50)            [probe]   {u64 key, CActor* actor}, 16 B each
    //     count = *(u64*)(space+0x1F80)                      the same table's CTable count at +0x30
    //     and the same shape again at +0x1F90 / +0x1FC0, which is the engine's own fallback.
    //   The key is the packed actor id -- {u16 slot, u16 pool, u16 generation, u16 pad} -- which is
    //   exactly what the component systems' actor-index rows hold, so a STALE GENERATION matches no
    //   entry and the resolve refuses rather than handing back the slot's current tenant.
    //   (ENGINE.ACTOR_GUID_CONTAINER_LOOKUP sub_1404FEF80 bisects those two tables and nothing
    //    else; identity_access.cpp's HOP 1 spends the same pair and records that they live on the
    //    ROOT space, so this walk tries the given space and then its parent chain.)
    //   CActor* -> node rows
    //     def   = *(u64*)(actor+0x370)             [probe]   the actor definition
    //     N     = *(u64*)(actor+0x3A8)                       node count, cross-checked against the
    //                                                        definition's own *(u64*)(def+0xB0)
    //     flags = *(u64*)(actor+0x378)             [probe]   u8[N], a DISABLE mask: byte == 0 means
    //                                                        the node is ENABLED
    //     name  = *(u64*)(*(u64*)(def+0xB8) + 8*i), falling back to def+0x80 when that qword is -1
    //   (sub_1404FD650 writes actor+0x370/+0x378/+0x3A8 from def+0xB0 in one breath; IsNodeEnabled
    //    0x1404E4EA0 reads the flag byte and EnableActorNode 0x1404EDA50 clears bit 0 of it, which
    //    is why 0 is "enabled"; sub_1403195E0 renders a node index through the two name arrays.)
    const char* nodesRefused = nullptr;
};

// The walk. Read-only, bounded, engine-call-free. Returns false only when no space was readable.
bool Take(const Input& in, Snapshot& out);

// The per-frame poll: re-read each disc's holder dword through the pointer the snapshot resolved.
// One probed dword per disc, no walking. Returns true when any disc's holder differs from the
// snapshot's -- which is this census's whole trigger.
bool HolderMoved(const Snapshot& s, uint32_t* holders, uint32_t cap, uint32_t& n);

// The lines. Both write one NUL-terminated line and return its length; both truncate rather than
// overrun, because a diagnostic that can overrun a buffer is worse than no diagnostic.
// `Describe` is the full first-sight snapshot, `Diff` is what changed since `prev`. Diff returns 0
// -- and writes an empty string -- when nothing changed, so the caller can stay silent.
size_t Describe(const Snapshot& s, uint32_t discIndex, char* out, size_t cap);
size_t Diff(const Snapshot& prev, const Snapshot& cur, uint32_t discIndex, char* out, size_t cap);

// True when the two snapshots do not describe the same set of disc actors -- a goal respawn, a
// level load, or the first time a second disc appears. The caller prints a full Describe then
// rather than a diff against a disc that no longer exists.
bool PopulationChanged(const Snapshot& prev, const Snapshot& cur);

}  // namespace disccensus
}  // namespace replay
