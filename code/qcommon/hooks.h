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

// the game modules: the client's cgame and ui, and the server's game,
// which makes extension calls (moduleCalls) alone. What the client's alone
// have is sized and counted by EXT_NUM_CLIENT_MODULES
typedef enum {
	EXT_MODULE_CGAME,
	EXT_MODULE_UI,
	EXT_NUM_CLIENT_MODULES,
	EXT_MODULE_GAME = EXT_NUM_CLIENT_MODULES,
	EXT_NUM_MODULES
} extModule_t;

#define EXT_MODULE_BIT( module )	( 1 << ( module ) )

// a finger on a touch screen, from its down to its lift (the touchEvent
// hook, and the SE_TOUCH event that carries it)
typedef enum {
	EXT_TOUCH_DOWN,
	EXT_TOUCH_MOVE,
	EXT_TOUCH_UP,
	EXT_TOUCH_CANCEL	// the touch ended with no lift: the system took it, or the input was cleared
} extTouchPhase_t;

typedef struct {
	int		finger;		// from 1, the same from the finger's down to its end, and new for each touch
	extTouchPhase_t	phase;
	float		x, y;		// in the window's pixels
	float		pressure;	// 0 to 1, or 1 where the platform gives none (the web)
	float		density;	// pixels in each of the window's points, which the mouse moves in
	int64_t		time;		// when the platform saw it, by Sys_Nanoseconds
} extTouch_t;

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

// why the client left the server it was on (clientLeft), as the client
// knows: only a leave that ends at the menu with no wait to go back
typedef enum {
	EXT_LEFT_OWN,			// the player's own disconnect
	EXT_LEFT_DROPPED,		// the server's disconnect, for its reason
	EXT_LEFT_KICKED,		// the server's disconnect, which it said was a kick
	EXT_LEFT_BANNED,		// the same, a ban
	EXT_LEFT_SHUTDOWN,		// the same, the server stopping
	EXT_LEFT_REFUSED,		// the server turned the connect down
	EXT_LEFT_LOST,			// the transport lost the connection, with nothing to go back by
	EXT_LEFT_UNREACHED,		// a connect the server never answered ended: it never connected
	EXT_LEFT_REPLACED,		// another connection took its place
	EXT_LEFT_GAVE_UP_LOST,	// the wait to rejoin after a lost connection gave up
	EXT_LEFT_GAVE_UP_AWAY,	// the same, the connection lost while the player was away
	EXT_LEFT_GAVE_UP_BACK,	// the wait for a restarting server to come back gave up
	EXT_LEFT_CANCELED,		// the player ended a wait to rejoin or for the server to come back
	EXT_LEFT_ERROR,			// any other error that ended the game
	EXT_LEFT_KINDS
} extLeft_t;

// the kinds by name, as the web page gets them
extern const char	*extLeftNames[EXT_LEFT_KINDS];

