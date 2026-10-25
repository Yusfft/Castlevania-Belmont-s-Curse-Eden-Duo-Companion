# SPDX-License-Identifier: GPL-3.0-or-later
# Castlevania: Belmont's Curse (0100BA4026E94000) Eden Duo companion module.
#
#   Linux:   cmake -S native -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   Android: cmake -S native -B build-android -G Ninja -DCMAKE_BUILD_TYPE=Release \
#              -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#              -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static
cmake_minimum_required(VERSION 3.22)
project(belmonts-curse-dsmod LANGUAGES CXX)

if(ANDROID AND NOT ANDROID_STL STREQUAL "c++_static")
    message(FATAL_ERROR "Eden Duo modules must use -DANDROID_STL=c++_static")
endif()

set(EDEN_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/eden" CACHE PATH
    "Eden Duo checkout (or the vendored ABI headers)")
foreach(header IN ITEMS dsmod_module_abi.h dsmod_module_extensions.h modules/dsmod_module_sdk.h)
    if(NOT EXISTS "${EDEN_SOURCE_ROOT}/src/core/mods/${header}")
        message(FATAL_ERROR "Eden Duo SDK header missing: ${header}")
    endif()
endforeach()

set(BC_SOURCES bc_unity.cpp bc_world.cpp bc_render.cpp bc_live.cpp bc_tex.cpp bc_levelmap.cpp bc_mapdraw.cpp bc_ui.cpp bc_cards.cpp bc_menu.cpp bc_menupage.cpp)

add_library(dsmod-bc SHARED 0100BA4026E94000.cpp ${BC_SOURCES})
target_compile_features(dsmod-bc PRIVATE cxx_std_20)
target_include_directories(dsmod-bc PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EDEN_SOURCE_ROOT}/src")
set_target_properties(dsmod-bc PROPERTIES
    PREFIX ""
    OUTPUT_NAME "0100BA4026E94000"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
set(exports "${CMAKE_CURRENT_BINARY_DIR}/dsmod-bc.exports")
file(WRITE "${exports}" "{\n    global:\n        eden_dsmod_get_module;\n        eden_dsmod_get_extensions;\n    local: *;\n};\n")
target_link_options(dsmod-bc PRIVATE "-Wl,--version-script=${exports}" "-Wl,--no-undefined")
if(ANDROID)
    target_link_options(dsmod-bc PRIVATE "-Wl,--exclude-libs,ALL")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(dsmod-bc PRIVATE Threads::Threads)
endif()

option(BC_BUILD_TOOLS "Build the test tools" ON)
if(BC_BUILD_TOOLS AND NOT ANDROID)
    enable_testing()
    find_package(Threads REQUIRED)
    # A test-only ASTC decoder for the host simulator (the host provides ASTC to the module).
    set(BC_TEST_ASTC_DIR "" CACHE PATH "Directory with astc.cpp and astc_rgba.h for bc_host_sim")
    foreach(tool bc_dump_tool bc_world_tool bc_live_test bc_host_sim bc_menu_tool bc_lm_tool)
        add_executable(${tool} tools/${tool}.cpp ${BC_SOURCES})
        target_compile_features(${tool} PRIVATE cxx_std_20)
        target_include_directories(${tool} PRIVATE "${CMAKE_CURRENT_LIST_DIR}"
            "${CMAKE_CURRENT_LIST_DIR}/tools" "${EDEN_SOURCE_ROOT}/src")
        target_link_libraries(${tool} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endforeach()
    if(BC_TEST_ASTC_DIR)
        foreach(tool bc_host_sim bc_menu_tool bc_lm_tool)
            target_sources(${tool} PRIVATE "${BC_TEST_ASTC_DIR}/astc.cpp")
            target_include_directories(${tool} PRIVATE "${BC_TEST_ASTC_DIR}")
            target_compile_definitions(${tool} PRIVATE BC_TEST_ASTC)
        endforeach()
    endif()
    add_test(NAME bc-live COMMAND bc_live_test)
endif()
