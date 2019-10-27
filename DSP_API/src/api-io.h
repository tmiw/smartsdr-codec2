// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * api-io.h - Support for command and control API socket of FlexRadio 6000
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
 * along with Foobar.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#ifndef API_IO_H
#define API_IO_H

#include <netinet/in.h>

int api_io_init(struct sockaddr_in *radio_addr);
int wait_for_api_io();
unsigned int send_api_command(char *command, ...);
unsigned int send_api_command_and_wait(char *command, char **response_message, ...);
int get_radio_addr(struct sockaddr_in *addr);

#endif // API_IO_H_
