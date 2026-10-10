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

#include "server.h"


/*
===============
SV_SendConfigstring

Creates and sends the server command necessary to update the CS index for the
given client
===============
*/
static void SV_SendConfigstring(client_t *client, int index)
{
	int maxChunkSize = MAX_STRING_CHARS - 24;
	int len;

	len = strlen(sv.configstrings[index]);

	if( len >= maxChunkSize ) {
		int		sent = 0;
		int		remaining = len;
		char	*cmd;
		char	buf[MAX_STRING_CHARS];

		while (remaining > 0 ) {
			if ( sent == 0 ) {
				cmd = "bcs0";
			}
			else if( remaining < maxChunkSize ) {
				cmd = "bcs2";
			}
			else {
				cmd = "bcs1";
			}
			Q_strncpyz( buf, &sv.configstrings[index][sent],
				maxChunkSize );

			SV_SendServerCommand( client, "%s %i \"%s\"\n", cmd,
				index, buf );

			sent += (maxChunkSize - 1);
			remaining -= (maxChunkSize - 1);
		}
	} else {
		// standard cs, just send it
		SV_SendServerCommand( client, "cs %i \"%s\"\n", index,
			sv.configstrings[index] );
	}
}

/*
===============
SV_UpdateConfigstrings

Called when a client goes from CS_PRIMED to CS_ACTIVE.  Updates all
Configstring indexes that have changed while the client was in CS_PRIMED
===============
*/
void SV_UpdateConfigstrings(client_t *client)
{
	int index;

	for( index = 0; index < MAX_CONFIGSTRINGS; index++ ) {
		// if the CS hasn't changed since we went to CS_PRIMED, ignore
		if(!client->csUpdated[index])
			continue;

		// do not always send server info to all clients
		if ( index == CS_SERVERINFO && client->gentity &&
			(client->gentity->r.svFlags & SVF_NOSERVERINFO) ) {
			continue;
		}
		SV_SendConfigstring(client, index);
		client->csUpdated[index] = qfalse;
	}
}

/*
===============
SV_SetConfigstring

===============
*/
void SV_SetConfigstring (int index, const char *val) {
	int		i;
	client_t	*client;

	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error (ERR_DROP, "SV_SetConfigstring: bad index %i", index);
	}

	if ( !val ) {
		val = "";
	}

	// don't bother broadcasting an update if no change
	if ( !strcmp( val, sv.configstrings[ index ] ) ) {
		return;
	}

	// change the string in sv
	Z_Free( sv.configstrings[index] );
	sv.configstrings[index] = CopyString( val );

	// send it to all the clients if we aren't
	// spawning a new server
	if ( sv.state == SS_GAME || sv.restarting ) {

		// send the data to all relevant clients
		for (i = 0, client = svs.clients; i < sv_maxclients->integer ; i++, client++) {
			if ( client->state < CS_ACTIVE ) {
				if ( client->state == CS_PRIMED )
					client->csUpdated[ index ] = qtrue;
				continue;
			}
			// do not always send server info to all clients
			if ( index == CS_SERVERINFO && client->gentity && (client->gentity->r.svFlags & SVF_NOSERVERINFO) ) {
				continue;
			}
		
			SV_SendConfigstring(client, index);
		}
	}
}

/*
===============
SV_GetConfigstring

===============
*/
void SV_GetConfigstring( int index, char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetConfigstring: bufferSize == %i", bufferSize );
	}
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error (ERR_DROP, "SV_GetConfigstring: bad index %i", index);
	}
	if ( !sv.configstrings[index] ) {
		buffer[0] = 0;
		return;
	}

	Q_strncpyz( buffer, sv.configstrings[index], bufferSize );
}


/*
===============
SV_SetUserinfo

===============
*/
void SV_SetUserinfo( int index, const char *val ) {
	if ( index < 0 || index >= sv_maxclients->integer ) {
		Com_Error (ERR_DROP, "SV_SetUserinfo: bad index %i", index);
	}

	if ( !val ) {
		val = "";
	}

	Q_strncpyz( svs.clients[index].userinfo, val, sizeof( svs.clients[ index ].userinfo ) );
	Q_strncpyz( svs.clients[index].name, Info_ValueForKey( val, "name" ), sizeof(svs.clients[index].name) );
}



