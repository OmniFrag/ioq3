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
// sv_dlcc.c -- a download's congestion controller
//
// Slow start until the queue passes half the target, then a window that
// grows while the path shows little queue and shrinks past the target, the
// target being a few milliseconds above the least RTT seen. A loss cuts it
// once a round trip where there's a queue, or where many are lost. Every 4
// seconds, on the server's clock, it halves for a round trip so that the
// queue empties and the least RTT stays the path's. Paced at a little over
// a window a round trip

#include "sv_dlcc.h"

#define DLCC_SECOND				1000000
#define DLCC_INIT_CWND			10		// datagrams
#define DLCC_MIN_CWND			2
#define DLCC_MAX_CWND			( 4 * 1024 * 1024 )
#define DLCC_MAX_PACKETS		4000	// the server's ring holds 4,096, probes past the window too
#define DLCC_PREFETCH_CWND		( 64 * 1024 )
#define DLCC_TARGET_LOADING		4000
#define DLCC_TARGET_PREFETCH	2000
#define DLCC_MIN_RTT_BUCKET		( 2 * DLCC_SECOND )
#define DLCC_DRAIN_INTERVAL		( 4 * DLCC_SECOND )
#define DLCC_LOSS_HEAVY			20		// 1 in this many bytes lost is congestion
#define DLCC_SS_EXIT_SAMPLES	8
#define DLCC_PTO_RESET			3		// probe timeouts in a row
#define DLCC_BURST_TIME			2000	// what the pacers may send at once
#define DLCC_INITIAL_RTT		333000	// RFC 9002's, for the probe timeout before a sample
#define DLCC_PTO_MIN			50000
#define DLCC_PTO_MAX			( 2 * DLCC_SECOND )
#define DLCC_LOADING_FLOOR		( 100 * 1024 )	// bytes a second: the classic download's default budget
#define DLCC_STUCK				DLCC_SECOND		// at the floor past 4 targets this long, or 8 RTTs

static int64_t DLCC_Target( const dlcc_t *cc ) {
	return cc->dlClass == DLCC_LOADING ? DLCC_TARGET_LOADING : DLCC_TARGET_PREFETCH;
}

static int64_t DLCC_MinCwnd( const dlcc_t *cc ) {
	return (int64_t)DLCC_MIN_CWND * cc->mss;
}

// the most the window holds: its class's, and the client's receive window
static int64_t DLCC_MaxCwnd( const dlcc_t *cc ) {
	int64_t	most = cc->dlClass == DLCC_PREFETCH ? DLCC_PREFETCH_CWND :
		MIN( DLCC_MAX_CWND, (int64_t)DLCC_MAX_PACKETS * cc->mss );

	if ( cc->receiveWindow > 0 ) {
		most = MIN( most, cc->receiveWindow );
	}
	return MAX( most, DLCC_MinCwnd( cc ) );
}

static int64_t DLCC_Clamp( int64_t value, int64_t low, int64_t high ) {
	return value < low ? low : value > high ? high : value;
}

// the smoothed RTT, at least a millisecond, to divide by
static int64_t DLCC_Rtt( const dlcc_t *cc ) {
	return MAX( cc->srtt, 1000 );
}

// the least the window holds: 2 datagrams, and while a client loads, its
// floor rate over the least RTT, which a loss-based flow on the same link
// mustn't push it under. Over the least RTT, not the smoothed one, which
// holds the queue the floor itself would build on a path slower than it.
// Against a loss-based flow, the least RTT comes to hold that flow's queue
// (the stuck rule in DLCC_OnAck). Prefetch yields entirely
static int64_t DLCC_FloorCwnd( const dlcc_t *cc ) {
	int64_t	lowest = DLCC_MinCwnd( cc );

	if ( cc->dlClass == DLCC_LOADING ) {
		lowest = MAX( lowest, cc->floorRate * MAX( DLCC_MinRtt( cc ), 1000 ) / DLCC_SECOND );
	}
	return MIN( lowest, DLCC_MaxCwnd( cc ) );
}

