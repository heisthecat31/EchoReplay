#pragma once

// The MANIFEST this packet writes, and the closed list of message types it taps.
//
// ---------------------------------------------------------------------------------------------
// ★ THE ONE-SHOT MESSAGES ARE CAPTURED VERBATIM, NOT PARSED INTO THE TYPED MANIFEST AND DISCARDED.
//
// S5.4.1 lists what MANIFEST must carry: level and gametype, team alignments, the net-id map, the two
// netvar baselines, the 16,817-byte balance blob, and per player the name, entrant JSON, loadout,
// equipment, profile unlocks and level. Every one of those arrives as a message on the same bus the
// event tap already listens to. So the capture stores those message bodies WHOLE, on S_EVENT, under a
// type index the manifest declares -- and fills the typed Manifest fields from them where a parse is
// established.
//
// That ordering is deliberate and it is the same rule S5.4.5 applies to event bodies: a file recorded
// today stays correct for a layout nobody has decoded yet. W2-C found five identity fields the typed
// struct cannot carry, and four of the five are already inside these bodies:
//
//   1. the 20 loadout slot ids in wire order, with the catalogue they were resolved against pinned by
//      FileHeader::content_hash -- a catalogue reorder between capture and playback then fails a hash
//      instead of dressing the wrong slot;
//   2. the u32 at loadout header +0x3C, which nothing establishes the meaning of and which is therefore
//      exactly the kind of field that must be stored rather than reasoned about;
//   3. `alignment` at its true width. Appendix B.0's PlayerRecord::alignment is a uint8_t and the engine
//      value is a uint16_t whose 0xFFFF means "no alignment" -- a sentinel that does not fit its field
//      does not fail loudly, it becomes a valid-looking alignment. The typed field is left at its
//      default and the full-width value lives in IdentitySnapshot below. THE WIDENING OF B.0 IS RAISED,
//      NOT WORKED AROUND: this stores the truth somewhere the narrowing cannot reach it;
//   4. `unlocks` in the form it arrived in, which is the only form anything can be driven from;
//   5. the entrant count at capture time, so "this entrant was absent" is distinguishable from "we
//      failed to reconstruct them".
//
// ★ AND THE GENERAL RULE W2-C's own residual argues for: where two arrays are related by POSITION, store
// the key. IdentitySnapshot stores each loadout item beside its slot index rather than relying on array
// order, so a correspondence that holds today is written down instead of assumed.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include <vector>

#include "capture/capture_types.h"
#include "foundation/demofile/demotypes.h"

