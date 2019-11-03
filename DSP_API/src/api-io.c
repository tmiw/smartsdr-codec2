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
#include <semaphore.h>
#include <stdbool.h>
#include <stdarg.h>
#include <limits.h>
#include <signal.h>

#include "api-io.h"
#include "api.h"
#include "utils.h"
#include "ringbuf.h"

#define MAX_API_COMMAND_SIZE 1024

struct response_queue_entry {
	unsigned int sequence;
	unsigned int code;
	char *message;
	struct response_queue_entry *next;
};

static int api_io_socket;
static unsigned int api_cmd_sequence;
static unsigned int api_session_handle;
static int api_version_major[2];
static int api_version_minor[2];
static bool api_io_abort = false;
static pthread_t api_io_thread;
static struct response_queue_entry *response_queue_head = NULL;
static pthread_mutex_t response_queue_lock;
static sem_t response_queue_sem;

static void add_sequence_to_response_queue(unsigned int sequence)
{
	struct response_queue_entry *new_entry, *current_entry;

	new_entry = (struct response_queue_entry *) malloc(sizeof(struct response_queue_entry));

	new_entry->sequence = sequence;
	new_entry->code = 0xffffffff;
	new_entry->next = NULL;

	pthread_mutex_lock(&response_queue_lock);
	if (response_queue_head == NULL) {
		response_queue_head = new_entry;
	} else {
		for (current_entry = response_queue_head; current_entry->next != NULL; current_entry = current_entry->next);
		current_entry->next = new_entry;
	}
	pthread_mutex_unlock(&response_queue_lock);
}

static void complete_response_entry(unsigned int sequence, unsigned int code, char *message)
{
	struct response_queue_entry *current_entry;

	if (response_queue_head == NULL)
		return;

	pthread_mutex_lock(&response_queue_lock);
	for (current_entry = response_queue_head; current_entry != NULL; current_entry = current_entry->next) {
		if (current_entry->sequence == sequence) {
			current_entry->code = code;
			current_entry->message = message;
			sem_post(&response_queue_sem);
			break;
		}
	}
	pthread_mutex_unlock(&response_queue_lock);
}

static struct response_queue_entry *pop_response_with_sequence(unsigned int sequence)
{
	struct response_queue_entry *current_entry, *last_entry;

	assert(response_queue_head != NULL);

	pthread_mutex_lock(&response_queue_lock);
	for (current_entry = last_entry = response_queue_head;
	     current_entry != NULL;
	     last_entry = current_entry, current_entry = current_entry->next) {
		if (current_entry->sequence == sequence)
			break;
	}

	if (current_entry == NULL || current_entry->code == 0xffffffff) {
		pthread_mutex_unlock(&response_queue_lock);
		return NULL;
	}

	if (last_entry == current_entry) {
		response_queue_head = NULL;
	} else {
		last_entry->next = current_entry->next;
	}

	pthread_mutex_unlock(&response_queue_lock);
	return current_entry;
}

