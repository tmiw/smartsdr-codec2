// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * api.h - API Processing
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
 * XXX TODO: Fix Foobars in licensing
 */
#ifndef API_H
#define API_H

struct meter_def {
	unsigned short	id;
	char name[32];
	float min;
	float max;
	char unit[16];  // TODO: enum?
};

int process_status_message(char *message);
int process_waveform_command(char *message);
int register_meters(struct meter_def *meters);
int find_meter_by_name(struct meter_def *meters, char *name);

#endif // API_H_
