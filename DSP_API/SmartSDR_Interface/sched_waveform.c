///*!   \file sched_waveform.c
// *    \brief Schedule Wavefrom Streams
// *
// *    \copyright  Copyright 2012-2014 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 29-AUG-2014
// *    \author 	Ed Gonzalez
// *    \mangler 	Graham / KE9H
// *
// */

/* *****************************************************************************
 *
 *  Copyright (C) 2014 FlexRadio Systems.
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
 * ************************************************************************** */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include "common.h"
#include "datatypes.h"
#include "hal_buffer.h"
#include "sched_waveform.h"
#include "vita_output.h"
#include "freedv_api.h"
#include "modem_stats.h"
#include "circular_buffer.h"  // Remove me!
#include "resampler.h" // Remove me!
#include "ringbuf.h"

#include "soxr.h"

static pthread_rwlock_t _list_lock;
static BufferDescriptor _root;

static pthread_t _waveform_thread;
static BOOL _waveform_thread_abort = FALSE;

static sem_t sched_waveform_sem;

struct freedv_params {
	int mode;
};

static void _dsp_convertBufEndian(BufferDescriptor buf_desc)
{
	int i;

	if(buf_desc->sample_size != 8)
		return;

	for(i = 0; i < buf_desc->num_samples*2; i++)
		((int32*)buf_desc->buf_ptr)[i] = htonl(((int32*)buf_desc->buf_ptr)[i]);
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
		if(!buf_desc || !buf_desc->prev || !buf_desc->next)
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

#define SCALE_TX_IN     24000.0 	// Multiplier   // Was 16000 GGH Jan 30, 2015
#define SCALE_TX_OUT    24000.0 	// Divisor

#define FILTER_TAPS	48
#define DECIMATION_FACTOR 	3

/* These are offsets for the input buffers to decimator */
#define MEM_24		FILTER_TAPS					   /* Memory required in 24kHz buffer */
#define MEM_8		FILTER_TAPS/DECIMATION_FACTOR   /* Memory required in 8kHz buffer */

static struct freedv *_freedvS;         // Initialize Coder structure
static struct my_callback_state  _my_cb_state;

#define MAX_RX_STRING_LENGTH 40
static char _rx_string[MAX_RX_STRING_LENGTH + 5];

static BOOL _end_of_transmission = FALSE;

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Circular Buffer Declarations

float TX1_buff[(PACKET_SAMPLES * 12) +1];		// TX1 Packet Input Buffer
short TX2_buff[(PACKET_SAMPLES * 12)+1];		// TX2 Vocoder input buffer
short TX3_buff[(PACKET_SAMPLES * 12)+1];		// TX3 Vocoder output buffer
float TX4_buff[(PACKET_SAMPLES * 12)+1];		// TX4 Packet output Buffer

circular_float_buffer tx1_cb;
Circular_Float_Buffer TX1_cb = &tx1_cb;
circular_short_buffer tx2_cb;
Circular_Short_Buffer TX2_cb = &tx2_cb;
circular_short_buffer tx3_cb;
Circular_Short_Buffer TX3_cb = &tx3_cb;
circular_float_buffer tx4_cb;
Circular_Float_Buffer TX4_cb = &tx4_cb;

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Callbacks for embedded ASCII stream, transmit and receive

void my_put_next_rx_char(void *callback_state, char c)
{
    char new_char[2];
    if ( (uint32) c < 32 || (uint32) c > 126 ) {
    	/* Treat all control chars as spaces */
    	//output(ANSI_YELLOW "Non-valid RX_STRING char. ASCII code = %d\n", (uint32) c);
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

    char* api_cmd = safe_malloc(80);
    sprintf(api_cmd, "waveform status slice=%d string=\"%s\"",0,_rx_string);
    tc_sendSmartSDRcommand(api_cmd,FALSE,NULL);
    safe_free(api_cmd);
}

struct my_callback_state
{
    char  tx_str[80];
    char *ptx_str;
};

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

void freedv_set_string(uint32 slice, char* string)
{
    strcpy(_my_cb_state.tx_str, string);
    _my_cb_state.ptx_str = _my_cb_state.tx_str;
    output(ANSI_MAGENTA "new TX string is '%s'\n",string);
}

void sched_waveform_setEndOfTX(BOOL end_of_transmission)
{
    _end_of_transmission = TRUE;
}

static void* _sched_waveform_thread(void *arg)
{
    int 	nin, nout, radio_samples;
    int		i;			// for loop counter
    int		ret;
    float	fsample;	// a float sample

    // Flags ...
    int		initial_tx = 1; 		// Flags for TX circular buffer, clear if starting transmit
    int		initial_rx = 1;			// Flags for RX circular buffer, clear if starting receive

	short *speech_in;
	short *speech_out;
	short *demod_in;
	short *mod_out;

	struct freedv_params *params = (struct freedv_params *) arg;

	float packet_buffer[PACKET_SAMPLES];

    // TX RESAMPLER I/O BUFFERS
    float 	tx_float_in_8k[PACKET_SAMPLES + FILTER_TAPS];
    float 	tx_float_out_8k[PACKET_SAMPLES];

    float 	tx_float_in_24k[PACKET_SAMPLES * DECIMATION_FACTOR + FILTER_TAPS];
    float 	tx_float_out_24k[PACKET_SAMPLES * DECIMATION_FACTOR ];

    BOOL inhibit_tx = FALSE;
    BOOL flush_tx = FALSE;

	soxr_error_t error;

	soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_INT16_I);
    soxr_t rx_downsampler = soxr_create(24000, 8000, 1, &error, &io_spec, NULL, NULL);

    io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_FLOAT32_I);
	soxr_t rx_upsampler = soxr_create(8000, 24000, 1, &error, &io_spec, NULL, NULL);

    // =======================  Initialization Section =========================
    if ((params->mode == FREEDV_MODE_700D) || (params->mode == FREEDV_MODE_2020)) {
        struct freedv_advanced adv;
        adv.interleave_frames = 1;
        _freedvS = freedv_open_advanced(params->mode, &adv);
    } else {
        _freedvS = freedv_open(params->mode);
    }

    assert(_freedvS != NULL);

    //  XXX Check these sizes.
    int rx_ringbuffer_size = freedv_get_n_max_modem_samples(_freedvS) * sizeof(float) * 4;
    ringbuf_t rx_input_buffer = ringbuf_new (rx_ringbuffer_size);
    ringbuf_t rx_output_buffer = ringbuf_new (rx_ringbuffer_size);

    freedv_set_squelch_en(_freedvS, 0);

    //  Allocate buffers
    speech_in = (short *) safe_malloc(freedv_get_n_speech_samples(_freedvS) * sizeof(short));
    speech_out = (short *) safe_malloc(freedv_get_n_speech_samples(_freedvS) * sizeof(short));
    demod_in = (short *) safe_malloc(freedv_get_n_max_modem_samples(_freedvS) * sizeof(short));
    mod_out = (short *) safe_malloc(freedv_get_n_nom_modem_samples(_freedvS) * sizeof(short));
    //  XXX Probably should memalign here.

    // Initialize the Circular Buffers
	TX1_cb->size  = PACKET_SAMPLES * 6 + 1;		// size = no.elements in array+1
	TX1_cb->start = 0;
	TX1_cb->end	  = 0;
	TX1_cb->elems = TX1_buff;

	TX2_cb->size  = PACKET_SAMPLES * 6 + 1;		// size = no.elements in array+1
	TX2_cb->start = 0;
	TX2_cb->end	  = 0;
	TX2_cb->elems = TX2_buff;

	TX3_cb->size  = PACKET_SAMPLES * 6 + 1;		// size = no.elements in array+1
	TX3_cb->start = 0;
	TX3_cb->end	  = 0;
	TX3_cb->elems = TX3_buff;

	TX4_cb->size  = PACKET_SAMPLES * 12 + 1;		// size = no.elements in array+1
	TX4_cb->start = 0;
	TX4_cb->end	  = 0;
	TX4_cb->elems = TX4_buff;

	initial_tx = TRUE;
	initial_rx = TRUE;

	struct timespec timeout;

    // Clear TX string
    memset(_my_cb_state.tx_str, 0, sizeof(_my_cb_state.tx_str));
    _my_cb_state.ptx_str = _my_cb_state.tx_str;
    freedv_set_callback_txt(_freedvS, &my_put_next_rx_char, &my_get_next_tx_char, &_my_cb_state);

	// show that we are running
	BufferDescriptor buf_desc;

	output("Starting processing thread...\n");

	while (_waveform_thread_abort == FALSE) {
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

		while(buf_desc = _WaveformList_UnlinkHead()) {
			// convert the buffer to little endian
			_dsp_convertBufEndian(buf_desc);

			if ((buf_desc->stream_id & 1) == 0) {
				//	Set the transmit 'initial' flag
				initial_tx = TRUE;
				inhibit_tx = FALSE;
				flush_tx = FALSE;
				_end_of_transmission = FALSE;

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
					struct MODEM_STATS stats;

					ringbuf_memcpy_from(resample_buffer, rx_input_buffer, radio_samples * sizeof(float));

					error = soxr_process (rx_downsampler,
										  resample_buffer, radio_samples, NULL,
										  demod_in, nin, &odone);

					if(error)
						output("Sox Error: %s\n", soxr_strerror(error));

					nout = freedv_rx(_freedvS, speech_out, demod_in);
					freedv_get_modem_extended_stats(_freedvS, &stats);

					if (stats.sync) {
						output("SNR: %f\n", stats.snr_est);
						output("Frequency Offset: %3.1f\n", stats.foff);
						output("Clock Offset: %5d\n", (int) round(stats.clock_offset * 1E6));
						output("Sync Quality: %3.2f\n", stats.sync_metric);
						output("\n");
					}

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
				emit_waveform_output(buf_desc);
			} else if ( (buf_desc->stream_id & 1) == 1) { //TX BUFFER
				//	If 'initial_rx' flag, clear buffers TX1, TX2, TX3, TX4
				if(initial_tx) {
					TX1_cb->start = 0;	// Clear buffers RX1, RX2, RX3, RX4
					TX1_cb->end	  = 0;
					TX2_cb->start = 0;
					TX2_cb->end	  = 0;
					TX3_cb->start = 0;
					TX3_cb->end	  = 0;
					TX4_cb->start = 0;
					TX4_cb->end	  = 0;

					/* Clear filter memory */

					memset(tx_float_in_24k, 0, MEM_24 * sizeof(float));
					memset(tx_float_in_8k, 0, MEM_8 * sizeof(float));

					/* Requires us to set initial_rx to FALSE which we do at the end of
					 * the first loop
					 */
				}

				initial_rx = TRUE;
				// Check for new receiver input packet & move to TX1_cb.
				// TODO - If transmit packet, discard here?

				if ( !inhibit_tx ) {
					for( i = 0 ; i < PACKET_SAMPLES ; i++ )
						cbWriteFloat(TX1_cb, ((Complex*)buf_desc->buf_ptr)[i].real);

					// Check for >= 384 samples in TX1_cb and spin downsampler
					//	Convert to shorts and move to TX2_cb.
					if(cfbContains(TX1_cb) >= 384) {
						for(i=0 ; i<384 ; i++)
							tx_float_in_24k[i + MEM_24] = cbReadFloat(TX1_cb);

						fdmdv_24_to_8(tx_float_out_8k, &tx_float_in_24k[MEM_24], 128);

						for(i=0 ; i<128 ; i++)
							cbWriteShort(TX2_cb, (short) (tx_float_out_8k[i]*SCALE_TX_IN));
					}

//						// Check for >= 320 samples in TX2_cb and spin vocoder
					// 	Move output to TX3_cb.
						if ( csbContains(TX2_cb) >= 320 ) {
							for( i=0 ; i< 320 ; i++)
								speech_in[i] = cbReadShort(TX2_cb);

							freedv_tx(_freedvS, mod_out, speech_in);

							for( i=0 ; i < 320 ; i++)
								cbWriteShort(TX3_cb, mod_out[i]);
						}

					// Check for >= 128 samples in TX3_cb, convert to floats
					//	and spin the upsampler. Move output to TX4_cb.

					if(csbContains(TX3_cb) >= 128) {
						for( i=0 ; i<128 ; i++)
							tx_float_in_8k[i+MEM_8] = ((float)  (cbReadShort(TX3_cb) / SCALE_TX_OUT));

						fdmdv_8_to_24(tx_float_out_24k, &tx_float_in_8k[MEM_8], 128);

						for( i=0 ; i<384 ; i++)
							cbWriteFloat(TX4_cb, tx_float_out_24k[i]);
						//Sig2Noise = (_freedvS->fdmdv_stats.snr_est);
					}
				}
				// Check for >= 128 samples in RX4_cb. Form packet and
				//	export.

				uint32 tx_check_samples = PACKET_SAMPLES;

				if(initial_tx)
					tx_check_samples = PACKET_SAMPLES * 3;

				if ( _end_of_transmission )
					flush_tx = TRUE;

				if ( !inhibit_tx ) {
					if(cfbContains(TX4_cb) >= tx_check_samples ) {
						for( i = 0 ; i < PACKET_SAMPLES ; i++) {
							//output("Fetching from end buffer \n");
							// Set up the outbound packet
							fsample = cbReadFloat(TX4_cb);
							// put the fsample into the outbound packet
							((Complex*)buf_desc->buf_ptr)[i].real = fsample;
							((Complex*)buf_desc->buf_ptr)[i].imag = fsample;
						}
					} else {
						memset( buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof(Complex));

						if(initial_tx)
							initial_tx = FALSE;
					}

					emit_waveform_output(buf_desc);

					if ( flush_tx ) {
						inhibit_tx = TRUE;

						while ( cfbContains(TX4_cb) > 0 ) {
							if ( cfbContains(TX4_cb) > PACKET_SAMPLES ) {
								for( i = 0 ; i < PACKET_SAMPLES ; i++) {
									// Set up the outbound packet
									fsample = cbReadFloat(TX4_cb);

									// put the fsample into the outbound packet
									((Complex*)buf_desc->buf_ptr)[i].real = fsample;
									((Complex*)buf_desc->buf_ptr)[i].imag = fsample;
								}
							} else {
								int end_index = 0;
								for ( i = 0 ; i <= cfbContains(TX4_cb); i++ ) {
									fsample = cbReadFloat(TX4_cb);
									((Complex*)buf_desc->buf_ptr)[i].real = fsample;
									((Complex*)buf_desc->buf_ptr)[i].imag = fsample;
									end_index = i+1;
								}

								for ( i = end_index ; i < PACKET_SAMPLES ; i++ ) {
									((Complex*)buf_desc->buf_ptr)[i].real = 0.0f;
									((Complex*)buf_desc->buf_ptr)[i].imag = 0.0f;
								}

							}
							emit_waveform_output(buf_desc);
						}
					}
				}
			}
		} // While Loop
		hal_BufferRelease(&buf_desc);
	} // For Loop
	output("Processing thread stopped...\n");
	_waveform_thread_abort = TRUE;
	 freedv_close(_freedvS);

	free(speech_in);
    free(speech_out);
    free(demod_in);
	free(mod_out);

	ringbuf_free(&rx_input_buffer);
	ringbuf_free(&rx_output_buffer);

	free(params);

	return NULL;
}

