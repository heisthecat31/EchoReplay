#pragma once
// The identity snapshot block: its version rule, its writer, and the FIRST reader of it.
//
// Nothing read this block before W2-C-2 -- the three structs in manifestbuild.h were declarations
// with no parser -- so the rules below are this file's specification rather than a change to an
// inherited one (ARCH-23 s14.8b item 3).
//
// THE VERSION RULE, IN BOTH DIRECTIONS, because only stating one of them is how a format loses data:
//   * `magic` says what the block IS.  It never changes.  An unexpected one is not our block:
//     IdentityBlockUnknownMagic, and nothing is parsed.
//   * `version` says how the block is LAID OUT.  A reader recognises every version whose layout it
//     can derive -- that is, every version up to its own.  An OLDER block therefore parses
//     read-only and IN FULL (name, team, teamSlot, alignment, chassis, both loadout numbers, the
//     profile JSON); it reports HasSlotKeys() == false and ONLY ResolveLoadout refuses, by name,
//     LoadoutSlotKeysNotRecorded.  Refusing such a block outright would throw away identity the
//     recording genuinely holds, which is the same fidelity loss the pair format exists to stop.
//   * IdentityBlockUnknownVersion is reserved for a version NEWER than the reader's -- a layout it
//     cannot derive.  There the WHOLE block refuses and nothing is returned, because a half-parsed
//     block is indistinguishable from a correct one that happens to be sparse.
//
// THE OFFSET RULE.  Every trailing offset is computed from the strides and count the header RECORDS
// -- the entrant's own stride included -- never from sizeof(IdentityLoadoutSlot), never from
// sizeof(IdentityEntrant) and never from the constant 20.  At v1's 16-byte slot stride a
// sizeof-derived offset lands 160 bytes inside the profile JSON: a silent misparse of the PROFILE,
// not a missing loadout.  The entrant's width grew at v3, so it is recorded too: a v2 block's slot
// array starts two bytes before this build's struct ends.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "capture/manifestbuild.h"
#include "capture/roster.h"
#include "engine/identity_access.h"

