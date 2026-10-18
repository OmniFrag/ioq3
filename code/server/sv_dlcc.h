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
// sv_dlcc.h -- a download's congestion controller: a delay-based window
// that keeps the queue it builds a few milliseconds long, so other
// players' snapshots through the same link aren't held up behind it.
// Pure functions of the events given them; times are in microseconds.
//
// The caller, for each ack, gives the RTT sample (if any) before the ack;
// counts in ackedBytes only datagrams it hasn't already given as lost (their
// bytes left the flight then); and passes every datagram it sends, the
// probe timeout's included, to DLCC_OnSend. mss must be positive

#ifndef SV_DLCC_H
#define SV_DLCC_H

#include "../qcommon/q_shared.h"

#define DLCC_FILTER			4	// RTT samples whose least is the current delay
#define DLCC_MIN_RTT_BUCKETS	4	// the minimum RTT is the least over these
#define DLCC_PREFETCH_RATE		( 128 * 1024 )	// bytes a second
#define DLCC_ACK_DELAY_MAX		25000	// a longer ack delay gives no RTT sample

typedef enum {
	DLCC_LOADING,		// a client not yet in the game
	DLCC_PREFETCH		// a client playing, fetching in the background
} dlccClass_t;

// a token bucket: the pacer's, and a rate shared by several controllers, as
// sv_dlMaxRate's
typedef struct {
	int64_t		rate;				// bytes a second, 0 for none
	int64_t		tokens;				// millionths of a byte
	int64_t		time;
} dlccBucket_t;

typedef struct {
	dlccClass_t	dlClass;
	int			mss;				// a datagram's bytes; windows count these
	int64_t		cwnd;				// bytes
	qboolean	slowStart;
	int64_t		recoveryStart;		// when the last loss cut the window
	int64_t		srtt, rttvar;		// 0 before the first sample
	int64_t		latestRtt;
	int64_t		minRtt[DLCC_MIN_RTT_BUCKETS];	// each bucket's least, 0 if empty
	int			minRttBucket;
	int64_t		minRttBucketEnd;
	int64_t		recent[DLCC_FILTER];
	int			samples;
	int64_t		bytesInFlight;
	qboolean	appLimited;
	int			ptoCount;
	int64_t		roundStart;			// this round trip's acked and lost bytes
	int64_t		roundAcked, roundLost;
	int64_t		lastAcked, lastLost;	// the last round trip's
	int64_t		drainPeriod, drainEnd;
	int64_t		stuckSince;			// at the floor past 4 targets since, or 0
	int64_t		receiveWindow;		// the client's, 0 for none
	int64_t		floorRate;			// loading's least, bytes a second
	dlccBucket_t	pacer;
} dlcc_t;

void	DLCC_Init( dlcc_t *cc, dlccClass_t dlClass, int mss, int64_t now );
void	DLCC_SetClass( dlcc_t *cc, dlccClass_t dlClass );
void	DLCC_SetReceiveWindow( dlcc_t *cc, int64_t bytes );
void	DLCC_SetFloorRate( dlcc_t *cc, int64_t rate );
void	DLCC_OnRttSample( dlcc_t *cc, int64_t now, int64_t rtt );
void	DLCC_OnAck( dlcc_t *cc, int64_t now, int ackedBytes, int lostBytes, int64_t newestLostSent );
void	DLCC_OnPto( dlcc_t *cc );
int64_t	DLCC_ProbeTimeout( const dlcc_t *cc );
int64_t	DLCC_LossDelay( const dlcc_t *cc );
int64_t	DLCC_SendWait( dlcc_t *cc, int64_t now, int bytes );
void	DLCC_OnSend( dlcc_t *cc, int bytes, qboolean moreWaiting );
int64_t	DLCC_PacingRate( const dlcc_t *cc );
int64_t	DLCC_MinRtt( const dlcc_t *cc );

void	DLCC_BucketInit( dlccBucket_t *bucket, int64_t rate, int64_t now );
int64_t	DLCC_BucketWait( dlccBucket_t *bucket, int64_t now, int bytes );
void	DLCC_BucketTake( dlccBucket_t *bucket, int bytes );

#endif
