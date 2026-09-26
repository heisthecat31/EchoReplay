# W1-A. A foundation module: its own static library, linked by both the DLL and tests\o1
# (docs\INSTALL.md, "Adding code").
add_library(bitio STATIC
    bitio.cpp
    enginewire.cpp)
target_include_directories(bitio PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bitio PRIVATE replay_flags)
target_link_libraries(echoreplay_core PRIVATE bitio)
