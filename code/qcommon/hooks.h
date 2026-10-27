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

// where in the client's frame the draw hook is called (cl_scrn.c)
typedef enum {
	EXT_DRAW_GAME,	// after cgame's frame, or the ui's connect screen while connecting
	EXT_DRAW_MENU,	// after the ui's menu, every frame, whether or not one is up
	EXT_DRAW_TOP	// after the console, before the debug graph
} extDrawPoint_t;

// the client's game modules
typedef enum {
	EXT_MODULE_CGAME,
	EXT_MODULE_UI,
	EXT_NUM_MODULES
} extModule_t;

#define EXT_MODULE_BIT( module )	( 1 << ( module ) )

// An extension call for game modules: a key they find with trap_GetValue
// (qcommon.h's COM_TRAP_GETVALUE), answered with trap, a syscall number
// from COM_TRAP_EXTENSION_FIRST to COM_TRAP_EXTENSION_LAST, which the
// modules in the modules bits call. The engine refuses to start when two
// extensions' calls share a number or a key, or a number is out of range.
typedef struct {
	const char	*key;		// trap_<Name>_OF (docs/contracts.md, rule 3)
	int		trap;
	int		modules;	// EXT_MODULE_BIT()s
} extModuleCall_t;

// what a module registered (moduleAsset)
typedef enum {
	EXT_ASSET_SHADER,
	EXT_ASSET_SHADER_NOMIP,
	EXT_ASSET_MODEL,
	EXT_ASSET_SKIN,
	EXT_ASSET_FONT,
	EXT_ASSET_IMAGE2D		// an image it made (trap_R_CreateImage2D_OF), named as the engine names it
} extAssetKind_t;

// a game module's renderer call (moduleRender), defined in client/cl_ext.h
// with the renderer's types
struct extRenderCall_s;

