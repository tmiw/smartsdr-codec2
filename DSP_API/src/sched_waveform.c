// SPDX-Licence-Identifier: GPL-3.0-or-later
/* *****************************************************************************
 *
 *  Copyright (C) 2014-2019 FlexRadio Systems.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Contact Information:
 *  email: gpl<at>flexradiosystems.com
 *  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 *  Author: Ed Gonzalez
 *  Author: Graham (KE9H)
 *  Author: Annaliese McDermond <nh6z@nh6z.net>
 *
 * ************************************************************************** */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>
#include <stdbool.h>

#include "modem_stats.h"

#include "soxr.h"

#include "sched_waveform.h"
#include "utils.h"
#include "vita-io.h"
#include "ringbuf.h"
#include "api.h"

static bool _end_of_transmission = false;

struct freedv_proc_t {
    pthread_t thread;
    int running;
    sem_t input_sem;
    int mode;
    struct freedv *fdv;
    ringbuf_t rx_input_buffer;
    ringbuf_t tx_input_buffer;

    // XXX Meter table?
};

struct meter_def meter_table[] = {
        { 0, "fdv-snr", -100.0f, 100.0f, "DB" },
        { 0, "fdv-foff", 0.0f, 1000000.0f, "DB" },
        { 0, "fdv-clock-offset", 0.0f, 1000000.0f, "DB"},
        { 0, "fdv-sync-quality", 0.0f, 1.0f, "DB"},
        { 0, "fdv-total-bits", 0.0f, 1000000.0f, "RPM" },
        { 0, "fdv-error-bits", 0.0f, 1000000.0f, "RPM" },
        { 0, "fdv-ber", 0.0f, 10000000.0f, "RPM" },
        { 0, "", 0.0f, 0.0f, "" }
};

#define PACKET_SAMPLES  128

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
    output(ANSI_MAGENTA "new TX string is '%s'\n",string);
}

void sched_waveform_setEndOfTX(bool end_of_transmission)
{
	output("Setting end of waveform\n");
    _end_of_transmission = true;
}

static void freedv_send_meters(struct freedv *freedv)
{
    short meter_block[7][2] = {0};
    int i;
    struct MODEM_STATS stats;

    freedv_get_modem_extended_stats(freedv, &stats);

    // XXX These need to be in order of the array definitions.
    // XXX Yeah, I know, weak, but it works.
    meter_block[0][1] = htons(float_to_fixed(stats.snr_est, 6));
    meter_block[1][1] = htons(float_to_fixed(stats.foff, 6));
    meter_block[2][1] = htons(float_to_fixed(stats.clock_offset, 6));
    meter_block[3][1] = htons(float_to_fixed(stats.sync, 6));
    meter_block[4][1] = htons(freedv_get_total_bits(freedv));
    meter_block[5][1] = htons(freedv_get_total_bit_errors(freedv));
    meter_block[6][1] = htons(float_to_fixed(freedv_get_total_bit_errors(freedv)/(1E-6+freedv_get_total_bits(freedv)), 6));

    for (i = 0; i < 4; ++i)
        meter_block[i][0] = htons(meter_table[i].id);

    vita_send_meter_packet(&meter_block, sizeof(meter_block));
    output(".");
}

static void freedv_processing_loop_cleanup(void *arg)
{
    freedv_proc_t params = (freedv_proc_t) arg;

    sem_destroy(&params->input_sem);

    freedv_close(params->fdv);
    ringbuf_free(&params->rx_input_buffer);
    ringbuf_free(&params->tx_input_buffer);
}

void freedv_queue_rx_samples(freedv_proc_t params, uint32_t *samples, size_t len)
{
    unsigned int num_samples = len / sizeof(uint32_t);

    for (unsigned int i = 0, j = 0; i < num_samples / 2; ++i, j += 2)
        samples[i] = ntohl(samples[j]);

    ringbuf_memcpy_into (params->rx_input_buffer, samples, (num_samples / 2) * sizeof(uint32_t));
    sem_post(&params->input_sem);
}

