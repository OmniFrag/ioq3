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
// qcommon.h -- definitions common between client and server, but not game.or ref modules
#ifndef _QCOMMON_H_
#define _QCOMMON_H_

#include "../qcommon/cm_public.h"

//#define	PRE_RELEASE_DEMO

//============================================================================

//
// msg.c
//
typedef struct {
	qboolean	allowoverflow;	// if false, do a Com_Error
	qboolean	overflowed;		// set to true if the buffer size failed (with allowoverflow set)
	qboolean	oob;			// set to true if the buffer size failed (with allowoverflow set)
	byte	*data;
	int		maxsize;
	int		cursize;
	int		readcount;
	int		bit;				// for bitwise reads and writes
} msg_t;

void MSG_Init (msg_t *buf, byte *data, int length);
void MSG_InitOOB( msg_t *buf, byte *data, int length );
void MSG_Clear (msg_t *buf);
void MSG_WriteData (msg_t *buf, const void *data, int length);
void MSG_Bitstream( msg_t *buf );

// TTimo
// copy a msg_t in case we need to store it as is for a bit
// (as I needed this to keep an msg_t from a static var for later use)
// sets data buffer as MSG_Init does prior to do the copy
void MSG_Copy(msg_t *buf, byte *data, int length, msg_t *src);

struct usercmd_s;
struct entityState_s;
struct playerState_s;

void MSG_WriteBits( msg_t *msg, int value, int bits );

void MSG_WriteChar (msg_t *sb, int c);
void MSG_WriteByte (msg_t *sb, int c);
void MSG_WriteShort (msg_t *sb, int c);
void MSG_WriteLong (msg_t *sb, int c);
void MSG_WriteFloat (msg_t *sb, float f);
void MSG_WriteString (msg_t *sb, const char *s);
void MSG_WriteBigString (msg_t *sb, const char *s);
void MSG_WriteAngle16 (msg_t *sb, float f);
int MSG_HashKey(const char *string, int maxlen);

void	MSG_BeginReading (msg_t *sb);
void	MSG_BeginReadingOOB(msg_t *sb);

int		MSG_ReadBits( msg_t *msg, int bits );

int		MSG_ReadChar (msg_t *sb);
int		MSG_ReadByte (msg_t *sb);
int		MSG_ReadShort (msg_t *sb);
int		MSG_ReadLong (msg_t *sb);
float	MSG_ReadFloat (msg_t *sb);
char	*MSG_ReadString (msg_t *sb);
char	*MSG_ReadBigString (msg_t *sb);
char	*MSG_ReadStringLine (msg_t *sb);
float	MSG_ReadAngle16 (msg_t *sb);
void	MSG_ReadData (msg_t *sb, void *buffer, int size);
int		MSG_LookaheadByte (msg_t *msg);

void MSG_WriteDeltaUsercmdKey( msg_t *msg, int key, usercmd_t *from, usercmd_t *to );
void MSG_ReadDeltaUsercmdKey( msg_t *msg, int key, usercmd_t *from, usercmd_t *to );

void MSG_WriteDeltaEntity( msg_t *msg, struct entityState_s *from, struct entityState_s *to
						   , qboolean force );
void MSG_ReadDeltaEntity( msg_t *msg, entityState_t *from, entityState_t *to, 
						 int number );

void MSG_WriteDeltaPlayerstate( msg_t *msg, struct playerState_s *from, struct playerState_s *to );
void MSG_ReadDeltaPlayerstate( msg_t *msg, struct playerState_s *from, struct playerState_s *to );


void MSG_ReportChangeVectors_f( void );

//============================================================================

/*
==============================================================

NET

==============================================================
*/

#define NET_ENABLEV4            0x01
#define NET_ENABLEV6            0x02
// if this flag is set, always attempt ipv6 connections instead of ipv4 if a v6 address is found.
#define NET_PRIOV6              0x04
// disables ipv6 multicast support if set.
#define NET_DISABLEMCAST        0x08


#define	PACKET_BACKUP	32	// number of old messages that must be kept on client and
							// server for delta comrpession and ping estimation
#define	PACKET_MASK		(PACKET_BACKUP-1)

#define	MAX_PACKET_USERCMDS		32		// max number of usercmd_t in a packet

#define	MAX_SNAPSHOT_ENTITIES	256

#define	PORT_ANY			-1

#define	MAX_RELIABLE_COMMANDS	64			// max string commands buffered for restransmit

typedef enum {
	NA_BAD = 0,					// an address lookup failed
	NA_BOT,
	NA_LOOPBACK,
	NA_BROADCAST,
	NA_IP,
	NA_IP6,
	NA_MULTICAST6,
	NA_UNSPEC
} netadrtype_t;

typedef enum {
	NS_CLIENT,
	NS_SERVER
} netsrc_t;

#define NET_ADDRSTRMAXLEN 48	// maximum length of an IPv6 address string including trailing '\0'
typedef struct {
	netadrtype_t	type;

	byte	ip[4];
	byte	ip6[16];

	unsigned short	port;
	unsigned long	scope_id;	// Needed for IPv6 link-local addresses
} netadr_t;

void		NET_Init( void );
void		NET_Shutdown( void );
void		NET_Restart_f( void );

// NET_QueryServer's answers, which are --status's exit statuses
#define QUERY_ANSWERED		0	// the server answered
#define QUERY_NO_ANSWER		1	// it didn't, within two seconds
#define QUERY_BAD_ADDRESS	2	// the address isn't one
int			NET_QueryServer( const char *address, char *summary, int size );
int			NET_Port( void );
int			NET_OpenPort( netadrtype_t type );
void		NET_Config( qboolean enableNetworking );
void		NET_ExitIfDisabled( const char *lead );
void		NET_FlushPacketQueue(void);
void		NET_SendPacket (netsrc_t sock, int length, const void *data, netadr_t to);
void		QDECL NET_OutOfBandPrint( netsrc_t net_socket, netadr_t adr, const char *format, ...) Q_PRINTF_FUNC(3, 4);
void		QDECL NET_OutOfBandData( netsrc_t sock, netadr_t adr, byte *format, int len );

qboolean	NET_CompareAdr (netadr_t a, netadr_t b);
qboolean	NET_CompareBaseAdrMask(netadr_t a, netadr_t b, int netmask);
qboolean	NET_CompareBaseAdr (netadr_t a, netadr_t b);
qboolean	NET_IsLocalAddress (netadr_t adr);
const char	*NET_TakeBroadcastFailure( void );
// why a broadcast failed since the last call, or NULL
const char	*NET_AdrToString (netadr_t a);
const char	*NET_AdrToStringwPort (netadr_t a);
int		NET_StringToAdr ( const char *s, netadr_t *a, netadrtype_t family);
qboolean	NET_GetLoopPacket (netsrc_t sock, netadr_t *net_from, msg_t *net_message);
void		NET_JoinMulticast6(void);
void		NET_LeaveMulticast6(void);
void		NET_Sleep(int msec);


#define	MAX_MSGLEN				16384		// max length of a message, which may
											// be fragmented into multiple packets

#define MAX_DOWNLOAD_WINDOW		48	// ACK window of 48 download chunks. Cannot set this higher, or clients
						// will overflow the reliable commands buffer
#define MAX_DOWNLOAD_BLKSIZE		1024	// 896 byte block chunks

// in unsigned arithmetic, which wraps rather than overflowing
#define NETCHAN_GENCHECKSUM(challenge, sequence) \
	((int)((unsigned int)(challenge) ^ ((unsigned int)(sequence) * (unsigned int)(challenge))))

/*
Netchan handles packet fragmentation and out of order / duplicate suppression
*/

typedef struct {
	netsrc_t	sock;

	int			dropped;			// between last packet and previous

	netadr_t	remoteAddress;
	int			qport;				// qport value to write when transmitting

	// sequencing variables
	int			incomingSequence;
	int			outgoingSequence;

	// incoming fragment assembly buffer
	int			fragmentSequence;
	int			fragmentLength;	
	byte		fragmentBuffer[MAX_MSGLEN];

	// outgoing fragment buffer
	// we need to space out the sending of large fragmented messages
	qboolean	unsentFragments;
	int			unsentFragmentStart;
	int			unsentLength;
	byte		unsentBuffer[MAX_MSGLEN];

	int			challenge;
	int		lastSentTime;
	int		lastSentSize;

#ifdef LEGACY_PROTOCOL
	qboolean	compat;
#endif
} netchan_t;

void Netchan_Init( int qport );
void Netchan_Setup(netsrc_t sock, netchan_t *chan, netadr_t adr, int qport, int challenge, qboolean compat);

