// The observer's arms, driven offline.
//
// Every input these rows read only exists inside a running game, which is exactly why the battery
// has to be provable without one: a launch is the most expensive test in this project, and an arm
// that cannot tell asked-pass from could-not-ask would spend one and answer nothing.
//
// Each row is driven three ways: the line/counter that makes it PASS, the one that makes it FAIL,
// and the absence that makes it COULD-NOT-ASK. The three must render differently -- that is
// And a two-state gate is the defect it names.
#include <doctest.h>

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../src/generated/facts_generated.h"
#include "../../tests/replay/observer_rows.h"

using namespace probe_replay;

namespace {

// The producers' real lines, in the log's real shape: `HH:MM:SS.mmm LEVEL [tag] message`.
const char* kBound =
    "12:00:01.000 INFO [entity] bound game space 000001F0 (netGame 000002A0, systems 7ff6 cap=64 "
    "count=12 entries 90): entity available; local pawn resolved; publisher direct";
const char* kBoundUnavailable =
    "12:00:01.000 INFO [entity] bound game space 000001F0 (netGame 000002A0, systems 7ff6 cap=64 "
    "count=12 entries 90): entity Resolve::PoolNotReady; local pawn none; publisher none";
// src/engine/session.cpp:593 -- the level-load predicate's producer. The state a transition
// arrives at is the one that answers, which is why the arrow half is what the reader takes.
const char* kStateLobby =
    "04:16:38.042 INFO [session] NetGame state 4 (loading global) -> 5 (lobby)";
const char* kStateLoadingLevel =
    "04:16:38.073 INFO [session] NetGame state 5 (lobby) -> 7 (loading level)";
// VERBATIM from the RECORDED L5 re-run 2 log,
// launch run L5-fc499e1 20260902-081617Z, its gamelogs/echoreplay.log,
// line 320 and line 583 (the LAST of its 239 `arm BROWSER` heartbeats, which is the sample the
// anchor is taken from). 15398073 - (15618698 - 239930) = 19305 ms.
const char* kRun2LoadingLevel =
    "04:16:38.073 INFO  [session] NetGame state 5 (lobby) -> 7 (loading level)";
const char* kRun2FirstSurvive =
    "04:16:18.792 INFO  [observer] SURVIVE 30 ms, heartbeat 1, arm BROWSER";
const char* kRun2LastSurvive =
    "04:20:18.698 INFO  [observer] SURVIVE 239930 ms, heartbeat 239, arm BROWSER";
const char* kStateInGame =
    "04:16:42.901 INFO [session] NetGame state 8 (ready for game) -> 9 (in game)";
// The observer's own heartbeat. It is the ONLY line in the log that carries both clocks -- a wall
// stamp and the SinceStartMs() the probe's stamps are on -- so it is the anchor everything else is
// converted through (probe_replay.cpp:975 writes it).
const char* kSurvive =
    "04:16:33.073 INFO [observer] SURVIVE 5000 ms, heartbeat 6, arm BROWSER";
// VERBATIM from the RECORDED L5 re-run 3 log,
// launch run L5-8abcbbf 20260902-165645Z, its gamelogs/echoreplay.log
// lines 330 and 342. Together they say: the level began loading 19340 ms into the observer's
// window (46626393 - (46632284 - 25231)), and OpenBrowser() was called at 5774 ms.
const char* kL5LoadingLevel =
    "12:57:06.393 INFO  [session] NetGame state 5 (lobby) -> 7 (loading level)";
const char* kL5Survive =
    "12:57:12.284 INFO  [observer] SURVIVE 25231 ms, heartbeat 26, arm BROWSER";
// The RECORDED L5 re-run 3 refusal line with exactly ONE edit: 162027c renamed the noun, so the
// two `game space` occurrences read `level space` here. The recorded bytes survive verbatim as
// kCameraRefusedOldNoun (bottom of this file), which is what proves the old spelling no longer
// parses -- this fixture is no longer byte-for-byte what that launch printed.
const char* kL5CameraRefused =
    "12:57:06.393 WARN  [camera] camera control refused rig 0000027155760000 on level space "
    "0000027118DA0000 (netGame 000002703B046180): the spectator CS is not in level space "
    "0000027118DA0000's system table; pose-owner: none, conflicts 0, arb created#1, rel-refused 0";
const char* kRebound =
    "12:00:02.000 INFO [playback] services re-bound: entity live (available, bind epoch 3), pose "
    "live, netvar live";
const char* kReboundNetvarNull =
    "12:00:02.000 INFO [playback] services re-bound: entity live (available, bind epoch 3), pose "
    "live, netvar null";
// Both edges of `ad17dd9`'s pair print this line, and the trigger it names is the trailing token.
const char* kBodies =
    "12:00:03.000 INFO [playback] body handles re-bound: 12 bound, 0 absent this session, 0 not net "
    "ids, 0 unresolvable (entity re-bound)";
const char* kBodiesWindowEdge =
    "12:00:03.000 INFO [playback] body handles re-bound: 12 bound, 0 absent this session, 0 not net "
    "ids, 0 unresolvable (window rebuilt)";
const char* kBodiesBad =
    "12:00:03.000 INFO [playback] body handles re-bound: 9 bound, 1 absent this session, 0 not net "
    "ids, 3 unresolvable (window rebuilt)";
const char* kRosterFull = "12:00:04.000 INFO [identity] roster: 2/2 players dressed, 0 NOT dressed";
const char* kRosterShort = "12:00:04.000 ERROR [identity] roster: 1/2 players dressed, 1 NOT dressed";
// The other THREE lines src/identity/dresser.cpp prints under the same tag and the same `roster: `
// prefix. The mapped one (:409) is UNCONDITIONAL and printed immediately after the dressed line, so
// the log the product actually produces is always at least a PAIR -- which is why a fixture holding
// the dressed line alone is a fixture that cannot fail.
const char* kRosterMapped = "12:00:04.001 INFO [identity] roster: 2/2 players mapped to a live user index";
const char* kRosterMappedNone = "12:00:04.001 ERROR [identity] roster: 0/2 players mapped to a live user index";
const char* kRosterResidue = "12:00:04.002 ERROR [identity] roster: 2 residue slot(s) from an earlier load, 2 vacated, 0 stale-slot, 0 vacate-refused, 1 dress-refused";
const char* kRosterReleased = "12:00:09.000 INFO [identity] roster: 2/2 replay users released, 0 refused";
const char* kPanelOwners =
    "12:00:05.000 INFO [ui] loaded 2 players | owners: 2/2 mapped | netvars: applied | rate 1.00x";
// The state sentences are src/playback/core/apply.cpp's own, VERBATIM -- `STORED-NOT-APPLIED` was
// a spelling no producer ever wrote, and a fixture that invents the input can only test itself.
const char* kPanelOwnersRefused =
    "12:00:05.000 INFO [ui] loaded 2 players | owners: 1/2 mapped | INV-OWN1 REFUSED | netvars: "
    "STORED, NOT APPLIED (no apply path on this build) | rate 1.00x";
const char* kPanelNoNetvarRecords =
    "12:00:05.000 INFO [ui] loaded 2 players | owners: 2/2 mapped | netvars: no netvar records in "
    "this capture (nothing to apply) | rate 1.00x";
const char* kPanelNoneDueHere =
    "12:00:05.000 INFO [ui] loaded 2 players | owners: 2/2 mapped | netvars: records present, none "
    "due at this playhead | rate 1.00x";
// The RETIRED spelling, kept as a fixture on purpose: this is what the row used to key on.
const char* kPanelNetvarsNone =
    "12:00:05.000 INFO [ui] loaded 2 players | owners: 2/2 mapped | netvars: none | rate 1.00x";
const char* kArmed =
    "12:00:06.000 INFO [playback] INV-OWN1 armed against local index 0 after 42 ticks";
// The other TWO lines src/playback/core/playback_module.cpp prints about the INV-OWN1 arm, copied
// from its format strings: :228, the ordinary arm at Load, which is what a healthy session prints
// and what a fixture holding kArmed alone can never see; and :234, the arm against nothing.
const char* kArmedAtLoad =
    "12:00:06.000 INFO [playback] INV-OWN1 armed against this session's own local user index 0";
const char* kArmedLocalUnknown =
    "12:00:06.000 WARN [playback] INV-OWN1 armed with NO local user index (None): the is-local-user "
    "arm refuses nobody until one exists; every other arm still runs. A replay body published onto "
    "this machine's own entrant would not be caught here. The tick asks again every frame and "
    "re-arms on the first answer";
const char* kCameraGood =
    "12:00:07.000 INFO [camera] camera control bound: rig 00000AA0 on level space 000001F0 (netGame "
    "000002A0, systems at 7ff6); rig enabled flag 1; pose-owner: replay, conflicts 0; modes drive "
    "from the panel and `cam`";
const char* kCameraDisabledRig =
    "12:00:07.000 INFO [camera] camera control bound: rig 00000AA0 on level space 000001F0 (netGame "
    "000002A0, systems at 7ff6); rig enabled flag 0; pose-owner: replay, conflicts 0; modes drive "
    "from the panel and `cam`";
const char* kCameraRefused =
    "12:00:07.000 WARN [camera] camera control refused rig 0 on level space 000001F0 (netGame "
    "000002A0): spectator CS disagrees; pose-owner: none, conflicts 2";
const char* kRateLine = "12:00:08.000 INFO [audio] transport rate 0.50x: sink commanded, deviceRatio 0.50";
const char* kClosed =
    "12:00:09.000 INFO [capture] closed: 4211 records, 3 chapters, 12 keyframes, 0 verbatim, 0 refusals";

char g_detail[1024];

Ask Ask1(const char* id, const LogFacts& f, const StatusFacts& s) {
    Ask out = Ask::CouldNotAsk;
    REQUIRE(EvaluateRow(id, f, s, &out, g_detail, sizeof g_detail));
    return out;
}

bool DetailMentions(const char* needle) { return strstr(g_detail, needle) != nullptr; }

}  // namespace

// The lines of AUDIT_L5_RUN_4's own record, quoted. `[boot] attached` is bootstrap::Attach's line
// and is written exactly once per launch, as the first line the plugin writes.
// Its wall stamp is not read arithmetically anywhere: the anchor's job is IDENTITY -- which
// launch these lines belong to -- and the launch's own clock comes from the SURVIVE heartbeats.
static const char* const kBootAttached = "16:55:57.501 INFO  [boot] attached";
static const char* const kStaleLevelL5Run1 =
    "04:16:38.073 INFO  [session] NetGame state 5 (lobby) -> 7 (loading level)";
static const char* const kThisLevelL5Run4 =
    "16:56:19.079 INFO  [session] NetGame state 5 (lobby) -> 7 (loading level)";
static const char* const kSurviveL5Run4 =
    "16:59:57.876 INFO  [observer] SURVIVE 239879 ms, heartbeat 239, arm BROWSER";

TEST_CASE("the line splitter refuses anything that is not one of our lines") {
    const char* tag = nullptr;
    const char* msg = nullptr;
    size_t n = 0;
    CHECK_FALSE(SplitLogLine("a line with no bracket at all", &tag, &n, &msg));
    CHECK_FALSE(SplitLogLine("12:00:00.000 INFO [unterminated message", &tag, &n, &msg));
    REQUIRE(SplitLogLine(kBound, &tag, &n, &msg));
    CHECK(TagIs(tag, n, "entity"));
    CHECK(StartsWith(msg, "bound game space "));
}

TEST_CASE("a token must sit on a boundary, so a fragment inside a word never matches") {
    // The commonest false pass in this project is a substring (9.5). `xowners:` is the near miss.
    CHECK(FindToken("a | owners: 2/2 mapped", "owners: ") != nullptr);
    CHECK(FindToken("xowners: 2/2 mapped", "owners: ") == nullptr);
    CHECK(FindToken("owners: 2/2 mapped", "owners: ") != nullptr);
}

TEST_CASE("W1-E entity-available scores the LIVE surface, not the last log line (L2-RUN-2 F5)") {
    LogFacts empty;
    LogFacts good;
    good.OnLine(kBound);
    LogFacts bad;
    bad.OnLine(kBoundUnavailable);

    // No live sample: the log tail is reported and NOT scored, in both directions. Without this the
    // row would quietly go on grading history the moment the sample stopped arriving.
    StatusFacts unsampled;
    CHECK(Ask1("w1e-entity-available-on-bound-space", empty, unsampled) == Ask::CouldNotAsk);
    CHECK(DetailMentions("has not reported a binding"));
    CHECK(Ask1("w1e-entity-available-on-bound-space", bad, unsampled) == Ask::CouldNotAsk);
    CHECK(DetailMentions("history, not a"));

    // â˜… THE L2 #2 CASE ITSELF: the last [entity] line carries a refusal, the surface is available
    // when the row is scored. The old row filed asked-fail here on a 1.2 s old transient.
    StatusFacts live;
    live.entitySampled = true;
    live.entityAvailable = true;
    CHECK(Ask1("w1e-entity-available-on-bound-space", bad, live) == Ask::Pass);
    CHECK(DetailMentions("reports AVAILABLE when scored"));
    CHECK(Ask1("w1e-entity-available-on-bound-space", good, live) == Ask::Pass);

    // The converse, or the row would pass anything with a live sample: a log line saying available
    // cannot carry it past a surface that is not.
    StatusFacts dead;
    dead.entitySampled = true;
    dead.entityAvailable = false;
    _snprintf_s(dead.entityDiagnostic, sizeof dead.entityDiagnostic, _TRUNCATE,
                "Resolve::PoolNotReady");
    CHECK(Ask1("w1e-entity-available-on-bound-space", good, dead) == Ask::Fail);
    CHECK(DetailMentions("Resolve::PoolNotReady"));

    // Nothing was ever bound, so an unavailable surface is not a binding that failed.
    CHECK(Ask1("w1e-entity-available-on-bound-space", empty, dead) == Ask::CouldNotAsk);
    CHECK(DetailMentions("nothing was bound"));
}

TEST_CASE("W1-F inbound: the product's four words reach the row and the two non-scoring ones stay apart") {
    LogFacts f;

    StatusFacts unread;
    CHECK(Ask1("w1f-inbound-loopback-only", f, unread) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never sampled"));

    StatusFacts good;
    good.inboundSampled = true;
    good.inboundArmed = true;
    good.inboundEndpointsRead = true;
    good.inboundRewritten = 3;
    good.inboundAlreadyLoopback = 1;
    _snprintf_s(good.inboundVerdict, sizeof good.inboundVerdict, _TRUNCATE, "asked-pass");
    CHECK(Ask1("w1f-inbound-loopback-only", f, good) == Ask::Pass);
    CHECK(DetailMentions("rewritten=3"));

    StatusFacts exposed = good;
    exposed.inboundBoundNonLoopback = 1;
    _snprintf_s(exposed.inboundVerdict, sizeof exposed.inboundVerdict, _TRUNCATE, "asked-fail");
    CHECK(Ask1("w1f-inbound-loopback-only", f, exposed) == Ask::Fail);
    CHECK(DetailMentions("NON-LOOPBACK"));

    // â˜… THE ANTI-VACUITY CLAUSE, MIRRORED (inbound.cpp:189). Armed, endpoints read, and the pair at
    // zero: the product grades that could-not-ask because it is indistinguishable from a launch
    // this hook protected. It must not read as a pass and must not read as never-asked either.
    StatusFacts vacuous;
    vacuous.inboundSampled = true;
    vacuous.inboundArmed = true;
    vacuous.inboundEndpointsRead = true;
    _snprintf_s(vacuous.inboundVerdict, sizeof vacuous.inboundVerdict, _TRUNCATE, "could-not-ask");
    CHECK(Ask1("w1f-inbound-loopback-only", f, vacuous) == Ask::CouldNotAsk);
    CHECK(DetailMentions("could-not-ask: the hook armed"));
    CHECK(DetailMentions("the rewrite was never exercised"));
    CHECK(!DetailMentions("not armed on this launch"));

    // never-asked shares a SCORING word with could-not-ask and no other part of the sentence.
    StatusFacts never;
    never.inboundSampled = true;
    _snprintf_s(never.inboundVerdict, sizeof never.inboundVerdict, _TRUNCATE, "never-asked");
    CHECK(Ask1("w1f-inbound-loopback-only", f, never) == Ask::CouldNotAsk);
    CHECK(DetailMentions("not armed on this launch"));
    CHECK(!DetailMentions("the hook armed"));

    // A word this reader is behind on is refused BY NAME, never defaulted into a verdict.
    StatusFacts alien;
    alien.inboundSampled = true;
    _snprintf_s(alien.inboundVerdict, sizeof alien.inboundVerdict, _TRUNCATE, "asked-maybe");
    CHECK(Ask1("w1f-inbound-loopback-only", f, alien) == Ask::CouldNotAsk);
    CHECK(DetailMentions("behind on"));
    CHECK(DetailMentions("asked-maybe"));
}

TEST_CASE("W2-B netvar liveness and the applied state are two separate answers") {
    StatusFacts s;
    LogFacts empty;
    CHECK(Ask1("w2b-services-rebound-netvar-live", empty, s) == Ask::CouldNotAsk);
    CHECK(Ask1("w2b-netvars-applied", empty, s) == Ask::CouldNotAsk);

    LogFacts good;
    good.OnLine(kRebound);
    good.OnLine(kPanelOwners);
    CHECK(Ask1("w2b-services-rebound-netvar-live", good, s) == Ask::Pass);
    CHECK(Ask1("w2b-netvars-applied", good, s) == Ask::Pass);

    // 8.3a row 2: a refusal WITH netvar live is inside INetVar::Write, not in the binding, and the
    // detail must carry both halves or the distinction the read exists for is lost.
    LogFacts refusedWithLiveNetvar;
    refusedWithLiveNetvar.OnLine(kRebound);
    refusedWithLiveNetvar.OnLine(kPanelOwnersRefused);
    CHECK(Ask1("w2b-netvars-applied", refusedWithLiveNetvar, s) == Ask::Fail);
    CHECK(DetailMentions("STORED, NOT APPLIED"));
    CHECK(DetailMentions("netvar 'live'"));

    // ---- W2-B NETVARS-B `ad17dd9`: four published sentences, and only ONE of them is a fail.
    // Each arm is asserted BY ITS TEXT and not by its verdict alone: three of these four answer
    // the same could-not-ask, so a row that lost the distinction would keep every verdict it has.
    LogFacts noRecords;
    noRecords.OnLine(kRebound);
    noRecords.OnLine(kPanelNoNetvarRecords);
    CHECK(Ask1("w2b-netvars-applied", noRecords, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("this capture carries no netvar records"));
    CHECK(DetailMentions("by construction"));
    CHECK_FALSE(DetailMentions("behind on"));

    LogFacts noneDue;
    noneDue.OnLine(kRebound);
    noneDue.OnLine(kPanelNoneDueHere);
    CHECK(Ask1("w2b-netvars-applied", noneDue, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("none due at the sampled playhead"));
    CHECK_FALSE(DetailMentions("behind on"));

    // The retired word. The product has not printed `netvars: none` since `ad17dd9`; a reader that
    // still answered on it would be scoring a word nothing writes, and one that quietly passed an
    // unrecognised word would be worse. It is named as unrecognised, and it is not a fail either.
    LogFacts retired;
    retired.OnLine(kRebound);
    retired.OnLine(kPanelNetvarsNone);
    CHECK(Ask1("w2b-netvars-applied", retired, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("behind on"));
    CHECK(DetailMentions("published 'none'"));

    LogFacts nullNetvar;
    nullNetvar.OnLine(kReboundNetvarNull);
    CHECK(Ask1("w2b-services-rebound-netvar-live", nullNetvar, s) == Ask::Fail);
}

TEST_CASE("W2-B body handles: bound with none unresolvable, or it is a fail") {
    StatusFacts s;
    LogFacts empty, good, bad;
    good.OnLine(kBodies);
    bad.OnLine(kBodiesBad);
    CHECK(Ask1("w2b-body-handles-rebound", empty, s) == Ask::CouldNotAsk);
    // `ad17dd9` binds from BOTH edges, so an absent line is no longer one named path not taken.
    CHECK(DetailMentions("neither 'window rebuilt' nor 'entity re-bound'"));
    CHECK_FALSE(DetailMentions("and it never ran"));

    CHECK(Ask1("w2b-body-handles-rebound", good, s) == Ask::Pass);
    CHECK(DetailMentions("the edge that closed it: entity re-bound"));

    // The other edge's sentence is the same answer, and the row says which one it was.
    LogFacts windowEdge;
    windowEdge.OnLine(kBodiesWindowEdge);
    CHECK(Ask1("w2b-body-handles-rebound", windowEdge, s) == Ask::Pass);
    CHECK(DetailMentions("the edge that closed it: window rebuilt"));

    CHECK(Ask1("w2b-body-handles-rebound", bad, s) == Ask::Fail);
    CHECK(DetailMentions("3 unresolvable"));
}

TEST_CASE("W2-C roster, owners and the INV-OWN1 arming") {
    StatusFacts s;
    LogFacts empty, full, shortRoster;
    // The product's REAL sequence, in the order dresser.cpp prints it: dressed, then the
    // unconditional mapped line, then the residue line and the unload's released line. A prefix
    // match reads the LAST of these as the dressed line.
    full.OnLine(kRosterFull);
    full.OnLine(kRosterMapped);
    full.OnLine(kRosterResidue);
    full.OnLine(kRosterReleased);
    shortRoster.OnLine(kRosterShort);
    shortRoster.OnLine(kRosterMapped);
    CHECK(Ask1("w2c-roster-players-dressed", empty, s) == Ask::CouldNotAsk);
    CHECK(Ask1("w2c-roster-players-dressed", full, s) == Ask::Pass);
    CHECK(Ask1("w2c-roster-players-dressed", shortRoster, s) == Ask::Fail);

    // AUDIT_P3_ARMS 1, the exact reproduction: nobody dressed and everybody mapped. Prefix matching
    // scored the mapped line and rendered asked-PASS over a roster where every body publishes
    // wearing the wrong skin.
    LogFacts noneDressed;
    noneDressed.OnLine("12:00:04.000 ERROR [identity] roster: 0/2 players dressed, 2 NOT dressed");
    noneDressed.OnLine(kRosterMapped);
    CHECK(Ask1("w2c-roster-players-dressed", noneDressed, s) == Ask::Fail);
    CHECK(DetailMentions("players dressed"));
    CHECK(DetailMentions("2 of 2 players are NOT dressed"));
    CHECK(noneDressed.rosterDressed == 0u);
    CHECK(noneDressed.rosterPlayers == 2u);

    // Each sibling ALONE is could-not-ask naming what it read, never a pass and never a fail: the
    // question was not asked, because none of these lines is about dressing.
    const char* siblings[] = {kRosterMapped, kRosterMappedNone, kRosterResidue, kRosterReleased};
    for (const char* line : siblings) {
        LogFacts only;
        only.OnLine(line);
        CHECK(Ask1("w2c-roster-players-dressed", only, s) == Ask::CouldNotAsk);
        CHECK(DetailMentions("none of them is the dressed line"));
        CHECK(only.rosterSeen == false);
        CHECK(only.rosterOtherCount == 1u);
    }

    // The tail is load-bearing: sscanf returns on assignment count alone, so a third number
    // followed by anything else must not be read as this line.
    LogFacts decoy;
    decoy.OnLine("12:00:04.000 INFO [identity] roster: 2/2 players dressed, 2 mapped to a live user index");
    CHECK(decoy.rosterSeen == false);
    CHECK(Ask1("w2c-roster-players-dressed", decoy, s) == Ask::CouldNotAsk);

    LogFacts owners, ownersBad;
    owners.OnLine(kPanelOwners);
    ownersBad.OnLine(kPanelOwnersRefused);
    CHECK(Ask1("w2c-owners-mapped", empty, s) == Ask::CouldNotAsk);
    CHECK(Ask1("w2c-owners-mapped", owners, s) == Ask::Pass);
    CHECK(Ask1("w2c-owners-mapped", ownersBad, s) == Ask::Fail);

    // The arming is an event: no owners at all is could-not-ask, owners with no arming is a fail.
    CHECK(Ask1("w2c-invown1-armed-against-local", empty, s) == Ask::CouldNotAsk);
    CHECK(Ask1("w2c-invown1-armed-against-local", owners, s) == Ask::Fail);
    LogFacts armed;
    armed.OnLine(kPanelOwners);
    armed.OnLine(kArmed);
    CHECK(Ask1("w2c-invown1-armed-against-local", armed, s) == Ask::Pass);

    // AUDIT_P3_ARMS_R2 2, the exact reproduction: the ORDINARY arm at Load
    // (playback_module.cpp:228) is the line a healthy session prints, and the row used to match
    // only the retry line (:314), so a correctly armed session rendered asked-FAIL.
    LogFacts armedOrdinary;
    armedOrdinary.OnLine(kArmedAtLoad);
    armedOrdinary.OnLine(kPanelOwners);
    CHECK(armedOrdinary.invOwn1Armed == true);
    CHECK(Ask1("w2c-invown1-armed-against-local", armedOrdinary, s) == Ask::Pass);
    CHECK(DetailMentions("this session's own local user index 0"));

    // The third product line is an arm against NOTHING. On its own it is this row's asked-FAIL -
    // the session ran with the is-local-user arm off - and it must not be read as a silence.
    LogFacts armedNothing;
    armedNothing.OnLine(kArmedLocalUnknown);
    armedNothing.OnLine(kPanelOwners);
    CHECK(armedNothing.invOwn1Armed == false);
    CHECK(armedNothing.invOwn1LocalUnknownArm == true);
    CHECK(Ask1("w2c-invown1-armed-against-local", armedNothing, s) == Ask::Fail);
    CHECK(DetailMentions("armed against NOTHING"));

    // ...and the degraded-then-recovered session is the one case where both appear. The retry line
    // is the answer, so it is a Pass.
    LogFacts recovered;
    recovered.OnLine(kArmedLocalUnknown);
    recovered.OnLine(kPanelOwners);
    recovered.OnLine(kArmed);
    CHECK(Ask1("w2c-invown1-armed-against-local", recovered, s) == Ask::Pass);

    // Negative controls, one per matcher: a [playback] line sharing the INV-OWN1 head but none of
    // the three forms, and the panel's own INV-OWN1 word under a different tag.
    const char* notArming[] = {
        "12:00:06.000 INFO [playback] INV-OWN1 refused 1 entrant that resolved to the local index",
        "12:00:06.000 INFO [playback] INV-OWN1 armed against nothing in particular",
        kPanelOwnersRefused};
    for (const char* line : notArming) {
        LogFacts only;
        only.OnLine(line);
        CHECK(only.invOwn1Armed == false);
        CHECK(only.invOwn1LocalUnknownArm == false);
    }
}

TEST_CASE("W2-E camera control: a disabled rig and a refusal are both fails, silence is not") {
    StatusFacts s;
    LogFacts empty, good, disabled, refused;
    good.OnLine(kCameraGood);
    disabled.OnLine(kCameraDisabledRig);
    refused.OnLine(kCameraRefused);
    CHECK(Ask1("w2e-camera-control-bound", empty, s) == Ask::CouldNotAsk);
    CHECK(Ask1("w2e-camera-control-bound", good, s) == Ask::Pass);
    CHECK(Ask1("w2e-camera-control-bound", disabled, s) == Ask::Fail);
    CHECK(DetailMentions("rig enabled flag 0"));
    CHECK(Ask1("w2e-camera-control-bound", refused, s) == Ask::Fail);
    CHECK(DetailMentions("refused rather than binding"));
}

// The producers' own types, so a fixture states the outcome the product would state. The row keys
// on these; the char buffers it also carries are display text and decide nothing.
using VP = replay::camera::ViewPoseRead;
using PM = replay::camera_control::PoseMotion;

TEST_CASE("RULING 4b is asked through the product's own view read, and refuses in named ways") {
    LogFacts f;
    StatusFacts noFile;
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, noFile) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no loaded file"));

    StatusFacts paused;
    paused.transportPresent = true;
    paused.durationUs = 300000000;
    paused.pausedForView = true;
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, paused) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no view-pose pair was taken"));

    // A refusal blanks the pose, so its zeros must never be scored as "did not move". Both of the
    // camera's refusal words are driven, because they mean different things.
    StatusFacts notBound = paused;
    notBound.viewPoseSampled = true;
    notBound.viewReadFirstCode = VP::Ok;
    notBound.viewReadSecondCode = VP::NotBound;
    strcpy_s(notBound.viewReadSecond, "not-bound");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, notBound) == Ask::CouldNotAsk);
    CHECK(DetailMentions("not-bound"));
    CHECK(DetailMentions("a refusal blanks the pose"));
    StatusFacts unreadable = notBound;
    unreadable.viewReadSecondCode = VP::Unreadable;
    strcpy_s(unreadable.viewReadSecond, "unreadable");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, unreadable) == Ask::CouldNotAsk);
    CHECK(DetailMentions("unreadable"));

    // W1-H's fourth word. The ENGINE had nothing built and the plugin DID ask, which is not the
    // plugin being idle -- the two used to share NotBound and the detail must keep them apart.
    StatusFacts noViewState = notBound;
    noViewState.viewReadSecondCode = VP::NoViewState;
    strcpy_s(noViewState.viewReadSecond, "no-view-state");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, noViewState) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no-view-state"));
    CHECK(DetailMentions("the ENGINE had built no view state 0, and the plugin did ask"));

    // ENG-VIEWREAD-R2's control: the CamelCase words this row used to strcmp are INERT now. A
    // fixture that hand-writes "Ok" into the display buffer while the producer's enum says
    // NotBound still refuses, so no fixture can spell its way to a verdict.
    StatusFacts handWritten = notBound;
    strcpy_s(handWritten.viewReadFirst, "Ok");
    strcpy_s(handWritten.viewReadSecond, "Ok");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, handWritten) == Ask::CouldNotAsk);
    CHECK(DetailMentions("GetViewPose refused"));

    StatusFacts read = paused;
    read.viewPoseSampled = true;
    read.viewReadFirstCode = VP::Ok;
    read.viewReadSecondCode = VP::Ok;
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, read) == Ask::CouldNotAsk);
    CHECK(DetailMentions("not report TransportState::Pause at both"));

    StatusFacts held = read;
    held.viewPausedAcross = true;
    held.viewSourceCode = replay::ViewPoseSource::CameraInstance;
    strcpy_s(held.viewSource, "camera-instance");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, held) == Ask::CouldNotAsk);
    CHECK(DetailMentions("not the view state"));

    StatusFacts armSet = held;
    armSet.viewSourceCode = replay::ViewPoseSource::ViewState;
    strcpy_s(armSet.viewSource, "view-state");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, armSet) == Ask::CouldNotAsk);
    CHECK(DetailMentions("integratorArmedKnown=0"));
    StatusFacts disarmed = armSet;
    disarmed.viewGateKnown = true;
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, disarmed) == Ask::CouldNotAsk);
    CHECK(DetailMentions("reachable and DISARMED"));

    StatusFacts armed = armSet;
    armed.viewGateKnown = true;
    armed.viewGateArmed = 1;
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, armed) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no-steering"));

    // Steering declared: now the row discriminates. A frozen view is a design finding against 4b.
    StatusFacts steered = armed;
    steered.viewSteeringDeclared = true;
    steered.viewSeconds = 0.3;
    steered.viewMetres = 0.42;
    steered.viewMotionCode = PM::Moved;
    strcpy_s(steered.viewMotion, "moved");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, steered) == Ask::Pass);
    CHECK(DetailMentions("0.4200 m"));

    StatusFacts frozen = steered;
    frozen.viewMotionCode = PM::Still;
    strcpy_s(frozen.viewMotion, "still");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, frozen) == Ask::Fail);
    CHECK(DetailMentions("did NOT move"));

    // The library's own third outcome is not a fail either.
    StatusFacts incomparable = steered;
    incomparable.viewMotionCode = PM::CouldNotAsk;
    strcpy_s(incomparable.viewMotion, "could-not-ask");
    CHECK(Ask1("w2e-4b-view-moves-while-paused", f, incomparable) == Ask::CouldNotAsk);
    CHECK(DetailMentions("one comparable span"));

    // The whole matrix: each of PoseMotionBetween's three words against steering declared and not.
    // On an UNATTENDED launch nobody steers the free camera, so every word is could-not-ask -- and
    // `Still` in particular must not become a design finding against 4b. This is what pins the
    // steering gate ABOVE the Moved/Still branches rather than below them.
    const PM motions[] = {PM::Moved, PM::Still, PM::CouldNotAsk};
    const Ask attendedWants[] = {Ask::Pass, Ask::Fail, Ask::CouldNotAsk};
    for (int i = 0; i < 3; ++i) {
        StatusFacts watched = steered;
        watched.viewMotionCode = motions[i];
        CHECK(Ask1("w2e-4b-view-moves-while-paused", f, watched) == attendedWants[i]);

        StatusFacts alone = watched;
        alone.viewSteeringDeclared = false;
        CHECK(Ask1("w2e-4b-view-moves-while-paused", f, alone) == Ask::CouldNotAsk);
        CHECK(DetailMentions("no-steering"));
    }
}

// Every csregister fixture below must publish the frozen ` [threads owner=<n> callback=<n>]`
// suffix, because a row that cannot read it answers could-not-ask before it judges anything. The
// helper exists so that a fixture cannot accidentally satisfy the suffix clause by omission.
namespace {
// Arbitrary fixture thread ids. The row only ever compares these two and prints them back, so any
// distinct pair does; they are deliberately not 1000/2000, because 1000 is 0x3E8 -- a real fact's
// value -- and a fixture that spells a fact value is suppressed by the transcription scanner in the
// same channel a genuine transcription would hide in.
const unsigned kFixtureOwnerThread = 7717;
const unsigned kFixtureCallbackThread = 7723;

void PublishThreads(StatusFacts& s, unsigned owner, unsigned callback) {
    s.csOwnerThreadId = owner;
    s.csCallbackThreadId = callback;
    s.csThreadsPublished = true;
    s.csOwnerThreadIdPublished = owner;
    s.csCallbackThreadIdPublished = callback;
}
}  // namespace

TEST_CASE("W2-F: registrationInvocations 0 with staleInvocations > 0 is its own loud outcome") {
    LogFacts f;
    StatusFacts never;
    CHECK(Ask1("w2f-csregister-epoch", f, never) == Ask::CouldNotAsk);

    StatusFacts deferring;
    deferring.csAsked = true;
    PublishThreads(deferring, kFixtureOwnerThread, kFixtureCallbackThread);
    strcpy_s(deferring.csStatus, "NotRegistered");
    deferring.csDeferrals = 900;
    CHECK(Ask1("w2f-csregister-epoch", f, deferring) == Ask::CouldNotAsk);
    CHECK(DetailMentions("deferrals=900"));

    StatusFacts running;
    running.csAsked = true;
    running.csRunning = true;
    strcpy_s(running.csStatus, "Running");
    running.csInvocations = 400; // facts-ok: AUDIO.LISTENER_COUNT_OFF a call count, not that offset
    running.csRegistrationInvocations = 400; // facts-ok: AUDIO.LISTENER_COUNT_OFF a call count, not that offset
    running.csRegistrations = 1;
    running.csSpaceChanges = 1;
    running.csSpacesSeen = 1;
    running.csMarshalledDraws = 380;
    running.csCoalescedInvocations = 20;
    PublishThreads(running, kFixtureOwnerThread, kFixtureCallbackThread);
    CHECK(Ask1("w2f-csregister-epoch", f, running) == Ask::Pass);

    // ARCH-13 ruling 1 (S1.2): DL-07 passes on `Running` OR `MarshalStale` OR `CallbackSilent`.
    // Those two words are recency diagnostics reachable ONLY from a session that already satisfied
    // all three legs, so a session that drew a thousand times and then went quiet is a pass with
    // its quiet stated -- never a FAIL of S8.5.4a's definition. The row scores the parts and reads
    // neither the word nor csRunning, so these hold today; they are pinned here so that a rewrite
    // to `status == Running` reds instead of silently narrowing DL-07.
    for (const char* word : {"MarshalStale", "CallbackSilent"}) {
        StatusFacts quiet = running;
        quiet.csRunning = false;
        strcpy_s(quiet.csStatus, word);
        CHECK(Ask1("w2f-csregister-epoch", f, quiet) == Ask::Pass);
    }
    CHECK(DetailMentions("[threads owner=7717 callback=7723]"));
    CHECK(DetailMentions("marshalledDraws=380"));
    CHECK(DetailMentions("registrationInvocations=400"));
    CHECK(DetailMentions("spaceChanges=1"));

    StatusFacts staleOnly;
    staleOnly.csAsked = true;
    strcpy_s(staleOnly.csStatus, "Registered");
    staleOnly.csInvocations = 77;
    staleOnly.csRegistrationInvocations = 0;
    staleOnly.csStaleInvocations = 77;
    staleOnly.csRegistrations = 2;
    staleOnly.csSpacesSeen = 2;
    staleOnly.csMarshalledDraws = 70;
    staleOnly.csCoalescedInvocations = 7;
    PublishThreads(staleOnly, kFixtureOwnerThread, kFixtureCallbackThread);
    CHECK(Ask1("w2f-csregister-epoch", f, staleOnly) == Ask::Fail);
    CHECK(DetailMentions("STALE-DISPATCH-ONLY"));
}

// AUDIT_L5_RUN3 M1 / section 12 item 3, and ARCH-11 S5.
TEST_CASE("W2-F: the epoch row reads both thread ids, cross-checks them, and names a direction") {
    LogFacts f;

    // The suffix is W2-F's contract: "on every line without exception". Its ABSENCE is a broken
    // contract, so the row refuses rather than reporting a quiet 0/0.
    StatusFacts noSuffix;
    noSuffix.csAsked = true;
    noSuffix.csRunning = true;
    strcpy_s(noSuffix.csStatus, "Running");
    noSuffix.csInvocations = 400; // facts-ok: AUDIO.LISTENER_COUNT_OFF a call count, not that offset
    noSuffix.csRegistrationInvocations = 400; // facts-ok: AUDIO.LISTENER_COUNT_OFF a call count, not that offset
    noSuffix.csMarshalledDraws = 400; // facts-ok: AUDIO.LISTENER_COUNT_OFF a call count, not that offset
    noSuffix.csRegistrations = 1;
    noSuffix.csSpacesSeen = 1;
    CHECK(Ask1("w2f-csregister-epoch", f, noSuffix) == Ask::CouldNotAsk);
    CHECK(DetailMentions("frozen ' [threads owner="));

    // ONE fact, TWO holders: the Report struct and the published sentence. A disagreement is not a
    // verdict on the module -- it is a reason not to name a direction at all.
    StatusFacts disagree = noSuffix;
    PublishThreads(disagree, kFixtureOwnerThread, kFixtureCallbackThread);
    disagree.csCallbackThreadIdPublished = 2001;
    CHECK(Ask1("w2f-csregister-epoch", f, disagree) == Ask::CouldNotAsk);
    CHECK(DetailMentions("two publications of one fact disagree"));
    CHECK(DetailMentions("callback=2001"));

    // ARCH-11 S5 retires WrongThread for the off-thread DISPATCH; a DRAIN off the owner thread
    // still carries the word and is still a defect. The direction is NAMED, both ids present.
    StatusFacts wrong = noSuffix;
    PublishThreads(wrong, kFixtureOwnerThread, kFixtureCallbackThread);
    strcpy_s(wrong.csStatus, "WrongThread");
    wrong.csRunning = false;
    CHECK(Ask1("w2f-csregister-epoch", f, wrong) == Ask::Fail);
    CHECK(DetailMentions("WRONG-THREAD DRAIN"));
    CHECK(DetailMentions("owner thread (our stage-0 tick's, ui_module.cpp:33) is 7717"));
    CHECK(DetailMentions("arrived on 7723"));
    CHECK(DetailMentions("different threads"));

    StatusFacts sameThread = wrong;
    PublishThreads(sameThread, kFixtureOwnerThread, kFixtureOwnerThread);
    CHECK(Ask1("w2f-csregister-epoch", f, sameThread) == Ask::Fail);
    CHECK(DetailMentions("the SAME thread"));

    // The marshal snapshot must account for itself. Short is a torn read, never a Fail.
    StatusFacts torn = noSuffix;
    PublishThreads(torn, kFixtureOwnerThread, kFixtureCallbackThread);
    torn.csInvocations = 500;
    torn.csMarshalledDraws = 100;
    torn.csCoalescedInvocations = 100;
    torn.csPendingInvocations = 0;
    CHECK(Ask1("w2f-csregister-epoch", f, torn) == Ask::CouldNotAsk);
    CHECK(DetailMentions("does not account for itself"));
    // The other direction is NOT an error: Status_() reads invocations before pending, so a worker
    // recording between the two inflates pending. Proved not to fire.
    StatusFacts skew = torn;
    skew.csPendingInvocations = 305;
    CHECK(Ask1("w2f-csregister-epoch", f, skew) == Ask::Pass);

    // The epoch clause is over spacesSeen (M2: registrations == spaceChanges can never hold).
    StatusFacts spaceRefused = noSuffix;
    PublishThreads(spaceRefused, kFixtureOwnerThread, kFixtureCallbackThread);
    spaceRefused.csSpacesSeen = 2;
    spaceRefused.csRegistrations = 1;
    CHECK(Ask1("w2f-csregister-epoch", f, spaceRefused) == Ask::Fail);
    CHECK(DetailMentions("1 registration(s) against 2 game space(s) seen"));
    // Re-registration after a stale epoch makes registrations EXCEED spacesSeen, which is the
    // design working. A clause that failed it would fail a true statement.
    StatusFacts reregistered = noSuffix;
    PublishThreads(reregistered, kFixtureOwnerThread, kFixtureCallbackThread);
    reregistered.csSpacesSeen = 1;
    reregistered.csRegistrations = 3;
    CHECK(Ask1("w2f-csregister-epoch", f, reregistered) == Ask::Pass);

    // DESIGN S8.5.4a: Running is scored from its three PARTS, not from the status word.
    StatusFacts wordOnly = noSuffix;
    PublishThreads(wordOnly, kFixtureOwnerThread, kFixtureCallbackThread);
    wordOnly.csMarshalledDraws = 0;
    wordOnly.csPendingInvocations = 400;   // recorded, never drained: the only shape with 0 draws; facts-ok: AUDIO.LISTENER_COUNT_OFF a call count, not that offset
    CHECK(Ask1("w2f-csregister-epoch", f, wordOnly) == Ask::Fail);
    CHECK(DetailMentions("marshalledDraws>0 NO"));
    // ... but zero draws with zero entries is CENSORED, never a Fail: nothing offered a frame.
    StatusFacts censored = wordOnly;
    censored.csInvocations = 0;
    censored.csPendingInvocations = 0;
    censored.csRegistrationInvocations = 0;
    CHECK(Ask1("w2f-csregister-epoch", f, censored) == Ask::CouldNotAsk);
    CHECK(DetailMentions("CENSORED"));
    // CLEAN(L5) clause 4: staleDetected == 0, and absent is not zero -- it is read from the struct.
    StatusFacts stale = noSuffix;
    PublishThreads(stale, kFixtureOwnerThread, kFixtureCallbackThread);
    stale.csStaleDetected = 2;
    CHECK(Ask1("w2f-csregister-epoch", f, stale) == Ask::Fail);
    CHECK(DetailMentions("staleDetected==0 NO"));
}

TEST_CASE("W1-L: a bare available=0 fails, an unavailable tap that names its subject passes") {
    LogFacts f;
    StatusFacts absent;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, absent) == Ask::CouldNotAsk);

    // Available() is what the tap COULD do; the registrations are what it did. This
    // row used to pass on the first alone, over a capture with 0 of 36 taps live.
    StatusFacts up;
    up.tapPresent = true;
    up.tapAvailable = true;
    up.tapRegStarted = true;
    up.tapRegistered = 37;
    up.tapTotal = 37;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, up) == Ask::Pass);
    CHECK(DetailMentions("all 37 message-bus registrations are live"));

    // THE MUST-FAIL CONTROL: available, asked, and short. A row that scores Available() alone
    // renders this Pass.
    StatusFacts short_;
    short_.tapPresent = true;
    short_.tapAvailable = true;
    short_.tapRegStarted = true;
    short_.tapRegistered = 33;
    short_.tapDeferred = 3;
    short_.tapGivenUp = 1;
    short_.tapTotal = 37;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, short_) == Ask::Fail);
    CHECK(DetailMentions("only 33 of 37 message-bus registrations are live"));
    CHECK(DetailMentions("3 still deferred, 1 given up on, 0 on a tap that answered unavailable"));

    // The fifth bucket. manifestbuild.h: an unavailable tap is built, asks nothing and is never
    // revisited, so those surfaces are terminally empty -- a reader waiting for `deferred` to drain
    // on that path waits for the process to exit. Naming them is the difference
    // between "still coming" and "never".
    StatusFacts unavail;
    unavail.tapPresent = true;
    unavail.tapAvailable = true;
    unavail.tapRegStarted = true;
    unavail.tapRegistered = 30;
    unavail.tapDeferred = 0;
    unavail.tapGivenUp = 0;
    unavail.tapUnavailable = 7;
    unavail.tapTotal = 37;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, unavail) == Ask::Fail);
    CHECK(DetailMentions("7 on a tap that answered unavailable"));

    // The five counts are published under one atomic word so that
    // `registered + deferred + givenUp + unavailable == total` holds on every sample. A snapshot
    // where it does not is torn or missing a bucket, and a shortfall computed over it is not a
    // measurement -- so it is could-not-ask, never the Fail it superficially resembles.
    StatusFacts torn = short_;
    torn.tapDeferred = 1;                 // 33 + 1 + 1 + 0 != 37
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, torn) == Ask::CouldNotAsk);
    CHECK(DetailMentions("does not account for itself"));
    CHECK(DetailMentions("there is no count here to score"));

    // L5 RUN 3, THE REGRESSION. The launch logged `36 of 38 message taps registered` and this row
    // answered could-not-ask on `services->wiretap is null`, scoring none of it. The wiretap is
    // W1-L's prologue tap; the registrations are W2-A's message bus, read out of capture.cpp in
    // this same process. A null wiretap cannot make them unknown.
    StatusFacts l5;
    l5.tapPresent = false;
    l5.tapAvailable = false;
    l5.tapRegStarted = true;
    l5.tapRegistered = 36;
    l5.tapDeferred = 0;
    l5.tapGivenUp = 0;
    l5.tapUnavailable = 1;
    // AUDIT_F3G F3G-3: 38 is `tableRows`, which is what the launch's own log line counted
    // (`36 of 38 message taps registered`). `TapRegistrationNow()` does not publish that number:
    // manifestbuild.h:77-82 has `total + excluded == tableRows` and manifestbuild.cpp:99-101
    // excludes the one ReplayIdentitySnapshot row, so total is 37 and the fixture must say so --
    // a 38 here made the all-live case below unreachable.
    l5.tapTotal = 37;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, l5) == Ask::Fail);
    CHECK(DetailMentions("only 36 of 37 message-bus registrations are live"));
    CHECK(DetailMentions("1 on a tap that answered unavailable"));
    CHECK_FALSE(DetailMentions("never offered"));

    // ...and with the registrations all live, a null wiretap is still could-not-ask: this row's
    // own subject is the prologue diagnostic, which was never offered. The registrations are said
    // out loud there rather than being silently dropped.
    StatusFacts l5ok = l5;
    l5ok.tapRegistered = 37;             // 37 of 37: reachable, unlike the 38 this case used to ask for
    l5ok.tapUnavailable = 0;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, l5ok) == Ask::CouldNotAsk);
    CHECK(DetailMentions("services->wiretap is null"));
    CHECK(DetailMentions("the capture's own registrations are all live"));

    // Zero registered is the same failure, not a different one -- but only once registration was
    // asked for at all.
    StatusFacts none = short_;
    none.tapRegistered = 0;
    none.tapDeferred = 37;
    none.tapGivenUp = 0;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, none) == Ask::Fail);
    CHECK(DetailMentions("only 0 of 37"));

    // A launch that never entered a match never built the tap. That is could-not-ask: failing it
    // would red every browser and menu launch on a fact nobody asked.
    StatusFacts notAsked = none;
    notAsked.tapRegStarted = false;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, notAsked) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never built its tap"));

    StatusFacts named;
    named.tapPresent = true;
    strcpy_s(named.tapDiagnostic,
             "prologue-unreadable: CNSNet::Dispatch at 0x14012AB30 + 16 bytes is not mapped in this "
             "image, so the build check could not be asked");
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, named) == Ask::Pass);
    CHECK(DetailMentions("names its subject"));

    StatusFacts bare;
    bare.tapPresent = true;
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, bare) == Ask::Fail);
    CHECK(DetailMentions("EMPTY diagnostic"));

    StatusFacts vague;
    vague.tapPresent = true;
    strcpy_s(vague.tapDiagnostic, "prologue-unreadable");
    CHECK(Ask1("w1l-prologue-unreadable-names-its-subject", f, vague) == Ask::Fail);
}

TEST_CASE("W2-H: the slope and the commanded line are both required, and their absences differ") {
    LogFacts noLine;
    LogFacts withLine;
    withLine.OnLine(kRateLine);
    // The row's operand is the 0.50x line, so the fixture that expects it to score has to carry
    // one. kRateLine is whatever the sink fixture needs it to be; this is the half-rate command.
    withLine.OnLine("12:00:08.000 INFO [audio] transport rate 0.50x: audio: slowed with picture");

    StatusFacts noFile;
    CHECK(Ask1("w2h-audio-slope-at-half-rate", noLine, noFile) == Ask::CouldNotAsk);
    CHECK(DetailMentions("NotRunInvalid"));

    StatusFacts loaded;
    loaded.transportPresent = true;
    loaded.durationUs = 300000000;
    CHECK(Ask1("w2h-audio-slope-at-half-rate", noLine, loaded) == Ask::CouldNotAsk);
    CHECK(DetailMentions("audioRate is null"));

    StatusFacts starved = loaded;
    starved.audioPresent = true;
    starved.audioAvailable = true;
    starved.slopeWindowMs = 800.0;
    CHECK(Ask1("w2h-audio-slope-at-half-rate", withLine, starved) == Ask::CouldNotAsk);
    CHECK(DetailMentions("NotRunStarved"));

    StatusFacts halved = starved;
    halved.slopeMeasured = true;
    halved.slope = 0.4991;
    strcpy_s(halved.audioRateLabel, "0.50x");
    CHECK(Ask1("w2h-audio-slope-at-half-rate", withLine, halved) == Ask::Pass);

    // Fail-mechanism and fail-wiring are different findings and must not read alike.
    StatusFacts unchanged = halved;
    unchanged.slope = 1.002;
    CHECK(Ask1("w2h-audio-slope-at-half-rate", withLine, unchanged) == Ask::Fail);
    CHECK(DetailMentions("fail-mechanism"));
    CHECK(Ask1("w2h-audio-slope-at-half-rate", noLine, halved) == Ask::Fail);
    CHECK(DetailMentions("fail-wiring"));
}

namespace {

// ---- the product's own words, read out of the checkout this suite was built against ----------
//
// A fixture that COPIES a product sentence cannot notice the sentence being retired: W2-H AUDIO2
// `65a3894` closed the sink's reasons into SinkVerdict and AUDIO4 `4ffac1d` DELETED the verdict
// (`RatioMoved`) whose sentence the sink fixture below used to carry, and the copy went on
// passing. So the fixture asks SinkVerdictName for the word instead, keyed on the ENUMERATOR --
// which is the thing that cannot be renamed without a compile error somewhere in the product.
std::string ReadRepoFile(const char* relPath) {
    char path[1024];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/%s", REPLAY_REPO_ROOT, relPath);
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || fp == nullptr) {
        return std::string();
    }
    std::string text;
    if (fseek(fp, 0, SEEK_END) == 0) {
        const long size = ftell(fp);
        if (size > 0 && fseek(fp, 0, SEEK_SET) == 0) {
            text.assign(static_cast<size_t>(size), '\0');
            const size_t got = fread(&text[0], 1, text.size(), fp);
            text.resize(got);
        }
    }
    fclose(fp);
    return text;
}

// The enumerators of src/audio/sinkvoice.h's SinkVerdict, in declaration order, without `Count`.
std::vector<std::string> SinkVerdictEnumerators() {
    std::string src = ReadRepoFile("src/audio/sinkvoice.h");
    std::vector<std::string> names;
    const size_t at = src.find("enum class SinkVerdict");
    if (at == std::string::npos) { return names; }
    const size_t brace = src.find('{', at);
    if (brace == std::string::npos) { return names; }
    const size_t end = src.find("};", brace);
    if (end == std::string::npos) { return names; }
    std::string body = src.substr(brace + 1, end - brace - 1);
    // The enumerators carry trailing `//` comments, and those comments contain commas.
    std::string bare;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '/' && i + 1 < body.size() && body[i + 1] == '/') {
            while (i < body.size() && body[i] != '\n') { ++i; }
        }
        if (i < body.size()) { bare.push_back(body[i]); }
    }
    size_t from = 0;
    while (from <= bare.size()) {
        const size_t comma = bare.find(',', from);
        const std::string item = bare.substr(from, (comma == std::string::npos ? bare.size() : comma) - from);
        std::string name;
        for (size_t i = 0; i < item.size(); ++i) {
            const char c = item[i];
            if (name.empty() && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) { continue; }
            if (isalnum(static_cast<unsigned char>(c)) || c == '_') { name.push_back(c); continue; }
            break;
        }
        if (!name.empty() && name != "Count") { names.push_back(name); }
        if (comma == std::string::npos) { break; }
        from = comma + 1;
    }
    return names;
}

// The sentence SinkVerdictName returns for one enumerator, empty if that enumerator has no arm.
// Adjacent literals are joined the way the compiler joins them; an escape makes this reader
// REFUSE (empty) rather than hand back a sentence it decoded wrongly.
std::string SinkVerdictSpelling(const std::string& enumerator) {
    const std::string src = ReadRepoFile("src/audio/sinkvoice.cpp");
    const size_t fn = src.find("const char* SinkVerdictName(SinkVerdict v) {");
    if (fn == std::string::npos) { return std::string(); }
    const size_t fnEnd = src.find("\n}", fn);
    if (fnEnd == std::string::npos) { return std::string(); }
    const std::string body = src.substr(fn, fnEnd - fn);
    const std::string label = "case SinkVerdict::" + enumerator + ":";
    const size_t at = body.find(label);
    if (at == std::string::npos) { return std::string(); }
    const size_t ret = body.find("return", at);
    const size_t next = body.find("case SinkVerdict::", at + label.size());
    if (ret == std::string::npos || (next != std::string::npos && ret > next)) { return std::string(); }
    const size_t semi = body.find(';', ret);
    if (semi == std::string::npos) { return std::string(); }
    std::string said;
    size_t i = ret;
    while (i < semi) {
        if (body[i] != '"') { ++i; continue; }
        size_t j = i + 1;
        while (j < semi && body[j] != '"') {
            if (body[j] == '\\') { return std::string(); }
            said.push_back(body[j]);
            ++j;
        }
        i = j + 1;
    }
    return said;
}

}  // namespace

TEST_CASE("W2-H: an unavailable sink publishes BOTH of the product's reasons, verbatim (L3-RUN-5 s8)") {
    // Three different reasons have been printed for this one failure across three launches, and
    // the row summarised them away. W2-H cannot act on a summary, so both of the product's own
    // sentences are reproduced word for word. Scoring is unchanged: could-not-ask before, and
    // could-not-ask after -- an unavailable sink is not a failed slope.
    LogFacts f;
    f.OnLine("09:49:44.220 INFO [audio] transport rate 0.50x: audio rate control unavailable");
    f.OnLine("09:49:46.891 INFO [audio] transport rate -1.00x: (empty)");
    f.OnLine("09:49:47.360 INFO [audio] transport rate 1.00x: (empty)");

    // The reason word comes from the product's CURRENT set. RatioOutOfDomain is the arm that
    // refuses this sink on what GetFrequencyRatio read back, which is what this fixture is about.
    const std::string reason = SinkVerdictSpelling("RatioOutOfDomain");
    REQUIRE(reason.size() > 20);                                  // a sentence, not a failed parse
    REQUIRE(reason.size() < sizeof(StatusFacts::audioDiagnostic));

    StatusFacts s;
    s.transportPresent = true;
    s.durationUs = 233231001;
    s.audioPresent = true;
    s.audioAvailable = false;
    strcpy_s(s.audioDiagnostic, reason.c_str());

    CHECK(Ask1("w2h-audio-slope-at-half-rate", f, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions(reason.c_str()));
    // ★ The driver's line at 0.5x, which is a DIFFERENT sentence from the sink's diagnostic and is
    // the one that dates the failure to the rate change. `rateLine` holds the LAST rate line -- the
    // 1.00x restore -- so quoting that one instead reds here.
    CHECK(DetailMentions("transport rate 0.50x: audio rate control unavailable"));
    CHECK_FALSE(DetailMentions("transport rate 1.00x"));

    // The two absences are different findings and neither may read as the other.
    StatusFacts mute = s;
    mute.audioDiagnostic[0] = '\0';
    CHECK(Ask1("w2h-audio-slope-at-half-rate", f, mute) == Ask::CouldNotAsk);
    CHECK(DetailMentions("published no diagnostic at all"));
    CHECK(DetailMentions("transport rate 0.50x: audio rate control unavailable"));

    LogFacts silent;
    silent.OnLine("09:49:47.360 INFO [audio] transport rate 1.00x: (empty)");
    CHECK(Ask1("w2h-audio-slope-at-half-rate", silent, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no `transport rate 0.50x` line was written at all"));
    CHECK(DetailMentions(reason.c_str()));
}

TEST_CASE("W1-C sc7: a refusal is NOT-RUN before any row-set comparison, never a fail") {
    LogFacts f;
    StatusFacts missing;
    CHECK(Ask1("w1c-sc7-text-diff", f, missing) == Ask::CouldNotAsk);

    StatusFacts refused;
    refused.sc7Present = true;
    refused.sc7Refused = true;
    CHECK(Ask1("w1c-sc7-text-diff", f, refused) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no verdict to score"));

    // The L5 run of 2026-09-02 scored this row asked-FAIL on `declared_sites 2 (expected 1)` beside
    // a verdict of PASS: `ed952e6` declares ledger rows 2.3 AND 2.4 under -offline and the row held
    // a constant 1. The expectation now comes from the record's own `expected_rows`, so the same
    // record reads PASS -- and the two numbers still have to agree, which is the actual claim.
    StatusFacts pass;
    pass.sc7Present = true;
    strcpy_s(pass.sc7Verdict, "PASS");
    pass.sc7DeclaredSites = 2;
    pass.sc7ExpectedRows = 2;                 // "2.3", "2.4" -- the L5 record's own array
    pass.sc7TextBytes = kSc7TextBytes;
    CHECK(Ask1("w1c-sc7-text-diff", f, pass) == Ask::Pass);
    CHECK(DetailMentions("declared_sites 2 == the 2 row(s) the build declares"));

    // Retroactive: the count the row USED to hold. A build declaring one row and mapping one site
    // is just as much a pass, which is what "derived, not pinned" has to mean in both directions.
    StatusFacts oneSite = pass;
    oneSite.sc7DeclaredSites = 1;
    oneSite.sc7ExpectedRows = 1;
    CHECK(Ask1("w1c-sc7-text-diff", f, oneSite) == Ask::Pass);

    // The three fields are load-bearing separately: a PASS over the wrong image is not a pass.
    StatusFacts wrongBytes = pass;
    wrongBytes.sc7TextBytes = 23859654;
    CHECK(Ask1("w1c-sc7-text-diff", f, wrongBytes) == Ask::Fail);
    // A site the build did NOT declare is still a finding -- the derivation must not make the
    // comparison vacuous, only stale-proof.
    StatusFacts wrongSites = pass;
    wrongSites.sc7DeclaredSites = 3;
    CHECK(Ask1("w1c-sc7-text-diff", f, wrongSites) == Ask::Fail);
    CHECK(DetailMentions("declared_sites 3 (the build declares 2)"));

    // sc7.cpp OMITS expected_rows on a refusal rather than writing an empty one, and an empty one
    // would be a denominator of zero. Neither can be scored, and neither is a pass or a fail.
    StatusFacts noDeclaration = pass;
    noDeclaration.sc7ExpectedRows = -1;
    CHECK(Ask1("w1c-sc7-text-diff", f, noDeclaration) == Ask::CouldNotAsk);
    CHECK(DetailMentions("carries no expected_rows at all"));
    StatusFacts emptyDeclaration = pass;
    emptyDeclaration.sc7ExpectedRows = 0;
    emptyDeclaration.sc7DeclaredSites = 0;
    CHECK(Ask1("w1c-sc7-text-diff", f, emptyDeclaration) == Ask::CouldNotAsk);
    CHECK(DetailMentions("EMPTY expected_rows"));
    StatusFacts failed = pass;
    strcpy_s(failed.sc7Verdict, "FAIL");
    CHECK(Ask1("w1c-sc7-text-diff", f, failed) == Ask::Fail);
}

TEST_CASE("W2-A stop sentinel: the expected name is reported whether or not it is there") {
    LogFacts closed;
    closed.OnLine(kClosed);
    StatusFacts unknown;
    CHECK(Ask1("w2a-capture-stop-sentinel", closed, unknown) == Ask::CouldNotAsk);

    StatusFacts noDir;
    noDir.stopSentinelNameKnown = true;
    strcpy_s(noDir.stopSentinelName, "capture_stop.1DB2F00.12345.json");
    CHECK(Ask1("w2a-capture-stop-sentinel", closed, noDir) == Ask::CouldNotAsk);
    CHECK(DetailMentions("capture_stop.1DB2F00.12345.json"));

    StatusFacts absent = noDir;
    strcpy_s(absent.stopSentinelDir, "C:/game/bin/win10/echoreplay");
    CHECK(Ask1("w2a-capture-stop-sentinel", closed, absent) == Ask::Fail);
    CHECK(DetailMentions("is NOT in"));

    StatusFacts present = absent;
    present.stopSentinelPresent = true;
    CHECK(Ask1("w2a-capture-stop-sentinel", closed, present) == Ask::Pass);
    CHECK(DetailMentions("4211 records"));
}

// W1-J. The window and the attendance are the two facts that used to want an environment name the
// harness's allow-list would refuse. Each is driven three ways here, because the absence is the
// outcome the ruling is about: could-not-ask named `no-launch-marker`, never a default.
StatusFacts MarkerRead(const char* windowJson, const char* attendedJson) {
    StatusFacts s;
    s.markerPresent = true;
    strcpy_s(s.markerSource, "C:/echo/bin/win10/echoreplay/launch-marker.json");
    if (windowJson != nullptr) {
        s.markerWindowKnown = true;
        s.markerWindowSec = atof(windowJson);
    } else {
        strcpy_s(s.markerWindowReason, "the marker declares no windowSec");
    }
    if (attendedJson != nullptr) {
        s.markerAttendedKnown = true;
        s.markerAttended = strcmp(attendedJson, "true") == 0;
    } else {
        strcpy_s(s.markerAttendedReason, "the marker declares no attended flag");
    }
    return s;
}

TEST_CASE("the launch marker's text is parsed, and an unreadable field is never a default") {
    StatusFacts good;
    ParseLaunchMarker("{\n  \"windowSec\": 180,\n  \"attended\": false\n}\n",
                      "C:/x/echoreplay/launch-marker.json", good);
    CHECK(good.markerPresent);
    CHECK(good.markerWindowKnown);
    CHECK(good.markerWindowSec == 180.0);
    CHECK(good.markerAttendedKnown);
    CHECK_FALSE(good.markerAttended);

    // L5's own marker, byte for byte what Write-LaunchMarker emits for the descriptor now that it
    // declares windowSec 180 and attended false. The window row goes asked-PASS, which is the one
    // thing that lets the BROWSER arm's record leave `provisional`; the attendance row is the
    // cross-check and it FAILS here, which is correct -- an -idle probe id under attended=false is
    // the contradiction that row exists to report.
    LogFacts noLines;
    CHECK(Ask1("w1j-launch-window-declared", noLines, good) == Ask::Pass);
    CHECK(DetailMentions("180"));
    CHECK(Ask1("w1j-launch-attendance-declared", noLines, good) == Ask::Fail);
    CHECK(DetailMentions("declares attended=false"));

    StatusFacts silent;
    ParseLaunchMarker("{}", "C:/x/launch-marker.json", silent);
    CHECK(silent.markerPresent);
    CHECK_FALSE(silent.markerWindowKnown);
    CHECK(strstr(silent.markerWindowReason, "declares no windowSec") != nullptr);
    CHECK_FALSE(silent.markerAttendedKnown);
    CHECK(strstr(silent.markerAttendedReason, "declares no attended flag") != nullptr);

    // A quoted number is not a number and `yes` is not a bool. Either one is a NAMED refusal, not a
    // zero window and not an unattended launch.
    StatusFacts bad;
    ParseLaunchMarker("{\"windowSec\": \"180\", \"attended\": \"yes\"}", "C:/x/launch-marker.json", bad);
    CHECK_FALSE(bad.markerWindowKnown);
    CHECK(bad.markerWindowSec == 0.0);
    CHECK(strstr(bad.markerWindowReason, "not a number") != nullptr);
    CHECK_FALSE(bad.markerAttendedKnown);
    CHECK(strstr(bad.markerAttendedReason, "neither true nor false") != nullptr);

    // The key is punctuated on both sides, so a longer key ending in the same word is not it.
    StatusFacts decoy;
    ParseLaunchMarker("{\"notwindowSec\": 9, \"attended\": true}", "C:/x/launch-marker.json", decoy);
    CHECK_FALSE(decoy.markerWindowKnown);
    CHECK(decoy.markerAttendedKnown);
    CHECK(decoy.markerAttended);

    StatusFacts none;
    LaunchMarkerAbsent("launch-marker.json is in neither 'C:/echo' nor 'C:/echo/bin'", none);
    CHECK_FALSE(none.markerPresent);
    CHECK(strstr(none.markerWindowReason, "is in neither") != nullptr);
}

TEST_CASE("the browser window is could-not-ask without a marker, and never a default") {
    LogFacts f;
    StatusFacts none;
    strcpy_s(none.markerWindowReason, "launch-marker.json is in neither 'C:/echo' nor 'C:/echo/bin'");
    CHECK(Ask1("w1j-launch-window-declared", f, none) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no-launch-marker"));
    CHECK(DetailMentions("is in neither"));

    // Present but silent on the field is the same answer, with its own reason: a marker is not a
    // window.
    CHECK(Ask1("w1j-launch-window-declared", f, MarkerRead(nullptr, "true")) == Ask::CouldNotAsk);
    CHECK(DetailMentions("declares no windowSec"));

    CHECK(Ask1("w1j-launch-window-declared", f, MarkerRead("180", "false")) == Ask::Pass);
    CHECK(DetailMentions("180"));

    // A declared zero is a FAIL, not a could-not-ask: somebody answered, and the answer would send
    // the record final before the arm observed anything.
    CHECK(Ask1("w1j-launch-window-declared", f, MarkerRead("0", "false")) == Ask::Fail);
    CHECK(DetailMentions("not a window"));
}

TEST_CASE("the idle arm's attendance is cross-checked against the probe id that selected it") {
    LogFacts f;
    StatusFacts none;
    CHECK(Ask1("w1j-launch-attendance-declared", f, none) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no-launch-marker"));

    CHECK(Ask1("w1j-launch-attendance-declared", f, MarkerRead("180", "true")) == Ask::Pass);
    CHECK(DetailMentions("attended=true"));

    // The id selected the attended arm and the harness says nobody is watching. Two sources, one
    // fact, and they disagree -- reported, not resolved.
    CHECK(Ask1("w1j-launch-attendance-declared", f, MarkerRead("180", "false")) == Ask::Fail);
    CHECK(DetailMentions("attended=false"));
    CHECK(DetailMentions("NOT-RUN"));
}

TEST_CASE("every row in the table evaluates, in every arm that lists it") {
    // never-asked is not representable in an arm's own battery: a listed row always answers one of
    // the three. The remaining way a listed row could go unanswered is the record omitting it, and
    // tests/replay/check.ps1 scores that against the DESCRIPTOR's row list.
    size_t count = 0;
    const RowSpec* rows = Rows(&count);
    CHECK(count > 0);
    LogFacts f;
    StatusFacts s;
    const unsigned arms[] = {kArmCapture, kArmPlayback, kArmBrowser, kArmIdle};
    for (unsigned arm : arms) {
        int listed = 0;
        for (size_t i = 0; i < count; ++i) {
            if ((rows[i].arms & arm) == 0) {
                continue;
            }
            ++listed;
            Ask out = Ask::Pass;
            char detail[600] = {0};
            REQUIRE(EvaluateRow(rows[i].id, f, s, &out, detail, sizeof detail));
            CHECK(out == Ask::CouldNotAsk);        // nothing observed yet, and nothing claimed
            CHECK(strlen(detail) > 0);             // a could-not-ask with no reason is a shrug
        }
        CHECK(listed > 0);
    }
}

TEST_CASE("every row id is unique, so a record cannot answer one row twice") {
    size_t count = 0;
    const RowSpec* rows = Rows(&count);
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            CHECK(strcmp(rows[i].id, rows[j].id) != 0);
        }
    }
}

TEST_CASE("the BROWSER arm is could-not-ask until a level loads, and then it scores the open") {
    StatusFacts menu;
    LogFacts f;
    CHECK(Ask1("w2r-browser-opened-over-level", f, menu) == Ask::CouldNotAsk);
    CHECK(DetailMentions("level-not-reached"));
    CHECK(DetailMentions("no [session] 'NetGame state' line"));

    // A session that logged states and never reached one of the level states says so with the
    // state it did reach -- not with a sentence about the entity module.
    LogFacts lobby;
    lobby.OnLine(kStateLobby);
    CHECK(Ask1("w2r-browser-opened-over-level", lobby, menu) == Ask::CouldNotAsk);
    CHECK(DetailMentions("level-not-reached"));
    CHECK(DetailMentions("never 7, 8 or 9"));
    CHECK_FALSE(DetailMentions("bound game space"));

    // THE L5 DEFECT (ARCH-10 4.1 row 4). The level loaded and the entity bind never happened. The
    // row must not call that "level not reached", and it must name the bind as what is missing.
    LogFacts levelNoBind;
    levelNoBind.OnLine(kStateLobby);
    levelNoBind.OnLine(kStateLoadingLevel);
    levelNoBind.OnLine(kStateInGame);
    CHECK(LevelReached(levelNoBind));
    CHECK(Ask1("w2r-browser-opened-over-level", levelNoBind, menu) == Ask::CouldNotAsk);
    CHECK_FALSE(DetailMentions("level-not-reached"));
    CHECK(DetailMentions("bind not observed"));
    CHECK(DetailMentions("ui::OpenBrowser was never called"));

    LogFacts f2;
    f2.OnLine(kBootAttached);
    f2.OnLine(kStateInGame);
    f2.OnLine(kBound);
    StatusFacts loaded = menu;
    strcpy_s(loaded.browserReason, "a game space bound and the tick has not yet opened the browser");
    CHECK(Ask1("w2r-browser-opened-over-level", f2, loaded) == Ask::CouldNotAsk);
    CHECK(DetailMentions("ui::OpenBrowser was never called"));
    CHECK_FALSE(DetailMentions("bind not observed"));

    f = f2;
    f.OnLine(kSurvive);      // the anchor that puts the two stamps on one clock
    // ...and the heartbeat that is still beating when the level line is written. Same epoch: a
    // launch's observed lifetime is its LAST heartbeat, and an interval past that one is refused
    // as out of window.
    f.OnLine("04:16:43.073 INFO [observer] SURVIVE 15000 ms, heartbeat 16, arm BROWSER");
    StatusFacts refused = loaded;
    refused.browserOpenRequested = true;
    refused.browserOpenedAtKnown = true;
    refused.browserOpenedAtMs = 51000.0;
    strcpy_s(refused.browserReason, "ui::BrowserOpen()=0, ui::PanelEnabled()=0");
    CHECK(Ask1("w2r-browser-opened-over-level", f, refused) == Ask::Fail);
    CHECK(DetailMentions("still"));

    StatusFacts open = refused;
    open.browserOpen = true;
    strcpy_s(open.browserReason, "ui::OpenBrowser() called on the tick at 51000 ms");
    CHECK(Ask1("w2r-browser-opened-over-level", f, open) == Ask::Pass);
    CHECK(DetailMentions("IN ORDER"));
}

// ============================================================================================
// B2 / the design notes The order assertion, and the units control the conversion needs.
// ============================================================================================
TEST_CASE("w2r-browser-opened-over-level is an ORDER assertion on ONE clock") {
    // The RECORDED L5 re-run 3 bytes. The level line and the heartbeat are the log's; the two
    // status numbers are the recorded done.json's own detail ("called on the tick at 5774 ms",
    // "BrowserOpen()=1").
    LogFacts l5;
    l5.OnLine(kBootAttached);
    l5.OnLine(kL5LoadingLevel);
    l5.OnLine(kL5Survive);
    REQUIRE(l5.levelStampKnown);
    REQUIRE(l5.armEpochKnown);
    double levelMs = 0.0;
    REQUIRE(LevelLoadMonotonicMs(l5, &levelMs));
    CHECK(levelMs == doctest::Approx(19340.0));

    StatusFacts asRecorded;
    asRecorded.browserOpenRequested = true;
    asRecorded.browserOpen = true;
    asRecorded.browserOpenedAtKnown = true;
    asRecorded.browserOpenedAtMs = 5774.0;
    strcpy_s(asRecorded.browserReason, "replay::ui::OpenBrowser() called on the tick at 5774 ms");
    // The row that filed asked-pass on these exact bytes now files asked-fail on them.
    CHECK(Ask1("w2r-browser-opened-over-level", l5, asRecorded) == Ask::Fail);
    CHECK(DetailMentions("OUT OF ORDER"));
    CHECK(DetailMentions("5774 ms"));
    CHECK(DetailMentions("19340 ms"));
    CHECK(DetailMentions("7 (loading level)"));

    // THE UNITS CONTROL. A reader that compared the level's WALL stamp (46626393 ms since
    // midnight) with the probe's launch-relative one would answer asked-fail for every launch that
    // ever existed, including this one -- so the recorded bytes alone cannot catch that bug. This
    // pair can: the same log, a browser opened 5.7 s AFTER the level, which is a pass only if the
    // conversion happened.
    StatusFacts afterTheLevel = asRecorded;
    afterTheLevel.browserOpenedAtMs = 25000.0;
    CHECK(Ask1("w2r-browser-opened-over-level", l5, afterTheLevel) == Ask::Pass);
    CHECK(DetailMentions("IN ORDER"));
    CHECK(DetailMentions("5660 ms later"));

    // A stamp is MISSING -> could-not-ask NAMING which one, never a comparison.
    LogFacts noAnchor;
    noAnchor.OnLine(kL5LoadingLevel);
    CHECK(Ask1("w2r-browser-opened-over-level", noAnchor, afterTheLevel) == Ask::CouldNotAsk);
    CHECK(DetailMentions("SURVIVE clock anchor MISSING"));
    CHECK(DetailMentions("level-load line stamp present"));

    StatusFacts noOpenStamp = afterTheLevel;
    noOpenStamp.browserOpenedAtKnown = false;
    CHECK(Ask1("w2r-browser-opened-over-level", l5, noOpenStamp) == Ask::CouldNotAsk);
    CHECK(DetailMentions("browserOpenedAtMs MISSING"));

    // L5 RE-RUN 2, RETROACTIVE ON ITS OWN BYTES. AUDIT_F3G F3G-1: this case used to claim run 2
    // had neither a SURVIVE line nor a level line and fed a default-constructed LogFacts. Both
    // halves are false -- that log carries 239 `arm BROWSER` heartbeats and reaches state 7 -- so
    // it scored an empty-facts case under run 2's name. Fed the real lines, the level IS reached
    // (19305 ms into the window) and the row lands on the OTHER refusal: the launch never called
    // OpenBrowser at all (`grep -c OpenBrowser` over that log is 0, as is `bound game space`).
    // A reader that still answered `level-not-reached` here would be reporting the old, wrong
    // cause with the right verdict word.
    LogFacts run2;
    run2.OnLine(kBootAttached);
    run2.OnLine(kRun2LoadingLevel);
    // BOTH ends of run 2's 239-heartbeat window, in the order the log carries them. The anchor is
    // the LATEST sample, so the epoch is 15618698 - 239930 and not 15378792 - 30: a reader that
    // kept the first heartbeat renders 19311 ms here, six milliseconds out, and this REQUIRE is
    // the only place in the suite where those two readings differ.
    run2.OnLine(kRun2FirstSurvive);
    run2.OnLine(kRun2LastSurvive);
    REQUIRE(run2.levelStampKnown);
    REQUIRE(run2.armEpochKnown);
    double run2LevelMs = 0.0;
    REQUIRE(LevelLoadMonotonicMs(run2, &run2LevelMs));
    CHECK(run2LevelMs == doctest::Approx(19305.0));

    StatusFacts run2Status;              // no ui::OpenBrowser line in that log, so nothing asked
    strcpy_s(run2Status.browserReason, "ui::BrowserOpen()=0, ui::PanelEnabled()=0");
    CHECK(Ask1("w2r-browser-opened-over-level", run2, run2Status) == Ask::CouldNotAsk);
    CHECK(DetailMentions("ui::OpenBrowser was never called"));
    CHECK_FALSE(DetailMentions("level-not-reached"));

    // ...and the empty-facts case keeps its own name. A log with no session line at all is
    // level-not-reached, which is a different refusal from run 2's.
    LogFacts nothing;
    CHECK(Ask1("w2r-browser-opened-over-level", nothing, afterTheLevel) == Ask::CouldNotAsk);
    CHECK(DetailMentions("level-not-reached"));

    // A SURVIVE-shaped line that is not the heartbeat grammar must not set the epoch.
    LogFacts impostor;
    impostor.OnLine("12:00:00.000 INFO [observer] SURVIVE 999 sessions were counted");
    CHECK_FALSE(impostor.armEpochKnown);
    // ... and neither must somebody else's tag.
    LogFacts wrongTag;
    wrongTag.OnLine("12:00:00.000 INFO [session] SURVIVE 5000 ms, heartbeat 6, arm BROWSER");
    CHECK_FALSE(wrongTag.armEpochKnown);

    // The stamp reader itself: shape, not sscanf's first number.
    double ms = 0.0;
    CHECK(ParseLogClockMs("12:57:06.393 INFO  [x] y", &ms));
    CHECK(ms == doctest::Approx(46626393.0));
    CHECK_FALSE(ParseLogClockMs("2:57:06.393 INFO [x] y", &ms));
    CHECK_FALSE(ParseLogClockMs("12:57:06,393 INFO [x] y", &ms));
    CHECK_FALSE(ParseLogClockMs("24:00:00.000 INFO [x] y", &ms));
    CHECK_FALSE(ParseLogClockMs("12:60:00.000 INFO [x] y", &ms));
}

// ============================================================================================
// The panel is never DRAWN (W2-F P2B: panel::Draw has one non-test caller, the band-3 drain).
// ============================================================================================
// ============================================================================================
// The probe reads replay::LogPath(), which is CUMULATIVE across launches.
// ============================================================================================
TEST_CASE("F10: a level stamp from another launch is discarded, never corrected") {
    // RETROACTIVE, against the recorded L5 #4 input. The live log held 42355 lines from NINE
    // launches; the first state-7 line in it (line 41019) was L5 run #1's, from the previous day.
    LogFacts f;
    f.OnLine(kStaleLevelL5Run1);
    f.OnLine(kBootAttached);
    f.OnLine(kThisLevelL5Run4);
    f.OnLine(kSurviveL5Run4);
    CHECK(f.launchAnchorSeen);
    CHECK(f.launchesInLog == 1u);
    CHECK(f.linesBeforeLaunch == 1u);          // the stale level line, dropped whole
    CHECK(f.levelWallMs == doctest::Approx(60979079.0));   // 16:56:19.079, not 04:16:38.073
    double levelMs = 0.0;
    char why[400] = {0}; // facts-ok: AUDIO.LISTENER_COUNT_OFF a text-buffer length, not that offset
    REQUIRE(LevelLoadMonotonicMs(f, &levelMs, why, sizeof why));
    CHECK(levelMs == doctest::Approx(21082.0));            // the TRUE interval, F10's arithmetic
    CHECK(why[0] == '\0');

    // THE MUTANT THIS TEST EXISTS FOR, stated as a value rather than as a code edit: the number
    // the shipped row printed was 40840076 ms, and it is unreachable now from any input.
    CHECK(levelMs < 240000.0);

    // ...and the completion predicate, which is what actually went NOT-RUN: survival 240886 ms
    // minus the level stamp must be a number an arm holding for 5000 ms can reach.
    CHECK(240886.0 - levelMs > 5000.0);

    // The order verdict L5 #4 reached is UNCHANGED and was always true: OpenBrowser at 6980 ms,
    // the level at 21082 ms. The row failed for the right reason with a garbage number in it.
    StatusFacts s;
    s.browserOpenRequested = true;
    s.browserOpen = true;
    s.browserOpenedAtKnown = true;
    s.browserOpenedAtMs = 6980.0;
    strcpy_s(s.browserReason, "replay::ui::OpenBrowser() called on the tick at 6980 ms");
    CHECK(Ask1("w2r-browser-opened-over-level", f, s) == Ask::Fail);
    CHECK(DetailMentions("OUT OF ORDER"));
    CHECK(DetailMentions("21082 ms"));
    CHECK_FALSE(DetailMentions("40840076"));
}

TEST_CASE("F10: the midnight guard is struck -- an out-of-window interval is refused by name") {
    // The exact anomaly, reached directly: a stamp 12.6 hours BEFORE the launch epoch. The struck
    // guard added 86400000 to it and produced 40840076 ms; there is no correction term left.
    LogFacts stale;
    stale.launchAnchorSeen = true;
    stale.levelStampKnown = true;
    stale.levelWallMs = 15398073.0;      // 04:16:38.073, the previous day's launch
    stale.armEpochKnown = true;
    stale.armEpochWallMs = 60957997.0;   // 16:55:57.997
    stale.armEpochMonoMs = 239879.0;
    strcpy_s(stale.levelStampLine, "NetGame state 5 (lobby) -> 7 (loading level)");
    double ms = -1.0;
    char why[400] = {0}; // facts-ok: AUDIO.LISTENER_COUNT_OFF a text-buffer length, not that offset
    CHECK_FALSE(LevelLoadMonotonicMs(stale, &ms, why, sizeof why));
    CHECK(ms == doctest::Approx(-1.0));   // untouched: a refusal writes no number
    CHECK(std::string(why).find("OUTSIDE this launch's own observed lifetime") != std::string::npos);
    CHECK(std::string(why).find("-45559924") != std::string::npos);

    // Forward, past the end of the launch, is the same refusal: the guard was never symmetric and
    // an interval longer than the launch is as impossible as one before it.
    LogFacts ahead = stale;
    ahead.levelWallMs = 60957997.0 + 400000.0;
    CHECK_FALSE(LevelLoadMonotonicMs(ahead, &ms, why, sizeof why));
    CHECK(std::string(why).find("OUTSIDE") != std::string::npos);

    // The slack, and its edge: a level line written between the last heartbeat and the read is
    // real; 5001 ms past it is not.
    LogFacts justInside = stale;
    justInside.levelWallMs = 60957997.0 + 239879.0 + 5000.0;
    CHECK(LevelLoadMonotonicMs(justInside, &ms, why, sizeof why));
    LogFacts justOutside = stale;
    justOutside.levelWallMs = 60957997.0 + 239879.0 + 5001.0;
    CHECK_FALSE(LevelLoadMonotonicMs(justOutside, &ms, why, sizeof why));

    // And no anchor at all is a THIRD refusal, with its own sentence: without `[boot] attached`
    // there is no way to tell whose lines these are.
    LogFacts unanchored = stale;
    unanchored.launchAnchorSeen = false;
    unanchored.levelWallMs = 60979079.0;
    CHECK_FALSE(LevelLoadMonotonicMs(unanchored, &ms, why, sizeof why));
    CHECK(std::string(why).find("[boot] attached") != std::string::npos);
}

TEST_CASE("F10: a second launch in the same file resets every fact the first one left") {
    LogFacts f;
    f.OnLine(kBootAttached);
    f.OnLine(kStaleLevelL5Run1);
    f.OnLine("04:20:00.000 INFO  [observer] SURVIVE 202000 ms, heartbeat 202, arm BROWSER");
    REQUIRE(f.levelStampKnown);
    f.OnLine("16:55:57.501 INFO  [boot] attached");
    CHECK(f.launchesInLog == 2u);
    CHECK_FALSE(f.levelStampKnown);
    CHECK_FALSE(f.armEpochKnown);
    CHECK_FALSE(f.sessionLevelStateSeen);
    CHECK(f.linesBeforeLaunch == 3u);
    CHECK(f.linesThisLaunch == 1u);
}

// ============================================================================================
// ARCH-15 S4.1-S4.5. The coverage row's THIRD numerator, and why the first two were struck.
// ============================================================================================
// RETROACTIVE against the two recorded numbers: L5 #7 run 2 measured
// drawNanosMax=629300 and L5 #8 run 2 measured 315700, both over the 200000 ns ceiling and both
// found by a person reading a report. Either record must RED this row.
TEST_CASE("w2f-draw-budget reds the recorded over-budget maxima and passes only under the ceiling") {
    LogFacts f;
    f.OnLine(kBootAttached);
    f.OnLine(kStateInGame);

    StatusFacts noModule;
    CHECK(Ask1("w2f-draw-budget", f, noModule) == Ask::CouldNotAsk);
    CHECK(DetailMentions("absent is not a draw that came in under budget"));

    // A record from before the ceiling travelled with the measurement. It carries a number and
    // no denominator, and a reader that supplied its own would be certifying the record against
    // a ceiling that build never had.
    StatusFacts noCeiling;
    noCeiling.csAsked = true;
    noCeiling.csInFrameKnown = true;
    noCeiling.csInFrameDraws = 5175;
    noCeiling.csDrawNanosMax = 315700;
    CHECK(Ask1("w2f-draw-budget", f, noCeiling) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never named the ceiling"));

    // L5 #8 run 2, verbatim.
    StatusFacts l5run8;
    l5run8.csAsked = true;
    l5run8.csInFrameKnown = true;
    l5run8.csInFrameDraws = 4727;
    l5run8.csDrawNanosTotal = 342760400;
    l5run8.csDrawNanosMax = 315700;
    l5run8.csFirstDrawNanos = 0;
    // inframe::kDrawNanosBudgetNs as the record carries it. This value is the FIXTURE s, not the
    // authority: the row scores every case below against the ceiling on the record in front of
    // it, so a product that moves the constant moves every live verdict with it.
    l5run8.csDrawNanosBudget = 200000;
    CHECK(Ask1("w2f-draw-budget", f, l5run8) == Ask::Fail);
    CHECK(DetailMentions("OVER BUDGET"));
    CHECK(DetailMentions("315700"));

    // L5 #7 run 2, the other recorded maximum.
    StatusFacts l5run7 = l5run8;
    l5run7.csDrawNanosMax = 629300;
    CHECK(Ask1("w2f-draw-budget", f, l5run7) == Ask::Fail);

    // The boundary, both sides. Equal to the ceiling is inside it.
    StatusFacts at = l5run8;
    at.csDrawPhasesKnown = true;
    at.csDrawNanosMax = at.csDrawNanosBudget;
    CHECK(Ask1("w2f-draw-budget", f, at) == Ask::Pass);
    StatusFacts over = l5run8;
    over.csDrawNanosMax = over.csDrawNanosBudget + 1;
    CHECK(Ask1("w2f-draw-budget", f, over) == Ask::Fail);

    // The excluded number is REPORTED on the pass, or the exclusion is invisible to the reader.
    StatusFacts under = l5run8;
    under.csDrawPhasesKnown = true;
    under.csImguiNanos = 37500;
    under.csDrawNanosMax = 37700;
    under.csFirstDrawNanos = 629300;
    CHECK(Ask1("w2f-draw-budget", f, under) == Ask::Pass);
    CHECK(DetailMentions("firstDrawNanos=629300"));

    // One draw is the FIRST draw, which the maximum excludes: there is no steady state to score.
    StatusFacts once = l5run8;
    once.csInFrameDraws = 1;
    once.csDrawNanosMax = 0;
    CHECK(Ask1("w2f-draw-budget", f, once) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no steady-state maximum yet"));

    StatusFacts nodraw = l5run8;
    nodraw.csInFrameDraws = 0;
    CHECK(Ask1("w2f-draw-budget", f, nodraw) == Ask::CouldNotAsk);
    CHECK(DetailMentions("not a measurement of anything"));
}

TEST_CASE("w2r-panel-drawn-over-level scores inFrameDraws and nothing else") {
    LogFacts f;
    f.OnLine(kBootAttached);
    f.OnLine(kStateInGame);

    StatusFacts noModule;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, noModule) == Ask::CouldNotAsk);
    CHECK(DetailMentions("absent -- which is not the same answer as a draw counter reading zero"));

    // RETROACTIVE, against the recorded L5 re-run 3 record: it carries no row-2.5 field at all, so
    // DL-07 is NOT EVALUABLE on it and the row must say the prerequisite by name. The two counters
    // it DOES carry are the ones ARCH-15 S4.1 struck.
    StatusFacts l5run3;
    l5run3.csAsked = true;
    strcpy_s(l5run3.csStatus, "WrongThread");
    l5run3.csInvocations = 4681;
    l5run3.csMarshalledDraws = 0;
    l5run3.csOwnerTicksKnown = true;
    l5run3.csOwnerTicks = 4681;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, l5run3) == Ask::CouldNotAsk);
    CHECK(DetailMentions("NOT EVALUABLE"));
    CHECK(DetailMentions("ARCH-15-C1"));
    CHECK_FALSE(DetailMentions("NOTHING DREW"));

    // THE F3J-1 CONTROL, verbatim: W2-F's own test case 1 -- registration never lands, so the
    // panel draws on every one of 4000 owner ticks and marshalledDraws stays 0. The row that
    // scored marshalledDraws filed this as its most emphatic FAIL. It must never PASS either:
    // ARCH-15 S1.4 shows every one of those 4000 draws was refused at NRadGui::Begin.
    StatusFacts case1;
    case1.csAsked = true;
    strcpy_s(case1.csStatus, "Running");
    case1.browserOpen = true;
    case1.panelEnabled = true;
    case1.panelVisible = true;
    case1.csOwnerTicksKnown = true;
    case1.csOwnerTicks = 4000;
    case1.csMarshalledDraws = 0;
    case1.csEnableStampsKnown = true;
    case1.csEnableStamps = 1;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, case1) == Ask::CouldNotAsk);
    CHECK(DetailMentions("NOT EVALUABLE"));

    // ...and with the row-2.5 counters present and every draw refused -- which is what that same
    // launch looks like once W2-F-C1 publishes them -- it is an asked-FAIL naming the count.
    StatusFacts case1WithSite = case1;
    case1WithSite.csInFrameKnown = true;
    case1WithSite.csInFrameInstalled = true;
    case1WithSite.csInFrameBaselineKnown = true;
    strcpy_s(case1WithSite.csInFrameBaselineStamp, "enable");
    case1WithSite.csFrameSiteEntries = 4000;
    case1WithSite.csInFrameDraws = 0;
    case1WithSite.csRefusedDraws = 4000;
    case1WithSite.csRefusedNoFrameByte = 4000;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, case1WithSite) == Ask::Fail);
    CHECK(DetailMentions("REFUSED AT THE SITE ON 4000 OF 4000"));
    CHECK(DetailMentions("noFrameByte=4000"));

    // THE PASS: in-frame draws on every entry.
    StatusFacts drew = case1WithSite;
    drew.csInFrameDraws = 4000;
    drew.csRefusedDraws = 0;
    drew.csRefusedNoFrameByte = 0;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, drew) == Ask::Pass);
    CHECK(DetailMentions("4000/4000 inFrameDraws/frameSiteEntries"));
    // ...and it passes with every piece of UI state FALSE, because it reads none of them.
    StatusFacts drewNoState = drew;
    drewNoState.browserOpen = false;
    drewNoState.panelEnabled = false;
    drewNoState.panelVisible = false;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, drewNoState) == Ask::Pass);

    // ONE tick of slack, and no more: 3999 of 4000 passes, 3998 does not.
    StatusFacts oneShort = drew;
    oneShort.csInFrameDraws = 3999;
    oneShort.csPanelClosedEntries = 1;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, oneShort) == Ask::Pass);
    StatusFacts twoShort = drew;
    twoShort.csInFrameDraws = 3998;
    twoShort.csPanelClosedEntries = 2;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, twoShort) == Ask::Fail);
    CHECK(DetailMentions("ONLY 3998 OF 4000"));
    CHECK(DetailMentions("2 were entries at which the panel was CLOSED"));

    // A SINGLE refusal is a fail even though the coverage margin would have absorbed it: a refused
    // draw is never a draw, and the slack is for a straddled sample.
    StatusFacts oneRefused = drew;
    oneRefused.csInFrameDraws = 3999;
    oneRefused.csRefusedDraws = 1;
    oneRefused.csRefusedNoLatch = 1;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, oneRefused) == Ask::Fail);
    CHECK(DetailMentions("REFUSED AT THE SITE ON 1 OF 4000"));

    // The counters are a DIFFERENCE across the stamp: draws before the window do not answer it.
    StatusFacts beforeOnly = drew;
    beforeOnly.csFrameSiteEntriesAtBaseline = 4000;
    beforeOnly.csInFrameDrawsAtBaseline = 4000;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, beforeOnly) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never ENTERED after the enable stamp"));
    CHECK(DetailMentions("the failure is the INSTALL"));

    // F3J-2: the window is the LATER of the two stamps, and a panel that never opened has none.
    StatusFacts neverOpened = drew;
    neverOpened.csEnableStamps = 0;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, neverOpened) == Ask::CouldNotAsk);
    CHECK(DetailMentions("enableStamps == 0"));

    StatusFacts noBaseline = drew;
    noBaseline.csInFrameBaselineKnown = false;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, noBaseline) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no baseline to be a difference from"));

    StatusFacts wentBack = drew;
    wentBack.csInFrameDrawsAtBaseline = 9000;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, wentBack) == Ask::CouldNotAsk);
    CHECK(DetailMentions("BACKWARDS"));

    // ARCH-15 S4.3's published invariant: parts that do not sum to their entries are an
    // instrument fault, and a fault is not a verdict in either direction.
    StatusFacts faulty = drew;
    faulty.csInFrameDraws = 4001;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, faulty) == Ask::CouldNotAsk);
    CHECK(DetailMentions("INSTRUMENT FAULT"));

    StatusFacts noTicks = drew;
    noTicks.csOwnerTicks = 0;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, noTicks) == Ask::CouldNotAsk);
    CHECK(DetailMentions("CENSORED (ARCH-15 S4.5)"));

    LogFacts noLevel;
    noLevel.OnLine(kBootAttached);
    CHECK(Ask1("w2r-panel-drawn-over-level", noLevel, drew) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no level for a draw to have landed over"));
}

// ============================================================================================
// ============================================================================================
// ARCH-15 S4.2. The published `[inframe ...]` group, parsed as a CROSS-CHECK of the Report read.
// The line carries `draws=` TWICE meaning two different counters, so every case below keeps the
// two apart on purpose: a parser that took the first `draws=` it found would report ownerDraws.
// ============================================================================================
TEST_CASE("the [inframe ...] group is parsed by group, never by the bare word draws=") {
    // The grammar exactly as src/ui/csregister.h freezes it: ticks, then inframe, then threads.
    // ticks draws=7 is ownerDraws; inframe draws=4000 is inFrameDraws. They are never equal here.
    const char* kLine =
        "csregister: Running invocations=4681 registrations=1 [ticks owner=4000 draws=7] "
        "[inframe entries=4000 draws=4000 refused=3 nobyte=1 nolatch=1 noctx=1 facade=0 "
        "closed=11 fallback=0 installed=1] [threads owner=1234 callback=0]";

    InFrameLine line;
    REQUIRE(ParseInFrameLine(kLine, line));
    CHECK(line.entries == 4000u);
    CHECK(line.draws == 4000u);   // NOT 7: the ticks group's draws= belongs to the fallback
    CHECK(line.refused == 3u);
    CHECK(line.noByte == 1u);
    CHECK(line.noLatch == 1u);
    CHECK(line.noCtx == 1u);
    CHECK(line.facade == 0u);
    CHECK(line.closed == 11u);
    CHECK(line.fallback == 0u);
    CHECK(line.installed);

    // THE NEAR MISS. Same line with the two counters swapped in value: a reader that keyed on the
    // bare word would score 7 here and 4000 above and pass both times. Keyed on the group, it
    // reads 7 both times it is the inframe group's field and never otherwise.
    const char* kSwapped =
        "csregister: Running [ticks owner=4000 draws=4000] "
        "[inframe entries=4000 draws=7 refused=3993 nobyte=3993 nolatch=0 noctx=0 facade=0 "
        "closed=0 fallback=0 installed=1] [threads owner=1234 callback=0]";
    InFrameLine swapped;
    REQUIRE(ParseInFrameLine(kSwapped, swapped));
    CHECK(swapped.draws == 7u);
    CHECK(swapped.refused == 3993u);

    // A line from before the group existed carries no reading at all -- which is not a zero.
    InFrameLine none;
    CHECK_FALSE(ParseInFrameLine(
        "csregister: Running [ticks owner=4000 draws=7] [threads owner=1234 callback=0]", none));
    CHECK(none.entries == 0u);
    CHECK_FALSE(ParseInFrameLine(nullptr, none));

    // A group missing a field is REFUSED WHOLE. Half a group is not a reading, and the fields that
    // did parse would otherwise be scored beside a fabricated 0 for the ones that did not.
    CHECK_FALSE(ParseInFrameLine(
        "x [inframe entries=4000 draws=4000 refused=0 nobyte=0 nolatch=0 noctx=0 facade=0 "
        "closed=0 fallback=0] [threads owner=1 callback=0]", none));
    // ...and so is an `installed=` outside the two values the grammar defines for it.
    CHECK_FALSE(ParseInFrameLine(
        "x [inframe entries=1 draws=1 refused=0 nobyte=0 nolatch=0 noctx=0 facade=0 closed=0 "
        "fallback=0 installed=2]", none));

    // The LAST group wins: a sentence that quoted a group must not answer for the trailer, which
    // is the same rule the threads parse takes.
    InFrameLine last;
    REQUIRE(ParseInFrameLine(
        "refusing: was [inframe entries=1 draws=1 refused=0 nobyte=0 nolatch=0 noctx=0 facade=0 "
        "closed=0 fallback=0 installed=0] now [inframe entries=9 draws=8 refused=1 nobyte=1 "
        "nolatch=0 noctx=0 facade=0 closed=0 fallback=0 installed=1]", last));
    CHECK(last.entries == 9u);
    CHECK(last.draws == 8u);
    CHECK(last.installed);
}

TEST_CASE("the published group censors a Report read that disagrees with it") {
    LogFacts f;
    f.OnLine(kBootAttached);
    f.OnLine(kStateInGame);

    const char* kLine =
        "csregister: Running [ticks owner=4000 draws=4000] "
        "[inframe entries=4000 draws=0 refused=4000 nobyte=4000 nolatch=0 noctx=0 facade=0 "
        "closed=0 fallback=0 installed=1] [threads owner=1234 callback=0]";
    InFrameLine line;
    REQUIRE(ParseInFrameLine(kLine, line));

    // W2-F's case 1 as the probe should read it: every draw refused at the site.
    StatusFacts s;
    s.csAsked = true;
    strcpy_s(s.csStatus, "Running");
    s.csOwnerTicksKnown = true;
    s.csOwnerTicks = 4000;
    s.csEnableStampsKnown = true;
    s.csEnableStamps = 1;
    s.csInFrameKnown = true;
    s.csInFrameInstalled = true;
    s.csInFrameBaselineKnown = true;
    strcpy_s(s.csInFrameBaselineStamp, "enable");
    s.csFrameSiteEntries = 4000;
    s.csInFrameDraws = 0;
    s.csRefusedDraws = 4000;
    s.csRefusedNoFrameByte = 4000;
    CHECK(InFrameLineAgrees(line, s));

    // â˜… THE MUTANT THE CROSS-CHECK EXISTS FOR: the sampler wired back to marshalledDraws. On this
    // launch marshalledDraws is 4000 (band 3 consumed every record) while inFrameDraws is 0, so
    // the struct read would claim a full-coverage pass off a site that refused every draw. The
    // published group disagrees, and the row is censored rather than passed.
    StatusFacts mutant = s;
    mutant.csInFrameDraws = 4000;   // = r.marshalledDraws
    mutant.csRefusedDraws = 4000;
    CHECK_FALSE(InFrameLineAgrees(line, mutant));

    mutant.csInFramePublished = true;
    mutant.csInFrameLineAgrees = false;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, mutant) == Ask::CouldNotAsk);
    CHECK(DetailMentions("INSTRUMENT FAULT"));
    CHECK(DetailMentions("DISAGREE"));

    // ...and with the same numbers agreeing, the row scores exactly as it did before: the
    // cross-check adds an arm, it does not move a verdict.
    StatusFacts agreeing = s;
    agreeing.csInFramePublished = true;
    agreeing.csInFrameLineAgrees = true;
    CHECK(Ask1("w2r-panel-drawn-over-level", f, agreeing) == Ask::Fail);
    CHECK(DetailMentions("REFUSED AT THE SITE ON 4000 OF 4000"));

    // A record carrying no group at all is not a disagreement -- `published` false is the state
    // "there was nothing to compare with", which must not censor anything.
    StatusFacts unpublished = s;
    CHECK_FALSE(unpublished.csInFramePublished);
    CHECK_FALSE(unpublished.csInFrameLineAgrees);
    CHECK(Ask1("w2r-panel-drawn-over-level", f, unpublished) == Ask::Fail);

    // Each field of the group is load-bearing: moving any ONE of the ten breaks agreement.
    StatusFacts one = s;
    one.csFallbackEntries = 1;
    CHECK_FALSE(InFrameLineAgrees(line, one));
    one = s;
    one.csInFrameInstalled = false;
    CHECK_FALSE(InFrameLineAgrees(line, one));
    one = s;
    one.csRefusedNoLatch = 1;
    CHECK_FALSE(InFrameLineAgrees(line, one));
    one = s;
    one.csPanelClosedEntries = 1;
    CHECK_FALSE(InFrameLineAgrees(line, one));
    one = s;
    one.csRefusedByFacade = 1;
    CHECK_FALSE(InFrameLineAgrees(line, one));
    one = s;
    one.csRefusedNoContext = 1;
    CHECK_FALSE(InFrameLineAgrees(line, one));
    one = s;
    one.csFrameSiteEntries = 3999;
    CHECK_FALSE(InFrameLineAgrees(line, one));
    one = s;
    one.csRefusedDraws = 3999;
    CHECK_FALSE(InFrameLineAgrees(line, one));
    one = s;
    one.csRefusedNoFrameByte = 3999;
    CHECK_FALSE(InFrameLineAgrees(line, one));
}

// RETROACTIVE, against L5 #4's own recorded observer record
// (launch run L5-d7cd260, its probes/probe_replay/done.json): it was written
// before any of this existed and carries NO row-2.5 and NO frame-open field. Both rows must say
// which field is missing and neither may fail -- an absent field is not a failed measurement.
TEST_CASE("L5 #4's record: both in-frame rows could-not-ask by name, neither fails") {
    LogFacts f;
    f.OnLine(kBootAttached);
    f.OnLine(kStateInGame);

    // The numbers are that record's own, off its w2f-csregister-epoch and w2r-panel-drawn rows.
    StatusFacts l5run4;
    l5run4.csAsked = true;
    strcpy_s(l5run4.csStatus, "Running");
    l5run4.csInvocations = 6351;
    l5run4.csMarshalledDraws = 6350;
    l5run4.csOwnerTicksKnown = true;
    l5run4.csOwnerTicks = 6925;
    l5run4.panelEnabled = true;
    l5run4.panelVisible = true;
    l5run4.browserOpen = true;

    // That record filed this row asked-FAIL ("THE PANEL DREW ON ONLY 6350 OF 6925 OWNER TICKS").
    // Under ARCH-15 S4.1 that verdict was scored off a counter of draw ATTEMPTS, so it is now a
    // could-not-ask naming the field the record does not carry -- a retroactive DOWNGRADE to
    // could-not-ask, never a pass and never a fail on evidence that cannot bear it.
    CHECK(Ask1("w2r-panel-drawn-over-level", f, l5run4) == Ask::CouldNotAsk);
    CHECK(DetailMentions("NOT EVALUABLE"));
    CHECK(DetailMentions("frameSiteEntries"));
    CHECK(DetailMentions("6350"));

    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, l5run4) == Ask::CouldNotAsk);
    CHECK(DetailMentions("GUI.FRAME_OPEN_BYTE"));

    // Its csregister detail line, verbatim from the record: the threads group is there and the
    // inframe group is not, so the cross-check has nothing to compare with and says so.
    InFrameLine none;
    CHECK_FALSE(ParseInFrameLine(
        "status=Running invocations=6351 registrationInvocations=6351 staleInvocations=0 "
        "staleDetected=0 spaceChanges=0 spacesSeen=1 registrations=1 deferrals=210 "
        "marshalledDraws=6350 coalesced=0 pending=1 [threads owner=524680 callback=523720]",
        none));
}

// ARCH-15 S5.3 / RES-W1M-C0 S6. The frame-open row, and the prediction filed in advance.
// ============================================================================================
TEST_CASE("ui-gui-frame-open-at-stage0 asks the byte and can be falsified by it") {
    LogFacts f;
    f.OnLine(kBootAttached);

    StatusFacts unsampled;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, unsampled) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no GUI.FRAME_OPEN_BYTE sample is in this record"));

    StatusFacts noTicks;
    noTicks.csFrameOpenSampled = true;
    noTicks.csGuiContextPresent = true;
    noTicks.csWindowGatePresent = true;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, noTicks) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never taken on a single owner tick"));

    // ARCH-15 S5.3's done-criterion: the could-not-ask arm exercised by a synthetic record with
    // guiContextPresent = false. There was no GUI to be inside of.
    StatusFacts noGui;
    noGui.csFrameOpenSampled = true;
    noGui.csOwnerTicksSampled = 4000;
    noGui.csGuiPresenceKnown = true;
    noGui.csWindowGatePresent = true;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, noGui) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no GUI to be inside of"));
    CHECK(DetailMentions("guiContextPresent=0"));
    StatusFacts noGate = noGui;
    noGate.csGuiContextPresent = true;
    noGate.csWindowGatePresent = false;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, noGate) == Ask::CouldNotAsk);

    // THE PREDICTION: zero on every tick. It is an asked-PASS because the stage-0 drain being
    // outside the frame is the fact the row-2.5 site exists for, not a failure of the product.
    StatusFacts predicted;
    predicted.csFrameOpenSampled = true;
    predicted.csOwnerTicksSampled = 4000;
    predicted.csGuiPresenceKnown = true;
    predicted.csGuiContextPresent = true;
    predicted.csWindowGatePresent = true;
    predicted.csStartFrameLatchPresent = true;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, predicted) == Ask::Pass);
    CHECK(DetailMentions("OUTSIDE THE ENGINE'S GUI FRAME"));
    CHECK(DetailMentions("all 4000 owner tick(s)"));
    CHECK(DetailMentions("not landed"));   // the site half is unasked before W2-F-C1

    // THE FALSIFIER, and the only thing that can produce it: ONE non-zero sample. If this ever
    // fires, RES-W1M-C0 S6 is wrong and ARCH-15's choice of site is wrong with it.
    StatusFacts falsified = predicted;
    falsified.csFrameOpenAtOwnerTick = 1;
    falsified.csFrameOpenLastNonZeroTick = 2731;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, falsified) == Ask::Fail);
    CHECK(DetailMentions("NON-ZERO AT THE STAGE-0 DRAIN on 1 of 4000"));
    CHECK(DetailMentions("most recently on tick 2731"));
    CHECK(DetailMentions("FALSIFIES"));

    // The other half of the sentence: the SAME byte read 1 at the row-2.5 site. Two sites, two
    // answers, and that difference is the whole reason the draw was moved.
    StatusFacts bothHalves = predicted;
    bothHalves.csInFrameKnown = true;
    bothHalves.csInFrameInstalled = true;
    bothHalves.csFrameSiteEntries = 3990;
    bothHalves.csInFrameDraws = 3990;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, bothHalves) == Ask::Pass);
    CHECK(DetailMentions("read the byte as 1 on all 3990"));

    StatusFacts siteRefused = bothHalves;
    siteRefused.csInFrameDraws = 0;
    siteRefused.csRefusedDraws = 3990;
    siteRefused.csRefusedNoFrameByte = 3990;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, siteRefused) == Ask::Pass);
    CHECK(DetailMentions("read the byte as 0 on 3990 of its 3990"));
    CHECK(DetailMentions("SECOND finding"));

    StatusFacts siteNeverRan = bothHalves;
    siteNeverRan.csFrameSiteEntries = 0;
    siteNeverRan.csInFrameDraws = 0;
    siteNeverRan.csInFrameInstalled = false;
    CHECK(Ask1("ui-gui-frame-open-at-stage0", f, siteNeverRan) == Ask::Pass);
    CHECK(DetailMentions("never entered"));
}

// ============================================================================================
// Twelve refusals in one launch and no row scored one.
// ============================================================================================
TEST_CASE("w2e-camera-rig-probe reports Probe's result and the refusal's own sentence") {
    // The RECORDED L5 re-run 3 refusal line in the post-162027c noun, twelve times as the launch
    // printed it. The pin below is camera_control.cpp:237's cause sentence, verbatim.
    LogFacts l5;
    l5.OnLine(kStateInGame);
    for (int i = 0; i < 12; ++i) {
        l5.OnLine(kL5CameraRefused);
    }
    CHECK(l5.cameraRefusalCount == 12u);
    StatusFacts neverProbed;
    CHECK(Ask1("w2e-camera-rig-probe", l5, neverProbed) == Ask::Fail);
    CHECK(DetailMentions("REFUSED the rig 12 time(s)"));
    CHECK(DetailMentions("the spectator CS is not in level space 0000027118DA0000's system table"));
    CHECK(DetailMentions("was never called on this arm's tick"));

    // With the probe wired, the same launch names how far the rig got.
    StatusFacts probed = neverProbed;
    probed.cameraProbed = true;
    strcpy_s(probed.cameraProbeResult, "NoRigSlot");
    CHECK(Ask1("w2e-camera-rig-probe", l5, probed) == Ask::Fail);
    CHECK(DetailMentions("last answered NoRigSlot"));

    // No level -> the rig cannot exist yet, so the row asks nothing rather than failing the launch.
    LogFacts noLevel;
    CHECK(Ask1("w2e-camera-rig-probe", noLevel, probed) == Ask::CouldNotAsk);

    // A quiet launch: no refusal line AND no probe is could-not-ask, not a pass.
    LogFacts quiet;
    quiet.OnLine(kStateInGame);
    CHECK(Ask1("w2e-camera-rig-probe", quiet, neverProbed) == Ask::CouldNotAsk);
    CHECK(DetailMentions("nothing in this launch says how far the rig resolved"));
    // ... and a probe that never reached Ready with nothing to blame is a Fail with the word.
    CHECK(Ask1("w2e-camera-rig-probe", quiet, probed) == Ask::Fail);
    CHECK(DetailMentions("never reached Ready"));

    StatusFacts ready = probed;
    ready.cameraProbeReadyEver = true;
    strcpy_s(ready.cameraProbeResult, "Ready");
    CHECK(Ask1("w2e-camera-rig-probe", quiet, ready) == Ask::Pass);
    CHECK(DetailMentions("Ready at least once"));

    // The sentence is re-derived from THIS line: one the reader cannot split leaves no stale cause.
    // The second line carries the current noun, so it IS matched -- it is a refusal this reader
    // recognises and cannot split, which is a different thing from the pre-rename spelling it does
    // not recognise at all (that one never reaches this arm; see the rename case at the bottom).
    LogFacts unparsable;
    unparsable.OnLine(kStateInGame);
    unparsable.OnLine(kL5CameraRefused);
    unparsable.OnLine(
        "12:00:00.000 WARN [camera] camera control refused rig 0 on level space 000001F0 with no "
        "trailer");
    CHECK(unparsable.cameraRefusalWhy[0] == '\0');
    CHECK(Ask1("w2e-camera-rig-probe", unparsable, probed) == Ask::Fail);
    CHECK(DetailMentions("the line carried no cause"));
}

// ---------------------------------------------------------------------------------------------
// ARCH-10 section 4.2's evidence line, one test per reading rule.
//
// The rendered candidates below carry a full <TABLE> tail, so the reader is proved to stop at the
// <Verdict>/<Why> pair rather than swallowing the numbers after it.
// ---------------------------------------------------------------------------------------------
namespace {

const char* const kTail =
    " base=17e9a446180 bytes=4096 alloc=17e9a440000 grow=64 cap=64 count=12 flags=1";

// ARCH-12 4.3's line, rendered from a <Verdict>/<Why> per candidate. "null" and "unreadable" are
// passed through as themselves; the empty string renders a shape the grammar does not define.
// `c2b10` defaults to a judged level space and the two symbols to one agreeing identity, so a case
// that says nothing about the fifth column still emits a GRAMMATICAL line -- the shapes that omit
// the column are LegacyCandidateLine's, and only the retroactive control has any business with
// them. Each candidate gets its OWN pointer: a builder that reused one would trip rule 5's
// aliasing guard in every case here and hide whatever the case was about.
std::string CandidateLine(const char* c8, const char* c2b08, const char* c7af0, const char* c7af8,
                          const char* c2b10 = "Ready/Ready",
                          const char* spawnsym = "A17E5B0C99341122",
                          const char* levelsym = "A17E5B0C99341122") {
    std::string out = "04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180";
    const char* names[5] = {" | +0x8=", " | +0x2B08=", " | +0x2B10=", " | game+0x7AF0=",
                            " | game+0x7AF8="};
    const char* ptrs[5] = {"0000017E9A446180 ", "0000017E9A447200 ", "0000017E9A44A500 ",
                           "0000017E9A448300 ", "0000017E9A449400 "};
    const char* vals[5] = {c8, c2b08, c2b10, c7af0, c7af8};
    for (int i = 0; i < 5; ++i) {
        out += names[i];
        if (strcmp(vals[i], "null") == 0 || strcmp(vals[i], "unreadable") == 0) {
            out += vals[i];
        } else if (vals[i][0] == '\0') {
            out += "not-a-pointer";
        } else {
            out += ptrs[i];
            out += vals[i];
            out += kTail;
        }
    }
    out += " | spawnsym=";
    out += spawnsym;
    out += " | levelsym=";
    out += levelsym;
    return out;
}

// ARCH-10 4.2's line: FOUR columns, no +0x2B10, no symbols. This is the shape the recorded L5 run
// 3 log actually carries, and it exists here for one job -- ARCH-12 4.3's retroactive control.
std::string LegacyCandidateLine(const char* c8, const char* c2b08, const char* c7af0,
                                const char* c7af8) {
    std::string out = "04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180";
    const char* names[4] = {" | +0x8=", " | +0x2B08=", " | game+0x7AF0=", " | game+0x7AF8="};
    const char* ptrs[4] = {"0000017E9A446180 ", "0000017E9A447200 ", "0000017E9A448300 ",
                           "0000017E9A449400 "};
    const char* vals[4] = {c8, c2b08, c7af0, c7af8};
    for (int i = 0; i < 4; ++i) {
        out += names[i];
        if (strcmp(vals[i], "null") == 0 || strcmp(vals[i], "unreadable") == 0) {
            out += vals[i];
        } else {
            out += ptrs[i];
            out += vals[i];
            out += kTail;
        }
    }
    return out;
}

// The build's own reading of +0x8: the CR15Game, which the guard must refuse (4.2 rule 5).
const char* const kGameNotReady = "NotReady/CapacityOverBlock";

}  // namespace

TEST_CASE("cs-space-candidates: rule 1, an absent line is could-not-ask about the LINE and nothing else") {
    LogFacts f;
    StatusFacts s;
    s.observedMs = 42000.0;
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no [entity] cs-space candidates line in 42000 ms"));
    // Never a statement about the level or the bind: that inference IS the L5 defect.
    CHECK_FALSE(DetailMentions("level"));
    CHECK_FALSE(DetailMentions("bound game space"));

    // A launch that reached a level AND bound, with no candidates line, is still could-not-ask:
    // the absence of this line is not evidence about the offset in either direction.
    f.OnLine(kStateInGame);
    f.OnLine(kBound);
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);
}

TEST_CASE("cs-space-candidates: ARCH-12 4.3 rule 2, +0x2B10 judged is the row's subject") {
    LogFacts f;
    StatusFacts s;
    f.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "unreadable", "Ready/Ok").c_str());
    CHECK(strcmp(f.csSpaceAt2B10.verdict, "Ready") == 0);
    CHECK(strcmp(f.csSpaceAt2B10.rendered, "Ready/Ok") == 0);
    CHECK(f.csSpaceAt2B10KeyPresent);
    REQUIRE(f.csSpaceSeen);
    CHECK(f.csSpaceCount == 1u);
    CHECK(strcmp(f.csSpaceAt2B08.verdict, "Ready") == 0);
    CHECK(strcmp(f.csSpaceAt2B08.rendered, "Ready/Ready") == 0);   // the pair, not the base= tail
    CHECK(strcmp(f.csSpaceAt8.verdict, "NotReady") == 0);
    CHECK(f.csSpaceAt7AF0.read);
    CHECK_FALSE(f.csSpaceAt7AF0.judged);
    CHECK(strcmp(f.csSpaceAt7AF0.rendered, "null") == 0);
    CHECK(strcmp(f.csSpaceAt7AF8.rendered, "unreadable") == 0);

    CHECK(Ask1("cs-space-candidates", f, s) == Ask::Pass);
    CHECK(DetailMentions("the netGame's LEVEL space is present and judged"));
    CHECK(DetailMentions("+0x2B10 rendered Ready on the last of 1 sample(s)"));
    // R2/R4's differential arm, reported beside the subject and four-state.
    CHECK(DetailMentions("Differential ok: +0x2B08 0000017E9A447200 and +0x2B10 0000017E9A44A500"));
    // Rule 6: the identity pair is INFORMATION and says so in as many words.
    CHECK(DetailMentions("spawnsym=A17E5B0C99341122 levelsym=A17E5B0C99341122 (AGREE"));
    CHECK(DetailMentions("information only"));
    // The other three are still listed verbatim: they are the launch's own record of what else the
    // guard judged, and an operator reading this row gets them without a second query.
    CHECK(DetailMentions("+0x8=NotReady/CapacityOverBlock"));
    CHECK(DetailMentions("game+0x7AF8=unreadable"));

    // Empty is 4.2 rule 2's other passing verdict: a constructed space whose table is empty is
    // still the space the accessor names.
    LogFacts empty;
    empty.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "null", "Empty/Ok").c_str());
    CHECK(Ask1("cs-space-candidates", empty, s) == Ask::Pass);
    CHECK(DetailMentions("+0x2B10 rendered Empty"));

    // R4, and it is the arm S6 files as four-state: +0x2B08 null is L3/L4's shape. The subject
    // still passes -- no bind step may use one slot's readiness as a proxy for the other's -- and
    // the arm is reported could-not-ask, never as a pass it did not earn.
    LogFacts oneSpace;
    oneSpace.OnLine(CandidateLine(kGameNotReady, "null", "null", "null", "Ready/Ready").c_str());
    CHECK(Ask1("cs-space-candidates", oneSpace, s) == Ask::Pass);
    CHECK(DetailMentions("Differential could-not-ask: +0x2B08 rendered null"));
    CHECK(DetailMentions("not a pass for that arm"));

    // AUDIT_F3H F3H-1, THE OTHER SIDE. The "ok" sentence was guarded on +0x2B08 alone, so a real
    // emitted line whose fifth column is `+0x2B10=null` printed "+0x2B08 <ptr> and +0x2B10  are
    // two distinct non-null spaces" -- a pass word, an empty pointer, and the same line's own data
    // contradicting it. Both of the auditor's inputs are here: null on the could-not-ask side,
    // unreadable on the asked-FAIL side, where the fabricated sentence was what an operator read
    // to diagnose the failure.
    LogFacts levelNull;
    levelNull.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", levelNull, s) == Ask::CouldNotAsk);
    CHECK_FALSE(DetailMentions("Differential ok"));
    CHECK_FALSE(DetailMentions("two distinct non-null spaces"));
    CHECK(DetailMentions("Differential could-not-ask: +0x2B10 rendered null"));
    CHECK(DetailMentions("is one pointer, not a pair"));

    LogFacts levelUnreadable;
    levelUnreadable.OnLine(
        CandidateLine(kGameNotReady, "Ready/Ready", "null", "null", "unreadable").c_str());
    CHECK(Ask1("cs-space-candidates", levelUnreadable, s) == Ask::Fail);
    CHECK_FALSE(DetailMentions("Differential ok"));
    CHECK(DetailMentions("Differential could-not-ask: +0x2B10 rendered unreadable"));
}

// ARCH-12 4.3's RETROACTIVE CONTROL, and the reason the amendment exists. The recorded L5 run 3
// lines carry four columns and no +0x2B10, and a parser that read them as asked-pass because
// +0x2B08 said Ready is the exact defect. A missing column is not a verdict.
TEST_CASE("cs-space-candidates: a four-column line is could-not-ask NAMING the missing column") {
    LogFacts f;
    StatusFacts s;
    f.OnLine(LegacyCandidateLine(kGameNotReady, "Ready/Ready", "null", "Ready/Ready").c_str());
    REQUIRE(f.csSpaceSeen);
    CHECK_FALSE(f.csSpaceAt2B10KeyPresent);
    CHECK_FALSE(f.csSpaceAt2B10.read);
    CHECK(f.csSpaceSpawnSym[0] == '\0');
    CHECK(f.csSpaceLevelSym[0] == '\0');

    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("carries NO +0x2B10 column"));
    CHECK_FALSE(DetailMentions("CONFIRMED"));
    // The +0x2B08 half WAS asked and answered, and that half is reported unchanged.
    CHECK(DetailMentions("The +0x2B08 half was asked and rendered Ready/Ready"));
    CHECK(DetailMentions("+0x2B08=0000017E9A447200 Ready/Ready base="));
    CHECK(DetailMentions("spawnsym=(not rendered) levelsym=(not rendered)"));

    // NEAR MISS. The same recorded shape with +0x2B08 NOT ready: the answer is the same
    // could-not-ask, because it is a statement about the LINE and not about either column.
    LogFacts cold;
    cold.OnLine(LegacyCandidateLine(kGameNotReady, "null", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", cold, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("carries NO +0x2B10 column"));

    // NEAR MISS. A line that DOES carry the column answers -- so the control above is discriminating
    // and not a reader that has simply stopped answering.
    LogFacts whole;
    whole.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "Ready/Ready").c_str());
    CHECK(Ask1("cs-space-candidates", whole, s) == Ask::Pass);

    // NEAR MISS, and the one a substring reader fails: `+0x2B08=` must not answer for `+0x2B10=`,
    // and neither may a `+0x2B10` mentioned anywhere but as a key at a ` | ` boundary.
    LogFacts decoy;
    decoy.OnLine("04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180 | "
                 "+0x8=0000017E9A446180 NotReady/CapacityOverBlock base=1 bytes=1 alloc=1 grow=1 "
                 "cap=1 count=1 flags=1 | +0x2B08=0000017E9A447200 Ready/Ready base=1 bytes=1 "
                 "alloc=1 grow=1 cap=1 count=1 flags=1x+0x2B10=1 | game+0x7AF0=null | "
                 "game+0x7AF8=null");
    CHECK_FALSE(decoy.csSpaceAt2B10KeyPresent);
    CHECK(Ask1("cs-space-candidates", decoy, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("carries NO +0x2B10 column"));
}

// ARCH-12 4.3 rule 5, extended. The netGame's two spaces have to be two OBJECTS.
TEST_CASE("cs-space-candidates: rule 5, +0x2B10 aliased to another slot disqualifies the line") {
    StatusFacts s;
    const char* const kAlias = "0000017E9A44A500 ";

    // The differential's own failure: one pointer wearing both space labels. ARCH-12 R2's third
    // arm, and it fails REGARDLESS of the verdicts -- both columns here read Ready.
    LogFacts f;
    std::string line = CandidateLine(kGameNotReady, "Ready/Ready", "null", "null");
    const size_t at = line.find("0000017E9A447200 ");
    REQUIRE(at != std::string::npos);
    line.replace(at, strlen(kAlias), kAlias);
    f.OnLine(line.c_str());
    REQUIRE(strcmp(f.csSpaceAt2B10.verdict, "Ready") == 0);
    REQUIRE(strcmp(f.csSpaceAt2B08.verdict, "Ready") == 0);
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::Fail);
    CHECK(DetailMentions("ALIASED: +0x2B08 and +0x2B10 rendered the SAME pointer"));
    CHECK_FALSE(DetailMentions("LEVEL space is present and judged"));

    // DIAG-L2 F7. Against each of the OTHER three slots the same equality is not a defect and
    // does not disqualify the line: L2's real line had game+0x7AF0 carrying +0x2B08's value after
    // a late publish, which is the correct topology -- one object reached through the netGame and
    // through the game -- and the wider guard failed the row for it. So each of these is reported
    // and scores nothing, and the row still answers.
    // Keyed needles: `ng=` renders the same 16 digits as `+0x8`, and an unkeyed find would
    // rewrite the netGame pointer instead of the candidate the case is about.
    struct Case { const char* victim; const char* name; } const cases[] = {
        {"+0x8=0000017E9A446180 ", "+0x8"},
        {"game+0x7AF0=0000017E9A448300 ", "game+0x7AF0"},
        {"game+0x7AF8=0000017E9A449400 ", "game+0x7AF8"},
    };
    for (const Case& c : cases) {
        CAPTURE(std::string(c.name));
        std::string one = CandidateLine("Empty/Empty", "Ready/Ready", "Ready/Ready", "Ready/Ready");
        const size_t hit = one.find(c.victim);
        REQUIRE(hit != std::string::npos);
        one.replace(hit + strlen(c.victim) - strlen(kAlias), strlen(kAlias), kAlias);
        LogFacts g;
        g.OnLine(one.c_str());
        CHECK(Ask1("cs-space-candidates", g, s) == Ask::Pass);
        CHECK_FALSE(DetailMentions("ALIASED"));
        std::string want = c.name;
        want += " carries the same pointer as +0x2B10 (0000017E9A44A500), which is one object "
                "reached two ways and not an aliased pair.";
        CHECK(DetailMentions(want.c_str()));
    }

    // NEAR MISS: five DISTINCT pointers, every one of them Ready. Nothing is aliased and the row
    // answers -- so the guard above is firing on the equality and not on the verdicts.
    LogFacts clean;
    clean.OnLine(CandidateLine("Empty/Empty", "Ready/Ready", "Ready/Ready", "Ready/Ready").c_str());
    CHECK(Ask1("cs-space-candidates", clean, s) == Ask::Pass);
}

TEST_CASE("cs-space-candidates: F7 -- a second real space beside +0x2B08 is information, not a tie") {
    // THE L5 RUN 3 DEFECT, in one case. `game+0x7AF8` is a game-level CncaGameSpace of 35 systems
    // and it goes Ready ~0.4 s before the netGame's 99-system space at +0x2B08. Under the old count
    // rule that was "2 candidate(s) judged Ready or Empty" -- asked-fail on a launch that had just
    // confirmed the product fact, and a rule that broke the tie by time would have picked the
    // WRONG one. ARCH-10 section 1 settles it: neither game+0x7AF* is a rival for this accessor.
    LogFacts f;
    StatusFacts s;
    f.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "Ready/Ready").c_str());
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::Pass);
    CHECK(DetailMentions("the netGame's LEVEL space is present and judged"));
    // ARCH-11 S4: the two spaces that both read Ready are separated in the detail by the producer's
    // OWN text, so a reader can tell the 99-system space from the 35-system one. Carried, not
    // parsed -- the count is inside the verbatim run.
    CHECK(DetailMentions("game+0x7AF8=0000017E9A449400 Ready/Ready base="));
    CHECK(DetailMentions("+0x2B08=0000017E9A447200 Ready/Ready base="));
    CHECK(DetailMentions("count=12"));
    CHECK(DetailMentions("information and not rivals"));

    // All three of the other candidates Ready at once changes nothing: the row is not a count.
    LogFacts three;
    three.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "Ready/Ready", "Ready/Ready").c_str());
    CHECK(Ask1("cs-space-candidates", three, s) == Ask::Pass);

    // ...and the converse is the fail this row exists for: the other spaces being Ready is NOT an
    // answer when +0x2B08 is not, and the detail must not name one of them as the right offset.
    LogFacts others;
    others.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "Ready/Ready", "Ready/Ready",
                                "NotReady/NoBlock")
                      .c_str());
    CHECK(Ask1("cs-space-candidates", others, s) == Ask::Fail);
    CHECK(DetailMentions("+0x2B10 rendered NotReady/NoBlock on the last of 1 sample(s)"));
    CHECK(DetailMentions("game-level spaces the product may not use"));
    CHECK_FALSE(DetailMentions("CONFIRMED"));
    CHECK_FALSE(DetailMentions("which offset is right"));
}

TEST_CASE("cs-space-candidates: rule 3, an unconfirmed level space is named by its own rendering") {
    StatusFacts s;
    struct Case { const char* at2b10; const char* expect; } const cases[] = {
        {"unreadable",            "+0x2B10 rendered unreadable"},
        {"NotReady/NoAllocator",  "+0x2B10 rendered NotReady/NoAllocator"},
        {"NotChecked/NotChecked", "+0x2B10 rendered NotChecked/NotChecked"},
        {"Unreadable/HeaderUnreadable", "+0x2B10 rendered Unreadable/HeaderUnreadable"},
    };
    for (const Case& c : cases) {
        LogFacts f;
        f.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "null", c.at2b10).c_str());
        CHECK(Ask1("cs-space-candidates", f, s) == Ask::Fail);
        CHECK(DetailMentions(c.expect));
        CHECK(DetailMentions("is not confirmed in this launch"));
        CHECK_FALSE(DetailMentions("LEVEL space is present and judged"));
    }
}

// ARCH-12 S6's launch table, which is rule 3 being specific: `null` at +0x2B10 is L3/L4's shape --
// sub_14017A050 is the map load, and with no map there is no table to ask about. "Not asked-fail:
// there is no negative answer, only an absent question." The split is what makes the row
// four-state, and it is what stops the amendment filing a refusal against every menu-only launch.
TEST_CASE("cs-space-candidates: a null +0x2B10 splits on whether a level was ever loaded") {
    StatusFacts s;
    const std::string line =
        CandidateLine(kGameNotReady, "Ready/Ready", "null", "null", "null");

    // L3 / L4: no [session] NetGame state 7/8/9 anywhere in the log.
    LogFacts noMap;
    noMap.OnLine(line.c_str());
    CHECK(Ask1("cs-space-candidates", noMap, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no map was loaded and there was no level space to ask about"));
    CHECK(DetailMentions("Not a refusal of +0x2B10"));
    CHECK_FALSE(DetailMentions("is not confirmed in this launch"));

    // L5: the map DID load and the slot is still null. That is a negative ANSWER, and the four-
    // state row must not launder it into "could not ask" -- the same rendering, the opposite word.
    LogFacts loaded;
    loaded.OnLine(kStateInGame);
    loaded.OnLine(line.c_str());
    REQUIRE(loaded.sessionLevelStateSeen);
    CHECK(Ask1("cs-space-candidates", loaded, s) == Ask::Fail);
    CHECK(DetailMentions("+0x2B10 rendered null on the last of 1 sample(s) with a level loaded"));
    CHECK(DetailMentions("is not confirmed in this launch"));
}

TEST_CASE("cs-space-candidates: rule 2, +0x8 Ready voids the launch, and it is latched across samples") {
    LogFacts f;
    StatusFacts s;
    f.OnLine(CandidateLine("Ready/Ok", "Ready/Ok", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::Fail);
    CHECK(DetailMentions("+0x8 rendered Ready"));
    CHECK(DetailMentions("void"));
    CHECK_FALSE(DetailMentions("CONFIRMED"));

    // Rule 2 outranks the confirmation as well as the refusal.
    LogFacts alone;
    alone.OnLine(CandidateLine("Ready/Ok", "NotReady/NoBlock", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", alone, s) == Ask::Fail);
    CHECK(DetailMentions("+0x8 rendered Ready"));

    // ANY sample, not the latest one. The guard judged the CR15Game Ready once; a later sample
    // refusing it does not restore the sensitivity that judgement disproved, and the row must not
    // be talked out of the finding by a bind attempt that happened to land differently.
    LogFacts latched;
    latched.OnLine(CandidateLine("Ready/Ok", "NotReady/NoBlock", "null", "null").c_str());
    latched.OnLine(CandidateLine(kGameNotReady, "Ready/Ok", "null", "null").c_str());
    CHECK(latched.csSpaceCount == 2u);
    CHECK(strcmp(latched.csSpaceAt8.verdict, "NotReady") == 0);   // the LATEST +0x8 is not Ready
    CHECK(latched.csSpaceAt8ReadyEver);
    CHECK(Ask1("cs-space-candidates", latched, s) == Ask::Fail);
    CHECK(DetailMentions("+0x8 rendered Ready on at least one sample"));
    // The detail quotes the sample that did it, not the current one.
    CHECK(DetailMentions("+0x8=0000017E9A446180 Ready/Ok"));
}

TEST_CASE("cs-space-candidates: F3A-1, Empty at +0x8 is noted and counted nowhere") {
    // tableguard answers Empty/Empty when base == 0 && bytes == 0, which is CR15Game+0x420 all
    // zero at boot -- reachable, not hypothetical. Under the old count rule this was a second
    // "judged" candidate: a false tie beside a Ready +0x2B08, and a rule-3 sentence naming the
    // CR15Game as the right offset when +0x2B08 was null. Neither is a thing this row can say.
    LogFacts f;
    StatusFacts s;
    f.OnLine(CandidateLine("Empty/Empty", "Ready/Ready", "null", "null").c_str());
    CHECK(strcmp(f.csSpaceAt8.verdict, "Empty") == 0);
    CHECK_FALSE(f.csSpaceAt8ReadyEver);
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::Pass);
    CHECK(DetailMentions("the netGame's LEVEL space is present and judged"));
    CHECK(DetailMentions("+0x8 rendered Empty"));
    CHECK(DetailMentions("not a second space"));

    // Empty at +0x8 with a null accessor is a plain refusal, and it names +0x2B08 -- never +0x8.
    LogFacts nul;
    nul.OnLine(kStateInGame);
    nul.OnLine(CandidateLine("Empty/Empty", "Ready/Ready", "null", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", nul, s) == Ask::Fail);
    CHECK(DetailMentions("+0x2B10 rendered null"));
    CHECK_FALSE(DetailMentions("which offset is right"));
    CHECK_FALSE(DetailMentions("the only candidate"));
}

TEST_CASE("cs-space-candidates: the latest sample decides, in both directions") {
    StatusFacts s;
    // The recorded L5 run 3 walk: the slot is null before the space exists, Empty while it is being
    // built, Ready once it is. Reading the FIRST sample -- or latching "was Ready once" -- would
    // score two of these three launches wrong.
    LogFacts up;
    up.OnLine(kStateInGame);
    up.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", up, s) == Ask::Fail);
    up.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "Ready/Ready", "Empty/Empty")
                  .c_str());
    CHECK(Ask1("cs-space-candidates", up, s) == Ask::Pass);
    up.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "Ready/Ready", "Ready/Ready")
                  .c_str());
    CHECK(Ask1("cs-space-candidates", up, s) == Ask::Pass);
    CHECK(up.csSpaceCount == 3u);
    CHECK(DetailMentions("on the last of 3 sample(s)"));

    // Downward: a space that was Ready and is gone by the last bind attempt is not confirmed.
    LogFacts down;
    down.OnLine(kStateInGame);
    down.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "null").c_str());
    REQUIRE(Ask1("cs-space-candidates", down, s) == Ask::Pass);
    down.OnLine(CandidateLine(kGameNotReady, "Ready/Ready", "null", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", down, s) == Ask::Fail);
    CHECK(DetailMentions("+0x2B10 rendered null on the last of 2 sample(s)"));
}

TEST_CASE("cs-space-candidates: the verdict is a WORD, not a substring of the Why") {
    LogFacts f;
    StatusFacts s;
    // `NotReady` contains `Ready`, and a <Why> may too. A substring reader scores every one of
    // these as a Ready candidate -- the commonest false pass there is.
    f.OnLine(CandidateLine("NotReady/NotReadyYet", "NotReady/NotReadyYet", "NotReady/PoolNotReady",
                           "NotChecked/NotChecked", "NotReady/NotReadyYet").c_str());
    CHECK(strcmp(f.csSpaceAt8.verdict, "NotReady") == 0);
    CHECK(strcmp(f.csSpaceAt2B08.verdict, "NotReady") == 0);
    CHECK(strcmp(f.csSpaceAt2B10.verdict, "NotReady") == 0);
    CHECK_FALSE(f.csSpaceAt8ReadyEver);          // ...and rule 2's latch is not fooled either
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::Fail);
    CHECK(DetailMentions("+0x2B10 rendered NotReady/NotReadyYet"));
    // ...and the pair is carried verbatim, so the operator reads the Why that was written.
    CHECK(DetailMentions("game+0x7AF0=NotReady/PoolNotReady"));
}

TEST_CASE("cs-space-candidates: a line the reader cannot parse is could-not-ask, and it clears the last one") {
    LogFacts f;
    StatusFacts s;
    s.observedMs = 9000.0;
    f.OnLine(CandidateLine(kGameNotReady, "Ready/Ok", "null", "null").c_str());
    REQUIRE(Ask1("cs-space-candidates", f, s) == Ask::Pass);

    // AUDIT_L5FIX (W2-R minor): a second line of the same shape that this reader cannot parse must
    // not leave the first line's answer standing as the current one.
    f.OnLine(CandidateLine(kGameNotReady, "", "null", "null").c_str());
    CHECK(f.csSpaceCount == 2u);
    CHECK_FALSE(f.csSpaceAt2B08.read);
    CHECK(f.csSpaceAt2B08.verdict[0] == '\0');
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("+0x2B08 did not render"));
    CHECK_FALSE(DetailMentions("LEVEL space is present and judged"));

    // The subject itself is a decider too: a +0x2B10 key that is PRESENT but renders a shape the
    // grammar does not define is could-not-ask, never a refusal of the offset. The key's presence
    // and what it rendered are two different questions and the row answers them separately.
    LogFacts subject;
    subject.OnLine(CandidateLine(kGameNotReady, "Ready/Ok", "null", "null", "").c_str());
    CHECK(subject.csSpaceAt2B10KeyPresent);
    CHECK_FALSE(subject.csSpaceAt2B10.read);
    CHECK(Ask1("cs-space-candidates", subject, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("+0x2B10 did not render"));
    CHECK_FALSE(DetailMentions("carries NO +0x2B10 column"));

    // +0x8 is the other candidate the answer is made of: if the canary is unreadable, the guard's
    // sensitivity is unknown and a confirmation drawn beside it would be unearned.
    LogFacts canary;
    canary.OnLine(CandidateLine("", "Ready/Ok", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", canary, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("+0x8 did not render"));
    CHECK_FALSE(DetailMentions("LEVEL space is present and judged"));

    // The two that decide nothing refuse nothing. A missing `game+0x7AF8` key is reported as
    // `(unparsed)` in the listing and the row still answers -- turning an unreadable INFORMATION
    // field into a could-not-ask would hand the launch's answer to a field nobody scores.
    LogFacts missing;
    missing.OnLine("04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180 | "
                   "+0x8=0000017E9A446180 NotReady/CapacityOverBlock base=1 bytes=1 alloc=1 grow=1 "
                   "cap=1 count=1 flags=1 | +0x2B08=0000017E9A447200 Ready/Ok base=1 bytes=1 "
                   "alloc=1 grow=1 cap=1 count=1 flags=1 | +0x2B10=0000017E9A44A500 Ready/Ok "
                   "base=1 bytes=1 alloc=1 grow=1 cap=1 count=1 flags=1 | game+0x7AF0=null");
    CHECK(missing.csSpaceSeen);
    CHECK_FALSE(missing.csSpaceAt7AF8.read);
    CHECK(Ask1("cs-space-candidates", missing, s) == Ask::Pass);
    CHECK(DetailMentions("game+0x7AF8=(unparsed)"));

    // A pointer with no <TABLE> after it, and a <Verdict> with no <Why>, are both refusals rather
    // than a verdict read off half a rendering.
    LogFacts halves;
    halves.OnLine("04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180 | "
                  "+0x8=0000017E9A446180 | +0x2B08=0000017E9A447200 Ready/ | "
                  "+0x2B10=0000017E9A44A500 Ready/Ok base=1 bytes=1 alloc=1 grow=1 cap=1 count=1 "
                  "flags=1 | game+0x7AF0=null | game+0x7AF8=null");
    CHECK_FALSE(halves.csSpaceAt8.read);
    CHECK_FALSE(halves.csSpaceAt2B08.read);
    CHECK(Ask1("cs-space-candidates", halves, s) == Ask::CouldNotAsk);
}

TEST_CASE("cs-space-candidates: W1-E's own emitted line, byte for byte") {
    // Copied out of W1-E's own unit test MESSAGE at 6d36e8b (the ARCH-12 5.3 rig case), not
    // composed here: <PTR16> is uppercase while <TABLE>'s hex is FormatTable's lowercase, and a
    // reader that assumed one case for both would refuse the real line. Both space candidates
    // carry `Ready/Ready` -- a <Why> equal to the <Verdict>, which is exactly where a reader that
    // split on the wrong side of the '/' still looks right -- and the two tables differ in COUNT,
    // which is what tells the level space from the other one.
    const char* const kW1E =
        "04:16:39.100 INFO [entity] cs-space candidates: ng=000000272EEF20F0 | "
        "+0x8=000001ED23F49000 Empty/Empty base=0 bytes=0 alloc=0 grow=0 cap=0 count=0 flags=0 | "
        "+0x2B08=000000272EEEE110 Ready/Ready base=272eeee570 bytes=64 alloc=272eeee630 grow=4 "
        "cap=4 count=1 flags=0 | +0x2B10=000000272EEEE640 Ready/Ready base=272eeeeaa0 bytes=64 "
        "alloc=272eeeeb60 grow=4 cap=4 count=3 flags=0 | game+0x7AF0=null | game+0x7AF8=null | "
        "spawnsym=A17E5B0C99341122 | levelsym=A17E5B0C99341122";
    LogFacts f;
    StatusFacts s;
    f.OnLine(kW1E);
    REQUIRE(f.csSpaceSeen);
    CHECK(strcmp(f.csSpaceAt8.verdict, "Empty") == 0);
    CHECK(strcmp(f.csSpaceAt8.rendered, "Empty/Empty") == 0);
    CHECK(strcmp(f.csSpaceAt2B08.verdict, "Ready") == 0);
    CHECK(strcmp(f.csSpaceAt2B08.rendered, "Ready/Ready") == 0);
    CHECK(f.csSpaceAt2B10KeyPresent);
    CHECK(strcmp(f.csSpaceAt2B10.verdict, "Ready") == 0);
    CHECK(strcmp(f.csSpaceAt2B10.ptr, "000000272EEEE640") == 0);
    CHECK(strcmp(f.csSpaceAt7AF0.rendered, "null") == 0);
    CHECK(strcmp(f.csSpaceAt7AF8.rendered, "null") == 0);
    CHECK(strcmp(f.csSpaceSpawnSym, "A17E5B0C99341122") == 0);
    CHECK(strcmp(f.csSpaceLevelSym, "A17E5B0C99341122") == 0);
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::Pass);
    CHECK(DetailMentions("the netGame's LEVEL space is present and judged"));
    // The count is CARRIED, not parsed -- this reader holds no copy of FormatTable's format.
    CHECK(DetailMentions("+0x2B10=000000272EEEE640 Ready/Ready base=272eeeeaa0"));
    CHECK(DetailMentions("count=3"));
    CHECK(DetailMentions("(AGREE; information only"));

    // The same line with the guard judging the CR15Game Ready as well. Rule 4 voids it, even
    // though +0x2B10 is Ready too and rule 2 would otherwise read as a confirmation.
    std::string voided = kW1E;
    const size_t at = voided.find("Empty/Empty");
    REQUIRE(at != std::string::npos);
    voided.replace(at, strlen("Empty/Empty"), "Ready/Ready");
    LogFacts g;
    g.OnLine(voided.c_str());
    CHECK(strcmp(g.csSpaceAt8.verdict, "Ready") == 0);
    CHECK(strcmp(g.csSpaceAt2B10.verdict, "Ready") == 0);
    CHECK(Ask1("cs-space-candidates", g, s) == Ask::Fail);
    CHECK(DetailMentions("+0x8 rendered Ready"));
    CHECK_FALSE(DetailMentions("CONFIRMED"));

    // ARCH-12 R4: a null +0x2B08 beside a judged +0x2B10 is the DIFFERENTIAL arm unanswered, not
    // the subject refused. No bind step may use one slot's readiness as a proxy for the other's,
    // so the row passes on its subject and reports the arm four-state.
    LogFacts nul;
    std::string nulled = kW1E;
    const size_t b = nulled.find("+0x2B08=");
    const size_t e = nulled.find(" | +0x2B10=");
    REQUIRE(b != std::string::npos);
    REQUIRE(e != std::string::npos);
    nulled.replace(b, e - b, "+0x2B08=null");
    nul.OnLine(nulled.c_str());
    CHECK(nul.csSpaceAt2B08.read);
    CHECK_FALSE(nul.csSpaceAt2B08.judged);
    CHECK(Ask1("cs-space-candidates", nul, s) == Ask::Pass);
    CHECK(DetailMentions("Differential could-not-ask: +0x2B08 rendered null"));
    CHECK(DetailMentions("+0x2B08=null"));

    // ...and the same line with the SUBJECT nulled instead. The map loaded, so this one is the
    // refutation: the same word in two columns, two different verdicts.
    LogFacts subjectNull;
    std::string flipped = kW1E;
    const size_t sb = flipped.find("+0x2B10=");
    const size_t se = flipped.find(" | game+0x7AF0=");
    REQUIRE(sb != std::string::npos);
    REQUIRE(se != std::string::npos);
    flipped.replace(sb, se - sb, "+0x2B10=null");
    subjectNull.OnLine(kStateInGame);
    subjectNull.OnLine(flipped.c_str());
    CHECK(Ask1("cs-space-candidates", subjectNull, s) == Ask::Fail);
    CHECK(DetailMentions("+0x2B10 rendered null on the last of 1 sample(s) with a level loaded"));
}

TEST_CASE("cs-space-candidates: the RECORDED L5 run 3 line, all three samples, byte for byte") {
    // Transcribed from launch run L5-8abcbbf 20260902-165645Z, launch-01/
    // gamelogs/echoreplay.log -- the three `[entity] cs-space candidates:` samples of the launch
    // this row scored asked-fail. The product fact was CONFIRMED in that launch (`[entity] bound
    // game space` fired on the +0x2B08 object); the check was what was wrong. Fed in order, the
    // row now walks refusal -> confirmation and ends where the launch ended.
    StatusFacts s;
    LogFacts f;
    const char* const kSamples[3] = {
        "12:56:52.831 INFO  [entity] cs-space candidates: ng=000002703B046180 | "
        "+0x8=0000027020E00D40 NotReady/CapacityOverBlock base=400000002000 bytes=2199023256064 "
        "alloc=8000000080 grow=70368744179712 cap=54043196648849408 count=1048576 flags=100000000 "
        "| +0x2B08=null | game+0x7AF0=null | game+0x7AF8=00000270F45E0000 Empty/Empty "
        "base=ffffffffffffffff bytes=0 alloc=2702a783720 grow=32 cap=0 count=0 flags=0",

        "12:57:04.659 INFO  [entity] cs-space candidates: ng=000002703B046180 | "
        "+0x8=0000027020E00D40 NotReady/CapacityOverBlock base=400000002000 bytes=2199023256064 "
        "alloc=8000000080 grow=70368744179712 cap=54043196648849408 count=1048576 flags=100000000 "
        "| +0x2B08=0000027118DA0000 Empty/Empty base=ffffffffffffffff bytes=0 alloc=2703ba90f40 "
        "grow=32 cap=0 count=0 flags=0 | game+0x7AF0=null | game+0x7AF8=00000270F45E0000 "
        "Ready/Ready base=270f4630f20 bytes=560 alloc=2702a783720 grow=0 cap=35 count=35 flags=0",

        "12:57:05.050 INFO  [entity] cs-space candidates: ng=000002703B046180 | "
        "+0x8=0000027020E00D40 NotReady/CapacityOverBlock base=400000002000 bytes=2199023256064 "
        "alloc=8000000080 grow=70368744179712 cap=54043196648849408 count=1048576 flags=100000000 "
        "| +0x2B08=0000027118DA0000 Ready/Ready base=27118deb1a0 bytes=1584 alloc=2703ba90f40 "
        "grow=0 cap=99 count=99 flags=0 | game+0x7AF0=null | game+0x7AF8=00000270F45E0000 "
        "Ready/Ready base=270f4630f20 bytes=560 alloc=2702a783720 grow=0 cap=35 count=35 flags=0",
    };

    // ARCH-12 4.3's RETROACTIVE CONTROL, run on the recorded bytes it was written for. These
    // three samples carry FOUR columns and no +0x2B10 -- ARCH-12 2.4: R14NetSpectatorCamera is not
    // among the 99 systems of *(ng+0x2B08) because it was never registered there, and the object
    // that has it was never measured. Every sample must therefore read could-not-ask NAMING the
    // missing column. A parser that scores sample 3 asked-pass because +0x2B08 says Ready is the
    // exact defect the amendment closes, and this is the artifact that would hide it.
    f.OnLine(kSamples[0]);
    CHECK_FALSE(f.csSpaceAt2B10KeyPresent);
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("carries NO +0x2B10 column"));
    // On this exact sample the OLD rule 3 fired: game+0x7AF8 was the only Empty candidate, so the
    // row said "the product fact is wrong and this launch says which offset is right" and named a
    // game-level space as the netGame accessor. It is the wrong answer stated affirmatively, and
    // it is the sentence the ruling removed.
    CHECK(DetailMentions("game+0x7AF8=00000270F45E0000 Empty/Empty base="));
    CHECK_FALSE(DetailMentions("which offset is right"));
    CHECK_FALSE(DetailMentions("the only candidate"));

    f.OnLine(kSamples[1]);
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);

    f.OnLine(kSamples[2]);
    CHECK(f.csSpaceCount == 3u);
    CHECK_FALSE(f.csSpaceAt8ReadyEver);      // CapacityOverBlock on every one of the three
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("carries NO +0x2B10 column"));
    CHECK_FALSE(DetailMentions("LEVEL space is present and judged"));
    // The +0x2B08 half was asked and answered, and it is reported UNCHANGED: a missing column
    // takes nothing away from the column that was there.
    CHECK(DetailMentions("The +0x2B08 half was asked and rendered Ready/Ready"));
    // Both of the other real spaces are named in the detail as information.
    CHECK(DetailMentions("+0x8=NotReady/CapacityOverBlock"));
    CHECK(DetailMentions("game+0x7AF0=null"));
    // ...and the launch is not reported as a refusal, which is the whole of F7.
    CHECK_FALSE(DetailMentions("candidate(s) judged Ready or Empty"));

    // AUDIT_L5_RUN3 section 12 item 4: the detail must let a reader separate the two Ready spaces.
    // These are the RECORDED numbers, and they arrive as the producer's own text.
    CHECK(DetailMentions("+0x2B08=0000027118DA0000 Ready/Ready base=27118deb1a0 bytes=1584 "
                         "alloc=2703ba90f40 grow=0 cap=99 count=99 flags=0"));
    CHECK(DetailMentions("game+0x7AF8=00000270F45E0000 Ready/Ready base=270f4630f20 bytes=560 "
                         "alloc=2702a783720 grow=0 cap=35 count=35 flags=0"));

    // ...and the LINE the could-not-ask branch quotes is the whole line. The real samples are 478
    // and 477 message bytes; the buffer that held them was 400, so this used to be a silent cut.
    CHECK_FALSE(f.csSpaceLineTruncated);
    CHECK(strlen(f.csSpaceLine) > 400u); // facts-ok: AUDIO.LISTENER_COUNT_OFF the old buffer length the line was cut at, not that offset
    CHECK(strstr(f.csSpaceLine, "cap=35 count=35 flags=0") != nullptr);
}

// ARCH-11 S4's NEW aliasing guard, and the buffer that used to eat the evidence.
TEST_CASE("cs-space-candidates: two offsets naming ONE object distinguish nothing") {
    LogFacts f;
    StatusFacts s;
    // +0x2B08 and game+0x7AF8 render the SAME pointer. Both say Ready, so every other rule in the
    // row would confirm the accessor off a line that separates no two objects.
    f.OnLine("04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180 | "
             "+0x8=0000017E9A446180 NotReady/CapacityOverBlock base=1 bytes=1 alloc=1 grow=1 cap=1 "
             "count=1 flags=1 | +0x2B08=0000017E9A447200 Ready/Ready base=2 bytes=2 alloc=2 grow=2 "
             "cap=99 count=99 flags=0 | +0x2B10=0000017E9A44A500 Ready/Ready base=4 bytes=4 "
             "alloc=4 grow=4 cap=35 count=35 flags=0 | game+0x7AF0=null | "
             "game+0x7AF8=0000017E9A447200 Ready/Ready "
             "base=2 bytes=2 alloc=2 grow=2 cap=35 count=35 flags=0");
    // DIAG-L2 F7 narrowed this: +0x2B08 and game+0x7AF8 naming one object is the netGame's space
    // reached through the game as well, not two labels on one candidate for THIS row's subject.
    // The equality is rendered, it scores nothing, and the row answers on +0x2B10 as before.
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::Pass);
    CHECK_FALSE(DetailMentions("ALIASED"));
    CHECK(DetailMentions("game+0x7AF8 carries the same pointer as +0x2B08 (0000017E9A447200), "
                         "which is one object reached two ways and not an aliased pair."));

    // The canary aliasing the accessor is the same defect from the other side.
    LogFacts canary;
    canary.OnLine("04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180 | "
                  "+0x8=0000017E9A447200 NotReady/CapacityOverBlock base=1 bytes=1 alloc=1 grow=1 "
                  "cap=1 count=1 flags=1 | +0x2B08=0000017E9A447200 Ready/Ready base=2 bytes=2 "
                  "alloc=2 grow=2 cap=99 count=99 flags=0 | +0x2B10=0000017E9A44A500 Ready/Ready "
                  "base=4 bytes=4 alloc=4 grow=4 cap=35 count=35 flags=0 | game+0x7AF0=null | "
                  "game+0x7AF8=0000017E9A447300 Ready/Ready base=3 bytes=3 alloc=3 grow=3 cap=35 "
                  "count=35 flags=0");
    CHECK(Ask1("cs-space-candidates", canary, s) == Ask::Pass);
    CHECK_FALSE(DetailMentions("ALIASED"));
    CHECK(DetailMentions("+0x8 carries the same pointer as +0x2B08 (0000017E9A447200), which is "
                         "one object reached two ways and not an aliased pair."));

    // `null` is not a pointer and two nulls are not an alias: the guard keys on what was rendered.
    LogFacts nulls;
    nulls.OnLine(CandidateLine("NotReady/CapacityOverBlock", "Ready/Ready", "null", "null").c_str());
    CHECK(Ask1("cs-space-candidates", nulls, s) == Ask::Pass);
    CHECK_FALSE(DetailMentions("ALIASED"));

    // The truncation flag is a real signal and not decoration: a line past the buffer says so.
    // Sized from the buffer rather than to a number, so a line that outgrows a future widening
    // still proves the flag rather than quietly fitting: the pad is the whole capacity again.
    std::string huge = "04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180 | "
                       "+0x8=0000017E9A446180 NotReady/X base=1 | +0x2B08=0000017E9A447200 "
                       "Ready/Ready pad=";
    LogFacts big;
    huge.append(sizeof big.csSpaceLine, 'z');
    big.OnLine(huge.c_str());
    CHECK(big.csSpaceLineTruncated);
    CHECK(strlen(big.csSpaceLine) == sizeof(big.csSpaceLine) - 1);
    // ...and the widening is real: the whole ARCH-12 4.3 line, every field at its widest, FITS.
    // 1331 bytes is the arithmetic in observer_rows.h, and this is the check that it was done.
    std::string widest = "04:16:39.100 INFO [entity] cs-space candidates: ng=0000017E9A446180";
    const char* keys[5] = {" | +0x8=", " | +0x2B08=", " | +0x2B10=", " | game+0x7AF0=",
                           " | game+0x7AF8="};
    for (int i = 0; i < 5; ++i) {
        widest += keys[i];
        widest += "0000017E9A44650";
        widest += char('0' + i);
        widest += ' ';
        widest.append(218, 'w');
    }
    widest += " | spawnsym=A17E5B0C99341122 | levelsym=A17E5B0C99341122";
    LogFacts fits;
    fits.OnLine(widest.c_str());
    CHECK_FALSE(fits.csSpaceLineTruncated);
    CHECK(strlen(fits.csSpaceLine) == widest.size() - strlen("04:16:39.100 INFO [entity] "));
}

TEST_CASE("cs-space-candidates: the prefix inside another message is not this line") {
    LogFacts f;
    StatusFacts s;
    // A near miss with the whole shape quoted inside a different message. StartsWith anchors it.
    f.OnLine("04:16:39.100 INFO [entity] deferring bind 7032: cs-space candidates: "
             "ng=0000017E9A446180 | +0x8=0000017E9A446180 Ready/Ok base=1 bytes=1 alloc=1 grow=1 "
             "cap=1 count=1 flags=1 | +0x2B08=null | game+0x7AF0=null | game+0x7AF8=null");
    CHECK_FALSE(f.csSpaceSeen);
    CHECK_FALSE(f.csSpaceAt8ReadyEver);      // the latch is not armed by a quoted line either
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no [entity] cs-space candidates line"));

    // The tag is part of the match too: another module quoting the line does not answer for W1-E.
    LogFacts wrongTag;
    wrongTag.OnLine("04:16:39.100 INFO [playback] cs-space candidates: ng=0000017E9A446180 | "
                    "+0x8=0000017E9A446180 Ready/Ok base=1 bytes=1 alloc=1 grow=1 cap=1 count=1 "
                    "flags=1 | +0x2B08=null | game+0x7AF0=null | game+0x7AF8=null");
    CHECK_FALSE(wrongTag.csSpaceSeen);
    CHECK_FALSE(wrongTag.csSpaceAt8ReadyEver);
}

TEST_CASE("the level-load predicate reads [session]'s own state line, and only that line") {
    LogFacts f;
    CHECK_FALSE(LevelReached(f));
    CHECK_FALSE(f.sessionStateSeen);

    // A bind with no state line does NOT make a level: that direction is the proxy this row lost.
    f.OnLine(kBound);
    CHECK_FALSE(LevelReached(f));

    LogFacts g;
    g.OnLine(kStateLobby);
    CHECK(g.sessionStateSeen);
    CHECK(g.sessionState == 5);          // the state the transition ARRIVED at, not the 4 it left
    CHECK_FALSE(LevelReached(g));

    g.OnLine(kStateLoadingLevel);
    CHECK(LevelReached(g));
    CHECK(g.sessionLevelState == 7);

    // Monotonic: a level was reached in this launch even after the session leaves it again.
    g.OnLine("04:20:00.000 INFO [session] NetGame state 9 (in game) -> 5 (lobby)");
    CHECK(g.sessionState == 5);
    CHECK(LevelReached(g));
    CHECK(g.sessionLevelState == 7);

    // Near misses: another module's line, and a [session] line that is not a transition.
    LogFacts wrongTag;
    wrongTag.OnLine("04:16:38.073 INFO [playback] NetGame state 5 (lobby) -> 9 (in game)");
    CHECK_FALSE(wrongTag.sessionStateSeen);
    CHECK_FALSE(LevelReached(wrongTag));

    LogFacts noArrow;
    noArrow.OnLine("04:16:38.073 INFO [session] NetGame state 9 is not a transition");
    CHECK_FALSE(noArrow.sessionStateSeen);
    CHECK_FALSE(LevelReached(noArrow));

    // 8 and 9 are level states too, so a launch that only sees the later transition still counts.
    LogFacts late;
    late.OnLine(kStateInGame);
    CHECK(LevelReached(late));
    CHECK(late.sessionLevelState == 9);
}

TEST_CASE("a second, malformed view-read refusal does not leave the first session's cause standing") {
    // W2-R minor. TextBetween used to leave `out` untouched on a failed parse, so the
    // row went on reporting a cause the current line does not carry.
    LogFacts f;
    StatusFacts s;
    f.OnLine("12:00:07.000 WARN [camera] camera control view read refused: the view pose owner is "
             "the game; refusals 4; pose-owner: game, conflicts 0, arb none, rel-refused 0");
    REQUIRE(Ask1("w2e-view-read-refusal-names-its-site", f, s) == Ask::Pass);
    CHECK(DetailMentions("the view pose owner is the game"));
    CHECK(f.cameraViewRefusals == 4u);

    f.OnLine("12:00:09.000 WARN [camera] camera control view read refused: truncated with no count");
    CHECK(f.cameraViewRefusal[0] == '\0');
    CHECK(f.cameraViewRefusals == 0u);
    CHECK(f.cameraViewRefusalPoseOwner[0] == '\0');
    CHECK(Ask1("w2e-view-read-refusal-names-its-site", f, s) == Ask::Fail);
    CHECK_FALSE(DetailMentions("the view pose owner is the game"));

    // TextBetween itself, at the level the row cannot see: a failed parse writes the empty string.
    char out[64];
    strcpy_s(out, "stale");
    CHECK_FALSE(TextBetween("no key here", "key: ", "; end", out, sizeof out));
    CHECK(out[0] == '\0');
    strcpy_s(out, "stale");
    CHECK_FALSE(TextBetween("key: text with no end", "key: ", "; end", out, sizeof out));
    CHECK(out[0] == '\0');
}

TEST_CASE("a capture close that closed no capture is not a stop the sentinel can be scored on") {
    // The three lines src/capture/capture.cpp prints under `[capture] closed: `, and only the first
    // of them means a capture happened.
    LogFacts real;
    real.OnLine("12:00:30.000 INFO [capture] closed: 4210 records, 3 chapters, 12 keyframes, 2 verbatim, 0 refusals");
    CHECK(real.captureClosedSeen);
    CHECK(real.captureClosedEmpty == false);

    const char* empties[] = {
        "12:00:30.000 WARN [capture] closed: nothing was recording when the 60 s capture deadline was reached",
        "12:00:30.000 WARN [capture] closed: capture.enabled is false, so the 60 s deadline has no capture to end",
    };
    StatusFacts s;
    s.stopSentinelNameKnown = true;
    strcpy_s(s.stopSentinelName, "stop-capture-L2-4812");
    strcpy_s(s.stopSentinelDir, "C:/echo/echoreplay/state");
    s.stopSentinelPresent = true;
    CHECK(Ask1("w2a-capture-stop-sentinel", real, s) == Ask::Pass);
    for (const char* line : empties) {
        LogFacts empty;
        empty.OnLine(line);
        CHECK(empty.captureClosedSeen);
        CHECK(empty.captureClosedEmpty);
        CHECK(Ask1("w2a-capture-stop-sentinel", empty, s) == Ask::CouldNotAsk);
        CHECK(DetailMentions("nothing-captured"));
        // Absent is could-not-ask too: a mechanism that was never reached must not be blamed.
        StatusFacts absent = s;
        absent.stopSentinelPresent = false;
        CHECK(Ask1("w2a-capture-stop-sentinel", empty, absent) == Ask::CouldNotAsk);
    }
    StatusFacts absentReal = s;
    absentReal.stopSentinelPresent = false;
    CHECK(Ask1("w2a-capture-stop-sentinel", real, absentReal) == Ask::Fail);
}

// -------------------------------------------------------------------------------------------
// W1-J's egress record. The harness has exactly one probe-side source for the last layer, and L5
// filed none: it fell back to the game log and reported the collection defect. These drive the
// composer the way probe_replay.cpp calls it, with the values the product's accessors return.
// -------------------------------------------------------------------------------------------

TEST_CASE("the egress record carries the product's own state word, and its hook set") {
    EgressFacts armed;
    armed.state = "armed";
    armed.armedBy = "sweep-override";
    armed.hooksJson = "\"connect\",\"WSAConnect\",\"sendto\"";
    armed.uncovered = "raw sockets, and \"WinHTTP\" over a handle we never see";
    armed.armedAtMs = 1471;
    armed.refused = 0;

    char hot[1024];
    hot[0] = '\0';
    CHECK(AppendEgressObject(hot, sizeof hot, armed));
    CHECK(strstr(hot, "\"egress\": {") != nullptr);
    CHECK(strstr(hot, "\"state\": \"armed\"") != nullptr);
    CHECK(strstr(hot, "\"hooks\": [\"connect\",\"WSAConnect\",\"sendto\"]") != nullptr);
    CHECK(strstr(hot, "\"armedAtMs\": 1471") != nullptr);
    CHECK(strstr(hot, "\"refused\": 0") != nullptr);
    // W1-J reports the sweep override as UNEXERCISED unless this string carries `sweep`.
    CHECK(strstr(hot, "\"armedBy\": \"sweep-override\"") != nullptr);
    // CoverageNote is a sentence, and a quote inside it must not end the JSON string early.
    CHECK(strstr(hot, "\\\"WinHTTP\\\"") != nullptr);

    // NOT ARMED is a state the record STATES, never a silence -- and a composer that spelled a
    // constant instead of copying f.state would put the armed word here too.
    EgressFacts cold = armed;
    cold.state = "not-attempted";
    cold.armedBy = "live-online-capture";
    cold.hooksJson = "";
    cold.uncovered = nullptr;
    char off[1024];
    off[0] = '\0';
    CHECK(AppendEgressObject(off, sizeof off, cold));
    CHECK(strstr(off, "\"state\": \"not-attempted\"") != nullptr);
    CHECK(strstr(off, "\"state\": \"armed\"") == nullptr);
    CHECK(strstr(off, "\"hooks\": []") != nullptr);
    CHECK(strstr(off, "uncovered") == nullptr);

    // The third state exists for a reason: a half-armed blocker has an OPEN path, and the legacy
    // boolean cannot say so. It must survive the composer as its own word.
    EgressFacts half = armed;
    half.state = "partially-armed";
    char part[1024];
    part[0] = '\0';
    CHECK(AppendEgressObject(part, sizeof part, half));
    CHECK(strstr(part, "\"state\": \"partially-armed\"") != nullptr);
}

TEST_CASE("no readable state files no egress object at all, rather than an empty one") {
    const char* unreadable[] = {nullptr, "", "?"};
    for (const char* word : unreadable) {
        EgressFacts f;
        f.state = word;
        f.armedBy = "sweep-override";
        f.hooksJson = "\"connect\"";
        f.uncovered = nullptr;
        f.armedAtMs = 7;
        f.refused = 3;
        CHECK_FALSE(EgressStateReadable(f));
        char out[256];
        strcpy_s(out, sizeof out, "PRIOR");
        CHECK_FALSE(AppendEgressObject(out, sizeof out, f));
        // An object that sets Reported without attesting anything is worse than none, so the
        // buffer must come back untouched.
        CHECK(strcmp(out, "PRIOR") == 0);
    }
    // The guard is not simply always-false: a state the product does spell is readable.
    EgressFacts real;
    real.state = "failed";
    real.armedBy = "mode-unknown";
    real.hooksJson = "";
    real.uncovered = nullptr;
    real.armedAtMs = 0;
    real.refused = 0;
    CHECK(EgressStateReadable(real));
    // 1024: the object now carries the destination fields too, and a buffer too small for it is
    // the REFUSAL path, which is a different case (below) and not what this one is asking.
    char out[1024];
    out[0] = '\0';
    CHECK(AppendEgressObject(out, sizeof out, real));
    CHECK(strstr(out, "\"state\": \"failed\"") != nullptr);
}

TEST_CASE("an attended value that merely STARTS with true or false is refused, not read") {
    StatusFacts truthy;
    ParseLaunchMarker("{\"windowSec\": 180, \"attended\": truthy}", "marker.json", truthy);
    CHECK_FALSE(truthy.markerAttendedKnown);
    CHECK(strstr(truthy.markerAttendedReason, "neither true nor false") != nullptr);
    CHECK(truthy.markerWindowKnown);

    StatusFacts falsey;
    ParseLaunchMarker("{\"attended\": falsey}", "marker.json", falsey);
    CHECK_FALSE(falsey.markerAttendedKnown);

    // The exact words still parse, with and without punctuation behind them.
    StatusFacts yes;
    ParseLaunchMarker("{\"attended\": true}", "marker.json", yes);
    CHECK(yes.markerAttendedKnown);
    CHECK(yes.markerAttended);

    StatusFacts no;
    ParseLaunchMarker("{\"attended\": false, \"windowSec\": 12}", "marker.json", no);
    CHECK(no.markerAttendedKnown);
    CHECK_FALSE(no.markerAttended);
}

// -------------------------------------------------------------------------------------------
// F1 (L5, 2026-09-02). The record serialiser threw away five correctly computed rows.
//
// `note[768]` held egress::CoverageNote()'s 763 bytes plus a 19-byte wrapper only if you never
// measured it; `strncat_s(..., _TRUNCATE)` cut the string mid-word and REPORTED SUCCESS, the
// closing `"]` met a full buffer and wrote nothing, and done.json was invalid JSON at offset 1820.
// The harness discarded the file and the launch read NOT-RUN.
//
// The test that was there could not have seen it: a 52-character stand-in note and `strstr`
// assertions, neither of which can notice that a document stopped being JSON. So this driver
// PARSES what the writer emits, and drives it at and far past the real length.
// -------------------------------------------------------------------------------------------
namespace {

// A strict JSON validator over the subset done.json is written in. It exists to answer one
// question the old assertions could not ask -- is this still a document? -- and it reports WHERE
// it stopped, so a failure names the offset the way the harness's parser did.
struct JsonScan {
    const char* text;
    size_t at;
    bool ok;

    void Skip() {
        while (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r') {
            ++at;
        }
    }
    bool Take(char c) {
        Skip();
        if (text[at] != c) {
            return ok = false;
        }
        ++at;
        return true;
    }
    bool String() {
        Skip();
        if (text[at] != '"') {
            return ok = false;
        }
        ++at;
        while (text[at] != '"') {
            if (text[at] == '\0') {
                return ok = false;                       // unterminated -- the L5 shape
            }
            if (static_cast<unsigned char>(text[at]) < 0x20) {
                return ok = false;                       // a raw control byte ends a JSON string
            }
            if (text[at] == '\\') {
                ++at;
                if (strchr("\"\\/bfnrtu", text[at]) == nullptr) {
                    return ok = false;
                }
            }
            ++at;
        }
        ++at;
        return true;
    }
    bool Value() {
        Skip();
        const char c = text[at];
        if (c == '"') {
            return String();
        }
        if (c == '{' || c == '[') {
            const char close = (c == '{') ? '}' : ']';
            ++at;
            Skip();
            if (text[at] == close) {
                ++at;
                return true;
            }
            for (;;) {
                if (c == '{') {
                    if (!String() || !Take(':')) {
                        return ok = false;
                    }
                }
                if (!Value()) {
                    return ok = false;
                }
                Skip();
                if (text[at] == ',') {
                    ++at;
                    continue;
                }
                return Take(close);
            }
        }
        if (strncmp(text + at, "true", 4) == 0) { at += 4; return true; }
        if (strncmp(text + at, "false", 5) == 0) { at += 5; return true; }
        if (strncmp(text + at, "null", 4) == 0) { at += 4; return true; }
        const size_t start = at;
        if (text[at] == '-') { ++at; }
        while (text[at] >= '0' && text[at] <= '9') { ++at; }
        if (text[at] == '.') {
            ++at;
            while (text[at] >= '0' && text[at] <= '9') { ++at; }
        }
        return at != start ? true : (ok = false);
    }
};

bool JsonParses(const char* text, size_t* stoppedAt) {
    JsonScan s{text, 0, true};
    const bool value = s.Value();
    s.Skip();
    const bool whole = value && s.ok && text[s.at] == '\0';
    if (stoppedAt != nullptr) {
        *stoppedAt = s.at;
    }
    return whole;
}

// The value of `"uncovered": ["..."]`, unescaped. A round trip is what proves nothing was cut:
// `strstr` for a prefix of the note passes just as happily on a note that stops mid-word.
bool UncoveredRoundTrip(const char* record, std::string& out) {
    const char* at = strstr(record, "\"uncovered\": [\"");
    if (at == nullptr) {
        return false;
    }
    at += strlen("\"uncovered\": [\"");
    out.clear();
    for (; *at != '\0' && *at != '"'; ++at) {
        if (*at != '\\') {
            out += *at;
            continue;
        }
        ++at;
        switch (*at) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '\0': return false;
            default: out += *at; break;
        }
    }
    return *at == '"';
}

// The real note this writer met was 763 bytes. This driver links no product code (T0), so it
// cannot call egress::CoverageNote(); it builds a note of the same SHAPE -- clauses carrying a
// quote and a backslash, so escaping is exercised at length -- of at least a given size.
const size_t kRealCoverageNoteBytes = 763;

std::string CoverageNoteOfAtLeast(size_t want) {
    const char* clause =
        "a raw socket opened before our sweep, a handle duplicated from another process, "
        "\"WinHTTP\" over a handle we never see, and C:\\hooks\\late; ";
    std::string s;
    while (s.size() < want) {
        s += clause;
    }
    return s;
}

std::string RecordAround(const char* egress) {
    std::string doc = "{\n";
    doc += egress;
    doc += "  \"rows\": []\n}\n";
    return doc;
}

}  // namespace

TEST_CASE("the JSON validator rejects the exact shape L5 emitted, and accepts the fixed one") {
    // The instrument first. A checker that says yes to everything would make every case below a
    // false pass, so it is shown refusing an unterminated string inside an unclosed array -- which
    // is byte for byte what done.json ended with: `...and anything befo},`.
    size_t stopped = 0;
    CHECK_FALSE(JsonParses("{\n  \"egress\": {\"uncovered\": [\"and anything befo},\n}\n", &stopped));
    CHECK_FALSE(JsonParses("{\"a\": [\"x\"}", &stopped));
    CHECK_FALSE(JsonParses("{\"a\": 1,}", &stopped));
    CHECK_FALSE(JsonParses("{\"a\": 1} trailing", &stopped));
    CHECK(JsonParses("{\n  \"egress\": {\"uncovered\": [\"and anything before it\"]},\n"
                     "  \"rows\": []\n}\n", &stopped));
    CHECK(JsonParses("[]", &stopped));
    CHECK(JsonParses("{\"a\": [1, -2.5, true, false, null, {\"b\": \"c\\\"d\"}]}", &stopped));
}

TEST_CASE("the egress record survives a coverage note at, and far past, the real one's length") {
    EgressFacts f;
    f.state = "armed";
    f.armedBy = "sweep-override";
    f.hooksJson = "\"connect\",\"WSAConnect\",\"sendto\"";
    f.armedAtMs = 63;
    f.refused = 76;

    // 763 is the length that broke it, and 2x is the control that a bound merely raised to fit
    // that one sample would fail. The old `note[768]` cannot hold either.
    const size_t lengths[] = {kRealCoverageNoteBytes, kRealCoverageNoteBytes * 2};
    for (size_t want : lengths) {
        const std::string note = CoverageNoteOfAtLeast(want);
        REQUIRE(note.size() >= want);
        f.uncovered = note.c_str();

        char record[16384];
        record[0] = '\0';
        REQUIRE(AppendEgressObject(record, sizeof record, f));

        const std::string doc = RecordAround(record);
        size_t stopped = 0;
        const bool parses = JsonParses(doc.c_str(), &stopped);
        INFO("note bytes ", note.size(), ", stopped at ", stopped);
        CHECK(parses);

        // Not a prefix, not a truncation: the note comes back byte for byte.
        std::string back;
        REQUIRE(UncoveredRoundTrip(record, back));
        CHECK(back == note);
        CHECK(back.size() == note.size());

        // The retroactive half. Everything asserted above is emitted through a span the pre-fix
        // 768-byte buffer could not have held, so a revert to it fails this case rather than
        // passing on a note that happens to be short.
        CHECK(strlen(record) > 768);
    }
}

TEST_CASE("a record buffer too small for the egress object REFUSES, and stays JSON") {
    EgressFacts f;
    f.state = "armed";
    f.armedBy = "sweep-override";
    f.hooksJson = "\"connect\"";
    f.armedAtMs = 63;
    f.refused = 76;
    const std::string note = CoverageNoteOfAtLeast(4000);
    f.uncovered = note.c_str();

    char record[512];
    record[0] = '\0';
    // False: no egress was filed. The old path returned true here and left a cut object behind.
    CHECK_FALSE(AppendEgressObject(record, sizeof record, f));
    CHECK(strstr(record, "\"egress\":") == nullptr);
    CHECK(strstr(record, "\"egressRefused\":") != nullptr);
    CHECK(strstr(record, "was NOT truncated") != nullptr);

    size_t stopped = 0;
    CHECK(JsonParses(RecordAround(record).c_str(), &stopped));

    // And the refusal is not the writer's only answer: the same facts with a note the buffer can
    // hold file an object, so this is a bound being respected rather than a path that never writes.
    const std::string small = CoverageNoteOfAtLeast(32);
    f.uncovered = small.c_str();
    // A buffer that CAN hold the object -- which now carries the destination fields too, so the
    // one this refusal was measured against is deliberately not reused for the control.
    std::vector<char> roomy(sizeof record + small.size() + strlen(EgressObjectFormat()), '\0');
    CHECK(AppendEgressObject(&roomy[0], roomy.size(), f));
    CHECK(strstr(&roomy[0], "\"egress\": {") != nullptr);
    CHECK(strstr(&roomy[0], "\"egressRefused\":") == nullptr);
    CHECK(JsonParses(RecordAround(&roomy[0]).c_str(), &stopped));
}

// -------------------------------------------------------------------------------------------
// W2-E's view-read refusal latch (AUDIT_L1 finding 3, producer landed at f404273).
// camera_control latches the FIRST view-read refusal of a binding session and counts all of them;
// camera_module publishes one [camera] line per binding session. These drive the consumer.
// -------------------------------------------------------------------------------------------
namespace {

const char* kViewRefused =
    "12:00:07.500 WARN [camera] camera control view read refused: the view-state array slot at "
    "0x14A2B3C4 is reserved, not committed memory; refusals 3; pose-owner: replay, conflicts 0, "
    "arb none, rel-refused 0";
// The sentence is W1-H's `why` and it can carry a colon, a comma and the word `refused:` itself.
// A consumer that searched for `refused: ` instead of anchoring on the prefix would cut the
// sentence in half here, or read this line as a second refusal.
const char* kViewRefusedInnerColon =
    "12:00:07.500 WARN [camera] camera control view read refused: the prologue guard refused: byte "
    "+3 differs, expected 0x48; refusals 12; pose-owner: replay, conflicts 0, arb none, "
    "rel-refused 0";
// ... and it can carry the word the COUNT is keyed on. The count comes off the `; refusals `
// delimiter's own tail, so the 99 in the sentence must not answer for the module's 4.
const char* kViewRefusedInnerWord =
    "12:00:07.500 WARN [camera] camera control view read refused: the guard counts refusals 99 at "
    "this site; refusals 4; pose-owner: replay, conflicts 0, arb none, rel-refused 0";
// The near miss: a [camera] line that merely CONTAINS the prefix. Nothing here refused a view read.
const char* kViewRefusedMentioned =
    "12:00:07.400 INFO [camera] camera control bound: rig 00000AA0 on level space 000001F0 (netGame "
    "000002A0, systems at 7ff6); rig enabled flag 1; pose-owner: replay, conflicts 0; camera "
    "control view read refused: nothing so far; refusals 0";

}  // namespace

TEST_CASE("TextBetween and UnsignedLongLongAfter answer only on the shape they are given") {
    char out[64] = {0};
    CHECK(TextBetween("alpha: one two; beta 4", "alpha: ", "; beta ", out, sizeof out));
    CHECK(strcmp(out, "one two") == 0);

    // The key is anchored the way FindToken anchors it: buried in a word, it opens nothing -- and
    // the buffer is CLEARED rather than left holding what an earlier line put there. This test used
    // to assert the opposite (UNTOUCHED), which is the behaviour AUDIT_L5FIX filed against W2-R: a
    // second malformed line left the first one's sentence standing as the current answer.
    strcpy_s(out, sizeof out, "STALE");
    CHECK_FALSE(TextBetween("xalpha: one; beta 4", "alpha: ", "; beta ", out, sizeof out));
    CHECK(out[0] == '\0');

    // Either half missing writes nothing at all, rather than the remainder of the wrong line.
    strcpy_s(out, sizeof out, "STALE");
    CHECK_FALSE(TextBetween("alpha: one two", "alpha: ", "; beta ", out, sizeof out));
    CHECK(out[0] == '\0');
    strcpy_s(out, sizeof out, "STALE");
    CHECK_FALSE(TextBetween("beta 4; alpha: one", "alpha: ", "; beta ", out, sizeof out));
    CHECK(out[0] == '\0');

    // The FIRST end after the key, and a span that carries the end token's own opening character.
    CHECK(TextBetween("alpha: a; b; beta 4", "alpha: ", "; beta ", out, sizeof out));
    CHECK(strcmp(out, "a; b") == 0);

    char tiny[6] = {0};
    CHECK(TextBetween("alpha: abcdefgh; beta 4", "alpha: ", "; beta ", tiny, sizeof tiny));
    CHECK(strcmp(tiny, "abcde") == 0);            // bounded, and NUL-terminated inside the bound

    unsigned long long v = 7;
    CHECK(UnsignedLongLongAfter("count 42 here", "count ", &v));
    CHECK(v == 42);
    // The reason this variant exists: an `unsigned` reader wraps a real 64-bit counter into a
    // smaller, plausible number, and an undercount reads as a healthier session than happened.
    CHECK(UnsignedLongLongAfter("count 4294967300 here", "count ", &v));
    CHECK(v == 4294967300ULL);
    CHECK_FALSE(UnsignedLongLongAfter("count here", "count ", &v));
    CHECK(v == 4294967300ULL);
    CHECK_FALSE(UnsignedLongLongAfter("xcount 9", "count ", &v));
}

TEST_CASE("the view-read refusal line is read whole, and only when the line IS one") {
    LogFacts f;
    f.OnLine(kViewRefused);
    CHECK(f.cameraViewRefusedSeen);
    CHECK(f.cameraViewRefusals == 3);
    CHECK(strcmp(f.cameraViewRefusal,
                 "the view-state array slot at 0x14A2B3C4 is reserved, not committed memory") == 0);
    CHECK(strcmp(f.cameraViewRefusalPoseOwner, "replay") == 0);
    CHECK(strstr(f.cameraViewRefusalLine, "rel-refused 0") != nullptr);
    // It is its own arm: the bind row's fields are untouched by it.
    CHECK_FALSE(f.cameraBoundSeen);
    CHECK(f.cameraPoseOwner[0] == '\0');

    // A sentence carrying `refused:` is still ONE refusal, and the whole sentence survives.
    LogFacts inner;
    inner.OnLine(kViewRefusedInnerColon);
    CHECK(inner.cameraViewRefusals == 12);
    CHECK(strcmp(inner.cameraViewRefusal,
                 "the prologue guard refused: byte +3 differs, expected 0x48") == 0);

    // A sentence carrying `refusals ` does not answer for the count.
    LogFacts word;
    word.OnLine(kViewRefusedInnerWord);
    CHECK(word.cameraViewRefusals == 4);
    CHECK(strcmp(word.cameraViewRefusal, "the guard counts refusals 99 at this site") == 0);

    // THE NEAR MISS. A bind line that mentions the prefix is not a refusal line, and must not
    // become one -- nor take the bind arm's fields with it.
    LogFacts mention;
    mention.OnLine(kViewRefusedMentioned);
    CHECK_FALSE(mention.cameraViewRefusedSeen);
    CHECK(mention.cameraViewRefusals == 0);
    CHECK(mention.cameraViewRefusal[0] == '\0');
    CHECK(mention.cameraBoundSeen);
    CHECK(strcmp(mention.cameraPoseOwner, "replay") == 0);

    // The other two [camera] arms still land where they did.
    LogFacts bound;
    bound.OnLine(kCameraGood);
    CHECK(bound.cameraBoundSeen);
    CHECK_FALSE(bound.cameraViewRefusedSeen);
    LogFacts refusedBind;
    refusedBind.OnLine(kCameraRefused);
    CHECK(refusedBind.cameraRefusedSeen);
    CHECK_FALSE(refusedBind.cameraViewRefusedSeen);
}

TEST_CASE("the refusal row answers three ways, and absence is never a pass") {
    StatusFacts s;

    LogFacts silent;
    CHECK(Ask1("w2e-view-read-refusal-names-its-site", silent, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("unexercised, not proven"));

    LogFacts named;
    named.OnLine(kViewRefused);
    CHECK(Ask1("w2e-view-read-refusal-names-its-site", named, s) == Ask::Pass);
    CHECK(DetailMentions("view-state array slot at 0x14A2B3C4"));
    CHECK(DetailMentions("refusals 3"));

    // The VR2-2 defect back: the operator gets the fact and not the site. The producer writes the
    // sentence with %s, so an empty latch is exactly the line this fixture carries.
    LogFacts causeless;
    causeless.OnLine(
        "12:00:07.500 WARN [camera] camera control view read refused: ; refusals 1; pose-owner: "
        "replay, conflicts 0, arb none, rel-refused 0");
    CHECK(causeless.cameraViewRefusedSeen);
    CHECK(causeless.cameraViewRefusal[0] == '\0');
    CHECK(Ask1("w2e-view-read-refusal-names-its-site", causeless, s) == Ask::Fail);
    CHECK(DetailMentions("the operator has the fact and not the site"));
}

TEST_CASE("4b's refusal detail names the site, not just the word") {
    StatusFacts s;
    s.transportPresent = true;
    s.durationUs = 1ull * 1000000;   // a 1 s span; this case scores the refusal detail
    s.pausedForView = true;
    s.viewPoseSampled = true;
    s.viewReadFirstCode = replay::camera::ViewPoseRead::NoViewState;
    s.viewReadSecondCode = replay::camera::ViewPoseRead::NoViewState;
    strcpy_s(s.viewReadFirst, "no-view-state");
    strcpy_s(s.viewReadSecond, "no-view-state");

    LogFacts none;
    CHECK(Ask1("w2e-4b-view-moves-while-paused", none, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("the module logged no sentence for it"));

    LogFacts named;
    named.OnLine(kViewRefused);
    CHECK(Ask1("w2e-4b-view-moves-while-paused", named, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("the view-state array slot at 0x14A2B3C4"));
}

// ---------------------------------------------------------------------------------------------
// AUDIT_F3H F3H-7 -- W1-E emits, W2-R parses, and they now read ONE file.
//
// ARCH-12 S4.3 froze this grammar as a pair: "W1-E emits, W2-R parses, neither moves alone."
// Nothing enforced it. The auditor proved the gap by mutating the emitter's frozen `+0x2B10=`
// label to `+0x2B11=`: entity_wiretest reddened and THIS suite stayed 52 | 52 green, against an
// emitter that no longer produced the column this row exists to read. In a launch that drift
// renders as could-not-ask "carries NO +0x2B10 column", which is indistinguishable from a
// legitimate menu-only launch -- the freeze failing silently, in the direction that reads normal.
//
// tests/entity/golden/cs-space-candidates.txt closes it. W1-E's own case WRITES and COMPARES that
// file from the shipping emitter's bytes; this case reads it READ-ONLY and feeds every arm to
// EvaluateRow. Neither side holds a copy of the other's spelling, so a one-character drift on
// either side now reddens one of the two suites.
//
// MISSING IS A REFUSAL. A check that disappears together with its input is not a check, so an
// absent, empty or malformed golden FAILS this case by name -- never a skip, never a pass.
// ---------------------------------------------------------------------------------------------
#ifndef REPLAY_REPO_ROOT
#error "REPLAY_REPO_ROOT is required: the ARCH-12 4.3 golden lives in the source tree, and without \
it this case would have no file to refuse the absence of."
#endif

struct GoldenArm {
    std::string name;
    std::string payload;
};

// The golden's contract, quoted from its own file head: one arm per line, `<arm-name> <TAB>
// <emitted line>`; LF line endings; ASCII only; no trailing spaces; a `#` in column 1 is a
// comment; no blank lines. Every departure is REPORTED rather than skipped -- a reader that drops
// the lines it does not understand turns a corrupted fixture into a green run, and these payloads
// are the only evidence this case has. Returns the empty string on success, the refusal otherwise.
std::string ReadGoldenArms(const char* path, std::vector<GoldenArm>* out) {
    FILE* fh = nullptr;
    if (fopen_s(&fh, path, "rb") != 0 || fh == nullptr) {
        return "the file is absent or could not be opened";
    }
    std::string text;
    char buf[4096];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof buf, fh)) > 0) {
        text.append(buf, got);
    }
    fclose(fh);
    if (text.empty()) {
        return "the file is empty";
    }

    size_t at = 0;
    int lineNo = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            return "the last line carries no LF terminator";
        }
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        ++lineNo;
        const std::string where = " (line " + std::to_string(lineNo) + ")";

        // A CR, a NUL or any non-ASCII byte lands here: the contract says LF and ASCII, and a
        // fixture read through a translating reader would compare bytes the emitter never wrote.
        for (size_t i = 0; i < line.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (c != '\t' && (c < 0x20 || c > 0x7E)) {
                return "a byte outside printable ASCII" + where;
            }
        }
        if (line.empty()) {
            return "a blank line" + where;
        }
        if (line[line.size() - 1] == ' ') {
            return "a trailing space" + where;
        }
        if (line[0] == '#') {
            continue;
        }
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
            return "a data line that is not `<arm-name> <TAB> <emitted line>`" + where;
        }
        GoldenArm arm;
        arm.name = line.substr(0, tab);
        arm.payload = line.substr(tab + 1);
        if (arm.payload.find('\t') != std::string::npos) {
            return "a second TAB inside the payload" + where;
        }
        out->push_back(arm);
    }
    if (out->empty()) {
        return "the file carries comments and no arms at all";
    }
    return std::string();
}

// The golden holds the EMITTER's payload; the logger's framing -- wall stamp, level word and the
// `[entity]` module tag -- is not part of the frozen grammar, and the golden's head says so. The
// reader puts it back rather than the fixture carrying bytes the emitter did not write. The stamp
// value decides nothing in this row.
const char* const kGoldenLogPrefix = "04:16:39.100 INFO [entity] ";

TEST_CASE("cs-space-candidates: every arm of W1-E's committed golden, scored by this row") {
    static const char kGoldenPath[] =
        REPLAY_REPO_ROOT "/tests/entity/golden/cs-space-candidates.txt";

    std::vector<GoldenArm> arms;
    const std::string why = ReadGoldenArms(kGoldenPath, &arms);
    INFO("golden file: " << kGoldenPath);
    REQUIRE_MESSAGE(why.empty(),
                    "REFUSED -- the ARCH-12 4.3 golden could not be read, so this row is unproved "
                    "against the bytes W1-E emits: " << why);

    // The seven arms and what this row must say about each. The two `+0x2B10=null` arms SPLIT on
    // whether a level was ever reached -- ARCH-13 S6.2's ratified amendment to ARCH-12 4.3 rule 3:
    // never seen -> could-not-ask (no map loaded, so the slot was never asked to hold a space);
    // seen and the slot still null -> asked-FAIL (a level loaded and the spectator-camera space
    // did not appear). Both sides are scored here, so a reader that collapsed the split back to
    // one answer reds on one of them whichever way it collapsed.
    struct Expect {
        const char* arm;
        Ask noLevel;
        Ask levelSeen;
        bool levelSpaceNull;
        const char* why;
    };
    static const Expect kExpect[] = {
        {"ready-2b08-only", Ask::CouldNotAsk, Ask::Fail, true,
         "+0x2B08 Ready and the LEVEL space null -- rule 3's split, and F3H-1's own input"},
        {"null-cr15game", Ask::CouldNotAsk, Ask::Fail, true,
         "+0x8 null, so the canary is quiet and the level space is still absent"},
        {"unreadable-netgame", Ask::Fail, Ask::Fail, false,
         "+0x2B10 rendered unreadable -- rule 3 unconditionally, level or no level"},
        {"both-spaces-symbols-agree", Ask::Pass, Ask::Pass, false,
         "+0x2B10 Ready is this row's subject (4.3 rule 2)"},
        {"symbols-disagree", Ask::Pass, Ask::Pass, false,
         "the two symbols are information: 4.3 rule 6 rejected them as a gate"},
        {"zero-level-symbol", Ask::Pass, Ask::Pass, false,
         "a zero levelsym is a rendered identity, not an absent one"},
        {"level-space-after-bind", Ask::Pass, Ask::Pass, false,
         "W1-E R3's post-bind line: +0x2B10 rendered Ready and distinct, which is what the "
         "sampler stopping at 21 s used to hide (AUDIT_L5_RUN_4 F11)"},
        {"level-aliased-to-2b08", Ask::Fail, Ask::Fail, false,
         "+0x2B10 and +0x2B08 rendered ONE pointer -- 4.3 rule 5"},
    };
    const int kExpectCount = static_cast<int>(sizeof kExpect / sizeof kExpect[0]);

    // Counted both ways below, so neither side can go quiet: an arm the emitter DROPS loses its
    // scoring row, and an arm the emitter GROWS arrives unscored -- and an unscored arm is exactly
    // the silent drift F3H-7 is about.
    CHECK(static_cast<int>(arms.size()) == kExpectCount);

    StatusFacts s;
    for (int i = 0; i < kExpectCount; ++i) {
        const Expect& e = kExpect[i];
        const GoldenArm* got = nullptr;
        for (size_t j = 0; j < arms.size(); ++j) {
            if (arms[j].name == kExpect[i].arm) {
                got = &arms[j];
                break;
            }
        }
        const std::string armName(e.arm);
        REQUIRE_MESSAGE(got != nullptr, "the golden carries no arm named " << armName);

        // The discriminator is read off the FILE, not asserted from this table's belief about it:
        // if the emitter stops rendering `null` in the fifth column of these two arms, the split
        // they are here to exercise is no longer being exercised and this reds instead of quietly
        // scoring a different input.
        const bool bytesSayNull = got->payload.find(" | +0x2B10=null | ") != std::string::npos;
        CHECK_MESSAGE(bytesSayNull == e.levelSpaceNull,
                      armName << ": the golden's fifth column no longer matches this table");

        const std::string full = std::string(kGoldenLogPrefix) + got->payload;

        LogFacts cold;
        cold.OnLine(full.c_str());
        REQUIRE_MESSAGE(cold.csSpaceSeen,
                        "the row did not recognise the emitted line at all: " << armName);
        REQUIRE_MESSAGE(cold.csSpaceAt2B10KeyPresent,
                        "the emitted line carries no +0x2B10 key: " << armName);
        CHECK_MESSAGE(Ask1("cs-space-candidates", cold, s) == e.noLevel,
                      armName << " (no level loaded): " << std::string(e.why));
        if (e.levelSpaceNull) {
            // AUDIT_F3H F3H-1's control, on the emitter's OWN bytes rather than a rebuilt line.
            // The "ok" sentence was guarded on +0x2B08 alone, so this exact arm printed "+0x2B08
            // <ptr> and +0x2B10  are two distinct non-null spaces" -- a pass word with an empty
            // pointer in the middle of it, contradicted by the same line's fifth column.
            CHECK_FALSE(DetailMentions("Differential ok"));
            CHECK_FALSE(DetailMentions("two distinct non-null spaces"));
            CHECK(DetailMentions("Differential could-not-ask: +0x2B10 rendered null"));
        }

        LogFacts hot;
        hot.OnLine(kStateLoadingLevel);
        hot.OnLine(full.c_str());
        CHECK_MESSAGE(Ask1("cs-space-candidates", hot, s) == e.levelSeen,
                      armName << " (a level loaded): " << std::string(e.why));
        if (e.levelSpaceNull) {
            // The asked-FAIL side of the split, where the fabricated sentence was what an operator
            // read to diagnose the failure.
            CHECK_FALSE(DetailMentions("Differential ok"));
            CHECK_FALSE(DetailMentions("two distinct non-null spaces"));
            CHECK(DetailMentions("Differential could-not-ask: +0x2B10 rendered null"));
        }
    }

    for (size_t j = 0; j < arms.size(); ++j) {
        bool scored = false;
        for (int i = 0; i < kExpectCount; ++i) {
            if (arms[j].name == kExpect[i].arm) {
                scored = true;
                break;
            }
        }
        CHECK_MESSAGE(scored,
                      "the golden carries an arm this case does not score: " << arms[j].name);
    }
}

// ============================================================================================
// The coupled rename, SECOND HALF. W2-E landed 162027c, so
// camera_module.cpp:96,107,125 write `level space` and no build of this product can emit
// `on game space` on a camera control line any more. P9F's both-spellings tolerance is gone
// with it: the old noun is now rejected, and the fixtures above carry the new one.
//
// Rejecting it silently would be the worse bug of the two -- a pre-162027c log would read as a
// launch where the camera module never bound anything. So the two negative controls below check
// BOTH halves: the line does not set the seen flags, AND each row answers could-not-ask quoting
// the line it could not parse.
// ============================================================================================
namespace {

// kCameraGood and kCameraRefused with the ONE token turned back, and nothing else touched.
const char* kCameraBoundOldNoun =
    "12:00:07.000 INFO [camera] camera control bound: rig 00000AA0 on game space 000001F0 (netGame "
    "000002A0, systems at 7ff6); rig enabled flag 1; pose-owner: replay, conflicts 0; modes drive "
    "from the panel and `cam`";
const char* kCameraRefusedOldNoun =
    "12:00:07.000 WARN [camera] camera control refused rig 0 on game space 000001F0 (netGame "
    "000002A0): spectator CS disagrees; pose-owner: none, conflicts 2";
const char* kCameraReleasedGameSpelling =
    "12:00:08.000 INFO [camera] camera control released (netGame 000002A0, game space 000001F0): "
    "the level went away";
const char* kCameraReleasedLevelSpelling =
    "12:00:08.000 INFO [camera] camera control released (netGame 000002A0, level space 000001F0): "
    "the level went away";

}  // namespace

TEST_CASE("the pre-rename space noun is not matched, and both camera rows say so") {
    StatusFacts none;

    // The control on the control: the CURRENT spelling parses and scores, so the negatives below
    // are not passing on a reader that stopped reading camera lines altogether.
    LogFacts current;
    current.OnLine(kCameraGood);
    CHECK(current.cameraBoundSeen);
    CHECK_FALSE(current.cameraStaleNounSeen);
    CHECK(current.cameraRigEnabled == 1u);
    CHECK(strcmp(current.cameraPoseOwner, "replay") == 0);
    CHECK(Ask1("w2e-camera-control-bound", current, none) == Ask::Pass);

    // The fixtures differ in exactly one token, and each really carries the noun it claims --
    // otherwise the rejection below could be rejecting something else entirely.
    CHECK(strstr(kCameraGood, " on level space ") != nullptr);
    CHECK(strstr(kCameraGood, " on game space ") == nullptr);
    CHECK(strstr(kCameraBoundOldNoun, " on game space ") != nullptr);
    CHECK(strstr(kCameraBoundOldNoun, " on level space ") == nullptr);
    {
        std::string turned(kCameraBoundOldNoun);
        turned.replace(turned.find(" on game space "), strlen(" on game space "),
                       " on level space ");
        CHECK(turned == std::string(kCameraGood));
    }

    // THE DROP. The old spelling sets neither seen flag ...
    LogFacts staleBound;
    staleBound.OnLine(kCameraBoundOldNoun);
    CHECK_FALSE(staleBound.cameraBoundSeen);
    CHECK_FALSE(staleBound.cameraRefusedSeen);
    CHECK(staleBound.cameraRigEnabled == 0u);
    CHECK(staleBound.cameraPoseOwner[0] == '\0');
    // ... and is not dropped on the floor either: the row refuses BY NAME and quotes the line,
    // instead of reporting a session in which the module never reached a rig probe.
    CHECK(staleBound.cameraStaleNounSeen);
    CHECK(strstr(kCameraBoundOldNoun, staleBound.cameraStaleNounLine) != nullptr);
    CHECK(strstr(staleBound.cameraStaleNounLine, "camera control bound: rig 00000AA0 on game "
                 "space 000001F0") == staleBound.cameraStaleNounLine);
    CHECK(Ask1("w2e-camera-control-bound", staleBound, none) == Ask::CouldNotAsk);
    CHECK(DetailMentions("pre-rename spelling"));
    CHECK(DetailMentions("on game space 000001F0"));
    CHECK_FALSE(DetailMentions("never reached a rig probe"));

    // Same for the refusal line, and the refusal COUNT must not tick for a line not parsed --
    // a stale line counted as a refusal would fail the launch for the wrong reason.
    LogFacts staleRefusal;
    staleRefusal.OnLine(kStateInGame);
    staleRefusal.OnLine(kCameraRefusedOldNoun);
    CHECK_FALSE(staleRefusal.cameraRefusedSeen);
    CHECK(staleRefusal.cameraRefusalCount == 0u);
    CHECK(staleRefusal.cameraRefusalWhy[0] == '\0');
    CHECK(staleRefusal.cameraStaleNounSeen);
    StatusFacts probed;
    probed.cameraProbed = true;
    strcpy_s(probed.cameraProbeResult, "NoRigSlot");
    CHECK(Ask1("w2e-camera-rig-probe", staleRefusal, probed) == Ask::CouldNotAsk);
    CHECK(DetailMentions("pre-rename spelling"));
    CHECK_FALSE(DetailMentions("no [camera] refusal line named a cause either"));

    // The current refusal spelling still parses every field out of its own token.
    LogFacts liveRefusal;
    liveRefusal.OnLine(kCameraRefused);
    CHECK(liveRefusal.cameraRefusedSeen);
    CHECK(liveRefusal.cameraRefusalCount == 1u);
    CHECK(strcmp(liveRefusal.cameraRefusalWhy, "spectator CS disagrees") == 0);
    CHECK(liveRefusal.cameraConflicts == 2u);

    // The release line is read by nothing, in either spelling, and must not be mistaken for one of
    // the two lines that ARE read -- nor for a stale control line, which it is not.
    for (const char* released : {kCameraReleasedGameSpelling, kCameraReleasedLevelSpelling}) {
        LogFacts f;
        f.OnLine(released);
        INFO("release: ", released);
        CHECK_FALSE(f.cameraBoundSeen);
        CHECK_FALSE(f.cameraRefusedSeen);
        CHECK_FALSE(f.cameraViewRefusedSeen);
        CHECK_FALSE(f.cameraStaleNounSeen);
    }

    // NEAR MISSES. Tolerating both nouns must not become tolerating anything: these differ from a
    // real line before the noun, and are not this row's evidence.
    const char* nearMisses[] = {
        "12:00:07.000 INFO [camera] camera control bound rig 00000AA0 on level space 000001F0; "
        "rig enabled flag 1; pose-owner: replay, conflicts 0",
        "12:00:07.000 INFO [camera] camera control bounded: rig 00000AA0 on level space 000001F0; "
        "rig enabled flag 1; pose-owner: replay, conflicts 0",
        "12:00:07.000 INFO [camera] deferred: camera control bound: rig 00000AA0 on level space "
        "000001F0; rig enabled flag 1; pose-owner: replay, conflicts 0",
        "12:00:07.000 WARN [camera] camera control refusedx rig 0 on level space 000001F0 "
        "(netGame 000002A0): spectator CS disagrees; pose-owner: none, conflicts 2",
        "12:00:07.000 WARN [session] camera control refused rig 0 on level space 000001F0 "
        "(netGame 000002A0): spectator CS disagrees; pose-owner: none, conflicts 2",
    };
    for (const char* miss : nearMisses) {
        LogFacts f;
        f.OnLine(miss);
        INFO("near miss: ", miss);
        CHECK_FALSE(f.cameraBoundSeen);
        CHECK_FALSE(f.cameraRefusedSeen);
        // These all carry the CURRENT noun, so a near miss is a line this reader does not
        // recognise at all -- not a pre-rename line, and it must not be reported as one.
        CHECK_FALSE(f.cameraStaleNounSeen);
    }
}

// ============================================================================================
// AUD-L5 section 12 item 8 -- THE DESTINATION SET. L5 #3's record carried "16/16 hooks, 60
// refused" and not one host, so "the blocker did its job" was not falsifiable. These drive the
// record writer the way probe_replay.cpp calls it, with the bodies egress.h's accessors return.
// ============================================================================================
namespace {

EgressFacts ArmedWith(const char* refusedBody, const char* allowedBody, const char* why) {
    EgressFacts f;
    f.state = "armed";
    f.armedBy = "sweep-override";
    f.hooksJson = "\"connect\",\"WSAConnect\"";
    f.armedAtMs = 78;
    f.refused = 60;
    f.refusedDestinations = refusedBody;
    f.allowedDestinations = allowedBody;
    f.redactionRefusal = why;
    return f;
}

// The L5 #3 peer set, ENG-EGRESS-HOSTS section 1: the whole observable set of that launch
// (127.0.0.1:80 x204, 127.0.0.1:6719 x2, 100.64.100.100:6894 x4), rendered by egress.h's rule.
// 127/8 is loopback and 100.64/10 is CGNAT, so all three go through VERBATIM -- the retroactive
// claim is that this set renders with NO redaction token anywhere in the record.
const char* kL5PeerBody =
    "{\"dest\": \"127.0.0.1:80\", \"via\": \"connect\", \"count\": 204}, "
    "{\"dest\": \"127.0.0.1:6719\", \"via\": \"connect\", \"count\": 2}, "
    "{\"dest\": \"100.64.100.100:6894\", \"via\": \"WSASendTo\", \"count\": 4}";

unsigned CountDestinations(const char* body) {
    unsigned n = 0;
    std::vector<char> dest(strlen(body) + 1, '\0');
    for (const char* p = body; (p = NextDestination(p, &dest[0], dest.size())) != nullptr;) {
        ++n;
    }
    return n;
}

// Both buffers are sized from what fills them: the detail from the battery's own detail buffer, the
// record from the bodies it is about to carry. That is the same rule the writer follows, and it is
// why nothing here has a bound chosen against a sample.
std::vector<char> DetailBuffer() { return std::vector<char>(sizeof g_detail, '\0'); }

std::string EgressRecordOf(const EgressFacts& f) {
    size_t need = 4096;
    if (f.refusedDestinations != nullptr) {
        need += strlen(f.refusedDestinations);
    }
    if (f.allowedDestinations != nullptr) {
        need += strlen(f.allowedDestinations);
    }
    if (f.redactionRefusal != nullptr) {
        need += strlen(f.redactionRefusal);
    }
    std::vector<char> buffer(need, '\0');
    REQUIRE(AppendEgressObject(&buffer[0], buffer.size(), f));
    return std::string(&buffer[0]);
}

}  // namespace

TEST_CASE("the record carries both destination arrays, and the row reads the ALLOWED set") {
    const EgressFacts f = ArmedWith("", kL5PeerBody, nullptr);
    const std::string rec = EgressRecordOf(f);

    CHECK(rec.find("\"refusedDestinations\": []") != std::string::npos);
    CHECK(rec.find("\"allowedDestinations\": [{\"dest\": \"127.0.0.1:80\"") != std::string::npos);
    CHECK(rec.find("100.64.100.100:6894") != std::string::npos);
    // THE RETROACTIVE VERDICT, and it FLIPPED (AUDIT_F3J F3J-3). This is the set L5 #3 actually
    // recorded, and `100.64.100.100` is CGNAT -- a Tailscale mesh peer, a DIFFERENT MACHINE. The
    // row that had one `local` bucket filed it asked-pass under the sentence "on this machine".
    CHECK(rec.find("\"destinationsVerdict\": \"asked-fail\"") != std::string::npos);
    CHECK(rec.find("\"destinationsVerdict\": \"asked-pass\"") == std::string::npos);
    // RETROACTIVE, on the auditor's own input: no token, because nothing in that set is redactable.
    CHECK(rec.find("redacted-") == std::string::npos);

    size_t stoppedAt = 0;
    const std::string doc = RecordAround(rec.c_str());
    INFO("record: ", doc);
    CHECK(JsonParses(doc.c_str(), &stoppedAt));

    std::vector<char> detail = DetailBuffer();
    CHECK(AskEgressDestinations(f, &detail[0], detail.size()) == Ask::Fail);
    CHECK(strstr(&detail[0], "`100.64.100.100:6894` is a `cgnat` destination") != nullptr);
    // The row LISTS them: a verdict with no set is the L5 defect one level down.
    CHECK(strstr(&detail[0], "127.0.0.1:80") != nullptr);
    CHECK(strstr(&detail[0], "127.0.0.1:6719") != nullptr);
    CHECK(strstr(&detail[0], "100.64.100.100:6894") != nullptr);
    CHECK(strstr(&detail[0], "3 allowed") != nullptr);

    // An empty ALLOWED set is a real answer and says so as one, rather than passing on the same
    // sentence a full-and-clean set gets.
    const EgressFacts nothingAllowed = ArmedWith(kL5PeerBody, "", nullptr);
    CHECK(AskEgressDestinations(nothingAllowed, &detail[0], detail.size()) == Ask::Pass);
    CHECK(strstr(&detail[0], "NO destination was allowed at all") != nullptr);
    CHECK(strstr(&detail[0], "3 refused") != nullptr);
}

TEST_CASE("a redacted destination in the ALLOWED array is asked-FAIL, not a footnote") {
    const char* leaked =
        "{\"dest\": \"127.0.0.1:6719\", \"via\": \"connect\", \"count\": 2}, "
        "{\"dest\": \"redacted-1a2b3c4d.net:443\", \"via\": \"WSAConnect\", \"count\": 9}";
    const EgressFacts f = ArmedWith("", leaked, nullptr);
    std::vector<char> detail = DetailBuffer();
    CHECK(AskEgressDestinations(f, &detail[0], detail.size()) == Ask::Fail);
    CHECK(strstr(&detail[0], "redacted-1a2b3c4d.net:443") != nullptr);
    CHECK(strstr(&detail[0], "is a `off-machine` destination, NOT loopback") != nullptr);

    const std::string rec = EgressRecordOf(f);
    CHECK(rec.find("\"destinationsVerdict\": \"asked-fail\"") != std::string::npos);
    CHECK(rec.find("\"destinationsVerdict\": \"asked-pass\"") == std::string::npos);
    size_t stoppedAt = 0;
    CHECK(JsonParses(RecordAround(rec.c_str()).c_str(), &stoppedAt));
}

TEST_CASE("a refused redactor is named in the record, never a silent absence") {
    const char* why = "no per-build salt: RedactionSalt() is empty, so tokens would not be stable";
    // Fail-closed, all three ways the module can hand this over.
    const EgressFacts noSet = ArmedWith(nullptr, nullptr, why);
    const EgressFacts halfSet = ArmedWith("", nullptr, why);
    const EgressFacts reasonOnly = ArmedWith("", kL5PeerBody, why);
    std::vector<char> detail = DetailBuffer();
    for (const EgressFacts& f : {noSet, halfSet, reasonOnly}) {
        const std::string rec = EgressRecordOf(f);
        INFO("record: ", rec);
        CHECK(rec.find("\"destinationsRefused\": \"") != std::string::npos);
        CHECK(rec.find(why) != std::string::npos);
        CHECK(rec.find("\"destinationsVerdict\": \"could-not-ask\"") != std::string::npos);
        // The counts alone, with no set and no reason, IS the L5 record. Neither array may be filed
        // on this branch, and the object must still say what it could not do.
        CHECK(rec.find("\"refusedDestinations\"") == std::string::npos);
        CHECK(rec.find("\"allowedDestinations\"") == std::string::npos);
        CHECK(rec.find("\"refused\": 60") != std::string::npos);
        size_t stoppedAt = 0;
        CHECK(JsonParses(RecordAround(rec.c_str()).c_str(), &stoppedAt));

        CHECK(AskEgressDestinations(f, &detail[0], detail.size()) == Ask::CouldNotAsk);
        CHECK(strstr(&detail[0], "NOT-PROTECTING-ATTESTED") != nullptr);
        CHECK(strstr(&detail[0], why) != nullptr);
    }

    // A refusal with no reason still refuses, and still says that much.
    const EgressFacts mute = ArmedWith(nullptr, nullptr, nullptr);
    CHECK(AskEgressDestinations(mute, &detail[0], detail.size()) == Ask::CouldNotAsk);
    CHECK(strstr(&detail[0], "gave no reason") != nullptr);
    CHECK(EgressRecordOf(mute).find("\"destinationsRefused\": \"the redactor returned no "
                                    "destination set") != std::string::npos);
}

TEST_CASE("the raw public host is in the input set and nowhere in the record") {
    // THE LEAK CONTROL, and it is written to be non-vacuous: the haystack it clears really does
    // contain the host. `_local/config.json` holds the owner's real endpoints and the compiled-in
    // fallbacks are live production hosts, so a record that names one turns a safety artefact into
    // the leak it exists to prevent. Redaction itself is src/bootstrap/egress.h's (and is tested in
    // tests/bootstrap/, which is where the salt lives); what is asserted HERE is the writer's half
    // -- it carries the rendered set and never reconstructs a name from it.
    const char* kRawHost = "login.echovr-prod.example.net";
    const char* kRecordedPeers[] = {
        "127.0.0.1:80",
        "login.echovr-prod.example.net:443",
        "100.64.100.100:6894",
    };
    std::string inputHaystack;
    for (const char* peer : kRecordedPeers) {
        inputHaystack += peer;
        inputHaystack += " ";
    }
    // NON-VACUITY, asserted before any absence is claimed: the host IS in the input.
    REQUIRE(inputHaystack.find(kRawHost) != std::string::npos);

    // What egress.h's rule renders for that input: the two literals verbatim, the DNS name as a
    // token that keeps only its TLD and its port.
    const char* renderedBody =
        "{\"dest\": \"127.0.0.1:80\", \"via\": \"connect\", \"count\": 204}, "
        "{\"dest\": \"redacted-9f3c17ab.net:443\", \"via\": \"connect\", \"count\": 11}, "
        "{\"dest\": \"100.64.100.100:6894\", \"via\": \"WSASendTo\", \"count\": 4}";
    const EgressFacts f = ArmedWith(renderedBody, "", nullptr);
    const std::string rec = EgressRecordOf(f);

    CHECK(rec.find(kRawHost) == std::string::npos);
    CHECK(rec.find("echovr-prod") == std::string::npos);
    // Redacted, not DROPPED -- a deletion would clear the grep too, and would lose the finding.
    CHECK(rec.find("redacted-9f3c17ab.net:443") != std::string::npos);
    CHECK(CountDestinations(renderedBody) ==
          static_cast<unsigned>(sizeof kRecordedPeers / sizeof kRecordedPeers[0]));
    std::vector<char> detail = DetailBuffer();
    CHECK(AskEgressDestinations(f, &detail[0], detail.size()) == Ask::Pass);
    CHECK(strstr(&detail[0], "3 refused") != nullptr);
    CHECK(strstr(&detail[0], kRawHost) == nullptr);
}

TEST_CASE("the local-destination rule is egress.h's families, at both edges of each") {
    struct Case { const char* dest; const char* family; };
    const Case cases[] = {
        {"127.0.0.1:80", "loopback"},   {"127.255.255.254", "loopback"},
        {"126.0.0.1", "off-machine"},   {"128.0.0.1", "off-machine"},
        {"10.0.0.1:6721", "lan"},       {"11.0.0.1", "off-machine"},
        {"172.15.0.1", "off-machine"},  {"172.16.0.1", "lan"},
        {"172.31.255.1", "lan"},        {"172.32.0.1", "off-machine"},
        {"192.168.1.1", "lan"},         {"192.169.1.1", "off-machine"},
        {"100.63.0.1", "off-machine"},  {"100.64.0.1", "cgnat"},
        {"100.64.100.100:6894", "cgnat"},
        {"100.64.100.100:41641", "cgnat"},
        {"100.127.255.1", "cgnat"},     {"100.128.0.1", "off-machine"},
        {"169.254.1.1", "link-local"},
        {"0.0.0.0:0", "unspecified"},   {"0.0.0.1", "off-machine"},
        {"::1", "loopback"},            {"[::1]:6719", "loopback"},
        {"::", "unspecified"},          {"fe80::1%12", "link-local"},
        {"fd00::1", "ula"},             {"fc00::1:443", "ula"},
        {"localhost:80", "loopback"},   {"localhost", "loopback"},
        {"localhosting.example.com:443", "off-machine"},
        {"fdcdn.example.com:443", "off-machine"},
        {"redacted-1a2b3c4d.net:443", "off-machine"},
        {"redacted-1a2b3c4d:6894", "off-machine"},
        {"1270.0.1.1", "off-machine"},  {"12.7.0.1", "off-machine"},
        {"10.0.0.1x", "off-machine"},   {"", "off-machine"},
        {nullptr, "off-machine"},
    };
    for (const Case& c : cases) {
        INFO("dest: ", std::string(c.dest != nullptr ? c.dest : "(null)"));
        CHECK(std::string(DestinationFamily(c.dest)) == std::string(c.family));
        // Only loopback and the unspecified address earn "on this machine" (AUDIT_F3J F3J-3);
        // everything egress.h renders verbatim is a superset of that and the two must not be
        // confused again.
        const bool onMachine = std::string(c.family) == "loopback" ||
                               std::string(c.family) == "unspecified";
        CHECK(DestinationOnThisMachine(c.family) == onMachine);
        CHECK(DestinationRenderedVerbatim(c.family) == (std::string(c.family) != "off-machine"));
    }
}

// AUDIT_F3J F3J-3 and AUDIT_L5_RUN_4 F14, RETROACTIVE: the address L5 #3 actually recorded as an
// inbound peer on this machine. `100.64.100.100:41641` is a Tailscale mesh address on its default
// WireGuard port -- a DIFFERENT MACHINE over the public internet or a third-party relay -- and the
// row that had one `local` bucket filed it as an asked-PASS reading "on this machine".
// One buffer, one call: AskEgressDestinations takes its own detail rather than the shared row
// buffer DetailMentions reads, so the egress cases carry theirs explicitly.
static Ask AskAllowed(EgressFacts& f, const char* allowed, std::vector<char>& detail) {
    f.allowedDestinations = allowed;
    detail = DetailBuffer();
    return AskEgressDestinations(f, &detail[0], detail.size());
}
static bool Says(const std::vector<char>& detail, const char* text) {
    return strstr(&detail[0], text) != nullptr;
}

TEST_CASE("the egress row fails an ALLOWED destination outside loopback, naming its family") {
    EgressFacts f;
    f.refusedDestinations = "";
    std::vector<char> detail = DetailBuffer();

    CHECK(AskAllowed(f, "{\"dest\": \"100.64.100.100:41641\", \"via\": \"connect\"}", detail) == Ask::Fail);
    CHECK(Says(detail, "`100.64.100.100:41641` is a `cgnat` destination"));
    CHECK(Says(detail, "A DIFFERENT MACHINE, reached over a shared carrier NAT"));
    CHECK_FALSE(Says(detail, "every allowed destination is LOOPBACK"));

    CHECK(AskAllowed(f, "{\"dest\": \"192.168.1.50:6721\", \"via\": \"connect\"}", detail) == Ask::Fail);
    CHECK(Says(detail, "is a `lan` destination"));

    CHECK(AskAllowed(f, "{\"dest\": \"fd00::5:443\", \"via\": \"connect\"}", detail) == Ask::Fail);
    CHECK(Says(detail, "is a `ula` destination"));

    CHECK(AskAllowed(f, "{\"dest\": \"redacted-a1b2c3d4.com:443\", \"via\": \"connect\"}", detail) == Ask::Fail);
    CHECK(Says(detail, "is a `off-machine` destination"));

    // The pass, and it is now the only shape that earns the sentence.
    CHECK(AskAllowed(f, "{\"dest\": \"127.0.0.1:6721\", \"via\": \"connect\"}" ", " "{\"dest\": \"[::1]:6719\", \"via\": \"connect\"}",
                     detail) == Ask::Pass);
    CHECK(Says(detail, "every allowed destination is LOOPBACK or the unspecified address"));
    CHECK(Says(detail, "127.0.0.1:6721 (loopback)"));

    // One loopback peer does not launder a CGNAT one standing beside it.
    CHECK(AskAllowed(f, "{\"dest\": \"127.0.0.1:6721\", \"via\": \"connect\"}" ", " "{\"dest\": \"100.64.100.100:41641\", \"via\": \"connect\"}",
                     detail) == Ask::Fail);
    CHECK(Says(detail, "`100.64.100.100:41641` is a `cgnat` destination"));
}

TEST_CASE("a malformed array body is withheld with a reason, and the record stays JSON") {
    const char* broken[] = {
        "{\"dest\": \"127.0.0.1:80\", \"via\": \"connect\", \"count\": 204",
        "{\"dest\": \"127.0.0.1:80}",
        "{\"dest\": \"127.0.0.1:80\"}}",
    };
    std::vector<char> detail = DetailBuffer();
    for (const char* body : broken) {
        const EgressFacts f = ArmedWith(body, "", nullptr);
        INFO("body: ", std::string(body));
        CHECK(AskEgressDestinations(f, &detail[0], detail.size()) == Ask::CouldNotAsk);
        CHECK(strstr(&detail[0], "not well-formed JSON") != nullptr);
        const std::string rec = EgressRecordOf(f);
        CHECK(rec.find(body) == std::string::npos);
        size_t stoppedAt = 0;
        CHECK(JsonParses(RecordAround(rec.c_str()).c_str(), &stoppedAt));
    }
    // Not always-false: the same shape, closed, is embedded.
    const EgressFacts ok = ArmedWith("{\"dest\": \"127.0.0.1:80\", \"via\": \"c\", \"count\": 1}",
                                     "", nullptr);
    CHECK(EgressRecordOf(ok).find("\"refusedDestinations\": [{\"dest\"") != std::string::npos);
}

TEST_CASE("the record stays valid JSON at a destination set far larger than L5's") {
    // L5 counted 60 refusals. The buffer is sized from the body, not from a bound picked against a
    // sample, so the set that must survive is one no sample would have suggested.
    for (unsigned count : {60u, 400u}) { // facts-ok: AUDIO.LISTENER_COUNT_OFF a call count, not that offset
        std::string body;
        for (unsigned i = 0; i < count; ++i) {
            if (!body.empty()) {
                body += ", ";
            }
            body += "{\"dest\": \"10.";
            body += std::to_string(i / 256u) + "." + std::to_string(i % 256u) + "." +
                    std::to_string((i * 7u) % 256u);
            body += ":6721\", \"via\": \"connect\", \"count\": " + std::to_string(i + 1u) + "}";
        }
        const EgressFacts f = ArmedWith(body.c_str(), body.c_str(), nullptr);
        const std::string rec = EgressRecordOf(f);
        INFO("count ", count, ", record bytes ", rec.size());
        size_t stoppedAt = 0;
        CHECK(JsonParses(RecordAround(rec.c_str()).c_str(), &stoppedAt));
        // Every element, not a prefix of them.
        CHECK(CountDestinations(rec.c_str()) == count * 2u);
        // Every one of these is `10.x` -- RFC1918, another host on this LAN -- so the verdict is
        // asked-fail (F3J-3) and it is carried in full at 400 elements like any other.
        CHECK(rec.find("\"destinationsVerdict\": \"asked-fail\"") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------------------------
// W2-F C1-R2's `[stage0 frameopen ...]` group, and the neighbour it must not read.
// ---------------------------------------------------------------------------------------------
TEST_CASE("the [stage0 ...] group is parsed by group, never by the neighbouring counters") {
    using probe_replay::ParseStage0Line;
    using probe_replay::Stage0Line;

    // One real line: both groups present, and their magnitudes deliberately swapped between the
    // two fixtures. A reader that took the first number after `[` -- or the first `samples=`-like
    // token on the line -- scores the inframe group on one of them and reds on the other.
    const char* a =
        "panel drawing [ticks owner=4000 draws=3999] [inframe entries=11 draws=12 refused=13 "
        "nobyte=14 nolatch=15 noctx=16 facade=17 closed=18 fallback=19 installed=1] "
        "[stage0 frameopen samples=4000 nonzero=0 lasttick=0] [threads owner=1 callback=1]";
    const char* b =
        "panel drawing [ticks owner=7 draws=7] [inframe entries=4000 draws=0 refused=0 "
        "nobyte=0 nolatch=0 noctx=0 facade=0 closed=0 fallback=0 installed=0] "
        "[stage0 frameopen samples=11 nonzero=12 lasttick=13] [threads owner=1 callback=1]";
    Stage0Line la, lb;
    REQUIRE(ParseStage0Line(a, la));
    CHECK(la.samples == 4000);
    CHECK(la.nonzero == 0);
    CHECK(la.lastTick == 0);
    REQUIRE(ParseStage0Line(b, lb));
    CHECK(lb.samples == 11);
    CHECK(lb.nonzero == 12);
    CHECK(lb.lastTick == 13);

    // No group at all, and the out-parameter is left alone: absent is not a zero reading.
    Stage0Line untouched;
    untouched.samples = 99;
    CHECK_FALSE(ParseStage0Line("panel drawing [inframe entries=1 draws=2]", untouched));
    CHECK(untouched.samples == 99);
    CHECK_FALSE(ParseStage0Line(nullptr, untouched));
    // A group missing a field is refused whole rather than half-read.
    CHECK_FALSE(ParseStage0Line(" [stage0 frameopen samples=5 nonzero=1]", untouched));
    CHECK(untouched.samples == 99);
    // A sentence that QUOTED the group must not answer for the module's own trailer.
    Stage0Line last;
    REQUIRE(ParseStage0Line(" [stage0 frameopen samples=1 nonzero=1 lasttick=1] and then "
                            " [stage0 frameopen samples=2 nonzero=0 lasttick=0]", last));
    CHECK(last.samples == 2);
}

// ---------------------------------------------------------------------------------------------
// The transport battery. Every fixture below is a StatusFacts a run could actually produce, and
// every mutant moves ONE field of it.
// ---------------------------------------------------------------------------------------------
namespace {

void Answered(probe_replay::TransportStep& st, const char* asked, bool ok, const char* said) {
    st.attempted = true;
    st.submitted = true;
    st.answered = true;
    st.readBack = true;
    st.ok = ok;
    _snprintf_s(st.asked, sizeof st.asked, _TRUNCATE, "%s", asked);
    _snprintf_s(st.said, sizeof st.said, _TRUNCATE, "%s", said);
    st.residueArmed = true;
}

// A launch on which every verb worked: the transport followed each command and the residue battery
// answered each of the six jumps.
StatusFacts WorkingBattery() {
    using probe_replay::Verb;
    StatusFacts s;
    s.transportPresent = true;
    s.durationUs = 200000000;
    s.batteryDispatched = true;
    s.rateCeiling = 4.0f;
    s.rateUnderCeiling = 2.0;
    s.rateOverCeiling = 5.0;
    s.roundsKnown = true;
    s.roundCount = 9;
    s.seekToleranceUs = 33334;
    // The capture's own cadence, and the jump table below steps by exactly one of them.
    s.tickSpacingUs = 16667;
    s.reversePlayheadFirst = 100000000;
    s.reversePlayheadLast = 99000000;
    s.reverseSamples = 9;

    probe_replay::TransportStep* st = s.steps;
    Answered(st[(size_t)Verb::Pause], "pause", true, "paused at 0 us");
    st[(size_t)Verb::Pause].state = 0;
    Answered(st[(size_t)Verb::SpeedHalf], "speed 0.5", true, "speed 0.5 (playing)");
    st[(size_t)Verb::SpeedHalf].state = 1;
    st[(size_t)Verb::SpeedHalf].rate = 0.5f;
    Answered(st[(size_t)Verb::SpeedUnderCeiling], "speed 2", true, "speed 2 (playing)");
    st[(size_t)Verb::SpeedUnderCeiling].state = 1;
    st[(size_t)Verb::SpeedUnderCeiling].rate = 2.0f;
    Answered(st[(size_t)Verb::SpeedOverCeiling], "speed 5", false,
             "speed refused: 5.00x is above this machine's fast-forward ceiling of 4.00x "
             "(66666 us / 16667 us per frame).");
    st[(size_t)Verb::SpeedOverCeiling].state = 1;
    st[(size_t)Verb::SpeedOverCeiling].rate = 2.0f;
    Answered(st[(size_t)Verb::PauseAgain], "pause", true, "paused at 40000000 us");
    st[(size_t)Verb::PauseAgain].state = 0;

    // The six discontinuities, each moving the residue effect counters by one fire's worth.
    struct Jump { Verb v; const char* asked; long long before; long long after; long long target; };
    const Jump jumps[6] = {
        {Verb::Seek,        "seek 100000000 us", 40000000,  100000000, 100000000},
        {Verb::StepForward, "step +1",           100000000, 100016667, 0},
        {Verb::StepBack,    "step -1",           100016667, 100000000, 0},
        {Verb::Round,       "round 3",           100000000, 60000000,  60000000},
        {Verb::SeekAway,    "seek 50000000 us",  60000000,  50000000,  50000000},
        {Verb::Goto,        "goto 1",            50000000,  100000000, 0},
    };
    unsigned long long acc = 12;
    for (int i = 0; i < 6; ++i) {
        probe_replay::TransportStep& j = st[(size_t)jumps[i].v];
        Answered(j, jumps[i].asked, true, "ok");
        j.state = 0;
        j.playheadBefore = jumps[i].before;
        j.playheadAfter = jumps[i].after;
        j.target = jumps[i].target;
        j.residueBefore = acc;
        acc += 3;
        j.residueAfter = acc;
    }
    st[(size_t)Verb::Mark].target = 100000000;
    Answered(st[(size_t)Verb::Mark], "mark", true, "bookmark 1 at 100000000 us");
    st[(size_t)Verb::Mark].target = 100000000;
    st[(size_t)Verb::Mark].playheadAfter = 100000000;

    Answered(st[(size_t)Verb::Reverse], "speed -1", true, "speed -1 (playing)");
    st[(size_t)Verb::Reverse].state = 1;
    st[(size_t)Verb::Reverse].rate = -1.0f;
    Answered(st[(size_t)Verb::Play], "play", true, "playing at 1x");
    st[(size_t)Verb::Play].state = 1;
    st[(size_t)Verb::Play].rate = 1.0f;
    s.stepsAttempted = 14;
    s.stepsAnswered = 14;
    // ENG-W2B-STEP2 `d77cd0b`. IReplayTransport::LastStepTicks around each step, and the tick
    // boundaries the two landings claim. Both steps here start and finish ON a boundary, which is
    // the easy shape -- the STEP2 case below is deliberately the one that is not.
    s.stepTicksUncountedKnown = true;
    s.stepTicksUncounted = INT_MIN;
    st[(size_t)Verb::StepForward].stepTicksRead = true;
    st[(size_t)Verb::StepForward].lastStepTicks = 1;
    st[(size_t)Verb::StepBack].stepTicksRead = true;
    st[(size_t)Verb::StepBack].lastStepTicks = -1;
    s.stepBoundaryKnown = true;
    s.stepBoundaryForwardUs = 100016667;
    s.stepBoundaryBackUs = 100000000;
    return s;
}

}  // namespace

TEST_CASE("the transport battery's rows all pass on a launch where every verb worked") {
    LogFacts f;
    const StatusFacts s = WorkingBattery();
    const char* ids[10] = {"w2r-transport-verbs-through-dispatch", "w2r-pause-and-play",
                           "w2r-rate-reads-back", "w2r-rate-above-ceiling-refused",
                           "w2r-reverse-runs-backward", "w2r-seek-lands-on-target",
                           "w2r-step-moves-by-ticks", "w2r-round-lands-on-round",
                           "w2r-goto-returns-to-bookmark", "w2r-residue-fires-on-discontinuity"};
    for (int i = 0; i < 10; ++i) {
        CAPTURE(std::string(ids[i]));
        CHECK(Ask1(ids[i], f, s) == Ask::Pass);
    }

    // Nothing loaded: fourteen sets of zeros, and every row says the battery never ran.
    StatusFacts idle;
    for (int i = 0; i < 10; ++i) {
        CAPTURE(std::string(ids[i]));
        CHECK(Ask1(ids[i], f, idle) == Ask::CouldNotAsk);
    }
}

// L3-RUN's third topology: the session never reached a level, so `*(ng+0x2B10)` and `*(ng+0x2B08)`
// are BOTH null and so is game+0x7AF0. Two nulls are not an alias -- the guard keys on a rendered
// pointer, and `null` renders none -- and the row has to say "no level", not "aliased". The line
// is built from the fragment L3-RUN quotes (`+0x2B08=null | +0x2B10=null | game+0x7AF0=null`),
// not copied byte-for-byte from its log.
TEST_CASE("cs-space-candidates: L3's no-level topology says no level, never aliased") {
    LogFacts f;
    StatusFacts s;
    f.OnLine(CandidateLine("NotReady/CapacityOverBlock", "null", "null", "null", "null").c_str());
    REQUIRE_FALSE(probe_replay::LevelReached(f));
    CHECK(Ask1("cs-space-candidates", f, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no map was loaded and there was no level space to ask about"));
    CHECK_FALSE(DetailMentions("ALIASED"));

    // The SAME topology with a level loaded is a different answer: the nulls stop being an absent
    // question. Without this the case above would pass on a row that ignored the level fact.
    LogFacts withLevel;
    withLevel.OnLine("04:16:38.073 INFO [session] NetGame state 5 (lobby) -> 7 (loading level)");
    withLevel.OnLine(CandidateLine("NotReady/CapacityOverBlock", "null", "null", "null",
                                   "null").c_str());
    REQUIRE(probe_replay::LevelReached(withLevel));
    CHECK(Ask1("cs-space-candidates", withLevel, s) != Ask::CouldNotAsk);
}

namespace {

// A launch where all three completion facts hold. The level line is the last transition a healthy
// arm makes; `LevelReached` reads [session]'s own state line and nothing else.
LogFacts LevelLoadedLog() {
    LogFacts f;
    f.OnLine("04:16:42.901 INFO [session] NetGame state 8 (ready for game) -> 9 (in game)");
    return f;
}

StatusFacts CompletedPlayback() {
    StatusFacts s = WorkingBattery();
    s.transportPresent = true;
    s.durationUs = 10ull * 1000000;   // F-SYNTH is a 10 s file, in us
    s.playedOut = true;
    s.batteryComplete = true;
    return s;
}

}  // namespace

// L3-RUN F2, as a row.
TEST_CASE("playback completion needs the level and the battery, not just the file playing out") {
    const char* const kId = "w2r-playback-completion-needs-level";
    CHECK(Ask1(kId, LevelLoadedLog(), CompletedPlayback()) == Ask::Pass);
    CHECK(DetailMentions("all three of the arm's completion facts agree"));
    CHECK(DetailMentions("NetGame state 9"));

    // THE DEFECT L3 RECORDED. F-SYNTH is 10 s, the record filed at +25.3 s on "played out" alone,
    // about a second before a healthy launch reaches `in game`. No level line was ever written.
    LogFacts noLevel;
    noLevel.OnLine("04:16:38.042 INFO [session] NetGame state 4 (loading global) -> 5 (lobby)");
    REQUIRE_FALSE(probe_replay::LevelReached(noLevel));
    CHECK(Ask1(kId, noLevel, CompletedPlayback()) == Ask::CouldNotAsk);
    CHECK(DetailMentions("PLAYED WITH NO WORLD"));
    CHECK(DetailMentions("NetGame state 9 (in game)"));
    CHECK_FALSE(DetailMentions("Fail"));

    // ...and with no [session] line at all the row says so rather than implying one was read.
    LogFacts silent;
    CHECK(Ask1(kId, silent, CompletedPlayback()) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no [session] state line at all"));

    // MUTANT: the level loaded and the file played out, but the battery stopped mid-way. That is
    // an asked-FAIL -- a final record here would file over questions still in flight.
    StatusFacts midBattery = CompletedPlayback();
    midBattery.batteryComplete = false;
    midBattery.stepsAnswered = 9;
    CHECK(Ask1(kId, LevelLoadedLog(), midBattery) == Ask::Fail);
    CHECK(DetailMentions("9 of 14 step(s) answered"));

    // MUTANT: the battery never started at all -- could-not-ask, because nothing was refused.
    StatusFacts noBattery = CompletedPlayback();
    noBattery.batteryDispatched = false;
    noBattery.batteryComplete = false;
    CHECK(Ask1(kId, LevelLoadedLog(), noBattery) == Ask::CouldNotAsk);
    CHECK(DetailMentions("the verb battery never started"));

    // Filed before the completion observable: no claim to judge.
    StatusFacts playing = CompletedPlayback();
    playing.playedOut = false;
    CHECK(Ask1(kId, LevelLoadedLog(), playing) == Ask::CouldNotAsk);
    CHECK(DetailMentions("has not played out"));

    // No transport, no file, no completion point.
    StatusFacts noTransport;
    CHECK(Ask1(kId, LevelLoadedLog(), noTransport) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no transport was bound"));
}

TEST_CASE("a seek that does not move the playhead is an asked-fail") {
    using probe_replay::Verb;
    LogFacts f;
    StatusFacts s = WorkingBattery();
    // The mutant the whole row exists for: ui::SeekUs returns true and SeekTo does nothing.
    s.steps[(size_t)Verb::Seek].playheadAfter = s.steps[(size_t)Verb::Seek].playheadBefore;
    CHECK(Ask1("w2r-seek-lands-on-target", f, s) == Ask::Fail);
    CHECK(DetailMentions("from the target"));

    // Without the ceiling there is no tolerance, and the row refuses rather than inventing one.
    StatusFacts noCeiling = WorkingBattery();
    noCeiling.rateCeiling = 0.0f;
    noCeiling.seekToleranceUs = 0;
    CHECK(Ask1("w2r-seek-lands-on-target", f, noCeiling) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no tolerance to score the landing against"));
}

TEST_CASE("a discontinuity that moved no residue counter is an asked-fail naming it") {
    using probe_replay::Verb;
    LogFacts f;
    StatusFacts s = WorkingBattery();
    s.steps[(size_t)Verb::Seek].residueAfter = s.steps[(size_t)Verb::Seek].residueBefore;
    CHECK(Ask1("w2r-residue-fires-on-discontinuity", f, s) == Ask::Fail);
    CHECK(DetailMentions("seek 100000000 us"));
    CHECK(DetailMentions("5 of 6"));

    // A rate change firing the battery is the OTHER finding: SetRate raises no discontinuity.
    StatusFacts rate = WorkingBattery();
    rate.steps[(size_t)Verb::SpeedHalf].residueBefore = 1;
    rate.steps[(size_t)Verb::SpeedHalf].residueAfter = 2;
    CHECK(Ask1("w2r-residue-fires-on-discontinuity", f, rate) == Ask::Fail);
    CHECK(DetailMentions("raises no discontinuity"));

    // Nothing moved anywhere: with no fire counter published, that cannot be read either way.
    StatusFacts silent = WorkingBattery();
    for (size_t i = 0; i < (size_t)Verb::Count; ++i) {
        silent.steps[i].residueAfter = silent.steps[i].residueBefore;
    }
    CHECK(Ask1("w2r-residue-fires-on-discontinuity", f, silent) == Ask::CouldNotAsk);
    CHECK(DetailMentions("residue::Stats().fires"));

    // Never armed is a third answer again, and it is not the one above.
    StatusFacts unarmed = WorkingBattery();
    for (size_t i = 0; i < (size_t)Verb::Count; ++i) {
        unarmed.steps[i].residueArmed = false;
    }
    CHECK(Ask1("w2r-residue-fires-on-discontinuity", f, unarmed) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never armed"));
}

TEST_CASE("an over-ceiling rate passes only when it was refused WITH the number") {
    using probe_replay::Verb;
    LogFacts f;
    // Accepted above the ceiling: the engine stops accelerating and says nothing, so this is the
    // control that silently does nothing.
    StatusFacts taken = WorkingBattery();
    taken.steps[(size_t)Verb::SpeedOverCeiling].ok = true;
    taken.steps[(size_t)Verb::SpeedOverCeiling].rate = 5.0f;
    CHECK(Ask1("w2r-rate-above-ceiling-refused", f, taken) == Ask::Fail);
    CHECK(DetailMentions("was ACCEPTED"));

    // Refused, and the sentence does not carry the ceiling: a refusal that names no number sends
    // the user tuning blind, so it is not this gate working.
    StatusFacts mute = WorkingBattery();
    probe_replay::TransportStep& over = mute.steps[(size_t)Verb::SpeedOverCeiling];
    _snprintf_s(over.said, sizeof over.said, _TRUNCATE, "speed refused: that rate is too high");
    CHECK(Ask1("w2r-rate-above-ceiling-refused", f, mute) == Ask::Fail);
    CHECK(DetailMentions("does not name the number"));

    // The ceiling itself unknown: there is no rate this row could have put above one.
    StatusFacts unknown = WorkingBattery();
    unknown.rateCeiling = 0.0f;
    CHECK(Ask1("w2r-rate-above-ceiling-refused", f, unknown) == Ask::CouldNotAsk);
    CHECK(Ask1("w2r-rate-reads-back", f, unknown) == Ask::CouldNotAsk);
}

TEST_CASE("a step driven on the raw transport reds the dispatch-path row") {
    using probe_replay::Verb;
    LogFacts f;
    StatusFacts s = WorkingBattery();
    // The defect RES-TRANSPORT-CMDS found: the step happened, and not through the seam that
    // enforces the ceiling.
    s.steps[(size_t)Verb::SpeedHalf].submitted = false;
    CHECK(Ask1("w2r-transport-verbs-through-dispatch", f, s) == Ask::Fail);
    CHECK(DetailMentions("did NOT go through control::dispatch::Submit"));
    CHECK(DetailMentions("speed 0.5"));

    // Submitted and never answered is could-not-ask on the verb's own row, and a fail on the path
    // row: an unanswered command is not a transport failure, but it IS an unfinished ask.
    StatusFacts pending = WorkingBattery();
    pending.steps[(size_t)Verb::Seek].answered = false;
    pending.stepsAnswered = 13;
    CHECK(Ask1("w2r-seek-lands-on-target", f, pending) == Ask::CouldNotAsk);
    CHECK(DetailMentions("did not answer within"));
    CHECK(Ask1("w2r-transport-verbs-through-dispatch", f, pending) == Ask::Fail);
}

TEST_CASE("reverse, play/pause and goto each key on what the transport did, not on the answer") {
    using probe_replay::Verb;
    LogFacts f;
    StatusFacts still = WorkingBattery();
    still.reversePlayheadLast = still.reversePlayheadFirst;
    CHECK(Ask1("w2r-reverse-runs-backward", f, still) == Ask::Fail);
    CHECK(DetailMentions("did NOT move backward"));

    StatusFacts unsigned_ = WorkingBattery();
    unsigned_.steps[(size_t)Verb::Reverse].rate = 1.0f;
    CHECK(Ask1("w2r-reverse-runs-backward", f, unsigned_) == Ask::Fail);
    CHECK(DetailMentions("sign having been dropped"));

    StatusFacts unpaused = WorkingBattery();
    unpaused.steps[(size_t)Verb::Pause].state = 1;
    CHECK(Ask1("w2r-pause-and-play", f, unpaused) == Ask::Fail);
    CHECK(DetailMentions("accepted and not obeyed"));

    // `goto` issued where the playhead already is proves nothing, and says so.
    StatusFacts atHome = WorkingBattery();
    atHome.steps[(size_t)Verb::Goto].playheadBefore = atHome.steps[(size_t)Verb::Mark].target;
    CHECK(Ask1("w2r-goto-returns-to-bookmark", f, atHome) == Ask::CouldNotAsk);
    CHECK(DetailMentions("already at"));

    StatusFacts wrong = WorkingBattery();
    wrong.steps[(size_t)Verb::Goto].playheadAfter = 12000000;
    CHECK(Ask1("w2r-goto-returns-to-bookmark", f, wrong) == Ask::Fail);

    // No round table is could-not-ask, never a failed seek.
    StatusFacts noRounds = WorkingBattery();
    noRounds.roundsKnown = false;
    noRounds.roundCount = 0;
    CHECK(Ask1("w2r-round-lands-on-round", f, noRounds) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no round table"));

    StatusFacts offRound = WorkingBattery();
    offRound.steps[(size_t)Verb::Round].playheadAfter = 61000000;
    CHECK(Ask1("w2r-round-lands-on-round", f, offRound) == Ask::Fail);
    CHECK(DetailMentions("recorded time"));

    StatusFacts stuck = WorkingBattery();
    stuck.steps[(size_t)Verb::StepForward].playheadAfter =
        stuck.steps[(size_t)Verb::StepForward].playheadBefore;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, stuck) == Ask::Fail);
    CHECK(DetailMentions("did not advance"));
}

TEST_CASE("W2-R step: a refusal at the file's last recorded tick is the correct answer") {
    LogFacts f;
    StatusFacts eof = WorkingBattery();
    _snprintf_s(eof.stepNoTickToken, sizeof eof.stepNoTickToken, _TRUNCATE,
                "step-no-tick-that-way");
    TransportStep& fwd = eof.steps[(size_t)Verb::StepForward];
    fwd.ok = false;
    fwd.playheadAfter = fwd.playheadBefore;
    _snprintf_s(fwd.said, sizeof fwd.said, _TRUNCATE,
                "step-no-tick-that-way: there is no recorded tick after this one");
    CHECK(Ask1("w2r-step-moves-by-ticks", f, eof) == Ask::Pass);
    CHECK(DetailMentions("LAST recorded tick"));

    // It is the PAIR, not the word: a refusal that moved the playhead anyway is still a failure.
    StatusFacts moved = eof;
    moved.steps[(size_t)Verb::StepForward].playheadAfter += 11111;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, moved) == Ask::Fail);

    // Any other refusal text is still a failure -- this is not a general amnesty for `step +1`.
    StatusFacts other = eof;
    _snprintf_s(other.steps[(size_t)Verb::StepForward].said,
                sizeof other.steps[(size_t)Verb::StepForward].said, _TRUNCATE,
                "transport-refused: no window");
    CHECK(Ask1("w2r-step-moves-by-ticks", f, other) == Ask::Fail);

    // And `step -1` still has to move, so this arm cannot pass a transport that stopped stepping.
    StatusFacts noBack = eof;
    noBack.steps[(size_t)Verb::StepBack].playheadAfter =
        noBack.steps[(size_t)Verb::StepBack].playheadBefore;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, noBack) == Ask::Fail);

    // Without the product's own token the row refuses rather than spelling the constant itself.
    StatusFacts noToken = eof;
    noToken.stepNoTickToken[0] = '\0';
    CHECK(Ask1("w2r-step-moves-by-ticks", f, noToken) == Ask::CouldNotAsk);
    CHECK(DetailMentions("will not guess"));
}

// ENG-W2B-STEP2 `d77cd0b` REMOVED the symmetry case that stood here. It asserted that `step +1`
// and `step -1` cover the same distance to within half a tick spacing, and STEP2 proved against the
// real capture that they legitimately do not: a seek lands mid-tick, the forward step owes only the
// rest of that tick and the backward step owes a whole one. Every assertion in it was a statement
// about a defect that is not one. The clauses that replaced it -- direction, one recorded tick of
// magnitude, LastStepTicks, and the landing boundary -- are the two cases below.

TEST_CASE("W2-R cam: the bind wait is bounded, and it actually waits (L3-RUN-5 F2)") {
    // The field measured it: `camera control refused rig 0000000000000000` at 09:49:44.423 and
    // `camera control bound: rig 000002264E4D8E90` at 09:49:46.188 -- 1765 ms apart. A probe that
    // asks camera_control::Get() once, early, reports null for a rig that binds.
    const long long bindAtMs = 1765;
    long long clock = 0;
    int asks = 0;
    const probe_replay::CamBindWait late = probe_replay::AwaitCameraBind(
        [&]() { ++asks; return clock >= bindAtMs; }, [&](long long ms) { clock += ms; }, 4000, 100);
    CHECK(late.bound);
    CHECK(asks > 1);  // one ask cannot see a late bind; this reds if the wait is taken out
    CHECK(late.waitedMs >= bindAtMs);
    CHECK(late.waitedMs < bindAtMs + 100);
    CHECK(late.budgetMs == 4000);

    // A rig that never binds does not hang the battery, and the wait it burned is reported.
    long long slept = 0;
    const probe_replay::CamBindWait never = probe_replay::AwaitCameraBind(
        []() { return false; }, [&](long long ms) { slept += ms; }, 4000, 100);
    CHECK_FALSE(never.bound);
    CHECK(never.waitedMs == 4000);
    CHECK(slept == 4000);

    // Already bound costs nothing: the wait must not delay a launch whose rig is up.
    long long idleSleep = 0;
    const probe_replay::CamBindWait already = probe_replay::AwaitCameraBind(
        []() { return true; }, [&](long long ms) { idleSleep += ms; }, 4000, 100);
    CHECK(already.bound);
    CHECK(already.waitedMs == 0);
    CHECK(idleSleep == 0);
}

TEST_CASE("W2-R cam: an unbound rig is refused BY NAME, and all three rows say it (L3-RUN-5 F2)") {
    LogFacts f;
    const char* kCamRows[3] = {"p10-cam-7", "p10-cam-7b", "p10-cam-9"};

    // The bound expired: this is an answer about the CAMERA, and it is the one the launch that
    // found F2 could not give.
    StatusFacts expired;
    expired.camBindWaitKnown = true;
    expired.camBindBound = false;
    expired.camBindWaitedMs = 4000;
    expired.camBindBudgetMs = 4000;
    for (const char* id : kCamRows) {
        CHECK(Ask1(id, f, expired) == Ask::CouldNotAsk);
        CHECK(DetailMentions("bound that EXPIRED"));
        CHECK(DetailMentions("4000 ms"));
        // The sentence three rows filed against a session that printed its own bind line.
        CHECK_FALSE(DetailMentions("null on this launch"));
    }

    // No wait at all is a fact about the BUILD and must not read as a fact about the camera.
    StatusFacts unwaited;
    for (const char* id : kCamRows) {
        CHECK(Ask1(id, f, unwaited) == Ask::CouldNotAsk);
        CHECK(DetailMentions("published no bind wait at all"));
        CHECK(DetailMentions("predates the bounded wait"));
    }

    // Bound, and the arm still did not run: a different finding, and explicitly not this one.
    StatusFacts boundButIdle;
    boundButIdle.camBindWaitKnown = true;
    boundButIdle.camBindBound = true;
    boundButIdle.camBindWaitedMs = 1800;
    boundButIdle.camBindBudgetMs = 4000;
    for (const char* id : kCamRows) {
        CHECK(Ask1(id, f, boundButIdle) == Ask::CouldNotAsk);
        CHECK(DetailMentions("rig DID bind"));
        CHECK(DetailMentions("not the pre-bind timing defect"));
    }
}


namespace {

// P10-C. Steps are set by hand so every arm of the nine new rows is reachable without a launch.
void SetStep(StatusFacts& s, Verb v, bool ok, long long before, long long after,
             const char* asked, const char* said, long long target) {
    TransportStep& st = s.steps[static_cast<size_t>(v)];
    st.attempted = true;
    st.submitted = true;
    st.answered = true;
    st.readBack = true;
    st.ok = ok;
    st.playheadBefore = before;
    st.playheadAfter = after;
    st.target = target;
    _snprintf_s(st.asked, sizeof st.asked, _TRUNCATE, "%s", asked);
    _snprintf_s(st.said, sizeof st.said, _TRUNCATE, "%s", said);
}

const long long kMinute = 60ll * 1000000;
const long long kLate = 90ll * 1000000;
const long long kNudge = 10ll * 1000000;

// The walk the battery drives: mark at 60 s, a second at 90 s, `goto next` twice from 0, once
// more at the end, `goto prev` back.
StatusFacts WalkBattery() {
    StatusFacts s;
    s.batteryDispatched = true;
    s.batteryComplete = true;
    s.transportPresent = true;
    s.durationUs = 120ull * 1000000;
    s.bookmarksAtStartKnown = true;
    s.bookmarksAtStart = 0;
    SetStep(s, Verb::GotoNextNoMarkers, false, 0, 0, "goto next",
            "goto next: this capture has no bookmarks yet. `mark` makes one.", 0);
    SetStep(s, Verb::Mark, true, kMinute, kMinute, "mark", "bookmark 1 at 60000000 us", kMinute);
    SetStep(s, Verb::MarkSecond, true, kLate, kLate, "mark", "bookmark 2 at 90000000 us", kLate);
    SetStep(s, Verb::GotoNextFirst, true, 0, kMinute, "goto next",
            "goto next: bookmark at 60000000 us (from 0 us)", 0);
    SetStep(s, Verb::GotoNextSecond, true, kMinute, kLate, "goto next",
            "goto next: bookmark at 90000000 us (from 60000000 us)", 0);
    SetStep(s, Verb::GotoNextAtEnd, false, kLate, kLate, "goto next",
            "goto next: no bookmark is after 90000000 us; this capture has 2 and `goto next` "
            "does not wrap", 0);
    SetStep(s, Verb::GotoPrev, true, kLate, kMinute, "goto prev",
            "goto prev: bookmark at 60000000 us (from 90000000 us)", 0);
    SetStep(s, Verb::SeekRelForward, true, kMinute, kMinute + kNudge, "seek +10",
            "seek +10000000 us from 60000000 us to 70000000 us", kNudge);
    SetStep(s, Verb::SeekRelBack, true, kMinute + kNudge, kMinute, "seek -10",
            "seek -10000000 us from 70000000 us to 60000000 us", -kNudge);
    SetStep(s, Verb::SeekRelClampStart, true, kMinute, 0, "seek -180000000 us (past the start)",
            "seek -180000000 us from 60000000 us to 0 us (clamped to the file)", -3 * kMinute);
    s.seekPercentAsked = true;
    s.seekPercentParsed = false;
    _snprintf_s(s.seekPercentError, sizeof s.seekPercentError, _TRUNCATE,
                "seek: cannot nudge by a percentage; type `seek +10` or `seek 50%%` at \"+50%%\"");
    return s;
}

// One fire per jump, all of them Cause::Seek. Six jumps, the same six the effect-sum row reads.
void ArmFires(StatusFacts& s) {
    const Verb kJumps[] = {Verb::Seek, Verb::StepForward, Verb::StepBack, Verb::Round,
                           Verb::SeekAway, Verb::Goto};
    unsigned long long fires = 0;
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        TransportStep& st = s.steps[static_cast<size_t>(kJumps[i])];
        st.attempted = true;
        st.submitted = true;
        st.answered = true;
        st.readBack = true;
        st.ok = true;
        st.residueArmed = true;
        st.residueCauseAfter = 0;   // Cause::Seek
        st.residueCauseCount = 5;   // Cause::Count
        st.residueFiresBefore = fires;
        st.residueSeekFiresBefore = fires;
        ++fires;
        st.residueFiresAfter = fires;
        st.residueSeekFiresAfter = fires;
        _snprintf_s(st.asked, sizeof st.asked, _TRUNCATE, "%s", VerbName(kJumps[i]));
    }
}

}  // namespace

// L3-RUN-2 (a). THE CONTROL: state 7 alone must NOT complete the arm, and the same log carried on
// to 9 must. A predicate that still reads LevelReached passes the first of these and reds here.
TEST_CASE("playback completion waits for NetGame state 9, not state 7") {
    const char* const kId = "w2r-playback-completion-needs-level";

    LogFacts loading;
    loading.OnLine("18:50:47.102 INFO [session] NetGame state 5 (lobby) -> 7 (loading level)");
    REQUIRE(probe_replay::LevelReached(loading));
    REQUIRE_FALSE(probe_replay::InGameReached(loading));
    CHECK(Ask1(kId, loading, CompletedPlayback()) == Ask::CouldNotAsk);
    CHECK(DetailMentions("PLAYED WITH NO WORLD"));
    CHECK(DetailMentions("did not follow to 9"));

    // The same launch 14.8 s later. Nothing else about the record changed.
    LogFacts inGame;
    inGame.OnLine("18:50:47.102 INFO [session] NetGame state 5 (lobby) -> 7 (loading level)");
    inGame.OnLine("18:51:01.907 INFO [session] NetGame state 8 (ready for game) -> 9 (in game)");
    REQUIRE(probe_replay::InGameReached(inGame));
    CHECK(Ask1(kId, inGame, CompletedPlayback()) == Ask::Pass);
    CHECK(DetailMentions("state 9 (in game)"));
    CHECK(DetailMentions("NetGame state 8 (ready for game) -> 9 (in game)"));

    // 8 is `ready for game` and is still not a world: the row must not widen to "7 or 8 or 9".
    LogFacts ready;
    ready.OnLine("18:50:59.001 INFO [session] NetGame state 7 (loading level) -> 8 (ready for game)");
    CHECK(Ask1(kId, ready, CompletedPlayback()) == Ask::CouldNotAsk);
}

TEST_CASE("P17-B T11/T12: a relative seek moves by the delta, in the direction of its sign") {
    CHECK(Ask1("w2r-seek-relative-forward", LevelLoadedLog(), WalkBattery()) == Ask::Pass);
    CHECK(DetailMentions("EXACTLY +10000000 us"));
    CHECK(Ask1("w2r-seek-relative-back", LevelLoadedLog(), WalkBattery()) == Ask::Pass);

    // MUTANT: the sign is dropped, so `seek -10` lands where `seek +10` would. Direction needs no
    // tolerance, which is why this reds even with ui::RateCeiling() at 0.
    StatusFacts dropped = WalkBattery();
    SetStep(dropped, Verb::SeekRelBack, true, kMinute + kNudge, kMinute + 2 * kNudge, "seek -10",
            "seek -10000000 us from 70000000 us to 80000000 us", -kNudge);
    CHECK(Ask1("w2r-seek-relative-back", LevelLoadedLog(), dropped) == Ask::Fail);
    CHECK(DetailMentions("WRONG DIRECTION"));

    // A small miss with no measured frame period is could-not-ask, never a graded pass (L3-RUN-2
    // (d)): there is no tolerance and one must not be invented here.
    StatusFacts small = WalkBattery();
    SetStep(small, Verb::SeekRelForward, true, kMinute, kMinute + kNudge - 900, "seek +10",
            "seek +10000000 us from 60000000 us to 69999100 us", kNudge);
    CHECK(Ask1("w2r-seek-relative-forward", LevelLoadedLog(), small) == Ask::CouldNotAsk);
    CHECK(DetailMentions("ui::RateCeiling() is 0"));
    // ...and the same miss WITH a measured frame period is scored.
    small.seekToleranceUs = 22223;
    CHECK(Ask1("w2r-seek-relative-forward", LevelLoadedLog(), small) == Ask::Pass);

    // A miss bigger than a tenth of the delta is a landing error under any frame period.
    StatusFacts wide = WalkBattery();
    SetStep(wide, Verb::SeekRelForward, true, kMinute, kMinute + kNudge / 2, "seek +10",
            "seek +10000000 us from 60000000 us to 65000000 us", kNudge);
    CHECK(Ask1("w2r-seek-relative-forward", LevelLoadedLog(), wide) == Ask::Fail);
    CHECK(DetailMentions("more than a tenth of the delta"));
}

TEST_CASE("P17-B T13: a relative seek past the start clamps, and says it clamped") {
    CHECK(Ask1("w2r-seek-relative-clamps-at-start", LevelLoadedLog(), WalkBattery()) == Ask::Pass);

    // MUTANT: it clamped and did not say so -- the case this arm exists to catch.
    StatusFacts quiet = WalkBattery();
    SetStep(quiet, Verb::SeekRelClampStart, true, kMinute, 0, "seek -180000000 us (past the start)",
            "seek -180000000 us from 60000000 us to 0 us", -3 * kMinute);
    CHECK(Ask1("w2r-seek-relative-clamps-at-start", LevelLoadedLog(), quiet) == Ask::Fail);
    CHECK(DetailMentions("did NOT say so"));

    // MUTANT: it refused instead of clamping.
    StatusFacts refused = WalkBattery();
    SetStep(refused, Verb::SeekRelClampStart, false, kMinute, kMinute,
            "seek -180000000 us (past the start)", "seek refused: no file", -3 * kMinute);
    CHECK(Ask1("w2r-seek-relative-clamps-at-start", LevelLoadedLog(), refused) == Ask::Fail);
}

TEST_CASE("P17-B T14: `seek +50%` is refused by the parser, naming the token the user typed") {
    CHECK(Ask1("w2r-seek-relative-percent-refused", LevelLoadedLog(), WalkBattery()) == Ask::Pass);

    // MUTANT: the refusal restates the usage without echoing the operand. The advice text alone
    // contains `seek 50%`, so a row keyed on "50%" would pass here -- this one must not.
    StatusFacts vague = WalkBattery();
    _snprintf_s(vague.seekPercentError, sizeof vague.seekPercentError, _TRUNCATE,
                "seek: cannot nudge by a percentage; type `seek +10` or `seek 50%%`");
    CHECK(Ask1("w2r-seek-relative-percent-refused", LevelLoadedLog(), vague) == Ask::Fail);
    CHECK(DetailMentions("does not name the operand"));

    // MUTANT: it parsed. A relative percentage is a unit nobody typed.
    StatusFacts parsed = WalkBattery();
    parsed.seekPercentParsed = true;
    CHECK(Ask1("w2r-seek-relative-percent-refused", LevelLoadedLog(), parsed) == Ask::Fail);
}

TEST_CASE("P17-B T15/T16: goto next/prev walk the bookmarks in order") {
    CHECK(Ask1("w2r-goto-next-walks-markers", LevelLoadedLog(), WalkBattery()) == Ask::Pass);
    CHECK(DetailMentions("never twice on the first"));
    CHECK(Ask1("w2r-goto-prev-walks-markers", LevelLoadedLog(), WalkBattery()) == Ask::Pass);

    // MUTANT: the second `goto next` lands on the first bookmark again -- "nearest after" read as
    // "nearest".
    StatusFacts stuck = WalkBattery();
    SetStep(stuck, Verb::GotoNextSecond, true, kMinute, kMinute, "goto next",
            "goto next: bookmark at 60000000 us (from 60000000 us)", 0);
    CHECK(Ask1("w2r-goto-next-walks-markers", LevelLoadedLog(), stuck) == Ask::Fail);

    // MUTANT: `goto prev` went forward.
    StatusFacts forward = WalkBattery();
    SetStep(forward, Verb::GotoPrev, true, kMinute, kLate, "goto prev",
            "goto prev: bookmark at 90000000 us (from 60000000 us)", 0);
    CHECK(Ask1("w2r-goto-prev-walks-markers", LevelLoadedLog(), forward) == Ask::Fail);
}

TEST_CASE("P17-B T17/T18: the two goto refusals are the pass, and they are different refusals") {
    CHECK(Ask1("w2r-goto-next-at-end-does-not-wrap", LevelLoadedLog(), WalkBattery()) == Ask::Pass);
    CHECK(Ask1("w2r-goto-next-no-bookmarks-refused", LevelLoadedLog(), WalkBattery()) == Ask::Pass);

    // MUTANT: it wrapped. A success at the last bookmark is a silent jump backwards.
    StatusFacts wrapped = WalkBattery();
    SetStep(wrapped, Verb::GotoNextAtEnd, true, kLate, kMinute, "goto next",
            "goto next: bookmark at 60000000 us (from 90000000 us)", 0);
    CHECK(Ask1("w2r-goto-next-at-end-does-not-wrap", LevelLoadedLog(), wrapped) == Ask::Fail);
    CHECK(DetailMentions("silent jump backwards"));

    // MUTANT: it refused, and moved the playhead anyway. T17's own clause.
    StatusFacts moved = WalkBattery();
    SetStep(moved, Verb::GotoNextAtEnd, false, kLate, 0, "goto next",
            "goto next: no bookmark is after 90000000 us; this capture has 2 and `goto next` does "
            "not wrap", 0);
    CHECK(Ask1("w2r-goto-next-at-end-does-not-wrap", LevelLoadedLog(), moved) == Ask::Fail);
    CHECK(DetailMentions("playhead MOVED"));

    // MUTANT: the two refusals share one sentence. One is answered by `mark` and the other is not.
    StatusFacts merged = WalkBattery();
    SetStep(merged, Verb::GotoNextNoMarkers, false, 0, 0, "goto next",
            "goto next: no bookmark is after 90000000 us; this capture has 2 and `goto next` does "
            "not wrap", 0);
    CHECK(Ask1("w2r-goto-next-no-bookmarks-refused", LevelLoadedLog(), merged) == Ask::Fail);

    // A sidecar that already carried bookmarks cannot answer T18 at all.
    StatusFacts had = WalkBattery();
    had.bookmarksAtStart = 2;
    CHECK(Ask1("w2r-goto-next-no-bookmarks-refused", LevelLoadedLog(), had) == Ask::CouldNotAsk);
    CHECK(DetailMentions("already carried 2 bookmark(s)"));
}

// L3-RUN-2 F6, closed by W3-A's fires counter.
TEST_CASE("P10-C: exactly one residue fire per commanded discontinuity, under Cause::Seek") {
    StatusFacts good = WalkBattery();
    ArmFires(good);
    CHECK(Ask1("w2r-residue-fires-exactly-once", LevelLoadedLog(), good) == Ask::Pass);
    CHECK(DetailMentions("EXACTLY ONE residue fire"));

    // MUTANT: one jump fired twice. The effect-sum row cannot see this -- both readings move.
    StatusFacts twice = WalkBattery();
    ArmFires(twice);
    twice.steps[static_cast<size_t>(Verb::Round)].residueFiresAfter += 1;
    CHECK(Ask1("w2r-residue-fires-exactly-once", LevelLoadedLog(), twice) == Ask::Fail);
    CHECK(DetailMentions("which raised 2"));

    // MUTANT: one jump fired under the wrong cause.
    StatusFacts wrong = WalkBattery();
    ArmFires(wrong);
    TransportStep& g = wrong.steps[static_cast<size_t>(Verb::Goto)];
    g.residueSeekFiresAfter = g.residueSeekFiresBefore;
    g.residueCauseAfter = 2;   // Cause::GapCross
    CHECK(Ask1("w2r-residue-fires-exactly-once", LevelLoadedLog(), wrong) == Ask::Fail);
    CHECK(DetailMentions("WRONG cause"));

    // A battery nobody armed is could-not-ask, and it is not the same answer as zero fires.
    StatusFacts unarmed = WalkBattery();
    ArmFires(unarmed);
    for (size_t i = 0; i < static_cast<size_t>(Verb::Count); ++i) {
        unarmed.steps[i].residueArmed = false;
    }
    CHECK(Ask1("w2r-residue-fires-exactly-once", LevelLoadedLog(), unarmed) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never ran"));
}


// ---------------------------------------------------------------------------------------------
// ARCH-20 S5 -- `ui-gui-begin-gate`. Every arm of S5.3 is driven by a synthetic record, and the
// B1 defect the S5.1 control was written for is reconstructed and shown to red it.
//
// The two atlas dwords are the control's whole discriminating power, so the fixtures carry two
// DISTINCT non-zero constants: a fixture whose expected and observed halves were equal-valued
// could not tell "term 3 held" from "term 3 was never asked".
namespace {

const unsigned kAtlasA = 0x00000800u;   // what GUI.ATLAS_CONST_A holds in .rdata
const unsigned kAtlasB = 0x00000400u;   // GUI.ATLAS_CONST_B -- deliberately not equal to A

// The record a healthy retail launch is predicted to file: the byte set, Begin refused, a live
// ImGui frame behind a base that passed all three terms of S5.1.
StatusFacts GateRecord() {
    StatusFacts s;
    s.guiGateSampled = true;
    s.guiGateAddrResolved = true;
    s.guiGateSamples = 12;
    s.guiSuppressed = 1;
    s.guiHotkeyEnable = 0;
    s.guiReentryDepth = 0;
    s.guiWindowBudget = 0;
    s.guiViewportsEnabled = 0;
    s.guiBeginBalanceDelta = 0;
    s.guiImguiBaseValid = true;
    s.guiAtlasDwordA = kAtlasA;
    s.guiAtlasDwordB = kAtlasB;
    s.guiAtlasExpectA = kAtlasA;
    s.guiAtlasExpectB = kAtlasB;
    s.guiImguiFrameCountDelta = 3;
    s.guiRenderDrawListsFnPresent = 1;
    s.guiRenderedCmdListsCount = 4;
    s.guiRenderedTotalVtxCount = 2160;
    return s;
}

// B1, reconstructed. The previous revision derived `0x142029940` by adding 0xC0 to
// GUI.IMGUI_CONTEXT -- the address OF the pointer -- and treated the result as the context object.
// That address is live engine `.data` (0x54 past ?g_LEngineDefaultPoolId), so it is mapped,
// non-null and readable: terms 1 and 2 of S5.1 both PASS on it. What it cannot do is hold the two
// atlas dwords at +0x20/+0x24 behind its +0x90 pointer, which is what these two observations are.
const unsigned kB1DwordA = 0x00000000u;
const unsigned kB1DwordB = 0x6E69614Du;

}  // namespace

TEST_CASE("ui-gui-begin-gate is could-not-ask before the read side publishes, never a zero") {
    LogFacts f;
    StatusFacts none;                        // the struct's own defaults, i.e. an older record
    CHECK(Ask1("ui-gui-begin-gate", f, none) == Ask::CouldNotAsk);
    CHECK(DetailMentions("ui.gate_probe"));
    CHECK(DetailMentions("Absent is not zero"));

    StatusFacts noEntries = GateRecord();
    noEntries.guiGateSamples = 0;
    CHECK(Ask1("ui-gui-begin-gate", f, noEntries) == Ask::CouldNotAsk);
    CHECK(DetailMentions("frameSiteEntries == 0"));

    StatusFacts unresolved = GateRecord();
    unresolved.guiGateAddrResolved = false;
    strcpy_s(unresolved.guiGateUnresolved, "GUI.BEGIN_BALANCE did not resolve");
    CHECK(Ask1("ui-gui-begin-gate", f, unresolved) == Ask::CouldNotAsk);
    CHECK(DetailMentions("GUI.BEGIN_BALANCE did not resolve"));
}

TEST_CASE("ui-gui-begin-gate scores the four suppressed/delta arms of S5.3") {
    LogFacts f;

    // suppressed == 1 && delta == 0 -- the prediction.
    CHECK(Ask1("ui-gui-begin-gate", f, GateRecord()) == Ask::Pass);
    CHECK(DetailMentions("ASKED-PASS"));
    CHECK(DetailMentions("0x140F912F5"));

    // suppressed == 0 && delta == +1 -- the gate is not the cause.
    StatusFacts h4 = GateRecord();
    h4.guiSuppressed = 0;
    h4.guiBeginBalanceDelta = 1;
    CHECK(Ask1("ui-gui-begin-gate", f, h4) == Ask::Fail);
    CHECK(DetailMentions("DIAG-G2 H4"));

    // suppressed == 0 && delta == 0 -- every conjunct satisfied and the counter did not move.
    StatusFacts contra = GateRecord();
    contra.guiSuppressed = 0;
    CHECK(Ask1("ui-gui-begin-gate", f, contra) == Ask::Fail);
    CHECK(DetailMentions("CONTRADICTION"));

    // [R2, F2] suppressed == 1 && delta == +1 -- the arm the audit added.
    StatusFacts f2 = GateRecord();
    f2.guiBeginBalanceDelta = 1;
    CHECK(Ask1("ui-gui-begin-gate", f, f2) == Ask::Fail);
    CHECK(DetailMentions("INSTRUMENT FAULT, LOUD (F2)"));

    // A step that is neither 0 nor +1 unseats the discriminator rather than being rounded onto an
    // arm: `delta != 0` must not read as "passed the gate".
    StatusFacts two = GateRecord();
    two.guiBeginBalanceDelta = 2;
    CHECK(Ask1("ui-gui-begin-gate", f, two) == Ask::Fail);
    CHECK(DetailMentions("moved by 2"));
}

TEST_CASE("ui-gui-begin-gate defers the suppressed prediction when it is not the engine's to make") {
    LogFacts f;

    // F6: the engine's own per-frame toggle owns the byte while the hotkey is enabled.
    StatusFacts hot = GateRecord();
    hot.guiHotkeyEnable = 1;
    CHECK(Ask1("ui-gui-begin-gate", f, hot) == Ask::CouldNotAsk);
    CHECK(DetailMentions("NOT SCORED (F6)"));
    CHECK(DetailMentions("0x140F9293A"));

    // S0.1's FIRST disjunct. With reentryDepth non-zero, Begin passing with the byte set is the
    // gate working as written -- filing it as F2's instrument fault would be a defect report
    // against our own reading of a documented disjunction.
    StatusFacts reentered = GateRecord();
    reentered.guiReentryDepth = 2;
    reentered.guiBeginBalanceDelta = 1;
    CHECK(Ask1("ui-gui-begin-gate", f, reentered) == Ask::CouldNotAsk);
    CHECK(DetailMentions("FIRST disjunct"));
}

TEST_CASE("ui-gui-begin-gate censors the whole record on a non-zero window budget") {
    LogFacts f;
    // The record is otherwise the asked-PASS one, so this test fails if the fault arm is ordered
    // after the verdict arms rather than before them.
    StatusFacts fault = GateRecord();
    fault.guiWindowBudget = 1;
    CHECK(Ask1("ui-gui-begin-gate", f, fault) == Ask::CouldNotAsk);
    CHECK(DetailMentions("INSTRUMENT FAULT, loud"));
    CHECK(DetailMentions("0x140F92BAF"));
    CHECK_FALSE(DetailMentions("ASKED-PASS"));
}

TEST_CASE("ui-gui-begin-gate: the frame counter not moving overturns S0, and needs a previous") {
    LogFacts f;
    StatusFacts dead = GateRecord();
    dead.guiImguiFrameCountDelta = 0;
    CHECK(Ask1("ui-gui-begin-gate", f, dead) == Ask::Fail);
    CHECK(DetailMentions("OVERTURNED"));

    // One entry has no previous to subtract, so a 0 there is could-not-ask on the delta -- not the
    // loudest alarm in the row fired against arithmetic on a single sample. The gate arms still
    // score, because they do not need a second entry.
    StatusFacts once = GateRecord();
    once.guiGateSamples = 1;
    once.guiImguiFrameCountDelta = 0;
    CHECK(Ask1("ui-gui-begin-gate", f, once) == Ask::Pass);
    CHECK(DetailMentions("frameCountDelta=could-not-ask"));
    CHECK(DetailMentions("no previous to subtract"));

    // A dead counter behind a base that did NOT prove out is not a finding about NewFrame.
    StatusFacts blind = GateRecord();
    blind.guiImguiBaseValid = false;
    blind.guiImguiFrameCountDelta = 0;
    CHECK(Ask1("ui-gui-begin-gate", f, blind) == Ask::Pass);
    CHECK_FALSE(DetailMentions("OVERTURNED"));
}

// â˜… THE MANDATORY CONTROL (S6.2 done-criteria). B1 reconstructed, and the mutant that drops the
// term which catches it.
TEST_CASE("ui-gui-begin-gate: the S5.1 control reds the B1 base, and dropping term 3 reds the row") {
    LogFacts f;

    // (1) The defect as it was: `0x142029940` treated as the context object. Terms 1 and 2 pass on
    // it -- it is mapped, non-null and readable -- so ONLY term 3 can catch it, and it does.
    StatusFacts b1 = GateRecord();
    b1.guiImguiBaseValid = false;
    strcpy_s(b1.guiImguiBaseReason,
             "term 3: atlas dwords behind GUI.IMGUI_CONTEXT+0xC0 (0x142029940) do not match");
    b1.guiAtlasDwordA = kB1DwordA;
    b1.guiAtlasDwordB = kB1DwordB;
    b1.guiRenderDrawListsFnPresent = 1;   // what the unrelated engine global read as: the false PASS
    b1.guiRenderedTotalVtxCount = 99999;
    CHECK(Ask1("ui-gui-begin-gate", f, b1) == Ask::Pass);   // the GATE fields still score
    CHECK(DetailMentions("imguiBaseValid=0"));
    // The four context-relative fields say could-not-ask and NEVER the numbers the wrong base read.
    CHECK(DetailMentions("renderDrawListsFnPresent=could-not-ask"));
    CHECK(DetailMentions("renderedTotalVtxCount=could-not-ask"));
    CHECK_FALSE(DetailMentions("renderDrawListsFnPresent=1"));
    CHECK_FALSE(DetailMentions("99999"));
    // The audit's condition: both observed dwords are on the published line beside the flag, so a
    // reader can see WHY the control refused rather than taking the flag on trust.
    CHECK(DetailMentions("atlasObserved=00000000/6E69614D"));
    CHECK(DetailMentions("atlasExpected=00000800/00000400"));

    // (2) THE MUTANT. A producer that drops term 3 keeps terms 1 and 2 -- both of which the B1
    // base satisfies -- and so publishes `imguiBaseValid = 1` over the very same observations.
    // The row re-runs term 3 off the published pair and refuses the record.
    StatusFacts mutant = b1;
    mutant.guiImguiBaseValid = true;
    mutant.guiImguiBaseReason[0] = '\0';
    CHECK(Ask1("ui-gui-begin-gate", f, mutant) == Ask::CouldNotAsk);
    CHECK(DetailMentions("INSTRUMENT FAULT"));
    CHECK(DetailMentions("does NOT hold on its own published numbers"));
    CHECK(DetailMentions("00000000/6E69614D"));

    // (3) Only the mismatch reds it: the same record with a base that really did pass term 3 is
    // the asked-PASS one, so the check above is not a flag that fires on every valid record.
    CHECK(Ask1("ui-gui-begin-gate", f, GateRecord()) == Ask::Pass);

    // (4) A term 3 that compared 0 against 0 asked nothing. An unread GUI.ATLAS_CONST_A/B is
    // exactly that, and it would otherwise pass the comparison and certify any base at all.
    StatusFacts vacuous = GateRecord();
    vacuous.guiAtlasDwordA = 0;
    vacuous.guiAtlasDwordB = 0;
    vacuous.guiAtlasExpectA = 0;
    vacuous.guiAtlasExpectB = 0;
    CHECK(Ask1("ui-gui-begin-gate", f, vacuous) == Ask::CouldNotAsk);
    CHECK(DetailMentions("compared 0 against 0"));
}

TEST_CASE("ui-gui-begin-gate publishes every field of S5.2 on one line, whatever the verdict") {
    LogFacts f;
    Ask1("ui-gui-begin-gate", f, GateRecord());
    static const char* const kNames[] = {
        "samples=", "suppressed=", "hotkeyEnable=", "reentryDepth=", "windowBudget=",
        "viewportsEnabled=", "beginBalanceDelta=", "imguiBaseValid=", "atlasObserved=",
        "atlasExpected=", "frameCountDelta=", "renderDrawListsFnPresent=",
        "renderedCmdListsCount=", "renderedTotalVtxCount=",
    };
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; ++i) {
        INFO("field ", kNames[i]);
        CHECK(DetailMentions(kNames[i]));
    }
    CHECK(DetailMentions("renderedTotalVtxCount=2160"));
}

TEST_CASE("ui-gui-begin-gate is registered, and its arms are the three its launch runs on") {
    size_t count = 0;
    const RowSpec* rows = Rows(&count);
    const RowSpec* mine = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(rows[i].id, "ui-gui-begin-gate") == 0) { mine = &rows[i]; }
    }
    REQUIRE(mine != nullptr);
    CHECK(mine->arms == (kArmBrowser | kArmPlayback | kArmIdle));
    CHECK(strcmp(mine->filedBy, "W2-R") == 0);

    // A row in the table that EvaluateRow does not dispatch is a row that never runs.
    Ask out = Ask::CouldNotAsk;
    char detail[1024] = {0};
    LogFacts f;
    CHECK(EvaluateRow("ui-gui-begin-gate", f, GateRecord(), &out, detail, sizeof detail));
    CHECK(out == Ask::Pass);
}


// -------------------------------------------------------------------------------------------
// ENG-W2R-P10E -- W2-F's `[facade ...]` group, its two self-named refusals, and the S8.4a fields
// on the record. The whole point of the group is that a line which published nothing says so, so
// every case below is built around the difference between "unread" and "zero".
// -------------------------------------------------------------------------------------------
namespace {

const char* kFullDiag =
    "INVOKED, NEVER DRAWN [ticks owner=100 draws=0] [inframe entries=50 draws=0 refused=7 "
    "nobyte=0 nolatch=0 noctx=0 facade=7 closed=0 fallback=0 installed=1] "
    "[stage0 frameopen samples=100 nonzero=0 lasttick=0] "
    "[facade nodraw=3 beginfalse=4 reason=beginunbound bound=12/17] "
    "[threads owner=4242 callback=4242]";

// What csregister publishes when the composition would have truncated: the refusal, and NOT ONE
// group behind it.
const char* kSuffixRefused =
    "DIAGNOSTIC SUFFIX DOES NOT FIT ITS 634-BYTE BUDGET: the groups are not on this line and no "
    "field of them may be read as absent-means-zero.";

// A StatusFacts in the shape SampleCsRegister leaves it after reading kFullDiag: the struct read
// and the published group agreeing, seven refusals, a named cause.
StatusFacts FacadeFacts() {
    StatusFacts s;
    s.csAsked = true;
    strncpy_s(s.csStatus, sizeof s.csStatus, "Running", _TRUNCATE);
    s.csInFrameKnown = true;
    s.csRefusedByFacade = 7;
    s.csRefusedNoDraw = 3;
    s.csRefusedBeginFalse = 4;
    strncpy_s(s.csBeginRefusal, sizeof s.csBeginRefusal, "beginunbound", _TRUNCATE);
    s.csFacadeBoundEntries = 12;
    s.csFacadeEntryCount = 17;
    s.csInFramePublished = true;
    s.csInFrameLineAgrees = true;
    s.csFacadePublished = true;
    s.csFacadeNoDrawPublished = 3;
    s.csFacadeBeginFalsePublished = 4;
    strncpy_s(s.csFacadeReasonPublished, sizeof s.csFacadeReasonPublished, "beginunbound",
              _TRUNCATE);
    s.csFacadeBoundPublished = 12;
    s.csFacadeEntryCountPublished = 17;
    return s;
}

Ask AskFacade(const StatusFacts& s, char* detail, size_t n) {
    Ask out = Ask::Fail;
    LogFacts f;
    REQUIRE(EvaluateRow("w2f-facade-refusal-names-its-cause", f, s, &out, detail, n));
    return out;
}

}  // namespace

TEST_CASE("the [facade ...] group parses with the publisher's own shape, and refuses a half group") {
    FacadeLine v;
    REQUIRE(ParseFacadeLine(kFullDiag, v));
    CHECK(v.nodraw == 3);
    CHECK(v.beginfalse == 4);
    CHECK(strcmp(v.reason, "beginunbound") == 0);
    CHECK(v.bound == 12);
    CHECK(v.entryCount == 17);

    // A group cut anywhere is refused WHOLE. A half-read group is not a reading, and four fields
    // plus a default is the absent-means-zero answer this parse exists to refuse.
    FacadeLine half;
    CHECK_FALSE(ParseFacadeLine("x [facade nodraw=3 beginfalse=4 reason=beginunbound", half));
    CHECK(half.nodraw == 0);
    CHECK_FALSE(ParseFacadeLine(kSuffixRefused, half));
    CHECK_FALSE(ParseFacadeLine("no group at all [threads owner=1 callback=1]", half));

    // The LAST occurrence, as the two neighbouring groups do it: a sentence that quoted the group
    // must not answer for the module's own trailer.
    std::string quoted = "the panel said '[facade nodraw=99 beginfalse=99 reason=none bound=0/0]' ";
    quoted += kFullDiag;
    FacadeLine last;
    REQUIRE(ParseFacadeLine(quoted.c_str(), last));
    CHECK(last.nodraw == 3);
}

TEST_CASE("both diagnostic refusals are matched BY NAME at the head of the line, never in prose") {
    CHECK(ClassifyDiagnostic(kSuffixRefused) == DiagRefusal::SuffixDoesNotFit);
    CHECK(ClassifyDiagnostic("DIAGNOSTIC PROSE DROPPED (it and the premise exceeded the 1402-byte "
                             "line budget): the status sentence is NOT on this line") ==
          DiagRefusal::ProseDropped);
    CHECK(ClassifyDiagnostic(kFullDiag) == DiagRefusal::None);
    CHECK(ClassifyDiagnostic(nullptr) == DiagRefusal::None);
    // Substring matching is the commonest false pass in this project. A status sentence that
    // QUOTES the phrase is prose, and a reader that censored itself on it would throw away a
    // perfectly good line.
    CHECK(ClassifyDiagnostic("the last line said DIAGNOSTIC PROSE DROPPED and this one did not") ==
          DiagRefusal::None);
    CHECK(strcmp(DiagRefusalWord(DiagRefusal::None), "") == 0);
}

TEST_CASE("w2f-facade-refusal-names-its-cause: a named cause passes, an unnamed one fails") {
    char d[2048] = {0};
    CHECK(AskFacade(FacadeFacts(), d, sizeof d) == Ask::Pass);
    CHECK(strstr(d, "beginunbound") != nullptr);
    CHECK(strstr(d, "bound=12/17") != nullptr);

    // The counter moved and nothing named it. This is the DIAG-G2 gap, and it is a FAIL, not a
    // shrug: `none` is the module's word for "no refusal since the last reset" and seven of them
    // are on the record.
    StatusFacts unnamedCause = FacadeFacts();
    strncpy_s(unnamedCause.csFacadeReasonPublished, sizeof unnamedCause.csFacadeReasonPublished,
              "none", _TRUNCATE);
    strncpy_s(unnamedCause.csBeginRefusal, sizeof unnamedCause.csBeginRefusal, "none", _TRUNCATE);
    CHECK(AskFacade(unnamedCause, d, sizeof d) == Ask::Fail);
    CHECK(strstr(d, "nothing named it") != nullptr);

    StatusFacts behindEnum = FacadeFacts();
    strncpy_s(behindEnum.csFacadeReasonPublished, sizeof behindEnum.csFacadeReasonPublished,
              "unnamed", _TRUNCATE);
    strncpy_s(behindEnum.csBeginRefusal, sizeof behindEnum.csBeginRefusal, "unnamed", _TRUNCATE);
    CHECK(AskFacade(behindEnum, d, sizeof d) == Ask::Fail);

    // Nothing was refused, so there is no cause to name. A pass here would be earned by a build
    // that refuses nothing because it draws nothing.
    StatusFacts quiet = FacadeFacts();
    quiet.csRefusedByFacade = 0;
    quiet.csRefusedNoDraw = 0;
    quiet.csRefusedBeginFalse = 0;
    quiet.csFacadeNoDrawPublished = 0;
    quiet.csFacadeBeginFalsePublished = 0;
    strncpy_s(quiet.csFacadeReasonPublished, sizeof quiet.csFacadeReasonPublished, "none",
              _TRUNCATE);
    strncpy_s(quiet.csBeginRefusal, sizeof quiet.csBeginRefusal, "none", _TRUNCATE);
    CHECK(AskFacade(quiet, d, sizeof d) == Ask::CouldNotAsk);
    CHECK(strstr(d, "no cause to name") != nullptr);
}

TEST_CASE("w2f-facade-refusal-names-its-cause: nodraw+beginfalse != facade is an instrument fault") {
    char d[2048] = {0};
    StatusFacts split = FacadeFacts();
    split.csFacadeBeginFalsePublished = 5;   // 3 + 5 != 7
    split.csRefusedBeginFalse = 5;
    CHECK(AskFacade(split, d, sizeof d) == Ask::CouldNotAsk);
    CHECK(strstr(d, "INSTRUMENT FAULT") != nullptr);
    CHECK(strstr(d, "does NOT hold") != nullptr);

    // The two publications of the halves disagreeing is the same fault from the other side.
    StatusFacts drift = FacadeFacts();
    drift.csFacadeNoDrawPublished = 4;
    drift.csFacadeBeginFalsePublished = 3;   // still sums to 7, but not the struct read's split
    CHECK(AskFacade(drift, d, sizeof d) == Ask::CouldNotAsk);
    CHECK(strstr(d, "INSTRUMENT FAULT") != nullptr);

    // One fact, two holders, two answers.
    StatusFacts twoWords = FacadeFacts();
    strncpy_s(twoWords.csBeginRefusal, sizeof twoWords.csBeginRefusal, "noplacement", _TRUNCATE);
    CHECK(AskFacade(twoWords, d, sizeof d) == Ask::CouldNotAsk);
    CHECK(strstr(d, "two holders") != nullptr);

    // A word outside the published vocabulary is a vocabulary this reader is behind on.
    StatusFacts newWord = FacadeFacts();
    strncpy_s(newWord.csFacadeReasonPublished, sizeof newWord.csFacadeReasonPublished, "clipped",
              _TRUNCATE);
    strncpy_s(newWord.csBeginRefusal, sizeof newWord.csBeginRefusal, "clipped", _TRUNCATE);
    CHECK(AskFacade(newWord, d, sizeof d) == Ask::CouldNotAsk);
    CHECK(strstr(d, "vocabulary this reader is behind on") != nullptr);
}

TEST_CASE("a self-named diagnostic refusal is refused BY NAME, and is not read as an old publisher") {
    char d[2048] = {0};
    StatusFacts refused = FacadeFacts();
    refused.csFacadePublished = false;       // the parse found nothing, because nothing was there
    refused.csFacadeNoDrawPublished = 0;
    refused.csFacadeBeginFalsePublished = 0;
    refused.csFacadeReasonPublished[0] = '\0';
    strncpy_s(refused.csDiagRefusal, sizeof refused.csDiagRefusal,
              DiagRefusalWord(DiagRefusal::SuffixDoesNotFit), _TRUNCATE);
    CHECK(AskFacade(refused, d, sizeof d) == Ask::CouldNotAsk);
    CHECK(strstr(d, "DIAGNOSTIC SUFFIX DOES NOT FIT") != nullptr);
    CHECK(strstr(d, "Nothing on that line is parsed as a value") != nullptr);
    // The wrong diagnosis of the same bytes: the publisher is NOT behind this reader, it refused.
    CHECK(strstr(d, "carried no ' [facade nodraw=...]' group") == nullptr);
    // And the counters are reported, never scored -- a `nodraw=0` verdict off this line is the bug.
    CHECK(strstr(d, "REPORTED, scored by nothing") != nullptr);

    // Same for the prose refusal: the groups behind it are complete, but a line the publisher
    // refused is not a line this row puts a verdict on.
    StatusFacts prose = FacadeFacts();
    strncpy_s(prose.csDiagRefusal, sizeof prose.csDiagRefusal,
              DiagRefusalWord(DiagRefusal::ProseDropped), _TRUNCATE);
    CHECK(AskFacade(prose, d, sizeof d) == Ask::CouldNotAsk);
    CHECK(strstr(d, "DIAGNOSTIC PROSE DROPPED") != nullptr);

    // A publisher that genuinely predates the group gets the OTHER answer, and says so.
    StatusFacts old = FacadeFacts();
    old.csFacadePublished = false;
    CHECK(AskFacade(old, d, sizeof d) == Ask::CouldNotAsk);
    CHECK(strstr(d, "carried no ' [facade nodraw=...]' group") != nullptr);
    CHECK(strstr(d, "DIAGNOSTIC") == nullptr);
}

TEST_CASE("w2f-csregister-epoch names the refusal instead of reporting a broken contract") {
    char d[2048] = {0};
    StatusFacts s = FacadeFacts();
    s.csThreadsPublished = false;   // it is not there, because NOTHING is on that line
    strncpy_s(s.csDiagRefusal, sizeof s.csDiagRefusal,
              DiagRefusalWord(DiagRefusal::SuffixDoesNotFit), _TRUNCATE);
    Ask out = Ask::Pass;
    LogFacts f;
    REQUIRE(EvaluateRow("w2f-csregister-epoch", f, s, &out, d, sizeof d));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(d, "DIAGNOSTIC SUFFIX DOES NOT FIT") != nullptr);
    // W2-F did not break the frozen suffix contract on a line it refused to publish at all.
    CHECK(strstr(d, "did not end in the frozen") == nullptr);

    // With the line intact and the suffix genuinely missing, the contract branch still fires.
    StatusFacts broken = FacadeFacts();
    broken.csThreadsPublished = false;
    REQUIRE(EvaluateRow("w2f-csregister-epoch", f, broken, &out, d, sizeof d));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(d, "did not end in the frozen") != nullptr);
}

TEST_CASE("the record carries the S8.4a fields, and absent means unread rather than zero") {
    const std::string full = CsRegisterObjectText(FacadeFacts());
    CHECK(full.find("\"csregister\": {\"asked\": true") != std::string::npos);
    CHECK(full.find("\"registrationInvocations\":") != std::string::npos);
    CHECK(full.find("\"spacesSeen\":") != std::string::npos);
    CHECK(full.find("\"inframe\":") != std::string::npos);
    CHECK(full.find("\"facade\": 7") != std::string::npos);
    CHECK(full.find("\"facadeReport\": {\"nodraw\": 3, \"beginfalse\": 4, "
                    "\"reason\": \"beginunbound\", \"bound\": 12, \"entryCount\": 17}") !=
          std::string::npos);
    CHECK(full.find("\"facadePublished\":") != std::string::npos);
    CHECK(full.find("\"sumsToFacade\": true") != std::string::npos);
    CHECK(full.substr(full.size() - 3) == "},\n");

    // THE MUTANT THIS CASE EXISTS FOR: a line that refused publishes NO group, and a record that
    // filed `"nodraw": 0` beside the refusal would hand the L5 #6 runner a measured-looking zero
    // off a line that says in words it measured nothing.
    StatusFacts refused = FacadeFacts();
    refused.csFacadePublished = false;
    strncpy_s(refused.csDiagRefusal, sizeof refused.csDiagRefusal,
              DiagRefusalWord(DiagRefusal::SuffixDoesNotFit), _TRUNCATE);
    const std::string ref = CsRegisterObjectText(refused);
    CHECK(ref.find("\"diagnosticRefusal\": \"DIAGNOSTIC SUFFIX DOES NOT FIT\"") !=
          std::string::npos);
    CHECK(ref.find("\"facadePublished\"") == std::string::npos);
    // The STRUCT read is a different source and survives the refusal, which is the whole reason
    // the record carries both.
    CHECK(ref.find("\"facadeReport\":") != std::string::npos);

    // An unread group with no refusal is absent too -- there is no third shape that means zero.
    StatusFacts behind = FacadeFacts();
    behind.csFacadePublished = false;
    const std::string old = CsRegisterObjectText(behind);
    CHECK(old.find("\"facadePublished\"") == std::string::npos);
    CHECK(old.find("\"diagnosticRefusal\"") == std::string::npos);

    // Never asked at all: one field, and not one counter.
    StatusFacts never;
    const std::string none = CsRegisterObjectText(never);
    CHECK(none == "  \"csregister\": {\"asked\": false},\n");

    // The append refuses rather than writing half an object: invalid JSON costs every other row.
    char tiny[64] = {0};
    strncpy_s(tiny, sizeof tiny, "{", _TRUNCATE);
    CHECK_FALSE(AppendCsRegisterObject(tiny, sizeof tiny, FacadeFacts()));
    CHECK(strstr(tiny, "\"nodraw\"") == nullptr);

    char room[4096] = {0};
    CHECK(AppendCsRegisterObject(room, sizeof room, FacadeFacts()));
    CHECK(strstr(room, "\"facadePublished\"") != nullptr);
}

TEST_CASE("w2f-facade-refusal-names-its-cause is registered on the three arms its launch runs") {
    size_t count = 0;
    const RowSpec* rows = Rows(&count);
    const RowSpec* mine = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(rows[i].id, "w2f-facade-refusal-names-its-cause") == 0) {
            mine = &rows[i];
        }
    }
    REQUIRE(mine != nullptr);
    CHECK(mine->arms == (kArmBrowser | kArmPlayback | kArmIdle));
    CHECK(strcmp(mine->filedBy, "W2-F") == 0);

    // A row the table lists and EvaluateRow does not dispatch is a row that never runs.
    Ask out = Ask::Fail;
    char detail[1024] = {0};
    LogFacts f;
    StatusFacts never;
    CHECK(EvaluateRow("w2f-facade-refusal-names-its-cause", f, never, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "was never read") != nullptr);
}


// =================================================================================================
// P10-F. The four rows added for ENG-W2G-P17E S7 and L3-RUN-3 F1/F7. Every fixture below is built
// so the row it drives can FAIL: each case moves ONE fact off the passing shape.
// =================================================================================================
namespace {

StatusFacts CamBattery() {
    StatusFacts s;
    s.camPoseAsked = true;
    snprintf(s.facingDegenerateToken, sizeof s.facingDegenerateToken, "%s", "facing-degenerate");
    snprintf(s.lookAtUnresolvableToken, sizeof s.lookAtUnresolvableToken, "%s",
             "look-at-unresolvable");
    s.camLookYawAsked = 30.0;
    s.camLookPitchAsked = 0.0;
    s.camFacingReadBefore = true;
    s.camYawBefore = 12.0f;
    s.camFacingReadAfter = true;
    s.camYawAfter = 30.02f;

    TransportStep& rel = s.steps[static_cast<size_t>(Verb::CamLookRelative)];
    rel.attempted = rel.submitted = rel.answered = true;
    rel.ok = true;
    snprintf(rel.asked, sizeof rel.asked, "%s", "cam look 10 0");
    snprintf(rel.said, sizeof rel.said, "%s",
             "cam look: at (1.000, 2.000, 3.000), yaw 22.0 pitch 0.0, regime commanded, "
             "source view-state");

    TransportStep& abs = s.steps[static_cast<size_t>(Verb::CamLookAbsolute)];
    abs.attempted = abs.submitted = abs.answered = true;
    abs.ok = true;
    snprintf(abs.asked, sizeof abs.asked, "%s", "cam look to 30 0");
    snprintf(abs.said, sizeof abs.said, "%s",
             "cam look: at (1.000, 2.000, 3.000), yaw 30.0 pitch 0.0, regime commanded, "
             "source view-state");

    TransportStep& at = s.steps[static_cast<size_t>(Verb::CamLookAtSubject)];
    at.attempted = at.submitted = at.answered = true;
    at.ok = false;
    snprintf(at.asked, sizeof at.asked, "%s", "cam look at 1");
    snprintf(at.said, sizeof at.said, "%s",
             "cam look at 1 refused: look-at-unresolvable -- cam look at 1 refused: no-roster");
    return s;
}

StatusFacts RelSeekReply(const char* said, long long landedUs);

// camera_control::SubjectResolveName's five non-Resolved spellings. test_observer links no
// product code, so the strings are carried here -- but what these fixtures are FOR is the
// LENGTHS: `DerivationUncertified` is 103 bytes and used to be copied with _TRUNCATE into a
// char[40], which published a 39-byte prefix that could never match anything the product said.
// PublishSubjectTokens publishes them exactly as probe_replay::Init does, through PublishToken,
// so a buffer that shrinks back reds here instead of silently matching nothing.
const char* const kSubjNoSuchSubject = "no player occupies that selector";
const char* const kSubjRosterUnreadable = "the roster could not be read";
const char* const kSubjNoBody = "that player has no body this tick";
const char* const kSubjNoSeam = "no subject-pose seam in this image";
const char* const kSubjDerivationUncertified =
    "the selector derivation is not certified against this engine yet (select that player once "
    "with cam pov)";

unsigned PublishSubjectTokens(StatusFacts& s) {
    struct Spelling { char* buf; size_t cap; const char* word; };
    const Spelling kAll[] = {
        {s.subjNoSuchSubjectToken, sizeof s.subjNoSuchSubjectToken, kSubjNoSuchSubject},
        {s.subjRosterUnreadableToken, sizeof s.subjRosterUnreadableToken, kSubjRosterUnreadable},
        {s.subjNoBodyToken, sizeof s.subjNoBodyToken, kSubjNoBody},
        {s.subjNoSeamToken, sizeof s.subjNoSeamToken, kSubjNoSeam},
        {s.subjDerivationUncertifiedToken, sizeof s.subjDerivationUncertifiedToken,
         kSubjDerivationUncertified},
    };
    unsigned dropped = 0;
    for (size_t i = 0; i < sizeof kAll / sizeof kAll[0]; ++i) {
        const unsigned len = TokenLen(kAll[i].word);
        if (len > s.subjTokenLongestLen) { s.subjTokenLongestLen = len; }
        if (!PublishToken(kAll[i].buf, kAll[i].cap, kAll[i].word)) { ++dropped; }
    }
    s.subjTokenDropped = dropped;
    return dropped;
}

// p10-cam-9 post-composer: the packing certified, the aim resolved, the engine agreeing.
StatusFacts Cam9() {
    StatusFacts s = CamBattery();
    s.camPackKnown = s.camPackAllPacked = s.camPackDistinct = true;
    s.camPackRefusesZero = s.camPackRefusesEleven = true;
    PublishSubjectTokens(s);
    s.camLookAtSelector = 1;
    // The facing the rig LANDED on, read back from the view after the aim -- the pass arm names
    // it, because `aimed at player 1` in the reply is the product's own claim about itself.
    s.camFacingReadAfter = true;
    s.camYawAfter = 44.0f;
    s.camPitchAfter = -3.0f;
    s.subjectPoseInstalledKnown = true;
    s.subjectPoseInstalled = true;
    TransportStep& st = s.steps[static_cast<size_t>(Verb::CamLookAtSubject)];
    st.attempted = st.submitted = st.answered = st.readBack = true;
    st.ok = true;
    snprintf(st.asked, sizeof st.asked, "%s", "cam look at 1");
    snprintf(st.said, sizeof st.said, "%s",
             "cam look at 1: aimed at player 1 from (1.000, 2.000, 3.000), yaw 44.0 "
             "pitch -3.0, regime commanded, source camera-instance");
    s.camSelectorObserved = true;
    s.camSelectorReadBack = probe_replay::SelectorReadBack::Ok;
    s.camSelectorCertified = true;
    s.camSelectorPacked = 0x0104;
    s.camSelectorExpected = s.camSelectorPacked;   // certified: expected IS what was read
    snprintf(s.camSelectorDerivation, sizeof s.camSelectorDerivation, "%s", "certified");
    return s;
}

StatusFacts Cam9RefusedRaw(const char* said) {
    StatusFacts s = Cam9();
    TransportStep& st = s.steps[static_cast<size_t>(Verb::CamLookAtSubject)];
    st.ok = false;
    s.camSelectorObserved = false;
    s.camSelectorCertified = false;
    snprintf(st.said, sizeof st.said, "%s", said);
    return s;
}

// The sentence camera_control ACTUALLY produces. dispatch wraps its refusal around the module's
// own diagnostic, which is itself a whole `... refused: <cause>` sentence -- so there are TWO
// `refused: ` segments, the first carrying the ControlRefusal enum word and the second the cause.
// A fixture with one segment cannot exercise the reader that has to find the second.
StatusFacts Cam9Cause(const char* causeSpelling) {
    StatusFacts s = Cam9RefusedRaw("");
    TransportStep& st = s.steps[static_cast<size_t>(Verb::CamLookAtSubject)];
    snprintf(st.said, sizeof st.said, "cam look at 1 refused: %s -- cam look at 1 refused: %s",
             s.lookAtUnresolvableToken, causeSpelling);
    return s;
}

void AbsSeekPoint(StatusFacts& s, Verb v, const char* asked, const char* said,
                  long long landedUs) {
    TransportStep& st = s.steps[static_cast<size_t>(v)];
    st.attempted = st.submitted = st.answered = st.readBack = true;
    st.ok = strstr(said, "refused") == nullptr && strstr(said, "landed-off-target") == nullptr;
    st.playheadAfter = landedUs;
    snprintf(st.asked, sizeof st.asked, "%s", asked);
    snprintf(st.said, sizeof st.said, "%s", said);
}

// All four of p10-seek-landing's points, each naming the microsecond it landed on.
StatusFacts FourSeekPoints() {
    StatusFacts s = RelSeekReply("seek +1000000 us from 2133312 us to 3133312 us", 3133312);
    AbsSeekPoint(s, Verb::SeekAbs25, "seek 2500000 us (25% of 10000000 us)",
                 "seek: from 3133312 us to 2500000 us", 2500000);
    AbsSeekPoint(s, Verb::SeekAbs50, "seek 5000000 us (50% of 10000000 us)",
                 "seek: from 2500000 us to 5000000 us", 5000000);
    AbsSeekPoint(s, Verb::SeekAbs90, "seek 9000000 us (90% of 10000000 us)",
                 "seek: from 5000000 us to 9000000 us", 9000000);
    return s;
}

StatusFacts RelSeekReply(const char* said, long long landedUs) {
    StatusFacts s;
    s.batteryDispatched = true;
    s.transportPresent = true;
    s.seekLandingToleranceUs = 33334;
    snprintf(s.seekLandingOffToken, sizeof s.seekLandingOffToken, "%s", "landed-off-target");
    TransportStep& st = s.steps[static_cast<size_t>(Verb::SeekRelForward)];
    st.attempted = st.submitted = st.answered = st.readBack = true;
    st.ok = strstr(said, "refused") == nullptr && strstr(said, "landed-off-target") == nullptr;
    st.target = 1000000;
    st.playheadBefore = 2133312;
    st.playheadAfter = landedUs;
    snprintf(st.asked, sizeof st.asked, "%s", "seek +1000000 us");
    snprintf(st.said, sizeof st.said, "%s", said);
    return s;
}

}  // namespace

TEST_CASE("p10-cam-7 scores the yaw this probe read back, not the reply") {
    LogFacts f;
    char detail[512];
    Ask out = Ask::CouldNotAsk;

    StatusFacts good = CamBattery();
    CHECK(EvaluateRow("p10-cam-7", f, good, &out, detail, sizeof detail));
    CHECK(out == Ask::Pass);
    CHECK(strstr(detail, "30.02") != nullptr);

    // The reply still says yaw 30.0; only the pose we read back moved. A row scoring the sentence
    // would pass here.
    StatusFacts off = CamBattery();
    off.camYawAfter = 35.0f;
    CHECK(Ask1("p10-cam-7", f, off) == Ask::Fail);

    StatusFacts edge = CamBattery();
    edge.camYawAfter = 30.4f;
    CHECK(Ask1("p10-cam-7", f, edge) == Ask::Pass);
    edge.camYawAfter = 30.6f;
    CHECK(Ask1("p10-cam-7", f, edge) == Ask::Fail);

    // Accepted, and the pose could not be read back: could-not-ask, never a landing.
    StatusFacts blind = CamBattery();
    blind.camFacingReadAfter = false;
    CHECK(Ask1("p10-cam-7", f, blind) == Ask::CouldNotAsk);

    StatusFacts vertical = CamBattery();
    vertical.camFacingVerticalAfter = true;
    CHECK(Ask1("p10-cam-7", f, vertical) == Ask::Fail);

    StatusFacts refused = CamBattery();
    refused.steps[static_cast<size_t>(Verb::CamLookAbsolute)].ok = false;
    CHECK(Ask1("p10-cam-7", f, refused) == Ask::Fail);

    StatusFacts unbound = CamBattery();
    unbound.camPoseAsked = false;
    CHECK(Ask1("p10-cam-7", f, unbound) == Ask::CouldNotAsk);

    StatusFacts unasked = CamBattery();
    unasked.steps[static_cast<size_t>(Verb::CamLookAbsolute)].attempted = false;
    CHECK(Ask1("p10-cam-7", f, unasked) == Ask::CouldNotAsk);
}

TEST_CASE("p10-cam-7b scores facing-degenerate by the product's own word") {
    LogFacts f;
    char detail[512];
    Ask out = Ask::CouldNotAsk;

    // The facing was not vertical: the branch was never reached, and the row says so.
    CHECK(EvaluateRow("p10-cam-7b", f, CamBattery(), &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "was not vertical") != nullptr);

    StatusFacts degen = CamBattery();
    degen.camFacingVerticalBefore = true;
    TransportStep& rel = degen.steps[static_cast<size_t>(Verb::CamLookRelative)];
    rel.ok = false;
    snprintf(rel.said, sizeof rel.said, "%s",
             "cam look refused: facing-degenerate -- the facing a yaw would be added to is "
             "vertical");
    CHECK(Ask1("p10-cam-7b", f, degen) == Ask::Pass);

    // Accepted on a vertical facing: there is no yaw axis to have used.
    StatusFacts accepted = degen;
    accepted.steps[static_cast<size_t>(Verb::CamLookRelative)].ok = true;
    CHECK(Ask1("p10-cam-7b", f, accepted) == Ask::Fail);

    // Refused, by some other name.
    StatusFacts other = degen;
    snprintf(other.steps[static_cast<size_t>(Verb::CamLookRelative)].said,
             sizeof other.steps[0].said, "%s", "cam look refused: not-available -- no rig");
    CHECK(Ask1("p10-cam-7b", f, other) == Ask::Fail);

    // The word appears, but only inside the diagnostic. A substring test passes this; the row
    // reads the one place the refusal formatter puts the word, so it fails.
    StatusFacts quoted = degen;
    snprintf(quoted.steps[static_cast<size_t>(Verb::CamLookRelative)].said,
             sizeof quoted.steps[0].said, "%s",
             "cam look refused: pose-unreadable -- not facing-degenerate, the pose would not read");
    CHECK(Ask1("p10-cam-7b", f, quoted) == Ask::Fail);

    // THE ANTI-HARDCODE CONTROL: the product renames the refusal and the probe carries the new
    // word. A row with its own spelling of `facing-degenerate` fails this case.
    StatusFacts renamed = degen;
    snprintf(renamed.facingDegenerateToken, sizeof renamed.facingDegenerateToken, "%s",
             "facing-has-no-yaw-axis");
    snprintf(renamed.steps[static_cast<size_t>(Verb::CamLookRelative)].said,
             sizeof renamed.steps[0].said, "%s",
             "cam look refused: facing-has-no-yaw-axis -- vertical");
    CHECK(Ask1("p10-cam-7b", f, renamed) == Ask::Pass);

    StatusFacts noToken = degen;
    noToken.facingDegenerateToken[0] = 0;
    CHECK(Ask1("p10-cam-7b", f, noToken) == Ask::CouldNotAsk);

    StatusFacts unread = degen;
    unread.camFacingReadBefore = false;
    CHECK(Ask1("p10-cam-7b", f, unread) == Ask::CouldNotAsk);
}

TEST_CASE("p10-cam-9 scores the packing and the aim, post-composer") {
    LogFacts f;
    char detail[2048];
    Ask out = Ask::CouldNotAsk;

    // The aim resolved and the ENGINE certifies it. This is the arm the row could not reach
    // before ENG-W1E-SUBJECT `eb31bd4` installed the composer.
    CHECK(EvaluateRow("p10-cam-9", f, Cam9(), &out, detail, sizeof detail));
    CHECK(out == Ask::Pass);
    CHECK(strstr(detail, "ENGINE certifies") != nullptr);
    CHECK(strstr(detail, "260") != nullptr);            // 0x104, the packed word, in the sentence
    CHECK(strstr(detail, "ten distinct words") != nullptr);

    // The pass arm carries the facing the rig LANDED on, read back from the view.
    CHECK(strstr(detail, "LANDED facing yaw 44.0 deg, pitch -3.0 deg") != nullptr);
    StatusFacts noFacing = Cam9();
    noFacing.camFacingReadAfter = false;
    CHECK(Ask1("p10-cam-9", f, noFacing) == Ask::CouldNotAsk);

    // ***** TRAP A, and its control. The published token has to hold the whole spelling: 103
    // bytes for DerivationUncertified. PublishToken REFUSES a buffer that cannot, so the row
    // reports an unpublished spelling; the _TRUNCATE copy it replaced published a 39-byte prefix
    // that matched nothing and read as "the product never said that word".
    {
        StatusFacts sized = Cam9();
        CHECK(PublishSubjectTokens(sized) == 0);
        CHECK(strcmp(sized.subjDerivationUncertifiedToken, kSubjDerivationUncertified) == 0);
        CHECK(strcmp(sized.subjNoSeamToken, kSubjNoSeam) == 0);
        CHECK(TokenLen(kSubjDerivationUncertified) == 103);
        char small[40] = {0};
        CHECK(PublishToken(small, sizeof small, kSubjDerivationUncertified) == false);
        CHECK(small[0] == 0);                    // refused, never a prefix
        CHECK(PublishToken(small, sizeof small, kSubjRosterUnreadable));
    }

    // ***** TRAP B, and its control. RefusalWordIs reads the FIRST `refused: `, whose payload is
    // the ControlRefusal enum word -- correct for p10-cam-7b and blind to the cause, which is
    // after the second. SubjectCauseIs reads the last one. The four CHECKs pin both directions.
    {
        StatusFacts nested = Cam9Cause(kSubjDerivationUncertified);
        const char* said = nested.steps[static_cast<size_t>(Verb::CamLookAtSubject)].said;
        CHECK(RefusalWordIs(said, nested.lookAtUnresolvableToken));
        CHECK(RefusalWordIs(said, kSubjDerivationUncertified) == false);
        CHECK(SubjectCauseIs(said, kSubjDerivationUncertified));
        CHECK(SubjectCauseIs(said, nested.lookAtUnresolvableToken) == false);
    }

    // ★ THE SPLIT. `look-at-unresolvable` is five causes wearing one word, and each one scores
    // differently. Certification is per session and attended, so nobody asked this launch.
    StatusFacts pending = Cam9Cause(kSubjDerivationUncertified);
    CHECK(EvaluateRow("p10-cam-9", f, pending, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "CERTIFICATION PENDING") != nullptr);
    CHECK(strstr(detail, "cam pov") != nullptr);

    // An UNINSTALLED seam is a build state, not this product failing to aim.
    StatusFacts noSeam = Cam9Cause(kSubjNoSeam);
    CHECK(EvaluateRow("p10-cam-9", f, noSeam, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "UNINSTALLED seam is a build state") != nullptr);

    // An empty slot with a close whose roster recorded nobody is this session having one player...
    StatusFacts emptyHost = Cam9Cause(kSubjNoSuchSubject);
    emptyHost.sentinelRosterRowsKnown = true;
    emptyHost.sentinelRosterRows = 0;
    CHECK(EvaluateRow("p10-cam-9", f, emptyHost, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "recorded 0 row(s)") != nullptr);

    // ... and the SAME refusal over a slot that close's roster DID record is an asked-fail.
    StatusFacts occupied = emptyHost;
    occupied.sentinelRosterRows = 1;
    CHECK(EvaluateRow("p10-cam-9", f, occupied, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "ASKED AND FAILED") != nullptr);
    CHECK(strstr(detail, "No such subject") != nullptr);

    StatusFacts rosterDead = Cam9Cause(kSubjRosterUnreadable);
    rosterDead.sentinelRosterRowsKnown = true;
    rosterDead.sentinelRosterRows = 2;
    CHECK(EvaluateRow("p10-cam-9", f, rosterDead, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "An unreadable roster") != nullptr);

    // No close has published a roster count at all: occupancy is unknown, not zero.
    StatusFacts noRoster = Cam9Cause(kSubjNoSuchSubject);
    CHECK(EvaluateRow("p10-cam-9", f, noRoster, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "no capture close has published a roster row count") != nullptr);

    // A body that is not in the world on the sampled tick is not an aim this row can score.
    CHECK(Ask1("p10-cam-9", f, Cam9Cause(kSubjNoBody)) == Ask::CouldNotAsk);

    // A cause naming none of the five is a fail, and so is a refusal that is not the enum word.
    StatusFacts strange = Cam9Cause("the sky fell on the rig");
    CHECK(EvaluateRow("p10-cam-9", f, strange, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "names NONE of camera_control's") != nullptr);
    CHECK(Ask1("p10-cam-9", f, Cam9RefusedRaw("cam look at 1 refused: not-available -- no rig")) ==
          Ask::Fail);

    // The state a char[40] leaves behind is REPORTED, never scored: an unpublished spelling must
    // not read as "the product never refused for that cause".
    StatusFacts truncated = Cam9Cause(kSubjDerivationUncertified);
    truncated.subjDerivationUncertifiedToken[0] = 0;
    truncated.subjTokenDropped = 1;
    truncated.subjTokenLongestLen = TokenLen(kSubjDerivationUncertified);
    CHECK(EvaluateRow("p10-cam-9", f, truncated, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "did not fit their published buffers") != nullptr);
    CHECK(strstr(detail, "103 bytes") != nullptr);

    // The aim is scored from the ENGINE, never from Result::ok or from our own sentence.
    StatusFacts unread = Cam9();
    unread.camSelectorObserved = false;
    unread.camSelectorCertified = false;
    snprintf(unread.camSelectorDerivation, sizeof unread.camSelectorDerivation, "%s",
             "uncertified");
    CHECK(EvaluateRow("p10-cam-9", f, unread, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "could not be read back") != nullptr);

    // `uncertified` contains `certified`: a row matching the WORD would pass this.
    StatusFacts wordOnly = Cam9();
    wordOnly.camSelectorCertified = false;
    snprintf(wordOnly.camSelectorDerivation, sizeof wordOnly.camSelectorDerivation, "%s",
             "uncertified");
    CHECK(Ask1("p10-cam-9", f, wordOnly) == Ask::CouldNotAsk);

    StatusFacts falsified = Cam9();
    falsified.camSelectorCertified = false;
    falsified.camSelectorFalsified = true;
    falsified.camSelectorPacked = 0x0203;
    CHECK(EvaluateRow("p10-cam-9", f, falsified, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "FALSIFIED") != nullptr);

    // Half A, and it is scored BEFORE the aim: each of these carries a fully certified aim.
    StatusFacts collide = Cam9();
    collide.camPackDistinct = false;
    CHECK(EvaluateRow("p10-cam-9", f, collide, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "not all different") != nullptr);

    StatusFacts unbounded = Cam9();
    unbounded.camPackRefusesZero = false;
    CHECK(EvaluateRow("p10-cam-9", f, unbounded, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "PackSelector accepted 0") != nullptr);

    StatusFacts bothEnds = Cam9();
    bothEnds.camPackRefusesZero = bothEnds.camPackRefusesEleven = false;
    CHECK(EvaluateRow("p10-cam-9", f, bothEnds, &out, detail, sizeof detail));
    CHECK(strstr(detail, "both 0 and max+1") != nullptr);

    StatusFacts halfPacked = Cam9();
    halfPacked.camPackAllPacked = false;
    CHECK(Ask1("p10-cam-9", f, halfPacked) == Ask::Fail);

    // Never asked is not a fail, and no published word is not a spelling of our own.
    StatusFacts notAsked = Cam9();
    notAsked.camPackKnown = false;
    CHECK(Ask1("p10-cam-9", f, notAsked) == Ask::CouldNotAsk);

    StatusFacts noWord = Cam9();
    noWord.subjNoSuchSubjectToken[0] = 0;
    CHECK(Ask1("p10-cam-9", f, noWord) == Ask::CouldNotAsk);

    StatusFacts noRig = Cam9();
    noRig.camPoseAsked = false;
    CHECK(Ask1("p10-cam-9", f, noRig) == Ask::CouldNotAsk);
}

// DIAG-PROBE-CRASH (L3-RUN-28). `probe_replay.dll+0x60cf0` = RunBattery @ probe_replay.cpp:1345,
// called from `+0x5ca0b` = ObserverWorker @ probe_replay.cpp:2052, `access=read va=0x0`, on the
// probe's own CreateThread worker (tid 46952). The line was
// `cc::Get()->ObservedSelector(&packed, &cameraIndex)`, and Get() answers null the moment
// camera_module unbinds -- which the engine log's `[LEVELLOAD] Loading level` in that same second
// is. Both halves are asserted here: the read-back must not dereference the null, and the row must
// say WHICH could-not-ask this is.
TEST_CASE("W2-R cam: the selector read-back survives a rig released under it (L3-RUN-28)") {
    struct FakeControl {
        bool answer = true;
        int32_t word = 0;
        uint16_t camera = 0;
        mutable int calls = 0;
        bool ObservedSelector(int32_t* packed, uint16_t* cameraIndex) const {
            ++calls;
            if (!answer) return false;
            *packed = word;
            *cameraIndex = camera;
            return true;
        }
    };

    // The word the fake answers with is the fixture's OWN certified word, not a second spelling of
    // it; the two sentinels are values it cannot produce, so "left alone" is decidable.
    const int32_t kAnswer = static_cast<int32_t>(Cam9().camSelectorPacked);
    const uint16_t kAnswerCamera = 7;
    const int32_t kUnwritten = -1;         // no packed selector word is negative
    const uint16_t kUnwrittenCamera = 3;   // and it is not the camera the fake reports

    int32_t packed = kUnwritten;
    uint16_t cameraIndex = kUnwrittenCamera;

    // The crash condition itself. A null control is a RESULT here, and nothing is dereferenced.
    CHECK(probe_replay::ReadObservedSelector(static_cast<const FakeControl*>(nullptr), &packed,
                                             &cameraIndex) ==
          probe_replay::SelectorReadBack::RigReleased);
    CHECK(packed == kUnwritten);   // the out-params are left alone: there was no answer to write
    CHECK(cameraIndex == kUnwrittenCamera);
    CHECK(strcmp(probe_replay::SelectorReadBackName(probe_replay::SelectorReadBack::RigReleased),
                 "rig-released") == 0);

    // A rig that is there and declines is a DIFFERENT could-not-ask, and must not read as the loss.
    FakeControl declines;
    declines.answer = false;
    CHECK(probe_replay::ReadObservedSelector(&declines, &packed, &cameraIndex) ==
          probe_replay::SelectorReadBack::NoAnswer);
    CHECK(declines.calls == 1);
    CHECK(packed == kUnwritten);

    FakeControl answers;
    answers.word = kAnswer;
    answers.camera = kAnswerCamera;
    CHECK(probe_replay::ReadObservedSelector(&answers, &packed, &cameraIndex) ==
          probe_replay::SelectorReadBack::Ok);
    CHECK(packed == kAnswer);
    CHECK(cameraIndex == kAnswerCamera);

    // The row. The aim was ACCEPTED and the rig then went away: could-not-ask, named as the rig
    // loss -- not as "the engine's word could not be read back", which is the other cause.
    LogFacts f;
    char detail[1024];
    Ask out = Ask::Fail;
    StatusFacts released = Cam9();
    released.camSelectorObserved = false;
    released.camSelectorCertified = false;
    released.camSelectorReadBack = probe_replay::SelectorReadBack::RigReleased;
    CHECK(EvaluateRow("p10-cam-9", f, released, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "RELEASED before the read-back") != nullptr);
    CHECK(strstr(detail, "rig-released") != nullptr);
    CHECK(strstr(detail, "could not be read back") == nullptr);

    // And the engine declining still reads as the engine declining.
    StatusFacts declined = Cam9();
    declined.camSelectorObserved = false;
    declined.camSelectorCertified = false;
    declined.camSelectorReadBack = probe_replay::SelectorReadBack::NoAnswer;
    CHECK(EvaluateRow("p10-cam-9", f, declined, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "could not be read back") != nullptr);
    CHECK(strstr(detail, "RELEASED before the read-back") == nullptr);
}
// The SAME rig loss one step earlier: it reaches the row as dispatch's own
// "the spectator rig has not checked out", a word the composer never issued, and the pre-fix chain
// scored that ASKED-FAIL -- accusing the product of a refusal nobody made, for the exact loss
// L3-RUN-28 recorded. One loss, one named deferral, wherever in the arm it lands.
TEST_CASE("W2-R cam: a rig released across `cam look at` is the same named deferral (AUD F1)") {
    LogFacts f;
    char detail[1024];
    Ask out = Ask::Pass;

    // dispatch.cpp:725/801/849 -- the sentence verbatim, and it carries no composer word.
    const char* kRigGone =
        "cam look at 1: refused: the spectator rig has not checked out, so no camera mode can be "
        "selected";

    StatusFacts released = Cam9();
    TransportStep& st = released.steps[static_cast<size_t>(Verb::CamLookAtSubject)];
    st.ok = false;
    snprintf(st.said, sizeof st.said, "%s", kRigGone);
    released.camSelectorObserved = false;
    released.camSelectorCertified = false;
    released.camRigAtLookAt = probe_replay::RigPresence::Released;
    CHECK(EvaluateRow("p10-cam-9", f, released, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "RELEASED across") != nullptr);
    CHECK(strstr(detail, "rig-released") != nullptr);
    // The accusation the pre-fix chain made, and it must be gone.
    CHECK(strstr(detail, "a refusal nobody in this row's chain issued") == nullptr);

    // The observation is what settles it, not the sentence: an UNOBSERVED rig with the very same
    // refusal text is still the composer finding, so this fix cannot be swallowing real failures.
    StatusFacts unobserved = released;
    unobserved.camRigAtLookAt = probe_replay::RigPresence::NotAsked;
    CHECK(EvaluateRow("p10-cam-9", f, unobserved, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "a refusal nobody in this row's chain issued") != nullptr);

    // And a rig that WAS present across the step leaves every composer arm exactly as it was.
    StatusFacts present = released;
    present.camRigAtLookAt = probe_replay::RigPresence::Present;
    CHECK(Ask1("p10-cam-9", f, present) == Ask::Fail);

    // Both ends are asked, because a release DURING the dispatch is the case that produces the
    // wrong-word refusal and only the after-sample can see it.
    CHECK(probe_replay::RigAcrossStep(true, true) == probe_replay::RigPresence::Present);
    CHECK(probe_replay::RigAcrossStep(true, false) == probe_replay::RigPresence::Released);
    CHECK(probe_replay::RigAcrossStep(false, true) == probe_replay::RigPresence::Released);
    CHECK(strcmp(probe_replay::RigPresenceName(probe_replay::RigPresence::Released),
                 "rig-released") == 0);
}

// Shutdown sets g_stop and joins with a bound; the battery's two waits are
// 4000 ms each. A wait that cannot hear the stop makes the join expire by construction and leaves
// the worker calling through g_services after the module has torn down.
TEST_CASE("W2-R cam: the bind wait abandons on teardown instead of burning its budget (AUD F2)") {
    long long clock = 0;
    long long stopAt = 400; // facts-ok: AUDIO.LISTENER_COUNT_OFF a clock reading in ms, not that offset
    int asks = 0;

    const probe_replay::CamBindWait tornDown = probe_replay::AwaitCameraBind(
        [&]() { ++asks; return false; },                 // the rig never binds
        [&](long long ms) { clock += ms; }, 4000, 100,
        [&]() { return clock >= stopAt; });
    CHECK(tornDown.stopped);
    CHECK_FALSE(tornDown.bound);
    // Promptly: within one poll of the stop, and nowhere near the 4000 ms budget the join is
    // measured against.
    CHECK(tornDown.waitedMs <= stopAt);
    CHECK(tornDown.waitedMs < tornDown.budgetMs);

    // A stop that never comes still waits its whole budget -- so `stopped` cannot be a constant.
    long long slept = 0;
    const probe_replay::CamBindWait never = probe_replay::AwaitCameraBind(
        []() { return false; }, [&](long long ms) { slept += ms; }, 4000, 100,
        []() { return false; });
    CHECK_FALSE(never.stopped);
    CHECK(never.waitedMs == 4000);

    // A rig that binds is not reported as a teardown.
    const probe_replay::CamBindWait bound = probe_replay::AwaitCameraBind(
        []() { return true; }, [](long long) {}, 4000, 100, []() { return false; });
    CHECK(bound.bound);
    CHECK_FALSE(bound.stopped);

    // The overload without a stop predicate is the old behaviour, unchanged.
    const probe_replay::CamBindWait plain = probe_replay::AwaitCameraBind(
        []() { return false; }, [](long long) {}, 4000, 100);
    CHECK_FALSE(plain.stopped);
    CHECK(plain.waitedMs == plain.budgetMs);
}

// L3-RUN-6 F5. The residue row printed the effect SUM, and "30" reads the same whether the
// battery reset the CI thirty times or woke thirty bodies. These two batteries carry the SAME
// total and differ only in ciResets: a sentence that names only the total renders them
// identically, so the two detail CHECKs below cannot both hold unless it names ciResets.
TEST_CASE("w2r-residue-fires-on-discontinuity names ciResets, not just the effect total (F5)") {
    LogFacts f;
    char detail[1024];
    Ask out = Ask::CouldNotAsk;
    static const Verb kJumps[] = {Verb::Seek, Verb::StepForward, Verb::StepBack, Verb::Round,
                                  Verb::SeekAway, Verb::Goto};

    StatusFacts ci;          // six effects moved, and every one of them was a CI reset
    ci.batteryDispatched = true;
    ci.durationUs = 80000000;
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        TransportStep& st = ci.steps[static_cast<size_t>(kJumps[i])];
        st.attempted = st.submitted = st.answered = st.ok = true;
        st.residueArmed = true;
        st.residueCauseAfter = 0;
        st.residueCauseCount = 6;
        snprintf(st.asked, sizeof st.asked, "%s", VerbName(kJumps[i]));
        st.residueBefore = 0;
        st.residueAfter = 1;
        st.residueCiResetsBefore = 0;
        st.residueCiResetsAfter = 1;
        st.residueFiresBefore = 7;
        st.residueFiresAfter = 8;
        st.residueSeekFiresBefore = 3;
        st.residueSeekFiresAfter = 4;
    }
    CHECK(EvaluateRow("w2r-residue-fires-on-discontinuity", f, ci, &out, detail, sizeof detail));
    CHECK(out == Ask::Pass);
    CHECK(strstr(detail, "6 in total, of which 6 ciResets") != nullptr);
    CHECK(strstr(detail, "6 fire(s), 6 of them Cause::Seek") != nullptr);

    StatusFacts wakes = ci;  // the same six effects, and NONE of them a CI reset
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        TransportStep& st = wakes.steps[static_cast<size_t>(kJumps[i])];
        st.residueCiResetsAfter = st.residueCiResetsBefore;
    }
    CHECK(EvaluateRow("w2r-residue-fires-on-discontinuity", f, wakes, &out, detail, sizeof detail));
    CHECK(out == Ask::Pass);
    CHECK(strstr(detail, "6 in total, of which 0 ciResets") != nullptr);

    // The asked-fail arm carries the same breakdown: a partial answer has to say which effect.
    StatusFacts partial = ci;
    partial.steps[static_cast<size_t>(Verb::Goto)].residueAfter =
        partial.steps[static_cast<size_t>(Verb::Goto)].residueBefore;
    partial.steps[static_cast<size_t>(Verb::Goto)].residueCiResetsAfter =
        partial.steps[static_cast<size_t>(Verb::Goto)].residueCiResetsBefore;
    CHECK(EvaluateRow("w2r-residue-fires-on-discontinuity", f, partial, &out, detail,
                      sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "5 in total, of which 5 ciResets") != nullptr);
    CHECK(strstr(detail, "of them Cause::Seek") != nullptr);
}

// L3-RUN-6 section 1 item (8) / F1, in the launch's own numbers. The capture's t0 was 18899811 us
// and a round-begin marker sat at absolute 1874427 us; the OLD writer recorded the marker as
// absolute minus t0 and the round rows differenced that against a playhead based at the file's
// own start, reading the seventeen-second gap between two clocks as a landing error. The rebase
// is W2-A's (F1 rules so). What these rows must do is refuse to score across two bases.
TEST_CASE("W2-R round rows: a marker on another base is NAMED, not differenced (F1)") {
    LogFacts f;
    char detail[1024];
    Ask out = Ask::CouldNotAsk;

    const long long kT0Us = 18899811;
    const long long kMarkerAbsUs = 1874427;
    const long long kOldWriterUs = kMarkerAbsUs - kT0Us;
    CHECK(kOldWriterUs == -17025384);          // the number the run recorded, derived not typed

    StatusFacts s = WorkingBattery();
    s.roundsKnown = true;
    s.roundCount = 3;
    s.tickSpacingUs = facts::CLOCK_FIXEDSTEP_30HZ_US;   // this capture's spacing
    s.seekToleranceUs = 2 * s.tickSpacingUs + 1;        // two of them, rounded up
    TransportStep& rd = s.steps[static_cast<size_t>(Verb::Round)];
    rd.attempted = rd.submitted = rd.answered = rd.readBack = rd.ok = true;
    snprintf(rd.asked, sizeof rd.asked, "%s", "round 1");
    snprintf(rd.said, sizeof rd.said, "%s", "round 1 at 1874427 us");
    rd.playheadBefore = 40000000;
    rd.playheadAfter = kMarkerAbsUs;           // the playhead IS file-relative and lands on it
    rd.target = kOldWriterUs;                  // ... and the marker is not

    CHECK(EvaluateRow("w2r-round-lands-on-round", f, s, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "OUTSIDE this file's span") != nullptr);
    CHECK(strstr(detail, "-17025384") != nullptr);
    CHECK(strstr(detail, "17025384 us that fall out") != nullptr);

    CHECK(EvaluateRow("w2c-goto-round-lands", f, s, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "OUTSIDE this file's span") != nullptr);

    // The NEW writer records the same marker file-relative, and both rows land on it.
    StatusFacts rebased = s;
    rebased.steps[static_cast<size_t>(Verb::Round)].target = kMarkerAbsUs;
    CHECK(Ask1("w2r-round-lands-on-round", f, rebased) == Ask::Pass);
    CHECK(Ask1("w2c-goto-round-lands", f, rebased) == Ask::Pass);

    // The other half of the same shape: a marker left on the ABSOLUTE clock, past the end of a
    // file whose own duration is t0-relative. 68.333 s of capture, 49.432839 s of playback.
    StatusFacts absolute = rebased;
    absolute.durationUs = 49432839;
    absolute.steps[static_cast<size_t>(Verb::Round)].target = 68333000;
    CHECK(EvaluateRow("w2r-round-lands-on-round", f, absolute, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "OUTSIDE this file's span") != nullptr);
    CHECK(Ask1("w2c-goto-round-lands", f, absolute) == Ask::Fail);

    // The guard must not fail true statements: a duration nobody published bounds nothing, and a
    // correctly rebased marker still lands.
    StatusFacts noSpan = rebased;
    noSpan.durationUs = 0;
    CHECK(Ask1("w2r-round-lands-on-round", f, noSpan) == Ask::Pass);
    CHECK(Ask1("w2c-goto-round-lands", f, noSpan) == Ask::Pass);
}

TEST_CASE("p10-seek-landing compares the sentence against the playhead") {
    LogFacts f;
    char detail[1024];
    Ask out = Ask::CouldNotAsk;

    StatusFacts exact = RelSeekReply("seek +1000000 us from 2133312 us to 3133312 us", 3133312);
    CHECK(EvaluateRow("p10-seek-landing", f, exact, &out, detail, sizeof detail));
    CHECK(out == Ask::Pass);
    // The row is now an aggregate over four seek points and this fixture drives one of them, so
    // the pass says so instead of reading like a pass over the transport.
    CHECK(strstr(detail, "1 of 4 seek point(s)") != nullptr);

    // The point's own sentence still names the microsecond it claimed and the one it read back --
    // it reaches the record through the failing point's text rather than through a one-point pass.
    StatusFacts wrong = RelSeekReply("seek +1000000 us from 2133312 us to 3133312 us", 2255533);
    CHECK(EvaluateRow("p10-seek-landing", f, wrong, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "3133312 us") != nullptr);
    CHECK(strstr(detail, "2255533 us") != nullptr);

    // L3-RUN-3 F1 itself: the sentence echoes the arithmetic and the playhead is elsewhere.
    StatusFacts f1 = RelSeekReply("seek +10000000 us from 2133312 us to 12133312 us", 2255533);
    CHECK(Ask1("p10-seek-landing", f, f1) == Ask::Fail);

    // A difference inside the product's own nearest-tick window is not scored either way.
    StatusFacts nearMiss = RelSeekReply("seek +1000000 us from 2133312 us to 3133312 us", 3122201);
    CHECK(Ask1("p10-seek-landing", f, nearMiss) == Ask::CouldNotAsk);
    StatusFacts past = RelSeekReply("seek +1000000 us from 2133312 us to 3133312 us", 3033312);
    CHECK(Ask1("p10-seek-landing", f, past) == Ask::Fail);

    // The interval now comes from IReplayTransport::TickSpacingUs, and a transport
    // that will not publish one leaves it 0. Zero is UNKNOWN, not a tolerance of zero: the same
    // near miss that is not scored above must not become a Fail merely because nobody could be
    // asked. The 33334 this row used to carry was a deleted control constant.
    StatusFacts noSpacing = nearMiss;
    REQUIRE(nearMiss.seekLandingToleranceUs > 0);
    noSpacing.seekLandingToleranceUs = 0;
    CHECK(Ask1("p10-seek-landing", f, noSpacing) == Ask::CouldNotAsk);
    // And an exact landing is still a Pass with no tolerance in hand -- could-not-ask must not
    // swallow the answers the row CAN give.
    StatusFacts exactHit = RelSeekReply("seek +1000000 us from 2133312 us to 3133312 us", 3133312);
    exactHit.seekLandingToleranceUs = 0;
    CHECK(Ask1("p10-seek-landing", f, exactHit) == Ask::Pass);

    StatusFacts refused =
        RelSeekReply("seek +1000000 us: landed-off-target -- from 2133312 us this asked for "
                     "3133312 us and the playhead is at 2255533 us (-877779 us)", 2255533);
    CHECK(refused.steps[static_cast<size_t>(Verb::SeekRelForward)].ok == false);
    CHECK(Ask1("p10-seek-landing", f, refused) == Ask::Pass);

    StatusFacts otherRefusal = RelSeekReply("seek refused: no file is loaded", 2133312);
    CHECK(Ask1("p10-seek-landing", f, otherRefusal) == Ask::CouldNotAsk);

    StatusFacts silent = RelSeekReply("seek done", 3133312);
    CHECK(Ask1("p10-seek-landing", f, silent) == Ask::Fail);

    StatusFacts renamed =
        RelSeekReply("seek +1000000 us: went-somewhere-else -- the playhead is at 2255533 us",
                     2255533);
    renamed.steps[static_cast<size_t>(Verb::SeekRelForward)].ok = false;
    snprintf(renamed.seekLandingOffToken, sizeof renamed.seekLandingOffToken, "%s",
             "went-somewhere-else");
    CHECK(Ask1("p10-seek-landing", f, renamed) == Ask::Pass);

    StatusFacts noToken = exact;
    noToken.seekLandingOffToken[0] = 0;
    CHECK(Ask1("p10-seek-landing", f, noToken) == Ask::CouldNotAsk);
}

// L3-RUN-4 s6. The row used to have ONE point and no denominator: `exact` above is a pass
// over a single relative nudge, and it read like a pass over the transport.
TEST_CASE("p10-seek-landing scores four seeks and publishes its denominator") {
    LogFacts f;
    char detail[2048];
    Ask out = Ask::CouldNotAsk;

    CHECK(EvaluateRow("p10-seek-landing", f, FourSeekPoints(), &out, detail, sizeof detail));
    CHECK(out == Ask::Pass);
    CHECK(strstr(detail, "all 4 seek point(s)") != nullptr);

    // One absolute jump lying about where it landed reds the row -- the whole point of the
    // three new points, since the relative nudge is still perfectly honest here.
    StatusFacts oneLies = FourSeekPoints();
    AbsSeekPoint(oneLies, Verb::SeekAbs50, "seek 5000000 us (50% of 10000000 us)",
                 "seek: from 2500000 us to 5000000 us", 1234567);
    CHECK(EvaluateRow("p10-seek-landing", f, oneLies, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "of 4 seek point(s)") != nullptr);
    CHECK(strstr(detail, "seek <50% of duration>") != nullptr);

    // Each of the other two, so the sweep is not carried by one point.
    StatusFacts lateLies = FourSeekPoints();
    AbsSeekPoint(lateLies, Verb::SeekAbs90, "seek 9000000 us (90% of 10000000 us)",
                 "seek: from 5000000 us to 9000000 us", 4000000);
    CHECK(Ask1("p10-seek-landing", f, lateLies) == Ask::Fail);
    StatusFacts earlyLies = FourSeekPoints();
    AbsSeekPoint(earlyLies, Verb::SeekAbs25, "seek 2500000 us (25% of 10000000 us)",
                 "seek: from 3133312 us to 2500000 us", 9999999);
    CHECK(Ask1("p10-seek-landing", f, earlyLies) == Ask::Fail);

    // A run in which only the relative nudge was scoreable still passes -- and SAYS SO,
    // rather than reading like a pass over four.
    StatusFacts oneOnly =
        RelSeekReply("seek +1000000 us from 2133312 us to 3133312 us", 3133312);
    CHECK(EvaluateRow("p10-seek-landing", f, oneOnly, &out, detail, sizeof detail));
    CHECK(out == Ask::Pass);
    CHECK(strstr(detail, "1 of 4 seek point(s)") != nullptr);
    CHECK(strstr(detail, "3 could not be scored") != nullptr);
    CHECK(strstr(detail, "all 4 seek point(s)") == nullptr);

    StatusFacts none;
    CHECK(EvaluateRow("p10-seek-landing", f, none, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "none of the 4 seek point(s)") != nullptr);
}

// L3-RUN-4 F2. Both records from that run were snapshots taken ~250 ms before the file
// loaded, and 39 rows stated the absence of events that then happened -- in the indicative.
TEST_CASE("a provisional record's could-not-ask carries its horizon") {
    char d[512];
    snprintf(d, sizeof d, "%s", "no [identity] 'roster:' line, so the roster path was never reached");
    AppendProvisionalHorizon(d, sizeof d, 21185.0, 4500000);
    CHECK(strstr(d, "AS OF THE LAST HEARTBEAT AT 21185 ms") != nullptr);
    CHECK(strstr(d, "playhead 4500000 us") != nullptr);
    CHECK(strstr(d, "had not happened YET") != nullptr);
    // The row's own reason survives; the horizon qualifies it, it does not replace it.
    CHECK(strstr(d, "the roster path was never reached") != nullptr);

    // No transport is not a playhead of 0, and -1 is never printed as a number.
    char e[512];
    snprintf(e, sizeof e, "%s", "waiting for a loaded file");
    AppendProvisionalHorizon(e, sizeof e, 250.0, -1);
    CHECK(strstr(e, "no transport to give a playhead") != nullptr);
    CHECK(strstr(e, "-1") == nullptr);
    CHECK(strstr(e, "playhead 0") == nullptr);

    // A detail already at its limit is truncated, never overrun.
    char full[64];
    memset(full, 'x', sizeof full);
    full[sizeof full - 1] = '\0';
    AppendProvisionalHorizon(full, sizeof full, 1.0, 1);
    CHECK(strlen(full) == sizeof full - 1);

    // A row that answered is not touched by the writer, so the helper must be a no-op on a
    // null buffer rather than a crash.
    AppendProvisionalHorizon(nullptr, 0, 1.0, 1);
}

// The read side is UNCONDITIONAL and `ui.gate_probe` was never added. The
// refusal used to tell an operator to go and turn on a gate that does not exist (L5-RUN-6 H6).
TEST_CASE("ui-gui-begin-gate's unread refusal does not name a gate that never existed") {
    LogFacts f;
    char detail[2048];
    Ask out = Ask::Pass;
    StatusFacts unread;
    CHECK(EvaluateRow("ui-gui-begin-gate", f, unread, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "sits behind") == nullptr);
    CHECK(strstr(detail, "UNCONDITIONAL") != nullptr);
    CHECK(strstr(detail, "none was ever added") != nullptr);
    // The name is still printed -- as the thing that does not exist, which is what an
    // operator who read the old sentence needs to see.
    CHECK(strstr(detail, "ui.gate_probe") != nullptr);
    // And the refusal still refuses to invent a zero.
    CHECK(strstr(detail, "Absent is not zero") != nullptr);
}

TEST_CASE("w2r-residue-fires-on-discontinuity reads the same counter as its sibling (F7)") {
    LogFacts f;
    char detail[512];
    Ask out = Ask::CouldNotAsk;

    StatusFacts s;
    s.batteryDispatched = true;
    s.durationUs = 80000000;
    static const Verb kJumps[] = {Verb::Seek, Verb::StepBack, Verb::Round, Verb::SeekAway,
                                  Verb::Goto};
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        TransportStep& st = s.steps[static_cast<size_t>(kJumps[i])];
        st.attempted = st.submitted = st.answered = st.ok = true;
        st.residueArmed = true;
        st.residueCauseAfter = 0;
        st.residueCauseCount = 6;
        snprintf(st.asked, sizeof st.asked, "%s", VerbName(kJumps[i]));
        st.residueBefore = st.residueAfter = 4;      // no EFFECT moved
        st.residueFiresBefore = 7;
        st.residueFiresAfter = 8;                    // ... but it FIRED
    }
    CHECK(EvaluateRow("w2r-residue-fires-on-discontinuity", f, s, &out, detail, sizeof detail));
    CHECK(out == Ask::CouldNotAsk);
    CHECK(strstr(detail, "nothing bound to act on") != nullptr);
    CHECK(strstr(detail, "ASK the residue packet") == nullptr);  // L3-RUN-3 F7: the stale request

    // Armed, accepted, no effect AND no fire: nobody delivered the discontinuity.
    StatusFacts dead = s;
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        TransportStep& st = dead.steps[static_cast<size_t>(kJumps[i])];
        st.residueFiresAfter = st.residueFiresBefore;
    }
    CHECK(EvaluateRow("w2r-residue-fires-on-discontinuity", f, dead, &out, detail, sizeof detail));
    CHECK(out == Ask::Fail);
    CHECK(strstr(detail, "raised no fire at all") != nullptr);

    // The counter was never read back on any accepted jump: honestly could-not-ask.
    StatusFacts unread = dead;
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        unread.steps[static_cast<size_t>(kJumps[i])].residueCauseAfter = -1;
    }
    CHECK(Ask1("w2r-residue-fires-on-discontinuity", f, unread) == Ask::CouldNotAsk);

    // An effect that moved is still the pass it always was.
    StatusFacts effective = s;
    for (size_t i = 0; i < sizeof kJumps / sizeof kJumps[0]; ++i) {
        effective.steps[static_cast<size_t>(kJumps[i])].residueAfter = 6;
    }
    CHECK(Ask1("w2r-residue-fires-on-discontinuity", f, effective) == Ask::Pass);
}

// ---------------------------------------------------------------------------------------------
// The Report -> StatusFacts copy, and the two done.json groups it feeds.
//
// The double is not a stand-in for product behaviour: `CopyGuiGateFacts` IS the shipped code and
// `probe_replay.cpp` instantiates the same template on the real `csregister::Report`, so a field
// spelled or typed differently there does not build. What the double buys is a DISTINCT value in
// every field, without which a swapped or dropped assignment cannot fail.
// ---------------------------------------------------------------------------------------------
namespace {

// The fixture wants fifteen values no two of which are EQUAL; it wants no particular number. Written
// as literals, two of them were particular numbers: 1104 and 1112 are 0x450 and 0x458, the values of
// CS.TABLE_COUNT_OFF and CS.TABLE_FLAGS_OFF, and the gate skipped four lines of this file by value
// as transcribed offsets. Deriving them from one seed says what the fixture actually means and can
// collide with nothing: a registry value that lands on `kGateFieldSeed + n` is not written here.
const int kGateFieldSeed = 1100;
constexpr int GateField(int n) { return kGateFieldSeed + n; }
constexpr unsigned GateFieldU(int n) { return static_cast<unsigned>(kGateFieldSeed + n); }

struct GateReportDouble {
    bool guiGateSampled = true;
    bool guiGateAddrResolved = false;
    char guiGateUnresolved[160] = "GUI.ATLAS_CONST_B";
    unsigned long long guiGateSamples = GateField(1);
    unsigned int guiSuppressed = GateFieldU(2);
    unsigned int guiHotkeyEnable = GateFieldU(3);
    int guiReentryDepth = GateField(4);
    int guiWindowBudget = GateField(5);
    int guiViewportsEnabled = GateField(6);
    int guiBeginBalanceDelta = GateField(7);
    bool guiImguiBaseValid = true;
    char guiImguiBaseReason[192] = "term 2: io.Fonts was null";
    unsigned int guiAtlasDwordA = GateFieldU(8);
    unsigned int guiAtlasDwordB = GateFieldU(9);
    unsigned int guiAtlasExpectA = GateFieldU(10);
    unsigned int guiAtlasExpectB = GateFieldU(11);
    int guiImguiFrameCountDelta = GateField(12);
    unsigned int guiRenderDrawListsFnPresent = GateFieldU(13);
    int guiRenderedCmdListsCount = GateField(14);
    int guiRenderedTotalVtxCount = GateField(15);
};

}  // namespace

TEST_CASE("every gui gate field arrives in StatusFacts unchanged and in its own member") {
    StatusFacts s;
    CopyGuiGateFacts(GateReportDouble(), s);

    CHECK(s.guiGateSampled == true);
    CHECK(s.guiGateAddrResolved == false);
    CHECK(std::string(s.guiGateUnresolved) == "GUI.ATLAS_CONST_B");
    CHECK(s.guiGateSamples == static_cast<unsigned long long>(GateField(1)));
    CHECK(s.guiSuppressed == GateFieldU(2));
    CHECK(s.guiHotkeyEnable == GateFieldU(3));
    CHECK(s.guiReentryDepth == GateField(4));
    CHECK(s.guiWindowBudget == GateField(5));
    CHECK(s.guiViewportsEnabled == GateField(6));
    CHECK(s.guiBeginBalanceDelta == GateField(7));
    CHECK(s.guiImguiBaseValid == true);
    CHECK(std::string(s.guiImguiBaseReason) == "term 2: io.Fonts was null");
    CHECK(s.guiAtlasDwordA == GateFieldU(8));
    CHECK(s.guiAtlasDwordB == GateFieldU(9));
    CHECK(s.guiAtlasExpectA == GateFieldU(10));
    CHECK(s.guiAtlasExpectB == GateFieldU(11));
    CHECK(s.guiImguiFrameCountDelta == GateField(12));
    CHECK(s.guiRenderDrawListsFnPresent == GateFieldU(13));
    CHECK(s.guiRenderedCmdListsCount == GateField(14));
    CHECK(s.guiRenderedTotalVtxCount == GateField(15));

    // The three booleans cannot be told apart by one assignment of values. This second pattern
    // makes every pairwise swap among them visible in one case or the other.
    GateReportDouble r2;
    r2.guiGateSampled = true;
    r2.guiGateAddrResolved = true;
    r2.guiImguiBaseValid = false;
    StatusFacts s2;
    CopyGuiGateFacts(r2, s2);
    CHECK(s2.guiGateSampled);
    CHECK(s2.guiGateAddrResolved);
    CHECK_FALSE(s2.guiImguiBaseValid);
}

TEST_CASE("a record with no gui gate copy is could-not-ask by name, and never a zero") {
    LogFacts f;
    StatusFacts uncopied;          // exactly the pre-C3 record: the copy never ran
    CHECK(Ask1("ui-gui-begin-gate", f, uncopied) == Ask::CouldNotAsk);
    CHECK_FALSE(DetailMentions("renderedTotalVtxCount=0"));
    CHECK_FALSE(DetailMentions("samples=0 suppressed=0"));

    // And the same record once the copy has run answers off the LIVE values, not a default. The
    // gate globals are zeroed and the atlas pair made to agree so that the record reaches the
    // published line at all: S5.3's window-budget arm and F6's hotkey arm are both ordered ahead
    // of every verdict, and neither is what this case is asking about.
    GateReportDouble live;
    live.guiGateAddrResolved = true;
    live.guiGateUnresolved[0] = '\0';
    live.guiSuppressed = 0;
    live.guiHotkeyEnable = 0;
    live.guiReentryDepth = 0;
    live.guiWindowBudget = 0;
    live.guiBeginBalanceDelta = 1;
    live.guiAtlasExpectA = live.guiAtlasDwordA;
    live.guiAtlasExpectB = live.guiAtlasDwordB;
    StatusFacts copied;
    CopyGuiGateFacts(live, copied);
    Ask1("ui-gui-begin-gate", f, copied);
    CHECK(DetailMentions((std::string("samples=") + std::to_string(GateField(1))).c_str()));
    CHECK(DetailMentions((std::string("renderedTotalVtxCount=") + std::to_string(GateField(15))).c_str()));
}

// L5 #10 s record, the reason this exists: five fields the module published, the probe never read
// and done.json therefore never carried, so "browserIoOnDrawThread == 0" could not be asked of a
// launch. Each value below is distinct, because a fixture where two fields share a value is a
// fixture on which a swapped assignment passes.
TEST_CASE("the draw's phase split reaches the record, each field in its own slot") {
    struct PhaseReportDouble {
        uint64_t imguiNanos = 11;
        uint64_t browserScanNanos = 22;
        uint64_t metadataReadNanos = 33;
        uint64_t textFormatNanos = 44;
        uint64_t browserIoOnDrawThread = 55;
        // The copy is TOTAL, so a double that omits a field it copies is a compile error here as
        // much as on the real Report. These seven are asserted by the case below this one.
        uint64_t drawNanosP99 = 66;
        uint64_t drawNanosP999 = 77;
        uint64_t drawNanosSamples = 88;
        uint64_t drawNanosOverHistogram = 99;
        uint64_t guiItemsPerFrame = 110;
        uint64_t guiGlyphsPerFrame = 121;
        uint64_t guiStreamFrames = 132;
    } r;

    StatusFacts s;
    CHECK_FALSE(s.csDrawPhasesKnown);
    CopyDrawPhaseFacts(r, s);
    CHECK(s.csDrawPhasesKnown);
    CHECK(s.csImguiNanos == 11u);
    CHECK(s.csBrowserScanNanos == 22u);
    CHECK(s.csMetadataReadNanos == 33u);
    CHECK(s.csTextFormatNanos == 44u);
    CHECK(s.csBrowserIoOnDrawThread == 55u);

    const std::string o = CsRegisterObjectText(s);
    CHECK(o.find("\"imguiNanos\": 11") == std::string::npos);   // no gate block, no gate fields
    StatusFacts g = s;
    g.csAsked = true;
    g.csGateKnown = true;
    const std::string withGate = CsRegisterObjectText(g);
    CHECK(withGate.find("\"imguiNanos\": 11") != std::string::npos);
    CHECK(withGate.find("\"browserScanNanos\": 22") != std::string::npos);
    CHECK(withGate.find("\"metadataReadNanos\": 33") != std::string::npos);
    CHECK(withGate.find("\"textFormatNanos\": 44") != std::string::npos);
    CHECK(withGate.find("\"browserIoOnDrawThread\": 55") != std::string::npos);

    // Absent is not zero: a record whose probe never read the split says nothing at all here.
    StatusFacts blank;
    blank.csAsked = true;
    blank.csGateKnown = true;
    const std::string none = CsRegisterObjectText(blank);
    CHECK(none.find("browserIoOnDrawThread") == std::string::npos);
}

// ENG-W2F-DRAW4 s3's seam. Seven more fields on the same Report -- the stream the browser page
// hands ImGui, and the distribution behind drawNanosMax -- published by the panel and unread here
// until this copy, which is why they could not reach done.json. Every value below is distinct,
// from each other AND from the five phase values above: two fields that share a value are two
// fields a swapped assignment would pass on.
TEST_CASE("the gui stream and the draw distribution reach the record, each field in its own slot") {
    struct StreamReportDouble {
        uint64_t imguiNanos = 101;
        uint64_t browserScanNanos = 202;
        uint64_t metadataReadNanos = 303;
        uint64_t textFormatNanos = 404;
        uint64_t browserIoOnDrawThread = 505;
        uint64_t drawNanosP99 = 3072;
        uint64_t drawNanosP999 = 6144;
        uint64_t drawNanosSamples = 999;
        uint64_t drawNanosOverHistogram = 7;
        uint64_t guiItemsPerFrame = 12;
        uint64_t guiGlyphsPerFrame = 883;
        uint64_t guiStreamFrames = 997;
    } r;

    StatusFacts s;
    CHECK(s.csGuiStreamFrames == 0u);
    CopyDrawPhaseFacts(r, s);
    CHECK(s.csImguiNanos == 101u);
    CHECK(s.csBrowserScanNanos == 202u);
    CHECK(s.csMetadataReadNanos == 303u);
    CHECK(s.csTextFormatNanos == 404u);
    CHECK(s.csBrowserIoOnDrawThread == 505u);
    CHECK(s.csDrawNanosP99 == 3072u);
    CHECK(s.csDrawNanosP999 == 6144u);
    CHECK(s.csDrawNanosSamples == 999u);
    CHECK(s.csDrawNanosOverHistogram == 7u);
    CHECK(s.csGuiItemsPerFrame == 12u);
    CHECK(s.csGuiGlyphsPerFrame == 883u);
    CHECK(s.csGuiStreamFrames == 997u);

    StatusFacts g = s;
    g.csAsked = true;
    g.csGateKnown = true;
    const std::string withGate = CsRegisterObjectText(g);
    CHECK(withGate.find("\"guiItemsPerFrame\": 12") != std::string::npos);
    CHECK(withGate.find("\"guiGlyphsPerFrame\": 883") != std::string::npos);
    CHECK(withGate.find("\"guiStreamFrames\": 997") != std::string::npos);
    CHECK(withGate.find("\"drawNanosP99\": 3072") != std::string::npos);
    CHECK(withGate.find("\"drawNanosP999\": 6144") != std::string::npos);
    CHECK(withGate.find("\"drawNanosSamples\": 999") != std::string::npos);
    CHECK(withGate.find("\"drawNanosOverHistogram\": 7") != std::string::npos);

    // Absent is not zero, for these as for the phases: a record whose probe never read the struct
    // says nothing here, and 12 items is not distinguishable from 0 items by omission.
    StatusFacts blank;
    blank.csAsked = true;
    blank.csGateKnown = true;
    const std::string none = CsRegisterObjectText(blank);
    CHECK(none.find("guiItemsPerFrame") == std::string::npos);
    CHECK(none.find("drawNanosP999") == std::string::npos);

    // The row prints the X axis beside the Y. DRAW4 s7 bounds L5 #12's maximum at 90 000-115 000
    // ns for this stream; the value below sits inside that bound, so this detail is the shape a
    // passing launch record will actually have.
    LogFacts f;
    f.OnLine(kBootAttached);
    f.OnLine(kStateInGame);
    StatusFacts pass;
    pass.csAsked = true;
    pass.csInFrameKnown = true;
    pass.csInFrameDraws = 4727;
    pass.csDrawNanosBudget = 200000;
    pass.csDrawNanosMax = 104200;
    pass.csFirstDrawNanos = 4200;
    CopyDrawPhaseFacts(r, pass);
    pass.csBrowserIoOnDrawThread = 0;  // 505 is a FAIL on its own term, which is a different row
    CHECK(Ask1("w2f-draw-budget", f, pass) == Ask::Pass);
    CHECK(DetailMentions("drawNanosMax=104200"));
    CHECK(DetailMentions("items=12"));
    CHECK(DetailMentions("glyphs=883"));
    CHECK(DetailMentions("over 997 browser frame(s)"));
    CHECK(DetailMentions("p99<=3072"));
    CHECK(DetailMentions("p999<=6144"));
    CHECK(DetailMentions("of 999 sample(s)"));
    CHECK(DetailMentions("7 past the histogram"));

    // A breach carries the same stream: a maximum with no items beside it cannot tell a slow
    // engine from a fat page.
    StatusFacts over = pass;
    over.csDrawNanosMax = over.csDrawNanosBudget + 1;
    CHECK(Ask1("w2f-draw-budget", f, over) == Ask::Fail);
    CHECK(DetailMentions("items=12"));
    CHECK(DetailMentions("p999<=6144"));

    // guiStreamFrames == 0: no browser frame was drawn, so the counts beside it are unmeasured.
    // The percentiles are NOT browser-conditioned and are still printed.
    StatusFacts noFrames = pass;
    noFrames.csGuiStreamFrames = 0;
    CHECK(Ask1("w2f-draw-budget", f, noFrames) == Ask::Pass);
    CHECK(DetailMentions("NO BROWSER FRAME WAS DRAWN"));
    CHECK_FALSE(DetailMentions("items=12"));
    CHECK(DetailMentions("p99<=3072"));
}

// The row scores the split as well as the number. A launch that is inside the ceiling and cannot
// say whether a file was opened on the render thread has not answered the question.
TEST_CASE("w2f-draw-budget refuses to pass a record that carries no phase split") {
    LogFacts f;
    f.OnLine(kBootAttached);
    f.OnLine(kStateInGame);

    StatusFacts base;
    base.csAsked = true;
    base.csInFrameKnown = true;
    base.csInFrameDraws = 4759;
    base.csDrawNanosBudget = 200000;
    base.csDrawNanosMax = 51224;

    StatusFacts noSplit = base;
    CHECK(Ask1("w2f-draw-budget", f, noSplit) == Ask::CouldNotAsk);
    CHECK(DetailMentions("carries no phase split"));

    StatusFacts clean = base;
    clean.csDrawPhasesKnown = true;
    clean.csImguiNanos = 50900;
    clean.csMetadataReadNanos = 812300;
    CHECK(Ask1("w2f-draw-budget", f, clean) == Ask::Pass);
    CHECK(DetailMentions("imgui=50900"));
    CHECK(DetailMentions("drawThreadIo=0"));

    // The regression this whole packet exists to make visible, in the record instead of in a
    // frame time: I/O back on the thread that draws, on a run that is still inside the ceiling.
    StatusFacts regressed = clean;
    regressed.csBrowserIoOnDrawThread = 1002;
    CHECK(Ask1("w2f-draw-budget", f, regressed) == Ask::Fail);
    CHECK(DetailMentions("FILE I/O ON THE RENDER THREAD"));

    // L5 #10 run 2, verbatim, with the split it should have carried: still a breach, and now the
    // breach names where the time went.
    StatusFacts l5run10 = clean;
    l5run10.csDrawNanosMax = 257000;
    l5run10.csImguiNanos = 256800;
    CHECK(Ask1("w2f-draw-budget", f, l5run10) == Ask::Fail);
    CHECK(DetailMentions("OVER BUDGET"));
    CHECK(DetailMentions("imgui=256800"));
}

TEST_CASE("done.json carries the gate counters and S6.3(a)'s vertex count") {
    StatusFacts s = FacadeFacts();
    s.csGateKnown = true;
    s.csGateScopeEnabled = true;
    s.csGateScopeEntries = 4023;
    s.csGateScopeRestores = 4022;
    s.csPanelHoveredFrames = 7;
    s.csDrawNanosTotal = 5555555;
    s.csDrawNanosMax = 987654;
    strncpy_s(s.csPanelHoverSource, sizeof s.csPanelHoverSource, "wantcapturemouse", _TRUNCATE);
    CopyGuiGateFacts(GateReportDouble(), s);

    const std::string o = CsRegisterObjectText(s);
    CHECK(o.find("\"scopeEntries\": 4023") != std::string::npos);
    CHECK(o.find("\"scopeRestores\": 4022") != std::string::npos);
    CHECK(o.find("\"hovered\": 7") != std::string::npos);
    CHECK(o.find("\"hoverSource\": \"wantcapturemouse\"") != std::string::npos);
    CHECK(o.find("\"drawNanosMax\": 987654") != std::string::npos);
    CHECK(o.find("\"scope\": true") != std::string::npos);
    // S6.3(a)'s number, on EVERY run, so the ui.gate_scope pair has two sides to difference.
    CHECK(o.find(std::string("\"renderedTotalVtxCount\": ") + std::to_string(GateField(15))) != std::string::npos);
    CHECK(o.find(std::string("\"renderedCmdListsCount\": ") + std::to_string(GateField(14))) != std::string::npos);
    CHECK(o.find(std::string("\"frameCountDelta\": ") + std::to_string(GateField(12))) != std::string::npos);
    CHECK(o.find("\"imguiBaseValid\": true") != std::string::npos);

    // Absent means unread. A base that did not prove out publishes no context field at all --
    // never a zero, and never the reading the wrong object happened to give.
    StatusFacts invalid = s;
    invalid.guiImguiBaseValid = false;
    const std::string ni = CsRegisterObjectText(invalid);
    CHECK(ni.find("\"guiGate\"") != std::string::npos);
    CHECK(ni.find("renderedTotalVtxCount") == std::string::npos);
    CHECK(ni.find("renderedCmdListsCount") == std::string::npos);
    CHECK(ni.find("frameCountDelta") == std::string::npos);

    // A single sample has no previous entry, so there is no delta to publish.
    StatusFacts one = s;
    one.guiGateSamples = 1;
    CHECK(CsRegisterObjectText(one).find("frameCountDelta") == std::string::npos);

    // A build without the read side, and a Report never read: whole groups absent, not zeroed.
    StatusFacts unsampled = s;
    unsampled.guiGateSampled = false;
    CHECK(CsRegisterObjectText(unsampled).find("\"guiGate\"") == std::string::npos);

    StatusFacts nogate = s;
    nogate.csGateKnown = false;
    CHECK(CsRegisterObjectText(nogate).find(", \"gate\": {") == std::string::npos);
    CHECK(CsRegisterObjectText(nogate).find("hoverSource") == std::string::npos);
}

namespace {

// The product's own text, read out of the checkout this suite was built against. A fixture that
// quotes a product word and is never checked against the product goes on passing after the word is
// retired -- which is what W2-H `65a3894` left behind at the two assertions above.
// The capture ENG-W2B-STEP2 measured records at 30 Hz, so its recorded ticks are one 30 Hz
// frame apart. Written as the division and not as the microsecond count: that count IS the
// engine's own fixed step, and a bare literal here reads as a claim about that fact rather
// than as this fixture's own spacing.
constexpr long long kThirtyHzUs = 1000000 / 30;

bool SourceContains(const char* relPath, const char* needle) {
    const std::string text = ReadRepoFile(relPath);
    if (text.empty()) {
        return false;
    }
    // The product wraps its long strings across lines, so the needle is matched with every run of
    // whitespace collapsed on BOTH sides -- otherwise this control would fail on a reflow.
    std::string flat;
    bool space = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"') {
            space = true;
            continue;
        }
        if (space && !flat.empty()) { flat.push_back(' '); }
        space = false;
        flat.push_back(c);
    }
    std::string want;
    space = false;
    for (const char* p = needle; *p != '\0'; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { space = true; continue; }
        if (space && !want.empty()) { want.push_back(' '); }
        space = false;
        want.push_back(*p);
    }
    return flat.find(want) != std::string::npos;
}

}  // namespace

TEST_CASE("the product words this suite quotes still exist in the product") {
    // The sink reason the fixtures above carry is read out of SinkVerdictName, so a rename
    // follows them by itself. What a copy could not do and this can: score the SET those readers
    // are pointed at. Every enumerator the header declares must answer with its own sentence --
    // AUDIO4 `4ffac1d` deleted `RatioMoved`, and the copy that quoted it stayed green for a day.
    const std::vector<std::string> verdicts = SinkVerdictEnumerators();
    REQUIRE(verdicts.size() >= 8);      // the header's own list, minus Count
    std::vector<std::string> said;
    for (size_t i = 0; i < verdicts.size(); ++i) {
        INFO("SinkVerdict::" << verdicts[i]);
        const std::string word = SinkVerdictSpelling(verdicts[i]);
        CHECK(word.size() > 1);
        for (size_t j = 0; j < said.size(); ++j) {
            CHECK(word != said[j]);
        }
        said.push_back(word);
    }
    // ... and the reader tells an absent enumerator from a present one, so the loop above is not
    // a row of empty strings agreeing with each other.
    CHECK(SinkVerdictSpelling("RatioMoved").empty());
    CHECK_FALSE(SinkVerdictSpelling("RatioOutOfDomain").empty());

    // W2-B `ad17dd9` publishes the netvar fidelity state as four sentences and w2b-netvars-applied
    // keys on all four. They cannot be read out of an enum from here, so they are pinned against
    // their producer: a rename reds HERE rather than turning three live arms into one shrug.
    CHECK(SourceContains("src/playback/core/apply.cpp", "netvars: applied"));
    CHECK(SourceContains("src/playback/core/apply.cpp",
                         "netvars: no netvar records in this capture"));
    CHECK(SourceContains("src/playback/core/apply.cpp",
                         "netvars: records present, none due at this playhead"));
    CHECK(SourceContains("src/playback/core/apply.cpp", "netvars: STORED, NOT APPLIED"));
    // The one it retired. If it ever comes back, the row that no longer reads it must be told.
    CHECK_FALSE(SourceContains("src/playback/core/apply.cpp", "netvars: none"));
    // And both edges the body-handle row now accepts. SourceContains reads a double quote as
    // whitespace, so the trigger words are matched without theirs.
    // PRESENT-H: the rebuild site now spells two triggers through one ternary, `light` picking the
    // scrub step. Pinning the whole expression keeps BOTH words here, which pinning only
    // "window rebuilt" no longer could.
    CHECK(SourceContains("src/playback/core/playback_module.cpp",
                         "BindBodyHandlesIfReady(light ? scrub step : window rebuilt , light)"));
    CHECK(SourceContains("src/playback/core/playback_module.cpp",
                         "BindBodyHandlesIfReady( entity re-bound )"));
    // And the fixture's copy of kStepTicksUncounted. INT_MIN and not 0: 0 is a COUNTED refusal, so
    // a fixture that used it would prove the row reads refusals rather than absences.
    CHECK(SourceContains("src/playback/core/transport.h",
                         "constexpr int kStepTicksUncounted = INT_MIN;"));
}

// ENG-W2B-STEP2 `d77cd0b` proved against the real capture that a seek landing mid-tick makes the
// two steps cover DIFFERENT distances, and that this is correct. `f4db605` scored the pair for
// symmetry inside half a spacing, so it would red a working product on exactly this data. The
// fixture obeys STEP2's rule: the playhead the steps start from is NOT on a boundary.
TEST_CASE("W2-R step: unequal deltas from a mid-tick start are the CORRECT answer") {
    LogFacts f;
    StatusFacts s = WorkingBattery();
    s.tickSpacingUs = kThirtyHzUs;
    TransportStep& fwd = s.steps[(size_t)Verb::StepForward];
    TransportStep& bak = s.steps[(size_t)Verb::StepBack];
    // At(i) = 100000000, At(i+1) = 100033333, and the seek landed 25000 us into that tick.
    fwd.playheadBefore = 100025000;
    fwd.playheadAfter = 100033333;   // +8333: the rest of the tick it was inside
    bak.playheadBefore = 100033333;
    bak.playheadAfter = 100000000;   // -kThirtyHzUs: a whole recorded tick, from a boundary
    s.stepBoundaryForwardUs = 100033333;
    s.stepBoundaryBackUs = 100000000;

    CHECK(Ask1("w2r-step-moves-by-ticks", f, s) == Ask::Pass);
    CHECK(DetailMentions("DIFFER by 25000 us"));
    CHECK(DetailMentions("landed mid-tick"));
    // The RED-BEFORE control, stated as arithmetic rather than as a claim: the skew this correct
    // pair produces is past `f4db605`'s bound of half a spacing, so the row as landed there
    // reported asked-FAIL on it.
    const long long skew = kThirtyHzUs - 8333;
    REQUIRE(skew > kThirtyHzUs / 2);
}

// The boundary clause against the numbers a REAL capture produced, now that the
// probe fills the channel from IReplayTransport::NearestTickUs (W2-B, 382990f) instead of leaving
// it unasked: the seek landed at 116615500 us, whose recorded boundary at or before is 116598834,
// so `step +1` owed only the rest of that tick and landed on 116632167, and `step -1` came back.
TEST_CASE("W2-R step: the STEP2 capture's own landings are scored against its own tick table") {
    LogFacts f;
    StatusFacts s = WorkingBattery();
    s.tickSpacingUs = kThirtyHzUs;
    TransportStep& fwd = s.steps[(size_t)Verb::StepForward];
    TransportStep& bak = s.steps[(size_t)Verb::StepBack];
    fwd.playheadBefore = 116615500;
    fwd.playheadAfter = 116632167;
    bak.playheadBefore = 116632167;
    bak.playheadAfter = 116598834;
    s.stepBoundaryForwardUs = 116632167;
    s.stepBoundaryBackUs = 116598834;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, s) == Ask::Pass);
    CHECK(DetailMentions("116632167"));

    // The same landings with the channel unfilled -- the state every launch was in while the probe
    // published no boundary -- pass on a sentence that says the clause was not asked. That is why
    // the WIRING is this change and the clause is not: the row could already fail, and never did.
    StatusFacts unasked = s;
    unasked.stepBoundaryKnown = false;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, unasked) == Ask::Pass);
    CHECK(DetailMentions("boundary-not-published"));

    // One microsecond off a recorded boundary is the failure the clause exists for, and every
    // other clause still passes on it.
    StatusFacts off = s;
    off.steps[(size_t)Verb::StepBack].playheadAfter = 116598835;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, off) == Ask::Fail);
    CHECK(DetailMentions("A step is exact by definition"));
}

TEST_CASE("W2-R step: each of the four clauses can fail on its own") {
    LogFacts f;
    StatusFacts base = WorkingBattery();
    base.tickSpacingUs = kThirtyHzUs;
    {
        TransportStep& fwd = base.steps[(size_t)Verb::StepForward];
        TransportStep& bak = base.steps[(size_t)Verb::StepBack];
        fwd.playheadBefore = 100025000;
        fwd.playheadAfter = 100033333;
        bak.playheadBefore = 100033333;
        bak.playheadAfter = 100000000;
        base.stepBoundaryForwardUs = 100033333;
        base.stepBoundaryBackUs = 100000000;
    }
    REQUIRE(Ask1("w2r-step-moves-by-ticks", f, base) == Ask::Pass);

    // 3. the transport's own count. Two ticks out of one command is a failure the microsecond
    //    deltas cannot see -- both landings are still legal boundaries.
    StatusFacts twoTicks = base;
    twoTicks.steps[(size_t)Verb::StepForward].lastStepTicks = 2;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, twoTicks) == Ask::Fail);
    CHECK(DetailMentions("reported 2 recorded tick(s)"));

    // 2. magnitude. One command must not cover more than one recorded tick.
    StatusFacts tooFar = base;
    tooFar.steps[(size_t)Verb::StepBack].playheadAfter = 99960000;
    tooFar.stepBoundaryBackUs = 99960000;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, tooFar) == Ask::Fail);
    CHECK(DetailMentions("longer than this capture's whole"));

    // 4. the boundary. A landing one microsecond off a recorded tick is a playhead the file does
    //    not contain, and every other clause above still passes on it.
    StatusFacts offBoundary = base;
    offBoundary.stepBoundaryForwardUs = 100033334;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, offBoundary) == Ask::Fail);
    CHECK(DetailMentions("A step is exact by definition"));

    // The boundary channel unfilled is a NAMED could-not-ask for that clause, never a silent pass.
    StatusFacts noBoundary = base;
    noBoundary.stepBoundaryKnown = false;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, noBoundary) == Ask::Pass);
    CHECK(DetailMentions("boundary-not-published"));

    // An uncounting transport has refused nothing, and is not graded as a zero-tick step.
    StatusFacts uncounted = base;
    uncounted.steps[(size_t)Verb::StepForward].lastStepTicks = INT_MIN;
    uncounted.steps[(size_t)Verb::StepBack].lastStepTicks = INT_MIN;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, uncounted) == Ask::CouldNotAsk);
    CHECK(DetailMentions("counts no steps at all"));

    StatusFacts unread = base;
    unread.steps[(size_t)Verb::StepBack].stepTicksRead = false;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, unread) == Ask::CouldNotAsk);
    CHECK(DetailMentions("LastStepTicks"));

    // The pair has to be one step out and one step back out of the same landing.
    StatusFacts drifted = base;
    drifted.steps[(size_t)Verb::StepBack].playheadBefore = 100033330;
    CHECK(Ask1("w2r-step-moves-by-ticks", f, drifted) == Ask::Fail);
    CHECK(DetailMentions("something moved the playhead between the two steps"));
}

TEST_CASE("W2-C goto round: the landing is scored against the CAPTURE's tick, and an empty table refuses") {
    LogFacts f;
    StatusFacts s = WorkingBattery();
    s.tickSpacingUs = kThirtyHzUs;
    s.roundsKnown = true;
    s.roundCount = 3;
    TransportStep& rd = s.steps[(size_t)Verb::Round];
    rd.ok = true;
    rd.target = 100000000;
    rd.playheadBefore = 40000000;
    rd.playheadAfter = 100010000;   // 10000 us out, inside one kThirtyHzUs us tick
    _snprintf_s(rd.asked, sizeof rd.asked, _TRUNCATE, "round 3");
    _snprintf_s(rd.said, sizeof rd.said, _TRUNCATE, "round 3 at 100000000 us");
    CHECK(Ask1("w2c-goto-round-lands", f, s) == Ask::Pass);
    CHECK(DetailMentions("begin marker"));

    StatusFacts wide = s;
    wide.steps[(size_t)Verb::Round].playheadAfter = 100040000;   // past one tick
    CHECK(Ask1("w2c-goto-round-lands", f, wide) == Ask::Fail);
    CHECK(DetailMentions("Landing on the wrong round does not fault"));

    // The row scores the CAPTURE's tick, not the machine's frame period: a seekToleranceUs wide
    // enough to admit the miss must not rescue it.
    StatusFacts machineWide = wide;
    machineWide.seekToleranceUs = 5000000;
    CHECK(Ask1("w2c-goto-round-lands", f, machineWide) == Ask::Fail);

    StatusFacts noCadence = s;
    noCadence.tickSpacingUs = 0;
    CHECK(Ask1("w2c-goto-round-lands", f, noCadence) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no cadence"));

    StatusFacts refusedWithTable = s;
    refusedWithTable.steps[(size_t)Verb::Round].ok = false;
    CHECK(Ask1("w2c-goto-round-lands", f, refusedWithTable) == Ask::Fail);
    CHECK(DetailMentions("REFUSED although"));

    // ***** ZERO ROUNDS IS A LEGAL FILE and the named refusal is the PASS.
    StatusFacts empty = s;
    empty.roundCount = 0;
    empty.steps[(size_t)Verb::Round].ok = false;
    empty.steps[(size_t)Verb::Round].playheadBefore = 40000000;
    empty.steps[(size_t)Verb::Round].playheadAfter = 40000000;
    _snprintf_s(empty.steps[(size_t)Verb::Round].said,
                sizeof empty.steps[(size_t)Verb::Round].said, _TRUNCATE,
                "no round 1 in this capture");
    CHECK(Ask1("w2c-goto-round-lands", f, empty) == Ask::Pass);
    CHECK(DetailMentions("The named refusal is the correct answer"));

    // And a jump that ANSWERED on a file with no round table is the silent clamp, not a pass.
    StatusFacts clamped = empty;
    clamped.steps[(size_t)Verb::Round].ok = true;
    clamped.steps[(size_t)Verb::Round].playheadAfter = 100000000;
    CHECK(Ask1("w2c-goto-round-lands", f, clamped) == Ask::Fail);
    CHECK(DetailMentions("silent clamp"));

    // A refusal that moved the playhead anyway is still a failure.
    StatusFacts refusedMoved = empty;
    refusedMoved.steps[(size_t)Verb::Round].playheadAfter = 41000000;
    CHECK(Ask1("w2c-goto-round-lands", f, refusedMoved) == Ask::Fail);

    // "no rounds" and "we could not read whether there are any" are different answers.
    StatusFacts unknown = empty;
    unknown.roundsKnown = false;
    CHECK(Ask1("w2c-goto-round-lands", f, unknown) == Ask::CouldNotAsk);
    CHECK(DetailMentions("could not read whether"));
}

TEST_CASE("W2-A marker source: the triple keeps three states apart, and `none` is asked-fail") {
    LogFacts f;
    StatusFacts base;
    base.sentinelRead = true;
    base.sentinelMarkerSourceKnown = true;
    strcpy_s(base.sentinelMarkerSource, "local");
    base.sentinelLocalBoundariesKnown = true;
    base.sentinelLocalBoundaries = 3;
    base.sentinelCorroboratorAskedKnown = true;
    base.sentinelCorroboratorAsked = false;
    base.sentinelWireRowsSeenKnown = true;
    base.sentinelWireRowsSeen = 0;

    // The solo-host answer the packet was written for: the sampler works, the source was chosen,
    // and nobody was fed a wire row. That is a PASS with the corroboration named as never asked.
    CHECK(Ask1("w2a-marker-source-named", f, base) == Ask::Pass);
    CHECK(DetailMentions("NEVER FED"));
    CHECK(DetailMentions("corroboratorAsked=false"));

    // ***** THE STATE THE TRIPLE EXISTS FOR. `local` with zero boundaries is "the sampler works and
    // no round started", which is could-not-ask -- NOT the same answer as having no source.
    StatusFacts noRound = base;
    noRound.sentinelLocalBoundaries = 0;
    CHECK(Ask1("w2a-marker-source-named", f, noRound) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no-round-started"));

    StatusFacts none = base;
    strcpy_s(none.sentinelMarkerSource, "none");
    CHECK(Ask1("w2a-marker-source-named", f, none) == Ask::Fail);
    CHECK(DetailMentions("NO MARKER SOURCE"));
    // The two must never read as each other.
    CHECK_FALSE(DetailMentions("no-round-started"));

    // A word the observer does not classify is refused, never sorted into the nearest bucket.
    StatusFacts notRecorded = base;
    strcpy_s(notRecorded.sentinelMarkerSource, "not_recorded");
    CHECK(Ask1("w2a-marker-source-named", f, notRecorded) == Ask::CouldNotAsk);
    CHECK(DetailMentions("neither `local`, `wire` nor `none`"));

    // Fed and empty is a DIFFERENT finding from never fed, and the sentences differ.
    StatusFacts fedEmpty = base;
    fedEmpty.sentinelCorroboratorAsked = true;
    CHECK(Ask1("w2a-marker-source-named", f, fedEmpty) == Ask::Pass);
    CHECK(DetailMentions("fed and saw zero netvar rows"));
    CHECK_FALSE(DetailMentions("NEVER FED"));

    StatusFacts corroborated = fedEmpty;
    corroborated.sentinelWireRowsSeen = 7;
    CHECK(Ask1("w2a-marker-source-named", f, corroborated) == Ask::Pass);
    CHECK(DetailMentions("saw 7 netvar row(s)"));

    // A wire source with nothing on the wire contradicts its own evidence.
    StatusFacts wireEmpty = base;
    strcpy_s(wireEmpty.sentinelMarkerSource, "wire");
    CHECK(Ask1("w2a-marker-source-named", f, wireEmpty) == Ask::Fail);
    CHECK(DetailMentions("nothing on the wire"));

    StatusFacts wireOk = wireEmpty;
    wireOk.sentinelWireRowsSeen = 12;
    CHECK(Ask1("w2a-marker-source-named", f, wireOk) == Ask::Pass);

    // Absent is not zero, and no sentinel is not `none`.
    StatusFacts noCount = base;
    noCount.sentinelLocalBoundariesKnown = false;
    CHECK(Ask1("w2a-marker-source-named", f, noCount) == Ask::CouldNotAsk);
    CHECK(DetailMentions("Absent is not zero"));

    StatusFacts unread;
    strcpy_s(unread.sentinelReadReason,
             "no capture close has published its stop facts in this process yet");
    CHECK(Ask1("w2a-marker-source-named", f, unread) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no-sentinel"));
}

// These rows used to be filled by parsing capture_stop.<ft>.<pid>.json off disk.
// They are now filled by FillFromStopFacts out of the const snapshot LastStopFacts publishes, so the
// fixture is a fake StopFactsSnapshot -- the close's own record shape -- and the mutant these cases
// exist for is a fill that IGNORES the snapshot and leaves the struct's zeros standing.
TEST_CASE("W2-A stop facts: the close's own snapshot fills the rows, and absent is not zero") {
    LogFacts f;

    replay::capture::StopFactsSnapshot played = {};
    played.markerSource = static_cast<uint8_t>(replay::MarkerSource::Local);
    played.localBoundaries = 3;
    played.corroboratorAsked = true;
    played.wireRowsSeen = 7;
    played.roundRowCount = 3;
    played.roundRowsRecorded = true;
    played.roundCounter = 3;
    played.roundCounterRead = true;
    strcpy_s(played.phase, "round_over");
    played.phaseSampled = true;

    StatusFacts s;
    FillFromStopFacts(s, played, "local");
    CHECK(s.sentinelRead);
    CHECK(s.sentinelLocalBoundaries == 3ul);
    CHECK(s.sentinelWireRowsSeen == 7ul);
    CHECK(s.sentinelRoundRowCount == 3ul);
    CHECK(s.sentinelRoundRowCountKnown);
    CHECK(strcmp(s.sentinelPhaseWord, "round_over") == 0);
    CHECK(Ask1("w2a-round-table-at-close", f, s) == Ask::Pass);
    CHECK(DetailMentions("3 round row(s)"));
    // The SAME snapshot still answers the marker triple, in the same words it did off the file.
    CHECK(Ask1("w2a-marker-source-named", f, s) == Ask::Pass);
    CHECK(DetailMentions("second opinion behind them"));

    // ***** THE MUTANT'S TARGET. Three boundaries cut and no round row written is rounds LOST, and
    // a fill that ignored the snapshot would land every arm below here as well.
    replay::capture::StopFactsSnapshot lost = played;
    lost.roundRowCount = 0;
    lost.roundCounter = 0;
    StatusFacts sLost;
    FillFromStopFacts(sLost, lost, "local");
    CHECK(Ask1("w2a-round-table-at-close", f, sLost) == Ask::Fail);
    CHECK(DetailMentions("ROUNDS LOST AT CLOSE"));

    // Reached the table with nothing to put in it: the solo host in pre_match. Could-not-ask.
    replay::capture::StopFactsSnapshot idle = played;
    idle.localBoundaries = 0;
    idle.roundRowCount = 0;
    idle.roundCounter = 0;
    StatusFacts sIdle;
    FillFromStopFacts(sIdle, idle, "local");
    CHECK(Ask1("w2a-round-table-at-close", f, sIdle) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no-round-started"));

    // ABSENT is not zero: a close that never reached the table cannot report that it held nothing.
    replay::capture::StopFactsSnapshot unreached = played;
    unreached.roundRowsRecorded = false;
    unreached.roundRowCount = 0;
    StatusFacts sUnreached;
    FillFromStopFacts(sUnreached, unreached, "local");
    CHECK(Ask1("w2a-round-table-at-close", f, sUnreached) == Ask::CouldNotAsk);
    CHECK(DetailMentions("absent count is not a count of zero"));

    // No close has published at all -- what LastStopFacts returning false leaves behind. The row
    // must say so rather than score a zeroed struct as a table that held nothing.
    StatusFacts never;
    CHECK(Ask1("w2a-round-table-at-close", f, never) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no-stop-facts"));

    // The WIRE arm: its rounds come off the netvar stream, so zero local boundaries behind two
    // round rows is not a contradiction and must not be scored as one.
    replay::capture::StopFactsSnapshot wire = {};
    wire.markerSource = static_cast<uint8_t>(replay::MarkerSource::Wire);
    wire.wireRowsSeen = 12;
    wire.roundRowCount = 2;
    wire.roundRowsRecorded = true;
    wire.roundCounter = 2;
    wire.roundCounterRead = true;
    StatusFacts sWire;
    FillFromStopFacts(sWire, wire, "wire");
    CHECK(Ask1("w2a-round-table-at-close", f, sWire) == Ask::Pass);
    CHECK(DetailMentions("2 round row(s)"));
    CHECK(Ask1("w2a-marker-source-named", f, sWire) == Ask::Pass);
}

// THE DESCRIPTOR IS THE DENOMINATOR, and it lives in a
// JSON file no compiler reads. `1652e8d` and `9474351` added w2a-marker-source-named,
// w2c-goto-round-lands and w2f-draw-budget to kRows without adding them to W2-R.probe.json, so for
// four commits three rows the arms produce were asked for by NOBODY: they would have rendered
// never-asked on every launch, and the suite that owns those rows stayed green throughout.
// tests/replay/check.ps1 caught it, but only by parsing kRows out of the header with a regex and
// only when someone ran the gate. This asks the same question at TEST time, off the compiled table
// and the real descriptor, in both directions -- a row the descriptor omits and a row it lists that
// no arm produces are both RED here.
namespace {

std::string W2RReadRepoFile(const char* relPath) {
    char path[1024];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s/%s", REPLAY_REPO_ROOT, relPath);
    std::string text;
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || fp == nullptr) { return text; }
    char buf[4096];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof buf, fp)) > 0) { text.append(buf, got); }
    fclose(fp);
    return text;
}

// The text between the brackets of `"<key>": [ ... ]`, searched only inside [from, to) so a key
// belonging to the NEXT probe cannot answer for this one. Empty when the key is absent.
std::string W2RArray(const std::string& doc, size_t from, size_t to, const char* key) {
    const std::string want = std::string("\"") + key + "\":";
    const size_t at = doc.find(want, from);
    if (at == std::string::npos || at >= to) { return std::string(); }
    const size_t open = doc.find('[', at);
    if (open == std::string::npos || open >= to) { return std::string(); }
    const size_t close = doc.find(']', open);
    if (close == std::string::npos || close > to) { return std::string(); }
    return doc.substr(open, close - open + 1);
}

// Matched WITH the quotes: a bare substring test would read w2r-goto-next-walks-markers as
// evidence for w2r-goto-next, and substring matching is the commonest false pass in this tree.
bool W2RLists(const std::string& arrayText, const char* id) {
    return arrayText.find(std::string("\"") + id + "\"") != std::string::npos;
}

size_t W2RQuotedCount(const std::string& arrayText) {
    size_t quotes = 0;
    for (size_t i = 0; i < arrayText.size(); ++i) {
        if (arrayText[i] == '"') { ++quotes; }
    }
    return quotes / 2;
}

// The two rules the module itself uses to pick an arm: the profile supplies ECHOREPLAY_OPEN's
// shape, and an id ending in -idle declares the attended arm (RK-11).
unsigned W2RArmOf(const std::string& id) {
    const char* suffix[] = {"-browser", "-capture", "-idle"};
    const unsigned bit[] = {kArmBrowser, kArmCapture, kArmIdle};
    for (size_t i = 0; i < 3; ++i) {
        const size_t n = strlen(suffix[i]);
        if (id.size() >= n && id.compare(id.size() - n, n, suffix[i]) == 0) { return bit[i]; }
    }
    return kArmPlayback;
}

}  // namespace

TEST_CASE("every row the arms produce is asked for by W2-R.probe.json, and nothing else is") {
    const std::string doc = W2RReadRepoFile("tests/replay/W2-R.probe.json");
    REQUIRE(doc.size() > 0);
    size_t rowCount = 0;
    const RowSpec* rows = Rows(&rowCount);
    REQUIRE(rowCount > 0);

    // Probe boundaries first, so each probe is scored against ITS OWN answers[] and rows[].
    std::vector<size_t> starts;
    const std::string idKey = "\"id\": \"";
    for (size_t at = doc.find(idKey); at != std::string::npos; at = doc.find(idKey, at + 1)) {
        starts.push_back(at);
    }
    // Non-vacuity: five probes, or this loop would pass by finding nothing to score.
    REQUIRE(starts.size() == 5);

    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t idAt = starts[i] + idKey.size();
        const size_t idEnd = doc.find('"', idAt);
        REQUIRE(idEnd != std::string::npos);
        const std::string probeId = doc.substr(idAt, idEnd - idAt);
        const size_t to = (i + 1 < starts.size()) ? starts[i + 1] : doc.size();
        const unsigned arm = W2RArmOf(probeId);
        const std::string answers = W2RArray(doc, starts[i], to, "answers");
        const std::string listed = W2RArray(doc, starts[i], to, "rows");
        INFO("probe ", probeId);
        REQUIRE(answers.size() > 0);
        REQUIRE(listed.size() > 0);

        size_t produced = 0;
        for (size_t r = 0; r < rowCount; ++r) {
            const bool onThisArm = (rows[r].arms & arm) != 0;
            INFO("row ", std::string(rows[r].id), std::string(onThisArm ? " (produced here)" : " (not produced here)"));
            // answers[] is what the READ ids are scored against and rows[] is what the observer
            // declares; ARCH-11 S2.2 requires the same set in both, so both are asserted.
            CHECK(W2RLists(answers, rows[r].id) == onThisArm);
            CHECK(W2RLists(listed, rows[r].id) == onThisArm);
            if (onThisArm) { ++produced; }
        }
        // Every arm produces something, and the descriptor carries exactly that many entries -- an
        // id in the descriptor that kRows does not publish at all is caught by the counts.
        CHECK(produced > 0);
        CHECK(W2RQuotedCount(answers) == produced);
        CHECK(W2RQuotedCount(listed) == produced);
    }
}


// =============================================================================================
// ENG-W2R-ROWS4 -- L3-RUN-7 F1, F2, F3 and F6.
// =============================================================================================

namespace {

const char* kRebindZero =
    "15:01:03.425 INFO [playback] body handles re-bound: 0 bound, 0 absent this session, "
    "0 not net ids, 0 unresolvable (window rebuilt)";
const char* kNoNetvarRecords =
    "15:01:03.440 INFO [playback] netvars: no netvar records in this capture (nothing to apply)";
const char* kNetvarRecordsPresent =
    "15:01:03.440 INFO [playback] netvars: records present, none due at this playhead";

}  // namespace

TEST_CASE("W2-B: an EMPTY candidate set is a different finding from a set that failed to resolve") {
    StatusFacts s;

    // The four counters sum to zero and no fidelity state was published: nothing in the session
    // says whether the file has net ids, and a reader that guessed would be guessing.
    LogFacts blind;
    blind.OnLine(kRebindZero);
    REQUIRE(blind.bodiesCountersParsed);
    REQUIRE(blind.bodiesAbsent == 0);
    REQUIRE(blind.bodiesNotNetIds == 0);
    CHECK(Ask1("w2b-body-handles-rebound", blind, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no 'netvars:' state was published"));

    // ARCH-21: a solo -offline host writes no netvar records, so there are no net ids to bind and
    // the question cannot be put to this file. L3-RUN-7 scored this asked-FAIL.
    LogFacts solo;
    solo.OnLine(kRebindZero);
    solo.OnLine(kNoNetvarRecords);
    CHECK(Ask1("w2b-body-handles-rebound", solo, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no net ids in this capture"));

    // ★ THE ARM THAT MUST STILL RED. Same empty candidate set, on a file that HAS records.
    LogFacts populated;
    populated.OnLine(kRebindZero);
    populated.OnLine(kNetvarRecordsPresent);
    CHECK(Ask1("w2b-body-handles-rebound", populated, s) == Ask::Fail);
    CHECK(DetailMentions("HAS netvar records"));

    // ★ AND THE TWO ARMS THE NEW CLAUSE MUST NOT SWALLOW, both on a no-records file:
    // a real bind still passes, and a considered-but-unresolved set still fails.
    LogFacts bound;
    bound.OnLine("15:01:03.425 INFO [playback] body handles re-bound: 2 bound, 0 absent this "
                 "session, 0 not net ids, 0 unresolvable (window rebuilt)");
    bound.OnLine(kNoNetvarRecords);
    CHECK(Ask1("w2b-body-handles-rebound", bound, s) == Ask::Pass);

    LogFacts unresolved;
    unresolved.OnLine("15:01:03.425 INFO [playback] body handles re-bound: 0 bound, 3 absent this "
                      "session, 0 not net ids, 1 unresolvable (window rebuilt)");
    unresolved.OnLine(kNoNetvarRecords);
    CHECK(Ask1("w2b-body-handles-rebound", unresolved, s) == Ask::Fail);
}

TEST_CASE("W2-H: a process with no mixer cannot be asked for a slope, and the operand is the 0.50x line") {
    // The refusal word is the PRODUCT's, and this is the control that notices it being retired.
    CHECK(SourceContains("src/audio/audiorate.cpp", probe_replay::kWwiseNotInitialised));
    CHECK(SourceContains("src/audio/audio_module.cpp", "CSoundPlayer not resolved"));
    CHECK(SourceContains("src/audio/audio_module.cpp", "audio rate control not armed at bind time"));

    LogFacts noMixer;
    noMixer.OnLine("15:00:44.601 INFO [audio] audio rate control not armed at bind time "
                   "(re-armed each frame): Wwise is not initialised (GetAudioSettings refused)");
    noMixer.OnLine("15:00:44.600 WARN [audio] CSoundPlayer not resolved: audio state will not be "
                   "reconciled on a seek (this is the expected state under -noaudio)");
    noMixer.OnLine("15:01:05.331 INFO [audio] transport rate 0.50x: audio: slowed with picture");
    noMixer.OnLine("15:01:08.472 INFO [audio] transport rate 1.00x: audio: 1.0x, as the game mixed it");
    REQUIRE(noMixer.audioBindRefusalSeen);
    REQUIRE_FALSE(noMixer.soundPlayerResolved);
    REQUIRE(noMixer.rateHalfLineSeen);

    StatusFacts measured;
    measured.transportPresent = true;
    measured.durationUs = 236530968;
    measured.audioPresent = true;
    measured.audioAvailable = true;
    measured.slopeMeasured = true;
    measured.slope = 0.208;
    measured.slopeWindowMs = 872.0;

    // L3-RUN-7 scored this asked-fail off a clock no voice of ours was driving.
    CHECK(Ask1("w2h-audio-slope-at-half-rate", noMixer, measured) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no mixer in this process"));
    CHECK(DetailMentions("Wwise is not initialised (GetAudioSettings refused)"));
    CHECK_FALSE(DetailMentions("fail-mechanism"));

    // ★ THE REFUSAL HAS A TAIL. The same slope on a process that DID reach a mixer still fails,
    // and it now quotes the 0.50x line instead of the battery's last rate change.
    LogFacts mixer;
    mixer.OnLine("15:00:44.601 INFO [audio] CSoundPlayer resolved; state reconcile armed");
    mixer.OnLine("15:01:05.331 INFO [audio] transport rate 0.50x: audio: slowed with picture");
    mixer.OnLine("15:01:08.472 INFO [audio] transport rate 1.00x: audio: 1.0x, as the game mixed it");
    REQUIRE(mixer.soundPlayerResolved);
    CHECK(Ask1("w2h-audio-slope-at-half-rate", mixer, measured) == Ask::Fail);
    CHECK(DetailMentions("fail-mechanism"));
    CHECK(DetailMentions("transport rate 0.50x"));
    CHECK_FALSE(DetailMentions("transport rate 1.00x"));

    // A halved slope on the same session passes, on the same operand.
    StatusFacts halved = measured;
    halved.slope = 0.4991;
    CHECK(Ask1("w2h-audio-slope-at-half-rate", mixer, halved) == Ask::Pass);
    CHECK(DetailMentions("transport rate 0.50x"));

    // Rate lines, none of them at 0.50x: there is no half-rate command to date the slope to, and
    // reading the 1.00x line as one is the bug this clause exists to stop.
    LogFacts wrongRate;
    wrongRate.OnLine("15:00:44.601 INFO [audio] CSoundPlayer resolved; state reconcile armed");
    wrongRate.OnLine("15:01:08.472 INFO [audio] transport rate 1.00x: audio: 1.0x, as the game mixed it");
    CHECK(Ask1("w2h-audio-slope-at-half-rate", wrongRate, halved) == Ask::CouldNotAsk);
    CHECK(DetailMentions("none of them is a 0.50x one"));
}

TEST_CASE("W2-A: the gamestate stream, as the product's reader decoded it") {
    LogFacts f;
    StatusFacts unscanned;
    CHECK(Ask1("w2a-gamestate-decoded", f, unscanned) == Ask::CouldNotAsk);
    CHECK(DetailMentions("not read back through the product's reader"));

    StatusFacts refused;
    strcpy_s(refused.demoScanRefusal, "the product's reader refused 'x.echodemo': header magic");
    CHECK(Ask1("w2a-gamestate-decoded", f, refused) == Ask::CouldNotAsk);
    CHECK(DetailMentions("header magic"));

    // A width the probe never filled cannot be a mask to score against.
    StatusFacts widthless;
    widthless.demoScanned = true;
    CHECK(Ask1("w2a-gamestate-decoded", f, widthless) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no defined present-mask"));

    // The fixture's format is four fields wide. The row takes the width from the scan, so no
    // magnitude of the real one is written down here.
    StatusFacts empty;
    empty.demoScanned = true;
    empty.gsPresentAll = 0xF;
    strcpy_s(empty.demoPath, "W2-G-fixture.echodemo");
    CHECK(Ask1("w2a-gamestate-decoded", f, empty) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no S_GAMESTATE record"));

    StatusFacts good = empty;
    good.gsRecords = 6;
    good.gsPresentUnion = 0xF;
    good.gsPresentAlways = 0xD;
    good.gsPhasesSeen = 3;
    CHECK(Ask1("w2a-gamestate-decoded", f, good) == Ask::Pass);
    CHECK(DetailMentions("3 distinct phase(s)"));

    StatusFacts undefined = good;
    undefined.gsPresentUnion = 0x1F;
    undefined.gsUndefinedBits = 0x10;
    CHECK(Ask1("w2a-gamestate-decoded", f, undefined) == Ask::Fail);
    CHECK(DetailMentions("does not define"));

    StatusFacts phaseless = good;
    phaseless.gsPhasesSeen = 0;
    CHECK(Ask1("w2a-gamestate-decoded", f, phaseless) == Ask::Fail);
    CHECK(DetailMentions("names a phase"));

    StatusFacts wobbly = good;
    wobbly.gsTimerReversals = 2;
    CHECK(Ask1("w2a-gamestate-decoded", f, wobbly) == Ask::Fail);
    CHECK(DetailMentions("changes direction"));
}

TEST_CASE("W2-A: the playerstats stream, as the product's reader decoded it") {
    LogFacts f;
    StatusFacts unscanned;
    CHECK(Ask1("w2a-playerstats-decoded", f, unscanned) == Ask::CouldNotAsk);
    CHECK(DetailMentions("not read back through the product's reader"));

    StatusFacts empty;
    empty.demoScanned = true;
    empty.psFillAll = 0x7;
    strcpy_s(empty.demoPath, "W2-G-fixture.echodemo");
    CHECK(Ask1("w2a-playerstats-decoded", f, empty) == Ask::CouldNotAsk);
    CHECK(DetailMentions("no S_PLAYER_STATS record"));

    StatusFacts good = empty;
    good.psRecords = 114;
    good.psUsers = 1;
    good.psFillUnion = 0x5;
    CHECK(Ask1("w2a-playerstats-decoded", f, good) == Ask::Pass);
    CHECK(DetailMentions("114 S_PLAYER_STATS record(s) for 1 user(s)"));

    StatusFacts nobody = good;
    nobody.psUsers = 0;
    CHECK(Ask1("w2a-playerstats-decoded", f, nobody) == Ask::Fail);
    CHECK(DetailMentions("could name"));

    StatusFacts hollow = good;
    hollow.psFillUnion = 0;
    CHECK(Ask1("w2a-playerstats-decoded", f, hollow) == Ask::Fail);
    CHECK(DetailMentions("fill mask is EMPTY"));

    StatusFacts backwards = good;
    backwards.psTimeBacksteps = 3;
    CHECK(Ask1("w2a-playerstats-decoded", f, backwards) == Ask::Fail);
    CHECK(DetailMentions("BACKWARDS in engine time"));
}

TEST_CASE("W2-R: the step row's spread decides its own sentence") {
    // L3-RUN-7 F6: the row explained a zero spread with the sentence for a non-zero one.
    CHECK(strstr(probe_replay::StepSpreadReason(0), "ON a tick boundary") != nullptr);
    CHECK(strstr(probe_replay::StepSpreadReason(0), "mid-tick") == nullptr);
    CHECK(strstr(probe_replay::StepSpreadReason(17), "mid-tick") != nullptr);
    CHECK(strstr(probe_replay::StepSpreadReason(-17), "mid-tick") != nullptr);
}

// =============================================================================================
// W2-P3 -- the two rows ARCH-23 S9.3 puts on L3 #12.
// =============================================================================================

// The census lines quoted from src/engine/pawn_provision.cpp:328-431, filled with the numbers
// ENG-W2-P1-4 S5 says a healthy solo run must print: recorded slot 0 resolved to slot 1, because
// the viewer's own user holds slot 0 and `0->0` means the map never fired.
static const char* const kPawnHead =
    "12:00:20.000 INFO [pawn] census: provisioned 1, mapped 1, failed 0 -- state=known inmatch=yes";
static const char* const kPawnHeadStateUnknown =
    "12:00:20.000 INFO [pawn] census: provisioned 1, mapped 1, failed 0 -- state=UNKNOWN "
    "inmatch=unknown";
static const char* const kPawnPools =
    "12:00:20.001 INFO [pawn] census pools: remote CIs=1, phys CIs=1, local entries=1, diag=''";
static const char* const kPawnSlot =
    "12:00:20.002 INFO [pawn] census slot=1 (recorded 0) class=Local entity=none | remote: "
    "ci=ABSENT phys=- driveable=no | local: ci=present echo=present driveable=yes | model: "
    "resolved=yes visible=yes";
static const char* const kPawnSlotUnknown =
    "12:00:20.002 INFO [pawn] census slot=1 (recorded 0) class=Unknown entity=none | remote: "
    "ci=ABSENT phys=- driveable=no | local: ci=ABSENT echo=- driveable=no | model: resolved=no "
    "visible=no";
static const char* const kPawnSlotNoBody =
    "12:00:20.002 INFO [pawn] census slot=1 (recorded 0) class=Local entity=none | remote: "
    "ci=ABSENT phys=- driveable=no | local: ci=present echo=present driveable=no | model: "
    "resolved=yes visible=yes";
static const char* const kPawnHandles =
    "12:00:20.003 INFO [pawn] census slot=1 handles: remote_ci=65535 remote_phys=65535 "
    "local_echo=12 fill=0x0000003f actor=Broadcaster refusal=None disagree=no heldByFn=yes";
static const char* const kPawnHandlesRefused =
    "12:00:20.003 INFO [pawn] census slot=1 handles: remote_ci=65535 remote_phys=65535 "
    "local_echo=12 fill=0x0000003f actor=Broadcaster refusal=NoPhysComponent disagree=no "
    "heldByFn=yes";
static const char* const kPawnTotals =
    "12:00:20.004 INFO [pawn] census totals over 1 provisioned slot(s): remote CIs=0, local CIs=1, "
    "in BOTH=0";
static const char* const kPawnArm23 =
    "12:00:20.005 INFO [pawn] FACT-PP1 -> ARM (2)/(3): 1 provisioned entrant(s) on the local "
    "surface and NOT ONE carries an SR15RemotePlayerCI. PlayerOf() cannot resolve a provisioned "
    "pawn.";
static const char* const kPawnRefused =
    "12:00:20.001 ERROR [pawn] census REFUSED NoEntityService: diag='no game space'. No pool was "
    "read, so this census answers NOTHING about FACT-PP1.";
// THE NEAR-MISS CONTROL. Every key this row reads is one character away from a key this line
// carries: `subclass=` for `class=`, `undriveable=` for `driveable=`, `census slots=` for
// `census slot=`. A substring matcher scores this as a censused, driveable, Local slot.
static const char* const kPawnNearMiss =
    "12:00:20.002 INFO [pawn] census slot=1 (recorded 0) subclass=Local entity=none | remote: "
    "ci=present phys=present undriveable=yes | local: ci=present echo=present undriveable=yes | "
    "model: resolved=yes visible=yes";
static const char* const kPawnNearMissPrefix =
    "12:00:20.002 INFO [pawn] census slots=1 (recorded 0) class=Local entity=none | remote: "
    "ci=present phys=present driveable=yes | local: ci=present echo=present driveable=yes | "
    "model: resolved=yes visible=yes";

TEST_CASE("w2p-pawns-provisioned reads the census by token and refuses a near-miss line") {
    StatusFacts s;

    LogFacts silent;
    CHECK(Ask1("w2p-pawns-provisioned", silent, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never reached a state-9 edge"));

    // A refusal publishes no verdict, so it is a could-not-ask and never an answer about an arm.
    LogFacts refused;
    refused.OnLine(kPawnHead);
    refused.OnLine(kPawnRefused);
    CHECK(Ask1("w2p-pawns-provisioned", refused, s) == Ask::CouldNotAsk);
    CHECK(DetailMentions("NoEntityService"));

    // ENG-W2-P1-4 reading rule 4: a verdict with no census slot row above it is a bug.
    LogFacts verdictOnly;
    verdictOnly.OnLine(kPawnHead);
    verdictOnly.OnLine(kPawnPools);
    verdictOnly.OnLine(kPawnArm23);
    CHECK(Ask1("w2p-pawns-provisioned", verdictOnly, s) == Ask::Fail);
    CHECK(DetailMentions("NO `census slot=` row"));

    LogFacts stateless;
    stateless.OnLine(kPawnHeadStateUnknown);
    stateless.OnLine(kPawnPools);
    stateless.OnLine(kPawnSlot);
    stateless.OnLine(kPawnHandles);
    stateless.OnLine(kPawnTotals);
    CHECK(Ask1("w2p-pawns-provisioned", stateless, s) == Ask::Fail);
    CHECK(DetailMentions("wired without a state word"));

    LogFacts unknownClass;
    unknownClass.OnLine(kPawnHead);
    unknownClass.OnLine(kPawnPools);
    unknownClass.OnLine(kPawnSlotUnknown);
    unknownClass.OnLine(kPawnHandles);
    CHECK(Ask1("w2p-pawns-provisioned", unknownClass, s) == Ask::Fail);
    CHECK(DetailMentions("class=Unknown"));

    LogFacts noBody;
    noBody.OnLine(kPawnHead);
    noBody.OnLine(kPawnPools);
    noBody.OnLine(kPawnSlotNoBody);
    noBody.OnLine(kPawnHandlesRefused);
    CHECK(Ask1("w2p-pawns-provisioned", noBody, s) == Ask::Fail);
    CHECK(DetailMentions("NOT ONE reads a body back"));

    LogFacts good;
    good.OnLine(kPawnHead);
    good.OnLine(kPawnPools);
    good.OnLine(kPawnSlot);
    good.OnLine(kPawnHandles);
    good.OnLine(kPawnTotals);
    good.OnLine(kPawnArm23);
    CHECK(Ask1("w2p-pawns-provisioned", good, s) == Ask::Pass);
    CHECK(DetailMentions("provisioned 1, mapped 1, failed 0, resolved 1"));
    CHECK(DetailMentions("1 local, 0 remote, 0 unknown"));
    CHECK(DetailMentions("0->1"));
    CHECK(good.pawnDriveableRows == 1u);
    CHECK(good.pawnEntityNamed == 0u);
    CHECK(good.pawnRefusalRows == 0u);

    // The refusal on a handle row is counted and named without changing the verdict: the local
    // surface refused a component, and the slot is still driveable through the other one.
    LogFacts refusalNamed;
    refusalNamed.OnLine(kPawnHead);
    refusalNamed.OnLine(kPawnPools);
    refusalNamed.OnLine(kPawnSlot);
    refusalNamed.OnLine(kPawnHandlesRefused);
    refusalNamed.OnLine(kPawnTotals);
    CHECK(Ask1("w2p-pawns-provisioned", refusalNamed, s) == Ask::Pass);
    CHECK(DetailMentions("first NoPhysComponent"));

    // ---- the near-miss control -------------------------------------------------------------
    LogFacts nearMiss;
    nearMiss.OnLine(kPawnHead);
    nearMiss.OnLine(kPawnPools);
    nearMiss.OnLine(kPawnNearMiss);
    nearMiss.OnLine(kPawnNearMissPrefix);
    CHECK(nearMiss.pawnSlotRows == 0u);
    CHECK(nearMiss.pawnClassLocal == 0u);
    CHECK(nearMiss.pawnDriveableRows == 0u);
    CHECK(Ask1("w2p-pawns-provisioned", nearMiss, s) == Ask::Fail);
    CHECK(DetailMentions("NOT ONE carried a resolved slot"));
    // ...and the control is not vacuous: the SAME head and pools with the real line pass.
    CHECK(Ask1("w2p-pawns-provisioned", good, s) == Ask::Pass);
}

TEST_CASE("w2b-body-decoded scores the applier's own verdict on every track it walked") {
    LogFacts f;

    StatusFacts unsampled;
    CHECK(Ask1("w2b-body-decoded", f, unsampled) == Ask::CouldNotAsk);
    CHECK(DetailMentions("never read ApplyStatsSnapshot()"));

    StatusFacts noTracks;
    noTracks.bodyApplySampled = true;
    noTracks.bodyApplyPolls = 42;
    CHECK(Ask1("w2b-body-decoded", f, noTracks) == Ask::CouldNotAsk);
    CHECK(DetailMentions("not one of them found a body track"));

    StatusFacts noneDriven;
    noneDriven.bodyApplySampled = true;
    noneDriven.bodyApplyPolls = 42;
    noneDriven.bodyUnhandled = 2;
    noneDriven.bodyAbsent = 1;
    CHECK(BodyTracksWalked(noneDriven) == 3u);
    CHECK(Ask1("w2b-body-decoded", f, noneDriven) == Ask::Fail);
    CHECK(DetailMentions("NOT ONE pose was written"));

    StatusFacts stillSimulated;
    stillSimulated.bodyApplySampled = true;
    stillSimulated.bodyApplyPolls = 42;
    stillSimulated.bodyInstalled = 1;
    stillSimulated.bodyLocallyControlled = 1;
    stillSimulated.bodyOwnerClaimed = 1;
    CHECK(Ask1("w2b-body-decoded", f, stillSimulated) == Ask::Fail);
    CHECK(DetailMentions("LOCALLY CONTROLLED"));

    StatusFacts refusedInstall;
    refusedInstall.bodyApplySampled = true;
    refusedInstall.bodyApplyPolls = 42;
    refusedInstall.bodyInstalled = 1;
    refusedInstall.bodyReadBack = 1;
    refusedInstall.bodyInstallFailed = 1;
    refusedInstall.bodyIdentityRefused = 1;
    CHECK(Ask1("w2b-body-decoded", f, refusedInstall) == Ask::Fail);
    CHECK(DetailMentions("SetHistoryWindow refused"));
    CHECK(DetailMentions("by name CiIdentityMismatch"));
    CHECK(DetailMentions("install_failed 1, identity_refused 1"));

    StatusFacts driven;
    driven.bodyApplySampled = true;
    driven.bodyApplyPolls = 137;
    driven.bodyInstalled = 2;
    driven.bodyReadBack = 2;
    driven.bodyAbsent = 1;
    driven.bodyOwnerClaimed = 2;
    CHECK(BodyTracksWalked(driven) == 3u);
    CHECK(Ask1("w2b-body-decoded", f, driven) == Ask::Pass);
    CHECK(DetailMentions("3 body track(s) decoded and walked, 2 driven-pose write(s)"));
    CHECK(DetailMentions("SAMPLES PER TRACK is NOT PUBLISHED"));
    CHECK(DetailMentions("installed_and_read_back 2, installed_but_unread 0, install_failed 0, "
                         "identity_refused 0"));
}

// ENG-RESIDUALS-1 (a). The residual POSEREAD left: `bodies_installed` counts a call that returned
// true, and the row read it as a driven-pose write. These two fixtures differ from `driven` above
// ONLY in the read-back, so a row that scored the install alone would call them both a pass.
TEST_CASE("w2b-body-decoded passes on the READ-BACK, never on the install's own return value") {
    LogFacts f;

    // Every install returned true and the engine's ring gave none of them back. This is the exact
    // window a blind installer produced, and it must not read as 2 driven-pose writes.
    StatusFacts unread;
    unread.bodyApplySampled = true;
    unread.bodyApplyPolls = 137;
    unread.bodyInstalled = 2;
    unread.bodyInstalledUnread = 2;
    unread.bodyAbsent = 1;
    unread.bodyOwnerClaimed = 2;
    CHECK(BodyTracksWalked(unread) == 3u);
    CHECK(Ask1("w2b-body-decoded", f, unread) == Ask::Fail);
    CHECK(DetailMentions("gave back NOT ONE of them"));
    CHECK(DetailMentions("installed_and_read_back 0, installed_but_unread 2"));
    // ...and it is not the driven text with a different verdict word.
    CHECK_FALSE(DetailMentions("driven-pose write(s) confirmed by read-back"));

    // One of the two came back. The row passes, and says how many of each.
    StatusFacts partial = unread;
    partial.bodyReadBack = 1;
    partial.bodyInstalledUnread = 1;
    CHECK(Ask1("w2b-body-decoded", f, partial) == Ask::Pass);
    CHECK(DetailMentions("installed_and_read_back 1, installed_but_unread 1"));

    // A single install the identity gate refused is asked-FAIL, never "1 driven-pose write".
    StatusFacts refused;
    refused.bodyApplySampled = true;
    refused.bodyApplyPolls = 12;
    refused.bodyInstalled = 0;
    refused.bodyInstallFailed = 1;
    refused.bodyIdentityRefused = 1;
    CHECK(Ask1("w2b-body-decoded", f, refused) == Ask::Fail);
    CHECK(DetailMentions("NOT ONE pose was written"));
    CHECK(DetailMentions("install_failed 1, identity_refused 1"));
    CHECK_FALSE(DetailMentions("driven-pose write(s) confirmed by read-back"));
}

// ENG-TICKS-ROW / POSEREAD minor 9. The Pass arm's own SENTENCE, not only its verdict word: the
// number in front of "driven-pose write(s)" is the read-back count, so a session that installed
// three and had one confirmed may not print "3". These two fixtures share `bodyInstalled = 3` and
// differ only in what came back, which is the whole difference the sentence has to carry.
TEST_CASE("w2b-body-decoded counts driven-pose writes by read-back, not by install") {
    LogFacts f;

    StatusFacts none;
    none.bodyApplySampled = true;
    none.bodyApplyPolls = 90;
    none.bodyInstalled = 3;
    none.bodyInstalledUnread = 3;
    CHECK(Ask1("w2b-body-decoded", f, none) == Ask::Fail);
    CHECK(DetailMentions("3 of 3 body track(s) installed and the engine's ring gave back NOT ONE "
                         "of them"));
    CHECK(DetailMentions("installed_and_read_back 0, installed_but_unread 3"));
    CHECK_FALSE(DetailMentions("driven-pose write(s) confirmed by read-back"));

    StatusFacts partial = none;
    partial.bodyReadBack = 2;
    partial.bodyInstalledUnread = 1;
    CHECK(Ask1("w2b-body-decoded", f, partial) == Ask::Pass);
    CHECK(DetailMentions("2 driven-pose write(s) confirmed by read-back (1 installed but unread)"));
    CHECK(DetailMentions("installed_and_read_back 2, installed_but_unread 1"));
    // The install count is 3 and must not be the number in that sentence.
    CHECK_FALSE(DetailMentions("3 driven-pose write(s)"));
}

// The locally-controlled Fail arm carried its own pose-write phrase fed by
// `bodies_installed`, so one session could be described with two different numbers depending on
// which arm scored it. It now renders through the same DrivenPoseWrites formatter as the Pass arm:
// the number in front of "driven-pose write(s)" is the read-back count on EVERY arm that has one.
TEST_CASE("w2b-body-decoded's locally-controlled Fail arm counts pose writes by read-back") {
    LogFacts f;

    StatusFacts simulated;
    simulated.bodyApplySampled = true;
    simulated.bodyApplyPolls = 90;
    simulated.bodyInstalled = 2;
    simulated.bodyInstalledUnread = 2;
    simulated.bodyLocallyControlled = 1;
    simulated.bodyOwnerClaimed = 3;
    CHECK(BodyTracksWalked(simulated) == 3u);
    CHECK(Ask1("w2b-body-decoded", f, simulated) == Ask::Fail);
    CHECK(DetailMentions("1 of 3 body track(s) are still LOCALLY CONTROLLED"));
    CHECK(DetailMentions("with 0 driven-pose write(s) confirmed by read-back (2 installed but "
                         "unread) beside it"));
    // Two installs returned true and the ring gave back neither: the arm may not headline "2".
    CHECK_FALSE(DetailMentions("2 driven-pose write(s)"));

    StatusFacts confirmed = simulated;
    confirmed.bodyReadBack = 2;
    confirmed.bodyInstalledUnread = 0;
    CHECK(Ask1("w2b-body-decoded", f, confirmed) == Ask::Fail);
    CHECK(DetailMentions("with 2 driven-pose write(s) confirmed by read-back (0 installed but "
                         "unread) beside it"));
}
