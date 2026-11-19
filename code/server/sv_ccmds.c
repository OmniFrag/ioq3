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

#include <ctype.h>
#include <time.h>

/*
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/


/*
==================
SV_GetPlayerByHandle

Returns the player with player id or name from Cmd_Argv(1)
==================
*/
static client_t *SV_GetPlayerByHandle( void ) {
	client_t	*cl;
	int			i;
	char		*s;
	char		cleanName[64];

	// make sure server is running
	if ( !com_sv_running->integer ) {
		return NULL;
	}

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "No player specified.\n" );
		return NULL;
	}

	s = Cmd_Argv(1);

	// Check whether this is a numeric player handle
	for(i = 0; s[i] >= '0' && s[i] <= '9'; i++);
	
	if(!s[i])
	{
		int plid = atoi(s);

		// Check for numeric playerid match
		if(plid >= 0 && plid < svs.maxclients)
		{
			cl = &svs.clients[plid];
			
			if(cl->state)
				return cl;
		}
	}

	// check for a name match
	for ( i=0, cl=svs.clients ; i < svs.maxclients ; i++,cl++ ) {
		if ( !cl->state ) {
			continue;
		}
		if ( !Q_stricmp( cl->name, s ) ) {
			return cl;
		}

		Q_strncpyz( cleanName, cl->name, sizeof(cleanName) );
		Q_CleanStr( cleanName );
		if ( !Q_stricmp( cleanName, s ) ) {
			return cl;
		}
	}

	Com_Printf( "Player %s is not on the server\n", s );

	return NULL;
}

/*
==================
SV_GetPlayerByNum

Returns the player with idnum from Cmd_Argv(1)
==================
*/
static client_t *SV_GetPlayerByNum( void ) {
	client_t	*cl;
	int			i;
	int			idnum;
	char		*s;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		return NULL;
	}

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "No player specified.\n" );
		return NULL;
	}

	s = Cmd_Argv(1);

	for (i = 0; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9') {
			Com_Printf( "Bad slot number: %s\n", s);
			return NULL;
		}
	}
	idnum = atoi( s );
	if ( idnum < 0 || idnum >= svs.maxclients ) {
		Com_Printf( "Bad client slot: %i\n", idnum );
		return NULL;
	}

	cl = &svs.clients[idnum];
	if ( !cl->state ) {
		Com_Printf( "Client %i is not active\n", idnum );
		return NULL;
	}
	return cl;
}

//=========================================================


/*
==================
SV_Map_f

Restart the server on a different map
==================
*/
static void SV_Map_f( void ) {
	char		*cmd;
	char		*map;
	qboolean	killBots, cheat, found;
	char		expanded[MAX_QPATH];
	char		mapname[MAX_QPATH];
	char		why[MAX_STRING_CHARS];

	map = Cmd_Argv(1);
	if ( !map ) {
		return;
	}
	// an extension's refusal: a stopped server's config line, say
	if ( !Hooks_MapAllowed( map, why, sizeof( why ) ) ) {
		Com_Printf( "%s\n", why );
		return;
	}

	// one asked for, if not found, isn't replaced by the default
	sv_mapAsked = qtrue;

	// make sure the level exists before trying to change, so that
	// a typo at the server console won't end the game
	Com_sprintf (expanded, sizeof(expanded), "maps/%s.bsp", map);
	found = FS_ReadFile (expanded, NULL) != -1;
	// or in a pak content add installed since the map loaded, which the
	// load reads (SV_SpawnServer's FS_Restart)
	if ( !found && SV_MapPending( map ) ) {
		Com_Printf( "%s is in a pak added since the map loaded, which this map change reads\n", expanded );
		found = qtrue;
	}
	SV_NoteStartupMap( map, found );
	// --check loads no map: it noted which, for its report
	if ( com_check ) {
		return;
	}
	if ( !found ) {
		Com_Printf ("Can't find map %s\n", expanded);
		return;
	}

	// force latched values to get set
	Cvar_Get ("g_gametype", "0", CVAR_SERVERINFO | CVAR_USERINFO | CVAR_LATCH );

	cmd = Cmd_Argv(0);
	if( Q_stricmpn( cmd, "sp", 2 ) == 0 ) {
		Cvar_SetValue( "g_gametype", GT_SINGLE_PLAYER );
		Cvar_SetValue( "g_doWarmup", 0 );
		// may not set sv_maxclients directly, always set latched
		Cvar_SetLatched( "sv_maxclients", "8" );
		cmd += 2;
		if (!Q_stricmp( cmd, "devmap" ) ) {
			cheat = qtrue;
		} else {
			cheat = qfalse;
		}
		killBots = qtrue;
	}
	else {
		if ( !Q_stricmp( cmd, "devmap" ) ) {
			cheat = qtrue;
			killBots = qtrue;
		} else {
			cheat = qfalse;
			killBots = qfalse;
		}
		if( sv_gametype->integer == GT_SINGLE_PLAYER ) {
			Cvar_SetValue( "g_gametype", GT_FFA );
		}
	}

	// save the map name here cause on a map restart we reload the q3config.cfg
	// and thus nuke the arguments of the map command
	Q_strncpyz(mapname, map, sizeof(mapname));

	// for SV_RestartAfterError, should the map end with an error as it loads
	sv_mapCheats = cheat;

	// start up the map
	SV_SpawnServer( mapname, killBots );
	// a drain it ended, inside a hook, left the map as it was, to quit
	if ( sv_drainReason[0] ) {
		return;
	}

	// set the cheat value
	// if the level was started with "map <levelname>", then
	// cheats will not be allowed.  If started with "devmap <levelname>"
	// then cheats will be allowed
	if ( cheat ) {
		Cvar_Set( "sv_cheats", "1" );
	} else {
		Cvar_Set( "sv_cheats", "0" );
	}
}

