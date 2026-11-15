// P-1: the capture module. The tick gathers, the taps deliver, the worker chapterises and writes.
//
// ---------------------------------------------------------------------------------------------
// â˜… WHAT RUNS WHERE, AND WHY THE TICK DOES SO LITTLE.
//
// CDeadlockMonitor kills the process with a deliberate MEMORY[0] = 0 after ten real seconds without a
// Tick() from RunLoop (R8 S6), and a WriteFile that meets a cold NTFS metadata path or an antivirus
// filter can block for hundreds of milliseconds. A blocking write on the tick is a crash, not a hitch.
// So the tick reads component structs into PODs and memcpys them into a ring, and does nothing else:
// no allocation, no lock, no syscall, no filesystem, no compression. S6.4's trap is compiled into the
// release build behind capture.trap so the zero-allocation half and the timing half can hold in the
// same binary rather than in two.
//
// â˜… AND WHAT THIS MODULE DELIBERATELY DOES NOT DO YET, stated here rather than discovered later:
//
//  * WHICH PLAYER FIELDS ARE SOURCED IS NOT WRITTEN DOWN HERE, ON PURPOSE. This comment used to carry
//    that list and the audit found it wrong in both directions at once: it named trigL/trigR/anchor as
//    missing after W1-E had started capturing them, and did not mention the action word or the hold
//    ids, which really were missing. A stale gap list is worse than none -- it is a map saying the road
//    is closed where it is open. The list is now READ from the mask IEntity returns, logged once per
//    session by ReportGapsOnce, and recorded per row in the file's own fill mask. Both statements
//    are derived from the engine, so neither can drift from it.
//  * THE ORACLE TAP IS CREATED, VERIFIED AND SAMPLED HERE (S5.4.7, ruling Q-B). It is NOT bound on
//    the shipped path, and it does not pretend to be: the two producers S5.4.7 names are ENGINE
//    addresses (the /session handler's two arms and the /player_bones builder), no module under
//    src/ produces either document, and their prototypes are DL-22 / V-I-W2I1 owned by W2-I.
//    BindBuilders is therefore NOT called when OracleBuilders() has no pair to give it -- binding
//    two nulls is a no-op wearing the shape of a bind. Sampling happens anyway: one OracleRec per
//    CORE chapter carrying TapStatus::NotBound, so the reason the payloads are empty is IN THE FILE
//    rather than a stream that silently never appears, and the same NotBound(<producers>) sentence
//    is on the log line, on the stop line and in the close document.
//    The CLOCK is real and needs no bind: oracletap defaults to W1-D's tick::ReadEngineClock, which
//    is the engine's own CClock+0x28 and CncaGame+0x830. IReplayClock is W2-D's PLAYBACK transport
//    clock -- it publishes no sim time and no frame index, so it cannot answer the bracket at all.
//  * VOIP IS W3-C's. The section, its default and its config gate are wired here; the ReceiveVoip
//    detour that fills it is that packet's row.
// ---------------------------------------------------------------------------------------------

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <type_traits>
#include <vector>

#include "bootstrap/bootstrap.h"
#include "bootstrap/buildguard.h"   // the identity the header records; measured once at load
#include "bootstrap/livesafety.h"
#include "bootstrap/log.h"   // kLogLineMax: the row list is sized from the log's own ceiling
#include "bootstrap/module_order.h"
#include "bootstrap/modreg.h"
#include "bootstrap/paths.h"
#include "bootstrap/services.h"
#include "capture/capturesecs.h"
#include "capture/capturestop.h"
#include "capture/capture_types.h"
#include "capture/chapterbuilder.h"
#include "capture/netvarsnapshot.h"
#include "capture/costmeter.h"
#include "capture/levelid.h"
#include "capture/gametypeid.h"
#include "capture/gather.h"
#include "capture/identity_emit.h"
#include "capture/duv.h"
#include "capture/manifestbuild.h"
#include "capture/oracleregistry.h"
#include "capture/playerstats.h"
#include "capture/roster.h"
#include "capture/preopen.h"
#include "capture/worldorigin.h"
#include "audio/audiopinsource.h"
#include "engine/oracletap.h"
#include "audio/voip/voipcapture.h"
#include "capture/ringset.h"
#include "capture/streamcodec.h"
#include "engine/asseen.h"
#include "engine/entity.h"
#include "engine/gamestate.h"
#include "engine/netvar.h"
#include "engine/session.h"
#include "engine/identity_access.h"
#include "bootstrap/inbound.h"
#include "engine/tick.h"
#include "engine/wiretap.h"
#include "foundation/demofile/demofile.h"
#include "foundation/ring/alloc_trap.h"

static const char* kReplayModuleName = "capture";

namespace replay {
namespace capture {
namespace {

// S6.4 enforcement 1: 8 MiB, sized once at load and never touched again. S11.1 replaced the old 64 MiB
// with this because the worker drains at ~25 KB/s and could never accumulate a backlog that large
// before CDeadlockMonitor fires at ten seconds -- it was half the resident budget standing idle.
constexpr uint32_t kRingBytes = 8u * 1024u * 1024u;
constexpr uint32_t kWorkerScratchBytes = 64u * 1024u;
constexpr uint32_t kWorkerDrainPerPass = 4096;
constexpr DWORD kWorkerIdleMs = 4;

enum class Phase : uint8_t { Idle, Arming, Recording, Closing };

// â˜… AUDIT_M6 F2. Which stage of OpenFile refused, so the retry can tell a state CHANGE from another
// instance of the same refusal and log once per state rather than once per attempt.
enum class OpenFailKind : uint8_t { None, SchemaRefused, WriterOpen, BuilderRefused };

const char* OpenFailStateName(OpenFailKind k) {
    switch (k) {
        case OpenFailKind::SchemaRefused:  return "schema-refused";
        case OpenFailKind::WriterOpen:     return "writer-open";
        case OpenFailKind::BuilderRefused: return "builder-refused";
        case OpenFailKind::None:           break;
    }
    return "none";
}

// The retry schedule for a capture that cannot be opened. The tick re-arms the open on the very next
// frame and the worker's idle pass is 4 ms, so an unbounded retry is ~250 open attempts and (since
// AUDIT_M5 F5 gave every failure a line) ~250 Error lines PER SECOND for a whole match. Doubling from
// a quarter second to a half minute makes that six attempts in the first ten seconds and about one a
// minute thereafter, which is still a capture that recovers on its own if the disk comes back.
constexpr uint32_t kOpenRetryFirstMs = 250;
constexpr uint32_t kOpenRetryCapMs = 30000;

// How many directories one launch may watch for `stop-capture` and leave its sentinels in. The
// declaration, both appends and the Init line that renders the list take the extent from here
// instead of each carrying its own copy of the number.
constexpr uint32_t kStopWatchMax = 2;

// The duration one injected cost sample claims. Taken from the meter's own hard-fail budget rather
// than as a magnitude of its own, so a window of them is over the budget by construction and stays
// over it if the budget ever moves.
constexpr uint32_t kCostInjectSlowUs = 4 * CostMeter::kP999HardFailUs;

// ---------------------------------------------------------------------------------------------
// â˜… THE PER-PHASE TICK BREAKDOWN, because L2's kill line named a COST and no PHASE.
//
// The kill reported a window median of 2000-4506 us and the file it wrote reports players: 0, so the
// gather loops -- the obvious suspect -- ran empty. That leaves the FIXED per-tick work, and a total
// with no breakdown cannot tell a slow world-origin read from a slow session probe. Six brackets, the
// same RDTSC clock the cost meter calibrates, summed over the same 512-tick window, published to the
// worker and logged there: the tick thread never formats or writes anything (S6.4: no syscalls while
// armed). Six __rdtsc reads per tick is about 60 ns against a budget of 20 us.
// ---------------------------------------------------------------------------------------------
enum TickPhase : uint32_t {
    kPhaseGate = 0,   // TrapArm, the session state read, the open/close transitions
    kPhasePeer,       // ReadPeerTime: the as-seen sample behind the time axis
    kPhaseOrigin,     // ReadWorldOrigin
    kPhaseSpine,      // the world-origin and tick records, staged and pushed
    kPhaseGather,     // GatherEntities: bodies and local pawns
    kPhaseRoster,     // ScanRosterTick: the entrant-count gate, and the scan when it moves
    kPhaseIdentity,   // EmitIdentityTick: the identity blocks, including the profile serialisation
    kPhaseAsSeen,     // GatherAsSeenFrame
    kPhaseGameState,  // SampleGameState: ARCH-21's local sample, the marker source
    kPhaseNetvarSnap, // NetvarSnapshotCache::SampleOnTick: the HUD keys' value, for the keyframe
    kPhaseStats,      // SamplePlayerStats: the live scoreboard, behind its own change gate
    kPhaseAudioPins,  // PublishAudioPinsTick: the audio pin walk, at a fraction of keyframe rate
    kPhaseOracle,     // SampleOracleTick: E-7's bracketed /session + /player_bones snapshot
    kPhaseCount,
};
// L2-RUN-5 F1: the roster gets its own bracket. It was inside `gather`, so a subsystem that grew
// 7.5x named a neighbour in the kill line and nothing in the phase table could localise it. A phase
// per subsystem is what makes the next regression say its own name.
const char* const kTickPhaseName[kPhaseCount] = {"gate",   "peer",   "origin",  "spine",
                                                 "gather", "roster", "identity", "asseen",
                                                 "gamestate", "nvsnap", "stats", "audiopins",
                                                 "oracle"};
// Half the default keyframe interval. The snapshot only ever supplies keys no recorded delta has
// spoken for, so a sample a fraction of a second old is the same answer as one taken at the
// keyframe itself -- and sampling on a cadence keeps the read off the chapter-boundary tick.
constexpr uint64_t kNetvarSnapshotIntervalUs = 1000000u;

// The two halves of `gather`, bracketed INSIDE it rather than promoted to phases of their own: the
// kill rung reads the phase totals, and splitting `gather` there would silently change what the
// charged sum is taken over.
enum GatherPart : uint32_t { kGatherBodies = 0, kGatherPawns, kGatherPartCount };
const char* const kGatherPartName[kGatherPartCount] = {"bodies", "pawns"};

struct PhaseWindow {
    uint64_t tsc[kPhaseCount];
    uint32_t maxUs[kPhaseCount];
    uint32_t ticks;
    // Of those ticks, the ones a capture was OPEN for. L2-RUN-8 F12 read two windows logged
    // after the file had closed and could not tell "the capture still costs 108 us" from "this
    // window is mostly ticks that did no capture work at all", because the charged mean was
    // divided by every tick in the window either way. Charged work only happens on these ticks,
    // so these are the ticks it is a mean over.
    uint32_t openTicks;
    // L3-RUN-16 F1: `gather` was 97 % of the charge and this line could not say which half of it,
    // nor whether the cost was ours at all. tests/capture already establishes that the capture-side
    // gather code is nanoseconds, so what is left is the ENGINE READS the gather makes per body and
    // per pawn -- and `replay::Readable` is a VirtualQuery whose REFUSAL is deliberately not cached
    // (entity.h), so a pool slot that refuses is a syscall every call, every tick, for the life of
    // the binding. The two sub-brackets say which half; the probe count says whether that half is
    // spending syscalls or walking. Two counter reads and two TSC reads a tick.
    uint64_t gatherTsc[kGatherPartCount];
    uint64_t gatherProbes;
};

struct Module {
    ReplayServices* svc = nullptr;
    ILog* log = nullptr;
    const IConfig* cfg = nullptr;

    // ---- the ring, sized once -----------------------------------------------------------------
    std::vector<uint8_t> ringMemory;
    RingSet rings;

    // ---- the worker ---------------------------------------------------------------------------
    HANDLE thread = nullptr;
    std::atomic<bool> quit{false};
    std::atomic<Phase> phase{Phase::Idle};
    std::atomic<bool> openRequested{false};
    // â˜… AUDIT_M5 F1, then AUDIT_M6 F3. A close is requested BY CAUSE. Two threads ask -- the match
    // end on the tick and the cost meter's rung-5 kill on the worker -- and the close used to
    // re-derive the cause at the point it fired, which meant every close claimed to be a match end.
    // A flag plus one reason byte then carried the cause but not both causes: when both threads
    // asked inside the same worker pass the loser's word was dropped with no trace, and the loser is
    // usually the cost kill, whose Error line is the only place the meter is ever reported. One bit
    // per StopReason instead: set with fetch_or(release), taken with exchange(acq_rel), so the mask
    // is both the request and the complete list of what asked for it.
    std::atomic<uint8_t> closeCauses{0};
    // Zero in every launch; only tests/capture's InjectCostWindowsForTest sets it. The
    // worker drains it on its own thread -- the meter is single-threaded state and must stay that
    // way -- and everything after the samples land is the shipped path: Evaluate decides the rung,
    // the rung sets the CostKill bit, the exchange takes it and StopCapture writes the document.
    std::atomic<uint32_t> costInjectWindows{0};

    // PROCESS-wide, and deliberately NOT in Init's per-launch reset block: ITick has
    // no unregister and src/engine/tick.cpp appends without dedupe, so a second Init that registered
    // again would gather every engine frame twice, and an Nth would fill the callback table until
    // RegisterEarly refused and capture would not initialise at all. The callback is the same
    // function reading the same globals in every session, so the first registration serves them all.
    bool tickRegistered = false;

    // Set at the one SetRoundTable call site, so the sentinel's `roundRowCount` is present exactly
    // when the table was filed. A count copied without this flag cannot say whether a zero means an
    // empty table or a close that never got that far.
    bool roundRowsRecorded = false;

    // ---- the capture deadline --------------
    // Written by Init before the worker thread exists and read by the worker afterwards, so these
    // need no synchronisation of their own; deadlineFired is the worker's alone.
    // Atomic because tests/capture arms it after Init to drive the deadline path without
    // waiting out kCaptureSecsMin; the worker reads it every pass.
    std::atomic<uint64_t> deadlineAtMs{0};   // 0 is not armed, which is the ordinary case
    uint32_t deadlineSecs = 0;
    uint32_t deadlineRefusals = 0;
    bool deadlineFired = false;

    // ---- the capture stop -------------------------
    // Published by the tick so the worker -- which must never ask the engine anything -- can say in
    // the sentinel whether a match was still live when the capture stopped.
    std::atomic<bool> inGame{false};
    // Set by the worker when a stop closes a file, cleared by the tick when the match ends. It is
    // what keeps a stop from opening a SECOND file for the SAME match: one file per match is a
    // locked constraint, and the tick's Idle+inGame edge would otherwise re-arm on the next frame.
    std::atomic<bool> suppressReopen{false};
    // Worker-only. Where the harness may drop `stop-capture`, resolved once at Init and logged, so
    // nothing has to guess which directory this process considers its own.
    wchar_t stopWatchDirs[kStopWatchMax][kPathMax] = {};
    uint32_t stopWatchCount = 0;
    uint64_t nextStopPollMs = 0;
    // This launch's identity (the process creation time): a stop-capture older than it is somebody
    // else's request, and it names the sentinel this launch is allowed to leave behind.
    uint64_t launchFileTime = 0;
    // â˜… AUDIT_M6 F1. Worker-only, and it gates the stop-REQUEST poll ONLY: `stop-capture` is left on
    // disk after it is honoured, so without a one-shot the poll would ask for another close every
    // second for the rest of the launch. It decides nothing about sentinels; every close writes one.
    bool stopRequestSeen = false;
    // â˜… AUDIT_M6 F2. Worker-only. The open-retry's degraded state: how many consecutive attempts
    // have failed, which stage refused (so one Error line is written per state change rather than
    // per attempt), when the next attempt is allowed, and how long the current backoff is.
    OpenFailKind openFailKind = OpenFailKind::None;
    uint32_t openFailStreak = 0;
    uint32_t openFailTotal = 0;   // over the whole launch, never cleared by a success: the sentinel's
    uint64_t openRetryAtMs = 0;
    uint32_t openBackoffMs = 0;
    // How many times this launch has closed, counted from 1 at the first close. Names the per-close
    // sentinel and is written into it. Atomic because Shutdown reserves an ordinal of
    // its own when the worker does not join: that is the one close the worker cannot
    // write, and its name must not collide with one a stuck worker may still be about to use.
    std::atomic<uint32_t> closeSeq{0};
    // The `closed:` line's counts, stashed by CloseFile so the line can be written after the sentinel.
    uint64_t closedRecords = 0;
    uint64_t closedChapters = 0;
    uint64_t closedKeyframes = 0;
    uint64_t closedVerbatim = 0;
    uint64_t closedRefusals = 0;
    uint32_t closedRefusalsNamed = 0;
    EncodeRefusalNote closedRefusalNotes[BuilderStats::kMaxRefusalsNamed] = {};
    // The marker deriver's counters as they stood when the file closed. Stashed here for the same
    // reason the counts above are: the builder is reset for the next capture before the sentinel is
    // written, so reading them at the sentinel would read the wrong capture's.
    MarkerDeriver::Stats closedMarkers{};
    // ARCH-21 S4.4's three-state answer, snapshotted at the same instant as closedMarkers so a
    // sentinel can never pair this file's marker counts with the previous file's source.
    LocalMarkerSource::Stats closedLocal{};
    MarkerSource closedSource = MarkerSource::NotRecorded;
    bool closedCorroboratorAsked = false;
    uint32_t closedWireRows = 0;
    bool closedLinePending = false;

    // ---- engine surfaces ----------------------------------------------------------------------
    IWiretap* tap = nullptr;
    INetVar* netvar = nullptr;
    uint16_t taps[64] = {};
    uint16_t tapCount = 0;
    // Registration is a pass that runs again on later ticks. W1-L's Listen refuses with
    // kInvalidHandle when the engine's CR15NetGame listener lock was still held at its 2000 us
    // deadline -- a refusal about that instant only -- so a single pass silently drops streams for
    // the life of the process. `tapDone` marks a type settled (registered, not ours, or abandoned)
    // and is the only thing that stops it being tried again.
    uint8_t  tapAttempts[64] = {};
    bool     tapDone[64] = {};
    // â˜… AUDIT_M4 B1. SEEDED BY Init, never left at the member default, and the settled-guard in
    // RegisterSurfaces is why. A zero here has to mean "the walk settled everything"; if it can also
    // mean "no walk has ever counted anything" then the first tick on which the netvar listener spends
    // the whole deadline -- and the fifth such tick settles the netvar -- latches that guard shut
    // before a single Listen was issued for any of the 36 tap types, for the life of the process.
    uint16_t tapsPending = 0;
    uint16_t tapsAbandoned = 0;
    // Terminal: IWiretap::Available() was false, decided when the tap was built and unchanged after.
    // The types still unsettled are then UNAVAILABLE, not deferred -- nothing will ask for them again
    // -- and this doubles as the key that keeps the Warn to one line.
    bool     tapUnavailable = false;
    // Whether a Listen was ever ISSUED, for any surface. Not "a tap object exists": a tap whose
    // Available() is false is built and asks nothing, and reporting that as registration-attempted is
    // the lie AUDIT_M4 M2 names.
    bool     tapListenAttempted = false;
    // The count the last `tap-registration-deferred` line carried. Zero means none was written yet
    // and needs no sentinel of its own: the line is only ever written when the count is above zero.
    // Pending only ever falls, so keying the line on a change bounds it at one line per type ever
    // settled -- a lock held for a thousand ticks says so once, not a thousand times.
    uint16_t tapStopPendingLogged = 0;
    uint16_t tapGivenUpLogged = 0;
    // The S_NETVAR listener is INetVar's handle, but its one registration draws on the same per-tick
    // lock budget as the tap walk, so its bookkeeping lives beside the taps'. `netvarDone` means
    // settled: registered, given up on, or refused for a reason another attempt cannot change.
    uint8_t  netvarAttempts = 0;
    bool     netvarDone = false;
    bool     netvarRegistered = false;
    // The answer that settled it. TapUnavailable is terminal for a reason no retry can change and no
    // attempt was made for, so it is reported as unavailable rather than given-up.
    netvar::Status netvarStatus = netvar::Status::ListenPending;

    // ---- config -------------------------------------------------------------------------------
    BuilderConfig builder;
    bool trapArmed = false;
    bool captureEnabled = true;
    bool autoRecord = true;      // capture.auto_record
    bool forceOffline = false;   // capture.force_offline
    bool recIndicator = true;    // capture.rec_indicator
    std::atomic<bool> manualStart{false};   // `record start`, consumed by the next tick
    bool sessionGateSaid = false;           // the classification is named once per session
    wchar_t replayDir[kPathMax] = {};
    // The file the worker is writing now, for the clip service (another thread). Guarded by curLock.
    SRWLOCK curLock = SRWLOCK_INIT;
    wchar_t curPath[kPathMax] = {};

    // ---- session state, written by the tick, read by the worker --------------------------------
    struct OpenParams {
        uint64_t t0Us;
        uint64_t levelId;
        uint64_t gametypeId;
        uint64_t authorityFlags;
        uint32_t captureRole;
        bool partialStart;
    };
    OpenParams pending = {};

    // ---- diagnostics --------------------------------------------------------------------------
    CostMeter cost;
    std::atomic<uint64_t> ticks{0};
    std::atomic<uint64_t> pushFailures{0};
    // Per RECORD KIND, because the ring cannot know what it refused and the worker cannot ask it.
    // Charging every ring drop to one stream keeps the file's total right and moves the loss between
    // the things being counted, which is the hardest class of error to notice -- and the producer is
    // holding the answer at the moment it drops the record.
    std::atomic<uint32_t> pushFailByKind[kRecKindMax + 1] = {};
    std::atomic<uint32_t> tickThread{0};
    // The recording's time axis, published for the threads that are not the tick. The VoIP detour runs
    // on the engine's audio thread and must NOT stamp its own clock: a second clock measuring the same
    // frames is how a drift nobody can attribute gets into the file.
    std::atomic<uint64_t> lastSourceUs{0};
    uint64_t prevFrameIndex = 0;
    bool localPawnWarned = false;
    bool bodyNetIdWarned = false;
    bool bodyGapWarned = false;
    // The ticks that did capture work: the ONE counter the tick record and the body walk are both
    // charged under, so no denominator derived from either can drift from the other.
    uint64_t workTicks = 0;
    uint32_t bodiesSampledTotal = 0;
    uint32_t bodiesRefusedTotal = 0;
    uint32_t bodiesFromLocalTotal = 0, bodiesFromHistoryTotal = 0;
    uint32_t bodiesSourceNoneTotal = 0, bodiesSourceNotAskedTotal = 0;
    uint32_t angVelByTotal[kAngVelSourceCount] = {};
    uint32_t angVelUnknownTotal = 0;
    uint32_t bodyRefusalTotals[kBodyRefusalCount] = {};
    uint32_t bodyRefusalUnknownTotal = 0;
    // The first refused body read of THIS CAPTURE, not of the tick that happened to refuse
    // last. Latched here rather than in the per-tick report because the report is rebuilt
    // every tick and the question the stop line answers is about the whole capture.
    bool bodyFirstRefusalSeen = false;
    replay::BodyReading bodyFirstRefusal{};
    bool fillReported = false;
    bool armedOnce = false;

    // Worker-side only.
    DemoWriter writer;
    ChapterBuilder builder2;
    ManifestBuilder manifest;
    // The roster, held for the whole capture rather than rebuilt per keyframe: a player who
    // LEFT is still part of the recording and a rebuilt table forgets them.
    RosterTable roster;
    uint32_t rosterAppended = 0, rosterTeamChanges = 0, rosterDepartures = 0;
    uint32_t rosterScansUnchanged = 0, rosterBandRefusals = 0;
    // Ticks the cheap entrant-count gate answered without scanning, and entrants the engine counted
    // past this table. Both were counted nowhere a reader could see them (L2-RUN-5 F1, AUD-ARCH21-B).
    uint32_t rosterSignalUnchanged = 0, rosterBandClamped = 0;
    RosterCadence rosterCadence = {};
    // The identity block producer (ARCH-23 s14.8b): one block per entrant, written when that
    // entrant's block content moves. Its per-row signatures are reset at every file open, because a
    // table held across two files would otherwise leave the second file carrying no identity at all.
    IdentityEmitter identityEmit;
    uint32_t identityBlocks = 0, identityJsonRefused = 0, identityKeyless = 0;
    uint32_t identityTruncated = 0, identityTruncationUnknown = 0, identityDeferred = 0;
    const char* identityLastJsonWhy = nullptr;
    bool identityRefusalWarned[kIdentityEmitRefusalCount] = {};
    uint32_t identityRefusals[kIdentityEmitRefusalCount] = {};
    uint16_t identityTypeIndex = kTapTypeIndexNone;
    bool identityTypeWarned = false, identityKeylessWarned = false;
    RosterRefusals rosterRefusals = {};
    bool rosterSourceWarned = false, rosterBandWarned = false;
    // ARCH-21 MAJOR-B2, the live scoreboard. The shadow store is held for the whole capture for the
    // same reason the roster table is: a player who left still has a last score, and a rebuilt store
    // would re-emit every stat of every remaining player as a change the moment it was rebuilt.
    PlayerStatsStore playerStats = {};
    uint64_t statsRecords = 0, statsKeyframes = 0, statsSweeps = 0;
    // Ticks the cheap signal gate answered without issuing one engine read, and ticks on which there
    // was no source at all. The first is the fix L2-RUN-5 F1 asks for, stated as its own number so a
    // regression to a per-tick sweep is visible in the sentinel and not only in the cost meter.
    uint64_t statsSignalUnchanged = 0, statsSourceAbsent = 0;
    StatsRefusals statsRefusals = {};
    bool statsSaidSource = false;
    // The uuid the OPEN minted, kept so the per-close sentinel can name the file it reports
    // on (L2-RUN-3 F8). Cleared nowhere: after a close it still identifies what was closed.
    DemoUuid lastUuid = {};
    bool fileOpen = false;
    // Worker-only. The stop has to name the file it closed, and it has to be able to name one even
    // when the match ended first and there is nothing open by the time the deadline arrives -- a
    // sentinel reporting "nothing was recording" beside a perfectly good capture is a false negative
    // on the one artifact the launch exists to produce.
    wchar_t openPath[kPathMax] = {};
    wchar_t lastClosedPath[kPathMax] = {};
    uint64_t lastRecords = 0;
    uint64_t lastChapters = 0;
    std::vector<uint8_t> scratch;
    // One-shot identity and session messages that arrived BEFORE the file existed. They are the whole
    // of S5.4.1's content, so they are held and replayed into the first chapter rather than lost --
    // a match's identity arrives during the lobby, which is before state 9 by construction.
    PreOpenBuffer preOpen;
    bool preOpenLossDeclared = false;