namespace replay {
namespace capture {

// 'I','D','N','1' in that byte order on disk.
constexpr uint32_t kIdentityBlockMagic = uint32_t('I') | (uint32_t('D') << 8) |
                                         (uint32_t('N') << 16) | (uint32_t('1') << 24);

constexpr uint32_t kIdentityBlockVersionV1 = 1;  // slotIndex + itemId; no key
constexpr uint32_t kIdentityBlockVersionV2 = 2;  // IdentityLoadoutSlot carries slotKey
constexpr uint32_t kIdentityBlockVersion   = 3;  // IdentityEntrant carries filledPairCount,
                                                 // and the header records the entrant's own stride

// A FEATURE IS PINNED TO THE VERSION THAT INTRODUCED IT, never to the moving current one
// `version_ >= kIdentityBlockVersion` reads correctly on the day it is
// written and turns into "only the newest layout has this" at the next bump: v2 blocks genuinely
// carry slot keys, and a reader that answered false for them would throw away every key recorded
// before this packet -- the same fidelity loss the format's version rule exists to stop.
constexpr uint32_t kIdentityBlockVersionSlotKeys  = kIdentityBlockVersionV2;
constexpr uint32_t kIdentityBlockVersionPairCount = 3;

// v1's header is the first 28 bytes of the current one; stride and count were appended at v2, and
// the entrant stride at v3.  Each older header is a byte-for-byte PREFIX of the newer one.
constexpr size_t   kIdentityHeaderSizeV1 = 28;
constexpr size_t   kIdentityHeaderSizeV2 = 32;
constexpr uint16_t kIdentitySlotStrideV1 = 16;
constexpr uint16_t kIdentitySlotCount    = kIdentityBlockSlotCount;

static_assert(sizeof(IdentitySnapshotHeader) == kIdentityHeaderSizeV2 + 2,
              "the v3 header must be the v2 header plus exactly the appended entrant stride");
static_assert(offsetof(IdentitySnapshotHeader, loadoutSlotStride) == kIdentityHeaderSizeV1,
              "a v1 header must be a byte-for-byte prefix of a v2 one");
static_assert(offsetof(IdentitySnapshotHeader, entrantStride) == kIdentityHeaderSizeV2,
              "a v2 header must be a byte-for-byte prefix of a v3 one");

// Closed at v3 by AUD-ARCH-23-S14 F6.  The entrant's width decides two trailing
// offsets -- the slot array's base and the JSON length's position -- and until v3 the format
// recorded no entrant width to derive them from, so a compiled sizeof was the only source and a
// grown struct silently re-laid-out every record a previous build wrote.  v3 records the width
// (`IdentitySnapshotHeader::entrantStride`); these constants are what the OLDER versions wrote, and
// they are summed from the members rather than typed, so they cannot drift into a tautology.
constexpr size_t kIdentityEntrantSizeV2 =
    sizeof(uint64_t) +                              // xplatformId
    3 * sizeof(uint16_t) +                          // userSlot, userIndex, generation
    sizeof(uint16_t) +                              // alignment
    4 * sizeof(uint8_t) +                           // teamId, teamSlot, bodyType, flags
    2 * sizeof(uint16_t) +                          // loadoutNumber, loadoutNumberBody
    sizeof(uint32_t) +                              // loadoutHeader3C
    sizeof(IdentityEntrant::displayName) + sizeof(IdentityEntrant::platformStr);
constexpr size_t kIdentityEntrantSizeV1 = kIdentityEntrantSizeV2;   // v1 and v2 share the record
constexpr size_t kIdentityEntrantSizeV3 = kIdentityEntrantSizeV2 + sizeof(uint16_t);

static_assert(sizeof(IdentityEntrant) == kIdentityEntrantSizeV3,
              "IdentityEntrant's width decides two trailing offsets: a field added here re-lays-out "
              "every record, so bump the version and record the entrant width with it");
static_assert(offsetof(IdentityEntrant, filledPairCount) == kIdentityEntrantSizeV2,
              "filledPairCount is APPENDED: a v2 record must be a byte-for-byte prefix of a v3 one");
static_assert(sizeof(IdentityLoadoutSlot) == 24, "v2's slot record is 24 bytes");
static_assert(offsetof(IdentityLoadoutSlot, slotKey) == kIdentitySlotStrideV1,
              "slotKey must sit past v1's 16 bytes, so a v1 record is a prefix of a v2 one");
static_assert(kIdentitySlotCount == identity_access::kLoadoutFormatMax,
              "the block writes the same twenty slots the loadout format is bounded at");

// The band a scan walks is the widest entrant list that can reach this block, so it is the bound
// the reader validates against rather than a second number to drift from.
constexpr uint16_t kIdentityMaxEntrants = kRosterBandCap;

// A two-state result cannot say WHICH question could not be asked, and every one of these has a
// different repair.  The text is the word a census prints.
enum class IdentityBlockRefusal : uint8_t {
    None = 0,
    UnknownMagic,         // not our block at all
    UnknownVersion,       // NEWER than this reader; the whole block is refused
    Truncated,            // the bytes end inside a record the header promised
    BadSlotLayout,        // the recorded stride/count cannot hold the fields that version defines
    BadEntrantLayout,     // ... and the same for the recorded entrant stride
    TooManyEntrants,      // more entrants than a scan can produce; reported, never clamped
    NoSuchEntrant,        // an index past what this block holds
    SlotKeysNotRecorded,  // the block predates the key; ONLY the loadout resolver answers this
    SlotsTruncated,       // the engine held more slots than this format records -- see below
    SlotsTruncationUnknown,  // the capture could not ask how many it held; NOT a claim of either
    ProfileNotAsked,      // the block predates the profile bits, or the capture never put the question
    ProfileRefused,       // the engine was asked for the document and would not give it
};

inline const char* IdentityBlockRefusalText(IdentityBlockRefusal r) {
    switch (r) {
        case IdentityBlockRefusal::None:                return "none";
        case IdentityBlockRefusal::UnknownMagic:        return "IdentityBlockUnknownMagic";
        case IdentityBlockRefusal::UnknownVersion:      return "IdentityBlockUnknownVersion";
        case IdentityBlockRefusal::Truncated:           return "IdentityBlockTruncated";
        case IdentityBlockRefusal::BadSlotLayout:       return "IdentityBlockBadSlotLayout";
        case IdentityBlockRefusal::BadEntrantLayout:    return "IdentityBlockBadEntrantLayout";
        case IdentityBlockRefusal::TooManyEntrants:     return "IdentityBlockTooManyEntrants";
        case IdentityBlockRefusal::NoSuchEntrant:       return "IdentityBlockNoSuchEntrant";
        case IdentityBlockRefusal::SlotKeysNotRecorded: return "LoadoutSlotKeysNotRecorded";
        case IdentityBlockRefusal::SlotsTruncated:      return "LoadoutSlotsTruncated";
        case IdentityBlockRefusal::SlotsTruncationUnknown:
            return "LoadoutSlotsTruncationUnknown";
        case IdentityBlockRefusal::ProfileNotAsked:     return "IdentityProfileNotAsked";
        case IdentityBlockRefusal::ProfileRefused:      return "IdentityProfileRefused";
    }
    return "IdentityBlockRefusalUnnamed";
}

// The bytes one entrant contributed, located rather than copied: `fields` is copied out because the
// block is packed and may be unaligned, but the slot array and the JSON are pointers INTO the
// caller's buffer and live exactly as long as it does.
struct IdentityBlockEntrantView {
    IdentityEntrant fields;
    const uint8_t*  slots;    // `slotCount` records, `slotStride` bytes apart
    uint32_t        jsonLen;
    const uint8_t*  json;
};

// What one entrant is written from.  `slots` is always in the CURRENT struct, so the writer owns
// the only place the on-disk stride is decided.
struct IdentityBlockEntrantIn {
    IdentityEntrant            fields;
    const IdentityLoadoutSlot* slots;      // kIdentitySlotCount of them, or nullptr for none
    Blob                       profileJson;
};

// Appends into a caller-owned buffer; `Finish` patches the count back into the header, so a writer
// that dies mid-block leaves a header claiming nothing rather than a header claiming records that
// are not there.
class IdentityBlockWriter {
public:
    IdentityBlockWriter(void* buf, size_t cap, uint64_t tUs, uint64_t contentHash)
        : buf_(static_cast<uint8_t*>(buf)), cap_(cap) {
        if (!buf_ || cap_ < sizeof(IdentitySnapshotHeader)) {
            full_ = true;
            return;
        }
        IdentitySnapshotHeader h{};
        h.magic = kIdentityBlockMagic;
#ifdef W2A3_MUTATE_VERSION_NOT_BUMPED
        h.version = kIdentityBlockVersionV2;   // a grown entrant filed under the old layout
#else
        h.version = kIdentityBlockVersion;
#endif
        h.tUs = tUs;
        h.entrantCount = 0;
        h.reserved = 0;
        h.contentHash = contentHash;
        h.loadoutSlotStride = uint16_t(sizeof(IdentityLoadoutSlot));
        h.loadoutSlotCount = kIdentitySlotCount;
        // The width this writer's records ACTUALLY are, so a later build's reader derives the slot
        // base and the JSON position from what was written instead of from its own struct.
        h.entrantStride = uint16_t(sizeof(IdentityEntrant));
        memcpy(buf_, &h, sizeof h);
        len_ = sizeof h;
    }

