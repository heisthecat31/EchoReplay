#pragma once

// W2-G, ARCH-19 E1/E3. The decisions behind `cam move`, `cam look` and `cam release`, written as
// pure functions for the same reason seekgoto.h and loadgate.h are: `tests/control` links
// `replay_control_core`, which does NOT carry dispatch.cpp, so a decision left inside a dispatcher
// arm is a decision the offline suite cannot sweep. The two virtual calls themselves stay in
// dispatch.cpp -- ARCH-19 E4 freezes an anchored `->CommandPose(` / `->ReleasePose(` grep over
// src/**.cpp that must name that file, and a call site hidden in a header would not satisfy it.

#include <math.h>
#include <stdio.h>

#include "camera/camera_control.h"
#include "control/command.h"

namespace replay {
namespace control {

// The parser's flags, carried across unchanged. FLAGS AND NEVER SENTINELS (command.h:150,
// camera_control.h:176): (0,0,0) is a real absolute destination and a real no-op delta, and
// `cam move 0 0 0` is not an error.
inline void PoseFromCommand(const Command& cmd, CommandedPose* out) {
    *out = CommandedPose();
    out->absolute = cmd.camAbsolute;
    if (cmd.id == CommandId::CamMove) {
        out->setPosition = true;
        out->pos[0] = cmd.camVec[0];
        out->pos[1] = cmd.camVec[1];
        out->pos[2] = cmd.camVec[2];
        return;
    }
    if (cmd.id != CommandId::CamLook) {
        return;
    }
    out->setOrientation = true;
    if (cmd.camLookAt) {
        // ARCH-19 R6's true/true row: an aim resolved from a subject is an ABSOLUTE facing, and
        // the current facing is not an input to it. The selector is the parser's, bounded there.
        out->lookAt         = true;
        out->lookAtSelector = static_cast<int32_t>(cmd.whole);
        out->absolute       = true;
        return;
    }
    // Degrees stay degrees. The quaternion is built by the ONE pair (camera_control::BuildFacing)
    // on the far side of the interface; a dispatcher that pre-composed one here would be the second
    // convention ARCH-19 E2 exists to prevent.
    out->yawDeg   = cmd.camVec[0];
    out->pitchDeg = cmd.camVec[1];
}

// THREE OUTCOMES, NOT TWO (ARCH-19 E3). A command that was accepted and whose landing could not be
// read back is neither "it worked, here is where you are" nor "it refused": the write landed and we
// cannot say where. A bool would make it indistinguishable from the first, which is the shape that
// lets a probe score a landing it never read.
enum class CamPoseOutcome {
    Refused,              // CommandPose returned false. LastRefusal() names why.
    Landed,               // accepted, and GetViewPose came back Ok. The sentence carries the pose.
    CommandedNotReadBack, // accepted, and the read-back did not. The Result stays TRUE -- the
                          // command did what it said -- and the sentence carries NO pose at all.
};

inline CamPoseOutcome ClassifyCamPose(bool commandAccepted, camera::ViewPoseRead readBack) {
    if (!commandAccepted) {
        return CamPoseOutcome::Refused;
    }
    return readBack == camera::ViewPoseRead::Ok ? CamPoseOutcome::Landed
                                                : CamPoseOutcome::CommandedNotReadBack;
}

// Only Refused is a failed command.
inline bool CamPoseOk(CamPoseOutcome outcome) { return outcome != CamPoseOutcome::Refused; }

// The read-back sentence: what the engine LANDED on, never what was asked for (dispatch.cpp's own
// rule for `cam`). `verb` is "cam move" or "cam look".
//
// Where the landed facing is vertical the two angles are replaced by `facing vertical` and NEVER
// printed as 0/0, which is a different pose: DecomposeFacing leaves its out-parameters untouched on
// a refusal precisely so no caller can score its zeros as a facing.
inline bool LandedFacing(const ViewPose& pose, float* yawDeg, float* pitchDeg) {
    float yaw   = 0.0f;
    float pitch = 0.0f;
    const bool haveFacing = camera_control::DecomposeFacing(pose.quat, &yaw, &pitch);
    // A negative zero prints as `-0.0`, which reads as a different number from `0.0` to
    // whoever is looking at the line. The comparison is true for both zeros and the store
    // leaves a positive one. `cam look to 0 0` lands here on every identity facing.
    if (yaw == 0.0f) yaw = 0.0f;
    if (pitch == 0.0f) pitch = 0.0f;
    *yawDeg   = yaw;
    *pitchDeg = pitch;
    return haveFacing;
}

inline void FormatCamPoseLanded(const char* verb, const ViewPose& pose,
                                PoseRegime regime, char* out, size_t cap) {
    float yaw   = 0.0f;
    float pitch = 0.0f;
    const bool haveFacing = LandedFacing(pose, &yaw, &pitch);
    if (haveFacing) {
        snprintf(out, cap, "%s: at (%.3f, %.3f, %.3f), yaw %.1f pitch %.1f, regime %s, source %s",
                 verb, pose.pos[0], pose.pos[1], pose.pos[2], yaw, pitch,
                 PoseRegimeName(regime),
                 ViewPoseSourceName(pose.source));
        return;
    }
    snprintf(out, cap, "%s: at (%.3f, %.3f, %.3f), facing vertical, regime %s, source %s", verb,
             pose.pos[0], pose.pos[1], pose.pos[2], PoseRegimeName(regime),
             ViewPoseSourceName(pose.source));
}

// `cam look at <n>` landed. Same three outcomes, same read-back, one extra fact: WHO the camera
// ended up aimed at. The subject is printed from the selector that was handed to CommandPose --
// not from the parsed token and not from the verb -- so a selector that shifted anywhere between
// the parser and the interface shows up as two different numbers in one sentence.
//
// The angles are the LANDED facing, decomposed by the one pair, exactly as the sibling arm does;
// a sentence that echoed the aim we asked for would be reporting arithmetic rather than a pose.
inline void FormatCamLookAtLanded(const char* verb, int32_t selector, const ViewPose& pose,
                                  PoseRegime regime, char* out, size_t cap) {
    float      yaw        = 0.0f;
    float      pitch      = 0.0f;
    const bool haveFacing = LandedFacing(pose, &yaw, &pitch);
    if (haveFacing) {
        snprintf(out, cap,
                 "%s: aimed at player %d from (%.3f, %.3f, %.3f), yaw %.1f pitch %.1f, regime %s, "
                 "source %s",
                 verb, static_cast<int>(selector), pose.pos[0], pose.pos[1], pose.pos[2], yaw,
                 pitch, PoseRegimeName(regime), ViewPoseSourceName(pose.source));
        return;
    }
    // Directly above or below the subject: a real aim with no yaw. Never printed as yaw 0.
    snprintf(out, cap,
             "%s: aimed at player %d from (%.3f, %.3f, %.3f), facing vertical, regime %s, "
             "source %s",
             verb, static_cast<int>(selector), pose.pos[0], pose.pos[1], pose.pos[2],
             PoseRegimeName(regime), ViewPoseSourceName(pose.source));
}

// The third outcome, and it must not be dressed as either of the other two.
inline void FormatCamPoseNotReadBack(const char* verb, camera::ViewPoseRead readBack,
                                     const char* readRefusal, char* out, size_t cap) {
    snprintf(out, cap, "%s: commanded, but the pose could not be read back (%s) -- %s", verb,
             camera::ViewPoseReadName(readBack),
             readRefusal != nullptr && readRefusal[0] != 0 ? readRefusal : "no sentence");
}

// Every refusal is a NAMED value from the enum, never "it refused": ControlRefusalName is what a
// P17-G probe row scores on.
inline void FormatCamPoseRefused(const char* verb, ControlRefusal refusal,
                                 const char* diagnostic, char* out, size_t cap) {
    snprintf(out, cap, "%s refused: %s -- %s", verb, ControlRefusalName(refusal),
             diagnostic != nullptr && diagnostic[0] != 0 ? diagnostic : "no sentence");
}

// `cam release` is a success that may have done nothing, and says which. Regime is read on both
// sides of the call because "handed back" and "was never ours" are different facts about the
// session, and a session with no rig bound is a third.
inline void FormatCamRelease(PoseRegime before, PoseRegime after,
                             char* out, size_t cap) {
    const char* const afterWord = PoseRegimeName(after);
    if (before == PoseRegime::Commanded) {
        snprintf(out, cap, "cam release: the pose is back with the stock free camera, regime %s",
                 afterWord);
        return;
    }
    if (before == PoseRegime::Unavailable) {
        snprintf(out, cap,
                 "cam release: no rig holds the pose, so there was nothing to hand back, regime %s",
                 afterWord);
        return;
    }
    snprintf(out, cap,
             "cam release: the pose was already with the stock free camera, regime %s", afterWord);
}

// ---- ARCH-23 S14.11, THE FLIGHT WINDOW --------------------------------------------------------
// The owner brackets his own free flight -- `cam flight begin`, fly, `cam flight end` -- and the
// run scores the pose samples the pair encloses. Five outcomes, because "no window was open" and
// "the camera module installed no writer" are different facts and only the second is a failure: a
// marker that WAS stamped is a marker the scorer will find, whether or not this side thought a
// window was open. A second `begin` re-stamps rather than refusing, so a forgotten `end` costs the
// owner nothing but the earlier marker.
enum class FlightMarkOutcome : uint8_t {
    Opened,
    Reopened,
    Closed,
    ClosedUnopened,
};

inline FlightMarkOutcome ClassifyFlightMark(bool begin, bool windowOpen) {
    if (begin) {
        return windowOpen ? FlightMarkOutcome::Reopened : FlightMarkOutcome::Opened;
    }
    return windowOpen ? FlightMarkOutcome::Closed : FlightMarkOutcome::ClosedUnopened;
}

// What this side believes after the call.
inline bool FlightWindowAfter(FlightMarkOutcome o) {
    return o == FlightMarkOutcome::Opened || o == FlightMarkOutcome::Reopened;
}

inline void FormatFlightMark(FlightMarkOutcome o, char* out, size_t cap) {
    switch (o) {
        case FlightMarkOutcome::Opened:
            snprintf(out, cap,
                     "cam flight begin: the window is open -- fly, then `cam flight end`");
            return;
        case FlightMarkOutcome::Reopened:
            snprintf(out, cap,
                     "cam flight begin: a window was already open, so this one RE-STAMPED it and "
                     "the later marker is the one that counts");
            return;
        case FlightMarkOutcome::Closed:
            snprintf(out, cap, "cam flight end: the window is closed");
            return;
        case FlightMarkOutcome::ClosedUnopened:
            snprintf(out, cap,
                     "cam flight end: no window was open, so the marker is stamped but brackets "
                     "nothing");
            return;
    }
    snprintf(out, cap, "cam flight: unhandled outcome");
}

}  // namespace control
}  // namespace replay