namespace replay {
namespace capture {

// The closed set of message types the capture registers a tap for. A type index in an S_EVENT record is
// an index into THIS table, and the table goes into the manifest, so a file says exactly which message
// types it could have recorded -- which is the honest scope statement for event coverage, and the thing
// W3-E's per-message-type disposition table is measured against.
struct TapType {
    const char* structName;   // as the binary spells it, leading 'S' included
    bool oneShot;             // identity and session state, as opposed to per-frame traffic
    bool ownStream;           // routed to its own stream rather than to S_EVENT
    StreamId stream;
};

const TapType* TapTable();
uint16_t TapTypeCount();

// The table's name for this packet's own identity snapshot, spelled ONCE: the row above, the
// by-design exclusion below and the producer that stamps a record with the row's index all take it
// from here, so a rename cannot leave a producer publishing under a name the table no longer has.
constexpr char kIdentitySnapshotTapName[] = "ReplayIdentitySnapshot";

// What TapTypeIndex answers when the table has no such row. Derived from the width rather than
// written out, and it is OUR bookkeeping value: no engine table is keyed on it.
constexpr uint16_t kTapTypeIndexNone = uint16_t(~uint16_t(0));

// Rows the capture never registers ON PURPOSE, so a reader can tell a deliberate omission from a
// dropped one. Our own ReplayIdentitySnapshot is not a wire message and nothing ever listens for it.
// The netvar row is NOT excluded: INetVar registers it, as the one S_NETVAR listener.
bool TapRowExcludedByDesign(const TapType&);

// What registration on the message bus has actually achieved, right now, in a form something other
// than the log can read. Without it a capture in which every tap was refused and then
// abandoned writes a file, and reports a wiretap, indistinguishable from a quiet match -- the observer
// asked IWiretap::Available(), which answers a different question: whether the tap could ever work,
// not whether anything is registered on it.
//
// ---- what the counts mean, and the two identities that hold over them --------------------------
//
// `tableRows` is every row of the tap table. `excluded` is the rows that are excluded BY DESIGN and
// were never going to be registered -- our own ReplayIdentitySnapshot type, which is not a wire
// message. `total` is what is left: every surface the capture must actually register, which is the
// reachable tap types PLUS the one S_NETVAR listener INetVar owns, because a capture whose netvars
// are empty is exactly as incomplete as one missing a tap.
//
//     registered + deferred + givenUp + unavailable == total
//     total + excluded == tableRows
//
// The second identity is the one L5 run 3 was missing: the log read "36 of 38 message taps
// registered", two rows unaccounted and unnamed, so a reader could not tell a row excluded on purpose
// from one that was dropped. Every row of the table is now in exactly one of the five buckets, and the
// registration log line names each row that is not in `registered`.
//
// `deferred` is "still being tried" -- asked and refused, or not asked yet this tick. `givenUp` is
// terminal after an attempt: abandoned at the attempt bound, or the handle table filled.
// `unavailable` is terminal WITHOUT an attempt: IWiretap::Available() was false, decided when the tap
// was built and never revisited, so nothing will ask again (AUDIT_M4 M1 -- a consumer waiting for
// `deferred` to drain on that path waits for the process to exit).
// `started` is whether a Listen has been ISSUED, for any surface. It is not "a tap object exists":
// an unavailable tap is built and asks nothing. Before the first tick carrying a
// CR15NetGame every count below reads "not asked yet" -- and a reader that took that for "asked and
// refused" would fail a launch that simply never entered a match. It is the difference between
// could-not-ask and asked-and-failed, which a count alone cannot carry.
//
// ---- THE THREAD RULE -------------------------------------------------------------
//
// TapRegistrationNow() may be called from ANY thread. It reads one atomic word published by the tick
// thread after each change, so the identities above hold on every sample -- where six plain loads of
// state the game thread is writing could be interleaved between two of them and return a struct whose
// own identity is broken, training a consumer to distrust the check.
//
// PublishTapRegistration() is the other half and is TICK THREAD ONLY: capture.cpp calls it, nothing
// else does, and it is declared here only because the snapshot it writes lives beside the reader.
struct TapRegistration {
    uint32_t registered;
    uint32_t deferred;
    uint32_t givenUp;
    uint32_t unavailable;
    uint32_t excluded;
    uint32_t total;
    uint32_t tableRows;
    bool     started;
};
TapRegistration TapRegistrationNow();
void PublishTapRegistration(const TapRegistration&);
// 0xFFFF when the name is not in the table. A message we did not register cannot arrive, so this is a
// lookup for the writer's own bookkeeping and never a runtime classification of unknown traffic.
uint16_t TapTypeIndex(const char* structName);

#pragma pack(push, 1)

// One entrant, at capture time, at full width. Written into S_EVENT under kTapIdentitySnapshot so it
// rides the same verbatim-body discipline as everything else identity-shaped.
// `magic` says WHAT this block is and never changes for the life of the record kind; `version` says
// how it is LAID OUT and is the field a layout change increments.  They are read differently: an
// unexpected magic is not our block at all (IdentityBlockUnknownMagic), while a version is a layout
// question -- see identity_block.h for the directional rule.
//
// `loadoutSlotStride` / `loadoutSlotCount` are APPENDED, so a v1 header is exactly the first
// kIdentityHeaderSizeV1 bytes of this one and a v1 block stays parseable byte for byte.  They exist
// because the trailing profile JSON sits past the slot array: a reader that sizes that array from a
// compiled sizeof lands inside the JSON the moment the two builds disagree, which misparses the
// PROFILE rather than merely losing the loadout.
struct IdentitySnapshotHeader {
    uint32_t magic;            // 'IDN1'
    uint32_t version;
    uint64_t tUs;
    uint16_t entrantCount;     // W2-C item 5: absent is distinguishable from unreconstructed
    uint16_t reserved;
    uint64_t contentHash;      // the catalogue the slot ids were resolved against
    uint16_t loadoutSlotStride;  // v2+: bytes per IdentityLoadoutSlot AS WRITTEN
    uint16_t loadoutSlotCount;   // v2+: how many of them follow each entrant
    uint16_t entrantStride;      // v3+: bytes per IdentityEntrant AS WRITTEN.  The entrant's width
                                 // decides where the slot array and the JSON length begin, and it
                                 // grew at v3; a reader that took it from its own sizeof would land
                                 // two bytes into a v2 block's first slot record and misparse the
                                 // whole entrant in silence.
};

struct IdentityEntrant {
    uint64_t xplatformId;
    uint16_t userSlot, userIndex, generation;
    uint16_t alignment;        // W2-C item 3: uint16_t, and 0xFFFF means no alignment
    uint8_t  teamId, teamSlot, bodyType;
    uint8_t  flags;            // kIdentityEntrantFlag*, below: v1 wrote a zero here and a zero still
                               // means "this recording never asked", never "not truncated"
    uint16_t loadoutNumber, loadoutNumberBody;
    uint32_t loadoutHeader3C;  // W2-C item 2: copied by operator=, meaning unestablished, so stored
    char     displayName[32];
    char     platformStr[32];
    // v3+ (ARCH-23 s14.8c F1): how many of the twenty slot records that follow are REAL pairs, i.e.
    // min(what the engine's component held, 20).  The header's loadoutSlotCount is the ARRAY WIDTH,
    // a per-block fact; this is a per-entrant one.  `kIdentityPairCountUnknown` when the capture
    // could not ask -- because ReadAppliedPairs zeroes its output first, a component holding n < 20
    // reaches disk as n real pairs followed by padding that is indistinguishable from slots the
    // capture lost, and a verification with no denominator either refuses everyone or compares
    // nothing and calls it a pass.  APPENDED: a v2 record is the first 92 bytes of this one.
    uint16_t filledPairCount;
    // `IdentitySnapshotHeader::loadoutSlotCount` IdentityLoadoutSlot records follow, each
    // `loadoutSlotStride` bytes apart, then a u32 length and the profile JSON bytes.  BOTH numbers
    // come from the header that was WRITTEN with this block -- never from sizeof(IdentityLoadoutSlot)
    // and never from the constant 20 -- because this struct's width is a per-version fact and the
    // JSON begins where the array ends.
};

// The entrant's own flags byte, at the offset v1 left reserved and zero.  THREE STATES, because two
// cannot hold this: the engine's customisation component may be wider than the twenty slots this
// format records (identity_access.cpp clamps to kLoadoutFormatMax), and a recording that clamped
// silently claims a complete loadout it does not have.  So `Known` says the capture was able to ask
// how many entries the component held, and `Truncated` is the answer -- a zero byte is "never
// asked", which is what every block written before this byte existed genuinely is.
//
// The profile document gets the same treatment: a document the engine REFUSED must
// not be byte-identical on disk to an entrant who genuinely has none.  `Asked` says the capture put
// the question, `Refused` is the answer, and the length word then separates present from absent.
enum : uint8_t {
    kIdentityEntrantSlotsKnown     = 1u << 0,
    kIdentityEntrantSlotsTruncated = 1u << 1,
    kIdentityEntrantProfileAsked   = 1u << 2,
    kIdentityEntrantProfileRefused = 1u << 3,
    // RECORDER-3.  Three facts about the document this block carries, in bits v3 left free.  A v3
    // reader ignores them; a v4 reader of a v3 block reads them clear, which is exactly what a
    // producer that could not tell would have written.
    kIdentityEntrantProfileFromEntrantRec = 1u << 4,  // the lobby record answered, not the user table
    kIdentityEntrantProfileLossyNumber    = 1u << 5,  // the walk wrote null over a non-integer number
    // The manifest row for this player was written with NO document (the file header cannot be
    // rewritten, so a document that arrives at t+3s can only reach the stream).  A reader building
    // a roster takes this block's document over the header's absence.
    kIdentityEntrantProfileSupersedes     = 1u << 6,
};

// The third state of `IdentityEntrant::filledPairCount`, and the value every block written before
// v3 reports.  It is not zero, because zero is an ANSWER -- the component held nothing -- and a
// resolver that reads "could not ask" as "no pairs" compares nothing and reports a pass.
constexpr uint16_t kIdentityPairCountUnknown = 0xFFFFu;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value

// v1 stopped at `itemId`, so the key the engine actually holds was not recorded and position was the
// only slot evidence.  `slotKey` is APPENDED rather than inserted: v1's 16 bytes are still the first
// 16 of this struct, which is what lets a v2 reader parse a v1 block field for field.
struct IdentityLoadoutSlot {
    uint16_t slotIndex;        // stored, not implied by position
    uint16_t reserved;
    uint32_t reserved2;
    Symbol64 itemId;
    Symbol64 slotKey;          // v2+: CustSlotEntry.key -- the engine's own key for this slot
};

#pragma pack(pop)

// The slot records an identity block writes per entrant.  Defined HERE, beside the struct whose
// trailing layout it describes; identity_block.h aliases it and asserts it against
// identity_access::kLoadoutFormatMax, which is where the twenty comes from.
constexpr uint16_t kIdentityBlockSlotCount = 20;

// ★ THE FORMAT'S MAXIMUM S_EVENT BODY, IN ONE PLACE.  The encoder and every decoder
// must agree on this or a body one of them cannot hold desynchronises the whole stream -- an
// oversized record makes EventDecoder return before it has consumed the body's bits, and every
// remaining row in that chapter, joins and round boundaries included, decodes as garbage.  The
// widest body S_EVENT carries is one entrant's identity block, so the bound is DERIVED from that
// record rather than chosen: header, entrant, its slot records at the current stride, the length
// word, and a whole profile document at the file's own cap.
constexpr uint32_t kMaxEventBodyBytes =
    uint32_t(sizeof(IdentitySnapshotHeader) + sizeof(IdentityEntrant) +
             sizeof(IdentityLoadoutSlot) * kIdentityBlockSlotCount + sizeof(uint32_t) +
             kPlayerJsonCap);

// Builds the manifest and owns everything its pointers borrow. The Manifest handed to IDemoWriter::Open
// is only valid while this object is.
class ManifestBuilder {
public:
    void Begin(uint64_t t0Us, uint32_t captureRole, uint64_t authorityFlags);

