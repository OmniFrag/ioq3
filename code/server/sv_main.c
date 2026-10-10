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

#ifdef USE_VOIP
cvar_t *sv_voip;
cvar_t *sv_voipProtocol;
#endif

serverStatic_t	svs;				// persistant server info
server_t		sv;					// local server
vm_t			*gvm = NULL;				// game virtual machine

cvar_t	*sv_fps = NULL;			// time rate for running non-clients
cvar_t	*sv_timeout;			// seconds without any message
cvar_t	*sv_zombietime;			// seconds to sink messages after disconnect
cvar_t	*sv_rconPassword;		// password for remote server commands
cvar_t	*sv_privatePassword;		// password for the privateClient slots
cvar_t	*sv_allowDownload;
cvar_t	*sv_refuseRetailPaks;	// the game's own paks aren't downloaded
cvar_t	*sv_maxDownloads;		// UDP downloads at once, 0 for no limit
cvar_t	*sv_maxclients;

cvar_t	*sv_privateClients;		// number of clients reserved for password
cvar_t	*sv_hostname;
cvar_t	*sv_master[MAX_MASTER_SERVERS];	// master server ip address
cvar_t	*sv_reconnectlimit;		// minimum seconds between connect messages
cvar_t	*sv_showloss;			// report when usercmds are lost
cvar_t	*sv_padPackets;			// add nop bytes to messages
cvar_t	*sv_killserver;			// menu system can set to 1 to shut server down
cvar_t	*sv_mapname;
cvar_t	*sv_mapChecksum;
cvar_t	*sv_serverid;
cvar_t	*sv_minRate;
cvar_t	*sv_maxRate;
cvar_t	*sv_dlRate;
cvar_t	*sv_minPing;
cvar_t	*sv_maxPing;
cvar_t	*sv_gametype;
cvar_t	*sv_pure;
cvar_t	*sv_floodProtect;
cvar_t	*sv_lanForceRate; // dedicated 1 (LAN) server forces local client rates to 99999 (bug #491)
#ifndef STANDALONE
cvar_t	*sv_strictAuth;
#endif
cvar_t	*sv_banFile;
cvar_t	*sv_rconAllow;		// the addresses rcon is taken from, empty for any

serverBan_t serverBans[SERVER_MAXBANS];
int serverBansCount = 0;

/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

/*
===============
SV_ExpandNewlines

Converts newlines to "\n" so a line prints nicer
===============
*/
static char	*SV_ExpandNewlines( char *in ) {
	static	char	string[1024];
	int		l;

	l = 0;
	while ( *in && l < sizeof(string) - 3 ) {
		if ( *in == '\n' ) {
			string[l++] = '\\';
			string[l++] = 'n';
		} else {
			string[l++] = *in;
		}
		in++;
	}
	string[l] = 0;

	return string;
}

/*
======================
SV_ReplacePendingServerCommands

FIXME: This is ugly
======================
*/
#if 0 // unused
static int SV_ReplacePendingServerCommands( client_t *client, const char *cmd ) {
	int i, index, csnum1, csnum2;

	for ( i = client->reliableSent+1; i <= client->reliableSequence; i++ ) {
		index = i & ( MAX_RELIABLE_COMMANDS - 1 );
		//
		if ( !Q_strncmp(cmd, client->reliableCommands[ index ], strlen("cs")) ) {
			sscanf(cmd, "cs %i", &csnum1);
			sscanf(client->reliableCommands[ index ], "cs %i", &csnum2);
			if ( csnum1 == csnum2 ) {
				Q_strncpyz( client->reliableCommands[ index ], cmd, sizeof( client->reliableCommands[ index ] ) );
				/*
				if ( client->netchan.remoteAddress.type != NA_BOT ) {
					Com_Printf( "WARNING: client %i removed double pending config string %i: %s\n", client-svs.clients, csnum1, cmd );
				}
				*/
				return qtrue;
			}
		}
	}
	return qfalse;
}
#endif

/*
======================
SV_AddServerCommand

The given command will be transmitted to the client, and is guaranteed to
not have future snapshot_t executed before it is executed
======================
*/
void SV_AddServerCommand( client_t *client, const char *cmd ) {
	int		index, i, oldest;

	// this is very ugly but it's also a waste to for instance send multiple config string updates
	// for the same config string index in one snapshot
//	if ( SV_ReplacePendingServerCommands( client, cmd ) ) {
//		return;
//	}

	// do not send commands until the gamestate has been sent
	if( client->state < CS_PRIMED )
		return;

	client->reliableSequence++;
	// if we would be losing an old command that hasn't been acknowledged,
	// or that the client hasn't had a snapshot with (snapshotAcknowledge),
	// we must drop the connection
	// we check == instead of >= so a broadcast print added by SV_DropClient()
	// doesn't cause a recursive drop client
	oldest = client->reliableAcknowledge - client->snapshotAcknowledge > 0 ?
		client->snapshotAcknowledge : client->reliableAcknowledge;
	if ( client->reliableSequence - oldest == MAX_RELIABLE_COMMANDS + 1 ) {
		if ( client->gamestateMessageNum == -1 )  {
			// invalid game state message 
			// this can occur in SV_DropClient() to avoid calling it more than once
			return;
		}

		Com_Printf( "===== pending server commands =====\n" );
		for ( i = client->reliableAcknowledge + 1 ; i <= client->reliableSequence ; i++ ) {
			Com_Printf( "cmd %5d: %s\n", i, client->reliableCommands[ i & (MAX_RELIABLE_COMMANDS-1) ] );
		}
		Com_Printf( "cmd %5d: %s\n", i, cmd );
		SV_DropClient( client, "Server command overflow" );
		return;
	}
	index = client->reliableSequence & ( MAX_RELIABLE_COMMANDS - 1 );
	Q_strncpyz( client->reliableCommands[ index ], cmd, sizeof( client->reliableCommands[ index ] ) );
}


