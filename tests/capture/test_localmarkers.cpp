// ARCH-21 W2-A: the LOCAL marker source, its round table, the one-per-file source choice, and the
// differential against the shipped wire deriver.
//
// The differential is the gate that licenses calling the wire path a corroborator, so its shape is
// stated here rather than buried in an assertion:
//
//   proj(S) = the subsequence of S restricted to {LAUNCH_RELEASE, GOAL, PAUSE_BEGIN, PAUSE_END} --
//   the kinds both sources derive FROM THE SAME EDGE. Compared pairwise for kind, payload, a
//   +/- one-tick time tolerance, and (clause 4) the declared index mapping L.index ==
//   firstRoundCounter + W.index.
//
// EXCLUSIONS, one line of reason each. Adding to this list is a reviewable act; it is a constant
// here and never an edit inside an assertion:
//
//   ROUND_END, MATCH_OVER  -- local only. The wire deriver emits neither anywhere in src/.
//   PHASE                  -- both emit it, for different things: local for TeamRoomWait/Unknown,
//                             wire for every game_state edge. The sets differ by construction.
//   SUDDEN_DEATH           -- the wire path keys it on kNvSuddenDeath, which capture_facts.h's own
//                             comment says is a PLACEHOLDER symbol. It cannot fire at all.
//   ROUND_BEGIN            -- ARCH-21 R2 S7.2 lists this kind in proj. IT CANNOT BE IN IT, and the
//                             reason is a fact about the two derivers rather than about this
//                             fixture: the wire path derives the boundary from the launch
//                             conjunction (markers.cpp emits LAUNCH_RELEASE and ROUND_BEGIN from the
//                             same first_launch_of_round edge), while the local path derives it from
//                             the round_counter / RoundStart edge SECONDS EARLIER. Same round, two
//                             instants, so a one-tick tolerance on them would assert a coincidence
//                             neither source has -- and the only way to make it pass would be to
//                             script a fixture where the round starts at the launch, which is the
//                             "weaken the gate until it passes" failure S7.2 was written against.
//                             Clause 4's real subject, the round UNIT (D5), is asserted on
//                             LAUNCH_RELEASE instead: both sources emit it, at the same edge, and it
//                             carries the same round index on both sides. Nothing is projected away.
//
// Non-vacuity is mandatory and is a CONTROL, not a comment: RunDifferential refuses an empty
// projection, and the M9 case below deletes every wire row and asserts the differential goes RED.

#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "doctest.h"

#include "capture/capture_facts.h"
#include "capture/markers.h"

using replay::MarkerSource;
using replay::RoundRow;
using replay::kRoundNumberNone;
using replay::kRoundNumberRefused;
using replay::kRoundCounterReset;
using replay::kRoundNumberSynthesised;
using replay::kRoundPartialEnd;
using replay::kRoundPartialStart;
using replay::capture::LocalMarkerSource;
using replay::capture::MarkerSourceChoice;
using replay::capture::MarkerDeriver;
using replay::capture::MarkRec;
using replay::capture::NetvarMessage;
using replay::capture::kMarkGoal;
using replay::capture::kMarkLaunchRelease;
using replay::capture::kMarkMatchOver;
using replay::capture::kMarkPauseBegin;
using replay::capture::kMarkPauseEnd;
using replay::capture::kMarkPhase;
using replay::capture::kMarkRoundBegin;
using replay::capture::kMarkRoundEnd;
using replay::capture::kMarkSuddenDeath;

namespace gs = replay::gamestate;

namespace {

// Symbols are opaque CSymbol64s to this code, exactly as they are to the deriver: it classifies on
// the Phase W1-D's table produced and carries the raw symbol as payload.
// They are DERIVED from one named seed rather than written as round hex literals, because round hex
// literals collide with real recovered facts -- 0x2000 is PHYS.CI_PAUSED_BIT and 0x4000 is
// SESSION.FLAG_BIT_ACTIVE -- and a fixture that spells a fact's value reads as a claim about that
// fact. Only distinctness and non-zeroness matter here.
constexpr uint64_t kSymSeed = uint64_t(kRoundNumberNone) << 4;
constexpr uint64_t kSymPreMatch = kSymSeed + 1;
constexpr uint64_t kSymRoundStart = kSymSeed + 2;
constexpr uint64_t kSymPlaying = kSymSeed + 3;
constexpr uint64_t kSymScore = kSymSeed + 4;
constexpr uint64_t kSymRoundOver = kSymSeed + 5;
constexpr uint64_t kSymPostMatch = kSymSeed + 6;
constexpr uint64_t kSymTeamRoom = kSymSeed + 7;
constexpr uint64_t kSymUnnamed = kSymSeed + 8;
constexpr uint64_t kSymPausedIdle = kSymSeed + 9;
constexpr uint64_t kSymPausedOn = kSymSeed + 10;

constexpr uint64_t kSecond = 1000000;
constexpr uint64_t kTickUs = 8333;  // 120 Hz, the tolerance clause 2 allows

// A scripted local sampler. Every Push() is one capture tick's sample.
struct LocalScript {
    LocalMarkerSource src;
    std::vector<MarkRec> marks;
    gs::GameStateSample s = {};
    uint64_t tUs = kSecond;

