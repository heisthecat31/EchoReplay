# W3-C. One registered check named after the directory; check.ps1 drives the arms and reports what
# each actually asserted.
#
# T0 rather than T1: every arm here runs with no game, no network and no launch -- the component
# system it walks is built in this process's own memory at the offsets the fact drop decoded -- so
# it belongs in the tier that gates a commit rather than the one that gates a sweep.
replay_add_script_check(NAME voip)

# The offline half: the receiver resolution walk, the five gates, the capture tap's dispositions,
# the injection decision, and the container round trip from written records back into a stand-in
# for VOIP.RECEIVE.
add_executable(voip_selftest test_voip.cpp test_voip_container.cpp test_voip_bind.cpp)
target_link_libraries(voip_selftest PRIVATE replay_flags doctest_main replay_voip)
target_include_directories(voip_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

# ★ THE MUTANT. Same sources, with the discontinuity guard neutered back to the form that shipped
# unreachable. check.ps1 REQUIRES this one to FAIL: an arm that cannot fail is not a control, and
# "no discontinuity was reported" reads identically against dead code and correct code.
#
# It compiles the three voip sources directly rather than linking replay_voip, because the define
# has to reach voipplay.cpp and the library is built once without it.
add_executable(voip_selftest_mutant
    test_voip.cpp test_voip_container.cpp test_voip_bind.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/voip/voipsite.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/voip/voipcapture.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/voip/voipplay.cpp)
target_compile_definitions(voip_selftest_mutant PRIVATE W3C_MUTANT_DISCONTINUITY=1)
target_link_libraries(voip_selftest_mutant PRIVATE replay_flags doctest_main replay_audio
                      replay_capture replay_demofile replay_bootstrap)
target_include_directories(voip_selftest_mutant PRIVATE ${CMAKE_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_SOURCE_DIR})

# The arm that proves the resolution against the SHIPPED BINARY rather than against this project's
# beliefs about it: it re-decodes every W3-C fact out of echovr.exe and requires the header the code
# compiles against to agree. It reports NOT RUN naming the path when the binary is absent, because a
# missing binary is a gate result and not a pass.
add_executable(voip_factcheck factcheck_voip.cpp)
target_link_libraries(voip_factcheck PRIVATE replay_flags replay_voip)
target_include_directories(voip_factcheck PRIVATE ${CMAKE_SOURCE_DIR}/src)
