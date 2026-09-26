// The roster scan. The defect these cover is L2-RUN-3 F3: `players : 0` beside 6 997 stored player
// rows, because ManifestBuilder::AddPlayer had no non-test caller and the only other roster feeder
// derived entrants from lobby traffic a solo host never sees.

#include "doctest.h"

#include "capture/roster.h"

#include "capture/identity_emit.h"

#include "engine/identity_access.h"   // the real TeamOf/TeamSlotOf, over a plain buffer
#include "engine/pawn_provision.h"     // JoinDescFromRecord: the map the recorded team ends at

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace replay;
using namespace replay::capture;

namespace {

// A fake identity table. Every primitive is backed by a row rather than a rule, so a test can take a
// field AWAY and see whether the scan noticed -- which is the only way the fill mask can be checked.
struct FakeRow {
    uint16_t index = 0, slot = 0, generation = 0;
    uint64_t xplat = 0;
    std::string name, platform;
    uint16_t team = 0, teamSlot = 0, alignment = 0;
    uint64_t items[20] = {};
    uint64_t keys[20] = {};
    bool present = true, local = false, profile = true;
    uint16_t number = 0, numberBody = 0;
    std::string entrantJson, unlocksJson;
    bool hasName = true, hasTeam = true, hasAlign = true, hasLoadout = true, hasGen = true;
    bool hasKeys = true;   // a source that answers items only -- every source before W2-C-2
    uint64_t available = 0;   // what the engine's component HOLDS, when this source can say
    bool hasAvailable = false;
    bool hasXplat = true, hasPlatform = true, hasNumbers = true;
    bool hasEntrantJson = true, hasUnlocksJson = false;
    // Which of the two storages answered for this row's document, and whether the walk had to
    // write null over a non-integer number (RECORDER-3 F1/F4).
    ProfileSource entrantJsonSource = ProfileSource::UserTable;
    bool entrantJsonLossy = false;
};

class FakeSource : public IRosterSource {
public:
    std::vector<FakeRow> rows;
    uint16_t band = 8;
    bool bandReadable = true;
    // Every read the fingerprint is supposed to skip is counted, because "the scan got the same
    // answer" and "the scan did not ask" are the two things this gate has to tell apart.
    mutable int platformReads = 0, alignReads = 0, genReads = 0, profileReads = 0, numberReads = 0;
    // AUD-W2-A-2 F9's witness. One customisation-component walk per entrant per scan is the claim;
    // a counter is the only thing that can falsify it, because a second walk costs microseconds in
    // the field and nothing at all against a fake.
    mutable int pairWalks = 0;
    // The profile DOCUMENT, counted apart from the HasProfileJson probe: the producer reads it only
    // for a row whose block content moved, and only a counter on the serialiser can show that.
    mutable int entrantJsonReads = 0;

    bool EntrantBand(uint16_t& out, const char*& why) const override {
        if (!bandReadable) {
            why = "EntrantCountOff";
            return false;
        }
        out = band;
        return true;
    }

    bool Fingerprint(uint16_t idx, uint64_t& out) const override {
        const FakeRow* r = Find(idx);
        if (!r || !r->hasName) return false;
        uint64_t h = kRosterFoldSeed;
        char name[kRosterNameCap] = {};
        DisplayName(idx, name, sizeof name);
        h = RosterFold(h, name, sizeof name);
        h = RosterFold(h, &r->slot, sizeof r->slot);
        h = RosterFold(h, &r->team, sizeof r->team);
        h = RosterFold(h, &r->teamSlot, sizeof r->teamSlot);
        // The SHIPPED fold, not a second copy of it: this is the rule under test in
        // `a component that grows behind the same twenty pairs re-reads the row`.
        h = RosterFoldLoadout(h, r->items, r->available, r->hasAvailable);
        h = RosterFold(h, &r->number, sizeof r->number);
        out = h;
        return true;
    }

    const FakeRow* Find(uint16_t idx) const {
        // O(1) when the fixture happens to be laid out by index, which is what the per-tick budget
        // case needs: a linear probe here would put the FAKE's cost in the roster's measurement.
        if (idx < rows.size() && rows[idx].index == idx) {
            return rows[idx].present ? &rows[idx] : nullptr;
        }
        for (const FakeRow& r : rows) {
            if (r.index == idx && r.present) return &r;
        }
        return nullptr;
    }
    bool SlotOf(uint16_t idx, uint16_t& slot) const override {
        const FakeRow* r = Find(idx);
        if (!r) return false;
        slot = r->slot;
        return true;
    }
    bool DisplayName(uint16_t idx, char* out, size_t cap) const override {
        const FakeRow* r = Find(idx);
        if (!r || !r->hasName) return false;
        memset(out, 0, cap);
        memcpy(out, r->name.c_str(), r->name.size() < cap - 1 ? r->name.size() : cap - 1);
        return true;
    }
    bool PlatformString(uint16_t idx, char* out, size_t cap) const override {
        ++platformReads;
        const FakeRow* r = Find(idx);
        if (!r || !r->hasPlatform) return false;
        memset(out, 0, cap);
        memcpy(out, r->platform.c_str(), r->platform.size() < cap - 1 ? r->platform.size() : cap - 1);
        return true;
    }
    bool XPlatformId(uint16_t idx, uint64_t& out) const override {
        const FakeRow* r = Find(idx);
        if (!r || !r->hasXplat) return false;
        out = r->xplat;
        return true;
    }
    bool Team(uint16_t idx, uint16_t& team, uint16_t& teamSlot) const override {
        const FakeRow* r = Find(idx);
        if (!r || !r->hasTeam) return false;
        team = r->team;
        teamSlot = r->teamSlot;
        return true;
    }
    bool Alignment(uint16_t idx, uint16_t& out) const override {
        ++alignReads;
        const FakeRow* r = Find(idx);
        if (!r || !r->hasAlign) return false;
        out = r->alignment;
        return true;
    }
    bool LoadoutPairs(uint16_t idx, uint64_t keys[20], uint64_t items[20], uint64_t& held,
                      bool& heldKnown) const override {
        ++pairWalks;
        heldKnown = false;
        const FakeRow* r = Find(idx);
        if (!r || !r->hasLoadout || !r->hasKeys) return false;
        memcpy(items, r->items, sizeof r->items);
        if (keys) memcpy(keys, r->keys, sizeof r->keys);
        if (r->hasAvailable) {
            held = r->available;
            heldKnown = true;
        }
        return true;
    }
    bool LoadoutItems(uint16_t idx, uint64_t items[20]) const override {
        const FakeRow* r = Find(idx);
        if (!r || !r->hasLoadout) return false;
        memcpy(items, r->items, sizeof r->items);
        return true;
    }
    bool LoadoutNumbers(uint16_t slot, uint16_t& n, uint16_t& nb, const char*& why) const override {
        ++numberReads;
        for (const FakeRow& r : rows) {
            if (r.slot == slot && r.present) {
                if (!r.hasNumbers) {
                    why = "BuildMismatch";
                    return false;
                }
                n = r.number;
                nb = r.numberBody;
                return true;
            }
        }
        why = "NoSuchEntrant";
        return false;
    }
    bool EntrantJson(uint16_t idx, void* out, size_t cap, size_t& written, bool& truncated,
                     const char*& why, identity_access::Refusal& refusal,
                     EntrantJsonOutcome* outcome = nullptr) const override {
        ++entrantJsonReads;
        written = 0;
        truncated = false;
        if (outcome) *outcome = EntrantJsonOutcome{};
        const FakeRow* r = Find(idx);
        if (!r || !r->hasEntrantJson) {
            why = "EmptyProfile";
            refusal = identity_access::Refusal::EmptyProfile;
            return false;
        }
        written = r->entrantJson.size();
        if (written > cap) {
            truncated = true;
            why = "ShortBuffer";
            refusal = identity_access::Refusal::ShortBuffer;
            return false;  // the short arm copies NOTHING -- CopyJsonBytes's contract
        }
        memcpy(out, r->entrantJson.data(), written);
        if (outcome) *outcome = {r->entrantJsonSource, r->entrantJsonLossy};
        return true;
    }
    bool UnlocksJson(uint16_t idx, void* out, size_t cap, size_t& written, bool& truncated,
                     const char*& why) const override {
        written = 0;
        truncated = false;
        const FakeRow* r = Find(idx);
        if (!r || !r->hasUnlocksJson) {
            why = "no subtree serialiser";
            return false;
        }
        written = r->unlocksJson.size();
        if (written > cap) {
            truncated = true;
            why = "ShortBuffer";
            return false;
        }
        memcpy(out, r->unlocksJson.data(), written);
        return true;
    }
    bool Generation(uint16_t slot, uint16_t& out) const override {
        ++genReads;
        for (const FakeRow& r : rows) {
            if (r.slot == slot && r.present) {
                if (!r.hasGen) return false;
                out = r.generation;
                return true;
            }
        }
        return false;
    }
    bool HasProfileJson(uint16_t idx) const override {
        ++profileReads;
        const FakeRow* r = Find(idx);
        return r && r->profile;
    }
    bool IsLocal(uint16_t idx) const override {
        const FakeRow* r = Find(idx);
        return r && r->local;
    }
};

// The one seed every loadout id in this file is built from. Named rather than transcribed at each
// assertion: an expectation that restates the fixture's arithmetic can only ever agree with itself.
// EVERY fixture magnitude in this file is built from a seed below 0x100 and named arithmetic. A
// literal at or above that floor is gated by the transcription rule wherever it appears, and a test
// fixture is exactly where an accidental collision with a registry value is hardest to notice.
constexpr uint64_t kItemSeed = uint64_t(0xA0) << 24;
constexpr uint64_t kAlignSeed = 0x10;
constexpr uint64_t kIdBase = 0x40;
constexpr uint64_t kTick = 100;

// The engine's own slot key. Deliberately unlike both the position and the item: a roster that
// mixed the two up would still look right against a fixture where they agree.
uint64_t KeyFor(uint16_t idx, int slot) {
    return 0x9E3779B97F4A7C15ull ^ (0x1000ull * uint64_t(19 - slot)) ^ (0x10ull * idx);
}

uint64_t ItemFor(uint16_t idx, int slot) {
    return kItemSeed + uint64_t(idx) * 100 + uint64_t(slot);
}

FakeRow MakeRow(uint16_t idx, const char* name, uint64_t xplat, uint16_t team) {
    FakeRow r;
    r.index = idx;
    r.slot = uint16_t(idx + 3);      // NOT equal to the index: the two spaces must not be conflated
    r.generation = uint16_t(idx + 1);
    r.xplat = xplat;
    r.name = name;
    r.platform = "PC";
    r.team = team;
    r.teamSlot = uint16_t(idx % 5);
    r.alignment = uint16_t(kAlignSeed + team);
    for (int i = 0; i < 20; ++i) {
        r.items[i] = ItemFor(idx, i);
        r.keys[i] = KeyFor(idx, i);
    }
    r.number = uint16_t(kAlignSeed + idx);
    r.numberBody = uint16_t(kAlignSeed * 2 + idx);
    r.entrantJson = std::string("{\"name\":\"") + name + "\"}";
    return r;
}

const RosterRow* RowNamed(const RosterTable& t, const char* name) {
    for (uint16_t i = 0; i < t.Count(); ++i) {
        if (strcmp(t.At(i).displayName, name) == 0) return &t.At(i);
    }
    return nullptr;
}

}  // namespace

TEST_CASE("roster: three engine rows produce three rows with every field") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "Blue One", kIdBase + 1, 0));
    src.rows.push_back(MakeRow(1, "Blue Two", kIdBase + 2, 0));
    src.rows.push_back(MakeRow(4, "Orange One", kIdBase + 3, 1));
    src.rows[0].local = true;

    RosterTable table;
    const RosterScanReport rep = ScanRoster(&src, table, kTick * 5);

    CHECK(rep.sourceAbsent == 0);
    CHECK(rep.present == 3);
    CHECK(rep.appended == 3);
    CHECK(table.Count() == 3);

    // EVERY field, named one at a time. A loop over the rows would let a scan that drops one field
    // for every row pass, because the assertion would be about the fixture and not about the field.
    const RosterRow* b1 = RowNamed(table, "Blue One");
    REQUIRE(b1 != nullptr);
    CHECK(b1->xplatformId == kIdBase + 1);
    CHECK(b1->userIndex == 0);
    CHECK(b1->userSlot == 3);
    CHECK(b1->generation == 1);
    CHECK(b1->team == 0);
    CHECK(b1->teamSlot == 0);
    CHECK(b1->alignment == kAlignSeed);
    CHECK(std::string(b1->platformStr) == "PC");
    CHECK(b1->loadoutSlots[0] == ItemFor(0, 0));
    CHECK(b1->loadoutSlots[19] == ItemFor(0, 19));
    CHECK(b1->loadoutSlotKeys[0] == KeyFor(0, 0));
    CHECK(b1->loadoutSlotKeys[19] == KeyFor(0, 19));
    CHECK(b1->hasProfileJson == 1);
    CHECK(b1->local == 1);
    CHECK(b1->firstSeenUs == kTick * 5);
    CHECK(b1->lastSeenUs == kTick * 5);
    CHECK(b1->active == 1);
    const uint32_t everything = kRosterFillSlot | kRosterFillName | kRosterFillPlatform |
                                kRosterFillXplat | kRosterFillTeam | kRosterFillAlign |
                                kRosterFillLoadout | kRosterFillGen | kRosterFillProfile |
                                kRosterFillLocal | kRosterFillNumbers | kRosterFillLoadoutKeys;
    CHECK(b1->fill == everything);

    const RosterRow* o1 = RowNamed(table, "Orange One");
    REQUIRE(o1 != nullptr);
    CHECK(o1->userIndex == 4);
    CHECK(o1->userSlot == 7);
    CHECK(o1->team == 1);
    CHECK(o1->alignment == kAlignSeed + 1);
    CHECK(o1->local == 0);
    CHECK((o1->fill & kRosterFillLocal) == 0);
    CHECK(o1->loadoutSlots[3] == ItemFor(4, 3));
}