    LocalScript() {
        s.availability = static_cast<uint8_t>(gs::Availability::Ok);
        s.present = gs::kHasPhaseSymbol | gs::kHasPausedSymbol | gs::kHasRoundCounter |
                    gs::kHasRoundPoints | gs::kHasMatchScore | gs::kHasFrisbeeOwnerId |
                    gs::kHasCountdownTimer | gs::kHasFirstLaunchOfRound;
        s.frisbeeOwnerId = gs::kFrisbeeUnowned;
        s.firstLaunchOfRound = 1;
        s.countdownTimer = 5.0f;
        s.pausedSymbol = kSymPausedIdle;
        s.phase = static_cast<uint8_t>(gs::Phase::PreMatch);
        s.phaseSymbol = kSymPreMatch;
    }

    void Phase(gs::Phase p, uint64_t sym) {
        s.phase = static_cast<uint8_t>(p);
        s.phaseSymbol = sym;
    }

    void Push(uint64_t dtUs = kTickUs) {
        tUs += dtUs;
        s.captureTimeUs = tUs;
        s.engineTimeUs = tUs + 500;
        MarkRec out[16];
        const uint32_t n = src.Note(s, out, 16);
        for (uint32_t i = 0; i < n; ++i) {
            marks.push_back(out[i]);
        }
    }
};

// The reader's S4.5 invariant, restated where the WRITER can be held to it: numbered rows ascend
// strictly. This is the exact predicate manifest_io.cpp refuses a round table for.
bool AscendingNumbers(const LocalMarkerSource& src) {
    bool saw = false;
    uint16_t prev = 0;
    for (uint16_t i = 0; i < src.RowCount(); ++i) {
        const uint16_t n = src.Rows()[i].number;
        if (n == kRoundNumberNone) {
            continue;
        }
        if (saw && n <= prev) {
            return false;
        }
        prev = n;
        saw = true;
    }
    return true;
}

uint32_t CountKind(const std::vector<MarkRec>& v, uint16_t kind) {
    uint32_t n = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].kind == kind) {
            ++n;
        }
    }
    return n;
}

std::vector<MarkRec> OfKind(const std::vector<MarkRec>& v, uint16_t kind) {
    std::vector<MarkRec> out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].kind == kind) {
            out.push_back(v[i]);
        }
    }
    return out;
}

// The scalar SValue encoding the wire deriver reads (markers.cpp ScalarInt): tag 0x212 in the high
// dword. Anything else is not a scalar and the deriver refuses it, which is why the fixture encodes
// rather than assigning a bare integer.
uint64_t SVal(uint32_t v) { return (uint64_t(0x00000212u) << 32) | v; }
uint64_t SValF(float f) {
    uint32_t bits = 0;
    memcpy(&bits, &f, sizeof bits);
    return SVal(bits);
}

void AddRow(NetvarMessage& m, uint64_t name, uint64_t value) {
    m.rows[m.varCount].name = name;
    m.rows[m.varCount].value = value;
    m.rows[m.varCount].slot = 0;
    ++m.varCount;
}

}  // namespace