/*
=================
SV_SendServerCommand

Sends a reliable command string to be interpreted by 
the client game module: "cp", "print", "chat", etc
A NULL client will broadcast to all clients
=================
*/
void QDECL SV_SendServerCommand(client_t *cl, const char *fmt, ...) {
	va_list		argptr;
	byte		message[MAX_MSGLEN];
	client_t	*client;
	int			j;
	
	va_start (argptr,fmt);
	Q_vsnprintf ((char *)message, sizeof(message), fmt,argptr);
	va_end (argptr);

	// Fix to http://aluigi.altervista.org/adv/q3msgboom-adv.txt
	// The actual cause of the bug is probably further downstream
	// and should maybe be addressed later, but this certainly
	// fixes the problem for now
	if ( strlen ((char *)message) > 1022 ) {
		return;
	}

	if ( cl != NULL ) {
		SV_AddServerCommand( cl, (char *)message );
		return;
	}

	// hack to echo broadcast prints to console
	if ( com_dedicated->integer && !strncmp( (char *)message, "print", 5) ) {
		Com_Printf ("broadcast: %s\n", SV_ExpandNewlines((char *)message) );
	}

	// send the data to all relevant clients
	for (j = 0, client = svs.clients; j < sv_maxclients->integer ; j++, client++) {
		SV_AddServerCommand( client, (char *)message );
	}
}


/*
==============================================================================

MASTER SERVER FUNCTIONS

==============================================================================
*/

/*
================
SV_MasterHeartbeat

Send a message to the masters every few minutes to
let it know we are alive, and log information.
We will also have a heartbeat sent when a server
changes from empty to non-empty, and full to non-full,
but not on every player enter or exit.
================
*/
#define	HEARTBEAT_MSEC	300*1000
#define	MASTERDNS_MSEC	24*60*60*1000
void SV_MasterHeartbeat(const char *message)
{
	static netadr_t	adr[MAX_MASTER_SERVERS][2]; // [2] for v4 and v6 address for the same address string.
	int			i;
	int			res;
	int			netenabled;

	netenabled = Cvar_VariableIntegerValue("net_enabled");

	// "dedicated 1" is for lan play, "dedicated 2" is for inet public play
	if (!com_dedicated || com_dedicated->integer != 2 || !(netenabled & (NET_ENABLEV4 | NET_ENABLEV6)))
		return;		// only dedicated servers send heartbeats

	// if not time yet, don't send anything
	if ( svs.time < svs.nextHeartbeatTime )
		return;

	if ( !Q_stricmp( com_gamename->string, LEGACY_MASTER_GAMENAME ) )
		message = LEGACY_HEARTBEAT_FOR_MASTER;

	svs.nextHeartbeatTime = svs.time + HEARTBEAT_MSEC;

	// send to group masters
	for (i = 0; i < MAX_MASTER_SERVERS; i++)
	{
		if(!sv_master[i]->string[0])
			continue;

		// see if we haven't already resolved the name or if it's been over 24 hours
		// resolving usually causes hitches on win95, so only do it when needed
		if (sv_master[i]->modified || svs.time > svs.masterResolveTime[i])
		{
			sv_master[i]->modified = qfalse;
			svs.masterResolveTime[i] = svs.time + MASTERDNS_MSEC;
			
			if(netenabled & NET_ENABLEV4)
			{
				Com_Printf("Resolving %s (IPv4)\n", sv_master[i]->string);
				res = NET_StringToAdr(sv_master[i]->string, &adr[i][0], NA_IP);

				if(res == 2)
				{
					// if no port was specified, use the default master port
					adr[i][0].port = BigShort(PORT_MASTER);
				}
				
				if(res)
					Com_Printf( "%s resolved to %s\n", sv_master[i]->string, NET_AdrToStringwPort(adr[i][0]));
				else
					Com_Printf( "%s has no IPv4 address.\n", sv_master[i]->string);
			}
			
			if(netenabled & NET_ENABLEV6)
			{
				Com_Printf("Resolving %s (IPv6)\n", sv_master[i]->string);
				res = NET_StringToAdr(sv_master[i]->string, &adr[i][1], NA_IP6);

				if(res == 2)
				{
					// if no port was specified, use the default master port
					adr[i][1].port = BigShort(PORT_MASTER);
				}
				
				if(res)
					Com_Printf( "%s resolved to %s\n", sv_master[i]->string, NET_AdrToStringwPort(adr[i][1]));
				else
					Com_Printf( "%s has no IPv6 address.\n", sv_master[i]->string);
			}
		}

		if(adr[i][0].type == NA_BAD && adr[i][1].type == NA_BAD)
		{
			continue;
		}


		Com_Printf ("Sending heartbeat to %s\n", sv_master[i]->string );

		// this command should be changed if the server info / status format
		// ever incompatably changes

		if(adr[i][0].type != NA_BAD)
			NET_OutOfBandPrint( NS_SERVER, adr[i][0], "heartbeat %s\n", message);
		if(adr[i][1].type != NA_BAD)
			NET_OutOfBandPrint( NS_SERVER, adr[i][1], "heartbeat %s\n", message);
	}
}

/*
=================
SV_MasterShutdown

Informs all masters that this server is going down
=================
*/
void SV_MasterShutdown( void ) {
	// send a heartbeat right now
	svs.nextHeartbeatTime = -9999;
	SV_MasterHeartbeat(HEARTBEAT_FOR_MASTER);

	// send it again to minimize chance of drops
	svs.nextHeartbeatTime = -9999;
	SV_MasterHeartbeat(HEARTBEAT_FOR_MASTER);

	// when the master tries to poll the server, it won't respond, so
	// it will be removed from the list
}


/*
==============================================================================

CONNECTIONLESS COMMANDS

==============================================================================
*/

// This is deliberately quite large to make it more of an effort to DoS
#define MAX_BUCKETS			16384
#define MAX_HASHES			1024

typedef struct {
	leakyBucket_t	buckets[ MAX_BUCKETS ];
	leakyBucket_t	*hashes[ MAX_HASHES ];
} bucketTable_t;

// the queries' and connects' buckets, and rcon's own, so a flood of
// getstatus from an address can't starve its admin's commands
static bucketTable_t queryBuckets;
static bucketTable_t rconBuckets;
rateLimit_t outboundLeakyBucket;

/*
================
SVC_HashForAddress
================
*/
static long SVC_HashForAddress( netadr_t address ) {
	byte 		*ip = NULL;
	size_t	size = 0;
	int			i;
	long		hash = 0;

	switch ( address.type ) {
		case NA_IP:  ip = address.ip;  size = 4; break;
		case NA_IP6: ip = address.ip6; size = 16; break;
		default: break;
	}

	for ( i = 0; i < size; i++ ) {
		hash += (long)( ip[ i ] ) * ( i + 119 );
	}

	hash = ( hash ^ ( hash >> 10 ) ^ ( hash >> 20 ) );
	hash &= ( MAX_HASHES - 1 );

	return hash;
}