TEST_CASE("roster: a dropped field is absent, not zero") {
    FakeSource src;
    FakeRow r = MakeRow(0, "No Team", kIdBase + 1, 2);
    r.hasTeam = false;
    r.hasAlign = false;
    src.rows.push_back(r);

    RosterTable table;
    ScanRoster(&src, table, 10);
    REQUIRE(table.Count() == 1);
    CHECK((table.At(0).fill & kRosterFillTeam) == 0);
    CHECK((table.At(0).fill & kRosterFillAlign) == 0);
    // The row's team is the NO TEAM word, not team 0, so nobody reads an unsourced player onto blue.
    CHECK(table.At(0).team == kRosterNoTeam);
    CHECK(table.At(0).alignment == kRosterNoAlign);
    CHECK((table.At(0).fill & kRosterFillName) != 0);
}

TEST_CASE("roster: a solo host produces exactly one row") {
    FakeSource src;
    FakeRow r = MakeRow(0, "Owner", kIdBase + 4, 0);
    r.local = true;
    src.rows.push_back(r);

    RosterTable table;
    const RosterScanReport rep = ScanRoster(&src, table, 1);
    CHECK(rep.present == 1);
    CHECK(table.Count() == 1);
    CHECK(table.At(0).local == 1);
    // L2 #4's expectation for the owner's own solo `-offline` launch: ROSTER 1, not ROSTER 0.
    CHECK(table.Count() == 1);
}

TEST_CASE("roster: a join mid-capture appends exactly one row") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "First", kIdBase + 1, 0));

    RosterTable table;
    for (int tick = 0; tick < 5; ++tick) {
        const RosterScanReport rep = ScanRoster(&src, table, kTick + uint64_t(tick));
        // THE IDEMPOTENCE THAT MAKES A PER-TICK SCAN LEGAL. Without it a 3 000-tick capture ends
        // with 3 000 rows for one player, which is the mirror of the defect this replaces.
        CHECK(rep.appended == (tick == 0 ? 1 : 0));
        CHECK(table.Count() == 1);
    }

    src.rows.push_back(MakeRow(2, "Latecomer", kIdBase + 2, 1));
    const RosterScanReport joined = ScanRoster(&src, table, kTick * 9);
    CHECK(joined.appended == 1);
    // The sitting row is UNCHANGED, not re-read: its fingerprint did not move, and a join elsewhere
    // is not a reason to ask the engine about a player who did not.
    CHECK(joined.updated == 0);
    CHECK(joined.unchanged == 1);
    CHECK(table.Count() == 2);

    const RosterRow* late = RowNamed(table, "Latecomer");
    REQUIRE(late != nullptr);
    CHECK(late->firstSeenUs == kTick * 9);       // not the capture's t0: the row knows when it arrived
    CHECK(table.At(0).firstSeenUs == kTick);

    // And the join is appended ONCE, not once per later tick.
    for (int tick = 0; tick < 4; ++tick) {
        const RosterScanReport rep = ScanRoster(&src, table, kTick * 10 + uint64_t(tick));
        CHECK(rep.appended == 0);
        CHECK(table.Count() == 2);
    }
}

TEST_CASE("roster: dedup keys on the user id, not the slot") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "Mover", kIdBase + 5, 0));

    RosterTable table;
    ScanRoster(&src, table, kTick);
    REQUIRE(table.Count() == 1);

    // Same person, new user index AND new slot -- a rejoin. One row, updated.
    src.rows.clear();
    FakeRow again = MakeRow(5, "Mover", kIdBase + 5, 1);
    src.rows.push_back(again);
    const RosterScanReport rep = ScanRoster(&src, table, kTick * 2);
    CHECK(rep.appended == 0);
    CHECK(rep.updated == 1);
    CHECK(table.Count() == 1);
    CHECK(table.At(0).userSlot == 8);
    CHECK(table.At(0).firstSeenUs == kTick);  // the row remembers the FIRST arrival, not the latest

    // A DIFFERENT id landing in the vacated slot is a different person and gets its own row.
    src.rows.clear();
    src.rows.push_back(MakeRow(5, "Someone Else", kIdBase + 6, 1));
    const RosterScanReport second = ScanRoster(&src, table, kTick * 3);
    CHECK(second.appended == 1);
    CHECK(table.Count() == 2);
    CHECK(RowNamed(table, "Mover") != nullptr);
    CHECK(RowNamed(table, "Someone Else") != nullptr);
}

TEST_CASE("roster: two unidentified players do not merge into one row") {
    FakeSource src;
    FakeRow a = MakeRow(0, "A", 0, 0);
    FakeRow b = MakeRow(1, "B", 0, 1);
    a.hasXplat = false;
    b.hasXplat = false;
    src.rows.push_back(a);
    src.rows.push_back(b);

    RosterTable table;
    ScanRoster(&src, table, 10);
    // Both ids are absent. Falling back to "equal because both are zero" would name one player.
    CHECK(table.Count() == 2);
}

TEST_CASE("roster: a team change is counted, and a departure is recorded without losing the row") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "Switcher", kIdBase + 1, 0));
    src.rows.push_back(MakeRow(1, "Stayer", kIdBase + 2, 1));

    RosterTable table;
    ScanRoster(&src, table, kTick);
    CHECK(table.Count() == 2);

    src.rows[0].team = 1;
    src.rows[0].teamSlot = 4;
    const RosterScanReport moved = ScanRoster(&src, table, kTick * 2);
    CHECK(moved.teamChanged == 1);
    CHECK(moved.appended == 0);
    const RosterRow* sw = RowNamed(table, "Switcher");
    REQUIRE(sw != nullptr);
    CHECK(sw->team == 1);
    CHECK(sw->teamSlot == 4);
    CHECK(sw->teamChanges == 1);

    // The first fill of a team is not a change: a row that starts on blue never moved.
    CHECK(RowNamed(table, "Stayer")->teamChanges == 0);

    src.rows[1].present = false;
    const RosterScanReport left = ScanRoster(&src, table, kTick * 3);
    CHECK(left.departed == 1);
    CHECK(table.Count() == 2);            // the row STAYS: a replay has to name who left
    const RosterRow* gone = RowNamed(table, "Stayer");
    REQUIRE(gone != nullptr);
    CHECK(gone->active == 0);
    CHECK(gone->lastSeenUs == kTick * 2);       // when they were last seen, not when the scan noticed
    CHECK(RowNamed(table, "Switcher")->active == 1);

    // A second scan does not re-report a departure that already happened.
    CHECK(ScanRoster(&src, table, kTick * 4).departed == 0);
}

TEST_CASE("roster: no source is ABSENT, which is not an empty match") {
    RosterTable table;
    const RosterScanReport rep = ScanRoster(nullptr, table, 10);
    CHECK(rep.sourceAbsent == 1);
    CHECK(rep.bandWalked == 0);
    CHECK(table.Count() == 0);

    // And a source that walks a band and finds nobody is the OTHER answer: the walk happened.
    FakeSource empty;
    const RosterScanReport walked = ScanRoster(&empty, table, 10);
    CHECK(walked.sourceAbsent == 0);
    CHECK(walked.bandWalked == 8);
    CHECK(walked.present == 0);
}

TEST_CASE("roster: the scan horizon is clamped and a full table refuses rather than overruns") {
    FakeSource src;
    src.band = 40000;                      // an engine answer nobody should take on trust
    for (uint16_t i = 0; i < kRosterMaxRows + 4; ++i) {
        src.rows.push_back(MakeRow(i, "P", kIdBase + 0x20 + i, uint16_t(i % 2)));
    }
    RosterTable table;
    const RosterScanReport rep = ScanRoster(&src, table, 1);
    CHECK(rep.bandWalked == kRosterBandCap);
    CHECK(table.Count() == kRosterMaxRows);
    CHECK(rep.tableFull == 0);             // the horizon stops the walk before the table fills
    CHECK(rep.appended == kRosterMaxRows);
}

// The band. ENG-W2A-ROSTER walked a horizon of kRosterBandCap because no accessor existed; W1-E's
// 2a233a0 landed one, so a horizon is now OUR number wearing the engine's name. Restoring the 64
// reds here twice over: the refused scan would walk 64 indices, and the readable scan would report a
// band the fixture never set.
TEST_CASE("roster: the band is the engine's entrant count, and an unreadable count refuses by name") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "alpha", kIdBase + 1, 1));
    src.rows.push_back(MakeRow(1, "bravo", kIdBase + 2, 2));
    src.band = 2;
    RosterTable table;

    const RosterScanReport ok = ScanRoster(&src, table, kTick);
    CHECK(ok.bandRefused == 0);
    CHECK(ok.bandWalked == 2);
    CHECK(ok.bandWalked != kRosterBandCap);
    CHECK(table.Count() == 2);

    src.bandReadable = false;
    RosterTable empty;
    const RosterScanReport refused = ScanRoster(&src, empty, kTick);
    CHECK(refused.bandRefused == 1);
    CHECK(refused.bandWalked == 0);
    CHECK(refused.present == 0);
    CHECK(empty.Count() == 0);
    REQUIRE(refused.bandWhy != nullptr);
    CHECK(std::string(refused.bandWhy) == "EntrantCountOff");
    CHECK(refused.refusals.byReason[size_t(RosterRefusal::Band)] == 1);
    CHECK(std::string(refused.refusals.wordByReason[size_t(RosterRefusal::Band)]) ==
          "EntrantCountOff");
}

TEST_CASE("roster: a tick that changed nothing re-reads nothing, and a changed field re-reads it") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "alpha", kIdBase + 1, 1));
    src.rows.push_back(MakeRow(1, "bravo", kIdBase + 2, 2));
    src.band = 2;
    RosterTable table;

    const RosterScanReport first = ScanRoster(&src, table, kTick);
    CHECK(first.appended == 2);
    CHECK(first.unchanged == 0);
    const int platformAfterFirst = src.platformReads;
    const int alignAfterFirst = src.alignReads;
    const int genAfterFirst = src.genReads;
    const int profileAfterFirst = src.profileReads;
    CHECK(platformAfterFirst == 2);

    const RosterScanReport second = ScanRoster(&src, table, kTick * 2);
    CHECK(second.appended == 0);
    CHECK(second.updated == 0);
    CHECK(second.unchanged == 2);
    CHECK(table.Count() == 2);
    // Not "the same answer": not asked at all.
    CHECK(src.platformReads == platformAfterFirst);
    CHECK(src.alignReads == alignAfterFirst);
    CHECK(src.genReads == genAfterFirst);
    CHECK(src.profileReads == profileAfterFirst);
    // The clock still moves -- a row skipped is still a row seen.
    REQUIRE(RowNamed(table, "bravo") != nullptr);
    CHECK(RowNamed(table, "bravo")->lastSeenUs == kTick * 2);
    CHECK(RowNamed(table, "bravo")->firstSeenUs == kTick);

    src.rows[1].team = 7;
    const RosterScanReport third = ScanRoster(&src, table, kTick * 3);
    CHECK(third.updated == 1);
    CHECK(third.unchanged == 1);
    CHECK(third.teamChanged == 1);
    CHECK(src.platformReads == platformAfterFirst + 1);
    CHECK(RowNamed(table, "bravo")->team == 7);
    CHECK(RowNamed(table, "alpha")->team == 1);
}

TEST_CASE("roster: the loadout numbers are sourced, and a refused pair is named rather than zeroed") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "alpha", kIdBase + 1, 1));
    src.band = 1;
    RosterTable table;
    ScanRoster(&src, table, kTick);
    const RosterRow* r = RowNamed(table, "alpha");
    REQUIRE(r != nullptr);
    CHECK((r->fill & kRosterFillNumbers) != 0);
    CHECK(r->loadoutNumber == src.rows[0].number);
    CHECK(r->loadoutNumberBody == src.rows[0].numberBody);
    CHECK(r->loadoutNumber != r->loadoutNumberBody);

    FakeSource off;
    off.rows.push_back(MakeRow(0, "alpha", kIdBase + 1, 1));
    off.rows[0].hasNumbers = false;
    off.band = 1;
    RosterTable t2;
    const RosterScanReport rep = ScanRoster(&off, t2, kTick);
    const RosterRow* r2 = RowNamed(t2, "alpha");
    REQUIRE(r2 != nullptr);
    CHECK((r2->fill & kRosterFillNumbers) == 0);
    CHECK((r2->fill & kRosterFillLoadout) != 0);  // the items did not go down with the header
    CHECK(rep.refusals.byReason[size_t(RosterRefusal::LoadoutNumbers)] == 1);
    CHECK(std::string(rep.refusals.wordByReason[size_t(RosterRefusal::LoadoutNumbers)]) ==
          "BuildMismatch");
}

