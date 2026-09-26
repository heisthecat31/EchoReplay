# W2-B. The playback core: transport, resolution, and the discontinuity seams W3-A/W3-B build against.
#
# A static library rather than sources on the DLL, because tests/playback links the transport and the
# resolver and must build on a machine with no game. Neither touches engine memory, so both are fully
# testable offline -- which is the point: the two gates that can go red today (G2 and the transport
# cells) do not wait on a launch.
#
# This list grows one line at a time, in the change that makes each file compile. The engine-facing
# half -- ApplyStateAt, the one-node ring, the module registration -- is not here yet and is not named
# here either: a CMakeLists that names a file before it exists reds the tree for every other packet.
add_library(replay_playback STATIC
    transport.cpp
    ownership.cpp
    sample.cpp
    smooth.cpp
    window.cpp
    apply.cpp
    netvarapply.cpp
    netvarsite.cpp
    netvarguard.cpp
    scoregate.cpp
    netvarhud.cpp
    hosthold.cpp
    clockpin.cpp
    shellplan.cpp
    shellfeed.cpp
    windowbuild.cpp
    windowprefetch.cpp
    resolve.cpp
    discontinuity.cpp
    recorded_loadout.cpp
    statsfeed.cpp
    goalfeed.cpp
    stunfeed.cpp
    componentfeed.cpp
    entry_gametype.cpp
    viewer_role.cpp
    reload_cycle.cpp)
# IEntity, IPosePublisher and INetVar are called through their INTERFACES, so none of their
# implementations is linked -- src/ on the include path is the whole of that dependency, and it is what
# keeps this library buildable on a machine with no game.
#
# The container and the codec ARE linked, because windowbuild.cpp decodes real .echodemo bytes through
# W2-A's own decoders rather than through a second reader written here. Two decoders for one format is
# how a file that round-trips through capture stops round-tripping through playback.
target_link_libraries(replay_playback PRIVATE replay_flags PUBLIC replay_demofile replay_capture bitio replay_bootstrap replay_events)
target_include_directories(replay_playback PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ★★★ THE LINE THAT MAKES ANY OF THIS EXIST IN THE PRODUCT.
#
# Everything above linked as a static library with 72,000 verified assertions behind it, and the DLL
# never pulled it in: ReplayServices::transport stayed null and four packets read null. W2-F's panel
# rendering "No replay transport has registered yet" was the only thing in the tree that noticed.
# Verified and wired are different properties, and every test here drove the library directly, so not
# one of them could tell the difference.
target_sources(echoreplay_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/playback_module.cpp)
target_link_libraries(echoreplay_core PRIVATE replay_playback)
