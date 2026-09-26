# W1-C's offline loader probe. Three small binaries reproduce the one condition no unit test reaches: our
# DLL_PROCESS_ATTACH running under the Windows loader lock, entered from another DLL's DllMain, with a
# second plugin sharing the folder.
#
# Not built by replay_add_test: these are not doctest targets. The script check drives them.
add_executable(probe_host probe_host.cpp)
target_link_libraries(probe_host PRIVATE replay_flags)

add_library(probe_loader SHARED probe_loader.cpp)
target_link_libraries(probe_loader PRIVATE replay_flags)

# Named rlay.dll on purpose: SC9 is about coexisting with the plugin that is actually in this folder on
# this machine, and GetModuleHandleA("rlay.dll") is the detection the design specifies.
add_library(probe_rlay SHARED probe_rlay.cpp)
set_target_properties(probe_rlay PROPERTIES OUTPUT_NAME rlay)
target_link_libraries(probe_rlay PRIVATE replay_flags)

add_dependencies(probe_host echoreplay_core probe_loader probe_rlay)

# The in-game half. Same entry point and same bootstrap as the shipped DLL - the point is to watch the
# real build guard ACCEPT, which no offline host can produce - plus one module that reports what it saw.
# Queued through runs\queue\W1-C.probe.json; this packet never launches the game itself.
add_library(probe_bootstrap SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    probe_ingame.cpp)
target_link_libraries(probe_bootstrap PRIVATE replay_flags replay_bootstrap)
set_target_properties(probe_bootstrap PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# The offline-local half. Same bootstrap, but it reports at MATCH rather than at attach, because the
# lock's Exercise verdict is only meaningful on a launch that can actually load one - `bare` never
# reaches the menu-script path that clears the offline bit, so there the verdict is structurally pinned
# to `not-exercised` and would prove nothing.
add_library(probe_offlinelock SHARED
    ${CMAKE_SOURCE_DIR}/src/plugin/dllmain.cpp
    probe_offlinelock.cpp)
target_link_libraries(probe_offlinelock PRIVATE replay_flags replay_bootstrap)
set_target_properties(probe_offlinelock PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

replay_add_script_check(NAME loaderprobe)

# The queue descriptor lives here as well as in runs\queue\ because runs\ is deliberately uncommitted
# (W1-J's .gitignore: "artifacts are machine-local evidence, not source"). A probe request is not
# evidence, it is this packet's statement of what it needs the sweep to execute, so the durable copy is
# here and is copied into runs\queue\ before a sweep:
#
#     copy tests\loaderprobe\W1-C.probe.json runs\queue\W1-C.probe.json
configure_file(W1-C.probe.json ${CMAKE_BINARY_DIR}/W1-C.probe.json COPYONLY)
