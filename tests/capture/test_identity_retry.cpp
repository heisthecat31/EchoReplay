// CAP-COST, the retry half. The field defect: one entrant's profile refused JsonNumberNotExact -- a
// full CJson walk that can only end the same way -- was re-offered on every 1 Hz roster scan, cost
// 1332 us per tick with 42 ms peaks, and the cost meter closed the owner's recording 48 s into a
// match. Every case here counts the ASKS, because an ask is the cost; a report field alone would
// certify the emitter's opinion of itself.

#include "doctest.h"

#include "capture/costmeter.h"
#include "capture/identity_emit.h"
#include "capture/roster.h"

#include "engine/identity_access.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace replay;
using namespace replay::capture;
using identity_access::Refusal;

namespace {

constexpr uint64_t kT0 = 5000000;
constexpr uint64_t kSecond = 1000000;

struct RetryEntrant {
    std::string name;
    uint64_t xplat = 0;
    uint16_t team = 0;
    std::string doc;
    Refusal refusal = Refusal::None;   // None: the document is handed over
    uint64_t tokenOverride = 0;        // non-zero: what ProfileDocToken answers, whatever the bytes
};

class RetrySource : public IRosterSource {
public:
    std::vector<RetryEntrant> entrants;
    mutable std::vector<int> docAsks;   // per entrant: the cost this packet exists to bound
    mutable int tokenReads = 0;

    void Add(const char* name, Refusal refusal, const char* doc = "{\"level\":9}") {
        RetryEntrant e;
        e.name = name;
        e.xplat = 0x7700000000000000ull + entrants.size();
        e.doc = doc;
        e.refusal = refusal;
        entrants.push_back(e);
        docAsks.push_back(0);
    }
    int TotalDocAsks() const {
        int n = 0;
        for (int a : docAsks) n += a;
        return n;
    }

    bool EntrantBand(uint16_t& band, const char*&) const override {
        band = uint16_t(entrants.size());
        return true;
    }
    bool Fingerprint(uint16_t i, uint64_t& out) const override {
        if (i >= entrants.size()) return false;
        uint64_t h = RosterFold(kRosterFoldSeed, entrants[i].name.data(), entrants[i].name.size());
        out = RosterFold(h, &entrants[i].team, sizeof entrants[i].team);
        return true;
    }
    bool SlotOf(uint16_t i, uint16_t& slot) const override {
        if (i >= entrants.size()) return false;
        slot = i;
        return true;
    }
    bool DisplayName(uint16_t i, char* out, size_t cap) const override {
        if (i >= entrants.size()) return false;
        memset(out, 0, cap);
        memcpy(out, entrants[i].name.data(),
               entrants[i].name.size() < cap - 1 ? entrants[i].name.size() : cap - 1);
        return true;
    }
    // Which storage this fake pretends answered, and whether its walk dropped a number.
    replay::capture::ProfileSource profileSource = replay::capture::ProfileSource::UserTable;
    bool lossyNumber = false;

    bool PlatformString(uint16_t, char*, size_t) const override { return false; }
    bool XPlatformId(uint16_t i, uint64_t& out) const override {
        if (i >= entrants.size()) return false;
        out = entrants[i].xplat;
        return true;
    }
    bool Team(uint16_t i, uint16_t& team, uint16_t& teamSlot) const override {
        if (i >= entrants.size()) return false;
        team = entrants[i].team;
        teamSlot = i;
        return true;
    }
    bool Alignment(uint16_t, uint16_t&) const override { return false; }
    bool LoadoutItems(uint16_t, uint64_t[20]) const override { return false; }
    bool LoadoutNumbers(uint16_t, uint16_t&, uint16_t&, const char*&) const override {
        return false;
    }
    bool Generation(uint16_t, uint16_t&) const override { return false; }
    bool HasProfileJson(uint16_t i) const override { return i < entrants.size(); }
    bool IsLocal(uint16_t) const override { return false; }