/*
================
SV_MapRestart_f

Completely restarts a level, but doesn't send a new gamestate to the clients.
This allows fair starts with variable load times.
================
*/
static void SV_MapRestart_f( void ) {
	int			i;
	client_t	*client;
	const char	*denied;
	qboolean	isBot;
	int			delay;

	// make sure we aren't restarting twice in the same frame
	if ( com_frameTime == sv.serverId ) {
		return;
	}

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	// the restart, delayed or not, runs in the game's time
	SV_Wake( "the map restarted" );

	if ( sv.restartTime ) {
		return;
	}

	if (Cmd_Argc() > 1 ) {
		delay = atoi( Cmd_Argv(1) );
	}
	else {
		delay = 5;
	}
	if( delay && !Cvar_VariableValue("g_doWarmup") ) {
		sv.restartTime = sv.time + delay * 1000;
		SV_SetConfigstring( CS_WARMUP, va("%i", sv.restartTime) );
		return;
	}

	// check for changes in variables that can't just be restarted
	// check for maxclients change
	// and a pure setting waiting, whose lists are made as the map starts
	if ( sv_maxclients->modified || sv_gametype->modified || !!sv_pure->integer != sv.pure ) {
		char	mapname[MAX_QPATH];

		Com_Printf( "variable change -- restarting.\n" );
		// restart the map the slow way
		Q_strncpyz( mapname, Cvar_VariableString( "mapname" ), sizeof( mapname ) );

		SV_SpawnServer( mapname, qfalse );
		return;
	}

	// toggle the server bit so clients can detect that a
	// map_restart has happened
	svs.snapFlagServerBit ^= SNAPFLAG_SERVERCOUNT;

	// generate a new serverid	
	// TTimo - don't update restartedserverId there, otherwise we won't deal correctly with multiple map_restart
	sv.serverId = com_frameTime;
	Cvar_Set( "sv_serverid", va("%i", sv.serverId ) );

	// if a map_restart occurs while a client is changing maps, we need
	// to give them the correct time so that when they finish loading
	// they don't violate the backwards time check in cl_cgame.c
	for (i=0 ; i<svs.maxclients ; i++) {
		if (svs.clients[i].state == CS_PRIMED) {
			svs.clients[i].oldServerTime = sv.restartTime;
		}
	}

	// reset all the vm data in place without changing memory allocation
	// note that we do NOT set sv.state = SS_LOADING, so configstrings that
	// had been changed from their default values will generate broadcast updates
	sv.state = SS_LOADING;
	sv.restarting = qtrue;

	SV_RestartGameProgs();

	// run a few frames to allow everything to settle
	for (i = 0; i < 3; i++)
	{
		VM_Call (gvm, GAME_RUN_FRAME, sv.time);
		sv.time += 100;
		svs.time += 100;
	}

	sv.state = SS_GAME;
	sv.restarting = qfalse;

	// connect and begin all the clients
	for (i=0 ; i<svs.maxclients ; i++) {
		client = &svs.clients[i];

		// send the new gamestate to all connected clients
		if ( client->state < CS_CONNECTED) {
			continue;
		}

		if ( client->netchan.remoteAddress.type == NA_BOT ) {
			isBot = qtrue;
		} else {
			isBot = qfalse;
		}

		// add the map_restart command
		SV_AddServerCommand( client, "map_restart\n" );

		// connect the client again, without the firstTime flag
		denied = VM_ExplicitArgStr( gvm, VM_Call( gvm, GAME_CLIENT_CONNECT, i, qfalse, isBot ), "GAME_CLIENT_CONNECT's result" );
		if ( denied ) {
			// this generally shouldn't happen, because the client
			// was connected before the level change
			SV_DropClient( client, denied );
			Com_Printf( "SV_MapRestart_f(%d): dropped client %i - denied!\n", delay, i );
			continue;
		}

		if(client->state == CS_ACTIVE)
			SV_ClientEnterWorld(client, &client->lastUsercmd);
		else
		{
			// If we don't reset client->lastUsercmd and are restarting during map load,
			// the client will hang because we'll use the last Usercmd from the previous map,
			// which is wrong obviously.
			SV_ClientEnterWorld(client, NULL);
		}
	}	

	// run another frame to allow things to look at all the players
	VM_Call (gvm, GAME_RUN_FRAME, sv.time);
	sv.time += 100;
	svs.time += 100;

	Hooks_MapRestarted();
}

//===============================================================

/*
==================
SV_Kick_f

Kick a user off of the server
==================
*/
static void SV_Kick_f( void ) {
	client_t	*cl;
	int			i;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: kick <player name>\nkick all = kick everyone\nkick allbots = kick all bots\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if ( !cl ) {
		if ( !Q_stricmp(Cmd_Argv(1), "all") ) {
			for ( i=0, cl=svs.clients ; i < svs.maxclients ; i++,cl++ ) {
				if ( !cl->state ) {
					continue;
				}
				if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
					continue;
				}
				SV_DropClientAs( cl, "was kicked", SV_DROP_KICKED );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		else if ( !Q_stricmp(Cmd_Argv(1), "allbots") ) {
			for ( i=0, cl=svs.clients ; i < svs.maxclients ; i++,cl++ ) {
				if ( !cl->state ) {
					continue;
				}
				if( cl->netchan.remoteAddress.type != NA_BOT ) {
					continue;
				}
				SV_DropClientAs( cl, "was kicked", SV_DROP_KICKED );
				cl->lastPacketTime = svs.time;	// in case there is a funny zombie
			}
		}
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
		Com_Printf("Cannot kick host player\n");
		return;
	}

	SV_DropClientAs( cl, "was kicked", SV_DROP_KICKED );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
==================
SV_KickBots_f

Kick all bots off of the server
==================
*/
static void SV_KickBots_f( void ) {
	client_t	*cl;
	int			i;

	// make sure server is running
	if( !com_sv_running->integer ) {
		Com_Printf("Server is not running.\n");
		return;
	}

	for( i = 0, cl = svs.clients; i < svs.maxclients; i++, cl++ ) {
		if( !cl->state ) {
			continue;
		}

		if( cl->netchan.remoteAddress.type != NA_BOT ) {
			continue;
		}

		SV_DropClientAs( cl, "was kicked", SV_DROP_KICKED );
		cl->lastPacketTime = svs.time; // in case there is a funny zombie
	}
}
/*
==================
SV_KickAll_f

Kick all users off of the server
==================
*/
static void SV_KickAll_f( void ) {
	client_t *cl;
	int i;

	// make sure server is running
	if( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	for( i = 0, cl = svs.clients; i < svs.maxclients; i++, cl++ ) {
		if( !cl->state ) {
			continue;
		}

		if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
			continue;
		}

		SV_DropClientAs( cl, "was kicked", SV_DROP_KICKED );
		cl->lastPacketTime = svs.time; // in case there is a funny zombie
	}
}

/*
==================
SV_KickNum_f

Kick a user off of the server
==================
*/
static void SV_KickNum_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: %s <client number>\n", Cmd_Argv(0));
		return;
	}

	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
		Com_Printf("Cannot kick host player\n");
		return;
	}

	SV_DropClientAs( cl, "was kicked", SV_DROP_KICKED );
	cl->lastPacketTime = svs.time;	// in case there is a funny zombie
}

/*
==================
SV_MuteNum_f

Mutes a client's chat and voice, for so many seconds or until it leaves,
or lifts its mute
==================
*/
static void SV_MuteNum_f( void ) {
	qboolean	mute = !Q_stricmp( Cmd_Argv( 0 ), "clientmute" );
	client_t	*cl;

	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}
	// seconds, if given, a whole number above 0
	if ( Cmd_Argc() != 2 && !( mute && Cmd_Argc() == 3 && atoi( Cmd_Argv( 2 ) ) > 0 &&
		strspn( Cmd_Argv( 2 ), "0123456789" ) == strlen( Cmd_Argv( 2 ) ) ) ) {
		Com_Printf( mute ? "Usage: %s <client number> [seconds]\n" : "Usage: %s <client number>\n", Cmd_Argv( 0 ) );
		return;
	}
	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}
	// SV_MuteClient says when it changes
	if ( !SV_MuteClient( cl, mute, atoi( Cmd_Argv( 2 ) ) ) ) {
		Com_Printf( "%s" S_COLOR_WHITE " %s\n", cl->name, mute ? "is still muted" : "isn't muted" );
	}
}