// the window between its floor and its ceiling. A probe timeout's reset
// and the drain go under the floor, on purpose
static void DLCC_FitWindow( dlcc_t *cc ) {
	cc->cwnd = DLCC_Clamp( cc->cwnd, DLCC_FloorCwnd( cc ), DLCC_MaxCwnd( cc ) );
}

/*
=================
DLCC_MinRtt

The least RTT over the last buckets, or 0 before a sample
=================
*/
int64_t DLCC_MinRtt( const dlcc_t *cc ) {
	int64_t	least = 0;
	int		i;

	for ( i = 0; i < DLCC_MIN_RTT_BUCKETS; i++ ) {
		if ( cc->minRtt[i] && ( !least || cc->minRtt[i] < least ) ) {
			least = cc->minRtt[i];
		}
	}
	return least;
}

// forgets the path's RTT: the least, and the recent samples measured
// against it, so slow start runs its samples again
static void DLCC_ResetMinRtt( dlcc_t *cc ) {
	Com_Memset( cc->minRtt, 0, sizeof( cc->minRtt ) );
	cc->minRttBucketEnd = 0;
	cc->samples = 0;
}

/*
=================
DLCC_Init

A controller for a client's downloads, sending datagrams of mss bytes
=================
*/
void DLCC_Init( dlcc_t *cc, dlccClass_t dlClass, int mss, int64_t now ) {
	Com_Memset( cc, 0, sizeof( *cc ) );
	cc->dlClass = dlClass;
	cc->mss = mss;
	cc->cwnd = MIN( (int64_t)DLCC_INIT_CWND * mss, DLCC_MaxCwnd( cc ) );
	cc->slowStart = qtrue;
	cc->recoveryStart = INT64_MIN;
	cc->pacer.time = now;
	cc->pacer.tokens = (int64_t)4 * mss * DLCC_SECOND;
	cc->drainPeriod = now / DLCC_DRAIN_INTERVAL;
	cc->floorRate = DLCC_LOADING_FLOOR;
}

/*
=================
DLCC_SetClass

A client that starts playing goes on in the background, within prefetch's
window and rate; one loading again may grow past them
=================
*/
void DLCC_SetClass( dlcc_t *cc, dlccClass_t dlClass ) {
	cc->dlClass = dlClass;
	DLCC_FitWindow( cc );
}

/*
=================
DLCC_SetFloorRate

The least a loading client gets, in bytes a second, however a loss-based
flow on its path presses it; 0 for none. The classic download's budget is
shared by everyone downloading, so the server may share this one too
=================
*/
void DLCC_SetFloorRate( dlcc_t *cc, int64_t rate ) {
	cc->floorRate = rate;
}

/*
=================
DLCC_SetReceiveWindow

The most bytes the client says it can take in flight, 0 for no limit
=================
*/
void DLCC_SetReceiveWindow( dlcc_t *cc, int64_t bytes ) {
	cc->receiveWindow = bytes;
	DLCC_FitWindow( cc );
}

/*
=================
DLCC_OnRttSample

A round trip, without the client's ack delay
=================
*/
void DLCC_OnRttSample( dlcc_t *cc, int64_t now, int64_t rtt ) {
	int64_t	*bucket, least = DLCC_MinRtt( cc );

	if ( rtt <= 0 ) {
		rtt = 1;
	}
	cc->latestRtt = rtt;
	if ( !cc->srtt ) {
		cc->srtt = rtt;
		cc->rttvar = rtt / 2;
	} else {
		cc->rttvar = ( 3 * cc->rttvar + llabs( cc->srtt - rtt ) ) / 4;
		cc->srtt = ( 7 * cc->srtt + rtt ) / 8;
	}

	// a new bucket every 2 s, the oldest forgotten: every one has seen a
	// drain (DLCC_SendWait), and so the path's own RTT
	if ( !cc->minRttBucketEnd || now - cc->minRttBucketEnd >= DLCC_MIN_RTT_BUCKETS * DLCC_MIN_RTT_BUCKET ) {
		// first, or after an idle that outlasted them all
		Com_Memset( cc->minRtt, 0, sizeof( cc->minRtt ) );
		cc->minRttBucketEnd = now + DLCC_MIN_RTT_BUCKET;
	}
	while ( now >= cc->minRttBucketEnd ) {
		cc->minRttBucket = ( cc->minRttBucket + 1 ) % DLCC_MIN_RTT_BUCKETS;
		cc->minRtt[cc->minRttBucket] = 0;
		cc->minRttBucketEnd += DLCC_MIN_RTT_BUCKET;
	}
	bucket = &cc->minRtt[cc->minRttBucket];
	if ( !*bucket || rtt < *bucket ) {
		*bucket = rtt;
	}
	// a path longer by more than the target: start again from it
	if ( least && DLCC_MinRtt( cc ) > least + DLCC_Target( cc ) ) {
		cc->slowStart = qtrue;
	}

	cc->recent[cc->samples % DLCC_FILTER] = rtt;
	cc->samples++;
}

