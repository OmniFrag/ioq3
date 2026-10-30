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
// com_pacing.c -- the client's frame cap as an interval, and the start of
// frames synced to the display's blanks, apart from the clock and the
// display they're read from

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
Com_MovesByFrame

Whether a server moves the player by the frame, so that the frame rate
sets how high a jump goes, from its systeminfo's pmove_fixed ("" where it
sends none, as stock's and most mods' game code) and its serverinfo's
gamename. CPMA has movement of its own that doesn't follow the frame rate
in any of its rulesets (47.27 units a jump at 60, 125 and 165 fps), and
sends no pmove_fixed
=================
*/
qboolean Com_MovesByFrame( const char *pmoveFixed, const char *gamename ) {
	return !atoi( pmoveFixed ) && Q_stricmp( gamename, "cpma" );
}

/*
=================
Com_HoldDropsVsync

Whether vsync is off for the display's two holding 125 (Com_MaxFpsInterval),
on any display (Joe, 2026-10-29). Faster than 125 Hz, vsync would show each
frame for one refresh or two, unevenly ("tearing is tolerable, judders are
not"); at 125 Hz or under it would hold the frames to the refresh rate,
and the jumps with them, where the hold is for the classic height. The
tear moves, as 125 isn't the refresh rate
=================
*/
qboolean Com_HoldDropsVsync( int maxfps, qboolean movesByFrame ) {
	return maxfps < 0 && movesByFrame;
}

/*
=================
Com_MaxFpsInterval

com_maxfps's cap as an interval in nanoseconds, given the display's refresh
period (0 if unknown): a number of frames a second (Com_CapInterval); 3%
faster than the display's refresh rate (COM_MAXFPS_DISPLAY); 3% under it
(COM_MAXFPS_BELOW_DISPLAY), for a display of variable refresh rate with
vsync; or 0, no cap. The display's two are 125 where the rate isn't known,
or where the server moves players by the frame (movesByFrame,
Com_MovesByFrame), whose jumps are the classic height at 8 ms frames.

With vsync, the display paces frames under a cap faster than it, one a
refresh. Without, frames exactly a refresh apart would each swap at the
same point of the scanout, and the tear between two frames would stay in
one band of the screen; 3% faster, it moves across the screen several
times a second (5 at 165 Hz, 1.8 at 60).
=================
*/
int64_t Com_MaxFpsInterval( int maxfps, int64_t refresh, qboolean movesByFrame ) {
	if ( maxfps > 0 ) {
		return Com_CapInterval( maxfps );
	}
	if ( maxfps == 0 ) {
		return 0;
	}
	if ( refresh <= 0 || movesByFrame ) {
		return Com_CapInterval( 125 );
	}
	if ( maxfps == COM_MAXFPS_BELOW_DISPLAY ) {
		return refresh * 100 / 97;
	}
	return refresh * 100 / 103;
}