// a client command an extension runs rather than the game (clientCommands)
#define EXT_COMMAND_SECRET	1	// its words after its own are a secret: no log, echo or chat line holds them
#define EXT_COMMAND_LIMITED	2	// 5 a minute for each client and 30 for every client, as guessing a secret needs
typedef struct {
	const char	*name;
	int		flags;		// EXT_COMMAND_ bits
} extClientCommand_t;

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
	// server tells its players. The network is still up. A quit run
	// inside a hook waits for the hooks to return, so every extension
	// gets this; startDefault and checkRotation, which start maps the
	// engine's way, don't count as hooks running. Not called after a
	// crash, nor on an extension whose own hook raised a fatal error:
	// that one gets abandon.
	void		(*shutdown)( const char *reason );

	// Once, in place of shutdown, when a fatal error ends the program
	// while this extension's own hook is running, its state perhaps half
	// changed. It ends only what would outlive the program, such as child
	// processes, and calls nothing in the engine, printing included.
	void		(*abandon)( const char *reason );

	// A dedicated server's own settings, kept the extension's way in place
	// of the engine's q3config_server.cfg. read is called where the configs
	// run, before init and under --check too, and again on a reload (reload
	// qtrue), a game restart (qfalse again); write when a saved setting
	// changed. Each returns whether it handled them; with none that does,
	// the engine keeps its config as before. An error raised in a first
	// read stops the program, as one in Com_Init does.
	qboolean	(*readServerSettings)( qboolean reload );
	qboolean	(*writeServerSettings)( void );

	// A dedicated server's maps, for an extension keeping its rotation (a
	// map pool), called on the extensions whether or not they're active, as
	// they run under --check too. startDefault: no map was given and the
	// startup's commands have all run; start one (a map command) and return
	// qtrue, which ends the calls, or return qfalse for the next extension
	// and then the engine's default map. startSummary: print the start
	// summary's lines of its own, after the map's; every extension's.
	// checkRotation: under --check, print the maps that follow the
	// startup's ("  then the pool: ...") and return qtrue, which ends the
	// calls, where the engine would follow nextmap
	qboolean	(*startDefault)( void );
	void		(*startSummary)( void );
	qboolean	(*checkRotation)( void );
	// A map command (map, devmap, spmap) is about to change or start the
	// map, from Cmd_Origin's origin: return qfalse with why, in an admin's
	// words, to refuse it, as a server its owner stopped refuses a config's
	// map line. Every extension's, until one refuses
	qboolean	(*mapAllowed)( const char *map, char *why, int whySize );
	// A dedicated server's map ended with an error as it loaded, after
	// mapLoading and before mapSpawned: map is its name and reason the
	// error's text. Return MAPFAILED_TAKEN, having queued the map to play
	// instead (a map command), and the engine doesn't start the failed one
	// again; MAPFAILED_EXIT with why, in an admin's words, when none of the
	// extension's maps would load, and the server exits with
	// EXIT_MAP_ERROR; or MAPFAILED_PASS for the next extension and then the
	// engine's own way, the map started again once (SV_RestartAfterError).
	// Called inside the error, on every extension, active or not, until one
	// doesn't pass: what it does must not raise an error of its own
	int			(*mapFailed)( const char *map, const char *reason, char *why, int whySize );

	// The console's status command printed the server's map and players,
	// or that it isn't running: print lines of the extension's own after
	// them, such as the program's uptime. Not called when the output is
	// redirected (rcon's reply, a tool's), which stays as stock prints it
	// for the admin bots and tools that parse it
	void		(*status)( void );

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
	// A client's chat and voice were muted, or the mute lifted, as the
	// client is told: by a command (clientmute, an extension's
	// SV_ExtMuteClient), whose Cmd_Origin and Cmd_IsConsole say whose it
	// is, or as a timed mute ran out, with no command running
	void		(*clientMuted)( int clientNum, qboolean muted );

	// A map is about to load: the last map's game has shut down and this
	// one's hasn't started, so what's set now (a mode's game type, which
	// latches) is this map's
	void		(*mapLoading)( const char *map );
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
	// A client's role, a ROLE_ rank (q_shared.h), by its slot and the device
	// key it proved (NULL for none), or of a key alone before it has a slot
	// (clientNum -1): the highest any extension gives
	int			(*clientRole)( int clientNum, const byte *key );
	// clientCommands, ending with a NULL name: commands a client sends that
	// this extension runs (runCommand, their words in Cmd_Argv) in place of
	// the game, which never sees them, nor clientCommand above. With
	// EXT_COMMAND_SECRET the engine logs and echoes none of their words but
	// the command's own, and sends no chat line that starts with one; with
	// EXT_COMMAND_LIMITED it limits how often they come. A bot's go to the
	// game, as a bot has no secrets
	const extClientCommand_t	*clientCommands;
	void		(*runCommand)( int clientNum );
	// The game sent a command to a client, or to every client when
	// clientNum is -1: a line to print, a chat line, the scores
	void		(*serverCommand)( int clientNum, const char *text );
	// A connectionless packet the server doesn't answer itself, from an
	// address: qtrue if the extension took it. Its line, up to
	// BIG_INFO_STRING, is in Cmd_Argv. Anyone can send one, from any
	// address, so every word is hostile. proxied when a trusted proxy
	// (sv_trustedProxies) named from rather than the packet coming from
	// it: a proxy can name any address, so a service's own messages
	// should be taken only from packets that came directly
	qboolean	(*connectionless)( const netadr_t *from, qboolean proxied );
	// The same for the client: a connectionless packet it doesn't handle
	// itself, its line in Cmd_Argv and, untokenized, Cmd_Cmd. Every word
	// is hostile here too
	qboolean	(*clientConnectionless)( const netadr_t *from, qboolean proxied );
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
	// The client left a server and stays at the menu, nothing waiting to
	// go back: why, and the words the menu shows (com_errorMessage), ""
	// for the player's own. A leave a wait follows, to rejoin or for a
	// restarting server, reports only when the wait gives up or the player
	// ends it, and one a connect elsewhere follows reports nothing. Called
	// after clientStateChanged, in the frame after the leave
	void		(*clientLeft)( extLeft_t kind, const char *reason );
	// The client begins a connect, to a server or to its own listen
	// server: called as it starts, before any of it is sent, so an
	// extension can take back what the connect would carry, such as a
	// password meant for another server. CL_ExtConnect's own calls it too
	void		(*clientConnecting)( void );

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
	// a finger on a touch screen. Its down is offered as a key's is, and
	// the extension that takes it gets the rest of that touch alone, its
	// moves and its end, wherever the input has gone since. A finger no
	// extension takes is the mouse while no other finger is: its position
	// is the pointer's or the mouse's motion, and it holds K_MOUSE1
	// (cl_keys.c's CL_TouchEvent)
	qboolean	(*touchEvent)( const extTouch_t *touch );

	// Extension calls for the game modules, the server's game too.
	// moduleCalls ends with a NULL key; moduleCall answers a module's call
	// of one of them with its arguments (args[0] is the number, args[1] on
	// the module's), checked with qcommon.h's VMA_ and VMV_ macros, whose
	// errors name the call by its key. Linked modules and QVMs both call
	// it. The hooks after it (moduleShutdown on) are the client's modules'
	// alone
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
	// qtrue when the extension turns moduleRender on and off itself via
	// Hooks_SetModuleRender (starting off), so Hooks_WantModuleRender stays
	// false while it doesn't need per-call interception
	qboolean	moduleRenderDynamic;
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
// whether an extension's hook is running, so a quit waits for it to return
qboolean	Hooks_Running( void );
void	Hooks_Shutdown( const char *reason );
qboolean	Hooks_ReadServerSettings( qboolean reload );
qboolean	Hooks_WriteServerSettings( void );
qboolean	Hooks_StartDefault( void );
void	Hooks_StartSummary( void );
qboolean	Hooks_CheckRotation( void );
// whether every extension lets the map command run (mapAllowed), else
// qfalse with the first refusal's why
qboolean	Hooks_MapAllowed( const char *map, char *why, int whySize );
// what an extension does with a map that failed as it loaded (mapFailed):
// MAPFAILED_PASS when none takes it
#define MAPFAILED_PASS		0
#define MAPFAILED_TAKEN		1
#define MAPFAILED_EXIT		2
int		Hooks_MapFailed( const char *map, const char *reason, char *why, int whySize );
// the status command's lines of the extensions' own, after its players,
// on the console alone (not under a redirect)
void	Hooks_Status( void );
// a crash stops the program: before the server shuts down
void	Hooks_Signal( void );
// the longest a dedicated server with no map may wait, as the extensions
// asked in their last frames: -1 for no limit
int	Hooks_IdleMsec( void );

