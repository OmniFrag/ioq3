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
// cl_demoinfo.c -- what a demo file holds, for a demo browser
// (CL_ExtDemoInfo): its map, server, game type, recorder and players from
// its first gamestate, read as it's first asked for, then its length, its
// players at the end and the configstrings a caller names, from a walk
// through its messages a slice a frame (CL_DemoInfoFrame), so a folder of
// big demos is read a little at a time. Results are kept by name, size
// and date.

#include <time.h>

#include "client.h"

#define DEMOINFO_CACHE			256
#define DEMOINFO_RECHECK_MSEC	1000				// between looks at a kept demo's file
#define DEMOINFO_IDLE_MSEC		1000				// with nothing asked for that long, no walk
#define DEMOINFO_SLICE_MSEC		2					// a frame's walking
#define DEMOINFO_SLICE_BYTES	( 1024 * 1024 )		// and its most, whatever the clock says
#define DEMOINFO_MAX_BYTES		( 64 * 1024 * 1024 )	// a demo's walk; past it the length is unknown
#define DEMOINFO_FIRST_MESSAGES	4					// that may come before the gamestate

typedef struct {
	char		name[MAX_QPATH];
	int			used;					// when last asked, for the cache's order
	int			checked;				// when its file was last looked at
	int64_t		statSize;				// FS_LastFileStat's: the file's, or its pak's
	int			numAsked, asked[MAX_DEMOINFO_CONFIGSTRINGS];	// the configstrings the walk keeps
	int			commandSequence;		// the gamestate's: the walk takes only later commands
	demoInfo_t	info;
} demoInfoEntry_t;

static demoInfoEntry_t	di_cache[DEMOINFO_CACHE];
static int				di_clock;
static int				di_asked;		// when a demo was last asked for
static byte				di_data[MAX_MSGLEN];

// the walk under way
static struct {
	demoInfoEntry_t	*entry;
	fileHandle_t	f;
	int				read;				// its bytes so far
	qboolean		timed;				// firstTime is the first snapshot's server time
	int				firstTime;
	int				lastCommand;		// the last server command taken
} di_walk;

// a demo's protocol from its name's extension, which the client must play
// (FS_IsDemoExt): 0 for any other
static int CL_DemoProtocol( const char *name ) {
	if ( !FS_IsDemoExt( name, strlen( name ) ) ) {
		return 0;
	}
	return atoi( strrchr( name, '.' ) + ARRAY_LEN( DEMOEXT ) );
}

/*
===============
CL_DemoReadMessage

A demo's next message, as CL_ReadDemoMessage reads it: a sequence number,
a length and the message, ready to parse; qfalse at its end, or where a
length is out of range, which damaged says
===============
*/
static qboolean CL_DemoReadMessage( fileHandle_t f, msg_t *msg, int *read, qboolean *damaged ) {
	int		sequence, length;

	MSG_Init( msg, di_data, sizeof( di_data ) );
	if ( FS_Read( &sequence, 4, f ) != 4 || FS_Read( &length, 4, f ) != 4 ) {
		return qfalse;
	}
	length = LittleLong( length );
	if ( length == -1 ) {
		return qfalse;
	}
	if ( length < 0 || length > msg->maxsize || FS_Read( msg->data, length, f ) != length ) {
		*damaged = qtrue;
		return qfalse;
	}
	msg->cursize = length;
	*read += 8 + length;
	MSG_Bitstream( msg );
	MSG_ReadLong( msg );		// the reliable acknowledge
	return qtrue;
}

// a player's configstring: "" for a slot nobody holds
static void CL_DemoPlayer( demoInfo_t *info, int client, const char *cs ) {
	demoPlayer_t	*player = &info->player[client];

	if ( !cs[0] ) {
		Com_Memset( player, 0, sizeof( *player ) );
		return;
	}
	Q_strncpyz( player->name, Info_ValueForKey( cs, "n" ), sizeof( player->name ) );
	player->team = atoi( Info_ValueForKey( cs, "t" ) );
	player->bot = Info_ValueForKey( cs, "skill" )[0] != '\0';
}