#ifndef STANDALONE
// these functions require the auth server which of course is not available anymore for stand-alone games.

/*
==================
SV_Ban_f

Ban a user from being able to play on this server through the auth
server
==================
*/
static void SV_Ban_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: banUser <player name>\n");
		return;
	}

	cl = SV_GetPlayerByHandle();

	if (!cl) {
		return;
	}

	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
		Com_Printf("Cannot kick host player\n");
		return;
	}

	// look up the authorize server's IP
	if ( !svs.authorizeAddress.ip[0] && svs.authorizeAddress.type != NA_BAD ) {
		Com_Printf( "Resolving %s\n", AUTHORIZE_SERVER_NAME );
		if ( !NET_StringToAdr( AUTHORIZE_SERVER_NAME, &svs.authorizeAddress, NA_IP ) ) {
			Com_Printf( "Couldn't resolve address\n" );
			return;
		}
		svs.authorizeAddress.port = BigShort( PORT_AUTHORIZE );
		Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", AUTHORIZE_SERVER_NAME,
			svs.authorizeAddress.ip[0], svs.authorizeAddress.ip[1],
			svs.authorizeAddress.ip[2], svs.authorizeAddress.ip[3],
			BigShort( svs.authorizeAddress.port ) );
	}

	// otherwise send their ip to the authorize server
	if ( svs.authorizeAddress.type != NA_BAD ) {
		NET_OutOfBandPrintDirect( NS_SERVER, svs.authorizeAddress,
			"banUser %i.%i.%i.%i", cl->netchan.remoteAddress.ip[0], cl->netchan.remoteAddress.ip[1], 
								   cl->netchan.remoteAddress.ip[2], cl->netchan.remoteAddress.ip[3] );
		Com_Printf("%s was banned from coming back\n", cl->name);
	}
}

/*
==================
SV_BanNum_f

Ban a user from being able to play on this server through the auth
server
==================
*/
static void SV_BanNum_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: banClient <client number>\n");
		return;
	}

	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}
	if( cl->netchan.remoteAddress.type == NA_LOOPBACK ) {
		Com_Printf("Cannot kick host player\n");
		return;
	}

	// look up the authorize server's IP
	if ( !svs.authorizeAddress.ip[0] && svs.authorizeAddress.type != NA_BAD ) {
		Com_Printf( "Resolving %s\n", AUTHORIZE_SERVER_NAME );
		if ( !NET_StringToAdr( AUTHORIZE_SERVER_NAME, &svs.authorizeAddress, NA_IP ) ) {
			Com_Printf( "Couldn't resolve address\n" );
			return;
		}
		svs.authorizeAddress.port = BigShort( PORT_AUTHORIZE );
		Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", AUTHORIZE_SERVER_NAME,
			svs.authorizeAddress.ip[0], svs.authorizeAddress.ip[1],
			svs.authorizeAddress.ip[2], svs.authorizeAddress.ip[3],
			BigShort( svs.authorizeAddress.port ) );
	}

	// otherwise send their ip to the authorize server
	if ( svs.authorizeAddress.type != NA_BAD ) {
		NET_OutOfBandPrintDirect( NS_SERVER, svs.authorizeAddress,
			"banUser %i.%i.%i.%i", cl->netchan.remoteAddress.ip[0], cl->netchan.remoteAddress.ip[1], 
								   cl->netchan.remoteAddress.ip[2], cl->netchan.remoteAddress.ip[3] );
		Com_Printf("%s was banned from coming back\n", cl->name);
	}
}
#endif

// changes whenever the ban list does (SV_ExtBanListChanges)
static int serverBansChanges;

static void SV_LiftExpiredBans(void);

/*
==================
SV_RehashBans_f

Load saved bans from file: the server's own (FS_ServerFile), or while it
has none, the one the game directory kept before, which servers on every
port shared
==================
*/
static void SV_RehashBans_f(void)
{
	int index, filelen;
	fileHandle_t readfrom;
	char *textbuf, *curpos, *maskpos, *newlinepos, *endpos;
	char filepath[MAX_OSPATH];

	serverBansCount = 0;
	serverBansChanges++;

	if(!sv_banFile->string || !*sv_banFile->string)
		return;

	Q_strncpyz(filepath, FS_ServerQPath(sv_banFile->string), sizeof(filepath));
	if((filelen = FS_BaseDir_FOpenFileRead(filepath, &readfrom)) < 0 && *FS_ServerDir())
	{
		Com_sprintf(filepath, sizeof(filepath), "%s/%s", FS_GetCurrentGameDir(), sv_banFile->string);
		filelen = FS_BaseDir_FOpenFileRead(filepath, &readfrom);
	}

	if(filelen >= 0)
	{
		if(filelen < 2)
		{
			// Don't bother if file is too short.
			FS_FCloseFile(readfrom);
			return;
		}

		curpos = textbuf = Z_Malloc(filelen);
		
		filelen = FS_Read(textbuf, filelen, readfrom);
		FS_FCloseFile(readfrom);
		
		endpos = textbuf + filelen;
		
		for(index = 0; index < SERVER_MAXBANS && curpos + 2 < endpos; index++)
		{
			// find the end of the address string
			for(maskpos = curpos + 2; maskpos < endpos && *maskpos != ' '; maskpos++);
			
			if(maskpos + 1 >= endpos)
				break;

			*maskpos = '\0';
			maskpos++;
			
			// find the end of the subnet specifier
			for(newlinepos = maskpos; newlinepos < endpos && *newlinepos != '\n'; newlinepos++);
			
			if(newlinepos >= endpos)
				break;
			
			*newlinepos = '\0';
			
			Com_Memset(&serverBans[index], 0, sizeof(serverBans[index]));
			if(NET_StringToAdr(curpos + 2, &serverBans[index].ip, NA_UNSPEC))
			{
				const char *details = strchr(maskpos, ' ');

				serverBans[index].isexception = (curpos[0] != '0');
				serverBans[index].subnet = atoi(maskpos);
				if(details)
					SV_ParseBanDetails(details + 1, &serverBans[index].details);
				
				if(serverBans[index].ip.type == NA_IP &&
				   (serverBans[index].subnet < 1 || serverBans[index].subnet > 32))
				{
					serverBans[index].subnet = 32;
				}
				else if(serverBans[index].ip.type == NA_IP6 &&
					(serverBans[index].subnet < 1 || serverBans[index].subnet > 128))
				{
					serverBans[index].subnet = 128;
				}
			}
			
			curpos = newlinepos + 1;
		}
			
		serverBansCount = index;
		
		Z_Free(textbuf);
	}
	// those that ran out while the server was down go before anyone connects
	SV_LiftExpiredBans();
}

