# W2-I. One registered check named after the directory, as the gate's A/B cross-check requires;
# check.ps1 drives the oracle harness and the doctest selftest and reports the total of what they
# asserted, and it mutation-tests that total.
replay_add_script_check(NAME verify SCRIPT check.ps1)

# The oracle harness. Not a doctest target: its output is coverage counters, mutation
# classifications and measured floor rungs, which check.ps1 reads. It links the SHIPPED oracletap
# translation unit, so the tap's guard table and bracket are the ones that go in the DLL.
add_executable(verify_harness
    verify_harness.cpp
    corpus.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/oracletap.cpp)
# replay_tick: oracletap's default clock reader is W1-D's, and linking the real one keeps the
# harness exercising the shipped translation unit rather than a copy of it. The arms bind their
# own clock, so nothing here touches the engine.
target_link_libraries(verify_harness PRIVATE replay_tick replay_flags replay_verify replay_bootstrap minhook)
target_include_directories(verify_harness PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# The plain unit half: the pieces that are ordinary functions with ordinary answers. Driven by
# check.ps1 rather than registered, because the gate keys its manifest by name and this directory
# already registers `verify` -- a second registration under one name replaces the first silently.
add_executable(verify_selftest test_verify.cpp corpus.cpp)
target_link_libraries(verify_selftest PRIVATE replay_flags doctest_main replay_verify)
target_include_directories(verify_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# The exact paths, written by CMake. check.ps1 searching for a binary would be a second source of
# truth about where it lives, and the two would drift.
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/verify_harness_path.txt CONTENT "$<TARGET_FILE:verify_harness>")
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/verify_selftest_path.txt CONTENT "$<TARGET_FILE:verify_selftest>")

# The attach-time probe. Queued through runs\queue\W2-I.probe.json -- this packet never launches the
# game. The durable copy of the queue descriptor lives here because runs\ is uncommitted:
#
#     copy tests\verify\W2-I.probe.json runs\queue\W2-I.probe.json
#
# It links the shipped oracletap translation unit so the site table it checks is the one the tap
# calls, rather than a second copy that would keep passing after the tap moved.
add_library(probe_oracletap SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/oracletap.cpp
    probe_oracletap.cpp)
target_link_libraries(probe_oracletap PRIVATE replay_tick replay_flags replay_bootstrap minhook)
target_include_directories(probe_oracletap PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_oracletap PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

configure_file(W2-I.probe.json ${CMAKE_BINARY_DIR}/W2-I.probe.json COPYONLY)
