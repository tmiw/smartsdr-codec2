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
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>

#include "event2/buffer.h"

#include "modem_stats.h"

#include "soxr.h"

#include "freedv-processor.h"
#include "utils.h"
#include "vita-io.h"
#include "ringbuf.h"
#include "api.h"

#define RADIO_SAMPLE_RATE 24000
#define FREEDV_SAMPLE_RATE 8000
#define SAMPLE_RATE_RATIO (RADIO_SAMPLE_RATE / FREEDV_SAMPLE_RATE)
#define PACKET_SAMPLES  128

struct fdv {
    struct freedv *fdv;
    struct vita *vita;
    enum freedv_xmit_state xmit_state;
    float squelch_level;
    int squelch_enabled;
    struct evbuffer *rx;
    struct evbuffer *tx;
    struct evbuffer *rx_out;
    short *speech_in;
    short *speech_out;
    short *demod_in;
    short *mod_out;
    float *resample_buffer;

    soxr_t rx_downsampler;
    soxr_t tx_downsampler;
    soxr_t rx_upsampler;
    soxr_t tx_upsampler;
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

//void my_put_next_rx_char(void *callback_state, char c)
//{
//    char new_char[2];
//    if ( (uint32_t) c < 32 || (uint32_t) c > 126 ) {
//    	/* Treat all control chars as spaces */
//    	//output(ANSI_YELLOW "Non-valid RX_STRING char. ASCII code = %d\n", (uint32_t) c);
//    	new_char[0] = (char) 0x7F;
//    } else if ( c == ' ' ) {
//    	/* Encode spaces differently */
//    	new_char[0] = (char) 0x7F;
//    } else {
//    	new_char[0] = c;
//    }
//
//    new_char[1] = 0;
//
//    strncat(_rx_string, new_char, MAX_RX_STRING_LENGTH+4);
//    if (strlen(_rx_string) > MAX_RX_STRING_LENGTH)
//    {
//        // lop off first character
//        strcpy(_rx_string, _rx_string+1);
//    }
//    //output(ANSI_MAGENTA "new string = '%s'\n",_rx_string);
//
//    char* api_cmd = malloc(80);
//    sprintf(api_cmd, "waveform status slice=%d string=\"%s\"",0,_rx_string);
////     tc_sendSmartSDRcommand(api_cmd,false,NULL);
//    free(api_cmd);
//}
//
//char my_get_next_tx_char(void *callback_state)
//{
//    struct my_callback_state* pstate = (struct my_callback_state*)callback_state;
//    char  c = *pstate->ptx_str++;
//
//    if (*pstate->ptx_str == 0)
//    {
//        pstate->ptx_str = pstate->tx_str;
//    }
//
//    return c;
//}
//
//void freedv_set_string(uint32_t slice, char* string)
//{
//    strcpy(_my_cb_state.tx_str, string);
//    _my_cb_state.ptx_str = _my_cb_state.tx_str;
//    output("new TX string is '%s'\n",string);
//}
//
//static void freedv_send_meters(struct freedv *freedv)
//{
//    int i;
//    struct MODEM_STATS stats;
//    int num_meters = (sizeof(meter_table) / sizeof(struct meter_def)) -1;
//    short meter_block[num_meters][2];
//
//    freedv_get_modem_extended_stats(freedv, &stats);
//
//    for(i = 0; i < num_meters; ++i) {
//        meter_block[i][0] = htons(meter_table[i].id);
//        meter_block[i][1] = htons(meter_table[i].set_func(freedv, &stats));
//    }
//
//    vita_send_meter_packet(&meter_block, sizeof(meter_block));
//}
//
//void freedv_queue_samples(freedv_proc_t params, int tx, size_t len, uint32_t *samples)
//{
//    assert(params != NULL);
//
//    unsigned int num_samples = len / sizeof(uint32_t);
//    ringbuf_t buffer = tx ? params->tx_input_buffer : params->rx_input_buffer;
//
//    for (unsigned int i = 0; i < num_samples / 2; ++i)
//        samples[i] = ntohl(samples[i * 2]);
//
//    ringbuf_memcpy_into (buffer, samples, (num_samples / 2) * sizeof(uint32_t));
//    sem_post(&params->input_sem);
//}
//
//static void freedv_send_buffer(ringbuf_t buffer, int tx, int flush)
//{
//    uint32_t packet_buffer[PACKET_SAMPLES];
//
//    while (ringbuf_bytes_used(buffer) >= sizeof(packet_buffer)) {
//        ringbuf_memcpy_from (&packet_buffer, buffer, sizeof(packet_buffer));
//        vita_send_audio_packet(packet_buffer, sizeof(packet_buffer), tx);
//    }
//
//    if (flush) {
//        ringbuf_memcpy_from (&packet_buffer, buffer, ringbuf_bytes_used(buffer));
//        vita_send_audio_packet(packet_buffer, ringbuf_bytes_used(buffer), tx);
//    }
//}
//
//void freedv_set_xmit_state(freedv_proc_t params, enum freedv_xmit_state state)
//{
//    params->xmit_state = state;
//}
//
//static void freedv_processing_loop_cleanup(void *arg)
//{
//    freedv_proc_t params = (freedv_proc_t) arg;
//
//    sem_destroy(&params->input_sem);
//    freedv_close(params->fdv);
//    ringbuf_free(&params->rx_input_buffer);
//    ringbuf_free(&params->tx_input_buffer);
//}

//static void *_sched_waveform_thread(void *arg)
//{
//    freedv_proc_t params = (freedv_proc_t) arg;
//
//    int 	nout;
//    int		ret;
//
//	struct timespec timeout;
//
//    int num_speech_samples = freedv_get_n_speech_samples(params->fdv);
//    int tx_modem_samples = freedv_get_n_nom_modem_samples(params->fdv);
//    int rx_max_modem_samples = freedv_get_n_max_modem_samples(params->fdv);
//
//	soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
//    soxr_t rx_downsampler = soxr_create(RADIO_SAMPLE_RATE, FREEDV_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);
//    soxr_t tx_downsampler = soxr_create(RADIO_SAMPLE_RATE, FREEDV_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);
//
//    io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_FLOAT32_I);
//	soxr_t rx_upsampler = soxr_create(FREEDV_SAMPLE_RATE, RADIO_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);
//	soxr_t tx_upsampler = soxr_create(FREEDV_SAMPLE_RATE, RADIO_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);
//
//    ringbuf_t rx_output_buffer = ringbuf_new (ringbuf_capacity(params->rx_input_buffer));
//    ringbuf_t tx_output_buffer = ringbuf_new (ringbuf_capacity(params->tx_input_buffer));
//
//    short *speech_in = (short *) malloc(num_speech_samples * sizeof(short));
//    short *speech_out = (short *) malloc(num_speech_samples * sizeof(short));
//    short *demod_in = (short *) malloc(rx_max_modem_samples * sizeof(short));
//    short *mod_out = (short *) malloc(tx_modem_samples * sizeof(short));
//
//    int resample_buffer_size = rx_max_modem_samples > tx_modem_samples ? rx_max_modem_samples : tx_modem_samples;
//    float *resample_buffer = (float *) malloc( resample_buffer_size * SAMPLE_RATE_RATIO * sizeof(float));
//
//    // Clear TX string
//    memset(_my_cb_state.tx_str, 0, sizeof(_my_cb_state.tx_str));
//    _my_cb_state.ptx_str = _my_cb_state.tx_str;
//    freedv_set_callback_txt(params->fdv, &my_put_next_rx_char, &my_get_next_tx_char, &_my_cb_state);
//
//	params->running = 1;
//	output("Starting processing thread...\n");
//
//    while (params->running) {
//		// wait for a buffer descriptor to get posted
//		if(clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
//			output("Couldn't get time.\n");
//			continue;
//		}
//		//  TODO:  Probably decrease this timeout.  We should be getting packets every sr / packet_size seconds.
//		timeout.tv_sec += 1;
//
//		while((ret = sem_timedwait(&params->input_sem, &timeout)) == -1 && errno == EINTR);
//
//		if(ret == -1) {
//            if(errno == ETIMEDOUT) {
//				continue;
//			} else {
//				output("Error acquiring semaphore: %s\n", strerror(errno));
//				continue;
//			}
//		}
//
//        //  TODO:  Create a "processing chain" structure to hold all of the data and abstract these into a single
//        //         function.
//		switch(params->xmit_state) {
//		    case READY:
//		    case RECEIVE:
//                //  RX Processing
//                for (int radio_samples = freedv_nin(params->fdv) * SAMPLE_RATE_RATIO;
//                     ringbuf_bytes_used(params->rx_input_buffer) >= radio_samples * sizeof(float);
//                     radio_samples = freedv_nin(params->fdv) * SAMPLE_RATE_RATIO) {
//
//                    size_t odone;
//
//                    ringbuf_memcpy_from(resample_buffer, params->rx_input_buffer, radio_samples * sizeof(float));
//
//                    soxr_process (rx_downsampler,
//                                  resample_buffer, radio_samples, NULL,
//                                  demod_in, radio_samples / SAMPLE_RATE_RATIO, &odone);
//
//                    nout = freedv_rx(params->fdv, speech_out, demod_in);
//                    freedv_send_meters(params->fdv);
//
//                    soxr_process (rx_upsampler,
//                                  speech_out, nout, NULL,
//                                  resample_buffer, radio_samples, &odone);
//
//                    ringbuf_memcpy_into(rx_output_buffer, resample_buffer, odone * sizeof(float));
//                }
//
//                freedv_send_buffer(rx_output_buffer, 0, 0);
//		        break;
//		    case PTT_REQUESTED:
//		        freedv_send_buffer(rx_output_buffer, 0, 1);
//		        ringbuf_reset(params->tx_input_buffer);
//		        ringbuf_reset(tx_output_buffer);
//		        break;
//		    case TRANSMITTING:
//                //  TX Processing
//                while (ringbuf_bytes_used(params->tx_input_buffer) >= num_speech_samples * sizeof(float) * SAMPLE_RATE_RATIO) {
//                    size_t odone;
//
//                    ringbuf_memcpy_from(resample_buffer, params->tx_input_buffer, num_speech_samples * SAMPLE_RATE_RATIO * sizeof(float));
//
//                    soxr_process (tx_downsampler,
//                                  resample_buffer, num_speech_samples * SAMPLE_RATE_RATIO, NULL,
//                                  speech_in, num_speech_samples, &odone);
//
//                    freedv_tx(params->fdv, mod_out, speech_in);
//
//                    soxr_process (tx_upsampler,
//                                  mod_out, tx_modem_samples, NULL,
//                                  resample_buffer, tx_modem_samples * SAMPLE_RATE_RATIO, &odone);
//
//                    ringbuf_memcpy_into (tx_output_buffer, resample_buffer, odone * sizeof(float));
//                }
//                freedv_send_buffer(tx_output_buffer, 1, 0);
//                break;
//		    case UNKEY_REQUESTED:
//		        freedv_send_buffer(tx_output_buffer, 1, 1);
//		        ringbuf_reset(params->rx_input_buffer);
//		        ringbuf_reset(rx_output_buffer);
//		        break;
//		}
//	}
//
//	output("Processing thread stopped...\n");
//
//	free(speech_in);
//    free(speech_out);
//    free(demod_in);
//	free(mod_out);
//	free(resample_buffer);
//
//	soxr_delete(rx_upsampler);
//	soxr_delete(rx_downsampler);
//	soxr_delete(tx_upsampler);
//	soxr_delete(tx_downsampler);
//
//	ringbuf_free(&rx_output_buffer);
//	ringbuf_free(&tx_output_buffer);
//
//	return NULL;
//}

//static void start_processing_thread(freedv_proc_t params)
//{
//    static const struct sched_param fifo_param = {
//            .sched_priority = 30
//    };
//
//	pthread_create(&params->thread, NULL, &_sched_waveform_thread, params);
//	pthread_setschedparam(params->thread, SCHED_FIFO, &fifo_param);
//}
//
//void freedv_destroy(freedv_proc_t params)
//{
//    if (params->running) {
//        params->running = 0;
//        pthread_join(params->thread, NULL);
//    }
//
//    sem_destroy(&params->input_sem);
//    freedv_close(params->fdv);
//    ringbuf_free(&params->rx_input_buffer);
//    ringbuf_free(&params->tx_input_buffer);
//
//    free(params);
//}

//static void freedv_resize_ringbuf(ringbuf_t *buf, size_t newsize)
//{
//    newsize *= sizeof(float) * 10;
//
//    assert(newsize > PACKET_SAMPLES * sizeof(float) * 4);
//
//    if (newsize != ringbuf_capacity(*buf)) {
//        ringbuf_t oldbuf = *buf;
//        *buf = ringbuf_new(newsize);
//        ringbuf_free(&oldbuf);
//    }
//}

static struct freedv *fdv_open(int mode)
{
    struct freedv *fdv;