/*
=================
DLCC_QueuingDelay

The least of the recent samples over the least RTT: how long the queue is
=================
*/
static int64_t DLCC_QueuingDelay( const dlcc_t *cc ) {
	int64_t	least = cc->recent[0], minRtt = DLCC_MinRtt( cc );
	int		i, count = MIN( cc->samples, DLCC_FILTER );

	// no least RTT yet, or none since a reset: no queue known
	if ( !count || !minRtt ) {
		return 0;
	}
	for ( i = 1; i < count; i++ ) {
		least = MIN( least, cc->recent[i] );
	}
	return MAX( least - minRtt, 0 );
}

/*
=================
DLCC_OnAck

An ack's bytes newly acknowledged, and the bytes it showed lost with the
time the latest of those was sent (when lostBytes is 0, any)
=================
*/
void DLCC_OnAck( dlcc_t *cc, int64_t now, int ackedBytes, int lostBytes, int64_t newestLostSent ) {
	int64_t	target = DLCC_Target( cc ), queuing = DLCC_QueuingDelay( cc );
	int64_t	lowest = DLCC_FloorCwnd( cc ), rtt, step, change;
	int64_t	lost, all;

	cc->bytesInFlight = MAX( cc->bytesInFlight - ackedBytes - lostBytes, 0 );
	cc->ptoCount = 0;

	// this round trip's and the last one's acked and lost bytes
	if ( now - cc->roundStart >= DLCC_Rtt( cc ) ) {
		cc->lastAcked = cc->roundAcked;
		cc->lastLost = cc->roundLost;
		cc->roundAcked = cc->roundLost = 0;
		cc->roundStart = now;
	}
	cc->roundAcked += ackedBytes;
	cc->roundLost += lostBytes;

	// a loss cuts the window once a round trip, where it's congestion: with
	// a queue, or many lost, as at a policer. One with the queue short is
	// the path's own (radio, a busy hop), which a cut can't help
	lost = cc->roundLost + cc->lastLost;
	all = lost + cc->roundAcked + cc->lastAcked;
	if ( lostBytes > 0 && newestLostSent > cc->recoveryStart &&
		( queuing >= target / 2 || lost * DLCC_LOSS_HEAVY > all ) ) {
		cc->recoveryStart = now;
		cc->cwnd = MAX( lowest, cc->cwnd * 7 / 10 );
		cc->slowStart = qfalse;
		return;
	}
	// loading, a queue past 4 targets that holds the window at its floor for
	// a while isn't this download's: the path got longer (a new route), or
	// its least RTT is another flow's queue. Measure it again. The floor
	// moves with the smoothed RTT, by up to an eighth a sample, so a window
	// fitted to it an ack ago may be a little over it now
	if ( cc->dlClass != DLCC_LOADING || cc->slowStart || queuing <= 4 * target || cc->cwnd > lowest + lowest / 4 ) {
		cc->stuckSince = 0;
	} else if ( !cc->stuckSince ) {
		cc->stuckSince = now;
	} else if ( now - cc->stuckSince >= MIN( DLCC_STUCK, 8 * DLCC_Rtt( cc ) ) ) {
		DLCC_ResetMinRtt( cc );
		cc->slowStart = qtrue;
		cc->stuckSince = 0;
		return;
	}

	if ( ackedBytes <= 0 || cc->appLimited ) {
		return;
	}

	if ( cc->slowStart ) {
		if ( cc->samples >= DLCC_SS_EXIT_SAMPLES && queuing > target / 2 ) {
			cc->slowStart = qfalse;
		} else {
			cc->cwnd += ackedBytes;
		}
	} else {
		// near the target, a few datagrams a round trip, the same for every
		// download through the same queue, while the cut past it is in
		// proportion to the window: the larger shrinks the more, so
		// downloads sharing a link come to share it alike. Well under the
		// target, the path has room: the window grows by a part of the gap,
		// (target - queuing) / srtt of itself in a round trip, at most 25%,
		// as what it adds shows as queue only a round trip later. Past the
		// target, it shrinks by the whole excess so, at most by half
		rtt = DLCC_Rtt( cc );
		step = DLCC_Clamp( cc->cwnd * target / ( 4 * rtt ), cc->mss, 4 * cc->mss );
		if ( queuing > target ) {
			change = MAX( ( target - queuing ) * ackedBytes / rtt, -ackedBytes / 2 );
		} else if ( queuing >= target / 4 ) {
			change = step * ackedBytes / cc->cwnd;
		} else {
			change = MIN( ( target - queuing ) * ackedBytes / ( 2 * rtt ), ackedBytes / 4 );
			change = MAX( change, step * ackedBytes / cc->cwnd );
		}
		cc->cwnd += change;
	}
	DLCC_FitWindow( cc );
}

