// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * freedv-processor.c - FreeDV sample processing
 *
 * Author: Annaliese McDermond <nh6z@nh6z.net>
 *
 * Copyright 2019 Annaliese McDermond
 *
 * This file is part of smartsdr-codec2.
 *
 * smartsdr-codec2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Foobar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with smartsdr-codec2.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>

#include "modem_stats.h"

#include "soxr.h"

#include "freedv-processor.h"
#include "utils.h"
#include "vita-io.h"
#include "ringbuf.h"
#include "api.h"
#include "dongle_protocol.h"

#define USE_EXTERNAL_DONGLE

// Enable the following for various debugging
//#define ANALOG_PASSTHROUGH_RX
//#define ANALOG_PASSTHROUGH_TX
//#define SINE_WAVE_RX
//#define SINE_WAVE_TX
#define ADD_GAIN_TO_TX_OUTPUT
#define FREEDV_TX_TIMINGS

#define RADIO_SAMPLE_RATE 24000
#if defined(USE_EXTERNAL_DONGLE)
#define FREEDV_SAMPLE_RATE 8000
#else
#define FREEDV_SAMPLE_RATE (freedv_get_modem_sample_rate(params->fdv))
#endif // defined(USE_EXTERNAL_DONGLE)
#define SAMPLE_RATE_RATIO (RADIO_SAMPLE_RATE / FREEDV_SAMPLE_RATE)
#define PACKET_SAMPLES  128

static char freedv_callsign[10];

struct freedv_proc_t {
    pthread_t thread;
    int running;
#if defined(USE_EXTERNAL_DONGLE)
    struct dongle_packet_handlers* port;
#else
    struct freedv *fdv;
    reliable_text_t rt;
#endif // defined(USE_EXTERNAL_DONGLE)

    ringbuf_t input_buffer;
    ringbuf_t process_in_buffer;
    ringbuf_t process_out_buffer;
    ringbuf_t output_buffer;
    enum freedv_xmit_state xmit_state;
    float squelch_level;
    int squelch_enabled;

    soxr_t downsampler;
    soxr_t upsampler;

    pthread_mutex_t queue_mtx;
    pthread_cond_t waiter;

    // TODO: Meter table?
};

inline static short snr_meter(struct freedv *freedv, struct MODEM_STATS *stats) {
    return float_to_fixed(stats->snr_est, 6);
}
inline static short foff_meter(struct freedv *freedv, struct MODEM_STATS *stats) {
    return float_to_fixed(stats->foff, 6);
}
inline static short clock_offset_meter(struct freedv *freedv, struct MODEM_STATS *stats) {
    return float_to_fixed(stats->clock_offset, 6);
}
inline static short sync_meter(struct freedv *freedv, struct MODEM_STATS *stats) {
    return float_to_fixed(stats->sync, 6);
}
inline static short bits_msb_meter(struct freedv *freedv, struct MODEM_STATS *stats) {
    return freedv_get_total_bits(freedv);
}
inline static short bits_lsb_meter(struct freedv *freedv, struct MODEM_STATS *stats) {
    return freedv_get_total_bits(freedv) >> 16;
}
inline static short errors_meter(struct freedv *freedv, struct MODEM_STATS *stats) {
    return freedv_get_total_bit_errors(freedv);
}
inline static short ber_meter(struct freedv *freedv, struct MODEM_STATS *stats) {
    return float_to_fixed(freedv_get_total_bit_errors(freedv)/(1E-6+freedv_get_total_bits(freedv)), 6);
}
struct meter_def meter_table[] = {
        { 0, "fdv-snr", -100.0f, 100.0f, "DB", snr_meter },
        { 0, "fdv-foff", 0.0f, 1000000.0f, "DB", foff_meter },
        { 0, "fdv-clock-offset", 0.0f, 1000000.0f, "DB", clock_offset_meter},
        { 0, "fdv-sync-quality", 0.0f, 1.0f, "DB", sync_meter},
        { 0, "fdv-total-bits-lsb", 0.0f, 1000000.0f, "RPM", bits_msb_meter },
        { 0, "fdv-total-bits-msb", 0.0f, 1000000.0f, "RPM", bits_lsb_meter },
        { 0, "fdv-error-bits", 0.0f, 1000000.0f, "RPM", errors_meter },
        { 0, "fdv-ber", 0.0f, 10000000.0f, "RPM", ber_meter },
        { 0, "", 0.0f, 0.0f, "", NULL }
};

