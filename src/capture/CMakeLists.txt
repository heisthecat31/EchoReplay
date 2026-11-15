# W2-A. The capture subsystem: gather, encode, ring push, worker chapterisation.
#
# A static library rather than sources bolted onto the DLL, because tests/capture and tools/sizecheck
# both link the codec and the completeness accounting, and both must build on a machine with no game.
#
# ★ This list grows one line at a time, and a source is added in the same change that makes it compile.
# A CMakeLists that names a file or a subdirectory before it exists reds the whole tree for every other
# packet, which is a far worse outcome than an incomplete library: path ownership is not build isolation.
add_library(replay_capture STATIC
    duv.cpp
    wirebuild.cpp
    streamcodec.cpp
    completeness.cpp
    chapterbuilder.cpp
    manifestbuild.cpp
    identity_emit.cpp
    keyframe.cpp
    markers.cpp
    costmeter.cpp
    capturesecs.cpp
    capturestop.cpp
    ringset.cpp
    clipcut.cpp)
target_link_libraries(replay_capture PRIVATE replay_flags
                      PUBLIC bitio replay_demofile replay_ring replay_events)
target_include_directories(replay_capture PUBLIC ${CMAKE_SOURCE_DIR}/src)

# The engine-facing half. src/engine/netvar.* is W2-A's but src/engine/CMakeLists.txt
# is W0-1's, and 13.1.3's src/engine/cmake/<packet>.cmake channel is specified without a glob to pick it
# up. Naming the file by absolute path from a file this packet DOES own keeps the contribution inside
# W2-A's ownership instead of editing a shared list. Reported to the orchestrator; one line in the glob
# makes this unnecessary.
target_sources(echoreplay_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/capture.cpp
    # The clip service's exports (EchoReplay_Clip / EchoReplay_ClipStatus) for EDITH-HUD.
    ${CMAKE_CURRENT_SOURCE_DIR}/clipservice.cpp
    # BACKLOG-1. The read-only status provider. On the DLL and not in replay_capture above,
    # because its REPLAY_MODULE registrar is referenced by nothing and a static library drops the
    # object that holds it.
    ${CMAKE_CURRENT_SOURCE_DIR}/capturestatus.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/worldorigin.cpp
    # RECORDER-4. The keyframe netvar snapshot's LIVE half: engine getters and playback's netvar
    # route, so it belongs here and not in replay_capture, which must build with no game.
    ${CMAKE_CURRENT_SOURCE_DIR}/netvarsnapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/netvar.cpp)
# ★ replay_voip is linked HERE, not onto replay_capture, because the call that needs it --
# capture.cpp's audio::voip::SetVoipSink -- is in THIS target's sources, not in the library. It was
# briefly PUBLIC on replay_capture, which propagated it to capture_selftest, capture_mkdemo,
# capture_corpus and capture_bench, none of which reference a voip symbol; the only thing that
# achieved was to break all four every time a sibling's voip edit was mid-flight. A dependency
# belongs on the target that has the call site, and declaring it in this file rather than relying on
# src/audio/voip/CMakeLists.txt keeps it true if that file changes.
target_link_libraries(echoreplay_core PRIVATE replay_capture replay_alloctrap replay_voip
                      replay_bootstrap)
# ★ replay_bootstrap is NAMED even though replay_voip already carries it PUBLIC, for the same reason
# replay_voip is named above: capture.cpp's own call site -- bootstrap::OpenRequestPresent, the
# viewer-session test the capture deadline turns on -- is in THIS target's sources, and a dependency
# that arrives through somebody else's PUBLIC link disappears the day that packet re-scopes it.

# tools/ is outside the root CMakeLists.txt globs (src/ and tests/ only), so the CLI is added from the
# module that owns it -- the same pattern W1-B uses for tools/echodemo.
add_subdirectory(${CMAKE_SOURCE_DIR}/tools/sizecheck ${CMAKE_BINARY_DIR}/tools/sizecheck)
