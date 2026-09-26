#pragma once

// W2-B. Filling a Window from a real .echodemo, through W2-A's decoders.
//
// ---------------------------------------------------------------------------------------------
// ★ THE LIFETIME RULE THIS FILE EXISTS TO OBEY. IDemoReader hands back a DecodedChapter whose
// pointers survive exactly TWO further DecodeChapter calls. This function decodes, converts and
// COPIES into the Window's own storage before returning, so nothing borrowed from the reader outlives
// the call. That is why BuildWindow takes the reader rather than the caller passing a DecodedChapter
// in: a signature that accepted one would invite the caller to hold it.
//
// ★ AND THE TWO KEY SPACES IT KEEPS APART.
//   Bodies are keyed on BodyFrame::netId -- a u64 the ENGINE assigns, durable across the capture.
//   S_WORLD's slot index is NOT that key: it is a compact per-file index, and a window keyed on it
//   would bind the wrong actor after any join or leave.
//   Players are keyed on the RECORDED USER SLOT (window.h PlayerIdentity): the sampled stream's
//   entity, or the low word of a wire record's header -- never the wire stream's dense index. Their
//   user id in THIS session is left UNRESOLVED here (user_id_valid == false) for W2-C to fill.
//   Guessing it would put a slot where INV-OWN1 expects a user id, and INV-OWN1 would then validate
//   the wrong number and pass -- a guard that passes on the wrong input is worse than no guard.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "engine/entity.h"
#include "generated/facts_generated.h"
#include "foundation/demofile/demofile.h"
#include "playback/core/window.h"

#include "audio/audiopins.h"

// ---------------------------------------------------------------------------------------------
// THE FROZEN PAWN CONTRACT, docs/spikes/ARCH-23-PAWN-PROVISION.md section 9. W2-P1 OWNS IT.
//
// It is transcribed here because W2-P2 (this file) and W2-P1 are built in parallel and P1's header
// does not exist yet. The guard below is the handover: when P1 lands its header it defines
// REPLAY_PAWN_CONTRACT, this block compiles out, and whichever include comes first supplies the one
// definition -- so the transcription cannot quietly become a second, drifting source of truth.
// ---------------------------------------------------------------------------------------------
#include "engine/pawn_provision.h"   // W2-P1 owns the contract; this file consumes it

namespace replay {
namespace capture {
struct DecodedPlayer;
struct DecodedBody;
}  // namespace capture

namespace playback {

// ★★ THE SELF-VERIFICATION EVERY STREAM BLOCK MAKES POSSIBLE, AND WHY IT IS NOT OPTIONAL.
//
// A block is `[u32 exact bit length][packed records]`, and streamcodec.h says why the length is there:
// "the length is what lets a decoder prove it consumed what the encoder produced rather than assume it
// did." Without it, a decoder that reads 50 bits too few per record finishes its loop, reports success,
// and hands back records that are wrong in the fields it never reached -- and every downstream check
// compares one truncated reproduction against another and agrees.
//
// This project has already lost the tail of every record to exactly that shape, while the gate that
// should have caught it reported "decode proved". A check must not be shorter than its subject.
//
// So: after consuming `recordCount` records, the bits read must equal the declared bit length EXACTLY.
// Not "at least" -- a decoder that over-reads has desynchronised just as badly as one that under-reads,
// and `Overrun()` only catches the case that ran off the end of the buffer.
struct BlockVerdict {
    bool ok = false;
    uint32_t declared_bits = 0;
    uint32_t consumed_bits = 0;
    bool overrun = false;
    const char* why = "";      // never empty when !ok
};

BlockVerdict VerifyBlockConsumed(uint32_t declared_bits, size_t bits_read, bool overrun);

// A wire record the capture stored verbatim because re-encoding its decode did not reproduce the
// source bits (a quaternion whose two largest components tie comes back under the other selector).
// The raw message is in the record, so decode it: every field read cleanly at the pinned offset with
// zero padding means the values are the engine's own. On success `d.frame` holds them and
// `d.verbatim` is cleared; a body that does not read that way is left verbatim.
bool RecoverWireVerbatim(capture::DecodedPlayer& d);
bool RecoverWireVerbatim(capture::DecodedBody& d);

// What happened to every record of one pose stream: read == keyed + the drops, always. A launch
// log prints all four streams (FormatBuildSummary), so a record that reached no track has a named
// reason instead of an absence.
struct StreamStats {
    uint32_t read = 0;
    uint32_t keyed = 0;
    // Stored verbatim by the capture and not decodable here either (RecoverWireVerbatim said no).
    uint32_t dropped_verbatim = 0;
    // Stored verbatim, decoded here, and passed on: a subset of what went on to be keyed or dropped.
    uint32_t recovered_verbatim = 0;
    // Nothing in the record says whose it is: a body id value with no reading, a wire player
    // record with no header word.
    uint32_t dropped_identity = 0;
    // A wire record whose sender's clock has no measured offset onto the capture axis.
    uint32_t dropped_clock = 0;
    // The Window refused it: track table or sample pool full. The build then refuses as a whole.
    uint32_t dropped_window = 0;
    // Keyed, but stamped before its sender's offset and so placed at capture time 0. Not a drop:
    // it is the trace a wrong offset or a wrong stamp leaves.
    uint32_t clock_clamped = 0;

