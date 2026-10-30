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
// glx_vblank.c -- when the display the game's window is on blanks, from the
// driver's own record of its blanks (GLX_OML_sync_control)
//
// glXGetSyncValuesOML gives the drawable's last blank: its count (MSC) and
// its time (UST), in microseconds of CLOCK_MONOTONIC as Mesa takes it from
// the kernel. Each call keeps a blank it hasn't seen, and Com_VBlankCounted
// takes the grid from them, refusing times that aren't the display's.
//
// Only for a GLX context under SDL's X11 driver whose driver lists the
// extension. Wayland's own surfaces, EGL, and drivers without it (Xvfb's)
// give none. Each call is a round trip to the X server, once a frame.

#include <time.h>
#include <string.h>

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL3/SDL.h"
#else
#	include <SDL3/SDL.h>
#endif

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

// GLX's types and calls, which SDL loads, without its headers
typedef void *glxDisplay_t;			// Display *
typedef unsigned long glxDrawable_t;	// GLXDrawable

typedef glxDisplay_t ( *glxGetCurrentDisplayFn_t )( void );
typedef glxDrawable_t ( *glxGetCurrentDrawableFn_t )( void );
typedef const char *( *glxQueryExtensionsStringFn_t )( glxDisplay_t display, int screen );
typedef int ( *glxGetSyncValuesFn_t )( glxDisplay_t display, glxDrawable_t drawable,
	int64_t *ust, int64_t *msc, int64_t *sbc );

// a blank after the last kept by more than this starts afresh: the clock
// wasn't asked for a while, and the display may have changed since
#define GLX_BLANKS_GAP	1000000000

static struct {
	SDL_GLContext				context;	// the context looked at, NULL for none yet
	SDL_WindowID				window;		// and its window: SDL never gives an ID twice, where
											// a context's address can come back after a vid_restart
	SDL_DisplayID				display;	// the display the blanks are kept for
	glxDrawable_t				drawable;	// and the window's drawable
	int64_t						offset;		// Sys_Nanoseconds less CLOCK_MONOTONIC's time
	int64_t						times[COM_SYNC_WAKES], counts[COM_SYNC_WAKES];	// oldest first
	int							numBlanks;

	glxGetCurrentDisplayFn_t	getCurrentDisplay;
	glxGetCurrentDrawableFn_t	getCurrentDrawable;
	glxGetSyncValuesFn_t		getSyncValues;		// NULL where the context has no record
} glx;

/*
=================
Sys_MonotonicNanoseconds
=================
*/
static int64_t Sys_MonotonicNanoseconds( void ) {
	struct timespec	monotonic;

	clock_gettime( CLOCK_MONOTONIC, &monotonic );
	return (int64_t)monotonic.tv_sec * 1000000000 + monotonic.tv_nsec;
}

/*
=================
Sys_GLXHasExtension

Whether list, space-separated, has name whole
=================
*/
static qboolean Sys_GLXHasExtension( const char *list, const char *name ) {
	size_t	length = strlen( name );

	const char	*start = list, *found;

	while ( start && ( found = strstr( start, name ) ) != NULL ) {
		if ( ( found == list || found[-1] == ' ' ) && ( found[length] == ' ' || found[length] == '\0' ) ) {
			return qtrue;
		}
		start = found + length;
	}
	return qfalse;
}