/*
=================
DLCC_OnPto

A probe timeout: nothing acked for a while. The third in a row starts
over, as the path may have changed
=================
*/
void DLCC_OnPto( dlcc_t *cc ) {
	if ( ++cc->ptoCount >= DLCC_PTO_RESET ) {
		cc->cwnd = DLCC_MinCwnd( cc );		// under the floor: the path may be gone
		cc->slowStart = qtrue;
		DLCC_ResetMinRtt( cc );
	}
}

/*
=================
DLCC_ProbeTimeout

How long with nothing acked before the oldest datagrams go again: the
smoothed RTT + max(4 x its variance, 1 ms) + the longest ack delay, at
least 50 ms, doubling with each timeout in a row up to 2 s
=================
*/
int64_t DLCC_ProbeTimeout( const dlcc_t *cc ) {
	int64_t	srtt = cc->srtt ? cc->srtt : DLCC_INITIAL_RTT;
	int64_t	rttvar = cc->srtt ? cc->rttvar : DLCC_INITIAL_RTT / 2;
	int64_t	pto = MAX( srtt + MAX( 4 * rttvar, 1000 ) + DLCC_ACK_DELAY_MAX, DLCC_PTO_MIN );

	return MIN( pto << MIN( cc->ptoCount, 6 ), DLCC_PTO_MAX );
}

/*
=================
DLCC_LossDelay

A datagram sent this long before an ack, and not acked, is lost: 9/8 of
the longer of the smoothed and the latest RTT, at least 1 ms, and of the
initial RTT before a sample (RFC 9002's threshold)
=================
*/
int64_t DLCC_LossDelay( const dlcc_t *cc ) {
	int64_t	rtt = cc->srtt ? MAX( cc->srtt, cc->latestRtt ) : DLCC_INITIAL_RTT;

	return MAX( rtt * 9 / 8, 1000 );
}

/*
=================
DLCC_PacingRate

Bytes a second: a little over a window a round trip, twice that in slow
start
=================
*/
int64_t DLCC_PacingRate( const dlcc_t *cc ) {
	int64_t	rate = cc->cwnd * DLCC_SECOND / DLCC_Rtt( cc );

	rate = cc->slowStart ? rate * 2 : rate * 5 / 4;
	if ( cc->dlClass == DLCC_PREFETCH ) {
		rate = MIN( rate, DLCC_PREFETCH_RATE );
	}
	return MAX( rate, 1 );
}