void	Hooks_ClientConnected( int clientNum );
void	Hooks_ClientBegin( int clientNum );
void	Hooks_ClientUserinfoChanged( int clientNum );
void	Hooks_ClientDropped( int clientNum, const char *reason );
void	Hooks_ClientMuted( int clientNum, qboolean muted );
void	Hooks_MapLoading( const char *map );
void	Hooks_MapSpawned( void );
void	Hooks_MapRestarted( void );
void	Hooks_ServerShutdown( const char *reason );
void	Hooks_GameFrameDone( int levelTime );
void	Hooks_Hibernation( qboolean asleep );
void	Hooks_GameFact( const char *fact, const char *info );
void	Hooks_ClientCommand( int clientNum, const char *text );
int		Hooks_ClientRole( int clientNum, const byte *key );
// the extension that runs a client command (clientCommands), with its
// flags, or -1; and running it there
int		Hooks_FindClientCommand( const char *name, int *flags );
void	Hooks_RunClientCommand( int extension, int clientNum );
void	Hooks_ClientCommandDone( int clientNum );
void	Hooks_ServerCommand( int clientNum, const char *text );
// whether an extension took a connectionless packet, its line in Cmd_Argv
qboolean	Hooks_Connectionless( const netadr_t *from, qboolean proxied );
// the same for a packet the client doesn't handle
qboolean	Hooks_ClientConnectionless( const netadr_t *from, qboolean proxied );
void	Hooks_Print( const char *text );
void	Hooks_ClientStateChanged( connstate_t state );
void	Hooks_DemoPlayback( qboolean playing );
void	Hooks_ClientLeft( extLeft_t kind, const char *reason );
void	Hooks_ClientConnecting( void );
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
// a finger's down: the number, from 1, of the extension that took it, which
// the rest of its touch goes to (Hooks_TouchTo); 0 when none did
int	Hooks_TouchDown( const extTouch_t *touch );
void	Hooks_TouchTo( int taker, const extTouch_t *touch );
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
// enables or disables moduleRender delivery for an extension
void		Hooks_SetModuleRender( const engineExtension_t *ext, qboolean want );
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
// A connect to whichever of several addresses answers first: each is sent
// the challenge request every FAST_RETRANSMIT msec for the first
// fastMsec, while routers open, then at connect's pace, until one answers
// or giveUpMsec pass. delays, when given, holds back each address that
// many msec from the start, so a last resort is asked only once the others
// haven't answered. With an expected key's fingerprint
// (Ident_Fingerprint), a server that proves another key has its address
// dropped, one that proves none is passed over, and the connect ends once
// no address is left. connect itself expects no key. name is what the
// connect is shown as
void		CL_ExtConnect( const char *name, const netadr_t *candidates, const int *delays, int count, int fastMsec,
				int giveUpMsec, const char *expectedFingerprint );
