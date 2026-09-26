# W2-C. One registered check named after the directory, as the gate's A/B cross-check requires;
# check.ps1 drives the native selftest and sitecheck.py and reports the total of what they asserted.
replay_add_script_check(NAME identity)

# The offline half: the pure logic the cosmetic path is built on -- the post-apply diff that
# tells a strip from the engine's own item remap, and the profile walk that carries a recorded
# ServerProfile in one scalar at a time.  Neither needs an engine, so both run in the gate.
#
# identity_access.cpp is linked in for WalkProfileScalars and for the site table.  BuildMatches()
# IS reached here, on purpose: this process is not echovr.exe, so every recorded site rebases to
# an address outside the mapped image and the module has to refuse all fourteen by name instead of
# faulting.  That is the offline half of C-1, and it needs replay_bootstrap for buildguard.
#
# W3-E's lobbymsg.cpp is linked in so the release body is decoded by the SHIPPED decoder for that
# message rather than by a reader written beside the writer.  A writer that carries its own copy of
# a layout certifies its own bytes -- the same defect as a checker with a private format, arriving
# from the other side.
#
# identity_access.cpp arrives as the link edge replay_identity_access (src/engine/cmake/W1-E.cmake),
# not as a source line: it gained a call into entity_json.cpp at 65eb960 and this target, which had
# its own copy of W1-E's file list, stopped linking. A copy of somebody else's closure goes stale
# silently; a link edge does not. It is NOT replay_entity_module, the bigger library beside it:
# nothing here wants entity.cpp, entity_subject.cpp, camera_control.cpp or the module registrar, and
# a test process that carries subjects it does not own is a test that can fail for somebody else's
# reason. (Measured: linking either library, the C-1 case below reads 22 sites, because
# identity_access.cpp's own table grew to 22 -- see the report; that constant is stale, not a
# consequence of this edge.)
add_executable(identity_selftest
    test_identity.cpp
    ${CMAKE_SOURCE_DIR}/src/events/lobbymsg.cpp
    ${CMAKE_SOURCE_DIR}/src/identity/dresser.cpp)
target_link_libraries(identity_selftest PRIVATE replay_flags replay_bootstrap doctest_main
                      replay_identity_access)
target_include_directories(identity_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The case that pins the entrant-table geometry to the merged facts/facts.tsv rows reads
# that file at run time; without a root it would have no outside record to disagree with.
target_compile_definitions(identity_selftest PRIVATE REPLAY_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# The attach-time probe: the shipped entry point and bootstrap, plus one module that re-reads
# W2-C's twelve engine sites out of the MAPPED image rather than the file on disk.  That is the
# half sitecheck.py cannot do -- a foreign detour lands in memory, not in echovr.exe -- and it
# is the reason rlay.dll's Detours patch was found rather than mistaken for a wrong build.
# Queued through runs\queue\W2-C.probe.json; this packet never launches the game.
#
#     copy tests\identity\W2-C.probe.json runs\queue\W2-C.probe.json
# identity_access.cpp is linked in so the probe walks the SHIPPED module's own resolved site
# table rather than a parallel copy of it: a control that re-derives its subject cannot see the
# subject drift away from it, which is exactly how the un-rebased addresses stayed invisible.
#
# Same edge, and for the second reason not to take replay_entity_module: that library carries
# entity_module.cpp, whose static registrar would file W1-E's module into the registry of a probe
# whose whole subject is W2-C's site table -- and the registry's instance claim refuses every module
# after the first alphabetically (see tests/entity's probe_entitymatch).
add_library(probe_identityattach SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    probe_identityattach.cpp)
target_link_libraries(probe_identityattach PRIVATE replay_flags replay_bootstrap minhook
                      replay_identity_access)
target_include_directories(probe_identityattach PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_identityattach PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

configure_file(W2-C.probe.json ${CMAKE_BINARY_DIR}/W2-C.probe.json COPYONLY)
