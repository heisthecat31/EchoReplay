# W2-I. The O2 and O3 oracles. A static library rather than a target_sources() contribution to the
# DLL, because tests\verify links it directly: an oracle whose only build is inside a 35 MB plugin
# cannot be driven offline, and every gate in S12.7's T1 row runs with no game at all.
add_library(replay_verify STATIC
    verify.cpp
    o2.cpp
    o3.cpp
    floor.cpp)
target_include_directories(replay_verify PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(replay_verify PRIVATE replay_flags)
# floor.cpp measures against the SHIPPED codec, not a model of it.
target_link_libraries(replay_verify PUBLIC bitio)
target_link_libraries(echoreplay PRIVATE replay_verify)