// The v1 record, field by field and then byte for byte. An expectation restating the mapping's own
// arithmetic would only agree with itself, so it is written from the FIXTURE's values.
TEST_CASE("roster: every v1 field reaches the record, and an unsourced one carries its absence value") {
    FakeSource src;
    src.rows.push_back(MakeRow(2, "charlie", kIdBase + 9, 3));
    src.band = 3;
    RosterTable table;
    ScanRoster(&src, table, kTick);
    const RosterRow* r = RowNamed(table, "charlie");
    REQUIRE(r != nullptr);

    PlayerRecord got;
    FillPlayerRecord(*r, got);

    PlayerRecord want;
    memset(&want, 0, sizeof want);
    want.xplatformId = kIdBase + 9;
    memcpy(want.displayName, "charlie", 7);
    memcpy(want.platformStr, "PC", 2);
    want.userSlot = 5;    // MakeRow: index + 3
    want.userIndex = 2;
    want.generation = 3;  // MakeRow: index + 1
    want.teamId = 3;
    want.teamSlot = 2;    // MakeRow: index % 5
    want.alignment = uint8_t(kAlignSeed + 3);
    want.loadoutNumber = uint16_t(kAlignSeed + 2);
    want.loadoutNumberBody = uint16_t(kAlignSeed * 2 + 2);
    for (int i = 0; i < 20; ++i) {
        want.loadoutSlots[i] = ItemFor(2, i);
    }
    memcpy(want.displayNameFull, "charlie", 7);
    memcpy(want.platformStrFull, "PC", 2);
    want.teamId16 = 3;
    want.teamSlot16 = 2;
    want.alignment16 = uint16_t(kAlignSeed + 3);
    want.firstSeenUs = kTick;
    want.lastSeenUs = kTick;
    want.fill = kPlayerFillSlot | kPlayerFillName | kPlayerFillPlatform | kPlayerFillXplat |
                kPlayerFillTeam | kPlayerFillAlign | kPlayerFillLoadout | kPlayerFillGen |
                kPlayerFillProfile;
    // v2's own word. The numbers were sourced -- which is the whole point of the bit, since this
    // row's jersey could legitimately have been 0 -- and so were the twenty slot keys; how many
    // entries the component holds, this source cannot say, so that bit stays clear.
    want.fillExt = kPlayerFillNumbers | kPlayerFillLoadoutKeys;
    want.extendedV2 = 1;
    CHECK(memcmp(&got, &want, sizeof want) == 0);

    // The absence values, which `echodemo verify` cross-checks against the fill mask in both
    // directions: a zeroed team beside a clear TEAM bit is a file it refuses.
    FakeSource bare;
    bare.rows.push_back(MakeRow(0, "delta", kIdBase + 4, 1));
    bare.rows[0].hasTeam = false;
    bare.rows[0].hasAlign = false;
    bare.band = 1;
    RosterTable t2;
    ScanRoster(&bare, t2, kTick);
    PlayerRecord absent;
    FillPlayerRecord(t2.At(0), absent);
    CHECK(absent.teamId16 == kPlayerNoTeam16);
    CHECK(absent.teamSlot16 == kPlayerNoTeam16);
    CHECK(absent.alignment16 == kPlayerNoAlignment16);
    CHECK((absent.fill & kPlayerFillTeam) == 0);
    CHECK((absent.fill & kPlayerFillAlign) == 0);
    CHECK(absent.teamId == 0xFF);
}

// THE ONE MEANING OF THE TRUNCATION BIT. W1-B's writer caps and flags; W1-E's source refuses and
// copies nothing. The capture asks at the file's own cap, so what it hands the writer is the WHOLE
// document or none of it, and the bit means the same thing at both layers: what is here is not the
// complete document.
TEST_CASE("roster: a profile that does not fit sets the bit and leaves no half document") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "alpha", kIdBase + 1, 1));
    src.rows.push_back(MakeRow(1, "bravo", kIdBase + 2, 2));
    src.rows[1].entrantJson = std::string("{") + std::string(kRosterJsonCap, 'x') + "}";
    src.band = 2;
    RosterTable table;
    ScanRoster(&src, table, kTick);
    REQUIRE(table.Count() == 2);

    std::vector<uint8_t> entrantBuf(kRosterJsonCap, 0xCC), unlocksBuf(kRosterJsonCap, 0xCC);
    RosterRefusals refusals = {};

    PlayerRecord fits;
    FillPlayerRecord(table.At(0), fits);
    FillPlayerJson(&src, table.At(0).userIndex, entrantBuf.data(), unlocksBuf.data(),
                   kRosterJsonCap, fits, refusals);
    CHECK((fits.jsonTruncated & kPlayerJsonTruncEntrant) == 0);
    CHECK(fits.entrantJson.n == src.rows[0].entrantJson.size());
    CHECK(memcmp(fits.entrantJson.p, src.rows[0].entrantJson.data(), fits.entrantJson.n) == 0);

    PlayerRecord over;
    FillPlayerRecord(table.At(1), over);
    FillPlayerJson(&src, table.At(1).userIndex, entrantBuf.data(), unlocksBuf.data(),
                   kRosterJsonCap, over, refusals);
    CHECK((over.jsonTruncated & kPlayerJsonTruncEntrant) != 0);
    CHECK(over.entrantJson.n == 0);
    CHECK(over.entrantJson.p == nullptr);
    // Nothing was copied: the buffer past the first row's document still holds its fill.
    CHECK(entrantBuf[kRosterJsonCap - 1] == 0xCC);
    CHECK(refusals.byReason[size_t(RosterRefusal::EntrantJson)] == 1);
    CHECK(std::string(refusals.wordByReason[size_t(RosterRefusal::EntrantJson)]) == "ShortBuffer");

    // A refusal that is not a cap leaves the bit CLEAR: "we could not read it" and "it did not fit"
    // are different facts and the file must not spell them the same way.
    CHECK((over.jsonTruncated & kPlayerJsonTruncUnlocks) == 0);
    CHECK(refusals.byReason[size_t(RosterRefusal::UnlocksJson)] == 2);
}

