cmake_minimum_required(VERSION 3.25)
project(librearmyoftwo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_VERSION "" CACHE STRING "Override SDK version (leave empty for default)")
if(REXSDK_VERSION)
    find_package(rexglue ${REXSDK_VERSION} EXACT QUIET CONFIG)
else()
    find_package(rexglue 0.8.1.4 QUIET CONFIG)
endif()
if(NOT rexglue_FOUND)
    message(FATAL_ERROR "ReXGlue SDK not found. Build the official SDK or pass -DCMAKE_PREFIX_PATH to its install prefix.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
    message(FATAL_ERROR "generated/default/sources.cmake is missing. Put your legal extraction in game/ and run scripts/build-aot.ps1 -Regenerate first.")
endif()

include(generated/default/sources.cmake)

set(AOT_SOURCES
    src/aot_app.cpp
    src/aot_bink_shim.cpp
    src/aot_xam_coop.cpp
    src/crash_dump.cpp
    src/main.cpp
    ${GENERATED_SOURCES}
)

if(WIN32)
    add_executable(aot WIN32 ${AOT_SOURCES})
else()
    add_executable(aot ${AOT_SOURCES})
endif()

target_include_directories(aot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default
)
target_link_libraries(aot PRIVATE rex::runtime)
if(WIN32)
    target_link_libraries(aot PRIVATE dbghelp)
endif()
rexglue_configure_target(aot)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/dll_targets.cmake")
    include(generated/default/dll_targets.cmake)
endif()
