# W2-D. The offline half of the clock gate: the classification and the rate meter, neither of which
# touches engine memory, so both run on a machine with no game.
#
# T0 rather than T1: no file is written and no binary is read by the native suite, so this belongs
# in the tier that gates every build. check.ps1's factcheck arm reads echovr.exe when it is present
# and reports NOT-RUN rather than failing when it is not.
replay_add_script_check(NAME clock SCRIPT check.ps1)

# The policy translation unit only, so the offline test needs no Windows, no engine and no facts.
add_executable(clock_selftest test_clock.cpp ${CMAKE_SOURCE_DIR}/src/engine/clock_policy.cpp)
target_link_libraries(clock_selftest PRIVATE replay_flags doctest_main)
target_include_directories(clock_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The exact path, written by CMake, so check.ps1 does not become a second source of truth about
# where the binary lives -- W1-A's pattern in tests/o1, W2-B's in tests/playback.
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/clock_selftest_path.txt CONTENT "$<TARGET_FILE:clock_selftest>")

# The in-game half: the collector for the seven parked reads in runs/queue/payload/W2-D.json.
# Queued through runs\queue\W2-D.probe.json -- this packet never launches the game. The durable copy
# of the queue descriptor lives here because runs\ is uncommitted:
#
#     copy tests\clock\W2-D.probe.json runs\queue\W2-D.probe.json
#
# It links the shipped bootstrap and the shipped clock, and NOT the transport: with
# ReplayServices::transport null the clock's mirror is inert and the probe drives IReplayClock
# directly, which is what lets one launch answer the rate and step reads without a demo file. The
# register state under test is the same either way, and done.json says so rather than implying the
# transport path was exercised.
add_library(probe_clock SHARED
    ${CMAKE_SOURCE_DIR}/src/engine/clock.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/clock_policy.cpp
    probe_clock.cpp)
target_link_libraries(probe_clock PRIVATE replay_tick replay_flags replay_bootstrap minhook)
target_include_directories(probe_clock PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(probe_clock PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

configure_file(W2-D.probe.json ${CMAKE_BINARY_DIR}/W2-D.probe.json COPYONLY)
