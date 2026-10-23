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
// cl_uispeeds.h -- ui_speeds' samples and their summaries, apart from the
// clock they're read from (cl_uispeeds_stats.c)

#ifndef CL_UISPEEDS_H
#define CL_UISPEEDS_H

// a set of per-frame samples, in nanoseconds or counts. It holds at most
// its capacity, which is even and at least 2: when full, it drops every
// other sample it holds and keeps one in twice as many after, so what it
// holds is an even sample of all it was given. Each is held as an int, so
// past about 2.1 seconds as that; the mean and max take every sample as
// it was given.
typedef struct {
	int			*values;
	int			capacity;
	int			count;		// held
	int			stride;		// one sample kept in every stride given
	int			seen;		// given in all
	int64_t		total;
	int64_t		max;
} uisSamples_t;

typedef struct {
	int			count;		// samples given
	double		median;
	double		p95;		// the nearest rank: at least 95% are at most this
	double		mean;
	int64_t		max;
} uisSummary_t;

void		UIS_Init( uisSamples_t *s, int *storage, int capacity );
void		UIS_Clear( uisSamples_t *s );
void		UIS_Add( uisSamples_t *s, int64_t value );

// the held samples' median and p95, sorting them in scratch, which holds
// the capacity; qfalse if none were given
qboolean	UIS_Summarize( const uisSamples_t *s, int *scratch, uisSummary_t *out );

// a span's time less the clock reads inside it, each costing clockCost
// nanoseconds; never below 0
int64_t		UIS_Corrected( int64_t span, int reads, double clockCost );

#endif
