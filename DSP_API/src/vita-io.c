// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * vita-io.c - Support for VITA-49 data socket of FlexRadio 6000 units.
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

#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <time.h>

#include "vita-io.h"
#include "common.h"
#include "sched_waveform.h"
#include "api-io.h"

static int vita_sock;
static bool vita_processing_thread_abort;
static pthread_t vita_processing_thread;
static uint8_t meter_sequence = 0;

static freedv_proc_t freedv_params;

static uint32_t rx_stream_id, tx_stream_id;

static void vita_process_waveform_packet(struct vita_packet *packet, ssize_t length)
{
	unsigned long payload_length = ((htons(packet->length) * sizeof(uint32_t)) - VITA_PACKET_HEADER_SIZE);
	if(payload_length != length - VITA_PACKET_HEADER_SIZE) {
		output("VITA header size doesn't match bytes read from network (%d != %d - %d) -- %d\n", payload_length, length, VITA_PACKET_HEADER_SIZE, sizeof(struct vita_packet));
		return;
	}

	if (!(htonl(packet->stream_id) & 0x0001u)) {
	    rx_stream_id = packet->stream_id;
	    //  This is a receive packet
	    freedv_queue_rx_samples(freedv_params, packet->if_samples, payload_length);
	} else {
        tx_stream_id = packet->stream_id;
	}
}

static void vita_parse_packet(struct vita_packet *packet, size_t packet_len)
{
	// make sure packet is long enough to inspect for VITA header info
	if (packet_len < VITA_PACKET_HEADER_SIZE)
        return;

	if((packet->class_id & VITA_OUI_MASK) != FLEX_OUI)
		return;

	switch(packet->stream_id & STREAM_BITS_MASK) {
		case STREAM_BITS_WAVEFORM | STREAM_BITS_IN:
            vita_process_waveform_packet(packet, packet_len);
			break;
		default:
			output("Undefined stream in %08X", htonl(packet->stream_id));
			break;
	}
}

static void* vita_processing_loop()
{
	struct vita_packet packet;
	int ret;
	ssize_t bytes_received = 0;

	struct pollfd fds = {
		.fd = vita_sock,
		.events = POLLIN,
		.revents = 0,
	};

	output("Beginning VITA Listener Loop...\n");
    vita_processing_thread_abort = false;

	while(!vita_processing_thread_abort) {
		ret = poll(&fds, 1, 500);

		if (ret == 0) {
			// timeout
			continue;
		} else if (ret == -1) {
			// error
			output("VITA poll failed: %s\n", strerror(errno));
			continue;
		}

		if ((bytes_received = recv(vita_sock, &packet, sizeof(packet), 0)) == -1) {
			output("VITA read failed: %s\n", strerror(errno));
			continue;
		}

        vita_parse_packet(&packet, bytes_received);
	}

	output("Ending VITA Listener Loop...\n");
	return NULL;
}

unsigned short vita_init(freedv_proc_t params)
{
    freedv_params = params;
	struct sockaddr_in bind_addr =  {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = 0,
	};
	socklen_t bind_addr_len = sizeof(bind_addr);

	struct sockaddr_in radio_addr;
	if (get_radio_addr(&radio_addr) == -1) {
		output("Failed to get radio address: %s\n", strerror(errno));
		return 0;
	}
	radio_addr.sin_port = htons(4993);

	output("Initializing VITA-49 engine...\n");

	vita_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (vita_sock == -1) {
		output(ANSI_RED " Failed to initialize VITA socket: %s\n", strerror(errno));
		return 0;
	}

	if(bind(vita_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr))) {
		output(ANSI_RED "error binding socket: %s\n",strerror(errno));
		close(vita_sock);
		return 0;
	}

	if(connect(vita_sock, (struct sockaddr *) &radio_addr, sizeof(struct sockaddr_in)) == -1) {
		output(ANSI_RED "Couldn't connect socket: %s\n", strerror(errno));
		close(vita_sock);
		return 0;
	}

	if (getsockname(vita_sock, (struct sockaddr *) &bind_addr, &bind_addr_len) == -1) {
		output("Couldn't get port number of VITA socket\n");
		close(vita_sock);
		return 0;
	}

    vita_processing_thread = (pthread_t) NULL;
	pthread_create(&vita_processing_thread, NULL, &vita_processing_loop, NULL);

	return ntohs(bind_addr.sin_port);
}

void vita_stop()
{
    vita_processing_thread_abort = true;
	pthread_join(vita_processing_thread, NULL);
	close(vita_sock);
}

static void vita_send_packet(struct vita_packet *packet,  size_t payload_len)
{
    ssize_t bytes_sent;
    size_t packet_len = VITA_PACKET_HEADER_SIZE + payload_len;

    //  XXX Lots of magic numbers here!
    packet->timestamp_type = 0x50u | (packet->timestamp_type & 0x0Fu);
    assert(packet_len % 4 == 0);
    packet->length = htons(packet_len / 4); // Length is in 32-bit words

    packet->timestamp_int = time(NULL);
    packet->timestamp_frac = 0;

    if ((bytes_sent = send(vita_sock, packet, packet_len, 0)) == -1) {
        output("Error sending vita packet: %s\n", strerror(errno));
        return;
    }

    if (bytes_sent != packet_len) {
        output("Short write on vita send\n");
        return;
    }
}

void vita_send_meter_packet(void *meters, size_t len)
{
    struct vita_packet packet = {0};

    packet.packet_type = VITA_PACKET_TYPE_EXT_DATA_WITH_STREAM_ID;
    packet.stream_id = METER_STREAM_ID;
    packet.class_id = METER_CLASS_ID;
    packet.timestamp_type = meter_sequence++;

    assert(len < sizeof(packet.raw_payload));
    memcpy(packet.raw_payload, meters, len);

    vita_send_packet(&packet, len);
}

static uint32_t audio_sequence = 0;
void vita_send_audio_packet(uint32_t *samples, size_t len, unsigned int tx)
{
    struct vita_packet packet = {0};
    unsigned int i, j;

    assert(len <= sizeof(packet.if_samples));

    packet.packet_type = VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID;
    packet.stream_id = tx ? tx_stream_id : rx_stream_id;
    packet.class_id = AUDIO_CLASS_ID;
    packet.timestamp_type = audio_sequence++;

    memcpy(packet.if_samples, samples, len);
    vita_send_packet(&packet, len);
}