typedef struct {
	const char	*name;

	// Once, as the program starts: after the network, and on a dedicated
	// server once its port is open and its PID file written; never under
	// --check. A client's renderer has started, its sound and ui not yet,
	// so the ui's first start finds the extensions' calls. Register cvars
	// and commands here. Returns whether the extension stays active: one
	// that returns qfalse gets no other call.
	// An error raised here stops the program, as one in Com_Init does.
	qboolean	(*init)( void );

	// Once per frame, at the end of Com_Frame, whether or not a map is
	// loaded. Returns the most milliseconds a dedicated server with no
	// map may then block on console input before the next frame, or -1
	// for no limit: an extension polling I/O of its own asks for a few
	// frames a second. (A hibernating server's frames come every 250 ms
	// at most anyway.)
	int		(*frame)( void );

	// Once, as the program exits: on quit, a fatal error, or SIGTERM,
	// SIGINT or SIGHUP (on Windows, a console's Ctrl-C or close), which
	// quit from the main loop as quit does. reason says why, as the
	// server tells its players. The network is still up. Not called on
	// an extension whose own hook asks for the quit, since its state may
	// be half changed, nor after a crash.
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
	// a map that ended in an error, killserver, the program's exit, or a
	// listen server's own player disconnecting. Com_Error sets
	// com_errorEntered for the error and the disconnect alike, and the
	// com_errorCode cvar to its errorParm_t, which tells them apart.
	// reason is what the clients are told
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

	// What the client does, called in a client build only, at the start
	// of the client's frame for what changed since the last: so a state
	// the client passes within one frame, as CA_LOADING usually is, isn't
	// reported. The CL_Ext* accessors below read the details.

	// The connection's state changed: CA_DISCONNECTED on the menu,
	// CA_CONNECTING to CA_CONNECTED while it connects, CA_LOADING and
	// CA_PRIMED as the map loads, CA_ACTIVE in the game, CA_CINEMATIC
	// for a cinematic. A demo plays through the same states
	void		(*clientStateChanged)( connstate_t state );
	// A demo started playing (qtrue) or stopped (qfalse): called before
	// clientStateChanged when both changed
	void		(*demoPlayback)( qboolean playing );

	// The client's renderer, frame and input, in a client build only. The
	// renderer's table and window are client/cl_ext.h's CL_ExtRenderer and
	// CL_ExtGlconfig.

	// The renderer has started, after CL_InitRenderer registered the
	// client's own shaders; every handle from before is gone. On every map
	// load and vid_restart, and once as the client starts
	void		(*rendererStarted)( void );
	// The renderer is about to stop, the window kept (a map load, a
	// vid_restart on the web) or not (quit, a native vid_restart). Handles
	// are valid until this returns
	void		(*rendererStopping)( qboolean destroyWindow );
	// Draw now, at this point of the frame, between the renderer's
	// BeginFrame and EndFrame; once for each stereo field, stereo being its
	// stereoFrame_t (tr_types.h). Each point is called every frame the
	// client draws: EXT_DRAW_GAME only while connecting, loading and in the
	// game, and not while the ui covers the screen (UI_IS_FULLSCREEN)
	void		(*draw)( extDrawPoint_t point, int stereo );
	// Input, before the ui, cgame and binds see it, and never while the
	// console or the chat line has the keyboard. Return qtrue to take the
	// event: nothing else sees it, and the up of a key whose down an
	// extension took goes to that extension alone, wherever the keyboard
	// has gone since. Keys are keyNum_t; the console key and Alt+Enter are
	// the engine's
	qboolean	(*keyEvent)( int key, qboolean down, unsigned time );
	// a character as UTF-32, before any mapping to the modules' bytes
	qboolean	(*charEvent)( int codepoint );
	// an input method's composition, while an extension's field has focus
	// (CL_ExtSetTextFocus): its text in UTF-8, and the caret and the length
	// of its selection in characters, -1 where the input method gives
	// none, as SDL passes them; "" when it ends
	qboolean	(*textEditing)( const char *utf8, int cursor, int selection );
	// the pointer's position in pixels, while an extension has asked for a
	// cursor (CL_ExtSetPointerMode)
	qboolean	(*pointerEvent)( float x, float y );
	// the mouse's relative motion, as the ui, cgame or the view would get it
	qboolean	(*mouseEvent)( int dx, int dy );

	// Extension calls for the client's game modules. moduleCalls ends with
	// a NULL key; moduleCall answers a module's call of one of them with
	// its arguments (args[0] is the number, args[1] on the module's), checked
	// with qcommon.h's VMA_ and VMV_ macros, whose errors name the call by
	// its key. Linked modules and QVMs both call it
	const extModuleCall_t	*moduleCalls;
	intptr_t	(*moduleCall)( extModule_t module, int trap, intptr_t *args );
	// A module is shutting down, its own shutdown run: a map's end, a
	// vid_restart, a disconnect, the ui's restart, quit
	void		(*moduleShutdown)( extModule_t module );

	// The modules' renderer calls, as they make them. moduleAsset tells of
	// each shader, model, skin and font a module registers, with the handle
	// the renderer gave it (0 for none) and, for a font, the glyphs the
	// renderer filled in; and of each image it makes, each time, the same
	// one again when it's given back. moduleRender sees each 2D and scene
	// call (cl_ext.h's extRenderKind_t) before the renderer does, and
	// returns qtrue to take it: the renderer then never gets it
	void		(*moduleAsset)( extModule_t module, extAssetKind_t kind, const char *name, qhandle_t handle,
				const fontInfo_t *font );
	qboolean	(*moduleRender)( extModule_t module, const struct extRenderCall_s *call );
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
// a crash stops the program: before the server shuts down
void	Hooks_Signal( void );
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
void	Hooks_ClientStateChanged( connstate_t state );
void	Hooks_DemoPlayback( qboolean playing );
void	Hooks_RendererStarted( void );
void	Hooks_RendererStopping( qboolean destroyWindow );
void	Hooks_Draw( extDrawPoint_t point, int stereo );
// whether an active extension has a draw hook, so ui_speeds times the
// draw points only then
qboolean	Hooks_WantDraw( void );
// a key's down: the number, from 1, of the extension that took it, which
// its repeats and its up go to (Hooks_KeyTo); 0 when none did
int	Hooks_KeyDown( int key, unsigned time );
void	Hooks_KeyTo( int taker, int key, qboolean down, unsigned time );
qboolean	Hooks_CharEvent( int codepoint );
void	Hooks_TextEditing( const char *utf8, int cursor, int selection );
void	Hooks_PointerEvent( float x, float y );
qboolean	Hooks_MouseEvent( int dx, int dy );
// a module's trap_GetValue for a key an extension answers: qtrue with its
// number written to value
qboolean	Hooks_ModuleValue( extModule_t module, const char *key, char *value, int valueSize );
// a module's syscall that no engine case took: qtrue with *result if an
// extension declared it for that module
qboolean	Hooks_ModuleCall( extModule_t module, intptr_t *args, intptr_t *result );
// the key of an extension's call, or NULL, for errors
const char	*Hooks_ModuleCallKey( int trap );
void	Hooks_ModuleShutdown( extModule_t module );
// whether an extension wants the modules' renderer calls, so the client
// skips building them when none does
qboolean	Hooks_WantModuleRender( void );
void	Hooks_ModuleAsset( extModule_t module, extAssetKind_t kind, const char *name, qhandle_t handle,
		const fontInfo_t *font );
qboolean	Hooks_ModuleRender( extModule_t module, const struct extRenderCall_s *call );
// a call each extension sees and none takes, whatever its moduleRender returns
void	Hooks_ModuleRenderSeen( extModule_t module, const struct extRenderCall_s *call );

// The client, for extensions, in a client build only (client/cl_main.c).
// Each reads the connection as it stands now.

connstate_t	CL_ExtState( void );
// the demo playing, by the name it was started with; "" while none plays
const char	*CL_ExtDemoName( void );
// the server's address and port, as NET_AdrToStringwPort gives it:
// "localhost" for the listen server this program runs, as
// SV_ExtClientAddress names its player; "" while disconnected or playing a
// demo. NET_AdrToStringwPort's buffer, which its next call, this one's
// too, overwrites
const char	*CL_ExtServerAddress( void );
// the server's info as the gamestate gave it (CS_SERVERINFO: mapname,
// sv_hostname, sv_maxclients and the game's keys, such as g_needpass);
// a demo's, as it was recorded; "" before a gamestate, and for one too
// long for Info_ValueForKey, which would drop the game
const char	*CL_ExtServerInfo( void );
// the player's client number on the server, as the gamestate gave it; a
// demo's recorder's; -1 before a gamestate
int		CL_ExtClientNum( void );
// the player state of the last snapshot, in the game (CA_ACTIVE): the
// player's own, or that of the player they follow, which ps->clientNum
// tells from CL_ExtClientNum. persistant[PERS_SCORE] is the score, which
// every mod keeps for server browsers; what the rest means is the
// game's. qfalse with none
qboolean	CL_ExtPlayerState( playerState_t *ps );
// the window's safe area in pixels, as x, y, width and height, clear of
// notches and the like: the whole window where the system reports none
void		CL_ExtSafeArea( int rect[4] );
// the key catchers up now: KEYCATCH_CONSOLE, _UI, _MESSAGE, _CGAME
// (keycodes.h)
int		CL_ExtKeyCatcher( void );
// Whether an extension shows a cursor: with qtrue the pointer is no longer
// held as view motion while the game reads it, and its position comes to
// the pointerEvent hook; and whether the extension draws that cursor
// itself where the pointer is, so the system's hides over the window, or
// the system's shows. qfalse gives the pointer back
void		CL_ExtSetPointerMode( qboolean cursor, qboolean drawn );
// Whether an extension's text field has focus, and where it is in pixels
// (x, y, width, height; NULL for anywhere), so text input runs, a phone's
// keyboard shows and an input method places its window there
void		CL_ExtSetTextFocus( qboolean focused, const int rect[4] );
// a configstring of the gamestate, "" for none or one out of range
const char	*CL_ExtConfigString( int index );
// the last snapshot's entities, at most max of them, in the game; returns
// how many, 0 with none
int		CL_ExtSnapshotEntities( entityState_t *out, int max );
// the running module's QVM checksum (Com_BlockChecksum of the file), 0 for
// linked or native code or none
int		CL_ExtModuleChecksum( extModule_t module );
// the file downloading, "" for none, with the bytes received and its size
// (0 while unknown)
const char	*CL_ExtDownloadState( int *count, int *size );

// the server's name as connect was given it, a demo's during playback, ""
// while disconnected
const char	*CL_ExtServerName( void );
// the server's message while connecting (full, banned, a bad password), ""
// for none and once the server has answered, with the challenge and
// connect packets sent
const char	*CL_ExtServerMessage( int *connectPackets );
// the latest snapshot's round trip in milliseconds, 999 with none and in a
// demo
int		CL_ExtPing( void );
// the latest snapshot's number, which counts the server's packets (any
// lost on the way among them), and its server time; 0 for both before one
// in the game
int		CL_ExtSnapshotNumber( int *serverTime );
// the last frame's real time in milliseconds, which a pause doesn't stop
int		CL_ExtFrameTime( void );
// the chat line: to whom, and what the player is typing there
typedef enum {
	EXT_CHAT_NONE,		// closed
	EXT_CHAT_ALL,		// messagemode
	EXT_CHAT_TEAM,		// messagemode2
	EXT_CHAT_TELL		// messagemode3 and 4: to the player aimed at or the last attacker
} extChatMode_t;
extChatMode_t	CL_ExtChatLine( const char **text );
// the microphone's level, 0 to 1; 0 without VoIP
float		CL_ExtVoipPower( void );
// the demo recording, "" for none, with its size so far
const char	*CL_ExtDemoRecording( int *bytes );
// the paks that couldn't be downloaded, a line each, with how many: the
// count goes on past the lines that fit
const char	*CL_ExtDownloadFailures( int *count );

// What a demo file holds, for a demo browser (client/cl_demoinfo.c)
#define MAX_DEMOINFO_CONFIGSTRINGS	8	// that a caller may ask for
#define MAX_DEMOINFO_STRING		128	// of each one's value kept

typedef enum {
	DEMOINFO_REFUSED,	// not a demo the client plays: its protocol, its place, no gamestate
	DEMOINFO_WALKING,	// the gamestate's fields are known; the rest is being read
	DEMOINFO_DONE		// all of it
} demoInfoState_t;

typedef struct {
	char		name[MAX_NAME_LENGTH];	// "" for a slot nobody holds
	int		team;			// the game's, from its configstring's "t"
	qboolean	bot;			// its configstring has a skill
} demoPlayer_t;

// ints and chars only, so a module's struct can share its layout (the UI
// service's call 835)
typedef struct {
	demoInfoState_t	state;
	int		protocol;		// from the name's .dm_NN
	int		size;			// the file's
	int		mtime;			// its date, in seconds from 1970, or its pak's; 0 unknown
	char		date[32];		// mtime in local time, "2026-10-27 13:05"; "" unknown
	// the first gamestate's
	char		map[MAX_QPATH];
	char		hostname[64];
	int		gametype;
	int		clientNum;		// the recorder's
	char		recorder[MAX_NAME_LENGTH];
	// the players, as the gamestate names them, then as they were at the
	// end once the walk is done
	demoPlayer_t	player[MAX_CLIENTS];
	int		players, bots;
	// once the walk is done: the length from the first snapshot to the
	// last, -1 when unknown (past the walk's limit); whether a message was
	// damaged, the length then being up to it; and the configstrings
	// asked for, as they were at the end
	int		lengthMsec;
	qboolean	damaged;
	char		configstrings[MAX_DEMOINFO_CONFIGSTRINGS][MAX_DEMOINFO_STRING];
} demoInfo_t;

// A demo, by its path from the game directory (demos/<name>.dm_NN, in a
// folder of demos/ too), as the file system finds it: at once the state
// and the fields its gamestate gives, and the rest once its walk is done,
// a slice a frame. configstrings names at most MAX_DEMOINFO_CONFIGSTRINGS,
// whose last values come in the same order (the game's scores, say),
// those set by a "cs" command (one long enough to need bcs never is). A
// demo of a protocol the client doesn't play, outside demos/, or without a
// gamestate it can read is refused, the protocol before anything is read.
// Results are kept, so asking each frame costs a lookup. The walks go on
// while demos are asked for, and stop a second after the last ask
demoInfoState_t	CL_ExtDemoInfo( const char *name, const int *configstrings, int numConfigstrings, demoInfo_t *info );

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

// A cvar, for extensions (qcommon/cvar.c): when a change to it takes
// effect, by the game's declarations or else its flags, as cvar help says
typedef enum {
	EXT_APPLIES_NOW,		// at once, or never for a read only cvar
	EXT_APPLIES_VID_RESTART,	// at vid_restart
	EXT_APPLIES_SND_RESTART,	// at snd_restart
	EXT_APPLIES_MAP,		// at the next map
	EXT_APPLIES_RESTART		// when the program starts again
} extCvarApplies_t;

extCvarApplies_t	Cvar_ExtApplies( const cvar_t *var );
// whether a settings screen may not show a cvar, which needn't exist: a
// secret (one cvar_dump leaves out), a protected one, or a CD key
qboolean	Cvar_ExtRefused( const char *var_name );

#endif