    bool EntrantJson(uint16_t i, void* out, size_t cap, size_t& written, bool& truncated,
                     const char*& why, Refusal& refusal,
                     replay::capture::EntrantJsonOutcome* outcome = nullptr) const override {
        if (outcome) *outcome = {profileSource, lossyNumber};
        written = 0;
        truncated = false;
        if (i >= entrants.size()) {
            why = "NoSuchEntrant";
            refusal = Refusal::NoSuchEntrant;
            return false;
        }
        ++docAsks[i];
        const RetryEntrant& e = entrants[i];
        if (e.refusal != Refusal::None) {
            why = "refused by the fixture";
            refusal = e.refusal;
            return false;
        }
        REQUIRE(e.doc.size() <= cap);
        written = e.doc.size();
        memcpy(out, e.doc.data(), written);
        return true;
    }
    bool UnlocksJson(uint16_t, void*, size_t, size_t& written, bool& truncated,
                     const char*& why) const override {
        written = 0;
        truncated = false;
        why = "not under test";
        return false;
    }
    // Bytes and length folded: a stand-in for the engine's document pointer that moves exactly when
    // the fixture's document is replaced, including by one of the same length.
    uint64_t ProfileDocToken(uint16_t i) const override {
        ++tokenReads;
        if (i >= entrants.size()) return 0;
        const RetryEntrant& e = entrants[i];
        if (e.tokenOverride) return e.tokenOverride;
        const uint64_t n = e.doc.size();
        return RosterFold(RosterFold(kRosterFoldSeed, &n, sizeof n), e.doc.data(), e.doc.size());
    }
};

struct RetrySink {
    std::vector<std::vector<uint8_t> > blocks;
    std::vector<IdentityEmitCensus> lines;
    std::vector<IdentityProfileRefusalNote> notes;
};

bool TakeBlock(const void* p, uint32_t n, uint64_t, void* ctx) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    static_cast<RetrySink*>(ctx)->blocks.push_back(std::vector<uint8_t>(b, b + n));
    return true;
}
void TakeCensus(const IdentityEmitCensus& line, void* ctx) {
    static_cast<RetrySink*>(ctx)->lines.push_back(line);
}
void TakeNote(const IdentityProfileRefusalNote& note, void* ctx) {
    static_cast<RetrySink*>(ctx)->notes.push_back(note);
}

struct RetryRig {
    RetrySource src;
    RosterTable table;
    RetrySink sink;
    IdentityEmitter em;
    IdentityEmitReport rep;
    std::vector<uint8_t> block = std::vector<uint8_t>(kIdentityBlockOneEntrantCap);
    std::vector<uint8_t> json = std::vector<uint8_t>(kRosterJsonCap);

    // The emitter's retry clock. It reads the scan's own time unless a case drives the two apart,
    // which is the field's shape: the capture's time axis is not the clock the back-off runs on.
    uint64_t clockUs = 0;
    bool clockFollowsScan = true;
    static uint64_t ReadClock(void* ctx) { return static_cast<RetryRig*>(ctx)->clockUs; }

    RetryRig() {
        em.SetProfileRefusalSink(&TakeNote, &sink);
        em.SetRetryClock(&ReadClock, this);
    }

    // One roster scan and the emit that follows it, as capture.cpp's tick runs them.
    void Scan(uint64_t tUs, uint16_t budget = kRosterMaxRows) {
        if (clockFollowsScan) clockUs = tUs;
        ScanRoster(&src, table, tUs);
        IdentityEmitScratch s;
        s.block = block.data();
        s.blockCap = block.size();
        s.json = json.data();
        s.jsonCap = json.size();
        em.EmitChanged(table, &src, tUs, 0, s, budget, &TakeBlock, &sink, &TakeCensus, &sink, rep);
        REQUIRE(rep.CountsBalance());
    }
};

bool LastBlockHasProfile(const RetrySink& sink) {
    IdentityBlockReader r;
    REQUIRE(!sink.blocks.empty());
    REQUIRE(r.Open(sink.blocks.back().data(), sink.blocks.back().size()));
    bool present = false;
    return r.ProfileRecorded(0, present) && present;
}

}  // namespace