// THE PER-TICK BUDGET. The scan runs inside the capture tick, so its cost is the tick's; ARCH-21's
// gamestate+roster phase is budgeted at 2 us and this is the roster half of it, at a full Echo Arena
// lobby. The steady state is what matters -- a match spends almost every tick with nothing changed --
// so the loop measures that, and one tick in ninety moves a team to keep the changed arm honest.
TEST_CASE("roster: a full lobby costs under the per-tick budget") {
    FakeSource src;
    const uint16_t kLobby = 16;
    for (uint16_t i = 0; i < kLobby; ++i) {
        src.rows.push_back(MakeRow(i, ("player" + std::to_string(i)).c_str(), kIdBase + i + 1,
                                   uint16_t(i % 2)));
    }
    src.band = kLobby;
    RosterTable table;
    ScanRoster(&src, table, kTick);
    REQUIRE(table.Count() == kLobby);

    const int kTicks = 4000;
    std::vector<long long> ns;
    ns.reserve(kTicks);
    for (int t = 0; t < kTicks; ++t) {
        if (t % 90 == 89) {
            src.rows[size_t(t / 90) % kLobby].team ^= 1;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const RosterScanReport rep = ScanRoster(&src, table, kTick + uint64_t(t) * 11111);
        const auto t1 = std::chrono::steady_clock::now();
        REQUIRE(rep.appended == 0);
        ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    std::sort(ns.begin(), ns.end());
    const long long median = ns[ns.size() / 2];
    const long long p99 = ns[(ns.size() * 99) / 100];
    MESSAGE("roster scan, " << kLobby << " entrants: median " << median << " ns, p99 " << p99
                            << " ns, max " << ns.back() << " ns");
    CHECK(median < 2000);
    CHECK(p99 < 2000);
    CHECK(table.Count() == kLobby);
}

// ---------------------------------------------------------------------------------------------
// L2-RUN-5 F1. THE REGRESSION'S OWN ORACLE, and it is a COUNT rather than a stopwatch: the field
// number is `unchangedRowScans: 1489` against `S_TICK 1489` -- one full roster scan per tick, which
// took `gather` from 166 us to 1250 us and had the cost meter kill the capture at 50 s.
//
// The counter below is on the two calls that DO the work: EntrantBand is the gate's one question,
// and Fingerprint is the first of the nine per-user reads a scan makes. A per-tick scan makes 1489
// of the second; the gate makes one per scan, and a scan happens only when the engine's count moves
// or the cadence expires.
namespace {

class CountingSource : public FakeSource {
public:
    mutable uint32_t bandCalls = 0;
    mutable uint32_t fingerprintCalls = 0;

    bool EntrantBand(uint16_t& band, const char*& why) const override {
        ++bandCalls;
        return FakeSource::EntrantBand(band, why);
    }
    bool Fingerprint(uint16_t userIndex, uint64_t& out) const override {
        ++fingerprintCalls;
        return FakeSource::Fingerprint(userIndex, out);
    }
};

}  // namespace

TEST_CASE("roster: 1489 ticks do not buy 1489 scans, and a quiet tick costs one engine call") {
    CountingSource src;
    src.rows.push_back(MakeRow(0, "Solo Host", kIdBase + 11, 0));
    src.rows[0].local = true;

    RosterTable table;
    RosterCadence cad = {};
    RosterScanReport rep = {};

    // The open-time scan, which is the one caller that is NOT gated: a file's manifest may not be
    // built out of a cadence.
    const uint64_t t0 = 1000000;
    REQUIRE(ScanRosterOnChange(&src, table, cad, t0, kRosterRescanIntervalUs, true, rep));
    CHECK(rep.appended == 1);
    CHECK(table.Count() == 1);
    const uint32_t fpAfterOpen = src.fingerprintCalls;

    // 1489 ticks at 90 Hz -- the launch L2-RUN-5 measured, tick for tick.
    const uint32_t kTicks = 1489;
    const uint64_t kTickUs = 11111;
    uint32_t scans = 0, gated = 0;
    for (uint32_t i = 1; i <= kTicks; ++i) {
        if (ScanRosterOnChange(&src, table, cad, t0 + uint64_t(i) * kTickUs,
                               kRosterRescanIntervalUs, false, rep)) {
            ++scans;
        } else {
            CHECK(rep.signalUnchanged == 1);
            ++gated;
        }
    }

    // 1489 ticks span 16.5 s, so the 1 Hz cadence is the ONLY thing that may scan: nobody joined,
    // nobody left, and the engine's count never moved.
    CHECK(scans >= 15);
    CHECK(scans <= 18);
    CHECK(gated == kTicks - scans);
    // ★ THE ASSERTION THE OLD CODE FAILS. Before the gate this read `scans == 1489`, which is the
    // sentinel's `unchangedRowScans: 1489` said from the other side.
    CHECK(scans * 50 < kTicks);
    // The nine per-user reads happen ONCE PER SCAN and never on a gated tick. This is the field
    // cost: a fingerprint is a name read, a team pair, twenty item ids and the loadout numbers.
    CHECK(src.fingerprintCalls == fpAfterOpen + scans);
    // And what a gated tick DOES cost: exactly one engine question. A scanning tick asks twice --
    // the gate's, then ScanRoster's own -- which is the two loads the budget is stated in.
    CHECK(src.bandCalls == 1u + 1u + gated + scans * 2u);
}

TEST_CASE("roster: a join is picked up on the very next tick, not at the next cadence") {
    CountingSource src;
    src.rows.push_back(MakeRow(0, "Solo Host", kIdBase + 11, 0));
    // The fixture's band is a MEMBER, not rows.size(): the gate's signal is the ENGINE's count,
    // so a fixture that pushes a row without moving it is not a join the engine can see.
    src.band = 1;

    RosterTable table;
    RosterCadence cad = {};
    RosterScanReport rep = {};
    const uint64_t t0 = 1000000;
    REQUIRE(ScanRosterOnChange(&src, table, cad, t0, kRosterRescanIntervalUs, true, rep));

    // Ten quiet ticks: the gate holds, and the table does not move.
    for (uint32_t i = 1; i <= 10; ++i) {
        CHECK_FALSE(ScanRosterOnChange(&src, table, cad, t0 + uint64_t(i) * 11111,
                                       kRosterRescanIntervalUs, false, rep));
    }
    CHECK(table.Count() == 1);

    // Somebody joins. The engine's own count moves, which is the signal -- and the roster must not
    // wait out the cadence for a player who is in the match NOW.
    src.rows.push_back(MakeRow(1, "Joiner", kIdBase + 12, 1));
    src.band = 2;
    REQUIRE(ScanRosterOnChange(&src, table, cad, t0 + 11u * 11111, kRosterRescanIntervalUs, false,
                               rep));
    CHECK(rep.appended == 1);
    CHECK(table.Count() == 2);

    // And a count the engine will not give up is an EDGE too, not a reason to hold: the answer
    // changed from a number to a refusal, which is exactly what a roster has to notice.
    src.bandReadable = false;
    REQUIRE(ScanRosterOnChange(&src, table, cad, t0 + 12u * 11111, kRosterRescanIntervalUs, false,
                               rep));
    CHECK(rep.bandRefused == 1);
}

TEST_CASE("fill_bit_distinguishes_empty_from_absent") {
    // Three states, and a two-state mask cannot hold them: the source answered with keys; the source
    // answered items only (every source that predates W2-C-2); the source answered with keys that
    // are all zero. Only the middle one may leave kRosterFillLoadoutKeys clear -- a recording that
    // says "recorded, and empty" is a different claim from one that says "never recorded", and the
    // second is the one the loadout resolver has to refuse rather than guess through.
    {
        FakeSource src;
        src.rows.push_back(MakeRow(0, "Keyed", kIdBase + 1, 0));
        RosterTable table;
        const RosterScanReport rep = ScanRoster(&src, table, kTick);
        const RosterRow* r = RowNamed(table, "Keyed");
        REQUIRE(r != nullptr);
        CHECK((r->fill & kRosterFillLoadout) != 0);
        CHECK((r->fill & kRosterFillLoadoutKeys) != 0);
        CHECK(r->loadoutSlotKeys[7] == KeyFor(0, 7));
        CHECK(rep.refusals.byReason[size_t(RosterRefusal::LoadoutKeys)] == 0);
    }
    {
        FakeSource src;
        FakeRow row = MakeRow(0, "Items Only", kIdBase + 2, 0);
        row.hasKeys = false;
        src.rows.push_back(row);
        RosterTable table;
        const RosterScanReport rep = ScanRoster(&src, table, kTick);
        const RosterRow* r = RowNamed(table, "Items Only");
        REQUIRE(r != nullptr);
        CHECK((r->fill & kRosterFillLoadout) != 0);       // the items are still recorded
        CHECK((r->fill & kRosterFillLoadoutKeys) == 0);   // and the slots they sit in are not
        CHECK(r->loadoutSlots[7] == ItemFor(0, 7));
        for (int i = 0; i < 20; ++i) CHECK(r->loadoutSlotKeys[i] == 0);
        CHECK(rep.refusals.byReason[size_t(RosterRefusal::LoadoutKeys)] == 1);
        CHECK(strcmp(RosterRefusalName(RosterRefusal::LoadoutKeys), "loadoutKeys") == 0);
    }
    {
        FakeSource src;
        FakeRow row = MakeRow(0, "Empty Keys", kIdBase + 3, 0);
        for (int i = 0; i < 20; ++i) row.keys[i] = 0;
        src.rows.push_back(row);
        RosterTable table;
        const RosterScanReport rep = ScanRoster(&src, table, kTick);
        const RosterRow* r = RowNamed(table, "Empty Keys");
        REQUIRE(r != nullptr);
        CHECK((r->fill & kRosterFillLoadoutKeys) != 0);   // asked and answered, with nothing in it
        for (int i = 0; i < 20; ++i) CHECK(r->loadoutSlotKeys[i] == 0);
        CHECK(rep.refusals.byReason[size_t(RosterRefusal::LoadoutKeys)] == 0);
    }
}

// ---------------------------------------------------------------------------------------------
// THE PRODUCER.  ENG-W2-C-2 s6 measured that nothing wrote this block into the stream, so
// these cases drive the real emitter -- fed by the real ScanRoster off a fixture whose slot KEY is
// nothing like its position -- and read the pushed bytes back through W2-C-2's own reader.  A test
// that built the block by hand here would be certifying its own copy of the format.
// ---------------------------------------------------------------------------------------------
namespace {

struct EmitSinkFixture {
    std::vector<std::vector<uint8_t> > blocks;
    std::vector<IdentityEmitCensus> lines;
    bool refuse = false;
};

bool CollectBlock(const void* p, uint32_t n, uint64_t /*tUs*/, void* ctx) {
    EmitSinkFixture* f = static_cast<EmitSinkFixture*>(ctx);
    if (f->refuse) return false;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    f->blocks.push_back(std::vector<uint8_t>(b, b + n));
    return true;
}

void CollectCensus(const IdentityEmitCensus& line, void* ctx) {
    static_cast<EmitSinkFixture*>(ctx)->lines.push_back(line);
}

// The buffers the tick thread lends the emitter, at the sizes capture.cpp lends them.
struct EmitScratchFixture {
    std::vector<uint8_t> block = std::vector<uint8_t>(kIdentityBlockOneEntrantCap);
    std::vector<uint8_t> json = std::vector<uint8_t>(kRosterJsonCap);
    IdentityEmitScratch Get() {
        IdentityEmitScratch s;
        s.block = block.data();
        s.blockCap = block.size();
        s.json = json.data();
        s.jsonCap = json.size();
        return s;
    }
};

// The emitter's retry clock, which is NOT the tUs a block is stamped with (CAP-COST). A case that
// needs a refused profile re-asked moves this one; without it the emitter reads the system clock,
// on which a test is over before the first back-off is.
struct RetryClockFixture {
    uint64_t nowUs = 0;
    static uint64_t Read(void* ctx) { return static_cast<RetryClockFixture*>(ctx)->nowUs; }
    void Attach(IdentityEmitter& em) { em.SetRetryClock(&Read, this); }
};

}  // namespace

TEST_CASE("identity_block_from_the_real_producer_round_trips_by_key") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "Blue One", kIdBase + 1, 0));
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 1);
    REQUIRE(table.Count() == 1);
    REQUIRE((table.At(0).fill & kRosterFillLoadoutKeys) != 0);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    CHECK(rep.considered == 1);
    CHECK(rep.emitted == 1);
    CHECK(rep.unchanged == 0);
    CHECK(rep.keyless == 0);
    CHECK(rep.RefusalTotal() == 0);
    REQUIRE(out.blocks.size() == 1);

    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[0].data(), out.blocks[0].size()));
    CHECK(r.Version() == kIdentityBlockVersion);
    CHECK(r.HasSlotKeys());
    CHECK(r.Count() == 1);
    CHECK(r.SlotCount() == kIdentitySlotCount);
    CHECK(r.SlotStride() == sizeof(IdentityLoadoutSlot));
    CHECK(r.TimeUs() == kTick);

    IdentityBlockEntrantView v;
    REQUIRE(r.Entrant(0, v));
    CHECK(strcmp(v.fields.displayName, "Blue One") == 0);
    CHECK(v.fields.userSlot == table.At(0).userSlot);
    CHECK(v.fields.teamId == 0);
    CHECK(v.fields.loadoutNumber == uint16_t(kAlignSeed));
    // The profile document, whole and parsable: a JSON cut to fit a buffer parses as nothing.
    REQUIRE(v.jsonLen == src.rows[0].entrantJson.size());
    CHECK(memcmp(v.json, src.rows[0].entrantJson.data(), v.jsonLen) == 0);
    CHECK(v.json[0] == '{');

    // THE POINT OF THE PACKET: the pairs come back keyed by the engine's own key, and the key is not
    // the position -- KeyFor reverses the slot order, so a producer that recorded position would
    // agree here only by also reversing.
    IdentityLoadoutSlot got[kIdentitySlotCount] = {};
    uint16_t n = 0;
    REQUIRE(r.ResolveLoadout(0, got, n));
    CHECK(n == kIdentitySlotCount);
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        CHECK(got[i].slotKey == KeyFor(0, i));
        CHECK(got[i].itemId == ItemFor(0, i));
        CHECK(got[i].slotKey != i);
    }

    // ... and the acceptance arithmetic agrees, compared against what the source holds.
    replay::identity_access::LoadoutPair engine[kIdentitySlotCount] = {};
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        engine[i].slotKey = src.rows[0].keys[i];
        engine[i].itemId = src.rows[0].items[i];
    }
    const LoadoutMatch m = CompareLoadoutByKey(got, n, engine, kIdentitySlotCount);
    CHECK(m.compared == kIdentitySlotCount);
    CHECK(m.matched == kIdentitySlotCount);
    CHECK(m.mismatched == 0);
    CHECK(m.missing == 0);

    // The census line the capture log prints, and its numbers are read out of the pushed bytes.
    REQUIRE(out.lines.size() == 1);
    CHECK(out.lines[0].version == kIdentityBlockVersion);
    CHECK(out.lines[0].slotsWritten == kIdentitySlotCount);
    CHECK(out.lines[0].slotsWithKey == kIdentitySlotCount);
    CHECK(out.lines[0].userSlot == table.At(0).userSlot);
    CHECK(out.lines[0].jsonBytes == uint32_t(src.rows[0].entrantJson.size()));
    CHECK(out.lines[0].blockBytes == uint32_t(out.blocks[0].size()));
    // Masked: enough to tell two entrants apart, never the name itself.
    CHECK(strstr(out.lines[0].nameMasked, "lue One") == nullptr);
    CHECK(out.lines[0].nameMasked[0] == 'B');
}

TEST_CASE("identity_block_is_written_on_change_and_a_late_loadout_change_lands") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "Blue One", kIdBase + 1, 0));
    RosterTable table;
    RosterCadence cad = {};
    RosterScanReport scan;
    REQUIRE(ScanRosterOnChange(&src, table, cad, kTick, kRosterRescanIntervalUs, true, scan));

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    REQUIRE(rep.emitted == 1);
    const int jsonReadsAfterFirst = src.entrantJsonReads;
    CHECK(jsonReadsAfterFirst == 1);

    // A quiet second: the scan runs again on the cadence, nothing moved, and NOTHING is pushed --
    // this is the difference between a producer and per-tick spam.
    const uint64_t later = kTick + kRosterRescanIntervalUs;
    REQUIRE(ScanRosterOnChange(&src, table, cad, later, kRosterRescanIntervalUs, false, scan));
    em.EmitChanged(table, &src, later, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    CHECK(rep.considered == 1);
    CHECK(rep.emitted == 0);
    CHECK(rep.unchanged == 1);
    CHECK(out.blocks.size() == 1);
    // The document is NOT re-serialised on a pass that pushed nothing -- the instrument would
    // otherwise cost a CJson walk per entrant per second for a block nobody asked for.
    CHECK(src.entrantJsonReads == jsonReadsAfterFirst);

    // A LATE LOADOUT CHANGE.  The entrant count does not move, so the signal gate holds; the cadence
    // is what brings the scan, the fingerprint moves with the item, and the block follows it.
    const uint64_t changedItem = ItemFor(0, 3) + 0x777ull;
    src.rows[0].items[3] = changedItem;
    const uint64_t muchLater = later + kRosterRescanIntervalUs;
    REQUIRE(ScanRosterOnChange(&src, table, cad, muchLater, kRosterRescanIntervalUs, false, scan));
    CHECK(scan.updated == 1);
    em.EmitChanged(table, &src, muchLater, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus,
                   &out, rep);
    CHECK(rep.emitted == 1);
    REQUIRE(out.blocks.size() == 2);
    // ... and it IS re-read for the row that moved, so a profile edited with the loadout lands.
    CHECK(src.entrantJsonReads == jsonReadsAfterFirst + 1);

    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[1].data(), out.blocks[1].size()));
    IdentityLoadoutSlot got[kIdentitySlotCount] = {};
    uint16_t n = 0;
    REQUIRE(r.ResolveLoadout(0, got, n));
    CHECK(got[3].slotKey == KeyFor(0, 3));   // the same slot ...
    CHECK(got[3].itemId == changedItem);     // ... now holding the new item
    CHECK(got[4].itemId == ItemFor(0, 4));
    CHECK(r.TimeUs() == muchLater);
}

TEST_CASE("identity_block_records_a_keyless_source_as_empty_never_as_position") {
    FakeSource src;
    FakeRow row = MakeRow(0, "Items Only", kIdBase + 2, 1);
    row.hasKeys = false;   // every source that predates W2-C-2
    src.rows.push_back(row);
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 1);
    REQUIRE((table.At(0).fill & kRosterFillLoadoutKeys) == 0);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    CHECK(rep.emitted == 1);
    CHECK(rep.keyless == 1);
    REQUIRE(out.blocks.size() == 1);
    REQUIRE(out.lines.size() == 1);
    CHECK(out.lines[0].slotsWithKey == 0);
    CHECK(out.lines[0].slotsWritten == kIdentitySlotCount);

    // The name and the items are still recorded -- refusing the whole block would throw away
    // identity the capture genuinely holds -- but NO slot key is invented from the position.
    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[0].data(), out.blocks[0].size()));
    IdentityBlockEntrantView v;
    REQUIRE(r.Entrant(0, v));
    CHECK(strcmp(v.fields.displayName, "Items Only") == 0);
    IdentityLoadoutSlot got[kIdentitySlotCount] = {};
    uint16_t n = 0;
    REQUIRE(r.ResolveLoadout(0, got, n));
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        CHECK(got[i].itemId == ItemFor(0, i));
        CHECK(got[i].slotKey == 0);
    }
    replay::identity_access::LoadoutPair engine[kIdentitySlotCount] = {};
    for (uint16_t i = 0; i < kIdentitySlotCount; ++i) {
        engine[i].slotKey = KeyFor(0, i);
        engine[i].itemId = ItemFor(0, i);
    }
    // Nothing is compared, so nothing passes: an unkeyed recording cannot close the acceptance.
    CHECK(CompareLoadoutByKey(got, n, engine, kIdentitySlotCount).compared == 0);
}

