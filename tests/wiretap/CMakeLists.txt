# W1-L's offline half of the tap's known-answer control.  It reads echovr.exe as a file and
# never launches it, so it runs in T0 alongside the other offline checks.
#
# test_guardprobe.cpp is the second arm: the build guard's own refusal ladder, driven against a
# reserved address range that stands in for a mapped image.  It compiles wiretap.cpp and needs
# replay_bootstrap for replay::Readable -- the probe every check in this plugin uses before it
# dereferences engine memory, imported rather than reimplemented here.
#
# test_arrivalstamp.cpp is the third arm: the tap's own arrival clock, driven over every kind in
# kTaps through the same real broadcaster tests/wirefeed builds.  It reads the linked capture::TapTable()
# rather than keeping a second copy, which is what replay_capture is linked for.
replay_add_test(NAME wiretap
    SOURCES test_matsym.cpp test_guardprobe.cpp test_locktimeout.cpp test_arrivalstamp.cpp
            ${CMAKE_SOURCE_DIR}/tests/wirefeed/engineimage.cpp
            ${CMAKE_SOURCE_DIR}/tests/wirefeed/synthengine.cpp
    LIBS replay_wiretap replay_bootstrap replay_capture)
target_include_directories(test_wiretap PRIVATE ${CMAKE_SOURCE_DIR}/tests/wirefeed)
add_dependencies(test_wiretap wiretap_guardhost wiretap_guardhost_noprobe
                 wiretap_lockhost wiretap_lockhost_unbounded)

# The mutation control, in a child process because the mutant's whole point is that it faults.
# Two targets, one source, one difference.
add_executable(wiretap_guardhost guardhost.cpp)
target_link_libraries(wiretap_guardhost PRIVATE replay_wiretap replay_flags replay_bootstrap)
target_include_directories(wiretap_guardhost PRIVATE ${CMAKE_SOURCE_DIR}/src)

# This target does NOT link replay_wiretap and must not: W1L_MUTATE_NO_READABLE_PROBE is
# target-private and wiretap.cpp:151 reads it, so the object library would hand the mutant the
# control's own object and the mutation would stop being a mutation -- silently. Hand list kept.
add_executable(wiretap_guardhost_noprobe guardhost.cpp ${CMAKE_SOURCE_DIR}/src/engine/wiretap.cpp)
target_compile_definitions(wiretap_guardhost_noprobe PRIVATE W1L_MUTATE_NO_READABLE_PROBE=1)
target_link_libraries(wiretap_guardhost_noprobe PRIVATE replay_flags replay_bootstrap)
target_include_directories(wiretap_guardhost_noprobe PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The registration lock's deadline, and its mutation control. Both run in a child process: the
# mutant's whole point is that it never returns, and a wait that never returns cannot be observed
# from inside the doctest process without hanging the suite.
#
# The host reuses tests/wirefeed's engineimage and synthengine (it does not modify them), so the
# lock it holds is the real CR15NetGame lock reached through the real NRadEngine::CBroadcaster,
# not a stand-in for either.
set(_lockhost_sources
    lockhost.cpp
    ${CMAKE_SOURCE_DIR}/tests/wirefeed/engineimage.cpp
    ${CMAKE_SOURCE_DIR}/tests/wirefeed/synthengine.cpp)

add_executable(wiretap_lockhost ${_lockhost_sources})
target_link_libraries(wiretap_lockhost PRIVATE replay_wiretap replay_flags replay_bootstrap)
target_include_directories(wiretap_lockhost PRIVATE ${CMAKE_SOURCE_DIR}/src
                                                    ${CMAKE_SOURCE_DIR}/tests/wirefeed)

# This target does NOT link replay_wiretap and must not: W1L_MUTATE_UNBOUNDED_LOCK is
# target-private and wiretap.cpp:406,425 read it, so the object library would hand the mutant the
# control's own object and the mutation would stop being a mutation -- silently. It names
# wiretap.cpp itself, which is why _lockhost_sources no longer carries it.
add_executable(wiretap_lockhost_unbounded ${_lockhost_sources}
    ${CMAKE_SOURCE_DIR}/src/engine/wiretap.cpp)
target_compile_definitions(wiretap_lockhost_unbounded PRIVATE W1L_MUTATE_UNBOUNDED_LOCK=1)
target_link_libraries(wiretap_lockhost_unbounded PRIVATE replay_flags replay_bootstrap)
target_include_directories(wiretap_lockhost_unbounded PRIVATE ${CMAKE_SOURCE_DIR}/src
                                                              ${CMAKE_SOURCE_DIR}/tests/wirefeed)