void Netchan_Transmit( netchan_t *chan, int length, const byte *data );
void Netchan_TransmitNextFragment( netchan_t *chan );

qboolean Netchan_Process( netchan_t *chan, msg_t *msg );


/*
==============================================================

PROTOCOL

==============================================================
*/

#define	PROTOCOL_VERSION	71
#define PROTOCOL_LEGACY_VERSION	68
// 1.31 - 67

// maintain a list of compatible protocols for demo playing
// NOTE: that stuff only works with two digits protocols
extern int demo_protocols[];

// The engine doesn't include the game modules' headers (bg_public.h). These
// are the game values it relies on; game modules must keep them.
#define	GT_FFA				0	// g_gametype
#define	GT_SINGLE_PLAYER	2
#define	PM_INTERMISSION		5	// playerState_t pm_type
#define	PERS_SCORE			0	// playerState_t persistant[]
#define	CS_WARMUP			5	// configstrings
#define	CS_PLAYERS			(32 + MAX_MODELS + MAX_SOUNDS)

#if !defined UPDATE_SERVER_NAME && !defined STANDALONE
#define	UPDATE_SERVER_NAME	"update.quake3arena.com"
#endif
// override on command line, config files etc.
#ifndef MASTER_SERVER_NAME
#define MASTER_SERVER_NAME	"master.quake3arena.com"
#endif

#ifndef STANDALONE
  #ifndef AUTHORIZE_SERVER_NAME
    #define	AUTHORIZE_SERVER_NAME	"authorize.quake3arena.com"
  #endif
  #ifndef PORT_AUTHORIZE
  #define	PORT_AUTHORIZE		27952
  #endif
#endif

#define	PORT_MASTER			27950
#define	PORT_UPDATE			27951
#define	PORT_SERVER			27960
#define	NUM_SERVER_PORTS	4		// broadcast scan this many ports after
									// PORT_SERVER so a single machine can
									// run multiple servers


// the svc_strings[] array in cl_parse.c should mirror this
//
// server to client
//
enum svc_ops_e {
	svc_bad,
	svc_nop,
	svc_gamestate,
	svc_configstring,			// [short] [string] only in gamestate messages
	svc_baseline,				// only in gamestate messages
	svc_serverCommand,			// [string] to be executed by client game module
	svc_download,				// [short] size [size bytes]
	svc_snapshot,
	svc_EOF,

// new commands, supported only by ioquake3 protocol but not legacy
	svc_voipSpeex,     // not wrapped in USE_VOIP, so this value is reserved.
	svc_voipOpus,      //
};


//
// client to server
//
enum clc_ops_e {
	clc_bad,
	clc_nop, 		
	clc_move,				// [[usercmd_t]
	clc_moveNoDelta,		// [[usercmd_t]
	clc_clientCommand,		// [string] message
	clc_EOF,

// new commands, supported only by ioquake3 protocol but not legacy
	clc_voipSpeex,   // not wrapped in USE_VOIP, so this value is reserved.
	clc_voipOpus,    //
};

/*
==============================================================

VIRTUAL MACHINE

==============================================================
*/

typedef struct vm_s vm_t;

typedef enum {
	VMI_NATIVE,
	VMI_BYTECODE,
	VMI_COMPILED
} vmInterpret_t;

typedef enum {
	TRAP_MEMSET = 100,
	TRAP_MEMCPY,
	TRAP_STRNCPY,
	TRAP_SIN,
	TRAP_COS,
	TRAP_ATAN2,
	TRAP_SQRT,
	TRAP_MATRIXMULTIPLY,
	TRAP_ANGLEVECTORS,
	TRAP_PERPENDICULARVECTOR,
	TRAP_FLOOR,
	TRAP_CEIL,

	TRAP_TESTPRINTINT,
	TRAP_TESTPRINTFLOAT
} sharedTraps_t;

typedef intptr_t (QDECL *vmMainProc)(int callNum, int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6, int arg7, int arg8, int arg9, int arg10, int arg11);

// A game module linked into the executable: the entry points a game dll
// exports, and the bounds of the module's data, which VM_Create resets on
// every load as reloading a dll would. The build generates vm_linkedModules,
// which ends with a NULL name.
typedef struct {
	const char	*name;		// "qagame", "cgame" or "ui"
	vmMainProc	vmMain;
	void		(*dllEntry)( intptr_t (QDECL *syscallptr)( intptr_t arg, ... ) );
	byte		*data, *dataEnd;	// initialized data
	byte		*bss, *bssEnd;		// zero-initialized data
	byte		*initialData;		// a copy of the data, taken before the first load
} vmLinkedModule_t;

extern vmLinkedModule_t vm_linkedModules[];

// Game modules look up engine extensions by name, as in Quake3e: the
// "//trap_GetValue" cvar holds this syscall number, which every module
// accepts as trap_GetValue( char *value, int valueSize, const char *key ).
// It writes the key's value, such as an extension's syscall number, and
// returns whether the engine knows the key. New features become new keys,
// so the fixed syscall numbers don't change.
#define	COM_TRAP_GETVALUE	700

// an extension a module can look up: its key and syscall number, or for a
// plain value (a key without "trap_"), the function that gives it
typedef struct {
	const char	*key;
	int			trap;
	const char	*(*value)( void );
} vmExtension_t;

// Extension syscalls, found through trap_GetValue, are numbered from 800 up,
// clear of every fixed table, and a number is never reused.

// trap_SetTextFocus( qboolean focused ): a cgame or ui text field has taken
// or lost focus. A module that calls it gets text input (and a phone's
// on-screen keyboard) only while it says a field has focus.
#define	COM_TRAP_SETTEXTFOCUS	800

// trap_FollowWindowSize(): a cgame or ui module reads glconfig every frame,
// so the window can change size without restarting it
#define	COM_TRAP_FOLLOWWINDOWSIZE	801

// Quake3e's, under its key names, so mods written for it take the same
// paths here: trap_Cvar_SetDescription_Q3E( const char *name, const char
// *description ) in every module, and qboolean trap_IsRecordingDemo() in
// cgame
#define	COM_TRAP_CVAR_SETDESCRIPTION	802
#define	COM_TRAP_ISRECORDINGDEMO	803

// trap_GetSafeArea( int rect[4] ) in cgame: the window's safe area in pixels,
// as x, y, width and height, clear of notches, rounded corners, home bars and
// overscan; the whole window where the system reports none
#define	COM_TRAP_GETSAFEAREA	804

// CNQ3's, under its key names, so mods written for it (CPMA) take the same
// paths here, in every module: trap_Cvar_SetHelp( const char *name, const
// char *help ) is trap_Cvar_SetDescription_Q3E's syscall, and
// trap_Cvar_SetRange( const char *name, int type, const char *min, const
// char *max ) gives a cvar a range (Cvar_SetRangeByName)
#define	COM_TRAP_CVAR_SETRANGE	805

// trap_ToggleFullscreen() in ui: a menu's fullscreen toggle, as Alt+Enter: the
// other of what's on screen, as the player's choice, and asked for again
// after a refusal, where setting r_fullscreen to what it already is asks
// nothing
#define	COM_TRAP_TOGGLEFULLSCREEN	806

// trap_Cvar_SetRange's types, CNQ3's cvarType_t; its others, CPMA's colors,
// are left unchecked
#define	CVAR_RANGE_STRING	0	// no range
#define	CVAR_RANGE_FLOAT	1
#define	CVAR_RANGE_INTEGER	2
#define	CVAR_RANGE_BITMASK	3	// an integer's range
#define	CVAR_RANGE_BOOL		4

void	VM_Init( void );
vm_t	*VM_Create( const char *module, intptr_t (*systemCalls)(intptr_t *), 
				   vmInterpret_t interpret );
// module should be bare: "cgame", not "cgame.dll" or "vm/cgame.qvm"

void	VM_Free( vm_t *vm );
void	VM_Clear(void);
void	VM_Forced_Unload_Start(void);
void	VM_Forced_Unload_Done(void);
vm_t	*VM_Restart(vm_t *vm, qboolean unpure);

intptr_t		QDECL VM_Call( vm_t *vm, int callNum, ... );

void	VM_Debug( int level );

intptr_t	VM_GetValue( intptr_t *args, const vmExtension_t *extensions );
const char	*VM_ExplicitArgStr( vm_t *vm, intptr_t value, const char *what );

