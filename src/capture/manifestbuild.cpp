#include <atomic>

#include "capture/manifestbuild.h"

#include <string.h>

#include "capture/streamcodec.h"
#include "foundation/demofile/format.h"

namespace replay {
namespace capture {
namespace {

// ★ The closed set. A message type absent from this table cannot be recorded, and the table travels in
// the manifest, so a file states its own event coverage rather than leaving a reader to infer it from
// what happens to be present. W3-E's per-message-type disposition table is measured against this list.
//
// ★ ORDER IS LOAD-BEARING. An S_EVENT record stores an INDEX into this table, so inserting a row
// renumbers every row below it. Files already written carry their own copy of the table and stay
// readable -- that is exactly what kKeyBlockTapTable bought -- but any code holding a hardcoded index
// would silently re-aim. Nothing does: every consumer resolves through the struct NAME.
//
// ★ AND EVERY NAME HERE IS CHECKED AGAINST THE BINARY, because CMatSym::TypeId is a total function
// over strings and therefore cannot reject a typo. A name that is not in the image hashes to a symbol
// nothing ever sends, Listen() succeeds, ActiveCount() counts it, and the stream is silently empty.
// tests/capture/capture_corpus verifies each name occurs as name + NUL in echovr.exe.
//
// Symbols are computed at run time through IWiretap::Symbol (CMatSym of the struct name) rather than
// written down here: W1-L found the symbol constants appear nowhere in the instruction stream because
// the engine computes them, and a second hand-typed copy would be a fact with one source.
const TapType kTaps[] = {
    // per-frame traffic, each on its own stream
    {"SR15NetPlayerStateMsg", false, true, kStreamPlayerWire},
    {"SR15NetPhysicsSimStateEvent", false, true, kStreamBodyWire},
    {"SR15NetReplicatedDeltaUnreliableVars", false, true, kStreamNetvar},

    // events (S5.4.5). Bodies are stored verbatim as received: five of these layouts are undecoded and
    // a file recorded today stays correct for every one of them.
    {"SR15NetComponentEvent", false, false, kStreamEvent},
    {"SR15NetTouchStateEvent", false, false, kStreamEvent},
    {"SR15NetTouchOwnershipGranted", false, false, kStreamEvent},
    // "SR15NetTouchOwnershipTransfer" is NOT a name in echovr.exe, and registering it lost
    // every ownership transfer from every capture taken so far. CMatSym::TypeId is total over
    // strings, so Listen() succeeded, ActiveCount() counted the tap, and it never fired -- B.14 names
    // that failure and says it looks exactly like a quiet match.
    //
    // ★ It is also why tests/capture checks each name against the image as name + NUL rather than as
    // a substring: the dead name occurs TWICE in echovr.exe, both times as a prefix of the two real
    // ones below, so a substring test reports it PRESENT. A check that would have passed the defect
    // it exists to catch is worth nothing, and that one nearly shipped.
    {"SR15NetTouchOwnershipTransferMailbox", false, false, kStreamEvent},
    {"SR15NetTouchOwnershipTransferReliable", false, false, kStreamEvent},
    {"SR15NetTouchOwnershipDeclined", false, false, kStreamEvent},
    {"SR15NetPhysicsOwnershipTransfer", false, false, kStreamEvent},
    {"SR15NetSetOwner", false, false, kStreamEvent},
    {"SR15NetReliablePostScoreUpdate", false, false, kStreamEvent},
    {"SR15StunMsg", false, false, kStreamEvent},
    {"SR15StunPlayerMsg", false, false, kStreamEvent},
    {"SR15RemoveStun", false, false, kStreamEvent},
    {"SR15NetPlayEmote", false, false, kStreamEvent},
    {"SR15NetPlayerPausedMsg", false, false, kStreamEvent},
    {"SR15NetReliableStatUpdate", false, false, kStreamEvent},
    {"SR15NetReliableTeamStatUpdate", false, false, kStreamEvent},
    {"SR15NetServerFrameMsg", false, false, kStreamEvent},

    // one-shot identity and session state (S5.4.1). Captured whole; the typed manifest fields are
    // filled from them where a parse is established, and the bytes stay either way.
    {"SNSLobbyOwnerState", true, false, kStreamEvent},
    {"SNSLobbyEntrantStatev2", true, false, kStreamEvent},
    {"SNSLobbyEntrantRemoved", true, false, kStreamEvent},
    {"SNSLobbyEntrantTeamChanged", true, false, kStreamEvent},
    {"SR15NetIdsResponse", true, false, kStreamEvent},
    {"SR15NetCurrentLoadoutResponse", true, false, kStreamEvent},
    {"SR15NetPlayerEquipmentUpdate", true, false, kStreamEvent},
    {"SR15NetLoadoutNumberNotification", true, false, kStreamEvent},
    {"SR15NetBalanceSettingsMassUpdate", true, false, kStreamEvent},
    {"SNSRefreshProfileFromServer", true, false, kStreamEvent},
    {"SR15NetServerInfo", true, false, kStreamEvent},
    {"SNSLobbyUpdatePings", false, false, kStreamEvent},

    // Found real in the image and absent from this table by the same audit. Each is a timeline event
    // the format could not carry at all, which is a different fault from carrying it badly.
    {"SR15NetUnreliableStatUpdate", false, false, kStreamEvent},
    {"SR15NetNewUnlocks", true, false, kStreamEvent},
    {"SR15NetCancelGoal", false, false, kStreamEvent},
    {"SNSLobbyEntrantDataChanged", false, false, kStreamEvent},
    {"SR15NetItemEquippedNotification", false, false, kStreamEvent},

    // this packet's own identity snapshot, which is not a wire message and is never tapped. It occupies
    // a type index so a reader can tell it apart from a recorded message body.
    {kIdentitySnapshotTapName, true, false, kStreamEvent},
};

}  // namespace

const TapType* TapTable() { return kTaps; }
uint16_t TapTypeCount() { return static_cast<uint16_t>(sizeof(kTaps) / sizeof(kTaps[0])); }

bool TapRowExcludedByDesign(const TapType& t) {
    return strcmp(t.structName, kIdentitySnapshotTapName) == 0;
}

// ---- the registration snapshot ---------------------------------------------------
// The tick publishes; anything may read. One word, so a reader cannot land between two fields of the
// same answer -- which is what the six plain loads this replaces allowed, and the identity the header
// sells would then have failed for a reason that was not a bookkeeping bug.
//
// Four dynamic counts of 10 bits each, then `started`, then a bit saying the tick has published at
// all. The three table-shaped numbers (total, excluded, tableRows) are NOT packed: they are functions
// of kTaps, identical in every process carrying this binary, so deriving them on read keeps the word
// small and keeps ONE definition of the table's shape.
namespace {

constexpr int kCountBits = 10;   // 1023, against a table bounded at kMaxTaps = 64 rows
constexpr uint64_t kCountMask = (1ull << kCountBits) - 1ull;
constexpr int kStartedBit = 4 * kCountBits;
constexpr int kPublishedBit = kStartedBit + 1;

std::atomic<uint64_t> g_tapRegPacked{0};

uint64_t PackCount(uint32_t v, int slot) {
    const uint32_t clamped = v > uint32_t(kCountMask) ? uint32_t(kCountMask) : v;
    return uint64_t(clamped) << (slot * kCountBits);
}

void FillTableShape(TapRegistration& r) {
    const uint16_t n = TapTypeCount();
    uint32_t excluded = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (TapRowExcludedByDesign(TapTable()[i])) {
            ++excluded;
        }
    }
    r.tableRows = n;
    r.excluded = excluded;
    r.total = n - excluded;
}

}  // namespace

void PublishTapRegistration(const TapRegistration& r) {
#if defined(W2A_MUTATE_NO_PUBLISH)
    // Mutation control: the publish is gone, so the accessor can only ever report the
    // never-asked default. The control's precondition -- that the snapshot carries a real answer
    // before the perturbation -- is what catches it.
    (void)r;
    return;
#endif
    const uint64_t packed = PackCount(r.registered, 0) | PackCount(r.deferred, 1) |
                            PackCount(r.givenUp, 2) | PackCount(r.unavailable, 3) |
                            (r.started ? 1ull << kStartedBit : 0ull) | (1ull << kPublishedBit);
    g_tapRegPacked.store(packed, std::memory_order_release);
}

TapRegistration TapRegistrationNow() {
    const uint64_t packed = g_tapRegPacked.load(std::memory_order_acquire);
    TapRegistration r{};
    FillTableShape(r);
    if ((packed & (1ull << kPublishedBit)) == 0) {
        // No tick has published: nothing has been asked of anything, which is not the same claim as
        // "asked and refused". Every registerable surface is still to be tried.
        r.deferred = r.total;
        return r;
    }
    r.registered = uint32_t(packed & kCountMask);
    r.deferred = uint32_t((packed >> kCountBits) & kCountMask);
    r.givenUp = uint32_t((packed >> (2 * kCountBits)) & kCountMask);
    r.unavailable = uint32_t((packed >> (3 * kCountBits)) & kCountMask);
    r.started = (packed & (1ull << kStartedBit)) != 0;
    return r;
}

uint16_t TapTypeIndex(const char* structName) {
    if (!structName) {
        return kTapTypeIndexNone;
    }
    for (uint16_t i = 0; i < TapTypeCount(); ++i) {
        if (strcmp(kTaps[i].structName, structName) == 0) {
            return i;
        }
    }
    return kTapTypeIndexNone;
}

// ---------------------------------------------------------------------------------------------

uint32_t ManifestBuilder::Append(const void* p, uint32_t n) {
    const uint32_t at = static_cast<uint32_t>(arena_.size());
    if (p && n) {
        arena_.insert(arena_.end(), static_cast<const uint8_t*>(p),
                      static_cast<const uint8_t*>(p) + n);
    }
    return at;
}

void ManifestBuilder::Begin(uint64_t t0Us, uint32_t captureRole, uint64_t authorityFlags) {
    view_ = Manifest{};
    players_.clear();
    arena_.clear();
    entrantOff_.clear();
    entrantLen_.clear();
    unlocksOff_.clear();
    unlocksLen_.clear();
    balanceLen_ = slot0Len_ = slot1Len_ = netIdLen_ = 0;
    view_.formatMajor = kFormatMajor;
    view_.formatMinor = kFormatMinor;
    view_.t0Us = t0Us;
    view_.captureRole = captureRole;
    view_.authorityFlags = authorityFlags;
    view_.keyframeIntervalUs = 2000000;
    view_.schemaCount = SchemaCount();
    view_.schemas = Schemas();
}

bool Md5HexToBytes(const char* hex, uint8_t out[16]) {
    memset(out, 0, 16);
    if (!hex) return false;
    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) {
        int nib[2];
        for (int j = 0; j < 2; ++j) {
            const char c = hex[i * 2 + j];
            if (c >= '0' && c <= '9') nib[j] = c - '0';
            else if (c >= 'a' && c <= 'f') nib[j] = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') nib[j] = c - 'A' + 10;
            else return false;
        }
        bytes[i] = uint8_t(nib[0] * 16 + nib[1]);
    }
    if (hex[32] != '\0') return false;   // 33 hex digits is not an md5 either
    memcpy(out, bytes, 16);
    return true;
}

