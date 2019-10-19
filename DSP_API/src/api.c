// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * api.c - API Processing
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
 * along with Foobar.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>

#include "api.h"
#include "api-io.h"
#include "utils.h"
#include "vita-io.h"
#include "sched_waveform.h"

#define MAX_ARGS		128

typedef int (*dispatch_handler_t)(char **, int);

struct dispatch_entry {
	char name[256];
	dispatch_handler_t handler;
};

static unsigned int num_slices = 0;

int dispatch_from_table(char *message, const struct dispatch_entry *dispatch_table)
{
	char *argv[MAX_ARGS];
	int argc;
 	int i;

 	assert(message != NULL);
 	assert(dispatch_table != NULL);

// 	output("Processing message: %s\n", message);

	argc = parse_argv(message, argv, MAX_ARGS);
	if (argc < 1)
		return -1;

	for (i = 0; strlen(dispatch_table[i].name) > 0; ++i) {
// 		output("Processing %s\n", dispatch_table[i].name);
		if (strncmp(dispatch_table[i].name, argv[0], 256) == 0) {
			assert(dispatch_table[i].handler != NULL);
			return (dispatch_table[i].handler)(argv, argc);
		}
	}

	return -1;
}

static void change_to_fdv_mode() {
	output("Slice changed to FDV mode\n");
	unsigned short vita_port;
	char command[256];

	if (num_slices++ > 0) {
		output("Slices using waveform, no need to start another\n");
		return;
	}

	// Start up the processing loop
	sched_waveform_Init();

	// Start the VITA-49 processing system
	if ((vita_port = vita_init()) == 0) {
		output ("Cannot start VITA-49 processing loop\n");
		return;
	}
	//  Inform the radio of our chosen port
	output("Using port %hu for VITA-49 communications\n", vita_port);
	snprintf(command, sizeof(command), "waveform set FreeDV-USB udpport=%d", vita_port);
	send_api_command(command);
	snprintf(command, sizeof(command), "waveform set FreeDV-LSB udpport=%d", vita_port);
	send_api_command(command);
}

static void change_from_fdv_mode()
{
	if (--num_slices > 0) {
		output("Slices still using waveform, not destroying\n");
		return;
	}
// 	output("Mode = %s\n", value);

	//  Stop the VITA-49 loop
	vita_stop();

	//  Stop the processing loop
	sched_waveformThreadExit();
}

static int process_slice_status(char **argv, int argc) {
	int i;
	int slice;
	char *value;
	char *end;

	if (argc < 3) {
		output("Not enough arguments to slice status message (%d)\n", argc);
		return -1;
	}

	errno = 0;
	slice = strtol(argv[1], &end, 0);
	if(*end) {
		output("Invalid slice specification: %s\n", argv[1]);
		return -1;
	}
	if (errno) {
		output("Error converting slice: %s\n", strerror(errno));
		return -1;
	}


	//  XXX We seem to do this a lot.  Maybe make a function that takes
	//  XXX a function pointer and iterates over all the args to process
	//  XXX key value pairs and evaluate them.
	for (i = 2; i < argc; ++i) {
		value = argv[i];
		strsep(&value, "=");
// 		output("Key: %s, Value: %s\n", argv[i], value);

		//  XXX Need to handle switching back here.  We probably need to
		//  XXX keep track of which slice we're using and go from there.
		//  XXX Maybe need to handle multi-slice eventually by forking an
		//  XXX additional processing thread for stuff.  Dunno.
		if(strcmp("mode", argv[i]) == 0) {
			if (strcmp("FDVU", value) == 0 ||
			    strcmp("FDVL", value) == 0) {
			    change_to_fdv_mode();
			} else {
				change_from_fdv_mode();
			}
		} else if (strcmp("in_use", argv[i]) == 0) {
		} else if (strcmp("tx", argv[i]) == 0) {
		}
	}

	return 0;
}

static const struct dispatch_entry status_dispatch_table[] = {
	{ "slice", &process_slice_status },
	{ "", NULL },
};

static int process_slice_command(char **argv, int argc) {
	char *value;

	if (argc != 3) {
		output("Improper number of arguments (%d)\n", argc);
		return -1;
	}
	value = argv[2];
	strsep(&value, "=");
	if (strcmp("fdv_mode", argv[2]) != 0) {
		return 0;
	}

	if (strcmp("700C", value) == 0) {
		output("Need to switch to 700C\n");
	} else if (strcmp("1200A", value) == 0) {
		output("Need to switch to 1200A\n");
	} else {
		output("Invalid FreeDV mode: %s\n", value);
	}

	return 0;
}

static const struct dispatch_entry command_dispatch_table[] = {
	{ "slice", &process_slice_command },
	{ "", NULL },
};


int process_status_message(char *message)
{
	assert(message != NULL);
	return dispatch_from_table(message, status_dispatch_table);
}

int process_waveform_command(char *message)
{
	assert(message != NULL);
	return dispatch_from_table(message, command_dispatch_table);
}

int register_meters(struct meter_def *meters)
{
	int i, id, response_code;
	char meter_cmd[256];
	char *response_message;

	assert(meters != NULL);

	for (i = 0; strlen(meters[i].name) != 0; ++i) {
		snprintf(meter_cmd, sizeof(meter_cmd), "meter create name=%s type=WAVEFORM min=%f max=%f unit=%s", meters[i].name, meters[i].min, meters[i].max, meters[i].unit);
		response_code = send_api_command_and_wait(meter_cmd, &response_message);

		if (response_code != 0) {
			output("Failed to register meter %s\n", meters[i].name);
			return -1;
		}

		errno = 0;
		meters[i].id = strtol(response_message, NULL, 10);
		if (errno != 0) {
			meters[i].id = 0;
			output("Got nonsensical meter id for %s (%s)\n", meters[i].name, response_message);
		}

		output("Allocated meter id %d\n", meters[i].id);
		free(response_message);
	}
}