static void process_api_line(char *line)
{
	char *message, *endptr, *response_message;
	int ret;
	unsigned int handle, code, sequence;

	output("Received: %s\n", line);

	switch(*(line++)) {
	case 'V':
		errno = 0;
		ret = sscanf(line, "%d.%d.%d.%d", &api_version_major[0], &api_version_major[1], &api_version_minor[0], &api_version_minor[1]);
		if (ret != 4)
			output("Error converting version string: %s\n", line);

		output("Radio API Version: %d.%d(%d.%d)\n", api_version_major[1], api_version_major[1], api_version_minor[0], api_version_minor[1]);

		break;
	case 'H':
		errno = 0;
        api_session_handle = strtoul(line, &endptr, 16);
        if ((errno == ERANGE && api_session_handle == ULONG_MAX) ||
            (errno != 0 && api_session_handle == 0)) {
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

        process_status_message(endptr + 1);
		break;
	case 'M':
	    //  We don't handle messages
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

        //  We need a copy of message because it needs to stick around
        //  in the response queue until the radio responds.
        message = (char *) malloc(strlen(response_message) + 1);
        strcpy(message, response_message + 1);

        complete_response_entry(sequence, code, message);
		break;
	case 'C':
		errno = 0;
        sequence = strtoul(line, &endptr, 10);
        if ((errno == ERANGE && sequence == ULONG_MAX) ||
            (errno != 0 && sequence == 0)) {
            output("Error finding command sequence: %s\n", strerror(errno));
            break;
        }

        if (line == endptr) {
            output("Cannot find command sequence in: %s\n", line);
            break;
        }

        process_waveform_command(sequence, endptr + 1);
		break;
	default:
		output("Unknown command: %s\n", line);
		break;
	}
}

static void *api_io_processing_loop(void *arg)
{
	char *line;
	int ret;
	ringbuf_t buffer;
	size_t eol;

	struct pollfd fds = {
		.fd = api_io_socket,
		.events = POLLIN,
		.revents = 0,
	};

	buffer = ringbuf_new(4096);

	output("Beginning API IO Loop...\n");
	while (!api_io_abort) {
		ret = poll(&fds, 1, 500);
		if (ret == 0) {
			// timeout
			continue;
		} else if (ret == -1) {
			// error
			output("Poll failed: %s\n", strerror(errno));
			continue;
		}

		if (ringbuf_read(api_io_socket, buffer, ringbuf_bytes_free(buffer)) == -1) {
            output("API IO read failed: %s\n", strerror(errno));
            close(api_io_socket);
            return (void *) -1;
		}

        while ((eol = ringbuf_findchr(buffer, '\n', 0)) != ringbuf_bytes_used(buffer)) {
            line = (char *) malloc(eol + 1);
            ringbuf_memcpy_from(line, buffer, eol + 1);

            if (eol != 0) {
                line[eol] = '\0';
                process_api_line(line);
            }
            free(line);
        }
	}
	output("API IO Loop Ending...\n");

	ringbuf_free(&buffer);

	close(api_io_socket);

	return (void *) 0;
}

int api_io_init(struct sockaddr_in *radio_addr)
{

	api_io_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (api_io_socket == -1) {
		output("Failed to initialize TCP socket: %s\n", strerror(errno));
		return -1;
	}

	if (connect(api_io_socket, (struct sockaddr *) radio_addr, sizeof(struct sockaddr_in)) == -1) {
		output("Couldn't connect to %s:%d: %s\n", inet_ntoa(radio_addr->sin_addr), radio_addr->sin_port, strerror(errno));
		close(api_io_socket);
		return -1;
	}

	if(fcntl(api_io_socket, F_SETFL, O_NONBLOCK) == -1) {
		output("Couldn't set API IO socket to nonblocking: %s\n", strerror(errno));
		close(api_io_socket);
		return -1;
	}

	if (pthread_create(&api_io_thread, NULL, &api_io_processing_loop, radio_addr) != 0) {
		output("Cannot create API IO thread: %s\n", strerror(errno));
		return -1;
	}

	sem_init(&response_queue_sem, 0, 0);
}

void api_io_stop()
{
    api_io_abort = true;
    pthread_join(api_io_thread, NULL);
    close(api_io_socket);
}

unsigned int send_api_command(char *command, ...)
{
    ssize_t bytes_written;
    int cmdlen = 0;
    va_list ap;
    char message[MAX_API_COMMAND_SIZE];
    char message_format[MAX_API_COMMAND_SIZE + 4];

	snprintf(message_format, MAX_API_COMMAND_SIZE, "C%d|%s\n", api_cmd_sequence++, command);
	if (cmdlen < 0)
	    return -1;

	va_start(ap, command);
	cmdlen = vsnprintf(message, sizeof(message), message_format, ap);
	va_end(ap);

	output("Sending: %s", message);

    bytes_written = write(api_io_socket, message, (size_t) cmdlen);
    if (bytes_written == -1) {
    	output("Error writing to TCP API socket: %s\n", strerror(errno));
    	return -1;
    } else if (bytes_written != cmdlen) {
        output("Short write to TCP API socket\n");
        return -1;
    }

    return api_cmd_sequence - 1;
}

unsigned int send_api_command_and_wait(char *command, char **response_message, ...)
{
	unsigned int code, sequence;
	struct response_queue_entry *response;
	va_list ap;

	va_start(ap, response_message);
	if ((sequence = send_api_command(command, ap)) == -1)
		return sequence;
	va_end(ap);

	add_sequence_to_response_queue(sequence);

	// XXX We may want to do some sort of timeout here to make sure that
	// XXX things don't hang up.
	while((response = pop_response_with_sequence(sequence)) == NULL)
		sem_wait(&response_queue_sem);

	if (response_message)
		*response_message = response->message;
	code = response->code;
	free(response);

	return code;
}

int get_radio_addr(struct sockaddr_in *addr)
{
	socklen_t addr_len = sizeof(struct sockaddr_in);

	return getpeername(api_io_socket, (struct sockaddr *) addr, &addr_len);
}