TEST_CASE("local marker source: three rounds, numbered by round_counter") {
    LocalScript sc;
    sc.s.roundCounter = 1;
    sc.Push();  // baseline: a first sighting is a value, not an edge

    for (uint32_t round = 1; round <= 3; ++round) {
        sc.s.roundCounter = round;
        sc.s.bluePoints = 0;
        sc.s.redPoints = 0;
        sc.s.firstLaunchOfRound = 1;
        sc.s.countdownTimer = 5.0f;
        sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
        sc.Push(kSecond);

        sc.s.countdownTimer = 0.0f;
        sc.s.firstLaunchOfRound = 0;
        sc.Push(3 * kSecond);

        sc.Phase(gs::Phase::Playing, kSymPlaying);
        sc.Push(kSecond);

        sc.s.bluePoints = 2;
        sc.s.blueMatchScore = 2 * round;
        sc.Phase(gs::Phase::Score, kSymScore);
        sc.Push(10 * kSecond);

        sc.Phase(gs::Phase::RoundOver, kSymRoundOver);
        sc.Push(kSecond);
    }
    sc.Phase(gs::Phase::PostMatch, kSymPostMatch);
    sc.Push(kSecond);
    sc.src.Finish(sc.tUs);

    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 3);
    CHECK(CountKind(sc.marks, kMarkRoundEnd) == 3);
    CHECK(CountKind(sc.marks, kMarkMatchOver) == 1);
    CHECK(CountKind(sc.marks, kMarkLaunchRelease) == 3);
    CHECK(CountKind(sc.marks, kMarkGoal) == 3);

    // M4: the index is the ENGINE's round_counter, not a synthesised 0-based ordinal. A build that
    // numbered by ordinal would read 0,1,2 here and every surface would then disagree with the
    // browser row and with `echodemo extract --round N`.
    const std::vector<MarkRec> begins = OfKind(sc.marks, kMarkRoundBegin);
    REQUIRE(begins.size() == 3);
    CHECK(begins[0].index == 1);
    CHECK(begins[1].index == 2);
    CHECK(begins[2].index == 3);

    REQUIRE(sc.src.RowCount() == 3);
    for (uint16_t i = 0; i < 3; ++i) {
        const RoundRow& r = sc.src.Rows()[i];
        CHECK(r.number == uint16_t(i + 1));
        CHECK(r.flags == 0);
        CHECK(r.phaseAtBegin == static_cast<uint8_t>(gs::Phase::RoundStart));
        CHECK(r.phaseAtEnd == static_cast<uint8_t>(gs::Phase::RoundOver));
        CHECK(r.bluePointsAtEnd == 2);
        CHECK(r.blueMatchScoreAtEnd == uint16_t(2 * (i + 1)));
        CHECK(r.beginUs < r.endUs);
        CHECK(r.beginEngineUs == r.beginUs + 500);
    }
    const LocalMarkerSource::Stats& st = sc.src.GetStats();
    CHECK(st.localBoundaries == 6);
    CHECK(st.roundNumberOutOfRange == 0);
    CHECK(st.roundCounterReset == false);
    CHECK(st.marksDroppedForCap == 0);
}

TEST_CASE("BL-3 / M8: team_room_wait between rounds is a PHASE and never a round begin") {
    LocalScript sc;
    sc.s.roundCounter = 1;
    sc.Push();
    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push(kSecond);
    sc.Phase(gs::Phase::RoundOver, kSymRoundOver);
    sc.Push(kSecond);

    const size_t before = sc.marks.size();
    sc.Phase(gs::Phase::TeamRoomWait, kSymTeamRoom);
    sc.Push(kSecond);

    const std::vector<MarkRec> after(sc.marks.begin() + before, sc.marks.end());
    REQUIRE(after.size() == 1);
    CHECK(after[0].kind == kMarkPhase);
    CHECK(after[0].payload == kSymTeamRoom);
    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 1);
}

TEST_CASE("M7: an unnamed phase symbol is never a round") {
    LocalScript sc;
    sc.s.roundCounter = 4;
    sc.Push();
    sc.Phase(gs::Phase::Unknown, kSymUnnamed);
    sc.Push(kSecond);

    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 0);
    CHECK(CountKind(sc.marks, kMarkPhase) == 1);
    CHECK(sc.src.GetStats().phaseUnknown == 1);
    CHECK(sc.src.RowCount() == 0);
}

TEST_CASE("M5 / C4: a round_counter outside the representable range refuses its boundary") {
    LocalScript sc;
    sc.s.roundCounter = 1;
    sc.Push();
    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push(kSecond);
    REQUIRE(CountKind(sc.marks, kMarkRoundBegin) == 1);

    sc.s.roundCounter = 0x10001;  // round 65537: a 16-bit truncation would call it round 1
    sc.Push(kSecond);

    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 1);
    CHECK(sc.src.GetStats().roundNumberOutOfRange == 1);
    REQUIRE(sc.src.RowCount() == 2);
    CHECK(sc.src.Rows()[1].number == kRoundNumberNone);
    CHECK((sc.src.Rows()[1].flags & kRoundNumberRefused) != 0);
}

