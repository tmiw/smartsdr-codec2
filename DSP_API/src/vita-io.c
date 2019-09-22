///*!	\file vita-io.c
// *	\brief transmit vita packets to the Ethernet
// *
// *	\copyright	Copyright 2012-2013 FlexRadio Systems.  All Rights Reserved.
// *				Unauthorized use, duplication or distribution of this software is
// *				strictly prohibited by law.
// *
// *	\date 2-APR-2012
// *	\author Stephen Hicks, N5AC
// *
// */

/* *****************************************************************************
 *
 *  Copyright (C) 2012-2014 FlexRadio Systems.
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

#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h> // for write, usleep
#include <errno.h>
#include <assert.h>
#include <poll.h>

#include "vita-io.h"
#include "common.h"
#include "hal_vita.h"
#include "sched_waveform.h"

#define FORMAT_DBFS 0
#define FORMAT_DBM 1

#define VITA_CLASS_ID_1			(uint32)VITA_OUI
#define VITA_CLASS_ID_2			SL_VITA_INFO_CLASS << 16 | SL_VITA_IF_DATA_CLASS

#define MAX_SAMPLES_PER_PACKET	(MAX_IF_DATA_PAYLOAD_SIZE/8)
#define MAX_BINS_PER_PACKET 700

#define MAX_COUNTED_STREAMS 100
#define COUNTER_INTERVAL_MS 1000

static vita_if_data waveform_packet;

static int vita_sock;
static BOOL hal_listen_abort;
static pthread_t _hal_listen_thread;

void vita_output_Init(const char *ip)
{
	assert(ip != NULL);

	struct sockaddr_in bind_addr =  {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(VITA_49_SOURCE_PORT),
	};
	struct sockaddr_in radio_addr = {
		.sin_family = AF_INET,
		.sin_addr = {0},
		.sin_port = htons(4993),
	};

	output("\033[32mInitializing VITA-49 output engine...\n\033[m");

	vita_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (vita_sock == -1) {
		output(ANSI_RED " Failed to initialize VITA socket: %s\n", strerror(errno));
		return;
	}

	if(bind(vita_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr))) {
		output(ANSI_RED "error binding socket: %s\n",strerror(errno));
		return;
	}

	if(!inet_aton(ip, &radio_addr.sin_addr)) {
		output(ANSI_RED "Could not convert local addr to binary\n");
		return;
	}

	if(connect(vita_sock, (struct sockaddr *) &radio_addr, sizeof(radio_addr)) == -1) {
		output(ANSI_RED "Couldn't connect socket: %s\n", strerror(errno));
		return;
	}

	output("Vita Output Init - ip = '%s' port = %d\n", ip,radio_addr.sin_port);
}

static void _vita_formatWaveformPacket(Complex* buffer, uint32 samples, uint32 stream_id, uint32 packet_count,
		uint32 class_id_h, uint32 class_id_l)
{
	waveform_packet.header = htonl(
			VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID |
			VITA_HEADER_CLASS_ID_PRESENT |
			VITA_TSI_OTHER |
			VITA_TSF_SAMPLE_COUNT |
			(packet_count << 16) |
			(7+samples*2));
	waveform_packet.stream_id = htonl(stream_id);
	waveform_packet.class_id_h =  htonl(class_id_h);
	waveform_packet.class_id_l =  htonl(class_id_l);
	waveform_packet.timestamp_int = 0;
	waveform_packet.timestamp_frac_h = 0;
	waveform_packet.timestamp_frac_l = 0;

	memcpy(waveform_packet.payload, buffer, samples * sizeof(Complex));
}

static uint32 _waveform_packet_count = 0;
void emit_waveform_output(BufferDescriptor buf_desc_out)
{
	int samples_sent, samples_to_send;
	Complex *buf_pointer;
	ssize_t bytes_sent;
	int i;

	// XXX Assertions?
	if (buf_desc_out == NULL) {
		output(ANSI_RED "buf_desc_out is NULL\n");
		return;
	}
	if (buf_desc_out->buf_ptr == NULL) {
		output(ANSI_RED "buf_desc_out->buf_ptr is NULL\n");
		return;
	}

	Complex* out_buffer = (Complex*) buf_desc_out->buf_ptr;
	uint32 buf_size = buf_desc_out->num_samples;

	// convert to big endian for network
	for(i=0; i<buf_size; i++) {
		*(uint32*)&out_buffer[i].real = htonl(*(uint32*)&out_buffer[i].real);
		*(uint32*)&out_buffer[i].imag = htonl(*(uint32*)&out_buffer[i].imag);
	}

	samples_sent = 0;
	buf_pointer = out_buffer;
	uint32 preferred_samples_per_packet = buf_size;
	//output("samples_to_send: %d\n", preferred_samples_per_packet);

	while (samples_sent < buf_size) {
		if ((buf_size - samples_sent) > preferred_samples_per_packet)
			samples_to_send = preferred_samples_per_packet;
		else
			samples_to_send =  buf_size - samples_sent;

		_vita_formatWaveformPacket(
				buf_pointer,
				samples_to_send,
				buf_desc_out->stream_id,
				_waveform_packet_count++ & 0xF,
				(uint32) FLEXRADIO_OUI,
				SL_VITA_SLICE_AUDIO_CLASS
		);

		if((bytes_sent = send(vita_sock, &waveform_packet, samples_to_send * 8 + 28, 0)) < 0) {
			output(ANSI_RED "Error sending packet: %s \n", strerror(errno));
			continue;
		}

		if(bytes_sent != samples_to_send * 8 + 28) {
			output(ANSI_RED "Short write in emit_waveform_output");
			continue;
		}

		buf_pointer += samples_to_send;
		samples_sent += samples_to_send;

	}
}

static void _hal_ListenerProcessWaveformPacket(struct vita_packet *packet, ssize_t length)
{
	BufferDescriptor buf_desc;
	buf_desc = hal_BufferRequest(HAL_RX_BUFFER_SIZE, sizeof(Complex));

	if(!buf_desc->timestamp_int) {
		buf_desc->timestamp_int = htonl(packet->timestamp_int);
		buf_desc->timestamp_frac_h = htonl(packet->timestamp_frac >> 32);
		buf_desc->timestamp_frac_l = htonl(packet->timestamp_frac);
	}

// 	calculate number of samples in the buffer
	short payload_length = ((htons(packet->length) * sizeof(uint32)) - VITA_PACKET_HEADER_SIZE);
	if(payload_length != length - VITA_PACKET_HEADER_SIZE) {
		output("VITA header size doesn't match bytes read from network\n");
		hal_BufferRelease(&buf_desc);
		return;
	}

	memcpy(buf_desc->buf_ptr, packet->payload.raw_payload, payload_length);
	buf_desc->stream_id = htonl(packet->stream_id);
	sched_waveform_Schedule(buf_desc);
}

static void _hal_ListenerParsePacket(void *data, ssize_t length)
{
	// make sure packet is long enough to inspect for VITA header info
	if(length < VITA_PACKET_HEADER_SIZE)
		return;

	struct vita_packet *packet = (struct vita_packet *) data;

	if(packet->class_id & VITA_OUI_MASK != FLEX_OUI)
		return;

	switch(packet->stream_id & STREAM_BITS_MASK) {
		case STREAM_BITS_WAVEFORM | STREAM_BITS_IN:
			_hal_ListenerProcessWaveformPacket(packet, length);
			break;
		default:
			output("Undefined stream in %08X", htonl(packet->stream_id));
			break;
	}
}

static void* _hal_ListenerLoop(void* param)
{
	// XXX Size this correctly?
	uint8 buf[ETH_FRAME_LEN];
	fd_set vita_sockets;
	int ret;
	ssize_t bytes_received = 0;

	struct pollfd fds = {
		.fd = vita_sock,
		.events = POLLIN,
		.revents = 0,
	};

	output("Beginning VITA Listener Loop...\n");

	while(!hal_listen_abort) {
		ret = poll(&fds, 1, 500);
		if(ret == 0) {
			// timeout
			continue;
		} else if(ret == -1) {
			// error
			output(ANSI_RED "Poll failed: %s\n", strerror(errno));
			continue;
		}
		if((bytes_received = recv(vita_sock, buf, sizeof(buf), 0)) == -1) {
			output(ANSI_RED "Read failed: %s\n", strerror(errno));
			continue;
		}
		_hal_ListenerParsePacket(buf, bytes_received);
	}

	output("Ending VITA Listener Loop...\n");
	return NULL;
}


void hal_Listener_Init(void)
{
	output("Vita Listener Init: Opening socket");

	_hal_listen_thread = (pthread_t) NULL;
	pthread_create(&_hal_listen_thread, NULL, &_hal_ListenerLoop, NULL);
}
