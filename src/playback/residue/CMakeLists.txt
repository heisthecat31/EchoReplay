# W3-A. The engine-facing half of the category-N reset battery.
#
# A static library rather than sources on the DLL, for the reason W2-B and W2-H both give: the walk,
# the guards and the eight rows must build and run on a machine with no game. Splitting resolution
# (residuesite.cpp's two Resolve* functions) from effect (everything else) is what makes that
# possible -- tests/residue drives the same walk code over a synthetic component-system image.
#
# ★ replay_bootstrap is PUBLIC because residuesite.cpp resolves every engine address through
# buildguard::MappedAddress and every entry point through buildguard::VerifySite. That is the one
# rebasing point in the tree, and DYNAMICBASE + HIGH_ENTROPY_VA means a literal address used without
# it is wrong silently. No file in this directory carries an address of its own.
add_library(replay_residue STATIC
    residuesite.cpp
    residue.cpp)
target_link_libraries(replay_residue PRIVATE replay_flags PUBLIC replay_playback replay_bootstrap)
target_include_directories(replay_residue PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ★★★ THE LINE THAT MAKES THIS PACKET REACH THE PRODUCT -- AND THE ONE THAT IS NOT ENOUGH.
#
# Linking the library puts the code in the DLL. It does NOT make it run: the rows reach the battery
# only when W2-B calls residue::ArmPlaybackResidue, and the battery lives in an anonymous-namespace
# Global in playback_module.cpp that nothing outside that file can name. Until that call lands, every
# row here is linked, tested and dead -- which is exactly the state audio::ArmAudioResidue has been
# in since W2-H landed, unnoticed, because nothing reported it.
#
# residue_module.cpp is that report. It is compiled straight onto the DLL and never into the archive
# above, because a REPLAY_MODULE's registrar is a static object nothing references by name and an
# archive member holding one is dropped by the linker as unused -- the module would silently never
# run, which is the failure it exists to detect.
target_sources(echoreplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/residue_module.cpp)
target_link_libraries(echoreplay PRIVATE replay_residue)