TEST_CASE("M1: late join mid-round is a baseline, not a round begin") {
    LocalScript sc;
    sc.s.roundCounter = 7;
    sc.Phase(gs::Phase::Playing, kSymPlaying);
    sc.Push();
    sc.Push(kSecond);

    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 0);
    REQUIRE(sc.src.RowCount() == 1);
    CHECK(sc.src.Rows()[0].number == 7);
    CHECK((sc.src.Rows()[0].flags & kRoundPartialStart) != 0);

    sc.Phase(gs::Phase::RoundOver, kSymRoundOver);
    sc.Push(kSecond);
    CHECK(CountKind(sc.marks, kMarkRoundEnd) == 1);
    CHECK(sc.src.Rows()[0].endUs == sc.tUs);
}

TEST_CASE("a round still open at close is a partialEnd row") {
    LocalScript sc;
    sc.s.roundCounter = 2;
    sc.Push();
    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push(kSecond);
    sc.src.Finish(sc.tUs + kSecond);

    REQUIRE(sc.src.RowCount() == 1);
    CHECK((sc.src.Rows()[0].flags & kRoundPartialEnd) != 0);
    CHECK(sc.src.Rows()[0].endUs == sc.tUs + kSecond);
}

TEST_CASE("the L2 shape: no round_counter and no phase symbol yields no rounds and says so") {
    LocalMarkerSource src;
    gs::GameStateSample s = {};
    s.availability = static_cast<uint8_t>(gs::Availability::Ok);
    s.phase = static_cast<uint8_t>(gs::Phase::Absent);
    MarkRec out[16];
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < 200; ++i) {
        s.captureTimeUs = kSecond + i * kTickUs;
        emitted += src.Note(s, out, 16);
    }
    CHECK(emitted == 0);
    CHECK(src.RowCount() == 0);
    CHECK(src.GetStats().samples == 200);
    CHECK(src.GetStats().localBoundaries == 0);
}

TEST_CASE("a sampler that refuses derives nothing and the gap is measured") {
    LocalScript sc;
    sc.s.roundCounter = 1;
    sc.Push();
    sc.s.availability = static_cast<uint8_t>(gs::Availability::NoNetGame);
    for (int i = 0; i < 10; ++i) {
        sc.Push(kSecond);
    }
    sc.s.availability = static_cast<uint8_t>(gs::Availability::Ok);
    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push(kSecond);

    CHECK(sc.src.GetStats().samplesRefused == 10);
    CHECK(sc.src.GetStats().localSampleGapUs == 11 * kSecond);
    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 1);
}