// Checked syscall arguments (vm_args.c): each macro decodes argument n of a
// handler's args and says what it is. A QVM's pointers must lie inside its
// memory; for every module, pointers that must be there aren't NULL, and
// lengths, counts and numbers are in range. A bad one drops the module.
// Numbers are ints: a native module passes them through "..." and the
// engine reads intptr_t, so only the low 32 bits are defined.
void		VM_SetSyscallNames( vm_t *vm, const char * const *names, int numNames );
// an entry of a syscall name table: SYSCALL( G_PRINT ) names G_PRINT
#define SYSCALL( x )				[x] = #x
void		QDECL VM_ArgError( const intptr_t *args, int n, const char *fmt, ... ) Q_NO_RETURN Q_PRINTF_FUNC(3, 4);
void		*VM_ArgBuf( const intptr_t *args, int n, int64_t size, qboolean optional );
char		*VM_ArgStrBuf( const intptr_t *args, int n, int64_t size );
void		*VM_ArgArray( const intptr_t *args, int n, size_t elemSize, int count1, int count2 );
const char	*VM_ArgStr( const intptr_t *args, int n, qboolean optional );
int			VM_ArgInt( const intptr_t *args, int n, int lo, int hi );
int			VM_PrivateCvarFlag( void );
// whole handlers for syscalls every module has
intptr_t	VM_Strncpy( const intptr_t *args );
intptr_t	VM_FOpenFile( const intptr_t *args );

// an input string, terminated inside module memory
#define VMA_STR( n )				VM_ArgStr( args, n, qfalse )
#define VMA_STR_OPT( n )			VM_ArgStr( args, n, qtrue )
// one T the engine reads, writes, or both
#define VMA_IN( n, T )				((const T *)VM_ArgBuf( args, n, sizeof( T ), qfalse ))
#define VMA_OUT( n, T )				((T *)VM_ArgBuf( args, n, sizeof( T ), qfalse ))
#define VMA_INOUT( n, T )			VMA_OUT( n, T )
#define VMA_IN_OPT( n, T )			((const T *)VM_ArgBuf( args, n, sizeof( T ), qtrue ))
#define VMA_OUT_OPT( n, T )			((T *)VM_ArgBuf( args, n, sizeof( T ), qtrue ))
#define VMA_VEC3( n )				((float *)VM_ArgBuf( args, n, sizeof( vec3_t ), qfalse ))
#define VMA_VEC3_OPT( n )			((float *)VM_ArgBuf( args, n, sizeof( vec3_t ), qtrue ))
// len bytes, or len bytes that receive a terminated string (len >= 1)
#define VMA_BUF( n, len )			VM_ArgBuf( args, n, (int)( len ), qfalse )
#define VMA_BUF_OPT( n, len )		VM_ArgBuf( args, n, (int)( len ), qtrue )
#define VMA_STRBUF( n, len )		VM_ArgStrBuf( args, n, (int)( len ) )
// count elements of T, or count1 * count2 of them
#define VMA_ARRAY( n, T, count )	((T *)VM_ArgArray( args, n, sizeof( T ), count, 1 ))
#define VMA_ARRAY2( n, T, count1, count2 )	((T *)VM_ArgArray( args, n, sizeof( T ), count1, count2 ))
// numbers: a count into an engine array, or any range
#define VMV_COUNT( n, max )			VM_ArgInt( args, n, 0, max )
#define VMV_RANGE( n, lo, hi )		VM_ArgInt( args, n, lo, hi )
#define VMV_ENTITY( n )				VM_ArgInt( args, n, 0, MAX_GENTITIES - 1 )
// an entity a trace passes through, or none: ENTITYNUM_NONE, or -1 as some
// game code and botlib pass
#define VMV_PASSENT( n )			VM_ArgInt( args, n, -1, MAX_GENTITIES - 1 )
// a file handle the module opened, or 0
int			VM_ArgFile( const intptr_t *args, int n );
#define VMV_FILE( n )				VM_ArgFile( args, n )
static ID_INLINE float _vmf(intptr_t x)
{
	floatint_t fi;
	fi.i = (int) x;
	return fi.f;
}
#define	VMF(x)	_vmf(args[x])


/*
==============================================================

CMD

Command text buffering and command execution

==============================================================
*/

/*

Any number of commands can be added in a frame, from several different sources.
Most commands come from either keybindings or console line input, but entire text
files can be execed.

*/

void Cbuf_Init (void);
// allocates an initial text buffer that will grow as needed

void Cbuf_AddText( const char *text );
// Adds command text at the end of the buffer, does NOT add a final \n

void Cbuf_ExecuteText( int exec_when, const char *text );
// this can be used in place of either Cbuf_AddText or Cbuf_InsertText

void Cbuf_ExecuteTextRestricted( int exec_when, const char *text );
// the same for game code's text, which runs restricted (Cmd_IsRestricted)

void Cbuf_AddTextRestricted( const char *text, qboolean restricted );
void Cbuf_InsertTextRestricted( const char *text, qboolean restricted );
// Cbuf_AddText and Cbuf_InsertText into game code's buffer, or the player's

qboolean Cmd_IsRestricted( void );
qboolean Cmd_IsScript( void );
// whether the running command came from a script that runs at every start
// (Cbuf_AddScriptText), whose sets aren't saved
void Cbuf_AddScriptText( const char *text );
// whether the running command came from game code, or text it queued or
// ran: it can't run the commands that reveal secrets or stop the process,
// or use private and protected cvars
void Cmd_EndRestricted( void );
// an error ended the running command
void Cmd_AddCommandLineExec( const char *filename );
// a config a dedicated server's command line execs, which runs as a
// startup script
qboolean Cmd_IsStartupScript( const char *filename );
// whether a startup script ran the config, so that writeconfig doesn't
// replace it

void Cbuf_Execute (void);
// Pulls off \n terminated lines of text from the command buffer and sends
// them through Cmd_ExecuteString.  Stops when the buffer is empty.
// Normally called once per frame, but may be explicitly invoked.
// Do not call inside a command function, or current args will be destroyed.

void Cbuf_ExecuteScripts( void );
// Runs only the startup scripts' text (Cbuf_AddScriptText), leaving the
// rest for the frame

qboolean Cbuf_Empty( void );
qboolean Cbuf_ScriptsEmpty( void );
// Whether no startup script's text waits (Cbuf_AddScriptText)
// Whether no text waits in any buffer: everything queued has run

void Cbuf_Clear( void );
// Drops every buffer's text and waits, unrun: --check's, before it exits

void Cbuf_AddTextFrom( const char *origin, const char *text );
// Cbuf_AddText for text whose lines all have the origin given, the
// command line's, say (Cmd_Origin)

//===========================================================================

/*

Command execution takes a null terminated string, breaks it into tokens,
then searches for a command or variable that matches the first token.

*/

typedef void (*xcommand_t) (void);

void	Cmd_Init (void);

void	Cmd_AddCommand( const char *cmd_name, xcommand_t function );
// called by the init functions of other parts of the program to
// register commands and functions to call for them.
// The cmd_name is referenced later, so it should not be in temp memory
// if function is NULL, the command will be forwarded to the server
// as a clc_clientCommand instead of executed locally

void	Cmd_RemoveCommand( const char *cmd_name );

typedef void (*completionFunc_t)( char *args, int argNum );

// don't allow VMs to remove system commands
void	Cmd_RemoveCommandSafe( const char *cmd_name );

void	Cmd_CommandCompletion( void(*callback)(const char *s) );
// callback with each valid string
void Cmd_SetCommandCompletionFunc( const char *command,
	completionFunc_t complete );
void Cmd_CompleteArgument( const char *command, char *args, int argNum );
void Cmd_CompleteCfgName( char *args, int argNum );

int		Cmd_Argc (void);
char	*Cmd_Argv (int arg);
void	Cmd_ArgvBuffer( int arg, char *buffer, int bufferLength );
char	*Cmd_Args (void);
char	*Cmd_ArgsFrom( int arg );
void	Cmd_ArgsBuffer( char *buffer, int bufferLength );
char	*Cmd_Cmd (void);
void	Cmd_Args_Sanitize( void );
// The functions that execute commands get their parameters with these
// functions. Cmd_Argv () will return an empty string, not a NULL
// if arg > argc, so string operations are allways safe.

void	Cmd_TokenizeString( const char *text );
void	Cmd_TokenizeStringIgnoreQuotes( const char *text_in );
// Takes a null terminated string.  Does not need to be /n terminated.
// breaks the string up into arg tokens.

void	Cmd_ExecuteString( const char *text );
// Parses a single line of text into arguments and tries to execute it
// as if it was typed at the console

void	Cmd_HoldMapCommands( void );
// a reload's: the scripts' map, map_restart and vstr wait until the
// scripts' text queued has run

