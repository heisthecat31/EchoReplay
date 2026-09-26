// The disc diff census. See disccensus.h for why every line of this file is a read.
//
// THE SHAPE OF THE WALK, so the safety review has something to check against:
//
//   space                       (given by the caller; probed)
//     +0x2C8 systems count, +0x2C0 system table          -> <= kSystemScanMax entries, 16 B each
//       entry+0 symbol, entry+8 component system          -> the three systems this census wants
//   R14Frisbee system
//     +0x110 actor index, +0x118 rows                     -> <= kRowScanMax rows, 24 B each
//       row+0 actorId, row+16 component index
//     +0xF8 CI pool -> +560*ci                            -> +0x18 physId, +120 timeSinceHeld
//   R14NetPhysics system
//     +0xC8 handle map -> +4*physId                       -> element index
//     +0xF8 CI pool -> +504*index                         -> +0x1C holder, +4 flags
//       +0x158 history list -> +16 newest node            -> +0x20 state word (0x200 = held)
//   ncaScript system
//     +0x110 actor index                                  -> rows whose actorId is a disc actor
//     +248 CI pool -> +184*ci -> +24 instance             -> +8 module, +224 state
//       instance+24 graph (embedded)                      -> +88 gametime, +104 EXECUTION_DATA
//
//   space (and, failing that, its parents up to kSpaceParentWalkMax)
//     +0x1F50 rows, +0x1F80 count                       -> {u64 actorId, CActor*}, 16 B each
//     +0x1F90 rows, +0x1FC0 count                       -> the engine's own fallback pair
//   CActor*
//     +0x370 definition -> +0xB0 node count (cross-check), +0xB8/+0x80 the two name arrays
//     +0x3A8 node count, +0x378 flag bytes              -> byte == 0 means the node is ENABLED
//
// Five hops is the longest chain, every one of them probed immediately before it is followed. No
// loop bound comes from the data alone: every count is capped by a constant in this file.

#include "engine/disccensus.h"

#include <stdio.h>
#include <string.h>

#include "bootstrap/pageprobe.h"
#include "engine/localpawn_facts.h"
#include "foundation/gametype.h"
#include "generated/facts_generated.h"

