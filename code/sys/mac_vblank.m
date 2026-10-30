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
// mac_vblank.m -- when the display the game's window is on blanks, from a
// display link (CVDisplayLink)
//
// A display link calls back on a thread of its own once a refresh, with
// the time the frame it asks for will show: a blank a refresh or two
// ahead. Those times are on the display's own grid, where the callback's
// wake isn't: on an M4 Mac mini at 144 Hz (2026-10-29), the output times
// were 6948.7 us apart throughout, while the callbacks woke up to 1.4 ms
// either side of their turn. So the blanks themselves are kept, and
// Com_VBlankFit fits them as it fits Windows' wakes, none of them late.
// CAMetalDisplayLink needs a Metal layer, which an OpenGL window hasn't.
//
// The link starts at the first ask, follows the display the window is on,
// and stops a second after the last ask (VBLANK_IDLE), while frames aren't synced to the
// blanks (com_lowLatencyVsync). Starting, stopping and moving it run on a
// queue of their own, as the callback can't stop its own link.

#ifndef __APPLE__
#error This file is for macOS only.
#endif

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>

// how long after the last ask the link stops
#define VBLANK_IDLE		1000000000LL

static struct {
	int					state;		// 0 not started yet, 1 running, -1 failed to start
	pthread_mutex_t		lock;		// for what follows it
	CGDirectDisplayID	display;	// the display the window is on, 0 for none
	int64_t				asked;		// when the blanks were last asked for
	qboolean			reconciling;	// a Sys_VBlankReconcile is queued
	int64_t				blanks[COM_SYNC_WAKES];
	int					numBlanks, nextBlank;
	uint64_t			lastOutput;	// the host time of the newest blank kept

	CVDisplayLinkRef	link;
	CGDirectDisplayID	linkDisplay;	// the display the link follows, on the queue
	dispatch_queue_t	queue;
	mach_timebase_info_data_t	timebase;
} vblank;

/*
=================
Sys_VBlankForget

Forgets the blanks, as the display changes or stops being asked for
=================
*/
static void Sys_VBlankForget( void ) {
	vblank.numBlanks = 0;
	vblank.nextBlank = 0;
	vblank.lastOutput = 0;
}

/*
=================
Sys_VBlankAsked

Whether the blanks were asked for within VBLANK_IDLE of now; called with
the lock held
=================
*/
static qboolean Sys_VBlankAsked( int64_t now ) {
	return now - vblank.asked <= VBLANK_IDLE;
}

/*
=================
Sys_VBlankReconcile

On the queue: runs the link for the display asked for, or stops it when
none is, or none has been for a while. A link stopped or moved forgets its
blanks, once it no longer calls back, so no other display's are kept
=================
*/
static void Sys_VBlankReconcile( void *unused ) {
	CGDirectDisplayID	display;
	qboolean			wanted;

	pthread_mutex_lock( &vblank.lock );
	vblank.reconciling = qfalse;
	display = vblank.display;
	wanted = display && Sys_VBlankAsked( Sys_Nanoseconds() );
	pthread_mutex_unlock( &vblank.lock );

	if ( !wanted || display != vblank.linkDisplay ) {
		if ( CVDisplayLinkIsRunning( vblank.link ) ) {
			CVDisplayLinkStop( vblank.link );
		}
		vblank.linkDisplay = 0;
		pthread_mutex_lock( &vblank.lock );
		Sys_VBlankForget();
		pthread_mutex_unlock( &vblank.lock );
	}
	if ( wanted && !vblank.linkDisplay ) {
		if ( CVDisplayLinkSetCurrentCGDisplay( vblank.link, display ) == kCVReturnSuccess &&
			CVDisplayLinkStart( vblank.link ) == kCVReturnSuccess ) {
			vblank.linkDisplay = display;
		} else {
			// the next ask tries again, as for a new display
			pthread_mutex_lock( &vblank.lock );
			if ( vblank.display == display ) {
				vblank.display = 0;
			}
			pthread_mutex_unlock( &vblank.lock );
		}
	}
}

/*
=================
Sys_VBlankQueueReconcile

Queues a Sys_VBlankReconcile, unless one is already; called with the lock
held
=================
*/
static void Sys_VBlankQueueReconcile( void ) {
	if ( !vblank.reconciling ) {
		vblank.reconciling = qtrue;
		dispatch_async_f( vblank.queue, NULL, Sys_VBlankReconcile );
	}
}

