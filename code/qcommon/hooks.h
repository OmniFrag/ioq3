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
// hooks.h -- engine extensions: code a build links into the client or the
// dedicated server, such as a stats exporter, a metrics endpoint or a
// control socket for local tools, which the engine calls at fixed points.
//
// An extension defines one engineExtension_t, named <name>_extension, and
// its CMake file calls add_engine_extension( <name> ... )
// (cmake/utils/engine_extension.cmake), which links it in and lists it in
// the executable's engineExtensions table. Every hook may be NULL. With no
// extension linked, the table is empty and nothing here changes what the
// engine does.
//
// The hooks run on the main thread, from the frame loop, and must not
// block: an extension that does I/O polls it, within a budget of its own.
#ifndef HOOKS_H
#define HOOKS_H

// the game's facts that reach the gameFact hook (server/sv_game.c's
// SV_GameFact refuses the rest)
#define MAX_GAME_FACT_NAME		15
#define MAX_GAME_FACTS_PER_FRAME	256

typedef struct {
	const char	*name;

	// Once, as the program starts: after the network, and on a dedicated
	// server once its port is open and its PID file written; never under
	// --check. Register cvars and commands here. Returns whether the
	// extension stays active: one that returns qfalse gets no other call.
	// An error raised here stops the program, as one in Com_Init does.
	qboolean	(*init)( void );

	// Once per frame, at the end of Com_Frame, whether or not a map is
	// loaded. Returns the most milliseconds a dedicated server with no
	// map may then block on console input before the next frame, or -1
	// for no limit: an extension polling I/O of its own asks for a few
	// frames a second. (A hibernating server's frames come every 250 ms
	// at most anyway.)
	int		(*frame)( void );

	// Once, as the program exits: on quit, a fatal error, or SIGTERM or
	// SIGINT. reason says why, as the server tells its players. The
	// network is still up. Not called on an extension whose own hook the
	// exit interrupts (a signal, or a quit the hook asks for), since its
	// state may be half changed. On Windows a console's Ctrl-C or close
	// stops the program from the console's own thread, which calls this
	// and serverShutdown while the main thread may still run a frame.
	void		(*shutdown)( const char *reason );

	// What the server does, dedicated or a listen server's, each called
	// once it has happened. clientNum runs from 0 to sv_maxclients - 1,
	// and the SV_Ext* accessors below read the client's details.

	// A client took a slot: a player the game let in, or a bot the game
	// added, whose userinfo, and so its name, the game sets right after
	void		(*clientConnected)( int clientNum );
	// A player entered the game, having loaded the map; and each bot
	// after a map change; and every client again on map_restart, before
	// mapRestarted, one still loading the map too. A bot the game adds
	// plays from the start
	void		(*clientBegin)( int clientNum );
	// A client's userinfo changed, its name with it, as the player sent
	// it or the game set it
	void		(*clientUserinfoChanged)( int clientNum );
	// A client is leaving: called while its slot still holds it, before
	// the game hears of it. reason is what the client and everyone are
	// told: "disconnected" when it left, "timed out", "was kicked", the
	// game's own reason or a fault's; "" for bots the game type's change
	// removes, NULL when nobody is told, "reconnected" for a client whose
	// new connection from the same address takes its slot, and "refused
	// by the game" for a bot the game added but then freed
	void		(*clientDropped)( int clientNum, const char *reason );

	// A map loaded, and its game started with the clients still connected
	void		(*mapSpawned)( void );
	// map_restart restarted the map's game, with every client
	void		(*mapRestarted)( void );
	// The server is stopping its map, its clients still in their slots:
	// a map that ended in an error (com_errorEntered is set), killserver,
	// or the program's exit. reason is what the clients are told
	void		(*serverShutdown)( const char *reason );
	// The game ran its frames for this server frame, up to levelTime
	void		(*gameFrameDone)( int levelTime );
	// A dedicated server with nobody playing froze its game (qtrue), or a
	// player, a map or an admin woke it, or it stopped asleep, just before
	// serverShutdown (qfalse)
	void		(*hibernation)( qboolean asleep );
	// The game module reported a fact through trap_GameFact (qcommon.h's
	// COM_TRAP_GAMEFACT): its name, 1 to MAX_GAME_FACT_NAME lowercase
	// letters, and an info string of its details, shorter than
	// MAX_INFO_STRING, which passed Info_Validate; at most
	// MAX_GAME_FACTS_PER_FRAME in one game frame. Called as the game makes
	// the call, from its init, its frames or a client's command, so before
	// the hooks above tell of the load or frame it came in. What the
	// details mean is the game's: treat each value as untrusted. A game
	// module that never calls it reports no facts
	void		(*gameFact)( const char *fact, const char *info );
	// A client's command that the engine passes to the game, a bot's too:
	// called just before the game runs it, while Cmd_Argc and Cmd_Argv
	// hold its words as the game will read them. text is the command as
	// the client sent it, valid only during the call. The engine's own
	// commands (userinfo, download, disconnect) aren't passed: the hooks
	// above report what they do
	void		(*clientCommand)( int clientNum, const char *text );
	// The game has run that command, which may have dropped the client.
	// What the game sent between the two calls (serverCommand), such as a
	// say's chat lines, answered it. An error that ends the frame inside
	// the game skips this call
	void		(*clientCommandDone)( int clientNum );
	// The game sent a command to a client, or to every client when
	// clientNum is -1: a line to print, a chat line, the scores
	void		(*serverCommand)( int clientNum, const char *text );
	// The program printed text: part of a line, a line or several, color
	// codes and all, but not what a redirect captures (rcon's replies).
	// What a print hook prints reaches no print hook. Called on the
	// thread that printed: the main thread, except when a Windows console
	// stops the program (shutdown, above). Every print pays for it, so
	// keep it cheap
	void		(*print)( const char *text );
} engineExtension_t;

