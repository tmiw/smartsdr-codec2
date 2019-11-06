cmake_minimum_required(VERSION 3.0)
include(ExternalProject)

set(CMAKE_CONFIGURATION_TYPES "ARMRelease;ARMDebug" CACHE STRING "" FORCE)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_C_FLAGS_RELEASE "-Ofast")
set(CMAKE_C_FLAGS_DEBUG "-g -Og")
set(CMAKE_C_FLAGS_ARMRELEASE "-Ofast")
set(CMAKE_C_FLAGS_ARMDEBUG "-g -Og")

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
#  Common options
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared library.  Set to OFF for static library.")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

#  CODEC2 library
ExternalProject_Add(codec2
        PREFIX codec2
        GIT_REPOSITORY https://github.com/drowe67/codec2.git
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        EXCLUDE_FROM_ALL ON
        BUILD_COMMAND cmake --build . --target codec2
        INSTALL_COMMAND ""
        CMAKE_CACHE_ARGS
          -DBUILD_SHARED_LIBS:BOOL=OFF
          -DUNITTEST:BOOL=OFF
          -DINSTALL_EXAMPLES:BOOL=OFF
          -DLPCNET:BOOL=OFF
          -DCMAKE_BUILD_TYPE:STRING=${UNDERLYING_BUILD_TYPE}
          -DCMAKE_TOOLCHAIN_FILE:STRING=${CMAKE_TOOLCHAIN_FILE})
ExternalProject_Get_Property(codec2 BINARY_DIR SOURCE_DIR)
set(CODEC2_INCLUDE_DIR ${SOURCE_DIR}/src)
set(CODEC2_LIB_PATH ${BINARY_DIR}/src/${CMAKE_STATIC_LIBRARY_PREFIX}codec2${CMAKE_STATIC_LIBRARY_SUFFIX})

#  SOXR resampling library
ExternalProject_Add(soxr
        PREFIX libsoxr
        GIT_REPOSITORY https://git.code.sf.net/p/soxr/code
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        EXCLUDE_FROM_ALL ON
        BUILD_COMMAND cmake --build . --target soxr
        INSTALL_COMMAND ""
        CMAKE_CACHE_ARGS
          -DBUILD_SHARED_LIBS:BOOL=OFF
          -DWITH_OPENMP:BOOL=OFF
          -DWITH_LSR_BINDINGS:BOOL=OFF
          -DCMAKE_BUILD_TYPE:STRING=${UNDERLYING_BUILD_TYPE}
          -DCMAKE_TOOLCHAIN_FILE:STRING=${CMAKE_TOOLCHAIN_FILE})
ExternalProject_Get_Property(soxr BINARY_DIR SOURCE_DIR)
set(SOXR_INCLUDE_DIR ${SOURCE_DIR}/src)
set(SOXR_LIB_PATH ${BINARY_DIR}/src/${CMAKE_STATIC_LIBRARY_PREFIX}soxr${CMAKE_STATIC_LIBRARY_SUFFIX})

#  Our code
add_subdirectory(src)
target_include_directories(waveform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CODEC2_INCLUDE_DIR} ${SOXR_INCLUDE_DIR})
add_dependencies(waveform codec2 soxr)

add_executable(freedv main.c)
target_include_directories(freedv PRIVATE src ${CODEC2_INCLUDE_DIR})
target_link_libraries(freedv waveform m pthread rt ${CODEC2_LIB_PATH} ${SOXR_LIB_PATH})
