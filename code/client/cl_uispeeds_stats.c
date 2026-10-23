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
// cl_uispeeds_stats.c -- ui_speeds' samples and their summaries

#include <stdlib.h>

#include "../qcommon/q_shared.h"
#include "cl_uispeeds.h"

void UISpeeds_Init( uisSamples_t *s, int *storage, int capacity ) {
	s->values = storage;
	s->capacity = capacity;
	UISpeeds_Clear( s );
}

void UISpeeds_Clear( uisSamples_t *s ) {
	s->count = 0;
	s->stride = 1;
	s->seen = 0;
	s->total = 0;
	s->max = 0;
}

void UISpeeds_Add( uisSamples_t *s, int64_t value ) {
	int v = value < 0 ? 0 : value > INT_MAX ? INT_MAX : (int)value;
	int i;

	if ( s->seen == INT_MAX ) {
		return;
	}
	if ( s->seen % s->stride == 0 ) {
		// full: keep every other, which are every other stride's; the
		// capacity being even, this one is still on the new stride
		if ( s->count == s->capacity ) {
			for ( i = 0; i < s->count / 2; i++ ) {
				s->values[i] = s->values[i * 2];
			}
			s->count /= 2;
			s->stride *= 2;
		}
		s->values[s->count++] = v;
	}
	s->seen++;
	s->total += MAX( value, 0 );
	s->max = MAX( s->max, value );
}

static int CompareInts( const void *a, const void *b ) {
	int x = *(const int *)a, y = *(const int *)b;

	return ( x > y ) - ( x < y );
}

qboolean UISpeeds_Summarize( const uisSamples_t *s, int *scratch, uisSummary_t *out ) {
	int n = s->count;

	Com_Memset( out, 0, sizeof( *out ) );
	if ( !n ) {
		return qfalse;
	}
	Com_Memcpy( scratch, s->values, n * sizeof( *scratch ) );
	qsort( scratch, n, sizeof( *scratch ), CompareInts );

	out->count = s->seen;
	out->median = n & 1 ? scratch[n / 2] : ( scratch[n / 2 - 1] + (double)scratch[n / 2] ) / 2;
	out->p95 = scratch[( (int64_t)n * 95 + 99 ) / 100 - 1];
	out->mean = (double)s->total / s->seen;
	out->max = s->max;
	return qtrue;
}

int64_t UISpeeds_Corrected( int64_t span, int reads, double clockCost ) {
	int64_t corrected = span - (int64_t)( reads * clockCost + 0.5 );

	return MAX( corrected, 0 );
}