/*
===============
SV_GetUserinfo

===============
*/
void SV_GetUserinfo( int index, char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetUserinfo: bufferSize == %i", bufferSize );
	}
	if ( index < 0 || index >= sv_maxclients->integer ) {
		Com_Error (ERR_DROP, "SV_GetUserinfo: bad index %i", index);
	}
	Q_strncpyz( buffer, svs.clients[ index ].userinfo, bufferSize );
}


/*
================
SV_CreateBaseline

Entity baselines are used to compress non-delta messages
to the clients -- only the fields that differ from the
baseline will be transmitted
================
*/
static void SV_CreateBaseline( void ) {
	sharedEntity_t *svent;
	int				entnum;	

	for ( entnum = 1; entnum < sv.num_entities ; entnum++ ) {
		svent = SV_GentityNum(entnum);
		if (!svent->r.linked) {
			continue;
		}
		svent->s.number = entnum;

		//
		// take current state as baseline
		//
		sv.svEntities[entnum].baseline = svent->s;
	}
}


/*
===============
SV_BoundMaxClients

===============
*/
static void SV_BoundMaxClients( int minimum ) {
	// get the current maxclients value
	Cvar_Get( "sv_maxclients", "8", 0 );

	sv_maxclients->modified = qfalse;

	if ( sv_maxclients->integer < minimum ) {
		Cvar_Set( "sv_maxclients", va("%i", minimum) );
	} else if ( sv_maxclients->integer > MAX_CLIENTS ) {
		Cvar_Set( "sv_maxclients", va("%i", MAX_CLIENTS) );
	}
}


/*
===============
SV_Startup

Called when a host starts a map when it wasn't running
one before.  Successive map or map_restart commands will
NOT cause this to be called, unless the game is exited to
the menu system first.
===============
*/
static void SV_Startup( void ) {
	if ( svs.initialized ) {
		Com_Error( ERR_FATAL, "SV_Startup: svs.initialized" );
	}
	SV_BoundMaxClients( 1 );

	svs.clients = Z_Malloc (sizeof(client_t) * sv_maxclients->integer );
	if ( com_dedicated->integer ) {
		svs.numSnapshotEntities = sv_maxclients->integer * PACKET_BACKUP * MAX_SNAPSHOT_ENTITIES;
	} else {
		// we don't need nearly as many when playing locally
		svs.numSnapshotEntities = sv_maxclients->integer * 4 * MAX_SNAPSHOT_ENTITIES;
	}
	svs.initialized = qtrue;

	// Don't respect sv_killserver unless a server is actually running
	if ( sv_killserver->integer ) {
		Cvar_Set( "sv_killserver", "0" );
	}

	Cvar_Set( "sv_running", "1" );
	
	// Join the ipv6 multicast group now that a map is running so clients can scan for us on the local network.
	NET_JoinMulticast6();
}


/*
==================
SV_ChangeMaxClients
==================
*/
void SV_ChangeMaxClients( void ) {
	int		oldMaxClients;
	int		i;
	client_t	*oldClients;
	int		count;

	// get the highest client number in use
	count = 0;
	for ( i = 0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			if (i > count)
				count = i;
		}
	}
	count++;

	oldMaxClients = sv_maxclients->integer;
	// never go below the highest client number in use
	SV_BoundMaxClients( count );
	// if still the same
	if ( sv_maxclients->integer == oldMaxClients ) {
		return;
	}

	oldClients = Hunk_AllocateTempMemory( count * sizeof(client_t) );
	// copy the clients to hunk memory
	for ( i = 0 ; i < count ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			oldClients[i] = svs.clients[i];
		}
		else {
			Com_Memset(&oldClients[i], 0, sizeof(client_t));
		}
	}

	// free old clients arrays
	Z_Free( svs.clients );

	// allocate new clients
	svs.clients = Z_Malloc ( sv_maxclients->integer * sizeof(client_t) );
	Com_Memset( svs.clients, 0, sv_maxclients->integer * sizeof(client_t) );

	// copy the clients over
	for ( i = 0 ; i < count ; i++ ) {
		if ( oldClients[i].state >= CS_CONNECTED ) {
			svs.clients[i] = oldClients[i];
		}
	}

	// free the old clients on the hunk
	Hunk_FreeTempMemory( oldClients );
	
	// allocate new snapshot entities
	if ( com_dedicated->integer ) {
		svs.numSnapshotEntities = sv_maxclients->integer * PACKET_BACKUP * MAX_SNAPSHOT_ENTITIES;
	} else {
		// we don't need nearly as many when playing locally
		svs.numSnapshotEntities = sv_maxclients->integer * 4 * MAX_SNAPSHOT_ENTITIES;
	}
}

