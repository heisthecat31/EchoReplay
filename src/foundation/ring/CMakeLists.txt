# W1-B. The ring is a foundation library with no dependencies; the S6.4 trap is a separate target so
# that linking the ring does not drag MinHook into everything that uses it.
add_library(replay_ring STATIC ring.cpp)
target_link_libraries(replay_ring PRIVATE replay_flags)
target_include_directories(replay_ring PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(replay_alloctrap STATIC alloc_trap.cpp)
target_link_libraries(replay_alloctrap PRIVATE replay_flags PUBLIC minhook)
target_include_directories(replay_alloctrap PUBLIC ${CMAKE_SOURCE_DIR}/src)