    // The tick fills `phaseOpen`; at a window boundary it copies into `phaseDone` and bumps the seq.
    // The worker logs whatever the last complete window measured. One writer, one reader, and the
    // seq is the release that orders the copy before the read.
    PhaseWindow phaseOpen = {};
    PhaseWindow phaseDone = {};
    std::atomic<uint64_t> phaseSeq{0};
    uint64_t phaseLogged = 0;
};

Module g;

// ---------------------------------------------------------------------------------------------
// The tick: gather and push, and nothing else. What a frame CONTAINS lives in capture/gather.h, which
// is a header so W2-B's end-to-end chain test can drive the real gathers instead of mirroring their
// fill masks by hand -- a gather that set them differently would not have been caught.
// ---------------------------------------------------------------------------------------------

// THE STAGE IS SIZED BY THE WIDEST RECORD A PRODUCER PUSHES, and that is one entrant's identity
// block: the header, the entrant, its twenty slot records at the current stride and the entrant's
// whole profile document at the file's own cap. Deriving it from those records (identity_emit.h)
// rather than picking a round number is what keeps a legal profile document from being cut to fit a
// buffer -- the loss ARCH-23 s14.8b exists to stop. Every other kind is far below it.
constexpr uint32_t kPushStageBytes =
    uint32_t(sizeof(RecHeader) + sizeof(EventRec) + kIdentityBlockOneEntrantCap);
static_assert(kIdentityBlockOneEntrantCap == kMaxEventBodyBytes,
              "the stage, the widest block and the stream's event bound are one number");
// RECORDER-5 F2 raised the document cap, and these are the two bounds above it that a constant
// cannot be raised past. Asserted rather than remembered: the first is the format's u16 body length,
// the second is the worker's drain scratch, and a widened document silently breaking either would
// desynchronise S_EVENT or drop whole records at the drain.
static_assert(kMaxEventBodyBytes <= 0xFFFFu,  // facts-ok: NETVAR.SLOT_NONE the u16 range limit, not the engine no-slot field
              "one entrant's identity block must fit S_EVENT's uint16_t body length");
static_assert(kPushStageBytes <= kWorkerScratchBytes,
              "the worker's drain scratch must hold the widest record a producer pushes");
thread_local uint8_t t_stage[kPushStageBytes];

// * The ONE writer of the published recording fact: every phase transition goes
// through here, so a reader of CaptureIsRecording() cannot see Idle while this module is arming.
// Set BEFORE the phase leaves Idle and cleared AFTER it returns, so the window the flag covers is
// never narrower than the window the capture is live in.
void SetPhase(Phase p) {
    if (p != Phase::Idle) PublishCaptureRecording(true);
    g.phase.store(p, std::memory_order_release);
    if (p == Phase::Idle) PublishCaptureRecording(false);
}

bool PushRecord(uint16_t kind, const void* a, uint32_t na, const void* b = nullptr, uint32_t nb = 0) {
    const uint32_t total = static_cast<uint32_t>(sizeof(RecHeader)) + na + nb;
    if (total > sizeof(t_stage)) {
        return false;
    }
    RecHeader h{};
    h.kind = kind;
    h.bytes = static_cast<uint16_t>(na + nb);
    memcpy(t_stage, &h, sizeof h);
    memcpy(t_stage + sizeof(RecHeader), a, na);
    if (nb) {
        memcpy(t_stage + sizeof(RecHeader) + na, b, nb);
    }
    if (!g.rings.Push(t_stage, total)) {
        g.pushFailures.fetch_add(1, std::memory_order_relaxed);
        if (kind <= kRecKindMax) {
            g.pushFailByKind[kind].fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    return true;
}

// The stream a record kind belongs to. Used only for attributing a loss, and it is a switch rather
// than a table so that adding a kind without deciding where its losses are charged does not compile
// into a silent default.
StreamId StreamOfKind(uint16_t kind) {
    // A kind appended to the enum must be given a stream below and must be inside the
    // attribution bound; this stops compiling when one is added without visiting both.
    static_assert(kRecKindMax == kRecPlayerStats, "a new record kind needs a case below");
    switch (kind) {
        case kRecTick: return kStreamTick;
        case kRecWirePlayer: return kStreamPlayerWire;
        case kRecWireBody: return kStreamBodyWire;
        case kRecSampledPlayer: return kStreamPlayerSampled;
        case kRecSampledBody: return kStreamBodySampled;
        case kRecNetvar: return kStreamNetvar;
        case kRecEvent: return kStreamEvent;
        case kRecAsSeen: return kStreamAsSeen;
        case kRecMark: return kStreamMark;
        case kRecOracle: return kStreamOracle;
        case kRecVoip: return kStreamVoip;
        // Its bytes land in the keyframe, so that is where losing one costs something: a dropped
        // world-origin record means the next keyframe may state a stale coordinate frame. Attributing
        // it to S_TICK -- which is what the default did -- would put a real loss on a stream that did
        // not suffer it, and inflate a count the file publishes.
        case kRecWorldOrigin: return kStreamKey;
        // The two kinds the tick samples itself, numbered above kRecWorldOrigin. While the
        // bound stopped there, a ring drop of either was invisible: the producer had no slot
        // to count it in and the worker had no iteration to file it from, so a retention=ALL
        // stream lost a record the file does not mention.
        case kRecGameState: return kStreamGameState;
        case kRecPlayerStats: return kStreamPlayerStats;
        default: return kStreamTick;
    }
}

// S6.3: our Update is the first thing in stage 0, and CClock::HeartbeatBefore ran immediately before
// it. So the component state we are about to read was left by the PREVIOUS frame's simulation, which
// ended at the beat before this one -- simTimeUs minus the delta the engine just handed us. Stamping
// with simTimeUs itself would attribute frame N-1's state to frame N and show up as a constant
// one-frame offset against the recorded /session game_clock.
uint64_t AttributedSimTime(const TickInfo& info) {
    return info.simTimeUs > info.scaledDeltaUs ? info.simTimeUs - info.scaledDeltaUs : 0;
}

// â˜… THE FILE'S TIME AXIS, IN ONE PLACE. The capture tick picks peer time on a client and the sim
// clock on a host (see the tick below); t0 used to be AttributedSimTime unconditionally, so on a
// client the header's origin was on the SIM axis while every record that followed was on the PEER
// axis. RECORDER-5: the 09-19 live capture opened at t0 = 16.334 s and its first tick arrived at
// 3862.614 s, so the header advertised 3951.939 s of duration for 105.659 s of footage, chapter 2
// was a 3846 s hole, and the eight Join marks -- which ride t0 -- sat outside the footage entirely.
// One function, both callers, and the axis can no longer differ between the origin and the records.
uint64_t CaptureAxisTime(const TickInfo& info, uint32_t captureRole) {
    const uint64_t sim = AttributedSimTime(info);
    if (captureRole != 0) {
        return sim;
    }
    uint64_t peerUs = 0;
    uint32_t valid = 0;
    if (g.svc && ReadPeerTime(g.svc->asSeen, peerUs, valid) && (valid & kAsSeenPeerTime) && peerUs) {
        return peerUs;
    }
    return sim;
}

// The sink the gathers push through. capture.cpp owns the ring; gather.h owns what a frame contains.
bool GatherPush(uint16_t kind, const void* head, uint32_t headBytes, const void* tail,
                uint32_t tailBytes, void* /*ctx*/) {
    return PushRecord(kind, head, headBytes, tail, tailBytes);
}

GatherConfig BuildGatherConfig() {
    GatherConfig gc{};
    for (uint16_t i = 0; i < 32; ++i) {
        gc.streamEnabled[i] = g.builder.streamEnabled[i];
    }
    return gc;
}

// The derived gap list. It used to be a comment at the top of this file and the audit found it wrong
// in BOTH directions at once, so it is read from the engine instead and logged once per session.
//
// THE PRECISE STATEMENT, because the imprecise one has been repeated: ReadPlayer DOES source bits28,
// holdL and holdR, so a REMOTE player's action word and grab state travel. What is missing is the
// LOCAL PAWN's own -- the player the recorder is. On a client that is one player in the match; on an
// offline host every player including the bots is a local pawn, so on the owner's own match it is
// everybody. Same gap, very different blast radius, and the role decides which.
void ReportGapsOnce(const GatherReport& rep) {
    if (g.fillReported || !rep.sawLocalPawn) {
        return;
    }
    g.fillReported = true;
    char have[256] = {}, missing[256] = {};
    const uint32_t all = kFillPose | kFillRefVel | kFillBody | kFillTrigger | kFillAnchor |
                         kFillActionWord | kFillHolding;
    for (uint32_t bit = 1; bit <= all; bit <<= 1) {
        if ((all & bit) == 0) continue;
        char* dst = (rep.localFillMask & bit) ? have : missing;
        const size_t used = strlen(dst);
        _snprintf_s(dst + used, sizeof have - used, _TRUNCATE, "%s%s", used ? "|" : "",
                    PlayerFillName(bit));
    }
    if (!g.log) {
        return;
    }
    // â˜… DIAG-L2 F4. WHICH arm refused. A file that carries no player rows has four possible causes
    // and they need different owners: no pawn in the pool (warned separately, and this function does
    // not run then), the pool resolved but ReadLocalPlayer returned an empty mask for every pawn
    // (upstream of capture -- the accessor, not us), some pawns refused, or every pawn was sourced
    // and the gap is per-group. L2 was the second and the log said only that groups were missing.
    const char* arm = "";
    if (rep.pawnsRefused && rep.pawnsRefused >= rep.pawnsPresent) {
        arm = "EVERY present local pawn refused";
    } else if (rep.pawnsRefused) {
        arm = "some present local pawns refused";
    }
    if (arm[0]) {
        // â˜… L2-RUN-2 F2. This line said "ReadLocalPlayer returned an EMPTY mask" and then printed the
        // engine's word for the same event as `ok`, because it had only ONE story for a pawn with no
        // row and the pawn had two possible histories: the accessor declined, or the accessor answered
        // and this build could not store the groups it answered with. The second is a CAPTURE defect
        // and it read as an engine one for a whole match. The mask decides it, so the mask is printed.
        const bool untranslated = rep.pawnsUntranslated != 0;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "S_PLAYER_SAMPLED refusal arm: %s -- %u of %u present pawn(s) produced no row. "
                     "%s The pool resolved, so this is not an empty pool; the file's DEGRADED row says "
                     "the rows are missing and this says which arm lost them. Refusal arm as the engine "
                     "named it: %s. Engine group mask: 0x%02X (%u of the refusals carried a NON-EMPTY "
                     "mask this build cannot store). Actor source: %s, echo component: %s, sources "
                     "disagree: %s. Accessor state: %s",
                     arm, rep.pawnsRefused, rep.pawnsPresent,
                     untranslated
                         ? "THE ACCESSOR ANSWERED and the capture dropped the row: the engine sourced "
                           "at least one group whose bit has no line in LocalMaskToFill, so the "
                           "translated fill was 0. This is ours, not the engine's."
                         : "ReadLocalPlayer returned an EMPTY mask for them, so nothing was sourced to "
                           "write.",
                     replay::LocalRefusalName(
                         static_cast<replay::IEntity::LocalRefusal>(rep.localRefusal)),
                     rep.localRefusedEngineMask, rep.pawnsUntranslated,
                     replay::ActorSourceName(
                         static_cast<replay::IEntity::ActorSource>(rep.localActorSource)),
                     rep.localEchoComponent ? "present" : "ABSENT",
                     rep.localSourcesDisagree ? "yes" : "no",
                     replay::entity::LocalPawnDiagnostic(g.svc->entity));
    }
    // â˜… AUDIT_M9 F2. The arm above can only be entered by a pawn that produced NO row, so a pawn whose
    // mask carried one group with no translation AND one with a translation went past it silently: the
    // row was written, the group is missing from it for every frame of the match, and nothing said so.
    // That is the half that ships a wrong file rather than an empty one.
    if (rep.pawnsGroupLoss != 0) {
        char lost[128] = {};
        for (uint32_t bit = 1; bit != 0; bit <<= 1) {
            if ((rep.lostLocalGroupBits & bit) == 0) continue;
            const size_t used = strlen(lost);
            _snprintf_s(lost + used, sizeof lost - used, _TRUNCATE, "%s%s", used ? "|" : "",
                        replay::LocalGroupName(bit));
        }
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "S_PLAYER_SAMPLED group loss: the engine sourced [%s] (mask 0x%02X) for %u "
                     "present pawn(s) and this build has no line in LocalMaskToFill for it, so those "
                     "groups are NOT in the file. %u of those pawns had a row written anyway, with the "
                     "group missing from it -- the row looks complete and is not. This is ours, not "
                     "the engine's.",
                     lost[0] ? lost : "(none)", rep.lostLocalGroupBits, rep.pawnsGroupLoss,
                     rep.rowsWithGroupLoss);
    }
    // â˜… AUDIT_M9 F4. Two actor sources naming different actors is a fact about the READ. It was
    // reported only from the refusal line above, and the rig that actually disagrees fills all six
    // groups -- a SUCCESSFUL read, which never reaches that line. If it happens in the field the
    // capture records the local pawn's groups from whichever actor the key array named, for the whole
    // match, and this is the only place that says so.
    if (rep.localDisagreements != 0) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "S_PLAYER_SAMPLED actor sources DISAGREE on %u successful read(s): the "
                     "broadcaster key array and CR15NetGame::UserActor named different actors and the "
                     "capture used %s, which is the first source in the fixed order. The local pawn's "
                     "rows in this file are that actor's; the other source's actor is not recorded.",
                     rep.localDisagreements,
                     replay::ActorSourceName(
                         static_cast<replay::IEntity::ActorSource>(rep.localDisagreeSource)));
    }
    const bool host = g.builder.captureRole != 0;
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "S_PLAYER_SAMPLED sources, read from the engine rather than declared: have [%s]; "
                 "NOT sourced [%s]. This is the LOCAL PAWN only -- ReadPlayer sources the action word "
                 "and hold ids for a remote, so those travel for everyone else. %s",
                 have[0] ? have : "(nothing)", missing[0] ? missing : "(nothing)",
                 host ? "role is HOST: every player including bots is a local pawn, so this gap is "
                        "the whole match"
                      : "role is CLIENT: this gap is the recorder only; remotes come off the wire");
}

// ---------------------------------------------------------------------------------------------
// THE ROSTER SOURCE. roster.h says why the composition is not in here; this is the other half --
// the engine reads, one per primitive, each returning false rather than a plausible default.
//
// The pointer everything hangs off is the CR15NetGame the session holds. `ng` is re-asked EVERY
// scan and never cached across ticks: a session teardown frees it, and a cached pointer read after
// that reads whatever now occupies the address.
class EngineRosterSource final : public IRosterSource {
public:
    EngineRosterSource(void* ng, const IIdentity* id, IEntity* ent)
        : ng_(ng), id_(id), ent_(ent) {}

    bool Usable() const { return ng_ != nullptr || id_ != nullptr; }

    bool EntrantBand(uint16_t& band, const char*& why) const override {
        // THE ENGINE'S OWN COUNT (IDENT.ENTRANT_COUNT_OFF, through identity_access::EntrantCount).
        // ENG-W2A-ROSTER walked a horizon of kRosterBandCap because no accessor existed; W1-E's
        // 2a233a0 landed one, and a horizon is no longer honest now that the number can be asked
        // for. A refusal here costs this scan's roster; substituting 64 would put OUR number in the
        // file under the engine's name and keep passing on a build where the count moved.
        if (!ng_) {
            why = "no netgame";
            return false;
        }
        uint32_t count = 0, nonReplay = 0;
        identity_access::Refusal r = identity_access::Refusal::None;
        if (!identity_access::EntrantCount(ng_, count, nonReplay, &r)) {
            why = identity_access::RefusalText(r);
            return false;
        }
        // Saturating rather than wrapping. A count past a u16 is not a roster this table can hold
        // and the clamp in ScanRoster reports the overflow; wrapping would report a small band.
        band = count > UINT16_MAX ? uint16_t(UINT16_MAX) : uint16_t(count);
        return true;
    }

    // Name, team, team slot, user slot and loadout -- every field that moves while a match runs.
    // Taken from the identity tables only: a summary that had to read the profile CJson would cost
    // what it is meant to save.
    bool Fingerprint(uint16_t idx, uint64_t& out) const override {
        uint64_t h = kRosterFoldSeed;
        char name[kRosterNameCap] = {};
        if (!DisplayName(idx, name, sizeof name)) {
            return false;
        }
        h = RosterFold(h, name, sizeof name);

        uint16_t slot = kRosterNoSlot;
        if (!SlotOf(idx, slot)) {
            return false;
        }
        h = RosterFold(h, &slot, sizeof slot);

        uint16_t team = kRosterNoTeam, teamSlot = kRosterNoTeam;
        if (!Team(idx, team, teamSlot)) {
            return false;
        }
        h = RosterFold(h, &team, sizeof team);
        h = RosterFold(h, &teamSlot, sizeof teamSlot);

        // ★ ONE COMPONENT WALK, and this is the hottest read in the capture: Fingerprint runs for
        // every entrant on every scan. AUD-W2-A-2 F5 needs the component's true width folded (the
        // twenty item ids are a CLAMPED view, so a component growing 20 -> 21 moves nothing above
        // and the row is never re-read); AUD-W2-A-2 F9 is what happened when that width came from a
        // SECOND ReadAppliedPairs -- a second BuildMatches + ResolveComponent + CustSlots per
        // entrant per scan, twenty pairs copied and thrown away, inside the bracket that has already
        // closed two captures. One call returns both.
        uint64_t items[20] = {};
        uint64_t held = 0;
        bool heldKnown = false;
        if (LoadoutPairs(idx, nullptr, items, held, heldKnown)) {
            h = RosterFoldLoadout(h, items, held, heldKnown);
        } else if (LoadoutItems(idx, items)) {
            // The item-only path: no component width to fold, which is the same statement the fill
            // bit makes on the row itself.
            h = RosterFoldLoadout(h, items, 0, false);
        } else {
            return false;
        }

        uint16_t number = 0, numberBody = 0;
        const char* why = nullptr;
        if (LoadoutNumbers(slot, number, numberBody, why)) {
            h = RosterFold(h, &number, sizeof number);
            h = RosterFold(h, &numberBody, sizeof numberBody);
        }
        out = h;
        return true;
    }

    bool SlotOf(uint16_t idx, uint16_t& slot) const override {
        UserSlot s(kRosterNoSlot);
        if (ng_ && identity_access::SlotOfIndex(ng_, UserIndex(idx), s)) {
            slot = s.v;
            return slot != kRosterNoSlot;
        }
        if (id_) {
            slot = id_->SlotOf(UserIndex(idx)).v;
            return slot != kRosterNoSlot;
        }
        return false;
    }

    bool DisplayName(uint16_t idx, char* out, size_t cap) const override {
        // ResolvedDisplayName is CR15NetGame::UserDisplayName -- the call the CLIENT prints from --
        // not our own profile write read back. Reading the profile instead is what let "every player
        // renders Unknown" pass as "name ok" (identity_access.h).
        if (id_ && identity_access::ResolvedDisplayName(id_, UserIndex(idx), out, cap)) {
            return out[0] != 0;
        }
        if (ng_ && identity_access::ResolvedDisplayName(ng_, UserIndex(idx), out, cap)) {
            return out[0] != 0;
        }
        return false;
    }

    bool PlatformString(uint16_t idx, char* out, size_t cap) const override {
        return ReadProfile(idx, "platform", out, cap);
    }

    bool XPlatformId(uint16_t idx, uint64_t& out) const override {
        // RECORDER-3 F3. The id64 is in the lobby member record's head, put there by the same
        // handler that parses the profile document -- so it is readable for a remote entrant whose
        // profile service reply has not arrived. No walk, no string: the engine already parsed it.
        uint16_t slot = 0;
        if (ng_ && SlotOf(idx, slot) &&
            identity_access::LobbyEntrantAccountId(ng_, UserSlot(slot), out) && out != 0) {
            return true;
        }
        char buf[64] = {};
        if (!ReadProfile(idx, "xplatformid", buf, sizeof buf)) {
            return false;
        }
        return ParseXPlatformId(buf, out);
    }

    bool Team(uint16_t idx, uint16_t& team, uint16_t& teamSlot) const override {
        // TRUE means THE READ HAPPENED -- which is all kRosterFillTeam ever claimed. Its answer may
        // be kRosterNoTeam, and that is a recorded fact about the entrant rather than a refusal:
        // returning false here would clear the fill bit and make an entrant who has joined nothing
        // indistinguishable from one the engine would not answer for.
        if (!ng_) return false;
        RosterTeamFromEngine(identity_access::TeamOf(ng_, UserIndex(idx)),
                             identity_access::TeamSlotOf(ng_, UserIndex(idx)), team, teamSlot);
        return true;
    }

    bool Alignment(uint16_t idx, uint16_t& out) const override {
        // THE COLOUR, and it is per team rather than per player (identity_access.h: CR15NetGame::
        // Alignment takes a team id). A player with no team therefore has no alignment, and that is
        // absence rather than alignment 0.
        if (!ng_) return false;
        uint16_t team = 0, teamSlot = 0;
        if (!Team(idx, team, teamSlot) || team == kRosterNoTeam) return false;
        out = identity_access::TeamAlignment(ng_, static_cast<uint8_t>(team));
        return true;
    }

    bool LoadoutItems(uint16_t idx, uint64_t items[20]) const override {
        if (!id_) return false;
        Symbol64 applied[20] = {};
        if (!id_->ReadAppliedItems(UserIndex(idx), applied)) return false;
        memcpy(items, applied, sizeof applied);
        return true;
    }

    // The pair form, and the one the recording is built from: the engine's customisation entry
    // carries its own key at +0x00 and ReadAppliedItems threw it away, which left POSITION as the
    // only slot evidence on disk (ARCH-23 s14.8b C1).
    //
    // ★ KEYS, ITEMS AND THE COMPONENT'S TRUE WIDTH, OUT OF ONE WALK. `held` is the UNCLAMPED entry
    // count -- the only thing that can tell a full loadout from a wider component cut down to twenty,
    // since the pairs themselves are min(count, 20) -- and it comes back from the same
    // ReadAppliedPairs resolution. It had its own reader for one commit (AUD-W2-A-2 F5's fix), and
    // AUD-W2-A-2 F9 measured what that cost: BuildMatches + ResolveComponent + CustSlots + twenty
    // pair copies, a second time, for every entrant on every scan, to keep one integer.
    // `keys` may be null for a caller that only needs the items (the fingerprint).
    bool LoadoutPairs(uint16_t idx, uint64_t keys[20], uint64_t items[20], uint64_t& held,
                      bool& heldKnown) const override {
        heldKnown = false;
        if (!ng_) return false;
        identity_access::LoadoutPair pairs[20] = {};
        uint64_t n = 0, count = 0;
        if (!identity_access::ReadAppliedPairs(ng_, UserIndex(idx), pairs, n, &count)) return false;
        for (uint16_t i = 0; i < 20; ++i) {
            if (keys) keys[i] = pairs[i].slotKey;
            items[i] = pairs[i].itemId;
        }
        held = count;
        heldKnown = true;
        return true;
    }

    bool LoadoutNumbers(uint16_t slot, uint16_t& number, uint16_t& numberBody,
                        const char*& why) const override {
        // ENG-W2A-ROSTER S1.3 wrote these as 0 -- "no table this build can read holds them". W1-E's
        // 2a233a0 disproved that: they are the u16 pair at +0x38/+0x3A of the live loadout header,
        // span-checked before either is touched. Keyed on the SLOT, which is what indexes the table.
        if (!ng_) {
            why = "no netgame";
            return false;
        }
        identity_access::Refusal r = identity_access::Refusal::None;
        if (!identity_access::LoadoutNumbers(ng_, UserSlot(slot), number, numberBody, &r)) {
            why = identity_access::RefusalText(r);
            return false;
        }
        return true;
    }

    bool Generation(uint16_t slot, uint16_t& out) const override {
        if (!ng_) return false;
        uint32_t netUserId = 0;
        if (!identity_access::NetUserId(ng_, UserSlot(slot), netUserId)) return false;
        out = static_cast<uint16_t>(netUserId >> 16);  // slot low word, generation high
        return true;
    }

    bool HasProfileJson(uint16_t idx) const override {
        return ng_ && identity_access::ProfileJsonUsable(ng_, UserIndex(idx));
    }

    bool IsLocal(uint16_t idx) const override {
        if (!ent_) return false;
        const size_t n = ent_->LocalPlayerCount();
        for (size_t i = 0; i < n; ++i) {
            if (ent_->LocalPlayerAt(i).v == idx) return true;
        }
        return false;
    }

    // The entrant's whole ServerProfile document, serialised by src/engine/entity_json.cpp out of the
    // engine's own CJson readers. `cap` is kRosterJsonCap, the file's own bound, so a document that
    // would not fit the file is refused HERE with its true length rather than cut -- see roster.h's
    // note on the one meaning of the truncation bit.
    bool EntrantJson(uint16_t idx, void* out, size_t cap, size_t& written, bool& truncated,
                     const char*& why, identity_access::Refusal& refusal,
                     EntrantJsonOutcome* outcome = nullptr) const override {
        written = 0;
        truncated = false;
        if (outcome) *outcome = EntrantJsonOutcome{};
        if (!ng_) {
            why = "no netgame";
            refusal = identity_access::Refusal::NoNetGame;
            return false;
        }
        refusal = identity_access::Refusal::None;
        bool lossy = false;
        if (identity_access::ProfileJsonBytes(ng_, UserIndex(idx), out, cap, written, truncated,
                                              &refusal, &lossy)) {
            if (outcome) *outcome = {ProfileSource::UserTable, lossy};
            return true;
        }
        // RECORDER-3 F1. EmptyProfile ONLY: every other word names something the second storage
        // cannot fix (no netgame, a buffer too small, a shape this build's read API cannot walk),
        // and retrying those would be a second walk that refuses for the same reason. On a live
        // server EmptyProfile is the common case -- the profile service fills the user table
        // asynchronously -- and the document the lobby message carried is already in the record.
        if (refusal != identity_access::Refusal::EmptyProfile) {
            why = identity_access::RefusalText(refusal);
            return false;
        }
        uint16_t slot = 0;
        if (!SlotOf(idx, slot)) {
            why = identity_access::RefusalText(refusal);
            return false;
        }
        identity_access::Refusal fallback = identity_access::Refusal::None;
        written = 0;
        truncated = false;
        lossy = false;
        if (identity_access::ProfileJsonBytesFromEntrantRecord(ng_, UserSlot(slot), out, cap,
                                                               written, truncated, &fallback,
                                                               &lossy)) {
            if (outcome) *outcome = {ProfileSource::EntrantRecord, lossy};
            return true;
        }
        // The FALLBACK's word is reported, not the user table's: both storages were asked, and
        // "the lobby message carried no document" is a different fact from "the profile service
        // has not replied yet". Both are Transient, so the retry policy is unchanged.
        refusal = fallback;
        why = identity_access::RefusalText(fallback);
        return false;
    }

    // The document POINTER the entrant carries. The engine's profile-received handler stores a new
    // document there rather than rewriting the old one, so a new profile moves it; one read, no
    // engine call, the same unguarded table read HasProfileJson already makes for a scanned index.
    uint64_t ProfileDocToken(uint16_t idx) const override {
        if (!ng_) return 0;
        const void* doc = identity_access::ProfileJson(ng_, UserIndex(idx));
        return uint64_t(reinterpret_cast<uintptr_t>(doc));
    }

    // ENG-W1E-JSONWALK S3 established from CR15NetGame's own unlocks walk (0x1401552D0) that
    // `unlocks` is a CHILD NODE of the entrant profile, so its bytes are inside EntrantJson's
    // document and copying that document in here again would double the blob and misname what it
    // is. This field was therefore refused by name until W1-E's `7e99462` shipped the subtree
    // serialiser -- ProfileJsonSubtree, which IS ProfileJsonBytes with a path, so the two cannot
    // drift apart. The refusal is now the serialiser's own (JsonNoSuchKey when the entrant carries
    // no unlocks node), which is a fact about the profile rather than about this build.
    bool UnlocksJson(uint16_t idx, void* out, size_t cap, size_t& written, bool& truncated,
                     const char*& why) const override {
        written = 0;
        truncated = false;
        if (!ng_) {
            why = "no netgame";
            return false;
        }
        identity_access::Refusal r = identity_access::Refusal::None;
        if (!identity_access::ProfileJsonSubtree(ng_, UserIndex(idx), "unlocks", out, cap, written,
                                                 truncated, &r)) {
            why = identity_access::RefusalText(r);
            return false;
        }
        return true;
    }

private:
    bool ReadProfile(uint16_t idx, const char* key, char* out, size_t cap) const {
        if (id_ && identity_access::ReadProfileString(id_, UserIndex(idx), key, out, cap)) {
            return out[0] != 0;
        }
        if (ng_ && identity_access::ReadProfileString(ng_, UserIndex(idx), key, out, cap)) {
            return out[0] != 0;
        }
        return false;
    }

    void* ng_;
    const IIdentity* id_;
    IEntity* ent_;
};

// ---------------------------------------------------------------------------------------------
// THE IDENTITY BLOCK PRODUCER. identity_emit.h owns WHEN a block is written and WHAT is in it; these
// three are the ring, the log and the tap-table index, which are this file's.
//
// ENG-W2-C-2 s6 measured the gap this closes: the format, the writer and the reader all existed and
// NOTHING put a block into S_EVENT, so no recording carried a per-entrant loadout at all.
// ---------------------------------------------------------------------------------------------

bool PushIdentityBlock(const void* block, uint32_t bytes, uint64_t /*tUs*/, void* ctx) {
    EventRec r{};
    // The tap's ARRIVAL axis, exactly like every other S_EVENT row: chapterbuilder's
    // StampIsOnSourceAxis declares kRecEvent's stamp foreign to the source axis, and the block's own
    // header already carries the capture-axis time it was gathered at. Two axes, two fields, and
    // filing a source-axis time here is the defect L3-RUN-6 F1 filed under a different name.
    r.stampUs = WiretapArrivalNowUs();
    r.dst = 0;
    r.typeIndex = *static_cast<const uint16_t*>(ctx);
    r.bodyBytes = static_cast<uint16_t>(bytes);
    return PushRecord(kRecEvent, &r, sizeof r, block, bytes);
}

void LogIdentityCensus(const IdentityEmitCensus& line, void* /*ctx*/) {
    if (!g.log) return;
    // Three states for the document, never a bare byte count: 0 B present and 0 B refused are the
    // same number and different facts.
    char profile[96];
    if (!line.profileAsked) {
        _snprintf_s(profile, sizeof(profile), _TRUNCATE, "NOT ASKED");
    } else if (line.profileRefused) {
        _snprintf_s(profile, sizeof(profile), _TRUNCATE, "REFUSED (%s)",
                    line.profileWhy ? line.profileWhy : "unnamed");
    } else {
        _snprintf_s(profile, sizeof(profile), _TRUNCATE, "%u B", unsigned(line.jsonBytes));
    }
    // `pairs` is the entrant's RECORDED denominator, not a count of the array: the three states
    // survive into the line, because "UNKNOWN" and "0" are a scan that could not ask and a component
    // that held nothing, and a reader who sees 0 for both cannot tell which capture to repair.
    char pairs[16];
    if (line.filledPairCount == kIdentityPairCountUnknown) {
        _snprintf_s(pairs, sizeof(pairs), _TRUNCATE, "UNKNOWN");
    } else {
        _snprintf_s(pairs, sizeof(pairs), _TRUNCATE, "%u", unsigned(line.filledPairCount));
    }
    // The name is masked by the emitter, not here: a census that relies on every call site
    // remembering to mask publishes a real one the first time somebody forgets.
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "identity block v%u slot %u: name %s pairs %s/%u, keys %u, profile %s, block %u B, "
                 "component %s",
                 unsigned(line.version), unsigned(line.userSlot), line.nameMasked,
                 pairs, unsigned(line.slotsWritten), unsigned(line.slotsWithKey), profile,
                 unsigned(line.blockBytes),
                 !line.truncationKnown
                     ? "width UNKNOWN -- the recording could not ask"
                     : (line.truncated ? "WIDER than the format records" : "fully recorded"));
}

// Once per entrant per refusal word (the emitter dedups), which is what replaced a WARN that fired
// every second for the whole of a match on one entrant's unserialisable profile.
void LogProfileRefusal(const IdentityProfileRefusalNote& note, void* /*ctx*/) {
    if (!g.log) return;
    const char* policy = "";
    switch (note.kind) {
        case ProfileRefusalKind::Deterministic:
            policy = "this document can never be serialised, so it is NOT asked again unless the "
                     "entrant's document is replaced or the row itself changes";
            break;
        case ProfileRefusalKind::Transient:
            policy = "re-asked with back-off, 1 s doubling to 60 s, and at most one such re-ask "
                     "per 20 s across the whole roster";
            break;
        case ProfileRefusalKind::NoSource:
            policy = "re-asked when an identity source appears";
            break;
    }
    // The document's own length when the refusal reported one. Without it a ShortBuffer refusal
    // says the buffer was too small and nothing says by how much, which is what made the 09-19
    // capture's eight lost profiles unsizeable from the log alone.
    char needed[64];
    needed[0] = 0;
    if (note.docBytes) {
        _snprintf_s(needed, sizeof(needed), _TRUNCATE, " [document %u B, buffer %u B]",
                    unsigned(note.docBytes), unsigned(kRosterJsonCap));
    }
    g.log->Write(LogLevel::Warn, kReplayModuleName,
                 "identity profile slot %u (%s): refused (%s)%s -- %s: %s. Its block is marked "
                 "ProfileRefused -- a reader must not read it as an entrant with no profile",
                 unsigned(note.userSlot), note.nameMasked, note.why ? note.why : "unnamed", needed,
                 ProfileRefusalKindText(note.kind), policy);
}