/*
==================
SV_WriteBans

Save bans to file, the server's own (FS_ServerFile). Every change to the
list ends here.
==================
*/
// a ban's line in the ban file, into out of size: as a classic server
// writes it, then its details, which a classic server reads past and keeps
// the ban for good; its length
static int SV_BanLine(const serverBan_t *curban, char *out, int size)
{
	char by[4 * EXT_BAN_NAME + 3], name[4 * EXT_BAN_NAME + 3], reason[4 * EXT_BAN_REASON + 3];
	int len = Com_sprintf(out, size, "%d %s %d", curban->isexception, NET_AdrToString(curban->ip), curban->subnet);

	// any details kept, an end above all: one dropped would make the ban
	// last for good after the next restart
	if(curban->details.recorded || curban->details.expires || curban->details.by[0] || curban->details.name[0] ||
		curban->details.reason[0])
	{
		SV_BanQuote(curban->details.by, by, sizeof(by));
		SV_BanQuote(curban->details.name, name, sizeof(name));
		SV_BanQuote(curban->details.reason, reason, sizeof(reason));
		len += Com_sprintf(out + len, size - len, " t=%lld e=%lld b=%s n=%s r=%s", (long long)curban->details.at,
			(long long)curban->details.expires, by, name, reason);
	}
	return len + Com_sprintf(out + len, size - len, "\n");
}

static void SV_WriteBans(void)
{
	int index, len = 0, size, pass;
	char filepath[MAX_OSPATH], *text;

	serverBansChanges++;

	if(!sv_banFile->string || !*sv_banFile->string)
		return;

	Q_strncpyz(filepath, FS_ServerQPath(sv_banFile->string), sizeof(filepath));

	// sized by a first pass of the lines, written in a second
	size = 1;
	text = NULL;
	for(pass = 0; pass < 2; pass++)
	{
		if(pass)
			text = Z_Malloc(size);
		for(index = len = 0; index < serverBansCount; index++)
		{
			char line[NET_ADDRSTRMAXLEN + 64 + 4 * (2 * EXT_BAN_NAME + EXT_BAN_REASON) + 32];

			len += SV_BanLine(&serverBans[index], pass ? text + len : line, pass ? size - len : sizeof(line));
		}
		size = len + 1;
	}
	// replaced whole, never half written, and never as a library, program,
	// game module or pak, as FS_FOpenFileWrite refuses
	if(FS_IsCodeFile(filepath))
		Com_Printf(S_COLOR_YELLOW "WARNING: the bans aren't written to %s, a code file's name\n", filepath);
	else if(!FS_ReplaceOSFile(FS_BaseDir_BuildOSPath(Cvar_VariableString("fs_homestatepath"), filepath), text, len, qfalse) &&
		!com_check)
		Com_Printf(S_COLOR_YELLOW "WARNING: the bans couldn't be written to %s\n", filepath);
	Z_Free(text);
}

/*
==================
SV_DelBanEntryFromList

Remove a ban or an exception from the list.
==================
*/

static qboolean SV_DelBanEntryFromList(int index)
{
	if(index == serverBansCount - 1)
		serverBansCount--;
	else if(index < ARRAY_LEN(serverBans) - 1)
	{
		memmove(serverBans + index, serverBans + index + 1, (serverBansCount - index - 1) * sizeof(*serverBans));
		serverBansCount--;
	}
	else
		return qtrue;

	return qfalse;
}

/*
==================
SV_ParseCIDRNotation

Parse a CIDR notation type string and return a netadr_t and suffix by reference
==================
*/

qboolean SV_ParseCIDRNotation(netadr_t *dest, int *mask, char *adrstr)
{
	char *suffix;
	
	suffix = strchr(adrstr, '/');
	if(suffix)
	{
		*suffix = '\0';
		suffix++;
	}

	if(!NET_StringToAdr(adrstr, dest, NA_UNSPEC))
		return qtrue;

	if(suffix)
	{
		*mask = atoi(suffix);
		
		if(dest->type == NA_IP)
		{
			if(*mask < 1 || *mask > 32)
				*mask = 32;
		}
		else
		{
			if(*mask < 1 || *mask > 128)
				*mask = 128;
		}
	}
	else if(dest->type == NA_IP)
		*mask = 32;
	else
		*mask = 128;
	
	return qfalse;
}

/*
==================
SV_AddBan

Add a ban or an exception of ip/mask, with details' who made it, when,
who it was, why and when it runs out if given, unless an entry already
supersedes it, delete the entries the new one supersedes, and save the
list. Returns
the new entry's index, or -1 with why: *covering is then the index of the
entry that supersedes it, or -1.
==================
*/

// whether a ban that runs out at ends lasts at least as long as one that
// runs out at other, each 0 for never, so that it can stand for it
static qboolean SV_BanOutlasts(int64_t ends, int64_t other)
{
	return !ends || (other && ends >= other);
}

static int SV_AddBan(netadr_t ip, int mask, qboolean isexception, const extBanDetails_t *details, int *covering,
	char *why, int whySize)
{
	char addy2[NET_ADDRSTRMAXLEN];
	int index;
	serverBan_t *curban;
	int64_t expires = details && !isexception ? details->expires : 0;

	*covering = -1;

	if(ip.type != NA_IP && ip.type != NA_IP6)
	{
		Q_strncpyz(why, "Error: Can ban players connected via the internet only.", whySize);
		return -1;
	}

	// first check whether a conflicting ban exists that would supersede the new one.
	for(index = 0; index < serverBansCount; index++)
	{
		curban = &serverBans[index];

		if(curban->subnet <= mask)
		{
			// a ban that runs out sooner doesn't: the new one replaces it
			if((curban->isexception || (!isexception && SV_BanOutlasts(curban->details.expires, expires))) &&
			   NET_CompareBaseAdrMask(curban->ip, ip, curban->subnet))
			{
				Q_strncpyz(addy2, NET_AdrToString(ip), sizeof(addy2));

				Com_sprintf(why, whySize, "Error: %s %s/%d supersedes %s %s/%d", curban->isexception ? "Exception" : "Ban",
					   NET_AdrToString(curban->ip), curban->subnet,
					   isexception ? "exception" : "ban", addy2, mask);
				*covering = index;
				return -1;
			}
		}
		if(curban->subnet >= mask)
		{
			if(!curban->isexception && isexception && NET_CompareBaseAdrMask(curban->ip, ip, mask))
			{
				Q_strncpyz(addy2, NET_AdrToString(curban->ip), sizeof(addy2));

				Com_sprintf(why, whySize, "Error: %s %s/%d supersedes already existing %s %s/%d", isexception ? "Exception" : "Ban",
					   NET_AdrToString(ip), mask,
					   curban->isexception ? "exception" : "ban", addy2, curban->subnet);
				return -1;
			}
		}
	}

	// after the entry that covers it, so adding it again names that entry
	// on a full list too (banaddr checks for a full list itself, first)
	if(serverBansCount >= ARRAY_LEN(serverBans))
	{
		Q_strncpyz(why, "Error: Maximum number of bans/exceptions exceeded.", whySize);
		return -1;
	}

	// now delete bans that are superseded by the new one: those it covers,
	// the same range's among them, that it lasts as long as. One the
	// checks above let through of the same range is a ban that runs out
	// sooner
	index = 0;
	while(index < serverBansCount)
	{
		curban = &serverBans[index];

		if(curban->subnet >= mask && (isexception || (!curban->isexception && SV_BanOutlasts(expires, curban->details.expires))) &&
		   NET_CompareBaseAdrMask(curban->ip, ip, mask))
			SV_DelBanEntryFromList(index);
		else
			index++;
	}

	index = serverBansCount++;
	Com_Memset(&serverBans[index], 0, sizeof(serverBans[index]));
	if(details)
		serverBans[index].details = *details;
	serverBans[index].ip = ip;
	serverBans[index].subnet = mask;
	serverBans[index].isexception = isexception;

	SV_WriteBans();

	return index;
}

