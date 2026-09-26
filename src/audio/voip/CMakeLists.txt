# W3-C. VOIP: the capture detour on VOIP.RECEIVE, the receiver resolution, and playback injection.
#
# A static library so tests/voip can drive the resolution walk, the tap and the injection decision
# against a synthetic component system on a machine with no game -- which is where every one of
# those can be wrong.
#
# ★ This list grows one line at a time, in the change that makes each file compile. A CMakeLists
# that names a file before it exists reds the configure for the whole tree, and configure is
# all-or-nothing.
add_library(replay_voip STATIC
    voipsite.cpp
    voipcapture.cpp
    voipplay.cpp)
target_link_libraries(replay_voip PRIVATE replay_flags
                      PUBLIC replay_audio replay_capture replay_demofile replay_bootstrap)
target_include_directories(replay_voip PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ★ voip_module.cpp is compiled into the DLL, never into the library above: a REPLAY_MODULE's
# registrar is a static object nothing references by name, so an archive member holding one is
# dropped by the linker as unused and the module silently never runs (docs/INSTALL.md).
target_sources(echoreplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/voip_module.cpp)
target_link_libraries(echoreplay PRIVATE replay_voip)