// Runs after a FULL roster scan and only then: the rows are what the engine last said, and the
// emitter decides which of them the file does not yet carry.
void EmitIdentityBlocks(const IRosterSource* src, uint64_t tUs, uint16_t profileBudget) {
    if (!g.builder.streamEnabled[kStreamEvent]) {
        ++g.identityRefusals[size_t(IdentityEmitRefusal::StreamDisabled)];
        return;
    }
    if (g.identityTypeIndex == kTapTypeIndexNone) {
        g.identityTypeIndex = TapTypeIndex(kIdentitySnapshotTapName);
    }
    if (g.identityTypeIndex == kTapTypeIndexNone) {
        // No row, no index, no record. Publishing under some other row's index would file the block
        // as a message body of a type it is not, which is worse than not writing it.
        ++g.identityRefusals[size_t(IdentityEmitRefusal::NoTapType)];
        if (!g.identityTypeWarned && g.log) {
            g.identityTypeWarned = true;
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "identity block: the tap table has no %s row, so this capture carries NO "
                         "identity blocks. A reader must not read that as an empty lobby",
                         kIdentitySnapshotTapName);
        }
        return;
    }

    // Tick-thread only, and sized from the format rather than chosen: one entrant's widest block and
    // the file's own document cap. Static rather than automatic because the tick thread's stack is
    // not where four kilobytes of profile belongs.
    static uint8_t blockBuf[kIdentityBlockOneEntrantCap];
    static uint8_t jsonBuf[kRosterJsonCap];
    IdentityEmitScratch scratch;
    scratch.block = blockBuf;
    scratch.blockCap = sizeof blockBuf;
    scratch.json = jsonBuf;
    scratch.jsonCap = sizeof jsonBuf;

    IdentityEmitReport rep;
    g.identityEmit.SetProfileRefusalSink(&LogProfileRefusal, nullptr);
    // contentHash stays 0 for the same reason the manifest's does (SetBuild, below): nothing in this
    // plugin computes a catalogue hash yet, and a made-up value would read as a recorded fact.
    g.identityEmit.EmitChanged(g.roster, src, tUs, 0, scratch, profileBudget, &PushIdentityBlock,
                               &g.identityTypeIndex, &LogIdentityCensus, nullptr, rep);
    g.identityBlocks += rep.emitted;
    g.identityJsonRefused += rep.jsonRefused;
    g.identityKeyless += rep.keyless;
    g.identityTruncated += rep.truncated;
    g.identityTruncationUnknown += rep.truncationUnknown;
    g.identityDeferred += rep.deferred;
    if (rep.lastJsonWhy) g.identityLastJsonWhy = rep.lastJsonWhy;
    // EVERY refusal gets a line the first time it fires, naming itself: a silent
    // census had five possible causes and the log distinguished one of them.
    for (size_t i = 0; i < kIdentityEmitRefusalCount; ++i) {
        g.identityRefusals[i] += rep.refusals[i];
        if (rep.refusals[i] && !g.identityRefusalWarned[i] && g.log) {
            g.identityRefusalWarned[i] = true;
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "identity block: %s -- %u block(s) not written for this reason. A capture "
                         "with no census line is NOT evidence that the producer never ran",
                         IdentityEmitRefusalText(static_cast<IdentityEmitRefusal>(i)),
                         unsigned(rep.refusals[i]));
        }
    }
    if (rep.truncated && g.log) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "identity block: %u entrant(s) hold MORE customisation entries than this "
                     "format records, so their blocks are marked truncated. The twenty pairs in "
                     "them are true; they are not all of them",
                     unsigned(rep.truncated));
    }
    if (rep.keyless && !g.identityKeylessWarned && g.log) {
        g.identityKeylessWarned = true;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "identity block: %u entrant(s) recorded with NO slot key -- the engine's "
                     "customisation entries could not be paired (%s). Those blocks carry items with "
                     "no slot evidence and a dresser must refuse them rather than guess by position",
                     unsigned(rep.keyless),
                     g.rosterRefusals.wordByReason[size_t(RosterRefusal::LoadoutKeys)]
                         ? g.rosterRefusals.wordByReason[size_t(RosterRefusal::LoadoutKeys)]
                         : "unnamed");
    }
}

// The armed tick pays for at most ONE profile document per pass. The rest of the
// rows that moved are deferred to the next scan, which costs them one cadence and keeps a round
// transition -- which moves every entrant at once -- from putting N CJson walks on one frame.
constexpr uint16_t kIdentityArmedTickProfileBudget = 1;

// One scan from whatever the services resolve THIS tick. `gateOnSignal` puts the scan behind the
// entrant-count gate (roster.h) -- which is every caller on the tick path; the open-time caller
// passes false, because a file's manifest may not be built out of a cadence.
RosterScanReport ScanRosterNow(uint64_t tUs, bool gateOnSignal) {
    void* ng = (g.svc && g.svc->session) ? g.svc->session->NetGame() : nullptr;
    const IIdentity* id = g.svc ? g.svc->identity : nullptr;
    if (!ng && id) {
        ng = identity_access::NetGameOf(id);
    }
    EngineRosterSource src(ng, id, g.svc ? g.svc->entity : nullptr);
    if (!src.Usable()) {
        RosterScanReport rep = {};
        rep.sourceAbsent = 1;
        return rep;
    }
    RosterScanReport rep = {};
    if (!ScanRosterOnChange(&src, g.roster, g.rosterCadence, tUs, kRosterRescanIntervalUs,
                            !gateOnSignal, rep)) {
        g.rosterSignalUnchanged += rep.signalUnchanged;
        return rep;
    }
    for (size_t i = 0; i < kRosterRefusalCount; ++i) {
        g.rosterRefusals.byReason[i] += rep.refusals.byReason[i];
        if (rep.refusals.wordByReason[i]) {
            g.rosterRefusals.wordByReason[i] = rep.refusals.wordByReason[i];
        }
    }
    g.rosterScansUnchanged += rep.unchanged;
    g.rosterBandRefusals += rep.bandRefused;
    g.rosterBandClamped += rep.bandClamped;
    // ON CHANGE, NOT PER TICK: this runs only where a FULL scan ran -- the entrant count moved or the
    // cadence expired -- and the emitter pushes only the rows whose block content moved with it. A
    // loadout swapped mid-match keeps the entrant count, so it arrives on the cadence: within
    // kRosterRescanIntervalUs, which is a bound rather than a hope.
    return rep;
}

uint16_t LocalRosterRows() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < g.roster.Count(); ++i) {
        if (g.roster.At(i).local) ++n;
    }
    return n;
}

// THE ROSTER, FROM THE ENGINE, AND OFF THE GATHER'S BRACKET. Not from a join event: a solo
// `-offline` host receives no lobby traffic at all, so the wire-derived roster was empty on
// exactly the launch the owner records most (ARCH-21, and L2-RUN-3 F3's `players : 0`). What
// it is NOT any more is a full scan per tick -- see roster.h's gate for what replaced it, and
// L2-RUN-5 F1 for the capture that cost.
// True when a FULL scan ran, which is the only state in which the identity emitter has anything new
// to look at. Returned rather than inferred, so the emitter's bracket is not entered on a quiet tick.
bool ScanRosterTick(uint64_t sourceUs) {
    bool scanRan = false;
    {
        const RosterScanReport rr = ScanRosterNow(sourceUs, true);
        scanRan = !rr.signalUnchanged && !rr.sourceAbsent;
        g.rosterAppended += rr.appended;
        g.rosterTeamChanges += rr.teamChanged;
        g.rosterDepartures += rr.departed;
        if ((rr.appended || rr.departed || rr.teamChanged) && g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "roster: %u row(s) after +%u join(s), %u departure(s), %u team change(s) "
                         "at %llu us (entrants %u, %u present, %u unchanged)",
                         unsigned(g.roster.Count()), unsigned(rr.appended), unsigned(rr.departed),
                         unsigned(rr.teamChanged), static_cast<unsigned long long>(sourceUs),
                         unsigned(rr.bandWalked), unsigned(rr.present), unsigned(rr.unchanged));
        }
        if (rr.bandRefused && !g.rosterBandWarned) {
            g.rosterBandWarned = true;
            if (g.log) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "roster: the engine's entrant count could not be read (%s), so this scan "
                             "walked NOTHING. A horizon of our own would put our number in the file "
                             "under the engine's name",
                             rr.bandWhy ? rr.bandWhy : "unnamed");
            }
        }
        if (rr.bandClamped && g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "roster: the engine counted %u entrant(s) past this table's %u rows, so that "
                         "many are NOT in this capture",
                         unsigned(rr.bandClamped), unsigned(kRosterMaxRows));
        }
        if (rr.sourceAbsent && !g.rosterSourceWarned) {
            g.rosterSourceWarned = true;
            if (g.log) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "roster: no identity source resolved, so this capture will name nobody. "
                             "That is an ABSENT roster and not an empty lobby -- a reader must not "
                             "read ROSTER 0 as 'the match had no players'");
            }
        }
    }
    return scanRan;
}

// The identity blocks, in a bracket of their OWN: the profile serialisation and the
// block build are measured apart from the scan that decides which rows moved, so a regression in
// either names itself instead of inflating the other's number. `armed` is the game tick, where the
// per-call document budget applies; the open-time caller runs on the worker thread and takes the
// whole table, which is what OpenFile's own profile fetch already does.
void EmitIdentityTick(uint64_t tUs, bool armed) {
    void* ng = (g.svc && g.svc->session) ? g.svc->session->NetGame() : nullptr;
    const IIdentity* id = g.svc ? g.svc->identity : nullptr;
    if (!ng && id) {
        ng = identity_access::NetGameOf(id);
    }
    EngineRosterSource src(ng, id, g.svc ? g.svc->entity : nullptr);
    EmitIdentityBlocks(src.Usable() ? &src : nullptr, tUs,
                       armed ? kIdentityArmedTickProfileBudget : kRosterMaxRows);
}

// `split` comes back holding the raw TSC each half cost, so the caller attributes the bracket it
// already keeps without walking anything twice. The module's own clock -- the one the cost meter
// calibrates -- because a second time source here would report in a unit the kill line does not use.
void GatherEntities(const GatherConfig& gc, uint64_t sourceUs, uint32_t frameIndex,
                    uint64_t split[kGatherPartCount]) {
    GatherReport rep;
    const uint64_t tBodies0 = g.cost.Start();
    GatherBodies(g.svc->entity, gc, sourceUs, frameIndex, &GatherPush, nullptr, rep);
    const uint64_t tPawns0 = g.cost.Start();
    GatherLocalPawns(g.svc->entity, gc, sourceUs, frameIndex, &GatherPush, nullptr, rep);
    const uint64_t tEnd = g.cost.Start();
    split[kGatherBodies] = tPawns0 - tBodies0;
    split[kGatherPawns]  = tEnd - tPawns0;

    ReportGapsOnce(rep);

    g.bodiesSampledTotal += rep.bodiesSampled;
    g.bodiesRefusedTotal += rep.bodiesRefused;
    g.bodiesFromLocalTotal += rep.bodiesFromLocal;
    g.bodiesFromHistoryTotal += rep.bodiesFromHistory;
    g.bodiesSourceNoneTotal += rep.bodiesSourceNone;
    g.bodiesSourceNotAskedTotal += rep.bodiesSourceNotAsked;
    for (unsigned i = 0; i < kAngVelSourceCount; ++i) {
        g.angVelByTotal[i] += rep.angVelBy[i];
    }
    g.angVelUnknownTotal += rep.angVelUnknown;
    for (unsigned i = 0; i < kBodyRefusalCount; ++i) {
        g.bodyRefusalTotals[i] += rep.bodyRefusals[i];
    }
    g.bodyRefusalUnknownTotal += rep.bodyRefusalUnknown;
    if (rep.firstRefusalSeen && !g.bodyFirstRefusalSeen) {
        g.bodyFirstRefusalSeen = true;
        g.bodyFirstRefusal = rep.firstRefusal;
    }

    // A CAPTURE WITH NO BODIES HAS NO POSITIONS AND NO PHYSICS, and until AUDIT_FINAL_E2E B1 it said
    // so nowhere: no log line, no record, no counter -- three captures across three builds carried
    // zero S_BODY_SAMPLED and every one of them closed reporting `ok`. The arm is named rather than
    // summarised, and the same fact goes into the FILE as a DEGRADED row so a reader who never sees
    // this log still cannot read the absence as an empty match.
    if (const char* arm = BodyGapArm(rep); arm && !g.bodyGapWarned) {
        g.bodyGapWarned = true;
        if (g.log) {
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "NO BODY SAMPLES: %s (pool %u, walked %u, locally controlled %u, refused "
                         "%u, over cap %u). BodyState.com is the only world position in the format, "
                         "so this capture cannot be played back as motion",
                         arm, unsigned(rep.bodyPoolCount), unsigned(rep.bodiesPresent),
                         unsigned(rep.bodiesLocallyControlled), unsigned(rep.bodiesRefused),
                         unsigned(rep.bodiesBeyondCap));
        }
        DegradedRec d{};
        d.tUs = sourceUs;
        d.what = kStreamBodySampled;
        d.reason = kDegradeCompletenessUnverified;
        d.detail = rep.bodiesPresent;
        PushRecord(kRecDegraded, &d, sizeof d);
    }

    if (rep.sawBodyWithoutNetId && !g.bodyNetIdWarned) {
        g.bodyNetIdWarned = true;
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "a sampled body has no net-actor entry, so its row carries no resolvable "
                         "identity. Purely local bodies are expected here; a networked one would mean "
                         "the net-actor table did not resolve");
        }
    }
    if (!rep.sawLocalPawn && !g.localPawnWarned) {
        g.localPawnWarned = true;
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "no local pawn: the broadcaster pool is empty (%s). S_PLAYER_SAMPLED will "
                         "carry nothing and the file records a DEGRADED row",
                         replay::entity::LocalPawnDiagnostic(g.svc->entity));
        }
        DegradedRec d{};
        d.tUs = sourceUs;
        d.what = kStreamPlayerSampled;
        d.reason = kDegradeCompletenessUnverified;
        d.detail = 1;
        PushRecord(kRecDegraded, &d, sizeof d);
    }
}

void GatherAsSeenFrame(const GatherConfig& gc, uint64_t sourceUs, uint32_t frameIndex,
                       const AsSeenSample* pre) {
    GatherReport rep;
    GatherAsSeen(g.svc->asSeen, g.svc->entity, gc, sourceUs, frameIndex, &GatherPush, nullptr, rep,
                 pre);
}

// ARCH-21 S2 / D2. The LOCAL game state, sampled on the capture tick after the gather, and pushed
// only when something a marker is derived from actually MOVED plus a periodic copy so a seek into
// the middle of a file finds a whole state without decoding back to the first change.
//
// gameTimer and countdownTimer are deliberately NOT in the change set: they move every tick, so
// including them would make the stream a 120 Hz copy of itself. They ride on whichever record an
// edge writes, which is exactly what B6's countdown conjunction needs -- the launch edge writes the
// record and the countdown at that instant is in it.
struct GameStateTap {
    gamestate::IGameState* state;
    bool tried;
    bool saidAvailability;
    bool haveShadow;
    uint64_t lastWriteUs;
    gamestate::GameStateSample shadow;
};
GameStateTap gGameState = {};

bool GameStateChanged(const gamestate::GameStateSample& a, const gamestate::GameStateSample& b) {
    return a.availability != b.availability || a.present != b.present || a.phase != b.phase ||
           a.phaseSymbol != b.phaseSymbol || a.pausedSymbol != b.pausedSymbol ||
           a.roundCounter != b.roundCounter || a.bluePoints != b.bluePoints ||
           a.redPoints != b.redPoints || a.blueMatchScore != b.blueMatchScore ||
           a.redMatchScore != b.redMatchScore || a.frisbeeOwnerId != b.frisbeeOwnerId ||
           a.firstLaunchOfRound != b.firstLaunchOfRound ||
           a.roundWinningTeam != b.roundWinningTeam || a.lastScoreTeam != b.lastScoreTeam ||
           a.lastScorePoints != b.lastScorePoints;
}

// ONE LINE, ONCE, SAYING WHETHER THE GAME STATE IS BEING SAMPLED AT ALL (L2-RUN-5 F3). The field
// could not tell "the sampler never ran" from "the sampler ran and the engine refused every field":
// S_GAMESTATE was absent from the stream table, the phase read 0.0/0 in all 17 cost windows, and not
// one line of a 465-line log mentioned the sampler. A stream that produces nothing has to say so
// itself, and it has to say WHICH of the three reasons it is.
void DiscloseGameState() {
    if (!g.log) {
        return;
    }
    if (!g.builder.streamEnabled[kStreamGameState]) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "gamestate: S_GAMESTATE is DISABLED (capture.stream.gamestate), so this capture "
                     "carries no phase, no round counter and no game timer, and the marker deriver "
                     "has no local candidate. That is a configuration fact, not a quiet match");
        return;
    }
    if (!gGameState.state) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "gamestate: the sampler did not bind on this build -- LiveEngineAccess resolved "
                     "no access object -- so S_GAMESTATE will hold nothing for the whole launch");
        return;
    }
    size_t unreadable = 0;
    const gamestate::UnreadableField* fields = gamestate::UnreadableFields(&unreadable);
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "gamestate: sampling S_GAMESTATE on the capture tick, on change plus a keyframe "
                 "copy every %u us. %u field(s) cannot be read on this build by construction",
                 unsigned(g.builder.keyframeIntervalUs ? g.builder.keyframeIntervalUs : 2000000u),
                 unsigned(unreadable));
    for (size_t i = 0; i < unreadable; ++i) {
        g.log->Write(LogLevel::Info, kReplayModuleName, "gamestate: %s is unreadable -- %s",
                     fields[i].field, fields[i].reason);
    }
}

void SampleGameState(uint64_t sourceUs, uint64_t engineUs) {
    if (!gGameState.state || !g.builder.streamEnabled[kStreamGameState]) {
        return;
    }
    GameStateRec rec = {};
    rec.sourceTimeUs = sourceUs;
    rec.sample.availability =
        static_cast<uint8_t>(gGameState.state->Sample(rec.sample));
    // W2-A owns both clocks on this record: the engine's own time for the read, and the file's
    // axis for where it lands. The sampler stamps neither -- it does not know what the file's axis
    // is, and a sampler that guessed would put the two sources of a marker on different clocks.
    rec.sample.engineTimeUs = engineUs;
    rec.sample.captureTimeUs = sourceUs;
    // The sampler's own verdict, said once. An availability that is never Ok is the interesting
    // case and it is the one that used to be invisible: the records are still written, so the file
    // carries the refusal too, but a reader of the LOG must not have to infer it from a stream's
    // size.
    if (!gGameState.saidAvailability) {
        gGameState.saidAvailability = true;
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "gamestate: first sample availability=%s, present mask 0x%X. Every "
                         "S_GAMESTATE record carries its own availability, so a refusing engine is "
                         "recorded rather than skipped",
                         gamestate::AvailabilityName(
                             static_cast<gamestate::Availability>(rec.sample.availability)),
                         unsigned(rec.sample.present));
        }
    }

    const uint32_t interval =
        g.builder.keyframeIntervalUs ? g.builder.keyframeIntervalUs : 2000000u;
    const bool periodic =
        !gGameState.haveShadow || sourceUs < gGameState.lastWriteUs ||
        sourceUs - gGameState.lastWriteUs >= interval;
    if (!periodic && !GameStateChanged(rec.sample, gGameState.shadow)) {
        return;
    }
    gGameState.shadow = rec.sample;
    gGameState.haveShadow = true;
    gGameState.lastWriteUs = sourceUs;
    PushRecord(kRecGameState, &rec, sizeof rec);
}

// THE LIVE SCOREBOARD, ARCH-21 MAJOR-B2. W1-B shipped the format and W1-E the read; this is the
// only place that joins them, so before it every real capture carried `PLAYERSTATS 0 record(s)`.
//
// The engine read is behind IPlayerStatsSource so the composition -- the gate, the change test, the
// keyframe copy -- is exercised by tests/capture with no engine in the process. Nothing here sets a
// fill bit or substitutes a value: the mask is exactly what ReadPlayerStats answered, and the disk
// sentinels for the bits it left clear are the format's, applied once, in the worker.
class EnginePlayerStatsSource : public IPlayerStatsSource {
public:
    explicit EnginePlayerStatsSource(void* ng) : ng_(ng) {}
    bool Usable() const override { return ng_ != nullptr; }
    bool Read(uint16_t userIndex, identity_access::PlayerStats& out, uint16_t& fill,
              uint8_t& reason, const char*& why) const override {
        fill = 0;
        identity_access::Refusal r = identity_access::Refusal::None;
        if (identity_access::ReadPlayerStats(ng_, UserIndex(userIndex), out, &fill,
                                             &r)) {
            return true;
        }
        reason = static_cast<uint8_t>(r);
        why = identity_access::RefusalText(r);
        return false;
    }

private:
    void* ng_;
};

void EmitPlayerStats(void* /*ctx*/, const PlayerStatsRec& rec) {
    PushRecord(kRecPlayerStats, &rec, sizeof rec);
}

void SamplePlayerStats(uint64_t sourceUs, uint64_t engineUs, bool force) {
    if (!g.builder.streamEnabled[kStreamPlayerStats]) {
        return;
    }
    void* ng = (g.svc && g.svc->session) ? g.svc->session->NetGame() : nullptr;
    if (!ng && g.svc && g.svc->identity) {
        ng = identity_access::NetGameOf(g.svc->identity);
    }
    EnginePlayerStatsSource src(ng);

    // The signal costs NOTHING: every field is one this tick has already read. gGameState.shadow is
    // the last sample the game-state phase took -- equal to this tick's sample whenever one could be
    // taken at all, because that phase updates the shadow on every write and returns early only when
    // the sample did not move. On a build where the state cannot be sampled the shadow never moves,
    // the signal is constant, and the cadence arm is the only one that fires. That is the honest
    // degradation: a slower scoreboard, never a fabricated one.
    const gamestate::GameStateSample& gs = gGameState.shadow;
    const uint64_t signal =
        StatsSignalOf(gs.roundCounter, gs.bluePoints, gs.redPoints, gs.lastScorePoints,
                      gs.lastScoreTeam, gs.phase, gs.availability, g.roster.Count());
    const uint64_t keyframeUs =
        g.builder.keyframeIntervalUs ? g.builder.keyframeIntervalUs : 2000000u;

    StatsSweepReport rep{};
    if (!SweepPlayerStats(&src, g.roster, g.playerStats, sourceUs, engineUs, signal,
                          kStatsRescanIntervalUs, keyframeUs, force, EmitPlayerStats, nullptr,
                          rep)) {
        if (rep.why == StatsSweepWhy::SourceAbsent) {
            ++g.statsSourceAbsent;
        } else {
            ++g.statsSignalUnchanged;
        }
        return;
    }
    ++g.statsSweeps;
    g.statsRecords += rep.written;
    g.statsKeyframes += rep.keyframes;
    g.statsRefusals.Merge(rep.refusals);

    if (!g.statsSaidSource && g.log) {
        g.statsSaidSource = true;
        // Said once, and it names the DENOMINATOR. `read 0 of 0` on a solo host with an empty
        // roster and `read 0 of 5` with five entrants the reader refused are different faults, and
        // a record count of zero cannot tell them apart.
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "stats: first sweep read %u of %u entrant(s), %u refused, %u record(s) "
                     "written. The scoreboard is read only when the game state, the roster or the "
                     "%llu us cadence says something moved",
                     unsigned(rep.read), unsigned(rep.entrants), unsigned(rep.refused),
                     unsigned(rep.written),
                     static_cast<unsigned long long>(kStatsRescanIntervalUs));
    }
}

// Roster rows this capture ever wrote a scoreboard for. Taken from the shadow store rather than
// counted into a variable, so it cannot drift from the records that were actually emitted.
uint16_t PlayerStatsUsers() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < kRosterMaxRows; ++i) {
        if (g.playerStats.rows[i].have) ++n;
    }
    return n;
}


// KB_AUDIO_PINS's source. The walk reads engine memory, which is legal on this thread and on no
// other, so the sample happens here and the chapter worker only copies what was published
// (capture/keyframe.h).
//
// ★ SAMPLED AT A FRACTION OF THE KEYFRAME INTERVAL, never per tick. Only a keyframe ever reads
// these bytes, and one encode per tick would pay 27 KB of walk and copy ninety times a second for
// a value read twice a minute. A quarter rather than the interval itself is what bounds the AGE of
// the pins a keyframe carries: at equal periods the two cadences beat against each other and a
// keyframe can carry pins a whole interval stale, which is a seek restoring the music of two
// seconds ago.
uint64_t g_pinsPublishedAtUs = 0;
uint64_t g_pinSamples = 0;
uint32_t g_pinLastBytes = 0;
audio::PinSource g_pinLastSource = audio::PinSource::Unstated;
uint64_t g_closedPinBlocks = 0;
uint64_t g_closedPinsAbsent = 0;
uint8_t g_pinEncodeBuf[audio::MaxPinBytes()];

void PublishAudioPinsTick(uint64_t sourceUs) {
    const uint64_t keyframeUs =
        g.builder.keyframeIntervalUs ? g.builder.keyframeIntervalUs : 2000000u;
    if (g_pinsPublishedAtUs != 0 && sourceUs - g_pinsPublishedAtUs < keyframeUs / 4u) {
        return;
    }
    g_pinsPublishedAtUs = sourceUs;
    audio::PinSource source = audio::PinSource::Unstated;
    const size_t n = audio::EncodeCurrentAudioPins(g_pinEncodeBuf, sizeof g_pinEncodeBuf, &source);
    ++g_pinSamples;
    g_pinLastBytes = static_cast<uint32_t>(n);
    g_pinLastSource = source;
    // n == 0 now means only that the encode did not fit: a host with no audio state still produces
    // a block, carrying PinSource::NoAudioState. It is published either way, and the worker counts
    // the keyframes it wrote without pins.
    PublishAudioPins(g_pinEncodeBuf, static_cast<uint32_t>(n));
}

// S5.4.7's ORACLE record (ruling Q-B: the stream ships in every capture, config-disableable).
//
// THE BUDGET, because "bounded" has to be a number rather than an intention. Both payloads and the
// record head must fit ONE ring push -- PushRecord stages into t_stage and refuses anything larger --
// so the two caps are DERIVED from that stage instead of chosen, and a builder promoted later cannot
// quietly outgrow the transport it pushes through. The tap runs once per CORE chapter, which at a
// ~90 Hz tick is one tick in about 180; the work it does is bracketed into its own `oracle` phase, so
// its cost is a measured row in the phase table rather than an estimate written down here.
constexpr uint32_t kOracleStageBytes =
    static_cast<uint32_t>(sizeof(t_stage) - sizeof(RecHeader) - sizeof(OracleRec));
constexpr uint32_t kOracleBonesCap = kOracleStageBytes / 2u;
constexpr uint32_t kOracleSessionCap = kOracleStageBytes - kOracleBonesCap;
static_assert(sizeof(OracleRec::bonesBytes) == sizeof(OracleRec::sessionBytes),
              "both payload sizes are stated in the same width");
constexpr uint64_t kOracleSizeFieldMax = (1ull << (8u * sizeof(OracleRec::sessionBytes))) - 1ull;
static_assert(kOracleSessionCap <= kOracleSizeFieldMax && kOracleBonesCap <= kOracleSizeFieldMax,
              "OracleRec states both payload sizes in its own size fields");

// The session payload occupies [0, kOracleSessionCap) and the bones payload starts at the cap, so the
// tap writes straight into the bytes that go on the ring and the record is one push.
uint8_t     g_oracleStage[kOracleStageBytes];
IOracleTap* g_oracleTap = nullptr;
uint64_t    g_oracleSampledAtUs = 0;
uint64_t    g_oracleSamples = 0;
uint64_t    g_oracleOk = 0;
TapStatus   g_oracleLastStatus = TapStatus::NotBound;
bool        g_oracleSitesOk = false;
char        g_oracleWhy[192] = {};
// Formatted ONCE at Init from the names OracleBuilders() refused with, then read by the tap line, by
// the stop line and by the close document, so the three cannot disagree about why the stream is
// empty. Empty means a real pair was bound.
char        g_oracleBindWhy[512] = {};

ClockFn   g_oracleClockForTest = nullptr;
// tests/capture only, and it is a REGISTRY and not a builder pair: the shipped decision below reads
// rows and tiers, so a suite reaches the bound state by PROMOTING rows in a view of the registry --
// the same promotion V-I-W2I1 will merge -- and by nothing else. A builder-pointer seam here would
// make "bound" a state only tests/ can construct, which is the shape AUD-FINAL R8c named.
const OracleRegistryView* g_oracleViewForTest = nullptr;

// Every row that could not be called, named with the tier it was refused at. Sized for all three rows
// at the longest tier word; the caller keeps the pointer, so it outlives the call.
char g_oracleMissing[256] = {};