TEST_CASE("identity_block_marks_a_component_wider_than_the_format_as_truncated") {
    // A 21-entry component against a 20-slot format: the twenty recorded pairs are
    // true, and a block that did not say so would read as a complete loadout.
    FakeSource wide;
    FakeRow wideRow = MakeRow(0, "Wide Load", kIdBase + 9, 0);
    wideRow.hasAvailable = true;
    wideRow.available = kIdentitySlotCount + 1;
    wide.rows.push_back(wideRow);
    RosterTable table;
    REQUIRE(ScanRoster(&wide, table, kTick).present == 1);
    REQUIRE((table.At(0).fill & kRosterFillLoadoutAvail) != 0);
    CHECK(table.At(0).loadoutSlotsAvailable == kIdentitySlotCount + 1);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &wide, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    REQUIRE(rep.emitted == 1);
    CHECK(rep.truncated == 1);
    CHECK(rep.truncationUnknown == 0);
    REQUIRE(out.lines.size() == 1);
    CHECK(out.lines[0].truncationKnown == 1);
    CHECK(out.lines[0].truncated == 1);
    CHECK(out.lines[0].slotsAvailable == kIdentitySlotCount + 1);

    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[0].data(), out.blocks[0].size()));
    bool truncated = false;
    REQUIRE(r.LoadoutTruncated(0, truncated));
    CHECK(truncated);
    CHECK(r.Refusal() == IdentityBlockRefusal::SlotsTruncated);
    CHECK(strcmp(IdentityBlockRefusalText(r.Refusal()), "LoadoutSlotsTruncated") == 0);
    // The twenty it did record are still true and still keyed.
    IdentityLoadoutSlot got[kIdentitySlotCount] = {};
    uint16_t n = 0;
    REQUIRE(r.ResolveLoadout(0, got, n));
    CHECK(got[0].slotKey == KeyFor(0, 0));

    // EXACTLY the width the format holds is NOT truncation, and it is a different answer again from
    // a source that cannot say -- three states, not two.
    {
        FakeSource exact;
        FakeRow exactRow = MakeRow(1, "Exact Load", kIdBase + 10, 1);
        exactRow.hasAvailable = true;
        exactRow.available = kIdentitySlotCount;
        exact.rows.push_back(exactRow);
        RosterTable t2;
        REQUIRE(ScanRoster(&exact, t2, kTick).present == 1);
        EmitSinkFixture o2;
        IdentityEmitter e2;
        IdentityEmitReport r2;
        e2.EmitChanged(t2, &exact, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &o2, &CollectCensus, &o2,
                       r2);
        REQUIRE(r2.emitted == 1);
        CHECK(r2.truncated == 0);
        CHECK(r2.truncationUnknown == 0);
        IdentityBlockReader r3;
        REQUIRE(r3.Open(o2.blocks[0].data(), o2.blocks[0].size()));
        bool t = true;
        REQUIRE(r3.LoadoutTruncated(0, t));
        CHECK(!t);
    }
    {
        // The engine-backed source of this build: it cannot ask, so the block says UNKNOWN and a
        // reader is refused BY NAME rather than handed a "not truncated" nobody established.
        FakeSource cannotAsk;
        cannotAsk.rows.push_back(MakeRow(2, "No Width", kIdBase + 11, 1));
        RosterTable t2;
        REQUIRE(ScanRoster(&cannotAsk, t2, kTick).present == 1);
        CHECK((t2.At(0).fill & kRosterFillLoadoutAvail) == 0);
        EmitSinkFixture o2;
        IdentityEmitter e2;
        IdentityEmitReport r2;
        e2.EmitChanged(t2, &cannotAsk, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &o2, &CollectCensus,
                       &o2, r2);
        REQUIRE(r2.emitted == 1);
        CHECK(r2.truncationUnknown == 1);
        CHECK(r2.truncated == 0);
        CHECK(o2.lines[0].truncationKnown == 0);
        IdentityBlockReader r3;
        REQUIRE(r3.Open(o2.blocks[0].data(), o2.blocks[0].size()));
        bool t = true;
        CHECK(!r3.LoadoutTruncated(0, t));
        CHECK(!t);
        CHECK(r3.Refusal() == IdentityBlockRefusal::SlotsTruncationUnknown);
        CHECK(strcmp(IdentityBlockRefusalText(r3.Refusal()), "LoadoutSlotsTruncationUnknown") == 0);
    }
}

TEST_CASE("identity_block_records_the_pair_count_the_scan_actually_got") {
    // ARCH-23 s14.8c F1.  ReadAppliedPairs zeroes its output first, so a component holding
    // seventeen entries reaches disk as seventeen real pairs followed by three zeroed records that
    // are byte-identical to slots the capture LOST.  The count is the only thing that tells them
    // apart, and it is a per-entrant fact: the header's slot count is the array's width.
    FakeSource src;
    FakeRow row = MakeRow(0, "Seventeen", kIdBase + 21, 0);
    row.hasAvailable = true;
    row.available = 17;
    src.rows.push_back(row);
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 1);
    REQUIRE((table.At(0).fill & kRosterFillLoadoutAvail) != 0);
    REQUIRE(table.At(0).loadoutSlotsAvailable == 17);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    REQUIRE(rep.emitted == 1);
    REQUIRE(out.lines.size() == 1);
    CHECK(out.lines[0].filledPairCount == 17);

    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[0].data(), out.blocks[0].size()));
    CHECK(r.Version() == kIdentityBlockVersion);
    CHECK(r.HasPairCount());
    CHECK(r.EntrantStride() == sizeof(IdentityEntrant));
    uint16_t filled = 0;
    REQUIRE(r.FilledPairCount(0, filled));
    CHECK(filled == 17);
    CHECK(filled != kIdentitySlotCount);      // not the array's width, which is what 20 would be
    CHECK(filled != kIdentityPairCountUnknown);
    IdentityBlockEntrantView v{};
    REQUIRE(r.Entrant(0, v));
    CHECK(v.fields.filledPairCount == 17);
    // The pairs themselves are unchanged by the count: all twenty records are still returned, and
    // the count says how many of them the engine actually held.
    IdentityLoadoutSlot got[kIdentitySlotCount] = {};
    uint16_t n = 0;
    REQUIRE(r.ResolveLoadout(0, got, n));
    CHECK(n == kIdentitySlotCount);
    CHECK(got[0].slotKey == KeyFor(0, 0));
    // ...and a component WIDER than the format contributes the format's twenty, never twenty-one:
    // the count is a denominator for the records that are there.
    {
        FakeSource wide;
        FakeRow wideRow = MakeRow(1, "Twenty One", kIdBase + 22, 1);
        wideRow.hasAvailable = true;
        wideRow.available = kIdentitySlotCount + 1;
        wide.rows.push_back(wideRow);
        RosterTable t2;
        REQUIRE(ScanRoster(&wide, t2, kTick).present == 1);
        EmitSinkFixture o2;
        IdentityEmitter e2;
        IdentityEmitReport r2;
        e2.EmitChanged(t2, &wide, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &o2, &CollectCensus, &o2,
                       r2);
        REQUIRE(r2.emitted == 1);
        IdentityBlockReader r3;
        REQUIRE(r3.Open(o2.blocks[0].data(), o2.blocks[0].size()));
        uint16_t wideFilled = 0;
        REQUIRE(r3.FilledPairCount(0, wideFilled));
        CHECK(wideFilled == kIdentitySlotCount);
        bool truncated = false;
        REQUIRE(r3.LoadoutTruncated(0, truncated));
        CHECK(truncated);        // and the truncation bit is what says twenty is not all of them
    }
}

TEST_CASE("identity_block_pair_count_is_unknown_when_the_scan_could_not_ask") {
    // The third state, and the reason it is not zero: a source that cannot report the component's
    // width leaves kRosterFillLoadoutAvail clear, and a zero here would be the answer "the engine
    // held nothing" -- which a verification would compare nothing against and call a pass.
    FakeSource src;
    src.rows.push_back(MakeRow(0, "No Width", kIdBase + 23, 0));
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 1);
    REQUIRE((table.At(0).fill & kRosterFillLoadoutAvail) == 0);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    REQUIRE(rep.emitted == 1);
    REQUIRE(out.lines.size() == 1);
    CHECK(out.lines[0].filledPairCount == kIdentityPairCountUnknown);

    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[0].data(), out.blocks[0].size()));
    uint16_t filled = 0;
    REQUIRE(r.FilledPairCount(0, filled));
    CHECK(filled == kIdentityPairCountUnknown);
    CHECK(filled != 0);
    // Everything else about the row is still recorded: an unknown denominator is not a broken block.
    IdentityBlockEntrantView v{};
    REQUIRE(r.Entrant(0, v));
    CHECK(strcmp(v.fields.displayName, "No Width") == 0);
    IdentityLoadoutSlot got[kIdentitySlotCount] = {};
    uint16_t n = 0;
    REQUIRE(r.ResolveLoadout(0, got, n));
    CHECK(got[0].itemId == ItemFor(0, 0));
}

TEST_CASE("identity_block_refused_by_the_ring_is_retried_not_forgotten") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "Blue One", kIdBase + 1, 0));
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 1);

    EmitSinkFixture out;
    out.refuse = true;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    CHECK(rep.emitted == 0);
    CHECK(rep.refusals[size_t(IdentityEmitRefusal::SinkRefused)] == 1);
    CHECK(out.blocks.empty());
    uint64_t published = 0;
    CHECK(!em.PublishedSignature(0, published));
    CHECK(strcmp(IdentityEmitRefusalText(IdentityEmitRefusal::SinkRefused),
                 "IdentityEmitSinkRefused") == 0);

    // The ring recovers, and the row is still owed a block: a producer that latched on the attempt
    // would leave this entrant out of the file for the rest of the match.
    out.refuse = false;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out,
                   rep);
    CHECK(rep.emitted == 1);
    CHECK(out.blocks.size() == 1);
    CHECK(em.PublishedSignature(0, published));

    // And a buffer too small for one entrant's block is its own word, not a silent nothing.
    IdentityEmitScratch small = scratch.Get();
    small.blockCap = kIdentityBlockOneEntrantCap - 1;
    IdentityEmitter em2;
    em2.EmitChanged(table, &src, kTick, 0, small, kRosterMaxRows, &CollectBlock, &out, &CollectCensus, &out, rep);
    CHECK(rep.emitted == 0);
    CHECK(rep.refusals[size_t(IdentityEmitRefusal::ScratchTooSmall)] == 1);
}

TEST_CASE("identity_block_tells_a_refused_profile_from_an_absent_one") {
    // Both are zero bytes of JSON. Only the flags separate them, and a reader that
    // cannot separate them tells the viewer this player has no profile when the engine refused one.
    FakeSource src;
    FakeRow present = MakeRow(0, "Has Doc", kIdBase + 20, 0);
    FakeRow empty = MakeRow(1, "Empty Doc", kIdBase + 21, 0);
    empty.entrantJson = "";                 // asked, answered, and the document is genuinely empty
    FakeRow refused = MakeRow(2, "No Doc", kIdBase + 22, 1);
    refused.hasEntrantJson = false;         // asked, and the engine said no
    src.rows.push_back(present);
    src.rows.push_back(empty);
    src.rows.push_back(refused);
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 3);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    REQUIRE(rep.emitted == 3);
    CHECK(rep.jsonRefused == 1);
    REQUIRE(rep.lastJsonWhy != nullptr);
    CHECK(std::string(rep.lastJsonWhy) == "EmptyProfile");   // the ENGINE's word, not ours

    bool p0 = false, p1 = false, p2 = false;
    IdentityBlockReader r0, r1, r2;
    REQUIRE(r0.Open(out.blocks[0].data(), out.blocks[0].size()));
    REQUIRE(r1.Open(out.blocks[1].data(), out.blocks[1].size()));
    REQUIRE(r2.Open(out.blocks[2].data(), out.blocks[2].size()));

    REQUIRE(r0.ProfileRecorded(0, p0));
    CHECK(p0);                                   // present
    CHECK(out.lines[0].profileRefused == 0);
    CHECK(out.lines[0].jsonBytes > 0);

    REQUIRE(r1.ProfileRecorded(0, p1));
    CHECK(!p1);                                  // ABSENT: asked, not refused, zero bytes
    CHECK(out.lines[1].profileAsked == 1);
    CHECK(out.lines[1].profileRefused == 0);

    CHECK(!r2.ProfileRecorded(0, p2));           // REFUSED: a different answer, by name
    CHECK(r2.Refusal() == IdentityBlockRefusal::ProfileRefused);
    CHECK(strcmp(IdentityBlockRefusalText(r2.Refusal()), "IdentityProfileRefused") == 0);
    CHECK(out.lines[2].profileRefused == 1);
    CHECK(std::string(out.lines[2].profileWhy) == "EmptyProfile");

    // A block from before these bits -- v1's zero byte -- is "never asked", never "no profile".
    IdentityEntrant old{};
    IdentityBlockEntrantIn in{};
    in.fields = old;
    std::vector<uint8_t> buf(kIdentityBlockOneEntrantCap);
    IdentityBlockWriter w(buf.data(), buf.size(), kTick, 0);
    REQUIRE(w.Add(in));
    size_t bytes = 0;
    REQUIRE(w.Finish(bytes));
    IdentityBlockReader r3;
    REQUIRE(r3.Open(buf.data(), bytes));
    bool p3 = true;
    CHECK(!r3.ProfileRecorded(0, p3));
    CHECK(r3.Refusal() == IdentityBlockRefusal::ProfileNotAsked);
}

TEST_CASE("identity_block_budget_defers_rows_it_does_not_drop") {
    // A round transition moves every entrant at once. The armed tick pays for one
    // profile document per pass and offers the rest again next scan -- deferred, never dropped.
    FakeSource src;
    for (uint16_t i = 0; i < 4; ++i) {
        src.rows.push_back(MakeRow(i, i == 0 ? "One" : (i == 1 ? "Two" : (i == 2 ? "Three" : "Four")),
                                   kIdBase + 30 + i, uint16_t(i % 2)));
    }
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 4);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), 1, &CollectBlock, &out, &CollectCensus,
                   &out, rep);
    CHECK(rep.considered == 4);
    CHECK(rep.emitted == 1);
    CHECK(rep.deferred == 3);
    CHECK(src.entrantJsonReads == 1);   // ONE document walked on this pass, which is the budget

    // Four passes, four blocks, nobody lost -- and the signatures latch one at a time.
    for (int pass = 0; pass < 3; ++pass) {
        em.EmitChanged(table, &src, kTick, 0, scratch.Get(), 1, &CollectBlock, &out, &CollectCensus,
                       &out, rep);
        CHECK(rep.emitted == 1);
    }
    CHECK(out.blocks.size() == 4);
    CHECK(src.entrantJsonReads == 4);
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), 1, &CollectBlock, &out, &CollectCensus,
                   &out, rep);
    CHECK(rep.emitted == 0);
    CHECK(rep.deferred == 0);
    CHECK(rep.unchanged == 4);
}