#define	CMD_ORIGIN_COMMAND_LINE	"the command line"
const char *Cmd_Origin( void );
// the config and line the running command came from, "server.cfg:12" or
// "the command line", or NULL: typed at a console, sent by rcon or game
// code, or queued by the engine
qboolean Cmd_OriginIsCommandLine( void );
qboolean Cmd_IsPakConfig( void );
// the running command is a pak's config's, on a dedicated server
void Cmd_PushOrigin( const char *name, int line );
void Cmd_PopOrigin( void );
// the origin of text run at once (EXEC_NOW), a settings file's line, say


/*
==============================================================

CVAR

==============================================================
*/

/*

cvar_t variables are used to hold scalar or string variables that can be changed
or displayed at the console or prog code as well as accessed directly
in C code.

The user can access cvars from the console in three ways:
r_draworder			prints the current value
r_draworder 0		sets the current value to 0
set r_draworder 0	as above, but creates the cvar if not present

Cvars are restricted from having the same names as commands to keep this
interface from being ambiguous.

The are also occasionally used to communicated information between different
modules of the program.

*/

cvar_t *Cvar_Get( const char *var_name, const char *value, int flags );
// creates the variable if it doesn't exist, or returns the existing one
// if it exists, the value will not be changed, but flags will be ORed in
// that allows variables to be unarchived without needing bitflags
// if value is "", the value will not override a previously set value.

void	Cvar_Register( vmCvar_t *vmCvar, const char *varName, const char *defaultValue, int flags );
// basically a slightly modified Cvar_Get for the interpreted modules

void	Cvar_Update( vmCvar_t *vmCvar );
// updates an interpreted modules' version of a cvar; one it may not read
// (VM_PrivateCvarFlag) reads as empty

void 	Cvar_Set( const char *var_name, const char *value );
// will create the variable with no flags if it doesn't exist

cvar_t	*Cvar_Set2(const char *var_name, const char *value, qboolean force);
// same as Cvar_Set, but allows more control over setting of cvar

typedef enum {
	CVAR_SOURCE_DEFAULT,	// no one's: the default (Cvar_Source)
	CVAR_SOURCE_ENGINE,	// engine code (Cvar_Set): saved if archived
	CVAR_SOURCE_GAME,	// game code, and a server running here (Cvar_SetFromVM, Cvar_SetSafe, restricted text): saved if archived
	CVAR_SOURCE_PLAYER,	// the console and configs: saved if archived
	CVAR_SOURCE_MENU,	// the menus, the ui module's sets: the player's choice, saved if archived
	CVAR_SOURCE_SCRIPT,	// what runs at every start (default.cfg, autoexec.cfg): not saved
	CVAR_SOURCE_SESSION,	// the command line: for this run only
	CVAR_SOURCE_SERVER,	// a server's requirement, until Cvar_EndServerValues drops it
	CVAR_SOURCE_SYSTEM	// the system's or browser's own change (fullscreen, a maximized window's size): for this run only
} cvarSource_t;

cvar_t	*Cvar_SetFrom( const char *var_name, const char *value, cvarSource_t source, qboolean force );
// a set, in the layer its source gives (cvar_t's serverString and the rest).
// A server may not set protected or private cvars (as Cvar_SetSafe)

void	Cvar_SetReason( cvar_t *var, const char *reason );
// its owner's word on why what's so isn't what the cvar wants, which
// cvar_why prints; NULL or "" when it is

cvarSource_t Cvar_Source( const cvar_t *var );
// whose value a cvar has

void	Cvar_BeginServerValues( void );
void	Cvar_EndServerValues( void );
// around a server's systeminfo: what it no longer requires is dropped at the
// end, and the player's own value comes back, and a cvar it created that no
// code registered goes; with nothing between them, on leaving the server

typedef struct {
	const char	*name;
	const char	*value;
} cvarDefault_t;

// Where a cvar is saved, by its declaration (docs/design/state.md, Scopes)
typedef enum {
	CVAR_SCOPE_NONE,	// never saved
	CVAR_SCOPE_PLAYER,	// the player's, everywhere
	CVAR_SCOPE_PLAYER_MOD,	// the player's, for one mod
	CVAR_SCOPE_DEVICE,	// this device's
	CVAR_SCOPE_DEVICE_MOD,	// this device's, for one mod
	CVAR_SCOPE_SERVER	// a server's own
} cvarScope_t;

#define CVAR_SCOPE_BIT( scope )	( 1 << ( scope ) )

typedef struct {
	const char	*name;
	cvarScope_t	scope;
	qboolean	clientOnly;	// it means nothing to a dedicated server
	// what describes it, as the game's declarations write it, each NULL
	// where they don't say (docs/design/settings-descriptions.md)
	const char	*role;		// "server", "preference", "tuning", "state", "capability"
	const char	*owner;		// the code it belongs to: "game", "bots", "server", "renderer"...
	const char	*tier;		// "basic", "notable", "all"
	const char	*type;		// "bool", "enum", "bits", "int", "number", "text", "password"
	const char	*unit;		// what a number counts, as said after it: "frames a second", "ms"
	const char	*applies;	// "at once", "next map", "restart"; NULL: as its flags say
} cvarDeclaration_t;

extern const cvarDeclaration_t cvar_declarations[];
// the game's, sorted as Q_stricmp compares, ending with a NULL name: the
// build's CVAR_DECLARATIONS_SOURCE, by default an empty table

void	Cvar_SetDeclarations( const cvarDeclaration_t *declarations );
// before any cvar is registered
int	Cvar_DeclaredScope( const char *var_name );
// a name's declared scope, whether or not the cvar exists, or -1

void	Cvar_NoteOrigin( cvar_t *var, const char *origin );
// where a text set of a cvar came from, "server.cfg:12", for cvar_why

void	Cvar_WarnConfigs( void );
// warns of config lines that set a cvar to no effect, once the game is up
void	Cvar_PrintSetLog( void );
// the sets a config's lines make, kept and printed for --check

typedef struct {
	const char	*name;
	const char	*programs;	// "Quake3e", "CNQ3", "CNQ3 and Quake3e"
} cvarForeign_t;

extern const cvarForeign_t cvar_foreignNames[];
// the cvars other engines register that this one doesn't, by the programs
// that do, sorted as Q_stricmp compares, ending with a NULL name: given by
// the build with the declarations (CVAR_DECLARATIONS_SOURCE), empty by default
const char	*Cvar_ForeignPrograms( const char *var_name );
// the programs a cvar this engine doesn't have is from, or NULL

extern const cvarDefault_t cvar_q3Defaults[];
extern const cvarDefault_t cvar_ioq3Defaults[];
extern const cvarDefault_t cvar_cnq3Defaults[];
extern const cvarDefault_t cvar_q3eDefaults[];
// the defaults stock Quake III's, ioquake3's, CNQ3's and Quake3e's configs
// hold their saved cvars at, which the importer leaves out as not chosen:
// a name once per default, ending with a NULL name; given by the build
// with the declarations (CVAR_DECLARATIONS_SOURCE), empty by default
extern const cvarDefault_t cvar_q3ServerDefaults[];
extern const cvarDefault_t cvar_ioq3ServerDefaults[];
extern const cvarDefault_t cvar_cnq3ServerDefaults[];
extern const cvarDefault_t cvar_q3eServerDefaults[];
// and those only their dedicated servers have, which a dedicated server's
// saved config holds too (ioquake3's net_enabled 1), as the above
extern const char * const cvar_ioq3Marks[];
extern const char * const cvar_cnq3Marks[];
extern const char * const cvar_q3eMarks[];
// the cvars each saves that no other program registers, which tell its
// configs from stock's, with the same first line; ending with NULL, and
// given by the build as the defaults are
cvarScope_t Cvar_Scope( const cvar_t *var );
// where a cvar is saved: its declaration's scope; one not declared that a
// mod or the player made, the player's for the mod running; one the engine
// made that isn't declared, the device's, which doesn't travel

void	Cvar_SetProfile( const cvarDefault_t *defaults );
// this platform's defaults where they differ from the engine's, ending with a
// NULL name, for the cvars registered after