namespace replay {
namespace disccensus {
namespace {

namespace lp = replay::localpawn_facts;
namespace facts = ::facts;   // the generated registry is a top-level namespace, not replay::facts

// Hard caps. The engine's counts are read from memory that may be mid-write or simply wrong, and a
// loop that trusts one is a hang or a fault, not a diagnostic.
constexpr uint64_t kSystemScanMax = 4096;
constexpr uint64_t kRowScanMax    = 8192;  // facts-ok: PHYS.CI_PAUSED_BIT a row-count cap, not a physics flag bit
constexpr uint32_t kCensusStride  = 24;   // {u64 actorId, u64 systemSymbol, u16 componentIndex}
constexpr uint32_t kListHeadNodeOff = 16;
constexpr uint32_t kScriptsPerDisc = 12;

// ---- actorId -> CActor* ------------------------------------------------------------------------
// The engine's own resolve, ENGINE.ACTOR_GUID_CONTAINER_LOOKUP sub_1404FEF80, is a pure read and
// this is its whole body: a search over {u64 key, CActor* actor} rows at space+0x1F50 bounded by
// the CTable count 0x30 bytes further on, then the identical pair at +0x1F90. identity_access.cpp's
// HOP 1 spends the same two tables and records (ARCH-23 S14.4) that they live on the ROOT space
// rather than on the child the component systems hang off -- hence the parent walk below.
constexpr uint32_t kActorTableOff[2]   = {0x1F50u, 0x1F90u};
constexpr uint32_t kActorTableCountOff = 0x30u;
constexpr uint32_t kActorTableStride   = 16u;
constexpr uint32_t kActorTableActorOff = 8u;
constexpr uint64_t kActorRowScanMax    = 65536u;

// ---- CActor* -> its nodes ----------------------------------------------------------------------
// sub_1404FD650 builds an actor from its definition and writes all four of these in one breath:
// actor+0x370 = def, actor+0x3A8 = *(u64*)(def+0xB0) = the node count, and the bytes at
// *(u64*)(actor+0x378) are filled one per node from the definition's own mask. IsNodeEnabled
// sub_1404E4EA0 answers `byte == 0` and EnableActorNode sub_1404EDA50 clears bit 0 of that byte, so
// the stored byte is a DISABLE mask and 0 is the node being ON. sub_1403195E0 renders a node index
// as *(u64*)(*(u64*)(def+0xB8) + 8*i), falling back to def+0x80 when that qword reads -1.
constexpr uint32_t kActorDefOff          = 0x370u;  // facts-ok: GSPACE.CLASS_TABLE_OFF a field of a CActor, not of the game space that fact belongs to
constexpr uint32_t kActorNodeFlagsOff    = 0x378u;  // facts-ok: PHYS.BODY_CLASS_OFF a field of a CActor, not of a net-physics component
constexpr uint32_t kActorNodeCountOff    = 0x3A8u;
constexpr uint32_t kActorDefNodeCountOff = 0xB0u;
constexpr uint32_t kActorDefNamesOff     = 0x80u;
constexpr uint32_t kActorDefAltNamesOff  = 0xB8u;
constexpr uint64_t kNodeScanMax          = 4096;
constexpr uint32_t kNodesPerDisc         = 24;
constexpr uint64_t kNoSymbol             = ~0ull;   // the engine's own "this node has no name"
constexpr uint32_t kNoNodeIndex          = ~0u;   // this file's own "no such node", not an engine value

// ---- the disc script's `bubble` ----------------------------------------------------------------
// entity.cpp's kDiscScriptHash: the graph that owns the ring. Its `bubble` variable is an
// actor_var_t -- {u64 space, u64 actorId, u64 nodeSymbol} -- living in EXECUTION_DATA, and the
// console census reaches it through the script module's getter. A getter is a call, so this file
// finds the same 24 bytes by their content instead: a candidate is only believed when its actor id
// resolves through the table above AND the node symbol beside it names a node on THAT actor. Two
// independent matches against live engine data is not something a neighbouring float produces.
constexpr uint64_t kDiscScriptHash = 0x38965D90A823F03Full;
constexpr uint32_t kVarScanBytes   = 512;   // a horizon, not a floor: reported when nothing is found
constexpr uint32_t kRefResolveMax  = 64;    // table searches one graph may cost

const char* const kWhyNoTable = "no {actor id -> CActor*} table on the space chain holds this id";
const char* const kWhyNoNodes = "the actor resolved but its node arrays did not read back";

// Derived, not transcribed: the same SymbolHash the rest of the plugin spends, pinned against the
// one R14Frisbee value that WAS transcribed from the image years before SymbolHash existed. A hash
// that stopped agreeing with the image fails the build instead of walking the wrong system.
constexpr uint64_t kSymNcaScript = replay::gametype::SymbolHash("ncaScript");
static_assert(replay::gametype::SymbolHash("R14Frisbee") == lp::kSymR14Frisbee,
              "SymbolHash no longer reproduces the image's own R14Frisbee symbol");

inline void* At(void* p, uint32_t off) { return static_cast<uint8_t*>(p) + off; }

inline bool Probe(const void* p, size_t len) { return replay::PageSpanReadable(p, len); }

template <typename T>
inline T Rd(const void* p, uint32_t off) {
    T v{};
    memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof(T));
    return v;
}

// A pointer field, read only after its own page was proved readable.
inline void* Deref(void* p, uint32_t off) {
    if (p == nullptr || !Probe(At(p, off), sizeof(void*))) return nullptr;
    return Rd<void*>(p, off);
}

// One of the two actor tables, already bounded and already probed for every row it claims.
struct ActorTable {
    const uint8_t* rows = nullptr;
    uint64_t count = 0;
};

bool LoadActorTable(void* space, uint32_t off, ActorTable& out) {
    if (space == nullptr || !Probe(At(space, off), kActorTableCountOff + sizeof(uint64_t))) {
        return false;
    }
    void* const rows = Rd<void*>(space, off);
    const uint64_t count = Rd<uint64_t>(space, off + kActorTableCountOff);
    if (rows == nullptr || count == 0 || count > kActorRowScanMax) return false;
    if (!Probe(rows, static_cast<size_t>(count) * kActorTableStride)) return false;
    out.rows = static_cast<const uint8_t*>(rows);
    out.count = count;
    return true;
}

// Linear where the engine bisects, and deliberately. The engine knows whether its own table is
// currently sorted -- its lookup sorts it in place when it is not -- and a reader does not; a
// bisection of an unsorted table misses silently, which is the one failure a census must not have.
// The whole span was probed once above, so this costs no probe per row.
void* ResolveActor(const ActorTable& t, uint64_t id) {
    if (t.rows == nullptr || id == 0) return nullptr;
    for (uint64_t i = 0; i < t.count; ++i) {
        const uint8_t* const row = t.rows + kActorTableStride * i;
        if (Rd<uint64_t>(row, 0) == id) return Rd<void*>(row, kActorTableActorOff);
    }
    return nullptr;
}

// Which space holds the table. The tables live on the ROOT, so the space the caller named is tried
// first and then its parents; the id match itself says which one was right, so this makes no guess
// about how deep the caller's space sits. The depth bound is what stops a cyclic parent field.
bool FindActorTable(void* space, uint64_t id, ActorTable& out) {
    void* s = space;
    for (uint32_t depth = 0; s != nullptr && depth < lp::kSpaceParentWalkMax; ++depth) {
        for (uint32_t k = 0; k < 2; ++k) {
            ActorTable t;
            if (LoadActorTable(s, kActorTableOff[k], t) && ResolveActor(t, id) != nullptr) {
                out = t;
                return true;
            }
        }
        s = Deref(s, lp::kGameSpaceParentOff);
    }
    return false;
}

const uint64_t* SymbolArray(void* def, uint32_t off, uint64_t n) {
    void* const p = Deref(def, off);
    if (p == nullptr || !Probe(p, static_cast<size_t>(n) * sizeof(uint64_t))) return nullptr;
    return static_cast<const uint64_t*>(p);
}

// An actor's node tree as three arrays. Either name array may be absent without costing the flags:
// the enabled bit is the finding, and a node with no readable name is still a node that moved.
struct NodeWalk {
    const uint8_t*  flags = nullptr;
    const uint64_t* names = nullptr;
    const uint64_t* alt = nullptr;
    uint64_t count = 0;
};

bool LoadNodes(void* actor, NodeWalk& w) {
    if (actor == nullptr || !Probe(At(actor, kActorDefOff), sizeof(void*)) ||
        !Probe(At(actor, kActorNodeFlagsOff), sizeof(void*)) ||
        !Probe(At(actor, kActorNodeCountOff), sizeof(uint64_t))) {
        return false;
    }
    const uint64_t n = Rd<uint64_t>(actor, kActorNodeCountOff);
    if (n == 0 || n > kNodeScanMax) return false;
    void* const def = Rd<void*>(actor, kActorDefOff);
    if (def == nullptr || !Probe(At(def, kActorDefNodeCountOff), sizeof(uint64_t))) return false;
    // The definition's count against the actor's copy of it. The engine writes the two together, so
    // a disagreement is an object that is not an actor, or one caught mid-build -- either way not
    // something to index a flag array with.
    if (Rd<uint64_t>(def, kActorDefNodeCountOff) != n) return false;
    void* const flags = Rd<void*>(actor, kActorNodeFlagsOff);
    if (flags == nullptr || !Probe(flags, static_cast<size_t>(n))) return false;
    w.flags = static_cast<const uint8_t*>(flags);
    w.count = n;
    w.names = SymbolArray(def, kActorDefNamesOff, n);
    w.alt = SymbolArray(def, kActorDefAltNamesOff, n);
    return true;
}

uint64_t NodeSymbol(const NodeWalk& w, uint64_t i) {
    if (w.alt != nullptr && w.alt[i] != kNoSymbol) return w.alt[i];
    if (w.names != nullptr && w.names[i] != kNoSymbol) return w.names[i];
    return 0;
}

uint32_t NodeIndexOf(const NodeWalk& w, uint64_t symbol) {
    if (symbol == 0 || symbol == kNoSymbol) return kNoNodeIndex;
    for (uint64_t i = 0; i < w.count; ++i) {
        if (NodeSymbol(w, i) == symbol) return static_cast<uint32_t>(i);
    }
    return kNoNodeIndex;
}

inline uint8_t EnabledFromMask(uint8_t flagByte) { return flagByte == 0 ? 1u : 0u; }

// The node rows for one disc. `nodeCount` keeps what the actor declares even when only the first
// kNodesPerDisc of them fit, so a truncated row set cannot read as a complete one.
const char* ReadNodes(const ActorTable& table, Snapshot& out, DiscRow& row) {
    void* const actor = ResolveActor(table, row.actorId);
    if (actor == nullptr) return kWhyNoTable;
    row.actor = reinterpret_cast<uint64_t>(actor);
    ++out.actorsResolved;
    NodeWalk w;
    if (!LoadNodes(actor, w)) return kWhyNoNodes;
    // No clamp: kNodeScanMax is what already keeps this inside a u16, and a clamp here would be a
    // second, quieter bound that could disagree with it.
    static_assert(kNodeScanMax <= UINT16_MAX, "a node count must fit the field that reports it");
    row.nodeCount = static_cast<uint16_t>(w.count);
    for (uint64_t i = 0; i < w.count && row.nodes < kNodesPerDisc &&
                         out.nodesWritten < Snapshot::kNodeCap;
         ++i) {
        if (row.nodes == 0) row.nodeFirst = out.nodesWritten;
        NodeRow& nd = out.nodes[out.nodesWritten++];
        ++row.nodes;
        nd.index = static_cast<uint16_t>(i);
        nd.symbol = NodeSymbol(w, i);
        nd.enabled = EnabledFromMask(w.flags[i]);
        nd.read = true;
    }
    return nullptr;
}

// `bubble`, found by what it points at rather than by an offset nobody has derived. See
// kDiscScriptHash above for why two independent matches are the test.
void ReadActorRef(const ActorTable& table, const void* data, DiscRow& disc) {
    if (data == nullptr || disc.refFound) return;
    const size_t span = kVarScanBytes + lp::kActorVarSize;
    if (!Probe(data, span)) return;
    uint32_t resolves = 0;
    for (uint32_t o = 0; o <= kVarScanBytes && resolves < kRefResolveMax; o += 8) {
        const uint64_t space = Rd<uint64_t>(data, o + lp::kActorVarSpaceOff);
        const uint64_t id = Rd<uint64_t>(data, o + lp::kActorVarActorOff);
        const uint64_t node = Rd<uint64_t>(data, o + lp::kActorVarNodeOff);
        // The shape first, because the shape is free: an actor_var names a space, carries a packed
        // actor id whose top word is the struct's own padding, and ends in a node symbol.
        if (space == 0 || id == 0 || (id >> 48) != 0 || node == 0 || node == kNoSymbol) continue;
        ++resolves;
        void* const actor = ResolveActor(table, id);
        NodeWalk w;
        if (actor == nullptr || !LoadNodes(actor, w)) continue;
        const uint32_t idx = NodeIndexOf(w, node);
        if (idx == kNoNodeIndex) continue;
        disc.refFound = true;
        disc.refActor = id;
        disc.refNode = node;
        disc.refIndex = static_cast<uint16_t>(idx);
        disc.refEnabled = EnabledFromMask(w.flags[idx]);
        disc.refEnabledRead = true;
        return;
    }
}

// The three systems one game space publishes that this census needs. A space that has none of them
// is not the disc's space and costs one table walk to rule out.
struct SpaceSystems {
    void* frisbee = nullptr;
    void* phys = nullptr;
    void* script = nullptr;
    uint32_t seen = 0;
};

bool FindSystems(void* space, SpaceSystems& out) {
    if (space == nullptr || !Probe(space, lp::kSystemTableCountOff + sizeof(uint64_t))) return false;
    const uint64_t systems = Rd<uint64_t>(space, lp::kSystemTableCountOff);
    void* const table = Deref(space, lp::kGameSpaceSystemTableOff);
    if (table == nullptr || systems == 0 || systems > kSystemScanMax) return false;
    for (uint64_t i = 0; i < systems; ++i) {
        void* const entry = At(table, static_cast<uint32_t>(lp::kSystemTableStride * i));
        if (!Probe(entry, static_cast<size_t>(lp::kSystemTableStride))) break;
        ++out.seen;
        const uint64_t symbol = Rd<uint64_t>(entry, 0);
        if (symbol == lp::kSymR14Frisbee && out.frisbee == nullptr) {
            out.frisbee = Deref(entry, lp::kSystemTableEntryCsOff);
        } else if (symbol == facts::CS_SYM_R14NETPHYSICS && out.phys == nullptr) {
            out.phys = Deref(entry, lp::kSystemTableEntryCsOff);
        } else if (symbol == kSymNcaScript && out.script == nullptr) {
            out.script = Deref(entry, lp::kSystemTableEntryCsOff);
        }
    }
    return true;
}

// The holder, and the two flag words beside it. This is the read the whole trigger hangs on:
// PhysCI+0x1C is the owner dword the engine's own possession gates compare against their NONE
// sentinel, and the newest history node's state word carries the remote arm's held bit.
void ReadHolder(void* physCs, DiscRow& row) {
    void* const map = Deref(physCs, facts::PHYS_CS_HANDLE_MAP_OFF);
    void* const base = Deref(physCs, facts::PHYS_CS_POOL_BASE_OFF);
    if (map == nullptr || base == nullptr) return;
    if (!Probe(At(map, 4u * row.physId), sizeof(uint32_t))) return;
    const uint32_t element = Rd<uint32_t>(map, 4u * row.physId);
    if (element == lp::kNoComponent) return;
    void* const ci = At(base, facts::PHYS_CI_STRIDE * element);
    if (!Probe(ci, facts::PHYS_CI_STRIDE)) return;
    row.physCi = reinterpret_cast<uint64_t>(ci);
    row.holder = Rd<uint32_t>(ci, facts::PHYS_CI_OWNER_OFF);
    row.physFlags = Rd<uint16_t>(ci, facts::PHYS_CI_FLAGS_OFF);
    row.holderRead = true;

    void* const list = Deref(ci, facts::PHYS_CI_LIST_HEAD_OFF);
    void* const head = list != nullptr ? Deref(list, kListHeadNodeOff) : nullptr;
    if (head != nullptr && Probe(At(head, facts::PHYS_NODE_STATE_OFF), sizeof(uint16_t))) {
        row.nodeState = Rd<uint16_t>(head, facts::PHYS_NODE_STATE_OFF);
        row.nodeStateRead = true;
    }
}

// Every ncaScript component on the disc actors already written. One walk of the script system's
// actor index serves all of them, because the index is keyed by actor and the discs are few.
void ReadScripts(void* scriptCs, Snapshot& out, uint32_t firstDisc, const ActorTable* table) {
    if (scriptCs == nullptr || !Probe(scriptCs, lp::kCsActorIndexCountOff + sizeof(uint64_t))) return;
    const uint64_t rows = Rd<uint64_t>(scriptCs, lp::kCsActorIndexCountOff);
    void* const index = Deref(scriptCs, lp::kCsActorIndexOff);
    void* const pool = Deref(scriptCs, lp::kScriptCsPoolBaseOff);
    if (index == nullptr || pool == nullptr || rows == 0 || rows > kRowScanMax) return;
    for (uint64_t r = 0; r < rows; ++r) {
        const void* element = At(index, static_cast<uint32_t>(kCensusStride * r));
        if (!Probe(element, kCensusStride)) break;
        const uint64_t actorId = Rd<uint64_t>(element, 0);
        DiscRow* disc = nullptr;
        for (uint32_t d = firstDisc; d < out.discsWritten && disc == nullptr; ++d) {
            if (out.discs[d].actorId == actorId) disc = &out.discs[d];
        }
        if (disc == nullptr || disc->scripts >= kScriptsPerDisc ||
            out.scriptsWritten >= Snapshot::kScriptCap) {
            continue;
        }
        const uint16_t ciIndex = Rd<uint16_t>(element, 2u * sizeof(uint64_t));
        if (ciIndex == lp::kNoComponent) continue;
        void* const ci = At(pool, lp::kScriptCiStride * uint32_t(ciIndex));
        if (!Probe(ci, lp::kScriptCiStride)) continue;
        void* const inst = Deref(ci, lp::kScriptCiInstanceOff);
        if (inst == nullptr || !Probe(inst, lp::kScriptInstStateOff + sizeof(int32_t))) continue;
        void* const module = Deref(inst, lp::kScriptInstModuleOff);
        if (module == nullptr || !Probe(module, lp::kScriptModuleHashOff + sizeof(uint64_t))) {
            continue;
        }
        if (disc->scripts == 0) disc->scriptFirst = out.scriptsWritten;
        ScriptRow& s = out.scripts[out.scriptsWritten++];
        ++disc->scripts;
        s.ciIndex = ciIndex;
        s.hash = Rd<uint64_t>(module, lp::kScriptModuleHashOff);
        s.state = Rd<int32_t>(inst, lp::kScriptInstStateOff);
        // The graph is embedded at inst+24, not pointed to; EXECUTION_DATA hangs off it.
        void* const graph = At(inst, lp::kScriptInstGraphOff);
        if (Probe(At(graph, lp::kScriptGraphGameTimeOff), sizeof(int64_t))) {
            s.graphTime = Rd<int64_t>(graph, lp::kScriptGraphGameTimeOff);
        }
        void* const data = Deref(graph, lp::kScriptGraphDataOff);
        if (data != nullptr && Probe(data, sizeof s.data)) {
            memcpy(s.data, data, sizeof s.data);
            s.dataRead = true;
        }
        // Only the disc's own graph is searched for the ring reference. Every other script on the
        // level would cost the same table walks to find nothing.
        if (table != nullptr && s.hash == kDiscScriptHash) ReadActorRef(*table, data, *disc);
    }
}

uint32_t DataDigest(const ScriptRow& s) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < ScriptRow::kDataWords; ++i) {
        h = (h ^ s.data[i]) * 16777619u;
    }
    return h;
}