/*
================
SV_ClearServer
================
*/
static void SV_ClearServer(void) {
	int i;

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( sv.configstrings[i] ) {
			Z_Free( sv.configstrings[i] );
		}
	}
	Com_Memset (&sv, 0, sizeof(sv));
}

/*
================
SV_TouchFile
================
*/
static void SV_TouchFile( const char *filename ) {
	fileHandle_t	f;

	FS_FOpenFileRead( filename, &f, qfalse );
	if ( f ) {
		FS_FCloseFile( f );
	}
}

// the default map: the full game's paks and the demo's both have it
#define SV_DEFAULT_MAP	"q3dm17"

qboolean	sv_mapAsked;

/*
================
SV_GametypeName

g_gametype's name: the base game's game types, in the order of the game
modules' gametype_t (bg_public.h, which the engine doesn't include)
================
*/
static const char *SV_GametypeName( int gametype ) {
	static const char * const names[] = {
		"free for all", "tournament", "single player", "team deathmatch", "capture the flag"
	};

	if ( gametype >= 0 && gametype < ARRAY_LEN( names ) ) {
		return names[gametype];
	}
	return va( "game type %d", gametype );
}

/*
================
SV_NextGametype

The game type the next map plays: g_gametype's latched value, which a map
sets, and free for all for single player, which map (not spmap) turns into
it
================
*/
static int SV_NextGametype( void ) {
	int	gametype = sv_gametype->latchedString ? atoi( sv_gametype->latchedString ) : sv_gametype->integer;

	return gametype == GT_SINGLE_PLAYER ? GT_FFA : gametype;
}

/*
================
SV_NoteFrameRate

A frame rate other than the default that a config set, not the command
line, as an old server's config often holds Quake III's 20: kept, and
said so
================
*/
static void SV_NoteFrameRate( void ) {
	cvarSource_t	source = Cvar_Source( sv_fps );

	if ( Cvar_SameValue( sv_fps->string, sv_fps->resetString ) ||
		( source != CVAR_SOURCE_PLAYER && source != CVAR_SOURCE_SCRIPT ) ) {
		return;
	}
	Com_Printf( "sv_fps %d (set by a config, not the command line); " PRODUCT_NAME "'s default is %s. "
		"Remove the setting or set sv_fps %s to change it.\n",
		sv_fps->integer, sv_fps->resetString, sv_fps->resetString );
}

/*
================
SV_PrintStartSummary

What a dedicated server's admin needs once it's up: what it plays, from
which data and home, and how players and admins reach it, under the heading
================
*/
static void SV_PrintStartSummary( const char *heading ) {
	int	net = Cvar_VariableIntegerValue( "net_enabled" );

	Com_Printf( "%s\n", heading );
	// --check loads no map, and reports the maps itself
	if ( com_sv_running->integer ) {
		Com_Printf( "  map:      %s, %s\n", sv_mapname->string, SV_GametypeName( sv_gametype->integer ) );
	}
	if ( !Q_stricmp( com_basegame->string, "demoq3" ) ) {
		Com_Printf( "  data:     the demo's (%s)\n", com_basegame->string );
	} else if ( !com_standalone->integer ) {
		Com_Printf( "  data:     the full game's (%s)\n", com_basegame->string );
	} else {
		Com_Printf( "  data:     a standalone game's (%s)\n", com_basegame->string );
	}
	Com_Printf( "  home:     %s\n", Cvar_VariableString( "fs_homepath" ) );
	Com_Printf( "  game dir: %s\n", FS_GetCurrentGameDir() );
	if ( net & NET_ENABLEV4 ) {
		Com_Printf( "  port:     UDP %d (IPv4)\n", Cvar_VariableIntegerValue( "net_port" ) );
	}
	if ( net & NET_ENABLEV6 ) {
		Com_Printf( "  port:     UDP %d (IPv6)\n", Cvar_VariableIntegerValue( "net_port6" ) );
	}
	if ( com_dedicated->integer == 2 ) {
		Com_Printf( "  listed:   on the master servers (dedicated 2)\n" );
	} else {
		Com_Printf( "  listed:   no, found on the local network only; dedicated 2 lists it\n" );
	}
	Com_Printf( "  password: %s\n", Cvar_VariableString( "g_password" )[0] ? "on (g_password)" : "none" );
	Sys_PrintFirewall( NET_Port() );
	Com_Printf( "  rcon:     %s\n", sv_rconPassword->string[0] ? "on (rconpassword)" : "off: set rconpassword to use it" );
	// asks with the state it should be in, which leaves it as it is
	if ( Sys_KeepAwake( SV_HumanCount() > 0 ) ) {
		Com_Printf( "  sleep:    this computer stays awake while people play here\n" );
	}
	SV_NoteFrameRate();
}