    uint32_t Dropped() const {
        return dropped_verbatim + dropped_identity + dropped_clock + dropped_window;
    }
};

// ★ WIRE RECORDS ARE STAMPED ON THEIR SENDER'S CLOCK. capture_types.h WireRec: "`stampUs` is the
// sender's stamp from the message's own [8:16), not our arrival time". Ticks, chapters and sampled
// rows are on the capture's axis, and on the real spectator capture the three senders sit 91 s,
// 1327 s and 2052 s away from it -- so a wire track sampled at the playhead is simply absent.
//
// The sender is the low dword of the message header word (`hdr0`), an SR15NetUserId: the player's
// own on a player record, the OWNER's on a body record, and all-ones for an unowned body, which the
// server stamps. One offset per sender serves every stream that sender feeds, the disc included
// while that player holds it. The generation is part of the sender, so a slot handed to a second
// person is a second clock.
//
// The offset is measured from the file's own structure. A record filed in a chapter arrived inside
// that chapter's span, and arrival = stamp - offset + latency, so for each chapter c
//   late_c  = last stamp  - chapter end     is at most  offset - the SMALLEST latency,
//   early_c = first stamp - chapter begin   is at least offset - the LARGEST.
// The highest late_c is the estimate: the least-delayed record is the one closest to the sender's
// true clock, which is the min-latency filter a clock sync uses, and it ignores both a lag spike
// and a stalled drain. A sender never seen across a chapter boundary leaves early above late,
// bracketing the offset instead of pinning it, and the midpoint is taken.
//
// One record stamped wrongly would otherwise own the extreme and move the sender's whole track. So
// each bound starts TrimFor(chapters) ranks inside its extreme, where one bad record cannot reach,
// and walks back out only while the next chapter's value is within kOutlierStepUs of the last. A
// sound file walks all the way and gets its extreme, exactly as before; a bad stamp stands alone
// and is left out, and one too small to stand alone moves the track by less than a step. The raw
// extremes are kept beside the bounds: (raw_late - raw_early) is what the file's chapters disagree
// by, a sender's latency spread when the file is sound and the size of the fault when it is not.
//
// It is constant for the whole file, so a window rebuild never moves a track in time. The residue
// is each sender's minimum latency.
struct WireClocks {
    static const size_t kMaxSenders = 64;
    // Above any link's honest jitter: the worst sender measured so far spread 154 ms, and the two
    // good ones 15 and 23 ms. Past this the load line is a Warn.
    static const int64_t kSpreadWarnUs = 250000;
    // Two send periods at the wire's 45 Hz. The top two late bounds of the real capture's senders
    // stand 3, 8 and 12 ms apart; a stamp wrong by less than this costs less than it would show.
    static const int64_t kOutlierStepUs = 50000;

    uint32_t sender[kMaxSenders] = {};
    int64_t  offset_us[kMaxSenders] = {};   // sender stamp minus capture time
    // The two structural bounds the offset was taken from, trimmed.
    int64_t  early_us[kMaxSenders] = {};
    int64_t  late_us[kMaxSenders] = {};
    int64_t  raw_early_us[kMaxSenders] = {};
    int64_t  raw_late_us[kMaxSenders] = {};
    uint32_t chapters[kMaxSenders] = {};    // CORE chapters the sender was seen in
    uint32_t ignored[kMaxSenders] = {};     // chapter bounds left outside early/late as outliers
    size_t   count = 0;
    // Senders past kMaxSenders. Their records have no clock and are dropped by name at build.
    uint32_t senders_overflowed = 0;

    static uint32_t SenderOf(uint64_t hdr0) { return (uint32_t)hdr0; }
    // Chapters ignored at each end of a sender's per-chapter bounds. One bad record spoils one
    // chapter; below four chapters there is nothing to tell it from the rest.
    static size_t TrimFor(size_t chapters) {
        if (chapters < 4) return 0;
        return chapters / 16 ? chapters / 16 : 1;
    }
    int64_t SpreadUs(size_t i) const { return raw_late_us[i] - raw_early_us[i]; }
    // The capture-axis time of a wire stamp; false when the sender has no measured offset.
    // `clamped` is set when the stamp lies before the sender's offset and was placed at 0.
    bool ToCaptureUs(uint64_t hdr0, uint64_t stamp_us, uint64_t& out, bool* clamped = nullptr) const;
};

// One pass over the file at LOAD, off the tick: the wire clocks above, and the deepest chapter of
// each pool so the window is sized by what the file holds. The wire's send rate is the sender's and
// owes nothing to the recorder's tick cadence, which is all CapsForCadence can see.
struct TrackScan {
    WireClocks clocks;
    uint32_t wire_records = 0;
    uint32_t max_body_records = 0;     // per CORE chapter, wire + sampled
    uint32_t max_player_records = 0;
    uint32_t max_ticks = 0;
    // ★ WHERE THE RECORDED CONTENT ACTUALLY IS, and the chapter table cannot answer it. A capture
    // written with `partial_start` carries ONE core chapter spanning every second between the
    // manifest origin and the first real tick (the owner's 2026-09-18 capture: chapter 2 is CORE,
    // t=[20999790..843887448], one tick in it), so "the smallest tBeginUs over core chapters" is
    // the file origin and a timeline drawn from it is 94% dead air. The ticks are the content.
    bool have_tick_span = false;
    int64_t first_tick_us = 0;
    int64_t last_tick_us = 0;
    // Every change of a component-id body's owned/unowned state over the WHOLE file, placed on the
    // capture axis exactly as the window places the row, sorted by (id_value, t_us). The disc is
    // such a body, and its first grab after a restart can lie anywhere in the file, so a seek far
    // past the restart cannot learn it from the window it lands in.
    struct HolderEdge {
        uint64_t id_value = 0;   // BodyIdentity::value of the track the row keys
        int64_t t_us = 0;
        bool owned = false;
    };
    static constexpr uint32_t kMaxHolderEdges = 8192;  // facts-ok: PHYS.CI_PAUSED_BIT an edge-count cap, not the paused bit
    std::vector<HolderEdge> holder_edges;
    uint32_t holder_edges_unplaced = 0;   // rows whose sender had no settled clock
    bool holder_edges_incomplete = false; // a block did not decode, or more edges/bodies than kept
};
bool ScanTrackSources(IDemoReader& reader, TrackScan& out, const char** why = nullptr);
// The load's `wire clock:` line for sender `i`. A Warn when SpreadUs(i) passes kSpreadWarnUs.
void FormatWireClock(const WireClocks& c, size_t i, const char* sender_name, char* out, size_t cap);
// `base` grown to hold kWindowChapters of the deepest chapter the scan saw. Never shrinks it.
WindowCaps CapsForScan(const WindowCaps& base, const TrackScan& scan);

struct BuildStats {
    uint32_t chapters = 0;
    size_t first_chapter = 0, center_chapter = 0, last_chapter = 0;
    // The per-stream account of every pose record, summed over the chapters of the build.
    StreamStats body_wire, body_sampled, player_wire, player_sampled;

