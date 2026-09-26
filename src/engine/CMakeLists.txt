# src/engine/ holds the thin bindings that touch game memory: the only layer allowed to
# dereference an engine pointer.
#
# THIS FILE IS CLOSED TO NEW LINES.  It is W0-1's, and the ten lines below
# predate the per-packet channel.  Do NOT append here -- drop src/engine/cmake/<packet>.cmake
# instead: the root CMakeLists.txt globs that directory with CONFIGURE_DEPENDS, so your file is
# picked up with no edit to this one and no cross-packet waiver.  See src/engine/cmake/README.md.
# Name your source ${CMAKE_SOURCE_DIR}/src/engine/<file>; hooks/cmake_sources.ps1 does not expand
# ${CMAKE_CURRENT_LIST_DIR} and refuses the commit.
target_sources(echoreplay PRIVATE wiretap.cpp)
target_sources(echoreplay PRIVATE tick.cpp)
target_sources(echoreplay PRIVATE tick_policy.cpp)
target_sources(echoreplay PRIVATE session.cpp)
target_sources(echoreplay PRIVATE entity.cpp)
target_sources(echoreplay PRIVATE disccensus.cpp)
target_sources(echoreplay PRIVATE asseen.cpp)
target_sources(echoreplay PRIVATE camera_writepoint.cpp)
target_sources(echoreplay PRIVATE oracletap.cpp)
target_sources(echoreplay PRIVATE identity_access.cpp)
target_sources(echoreplay PRIVATE entity_subject.cpp)
