# Goof Troop Boop (c) 2025
# Based on VGMTrans (c) 2002-2021
# Licensed under the zlib license
# Check the included LICENSE.txt for details

# For cmake-presets v3
cmake_minimum_required(VERSION 3.21)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)

# Use ccache if present on the system
find_program(CCACHE ccache)
if(CCACHE)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
endif()

project(
  GoofTroopBoop
  VERSION 0.1.0
  LANGUAGES C CXX)

add_compile_definitions(NOMINMAX)

# Keep build-machine paths out of shipped binaries: __FILE__ (asserts and log
# locations in the engine and vendored C libraries) is made relative to the
# source tree instead of embedding the absolute checkout path.
if(MSVC)
  file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/" GTB_NATIVE_SOURCE_DIR)
  add_compile_options("/d1trimfile:${GTB_NATIVE_SOURCE_DIR}")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Git)
if(GIT_FOUND)
  execute_process(
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMAND ${GIT_EXECUTABLE} rev-parse --git-path HEAD
    OUTPUT_VARIABLE GIT_HEAD_FILEREF
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${GIT_HEAD_FILEREF}")

  execute_process(
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    OUTPUT_VARIABLE VCS_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  execute_process(
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
    OUTPUT_VARIABLE VCS_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  execute_process(
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --dirty=+
    OUTPUT_VARIABLE VCS_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE)
else()
  set(VCS_TAG "v${CMAKE_PROJECT_VERSION}")
  set(VCS_HASH "unknown")
  set(VCS_BRANCH "unknown")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

#
# Options
#

option(ENABLE_UI_QT "Build the legacy UI (Qt Widgets)" OFF)
option(ENABLE_CLI "Build the CLI application" ON)

add_subdirectory("${PROJECT_SOURCE_DIR}/lib/spdlog")
add_subdirectory("${PROJECT_SOURCE_DIR}/lib/mio")

add_subdirectory(src)