// The two producers S5.4.7 names, answered as a PAIR from the REGISTRY. Binding one is NOT half a
// sample: a tap bound to a single /session arm returns an empty document in the other mode and that
// reads as a clean snapshot, so oracletap wants BOTH or neither.
//
// NEITHER IS A MODULE UNDER src/. S5.4.7's sources are engine addresses -- the /session handler's two
// arms and the /player_bones builder -- and oracleregistry.h holds the three rows the registry
// publishes for them. A row is callable only when the registry has promoted its CALL SHAPE (DL-22 /
// V-I-W2I1, W2-I); until then each refuses with its own name and tier, and the sentence travels to
// the log, the stop line and the close document. The two capture-side samplers that exist
// (S_GAMESTATE, S_PLAYER_SAMPLED) are our OWN reads: an ORACLE fed from them would be this capture
// compared against itself, the tautology S12.3 exists to avoid. Returns null once a real pair
// resolves -- and that happens when the REGISTRY changes, not when this function does.
// The ONE place the NotBound sentence is composed. Init writes it into g_oracleBindWhy, which the tap
// log line, the stop line and the close document all read, so whatever this writes is what all three
// print. Split out of Init because the shipped build takes the unbound branch and every wire case
// binds a stub pair: a branch no suite can enter is a branch no mutant can red.
void FormatOracleNotBound(const char* missing, char* out, size_t cap) {
    _snprintf_s(out, cap, _TRUNCATE,
                "NotBound(%s): S5.4.7's producers are engine addresses whose prototypes "
                "DL-22 / V-I-W2I1 (W2-I) has not promoted, so nothing was called -- the "
                "status is in every record and the stream is not empty",
                missing);
}

const char* OracleBuilders(BuilderFn& session, void*& sessionCtx, BuilderFn& bones, void*& bonesCtx) {
    const OracleRegistryView* const view = g_oracleViewForTest;
    const OracleRegistryRow* const rows = view != nullptr ? view->rows : kOracleRegistry;
    const size_t count = view != nullptr ? view->count : kOracleRoleCount;
    const OracleResolveFn resolve =
        (view != nullptr && view->resolve != nullptr) ? view->resolve : &GuardedOracleBuilder;

    session = nullptr;
    bones = nullptr;
    sessionCtx = view != nullptr ? view->sessionCtx : nullptr;
    bonesCtx = view != nullptr ? view->bonesCtx : nullptr;

    BuilderFn resolved[kOracleRoleCount] = {};
    bool named[kOracleRoleCount] = {};
    g_oracleMissing[0] = 0;
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        const OracleRegistryRow& r = rows[i];
        const char* tier = OracleRowTier(r);
        BuilderFn fn = nullptr;
        if (strcmp(tier, "CALLABLE") == 0) {
            fn = resolve(r);
            // The address is registered and the shape is promoted, but the bytes at the entry point
            // are not the ones the registry recorded: another mod's detour, or a rebase. Refused,
            // never called through.
            if (fn == nullptr) tier = "SITE-REFUSED";
        }
        if (fn != nullptr) {
            resolved[size_t(r.role)] = fn;
            continue;
        }
        named[size_t(r.role)] = true;
        const int n = _snprintf_s(g_oracleMissing + at, sizeof g_oracleMissing - at, _TRUNCATE,
                                  "%s%s:%s", at != 0 ? ", " : "", r.factId, tier);
        if (n < 0) break;   // the buffer holds all three; a truncated list would under-name them
        at += size_t(n);
    }
    // A role no row spoke for is refused too. A view that simply omits a producer must not read as a
    // producer that answered.
    for (size_t role = 0; role < kOracleRoleCount; ++role) {
        if (resolved[role] != nullptr || named[role]) continue;
        const int n = _snprintf_s(g_oracleMissing + at, sizeof g_oracleMissing - at, _TRUNCATE,
                                  "%s%s:ABSENT-FROM-REGISTRY", at != 0 ? ", " : "",
                                  kOracleRegistry[role].factId);
        if (n < 0) break;
        at += size_t(n);
    }
    if (g_oracleMissing[0] != 0) return g_oracleMissing;

    session = resolved[size_t(OracleRole::SessionPrimary)];
    bones = resolved[size_t(OracleRole::Bones)];
    return nullptr;
}

// ONE per CORE chapter (S5.4.7). The cadence is READ from the interval chapterbuilder splits a
// chapter on rather than restated as 0.5 Hz here, so the record cannot drift from the chapter it is
// defined as one of. Called from the tick thread only: everything the no-skew claim rests on is the
// absence of a frame boundary between the two builder calls, and that is only true here.
void SampleOracleTick(uint64_t sourceUs, uint32_t frameIndex) {
    if (g_oracleTap == nullptr || !g.builder.streamEnabled[kStreamOracle]) {
        return;
    }
    const uint64_t chapterUs = g.builder.keyframeIntervalUs;
    if (chapterUs == 0) {
        return;
    }
    if (g_oracleSampledAtUs != 0 && sourceUs - g_oracleSampledAtUs < chapterUs) {
        return;
    }
    g_oracleSampledAtUs = sourceUs;

    OracleSample s{};
    g_oracleTap->Sample(s);
    ++g_oracleSamples;
    g_oracleLastStatus = s.status;
    if (s.status == TapStatus::Ok) {
        ++g_oracleOk;
    }

    // A builder that claims more bytes than the cap it was handed is not believed: the payload is
    // dropped and the status the tap gave still reaches the file, which is a visible empty rather
    // than a read past the end of the stage.
    const uint32_t sessionLen = s.sessionLen <= kOracleSessionCap ? s.sessionLen : 0u;
    const uint32_t bonesLen = s.bonesLen <= kOracleBonesCap ? s.bonesLen : 0u;
    if (bonesLen != 0 && sessionLen < kOracleSessionCap) {
        memmove(g_oracleStage + sessionLen, g_oracleStage + kOracleSessionCap, bonesLen);
    }

    OracleRec r{};
    r.sourceTimeUs = sourceUs;
    r.frameIndex = frameIndex;
    r.status = static_cast<uint8_t>(s.status);
    r.sessionBytes = static_cast<uint16_t>(sessionLen);
    r.bonesBytes = static_cast<uint16_t>(bonesLen);
    // A failed push is already counted by kind inside PushRecord; the stop line reads that counter
    // rather than keeping a second one that could disagree with it.
    PushRecord(kRecOracle, &r, sizeof r, g_oracleStage, sessionLen + bonesLen);
}

void OnTick(const TickInfo& info, void* /*ctx*/) {
    if (!g.captureEnabled) {
        return;
    }
    g.tickThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // Constructed OUTSIDE the TrapGuard scope below, because CreateGameState allocates once and the
    // trap is armed while gathering. One instance per process, which is W1-D's contract.
    if (!gGameState.tried) {
        gGameState.tried = true;
        gGameState.state = gamestate::CreateGameState(gamestate::LiveEngineAccess());
        DiscloseGameState();
    }

    const uint64_t started = g.cost.Start();
    uint64_t phaseMark = started;
    // â˜… WHERE THE CAPTURE'S BUDGET STARTS (L2-RUN-2 F1). `gate` runs on every engine frame of the
    // launch, capture open or not, so it is the plugin's fixed cost and not the capture's. The
    // meter is charged from the end of it; see CostMeter::AddUnchargedUs for why the kill line is a
    // constant over this numerator rather than a baseline measured over the whole tick.
    uint64_t chargeFrom = started;
    // Set inside the block below and read after it: whether THIS tick did capture work is what
    // decides whether its cost is the capture's budget or the plugin's always-on floor.
    bool captureOpen = false;
    const auto lap = [&phaseMark](uint32_t which) {
        const uint64_t now = g.cost.Start();
        g.phaseOpen.tsc[which] += now - phaseMark;
        const double per = g.cost.TscPerUs();
        if (per > 0.0) {
            const uint32_t us = static_cast<uint32_t>(double(now - phaseMark) / per);
            if (us > g.phaseOpen.maxUs[which]) {
                g.phaseOpen.maxUs[which] = us;
            }
        }
        phaseMark = now;
    };
    {
        // Armed only while gathering, and only on this thread: the worker keeps allocating normally.
        TrapGuard guard;

        const Phase phase = g.phase.load(std::memory_order_acquire);
        const bool inGame = g.svc->session && g.svc->session->State() == NetGameState::InGame;

        g.inGame.store(inGame, std::memory_order_release);
        if (!inGame) {
            // A new match is a new file. Clearing the latch here rather than at the stop is exactly
            // what confines the suppression to the match whose file was already closed.
            g.suppressReopen.store(false, std::memory_order_release);
        }

        // ★ PART C + PART D. The recorder had NO session-type gate: it opened a file on the
        // state-9 tick of every session, offline playback launches included. Its subject is a live
        // online match, so that is what it now asks for -- through the same verdict part A's write
        // gates use, so there is one statement of "this is a live session" in the plugin.
        //
        // The gametype is deliberately NOT a term. On the owner's -spectatorstream path it reads
        // FFFFFFFF at open and resolves only later, so a recorder that waited for it would record
        // nothing at all.
        const bool manualStart = g.manualStart.exchange(false, std::memory_order_acq_rel);
        const bool liveSession =
            livesafety::Current() == livesafety::Verdict::LiveOnlineExempt || g.forceOffline;
        const bool startWanted = g.autoRecord || manualStart || g.forceOffline;
        if (inGame && !g.sessionGateSaid) {
            g.sessionGateSaid = true;
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "session gate: %s (%s), auto_record=%s force_offline=%s -- %s",
                         liveSession ? "recordable" : "NOT recordable",
                         livesafety::VerdictName(livesafety::Current()),
                         g.autoRecord ? "on" : "off", g.forceOffline ? "on" : "off",
                         liveSession ? (g.autoRecord ? "recording this match"
                                                     : "waiting for `record start`")
                                     : "no file will be written");
        }
        if (!inGame) g.sessionGateSaid = false;

        if (phase == Phase::Idle && inGame && liveSession && startWanted &&
            !g.suppressReopen.load(std::memory_order_acquire)) {
            g.pending.authorityFlags = session::SessionFlags();
            // S6.5's derivation, and it is descriptive metadata rather than a playback branch.
            const uint64_t f = g.pending.authorityFlags;
            g.pending.captureRole = (f & 0x04) ? 2u : ((f & 0x44) == 0x40 ? 1u : 0u);
            // The role decides the axis, so it is settled BEFORE the origin is taken.
            g.pending.t0Us = CaptureAxisTime(info, g.pending.captureRole);
            g.pending.partialStart = g.armedOnce || info.frameIndex > 600;
            // Per file, and here rather than at the open: the open runs on the worker and the netvar
            // pool belongs to this thread, so the ticks between arming and the file existing are the
            // only chance to have a sample ready for the FIRST keyframe. Reset must not land after
            // them or it would wipe exactly what it is here to collect.
            LiveNetvarSnapshot().Reset();
            g.openRequested.store(true, std::memory_order_release);
            SetPhase(Phase::Arming);
        } else if (phase == Phase::Recording && !inGame) {
            g.closeCauses.fetch_or(CloseCauseBit(StopReason::MatchEnd), std::memory_order_release);
            SetPhase(Phase::Closing);
        }

        lap(kPhaseGate);
        chargeFrom = phaseMark;

        // L2-RUN-11 F26. Read AFTER this tick's own transition rather than from the snapshot
        // above: a tick that has just published Phase::Closing would otherwise push a record and
        // walk a body that the closing drain may never see, which is how `bodies.refused` came to
        // count 7068 ticks against a stream carrying 7067 records. One predicate, one counter,
        // and both denominators are inside it.
        captureOpen = (phase == Phase::Recording) &&
                      (g.phase.load(std::memory_order_acquire) == Phase::Recording);
        // RECORDER-5 F3. The worker opens the file, and the open writes the first keyframe -- on a
        // thread that may not read the netvar pool. Every tick spent arming is a chance to have a
        // sample ready for that keyframe, so the HUD has a value at T+0 rather than at the second
        // keyframe. `0` because these ticks are the warm-up: there is nothing yet to be due.
        if (!captureOpen && g.phase.load(std::memory_order_acquire) == Phase::Arming) {
            LiveNetvarSnapshot().SampleOnTick(
                (g.svc && g.svc->session) ? g.svc->session->NetGame() : nullptr,
                CaptureAxisTime(info, g.pending.captureRole), 0);
        }
        if (captureOpen) {
            ++g.workTicks;
            const uint64_t sourceUs = AttributedSimTime(info);

            TickRec t{};
            t.sourceTimeUs = sourceUs;
            t.simTimeUs = info.simTimeUs;
            t.frameIndex = info.frameIndex;
            t.scaledDeltaUs = static_cast<uint32_t>(info.scaledDeltaUs);
            t.frameDeltaUs = static_cast<uint32_t>(info.frameDeltaUs);
            // ONE reading of the as-seen frame per tick. It used to be two: this call kept
            // peerTimeUs and valid, and GatherAsSeenFrame asked the engine for the whole frame
            // again a few phases later.
            AsSeenSample asSeen{};
            const bool asSeenRead = ReadPeerTime(g.svc->asSeen, t.peerTimeUs, t.asSeenValid, &asSeen);
            // The stream config is a property of the OPEN capture, not of the frame, so it is built
            // once here and handed to both gathers rather than rebuilt by each of them.
            const GatherConfig gc = BuildGatherConfig();
            lap(kPhasePeer);
            // â˜… The replay time axis. On a client the wire records carry the emitter's stamp and the
            // engine's peer-time estimate is the same domain, so the two agree; on a host there is no
            // peer time and the local sim clock IS the origin of every stamp. Choosing by role rather
            // than by "whichever is non-zero" keeps a client with a momentarily unavailable peer clock
            // from silently switching axes mid-file.
            if (g.builder.captureRole == 0 && (t.asSeenValid & kAsSeenPeerTime) && t.peerTimeUs) {
                t.sourceTimeUs = t.peerTimeUs;
            }
            // â˜… BEFORE the tick, not after. The first record of a session opens the first chapter
            // and writes its keyframe, so an origin pushed after the tick arrives one keyframe too
            // late and chapter 0 states "offset not sourced" for a session that had one all along.
            // Sampled every tick rather than every keyframe because the builder splits a chapter when
            // this CHANGES, and a change noticed two seconds late has already contaminated the
            // chapter it was meant to split.
            const WorldOriginSample wo = ReadWorldOrigin();
            lap(kPhaseOrigin);
            WorldOriginRec wr{};
            wr.sourceTimeUs = t.sourceTimeUs;
            wr.x = wo.x;
            wr.y = wo.y;
            wr.z = wo.z;
            wr.valid = wo.valid ? 1u : 0u;
            wr.suspect = wo.suspect ? 1u : 0u;
            PushRecord(kRecWorldOrigin, &wr, sizeof wr);

            PushRecord(kRecTick, &t, sizeof t);
            g.lastSourceUs.store(t.sourceTimeUs, std::memory_order_release);
            lap(kPhaseSpine);

            // The engine-read count across the SAME bracket, as a DIFFERENCE: SpanProbeCount is a
            // process-wide running total, and only a difference over a bracket is a per-tick budget.
            const uint64_t probesBefore = replay::entity::SpanProbeCount();
            uint64_t gatherSplit[kGatherPartCount] = {};
            GatherEntities(gc, t.sourceTimeUs, info.frameIndex, gatherSplit);
            g.phaseOpen.gatherProbes += replay::entity::SpanProbeCount() - probesBefore;
            for (uint32_t i = 0; i < kGatherPartCount; ++i) {
                g.phaseOpen.gatherTsc[i] += gatherSplit[i];
            }
            lap(kPhaseGather);
            const bool rosterScanRan = ScanRosterTick(t.sourceTimeUs);
            lap(kPhaseRoster);
            if (rosterScanRan) {
                EmitIdentityTick(t.sourceTimeUs, true);
            }
            lap(kPhaseIdentity);
            GatherAsSeenFrame(gc, t.sourceTimeUs, info.frameIndex, asSeenRead ? &asSeen : nullptr);
            lap(kPhaseAsSeen);
            // ARCH-21 S2: after the gather, and O(1) -- the sampler calls the engine's own typed
            // getters, so there is no walk and no slot address cached to go stale.
            SampleGameState(t.sourceTimeUs, info.simTimeUs);
            lap(kPhaseGameState);
            // SPEC-RECORDER-2 S2.3. The delta tap cannot see a netvar that does not change, so the
            // HUD keys' CURRENT value is read here, on the thread that owns the netvar pool, and the
            // worker's keyframe copies it. Twice per default keyframe interval, so every keyframe
            // gets a sample less than a second old even at the raised interval.
            LiveNetvarSnapshot().SampleOnTick(
                (g.svc && g.svc->session) ? g.svc->session->NetGame() : nullptr,
                t.sourceTimeUs, kNetvarSnapshotIntervalUs);
            lap(kPhaseNetvarSnap);
            // AFTER the game-state sample, because the sample IS the change signal: reading the
            // scoreboard first would gate this tick's read on last tick's score.
            SamplePlayerStats(t.sourceTimeUs, info.simTimeUs, false);
            lap(kPhaseStats);
            PublishAudioPinsTick(t.sourceTimeUs);
            lap(kPhaseAudioPins);
            // LAST, and in its own bracket. The tap calls into the engine's own builders, so its cost
            // is charged where it is spent instead of widening a neighbour's row.
            SampleOracleTick(t.sourceTimeUs, info.frameIndex);
            lap(kPhaseOracle);
        }
    }
    g.phaseOpen.openTicks += captureOpen ? 1u : 0u;
    if (++g.phaseOpen.ticks >= CostMeter::kWindowTicks) {
        g.phaseDone = g.phaseOpen;
        g.phaseSeq.fetch_add(1, std::memory_order_release);
        g.phaseOpen = PhaseWindow{};
    }
    const double perUs = g.cost.TscPerUs();
    // L2-RUN-8 F12. THE CHARGE WINDOW CLOSES WHEN THE CAPTURE DOES. `charged` is defined as work
    // that exists because a capture is open; with no capture open there is no such work, and a
    // tick that did none must not put a sample into the histogram the kill rung reads -- a run of
    // cheap post-close samples moves that median as surely as expensive ones do, and in the
    // direction that hides a real cost. The whole tick is the always-on floor instead, which is
    // where it belongs and where it is still reported.
    if (!captureOpen) {
        if (perUs > 0.0) {
            g.cost.AddUnchargedUs(static_cast<uint32_t>(double(g.cost.Start() - started) / perUs));
        }
    } else {
        if (perUs > 0.0) {
            g.cost.AddUnchargedUs(static_cast<uint32_t>(double(chargeFrom - started) / perUs));
        }
        g.cost.Stop(chargeFrom);
    }
    g.ticks.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// The taps. These may run on a thread that is not the tick -- W1-L could not establish which, so the
// ring set gives each producing thread its own SPSC ring and REPORTS the thread ids it saw.
// ---------------------------------------------------------------------------------------------

void OnNetvarMessage(const void* body, uint32_t bytes, uint64_t stampUs, void* /*ctx*/) {
    if (g.phase.load(std::memory_order_acquire) != Phase::Recording ||
        !g.builder.streamEnabled[kStreamNetvar]) {
        return;
    }
    NetvarRec r{};
    r.arrivalUs = stampUs;
    r.bodyBytes = static_cast<uint16_t>(bytes > 0xFFFFu ? 0xFFFFu : bytes);  // facts-ok: NETVAR.SLOT_NONE the u16 range limit, not the engine no-slot field
    PushRecord(kRecNetvar, &r, sizeof r, body, r.bodyBytes);
}

struct TapCtx {
    uint16_t typeIndex;
    StreamId stream;
};
TapCtx g_tapCtx[64];

// The tap the next tick adopts instead of building one from the engine. Null in the shipping DLL --
// nothing but tests/capture ever calls the setter. It exists because a host with no echovr.exe mapped
// has no CBroadcaster, so IWiretap::Listen's refusal-and-retry path is unreachable through
// WiretapCreate: every real tap there refuses at Available() long before any registration is tried.
IWiretap* g_tapOverrideForTest = nullptr;

void OnTapMessage(const WireMessage& m, void* ctx) {
    const TapCtx* t = static_cast<const TapCtx*>(ctx);
    const uint32_t bytes = static_cast<uint32_t>(m.bytes);
    if (bytes > 0xFFFFu) {  // facts-ok: NETVAR.SLOT_NONE the u16 range limit, not the engine no-slot field
        return;
    }
    const Phase phase = g.phase.load(std::memory_order_acquire);

    if (t->stream == kStreamPlayerWire || t->stream == kStreamBodyWire) {
        if (phase != Phase::Recording || !g.builder.streamEnabled[t->stream]) {
            return;
        }
        WireRec r{};
        // S6.3: a wire-sourced record keeps its own [8:16) source stamp and is NEVER re-stamped.
        memcpy(&r.stampUs, static_cast<const uint8_t*>(m.data) + 8,
               bytes >= 16 ? sizeof r.stampUs : 0);
        r.dst = m.peer;
        // â˜… W1-L (f05bd99): `m.stamp` is the engine delegate's argument 5, an uninterpreted qword
        // whose value is -1 for a locally-raised event. It was never a clock, and reading it as one is
        // what filed L2's three markers at 18446744073709.551 s. `arrivalUs` is the tap's own QPC
        // microsecond stamp, one basis across all 37 kinds, non-decreasing.
        r.arrivalUs = m.arrivalUs;
        r.bodyBytes = static_cast<uint16_t>(bytes);
        PushRecord(t->stream == kStreamPlayerWire ? kRecWirePlayer : kRecWireBody, &r, sizeof r,
                   m.data, bytes);
        return;
    }

    if (!g.builder.streamEnabled[kStreamEvent]) {
        return;
    }
    EventRec r{};
    // W1-L (f05bd99), as above: the arrival stamp is a time; `m.stamp` is an engine register. Every
    // consumer of EventRec::stampUs reads it as a time -- the lobby marker deriver among them -- so
    // this is the only thing that may be stored here.
    r.stampUs = m.arrivalUs;
    r.dst = m.peer;
    r.typeIndex = t->typeIndex;
    r.bodyBytes = static_cast<uint16_t>(bytes);
    // â˜… Recorded even before the file exists. The identity S5.4.1 needs -- entrant state, loadouts,
    // profiles, the balance blob -- all arrives during the lobby, which is before state 9 by
    // construction. The worker holds these until a file opens and then writes them into its first
    // chapter; dropping them would make every capture identity-less through a timing accident.
    PushRecord(kRecEvent, &r, sizeof r, m.data, bytes);
}

// ---------------------------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------------------------

void BuildFileName(wchar_t* out, size_t cap, uint64_t levelId, uint64_t gametypeId) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%04u-%02u-%02u_%02u-%02u-%02u_%016llX_%016llX.echodemo",
                 g.replayDir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                 static_cast<unsigned long long>(levelId),
                 static_cast<unsigned long long>(gametypeId));
}

void EnsureReplayDir() {
    // â˜… DELIBERATELY OUTSIDE paths::IsWritablePath. That predicate is W1-C's rule for PLUGIN STATE and
    // it enforces S4.5.3's "uninstall = delete one file" by keeping our own files inside the install.
    // S6.6 says replays go to the user's Documents and NEVER inside the game install, for the same
    // reason: a user's recordings are not install residue and must survive uninstalling the plugin.
    // The two rules do not conflict, they cover different things -- but the bypass is deliberate and
    // is written down here rather than left to be discovered.
    CreateDirectoryW(g.replayDir, nullptr);
}

void FlushPreOpen(ChapterBuilder& b) {
    const uint8_t* p = g.preOpen.Data();
    const uint8_t* end = p + g.preOpen.Size();
    while (p + sizeof(RecHeader) <= end) {
        RecHeader h{};
        memcpy(&h, p, sizeof h);
        const size_t total = sizeof(RecHeader) + h.bytes;
        if (p + total > end) {
            break;
        }
        b.Consume(p, static_cast<uint32_t>(total));
        p += total;
    }
    g.preOpen.Drain();
}

// The four MANIFEST blobs S5.4.1 specifies, filled from the one-shot messages the tap already
// buffered before the file existed. Every one of them is a Blob -- raw bytes, not a parsed structure --
// so this populates them VERBATIM and decodes nothing. S5.4.1 even sizes netIdSlotMap at 218-278 B,
// which is exactly the SR15NetIdsResponse body.
//
// â˜… WHY VERBATIM AND NOT PARSED, on the net-id map specifically. Its framing is certain: over both real
// messages in the reference capture, size == 8 + n*(8+2) exactly, the trailing u16 column is ascending
// and consecutive, and the two messages carry disjoint contiguous slot ranges 0..26 and 27..47. But the
// u64 column is NOT a per-actor identity in the simple sense the name suggests -- in one message its 27
// entries hold only FOUR distinct values, one repeated 15 times and one 10 times, which lines up with
// S5.4.5's structural groups rather than with unique ids. So the framing is known and the semantics is
// not, and this stores the bytes rather than a reading of them.
void FillManifestBlobsFromPreOpen() {
    const uint16_t idxNetIds = TapTypeIndex("SR15NetIdsResponse");
    const uint16_t idxBalance = TapTypeIndex("SR15NetBalanceSettingsMassUpdate");

    const uint8_t* p = g.preOpen.Data();
    const uint8_t* end = p + g.preOpen.Size();
    uint32_t netIds = 0, balance = 0, slot0 = 0, slot1 = 0;
    uint16_t netIdMsgs = 0;
    std::vector<uint8_t> netIdList;
    while (p + sizeof(RecHeader) <= end) {
        RecHeader h{};
        memcpy(&h, p, sizeof h);
        const size_t total = sizeof(RecHeader) + h.bytes;
        if (p + total > end) {
            break;
        }
        const uint8_t* payload = p + sizeof(RecHeader);

        if (h.kind == kRecEvent && h.bytes >= sizeof(EventRec)) {
            EventRec r{};
            memcpy(&r, payload, sizeof r);
            const uint8_t* body = payload + sizeof(EventRec);
            if (sizeof(EventRec) + r.bodyBytes <= h.bytes) {
                if (r.typeIndex == idxNetIds) {
                    // â˜… EVERY response, not the first. There are TWO, carrying two disjoint slot
                    // ranges (0..26 and 27..47) out of two different id spaces, and keeping only the
                    // first stored 27 of 48 slots -- the half whose 27 entries hold four distinct
                    // values -- while nothing in the file said the other half existed. The bytes did
                    // survive on S_EVENT, so no file already recorded lost data; the manifest field was
                    // a silent partial, which is the worse failure of the two because it reads as
                    // complete.
                    //
                    // The blob is a length-prefixed LIST rather than a concatenation. A concatenation
                    // happens to be walkable here, since one response's n falls out of (len - 8) / 10,
                    // but that is a property of today's framing and a reader would be inferring it. The
                    // prefix is written even for a single response so there is one rule, not two.
                    const uint32_t n = r.bodyBytes;
                    const uint8_t* np = reinterpret_cast<const uint8_t*>(&n);
                    netIdList.insert(netIdList.end(), np, np + sizeof n);
                    netIdList.insert(netIdList.end(), body, body + n);
                    netIds += n;
                    ++netIdMsgs;
                } else if (r.typeIndex == idxBalance && !balance) {
                    // S5.4.1: stored as received, never re-encoded. It is already zstd on the wire.
                    g.manifest.SetBalanceBlob(body, r.bodyBytes);
                    balance = r.bodyBytes;
                }
            }
        } else if (h.kind == kRecNetvar && h.bytes >= sizeof(NetvarRec)) {
            NetvarRec r{};
            memcpy(&r, payload, sizeof r);
            const uint8_t* body = payload + sizeof(NetvarRec);
            if (sizeof(NetvarRec) + r.bodyBytes <= h.bytes) {
                const DuvMessage m = DuvParse(body, r.bodyBytes);
                // The baselines are the two big opening rowsets: 56 rows on slot 0, 23 on slot 1. The
                // test is "the biggest rowset seen for this slot", not a row count this code asserts,
                // because a level with a different var set would still have a baseline and a hardcoded
                // 56 would silently skip it.
                if (m.status == DuvStatus::Ok && m.hasRowset) {
                    if (m.slot == 0 && r.bodyBytes > slot0) {
                        g.manifest.SetSlot0Baselines(body, r.bodyBytes);
                        slot0 = r.bodyBytes;
                    } else if (m.slot == 1 && r.bodyBytes > slot1) {
                        g.manifest.SetSlot1RuleVars(body, r.bodyBytes);
                        slot1 = r.bodyBytes;
                    }
                }
            }
        }
        p += total;
    }

    if (!netIdList.empty()) {
        g.manifest.SetNetIdSlotMap(netIdList.data(), static_cast<uint32_t>(netIdList.size()));
    }

    if (g.log) {
        // Reported every session, because an empty blob is a dependency nobody can see from the file.
        // Zero on netIdSlotMap means player rows -- which are keyed by user slot -- have nothing in the
        // file to resolve them against.
        g.log->Write(netIds ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
                     "manifest blobs from %u buffered pre-open records: netIdSlotMap=%u B in %u "
                     "response(s), balance=%u B, slot0Baselines=%u B, slot1RuleVars=%u B; "
                     "pre-open shed %u records / %u B over %u shed(s)",
                     g.preOpen.Records(), netIds, netIdMsgs, balance, slot0, slot1,
                     g.preOpen.DroppedRecords(), g.preOpen.DroppedBytes(), g.preOpen.Sheds());
    }
}

// â˜… AUDIT_M6 F2. Every failed open ends here, and this is what bounds it. An open that fails once
// usually fails for the same reason on every attempt -- a full disk, a path that does not fit, a
// builder that refuses this manifest -- so the reason is logged when the STATE changes and counted
// otherwise, and the next attempt is pushed out by a doubling backoff. Worker thread only.
void NoteOpenFailure(OpenFailKind kind, const char* what) {
    const bool stateChanged = kind != g.openFailKind;
    g.openFailKind = kind;
    ++g.openFailStreak;
    ++g.openFailTotal;
    g.openBackoffMs = g.openBackoffMs == 0
                          ? kOpenRetryFirstMs
                          : (g.openBackoffMs >= kOpenRetryCapMs / 2 ? kOpenRetryCapMs
                                                                    : g.openBackoffMs * 2);
    g.openRetryAtMs = GetTickCount64() + g.openBackoffMs;
    bool sayIt = stateChanged;
#if defined(W2A_MUTATE_OPEN_RETRY_UNBOUNDED)
    // Mutation control: the pre-fix shape -- no backoff and one Error line per attempt,
    // which is ~250 of each per second for as long as the match lasts.
    g.openRetryAtMs = 0;
    sayIt = true;
#endif
    if (sayIt && g.log) {
        g.log->Write(LogLevel::Error, kReplayModuleName,
                     "could not open the capture (%s): %s. Nothing is being recorded for this "
                     "match; the open is retried with a backoff of up to %u ms and this line is "
                     "written again only if the failure changes",
                     OpenFailStateName(kind), what, unsigned(kOpenRetryCapMs));
    }
}

