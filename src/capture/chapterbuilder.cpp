#include "capture/chapterbuilder.h"
#include "capture/playerstats.h"
#include "capture/roster.h"

#include "capture/manifestbuild.h"   // TapTable(), for the event vocabulary in every keyframe
#include "foundation/gametype.h"

#include <string.h>

#include <vector>

namespace replay {
namespace capture {
namespace {

// Per-stream soft cap and headroom. The cap is what closes a chapter early; the headroom is what
// guarantees the record that crossed it was written whole. One record is never larger than a verbatim
// wire body plus framing, so 8 KiB of headroom is generous by two orders of magnitude.
constexpr uint32_t kStreamSoftCapBytes = 96u * 1024u;
constexpr uint32_t kStreamHeadroomBytes = 8u * 1024u;
// S5.2 caps a whole chapter payload at 1 MiB, so a section closes before its streams sum past that,
// with room for the record that crossed the cap and for the accounting blocks written after it.
constexpr uint32_t kSectionSoftCapBytes = 700u * 1024u;

// This bound sizes the per-stream buffer and writer arrays, and a stream id past it indexes off the
// end of both. It is the FORMAT's highest id, not the highest one this file mentions: S_PLAYER_STATS
// (20) was added to demotypes.h and this bound stayed at S_GAMESTATE (19), and the only thing
// standing between that and an out-of-bounds write was the missing config row that made the stream
// unproducible (L2-RUN-7 F2). Two defects were holding each other up.
constexpr uint16_t kMaxStreamId = kFormatMaxStreamId;
static_assert(kMaxStreamId >= MaxProducibleStreamId(),
              "a producible stream would index past bufs[]/writers[]/frameSetOpen[]");
constexpr uint16_t kMaxNetvarShadow = 256;

SectionId SectionOf(StreamId s) {
    switch (s) {
        case kStreamAsSeen: return kSectionAsSeen;
        case kStreamVoip: return kSectionVoip;
        case kStreamOracle: return kSectionOracle;
        default: return kSectionCore;
    }
}

SchemaId SchemaOf(StreamId s) {
    switch (s) {
        case kStreamTick: return kSchemaTick;
        case kStreamPlayerWire: return kSchemaPlayerWire;
        case kStreamPlayerSampled: return kSchemaPlayerSampled;
        case kStreamBodyWire: return kSchemaBodyWire;
        case kStreamBodySampled: return kSchemaBodySampled;
        case kStreamNetvar: return kSchemaNetvar;
        case kStreamEvent: return kSchemaEvent;
        case kStreamKey: return kSchemaKey;
        case kStreamMark: return kSchemaMark;
        case kStreamAsSeen: return kSchemaAsSeen;
        case kStreamVoip: return kSchemaVoip;
        case kStreamOracle: return kSchemaOracle;
        case kStreamGap:
        case kStreamDropped: return kSchemaGap;
        case kStreamDegraded: return kSchemaDegraded;
        default: return kSchemaEvent;
    }
}

// Streams whose byte layout the CONTAINER already defines and parses -- S_MARK's round harvest, the
// GAP/DROPPED accounting, the DEGRADED ladder record, S_GAMESTATE's 84-byte sample. Those go in as
// plain packed struct arrays; only this packet's own streams are bit-packed. A bit-packed GAP block
// would make `echodemo verify` read the accounting as noise while reporting nothing wrong, which is
// the worst available outcome.
//
// L2-RUN-7 F1: S_GAMESTATE was absent from here while DispatchRecord appended it with AppendRaw, so
// the flush measured a bit cursor nothing had advanced and shipped a 4-byte empty block for every
// chapter of every capture. THE APPEND AND THE MEASURE ARE ONE DECISION: a stream added to one and
// not the other writes its records into a buffer and then discards them.
bool IsContainerFramed(StreamId s) {
    return s == kStreamMark || s == kStreamGap || s == kStreamDropped || s == kStreamDegraded ||
           s == kStreamGameState;
}

// The on-disk record size of a container-framed stream, taken from the struct rather than from any
// file. 0 for streams this does not apply to.
uint32_t ContainerRecordBytes(StreamId s) {
    switch (s) {
        case kStreamMark: return static_cast<uint32_t>(sizeof(MarkRecordDisk));
        case kStreamGap:
        case kStreamDropped: return static_cast<uint32_t>(sizeof(GapRecordDisk));
        case kStreamDegraded: return static_cast<uint32_t>(sizeof(DegradedRecordDisk));
        case kStreamGameState: return static_cast<uint32_t>(sizeof(GameStateRecordDisk));
        default: return 0;
    }
}

struct StreamBuf {
    std::vector<uint8_t> bytes;
    uint32_t used = 0;   // container-framed streams
    size_t bits = 0;     // bit-packed streams
    uint32_t records = 0;
};

// A compact alias for a wire entity, assigned from the key the message itself carries. It is only which
// delta predecessor a record is coded against -- never an identity anything downstream may rely on.
class SlotMap {
public:
    void Reset() {
        count_ = 0;
        memset(keys_, 0, sizeof keys_);
        memset(used_, 0, sizeof used_);
    }
    // `evicted` is set when the map was full and an existing slot was reused. That costs compression,
    // never correctness: the record still carries its own key.
    uint16_t Slot(uint64_t key, bool& evicted) {
        evicted = false;
        for (uint16_t i = 0; i < count_; ++i) {
            if (used_[i] && keys_[i] == key) return i;
        }
        if (count_ < kMaxEntities) {
            keys_[count_] = key;
            used_[count_] = true;
            return count_++;
        }
        evicted = true;
        const uint16_t victim = static_cast<uint16_t>(key % kMaxEntities);
        keys_[victim] = key;
        return victim;
    }

private:
    uint64_t keys_[kMaxEntities] = {};
    bool used_[kMaxEntities] = {};
    uint16_t count_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------------------------

struct ChapterBuilder::Impl {
    IDemoWriter* writer = nullptr;
    Manifest manifest = {};
    // The open chapter's scoreboard, handed to the writer between its BeginChapter and EndChapter.
    // It cannot be written at ingest time: this builder opens the container's chapter only at the
    // flush, and WritePlayerStats refuses a record with no chapter open.
    std::vector<PlayerStatsRecord> statsPending;

    StreamBuf bufs[kMaxStreamId + 1];
    bitio::BitWriter* writers[kMaxStreamId + 1] = {};

    PlayerEncoder playerWire{false};
    PlayerEncoder playerSampled{true};
    BodyEncoder bodyWire{false};
    BodyEncoder bodySampled{true};
    TickEncoder tickEnc;
    NetvarEncoder netvarEnc;
    EventEncoder eventEnc;
    AsSeenEncoder asSeenEnc;

    DuvSequence duvSeq;
    MarkerDeriver markers;
    // ARCH-21 D2: LOCAL is primary in every capture role, so this is the source that FILES and
    // `markers` above is the corroborator whose output is compared and counted.
    LocalMarkerSource local;
    MarkerSourceChoice choice;

    // The arm-up hold (ARCH-21 S4.4). While the choice is provisional a wire-derived mark is neither
    // filed nor thrown away: filing it under a NotRecorded source is the mixed-provenance fork D2
    // refuses, and discarding it loses a real edge from a file whose source turns out to be the wire.
    // It waits here until the choice can say which.
    static const uint32_t kMaxPendingWireMarks = 64;
    MarkRec pendingWire[kMaxPendingWireMarks] = {};
    uint32_t pendingWireCount = 0;
    events::LobbyTracker lobby;
    // The first whole gametype an SNSLobbyOwnerState body stated in this file; 0 until one does.
    uint64_t ownerStateGametype = 0;
    SlotMap playerSlots, bodySlots;

    // The shadow of the world, so a chapter can open with a complete keyframe even for entities and
    // netvars that produced nothing during it.
    struct PlayerShadow {
        bool present = false;
        uint64_t stampUs = 0;
        PlayerFrame frame = {};
    };
    struct BodyShadow {
        bool present = false;
        uint64_t stampUs = 0;
        BodyFrame frame = {};
    };
    PlayerShadow playerWireShadow[kMaxEntities];
    PlayerShadow playerSampledShadow[kMaxEntities];
    BodyShadow bodyWireShadow[kMaxBodies];
    BodyShadow bodySampledShadow[kMaxBodies];
    struct NetvarShadow {
        uint64_t name = 0;
        uint64_t value = 0;
        uint16_t slot = 0;
        bool used = false;
        // Set once a RECORDED delta row has supplied this value. The keyframe snapshot refuses to
        // touch such an entry: the delta stream carries that row, and a keyframe holding a value no
        // record in the file agrees with is the file disagreeing with itself (the same rule the
        // shadow-after-encode ordering below exists for). A snapshot-sourced entry contradicts
        // nothing, so it is re-read at every keyframe and stays the engine's current truth.
        bool fromDelta = false;
    };
    NetvarShadow netvarShadow[kMaxNetvarShadow];
    // The last (seeded, unreadable) pair a snapshot line reported, so a capture logs the keyframe
    // that first filled the block and every change after it, not one line per chapter.
    uint16_t netvarSnapLoggedSeeded = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    uint16_t netvarSnapLoggedUnreadable = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
    // The coordinate frame the open chapter is being recorded in. A chapter holds ONE, by
    // construction: kRecWorldOrigin splits the chapter when the value changes, so every record inside
    // a chapter shares the offset its keyframe states. That is what makes playback one constant add
    // per chapter instead of a per-record correction nobody could place in time.
    WorldOriginRec worldOrigin = {};
    bool haveWorldOrigin = false;

    TickRec lastTick = {};
    bool haveTick = false;

    bool chapterOpen = false;
    uint64_t chapterBeginUs = 0;
    uint64_t chapterEndUs = 0;
    uint64_t bytesToWriter = 0;
    bool netvarUnverifiedDeclared = false;
    bool frameSetOpen[kMaxStreamId + 1] = {};

    std::vector<uint8_t> keyBlob;
    std::vector<uint8_t> verbatimScratch;
    std::vector<uint8_t> pinScratch;   // sized once in Open: the keyframe path allocates nothing

    ~Impl() { DestroyWriters(); }

    void DestroyWriters() {
        for (uint16_t i = 0; i <= kMaxStreamId; ++i) {
            delete writers[i];
            writers[i] = nullptr;
        }
    }

    // A BitWriter is constructed over its buffer and carries the bit cursor, so it is rebuilt at each
    // chapter boundary rather than rewound: a refusal is sticky and there is deliberately no reset.
    void RearmWriters() {
        for (uint16_t i = 0; i <= kMaxStreamId; ++i) {
            if (IsContainerFramed(i) || bufs[i].bytes.empty()) {
                continue;
            }
            delete writers[i];
            writers[i] = new bitio::BitWriter(bufs[i].bytes.data(), bufs[i].bytes.size());
        }
    }

    void ResetEncoders() {
        playerWire.Reset();
        playerSampled.Reset();
        bodyWire.Reset();
        bodySampled.Reset();
        tickEnc.Reset();
        netvarEnc.Reset();
        eventEnc.Reset();
        asSeenEnc.Reset();
        RearmWriters();
    }