// a configstring the walk keeps: a player's, or one the caller named
static void CL_DemoConfigString( demoInfoEntry_t *entry, int index, const char *value ) {
	int		i;

	if ( index >= CS_PLAYERS && index < CS_PLAYERS + MAX_CLIENTS ) {
		CL_DemoPlayer( &entry->info, index - CS_PLAYERS, value );
	}
	for ( i = 0; i < entry->numAsked; i++ ) {
		if ( entry->asked[i] == index ) {
			Q_strncpyz( entry->info.configstrings[i], value, sizeof( entry->info.configstrings[i] ) );
		}
	}
}

// the players and bots counted
static void CL_DemoCountPlayers( demoInfo_t *info ) {
	int		i;

	info->players = info->bots = 0;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( info->player[i].name[0] ) {
			if ( info->player[i].bot ) {
				info->bots++;
			} else {
				info->players++;
			}
		}
	}
}

/*
===============
CL_DemoCommand

A server command the walk takes: a configstring changed, "cs <index>
<value>", as CL_ConfigstringModified reads it. bcs, a value too long for
a command, carries none the walk keeps
===============
*/
static void CL_DemoCommand( demoInfoEntry_t *entry, const char *s ) {
	char	*text = (char *)s;
	int		index;

	if ( strcmp( COM_Parse( &text ), "cs" ) ) {
		return;
	}
	index = atoi( COM_Parse( &text ) );
	if ( index >= 0 && index < MAX_CONFIGSTRINGS ) {
		CL_DemoConfigString( entry, index, COM_Parse( &text ) );
	}
}

/*
===============
CL_DemoCommands

A message's server commands, up to what follows them, which is returned:
taken by the walk's entry, only those past the last, as
CL_ParseCommandString takes a server's that come again until they're
acknowledged; passed over with none
===============
*/
static int CL_DemoCommands( msg_t *msg, demoInfoEntry_t *entry ) {
	const char	*s;
	int			cmd, sequence;

	while ( ( cmd = MSG_ReadByte( msg ) ) == svc_nop || cmd == svc_serverCommand ) {
		if ( cmd == svc_serverCommand ) {
			sequence = MSG_ReadLong( msg );
			s = MSG_ReadString( msg );
			if ( entry && sequence > di_walk.lastCommand ) {
				di_walk.lastCommand = sequence;
				CL_DemoCommand( entry, s );
			}
		}
	}
	return cmd;
}