// the command that makes the connect just begun again, for a reconnect
// that goes back the way the player came (a join by its code); a connect
// begun since forgets it
void		CL_ExtSetRejoin( const char *command );
// a leave an extension reports itself, as clientLeft and a web page get
// it: a join it began for the page that failed before any connect, so
// the page hears of it as of the client's own leaves
void		CL_ExtLeft( extLeft_t kind, const char *reason );
// the connect under way, or a wait to rejoin a server, ended as the
// player's own Escape would end it, but told as canceled: at once, from the
// state the client is in now; nothing in a match, a match changing maps,
// a demo or at the menu with no wait. qtrue when it ended one. A connect still connecting ends
// in the error that ends the frame (ERR_DISCONNECT), so call it last
qboolean	CL_ExtCancel( void );
// grows with each connect begun, and each one the player stopped: a
// disconnect, a reconnect's wait ended. A join that sees it change before
// its own connect was overtaken
int			CL_ExtConnectGeneration( void );
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
// whether the input the hooks get now (the pointer, the mouse's motion, or
// K_MOUSE1, down or up) is the mouse the engine makes of a finger no
// extension took, rather than a real mouse
qboolean	CL_ExtInputFromTouch( void );
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
// the same with its port, as NET_AdrToStringwPort gives it: "1.2.3.4:27960",
// "[::1]:27960"; "localhost", "bot" and "" as above, and the same buffer
// rule, NET_AdrToStringwPort's own buffer
const char	*SV_ExtClientAddressPort( int clientNum );
qboolean	SV_ExtClientIsBot( int clientNum );
// the device key the client proved on connect (identity.h), into key;
// qfalse for an unproven one, a stock client's, or none
qboolean	SV_ExtClientIdentity( int clientNum, byte key[32] );
// whether it plays from a browser, its packets coming through the gateway's
// own socket (NET_PacketFromGateway); qfalse for a native client, a bot or
// none
qboolean	SV_ExtClientBrowser( int clientNum );
// prints text on a client's console only, as print does; nothing for none
void	SV_ExtClientPrint( int clientNum, const char *text );
// a line to every player, as the server's own: in the middle of their
// screens (cp) with center, else on their consoles and chat (print), quotes
// made ' as SV_ExtClientPrint makes them; nothing with no map running
void	SV_ExtBroadcast( const char *text, qboolean center );
// whether the server lists itself now: sv_visibility everyone, or with it
// "" dedicated 2; a network on, a master set, and no drain, as its
// heartbeats go
qboolean	SV_ExtListed( void );
// who the server takes, as sv_visibility says:
// unset leaves it to dedicated; local takes the local network alone,
// unlisted; invite anyone with the address, unlisted; everyone lists it
typedef enum {
	EXT_VISIBILITY_UNSET,
	EXT_VISIBILITY_LOCAL,
	EXT_VISIBILITY_INVITE,
	EXT_VISIBILITY_EVERYONE
} extVisibility_t;
extVisibility_t	SV_ExtVisibility( void );
// an address that stands for many players, as a container's network
// gateway does where its port publishing shows every peer by it: under
// local, a player from it is outside, whatever the local network's rule
// says, and said is printed once, the first time a connect from it is
// refused; NULL clears it
void		SV_ExtSetStandIn( const netadr_t *adr, const char *said );
// whether a player from an address, come to one of this computer's (to, or
// NULL), is on the local network as sv_visibility local judges it: the
// local network's rule, and never the address that stands for many
qboolean	SV_ExtLocalPlayer( const netadr_t *from, const netadr_t *to );
// whether who can join asks for the masters' lists: everyone, or with it
// unset dedicated 2, as ever; SV_ExtListed says whether it's listed now
qboolean	SV_ExtListingWanted( void );
// how many players come from outside the local network (NET_IsLocalNetwork),
// as sv_visibility local would drop: never a bot or the server's own player
int		SV_ExtOutsidePlayers( void );
// the slots the server has and the game type its map plays, as it started
// (ENG-083): the cvars can hold a latched change a module applied early
int		SV_ExtMaxClients( void );
int		SV_ExtGametype( void );
// holds the masters off, why saying why: no heartbeats and no answers to
// their polls, though the server stays listed for the rest (SV_ExtListed);
// NULL or "" lets them go again, a heartbeat at the next frame. Not a
// drain: players still come in
void	SV_ExtHoldListing( const char *why );
// why the masters are held off now, "" when they aren't
const char	*SV_ExtListingHold( void );
// a master server set (sv_master1 and on): its name, when it was last sent
// a heartbeat, and when its getinfo was last answered, as the master asks
// once it has a heartbeat; 0 for never since its name was set
typedef struct {
	const char	*name;
	time_t		heartbeatAt, polledAt;
} extMaster_t;
// the masters set, in order, up to max, and how many
int		SV_ExtMasters( extMaster_t *masters, int max );
// its ping in milliseconds, as status and getstatus give it; 999 while it
// loads the map, 0 for a bot or none
int		SV_ExtClientPing( int clientNum );
// its score as getstatus reads it, the game's PERS_SCORE, which every mod
// keeps for server browsers; qfalse with no game running. A bot's, in
// clientConnected and the userinfo change the game makes right after, is
// still the slot's last client's: the game sets the bot up after both
qboolean	SV_ExtClientScore( int clientNum, int *score );
// and its place as the game ranks it, from 0, RANK_TIED_FLAG set when it
// shares it; in a team mode, the teams' order (0 red ahead, 1 blue, 2 tied)
qboolean	SV_ExtClientRank( int clientNum, int *rank );
// a configstring the game set, as SV_GetConfigstring reads it, cut to
// size: a fact such as its warmup, its level's start or a player's team,
// which an extension reads by the game's own numbering; qfalse with no
// map running or an index out of range. And the game's time, which its
// times in configstrings count in, in milliseconds; 0 with no map
qboolean	SV_ExtConfigstring( int index, char *out, int size );
int			SV_ExtGameTime( void );
// why an extension drops a client, sent as the disconnect command's second
// argument (SV_DropClientAs) so our client acts on a machine code while
// stock clients still display the reason text alone
typedef enum {
	EXT_DROP_PLAIN,
	EXT_DROP_KICKED,
	EXT_DROP_BANNED,
	EXT_DROP_AWAY,
	EXT_DROP_LOST
} extDrop_t;
// drops a client, telling it and everyone reason, as clientkick does
// with "was kicked"; qfalse for none, or a listen server's own player,
// whom clientkick won't drop either
qboolean	SV_ExtDropClient( int clientNum, const char *reason );
// the same, naming the drop's machine kind after its reason
qboolean	SV_ExtDropClientAs( int clientNum, const char *reason, extDrop_t kind );
// the same, the disconnect saying it was a kick (SV_DropClientAs), as
// clientkick's does
qboolean	SV_ExtKickClient( int clientNum, const char *reason );
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
// and the status that exit has: 0, or EXIT_RESTART for a restart asked
// for; -1 while it isn't draining
int			SV_ExtDrainStatus( void );
// a drain started, as the console's drain [reason] starts one, whose exit
// has status: qfalse, saying why, without a map up on a dedicated server.
// Started again, it takes the new reason and status
qboolean	SV_ExtDrain( const char *reason, int status );
// SV_ExtDrain's status for a drain whose end shuts the map down, the
// program staying up with no map (an owner's stop), rather than an exit
#define	SV_DRAIN_STOP	256
// the map shut down, its players told reason, as the next frame starts
// (never inside the hook or map change asking); nothing with no map
void		SV_ExtShutdown( const char *reason );
// while a dedicated server has no map on purpose (its owner stopped it), a
// connect is answered with text, "The server is stopped: <reason>", say,
// masters left unanswered; NULL or "" for none, as a server with no map
// is otherwise silent but for rcon
void		SV_ExtHold( const char *text );
// whether the command running came by rcon, which names nobody
qboolean	SV_ExtRcon( void );
// the players on, bots not counting, as a drain and keeping awake count
int			SV_ExtHumanCount( void );
// a drain called off, as undrain does; nothing while it isn't draining
void		SV_ExtUndrain( void );