    // ★ A4. The slot maps are NOT reset here, and that is the fix rather than an omission.
    //
    // A wire row is keyed by a dense first-seen index over the frame's own header word; the shadow
    // that the next chapter's keyframe is written from is indexed by that same number. Resetting the
    // map at the end of every chapter while leaving the shadow alone made those two disagree: the map
    // re-derived slot numbers in the next chapter's ARRIVAL order, so slot k in the keyframe opening
    // chapter N+1 still meant chapter N's player while slot k in the chapter's rows meant whoever
    // happened to arrive first. Nothing in the file disagreed, and a wire track had no identity across
    // a 2 s boundary -- which is "respect player names" failing at its root.
    //
    // The map and the shadow it keys are two halves of one structure, so they get one lifetime: the
    // file's. A recovered chapter is still self-describing, because the keyframe carries the slot
    // number beside the frame whose header word IS the key.
    //
    // Identity is per FILE, so it is cleared when a file opens -- not when a chapter does.
    void ResetIdentity() {
        playerSlots.Reset();
        bodySlots.Reset();
        memset(playerWireShadow, 0, sizeof playerWireShadow);
        memset(playerSampledShadow, 0, sizeof playerSampledShadow);
        memset(bodyWireShadow, 0, sizeof bodyWireShadow);
        memset(bodySampledShadow, 0, sizeof bodySampledShadow);
        memset(netvarShadow, 0, sizeof netvarShadow);
        netvarSnapLoggedSeeded = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        netvarSnapLoggedUnreadable = 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE this code's own all-ones u16 "none"; facts.tsv keeps the three engine fields that spend 0xFFFF as separate ids
        lastTick = TickRec{};
        haveTick = false;
        worldOrigin = WorldOriginRec{};
        haveWorldOrigin = false;
    }

    // ONE FUNCTION OWNS EVERY PER-FILE STATE, because a state cleared in two places drifts and this
    // one did: identity was cleared here, `local` and `choice` in Open(), and `markers` -- the wire
    // deriver -- in neither. Its watched values, its seen flags and its round index survived into the
    // next match of the same session, so match 2's first netvar record read as an EDGE instead of a
    // first sighting: a LAUNCH_RELEASE and a ROUND_BEGIN at an instant where no round began, and the
    // ROUND_BEGIN cut a chapter, which put a seek target in the wrong place in the timeline.
    // Anything that must not outlive a file is cleared HERE, and Open() clears nothing itself.
    void ResetPerFile(bool localCandidate, uint64_t t0Us) {
        ResetIdentity();
        markers.Reset();
        local.Reset();
        choice.Reset();
        choice.SetLocalCandidate(localCandidate);
        choice.NoteFileOpen(t0Us);
        pendingWireCount = 0;
        ownerStateGametype = 0;
        netvarUnverifiedDeclared = false;
        // A new file inherits no open chapter. These three were the only per-FILE
        // state left out of the per-file reset, so a second Open in one process began with the
        // previous file's chapter still nominally open and its begin time as this file's.
        chapterOpen = false;
        chapterBeginUs = 0;
        chapterEndUs = 0;
    }

