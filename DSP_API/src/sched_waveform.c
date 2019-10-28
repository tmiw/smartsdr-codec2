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

#include "common.h"
#include "hal_buffer.h"
#include "sched_waveform.h"
#include "vita-io.h"
#include "modem_stats.h"
#include "ringbuf.h"
#include "api.h"

#include "soxr.h"

static pthread_rwlock_t _list_lock;
static BufferDescriptor _root;

static pthread_t _waveform_thread;
static bool _waveform_thread_abort = false;

static sem_t sched_waveform_sem;

static int freedv_mode = FREEDV_MODE_1600;

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

static void _dsp_convertBufEndian(BufferDescriptor buf_desc)
{
	unsigned int i;

	if(buf_desc->sample_size != 8)
		return;

	for(i = 0; i < buf_desc->num_samples*2; i++)
		((int32_t*)buf_desc->buf_ptr)[i] = htonl(((int32_t*)buf_desc->buf_ptr)[i]);
}

static BufferDescriptor _WaveformList_UnlinkHead(void)
{
	BufferDescriptor buf_desc = NULL;
	pthread_rwlock_wrlock(&_list_lock);

	if (_root == NULL || _root->next == NULL)
	{
		output("Attempt to unlink from a NULL head");
		pthread_rwlock_unlock(&_list_lock);
		return NULL;
	}

	if(_root->next != _root)
		buf_desc = _root->next;

	if(buf_desc != NULL)
	{
		// make sure buffer exists and is actually linked
		if(!buf_desc->prev || !buf_desc->next)
		{
			output( "Invalid buffer descriptor");
			buf_desc = NULL;
		}
		else
		{
			buf_desc->next->prev = buf_desc->prev;
			buf_desc->prev->next = buf_desc->next;
			buf_desc->next = NULL;
			buf_desc->prev = NULL;
		}
	}

	pthread_rwlock_unlock(&_list_lock);
	return buf_desc;
}

static void _WaveformList_LinkTail(BufferDescriptor buf_desc)
{
	pthread_rwlock_wrlock(&_list_lock);
	buf_desc->next = _root;
	buf_desc->prev = _root->prev;
	_root->prev->next = buf_desc;
	_root->prev = buf_desc;
	pthread_rwlock_unlock(&_list_lock);
}

void sched_waveform_Schedule(BufferDescriptor buf_desc)
{
	_WaveformList_LinkTail(buf_desc);
	sem_post(&sched_waveform_sem);
}

void sched_waveform_signal()
{
	sem_post(&sched_waveform_sem);
}

/* *********************************************************************************************
 * *********************************************************************************************
 * *********************                                                 ***********************
 * *********************  LOCATION OF MODULATOR / DEMODULATOR INTERFACE  ***********************
 * *********************                                                 ***********************
 * *********************************************************************************************
 * ****************************************************************************************** */

#define PACKET_SAMPLES  128

//static struct my_callback_state  _my_cb_state;
static struct my_callback_state
{
    char  tx_str[80];
    char *ptx_str;
} _my_cb_state;

#define MAX_RX_STRING_LENGTH 40
static char _rx_string[MAX_RX_STRING_LENGTH + 5];

static bool _end_of_transmission = false;

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

static void* _sched_waveform_thread()
{
    int 	nin, nout;
    unsigned long		i;
    int		ret;

	float packet_buffer[PACKET_SAMPLES];
	struct timespec timeout;

	BufferDescriptor buf_desc;

	struct freedv *_freedvS;         // Initialize Coder structure

	soxr_error_t error;
	soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    soxr_t rx_downsampler = soxr_create(24000, 8000, 1, &error, &io_spec, NULL, NULL);
    soxr_t tx_downsampler = soxr_create(24000, 8000, 1, &error, &io_spec, NULL, NULL);

    io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_FLOAT32_I);
	soxr_t rx_upsampler = soxr_create(8000, 24000, 1, &error, &io_spec, NULL, NULL);
	soxr_t tx_upsampler = soxr_create(8000, 24000, 1, &error, &io_spec, NULL, NULL);

    if ((freedv_mode == FREEDV_MODE_700D) || (freedv_mode == FREEDV_MODE_2020)) {
        struct freedv_advanced adv;
        adv.interleave_frames = 1;
        _freedvS = freedv_open_advanced(freedv_mode, &adv);
    } else {
        _freedvS = freedv_open(freedv_mode);
    }