/*
================
SV_PrintReady

Once a dedicated server is up, its map loaded and players able to join,
the last line of its startup, which tools wait for, the same from
version to version: "server ready: <map> <port>". A service manager that
asked (NOTIFY_SOCKET) is told too
================
*/
static void SV_PrintReady( void ) {
	int	port = NET_Port();

	Com_Printf( "server ready: %s %d\n", sv_mapname->string, port );
	Sys_Notify( va( "READY=1\nSTATUS=%s on UDP port %d", sv_mapname->string, port ) );
}

/*
================
SV_StartDefaultMap

Whether a dedicated server with no map started the default one: once its
startup commands have all run (a config's map or vstr among them), rather
than wait with none, saying how to choose another
================
*/
qboolean SV_StartDefaultMap( void ) {
	static qboolean	checked;

	if ( checked || sv_mapAsked || !com_dedicated->integer || !Cbuf_Empty() ) {
		return qfalse;
	}
	checked = qtrue;
	if ( FS_ReadFile( "maps/" SV_DEFAULT_MAP ".bsp", NULL ) <= 0 ) {
		return qfalse;
	}
	Com_Printf( "No map was given, so the server starts " SV_DEFAULT_MAP " (%s). To start another, add "
		"+map <name> to the command line, or a map line to server.cfg.\n",
		SV_GametypeName( SV_NextGametype() ) );
	Cbuf_ExecuteText( EXEC_NOW, "map " SV_DEFAULT_MAP );
	return qtrue;
}

/*
================
SV_CheckRconPassword

rcon_password is other engines' name for rconPassword: set alone, it
leaves rcon off without a word. Said once a run, as a map starts.
================
*/
static void SV_CheckRconPassword( void ) {
	static qboolean	warned;

	if ( !warned && Cvar_VariableString( "rcon_password" )[0] && !sv_rconPassword->string[0] ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: rcon_password is set, but this server's rcon "
			"password is rconPassword, which isn't, so rcon is off.\n" );
		warned = qtrue;
	}
}