    uint32_t body_samples = 0;
    // Samples whose identity was not resolvable -- idValue present but nothing saying how to read it.
    // Counted and dropped rather than keyed on: a track built on a number whose meaning was not
    // recorded is worse than a missing track. A WIRE row is not one of these: its 64-bit id field
    // carries its own tag in the high dword (BodyKeyOfWire in windowbuild.cpp).
    uint32_t body_identity_unresolvable = 0;
    // Body records that named no net actor at all -- `idValue` 0 with no identity fill. NOT a
    // malformed row: it is what the recording client's OWN body looks like, and it is admitted as a
    // track keyed on its capture pool handle so BindBodyHandles can pair it with the pawn the file's
    // one locally authored user slot names. Distinct from the count above, which is a value nothing
    // says how to read.
    uint32_t body_no_net_id = 0;
    uint32_t player_samples = 0;
    uint32_t netvar_rows = 0;
    uint32_t netvar_seeds = 0;
    // ★ Whether the FILE carries a time per netvar message (the fifth field of its recorded netvar
    // schema). False is not an error -- it is every capture written before ENG-NETVAR-STAMP -- but it
    // is the difference between a resolved value and an interpolated one, so it is published rather
    // than blended in. NetvarStampsPresent below is the one place that decides it.
    bool netvar_stamps_present = false;
    // Rows whose time this build INVENTED by spreading a chapter's arrivals evenly, because the file
    // carries none. Zero on a stamped file. Non-zero means the clock is an approximation.
    uint32_t netvar_rows_spread = 0;
    // The widest chapter the spread placed rows across, and what that costs. A key with ONE row in
    // a chapter lands at its midpoint, so its recorded instant can be anywhere within half a span
    // either side: with 4 s chapters a score flips up to 2 s from when it really did. Microseconds,
    // not adjectives -- a reader comparing a HUD against the picture needs the number.
    uint32_t netvar_spread_span_us = 0;
    // The worst PER-KEY placement interval over every chapter this build spread, the row count of
    // the key it belongs to, and how many keys had a single row in a chapter. The worst case is a
    // statement about sparse keys; a dense one is placed to within its own tap period.
    uint32_t netvar_spread_worst_us = 0;
    uint32_t netvar_spread_sparsest_rows = 0;
    uint32_t netvar_spread_one_row_keys = 0;
    // Keys this window carried no data for at all, seeded from the file's first recorded value so
    // the HUD shows what the recording opens on rather than the host's own residue.
    uint32_t netvar_seeds_primed = 0;
    // Netvar rows the row budget could not hold. The build refuses when this is non-zero: it used to
    // stop adding rows, skip the block's bit-consumption check and return a truncated track.
    uint32_t netvar_rows_over_budget = 0;
    uint32_t ticks = 0;
    // Blocks whose consumed bit count matched the declared one exactly. Every stream block decoded is
    // counted here; a build that decodes blocks and verifies none is a build with the check switched
    // off, and the two numbers being equal is what says it is on.
    uint32_t blocks_verified = 0;
    uint32_t blocks_bit_mismatch = 0;
    // Records the decoder refused. Non-zero is a real finding: the file and the codec disagree, and
    // continuing would replay a partial chapter as if it were whole.
    uint32_t decode_failures = 0;
    // Records stored verbatim that RecoverWireVerbatim could not decode either: counted and skipped.
    uint32_t verbatim_skipped = 0;
    // Blocks in S_KEY whose id this build does not know. Skipped by length, and COUNTED: a silent skip
    // is how a block gets written by capture and never read by playback.
    uint32_t key_blocks_unknown = 0;
    // Whole STREAMS present in the chapter that this build never asked for -- the same statement as
    // key_blocks_unknown, one level up. S_MARK, S_VOIP, S_ORACLE and the shed-ladder streams today,
    // and S_WORLD in any capture recorded before it was retired.
    //
    // NOT an error: a window samples poses, and several streams are not poses. It is a number because
    // the alternative is an absence, and this file used to have one. The decode table listed S_WORLD,
    // and being in that table means "decode this stream with the SAMPLED BODY codec" -- while the
    // retired WorldRec put frameIndex and localSlot where SampledBodyRec has idValue and handle. On an
    // old capture that is either a bit-length mismatch that refuses the whole window, or bodies that
    // were never recorded. Counted here instead, and named.
    uint32_t streams_undecoded = 0;
    const char* first_undecoded_stream = nullptr;
    // ★ The ids the decode ASKED for, one bit per stream id -- the mask the count above is taken
    // against, published so a check can assert what this build does and does not decode without
    // restating the table and drifting from it. Bit 10 clear is the retired-S_WORLD invariant, and it
    // goes red the moment the id is put back, whether or not any file contains one.
    uint32_t streams_asked = 0;
    // S_EVENT records present in the chapter and NOT decoded by this build. Counted rather than
    // ignored, so "playback does not replay events yet" is a number in the artifact instead of an
    // absence nobody notices.
    //
    // ★★ AND A STANDING RULE FOR WHOEVER DECODES THEM. Every S_EVENT row's typeIndex refers to the
    // tap table, and that table used to exist ONLY as a C array in source -- so reordering it would
    // have made every event in every previously recorded file decode as a different message type,
    // with nothing disagreeing. W2-A now writes the table into every keyframe. RESOLVE EVENT TYPES
    // FROM THE FILE'S TABLE, NEVER FROM A COMPILED-IN ONE: a compiled-in table is a second source of
    // truth that is right until somebody edits it, and then silently wrong for every old file.
    // ★★ THE OR OF EVERY DECODED RECORD'S `fill`: what the FILE says was actually SOURCED. A field
    // the gather never read is zero in the file and zero on playback, so comparing it against itself
    // agrees at both ends and the absence is invisible. Asserting on this instead makes the gap a
    // statement the file makes about itself, which is the only form that fails when it changes.
    uint32_t body_fill_seen = 0;
    uint32_t player_fill_seen = 0;
    uint32_t events_undecoded = 0;
    // GameTimer-bearing S_GAMESTATE samples decoded into the match-clock track.
    // Zero on a file with no S_GAMESTATE stream (or one that never sampled the clock) -- which is the
    // signal that the clock cannot be replayed from this capture, distinct from "the write is off".
    uint32_t gamestate_clock_samples = 0;
    bool keyframe_present = false;
    // The pins carried by the keyframe this window resolved. AUDIO_PINS is a defined block that
    // capture RESERVES and does not write (capture/keyframe.cpp: "reserved, unwritten"), so absent
    // is the normal case -- and absent is not "no loops": a consumer must not turn a block the
    // recorder never wrote into a claim about what the file says is sustained.
    bool audio_pins_present = false;
    uint32_t audio_pins_bytes = 0;
    // The block VERBATIM, not a decode: DecodePins lives in W2-H's audio library and this one is
    // linked by consumers that have no audio in them at all. MaxPinBytes() is the format's own
    // worst case, so nothing here decides how big a pin block may be.
    uint8_t audio_pins_raw[audio::MaxPinBytes()] = {};