TEST_CASE("cap-cost: a deterministic profile refusal is asked once, however many scans follow") {
    RetryRig rig;
    rig.src.Add("Floaty", Refusal::JsonNumberNotExact);
    rig.src.Add("Fine", Refusal::None);

    const int kScans = 600;   // a ten-minute match at the 1 Hz roster cadence
    for (int s = 0; s < kScans; ++s) rig.Scan(kT0 + uint64_t(s) * kSecond);

    CHECK(rig.src.docAsks[0] == 1);
    CHECK(rig.src.docAsks[1] == 1);
    CHECK(rig.sink.blocks.size() == 2);   // the refused entrant is IN the file, marked refused
    CHECK(rig.rep.unchanged == 2);
    CHECK(rig.rep.retryWaiting == 1);
    CHECK(rig.rep.profileRetried == 0);

    // The classification is said once per entrant, not once per scan.
    REQUIRE(rig.sink.notes.size() == 1);
    CHECK(rig.sink.notes[0].refusal == Refusal::JsonNumberNotExact);
    CHECK(rig.sink.notes[0].kind == ProfileRefusalKind::Deterministic);
    CHECK(rig.sink.notes[0].userSlot == 0);
    CHECK(std::string(rig.sink.notes[0].nameMasked) == "F***(6)");
}

TEST_CASE("cap-cost: a replaced document is offered again, and only once") {
    RetryRig rig;
    rig.src.Add("Floaty", Refusal::JsonNumberNotExact, "{\"xp\":1.5}");
    uint64_t t = kT0;
    for (int s = 0; s < 30; ++s, t += kSecond) rig.Scan(t);
    REQUIRE(rig.src.docAsks[0] == 1);

    // A new profile arrives, the same LENGTH and still unserialisable. Refusing document X said
    // nothing about document Y, so Y is asked about -- once.
    rig.src.entrants[0].doc = "{\"xp\":2.5}";
    for (int s = 0; s < 30; ++s, t += kSecond) rig.Scan(t);
    CHECK(rig.src.docAsks[0] == 2);
    CHECK(rig.sink.blocks.size() == 1);   // still refused, nothing else moved: no second block
    CHECK(rig.sink.notes.size() == 1);    // same entrant, same word: not said twice

    // And a document this build CAN serialise lands in the file on the scan it appears.
    rig.src.entrants[0].doc = "{\"xp\":3}";
    rig.src.entrants[0].refusal = Refusal::None;
    rig.Scan(t);
    CHECK(rig.src.docAsks[0] == 3);
    CHECK(rig.rep.emitted == 1);
    CHECK(LastBlockHasProfile(rig.sink));

    t += kSecond;
    for (int s = 0; s < 30; ++s, t += kSecond) rig.Scan(t);
    CHECK(rig.src.docAsks[0] == 3);
}

TEST_CASE("cap-cost: a row that itself changes re-asks a deterministically refused document") {
    // Pass 0 is unchanged by this packet: a row whose block content moved is rebuilt, and the block
    // needs its document. Pinned so the latch is known NOT to cover it.
    RetryRig rig;
    rig.src.Add("Floaty", Refusal::JsonNumberNotExact);
    rig.Scan(kT0);
    rig.Scan(kT0 + kSecond);
    REQUIRE(rig.src.docAsks[0] == 1);

    rig.src.entrants[0].team = 1;
    rig.Scan(kT0 + 2 * kSecond);
    CHECK(rig.src.docAsks[0] == 2);
    CHECK(rig.sink.blocks.size() == 2);
    for (int s = 3; s < 40; ++s) rig.Scan(kT0 + uint64_t(s) * kSecond);
    CHECK(rig.src.docAsks[0] == 2);
}