// The executable's extensions, NULL-terminated, and the line naming them
// that the engine prints at startup, empty for none; generated by the
// build (engine_extensions.cmake)
extern const engineExtension_t *const engineExtensions[];
extern const char engineExtensionsLine[];

void	Hooks_Init( void );
void	Hooks_Frame( void );
// an ERR_DROP ended the frame, in a hook perhaps
void	Hooks_FrameAborted( void );
void	Hooks_Shutdown( const char *reason );
// a signal stops the program: before the server shuts down
void	Hooks_Signal( qboolean crashed );
// the longest a dedicated server with no map may wait, as the extensions
// asked in their last frames: -1 for no limit
int	Hooks_IdleMsec( void );

void	Hooks_ClientConnected( int clientNum );
void	Hooks_ClientBegin( int clientNum );
void	Hooks_ClientUserinfoChanged( int clientNum );
void	Hooks_ClientDropped( int clientNum, const char *reason );
void	Hooks_MapSpawned( void );
void	Hooks_MapRestarted( void );
void	Hooks_ServerShutdown( const char *reason );
void	Hooks_GameFrameDone( int levelTime );
void	Hooks_Hibernation( qboolean asleep );
void	Hooks_GameFact( const char *fact, const char *info );
void	Hooks_ClientCommand( int clientNum, const char *text );
void	Hooks_ClientCommandDone( int clientNum );
void	Hooks_ServerCommand( int clientNum, const char *text );
void	Hooks_Print( const char *text );

// A client of the server, for extensions (server/sv_main.c, sv_game.c).
// Each gives its "none" for a number out of range, a slot nobody holds or
// one whose client has just left, and while no map runs.
typedef enum {
	EXT_CLIENT_FREE,		// nobody
	EXT_CLIENT_CONNECTED,		// connected, loading the map
	EXT_CLIENT_ACTIVE		// in the game
} extClientState_t;

