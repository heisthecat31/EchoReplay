#pragma once
// THE ROSTER, SOURCED LOCALLY.
//
// `players : 0` beside 6 997 stored player-sample rows (L2-RUN-3 F3) is not a sampler defect and it is
// not a truncation. `ManifestBuilder::AddPlayer` had ZERO non-test callers: the manifest roster was
// never fed by anything, on any launch, so every capture this project has ever taken names nobody.
// The keyframe's kKeyBlockRoster had a feeder -- events::LobbyTracker -- but it derives entrants from
// the lobby bodies on S_EVENT, and a solo `-offline` host receives no lobby traffic at all
// (`netvarRecords 0`, `rowsSeen 0`: the deriver was never fed). Both roster surfaces were therefore
// keyed, directly or indirectly, on WIRE EVENTS THAT A SOLO HOST NEVER SEES.
//
// That is ARCH-21's lesson in its second instance: state the client HOLDS is sourced from the client,
// per tick or on change, and never inferred from traffic that a legal session may not carry. The scan
// below walks the engine's own entrant count and asks the identity tables what they hold, so a
// single-player host produces exactly one row -- the owner's own pawn -- rather than none.
//
// ★ WHY THE ENGINE READS SIT BEHIND IRosterSource. The composition -- which indices are present, which
// fields were sourced, when a row was first and last seen, whether an index is a new person or the one
// already in the table -- is the part that can be wrong, so it is the part a test has to exercise. If
// the scan called identity_access directly it would need a live CR15NetGame to run at all, and the
// only testable thing left would be the table. The seam is the primitives; the composition is ours.

#include <stdint.h>
#include <string.h>

#include "foundation/demofile/demotypes.h"
#include "generated/facts_generated.h"

