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
// call into the engine say, so a quit a hook asks for doesn't call back
// into it
static int	hooks_depth[MAX_ENGINE_EXTENSIONS];
// the print hooks are running, so what they print isn't passed to them
static qboolean	hooks_printing;
// an active extension wants the modules' renderer calls
static qboolean	hooks_wantModuleRender;

/*
===============
Hooks_CheckModuleCalls

Every extension's calls for the game modules, whether it starts or not:
each number in the range for extensions, used once, each key once, with
moduleCall to answer them
===============
*/
static void Hooks_CheckModuleCalls( void ) {
	const extModuleCall_t	*call, *other;
	int						i, j;

	for ( i = 0; engineExtensions[i]; i++ ) {
		for ( call = engineExtensions[i]->moduleCalls; call && call->key; call++ ) {
			if ( !engineExtensions[i]->moduleCall ) {
				Com_Error( ERR_FATAL, "Engine extension %s declares %s but has no moduleCall",
					engineExtensions[i]->name, call->key );
			}
			if ( call->trap < COM_TRAP_EXTENSION_FIRST || call->trap > COM_TRAP_EXTENSION_LAST ) {
				Com_Error( ERR_FATAL, "Engine extension %s numbers %s %i, outside %i to %i",
					engineExtensions[i]->name, call->key, call->trap, COM_TRAP_EXTENSION_FIRST,
					COM_TRAP_EXTENSION_LAST );
			}
			if ( !call->modules || ( call->modules & ~( ( 1 << EXT_NUM_MODULES ) - 1 ) ) ) {
				Com_Error( ERR_FATAL, "Engine extension %s gives %s no modules, or unknown ones",
					engineExtensions[i]->name, call->key );
			}
			// against every call before it, its own extension's too
			for ( j = 0; j <= i; j++ ) {
				for ( other = engineExtensions[j]->moduleCalls; other && other->key && other != call; other++ ) {
					if ( other->trap == call->trap || !Q_stricmp( other->key, call->key ) ) {
						Com_Error( ERR_FATAL, "Engine extensions %s and %s both declare %s %i",
							engineExtensions[j]->name, engineExtensions[i]->name, call->key, call->trap );
					}
				}
			}
		}
	}
}

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
	Hooks_CheckModuleCalls();
	for ( i = 0; engineExtensions[i]; i++ ) {
		hooks_active[i] = engineExtensions[i]->init ? engineExtensions[i]->init() : qtrue;
		if ( hooks_active[i] ) {
			hooks_activeCount++;
			if ( engineExtensions[i]->moduleRender ) {
				hooks_wantModuleRender = qtrue;
			}
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
	Com_Memset( hooks_depth, 0, sizeof( hooks_depth ) );
	hooks_printing = qfalse;
}

/*
===============
Hooks_Signal

A crash stops the program: no extension gets more calls, since its heap
may be broken. A stop that was asked for quits from the main loop, which
calls them as the quit command does
===============
*/
void Hooks_Signal( void ) {
	hooks_activeCount = 0;
	hooks_wantModuleRender = qfalse;
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
	hooks_wantModuleRender = qfalse;
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

void Hooks_GameFact( const char *fact, const char *info ) {
	HOOKS_CALL( gameFact, ( fact, info ) );
}

void Hooks_ClientCommand( int clientNum, const char *text ) {
	HOOKS_CALL( clientCommand, ( clientNum, text ) );
}

void Hooks_ClientCommandDone( int clientNum ) {
	HOOKS_CALL( clientCommandDone, ( clientNum ) );
}

void Hooks_ServerCommand( int clientNum, const char *text ) {
	HOOKS_CALL( serverCommand, ( clientNum, text ) );
}

void Hooks_ClientStateChanged( connstate_t state ) {
	HOOKS_CALL( clientStateChanged, ( state ) );
}

void Hooks_DemoPlayback( qboolean playing ) {
	HOOKS_CALL( demoPlayback, ( playing ) );
}

// not what a print hook prints, which would call it again
void Hooks_Print( const char *text ) {
	if ( hooks_activeCount && !hooks_printing ) {
		hooks_printing = qtrue;
		HOOKS_CALL( print, ( text ) );
		hooks_printing = qfalse;
	}
}

void Hooks_RendererStarted( void ) {
	HOOKS_CALL( rendererStarted, () );
}

void Hooks_RendererStopping( qboolean destroyWindow ) {
	HOOKS_CALL( rendererStopping, ( destroyWindow ) );
}

void Hooks_Draw( extDrawPoint_t point, int stereo ) {
	HOOKS_CALL( draw, ( point, stereo ) );
}

// offers an event to each active extension that has hook, with its
// arguments in parentheses, until one takes it, whose number from 1 goes in
// taker; 0 when none does
#define HOOKS_OFFER( taker, hook, args ) do { \
		int	hook_i; \
		taker = 0; \
		if ( hooks_activeCount ) { \
			for ( hook_i = 0; engineExtensions[hook_i] && !taker; hook_i++ ) { \
				if ( hooks_active[hook_i] && engineExtensions[hook_i]->hook ) { \
					hooks_depth[hook_i]++; \
					if ( engineExtensions[hook_i]->hook args ) { \
						taker = hook_i + 1; \
					} \
					hooks_depth[hook_i]--; \
				} \
			} \
		} \
	} while ( 0 )

int Hooks_KeyDown( int key, unsigned time ) {
	int	taker;

	HOOKS_OFFER( taker, keyEvent, ( key, qtrue, time ) );
	return taker;
}

void Hooks_KeyTo( int taker, int key, qboolean down, unsigned time ) {
	int	i = taker - 1;

	if ( hooks_activeCount && i >= 0 && hooks_active[i] ) {
		hooks_depth[i]++;
		engineExtensions[i]->keyEvent( key, down, time );
		hooks_depth[i]--;
	}
}

qboolean Hooks_CharEvent( int codepoint ) {
	int	taker;

	HOOKS_OFFER( taker, charEvent, ( codepoint ) );
	return taker != 0;
}

void Hooks_TextEditing( const char *utf8, int cursor, int selection ) {
	int	taker;

	HOOKS_OFFER( taker, textEditing, ( utf8, cursor, selection ) );
}

void Hooks_PointerEvent( float x, float y ) {
	int	taker;

	HOOKS_OFFER( taker, pointerEvent, ( x, y ) );
}

qboolean Hooks_MouseEvent( int dx, int dy ) {
	int	taker;

	HOOKS_OFFER( taker, mouseEvent, ( dx, dy ) );
	return taker != 0;
}

/*
===============
Hooks_FindModuleCall

An active extension's call for module: by key, or with key NULL by
number; NULL for none. The extension's index goes in extension, if given
===============
*/
static const extModuleCall_t *Hooks_FindModuleCall( extModule_t module, const char *key, int trap, int *extension ) {
	const extModuleCall_t	*call;
	int						i;

	if ( !hooks_activeCount ) {
		return NULL;
	}
	for ( i = 0; engineExtensions[i]; i++ ) {
		if ( !hooks_active[i] ) {
			continue;
		}
		for ( call = engineExtensions[i]->moduleCalls; call && call->key; call++ ) {
			if ( ( call->modules & EXT_MODULE_BIT( module ) ) &&
				( key ? !Q_stricmp( key, call->key ) : call->trap == trap ) ) {
				if ( extension ) {
					*extension = i;
				}
				return call;
			}
		}
	}
	return NULL;
}

qboolean Hooks_ModuleValue( extModule_t module, const char *key, char *value, int valueSize ) {
	const extModuleCall_t	*call = Hooks_FindModuleCall( module, key, 0, NULL );

	if ( !call ) {
		return qfalse;
	}
	Com_sprintf( value, valueSize, "%i", call->trap );
	return qtrue;
}

qboolean Hooks_ModuleCall( extModule_t module, intptr_t *args, intptr_t *result ) {
	int						extension;
	const extModuleCall_t	*call;

	if ( args[0] < COM_TRAP_EXTENSION_FIRST || args[0] > COM_TRAP_EXTENSION_LAST ) {
		return qfalse;
	}
	call = Hooks_FindModuleCall( module, NULL, args[0], &extension );
	if ( !call ) {
		return qfalse;
	}
	hooks_depth[extension]++;
	*result = engineExtensions[extension]->moduleCall( module, call->trap, args );
	hooks_depth[extension]--;
	return qtrue;
}

// any linked extension's, so an error names the call even after a module
// called one no extension answers
const char *Hooks_ModuleCallKey( int trap ) {
	const extModuleCall_t	*call;
	int						i;

	for ( i = 0; engineExtensions[i]; i++ ) {
		for ( call = engineExtensions[i]->moduleCalls; call && call->key; call++ ) {
			if ( call->trap == trap ) {
				return call->key;
			}
		}
	}
	return NULL;
}

void Hooks_ModuleShutdown( extModule_t module ) {
	HOOKS_CALL( moduleShutdown, ( module ) );
}

qboolean Hooks_WantModuleRender( void ) {
	return hooks_wantModuleRender;
}

void Hooks_ModuleAsset( extModule_t module, extAssetKind_t kind, const char *name, qhandle_t handle,
	const fontInfo_t *font ) {
	HOOKS_CALL( moduleAsset, ( module, kind, name ? name : "", handle, font ) );
}

qboolean Hooks_ModuleRender( extModule_t module, const struct extRenderCall_s *call ) {
	int	taker;

	HOOKS_OFFER( taker, moduleRender, ( module, call ) );
	return taker != 0;
}

void Hooks_ModuleRenderSeen( extModule_t module, const struct extRenderCall_s *call ) {
	HOOKS_CALL( moduleRender, ( module, call ) );
}