// snprintf into a cursor that never moves past the end. Every writer below goes through this, so a
// line that runs out of room is truncated at a field boundary instead of overrunning.
struct Line {
    char* p;
    size_t cap;
    size_t n = 0;
    template <typename... A>
    void Add(const char* fmt, A... a) {
        if (n + 1 >= cap) return;
        const int wrote = snprintf(p + n, cap - n, fmt, a...);
        if (wrote <= 0) return;
        n += static_cast<size_t>(wrote) < cap - n ? static_cast<size_t>(wrote) : cap - n - 1;
    }
};

}  // namespace

bool Take(const Input& in, Snapshot& out) {
    out = Snapshot();
    for (uint32_t i = 0; i < in.nSpaces && i < Input::kSpaceCap; ++i) {
        void* const space = in.spaces[i];
        if (space == nullptr) continue;
        // A space named twice by the caller's candidate list would double every disc row.
        bool repeat = false;
        for (uint32_t k = 0; k < i && !repeat; ++k) repeat = in.spaces[k] == space;
        if (repeat) continue;

        SpaceSystems sys;
        const bool walked = FindSystems(space, sys);
        out.systemsSeen += sys.seen;
        if (!walked) continue;
        ++out.spacesWalked;
        out.taken = true;
        if (sys.frisbee == nullptr) continue;

        const uint32_t firstDisc = out.discsWritten;
        if (!Probe(sys.frisbee, lp::kCsActorIndexCountOff + sizeof(uint64_t))) continue;
        const uint64_t rows = Rd<uint64_t>(sys.frisbee, lp::kCsActorIndexCountOff);
        void* const index = Deref(sys.frisbee, lp::kCsActorIndexOff);
        void* const pool = Deref(sys.frisbee, lp::kFrisbeeCsPoolBaseOff);
        if (index == nullptr || rows == 0 || rows > kRowScanMax) continue;
        for (uint64_t r = 0; r < rows && out.discsWritten < Snapshot::kDiscCap; ++r) {
            const void* element = At(index, static_cast<uint32_t>(kCensusStride * r));
            if (!Probe(element, kCensusStride)) break;
            DiscRow& row = out.discs[out.discsWritten++];
            row.space = reinterpret_cast<uint64_t>(space);
            row.actorId = Rd<uint64_t>(element, 0);
            row.frisbeeCi = Rd<uint16_t>(element, 2u * sizeof(uint64_t));
            if (pool == nullptr || row.frisbeeCi == lp::kNoComponent) continue;
            void* const ci = At(pool, lp::kFrisbeeCiStride * uint32_t(row.frisbeeCi));
            if (!Probe(ci, lp::kFrisbeeCiStride)) continue;
            row.physId = Rd<uint32_t>(ci, lp::kFrisbeeCiPhysIdOff);
            row.timeSinceHeld = Rd<float>(ci, lp::kFrisbeeCiTimeSinceHeldOff);
            if (sys.phys != nullptr) ReadHolder(sys.phys, row);
        }
        // The actor table is chosen by the first disc id it can answer, and then serves every
        // resolve this space needs: the discs' own node trees and whatever `bubble` points at.
        ActorTable table;
        bool haveTable = false;
        for (uint32_t d = firstDisc; d < out.discsWritten && !haveTable; ++d) {
            haveTable = FindActorTable(space, out.discs[d].actorId, table);
        }
        for (uint32_t d = firstDisc; d < out.discsWritten; ++d) {
            const char* why = haveTable ? ReadNodes(table, out, out.discs[d]) : kWhyNoTable;
            if (why != nullptr) out.nodesRefused = why;
        }
        ReadScripts(sys.script, out, firstDisc, haveTable ? &table : nullptr);
    }
    return out.taken;
}