namespace replay {

// Declared, not included: the seam carries the engine's refusal word as the enum it is, and
// engine/identity_access.h stays out of every file that only composes rosters.
namespace identity_access {
enum class Refusal : uint8_t;
}

namespace capture {

// The engine's widths, not Appendix B.0's. B.0's PlayerRecord declares displayName and platformStr as
// char[32] and teamId as uint8_t; the engine field is 0x24 bytes and the team word's 0xFFFF means NO
// TEAM. Narrowing here would truncate a legal 36-character name and turn "no team" into team 255, so
// the row carries all three at full width and PlayerRecord's v1 fields take them unnarrowed.
constexpr size_t   kRosterNameCap  = 36;
constexpr uint16_t kRosterNoTeam = 0xFFFFu;   // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
constexpr uint16_t kRosterNoAlign = 0xFFFFu;  // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
constexpr uint16_t kRosterNoSlot = 0xFFFFu;   // facts-ok: LOBBY.TEAM_NONE PC_PRESENT, so gen_facts emits no value
constexpr uint16_t kRosterMaxRows  = 64;    // Echo Arena is 5v5 plus spectators; 64 is not a limit met
// The widest band a scan will walk. This is a BACKSTOP on the engine's own count, never a substitute
// for it: EntrantBand refuses by name when the count is unreadable and the scan then walks nothing.
constexpr uint16_t kRosterBandCap  = 64;

static_assert(kRosterNameCap == sizeof(PlayerRecord::displayNameFull),
              "the roster row and the v1 record must hold the same name width");

// The demo file's cap, imported rather than restated: this is the size a capture asks the engine for,
// so a document that would not fit the file is refused at the source instead of being cut here.
constexpr size_t kRosterJsonCap = kPlayerJsonCap;

// ★ THE ENGINE'S OWN "IS THIS ENTRANT SEATED ON A TEAM" TEST, imported rather than invented.
//
// The 3-bit team field of the SR15NetUser packed word is not a team on its own. Every engine path
// that reads it replaces it with 0xFFFF when it is not below NETGAME.USER_ROLE_BOUND -- the
// [NETGAME] dump's `Team ID` line (sub_1401CD1A0, `v17 = -1; if (((word >> 10) & 7u) < 5) v17 =
// ...`), the spectator gate CAM.SPECTATOR_TEAM_GATE, and 112 of the 164 sites in echovr.exe that
// extract this field. SR15NetUser's own reset (sub_140176420) writes 0xFFFF over the whole packed
// word, so an entrant that has joined no side carries 7 here and the engine calls it NO TEAM.
//
// Without this the capture wrote a raw 0..7 unconditionally, and an unjoined entrant -- team 7,
// masked to 3 bits and never checked -- could only be recorded as a number. It is the difference
// between "no team" and blue.
inline bool RosterTeamSeated(uint8_t rawTeam) {
#if defined(W2A4_MUTATE_TEAM_ALWAYS_TRUE)
    (void)rawTeam;
    return true;
#elif defined(W2A4_MUTATE_CRITERION_INVERTED)
    return rawTeam >= uint8_t(facts::NETGAME_USER_ROLE_BOUND);
#else
    return rawTeam < uint8_t(facts::NETGAME_USER_ROLE_BOUND);
#endif
}

// The capture's whole team decision, off the two raw fields the engine's accessors return, in one
// place so the live source and the tests that drive it cannot drift. A seat the engine does not
// recognise has no slot on it either: recording slot 31 of no team would be a second false fact.
inline void RosterTeamFromEngine(uint8_t rawTeam, uint8_t rawTeamSlot, uint16_t& team,
                                 uint16_t& teamSlot) {
    if (!RosterTeamSeated(rawTeam)) {
        team = kRosterNoTeam;
        teamSlot = kRosterNoTeam;
        return;
    }
    team = rawTeam;
    teamSlot = rawTeamSlot;
}

// A MASK, NOT A BOOL, and for the reason AsSeenSample::valid has one: a field whose bit is CLEAR is
// zero because nothing sourced it, not because it measured zero. A roster that cannot tell those apart
// puts every player on team 0 and renders a plausible, wrong replay in silence (S12.0).
enum : uint32_t {
    kRosterFillSlot     = 1u << 0,
    kRosterFillName     = 1u << 1,
    kRosterFillPlatform = 1u << 2,
    kRosterFillXplat    = 1u << 3,
    kRosterFillTeam     = 1u << 4,
    kRosterFillAlign    = 1u << 5,
    kRosterFillLoadout  = 1u << 6,   // the 20 item ids
    kRosterFillGen      = 1u << 7,
    kRosterFillProfile  = 1u << 8,
    kRosterFillLocal    = 1u << 9,
    // CAPTURE-SIDE ONLY, and deliberately the first bit past kPlayerFillAll. The two loadout numbers
    // now have a reader (identity_access::LoadoutNumbers) but PlayerRecord's fill mask is frozen at ten
    // bits, so the file cannot say whether its loadoutNumber pair was sourced or defaulted. Until W1-B
    // spends bit 10, a refused pair is written as 0 and counted in RosterScanReport::refusals.
    kRosterFillNumbers  = 1u << 10,
    // The twenty slot KEYS, and a bit of their own: "recorded and empty" and "never recorded" are
    // different states. The engine holds (key, item) per slot and a recording carrying only the item
    // leaves POSITION as the whole of its slot evidence -- a guess that dresses a pawn in the wrong
    // things and looks correct (ARCH-23 s14.8b C1).
    kRosterFillLoadoutKeys = 1u << 11,
    // How many customisation entries the engine's component HOLDS, which is not how many the format
    // records. Its own bit for the AUD-W2-C-2 F2 reason: a source that cannot ask leaves it clear,
    // and the recording then says "unknown" rather than "twenty, complete".
    kRosterFillLoadoutAvail = 1u << 12,
    // RECORDER-3 F1/F4, and capture-side for the same reason as bits 10-12: WHICH of the two
    // storages answered for the document, and whether the walk had to write `null` over a
    // non-integer number. "Re-serialised, one value dropped" and "verbatim" are different files.
    kRosterFillProfileFromEntrantRec = 1u << 13,
    kRosterFillProfileLossyNumber    = 1u << 14,
};

// The bits above kPlayerFillAll, as one word. A file that carries the v2 player extension carries
// these; one that does not says nothing about them, which is not the same as saying they are clear.
constexpr uint32_t kRosterFillExtAll =
    kRosterFillNumbers | kRosterFillLoadoutKeys | kRosterFillLoadoutAvail |
    kRosterFillProfileFromEntrantRec | kRosterFillProfileLossyNumber;

// Which storage the entrant's profile document came out of, and what the walk had to give up to
// serialise it. RECORDER-3 F1/F4.
enum class ProfileSource : uint8_t {
    None = 0,
    UserTable = 1,       // CR15NetGame's user array -- the profile service's reply
    EntrantRecord = 2,   // the lobby member record -- the document the lobby message carried
};
struct EntrantJsonOutcome {
    ProfileSource source = ProfileSource::None;
    bool lossyNumber = false;
};

// The engine spells a cross-platform id more than one way and the recorder accepted only decimal
// digits, so an `OVR-ORG|0012345678abcdef`-shaped id was refused as unparsable (RECORDER-3 F3). The tag
// before the last separator names the platform, not the number, and the number after it is hex
// when a letter appears in it or a '|' introduced it.
//
// A PARTIAL PARSE IS STILL A REFUSAL -- a truncated id is a different person and the roster dedups
// on it -- and so is an id wider than 64 bits.
inline bool ParseXPlatformId(const char* s, uint64_t& out) {
    if (!s || !*s) return false;
    const char* body = s;
    bool hex = false;
    if (const char* bar = strrchr(s, '|')) {
        body = bar + 1;
        hex = true;
    } else if (const char* dash = strrchr(s, '-')) {
        body = dash + 1;
    }
    if (!*body) return false;
    for (const char* c = body; *c && !hex; ++c) {
        if (*c < '0' || *c > '9') hex = true;
    }
    const uint64_t base = hex ? 16u : 10u;
    uint64_t v = 0;
    for (const char* c = body; *c; ++c) {
        uint64_t d;
        if (*c >= '0' && *c <= '9') d = uint64_t(*c - '0');
        else if (hex && *c >= 'a' && *c <= 'f') d = uint64_t(*c - 'a') + 10u;
        else if (hex && *c >= 'A' && *c <= 'F') d = uint64_t(*c - 'A') + 10u;
        else return false;
        if (v > (UINT64_MAX - d) / base) return false;
        v = v * base + d;
    }
    out = v;
    return true;
}
static_assert(kRosterFillSlot == kPlayerFillSlot && kRosterFillName == kPlayerFillName &&
                  kRosterFillPlatform == kPlayerFillPlatform && kRosterFillXplat == kPlayerFillXplat &&
                  kRosterFillTeam == kPlayerFillTeam && kRosterFillAlign == kPlayerFillAlign &&
                  kRosterFillLoadout == kPlayerFillLoadout && kRosterFillGen == kPlayerFillGen &&
                  kRosterFillProfile == kPlayerFillProfile && kRosterFillLocal == kPlayerFillLocal,
              "the capture's fill bits and the file's on-disk numbering must be the same bits");
static_assert(kRosterFillNumbers == kPlayerFillAll + 1u,
              "kRosterFillNumbers must be the first bit the file's v1 record carries");
static_assert(uint32_t(kRosterFillNumbers) == uint32_t(kPlayerFillNumbers) &&
                  uint32_t(kRosterFillLoadoutKeys) == uint32_t(kPlayerFillLoadoutKeys) &&
                  uint32_t(kRosterFillLoadoutAvail) == uint32_t(kPlayerFillLoadoutAvail) &&
                  uint32_t(kRosterFillProfileFromEntrantRec) ==
                      uint32_t(kPlayerFillProfileFromEntrantRec) &&
                  uint32_t(kRosterFillProfileLossyNumber) ==
                      uint32_t(kPlayerFillProfileLossyNumber) &&
                  kRosterFillExtAll == kPlayerFillExtAll,
              "the capture's extension bits and the v2 record's numbering must be the same bits");
static_assert(kRosterFillLoadoutKeys == kRosterFillNumbers << 1,
              "the capture-side bits are consecutive above the ten the file carries");
static_assert(kRosterFillLoadoutAvail == kRosterFillLoadoutKeys << 1,
              "the capture-side bits are consecutive above the ten the file carries");

// WHY A FIELD IS MISSING, counted per scan and carried into the stop sentinel as `rosterRefusals`.
// A two-state fill mask says a field is absent; it cannot say whether the engine had no netgame, named
// no such entrant, or held an empty profile. Those are different repairs, so they are different words.
enum class RosterRefusal : uint8_t {
    Band = 0,
    Name,
    Platform,
    Xplat,
    Team,
    Alignment,
    LoadoutItems,
    LoadoutKeys,
    LoadoutNumbers,
    Generation,
    Profile,
    EntrantJson,
    UnlocksJson,
    Count,
};
constexpr size_t kRosterRefusalCount = size_t(RosterRefusal::Count);

inline const char* RosterRefusalName(RosterRefusal r) {
    switch (r) {
        case RosterRefusal::Band:           return "band";
        case RosterRefusal::Name:           return "name";
        case RosterRefusal::Platform:       return "platform";
        case RosterRefusal::Xplat:          return "xplat";
        case RosterRefusal::Team:           return "team";
        case RosterRefusal::Alignment:      return "alignment";
        case RosterRefusal::LoadoutItems:   return "loadoutItems";
        case RosterRefusal::LoadoutKeys:    return "loadoutKeys";
        case RosterRefusal::LoadoutNumbers: return "loadoutNumbers";
        case RosterRefusal::Generation:     return "generation";
        case RosterRefusal::Profile:        return "profile";
        case RosterRefusal::EntrantJson:    return "entrantJson";
        case RosterRefusal::UnlocksJson:    return "unlocksJson";
        case RosterRefusal::Count:          break;
    }
    return "unknown";
}

struct RosterRefusals {
    uint32_t byReason[kRosterRefusalCount];
    // The engine's own word for the most recent refusal of each reason, so the sentinel can say
    // `band: NoNetGame` rather than `band: 1`. Static storage owned by identity_access::RefusalText.
    const char* wordByReason[kRosterRefusalCount];