TEST_CASE("cap-cost: a transient refusal backs off per row, inside the roster's one re-ask per gap") {
    // The row's own delay runs 1 2 4 8 16 32 60 60 s; the roster allows one back-off re-ask per
    // 20 s. So: 1 (the first re-ask finds the gap open), then the gap decides (21 41 61 81) until
    // the row's delay outgrows it (81+32, then +60 +60).
    RetryRig rig;
    rig.src.Add("NotYet", Refusal::EmptyProfile);
    rig.src.entrants[0].tokenOverride = 0x1234;   // the document never moves: only the clock re-asks

    // Scanned four times a second, so an emitter that counted SCANS instead of time asks early.
    std::vector<uint64_t> askedAtQuarterSeconds;
    for (uint64_t q = 0; q <= 4 * 250; ++q) {
        const int before = rig.src.docAsks[0];
        rig.Scan(kT0 + q * (kSecond / 4));
        if (rig.src.docAsks[0] != before) askedAtQuarterSeconds.push_back(q);
    }
    REQUIRE(kProfileRetryRosterGapUs == 20 * kSecond);
    const std::vector<uint64_t> expectedSeconds = {0, 1, 21, 41, 61, 81, 113, 173, 233};
    REQUIRE(askedAtQuarterSeconds.size() == expectedSeconds.size());
    for (size_t i = 0; i < expectedSeconds.size(); ++i) {
        CHECK(askedAtQuarterSeconds[i] == expectedSeconds[i] * 4);
    }
    CHECK(rig.sink.blocks.size() == 1);
    REQUIRE(rig.sink.notes.size() == 1);
    CHECK(rig.sink.notes[0].kind == ProfileRefusalKind::Transient);
}

TEST_CASE("cap-cost: a transient refusal that clears lands the document at its next re-ask") {
    RetryRig rig;
    rig.src.Add("Late", Refusal::JsonTreeChanged);
    rig.src.entrants[0].tokenOverride = 0x77;
    for (int s = 0; s <= 9; ++s) rig.Scan(kT0 + uint64_t(s) * kSecond);   // asked at 0, 1
    REQUIRE(rig.src.docAsks[0] == 2);

    rig.src.entrants[0].refusal = Refusal::None;
    for (int s = 10; s <= 20; ++s) {
        rig.Scan(kT0 + uint64_t(s) * kSecond);
        CHECK(rig.rep.emitted == 0);
    }
    rig.Scan(kT0 + 21 * kSecond);
    CHECK(rig.src.docAsks[0] == 3);
    CHECK(rig.rep.emitted == 1);
    CHECK(LastBlockHasProfile(rig.sink));
}

TEST_CASE("cap-cost: a new document cuts a transient back-off short and restarts it") {
    RetryRig rig;
    rig.src.Add("Arrives", Refusal::EmptyProfile, "");
    uint64_t t = kT0;
    for (int s = 0; s <= 100; ++s, t += kSecond) rig.Scan(t);   // asked at 0 1 21 41 61 81; next 113
    REQUIRE(rig.src.docAsks[0] == 6);

    // The profile arrives 12 s before the back-off would have looked. Still refused (mid-write).
    rig.src.entrants[0].doc = "{\"level\":1}";
    rig.src.entrants[0].refusal = Refusal::JsonTreeChanged;
    rig.Scan(t);
    CHECK(rig.src.docAsks[0] == 7);
    CHECK(rig.sink.notes.size() == 2);   // a different word for this entrant is a new line

    // A new question starts a new schedule: one second, not sixty-four. (The roster's last back-off
    // re-ask was at 81 s, so its gap is open again; the replaced document's own ask never closed it.)
    rig.src.entrants[0].refusal = Refusal::None;
    rig.Scan(t + kSecond / 2);
    CHECK(rig.src.docAsks[0] == 7);
    rig.Scan(t + kSecond);
    CHECK(rig.src.docAsks[0] == 8);
    CHECK(LastBlockHasProfile(rig.sink));
}