static void *_sched_waveform_thread(void *arg)
{
    freedv_proc_t params = (freedv_proc_t) arg;

    int 	nin, nout;
    int		ret;

	uint32_t packet_buffer[PACKET_SAMPLES * 2];
	struct timespec timeout;

    int tx_speech_samples = freedv_get_n_speech_samples(params->fdv);
    int tx_modem_samples = freedv_get_n_nom_modem_samples(params->fdv);

	soxr_error_t error;
	soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    soxr_t rx_downsampler = soxr_create(24000, 8000, 1, &error, &io_spec, NULL, NULL);
    soxr_t tx_downsampler = soxr_create(24000, 8000, 1, &error, &io_spec, NULL, NULL);

    io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_FLOAT32_I);
	soxr_t rx_upsampler = soxr_create(8000, 24000, 1, &error, &io_spec, NULL, NULL);
	soxr_t tx_upsampler = soxr_create(8000, 24000, 1, &error, &io_spec, NULL, NULL);

    ringbuf_t rx_output_buffer = ringbuf_new (ringbuf_capacity(params->rx_input_buffer));
    ringbuf_t tx_output_buffer = ringbuf_new (ringbuf_capacity(params->tx_input_buffer));

    short *speech_in = (short *) malloc(freedv_get_n_speech_samples(params->fdv) * sizeof(short));
    short *speech_out = (short *) malloc(freedv_get_n_speech_samples(params->fdv) * sizeof(short));
    short *demod_in = (short *) malloc(freedv_get_n_max_modem_samples(params->fdv) * sizeof(short));
    short *mod_out = (short *) malloc(freedv_get_n_nom_modem_samples(params->fdv) * sizeof(short));

    // Clear TX string
    memset(_my_cb_state.tx_str, 0, sizeof(_my_cb_state.tx_str));
    _my_cb_state.ptx_str = _my_cb_state.tx_str;
    freedv_set_callback_txt(params->fdv, &my_put_next_rx_char, &my_get_next_tx_char, &_my_cb_state);

    if (meter_table[0].id == 0)
        register_meters(meter_table);

	params->running = 1;
	output("Starting processing thread...\n");
    pthread_cleanup_push(freedv_processing_loop_cleanup, params);

    while (params->running) {
		// wait for a buffer descriptor to get posted
		if(clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
			output("Couldn't get time.\n");
			continue;
		}
		timeout.tv_sec += 1;

		while((ret = sem_timedwait(&params->input_sem, &timeout)) == -1 && errno == EINTR)
			continue;

		if(ret == -1) {
			if(errno == ETIMEDOUT) {
				continue;
			} else {
				output("Error acquiring semaphore: %s\n", strerror(errno));
				continue;
			}
		}

        //  Check how many samples the converter wants and see if the
        //  buffer has that right now.  We multiply by 3 because the
        //  FreeDV functions want 8ksps, and we get 24ksps from the
        //  radio.
        nin = freedv_nin(params->fdv);
        int radio_samples = nin * 3;
// 		output("FreeDV wants %d samples, have %d samples\n", radio_samples, ringbuf_bytes_used(rx_input_buffer) / sizeof(float));

        if(ringbuf_bytes_used(params->rx_input_buffer) >= radio_samples * sizeof(float)) {
            //  XXX This should be allocated at loop start and sized
            //  XXX to be sizeof(demod_in) * 3.
            float resample_buffer[radio_samples];
            size_t odone;

            ringbuf_memcpy_from(resample_buffer, params->rx_input_buffer, radio_samples * sizeof(float));

            error = soxr_process (rx_downsampler,
                                  resample_buffer, radio_samples, NULL,
                                  demod_in, nin, &odone);

            if(error)
                output("Sox Error: %s\n", soxr_strerror(error));

            nout = freedv_rx(params->fdv, speech_out, demod_in);
            freedv_send_meters(params->fdv);

            error = soxr_process (rx_upsampler,
                                  speech_out, nout, NULL,
                                  resample_buffer, radio_samples, &odone);

            ringbuf_memcpy_into (rx_output_buffer, resample_buffer, odone * sizeof(float));
        }

        if (ringbuf_bytes_used(rx_output_buffer) >= PACKET_SAMPLES * sizeof(uint32_t)) {
            //  Copy the data into the high side of the buffer so that we can restrucutre it for real/imag format
            ringbuf_memcpy_from (&packet_buffer[PACKET_SAMPLES], rx_output_buffer, sizeof(packet_buffer) / 2);

            for (unsigned int i = PACKET_SAMPLES, j = 0; i < PACKET_SAMPLES * 2; ++i, j += 2)
                packet_buffer[j] = packet_buffer[j + 1] = htonl(packet_buffer[i]);

            vita_send_audio_packet(packet_buffer, sizeof(packet_buffer), 0);
        }

//			} else if ( (buf_desc->stream_id & 0x1u) == 1) { //TX BUFFER
//				if(_end_of_transmission && ringbuf_is_empty(tx_output_buffer))
//					continue;
//
//				for( i = 0 ; i < PACKET_SAMPLES ; i++)
//					packet_buffer[i] = ((Complex *) buf_desc->buf_ptr)[i].real;
//				ringbuf_memcpy_into (params->tx_input_buffer, packet_buffer, sizeof(packet_buffer));
//
//				if(ringbuf_bytes_used(params->tx_input_buffer) >= tx_speech_samples * sizeof(float) * 3) {
//					float resample_buffer[tx_speech_samples * 3];
//					size_t odone;
//
//					ringbuf_memcpy_from(resample_buffer, params->tx_input_buffer, tx_speech_samples * sizeof(float) * 3);
//
//					error = soxr_process (tx_downsampler,
//											resample_buffer, tx_speech_samples * 3, NULL,
//											speech_in, tx_speech_samples, &odone);
//
//					freedv_tx(params->fdv, mod_out, speech_in);
//
//					error = soxr_process (tx_upsampler,
//										  mod_out, tx_modem_samples, NULL,
//										  resample_buffer, tx_modem_samples * 3, &odone);
//
//					ringbuf_memcpy_into (tx_output_buffer, resample_buffer, odone * sizeof(float));
//				}
//
//				if(ringbuf_bytes_used(tx_output_buffer) >= PACKET_SAMPLES * sizeof(float)) {
//					ringbuf_memcpy_from (packet_buffer, tx_output_buffer, sizeof(packet_buffer));
//					for (i = 0; i < PACKET_SAMPLES; ++i)
//						((Complex *) buf_desc->buf_ptr)[i].real =
//							((Complex *) buf_desc->buf_ptr)[i].imag =
//							packet_buffer[i];
//				} else {
//					memset (buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof(Complex));
//				}
//                vita_send_audio_packet(buf_desc);
//
//				//  If we're at the end, drain the buffer
//				if (_end_of_transmission) {
//					while(!ringbuf_is_empty(tx_output_buffer)) {
//						unsigned long n = ringbuf_bytes_used(tx_output_buffer) >= PACKET_SAMPLES ? PACKET_SAMPLES : ringbuf_bytes_used(tx_output_buffer);
//						output("Draining %d bytes from buffer\n", n);
//
//						memset (buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof(Complex));
//
//						ringbuf_memcpy_from (packet_buffer, tx_output_buffer, n);
//
//						for (i = 0; i < n / sizeof(float); ++i)
//							((Complex *) buf_desc->buf_ptr)[i].real =
//								((Complex *) buf_desc->buf_ptr)[i].imag =
//								packet_buffer[i];
//                        vita_send_audio_packet(buf_desc);
//					}
//					output("Buffer drained\n");
//				}
//			}
//		} // While Loop
//		hal_BufferRelease(&buf_desc);
	} // Main Thread Loop
	output("Processing thread stopped...\n");
    pthread_cleanup_pop(!params->running);

	free(speech_in);
    free(speech_out);
    free(demod_in);
	free(mod_out);

	soxr_delete(rx_upsampler);
	soxr_delete(rx_downsampler);
	soxr_delete(tx_upsampler);
	soxr_delete(tx_downsampler);

	ringbuf_free(&rx_output_buffer);
	ringbuf_free(&tx_output_buffer);

	return NULL;
}

