// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * vita-io.h - Support for VITA-49 data socket of FlexRadio 6000 units.
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
 * You should have received a copy of the GNU General Public License
 * along with smartsdr-codec2.  If not, see <https://www.gnu.org/licenses/>.
 *
 */#ifndef VITA_OUTPUT_H_
#define VITA_OUTPUT_H_

#include "freedv-processor.h"

void vita_output_Init(const char * ip );
void hal_Listener_Init(void);
void vita_send_audio_packet(uint32_t *samples, size_t len, unsigned int tx);
unsigned short vita_init(freedv_proc_t params);
void vita_stop();
void vita_send_meter_packet(void *meters, size_t len);


#define HAL_RX_BUFFER_SIZE	128
#define HAL_TX_BUFFER_SIZE	HAL_RX_BUFFER_SIZE
#define HAL_SAMPLE_SIZE	sizeof(Complex);

enum STREAM_DIRECTION {
	INPUT 	= 1,
	OUTPUT  = 2
};
typedef enum STREAM_DIRECTION StreamDirection;

enum STREAM_TYPEX {
	FFT = 1,
	MMX = 2,
	IQD = 3,
	AUD = 4,
	MET = 5,
	DSC = 6,
	TXD = 7,
	PAN = 8,
	WFL = 9,
	WFM = 10,
	XXX = 99
};

typedef enum STREAM_TYPEX ShortStreamType;

#define HAL_STATUS_PROCESSED 1
#define HAL_STATUS_INVALID_OUI 2
#define HAL_STATUS_NO_DESC 3
#define HAL_STATUS_UNSUPPORTED_SAMP 4
#define HAL_STATUS_UNSUPPORTED_FFT 5
#define HAL_STATUS_BAD_TYPE 6
#define HAL_STATUS_FFT_NO_STREAM 7
#define HAL_STATUS_IQ_NO_STREAM 8
#define HAL_STATUS_OUTPUT_OK 9
#define HAL_STATUS_DSP_NO_STREAM 10
#define HAL_STATUS_DAX_NO_STREAM 11
#define HAL_STATUS_DAX_SIZE_WRONG 12
#define HAL_STATUS_DAX_WRONG_CHAN 13
#define HAL_STATUS_TX_SKIP 14
#define HAL_STATUS_TX_ZERO 15
#define HAL_STATUS_UNK_STREAM 16


/* Waveform defines */
#define HAL_STATUS_WFM_SIZE_WRONG 17
#define HAL_STATUS_WFM_NO_STREAM 18

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define VITA_OUTPUT_PORT		0x8113
#define VITA_INPUT_PORT         0x7F13
#else
#define VITA_OUTPUT_PORT		0x1381
#define VITA_INPUT_PORT         0x137F
#endif


#endif /* VITA_OUTPUT_H_ */