static void start_processing_thread(int mode)
{
	struct freedv_params *params;
	params = (struct freedv_params *) malloc(sizeof(struct freedv_params));

	params->mode = mode;

	pthread_create(&_waveform_thread, NULL, &_sched_waveform_thread, params);

	struct sched_param fifo_param;
	fifo_param.sched_priority = 30;
	pthread_setschedparam(_waveform_thread, SCHED_FIFO, &fifo_param);
}

void sched_waveform_Init(void)
{
	pthread_rwlock_init(&_list_lock, NULL);

	pthread_rwlock_wrlock(&_list_lock);
	_root = (BufferDescriptor)safe_malloc(sizeof(buffer_descriptor));
	memset(_root, 0, sizeof(buffer_descriptor));
	_root->next = _root;
	_root->prev = _root;
	pthread_rwlock_unlock(&_list_lock);

	sem_init(&sched_waveform_sem, 0, 0);

	start_processing_thread(FREEDV_MODE_1600);
}

void freedv_set_mode(int mode)
{
	output("Stopping Thread...\n");
	_waveform_thread_abort = TRUE;
	pthread_join(_waveform_thread, NULL);
	output("Restarting thread with new mode...\n");

	_waveform_thread_abort = FALSE;
	start_processing_thread(mode);
}

void sched_waveformThreadExit()
{
	_waveform_thread_abort = TRUE;
	sem_post(&sched_waveform_sem);
}

uint32 cmd_freedv_mode(int requester_fd, int argc, char **argv)
{
	assert (argv != NULL);
	assert (argv[0] != NULL);

	if(argc != 2) {
		output("Usage: freedv-mode <1600|700D>\n");
		return SL_BAD_COMMAND;
	}

	if(strncmp(argv[1], "1600\n", 4) == 0) {
		output("Changing to 1600");
		freedv_set_mode(FREEDV_MODE_1600);
	} else if(strncmp(argv[1], "700D", 4) == 0) {
		output("Changing to 700D\n");
		freedv_set_mode(FREEDV_MODE_700D);
	} else {
		output("Unknown mode %s\n", argv[1]);
	}
	return SUCCESS;
}