/*
================
SV_SpawnServer

Change the server to a new map, taking all connected
clients along with it.
This is NOT called for map_restart
================
*/
void SV_SpawnServer( char *server, qboolean killBots ) {
	int			i;
	int			checksum;
	qboolean	isBot;
	char		systemInfo[16384];
	const char	*p;

	// shut down the existing game if it is running
	SV_ShutdownGameProgs();

	Com_Printf ("------ Server Initialization ------\n");
	Com_Printf ("Server: %s\n",server);

	// if not running a dedicated server CL_MapLoading will connect the client to the server
	// also print some status stuff
	CL_MapLoading();

	// make sure all the client stuff is unloaded
	CL_ShutdownAll(qfalse);

	// clear the whole hunk because we're (re)loading the server
	Hunk_Clear();

	// clear collision map data
	CM_ClearMap();

	// init client structures and svs.numSnapshotEntities 
	if ( !Cvar_VariableValue("sv_running") ) {
		SV_Startup();
	} else {
		// check for maxclients change
		if ( sv_maxclients->modified ) {
			SV_ChangeMaxClients();
		}
	}

	// clear pak references
	FS_ClearPakReferences(0);

	// allocate the snapshot entities on the hunk
	svs.snapshotEntities = Hunk_Alloc( sizeof(entityState_t)*svs.numSnapshotEntities, h_high );
	// OmniFrag's tests start the index near its limit, to test that
	// SV_BuildClientSnapshot lowers it; otherwise it starts at 0
	svs.nextSnapshotEntities = Cvar_Get( "sv_snapshotEntitiesStart", "0", CVAR_INIT )->integer;
	if ( svs.nextSnapshotEntities < 0 ) {
		svs.nextSnapshotEntities = 0;
	}

	// toggle the server bit so clients can detect that a
	// server has changed
	svs.snapFlagServerBit ^= SNAPFLAG_SERVERCOUNT;

	// set nextmap to the same map, but it may be overriden
	// by the game startup or another console command
	Cvar_Set( "nextmap", "map_restart 0");
//	Cvar_Set( "nextmap", va("map %s", server) );

	for (i=0 ; i<sv_maxclients->integer ; i++) {
		// save when the server started for each client already connected
		if (svs.clients[i].state >= CS_CONNECTED) {
			svs.clients[i].oldServerTime = sv.time;
		}
	}

	// wipe the entire per-level structure
	SV_ClearServer();
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		sv.configstrings[i] = CopyString("");
	}

	// make sure we are not paused
	Cvar_Set("cl_paused", "0");

	// get a new checksum feed and restart the file system
	sv.checksumFeed = ( ((unsigned int)rand() << 16) ^ (unsigned int)rand() ) ^ Com_Milliseconds();
	FS_Restart( sv.checksumFeed );

	CM_LoadMap( va("maps/%s.bsp", server), qfalse, &checksum );

	// set serverinfo visible name
	Cvar_Set( "mapname", server );

	Cvar_Set( "sv_mapChecksum", va("%i",checksum) );

	// serverid should be different each time
	sv.serverId = com_frameTime;
	sv.restartedServerId = sv.serverId; // I suppose the init here is just to be safe
	sv.checksumFeedServerId = sv.serverId;
	Cvar_Set( "sv_serverid", va("%i", sv.serverId ) );

	// clear physics interaction links
	SV_ClearWorld ();
	
	// media configstring setting should be done during
	// the loading stage, so connected clients don't have
	// to load during actual gameplay
	sv.state = SS_LOADING;

	// load and spawn all other entities
	SV_InitGameProgs();

	// don't allow a map_restart if game is modified
	sv_gametype->modified = qfalse;

	// run a few frames to allow everything to settle
	for (i = 0;i < 3; i++)
	{
		VM_Call (gvm, GAME_RUN_FRAME, sv.time);
		SV_BotFrame (sv.time);
		sv.time += 100;
		svs.time += 100;
	}

	// create a baseline for more efficient communications
	SV_CreateBaseline ();

	for (i=0 ; i<sv_maxclients->integer ; i++) {
		// send the new gamestate to all connected clients
		if (svs.clients[i].state >= CS_CONNECTED) {
			const char	*denied;

			if ( svs.clients[i].netchan.remoteAddress.type == NA_BOT ) {
				if ( killBots ) {
					SV_DropClient( &svs.clients[i], "" );
					continue;
				}
				isBot = qtrue;
			}
			else {
				isBot = qfalse;
			}

			// connect the client again
			denied = VM_ExplicitArgStr( gvm, VM_Call( gvm, GAME_CLIENT_CONNECT, i, qfalse, isBot ), "GAME_CLIENT_CONNECT's result" );	// firstTime = qfalse
			if ( denied ) {
				// this generally shouldn't happen, because the client
				// was connected before the level change
				SV_DropClient( &svs.clients[i], denied );
			} else {
				if( !isBot ) {
					// when we get the next packet from a connected client,
					// the new gamestate will be sent
					svs.clients[i].state = CS_CONNECTED;
				}
				else {
					client_t		*client;
					sharedEntity_t	*ent;

					client = &svs.clients[i];
					client->state = CS_ACTIVE;
					ent = SV_GentityNum( i );
					ent->s.number = i;
					client->gentity = ent;

					client->deltaMessage = -1;
					client->lastSnapshotTime = 0;	// generate a snapshot immediately

					VM_Call( gvm, GAME_CLIENT_BEGIN, i );
				}
			}
		}
	}	

	// run another frame to allow things to look at all the players
	VM_Call (gvm, GAME_RUN_FRAME, sv.time);
	SV_BotFrame (sv.time);
	sv.time += 100;
	svs.time += 100;

	if ( sv_pure->integer ) {
		// the server sends these to the clients so they will only
		// load pk3s also loaded at the server
		p = FS_LoadedPakChecksums();
		Cvar_Set( "sv_paks", p );
		if (strlen(p) == 0) {
			Com_Printf( "WARNING: sv_pure set but no PK3 files loaded\n" );
		}
		p = FS_LoadedPakNames();
		Cvar_Set( "sv_pakNames", p );

		// we need to touch the cgame and ui qvm because they could be in
		// separate pk3 files and the client will need to download the pk3
		// files with the latest cgame and ui qvm to pass the pure check
		SV_TouchFile( "vm/cgame.qvm" );
		SV_TouchFile( "vm/ui.qvm" );
	}
	else {
		Cvar_Set( "sv_paks", "" );
		Cvar_Set( "sv_pakNames", "" );
	}
	// the server sends these to the clients so they can figure
	// out which pk3s should be auto-downloaded
	p = FS_ReferencedPakChecksums();
	Cvar_Set( "sv_referencedPaks", p );
	p = FS_ReferencedPakNames();
	Cvar_Set( "sv_referencedPakNames", p );

	// save systeminfo and serverinfo strings
	Q_strncpyz( systemInfo, Cvar_InfoString_Big( CVAR_SYSTEMINFO ), sizeof( systemInfo ) );
	cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;
	SV_SetConfigstring( CS_SYSTEMINFO, systemInfo );

	SV_SetConfigstring( CS_SERVERINFO, Cvar_InfoString( CVAR_SERVERINFO ) );
	cvar_modifiedFlags &= ~CVAR_SERVERINFO;

	// any media configstring setting now should issue a warning
	// and any configstring changes should be reliably transmitted
	// to all clients
	sv.state = SS_GAME;

	// send a heartbeat now so the master will get up to date info
	SV_Heartbeat_f();

	Hunk_SetMark();

