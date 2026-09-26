# W1-D. One registered check named after the directory, as the gate's A/B cross-check requires;
# check.ps1 drives the native selftest and pecheck.py and reports the total of what they asserted.
replay_add_script_check(NAME tick)

# The policy translation unit only, so the offline test needs no Windows, no MinHook and no engine.
add_executable(tick_selftest test_tick.cpp)
# replay_bootstrap for config: the mode resolution ClaimMain publishes is config state, and the
# ordering it depends on is checkable with no engine in the process.
target_link_libraries(tick_selftest PRIVATE replay_tick_policy replay_flags replay_bootstrap doctest_main)
target_include_directories(tick_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The in-game half: the shipped entry point, the shipped bootstrap, the shipped tick, plus one module
# that measures what the tick did. Queued through runs\queue\W1-D.probe.json -- this packet never
# launches the game. The durable copy of the queue descriptor lives here because runs\ is uncommitted:
#
#     copy tests\tick\W1-D.probe.json runs\queue\W1-D.probe.json
add_library(probe_tick SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    probe_tick.cpp)
target_link_libraries(probe_tick PRIVATE replay_tick replay_flags replay_bootstrap minhook)
target_include_directories(probe_tick PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_tick PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# The attach-time half: the same entry point and bootstrap, and the same two exports, but it asserts
# only what is true before the engine has initialised, so it runs on `bare` / `bare-norlay` today.
add_library(probe_tickattach SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    probe_tickattach.cpp)
target_link_libraries(probe_tickattach PRIVATE replay_tick replay_flags replay_bootstrap minhook)
target_include_directories(probe_tickattach PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_tickattach PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

configure_file(W1-D.probe.json ${CMAKE_BINARY_DIR}/W1-D.probe.json COPYONLY)
configure_file(W1-D-match.probe.json ${CMAKE_BINARY_DIR}/W1-D-match.probe.json COPYONLY)

# Reproduces sweep 4's condition offline: two copies of the plugin in one process, so coexist refuses
# the second. check.ps1 uses it to prove the attach probe still files a verdict when it is the refused
# one -- the defect that cost two launches.
add_executable(tick_probehost probehost.cpp)
target_link_libraries(tick_probehost PRIVATE replay_flags)