    bool Add(const IdentityBlockEntrantIn& in) {
        if (full_) return false;
        if (count_ >= kIdentityMaxEntrants) return Full();
        const uint32_t jsonLen = in.profileJson.p ? in.profileJson.n : 0;
        const size_t need = sizeof(IdentityEntrant) +
                            sizeof(IdentityLoadoutSlot) * kIdentitySlotCount + sizeof(uint32_t) +
                            jsonLen;
        if (len_ + need > cap_) return Full();

        memcpy(buf_ + len_, &in.fields, sizeof(IdentityEntrant));
        len_ += sizeof(IdentityEntrant);
        for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
            IdentityLoadoutSlot s{};
            if (in.slots) s = in.slots[i];
            memcpy(buf_ + len_, &s, sizeof s);
            len_ += sizeof s;
        }
        memcpy(buf_ + len_, &jsonLen, sizeof jsonLen);
        len_ += sizeof jsonLen;
        if (jsonLen) memcpy(buf_ + len_, in.profileJson.p, jsonLen);
        len_ += jsonLen;
        ++count_;
        return true;
    }

    // False when anything was refused along the way: a short buffer must not produce a block that
    // parses cleanly and is missing players.
    bool Finish(size_t& written) {
        written = full_ ? 0 : len_;
        if (full_) return false;
        IdentitySnapshotHeader h{};
        memcpy(&h, buf_, sizeof h);
        h.entrantCount = count_;
        memcpy(buf_, &h, sizeof h);
        return true;
    }