// content add, for extensions (server/sv_export.c): a pak in sv_contentDir's
// incoming checked and installed, as the console's content add <file>
// [game] does it, with a verdict besides its lines. The verdict's code is
// added, installed_already, or why it was refused: other_contents,
// retail, damaged, zip64, native_code, encrypted, method, escapes,
// long_name, too_big, unpacks_too_big, bad_name, bad_game, no_room or
// failed; and ok, from SV_ExtContentCheck, for a pak it can't refuse yet.
// Its warnings' codes: overrides_game_code, overrides_many, drops_shaders,
// map_pack_scope, game_code_left_out, for a map pak whose game code the
// game leaves out, running its own (FS_CodeLeftOut), and
// classic_clients_load_code, where classic clients that get that pak load
// its cgame or ui (FS_ClassicClientCode).
// a set of the game's own files, the full game's or Team Arena's, as the
// owner adds them: how many of it are here or held, the rest, and when
// they count (sv_export.c)
typedef struct {
	const char	*set;			// "base" (the full game's 9) or "missionpack" (Team Arena's 4); NULL for none
	char		name[MAX_QPATH];	// for a verdict, the file as installed, "baseq3/pak0.pk3"; "" otherwise
	int			inPlace, of;	// how many of the set are here or held, of how many
	char		missing[128];	// the rest, "pak1.pk3 pak2.pk3"
	const char	*applies;		// "when_complete", "restart" or "never"
	char		reason[256];	// for never, or a note for restart, in an admin's words; "" for none
} extGameOwnSet_t;
// each of the game's sets that's partly there, or whole with some held:
// how many, at most max into sets
int		SV_ExtGameOwnSets( extGameOwnSet_t *sets, int max );

