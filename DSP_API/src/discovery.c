// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * discovery.c - Support for network discovery of FlexRadio 6000 units.
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "discovery.h"
#include "utils.h"
#include "vita.h"

#define DISCOVERY_PORT		4992
#define MAX_DISCOVERY_ARGC	 128

static const int one = 1;

int parse_discovery_packet(struct vita_packet *packet, struct sockaddr_in *addr)
{
	char *argv[MAX_DISCOVERY_ARGC];
	struct kwarg *kwargs = NULL;
	char *value;
	int argc;

	if(packet->class_id != DISCOVERY_CLASS_ID) {
		output("Received packet with invalid ID: 0x%llX\n", packet->class_id);
		return -1;
	}

	if (packet->packet_type != VITA_PACKET_TYPE_EXT_DATA_WITH_STREAM_ID) {
		output("Received packet is not correct type: 0x%x\n", packet->packet_type);
		return -1;
	}

	if (packet->stream_id != DISCOVERY_STREAM_ID) {
		output("Received packet does not have correct stream id: 0x%x\n", packet->stream_id);
		return -1;
	}

	argc = parse_argv((char *) packet->raw_payload, argv, MAX_DISCOVERY_ARGC);
	if ((kwargs = parse_kwargs(argv, argc, 0)) == NULL)
	    return -1;

	if ((value = find_kwarg(kwargs, "ip"))) {
	    if (inet_aton(value, &addr->sin_addr) == 0) {
	        output("Received packet  has invalid iP: %s\n", value);
	        goto fail;
	    }
	} else {
	    goto fail;
	}

	if ((value = find_kwarg(kwargs, "port"))) {
	    char *end;
	    unsigned long port;

	    errno = 0;
        port = strtoul(value, &end, 10);
	    if (*end)
	        goto fail;
	    if (errno)
	        goto fail;

	    if (port > USHRT_MAX)
	        goto fail;

        addr->sin_port = htons(port);
	} else {
	   goto fail;
	}

	return 0;

    fail:
    free(kwargs);
    return -1;
}
int discover_radio(struct sockaddr_in *addr)
{
	int discovery_sock;
	struct vita_packet packet;
	struct sockaddr_in remote_address;
	socklen_t remote_address_size = sizeof(remote_address);

	struct sockaddr_in local_address = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(DISCOVERY_PORT),
	};

	output ("Discovering Radios\n");

	if ((discovery_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		output("Cannot open discovery socket: %s\n", strerror(errno));
		return -1;
	}

	if(setsockopt(discovery_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1) {
		output("Cannot set discovery socket for reuse: %s\n", strerror(errno));
		close(discovery_sock);
		return -1;
	}

	if(bind(discovery_sock, (struct sockaddr *) &local_address, sizeof(local_address)) == -1) {
		output("Cannot bind to socket on port %d: %s\n", DISCOVERY_PORT, strerror(errno));
		close(discovery_sock);
		return -1;
	}

	struct pollfd fds = {
		.fd = discovery_sock,
		.events = POLLIN,
		.revents = 0,
	};

	for (;;) {
		int ret = poll(&fds, 1, 5000);
		int bytes_received;
		if(ret == 0) {
			output("Timed out trying to find radio\n");
			// timeout
			continue;
		} else if(ret == -1) {
			// error
			output("Poll failed: %s\n", strerror(errno));
			return -1;
		}
		if((bytes_received = recvfrom(discovery_sock, &packet, sizeof(packet), 0, (struct sockaddr *) &remote_address, &remote_address_size)) == -1) {
			output("Read failed: %s\n", strerror(errno));
			continue;
		}
		output("Received discovery packet\n");
		if (parse_discovery_packet(&packet, addr) == 0) {
			output("Received valid discovery packet\n");
			break;
		}
	}

	close(discovery_sock);

	return 0;
}