// The entropy, kept apart from the shape (capture_types.h decides what a demo uuid IS). Three words
// that move for different reasons: the wall clock, which separates two runs; the process and thread
// ids plus a load-address bit, which separate two clients started in the same 100 ns tick; and a
// per-open counter, which separates the several files ONE session writes when a match reopens.
uint32_t g_demoUuidOpens = 0;

DemoUuid MakeDemoUuid() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t when = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const uint64_t who = (uint64_t(GetCurrentProcessId()) << 32) ^ GetCurrentThreadId() ^
                         reinterpret_cast<uintptr_t>(&g_demoUuidOpens);
    return DemoUuidFrom(when, who, ++g_demoUuidOpens);
}

bool OpenFile() {
    EnsureReplayDir();
    g.manifest.Begin(g.pending.t0Us, g.pending.captureRole, g.pending.authorityFlags);
    // L2-RUN-2 F7. SetUuid had no caller, so every capture ever written carries an all-zero demoUuid
    // and no artifact -- sentinel, harness row, or a second copy of the same file -- can be tied to
    // the capture it came from. Minted per OPEN, not per session: one match, one file, one identity.
    {
        const DemoUuid id = MakeDemoUuid();
        g.manifest.SetUuid(id.lo, id.hi);
        g.lastUuid = id;
    }
    // W2-A. The level id is READ OFF THE ENGINE here, not invented and not left at the
    // zero-initialised default every file carried until now: playback compares each netvar
    // rowset owner word against exactly this u64 (levelid.h names the two hops and the
    // instructions they come from). Read at open on the worker thread, beside the roster scan
    // and the profile fetch -- never on the armed tick. A chain that cannot be read records 0
    // and says which hop refused; it never refuses the capture.
    {
        void* const ng = (g.svc && g.svc->session) ? g.svc->session->NetGame() : nullptr;
        LevelIdRead lid;
        if (!FillLevelId(g.pending.levelId, ng, ng ? LiveFindCs() : nullptr, lid) && g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "level id: not read (%s). The file records 0, so playback will leave "
                         "its netvar rowsets unbound rather than compare them against the "
                         "wrong level; everything else in this capture is unaffected",
                         LevelIdWhyText(lid.why));
        }
    }
    // W2-A. The gametype id, on the same terms as the level id above: it was declared, read by
    // SetLevel and by BuildFileName, and assigned nowhere, so every file carried gametype 0 and
    // every filename claimed it. Two hops off the CR15NetGame, both registered (gametypeid.h names
    // them and the instructions they come from), span-proved, on the worker thread at open.
    {
        void* const ng = (g.svc && g.svc->session) ? g.svc->session->NetGame() : nullptr;
        GametypeIdRead gt;
        if (!FillGametypeId(g.pending.gametypeId, ng, gt) && g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "gametype id: not read (%s). The file records 0, so the manifest and the "
                         "file name both say gametype 0 rather than a wrong one; everything else "
                         "in this capture is unaffected",
                         GametypeIdWhyText(gt.why));
        }
    }
    g.manifest.SetLevel(g.pending.levelId, g.pending.gametypeId, 0);
    // L3-RUN-10 F3. SetBuild had no non-test caller, so every capture ever made recorded sixteen
    // zero bytes where the game's fingerprint goes, and the browser -- which compares that field
    // against this build's expected md5 -- had nothing to compare. The identity is NOT measured
    // here: buildguard hashed the main module once at load and latched it (35 MB), and the file is
    // never re-read. Recorded whatever the verdict was: a capture made against a build we refused
    // is exactly the capture whose fingerprint matters most, and erasing it would leave the reader
    // unable to tell "wrong build" from "never recorded".
    {
        const BuildIdentity& id = buildguard::LastIdentity();
        uint8_t md5[16];
        const bool haveMd5 = id.md5Read && Md5HexToBytes(id.md5, md5);
        if (!haveMd5) memset(md5, 0, sizeof md5);
        // The build string is a WITNESS, not our constant asserted: buildguard counts occurrences of
        // the expected RAD_Version string in the mapped image and does not extract the running one,
        // so this records the hash only when the scan actually found it, and 0 when it did not.
        // contentHash stays 0: nothing in this plugin computes a catalogue hash yet, and a made-up
        // value would read as a recorded fact.
        const uint64_t buildStringHash = (id.imageScanned && id.versionStringCount > 0)
                                             ? BuildStringHash64(ExpectedVersionString())
                                             : 0;
        g.manifest.SetBuild(md5, buildStringHash, 0);
        if (!haveMd5 && g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "build fingerprint: not recorded (the guard has no md5 for this module). "
                         "The file records sixteen zero bytes, which readers report as NOT RECORDED "
                         "rather than as a mismatch; everything else in this capture is unaffected");
        }
    }
    FillManifestBlobsFromPreOpen();
    g.manifest.SetKeyframeIntervalUs(g.cost.KeyframeIntervalUs());
    g.manifest.SetPartialStart(g.pending.partialStart);

    // THE MANIFEST ROSTER, filled HERE because this is the last instant before the header is
    // serialised. AddPlayer had no non-test caller at all, which is why `echodemo info` printed
    // `players : 0` beside 6 997 stored player rows (L2-RUN-3 F3): nothing was wrong with the
    // sampler. Scanned rather than assumed -- the table may already hold rows from ticks before the
    // file existed, and the scan is idempotent, so this both fills an empty table and refreshes a
    // populated one.
    // Every file carries its own complete set of identity blocks. The roster table lives for the
    // process and a second capture would find every row unchanged, so a producer keyed on change
    // alone would open file two with no identity in it at all.
    g.identityEmit.Reset();
    ScanRosterNow(g.pending.t0Us, false);
    // Off the armed tick: this is the worker thread, and the file needs its whole roster at once.
    EmitIdentityTick(g.pending.t0Us, false);
    {
        // The profile documents are fetched HERE and not per tick: AddPlayer copies the blob into the
        // manifest's arena, the manifest is serialised once, and walking every entrant's CJson tree
        // on a 90 Hz tick to produce bytes nothing reads until this instant would be the instrument
        // perturbing the system. One buffer, reused per row, at the file's own cap.
        void* ng = (g.svc && g.svc->session) ? g.svc->session->NetGame() : nullptr;
        const IIdentity* id = g.svc ? g.svc->identity : nullptr;
        if (!ng && id) {
            ng = identity_access::NetGameOf(id);
        }
        EngineRosterSource src(ng, id, g.svc ? g.svc->entity : nullptr);
        const IRosterSource* jsonSrc = src.Usable() ? &src : nullptr;
        static uint8_t entrantBuf[kRosterJsonCap];
        static uint8_t unlocksBuf[kRosterJsonCap];

        for (uint16_t i = 0; i < g.roster.Count(); ++i) {
            PlayerRecord pr;
            FillPlayerRecord(g.roster.At(i), pr);
            FillPlayerJson(jsonSrc, g.roster.At(i).userIndex, entrantBuf, unlocksBuf, kRosterJsonCap,
                           pr, g.rosterRefusals);
            g.manifest.AddPlayer(pr);
        }
    }
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "roster: %u row(s) into the manifest at open, %u of them local, %u field "
                     "refusal(s) so far. A zero here is an absent roster, not an empty match",
                     unsigned(g.roster.Count()), unsigned(LocalRosterRows()),
                     unsigned(g.rosterRefusals.Total()));
    }
    if (const char* why = g.manifest.Refusal()) {
        char what[192];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "the schema table refused it: %s", why);
        NoteOpenFailure(OpenFailKind::SchemaRefused, what);
        return false;
    }

    wchar_t path[kPathMax];
    BuildFileName(path, kPathMax, g.pending.levelId, g.pending.gametypeId);
    if (!g.writer.Open(path, g.manifest.View())) {
        char what[192];
        _snprintf_s(what, sizeof(what), _TRUNCATE, "%ls could not be created: %s", path,
                    g.writer.LastError() ? g.writer.LastError() : "no reason given");
        NoteOpenFailure(OpenFailKind::WriterOpen, what);
        return false;
    }
    AcquireSRWLockExclusive(&g.curLock);
    wcscpy_s(g.curPath, kPathMax, path);
    ReleaseSRWLockExclusive(&g.curLock);
    g.builder.captureRole = g.pending.captureRole;
    g.builder.keyframeIntervalUs = g.cost.KeyframeIntervalUs();
    // DemoWriter::WritePlayerStats is non-virtual -- Appendix B.2's IDemoWriter signatures are
    // frozen -- so the concrete writer is handed over separately, exactly as SetRoundTable is.
    // L2-RUN-11 F27. SetLog had NO caller anywhere, so log_ was null in every launch and the
    // Warn line that names a refused round boundary could not fire on any path -- two refusals were
    // counted on the stop line and named nowhere in the log. Handed over at the open that gives the
    // builder its file, before any record reaches it.
    g.builder2.SetLog(g.log);
    g.builder2.SetStatsWriter(&g.writer);
    // NOT reset here. The reset moved to the arming tick (the gate, above) so the ticks between
    // arming and this open can prime the sample the first keyframe copies; resetting here would
    // throw that sample away at the last moment.
    g.builder2.SetNetvarSnapshotSource(&LiveNetvarSnapshot());
    if (!g.builder2.Open(&g.writer, g.manifest.View(), g.builder)) {
        // â˜… AUDIT_M5 F5. This was the one way to produce no capture and no reason anywhere: the two
        // failure paths above log at Error, this one closed the writer and returned false in silence,
        // and the worker then set Phase::Idle equally silently. ChapterBuilder::Open takes no logger
        // and returns a bare false, so the path is named here rather than inside it.
        char what[192];
        _snprintf_s(what, sizeof(what), _TRUNCATE,
                    "%ls opened but its chapter builder refused it, so it is closed again", path);
        NoteOpenFailure(OpenFailKind::BuilderRefused, what);
        g.writer.Close();
        return false;
    }
    g.builder2.SetRoster(&g.roster);
    const uint32_t preOpenPlaced = g.preOpen.Records();
    FlushPreOpen(g.builder2);
    // RECORDER-5 F1. These rode the file origin -- Consume rides the axis for a foreign-stamped
    // record and nothing has moved it off t0 yet -- but each one still carries its OWN arrival
    // stamp, which is on the pre-open axis. Saying so is what stops a reader from reading those
    // stamps as file times: the goal that arrived 0.2 s before the 09-19 capture opened is in the
    // file, it is real, and it belongs at the origin rather than wherever its own stamp lands.
    if (preOpenPlaced) {
        g.builder2.NoteDegraded(g.pending.t0Us, kStreamEvent, kDegradePreOpenIdentityPlaced,
                                preOpenPlaced);
    }

    // A5. Identity traffic shed before the file existed is a real loss and the file has to say so.
    // It cannot be a GAP -- a gap names a range of samples on a stream, and these records were shed
    // before any stream had a range -- so it is a DEGRADED record naming the count, emitted once, at
    // the first instant there is a file to write it into. Zero shed writes nothing: a degraded record
    // that always appears would train a reader to ignore it.
    if (g.preOpen.DroppedRecords() && !g.preOpenLossDeclared) {
        g.builder2.NoteDegraded(g.pending.t0Us, 0, kDegradePreOpenIdentityDropped,
                                g.preOpen.DroppedRecords());
        g.preOpenLossDeclared = true;
    }

    // S5.7's MATCH_BEGIN. It is the one marker no netvar edge can produce, because it is a property of
    // the file rather than of the match.
    g.builder2.EmitMatchBegin(g.pending.t0Us);
    wcscpy_s(g.openPath, kPathMax, path);
    g.fileOpen = true;
    if (g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName, "recording, role=%u", g.builder.captureRole);
    }
    return true;
}

// The `closed:` line is the harness's kill signal (H10/P10): it taskkills as soon as it sees one. On
// the stop path that line therefore has to be the LAST thing written -- after the reopen verdict and
// after the sentinel is on disk (AUDIT_D_SC7 D-5) -- so the counts are stashed here and the line is
// emitted by whoever knows this process is safe to lose.
void LogClosedLine() {
    if (!g.closedLinePending) {
        return;
    }
    g.closedLinePending = false;
    if (!g.log) {
        return;
    }
    // L2-RUN-13 F33: the pin counters were incremented and read by nobody, so a capture with no
    // block 16 could not be told from one whose pin walk never ran. Emitted BEFORE the closed line,
    // which the harness treats as the kill signal.
    const char* pinWhy = "";
    if (g_pinSamples == 0) {
        pinWhy = ", never sampled: no tick ran with a capture open";
    } else if (g_pinLastBytes == 0) {
        pinWhy = ", encode did not fit";
    } else if (g_pinLastSource != audio::PinSource::Bound) {
        pinWhy = ", no audio state bound: empty for want of a source, not for want of sound";
    }
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "audio pins: %llu samples, %u bytes, %llu keyframe blocks, %llu absent%s",
                 static_cast<unsigned long long>(g_pinSamples), g_pinLastBytes,
                 static_cast<unsigned long long>(g_closedPinBlocks),
                 static_cast<unsigned long long>(g_closedPinsAbsent), pinWhy);
    // The same shape as the pin line above, and for the same reason: a stream that ships by default
    // (ruling Q-B) with nothing in it has to say WHICH of several reasons it is empty for. An unbound
    // tap is a named reason, never a refusal of the capture.
    const char* oracleWhy = "";
    if (g_oracleSamples == 0) {
        oracleWhy = g.builder.streamEnabled[kStreamOracle]
                        ? "never sampled: no tick ran with a capture open"
                        : "capture.oracle is off";
    } else if (!g_oracleSitesOk) {
        oracleWhy = "builder sites refused: somebody else is on them, so nothing was called";
    } else if (g_oracleOk == 0) {
        oracleWhy = g_oracleBindWhy[0] ? g_oracleBindWhy
                                       : "bound, but every sample refused: read the last status";
    }
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "oracle: %llu samples, %llu ok, %llu dropped, last status %s%s%s",
                 static_cast<unsigned long long>(g_oracleSamples),
                 static_cast<unsigned long long>(g_oracleOk),
                 static_cast<unsigned long long>(
                     g.pushFailByKind[kRecOracle].load(std::memory_order_relaxed)),
                 TapStatusName(g_oracleLastStatus), oracleWhy[0] ? ", " : "", oracleWhy);
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "closed: %llu records, %llu chapters, %llu keyframes, %llu verbatim, %llu refusals",
                 static_cast<unsigned long long>(g.closedRecords),
                 static_cast<unsigned long long>(g.closedChapters),
                 static_cast<unsigned long long>(g.closedKeyframes),
                 static_cast<unsigned long long>(g.closedVerbatim),
                 static_cast<unsigned long long>(g.closedRefusals));
    // A count of refusals is not a diagnosis. One line per NAMED refusal says which field of which
    // record could not be stored, what it held and how wide the format declares it -- the three
    // facts that separate a value which outgrew its field from a buffer that ran out.
    for (uint32_t i = 0; i < g.closedRefusalsNamed; ++i) {
        const EncodeRefusalNote& n = g.closedRefusalNotes[i];
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "refused: %s field=%s value=0x%llX width=%u",
                     n.stream ? n.stream : "?", n.field ? n.field : "?",
                     static_cast<unsigned long long>(n.value), unsigned(n.width));
    }
}

// The Init sweep, one line per file (plan item 2, "logging each deletion"). A survivor is the case
// that decides a verdict -- a stale document the reader can still find -- so it is named at Warn
// (AUDIT_G G-11).
void ReportSweptSentinel(const wchar_t* name, bool deleted, void* ctx) {
    if (!g.log) {
        return;
    }
    const wchar_t* dir = static_cast<const wchar_t*>(ctx);
    g.log->Write(deleted ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
                 deleted ? "stop sentinel swept: %ls\\%ls"
                         : "stop sentinel WOULD NOT DELETE: %ls\\%ls - it can still be read as this "
                           "launch's",
                 dir, name);
}

// RECORDER-5 F5. On the owner's spectate path the engine's gametype field reads all-ones at open
// and only the lobby's owner-state event resolves it, so the file is NAMED `FFFFFFFFFFFFFFFF` while
// its manifest correctly says `echo_arena`. The manifest is already corrected at close; this makes
// the name agree with it.
//
// AFTER g.writer.Close() and nowhere else: the writer owns the handle until then, and Windows will
// not move a file that is still open for writing. Nothing here may put the capture at risk -- a
// refused rename keeps the old name and says so, and the FILE is identical either way, so a reader
// that goes by the manifest never notices. Only the last field of the name changes, so the
// timestamp and the level id that a person recognises the recording by stay exactly as they were.
void RenameToResolvedGametype(uint64_t resolved) {
    if (resolved == 0 || g.openPath[0] == 0) {
        return;
    }
    wchar_t want[17];
    _snwprintf_s(want, 17, _TRUNCATE, L"%016llX", static_cast<unsigned long long>(resolved));

    // The name ends `_<16 hex>.echodemo`; the field is found from the end rather than by re-building
    // the name, because BuildFileName takes a fresh local time and would not reproduce this one.
    const size_t len = wcslen(g.openPath);
    const wchar_t* kExt = L".echodemo";
    const size_t extLen = wcslen(kExt);
    if (len < extLen + 17 || _wcsicmp(g.openPath + len - extLen, kExt) != 0) {
        return;
    }
    const size_t at = len - extLen - 16;
    if (g.openPath[at - 1] != L'_' || wcsncmp(g.openPath + at, want, 16) == 0) {
        return;   // already names the resolved value: nothing to do, and no log line to explain
    }

    wchar_t renamed[kPathMax];
    wcscpy_s(renamed, kPathMax, g.openPath);
    memcpy(renamed + at, want, 16 * sizeof(wchar_t));
    if (MoveFileExW(g.openPath, renamed, 0)) {
        if (g.log) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "renamed to the resolved gametype: %ls (the engine field read all-ones at "
                         "open; the manifest and the name now agree)",
                         renamed);
        }
        wcscpy_s(g.openPath, kPathMax, renamed);
        return;
    }
    if (g.log) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "the capture keeps its open-time name %ls: renaming it to the resolved "
                     "gametype 0x%016llX failed (Win32 %lu). The manifest carries the resolved "
                     "value, so only the name is stale",
                     g.openPath, static_cast<unsigned long long>(resolved), ::GetLastError());
    }
}

void CloseFile(bool announce) {
    if (!g.fileOpen) {
        return;
    }
    g.builder2.EmitMatchEnd(g.builder2.LastSourceTimeUs());
    g.builder2.Close();
    // ARCH-21 S4.4/S4.5: the round table is decided at close and filed on the CONCRETE writer --
    // IDemoWriter's signatures are frozen (B.2), so this is the seam that can carry it. After
    // builder2.Close(), which is what turns a round still open into a partialEnd row.
    g.roundRowsRecorded = true;
    g.writer.SetRoundTable(g.builder2.RoundRows(), g.builder2.RoundRowCount(),
                           g.builder2.ChosenMarkerSource());
    const uint64_t lateGametype =
        LateGametype(g.manifest.View().gametypeId, g.builder2.OwnerStateGametype());
    if (lateGametype != 0 && g.writer.SetGametypeId(lateGametype) && g.log) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "gametype id: the engine field read 0x%016llX at open; the manifest records "
                     "0x%016llX, which the lobby's owner-state event stated",
                     static_cast<unsigned long long>(g.manifest.View().gametypeId),
                     static_cast<unsigned long long>(lateGametype));
    }
    g.writer.Close();
    g.fileOpen = false;
    RenameToResolvedGametype(lateGametype);
    wcscpy_s(g.lastClosedPath, kPathMax, g.openPath);
    g.openPath[0] = 0;
    const BuilderStats& b = g.builder2.Stats();
    g.lastRecords = b.recordsWritten;
    g.lastChapters = b.chaptersWritten;
    g.closedRecords = b.recordsWritten;
    g.closedChapters = b.chaptersWritten;
    g.closedKeyframes = b.keyframes;
    g_closedPinBlocks = b.audioPinBlocks;
    g_closedPinsAbsent = b.audioPinsAbsent;
    g.closedVerbatim = b.wireVerbatim;
    g.closedRefusals = b.encodeRefusals;
    g.closedRefusalsNamed = b.encodeRefusalsNamed;
    for (uint32_t i = 0; i < g.closedRefusalsNamed; ++i) {
        g.closedRefusalNotes[i] = b.encodeRefusalNotes[i];
    }
    g.closedMarkers = g.builder2.MarkerStats();
    g.closedLocal = g.builder2.LocalMarkerStats();
    g.closedSource = g.builder2.ChosenMarkerSource();
    g.closedCorroboratorAsked = g.builder2.CorroboratorAsked();
    g.closedWireRows = g.builder2.WireRowsSeen();
    g.closedLinePending = true;
    if (g.log) {
        // RECORDER-5 F3. The snapshot's own close line. `never read` is the one that matters: a key
        // that answered zero times in a whole match is a symbol this build cannot read, not a quiet
        // match, and the recorder and the playback HUD share the list -- so a wrong symbol here is a
        // wrong column there. Named individually, because a count alone cannot be acted on.
        const NetvarSnapshotStats ns = LiveNetvarSnapshot().Stats();
        char never[256];
        never[0] = 0;
        for (uint16_t i = 0; i < NetvarSnapshotCache::KeyCount(); ++i) {
            if (ns.everMask & (1u << i)) {
                continue;
            }
            strcat_s(never, sizeof never, never[0] ? " " : "");
            strcat_s(never, sizeof never, NetvarSnapshotCache::KeyName(i));
        }
        g.log->Write(ns.keysNeverRead ? LogLevel::Warn : LogLevel::Info, kReplayModuleName,
                     "netvar snapshot: %u sample(s), %u refused for an unbound route, %u key-read(s),"
                     " %u fault(s); %u of %u keys never answered%s%s",
                     ns.samples, ns.routeRefused, ns.keysRead, ns.keysFaulted, ns.keysNeverRead,
                     unsigned(NetvarSnapshotCache::KeyCount()), never[0] ? ": " : "", never);

        // â˜… A marker count of zero is ambiguous on its own -- a quiet match and a wrong set of netvar
        // symbols look identical. The seen-mask is what tells them apart, so it is logged beside the
        // count rather than left for somebody to work out.
        const MarkerDeriver::Stats& ms = g.builder2.MarkerStats();
        g.log->Write(ms.namesSeenMask == 0 && ms.rowsSeen > 0 ? LogLevel::Warn : LogLevel::Info,
                     kReplayModuleName,
                     "markers: %u emitted from %u netvar rows out of %llu DUV record(s) delivered "
                     "(%llu malformed); %u of %u watched names were ever seen (mask 0x%X); %u refused "
                     "for an untrusted timestamp. A zero mask with a non-zero row count means the "
                     "netvar symbols are wrong, not that the match was quiet; a zero DENOMINATOR would "
                     "mean the watch table was never built; and ZERO DELIVERED RECORDS means the "
                     "deriver was never fed at all, which is neither of those and is not ours",
                     ms.emitted, ms.rowsSeen, g.builder2.Stats().netvarRecords,
                     g.builder2.Stats().netvarMalformed,
                     unsigned(__popcnt(ms.namesSeenMask)), ms.namesWatched,
                     ms.namesSeenMask, ms.timeRefused);
    }
    if (announce) {
        LogClosedLine();
    }
}

void SinkRecord(const void* rec, uint32_t bytes, void* /*ctx*/) {
    if (g.fileOpen) {
        g.builder2.Consume(rec, bytes);
        return;
    }
    // No file yet. The cap, the shed and the accounting for what the shed cost are the buffer's
    // own invariant -- see preopen.h.
    g.preOpen.Push(rec, bytes);
}

// E-C12: what is still queued belongs in the file, not in a ring nobody read.
void DrainAndClose(bool announce = true) {
    for (int i = 0; i < 64; ++i) {
        if (g.rings.Drain(g.scratch.data(), kWorkerScratchBytes, &SinkRecord, nullptr,
                          kWorkerDrainPerPass) == 0) {
            break;
        }
    }
    CloseFile(announce);
}

// L3-RUN-13 F3. ONE WRITER for the close document's tail block. Every site that builds a StopFacts
// reads the meter through this, because a site that forgets it publishes five zeros -- and zero
// measured windows reads as "the rung ran and never fired" rather than "nobody asked the meter".
CostTailFacts CostTailNow() {
    const CostMeter::WindowStat w = g.cost.LastWindow();
    CostTailFacts t;
    t.measured = w.tailMeasuredWindows;
    t.notRun = w.tailNotRunWindows;
    t.overBudget = w.tailOverBudgetWindows;
    t.kills = w.tailKillWindows;
    t.chargedMeanUs = w.chargedMeanUs;
    return t;
}