#define EXT_CONTENT_MAX_WARNINGS	16
typedef struct {
	const char	*code;
	char		message[MAX_STRING_CHARS];	// the console's line for it, with no line end
	int		numWarnings;
	struct {
		const char	*code;
		char		message[MAX_STRING_CHARS];
	}		warnings[EXT_CONTENT_MAX_WARNINGS];
	char		sha256[65];		// the pak's, in lowercase hex; "" where it wasn't read
	long long	size;			// its bytes, -1 where unknown
	// one of the game's own files, which the server's owner may add
	// (owner): installed under its own name and held until its set is
	// whole, taken up as the program next starts (files.c's FS_TakeHeldSets)
	extGameOwnSet_t	gameOwn;
} extContentVerdict_t;
typedef void	( *extContentDone_t )( void *user, const extContentVerdict_t *verdict );
// Starts content add of file, a pak's name in incoming, for gameDir, this
// server's for NULL. done gets the verdict with user, at once for a pak
// refused before its copy starts, else as the add ends, a frame or more
// later. With removeSource, the file is removed from incoming as the add
// ends, whatever the verdict, unless its name can't be a pak's. owner: the
// server's owner's, who may add the game's own files, refused (retail) to
// anyone else. qfalse, doing nothing, while an add or an export runs
qboolean	SV_ExtContentAdd( const char *file, const char *gameDir, qboolean removeSource, qboolean owner,
	extContentDone_t done, void *user );