static struct my_callback_state
{
    char  tx_str[80];
    char *ptx_str;
} _my_cb_state;

#define MAX_RX_STRING_LENGTH 40
static char _rx_string[MAX_RX_STRING_LENGTH + 5];

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Callbacks for embedded ASCII stream, transmit and receive

void my_put_next_rx_char(void *callback_state, char c)
{
    char new_char[2];
    if ( (uint32_t) c < 32 || (uint32_t) c > 126 ) {
        /* Treat all control chars as spaces */
        //output(ANSI_YELLOW "Non-valid RX_STRING char. ASCII code = %d\n", (uint32_t) c);
        new_char[0] = (char) 0x7F;
    } else if ( c == ' ' ) {
        /* Encode spaces differently */
        new_char[0] = (char) 0x7F;
    } else {
        new_char[0] = c;
    }

    new_char[1] = 0;

    strncat(_rx_string, new_char, MAX_RX_STRING_LENGTH+4);
    if (strlen(_rx_string) > MAX_RX_STRING_LENGTH)
    {
        // lop off first character
        strcpy(_rx_string, _rx_string+1);
    }
    //output(ANSI_MAGENTA "new string = '%s'\n",_rx_string);

    char* api_cmd = malloc(80);
    sprintf(api_cmd, "waveform status slice=%d string=\"%s\"",0,_rx_string);
//     tc_sendSmartSDRcommand(api_cmd,false,NULL);
    free(api_cmd);
}

char my_get_next_tx_char(void *callback_state)
{
    struct my_callback_state* pstate = (struct my_callback_state*)callback_state;
    char  c = *pstate->ptx_str++;

    if (*pstate->ptx_str == 0)
    {
        pstate->ptx_str = pstate->tx_str;
    }

    return c;
}

void freedv_set_string(uint32_t slice, char* string)
{
    strcpy(_my_cb_state.tx_str, string);
    _my_cb_state.ptx_str = _my_cb_state.tx_str;
    output("new TX string is '%s'\n",string);
}

static void freedv_send_meters(struct freedv *freedv)
{
    int i;
    struct MODEM_STATS stats;
    int num_meters = (sizeof(meter_table) / sizeof(struct meter_def)) -1;
    short meter_block[num_meters][2];

    freedv_get_modem_extended_stats(freedv, &stats);

    for(i = 0; i < num_meters; ++i) {
        meter_block[i][0] = htons(meter_table[i].id);
        meter_block[i][1] = htons(meter_table[i].set_func(freedv, &stats));
    }

    vita_send_meter_packet(&meter_block, sizeof(meter_block));
}

void freedv_queue_samples(freedv_proc_t params, int tx, size_t len, uint32_t *samples)
{
    assert(params != NULL);

    unsigned int num_samples = len / sizeof(uint32_t);

    for (unsigned int i = 0; i < num_samples / 2; ++i)
        samples[i] = ntohl(samples[i * 2]);

    pthread_mutex_lock(&params->queue_mtx);
    ringbuf_memcpy_into (params->input_buffer, samples, (num_samples / 2) * sizeof(uint32_t));
    pthread_mutex_unlock(&params->queue_mtx);
}

void freedv_signal_processing_thread(freedv_proc_t params)
{
    assert(params != NULL);

    pthread_mutex_lock(&params->queue_mtx);
    pthread_cond_signal(&params->waiter);
    pthread_mutex_unlock(&params->queue_mtx);
}

static void freedv_send_buffer(ringbuf_t buffer, int tx, int flush)
{
    uint32_t packet_buffer[PACKET_SAMPLES];

    while (ringbuf_bytes_used(buffer) >= sizeof(packet_buffer)) {
        ringbuf_memcpy_from (&packet_buffer, buffer, sizeof(packet_buffer));
        vita_send_audio_packet(packet_buffer, sizeof(packet_buffer), tx);
    }

    if (flush) {
        ringbuf_memcpy_from (&packet_buffer, buffer, ringbuf_bytes_used(buffer));
        vita_send_audio_packet(packet_buffer, ringbuf_bytes_used(buffer), tx);
    }
}