/*
===============
CL_DemoGamestate

The first gamestate, read with CL_ParseGamestate's bounds: the fields the
browser shows at once. A recording starts with it (CL_Record_f), so only
the first messages are looked in. A baseline's field count is checked
before MSG_ReadDeltaEntity, which would drop the game for one out of
range. qfalse for a demo without one there, or a damaged one
===============
*/
static qboolean CL_DemoGamestate( demoInfoEntry_t *entry, fileHandle_t f ) {
	static gameState_t	gs;
	msg_t			msg;
	entityState_t	nullstate, baseline;
	demoInfo_t		*info = &entry->info;
	const char		*serverinfo, *s;
	int				cmd, i, len, number, read = 0, messages;
	qboolean		damaged = qfalse;

	for ( messages = 0; messages < DEMOINFO_FIRST_MESSAGES && CL_DemoReadMessage( f, &msg, &read, &damaged );
		messages++ ) {
		if ( CL_DemoCommands( &msg, NULL ) != svc_gamestate ) {
			continue;
		}
		Com_Memset( &gs, 0, sizeof( gs ) );
		gs.dataCount = 1;
		entry->commandSequence = MSG_ReadLong( &msg );
		while ( ( cmd = MSG_ReadByte( &msg ) ) != svc_EOF ) {
			if ( cmd == svc_configstring ) {
				i = MSG_ReadShort( &msg );
				if ( i < 0 || i >= MAX_CONFIGSTRINGS ) {
					return qfalse;
				}
				s = MSG_ReadBigString( &msg );
				len = strlen( s );
				if ( len + 1 + gs.dataCount > MAX_GAMESTATE_CHARS ) {
					return qfalse;
				}
				gs.stringOffsets[i] = gs.dataCount;
				Com_Memcpy( gs.stringData + gs.dataCount, s, len + 1 );
				gs.dataCount += len + 1;
			} else if ( cmd == svc_baseline ) {
				number = MSG_ReadBits( &msg, GENTITYNUM_BITS );
				if ( number < 0 || number >= MAX_GENTITIES || !MSG_DeltaEntityValid( &msg ) ) {
					return qfalse;
				}
				Com_Memset( &nullstate, 0, sizeof( nullstate ) );
				MSG_ReadDeltaEntity( &msg, &nullstate, &baseline, number );
			} else if ( cmd == svc_ofManifest ) {
				byte	manifest[SHA256_DIGEST_SIZE];

				MSG_ReadData( &msg, manifest, sizeof( manifest ) );
			} else {
				// the message's end too (-1)
				return qfalse;
			}
		}
		info->clientNum = MSG_ReadLong( &msg );
		if ( msg.readcount > msg.cursize ) {
			return qfalse;
		}
		serverinfo = gs.stringData + gs.stringOffsets[CS_SERVERINFO];
		Q_strncpyz( info->map, Info_ValueForKey( serverinfo, "mapname" ), sizeof( info->map ) );
		Q_strncpyz( info->hostname, Info_ValueForKey( serverinfo, "sv_hostname" ), sizeof( info->hostname ) );
		info->gametype = atoi( Info_ValueForKey( serverinfo, "g_gametype" ) );
		for ( i = 0; i < MAX_CONFIGSTRINGS; i++ ) {
			CL_DemoConfigString( entry, i, gs.stringData + gs.stringOffsets[i] );
		}
		if ( info->clientNum >= 0 && info->clientNum < MAX_CLIENTS ) {
			Q_strncpyz( info->recorder, info->player[info->clientNum].name, sizeof( info->recorder ) );
		}
		CL_DemoCountPlayers( info );
		return qtrue;
	}
	return qfalse;
}

// a walk stopped, for its entry's slot going to another, or for nothing
// asked for a while
static void CL_DemoWalkStop( demoInfoEntry_t *entry ) {
	if ( di_walk.entry == entry ) {
		if ( di_walk.f ) {
			FS_FCloseFile( di_walk.f );
		}
		Com_Memset( &di_walk, 0, sizeof( di_walk ) );
	}
}

// the walk at its end: its players counted, its file closed
static void CL_DemoWalkDone( qboolean lengthKnown ) {
	demoInfo_t	*info = &di_walk.entry->info;

	CL_DemoCountPlayers( info );
	if ( !lengthKnown ) {
		info->lengthMsec = -1;
	}
	info->state = DEMOINFO_DONE;
	CL_DemoWalkStop( di_walk.entry );
}

