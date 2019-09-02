set(WAVEFORM_SRCS
    cmd_basics.c
    cmd_engine.c
    discovery_client.c
    hal_buffer.c
    hal_vita.c
    io_utils.c
    ringbuf.c
    sched_waveform.c
    smartsdr_dsp_api.c
    status_processor.c
    traffic_cop.c
    utils.c
    vita-io.c
)

add_library(waveform ${WAVEFORM_SRCS})
target_include_directories(waveform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
