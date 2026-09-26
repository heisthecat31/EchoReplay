# W2-E. The camera control seam: selecting one of the shipping spectator rig's modes.
# The rig itself is the engine's; this contributes the packet-boundary interface B.7 does not
# expose, which W2-F's panel and W2-G's console both need.
target_sources(echoreplay_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_control.cpp)

# The module translation unit: the one shipping caller of camera_control::Create (DIAG-WIRING gap
# 3). Compiled into the DLL, not a library, so its static registrar survives the link.
target_sources(echoreplay_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_module.cpp)