/*
=================
Com_CompareTimes
=================
*/
static int Com_CompareTimes( const void *a, const void *b ) {
	int64_t	x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

/*
=================
Com_SyncBlankAfter

The first blank on the grid of vblank + k * period after t
=================
*/
static int64_t Com_SyncBlankAfter( int64_t vblank, int64_t period, int64_t t ) {
	int64_t	d = t - vblank;
	int64_t	k = d / period;

	// rounded down, for a t before vblank too
	if ( d < 0 && d % period ) {
		k--;
	}
	return vblank + ( k + 1 ) * period;
}

/*
=================
Com_SyncReset

Forgets the frames before, as frames start being synced
=================
*/
void Com_SyncReset( comFrameSync_t *sync ) {
	Com_Memset( sync, 0, sizeof( *sync ) );
	sync->margin = COM_SYNC_MARGIN_START;
}

/*
=================
Com_SyncCost

How long a frame takes, from its start to the end of its drawing, before
its swap (ri.FrameDrawn), as the engine plans for it: the 95th percentile
of the last COM_SYNC_COSTS frames', at most a refresh. Half a refresh
until 8 frames have been timed
=================
*/
int64_t Com_SyncCost( const comFrameSync_t *sync, int64_t period ) {
	int64_t	sorted[COM_SYNC_COSTS];

	if ( sync->numCosts < 8 ) {
		return period / 2;
	}
	Com_Memcpy( sorted, sync->costs, sync->numCosts * sizeof( sorted[0] ) );
	qsort( sorted, sync->numCosts, sizeof( sorted[0] ), Com_CompareTimes );
	return MIN( sorted[( sync->numCosts - 1 ) * 95 / 100], period );
}

/*
=================
Com_SyncNextStart

When the next frame starts: as late as it can and still be ready for the
blank it aims for, by its cost and the margin. It aims for the first blank
after now, and after the one the frame before shows on, by half a refresh,
so two frames never aim for one. A start before now is at once.

It never starts before the frame before has shown, though. Until that
frame's flip, the GPU can't draw the next into the buffer it frees (a
flip-model swap chain with two buffers, as NVIDIA's OpenGL on Windows
gives), so a frame that started earlier waits there, and that wait would
count in its cost: a cost of about a refresh, which starts the next frame
earlier still. Measured on Joe's PC (2026-10-29), frames sat in that loop
for 15 to 25 seconds at a time, each started a refresh early, and came out
of it only as the margin shrank.
=================
*/
int64_t Com_SyncNextStart( comFrameSync_t *sync, int64_t now, int64_t vblank, int64_t period ) {
	int64_t	after = now, shown = sync->target, start;

	if ( shown ) {
		after = MAX( after, shown + period / 2 );
	}
	sync->target = Com_SyncBlankAfter( vblank, period, after );

	start = sync->target - Com_SyncCost( sync, period ) - sync->margin;
	if ( shown ) {
		start = MAX( start, shown );
	}
	return start;
}

/*
=================
Com_SyncFrameDone

As a synced frame is done (when its drawing ended, before its swap:
ri.FrameDrawn), its cost is kept, and whether it was in time for its blank
sets the margin. One that missed it shows on the next: the margin
grows by COM_SYNC_MARGIN_STEP, up to half a refresh, and the next frame aims
past that blank. One in time shrinks the margin's part above
COM_SYNC_MARGIN_MIN, over COM_SYNC_MARGIN_DECAY, so that one miss holds it
up for some seconds, rather than for a few frames before the next miss.
A frame over two refreshes is a hitch, such as a map loading, which says
nothing about the frames to come: it moves neither. Returns COM_SYNC_IN_TIME,
COM_SYNC_MISSED or COM_SYNC_HITCH
=================
*/
int Com_SyncFrameDone( comFrameSync_t *sync, int64_t start, int64_t done, int64_t vblank, int64_t period ) {
	if ( done - start > 2 * period ) {
		if ( sync->target ) {
			sync->target = Com_SyncBlankAfter( vblank, period, done );
		}
		return COM_SYNC_HITCH;
	}

	sync->costs[sync->nextCost] = MIN( MAX( done - start, 0 ), period );
	sync->nextCost = ( sync->nextCost + 1 ) % COM_SYNC_COSTS;
	sync->numCosts = MIN( sync->numCosts + 1, COM_SYNC_COSTS );

	if ( !sync->target ) {
		return COM_SYNC_IN_TIME;
	}
	if ( done > sync->target ) {
		sync->margin = MIN( sync->margin + COM_SYNC_MARGIN_STEP, MAX( period / 2, COM_SYNC_MARGIN_MIN ) );
		sync->target = Com_SyncBlankAfter( vblank, period, done );
		return COM_SYNC_MISSED;
	}
	if ( sync->margin > COM_SYNC_MARGIN_MIN ) {
		sync->margin -= ( sync->margin - COM_SYNC_MARGIN_MIN ) * period / COM_SYNC_MARGIN_DECAY;
	}
	return COM_SYNC_IN_TIME;
}

/*
=================
Com_SyncFallBack

Counts a synced frame's result (Com_SyncFrameDone) toward giving up on
low latency, at now: where frames miss their blank on COM_SYNC_MISS_LIMIT
of the last COM_SYNC_MISS_WINDOW, as when a frame's work doesn't fit in a
refresh, plain vsync, which overlaps one frame's drawing with the next,
keeps the full rate where low latency would halve it. Returns qtrue as it
gives up: it tries again COM_SYNC_RETRY later (Com_SyncFallenBack), or at
the next map (Com_SyncRetry), and each time it gives up again it waits
twice as long, up to COM_SYNC_RETRY_MAX, until that long has passed since
it was last tried again without giving up. Hitches aren't counted. On Joe's PC (2026-10-29)
the demo loop's restarts missed at most 2 blanks in 128 frames
=================
*/
qboolean Com_SyncFallBack( comSyncFallback_t *fallback, int result, int64_t now ) {
	int		missed = result == COM_SYNC_MISSED;

	if ( result == COM_SYNC_HITCH ) {
		return qfalse;
	}
	// on since retryAt
	if ( now - fallback->retryAt > COM_SYNC_RETRY_MAX ) {
		fallback->wait = 0;
	}

	fallback->misses += missed - fallback->missed[fallback->nextFrame];
	fallback->missed[fallback->nextFrame] = missed;
	fallback->nextFrame = ( fallback->nextFrame + 1 ) % COM_SYNC_MISS_WINDOW;
	if ( fallback->misses < COM_SYNC_MISS_LIMIT ) {
		return qfalse;
	}

	fallback->wait = fallback->wait ? MIN( fallback->wait * 2, COM_SYNC_RETRY_MAX ) : COM_SYNC_RETRY;
	fallback->retryAt = now + fallback->wait;
	Com_Memset( fallback->missed, 0, sizeof( fallback->missed ) );
	fallback->misses = 0;
	return qtrue;
}

/*
=================
Com_SyncFallenBack

Whether low latency has given up for now (Com_SyncFallBack); once its wait
is over, it's on again
=================
*/
qboolean Com_SyncFallenBack( const comSyncFallback_t *fallback, int64_t now ) {
	return now < fallback->retryAt;
}

/*
=================
Com_SyncRetry

At a map load, at now: low latency is tried again at once, if it has given
up. The wait the next time it gives up stays doubled
=================
*/
void Com_SyncRetry( comSyncFallback_t *fallback, int64_t now ) {
	if ( now < fallback->retryAt ) {
		fallback->retryAt = now;
	}
}

/*
=================
Com_VBlankEarliest

Of the wakes from first to last (not included), the one earliest after its
blank on a grid of period through wakes[0]: how far after it, and which
blank of the grid it's for
=================
*/
static void Com_VBlankEarliest( const int64_t *wakes, int first, int last, int64_t period, int64_t *offset, int64_t *blank ) {
	int64_t	d, k;
	int		i;

	for ( i = first; i < last; i++ ) {
		d = wakes[i] - wakes[0];
		k = ( d + period / 2 ) / period;	// a wake the thread missed leaves out a blank
		if ( i == first || d - k * period < *offset ) {
			*offset = d - k * period;
			*blank = k;
		}
	}
}

/*
=================
Com_VBlankFit

A display's blanks from the times a thread woke for them, oldest first: a
wake comes after its blank, by a little or by much, never before. The
period starts as the median interval, and is corrected twice by the drift
between the earliest wakes of the first and last quarters; the blank is a
recent one, from the earliest of the last 16 wakes, so the wakes' jitter
drops out.

No clock (qfalse) with fewer than half of COM_SYNC_WAKES wakes, with the
newest more than two periods before now (the display turned off, say), or
with intervals that spread over a tenth of the period, as a display of
variable refresh rate's do when it follows the frames
=================
*/
qboolean Com_VBlankFit( const int64_t *wakes, int count, int64_t now, int64_t *vblank, int64_t *period ) {
	int64_t	intervals[COM_SYNC_WAKES];
	int64_t	p, first = 0, second = 0, kFirst = 0, kSecond = 0, best = 0, kBest = 0;
	int		i, n, pass;

	if ( count > COM_SYNC_WAKES ) {
		wakes += count - COM_SYNC_WAKES;
		count = COM_SYNC_WAKES;
	}
	if ( count < COM_SYNC_WAKES / 2 ) {
		return qfalse;
	}

	n = count - 1;
	for ( i = 0; i < n; i++ ) {
		intervals[i] = wakes[i + 1] - wakes[i];
	}
	qsort( intervals, n, sizeof( intervals[0] ), Com_CompareTimes );
	p = intervals[n / 2];
	if ( p <= 0 || intervals[n * 3 / 4] - intervals[n / 4] > p / 10 || now - wakes[count - 1] > 2 * p ) {
		return qfalse;
	}

	// the first pass's period is off by as much as the median is, which
	// can pick wakes far after their blank as the earliest; the second's
	// isn't
	for ( pass = 0; pass < 2; pass++ ) {
		Com_VBlankEarliest( wakes, 0, count / 4, p, &first, &kFirst );
		Com_VBlankEarliest( wakes, count - count / 4, count, p, &second, &kSecond );
		if ( kSecond > kFirst ) {
			p += ( second - first ) / ( kSecond - kFirst );
		}
	}

	Com_VBlankEarliest( wakes, count - 16, count, p, &best, &kBest );
	*vblank = wakes[0] + kBest * p + best;
	*period = p;
	return qtrue;
}
