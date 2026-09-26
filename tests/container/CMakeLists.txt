# W1-B. One registered check named after the directory, as the gate's A/B cross-check requires.
# check.ps1 drives the instruments and reports the total of what they actually asserted.
# T1 because S12.7 places the container round-trip and the SC8 recovery fuzz there, not in T0.
replay_add_script_check(NAME container)

add_executable(container_selftest test_container.cpp synthcap.cpp)
target_link_libraries(container_selftest PRIVATE replay_flags doctest_main replay_demofile replay_ring replay_alloctrap replay_capture)
target_include_directories(container_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# The fixture generator, and the process SC8 kills.
add_executable(container_mkdemo mkdemo.cpp synthcap.cpp)
target_link_libraries(container_mkdemo PRIVATE replay_flags replay_demofile replay_capture)
target_include_directories(container_mkdemo PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# The real-data arm. Its corpus lives outside the repository, so check.ps1 reports NOT RUN with
# the path it looked for rather than letting the absence pass quietly.
add_executable(container_realdata realdata.cpp synthcap.cpp)
target_link_libraries(container_realdata PRIVATE replay_flags replay_demofile replay_capture)
target_include_directories(container_realdata PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# The pseudonymiser case drives both of these as child processes, so a build of the test alone
# must still produce them.
add_dependencies(container_selftest container_mkdemo echodemo)