/*
==================
SV_AddBanToList

Ban a user from being able to play on this server based on his ip address.
==================
*/

static void SV_AddBanToList(qboolean isexception)
{
	char *banstring;
	char why[MAX_STRING_CHARS];
	netadr_t ip;
	int argc, mask, covering, duration = 0, i;
	extBanDetails_t details;
	client_t *named = NULL;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	argc = Cmd_Argc();

	// a ban's length last, which no subnet looks like: "1h", "7d"
	Com_Memset(&details, 0, sizeof(details));
	if(!isexception && argc >= 3 && Cmd_Argv(argc - 1)[0] &&
	   !isdigit((unsigned char)Cmd_Argv(argc - 1)[strlen(Cmd_Argv(argc - 1)) - 1]))
	{
		if(!(duration = SV_BanDuration(Cmd_Argv(argc - 1))))
		{
			Com_Printf("Error: %s isn't a ban's length: a number and s, m, h, d or w, up to %d days\n",
				Cmd_Argv(argc - 1), SERVER_BAN_LONGEST / (24 * 60 * 60));
			return;
		}
		argc--;
	}

	if(argc < 2 || argc > 3)
	{
		Com_Printf ("Usage: %s (ip[/subnet] | clientnum [subnet])%s\n", Cmd_Argv(0),
			isexception ? "" : " [length: 1h, 1d, 7d, 30d]");
		return;
	}

	if(serverBansCount >= ARRAY_LEN(serverBans))
	{
		Com_Printf ("Error: Maximum number of bans/exceptions exceeded.\n");
		return;
	}

	banstring = Cmd_Argv(1);
	
	if(strchr(banstring, '.') || strchr(banstring, ':'))
	{
		// This is an ip address, not a client num.
		
		if(SV_ParseCIDRNotation(&ip, &mask, banstring))
		{
			Com_Printf("Error: Invalid address %s\n", banstring);
			return;
		}
	}
	else
	{
		client_t *cl;
		
		// client num.
		
		cl = SV_GetPlayerByNum();

		if(!cl)
		{
			Com_Printf("Error: Playernum %s does not exist.\n", Cmd_Argv(1));
			return;
		}
		
		ip = cl->netchan.remoteAddress;
		named = cl;
		
		if(argc == 3)
		{
			mask = atoi(Cmd_Argv(2));
			
			if(ip.type == NA_IP)
			{
				if(mask < 1 || mask > 32)
					mask = 32;
			}
			else
			{
				if(mask < 1 || mask > 128)
					mask = 128;
			}
		}
		else
			mask = (ip.type == NA_IP6) ? 128 : 32;
	}

	// who it was: the client named, or the first one the ban covers
	for(i = 0; !named && i < sv_maxclients->integer; i++)
	{
		if(svs.clients[i].state >= CS_CONNECTED && NET_CompareBaseAdrMask(svs.clients[i].netchan.remoteAddress, ip, mask))
			named = &svs.clients[i];
	}
	if(!isexception)
	{
		details.recorded = qtrue;
		details.at = (int64_t)time(NULL);
		details.expires = duration ? details.at + duration : 0;
		Q_strncpyz(details.by, Cmd_Origin() ? Cmd_Origin() : Com_IsRedirecting() ? "rcon" : "console", sizeof(details.by));
		if(named)
			Q_strncpyz(details.name, named->name, sizeof(details.name));
	}

	if(SV_AddBan(ip, mask, isexception, isexception ? NULL : &details, &covering, why, sizeof(why)) < 0)
	{
		Com_Printf("%s\n", why);
		return;
	}

	Com_Printf("Added %s: %s/%d%s\n", isexception ? "ban exception" : "ban",
		   NET_AdrToString(ip), mask, duration ? va(", for %s", Cmd_Argv(Cmd_Argc() - 1)) : "");
}

/*
==================
SV_DelBanFromList

Remove a ban or an exception from the list.
==================
*/

static void SV_DelBanFromList(qboolean isexception)
{
	int index, count = 0, todel, mask;
	netadr_t ip;
	char *banstring;
	
	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}
	
	if(Cmd_Argc() != 2)
	{
		Com_Printf ("Usage: %s (ip[/subnet] | num)\n", Cmd_Argv(0));
		return;
	}

	banstring = Cmd_Argv(1);
	
	if(strchr(banstring, '.') || strchr(banstring, ':'))
	{
		serverBan_t *curban;
		
		if(SV_ParseCIDRNotation(&ip, &mask, banstring))
		{
			Com_Printf("Error: Invalid address %s\n", banstring);
			return;
		}
		
		index = 0;
		
		while(index < serverBansCount)
		{
			curban = &serverBans[index];
			
			if(curban->isexception == isexception		&&
			   curban->subnet >= mask 			&&
			   NET_CompareBaseAdrMask(curban->ip, ip, mask))
			{
				Com_Printf("Deleting %s %s/%d\n",
					   isexception ? "exception" : "ban",
					   NET_AdrToString(curban->ip), curban->subnet);
					   
				SV_DelBanEntryFromList(index);
			}
			else
				index++;
		}
	}
	else
	{
		todel = atoi(Cmd_Argv(1));

		if(todel < 1 || todel > serverBansCount)
		{
			Com_Printf("Error: Invalid ban number given\n");
			return;
		}
	
		for(index = 0; index < serverBansCount; index++)
		{
			if(serverBans[index].isexception == isexception)
			{
				count++;
			
				if(count == todel)
				{
					Com_Printf("Deleting %s %s/%d\n",
					   isexception ? "exception" : "ban",
					   NET_AdrToString(serverBans[index].ip), serverBans[index].subnet);

					SV_DelBanEntryFromList(index);

					break;
				}
			}
		}
	}
	
	SV_WriteBans();
}


// a ban's text for the console, each control character, which a ban file
// edited by hand may hold, a space, so no line it prints looks like another
static const char *SV_BanPrintable(const char *in, char *out, int size)
{
	int len = 0;

	for(; *in && len < size - 1; in++)
		out[len++] = ((unsigned char)*in < 0x20 || *in == 0x7f) ? ' ' : *in;
	out[len] = '\0';
	return out;
}

// a ban's time for the console; gmtime refuses some a hand-edited file may
// name (Windows' past the year 3000)
static void SV_BanTime(int64_t at, char *out, int size)
{
	time_t when = (time_t)at;
	struct tm *tm = gmtime(&when);

	if(!tm || !strftime(out, size, "%Y-%m-%d %H:%M UTC", tm))
		Q_strncpyz(out, "an unknown time", size);
}

