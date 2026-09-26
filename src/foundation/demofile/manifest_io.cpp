#include <stdio.h>

#include "foundation/demofile/manifest_io.h"

#include <stddef.h>
#include <string.h>

// For kMaxChapterPayload: the player-blob cap is derived from the payload the manifest has to fit in,
// so the number the codec enforces and the number the container enforces are one number.
#include "foundation/demofile/format.h"

namespace replay {
namespace {

#pragma pack(push, 1)
struct ManifestFixedDisk {
    uint64_t levelId;
    uint64_t gametypeId;
    uint64_t startTimeEpoch;
    uint32_t keyframeIntervalUs;
    uint8_t partialStart;
    uint8_t pad[3];
};

// v0 is the first 254 bytes and ends at `unlocksJsonLen`; v1 appends the block after it. The two blob
// lengths stay inside v0 on purpose -- they size the rest of the record, so a reader that has only read
// the v0 prefix already knows where the blobs begin and how long the fixed part must be.
struct PlayerFixedDisk {
    uint64_t xplatformId;
    char platformStr[32];
    char displayName[32];
    uint16_t userSlot, userIndex, generation;
    uint8_t teamId, teamSlot, alignment, bodyType;
    uint16_t loadoutNumber, loadoutNumberBody;
    uint64_t loadoutSlots[20];
    uint32_t entrantJsonLen;
    uint32_t unlocksJsonLen;

    char displayNameFull[36];
    char platformStrFull[36];
    uint16_t teamId16, teamSlot16, alignment16;
    uint64_t firstSeenUs, lastSeenUs;
    uint32_t fill;
    uint8_t jsonTruncated;
    uint8_t reserved[3];

