cmake_minimum_required(VERSION 3.0)
include(ExternalProject)

set(CMAKE_CONFIGURATION_TYPES "ARMRelease;ARMDebug" CACHE STRING "" FORCE)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_EXE_LINKER_FLAGS ${CMAKE_EXE_LINKER_FLAGS} -static)

set(CMAKE_C_FLAGS_RELEASE "-Ofast -pg")
set(CMAKE_C_FLAGS_DEBUG "-g -Og -pg")
set(CMAKE_C_FLAGS_ARMRELEASE "-O2 -mfpu=neon -mcpu=cortex-a8")
set(CMAKE_C_FLAGS_ARMDEBUG "-g -Og -pg -mfpu=neon -mcpu=cortex-a8")

string(TOUPPER ${CMAKE_BUILD_TYPE} BUILD_TYPE_UC)

if(CMAKE_BUILD_TYPE MATCHES ARMRelease)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake-toolchains/flex6k.cmake)
  set(UNDERLYING_BUILD_TYPE Release)
elseif(CMAKE_BUILD_TYPE MATCHES ARMDebug)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake-toolchains/flex6k.cmake)
  set(UNDERLYING_BUILD_TYPE Debug)
else()
  set(UNDERLYING_BUILD_TYPE ${CMAKE_BUILD_TYPE})
endif()

#
# Prevent in-source builds
# If an in-source build is attempted, you will still need to clean up a few
# files manually.
#
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
  message(FATAL_ERROR "In-source builds in ${CMAKE_BINARY_DIR} are not "
   "allowed, please remove ./CMakeCache.txt and ./CMakeFiles/, create a "
   "separate build directory and run cmake from there.")
endif("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")

project(freedv_waveform C)

add_subdirectory(src)

add_custom_target(create_zip
        DEPENDS zipdir
        COMMAND zip -r FreeDV.ssdr_waveform FreeDV)

add_custom_command(OUTPUT zipdir
        DEPENDS freedv
        COMMAND ${CMAKE_COMMAND} -E copy
          ${CMAKE_SOURCE_DIR}/FreeDV.cfg
          ${CMAKE_CURRENT_BINARY_DIR}/FreeDV/FreeDV.cfg
        COMMAND ${CMAKE_COMMAND} -E copy
          ${CMAKE_CURRENT_BINARY_DIR}/freedv
          ${CMAKE_CURRENT_BINARY_DIR}/FreeDV/freedv)

add_custom_target(create_installer
        DEPENDS create_zip
        COMMAND wixl -v ${CMAKE_SOURCE_DIR}/FreeDV.wxs -o ${CMAKE_CURRENT_BINARY_DIR}/FreeDV.msi)