/* *****************************************************************************
 *	main.c															2014 SEP 01
 *
 *  Author: Graham / KE9H
 *  Date created: August 5, 2014
 *
 *  Wrapper program for "Embedded FreeDV" including CODEC2.
 *
 *		Derived, in part from code provided by David Rowe under LGPL in:
 *			freedv_rx.c
 *			freedv_tx.c
 *
 *		Calls and API defined by David Rowe in
 *			freedv_api.c
 *			freedv_api.h
 *
 *  Portions of this file are Copyright (C) 2014 David Rowe
 *
 * *****************************************************************************
 *
 *	Copyright (C) 2014 FlexRadio Systems.
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *	GNU General Public License for more details.
 *	You should have received a copy of the GNU General Public License
 *	along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * *****************************************************************************
 * TODO
 * 		distinguish between rx data and tx data packets?
 * 		discard rx data and tx data packet if wrong type?
 * 		get call for S/N
 *
 *
 *
 **************************************************************************** */

#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "smartsdr_dsp_api.h"
#include "discovery.h"
#include "api-io.h"
#include "api.h"

#include "common.h"

const char* APP_NAME = "FreeDV";            // Name of Application
//const char* CFG_FILE = "FreeDV.cfg";        // Name of associated configuration file
char * cfg_path = NULL;

struct meter_def meter_table[] = {
	{ 0, "fdv-snr", 0.0, 100.0, "DB" },
	{ 0, "fdv-foff", 0.0, 1000000.0, "RPM" },
	{ 0, "fdv-clock-offset", 0.0, 1000000.0, "RPM"},
	{ 0, "fdv-sync-quality", 0.0, 1.0, "RPM"},
	{ 0, "", 0.0, 0.0, "" }
};


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// 	main()

int main(int argc, char **argv)
{
	struct sockaddr_in radio_address;
	int response_code;

	// XXX Loop around discovery/initiate?

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

	//  XXX These commands should be encapsulated in freedv handling code in
	//  XXX separate file.
	response_code = send_api_command_and_wait("waveform create name=FreeDV-USB mode=FDVU underlying_mode=USB version=2.0.0", NULL);
	output("Got response 0x%0.8x\n", response_code);
	send_api_command_and_wait("waveform create name=FreeDV-LSB mode=FDVL underlying_mode=LSB version=2.0.0", NULL);
	output("Got response 0x%0.8x\n", response_code);

// 	response_code = send_api_command_and_wait("meter create name=fdv-snr type=WAVEFORM min=0.0 max=100.0 unit=DB", response_message);
// 	if (response_code == 0)
// 		output("Meter ID is %s\n", response_message);
// 	else
// 		output("Failed to register meter with code %d\n", response_code);
	register_meters(meter_table);
	fflush(stdout);


	wait_for_api_io();
	output("Complete");

//     SmartSDR_API_Init(enable_console, restrict_ip);
    exit(0);
}