/*
==================
SV_BanDetailsText

What listbans says of a ban after its address: who it was, who made it
and when, when it runs out, and why; "" for a ban made before the file
kept them, and its end alone for one with an end but no maker or time
==================
*/
static const char *SV_BanDetailsText(const serverBan_t *ban)
{
	static char text[MAX_STRING_CHARS];
	char made[32], ends[32], name[EXT_BAN_NAME], by[EXT_BAN_NAME], reason[EXT_BAN_REASON];

	if(!ban->details.recorded && !ban->details.expires)
		return "";
	SV_BanTime(ban->details.at, made, sizeof(made));
	SV_BanTime(ban->details.expires, ends, sizeof(ends));
	// one whose maker and time aren't known, but whose end is (a ban
	// file's, edited by hand): that alone
	if(!ban->details.recorded)
	{
		Com_sprintf(text, sizeof(text), ", until %s", ends);
		return text;
	}
	Com_sprintf(text, sizeof(text), ", %s" S_COLOR_WHITE ", by %s, %s, %s%s%s",
		ban->details.name[0] ? SV_BanPrintable(ban->details.name, name, sizeof(name)) : "no name",
		SV_BanPrintable(ban->details.by, by, sizeof(by)), made, ban->details.expires ? "until " : "for good", ban->details.expires ? ends : "",
		ban->details.reason[0] ? va(": %s", SV_BanPrintable(ban->details.reason, reason, sizeof(reason))) : "");
	return text;
}

/*
==================
SV_LiftExpiredBans, SV_ExpireBans

The bans past their end lifted and saved, each said at the console: as
the file's read, and each second. An exception never runs out
==================
*/
static void SV_LiftExpiredBans(void)
{
	int64_t now = (int64_t)time(NULL);
	int index = 0;
	qboolean lifted = qfalse;

	while(index < serverBansCount)
	{
		serverBan_t *ban = &serverBans[index];

		if(!ban->isexception && ban->details.expires && ban->details.expires <= now)
		{
			char name[EXT_BAN_NAME];

			Com_Printf("The ban on %s/%d%s ran out.\n", NET_AdrToString(ban->ip), ban->subnet,
				ban->details.name[0] ? va(" (%s" S_COLOR_WHITE ")", SV_BanPrintable(ban->details.name, name, sizeof(name))) : "");
			SV_DelBanEntryFromList(index);
			lifted = qtrue;
		}
		else
			index++;
	}
	if(lifted)
		SV_WriteBans();
}

void SV_ExpireBans(void)
{
	static int64_t checked;
	int64_t now = (int64_t)time(NULL);

	if(now != checked)
	{
		checked = now;
		SV_LiftExpiredBans();
	}
}

/*
==================
SV_ListBans_f

List all bans and exceptions on console
==================
*/

static void SV_ListBans_f(void)
{
	int index, count;
	serverBan_t *ban;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}
	
	// List all bans
	for(index = count = 0; index < serverBansCount; index++)
	{
		ban = &serverBans[index];
		if(!ban->isexception)
		{
			count++;

			Com_Printf("Ban #%d: %s/%d%s\n", count,
				    NET_AdrToString(ban->ip), ban->subnet, SV_BanDetailsText(ban));
		}
	}
	// List all exceptions
	for(index = count = 0; index < serverBansCount; index++)
	{
		ban = &serverBans[index];
		if(ban->isexception)
		{
			count++;

			Com_Printf("Except #%d: %s/%d\n", count,
				    NET_AdrToString(ban->ip), ban->subnet);
		}
	}
}

/*
==================
SV_FlushBans_f

Delete all bans and exceptions.
==================
*/

static void SV_FlushBans_f(void)
{
	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	serverBansCount = 0;
	
	// empty the ban file.
	SV_WriteBans();
	
	Com_Printf("All bans and exceptions have been deleted.\n");
}

/*
==================
SV_ExtBanCount, SV_ExtGetBan, SV_ExtBanListChanges

The ban list, for extensions (qcommon/hooks.h)
==================
*/
int SV_ExtBanCount( void ) {
	return serverBansCount;
}

qboolean SV_ExtGetBan( int index, extBan_t *ban ) {
	if ( index < 0 || index >= serverBansCount ) {
		return qfalse;
	}
	Com_Memset( ban, 0, sizeof( *ban ) );
	Q_strncpyz( ban->address, NET_AdrToString( serverBans[index].ip ), sizeof( ban->address ) );
	ban->subnet = serverBans[index].subnet;
	ban->exception = serverBans[index].isexception;
	ban->details = serverBans[index].details;
	return qtrue;
}

int SV_ExtBanListChanges( void ) {
	return serverBansChanges;
}

/*
==================
SV_ExtAddBan

Add a ban or an exception for an extension (qcommon/hooks.h): a numeric
address, or a range in CIDR notation whose prefix is in range, which
banaddr would widen to one address
==================
*/
int SV_ExtAddBan( const char *address, qboolean exception, const extBanDetails_t *details, int *covering,
	char *why, int whySize ) {
	char		text[NET_ADDRSTRMAXLEN + 4];
	char		*suffix;
	netadr_t	ip;
	int		bits, mask, index;
	extBanDetails_t	given;

	*covering = -1;
	Q_strncpyz( text, address, sizeof( text ) );
	suffix = strchr( text, '/' );
	if ( suffix ) {
		*suffix++ = '\0';
	}
	if ( strlen( address ) >= sizeof( text ) || !Sys_NumericToAdr( text, &ip ) ) {
		goto invalid;
	}

	bits = ip.type == NA_IP6 ? 128 : 32;
	mask = bits;
	if ( suffix ) {
		if ( !*suffix || strlen( suffix ) > 3 || strspn( suffix, "0123456789" ) != strlen( suffix ) ) {
			goto invalid;
		}
		mask = atoi( suffix );
	}
	if ( mask < 1 || mask > bits ) {
		goto invalid;
	}

	if ( details && !exception ) {
		given = *details;
		given.recorded = qtrue;
		given.at = details->at ? details->at : (int64_t)time( NULL );
	}
	index = SV_AddBan( ip, mask, exception, details && !exception ? &given : NULL, covering, why, whySize );
	return index >= 0 ? index : EXT_BAN_REFUSED;

invalid:
	Com_sprintf( why, whySize, "Error: Invalid address %s", address );
	return EXT_BAN_INVALID;
}

/*
==================
SV_ExtRemoveBan

Remove a ban or an exception for an extension (qcommon/hooks.h)
==================
*/
qboolean SV_ExtRemoveBan( int index ) {
	if ( index < 0 || index >= serverBansCount ) {
		return qfalse;
	}
	SV_DelBanEntryFromList( index );
	SV_WriteBans();
	return qtrue;
}

static void SV_BanAddr_f(void)
{
	SV_AddBanToList(qfalse);
}

static void SV_ExceptAddr_f(void)
{
	SV_AddBanToList(qtrue);
}