void	Cvar_SetSafe( const char *var_name, const char *value );
// sometimes we set variables from an untrusted source: fail if flags &
// CVAR_PROTECTED, or CVAR_PRIVATE (but for password)
void	Cvar_ResetSafe( const char *var_name );
qboolean Cvar_RunsRestricted( const char *var_name );
qboolean Cvar_SetByPak( const char *var_name );
// a pak's config on a dedicated server set it last (Cmd_IsPakConfig)
// whether a cvar's value, run as commands, runs restricted: with the
// rights of whoever set it
qboolean Cvar_AllowedFromText( const char *var_name );
// false, with a message, if the running command is restricted and the
// cvar private or protected
void	Cvar_SetFromVM( const char *var_name, const char *value, const char * const *allowed, cvarSource_t source );
void	Cvar_SetValueFromVM( const char *var_name, float value, const char * const *allowed, cvarSource_t source );
// a game module's set: as Cvar_SetSafe, and an engine cvar that is read
// only, set at startup or cheat protected keeps its value, unless its
// name is in allowed

void Cvar_SetLatched( const char *var_name, const char *value);
// don't set the cvar immediately

void	Cvar_SetValue( const char *var_name, float value );

qboolean	Cvar_SameValue( const char *a, const char *b );
// whether two values are the same, as numbers if both are

int	Cvar_ServerCreated( int *bytes );
// how many cvars a server created that no code has registered, and the
// bytes their names and values take

float	Cvar_VariableValue( const char *var_name );
int		Cvar_VariableIntegerValue( const char *var_name );
// returns 0 if not defined or non numeric

char	*Cvar_VariableString( const char *var_name );
void	Cvar_VariableStringBuffer( const char *var_name, char *buffer, int bufsize );
// returns an empty string if not defined

float	Cvar_VariableValueSafe( const char *var_name );
int		Cvar_VariableIntegerValueSafe( const char *var_name );
void	Cvar_VariableStringBufferSafe( const char *var_name, char *buffer, int bufsize );
// for game code: a cvar it may not read (VM_PrivateCvarFlag) reads as one
// that isn't defined

int	Cvar_Flags(const char *var_name);
// returns CVAR_NONEXISTENT if cvar doesn't exist or the flags of that particular CVAR.

void	Cvar_CommandCompletion( void(*callback)(const char *s) );
// callback with each valid string

void 	Cvar_Reset( const char *var_name );
void 	Cvar_ForceReset(const char *var_name);

void	Cvar_SetCheatState( void );
// reset all testing vars to a safe value

qboolean Cvar_Command( void );
// called by Cmd_ExecuteString when Cmd_Argv(0) doesn't match a known
// command.  Returns true if the command was a variable reference that
// was handled. (print or change)

// a config's text, built in memory, so a file is written only when what it
// would hold changed
typedef struct {
	char	*text;	// Z_Malloc'd; NULL until something is appended
	int	length;
	int	size;
} configText_t;

void Com_ConfigAppend( configText_t *config, const char *text );

// every scope that's saved: all but none's
#define CVAR_SCOPES_SAVED	( ~CVAR_SCOPE_BIT( CVAR_SCOPE_NONE ) )

void 	Cvar_WriteVariables( configText_t *config, int hideFlags, int scopes );
// appends a "seta" line for each archived cvar of the scopes asked for
// (CVAR_SCOPE_BIT) whose saved value isn't its default, sorted by name, but
// those with any of hideFlags

void	Cvar_Init( void );

char	*Cvar_InfoString( int bit );
char	*Cvar_InfoString_Big( int bit );
// returns an info string containing all the cvars that have the given bit set
// in their flags ( CVAR_USERINFO, CVAR_SERVERINFO, CVAR_SYSTEMINFO, etc )
void	Cvar_InfoStringBuffer( int bit, char *buff, int buffsize );
void	Cvar_InfoStringBufferSafe( int bit, char *buff, int buffsize );
// private cvars are only ever in the userinfo, and the Safe version, for
// game code, leaves out the cvars it may not read (VM_PrivateCvarFlag)
void Cvar_CheckRange( cvar_t *cv, float minVal, float maxVal, qboolean shouldBeIntegral );
void Cvar_SetDescription( cvar_t *var, const char *var_description );
// the game modules, as the cvars they describe and bound tell them apart
typedef enum {
	CVAR_MODULE_GAME = 1,
	CVAR_MODULE_CGAME = 2,
	CVAR_MODULE_UI = 4
} cvarModule_t;

void Cvar_SetDescriptionByName( cvarModule_t module, const char *var_name, const char *var_description );
void Cvar_SetRangeByName( cvarModule_t module, const char *var_name, int type, const char *min, const char *max );
// what the module gave cvars goes, and each has its own description and
// range again; VM_Free's, for every module that unloads
void Cvar_ModuleUnloaded( int module );

void	Cvar_Restart(qboolean unsetVM);
void	Cvar_Restart_f( void );

void Cvar_CompleteCvarName( char *args, int argNum );

extern	int			cvar_modifiedFlags;
// whenever a cvar is modifed, its flags will be OR'd into this, so
// a single check can determine if any CVAR_USERINFO, CVAR_SERVERINFO,
// etc, variables have been modified since the last check.  The bit
// can then be cleared to allow another change detection.

/*
==============================================================

FILESYSTEM

No stdio calls should be used by any part of the game, because
we need to deal with all sorts of directory and seperator char
issues.
==============================================================
*/

// referenced flags
// these are in loop specific order so don't change the order
#define FS_GENERAL_REF	0x01
#define FS_UI_REF		0x02
#define FS_CGAME_REF	0x04
// number of id paks that will never be autodownloaded from baseq3/missionpack
#define NUM_ID_PAKS		9
#define NUM_TA_PAKS		4

#define	MAX_FILE_HANDLES	64

#ifdef DEDICATED
#	define Q3CONFIG_CFG CONFIG_PREFIX "_server.cfg"
#else
#	define Q3CONFIG_CFG CONFIG_PREFIX ".cfg"
#endif

// which of the engine's changed defaults a config was written after
// (Com_WriteConfigLines)
#define COM_CONFIG_VERSION	"1"

// a config's first line, the engine's own (settings/'s files among them),
// which the importer tells it by (Com_ConfigWriter)
#define COM_CONFIG_HEADER	"// generated by " PRODUCT_NAME ","

qboolean FS_Initialized( void );

void	FS_InitFilesystem ( void );
void	FS_Shutdown( qboolean closemfp );

qboolean FS_ConditionalRestart(int checksumFeed, qboolean disconnect);
void	FS_Restart( int checksumFeed );
// shutdown and restart the filesystem so changes to fs_gamedir can take effect

char	**FS_ListFiles( const char *directory, const char *extension, int *numfiles );
// directory should not have either a leading or trailing /
// if extension is "/", only subdirectories will be returned
// the returned files will not include any directories or /

void	FS_FreeFileList( char **list );

qboolean FS_FileExists_HomeData( const char *file );

qboolean FS_CreatePath (const char *OSPath);

int FS_FindVM(void **startSearch, char *found, int foundlen, const char *name, int enableDll);

char	*FS_BaseDir_BuildOSPath( const char *base, const char *qpath );
char	*FS_BuildOSPath( const char *base, const char *game, const char *qpath );
qboolean FS_CompareZipChecksum(const char *zipfile);

int		FS_LoadStack( void );

int		FS_GetFileList(  const char *path, const char *extension, char *listbuf, int bufsize );
int		FS_GetModList(  char *listbuf, int bufsize );

void	FS_GetModDescription( const char *modDir, char *description, int descriptionLen );

fileHandle_t	FS_FOpenFileWrite_HomeConfig( const char *filename );
fileHandle_t	FS_FOpenFileWrite_HomeData( const char *filename );
fileHandle_t	FS_FOpenFileWrite_HomeState( const char *filename );
fileHandle_t	FS_FOpenFileAppend_HomeData( const char *filename );
qboolean	FS_RotateFile_HomeData( const char *filename, long maxBytes );
// the file becomes <filename>.1 where it's larger than maxBytes
fileHandle_t	FS_FCreateOpenPipeFile( const char *filename );
// will properly create any needed paths and deal with seperater character issues

fileHandle_t FS_BaseDir_FOpenFileWrite_HomeConfig( const char *filename );
fileHandle_t FS_BaseDir_FOpenFileWrite_HomeData( const char *filename );
fileHandle_t FS_BaseDir_FOpenFileWrite_HomeState( const char *filename );
long		FS_BaseDir_FOpenFileRead( const char *filename, fileHandle_t *fp );
long		FS_BaseDir_ReadFile_HomeConfig( const char *filename, void **buffer );
// reads a whole file from the home's config directory only, into a buffer
// to Z_Free, ending with a 0; -1 if there's none
const char	*FS_SearchDirs( const char *predecessorFile );
// the search path's directories, quoted, as a list for a message, with the
// predecessor's homes where FS_ReadPredecessorConfig looked for
// predecessorFile too (NULL when it wasn't asked to)
long		FS_ReadPredecessorConfig( const char *filename, void **buffer, char *ospath, int size );
// the same for a config in the predecessor's game directory, read only,
// where a dedicated server that ran before this one kept its server.cfg
const char	*FS_LoadedGameDir( void );
// the game directory loaded, which fs_game names before a restart loads it
void	FS_BaseDir_Rename_HomeData( const char *from, const char *to, qboolean safe );
long		FS_FOpenFileRead( const char *qpath, fileHandle_t *file, qboolean uniqueFILE );
// if uniqueFILE is true, then a new FILE will be fopened even if the file
// is found in an already open pak file.  If uniqueFILE is false, you must call
// FS_FCloseFile instead of fclose, otherwise the pak FILE would be improperly closed
// It is generally safe to always set uniqueFILE to true, because the majority of
// file IO goes through FS_ReadFile, which Does The Right Thing already.

