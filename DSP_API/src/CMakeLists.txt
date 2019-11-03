set(WAVEFORM_SRCS
        api.c
        api-io.c
        discovery.c
        ringbuf.c
        freedv-processor.c
        utils.c
        vita-io.c
)

add_library(waveform ${WAVEFORM_SRCS})
target_include_directories(waveform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
