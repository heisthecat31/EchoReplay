# W1-F's offline half: the SE-A token rules, a known-answer control that re-derives every number in
# session_facts.h from the shipping echovr.exe with a second PE reader, and the reporting proof --
# that the probe still files a verdict when the bootstrap refuses its instance claim. It reads the
# exe as a file and never launches it, so it runs in T0 with the other offline checks.
replay_add_test(NAME session
    SOURCES test_session.cpp boot_flag_expectation.h
    LIBS replay_session replay_bootstrap)
add_dependencies(test_session probe_session probe_session_nofiler session_probehost)

# The in-game half: the shipped bootstrap and the shipped ISessionEntry, plus one probe module that
# measures what the engine did. Queued through runs\queue\W1-F.probe.json -- this packet never
# launches the game. The durable copy of the queue descriptor lives here because runs\ is
# uncommitted:
#
#     copy tests\session\W1-F.probe.json runs\queue\W1-F.probe.json
add_library(probe_session SHARED
    boot_flag_expectation.h
    probe_session.cpp)
target_link_libraries(probe_session PRIVATE replay_session replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(probe_session PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_session PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# ★ The mutation. Identical in every respect except that the static-initialiser filer is compiled
# out, so it can only file from the bootstrap path. The offline proof asserts this one goes SILENT
# under the same refusal the real probe survives -- a control that has only ever agreed has not been
# shown able to disagree.
# W1F_NO_STATIC_FILER is read by probe_session.cpp, which stays a source of this target, and
# appears nowhere under src/engine -- so linking the shared session object still leaves this
# mutant different from probe_session. session.cpp reads no mutation define at all.
add_library(probe_session_nofiler SHARED
    boot_flag_expectation.h
    probe_session.cpp)
target_compile_definitions(probe_session_nofiler PRIVATE W1F_NO_STATIC_FILER=1)
target_link_libraries(probe_session_nofiler PRIVATE replay_session replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(probe_session_nofiler PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_session_nofiler PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# Loads probe DLLs into one process the way EchoLoader does, so the instance-claim refusal is
# reproducible offline instead of costing a launch to rediscover.
add_executable(session_probehost probehost.cpp)
target_link_libraries(session_probehost PRIVATE replay_flags)

configure_file(W1-F.probe.json ${CMAKE_BINARY_DIR}/W1-F.probe.json COPYONLY)
