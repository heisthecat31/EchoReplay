# W2-F. The offline half of the UI gate: the timeline arithmetic, which touches neither the ImGui
# facade nor engine memory and so runs on a machine with no game.
#
# T0 rather than T1: nothing is read from echovr.exe by the executable. check.ps1 adds a SECOND,
# binary-dependent step that re-reads every guard byte in src/ui/ui_facts.h back out of the shipped
# exe; that step reports itself separately and refuses to be counted when the binary is absent.
replay_add_script_check(NAME ui SCRIPT check.ps1)

add_executable(ui_selftest test_ui.cpp)
target_link_libraries(ui_selftest PRIVATE replay_flags doctest_main replay_ui_timeline)
target_include_directories(ui_selftest PRIVATE ${CMAKE_SOURCE_DIR}/src)

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/ui_selftest_path.txt CONTENT "$<TARGET_FILE:ui_selftest>")

# The panel's WIRING, offline. ui_selftest above links timeline.cpp alone, which is why three
# deliverables in panel.cpp and demoindex.cpp shipped with no caller and a green gate: the load
# funnel's session-entry consult, the show/hide switch and the cursor claim. This target links the
# REAL panel.cpp, demoindex.cpp and csregister.cpp against a fake ImGui facade (fake_gui.cpp) and
# real .echodemo files written by the shipped DemoWriter, so those call sites are exercised rather
# than assumed. The facade binding itself stays check.ps1's job -- it needs the binary.
add_executable(ui_paneltest
    test_panel.cpp
    test_inframe.cpp
    test_bar.cpp
    test_recorder.cpp
    test_import.cpp
    fake_gui.cpp
    fake_deps.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/bar.cpp
    # BACKLOG-1. The recorder surface's view model. NOTHING from src/capture is linked here on
    # purpose: the UI reads the recorder through capture/capturestatus.h, whose registry is inline,
    # so a UI that called the recorder itself would not link. That is half the read-only proof;
    # test_recorder.cpp's source scan is the other half.
    ${CMAKE_SOURCE_DIR}/src/ui/recorder.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/panel.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/demoindex.cpp
    # FORMATS-P6. The conversion worker; every case swaps in a fake converter, so this suite binds a
    # refusing one instead of linking the real converter library.
    ${CMAKE_SOURCE_DIR}/src/ui/legacyimport.cpp
    importbind_refusing.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/csregister.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/inframe.cpp
    ${CMAKE_SOURCE_DIR}/src/camera/camera_control.cpp)
# replay_alloctrap is the tree's own zero-allocation instrument (foundation/ring/alloc_trap.h) and
# it is what the recorder's steady-tick case is asserted with. It brings MinHook, which this target
# already links.
target_link_libraries(ui_paneltest PRIVATE replay_flags doctest_main replay_ui_timeline
    replay_demofile replay_bootstrap replay_alloctrap minhook)
target_include_directories(ui_paneltest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
# test_panel.cpp reads src/ui/panel.cpp as text: the exit-teardown pin is a property of the source.
target_compile_definitions(ui_paneltest PRIVATE REPLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/ui_paneltest_path.txt CONTENT "$<TARGET_FILE:ui_paneltest>")

# The in-game half: the shipped bootstrap, the shipped tick and the shipped csregister, plus one
# module that measures what the engine did with them -- W1-H's probe_camera pattern. Linking the
# real csregister.cpp is the point: a reimplemented registrar could get the order key or the scheme
# offset right by accident while the product had it wrong.
#
# Queued through runs\queue\W2-F.probe.json -- this packet never launches the game. The durable copy
# of the queue descriptor lives here because runs\ is uncommitted:
#
#     copy tests\ui\W2-F.probe.json runs\queue\W2-F.probe.json
add_library(probe_ui SHARED
    ${CMAKE_SOURCE_DIR}/src/ui/csregister.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/inframe.cpp
    # inframe's GateScope reaches the engine's two setters through the facade table, so the probe
    # links the REAL gui.cpp. A reimplemented binder here could pass its guard while the shipped
    # one refused.
    ${CMAKE_SOURCE_DIR}/src/ui/gui.cpp
    probe_ui.cpp)
target_link_libraries(probe_ui PRIVATE replay_tick replay_flags replay_bootstrap minhook)
target_include_directories(probe_ui PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(probe_ui PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/probe_ui_path.txt CONTENT "$<TARGET_FILE:probe_ui>")
configure_file(W2-F.probe.json ${CMAKE_BINARY_DIR}/W2-F.probe.json COPYONLY)

# Loads the probe the way the game's loader does, so check.ps1 can prove the provisional record is
# filed from the static initialiser rather than assume it.
add_executable(uiprobehost uiprobehost.cpp)
target_link_libraries(uiprobehost PRIVATE replay_flags)
add_dependencies(uiprobehost probe_ui)
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/uiprobehost_path.txt CONTENT "$<TARGET_FILE:uiprobehost>")