/*
================
SVC_BucketForAddress

Find or allocate a bucket for an address
================
*/
static leakyBucket_t *SVC_BucketForAddress( bucketTable_t *table, netadr_t address, int burst, int period ) {
	leakyBucket_t	*bucket = NULL;
	int						i;
	long					hash = SVC_HashForAddress( address );
	int						now = Sys_Milliseconds();

	for ( bucket = table->hashes[ hash ]; bucket; bucket = bucket->next ) {
		switch ( bucket->type ) {
			case NA_IP:
				if ( memcmp( bucket->ipv._4, address.ip, 4 ) == 0 ) {
					return bucket;
				}
				break;

			case NA_IP6:
				if ( memcmp( bucket->ipv._6, address.ip6, 16 ) == 0 ) {
					return bucket;
				}
				break;

			default:
				break;
		}
	}

	for ( i = 0; i < MAX_BUCKETS; i++ ) {
		int interval;

		bucket = &table->buckets[ i ];
		interval = now - bucket->rate.lastTime;

		// Reclaim expired buckets
		if ( bucket->rate.lastTime > 0 && ( interval > ( burst * period ) ||
					interval < 0 ) ) {
			if ( bucket->prev != NULL ) {
				bucket->prev->next = bucket->next;
			} else {
				table->hashes[ bucket->hash ] = bucket->next;
			}
			
			if ( bucket->next != NULL ) {
				bucket->next->prev = bucket->prev;
			}

			Com_Memset( bucket, 0, sizeof( leakyBucket_t ) );
		}

		if ( bucket->type == NA_BAD ) {
			bucket->type = address.type;
			switch ( address.type ) {
				case NA_IP:  Com_Memcpy( bucket->ipv._4, address.ip, 4 );   break;
				case NA_IP6: Com_Memcpy( bucket->ipv._6, address.ip6, 16 ); break;
				default: break;
			}

			bucket->rate.lastTime = now;
			bucket->rate.burst = 0;
			bucket->hash = hash;

			// Add to the head of the relevant hash chain
			bucket->next = table->hashes[ hash ];
			if ( table->hashes[ hash ] != NULL ) {
				table->hashes[ hash ]->prev = bucket;
			}

			bucket->prev = NULL;
			table->hashes[ hash ] = bucket;

			return bucket;
		}
	}

	// Couldn't allocate a bucket for this address
	return NULL;
}

/*
================
SVC_RateLimit
================
*/
qboolean SVC_RateLimit( rateLimit_t *bucket, int burst, int period ) {
	if ( bucket != NULL ) {
		int now = Sys_Milliseconds();
		int interval = now - bucket->lastTime;
		int expired = interval / period;
		int expiredRemainder = interval % period;

		if ( expired > bucket->burst || interval < 0 ) {
			bucket->burst = 0;
			bucket->lastTime = now;
		} else {
			bucket->burst -= expired;
			bucket->lastTime = now - expiredRemainder;
		}

		if ( bucket->burst < burst ) {
			bucket->burst++;

			return qfalse;
		}
	}

	return qtrue;
}

/*
================
SVC_RateLimitAddress

Rate limit for a particular address
================
*/
qboolean SVC_RateLimitAddress( netadr_t from, int burst, int period ) {
	leakyBucket_t *bucket = SVC_BucketForAddress( &queryBuckets, from, burst, period );

	return SVC_RateLimit( bucket ? &bucket->rate : NULL, burst, period );
}

/*
================
SVC_RateRestoreBurstAddress

Give back the request an address's last SVC_RateLimitAddress counted
================
*/
void SVC_RateRestoreBurstAddress( netadr_t from, int burst, int period ) {
	leakyBucket_t *bucket = SVC_BucketForAddress( &queryBuckets, from, burst, period );

	if ( bucket != NULL && bucket->rate.burst > 0 ) {
		bucket->rate.burst--;
	}
}

/*
================
SVC_Status

Responds with all the info that qplug or qspy can see about the server
and all connected players.  Used for getting detailed information after
the simple info query.
================
*/
static void SVC_Status( netadr_t from ) {
	char	player[1024];
	char	status[MAX_MSGLEN];
	int		i;
	client_t	*cl;
	playerState_t	*ps;
	int		statusLength;
	int		playerLength;
	char	infostring[MAX_INFO_STRING];

	// ignore if we are in single player
	if ( Cvar_VariableValue( "g_gametype" ) == GT_SINGLE_PLAYER || Cvar_VariableValue("ui_singlePlayerActive")) {
		return;
	}

	// Prevent using getstatus as an amplifier
	if ( SVC_RateLimitAddress( from, 10, 1000 ) ) {
		Com_DPrintf( "SVC_Status: rate limit from %s exceeded, dropping request\n",
			NET_AdrToString( from ) );
		return;
	}

	// Allow getstatus to be DoSed relatively easily, but prevent
	// excess outbound bandwidth usage when being flooded inbound
	if ( SVC_RateLimit( &outboundLeakyBucket, 10, 100 ) ) {
		Com_DPrintf( "SVC_Status: rate limit exceeded, dropping request\n" );
		return;
	}

	// A maximum challenge length of 128 should be more than plenty.
	if(strlen(Cmd_Argv(1)) > 128)
		return;

	strcpy( infostring, Cvar_InfoString( CVAR_SERVERINFO ) );

	// echo back the parameter to status. so master servers can use it as a challenge
	// to prevent timed spoofed reply packets that add ghost servers
	Info_SetValueForKey( infostring, "challenge", Cmd_Argv(1) );

	status[0] = 0;
	statusLength = 0;

	for (i=0 ; i < sv_maxclients->integer ; i++) {
		cl = &svs.clients[i];
		if ( cl->state >= CS_CONNECTED ) {
			ps = SV_GameClientNum( i );
			Com_sprintf (player, sizeof(player), "%i %i \"%s\"\n", 
				ps->persistant[PERS_SCORE], cl->ping, cl->name);
			playerLength = strlen(player);
			if (statusLength + playerLength >= sizeof(status) ) {
				break;		// can't hold any more
			}
			strcpy (status + statusLength, player);
			statusLength += playerLength;
		}
	}

	NET_OutOfBandPrint( NS_SERVER, from, "statusResponse\n%s\n%s", infostring, status );
}

