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
 * along with smartsdr-codec2.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <libltdl/lt_system.h>

#include "api.h"
#include "api-io.h"
#include "utils.h"
#include "vita-io.h"
#include "freedv-processor.h"
//#include "../../../Library/Caches/CLion2019.3/.remote/commune_22/2778bf1d-d128-48da-9588-887f0d6ac5cc/home/mcdermj/x-tools/flex6k/lib/gcc/arm-linux-gnueabihf/8.3.0/include/stddef.h"

static int active_slice = -1;
static unsigned int is_lsb = 0;
struct vita *vita;

struct mode_entry {
    int mode;
    char *name;
    int high_cut;
    int low_cut;
    int offset;
};

static const struct mode_entry mode_table[] = {
        { FREEDV_MODE_700C,     "700C",     250,    2750, 1500 },
        { FREEDV_MODE_700D,     "700D",     250,    2750, 1500 },
        { FREEDV_MODE_800XA,    "800XA",    250,    2750, 1500 },
        { FREEDV_MODE_1600,     "1600",     250,    2750, 1500 }
};

static void set_mode_filter(struct api *api, int slice, const struct mode_entry *entry)
{
    if(is_lsb) {
        send_api_command_simple(api, "filt %d %d %d", slice, entry->low_cut * -1, entry->high_cut * -1);
        send_api_command_simple(api, "slice set %d digl_offset=%d", slice, entry->offset * -1);
    } else {
        send_api_command_simple(api, "filt %d %d %d", slice, entry->high_cut, entry->low_cut);
        send_api_command_simple(api, "slice set %d digu_offset=%d", slice, entry->offset);
    }
}

static void send_waveform_status(struct api *api) {
//    for (unsigned long i = 0; i < ARRAY_SIZE(mode_table); ++i)
//        if (mode_table[i].mode == freedv_proc_get_mode(freedv_params))
//            send_api_command_simple(api, "waveform status slice=%d fdv-mode=%s fdv-squelch-enable=%d fdv-squelch-level=%d", active_slice, mode_table[i].name, freedv_proc_get_squelch_status(freedv_params), freedv_proc_get_squelch_level(freedv_params));
}

static void change_to_fdv_mode(struct api *api, unsigned char slice) {
    unsigned short vita_port;
    struct sockaddr_in radio_addr;

    if (active_slice == slice || active_slice < 0) {
        for (unsigned long i = 0; i < ARRAY_SIZE(mode_table); ++i)
            if (mode_table[i].mode == FREEDV_MODE_1600)
                set_mode_filter(api, slice, &mode_table[i]);
    }
    
    if (active_slice >= 0) {
        output("Slice %u is using the waveform\n", active_slice);
        send_waveform_status(api);
        return;
    }

    output("Slice %u changed to FDV mode\n", slice);
    active_slice = slice;

    // Start the VITA-49 processing system
    get_radio_addr(api, &radio_addr);
    vita = vita_new(api_io_get_base(api), &radio_addr);
    if(!vita) {
        output("Couldn't start VITA-49 processor\n");
        return;
    }

	//  Inform the radio of our chosen port
	vita_port = vita_get_port(vita);
	output("Using port %hu for VITA-49 communications\n", vita_port);
	send_api_command_simple(api, "waveform set FreeDV-USB udpport=%d", vita_port);
    send_api_command_simple(api, "waveform set FreeDV-LSB udpport=%d", vita_port);
    send_api_command_simple(api, "client udpport %d", vita_port);

    send_waveform_status(api);
}

static void change_from_fdv_mode(unsigned char slice)
{
    if (slice != active_slice)
        return;

    vita_free(vita);
	active_slice = -1;
}

static int process_slice_status(void *ctx, char **argv, int argc)
{
    struct api *api = (struct api *) ctx;
	long slice;
	char *value;
	char *end;
	struct kwarg *kwargs;
	char *mode;

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

	kwargs = parse_kwargs(argv, argc, 2);
	if((mode = find_kwarg(kwargs, "mode")) != NULL) {
        if (strcmp("FDVU", mode) == 0) {
            is_lsb = 0;
            change_to_fdv_mode(api, slice);
        } else if (strcmp("FDVL", mode) == 0) {
            is_lsb = 1;
            change_to_fdv_mode(api, slice);
        } else {
            change_from_fdv_mode(slice);
        }
	}

	kwargs_destroy(&kwargs);
	return 0;
}

static int process_interlock_status(void *ctx, char **argv, int argc)
{
    struct kwarg *kwargs;
    char *state;

    if (argc < 2) {
        output("Not enough arguments to slice status message (%d)\n", argc);
        return -1;
    }

//    kwargs = parse_kwargs(argv, argc, 1);
//    if((state = find_kwarg(kwargs, "state")) != NULL) {
//        output("Interlock changed state to %s\n", state);
//        if (strcmp("READY", state) == 0)
//            freedv_set_xmit_state(freedv_params, READY);
//        else if (strcmp("PTT_REQUESTED", state) == 0)
//            freedv_set_xmit_state(freedv_params, PTT_REQUESTED);
//        else if (strcmp("TRANSMITTING", state) == 0)
//            freedv_set_xmit_state(freedv_params, TRANSMITTING);
//        else if (strcmp("UNKEY_REQUESTED", state) == 0)
//            freedv_set_xmit_state(freedv_params, UNKEY_REQUESTED);
//        else if (strcmp("RECEIVE", state) == 0)
//            freedv_set_xmit_state(freedv_params, RECEIVE);
//        else
//            output("Unknown interlock state %s\n", state);
//    }
//
//    kwargs_destroy(&kwargs);
    return 0;
}