    if ((mode == FREEDV_MODE_700D) || (mode == FREEDV_MODE_2020)) {
        struct freedv_advanced adv;
        adv.interleave_frames = 1;
        fdv = freedv_open_advanced(mode, &adv);
    } else {
        fdv = freedv_open(mode);
    }

    assert(fdv != NULL);

    return fdv;
}

//void fdv_set_mode(freedv_proc_t params, int mode)
//{
//    assert(params != NULL);
//
//    if (params->running) {
//        params->running = 0;
//        pthread_join(params->thread, NULL);
//    }
//
//    freedv_close(params->fdv);
//    params->fdv = fdv_open(mode);
//
//    freedv_resize_ringbuf(&params->rx_input_buffer, freedv_get_n_max_modem_samples(params->fdv));
//    freedv_resize_ringbuf(&params->tx_input_buffer, freedv_get_n_speech_samples(params->fdv));
//
//    start_processing_thread(params);
//}


static void fdv_rx_cb(struct evbuffer *buffer, const struct evbuffer_cb_info *info, void *ctx)
{
    struct fdv *fdv = (struct fdv *) ctx;
    int nout;
    size_t odone;

    if (info->n_added < 1)
        return;

    switch(fdv->xmit_state) {
        case READY:
        case RECEIVE:
            for (int radio_samples = freedv_nin(fdv->fdv) * SAMPLE_RATE_RATIO;
                 evbuffer_get_length(fdv->rx) >= radio_samples * sizeof(float);
                 radio_samples = freedv_nin(fdv->fdv) * SAMPLE_RATE_RATIO) {

                evbuffer_remove(fdv->rx, fdv->resample_buffer, radio_samples * sizeof(float));

                soxr_process(fdv->rx_downsampler,
                             fdv->resample_buffer, radio_samples, NULL,
                             fdv->demod_in, radio_samples / SAMPLE_RATE_RATIO, &odone);

                nout = freedv_rx(fdv->fdv, fdv->speech_out, fdv->demod_in);
//                freedv_send_meters(params->fdv);

                soxr_process(fdv->rx_upsampler,
                             fdv->speech_out, nout, NULL,
                             fdv->resample_buffer, radio_samples, &odone);

                evbuffer_add(fdv->rx_out, fdv->resample_buffer, odone * sizeof(float));
//                ringbuf_memcpy_into(rx_output_buffer, resample_buffer, odone * sizeof(float));
            }

            break;
        case PTT_REQUESTED:
            evbuffer_drain(fdv->tx, evbuffer_get_length(fdv->tx));
//            ringbuf_reset(params->tx_input_buffer);
//            ringbuf_reset(tx_output_buffer);
            break;
    }
}

static void fdv_rx_out_cb(struct evbuffer *buffer, const struct evbuffer_cb_info *info, void *ctx)
{
    struct fdv *fdv = (struct fdv *) ctx;
    uint32_t packet_buffer[PACKET_SAMPLES];
    int bytes_read;

    if (info->n_added < 1)
        return;

    while (evbuffer_get_length(buffer) >= sizeof(packet_buffer)) {
        bytes_read = evbuffer_remove(buffer, packet_buffer, sizeof(packet_buffer));
        vita_send_audio_packet(fdv->vita, packet_buffer, bytes_read, 0);
    }

    if (fdv->xmit_state == PTT_REQUESTED) {
        bytes_read = evbuffer_remove(buffer, packet_buffer, evbuffer_get_length(buffer));
        vita_send_audio_packet(fdv->vita, packet_buffer, bytes_read, 0);
    }
}

static void fdv_tx_cb(struct evbuffer *buffer, const struct evbuffer_cb_info *info, void *arg)
{
    size_t fill = info->n_added + info->n_added - info->n_deleted;
    if (info->n_added < 1)
        return;
}

struct fdv *fdv_new(struct vita *vita, int mode)
{
    struct fdv *fdv = malloc(sizeof(struct fdv));
    if(!fdv) {
        output("Couldn't malloc memory for fdv struct\n");
        return NULL;
    }

