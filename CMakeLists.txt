# Root build for echoreplay. Configure and build through tools/build.ps1 (MSVC + Ninja).

# CMake 4 rejects projects that declare cmake_minimum_required below 3.5; vendored MinHook declares 3.0.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

cmake_minimum_required(VERSION 3.20)
project(echoreplay LANGUAGES C CXX)

if(NOT MSVC)
    message(FATAL_ERROR "MSVC only. Configure through tools/build.ps1.")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Several test directories file(GENERATE) a path to a built binary with no $<CONFIG> in the output name,
# which a multi-config generator refuses. Pin the configuration list to the one build type.
get_property(_replay_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_replay_multi_config)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_BUILD_TYPE}" CACHE STRING "" FORCE)
    message(STATUS "multi-config generator: only ${CMAKE_BUILD_TYPE} is configured, so build it with "
                   "--config ${CMAKE_BUILD_TYPE}. tools/build.ps1 (-G Ninja) is the supported build.")
endif()

# Static CRT, like the other plugins the game loads.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_library(replay_flags INTERFACE)
target_compile_options(replay_flags INTERFACE
    /W4 /permissive- /EHsc /O2 /Zi /FS
    $<$<COMPILE_LANGUAGE:CXX>:/std:c++20>)
target_compile_definitions(replay_flags INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX)
# /PDBALTPATH keeps the build folder's absolute path out of the shipped binaries.
target_link_options(replay_flags INTERFACE /DEBUG /PDBALTPATH:%_PDB% /OPT:REF /OPT:ICF)