    NetvarShadow* FindNetvar(uint64_t name, uint16_t slot, bool create) {
        for (uint16_t i = 0; i < kMaxNetvarShadow; ++i) {
            if (netvarShadow[i].used && netvarShadow[i].name == name &&
                netvarShadow[i].slot == slot) {
                return &netvarShadow[i];
            }
        }
        if (!create) {
            return nullptr;
        }
        for (uint16_t i = 0; i < kMaxNetvarShadow; ++i) {
            if (!netvarShadow[i].used) {
                netvarShadow[i].used = true;
                netvarShadow[i].name = name;
                netvarShadow[i].slot = slot;
                return &netvarShadow[i];
            }
        }
        return nullptr;
    }
};

ChapterBuilder::ChapterBuilder() : impl_(new Impl) {}

ChapterBuilder::~ChapterBuilder() {
    delete impl_;
}

const DuvSequence& ChapterBuilder::NetvarSequence() const { return impl_->duvSeq; }

const MarkerDeriver::Stats& ChapterBuilder::MarkerStats() const { return impl_->markers.GetStats(); }
const LocalMarkerSource::Stats& ChapterBuilder::LocalMarkerStats() const {
    return impl_->local.GetStats();
}
MarkerSource ChapterBuilder::ChosenMarkerSource() const { return impl_->choice.Source(); }
bool ChapterBuilder::CorroboratorAsked() const { return impl_->choice.CorroboratorAsked(); }
uint32_t ChapterBuilder::WireRowsSeen() const { return impl_->choice.WireRowsSeen(); }
const RoundRow* ChapterBuilder::RoundRows() const { return impl_->local.Rows(); }
uint16_t ChapterBuilder::RoundRowCount() const { return impl_->local.RowCount(); }

const events::LobbyTracker& ChapterBuilder::Lobby() const { return impl_->lobby; }
uint64_t ChapterBuilder::OwnerStateGametype() const { return impl_->ownerStateGametype; }
const events::LobbyTracker::Stats& ChapterBuilder::LobbyStats() const {
    return impl_->lobby.GetStats();
}


// Stream ids that were allocated, used, and withdrawn. DESIGN S5.4.0's INV-STREAM-ID: a retired id is
// never reassigned, because MANIFEST's schema table, every WriteStreamBlock call, INDEX's per-chapter
// section word and every already-recorded .echodemo are all indexed by that number.
bool StreamIsRetired(uint16_t id) {
    // The container still spells id 10 kStreamWorld and that is W1-B's call to make -- INV-STREAM-ID
    // requires that the NUMBER is never reassigned, not that the name changes. This is the one place
    // in this packet that still names it, and it names it in order to refuse it.
    return id == kStreamWorld;
}

bool ChapterBuilder::Open(IDemoWriter* w, const Manifest& m, const BuilderConfig& cfg) {
    if (!w || SchemaTableRefusal()) {
        return false;
    }
    cfg_ = cfg;
    stats_ = BuilderStats{};

    // ★ ENFORCED HERE RATHER THAN TRUSTED OF EVERY CALLER. A caller that enables a retired stream --
    // a config that still names it, or a test that turns the whole array on -- must not be able to
    // put blocks in a file that no reader has a meaning for. Clearing it centrally means the
    // invariant is held by the code that owns the format, not by everyone who calls into it.
    //
    // AFTER the stats reset, not before: the first version counted the refusal and then had the count
    // wiped by `stats_ = BuilderStats{}` on the next line. The refusal itself still worked, so the
    // only symptom was a counter that could never report -- which is why mkdemo asserts the count and
    // not merely the absence of the stream.
    for (uint16_t id = 0; id < 32; ++id) {
        if (StreamIsRetired(id) && cfg_.streamEnabled[id]) {
            cfg_.streamEnabled[id] = false;
            ++stats_.retiredStreamsRefused;
        }
    }
    impl_->writer = w;
    impl_->manifest = m;
    for (uint16_t s = 0; s <= kMaxStreamId; ++s) {
        // S_PLAYER_STATS is the one producible stream that never lands in a StreamBuf -- it goes
        // through the writer's own record call -- so it gets an index and no megabyte.
        if (s == kStreamPlayerStats) continue;
        impl_->bufs[s].bytes.assign(kStreamSoftCapBytes + kStreamHeadroomBytes, 0);
    }
    impl_->keyBlob.assign(256u * 1024u, 0);
    impl_->verbatimScratch.assign(4096, 0);
    impl_->pinScratch.assign(audio::MaxPinBytes(), 0);
    impl_->RearmWriters();

    // A second match in one session used to open over the first one's identity: nothing cleared the
    // slot maps or the shadows, so match 2's first keyframe published match 1's players. Always-on
    // capture writes one file per match, so this is the ordinary path, not a corner.
    impl_->ResetPerFile(cfg_.streamEnabled[kStreamGameState], m.t0Us);

    // L3-RUN-6 F1. The file's axis begins where the file begins. A record whose stamp is NOT on the
    // capture axis rides `lastSourceUs_`, and that started at 0, so a foreign-stamped record arriving
    // before the first tick opened a chapter at 0 -- 18.9 s before t0 in L2 #7's capture, which is
    // exactly the 68.333 s the manifest reported against the transport's 49.433 s (68.333 - t0).
    // Nothing can be placed before the moment the file was opened.
    lastSourceUs_ = m.t0Us;

    // Every stream declares where its `observed` count comes from. A stream whose source is None is
    // declared unverified in the file the first time it stores anything, because silence and agreement
    // look identical to a reader and only one of them is a claim we can make.
    // Every source is checked against the stream's WRITE POLICY at compile time. The one this
    // catches is real and shipped: S_GAMESTATE was configured with the engine frame index while
    // being written on change plus a periodic copy, so its observed count was a number no engine
    // frame produced.
#define CONFIGURE_OBSERVED(id, src)                                                      \
    static_assert(ObservedSourceFitsCadence(id, src),                                    \
                  #id " is configured with a source its write policy cannot supply");    \
    accounting_.Configure(id, src)

    CONFIGURE_OBSERVED(kStreamTick, ObservedSource::EngineFrameIndex);
    CONFIGURE_OBSERVED(kStreamPlayerSampled, ObservedSource::EngineFrameIndex);
    CONFIGURE_OBSERVED(kStreamBodySampled, ObservedSource::EngineFrameIndex);
    CONFIGURE_OBSERVED(kStreamAsSeen, ObservedSource::EngineFrameIndex);
    CONFIGURE_OBSERVED(kStreamKey, ObservedSource::EngineFrameIndex);
    // S_ORACLE emits once per CORE chapter, so the sampler's decision to emit IS its numbering:
    // nothing else numbers these records and no engine frame counter can.
    CONFIGURE_OBSERVED(kStreamOracle, ObservedSource::SamplerDecision);
    CONFIGURE_OBSERVED(kStreamPlayerWire, ObservedSource::TapDelivery);
    CONFIGURE_OBSERVED(kStreamBodyWire, ObservedSource::TapDelivery);
    CONFIGURE_OBSERVED(kStreamEvent, ObservedSource::TapDelivery);
    CONFIGURE_OBSERVED(kStreamVoip, ObservedSource::TapDelivery);
    CONFIGURE_OBSERVED(kStreamMark, ObservedSource::TapDelivery);
    // S_GAMESTATE is written when the state CHANGES, plus a periodic copy. There is no engine frame
    // per record, so what the law can measure on it is the difference between the samples the
    // sampler decided to emit and the samples that reached the file -- and nothing else numbers
    // them. S_PLAYER_STATS is deliberately absent from this table: W1-B ruled it out of the law
    // because the engine publishes no numbering of stat CHANGES at all, so it declares no source
    // rather than being handed one that cannot be checked.
    CONFIGURE_OBSERVED(kStreamGameState, ObservedSource::SamplerDecision);
    // ★ S5.6.2 F-6: on offline-host and dedicated the netvars are ORIGINATED locally and never cross
    // the wire, so there is no packet number to count. The design states the completeness of this
    // stream is unverified in those roles and claims nothing; so does this.
    accounting_.Configure(kStreamNetvar, cfg.captureRole == 0 ? ObservedSource::WireSequence
                                                              : ObservedSource::None);
    static_assert(ObservedSourceFitsCadence(kStreamNetvar, ObservedSource::WireSequence) &&
                      ObservedSourceFitsCadence(kStreamNetvar, ObservedSource::None),
                  "S_NETVAR's two roles must both suit its write policy");
#undef CONFIGURE_OBSERVED
    return true;
}

namespace {

bool AppendRaw(StreamBuf& b, const void* p, uint32_t n) {
    if (b.used + n > b.bytes.size()) {
        return false;
    }
    memcpy(b.bytes.data() + b.used, p, n);
    b.used += n;
    ++b.records;
    return true;
}

}  // namespace

namespace {
void AppendMark(StreamBuf& b, const MarkRec& m) {
    MarkRecordDisk d{};
    d.tUs = m.tUs;
    d.kind = m.kind;
    d.index = m.index;
    d.payload = m.payload;
    AppendRaw(b, &d, sizeof d);
}
}  // namespace

// A2. EVERY append must happen inside an open chapter, because BeginChapter ZEROES the per-chapter
// counters. EmitMatchBegin used to run before the first Consume opened one: the record reached the
// buffer and its accounting was then wiped, so the first S_MARK block of every capture declared one
// fewer record than it contained.
//
// AND THE REASON THAT SURVIVED EVERY CHECK IS WORSE THAN THE OFF-BY-ONE. echodemo verify passed with
// zero fails because both sides of observed == stored + gaps came from the SAME counter, and a
// counter that is wrong is still self-consistent. The one independent test -- the record-width
// cross-check -- LEARNS ITS WIDTH FROM THE FIRST BLOCK: handed a corrupted first block it learned a
// width that did not divide, marked the stream variable-width, and switched itself off. A check that
// derives its expectation from the artifact it is checking cannot fail on that artifact.
//
// The ordering is fixed here; the width is asserted from sizeof() in FlushChapter, which is a source
// no block can influence.
void ChapterBuilder::EnsureChapterOpen(uint64_t tUs) {
    Impl& s = *impl_;
    if (s.chapterOpen) {
        return;
    }
    s.chapterOpen = true;
    s.chapterBeginUs = tUs ? tUs : lastSourceUs_;
    s.chapterEndUs = s.chapterBeginUs;
    accounting_.BeginChapter(s.chapterBeginUs);
    s.duvSeq.BeginChapter();
    WriteKeyframe(s.chapterBeginUs);
}

void ChapterBuilder::EmitMatchBegin(uint64_t tUs) {
    if (!cfg_.streamEnabled[kStreamMark]) {
        return;
    }
    // The boundary begins the chapter it marks -- see IsRoundBoundary above. These two emitters do
    // not travel through Consume, so they cut here.
    BeginBoundaryChapter(tUs);
    AppendMark(impl_->bufs[kStreamMark], impl_->markers.MatchBegin(tUs));
    accounting_.NoteObserved(kStreamMark, 1);
    accounting_.NoteStored(kStreamMark, 1);
}

void ChapterBuilder::EmitMatchEnd(uint64_t tUs) {
    if (!cfg_.streamEnabled[kStreamMark]) {
        return;
    }
    BeginBoundaryChapter(tUs);
    AppendMark(impl_->bufs[kStreamMark], impl_->markers.MatchEnd(tUs));
    accounting_.NoteObserved(kStreamMark, 1);
    accounting_.NoteStored(kStreamMark, 1);
}

// ---------------------------------------------------------------------------------------------
// The keyframe. A plain packed snapshot, deliberately NOT bit-packed: it is the seek target, and a
// one-bit desync in a keyframe costs the whole chapter it opens. The delta streams pay for density;
// this pays for robustness, and zstd recovers most of the difference either way.
// ---------------------------------------------------------------------------------------------

// Whether the lobby-derived roster already carries this person, so the local scan's copy is dropped
// instead of doubling the row.
//
// The key is the ACCOUNT ID and nothing else. A slot comparison would be cheaper and wrong: the
// lobby's entrantIndex and the identity tables' user slot are two index spaces, and a match between
// them is a coincidence, not a person. A zero on either side is "not identified" and never matches --
// an extra row is a recoverable reading, a merged row is a fabricated one.
static bool LobbyCarriesId(const events::LobbyTracker& lobby, uint16_t count, uint64_t xplatformId) {
    if (xplatformId == 0) {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        const events::EntrantRow& e = lobby.EntrantAt(i);
        if (e.accountId == xplatformId || e.platformCode == xplatformId) {
            return true;
        }
    }
    return false;
}

// SPEC-RECORDER-2 S2.3. Read-only on the engine, and the only merge rule is stated in NetvarShadow:
// a key a recorded delta has already spoken for keeps the delta's value. That leaves the snapshot
// doing exactly the job the delta-only tap cannot -- giving a never-changed key a value at all, and
// keeping a snapshot-only key current -- without ever contradicting a row that is in the file.
void ChapterBuilder::SnapshotNetvars() {
    if (netvarSnapshot_ == nullptr) {
        return;
    }
    Impl& s = *impl_;
    NetvarSnapshotRow rows[32];
    uint16_t unreadable = 0;
    const uint16_t n = netvarSnapshot_->Snapshot(rows, uint16_t(sizeof rows / sizeof rows[0]),
                                                 unreadable);
    uint16_t seeded = 0;
    for (uint16_t i = 0; i < n; ++i) {
        Impl::NetvarShadow* sh = s.FindNetvar(rows[i].name, rows[i].slot, true);
        if (sh == nullptr || sh->fromDelta) {
            continue;
        }
        sh->value = rows[i].value;
        ++seeded;
    }
    ++stats_.netvarSnapshots;
    stats_.netvarSnapshotSeeded += seeded;
    stats_.netvarSnapshotUnreadable += unreadable;
    if (log_ && (seeded != s.netvarSnapLoggedSeeded ||
                 unreadable != s.netvarSnapLoggedUnreadable)) {
        s.netvarSnapLoggedSeeded = seeded;
        s.netvarSnapLoggedUnreadable = unreadable;
        log_->Write(LogLevel::Info, "capture",
                    "netvar snapshot: %u keys at keyframe %llu (%u unreadable)",
                    (unsigned)seeded, (unsigned long long)stats_.netvarSnapshots,
                    (unsigned)unreadable);
    }
}

void ChapterBuilder::WriteKeyframe(uint64_t tUs) {
    Impl& s = *impl_;
    if (s.keyBlob.empty()) {
        return;
    }
    SnapshotNetvars();
    KeyframeBlobWriter k(s.keyBlob.data(), s.keyBlob.size(), tUs);

    // ★ ONE BLOCK PER CONCEPT, each self-describing by length. Adding the determinism pins later is an
    // extra Block() here and nothing else -- no field moves, no reader breaks, and every file recorded
    // before they arrive stays readable by the reader that has them. That is the whole reason this
    // framing landed before the fields it exists for.
    k.Block(kKeyBlockTick);
    k.Put(s.haveTick ? uint8_t(1) : uint8_t(0));
    k.Put(s.lastTick);

    auto putPlayers = [&](uint16_t blockId, const Impl::PlayerShadow* sh, uint16_t n) {
        k.Block(blockId);
        uint16_t count = 0;
        for (uint16_t i = 0; i < n; ++i) {
            if (sh[i].present) ++count;
        }
        k.Put(count);
        for (uint16_t i = 0; i < n; ++i) {
            if (!sh[i].present) continue;
            k.Put(i);
            k.Put(sh[i].stampUs);
            k.Put(sh[i].frame);
        }
    };
    auto putBodies = [&](uint16_t blockId, const Impl::BodyShadow* sh, uint16_t n) {
        k.Block(blockId);
        uint16_t count = 0;
        for (uint16_t i = 0; i < n; ++i) {
            if (sh[i].present) ++count;
        }
        k.Put(count);
        for (uint16_t i = 0; i < n; ++i) {
            if (!sh[i].present) continue;
            k.Put(i);
            k.Put(sh[i].stampUs);
            k.Put(sh[i].frame);
        }
    };

    putPlayers(kKeyBlockPlayerWire, s.playerWireShadow, kMaxEntities);
    putPlayers(kKeyBlockPlayerSampled, s.playerSampledShadow, kMaxEntities);
    putBodies(kKeyBlockBodyWire, s.bodyWireShadow, kMaxBodies);
    putBodies(kKeyBlockBodySampled, s.bodySampledShadow, kMaxBodies);

    k.Block(kKeyBlockNetvar);
    uint16_t netvarCount = 0;
    for (uint16_t i = 0; i < kMaxNetvarShadow; ++i) {
        if (s.netvarShadow[i].used) ++netvarCount;
    }
    k.Put(netvarCount);
    for (uint16_t i = 0; i < kMaxNetvarShadow; ++i) {
        if (!s.netvarShadow[i].used) continue;
        k.Put(s.netvarShadow[i].slot);
        k.Put(s.netvarShadow[i].name);
        k.Put(s.netvarShadow[i].value);   // the raw SValue, never re-encoded (S5.4.5 rule 2)
    }

    // ★ The event vocabulary. Without it a typeIndex in an S_EVENT row is an index into a table that
    // exists only in the writer's binary, and reordering that array silently repoints every event in
    // every file already recorded.
    k.Block(kKeyBlockTapTable);
    const uint16_t tapCount = TapTypeCount();
    k.Put(tapCount);
    for (uint16_t i = 0; i < tapCount; ++i) {
        const TapType& t = TapTable()[i];
        const size_t len = strlen(t.structName);
        k.Put(i);
        k.Put(t.stream);
        k.Put(static_cast<uint16_t>(len));
        k.Raw(t.structName, len);
    }

    // ★ How much of that table was actually LIVE when this keyframe was written. The
    // block above is the closed list of types the capture could have recorded; this is the list that
    // was registered, so an empty stream can be told from an unregistered one. Read through the
    // published snapshot, which is what makes it legal from this thread (manifestbuild.h).
    const TapRegistration reg = TapRegistrationNow();
    TapRegistrationBlock rb{};
    rb.registered = static_cast<uint16_t>(reg.registered);
    rb.deferred = static_cast<uint16_t>(reg.deferred);
    rb.givenUp = static_cast<uint16_t>(reg.givenUp);
    rb.unavailable = static_cast<uint16_t>(reg.unavailable);
    rb.excluded = static_cast<uint16_t>(reg.excluded);
    rb.total = static_cast<uint16_t>(reg.total);
    rb.tableRows = static_cast<uint16_t>(reg.tableRows);
    rb.started = reg.started ? 1u : 0u;
    PutTapRegistrationBlock(k, rb);

    // ★ The roster, at the engine's widths. See kKeyBlockRoster for why it is not MANIFEST's player
    // rows. Written even when empty, because a zero count is the statement "no entrant was decoded by
    // this point" and an absent block is the statement "this writer did not carry rosters" -- a reader
    // that cannot tell those apart cannot tell a spectator-only capture from an old file.
    k.Block(kKeyBlockRoster);
    // THE UNION of the two sources, lobby first so an existing reader sees what it always saw. A
    // locally sourced row whose user slot is already carried by a lobby entrant is DROPPED rather
    // than appended: two rows for one person is a fabricated roster, and the lobby row is the one
    // built from the wire the reader can recompute from the file.
    const uint16_t entrantCount = s.lobby.EntrantCount();
    uint16_t localExtra = 0;
    if (roster_) {
        for (uint16_t i = 0; i < roster_->Count(); ++i) {
            if (!LobbyCarriesId(s.lobby, entrantCount, roster_->At(i).xplatformId)) {
                ++localExtra;
            }
        }
    }
    k.Put(static_cast<uint16_t>(entrantCount + localExtra));
    for (uint16_t i = 0; i < entrantCount; ++i) {
        const events::EntrantRow& e = s.lobby.EntrantAt(i);
        k.Put(e.platformCode);
        k.Put(e.accountId);
        k.Put(e.sessionId);
        k.Put(e.firstSeenUs);
        k.Put(e.lastSeenUs);
        k.Put(e.entrantIndex);
        k.Put(e.generation);
        k.Put(e.team);          // u16: 0xFFFF is NO TEAM and stays distinguishable from team 255
        k.Put(e.marker);
        k.Put(static_cast<uint8_t>(e.active ? 1 : 0));
        k.Put(static_cast<uint8_t>(e.hasProfileJson ? 1 : 0));
        const uint16_t nameLen = static_cast<uint16_t>(strnlen(e.displayName, sizeof e.displayName));
        k.Put(nameLen);
        k.Raw(e.displayName, nameLen);
        const uint16_t platLen = static_cast<uint16_t>(strnlen(e.platformStr, sizeof e.platformStr));
        k.Put(platLen);
        k.Raw(e.platformStr, platLen);
    }
    if (roster_) {
        for (uint16_t i = 0; i < roster_->Count(); ++i) {
            const RosterRow& r = roster_->At(i);
            if (LobbyCarriesId(s.lobby, entrantCount, r.xplatformId)) {
                continue;
            }
            k.Put(r.xplatformId);
            k.Put(uint64_t(0));         // accountId: no reader on this build sources it
            k.Put(uint64_t(0));         // sessionId: likewise
            k.Put(r.firstSeenUs);
            k.Put(r.lastSeenUs);
            k.Put(r.userIndex);
            k.Put(r.generation);
            k.Put(r.team);              // u16: 0xFFFF is NO TEAM, never narrowed to 255
            k.Put(uint16_t(0));         // marker: a locally sourced row is not derived from a mark
            k.Put(static_cast<uint8_t>(r.active ? 1 : 0));
            k.Put(static_cast<uint8_t>(r.hasProfileJson ? 1 : 0));
            const uint16_t nameLen = static_cast<uint16_t>(strnlen(r.displayName, sizeof r.displayName));
            k.Put(nameLen);
            k.Raw(r.displayName, nameLen);
            const uint16_t pLen = static_cast<uint16_t>(strnlen(r.platformStr, sizeof r.platformStr));
            k.Put(pLen);
            k.Raw(r.platformStr, pLen);
        }
    }

    // ★ The coordinate frame this chapter was recorded in, absolute, never accumulated by the reader
    // -- the engine already accumulated, and the triple is never reset for the life of the process, so
    // a capture that starts mid-session correctly sees a non-zero value on its first frame.
    //
    // `valid` is written even when it is 0. An absent block and a zero vector are different claims and
    // a reader must not have to guess which one it is holding: 0 means "this build could not source
    // the offset", which is not the same as "the world is at the origin".
    k.Block(kKeyBlockWorldOrigin);
    k.Put(s.worldOrigin.x);
    k.Put(s.worldOrigin.y);
    k.Put(s.worldOrigin.z);
    k.Put(static_cast<uint8_t>(s.haveWorldOrigin ? s.worldOrigin.valid : 0u));
    k.Put(static_cast<uint8_t>(s.haveWorldOrigin ? s.worldOrigin.suspect : 0u));

    // ★ The audio determinism pins (S7.4, ruling Q4): the states, per-emitter switches, sustained
    // loops and Wwise random state a backwards seek has to restore, because every one of them was
    // set by an EDGE in the past that seeking to this moment never re-issues.
    //
    // Written from the snapshot the TICK thread published, exactly like the tap registration above:
    // the pin walk reads engine memory and this is the worker. An absent snapshot writes no block --
    // a keyframe carrying an empty pin block would tell a reader the file says nothing is sustained
    // here, which is a claim, not a silence.
    //
    // The other three reserved ids stay unwritten; they are blocked on packets that have not
    // reported, and their absence from a file recorded today is the honest state.
#if !defined(W2A_MUTATE_NO_PIN_BLOCK)
    const uint32_t pinBytes =
        AudioPinsNow(s.pinScratch.data(), static_cast<uint32_t>(s.pinScratch.size()));
    if (pinBytes != 0) {
        k.Block(kKeyBlockAudioPins);
        k.Raw(s.pinScratch.data(), pinBytes);
        ++stats_.audioPinBlocks;
    } else {
        ++stats_.audioPinsAbsent;
    }
#else
    // Mutation control: the block is not written, so a keyframe carries no pins and every seek is
    // left with whatever the last one armed. If the round-trip case stays green with this defined,
    // it is not reading the file.
    ++stats_.audioPinsAbsent;
#endif

    const size_t bytes = k.Finish();
    if (bytes == 0) {
        // A keyframe that did not fit is a seek target we cannot honour. Recorded as a degradation
        // rather than written short: a truncated keyframe decodes into plausible nonsense, and a
        // missing one is at least visible.
        NoteDegraded(tUs, kStreamKey, kDegradeKeyframeInterval, static_cast<uint32_t>(k.Used()));
        return;
    }

    bitio::BitWriter* w = s.writers[kStreamKey];
    if (!w || !EncodeRawRecord(*w, s.keyBlob.data(), static_cast<uint16_t>(bytes))) {
        ++stats_.encodeRefusals;
        return;
    }
    s.bufs[kStreamKey].bits = w->BitsWritten();
    ++s.bufs[kStreamKey].records;
    accounting_.NoteObserved(kStreamKey, 1);
    accounting_.NoteStored(kStreamKey, 1);
    ++stats_.keyframes;
}

// ---------------------------------------------------------------------------------------------
// Chapter lifecycle
// ---------------------------------------------------------------------------------------------

void ChapterBuilder::FlushChapter() {
    Impl& s = *impl_;
    if (!s.chapterOpen) {
        return;
    }
    s.chapterOpen = false;

    // Any run the per-record array could not hold becomes one summary record for its own stream,
    // before anything is read. Doing it after would silently drop the overflow.
    // Close any frame set still open, so its present-and-unsampled entities become DROPPED records in
    // the chapter that lost them rather than in the next one.
    for (uint16_t id = 0; id <= kMaxStreamId; ++id) {
        if (s.frameSetOpen[id]) {
            accounting_.EndFrameSet(id, s.chapterEndUs);
            s.frameSetOpen[id] = false;
        }
    }
    accounting_.SealChapter(s.chapterBeginUs);
    uint32_t gapCount = 0, dropCount = 0;
    const GapRecordDisk* gaps = accounting_.GapRecords(gapCount);
    const GapRecordDisk* drops = accounting_.DroppedRecords(dropCount);
    const uint64_t tEnd = s.chapterEndUs > s.chapterBeginUs ? s.chapterEndUs : s.chapterBeginUs;
    static thread_local GapRecordDisk sectionGaps[kMaxGapRecords];
    static thread_local GapRecordDisk sectionDrops[kMaxGapRecords];

    // One chapter per section, written in section order: the container holds one chapter open at a
    // time, so the sections are serialised here rather than interleaved.
    for (uint16_t section = 0; section < kSectionCount; ++section) {
        if (section == kSectionManifest || section == kSectionIndex) {
            continue;
        }
        bool any = false;
        for (uint16_t id = 1; id <= kMaxStreamId; ++id) {
            if (SectionOf(id) == section && s.bufs[id].records > 0) {
                any = true;
                break;
            }
        }

        // ★ A gap belongs in the chapter of the stream it names, not in one chapter for the whole
        // file. The container auto-emits a GAP for any deficit the caller did not name IN THAT
        // CHAPTER, so putting every gap in CORE left S_ASSEEN's deficit unnamed in the ASSEEN chapter
        // and the writer recorded it a second time -- the file then reported more lost records than
        // the engine had numbered, and `echodemo verify` printed an unsigned underflow. The double
        // count was visible only because the arithmetic is checked; a looser check would have shown a
        // plausible number.
        uint32_t gapHere = 0, dropHere = 0;
        for (uint32_t i = 0; i < gapCount; ++i) {
            if (SectionOf(gaps[i].streamId) == section) sectionGaps[gapHere++] = gaps[i];
        }
        for (uint32_t i = 0; i < dropCount; ++i) {
            if (SectionOf(drops[i].streamId) == section) sectionDrops[dropHere++] = drops[i];
        }
        // S_PLAYER_STATS never lands in s.bufs -- it goes through the writer's own buffer, which
        // is where the format's rules live -- so `any` above cannot see it, and a chapter holding
        // nothing but a scoreboard would be skipped with the records still pending.
        const bool statsHere =
            !s.statsPending.empty() && SectionOf(kStreamPlayerStats) == section;
        const bool accountingHere = gapHere || dropHere;
        if (!any && !accountingHere && !statsHere) {
            continue;
        }

        s.writer->BeginChapter(section, s.chapterBeginUs);
        if (statsHere) {
            for (size_t i = 0; i < s.statsPending.size(); ++i) {
                if (statsWriter_->WritePlayerStats(s.statsPending[i])) {
                    ++stats_.playerStatsRecords;
                    continue;
                }
                // Per RECORD: a time that went backwards for one user must not cost the other
                // fifteen their scoreboard. Counted here so a file short of records says so.
                ++stats_.playerStatsRefused;
                if (log_) {
                    log_->Write(LogLevel::Error, "capture",
                                "S_PLAYER_STATS record for user index %u REFUSED by the writer: %s",
                                unsigned(s.statsPending[i].userIndex),
                                statsWriter_->LastError() ? statsWriter_->LastError() : "unnamed");
                }
            }
            s.statsPending.clear();
        }
        for (uint16_t id = 1; id <= kMaxStreamId; ++id) {
            StreamBuf& b = s.bufs[id];
            if (SectionOf(id) != section || b.records == 0) {
                continue;
            }
            // A stream inside the completeness law reports its own observed count, ZERO INCLUDED.
            // Substituting the block's record count for a missing observation compares the count
            // against itself and can only ever agree -- which is how a stream that stopped observing
            // altogether would have written a green block. The substitution is right only OUTSIDE
            // the law (never configured, or a role with no engine source), where there is no
            // observation to report and the block still has to declare what it holds.
            const uint32_t observed = accounting_.IsAccounted(id) ? accounting_.ChapterObserved(id)
                                                                  : b.records;
            const uint32_t stored = accounting_.ChapterStored(id);
            if (IsContainerFramed(id)) {
                // The width comes from sizeof(), NOT from this block. A container-framed stream is a
                // packed array of a struct whose size is a compile-time fact, so the byte length and
                // the record count are two statements about one thing and must agree. This is the
                // check that would have caught A2 when it happened instead of never.
                const uint32_t declared = stored ? stored : b.records;
                const uint32_t width = ContainerRecordBytes(id);
                if (width && b.used != declared * width) {
                    ++stats_.blockWidthMismatch;
                    if (log_) {
                        log_->Write(LogLevel::Error, "capture",
                                    "%s block holds %u bytes but declares %u records of %u; the "
                                    "count and the payload disagree so the block is REFUSED rather "
                                    "than written with a count nothing can check",
                                    StreamName(id), b.used, declared, width);
                    }
                    continue;
                }
                s.writer->WriteStreamBlock(id, SchemaOf(id), b.bytes.data(), b.used, observed,
                                           declared);
                s.bytesToWriter += b.used;
            } else {
                // [u32 exact bit length][packed records]. The length is what lets a decoder prove it
                // consumed what the encoder produced rather than assume it did.
                const uint32_t bits = static_cast<uint32_t>(b.bits);
                const uint32_t packed = (bits + 7u) / 8u;
                const uint32_t payload = kBlockPrefixBytes + packed;
                memmove(b.bytes.data() + kBlockPrefixBytes, b.bytes.data(), packed);
                memcpy(b.bytes.data(), &bits, sizeof bits);
                s.writer->WriteStreamBlock(id, SchemaOf(id), b.bytes.data(), payload,
                                           observed ? observed : b.records,
                                           stored ? stored : b.records);
                s.bytesToWriter += payload;
            }
            stats_.recordsWritten += b.records;
        }
        if (gapHere) {
            s.writer->WriteStreamBlock(kStreamGap, kSchemaGap, sectionGaps,
                                       gapHere * sizeof(GapRecordDisk), gapHere, gapHere);
        }
        if (dropHere) {
            s.writer->WriteStreamBlock(kStreamDropped, kSchemaGap, sectionDrops,
                                       dropHere * sizeof(GapRecordDisk), dropHere, dropHere);
        }
        s.writer->EndChapter(tEnd);
        ++stats_.chaptersWritten;
    }

    for (uint16_t id = 0; id <= kMaxStreamId; ++id) {
        s.bufs[id].used = 0;
        s.bufs[id].bits = 0;
        s.bufs[id].records = 0;
    }
    s.ResetEncoders();
    stats_.bytesToWriter = s.bytesToWriter;
}

void ChapterBuilder::Close() {
    // ★ Before the last flush: every ENABLED stream that never observed anything says so, in the file.
    //
    // completeness.h has carried EverObserved() and the reason for it since A3 -- "its arithmetic
    // closes at 0 == 0 + 0, which is the vacuous pass the invariant exists to forbid" -- but nothing
    // ever called it, so the streams it describes stayed silent. S_WORLD is configured with an engine
    // observed-source and has no producer at all, so it closed every file at 0 == 0 + 0 and read as a
    // fully-accounted stream; S_ORACLE does the same. UnverifiedStreams() could not name either,
    // because it requires stored > 0 and these store nothing.
    //
    // This is not an error claim. It is the difference between "nothing happened on this stream" and
    // "something happened and the file lost it", which a reader cannot otherwise tell apart -- and on
    // S_VOIP it is exactly the statement that nobody talked, rather than that nobody was listening.
    // Scoped to the streams THIS packet produces samples on -- kStreamTick(3) through
    // kStreamOracle(15). The container owns 0-2 and 16-18 (manifest, index, gap, dropped, degraded)
    // and fills them itself, so declaring them never-produced would be noise; declaring S_DEGRADED
    // never-produced inside a degraded record would also be its own refutation.
    for (uint16_t id = kStreamTick; id <= kStreamOracle; ++id) {
        if (cfg_.streamEnabled[id] && !accounting_.EverObserved(static_cast<StreamId>(id))) {
            NoteDegraded(lastSourceUs_, id, kDegradeStreamNeverProduced, 0);
        }
    }
    // S_GAMESTATE is outside the loop above because that loop predates the stream and stops at
    // kStreamOracle. A stream this packet produces samples on says "never produced" for itself.
    if (cfg_.streamEnabled[kStreamGameState] && !accounting_.EverObserved(kStreamGameState)) {
        NoteDegraded(lastSourceUs_, kStreamGameState, kDegradeStreamNeverProduced, 0);
    }
    // ARCH-21 S4.5: a round still open at close is a partialEnd ROW, not a round that vanished --
    // it is the only footage of that round anyone has. And the file names its marker source even
    // when the honest answer is None.
    impl_->local.Finish(lastSourceUs_);
    impl_->choice.Settle();
    // Settle() is the last word on provenance, so anything still held is now decidable: filed if the
    // file turned out to be WIRE, counted and dropped if it did not.
    ResolveWireMarks();
    FlushChapter();
    impl_->writer = nullptr;
}

void ChapterBuilder::NoteDegraded(uint64_t tUs, uint16_t what, uint16_t reason, uint32_t detail) {
    DegradedRecordDisk d{};
    d.tUs = tUs ? tUs : lastSourceUs_;
    d.what = what;
    d.reason = reason;
    d.detail = detail;
    AppendRaw(impl_->bufs[kStreamDegraded], &d, sizeof d);
}

void ChapterBuilder::NoteDropped(StreamId stream, uint64_t tUs, uint16_t reason, uint32_t count) {
    accounting_.NoteDropped(stream, tUs ? tUs : lastSourceUs_, reason, count);
}

// ---------------------------------------------------------------------------------------------
// Per-record dispatch
// ---------------------------------------------------------------------------------------------
namespace {

// `fill` is carried in, never inferred here. The builder cannot know which engine accessors answered;
// only the gather that called them knows, and a builder that guessed would be manufacturing the very
// distinction the mask exists to preserve.
PlayerFrame FromEngine(const PlayerState& p, uint32_t fill) {
    PlayerFrame f{};
    f.fill = fill;
    memcpy(f.hmdOri, p.hmdOri, sizeof f.hmdOri);
    memcpy(f.lHandOri, p.lHandOri, sizeof f.lHandOri);
    memcpy(f.rHandOri, p.rHandOri, sizeof f.rHandOri);
    memcpy(f.refVel, p.refVel, sizeof f.refVel);
    memcpy(f.lHandOff, p.lHandOff, sizeof f.lHandOff);
    memcpy(f.rHandOff, p.rHandOff, sizeof f.rHandOff);
    f.bendBack = p.bendBack;
    f.bendFwd = p.bendFwd;
    f.restPoseHeight = p.restPoseHeight;
    f.trigL = p.trigL;
    f.trigR = p.trigR;
    f.bits28 = p.bits28;
    // The wire splits each holding id into a slot index and a generation index; IEntity hands over the
    // combined dword. Splitting it here keeps one storage shape for both sources.
    f.holdLSlot = p.holdL & 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    f.holdLGen = (p.holdL >> 16) & 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    f.holdRSlot = p.holdR & 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    f.holdRGen = (p.holdR >> 16) & 0xFFFFu;  // facts-ok: NETVAR.SLOT_NONE a u16 mask over a packed id, not the engine no-slot field
    f.anchor = p.anchor;
    f.hasDup = false;
    return f;
}

// The identity pair and `fill` are carried in, never inferred. IEntity::ReadBody supplies a transform and
// nothing else -- no net id, because the offset inside the net-actor CI is not yet derived on PC and
// W1-E stopped rather than guess it. So a sampled body row says so instead of storing a zero that
// would read like an identity.
BodyFrame FromEngineBody(const BodyState& b, uint64_t idValue, uint32_t idTag, uint32_t fill) {
    BodyFrame f{};
    f.idValue = idValue;
    f.idTag = idTag;
    f.fill = fill;
    memcpy(f.com, b.com, sizeof f.com);
    memcpy(f.ori, b.ori, sizeof f.ori);
    memcpy(f.vel, b.vel, sizeof f.vel);
    memcpy(f.angVel, b.angVel, sizeof f.angVel);
    memcpy(f.lastHeldCom, b.lastHeldCom, sizeof f.lastHeldCom);
    f.bits11 = b.bits11;
    f.hasHeader = false;
    return f;
}

// A round boundary is a seek target. Locked constraint 6 is "always-on capture, one file per match
// with round markers", and a marker that lands in the middle of a chapter is not something playback
// can jump to -- it has to decode from the previous keyframe to reach it. So a boundary marker
// BEGINS a chapter, on both sides of the round.
//
// This predicate used to be consulted only from Consume, on a kRecMark record that
// nothing constructs. The derived markers reach the file through AppendMark in DispatchRecord below,
// which is why the split was dead in production while every test stayed green. The predicate lives up
// here now because that is the caller that matters.
bool IsRoundBoundary(uint16_t kind) {
    return kind == kMarkMatchBegin || kind == kMarkMatchEnd || kind == kMarkRoundBegin ||
           kind == kMarkRoundEnd;
}

}  // namespace

// Cut here, then open the next chapter AT the boundary so the marker is the first thing after a
// keyframe rather than something a seek has to decode forward to reach.
void ChapterBuilder::BeginBoundaryChapter(uint64_t tUs) {
    if (impl_->chapterOpen) {
        FlushChapter();
        ++stats_.roundBoundarySplits;
    }
    EnsureChapterOpen(tUs);
    if (tUs > impl_->chapterEndUs) {
        impl_->chapterEndUs = tUs;
    }
}

void ChapterBuilder::FileMark(const MarkRec& m) {
    if (IsRoundBoundary(m.kind)) {
        // A held mark is filed when the choice settles, which can be a whole arm-up window after its
        // own instant. Cutting there would open the next chapter at an instant the CURRENT chapter
        // has already passed, and two chapters that overlap break the seek index a boundary exists
        // for. Such a boundary is filed without a cut and counted. On the live path a mark never
        // predates the end of the chapter it lands in, so this costs nothing there.
        if (impl_->chapterOpen && m.tUs < impl_->chapterEndUs) {
            ++stats_.boundaryMarksNotCut;
        } else {
            BeginBoundaryChapter(m.tUs);
        }
    }
    AppendMark(impl_->bufs[kStreamMark], m);
    accounting_.NoteObserved(kStreamMark, 1);
    accounting_.NoteStored(kStreamMark, 1);
    // A marker is in the stream now, so the file's provenance is no longer a guess that can improve.
    impl_->choice.NoteMarkFiled();
}

void ChapterBuilder::ResolveWireMarks() {
    Impl& s = *impl_;
    if (s.pendingWireCount == 0) {
        return;
    }
    const MarkerSource src = s.choice.Source();
    if (src == MarkerSource::NotRecorded) {
        return;
    }
    if (src != MarkerSource::Wire) {
        // LOCAL owns the table, or the file never had a readable source at all. Either way these are
        // counted and not filed: one file, one emitter.
        stats_.wireMarksSuppressed += s.pendingWireCount;
        s.pendingWireCount = 0;
        return;
    }
    for (uint32_t i = 0; i < s.pendingWireCount; ++i) {
        FileMark(s.pendingWire[i]);
    }
    s.pendingWireCount = 0;
}

// The stream a refusal names, spelled as the format's own record name so a log line and a file
// dump say the same word.
static const char* const kStreamNameBodySampled = "S_BODY_SAMPLED";

void ChapterBuilder::DispatchRecord(const RecHeader& hdr, const uint8_t* body, uint64_t tUs) {
    Impl& s = *impl_;

    auto refuse = [&](StreamId stream) {
        ++stats_.encodeRefusals;
        // The record was observed and did not reach the file. That is a DROPPED, not a silent loss:
        // E-C10's shape, with our own encoder as the cause.
        accounting_.NoteDropped(stream, tUs, kGapBlockTooLarge, 1);
    };

    // Names the field the encoder choked on, beside the count `refuse` keeps.
    auto nameRefusal = [&](const char* streamName, const BodyEncoder& enc) {
        if (stats_.encodeRefusalsNamed >= BuilderStats::kMaxRefusalsNamed) {
            return;
        }
        const RefusedField& r = enc.LastRefusal();
        EncodeRefusalNote& n = stats_.encodeRefusalNotes[stats_.encodeRefusalsNamed++];
        n.stream = streamName;
        n.field = r.identified ? r.name : nullptr;
        n.value = r.value;
        n.width = r.width;
    };

    switch (hdr.kind) {
        case kRecTick: {
            if (hdr.bytes < sizeof(TickRec) || !cfg_.streamEnabled[kStreamTick]) return;
            TickRec t{};
            memcpy(&t, body, sizeof t);
            accounting_.BeginFrameSet(kStreamTick, t.frameIndex);
            // One record per tick IS the observation for this stream, so presence and the
            // store are the same event and are reported together.
            accounting_.BeginFrameSet(kStreamTick, t.frameIndex);
            accounting_.NoteEntityPresent(kStreamTick, 0, t.frameIndex, tUs);
            accounting_.NoteEntitySampled(kStreamTick, 0);
            accounting_.EndFrameSet(kStreamTick, tUs);
            bitio::BitWriter* w = s.writers[kStreamTick];
            if (!w || !s.tickEnc.Encode(*w, t)) {
                refuse(kStreamTick);
                return;
            }
            s.bufs[kStreamTick].bits = w->BitsWritten();
            ++s.bufs[kStreamTick].records;
            s.lastTick = t;
            s.haveTick = true;
            return;
        }

        case kRecWirePlayer:
        case kRecWireBody: {
            if (hdr.bytes < sizeof(WireRec)) return;
            WireRec r{};
            memcpy(&r, body, sizeof r);
            const uint8_t* msg = body + sizeof(WireRec);
            if (sizeof(WireRec) + r.bodyBytes > hdr.bytes) return;
            const bool isPlayer = hdr.kind == kRecWirePlayer;
            const StreamId stream = isPlayer ? kStreamPlayerWire : kStreamBodyWire;
            if (!cfg_.streamEnabled[stream]) return;

            // The tap delivered it, so the engine numbered it. That is the whole of `observed` for this
            // stream and it is not our sample loop counting itself.
            accounting_.NoteObserved(stream, 1);

            bitio::BitWriter* w = s.writers[stream];
            if (!w) {
                refuse(stream);
                return;
            }
            bool evicted = false;
            if (isPlayer) {
                PlayerFrame f{};
                const WireDecode d = DecodePlayerBody(msg, r.bodyBytes, f);
                const uint16_t slot = s.playerSlots.Slot(f.hdr0, evicted);
                if (evicted) ++stats_.slotEvictions;
                bool ok;
                if (d == WireDecode::Ok) {
                    ok = s.playerWire.Encode(*w, slot, r.stampUs, f);
                    ++stats_.wireDecoded;
                    if (ok) {
                        s.playerWireShadow[slot].present = true;
                        s.playerWireShadow[slot].stampUs = r.stampUs;
                        s.playerWireShadow[slot].frame = f;
                    }
                } else {
                    // The decode could not be proved, so the body goes in whole. Bigger, never wrong,
                    // and counted by reason so a non-zero count names its own cause.
                    ok = s.playerWire.EncodeVerbatim(*w, slot, r.stampUs, msg, r.bodyBytes);
                    ++stats_.wireVerbatim;
                    ++stats_.wireDecodeFail[static_cast<uint32_t>(d)];
                }
                if (!ok) {
                    refuse(stream);
                    return;
                }
            } else {
                BodyFrame f{};
                const WireDecode d = DecodeBodyBody(msg, r.bodyBytes, f);
                // Grouping only, and it says so: a value whose meaning was not recorded is not an
                // identity, so an unresolvable row groups by its message header rather than by a
                // number nothing can interpret. Nothing downstream may read a clear identity bit as
                // "the net id happened to be zero".
                const uint64_t groupKey = (d == WireDecode::Ok && BodyIdentityResolvable(f.fill))
                                              ? f.idValue
                                              : f.hdr0;
                const uint16_t slot = s.bodySlots.Slot(groupKey, evicted);
                if (evicted) ++stats_.slotEvictions;
                bool ok;
                if (d == WireDecode::Ok) {
                    ok = s.bodyWire.Encode(*w, slot, r.stampUs, f);
                    ++stats_.wireDecoded;
                    if (ok) {
                        s.bodyWireShadow[slot].present = true;
                        s.bodyWireShadow[slot].stampUs = r.stampUs;
                        s.bodyWireShadow[slot].frame = f;
                    }
                } else {
                    ok = s.bodyWire.EncodeVerbatim(*w, slot, r.stampUs, msg, r.bodyBytes);
                    ++stats_.wireVerbatim;
                    ++stats_.wireDecodeFail[static_cast<uint32_t>(d)];
                }
                if (!ok) {
                    refuse(stream);
                    return;
                }
            }
            s.bufs[stream].bits = w->BitsWritten();
            ++s.bufs[stream].records;
            accounting_.NoteStored(stream, 1);
            return;
        }

        case kRecSampledPlayer: {
            if (hdr.bytes < sizeof(SampledPlayerRec) || !cfg_.streamEnabled[kStreamPlayerSampled]) {
                return;
            }
            SampledPlayerRec r{};
            memcpy(&r, body, sizeof r);
            // The user slot, the same key a remote row uses. Folding it into the shadow array is a
            // compression grouping only -- the stored `entity` is the slot itself.
            const uint16_t slot = r.entity % kMaxEntities;
            bitio::BitWriter* w = s.writers[kStreamPlayerSampled];
            const PlayerFrame f = FromEngine(r.state, r.fill);
            if (!w || !s.playerSampled.Encode(*w, slot, r.sourceTimeUs, f)) {
                refuse(kStreamPlayerSampled);
                return;
            }
            s.bufs[kStreamPlayerSampled].bits = w->BitsWritten();
            ++s.bufs[kStreamPlayerSampled].records;
            accounting_.NoteEntitySampled(kStreamPlayerSampled, slot);
            s.playerSampledShadow[slot].present = true;
            s.playerSampledShadow[slot].stampUs = r.sourceTimeUs;
            s.playerSampledShadow[slot].frame = f;
            return;
        }

        case kRecSampledBody: {
            if (hdr.bytes < sizeof(SampledBodyRec) || !cfg_.streamEnabled[kStreamBodySampled]) {
                return;
            }
            SampledBodyRec r{};
            memcpy(&r, body, sizeof r);
            // The delta grouping folds the LOCAL handle; the stored identity is the net id in the
            // frame. Nothing downstream may read this slot as an identity.
            const uint16_t slot = r.handle % kMaxBodies;
            bitio::BitWriter* w = s.writers[kStreamBodySampled];
            const BodyFrame f = FromEngineBody(r.state, r.idValue, r.idTag, r.fill);
            if (!w || !s.bodySampled.Encode(*w, slot, r.sourceTimeUs, f)) {
                if (w) {
                    nameRefusal(kStreamNameBodySampled, s.bodySampled);
                }
                refuse(kStreamBodySampled);
                return;
            }
            s.bufs[kStreamBodySampled].bits = w->BitsWritten();
            ++s.bufs[kStreamBodySampled].records;
            accounting_.NoteEntitySampled(kStreamBodySampled, slot);
            s.bodySampledShadow[slot].present = true;
            s.bodySampledShadow[slot].stampUs = r.sourceTimeUs;
            s.bodySampledShadow[slot].frame = f;
            return;
        }

        case kRecNetvar: {
            if (hdr.bytes < sizeof(NetvarRec) || !cfg_.streamEnabled[kStreamNetvar]) return;
            NetvarRec r{};
            memcpy(&r, body, sizeof r);
            const uint8_t* msg = body + sizeof(NetvarRec);
            if (sizeof(NetvarRec) + r.bodyBytes > hdr.bytes) return;
            ++stats_.netvarRecords;

            const DuvMessage m = DuvParse(msg, r.bodyBytes);
            if (m.status == DuvStatus::TooShort) {
                // Bytes we cannot parse are still bytes the client received. The message is stored on
                // S_EVENT verbatim rather than dropped, and the malformed count is reported.
                ++stats_.netvarMalformed;
                accounting_.NoteObserved(kStreamNetvar, 1);
                accounting_.NoteDropped(kStreamNetvar, tUs, kGapWireSeqJump, 1);
                return;
            }

            DuvSequence::Gap gap{};
            s.duvSeq.Note(m, &gap);
            if (gap.count) {
                accounting_.NoteGap(kStreamNetvar, tUs, kGapWireSeqJump, gap.firstMissing, gap.count);
            }
            accounting_.NoteObserved(kStreamNetvar, 1);

            NetvarMessage nm{};
            // ★ THE DISPATCH AXIS TIME, NOT NetvarRec::arrivalUs. `arrivalUs` here is the engine
            // delegate's argument 5 (capture.cpp OnNetvarMessage passes netvar.cpp's `m.stamp`),
            // which StampIsOnSourceAxis already declares foreign and which W1-L found reads -1 for a
            // locally raised message. `tUs` is what Consume placed this record on -- the capture's
            // own source-time axis, bounded by EnsureChapterOpen into [chapterBegin, chapterEnd] --
            // so a window can put the row at the playhead without a second clock to reconcile.
            nm.stampUs = tUs;
            nm.owner = m.owner;
            nm.slot = m.slot;
            nm.gen = m.gen;
            nm.pktSeq = m.pktSeq;
            nm.hasRowset = m.hasRowset;
            nm.rowsetSeq = m.rowsetSeq;
            nm.varCount = m.varCount > NetvarMessage::kMaxRows ? NetvarMessage::kMaxRows : m.varCount;
            for (uint16_t i = 0; i < nm.varCount; ++i) {
                const DuvRow row = DuvRowAt(m, i);
                nm.rows[i].name = row.name;
                nm.rows[i].value = row.value;
                nm.rows[i].slot = m.slot;
            }
            bitio::BitWriter* w = s.writers[kStreamNetvar];
            if (!w || !s.netvarEnc.Encode(*w, nm)) {
                refuse(kStreamNetvar);
                return;
            }
            s.bufs[kStreamNetvar].bits = w->BitsWritten();
            ++s.bufs[kStreamNetvar].records;
            accounting_.NoteStored(kStreamNetvar, 1);

            // The shadow advances AFTER the record reached the chapter, never before. It used to
            // update inside the loop above, so a refused encode left the shadow holding a value no
            // record in the file carried -- and the shadow is what S_KEY publishes, so the next
            // keyframe would have contained a value the stream never stored while the accounting
            // correctly reported the drop. The file would have disagreed with itself.
            //
            // W2-B states the general rule and it is worth keeping: a cache keyed on INTENT rather
            // than EFFECT survives the fix that was supposed to end it. Every other shadow here was
            // already guarded this way; this one was not.
            for (uint16_t i = 0; i < nm.varCount; ++i) {
                if (Impl::NetvarShadow* sh = s.FindNetvar(nm.rows[i].name, nm.rows[i].slot, true)) {
                    sh->value = nm.rows[i].value;
                    sh->fromDelta = true;
                }
            }

            // S5.7's markers, derived from the same rows on the way past. A marker is an edge in
            // replicated state and the stream carries every edge, so deriving them here means the
            // table in the file is recomputable from the file.
            if (cfg_.streamEnabled[kStreamMark]) {
                MarkRec marks[16];
                const uint32_t markCount = s.markers.Note(nm, tUs, marks, 16);
                s.choice.NoteCorroboratorFed(nm.varCount, tUs);
                for (uint32_t i = 0; i < markCount; ++i) {
                    if (!MarkTimeTrusted(marks[i].tUs)) {
                        s.markers.NoteTimeRefused();
                        continue;
                    }
                    if (s.choice.Source() == MarkerSource::Local) {
                        // ARCH-21 S4.4: one emitter per file, chosen once. The corroborator still
                        // runs -- its watched values must keep advancing or its own baseline rule
                        // would be wrong -- but its markers are counted, never filed. A table with
                        // two producers is the mixed-provenance fork D2 refuses.
                        ++stats_.wireMarksSuppressed;
                        continue;
                    }
                    if (!s.choice.Chosen()) {
                        // The arm-up window, on THIS path too. NotRecorded is not "not local": it is
                        // the window in which the design says buffer nothing, emit nothing, and
                        // filing here is what let one file hold wire rows and local rows under a
                        // single markerSource that no reader can split. Held, not filed.
                        if (s.pendingWireCount < Impl::kMaxPendingWireMarks) {
                            s.pendingWire[s.pendingWireCount++] = marks[i];
                        } else {
                            ++stats_.wireMarksHeldDropped;
                        }
                        continue;
                    }
                    FileMark(marks[i]);
                }
                // The record that fed the corroborator may also have ended the window.
                ResolveWireMarks();
            }

            if (!s.netvarUnverifiedDeclared &&
                accounting_.SourceOf(kStreamNetvar) == ObservedSource::None) {
                // The role has no wire sequence to count. Say so in the file: a reader must not read
                // observed == stored on this stream as a completeness result.
                NoteDegraded(tUs, kStreamNetvar, kDegradeCompletenessUnverified, cfg_.captureRole);
                s.netvarUnverifiedDeclared = true;
            }
            return;
        }

        case kRecGameState: {
            if (hdr.bytes < sizeof(GameStateRec) || !cfg_.streamEnabled[kStreamGameState]) return;
            GameStateRec r{};
            memcpy(&r, body, sizeof r);
            // The observation is NOT counted here. Between this point and the store below, a round
            // boundary can cut the chapter, and an observation counted before the cut is filed
            // against the chapter that closed while its record goes into the one that opened. Both
            // move together at the store, in NoteEmitted.
            const bool sampleOk =
                r.sample.availability == static_cast<uint8_t>(gamestate::Availability::Ok);
            s.choice.NoteLocalSample(sampleOk, tUs);
            // A readable local sample settles the file to LOCAL, which is the answer the held wire
            // marks were waiting for: they are discarded here rather than at close, so nothing can
            // file them in between.
            ResolveWireMarks();

            // The markers come FIRST so that a boundary opens its chapter before the sample that
            // caused it is written: a seek to the round lands on the sample that starts it, not one
            // chapter earlier.
            if (s.choice.Source() == MarkerSource::Local && cfg_.streamEnabled[kStreamMark]) {
                MarkRec marks[16];
                const uint32_t markCount = s.local.Note(r.sample, marks, 16);
                for (uint32_t i = 0; i < markCount; ++i) {
                    if (!MarkTimeTrusted(marks[i].tUs)) {
                        s.markers.NoteTimeRefused();
                        continue;
                    }
                    FileMark(marks[i]);
                }
            }

            // A refused round boundary is a fact about THIS capture that no later reader can
            // recover from the file: the row was never written. Name it here, once each, for the
            // first kMaxRefusalsNamed of them; the total is in LocalMarkerStats().
            {
                LocalMarkerSource::RefusalNote refused[LocalMarkerSource::kMaxRefusalsNamed];
                const uint32_t nRefused =
                    s.local.TakeRefusals(refused, LocalMarkerSource::kMaxRefusalsNamed);
                for (uint32_t i = 0; log_ && i < nRefused; ++i) {
                    log_->Write(LogLevel::Warn, "capture",
                                "round table REFUSED a boundary: offered round %u, admitted round %u "
                                "stands, phase %u, capture t=%llu us",
                                unsigned(refused[i].offered), unsigned(refused[i].admitted),
                                unsigned(refused[i].phase),
                                static_cast<unsigned long long>(refused[i].captureTimeUs));
                }
            }

            EnsureChapterOpen(tUs);
            GameStateRecordDisk d{};
            d.engineTimeUs = r.sample.engineTimeUs;
            d.captureTimeUs = r.sample.captureTimeUs;
            d.phaseSymbol = r.sample.phaseSymbol;
            d.pausedSymbol = r.sample.pausedSymbol;
            d.roundCounter = r.sample.roundCounter;
            d.bluePoints = r.sample.bluePoints;
            d.redPoints = r.sample.redPoints;
            d.blueMatchScore = r.sample.blueMatchScore;
            d.redMatchScore = r.sample.redMatchScore;
            d.frisbeeOwnerId = r.sample.frisbeeOwnerId;
            d.gameTimer = r.sample.gameTimer;
            d.countdownTimer = r.sample.countdownTimer;
            d.firstLaunchOfRound = r.sample.firstLaunchOfRound;
            d.roundWinningTeam = r.sample.roundWinningTeam;
            d.lastScoreTeam = r.sample.lastScoreTeam;
            d.lastScorePoints = r.sample.lastScorePoints;
            d.present = r.sample.present;
            d.phase = r.sample.phase;
            d.availability = r.sample.availability;
            if (!AppendRaw(s.bufs[kStreamGameState], &d, sizeof d)) {
                refuse(kStreamGameState);
                return;
            }
            accounting_.NoteEmitted(kStreamGameState, 1);
            ++stats_.gameStateRecords;
            return;
        }

        case kRecPlayerStats: {
            if (hdr.bytes < sizeof(PlayerStatsRec) || !cfg_.streamEnabled[kStreamPlayerStats]) return;
            PlayerStatsRec r{};
            memcpy(&r, body, sizeof r);
            // NO accounting_ call, deliberately. W1-B ruled S_PLAYER_STATS out of the completeness
            // law because the engine publishes no numbering of stat CHANGES -- an `observed` for it
            // could only ever be our own loop count, which is the fabricated denominator IDemoWriter
            // forbids. A stream whose deficit cannot be measured stays out of the law rather than
            // being fed a number nothing can check.
            if (!statsWriter_) {
                ++stats_.playerStatsRefused;
                return;
            }
            EnsureChapterOpen(tUs);

            PlayerStatsRecord d{};
            d.xplatformId = r.xplatformId;
            d.engineTimeUs = r.engineTimeUs;
            d.captureTimeUs = r.sourceTimeUs;
            d.possessionTime = r.v.possessionTime;
            d.points = r.v.points;
            d.goals = r.v.goals;
            d.stuns = r.v.stuns;
            d.blocks = r.v.blocks;
            d.assists = r.v.assists;
            d.saves = r.v.saves;
            d.interceptions = r.v.interceptions;
            d.steals = r.v.steals;
            d.catches = r.v.catches;
            d.passes = r.v.passes;
            d.shotsTaken = r.v.shotsTaken;
            d.ping = r.v.ping;
            d.packetLossRatio = r.v.packetLossRatio;
            // The reader's mask, unchanged. Every value whose bit is clear is replaced by the
            // format's own absence sentinel inside WritePlayerStats, so a stat this build cannot
            // source lands on disk as "not recorded" and never as a score of zero.
            d.fill = r.fill;
            d.userIndex = r.userIndex;
            d.isEmotePlaying = r.v.isEmotePlaying;
            d.keyframe = r.keyframe;
            s.statsPending.push_back(d);
            return;
        }


        case kRecEvent: {
            if (hdr.bytes < sizeof(EventRec) || !cfg_.streamEnabled[kStreamEvent]) return;
            EventRec r{};
            memcpy(&r, body, sizeof r);
            if (sizeof(EventRec) + r.bodyBytes > hdr.bytes) return;
            accounting_.NoteObserved(kStreamEvent, 1);
            EventMessage e{};
            e.stampUs = r.stampUs;
            e.dst = r.dst;
            e.typeIndex = r.typeIndex;
            e.bodyBytes = r.bodyBytes;
            e.body = body + sizeof(EventRec);
            bitio::BitWriter* w = s.writers[kStreamEvent];
            if (!w || !s.eventEnc.Encode(*w, e)) {
                refuse(kStreamEvent);
                return;
            }
            s.bufs[kStreamEvent].bits = w->BitsWritten();
            ++s.bufs[kStreamEvent].records;
            accounting_.NoteStored(kStreamEvent, 1);

            if (s.ownerStateGametype == 0 && r.typeIndex < TapTypeCount() &&
                events::LobbyKindForName(TapTable()[r.typeIndex].structName) ==
                    events::LobbyKind::OwnerState) {
                uint64_t stated = 0;
                if (gametype::ParseOwnerStateGametype(reinterpret_cast<const char*>(e.body),
                                                      e.bodyBytes, &stated) &&
                    gametype::IsFull(stated)) {
                    s.ownerStateGametype = stated;
                }
            }

            // S5.7's three lobby-derived markers, taken from the same body on the way past -- the exact
            // shape kRecNetvar already uses for the six netvar-derived kinds.
            //
            // ★ The type index is resolved to a kind through the tap table's struct NAME, never through
            // the index itself. The index is positional: a row inserted above would re-aim it silently,
            // which is the failure the table's own header warns about.
            if (cfg_.streamEnabled[kStreamMark] && r.typeIndex < TapTypeCount()) {
                const events::LobbyKind kind =
                    events::LobbyKindForName(TapTable()[r.typeIndex].structName);
                if (kind != events::LobbyKind::Other) {
                    MarkRec marks[8];
                    // ★ THE AXIS, NOT THE STAMP -- L3-RUN-6 F1. `r.stampUs` is the tap's arrival
                    // microsecond, which `StampIsOnSourceAxis` already declares foreign for
                    // kRecEvent; `tUs` is the capture-axis time Consume resolved for this record.
                    // Filing the arrival stamp put L2 #7's JOIN and two TEAM_CHANGE markers at
                    // 1.874 s -- 17.0 s BEFORE t0 -- so the UI's `t - t0Us` (panel.cpp) read
                    // -17025384 us and `round`/`goto round` clamped to the first tick. The netvar
                    // deriver above is already fed `tUs`; this one was the last foreign stamp filed
                    // as a file time.
                    const uint32_t markCount =
                        s.lobby.Note(kind, e.body, e.bodyBytes, tUs, marks, 8);
                    for (uint32_t i = 0; i < markCount; ++i) {
                        // AUDIT_M8 F2 / L2 F5: these are the three markers that reached the file at
                        // 18446744073709.551 s. r.stampUs is W1-L's arrival microsecond now, so the
                        // cause is fixed upstream; the refusal is here so a stamp that is not a time
                        // can never be filed as one again.
                        if (!MarkTimeTrusted(marks[i].tUs)) {
                            s.markers.NoteTimeRefused();
                            continue;
                        }
                        if (IsRoundBoundary(marks[i].kind)) {
                            BeginBoundaryChapter(marks[i].tUs);
                        }
                        AppendMark(s.bufs[kStreamMark], marks[i]);
                        accounting_.NoteObserved(kStreamMark, 1);
                        accounting_.NoteStored(kStreamMark, 1);
                    }
                }
            }
            return;
        }

        case kRecAsSeen: {
            if (hdr.bytes < sizeof(AsSeenRec) || !cfg_.streamEnabled[kStreamAsSeen]) return;
            AsSeenRec r{};
            memcpy(&r, body, sizeof r);
            AsSeenFrame f{};
            f.sourceTimeUs = r.sourceTimeUs;
            f.frameIndex = r.frameIndex;
            f.sample = r.sample;
            f.playerCount = r.playerCount > AsSeenFrame::kMaxPlayers ? AsSeenFrame::kMaxPlayers
                                                                     : r.playerCount;
            const size_t need = sizeof(AsSeenRec) + size_t(f.playerCount) * sizeof(AsSeenPerPlayer);
            if (need > hdr.bytes) return;
            memcpy(f.players, body + sizeof(AsSeenRec),
                   size_t(f.playerCount) * sizeof(AsSeenPerPlayer));
            bitio::BitWriter* w = s.writers[kStreamAsSeen];
            if (!w || !s.asSeenEnc.Encode(*w, f)) {
                refuse(kStreamAsSeen);
                return;
            }
            s.bufs[kStreamAsSeen].bits = w->BitsWritten();
            ++s.bufs[kStreamAsSeen].records;
            // One record per tick IS the observation for this stream, so presence and the
            // store are the same event and are reported together.
            accounting_.BeginFrameSet(kStreamAsSeen, r.frameIndex);
            accounting_.NoteEntityPresent(kStreamAsSeen, 0, r.frameIndex, tUs);
            accounting_.NoteEntitySampled(kStreamAsSeen, 0);
            accounting_.EndFrameSet(kStreamAsSeen, tUs);
            return;
        }

        case kRecFrameSet: {
            if (hdr.bytes < sizeof(FrameSetRec)) return;
            FrameSetRec r{};
            memcpy(&r, body, sizeof r);
            const size_t need = sizeof(FrameSetRec) + size_t(r.count) * sizeof(uint16_t);
            if (need > hdr.bytes) return;
            // The previous frame's set is closed HERE rather than when it opened, because the records
            // it accounts for arrive between the two. Closing it is what turns present-and-unsampled
            // into a DROPPED record.
            if (s.frameSetOpen[r.stream <= kMaxStreamId ? r.stream : 0]) {
                accounting_.EndFrameSet(r.stream, tUs);
            }
            accounting_.BeginFrameSet(r.stream, r.frameIndex);
            const uint8_t* ids = body + sizeof(FrameSetRec);
            for (uint16_t i = 0; i < r.count; ++i) {
                uint16_t e = 0;
                memcpy(&e, ids + size_t(i) * sizeof(uint16_t), sizeof e);
                accounting_.NoteEntityPresent(r.stream, e, r.frameIndex, r.tUs);
            }
            if (r.stream <= kMaxStreamId) {
                s.frameSetOpen[r.stream] = true;
            }
            return;
        }

        case kRecMark: {
            if (hdr.bytes < sizeof(MarkRec) || !cfg_.streamEnabled[kStreamMark]) return;
            MarkRec r{};
            memcpy(&r, body, sizeof r);
            MarkRecordDisk d{};
            d.tUs = r.tUs;
            d.kind = r.kind;
            d.index = r.index;
            d.payload = r.payload;
            if (!AppendRaw(s.bufs[kStreamMark], &d, sizeof d)) {
                refuse(kStreamMark);
                return;
            }
            accounting_.NoteObserved(kStreamMark, 1);
            accounting_.NoteStored(kStreamMark, 1);
            return;
        }

        case kRecVoip: {
            if (hdr.bytes < sizeof(VoipRec) || !cfg_.streamEnabled[kStreamVoip]) return;
            VoipRec r{};
            memcpy(&r, body, sizeof r);
            if (sizeof(VoipRec) + r.bodyBytes > hdr.bytes) return;
            accounting_.NoteObserved(kStreamVoip, 1);
            bitio::BitWriter* w = s.writers[kStreamVoip];
            // The voice payload is opaque and relayed verbatim by the host, so it is stored the same
            // way: the record head plus the bytes, with no attempt to interpret the codec.
            if (!w || !EncodeRawRecord(*w, body, static_cast<uint16_t>(sizeof(VoipRec) + r.bodyBytes))) {
                refuse(kStreamVoip);
                return;
            }
            s.bufs[kStreamVoip].bits = w->BitsWritten();
            ++s.bufs[kStreamVoip].records;
            accounting_.NoteStored(kStreamVoip, 1);
            return;
        }

        case kRecOracle: {
            if (hdr.bytes < sizeof(OracleRec) || !cfg_.streamEnabled[kStreamOracle]) return;
            OracleRec r{};
            memcpy(&r, body, sizeof r);
            const size_t need = sizeof(OracleRec) + r.sessionBytes + r.bonesBytes;
            if (need > hdr.bytes) return;
            bitio::BitWriter* w = s.writers[kStreamOracle];
            if (!w || !EncodeRawRecord(*w, body, static_cast<uint16_t>(need))) {
                refuse(kStreamOracle);
                return;
            }
            s.bufs[kStreamOracle].bits = w->BitsWritten();
            ++s.bufs[kStreamOracle].records;
            // One record per CORE chapter IS the observation for this stream: the decision to emit
            // and the store are one event, so the two counters move in one call.
            accounting_.NoteEmitted(kStreamOracle, 1);
            return;
        }

        case kRecDegraded: {
            if (hdr.bytes < sizeof(DegradedRec)) return;
            DegradedRec r{};
            memcpy(&r, body, sizeof r);
            NoteDegraded(r.tUs, r.what, r.reason, r.detail);
            return;
        }

        default:
            return;
    }
}

// ---------------------------------------------------------------------------------------------

namespace {

// Which record kinds carry a time on THIS FILE'S source-time axis. A record the capture stamped
// itself does. A record carrying a stamp the wire or the tap layer supplied does NOT -- those are
// the sender's clock, or the tap's arrival counter, or nothing at all.
//
// L5 re-run #4 (2026-09-03) is what treating them alike costs. Its S_EVENT rows arrived stamped 6,
// 34, 36, 65 and 0xFFFFFFFFFFFFFFFF; each one became a chapter's t_begin, and the next real tick was
// then a keyframe-interval away from it by construction. The file cut 64 core chapters in 32 s where
// its own 2.0 s cadence asks for 16, wrote a full keyframe into each, and spent 77 KB of a live
// match on 64 keyframes and almost nothing else. Its chapter index reads t=[6..18446744073709551615],
// so seek has nothing to seek on.
//
// The foreign stamp is still written into the record verbatim -- S6.3 says a wire record is never
// re-stamped and constraint 8 is lossless. It simply does not get to say when a chapter began.
bool StampIsOnSourceAxis(uint16_t kind) {
    switch (kind) {
        case kRecTick:
        case kRecSampledPlayer:
        case kRecSampledBody:
        case kRecAsSeen:
        case kRecOracle:
        case kRecWorldOrigin:
        case kRecMark:
            return true;
        default:
            // kRecWirePlayer, kRecWireBody: the emitter's stamp out of the message body.
            // kRecNetvar, kRecEvent: the tap's arrival stamp. kRecVoip: the voice frame's own.
            return false;
    }
}

}  // namespace

void ChapterBuilder::Consume(const void* record, uint32_t bytes) {
    Impl& s = *impl_;
    if (!s.writer || bytes < sizeof(RecHeader)) {
        return;
    }
    RecHeader hdr{};
    memcpy(&hdr, record, sizeof hdr);
    if (size_t(sizeof(RecHeader)) + hdr.bytes > bytes) {
        return;
    }
    const uint8_t* body = static_cast<const uint8_t*>(record) + sizeof(RecHeader);
    ++stats_.recordsIn;

    // Every record carries the time it belongs to, and the chapter's span is those times. A record kind
    // with no time of its own rides the last one seen.
    uint64_t tUs = lastSourceUs_;
    auto take = [&](size_t need, size_t off) {
        if (hdr.bytes >= need) {
            uint64_t v = 0;
            memcpy(&v, body + off, sizeof v);
            tUs = v;
        }
    };
    switch (hdr.kind) {
        case kRecTick: take(sizeof(TickRec), offsetof(TickRec, sourceTimeUs)); break;
        case kRecWirePlayer:
        case kRecWireBody: take(sizeof(WireRec), offsetof(WireRec, stampUs)); break;
        case kRecSampledPlayer:
            take(sizeof(SampledPlayerRec), offsetof(SampledPlayerRec, sourceTimeUs));
            break;
        case kRecSampledBody:
            take(sizeof(SampledBodyRec), offsetof(SampledBodyRec, sourceTimeUs));
            break;
        case kRecNetvar: take(sizeof(NetvarRec), offsetof(NetvarRec, arrivalUs)); break;
        case kRecEvent: take(sizeof(EventRec), offsetof(EventRec, stampUs)); break;
        case kRecAsSeen: take(sizeof(AsSeenRec), offsetof(AsSeenRec, sourceTimeUs)); break;
        case kRecMark: take(sizeof(MarkRec), offsetof(MarkRec, tUs)); break;
        case kRecOracle: take(sizeof(OracleRec), offsetof(OracleRec, sourceTimeUs)); break;
        case kRecVoip: take(sizeof(VoipRec), offsetof(VoipRec, tUs)); break;
        case kRecWorldOrigin:
            take(sizeof(WorldOriginRec), offsetof(WorldOriginRec, sourceTimeUs));
            break;
        default: break;
    }
    // The axis, not the stamp. Everything that POSITIONS this record in the file -- the chapter's
    // span, a marker derived on the way past, a DEGRADED or DROPPED row -- is placed on the capture's
    // own source-time axis; a foreign stamp rides the last axis time the capture stamped.
    const bool onAxis = StampIsOnSourceAxis(hdr.kind);
    if (onAxis && tUs) {
        lastSourceUs_ = tUs;
    }
    const uint64_t axisUs = onAxis ? tUs : lastSourceUs_;
    if (!onAxis) {
        ++stats_.foreignStampsRidden;
    }

    // ★ HANDLED BEFORE EnsureChapterOpen, and the order is the whole point. This is the state a
    // keyframe is WRITTEN FROM, and EnsureChapterOpen writes the keyframe. Dispatched in the normal
    // place -- after the chapter has opened -- the first keyframe of every file states "offset not
    // sourced" for a session that had an offset from frame 0, which is the field lying about its own
    // provenance in exactly the way `valid` exists to prevent.
    //
    // It stores no bytes and belongs to no stream, so it neither opens a chapter nor extends one.
    if (hdr.kind == kRecWorldOrigin) {
        if (hdr.bytes >= sizeof(WorldOriginRec)) {
            WorldOriginRec r{};
            memcpy(&r, body, sizeof r);

            // A CHANGE ENDS THE CHAPTER. A re-origin mid-chapter would leave the records before it and
            // after it in different coordinate frames while the keyframe stated only one, so every
            // body on one side of the shift would be published 100+ metres out with nothing failing.
            // The trigger is a 100 m move, so this fires a handful of times a session at most, and it
            // turns "which records does this offset apply to" into a question the file answers
            // structurally rather than one playback has to guess.
            //
            // Compared on the raw bytes, not as floats: this asks whether the engine's value changed,
            // and NaN != NaN would otherwise split a chapter every single tick.
            const bool changed = s.haveWorldOrigin &&
                                 (memcmp(&r.x, &s.worldOrigin.x, sizeof(float) * 3) != 0 ||
                                  r.valid != s.worldOrigin.valid);
            if (changed && s.chapterOpen) {
                FlushChapter();
                ++stats_.worldOriginSplits;
            }
            s.worldOrigin = r;
            s.haveWorldOrigin = true;
        }
        return;
    }

    if (hdr.kind == kRecMark && hdr.bytes >= sizeof(MarkRec)) {
        MarkRec m{};
        memcpy(&m, body, sizeof m);
        if (IsRoundBoundary(m.kind) && s.chapterOpen) {
            FlushChapter();
            ++stats_.roundBoundarySplits;
        }
    }

    // â˜… CUT BEFORE PLACING, not after. The cut below fires once a chapter has ALREADY been
    // stretched past the keyframe interval, so a single record that jumps the axis forward writes
    // itself into the old chapter and leaves a chapter as long as the jump. RECORDER-5: the 09-19
    // capture's chapter 2 spanned 3846 s that way -- it held the pre-open records at the origin and
    // then the first tick arrived on the peer clock. The origin bug is fixed at its source, but the
    // invariant "no chapter is longer than the keyframe interval" is worth holding structurally,
    // because every seek, every thumbnail and every scrub percentage is built on it.
    // The test is the GAP this record opens, not the span it would produce: a jump wider than a
    // whole chapter is an axis change or a hole, while the ordinary two-millisecond step past the
    // interval is the normal cut and still belongs to the chapter it ends.
    if (s.chapterOpen && axisUs > s.chapterEndUs &&
        axisUs - s.chapterEndUs >= cfg_.keyframeIntervalUs) {
        FlushChapter();
        ++stats_.axisJumpSplits;
    }
    EnsureChapterOpen(axisUs);
    if (axisUs > s.chapterEndUs) {
        s.chapterEndUs = axisUs;
    }

    DispatchRecord(hdr, body, axisUs);

    // Close on time or on size. Both are S5.2 conditions and neither is an optimisation: the time one
    // is the seek cadence, the size one bounds a recovery scan.
    // Subtraction, not `begin + interval`: the addition wrapped when a chapter opened on a
    // 0xFFFFFFFFFFFFFFFF stamp, which made `timeUp` true for every record that followed.
    const bool timeUp = s.chapterEndUs >= s.chapterBeginUs &&
                        s.chapterEndUs - s.chapterBeginUs >= cfg_.keyframeIntervalUs;
    bool sizeUp = false;
    uint32_t coreBytes = 0;
    for (uint16_t id = 1; id <= kMaxStreamId; ++id) {
        const StreamBuf& b = s.bufs[id];
        const uint32_t used = IsContainerFramed(id) ? b.used
                                                    : static_cast<uint32_t>((b.bits + 7) / 8);
        if (used >= kStreamSoftCapBytes) {
            sizeUp = true;
        }
        if (SectionOf(id) == kSectionCore) {
            coreBytes += used;
        }
    }
    if (coreBytes >= kSectionSoftCapBytes) {
        sizeUp = true;
    }
    if (timeUp || sizeUp) {
        FlushChapter();
    }
}

}  // namespace capture
}  // namespace replay