bool HolderMoved(const Snapshot& s, uint32_t* holders, uint32_t cap, uint32_t& n) {
    n = 0;
    bool moved = false;
    for (uint32_t i = 0; i < s.discsWritten && i < cap; ++i) {
        const DiscRow& row = s.discs[i];
        uint32_t holder = kUnowned;
        void* const ci = reinterpret_cast<void*>(static_cast<uintptr_t>(row.physCi));
        // The CI pointer belongs to the previous snapshot. It is probed here, every time, because
        // the level it points into can be torn down between two ticks; a freed page refuses and the
        // poll reports the unowned sentinel, which the next snapshot corrects.
        if (row.physCi != 0 && Probe(At(ci, facts::PHYS_CI_OWNER_OFF), sizeof(uint32_t))) {
            holder = Rd<uint32_t>(ci, facts::PHYS_CI_OWNER_OFF);
        }
        holders[n++] = holder;
        if (row.holderRead && holder != row.holder) moved = true;
    }
    return moved;
}

bool PopulationChanged(const Snapshot& prev, const Snapshot& cur) {
    if (prev.discsWritten != cur.discsWritten) return true;
    for (uint32_t i = 0; i < cur.discsWritten; ++i) {
        if (prev.discs[i].actorId != cur.discs[i].actorId) return true;
        if (prev.discs[i].space != cur.discs[i].space) return true;
        if (prev.discs[i].frisbeeCi != cur.discs[i].frisbeeCi) return true;
        // The same id pointing at a different CActor* is a rebuilt actor, and its node indexes are
        // a fresh set: diffing them row against row would compare two different trees.
        if (prev.discs[i].actor != cur.discs[i].actor) return true;
        if (prev.discs[i].nodes != cur.discs[i].nodes) return true;
        if (prev.discs[i].scripts != cur.discs[i].scripts) return true;
    }
    return false;
}