void freedv_set_xmit_state(freedv_proc_t params, enum freedv_xmit_state state)
{
    params->xmit_state = state;
}

void freedv_set_callsign(freedv_proc_t params, char* callsign)
{
    strncpy(freedv_callsign, callsign, 9);
    output("Setting callsign to %s\n", freedv_callsign);
#if !defined(USE_EXTERNAL_DONGLE)
    // TBD: reliable_text support on dongle
    if (params && params->fdv && params->rt)
    {
        output("Sending callsign %s to reliable_text\n", freedv_callsign);
        reliable_text_set_string(params->rt, freedv_callsign, strlen(freedv_callsign));
    }
#endif // defined(USE_EXTERNAL_DONGLE)
}

static void freedv_processing_loop_cleanup(void *arg)
{
    freedv_proc_t params = (freedv_proc_t) arg;

#if defined(USE_EXTERNAL_DONGLE)
    dongle_close_port(params->port);
#else
    // TBD: reliable_text support on dongle
    if (params->rt)
    {
        reliable_text_destroy(params->rt);
        params->rt = NULL;
    }
    freedv_close(params->fdv);
#endif // defined(USE_EXTERNAL_DONGLE)
    ringbuf_free(&params->input_buffer);
    ringbuf_free(&params->process_in_buffer);
    ringbuf_free(&params->process_out_buffer);
    ringbuf_free(&params->output_buffer);

    pthread_cond_destroy(&params->waiter);
    pthread_mutex_destroy(&params->queue_mtx);
}

static float tx_scale_factor = exp(5.0f/20.0f * log(10.0f));

#if defined(SINE_WAVE_RX) || defined(SINE_WAVE_TX)
static short sinewave[] = {8000, 0, -8000, 0};
static int sw_idx = 0;
#endif // defined(SINE_WAVE_RX) || defined(SINE_WAVE_TX)

#if defined(USE_EXTERNAL_DONGLE)
static void dongle_rx_tx_common(freedv_proc_t params, int tx)
{
    short inBuf[DONGLE_AUDIO_LENGTH];
    struct dongle_packet packet;

    while (ringbuf_bytes_used(params->process_in_buffer) >= DONGLE_AUDIO_LENGTH * sizeof(short)) {
        ringbuf_memcpy_from(inBuf, params->process_in_buffer, DONGLE_AUDIO_LENGTH * sizeof(short));
        send_audio_packet(params->port, inBuf, tx);

        while (dongle_has_data_available(params->port, 0, 0))
        {
            if (read_packet(params->port, &packet) <= 0) {
                printf("error: %d\n", errno);
                break;
            }
            else if (packet.type != DONGLE_PACKET_RX_AUDIO && packet.type != DONGLE_PACKET_TX_AUDIO) continue;

#if defined(ADD_GAIN_TO_TX_OUTPUT)
            for (int index = 0; index < packet.length; index++)
            {
                // Make samples louder to compensate for lower than expected 
                // power output otherwise on Flex (e.g. setting the power slider to 
                // max only gives 30-40W out on the 6300 using 700D + clipping/BPF on).
                packet.packet_data.audio_data.audio[index] *= tx_scale_factor;
            }
#endif // defined(ADD_GAIN_TO_TX_OUTPUT)

            ringbuf_memcpy_into (params->process_out_buffer, packet.packet_data.audio_data.audio, packet.length);
        }
    }
}

static void rx_handling(freedv_proc_t params)
{
    // RX and TX are the same for dongle on our side. The dongle does the heavy lifting.
    dongle_rx_tx_common(params, 0);
}