/*
================
SVC_Info

Responds with a short info message that should be enough to determine
if a user is interested in a server to do a full status
================
*/
void SVC_Info( netadr_t from ) {
	int		i, count, humans;
	char	*gamedir;
	char	infostring[MAX_INFO_STRING];

	// ignore if we are in single player
	if ( Cvar_VariableValue( "g_gametype" ) == GT_SINGLE_PLAYER || Cvar_VariableValue("ui_singlePlayerActive")) {
		return;
	}

	// Prevent using getinfo as an amplifier
	if ( SVC_RateLimitAddress( from, 10, 1000 ) ) {
		Com_DPrintf( "SVC_Info: rate limit from %s exceeded, dropping request\n",
			NET_AdrToString( from ) );
		return;
	}

	// Allow getinfo to be DoSed relatively easily, but prevent
	// excess outbound bandwidth usage when being flooded inbound
	if ( SVC_RateLimit( &outboundLeakyBucket, 10, 100 ) ) {
		Com_DPrintf( "SVC_Info: rate limit exceeded, dropping request\n" );
		return;
	}

	/*
	 * Check whether Cmd_Argv(1) has a sane length. This was not done in the original Quake3 version which led
	 * to the Infostring bug discovered by Luigi Auriemma. See http://aluigi.altervista.org/ for the advisory.
	 */

	// A maximum challenge length of 128 should be more than plenty.
	if(strlen(Cmd_Argv(1)) > 128)
		return;

	// don't count privateclients
	count = humans = 0;
	for ( i = sv_privateClients->integer ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			count++;
			if (svs.clients[i].netchan.remoteAddress.type != NA_BOT) {
				humans++;
			}
		}
	}

	infostring[0] = 0;

	// echo back the parameter to status. so servers can use it as a challenge
	// to prevent timed spoofed reply packets that add ghost servers
	Info_SetValueForKey( infostring, "challenge", Cmd_Argv(1) );

	Info_SetValueForKey( infostring, "gamename", com_gamename->string );

#ifdef LEGACY_PROTOCOL
	if(com_legacyprotocol->integer > 0)
		Info_SetValueForKey(infostring, "protocol", va("%i", com_legacyprotocol->integer));
	else
#endif
		Info_SetValueForKey(infostring, "protocol", va("%i", com_protocol->integer));

	Info_SetValueForKey( infostring, "hostname", sv_hostname->string );
	Info_SetValueForKey( infostring, "mapname", sv_mapname->string );
	Info_SetValueForKey( infostring, "clients", va("%i", count) );
	Info_SetValueForKey(infostring, "g_humanplayers", va("%i", humans));
	Info_SetValueForKey( infostring, "sv_maxclients", 
		va("%i", sv_maxclients->integer - sv_privateClients->integer ) );
	Info_SetValueForKey( infostring, "gametype", va("%i", sv_gametype->integer ) );
	Info_SetValueForKey( infostring, "pure", va("%i", sv_pure->integer ) );
	Info_SetValueForKey(infostring, "g_needpass", va("%d", Cvar_VariableIntegerValue("g_needpass")));

#ifdef USE_VOIP
	if (sv_voipProtocol->string && *sv_voipProtocol->string) {
		Info_SetValueForKey( infostring, "voip", sv_voipProtocol->string );
	}
#endif

	if( sv_minPing->integer ) {
		Info_SetValueForKey( infostring, "minPing", va("%i", sv_minPing->integer) );
	}
	if( sv_maxPing->integer ) {
		Info_SetValueForKey( infostring, "maxPing", va("%i", sv_maxPing->integer) );
	}
	gamedir = Cvar_VariableString( "fs_game" );
	if( *gamedir ) {
		Info_SetValueForKey( infostring, "game", gamedir );
	}

	NET_OutOfBandPrint( NS_SERVER, from, "infoResponse\n%s", infostring );
}

/*
================
SVC_FlushRedirect

================
*/
static void SV_FlushRedirect( char *outputbuf ) {
	NET_OutOfBandPrint( NS_SERVER, svs.redirectAddress, "print\n%s", outputbuf );
}

/*
===============
SVC_RconText

The command an rcon packet carries, for the log, with the value hidden
when it sets a secret: a private cvar, or a password the game reads
===============
*/
static const char *SVC_RconText( void ) {
	static const char *setters[] = { "set", "seta", "sets", "setu", "toggle" };
	static char	text[MAX_STRING_CHARS];
	int			name = 2, i;

	for ( i = 0; i < ARRAY_LEN( setters ); i++ ) {
		if ( !Q_stricmp( Cmd_Argv( 2 ), setters[i] ) ) {
			name = 3;
		}
	}
	// g_password is the game's, and not private
	if ( Cmd_Argc() > name + 1 &&
		( ( Cvar_Flags( Cmd_Argv( name ) ) & ( CVAR_NONEXISTENT | CVAR_PRIVATE ) ) == CVAR_PRIVATE ||
		!Q_stricmp( Cmd_Argv( name ), "g_password" ) ) ) {
		Com_sprintf( text, sizeof( text ), "%s%s%s (value hidden)", name == 3 ? Cmd_Argv( 2 ) : "",
			name == 3 ? " " : "", Cmd_Argv( name ) );
		return text;
	}
	return Cmd_ArgsFrom( 2 );
}

/*
===============
SVC_RconRefused

Logs an rcon refused for a wrong password or the rate limit at most once a
second for each address, and ten times a second in all, with how many more
were refused in between, so guessing shows in the log without flooding it
===============
*/
static void SVC_RconRefused( leakyBucket_t *bucket, netadr_t from, const char *why ) {
	static rateLimit_t	noticeRate;
	static int			refusedUnlogged;	// with no bucket of their own
	int		*refused = bucket ? &bucket->refused : &refusedUnlogged;

	if ( bucket && SVC_RateLimit( &bucket->noticeRate, 1, 1000 ) ) {
		( *refused )++;
		return;
	}
	if ( SVC_RateLimit( &noticeRate, 10, 1000 ) ) {
		// the address's notice wasn't logged, so it may log the next
		if ( bucket && bucket->noticeRate.burst > 0 ) {
			bucket->noticeRate.burst--;
		}
		( *refused )++;
		return;
	}
	Com_Printf( "%s from %s: %s%s\n", why, NET_AdrToString( from ), SVC_RconText(),
		*refused ? va( " (and %i more refused since the last notice)", *refused ) : "" );
	*refused = 0;
}

