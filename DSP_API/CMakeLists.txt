project(freedv_waveform C)

cmake_minimum_required(VERSION 3.0)

list (APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/soxr-code/cmake/Modules)


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

option(BUILD_SHARED_LIBS
    "Build shared library. Set to OFF for static library." OFF)

set(CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -fopenmp")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
#set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
#set(BUILD_SHARED OFF)
#set_property(GLOBAL PROPERTY LINK_SEARCH_START_STATIC ON)
#set_property(GLOBAL PROPERTY LINK_SEARCH_END_STATIC ON)

add_subdirectory(codec2 EXCLUDE_FROM_ALL)
add_subdirectory(soxr-code EXCLUDE_FROM_ALL)
add_subdirectory(src)

target_include_directories(waveform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} codec2/src)

add_executable(freedv main.c)
target_link_libraries(freedv waveform soxr codec2 m pthread)
target_include_directories(freedv PUBLIC src ${CMAKE_CURRENT_SOURCE_DIR})