static void SV_BanDel_f(void)
{
	SV_DelBanFromList(qfalse);
}

static void SV_ExceptDel_f(void)
{
	SV_DelBanFromList(qtrue);
}

/*
** SV_Strlen -- skips color escape codes
*/
static int SV_Strlen( const char *str ) {
	const char *s = str;
	int count = 0;

	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			s += 2;
		} else {
			count++;
			s++;
		}
	}

	return count;
}

/*
================
SV_Status_f

The map and its players, then on the console the engine extensions'
lines (Hooks_Status)
================
*/
static void SV_Status_f( void ) {
	int			i, j, l;
	client_t	*cl;
	playerState_t	*ps;
	const char		*s;
	int			ping;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		Hooks_Status();
		return;
	}

	Com_Printf ("map: %s\n", sv_mapname->string );

	Com_Printf ("cl score ping name            address                                 rate \n");
	Com_Printf ("-- ----- ---- --------------- --------------------------------------- -----\n");
	for (i=0,cl=svs.clients ; i < svs.maxclients ; i++,cl++)
	{
		if (!cl->state)
			continue;
		Com_Printf ("%2i ", i);
		ps = SV_GameClientNum( i );
		Com_Printf ("%5i ", ps->persistant[PERS_SCORE]);

		if (cl->state == CS_CONNECTED)
			Com_Printf ("CON ");
		else if (cl->state == CS_ZOMBIE)
			Com_Printf ("ZMB ");
		else
		{
			ping = cl->ping < 9999 ? cl->ping : 9999;
			Com_Printf ("%4i ", ping);
		}

		Com_Printf ("%s", cl->name);
		
		l = 16 - SV_Strlen(cl->name);
		j = 0;
		
		do
		{
			Com_Printf (" ");
			j++;
		} while(j < l);


		// TTimo adding a ^7 to reset the color
		s = NET_AdrToString( cl->netchan.remoteAddress );
		Com_Printf ("^7%s", s);
		l = 39 - strlen(s);
		j = 0;
		
		do
		{
			Com_Printf(" ");
			j++;
		} while(j < l);
		
		Com_Printf (" %5i", cl->rate);

		Com_Printf ("\n");
	}
	Com_Printf ("\n");
	Hooks_Status();
}

/*
==================
SV_ConSay_f
==================
*/
static void SV_ConSay_f(void) {
	char	*p;
	char	text[1024];

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc () < 2 ) {
		return;
	}

	Q_strncpyz( text, "console: ", sizeof( text ) );
	p = Cmd_Args();

	if ( *p == '"' ) {
		p++;
		p[strlen(p)-1] = 0;
	}

	Q_strcat( text, sizeof( text ), p );

	Com_Printf("%s\n", text);
	SV_SendServerCommand(NULL, "chat \"%s\"", text);
}

/*
==================
SV_ConTell_f
==================
*/
static void SV_ConTell_f(void) {
	char	*p;
	char	text[1024];
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() < 3 ) {
		Com_Printf ("Usage: tell <client number> <text>\n");
		return;
	}

	cl = SV_GetPlayerByNum();
	if ( !cl ) {
		return;
	}

	Q_strncpyz( text, "console_tell: ", sizeof( text ) );
	p = Cmd_ArgsFrom(2);

	if ( *p == '"' ) {
		p++;
		p[strlen(p)-1] = 0;
	}

	Q_strcat( text, sizeof( text ), p );

	Com_Printf("%s\n", text);
	SV_SendServerCommand(cl, "chat \"%s\"", text);
}


/*
==================
SV_ConSayto_f
==================
*/
static void SV_ConSayto_f(void) {
	char		*p;
	char		text[1024];
	client_t	*cl;
	char		*rawname;
	char		name[MAX_NAME_LENGTH];
	char		cleanName[MAX_NAME_LENGTH];
	client_t	*saytocl;
	int			i;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() < 3 ) {
		Com_Printf ("Usage: sayto <player name> <text>\n");
		return;
	}

	rawname = Cmd_Argv(1);
	
	//allowing special characters in the console 
	//with hex strings for player names
	Com_FieldStringToPlayerName( name, MAX_NAME_LENGTH, rawname );

	saytocl = NULL;
	for ( i=0, cl=svs.clients ; i < svs.maxclients ; i++,cl++ ) {
		if ( !cl->state ) {
			continue;
		}
		Q_strncpyz( cleanName, cl->name, sizeof(cleanName) );
		Q_CleanStr( cleanName );

		if ( !Q_stricmp( cleanName, name ) ) {
			saytocl = cl;
			break;
		}
	}
	if( !saytocl )
	{
		Com_Printf ("No such player name: %s.\n", name);
		return;
	}

	Q_strncpyz( text, "console_sayto: ", sizeof( text ) );
	p = Cmd_ArgsFrom(2);

	if ( *p == '"' ) {
		p++;
		p[strlen(p)-1] = 0;
	}

	Q_strcat( text, sizeof( text ), p );

	Com_Printf("%s\n", text);
	SV_SendServerCommand(saytocl, "chat \"%s\"", text);
}


/*
==================
SV_Heartbeat_f

Also called by SV_DropClient, SV_DirectConnect, SV_SpawnServer, and SV_ExtUndrain
==================
*/
void SV_Heartbeat_f( void ) {
	svs.nextHeartbeatTime = -9999999;
}

/*
==================
SV_ExtDrain

A dedicated server takes no new player, refusing them with the reason,
leaves the master servers' lists, and stops once its players have left or
its map ends (SV_DrainDone), its players told why, exiting with status (0,
or EXIT_RESTART for a restart asked for); SV_ExtUndrain cancels it before
then. Its players can reconnect meanwhile
==================
*/
qboolean SV_ExtDrain( const char *reason, int status ) {
	char	*s;

	if ( !com_dedicated->integer || !com_sv_running->integer ) {
		Com_Printf( "drain is for a dedicated server with a map up.\n" );
		return qfalse;
	}
	// the masters' last heartbeat, before the server stops answering their
	// polls, so they drop it
	if ( !sv_drainReason[0] ) {
		SV_MasterShutdown();
	}
	Q_strncpyz( sv_drainReason, reason && *reason ? reason : "This server is closing.", sizeof( sv_drainReason ) );
	sv_drainStatus = status;
	// what an info string's value can't hold
	for ( s = sv_drainReason; *s; s++ ) {
		if ( *s == '\\' || *s == ';' || *s == '"' ) {
			*s = ' ';
		}
	}
	Com_Printf( "Draining: new players are refused (\"%s\"), the server leaves the master lists, and it "
		"stops once its players have left or the map ends; undrain cancels it.\n", sv_drainReason );
	return qtrue;
}

void SV_ExtUndrain( void ) {
	if ( !sv_drainReason[0] ) {
		return;
	}
	sv_drainReason[0] = '\0';
	sv_drainStatus = 0;
	// back on the masters' lists
	SV_Heartbeat_f();
	Com_Printf( "Undrained: new players are let in, and the server is listed again.\n" );
}

