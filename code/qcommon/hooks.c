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
// how many of each extension's hooks are running, one inside another's
// call into the engine say, so a signal's exit doesn't call back into code
// it interrupted
static volatile int	hooks_depth[MAX_ENGINE_EXTENSIONS];

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
			hooks_depth[i]++;
			msec = engineExtensions[i]->frame();
			hooks_depth[i]--;
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
	Com_Memset( (void *)hooks_depth, 0, sizeof( hooks_depth ) );
}

/*
===============
Hooks_Signal

A signal stops the program: an extension it interrupted gets no more
calls, since its state may be half changed, and after a crash, whose heap
may be broken, no extension does
===============
*/
void Hooks_Signal( qboolean crashed ) {
	int	i;

	for ( i = 0; engineExtensions[i]; i++ ) {
		if ( crashed || hooks_depth[i] ) {
			hooks_active[i] = qfalse;
		}
	}
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
		// not into one whose own hook is quitting
		if ( hooks_active[i] && engineExtensions[i]->shutdown && !hooks_depth[i] ) {
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

// calls hook, with its arguments in parentheses, on each active extension
// that has it
#define HOOKS_CALL( hook, args ) do { \
		int	hook_i; \
		if ( hooks_activeCount ) { \
			for ( hook_i = 0; engineExtensions[hook_i]; hook_i++ ) { \
				if ( hooks_active[hook_i] && engineExtensions[hook_i]->hook ) { \
					hooks_depth[hook_i]++; \
					engineExtensions[hook_i]->hook args; \
					hooks_depth[hook_i]--; \
				} \
			} \
		} \
	} while ( 0 )

void Hooks_ClientConnected( int clientNum ) {
	HOOKS_CALL( clientConnected, ( clientNum ) );
}

void Hooks_ClientBegin( int clientNum ) {
	HOOKS_CALL( clientBegin, ( clientNum ) );
}

// not for a slot nobody holds, as a dropped client's userinfo is cleared
void Hooks_ClientUserinfoChanged( int clientNum ) {
	if ( SV_ExtClientState( clientNum ) != EXT_CLIENT_FREE ) {
		HOOKS_CALL( clientUserinfoChanged, ( clientNum ) );
	}
}

// not for a slot nobody holds: a client still connecting was never
// reported connected, and one the game dropped has gone already
void Hooks_ClientDropped( int clientNum, const char *reason ) {
	if ( SV_ExtClientState( clientNum ) != EXT_CLIENT_FREE ) {
		HOOKS_CALL( clientDropped, ( clientNum, reason ) );
	}
}

void Hooks_MapSpawned( void ) {
	HOOKS_CALL( mapSpawned, () );
}

void Hooks_MapRestarted( void ) {
	HOOKS_CALL( mapRestarted, () );
}

void Hooks_ServerShutdown( const char *reason ) {
	HOOKS_CALL( serverShutdown, ( reason ) );
}

void Hooks_GameFrameDone( int levelTime ) {
	HOOKS_CALL( gameFrameDone, ( levelTime ) );
}

void Hooks_Hibernation( qboolean asleep ) {
	HOOKS_CALL( hibernation, ( asleep ) );
}
