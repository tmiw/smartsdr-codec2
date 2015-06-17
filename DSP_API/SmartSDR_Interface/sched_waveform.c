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

#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>		// for memset
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>

#include "common.h"
#include "datatypes.h"
#include "hal_buffer.h"
#include "sched_waveform.h"
#include "vita_output.h"
#include "thumbDV.h"
#include "bit_pattern_matcher.h"
#include "dstar.h"
#include "DStarDefines.h"


//static Queue sched_fft_queue;
static pthread_rwlock_t _list_lock;
static BufferDescriptor _root;

static pthread_t _waveform_thread;
static BOOL _waveform_thread_abort = FALSE;

static sem_t sched_waveform_sem;

static void _dsp_convertBufEndian(BufferDescriptor buf_desc)
{
	int i;

	if(buf_desc->sample_size != 8)
	{
		//TODO: horrendous error here
		return;
	}

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

#include <stdio.h>
#include "circular_buffer.h"
#include "resampler.h"

#include "gmsk_modem.h"

#define PACKET_SAMPLES  128
#define DV_PACKET_SAMPLES 160

#define SCALE_AMBE      32767.0f
//
//#define SCALE_RX_IN      32767.0f   // Multiplier   // Was 16000 GGH Jan 30, 2015
//#define SCALE_RX_OUT     32767.0f       // Divisor
//#define SCALE_TX_IN     32767.0f    // Multiplier   // Was 16000 GGH Jan 30, 2015
//#define SCALE_TX_OUT    32767.0f    // Divisor

#define SCALE_RX_IN     SCALE_AMBE
#define SCALE_TX_OUT    SCALE_AMBE


#define SCALE_RX_OUT    SCALE_AMBE
#define SCALE_TX_IN     SCALE_AMBE


#define FILTER_TAPS	48
#define DECIMATION_FACTOR 	3

/* These are offsets for the input buffers to decimator */
#define MEM_24		FILTER_TAPS					   /* Memory required in 24kHz buffer */
#define MEM_8		FILTER_TAPS/DECIMATION_FACTOR   /* Memory required in 8kHz buffer */

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Circular Buffer Declarations

float RX1_buff[(DV_PACKET_SAMPLES * 12)+1];		// RX1 Packet Input Buffer
short RX2_buff[(DV_PACKET_SAMPLES * 12)+1];		// RX2 Vocoder input buffer
short RX3_buff[(DV_PACKET_SAMPLES * 12)+1];		// RX3 Vocoder output buffer
float RX4_buff[(DV_PACKET_SAMPLES * 12)+1];		// RX4 Packet output Buffer

float TX1_buff[(DV_PACKET_SAMPLES * 12) +1];		// TX1 Packet Input Buffer
short TX2_buff[(DV_PACKET_SAMPLES * 12)+1];		// TX2 Vocoder input buffer
short TX3_buff[(DV_PACKET_SAMPLES * 12)+1];		// TX3 Vocoder output buffer
float TX4_buff[(DV_PACKET_SAMPLES * 12)+1];		// TX4 Packet output Buffer

circular_float_buffer rx1_cb;
Circular_Float_Buffer RX1_cb = &rx1_cb;
circular_short_buffer rx2_cb;
Circular_Short_Buffer RX2_cb = &rx2_cb;
circular_short_buffer rx3_cb;
Circular_Short_Buffer RX3_cb = &rx3_cb;
circular_float_buffer rx4_cb;
Circular_Float_Buffer RX4_cb = &rx4_cb;

circular_float_buffer tx1_cb;
Circular_Float_Buffer TX1_cb = &tx1_cb;
circular_short_buffer tx2_cb;
Circular_Short_Buffer TX2_cb = &tx2_cb;
circular_short_buffer tx3_cb;
Circular_Short_Buffer TX3_cb = &tx3_cb;
circular_float_buffer tx4_cb;
Circular_Float_Buffer TX4_cb = &tx4_cb;

static int _dv_serial_fd = 0;

static GMSK_DEMOD _gmsk_demod = NULL;
static GMSK_MOD   _gmsk_mod = NULL;
static DSTAR_MACHINE _dstar = NULL;

static BOOL write_dat = TRUE;
static uint32 data_i = 0;

#define FREEDV_NSAMPLES 160

static void* _sched_waveform_thread(void* param)
{
    int 	nout;

    int		i;			// for loop counter
    float	fsample;	// a float sample
//    float   Sig2Noise;	// Signal to noise ratio

    // Flags ...
    int		initial_tx = 1; 		// Flags for TX circular buffer, clear if starting transmit
    int		initial_rx = 1;			// Flags for RX circular buffer, clear if starting receive

	// VOCODER I/O BUFFERS
    short	speech_in[FREEDV_NSAMPLES];
    short 	speech_out[FREEDV_NSAMPLES];
    //short 	demod_in[FREEDV_NSAMPLES];
    short 	mod_out[FREEDV_NSAMPLES];

    //unsigned char packet_out[FREEDV_NSAMPLES];

    // RX RESAMPLER I/O BUFFERS
    float 	float_in_8k[DV_PACKET_SAMPLES + FILTER_TAPS];
    //float 	float_out_8k[DV_PACKET_SAMPLES];

    float 	float_in_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR + FILTER_TAPS];
    float 	float_out_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR ];

    // TX RESAMPLER I/O BUFFERS
    float 	tx_float_in_8k[DV_PACKET_SAMPLES + FILTER_TAPS];
    float 	tx_float_out_8k[DV_PACKET_SAMPLES];

    float 	tx_float_in_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR + FILTER_TAPS];
    float 	tx_float_out_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR ];



    // =======================  Initialization Section =========================

    thumbDV_init("/dev/ttyUSB0", &_dv_serial_fd);

    // Initialize the Circular Buffers

	RX1_cb->size  = PACKET_SAMPLES*6 +1;		// size = no.elements in array+1
	RX1_cb->start = 0;
	RX1_cb->end	  = 0;
	RX1_cb->elems = RX1_buff;
	RX2_cb->size  = PACKET_SAMPLES*6 +1;		// size = no.elements in array+1
	RX2_cb->start = 0;
	RX2_cb->end	  = 0;
	RX2_cb->elems = RX2_buff;
	RX3_cb->size  = PACKET_SAMPLES*6 +1;		// size = no.elements in array+1
	RX3_cb->start = 0;
	RX3_cb->end	  = 0;
	RX3_cb->elems = RX3_buff;
	RX4_cb->size  = PACKET_SAMPLES*12 +1;		// size = no.elements in array+1
	RX4_cb->start = 0;
	RX4_cb->end	  = 0;
	RX4_cb->elems = RX4_buff;

	TX1_cb->size  = PACKET_SAMPLES*6 +1;		// size = no.elements in array+1
	TX1_cb->start = 0;
	TX1_cb->end	  = 0;
	TX1_cb->elems = TX1_buff;
	TX2_cb->size  = PACKET_SAMPLES*6 +1;		// size = no.elements in array+1
	TX2_cb->start = 0;
	TX2_cb->end	  = 0;
	TX2_cb->elems = TX2_buff;
	TX3_cb->size  = PACKET_SAMPLES *6 +1;		// size = no.elements in array+1
	TX3_cb->start = 0;
	TX3_cb->end	  = 0;
	TX3_cb->elems = TX3_buff;
	TX4_cb->size  = PACKET_SAMPLES *12 +1;		// size = no.elements in array+1
	TX4_cb->start = 0;
	TX4_cb->end	  = 0;
	TX4_cb->elems = TX4_buff;

	initial_tx = TRUE;
	initial_rx = TRUE;

	// show that we are running
	BufferDescriptor buf_desc;

	while( !_waveform_thread_abort )
	{
		// wait for a buffer descriptor to get posted
		sem_wait(&sched_waveform_sem);

		if(!_waveform_thread_abort)
		{
			do {
				buf_desc = _WaveformList_UnlinkHead();
				// if we got signalled, but there was no new data, something's wrong
				// and we'll just wait for the next packet
				if (buf_desc == NULL)
				{
					//output( "We were signaled that there was another buffer descriptor, but there's not one here");
					break;
				}
				else
				{
					// convert the buffer to little endian
					_dsp_convertBufEndian(buf_desc);

					//output(" \"Processed\" buffer stream id = 0x%08X\n", buf_desc->stream_id);

					if( (buf_desc->stream_id & 1) == 0) { //RX BUFFER
						//	If 'initial_rx' flag, clear buffers RX1, RX2, RX3, RX4
						if(initial_rx)
						{
							RX1_cb->start = 0;	// Clear buffers RX1, RX2, RX3, RX4
							RX1_cb->end	  = 0;
							RX2_cb->start = 0;
							RX2_cb->end	  = 0;
							RX3_cb->start = 0;
							RX3_cb->end	  = 0;
							RX4_cb->start = 0;
							RX4_cb->end	  = 0;


							/* Clear filter memory */
							memset(float_in_24k, 0, MEM_24 * sizeof(float));
							memset(float_in_8k, 0, MEM_8 * sizeof(float));

							/* Requires us to set initial_rx to FALSE which we do at the end of
							 * the first loop
							 */
						}

						//	Set the transmit 'initial' flag
						initial_tx = TRUE;
						write_dat = TRUE;

						// Check for new receiver input packet & move to RX1_cb.
						// TODO - If transmit packet, discard here?


						for( i = 0 ; i < PACKET_SAMPLES ; i++)
						{
							//output("Outputting ")
							//	fsample = Get next float from packet;
							cbWriteFloat(RX1_cb, ((Complex*)buf_desc->buf_ptr)[i].real);

						}


						// Check for >= 384 samples in RX1_cb and spin downsampler
						//	Convert to shorts and move to RX2_cb.
						if(cfbContains(RX1_cb) >= DV_PACKET_SAMPLES * DECIMATION_FACTOR)
						{
						    enum DEMOD_STATE state = DEMOD_UNKNOWN;
							for(i=0 ; i< DV_PACKET_SAMPLES * DECIMATION_FACTOR ; i++)
							{
							    state = gmsk_decode(_gmsk_demod, cbReadFloat(RX1_cb));

							    unsigned char ambe_out[9] = {0};
							    BOOL ambe_packet_out = FALSE;
                                if ( state == DEMOD_TRUE ) {
                                    ambe_packet_out = dstar_stateMachine(_dstar, TRUE, ambe_out, 9);
                                } else if ( state == DEMOD_FALSE ) {
                                    ambe_packet_out = dstar_stateMachine(_dstar, FALSE, ambe_out, 9);
                               } else {
                                   /* Nothing to do since we have not "locked" a bit out yet */
                               }

                               if ( ambe_packet_out == TRUE ) {
                                   nout = 0;
                                   nout = thumbDV_decode(_dv_serial_fd, ambe_out, speech_out, DV_PACKET_SAMPLES);
                                   //if (nout) output(" %d \n", speech_out[i]);
                                   for( i = 0 ; i < nout ; i++)
                                       cbWriteShort(RX3_cb, speech_out[i]);
                               }
							}
						}

						// Check for >= 128 samples in RX3_cb, convert to floats
						//	and spin the upsampler. Move output to RX4_cb.

						if(csbContains(RX3_cb) >= DV_PACKET_SAMPLES)
						{
							for( i=0 ; i< DV_PACKET_SAMPLES ; i++)
							{
								float_in_8k[i+MEM_8] = ((float)  (cbReadShort(RX3_cb) / SCALE_RX_OUT));

							}

							fdmdv_8_to_24(float_out_24k, &float_in_8k[MEM_8], DV_PACKET_SAMPLES);

							for( i=0 ; i< DV_PACKET_SAMPLES * DECIMATION_FACTOR ; i++)
							{
								cbWriteFloat(RX4_cb, float_out_24k[i]);
							}
							//Sig2Noise = (_freedvS->fdmdv_stats.snr_est);
						}

						// Check for >= 128 samples in RX4_cb. Form packet and
						//	export.

						uint32 check_samples = PACKET_SAMPLES;

						if(initial_rx)
							check_samples = PACKET_SAMPLES * 3;


						if(cfbContains(RX4_cb) >= check_samples )
						{
							for( i=0 ; i< PACKET_SAMPLES ; i++)
							{
								//output("Fetching from end buffer \n");
								// Set up the outbound packet
								fsample = cbReadFloat(RX4_cb);
//								// put the fsample into the outbound packet
//
								((Complex*)buf_desc->buf_ptr)[i].real = fsample;
								((Complex*)buf_desc->buf_ptr)[i].imag = fsample;

							}
						} else {
							//output("RX Starved buffer out\n");

							memset( buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof(Complex));

							if(initial_rx)
								initial_rx = FALSE;
						}

					} else if ( (buf_desc->stream_id & 1) == 1) { //TX BUFFER
						//	If 'initial_rx' flag, clear buffers TX1, TX2, TX3, TX4
						if(initial_tx)
						{
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

						for( i = 0 ; i < PACKET_SAMPLES ; i++ )
						{
							//output("Outputting ")
							//	fsample = Get next float from packet;
							cbWriteFloat(TX1_cb, ((Complex*)buf_desc->buf_ptr)[i].real);

						}

//
						// Check for >= 384 samples in TX1_cb and spin downsampler
						//	Convert to shorts and move to TX2_cb.
						if(cfbContains(TX1_cb) >= DV_PACKET_SAMPLES * DECIMATION_FACTOR)
						{
							for(i=0 ; i< DV_PACKET_SAMPLES * DECIMATION_FACTOR ; i++)
							{
								tx_float_in_24k[i + MEM_24] = cbReadFloat(TX1_cb);
							}

							fdmdv_24_to_8(tx_float_out_8k, &tx_float_in_24k[MEM_24], DV_PACKET_SAMPLES);

							for(i=0 ; i < DV_PACKET_SAMPLES ; i++)
							{
								cbWriteShort(TX2_cb, (short) (tx_float_out_8k[i]*SCALE_TX_IN));
							}

						}
//
//						// Check for >= 320 samples in TX2_cb and spin vocoder
						// 	Move output to TX3_cb.


                        if ( csbContains(TX2_cb) >= DV_PACKET_SAMPLES )
                        {
                            for( i=0 ; i< DV_PACKET_SAMPLES ; i++)
                            {
                                speech_in[i] = cbReadShort(TX2_cb);
                            }

                            //output("Speech in = %d", speech_in[0]);

                            /* DECODE */
                            uint32 decode_out = 0;
                            decode_out = thumbDV_encode(_dv_serial_fd, speech_in, (unsigned char * )mod_out, DV_PACKET_SAMPLES);
                        }

                        FILE * dat = NULL;

                        if ( write_dat ) {
                            dat = fopen("gmsk_txNew2.dat", "a");
                        }
                        if ( initial_tx ) {

                            initial_tx = FALSE;
                            float buf[5];
                            uint32 j = 0;
                            /* Create Sync */
                            for ( i = 0 ; i < 64 * 2 ; i += 2 ) {
                                gmsk_encode(_gmsk_mod, TRUE, buf, DSTAR_RADIO_BIT_LENGTH);

                                for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                                    cbWriteFloat(TX4_cb, buf[j]);
                                }

                                gmsk_encode(_gmsk_mod, FALSE, buf, DSTAR_RADIO_BIT_LENGTH);

                                for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                                    cbWriteFloat(TX4_cb, buf[j]);
                                }
                            }

                            for ( i = 0 ; i < FRAME_SYNC_LENGTH_BITS ; i++ ) {
                                gmsk_encode(_gmsk_mod, FRAME_SYNC_BITS[i], buf, DSTAR_RADIO_BIT_LENGTH);

                                for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                                    cbWriteFloat(TX4_cb, buf[j]);
                                }
                            }

//                            fclose(dat);

//                            dstar_header tmp_h;
//                            tmp_h.flag1 = 0;
//                            tmp_h.flag2 = 0;
//                            tmp_h.flag3 = 0;
//
//                            strncpy((char*)tmp_h.destination_rptr, "DIRECT", 9);
//                            strncpy((char*)tmp_h.departure_rptr, "DIRECT", 9);
//                            strncpy((char*)tmp_h.companion_call, "CQCQCQ", 9);
//                            strncpy((char*)tmp_h.own_call1, "K5SDR", 9);
//                            strncpy((char*)tmp_h.own_call1, "K5SDR", 9);
//
//                            dstar_pfcs pfcs;
//                            pfcs.crc16 = 0xFFFF;
//                            dstar_pfcsUpdateBuffer(&pfcs, (unsigned char * ) &tmp_h, 312);
//                            dstar_pfcsResult(&pfcs, (unsigned char * )&(tmp_h.p_fcs));
//
//                            BOOL bits[FEC_SECTION_LENGTH_BITS] = {0};
//
//                            gmsk_bytesToBits((unsigned char *) &tmp_h, bits, 328);
//                            BOOL encoded[330*2] = {0};
//                            BOOL interleaved[330*2] = {0};
//                            BOOL scrambled[330*2] = {0};
//                            uint32 outLen = 0;
//                            dstar_FECencode(bits, encoded, 330, &outLen);
//                            //output("Encode outLen = %d\n", outLen);
//
//                            outLen = 660;
//                            dstar_interleave(encoded, interleaved, outLen);
//
//                            uint32 count = 0;
//                            dstar_scramble(interleaved, scrambled, outLen, &count);
                            //output("Count = %d\n", count);
//                            for ( i = 0 ; i < count ; i++ ) {
//                                gmsk_encode(_gmsk_mod, scrambled[i], buf, DSTAR_RADIO_BIT_LENGTH);
//                                for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
//                                    cbWriteFloat(TX4_cb, buf[j]);
//                                    //fprintf(dat, "%d %.12f\n", data_i++, buf[j]);
//                                }
//                            }

                            //fclose(dat);

                        }

						uint32 tx_check_samples = PACKET_SAMPLES;

						if ( !initial_tx &&  cfbContains(TX4_cb) < PACKET_SAMPLES * 5)  {
                            for ( i = 0 ; i < 13 ; i++ ) {
                                float buf[5];
                                uint32 j = 0;
                                gmsk_encode(_gmsk_mod, TRUE, buf, DSTAR_RADIO_BIT_LENGTH);

                                 for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                                     cbWriteFloat(TX4_cb, buf[j]);
//                                     if ( write_dat )  fprintf(dat, "%d %.12f\n", data_i++, buf[j]);
                                 }

                                 gmsk_encode(_gmsk_mod, FALSE, buf, DSTAR_RADIO_BIT_LENGTH);

                                 for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                                     cbWriteFloat(TX4_cb, buf[j]);
//                                     if ( write_dat ) fprintf(dat, "%d %.12f\n", data_i++, buf[j]);
                                 }
                            }

//                            if ( write_dat )  {
//                                fclose(dat);
//
//                                sync();
//                                write_dat = FALSE;
//                            }

						}


						if(cfbContains(TX4_cb) >= tx_check_samples )
						{
							for( i = 0 ; i < PACKET_SAMPLES ; i++)
							{
							//output("Fetching from end buffer \n");
								// Set up the outbound packet
								fsample = cbReadFloat(TX4_cb);

								// put the fsample into the outbound packet
								((Complex*)buf_desc->buf_ptr)[i].real = fsample;
								((Complex*)buf_desc->buf_ptr)[i].imag = fsample;
							}
						} else {
							//output("TX Starved buffer out\n");

							memset( buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof(Complex));


						}

						initial_tx = FALSE;

					}


					emit_waveform_output(buf_desc);

					hal_BufferRelease(&buf_desc);
				}
			} while(1); // Seems infinite loop but will exit once there are no longer any buffers linked in _Waveformlist
		}
	}
	_waveform_thread_abort = TRUE;

	gmsk_destroyDemodulator(_gmsk_demod);
	gmsk_destroyModulator(_gmsk_mod);

	return NULL;
}

void sched_waveform_Init(void)
{
    //dstar_FECTest();
    //exit(0);

    _dstar = dstar_createMachine();

    _gmsk_demod = gmsk_createDemodulator();
    _gmsk_mod = gmsk_createModulator();
    _gmsk_mod->m_invert = TRUE;

	pthread_rwlock_init(&_list_lock, NULL);

	pthread_rwlock_wrlock(&_list_lock);
	_root = (BufferDescriptor)safe_malloc(sizeof(buffer_descriptor));
	memset(_root, 0, sizeof(buffer_descriptor));
	_root->next = _root;
	_root->prev = _root;
	pthread_rwlock_unlock(&_list_lock);

	sem_init(&sched_waveform_sem, 0, 0);

	pthread_create(&_waveform_thread, NULL, &_sched_waveform_thread, NULL);

	struct sched_param fifo_param;
	fifo_param.sched_priority = 30;
	pthread_setschedparam(_waveform_thread, SCHED_FIFO, &fifo_param);

//	gmsk_testBitsAndEncodeDecode();
//	exit(0);

}

void sched_waveformThreadExit()
{
	_waveform_thread_abort = TRUE;
	sem_post(&sched_waveform_sem);
}
