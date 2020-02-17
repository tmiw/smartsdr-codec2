// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * api-io.c - Support for command and control API socket of FlexRadio 6000
 *            units.
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
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>

#include "event2/event.h"
#include "event2/bufferevent.h"
#include "event2/buffer.h"

#include "api-io.h"
#include "api.h"
#include "utils.h"
#include "ringbuf.h"

#define MAX_API_COMMAND_SIZE 1024

struct response_queue_entry {
    unsigned int sequence;
    response_cb cb;
    void *ctx;
	struct response_queue_entry *next;
};

struct api {
    unsigned int sequence;
    unsigned int handle;
    int api_version_major[2];
    int api_version_minor[2];
    struct response_queue_entry *rq_head;
    struct bufferevent *bev;
};

static void add_sequence_to_response_queue(struct api *api, response_cb cb, void *ctx)
{
	struct response_queue_entry *new_entry, *current_entry;

	new_entry = (struct response_queue_entry *) malloc(sizeof(struct response_queue_entry));

	new_entry->cb = cb;
	new_entry->sequence = api->sequence;
	new_entry->ctx = ctx;
	new_entry->next = NULL;

	if (api->rq_head == NULL) {
		api->rq_head = new_entry;
	} else {
		for (current_entry = api->rq_head; current_entry->next != NULL; current_entry = current_entry->next);
		current_entry->next = new_entry;
	}
}

static void complete_response_entry(struct api *api, unsigned int sequence, unsigned int code, char *message)
{
	struct response_queue_entry *current_entry;
	struct response_queue_entry *head = api->rq_head;

	if (head == NULL)
        return;

	for (current_entry = head; current_entry != NULL; current_entry = current_entry->next) {
		if (current_entry->sequence == sequence) {
            current_entry->cb(api, current_entry->ctx, code, message);
			break;
		}
	}
}

static void destroy_response_queue(struct api *api)
{
    struct response_queue_entry *current_entry, *next_entry;

    if (api->rq_head == NULL)
        return;

    for (current_entry = api->rq_head, next_entry = api->rq_head->next; next_entry != NULL; current_entry = next_entry, next_entry = current_entry->next)
        free(current_entry);

    free(current_entry);
    api->rq_head = NULL;
}

static void process_api_line(struct api *api, char *line)
{
	char *message, *endptr, *response_message;
	int ret;
	unsigned int handle, code, sequence;
	unsigned int api_version[4];

	output("Received: %s\n", line);

	switch(*(line++)) {
	case 'V':
	    // TODO: Fix me so that I read into the api struct.
		errno = 0;
		ret = sscanf(line, "%d.%d.%d.%d", &api_version[0], &api_version[1], &api_version[2], &api_version[3]);
		if (ret != 4)
			output("Error converting version string: %s\n", line);

		output("Radio API Version: %d.%d(%d.%d)\n", api_version[0], api_version[1], api_version[2], api_version[3]);

		break;
	case 'H':
		errno = 0;
            api->handle = strtoul(line, &endptr, 16);
        if ((errno == ERANGE && api->handle == ULONG_MAX) ||
            (errno != 0 && api->handle == 0)) {
            output("Error finding session handle: %s\n", strerror(errno));
            break;
        }

        if (endptr == line) {
            output("Cannot find session handle in: %s\n", line);
            break;
        }

        break;
	case 'S':
		errno = 0;
		handle = strtoul(line, &endptr, 16);
        if ((errno == ERANGE && handle == ULONG_MAX) ||
            (errno != 0 && handle == 0)) {
            output("Error finding status handle: %s\n", strerror(errno));
            break;
        }

        if (endptr == line) {
            break;
        }

        process_status_message(api, endptr + 1);
		break;
	case 'M':
		break;
	case 'R':
		errno = 0;
		sequence = strtoul(line, &endptr, 10);
        if ((errno == ERANGE && sequence == ULONG_MAX) ||
            (errno != 0 && sequence == 0)) {
            output("Error finding response sequence: %s\n", strerror(errno));
            break;
        }

        if (endptr == line) {
            output("Cannot find response sequence in: %s\n", line);
            break;
        }

        errno = 0;
        code = strtoul(endptr + 1, &response_message, 16);
        if ((errno == ERANGE && code == ULONG_MAX) ||
            (errno != 0 && code == 0)) {
            output("Error finding response code: %s\n", strerror(errno));
            break;
        }

        if (response_message == endptr + 1) {
            output("Cannot find response code in: %s\n", line);
            break;
        }

        complete_response_entry(api, sequence, code, response_message + 1);
		break;
	case 'C':
//		errno = 0;
//        sequence = strtoul(line, &endptr, 10);
//        if ((errno == ERANGE && sequence == ULONG_MAX) ||
//            (errno != 0 && sequence == 0)) {
//            output("Error finding command sequence: %s\n", strerror(errno));
//            break;
//        }
//
//        if (line == endptr) {
//            output("Cannot find command sequence in: %s\n", line);
//            break;
//        }
//
//        process_waveform_command(sequence, endptr + 1);
		break;
	default:
		output("Unknown command: %s\n", line);
		break;
	}
}

