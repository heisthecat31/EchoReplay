# OWNERSHIP: this directory is W2-E's. The CONTENT below is still
# W1-H's -- the write-point assertion, the viewstate+0x9C re-arm and S8.1.4's flag matrix,
# which now cover S8.1.7's FALLBACK rather than the primary path. A transfer, recorded, not
# a deletion: round 10 found this path listed three times in the ownership table (twice
# W1-H, once W2-E), and two owners for one path is a pre-commit hook that would approve
# either packet writing here.
#
# W1-H's offline half: every probe criterion driven with both a passing and a failing input, a
# known-answer control that re-derives camera_writepoint_facts.h from the shipping echovr.exe with a
# second PE reader, and the reporting proof -- that the probe still files a verdict when the
# bootstrap refuses its instance claim. It reads the exe as a file and never launches it, so it runs
# in T0 with the other offline checks.
#
# This directory closes the ownership gap W1-H reported in its first spike: the packet had no tests/
# path, so its criteria had no runnable assertion and F5's defect had nowhere to be caught.
#
# W2-E. test_camera_module.cpp is the wiring check, in the same binary because the gate registers one
# check per directory: the real module translation unit through the real modreg, driven by a fake
# netGame, a fake game space and a fake rig, with the two engine routines behind the test seam. It
# is the assertion DIAG-WIRING gap 3 said nothing had -- that camera_control::Get() is non-null
# after Init + tick, which is the predicate the dispatcher and the panel gate every camera command
# on. replay_control_core supplies the same Parse / SpecFor the dispatcher uses for `cam`.

replay_add_test(NAME camera
    SOURCES test_camera.cpp
            test_camera_module.cpp
            ${CMAKE_SOURCE_DIR}/src/camera/camera_module.cpp
            ${CMAKE_SOURCE_DIR}/src/camera/camera_control.cpp
    LIBS replay_bootstrap replay_control_core replay_camera_writepoint)

# The Release() caller census (VR2-3) is a property of the source tree, so the case that asserts it
# has to be told where the tree is.
target_compile_definitions(test_camera PRIVATE REPLAY_REPO_ROOT="${CMAKE_SOURCE_DIR}")
add_dependencies(test_camera probe_camera probe_camera_nofiler camera_probehost probe_teamgate)

# The in-game half: the shipped bootstrap, the shipped tick and the shipped IReplayCamera, plus one
# module that measures what the engine did with them. Queued through runs\queue\W1-H.probe.json --
# this packet never launches the game. The durable copy of the queue descriptor lives here because
# runs\ is uncommitted:
#
#     copy tests\camera\W1-H.probe.json runs\queue\W1-H.probe.json
add_library(probe_camera SHARED probe_camera.cpp)
target_link_libraries(probe_camera PRIVATE replay_camera_writepoint replay_tick replay_flags
                      replay_bootstrap minhook ws2_32)
target_include_directories(probe_camera PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(probe_camera PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# ★ The mutation. Identical in every respect except that the static-initialiser filer is compiled
# out and the verdict is filed from a REPLAY_MODULE init instead -- the OLD shape, not merely the
# new one deleted. The offline proof asserts this build goes SILENT under the same refusal the real
# probe survives; a control that has only ever agreed has not been shown able to disagree.
add_library(probe_camera_nofiler SHARED probe_camera.cpp)
target_compile_definitions(probe_camera_nofiler PRIVATE W1H_NO_STATIC_FILER=1)
target_link_libraries(probe_camera_nofiler PRIVATE replay_camera_writepoint replay_tick replay_flags
                      replay_bootstrap minhook ws2_32)
target_include_directories(probe_camera_nofiler PRIVATE ${CMAKE_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(probe_camera_nofiler PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# Loads probe DLLs into one process the way EchoLoader does, so the instance-claim refusal is
# reproducible offline instead of costing a launch to rediscover.
add_executable(camera_probehost probehost.cpp)
target_link_libraries(camera_probehost PRIVATE replay_flags)

configure_file(W1-H.probe.json ${CMAKE_BINARY_DIR}/W1-H.probe.json COPYONLY)

# W2-E. The contract check for IReplayCameraControl: a translation unit that consumes the seam
# exactly as W2-F's panel and W2-G's console will, so the interface is proven CONSUMABLE and not
# merely well-formed. It odr-uses kPlayerSelectorMax through a const reference, which is the use
# that a `static const` (rather than constexpr) member would fail to link -- the failing input for
# this check is a real defect it already caught once.
add_executable(camera_control_consumer camera_control_consumer.cpp
    ${CMAKE_SOURCE_DIR}/src/camera/camera_control.cpp)
target_link_libraries(camera_control_consumer PRIVATE replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(camera_control_consumer PRIVATE ${CMAKE_SOURCE_DIR}/src)

# W2-E. The in-game half: does the spectator-camera enable gate come true on its own?
# Queued through runs\queue\W2-E.probe.json; this packet never launches the game. The durable copy
# of the queue descriptor lives here because runs\ is uncommitted:
#
#     copy tests\camera\W2-E.probe.json runs\queue\W2-E.probe.json
# ★ camera_writepoint.cpp reaches this target -- now by linking replay_camera_writepoint rather
# than by a hand list -- because the probe ASKS camera::detail::ResolveSpectatorCs
# for the spectator-CS resolve instead of carrying a second copy of it. The
# probe and the shipping Create() now run the same policy, and probe_teamgate_outcome.h -- the row's
# outcome mapping -- is compiled into test_camera too, so the mapping is mutable by a test.
add_library(probe_teamgate SHARED probe_teamgate.cpp)
target_link_libraries(probe_teamgate PRIVATE replay_camera_writepoint replay_flags replay_bootstrap
                      minhook ws2_32)
target_include_directories(probe_teamgate PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(probe_teamgate PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
configure_file(W2-E.probe.json ${CMAKE_BINARY_DIR}/W2-E.probe.json COPYONLY)