    void Note(RosterRefusal r, const char* why) {
        const size_t i = size_t(r);
        ++byReason[i];
        if (why) wordByReason[i] = why;
    }
    uint32_t Total() const {
        uint32_t n = 0;
        for (size_t i = 0; i < kRosterRefusalCount; ++i) n += byReason[i];
        return n;
    }
};

struct RosterRow {
    uint64_t xplatformId;
    uint64_t firstSeenUs, lastSeenUs;
    uint64_t loadoutSlots[20];      // Symbol64 item ids; loadoutSlotKeys[i] is the slot each sits in
    uint64_t loadoutSlotKeys[20];   // CustSlotEntry.key -- the engine's own key, never a position
    uint64_t fingerprint;           // what the last full read of this row saw; 0 until one has run
    uint32_t fill;
    uint32_t teamChanges;           // how many times this row's team moved after it was first filled
    uint16_t loadoutSlotsAvailable; // entries the engine's component held; only a fact with the bit
    uint16_t userIndex, userSlot, generation;
    uint16_t team, teamSlot, alignment;
    uint16_t loadoutNumber, loadoutNumberBody;
    uint8_t  active;                // seen by the most recent scan
    uint8_t  local;                 // this machine authors this player (host: the human plus bots)
    uint8_t  hasProfileJson;
    uint8_t  haveFingerprint;
    char     displayName[kRosterNameCap];
    char     platformStr[kRosterNameCap];
};

// What one scan did, so a zero roster is never read as "the match was empty". A band that was walked
// and yielded nothing (`bandWalked > 0, present 0`) is a different claim from a band that could not be
// walked at all (`bandRefused`), and both are different from a full table.
struct RosterScanReport {
    uint16_t bandWalked;
    uint16_t bandClamped;   // entrants the engine counted past kRosterMaxRows, so past our table
    uint16_t present;       // indices the engine said exist
    uint16_t appended;      // rows this scan added
    uint16_t updated;       // rows this scan re-read in full
    uint16_t unchanged;     // rows whose fingerprint matched, so no field was re-read
    uint16_t departed;      // rows that were active and are not present any more
    uint16_t teamChanged;
    uint16_t tableFull;     // present indices refused because the table is at kRosterMaxRows
    uint8_t  signalUnchanged;  // the per-tick gate answered "nothing moved" and NO scan ran
    uint8_t  sourceAbsent;  // no source object at all -- the scan did not happen
    uint8_t  bandRefused;   // the engine's entrant count could not be read -- NO horizon is substituted
    const char* bandWhy;    // the engine's word for it, never a number
    RosterRefusals refusals;
};

// The primitives, one per engine table. Every one returns false for "this build could not source it",
// which is what makes the fill mask honest; none of them may report a plausible default. Each hands
// back the engine's own refusal word through `why` so the composition can name a reason it cannot see.
//
// `out` is written only when the call returns true.
class IRosterSource {
public:
    virtual ~IRosterSource() {}

    // HOW MANY ENTRANTS THE ENGINE SAYS THERE ARE. Not a horizon of ours: a scan that invents its own
    // bound reports our number as the engine's, and a build where the count moved would keep passing.
    // Refusing costs one capture's roster; guessing 64 costs the reader's ability to trust any of them.
    virtual bool EntrantBand(uint16_t& band, const char*& why) const = 0;