static void tx_handling(freedv_proc_t params)
{
    // RX and TX are the same for dongle on our side. The dongle does the heavy lifting.
    dongle_rx_tx_common(params, 1);
}
#else
static void rx_handling(freedv_proc_t params)
{
    int num_speech_samples = freedv_get_n_speech_samples(params->fdv);
    int rx_max_modem_samples = freedv_get_n_max_modem_samples(params->fdv);
    short demod_in[rx_max_modem_samples];
    short speech_out[num_speech_samples];
    int radio_samples;
    int nout;

#if defined(ANALOG_PASSTHROUGH_RX) || defined(SINE_WAVE_RX)
    radio_samples = PACKET_SAMPLES;
#else
    radio_samples = freedv_nin(params->fdv);
#endif // defined(ANALOG_PASSTHROUGH_RX) || defined(SINE_WAVE_RX)

    while (ringbuf_bytes_used(params->process_in_buffer) >= radio_samples * sizeof(short)) {
        ringbuf_memcpy_from(demod_in, params->process_in_buffer, radio_samples * sizeof(short));

#if defined(ANALOG_PASSTHROUGH_RX)
        memcpy(speech_out, demod_in, radio_samples * sizeof(short));
        nout = radio_samples;
#elif defined(SINE_WAVE_RX)
        for (int index = 0; index < radio_samples; index++)
        {
            speech_out[index] = sinewave[sw_idx];
            sw_idx = (sw_idx + 1) % (sizeof(sinewave)/sizeof(short));
        }
        nout = radio_samples;
#else
        nout = freedv_rx(params->fdv, speech_out, demod_in);
        freedv_send_meters(params->fdv);
#endif // ANALOG_PASSTHROUGH_RX || SINE_WAVE_RX

        ringbuf_memcpy_into (params->process_out_buffer, &speech_out[0], nout * sizeof(short));

#if !defined(ANALOG_PASSTHROUGH_RX) && !defined(SINE_WAVE_RX)
        radio_samples = freedv_nin(params->fdv);
#endif // !defined(ANALOG_PASSTHROUGH_RX) && !defined(SINE_WAVE_RX)
    }
}

static void tx_handling(freedv_proc_t params)
{
    int num_speech_samples = freedv_get_n_speech_samples(params->fdv);
    int tx_modem_samples = freedv_get_n_nom_modem_samples(params->fdv);
    short speech_in[num_speech_samples];
    short mod_out[tx_modem_samples];

    while (ringbuf_bytes_used(params->process_in_buffer) >= num_speech_samples * sizeof(short)) {
        ringbuf_memcpy_from(speech_in, params->process_in_buffer, num_speech_samples * sizeof(short));

#if defined(ANALOG_PASSTHROUGH_TX)
        memcpy(mod_out, speech_in, num_speech_samples * sizeof(short));
#elif defined(SINE_WAVE_TX)
        for (int index = 0; index < tx_modem_samples; index++)
        {
            mod_out[index] = sinewave[sw_idx];
            sw_idx = (sw_idx + 1) % (sizeof(sinewave)/sizeof(short));
        }
#else
        freedv_tx(params->fdv, mod_out, speech_in);
#endif // ANALOG_PASSTHROUGH_TX || SINE_WAVE_TX

        for (int index = 0; index < tx_modem_samples; index++)
        {
#if defined(ADD_GAIN_TO_TX_OUTPUT)
            // Make samples louder to compensate for lower than expected 
            // power output otherwise on Flex (e.g. setting the power slider to 
            // max only gives 30-40W out on the 6300 using 700D + clipping/BPF on).
            mod_out[index] *= tx_scale_factor;
#endif // defined(ADD_GAIN_TO_TX_OUTPUT)
        }

        ringbuf_memcpy_into (params->process_out_buffer, &mod_out[0], tx_modem_samples * sizeof(short));
    }
}
#endif // defined(USE_EXTERNAL_DONGLE)

void downsample_input(freedv_proc_t params)
{
    int bytes_used_float = ringbuf_bytes_used(params->input_buffer);
    int samples_used_float = bytes_used_float / sizeof(float);
    if (samples_used_float == 0) return;

    int samples_used_int16 = samples_used_float / SAMPLE_RATE_RATIO + 1;
    int bytes_used_int16 = samples_used_int16 * sizeof(short);
    size_t idone = 0, odone = 0;

    float* resample_buffer_float32 = malloc(bytes_used_float);
    short* resample_buffer_int16 = malloc(bytes_used_int16);
    assert(resample_buffer_float32 && resample_buffer_int16);

    ringbuf_memcpy_from (resample_buffer_float32, params->input_buffer, bytes_used_float);

    soxr_process(
        params->downsampler, resample_buffer_float32, samples_used_float, &idone,
        resample_buffer_int16, samples_used_int16, &odone);

    ringbuf_memcpy_into (params->process_in_buffer, resample_buffer_int16, odone * sizeof(short));
    ringbuf_memcpy_into (params->input_buffer, &resample_buffer_float32[idone], (samples_used_float - idone) * sizeof(float));

    free(resample_buffer_int16);
    free(resample_buffer_float32);
}