    uint32_t fillExt;
};

struct SchemaFixedDisk {
    uint16_t id;
    uint8_t retention;
    uint8_t required;
    uint16_t fieldCount;
};

// Provenance is part of the field's parameters, not decoration: the codec refuses a field whose
// parameters nobody derived, so a manifest that dropped it would turn every stream unreadable.
struct FieldDefDisk {
    uint16_t id;
    uint8_t cls;
    uint8_t rawBits;
    uint8_t prov;
    uint8_t pad[3];
    float min, max, step;
};
#pragma pack(pop)

static_assert(sizeof(ManifestFixedDisk) == 32, "manifest fixed block is 32 bytes");
// The version boundary, pinned from both sides: v0's last byte is the end of unlocksJsonLen, and every
// v1 field sits behind it. A field inserted rather than appended moves the whole extension and the
// offset asserts below are what stops that from being a silent reinterpretation of old files.
constexpr uint32_t kPlayerFixedV0Bytes = 254;
constexpr uint32_t kPlayerExtV1Bytes = 102;
// v2 appends the extension fill word and nothing else. A v1 reader takes its own 102 bytes and
// skips the tail, which is what makes a file this build writes open unchanged in the old one.
constexpr uint32_t kPlayerExtV2Bytes = 106;

// Every v1 offset is stated as its distance from the version boundary, which is what it actually is:
// the two names carry the layout and no offset appears as a number of its own.
static_assert(sizeof(PlayerFixedDisk) == kPlayerFixedV0Bytes + kPlayerExtV2Bytes,
              "the player record is the v0 block plus the v1 and v2 blocks, and nothing else");
static_assert(offsetof(PlayerFixedDisk, fillExt) == kPlayerFixedV0Bytes + kPlayerExtV1Bytes,
              "the v2 block starts exactly where v1 ended");
static_assert(offsetof(PlayerFixedDisk, entrantJsonLen) == kPlayerFixedV0Bytes - 8,
              "the two blob lengths are the last eight bytes of v0");
static_assert(offsetof(PlayerFixedDisk, unlocksJsonLen) == kPlayerFixedV0Bytes - 4,
              "the two blob lengths are the last eight bytes of v0");
static_assert(offsetof(PlayerFixedDisk, displayNameFull) == kPlayerFixedV0Bytes,
              "the v1 block starts exactly where v0 ended");
static_assert(offsetof(PlayerFixedDisk, teamId16) == kPlayerFixedV0Bytes + 72,
              "the v1 team block follows the two 36-byte names");
static_assert(offsetof(PlayerFixedDisk, firstSeenUs) == kPlayerFixedV0Bytes + 78,
              "the v1 timestamps follow the team block");
static_assert(offsetof(PlayerFixedDisk, fill) == kPlayerFixedV0Bytes + 94,
              "the v1 fill mask follows the timestamps");
static_assert(offsetof(PlayerFixedDisk, jsonTruncated) == kPlayerFixedV0Bytes + 98,
              "the v1 truncation flags follow the fill mask");

// The floor's own arithmetic, stated rather than asserted by hand: a full roster of rows at the
// GUARANTEED width plus the budget kept for schemas, fields, markers and the round table has to fit
// one chapter payload. The per-blob cap is four times the floor and cannot be asserted this way --
// see kPlayerJsonFloorCap -- so the serialiser enforces the same bound at run time instead.
static_assert(kPlayerJsonRosterBudget * (2ull * kPlayerJsonFloorCap + sizeof(PlayerFixedDisk) + 4) +
                      kManifestNonRosterBudget <=
                  kMaxChapterPayload,
              "a full roster of floor-width player rows must still fit one chapter payload");
static_assert(kPlayerJsonFloorCap <= kPlayerJsonCap, "the floor cannot exceed the cap");

// Every byte the roster's documents may spend between them. Derived from the same three terms the
// assert above uses, so raising either budget moves this with it.
constexpr uint64_t kPlayerJsonTotalBudget =
    kMaxChapterPayload - kManifestNonRosterBudget -
    kPlayerJsonRosterBudget * (sizeof(PlayerFixedDisk) + 4);
static_assert(sizeof(SchemaFixedDisk) == 6, "schema fixed block is 6 bytes");
static_assert(sizeof(FieldDefDisk) == 20, "field def is 20 bytes");

constexpr uint32_t kManifestBlobCount = 4;

// Smallest a record of each kind can be on disk. A count field is checked against the bytes that are
// actually left before it sizes anything, so a malformed manifest is a named refusal rather than an
// allocation the payload could never have justified.
constexpr uint32_t kMinPlayerRecordBytes = 4 + 254;
constexpr uint32_t kMinSchemaRecordBytes = 4 + 6;

template <typename T>
void Put(std::vector<uint8_t>& out, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

void PutBytes(std::vector<uint8_t>& out, const void* p, uint32_t n) {
    if (n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
}

class Cursor {
public:
    Cursor(const uint8_t* p, uint32_t n) : p_(p), n_(n) {}

    bool Take(void* dst, uint32_t bytes) {
        if (bytes > n_ - at_) {
            return false;
        }
        memcpy(dst, p_ + at_, bytes);
        at_ += bytes;
        return true;
    }

    // Skips forward to `absolute`, refusing to move backwards: a record claiming a smaller size than it
    // already consumed would otherwise re-read its own tail as the next record.
    bool SeekTo(uint32_t absolute) {
        if (absolute < at_ || absolute > n_) {
            return false;
        }
        at_ = absolute;
        return true;
    }

    const uint8_t* Ptr() const { return p_ + at_; }
    uint32_t At() const { return at_; }
    uint32_t Left() const { return n_ - at_; }

private:
    const uint8_t* p_;
    uint32_t n_;
    uint32_t at_ = 0;
};

}  // namespace

void SerializeManifest(const Manifest& m, std::vector<uint8_t>& out) {
    out.clear();

    ManifestFixedDisk fixed{};
    fixed.levelId = m.levelId;
    fixed.gametypeId = m.gametypeId;
    fixed.startTimeEpoch = m.startTimeEpoch;
    fixed.keyframeIntervalUs = m.keyframeIntervalUs;
    fixed.partialStart = m.partialStart ? 1u : 0u;
    Put<uint32_t>(out, sizeof(fixed));
    Put(out, fixed);

    Put<uint16_t>(out, m.playerCount);
    uint64_t jsonBudgetLeft = kPlayerJsonTotalBudget;
    for (uint16_t i = 0; i < m.playerCount; ++i) {
        const PlayerRecord& s = m.players[i];
        PlayerFixedDisk d{};
        d.xplatformId = s.xplatformId;
        memcpy(d.platformStr, s.platformStr, sizeof(d.platformStr));
        memcpy(d.displayName, s.displayName, sizeof(d.displayName));
        d.userSlot = s.userSlot;
        d.userIndex = s.userIndex;
        d.generation = s.generation;
        d.teamId = s.teamId;
        d.teamSlot = s.teamSlot;
        d.alignment = s.alignment;
        d.bodyType = s.bodyType;
        d.loadoutNumber = s.loadoutNumber;
        d.loadoutNumberBody = s.loadoutNumberBody;
        memcpy(d.loadoutSlots, s.loadoutSlots, sizeof(d.loadoutSlots));

        memcpy(d.displayNameFull, s.displayNameFull, sizeof(d.displayNameFull));
        memcpy(d.platformStrFull, s.platformStrFull, sizeof(d.platformStrFull));
        // The full-width field is the authoritative one, so a producer that only knows B.0's 32-byte
        // field still gets its name onto disk instead of losing it to an empty v1 slot.
        if (d.displayNameFull[0] == 0) {
            memcpy(d.displayNameFull, s.displayName, sizeof(s.displayName));
        }
        if (d.platformStrFull[0] == 0) {
            memcpy(d.platformStrFull, s.platformStr, sizeof(s.platformStr));
        }
        // `fill` is the producer's claim about what it actually sourced, and a value it does not claim
        // is not written: that is what stops a zero-initialised row from arriving on disk as "team 0"
        // and being indistinguishable from a row whose team the capture could never read.
        const bool haveTeam = (s.fill & kPlayerFillTeam) != 0;
        d.teamId16 = haveTeam ? s.teamId16 : kPlayerNoTeam16;
        d.teamSlot16 = haveTeam ? s.teamSlot16 : kPlayerNoTeam16;
        d.alignment16 = (s.fill & kPlayerFillAlign) ? s.alignment16 : kPlayerNoAlignment16;
        d.firstSeenUs = s.firstSeenUs;
        d.lastSeenUs = s.lastSeenUs;
        d.fill = s.fill & kPlayerFillAll;
        d.fillExt = s.fillExt & kPlayerFillExtAll;

        // The ONE truncation site. A blob longer than what it may take is stored at exactly that
        // length with its bit set: the length on disk is the length stored, so a reader never has to
        // guess whether the bytes it got are the whole document.
        //
        // Two bounds, and the row meets the tighter of them. The per-blob cap is the format's; the
        // allowance is this manifest's remaining budget less the floor the rows behind this one are
        // guaranteed, which is what keeps a wide early document from starving a full roster and what
        // keeps the whole manifest inside one chapter payload.
        const uint64_t reserve =
            uint64_t(m.playerCount - 1 - i) * 2ull * kPlayerJsonFloorCap;
        uint64_t allow = jsonBudgetLeft > reserve ? jsonBudgetLeft - reserve : 0;
        d.jsonTruncated = 0;
        // The row's allowance is shared by its TWO blobs, so the first one may not eat the floor the
        // second is owed: `allow` is at least two floors by the reserve above, and this hands one of
        // them to the unlocks document before the profile is measured against the rest.
        // Unsigned, so the subtraction is guarded rather than trusted: a roster wider than the budget
        // was sized for leaves `allow` below one floor, and both blobs are then stored short.
        const uint64_t entrantRoom =
            allow > kPlayerJsonFloorCap ? allow - kPlayerJsonFloorCap : 0;
        const uint32_t entrantAllow =
            uint32_t(entrantRoom < kPlayerJsonCap ? entrantRoom : uint64_t(kPlayerJsonCap));
        d.entrantJsonLen = s.entrantJson.n;
        if (d.entrantJsonLen > entrantAllow) {
            d.entrantJsonLen = entrantAllow;
            d.jsonTruncated |= kPlayerJsonTruncEntrant;
        }
        allow -= d.entrantJsonLen;
        const uint32_t unlocksAllow =
            uint32_t(allow < kPlayerJsonCap ? allow : uint64_t(kPlayerJsonCap));
        d.unlocksJsonLen = s.unlocksJson.n;
        if (d.unlocksJsonLen > unlocksAllow) {
            d.unlocksJsonLen = unlocksAllow;
            d.jsonTruncated |= kPlayerJsonTruncUnlocks;
        }
        jsonBudgetLeft -= uint64_t(d.entrantJsonLen) + d.unlocksJsonLen;

        Put<uint32_t>(out, static_cast<uint32_t>(sizeof(d)) + d.entrantJsonLen + d.unlocksJsonLen);
        Put(out, d);
        PutBytes(out, s.entrantJson.p, d.entrantJsonLen);
        PutBytes(out, s.unlocksJson.p, d.unlocksJsonLen);
    }

    Put<uint16_t>(out, m.roundCount);
    for (uint16_t i = 0; i < m.roundCount; ++i) {
        MarkRecordDisk d{};
        d.tUs = m.rounds[i].tUs;
        d.kind = m.rounds[i].kind;
        d.index = m.rounds[i].index;
        d.payload = m.rounds[i].payload;
        Put(out, d);
    }

    Put<uint16_t>(out, m.schemaCount);
    for (uint16_t i = 0; i < m.schemaCount; ++i) {
        const SchemaDef& s = m.schemas[i];
        SchemaFixedDisk d{};
        d.id = s.id;
        d.retention = static_cast<uint8_t>(s.retention);
        d.required = s.required ? 1u : 0u;
        d.fieldCount = s.fieldCount;
        Put<uint32_t>(out, static_cast<uint32_t>(sizeof(d)) + uint32_t(s.fieldCount) * sizeof(FieldDefDisk));
        Put(out, d);
        for (uint16_t f = 0; f < s.fieldCount; ++f) {
            FieldDefDisk fd{};
            fd.id = s.fields[f].id;
            fd.cls = static_cast<uint8_t>(s.fields[f].cls);
            fd.rawBits = s.fields[f].rawBits;
            fd.prov = static_cast<uint8_t>(s.fields[f].prov);
            fd.min = s.fields[f].min;
            fd.max = s.fields[f].max;
            fd.step = s.fields[f].step;
            Put(out, fd);
        }
    }

    const Blob blobs[kManifestBlobCount] = {m.balanceBlobZstd, m.slot0Baselines, m.slot1RuleVars,
                                            m.netIdSlotMap};
    Put<uint32_t>(out, kManifestBlobCount);
    for (uint32_t i = 0; i < kManifestBlobCount; ++i) {
        Put<uint32_t>(out, blobs[i].n);
        PutBytes(out, blobs[i].p, blobs[i].n);
    }

    // ARCH-21's round table, a tagged trailing block. It is written only when the writer has an answer
    // to record, so its ABSENCE is the honest report for every capture written before this existed --
    // and the reader turns that absence into MarkerSource::NotRecorded rather than into "no rounds".
    if (m.markerSource != MarkerSource::NotRecorded || m.roundRowCount != 0) {
        RoundTableHeadDisk head{};
        head.rowCount = m.roundRowCount;
        head.rowStride = static_cast<uint16_t>(sizeof(RoundRowDisk));
        head.markerSource = static_cast<uint8_t>(m.markerSource);
        Put<uint32_t>(out, kRoundTableTag);
        Put<uint32_t>(out, static_cast<uint32_t>(sizeof(head)) +
                               uint32_t(m.roundRowCount) * static_cast<uint32_t>(sizeof(RoundRowDisk)));
        Put(out, head);
        for (uint16_t i = 0; i < m.roundRowCount; ++i) {
            const RoundRow& s = m.roundRows[i];
            RoundRowDisk d{};
            d.number = s.number;
            d.flags = s.flags;
            d.phaseAtBegin = s.phaseAtBegin;
            d.phaseAtEnd = s.phaseAtEnd;
            d.bluePointsAtEnd = s.bluePointsAtEnd;
            d.redPointsAtEnd = s.redPointsAtEnd;
            d.blueMatchScoreAtEnd = s.blueMatchScoreAtEnd;
            d.redMatchScoreAtEnd = s.redMatchScoreAtEnd;
            d.beginUs = s.beginUs;
            d.endUs = s.endUs;
            d.beginEngineUs = s.beginEngineUs;
            d.endEngineUs = s.endEngineUs;
            Put(out, d);
        }
    }
}

bool DeserializeManifest(const uint8_t* p, uint32_t n, ManifestStore& store, const char** error) {
    const char* dummy = nullptr;
    if (!error) {
        error = &dummy;
    }
    *error = nullptr;

    Cursor c(p, n);
    store.players_.clear();
    store.rounds_.clear();
    store.roundRows_.clear();
    store.roundsRefusedBuf_[0] = '\0';
    store.schemas_.clear();
    store.fields_.clear();
    store.fieldOffsets_.clear();
    store.arena_.clear();
    store.blobOffsets_.clear();
    store.blobLengths_.clear();

    uint32_t fixedBytes = 0;
    ManifestFixedDisk fixed{};
    if (!c.Take(&fixedBytes, sizeof(fixedBytes)) || fixedBytes < sizeof(fixed)) {
        *error = "MANIFEST: fixed block is short";
        return false;
    }
    const uint32_t fixedEnd = c.At() + fixedBytes;
    if (!c.Take(&fixed, sizeof(fixed)) || !c.SeekTo(fixedEnd)) {
        *error = "MANIFEST: fixed block overruns the payload";
        return false;
    }

    Manifest& m = store.view_;
    m.levelId = fixed.levelId;
    m.gametypeId = fixed.gametypeId;
    m.startTimeEpoch = fixed.startTimeEpoch;
    m.keyframeIntervalUs = fixed.keyframeIntervalUs;
    m.partialStart = fixed.partialStart != 0;

    uint16_t playerCount = 0;
    if (!c.Take(&playerCount, sizeof(playerCount))) {
        *error = "MANIFEST: player count is missing";
        return false;
    }
    if (uint64_t(playerCount) * kMinPlayerRecordBytes > c.Left()) {
        *error = "MANIFEST: player count exceeds what the payload can hold";
        return false;
    }
    store.players_.resize(playerCount);
    for (uint16_t i = 0; i < playerCount; ++i) {
        uint32_t recBytes = 0;
        // Pre-seeded with the v1 ABSENCE values, so a v0 record -- which stops at kPlayerFixedV0Bytes --
        // yields "not recorded" for every v1 field rather than a plausible zero.
        PlayerFixedDisk d{};
        d.teamId16 = kPlayerNoTeam16;
        d.teamSlot16 = kPlayerNoTeam16;
        d.alignment16 = kPlayerNoAlignment16;
        d.firstSeenUs = kPlayerTimeNone;
        d.lastSeenUs = kPlayerTimeNone;

        if (!c.Take(&recBytes, sizeof(recBytes)) || recBytes < kPlayerFixedV0Bytes) {
            *error = "MANIFEST: player record is short";
            return false;
        }
        const uint32_t recEnd = c.At() + recBytes;
        if (recEnd > n || !c.Take(&d, kPlayerFixedV0Bytes)) {
            *error = "MANIFEST: player record overruns the payload";
            return false;
        }
        if (uint64_t(d.entrantJsonLen) + d.unlocksJsonLen > uint64_t(recBytes) - kPlayerFixedV0Bytes) {
            *error = "MANIFEST: player blobs overrun their record";
            return false;
        }
        // What is left between the v0 prefix and the blobs is the extension. A LONGER one comes from a
        // later minor and its tail is skipped; a shorter-than-v1 but non-empty one is a record no
        // version ever wrote, and reading it would mean guessing which fields arrived.
        const uint32_t extBytes =
            recBytes - kPlayerFixedV0Bytes - d.entrantJsonLen - d.unlocksJsonLen;
        if (extBytes != 0 && extBytes < kPlayerExtV1Bytes) {
            *error = "MANIFEST: player record's extension block is shorter than any version wrote";
            return false;
        }
        const uint32_t extTake = extBytes >= kPlayerExtV2Bytes ? kPlayerExtV2Bytes
                                                               : kPlayerExtV1Bytes;
        if (extBytes != 0 &&
            !c.Take(reinterpret_cast<uint8_t*>(&d) + kPlayerFixedV0Bytes, extTake)) {
            *error = "MANIFEST: player record overruns the payload";
            return false;
        }
        if (!c.SeekTo(recEnd - d.entrantJsonLen - d.unlocksJsonLen)) {
            *error = "MANIFEST: player record size disagrees with its content";
            return false;
        }
        PlayerRecord& r = store.players_[i];
        r.extended = extBytes != 0 ? 1u : 0u;
        r.xplatformId = d.xplatformId;
        memcpy(r.platformStr, d.platformStr, sizeof(r.platformStr));
        memcpy(r.displayName, d.displayName, sizeof(r.displayName));
        r.userSlot = d.userSlot;
        r.userIndex = d.userIndex;
        r.generation = d.generation;
        r.teamId = d.teamId;
        r.teamSlot = d.teamSlot;
        r.alignment = d.alignment;
        r.bodyType = d.bodyType;
        r.loadoutNumber = d.loadoutNumber;
        r.loadoutNumberBody = d.loadoutNumberBody;
        memcpy(r.loadoutSlots, d.loadoutSlots, sizeof(r.loadoutSlots));

        memcpy(r.displayNameFull, d.displayNameFull, sizeof(r.displayNameFull));
        memcpy(r.platformStrFull, d.platformStrFull, sizeof(r.platformStrFull));
        r.teamId16 = d.teamId16;
        r.teamSlot16 = d.teamSlot16;
        r.alignment16 = d.alignment16;
        r.firstSeenUs = d.firstSeenUs;
        r.lastSeenUs = d.lastSeenUs;
        r.fill = d.fill;
        r.jsonTruncated = d.jsonTruncated;
        r.extendedV2 = extBytes >= kPlayerExtV2Bytes ? 1u : 0u;
        r.fillExt = r.extendedV2 ? (d.fillExt & kPlayerFillExtAll) : 0u;
        if (!r.extended) {
            // A v0 row widened, not invented. B.0 wrote 0xFF into the 8-bit team fields for "absent"
            // (capture/roster.h names it), and that is the only absence it could express, so 0xFF is
            // the one value that becomes kPlayerNoTeam16 instead of team 255.
            memcpy(r.displayNameFull, r.displayName, sizeof(r.displayName));
            memcpy(r.platformStrFull, r.platformStr, sizeof(r.platformStr));
            r.teamId16 = r.teamId == 0xFF ? kPlayerNoTeam16 : r.teamId;
            r.teamSlot16 = r.teamSlot == 0xFF ? kPlayerNoTeam16 : r.teamSlot;
            r.alignment16 = r.alignment == 0xFF ? kPlayerNoAlignment16 : r.alignment;
        }

        store.blobOffsets_.push_back(static_cast<uint32_t>(store.arena_.size()));
        store.blobLengths_.push_back(d.entrantJsonLen);
        store.arena_.insert(store.arena_.end(), c.Ptr(), c.Ptr() + d.entrantJsonLen);
        if (!c.SeekTo(c.At() + d.entrantJsonLen)) {
            *error = "MANIFEST: entrant json overruns";
            return false;
        }
        store.blobOffsets_.push_back(static_cast<uint32_t>(store.arena_.size()));
        store.blobLengths_.push_back(d.unlocksJsonLen);
        store.arena_.insert(store.arena_.end(), c.Ptr(), c.Ptr() + d.unlocksJsonLen);
        if (!c.SeekTo(recEnd)) {
            *error = "MANIFEST: player record size disagrees with its content";
            return false;
        }
    }

    uint16_t roundCount = 0;
    if (!c.Take(&roundCount, sizeof(roundCount))) {
        *error = "MANIFEST: round count is missing";
        return false;
    }
    if (uint64_t(roundCount) * sizeof(MarkRecordDisk) > c.Left()) {
        *error = "MANIFEST: round count exceeds what the payload can hold";
        return false;
    }
    store.rounds_.resize(roundCount);
    for (uint16_t i = 0; i < roundCount; ++i) {
        MarkRecordDisk d{};
        if (!c.Take(&d, sizeof(d))) {
            *error = "MANIFEST: round table overruns the payload";
            return false;
        }
        store.rounds_[i] = RoundMarker{d.tUs, d.kind, d.index, d.payload};
    }

    uint16_t schemaCount = 0;
    if (!c.Take(&schemaCount, sizeof(schemaCount))) {
        *error = "MANIFEST: schema count is missing";
        return false;
    }
    if (uint64_t(schemaCount) * kMinSchemaRecordBytes > c.Left()) {
        *error = "MANIFEST: schema count exceeds what the payload can hold";
        return false;
    }
    store.schemas_.resize(schemaCount);
    store.fieldOffsets_.resize(schemaCount);
    for (uint16_t i = 0; i < schemaCount; ++i) {
        uint32_t recBytes = 0;
        SchemaFixedDisk d{};
        if (!c.Take(&recBytes, sizeof(recBytes)) || recBytes < sizeof(d)) {
            *error = "MANIFEST: schema record is short";
            return false;
        }
        const uint32_t recEnd = c.At() + recBytes;
        if (recEnd > n || !c.Take(&d, sizeof(d))) {
            *error = "MANIFEST: schema record overruns the payload";
            return false;
        }
        if (d.retention > static_cast<uint8_t>(Retention::Edge)) {
            *error = "MANIFEST: schema declares a retention outside {ALL, EDGE}";
            return false;
        }
        if (uint64_t(d.fieldCount) * sizeof(FieldDefDisk) > uint64_t(recEnd) - c.At()) {
            *error = "MANIFEST: schema field list overruns its record";
            return false;
        }
        store.schemas_[i].id = d.id;
        store.schemas_[i].retention = static_cast<Retention>(d.retention);
        store.schemas_[i].required = d.required != 0;
        store.schemas_[i].fieldCount = d.fieldCount;
        store.fieldOffsets_[i] = static_cast<uint32_t>(store.fields_.size());
        for (uint16_t f = 0; f < d.fieldCount; ++f) {
            FieldDefDisk fd{};
            if (!c.Take(&fd, sizeof(fd))) {
                *error = "MANIFEST: field list overruns the payload";
                return false;
            }
            if (fd.cls > kMaxFieldClass) {
                *error = "MANIFEST: field declares an unknown class";
                return false;
            }
            if (fd.prov > kMaxFieldProv) {
                *error = "MANIFEST: field declares an unknown provenance";
                return false;
            }
            FieldDef out{};
            out.id = fd.id;
            out.cls = static_cast<FieldClass>(fd.cls);
            out.rawBits = fd.rawBits;
            out.prov = static_cast<FieldProv>(fd.prov);
            out.min = fd.min;
            out.max = fd.max;
            out.step = fd.step;
            store.fields_.push_back(out);
        }
        if (!c.SeekTo(recEnd)) {
            *error = "MANIFEST: schema record size disagrees with its content";
            return false;
        }
    }

    uint32_t blobCount = 0;
    if (!c.Take(&blobCount, sizeof(blobCount))) {
        *error = "MANIFEST: blob count is missing";
        return false;
    }
    for (uint32_t i = 0; i < blobCount; ++i) {
        uint32_t len = 0;
        if (!c.Take(&len, sizeof(len)) || len > c.Left()) {
            *error = "MANIFEST: blob overruns the payload";
            return false;
        }
        if (i < kManifestBlobCount) {
            store.blobOffsets_.push_back(static_cast<uint32_t>(store.arena_.size()));
            store.blobLengths_.push_back(len);
            store.arena_.insert(store.arena_.end(), c.Ptr(), c.Ptr() + len);
        }
        c.SeekTo(c.At() + len);
    }
    // A minor-version writer may append blobs past the four this reader knows; those are skipped by
    // length. Fewer than four means the missing ones are empty, which is absence rather than a guess.
    for (uint32_t i = blobCount; i < kManifestBlobCount; ++i) {
        store.blobOffsets_.push_back(static_cast<uint32_t>(store.arena_.size()));
        store.blobLengths_.push_back(0);
    }

    // Trailing tagged blocks (ARCH-21). Reaching the end without the round table is not a failure and
    // not "zero rounds": it is MarkerSource::NotRecorded, the state every capture written before ARCH-21
    // is in. A block that is present but malformed is refused by name -- never skipped, never guessed.
    m.markerSource = MarkerSource::NotRecorded;
    bool sawRoundTable = false;
    while (c.Left() != 0) {
        uint32_t tag = 0, blockBytes = 0;
        if (!c.Take(&tag, sizeof(tag)) || !c.Take(&blockBytes, sizeof(blockBytes))) {
            *error = "MANIFEST: trailing block header is short";
            return false;
        }
        if (blockBytes > c.Left()) {
            *error = "MANIFEST: trailing block overruns the payload";
            return false;
        }
        const uint32_t blockEnd = c.At() + blockBytes;
        if (tag == kRoundTableTag) {
            // A malformed round table used to refuse the whole FILE, which spent the recording to
            // protect the feature. The block is parsed into locals first; anything wrong with it
            // names itself, the rows are dropped, and the manifest -- and the file -- still open.
            std::vector<RoundRow> parsed;
            uint8_t source = 0;
            const char* refusal = [&]() -> const char* {
                if (sawRoundTable) {
                    return "the round table appears twice";
                }
                RoundTableHeadDisk h{};
                if (blockBytes < sizeof(h) || !c.Take(&h, sizeof(h))) {
                    return "round table block is short";
                }
                if (h.rowStride < sizeof(RoundRowDisk)) {
                    return "round table row stride is smaller than one row";
                }
                if (uint64_t(h.rowCount) * h.rowStride != uint64_t(blockBytes) - sizeof(h)) {
                    return "round table size disagrees with its row count";
                }
                if (h.markerSource > kMaxMarkerSource) {
                    return "round table declares a marker source outside the vocabulary";
                }
                if (h.markerSource == static_cast<uint8_t>(MarkerSource::NotRecorded)) {
                    // The block's presence IS the claim that the writer chose. `none` is the value for
                    // "chose and found nothing"; NOT_RECORDED can only be said by leaving the block out.
                    return "round table declares no marker source";
                }
                source = h.markerSource;
                parsed.resize(h.rowCount);
                bool sawNumber = false;
                uint16_t prevNumber = 0;
                for (uint16_t i = 0; i < h.rowCount; ++i) {
                    RoundRowDisk d{};
                    if (!c.Take(&d, sizeof(d)) ||
                        !c.SeekTo(c.At() + (h.rowStride - static_cast<uint32_t>(sizeof(d))))) {
                        return "round table row overruns the payload";
                    }
                    if (d.flags & ~kAllRoundRowFlags) {
                        return "round row sets a flag bit the format does not define";
                    }
                    if (d.phaseAtBegin > kMaxGameStatePhase || d.phaseAtEnd > kMaxGameStatePhase) {
                        return "round row declares a phase outside the vocabulary";
                    }
                    if (d.number != kRoundNumberNone) {
                        // S4.5: numbered rows are unique and ascending, which is what makes SeekRound(n)
                        // a decidable question. The offending PAIR is named, because a table nobody can
                        // read back cannot be asked afterwards which of its rows was the wrong one.
                        if (sawNumber && d.number <= prevNumber) {
                            snprintf(store.roundsRefusedBuf_, sizeof(store.roundsRefusedBuf_),
                                     "round table numbers do not ascend: row %u is %u after %u",
                                     unsigned(i), unsigned(d.number), unsigned(prevNumber));
                            return store.roundsRefusedBuf_;
                        }
                        prevNumber = d.number;
                        sawNumber = true;
                    }
                    RoundRow& r = parsed[i];
                    r.number = d.number;
                    r.flags = d.flags;
                    r.phaseAtBegin = d.phaseAtBegin;
                    r.phaseAtEnd = d.phaseAtEnd;
                    r.bluePointsAtEnd = d.bluePointsAtEnd;
                    r.redPointsAtEnd = d.redPointsAtEnd;
                    r.blueMatchScoreAtEnd = d.blueMatchScoreAtEnd;
                    r.redMatchScoreAtEnd = d.redMatchScoreAtEnd;
                    r.reserved = 0;
                    r.beginUs = d.beginUs;
                    r.endUs = d.endUs;
                    r.beginEngineUs = d.beginEngineUs;
                    r.endEngineUs = d.endEngineUs;
                }
                return nullptr;
            }();
            sawRoundTable = true;
            if (refusal != nullptr) {
                // Half a table is worse than none: nothing downstream may read rows this reader has
                // already said it does not trust, so the block degrades whole and by name.
                if (refusal != store.roundsRefusedBuf_) {
                    snprintf(store.roundsRefusedBuf_, sizeof(store.roundsRefusedBuf_), "%s", refusal);
                }
                store.roundRows_.clear();
                m.markerSource = MarkerSource::NotRecorded;
            } else {
                store.roundRows_.swap(parsed);
                m.markerSource = static_cast<MarkerSource>(source);
            }
        }
        if (!c.SeekTo(blockEnd)) {
            *error = "MANIFEST: trailing block size disagrees with its content";
            return false;
        }
    }

    m.playerCount = playerCount;
    m.roundCount = roundCount;
    m.schemaCount = schemaCount;
    store.Rebind();
    return true;
}

void ManifestStore::Rebind() {
    static const uint8_t kEmpty = 0;
    const uint8_t* base = arena_.empty() ? &kEmpty : arena_.data();
    size_t blobAt = 0;
    auto next = [&]() -> Blob {
        if (blobAt >= blobOffsets_.size()) {
            return Blob{nullptr, 0};
        }
        const Blob b{base + blobOffsets_[blobAt], blobLengths_[blobAt]};
        ++blobAt;
        return b;
    };
    for (size_t i = 0; i < players_.size(); ++i) {
        players_[i].entrantJson = next();
        players_[i].unlocksJson = next();
    }
    Blob* dst[4] = {&view_.balanceBlobZstd, &view_.slot0Baselines, &view_.slot1RuleVars,
                    &view_.netIdSlotMap};
    for (size_t i = 0; i < 4; ++i) {
        *dst[i] = next();
    }
    for (size_t i = 0; i < schemas_.size(); ++i) {
        schemas_[i].fields = fields_.empty() ? nullptr : fields_.data() + fieldOffsets_[i];
    }
    view_.players = players_.empty() ? nullptr : players_.data();
    view_.rounds = rounds_.empty() ? nullptr : rounds_.data();
    view_.roundRows = roundRows_.empty() ? nullptr : roundRows_.data();
    view_.roundsRefused = roundsRefusedBuf_[0] != '\0' ? roundsRefusedBuf_ : nullptr;
    view_.schemas = schemas_.empty() ? nullptr : schemas_.data();
    view_.playerCount = static_cast<uint16_t>(players_.size());
    view_.roundCount = static_cast<uint16_t>(rounds_.size());
    view_.roundRowCount = static_cast<uint16_t>(roundRows_.size());
    view_.schemaCount = static_cast<uint16_t>(schemas_.size());
}

void ManifestStore::CopyFrom(const Manifest& src) {
    view_ = src;
    snprintf(roundsRefusedBuf_, sizeof(roundsRefusedBuf_), "%s",
             src.roundsRefused != nullptr ? src.roundsRefused : "");
    players_.assign(src.players, src.players + src.playerCount);
    rounds_.assign(src.rounds, src.rounds + src.roundCount);
    if (src.roundRows && src.roundRowCount) {
        roundRows_.assign(src.roundRows, src.roundRows + src.roundRowCount);
    } else {
        roundRows_.clear();
    }
    schemas_.assign(src.schemas, src.schemas + src.schemaCount);

    fields_.clear();
    fieldOffsets_.assign(src.schemaCount, 0);
    for (uint16_t i = 0; i < src.schemaCount; ++i) {
        fieldOffsets_[i] = static_cast<uint32_t>(fields_.size());
        fields_.insert(fields_.end(), src.schemas[i].fields, src.schemas[i].fields + src.schemas[i].fieldCount);
    }

    arena_.clear();
    blobOffsets_.clear();
    blobLengths_.clear();
    auto stash = [&](const Blob& b) {
        blobOffsets_.push_back(static_cast<uint32_t>(arena_.size()));
        blobLengths_.push_back(b.n);
        if (b.n && b.p) {
            arena_.insert(arena_.end(), b.p, b.p + b.n);
        }
    };
    for (uint16_t i = 0; i < src.playerCount; ++i) {
        stash(src.players[i].entrantJson);
        stash(src.players[i].unlocksJson);
    }
    stash(src.balanceBlobZstd);
    stash(src.slot0Baselines);
    stash(src.slot1RuleVars);
    stash(src.netIdSlotMap);
    Rebind();
}

void ManifestStore::FillFromHeader(const FileHeaderDisk& h) {
    Manifest& m = view_;
    m.formatMajor = h.formatMajor;
    m.formatMinor = h.formatMinor;
    m.t0Us = h.t0Us;
    memcpy(&m.demoUuidLo, h.demoUuid, 8);
    memcpy(&m.demoUuidHi, h.demoUuid + 8, 8);
    memcpy(m.echovrMd5, h.echovrMd5, sizeof(m.echovrMd5));
    m.buildStringHash = h.buildStringHash;
    m.contentHash = h.contentHash;
    m.captureRole = h.captureRole;
    m.authorityFlags = h.authorityFlags;
    m.flags = static_cast<uint32_t>(h.flags);
}

void ManifestStore::SetRounds(const RoundMarker* rounds, size_t count) {
    // Manifest::roundCount is uint16_t (Appendix B.0), so the mirrored table stops there. The S_MARK
    // stream still carries every marker; this is the browser's summary, not the record of them.
    if (count > 0xFFFFu) {
        count = 0xFFFFu;
    }
    rounds_.assign(rounds, rounds + count);
    Rebind();
}

bool ManifestStore::SetRoundTable(const RoundRow* rows, size_t count, MarkerSource source) {
    // Manifest::roundRowCount is uint16_t. Dropping rows past that would make SeekRound silently refuse
    // a round the file holds, so the table is refused whole and the caller says so.
    if (count > UINT16_MAX) {
        return false;
    }
    if (count && !rows) {
        return false;
    }
    roundRows_.assign(rows, rows + count);
    roundsRefusedBuf_[0] = '\0';
    view_.markerSource = source;
    Rebind();
    return true;
}

}  // namespace replay
