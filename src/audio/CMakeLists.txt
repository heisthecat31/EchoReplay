# W2-H. Audio: the AK bindings, the sink and rate ladder, the pin codec, the S_VOIP record, and the
# category-N primitives.
#
# A static library rather than sources bolted onto the DLL, for the same reason W2-A gives: tests/audio
# links the rate policy, the pin codec and the VOIP record, and all three must build and run on a
# machine with no game. Splitting them out is what lets the offline half of this packet go green
# without a launch.
#
# This list grows one line at a time, in the change that makes each file compile.
add_library(replay_audio STATIC
    akbind.cpp
    sinkvoice.cpp
    voicelifetime.cpp
    ratepolicy.cpp
    audiopins.cpp
    # The live read behind the codec, kept in its own TU so a decode-only consumer can compile
    # audiopins.cpp alone -- tests/capture does exactly that.
    audiopinsource.cpp
    voipformat.cpp
    audioresidue.cpp
    audiorate.cpp
    ratedrive.cpp
    foleymix.cpp
    # src/engine/audiostate.* is W2-H's by the design, but src/engine/CMakeLists.txt is
    # W0-1's and 13.1.3's src/engine/cmake/<packet>.cmake channel is specified without a glob to
    # pick it up. Naming it by absolute path from a file this packet DOES own keeps the
    # contribution inside W2-H's ownership instead of editing a shared list -- the same pattern
    # W2-A uses for src/engine/netvar.cpp, and reported for the same reason. It lives in the
    # library rather than on the DLL so tests/audio can drive the memo walk against a synthetic
    # CSoundPlayer with no game in the process.
    ${CMAKE_SOURCE_DIR}/src/engine/audiostate.cpp)
# replay_bootstrap is PUBLIC because sinkvoice.cpp resolves AUDIO.SINK_LIST_PTR through
# buildguard::MappedAddress -- the one rebasing point in the tree, so this subsystem never carries a
# literal address of its own.
target_link_libraries(replay_audio PRIVATE replay_flags
                      PUBLIC replay_demofile replay_playback replay_bootstrap)
target_include_directories(replay_audio PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ★ audio_module.cpp is compiled into the DLL, never into the library above: a REPLAY_MODULE's
# registrar is a static object nothing references by name, so an archive member holding one is
# dropped by the linker as unused and the module silently never runs (docs/INSTALL.md).
target_sources(echoreplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/audio_module.cpp)
target_link_libraries(echoreplay PRIVATE replay_audio)

# ★ W3-C's src/audio/voip/ IS ADDED FROM HERE BECAUSE THE ROOT WILL NOT ADD IT. The root's discovery
# glob skips a nested module whose parent has a CMakeLists.txt of its own -- "a group directory that
# has its own CMakeLists.txt adds its children itself" -- so src/audio being a module makes this the
# only sanctioned place the line can go. voipformat.h already names src/audio/voip/ as W3-C's, and
# the directory it points at exists in the same commit as this line: a CMakeLists naming a file
# before it exists reds the configure for every packet in the tree.
add_subdirectory(voip)
