/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// com_pacing.c -- the client's frame cap as an interval, apart from the
// clock and the display it's read from

#include "q_shared.h"
#include "qcommon.h"

/*
=================
Com_RefreshNanoseconds

A display's refresh period in nanoseconds, from its rate as a fraction, as
SDL gives it: 60000/1001 Hz is 16683333 ns, not 60 Hz's 16666666. 0 if it
isn't known
=================
*/
int64_t Com_RefreshNanoseconds( int numerator, int denominator ) {
	if ( numerator <= 0 || denominator <= 0 ) {
		return 0;
	}
	return (int64_t)1000000000 * denominator / numerator;
}

/*
=================
Com_CapInterval

The interval of a cap of fps frames a second, in nanoseconds. A classic
cap, 1000/k rounded down (125, 250, 333, 90...), is exactly k milliseconds
a frame, as every Quake III engine has given it: players choose those
for how the game moves in frames of that many milliseconds. Any other
rate is paced exactly.
=================
*/
int64_t Com_CapInterval( int fps ) {
	int		k = 1000 / fps;

	if ( k > 0 && 1000 / k == fps ) {
		return k * (int64_t)1000000;
	}
	return 1000000000 / fps;
}

/*
=================
Com_MaxFpsInterval

com_maxfps's cap as an interval in nanoseconds, given the display's refresh
period (0 if unknown): a number of frames a second (Com_CapInterval); the
display's refresh rate (COM_MAXFPS_DISPLAY), exactly; 3% under it
(COM_MAXFPS_BELOW_DISPLAY), for a display of variable refresh rate with
vsync; 125 where the rate isn't known; or 0, no cap
=================
*/
int64_t Com_MaxFpsInterval( int maxfps, int64_t refresh ) {
	if ( maxfps > 0 ) {
		return Com_CapInterval( maxfps );
	}
	if ( maxfps == 0 ) {
		return 0;
	}
	if ( refresh <= 0 ) {
		return Com_CapInterval( 125 );
	}
	if ( maxfps == COM_MAXFPS_BELOW_DISPLAY ) {
		return refresh * 100 / 97;
	}
	return refresh;
}
