// W2-E's module translation unit: the one place on the shipping path that
// creates the camera control. DIAG-WIRING gap 3: camera_control::Create had no caller, so every
// camera command W2-F's panel and W2-G's console issued was refused NotAvailable, and "freely move
// around" was unreachable from our surface.
//
// The control is a CONTROLLER over the engine's own spectator rig (S8.1): the rig enables itself
// on the team gate, integrates its free camera on wall time, and owns input, so this module
// creates nothing on Init and binds nothing until a tick carries a netGame whose LEVEL space (at
// NETGAME.SPECTATORCAM_CS_OFF -- ARCH-12 R1) has passed W1-C's table check. The binding is
// re-validated every tick and re-created when its identity changes -- a new netGame, a new level
// space, or a rebuilt system table behind the same address (AVFIX F3) -- and dropped, without a
// write, when that space stops validating.
//
// VR (S8.1.6): the engine selects DisableSystem for the rig when a headset is worn, and that gate
// is the one thing a VR port revisits. This module takes no position on it: it reports the rig's
// own enabled flag and binds whatever instance the rig holds, so a headset build changes what the
// engine does, not what this file assumes.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bootstrap/buildguard.h"
#include "bootstrap/log.h"
#include "bootstrap/services.h"
#include "camera/camera_control.h"
#include "camera/camera_module.h"
#include "engine/tick.h"
#include "generated/facts_generated.h"

namespace replay {
namespace camera_module {
namespace {

const char* const kReplayModuleName = "camera";

// One refusal line per change, then one every kLogEveryDeferrals ticks, so a load window leaves
// evidence without burying the log in it.
const uint64_t kLogEveryDeferrals = 600u;
const uint64_t kRetryEveryTicks   = 60u;

// The rig-state census window: one second at the engine's tick rate. Every 60th tick of the
// module's life carries the line, so any 60 consecutive ticks carry exactly one of them whatever
// the phase.
const uint64_t kRigCensusEveryTicks = 60u;

// One entry per census window, so this is an eight-minute horizon at the engine's tick rate. A
// window that outruns it is reported as overflowed rather than scored over what is left: the
// clause's FAIL is a universal over the window's samples, and a subset can redden a window the
// whole of which would have passed.
const size_t kFlightRing = 512u;

struct State {
    ReplayServices*            services = nullptr;
    bool                       bound    = false;
    camera_control::RigIdentity identity;    // the binding's while bound; the last refusal's otherwise
    camera_control::RigProbe   lastProbe = camera_control::RigProbe::NoNetGame;
    uint64_t                   deferrals = 0;
    uint64_t                   retries   = 0;     // ticks a Ready identity has stayed refused
    uint64_t                   epoch     = 0;     // ticks the live binding has held
    int                        rigEnabled = -1;   // CAM.SPECTATOR_ENABLED_GLOBAL as last logged; -1 unknown
    uint64_t                   rigCensus  = 0;    // ticks counted for the per-window state read
    bool                       viewRefusalSaid = false;  // this binding session's view-read
                                                         // refusal is already on the status line

