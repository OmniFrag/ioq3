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
// win_vblank.c -- when the display the game's window is on blanks, from a
// thread that waits for each blank (D3DKMTWaitForVerticalBlankEvent)
//
// Of Windows' clocks for the display, DwmGetCompositionTimingInfo gives an
// exact grid, but only the primary display's, whatever display a window is
// on, and DWM's, which a window covering the screen skips. Polling the
// scanline (D3DKMTGetScanLine) needs the display's vertical total, which
// isn't given. A wait for each blank is per display and follows its
// scanout; it wakes 10 us to 1 ms after the blank, so the wakes are kept,
// and Com_VBlankFit takes the grid from the earliest.
//
// The thread starts at the first ask, and stops waiting a second after the
// last, while frames aren't synced to the blanks (com_lowLatencyVsync).

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

#include <windows.h>

// gdi32's calls into the display driver, as d3dkmthk.h declares them,
// which older SDKs lack
typedef UINT vblankHandle_t;	// D3DKMT_HANDLE

typedef struct {
	HDC				hDc;
	vblankHandle_t	hAdapter;
	LUID			AdapterLuid;
	UINT			VidPnSourceId;
} vblankOpenAdapter_t;			// D3DKMT_OPENADAPTERFROMHDC

typedef struct {
	vblankHandle_t	hAdapter;
} vblankCloseAdapter_t;			// D3DKMT_CLOSEADAPTER

typedef struct {
	vblankHandle_t	hAdapter;
	vblankHandle_t	hDevice;
	UINT			VidPnSourceId;
} vblankWait_t;					// D3DKMT_WAITFORVERTICALBLANKEVENT

typedef LONG ( APIENTRY *vblankOpenAdapterFn_t )( vblankOpenAdapter_t *open );
typedef LONG ( APIENTRY *vblankCloseAdapterFn_t )( const vblankCloseAdapter_t *close );
typedef LONG ( APIENTRY *vblankWaitFn_t )( const vblankWait_t *wait );

static struct {
	int						state;		// 0 not started yet, 1 running, -1 failed to start
	CRITICAL_SECTION		lock;		// for what follows it
	HMONITOR				monitor;	// the display the window is on
	int64_t					asked;		// when the wakes were last asked for
	int64_t					wakes[COM_SYNC_WAKES];
	int						numWakes, nextWake;

	vblankOpenAdapterFn_t	openAdapter;
	vblankCloseAdapterFn_t	closeAdapter;
	vblankWaitFn_t			wait;
} vblank;

/*
=================
Sys_VBlankOpen

The display driver's adapter and source of a display, from its device
name; qfalse if the driver won't say
=================
*/
static qboolean Sys_VBlankOpen( HMONITOR monitor, vblankWait_t *wait ) {
	MONITORINFOEXW		info;
	vblankOpenAdapter_t	open;
	HDC					dc;

	Com_Memset( wait, 0, sizeof( *wait ) );
	Com_Memset( &info, 0, sizeof( info ) );
	info.cbSize = sizeof( info );
	if ( !monitor || !GetMonitorInfoW( monitor, (MONITORINFO *)&info ) ) {
		return qfalse;
	}

	dc = CreateDCW( info.szDevice, info.szDevice, NULL, NULL );
	if ( !dc ) {
		return qfalse;
	}
	Com_Memset( &open, 0, sizeof( open ) );
	open.hDc = dc;
	if ( vblank.openAdapter( &open ) != 0 ) {
		DeleteDC( dc );
		return qfalse;
	}
	DeleteDC( dc );

	wait->hAdapter = open.hAdapter;
	wait->VidPnSourceId = open.VidPnSourceId;
	return qtrue;
}

/*
=================
Sys_VBlankClose
=================
*/
static void Sys_VBlankClose( vblankWait_t *wait ) {
	vblankCloseAdapter_t	close;

	if ( wait->hAdapter ) {
		close.hAdapter = wait->hAdapter;
		vblank.closeAdapter( &close );
		wait->hAdapter = 0;
	}
}

/*
=================
Sys_VBlankForget

Forgets the wakes, as the display changes or stops being waited for
=================
*/
static void Sys_VBlankForget( void ) {
	vblank.numWakes = 0;
	vblank.nextWake = 0;
}

/*
=================
Sys_VBlankStop

Stops waiting, and forgets the wakes
=================
*/
static void Sys_VBlankStop( vblankWait_t *wait ) {
	Sys_VBlankClose( wait );
	EnterCriticalSection( &vblank.lock );
	Sys_VBlankForget();
	LeaveCriticalSection( &vblank.lock );
}