TEST_CASE("the source is chosen once per file") {
    SUBCASE("solo host: local answers, the corroborator is never asked") {
        MarkerSourceChoice c;
        c.NoteFileOpen(kSecond);
        c.NoteLocalSample(true, kSecond + kTickUs);
        CHECK(c.Source() == MarkerSource::Local);
        CHECK(c.CorroboratorAsked() == false);
        CHECK(c.WireRowsSeen() == 0);
        // Fed later, it is still LOCAL: the source is never switched mid-file, and the ask is now
        // recorded as an ask rather than as agreement.
        c.NoteCorroboratorFed(6, 5 * kSecond);
        CHECK(c.Source() == MarkerSource::Local);
        CHECK(c.CorroboratorAsked() == true);
        CHECK(c.WireRowsSeen() == 6);
    }
    SUBCASE("no local candidate: the wire path is chosen at its first row") {
        MarkerSourceChoice c;
        c.SetLocalCandidate(false);
        c.NoteFileOpen(kSecond);
        c.NoteCorroboratorFed(3, kSecond + kTickUs);
        CHECK(c.Source() == MarkerSource::Wire);
    }
    SUBCASE("neither answers: NONE is named, loudly, and not left as an unlabelled wire") {
        MarkerSourceChoice c;
        c.NoteFileOpen(kSecond);
        for (uint32_t i = 0; i < MarkerSourceChoice::kArmTicks; ++i) {
            c.NoteLocalSample(false, kSecond + i * kTickUs);
        }
        // AUDIT_ARCH21_PACKETS MAJOR-1. The window expiring establishes that the first two seconds
        // were quiet and nothing else, so the choice stays OPEN here -- writing None at expiry
        // condemned every always-on file, which opens in the lobby where netGame does not exist yet.
        // The close is the only place that can say a file never had a source, and it still says it.
        CHECK(c.Chosen() == false);
        CHECK(c.Settle() == MarkerSource::None);
        CHECK(c.CorroboratorAsked() == false);
    }
    SUBCASE("netGame comes up after the window: LOCAL, not a file condemned by its own start time") {
        MarkerSourceChoice c;
        c.NoteFileOpen(kSecond);
        for (uint32_t i = 0; i < MarkerSourceChoice::kArmTicks; ++i) {
            c.NoteLocalSample(false, kSecond + i * kTickUs);
        }
        CHECK(c.Chosen() == false);
        c.NoteLocalSample(true, kSecond + 4 * MarkerSourceChoice::kArmWindowUs);
        CHECK(c.Source() == MarkerSource::Local);
        CHECK(c.Settle() == MarkerSource::Local);
    }
    SUBCASE("a mark filed under WIRE closes the choice, so nothing can re-provenance it") {
        MarkerSourceChoice c;
        c.NoteFileOpen(kSecond);
        c.NoteCorroboratorFed(4, kSecond + 3 * MarkerSourceChoice::kArmWindowUs);
        CHECK(c.Source() == MarkerSource::Wire);
        c.NoteMarkFiled();
        // One emitter per file: rows already in the stream fix the file's provenance, and a local
        // sample arriving afterwards must not relabel them.
        c.NoteLocalSample(true, kSecond + 4 * MarkerSourceChoice::kArmWindowUs);
        CHECK(c.Source() == MarkerSource::Wire);
        CHECK(c.Settle() == MarkerSource::Wire);
    }
    SUBCASE("a transient refusal does not condemn the file (C6)") {
        MarkerSourceChoice c;
        c.NoteFileOpen(kSecond);
        c.NoteLocalSample(false, kSecond + kTickUs);
        CHECK(c.Chosen() == false);
        c.NoteLocalSample(true, kSecond + 2 * kTickUs);
        CHECK(c.Source() == MarkerSource::Local);
    }
    SUBCASE("a file shorter than the window still names a source") {
        MarkerSourceChoice c;
        c.NoteFileOpen(kSecond);
        c.NoteLocalSample(false, kSecond + kTickUs);
        CHECK(c.Settle() == MarkerSource::None);
    }
}

// ---------------------------------------------------------------------------------------------
// The differential.

namespace {

struct Pair {
    std::vector<MarkRec> local;
    std::vector<MarkRec> wire;
};

bool InProjection(uint16_t kind) {
    return kind == kMarkLaunchRelease || kind == kMarkGoal || kind == kMarkPauseBegin ||
           kind == kMarkPauseEnd;
}

std::vector<MarkRec> Project(const std::vector<MarkRec>& v) {
    std::vector<MarkRec> out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (InProjection(v[i].kind)) {
            out.push_back(v[i]);
        }
    }
    // Declared normalisation: markers derived from the same instant are ordered by kind, because the
    // order of two edges inside one sample is not a property either source has. Cross-instant order
    // is fully compared -- the fixture separates its rounds by seconds.
    std::stable_sort(out.begin(), out.end(), [](const MarkRec& a, const MarkRec& b) {
        if (a.tUs != b.tUs) {
            return a.tUs < b.tUs;
        }
        return a.kind < b.kind;
    });
    return out;
}

uint64_t AbsDiff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

bool RunDifferential(const std::vector<MarkRec>& localSeq, const std::vector<MarkRec>& wireSeq,
                     uint16_t firstRoundCounter, std::string& why) {
    const std::vector<MarkRec> l = Project(localSeq);
    const std::vector<MarkRec> w = Project(wireSeq);
    // The non-vacuity control (BL-2b). A gate that passes on two empty sequences is not a gate.
    if (l.empty() || w.empty()) {
        why = "non-vacuity: a projected sequence is empty";
        return false;
    }
    if (CountKind(localSeq, kMarkRoundBegin) < 3 || CountKind(wireSeq, kMarkRoundBegin) < 3) {
        why = "non-vacuity: the scripted match did not produce three rounds in each source";
        return false;
    }
    if (l.size() != w.size()) {
        why = "clause 1: the projections differ in length";
        return false;
    }
    for (size_t i = 0; i < l.size(); ++i) {
        if (l[i].kind != w[i].kind) {
            why = "clause 1: kind mismatch";
            return false;
        }
        if (AbsDiff(l[i].tUs, w[i].tUs) > kTickUs) {
            why = "clause 2: the two observations are more than one tick apart";
            return false;
        }
        if (l[i].payload != w[i].payload) {
            why = "clause 3: payload mismatch";
            return false;
        }
        // Clause 4, and where it can be asserted at all. R2-c1: GOAL's index is the TEAM on both
        // sides, not a round unit. And the shipped wire deriver POST-INCREMENTS its ordinal at the
        // launch (markers.cpp: emit(ROUND_BEGIN, roundIndex_); ++roundIndex_), so every marker it
        // emits later in that round already carries the NEXT round's ordinal -- its index is a round
        // unit only AT the boundary. So the mapping is asserted on LAUNCH_RELEASE, the boundary kind
        // both sources emit from the same edge; PAUSE keeps its kind, time and payload comparison.
        if (l[i].kind == kMarkLaunchRelease &&
            l[i].index != uint16_t(firstRoundCounter + w[i].index)) {
            why = "clause 4: the round unit drifted between the two sources";
            return false;
        }
    }
    return true;
}