    void SetBuild(const uint8_t echovrMd5[16], uint64_t buildStringHash, uint64_t contentHash);
    void SetLevel(uint64_t levelId, uint64_t gametypeId, uint64_t startTimeEpoch);
    void SetKeyframeIntervalUs(uint32_t us);
    void SetPartialStart(bool v);
    void SetFlags(uint32_t flags);
    void SetUuid(uint64_t lo, uint64_t hi);

    // The typed per-player row the browser and W2-C read. `alignment` is deliberately NOT taken here:
    // the field is a byte and the engine value is a word, so the full-width value goes into the
    // identity snapshot and this row carries only what fits without a sentinel collision.
    void AddPlayer(const PlayerRecord&);

    void SetBalanceBlob(const void*, uint32_t);
    void SetSlot0Baselines(const void*, uint32_t);
    void SetSlot1RuleVars(const void*, uint32_t);
    void SetNetIdSlotMap(const void*, uint32_t);

    // Non-null if the schema table refuses; the caller must not open a file.
    const char* Refusal() const;

    const Manifest& View();

private:
    Manifest view_ = {};
    std::vector<PlayerRecord> players_;
    std::vector<uint8_t> arena_;
    std::vector<uint32_t> entrantOff_, entrantLen_, unlocksOff_, unlocksLen_;
    uint32_t balanceOff_ = 0, balanceLen_ = 0;
    uint32_t slot0Off_ = 0, slot0Len_ = 0;
    uint32_t slot1Off_ = 0, slot1Len_ = 0;
    uint32_t netIdOff_ = 0, netIdLen_ = 0;
    uint32_t Append(const void*, uint32_t);
    void Rebind();
};

// The two derivations OpenFile needs to turn what buildguard already measured into what the header
// records. Pure and free-standing so tests can drive every malformed input without a game present.

// 32 hex digits -> 16 bytes. Returns false and leaves `out` ALL ZERO for anything else -- a length
// that is not 32, a non-hex digit, a null. An md5 we could not read must record sixteen zeros, which
// readers classify as "not recorded"; it must never round to a wrong build.
bool Md5HexToBytes(const char* hex, uint8_t out[16]);

// FNV-1a 64 over the bytes of `s` (null -> 0). The header's buildStringHash is a fingerprint of the
// game's build string, and this is the only hash in the format that is computed from text.
uint64_t BuildStringHash64(const char* s);

}  // namespace capture
}  // namespace replay
