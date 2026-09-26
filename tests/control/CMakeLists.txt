# W2-G. The offline half of the control gate.
#
# T0: nothing here reads echovr.exe and nothing needs a game. What it does NOT cover -- registering
# an HTTP route, and any dispatch that reaches IReplayTransport or replay::ui -- is named at the
# head of test_control.cpp and parked in runs/queue/payload/W2-G.json, so a green run here is not
# read as the control surface working.
#
# ★ ONE THING THAT USED TO BE "NEEDS A GAME" AND IS NOT: the sidecar's file layer, which now runs
# against a real temporary directory in test_notes_io.cpp. It was untested on the strength of the
# source library being called "pure", and it was destroying the owner's bookmarks.
#
# ★★ AND ONE THING THAT IS DELIBERATELY NOT HERE. A control_closeprobe target used to spawn a child
# that allocated a REAL console window to measure what a console close does to its process. The
# owner has ruled the close behaviour is not a defect ("I know its attatched"), so the requirement
# is gone -- and the probe was painting windows on his desktop while a mutation loop re-ran this
# binary. Nothing in this directory may open a window again. The measurements it produced are kept
# in docs/spikes/W2-G.md.
# camera_control.cpp is compiled in for ONE assertion, which ARCH-17 S3.1 requires to live here:
# that `move`, `look` and `release` -- the sub-words the commanded-pose family claims ahead of the
# mode word -- are not engine mode names. It has to ask the engine's OWN ModeFromName; a second
# hand-written list of mode names is the exact defect camera_control.h names. camera_writepoint.cpp
# comes with it -- through replay_camera_writepoint, W1-H's object library, rather than a hand
# list -- because camera_control.cpp links against it. Neither is initialised here and
# nothing in this gate reads a game.
replay_add_test(NAME control
    SOURCES test_control.cpp test_notes_io.cpp
            ${CMAKE_SOURCE_DIR}/src/camera/camera_control.cpp
    LIBS replay_bootstrap replay_control_core replay_camera_writepoint)

# P17-E's S9 case reads src/control/dispatch.cpp and src/ui/panel.cpp out of the tree: the arms
# themselves are on the echoreplay_core target and cannot be linked here, so the wiring is asserted from
# the source. Same mechanism tests/camera and tests/replay already use.
target_compile_definitions(test_control PRIVATE REPLAY_REPO_ROOT="${CMAKE_SOURCE_DIR}")