// One scripted match driving BOTH sources. blue_score (a depth-0 MATCH concept on the wire) is kept
// equal to bluePoints (the depth-2 ROUND concept locally) so clause 3 tests the derivers rather than
// the fixture's arithmetic; S3.1 is explicit that the two are different engine values.
Pair ScriptBothSources(bool deleteWireRows) {
    Pair out;
    LocalScript sc;
    MarkerDeriver wire;
    uint64_t wireT = 0;

    const auto wireMsg = [&](NetvarMessage& m) {
        if (deleteWireRows) {
            return;  // M9: every wire row deleted
        }
        MarkRec buf[16];
        // The two sources observe the same edge on different clocks; the offset is what makes
        // clause 2's tolerance a real test rather than an identity.
        wireT = sc.tUs + 2000;
        const uint32_t n = wire.Note(m, wireT, buf, 16);
        for (uint32_t i = 0; i < n; ++i) {
            out.wire.push_back(buf[i]);
        }
    };

    namespace facts = replay::capture_facts;
    NetvarMessage base = {};
    base.varCount = 0;
    AddRow(base, facts::kNvBlueScore, SVal(0));
    AddRow(base, facts::kNvFirstLaunchOfRound, SVal(1));
    AddRow(base, facts::kNvCountdownTimer, SValF(5.0f));
    AddRow(base, facts::kNvFrisbeeOwnerId, SVal(gs::kFrisbeeUnowned));
    // paused_state is a raw CSymbol64 on both sides (S3.4), not a scalar SValue: the wire deriver
    // compares and carries the qword verbatim, so the fixture must too.
    AddRow(base, facts::kNvPausedState, kSymPausedIdle);

    sc.s.roundCounter = 1;
    sc.Push();
    wireMsg(base);

    uint32_t score = 0;
    for (uint32_t round = 1; round <= 3; ++round) {
        sc.s.roundCounter = round;
        sc.s.firstLaunchOfRound = 1;
        sc.s.countdownTimer = 5.0f;
        sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
        sc.Push(kSecond);
        {
            NetvarMessage m = {};
            AddRow(m, facts::kNvFirstLaunchOfRound, SVal(1));
            AddRow(m, facts::kNvCountdownTimer, SValF(5.0f));
            wireMsg(m);
        }

        // The launch: the one edge both sources derive the same kind from.
        sc.s.countdownTimer = 0.0f;
        sc.s.firstLaunchOfRound = 0;
        sc.Push(3 * kSecond);
        {
            NetvarMessage m = {};
            AddRow(m, facts::kNvCountdownTimer, SValF(0.0f));
            AddRow(m, facts::kNvFirstLaunchOfRound, SVal(0));
            wireMsg(m);
        }

        sc.Phase(gs::Phase::Playing, kSymPlaying);
        sc.Push(kSecond);

        ++score;
        sc.s.bluePoints = score;
        sc.Phase(gs::Phase::Score, kSymScore);
        sc.Push(10 * kSecond);
        {
            NetvarMessage m = {};
            AddRow(m, facts::kNvBlueScore, SVal(score));
            wireMsg(m);
        }

        if (round == 3) {
            sc.s.pausedSymbol = kSymPausedOn;
            sc.Push(kSecond);
            {
                NetvarMessage m = {};
                AddRow(m, facts::kNvPausedState, kSymPausedOn);
                wireMsg(m);
            }
            sc.s.pausedSymbol = kSymPausedIdle;
            sc.Push(kSecond);
            {
                NetvarMessage m = {};
                AddRow(m, facts::kNvPausedState, kSymPausedIdle);
                wireMsg(m);
            }
        }

        sc.Phase(gs::Phase::RoundOver, kSymRoundOver);
        sc.Push(kSecond);
    }
    out.local = sc.marks;
    return out;
}

}  // namespace