void upsample_output(freedv_proc_t params)
{
    int bytes_used_int16 = ringbuf_bytes_used(params->process_out_buffer);
    int samples_used_int16 = bytes_used_int16 / sizeof(short);
    if (samples_used_int16 == 0) return;

    int samples_used_float = (samples_used_int16 + 1) * SAMPLE_RATE_RATIO;
    int bytes_used_float = samples_used_float * sizeof(float);
    size_t idone = 0, odone = 0;

    float* resample_buffer_float32 = malloc(bytes_used_float);
    short* resample_buffer_int16 = malloc(bytes_used_int16);
    assert(resample_buffer_float32 && resample_buffer_int16);

    ringbuf_memcpy_from (resample_buffer_int16, params->process_out_buffer, bytes_used_int16);

    soxr_process(
        params->upsampler, resample_buffer_int16, samples_used_int16, &idone,
        resample_buffer_float32, samples_used_float, &odone);

    ringbuf_memcpy_into (params->output_buffer, resample_buffer_float32, odone * sizeof(float));
    ringbuf_memcpy_into (params->process_out_buffer, &resample_buffer_int16[idone], (samples_used_int16 - idone) * sizeof(short));

    free(resample_buffer_int16);
    free(resample_buffer_float32);
}

static void *_sched_waveform_thread(void *arg)
{
    freedv_proc_t params = (freedv_proc_t) arg;

    int ret;

    struct timespec timeout;
#if defined(FREEDV_TX_TIMINGS)
    struct timespec time_begin;
    struct timespec time_end;
#endif // defined(FREEDV_TX_TIMINGS)

    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_FLOAT32_I);
    params->upsampler = soxr_create(FREEDV_SAMPLE_RATE, RADIO_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);

    soxr_io_spec_t io_spec_downsample = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    params->downsampler = soxr_create(RADIO_SAMPLE_RATE, FREEDV_SAMPLE_RATE, 1, NULL, &io_spec_downsample, NULL, NULL);

    params->running = 1;
    output("Starting processing thread...\n");

    while (params->running) {
        // wait for a buffer descriptor to get posted
        if(clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
            output("Couldn't get time.\n");
            continue;
        }

#if defined(FREEDV_TX_TIMINGS)
        memcpy(&time_begin, &timeout, sizeof(struct timespec));
#endif // defined(FREEDV_TX_TIMINGS)

        long nanoseconds = (1000000000 / RADIO_SAMPLE_RATE) * PACKET_SAMPLES;
        long seconds = (timeout.tv_nsec + nanoseconds) / 1000000000;
        timeout.tv_nsec = (timeout.tv_nsec + nanoseconds) % 1000000000;
        timeout.tv_sec += seconds;

        pthread_mutex_lock(&params->queue_mtx);

        while((ret = pthread_cond_timedwait(&params->waiter, &params->queue_mtx, &timeout)) == -1 && errno == EINTR);

        if(ret == -1) {
            if(errno == ETIMEDOUT) {
                output("[process] Timed out while waiting for semaphore.\n");
                continue;
            } else {
                output("Error acquiring semaphore: %s\n", strerror(errno));
                continue;
            }
        }

        downsample_input(params);
        pthread_mutex_unlock(&params->queue_mtx);

        int radio_samples = 0;
        int reset_buffers = 0;
        switch(params->xmit_state) {
            case READY:
            case RECEIVE:
                rx_handling(params);
                break;
            case PTT_REQUESTED:
            case UNKEY_REQUESTED:
                reset_buffers = 1;
                break;
            case TRANSMITTING:
                tx_handling(params);
                break;
        }

        if (reset_buffers)
        {
            freedv_send_buffer(params->output_buffer, 
                (params->xmit_state == UNKEY_REQUESTED) ? 1 : 0,
                1);

            pthread_mutex_lock(&params->queue_mtx);
            ringbuf_reset(params->input_buffer);
            ringbuf_reset(params->process_in_buffer);
            ringbuf_reset(params->process_out_buffer);
            soxr_clear(params->upsampler);
            soxr_clear(params->downsampler);
            pthread_mutex_unlock(&params->queue_mtx);
        }

        upsample_output(params);
        freedv_send_buffer(
            params->output_buffer, 
            (params->xmit_state == TRANSMITTING || params->xmit_state == UNKEY_REQUESTED) ? 1 : 0,
            0);
         
#if defined(FREEDV_TX_TIMINGS)
        if(clock_gettime(CLOCK_REALTIME, &time_end) == -1) {
            output("Warning: could not get end time for timing checks (errno = %d)\n", errno);
        } else {
            long nanosec_diff = time_end.tv_nsec - time_begin.tv_nsec;
            long sec_diff = time_end.tv_sec - time_begin.tv_sec;
            if (nanosec_diff < 0) {
                sec_diff--;
                nanosec_diff += 1000000000;
            }

            // We receive 10 VITA packets in ~53.3ms @ 24000sps (1280 samples req'd for 700D TX), 
            // so the longest running operation here should not exceed this to guarantee smooth output.
            if (sec_diff > 0 || nanosec_diff > 53333333) {
                output("XXX Exceeded operation max time (s = %d, ns = %d)\n", sec_diff, nanosec_diff);
            }
        }
#endif // defined(FREEDV_TX_TIMINGS)
    }

    output("Processing thread stopped...\n");

    soxr_delete(params->upsampler);
    soxr_delete(params->downsampler);

    return NULL;
}