    fdv->vita = vita;

    fdv->fdv = fdv_open(mode);

    fdv->rx = evbuffer_new();
    evbuffer_add_cb(fdv->rx, fdv_rx_cb, fdv);
    fdv->tx = evbuffer_new();
    evbuffer_add_cb(fdv->tx, fdv_tx_cb, fdv);
    fdv->rx_out = evbuffer_new();
    evbuffer_add_cb(fdv->rx_out, fdv_rx_out_cb, fdv);

    fdv->xmit_state = READY;
    fdv->squelch_enabled = 0;
    fdv->squelch_level = 0.0f;

    int num_speech_samples = freedv_get_n_speech_samples(fdv->fdv);
    int tx_modem_samples = freedv_get_n_nom_modem_samples(fdv->fdv);
    int rx_max_modem_samples = freedv_get_n_max_modem_samples(fdv->fdv);
    fdv->speech_in = (short *) malloc(num_speech_samples * sizeof(short));
    fdv->speech_out = (short *) malloc(num_speech_samples * sizeof(short));
    fdv->demod_in = (short *) malloc(rx_max_modem_samples * sizeof(short));
    fdv->mod_out = (short *) malloc(tx_modem_samples * sizeof(short));

    int resample_buffer_size = rx_max_modem_samples > tx_modem_samples ? rx_max_modem_samples : tx_modem_samples;
    fdv->resample_buffer = (float *) malloc( resample_buffer_size * SAMPLE_RATE_RATIO * sizeof(float));

    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    fdv->rx_downsampler = soxr_create(RADIO_SAMPLE_RATE, FREEDV_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);
    fdv->tx_downsampler = soxr_create(RADIO_SAMPLE_RATE, FREEDV_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);
    io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_FLOAT32_I);
	fdv->rx_upsampler = soxr_create(FREEDV_SAMPLE_RATE, RADIO_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);
	fdv->tx_upsampler = soxr_create(FREEDV_SAMPLE_RATE, RADIO_SAMPLE_RATE, 1, NULL, &io_spec, NULL, NULL);