TEST_CASE("cap-cost: the settled scan asks for no document and pushes nothing") {
    RetryRig rig;
    for (int i = 0; i < 10; ++i) rig.src.Add(("Fine" + std::to_string(i)).c_str(), Refusal::None);
    rig.src.Add("Floaty", Refusal::JsonNumberNotExact);
    rig.src.Add("NotYet", Refusal::EmptyProfile);
    rig.src.entrants[11].tokenOverride = 0x99;

    uint64_t t = kT0;
    for (int s = 0; s <= 113; ++s, t += kSecond) rig.Scan(t);   // NotYet was last asked at 113
    REQUIRE(rig.src.docAsks[11] == 7);

    const int asks = rig.src.TotalDocAsks();
    const size_t blocks = rig.sink.blocks.size();
    const size_t lines = rig.sink.lines.size();
    const size_t notes = rig.sink.notes.size();
    const int kQuietScans = 59;   // 114 .. 172: inside NotYet's sixty-second back-off
    rig.src.tokenReads = 0;
    for (int s = 0; s < kQuietScans; ++s, t += kSecond) {
        rig.Scan(t);
        CHECK(rig.rep.emitted == 0);
        CHECK(rig.rep.unchanged == 12);
        CHECK(rig.rep.retryWaiting == 2);
        CHECK(rig.rep.profileRetried == 0);
        CHECK(rig.rep.jsonRefused == 0);
    }
    CHECK(rig.src.TotalDocAsks() == asks);
    CHECK(rig.sink.blocks.size() == blocks);
    CHECK(rig.sink.lines.size() == lines);
    CHECK(rig.sink.notes.size() == notes);
    // The token is the only thing a settled scan reads, and only for the two REFUSED rows: the ten
    // entrants whose profiles are in the file cost what they cost before this packet.
    CHECK(rig.src.tokenReads == 2 * kQuietScans);

    rig.Scan(t);   // 173: the back-off expires, and exactly one document is asked for
    CHECK(rig.src.TotalDocAsks() == asks + 1);
    CHECK(rig.src.docAsks[11] == 8);
}

TEST_CASE("cap-cost: Reset gives the next capture its own single ask") {
    RetryRig rig;
    rig.src.Add("Floaty", Refusal::JsonNumberNotExact);
    for (int s = 0; s < 5; ++s) rig.Scan(kT0 + uint64_t(s) * kSecond);
    REQUIRE(rig.src.docAsks[0] == 1);

    rig.em.Reset();
    for (int s = 5; s < 10; ++s) rig.Scan(kT0 + uint64_t(s) * kSecond);
    CHECK(rig.src.docAsks[0] == 2);
    CHECK(rig.sink.blocks.size() == 2);
    CHECK(rig.sink.notes.size() == 2);   // the second file's log says it too
}

TEST_CASE("cap-cost: only a refusal that is a property of the document is deterministic") {
    const Refusal deterministic[] = {
        Refusal::JsonDepthLimit,     Refusal::JsonNodeLimit,        Refusal::JsonCycle,
        Refusal::JsonArrayNotWalkable, Refusal::JsonNumberNotExact, Refusal::JsonUnknownKind,
        Refusal::JsonStringNotUtf8,  Refusal::ShortBuffer,
    };
    // The enum's last value, so a value added after it widens this sweep's blind spot visibly: the
    // exhaustive switch in identity_emit.cpp stops compiling first.
    const int kLast = int(Refusal::AppliedSlotsUnstable);
    int deterministicSeen = 0;
    for (int v = 0; v <= kLast; ++v) {
        bool expected = false;
        for (Refusal d : deterministic) expected = expected || d == Refusal(v);
        INFO("Refusal value " << v);
        CHECK((ClassifyProfileRefusal(Refusal(v)) == ProfileRefusalKind::Deterministic) == expected);
        if (expected) ++deterministicSeen;
    }
    CHECK(deterministicSeen == 8);
    // The words a profile that simply has not arrived yet is refused with. Filing either as
    // deterministic would lose that entrant's profile for the whole file.
    CHECK(ClassifyProfileRefusal(Refusal::EmptyProfile) == ProfileRefusalKind::Transient);
    CHECK(ClassifyProfileRefusal(Refusal::NoSuchEntrant) == ProfileRefusalKind::Transient);
    CHECK(ClassifyProfileRefusal(Refusal(200)) == ProfileRefusalKind::Transient);
}

TEST_CASE("cap-cost: a document over the file's cap is asked once, and its replacement lands") {
    RetryRig rig;
    rig.src.Add("Big", Refusal::ShortBuffer);
    uint64_t t = kT0;
    for (int s = 0; s < 600; ++s, t += kSecond) rig.Scan(t);
    CHECK(rig.src.docAsks[0] == 1);
    REQUIRE(rig.sink.notes.size() == 1);
    CHECK(rig.sink.notes[0].kind == ProfileRefusalKind::Deterministic);

    rig.src.entrants[0].doc = "{\"level\":10}";
    rig.src.entrants[0].refusal = Refusal::None;
    rig.Scan(t);
    CHECK(rig.src.docAsks[0] == 2);
    CHECK(LastBlockHasProfile(rig.sink));
}