void api_io_free(struct api *api)
{
    struct event_base *base = bufferevent_get_base(api->bev);

    api_close(api);
    destroy_response_queue(api);
    bufferevent_free(api->bev);
}

static void api_read_cb(struct bufferevent *bev, void *ctx)
{
    struct evbuffer *input_buffer;
    char *line;
    struct api *api = (struct api *) ctx;

    input_buffer = bufferevent_get_input(bev);

    while(line = evbuffer_readln(input_buffer, NULL, EVBUFFER_EOL_ANY)) {
        process_api_line(api, line);
        free(line);
    }
}

static void api_event_cb(struct bufferevent *bev, short what, void *ctx)
{
    struct api *api = (struct api *) ctx;
    switch(what) {
        case BEV_EVENT_CONNECTED:
            output("Waveform has connected to the radio\n");
            api_init(api);
            break;
        case BEV_EVENT_ERROR:
            output("Waveform failed to connect to the radio\n");
            api_io_free(api);
            break;
        case BEV_EVENT_TIMEOUT:
            output("Waveform timed out connecting to the radio\n");
            api_io_free(api);
            break;
        default:
            output("Unknown socket event\n");
            break;
    }
}

struct api *api_io_new(struct sockaddr_in *radio_addr, struct event_base *base)
{
    struct api *api = (struct api *) malloc(sizeof(struct api));
    if (api == NULL) {
        output("Couldn't allocate memory for io structure\n");
        return NULL;
    }
    api->rq_head = NULL;
    api->sequence = 0;

    api->bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(api->bev, api_read_cb, NULL, api_event_cb, api);
    bufferevent_enable(api->bev, EV_READ|EV_WRITE);

    if (bufferevent_socket_connect(api->bev, (struct sockaddr *) radio_addr, sizeof(struct sockaddr_in)) < 0) {
        bufferevent_free(api->bev);
        output("Could not connect to radio: %s\n", strerror(errno));
        return NULL;
    }

	return api;
}

struct event_base *api_io_get_base(struct api *api)
{
   return bufferevent_get_base(api->bev);
}

unsigned int send_api_command(struct api *api, response_cb cb, void *ctx, char *command, ...)
{
    int cmdlen = 0;
    va_list ap;
    char message_format[MAX_API_COMMAND_SIZE + 4];
    struct evbuffer *output = bufferevent_get_output(api->bev);

	cmdlen = snprintf(message_format, MAX_API_COMMAND_SIZE, "C%d|%s\n", api->sequence, command);
	if (cmdlen < 0)
	    return -1;

	va_start(ap, command);
    evbuffer_add_vprintf(output, message_format, ap);
	va_end(ap);

	if (cb)
	    add_sequence_to_response_queue(api, cb, ctx);

    return ++api->sequence;
}

int get_radio_addr(struct api *api, struct sockaddr_in *addr)
{
	socklen_t addr_len = sizeof(struct sockaddr_in);

	return getpeername(bufferevent_getfd(api->bev), (struct sockaddr *) addr, &addr_len);
}