uint64_t BuildStringHash64(const char* s) {
    if (!s) return 0;
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) {
        h ^= uint8_t(*s);
        h *= 1099511628211ull;
    }
    return h;
}

void ManifestBuilder::SetBuild(const uint8_t md5[16], uint64_t buildStringHash, uint64_t contentHash) {
    memcpy(view_.echovrMd5, md5, sizeof view_.echovrMd5);
    view_.buildStringHash = buildStringHash;
    view_.contentHash = contentHash;
}

void ManifestBuilder::SetLevel(uint64_t levelId, uint64_t gametypeId, uint64_t startTimeEpoch) {
    view_.levelId = levelId;
    view_.gametypeId = gametypeId;
    view_.startTimeEpoch = startTimeEpoch;
}

void ManifestBuilder::SetKeyframeIntervalUs(uint32_t us) { view_.keyframeIntervalUs = us; }
void ManifestBuilder::SetPartialStart(bool v) { view_.partialStart = v; }
void ManifestBuilder::SetFlags(uint32_t flags) { view_.flags = flags; }

void ManifestBuilder::SetUuid(uint64_t lo, uint64_t hi) {
    view_.demoUuidLo = lo;
    view_.demoUuidHi = hi;
}

void ManifestBuilder::AddPlayer(const PlayerRecord& p) {
    players_.push_back(p);
    entrantOff_.push_back(Append(p.entrantJson.p, p.entrantJson.n));
    entrantLen_.push_back(p.entrantJson.n);
    unlocksOff_.push_back(Append(p.unlocksJson.p, p.unlocksJson.n));
    unlocksLen_.push_back(p.unlocksJson.n);
}