// drain [reason]: SV_ExtDrain, exiting 0
static void SV_Drain_f( void ) {
	SV_ExtDrain( Cmd_ArgsFrom( 1 ), 0 );
}

static void SV_Undrain_f( void ) {
	if ( !sv_drainReason[0] ) {
		Com_Printf( "The server isn't draining.\n" );
		return;
	}
	SV_ExtUndrain();
}


/*
===========
SV_Serverinfo_f

Examine the serverinfo string
===========
*/
static void SV_Serverinfo_f( void ) {
	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	Com_Printf ("Server info settings:\n");
	Info_Print ( SV_ServerInfo() );
}


/*
===========
SV_Systeminfo_f

Examine the systeminfo string
===========
*/
static void SV_Systeminfo_f( void ) {
	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	Com_Printf ("System info settings:\n");
	// as clients are sent them, sv_pure 0 where the map plays as not pure
	Info_Print ( SV_SystemInfo() );
}


/*
===========
SV_DumpUser_f

Examine all a users info strings
===========
*/
static void SV_DumpUser_f( void ) {
	client_t	*cl;

	// make sure server is running
	if ( !com_sv_running->integer ) {
		Com_Printf( "Server is not running.\n" );
		return;
	}

	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("Usage: dumpuser <userid>\n");
		return;
	}

	cl = SV_GetPlayerByHandle();
	if ( !cl ) {
		return;
	}

	Com_Printf( "userinfo\n" );
	Com_Printf( "--------\n" );
	Info_Print( cl->userinfo );
}


/*
=================
SV_KillServer
=================
*/
static void SV_KillServer_f( void ) {
	SV_Shutdown( "killserver" );
}

//===========================================================

/*
==================
SV_CompleteMapName
==================
*/
static void SV_CompleteMapName( char *args, int argNum ) {
	if( argNum == 2 ) {
		Field_CompleteFilename( "maps", "bsp", NULL, qtrue, qfalse );
	}
}

/*
==================
SV_CompletePlayerName
==================
*/
static void SV_CompletePlayerName( char *args, int argNum ) {
	if( argNum == 2 ) {
		char		names[MAX_CLIENTS][MAX_NAME_LENGTH];
		const char	*namesPtr[MAX_CLIENTS];
		client_t	*cl;
		int			i;
		int			nameCount;
		int			clientCount;

		nameCount = 0;
		clientCount = svs.maxclients;

		for ( i=0, cl=svs.clients ; i < clientCount; i++,cl++ ) {
			if ( !cl->state ) {
				continue;
			}
			if( i >= MAX_CLIENTS ) {
				break;
			}
			Q_strncpyz( names[nameCount], cl->name, sizeof(names[nameCount]) );
			Q_CleanStr( names[nameCount] );

			namesPtr[nameCount] = names[nameCount];

			nameCount++;
		}
		qsort( (void*)namesPtr, nameCount, sizeof( namesPtr[0] ), Com_strCompare );

		Field_CompletePlayerName( namesPtr, nameCount );
	}
}

/*
==================
SV_AddOperatorCommands
==================
*/
void SV_AddOperatorCommands( void ) {
	static qboolean	initialized;

	if ( initialized ) {
		return;
	}
	initialized = qtrue;

	Cmd_AddCommand ("heartbeat", SV_Heartbeat_f);
	Cmd_AddCommand ("drain", SV_Drain_f);
	Cmd_AddCommand ("undrain", SV_Undrain_f);
	Cmd_AddCommand ("kick", SV_Kick_f);
#ifndef STANDALONE
	if(!com_standalone->integer)
	{
		Cmd_AddCommand ("banUser", SV_Ban_f);
		Cmd_AddCommand ("banClient", SV_BanNum_f);
	}
#endif
	Cmd_AddCommand ("kickbots", SV_KickBots_f);
	Cmd_AddCommand ("kickall", SV_KickAll_f);
	Cmd_AddCommand ("kicknum", SV_KickNum_f);
	Cmd_AddCommand ("clientkick", SV_KickNum_f); // Legacy command
	Cmd_AddCommand ("clientmute", SV_MuteNum_f);
	Cmd_AddCommand ("clientunmute", SV_MuteNum_f);
	Cmd_AddCommand ("status", SV_Status_f);
	Cmd_AddCommand ("serverinfo", SV_Serverinfo_f);
	Cmd_AddCommand ("systeminfo", SV_Systeminfo_f);
	Cmd_AddCommand ("dumpuser", SV_DumpUser_f);
	Cmd_AddCommand ("map_restart", SV_MapRestart_f);
	Cmd_AddCommand ("sectorlist", SV_SectorList_f);
	Cmd_AddCommand ("map", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "map", SV_CompleteMapName );
#ifndef PRE_RELEASE_DEMO
	Cmd_AddCommand ("devmap", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "devmap", SV_CompleteMapName );
	Cmd_AddCommand ("spmap", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "spmap", SV_CompleteMapName );
	Cmd_AddCommand ("spdevmap", SV_Map_f);
	Cmd_SetCommandCompletionFunc( "spdevmap", SV_CompleteMapName );
#endif
	Cmd_AddCommand ("killserver", SV_KillServer_f);
	if( com_dedicated->integer ) {
		Cmd_AddCommand ("say", SV_ConSay_f);
		Cmd_AddCommand ("tell", SV_ConTell_f);
		Cmd_AddCommand ("sayto", SV_ConSayto_f);
		Cmd_SetCommandCompletionFunc( "sayto", SV_CompletePlayerName );
	}
	
	Cmd_AddCommand("rehashbans", SV_RehashBans_f);
	Cmd_AddCommand("listbans", SV_ListBans_f);
	Cmd_AddCommand("banaddr", SV_BanAddr_f);
	Cmd_AddCommand("exceptaddr", SV_ExceptAddr_f);
	Cmd_AddCommand("bandel", SV_BanDel_f);
	Cmd_AddCommand("exceptdel", SV_ExceptDel_f);
	Cmd_AddCommand("flushbans", SV_FlushBans_f);
	Cmd_AddCommand("content", SV_Content_f);
}

/*
==================
SV_RemoveOperatorCommands
==================
*/
void SV_RemoveOperatorCommands( void ) {
#if 0
	// removing these won't let the server start again
	Cmd_RemoveCommand ("heartbeat");
	Cmd_RemoveCommand ("kick");
	Cmd_RemoveCommand ("kicknum");
	Cmd_RemoveCommand ("clientkick");
	Cmd_RemoveCommand ("clientmute");
	Cmd_RemoveCommand ("clientunmute");
	Cmd_RemoveCommand ("kickall");
	Cmd_RemoveCommand ("kickbots");
	Cmd_RemoveCommand ("banUser");
	Cmd_RemoveCommand ("banClient");
	Cmd_RemoveCommand ("status");
	Cmd_RemoveCommand ("serverinfo");
	Cmd_RemoveCommand ("systeminfo");
	Cmd_RemoveCommand ("dumpuser");
	Cmd_RemoveCommand ("map_restart");
	Cmd_RemoveCommand ("sectorlist");
	Cmd_RemoveCommand ("say");
#endif
}