    // ARCH-23 S14.11's flight window. The samples ARE the census line's poses, so the clause
    // scores exactly the numbers a reader of the log can see for himself.
    camera_control::FlightSample ring[kFlightRing];
    size_t   ringCount = 0;             // entries written, capped at kFlightRing
    size_t   ringHead  = 0;             // the oldest entry once the ring is full
    bool     windowOpen = false;
    bool     windowTruncated = false;   // the ring wrapped while this window was open
    double   windowBegin = 0.0;
    double   windowEnd   = 0.0;
};

State g;

// The engine's own "spectator system enabled" dword -- the flag S8.1.1's enable path and the HTTP
// routes gate on. Evidence for the launch question, not a decision: -1 when the image is not
// mapped in this process.
const void* g_rigFlagForTest = nullptr;

int ReadRigEnabled() {
    const void* flag = g_rigFlagForTest != nullptr
                           ? g_rigFlagForTest
                           : buildguard::MappedAddress(facts::CAM_SPECTATOR_ENABLED_GLOBAL);
    if (!flag || !Readable(flag, sizeof(uint32_t))) return -1;
    uint32_t v = 0;
    memcpy(&v, flag, sizeof v);
    return v != 0 ? 1 : 0;
}

// ARCH-23 S14.10 / AUD-S14 F3. The seat's visible acceptance, and it is a STATE read, not a
// transition: "rig enabled flag %d -> %d" below is emitted only when the value moves while bound,
// and Bind() re-baselines rigEnabled on every attempt, so a rig that is already enabled the first
// time we bind -- a working seat -- prints nothing at all, and an unbound module prints nothing
// either. This says what the flag IS, this window, read here and never taken from rigEnabled, in
// three renderings so that "could not ask" is not spelled like an answer.
//
// ARCH-23 S14.11 grew it. CR15NetSpectatorCameraCS::Update has six preconditions, their failures
// are indistinguishable from outside -- seat perfect, camera frozen -- and this line names five.
// The sixth, P0b (bit 7 of +0xC40 on the singleton sub_14018AD20 returns), would take an
// unguarded call into game code, so it prints `?` on EVERY line, always, and stays diagnosable
// only by elimination. Printing it is the point: a gate nobody names is a gate nobody checks.
//
// BOTH arms carry every field. The unreadable arm loses only the word `global`; losing the other
// five numbers with it would strand the reader who most needs them.

// `?` is "could not ask" and is never spelled like an answer (BRIEF_COMMON:40).
const char* Field(char* buf, size_t n, bool known, long value) {
#ifdef W2E32_MUTATE_UNREADABLE_AS_ZERO
    _snprintf_s(buf, n, _TRUNCATE, "%ld", known ? value : 0L);
#else
    if (!known) {
        _snprintf_s(buf, n, _TRUNCATE, "?");
        return buf;
    }
    _snprintf_s(buf, n, _TRUNCATE, "%ld", value);
#endif
    return buf;
}

void PushFlightSample(const camera_control::FlightSample& s) {
    if (g.ringCount < kFlightRing) {
        g.ring[g.ringCount++] = s;
        return;
    }
    g.ring[g.ringHead] = s;
    g.ringHead = (g.ringHead + 1) % kFlightRing;
    if (g.windowOpen) g.windowTruncated = true;
}

// The ring, oldest first, into a flat array the evaluator can index. 128 copies once per marker
// pair is nothing, and it keeps EvaluateFlight a pure function over an ordered array instead of a
// reader of this module's ring arithmetic.
size_t FlattenRing(camera_control::FlightSample* out) {
    for (size_t i = 0; i < g.ringCount; ++i) {
        out[i] = g.ring[(g.ringHead + i) % kFlightRing];
    }
    return g.ringCount;
}

// Everything the line reports, taken in ONE pass so the emitter cannot mix a fresh number with a
// stale one. Each `have*` is the answer to "could the address be read", never a value.
struct Census {
    int        p0a         = -1;
    bool       counts      = false;
    uint16_t   instances   = 0;
    uint16_t   local       = 0;
    bool       haveMode    = false;
    CameraMode mode        = CameraMode::Pov;
    bool       haveFreeCam = false;
    int32_t    freeCam     = 0;
    bool       poseOk      = false;
    camera_control::ViewPoseSample sample;
};

Census ReadCensus(void* netGame) {
    Census c;
    // P0a is the RAW bit and carries no predicate: 0 is healthy, 1 is Update returning at
    // :290-293, `?` is a dereference that could not be taken.
    c.p0a    = camera_control::NetGameUpdateGateBit(netGame);
    c.counts = camera_control::BoundInstanceCounts(&c.instances, &c.local);

    IReplayCameraControl* control = camera_control::Get();
    c.haveMode    = control != nullptr && control->ObservedMode(&c.mode);
    c.haveFreeCam = control != nullptr && control->ObservedFreeCam(&c.freeCam);
    camera_control::SampleViewPose(control, &c.sample);
    c.poseOk = c.sample.read == camera::ViewPoseRead::Ok;
    return c;
}

// Four prints a second of a line whose fields change a handful of times a
// session. The read is cheap and stays; the write is spaced by `log.chatter_period_ms`.
uint64_t g_rigChatterAt = 0;

void SayRigState(void* netGame) {
    if (!replay::log::ChatterDue(g_rigChatterAt)) return;
#ifdef W2E31C_MUTATE_RIG_LINE_BEHIND_BIND
    if (!g.bound) return;
#endif
#ifdef W2E31C_MUTATE_RIG_LINE_CACHED
    const int enabled = g.rigEnabled;
#else
    const int enabled = ReadRigEnabled();
#endif
    const int bound = g.bound ? 1 : 0;

#ifdef W2E32_MUTATE_CENSUS_CACHED
    static Census cached = ReadCensus(netGame);
    const Census c = cached;
#else
    const Census c = ReadCensus(netGame);
#endif

    camera_control::FlightSample flight;
    flight.seconds = camera_control::WallSeconds(c.sample.wallTicks);
    flight.poseOk  = c.poseOk;
    flight.freeCam = c.haveFreeCam && c.freeCam != 0;
    flight.armed   = c.poseOk && c.sample.pose.integratorArmedKnown &&
                     c.sample.pose.integratorArmed != 0;
    if (c.poseOk) memcpy(flight.pos, c.sample.pose.pos, sizeof flight.pos);
    PushFlightSample(flight);

    char p0aText[16], inst[16], loc[16], md[16], fc[16], arm[16], pose[64];
    Field(p0aText, sizeof p0aText, c.p0a == 0 || c.p0a == 1, c.p0a);
    Field(inst, sizeof inst, c.counts, c.instances);
    Field(loc, sizeof loc, c.counts, c.local);
    Field(md, sizeof md, c.haveMode, static_cast<long>(c.mode));
    Field(fc, sizeof fc, c.haveFreeCam, c.freeCam != 0 ? 1 : 0);
    Field(arm, sizeof arm, c.poseOk && c.sample.pose.integratorArmedKnown,
          c.sample.pose.integratorArmed != 0 ? 1 : 0);
    if (c.poseOk) {
        _snprintf_s(pose, sizeof pose, _TRUNCATE, "%.3f,%.3f,%.3f",
                    static_cast<double>(c.sample.pose.pos[0]),
                    static_cast<double>(c.sample.pose.pos[1]),
                    static_cast<double>(c.sample.pose.pos[2]));
    } else {
        _snprintf_s(pose, sizeof pose, _TRUNCATE, "?");
    }

    if (enabled != 0 && enabled != 1) {
        g.services->log->Write(LogLevel::Info, kReplayModuleName,
                               "spectator rig unreadable, p0a=%s, p0b=?, bound=%d, instances=%s, "
                               "local=%s, mode=%s, freecam=%s, armed=%s, pose=%s",
                               p0aText, bound, inst, loc, md, fc, arm, pose);
        return;
    }
    g.services->log->Write(LogLevel::Info, kReplayModuleName,
                           "spectator rig global=%d (%s), p0a=%s, p0b=?, bound=%d, instances=%s, "
                           "local=%s, mode=%s, freecam=%s, armed=%s, pose=%s",
                           enabled, enabled != 0 ? "enabled" : "disabled", p0aText, bound, inst,
                           loc, md, fc, arm, pose);
}

// The markers are drained here rather than written at the command, so they land on the SAME clock
// and in the SAME stream as the samples they bracket -- to within one census window, which is the
// sample cadence and the finest resolution any marker over these samples could have.
void DrainFlightMarks() {
    bool begin = false;
    while (camera_control::TakeFlightMark(&begin)) {
        const double at = camera_control::WallSeconds(camera_control::WallTicksNow());
        g.services->log->Write(LogLevel::Info, kReplayModuleName, "%s",
                               camera_control::FlightMarkerText(begin));
        if (begin) {
            g.windowBegin = at;
            g.windowOpen = true;
            g.windowTruncated = false;
            continue;
        }
        g.windowEnd = at;

        camera_control::FlightSample flat[kFlightRing];
        camera_control::FlightWindow w;
        w.count        = FlattenRing(flat);
        w.samples      = flat;
        w.haveBegin    = g.windowOpen;
        w.haveEnd      = true;
        w.beginSeconds = g.windowBegin;
        w.endSeconds   = g.windowEnd;
        w.truncated    = g.windowTruncated;

        camera_control::FlightReport report;
        const camera_control::FlightVerdict verdict = camera_control::EvaluateFlight(w, &report);
        g.services->log->Write(
            LogLevel::Info, kReplayModuleName,
            "flight window verdict %s: %s; samples %llu, span %.2f s, max delta %.3f m, "
            "marks dropped %llu",
            camera_control::FlightVerdictName(verdict), report.reason,
            static_cast<unsigned long long>(report.scored), report.spanSeconds, report.maxDeltaM,
            static_cast<unsigned long long>(camera_control::FlightMarksDropped()));
        g.windowOpen = false;
    }
}

void Bind(const camera_control::RigIdentity& id, bool sayRefusal) {
    IReplayCameraControl* control = camera_control::Create(id.netGame);
    g.bound = control != nullptr;
    g.viewRefusalSaid = false;  // a new binding session gets its own first refusal
    g.identity = id;
    g.rigEnabled = ReadRigEnabled();
    if (!g.bound && !sayRefusal) return;
    if (g.bound) {
        // pose-owner is on the bound line because that is the line a launch is read for: the
        // failure this enforces looks exactly like a camera that ignores input, and the only way
        // to tell it apart afterwards is to see who held the pose. `arb` is created#/opened# for
        // the process-wide section: exactly one image creates it, so two images both reporting
        // `created` is the AUDIT_E E3 defect back again, which `conflicts 0` alone cannot show.
        //
        // What a LAUNCH can actually show is narrower than that, and saying so is the point
        // (AUDIT_I finding 3): bootstrap.cpp:423 refuses the single-instance claim to all but one
        // image and :450 starts WorkerMain only past it, so exactly one image's modules ever
        // initialise and only one claim site runs. So a shipped launch cannot produce a
        // cross-image conflict at all -- that is proved offline, by the two- and three-image cases
        // in tests/camera/writepoint/test_poseowner_images.cpp. What a launch DOES witness is the
        // invariant that keeps it safe: `arb created#1` on every line, and never an `opened#`,
        // because an `opened#` here would mean a second image initialised and instance exclusivity
        // had failed. `rel-refused` is the same shape for the release side: non-zero means some
        // image tried to free a token it did not hold.
        // ★ The noun in the two lines below is "level space" because the pointer IS the level
        // space (+0x2B10); ARCH-12 R5 reserves bare "game space" against it. tests/replay/
        // observer_rows.h parses these lines, so emitter and parser move together (ARCH-12 P-C)
        // -- W2-R P9F (17cb72b) drives BOTH spellings through its reader and requires identical
        // rows, which is what let this rename land on the emitter side alone. "rig" is the
        // lookup's answer rather than the +0x2B10 slot -- what the old line was really printing.
        g.services->log->Write(LogLevel::Info, kReplayModuleName,
                               "camera control bound: rig %p on level space %p (netGame %p, systems "
                               "at %llx); rig enabled flag %d; pose-owner: %s, conflicts %llu, "
                               "arb %s, rel-refused %llu, transfers %llu, xfer-refused %llu; modes drive from the panel and `cam`",
                               camera_control::BoundSpectatorCs(), id.levelSpace, id.netGame,
                               static_cast<unsigned long long>(id.tableBase), g.rigEnabled,
                               camera::PoseOwnerName(camera::PoseOwnerHeld()),
                               static_cast<unsigned long long>(camera::PoseOwnerConflicts()),
                               camera::PoseOwnerArbiter(),
                               static_cast<unsigned long long>(camera::PoseOwnerReleaseRefusals()),
                               static_cast<unsigned long long>(camera::PoseOwnerTransfers()),
                               static_cast<unsigned long long>(
                                   camera::PoseOwnerTransferRefusals()));
    } else {
        g.services->log->Write(LogLevel::Warn, kReplayModuleName,
                               "camera control refused rig %p on level space %p (netGame %p): %s; "
                               "pose-owner: %s, conflicts %llu, arb %s, rel-refused %llu, transfers %llu, xfer-refused %llu",
                               camera_control::BoundSpectatorCs(), id.levelSpace, id.netGame,
                               camera_control::Diagnostic(),
                               camera::PoseOwnerName(camera::PoseOwnerHeld()),
                               static_cast<unsigned long long>(camera::PoseOwnerConflicts()),
                               camera::PoseOwnerArbiter(),
                               static_cast<unsigned long long>(camera::PoseOwnerReleaseRefusals()),
                               static_cast<unsigned long long>(camera::PoseOwnerTransfers()),
                               static_cast<unsigned long long>(
                                   camera::PoseOwnerTransferRefusals()));
    }
}

// `why` is captured before Release(), which overwrites the control's own diagnostic with
// "released".
void Unbind(const char* why) {
    camera_control::Release();  // this clears the view-read latch the line above publishes
    g.viewRefusalSaid = false;
    g.bound = false;
    g.services->log->Write(LogLevel::Info, kReplayModuleName,
                           "camera control released (netGame %p, level space %p): %s",
                           g.identity.netGame, g.identity.levelSpace, why);
}

// AUD-F3H F3H-2. The differential's CS-space arm is re-asked once per bind epoch. sub_14017A050
// writes NETGAME.SPECTATORCAM_CS_OFF and only afterwards NETGAME.CS_SPACE_OFF, so a bind made
// inside that window records the could-not-ask word "CsSpaceAbsent" -- and csSpace is outside the
// rig identity by ARCH-12 R4, so `changed` stays false and nothing else would ever ask again. This
// re-asks WITHOUT tearing down a working bind; it releases only when the arm turns into a refusal,
// which is R2 saying the object to bind is no longer decided. One second, never per frame, because
// the arm can call CS.LOOKUP_BY_SYMBOL and that lookup sorts in place (ARCH-10 S7).
void ReviseBoundDifferential(const camera_control::RigIdentity& id) {
    const char* before = camera_control::DifferentialWord();
    if (!camera_control::ReviseDifferential(id)) {
        g.services->log->Write(LogLevel::Warn, kReplayModuleName,
                               "camera control differential now refuses on level space %p "
                               "(netGame %p, CS space %p): %s",
                               id.levelSpace, id.netGame, id.csSpace,
                               camera_control::Diagnostic());
        Unbind("differential refused after the bind");
        return;
    }
    const char* now = camera_control::DifferentialWord();
    if (before && now && strcmp(before, now) != 0) {
        g.services->log->Write(LogLevel::Info, kReplayModuleName,
                               "camera control differential %s -> %s on level space %p "
                               "(CS space %p)", before, now, id.levelSpace, id.csSpace);
    }
    g.identity.csSpace = id.csSpace;
}

// The one production install of W1-H's write point, here for the same reason the view-state read
// is: camera_control.cpp is linked into four images that do not carry camera_writepoint.cpp, so it
// reaches camera::Create through a pointer or not at all. test_camera_module asserts this install
// happened -- an uninstalled write point would make every `cam move` refuse forever.
const char* WritePointWhy() {
    camera::Stats stats;
    camera::GetStats(&stats);
    return camera::RefusalName(stats.refusal);
}

const camera_control::WritePointSeam kWritePoint = {&camera::Create, &camera::Release,
                                                    &WritePointWhy,
                                                    &camera::ReleaseKeepingToken};

bool Init(ReplayServices* s) {
    // The one production install of W1-H's view-state pose read, here rather than inside
    // camera_control.cpp because that file is linked into images the write point is not in. The
    // camera DLL carries both, and test_camera_module asserts this install actually happened --
    // an uninstalled reader would make GetViewPose refuse on the free camera's arm forever.
    camera_control::SetViewStateReader(&camera::ReadViewStatePose);
    camera_control::SetWritePoint(&kWritePoint);
    // ARCH-19 R3's subject-pose seam is installed HERE, through
    // camera_control::SetSubjectPose(&seam), by the same reasoning as the two installs above:
    // camera_control.cpp is linked into four images that carry no entity module, so it reaches
    // TeamOf/TeamSlotOf/PlayerOf/PhysOf/ReadBody through a pointer or not at all. The composer
    // itself is W1-E's -- it is the only packet that already links identity_access AND entity, and
    // a second roster scan here would be the two-surfaces-disagree defect. Until it publishes one,
    // nothing is installed and `cam look at <n>` refuses NoSeam by name; it never aims at a point
    // this module invented. The derivation the composer needs is camera_control::PackSelector,
    // which is published for exactly that reason and exists once.
    REPLAY_REQUIRE(s, log);
    g = State();
    g.services = s;
    camera_control::Release();
    s->log->Write(LogLevel::Info, kReplayModuleName,
                  "camera control waits for a constructed game space carrying the spectator rig");
    return true;
}

void Tick(const TickInfo& info) {
    // First statement of the tick, above every branch and every return this function has, because
    // the one thing a reader must be able to tell apart is "the rig is off" from "nobody said".
    DrainFlightMarks();
    if (g.rigCensus++ % kRigCensusEveryTicks == 0) SayRigState(info.netGame);

    // AUDIT_L1 finding 3. GetViewPose's refusal sentence had no operator surface at all: its only
    // reader was a static printed by the bind-REFUSAL branch, which cannot fire in a session that
    // is asking for a pose. It goes on the same [camera] line that carries pose-owner/arb, which
    // is the line a launch is read for -- once per binding session, because the ask is per frame.
    if (!g.viewRefusalSaid && camera_control::ViewReadRefusals() != 0) {
        g.viewRefusalSaid = true;
        g.services->log->Write(LogLevel::Warn, kReplayModuleName,
                               "camera control view read refused: %s; refusals %llu; pose-owner: "
                               "%s, conflicts %llu, arb %s, rel-refused %llu, transfers %llu, xfer-refused %llu",
                               camera_control::ViewReadRefusal(),
                               camera_control::ViewReadRefusals(),
                               camera::PoseOwnerName(camera::PoseOwnerHeld()),
                               static_cast<unsigned long long>(camera::PoseOwnerConflicts()),
                               camera::PoseOwnerArbiter(),
                               static_cast<unsigned long long>(camera::PoseOwnerReleaseRefusals()),
                               static_cast<unsigned long long>(camera::PoseOwnerTransfers()),
                               static_cast<unsigned long long>(
                                   camera::PoseOwnerTransferRefusals()));
    }

    camera_control::RigIdentity id;
    const camera_control::RigProbe probe = camera_control::Probe(info.netGame, &id);

    if (probe == camera_control::RigProbe::Ready) {
        const bool changed = g.identity != id;
        if (g.bound && changed) {
            g.services->log->Write(LogLevel::Info, kReplayModuleName,
                                   "level space changed %p -> %p (systems %llx -> %llx); "
                                   "re-binding",
                                   g.identity.levelSpace, id.levelSpace,
                                   static_cast<unsigned long long>(g.identity.tableBase),
                                   static_cast<unsigned long long>(id.tableBase));
        }
        if (changed) {
            g.retries = 0;
            g.epoch   = 0;
            Bind(id, true);
        } else if (g.bound && ++g.epoch % kRetryEveryTicks == 0) {
            ReviseBoundDifferential(id);
        } else if (!g.bound && ++g.retries % kRetryEveryTicks == 0) {
            // A refusal on an unchanged identity can be the load window (the rig's instance
            // array fills after the rig is installed), so it is retried -- at one second, not
            // every frame, because a site-guard refusal logs each time it is asked.
            Bind(id, g.retries % kLogEveryDeferrals == 0);
        }
        const int enabled = ReadRigEnabled();
        if (g.bound && enabled != g.rigEnabled) {
            g.services->log->Write(LogLevel::Info, kReplayModuleName, "rig enabled flag %d -> %d",
                                   g.rigEnabled, enabled);
            g.rigEnabled = enabled;
        }
        g.deferrals = 0;
        g.lastProbe = probe;
        return;
    }

    if (g.bound) {
        Unbind(camera_control::RigProbeName(probe));
        g.deferrals = 0;
    }

    // A space that exists but does not validate yet is the load window; say so once per change and
    // then sparsely, and never latch -- the next tick asks again.
    const bool changed = probe != g.lastProbe || g.identity != id;
    if (changed) g.deferrals = 0;
    // Gated on the netGame, not on the space: a null level space IS the L3/L4 state (no map
    // loaded), and a deferral that says nothing about the commonest reason to defer is a silence a
    // reader has to guess at.
    if (id.netGame && (changed || ++g.deferrals % kLogEveryDeferrals == 0)) {
        g.services->log->Write(LogLevel::Info, kReplayModuleName,
                               "level space %p (netGame %p) deferred: %s", id.levelSpace, id.netGame,
                               camera_control::RigProbeName(probe));
    }
    g.identity = id;
    g.lastProbe = probe;
}

void Shutdown() {
    camera_control::Release();
    g = State();
}

}  // namespace

void SetRigFlagForTest(const void* flag) { g_rigFlagForTest = flag; }

}  // namespace camera_module
}  // namespace replay

REPLAY_MODULE("camera", 30, ::replay::camera_module::Init, ::replay::camera_module::Tick,
              ::replay::camera_module::Shutdown);