    uint16_t Count() const { return count_; }

private:
    bool Full() {
        full_ = true;
        return false;
    }
    uint8_t* buf_ = nullptr;
    size_t   cap_ = 0;
    size_t   len_ = 0;
    uint16_t count_ = 0;
    bool     full_ = false;
};

class IdentityBlockReader {
public:
    // False and a named refusal, with nothing readable afterwards.  Validated in full here rather
    // than per entrant, so a truncated block cannot hand out the entrants that happen to fit.
    bool Open(const void* bytes, size_t len) {
        const uint32_t pinned = pinnedVersion_;   // PinVersion is set BEFORE Open; a blanket reset
        *this = IdentityBlockReader{};            // would silently unpin the reader under test
        pinnedVersion_ = pinned;
        base_ = static_cast<const uint8_t*>(bytes);
        if (!base_ || len < kIdentityHeaderSizeV1) return Refuse(IdentityBlockRefusal::Truncated);

        IdentitySnapshotHeader h{};
        memcpy(&h, base_, kIdentityHeaderSizeV1);
#ifndef W2C2_MUTATE_MAGIC_UNCHECKED
        if (h.magic != kIdentityBlockMagic) return Refuse(IdentityBlockRefusal::UnknownMagic);
#endif
#if !defined(W2C2_MUTATE_ACCEPT_ANY_VERSION) && !defined(W2C2_MUTATE_PARTIAL_PARSE_ON_NEWER_VERSION)
        if (h.version > pinnedVersion_) return Refuse(IdentityBlockRefusal::UnknownVersion);
#endif
#ifdef W2C2_MUTATE_PARTIAL_PARSE_ON_NEWER_VERSION
        if (h.version > pinnedVersion_) h.version = pinnedVersion_;  // parse what we recognise
#endif
#ifdef W2C2_MUTATE_REFUSE_OLDER_VERSION
        if (h.version < pinnedVersion_) return Refuse(IdentityBlockRefusal::UnknownVersion);
#endif
        if (h.version < kIdentityBlockVersionV1) return Refuse(IdentityBlockRefusal::UnknownVersion);
        version_ = h.version;

        // The layout the block was WRITTEN with.  v1 recorded neither number, so v1's constants are
        // the derivation -- which is exactly what "a reader recognises every version whose layout it
        // can derive" means in code.
        size_t headerSize = kIdentityHeaderSizeV1;
        entrantStride_ = uint16_t(kIdentityEntrantSizeV1);
        if (version_ >= kIdentityBlockVersionV2) {
            headerSize = version_ >= kIdentityBlockVersionPairCount ? sizeof(IdentitySnapshotHeader)
                                                                   : kIdentityHeaderSizeV2;
            if (len < headerSize) return Refuse(IdentityBlockRefusal::Truncated);
            memcpy(&h, base_, headerSize);
            slotStride_ = h.loadoutSlotStride;
            slotCount_ = h.loadoutSlotCount;
            if (slotStride_ < sizeof(IdentityLoadoutSlot) || slotCount_ != kIdentitySlotCount)
                return Refuse(IdentityBlockRefusal::BadSlotLayout);
            entrantStride_ = uint16_t(kIdentityEntrantSizeV2);
            if (version_ >= kIdentityBlockVersionPairCount) {
                // The width the block RECORDS. A v3 record shorter than this build's struct cannot
                // hold the fields v3 defines, and parsing it would fabricate the tail out of the
                // bytes that follow the record.
                entrantStride_ = h.entrantStride;
                if (entrantStride_ < sizeof(IdentityEntrant))
                    return Refuse(IdentityBlockRefusal::BadEntrantLayout);
            }
        } else {
            slotStride_ = kIdentitySlotStrideV1;
            slotCount_ = kIdentitySlotCount;
        }
        tUs_ = h.tUs;
        contentHash_ = h.contentHash;

        if (h.entrantCount > kIdentityMaxEntrants)
            return Refuse(IdentityBlockRefusal::TooManyEntrants);

        size_t at = headerSize;
        for (uint16_t i = 0; i < h.entrantCount; ++i) {
            offsets_[i] = at;
            const size_t fixed = size_t(entrantStride_) + SlotBytes() + sizeof(uint32_t);
            if (at + fixed > len) return Refuse(IdentityBlockRefusal::Truncated);
            uint32_t jsonLen = 0;
            memcpy(&jsonLen, base_ + at + entrantStride_ + SlotBytes(), sizeof jsonLen);
            if (at + fixed + jsonLen > len) return Refuse(IdentityBlockRefusal::Truncated);
            at += fixed + jsonLen;
        }
        count_ = h.entrantCount;
        len_ = len;
        return true;
    }