    const char* refusal = nullptr;   // non-null iff the build failed; names the cause
};

// Fills `out` from the CORE chapter at `chapter_index`, plus the netvar seed values from that
// chapter's S_KEY. Returns false and sets stats.refusal on a decode failure.
//
// `clocks` places wire records on the capture axis (WireClocks above). Null keeps the stamp as
// written, which is right only for a file whose wire stamps are already on that axis -- the shipped
// load always passes the scan's.
bool BuildWindow(IDemoReader& reader, size_t chapter_index, Window& out, BuildStats& stats,
                 const WireClocks* clocks = nullptr);

// ★★ THE SUMMARY DOES NOT FIT ONE LOG LINE. At 512 (546 with the prefix) the writer truncated it
// mid-number and every netvar count -- the per-key row counts and the one-row-key count the spread
// warning exists to report -- was unreadable in all 72 lines of the last launch. Written in PARTS
// rather than into a bigger buffer, because this formats on the tick path: two stream accounts per
// part, then the netvar account. Always NUL-terminated; `cap` 0 and a part past the end write nothing.
inline constexpr size_t kBuildSummaryParts = 3;
void FormatBuildSummary(const BuildStats& st, size_t part, char* out, size_t cap);

// Whether this file's netvar messages carry their own time, read off the field count its manifest
// recorded for the netvar schema. A capture written before that field declares four fields and must
// be decoded as four; asking the manifest is what keeps an old file readable byte for byte instead of
// refused or mis-decoded. Exposed so a test can state the compatibility rule directly.
bool NetvarStampsPresent(const Manifest& m);

// ★ THE OLD FILE'S ONLY AVAILABLE APPROXIMATION, and it is exposed because the resolver would HIDE a
// bug in it: KeyedResolver::Build clamps each key's series non-decreasing, so a spread that went
// backwards would be silently straightened and a test that checked it through ValueAt would pass.
// The rows are what must be right, so the rows are what a test gets to see.
//
// `rows` is one chapter's netvar rows, in recorded order, all currently stamped tBeginUs. Each key's
// rows are re-spaced evenly across (tBeginUs, tEndUs) -- strictly inside, so this chapter's keyframe
// snapshot at tBeginUs still sorts first and the next chapter's is still reached later. Monotone per
// key by construction; two keys' rows are independent, which is what the resolver wants.
//
// Nothing is allocated and nothing outside `rows[0..n)` is touched.
// What a chapter's spread cost, measured per key rather than per chapter.
struct SpreadBound {
    uint32_t worst_us = 0;       // the sparsest key's placement interval
    uint32_t sparsest_rows = 0;  // how many rows that key had -- 1 means a midpoint placement
    uint32_t one_row_keys = 0;   // keys with a single row: the ones the worst case is about
    uint32_t keys = 0;
};

void SpreadRowsOverChapter(KeyedRecord* rows, size_t n, uint64_t tBeginUs, uint64_t tEndUs,
                           SpreadBound* bound = nullptr);

// ★★★ THE RESIDENT WINDOW: [current-1, current, current+1], which is what S11.3 asks for and what
// playback across a chapter boundary needs.
//
// One chapter is not enough for two reasons and the second is the one that bites. Forward playback
// crossing a boundary would sample past the end of the last track and get a clamped, stale pose --
// plausible, and wrong. And REVERSE needs the chapter BEHIND the playhead resident, which a
// current-only window never has: every backwards step would leave the span immediately.
//
// ★ It reads three chapters and copies each out before decoding the next, so the reader's depth-2
// guarantee is never leaned on -- the third decode would invalidate the first, which is precisely the
// use-after-free demotypes.h warns about and precisely why Window owns its storage.
// ★★★ WHAT EVERY DRIVEN KEY OPENS THE FILE ON. A window is three chapters, and a key the file does
// not carry in any of them has no series at all -- the resolver refuses, the HUD key is never
// claimed, and the HOST's own residual value is what the viewer reads (a `05:00` pre-match clock
// over a replay whose match clock is 02:00). The chapter keyframes already carry a full netvar
// snapshot, so one walk at load answers "the first value this file ever recorded for this key",
// which is exactly what value-at-T is before the first recorded one.
//
// Values, not pointers: nothing here borrows from a DecodedChapter, so it outlives the walk.
struct NetVarPrimer {
    static const size_t kMax = 256;
    uint64_t key[kMax] = {};
    uint64_t value[kMax] = {};
    uint16_t slot[kMax] = {};
    size_t count = 0;
    // Chapters walked and keyframes read, so an empty primer can say which of the two it was.
    uint32_t chapters = 0;
    uint32_t keyframes = 0;

