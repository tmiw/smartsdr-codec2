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

#include "discovery.h"
#include "api-io.h"
#include "vita-io.h"
#include "utils.h"

const char* APP_NAME = "FreeDV";            // Name of Application

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// 	main()

int main(int argc, char **argv)
{
	struct sockaddr_in radio_address;
    sigset_t stop_sigs;

    sigemptyset(&stop_sigs);
    sigaddset(&stop_sigs, SIGINT);
    sigaddset(&stop_sigs, SIGTERM);

	// XXX TODO: Loop around discovery/initiate?

	radio_address.sin_family = AF_INET;

	if (discover_radio(&radio_address) == -1) {
		output("Failed to find radio\n");
		exit(1);
	}
	output("Found radio at %s:%d\n", inet_ntoa(radio_address.sin_addr), ntohs(radio_address.sin_port));

	if (api_io_init(&radio_address) == -1) {
		output("Couldn't connect to radio\n");
		exit(1);
	}

	output("Radio connected\n");
	send_api_command("sub slice all");
    if (meter_table[0].id == 0)
        register_meters(meter_table);

	//  TODO: These commands should be encapsulated in freedv handling code in
	//        separate file.
	send_api_command("waveform create name=FreeDV-USB mode=FDVU underlying_mode=USB version=2.0.0");
	send_api_command("waveform set FreeDV-USB tx=1");
    send_api_command("waveform set FreeDV-USB rx_filter depth=8");
    send_api_command("waveform set FreeDV-USB tx_filter depth=8");

	send_api_command("waveform create name=FreeDV-LSB mode=FDVL underlying_mode=LSB version=2.0.0");
	send_api_command("waveform set FreeDV-LSB tx=1");
    send_api_command("waveform set FreeDV-LSB rx_filter depth=8");
    send_api_command("waveform set FreeDV-LSB tx_filter depth=8");

    sigprocmask(SIG_BLOCK, &stop_sigs, NULL);
    while (sigwaitinfo(&stop_sigs, NULL) < 0 && errno == EINTR);

    output("Program stop requested.  Shutting Down\n");
    send_api_command("waveform remove FreeDV-USB");
    vita_stop();
    api_io_stop();
	output("FreeDV Waveform Stopped.\n");
    exit(0);
}