    bool Entrant(uint16_t i, IdentityBlockEntrantView& out) const {
        if (i >= count_) return Refuse(IdentityBlockRefusal::NoSuchEntrant);
        const uint8_t* p = base_ + offsets_[i];
        // The bytes an OLDER record carries, into a zeroed struct: the tail this build's struct has
        // and that block does not stays at zero and is then given its absence value below, never
        // filled from whatever follows the record.
        out.fields = IdentityEntrant{};
        const size_t copy = entrantStride_ < sizeof(IdentityEntrant) ? size_t(entrantStride_)
                                                                     : sizeof(IdentityEntrant);
#ifdef W2A3_MUTATE_STRIDE_FROM_SIZEOF_ENTRANT
        memcpy(&out.fields, p, sizeof(IdentityEntrant));
        out.slots = p + sizeof(IdentityEntrant);
#else
        memcpy(&out.fields, p, copy);
        out.slots = p + entrantStride_;
#endif
        // A count out of a block written before the field existed is NOT zero -- zero is the answer
        // "the component held nothing", and a resolver reading it would compare nothing and pass.
#ifndef W2A3_MUTATE_OLDER_VERSION_COUNT_GUESSED
        if (version_ < kIdentityBlockVersionPairCount)
            out.fields.filledPairCount = kIdentityPairCountUnknown;
#endif
        // THE OFFSET, from what the block recorded.  The mutant below is what a compiled sizeof
        // does to a v1 block: it lands inside the JSON and the profile is misparsed in silence.
#if defined(W2C2_MUTATE_OFFSET_FROM_SIZEOF) || defined(W2A3_MUTATE_STRIDE_FROM_SIZEOF_ENTRANT)
        const size_t past = sizeof(IdentityEntrant) + sizeof(IdentityLoadoutSlot) * kIdentitySlotCount;
#else
        const size_t past = size_t(entrantStride_) + SlotBytes();
#endif
        memcpy(&out.jsonLen, p + past, sizeof out.jsonLen);
        out.json = p + past + sizeof(uint32_t);
        return true;
    }