#ifndef DEDICATED
	if ( com_dedicated->integer ) {
		// restart renderer in order to show console for dedicated servers
		// launched through the regular binary
		CL_StartHunkUsers( qtrue );
	}
#endif

	SV_CheckRconPassword();

	Com_Printf ("-----------------------------------\n");
}

// the maps a dedicated server's startup started, until it's done: the
// latest, where from (Cmd_Origin), and one a config started that the
// command line's +map replaced
typedef struct {
	char	map[MAX_QPATH];
	char	origin[MAX_OSPATH];
} svStartupMap_t;

static svStartupMap_t	sv_startupMap, sv_replacedMap;
static qboolean		sv_startupFinished;

/*
================
SV_NoteStartupMap

A map a dedicated server's startup starts: one the command line's +map
starts over one a config started, its rotation's, say, is said when the
startup is done. --check notes every one, found or not, the rotation's too
================
*/
void SV_NoteStartupMap( const char *map, qboolean found ) {
	const char	*origin = Cmd_Origin();

	if ( com_check ) {
		SV_CheckMap( map, found );
	}
	if ( !found || sv_startupFinished || !com_dedicated->integer ) {
		return;
	}
	if ( Cmd_OriginIsCommandLine() && sv_startupMap.origin[0] &&
		strcmp( sv_startupMap.origin, CMD_ORIGIN_COMMAND_LINE ) ) {
		sv_replacedMap = sv_startupMap;
	}
	Q_strncpyz( sv_startupMap.map, map, sizeof( sv_startupMap.map ) );
	Q_strncpyz( sv_startupMap.origin, origin ? origin : "", sizeof( sv_startupMap.origin ) );
}

/*
================
SV_FinishStartup

Once a dedicated server's startup commands have all run with a map up:
its summary, its configs' lines that did nothing, then that it's ready
(SV_PrintReady). A config that never empties the buffers, a message
looping on a wait, say, gets them a few seconds after the first map is
up. --check's, with no map, is its report, and its exit
================
*/
#define SV_STARTUP_GRACE_MSEC	5000

void SV_FinishStartup( void ) {
	static int	firstFrame = -1;

	if ( sv_startupFinished || !com_dedicated->integer ) {
		return;
	}
	if ( firstFrame < 0 ) {
		firstFrame = Sys_Milliseconds();
	}
	if ( !Cbuf_Empty() && Sys_Milliseconds() - firstFrame < SV_STARTUP_GRACE_MSEC ) {
		return;
	}
	sv_startupFinished = qtrue;
	if ( com_check ) {
		SV_CheckReport();
	}
	SV_PrintStartSummary( com_check ? "The server would start with:" : "Server started:" );
	Cvar_WarnConfigs();
	if ( com_check ) {
		SV_CheckRconPassword();	// which a map's start says
	}
	if ( sv_replacedMap.map[0] ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: the command line's +map %s replaced %s, which %s started; "
			"leave +map out to keep that config's maps\n", sv_startupMap.map, sv_replacedMap.map, sv_replacedMap.origin );
	}
	if ( com_check ) {
		SV_CheckExit();
		return;
	}
	SV_PrintReady();
}