//     freedv_set_squelch_en(_freedvS, 0);

    assert(_freedvS != NULL);

	int tx_speech_samples = freedv_get_n_speech_samples(_freedvS);
	int tx_modem_samples = freedv_get_n_nom_modem_samples(_freedvS);
    unsigned long rx_ringbuffer_size = freedv_get_n_max_modem_samples(_freedvS) * sizeof(float) * 4;

    ringbuf_t rx_input_buffer = ringbuf_new (rx_ringbuffer_size);
    ringbuf_t rx_output_buffer = ringbuf_new (rx_ringbuffer_size);

    ringbuf_t tx_input_buffer = ringbuf_new (tx_speech_samples * sizeof(float) * 4);
    ringbuf_t tx_output_buffer = ringbuf_new (tx_modem_samples * sizeof(float) * 4);

    short *speech_in = (short *) malloc(freedv_get_n_speech_samples(_freedvS) * sizeof(short));
    short *speech_out = (short *) malloc(freedv_get_n_speech_samples(_freedvS) * sizeof(short));
    short *demod_in = (short *) malloc(freedv_get_n_max_modem_samples(_freedvS) * sizeof(short));
    short *mod_out = (short *) malloc(freedv_get_n_nom_modem_samples(_freedvS) * sizeof(short));
    //  XXX Probably should memalign here.

    // Clear TX string
    memset(_my_cb_state.tx_str, 0, sizeof(_my_cb_state.tx_str));
    _my_cb_state.ptx_str = _my_cb_state.tx_str;
    freedv_set_callback_txt(_freedvS, &my_put_next_rx_char, &my_get_next_tx_char, &_my_cb_state);

    if (meter_table[0].id == 0)
        register_meters(meter_table);

    // show that we are running

	_waveform_thread_abort = false;
	output("Starting processing thread...\n");

	while (_waveform_thread_abort == false) {
		// wait for a buffer descriptor to get posted
		if(clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
			output("Couldn't get time.\n");
			continue;
		}
		timeout.tv_sec += 1;

		while((ret = sem_timedwait(&sched_waveform_sem, &timeout)) == -1 && errno == EINTR)
			continue;

		if(ret == -1) {
			if(errno == ETIMEDOUT) {
				continue;
			} else {
				output("Error acquiring semaphore: %s\n", strerror(errno));
				continue;
			}
		}

		while((buf_desc = _WaveformList_UnlinkHead())) {
			// convert the buffer to little endian
			_dsp_convertBufEndian(buf_desc);

			if ((buf_desc->stream_id & 1u) == 0) {
				//	Set the transmit 'initial' flag
				_end_of_transmission = false;

				for( i = 0 ; i < PACKET_SAMPLES ; i++)
					packet_buffer[i] = ((Complex *) buf_desc->buf_ptr)[i].real;
				ringbuf_memcpy_into (rx_input_buffer, packet_buffer, sizeof(packet_buffer));

				//  Check how many samples the converter wants and see if the
				//  buffer has that right now.  We multiply by 3 because the
				//  FreeDV functions want 8ksps, and we get 24ksps from the
				//  radio.
				nin = freedv_nin(_freedvS);
				int radio_samples = nin * 3;
// 				output("FreeDV wants %d samples, have %d samples\n", radio_samples, ringbuf_bytes_used(rx_input_buffer) / sizeof(float));

				if(ringbuf_bytes_used(rx_input_buffer) >= radio_samples * sizeof(float)) {
					//  XXX This should be allocated at loop start and sized
					//  XXX to be sizeof(demod_in) * 3.
					float resample_buffer[radio_samples];
					size_t odone;

					ringbuf_memcpy_from(resample_buffer, rx_input_buffer, radio_samples * sizeof(float));

					error = soxr_process (rx_downsampler,
										  resample_buffer, radio_samples, NULL,
										  demod_in, nin, &odone);

					if(error)
						output("Sox Error: %s\n", soxr_strerror(error));

					nout = freedv_rx(_freedvS, speech_out, demod_in);
                    freedv_send_meters(_freedvS);

					error = soxr_process (rx_upsampler,
					                      speech_out, nout, NULL,
					                      resample_buffer, radio_samples, &odone);

					ringbuf_memcpy_into (rx_output_buffer, resample_buffer, odone * sizeof(float));
				}

				if (ringbuf_bytes_used(rx_output_buffer) >= PACKET_SAMPLES * sizeof(float)) {
					ringbuf_memcpy_from (packet_buffer, rx_output_buffer, sizeof(packet_buffer));
					for (i = 0; i < PACKET_SAMPLES; ++i)
						((Complex *) buf_desc->buf_ptr)[i].real =
							((Complex *) buf_desc->buf_ptr)[i].imag =
							packet_buffer[i];
				} else {
					memset (buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof(Complex));
				}
                vita_send_audio_packet(buf_desc);
			} else if ( (buf_desc->stream_id & 0x1u) == 1) { //TX BUFFER
				if(_end_of_transmission && ringbuf_is_empty(tx_output_buffer))
					continue;

				for( i = 0 ; i < PACKET_SAMPLES ; i++)
					packet_buffer[i] = ((Complex *) buf_desc->buf_ptr)[i].real;
				ringbuf_memcpy_into (tx_input_buffer, packet_buffer, sizeof(packet_buffer));

				if(ringbuf_bytes_used(tx_input_buffer) >= tx_speech_samples * sizeof(float) * 3) {
					float resample_buffer[tx_speech_samples * 3];
					size_t odone;

					ringbuf_memcpy_from(resample_buffer, tx_input_buffer, tx_speech_samples * sizeof(float) * 3);

					error = soxr_process (tx_downsampler,
											resample_buffer, tx_speech_samples * 3, NULL,
											speech_in, tx_speech_samples, &odone);

					freedv_tx(_freedvS, mod_out, speech_in);

					error = soxr_process (tx_upsampler,
										  mod_out, tx_modem_samples, NULL,
										  resample_buffer, tx_modem_samples * 3, &odone);

					ringbuf_memcpy_into (tx_output_buffer, resample_buffer, odone * sizeof(float));
				}

				if(ringbuf_bytes_used(tx_output_buffer) >= PACKET_SAMPLES * sizeof(float)) {
					ringbuf_memcpy_from (packet_buffer, tx_output_buffer, sizeof(packet_buffer));
					for (i = 0; i < PACKET_SAMPLES; ++i)
						((Complex *) buf_desc->buf_ptr)[i].real =
							((Complex *) buf_desc->buf_ptr)[i].imag =
							packet_buffer[i];
				} else {
					memset (buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof(Complex));
				}
                vita_send_audio_packet(buf_desc);

				//  If we're at the end, drain the buffer
				if (_end_of_transmission) {
					while(!ringbuf_is_empty(tx_output_buffer)) {
						unsigned long n = ringbuf_bytes_used(tx_output_buffer) >= PACKET_SAMPLES ? PACKET_SAMPLES : ringbuf_bytes_used(tx_output_buffer);
						output("Draining %d bytes from buffer\n", n);

						memset (buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof(Complex));

						ringbuf_memcpy_from (packet_buffer, tx_output_buffer, n);

						for (i = 0; i < n / sizeof(float); ++i)
							((Complex *) buf_desc->buf_ptr)[i].real =
								((Complex *) buf_desc->buf_ptr)[i].imag =
								packet_buffer[i];
                        vita_send_audio_packet(buf_desc);
					}
					output("Buffer drained\n");
				}
			}
		} // While Loop
		hal_BufferRelease(&buf_desc);
	} // For Loop
	output("Processing thread stopped...\n");
	_waveform_thread_abort = true;
	 freedv_close(_freedvS);

	free(speech_in);
    free(speech_out);
    free(demod_in);
	free(mod_out);

	ringbuf_free(&rx_input_buffer);
	ringbuf_free(&rx_output_buffer);
	ringbuf_free(&tx_input_buffer);
	ringbuf_free(&tx_output_buffer);

	return NULL;
}