    // The loadout resolver, and the ONLY way slot identity leaves this block.  A block written
    // before the key existed refuses here BY NAME -- it never returns a position dressed as a key,
    // because a replay dressed from a guessed slot map wears the wrong things and looks fine.
    bool ResolveLoadout(uint16_t entrant, IdentityLoadoutSlot out[kIdentitySlotCount],
                        uint16_t& n) const {
        memset(out, 0, sizeof(IdentityLoadoutSlot) * kIdentitySlotCount);
        IdentityBlockEntrantView v{};
        if (!Entrant(entrant, v)) return false;
#ifndef W2C2_MUTATE_RESOLVER_GUESSES_BY_POSITION
        if (!HasSlotKeys()) return Refuse(IdentityBlockRefusal::SlotKeysNotRecorded);
#endif
        for (uint16_t i = 0; i < slotCount_; ++i) {
            const uint8_t* rec = v.slots + size_t(i) * slotStride_;
            const size_t copy = slotStride_ < sizeof(IdentityLoadoutSlot)
                                    ? size_t(slotStride_)
                                    : sizeof(IdentityLoadoutSlot);
            memcpy(&out[i], rec, copy);
#ifdef W2C2_MUTATE_RESOLVER_GUESSES_BY_POSITION
            if (!HasSlotKeys()) out[i].slotKey = i;
#endif
        }
        n = slotCount_;
        refusal_ = IdentityBlockRefusal::None;
        return true;
    }

    // Whether the engine's customisation component was WIDER than the twenty slots
    // this format records.  Returns false when the recording never asked -- `LoadoutSlotsTruncationUnknown`
    // -- because "we did not ask" and "it was not truncated" are different facts and a dresser that
    // reads the second off the first announces a complete loadout it never verified.  The twenty
    // pairs ResolveLoadout returns are TRUE either way; this says whether they are ALL of them.
    bool LoadoutTruncated(uint16_t entrant, bool& truncated) const {
        truncated = false;
        IdentityBlockEntrantView v{};
        if (!Entrant(entrant, v)) return false;
        if ((v.fields.flags & kIdentityEntrantSlotsKnown) == 0)
            return Refuse(IdentityBlockRefusal::SlotsTruncationUnknown);
        truncated = (v.fields.flags & kIdentityEntrantSlotsTruncated) != 0;
        if (truncated) {
            refusal_ = IdentityBlockRefusal::SlotsTruncated;
        } else {
            refusal_ = IdentityBlockRefusal::None;
        }
        return true;
    }

    // THREE STATES for the profile document, because a refused one and an absent one
    // are the same zero bytes on disk: `present` is only meaningful when this returns true, and it
    // refuses BY NAME when the document was asked for and refused (IdentityProfileRefused) or when
    // the block never put the question at all (IdentityProfileNotAsked -- which is what every block
    // written before these bits existed genuinely is).  The engine's own word for a refusal is a
    // per-capture fact and lives in the census line, not in the block.
    bool ProfileRecorded(uint16_t entrant, bool& present) const {
        present = false;
        IdentityBlockEntrantView v{};
        if (!Entrant(entrant, v)) return false;
        if ((v.fields.flags & kIdentityEntrantProfileAsked) == 0)
            return Refuse(IdentityBlockRefusal::ProfileNotAsked);
        if (v.fields.flags & kIdentityEntrantProfileRefused)
            return Refuse(IdentityBlockRefusal::ProfileRefused);
        present = v.jsonLen != 0;
        refusal_ = IdentityBlockRefusal::None;
        return true;
    }

    // RECORDER-3.  The three facts about a recorded document that the reader side needs in order to
    // report it honestly: which of the engine's two storages answered, whether the walk had to drop
    // a non-integer number, and whether this block's document supersedes a manifest row that was
    // written -- before the document existed -- with none.  A block from any earlier version reads
    // all three clear, which is exactly what a producer that could not tell would have written.
    bool ProfileFlags(uint16_t entrant, bool& fromEntrantRecord, bool& lossyNumber,
                      bool& supersedesManifest) const {
        fromEntrantRecord = lossyNumber = supersedesManifest = false;
        IdentityBlockEntrantView v{};
        if (!Entrant(entrant, v)) return false;
        fromEntrantRecord = (v.fields.flags & kIdentityEntrantProfileFromEntrantRec) != 0;
        lossyNumber = (v.fields.flags & kIdentityEntrantProfileLossyNumber) != 0;
        supersedesManifest = (v.fields.flags & kIdentityEntrantProfileSupersedes) != 0;
        refusal_ = IdentityBlockRefusal::None;
        return true;
    }

