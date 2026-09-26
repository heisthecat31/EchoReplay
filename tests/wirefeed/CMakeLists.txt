# W1-L's feed check: the in-process replacement for section 9 check 3.
#
# feedhost maps echovr.exe into its own process, builds a CBroadcaster out of the engine's own
# container code, registers through the shipped IWiretap -- which calls the real
# NRadEngine::CBroadcaster::Listen -- and drives the registered listener nodes with R4's
# captured Echo Arena session at the rate that session actually had. No socket, no launch; see
# docs/spikes/W1-L.md section 11.
#
# T1 because the paced run takes the capture's own 125.387 s, and takes it twice: once for the
# real host and once for the wrong-bus control, which has to be shown to deliver nothing over
# the whole window rather than over a convenient prefix.
replay_add_test(NAME wirefeed SOURCES test_feed.cpp)
add_dependencies(test_wirefeed wiretap_feedhost wiretap_feedhost_oldshape
                 wiretap_feedhost_wrongbus probe_wiretap probe_wiretap_nofiler session_probehost)

# replay_bootstrap: wiretap.cpp probes every derived VA with replay::Readable before comparing
# it, so the host that compiles wiretap.cpp links the library that publishes it.
# wiretap.cpp is NOT in this set. The two hosts that do not mutate it link replay_wiretap
# (W1-L.cmake) and get the shipped translation unit as an object; wiretap_feedhost_oldshape,
# whose private W1L_MUTATE_PROXY_SLOT wiretap.cpp:28 reads, names the path itself below.
set(_feedhost_sources
    feedhost.cpp
    engineimage.cpp
    synthengine.cpp
    wirefeed.cpp)

add_executable(wiretap_feedhost ${_feedhost_sources})
target_link_libraries(wiretap_feedhost PRIVATE replay_wiretap replay_flags replay_bootstrap)
target_include_directories(wiretap_feedhost PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ★ The two mutations. Each reproduces a shape this packet actually held, rather than deleting
# the thing under test: a mutant that merely removes code often proves less than it looks, and
# a control that has only ever agreed has not been shown able to disagree.
#
# oldshape: the delegate this packet built before it read the two dispatchers, with the invoker
# one slot after the context. Section 7 attempt 9 records both the belief and the correction.
# The node is otherwise perfect and every symbol is right, so the known-answer control that
# guards the T0 check cannot see it.
#
# This target does NOT link replay_wiretap and must not: W1L_MUTATE_PROXY_SLOT is target-private
# and wiretap.cpp:28 reads it, so the object library would hand the mutant the control's own
# object and the mutation would stop being a mutation -- silently. It keeps the hand list.
add_executable(wiretap_feedhost_oldshape ${_feedhost_sources}
    ${CMAKE_SOURCE_DIR}/src/engine/wiretap.cpp)
target_compile_definitions(wiretap_feedhost_oldshape PRIVATE W1L_MUTATE_PROXY_SLOT=1)
target_link_libraries(wiretap_feedhost_oldshape PRIVATE replay_flags replay_bootstrap)
target_include_directories(wiretap_feedhost_oldshape PRIVATE ${CMAKE_SOURCE_DIR}/src)

# wrongbus: the tap lands on a second, separately built CBroadcaster in the same CR15NetGame.
# Registration succeeds against real engine code and returns real handles, every symbol is
# right, every node is well formed -- and nothing ever arrives. That is exactly the failure
# section 9 names as the reason check 3 exists, and only a measured rate catches it.
add_executable(wiretap_feedhost_wrongbus ${_feedhost_sources})
target_compile_definitions(wiretap_feedhost_wrongbus PRIVATE W1L_MUT_WRONG_BUS=1)
target_link_libraries(wiretap_feedhost_wrongbus PRIVATE replay_wiretap replay_flags replay_bootstrap)
target_include_directories(wiretap_feedhost_wrongbus PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ★ The in-game half, added after the orchestrator unblocked the `offline-local` profile. It answers
# the three payload reads tests/wirefeed cannot construct offline -- the client-mode broadcaster, the
# callback thread, and the W1-L half of SC7 -- and it is queued, never launched, by this packet:
#
#     copy tests\wirefeed\W1-L.probe.json runs\queue\W1-L.probe.json
#
# No src/plugin/dllmain.cpp: this probe files its verdict from a static initialiser and reports the
# bootstrap's answer rather than obeying it, so returning FALSE from DllMain on a refused instance
# claim would unload a module whose worker is already measuring. The Update and RadPluginShutdown
# exports the CNSProvider slot needs come from src/engine/tick.cpp, not from the entry point.
add_library(probe_wiretap SHARED probe_wiretap.cpp)
target_link_libraries(probe_wiretap PRIVATE replay_wiretap replay_tick replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(probe_wiretap PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_wiretap PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

configure_file(W1-L.probe.json ${CMAKE_BINARY_DIR}/W1-L.probe.json COPYONLY)

# ★ The reporting control. Identical except that the static-initialiser filer is compiled out, so it
# can only file from a REPLAY_MODULE init -- the shape sweep 4's three silent probes had. The offline
# proof asserts this one goes SILENT under the same refusal the real probe survives; a control that
# has only ever agreed has not been shown able to disagree.
# W1L_NO_STATIC_FILER is read by probe_wiretap.cpp, which stays a source of this target, and
# appears nowhere under src/engine -- so linking the shared wiretap object still leaves this
# mutant different from probe_wiretap.
add_library(probe_wiretap_nofiler SHARED probe_wiretap.cpp)
target_compile_definitions(probe_wiretap_nofiler PRIVATE W1L_NO_STATIC_FILER=1)
target_link_libraries(probe_wiretap_nofiler PRIVATE replay_wiretap replay_tick replay_flags replay_bootstrap minhook ws2_32)
target_include_directories(probe_wiretap_nofiler PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(probe_wiretap_nofiler PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