TEST_CASE("identity_emitter_cost_per_entrant_is_measured_not_assumed") {
    // AUD-W2-A-2 F4's other half. This is the cost the PRODUCER adds to the armed tick with the
    // document already in hand: the fold, the block build and the push. The engine's own CJson walk
    // is not here and cannot be -- BuildMatches() is false off a real image -- which is exactly why
    // the emitter now has its own cost-meter bracket for the live run to read.
    FakeSource src;
    FakeRow row = MakeRow(0, "Full Doc", kIdBase + 40, 0);
    row.entrantJson = std::string(kRosterJsonCap - 2, 'x');   // a full-width document
    row.entrantJson[0] = '{';
    row.entrantJson[row.entrantJson.size() - 1] = '}';
    src.rows.push_back(row);
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 1);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitReport rep;
    const int kPasses = 200;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPasses; ++i) {
        IdentityEmitter em;   // a fresh emitter each pass, so every pass actually writes a block
        em.EmitChanged(table, &src, kTick + uint64_t(i), 0, scratch.Get(), kRosterMaxRows,
                       &CollectBlock, &out, &CollectCensus, &out, rep);
    }
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() /
        double(kPasses);
    CHECK(out.blocks.size() == size_t(kPasses));
    MESSAGE("identity emitter: " << us << " us per entrant (block build + 4 KB document copy + push)");
    // The gather budget the cost meter kills on is tens of microseconds per tick; this asserts the
    // producer's own share is inside it with room, and PRINTS the number so it is a measurement
    // rather than a claim.
    CHECK(us < 30.0);
}

TEST_CASE("the scan walks the customisation component once per entrant") {
    // The component walk is the expensive read in the roster's bracket -- the one
    // roster.h:333-341 and costmeter.cpp:225-232 record as having closed two captures -- and asking
    // one call for the pairs and a second for the component's width doubled it per entrant per scan.
    // The keys, the items and the width now come out of ONE call, and this counts them.
    FakeSource src;
    const uint16_t kEntrants = 12;   // a full Echo Arena lobby
    for (uint16_t i = 0; i < kEntrants; ++i) {
        FakeRow row = MakeRow(i, ("Player" + std::to_string(i)).c_str(), kIdBase + 60 + i,
                              uint16_t(i % 2));
        row.hasAvailable = true;
        row.available = kIdentitySlotCount;
        src.rows.push_back(row);
    }
    src.band = kEntrants;
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == kEntrants);
    CHECK(src.pairWalks == int(kEntrants));          // ONE per entrant, not two
    REQUIRE(table.Count() == kEntrants);
    // ... and the width still arrives, so this is not "one walk" bought by dropping the answer.
    for (uint16_t i = 0; i < kEntrants; ++i) {
        CHECK((table.At(i).fill & kRosterFillLoadoutAvail) != 0);
        CHECK(table.At(i).loadoutSlotsAvailable == kIdentitySlotCount);
        CHECK(table.At(i).loadoutSlotKeys[3] == KeyFor(table.At(i).userIndex, 3));
    }
}

TEST_CASE("a profile that becomes readable re-emits instead of standing refused") {
    // The signature is folded before the profile flags exist, so a row whose document
    // was refused folds identically once it becomes readable -- and a latched ProfileRefused would
    // then stand for the rest of the match on a profile the engine now hands over.
    FakeSource src;
    FakeRow row = MakeRow(0, "Late Doc", kIdBase + 80, 0);
    row.hasEntrantJson = false;
    src.rows.push_back(row);
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 1);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    RetryClockFixture clock;
    clock.Attach(em);
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    REQUIRE(rep.emitted == 1);
    CHECK(rep.jsonRefused == 1);

    // Still refused, nothing else moved: no second identical block, and the retry is REPORTED.
    // EmptyProfile is a transient refusal, so the re-ask comes when its back-off expires (CAP-COST;
    // test_identity_retry.cpp owns the schedule) rather than on the very next scan.
    clock.nowUs += kProfileRetryFirstDelayUs;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    CHECK(rep.emitted == 0);
    CHECK(rep.profileRetried == 1);
    CHECK(out.blocks.size() == 1);

    // The engine hands the document over. NOTHING else about the row changed, so only the profile
    // flags can carry this -- and the block must be written again.
    src.rows[0].hasEntrantJson = true;
    clock.nowUs += kProfileRetryRosterGapUs;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    CHECK(rep.emitted == 1);
    REQUIRE(out.blocks.size() == 2);
    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[1].data(), out.blocks[1].size()));
    bool present = false;
    REQUIRE(r.ProfileRecorded(0, present));
    CHECK(present);

    // And once it is readable and unchanged, it settles: no per-scan republishing.
    clock.nowUs += kProfileRetryRosterGapUs;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    CHECK(rep.emitted == 0);
    CHECK(rep.unchanged == 1);
    CHECK(rep.profileRetried == 0);
}

TEST_CASE("a component that grows behind the same twenty pairs re-reads the row and re-emits") {
    // AUD-HOP4-BIND F4 / AUD-W2-A-2 F5. The twenty item ids are min(count, 20). An entrant whose
    // component grows from twenty entries to twenty-one changes NONE of them, so without the width
    // in the fingerprint the row is never re-read and every later block keeps saying "not
    // truncated" -- a completeness claim about a loadout the recording no longer holds all of.
    FakeSource src;
    FakeRow row = MakeRow(0, "Grower", kIdBase + 90, 0);
    row.hasAvailable = true;
    row.available = kIdentitySlotCount;
    src.rows.push_back(row);

    RosterTable table;
    RosterCadence cad = {};
    RosterScanReport scan;
    REQUIRE(ScanRosterOnChange(&src, table, cad, kTick, kRosterRescanIntervalUs, true, scan));
    REQUIRE(table.Count() == 1);
    CHECK(table.At(0).loadoutSlotsAvailable == kIdentitySlotCount);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    REQUIRE(rep.emitted == 1);
    CHECK(rep.truncated == 0);

    // The twenty pairs are BYTE-FOR-BYTE what they were; only the count behind them moved.
    const uint64_t before[20] = {};
    (void)before;
    src.rows[0].available = kIdentitySlotCount + 1;
    for (int i = 0; i < 20; ++i) {
        REQUIRE(src.rows[0].items[i] == ItemFor(0, i));   // nothing else changed, by construction
    }
    const uint64_t later = kTick + kRosterRescanIntervalUs;
    REQUIRE(ScanRosterOnChange(&src, table, cad, later, kRosterRescanIntervalUs, false, scan));
    CHECK(scan.updated == 1);        // the FINGERPRINT moved -- this is the assertion F5 exists for
    CHECK(scan.unchanged == 0);
    CHECK(table.At(0).loadoutSlotsAvailable == kIdentitySlotCount + 1);

    em.EmitChanged(table, &src, later, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    CHECK(rep.emitted == 1);
    CHECK(rep.truncated == 1);
    REQUIRE(out.blocks.size() == 2);
    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[1].data(), out.blocks[1].size()));
    bool truncated = false;
    REQUIRE(r.LoadoutTruncated(0, truncated));
    CHECK(truncated);

    // The OTHER edge of the same rule: the width becoming READABLE is itself a change, because the
    // block goes from "we could not ask" to a stated answer.
    FakeSource silent;
    FakeRow quiet = MakeRow(1, "Quiet", kIdBase + 91, 1);
    quiet.hasAvailable = false;
    silent.rows.push_back(quiet);
    RosterTable t2;
    RosterCadence cad2 = {};
    REQUIRE(ScanRosterOnChange(&silent, t2, cad2, kTick, kRosterRescanIntervalUs, true, scan));
    CHECK((t2.At(0).fill & kRosterFillLoadoutAvail) == 0);
    silent.rows[0].hasAvailable = true;
    silent.rows[0].available = kIdentitySlotCount;
    REQUIRE(ScanRosterOnChange(&silent, t2, cad2, kTick + kRosterRescanIntervalUs,
                               kRosterRescanIntervalUs, false, scan));
    CHECK(scan.updated == 1);
    CHECK((t2.At(0).fill & kRosterFillLoadoutAvail) != 0);
}

TEST_CASE("a profile the engine keeps refusing does not starve the other entrants") {
    // The armed tick buys ONE document read per scan, and F7 re-offers a row whose
    // profile was refused. In one pass the retry at index 0 spent that read before any first-time
    // row was reached, so eleven entrants never reached the file at all and `deferredToNextScan`
    // climbed for the whole match. First-time rows go first; a retry gets what is left.
    FakeSource src;
    const uint16_t kEntrants = 12;
    for (uint16_t i = 0; i < kEntrants; ++i) {
        FakeRow row = MakeRow(i, ("Ent" + std::to_string(i)).c_str(), kIdBase + 100 + i,
                              uint16_t(i % 2));
        if (i == 0) row.hasEntrantJson = false;   // permanently refused, at the LOWEST index
        src.rows.push_back(row);
    }
    src.band = kEntrants;
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == kEntrants);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    RetryClockFixture clock;
    clock.Attach(em);
    IdentityEmitReport rep;
    for (uint16_t scan = 0; scan < kEntrants; ++scan) {
        // A roster gap apart, so the refused row's re-ask is due and allowed on EVERY scan after
        // the first: that is what puts it in competition with a first-time row for the one read.
        em.EmitChanged(table, &src, kTick + scan, 0, scratch.Get(), 1, &CollectBlock, &out,
                       &CollectCensus, &out, rep);
        if (scan > 0) CHECK(rep.retryDeferred == 1);
        clock.nowUs += kProfileRetryRosterGapUs;
    }
    // Twelve scans at one document each: every entrant published exactly once, the refused one
    // included (its block is written, marked ProfileRefused).
    CHECK(out.blocks.size() == size_t(kEntrants));
    CHECK(rep.deferred == 0);
    std::vector<uint16_t> slots;
    for (const IdentityEmitCensus& line : out.lines) slots.push_back(line.userSlot);
    std::sort(slots.begin(), slots.end());
    CHECK(std::unique(slots.begin(), slots.end()) == slots.end());   // no entrant published twice
    CHECK(slots.size() == size_t(kEntrants));

    // And it settles: nothing new to say, so no republishing, and the retry -- no first-time row
    // is left to take the read from it -- is still reported.
    em.EmitChanged(table, &src, kTick + 100, 0, scratch.Get(), 1, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    CHECK(rep.emitted == 0);
    CHECK(rep.profileRetried == 1);
    CHECK(out.blocks.size() == size_t(kEntrants));
}