extern pthread_attr_t global_pthread_properties;

static void start_processing_thread(freedv_proc_t params)
{
    pthread_create(&params->thread, &global_pthread_properties, &_sched_waveform_thread, params);
    pthread_setname_np(params->thread, "FreeDV Modem");
}

void freedv_destroy(freedv_proc_t params)
{
    if (params->running) {
        params->running = 0;
        pthread_join(params->thread, NULL);
    }

#if defined(USE_EXTERNAL_DONGLE)
    dongle_close_port(params->port);
#else
    freedv_close(params->fdv);
#endif // USE_EXTERNAL_DONGLE
    ringbuf_free(&params->process_in_buffer);
    ringbuf_free(&params->process_out_buffer);
    ringbuf_free(&params->input_buffer);
    ringbuf_free(&params->output_buffer);

    pthread_cond_destroy(&params->waiter);
    pthread_mutex_destroy(&params->queue_mtx);
    free(params);
}

static struct freedv *fdv_open(int mode)
{
    struct freedv *fdv;

    if ((mode == FREEDV_MODE_700D) || (mode == FREEDV_MODE_700E) || (mode == FREEDV_MODE_2020)) {
        struct freedv_advanced adv;
        fdv = freedv_open_advanced(mode, &adv);
    } else {
        fdv = freedv_open(mode);
    }

    assert(fdv != NULL);

    if (mode == FREEDV_MODE_700D || mode == FREEDV_MODE_700E)
    {
        freedv_set_clip(fdv, 1);
        freedv_set_tx_bpf(fdv, 1);
    }

    return fdv;
}

static void ReliableTextRx(reliable_text_t rt, const char* txt_ptr, int length, void* state)
{
    // Does nothing for now. We may want to pipe the received callsign to the companion app eventually.
    reliable_text_reset(rt);
}

