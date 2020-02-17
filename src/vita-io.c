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
#include <pthread.h>

#include "event2/event.h"
#include "event2/buffer.h"

#include "utils.h"
#include "vita.h"
#include "freedv-processor.h"
#include "api-io.h"

struct vita {
    struct event *evt;
    short port;
    uint32_t rx_stream_id;
    uint32_t tx_stream_id;
    uint8_t meter_sequence;
    uint32_t audio_sequence;
    struct fdv *fdv;
};

static void vita_process_waveform_packet(struct vita *vita, struct vita_packet *packet, ssize_t length)
{
	unsigned long payload_length = ((htons(packet->length) * sizeof(uint32_t)) - VITA_PACKET_HEADER_SIZE);
	unsigned int num_samples = payload_length / sizeof(uint32_t);
    uint32_t *samples = packet->if_samples;

	if(payload_length != length - VITA_PACKET_HEADER_SIZE) {
		output("VITA header size doesn't match bytes read from network (%d != %d - %d) -- %d\n", payload_length, length, VITA_PACKET_HEADER_SIZE, sizeof(struct vita_packet));
		return;
	}

    for (unsigned int i = 0; i < num_samples / 2; ++i)
        samples[i] = ntohl(samples[i * 2]);

	if (!(htonl(packet->stream_id) & 0x0001u)) {
	    //  Receive Packet Processing
	    vita->rx_stream_id = packet->stream_id;
        evbuffer_add(fdv_get_rx(vita->fdv), samples, (num_samples / 2) * sizeof(uint32_t));
	} else {
	    //  Transmit packet processing
        vita->tx_stream_id = packet->stream_id;
        output("Queue samples for TX\n");
        evbuffer_add(fdv_get_tx(vita->fdv), samples, (num_samples / 2) * sizeof(uint32_t));
	}
}

static void vita_parse_packet(struct vita *vita, struct vita_packet *packet, size_t packet_len)
{
	// make sure packet is long enough to inspect for VITA header info
	if (packet_len < VITA_PACKET_HEADER_SIZE)
        return;

	if((packet->class_id & VITA_OUI_MASK) != FLEX_OUI)
		return;

	switch(packet->stream_id & STREAM_BITS_MASK) {
		case STREAM_BITS_WAVEFORM | STREAM_BITS_IN:
            vita_process_waveform_packet(vita, packet, packet_len);
			break;
		default:
			output("Undefined stream in %08X", htonl(packet->stream_id));
			break;
	}
}

static void vita_read_cb(evutil_socket_t socket, short what, void *ctx)
{
    struct vita *vita = (struct vita *) ctx;
    ssize_t bytes_received = 0;
    struct vita_packet packet;



    if (!(what & EV_READ)) {
        output("Callback is not for a read?!\n");
        return;
    }

    if ((bytes_received = recv(socket, &packet, sizeof(packet), 0)) == -1) {
        output("VITA read failed: %s\n", strerror(errno));
        return;
    }

    vita_parse_packet(vita, &packet, bytes_received);
}

struct vita *vita_new(struct event_base *base, struct sockaddr_in *radio_addr)
{
    int vita_sock;
    struct vita *vita;

    vita = (struct vita *) malloc(sizeof(struct vita));
    if (!vita) {
        output("Couldn't allocate space for VITA info structure");
        return NULL;
    }

	struct sockaddr_in bind_addr =  {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = 0,
	};
	socklen_t bind_addr_len = sizeof(bind_addr);

    radio_addr->sin_port = htons(4993);

	output("Initializing VITA-49 engine...\n");

	vita_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (vita_sock == -1) {
		output(" Failed to initialize VITA socket: %s\n", strerror(errno));
		goto fail;
	}

	if(bind(vita_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr))) {
		output("error binding socket: %s\n",strerror(errno));
		goto fail_socket;
	}

	if(connect(vita_sock, (struct sockaddr *) radio_addr, sizeof(struct sockaddr_in)) == -1) {
		output("Couldn't connect socket: %s\n", strerror(errno));
		goto fail_socket;
	}

	if (getsockname(vita_sock, (struct sockaddr *) &bind_addr, &bind_addr_len) == -1) {
		output("Couldn't get port number of VITA socket\n");
        goto fail_socket;
	}

	vita->evt = event_new(base, vita_sock, EV_READ|EV_PERSIST,  vita_read_cb, vita);
	event_add(vita->evt, NULL);

	vita->port = ntohs(bind_addr.sin_port);

	vita->fdv = fdv_new(vita, FREEDV_MODE_1600);

	vita->audio_sequence = 0;
	vita->meter_sequence = 0;

    return vita;

    fail_socket:
    close(vita_sock);
    fail:
    free(vita);
    return NULL;
}

short vita_get_port(struct vita *vita)
{
    return vita->port;
}

void vita_free(struct vita *vita)
{
    if (!vita)
        return;

    event_del(vita->evt);
    close(event_get_fd(vita->evt));
    event_free(vita->evt);
    fdv_free(vita->fdv);

    free(vita);
}

static void vita_send_packet(struct vita *vita, struct vita_packet *packet,  size_t payload_len)
{
    ssize_t bytes_sent;
    size_t packet_len = VITA_PACKET_HEADER_SIZE + payload_len;

    //  XXX Lots of magic numbers here!
    packet->timestamp_type = 0x50u | (packet->timestamp_type & 0x0Fu);
    assert(packet_len % 4 == 0);
    packet->length = htons(packet_len / 4); // Length is in 32-bit words

    packet->timestamp_int = time(NULL);
    packet->timestamp_frac = 0;

    if ((bytes_sent = send(event_get_fd(vita->evt), packet, packet_len, 0)) == -1) {
        output("Error sending vita packet: %s\n", strerror(errno));
        return;
    }

    if (bytes_sent != packet_len) {
        output("Short write on vita send\n");
        return;
    }
}

void vita_send_meter_packet(struct vita *vita, void *meters, size_t len)
{
    struct vita_packet packet = {0};

    packet.packet_type = VITA_PACKET_TYPE_EXT_DATA_WITH_STREAM_ID;
    packet.stream_id = METER_STREAM_ID;
    packet.class_id = METER_CLASS_ID;
    packet.timestamp_type = vita->meter_sequence++;

    assert(len < sizeof(packet.raw_payload));
    memcpy(packet.raw_payload, meters, len);

    vita_send_packet(vita, &packet, len);
}
//
void vita_send_audio_packet(struct vita *vita, uint32_t *samples, size_t len, unsigned int tx)
{
    struct vita_packet packet = {0};

    assert(len * 2 <= sizeof(packet.if_samples));

    packet.packet_type = VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID;
    packet.stream_id = tx ? vita->tx_stream_id : vita->rx_stream_id;
    packet.class_id = AUDIO_CLASS_ID;
    packet.timestamp_type = vita->audio_sequence++;

    for (unsigned int i = 0, j = 0; i < len / sizeof(uint32_t); ++i, j += 2)
        packet.if_samples[j] = packet.if_samples[j + 1] = htonl(samples[i]);

    vita_send_packet(vita, &packet, len * 2);
}