TEST_CASE("no identity source at all is one refusal per row, not one per scan") {
    // AUD-W2-A-2 F10's other half. A null source refuses every row on every scan; retrying a
    // question with nobody to answer it would spend the budget forever and republish nothing.
    FakeSource src;
    for (uint16_t i = 0; i < 3; ++i) {
        src.rows.push_back(MakeRow(i, ("Anon" + std::to_string(i)).c_str(), kIdBase + 120 + i, 0));
    }
    src.band = 3;
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == 3);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    IdentityEmitReport rep;
    em.EmitChanged(table, nullptr, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    CHECK(rep.emitted == 3);
    CHECK(rep.jsonRefused == 3);
    CHECK(out.blocks.size() == 3);

    // Settled: still no source, so nothing is re-asked and nothing is republished.
    em.EmitChanged(table, nullptr, kTick + 1, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    CHECK(rep.emitted == 0);
    CHECK(rep.profileRetried == 0);
    CHECK(rep.unchanged == 3);
    CHECK(out.blocks.size() == 3);

    // The source appears: NOW the question is worth asking again, and the documents land.
    em.EmitChanged(table, &src, kTick + 2, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &out,
                   &CollectCensus, &out, rep);
    CHECK(rep.emitted == 3);
    CHECK(out.blocks.size() == 6);
    IdentityBlockReader r;
    REQUIRE(r.Open(out.blocks[5].data(), out.blocks[5].size()));
    bool present = false;
    REQUIRE(r.ProfileRecorded(0, present));
    CHECK(present);
}

TEST_CASE("every considered row lands in exactly one counter") {
    // Identity_emit.h says a reader can add the counters against `considered`; a
    // retry that missed the budget used to increment nothing at all, so the sum came up short and
    // the missing row was invisible. The starved scan is where that happens, so that is where this
    // is asserted -- not on a quiet roster where everything is `unchanged` anyway.
    FakeSource src;
    const uint16_t kEntrants = 12;
    for (uint16_t i = 0; i < kEntrants; ++i) {
        FakeRow row = MakeRow(i, ("Bal" + std::to_string(i)).c_str(), kIdBase + 140 + i,
                              uint16_t(i % 2));
        if (i == 0) row.hasEntrantJson = false;   // the retry that competes for the budget
        src.rows.push_back(row);
    }
    src.band = kEntrants;
    RosterTable table;
    REQUIRE(ScanRoster(&src, table, kTick).present == kEntrants);

    EmitSinkFixture out;
    EmitScratchFixture scratch;
    IdentityEmitter em;
    RetryClockFixture clock;
    clock.Attach(em);
    IdentityEmitReport rep;
    bool sawRetryDeferred = false;
    for (uint16_t scan = 0; scan < kEntrants + 2; ++scan) {
        // One scan a second, as in the field: the refused row's retry is DUE from scan 1 on, which
        // is what puts it in competition for the one read.
        clock.nowUs = scan * kProfileRetryFirstDelayUs;
        em.EmitChanged(table, &src, kTick + scan, 0, scratch.Get(), 1, &CollectBlock, &out,
                       &CollectCensus, &out, rep);
        INFO("scan " << scan << ": considered " << rep.considered << " emitted " << rep.emitted
                     << " unchanged " << rep.unchanged << " deferred " << rep.deferred
                     << " retryDeferred " << rep.retryDeferred);
        CHECK(rep.considered == kEntrants);
        CHECK(rep.CountsBalance());
        if (rep.retryDeferred) sawRetryDeferred = true;
    }
    // The bucket this finding is about must actually have been exercised, or the identity above
    // holds for a reason that has nothing to do with it.
    CHECK(sawRetryDeferred);

    // A sink that refuses everything is accounted for too: those rows are in the refusal bucket.
    EmitSinkFixture blocked;
    blocked.refuse = true;
    IdentityEmitter em2;
    em2.EmitChanged(table, &src, kTick, 0, scratch.Get(), kRosterMaxRows, &CollectBlock, &blocked,
                    &CollectCensus, &blocked, rep);
    CHECK(rep.considered == kEntrants);
    CHECK(rep.emitted == 0);
    CHECK(rep.CountsBalance());
}

// ================================================================================================
// W2-A-4. AN ENTRANT THAT HAS JOINED NO SIDE MUST BE RECORDED AS NO TEAM, NEVER AS BLUE.
//
// The capture used to write identity_access::TeamOf's 3-bit answer unconditionally, so
// kRosterNoTeam was unreachable from the shipped path and an unseated entrant reached the file as
// team 0. The engine's own criterion -- and every one of these numbers is the engine's, not ours --
// is that the field is a team only below NETGAME.USER_ROLE_BOUND; SR15NetUser's reset writes
// 0xFFFF over the whole packed word, so "not seated" reads back as team 7.
// ================================================================================================
namespace {

// The packed word in the engine's own layout: user slot in bits 0..4, team slot in 5..9, team in
// 10..12. Every shift and mask is the registry's, so this is the layout rather than a copy of it.
uint16_t PackUserWord(uint8_t userSlot, uint8_t teamSlot, uint8_t team) {
    return uint16_t(uint16_t(userSlot & uint8_t(facts::IDENT_USER_SLOT_MASK)) |
                    uint16_t(uint16_t(teamSlot & uint8_t(facts::IDENT_USER_TEAMSLOT_MASK))
                             << facts::IDENT_USER_TEAMSLOT_SHIFT) |
                    uint16_t(uint16_t(team & uint8_t(facts::IDENT_USER_TEAM_MASK))
                             << facts::IDENT_USER_TEAM_SHIFT));
}

// A roster source whose team half is the REAL arithmetic -- identity_access::TeamOf and
// TeamSlotOf reading a plain buffer laid out as CR15NetGame is -- composed by the same
// RosterTeamFromEngine the live EngineRosterSource calls. Everything else stays the fake's, so a
// test can move one entrant's word and watch the recorded row follow.
class PackedWordSource : public FakeSource {
public:
    void* ng = nullptr;

    bool Team(uint16_t idx, uint16_t& team, uint16_t& teamSlot) const override {
        if (!ng) return false;
        RosterTeamFromEngine(identity_access::TeamOf(ng, UserIndex(idx)),
                             identity_access::TeamSlotOf(ng, UserIndex(idx)), team, teamSlot);
        return true;
    }

    // The live source's rule: alignment is per TEAM, so an entrant on none has no colour.
    bool Alignment(uint16_t idx, uint16_t& out) const override {
        uint16_t team = kRosterNoTeam, teamSlot = kRosterNoTeam;
        if (!Team(idx, team, teamSlot) || team == kRosterNoTeam) return false;
        return FakeSource::Alignment(idx, out);
    }

    // The live source folds the team pair into the fingerprint, and the fingerprint is what decides
    // whether the scan re-reads a row at all. Without this the fake's summary would never move when
    // the packed word does, and a test of a seat CHANGE would be measuring the fake.
    bool Fingerprint(uint16_t idx, uint64_t& out) const override {
        if (!FakeSource::Fingerprint(idx, out)) return false;
        uint16_t team = kRosterNoTeam, teamSlot = kRosterNoTeam;
        if (!Team(idx, team, teamSlot)) return false;
        out = RosterFold(out, &team, sizeof team);
        out = RosterFold(out, &teamSlot, sizeof teamSlot);
        return true;
    }
};

constexpr uint8_t kSeatBound = uint8_t(facts::NETGAME_USER_ROLE_BOUND);
constexpr uint8_t kResetTeam = uint8_t(facts::IDENT_USER_TEAM_MASK);   // what the reset word leaves

// SR15NetUser::Reset (sub_140176420) writes EVERY bit of the packed word, so an entrant that
// has joined nothing reads back with each of its three registered fields at its own mask --
// and the team field at its mask is above the bound the engine accepts.
constexpr uint16_t kResetWord = uint16_t(~uint16_t(0));

}  // namespace

TEST_CASE("roster: the seating criterion is the engine's, and it has a boundary") {
    CHECK(RosterTeamSeated(0));
    CHECK(RosterTeamSeated(uint8_t(kSeatBound - 1)));
    CHECK_FALSE(RosterTeamSeated(kSeatBound));
    CHECK_FALSE(RosterTeamSeated(kResetTeam));
}

TEST_CASE("roster: an unseated entrant is recorded as no team, off the real packed word") {
    const uint8_t kSeatedTeam = uint8_t(kSeatBound - 1);   // the last team the engine accepts
    const uint8_t kSeatedSlot = 3;
    const uint16_t kEntrants = 3;

    // A plain buffer standing in for CR15NetGame. The entrant words go where the engine's own
    // accessors look for them: USER_ARRAY_OFF + USER_STRIDE * index.
    std::vector<uint8_t> ng(size_t(facts::USER_ARRAY_OFF) +
                                size_t(facts::USER_STRIDE) * kEntrants + sizeof(uint16_t),
                            0);
    auto put = [&](uint16_t idx, uint16_t word) {
        memcpy(ng.data() + size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * idx, &word,
               sizeof word);
    };
    put(0, PackUserWord(0, kSeatedSlot, kSeatedTeam));
    put(1, kResetWord);                           // SR15NetUser's reset: joined nothing
    put(2, PackUserWord(2, 0, kSeatBound));      // seated on a team the engine does not accept

    // The engine reads first, so the fixture is proved to hold what it claims before anything
    // downstream is asked about it.
    CHECK(identity_access::TeamOf(ng.data(), UserIndex(0)) == kSeatedTeam);
    CHECK(identity_access::TeamSlotOf(ng.data(), UserIndex(0)) == kSeatedSlot);
    CHECK(identity_access::TeamOf(ng.data(), UserIndex(1)) == kResetTeam);
    CHECK(identity_access::TeamOf(ng.data(), UserIndex(2)) == kSeatBound);

    PackedWordSource src;
    src.ng = ng.data();
    src.rows.push_back(MakeRow(0, "seated", kIdBase + 1, 1));
    src.rows.push_back(MakeRow(1, "unseated", kIdBase + 2, 2));
    src.rows.push_back(MakeRow(2, "overbound", kIdBase + 3, 3));
    src.band = kEntrants;

    RosterTable table;
    ScanRoster(&src, table, kTick);
    REQUIRE(table.Count() == kEntrants);
    REQUIRE(table.At(0).userIndex == 0);
    REQUIRE(table.At(1).userIndex == 1);

    PlayerRecord seated, unseated, overbound;
    FillPlayerRecord(table.At(0), seated);
    FillPlayerRecord(table.At(1), unseated);
    FillPlayerRecord(table.At(2), overbound);

    CHECK(seated.teamId16 == uint16_t(kSeatedTeam));
    CHECK(seated.teamSlot16 == uint16_t(kSeatedSlot));
    CHECK(seated.teamId == kSeatedTeam);
    CHECK((seated.fill & kPlayerFillTeam) != 0);
    CHECK((seated.fill & kPlayerFillAlign) != 0);

    // The whole point: no team, and the fill bit still SET, because the read happened and its
    // answer was "this entrant is on no side". Team 0 here would be blue.
    CHECK(unseated.teamId16 == kPlayerNoTeam16);
    CHECK(unseated.teamSlot16 == kPlayerNoTeam16);
    CHECK(unseated.teamId != 0);
    CHECK(unseated.teamId == 0xFF);
    CHECK((unseated.fill & kPlayerFillTeam) != 0);
    // No team means no colour: the alignment table is indexed by team id.
    CHECK(unseated.alignment16 == kPlayerNoAlignment16);
    CHECK((unseated.fill & kPlayerFillAlign) == 0);

    CHECK(overbound.teamId16 == kPlayerNoTeam16);
    CHECK(overbound.teamSlot16 == kPlayerNoTeam16);

    // AND THE MAP AT THE OTHER END. The shipped JoinDescFromRecord turns the recorded team into the
    // role the provision issues; the sentinel must survive it unmapped, which is what makes
    // Participate refuse TeamNotRecorded instead of seating this entrant on blue.
    // `extended` is a property of the FILE rather than of the row: the reader sets it from the
    // record's own extended byte count (manifest_io.cpp:394), and every record this capture writes
    // carries the 16-bit fields. JoinDescFromRecord consults teamId16 only when it is set.
    seated.extended = 1;
    unseated.extended = 1;
    const JoinDesc seatedDesc = JoinDescFromRecord(seated);
    CHECK(seatedDesc.team == uint16_t(kSeatedTeam));
    CHECK(seatedDesc.team != uint16_t(PawnProvision::kRoleNotRecorded));

    const JoinDesc noneDesc = JoinDescFromRecord(unseated);
    CHECK(noneDesc.team == kPlayerNoTeam16);
    CHECK(noneDesc.team == uint16_t(PawnProvision::kRoleNotRecorded));
}

TEST_CASE("roster: joining a side after the first scan is a team change, not a silent zero") {
    const uint16_t kEntrants = 1;
    std::vector<uint8_t> ng(size_t(facts::USER_ARRAY_OFF) +
                                size_t(facts::USER_STRIDE) * kEntrants + sizeof(uint16_t),
                            0);
    auto put = [&](uint16_t word) {
        memcpy(ng.data() + size_t(facts::USER_ARRAY_OFF), &word, sizeof word);
    };
    put(kResetWord);

    PackedWordSource src;
    src.ng = ng.data();
    src.rows.push_back(MakeRow(0, "joiner", kIdBase + 7, 1));
    src.band = kEntrants;

    RosterTable table;
    ScanRoster(&src, table, kTick);
    CHECK(table.At(0).team == kRosterNoTeam);
    CHECK(table.At(0).teamChanges == 0);

    put(PackUserWord(0, 1, 0));   // seated on blue
    ScanRoster(&src, table, kTick + 1);
    CHECK(table.At(0).team == 0);
    CHECK(table.At(0).teamSlot == 1);
    CHECK(table.At(0).teamChanges == 1);
}

// AUD-W2-A-4's blocker: the record shape this packet introduced -- TEAM bit SET beside
// kPlayerNoTeam16 -- failed `echodemo verify`'s old biconditional, so every capture holding an
// unseated entrant was declared corrupt. The rule now lives once, here, and the tool imports it.
TEST_CASE("roster: verify's team/fill rule admits a recorded no-team and still refuses an unread one") {
    PlayerRecord p;
    memset(&p, 0, sizeof p);
    p.extended = 1;

    // The shape an unseated entrant is written in: the read happened, and its answer was no team.
    p.fill = kPlayerFillTeam;
    p.teamId16 = kPlayerNoTeam16;
    p.teamSlot16 = kPlayerNoTeam16;
    CHECK(RecordTeamAgreesWithFill(p));

    // A seated entrant, at both ends of the range the engine accepts.
    p.teamId16 = 0;
    CHECK(RecordTeamAgreesWithFill(p));
    p.teamId16 = uint16_t(kSeatBound - 1);
    CHECK(RecordTeamAgreesWithFill(p));

    // A team the engine would not accept is still a corrupt row, sentinel or not.
    p.teamId16 = uint16_t(kSeatBound);
    CHECK_FALSE(RecordTeamAgreesWithFill(p));

    // And the direction the check existed for in the first place: a zeroed team beside a CLEAR bit
    // is a team nothing sourced, written as if it had been.
    p.fill = 0;
    p.teamId16 = 0;
    CHECK_FALSE(RecordTeamAgreesWithFill(p));
    p.teamId16 = kPlayerNoTeam16;
    CHECK(RecordTeamAgreesWithFill(p));
}

// The two rows a capture actually produces, taken through the writer rather than hand-built, and
// handed to the verifier's rule as `echodemo verify` hands it every row of a file.
TEST_CASE("roster: the rows this capture writes all satisfy verify's team/fill rule") {
    const uint16_t kEntrants = 2;
    std::vector<uint8_t> ng(size_t(facts::USER_ARRAY_OFF) +
                                size_t(facts::USER_STRIDE) * kEntrants + sizeof(uint16_t),
                            0);
    auto put = [&](uint16_t idx, uint16_t word) {
        memcpy(ng.data() + size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * idx, &word,
               sizeof word);
    };
    put(0, PackUserWord(0, 2, 0));   // seated on blue
    put(1, kResetWord);              // joined nothing

    PackedWordSource src;
    src.ng = ng.data();
    src.rows.push_back(MakeRow(0, "seated", kIdBase + 11, 1));
    src.rows.push_back(MakeRow(1, "unseated", kIdBase + 12, 2));
    src.band = kEntrants;

    RosterTable table;
    ScanRoster(&src, table, kTick);
    REQUIRE(table.Count() == kEntrants);
    for (uint16_t i = 0; i < table.Count(); ++i) {
        PlayerRecord rec;
        FillPlayerRecord(table.At(i), rec);
        rec.extended = 1;
        CHECK(RecordTeamAgreesWithFill(rec));
    }
}

// ================================================================================================
// The colour's half of the same one-source rule.
//
// L2 #17: `[ 0] KFC- ... team 2 slot 0 align - ... fill 01F3` and `echodemo verify` FAIL on 2403
// checks -- "a player row's alignment and its ALIGN fill bit disagree". The ALIGN bit (0x20) is
// set in 0x01F3 and the alignment is the sentinel, which the old biconditional called corrupt.
// It is not: IDENT.ALIGN_TEAM_LIMIT is `cmp dx,2 / jnb` inside CR15NetGame::Alignment, so the
// engine's own answer for a spectator IS 0xFFFF, and the read did happen.

TEST_CASE("roster: verify's alignment/fill rule admits a team the engine gives no colour") {
    PlayerRecord p;
    memset(&p, 0, sizeof p);
    p.extended = 1;

    // THE L2 #17 SHAPE: read happened, spectator team, engine answered "no colour".
    p.fill = kPlayerFillAlign;
    p.teamId16 = uint16_t(facts::IDENT_ALIGN_TEAM_LIMIT);
    p.alignment16 = kPlayerNoAlignment16;
    CHECK(RecordAlignmentAgreesWithFill(p));

    // A team that DOES have a colour, at both ends of the range the alignment table is indexed by.
    p.teamId16 = 0;
    p.alignment16 = 0;
    CHECK(RecordAlignmentAgreesWithFill(p));
    p.teamId16 = uint16_t(facts::IDENT_ALIGN_TEAM_LIMIT - 1);
    p.alignment16 = 1;
    CHECK(RecordAlignmentAgreesWithFill(p));
    // ... and that team with no colour recorded is still admitted: the table entry is the
    // engine's to answer, and a lobby that has not assigned one is not a corrupt file.
    p.alignment16 = kPlayerNoAlignment16;
    CHECK(RecordAlignmentAgreesWithFill(p));

    // THE DIRECTION THE CHECK EXISTS FOR: a colour on a team the engine gives none to was
    // invented somewhere. A spectator, a moderator and the no-team sentinel all refuse.
    // The whole population of the 3-bit team field, not a sample: a colour is admitted for
    // exactly the teams the engine's own alignment table is indexed by, and refused for the rest.
    p.alignment16 = 0;
    for (uint16_t team = 0; team <= uint16_t(facts::IDENT_USER_TEAM_MASK); ++team) {
        p.teamId16 = team;
        CHECK(RecordAlignmentAgreesWithFill(p) ==
              (team < uint16_t(facts::IDENT_ALIGN_TEAM_LIMIT)));
    }
    p.teamId16 = kPlayerNoTeam16;
    CHECK_FALSE(RecordAlignmentAgreesWithFill(p));

    // And a CLEAR bit still admits the sentinel only -- a zeroed colour nothing sourced is the
    // half of the old rule that was right.
    p.fill = 0;
    p.teamId16 = 0;
    p.alignment16 = 0;
    CHECK_FALSE(RecordAlignmentAgreesWithFill(p));
    p.alignment16 = kPlayerNoAlignment16;
    CHECK(RecordAlignmentAgreesWithFill(p));
}

// The same three rows, built by the WRITER off a packed word and an alignment source that answers
// the way CR15NetGame::Alignment answers, then handed to both of verify's rules exactly as
// `echodemo verify` hands it every row of a file.
namespace {
class EngineAlignSource : public PackedWordSource {
public:
    // EngineRosterSource::Alignment (capture.cpp:819-827), through the SHIPPED reader rather than
    // a restatement of its `< 2` -- so a team limit that moves moves this fixture with it.
    bool Alignment(uint16_t idx, uint16_t& out) const override {
        uint16_t team = kRosterNoTeam, teamSlot = kRosterNoTeam;
        if (!Team(idx, team, teamSlot) || team == kRosterNoTeam) return false;
        out = identity_access::TeamAlignment(ng, static_cast<uint8_t>(team));
        return true;
    }
};
}  // namespace

TEST_CASE("roster: a spectator row satisfies both of verify's fill rules") {
    const uint16_t kEntrants = 3;
    const uint8_t kSpectator = uint8_t(facts::GAME_TEAM_SPECTATOR);
    std::vector<uint8_t> ng(size_t(facts::USER_ARRAY_OFF) +
                                size_t(facts::USER_STRIDE) * kEntrants + sizeof(uint16_t),
                            0);
    auto put = [&](uint16_t idx, uint16_t word) {
        memcpy(ng.data() + size_t(facts::USER_ARRAY_OFF) + size_t(facts::USER_STRIDE) * idx, &word,
               sizeof word);
    };
    put(0, PackUserWord(0, 0, 0));            // blue -- a team the alignment table is indexed by
    put(1, PackUserWord(1, 0, kSpectator));   // the L2 #17 row
    put(2, kResetWord);                       // joined nothing

    // The fixture is proved through the engine's own readers before anything downstream sees it,
    // and the coupling this rule rests on is asserted rather than assumed.
    REQUIRE(identity_access::TeamOf(ng.data(), UserIndex(1)) == kSpectator);
    REQUIRE(identity_access::TeamAlignment(ng.data(), kSpectator) == kPlayerNoAlignment16);
    REQUIRE(identity_access::TeamAlignment(ng.data(), 0) != kPlayerNoAlignment16);

    EngineAlignSource src;
    src.ng = ng.data();
    src.rows.push_back(MakeRow(0, "blue", kIdBase + 21, 1));
    src.rows.push_back(MakeRow(1, "spectator", kIdBase + 22, 2));
    src.rows.push_back(MakeRow(2, "unseated", kIdBase + 23, 3));
    src.band = kEntrants;

    RosterTable table;
    ScanRoster(&src, table, kTick);
    REQUIRE(table.Count() == kEntrants);

    PlayerRecord blue, spectator, unseated;
    FillPlayerRecord(table.At(0), blue);
    FillPlayerRecord(table.At(1), spectator);
    FillPlayerRecord(table.At(2), unseated);

    // The shape the file carried, reproduced: TEAM and ALIGN both set, team 2, colour absent.
    CHECK(spectator.teamId16 == uint16_t(kSpectator));
    CHECK((spectator.fill & kPlayerFillTeam) != 0);
    CHECK((spectator.fill & kPlayerFillAlign) != 0);
    CHECK(spectator.alignment16 == kPlayerNoAlignment16);
    CHECK(spectator.alignment == 0xFFu);

    // Blue keeps a colour, so the case above is not passing because nothing has one.
    CHECK((blue.fill & kPlayerFillAlign) != 0);
    CHECK(blue.alignment16 != kPlayerNoAlignment16);

    // No team, no read, no colour.
    CHECK((unseated.fill & kPlayerFillAlign) == 0);
    CHECK(unseated.alignment16 == kPlayerNoAlignment16);

    PlayerRecord* const rows[] = {&blue, &spectator, &unseated};
    for (PlayerRecord* rec : rows) {
        rec->extended = 1;
        CHECK(RecordTeamAgreesWithFill(*rec));
        CHECK(RecordAlignmentAgreesWithFill(*rec));
    }
}

// =================================================================================================
// RECORDER-3 -- the profile document a live server actually has, and the bits the file grew to say
// where it came from.
// =================================================================================================

TEST_CASE("RECORDER-3 F1: a document the LOBBY RECORD answered is recorded, and says so") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "remote", kIdBase + 1, 1));
    // What a spectator on a live server sees: the user table is empty for this entrant and the
    // document the lobby message carried is the only copy in the process.
    src.rows[0].entrantJsonSource = ProfileSource::EntrantRecord;
    RosterTable table;
    ScanRoster(&src, table, kTick);
    const RosterRow* r = RowNamed(table, "remote");
    REQUIRE(r != nullptr);

    PlayerRecord pr;
    FillPlayerRecord(*r, pr);
    uint8_t entrantBuf[kPlayerJsonCap] = {};
    uint8_t unlocksBuf[kPlayerJsonCap] = {};
    RosterRefusals refusals{};
    FillPlayerJson(&src, r->userIndex, entrantBuf, unlocksBuf, kPlayerJsonCap, pr, refusals);

    CHECK(pr.entrantJson.n > 0);
    CHECK((pr.fillExt & kPlayerFillProfileFromEntrantRec) != 0);
    CHECK((pr.fillExt & kPlayerFillProfileLossyNumber) == 0);

    // The control: the same row answered by the user table sets neither bit, so the bit is a fact
    // about the source and not about having a document at all.
    FakeSource usual;
    usual.rows.push_back(MakeRow(0, "remote", kIdBase + 1, 1));
    RosterTable t2;
    ScanRoster(&usual, t2, kTick);
    PlayerRecord pr2;
    FillPlayerRecord(*RowNamed(t2, "remote"), pr2);
    FillPlayerJson(&usual, 0, entrantBuf, unlocksBuf, kPlayerJsonCap, pr2, refusals);
    CHECK((pr2.fillExt & kPlayerFillProfileFromEntrantRec) == 0);
}