    size_t IndexOf(uint64_t k) const {
        for (size_t i = 0; i < count; ++i) {
            if (key[i] == k) return i;
        }
        return kMax;
    }
};

// Walks every CORE chapter's keyframe once and records each key's FIRST recorded value and slot.
// Load-time only -- it decodes the whole file, which is the same walk ScanRecordedIdentity makes.
void ScanNetVarPrimer(IDemoReader& reader, NetVarPrimer& out);

bool BuildWindowRange(IDemoReader& reader, size_t center_chapter, Window& out, BuildStats& stats,
                      const WireClocks* clocks = nullptr, const NetVarPrimer* primer = nullptr);

// ★★ THE RE-ENTRANT FORM, AND WHY THERE ARE TWO.
//
// The overload above decodes through a function-STATIC ~860 KB accumulator: it was a stack local
// once and overflowed the stack, and a per-call heap allocation on the tick path is a cost this
// code does not want. A static is correct for exactly one caller, and silently wrong for two --
// two builds in flight would interleave their tick tables, their netvar rows and their keyframe
// blob with nothing failing anywhere. So a second thread brings its OWN, and the tick path is
// untouched. The two forms run the same body, so the window they produce is byte-identical.
//
// Opaque and heap-only on purpose: 860 KB is not a thing to put on a stack by accident. Create one
// per thread, keep it for that thread's life, destroy it after the thread has joined.
struct BuildScratch;
BuildScratch* CreateBuildScratch();          // null when the allocation failed; never throws
void DestroyBuildScratch(BuildScratch* s);   // null-safe
bool BuildWindowRange(IDemoReader& reader, size_t center_chapter, Window& out, BuildStats& stats,
                      const WireClocks* clocks, const NetVarPrimer* primer, BuildScratch& scratch);

// Counts the streams in `dc` whose ids are not in `asked`, into `stats`. `asked` is a bitmask of the
// ids the decode actually looked up, built BY the decode rather than by a second list somebody has to
// remember to update -- so a stream that stops being decoded starts being counted on the same edit.
//
// Exposed because the case that matters cannot be reached through the writer: this build refuses to
// write a retired stream id, so a chapter carrying S_WORLD can only be built by hand.
void CountUndecodedStreams(const DecodedChapter& dc, uint32_t asked, BuildStats& stats);

// The CORE chapter before/after `i`, or `i` itself at the ends. Chapters of other sections sit between
// them in the file, so neither is i-1 or i+1.
size_t PrevCoreChapter(IDemoReader& reader, size_t i);
size_t NextCoreChapter(IDemoReader& reader, size_t i);

// Resolves each BodyTrack's engine handle from its durable u64 net id, through W1-E's own
// CR15NetActorCS table. Returns how many resolved.
//
// ★★ THIS IS WHY THE WINDOW KEYS ON A NET ID. A PhysHandle is a pool slot: it does not survive a
// session, let alone a recording, so a window keyed on one would bind the wrong actor the moment the
// pool differed -- which it always does, because a replay creates its entities in its own order. The
// net id is what the wire carries and what SimStateCB resolves every inbound record through, so this
// is the engine's own mapping rather than one we invented.
//
// A track whose id this session does not carry keeps handle_valid == false and is NOT published.
// Absence is represented, never defaulted to slot 0 -- publishing a recorded body onto whichever
// actor happened to land on that handle is a wrong replay that nothing reports.
// ★★★ HOW A STORED IDENTITY IS READ, AND THE TRAP IN IT.
//
// SimStateCB's dispatch is `cmp dword ptr [r14+4Ch], 2` / `jb` (read from the bytes at 0x140D505BF):
// tag < 2 takes the slot+generation arm, tag >= 2 falls through to FindNetActor. So on the WIRE the
// rule is a THRESHOLD -- 0 an SR15NetUserId, 1 a component id, >= 2 a u64 net id -- and it is
// unsigned, which matches idTag being a uint32_t on both sides.
//
// ★★★ BUT THAT RULE MUST NOT BE APPLIED TO idTag WITHOUT CHECKING kFillBodyIdTag FIRST, AND THIS IS
// THE PART THAT WOULD HAVE BITTEN. W2-A's capture site writes `rec.idTag = 0` UNCONDITIONALLY and
// sets kFillBodyIdNetActor, never kFillBodyIdTag -- deliberately, and its comment says why: the
// accessor guarantees a net id whatever tag byte the component held, and "writing a tag value nobody
// read would be inventing the one field this pair exists to preserve."
//
// So in every file written today, idTag == 0 means "NOBODY READ A TAG", not "SR15NetUserId". A reader
// that applied the >= 2 threshold to the stored value would classify every sampled body as a user id
// and resolve NONE of them -- correct-looking code, no error, an empty replay. The disambiguator is
// the FILL BIT, not the value, and the precedence below is therefore not a stylistic ordering.
// The wire threshold, read from the dispatch at ENTITY.SIMSTATE_ID_TAG_SITE: `cmp [r14+4Ch], 2` /
// `jb`, unsigned. It reaches code through facts_generated.h once W0-2 merges the row; until then it
// is defined here beside the derivation rather than as a bare 2 three lines down.
static const uint32_t kBodyIdTagNetIdMin = 2;

enum class IdKind : uint8_t {
    NetActorGuaranteed,   // kFillBodyIdNetActor: came through NetIdOf; idTag is a placeholder, ignore it
    NetIdByTag,           // kFillBodyIdTag and tag >= ENTITY.BODY_ID_TAG_NETID_MIN
    UserIdByTag,          // kFillBodyIdTag and tag == 0: an SR15NetUserId, NOT resolvable via PhysOfNetId
    ComponentIdByTag,     // kFillBodyIdTag and tag == 1: likewise not a net id
    Unresolvable,         // value with nothing saying how to read it
};

const char* IdKindName(IdKind);
IdKind ClassifyBodyIdentity(uint32_t fill, uint32_t tag);

// `bound` counts tracks resolved to a live handle. `deferred` counts tracks whose identity IS
// resolvable but is not known to be a NET id -- the raw tag was read and this build cannot yet say
// which tag value means net-actor. Those are left unbound rather than passed to PhysOfNetId, because
// handing it a component id would return either 0xFFFF or, worse, a collision.
// The absence word every handle field in the pawn contract carries, named ONCE so the comparisons
// that follow are not a dozen transcriptions of one number.
static const uint16_t kPawnAbsent = 0xFFFFu;  // facts-ok: LOCAL.NO_COMPONENT the component-absent word

struct BindResult {
    size_t bound = 0;
    // A resolvable identity that is a COMPONENT id (tag 1) -- the disc on the wire. Not an error and
    // not a deferral: nothing on this side maps a recorded component id to a body in this session,
    // and handing one to PhysOfNetId returns 0xFFFF at best and collides at worst. A USER id is no
    // longer counted here: it names a player's body and binds through that player's slot.
    size_t not_a_net_id = 0;
    size_t unresolvable = 0;
    // `absent_this_session` IS RETIRED. It counted two unrelated causes -- a slot no pool holds and
    // a net id this session does not carry -- and hid a third (a CI carrying no body) inside them.
    // One field per cause below, and FormatBindSummary prints every one of them by name.
    //
    // The NET-ID arm's own absence: PhysOfNetId answered 0xFFFF for a body that named a net actor.
    // No pawn was looked up, so it is counted apart from the pawn-resolve refusals.
    size_t no_net_actor = 0;
    // The pawn-resolve refusals, by name (ARCH-23 section 5). `no_model` is not one of them: the
    // track IS bound and driven, and the count says the pawn cannot be seen -- the difference
    // between "the replay did not run" and "the replay ran behind an invisible body".
    size_t no_player_ci = 0;
    size_t no_phys_ci = 0;
    // The same refusal, still inside its wait budget: the CI is there and the body is not there
    // YET. Counted apart so a run can tell a pawn that is coming from a pawn that is not.
    size_t awaiting_phys_ci = 0;
    size_t class_mismatch = 0;
    size_t slot_collision = 0;
    size_t no_model = 0;
    size_t no_entity_service = 0;
    // Which surface answered for the slot-bound track. Unknown when nothing bound by slot. The log
    // line prints the word, so a run states `class=local` or `class=remote` instead of leaving
    // FACT-PP1 to be inferred from whether a body moved.
    PawnClass slot_class = PawnClass::Unknown;
    // The two slot numbers, because they are not the same number. The viewer's own user occupies
    // slot 0, so the slot a FILE recorded is not the slot its pawn was provisioned into, and a
    // line printing one of them cannot be read against a roster that uses the other. Equal when no
    // map is installed, and both are 0xFFFF when nothing was bound by slot.
    uint16_t slot_recorded = kPawnAbsent;
    uint16_t slot_resolved = kPawnAbsent;
    // The engine entity key of the pawn that bound by slot, read off the surface that answered.
    // kEntityKeyUnknown when the surface published none -- NOT 0, which is a legal id (the local
    // pawn's) and the one this line is most likely to be printing.
    uint32_t slot_entity = kEntityKeyUnknown;
    // Of `bound`, the ones bound through a user slot instead of a net id: the recording client's
    // own body, and every player body the wire named by SR15NetUserId. A solo capture has exactly
    // one; a spectator capture has one per player. The slot_* fields above describe the LAST of
    // them -- each track carries its own (BodyTrack::user_slot / slot_entity).
    size_t bound_by_slot = 0;
    // A second track resolving to a slot an earlier track of this bind already drives. Refused:
    // two tracks installing into one body is a pose that flickers between two recordings.
    size_t slot_already_driven = 0;
    // A wire body whose recorded slot the provisioner has not assigned a live slot. The identity
    // map would resolve recorded slot 0 at live slot 0, which is the VIEWER's, so it is refused
    // until the assignment exists. `first_unassigned_slot` names one of them on the bind line.
    size_t slot_unassigned = 0;
    uint16_t first_unassigned_slot = kPawnAbsent;
    // A netId-0 body the file cannot pair with a pawn, because the window carries a different number
    // of locally authored pawns than of netId-0 bodies. Which pawn owns which body is not decidable
    // from the file at that point, so it is refused and counted rather than guessed onto the first
    // slot -- a wrong pairing would drive the human's recorded body onto a bot, silently.
    size_t local_pawn_ambiguous = 0;