TEST_CASE("the differential: the local sequence and the corroborator agree where they can") {
    const Pair p = ScriptBothSources(false);
    std::string why;

    // Both sources really did produce the rounds the control requires.
    CHECK(CountKind(p.local, kMarkRoundBegin) == 3);
    CHECK(CountKind(p.wire, kMarkRoundBegin) == 3);
    CHECK(CountKind(p.local, kMarkLaunchRelease) == 3);
    CHECK(CountKind(p.wire, kMarkLaunchRelease) == 3);
    CHECK(Project(p.local).size() == Project(p.wire).size());
    CHECK(Project(p.local).empty() == false);

    const bool agreed = RunDifferential(p.local, p.wire, 1, why);
    INFO("differential: ", why);
    CHECK(agreed);
}

TEST_CASE("M9: with every wire row deleted the differential goes RED, not green-on-empty") {
    const Pair p = ScriptBothSources(true);
    std::string why;
    CHECK(p.wire.empty());
    CHECK(RunDifferential(p.local, p.wire, 1, why) == false);
    CHECK(why.find("non-vacuity") != std::string::npos);
}

TEST_CASE("the differential can fail: a drifted round unit is clause 4's whole subject") {
    const Pair p = ScriptBothSources(false);
    std::string why;
    // M4's shape: the local index carries a 0-based ordinal while round_counter is present. The
    // mapping then reads L == W and the gate must refuse it.
    std::vector<MarkRec> drifted = p.local;
    for (size_t i = 0; i < drifted.size(); ++i) {
        if (drifted[i].index > 0) {
            --drifted[i].index;
        }
    }
    CHECK(RunDifferential(drifted, p.wire, 1, why) == false);
    CHECK(why.find("clause 4") != std::string::npos);
}

TEST_CASE("R15: run 2's shape -- two RoundStart transitions at one counter write ONE row") {
    // L5-RUN-14 run 2, read back off the file: two rows, BOTH numbered 0, and the shipped reader
    // refused the whole 55 099-byte capture over the pair. `round_counter` never moves on the
    // owner's -offline host, so B2's phase rule fires twice past its 500 ms window and the second
    // boundary repeats the first one's number.
    LocalScript sc;
    sc.s.roundCounter = 0;
    sc.Push();  // baseline: a first sighting is a value, not an edge

    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push();
    sc.Phase(gs::Phase::TeamRoomWait, kSymTeamRoom);
    sc.Push(kSecond);
    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push(kSecond);  // well past kB2SuppressUs, so B2 is live again

    // One boundary at counter 0 is representable; the second one is not, and is refused BY NAME
    // rather than written as the row that costs the file.
    REQUIRE(sc.src.RowCount() == 1);
    CHECK(sc.src.Rows()[0].number == 0);
    CHECK(sc.src.GetStats().refusedNonAscending == 1);
    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 1);
    CHECK(AscendingNumbers(sc.src));

    // The refusal costs the boundary, not the footage: the round opened by the first transition is
    // still open and still gets its row when the file closes.
    sc.src.Finish(sc.tUs + kSecond);
    REQUIRE(sc.src.RowCount() == 1);
    CHECK((sc.src.Rows()[0].flags & kRoundPartialEnd) != 0);
}

TEST_CASE("R15: a round_counter that restarts re-bases the number and never descends") {
    LocalScript sc;
    sc.s.roundCounter = 1;
    sc.Push();  // baseline
    for (uint32_t c = 2; c <= 4; ++c) {
        sc.s.roundCounter = c;
        sc.Push();
    }
    REQUIRE(sc.src.RowCount() == 3);
    CHECK(sc.src.Rows()[2].number == 4);

    // The rematch (ARCH-21 C5): the engine restarts its counter inside one open file.
    sc.s.roundCounter = 1;
    sc.Push();
    sc.s.roundCounter = 2;
    sc.Push();

    CHECK(sc.src.GetStats().roundCounterReset);
    REQUIRE(sc.src.RowCount() == 4);
    const RoundRow& r = sc.src.Rows()[3];
    CHECK(r.number == 5);  // one past where the table already stood, never back to 2
    CHECK((r.flags & kRoundCounterReset) != 0);
    CHECK((r.flags & kRoundNumberSynthesised) != 0);
    CHECK(sc.src.GetStats().rebasedAfterReset == 1);
    CHECK(sc.src.GetStats().refusedNonAscending == 0);
    CHECK(AscendingNumbers(sc.src));
}