extClientState_t	SV_ExtClientState( int clientNum );
// the name its userinfo gives, color codes and all; "" for none
const char	*SV_ExtClientName( int clientNum );
// its IP address, without the port: "localhost" for the server's own
// machine, "bot" for a bot, "" for none; NET_AdrToString's buffer, which
// its next call, this one's too, overwrites
const char	*SV_ExtClientAddress( int clientNum );
qboolean	SV_ExtClientIsBot( int clientNum );
// its ping in milliseconds, as status and getstatus give it; 999 while it
// loads the map, 0 for a bot or none
int		SV_ExtClientPing( int clientNum );
// its score as getstatus reads it, the game's PERS_SCORE, which every mod
// keeps for server browsers; qfalse with no game running. A bot's, in
// clientConnected and the userinfo change the game makes right after, is
// still the slot's last client's: the game sets the bot up after both
qboolean	SV_ExtClientScore( int clientNum, int *score );
// drops a client, telling it and everyone reason, as clientkick does
// with "was kicked"; qfalse for none, or a listen server's own player,
// whom clientkick won't drop either
qboolean	SV_ExtDropClient( int clientNum, const char *reason );
// mutes a client, as clientmute does: its chat (say, say_team, tell, the
// voice chats and orders) reaches neither the game nor anyone, nor does
// its voice, for seconds or, with 0, until it leaves; a mute longer than
// EXT_MUTE_MAX_SECONDS lasts until it leaves. With mute qfalse, lifts the
// mute. Muting a muted client sets its time anew. The client is told as
// its mute begins and ends. qfalse for none
#define EXT_MUTE_MAX_SECONDS	( 7 * 24 * 60 * 60 )	// a week: Sys_Milliseconds counts it
qboolean	SV_ExtMuteClient( int clientNum, qboolean mute, int seconds );
// whether it's muted now: by an extension or the console, its time not run
// out
qboolean	SV_ExtClientMuted( int clientNum );

// the reason a draining dedicated server refuses new players with (drain),
// or "" while it isn't draining. A drain ends in the program's exit once
// its players have left or its map ends, or with undrain
const char	*SV_ExtDrainReason( void );

// The server's address bans and their exceptions (banaddr, exceptaddr),
// which sv_banFile keeps. Indexes run from 0 to SV_ExtBanCount() - 1, and
// move as entries come and go.
typedef struct {
	char		address[NET_ADDRSTRMAXLEN];	// without the prefix: "192.168.1.0", "fe80::"
	int		subnet;				// the prefix's bits: 32, or 128 for IPv6, for one address
	qboolean	exception;			// an exception to the bans, not a ban
} extBan_t;

int		SV_ExtBanCount( void );
qboolean	SV_ExtGetBan( int index, extBan_t *ban );
// changes whenever the list does: by the console, rcon, an extension, or
// the file read again (rehashbans)
int		SV_ExtBanListChanges( void );
// Adds a ban, or an exception, as banaddr and exceptaddr do: of an IPv4
// or IPv6 address written as numbers, or a range in CIDR notation
// ("192.168.1.0/24"), never a host name, which would be looked up. The
// entries it supersedes go, and the list is saved. Returns the new entry's
// index, or one of the EXT_BAN_* below with why as banaddr prints it, and
// *covering the index of an entry that already supersedes it, or -1.
// Nobody connected is dropped: SV_ExtDropBanned does that
int		SV_ExtAddBan( const char *address, qboolean exception, int *covering, char *why, int whySize );
#define EXT_BAN_INVALID		-1	// not an address, or a prefix out of range
#define EXT_BAN_REFUSED		-2	// an entry supersedes it, the list is full, or a ban supersedes the exception
// removes an entry, and saves the list
qboolean	SV_ExtRemoveBan( int index );
// drops every client the bans would refuse to let in now, telling each
// and everyone reason; returns how many
int		SV_ExtDropBanned( const char *reason );

#endif