void ManifestBuilder::SetBalanceBlob(const void* p, uint32_t n) {
    balanceOff_ = Append(p, n);
    balanceLen_ = n;
}
void ManifestBuilder::SetSlot0Baselines(const void* p, uint32_t n) {
    slot0Off_ = Append(p, n);
    slot0Len_ = n;
}
void ManifestBuilder::SetSlot1RuleVars(const void* p, uint32_t n) {
    slot1Off_ = Append(p, n);
    slot1Len_ = n;
}
void ManifestBuilder::SetNetIdSlotMap(const void* p, uint32_t n) {
    netIdOff_ = Append(p, n);
    netIdLen_ = n;
}

const char* ManifestBuilder::Refusal() const { return SchemaTableRefusal(); }

void ManifestBuilder::Rebind() {
    // The arena can only be repointed once it has stopped growing; every offset above was recorded
    // against the arena rather than against a pointer for exactly that reason.
    const uint8_t* base = arena_.empty() ? nullptr : arena_.data();
    for (size_t i = 0; i < players_.size(); ++i) {
        players_[i].entrantJson.p = entrantLen_[i] ? base + entrantOff_[i] : nullptr;
        players_[i].entrantJson.n = entrantLen_[i];
        players_[i].unlocksJson.p = unlocksLen_[i] ? base + unlocksOff_[i] : nullptr;
        players_[i].unlocksJson.n = unlocksLen_[i];
    }
    view_.playerCount = static_cast<uint16_t>(players_.size());
    view_.players = players_.empty() ? nullptr : players_.data();
    view_.balanceBlobZstd = Blob{balanceLen_ ? base + balanceOff_ : nullptr, balanceLen_};
    view_.slot0Baselines = Blob{slot0Len_ ? base + slot0Off_ : nullptr, slot0Len_};
    view_.slot1RuleVars = Blob{slot1Len_ ? base + slot1Off_ : nullptr, slot1Len_};
    view_.netIdSlotMap = Blob{netIdLen_ ? base + netIdOff_ : nullptr, netIdLen_};
    view_.schemaCount = SchemaCount();
    view_.schemas = Schemas();
}

const Manifest& ManifestBuilder::View() {
    Rebind();
    return view_;
}

}  // namespace capture
}  // namespace replay
