set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared library.  Set to OFF for static library.")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

#  CODEC2 library
ExternalProject_Add(codec2
    PREFIX codec2
    #        GIT_REPOSITORY https://github.com/drowe67/codec2.git
    #        GIT_TAG c515387982bbe9f0ea9d6d0f50884716c0b1c6c4
    GIT_REPOSITORY https://github.com/mcdermj/codec2.git
    GIT_SHALLOW OFF
    GIT_PROGRESS OFF
    EXCLUDE_FROM_ALL ON
    BUILD_COMMAND cmake --build . --target codec2
    INSTALL_COMMAND ""
    CMAKE_CACHE_ARGS
    -DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS_${BUILD_TYPE_UC}}
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
    -DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS_${BUILD_TYPE_UC}}
    -DBUILD_SHARED_LIBS:BOOL=OFF
    -DWITH_OPENMP:BOOL=OFF
    -DWITH_LSR_BINDINGS:BOOL=OFF
    -DCMAKE_BUILD_TYPE:STRING=${UNDERLYING_BUILD_TYPE}
    -DCMAKE_TOOLCHAIN_FILE:STRING=${CMAKE_TOOLCHAIN_FILE})
ExternalProject_Get_Property(soxr BINARY_DIR SOURCE_DIR)
set(SOXR_INCLUDE_DIR ${SOURCE_DIR}/src)
set(SOXR_LIB_PATH ${BINARY_DIR}/src/${CMAKE_STATIC_LIBRARY_PREFIX}soxr${CMAKE_STATIC_LIBRARY_SUFFIX})

ExternalProject_Add(event_core
        PREFIX libevent
        GIT_REPOSITORY https://github.com/libevent/libevent.git
        GIT_SHALLOW ON
        GIT_PROGRESS ON
        GIT_TAG release-2.1.11-stable
        EXCLUDE_FROM_ALL ON
        BUILD_COMMAND cmake --build . --target event_core_static
        INSTALL_COMMAND ""
        CMAKE_CACHE_ARGS
        -DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS_${BUILD_TYPE_UC}}
        -DCMAKE_BUILD_TYPE:STRING=${UNDERLYING_BUILD_TYPE}
        -DCMAKE_TOOLCHAIN_FILE:STRING=${CMAKE_TOOLCHAIN_FILE}
        -DEVENT__LIBRARY_TYPE:STRING=STATIC
        -DEVENT__DISABLE_BENCHMARK:BOOL=ON
        -DEVENT__DISABLE_OPENSSL:BOOL=ON
        -DEVENT__DISABLE_REGRESS:BOOL=ON
        -DEVENT__DISABLE_SAMPLES:BOOL=ON
        -DEVENT__DISABLE_TESTS:BOOL=ON
        -DEVENT__DISABLE_THREAD_SUPPORT:BOOL=ON
        -DEVENT__DISABLE_MM_REPLACEMENT:BOOL=ON
)
ExternalProject_Get_Property(event_core BINARY_DIR SOURCE_DIR)
set(EVENT_INCLUDE_DIR ${SOURCE_DIR}/include)
set(EVENT_LIB_PATH ${BINARY_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}event_core${CMAKE_STATIC_LIBRARY_SUFFIX})

set(WAVEFORM_SRCS
    api.c
    api-io.c
    discovery.c
    ringbuf.c
    freedv-processor.c
    utils.c
    vita-io.c
    main.c
)

#  Our code
add_executable(freedv ${WAVEFORM_SRCS})
add_dependencies(freedv codec2 soxr event_core)
target_include_directories(freedv PRIVATE ${CODEC2_INCLUDE_DIR} ${SOXR_INCLUDE_DIR} ${EVENT_INCLUDE_DIR})
target_link_libraries(freedv ${CODEC2_LIB_PATH} ${SOXR_LIB_PATH} ${EVENT_LIB_PATH} m pthread rt)