// What content add would say of a pak before it's in incoming, from file,
// gameDir (NULL for this server's), its size and its pak checksum, which a
// page computes from the zip's directory (web/src/pak-check.ts): the
// names, the size, one of the game's own by its checksum, and an installed
// pak of its name, the same file if it's the same size and checksum. With
// hasChecksum qfalse, an installed pak of another size is another one, and
// the rest is left to the add; owner as for SV_ExtContentAdd. The console
// says nothing
void	SV_ExtContentCheck( const char *file, const char *gameDir, long long size, qboolean hasChecksum,
	unsigned int checksum, qboolean owner, extContentVerdict_t *verdict );

// The paks the server has, and taking one away (server/sv_installed.c),
// as the console's content list, content remove and content restore.
typedef struct {
	const char	*map;
	const char	*why;			// "playing", "next" (nextmap names it) or "rotation" (sv_pool has it)
} extContentUse_t;
typedef struct {
	char		game[MAX_QPATH];
	char		name[MAX_QPATH * 2];	// its file's name, with .pk3
	int64_t		size;
	const char	*origin;		// "game" (one of the game's own), "added" (content add, or
								// restored as added) or "by_hand"
	const char	*state;			// "loaded", "pending" (added since the map loaded),
								// "removing" (goes as the next map loads) or "removed" (a kept copy)
	qboolean	home;			// in the home's game directory, where content add installs
	int			rank;			// its place among files of its game and name: 0 the home's,
								// 1 its kept copy, 2 another folder's (the list's order)
	qboolean	removable;		// content remove may take it
	int64_t		addedAt, removedAt, keptUntil;	// Unix seconds, 0 for none
	qboolean	gameOwn, mapPack, gameCode, codeLeftOut;	// as the file system says (fsPak_t)
	// the game's own: its release, "demo" (the demo's pak0), "base" (Quake III
	// Arena's pak0 to pak8) or "missionpack" (Team Arena's pak0 to pak3), NULL
	// for another file; its number in the release (pak<part>.pk3, -1 where
	// it isn't known), and how many files the release has
	const char	*release;
	int			part, parts;
	int			numMaps;		// with details: its maps (none for a held file of the game's own)
	const char	**maps;
	int			numUses;		// and those in use, a map once for each why
	extContentUse_t	*uses;
} extContentFile_t;
typedef struct {
	int					count;
	extContentFile_t	*files;
	int					installed;	// the files not removed, and their bytes
	int64_t				bytes;
	void				*blocks;	// its strings'
} extContentList_t;
// the list, sorted by game directory, then name, the home's file before
// another of its name; only game's for a game, removed's kept copies with
// removed, and maps and uses with details. Freed by SV_ExtContentListFree
qboolean	SV_ExtContentList( const char *game, qboolean removed, qboolean details, extContentList_t *list );
void		SV_ExtContentListFree( extContentList_t *list );
// moves whenever the list may have: a pak added, removed, restored or
// pruned, a map loaded, the pending maps, sv_pool or nextmap changed
int			SV_ExtContentRevision( void );
// content remove's and restore's verdict: code, removed or restored, else
// installed_already (restore) or why not: not_found, game_own (one of the
// game's own files, to anyone but the owner), game_needs (one the running
// game needs, even to the owner), not_server_folder, in_use, bad_name,
// bad_game, other_contents, failed,
// or content add's codes for a copy it refuses (restore); its words, for
// an admin; when it applies, "now" or "next_session", NULL when it
// doesn't; and the file's entry, its maps and uses, where it was found.
// Freed by SV_ExtContentChangeFree
typedef struct {
	const char			*code;
	char				message[MAX_STRING_CHARS];
	const char			*applies;
	const extContentFile_t	*entry;	// the file's entry, in list, or NULL
	extContentList_t	list;
} extContentChange_t;
// qfalse, doing nothing, while an add or an export runs
qboolean	SV_ExtContentRemove( const char *name, const char *gameDir, qboolean owner, extContentChange_t *change );
qboolean	SV_ExtContentRestore( const char *name, const char *gameDir, extContentChange_t *change );
void		SV_ExtContentChangeFree( extContentChange_t *change );