    // P3-MOTION. The pose-vouch discriminator for the slot-bound REMOTE entrant, filled only when a
    // remote pawn resolved by slot (`p3_probed`). RUN-L3-47 reads these to separate Scenario A (a
    // vouched phys handle was found via ComponentOnActor and bound -> the recorded pose installs)
    // from Scenario B (the actor carries no phys component and the CI id is uninitialised -> the
    // bind keeps the old handle and the write refuses exactly as before -- the receive-path wall).
    // FormatBindSummary prints every one of them; a field the line never prints is a fact nothing
    // outside this process can read.
    bool     p3_probed           = false;
    uint16_t p3_bound_handle     = kPawnAbsent;  // PhysOf(PlayerOf(slot)) -- the handle bound before
    uint16_t p3_ci_entity_id     = 0;            // CI[+0] the bound handle addresses (the discriminator)
    uint16_t p3_rev_handle       = 0;            // the reverse-map entry for that CI index
    bool     p3_component_present = false;       // ComponentOnActor(phys_.cs, actor) != kNoComponent
    bool     p3_vouched          = false;        // a ResolveVouched-passing handle was found and bound
    uint16_t p3_vouched_handle   = kPawnAbsent;  // that handle

    // DISC-BIND. The last tag-1 (component id) track this bind looked at, and how the live
    // frisbee's own SR15FrisbeeCI+0x18 answered for it. Filled only when the disc drive is on.
    enum class DiscVerdict : uint8_t {
        NotAsked,
        Agreed,             // the recorded id is a live frisbee's physics component
        ReaimedToLive,      // it is not; the one track was bound to the one live frisbee instead
        OracleUnreadable,   // no live frisbee could be read: not driven
        Ambiguous,          // disagreement with more than one track or more than one frisbee
        NoSuchComponent,    // the physics pool does not hold the chosen id
        HandleInUse,        // another track of this bind already drives that body
    };
    DiscVerdict disc_verdict = DiscVerdict::NotAsked;
    uint32_t    disc_recorded_id = 0;
    uint32_t    disc_bound_id = 0;
    uint32_t    disc_live_id = 0;         // the live disc's id, when the chain resolved
    size_t      disc_live_count = 0;
    const char* disc_oracle_unreadable = nullptr;
    IEntity::ComponentIdReading disc_reading;
    // The two CR15NetPhysicsCS instances, side by side. They are expected to DIFFER -- the disc's
    // comes from the level space -- and the line prints both so the next launch settles it without
    // another round trip. Indexing the disc's id into the players' pool would address a player body.
    void* disc_phys_cs = nullptr;
    void* disc_player_phys_cs = nullptr;
    void* disc_frisbee_cs = nullptr;
    bool  disc_shortcut_agreed = false;   // rules+0x290/+0x298 named the same {cs, id}
    size_t bound_disc = 0;
    size_t disc_refused = 0;
};
const char* DiscVerdictName(BindResult::DiscVerdict v);
// The disc half of a bind, on its own. Exported because the level game space the frisbee lives in
// is created EMPTY at map load and filled later, so the bind has to be re-attempted while a replay
// is loaded -- and re-running the whole bind to do it would release and re-claim every player body
// once per retry. Fills only the disc fields and `bound_disc`; `bound` is the caller's to carry.
// once per retry. Fills only the disc fields and `bound_disc`; `bound` is the caller's to carry.
void BindDiscTracks(IEntity& entity, void* netGame, Window& w, BindResult& r);

// The worst-case placement error a chapter of `spanUs` costs a key with `rowsForKey` rows in it:
// one placement interval, span/(n+1). A one-row key gets span/2 -- which is what this used to
// return for EVERY key, including a 180 Hz one whose real error is milliseconds.
uint32_t NetvarSpreadWorstUs(uint64_t spanUs, uint32_t rowsForKey);
// The disc's bind, as one log line; false and empty when this bind did not ask.
bool FormatDiscBind(const BindResult& b, char* out, size_t cap);
// THE RESOLVE, OVER BOTH SURFACES, CACHED BY BIND EPOCH.
//
// ARCH-23 section 8: resolution is once per player per BIND EPOCH and never per tick. The binder
// already re-runs only on a window rebuild or an entity rebind (L3 #11: 25 rebuilds in four
// minutes), and the epoch is the signal that says a handle issued against the old game space is
// garbage. An IEntity that publishes no epoch is resolved on every ask rather than cached against a
// signal it does not have: a cache with no invalidation is a stale handle waiting for a rebuild.
//
// The attempt cap is the other half. A permanently absent slot must not spend two pool lookups on
// every rebuild forever, so after AttemptCap() failed attempts within one epoch the refusal LATCHES
// and no surface is touched again until the epoch moves.
class PawnResolver {
public:
    // ENTITY.USER_SLOT_TABLE_MAX, derived: the table is keyed by user slot, and a slot the engine's
    // own table cannot hold is a slot no pawn can occupy.
    static const size_t kSlots = facts::ENTITY_USER_SLOT_TABLE_MAX;
    // `provision.resolve_attempts`, ARCH-23 section 8. A budget this side chooses, not a fact about
    // the game, so it is a default here and settable by whoever owns the config plumbing.
    static const uint8_t kAttemptCapDefault = 8;