    // ARCH-23 s14.8c F1.  How many of the twenty slot records are REAL pairs -- the denominator the
    // loadout verification counts against.  `kIdentityPairCountUnknown` when the capture could not
    // ask, and for every block written before v3.  What to DO about an unknown count is the
    // resolver's ruling; here it is reported, never guessed and never flattened into a zero.
    bool FilledPairCount(uint16_t entrant, uint16_t& count) const {
        count = kIdentityPairCountUnknown;
        IdentityBlockEntrantView v{};
        if (!Entrant(entrant, v)) return false;
        count = v.fields.filledPairCount;
        refusal_ = IdentityBlockRefusal::None;
        return true;
    }

    // v1 carried no key, so the field exists in memory and is not a fact.  A caller that skips this
    // and reads slotKey anyway reads a zero, which is why ResolveLoadout is the only accessor.
    // Pinned to the version that INTRODUCED the key rather than the current one -- see the constant.
#ifdef W2A3_MUTATE_HASSLOTKEYS_MOVING_VERSION
    bool HasSlotKeys() const { return version_ >= kIdentityBlockVersion; }
#else
    bool HasSlotKeys() const { return version_ >= kIdentityBlockVersionSlotKeys; }
#endif
    bool HasPairCount() const { return version_ >= kIdentityBlockVersionPairCount; }

    uint16_t Count() const { return count_; }
    uint32_t Version() const { return version_; }
    uint16_t SlotStride() const { return slotStride_; }
    uint16_t EntrantStride() const { return entrantStride_; }
    uint16_t SlotCount() const { return slotCount_; }
    uint64_t TimeUs() const { return tUs_; }
    uint64_t ContentHash() const { return contentHash_; }
    size_t   Size() const { return len_; }
    IdentityBlockRefusal Refusal() const { return refusal_; }

    // A reader pinned to an older version, for the direction that has no fixture: what a build that
    // predates a future format bump does when this build's block reaches it.
    void PinVersion(uint32_t v) { pinnedVersion_ = v; }

private:
    size_t SlotBytes() const { return size_t(slotStride_) * slotCount_; }
    bool Refuse(IdentityBlockRefusal r) const {
        refusal_ = r;
        return false;
    }

