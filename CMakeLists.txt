cmake_minimum_required(VERSION 3.0)
include(ExternalProject)
include(FetchContent)

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
#  Common options
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared library.  Set to OFF for static library.")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

#  CODEC2 library
#ExternalProject_Add(codec2
#        PREFIX codec2
##        GIT_REPOSITORY https://github.com/drowe67/codec2.git
#        GIT_REPOSITORY https://github.com/mcdermj/codec2.git
#        GIT_TAG origin/embedded-fixes
#        GIT_SHALLOW OFF
#        GIT_PROGRESS OFF
#        EXCLUDE_FROM_ALL ON
#        BUILD_COMMAND cmake --build . --target codec2
#        INSTALL_COMMAND ""
#        CMAKE_CACHE_ARGS
#          -DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS_${BUILD_TYPE_UC}}
#          -DBUILD_SHARED_LIBS:BOOL=OFF
#          -DUNITTEST:BOOL=OFF
#          -DINSTALL_EXAMPLES:BOOL=OFF
#          -DLPCNET:BOOL=OFF
#          -DCMAKE_BUILD_TYPE:STRING=${UNDERLYING_BUILD_TYPE}
#          -DCMAKE_TOOLCHAIN_FILE:STRING=${CMAKE_TOOLCHAIN_FILE})
#ExternalProject_Get_Property(codec2 BINARY_DIR SOURCE_DIR)
#set(CODEC2_INCLUDE_DIR ${SOURCE_DIR}/src)
#set(CODEC2_LIB_PATH ${BINARY_DIR}/src/${CMAKE_STATIC_LIBRARY_PREFIX}codec2${CMAKE_STATIC_LIBRARY_SUFFIX})

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
          -DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS_${BUILD_TYPE_UC}}
          -DBUILD_SHARED_LIBS:BOOL=OFF
          -DWITH_OPENMP:BOOL=OFF
          -DWITH_LSR_BINDINGS:BOOL=OFF
          -DCMAKE_BUILD_TYPE:STRING=${UNDERLYING_BUILD_TYPE}
          -DCMAKE_TOOLCHAIN_FILE:STRING=${CMAKE_TOOLCHAIN_FILE})
ExternalProject_Get_Property(soxr BINARY_DIR SOURCE_DIR)
set(SOXR_INCLUDE_DIR ${SOURCE_DIR}/src)
set(SOXR_LIB_PATH ${BINARY_DIR}/src/${CMAKE_STATIC_LIBRARY_PREFIX}soxr${CMAKE_STATIC_LIBRARY_SUFFIX})


#  This is conditional on ARM architecture!
FetchContent_Declare(CMSIS
        GIT_REPOSITORY https://github.com/ARM-software/CMSIS_5.git
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        GIT_TAG 5.7.0
)
FetchContent_GetProperties(cmsis)
if(NOT ${cmsis_POPULATED})
  FetchContent_Populate(CMSIS)
endif()

set(ROOT ${cmsis_SOURCE_DIR})
set(DSP ${cmsis_SOURCE_DIR}/CMSIS/DSP)
set(ARM_CPU "cortex-a8")
include(${cmsis_SOURCE_DIR}/CMSIS/DSP/Toolchain/GCC.cmake)
# Add DSP folder to module path
list(APPEND CMAKE_MODULE_PATH ${cmsis_SOURCE_DIR}/CMSIS/DSP ${cmsis_SOURCE_DIR}/CMSIS/DSP/Source)

message("Adding ${cmsis_SOURCE_DIR}/CMSIS/DSP/Source")
include_directories(AFTER ${cmsis_SOURCE_DIR}/CMSIS/Core/Include)
add_subdirectory(${cmsis_SOURCE_DIR}/CMSIS/DSP/Source ${cmsis_BINARY_DIR} EXCLUDE_FROM_ALL)

FetchContent_Declare(codec2
  GIT_REPOSITORY https://github.com/mcdermj/codec2.git
  GIT_TAG origin/embedded-fixes
  GIT_SHALLOW OFF
  GIT_PROGRESS OFF
)
FetchContent_GetProperties(codec2)
if(NOT ${codec2_POPULATED})
  FetchContent_Populate(codec2)
endif()

#  Our code
add_subdirectory(src)
target_include_directories(waveform PUBLIC ${codec2_SOURCE_DIR}/src ${SOXR_INCLUDE_DIR})
add_dependencies(waveform  soxr)
# codec2

add_executable(freedv main.c)
target_include_directories(freedv PRIVATE src ${codec2_SOURCE_DIR}/src)
target_link_libraries(freedv waveform  ${SOXR_LIB_PATH} CMSISDSP m pthread rt)
# ${CODEC2_LIB_PATH}

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