const char	*FS_SkipPathPrefix( const char *qpath );
// a game path without its leading slashes and "./", which the search ignores

qboolean	FS_IsEngineFile( const char *qpath );
// whether the file is one the engine keeps for itself in a game directory:
// the configs it runs at startup, the console and crash logs, the CD key
// and the command pipe

qboolean	FS_LastFileIsGameContent( void );
// whether the last file FS_FOpenFileRead, and so FS_ReadFile, found is in a
// pk3 or a pk3dir, not a game directory
const char	*FS_LastFilePath( const char *qpath );
// where the last file found was, its path or its pak's
const char	*FS_LastFileContent( void );
// the pk3 or pk3dir the last file read is in ("pak0.pk3"), or ""

int		FS_FileIsInPAK(const char *filename, int *pChecksum );
// returns 1 if a file is in the PAK file, otherwise -1

int		FS_Write( const void *buffer, int len, fileHandle_t f );

int		FS_Read( void *buffer, int len, fileHandle_t f );
// properly handles partial reads and reads from other dlls

void	FS_FCloseFile( fileHandle_t f );
// note: you can't just fclose from another DLL, due to MS libc issues

long	FS_ReadFileDir(const char *qpath, void *searchPath, qboolean unpure, void **buffer);
long	FS_ReadFile(const char *qpath, void **buffer);
// returns the length of the file
// a null buffer will just return the file length without loading
// as a quick check for existence. -1 length == not present
// A 0 byte will always be appended at the end, so string ops are safe.
// the buffer should be considered read-only, because it may be cached
// for other uses.

void	FS_ForceFlush( fileHandle_t f );
// forces flush on files we're writing to.

void	FS_FreeFile( void *buffer );
// frees the memory returned by FS_ReadFile

void	FS_WriteFile( const char *qpath, const void *buffer, int size );
// writes a complete file, creating any subdirectories needed

long FS_filelength(fileHandle_t f);
// doesn't work for files that are opened from a pack file

int		FS_FTell( fileHandle_t f );
// where are we?

void	FS_Flush( fileHandle_t f );

void 	QDECL FS_Printf( fileHandle_t f, const char *fmt, ... ) Q_PRINTF_FUNC(2, 3);
// like fprintf

int		FS_FOpenFileByMode( const char *qpath, fileHandle_t *f, fsMode_t mode );
// a module's files: only the module that opened a handle may use it
int		FS_VM_FOpenFile( const vm_t *vm, const char *qpath, fileHandle_t *f, fsMode_t mode );
qboolean	FS_HandleOwnedBy( fileHandle_t f, const vm_t *vm );
void	FS_VM_CloseFiles( const vm_t *vm );
// opens a file for reading, writing, or appending depending on the value of mode

int		FS_Seek( fileHandle_t f, long offset, int origin );
// seek on a file

qboolean FS_FilenameCompare( const char *s1, const char *s2 );

qboolean FS_RetailPak( const char *download, char *retail, int size );
// whether a download's pak ("<game>/<name>") is one of the game's own, known
// by checksum, and its own name

const char *FS_LoadedPakNames( void );
const char *FS_LoadedPakChecksums( void );
const char *FS_LoadedPakPureChecksums( void );
// Returns a space separated string containing the checksums of all loaded pk3 files.
// Servers with sv_pure set will get this string and pass it to clients.

const char *FS_ReferencedPakNames( void );
const char *FS_ReferencedPakChecksums( void );
const char *FS_ReferencedPakPureChecksums( void );
// Returns a space separated string containing the checksums of all loaded 
// AND referenced pk3 files. Servers with sv_pure set will get this string 
// back from clients for pure validation 

void FS_ClearPakReferences( int flags );
// clears referenced booleans on loaded pk3s

void FS_PureServerSetReferencedPaks( const char *pakSums, const char *pakNames );
void FS_PureServerSetLoadedPaks( const char *pakSums, const char *pakNames );
// If the string is empty, all data sources will be allowed.
// If not empty, only pk3 files that match one of the space
// separated checksums will be checked for files, with the
// sole exception of .cfg files.

qboolean FS_CheckDirTraversal(const char *checkdir);
qboolean FS_InvalidGameDir(const char *gamedir);
qboolean FS_ComparePaks( char *neededpaks, int len, qboolean dlstring );

void FS_Remove( const char *osPath );
void FS_Remove_HomeData( const char *homePath );

void	FS_FilenameCompletion( const char *dir, const char *ext, char *filter,
		qboolean stripExt, void(*callback)(const char *s), qboolean allowNonPureFilesOnDisk );

const char *FS_GetCurrentGameDir(void);
const char *FS_ServerDir( void );
// a dedicated server's own directory in the game directory, "servers/<port>",
// "" on a client
const char *FS_ServerFile( const char *filename );
// filename in it, through va(); the name alone on a client
qboolean FS_Which(const char *filename, void *searchPath);

/*
==============================================================

Edit fields and command line history/completion

==============================================================
*/

#define	MAX_EDIT_LINE	256
typedef struct {
	int		cursor;
	int		scroll;
	int		widthInChars;
	char	buffer[MAX_EDIT_LINE];
} field_t;

void Field_Clear( field_t *edit );
void Field_AutoComplete( field_t *edit );
void Field_CompleteKeyname( void );
void Field_CompleteFilename( const char *dir, const char *ext,
		char *filter, qboolean stripExt,
		qboolean allowNonPureFilesOnDisk );
void Field_CompleteCommand( char *cmd,
		qboolean doCommands, qboolean doCvars );
void Field_CompletePlayerName( const char **names, int count );

/*
==============================================================

MISC

==============================================================
*/

// centralizing the declarations for cl_cdkey
// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=470
extern char cl_cdkey[34];

// returned by Sys_GetProcessorFeatures
typedef enum
{
  CF_RDTSC      = 1 << 0,
  CF_MMX        = 1 << 1,
  CF_MMX_EXT    = 1 << 2,
  CF_3DNOW      = 1 << 3,
  CF_3DNOW_EXT  = 1 << 4,
  CF_SSE        = 1 << 5,
  CF_SSE2       = 1 << 6,
  CF_ALTIVEC    = 1 << 7
} cpuFeatures_t;

// centralized and cleaned, that's the max string you can send to a Com_Printf / Com_DPrintf (above gets truncated)
#define	MAXPRINTMSG	4096


typedef enum {
	// SE_NONE must be zero
	SE_NONE = 0,		// evTime is still valid
	SE_KEY,			// evValue is a key code, evValue2 is the down flag
	SE_CHAR,		// evValue is an ascii char
	SE_MOUSE,		// evValue and evValue2 are relative signed x / y moves
	SE_JOYSTICK_AXIS,	// evValue is an axis number and evValue2 is the current state (-127 to 127)
	SE_CONSOLE		// evPtr is a char*
} sysEventType_t;

typedef struct {
	int				evTime;
	sysEventType_t	evType;
	int				evValue, evValue2;
	int				evPtrLength;	// bytes of data pointed to by evPtr, for journaling
	void			*evPtr;			// this must be manually freed if not NULL
} sysEvent_t;

void		Com_QueueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr );
int			Com_EventLoop( void );
sysEvent_t	Com_GetSystemEvent( void );

char		*CopyString( const char *in );
void		Info_Print( const char *s );

void		Com_BeginRedirect (char *buffer, int buffersize, void (*flush)(char *));
void		Com_EndRedirect( void );
qboolean	Com_IsRedirecting( void );
void		Com_NoteConfigRun( const char *name, const char *from );
void		Com_PrintConfigsRun( void );
// the configs --check's startup ran, and from where
void 		QDECL Com_Printf( const char *fmt, ... ) Q_PRINTF_FUNC(1, 2);
void		Com_PrintLines( const char *msg, qboolean *lineStart, int level,
	void (*write)( const char *text ) );
