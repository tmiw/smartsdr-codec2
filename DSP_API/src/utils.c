// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * utils.c - General Utilities
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

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

void output(const char *fmt,...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    fflush(stdout);
    va_end(args);
}

int parse_argv(char *string, char **argv, int max_args)
{
	char **argptr;
	char *argsstring;
	int argc = 1;

	argsstring = string;
	for(argptr = argv; (*argptr = strsep(&argsstring, " \t")) != NULL; ++argc)
		if (**argptr != '\0')
			if (++argptr >= &argv[max_args])
				break;

	return(argc - 1);
}

short float_to_fixed(double input, char fractional_bits)
{
    return (short)(round(input * (1 << fractional_bits)));
}