# W2-A. One registered check named after the directory; check.ps1 drives the instruments and reports
# the total of what they actually asserted.
#
# T1 rather than T0 because the file-level arm writes and re-reads real .echodemo files through the
# container, which is where S12.7 puts the container round-trip.
replay_add_script_check(NAME capture)

# The offline half: the codec, the completeness accounting, the DUV parse, the ring set.
add_executable(capture_selftest test_capture.cpp test_capturesecs.cpp test_capturestop.cpp
                               test_roster.cpp test_localmarkers.cpp test_playerstats.cpp
                               test_levelid.cpp test_gametypeid.cpp test_audiopins.cpp
                               # W2-H's pin CODEC, compiled in rather than linked: test_audiopins
                               # asserts through audio::DecodePins, the same call the playback
                               # module makes, and a second copy of that layout here would certify
                               # nothing. audiopins.cpp is the codec alone -- the live read lives in
                               # audiopinsource.cpp -- so this pulls in no binding, no exports and
                               # no replay_playback, which linking replay_audio would.
                               ${CMAKE_SOURCE_DIR}/src/audio/audiopins.cpp)
# replay_entity_module: the stop line renders the first body refusal with the ENGINE's own
# DescribeDiag rather than with a second copy of BodyDiag's field list, so replay_capture now has
# a real edge to it. capture_wiretest already carried it; this target linked only because nothing
# in it had referenced that renderer yet.
target_link_libraries(capture_selftest PRIVATE replay_flags doctest_main replay_capture
                      replay_bootstrap replay_entity_module)
target_include_directories(capture_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)
# CAP-COST: the identity emitter's profile retry policy. Its own file and its own line, so it
# collides with nobody editing the list above.
target_sources(capture_selftest PRIVATE test_identity_retry.cpp)

# RECORDER-5: the four defects the first live capture put in a file. Its own line for the same
# reason test_identity_retry.cpp has one -- it collides with nobody editing the list above.
target_sources(capture_selftest PRIVATE test_recorder5.cpp)

# The file-level arm and S5.6.2's own mutation. It writes a capture through the real ChapterBuilder and
# the real container, once honestly and once with the SAMPLER halved -- not with a counter decremented,
# which is the difference the whole subsection is about.
add_executable(capture_mkdemo mkdemo.cpp)
target_link_libraries(capture_mkdemo PRIVATE replay_flags replay_capture)
target_include_directories(capture_mkdemo PRIVATE ${CMAKE_SOURCE_DIR}/src)

# SC4's two halves: the per-tick cost histogram and the allocation trap demonstrated firing. It links
# replay_alloctrap, which pulls in MinHook, so it is a separate target from the doctest suite.
add_executable(capture_bench bench.cpp)
target_link_libraries(capture_bench PRIVATE replay_flags replay_capture replay_alloctrap)
target_include_directories(capture_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

# A status probe, not a gate: does replay::IEntity resolve to a complete type yet? Reading the header
# does not answer that -- the compiler does. check.ps1 runs it and prints the two booleans capture.cpp
# branches on, so "S_BODY_SAMPLED is empty" always comes with the reason beside it.
add_executable(capture_nscheck nscheck.cpp)
target_link_libraries(capture_nscheck PRIVATE replay_flags)
target_include_directories(capture_nscheck PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The real-data arm. Its corpus lives outside the repository, so check.ps1 reports NOT RUN naming the
# path it looked for rather than letting the absence read as a pass.
add_executable(capture_corpus corpus.cpp)
target_link_libraries(capture_corpus PRIVATE replay_flags replay_capture)
target_include_directories(capture_corpus PRIVATE ${CMAKE_SOURCE_DIR}/src)

# D13 / P13: the WIRING half. capture.cpp is the module translation unit and is compiled into
# EchoReplay.dll only -- replay_capture deliberately excludes it -- so until this target existed both
# of capture's registrations could be deleted with every suite still green. It links the real module
# TU through the real modreg and reads the fake tick provider, the fake VoIP seam and the services
# struct, the three places a missing registration is visible.
#
# replay_voip is NOT linked, on purpose: voipcapture.cpp keeps its sink in an anonymous namespace with
# no reader, so linking it would make the installation unobservable. test_capture_wire.cpp supplies
# the SetVoipSink definition, which is the seam the assertion reads.
add_executable(capture_wiretest
    test_capture_wire.cpp
    test_alloctrap_scope.cpp
    ${CMAKE_SOURCE_DIR}/src/capture/capture.cpp
    ${CMAKE_SOURCE_DIR}/src/capture/worldorigin.cpp
    ${CMAKE_SOURCE_DIR}/src/capture/netvarsnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/netvar.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/oracletap.cpp)
# entity/asseen/identity_access and the gamestate sampler come from the OBJECT libraries their own
# packets declare (W1-E.cmake, W1-D.cmake). Hand-listing them here is what left this target
# unresolved when capture.cpp gained the ARCH-21 sampler: a list of somebody else`s files goes
# stale silently, a link edge does not.
# The tap and the session entry come from replay_wiretap (W1-L.cmake) and replay_session
# (W1-F.cmake) for the same reason: neither reads a define this target sets.
target_link_libraries(capture_wiretest PRIVATE replay_flags doctest_main replay_capture
                      replay_alloctrap replay_bootstrap replay_entity_module replay_gamestate
                      replay_wiretap replay_session
                      # RECORDER-4: the keyframe netvar snapshot reads through the SAME route and the
                      # SAME key table playback drives the HUD from -- one statement of both, so the
                      # recorder cannot record a key the player does not read.
                      replay_playback)
target_include_directories(capture_wiretest PRIVATE ${CMAKE_SOURCE_DIR}/src)