/*
===============
SV_Init

Only called at main exe startup, not for each game
===============
*/
void SV_Init (void)
{
	int index;

	SV_AddOperatorCommands ();

	// serverinfo vars
	Cvar_Get ("dmflags", "0", CVAR_SERVERINFO);
	Cvar_Get ("fraglimit", "20", CVAR_SERVERINFO);
	Cvar_Get ("timelimit", "0", CVAR_SERVERINFO);
	sv_gametype = Cvar_Get ("g_gametype", "0", CVAR_SERVERINFO | CVAR_LATCH );
	Cvar_Get ("sv_keywords", "", CVAR_SERVERINFO);
	sv_mapname = Cvar_Get ("mapname", "nomap", CVAR_SERVERINFO | CVAR_ROM);
	sv_privateClients = Cvar_Get ("sv_privateClients", "0", CVAR_SERVERINFO);
	sv_hostname = Cvar_Get ("sv_hostname", "noname", CVAR_SERVERINFO | CVAR_ARCHIVE );
	sv_maxclients = Cvar_Get ("sv_maxclients", "8", CVAR_SERVERINFO | CVAR_LATCH);

	sv_minRate = Cvar_Get ("sv_minRate", "0", CVAR_ARCHIVE | CVAR_SERVERINFO );
	sv_maxRate = Cvar_Get ("sv_maxRate", "0", CVAR_ARCHIVE | CVAR_SERVERINFO );
	sv_dlRate = Cvar_Get("sv_dlRate", "100", CVAR_ARCHIVE | CVAR_SERVERINFO);
	sv_minPing = Cvar_Get ("sv_minPing", "0", CVAR_ARCHIVE | CVAR_SERVERINFO );
	sv_maxPing = Cvar_Get ("sv_maxPing", "0", CVAR_ARCHIVE | CVAR_SERVERINFO );
	sv_floodProtect = Cvar_Get ("sv_floodProtect", "1", CVAR_ARCHIVE | CVAR_SERVERINFO );

	// systeminfo
	Cvar_Get ("sv_cheats", "1", CVAR_SYSTEMINFO | CVAR_ROM );
	sv_serverid = Cvar_Get ("sv_serverid", "0", CVAR_SYSTEMINFO | CVAR_ROM );
#ifdef __EMSCRIPTEN__
	// the web page's own server, which ioquake3's page always ran unpure
	sv_pure = Cvar_Get ("sv_pure", "0", CVAR_SYSTEMINFO );
#else
	sv_pure = Cvar_Get ("sv_pure", "1", CVAR_SYSTEMINFO );
#endif
#ifdef USE_VOIP
	sv_voip = Cvar_Get("sv_voip", "1", CVAR_LATCH);
	Cvar_CheckRange(sv_voip, 0, 1, qtrue);
	sv_voipProtocol = Cvar_Get("sv_voipProtocol", sv_voip->integer ? "opus" : "", CVAR_SYSTEMINFO | CVAR_ROM );
#endif
	Cvar_Get ("sv_paks", "", CVAR_SYSTEMINFO | CVAR_ROM );
	Cvar_Get ("sv_pakNames", "", CVAR_SYSTEMINFO | CVAR_ROM );
	Cvar_Get ("sv_referencedPaks", "", CVAR_SYSTEMINFO | CVAR_ROM );
	Cvar_Get ("sv_referencedPakNames", "", CVAR_SYSTEMINFO | CVAR_ROM );

	// server vars
	sv_rconPassword = Cvar_Get ("rconPassword", "", CVAR_TEMP | CVAR_PRIVATE );
	sv_privatePassword = Cvar_Get ("sv_privatePassword", "", CVAR_TEMP | CVAR_PRIVATE );
	sv_fps = Cvar_Get ("sv_fps", "40", CVAR_TEMP );
	// as Quake3e keeps it
	Cvar_CheckRange( sv_fps, 10, 125, qtrue );
	sv_timeout = Cvar_Get ("sv_timeout", "200", CVAR_TEMP );
	sv_zombietime = Cvar_Get ("sv_zombietime", "2", CVAR_TEMP );
	Cvar_Get ("nextmap", "", CVAR_TEMP );

	sv_allowDownload = Cvar_Get ("sv_allowDownload", "0", CVAR_SERVERINFO);
	sv_maxDownloads = Cvar_Get ("sv_maxDownloads", "8", CVAR_ARCHIVE);
	sv_refuseRetailPaks = Cvar_Get( "sv_refuseRetailPaks", "1", CVAR_ARCHIVE );
	Cvar_SetDescription( sv_refuseRetailPaks, "1: don't send the game's own paks (Quake III Arena 1.32's and "
		"Team Arena's, known by checksum, whatever their name) through this server's downloads; a player "
		"who lacks them is told which to add from their own copy. Downloads from sv_dlURL come from your "
		"own web host and aren't affected." );
	Cvar_Get ("sv_dlURL", "", CVAR_SERVERINFO | CVAR_ARCHIVE);
	
	sv_master[0] = Cvar_Get("sv_master1", MASTER_SERVER_NAME, 0);
	sv_master[1] = Cvar_Get("sv_master2", "directory.ioquake3.org", 0);
	for(index = 2; index < MAX_MASTER_SERVERS; index++)
		sv_master[index] = Cvar_Get(va("sv_master%d", index + 1), "", CVAR_ARCHIVE);

	sv_reconnectlimit = Cvar_Get ("sv_reconnectlimit", "3", 0);
	sv_showloss = Cvar_Get ("sv_showloss", "0", 0);
	sv_padPackets = Cvar_Get ("sv_padPackets", "0", 0);
	// a request: the client's, or the ui's (uiSetCvars)
	sv_killserver = Cvar_Get ("sv_killserver", "0", CVAR_ROM);
	sv_mapChecksum = Cvar_Get ("sv_mapChecksum", "", CVAR_ROM);
	sv_lanForceRate = Cvar_Get ("sv_lanForceRate", "1", CVAR_ARCHIVE );
#ifndef STANDALONE
	sv_strictAuth = Cvar_Get ("sv_strictAuth", "1", CVAR_ARCHIVE );
#endif
	sv_banFile = Cvar_Get("sv_banFile", "serverbans.dat", CVAR_ARCHIVE);

	// initialize bot cvars so they are listed and can be set before loading the botlib
	SV_BotInitCvars();

	// init the botlib here because we need the pre-compiler in the UI
	SV_BotInitBotLib();
	
	// Load saved bans
	Cbuf_AddText("rehashbans\n");
}


