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
// hooks.c -- calls the engine extensions linked in (hooks.h)

#include "q_shared.h"
#include "qcommon.h"

// the build refuses more (engine_extensions.cmake)
#define MAX_ENGINE_EXTENSIONS	32

static qboolean	hooks_active[MAX_ENGINE_EXTENSIONS];
// 0 before init and after shutdown, when no hook runs
static int	hooks_activeCount;
static int	hooks_idleMsec = -1;
// 1 + the index of the extension whose frame runs, so a signal's exit
// doesn't call back into code it interrupted
static volatile int	hooks_inFrame;

/*
===============
Hooks_Init
===============
*/
void Hooks_Init( void ) {
	int	i;

	if ( engineExtensionsLine[0] ) {
		Com_Printf( "%s\n", engineExtensionsLine );
	}
	for ( i = 0; engineExtensions[i]; i++ ) {
		hooks_active[i] = engineExtensions[i]->init ? engineExtensions[i]->init() : qtrue;
		if ( hooks_active[i] ) {
			hooks_activeCount++;
		}
	}
	// no wait before the extensions' first frames say how long they allow
	if ( hooks_activeCount ) {
		hooks_idleMsec = 0;
	}
}

/*
===============
Hooks_Frame
===============
*/
void Hooks_Frame( void ) {
	int	i, msec;

	hooks_idleMsec = -1;
	if ( !hooks_activeCount ) {
		return;
	}
	for ( i = 0; engineExtensions[i]; i++ ) {
		if ( hooks_active[i] && engineExtensions[i]->frame ) {
			hooks_inFrame = i + 1;
			msec = engineExtensions[i]->frame();
			hooks_inFrame = 0;
			if ( msec >= 0 && ( hooks_idleMsec < 0 || msec < hooks_idleMsec ) ) {
				hooks_idleMsec = msec;
			}
		}
	}
}

/*
===============
Hooks_FrameAborted

An error unwound the frame, out of any hook it ended in
===============
*/
void Hooks_FrameAborted( void ) {
	hooks_inFrame = 0;
}

/*
===============
Hooks_Shutdown
===============
*/
void Hooks_Shutdown( const char *reason ) {
	int	i;

	if ( !hooks_activeCount ) {
		return;
	}
	hooks_activeCount = 0;
	hooks_idleMsec = -1;
	for ( i = 0; engineExtensions[i]; i++ ) {
		if ( hooks_active[i] && engineExtensions[i]->shutdown && hooks_inFrame != i + 1 ) {
			engineExtensions[i]->shutdown( reason );
		}
	}
}

/*
===============
Hooks_IdleMsec
===============
*/
int Hooks_IdleMsec( void ) {
	return hooks_idleMsec;
}