TEST_CASE("cap-cost: the back-off runs on its own clock, not on the capture's time axis") {
    // The field's tick axis is the server's peer clock, which ran at 0.54x: a back-off counted on it
    // re-asks late, and one written on the open's sim axis and compared on the peer axis may never.
    RetryRig rig;
    rig.clockFollowsScan = false;
    rig.src.Add("Torn", Refusal::JsonTreeChanged);
    rig.src.entrants[0].tokenOverride = 0x51;
    rig.clockUs = 40 * kSecond;
    rig.Scan(kT0);
    REQUIRE(rig.src.docAsks[0] == 1);

    rig.src.entrants[0].refusal = Refusal::None;
    rig.clockUs += kSecond;
    rig.Scan(kT0 + 540000);
    CHECK(rig.src.docAsks[0] == 2);
    CHECK(rig.rep.emitted == 1);
    CHECK(LastBlockHasProfile(rig.sink));
}

TEST_CASE("cap-cost: a refusal latched at the open is asked again on the tick's earlier time axis") {
    // The owner's second match in one process: the file opens on the sim
    // clock (an hour in) and ticks on a fresh server's peer clock (a hundred seconds in).
    const uint64_t kOpenUs = 3600 * kSecond;
    for (int clockIsTheAxis = 0; clockIsTheAxis < 2; ++clockIsTheAxis) {
        INFO("retry clock follows the capture axis: " << clockIsTheAxis);
        RetryRig rig;
        // 0: the emitter's own monotonic clock, as shipped. 1: a retry clock that itself reads
        // earlier than the latch, which must read as "the wait is over", not as an hour to go.
        rig.clockFollowsScan = clockIsTheAxis != 0;
        rig.src.Add("Torn", Refusal::JsonTreeChanged);
        rig.src.entrants[0].tokenOverride = 0x52;   // the document never moves: only the clock re-asks
        rig.clockUs = 27 * kSecond;
        rig.Scan(kOpenUs);
        REQUIRE(rig.src.docAsks[0] == 1);

        rig.src.entrants[0].refusal = Refusal::None;
        for (int s = 100; s < 700; ++s) {
            if (!rig.clockFollowsScan) rig.clockUs += kSecond;
            rig.Scan(uint64_t(s) * kSecond);
        }
        CHECK(rig.src.docAsks[0] == 2);
        CHECK(rig.sink.blocks.size() == 2);
        CHECK(LastBlockHasProfile(rig.sink));
    }
}

TEST_CASE("cap-cost: a lobby sharing one transient refusal is re-asked one entrant per cost window") {
    // The field head-room is ONE document walk per cost-meter window, and
    // refusal words are correlated across entrants. Ticks at the field's rate, one scan a second.
    const uint64_t kTicksPerSecond = 29;
    const int kRows = 10;
    RetryRig rig;
    rig.clockFollowsScan = false;
    for (int i = 0; i < kRows; ++i) {
        rig.src.Add(("Torn" + std::to_string(i)).c_str(), Refusal::JsonNodeUnreadable);
        rig.src.entrants[size_t(i)].tokenOverride = 0x100 + uint64_t(i);
    }
    rig.Scan(kT0);   // the open: every row's first ask, off the tick
    REQUIRE(rig.src.TotalDocAsks() == kRows);

    std::vector<uint64_t> reaskTicks;
    std::vector<int> reaskRows;
    const uint64_t kTicks = 60 * CostMeter::kWindowTicks;   // about eighteen minutes
    for (uint64_t tick = kTicksPerSecond; tick < kTicks; tick += kTicksPerSecond) {
        const std::vector<int> before = rig.src.docAsks;
        rig.clockUs = tick * kSecond / kTicksPerSecond;
        rig.Scan(kT0 + rig.clockUs, 1);
        for (int i = 0; i < kRows; ++i) {
            if (rig.src.docAsks[size_t(i)] == before[size_t(i)]) continue;
            reaskTicks.push_back(tick);
            reaskRows.push_back(i);
        }
    }

    // No 512-tick window, wherever it starts, holds two re-asks.
    REQUIRE(reaskTicks.size() >= size_t(3 * kRows));
    for (size_t k = 1; k < reaskTicks.size(); ++k) {
        INFO("re-ask " << k << " at tick " << reaskTicks[k] << " after tick " << reaskTicks[k - 1]);
        CHECK(reaskTicks[k] - reaskTicks[k - 1] >= CostMeter::kWindowTicks);
    }
    // And nobody is starved for it: the rows take turns, so every ten re-asks name every entrant.
    for (size_t k = 0; k + kRows <= reaskRows.size(); k += kRows) {
        std::vector<int> round(reaskRows.begin() + k, reaskRows.begin() + k + kRows);
        std::sort(round.begin(), round.end());
        INFO("re-asks " << k << " .. " << k + kRows - 1);
        CHECK(std::unique(round.begin(), round.end()) == round.end());
    }
}

