# W1-E. One registered check named after the directory; check.ps1 drives the native selftest and
# reports the total of what it asserted.
replay_add_script_check(NAME entity)

# The offline half: the pure logic E-4 owns that needs no engine -- the fp16 codec the PP-B write path
# runs every pose through, and the two constants entity.cpp DERIVES rather than carries as facts.
add_executable(entity_selftest test_entity.cpp)
target_link_libraries(entity_selftest PRIVATE replay_flags doctest_main)
target_include_directories(entity_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The wiring half: the real module translation unit, through the real modreg, into a services struct
# the test can read. DIAG-WIRING gap 1 was three adapters with zero callers behind 72k green
# assertions; this is the assertion that sees a registration. entity.cpp needs replay_bootstrap for
# buildguard, so every site guard resolved out of the MAPPED image refuses on this host -- which is
# the point for the wiring half: the module must reach the adapter and publish it without a game.
#
# It is NOT a statement about the span checks (AUDIT_K1 K1-3 read it as one). Those are pure over
# their inputs, and the cases at the foot of test_entity_module.cpp drive four of the five to BOTH
# outcomes through EntityCreateWithLookup's lookup and entry-point seams, against a two-page
# reservation with one page committed. The fifth -- the netGame back-map -- is proved subsumed by
# the check above it rather than given a fixture that cannot fail.
#
# The object list is replay_entity_module (src/engine/cmake/W1-E.cmake), not a copy of it. A copy is
# a claim about the module's closure that nothing checks, and it goes stale the moment the module
# grows a file: that is how tests/playback stopped linking at eb31bd4 and how all four targets in
# this file stopped linking at 65eb960 (identity_access.cpp gained a call into entity_json.cpp).
add_executable(entity_wiretest test_entity_module.cpp)
# replay_capture is linked so the bits11 cases below drive the SHIPPED BodyEncoder rather than a
# local restatement of its field widths. A test with its own copy of a format certifies broken
# files; this one asks the encoder the capture path calls.
target_link_libraries(entity_wiretest PRIVATE replay_flags doctest_main replay_entity_module
                      replay_capture)
# The ARCH-12 4.3 golden lives in the SOURCE tree, not the build tree: it is a committed artifact
# that W2-R's parser test reads, so the case that writes it must reach the checkout it belongs to.
target_compile_definitions(entity_wiretest PRIVATE REPLAY_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# The `cam look at <n>` composer: the roster scan and the body read W2-E's seam calls, driven with a
# fake CR15NetGame and a fake IEntity. It links camera_control.cpp for PackSelector -- the packing is
# the ENGINE's and must not be re-derived here -- and identity_access.cpp for the roster reads.
# REPLAY_REPO_ROOT is for the case that asserts the install line exists in entity_module.cpp: a unit
# test of the two composer functions cannot see that nothing calls them (DIAG-WIRING gap 1).
add_executable(entity_subjecttest test_entity_subject.cpp)
target_link_libraries(entity_subjecttest PRIVATE replay_flags doctest_main replay_entity_module)
target_compile_definitions(entity_subjecttest PRIVATE REPLAY_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# The three readers ENG-W2A-ROSTER S1 asked this packet for: the engine's own entrant count, the
# two loadout numbers, and the profile-JSON contract. It links identity_access.cpp only -- these are
# pure reads over a fake CR15NetGame, with no engine call in any of them, which is exactly why they
# are testable on a host with no game.
add_executable(entity_readerstest test_identity_readers.cpp)
target_link_libraries(entity_readerstest PRIVATE replay_flags doctest_main replay_entity_module)

# JSONWALK: the profile serialiser, driven by a fake node tree built to the ENGINE'S accessor
# contract rather than to its node layout -- nothing in entity_json.cpp reads a node, so nothing
# here has to reproduce one. It links identity_access.cpp for the site table, because a unit test of
# the emitter alone cannot see that the shipped path binds the eight accessors it needs.
# The live scoreboard: the fifteen per-entrant values ReadPlayerStats sources, driven against a
# fake CR15NetGame whose stat words are planted through the SAME registry offsets the reader uses.
# It links replay_entity_module for entity.cpp's SpanProof, which is the cache the reader shares
# with the tick-path accessors rather than keeping a second copy of.
add_executable(entity_statstest test_player_stats.cpp)
target_link_libraries(entity_statstest PRIVATE replay_flags doctest_main replay_entity_module)

add_executable(entity_jsontest test_entity_json.cpp)
target_link_libraries(entity_jsontest PRIVATE replay_flags doctest_main replay_entity_module)

# The attach-time probe: the shipped entry point and bootstrap, plus one module that reads W1-E's
# sites out of the MAPPED image. Queued through runs\queue\W1-E.probe.json -- this packet never
# launches the game. The durable copy of the queue descriptor lives here because runs\ is uncommitted:
#
#     copy tests\entity\W1-E.probe.json runs\queue\W1-E.probe.json
#
# W1-E-match.probe.json is the in-match half. It is NOT copied into the queue while `flat` is refused:
# a probe that cannot be planned makes the whole sweep unplannable for everyone else.
add_library(probe_entityattach SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    probe_entityattach.cpp)
target_link_libraries(probe_entityattach PRIVATE replay_flags replay_bootstrap minhook)
target_include_directories(probe_entityattach PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_entityattach PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# The IN-MATCH probe. It links replay_flags ONLY and brings its own DllMain: linking replay_bootstrap
# would pull in the module registry whose instance claim refuses every module after the first
# alphabetically, and that claim is precisely what the static-initialiser filing exists to route
# around. A probe whose verdict depends on winning that race files nothing on the sweep it was
# written for.
add_library(probe_entitymatch SHARED probe_entitymatch.cpp)
target_link_libraries(probe_entitymatch PRIVATE replay_flags)
target_include_directories(probe_entitymatch PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_entitymatch PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

configure_file(W1-E.probe.json ${CMAKE_BINARY_DIR}/W1-E.probe.json COPYONLY)
configure_file(W1-E-match.probe.json ${CMAKE_BINARY_DIR}/W1-E-match.probe.json COPYONLY)

# AUDIT_ARCH21 CONDITION-B6: the span cache's re-probe cadence, driven over a page this test
# owns and frees itself -- the one way to reach `freed at an unchanged base` without a game. It
# links replay_entity_module for the ONE SpanProof definition; a copy compiled into the test
# would be a second answer to `is this span proved`.
add_executable(entity_spanprooftest test_entity_spanproof.cpp)
target_link_libraries(entity_spanprooftest PRIVATE replay_flags doctest_main replay_entity_module)