/*
=================
Sys_VBlankThread

Waits for each blank of the display the window is on, and keeps when it
woke. A wait that fails, as for a display that's gone, tries the display
again a little later; so do waits that return at once, as some drivers'
do while a display is off.
=================
*/
static void Sys_VBlankThread( void *unused ) {
	vblankWait_t	wait;
	HMONITOR		open = NULL, monitor;
	int64_t			now, last = 0, asked;
	int				quick = 0;

	// woken sooner, it wakes nearer each blank
	SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_HIGHEST );

	Com_Memset( &wait, 0, sizeof( wait ) );
	for ( ;; ) {
		EnterCriticalSection( &vblank.lock );
		monitor = vblank.monitor;
		asked = vblank.asked;
		LeaveCriticalSection( &vblank.lock );

		// no one has asked for a second: frames aren't synced. The wakes
		// before a pause, or a failure, are forgotten, as a grid fitted
		// across the gap would be off
		if ( Sys_Nanoseconds() - asked > 1000000000 ) {
			if ( open ) {
				Sys_VBlankStop( &wait );
				open = NULL;
			}
			Sleep( 100 );
			continue;
		}

		if ( monitor != open || !wait.hAdapter ) {
			Sys_VBlankClose( &wait );
			open = monitor;
			if ( !Sys_VBlankOpen( monitor, &wait ) ) {
				Sleep( 1000 );
				continue;
			}
		}

		if ( vblank.wait( &wait ) != 0 ) {
			Sys_VBlankStop( &wait );
			Sleep( 100 );
			continue;
		}
		now = Sys_Nanoseconds();
		quick = now - last < 1000000 ? quick + 1 : 0;
		last = now;

		if ( quick > 8 ) {
			Sys_VBlankStop( &wait );
			quick = 0;
			Sleep( 100 );
			continue;
		}

		EnterCriticalSection( &vblank.lock );
		if ( vblank.monitor == open ) {
			vblank.wakes[vblank.nextWake] = now;
			vblank.nextWake = ( vblank.nextWake + 1 ) % COM_SYNC_WAKES;
			vblank.numWakes = MIN( vblank.numWakes + 1, COM_SYNC_WAKES );
		}
		LeaveCriticalSection( &vblank.lock );
	}
}

/*
=================
Sys_VBlankStart

Finds gdi32's calls and starts the thread, once; qfalse where either
isn't there
=================
*/
static qboolean Sys_VBlankStart( void ) {
	HMODULE	gdi;

	if ( vblank.state ) {
		return vblank.state > 0;
	}
	vblank.state = -1;

	gdi = GetModuleHandleW( L"gdi32.dll" );
	if ( !gdi ) {
		return qfalse;
	}
	vblank.openAdapter = (vblankOpenAdapterFn_t)GetProcAddress( gdi, "D3DKMTOpenAdapterFromHdc" );
	vblank.closeAdapter = (vblankCloseAdapterFn_t)GetProcAddress( gdi, "D3DKMTCloseAdapter" );
	vblank.wait = (vblankWaitFn_t)GetProcAddress( gdi, "D3DKMTWaitForVerticalBlankEvent" );
	if ( !vblank.openAdapter || !vblank.closeAdapter || !vblank.wait ) {
		return qfalse;
	}

	InitializeCriticalSection( &vblank.lock );
	vblank.asked = Sys_Nanoseconds();
	if ( !Sys_StartThread( Sys_VBlankThread, NULL ) ) {
		DeleteCriticalSection( &vblank.lock );
		return qfalse;
	}

	vblank.state = 1;
	return qtrue;
}

/*
=================
Sys_VBlankWakes

When the thread woke for the last blanks of the display window (an HWND) is
on, oldest first, up to COM_SYNC_WAKES of them; the number given. 0 until
it has waited for some, or where the system has no such waits
=================
*/
int Sys_VBlankWakes( void *window, int64_t *wakes ) {
	HMONITOR	monitor = window ? MonitorFromWindow( (HWND)window, MONITOR_DEFAULTTONEAREST ) : NULL;
	int			count, first, i;

	if ( !Sys_VBlankStart() ) {
		return 0;
	}

	EnterCriticalSection( &vblank.lock );
	vblank.asked = Sys_Nanoseconds();
	if ( monitor != vblank.monitor ) {
		vblank.monitor = monitor;
		Sys_VBlankForget();
	}
	count = vblank.numWakes;
	first = vblank.nextWake - count + COM_SYNC_WAKES;
	for ( i = 0; i < count; i++ ) {
		wakes[i] = vblank.wakes[( first + i ) % COM_SYNC_WAKES];
	}
	LeaveCriticalSection( &vblank.lock );

	return count;
}