void 		QDECL Com_DPrintf( const char *fmt, ... ) Q_PRINTF_FUNC(1, 2);
void 		QDECL Com_Error( int code, const char *fmt, ... ) Q_NO_RETURN Q_PRINTF_FUNC(2, 3);
void 		QDECL Com_ErrorExit( int status, const char *fmt, ... ) Q_NO_RETURN Q_PRINTF_FUNC(2, 3);

// Exit statuses for service managers, besides 0 for a quit or a requested
// stop, 2 for any other signal, and 3 for a fatal error (Sys_Error)
#define EXIT_MISSING_DATA	4	// the game's data isn't installed
#define EXIT_NO_NETWORK		5	// a dedicated server can't open its port
void 		Com_Quit_f( void ) Q_NO_RETURN;
void		Com_Quit( char *reason ) Q_NO_RETURN;
void		Com_GameRestart(int checksumFeed, qboolean disconnect);
void		Com_QueueReload( void );
// SIGHUP's: the next frame runs the command line's configs again (reload),
// safe to call from a signal handler

int			Com_Milliseconds( void );	// will be journaled properly
unsigned	Com_BlockChecksum( const void *buffer, int length );
char		*Com_MD5File(const char *filename, int length, const char *prefix, int prefix_len);
int			Com_Filter(char *filter, char *name, int casesensitive);
int			Com_FilterPath(char *filter, char *name, int casesensitive);
int			Com_RealTime(qtime_t *qtime);
qboolean	Com_SafeMode( void );
void		Com_RunAndTimeServerPacket(netadr_t *evFrom, msg_t *buf);

qboolean	Com_IsVoipTarget(uint8_t *voipTargets, int voipTargetsSize, int clientNum);

void		Com_StartupVariable( const char *match );
// checks for and removes command line "+set var arg" constructs
// if match is NULL, all set commands will be executed, otherwise
// only a set with the exact name.  Only used during startup.

qboolean		Com_PlayerNameToFieldString( char *str, int length, const char *name );
qboolean		Com_FieldStringToPlayerName( char *name, int length, const char *rawname );
int QDECL	Com_strCompare( const void *a, const void *b );


extern	cvar_t	*com_developer;
extern	cvar_t	*com_dedicated;
extern	cvar_t	*com_speeds;
extern	cvar_t	*com_timescale;
extern	cvar_t	*com_randomSeed;
extern	cvar_t	*com_sv_running;
extern	cvar_t	*com_cl_running;
extern	cvar_t	*com_version;
extern	cvar_t	*com_blood;
extern	cvar_t	*com_buildScript;		// for building release pak files
extern	cvar_t	*com_journal;
extern	cvar_t	*com_cameraMode;
extern	cvar_t	*com_ansiColor;
extern	cvar_t	*com_timestamps;
extern	cvar_t	*com_unfocused;
extern	cvar_t	*com_maxfpsUnfocused;
extern	cvar_t	*com_minimized;
extern	cvar_t	*com_maxfpsMinimized;
extern	cvar_t	*com_altivec;
extern	cvar_t	*com_standalone;
extern	cvar_t	*com_basegame;
extern	cvar_t	*com_homepath;

// both client and server must agree to pause
extern	cvar_t	*cl_paused;
extern	cvar_t	*sv_paused;

extern	cvar_t	*cl_packetdelay;
extern	cvar_t	*sv_packetdelay;

extern	cvar_t	*com_gamename;
extern	cvar_t	*com_protocol;
#ifdef LEGACY_PROTOCOL
extern	cvar_t	*com_legacyprotocol;
#endif
#ifndef DEDICATED
extern  cvar_t  *con_autochat;
#endif

// com_speeds times
extern	int		time_game;
extern	int		time_frontend;
extern	int		time_backend;		// renderer backend time

// com_speeds pacing, in microseconds, or -1 for none this frame
extern	int		time_late;			// the frame's start after it was due
extern	int		time_inputAge;		// the newest input's age as the usercmd is built
extern	int		time_swap;			// since the last frame's swap

extern	int		com_frameTime;

extern	qboolean	com_errorEntered;
extern	qboolean	com_fullyInitialized;
extern	qboolean	com_check;	// --check: report the startup, start nothing (sv_check.c)

extern	fileHandle_t	com_journalFile;
extern	fileHandle_t	com_journalDataFile;

typedef enum {
	TAG_FREE,
	TAG_GENERAL,
	TAG_BOTLIB,
	TAG_RENDERER,
	TAG_SMALL,
	TAG_STATIC
} memtag_t;

/*

--- low memory ----
server vm
server clipmap
---mark---
renderer initialization (shaders, etc)
UI vm
cgame vm
renderer map
renderer models

---free---

temp file loading
--- high memory ---

*/

#if !defined(NDEBUG) && !defined(BSPC)
	#define ZONE_DEBUG
#endif

#ifdef ZONE_DEBUG
#define Z_TagMalloc(size, tag)			Z_TagMallocDebug(size, tag, #size, __FILE__, __LINE__)
#define Z_Malloc(size)					Z_MallocDebug(size, #size, __FILE__, __LINE__)
#define S_Malloc(size)					S_MallocDebug(size, #size, __FILE__, __LINE__)
void *Z_TagMallocDebug( int size, int tag, char *label, char *file, int line );	// NOT 0 filled memory
void *Z_MallocDebug( int size, char *label, char *file, int line );			// returns 0 filled memory
void *S_MallocDebug( int size, char *label, char *file, int line );			// returns 0 filled memory
#else
void *Z_TagMalloc( int size, int tag );	// NOT 0 filled memory
void *Z_Malloc( int size );			// returns 0 filled memory
void *S_Malloc( int size );			// NOT 0 filled memory only for small allocations
#endif
void Z_Free( void *ptr );
void Z_FreeTags( int tag );
int Z_AvailableMemory( void );
void Z_LogHeap( void );

void Hunk_Clear( void );
void Hunk_ClearToMark( void );
void Hunk_SetMark( void );
qboolean Hunk_CheckMark( void );
void Hunk_ClearTempMemory( void );
void *Hunk_AllocateTempMemory( int size );
void Hunk_FreeTempMemory( void *buf );
int	Hunk_MemoryRemaining( void );
void Hunk_Log( void);

void Com_TouchMemory( void );

// commandLine should not include the executable name (argv[0])
void Com_Init( char *commandLine );
qboolean Com_FrameDue( void );
qboolean Com_WaitFrame( void );
void Com_Frame( void );
void Com_Shutdown( void );


/*
==============================================================

CLIENT / SERVER SYSTEMS

==============================================================
*/

//
// client interface
//
void CL_InitKeyCommands( void );
// the keyboard binding interface must be setup before execing
// config files, but the rest of client startup will happen later

void CL_Init( void );
void CL_Disconnect( qboolean showMainMenu );
// called as an error drops the client, before it disconnects
void CL_NextDemoAfterError( int code );
void CL_Shutdown(char *finalmsg, qboolean disconnect, qboolean quit);
void CL_Frame( int msec );

// whether the renderer is ready for a new frame (re.FrameReady)
qboolean CL_FrameReady( void );
// whether the server moves the player by the frame (Com_MovesByFrame), so
// the frame rate sets jumps
qboolean CL_MovesByFrame( void );
qboolean CL_GameCommand( void );
void CL_KeyEvent (int key, qboolean down, unsigned time);

void CL_CharEvent( int key );
// char events are for field typing, not game control

void CL_MouseEvent( int dx, int dy, int time );

void CL_JoystickEvent( int axis, int value, int time );

void CL_PacketEvent( netadr_t from, msg_t *msg );

void CL_ConsolePrint( char *text );

void CL_MapLoading( void );
// do a screen update before starting to load a map
// when the server is going to load a new map, the entire hunk
// will be cleared, so the client must shutdown cgame, ui, and
// the renderer

void	CL_ForwardCommandToServer( const char *string );
// adds the current command line as a clc_clientCommand to the client message.
// things like godmode, noclip, etc, are commands directed to the server,
// so when they are typed in at the console, they will need to be forwarded.

void CL_CDDialog( void );
// bring up the "need a cd to play" dialog

void CL_FlushMemory( void );
// dump all memory on an error

void CL_ShutdownAll(qboolean shutdownRef);
// shutdown client

void CL_InitRef(void);
// initialize renderer interface

void CL_StartHunkUsers( qboolean rendererOnly );
// start all the client stuff using the hunk

void CL_Snd_Shutdown(void);
// Restart sound subsystem