size_t Describe(const Snapshot& s, uint32_t discIndex, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';
    if (discIndex >= s.discsWritten) return 0;
    const DiscRow& d = s.discs[discIndex];
    Line line{out, cap};
    line.Add("[%u] space %016llX actor %016llX fci %u phys %u untouched %.2fs holder %08X",
             discIndex, (unsigned long long)d.space, (unsigned long long)d.actorId,
             (unsigned)d.frisbeeCi, (unsigned)d.physId, (double)d.timeSinceHeld,
             (unsigned)d.holder);
    if (!d.holderRead) line.Add("(unread)");
    line.Add(" flags %04X node %04X%s", (unsigned)d.physFlags, (unsigned)d.nodeState,
             d.nodeStateRead ? "" : "(unread)");
    if (d.nodes != 0) {
        line.Add(" actor* %016llX nodes %u/%u", (unsigned long long)d.actor, (unsigned)d.nodes,
                 (unsigned)d.nodeCount);
        for (uint32_t i = 0; i < d.nodes; ++i) {
            const NodeRow& nd = s.nodes[d.nodeFirst + i];
            line.Add(" [%u]%016llX=%u", (unsigned)nd.index, (unsigned long long)nd.symbol,
                     (unsigned)nd.enabled);
        }
    } else {
        line.Add(" nodes none (%s)", s.nodesRefused ? s.nodesRefused : "not walked");
    }
    if (d.refEnabledRead) {
        line.Add(" bubble -> actor %016llX node %016llX enabled=%u",
                 (unsigned long long)d.refActor, (unsigned long long)d.refNode,
                 (unsigned)d.refEnabled);
    } else {
        line.Add(" bubble unresolved (no actor_var in the graph's first %u data bytes)",
                 (unsigned)kVarScanBytes);
    }
    for (uint32_t i = 0; i < d.scripts; ++i) {
        const ScriptRow& sc = s.scripts[d.scriptFirst + i];
        line.Add(" script[%u] %016llX ci %u st %d data#%08X", i, (unsigned long long)sc.hash,
                 (unsigned)sc.ciIndex, (int)sc.state, (unsigned)DataDigest(sc));
        if (!sc.dataRead) line.Add("(unread)");
    }
    return line.n;
}

