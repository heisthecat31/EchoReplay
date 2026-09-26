#pragma once

// SR15NetReplicatedDeltaUnreliableVars -- the message S_NETVAR is sourced from, parsed.
//
// ★ THIS FILE IS WHERE S_NETVAR'S COMPLETENESS COMES FROM, so the evidence is here rather than in a
// comment somewhere downstream.
//
// S5.6.2 requires `observed` to come from the engine's own numbering and never from our sample loop, and
// names the DUV wire sequence as the source for this stream. That sequence is a field of the message:
//
//     [ 0: 8) u64  owner      the loaded level id; a mismatch makes the engine drop the whole message
//     [ 8:10) u16  slot       dense netvar-component index (0 global_vars, 1 mp_arena_rules_netsettings)
//     [10:12) u16  gen        component generation
//     [12:16) u32  == 1       reserved; the host writes 1
//     [16:20) u32  pkt_seq    the value the client echoes in SR15NetReplicatedDeltaAck
//     [20:24) u32  == 0       reserved
//     [24:28) u32  rowset_seq  |  absent on a 24-byte message, which is legal
//     [28:30) u16  var_count  |
//     [30:  ) var_count x { u64 name (CSymbol64) ; u64 value (SValue) }
//
// ★ MEASURED, not assumed, over all 14,795 DUV messages in the only real capture on this machine
// (arena_decoded.txt, the decode of the private freshcap capture arena.bin):
//
//     size == 30 + 16*var_count                        14,794 of 14,795   (the 15th is 24 bytes, below)
//     pkt_seq == rowset_seq wherever both exist        14,794 of 14,794
//     consecutive pkt_seq deltas                       14,793 x (+1), 1 x (0)
//     the single 0 delta                               slot 0 and slot 1 sharing one packet number
//     reserved[12:16) == 1                             14,795 of 14,795
//     reserved[20:24) == 0                             14,795 of 14,795
//
// So the numbering is GLOBAL over the DUV channel, monotonic, and steps by 0 or 1 -- 0 exactly when one
// packet carried rowsets for more than one slot. That is enough to compute a gap: a jump of n means
// n-1 packets were never delivered to us, and the range is nameable. tests/capture replays the whole
// corpus through this parser and asserts every count above, so the claim is a check rather than a note.
//
// ★ AND THE SEQUENCE IS STORED IN THE RECORD, not only counted. A reader holding the file can redo the
// arithmetic itself; it does not have to believe the writer's counter. S5.6.2.0 sets exactly that bar
// for re-admitting anything that stores less than it saw, and it is the bar this stream clears.
//
// The 24-byte message is real and is not corruption: one appeared in the corpus, slot 1, carrying a
// header and no rowset section at all. It still advances the packet number, so it is `Ok` with
// `hasRowset == false` and no rows -- treating it as malformed would invent a gap that did not happen.

#include <stddef.h>
#include <stdint.h>

namespace replay {
namespace capture {

enum class DuvStatus : uint8_t {
    Ok = 0,
    TooShort,        // fewer than 24 bytes: not even a header
    RowsetTruncated, // a rowset section that does not hold var_count rows
    RowsetOversize,  // var_count would exceed the body
};
const char* DuvStatusName(DuvStatus);

struct DuvRow {
    uint64_t name;   // CSymbol64
    uint64_t value;  // the raw SValue qword. NEVER re-encoded (S5.4.5 rule 2)
};

struct DuvMessage {
    DuvStatus status;
    uint64_t  owner;
    uint16_t  slot;
    uint16_t  gen;
    uint32_t  reserved12;
    uint32_t  pktSeq;
    uint32_t  reserved20;
    bool      hasRowset;
    uint32_t  rowsetSeq;
    uint16_t  varCount;
    const uint8_t* rows;  // borrowed from the caller's buffer; null when varCount == 0
};

// Parses in place. `rows` points into `body`, so the result lives exactly as long as the body does.
DuvMessage DuvParse(const void* body, size_t bytes);

// Row `i`, read unaligned. The rows sit at body+30, so on an odd body address a struct read would be
// undefined; this is the only sanctioned way to reach them.
DuvRow DuvRowAt(const DuvMessage&, uint16_t i);

// The engine's SValue tagging (R4 S1.1). Recorded because the CLIENT'S OWN GETTER branches on the
// encoding rather than the value, so a re-encoded "equivalent" is a different value to the game -- the
// documented cause of a pause-overlay regression. Nothing in this packet ever converts an SValue; this
// exists so a diagnostic can print what a qword is without anybody being tempted to normalise it.
enum class SValueKind : uint8_t { Scalar, FlagFalse, FlagTrue, Unset, Opaque };
SValueKind SValueKindOf(uint64_t v);
const char* SValueKindName(SValueKind);

// ---- the completeness tracker ---------------------------------------------------------------
//
// Feed it every message the tap delivered, in delivery order. It reports how many packets the engine
// numbered (`observed`) against how many reached us (`delivered`), and names each missing run.
//
// It is deliberately NOT told how many records the writer stored: that is the writer's own opinion, and
// a counter that takes it would be measuring the writer against itself, which is hole 2 of S5.6.2.0.
class DuvSequence {
public:
    struct Gap {
        uint32_t firstMissing;
        uint32_t count;
    };

    // False when the message is malformed; a malformed message advances nothing, and the caller must
    // still store it verbatim, because bytes we cannot parse are bytes the client received.
    bool Note(const DuvMessage& m, Gap* gapOut);

    bool     Started() const { return started_; }
    uint32_t Delivered() const { return delivered_; }
    uint32_t Observed() const { return delivered_ + missing_; }
    uint32_t Missing() const { return missing_; }
    uint32_t LastSeq() const { return last_; }
    uint32_t Regressions() const { return regressions_; }
    uint32_t Duplicates() const { return duplicates_; }

    // Zeroes the per-chapter halves and keeps the sequence position, because a chapter boundary is not
    // a break in the engine's numbering.
    void BeginChapter();
    uint32_t ChapterObserved() const { return chapterDelivered_ + chapterMissing_; }
    uint32_t ChapterDelivered() const { return chapterDelivered_; }

private:
    bool     started_ = false;
    uint32_t last_ = 0;
    uint32_t delivered_ = 0;
    uint32_t missing_ = 0;
    uint32_t regressions_ = 0;
    uint32_t duplicates_ = 0;
    uint32_t chapterDelivered_ = 0;
    uint32_t chapterMissing_ = 0;
};

}  // namespace capture
}  // namespace replay