/*
===============
CL_DemoInfoFrame

A slice of the walk under way, or of the next demo asked for: each
message's server commands and its snapshot's server time, as long as the
frame's budget lasts. Nothing once no demo was asked for a while: a list
shows them no more
===============
*/
void CL_DemoInfoFrame( void ) {
	msg_t		msg;
	demoInfo_t	*info;
	int			start = Sys_Milliseconds(), sliceStart, i, time;
	int64_t		length;
	qboolean	damaged = qfalse;

	// its file closed while it waits, to be read again from its start
	if ( start - di_asked > DEMOINFO_IDLE_MSEC ) {
		if ( di_walk.entry ) {
			CL_DemoWalkStop( di_walk.entry );
		}
		return;
	}
	if ( !di_walk.entry ) {
		// the demo asked for last that waits: one a list shows now, before
		// one it scrolled past
		for ( i = 0; i < DEMOINFO_CACHE; i++ ) {
			if ( di_cache[i].info.state == DEMOINFO_WALKING &&
				( !di_walk.entry || di_cache[i].used > di_walk.entry->used ) ) {
				di_walk.entry = &di_cache[i];
			}
		}
		if ( !di_walk.entry ) {
			return;
		}
		// read again from its start: the gamestate's message is passed over
		FS_FOpenFileRead( di_walk.entry->name, &di_walk.f, qtrue );
		if ( !di_walk.f ) {
			CL_DemoWalkDone( qfalse );
			return;
		}
		di_walk.lastCommand = di_walk.entry->commandSequence;
	}
	info = &di_walk.entry->info;
	sliceStart = di_walk.read;
	while ( di_walk.read - sliceStart < DEMOINFO_SLICE_BYTES && Sys_Milliseconds() - start < DEMOINFO_SLICE_MSEC ) {
		if ( di_walk.read >= DEMOINFO_MAX_BYTES ) {
			CL_DemoWalkDone( qfalse );
			return;
		}
		if ( !CL_DemoReadMessage( di_walk.f, &msg, &di_walk.read, &damaged ) ) {
			info->damaged = damaged;
			CL_DemoWalkDone( qtrue );
			return;
		}
		// the rest of the message is the snapshot's, from its server time
		if ( CL_DemoCommands( &msg, di_walk.entry ) == svc_snapshot ) {
			time = MSG_ReadLong( &msg );
			if ( !di_walk.timed ) {
				di_walk.timed = qtrue;
				di_walk.firstTime = time;
			}
			// a damaged demo's times can be any
			length = (int64_t)time - di_walk.firstTime;
			if ( length > info->lengthMsec ) {
				info->lengthMsec = (int)MIN( length, INT_MAX );
			}
		}
	}
}

// a date in local time, "2026-10-27 13:05"; "" for none
static void CL_DemoDate( int mtime, char *date, int size ) {
	time_t		t = mtime;
	struct tm	*local = mtime > 0 ? localtime( &t ) : NULL;

	if ( !local || !strftime( date, size, "%Y-%m-%d %H:%M", local ) ) {
		date[0] = '\0';
	}
}