static void start_processing_thread()
{
	pthread_create(&_waveform_thread, NULL, &_sched_waveform_thread, NULL);

	struct sched_param fifo_param;
	fifo_param.sched_priority = 30;
	pthread_setschedparam(_waveform_thread, SCHED_FIFO, &fifo_param);
}

void sched_waveform_Init(void)
{
	pthread_rwlock_init(&_list_lock, NULL);

	pthread_rwlock_wrlock(&_list_lock);
	_root = (BufferDescriptor)malloc(sizeof(buffer_descriptor));
	memset(_root, 0, sizeof(buffer_descriptor));
	_root->next = _root;
	_root->prev = _root;
	pthread_rwlock_unlock(&_list_lock);

	sem_init(&sched_waveform_sem, 0, 0);

	start_processing_thread();
}

void freedv_set_mode(int mode)
{
    int ret;

    freedv_mode = mode;

    //  If the thread is running, we need to restart it.
    ret = pthread_tryjoin_np(_waveform_thread, NULL);
    if (ret == 0 && errno == EBUSY) {
        output("Stopping Thread...\n");
        _waveform_thread_abort = true;
        pthread_join(_waveform_thread, NULL);
        output("Restarting thread with new mode...\n");

        _waveform_thread_abort = false;
        start_processing_thread();
    }
}

void sched_waveformThreadExit()
{
	_waveform_thread_abort = true;
	sem_post(&sched_waveform_sem);
	pthread_join(_waveform_thread, NULL);
}