void fdv_set_mode(freedv_proc_t params, int mode)
{
    assert(params != NULL);

    if (params->running) {
        params->running = 0;
        pthread_join(params->thread, NULL);
    }

#if defined(USE_EXTERNAL_DONGLE)
    dongle_close_port(params->port);
    params->port = dongle_open_port("/dev/ttyACM0");
    if (params->port == NULL)
    {
        fprintf(stderr, "Could not open dongle (errno %d)\n", errno);
    }
    send_set_fdv_mode_packet(params->port, mode);

    // Get acks from serial port
    while(dongle_has_data_available(params->port, 0, 5000))
    {
        struct dongle_packet packet;
        if (read_packet(params->port, &packet) <= 0) break;
    }
#else
    freedv_close(params->fdv);
    params->fdv = fdv_open(mode);
    params->rt = NULL;

    // Set up reliable_text.
    if (mode != FREEDV_MODE_700C)
    {
        output("Enabling reliable_text using callsign %s\n", freedv_callsign);
        params->rt = reliable_text_create();
        reliable_text_set_string(params->rt, freedv_callsign, strlen(freedv_callsign));
        reliable_text_use_with_freedv(params->rt, params->fdv, &ReliableTextRx, NULL);
    }
#endif // defined(USE_EXTERNAL_DONGLE)

    ringbuf_reset(params->input_buffer);
    ringbuf_reset(params->process_in_buffer);
    ringbuf_reset(params->process_out_buffer);
    ringbuf_reset(params->output_buffer);

    start_processing_thread(params);
}

freedv_proc_t freedv_init(int mode)
{
    freedv_proc_t params = malloc(sizeof(struct freedv_proc_t));

    params->xmit_state = READY;

#if defined(USE_EXTERNAL_DONGLE)
    params->port = dongle_open_port("/dev/ttyACM0");
    if (params->port == NULL)
    {
        fprintf(stderr, "Could not open dongle (errno %d)\n", errno);
    }
    send_set_fdv_mode_packet(params->port, mode);

    // Get acks from serial port
    while(dongle_has_data_available(params->port, 0, 5000))
    {
        struct dongle_packet packet;
        if (read_packet(params->port, &packet) <= 0) break;
    }
#else
    // TBD: reliable_text on dongle

    params->fdv = fdv_open(mode);

    // Set up reliable_text.
    if (mode != FREEDV_MODE_700C)
    {
        output("Enabling reliable_text using callsign %s\n", freedv_callsign);
        params->rt = reliable_text_create();
        reliable_text_set_string(params->rt, freedv_callsign, strlen(freedv_callsign));
        reliable_text_use_with_freedv(params->rt, params->fdv, &ReliableTextRx, NULL);
    }
#endif // defined(USE_EXTERNAL_DONGLE)

    params->input_buffer = ringbuf_new(PACKET_SAMPLES * 100 * sizeof(float));
    params->process_in_buffer = ringbuf_new(ringbuf_capacity(params->input_buffer) / sizeof(float) / SAMPLE_RATE_RATIO * sizeof(short));
    params->process_out_buffer = ringbuf_new(ringbuf_capacity(params->process_in_buffer));
    params->output_buffer = ringbuf_new(ringbuf_capacity(params->input_buffer));

    params->squelch_enabled = 0;
    params->squelch_level = 0.0f;

    pthread_cond_init(&params->waiter, NULL);
    pthread_mutex_init(&params->queue_mtx, NULL);

    start_processing_thread(params);

    return params;
}

void freedv_set_squelch_level(freedv_proc_t params, float squelch)
{
#if !defined(USE_EXTERNAL_DONGLE)
    // TBD: squelch on dongle
    if (params == NULL)
        return;

    output("Setting squelch to %f\n", squelch);
    params->squelch_level = squelch;
    freedv_set_snr_squelch_thresh(params->fdv, squelch);
#endif // !defined(USE_EXTERNAL_DONGLE)
}

void freedv_set_squelch_status(freedv_proc_t params, int status)
{
    params->squelch_enabled = status;
#if !defined(USE_EXTERNAL_DONGLE)
    // TBD: squelch on dongle
    freedv_set_squelch_en(params->fdv, status);
#endif // !defined(USE_EXTERNAL_DONGLE)
}

int freedv_proc_get_mode(freedv_proc_t params)
{
#if !defined(USE_EXTERNAL_DONGLE)
    return freedv_get_mode(params->fdv);
#else
    return 0;
#endif // !defined(USE_EXTERNAL_DONGLE)
}

float freedv_proc_get_squelch_level(freedv_proc_t params)
{
    return params->squelch_level;
}

int freedv_proc_get_squelch_status(freedv_proc_t params)
{
    return params->squelch_enabled;
}