// C6's close-and-prove-it. Worker thread only -- it re-opens and decodes a file, which is filesystem
// work no tick may ever do (CDeadlockMonitor kills the process after ten seconds without a Tick).
//
// The order is deliberate: close FIRST so the container is whole, then re-open it with the shipped
// reader, then write the sentinel carrying that reader's verdict, and only THEN write the `closed:`
// line. A sentinel written before the verification would say what we intended rather than what is on
// disk; a `closed:` line written before the sentinel invites the harness to kill this process in the
// window between them, which leaves the launch's one artefact missing (AUDIT_D_SC7 D-5).
// `causes` is every cause that asked for this close; `reason` is the one that takes the word. They
// differ only when two triggers land in the same worker pass, and the loser reached no
// document before -- only a Warn line, which the sentinel's audience does not read.
void StopCapture(StopReason reason, uint8_t causes) {
    // â˜… AUDIT_M6 F1. THE INVARIANT, kept here because here is the only place it can be: EVERY close
    // of EVERY file runs this whole path -- drain, close, re-open and verify with the shipped reader,
    // write the per-close sentinel, and only then announce `closed:`. There is no early return above
    // this line, and there must never be one: the early return that used to sit here still called
    // DrainAndClose() with announce=true, so it wrote the `closed:` line the harness kills the
    // process on with NO sentinel behind it, no verification and no `closeSeq`, while the log named
    // the PREVIOUS capture. A launch whose first close was a deadline, a stop request or a shutdown
    // then recorded every later match with no evidence that it had closed at all.
    //
    // The `stopFired` latch was for a world where the launch had ONE sentinel to protect. Per-close
    // names made overwriting impossible by construction, so the latch protected
    // nothing and silenced everything after the first close. What still needs a one-shot is the
    // stop-REQUEST poll -- `stop-capture` stays on disk, so without one it would ask for a close
    // every second for the rest of the launch -- and that one-shot is `g.stopRequestSeen`, which the
    // worker owns and which decides nothing about sentinels.
#if defined(W2A_MUTATE_STOP_LATCH_SILENCES)
    // Mutation control: the pre-fix shape -- a launch-scoped latch, so every close
    // after the first announces itself with no sentinel behind it.
    static bool mutantStopFired = false;
    if (mutantStopFired) {
        DrainAndClose();
        return;
    }
    if (reason != StopReason::MatchEnd && reason != StopReason::CostKill) {
        mutantStopFired = true;
    }
#endif

    const bool hadFile = g.fileOpen;
    const bool live = g.inGame.load(std::memory_order_acquire);
    // â˜… AUDIT_M5 F1, second order. ONE rule, whatever the cause: a close that leaves a match still
    // being played stops a SECOND file being opened for that same match (one file per match), and
    // stops nothing beyond it -- the latch is cleared the moment the match ends (see the !inGame arm
    // of the tick). So no cause, the cost meter's kill included, can end capture for the session;
    // always-on capture is locked constraint 6. What was undeclared before is that it happens at all,
    // so it is now a fact in the sentinel and a line in the log rather than a silent side effect.
    const bool suppressReopen = hadFile && live;
    DrainAndClose(false);

    StopFacts facts{};
    facts.costTail = CostTailNow();
    facts.launchFileTime = g.launchFileTime;
    facts.pid = ::GetCurrentProcessId();
    facts.file = g.lastClosedPath;
    facts.records = g.lastRecords;
    facts.chapters = g.lastChapters;
    facts.reason = reason;
    facts.markers = g.closedMarkers;
    facts.markerSource = static_cast<uint8_t>(g.closedSource);
    facts.localBoundaries = g.closedLocal.localBoundaries;
    facts.corroboratorAsked = g.closedCorroboratorAsked;
    facts.wireRowsSeen = g.closedWireRows;
    // â˜… AUDIT_L2 F2. A close that closed nothing does not get to wear the previous close's facts.
    // The reason word says what this event was; `closeCauses` still names the request or deadline
    // that arrived, so nothing is lost, and the counts are zeroed rather than copied from a file this
    // stop did not touch. `fileClosed` stays the field it always was and now agrees with `reason`
    // instead of contradicting it.
    if (!hadFile) {
        facts.reason = StopReason::NoOp;
        facts.file = L"";
        facts.records = 0;
        facts.chapters = 0;
        // Same rule as the counts: a stop that closed no file did not derive these either, and a
        // watch-table denominator copied out of the previous capture would read as this one's.
        facts.markers = MarkerDeriver::Stats{};
        facts.markerSource = static_cast<uint8_t>(MarkerSource::NotRecorded);
        facts.localBoundaries = 0;
        facts.corroboratorAsked = false;
        facts.wireRowsSeen = 0;
    }
    facts.closeSeq = g.closeSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    facts.closeCauses = causes;
    facts.workerJoin = "n/a";
    facts.matchStillLive = live;
    facts.reopenSuppressed = suppressReopen;
    // â˜… AUDIT_M6 F5. A close that had no file open still leaves a sentinel -- a deadline that met no
    // recording is a fact the harness needs -- but it names the PREVIOUS capture in `file`, which is
    // the one document a reader could mistake for a second close of that capture. It says which it
    // is instead. With M6-F1's invariant above, `fileClosed: true` documents stand one per .echodemo
    // the launch produced, and no sentinel is ever skipped, so a missing one means a missing close.
    facts.fileClosed = hadFile;
    // â˜… AUDIT_M6 F2. The degraded state, in the document rather than only in a log nobody kept: how
    // many consecutive open attempts failed and which stage refused. Zero and "none" on a launch
    // that opened everything it tried to.
    facts.openFailures = g.openFailStreak;
    // The streak alone said "none" about a launch that failed thirty opens and then
    // recovered, because a success clears it. The launch total never clears.
#if defined(W2A_MUTATE_OPENFAILTOTAL_IS_STREAK)
    // Mutation control: the pre-fix shape -- the document carries the streak twice, so
    // a launch that failed and recovered is byte-identical to one that never failed.
    facts.openFailTotal = g.openFailStreak;
#else
    facts.openFailTotal = g.openFailTotal;
#endif
    facts.openFailState = OpenFailStateName(g.openFailKind);
    facts.taps = TapRegistrationNow();
    if (hadFile) {
        VerifyClosedFile(g.lastClosedPath, &facts.verdict);
    } else {
        // Re-verifying the PREVIOUS capture here is what produced `verified: true` on a document about
        // a close that did not happen. There is no file to verify, and the detail says that rather
        // than leaving a reader to infer it from a zero.
        facts.verdict = ReopenVerdict{};
        strcpy_s(facts.verdict.detail, sizeof facts.verdict.detail,
                 "nothing was recording when this stop arrived: no file was closed, so there was "
                 "nothing to re-open. See closeCauses for what asked");
    }

    // The sentinel goes in the LAUNCH directories -- the same ones `stop-capture` is polled in, both
    // logged at Init -- not beside the capture. The capture directory is one fixed place every launch
    // shares (D-4); a launch directory is the harness's own, and it is where a harness that asked for
    // the stop is already looking. Both are written when both resolved: the plugin does not control
    // its CWD, so it does not get to decide which of the two the harness considers the launch's.
    facts.netvarRecords = g.builder2.Stats().netvarRecords;
    // The same four numbers the stop line prints, on the document a harness can read without a log.
    facts.oracleSamples = g_oracleSamples;
    facts.oracleOk = g_oracleOk;
    facts.oracleDropped = g.pushFailByKind[kRecOracle].load(std::memory_order_relaxed);
    facts.oracleStatus = TapStatusName(g_oracleLastStatus);
    facts.oracleNotBound = g_oracleBindWhy[0] ? g_oracleBindWhy : "";
    // Taken from the builder that OWNS the table rather than from a copy
    // kept beside it, and marked recorded only where CloseFile actually filed it -- see
    // g.roundRowsRecorded, which is set at the SetRoundTable call site and nowhere else.
    facts.roundRowCount = static_cast<uint32_t>(g.builder2.RoundRowCount());
    facts.roundRowsRecorded = g.roundRowsRecorded;
    // A boundary the writer refused because its number would have broken
    // the reader ascent invariant is a round that happened and has no row: roundRowCount alone
    // cannot show it, because a refused boundary and a match with fewer rounds produce the same
    // count. Until now the two numbers existed only for a debugger.
    facts.roundsRefusedNonAscending = g.builder2.LocalMarkerStats().refusedNonAscending;
    facts.roundsRebasedAfterReset = g.builder2.LocalMarkerStats().rebasedAfterReset;
    if (gGameState.haveShadow) {
        facts.phase = gamestate::PhaseName(static_cast<gamestate::Phase>(gGameState.shadow.phase));
        facts.roundCounter = gGameState.shadow.roundCounter;
        facts.roundCounterRead = true;
    }
    facts.rosterRows = g.roster.Count();
    facts.rosterLocalRows = LocalRosterRows();
    facts.rosterBandRefusals = g.rosterBandRefusals;
    facts.rosterUnchanged = g.rosterScansUnchanged;
    facts.rosterSignalUnchanged = g.rosterSignalUnchanged;
    facts.rosterBandClamped = g.rosterBandClamped;
    facts.wireMarksHeldDropped = g.builder2.Stats().wireMarksHeldDropped;
    facts.boundaryMarksNotCut = g.builder2.Stats().boundaryMarksNotCut;
    facts.rosterRefusals = g.rosterRefusals;
    // The producer's own counters, for the reason the roster's are here: a run whose
    // log shows no census line must be able to say WHICH of the five reasons that was, and a count
    // nobody reads is the two-state gate the BRIEF forbids.
    facts.identityBlocks = g.identityBlocks;
    facts.identityJsonRefused = g.identityJsonRefused;
    facts.identityKeyless = g.identityKeyless;
    facts.identityTruncated = g.identityTruncated;
    facts.identityTruncationUnknown = g.identityTruncationUnknown;
    facts.identityDeferred = g.identityDeferred;
    facts.identityLastJsonWhy = g.identityLastJsonWhy;
    for (size_t i = 0; i < kIdentityEmitRefusalCount; ++i) {
        facts.identityRefusals[i] = g.identityRefusals[i];
    }
    // ARCH-21 MAJOR-B2. The scoreboard's own counters, for the reason `taps` and `markers` are here:
    // a harness that never opens the file must be able to tell "nobody scored" from "the stat block
    // was never readable", and `records 0` alone says neither.
    facts.statsRecords = g.statsRecords;
    facts.statsKeyframes = g.statsKeyframes;
    facts.statsSweeps = g.statsSweeps;
    facts.statsPlayers = PlayerStatsUsers();
    facts.statsSignalUnchanged = g.statsSignalUnchanged;
    facts.statsSourceAbsent = g.statsSourceAbsent;
    facts.statsRefusals = g.statsRefusals;
    facts.bodiesSampled = g.bodiesSampledTotal;
    facts.bodiesRefused = g.bodiesRefusedTotal;
    facts.bodiesLocal = g.bodiesFromLocalTotal;
    facts.bodiesHistory = g.bodiesFromHistoryTotal;
    facts.bodiesNone = g.bodiesSourceNoneTotal;
    facts.bodiesNotAsked = g.bodiesSourceNotAskedTotal;
    for (unsigned i = 0; i < kAngVelSourceCount; ++i) {
        facts.angVelBy[i] = g.angVelByTotal[i];
    }
    facts.angVelUnknown = g.angVelUnknownTotal;
    for (unsigned i = 0; i < kBodyRefusalCount; ++i) {
        facts.bodyRefusals[i] = g.bodyRefusalTotals[i];
    }
    facts.bodyRefusalUnknown = g.bodyRefusalUnknownTotal;
    facts.bodyFirstRefusalSeen = g.bodyFirstRefusalSeen;
    facts.bodyFirstRefusal = g.bodyFirstRefusal;
    facts.encodeRefusalsNamed = g.closedRefusalsNamed;
    for (uint32_t i = 0; i < g.closedRefusalsNamed; ++i) {
        facts.encodeRefusalNotes[i] = g.closedRefusalNotes[i];
    }
    facts.demoUuidLo = g.lastUuid.lo;
    facts.demoUuidHi = g.lastUuid.hi;

    // W1-F R3's second report occasion. HERE rather than in the loop below because a launch
    // with two watch directories services ONE close, and a per-directory call reports two.
    inbound::NoteCaptureCloseServiced();

    char err[192] = {};
    char firstErr[192] = {};
    wchar_t firstErrDir[kPathMax] = {};
    uint32_t wroteCount = 0;
    uint32_t pointerCount = 0;
    uint32_t archivedCount = 0;
    for (uint32_t i = 0; i < g.stopWatchCount; ++i) {
        // The per-close document FIRST: it is the evidence, it is never replaced, and it is what the
        // invariant "every closed file has exactly one sentinel that survives every later close"
        // rests on. The launch-scoped name below is a pointer at the latest close for
        // readers that poll one path; a failure to refresh it loses nothing but currency.
        if (WriteStopSentinelForClose(g.stopWatchDirs[i], facts, err, sizeof(err))) {
            ++wroteCount;
        } else if (firstErr[0] == 0) {
            strcpy_s(firstErr, sizeof(firstErr), err);
            wcscpy_s(firstErrDir, kPathMax, g.stopWatchDirs[i]);
        }
        char pointerErr[192] = {};
        if (WriteStopSentinel(g.stopWatchDirs[i], facts, pointerErr, sizeof(pointerErr))) {
            ++pointerCount;
        } else if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "the latest-close sentinel pointer could not be refreshed in %ls: %s - the "
                         "per-close sentinel for this close is still the record",
                         g.stopWatchDirs[i], pointerErr);
        }
        // G-8: the swept path is the reader's; this copy is the one that survives the next launch's
        // Init and can still be collected into the run directory afterwards.
        char archiveErr[192] = {};
        if (ArchiveStopSentinel(g.stopWatchDirs[i], facts, archiveErr, sizeof(archiveErr))) {
            ++archivedCount;
        } else if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "the stop sentinel could not be archived under %ls\\%ls: %s",
                         g.stopWatchDirs[i], kStopArchiveDirName, archiveErr);
        }
    }
    if (g.stopWatchCount == 0) {
        // No launch directory resolved at Init (Init said so, at Warn). The capture's own directory is
        // the only place left; a sentinel somewhere is worth more than no sentinel at all, and its
        // name still carries the launch it belongs to.
        wchar_t dir[kPathMax];
        if (!DirectoryOfFile(g.lastClosedPath, dir, kPathMax)) {
            wcscpy_s(dir, kPathMax, g.replayDir);
        }
        if (WriteStopSentinelForClose(dir, facts, err, sizeof(err))) {
            ++wroteCount;
        } else if (firstErr[0] == 0) {
            strcpy_s(firstErr, sizeof(firstErr), err);
            wcscpy_s(firstErrDir, kPathMax, dir);
        }
        char pointerErr[192] = {};
        if (WriteStopSentinel(dir, facts, pointerErr, sizeof(pointerErr))) {
            ++pointerCount;
        }
        char archiveErr[192] = {};
        if (ArchiveStopSentinel(dir, facts, archiveErr, sizeof(archiveErr))) {
            ++archivedCount;
        } else if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "the stop sentinel could not be archived under %ls\\%ls: %s", dir,
                         kStopArchiveDirName, archiveErr);
        }
    }
    const bool wrote = wroteCount != 0;

    wchar_t sentinelName[64] = {};
    StopSentinelNameForClose(facts.launchFileTime, facts.pid, facts.closeSeq, sentinelName, 64);

    if (g.log) {
        // The `closed:` line is the writer's account. This one is the reader's, and the two are
        // reported separately on purpose.
        g.log->Write(facts.verdict.verified ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
                     "stop (%s): %ls verified=%s, %llu bytes, %u chapters - %s",
                     StopReasonName(facts.reason),
                     hadFile && g.lastClosedPath[0] ? g.lastClosedPath
                                                    : L"(nothing was recording)",
                     facts.verdict.verified ? "true" : "false",
                     static_cast<unsigned long long>(facts.verdict.bytes), facts.verdict.chapters,
                     facts.verdict.detail);
        if (wrote) {
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "stop sentinel %ls (close %u of this launch) written into %u launch "
                         "director%s, archived in %u, latest-close pointer refreshed in %u",
                         sentinelName, facts.closeSeq, wroteCount, wroteCount == 1 ? "y" : "ies",
                         archivedCount, pointerCount);
            if (firstErr[0]) {
                // G-9: a partial write used to be reported only as a count. The harness that polls
                // the directory that failed sees nothing there and would file CAPTURE-UNFINALIZED
                // with no line saying why, so the directory is named even when another one worked.
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "but the stop sentinel could NOT be written into %ls: %s - a reader "
                             "polling that directory will see no sentinel for this launch",
                             firstErrDir, firstErr);
            }
        } else {
            // A stop nobody can read about is the silence C6 forbids, so the failure is louder than
            // the stop itself.
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "the stop sentinel %ls could not be written into %ls: %s", sentinelName,
                         firstErrDir, firstErr);
        }
        if (suppressReopen) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "the match was still live at this %s close: capture stays on, but this "
                         "match gets no second file (one file per match). The next match opens a "
                         "new one",
                         StopReasonName(reason));
        }
        if (reason == StopReason::CostKill) {
            // The one cause that ends a live match's capture on its own account rather than because
            // the match ended. It must never be inferable only from a `reason` field nobody printed
            // Whether or not it suppressed a reopen, it says which it did.
            // ...and it says what it MEASURED. L5 re-run #4 killed a live match and left no number
            // in the log, the sentinel or the file, so the threshold could not be argued with.
            const CostMeter::WindowStat cw = g.cost.LastWindow();
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "the capture cost meter reached its kill rung and closed this file; %s. "
                         "Capture is NOT disarmed for the launch. The last window of %u tick(s) "
                         "measured median in (%u, %u] us, p99 in (%u, %u] us, max %u us (exact); "
                         "the kill needs a median provably over %u us on %u consecutive window(s) "
                         "and had %u; the TAIL rung needs a p99 provably over %u us on the same "
                         "%u window(s) and had %u, read over %u charged sample(s) of this window "
                         "(the tail rung was %s here; %u window(s) this launch carried too few "
                         "charged samples for p99 to be a tail at all and were NOT-RUN -- neither "
                         "pass nor fail), and needs this window's charged mean over %u us "
                         "as well -- it was %u us, and the tail read %sover its own line "
                         "here) "
                         "(window %llu of this launch). Those are CHARGED figures -- "
                         "capture-only tick work. The plugin's always-on tick work over the same "
                         "window cost %u us mean / %u us max and is deliberately not in them: it "
                         "runs whether or not a capture is open and no capture can lower it",
                         suppressReopen
                             ? "the match now being played gets no further capture"
                             : "no match was in progress, so nothing further was suppressed",
                         CostMeter::kWindowTicks, cw.medianLoUs, cw.medianUs, cw.p99LoUs, cw.p99Us,
                         cw.maxUs,
                         CostMeter::kKillMedianUs, CostMeter::kKillWindows, cw.overKillWindows,
                         CostMeter::kKillTailUs, CostMeter::kKillWindows, cw.overKillTailWindows,
                         cw.chargedSamples, cw.tailMeasured ? "asked" : "NOT-RUN",
                         cw.tailNotRunWindows, CostMeter::kKillTailMeanUs, cw.chargedMeanUs,
                         cw.tailOverBudget ? "" : "not ",
                         static_cast<unsigned long long>(cw.windows), cw.unchargedMeanUs,
                         cw.unchargedMaxUs);
        }
    }
    if (suppressReopen) {
        g.suppressReopen.store(true, std::memory_order_release);
    }
    // LAST, and only now: the harness may kill this process the instant it reads this line, and by
    // now the sentinel it is about is on disk (D-5).
    LogClosedLine();
}

// The worker half of the phase breakdown: one line per complete 512-tick window, off the tick thread.
void LogPhaseWindow() {
    const uint64_t seq = g.phaseSeq.load(std::memory_order_acquire);
    if (seq == g.phaseLogged || !g.log) {
        return;
    }
    g.phaseLogged = seq;
    const PhaseWindow w = g.phaseDone;
    const double per = g.cost.TscPerUs();
    if (w.ticks == 0 || !(per > 0.0)) {
        return;
    }
    // Sized FROM the phase count, so a phase added without widening this does not silently
    // truncate the phase the next kill line is about.
    char line[kPhaseCount * 40 + 32] = {};
    char gatherLine[kGatherPartCount * 40 + 32] = {};
    uint64_t chargedTsc = 0;
    for (uint32_t i = 0; i < kPhaseCount; ++i) {
        if (i != kPhaseGate) {
            chargedTsc += w.tsc[i];
        }
    }
    for (uint32_t i = 0; i < kPhaseCount; ++i) {
        const size_t used = strlen(line);
        _snprintf_s(line + used, sizeof line - used, _TRUNCATE, "%s%s %.1f/%u", used ? ", " : "",
                    kTickPhaseName[i], double(w.tsc[i]) / per / double(w.ticks), w.maxUs[i]);
    }
    for (uint32_t i = 0; i < kGatherPartCount; ++i) {
        const size_t used = strlen(gatherLine);
        _snprintf_s(gatherLine + used, sizeof gatherLine - used, _TRUNCATE, "%s%s %.1f",
                    used ? ", " : "", kGatherPartName[i],
                    double(w.gatherTsc[i]) / per / double(w.ticks));
    }
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "tick cost by phase, mean us/max us over the last %u tick(s), %u of them with a "
                 "capture open: %s; charged %.1f us "
                 "mean (everything after `gate` -- work that exists because a capture is open, and "
                 "the only thing the kill rung reads, and a mean over the OPEN ticks alone, so a "
                 "window with none of them charges nothing), uncharged %.1f us mean (`gate`, runs "
                 "on every engine frame whether or not a capture is open). A kill line quoting a "
                 "median the CHARGED total does not add up to is measuring something outside these "
                 "six brackets. `gather` splits into %s (same denominator, so the two add up to "
                 "it) and cost %.2f engine span probe(s) per OPEN tick -- a probe is a "
                 "VirtualQuery, and a REFUSED span is never cached, so this number rising with "
                 "nothing else moving means the gather is asking the OS about slots that are not "
                 "there",
                 w.ticks, w.openTicks, line,
                 w.openTicks ? double(chargedTsc) / per / double(w.openTicks) : 0.0,
                 double(w.tsc[kPhaseGate]) / per / double(w.ticks), gatherLine,
                 w.openTicks ? double(w.gatherProbes) / double(w.openTicks) : 0.0);
}

DWORD WINAPI WorkerMain(LPVOID) {
    g.scratch.assign(kWorkerScratchBytes, 0);
    while (!g.quit.load(std::memory_order_acquire)) {
        LogPhaseWindow();
        if (g.openRequested.exchange(false, std::memory_order_acq_rel)) {
            // â˜… AUDIT_M6 F2. The tick re-arms this request on the very next frame after a failure,
            // so the bound belongs here: inside the backoff window the request is dropped without a
            // filesystem call and without a line. The phase still returns to Idle, which is what
            // lets the tick keep the request alive until the window closes.
            if (g.openRetryAtMs != 0 && GetTickCount64() < g.openRetryAtMs) {
                SetPhase(Phase::Idle);
            } else if (OpenFile()) {
                if (g.openFailStreak && g.log) {
                    g.log->Write(LogLevel::Info, kReplayModuleName,
                                 "the capture opened after %u consecutive failure(s) (%s); "
                                 "%u failed open(s) so far this launch",
                                 g.openFailStreak, OpenFailStateName(g.openFailKind),
                                 g.openFailTotal);
                }
                g.openFailKind = OpenFailKind::None;
                g.openFailStreak = 0;
                g.openBackoffMs = 0;
                g.openRetryAtMs = 0;
                SetPhase(Phase::Recording);
            } else {
                SetPhase(Phase::Idle);
            }
        }

        const uint32_t drained =
            g.rings.Drain(g.scratch.data(), kWorkerScratchBytes, &SinkRecord, nullptr,
                          kWorkerDrainPerPass);

        // The ring refused records. They were observed and lost downstream, which is a DROPPED record
        // naming the count -- E-C10's shape, and the difference between a file that is honest about a
        // gap and one that has an unexplained hole.
        //
        // Charged to the stream that actually lost them. The first version charged every ring drop to
        // S_TICK because the ring reports one number and does not know what it refused; the totals
        // were right and a body loss read as a spine loss. The producer knows the kind at the moment
        // it drops, so it counts there and the worker only moves the counters into the file.
        if (g.fileOpen) {
            for (uint16_t kind = 1; kind <= kRecKindMax; ++kind) {
                const uint32_t n = g.pushFailByKind[kind].exchange(0, std::memory_order_acq_rel);
                if (n) {
                    g.builder2.NoteDropped(StreamOfKind(kind), g.builder2.LastSourceTimeUs(),
                                           kGapRingFull, n);
                }
            }
        }

        if (g.costInjectWindows.load(std::memory_order_acquire) != 0) {
            g.costInjectWindows.fetch_sub(1, std::memory_order_acq_rel);
            for (uint32_t i = 0; i < CostMeter::kWindowTicks; ++i) {
                g.cost.AddSampleUs(kCostInjectSlowUs);
            }
        }

        CostMeter::Rung rung = CostMeter::Rung::Nominal;
        if (g.cost.Evaluate(rung) && g.fileOpen) {
            g.builder2.NoteDegraded(g.builder2.LastSourceTimeUs(), 0,
                                    rung == CostMeter::Rung::OracleCadenceReduced
                                        ? kDegradeOracleCadence
                                        : (rung == CostMeter::Rung::Killed ? kDegradeKillSwitch
                                                                           : kDegradeKeyframeInterval),
                                    g.cost.KeyframeIntervalUs());
            g.builder2.SetKeyframeIntervalUs(g.cost.KeyframeIntervalUs());
            if (rung == CostMeter::Rung::Killed) {
                // S6.4 rung 5: stop, flag, close cleanly at the last complete chapter. The match is
                // still being played, so this is NOT a match end and must not be reported as one
                // -- the cause travels with the request.
#if !defined(W2A_MUTATE_COSTKILL_NO_BIT)
                // Mutation control: without this the meter reaches rung 5 and asks for
                // nothing, so no close is ever filed as a cost kill and no document names one.
                g.closeCauses.fetch_or(CloseCauseBit(StopReason::CostKill),
                                       std::memory_order_release);
#endif
            }
        }

        // C6's first close trigger. One shot, and it asks for exactly the close a match ending asks
        // for: it does not disarm capture, because always-on capture is a locked constraint and a
        // deadline whose job is to give the launcher a close signal must not quietly turn it off.
        // â˜… AUDIT_M6 F1. Guarded by its own one-shot and nothing else. It used to be guarded by the
        // launch-wide `stopFired` as well, which meant a stop request cancelled the deadline; now
        // that every close leaves its own evidence there is nothing to protect by cancelling it, and
        // a deadline that fires still produces the `closed:` line a launcher may be waiting for.
        const uint64_t deadlineAt = g.deadlineAtMs.load(std::memory_order_relaxed);
        if (deadlineAt != 0 && !g.deadlineFired && GetTickCount64() >= deadlineAt) {
            g.deadlineFired = true;
            const bool wasRecording = g.fileOpen;
            if (wasRecording) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "the %u s capture deadline was reached; closing the open capture",
                             unsigned(g.deadlineSecs));
            }
            // Either way the stop runs: with a file it closes and verifies it, without one it still
            // has the previous match's file to report on, and the sentinel is written in both cases.
            StopCapture(StopReason::Deadline, CloseCauseBit(StopReason::Deadline));
            if (!wasRecording) {
                // The launcher waits for a `closed:` line before it kills the process, and CloseFile
                // writes that line only when there was a file. A deadline that met no recording has
                // to produce one itself, saying so, or the wait times out and reads as a hang. It
                // comes AFTER the stop for the same reason every other `closed:` does (D-5).
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "closed: nothing was recording when the %u s capture deadline was "
                             "reached",
                             unsigned(g.deadlineSecs));
            }
        }

        // C6's second trigger, independent of the deadline: a file the harness creates. Polled once a
        // second rather than every pass -- this is a filesystem call, and the worker's idle pass is
        // measured in milliseconds.
        if (!g.stopRequestSeen && g.stopWatchCount) {
            const uint64_t now = GetTickCount64();
            if (now >= g.nextStopPollMs) {
                g.nextStopPollMs = now + 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
                for (uint32_t i = 0; i < g.stopWatchCount; ++i) {
                    if (StopRequestPresent(g.stopWatchDirs[i], g.launchFileTime)) {
                        g.log->Write(LogLevel::Info, kReplayModuleName,
                                     "a %ls request appeared in %ls; closing the capture",
                                     kStopRequestName, g.stopWatchDirs[i]);
                        // The request file is not deleted -- it is the harness's, and a launch does
                        // not get to erase the record of what it was asked to do -- so the poll is
                        // one-shot. Capture stays armed: a later match still opens, records and
                        // closes with a sentinel of its own.
                        g.stopRequestSeen = true;
                        StopCapture(StopReason::Request, CloseCauseBit(StopReason::Request));
                        break;
                    }
                }
            }
        }

        if (const uint8_t causes = g.closeCauses.exchange(0, std::memory_order_acq_rel)) {
            // â˜… AUDIT_L5_RUN3 M5. This is the ORDINARY close -- the match ended and nobody asked for
            // anything -- and it used to write no sentinel, so a launch that recorded a whole match
            // was reported as CAPTURE-NOT-ASKED. It goes through the same path a requested stop does:
            // close, re-open and verify, then write the sentinel with the reason and the registration
            // accounting, then say `closed:`.
            //
            // â˜… AUDIT_M5 F1. The reason is READ here, not decided here. Two triggers set this mask
            // and only the setter knows which one it was; deriving `match-end` at the close made the
            // cost meter's kill switch report a cause that did not happen.
            //
            // â˜… AUDIT_M6 F3. Both causes can be true of the same close. The cost kill is the one
            // that names itself in the sentinel and the one whose Error line is the owner's only
            // report of the meter, so it takes the word; the match end that also asked is not
            // dropped in silence, it is said here. `matchStillLive` still separates them in the
            // document.
            const bool costKill = (causes & CloseCauseBit(StopReason::CostKill)) != 0;
            const bool matchEnd = (causes & CloseCauseBit(StopReason::MatchEnd)) != 0;
            if (costKill && matchEnd && g.log) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "the cost meter's kill and the match ending asked for the same close; "
                             "it is recorded as cost-kill and the match had also ended");
            }
            const bool asked = (causes & CloseCauseBit(StopReason::Request)) != 0;
            StopCapture(costKill  ? StopReason::CostKill
                        : matchEnd ? StopReason::MatchEnd
                                   : asked ? StopReason::Request
                                           : StopReason::MatchEnd,
                        causes);
            SetPhase(Phase::Idle);
        }

        if (drained == 0) {
            Sleep(kWorkerIdleMs);
        }
    }
    // The worker is exiting, so this is the last chance any file has to be closed -- and the last
    // chance a stop has to be reported. `taskkill /F` reaches neither (F8); a clean unload reaches
    // both.
    StopCapture(StopReason::Shutdown, CloseCauseBit(StopReason::Shutdown));
    return 0;
}

// ---------------------------------------------------------------------------------------------

// How many times one message type may be refused before the capture stops asking. A refusal costs a
// contended-lock deadline on the game thread, so this is a small number and the give-up is loud: a
// stream missing because we gave up must never look like a stream nothing was sent on.
constexpr uint8_t  kTapAttempts = 5;
constexpr uint16_t kMaxTaps = 64;   // sizes g.taps, g.tapAttempts, g.tapDone and g_tapCtx

// The counts, read off the state the tick owns. TICK THREAD ONLY -- every field it touches is written
// by RegisterSurfaces below. Anything else calls TapRegistrationNow(), which reads the snapshot this
// one is published into.
TapRegistration ComputeTapRegistration() {
    const uint16_t n = TapTypeCount();
    uint32_t unsettled = 0, unreachable = 0;
    for (uint16_t i = 0; i < n; ++i) {
        const TapType& t = TapTable()[i];
        if (t.stream == kStreamNetvar || TapRowExcludedByDesign(t)) {
            continue;   // the netvar listener is counted below; excluded rows are the table's shape
        }
        if (i >= kMaxTaps) {
            // Past the handle table, which RegisterTaps says at Error the first time it sees the
            // table. Nothing can ever register it, so it is given up on rather than forever deferred.
            ++unreachable;
            continue;
        }
        if (!g.tapDone[i]) {
            ++unsettled;
        }
    }
    // An unsettled type on a TERMINAL path is not deferred. `deferred` promises that
    // something is still trying; when the tap answered Available() false nothing is, and a consumer
    // waiting for that count to drain waits for the process to exit.
#if defined(W2A_MUTATE_TERMINAL_AS_DEFERRED)
    // Mutation control: the pre-M1 shape -- a dead surface reported as still being tried.
    const bool tapTerminal = false;
#else
    const bool tapTerminal = g.tapUnavailable;
#endif
    const bool netvarUnavailable = g.netvarDone && g.netvarStatus == netvar::Status::TapUnavailable;

    // total, excluded and tableRows are the table's shape and are filled by TapRegistrationNow, which
    // is the one definition of them; this half publishes only what the tick knows.
    TapRegistration r{};
    r.registered = uint32_t(g.tapCount) + (g.netvarRegistered ? 1u : 0u);
    r.givenUp = uint32_t(g.tapsAbandoned) + unreachable +
                ((g.netvarDone && !g.netvarRegistered && !netvarUnavailable) ? 1u : 0u);
    r.unavailable = (tapTerminal ? unsettled : 0u) + (netvarUnavailable ? 1u : 0u);
    r.deferred = (tapTerminal ? 0u : unsettled) + (g.netvarDone ? 0u : 1u);
#if defined(W2A_MUTATE_STARTED_FROM_TAP_POINTER)
    // Mutation control: the pre-M2 shape -- `started` reads the tap POINTER, so a tap that asked
    // nothing at all reports registration as attempted.
    r.started = (g.tap != nullptr);
#else
    r.started = g.tapListenAttempted;
#endif
#if defined(W2A_MUTATE_COMPUTE_PUBLISHES)
    // Mutation control: computing publishes, so the answer a cross-thread reader gets
    // moves with the tick's own globals instead of only at a publish. That is the M3 defect's shape.
    PublishTapRegistration(r);
#endif
    return r;
}

// Whether a handle is held for table row `i`. The handle contexts carry the row index, so the
// registration itself answers -- `tapDone` is also set on a give-up and on the rows nobody asks for.
bool RowRegistered(uint16_t i) {
    for (uint16_t k = 0; k < g.tapCount && k < kMaxTaps; ++k) {
        if (g_tapCtx[k].typeIndex == i) {
            return true;
        }
    }
    return false;
}