    const uint8_t* base_ = nullptr;
    size_t   len_ = 0;
    size_t   offsets_[kIdentityMaxEntrants] = {};
    uint16_t count_ = 0;
    uint32_t version_ = 0;
    uint16_t slotStride_ = 0;
    uint16_t slotCount_ = 0;
    uint16_t entrantStride_ = 0;
    uint64_t tUs_ = 0;
    uint64_t contentHash_ = 0;
    uint32_t pinnedVersion_ = kIdentityBlockVersion;
    mutable IdentityBlockRefusal refusal_ = IdentityBlockRefusal::None;
};

// The 20/20 acceptance's arithmetic, by SLOT KEY.  Its two sides are the RECORDED pairs and the
// pairs read back OUT OF THE ENGINE after the apply -- never the buffer the dresser authored, which
// is green whenever nothing reached the engine at all.
struct LoadoutMatch {
    uint16_t compared   = 0;  // recorded slots carrying a key
    uint16_t matched    = 0;  // ... whose item the engine holds under that same key
    uint16_t missing    = 0;  // ... whose key the engine does not hold at all
    uint16_t mismatched = 0;  // ... present under that key, holding something else
};

inline LoadoutMatch CompareLoadoutByKey(const IdentityLoadoutSlot* recorded, uint16_t nRecorded,
                                        const identity_access::LoadoutPair* engine,
                                        uint64_t nEngine) {
    LoadoutMatch m;
    for (uint16_t i = 0; i < nRecorded; ++i) {
        if (recorded[i].slotKey == 0) continue;
        ++m.compared;
        bool found = false;
        for (uint64_t j = 0; j < nEngine; ++j) {
#ifdef W2C2_MUTATE_COMPARE_AUTHORED_SIDE
            const uint64_t key = recorded[i].slotKey;
            const uint64_t item = recorded[i].itemId;
#else
            const uint64_t key = engine[j].slotKey;
            const uint64_t item = engine[j].itemId;
#endif
            if (key != recorded[i].slotKey) continue;
            found = true;
            if (item == recorded[i].itemId) {
                ++m.matched;
            } else {
                ++m.mismatched;
            }
            break;
        }
        if (!found) ++m.missing;
    }
    return m;
}

// One roster row, in the shape the block writes.  The keys come from the row's own key array and
// the fill bit decides whether they are written at all: a row that never recorded keys writes zeros
// and the reader's HasSlotKeys stays a statement about the BLOCK's version, not about one row.
inline void IdentityEntrantFromRosterRow(const RosterRow& r, IdentityEntrant& e,
                                         IdentityLoadoutSlot slots[kIdentitySlotCount]) {
    e = IdentityEntrant{};
    e.xplatformId = (r.fill & kRosterFillXplat) ? r.xplatformId : 0;
    e.userSlot = r.userSlot;
    e.userIndex = r.userIndex;
    e.generation = (r.fill & kRosterFillGen) ? r.generation : 0;
    e.alignment = (r.fill & kRosterFillAlign) ? r.alignment : kRosterNoAlign;
    e.teamId = (r.fill & kRosterFillTeam) && r.team <= 0xFF ? uint8_t(r.team) : 0xFFu;
    e.teamSlot = (r.fill & kRosterFillTeam) && r.teamSlot <= 0xFF ? uint8_t(r.teamSlot) : 0xFFu;
    e.loadoutNumber = (r.fill & kRosterFillNumbers) ? r.loadoutNumber : 0;
    e.loadoutNumberBody = (r.fill & kRosterFillNumbers) ? r.loadoutNumberBody : 0;
    memcpy(e.displayName, r.displayName, sizeof e.displayName);
    memcpy(e.platformStr, r.platformStr, sizeof e.platformStr);

    // The truncation state, and it stays UNKNOWN unless the scan actually got the engine's count:
    // a source that cannot ask leaves the fill bit clear, and clear means unknown rather than fine.
    // ASSIGNED, not OR-ed into whatever the caller's struct held: every other field
    // here is assigned, and a reused struct would otherwise carry the previous entrant's claim.
    //
    // The DENOMINATOR travels with them (ARCH-23 s14.8c F1): how many of the twenty records that
    // follow are real pairs, from the same one walk the truncation bits come from.  A component
    // wider than the format contributes twenty -- twenty is what the block records -- and a source
    // that could not ask contributes the unknown value, never a confident zero.
    e.flags = 0;
    e.filledPairCount = kIdentityPairCountUnknown;
    if (r.fill & kRosterFillLoadoutAvail) {
        e.flags |= kIdentityEntrantSlotsKnown;
        if (r.loadoutSlotsAvailable > kIdentitySlotCount) {
            e.flags |= kIdentityEntrantSlotsTruncated;
        }
#ifndef W2A3_MUTATE_COUNT_NOT_WRITTEN
#ifdef W2A3_MUTATE_COUNT_FROM_ARRAY_WIDTH
        e.filledPairCount = kIdentitySlotCount;   // the ARRAY's width read back as the row's fill
#else
        e.filledPairCount = r.loadoutSlotsAvailable > kIdentitySlotCount
                                ? kIdentitySlotCount
                                : r.loadoutSlotsAvailable;
#endif
#endif
    }

    memset(slots, 0, sizeof(IdentityLoadoutSlot) * kIdentitySlotCount);
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        slots[i].slotIndex = i;
        if (r.fill & kRosterFillLoadout) slots[i].itemId = r.loadoutSlots[i];
        if (r.fill & kRosterFillLoadoutKeys) slots[i].slotKey = r.loadoutSlotKeys[i];
    }
}

}  // namespace capture
}  // namespace replay
