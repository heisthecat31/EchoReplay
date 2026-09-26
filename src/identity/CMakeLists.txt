# W2-C. Identity and cosmetics: profile synthesis, loadout apply, alignment, and the
# slot-level read-back that turns three silent engine failures into reported ones.
# The engine binding itself lives in src/engine/identity_access.cpp, which src/engine
# contributes to the shipped target.
target_sources(echoreplay_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dresser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/identity_module.cpp)