/*
=================
Sys_GLXLook

Looks at the context current on this thread: GLX's with the driver's record
of the blanks, or getSyncValues is left NULL; said once a context, for
developers
=================
*/
static void Sys_GLXLook( SDL_Window *window ) {
	const char						*driver = SDL_GetCurrentVideoDriver( );
	glxQueryExtensionsStringFn_t	queryExtensions = NULL;
	const char						*none = NULL;
	int64_t							before, after, gap = -1, now;
	int								i;

	glx.getSyncValues = NULL;

	// an EGL context's SDL_GL_GetProcAddress can hand back GL's stubs for
	// any name, so GLX's calls are looked for only without one
	if ( !driver || strcmp( driver, "x11" ) ) {
		none = "not SDL's X11 driver";
	} else if ( SDL_EGL_GetCurrentDisplay( ) ) {
		none = "an EGL context";
	} else {
		glx.getCurrentDisplay = (glxGetCurrentDisplayFn_t)SDL_GL_GetProcAddress( "glXGetCurrentDisplay" );
		glx.getCurrentDrawable = (glxGetCurrentDrawableFn_t)SDL_GL_GetProcAddress( "glXGetCurrentDrawable" );
		queryExtensions = (glxQueryExtensionsStringFn_t)SDL_GL_GetProcAddress( "glXQueryExtensionsString" );
		glx.getSyncValues = (glxGetSyncValuesFn_t)SDL_GL_GetProcAddress( "glXGetSyncValuesOML" );
		if ( !glx.getCurrentDisplay || !glx.getCurrentDrawable || !queryExtensions || !glx.getSyncValues ) {
			none = "no GLX";
		} else if ( !glx.getCurrentDisplay( ) || !Sys_GLXHasExtension( queryExtensions( glx.getCurrentDisplay( ),
				(int)SDL_GetNumberProperty( SDL_GetWindowProperties( window ), SDL_PROP_WINDOW_X11_SCREEN_NUMBER, 0 ) ),
				"GLX_OML_sync_control" ) ) {
			none = "no GLX_OML_sync_control";
		}
	}

	if ( none ) {
		glx.getSyncValues = NULL;
		Com_DPrintf( "Display blanks' clock: none (%s)\n", none );
		return;
	}
	Com_DPrintf( "Display blanks' clock: GLX_OML_sync_control\n" );

	// UST is CLOCK_MONOTONIC's, as Sys_Nanoseconds is, from its own start;
	// one that isn't fails Com_VBlankCounted's checks. Of a few reads, the
	// one least likely to have been interrupted between its clocks
	for ( i = 0; i < 4; i++ ) {
		before = Sys_MonotonicNanoseconds( );
		now = Sys_Nanoseconds( );
		after = Sys_MonotonicNanoseconds( );
		if ( gap < 0 || after - before < gap ) {
			gap = after - before;
			glx.offset = now - before - gap / 2;
		}
	}
}

/*
=================
Sys_GLXBlanks
=================
*/
int Sys_GLXBlanks( int64_t *times, int64_t *counts ) {
	SDL_GLContext	context = SDL_GL_GetCurrentContext( );
	SDL_Window		*window = SDL_GL_GetCurrentWindow( );
	SDL_WindowID	windowID = window ? SDL_GetWindowID( window ) : 0;
	SDL_DisplayID	display;
	glxDrawable_t	drawable;
	int64_t			ust, msc, sbc, time;
	int				n = glx.numBlanks;

	if ( !context || !window ) {
		return 0;
	}
	if ( context != glx.context || windowID != glx.window ) {
		glx.context = context;
		glx.window = windowID;
		Sys_GLXLook( window );
		glx.numBlanks = n = 0;
	}
	if ( !glx.getSyncValues ) {
		return 0;
	}

	// the blanks of another display, or of another drawable, aren't these
	display = SDL_GetDisplayForWindow( window );
	drawable = glx.getCurrentDrawable( );
	if ( display != glx.display || drawable != glx.drawable ) {
		glx.display = display;
		glx.drawable = drawable;
		n = 0;
	}
	if ( !drawable || !glx.getSyncValues( glx.getCurrentDisplay( ), drawable, &ust, &msc, &sbc ) ) {
		glx.numBlanks = n;
		return 0;
	}
	time = ust * 1000 + glx.offset;

	// a blank seen already is kept once; one from before the last kept, or
	// long after it, starts afresh
	if ( n && ( msc < glx.counts[n - 1] || time < glx.times[n - 1] || time - glx.times[n - 1] > GLX_BLANKS_GAP ) ) {
		n = 0;
	}
	if ( !n || msc > glx.counts[n - 1] ) {
		if ( n == COM_SYNC_WAKES ) {
			n--;
			memmove( glx.times, glx.times + 1, n * sizeof( glx.times[0] ) );
			memmove( glx.counts, glx.counts + 1, n * sizeof( glx.counts[0] ) );
		}
		glx.times[n] = time;
		glx.counts[n] = msc;
		n++;
	}
	glx.numBlanks = n;

	memcpy( times, glx.times, n * sizeof( times[0] ) );
	memcpy( counts, glx.counts, n * sizeof( counts[0] ) );
	return n;
}
