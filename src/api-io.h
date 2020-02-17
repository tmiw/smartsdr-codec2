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
 * along with smartsdr-codec2.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#ifndef API_IO_H
#define API_IO_H

#include <netinet/in.h>
#include "event2/event.h"

struct api;
typedef void(*response_cb) (struct api *api, void *ctx, unsigned int code, char *message);

struct api *api_io_new(struct sockaddr_in *radio_addr, struct event_base *base);
void api_io_free(struct api *api);
struct event_base *api_io_get_base(struct api *api);
unsigned int send_api_command(struct api *api, response_cb cb, void *ctx, char *command, ...);
int get_radio_addr(struct api *api, struct sockaddr_in *addr);

#define send_api_command_simple(api, format, ...) send_api_command(api, NULL, NULL, format, ##__VA_ARGS__)

#endif // API_IO_H_
