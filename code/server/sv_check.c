/*
===========================================================================
Copyright (C) 2026 the ioquake3 contributors

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
// sv_check.c -- a dedicated server's --check: it runs the startup and the
// configs it's given, opening no port (NET_Config) and loading no map
// (SV_Map_f notes it here), then, as the startup finishes
// (SV_FinishStartup), reports what they did and whether the server would
// start, and exits

#include "server.h"

// the maps the startup, and then the rotation, would start
#define MAX_CHECK_MAPS	64
typedef struct {
	char		map[MAX_QPATH];
	char		origin[MAX_QPATH + 16];	// the config's line, Cmd_Origin's
	qboolean	found;
} checkMap_t;

static checkMap_t	sv_checkMaps[MAX_CHECK_MAPS];
static int			sv_numCheckMaps;
static checkMap_t	*sv_checkStart;	// the map the startup ends on, if it's there

/*
==================
SV_CheckMap

A map a map command would start, which --check notes rather than load
==================
*/
void SV_CheckMap( const char *map, qboolean found ) {
	checkMap_t	*check;

	if ( sv_numCheckMaps == MAX_CHECK_MAPS ) {
		return;
	}
	check = &sv_checkMaps[sv_numCheckMaps++];
	Q_strncpyz( check->map, map, sizeof( check->map ) );
	// the default map (SV_StartDefaultMap) has no line
	Q_strncpyz( check->origin, Cmd_Origin() ? Cmd_Origin() : "no map given", sizeof( check->origin ) );
	check->found = found;
}

/*
==================
SV_CheckRotation

Follows the rotation from the map the startup would start, as each map's
end would, running nextmap until it names no map or one already played
==================
*/
static void SV_CheckRotation( void ) {
	int	first = sv_numCheckMaps, before, i;

	// the startup's queued commands, a repeating vstr's, say, would hold
	// the rotation's behind their wait; they'd run no more anyway
	Cbuf_Clear();
	while ( sv_numCheckMaps < MAX_CHECK_MAPS && Cvar_VariableString( "nextmap" )[0] ) {
		before = sv_numCheckMaps;
		Cbuf_ExecuteText( EXEC_APPEND, "vstr nextmap\n" );
		Cbuf_Execute();
		if ( sv_numCheckMaps == before ) {
			return;	// nextmap started no map
		}
		for ( i = first - 1; i < sv_numCheckMaps - 1; i++ ) {
			if ( !Q_stricmp( sv_checkMaps[i].map, sv_checkMaps[sv_numCheckMaps - 1].map ) ) {
				return;	// back to a map already played
			}
		}
	}
}

/*
==================
SV_CheckReportMaps

The maps the startup and the rotation would play, the missing ones said
==================
*/
static void SV_CheckReportMaps( void ) {
	int	startup = sv_numCheckMaps, i;

	if ( !startup ) {
		Com_Printf( "  none\n" );
		return;
	}
	for ( i = 0; i < startup; i++ ) {
		Com_Printf( "  %-24s %s%s\n", sv_checkMaps[i].origin, sv_checkMaps[i].map,
			sv_checkMaps[i].found ? "" : ": not found" );
	}
	// the startup ends on the last map that's there: one missing after it
	// leaves it playing
	for ( i = startup - 1; i >= 0 && !sv_checkMaps[i].found; i-- ) {
	}
	if ( i < 0 ) {
		return;
	}
	sv_checkStart = &sv_checkMaps[i];

	// the rotation's maps come from vstr, with no line of their own
	SV_CheckRotation();
	if ( startup < sv_numCheckMaps ) {
		const char	*playing = sv_checkStart->map;

		// a map that isn't there leaves the one playing for another round
		Com_Printf( "  then the rotation:" );
		for ( i = startup; i < sv_numCheckMaps; i++ ) {
			if ( sv_checkMaps[i].found ) {
				playing = sv_checkMaps[i].map;
				Com_Printf( " %s", playing );
			} else {
				Com_Printf( " %s (not found: %s plays again)", sv_checkMaps[i].map, playing );
			}
		}
		Com_Printf( "\n" );
	}
}

/*
==================
SV_CheckReport

What the startup did, before the start summary: the configs it ran, what
they set and the mods
==================
*/
void SV_CheckReport( void ) {
	char		mods[MAX_STRING_CHARS];
	int			count, i;
	const char	*mod;

	Com_Printf( "\n----- " PRODUCT_NAME " --check: nothing was started -----\n" );
	if ( !Cbuf_Empty() ) {
		Com_Printf( "(commands were still queued a few seconds on: a repeating vstr, say)\n" );
	}
	Com_Printf( "Configs run, in order:\n" );
	Com_PrintConfigsRun();
	Com_Printf( "Settings they set, in order, and what set them over:\n" );
	Cvar_PrintSetLog();
	Com_Printf( "Mods:\n" );
	count = FS_GetModList( mods, sizeof( mods ) );
	for ( i = 0, mod = mods; i < count; i++ ) {
		// a mod's directory, then its description
		Com_Printf( "  %-24s %s\n", mod, mod + strlen( mod ) + 1 );
		mod += strlen( mod ) + 1;
		mod += strlen( mod ) + 1;
	}
	if ( !count ) {
		Com_Printf( "  none\n" );
	}
}

/*
==================
SV_CheckExit

After the report, the summary and the warnings: the maps, then the exit,
with the status the server would have, or 1 if it would start no map
==================
*/
void SV_CheckExit( void ) {
	// last, as following the rotation runs its commands, which could change
	// what the summary and the warnings say
	Com_Printf( "Maps:\n" );
	SV_CheckReportMaps();
	NET_ExitIfDisabled( "The server wouldn't start" );
	if ( !sv_checkStart ) {
		Com_ErrorExit( 1, "The server would start with no map: give one that's there (+map <name>, "
			"or a map line in server.cfg)." );
	}
	Com_Printf( "The server would start on %s.\n", sv_checkStart->map );
	Cbuf_ExecuteText( EXEC_NOW, "quit" );
}