TEST_CASE("RECORDER-3 F4: a document whose walk dropped a number is RECORDED and flagged") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "lossy", kIdBase + 2, 1));
    src.rows[0].entrantJsonLossy = true;
    RosterTable table;
    ScanRoster(&src, table, kTick);
    PlayerRecord pr;
    FillPlayerRecord(*RowNamed(table, "lossy"), pr);
    uint8_t entrantBuf[kPlayerJsonCap] = {};
    uint8_t unlocksBuf[kPlayerJsonCap] = {};
    RosterRefusals refusals{};
    FillPlayerJson(&src, 0, entrantBuf, unlocksBuf, kPlayerJsonCap, pr, refusals);

    CHECK(pr.entrantJson.n > 0);     // not refused -- that is the whole of the ruling
    CHECK((pr.fillExt & kPlayerFillProfileLossyNumber) != 0);
}

TEST_CASE("RECORDER-3 F5: an unsourced jersey number is not a jersey 0") {
    FakeSource src;
    src.rows.push_back(MakeRow(0, "nonumber", kIdBase + 3, 1));
    src.rows[0].hasNumbers = false;
    RosterTable table;
    ScanRoster(&src, table, kTick);
    PlayerRecord pr;
    FillPlayerRecord(*RowNamed(table, "nonumber"), pr);
    CHECK(pr.loadoutNumber == 0);
    CHECK((pr.fillExt & kPlayerFillNumbers) == 0);   // 0 is what the field holds, not what he wears
    CHECK(pr.extendedV2 == 1);

    // A player who genuinely wears 0 is the other state, and the two are now different rows.
    FakeSource zero;
    zero.rows.push_back(MakeRow(0, "wearszero", kIdBase + 4, 1));
    zero.rows[0].number = 0;
    zero.rows[0].numberBody = 0;
    RosterTable t2;
    ScanRoster(&zero, t2, kTick);
    PlayerRecord pr2;
    FillPlayerRecord(*RowNamed(t2, "wearszero"), pr2);
    CHECK(pr2.loadoutNumber == 0);
    CHECK((pr2.fillExt & kPlayerFillNumbers) != 0);
}

TEST_CASE("RECORDER-3 F3: the xplatformid parser accepts the spellings the engine uses") {
    uint64_t v = 0;
    CHECK(ParseXPlatformId("OVR-ORG|0012345678abcdef", v));
    CHECK(v == 0x0012345678abcdefull);
    CHECK(ParseXPlatformId("OVR-12345678901234567890", v));
    CHECK(v == 12345678901234567890ull);
    CHECK(ParseXPlatformId("1234567890", v));
    CHECK(v == 1234567890ull);
    CHECK(ParseXPlatformId("0012345678abcdef", v));
    CHECK(v == 0x0012345678abcdefull);

    // A PARTIAL PARSE IS A REFUSAL: a truncated id is a different person and the roster dedups on it.
    CHECK_FALSE(ParseXPlatformId("", v));
    CHECK_FALSE(ParseXPlatformId(nullptr, v));
    CHECK_FALSE(ParseXPlatformId("OVR-ORG|", v));
    CHECK_FALSE(ParseXPlatformId("12345 6789", v));
    CHECK_FALSE(ParseXPlatformId("OVR-ORG|00185 6edc", v));
    CHECK_FALSE(ParseXPlatformId("zzz", v));
    // Wider than 64 bits is also a different person.
    CHECK_FALSE(ParseXPlatformId("99999999999999999999999", v));
    CHECK_FALSE(ParseXPlatformId("OVR-ORG|12345678abcdef1ff", v));   // 17 significant hex digits
    CHECK(ParseXPlatformId("OVR-ORG|00012345678abcdef", v));         // leading zeros are not width
    CHECK(v == 0x0012345678abcdefull);
}