    struct Stats {
        size_t asked = 0;             // calls to ResolvePawn
        size_t surface_lookups = 0;   // the ones that actually touched a surface
        size_t cache_hits = 0;
        size_t epoch_changes = 0;
        size_t latched = 0;           // refused with no lookup: the cap is spent for this epoch
        size_t uncached = 0;          // asked of an IEntity that publishes no bind epoch
        size_t resolved_local = 0;
        size_t resolved_remote = 0;
    };

    void Reset();                     // a new load: forget every slot and the epoch

    // W2-P1's recorded-slot -> provisioned-slot map. A recorded player is not provisioned into the
    // slot the file names -- the viewer's own user is already in slot 0 -- so the surfaces are
    // asked about the MAPPED slot while the cache stays keyed on the recorded one, which is what
    // the caller has. No map installed is the identity map, not a refusal: a session that
    // provisioned nothing still has to be resolvable against what it does carry.
    //
    // An entry of kSlotUnassigned, like an entry past `n`, is a recorded slot nobody has placed:
    // MappedSlot answers the identity for it and SlotAssigned answers false.
    static const uint16_t kSlotUnassigned = kPawnAbsent;
    void SetSlotMap(const uint16_t* map, size_t n);
    uint16_t MappedSlot(uint16_t recorded) const;
    bool SlotAssigned(uint16_t recorded) const {
        return have_map_ && recorded < kSlots && assigned_[recorded];
    }
    void SetAttemptCap(uint8_t attempts) { cap_ = attempts ? attempts : 1; }
    uint8_t AttemptCap() const { return cap_; }