add_library(replay_flags_vendor INTERFACE)
target_compile_options(replay_flags_vendor INTERFACE /W0 /O2 /Zi /FS)
target_compile_definitions(replay_flags_vendor INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

add_subdirectory(third_party)

# The product core. Modules add their sources with target_sources(). echoreplay_core.dll is built for the
# tests and is never installed; the plugin users install is EchoReplay, defined further down.
#
# WINDOWS_EXPORT_ALL_SYMBOLS stays OFF: the engine looks up provider method names on our module every
# frame, so an accidental export could make the plugin look like a provider. The export surface is exactly
# Update and RadPluginShutdown, and tests/exports pins it.
add_library(echoreplay_core SHARED)
set_target_properties(echoreplay_core PROPERTIES
    OUTPUT_NAME echoreplay_core
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)
target_link_libraries(echoreplay_core PRIVATE replay_flags minhook zstd)
target_include_directories(echoreplay_core PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Test registration. Every test program and check script is listed in build/test_targets.txt, which
# tools/test.ps1 runs.
function(_replay_register_check kind name path)
    set_property(GLOBAL APPEND PROPERTY REPLAY_TEST_CHECKS "${kind}|${name}|${path}")
endfunction()

function(replay_add_test)
    cmake_parse_arguments(T "" "NAME" "SOURCES;LIBS" ${ARGN})
    if(NOT T_NAME)
        message(FATAL_ERROR "replay_add_test: NAME is required")
    endif()
    if(NOT T_SOURCES)
        message(FATAL_ERROR "replay_add_test(${T_NAME}): SOURCES is required")
    endif()
    add_executable(test_${T_NAME} ${T_SOURCES})
    target_link_libraries(test_${T_NAME} PRIVATE replay_flags doctest_main ${T_LIBS})
    target_include_directories(test_${T_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    _replay_register_check(native ${T_NAME} "$<TARGET_FILE:test_${T_NAME}>")
endfunction()

function(replay_add_script_check)
    cmake_parse_arguments(S "" "NAME;SCRIPT" "" ${ARGN})
    if(NOT S_NAME)
        message(FATAL_ERROR "replay_add_script_check: NAME is required")
    endif()
    if(NOT S_SCRIPT)
        set(S_SCRIPT check.ps1)
    endif()
    _replay_register_check(script ${S_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/${S_SCRIPT}")
endfunction()

# Modules: src/<module>/ or src/<group>/<module>/. A group directory with its own CMakeLists.txt adds
# its children itself.
file(GLOB _module_lists CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*/CMakeLists.txt
    ${CMAKE_SOURCE_DIR}/src/*/*/CMakeLists.txt)
foreach(_list ${_module_lists})
    get_filename_component(_dir ${_list} DIRECTORY)
    get_filename_component(_parent ${_dir} DIRECTORY)
    if(EXISTS "${_parent}/CMakeLists.txt" AND NOT "${_parent}" STREQUAL "${CMAKE_SOURCE_DIR}/src")
        continue()
    endif()
    add_subdirectory(${_dir})
endforeach()

# Engine modules contribute to the product through src/engine/cmake/*.cmake.
file(GLOB _engine_contribs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/engine/cmake/*.cmake)
foreach(_contrib ${_engine_contribs})
    include(${_contrib})
endforeach()

file(GLOB _test_lists CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*/CMakeLists.txt)
foreach(_list ${_test_lists})
    get_filename_component(_dir ${_list} DIRECTORY)
    add_subdirectory(${_dir})
endforeach()

# tests/<group>/<name>/ unless its parent already added it (adding it twice is a duplicate-target error).
file(GLOB _nested_test_lists CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*/*/CMakeLists.txt)
foreach(_list ${_nested_test_lists})
    get_filename_component(_dir ${_list} DIRECTORY)
    get_filename_component(_parent ${_dir} DIRECTORY)
    set(_added_by_parent FALSE)
    if(EXISTS "${_parent}/CMakeLists.txt")
        get_property(_parent_kids DIRECTORY "${_parent}" PROPERTY SUBDIRECTORIES)
        if("${_dir}" IN_LIST _parent_kids)
            set(_added_by_parent TRUE)
        endif()
    endif()
    if(NOT _added_by_parent)
        add_subdirectory(${_dir})
    endif()
endforeach()

get_property(_test_checks GLOBAL PROPERTY REPLAY_TEST_CHECKS)
string(REPLACE ";" "\n" _test_manifest "${_test_checks}")
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/test_targets.txt CONTENT "${_test_manifest}\n")

# EchoReplay: the product plus one observer module (tests/replay/observer.cpp). This is the DLL
# users install. It reads echoreplay_core's own source list, so it must come after every add_subdirectory().
# That list must hold absolute paths (CMP0076 NEW), or a relative entry would resolve against this
# directory and name a different file.
get_target_property(_prod_sources echoreplay_core SOURCES)
get_target_property(_prod_links   echoreplay_core LINK_LIBRARIES)
get_target_property(_prod_incs    echoreplay_core INCLUDE_DIRECTORIES)

if(NOT _prod_sources)
    message(FATAL_ERROR "EchoReplay: echoreplay_core has no SOURCES property")
endif()
foreach(_src IN LISTS _prod_sources)
    if(NOT IS_ABSOLUTE "${_src}")
        message(FATAL_ERROR "EchoReplay: echoreplay_core SOURCES holds the relative entry '${_src}'")
    endif()
endforeach()

set(_observer_tu ${CMAKE_SOURCE_DIR}/tests/replay/observer.cpp)
add_library(EchoReplay SHARED ${_prod_sources} ${_observer_tu})
set_target_properties(EchoReplay PROPERTIES
    OUTPUT_NAME EchoReplay
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)
target_link_libraries(EchoReplay PRIVATE ${_prod_links})
target_include_directories(EchoReplay PRIVATE ${_prod_incs})

function(_replay_link_closure _root _out)
    set(_seen "")
    set(_pending "${_root}")
    while(_pending)
        list(POP_FRONT _pending _t)
        string(REGEX REPLACE "^[$]<LINK_ONLY:(.*)>$" "\\1" _t "${_t}")
        if(NOT TARGET ${_t})
            continue()
        endif()
        if(_t IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen ${_t})
        get_target_property(_direct ${_t} LINK_LIBRARIES)
        get_target_property(_iface  ${_t} INTERFACE_LINK_LIBRARIES)
        foreach(_dep IN LISTS _direct _iface)
            if(_dep AND NOT _dep MATCHES "NOTFOUND$")
                list(APPEND _pending "${_dep}")
            endif()
        endforeach()
    endwhile()
    set(${_out} "${_seen}" PARENT_SCOPE)
endfunction()

function(_replay_closure_sources _root _out)
    _replay_link_closure(${_root} _closure)
    set(_acc "")
    foreach(_c IN LISTS _closure)
        get_target_property(_cs ${_c} SOURCES)
        get_target_property(_cdir ${_c} SOURCE_DIR)
        foreach(_f IN LISTS _cs)
            if(NOT _f MATCHES "NOTFOUND$")
                if(NOT IS_ABSOLUTE "${_f}")
                    set(_f "${_cdir}/${_f}")
                endif()
                list(APPEND _acc "${_f}")
            endif()
        endforeach()
    endforeach()
    set(${_out} "${_acc}" PARENT_SCOPE)
endfunction()

function(_replay_all_targets _dir _out)
    get_property(_here DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_kids DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
    set(_acc ${_here})
    foreach(_kid IN LISTS _kids)
        _replay_all_targets("${_kid}" _sub)
        list(APPEND _acc ${_sub})
    endforeach()
    set(${_out} "${_acc}" PARENT_SCOPE)
endfunction()

# Test probe DLLs compile src/plugin/dllmain.cpp, which calls a detach hook defined in
# pawn_provision.cpp. A probe that does not carry that file links the no-op stub instead. The set is
# derived from the link graph rather than listed by hand.
_replay_all_targets("${CMAKE_SOURCE_DIR}" _all_targets)

set(_detach_stub ${CMAKE_SOURCE_DIR}/tests/buildsys/probe_detach_stub.cpp)
if(NOT EXISTS ${_detach_stub})
    message(FATAL_ERROR "${_detach_stub} is missing; every probe DLL that compiles dllmain.cpp needs it to link")
endif()
foreach(_t IN LISTS _all_targets)
    get_target_property(_type ${_t} TYPE)
    if(NOT _type STREQUAL "SHARED_LIBRARY")
        continue()
    endif()
    _replay_closure_sources(${_t} _contains)
    set(_has_entry FALSE)
    set(_has_pawn FALSE)
    foreach(_f IN LISTS _contains)
        if(_f MATCHES "/src/plugin/dllmain[.]cpp$")
            set(_has_entry TRUE)
        endif()
        if(_f MATCHES "/src/engine/pawn_provision[.]cpp$")
            set(_has_pawn TRUE)
        endif()
    endforeach()
    if(_has_entry AND NOT _has_pawn)
        target_sources(${_t} PRIVATE ${_detach_stub})
    endif()
endforeach()

# target_manifest.json: every DLL target, what it links and whether it carries the bootstrap or tick
# code. tests/replay reads it.
set(_manifest "{\n  \"targets\": [")
set(_first TRUE)
foreach(_t IN LISTS _all_targets)
    get_target_property(_type ${_t} TYPE)
    if(NOT _type STREQUAL "SHARED_LIBRARY")
        continue()
    endif()
    _replay_link_closure(${_t} _closure)
    _replay_closure_sources(${_t} _closure_sources)
    set(_carries_bootstrap "false")
    set(_carries_tick "false")
    foreach(_f IN LISTS _closure_sources)
        if(_f MATCHES "/src/bootstrap/bootstrap[.]cpp$")
            set(_carries_bootstrap "true")
        endif()
        if(_f MATCHES "/src/engine/tick[.]cpp$" OR _f MATCHES "/src/engine/tick_policy[.]cpp$")
            set(_carries_tick "true")
        endif()
    endforeach()
    get_target_property(_own_sources ${_t} SOURCES)
    get_target_property(_own_dir ${_t} SOURCE_DIR)
    set(_rows "")
    foreach(_f IN LISTS _own_sources)
        if(NOT IS_ABSOLUTE "${_f}")
            set(_f "${_own_dir}/${_f}")
        endif()
        file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_f}")
        list(APPEND _rows "\"${_rel}\"")
    endforeach()
    list(JOIN _rows ", " _rows_json)
    set(_link_rows "")
    foreach(_c IN LISTS _closure)
        if(NOT _c STREQUAL _t)
            list(APPEND _link_rows "\"${_c}\"")
        endif()
    endforeach()
    list(JOIN _link_rows ", " _links_json)
    get_target_property(_out_name ${_t} OUTPUT_NAME)
    if(NOT _out_name)
        set(_out_name ${_t})
    endif()
    if(NOT _first)
        string(APPEND _manifest ",")
    endif()
    set(_first FALSE)
    string(APPEND _manifest
        "\n    {\"target\": \"${_t}\", \"output\": \"${_out_name}\", \"kind\": \"SHARED\","
        "\n     \"carriesBootstrap\": ${_carries_bootstrap}, \"carriesTick\": ${_carries_tick},"
        "\n     \"links\": [${_links_json}],"
        "\n     \"sources\": [${_rows_json}]}")
endforeach()
string(APPEND _manifest "\n  ]\n}\n")
file(WRITE ${CMAKE_BINARY_DIR}/target_manifest.json "${_manifest}")