/*
===============
CL_ExtDemoInfo

What a demo holds (hooks.h). Its file is looked at again at most once a
second, and read again when its size or date changed
===============
*/
demoInfoState_t CL_ExtDemoInfo( const char *name, const int *configstrings, int numConfigstrings, demoInfo_t *info ) {
	demoInfoEntry_t	*entry = NULL, *oldest = &di_cache[0];
	fileHandle_t	f;
	int64_t			size = -1, mtime = 0;
	long			length;
	int				i, protocol, now = Sys_Milliseconds();

	Com_Memset( info, 0, sizeof( *info ) );
	info->state = DEMOINFO_REFUSED;
	numConfigstrings = Com_Clamp( 0, MAX_DEMOINFO_CONFIGSTRINGS, numConfigstrings );
	protocol = CL_DemoProtocol( name );
	if ( !protocol || Q_stricmpn( name, "demos/", 6 ) || strlen( name ) >= MAX_QPATH ) {
		return info->state;
	}
	di_asked = now;
	for ( i = 0; i < DEMOINFO_CACHE; i++ ) {
		if ( !Q_stricmp( di_cache[i].name, name ) && di_cache[i].numAsked == numConfigstrings &&
			( !numConfigstrings || !memcmp( di_cache[i].asked, configstrings, numConfigstrings * sizeof( *configstrings ) ) ) ) {
			entry = &di_cache[i];
			break;
		}
		if ( di_cache[i].used < oldest->used ) {
			oldest = &di_cache[i];
		}
	}
	if ( !entry || now - entry->checked >= DEMOINFO_RECHECK_MSEC ) {
		// the engine's own path rules (FS_FOpenFileReadDir's)
		length = FS_FOpenFileRead( name, &f, qtrue );
		if ( f && !FS_LastFileStat( name, &size, &mtime ) ) {
			size = length;
		}
		// the date as an int, as the modules read it, until 2038
		mtime = mtime > 0 && mtime <= INT_MAX ? mtime : 0;
		if ( !entry || entry->statSize != size || entry->info.mtime != mtime ) {
			if ( !entry ) {
				entry = oldest;
			}
			CL_DemoWalkStop( entry );
			Com_Memset( entry, 0, sizeof( *entry ) );
			Q_strncpyz( entry->name, name, sizeof( entry->name ) );
			entry->statSize = size;
			entry->numAsked = numConfigstrings;
			if ( numConfigstrings ) {
				Com_Memcpy( entry->asked, configstrings, numConfigstrings * sizeof( *configstrings ) );
			}
			entry->info.state = DEMOINFO_REFUSED;
			entry->info.protocol = protocol;
			entry->info.size = (int)length;
			entry->info.mtime = (int)mtime;
			CL_DemoDate( entry->info.mtime, entry->info.date, sizeof( entry->info.date ) );
			entry->info.lengthMsec = -1;
			if ( f && CL_DemoGamestate( entry, f ) ) {
				entry->info.state = DEMOINFO_WALKING;
				entry->info.lengthMsec = 0;
			}
		}
		if ( f ) {
			FS_FCloseFile( f );
		}
		entry->checked = now;
	}
	entry->used = ++di_clock;
	*info = entry->info;
	return info->state;
}

/*
===============
CL_DemoInfo_f

demoinfo <demos/name.dm_NN> [<configstring index>...]: what the demo
browser reads of a demo, its walk run to the end at once
===============
*/
void CL_DemoInfo_f( void ) {
	static demoInfo_t	info;
	int					configstrings[MAX_DEMOINFO_CONFIGSTRINGS], count, i, start;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: demoinfo <demos/name.dm_NN> [<configstring index>...]\n" );
		return;
	}
	count = MIN( Cmd_Argc() - 2, MAX_DEMOINFO_CONFIGSTRINGS );
	for ( i = 0; i < count; i++ ) {
		configstrings[i] = atoi( Cmd_Argv( i + 2 ) );
	}
	start = Sys_Milliseconds();
	CL_ExtDemoInfo( Cmd_Argv( 1 ), configstrings, count, &info );
	while ( info.state == DEMOINFO_WALKING ) {
		CL_DemoInfoFrame();
		CL_ExtDemoInfo( Cmd_Argv( 1 ), configstrings, count, &info );
	}
	if ( info.state == DEMOINFO_REFUSED ) {
		Com_Printf( "%s: refused\n", Cmd_Argv( 1 ) );
		return;
	}
	Com_Printf( "%s: protocol %d, %d bytes, map %s, server \"%s\", game type %d, recorded by \"%s\" (client %d), "
		"%d players and %d bots, length %d msec%s\n", Cmd_Argv( 1 ), info.protocol, info.size, info.map,
		info.hostname, info.gametype, info.recorder, info.clientNum, info.players, info.bots, info.lengthMsec,
		info.damaged ? ", damaged" : "" );
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( info.player[i].name[0] ) {
			Com_Printf( "  player %d: \"%s\", team %d%s\n", i, info.player[i].name, info.player[i].team,
				info.player[i].bot ? ", bot" : "" );
		}
	}
	for ( i = 0; i < count; i++ ) {
		Com_Printf( "  configstring %d: \"%s\"\n", configstrings[i], info.configstrings[i] );
	}
	// read again only if it changed, so a second demoinfo times the lookup
	Com_Printf( "  read in %d msec\n", Sys_Milliseconds() - start );
}
