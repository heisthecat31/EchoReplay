cmake_minimum_required(VERSION 3.25)
project(EchoReplayViewer C CXX)

# EchoReplayViewer: watch EchoReplay recordings (.echodemo, and EDITH clips) without the game.
#   levels    EchoConstruct's renderer and stock-level loader, over the corpus it extracted from your install
#   replays   EchoReplay's decoders, through echoreplay_reader.dll (EchoReplay/tools/viewerapi)
#
# Build both first: EchoConstruct (its build/ folder, same MSVC as here) and EchoReplay (tools\build.ps1).
# EchoConstruct's CMake cannot be a subdirectory (it is written for its own top level), so its built
# libraries are linked from ECHOCONSTRUCT_BUILD.

set(ECHOCONSTRUCT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../EchoConstruct" CACHE PATH "EchoConstruct checkout")
set(ECHOCONSTRUCT_BUILD "${ECHOCONSTRUCT_DIR}/build" CACHE PATH "EchoConstruct build folder (RelWithDebInfo)")
set(ECHOREPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "EchoReplay checkout (the repo this folder is in)")
set(ECHOREPLAY_BUILD "${ECHOREPLAY_DIR}/build" CACHE PATH "EchoReplay build folder (has bin/echoreplay_reader.dll)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB viewer_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
# MinHook (from EchoReplay): the viewer answers file calls under its virtual corpus folder from Echo's packages.
set(minhook "${ECHOREPLAY_DIR}/third_party/minhook")
set(minhook_sources "${minhook}/src/buffer.c" "${minhook}/src/hook.c" "${minhook}/src/trampoline.c"
    "${minhook}/src/hde/hde32.c" "${minhook}/src/hde/hde64.c")
set_source_files_properties(${minhook_sources} PROPERTIES COMPILE_OPTIONS "/W0")
add_executable(EchoReplayViewer WIN32 ${viewer_sources} ${minhook_sources})

set(ec "${ECHOCONSTRUCT_DIR}")
set(ecb "${ECHOCONSTRUCT_BUILD}")
target_include_directories(EchoReplayViewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${ECHOREPLAY_DIR}/tools/viewerapi"
    "${ec}/design/contracts" "${ec}/src/foundation/include" "${ec}/src/document-model/include" "${ec}/src/codecs/include"
    "${ec}/src/spec-registry/include" "${ec}/src/scripting/include" "${ec}/src/persistence/include" "${ec}/src/bakes/include"
    "${ec}/src/distribution/include" "${ec}/src/importers/mesh/include" "${ec}/src/importers/include"
    "${ec}/src/assembler/include" "${ec}/src/renderer/include" "${ec}/src/capture/include" "${ec}/src/shell-ui/include"
    "${ec}/third_party/miniz" "${ec}/third_party/zstd/lib" "${ec}/third_party/lucide" "${ec}/third_party/meshoptimizer" "${minhook}/include" "${ecb}/generated")
target_include_directories(EchoReplayViewer SYSTEM PRIVATE "${ec}/third_party/imgui" "${ec}/third_party/imgui/backends")
target_compile_definitions(EchoReplayViewer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN IMGUI_DEFINE_MATH_OPERATORS
    "ECHO_SOURCE_DIR=\"${ec}\"" "ERV_ECHOCONSTRUCT_DIR=L\"${ec}\"" "ERV_ECHOREPLAY_BIN=L\"${ECHOREPLAY_BUILD}/bin\"")
target_compile_options(EchoReplayViewer PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /permissive- /EHsc> /utf-8)
set_target_properties(EchoReplayViewer PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# The editor's own link order (build.ninja, EchoConstruct.exe).
set(ec_libs foundation spec-registry document-model codecs bakes scripting capture persistence importers assembler
    renderer distribution shell-ui imgui imguizmo imgui_node_editor
    shell-ui importers persistence xatlas/echo_xatlas renderer capture distribution assembler bakes baker_fingerprint
    bakes baker_fingerprint scripting codecs document-model spec-registry platform foundation echo_meshoptimizer echo_directxtex
    echo_zydis zstd/echo_zstd_decompress miniz/echo_miniz imgui)
foreach(l ${ec_libs})
    target_link_libraries(EchoReplayViewer PRIVATE "${ecb}/${l}.lib")
endforeach()
target_link_libraries(EchoReplayViewer PRIVATE "${ECHOREPLAY_BUILD}/lib/echoreplay_reader.lib"
    d3d12 dxgi d3d11 dxguid windowscodecs mfplat mfuuid xaudio2 d3dcompiler dwmapi user32 gdi32 shell32 ole32 comdlg32 winmm shcore)

# Next to the exe: the reader DLL, and the folders EchoConstruct's renderer reads beside its executable.
add_custom_command(TARGET EchoReplayViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ECHOREPLAY_BUILD}/bin/echoreplay_reader.dll" "$<TARGET_FILE_DIR:EchoReplayViewer>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ecb}/shaders" "$<TARGET_FILE_DIR:EchoReplayViewer>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ecb}/bake" "$<TARGET_FILE_DIR:EchoReplayViewer>/bake"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ecb}/data" "$<TARGET_FILE_DIR:EchoReplayViewer>/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ecb}/ui" "$<TARGET_FILE_DIR:EchoReplayViewer>/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/maps" "$<TARGET_FILE_DIR:EchoReplayViewer>/maps"
    # EchoConstruct's script node catalogue: the lifter reads it to turn the game's screen scripts into graphs
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ecb}/script" "$<TARGET_FILE_DIR:EchoReplayViewer>/script"
    VERBATIM)