/*
=================
DLCC_Refill

Fills a bucket for the time since it was last filled, up to burst bytes,
and returns the microseconds until it holds bytes, 0 if it does. A second's
filling fills any burst, and keeps the sums in range after a long idle
=================
*/
static int64_t DLCC_Refill( dlccBucket_t *bucket, int64_t now, int64_t burst, int bytes ) {
	int64_t	missing;

	if ( now > bucket->time ) {
		bucket->tokens = MIN( bucket->tokens + bucket->rate * MIN( now - bucket->time, DLCC_SECOND ),
			burst * DLCC_SECOND );
		bucket->time = now;
	}
	missing = (int64_t)bytes * DLCC_SECOND - bucket->tokens;
	if ( missing <= 0 ) {
		return 0;
	}
	return ( missing + bucket->rate - 1 ) / bucket->rate;
}

/*
=================
DLCC_SendWait

Whether a datagram of bytes may go now: 0 if so, -1 if the window is full
(an ack opens it), else the microseconds until the pacer allows it. now is
the server's clock, the same for every client
=================
*/
int64_t DLCC_SendWait( dlcc_t *cc, int64_t now, int bytes ) {
	int64_t	window = cc->cwnd, period = now / DLCC_DRAIN_INTERVAL;

	// for a round trip every few seconds, half the window, which empties
	// the queue: the RTTs then seen are the path's own, which keeps the
	// least RTT current and lets it follow a path that changes. On a grid
	// of the server's time, so that all of its downloads drain at once and
	// none mistakes another's queue for its path, however late it's asked
	if ( period != cc->drainPeriod ) {
		cc->drainPeriod = period;
		if ( !cc->slowStart && cc->srtt ) {
			cc->drainEnd = period * DLCC_DRAIN_INTERVAL + cc->srtt;
		}
	}
	if ( now < cc->drainEnd ) {
		window /= 2;
	}
	if ( cc->bytesInFlight + bytes > window ) {
		return -1;
	}

	// what may go at once: 2 ms at the rate, at least 4 datagrams
	cc->pacer.rate = DLCC_PacingRate( cc );
	return DLCC_Refill( &cc->pacer, now, MAX( (int64_t)4 * cc->mss,
		cc->pacer.rate * DLCC_BURST_TIME / DLCC_SECOND ), bytes );
}

/*
=================
DLCC_OnSend

A datagram sent, of bytes; moreWaiting if the client has more that only
this controller holds back. One held by sv_dlMaxRate's bucket isn't: the
window then stops growing past what the cap lets it use
=================
*/
void DLCC_OnSend( dlcc_t *cc, int bytes, qboolean moreWaiting ) {
	cc->pacer.tokens -= (int64_t)bytes * DLCC_SECOND;
	cc->bytesInFlight += bytes;
	cc->appLimited = !moreWaiting && cc->bytesInFlight < cc->cwnd / 2;
}

/*
=================
DLCC_BucketInit

A token bucket of rate bytes a second (0 for no limit) shared by several
controllers, as sv_dlMaxRate; it bursts 2 ms of its rate
=================
*/
void DLCC_BucketInit( dlccBucket_t *bucket, int64_t rate, int64_t now ) {
	bucket->rate = rate;
	bucket->tokens = rate * DLCC_BURST_TIME;
	bucket->time = now;
}

/*
=================
DLCC_BucketWait

0 if a datagram of bytes fits the bucket now, else the microseconds until
it does
=================
*/
int64_t DLCC_BucketWait( dlccBucket_t *bucket, int64_t now, int bytes ) {
	if ( bucket->rate <= 0 ) {
		return 0;
	}
	// at least one datagram, at a rate too low for one in 2 ms
	return DLCC_Refill( bucket, now, MAX( bucket->rate * DLCC_BURST_TIME / DLCC_SECOND, bytes ), bytes );
}

void DLCC_BucketTake( dlccBucket_t *bucket, int bytes ) {
	if ( bucket->rate > 0 ) {
		bucket->tokens -= (int64_t)bytes * DLCC_SECOND;
	}
}
