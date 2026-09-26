# W2-B. The offline half of the playback gate: the transport arithmetic and the resolver, neither of
# which touches engine memory, so both run on a machine with no game.
#
# T0 rather than T1: no file is written and no binary is read, so this belongs in the tier that gates
# every build rather than the one that needs artifacts.
replay_add_script_check(NAME playback SCRIPT check.ps1)

add_executable(playback_selftest test_playback.cpp test_netvarapply.cpp test_netvarsite.cpp test_netvarhud.cpp test_netvarguard.cpp test_viewer_role.cpp
                               test_recorded_loadout.cpp test_track_map.cpp test_wire_fixture.cpp
                               test_scoregate.cpp test_disc_bind.cpp test_disc_possession.cpp
                               test_entry_gametype.cpp test_netvarstamp.cpp
                               # INSTRUMENTS-1: the grab-target remap and its log's per-track
                               # throttle, and the real-capture disc drive the axis defect hid.
                               test_grip_hold.cpp test_disc_motion.cpp test_action_cadence.cpp
                               # The stat feed, decode through feed.
                               test_statsfeed.cpp
                               # The goal feed, decode through edge and level.
                               test_goalfeed.cpp
                               # The stun feed, decode through edge and level.
                               test_stunfeed.cpp
                               # The component-event feed, decode through the raise.
                               test_componentfeed.cpp
                               # RECORDER-4: the keyframe netvar snapshot, writer through primer.
                               test_netvarsnapshot.cpp
                               # WINDOW-DECODE-1: the off-tick window decode, worker against tick.
                               test_windowprefetch.cpp
                               # SMOOTH-1: motion smoothing for slow-motion replay.
                               test_smoothing.cpp test_smoothing_off.cpp
                               # RELOAD-6: the reload level-cycle sequence.
                               test_reload_cycle.cpp
                               # DESIGN-HOST-SYNC P1/P4: the clock pin and the disc shell feed.
                               test_clockpin.cpp test_shellfeed.cpp)
# replay_identity_access because viewer_role.cpp's V3 takes PawnProvision::EngineCallLatch and
# writes PawnProvision::MarkHop around the engine call -- the ONE latch every engine call issued
# from the stage-0 tick shares (pawn_provision.h:699). replay_playback is STATIC, so only a target
# that actually references viewer_role.cpp pulls its object and needs those symbols; that is this
# one, echoreplay_core and playback_armtest, and the latter two already carry pawn_provision.cpp.
target_link_libraries(playback_selftest PRIVATE replay_flags doctest_main replay_playback replay_capture replay_demofile replay_identity_access)
target_include_directories(playback_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The real capture L3-RUN-5 played, beside the test that reads it. A path baked in at configure
# time rather than a working-directory guess: a case that cannot find its file must fail, not skip.
target_compile_definitions(playback_selftest PRIVATE
    REPLAY_PLAYBACK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    REPLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    # test_track_map.cpp's solo control. runs/ is machine-local, so the case skips by name without it.
    REPLAY_TRACKMAP_SOLO_FIXTURE="${CMAKE_SOURCE_DIR}/runs/replays/W2-G-fixture-gtfull.echodemo")

# The exact path, written by CMake, so check.ps1 does not become a second source of truth about where
# the binary lives -- W1-A's pattern in tests/o1.
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/playback_selftest_path.txt CONTENT "$<TARGET_FILE:playback_selftest>")

# ★★★ THE ARM. playback_selftest links the LIBRARY, so nothing in it can observe whether
# playback_module.cpp exists, let alone what its Init calls -- which is how audio::ArmAudioResidue and
# residue::ArmPlaybackResidue each spent a wave written, verified and with no caller anywhere.
#
# This target compiles the three MODULE translation units instead, so modreg::InitAll runs W2-H's,
# W2-B's and W3-A's real inits in the real order and the battery under test is the one the DLL builds.
# Naming another packet's .cpp from here is a reference, not an edit: the same shape src/audio's list
# uses for src/engine/audiostate.cpp, and it keeps the wiring check inside the packet that owns the
# wiring.
add_executable(playback_armtest
    test_playback_arm.cpp
    ${CMAKE_SOURCE_DIR}/src/playback/core/playback_module.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/audio_module.cpp
    ${CMAKE_SOURCE_DIR}/src/playback/residue/residue_module.cpp
    # Load dresses the roster through W2-C's DressRoster, so the module TU pulls the dresser and
    # the IIdentity free functions it reads refusals through -- the same two files tests/identity
    # compiles, because neither is in a library.
    ${CMAKE_SOURCE_DIR}/src/identity/dresser.cpp)
# W1-E's module, so the entity and pose surfaces playback binds are published by the real InitAll at
# order 15 -- and the IIdentity free functions the dresser reads refusals through come with it.
# replay_entity_module is that module's own object list, kept in src/engine/cmake/W1-E.cmake by the
# packet that owns it. A hand-written copy of that list is what broke the build at eb31bd4: the module
# gained entity_subject.cpp and a dependency on W2-E's camera_control.cpp, and a list living here
# cannot follow a change made there.
# replay_wiretap (W1-L.cmake) because W2-B's Tick calls WiretapCreate directly: this target compiles
# playback_module.cpp itself rather than linking replay_playback's copy, so the tap it now calls has
# to come with it. An OBJECT library, so the self-registrar in wiretap.cpp survives the link.
target_link_libraries(playback_armtest PRIVATE replay_flags doctest_main replay_playback replay_audio
                      replay_residue replay_bootstrap replay_capture replay_demofile
                      replay_entity_module replay_wiretap)
target_include_directories(playback_armtest PRIVATE ${CMAKE_SOURCE_DIR}/src)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/playback_armtest_path.txt CONTENT "$<TARGET_FILE:playback_armtest>")

# ★ The in-match half. A probe DLL, not a check: the test harness installs it for one launch and
# reads the verdict it writes. It answers the two parked reads that are passive engine reads; the
# other four are blocked on things no probe can supply, and W2-B.probe.json says which and why.
#
# replay_bootstrap is named directly because this file CALLS it -- buildguard::MappedAddress is the
# one rebasing point in the tree, and every site the probe reads comes from facts_generated.h through
# it. src/plugin/dllmain.cpp carries the same bootstrap the shipped DLL uses, so the probe attaches
# the way the product does rather than through a path only it takes.
add_library(probe_playback SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    probe_playback.cpp)
target_link_libraries(probe_playback PRIVATE replay_flags replay_bootstrap)
target_include_directories(probe_playback PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_playback PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# The queue descriptor lives here as well as in runs\queue\ because runs\ is deliberately
# uncommitted (W1-J: artifacts are machine-local evidence, not source). A probe request is not
# evidence, it is this packet's statement of what it needs the sweep to execute, so the durable copy
# is here and is copied into runs\queue\ before a sweep:
#
#     copy tests\playback\W2-B.probe.json runs\queue\W2-B.probe.json
configure_file(W2-B.probe.json ${CMAKE_BINARY_DIR}/W2-B.probe.json COPYONLY)