    // `recorded` is the class the FILE says the row was captured from. Unknown is the honest answer
    // for every capture written so far (INV-PP2's NOT-RUN) and means "prefer the surface the
    // capture reads": Local, because GatherLocalPawns is the only producer of a player row, with
    // Remote as the fallback. A KNOWN class binds -- the other surface answering alone is
    // PawnClassMismatch, never a drive of the wrong CI.
    bool ResolvePawn(const IEntity& entity, uint16_t slot, PawnClass recorded, PawnRef& out);

    PawnResolve::Refusal LastResolveRefusal() const { return last_; }

    // ★ AWAITING, NOT ABSENT. L3 #22 watched the engine allocate a player actor for slot 1 and the
    // bind still report `no phys ci`: the component instance that carries the body is built after
    // the actor, not with it. NoPhysCi is therefore the one refusal that can be a step BEHIND
    // rather than a step missing, so the entry keeps being re-asked on the next epoch instead of
    // spending the permanent-absence budget -- bounded by that same cap on its own counter, so a
    // body that never arrives does stop being asked for. True while this slot is still waiting.
    bool AwaitingPhysCi(uint16_t recorded) const {
        if (recorded >= kSlots) return false;
        const Entry& e = table_[recorded];
        return e.used && !e.ok && e.refusal == PawnResolve::Refusal::NoPhysCi &&
               e.phys_waits < cap_;
    }
    const Stats& GetStats() const { return stats_; }

private:
    struct Entry {
        bool    used = false;
        bool    ok = false;
        uint8_t attempts = 0;
        // Passes spent WAITING for a body on a CI that already exists, counted apart from the
        // absence budget above -- see AwaitingPhysCi.
        uint8_t phys_waits = 0;
        PawnRef ref{PlayerHandle(kPawnAbsent), PhysHandle(kPawnAbsent), kEntityKeyUnknown,
                   PawnClass::Unknown, false};
        PawnResolve::Refusal refusal = PawnResolve::Refusal::NotAttempted;
    };
    Entry    table_[kSlots];
    uint16_t map_[kSlots];
    bool     assigned_[kSlots] = {};
    bool     have_map_ = false;
    uint64_t epoch_ = 0;
    bool     have_epoch_ = false;
    uint8_t  cap_ = kAttemptCapDefault;
    PawnResolve::Refusal last_ = PawnResolve::Refusal::NotAttempted;
    Stats    stats_;
};

// The two-argument form resolves through a call-scoped resolver: repeated slots inside ONE bind are
// still looked up once, and nothing is cached across binds. A caller that wants the epoch cache
// owns a PawnResolver and passes it.
BindResult BindBodyHandles(IEntity& entity, Window& w);
// `drive_disc` (playback.disc_drive) binds the tag-1 component-id track as well; off, such a
// track stays unbound and counted under not_a_net_id.
BindResult BindBodyHandles(IEntity& entity, Window& w, PawnResolver& resolver,
                           bool drive_disc = false, void* netGame = nullptr);

// The map form. The shipped bind takes it: the recorded slot is the VIEWER'S own slot on every
// live lobby, so a bind that asks the surfaces about the number the FILE carries asks about the
// wrong pawn or about nothing at all. `n` 0 or `slot_map` null is the identity map with no slot
// assigned, so every wire body is refused `slot unassigned` (PawnResolver::SetSlotMap).
BindResult BindBodyHandles(IEntity& entity, Window& w, const uint16_t* slot_map, size_t n,
                           bool drive_disc = false, void* netGame = nullptr);

// The bind's one line, formatted where the fields are DEFINED rather than at the log site. It is the
// sentence a launch quotes back, and a BindResult field the line never prints is a fact nothing
// outside this process can read -- `bound_by_slot` was exactly that until B1's second half. Always
// NUL-terminated; `cap` 0 writes nothing.
// The words the line prints. Beside the counters they describe, for the same reason the summary
// is formatted here: a name that lives at the log site is a second spelling of an enum.
const char* PawnClassName(PawnClass);

// The widest text FormatEntityKey writes: UINT32_MAX in decimal, or the one-character no-key word,
// plus the terminator.  Exported beside the formatter so every caller sizes from the same number.
constexpr size_t kEntityKeyTextMax = 12;

// `ent=?` and `ent=0` are different sentences -- 0 is the local pawn's id -- so no caller spells
// the no-key word for itself.
void FormatEntityKey(uint32_t key, char* out, size_t cap);

void FormatBindSummary(const BindResult& b, char* out, size_t cap);

// The chapter whose span contains `t_us`, or the last one at or before it. Wraps
// IDemoReader::FindChapter so the caller does not have to know the section id.
size_t ChapterForTime(IDemoReader& reader, int64_t t_us);

}  // namespace playback
}  // namespace replay