TEST_CASE("DIAG R1: a refused boundary names itself once and cannot spam a pathological session") {
    // The refusal was previously a COUNTER and nothing else: nothing in the file or the log said
    // which boundary was dropped, so an owner reading `roundRowCount: 1` could not tell a correct
    // refusal from a lost round. One line per refusal, rate-limited, answers that.
    LocalScript sc;
    sc.s.roundCounter = 0;
    sc.Push();  // baseline
    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push();  // admitted: round 0

    LocalMarkerSource::RefusalNote notes[LocalMarkerSource::kMaxRefusalsNamed * 2];
    CHECK(sc.src.TakeRefusals(notes, LocalMarkerSource::kMaxRefusalsNamed) == 0);

    sc.Phase(gs::Phase::TeamRoomWait, kSymTeamRoom);
    sc.Push(kSecond);
    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push(kSecond);  // past kB2SuppressUs, so B2 is live and offers 0 again
    const uint64_t refusedAtUs = sc.tUs;

    REQUIRE(sc.src.GetStats().refusedNonAscending == 1);
    REQUIRE(sc.src.TakeRefusals(notes, LocalMarkerSource::kMaxRefusalsNamed) == 1);
    CHECK(notes[0].offered == 0);
    CHECK(notes[0].admitted == 0);
    CHECK(notes[0].phase == static_cast<uint8_t>(gs::Phase::RoundStart));
    CHECK(notes[0].captureTimeUs == refusedAtUs);
    // Named ONCE: the second drain has nothing, so no refusal can be logged twice.
    CHECK(sc.src.TakeRefusals(notes, LocalMarkerSource::kMaxRefusalsNamed) == 0);

    // The rate limit. Every further oscillation is still COUNTED; only the naming stops, and
    // draining does not buy the session a fresh allowance.
    for (uint32_t i = 0; i < LocalMarkerSource::kMaxRefusalsNamed + 4; ++i) {
        sc.Phase(gs::Phase::TeamRoomWait, kSymTeamRoom);
        sc.Push(kSecond);
        sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
        sc.Push(kSecond);
    }
    CHECK(sc.src.GetStats().refusedNonAscending == LocalMarkerSource::kMaxRefusalsNamed + 5);
    CHECK(sc.src.TakeRefusals(notes, LocalMarkerSource::kMaxRefusalsNamed * 2) ==
          LocalMarkerSource::kMaxRefusalsNamed - 1);
    CHECK(sc.src.RowCount() == 1);
    CHECK(AscendingNumbers(sc.src));
}

TEST_CASE("DIAG R1: a refused RoundStart still files its PHASE mark, so the timeline has no hole") {
    // DIAG-ROUNDS-NONASC R1: the two ch44 re-entries emitted NOTHING -- not even the kMarkPhase
    // every other phase change gets -- so a reader of S_MARK alone saw a hole at +49.766 s where
    // S_GAMESTATE showed a phase change. The round-table refusal and the phase mark are separate
    // facts and only the first one is refused here.
    LocalScript sc;
    sc.s.roundCounter = 0;
    sc.Push();  // baseline
    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push();  // admitted: ROUND_BEGIN stands for this change, so it is NOT also a PHASE
    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 1);
    CHECK(CountKind(sc.marks, kMarkPhase) == 0);

    sc.Phase(gs::Phase::TeamRoomWait, kSymTeamRoom);
    sc.Push(kSecond);
    const uint32_t phasesBefore = CountKind(sc.marks, kMarkPhase);
    const size_t marksBefore = sc.marks.size();

    sc.Phase(gs::Phase::RoundStart, kSymRoundStart);
    sc.Push(kSecond);  // refused: 0 does not ascend past 0

    REQUIRE(sc.src.GetStats().refusedNonAscending == 1);
    CHECK(CountKind(sc.marks, kMarkRoundBegin) == 1);  // the refusal cost the BOUNDARY
    REQUIRE(sc.marks.size() == marksBefore + 1);       // and only the boundary
    const MarkRec& m = sc.marks.back();
    CHECK(m.kind == kMarkPhase);
    CHECK(m.payload == kSymRoundStart);
    CHECK(m.tUs == sc.tUs);
    CHECK(CountKind(sc.marks, kMarkPhase) == phasesBefore + 1);
}
