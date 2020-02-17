// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * main.c - FreeDV Main Thread
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

#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>

#include "event2/event.h"
#include "event2/bufferevent.h"
#include "event2/buffer.h"

#include "discovery.h"
#include "api-io.h"
#include "vita-io.h"
#include "utils.h"

const char* APP_NAME = "FreeDV";            // Name of Application
static const char *version = "2.0.0";
extern const char *GIT_REV;

void signal_cb(evutil_socket_t sock, short what, void *ctx)
{
    struct event_base *base = (struct event_base *) ctx;
    if(what & EV_SIGNAL) {
        output("Program stop requested.  Shutting Down\n");
        event_base_loopexit(base, NULL);
    }
}

int main(int argc, char **argv)
{
	struct sockaddr_in radio_address;
	struct event_base *base;
	struct api *api;
	pthread_t this_thread = pthread_self();
	struct sched_param thread_param;
	int ret;
	struct event *terminate;

	output("SmartSDR FreeDV Waveform v%s (%s)\n", version, GIT_REV);
	// XXX TODO: Loop around discovery/initiate?

	radio_address.sin_family = AF_INET;

	if (discover_radio(&radio_address) == -1) {
		output("Failed to find radio\n");
		exit(1);
	}
	output("Found radio at %s:%d\n", inet_ntoa(radio_address.sin_addr), ntohs(radio_address.sin_port));

//	event_enable_debug_logging(EVENT_DBG_ALL);
    base = event_base_new();

    thread_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    ret = pthread_setschedparam(this_thread, SCHED_FIFO, &thread_param);
    if (ret != 0) {
      output("Cannot set realtime priority, %s\n", strerror(ret));
    }

    api = api_io_new(&radio_address, base);
    if(!api) {
        output("Error occurred connecting to radio\n");
        exit(0);
    }

	// TODO: This should have a reference to the api loop passed so that we
	//       can shut things down correctly.  We also probably need to send in
	//       the base as well.
    terminate = evsignal_new(base, SIGINT, signal_cb, base);
    event_add(terminate, NULL);

    event_base_dispatch(base);

    api_io_free(api);

    output("FreeDV Waveform Stopped.\n");
    exit(0);
}