    // The cheap per-entrant summary that decides whether the row is re-read at all: name, team, team
    // slot, user slot and loadout, which is every field that moves during a match. `false` means the
    // summary could not be taken, and the row is then re-read in full rather than assumed unchanged.
    virtual bool Fingerprint(uint16_t userIndex, uint64_t& out) const = 0;

    // THE PRESENCE TEST, and it is the engine's answer rather than ours: an index with no user slot
    // is not a person. Everything else is optional; this one decides whether a row exists at all.
    virtual bool SlotOf(uint16_t userIndex, uint16_t& slot) const = 0;
    virtual bool DisplayName(uint16_t userIndex, char* out, size_t cap) const = 0;
    virtual bool PlatformString(uint16_t userIndex, char* out, size_t cap) const = 0;
    virtual bool XPlatformId(uint16_t userIndex, uint64_t& out) const = 0;
    virtual bool Team(uint16_t userIndex, uint16_t& team, uint16_t& teamSlot) const = 0;
    virtual bool Alignment(uint16_t userIndex, uint16_t& out) const = 0;
    virtual bool LoadoutItems(uint16_t userIndex, uint64_t items[20]) const = 0;
    // The slot KEY beside each item.  Non-pure and refusing by default because every source that
    // predates it can only answer with positions, and a positional answer here is exactly the
    // silent wrong-slot dressing this call exists to remove: a source that cannot say leaves
    // kRosterFillLoadoutKeys clear and the recording says so.
    //
    // ★ ONE WALK. `held` is the entries the component ACTUALLY holds, unclamped, and it comes back
    // from the SAME resolution as the keys and items -- AUD-W2-A-2 F9. Asking a second call for it
    // resolved the component twice per entrant per scan and threw the second walk's twenty pairs
    // away, doubling the term roster.h:333-341 and costmeter.cpp:225-232 record as having closed two
    // captures. `heldKnown` keeps the three states: a source that walked the component but cannot
    // report its width leaves it false, and false is UNKNOWN rather than "not truncated".
    virtual bool LoadoutPairs(uint16_t /*userIndex*/, uint64_t /*keys*/[20],
                              uint64_t /*items*/[20], uint64_t& /*held*/,
                              bool& /*heldKnown*/) const {
        return false;
    }
    // Keyed on the USER SLOT, because the live loadout header is indexed by slot and not by entrant
    // index, and it refuses independently of the items: a header this build cannot span-check must not
    // take the item ids down with it.
    virtual bool LoadoutNumbers(uint16_t userSlot, uint16_t& number, uint16_t& numberBody,
                                const char*& why) const = 0;
    virtual bool Generation(uint16_t userSlot, uint16_t& out) const = 0;
    virtual bool HasProfileJson(uint16_t userIndex) const = 0;
    virtual bool IsLocal(uint16_t userIndex) const = 0;

    // THE PROFILE DOCUMENTS. `written` is the length of the WHOLE document on every path and `out` is
    // untouched unless the whole document fitted -- identity_access::CopyJsonBytes's contract, kept
    // here unchanged, because a JSON document cut at a byte boundary parses as nothing and would reach
    // the demo file looking like a capture.
    //
    // `refusal` is the engine's word as the ENUM, beside `why` as its text: the identity emitter's
    // retry policy is decided on it, and a policy keyed on prose breaks the day a sentence is edited.
    // A source that refuses for a reason the engine never named leaves it at Refusal::None.
    //
    // `outcome`, when the caller asks for it, says WHICH storage answered and whether the walk had
    // to drop a value (RECORDER-3 F1/F4). A source that cannot tell leaves it at its default.
    virtual bool EntrantJson(uint16_t userIndex, void* out, size_t cap, size_t& written,
                             bool& truncated, const char*& why,
                             identity_access::Refusal& refusal,
                             EntrantJsonOutcome* outcome = nullptr) const = 0;
    // A cheap token that moves when the entrant's profile document is REPLACED, readable without
    // serialising anything -- a refused document has no bytes to hash. 0 means the source cannot say.
    virtual uint64_t ProfileDocToken(uint16_t /*userIndex*/) const { return 0; }
    virtual bool UnlocksJson(uint16_t userIndex, void* out, size_t cap, size_t& written,
                             bool& truncated, const char*& why) const = 0;
};

// Append-only, deduplicated by USER ID. A row is never rewritten by a different person: the key is the
// cross-platform id when the engine gave one, and the user slot otherwise, so a player who leaves and
// rejoins into the same slot with the same id updates their row rather than doubling it, while a
// DIFFERENT id arriving in a vacated slot gets its own row.
//
// ★ The slot fallback is deliberate and it is the narrower key. Two players cannot hold one slot at
// once, so it never merges two live people; what it cannot do is recognise a rejoin after somebody
// else has used the slot, which appends a second row for one person. An extra row is a recoverable
// reading; a merged row is a fabricated one.
class RosterTable {
public:
    uint16_t Count() const { return count_; }
    const RosterRow& At(uint16_t i) const { return rows_[i]; }
    RosterRow& Mutable(uint16_t i) { return rows_[i]; }

    // Null when the table is full. `appended` says which of the two happened.
    RosterRow* FindOrAppend(uint64_t xplatformId, uint16_t userSlot, uint64_t tUs, bool& appended) {
        appended = false;
        for (uint16_t i = 0; i < count_; ++i) {
            if (SameEntrant(rows_[i], xplatformId, userSlot)) {
                return &rows_[i];
            }
        }
        if (count_ >= kRosterMaxRows) {
            return nullptr;
        }
        RosterRow& r = rows_[count_++];
        memset(&r, 0, sizeof r);
        r.xplatformId = xplatformId;
        r.userSlot = userSlot;
        r.team = kRosterNoTeam;
        r.teamSlot = kRosterNoTeam;
        r.alignment = kRosterNoAlign;
        r.firstSeenUs = tUs;
        r.lastSeenUs = tUs;
        appended = true;
        return &r;
    }