// â˜… L5 run 3 F8: the summary read "36 of 38 message taps registered" and left two rows unaccounted
// and unnamed, so nothing could tell a row left out ON PURPOSE from one that was dropped. Every row
// of the table that is not registered is named here, with the state that made it so.
//
// â˜… AUDIT_M5 F8-buf. The list used to go into one fixed 320-byte buffer, so the state where naming
// every row matters most -- nothing registered, 36 rows to name -- was the state the buffer could not
// carry, and it degraded to about ten names and `(+26 more)`. Growing the buffer does not fix it:
// ILog::Write truncates the WHOLE line at kLogLineMax and says nothing when it does, so the ceiling
// is the log line, not the buffer. The size comes from that ceiling and the list is emitted over as
// many lines as the table needs. `*from` is the row to resume at, advanced past the last row written;
// false means the table is exhausted, so a caller loops on it.
bool DescribeUnregisteredRows(char* buf, size_t cap, uint16_t* from) {
    if (!buf || cap == 0 || !from) {
        return false;
    }
    buf[0] = '\0';
    size_t at = 0;
    const uint16_t n = TapTypeCount();
    for (; *from < n; ++*from) {
        const uint16_t i = *from;
        const TapType& t = TapTable()[i];
        const char* state = nullptr;
        if (TapRowExcludedByDesign(t)) {
            state = "excluded-by-design(ours, never a wire message)";
        } else if (t.stream == kStreamNetvar) {
            if (g.netvarRegistered) {
                continue;
            }
            state = g.netvarStatus == netvar::Status::TapUnavailable
                        ? "unavailable(the tap is not available)"
                        : (g.netvarDone ? "givenUp(the S_NETVAR listener)" : "deferred");
        } else if (i >= kMaxTaps) {
            state = "givenUp(past the handle table)";
        } else if (RowRegistered(i)) {
            continue;
        } else if (g.tapUnavailable) {
            state = "unavailable(the tap is not available)";
        } else {
            state = g.tapDone[i] ? "givenUp(abandoned at the attempt bound)" : "deferred";
        }
        const int wrote = snprintf(buf + at, cap - at, "%s%s=%s", at ? ", " : "", t.structName, state);
        if (wrote < 0 || size_t(wrote) >= cap - at) {
            buf[at] = '\0';
            if (at == 0) {
                // Nothing fits an empty buffer, so no number of further lines would carry this row.
                // It is named by its index rather than dropped: a row a line cannot carry is still a
                // row the reader has to be told about, and `from` advances so the rest still come.
                snprintf(buf, cap, "row %u=(the name did not fit a log line)", unsigned(i));
                ++*from;
            }
            return true;
        }
        at += size_t(wrote);
    }
    return at != 0;
}

// The list, over as many log lines as the table needs. Bounded by the table: at most one line per row.
void LogUnregisteredRows() {
    if (!g.log) {
        return;
    }
    // Sized from the log's own ceiling, not guessed: the prefix below is 33 bytes, and the module and
    // level the ring adds are stored beside the text rather than in it.
    char rows[kLogLineMax - 128];
    uint16_t from = 0;
    while (DescribeUnregisteredRows(rows, sizeof rows, &from)) {
        g.log->Write(LogLevel::Info, kReplayModuleName, "tap-registration not registered: %s", rows);
    }
}

// Registration bookkeeping, back to "nothing settled, everything still to ask". Init seeds it before
// the first tick, Shutdown re-seeds it so a second Init in this process starts from the same place,
// and the test helper re-arms a walk with it. Live handles are the caller's to hand back FIRST; this
// only forgets them.
void ClearTapRegistrationState() {
    for (uint16_t i = 0; i < kMaxTaps; ++i) {
        g.taps[i] = 0;
        g.tapAttempts[i] = 0;
        g.tapDone[i] = false;
    }
    g.tapCount = 0;
#if defined(W2A_MUTATE_UNSEEDED_PENDING)
    // Mutation control: AUDIT_M4 B1's shape -- the pending count left at zero until a walk sets it,
    // so RegisterSurfaces' settled-guard reads "nothing pending" on a state where nothing was asked.
    g.tapsPending = 0;
#else
    g.tapsPending = TapTypeCount();
#endif
    g.tapsAbandoned = 0;
    g.tapStopPendingLogged = 0;
    g.tapGivenUpLogged = 0;
    g.tapUnavailable = false;
    g.tapListenAttempted = false;
    g.netvarAttempts = 0;
    g.netvarDone = false;
    g.netvarRegistered = false;
    g.netvarStatus = netvar::Status::ListenPending;
    PublishTapRegistration(ComputeTapRegistration());
}

// Whether the Listen that just refused was the one that waited out the CR15NetGame listener lock, as
// opposed to one of the cheap refusals (an unknown symbol, a call from inside a callback, a full tap
// table, an exhausted listener pool). The tap's own counter answers it: LockTimeouts() moves on exactly
// the path that spent the deadline. The alternative -- matching `wiretap-lock-timeout` inside
// Diagnostic() -- would put capture.cpp's private copy of a W1-L string on the decision path.
//
// The counter is aggregate, so this reads a delta rather than a flag, and the delta is only this call's
// because every Listen in src/ is issued from the game tick (here and netvar.cpp, both driven by
// RegisterSurfaces below). A second thread taking the tap would silently break that.
bool SpentTheLockDeadline(uint32_t timeoutsBefore) {
#if defined(W2A_MUTATE_STOP_ON_ANY_REFUSAL)
    // Mutation control: every refusal looks like a spent deadline, so the walk stops on the cheap ones
    // too and one busy pool costs kTapAttempts ticks of head-of-line block instead of nothing.
    (void)timeoutsBefore;
    return true;
#else
    return g.tap->LockTimeouts() != timeoutsBefore;
#endif
}

// Returns true if this walk spent the tick's one lock deadline.
bool RegisterTaps() {
    if (!g.tap || !g.tap->Available()) {
        // Availability is decided when the tap is built and never changes afterwards, so this one is
        // terminal -- retrying it would only reprint the same line on every frame.
        if (g.log && !g.tapUnavailable) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "the message-bus tap is unavailable (%s): S_PLAYER_WIRE, S_BODY_WIRE, "
                         "S_EVENT and S_NETVAR will be empty and the file will say so",
                         g.tap ? g.tap->Diagnostic() : "no tap");
        }
        g.tapUnavailable = true;
        g.tapsPending = 0;
        return false;
    }
    const uint16_t n = TapTypeCount();
    if (n > kMaxTaps && g.log) {
        g.log->Write(LogLevel::Error, kReplayModuleName,
                     "the tap table has %u types but only %u fit: %u types are unreachable",
                     unsigned(n), unsigned(kMaxTaps), unsigned(n - kMaxTaps));
    }
    // â˜… AUDIT_M2_P3R6_TAPRETRY_W02R7: one tick's walk may spend at most ONE lock deadline. W1-L's
    // Listen busy-spins up to 2000 us before it refuses a held CR15NetGame listener lock, and this
    // walk runs on the game thread, so a table of 36 types all refused that way costs 72 ms in one
    // frame -- and a per-tick retry pass costs it every frame. On the first Listen that reaches the
    // deadline the walk stops; the types after it are not skipped, only deferred, because the next
    // tick starts again at the first type still unsettled. A type refused here therefore waits at
    // most until the lock is free, never forever.
    bool changed = false;
    bool stoppedOnLockTimeout = false;
    for (uint16_t i = 0; i < n && i < kMaxTaps; ++i) {
        if (g.tapDone[i]) {
            continue;
        }
        const TapType& t = TapTable()[i];
        if (t.stream == kStreamNetvar) {
            g.tapDone[i] = true;   // INetVar owns it; two listeners would double every message
            continue;
        }
        if (TapRowExcludedByDesign(t)) {
            g.tapDone[i] = true;   // ours, never a wire message
            continue;
        }
        if (g.tapCount >= kMaxTaps) {
            g.tapDone[i] = true;
            ++g.tapsAbandoned;
            changed = true;
            if (g.log) {
                g.log->Write(LogLevel::Error, kReplayModuleName,
                             "tap-registration-abandoned %s after %u attempts: the tap table is "
                             "full at %u handles",
                             t.structName, unsigned(g.tapAttempts[i]), unsigned(kMaxTaps));
            }
            continue;
        }

        g_tapCtx[g.tapCount].typeIndex = i;
        g_tapCtx[g.tapCount].stream = t.stream;
        const uint32_t timeoutsBefore = g.tap->LockTimeouts();
        const uint16_t h = g.tap->Listen(t.structName, &OnTapMessage, &g_tapCtx[g.tapCount]);
        ++g.tapAttempts[i];
        g.tapListenAttempted = true;
        if (h == IWiretap::kInvalidHandle) {
            const bool spentTheDeadline = SpentTheLockDeadline(timeoutsBefore);
            // The reason is on the tap, not in the handle: without it the log says a tap was refused
            // and never which of `wiretap-lock-timeout`, an unknown symbol or a full pool it was.
            if (g.log) {
                g.log->Write(LogLevel::Warn, kReplayModuleName,
                             "tap refused for %s (%s); attempt %u of %u", t.structName,
                             g.tap->Diagnostic(), unsigned(g.tapAttempts[i]),
                             unsigned(kTapAttempts));
            }
            if (g.tapAttempts[i] >= kTapAttempts) {
                g.tapDone[i] = true;
                ++g.tapsAbandoned;
                changed = true;
                if (g.log) {
                    g.log->Write(LogLevel::Error, kReplayModuleName,
                                 "tap-registration-abandoned %s after %u attempts (%s)",
                                 t.structName, unsigned(g.tapAttempts[i]), g.tap->Diagnostic());
                }
            }
#if !defined(W2A_MUTATE_NO_TAP_EARLY_STOP)
            if (spentTheDeadline) {
                stoppedOnLockTimeout = true;
                break;
            }
#else
            (void)spentTheDeadline;
#endif
            continue;
        }
        g.taps[g.tapCount++] = h;
        g.tapDone[i] = true;
        changed = true;
    }
    // Counted after the walk rather than during it: a type the walk stopped short of is every bit as
    // pending as one it asked for and was refused, and only a count of what is still unsettled brings
    // the deferred tail back on the next tick.
    uint16_t pending = 0;
    for (uint16_t i = 0; i < n && i < kMaxTaps; ++i) {
        if (!g.tapDone[i]) {
            ++pending;
        }
    }
    g.tapsPending = pending;
    if (changed && g.log) {
        PublishTapRegistration(ComputeTapRegistration());
        const TapRegistration r = TapRegistrationNow();
        // One vocabulary. These are the TapRegistration struct's field names, the same
        // words the keyframe block and the stop sentinel write.
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "tap-registration: %u registered, %u deferred, %u givenUp, %u unavailable, "
                     "%u excluded = %u of %u tap-table rows",
                     unsigned(r.registered), unsigned(r.deferred), unsigned(r.givenUp),
                     unsigned(r.unavailable), unsigned(r.excluded),
                     unsigned(r.registered + r.deferred + r.givenUp + r.unavailable + r.excluded),
                     unsigned(r.tableRows));
        LogUnregisteredRows();
    }
    return stoppedOnLockTimeout;
}

// The S_NETVAR listener, under the same discipline as the tap walk: bounded attempts, a loud give-up,
// and at most one lock deadline. INetVar owns the handle -- registering the symbol in the walk as well
// would double every message -- so the attempt goes through netvar::TryListen.
//
// Returns true if this call spent the tick's one lock deadline.
bool RegisterNetvarListener() {
    if (!g.tap || !g.netvar || g.netvarDone) {
        return false;
    }
    const uint32_t timeoutsBefore = g.tap->LockTimeouts();
    const netvar::Status s = netvar::TryListen(g.netvar);
    if (s == netvar::Status::Ok || s == netvar::Status::ListenRefused) {
        // The only two answers a Listen was actually issued for: NoWiretap and TapUnavailable are
        // decided before the call (netvar.cpp TryListen), and reporting registration as attempted on
        // those is AUDIT_M4 M2.
        g.tapListenAttempted = true;
    }
    if (s != netvar::Status::ListenRefused) {
        // Ok, or a refusal no later tick can change (no tap, or a tap that never came up).
        g.netvarDone = true;
        g.netvarStatus = s;
        g.netvarRegistered = (s == netvar::Status::Ok);
        if (g.log) {
            g.log->Write(g.netvarRegistered ? LogLevel::Info : LogLevel::Error, kReplayModuleName,
                         "the S_NETVAR listener %s (%s)",
                         g.netvarRegistered ? "registered" : "will never register",
                         netvar::StatusText(s));
        }
        return false;
    }
    ++g.netvarAttempts;
#if defined(W2A_MUTATE_NETVAR_NO_RETRY)
    // Mutation control: the shape AUDIT_M3 F2 rejected -- one Listen, issued from netvar::Create on the
    // state-9 tick, and no second chance -- so one contended frame empties S_NETVAR for the match.
    g.netvarDone = true;
#endif
    const bool spentTheDeadline = SpentTheLockDeadline(timeoutsBefore);
    if (g.log) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "the S_NETVAR listener was refused (%s); attempt %u of %u", g.tap->Diagnostic(),
                     unsigned(g.netvarAttempts), unsigned(kTapAttempts));
    }
    if (g.netvarAttempts >= kTapAttempts) {
        g.netvarDone = true;
        g.netvarStatus = s;   // ListenRefused: asked, refused, and now given up on
        if (g.log) {
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "netvar-registration-abandoned after %u attempts (%s): S_NETVAR will be "
                         "empty for this match and the file will say so",
                         unsigned(g.netvarAttempts), g.tap->Diagnostic());
        }
    }
    return spentTheDeadline;
}

// â˜… AUDIT_M3 F2: the bound is per TICK, not per walker. W1-L's Listen busy-spins up to 2000 us before
// it refuses a held CR15NetGame listener lock, and everything the capture registers -- 36 tap types and
// the S_NETVAR listener -- draws on the same game-thread frame. Registration therefore has ONE entry
// point, and it spends at most one deadline: whatever it did not get to is deferred to the next tick.
//
// The netvar listener asks FIRST. It is a single Listen for a whole stream whose declared retention is
// ALL (DESIGN S5.4.5), while a deferred tap costs one message type -- and the tap walk can defer 36
// deep at kTapAttempts each, so letting it go first would hold S_NETVAR off for up to 180 ticks.
void RegisterSurfaces() {
    if (!g.tap) {
        return;
    }
    if (g.tapsPending == 0 && g.netvarDone) {
        // Everything is settled, and this runs on the game thread inside S6.4's 20 us tick budget.
        //
        // This is only sound because `tapsPending` is SEEDED before the first tick. Left
        // at its member default it would mean both "none pending" and "no walk has counted anything",
        // and `netvarDone` cannot tell those apart -- the netvar listener asks FIRST and settles after
        // five contended ticks in which the walk never ran, so the guard would close for the life of
        // the process before a single tap had been asked for. ClearTapRegistrationState and the walk
        // itself are the only writers of this count.
        return;
    }
    bool spentTheDeadline = RegisterNetvarListener();
    if (!spentTheDeadline) {
        spentTheDeadline = RegisterTaps();
    }
    // Published before either exit below: this tick may have registered, deferred or given up on
    // something, and a consumer on another thread reads only what was published.
    PublishTapRegistration(ComputeTapRegistration());
    // Read back rather than reused: TapRegistrationNow fills the table-shaped counts, and the line
    // below then reports exactly what any other reader of this tick would see.
    const TapRegistration r = TapRegistrationNow();
    if (!spentTheDeadline) {
        return;
    }
    // What is left is reported once per value, not once per tick: a lock held for a thousand ticks
    // says so once. The key is the PAIR -- `deferred` also falls when a type is given up
    // on, and a line saying "35 left for a later tick" because one was abandoned understates the loss.
    if (r.deferred > 0 && g.log &&
        (r.deferred != g.tapStopPendingLogged || r.givenUp != g.tapGivenUpLogged)) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "tap-registration-deferred: one lock deadline spent this tick, %u of %u "
                     "message-bus registration%s still unsettled, %u given up on (%s)",
                     unsigned(r.deferred), unsigned(r.total), r.deferred == 1 ? "" : "s",
                     unsigned(r.givenUp), g.tap->Diagnostic());
        // Inside the guard, not beside it: a tick with no log must not consume the value
        // and suppress the first line that would have carried it.
        g.tapStopPendingLogged = static_cast<uint16_t>(r.deferred);
        g.tapGivenUpLogged = static_cast<uint16_t>(r.givenUp);
    }
}

bool OnVoipFrame(uint32_t speakerSlot, uint16_t seq, const void* payload, uint32_t bytes, void*);

// From the other end: if a stale launcher still puts `-replaycapturesecs` on the
// engine's line, the engine has ALREADY discarded that whole line by the time this runs -- -offline,
// -level and all four host pins with it -- and the capture is running on an online, unpinned
// session. Nothing here can undo that. What it can do is make it the loudest line in the log
// instead of a capture nobody could tell from a good one.
void WarnOnLegacyCaptureSecsSwitch() {
    if (!CommandLineMentionsCaptureSecs(GetCommandLineW())) {
        return;
    }
    g.log->Write(LogLevel::Warn, kReplayModuleName, "%s", kLegacyCaptureSecsWarning);
}

// Three answers, not two. A value that was set and refused must not report on the ready line as a
// launch that asked for no deadline at all.
const char* DeadlineWord() {
    if (g.deadlineRefusals) {
        return "refused";
    }
    return g.deadlineAtMs.load(std::memory_order_relaxed) ? "armed" : "absent";
}

// How long this process was launched to capture for, if anything said so.
void ReadCaptureDeadline() {
    WarnOnLegacyCaptureSecsSwitch();

    CaptureSecsRequest req;
    if (!ReadCaptureSecondsRequest(&req)) {
        return;   // nothing asked: always-on capture, closed by the match ending, exactly as before
    }
    if (!req.hasSeconds) {
        // Named and counted, never clamped into range. A capture silently shortened or lengthened
        // to fit a bound is a capture of a length nobody asked for, reported as the one they did.
        ++g.deadlineRefusals;
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "%ls refused: %s (value as read: '%ls'). No deadline is armed and capture "
                     "behaves exactly as it does when the variable is not set",
                     kCaptureSecsEnvVar, CaptureSecsRefusalText(req.refusal),
                     req.value[0] != 0 ? req.value : L"the value did not fit");
        return;
    }
    // C6: a viewer session arms nothing. The two carriers can both be set -- an environment
    // variable, unlike a switch, can already be there machine-wide (RK-11) -- and closing the
    // capture out from under a session launched to WATCH one is the wrong answer to that.
    if (bootstrap::OpenRequestPresent()) {
        g.log->Write(LogLevel::Info, kReplayModuleName,
                     "%ls %u: ignored - viewer session (%ls is set)", kCaptureSecsEnvVar,
                     unsigned(req.seconds), bootstrap::OpenEnvVarName());
        return;
    }
    g.deadlineSecs = req.seconds;
    g.deadlineAtMs.store(GetTickCount64() + static_cast<uint64_t>(req.seconds) * 1000ull,  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF plain decimal 1000 (a millisecond scale or a count), not the CR15Game vtable slot
                         std::memory_order_relaxed);
    // RK-11 again: the value is logged rather than quietly obeyed, because a machine-wide setting
    // left over from an earlier launch is invisible in every other record this process leaves.
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "%ls %u: the capture closes %u s from now", kCaptureSecsEnvVar,
                 unsigned(req.seconds), unsigned(req.seconds));
}

// Init APPENDS to the watch list, so the list is reset with the other per-launch
// latches and the append is bounded by the array's own extent rather than by trust in the caller. A
// second Init used to list the same directory twice -- every close then wrote its sentinel there
// twice, the second write hit the never-replace rule, and the module raised G-9's "a
// reader polling that directory will see no sentinel for this launch" alarm, falsely, about a
// directory that had one. A third wrote past the end of the array, over stopWatchCount, the open-
// retry block and the close ordinal the per-close invariant rests on. A duplicate is dropped and a
// directory that does not fit is refused BY NAME, never written.
bool AddStopWatchDir(const wchar_t* dir) {
    if (!dir || !dir[0]) {
        return false;
    }
#if defined(W2A_MUTATE_STOPWATCH_UNBOUNDED)
    // Mutation control: the pre-fix shape. Append, trusting the caller for both the
    // duplicate and the extent; with the reset below disabled too, a second Init lists the same
    // directory twice and a third writes past the end of the array.
    wcscpy_s(g.stopWatchDirs[g.stopWatchCount++], kPathMax, dir);
    return true;
#endif
    for (uint32_t i = 0; i < g.stopWatchCount; ++i) {
        if (_wcsicmp(dir, g.stopWatchDirs[i]) == 0) {
            return false;
        }
    }
    if (g.stopWatchCount >= kStopWatchMax) {
        if (g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "%ls will NOT be watched for %ls: this launch can watch %u director(ies) "
                         "and both are taken",
                         dir, kStopRequestName, unsigned(kStopWatchMax));
        }
        return false;
    }
    wcscpy_s(g.stopWatchDirs[g.stopWatchCount++], kPathMax, dir);
    return true;
}

// The config-row table's element type, at namespace scope so the coverage check below can be a
// constant expression: a row missing from that table is the defect that disabled S_GAMESTATE for the
// whole project's history and S_PLAYER_STATS for the run after it, and a comment above the table did
// not stop the second one.
struct StreamKeyRow {
    uint16_t id;
    const char* key;
};

template <size_t N>
constexpr bool EveryProducibleStreamHasAConfigRow(const StreamKeyRow (&rows)[N]) {
    for (uint16_t id : kProducibleStreams) {
        bool found = false;
        for (size_t i = 0; i < N; ++i) {
            if (rows[i].id == id) found = true;
        }
        if (!found) return false;
    }
    return true;
}

// modreg::RunPreExit, from the engine's RadPluginShutdown: the owner closed the game with a match
// still recording. The worker closes the file itself once `quit` is set -- StopCapture, its last
// act -- so this asks and waits, and touches nothing else: the tap, the netvar reader and the
// detours all reach into an engine that is already shutting down. A worker that has not finished
// is left running; it may still make it before the process goes.
void FinalizeForExit(uint32_t budgetMs) {
    g.quit.store(true, std::memory_order_release);
    if (!g.thread) {
        return;
    }
    const DWORD joined = WaitForSingleObject(g.thread, budgetMs);
    if (!g.log) {
        return;
    }
    if (joined == WAIT_OBJECT_0) {
        g.log->Write(LogLevel::Info, kReplayModuleName, "pre-exit: the capture worker closed out");
    } else {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "pre-exit: the capture worker had not closed out after %u ms (wait returned "
                     "0x%lX); an open capture may be left without its footer",
                     budgetMs, static_cast<unsigned long>(joined));
    }
}