/*
=================
Sys_VBlankOutput

The link's callback: keeps the blank its output time is, on
Sys_Nanoseconds' clock (CLOCK_MONOTONIC, which runs on through sleep,
where the host time doesn't, so it's converted by the difference to now)
=================
*/
static CVReturn Sys_VBlankOutput( CVDisplayLinkRef link, const CVTimeStamp *now,
	const CVTimeStamp *output, CVOptionFlags flags, CVOptionFlags *flagsOut, void *context ) {
	uint64_t	host = mach_absolute_time();
	int64_t		ns = Sys_Nanoseconds(), ahead, blank;

	if ( !( output->flags & kCVTimeStampHostTimeValid ) ) {
		return kCVReturnSuccess;
	}
	ahead = (int64_t)( output->hostTime - host );
	blank = ns + ahead * vblank.timebase.numer / vblank.timebase.denom;

	pthread_mutex_lock( &vblank.lock );
	if ( !Sys_VBlankAsked( ns ) ) {
		// frames aren't synced: the link stops
		Sys_VBlankQueueReconcile();
	} else if ( CVDisplayLinkGetCurrentCGDisplay( link ) != vblank.display ) {
		// the window has moved on, and the link follows it once it stops
	} else if ( output->hostTime > vblank.lastOutput ) {
		vblank.lastOutput = output->hostTime;
		vblank.blanks[vblank.nextBlank] = blank;
		vblank.nextBlank = ( vblank.nextBlank + 1 ) % COM_SYNC_WAKES;
		vblank.numBlanks = MIN( vblank.numBlanks + 1, COM_SYNC_WAKES );
	}
	pthread_mutex_unlock( &vblank.lock );

	return kCVReturnSuccess;
}

/*
=================
Sys_VBlankStart

Makes the link and its queue, once; qfalse where the system won't
=================
*/
static qboolean Sys_VBlankStart( void ) {
	if ( vblank.state ) {
		return vblank.state > 0;
	}
	vblank.state = -1;

	if ( mach_timebase_info( &vblank.timebase ) != KERN_SUCCESS || !vblank.timebase.denom ) {
		return qfalse;
	}
	if ( CVDisplayLinkCreateWithActiveCGDisplays( &vblank.link ) != kCVReturnSuccess ) {
		return qfalse;
	}
	if ( CVDisplayLinkSetOutputCallback( vblank.link, Sys_VBlankOutput, NULL ) != kCVReturnSuccess ) {
		CVDisplayLinkRelease( vblank.link );
		return qfalse;
	}
	vblank.queue = dispatch_queue_create( "vblank", DISPATCH_QUEUE_SERIAL );
	pthread_mutex_init( &vblank.lock, NULL );

	vblank.state = 1;
	return qtrue;
}

/*
=================
Sys_VBlankWakes

The last blanks of the display window (an NSWindow) is on, oldest first,
up to COM_SYNC_WAKES of them; the number given. 0 until the link has
called back for some, or where there's no link or display. Called on the
main thread, as the window is AppKit's
=================
*/
int Sys_VBlankWakes( void *window, int64_t *wakes ) {
	CGDirectDisplayID	display = 0;
	int64_t				now;
	int					count, first, i;

	if ( !Sys_VBlankStart() ) {
		return 0;
	}

	if ( window ) {
		@autoreleasepool {
			NSScreen	*screen = [(__bridge NSWindow *)window screen];
			NSNumber	*number = screen ? screen.deviceDescription[@"NSScreenNumber"] : nil;

			display = number ? (CGDirectDisplayID)number.unsignedIntValue : 0;
		}
	}

	now = Sys_Nanoseconds();
	pthread_mutex_lock( &vblank.lock );
	if ( display != vblank.display || !Sys_VBlankAsked( now ) ) {
		// another display's blanks aren't this one's
		if ( display != vblank.display ) {
			Sys_VBlankForget();
		}
		vblank.display = display;
		Sys_VBlankQueueReconcile();
	}
	vblank.asked = now;
	count = vblank.numBlanks;
	first = vblank.nextBlank - count + COM_SYNC_WAKES;
	for ( i = 0; i < count; i++ ) {
		wakes[i] = vblank.blanks[( first + i ) % COM_SYNC_WAKES];
	}
	pthread_mutex_unlock( &vblank.lock );

	return count;
}