/*
==================
SV_FinalMessage

Used by SV_Shutdown to send a final message to all
connected clients before the server goes down.  The messages are sent immediately,
not just stuck on the outgoing message list, because the server is going
to totally exit after returning from this function.
==================
*/
void SV_FinalMessage( char *message ) {
	int			i, j;
	client_t	*cl;
	
	// send it twice, ignoring rate
	for ( j = 0 ; j < 2 ; j++ ) {
		for (i=0, cl = svs.clients ; i < sv_maxclients->integer ; i++, cl++) {
			if (cl->state >= CS_CONNECTED) {
				// don't send a disconnect to a local client
				if ( cl->netchan.remoteAddress.type != NA_LOOPBACK ) {
					SV_SendServerCommand( cl, "print \"%s\n\"\n", message );
					SV_SendServerCommand( cl, "disconnect \"%s\"", message );
				}
				// force a snapshot to be sent
				cl->lastSnapshotTime = 0;
				SV_SendClientSnapshot( cl );
			}
		}
	}
}


/*
================
SV_Shutdown

Called when each game quits,
before Sys_Quit or Sys_Error
================
*/
void SV_Shutdown( char *finalmsg ) {
	if ( !com_sv_running || !com_sv_running->integer ) {
		return;
	}

	Com_Printf( "----- Server Shutdown (%s) -----\n", finalmsg );

	NET_LeaveMulticast6();

	if ( svs.clients && !com_errorEntered ) {
		SV_FinalMessage( finalmsg );
	}

	SV_RemoveOperatorCommands();
	SV_MasterShutdown();
	SV_ShutdownGameProgs();
	Sys_KeepAwake( qfalse );

	// free current level
	SV_ClearServer();

	// free server static data
	if(svs.clients)
	{
		int index;
		
		for(index = 0; index < sv_maxclients->integer; index++)
			SV_FreeClient(&svs.clients[index]);
		
		Z_Free(svs.clients);
	}
	Com_Memset( &svs, 0, sizeof( svs ) );

	Cvar_Set( "sv_running", "0" );
	Cvar_Set("ui_singlePlayerActive", "0");

	Com_Printf( "---------------------------\n" );

	// disconnect any local clients
	if( sv_killserver->integer != 2 )
		CL_Disconnect( qfalse );
}