size_t Diff(const Snapshot& prev, const Snapshot& cur, uint32_t discIndex, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';
    if (discIndex >= cur.discsWritten || discIndex >= prev.discsWritten) return 0;
    const DiscRow& a = prev.discs[discIndex];
    const DiscRow& b = cur.discs[discIndex];
    Line line{out, cap};
    line.Add("[%u] actor %016llX:", discIndex, (unsigned long long)b.actorId);
    const size_t head = line.n;
    if (a.holder != b.holder) line.Add(" holder %08X->%08X", (unsigned)a.holder, (unsigned)b.holder);
    if (a.physFlags != b.physFlags) {
        line.Add(" flags %04X->%04X", (unsigned)a.physFlags, (unsigned)b.physFlags);
    }
    if (a.nodeState != b.nodeState) {
        line.Add(" node %04X->%04X", (unsigned)a.nodeState, (unsigned)b.nodeState);
    }
    // The untouched timer only matters when it RESETS: it counts up every frame, so reporting every
    // change would make this line fire on every trigger and say nothing.
    if (b.timeSinceHeld < a.timeSinceHeld) {
        line.Add(" untouched %.2f->%.2fs", (double)a.timeSinceHeld, (double)b.timeSinceHeld);
    }
    const uint32_t nodes = a.nodes < b.nodes ? a.nodes : b.nodes;
    for (uint32_t i = 0; i < nodes; ++i) {
        const NodeRow& x = prev.nodes[a.nodeFirst + i];
        const NodeRow& y = cur.nodes[b.nodeFirst + i];
        if (x.index != y.index || x.symbol != y.symbol || x.enabled == y.enabled) continue;
        if (y.symbol != 0) {
            line.Add(" node %016llX enabled %u->%u", (unsigned long long)y.symbol,
                     (unsigned)x.enabled, (unsigned)y.enabled);
        } else {
            line.Add(" node #%u enabled %u->%u", (unsigned)y.index, (unsigned)x.enabled,
                     (unsigned)y.enabled);
        }
    }
    // The ring itself, when the disc script's own reference named it. Guarded on the reference not
    // having MOVED: a `bubble` repointed at another actor is not the same bit changing value.
    if (a.refEnabledRead && b.refEnabledRead && a.refActor == b.refActor && a.refNode == b.refNode &&
        a.refEnabled != b.refEnabled) {
        line.Add(" bubble node %016llX enabled %u->%u", (unsigned long long)b.refNode,
                 (unsigned)a.refEnabled, (unsigned)b.refEnabled);
    }
    const uint32_t scripts = a.scripts < b.scripts ? a.scripts : b.scripts;
    for (uint32_t i = 0; i < scripts; ++i) {
        const ScriptRow& x = prev.scripts[a.scriptFirst + i];
        const ScriptRow& y = cur.scripts[b.scriptFirst + i];
        if (x.hash != y.hash) {
            line.Add(" script[%u] %016llX->%016llX", i, (unsigned long long)x.hash,
                     (unsigned long long)y.hash);
            continue;
        }
        if (x.state != y.state) line.Add(" script[%u] st %d->%d", i, (int)x.state, (int)y.state);
        if (!x.dataRead || !y.dataRead) continue;
        // The whole point of the packet: the graph's own words, named by INDEX rather than by a
        // variable name this plugin cannot resolve without calling into the script module. A word
        // that moves at the first grab is the finding; the script DB names it afterwards, offline.
        for (uint32_t w = 0; w < ScriptRow::kDataWords; ++w) {
            if (x.data[w] == y.data[w]) continue;
            line.Add(" script[%u] w%u %08X->%08X", i, w, (unsigned)x.data[w], (unsigned)y.data[w]);
        }
    }
    if (line.n == head) {
        out[0] = '\0';
        return 0;
    }
    return line.n;
}

}  // namespace disccensus
}  // namespace replay
