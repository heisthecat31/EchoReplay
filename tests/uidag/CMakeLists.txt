# W1-M. The in-game half of the update-DAG spike: the five reads listed in docs/spikes/W1-M.md that no
# offline check can reach. One registered check named after the directory, as the gate's A/B cross-check
# requires; check.ps1 drives the offline proof that the probe files a verdict under the condition that
# has silently eaten probe verdicts on this project before.
replay_add_script_check(NAME uidag)

# The probe. No bootstrap, no MinHook, no engine sources, no exports: it only reads. The verdict is
# filed from a static initialiser, so it does not depend on any of that having run.
# Queued through runs\queue\W1-M.probe.json -- this packet never launches the game. The durable copy of
# the queue descriptor lives here because runs\ is uncommitted:
#
#     copy tests\uidag\W1-M.probe.json runs\queue\W1-M.probe.json
add_library(probe_uidag SHARED probe_uidag.cpp)
target_link_libraries(probe_uidag PRIVATE replay_flags)
target_include_directories(probe_uidag PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_uidag PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# ★ The mutant: the SAME source with the filing moved back into a bootstrap module's Init, which is the
# shape that filed nothing on an earlier sweep. It is built so check.ps1 can require that it fails where
# the real probe succeeds -- a fix whose failure mode is never reproduced is a fix nobody has tested.
add_library(probe_uidag_mutant SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    probe_uidag.cpp)
target_compile_definitions(probe_uidag_mutant PRIVATE W1M_MUTANT_FILE_FROM_MODULE_INIT)
target_link_libraries(probe_uidag_mutant PRIVATE replay_flags replay_bootstrap minhook)
target_include_directories(probe_uidag_mutant PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_uidag_mutant PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# A module that does nothing but win coexist's instance claim, so the probe under test is the REFUSED
# one. The shipped entry point plus the shipped bootstrap and no product modules at all.
add_library(uidag_claimer SHARED ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp)
target_link_libraries(uidag_claimer PRIVATE replay_flags replay_bootstrap minhook)
target_include_directories(uidag_claimer PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(uidag_claimer PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# Loads DLLs into one process in a given order, the way EchoLoader does, with the environment the
# harness sets on the game process.
add_executable(uidag_probehost probehost.cpp)
target_link_libraries(uidag_probehost PRIVATE replay_flags)

configure_file(W1-M.probe.json ${CMAKE_BINARY_DIR}/W1-M.probe.json COPYONLY)