TEST_CASE("cap-cost: Reset opens the roster's re-ask gap for the next capture") {
    RetryRig rig;
    rig.src.Add("NotYet", Refusal::EmptyProfile);
    rig.src.entrants[0].tokenOverride = 0x61;
    for (int s = 0; s <= 4; ++s) rig.Scan(kT0 + uint64_t(s) * kSecond);   // asked at 0, re-asked at 1
    REQUIRE(rig.src.docAsks[0] == 2);

    rig.em.Reset();
    rig.Scan(kT0 + 5 * kSecond);
    rig.Scan(kT0 + 6 * kSecond);   // 5 s after the last capture's re-ask: inside ITS gap, not this one's
    CHECK(rig.src.docAsks[0] == 4);
}

// =================================================================================================
// RECORDER-3 F1/F2/F4 -- what the identity block says about the document it carries.
// =================================================================================================

namespace {
struct BlockProfileFlags {
    bool fromEntrantRecord = false, lossy = false, supersedes = false;
};
BlockProfileFlags LastBlockProfileFlags(const RetrySink& sink) {
    IdentityBlockReader r;
    BlockProfileFlags f;
    REQUIRE(!sink.blocks.empty());
    REQUIRE(r.Open(sink.blocks.back().data(), sink.blocks.back().size()));
    REQUIRE(r.ProfileFlags(0, f.fromEntrantRecord, f.lossy, f.supersedes));
    return f;
}
}  // namespace

TEST_CASE("RECORDER-3 F2: the document that arrives LATE supersedes the manifest row") {
    RetryRig rig;
    // The live-server shape: at file-open the profile service has not replied, so the manifest row
    // for this entrant is written with no document at all.
    rig.src.Add("LateDoc", Refusal::EmptyProfile);
    rig.src.entrants[0].tokenOverride = 0x71;
    rig.Scan(kT0);
    REQUIRE(rig.sink.blocks.size() == 1);   // the entrant is new, so a block goes out without one
    REQUIRE(LastBlockHasProfile(rig.sink) == false);

    // ... and then the lobby record answers on a later scan.
    rig.src.entrants[0].refusal = Refusal::None;
    rig.src.profileSource = replay::capture::ProfileSource::EntrantRecord;
    for (int s = 1; s < 8; ++s) rig.Scan(kT0 + uint64_t(s) * kSecond);

    REQUIRE(!rig.sink.blocks.empty());
    CHECK(LastBlockHasProfile(rig.sink));
    const BlockProfileFlags f = LastBlockProfileFlags(rig.sink);
    CHECK(f.fromEntrantRecord == true);
    CHECK(f.supersedes == true);   // the header cannot be rewritten; this block is the re-stamp
    CHECK(f.lossy == false);
}

TEST_CASE("RECORDER-3 F4: a lossy document is published, flagged, and not refused") {
    RetryRig rig;
    rig.src.Add("Floaty", Refusal::None);
    rig.src.lossyNumber = true;
    rig.Scan(kT0);

    REQUIRE(!rig.sink.blocks.empty());
    CHECK(LastBlockHasProfile(rig.sink));
    const BlockProfileFlags f = LastBlockProfileFlags(rig.sink);
    CHECK(f.lossy == true);
    CHECK(f.fromEntrantRecord == false);   // the user table answered; only the number was dropped
    CHECK(f.supersedes == false);          // it was never refused, so the manifest row has it too
}