static const struct dispatch_entry status_dispatch_table[] = {
	{ "slice", &process_slice_status },
    { "interlock", &process_interlock_status },
	{ "", NULL },
};

//static int process_slice_command(char **argv, int argc) {
//    char *value;
//    struct kwarg *kwargs;
//
//    if (argc < 3) {
//        output("Improper number of arguments (%d)\n", argc);
//        return -1;
//    }
//
//    if (freedv_params == NULL)
//        return -1;
//
//    kwargs = parse_kwargs(argv, argc, 2);
//    if ((value = find_kwarg(kwargs, "fdv-set-mode"))) {
//        int found = 0;
//        for (unsigned long i = 0; i < ARRAY_SIZE(mode_table); ++i) {
//            if (strcmp(mode_table[i].name, value) == 0) {
//                fdv_set_mode(freedv_params, mode_table[i].mode);
//                set_mode_filter(active_slice, &mode_table[i]);
//                found = 1;
//            }
//        }
//
//        //  Invalid mode
//        if (!found)
//            goto fail;
//    } else if ((value = find_kwarg(kwargs, "fdv-set-squelch-level"))) {
//        float squelch;
//        char *end;
//
//        if (strlen(value) == 0)
//            goto fail;
//
//        errno = 0;
//        squelch = strtof(value, &end);
//        if(*end) {
//            output("Invalid squelch value: %s\n",value);
//            goto fail;
//        }
//        if (errno) {
//            output("Error converting squelch value: %s\n", strerror(errno));
//            goto fail;
//        }
//
//        freedv_set_squelch_level(freedv_params, squelch);
//    } else if ((value = find_kwarg(kwargs, "fdv-set-squelch-enable"))) {
//        if (strcmp(value, "true") == 0)
//            freedv_set_squelch_status(freedv_params, 1);
//        else if (strcmp(value, "false") == 0)
//            freedv_set_squelch_status(freedv_params, 0);
//        else
//            goto fail;
//    } else {
//        //  Invalid command
//        goto fail;
//    }
//    kwargs_destroy(&kwargs);
//
////    send_waveform_status(api);
//    return 0;
//
//    fail:
//    kwargs_destroy(&kwargs);
//    return -1;
//}

//static const struct dispatch_entry command_dispatch_table[] = {
//	{ "slice", &process_slice_command },
//	{ "", NULL },
//};


int process_status_message(struct api *api, char *message)
{
    assert(message != NULL);
    return dispatch_from_table(api, message, status_dispatch_table);
}

//int process_waveform_command(unsigned int sequence, char *message)
//{
//    int ret;
//
//    assert(message != NULL);
//
//    ret = dispatch_from_table(message, command_dispatch_table);
//    if(!ret) {
//        send_api_command("waveform response %d|0", sequence);
//    } else {
//        send_api_command("waveform response %d|50000016", sequence);
//    }
//
//    return ret;
//}

void reg_meter_cb(struct api *api, void *ctx, unsigned int code, char *message)
{
    struct meter_def *meter = (struct meter_def *) ctx;
    if (code != 0) {
        output("Failed to register meter: %s\n", meter->name);
        return;
    }

    errno = 0;
    meter->id = strtol(message, NULL, 10);
    if (errno != 0) {
        meter->id = 0;
        output("Got nonsensical meter id for %s (%s)\n", meter->name, message);
    }

    output("Allocated meter id %d\n", meter->id);
}

int register_meters(struct api *api, struct meter_def *meters)
{
	assert(meters != NULL);

	for (int i = 0; strlen(meters[i].name) != 0; ++i)
	    send_api_command(api, reg_meter_cb, &meters[i], "meter create name=%s type=WAVEFORM min=%f max=%f unit=%s fps=20", meters[i].name, meters[i].min, meters[i].max, meters[i].unit);

}

int find_meter_by_name(struct meter_def *meters, char *name)
{
    int i;

    for (i = 0; strlen(meters[i].name) != 0; ++i)
        if (strcmp(meters[i].name, name) == 0)
            return meters[i].id;

    return -1;
}

void api_init(struct api *api)
{
    send_api_command_simple(api, "sub slice all");
    send_api_command_simple(api, "waveform create name=FreeDV-USB mode=FDVU underlying_mode=USB version=2.0.0");
    send_api_command_simple(api, "waveform set FreeDV-USB tx=1");  // TODO: This is broken?
    send_api_command_simple(api, "waveform set FreeDV-USB rx_filter depth=8");
    send_api_command_simple(api, "waveform set FreeDV-USB tx_filter depth=8");

    send_api_command_simple(api, "waveform create name=FreeDV-LSB mode=FDVL underlying_mode=LSB version=2.0.0");
    send_api_command_simple(api, "waveform set FreeDV-LSB tx=1"); // TODO: This is broken?
    send_api_command_simple(api, "waveform set FreeDV-LSB rx_filter depth=8");
    send_api_command_simple(api, "waveform set FreeDV-LSB tx_filter depth=8");

    register_meters(api, meter_table);
}

void api_close(struct api *api)
{
    send_api_command_simple(api, "waveform remove FreeDV-USB");
}