    void Clear() { count_ = 0; }

private:
    static bool SameEntrant(const RosterRow& r, uint64_t xplatformId, uint16_t userSlot) {
        if (xplatformId != 0 && r.xplatformId != 0) {
            return r.xplatformId == xplatformId;
        }
        // One side has no id. Falling back to the slot is right; falling back to "equal because both
        // are zero" would merge every unidentified player into one row.
        return xplatformId == 0 && r.xplatformId == 0 && r.userSlot == userSlot;
    }

    RosterRow rows_[kRosterMaxRows];
    uint16_t count_ = 0;
};

// FNV-1a over whatever a source chooses to summarise. Shared so the fake in the tests and the engine
// source fold the same bytes the same way, and so a caller cannot accidentally hash a struct's padding.
// Eight bytes at a time where it can be: the loadout alone is 160 bytes, and a byte-wise fold over it
// was the whole of this scan's per-tick cost. The construction is unchanged -- xor then multiply by the
// FNV prime -- so every input byte still reaches the digest.
inline uint64_t RosterFold(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    size_t i = 0;
    for (; i + sizeof(uint64_t) <= n; i += sizeof(uint64_t)) {
        uint64_t word = 0;
        memcpy(&word, b + i, sizeof word);
        h = (h ^ word) * 1099511628211ull;
    }
    for (; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
constexpr uint64_t kRosterFoldSeed = 14695981039346656037ull;

// THE COMPONENT'S CONTRIBUTION TO A ROW'S FINGERPRINT, in one definition because it is one fact.
// The twenty item ids are a CLAMPED view -- `min(count, 20)` -- so a component that grows from
// twenty entries to twenty-one moves none of them, the row is never re-read, and the recording goes
// on saying "not truncated". The unclamped width is what makes that growth visible,
// and it is folded HERE rather than in each source so a source cannot fold one of the two and a test
// cannot certify its own copy of the rule. `heldKnown` false folds NOTHING: a build that cannot ask
// must not fold a zero, which would be a made-up number every row shares.
inline uint64_t RosterFoldLoadout(uint64_t h, const uint64_t items[20], uint64_t held,
                                  bool heldKnown) {
    h = RosterFold(h, items, sizeof(uint64_t) * 20);
#ifndef W2A2_MUTATE_FINGERPRINT_IGNORES_HELD
    if (heldKnown) {
        h = RosterFold(h, &held, sizeof held);
    }
#endif
    return h;
}

// F1 (L2-RUN-5): THE PER-TICK GATE, and the reason ScanRoster is not called per tick any more.
//
// The fingerprint below skips the nine field reads of an unchanged row, but it does not skip the
// reads that DECIDE that: the fingerprint itself is a name read, a team pair, twenty applied item
// ids and the loadout numbers, and the dedup key on top of it walks the entrant's profile document
// on every scan by design. In the field that composition cost 1247-1371 us of the capture's tick on
// every one of 1489 ticks, `gather` went 7.5x, and the cost meter killed the capture at 50 s.
//
// So the tick asks the ONE question the engine answers out of its own count -- EntrantBand, the same
// call ScanRoster opens with -- and runs the full scan only when that answer MOVES, or when the
// cadence expires. Two engine loads on a quiet tick instead of nine per user.
//
// The cadence is not decoration. A team change keeps the entrant count, so a signal-only gate would
// hold a stale team for the rest of the match; `intervalUs` is the bound on how stale a row can get,
// and it is passed in rather than baked in so the bound is the caller's declared number.
struct RosterCadence {
    uint64_t lastScanUs;
    uint16_t lastBand;
    uint8_t  haveSignal;
    uint8_t  bandRefused;
};

// The roster is re-read at this cadence even when the entrant count has not moved. One second is
// 90 ticks of a scan that costs ~1.2 ms in the field: a median of zero over any window the meter
// measures, and a worst case a reader can still name.
constexpr uint64_t kRosterRescanIntervalUs = 1000000;

// Returns true when a full scan ran and `out` reports it; false when the gate refused and `out`
// carries only `signalUnchanged` (or `sourceAbsent`). `force` runs the scan unconditionally and is
// how the open-time scan arms the cadence, so the first tick after an open does not scan twice.
inline RosterScanReport ScanRoster(const IRosterSource* src, RosterTable& table, uint64_t tUs);

inline bool ScanRosterOnChange(const IRosterSource* src, RosterTable& table, RosterCadence& cad,
                               uint64_t tUs, uint64_t intervalUs, bool force,
                               RosterScanReport& out) {
    out = RosterScanReport{};
    if (!src) {
        out.sourceAbsent = 1;
        return false;
    }

    uint16_t band = 0;
    const char* why = nullptr;
    const bool haveBand = src->EntrantBand(band, why);
    const uint8_t refused = haveBand ? 0u : 1u;

    // A count that cannot be read is an EDGE, not a reason to hold: the engine's answer changed from
    // a number to a refusal, which is exactly the transition a roster has to notice.
    const bool moved = !cad.haveSignal || refused != cad.bandRefused ||
                       (haveBand && band != cad.lastBand);
    const bool due = !cad.haveSignal || tUs < cad.lastScanUs ||
                     tUs - cad.lastScanUs >= intervalUs;
    if (!force && !moved && !due) {
        out.signalUnchanged = 1;
        return false;
    }

    out = ScanRoster(src, table, tUs);
    cad.haveSignal = 1;
    cad.bandRefused = refused;
    cad.lastBand = band;
    cad.lastScanUs = tUs;
    return true;
}

// One pass over the engine's entrant band. Idempotent: running it twice with an unchanged engine
// appends nothing the second time, which is what makes it safe to call per tick.
//
// ★ THE FINGERPRINT IS WHY THIS IS AFFORDABLE PER TICK. It summarises name, team, team slot, user slot
// and loadout -- every field that moves during a match -- and when it matches, the nine reads behind it
// are skipped: the two strings, the team pair, the alignment, the item ids, the loadout numbers, the
// generation, the profile probe and the local-authorship walk. Those are safe to skip only because none
// of them can move while the fingerprint does not: platform string and local authorship are fixed for
// an entrant's session, and alignment and generation are functions of the team and slot the fingerprint
// already covers. A field that CAN move independently must be added to the fingerprint, not exempted.
// Presence, the user id and the clock are read every scan regardless, because they answer WHICH row.
inline RosterScanReport ScanRoster(const IRosterSource* src, RosterTable& table, uint64_t tUs) {
    RosterScanReport rep = {};
    if (!src) {
        rep.sourceAbsent = 1;
        return rep;
    }

    uint16_t band = 0;
    const char* bandWhy = nullptr;
    if (!src->EntrantBand(band, bandWhy)) {
        rep.bandRefused = 1;
        rep.bandWhy = bandWhy ? bandWhy : "unnamed";
        rep.refusals.Note(RosterRefusal::Band, rep.bandWhy);
        return rep;
    }
    if (band > kRosterBandCap) {
        rep.bandClamped = uint16_t(band - kRosterBandCap);
        band = kRosterBandCap;
    }
    rep.bandWalked = band;

    bool present[kRosterMaxRows] = {};

    for (uint16_t idx = 0; idx < band; ++idx) {
        uint16_t slot = kRosterNoSlot;
        if (!src->SlotOf(idx, slot) || slot == kRosterNoSlot) {
            continue;
        }
        ++rep.present;

        uint64_t fp = 0;
        const bool haveFp = src->Fingerprint(idx, fp);

        // THE DEDUP KEY IS ASKED FOR ON EVERY SCAN and is never gated by the fingerprint. Two people
        // can hold one slot in turn, and a fingerprint is a summary rather than an identity: matching
        // one to decide WHICH ROW an index belongs to would merge a rejoin into whoever left, which is
        // the fabricated row this table exists to avoid. The fingerprint decides what to re-read, not
        // who is being read.
        uint64_t xplat = 0;
        const bool haveXplat = src->XPlatformId(idx, xplat);

        bool appended = false;
        RosterRow* r = table.FindOrAppend(haveXplat ? xplat : 0, slot, tUs, appended);
        if (!r) {
            ++rep.tableFull;
            continue;
        }

        const size_t rowIndex = size_t(r - &table.Mutable(0));
        if (rowIndex < kRosterMaxRows) {
            present[rowIndex] = true;
        }
        r->lastSeenUs = tUs;
        r->active = 1;
        r->userIndex = idx;
        r->userSlot = slot;
        r->fill |= kRosterFillSlot;

        if (!appended && haveFp && r->haveFingerprint && r->fingerprint == fp) {
            ++rep.unchanged;
            continue;
        }
        if (appended) {
            ++rep.appended;
        } else {
            ++rep.updated;
        }
        r->fingerprint = fp;
        r->haveFingerprint = haveFp ? 1u : 0u;

        if (haveXplat) {
            r->xplatformId = xplat;
            r->fill |= kRosterFillXplat;
        } else {
            rep.refusals.Note(RosterRefusal::Xplat,
                              "the entrant profile carried no parsable xplatformid");
        }

        char buf[kRosterNameCap];
        if (src->DisplayName(idx, buf, sizeof buf)) {
            memcpy(r->displayName, buf, sizeof buf);
            r->fill |= kRosterFillName;
        } else {
            rep.refusals.Note(RosterRefusal::Name,
                              "the engine resolved no display name for this user index");
        }
        if (src->PlatformString(idx, buf, sizeof buf)) {
            memcpy(r->platformStr, buf, sizeof buf);
            r->fill |= kRosterFillPlatform;
        } else {
            rep.refusals.Note(RosterRefusal::Platform,
                              "the entrant profile carried no platform string");
        }

        uint16_t team = kRosterNoTeam, teamSlot = kRosterNoTeam;
        if (src->Team(idx, team, teamSlot)) {
            // A TEAM CHANGE IS THE POINT OF SCANNING PER TICK. It is counted rather than only stored,
            // because a row that silently takes its newest value cannot say a change ever happened and
            // the keyframe roster would be the only witness.
            if ((r->fill & kRosterFillTeam) != 0 && (r->team != team || r->teamSlot != teamSlot)) {
                ++r->teamChanges;
                ++rep.teamChanged;
            }
            r->team = team;
            r->teamSlot = teamSlot;
            r->fill |= kRosterFillTeam;
        } else {
            rep.refusals.Note(RosterRefusal::Team,
                              "the engine source refused this user index's team");
        }

        uint16_t align = kRosterNoAlign;
        if (src->Alignment(idx, align)) {
            r->alignment = align;
            r->fill |= kRosterFillAlign;
        } else {
            rep.refusals.Note(RosterRefusal::Alignment,
                              "the engine source refused this user index's alignment");
        }

        // ONE read for both halves where the source has a pair reader, so a row's key and its item
        // can never come from two separate walks of a table that moved in between.
        uint64_t items[20] = {};
        uint64_t keys[20] = {};
        uint64_t available = 0;
        bool availableKnown = false;
        // ONE call, three answers: the keys, the items and the component's true width. Two calls
        // walked the component twice for the same row.
#ifdef W2A2_MUTATE_SINGLE_WALK_BROKEN
        {
            uint64_t discard[20] = {}, discardHeld = 0;
            bool discardKnown = false;
            src->LoadoutPairs(idx, discard, discard, discardHeld, discardKnown);
        }
#endif
        if (src->LoadoutPairs(idx, keys, items, available, availableKnown)) {
            memcpy(r->loadoutSlots, items, sizeof items);
            memcpy(r->loadoutSlotKeys, keys, sizeof keys);
            r->fill |= kRosterFillLoadout | kRosterFillLoadoutKeys;
            if (availableKnown) {
                // Saturating rather than wrapping, for the reason EntrantBand's count already is: a
                // component past what this field holds is not one the row can state, and a wrapped
                // count would read as a small, complete loadout -- the exact claim F2 is about.
                constexpr uint64_t kAvailCap = uint64_t(uint16_t(~uint16_t(0)));
                r->loadoutSlotsAvailable =
                    available > kAvailCap ? uint16_t(kAvailCap) : uint16_t(available);
                r->fill |= kRosterFillLoadoutAvail;
            }
        } else if (src->LoadoutItems(idx, items)) {
            memcpy(r->loadoutSlots, items, sizeof items);
            r->fill |= kRosterFillLoadout;
#ifdef W2C2_MUTATE_FILL_BIT_ALWAYS_SET
            r->fill |= kRosterFillLoadoutKeys;
#endif
            rep.refusals.Note(RosterRefusal::LoadoutKeys,
                              "the engine source answered items only, so the slots are positional");
        } else {
            rep.refusals.Note(RosterRefusal::LoadoutItems,
                              "the engine source refused this user index's loadout items");
        }

        uint16_t number = 0, numberBody = 0;
        const char* numWhy = nullptr;
        if (src->LoadoutNumbers(slot, number, numberBody, numWhy)) {
            r->loadoutNumber = number;
            r->loadoutNumberBody = numberBody;
            r->fill |= kRosterFillNumbers;
        } else {
            rep.refusals.Note(RosterRefusal::LoadoutNumbers, numWhy);
        }

        uint16_t gen = 0;
        if (src->Generation(slot, gen)) {
            r->generation = gen;
            r->fill |= kRosterFillGen;
        } else {
            rep.refusals.Note(RosterRefusal::Generation,
                              "the engine source refused this user index's generation");
        }

        r->hasProfileJson = src->HasProfileJson(idx) ? 1u : 0u;
        if (r->hasProfileJson) {
            r->fill |= kRosterFillProfile;
        } else {
            rep.refusals.Note(RosterRefusal::Profile,
                              "the engine source refused this user index's profile document");
        }
        if (src->IsLocal(idx)) {
            r->local = 1;
            r->fill |= kRosterFillLocal;
        }
    }

    // A row that WAS active and is not present any more has left. The row stays -- a replay has to name
    // the player who left at 2:10 -- and `lastSeenUs` is left where the last scan that saw them put it,
    // so the file says when rather than only that.
    for (uint16_t i = 0; i < table.Count(); ++i) {
        RosterRow& r = table.Mutable(i);
        if (r.active && !present[i]) {
            r.active = 0;
            ++rep.departed;
        }
    }
    return rep;
}

// ---------------------------------------------------------------------------------------------
// THE ONE RULE BINDING A ROW'S TEAM TO ITS TEAM FILL BIT, defined here beside the writer that
// produces the shape rather than copied into the tool that checks it.
//
// The bit says THE READ HAPPENED -- not that a team came back. The engine's own answer for an
// entrant seated on no side is kPlayerNoTeam16 (RosterTeamSeated), so a SET bit admits a team the
// engine accepts or that sentinel, and nothing between. A CLEAR bit admits the sentinel only:
// unread is absent, and a zeroed team beside a clear bit is a file `echodemo verify` refuses.
inline bool RecordTeamAgreesWithFill(const PlayerRecord& pr) {
#if defined(W2A4_MUTATE_VERIFY_REJECTS_NO_TEAM)
    // The biconditional this replaced: it could not tell a recorded "no team" from an unread one,
    // and declared every capture holding an unseated entrant corrupt.
    return ((pr.fill & kPlayerFillTeam) != 0) == (pr.teamId16 != kPlayerNoTeam16);
#else
    if ((pr.fill & kPlayerFillTeam) == 0) {
        return pr.teamId16 == kPlayerNoTeam16;
    }
    return pr.teamId16 == kPlayerNoTeam16 ||
           pr.teamId16 < uint16_t(facts::NETGAME_USER_ROLE_BOUND);
#endif
}

// ---------------------------------------------------------------------------------------------
// THE SAME RULE FOR THE COLOUR, and it is the engine's, not a biconditional over our own fill bit.
//
// `IDENT.ALIGN_TEAM_LIMIT` is `cmp dx,2 / jnb` inside CR15NetGame::Alignment: teams 2 and 4 --
// spectator and moderator -- index no alignment table and the engine answers 0xFFFF for them.
// That answer IS kPlayerNoAlignment16, so a row whose alignment was READ and came back "this team
// has no colour" carries a SET bit beside the sentinel, and the biconditional called it corrupt
// (L2 #17: `team 2 ... align -`, fill 0x01F3, `echodemo verify` FAIL on 2403 checks).
//
// So: a CLEAR bit admits the sentinel only -- unread is absent. A SET bit admits the sentinel from
// any team, because that is the engine's own answer for a team without one; what it does NOT admit
// is a COLOUR on a team the engine gives no colour to. An alignment beside a spectator, a
// moderator or the no-team sentinel was invented somewhere, and inventing colour is the defect
// this pair of rules exists to catch.
inline bool RecordAlignmentAgreesWithFill(const PlayerRecord& pr) {
#if defined(W2A4_MUTATE_VERIFY_REJECTS_NO_ALIGN)
    // The biconditional this replaced: it could not tell a recorded "this team has no colour" from
    // an unread one, and failed every capture holding a spectator row.
    return ((pr.fill & kPlayerFillAlign) != 0) == (pr.alignment16 != kPlayerNoAlignment16);
#else
    if ((pr.fill & kPlayerFillAlign) == 0) {
        return pr.alignment16 == kPlayerNoAlignment16;
    }
    return pr.alignment16 == kPlayerNoAlignment16 ||
           pr.teamId16 < uint16_t(facts::IDENT_ALIGN_TEAM_LIMIT);
#endif
}

// ---------------------------------------------------------------------------------------------
// THE V1 RECORD. Every field the format has, from the row that sourced it, with the absence value
// written wherever the fill bit is clear -- `echodemo verify` cross-checks the two through
// RecordTeamAgreesWithFill, which is this header's rule and not a second copy of it.
inline void FillPlayerRecord(const RosterRow& r, PlayerRecord& pr) {
    memset(&pr, 0, sizeof pr);
    pr.xplatformId = (r.fill & kRosterFillXplat) ? r.xplatformId : 0;
    pr.userSlot = r.userSlot;
    pr.userIndex = r.userIndex;
    pr.generation = r.generation;

    // B.0's narrow fields stay, so a row this version writes is still a B.0 row. 0xFF is the closest a
    // byte gets to "no team"; the 16-bit fields beside it are the answer and the fill mask is the proof.
    pr.teamId = ((r.fill & kRosterFillTeam) && r.team <= 0xFF) ? uint8_t(r.team) : 0xFFu;
    pr.teamSlot = ((r.fill & kRosterFillTeam) && r.teamSlot <= 0xFF) ? uint8_t(r.teamSlot) : 0xFFu;
    pr.alignment = ((r.fill & kRosterFillAlign) && r.alignment <= 0xFF) ? uint8_t(r.alignment) : 0xFFu;
    memcpy(pr.displayName, r.displayName, sizeof pr.displayName);
    memcpy(pr.platformStr, r.platformStr, sizeof pr.platformStr);

    pr.loadoutNumber = (r.fill & kRosterFillNumbers) ? r.loadoutNumber : 0;
    pr.loadoutNumberBody = (r.fill & kRosterFillNumbers) ? r.loadoutNumberBody : 0;
    memcpy(pr.loadoutSlots, r.loadoutSlots, sizeof pr.loadoutSlots);

    memcpy(pr.displayNameFull, r.displayName, sizeof pr.displayNameFull);
    memcpy(pr.platformStrFull, r.platformStr, sizeof pr.platformStrFull);
    pr.teamId16 = (r.fill & kRosterFillTeam) ? r.team : kPlayerNoTeam16;
    pr.teamSlot16 = (r.fill & kRosterFillTeam) ? r.teamSlot : kPlayerNoTeam16;
    pr.alignment16 = (r.fill & kRosterFillAlign) ? r.alignment : kPlayerNoAlignment16;
    pr.firstSeenUs = r.firstSeenUs;
    pr.lastSeenUs = r.lastSeenUs;
    // Two words, because they are gated separately: `fill` is v1's and means what it always meant,
    // `fillExt` rides the v2 extension and is the answer to "was this ever sourced" for the bits
    // the file could not carry before -- the jersey numbers above all (RECORDER-3 F5).
    pr.fill = r.fill & kPlayerFillAll;
    pr.fillExt = r.fill & kRosterFillExtAll;
    pr.extendedV2 = 1u;
}

// THE ONE MEANING OF THE TRUNCATION BIT, reconciling ENG-W1B-PLAYERRECORD ef1fd5d with
// ENG-W1E-READERS 2a233a0 / ENG-W1E-JSONWALK 65eb960:
//
//   `jsonTruncated` set  ==  WHAT THE FILE HOLDS IS NOT THE COMPLETE DOCUMENT.
//
// The two layers disagreed on how to get there. The source layer (CopyJsonBytes) reports the full
// document length in `written`, copies NOTHING when it does not fit, and refuses ShortBuffer. The
// writer layer (manifest_io) caps at kPlayerJsonCap and sets the bit. Both are kept, and the capture
// asks the source with cap == kPlayerJsonCap so the two can never both fire: the blob handed to the
// writer is either the WHOLE document or empty, the writer's cap is therefore never reached from a
// live capture, and a set bit always means the same thing at both layers -- a document existed and
// none of it is here. A half document never reaches the file by either route.
inline void FillPlayerJson(const IRosterSource* src, uint16_t userIndex, uint8_t* entrantBuf,
                           uint8_t* unlocksBuf, size_t cap, PlayerRecord& pr,
                           RosterRefusals& refusals) {
    pr.entrantJson.p = nullptr;
    pr.entrantJson.n = 0;
    pr.unlocksJson.p = nullptr;
    pr.unlocksJson.n = 0;
    pr.jsonTruncated = 0;
    if (!src) {
        return;
    }

    size_t written = 0;
    bool truncated = false;
    const char* why = nullptr;
    identity_access::Refusal refusal{};
    EntrantJsonOutcome outcome{};
    if (src->EntrantJson(userIndex, entrantBuf, cap, written, truncated, why, refusal, &outcome)) {
        pr.entrantJson.p = entrantBuf;
        pr.entrantJson.n = uint32_t(written);
        if (outcome.source == ProfileSource::EntrantRecord) {
            pr.fillExt |= kRosterFillProfileFromEntrantRec;
        }
        if (outcome.lossyNumber) pr.fillExt |= kRosterFillProfileLossyNumber;
    } else {
        if (truncated) pr.jsonTruncated |= kPlayerJsonTruncEntrant;
        refusals.Note(RosterRefusal::EntrantJson, why);
    }

    written = 0;
    truncated = false;
    why = nullptr;
    if (src->UnlocksJson(userIndex, unlocksBuf, cap, written, truncated, why)) {
        pr.unlocksJson.p = unlocksBuf;
        pr.unlocksJson.n = uint32_t(written);
    } else {
        if (truncated) pr.jsonTruncated |= kPlayerJsonTruncUnlocks;
        refusals.Note(RosterRefusal::UnlocksJson, why);
    }
}

}  // namespace capture
}  // namespace replay