bool Init(ReplayServices* s) {
    REPLAY_REQUIRE(s, log);
    REPLAY_REQUIRE(s, config);
    REPLAY_REQUIRE(s, tick);
    g.svc = s;
    g.log = s->log;
    g.cfg = s->config;
    // Ahead of every early return below, because the tick reads this state whether or not the rest of
    // Init runs.
    ClearTapRegistrationState();

    // â˜… AUDIT_M6 F1(b). Every PER-LAUNCH latch, reset here. A second Init in one process is a case
    // this file already handles elsewhere (the tap state above, AUDIT_M3 F7), and left standing
    // these carried the first session's history into the second: `quit` still set from Shutdown, so
    // the new worker exited on its first pass; `deadlineFired` set, so the new deadline could never
    // fire; `suppressReopen` set, so the first match of the new session was never captured.
    //
    // `closeSeq` is deliberately NOT reset. `launchFileTime` (this process's creation time) and the
    // pid are the same across a re-Init, so restarting the ordinal would name a sentinel that is
    // already on disk and hit the refuse-never-replace rule. The sequence continuing
    // is what keeps the two sessions' documents distinguishable.
    g.quit.store(false, std::memory_order_release);
    SetPhase(Phase::Idle);
    g.openRequested.store(false, std::memory_order_release);
    g.closeCauses.store(0, std::memory_order_release);
    g.inGame.store(false, std::memory_order_release);
    g.suppressReopen.store(false, std::memory_order_release);
    g.stopRequestSeen = false;
    g.deadlineFired = false;
    // Cleared BEFORE ReadCaptureDeadline runs below: a second Init with no deadline asked for must
    // not inherit the first session's elapsed one and close its first capture instantly.
    g.deadlineAtMs.store(0, std::memory_order_relaxed);
    g.deadlineSecs = 0;
    g.deadlineRefusals = 0;
    g.nextStopPollMs = 0;
    g.openFailKind = OpenFailKind::None;
    g.openFailStreak = 0;
    g.openFailTotal = 0;
    g.openBackoffMs = 0;
    g.openRetryAtMs = 0;
    // The latch F1(b) missed, and the only one whose absence is a memory-safety defect
    // rather than a reporting one: this list is APPENDED to below, never assigned.
#if !defined(W2A_MUTATE_STOPWATCH_UNBOUNDED)
    g.stopWatchCount = 0;
    memset(g.stopWatchDirs, 0, sizeof g.stopWatchDirs);
#endif
    g.costInjectWindows.store(0, std::memory_order_release);

    if (const char* why = SchemaTableRefusal()) {
        g.log->Write(LogLevel::Error, kReplayModuleName,
                     "the stream schema table does not validate (%s); capture will not run", why);
        return false;
    }

    g.captureEnabled = g.cfg->GetBool("capture.enabled", true) &&
                       !g.cfg->GetBool("bootstrap.minimal", false);
    g.autoRecord = g.cfg->GetBool("capture.auto_record", true);
    g.forceOffline = g.cfg->GetBool("capture.force_offline", false);
    g.recIndicator = g.cfg->GetBool("capture.rec_indicator", true);
    ReadCaptureDeadline();
    if (!g.captureEnabled) {
        if (g.deadlineAtMs.load(std::memory_order_relaxed)) {
            // The worker never starts here, so the deadline can never fire and the launcher waiting
            // for a `closed:` line would wait out its whole timeout and record a hang. Say what
            // happened instead, once, and disarm.
            // This was the one `closed:` line with no sentinel behind it -- and it is
            // the launch where a reader has nothing else to go on, because no file and no worker
            // ever existed. The watch directories are resolved here because this early return never
            // reaches the block that resolves them below, and the document says what it is.
            g.launchFileTime = LaunchFileTime(nullptr);
            wchar_t disabledCwd[kPathMax] = {};
            if (GetCurrentDirectoryW(kPathMax, disabledCwd)) {
                AddStopWatchDir(disabledCwd);
            }
            AddStopWatchDir(Resolved() ? StateDir() : nullptr);
            StopFacts facts{};
            facts.costTail = CostTailNow();
            facts.launchFileTime = g.launchFileTime;
            facts.pid = ::GetCurrentProcessId();
            facts.reason = StopReason::Disabled;
            facts.closeCauses = CloseCauseBit(StopReason::Disabled);
            facts.closeSeq = g.closeSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
            facts.openFailState = OpenFailStateName(OpenFailKind::None);
            facts.workerJoin = "n/a";
            strcpy_s(facts.verdict.detail, sizeof facts.verdict.detail,
                     "capture.enabled is false: no file was opened, so there is nothing to verify");
            uint32_t disabledWrote = 0;
            for (uint32_t i = 0; i < g.stopWatchCount; ++i) {
                char err[192] = {};
                if (WriteStopSentinelForClose(g.stopWatchDirs[i], facts, err, sizeof err)) {
                    ++disabledWrote;
                    char archiveErr[192] = {};
                    ArchiveStopSentinel(g.stopWatchDirs[i], facts, archiveErr, sizeof archiveErr);
                } else {
                    g.log->Write(LogLevel::Warn, kReplayModuleName,
                                 "the disabled-capture sentinel could not be written into %ls: %s",
                                 g.stopWatchDirs[i], err);
                }
            }
            wchar_t disabledName[64] = {};
            StopSentinelNameForClose(facts.launchFileTime, facts.pid, facts.closeSeq, disabledName,
                                     64);
            if (disabledWrote) {
                g.log->Write(LogLevel::Info, kReplayModuleName,
                             "stop sentinel %ls (close %u of this launch) written into %u launch "
                             "director%s: capture.enabled is false",
                             disabledName, facts.closeSeq, disabledWrote,
                             disabledWrote == 1 ? "y" : "ies");
            } else {
                g.log->Write(LogLevel::Error, kReplayModuleName,
                             "the stop sentinel %ls could not be written anywhere: the `closed:` "
                             "line below has no document behind it",
                             disabledName);
            }
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "closed: capture.enabled is false, so the %u s deadline has no capture to "
                         "close",
                         unsigned(g.deadlineSecs));
            g.deadlineAtMs.store(0, std::memory_order_relaxed);
        }
        g.log->Write(LogLevel::Info, kReplayModuleName, "capture.enabled is false");
        return true;
    }

    for (uint16_t i = 0; i < kSectionCount; ++i) {
        g.builder.sectionEnabled[i] = true;
    }
    g.builder.sectionEnabled[kSectionAsSeen] = g.cfg->GetBool("capture.section.asseen", true);
    static constexpr StreamKeyRow kStreamKeys[] = {
        {kStreamTick, "capture.stream.tick"},
        {kStreamPlayerWire, "capture.stream.player_wire"},
        {kStreamPlayerSampled, "capture.stream.player_sampled"},
        {kStreamBodyWire, "capture.stream.body_wire"},
        {kStreamBodySampled, "capture.stream.body_sampled"},
        {kStreamNetvar, "capture.stream.netvar"},
        {kStreamEvent, "capture.stream.event"},
        {kStreamKey, "capture.stream.key"},
        {kStreamMark, "capture.stream.mark"},
        {kStreamAsSeen, "capture.stream.asseen"},
        // L2-RUN-5 F3. THE MISSING ROW. streamEnabled[] is zero-initialised and only this table sets
        // it, so S_GAMESTATE was disabled in every capture ever taken: SampleGameState returned at
        // its first line, the phase cost 0.0 us, the `never produced` degrade record is itself gated
        // on the same flag, and no line anywhere said the stream was off. A stream added to the
        // writer and not to this table is a stream that cannot be produced.
        {kStreamGameState, "capture.stream.gamestate"},
        // L2-RUN-7 F2. THE SAME MISSING ROW, one commit later, for S_PLAYER_STATS: SamplePlayerStats
        // returned at its first line on every tick of every capture and the six stats counters read
        // zero, which is what a stream that cannot be produced looks like from the outside. The
        // static_assert below is what stops the third one.
        {kStreamPlayerStats, "capture.stream.playerstats"},
        {kStreamVoip, "capture.stream.voip"},
        {kStreamOracle, "capture.oracle"},
    };
    static_assert(sizeof kStreamKeys / sizeof kStreamKeys[0] == kProducibleStreamCount,
                  "every producible stream needs exactly one config row, and no row may name a "
                  "stream this capture cannot produce");
    static_assert(EveryProducibleStreamHasAConfigRow(kStreamKeys),
                  "a producible stream has no config row, so streamEnabled[] leaves it off and "
                  "nothing says so -- L2-RUN-5 F3 and L2-RUN-7 F2");
    for (const auto& row : kStreamKeys) {
        g.builder.streamEnabled[row.id] = g.cfg->GetBool(row.key, true);
    }
    // â˜… A RETIRED KEY IS IGNORED AND SAID OUT LOUD, never fatal. capture.stream.world went with
    // S_WORLD (DESIGN S5.4.5), and an owner whose config still carries it must not get a hard failure
    // for a setting we removed -- nor silence, which is how somebody spends an afternoon on a knob
    // that does nothing.
    //
    // Presence is probed through the existing interface rather than a new one: an ABSENT key returns
    // whatever default it is handed, so the two calls disagree; a PRESENT key returns its stored value
    // both times. No IConfig change, and it cannot misreport a key that is genuinely set either way.
    for (const char* retired : {"capture.stream.world"}) {
        if (g.cfg->GetBool(retired, true) == g.cfg->GetBool(retired, false)) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "config key '%s' is RETIRED and is being ignored: S_WORLD was withdrawn "
                         "(DESIGN S5.4.5) because a stream carved out by subject is necessarily "
                         "redundant or empty. Its state is on S_BODY_*, S_NETVAR and the keyframe. "
                         "Stream id 10 stays reserved and is never reassigned",
                         retired);
        }
    }

    g.builder.streamEnabled[kStreamGap] = true;
    g.builder.streamEnabled[kStreamDropped] = true;
    g.builder.streamEnabled[kStreamDegraded] = true;

    // Ruling Q-C's asymmetry, applied from the role rather than from a preference: on a client the
    // recorder captures only what he was already permitted to hear, in his own match; on a dedicated
    // server the operator is recording other people wholesale.
    const bool voipDefault = true;   // corrected to the role's default when the role is known
    g.builder.streamEnabled[kStreamVoip] =
        g.builder.streamEnabled[kStreamVoip] && g.cfg->GetBool("capture.voip", voipDefault);

    const char* dir = g.cfg->GetString("capture.dir", nullptr);
    if (dir && dir[0]) {
        MultiByteToWideChar(CP_UTF8, 0, dir, -1, g.replayDir, kPathMax);
    } else {
        wchar_t home[kPathMax] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", home, kPathMax)) {
            _snwprintf_s(g.replayDir, kPathMax, _TRUNCATE, L"%s\\Documents\\EchoReplay", home);
        } else {
            wcscpy_s(g.replayDir, kPathMax, L".\\EchoReplay");
        }
    }

    // C6's second close trigger. The harness drops `stop-capture` in the launch directory; the state
    // directory is watched as well because paths.h is explicit that the CWD is an input this plugin
    // does not control -- a launcher that sets it elsewhere still loads us. Both absolute paths are
    // logged, so the harness reads where to put the request instead of assuming it.
    {
        bool stampIsProcessCreation = false;
        g.launchFileTime = LaunchFileTime(&stampIsProcessCreation);
        if (!stampIsProcessCreation) {
            // G-10: the harness names this launch by the child's creation FILETIME. A wall-clock
            // substitute cannot match it, and the reader's answer for a mismatched stamp is
            // CAPTURE-STALE-SENTINEL -- so the substitution is stated here rather than discovered
            // as a wrong verdict later.
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "the process creation time could not be read: the launch stamp is a "
                         "wall-clock substitute and will NOT match the harness's own; read the "
                         "stamp off the sentinel, not by recomputing it");
        }

        wchar_t cwd[kPathMax] = {};
        if (GetCurrentDirectoryW(kPathMax, cwd)) {
            AddStopWatchDir(cwd);
        }
        AddStopWatchDir(Resolved() ? StateDir() : nullptr);
        if (g.stopWatchCount == 0) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "no directory could be resolved to watch for %ls: the deadline is the only "
                         "close trigger this launch has",
                         kStopRequestName);
        } else {
            static_assert(kStopWatchMax == 2, "the line below renders exactly two directories");
            g.log->Write(LogLevel::Info, kReplayModuleName,
                         "%ls is polled once a second in: %ls%ls%ls (a copy older than this launch is "
                         "ignored as stale)",
                         kStopRequestName, g.stopWatchDirs[0], g.stopWatchCount > 1 ? L" and " : L"",
                         g.stopWatchCount > 1 ? g.stopWatchDirs[1] : L"");
        }

        // D-4: every sentinel in these directories belongs to a launch that is over. Swept before this
        // launch can write one, so from here an existing sentinel is either this launch's or nobody's
        // -- which is what lets an ABSENT sentinel mean "this capture never finalised" instead of
        // "somebody's did". A sentinel that will not delete is reported: it can still be read as ours.
        uint32_t sweptTotal = 0;
        uint32_t sweepFailures = 0;
        for (uint32_t i = 0; i < g.stopWatchCount; ++i) {
            uint32_t failed = 0;
            sweptTotal += DeleteStaleStopSentinels(g.stopWatchDirs[i], &failed,
                                                   &ReportSweptSentinel, g.stopWatchDirs[i]);
            sweepFailures += failed;
        }
        wchar_t sentinelName[64] = {};
        StopSentinelNameFor(g.launchFileTime, ::GetCurrentProcessId(), sentinelName, 64);
        g.log->Write(sweepFailures ? LogLevel::Warn : LogLevel::Info, kReplayModuleName,
                     "stop sentinels: %u stale document(s) deleted, %u could not be deleted; this "
                     "launch will write %ls, and a copy of it under %ls\\%ls",
                     sweptTotal, sweepFailures, sentinelName,
                     g.stopWatchCount ? g.stopWatchDirs[0] : L"(the capture directory)",
                     kStopArchiveDirName);
    }

    g.ringMemory.assign(kRingBytes, 0);
    if (!g.rings.Init(g.ringMemory.data(), kRingBytes)) {
        g.log->Write(LogLevel::Error, kReplayModuleName, "the capture ring would not initialise");
        return false;
    }
    g.cost.Init();

    // W3-C spike section 6. Registration is explicit and happens once, here, where the ring and the
    // time axis this sink depends on have just been created. Before this call every voice frame the
    // detour sees is counted droppedNoSink and reaches no file.
    audio::voip::SetVoipSink(&OnVoipFrame, nullptr);

    // S5.4.7 / ruling Q-B. The ORACLE producer is created with the ring it pushes into and torn down
    // with it. VerifySites is asked ONCE and its answer is logged verbatim: a detour another mod put
    // on either builder site has to read as SiteRefused here, never as a call through.
    if (g.builder.streamEnabled[kStreamOracle] && g_oracleTap == nullptr) {
        g_oracleTap = oracletap::Create(g_oracleStage, kOracleSessionCap,
                                        g_oracleStage + kOracleSessionCap, kOracleBonesCap);
        if (g_oracleTap == nullptr) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "oracle tap: could not be created; S_ORACLE will carry nothing");
        } else {
            g_oracleWhy[0] = 0;
            g_oracleSitesOk = oracletap::VerifySites(g_oracleWhy, sizeof g_oracleWhy);
            BuilderFn session = nullptr, bones = nullptr;
            void* sessionCtx = nullptr;
            void* bonesCtx = nullptr;
            const char* missing = OracleBuilders(session, sessionCtx, bones, bonesCtx);
            g_oracleBindWhy[0] = 0;
            if (missing == nullptr) {
                oracletap::BindBuilders(g_oracleTap, session, sessionCtx, bones, bonesCtx);
            } else {
                FormatOracleNotBound(missing, g_oracleBindWhy, sizeof g_oracleBindWhy);
            }
            if (g_oracleClockForTest != nullptr) {
                oracletap::BindClock(g_oracleTap, g_oracleClockForTest);
            }
            g.log->Write(g_oracleSitesOk ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
                         "oracle tap: %u sites %s%s%s, %s, caps %u/%u B, one sample per %u us",
                         unsigned(oracletap::SiteCount()),
                         g_oracleSitesOk ? "verified" : "REFUSED",
                         g_oracleWhy[0] ? ": " : "", g_oracleWhy,
                         (missing == nullptr) ? "both producers bound" : g_oracleBindWhy,
                         unsigned(kOracleSessionCap), unsigned(kOracleBonesCap),
                         unsigned(g.builder.keyframeIntervalUs));
        }
    }

    if (g.cfg->GetBool("capture.trap", false)) {
        const uint32_t live = TrapInstall(kTrapAll);
        g.trapArmed = live == kTrapAll;
        // A site that failed to install is a trap that silently cannot fire, so the asked-for mask and
        // the live mask are compared rather than assumed equal.
        g.log->Write(g.trapArmed ? LogLevel::Info : LogLevel::Warn, kReplayModuleName,
                     "capture.trap: asked for 0x%X, live 0x%X", unsigned(kTrapAll), unsigned(live));
    }

    // The completeness-detector trait that used to live here is gone with the defect it detected.
    // services.h forward-declared IEntity and IAsSeen in namespace replay while their headers defined
    // them at global scope, so ReplayServices named types that were never defined and two streams
    // could not be gathered at all; W1-E has moved both headers and the gathers compile in. The
    // standing report is tests/capture/nscheck.cpp, which asks the COMPILER every gate run rather than
    // asserting it here once -- reading a header is not how that question gets answered.

    // Once per process. tick.cpp appends without dedupe and ITick offers no
    // unregister, so a second Init that registered again would gather every frame twice; the
    // callback the first Init installed is the same function over the same globals.
    if (!g.tickRegistered) {
        if (!s->tick->RegisterEarly(&OnTick, nullptr)) {
            g.log->Write(LogLevel::Error, kReplayModuleName, "the tick refused our callback");
            return false;
        }
        g.tickRegistered = true;
    }

    g.thread = CreateThread(nullptr, 256 * 1024, &WorkerMain, nullptr, 0, nullptr);
    if (!g.thread) {
        g.log->Write(LogLevel::Error, kReplayModuleName, "the capture worker would not start");
        return false;
    }
    if (!modreg::RegisterPreExit(&FinalizeForExit)) {
        g.log->Write(LogLevel::Warn, kReplayModuleName,
                     "the pre-exit table is full; a capture open when the game is closed will not "
                     "be finalised");
    }
    g.log->Write(LogLevel::Info, kReplayModuleName,
                 "armed; ring %u KiB, %u schemas, deadline %s (%ls, %u refused)",
                 kRingBytes / 1024u, unsigned(SchemaCount()), DeadlineWord(), kCaptureSecsEnvVar,
                 unsigned(g.deadlineRefusals));
    return true;
}

void Tick(const TickInfo& info) {
    // The tap surfaces need a CR15NetGame, which does not exist before state 9. Creating them from the
    // tick is what B.14 requires anyway: Listen from inside a callback is refused by the engine's own
    // list discipline, so registration belongs on the tick.
    const bool built = (!g.tap && info.netGame != nullptr);
    if (built) {
        g.tap = g_tapOverrideForTest ? g_tapOverrideForTest : WiretapCreate(info.netGame);
        // Built, not registered: RegisterSurfaces below issues its Listen inside the same budget the
        // tap walk draws on, so the busiest tick of the session cannot spend two deadlines.
        g.netvar = netvar::Create(g.tap, &OnNetvarMessage, nullptr);
        netvar::NoteTickThread(g.netvar, GetCurrentThreadId());
        if (g.svc) {
            g.svc->netVar = g.netvar;
        }
    }
    if (!g.tap) {
        return;
    }
#if defined(W2A_MUTATE_NO_TAP_RETRY)
    // W1-L bounded the registration lock and made a contended one a refusal instead of a hung frame.
    // The refusal is only about that instant, so the answer it names -- "retry on a later tick" -- has
    // to actually happen. This mutation deletes the retry: one pass and nothing more.
    if (!built) {
        return;
    }
#endif
    RegisterSurfaces();
    if (built && g.log) {
        // After the first registration attempt, not before it: reporting the status of a surface
        // nobody has asked anything of yet would say "pending" on every launch.
        g.log->Write(LogLevel::Info, kReplayModuleName, "netvar surface: %s",
                     netvar::StatusText(netvar::StatusOf(g.netvar)));
    }
}

// One voice frame, handed over by W3-C's detour on the ENGINE'S AUDIO THREAD. Everything it needs is
// already thread-safe: the phase is atomic, the time axis is published atomically by the tick, and the
// ring is multi-producer.
//
// â˜… Every refusal here returns false rather than storing something approximate, because W3-C counts a
// false as droppedSinkRefused and a stored-but-wrong frame is counted as success. The three refusals
// are: not recording, no tick has published a time axis yet, and a payload too large for the record's
// own length field. That last one is explicit even though PushRecord would refuse the oversize frame
// anyway -- the u16 narrowing happens BEFORE that check, so without the guard the record would carry a
// wrapped length that nothing downstream could tell from a real one.
bool OnVoipFrame(uint32_t speakerSlot, uint16_t seq, const void* payload, uint32_t bytes, void*) {
    if (g.phase.load(std::memory_order_acquire) != Phase::Recording ||
        !g.builder.streamEnabled[kStreamVoip]) {
        return false;
    }
    const uint64_t tUs = g.lastSourceUs.load(std::memory_order_acquire);
    if (!tUs) {
        // Voice before the first tick has no time axis to be placed on. Refused so it is counted,
        // rather than stamped zero and buried at the head of the file.
        return false;
    }
    if (bytes > 0xFFFFu) {  // facts-ok: NETVAR.SLOT_NONE the u16 range limit, not the engine no-slot field
        return false;
    }
    VoipRec r{};
    r.tUs = tUs;
    // The SR15NetUserId as the engine gave it: {u16 userSlot, u16 generation}, stored WHOLE and never
    // re-derived. The engine's own receiver resolves a speaker from the low word alone and never checks
    // the generation, so a handle recorded in one session resolves in a later one to whoever now holds
    // that pool slot -- voice out of the wrong player's head, with nothing failing. Keeping both halves
    // is what lets playback detect that instead of reproducing it.
    r.speakerSlot = speakerSlot;
    r.frameCtr = seq;
    r.bodyBytes = static_cast<uint16_t>(bytes);
    return PushRecord(kRecVoip, &r, sizeof r, payload, bytes);
}

// The close the WORKER cannot write. Every other sentinel is produced inside
// StopCapture, before the join is even attempted, so "the worker did not join" is a fact that only
// exists after the last document the worker could have produced. It takes a close ordinal of its own
// -- reserved atomically, because a stuck worker may still be about to use one -- and states in the
// document what the log states: this launch ended with its worker unaccounted for.
void WriteJoinTimeoutSentinel() {
    StopFacts facts{};
    facts.costTail = CostTailNow();
    facts.launchFileTime = g.launchFileTime;
    facts.pid = ::GetCurrentProcessId();
    facts.reason = StopReason::Shutdown;
    facts.closeCauses = CloseCauseBit(StopReason::Shutdown);
    facts.closeSeq = g.closeSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    facts.openFailState = OpenFailStateName(OpenFailKind::None);
    facts.workerJoin = "timed out";
    strcpy_s(facts.verdict.detail, sizeof facts.verdict.detail,
             "the capture worker did not join, so no file could be verified for this close");
    for (uint32_t i = 0; i < g.stopWatchCount; ++i) {
        char err[192] = {};
        if (!WriteStopSentinelForClose(g.stopWatchDirs[i], facts, err, sizeof err) && g.log) {
            g.log->Write(LogLevel::Warn, kReplayModuleName,
                         "the worker-join sentinel could not be written into %ls: %s",
                         g.stopWatchDirs[i], err);
        }
        char archiveErr[192] = {};
        ArchiveStopSentinel(g.stopWatchDirs[i], facts, archiveErr, sizeof archiveErr);
    }
}

void Shutdown() {
    // Torn down with the ring it pushed into: the tap borrows g_oracleStage and the stage outlives
    // nothing else, so the destroy belongs at the same edge the sink unregistration does.
    if (g_oracleTap != nullptr) {
        oracletap::Destroy(g_oracleTap);
        g_oracleTap = nullptr;
    }
    // Before anything else: the detour must stop calling into state this function is about to tear
    // down. W3-C counts the frames that arrive with no sink, so unregistering loses nothing silently.
    audio::voip::SetVoipSink(nullptr, nullptr);

    g.quit.store(true, std::memory_order_release);
    if (g.thread) {
        // The result used to be discarded. The shutdown close is the heaviest path in
        // the module -- drain, close, re-open and decode the file, then three sentinel writes per
        // watch directory -- and a slow disk is exactly what NoteOpenFailure exists for, so the
        // timeout is a real state and it is now said out loud and documented.
        //
        // The teardown below still runs on a timeout, deliberately: TrapUninstall must remove the
        // detours before this DLL can be unloaded, and detours outliving their module is a worse
        // fault than the one it avoids. That residual hazard is stated here rather than traded for
        // a different one silently.
        const DWORD joined = WaitForSingleObject(g.thread, 2000);
        if (joined == WAIT_OBJECT_0) {
            g.log->Write(LogLevel::Info, kReplayModuleName, "worker-join: ok");
        } else {
            g.log->Write(LogLevel::Error, kReplayModuleName,
                         "worker-join: timed out - the capture worker did not exit within 2000 ms "
                         "(wait returned 0x%lX) and its collaborators are being torn down while it "
                         "may still be inside them",
                         static_cast<unsigned long>(joined));
            WriteJoinTimeoutSentinel();
        }
        CloseHandle(g.thread);
        g.thread = nullptr;
    }
    if (g.netvar) {
        netvar::Destroy(g.netvar);
        g.netvar = nullptr;
    }
    if (g.tap) {
        g.tap->UnlistenAll();
        if (g.tap != g_tapOverrideForTest) {
            WiretapDestroy(g.tap);
        }
        g.tap = nullptr;
    }
    // A second Init in this process has to start from nothing registered. Left as they
    // are, tapDone says every type is settled and g.taps holds handles belonging to a destroyed tap:
    // the new session would register nothing and the next Shutdown would Unlisten stale handles. The
    // tap itself was handed back above, so forgetting the handles here is safe.
    ClearTapRegistrationState();
    if (g.trapArmed) {
        TrapUninstall();
    }
}

}  // namespace

// RECORDER-2 part D -- the manual door. Both run on the TICK thread: the command queue is drained
// there, so neither needs a lock and neither opens or closes a file itself. Start arms the same
// gate auto-record uses; stop asks for the close the match end asks for.
void RequestManualRecordStart() { g.manualStart.store(true, std::memory_order_release); }

bool CaptureSessionRecordable(const char** kind) {
    if (kind) *kind = livesafety::VerdictName(livesafety::Current());
    return livesafety::Current() == livesafety::Verdict::LiveOnlineExempt || g.forceOffline;
}

void CaptureRecordingPaths(wchar_t* dir, size_t dirCap, wchar_t* cur, size_t curCap) {
    if (dir && dirCap) wcscpy_s(dir, dirCap, g.replayDir);
    if (cur && curCap) {
        cur[0] = 0;
        AcquireSRWLockShared(&g.curLock);
        if (g.phase.load(std::memory_order_acquire) == Phase::Recording) wcscpy_s(cur, curCap, g.curPath);
        ReleaseSRWLockShared(&g.curLock);
    }
}

bool RequestManualRecordStop() {
    if (g.phase.load(std::memory_order_acquire) != Phase::Recording) {
        return false;
    }
    g.closeCauses.fetch_or(CloseCauseBit(StopReason::Request), std::memory_order_release);
    SetPhase(Phase::Closing);
    return true;
}

void RecordStatusLine(char* out, size_t cap) {
    if (out == nullptr || cap == 0) return;
    const Phase phase = g.phase.load(std::memory_order_acquire);
    const char* phaseWord = phase == Phase::Idle        ? "idle"
                            : phase == Phase::Arming    ? "arming"
                            : phase == Phase::Recording ? "recording"
                                                        : "closing";
    _snprintf_s(out, cap, _TRUNCATE,
                "record: %s; session=%s auto_record=%s force_offline=%s indicator=%s enabled=%s; "
                "file=%ls",
                phaseWord, livesafety::VerdictName(livesafety::Current()),
                g.autoRecord ? "on" : "off", g.forceOffline ? "on" : "off",
                g.recIndicator ? "on" : "off", g.captureEnabled ? "yes" : "no",
                g.fileOpen ? g.openPath : (g.lastClosedPath[0] ? g.lastClosedPath : L"(none)"));
}

// tests/capture only, and declared there rather than in a header so nothing outside a test can reach
// it by including something. Must be called before the first tick carrying a CR15NetGame.
void SetTapOverrideForTest(IWiretap* tap) { g_tapOverrideForTest = tap; }

// A view of the registry AS V-I-W2I1 WILL PUBLISH IT: rows whose call shape is promoted, plus the
// resolver that turns one into a pointer (the shipped resolver reads the loaded image, which a test
// host has none of). A suite installs one BEFORE Init -- the only place BindBuilders is called from
// -- and then reads the records back out of the closed file. There is no builder-pointer seam: the
// shipped decision reads rows and tiers, so promoting a row is the only way to reach a bind.
void SetOracleRegistryViewForTest(const OracleRegistryView* view) { g_oracleViewForTest = view; }

// tests/capture only. Runs the SHIPPED decision over `view` -- null means the real registry -- and
// reports both halves of what it decided: the refusal it composed (null when none) and whether a
// callable pair came back. The installed view is put back before returning, so a suite that promoted
// rows keeps them.
const char* OracleDecideForTest(const OracleRegistryView* view, char* out, size_t cap, bool& bound) {
    const OracleRegistryView* const saved = g_oracleViewForTest;
    g_oracleViewForTest = view;
    BuilderFn session = nullptr, bones = nullptr;
    void* sessionCtx = nullptr;
    void* bonesCtx = nullptr;
    const char* missing = OracleBuilders(session, sessionCtx, bones, bonesCtx);
    bound = session != nullptr && bones != nullptr;
    if (out != nullptr && cap != 0) {
        out[0] = 0;
        if (missing != nullptr) FormatOracleNotBound(missing, out, cap);
    }
    g_oracleViewForTest = saved;
    return missing;
}

void SetOracleClockForTest(ClockFn fn) { g_oracleClockForTest = fn; }

// tests/capture only. Asks OracleBuilders() with the test seam momentarily vacated -- so the answer is
// the SHIPPED producers', not an installed stub pair's -- and composes the sentence Init would write
// from it. Returns the producer names it could not answer with, or null once W2-I promotes them. The
// caller's pair is put back before returning, so a suite that bound stubs keeps them.
const char* OracleShippedNotBoundForTest(char* out, size_t cap) {
    bool bound = false;
    return OracleDecideForTest(nullptr, out, cap, bound);
}

// The live word Init composed, as the tap line, the stop line and the close document read it.
void OracleBindWhyForTest(char* out, size_t cap) { strncpy_s(out, cap, g_oracleBindWhy, _TRUNCATE); }

void OracleCountsForTest(uint64_t& samples, uint64_t& ok) {
    samples = g_oracleSamples;
    ok = g_oracleOk;
}

// Init resets the watch list and adds at most two directories, so the bound
// AddStopWatchDir enforces is unreachable from the shipped entry points -- and a bound nothing
// can drive is a bound nothing checks. The count is published alongside the append because
// stopWatchCount is the word the overflowing wcscpy_s used to land on: a count that is still at
// the extent is the evidence that the refused write did not happen.
bool AddStopWatchDirForTest(const wchar_t* dir) { return AddStopWatchDir(dir); }
uint32_t StopWatchCountForTest() { return g.stopWatchCount; }
const wchar_t* StopWatchDirForTest(uint32_t i) {
    return i < kStopWatchMax ? g.stopWatchDirs[i] : nullptr;
}

// Same story. The open-retry's own counter, so a test can bound the ATTEMPTS and not
// only the Error lines they wrote -- an unbounded retry that learned to log once would still be
// hammering the filesystem 250 times a second, and the lines alone cannot see that.
uint32_t OpenFailuresForTest() { return g.openFailTotal; }

// B3's two halves, read by tests/capture: which stream a lost record of this kind is charged
// to, and how many kinds the producer's counter can hold. A kind outside the second is
// invisible to the first.
StreamId StreamOfKindForTest(uint16_t kind) { return StreamOfKind(kind); }
uint32_t PushFailKindSlotsForTest() {
    return static_cast<uint32_t>(sizeof g.pushFailByKind / sizeof g.pushFailByKind[0]);
}

// Same story: tests/capture only, declared in no header. What the meter's last complete window
// measured, so a test can ask which half of the tick a cost landed in -- the charged figures are the
// capture's budget and unchargedMeanUs is the always-on remainder. Evaluate() is what completes a
// window. The WORKER completes a window (WorkerMain calls Evaluate every pass), so a test drives
// ticks and then waits for `windows` to advance rather than calling Evaluate itself -- a second
// caller would reset the histogram underneath the worker and both would read half a window.
CostMeter::WindowStat CostWindowForTest() { return g.cost.LastWindow(); }
ILog* BuilderLogForTest() { return g.builder2.Log(); }
uint32_t PhaseWindowTicksForTest() { return g.phaseDone.ticks; }
uint32_t PhaseWindowOpenTicksForTest() { return g.phaseDone.openTicks; }
// The engine-read count `gather` spent in the last complete window. Published because the number
// the kill line prints has to be one a test can plant an answer for and read back: a counter that
// is only ever read by a log line cannot be shown to be counting the right bracket.
uint64_t PhaseWindowGatherProbesForTest() { return g.phaseDone.gatherProbes; }
uint64_t PhaseWindowGatherTscForTest(uint32_t part) {
    return part < kGatherPartCount ? g.phaseDone.gatherTsc[part] : 0;
}
uint64_t CaptureWorkTicksForTest() { return g.workTicks; }

// Same story. Arms the capture deadline `ms` from now, so a test can drive the deadline close
// without waiting out kCaptureSecsMin (30 s). It sets exactly what ReadCaptureDeadline sets and
// nothing else -- in particular it does not clear `deadlineFired`, which is the latch AUDIT_M6
// F1(b) is about.
void ArmDeadlineForTest(uint32_t ms) {
    g.deadlineSecs = (ms + 999) / 1000;  // facts-ok: GAME.VTBL_SETFIXEDTIMESTEP_OFF ms<->s conversion, not that slot
    g.deadlineAtMs.store(GetTickCount64() + ms, std::memory_order_relaxed);
}

// Same story: tests/capture only, declared in no header. Re-arms tap registration so a test can put
// the walk back at the start of the table against a tap that behaves differently. What is already
// registered is handed back first -- forgetting a live handle would leak a listener on the very tap
// the caller is about to re-drive.
void ResetTapRegistrationForTest() {
    if (g.tap) {
        for (uint16_t i = 0; i < g.tapCount; ++i) {
            g.tap->Unlisten(g.taps[i]);
        }
    }
    ClearTapRegistrationState();
    // The netvar listener is one of the registrations the tick budgets, so re-arming half of them
    // would leave a test measuring a tick that still has S_NETVAR settled from the previous one. The
    // surface is rebuilt rather than reset in place: that is exactly what the creating tick does.
    if (g.netvar) {
        netvar::Destroy(g.netvar);
        g.netvar = netvar::Create(g.tap, &OnNetvarMessage, nullptr);
        if (g.svc) {
            g.svc->netVar = g.netvar;
        }
    }
    PublishTapRegistration(ComputeTapRegistration());
}

// tests/capture only, declared in no header. Asks the WORKER to feed `windows` full windows of
// samples over the hard-fail budget into the launch's real cost meter, so the meter's own Evaluate
// climbs its own ladder to rung 5 and the kill reaches StopCapture through the shipped path
// Only the samples are synthetic; every decision taken on them is the module's.
void InjectCostWindowsForTest(uint32_t windows) {
    g.costInjectWindows.store(windows, std::memory_order_release);
}

// â˜… AUDIT_M5 F3. tests/capture only, declared in no header. The registration answer computed from the
// TICK's own globals -- the state a reader on any other thread must never be reading. The control
// uses it to prove its perturbation is visible at all before asserting the published snapshot has not
// moved; without that, an assertion that nothing changed would pass on a perturbation that changed
// nothing.
TapRegistration TapRegistrationLiveForTest() { return ComputeTapRegistration(); }

// Same story. Changes one tick-owned registration global and does NOT publish it. This is the whole
// M3 property in one call: after it, TapRegistrationNow() must still report the last PUBLISHED
// answer, and any regression to answering from the live globals reports the perturbed one instead.
void PerturbTapRegistrationWithoutPublishingForTest() { ++g.tapsAbandoned; }

}  // namespace capture
}  // namespace replay

REPLAY_MODULE("capture", ::replay::kModuleOrderCapture, ::replay::capture::Init, ::replay::capture::Tick,
              ::replay::capture::Shutdown);
