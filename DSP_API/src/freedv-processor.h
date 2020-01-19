// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * freedv-processor.h - FreeDV sample processing
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

#ifndef SCHED_WAVEFORM_H_
#define SCHED_WAVEFORM_H_

//  To import the constants to feed to freedv_set_mode
#include "freedv_api.h"

typedef struct freedv_proc_t *freedv_proc_t;
enum freedv_xmit_state { READY, PTT_REQUESTED, TRANSMITTING, UNKEY_REQUESTED };

freedv_proc_t freedv_init(int mode);
void freedv_set_mode(freedv_proc_t params, int mode);
void freedv_queue_samples(freedv_proc_t params, int tx, size_t len, uint32_t *samples);
void freedv_destroy(freedv_proc_t params);
int freedv_proc_get_mode(freedv_proc_t params);
void freedv_set_xmit_state(freedv_proc_t params, enum freedv_xmit_state state);
void freedv_set_squelch_level(freedv_proc_t params, float squelch);
void freedv_set_squelch_status(freedv_proc_t params, int status);
float freedv_proc_get_squelch_level(freedv_proc_t params);
int freedv_proc_get_squelch_status(freedv_proc_t params);


#endif /* SCHED_WAVEFORM_H_ */