#define	MAX_RCON_ALLOW	32

// sv_rconAllow's addresses, as it was last parsed
static struct {
	netadr_t	adr;
	int			mask;
} rconAllow[MAX_RCON_ALLOW];
static int		rconAllowCount;

/*
===============
SV_ParseRconAllow

Parses sv_rconAllow when it has changed, each frame, saying what it can't
use, so a name is looked up once, as the admin sets it, rather than for a
packet. When it has text but no address, rcon is refused to all rather
than allowed to any
===============
*/
static void SV_ParseRconAllow( void ) {
	char	list[MAX_STRING_CHARS];
	char	*entry;

	if ( !sv_rconAllow->modified ) {
		return;
	}
	sv_rconAllow->modified = qfalse;
	rconAllowCount = 0;

	Q_strncpyz( list, sv_rconAllow->string, sizeof( list ) );
	if ( strlen( sv_rconAllow->string ) >= sizeof( list ) ) {
		// a cut entry could name another address: leave it out whole
		char	*p, *last = list;

		for ( p = list; *p; p++ ) {
			if ( strchr( " ,;", *p ) ) {
				last = p;
			}
		}
		*last = '\0';
		Com_Printf( S_COLOR_YELLOW "WARNING: sv_rconAllow is longer than %i characters; "
			"the addresses after that are left out.\n", (int)sizeof( list ) - 1 );
	}
	for ( entry = strtok( list, " ,;" ); entry; entry = strtok( NULL, " ,;" ) ) {
		if ( rconAllowCount == MAX_RCON_ALLOW ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: sv_rconAllow lists more than %i addresses; "
				"%s and those after it are left out.\n", MAX_RCON_ALLOW, entry );
			break;
		}
		if ( SV_ParseCIDRNotation( &rconAllow[rconAllowCount].adr, &rconAllow[rconAllowCount].mask, entry ) ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: sv_rconAllow: %s isn't an address; left out.\n", entry );
			continue;
		}
		// "localhost" parses as the client's own loopback, which no packet
		// comes from: rcon from this machine comes from 127.0.0.0/8
		if ( rconAllow[rconAllowCount].adr.type == NA_LOOPBACK ) {
			NET_StringToAdr( "127.0.0.1", &rconAllow[rconAllowCount].adr, NA_IP );
			rconAllow[rconAllowCount].mask = 8;
		}
		rconAllowCount++;
	}
	if ( sv_rconAllow->string[0] && !rconAllowCount ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: sv_rconAllow lists no address it can use, "
			"so rcon is refused from everywhere.\n" );
	}
}