// The maps the server can load (server/sv_export.c): those the search
// path has, and the pending ones, in the paks content add installed since
// the map loaded for a game directory the search path reads. It reads
// them at the next map load, so until then the file system doesn't find
// them, though the map command loads one, the load being what reads its
// pak. A pak removed since loses its maps, and one changed since is read
// again, as they're asked for. Indexes run from 0 to
// SV_ExtPendingMapCount() - 1, and may name a map the search path has
// already, in another pak.
typedef struct {
	char	map[MAX_QPATH];		// its name, without maps/ or .bsp
	char	title[256];		// its pak's own .arena's longname, as written, cut to fit; "" for none
} extPendingMap_t;

// whether the server has maps/<map>.bsp, or can load it as a pending map,
// and not only in paks waiting to be removed (SV_ExtMapRemoving)
qboolean	SV_ExtHasMap( const char *map );
// whether a map the search path has is only in paks content remove took,
// so it's gone as the next map loads
qboolean	SV_ExtMapRemoving( const char *map );
int		SV_ExtPendingMapCount( void );
qboolean	SV_ExtGetPendingMap( int index, extPendingMap_t *map );
// changes whenever the pending maps do: a pak added, removed or changed, or
// the search path read again
int		SV_ExtPendingMapChanges( void );

// The server's address bans and their exceptions (banaddr, exceptaddr),
// which sv_banFile keeps. Indexes run from 0 to SV_ExtBanCount() - 1, and
// move as entries come and go.
#define EXT_BAN_NAME		64
#define EXT_BAN_REASON		128
#define EXT_BAN_LONGEST		( 365 * 24 * 60 * 60 )	// the longest a ban may last before it runs out, in seconds
// what the ban file keeps of a ban after its subnet: whether who made it
// and when are known (a ban from before the file kept them, or another
// program's, isn't), when, by whom, the banned player's name, why, and
// when it runs out (Unix seconds, 0 for never), each "" or 0 for none
typedef struct {
	qboolean	recorded;
	int64_t		at, expires;
	char		by[EXT_BAN_NAME], name[EXT_BAN_NAME], reason[EXT_BAN_REASON];
} extBanDetails_t;
typedef struct {
	char		address[NET_ADDRSTRMAXLEN];	// without the prefix: "192.168.1.0", "fe80::"
	int		subnet;				// the prefix's bits: 32, or 128 for IPv6, for one address
	qboolean	exception;			// an exception to the bans, not a ban
	extBanDetails_t	details;
} extBan_t;

int		SV_ExtBanCount( void );
qboolean	SV_ExtGetBan( int index, extBan_t *ban );
// changes whenever the list does: by the console, rcon, an extension, or
// the file read again (rehashbans)
int		SV_ExtBanListChanges( void );
// Adds a ban, or an exception, as banaddr and exceptaddr do: of an IPv4
// or IPv6 address written as numbers, or a range in CIDR notation
// ("192.168.1.0/24"), never a host name, which would be looked up. The
// entries it supersedes go, and the list is saved; a ban supersedes another
// only if it runs out no sooner, so a ban for good stays one. Returns
// the new entry's index, or one of the EXT_BAN_* below with why as
// banaddr prints it, and *covering the index of an entry that already
// supersedes it, or -1.
// Nobody connected is dropped: SV_ExtDropBanned does that. A ban's
// details, if given, are kept with it: by, name, reason and expires (0 for
// never; the server lifts it once past), at now if 0
int		SV_ExtAddBan( const char *address, qboolean exception, const extBanDetails_t *details, int *covering,
	char *why, int whySize );
#define EXT_BAN_INVALID		-1	// not an address, or a prefix out of range
#define EXT_BAN_REFUSED		-2	// an entry supersedes it, the list is full, or a ban supersedes the exception
// removes an entry, and saves the list
qboolean	SV_ExtRemoveBan( int index );
// drops every client the bans would refuse to let in now, telling each
// and everyone reason, each disconnect saying it was a ban; returns how
// many
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