static void start_processing_thread(freedv_proc_t params)
{
    static const struct sched_param fifo_param = {
            .sched_priority = 30
    };

	pthread_create(&params->thread, NULL, &_sched_waveform_thread, params);
	pthread_setschedparam(params->thread, SCHED_FIFO, &fifo_param);
}

freedv_proc_t freedv_init(int mode)
{
    freedv_proc_t params = malloc(sizeof(struct freedv_proc_t));

    params->mode = mode;

	sem_init(&params->input_sem, 0, 0);

    if ((params->mode == FREEDV_MODE_700D) || (params->mode == FREEDV_MODE_2020)) {
        struct freedv_advanced adv;
        adv.interleave_frames = 1;
        params->fdv = freedv_open_advanced(params->mode, &adv);
    } else {
        params->fdv = freedv_open(params->mode);
    }

//     freedv_set_squelch_en(params->fdv, 0);

    assert(params->fdv != NULL);

    unsigned long rx_ringbuffer_size = freedv_get_n_max_modem_samples(params->fdv) * sizeof(float) * 4;
    params->rx_input_buffer = ringbuf_new (rx_ringbuffer_size);
    params->tx_input_buffer = ringbuf_new (freedv_get_n_speech_samples(params->fdv) * sizeof(float) * 4);

    start_processing_thread(params);

    return params;
}

void freedv_set_mode(freedv_proc_t params, int mode)
{
    int ret;

    // TODO:  This can be called before there's a params!
    //        Also needs to be rewritten with the restructuring.
    //        freedv_init needs to be called again if we're running.
    params->mode = mode;

    //  If the thread is running, we need to restart it.
    ret = pthread_tryjoin_np(params->thread, NULL);
    if (ret == 0 && errno == EBUSY) {
        output("Stopping Thread...\n");
        params->running = 0;
        pthread_join(params->thread, NULL);
        output("Restarting thread with new mode...\n");

//        start_processing_thread();
    }
}

void sched_waveformThreadExit(freedv_proc_t params)
{
    if (params == NULL)
        return;

	params->running = 0;
	pthread_join(params->thread, NULL);
}