void Key_KeynameCompletion( void(*callback)(const char *s) );
// for keyname autocompletion

void Key_WriteBindings( configText_t *config );
// for writing the config files

void S_ClearSoundBuffer( void );
// call before filesystem access

void SCR_DebugGraph (float value);	// FIXME: move logging to common?

// AVI files have the start of pixel lines 4 byte-aligned
#define AVI_LINE_PADDING 4

//
// server interface
//
void SV_Init( void );
void SV_Shutdown( char *finalmsg );
void SV_Frame( int msec );
void SV_PacketEvent( netadr_t from, msg_t *msg );
int SV_FrameMsec(void);
int SV_FrameLength( void );
qboolean SV_GameCommand( void );
int SV_SendQueuedPackets(void);

//
// UI interface
//
qboolean UI_GameCommand( void );
qboolean UI_usesUniqueCDKey(void);

//
// input interface
//
void IN_Init( void *windowData );
void IN_Frame( void );
void IN_Shutdown( void );
void IN_Restart( void );
qboolean IN_GetSafeArea( int rect[4] );
int64_t IN_NewestInput( void );	// the newest key or mouse event, in Sys_Nanoseconds

/*
==============================================================

NON-PORTABLE SYSTEM SERVICES

==============================================================
*/

#define MAX_JOYSTICK_AXIS 16

qboolean Com_IsClient( void );
// whether this process is a client, rather than a dedicated server: decided
// from the command line, before the first config runs

void	Sys_Init (void);
qboolean Sys_TouchDevice( void );
// whether the main pointer is a finger, as on a phone or tablet

// general development dll loading for virtual machine testing
void	* QDECL Sys_LoadGameDll( const char *name, vmMainProc *entryPoint,
				  intptr_t (QDECL *systemcalls)(intptr_t, ...) );
void	Sys_UnloadDll( void *dllHandle );

qboolean Sys_DllExtension( const char *name );

char	*Sys_GetCurrentUser( void );

void	QDECL Sys_Error( const char *error, ...) Q_NO_RETURN Q_PRINTF_FUNC(1, 2);
void	Sys_ErrorExit( int status, const char *message ) Q_NO_RETURN;
void	Sys_Quit (void) Q_NO_RETURN;
char	*Sys_GetClipboardData( void );	// note that this isn't journaled...

void	Sys_Print( const char *msg );
void	Sys_Notify( const char *state );	// to the service manager, as sd_notify
void	Sys_PrintFirewall( int port );	// whether Windows Firewall lets players in

// Sys_Milliseconds should only be used for profiling purposes,
// any game related timing information should come from event timestamps.
// It's Sys_Nanoseconds, a monotonic clock from the first call, in whole
// milliseconds.
int		Sys_Milliseconds (void);
int64_t	Sys_Nanoseconds( void );

#ifndef DEDICATED
// sleeps for ns nanoseconds, to within a microsecond or so (SDL_DelayPrecise)
void	Sys_SleepPrecise( int64_t ns );
// the refresh interval of the display the game is on, in nanoseconds; 0
// when it isn't known
int64_t	Sys_RefreshInterval( void );
#endif

// com_maxfps's values besides a number of frames a second and 0 (no cap):
// the display's refresh rate, and 3% under it, for a display of variable
// refresh rate with vsync (com_pacing.c)
#define	COM_MAXFPS_DISPLAY			-1
#define	COM_MAXFPS_BELOW_DISPLAY	-2

int64_t	Com_RefreshNanoseconds( int numerator, int denominator );
int64_t	Com_CapInterval( int fps );
int64_t	Com_MaxFpsInterval( int maxfps, int64_t refresh, qboolean movesByFrame );
qboolean	Com_MovesByFrame( const char *pmoveFixed, const char *gamename );

qboolean Sys_RandomBytes( byte *string, int len );

// the system console is shown when a dedicated server is running
void	Sys_DisplaySystemConsole( qboolean show );

cpuFeatures_t Sys_GetProcessorFeatures( void );

void	Sys_SetErrorText( const char *text );

void	Sys_SendPacket( int length, const void *data, netadr_t to );

qboolean	Sys_StringToAdr( const char *s, netadr_t *a, netadrtype_t family );
//Does NOT parse port numbers, only base addresses.

qboolean	Sys_IsLANAddress (netadr_t adr);
void		Sys_ShowIP(void);

FILE	*Sys_FOpen( const char *ospath, const char *mode );
qboolean Sys_Mkdir( const char *path );
FILE	*Sys_Mkfifo( const char *ospath );
char	*Sys_Cwd( void );
void	Sys_SetDefaultInstallPath(const char *path);
char	*Sys_DefaultInstallPath(void);
char	*Sys_SteamPath(void);
char	*Sys_GogPath(void);
char	*Sys_MicrosoftStorePath(void);

#ifdef __APPLE__
char    *Sys_DefaultAppPath(void);
#endif

char	*Sys_DefaultHomeConfigPath(void);
char	*Sys_DefaultHomeDataPath(void);
char	*Sys_DefaultHomeStatePath(void);
// the config and data homes of the product this build succeeds
// (HOMEPATH_NAME_PREDECESSOR), or ""
void	Sys_PredecessorHomePaths( const char **configPath, const char **dataPath );
const char *Sys_Dirname( char *path );
const char *Sys_Basename( char *path );
char *Sys_ConsoleInput(void);

char **Sys_ListFiles( const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs );
void	Sys_FreeFileList( char **list );
void	Sys_Sleep(int msec);

qboolean Sys_LowPhysicalMemory( void );
// keeps the system from sleeping when idle, or lets it again; returns
// whether the system can be kept awake (macOS)
qboolean Sys_KeepAwake( qboolean awake );

void Sys_SetEnv(const char *name, const char *value);

typedef enum
{
	DR_YES = 0,
	DR_NO = 1,
	DR_OK = 0,
	DR_CANCEL = 1
} dialogResult_t;

typedef enum
{
	DT_INFO,
	DT_WARNING,
	DT_ERROR,
	DT_YES_NO,
	DT_OK_CANCEL
} dialogType_t;

dialogResult_t Sys_Dialog( dialogType_t type, const char *message, const char *title );
qboolean Sys_OpenFolderInFileManager( const char *path, qboolean create );

void Sys_RemovePIDFile( void );
void Sys_InitPIDFile( const char *gamedir );

/* This is based on the Adaptive Huffman algorithm described in Sayood's Data
 * Compression book.  The ranks are not actually stored, but implicitly defined
 * by the location of a node within a doubly-linked list */

#define NYT HMAX					/* NYT = Not Yet Transmitted */
#define INTERNAL_NODE (HMAX+1)

typedef struct nodetype {
	struct	nodetype *left, *right, *parent; /* tree structure */ 
	struct	nodetype *next, *prev; /* doubly-linked list */
	struct	nodetype **head; /* highest ranked node in block */
	int		weight;
	int		symbol;
} node_t;

#define HMAX 256 /* Maximum symbol */

typedef struct {
	int			blocNode;
	int			blocPtrs;

	node_t*		tree;
	node_t*		lhead;
	node_t*		ltail;
	node_t*		loc[HMAX+1];
	node_t**	freelist;

	node_t		nodeList[768];
	node_t*		nodePtrs[768];
} huff_t;

typedef struct {
	huff_t		compressor;
	huff_t		decompressor;
} huffman_t;

void	Huff_Compress(msg_t *buf, int offset);
void	Huff_Decompress(msg_t *buf, int offset);
void	Huff_Init(huffman_t *huff);
void	Huff_addRef(huff_t* huff, byte ch);
int		Huff_Receive (node_t *node, int *ch, byte *fin);
void	Huff_transmit (huff_t *huff, int ch, byte *fout, int maxoffset);
void	Huff_offsetReceive (node_t *node, int *ch, byte *fin, int *offset, int maxoffset);
void	Huff_offsetTransmit (huff_t *huff, int ch, byte *fout, int *offset, int maxoffset);
void	Huff_putBit( int bit, byte *fout, int *offset);
int		Huff_getBit( byte *fout, int *offset);

// don't use if you don't know what you're doing.
int		Huff_getBloc(void);
void	Huff_setBloc(int _bloc);


extern huffman_t clientHuffTables;

#define	SV_ENCODE_START		4
#define SV_DECODE_START		12
#define	CL_ENCODE_START		12
#define CL_DECODE_START		4

// flags for sv_allowDownload and cl_allowDownload
#define DLF_ENABLE 1
#define DLF_NO_REDIRECT 2
#define DLF_NO_UDP 4
#define DLF_NO_DISCONNECT 8

#endif // _QCOMMON_H_