/*
===============
SVC_RconAllowed

Whether rcon may come from this address: any when sv_rconAllow is empty
===============
*/
static qboolean SVC_RconAllowed( netadr_t from ) {
	int		i;

	if ( !sv_rconAllow->string[0] ) {
		return qtrue;
	}
	for ( i = 0; i < rconAllowCount; i++ ) {
		if ( NET_CompareBaseAdrMask( rconAllow[i].adr, from, rconAllow[i].mask ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
===============
SVC_RemoteCommand

An rcon packet arrived from the network.
Shift down the remaining args
Redirect all printfs
===============
*/
static void SVC_RemoteCommand( netadr_t from, msg_t *msg ) {
	qboolean	valid;
	char		remaining[1024];
	// TTimo - scaled down to accumulate, but not overflow anything network wise, print wise etc.
	// (OOB messages are the bottleneck here)
#define SV_OUTPUTBUF_LENGTH (1024 - 16)
	char		sv_outputbuf[SV_OUTPUTBUF_LENGTH];
	char *cmd_aux;
	leakyBucket_t	*bucket = SVC_BucketForAddress( &rconBuckets, from, 10, 1000 );

	// Prevent using rcon as an amplifier and make dictionary attacks impractical
	if ( SVC_RateLimit( bucket ? &bucket->rate : NULL, 10, 1000 ) ) {
		SVC_RconRefused( bucket, from, "Rcon over the rate limit, dropped" );
		return;
	}

	// no answer, so an address left out learns nothing
	if ( !SVC_RconAllowed( from ) ) {
		SVC_RconRefused( bucket, from, "Rcon outside sv_rconAllow, dropped" );
		return;
	}

	if ( !strlen( sv_rconPassword->string ) ||
		strcmp (Cmd_Argv(1), sv_rconPassword->string) ) {
		static rateLimit_t badRate;

		// Make DoS via rcon impractical
		if ( SVC_RateLimit( &badRate, 10, 1000 ) ) {
			SVC_RconRefused( bucket, from, "Rcon over the rate limit, dropped" );
			return;
		}

		valid = qfalse;
		SVC_RconRefused( bucket, from, "Bad rcon" );
	} else {
		valid = qtrue;
		Com_Printf ("Rcon from %s: %s\n", NET_AdrToString (from), SVC_RconText() );
	}

	// start redirecting all print outputs to the packet
	svs.redirectAddress = from;
	Com_BeginRedirect (sv_outputbuf, SV_OUTPUTBUF_LENGTH, SV_FlushRedirect);

	if ( !strlen( sv_rconPassword->string ) ) {
		Com_Printf ("No rconpassword set on the server.\n");
	} else if ( !valid ) {
		Com_Printf ("Bad rconpassword.\n");
	} else {
		remaining[0] = 0;
		
		// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=543
		// get the command directly, "rcon <pass> <command>" to avoid quoting issues
		// extract the command by walking
		// since the cmd formatting can fuckup (amount of spaces), using a dumb step by step parsing
		cmd_aux = Cmd_Cmd();
		cmd_aux+=4;
		while(cmd_aux[0]==' ')
			cmd_aux++;
		while(cmd_aux[0] && cmd_aux[0]!=' ') // password
			cmd_aux++;
		while(cmd_aux[0]==' ')
			cmd_aux++;
		
		Q_strcat( remaining, sizeof(remaining), cmd_aux);
		
		Cmd_ExecuteString (remaining);

	}

	Com_EndRedirect ();
}

/*
=================
SV_ConnectionlessPacket

A connectionless packet has four leading 0xff
characters to distinguish it from a game channel.
Clients that are in the game can still send
connectionless packets.
=================
*/
static void SV_ConnectionlessPacket( netadr_t from, msg_t *msg ) {
	char	*s;
	char	*c;
	qboolean	connectCounted = qfalse;

	MSG_BeginReadingOOB( msg );
	MSG_ReadLong( msg );		// skip the -1 marker

	if ( msg->cursize >= 12 && !memcmp( "connect ", &msg->data[4], 8 ) ) {
		// the userinfo is at most MAX_INFO_STRING, so anything
		// longer isn't worth decompressing
		if ( msg->cursize > MAX_INFO_STRING * 2 ) {
			Com_DPrintf( "%s: connect packet is too long, %i bytes\n",
				NET_AdrToString( from ), msg->cursize );
			return;
		}
		// Prevent using connect as an amplifier; SV_DirectConnect
		// gives the request back once the challenge checks out
		if ( !NET_IsLocalAddress( from ) && SVC_RateLimitAddress( from, 10, 1000 ) ) {
			Com_DPrintf( "SV_ConnectionlessPacket: connect rate limit from %s exceeded, dropping request\n",
				NET_AdrToString( from ) );
			return;
		}
		connectCounted = qtrue;
		Huff_Decompress(msg, 12);
	}

	s = MSG_ReadStringLine( msg );
	Cmd_TokenizeString( s );

	c = Cmd_Argv(0);
	Com_DPrintf ("SV packet %s : %s\n", NET_AdrToString(from), c);

	if (!Q_stricmp(c, "getstatus")) {
		SVC_Status( from );
  } else if (!Q_stricmp(c, "getinfo")) {
		SVC_Info( from );
	} else if (!Q_stricmp(c, "getchallenge")) {
		SV_GetChallenge(from);
	} else if (!Q_stricmp(c, "connect")) {
		// a connect that isn't compressed, or is spelled another way,
		// is limited the same
		if ( !connectCounted && !NET_IsLocalAddress( from ) &&
			SVC_RateLimitAddress( from, 10, 1000 ) ) {
			Com_DPrintf( "SV_ConnectionlessPacket: connect rate limit from %s exceeded, dropping request\n",
				NET_AdrToString( from ) );
			return;
		}
		SV_DirectConnect( from );
#ifndef STANDALONE
	} else if (!Q_stricmp(c, "ipAuthorize")) {
		SV_AuthorizeIpPacket( from );
#endif
	} else if (!Q_stricmp(c, "rcon")) {
		SVC_RemoteCommand( from, msg );
	} else if (!Q_stricmp(c, "disconnect")) {
		// if a client starts up a local server, we may see some spurious
		// server disconnect messages when their new server sees our final
		// sequenced messages to the old client
	} else {
		Com_DPrintf ("bad connectionless packet from %s:\n%s\n",
			NET_AdrToString (from), s);
	}
}

//============================================================================

/*
=================
SV_ClientPacket

A client's packet has passed the netchan's checks
=================
*/
static void SV_ClientPacket( client_t *cl, msg_t *msg ) {
	// zombie clients still need to do the Netchan_Process
	// to make sure they don't need to retransmit the final
	// reliable message, but they don't do any other processing
	if ( cl->state != CS_ZOMBIE ) {
		cl->lastPacketTime = svs.time;	// don't timeout
		SV_ExecuteClientMessage( cl, msg );
	}
}

/*
=================
SV_PacketEvent
=================
*/
void SV_PacketEvent( netadr_t from, msg_t *msg ) {
	int			i;
	client_t	*cl;
	int			qport;

	// too short for a sequence number and qport, and for any
	// connectionless command
	if ( msg->cursize < 6 ) {
		return;
	}

	// check for connectionless packet (0xffffffff) first
	if ( *(int *)msg->data == -1) {
		SV_ConnectionlessPacket( from, msg );
		return;
	}

	// read the qport out of the message so we can fix up
	// stupid address translating routers
	MSG_BeginReadingOOB( msg );
	MSG_ReadLong( msg );				// sequence number
	qport = MSG_ReadShort( msg ) & 0xffff;

	// it is possible to have multiple clients from a single IP
	// address, so they are differentiated by the qport variable.
	// A client whose address, port and qport all match gets the
	// packet
	for (i=0, cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( cl->state != CS_FREE && cl->netchan.qport == qport &&
			NET_CompareAdr( from, cl->netchan.remoteAddress ) ) {
			// make sure it is a valid, in sequence packet
			if ( SV_Netchan_Process( cl, msg ) ) {
				SV_ClientPacket( cl, msg );
			}
			return;
		}
	}

	// the IP port can't be used to differentiate them, because
	// some address translating routers periodically change UDP
	// port assignments. Try each client with this address and
	// qport, and move the one whose packet checks out to the new
	// port, so a packet that doesn't can't redirect a client's
	// messages
	for (i=0, cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( cl->state == CS_FREE || cl->netchan.qport != qport ||
			!NET_CompareBaseAdr( from, cl->netchan.remoteAddress ) ) {
			continue;
		}

		if ( SV_Netchan_Process( cl, msg ) ) {
			Com_Printf( "SV_PacketEvent: fixing up a translated port for client %i: %i to %i\n",
				i, (unsigned short)BigShort( cl->netchan.remoteAddress.port ),
				(unsigned short)BigShort( from.port ) );
			cl->netchan.remoteAddress.port = from.port;
			SV_ClientPacket( cl, msg );
			return;
		}
	}
}


/*
===================
SV_CalcPings

Updates the cl->ping variables
===================
*/
static void SV_CalcPings( void ) {
	int			i, j;
	client_t	*cl;
	int			total, count;
	int			delta;
	playerState_t	*ps;

	for (i=0 ; i < sv_maxclients->integer ; i++) {
		cl = &svs.clients[i];
		if ( cl->state != CS_ACTIVE ) {
			cl->ping = 999;
			continue;
		}
		if ( !cl->gentity ) {
			cl->ping = 999;
			continue;
		}
		if ( cl->gentity->r.svFlags & SVF_BOT ) {
			cl->ping = 0;
			continue;
		}

		total = 0;
		count = 0;
		for ( j = 0 ; j < PACKET_BACKUP ; j++ ) {
			if ( cl->frames[j].messageAcked <= 0 ) {
				continue;
			}
			delta = cl->frames[j].messageAcked - cl->frames[j].messageSent;
			count++;
			total += delta;
		}
		if (!count) {
			cl->ping = 999;
		} else {
			cl->ping = total/count;
			if ( cl->ping > 999 ) {
				cl->ping = 999;
			}
		}

		// let the game dll know about the ping
		ps = SV_GameClientNum( i );
		ps->ping = cl->ping;
	}
}

#define SV_SILENT_CONNECT_TIME	4000	// msec a new client has to send its first packet

/*
==================
SV_CheckTimeouts

If a packet has not been received from a client for timeout->integer 
seconds, drop the conneciton.  Server time is used instead of
realtime to avoid dropping the local client while debugging.

When a client is normally dropped, the client_t goes into a zombie state
for a few seconds to make sure any final reliable message gets resent
if necessary
==================
*/
static void SV_CheckTimeouts( void ) {
	int		i;
	client_t	*cl;
	int			droppoint;
	int			zombiepoint;

	droppoint = svs.time - 1000 * sv_timeout->integer;
	zombiepoint = svs.time - 1000 * sv_zombietime->integer;

	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		// message times may be wrong across a changelevel
		if (cl->lastPacketTime > svs.time) {
			cl->lastPacketTime = svs.time;
		}

		if (cl->state == CS_ZOMBIE
		&& cl->lastPacketTime < zombiepoint) {
			// using the client id cause the cl->name is empty at this point
			Com_DPrintf( "Going from CS_ZOMBIE to CS_FREE for client %d\n", i );
			cl->state = CS_FREE;	// can now be reused
			continue;
		}
		// a client sends packets from the moment it connects, so one
		// that has sent none since is holding the slot without playing.
		// Free it quietly, well before the timeout
		if ( cl->state == CS_CONNECTED && cl->netchan.incomingSequence == 0 &&
			cl->netchan.remoteAddress.type != NA_BOT &&
			cl->netchan.remoteAddress.type != NA_LOOPBACK &&
			svs.time - cl->lastConnectTime > SV_SILENT_CONNECT_TIME ) {
			Com_DPrintf( "Freeing client %d, which sent nothing after connecting\n", i );
			SV_DropClient( cl, NULL );
			cl->state = CS_FREE;	// don't bother with zombie state
			continue;
		}
		// a bot sends no packets: it's marked alive only as the game reads
		// its console messages, which it doesn't while bot_pause is set
		if ( cl->state >= CS_CONNECTED && cl->netchan.remoteAddress.type != NA_BOT &&
			cl->lastPacketTime < droppoint ) {
			// wait several frames so a debugger session doesn't
			// cause a timeout
			if ( ++cl->timeoutCount > 5 ) {
				SV_DropClient (cl, "timed out"); 
				cl->state = CS_FREE;	// don't bother with zombie state
			}
		} else {
			cl->timeoutCount = 0;
		}
	}
}


/*
==================
SV_CheckPaused
==================
*/
static qboolean SV_CheckPaused( void ) {
	if ( !cl_paused->integer ) {
		return qfalse;
	}

	// only pause if there is just a single client connected
	if ( SV_HumanCount() > 1 ) {
		// don't pause
		if (sv_paused->integer)
			Cvar_Set("sv_paused", "0");
		return qfalse;
	}

	if (!sv_paused->integer)
		Cvar_Set("sv_paused", "1");
	return qtrue;
}

/*
==================
SV_FrameMsec
Return time in millseconds until processing of the next server frame.
==================
*/
int SV_FrameMsec(void)
{
	if(sv_fps)
	{
		int frameMsec;
		
		frameMsec = SV_FrameLength();
		
		if(frameMsec < sv.timeResidual)
			return 0;
		else
			return frameMsec - sv.timeResidual;
	}
	else
		return 1;
}

/*
==================
SV_HumanCount

The players connected, bots aside
==================
*/
int SV_HumanCount( void ) {
	int	count = 0, i;

	if ( !svs.clients ) {
		return 0;	// no map yet: --check's
	}
	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		count += svs.clients[i].state >= CS_CONNECTED && svs.clients[i].netchan.remoteAddress.type != NA_BOT;
	}
	return count;
}

/*
==================
SV_KeepAwake

A dedicated server keeps its system from sleeping while people play on
it, bots aside, and lets it sleep again when they've gone, checked once
a second (Sys_KeepAwake, macOS's power assertion)
==================
*/
void SV_KeepAwake( void ) {
	if ( !com_dedicated->integer || svs.time < svs.nextKeepAwakeTime ) {
		return;
	}
	svs.nextKeepAwakeTime = svs.time + 1000;
	Sys_KeepAwake( SV_HumanCount() > 0 );
}

/*
==================
SV_FrameLength

How long a server frame lasts, in milliseconds
==================
*/
int SV_FrameLength( void ) {
	return sv_fps && sv_fps->integer > 0 ? 1000 / sv_fps->integer : 1;
}

/*
==================
SV_FrameRateChanged

At the start, and when sv_fps changes. Frames are whole milliseconds, so a
rate that doesn't divide 1000 runs a little faster than set: say what it
runs, and the nearest rates that run exactly. Clients' snapshot rates,
which sv_fps caps, follow it.
==================
*/
static void SV_FrameRateChanged( void ) {
	int		fps = sv_fps->integer;
	int		frameMsec = 1000 / fps;
	int		below, above;
	client_t	*cl;
	int		i;

	if ( 1000 % fps ) {
		for ( below = fps - 1; 1000 % below; below-- ) {
		}
		for ( above = fps + 1; 1000 % above; above++ ) {
		}
		Com_Printf( "sv_fps %i runs %i ms frames, %g a second; %i or %i run exactly as set\n",
			fps, frameMsec, floor( 10000.0 / frameMsec ) / 10, below, above );
	}

	for ( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if ( cl->state >= CS_CONNECTED ) {
			SV_UpdateSnapshotRate( cl );
		}
	}
}

/*
==================
SV_Frame

Player movement occurs as a result of packet events, which
happen before SV_Frame is called
==================
*/
void SV_Frame( int msec ) {
	int		frameMsec;
	int		startTime;

	// the menu kills the server with this cvar
	if ( sv_killserver->integer ) {
		SV_Shutdown ("Server was killed");
		Cvar_Set( "sv_killserver", "0" );
		return;
	}

	if (!com_sv_running->integer)
	{
		// Running as a server, but no map loaded: a dedicated server
		// given none starts one
		if ( SV_StartDefaultMap() )
			return;

		// --check loads none: its report, once its startup's done
		if ( com_check ) {
			SV_FinishStartup();
			return;
		}

#ifdef DEDICATED
		// Block until something interesting happens, unless commands wait
		// to run, a config's after a wait, say
		if ( Cbuf_Empty() )
			Sys_Sleep(-1);
#endif

		return;
	}

	SV_FinishStartup();
	SV_KeepAwake();
	// before the pause, as rcon is taken while paused
	SV_ParseRconAllow();

	// allow pause if only the local client is connected
	if ( SV_CheckPaused() ) {
		return;
	}

	if ( sv_fps->modified ) {
		sv_fps->modified = qfalse;
		SV_FrameRateChanged();
	}

	// if it isn't time for the next frame, do nothing
	frameMsec = SV_FrameLength() * com_timescale->value;
	// don't let it scale below 1ms
	if(frameMsec < 1)
	{
		Cvar_Set("timescale", va("%f", sv_fps->integer / 1000.0f));
		frameMsec = 1;
	}

	sv.timeResidual += msec;

	if (!com_dedicated->integer) SV_BotFrame (sv.time + sv.timeResidual);

	// if time is about to hit the 32nd bit, kick all clients
	// and clear sv.time, rather
	// than checking for negative time wraparound everywhere.
	// 2giga-milliseconds = 23 days, so it won't be too often
	if ( svs.time > 0x70000000 ) {
		SV_Shutdown( "Restarting server due to time wrapping" );
		Cbuf_AddText( va( "map %s\n", Cvar_VariableString( "mapname" ) ) );
		return;
	}

	if( sv.restartTime && sv.time >= sv.restartTime ) {
		sv.restartTime = 0;
		Cbuf_AddText( "map_restart 0\n" );
		return;
	}

	// update infostrings if anything has been changed
	if ( cvar_modifiedFlags & CVAR_SERVERINFO ) {
		SV_SetConfigstring( CS_SERVERINFO, Cvar_InfoString( CVAR_SERVERINFO ) );
		cvar_modifiedFlags &= ~CVAR_SERVERINFO;
	}
	if ( cvar_modifiedFlags & CVAR_SYSTEMINFO ) {
		SV_SetConfigstring( CS_SYSTEMINFO, Cvar_InfoString_Big( CVAR_SYSTEMINFO ) );
		cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;
	}

	if ( com_speeds->integer ) {
		startTime = Sys_Milliseconds ();
	} else {
		startTime = 0;	// quite a compiler warning
	}

	// update ping based on the all received frames
	SV_CalcPings();

	if (com_dedicated->integer) SV_BotFrame (sv.time);

	// run the game simulation in chunks
	while ( sv.timeResidual >= frameMsec ) {
		sv.timeResidual -= frameMsec;
		svs.time += frameMsec;
		sv.time += frameMsec;

		// let everything in the world think and move
		VM_Call (gvm, GAME_RUN_FRAME, sv.time);
	}

	if ( com_speeds->integer ) {
		time_game = Sys_Milliseconds () - startTime;
	}

	// check timeouts
	SV_CheckTimeouts();

	// send messages back to the clients
	SV_SendClientMessages();

	// send a heartbeat to the master if needed
	SV_MasterHeartbeat(HEARTBEAT_FOR_MASTER);
}

/*
====================
SV_RateMsec

Return the number of msec until another message can be sent to
a client based on its rate settings
====================
*/

#define UDPIP_HEADER_SIZE 28
#define UDPIP6_HEADER_SIZE 48

int SV_RateMsec(client_t *client)
{
	int rate, rateMsec;
	int messageSize;
	
	messageSize = client->netchan.lastSentSize;
	rate = client->rate;

	if(sv_maxRate->integer)
	{
		if(sv_maxRate->integer < 1000)
			Cvar_Set( "sv_MaxRate", "1000" );
		if(sv_maxRate->integer < rate)
			rate = sv_maxRate->integer;
	}

	if(sv_minRate->integer)
	{
		if(sv_minRate->integer < 1000)
			Cvar_Set("sv_minRate", "1000");
		if(sv_minRate->integer > rate)
			rate = sv_minRate->integer;
	}

	if(client->netchan.remoteAddress.type == NA_IP6)
		messageSize += UDPIP6_HEADER_SIZE;
	else
		messageSize += UDPIP_HEADER_SIZE;
		
	rateMsec = messageSize * 1000 / ((int) (rate * com_timescale->value));
	rate = Sys_Milliseconds() - client->netchan.lastSentTime;
	
	if(rate > rateMsec)
		return 0;
	else
		return rateMsec - rate;
}

/*
====================
SV_SendQueuedPackets

Send download messages and queued packets in the time that we're idle, i.e.
not computing a server frame or sending client snapshots.
Return the time in msec until we expect to be called next
====================
*/

int SV_SendQueuedPackets(void)
{
	int numBlocks;
	int dlStart, deltaT, delayT;
	static int dlNextRound = 0;
	int timeVal = INT_MAX;

	// Send out fragmented packets now that we're idle
	delayT = SV_SendQueuedMessages();
	if(delayT >= 0)
		timeVal = delayT;

	if(sv_dlRate->integer)
	{
		// Rate limiting. This is very imprecise for high
		// download rates due to millisecond timedelta resolution
		dlStart = Sys_Milliseconds();
		deltaT = dlNextRound - dlStart;

		if(deltaT > 0)
		{
			if(deltaT < timeVal)
				timeVal = deltaT + 1;
		}
		else
		{
			numBlocks = SV_SendDownloadMessages();

			if(numBlocks)
			{
				// There are active downloads
				deltaT = Sys_Milliseconds() - dlStart;

				delayT = 1000 * numBlocks * MAX_DOWNLOAD_BLKSIZE;
				delayT /= sv_dlRate->integer * 1024;

				if(delayT <= deltaT + 1)
				{
					// Sending the last round of download messages
					// took too long for given rate, don't wait for
					// next round, but always enforce a 1ms delay
					// between DL message rounds so we don't hog
					// all of the bandwidth. This will result in an
					// effective maximum rate of 1MB/s per user, but the
					// low download window size limits this anyways.
					if(timeVal > 2)
						timeVal = 2;

					dlNextRound = dlStart + deltaT + 1;
				}
				else
				{
					dlNextRound = dlStart + delayT;
					delayT -= deltaT;

					if(delayT < timeVal)
						timeVal = delayT;
				}
			}
		}
	}
	else
	{
		if(SV_SendDownloadMessages())
			timeVal = 0;
	}

	return timeVal;
}