    return fdv;
}

void fdv_free(struct fdv *fdv)
{
    evbuffer_free(fdv->tx);
    evbuffer_free(fdv->rx);

    free(fdv->speech_in);
    free(fdv->speech_out);
    free(fdv->demod_in);
    free(fdv->mod_out);
    free(fdv->resample_buffer);

    freedv_close(fdv->fdv);
    free(fdv);
}

struct evbuffer *fdv_get_tx(struct fdv *fdv)
{
    return fdv->tx;
}

struct evbuffer *fdv_get_rx(struct fdv *fdv)
{
    return fdv->rx;
}

//void freedv_set_squelch_level(freedv_proc_t params, float squelch)
//{
//    if (params == NULL)
//        return;
//
//    output("Setting squelch to %f\n", squelch);
//    params->squelch_level = squelch;
//    freedv_set_snr_squelch_thresh(params->fdv, squelch);
//}
//
//void freedv_set_squelch_status(freedv_proc_t params, int status)
//{
//    params->squelch_enabled = status;
//    freedv_set_squelch_en(params->fdv, status);
//}
//
//int freedv_proc_get_mode(freedv_proc_t params)
//{
//    return freedv_get_mode(params->fdv);
//}
//
//float freedv_proc_get_squelch_level(freedv_proc_t params)
//{
//    return params->squelch_level;
//}
//
//int freedv_proc_get_squelch_status(freedv_proc_t params)
//{
//    return params->squelch_enabled;
//}