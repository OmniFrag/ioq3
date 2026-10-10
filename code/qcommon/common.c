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
// common.c -- misc functions used in client and server

#include "q_shared.h"
#include "qcommon.h"
#include <setjmp.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/stat.h> // umask
#else
#include <winsock.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

int demo_protocols[] =
{ 67, 66, 0 };

#define MAX_NUM_ARGVS	50

#define MIN_DEDICATED_COMHUNKMEGS 1
#define DEF_COMHUNKMEGS 	128
#define MIN_COMHUNKMEGS		DEF_COMHUNKMEGS
#define DEF_COMZONEMEGS		48
#define DEF_COMHUNKMEGS_S	XSTRING(DEF_COMHUNKMEGS)
#define DEF_COMZONEMEGS_S	XSTRING(DEF_COMZONEMEGS)

int		com_argc;
char	*com_argv[MAX_NUM_ARGVS+1];

jmp_buf abortframe;		// an ERR_DROP occurred, exit the entire frame


FILE *debuglogfile;
static fileHandle_t pipefile;
static fileHandle_t logfile;
fileHandle_t	com_journalFile;			// events are written here
fileHandle_t	com_journalDataFile;		// config files are written here

cvar_t	*com_speeds;
cvar_t	*com_developer;
cvar_t	*com_dedicated;
cvar_t	*com_timescale;
cvar_t	*com_fixedtime;
cvar_t	*com_randomSeed;
cvar_t	*com_journal;
cvar_t	*com_maxfps;
cvar_t	*com_altivec;
cvar_t	*com_timedemo;
cvar_t	*com_sv_running;
cvar_t	*com_cl_running;
cvar_t	*com_logfile;		// 1 = buffer log, 2 = flush after each print
cvar_t	*com_pipefile;
cvar_t	*com_showtrace;
cvar_t	*com_version;
cvar_t	*com_blood;
cvar_t	*com_buildScript;	// for automated data building scripts
#ifdef CINEMATICS_INTRO
cvar_t	*com_introPlayed;
#endif
cvar_t	*cl_paused;
cvar_t	*sv_paused;
cvar_t  *cl_packetdelay;
cvar_t  *sv_packetdelay;
cvar_t	*com_cameraMode;
cvar_t	*com_ansiColor;
cvar_t	*com_timestamps;
cvar_t	*com_unfocused;
cvar_t	*com_maxfpsUnfocused;
cvar_t	*com_minimized;
cvar_t	*com_maxfpsMinimized;
#ifndef DEDICATED
cvar_t	*com_swapIntervalActive;
#endif
cvar_t	*com_abnormalExit;
cvar_t	*com_standalone;
cvar_t	*com_gamename;
cvar_t	*com_protocol;
#ifdef LEGACY_PROTOCOL
cvar_t	*com_legacyprotocol;
#endif
cvar_t	*com_basegame;
cvar_t  *com_homepath;
cvar_t	*com_busyWait;
#ifndef DEDICATED
cvar_t  *con_autochat;
#endif

#if idx64
	int (*Q_VMftol)(void);
#elif id386
	long (QDECL *Q_ftol)(float f);
	int (QDECL *Q_VMftol)(void);
	void (QDECL *Q_SnapVector)(vec3_t vec);
#endif

// com_speeds times
int		time_game;
int		time_frontend;		// renderer frontend time
int		time_backend;		// renderer backend time

// com_speeds pacing, in microseconds, or -1 for none this frame
int		time_late;			// the frame's start after it was due
int		time_inputAge;		// the newest input's age as the usercmd is built
int		time_swap;			// since the last frame's swap

int			com_frameTime;
int			com_frameNumber;

qboolean	com_errorEntered = qfalse;
qboolean	com_fullyInitialized = qfalse;
qboolean	com_check;	// --check: run the startup, report what it did and exit (sv_check.c)
qboolean	com_gameRestarting = qfalse;
qboolean	com_gameClientRestarting = qfalse;

char	com_errorMessage[MAXPRINTMSG];

void Com_WriteConfig_f( void );
void Com_WriteConfiguration( void );
static void Com_SettingsExport_f( void );
static void Com_SettingsImport_f( void );
void CIN_CloseAllVideos( void );

//============================================================================

static char	*rd_buffer;
static int	rd_buffersize;
static void	(*rd_flush)( char *buffer );

void Com_BeginRedirect (char *buffer, int buffersize, void (*flush)( char *) )
{
	if (!buffer || !buffersize || !flush)
		return;
	rd_buffer = buffer;
	rd_buffersize = buffersize;
	rd_flush = flush;

	*rd_buffer = 0;
}

/*
=============
Com_IsRedirecting

Whether what's printed goes back to rcon's sender, say
=============
*/
qboolean Com_IsRedirecting( void )
{
	return rd_buffer != NULL;
}

void Com_EndRedirect (void)
{
	if ( rd_flush ) {
		rd_flush(rd_buffer);
	}

	rd_buffer = NULL;
	rd_buffersize = 0;
	rd_flush = NULL;
}

/*
=============
Com_PrintLines

Hands write the text without the bytes that control a terminal (names
and chat reach here too). Where com_timestamps is level or more, a line at
a time, each starting with the date and time. *lineStart keeps whether the
output's next text starts a line.
=============
*/
void Com_PrintLines( const char *msg, qboolean *lineStart, int level,
	void (*write)( const char *text ) ) {
	static time_t	stampTime = -1;
	static char		stamp[32];
	char		buffer[MAXPRINTMSG];
	char		line[MAXPRINTMSG];
	const char	*p;
	int			length, stampLength;

	if ( !*msg ) {
		return;
	}

	if ( !com_timestamps || com_timestamps->integer < level ) {
		while ( *msg ) {
			msg += Q_FilterTerminalText( buffer, sizeof( buffer ), msg );
			write( buffer );
		}
		*lineStart = msg[-1] == '\n';
		return;
	}

	while ( *msg ) {
		length = strcspn( msg, "\n" );
		if ( msg[length] == '\n' ) {
			length++;
		}
		if ( length > sizeof( line ) - 1 ) {
			length = sizeof( line ) - 1;
		}
		Q_strncpyz( line, msg, length + 1 );
		msg += length;

		// the stamp and the line in one write
		stampLength = 0;
		if ( *lineStart ) {
			time_t	now = time( NULL );

			if ( now != stampTime ) {
				stampTime = now;
				strftime( stamp, sizeof( stamp ), "%Y-%m-%d %H:%M:%S ", localtime( &now ) );
			}
			Q_strncpyz( buffer, stamp, sizeof( buffer ) );
			stampLength = strlen( buffer );
		}
		*lineStart = line[length - 1] == '\n';

		for ( p = line; *p; stampLength = 0 ) {
			p += Q_FilterTerminalText( buffer + stampLength, sizeof( buffer ) - stampLength, p );
			write( buffer );
		}
	}
}

/*
=============
Com_LogWrite
=============
*/
static void Com_LogWrite( const char *text ) {
	FS_Write( text, strlen( text ), logfile );
}

/*
=============
Com_Printf

Both client and server can use this, and it will output
to the appropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
=============
*/
void QDECL Com_Printf( const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];
  static qboolean opening_qconsole = qfalse;


	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	if ( rd_buffer ) {
		if ((strlen (msg) + strlen(rd_buffer)) > (rd_buffersize - 1)) {
			rd_flush(rd_buffer);
			*rd_buffer = 0;
		}
		Q_strcat(rd_buffer, rd_buffersize, msg);
    // TTimo nooo .. that would defeat the purpose
		//rd_flush(rd_buffer);			
		//*rd_buffer = 0;
		return;
	}

#ifndef DEDICATED
	CL_ConsolePrint( msg );
#endif

	// echo to dedicated console and early console
	Sys_Print( msg );

	// logfile
	if ( com_logfile && com_logfile->integer ) {
    // TTimo: only open the qconsole.log if the filesystem is in an initialized state
    //   also, avoid recursing in the qconsole.log opening (i.e. if fs_debug is on)
		if ( !logfile && FS_Initialized() && !opening_qconsole) {
			struct tm *newtime;
			time_t aclock;

      opening_qconsole = qtrue;

			time( &aclock );
			newtime = localtime( &aclock );

			logfile = FS_FOpenFileWrite_HomeData( "qconsole.log" );
			
			if(logfile)
			{
				Com_Printf( "logfile opened on %s\n", asctime( newtime ) );
			
				if ( com_logfile->integer > 1 )
				{
					// force it to not buffer so we get valid
					// data even if we are crashing
					FS_ForceFlush(logfile);
				}
			}
			else
			{
				Com_Printf("Opening qconsole.log failed!\n");
				Cvar_SetValue("logfile", 0);
			}

      opening_qconsole = qfalse;
		}
		if ( logfile && FS_Initialized()) {
			// as Sys_Print, with times unless com_timestamps is 0
			static qboolean	lineStart = qtrue;

			Com_PrintLines( msg, &lineStart, 1, Com_LogWrite );
		}
	}
}


/*
================
Com_DPrintf

A Com_Printf that only shows up if the "developer" cvar is set
================
*/
void QDECL Com_DPrintf( const char *fmt, ...) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];
		
	if ( !com_developer || !com_developer->integer ) {
		return;			// don't confuse non-developers with techie stuff...
	}

	va_start (argptr,fmt);	
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);
	
	Com_Printf ("%s", msg);
}

/*
=============
Com_FatalShutdown

Shuts everything down before the program exits on an error, the client
and server saying how ("fatal crashed")
=============
*/
static void Com_FatalShutdown( const char *how ) {
	VM_Forced_Unload_Start();
	CL_Shutdown(va("Client %s: %s", how, com_errorMessage), qtrue, qtrue);
	SV_Shutdown(va("Server %s: %s", how, com_errorMessage));
	VM_Forced_Unload_Done();

	Com_Shutdown ();
}

/*
=============
Com_Error

Both client and server can use this, and it will
do the appropriate thing.
=============
*/
void QDECL Com_Error( int code, const char *fmt, ... ) {
	va_list		argptr;
	static int	lastErrorTime;
	static int	errorCount;
	int			currentTime;
	qboolean	restartClient;

	if(com_errorEntered)
		Sys_Error("recursive error after: %s", com_errorMessage);

	com_errorEntered = qtrue;

	Cvar_Set("com_errorCode", va("%i", code));

	// when we are running automated scripts, make sure we
	// know if anything failed
	if ( com_buildScript && com_buildScript->integer ) {
		code = ERR_FATAL;
	}

	// if we are getting a solid stream of ERR_DROP, do an ERR_FATAL
	currentTime = Sys_Milliseconds();
	if ( currentTime - lastErrorTime < 100 ) {
		if ( ++errorCount > 3 ) {
			code = ERR_FATAL;
		}
	} else {
		errorCount = 0;
	}
	lastErrorTime = currentTime;

	va_start (argptr,fmt);
	Q_vsnprintf (com_errorMessage, sizeof(com_errorMessage),fmt,argptr);
	va_end (argptr);

	if (code != ERR_DISCONNECT && code != ERR_NEED_CD)
		Cvar_Set("com_errorMessage", com_errorMessage);

	restartClient = com_gameClientRestarting && !( com_cl_running && com_cl_running->integer );

	com_gameRestarting = qfalse;
	com_gameClientRestarting = qfalse;

	if (code == ERR_DISCONNECT || code == ERR_SERVERDISCONNECT) {
		VM_Forced_Unload_Start();
		SV_Shutdown( "Server disconnected" );
		if ( restartClient ) {
			CL_Init();
		}
		CL_NextDemoAfterError( code );
		CL_Disconnect( qtrue );
		CL_FlushMemory( );
		VM_Forced_Unload_Done();
		// make sure we can get at our local stuff
		FS_PureServerSetLoadedPaks("", "");
		com_errorEntered = qfalse;
		longjmp (abortframe, -1);
	} else if (code == ERR_DROP) {
		Com_Printf ("********************\nERROR: %s\n********************\n", com_errorMessage);
		VM_Forced_Unload_Start();
		SV_Shutdown (va("Server crashed: %s",  com_errorMessage));
		if ( restartClient ) {
			CL_Init();
		}
		CL_NextDemoAfterError( code );
		CL_Disconnect( qtrue );
		CL_FlushMemory( );
		VM_Forced_Unload_Done();
		FS_PureServerSetLoadedPaks("", "");
		com_errorEntered = qfalse;
		longjmp (abortframe, -1);
	} else if ( code == ERR_NEED_CD ) {
		VM_Forced_Unload_Start();
		SV_Shutdown( "Server didn't have CD" );
		if ( restartClient ) {
			CL_Init();
		}
		if ( com_cl_running && com_cl_running->integer ) {
			CL_Disconnect( qtrue );
			CL_FlushMemory( );
			VM_Forced_Unload_Done();
			CL_CDDialog();
		} else {
			Com_Printf("Server didn't have CD\n" );
			VM_Forced_Unload_Done();
		}

		FS_PureServerSetLoadedPaks("", "");

		com_errorEntered = qfalse;
		longjmp (abortframe, -1);
	}

	Com_FatalShutdown( "fatal crashed" );

	Sys_Error ("%s", com_errorMessage);
}

/*
=============
Com_ErrorExit

A fatal error that isn't a crash, such as missing game data, which the
player or admin can put right: prints it while the log is still open,
shuts down as ERR_FATAL does, then exits with status, without a crash log
=============
*/
void QDECL Com_ErrorExit( int status, const char *fmt, ... ) {
	va_list		argptr;

	if(com_errorEntered)
		Sys_Error("recursive error after: %s", com_errorMessage);

	com_errorEntered = qtrue;

	va_start (argptr,fmt);
	Q_vsnprintf (com_errorMessage, sizeof(com_errorMessage),fmt,argptr);
	va_end (argptr);

	Com_Printf( "%s\n", com_errorMessage );

	Com_FatalShutdown( "exited" );

	Sys_ErrorExit (status, com_errorMessage);
}


/*
=============
Com_Quit_f

Both client and server can use this, and it will
do the appropriate things.
=============
*/
void Com_Quit_f( void ) {
	// don't try to shutdown if we are in a recursive error
	char *p = Cmd_Args( );
	if ( !com_errorEntered ) {
		// what changed since the last frame, such as a "seta x 1; quit"
		Com_WriteConfiguration();

		// Some VMs might execute "quit" command directly,
		// which would trigger an unload of active VM error.
		// Sys_Quit will kill this process anyways, so
		// a corrupt call stack makes no difference
		VM_Forced_Unload_Start();
		SV_Shutdown(p[0] ? p : "Server quit");
		CL_Shutdown(p[0] ? p : "Client quit", qtrue, qtrue);
		VM_Forced_Unload_Done();
		Com_Shutdown ();
		FS_Shutdown(qtrue);
	}
	Sys_Quit ();
}



/*
============================================================================

COMMAND LINE FUNCTIONS

+ characters separate the commandLine string into multiple console
command lines.

All of these are valid:

quake3 +set test blah +map test
quake3 set test blah+map test
quake3 set test blah + map test

============================================================================
*/

#define	MAX_CONSOLE_LINES	32
int		com_numConsoleLines;
char	*com_consoleLines[MAX_CONSOLE_LINES];

/*
==================
Com_ParseCommandLine

Break it up into multiple console lines
==================
*/
void Com_ParseCommandLine( char *commandLine ) {
    int inq = 0;
    com_consoleLines[0] = commandLine;
    com_numConsoleLines = 1;

    while ( *commandLine ) {
        if (*commandLine == '"') {
            inq = !inq;
        }
        // look for a + separating character
        // if commandLine came from a file, we might have real line seperators
        if ( (*commandLine == '+' && !inq) || *commandLine == '\n'  || *commandLine == '\r' ) {
            if ( com_numConsoleLines == MAX_CONSOLE_LINES ) {
                return;
            }
            com_consoleLines[com_numConsoleLines] = commandLine + 1;
            com_numConsoleLines++;
            *commandLine = 0;
        }
        commandLine++;
    }
}


/*
===================
Com_SafeMode

Check for "safe" on the command line, which will
skip loading of q3config.cfg
===================
*/
qboolean Com_SafeMode( void ) {
	int		i;

	for ( i = 0 ; i < com_numConsoleLines ; i++ ) {
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( !Q_stricmp( Cmd_Argv(0), "safe" )
			|| !Q_stricmp( Cmd_Argv(0), "cvar_restart" ) ) {
			com_consoleLines[i][0] = 0;
			return qtrue;
		}
	}
	return qfalse;
}


/*
===============
Com_StartupVariable

Searches for command line parameters that are set commands.
If match is not NULL, only that cvar will be looked for.
That is necessary because cddir and basedir need to be set
before the filesystem is started, but all other sets should
be after execing the config and default.
===============
*/
static void Com_StartupVariables( const char *match, int skipFlags ) {
	int		i;
	char	*s;

	for (i=0 ; i < com_numConsoleLines ; i++) {
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( strcmp( Cmd_Argv(0), "set" ) ) {
			continue;
		}

		s = Cmd_Argv(1);
		
		if( ( !match || !strcmp(s, match) ) && !( Cvar_Flags( s ) & skipFlags ) )
		{
			cvar_t	*var = Cvar_SetFrom(s, Cmd_ArgsFrom(2), CVAR_SOURCE_SESSION, qfalse);

			if ( var ) {
				Cvar_NoteOrigin( var, CMD_ORIGIN_COMMAND_LINE );
			}
		}
	}
}

void Com_StartupVariable( const char *match ) {
	Com_StartupVariables( match, 0 );
}


/*
=================
Com_AddStartupCommands

Adds command line parameters as script statements
Commands are separated by + signs

Returns qtrue if any late commands were added, which
will keep the demoloop from immediately starting
=================
*/
qboolean Com_AddStartupCommands( void ) {
	int		i;
	qboolean	added;

	added = qfalse;
	// quote every token, so args with semicolons can work
	for (i=0 ; i < com_numConsoleLines ; i++) {
		if ( !com_consoleLines[i] || !com_consoleLines[i][0] ) {
			continue;
		}

		// set commands already added with Com_StartupVariable
		if ( !Q_stricmpn( com_consoleLines[i], "set ", 4 ) ) {
			continue;
		}

		added = qtrue;
		// a dedicated server's server.cfg runs every start, as autoexec.cfg
		// does; a client's +exec stands for one the player typed
		Cmd_TokenizeString( com_consoleLines[i] );
		if ( !Com_IsClient() && Cmd_Argc() == 2 &&
			( !Q_stricmp( Cmd_Argv( 0 ), "exec" ) || !Q_stricmp( Cmd_Argv( 0 ), "execq" ) ) ) {
			Cmd_AddCommandLineExec( Cmd_Argv( 1 ) );
		}
		Cbuf_AddTextFrom( CMD_ORIGIN_COMMAND_LINE, com_consoleLines[i] );
	}

	return added;
}


//============================================================================

void Info_Print( const char *s ) {
	char	key[BIG_INFO_KEY];
	char	value[BIG_INFO_VALUE];
	char	*o;
	int		l;

	if (*s == '\\')
		s++;
	while (*s)
	{
		o = key;
		while (*s && *s != '\\')
			*o++ = *s++;

		l = o - key;
		if (l < 20)
		{
			Com_Memset (o, ' ', 20-l);
			key[20] = 0;
		}
		else
			*o = 0;
		Com_Printf ("%s ", key);

		if (!*s)
		{
			Com_Printf ("MISSING VALUE\n");
			return;
		}

		o = value;
		s++;
		while (*s && *s != '\\')
			*o++ = *s++;
		*o = 0;

		if (*s)
			s++;
		Com_Printf ("%s\n", value);
	}
}

/*
============
Com_StringContains
============
*/
char *Com_StringContains(char *str1, char *str2, int casesensitive) {
	int len, i, j;

	len = strlen(str1) - strlen(str2);
	for (i = 0; i <= len; i++, str1++) {
		for (j = 0; str2[j]; j++) {
			if (casesensitive) {
				if (str1[j] != str2[j]) {
					break;
				}
			}
			else {
				if (toupper(str1[j]) != toupper(str2[j])) {
					break;
				}
			}
		}
		if (!str2[j]) {
			return str1;
		}
	}
	return NULL;
}

/*
============
Com_Filter
============
*/
int Com_Filter(char *filter, char *name, int casesensitive)
{
	char buf[MAX_TOKEN_CHARS];
	char *ptr;
	int i, found;

	while(*filter) {
		if (*filter == '*') {
			filter++;
			for (i = 0; *filter; i++) {
				if (*filter == '*' || *filter == '?' || *filter == '[') break;
				buf[i] = *filter;
				filter++;
			}
			buf[i] = '\0';
			if (strlen(buf)) {
				ptr = Com_StringContains(name, buf, casesensitive);
				if (!ptr) return qfalse;
				name = ptr + strlen(buf);
			}
		}
		else if (*filter == '?') {
			filter++;
			name++;
		}
		else if (*filter == '[' && *(filter+1) == '[') {
			filter++;
		}
		else if (*filter == '[') {
			filter++;
			found = qfalse;
			while(*filter && !found) {
				if (*filter == ']' && *(filter+1) != ']') break;
				if (*(filter+1) == '-' && *(filter+2) && (*(filter+2) != ']' || *(filter+3) == ']')) {
					if (casesensitive) {
						if (*name >= *filter && *name <= *(filter+2)) found = qtrue;
					}
					else {
						if (toupper(*name) >= toupper(*filter) &&
							toupper(*name) <= toupper(*(filter+2))) found = qtrue;
					}
					filter += 3;
				}
				else {
					if (casesensitive) {
						if (*filter == *name) found = qtrue;
					}
					else {
						if (toupper(*filter) == toupper(*name)) found = qtrue;
					}
					filter++;
				}
			}
			if (!found) return qfalse;
			while(*filter) {
				if (*filter == ']' && *(filter+1) != ']') break;
				filter++;
			}
			filter++;
			name++;
		}
		else {
			if (casesensitive) {
				if (*filter != *name) return qfalse;
			}
			else {
				if (toupper(*filter) != toupper(*name)) return qfalse;
			}
			filter++;
			name++;
		}
	}
	return qtrue;
}

/*
============
Com_FilterPath
============
*/
int Com_FilterPath(char *filter, char *name, int casesensitive)
{
	int i;
	char new_filter[MAX_QPATH];
	char new_name[MAX_QPATH];

	for (i = 0; i < MAX_QPATH-1 && filter[i]; i++) {
		if ( filter[i] == '\\' || filter[i] == ':' ) {
			new_filter[i] = '/';
		}
		else {
			new_filter[i] = filter[i];
		}
	}
	new_filter[i] = '\0';
	for (i = 0; i < MAX_QPATH-1 && name[i]; i++) {
		if ( name[i] == '\\' || name[i] == ':' ) {
			new_name[i] = '/';
		}
		else {
			new_name[i] = name[i];
		}
	}
	new_name[i] = '\0';
	return Com_Filter(new_filter, new_name, casesensitive);
}

/*
================
Com_RealTime
================
*/
int Com_RealTime(qtime_t *qtime) {
	time_t t;
	struct tm *tms;

	t = time(NULL);
	if (!qtime)
		return t;
	tms = localtime(&t);
	if (tms) {
		qtime->tm_sec = tms->tm_sec;
		qtime->tm_min = tms->tm_min;
		qtime->tm_hour = tms->tm_hour;
		qtime->tm_mday = tms->tm_mday;
		qtime->tm_mon = tms->tm_mon;
		qtime->tm_year = tms->tm_year;
		qtime->tm_wday = tms->tm_wday;
		qtime->tm_yday = tms->tm_yday;
		qtime->tm_isdst = tms->tm_isdst;
	}
	return t;
}


/*
==============================================================================

						ZONE MEMORY ALLOCATION

There is never any space between memblocks, and there will never be two
contiguous free memblocks.

The rover can be left pointing at a non-empty block

The zone calls are pretty much only used for small strings and structures,
all big things are allocated on the hunk.
==============================================================================
*/

#define	ZONEID	0x1d4a11
#define MINFRAGMENT	64

// Under AddressSanitizer, the zone and the hunk mark what they haven't
// handed out, and a redzone after each block, as unaddressable, so a read
// or write past a block or into freed memory is reported, as it would be
// for malloc's blocks. Elsewhere these do nothing.
#if defined( __SANITIZE_ADDRESS__ )
#define MEM_ASAN
#elif defined( __has_feature )
#if __has_feature( address_sanitizer )
#define MEM_ASAN
#endif
#endif

#ifdef MEM_ASAN
#include <sanitizer/asan_interface.h>
#define MEM_POISON( p, n )		ASAN_POISON_MEMORY_REGION( ( p ), ( n ) )
#define MEM_UNPOISON( p, n )	ASAN_UNPOISON_MEMORY_REGION( ( p ), ( n ) )
#define ZONE_REDZONE	16
#define HUNK_REDZONE	32
#else
#define MEM_POISON( p, n )		( (void)( p ), (void)( n ) )
#define MEM_UNPOISON( p, n )	( (void)( p ), (void)( n ) )
#define ZONE_REDZONE	0
#define HUNK_REDZONE	0
#endif

typedef struct zonedebug_s {
	char *label;
	char *file;
	int line;
	int allocSize;
} zonedebug_t;

typedef struct memblock_s {
	int		size;           // including the header and possibly tiny fragments
	int     tag;            // a tag of 0 is a free block
	struct memblock_s       *next, *prev;
	int     id;        		// should be ZONEID
#ifdef ZONE_DEBUG
	zonedebug_t d;
#endif
} memblock_t;

typedef struct {
	int		size;			// total bytes malloced, including header
	int		used;			// total bytes used
	memblock_t	blocklist;	// start / end cap for linked list
	memblock_t	*rover;
} memzone_t;

// main zone for all "dynamic" memory allocation
static memzone_t	*mainzone;
// we also have a small zone for small allocations that would only
// fragment the main zone (think of cvar and cmd strings)
static memzone_t	*smallzone;

static void Z_CheckHeap( void );

/*
========================
Z_ClearZone
========================
*/
static void Z_ClearZone( memzone_t *zone, int size ) {
	memblock_t	*block;
	
	// set the entire zone to one free block

	zone->blocklist.next = zone->blocklist.prev = block =
		(memblock_t *)( (byte *)zone + sizeof(memzone_t) );
	zone->blocklist.tag = 1;	// in use block
	zone->blocklist.id = 0;
	zone->blocklist.size = 0;
	zone->rover = block;
	zone->size = size;
	zone->used = 0;
	
	block->prev = block->next = &zone->blocklist;
	block->tag = 0;			// free block
	block->id = ZONEID;
	block->size = size - sizeof(memzone_t);
	MEM_POISON( block + 1, block->size - sizeof( memblock_t ) );
}

/*
========================
Z_AvailableZoneMemory
========================
*/
static int Z_AvailableZoneMemory( memzone_t *zone ) {
	return zone->size - zone->used;
}

/*
========================
Z_AvailableMemory
========================
*/
int Z_AvailableMemory( void ) {
	return Z_AvailableZoneMemory( mainzone );
}

/*
========================
Z_Free
========================
*/
void Z_Free( void *ptr ) {
	memblock_t	*block, *other;
	memzone_t *zone;
	
	if (!ptr) {
		Com_Error( ERR_DROP, "Z_Free: NULL pointer" );
	}

	block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));
	if (block->id != ZONEID) {
		Com_Error( ERR_FATAL, "Z_Free: freed a pointer without ZONEID" );
	}
	if (block->tag == 0) {
		Com_Error( ERR_FATAL, "Z_Free: freed a freed pointer" );
	}
	// if static memory
	if (block->tag == TAG_STATIC) {
		return;
	}

	// check the memory trash tester
	if ( *(int *)((byte *)block + block->size - 4 ) != ZONEID ) {
		Com_Error( ERR_FATAL, "Z_Free: memory block wrote past end" );
	}

	if (block->tag == TAG_SMALL) {
		zone = smallzone;
	}
	else {
		zone = mainzone;
	}

	zone->used -= block->size;
	// set the block to something that should cause problems
	// if it is referenced...
	MEM_UNPOISON( ptr, block->size - sizeof( *block ) );
	Com_Memset( ptr, 0xaa, block->size - sizeof( *block ) );
	MEM_POISON( ptr, block->size - sizeof( *block ) );

	block->tag = 0;		// mark as free
	
	other = block->prev;
	if (!other->tag) {
		// merge with previous free block
		other->size += block->size;
		other->next = block->next;
		other->next->prev = other;
		if (block == zone->rover) {
			zone->rover = other;
		}
		// its header is part of the free space now
		MEM_POISON( block, sizeof( *block ) );
		block = other;
	}

	zone->rover = block;

	other = block->next;
	if ( !other->tag ) {
		// merge the next free block onto the end
		block->size += other->size;
		block->next = other->next;
		block->next->prev = block;
		MEM_POISON( other, sizeof( *other ) );
	}
}


/*
================
Z_FreeTags
================
*/
void Z_FreeTags( int tag ) {
	memzone_t	*zone;

	if ( tag == TAG_SMALL ) {
		zone = smallzone;
	}
	else {
		zone = mainzone;
	}
	// use the rover as our pointer, because
	// Z_Free automatically adjusts it
	zone->rover = zone->blocklist.next;
	do {
		if ( zone->rover->tag == tag ) {
			Z_Free( (void *)(zone->rover + 1) );
			continue;
		}
		zone->rover = zone->rover->next;
	} while ( zone->rover != &zone->blocklist );
}


/*
================
Z_TagMalloc
================
*/
#ifdef ZONE_DEBUG
void *Z_TagMallocDebug( int size, int tag, char *label, char *file, int line ) {
	int		allocSize;
#else
void *Z_TagMalloc( int size, int tag ) {
#endif
	int		extra, request;
	memblock_t	*start, *rover, *new, *base;
	memzone_t *zone;

	if (!tag) {
		Com_Error( ERR_FATAL, "Z_TagMalloc: tried to use a 0 tag" );
	}

	if ( tag == TAG_SMALL ) {
		zone = smallzone;
	}
	else {
		zone = mainzone;
	}

	// a negative size, or one so large the header's room would wrap it
	if ( size < 0 || size > zone->size ) {
		Com_Error( ERR_DROP, "Z_Malloc: bad size %i", size );
	}

#ifdef ZONE_DEBUG
	allocSize = size;
#endif
	//
	// scan through the block list looking for the first free block
	// of sufficient size
	//
	request = size;
	size += sizeof(memblock_t);	// account for size of block header
	size += 4;					// space for memory trash tester
	size += ZONE_REDZONE;		// before the trash tester
	size = PAD(size, sizeof(intptr_t));		// align to 32/64 bit boundary
	
	base = rover = zone->rover;
	start = base->prev;
	
	do {
		if (rover == start)	{
			// scaned all the way around the list
#ifdef ZONE_DEBUG
			Z_LogHeap();

			Com_Error(ERR_FATAL, "Z_Malloc: failed on allocation of %i bytes from the %s zone: %s, line: %d (%s)",
								size, zone == smallzone ? "small" : "main", file, line, label);
#else
			Com_Error(ERR_FATAL, "Z_Malloc: failed on allocation of %i bytes from the %s zone",
								size, zone == smallzone ? "small" : "main");
#endif
			return NULL;
		}
		if (rover->tag) {
			base = rover = rover->next;
		} else {
			rover = rover->next;
		}
	} while (base->tag || base->size < size);
	
	//
	// found a block big enough
	//
	extra = base->size - size;
	if (extra > MINFRAGMENT) {
		// there will be a free fragment after the allocated block
		new = (memblock_t *) ((byte *)base + size );
		MEM_UNPOISON( new, sizeof( *new ) );
		new->size = extra;
		new->tag = 0;			// free block
		new->prev = base;
		new->id = ZONEID;
		new->next = base->next;
		new->next->prev = new;
		base->next = new;
		base->size = size;
	}
	
	base->tag = tag;			// no longer a free block
	
	zone->rover = base->next;	// next allocation will start looking here
	zone->used += base->size;	//
	
	base->id = ZONEID;

#ifdef ZONE_DEBUG
	base->d.label = label;
	base->d.file = file;
	base->d.line = line;
	base->d.allocSize = allocSize;
#endif

	// only what was asked for is addressable, and the trash tester's last
	// 8 bytes (a poisoned region can't end part way into them)
	MEM_POISON( base + 1, base->size - sizeof( memblock_t ) );
	MEM_UNPOISON( base + 1, request );
	MEM_UNPOISON( (byte *)base + base->size - 8, 8 );

	// marker for memory trash testing
	*(int *)((byte *)base + base->size - 4) = ZONEID;

	return (void *) ((byte *)base + sizeof(memblock_t));
}

/*
========================
Z_Malloc
========================
*/
#ifdef ZONE_DEBUG
void *Z_MallocDebug( int size, char *label, char *file, int line ) {
#else
void *Z_Malloc( int size ) {
#endif
	void	*buf;
	
  //Z_CheckHeap ();	// DEBUG

#ifdef ZONE_DEBUG
	buf = Z_TagMallocDebug( size, TAG_GENERAL, label, file, line );
#else
	buf = Z_TagMalloc( size, TAG_GENERAL );
#endif
	Com_Memset( buf, 0, size );

	return buf;
}

#ifdef ZONE_DEBUG
void *S_MallocDebug( int size, char *label, char *file, int line ) {
	return Z_TagMallocDebug( size, TAG_SMALL, label, file, line );
}
#else
void *S_Malloc( int size ) {
	return Z_TagMalloc( size, TAG_SMALL );
}
#endif

/*
========================
Z_CheckHeap
========================
*/
static void Z_CheckHeap( void ) {
	memblock_t	*block;
	
	for (block = mainzone->blocklist.next ; ; block = block->next) {
		if (block->next == &mainzone->blocklist) {
			break;			// all blocks have been hit
		}
		if ( (byte *)block + block->size != (byte *)block->next)
			Com_Error( ERR_FATAL, "Z_CheckHeap: block size does not touch the next block" );
		if ( block->next->prev != block) {
			Com_Error( ERR_FATAL, "Z_CheckHeap: next block doesn't have proper back link" );
		}
		if ( !block->tag && !block->next->tag ) {
			Com_Error( ERR_FATAL, "Z_CheckHeap: two consecutive free blocks" );
		}
	}
}

/*
========================
Z_LogZoneHeap
========================
*/
void Z_LogZoneHeap( memzone_t *zone, char *name ) {
#ifdef ZONE_DEBUG
	char dump[32], *ptr;
	int  i, j;
#endif
	memblock_t	*block;
	char		buf[4096];
	int size, allocSize, numBlocks;

	if (!logfile || !FS_Initialized())
		return;
	size = numBlocks = 0;
#ifdef ZONE_DEBUG
	allocSize = 0;
#endif
	Com_sprintf(buf, sizeof(buf), "\r\n================\r\n%s log\r\n================\r\n", name);
	FS_Write(buf, strlen(buf), logfile);
	for (block = zone->blocklist.next ; block->next != &zone->blocklist; block = block->next) {
		if (block->tag) {
#ifdef ZONE_DEBUG
			ptr = ((char *) block) + sizeof(memblock_t);
			j = 0;
			for (i = 0; i < 20 && i < block->d.allocSize; i++) {
				if (ptr[i] >= 32 && ptr[i] < 127) {
					dump[j++] = ptr[i];
				}
				else {
					dump[j++] = '_';
				}
			}
			dump[j] = '\0';
			Com_sprintf(buf, sizeof(buf), "size = %8d: %s, line: %d (%s) [%s]\r\n", block->d.allocSize, block->d.file, block->d.line, block->d.label, dump);
			FS_Write(buf, strlen(buf), logfile);
			allocSize += block->d.allocSize;
#endif
			size += block->size;
			numBlocks++;
		}
	}
#ifdef ZONE_DEBUG
	// subtract debug memory
	size -= numBlocks * sizeof(zonedebug_t);
#else
	allocSize = numBlocks * sizeof(memblock_t); // + 32 bit alignment
#endif
	Com_sprintf(buf, sizeof(buf), "%d %s memory in %d blocks\r\n", size, name, numBlocks);
	FS_Write(buf, strlen(buf), logfile);
	Com_sprintf(buf, sizeof(buf), "%d %s memory overhead\r\n", size - allocSize, name);
	FS_Write(buf, strlen(buf), logfile);
}

/*
========================
Z_LogHeap
========================
*/
void Z_LogHeap( void ) {
	Z_LogZoneHeap( mainzone, "MAIN" );
	Z_LogZoneHeap( smallzone, "SMALL" );
}

// static mem blocks to reduce a lot of small zone overhead
typedef struct memstatic_s {
	memblock_t b;
	byte mem[2];
} memstatic_t;

memstatic_t emptystring =
	{ {(sizeof(memblock_t)+2 + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'\0', '\0'} };
memstatic_t numberstring[] = {
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'0', '\0'} },
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'1', '\0'} },
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'2', '\0'} },
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'3', '\0'} },
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'4', '\0'} },
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'5', '\0'} },
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'6', '\0'} },
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'7', '\0'} },
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'8', '\0'} }, 
	{ {(sizeof(memstatic_t) + 3) & ~3, TAG_STATIC, NULL, NULL, ZONEID}, {'9', '\0'} }
};

/*
========================
CopyString

 NOTE:	never write over the memory CopyString returns because
		memory from a memstatic_t might be returned
========================
*/
char *CopyString( const char *in ) {
	char	*out;

	if (!in[0]) {
		return ((char *)&emptystring) + sizeof(memblock_t);
	}
	else if (!in[1]) {
		if (in[0] >= '0' && in[0] <= '9') {
			return ((char *)&numberstring[in[0]-'0']) + sizeof(memblock_t);
		}
	}
	out = S_Malloc (strlen(in)+1);
	strcpy (out, in);
	return out;
}

/*
==============================================================================

Goals:
	reproducible without history effects -- no out of memory errors on weird map to map changes
	allow restarting of the client without fragmentation
	minimize total pages in use at run time
	minimize total pages needed during load time

  Single block of memory with stack allocators coming from both ends towards the middle.

  One side is designated the temporary memory allocator.

  Temporary memory can be allocated and freed in any order.

  A highwater mark is kept of the most in use at any time.

  When there is no temporary memory allocated, the permanent and temp sides
  can be switched, allowing the already touched temp memory to be used for
  permanent storage.

  Temp memory must never be allocated on two ends at once, or fragmentation
  could occur.

  If we have any in-use temp memory, additional temp allocations must come from
  that side.

  If not, we can choose to make either side the new temp side and push future
  permanent allocations to the other side.  Permanent allocations should be
  kept on the side that has the current greatest wasted highwater mark.

==============================================================================
*/


#define	HUNK_MAGIC	0x89537892
#define	HUNK_FREE_MAGIC	0x89537893

typedef struct {
	int		magic;
	int		size;
} hunkHeader_t;

typedef struct {
	int		mark;
	int		permanent;
	int		temp;
	int		tempHighwater;
} hunkUsed_t;

typedef struct hunkblock_s {
	int size;
	byte printed;
	struct hunkblock_s *next;
	char *label;
	char *file;
	int line;
} hunkblock_t;

static	hunkblock_t *hunkblocks;

static	hunkUsed_t	hunk_low, hunk_high;
static	hunkUsed_t	*hunk_permanent, *hunk_temp;

static	byte	*s_hunkData = NULL;
static	int		s_hunkTotal;

static	int		s_zoneTotal;
static	int		s_smallZoneTotal;


/*
=================
Com_Meminfo_f
=================
*/
void Com_Meminfo_f( void ) {
	memblock_t	*block;
	int			zoneBytes, zoneBlocks;
	int			smallZoneBytes;
	int			botlibBytes, rendererBytes;
	int			unused;

	zoneBytes = 0;
	botlibBytes = 0;
	rendererBytes = 0;
	zoneBlocks = 0;
	for (block = mainzone->blocklist.next ; ; block = block->next) {
		if ( Cmd_Argc() != 1 ) {
			Com_Printf ("block:%p    size:%7i    tag:%3i\n",
				(void *)block, block->size, block->tag);
		}
		if ( block->tag ) {
			zoneBytes += block->size;
			zoneBlocks++;
			if ( block->tag == TAG_BOTLIB ) {
				botlibBytes += block->size;
			} else if ( block->tag == TAG_RENDERER ) {
				rendererBytes += block->size;
			}
		}

		if (block->next == &mainzone->blocklist) {
			break;			// all blocks have been hit	
		}
		if ( (byte *)block + block->size != (byte *)block->next) {
			Com_Printf ("ERROR: block size does not touch the next block\n");
		}
		if ( block->next->prev != block) {
			Com_Printf ("ERROR: next block doesn't have proper back link\n");
		}
		if ( !block->tag && !block->next->tag ) {
			Com_Printf ("ERROR: two consecutive free blocks\n");
		}
	}

	smallZoneBytes = 0;
	for (block = smallzone->blocklist.next ; ; block = block->next) {
		if ( block->tag ) {
			smallZoneBytes += block->size;
		}

		if (block->next == &smallzone->blocklist) {
			break;			// all blocks have been hit	
		}
	}

	Com_Printf( "%8i bytes total hunk\n", s_hunkTotal );
	Com_Printf( "%8i bytes total zone\n", s_zoneTotal );
	Com_Printf( "\n" );
	Com_Printf( "%8i low mark\n", hunk_low.mark );
	Com_Printf( "%8i low permanent\n", hunk_low.permanent );
	if ( hunk_low.temp != hunk_low.permanent ) {
		Com_Printf( "%8i low temp\n", hunk_low.temp );
	}
	Com_Printf( "%8i low tempHighwater\n", hunk_low.tempHighwater );
	Com_Printf( "\n" );
	Com_Printf( "%8i high mark\n", hunk_high.mark );
	Com_Printf( "%8i high permanent\n", hunk_high.permanent );
	if ( hunk_high.temp != hunk_high.permanent ) {
		Com_Printf( "%8i high temp\n", hunk_high.temp );
	}
	Com_Printf( "%8i high tempHighwater\n", hunk_high.tempHighwater );
	Com_Printf( "\n" );
	Com_Printf( "%8i total hunk in use\n", hunk_low.permanent + hunk_high.permanent );
	unused = 0;
	if ( hunk_low.tempHighwater > hunk_low.permanent ) {
		unused += hunk_low.tempHighwater - hunk_low.permanent;
	}
	if ( hunk_high.tempHighwater > hunk_high.permanent ) {
		unused += hunk_high.tempHighwater - hunk_high.permanent;
	}
	Com_Printf( "%8i unused highwater\n", unused );
	Com_Printf( "\n" );
	Com_Printf( "%8i bytes in %i zone blocks\n", zoneBytes, zoneBlocks	);
	Com_Printf( "        %8i bytes in dynamic botlib\n", botlibBytes );
	Com_Printf( "        %8i bytes in dynamic renderer\n", rendererBytes );
	Com_Printf( "        %8i bytes in dynamic other\n", zoneBytes - ( botlibBytes + rendererBytes ) );
	Com_Printf( "        %8i bytes in small Zone memory\n", smallZoneBytes );
}

/*
===============
Com_TouchMemory

Touch all known used data to make sure it is paged in
===============
*/
void Com_TouchMemory( void ) {
	int		start, end;
	int		i, j;
	unsigned	sum;
	memblock_t	*block;

	Z_CheckHeap();

#ifdef MEM_ASAN
	// it reads every page of the hunk and the zone, redzones included
	return;
#endif

	start = Sys_Milliseconds();

	sum = 0;

	j = hunk_low.permanent >> 2;
	for ( i = 0 ; i < j ; i+=64 ) {			// only need to touch each page
		sum += ((int *)s_hunkData)[i];
	}

	i = ( s_hunkTotal - hunk_high.permanent ) >> 2;
	j = hunk_high.permanent >> 2;
	for (  ; i < j ; i+=64 ) {			// only need to touch each page
		sum += ((int *)s_hunkData)[i];
	}

	for (block = mainzone->blocklist.next ; ; block = block->next) {
		if ( block->tag ) {
			j = block->size >> 2;
			for ( i = 0 ; i < j ; i+=64 ) {				// only need to touch each page
				sum += ((int *)block)[i];
			}
		}
		if ( block->next == &mainzone->blocklist ) {
			break;			// all blocks have been hit	
		}
	}

	end = Sys_Milliseconds();

	(void)sum; // Suppress warning

	Com_Printf( "Com_TouchMemory: %i msec\n", end - start );
}



/*
=================
Com_InitZoneMemory
=================
*/
void Com_InitSmallZoneMemory( void ) {
	s_smallZoneTotal = 512 * 1024;
	smallzone = calloc( s_smallZoneTotal, 1 );
	if ( !smallzone ) {
		Com_Error( ERR_FATAL, "Small zone data failed to allocate %1.1f megs", (float)s_smallZoneTotal / (1024*1024) );
	}
	Z_ClearZone( smallzone, s_smallZoneTotal );
}

void Com_InitZoneMemory( void ) {
	cvar_t	*cv;

	// Please note: com_zoneMegs can only be set on the command line, and
	// not in q3config.cfg or Com_StartupVariable, as they haven't been
	// executed by this point. It's a chicken and egg problem. We need the
	// memory manager configured to handle those places where you would
	// configure the memory manager.

	// allocate the random block zone
	cv = Cvar_Get( "com_zoneMegs", DEF_COMZONEMEGS_S, CVAR_LATCH | CVAR_ARCHIVE );

	if ( cv->integer < DEF_COMZONEMEGS ) {
		s_zoneTotal = 1024 * 1024 * DEF_COMZONEMEGS;
	} else {
		s_zoneTotal = cv->integer * 1024 * 1024;
	}

	mainzone = calloc( s_zoneTotal, 1 );
	if ( !mainzone ) {
		Com_Error( ERR_FATAL, "Zone data failed to allocate %i megs", s_zoneTotal / (1024*1024) );
	}
	Z_ClearZone( mainzone, s_zoneTotal );

}

/*
=================
Hunk_Log
=================
*/
void Hunk_Log( void) {
	hunkblock_t	*block;
	char		buf[4096];
	int size, numBlocks;

	if (!logfile || !FS_Initialized())
		return;
	size = 0;
	numBlocks = 0;
	Com_sprintf(buf, sizeof(buf), "\r\n================\r\nHunk log\r\n================\r\n");
	FS_Write(buf, strlen(buf), logfile);
	for (block = hunkblocks ; block; block = block->next) {
#ifdef HUNK_DEBUG
		Com_sprintf(buf, sizeof(buf), "size = %8d: %s, line: %d (%s)\r\n", block->size, block->file, block->line, block->label);
		FS_Write(buf, strlen(buf), logfile);
#endif
		size += block->size;
		numBlocks++;
	}
	Com_sprintf(buf, sizeof(buf), "%d Hunk memory\r\n", size);
	FS_Write(buf, strlen(buf), logfile);
	Com_sprintf(buf, sizeof(buf), "%d hunk blocks\r\n", numBlocks);
	FS_Write(buf, strlen(buf), logfile);
	FS_Flush(logfile);
}

/*
=================
Hunk_SmallLog
=================
*/
void Hunk_SmallLog( void) {
	hunkblock_t	*block, *block2;
	char		buf[4096];
	int size, locsize, numBlocks;

	if (!logfile || !FS_Initialized())
		return;
	for (block = hunkblocks ; block; block = block->next) {
		block->printed = qfalse;
	}
	size = 0;
	numBlocks = 0;
	Com_sprintf(buf, sizeof(buf), "\r\n================\r\nHunk Small log\r\n================\r\n");
	FS_Write(buf, strlen(buf), logfile);
	for (block = hunkblocks; block; block = block->next) {
		if (block->printed) {
			continue;
		}
		locsize = block->size;
		for (block2 = block->next; block2; block2 = block2->next) {
			if (block->line != block2->line) {
				continue;
			}
			if (Q_stricmp(block->file, block2->file)) {
				continue;
			}
			size += block2->size;
			locsize += block2->size;
			block2->printed = qtrue;
		}
#ifdef HUNK_DEBUG
		Com_sprintf(buf, sizeof(buf), "size = %8d: %s, line: %d (%s)\r\n", locsize, block->file, block->line, block->label);
		FS_Write(buf, strlen(buf), logfile);
#else
		(void)locsize; // Suppress warning
#endif
		size += block->size;
		numBlocks++;
	}
	Com_sprintf(buf, sizeof(buf), "%d Hunk memory\r\n", size);
	FS_Write(buf, strlen(buf), logfile);
	Com_sprintf(buf, sizeof(buf), "%d hunk blocks\r\n", numBlocks);
	FS_Write(buf, strlen(buf), logfile);
	FS_Flush(logfile);
}

/*
=================
Com_InitHunkZoneMemory
=================
*/
void Com_InitHunkMemory( void ) {
	cvar_t	*cv;
	int nMinAlloc;
	char *pMsg = NULL;

	// make sure the file system has allocated and "not" freed any temp blocks
	// this allows the config and product id files ( journal files too ) to be loaded
	// by the file system without redunant routines in the file system utilizing different 
	// memory systems
	if (FS_LoadStack() != 0) {
		Com_Error( ERR_FATAL, "Hunk initialization failed. File system load stack not zero");
	}

	// allocate the stack based hunk allocator
	cv = Cvar_Get( "com_hunkMegs", DEF_COMHUNKMEGS_S, CVAR_LATCH | CVAR_ARCHIVE );
	Cvar_SetDescription(cv, "The size of the hunk memory segment");

	// if we are not dedicated min allocation is 56, otherwise min is 1
	if (com_dedicated && com_dedicated->integer) {
		nMinAlloc = MIN_DEDICATED_COMHUNKMEGS;
		pMsg = "Minimum com_hunkMegs for a dedicated server is %i, allocating %i megs.\n";
	}
	else {
		nMinAlloc = MIN_COMHUNKMEGS;
		pMsg = "Minimum com_hunkMegs is %i, allocating %i megs.\n";
	}

	if ( cv->integer < nMinAlloc ) {
		s_hunkTotal = 1024 * 1024 * nMinAlloc;
	    Com_Printf(pMsg, nMinAlloc, s_hunkTotal / (1024 * 1024));
	} else {
		s_hunkTotal = cv->integer * 1024 * 1024;
	}

	s_hunkData = calloc( s_hunkTotal + 31, 1 );
	if ( !s_hunkData ) {
		Com_Error( ERR_FATAL, "Hunk data failed to allocate %i megs", s_hunkTotal / (1024*1024) );
	}
	// cacheline align
	s_hunkData = (byte *) ( ( (intptr_t)s_hunkData + 31 ) & ~31 );
	Hunk_Clear();

	Cmd_AddCommand( "meminfo", Com_Meminfo_f );
#ifdef ZONE_DEBUG
	Cmd_AddCommand( "zonelog", Z_LogHeap );
#endif
#ifdef HUNK_DEBUG
	Cmd_AddCommand( "hunklog", Hunk_Log );
	Cmd_AddCommand( "hunksmalllog", Hunk_SmallLog );
#endif
}

/*
====================
Hunk_MemoryRemaining
====================
*/
int	Hunk_MemoryRemaining( void ) {
	int		low, high;

	low = hunk_low.permanent > hunk_low.temp ? hunk_low.permanent : hunk_low.temp;
	high = hunk_high.permanent > hunk_high.temp ? hunk_high.permanent : hunk_high.temp;

	return s_hunkTotal - ( low + high );
}

/*
===================
Hunk_SetMark

The server calls this after the level and game VM have been loaded
===================
*/
void Hunk_SetMark( void ) {
	hunk_low.mark = hunk_low.permanent;
	hunk_high.mark = hunk_high.permanent;
}

/*
=================
Hunk_ClearToMark

The client calls this before starting a vid_restart or snd_restart
=================
*/
void Hunk_ClearToMark( void ) {
	if ( s_hunkData != NULL ) {
		// what's released is unaddressable until it's handed out again
		int		low = hunk_low.permanent > hunk_low.temp ? hunk_low.permanent : hunk_low.temp;
		int		high = hunk_high.permanent > hunk_high.temp ? hunk_high.permanent : hunk_high.temp;

		MEM_POISON( s_hunkData + hunk_low.mark, low - hunk_low.mark );
		MEM_POISON( s_hunkData + s_hunkTotal - high, high - hunk_high.mark );
	}
	hunk_low.permanent = hunk_low.temp = hunk_low.mark;
	hunk_high.permanent = hunk_high.temp = hunk_high.mark;
}

/*
=================
Hunk_CheckMark
=================
*/
qboolean Hunk_CheckMark( void ) {
	if( hunk_low.mark || hunk_high.mark ) {
		return qtrue;
	}
	return qfalse;
}

void CL_ShutdownCGame( void );
void CL_ShutdownUI( void );
void SV_ShutdownGameProgs( void );

/*
=================
Hunk_Clear

The server calls this before shutting down or loading a new map
=================
*/
void Hunk_Clear( void ) {

#ifndef DEDICATED
	CL_ShutdownCGame();
	CL_ShutdownUI();
#endif
	SV_ShutdownGameProgs();
#ifndef DEDICATED
	CIN_CloseAllVideos();
#endif
	hunk_low.mark = 0;
	hunk_low.permanent = 0;
	hunk_low.temp = 0;
	hunk_low.tempHighwater = 0;

	hunk_high.mark = 0;
	hunk_high.permanent = 0;
	hunk_high.temp = 0;
	hunk_high.tempHighwater = 0;

	hunk_permanent = &hunk_low;
	hunk_temp = &hunk_high;

	if ( s_hunkData != NULL ) {
		MEM_POISON( s_hunkData, s_hunkTotal );
	}

	Com_Printf( "Hunk_Clear: reset the hunk ok\n" );
	VM_Clear();
#ifdef HUNK_DEBUG
	hunkblocks = NULL;
#endif
}

static void Hunk_SwapBanks( void ) {
	hunkUsed_t	*swap;

	// can't swap banks if there is any temp already allocated
	if ( hunk_temp->temp != hunk_temp->permanent ) {
		return;
	}

	// if we have a larger highwater mark on this side, start making
	// our permanent allocations here and use the other side for temp
	if ( hunk_temp->tempHighwater - hunk_temp->permanent >
		hunk_permanent->tempHighwater - hunk_permanent->permanent ) {
		swap = hunk_temp;
		hunk_temp = hunk_permanent;
		hunk_permanent = swap;
	}
}

/*
=================
Hunk_Alloc

Allocate permanent (until the hunk is cleared) memory
=================
*/
#ifdef HUNK_DEBUG
void *Hunk_AllocDebug( int size, ha_pref preference, char *label, char *file, int line ) {
#else
void *Hunk_Alloc( int size, ha_pref preference ) {
#endif
	void	*buf;
	int		request;

	if ( s_hunkData == NULL)
	{
		Com_Error( ERR_FATAL, "Hunk_Alloc: Hunk memory system not initialized" );
	}

	// a negative size, or one so large rounding it up would wrap it
	if ( size < 0 || size > s_hunkTotal ) {
		Com_Error( ERR_DROP, "Hunk_Alloc: bad size %i", size );
	}

	// can't do preference if there is any temp allocated
	if (preference == h_dontcare || hunk_temp->temp != hunk_temp->permanent) {
		Hunk_SwapBanks();
	} else {
		if (preference == h_low && hunk_permanent != &hunk_low) {
			Hunk_SwapBanks();
		} else if (preference == h_high && hunk_permanent != &hunk_high) {
			Hunk_SwapBanks();
		}
	}

#ifdef HUNK_DEBUG
	size += sizeof(hunkblock_t);
#endif
	request = size;
	size += HUNK_REDZONE;

	// round to cacheline
	size = (size+31)&~31;

	// against what's left: adding the size to what's in use can overflow
	// on a hunk of 1 GB or more
	if ( size > s_hunkTotal - hunk_low.temp - hunk_high.temp ) {
#ifdef HUNK_DEBUG
		Hunk_Log();
		Hunk_SmallLog();

		Com_Error(ERR_DROP, "Hunk_Alloc failed on %i: %s, line: %d (%s)", size, file, line, label);
#else
		Com_Error(ERR_DROP, "Hunk_Alloc failed on %i", size);
#endif
	}

	if ( hunk_permanent == &hunk_low ) {
		buf = (void *)(s_hunkData + hunk_permanent->permanent);
		hunk_permanent->permanent += size;
	} else {
		hunk_permanent->permanent += size;
		buf = (void *)(s_hunkData + s_hunkTotal - hunk_permanent->permanent );
	}

	hunk_permanent->temp = hunk_permanent->permanent;

	// the rest, the rounding and the redzone, stays unaddressable
	MEM_UNPOISON( buf, size );
	Com_Memset( buf, 0, size );
	MEM_POISON( (byte *)buf + request, size - request );

#ifdef HUNK_DEBUG
	{
		hunkblock_t *block;

		block = (hunkblock_t *) buf;
		block->size = size - sizeof(hunkblock_t);
		block->file = file;
		block->label = label;
		block->line = line;
		block->next = hunkblocks;
		hunkblocks = block;
		buf = ((byte *) buf) + sizeof(hunkblock_t);
	}
#endif
	return buf;
}

/*
=================
Hunk_AllocateTempMemory

This is used by the file loading system.
Multiple files can be loaded in temporary memory.
When the files-in-use count reaches zero, all temp memory will be deleted
=================
*/
void *Hunk_AllocateTempMemory( int size ) {
	void		*buf;
	hunkHeader_t	*hdr;
	int			request;

	// a negative size, or one so large the header's room would wrap it
	if ( size < 0 || ( s_hunkData != NULL && size > s_hunkTotal ) ) {
		Com_Error( ERR_DROP, "Hunk_AllocateTempMemory: bad size %i", size );
	}

	// return a Z_Malloc'd block if the hunk has not been initialized
	// this allows the config and product id files ( journal files too ) to be loaded
	// by the file system without redunant routines in the file system utilizing different 
	// memory systems
	if ( s_hunkData == NULL )
	{
		return Z_Malloc(size);
	}

	Hunk_SwapBanks();

	request = size;
	size = PAD(size + HUNK_REDZONE, sizeof(intptr_t)) + sizeof( hunkHeader_t );

	// against what's left: adding the size to what's in use can overflow
	// on a hunk of 1 GB or more
	if ( size > s_hunkTotal - hunk_temp->temp - hunk_permanent->permanent ) {
		Com_Error( ERR_DROP, "Hunk_AllocateTempMemory: failed on %i", size );
	}

	if ( hunk_temp == &hunk_low ) {
		buf = (void *)(s_hunkData + hunk_temp->temp);
		hunk_temp->temp += size;
	} else {
		hunk_temp->temp += size;
		buf = (void *)(s_hunkData + s_hunkTotal - hunk_temp->temp );
	}

	if ( hunk_temp->temp > hunk_temp->tempHighwater ) {
		hunk_temp->tempHighwater = hunk_temp->temp;
	}

	hdr = (hunkHeader_t *)buf;
	buf = (void *)(hdr+1);
	MEM_UNPOISON( hdr, sizeof( *hdr ) + request );

	hdr->magic = HUNK_MAGIC;
	hdr->size = size;

	// don't bother clearing, because we are going to load a file over it
	return buf;
}


/*
==================
Hunk_FreeTempMemory
==================
*/
void Hunk_FreeTempMemory( void *buf ) {
	hunkHeader_t	*hdr;

	  // free with Z_Free if the hunk has not been initialized
	  // this allows the config and product id files ( journal files too ) to be loaded
	  // by the file system without redunant routines in the file system utilizing different 
	  // memory systems
	if ( s_hunkData == NULL )
	{
		Z_Free(buf);
		return;
	}


	hdr = ( (hunkHeader_t *)buf ) - 1;
	if ( hdr->magic != HUNK_MAGIC ) {
		Com_Error( ERR_FATAL, "Hunk_FreeTempMemory: bad magic" );
	}

	hdr->magic = HUNK_FREE_MAGIC;

	// this only works if the files are freed in stack order,
	// otherwise the memory will stay around until Hunk_ClearTempMemory
	if ( hunk_temp == &hunk_low ) {
		if ( hdr == (void *)(s_hunkData + hunk_temp->temp - hdr->size ) ) {
			hunk_temp->temp -= hdr->size;
		} else {
			Com_Printf( "Hunk_FreeTempMemory: not the final block\n" );
		}
	} else {
		if ( hdr == (void *)(s_hunkData + s_hunkTotal - hunk_temp->temp ) ) {
			hunk_temp->temp -= hdr->size;
		} else {
			Com_Printf( "Hunk_FreeTempMemory: not the final block\n" );
		}
	}
	// freed, even when it stays in place: using it again is an error
	MEM_POISON( hdr, hdr->size );
}


/*
=================
Hunk_ClearTempMemory

The temp space is no longer needed.  If we have left more
touched but unused memory on this side, have future
permanent allocs use this side.
=================
*/
void Hunk_ClearTempMemory( void ) {
	if ( s_hunkData != NULL ) {
		if ( hunk_temp == &hunk_low ) {
			MEM_POISON( s_hunkData + hunk_temp->permanent, hunk_temp->temp - hunk_temp->permanent );
		} else {
			MEM_POISON( s_hunkData + s_hunkTotal - hunk_temp->temp, hunk_temp->temp - hunk_temp->permanent );
		}
		hunk_temp->temp = hunk_temp->permanent;
	}
}

/*
===================================================================

EVENTS AND JOURNALING

In addition to these events, .cfg files are also copied to the
journaled file
===================================================================
*/

#define	MAX_PUSHED_EVENTS	            1024
static int com_pushedEventsHead = 0;
static int com_pushedEventsTail = 0;
static sysEvent_t	com_pushedEvents[MAX_PUSHED_EVENTS];

/*
=================
Com_InitJournaling
=================
*/
void Com_InitJournaling( void ) {
	Com_StartupVariable( "journal" );
	com_journal = Cvar_Get ("journal", "0", CVAR_INIT);
	if ( !com_journal->integer ) {
		return;
	}

	if ( com_journal->integer == 1 ) {
		Com_Printf( "Journaling events\n");
		com_journalFile = FS_FOpenFileWrite_HomeState( "journal.dat" );
		com_journalDataFile = FS_FOpenFileWrite_HomeState( "journaldata.dat" );
	} else if ( com_journal->integer == 2 ) {
		Com_Printf( "Replaying journaled events\n");
		FS_FOpenFileRead( "journal.dat", &com_journalFile, qtrue );
		FS_FOpenFileRead( "journaldata.dat", &com_journalDataFile, qtrue );
	}

	if ( !com_journalFile || !com_journalDataFile ) {
		Cvar_Set( "com_journal", "0" );
		com_journalFile = 0;
		com_journalDataFile = 0;
		Com_Printf( "Couldn't open journal files\n" );
	}
}

/*
========================================================================

EVENT LOOP

========================================================================
*/

#define MAX_QUEUED_EVENTS  256
#define MASK_QUEUED_EVENTS ( MAX_QUEUED_EVENTS - 1 )

static sysEvent_t  eventQueue[ MAX_QUEUED_EVENTS ];
static int         eventHead = 0;
static int         eventTail = 0;

/*
================
Com_QueueEvent

A time of 0 will get the current time
Ptr should either be null, or point to a block of data that can
be freed by the game later.
================
*/
void Com_QueueEvent( int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr )
{
	sysEvent_t  *ev;

	// combine mouse movement with previous mouse event
	if ( type == SE_MOUSE && eventHead != eventTail )
	{
		ev = &eventQueue[ ( eventHead + MAX_QUEUED_EVENTS - 1 ) & MASK_QUEUED_EVENTS ];

		if ( ev->evType == SE_MOUSE )
		{
			ev->evValue += value;
			ev->evValue2 += value2;
			return;
		}
	}

	ev = &eventQueue[ eventHead & MASK_QUEUED_EVENTS ];

	if ( eventHead - eventTail >= MAX_QUEUED_EVENTS )
	{
		Com_Printf("Com_QueueEvent: overflow\n");
		// we are discarding an event, but don't leak memory
		if ( ev->evPtr )
		{
			Z_Free( ev->evPtr );
		}
		eventTail++;
	}

	eventHead++;

	if ( time == 0 )
	{
		time = Sys_Milliseconds();
	}

	ev->evTime = time;
	ev->evType = type;
	ev->evValue = value;
	ev->evValue2 = value2;
	ev->evPtrLength = ptrLength;
	ev->evPtr = ptr;
}

/*
================
Com_GetSystemEvent

================
*/
sysEvent_t Com_GetSystemEvent( void )
{
	sysEvent_t  ev;
	char        *s;

	// return if we have data
	if ( eventHead > eventTail )
	{
		eventTail++;
		return eventQueue[ ( eventTail - 1 ) & MASK_QUEUED_EVENTS ];
	}

	// check for console commands
	s = Sys_ConsoleInput();
	if ( s )
	{
		char  *b;
		int   len;

		len = strlen( s ) + 1;
		b = Z_Malloc( len );
		strcpy( b, s );
		Com_QueueEvent( 0, SE_CONSOLE, 0, 0, len, b );
	}

	// return if we have data
	if ( eventHead > eventTail )
	{
		eventTail++;
		return eventQueue[ ( eventTail - 1 ) & MASK_QUEUED_EVENTS ];
	}

	// create an empty event to return
	memset( &ev, 0, sizeof( ev ) );
	ev.evTime = Sys_Milliseconds();

	return ev;
}

/*
=================
Com_GetRealEvent
=================
*/
sysEvent_t	Com_GetRealEvent( void ) {
	int			r;
	sysEvent_t	ev;

	// either get an event from the system or the journal file
	if ( com_journal->integer == 2 ) {
		r = FS_Read( &ev, sizeof(ev), com_journalFile );
		if ( r != sizeof(ev) ) {
			Com_Error( ERR_FATAL, "Error reading from journal file" );
		}
		if ( ev.evPtrLength ) {
			ev.evPtr = Z_Malloc( ev.evPtrLength );
			r = FS_Read( ev.evPtr, ev.evPtrLength, com_journalFile );
			if ( r != ev.evPtrLength ) {
				Com_Error( ERR_FATAL, "Error reading from journal file" );
			}
		}
	} else {
		ev = Com_GetSystemEvent();

		// write the journal value out if needed
		if ( com_journal->integer == 1 ) {
			r = FS_Write( &ev, sizeof(ev), com_journalFile );
			if ( r != sizeof(ev) ) {
				Com_Error( ERR_FATAL, "Error writing to journal file" );
			}
			if ( ev.evPtrLength ) {
				r = FS_Write( ev.evPtr, ev.evPtrLength, com_journalFile );
				if ( r != ev.evPtrLength ) {
					Com_Error( ERR_FATAL, "Error writing to journal file" );
				}
			}
		}
	}

	return ev;
}


/*
=================
Com_InitPushEvent
=================
*/
void Com_InitPushEvent( void ) {
  // clear the static buffer array
  // this requires SE_NONE to be accepted as a valid but NOP event
  memset( com_pushedEvents, 0, sizeof(com_pushedEvents) );
  // reset counters while we are at it
  // beware: GetEvent might still return an SE_NONE from the buffer
  com_pushedEventsHead = 0;
  com_pushedEventsTail = 0;
}


/*
=================
Com_PushEvent
=================
*/
void Com_PushEvent( sysEvent_t *event ) {
	sysEvent_t		*ev;
	static int printedWarning = 0;

	ev = &com_pushedEvents[ com_pushedEventsHead & (MAX_PUSHED_EVENTS-1) ];

	if ( com_pushedEventsHead - com_pushedEventsTail >= MAX_PUSHED_EVENTS ) {

		// don't print the warning constantly, or it can give time for more...
		if ( !printedWarning ) {
			printedWarning = qtrue;
			Com_Printf( "WARNING: Com_PushEvent overflow\n" );
		}

		if ( ev->evPtr ) {
			Z_Free( ev->evPtr );
		}
		com_pushedEventsTail++;
	} else {
		printedWarning = qfalse;
	}

	*ev = *event;
	com_pushedEventsHead++;
}

/*
=================
Com_GetEvent
=================
*/
sysEvent_t	Com_GetEvent( void ) {
	if ( com_pushedEventsHead > com_pushedEventsTail ) {
		com_pushedEventsTail++;
		return com_pushedEvents[ (com_pushedEventsTail-1) & (MAX_PUSHED_EVENTS-1) ];
	}
	return Com_GetRealEvent();
}

/*
=================
Com_RunAndTimeServerPacket
=================
*/
void Com_RunAndTimeServerPacket( netadr_t *evFrom, msg_t *buf ) {
	int		t1, t2, msec;

	t1 = 0;

	if ( com_speeds->integer ) {
		t1 = Sys_Milliseconds ();
	}

	SV_PacketEvent( *evFrom, buf );

	if ( com_speeds->integer ) {
		t2 = Sys_Milliseconds ();
		msec = t2 - t1;
		if ( com_speeds->integer == 3 ) {
			Com_Printf( "SV_PacketEvent time: %i\n", msec );
		}
	}
}

/*
=================
Com_EventLoop

Returns last event time
=================
*/
int Com_EventLoop( void ) {
	sysEvent_t	ev;
	netadr_t	evFrom;
	byte		bufData[MAX_MSGLEN];
	msg_t		buf;

	MSG_Init( &buf, bufData, sizeof( bufData ) );

	while ( 1 ) {
		ev = Com_GetEvent();

		// if no more events are available
		if ( ev.evType == SE_NONE ) {
			// manually send packet events for the loopback channel
			while ( NET_GetLoopPacket( NS_CLIENT, &evFrom, &buf ) ) {
				CL_PacketEvent( evFrom, &buf );
			}

			while ( NET_GetLoopPacket( NS_SERVER, &evFrom, &buf ) ) {
				// if the server just shut down, flush the events
				if ( com_sv_running->integer ) {
					Com_RunAndTimeServerPacket( &evFrom, &buf );
				}
			}

			return ev.evTime;
		}


		switch(ev.evType)
		{
			case SE_KEY:
				CL_KeyEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
			case SE_CHAR:
				CL_CharEvent( ev.evValue );
			break;
			case SE_MOUSE:
				CL_MouseEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
			case SE_JOYSTICK_AXIS:
				CL_JoystickEvent( ev.evValue, ev.evValue2, ev.evTime );
			break;
			case SE_CONSOLE:
				Cbuf_AddText( (char *)ev.evPtr );
				Cbuf_AddText( "\n" );
			break;
			default:
				Com_Error( ERR_FATAL, "Com_EventLoop: bad event type %i", ev.evType );
			break;
		}

		// free any block data
		if ( ev.evPtr ) {
			Z_Free( ev.evPtr );
		}
	}

	return 0;	// never reached
}

/*
================
Com_Milliseconds

Can be used for profiling, but will be journaled accurately
================
*/
int Com_Milliseconds (void) {
	sysEvent_t	ev;

	// get events and push them until we get a null event with the current time
	do {

		ev = Com_GetRealEvent();
		if ( ev.evType != SE_NONE ) {
			Com_PushEvent( &ev );
		}
	} while ( ev.evType != SE_NONE );
	
	return ev.evTime;
}

//============================================================================

/*
=============
Com_Error_f

Just throw a fatal error to
test error shutdown procedures
=============
*/
static void Q_NO_RETURN Com_Error_f (void) {
	if ( Cmd_Argc() > 1 ) {
		Com_Error( ERR_DROP, "Testing drop error" );
	} else {
		Com_Error( ERR_FATAL, "Testing fatal error" );
	}
}


/*
=============
Com_Freeze_f

Just freeze in place for a given number of seconds to test
error recovery
=============
*/
static void Com_Freeze_f (void) {
	float	s;
	int		start, now;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "freeze <seconds>\n" );
		return;
	}
	s = atof( Cmd_Argv(1) );

	start = Com_Milliseconds();

	while ( 1 ) {
		now = Com_Milliseconds();
		if ( ( now - start ) * 0.001 > s ) {
			break;
		}
	}
}

/*
=================
Com_Crash_f

A way to force a bus error for development reasons
=================
*/
static void Com_Crash_f( void ) {
	* ( volatile int * ) 0 = 0x12345678;
}

/*
==================
Com_Setenv_f

For controlling environment variables
==================
*/
void Com_Setenv_f(void)
{
	int argc = Cmd_Argc();
	char *arg1 = Cmd_Argv(1);

	if(argc > 2)
	{
		char *arg2 = Cmd_ArgsFrom(2);
		
		Sys_SetEnv(arg1, arg2);
	}
	else if(argc == 2)
	{
		char *env = getenv(arg1);
		
		if(env)
			Com_Printf("%s=%s\n", arg1, env);
		else
			Com_Printf("%s undefined\n", arg1);
        }
}

// Com_IsClient, decided at start; the client's is a client until its
// command line says otherwise, so its first errors still show a dialog
#ifdef DEDICATED
static qboolean	com_isClient = qfalse;
#else
static qboolean	com_isClient = qtrue;
#endif

/*
=================
Com_IsClient
=================
*/
qboolean Com_IsClient( void ) {
	return com_isClient;
}

/*
=================
Com_ConfigWritten

A config or settings file was written: the web page keeps the home in the
browser's storage for the player's next visit, and saves it now rather
than only as the page closes
=================
*/
static void Com_ConfigWritten( void ) {
#ifdef __EMSCRIPTEN__
	MAIN_THREAD_EM_ASM( { Module.configWritten?.(); } );
#endif
}

// The files settings are saved in, in the order they're read: where each is
// in the home's config directory, with %s for the game directory's own; the
// scopes it holds; whether it holds the binds; and its text as it was read
// or last written, NULL where there's no file, so a file is written only
// when what it would hold changed. A client keeps settings/, where the base
// game has its own as a mod does, so switching between them moves nothing;
// a dedicated server keeps one config in its game directory, of every scope
// that's saved
typedef struct {
	const char	*path;
	int		scopes;
	qboolean	binds;
	char		*text;
} settingsFile_t;

static settingsFile_t com_settingsFiles[] = {
	{ "settings/device.cfg", CVAR_SCOPE_BIT( CVAR_SCOPE_DEVICE ), qfalse },
	{ "settings/player.cfg", CVAR_SCOPE_BIT( CVAR_SCOPE_PLAYER ), qtrue },
	{ "settings/server.cfg", CVAR_SCOPE_BIT( CVAR_SCOPE_SERVER ), qfalse },
	{ "settings/devices/%s.cfg", CVAR_SCOPE_BIT( CVAR_SCOPE_DEVICE_MOD ), qfalse },
	{ "settings/mods/%s.cfg", CVAR_SCOPE_BIT( CVAR_SCOPE_PLAYER_MOD ), qfalse }
};

static settingsFile_t com_serverConfig = { "%s/" Q3CONFIG_CFG, CVAR_SCOPES_SAVED, qfalse };

static char	com_settingsMod[MAX_QPATH];	// the game directory whose settings were read

/*
=================
Com_SettingsFiles

The files this program saves its settings in
=================
*/
static settingsFile_t *Com_SettingsFiles( int *count ) {
	if ( Com_IsClient() ) {
		*count = ARRAY_LEN( com_settingsFiles );
		return com_settingsFiles;
	}
	*count = 1;
	return &com_serverConfig;
}

/*
=================
Com_SettingsPath

Where one of the files is, for the game directory whose settings were read
=================
*/
static void Com_SettingsPath( const settingsFile_t *file, char *path, int size ) {
	Com_sprintf( path, size, file->path, com_settingsMod );
}

/*
=================
Com_IsDefault

Whether a value is one of a cvar's defaults in a program's table, which
can list a name once per default
=================
*/
static qboolean Com_IsDefault( const cvarDefault_t *defaults, const char *name, const char *value ) {
	for ( ; defaults && defaults->name; defaults++ ) {
		if ( !Q_stricmp( defaults->name, name ) && Cvar_SameValue( defaults->value, value ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
Com_IsPerModScope

Whether a cvar of this name is saved for the game directory running: a
mod's own, declared so, or not declared and not the engine's (Cvar_Scope)
=================
*/
static qboolean Com_IsPerModScope( const char *name ) {
	int scope = Cvar_DeclaredScope( name );
	int flags;

	if ( scope >= 0 ) {
		return scope == CVAR_SCOPE_PLAYER_MOD || scope == CVAR_SCOPE_DEVICE_MOD;
	}
	flags = Cvar_Flags( name );
	return flags == CVAR_NONEXISTENT || ( flags & ( CVAR_VM_CREATED | CVAR_USER_CREATED ) );
}

// a cvar of ours another program saves under its own name, and what an
// imported line of it sets
typedef struct {
	const char	*name;
	const char	*ours;	// NULL: the line is left out
	const char	*also;	// a line it sets as well, or NULL
	const char	*pair;	// the writer's name for the other half of a pair
				// set together, which a config leaving it at the
				// writer's default gets at that default, or NULL
} cvarRename_t;

// CNQ3's mouse, which computes as ours does, its v-sync and its window's
// size, which ours has only at r_mode -1: a width or height chosen alone
// keeps CNQ3's default for the other (1280x1024 is r_height alone). Its
// r_mode is its fullscreen resolution, the desktop's or upscaled, where
// ours is always the desktop's, and is left out
static const cvarRename_t com_cnq3Renames[] = {
	{ "m_speed", "sensitivity", NULL, NULL },
	{ "m_accel", "cl_mouseAccel", NULL, NULL },
	{ "m_accelStyle", "cl_mouseAccelStyle", NULL, NULL },
	{ "m_accelOffset", "cl_mouseAccelOffset", NULL, NULL },
	{ "m_limit", "cl_mouseAccelLimit", NULL, NULL },
	{ "r_vsync", "r_swapInterval", NULL, NULL },
	{ "r_width", "r_customwidth", "seta r_mode -1", "r_height" },
	{ "r_height", "r_customheight", "seta r_mode -1", "r_width" },
	{ "r_mode", NULL, NULL, NULL },
	{ NULL, NULL, NULL, NULL }
};

/*
=================
Com_DefaultOf

A cvar's first default in a writer's table, or NULL if it has none
=================
*/
static const char *Com_DefaultOf( const cvarDefault_t *defaults, const char *name ) {
	for ( ; defaults && defaults->name; defaults++ ) {
		if ( !Q_stricmp( defaults->name, name ) ) {
			return defaults->value;
		}
	}
	return NULL;
}

// what the player feels at once, the mouse's speed, the volume and the
// brightness: an imported config's value comes across even at its
// writer's default, where the player played with it, as a default of ours
// that differs would change the game under them (Joe, 2026-10-06, open
// question 25, option D). Only CNQ3's differ (2, 0.2 and 1.2 against 5,
// 0.8 and 1); one that is ours as well isn't saved
static const char * const com_feltSettings[] = { "sensitivity", "s_volume", "r_gamma", NULL };

// a stock menu's top choice, saved as chosen though it meant "the fastest
// there is": Quake III's Data Rate list tops out at LAN/Cable/xDSL, 25000,
// which every other writer has as its default. From any program's config
// it's left out as a default is, so ours applies (2026-10-07)
static const cvarDefault_t com_menuTops[] = { { "rate", "25000" }, { NULL, NULL } };

/*
=================
Com_IsFelt

Whether a cvar is one of com_feltSettings
=================
*/
static qboolean Com_IsFelt( const char *name ) {
	const char * const	*felt;

	for ( felt = com_feltSettings; *felt; felt++ ) {
		if ( !Q_stricmp( *felt, name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
Com_Rename

A name's rename in a writer's table, or NULL
=================
*/
static const cvarRename_t *Com_Rename( const cvarRename_t *renames, const char *name ) {
	for ( ; renames && renames->name; renames++ ) {
		if ( !Q_stricmp( renames->name, name ) ) {
			return renames;
		}
	}
	return NULL;
}

// the configs a --check's startup ran, and from where (Com_NoteConfigRun)
#define	MAX_CONFIGS_RUN	64
static struct {
	char	name[MAX_QPATH];
	char	from[MAX_OSPATH];
} com_configsRun[MAX_CONFIGS_RUN];
static int	com_numConfigsRun;

/*
=================
Com_NoteConfigRun

A config --check's startup ran, and where it was found, which it lists
=================
*/
void Com_NoteConfigRun( const char *name, const char *from ) {
	if ( com_numConfigsRun == MAX_CONFIGS_RUN ) {
		return;
	}
	Q_strncpyz( com_configsRun[com_numConfigsRun].name, name, sizeof( com_configsRun[0].name ) );
	Q_strncpyz( com_configsRun[com_numConfigsRun].from, from, sizeof( com_configsRun[0].from ) );
	com_numConfigsRun++;
}

/*
=================
Com_PrintConfigsRun
=================
*/
void Com_PrintConfigsRun( void ) {
	int	i;

	for ( i = 0; i < com_numConfigsRun; i++ ) {
		Com_Printf( "  %-24s %s\n", com_configsRun[i].name, com_configsRun[i].from );
	}
}

/*
=================
Com_ExecuteConfigLine

Runs a settings file's line at once, as from that file and line
=================
*/
static void Com_ExecuteConfigLine( const char *path, int number, const char *line ) {
	Cmd_PushOrigin( path, number );
	Cbuf_ExecuteText( EXEC_NOW, line );
	Cmd_PopOrigin();
}

/*
=================
Com_ApplyConfig

A config's settings and binds, as the player's own, with full rights, as
they're read. A config is data, so anything else in it is left out, with a
warning: Quake III's aliases are cvars (vstr), which apply as settings. An
imported config's values that are its writer's defaults are left out too
(Com_ConfigWriter), as it saved them unchosen, and so is a stock menu's top
choice (com_menuTops), but for those the player feels at once
(com_feltSettings), and those its writer names otherwise set ours
(renames); and with modOnly, all but the cvars saved for the game
directory, a mod's config read on the mod's first visit
=================
*/
static void Com_ApplyConfig( char *text, const char *path, const cvarDefault_t *defaults,
	const cvarRename_t *renames, qboolean modOnly ) {
	char		*line, *next;
	int		number = 0;
	qboolean	inMod = Q_stricmp( FS_GetCurrentGameDir(), com_basegame->string ) != 0;
	qboolean	paired = qfalse;	// a half of the writer's one pair is set
	for ( line = text; line; line = next ) {
		const char		*command, *name;
		const cvarRename_t	*rename;
		qboolean		set;

		next = strchr( line, '\n' );
		if ( next ) {
			*next++ = 0;
		}
		number++;
		Cmd_TokenizeString( line );
		if ( !Cmd_Argc() ) {
			continue;
		}
		command = Cmd_Argv( 0 );
		set = !Q_stricmp( command, "seta" ) || !Q_stricmp( command, "set" );
		rename = set ? Com_Rename( renames, Cmd_Argv( 1 ) ) : NULL;
		if ( rename && !rename->ours ) {
			continue;
		}
		name = rename ? rename->ours : Cmd_Argv( 1 );
		if ( modOnly && ( !set || !Com_IsPerModScope( name ) ) ) {
			continue;
		}
		if ( set ) {
			// the tables hold the base game's defaults, under the writer's
			// names, not a mod's own
			const char	*value = Cmd_ArgsFrom( 2 );
			qboolean	unchosen = Com_IsDefault( defaults, Cmd_Argv( 1 ), value ) ||
				( defaults && Com_IsDefault( com_menuTops, Cmd_Argv( 1 ), value ) );

			if ( ( inMod && Com_IsPerModScope( name ) ) || Com_IsFelt( name ) || !unchosen ) {
				if ( rename ) {
					char			setting[MAX_STRING_CHARS], partner[MAX_STRING_CHARS] = "";
					const cvarRename_t	*other = rename->pair ? Com_Rename( renames, rename->pair ) : NULL;
					const char		*otherDefault = other && other->ours && !paired ?
						Com_DefaultOf( defaults, rename->pair ) : NULL;

					// both lines made before either runs, which retokenizes
					Com_sprintf( setting, sizeof( setting ), "%s %s \"%s\"", command, name, Cmd_ArgsFrom( 2 ) );
					// the pair's first half chosen gives the other the
					// writer's default, which the other's own line, if
					// the config chose it too, then sets over
					if ( otherDefault ) {
						Com_sprintf( partner, sizeof( partner ), "%s %s \"%s\"", command, other->ours, otherDefault );
					}
					if ( rename->pair ) {
						paired = qtrue;
					}
					Com_ExecuteConfigLine( path, number, setting );
					if ( partner[0] ) {
						Com_ExecuteConfigLine( path, number, partner );
					}
					if ( rename->also ) {
						Com_ExecuteConfigLine( path, number, rename->also );
					}
				} else {
					Com_ExecuteConfigLine( path, number, line );
				}
			}
		} else if ( !Q_stricmp( command, "bind" ) || !Q_stricmp( command, "unbind" ) ||
			!Q_stricmp( command, "unbindall" ) ) {
			Com_ExecuteConfigLine( path, number, line );
		} else {
			Com_Printf( S_COLOR_YELLOW "WARNING: %s, line %d: %s isn't a setting or a binding, and was left out\n",
				path, number, command );
		}
	}
}

// a program writing stock Quake III's first line, whose configs the
// importer reads
typedef struct {
	const char		*name;
	const cvarDefault_t	*defaults;	// its saved cvars' defaults
	const char * const	*marks;		// cvars it saves that no other has
	const cvarRename_t	*renames;	// its names for cvars of ours, or NULL
} configWriter_t;

// each told by its marks, a cvar it saves that no other program even
// registers, so the player can't have set it with seta; stock, with no
// marks, last
static const configWriter_t com_quakeWriters[] = {
	{ "CNQ3", cvar_cnq3Defaults, cvar_cnq3Marks, com_cnq3Renames },
	{ "Quake3e", cvar_q3eDefaults, cvar_q3eMarks, NULL },
	{ "ioquake3", cvar_ioq3Defaults, cvar_ioq3Marks, NULL },	// before 2025-10-17
	{ "Quake III", cvar_q3Defaults, NULL, NULL }
};

/*
=================
Com_SavesAny

Whether a config saves any of the cvars named
=================
*/
static qboolean Com_SavesAny( const char *text, const char * const *names ) {
	for ( ; names && *names; names++ ) {
		if ( strstr( text, va( "\nseta %s ", *names ) ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
Com_ConfigWriter

The program that wrote a config, by its first line (COM_CONFIG_HEADER),
the defaults it saved unchosen cvars at, and its names for cvars of ours
(cvarRename_t): stock Quake III's, or that of another program writing
its first line, by its marks (com_quakeWriters); ioquake3's; or this
engine's, whose values are all chosen once it states its version, and
which before that held ioquake3's defaults. A config anyone else wrote,
by hand among them, is all chosen
=================
*/
static const char *Com_ConfigWriter( const char *text, const cvarDefault_t **defaults, const cvarRename_t **renames ) {
	int	writer;

	*defaults = NULL;
	*renames = NULL;
	// first, as it can be ioquake3's own first line (PRODUCT_NAME "ioq3")
	if ( !Q_strncmp( text, COM_CONFIG_HEADER, strlen( COM_CONFIG_HEADER ) ) ) {
		if ( !strstr( text, "\nseta com_configVersion " ) ) {
			*defaults = cvar_ioq3Defaults;
		}
		return PRODUCT_NAME;
	}
	if ( !Q_strncmp( text, "// generated by quake,", strlen( "// generated by quake," ) ) ) {
		for ( writer = 0; writer < ARRAY_LEN( com_quakeWriters ) - 1; writer++ ) {
			if ( Com_SavesAny( text, com_quakeWriters[writer].marks ) ) {
				break;
			}
		}
		*defaults = com_quakeWriters[writer].defaults;
		*renames = com_quakeWriters[writer].renames;
		return com_quakeWriters[writer].name;
	}
	if ( !Q_strncmp( text, "// generated by ioq3,", strlen( "// generated by ioq3," ) ) ) {
		*defaults = cvar_ioq3Defaults;
		return "ioquake3";
	}
	return "someone else";
}

/*
=================
Com_ImportConfig

Applies a config (Com_ApplyConfig) as its writer saved it
(Com_ConfigWriter); returns who wrote it
=================
*/
static const char *Com_ImportConfig( char *text, const char *path, qboolean homeConfig, qboolean modOnly ) {
	const cvarDefault_t	*defaults;
	const cvarRename_t	*renames;
	const char		*writer = Com_ConfigWriter( text, &defaults, &renames );

	if ( com_check ) {
		Com_NoteConfigRun( path, va( "%s, saved settings written by %s", homeConfig ?
			FS_BaseDir_BuildOSPath( Cvar_VariableString( "fs_homeconfigpath" ), path ) : FS_LastFilePath( path ), writer ) );
	}
	Com_ApplyConfig( text, path, defaults, renames, modOnly );
	return writer;
}

/*
=================
Com_ImportOldConfig

The config the game directory kept before settings/ (one copied from
ioquake3's home among them), left as it is, imported. Returns whether
there was one
=================
*/
static qboolean Com_ImportOldConfig( const char *game, qboolean modOnly ) {
	char	path[MAX_QPATH];
	char	*text;

	Com_sprintf( path, sizeof( path ), "%s/" Q3CONFIG_CFG, game );
	if ( FS_BaseDir_ReadFile_HomeConfig( path, (void **)&text ) < 0 ) {
		return qfalse;
	}
	Com_Printf( "Imported %s, written by %s.\n", path, Com_ImportConfig( text, path, qtrue, modOnly ) );
	Z_Free( text );
	return qtrue;
}

/*
=================
Com_ReadSettingsFile

Applies one of the files, if there is one, from the home's config
directory only, as its writer saved it (Com_ImportConfig): a dedicated
server's config from before this engine stated its version holds
ioquake3's defaults. Keeps the file's text, NULL where there's none, which
a write compares with
=================
*/
static qboolean Com_ReadSettingsFile( settingsFile_t *file ) {
	char	path[MAX_QPATH];
	char	*text;

	if ( file->text ) {
		Z_Free( file->text );
	}
	Com_SettingsPath( file, path, sizeof( path ) );
	if ( FS_BaseDir_ReadFile_HomeConfig( path, (void **)&file->text ) < 0 ) {
		return qfalse;
	}
	// applying it cuts its text into lines; a copy from the main zone, as
	// a config can outgrow the small one CopyString uses
	text = Z_Malloc( strlen( file->text ) + 1 );
	strcpy( text, file->text );
	Com_ImportConfig( text, path, qtrue, qfalse );
	Z_Free( text );
	return qtrue;
}

/*
=================
Com_ReadSettings

The settings saved, in the files' order: a client's device's, player's,
listen server's, then the game directory's own; or a dedicated server's
config. What a client's settings/ lacks, the config the game directory
kept before has: all of it on the first start with settings/, and the
directory's own on its first visit. The files that didn't exist are
written at the next write, as what they'd hold differs from nothing
=================
*/
static void Com_ReadSettings( void ) {
	qboolean	found = qfalse, foundMod = qfalse;
	int		i;

	if ( !Com_IsClient() ) {
		Com_ReadSettingsFile( &com_serverConfig );
		return;
	}
	for ( i = 0; i < ARRAY_LEN( com_settingsFiles ); i++ ) {
		if ( Com_ReadSettingsFile( &com_settingsFiles[i] ) ) {
			if ( com_settingsFiles[i].scopes &
				( CVAR_SCOPE_BIT( CVAR_SCOPE_PLAYER_MOD ) | CVAR_SCOPE_BIT( CVAR_SCOPE_DEVICE_MOD ) ) ) {
				foundMod = qtrue;
			} else {
				found = qtrue;
			}
		}
	}
	if ( !found ) {
		Com_ImportOldConfig( com_settingsMod, qfalse );
	} else if ( !foundMod ) {
		Com_ImportOldConfig( com_settingsMod, qtrue );
	}
}

/*
=================
Com_ExecuteScript

A config that runs at every start, whose sets are a script's: applied,
and not saved over the player's own choices
=================
*/
static void Com_ExecuteScript( const char *name ) {
	// one that's run if it's there: no need to say where it isn't
	if ( FS_ReadFile( name, NULL ) < 0 ) {
		Com_Printf( "couldn't exec %s\n", name );
		return;
	}
	Cbuf_AddScriptText( va( "exec %s\n", name ) );
	Cbuf_ExecuteScripts();
}

/*
==================
Com_ExecuteCfg

default.cfg and autoexec.cfg are scripts, around the settings saved
(Com_ReadSettings). The next write compares the files with what they'd
hold, and writes those that differ: an imported config's, and those that
didn't exist. "safe" on the command line skips the settings and
autoexec.cfg, and that first write saves over them, as stock's did
==================
*/
void Com_ExecuteCfg(void)
{
	// the game directory whose settings are read and written
	Q_strncpyz( com_settingsMod, FS_LoadedGameDir(), sizeof( com_settingsMod ) );

	Com_ExecuteScript( "default.cfg" );

	if(!Com_SafeMode())
	{
		Com_ReadSettings();
		Com_ExecuteScript( "autoexec.cfg" );
	}
	else
	{
		// nothing was read, so nothing is compared with: a game directory
		// changed to isn't compared with the one left's text
		int		count, i;
		settingsFile_t	*files = Com_SettingsFiles( &count );

		for ( i = 0; i < count; i++ ) {
			if ( files[i].text ) {
				Z_Free( files[i].text );
				files[i].text = NULL;
			}
		}
	}
	cvar_modifiedFlags |= CVAR_ARCHIVE;
}

/*
==================
Com_GameRestart

Change to a new mod properly with cleaning up cvars before switching.
==================
*/

void Com_GameRestart(int checksumFeed, qboolean disconnect)
{
	// make sure no recursion can be triggered
	if(!com_gameRestarting && com_fullyInitialized)
	{
		com_gameRestarting = qtrue;
		com_gameClientRestarting = com_cl_running->integer;

		// the settings changed in the game left
		Com_WriteConfiguration();

		// Kill server if we have one
		if(com_sv_running->integer)
			SV_Shutdown("Game directory changed");

		if(com_gameClientRestarting)
		{
			if(disconnect)
				CL_Disconnect(qfalse);
				
			CL_Shutdown("Game directory changed", disconnect, qfalse);
		}

		FS_Restart(checksumFeed);
	
		// Clean out any user and VM created cvars
		Cvar_Restart(qtrue);
		Com_ExecuteCfg();
		// the command line's sets, which Cvar_Restart undid, after the
		// configs as at startup; not those of the cvars it kept, such as
		// fs_game, which the restart may have changed
		Com_StartupVariables( NULL, CVAR_ROM | CVAR_INIT | CVAR_NORESTART );

		if(disconnect)
		{
			// We don't want to change any network settings if gamedir
			// change was triggered by a connect to server because the
			// new network settings might make the connection fail.
			NET_Restart_f();
		}

		if(com_gameClientRestarting)
		{
			CL_Init();
			CL_StartHunkUsers(qfalse);
		}
		
		com_gameRestarting = qfalse;
		com_gameClientRestarting = qfalse;
	}
}

/*
==================
Com_GameRestart_f

Expose possibility to change current running mod to the user
==================
*/

void Com_GameRestart_f(void)
{
	Cvar_Set("fs_game", Cmd_Argv(1));

	Com_GameRestart(0, qtrue);
}

#ifndef STANDALONE

// TTimo: centralizing the cl_cdkey stuff after I discovered a buffer overflow problem with the dedicated server version
//   not sure it's necessary to have different defaults for regular and dedicated, but I don't want to risk it
//   https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=470
#ifndef DEDICATED
char	cl_cdkey[34] = "                                ";
#else
char	cl_cdkey[34] = "123456789";
#endif

/*
=================
Com_ReadCDKey
=================
*/
qboolean CL_CDKeyValidate( const char *key, const char *checksum );
void Com_ReadCDKey( const char *filename ) {
	fileHandle_t	f;
	char			buffer[33];
	char			fbuffer[MAX_OSPATH];

	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/q3key", filename);

	FS_BaseDir_FOpenFileRead( fbuffer, &f );
	if ( !f ) {
		Com_Memset( cl_cdkey, '\0', 17 );
		return;
	}

	Com_Memset( buffer, 0, sizeof(buffer) );

	FS_Read( buffer, 16, f );
	FS_FCloseFile( f );

	Q_strlwr(buffer);

	if (CL_CDKeyValidate(buffer, NULL)) {
		Q_strncpyz( cl_cdkey, buffer, 17 );
	} else {
		Com_Memset( cl_cdkey, '\0', 17 );
	}
}

/*
=================
Com_AppendCDKey
=================
*/
void Com_AppendCDKey( const char *filename ) {
	fileHandle_t	f;
	char			buffer[33];
	char			fbuffer[MAX_OSPATH];

	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/q3key", filename);

	FS_BaseDir_FOpenFileRead( fbuffer, &f );
	if (!f) {
		Com_Memset( &cl_cdkey[16], '\0', 17 );
		return;
	}

	Com_Memset( buffer, 0, sizeof(buffer) );

	FS_Read( buffer, 16, f );
	FS_FCloseFile( f );

	if (CL_CDKeyValidate(buffer, NULL)) {
		strcat( &cl_cdkey[16], buffer );
	} else {
		Com_Memset( &cl_cdkey[16], '\0', 17 );
	}
}

#ifndef DEDICATED
/*
=================
Com_WriteCDKey
=================
*/
static void Com_WriteCDKey( const char *filename, const char *ikey ) {
	fileHandle_t	f;
	char			fbuffer[MAX_OSPATH];
	char			key[17];
#ifndef _WIN32
	mode_t			savedumask;
#endif


	Com_sprintf(fbuffer, sizeof(fbuffer), "%s/q3key", filename);


	Q_strncpyz( key, ikey, 17 );

	if(!CL_CDKeyValidate(key, NULL) ) {
		return;
	}

#ifndef _WIN32
	savedumask = umask(0077);
#endif
	f = FS_BaseDir_FOpenFileWrite_HomeState( fbuffer );
	if ( !f ) {
		Com_Printf ("Couldn't write CD key to %s.\n", fbuffer );
		goto out;
	}

	Q_strlwr(key);

	FS_Write( key, 16, f );

	FS_Printf( f, "\n// generated by " PRODUCT_NAME ", do not modify\r\n" );
	FS_Printf( f, "// Do not give this file to ANYONE.\r\n" );
	FS_Printf( f, "// id Software and Activision will NOT ask you to send this file to them.\r\n");

	FS_FCloseFile( f );
out:
#ifndef _WIN32
	umask(savedumask);
#else
	;
#endif
}
#endif

#endif // STANDALONE

static void Com_DetectAltivec(void)
{
	// Only detect if user hasn't forcibly disabled it.
	if (com_altivec->integer) {
		static qboolean altivec = qfalse;
		static qboolean detected = qfalse;
		if (!detected) {
			altivec = !!( Sys_GetProcessorFeatures( ) & CF_ALTIVEC );
			detected = qtrue;
		}

		if (!altivec) {
			Cvar_Set( "com_altivec", "0" );  // we don't have it! Disable support!
		}
	}
}

/*
=================
Com_DetectSSE
Find out whether we have SSE support for Q_ftol function
=================
*/

#if id386 || idx64

static void Com_DetectSSE(void)
{
#if !idx64
	cpuFeatures_t feat;
	
	feat = Sys_GetProcessorFeatures();

	if(feat & CF_SSE)
	{
		if(feat & CF_SSE2)
			Q_SnapVector = qsnapvectorsse;
		else
			Q_SnapVector = qsnapvectorx87;

		Q_ftol = qftolsse;
#endif
		Q_VMftol = qvmftolsse;

		Com_Printf("SSE instruction set enabled\n");
#if !idx64
	}
	else
	{
		Q_ftol = qftolx87;
		Q_VMftol = qvmftolx87;
		Q_SnapVector = qsnapvectorx87;

		Com_Printf("SSE instruction set not available\n");
	}
#endif
}

#else

#define Com_DetectSSE()

#endif

/*
=================
Com_InitRand
Seed the random number generator, if possible with an OS supplied random seed.
=================
*/
static void Com_InitRand(void)
{
	unsigned int seed;

	if(Sys_RandomBytes((byte *) &seed, sizeof(seed)))
		srand(seed);
	else
		srand(time(NULL));
}

// A touch device's defaults where they differ from the engine's
// (Cvar_SetProfile): half-size textures without filtering at an angle,
// until a phone profile is measured, as full-size textures take four times
// the memory
static const cvarDefault_t com_touchProfile[] = {
	{ "r_picmip", "1" },
	{ "r_ext_texture_filter_anisotropic", "0" },
	{ NULL, NULL }
};

/*
=================
Com_Init
=================
*/
void Com_Init( char *commandLine ) {
	char	*s;
	int	qport;

	Com_Printf( "%s %s %s\n", Q3_VERSION, PLATFORM_STRING, PRODUCT_DATE );

	if ( setjmp (abortframe) ) {
		Sys_Error ("Error during initialization");
	}

	// Clear queues
	Com_Memset( &eventQueue[ 0 ], 0, MAX_QUEUED_EVENTS * sizeof( sysEvent_t ) );

	// initialize the weak pseudo-random number generator for use later.
	Com_InitRand();

	// do this before anything else decides to push events
	Com_InitPushEvent();

	Com_InitSmallZoneMemory();
	// before any cvar is set or registered
	Cvar_SetDeclarations( cvar_declarations );
	Cvar_Init ();
	if ( Sys_TouchDevice() ) {
		Cvar_SetProfile( com_touchProfile );
	}

	// prepare enough of the subsystems to handle
	// cvar and command buffer management
	Com_ParseCommandLine( commandLine );

//	Swap_Init ();
	Cbuf_Init ();

	Com_DetectSSE();

	// override anything from the config files with command line args
	Com_StartupVariable( NULL );

#ifndef DEDICATED
	com_isClient = !Cvar_VariableIntegerValue( "dedicated" );
#endif

	Com_InitZoneMemory();
	Cmd_Init ();

	// get the developer cvar set as early as possible
	com_developer = Cvar_Get("developer", "0", CVAR_TEMP);

	// as early, so the first lines have them too
	com_timestamps = Cvar_Get( "com_timestamps", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( com_timestamps, 0, 2, qtrue );
	Cvar_SetDescription( com_timestamps, "The date and time at the start of each line: "
		"0 nowhere; 1 in qconsole.log, the default; 2 on the console too, where tools "
		"reading the server's lines may not expect them." );

	// done early so bind command exists
	CL_InitKeyCommands();

	com_standalone = Cvar_Get("com_standalone", "0", CVAR_ROM);
	com_basegame = Cvar_Get("com_basegame", BASEGAME, CVAR_INIT);
	com_homepath = Cvar_Get("com_homepath", "", CVAR_INIT|CVAR_PROTECTED);

	FS_InitFilesystem ();

	Com_InitJournaling();

	// Add some commands here already so users can use them from config files
	Cmd_AddCommand ("setenv", Com_Setenv_f);
	if (com_developer && com_developer->integer)
	{
		Cmd_AddCommand ("error", Com_Error_f);
		Cmd_AddCommand ("crash", Com_Crash_f);
		Cmd_AddCommand ("freeze", Com_Freeze_f);
	}
	Cmd_AddCommand ("quit", Com_Quit_f);
	Cmd_AddCommand ("changeVectors", MSG_ReportChangeVectors_f );
	Cmd_AddCommand ("writeconfig", Com_WriteConfig_f );
	Cmd_AddCommand ("settings_export", Com_SettingsExport_f );
	Cmd_AddCommand ("settings_import", Com_SettingsImport_f );
	Cmd_SetCommandCompletionFunc( "writeconfig", Cmd_CompleteCfgName );
	Cmd_AddCommand("game_restart", Com_GameRestart_f);

	Com_ExecuteCfg();

	// the config states it (Com_WriteConfigLines), so the next start knows
	// the config is this new
	Cvar_SetDescription( Cvar_Get( "com_configVersion", COM_CONFIG_VERSION, CVAR_ARCHIVE | CVAR_PROTECTED ),
		"Which of the engine's changed defaults the config was written after; set by the engine" );

	// override anything from the config files with command line args
	Com_StartupVariable( NULL );

	// a fixed seed, and fixedtime, make a server's matches repeat exactly
	// under the same commands, which tests compare
	com_randomSeed = Cvar_Get( "com_randomSeed", "0", CVAR_INIT | CVAR_PROTECTED );
	Cvar_SetDescription( com_randomSeed, "Seed random numbers with this rather than the time, so matches repeat (for tests); 0 for the time" );
	if ( com_randomSeed->integer ) {
		srand( com_randomSeed->integer );
	}

  // get dedicated here for proper hunk megs initialization
#ifdef DEDICATED
	com_dedicated = Cvar_Get ("dedicated", "1", CVAR_INIT);
	Cvar_CheckRange( com_dedicated, 1, 2, qtrue );
#else
	com_dedicated = Cvar_Get ("dedicated", "0", CVAR_LATCH);
	Cvar_CheckRange( com_dedicated, 0, 2, qtrue );
#endif
	// allocate the stack based hunk allocator
	Com_InitHunkMemory();

	//
	// init commands and vars
	//
	com_altivec = Cvar_Get ("com_altivec", "1", CVAR_ARCHIVE);
#ifdef __EMSCRIPTEN__
	// Under Emscripten the browser handles throttling the frame rate.
	// Manual framerate throttling interacts poorly with Emscripten's
	// browser-driven event loop. So default throttling to off.
	com_maxfps = Cvar_Get ("com_maxfps", "0", CVAR_ARCHIVE);
#else
	// the display's refresh rate: with movement in fixed steps (pmove_fixed),
	// the frame rate no longer sets how the game moves
	com_maxfps = Cvar_Get ("com_maxfps", "-1", CVAR_ARCHIVE_ND);
#endif
	Cvar_CheckRange( com_maxfps, -2, 1000, qtrue );	// down to COM_MAXFPS_BELOW_DISPLAY
	Cvar_SetDescription( com_maxfps, "Frames a second at most; 0 for no cap, -1 for the display's refresh rate, "
		"-2 for 3% under it (a display of variable refresh rate, with vsync); both are 125 on a server "
		"that moves the player by the frame (pmove_fixed 0 or none, not CPMA), whose jumps are the classic height there" );
	com_blood = Cvar_Get ("com_blood", "1", CVAR_ARCHIVE);

	com_logfile = Cvar_Get ("logfile", "0", CVAR_TEMP | CVAR_PROTECTED );

	com_timescale = Cvar_Get ("timescale", "1", CVAR_CHEAT | CVAR_SYSTEMINFO );
	com_fixedtime = Cvar_Get ("fixedtime", "0", CVAR_CHEAT);
	com_showtrace = Cvar_Get ("com_showtrace", "0", CVAR_CHEAT);
	com_speeds = Cvar_Get ("com_speeds", "0", 0);
	com_timedemo = Cvar_Get ("timedemo", "0", CVAR_CHEAT);
	com_cameraMode = Cvar_Get ("com_cameraMode", "0", CVAR_CHEAT);

	cl_paused = Cvar_Get ("cl_paused", "0", CVAR_ROM);
	sv_paused = Cvar_Get ("sv_paused", "0", CVAR_ROM);
	cl_packetdelay = Cvar_Get ("cl_packetdelay", "0", CVAR_CHEAT);
	sv_packetdelay = Cvar_Get ("sv_packetdelay", "0", CVAR_CHEAT);
	com_sv_running = Cvar_Get ("sv_running", "0", CVAR_ROM);
	com_cl_running = Cvar_Get ("cl_running", "0", CVAR_ROM);
	com_buildScript = Cvar_Get( "com_buildScript", "0", 0 );
	com_ansiColor = Cvar_Get( "com_ansiColor", "0", CVAR_ARCHIVE );

	com_unfocused = Cvar_Get( "com_unfocused", "0", CVAR_ROM );
	com_minimized = Cvar_Get( "com_minimized", "0", CVAR_ROM );
#ifdef __EMSCRIPTEN__
	// the browser throttles a page in the background itself
	com_maxfpsUnfocused = Cvar_Get( "com_maxfpsUnfocused", "0", CVAR_ARCHIVE );
	com_maxfpsMinimized = Cvar_Get( "com_maxfpsMinimized", "0", CVAR_ARCHIVE );
#else
	// a game in the background needn't heat the machine or drain a battery
	com_maxfpsUnfocused = Cvar_Get( "com_maxfpsUnfocused", "60", CVAR_ARCHIVE_ND );
	com_maxfpsMinimized = Cvar_Get( "com_maxfpsMinimized", "20", CVAR_ARCHIVE_ND );
#endif
	com_abnormalExit = Cvar_Get( "com_abnormalExit", "0", CVAR_ROM );
	com_busyWait = Cvar_Get("com_busyWait", "0", CVAR_ARCHIVE);
#ifndef DEDICATED
	// the renderer's, set when it makes the window
	com_swapIntervalActive = Cvar_Get( "r_swapIntervalActive", "0", CVAR_ROM );
#endif
	Cvar_Get("com_errorMessage", "", CVAR_ROM | CVAR_NORESTART);
	Cvar_Get("com_errorCode", "", CVAR_ROM | CVAR_NORESTART);

#ifdef CINEMATICS_INTRO
	com_introPlayed = Cvar_Get( "com_introplayed", "0", CVAR_ARCHIVE);
#endif

	s = va("%s %s %s", Q3_VERSION, PLATFORM_STRING, PRODUCT_DATE );
	com_version = Cvar_Get ("version", s, CVAR_ROM | CVAR_SERVERINFO );
	com_gamename = Cvar_Get("com_gamename", GAMENAME_FOR_MASTER, CVAR_SERVERINFO | CVAR_INIT);
	com_protocol = Cvar_Get("com_protocol", va("%i", PROTOCOL_VERSION), CVAR_SERVERINFO | CVAR_INIT);
#ifdef LEGACY_PROTOCOL
	com_legacyprotocol = Cvar_Get("com_legacyprotocol", va("%i", PROTOCOL_LEGACY_VERSION), CVAR_INIT);

	// Keep for compatibility with old mods / mods that haven't updated yet.
	if(com_legacyprotocol->integer > 0)
		Cvar_Get("protocol", com_legacyprotocol->string, CVAR_ROM);
	else
#endif
		Cvar_Get("protocol", com_protocol->string, CVAR_ROM);

	Cvar_Get("//trap_GetValue", va("%i", COM_TRAP_GETVALUE), CVAR_PROTECTED | CVAR_ROM);

#ifndef DEDICATED
	con_autochat = Cvar_Get("con_autochat", "1", CVAR_ARCHIVE);
#endif

	Sys_Init();

	Sys_InitPIDFile( FS_GetCurrentGameDir() );

	// Pick a random port value
	Com_RandomBytes( (byte*)&qport, sizeof(int) );
	Netchan_Init( qport & 0xffff );

	VM_Init();
	SV_Init();

	com_dedicated->modified = qfalse;
#ifndef DEDICATED
	CL_Init();
#endif

	// set com_frameTime so that if a map is started on the
	// command line it will still be able to count on com_frameTime
	// being random enough for a serverid
	com_frameTime = Com_Milliseconds();

	// add + commands from command line
	if ( !Com_AddStartupCommands() ) {
		// if the user didn't give any commands, run default action
		if ( !com_dedicated->integer ) {
#ifdef CINEMATICS_LOGO
			Cbuf_AddText ("cinematic " CINEMATICS_LOGO "\n");
#endif
#ifdef CINEMATICS_INTRO
			if( !com_introPlayed->integer ) {
				Cvar_Set( com_introPlayed->name, "1" );
				Cvar_Set( "nextmap", "cinematic " CINEMATICS_INTRO );
			}
#endif
		}
	}

	// start in full screen ui mode
	Cvar_Set("r_uiFullScreen", "1");

	CL_StartHunkUsers( qfalse );

	// make sure single player is off by default
	Cvar_Set("ui_singlePlayerActive", "0");

	com_fullyInitialized = qtrue;

	// always set the cvar, but only print the info if it makes sense.
	Com_DetectAltivec();
#if idppc
	Com_Printf ("Altivec support is %s\n", com_altivec->integer ? "enabled" : "disabled");
#endif

	com_pipefile = Cvar_Get( "com_pipefile", "", CVAR_ARCHIVE|CVAR_LATCH|CVAR_PROTECTED );
	if( com_pipefile->string[0] )
	{
		pipefile = FS_FCreateOpenPipeFile( com_pipefile->string );
	}

	Com_Printf ("--- Common Initialization Complete ---\n");
}

/*
===============
Com_ReadFromPipe

Read whatever is in com_pipefile, if anything, and execute it
===============
*/
void Com_ReadFromPipe( void )
{
	static char buf[MAX_STRING_CHARS];
	static int accu = 0;
	int read;

	if( !pipefile )
		return;

	while( ( read = FS_Read( buf + accu, sizeof( buf ) - accu - 1, pipefile ) ) > 0 )
	{
		char *brk = NULL;
		int i;

		for( i = accu; i < accu + read; ++i )
		{
			if( buf[ i ] == '\0' )
				buf[ i ] = '\n';
			if( buf[ i ] == '\n' || buf[ i ] == '\r' )
				brk = &buf[ i + 1 ];
		}
		buf[ accu + read ] = '\0';

		accu += read;

		if( brk )
		{
			char tmp = *brk;
			*brk = '\0';
			Cbuf_ExecuteText( EXEC_APPEND, buf );
			*brk = tmp;

			accu -= brk - buf;
			memmove( buf, brk, accu + 1 );
		}
		else if( accu >= sizeof( buf ) - 1 ) // full
		{
			Cbuf_ExecuteText( EXEC_APPEND, buf );
			accu = 0;
		}
	}
}


//==================================================================

/*
===============
Com_ConfigAppend

Adds text to a config being built
===============
*/
void Com_ConfigAppend( configText_t *config, const char *text ) {
	int	length = strlen( text );

	if ( config->length + length >= config->size ) {
		int	size = MAX( 4096, 2 * ( config->length + length ) );
		char	*grown = Z_Malloc( size );

		if ( config->text ) {
			Com_Memcpy( grown, config->text, config->length );
			Z_Free( config->text );
		}
		config->text = grown;
		config->size = size;
	}
	Com_Memcpy( config->text + config->length, text, length + 1 );
	config->length += length;
}

/*
===============
Com_WriteConfigLines

A config's lines: its first line (COM_CONFIG_HEADER), which tells the
importer who wrote it, the binds if asked, the archived cvars of the scopes
asked for but those with any of hideFlags, and the version of the
engine's defaults it was written after
===============
*/
static void Com_WriteConfigLines( configText_t *config, qboolean binds, int hideFlags, int scopes ) {
	Com_ConfigAppend( config, COM_CONFIG_HEADER " do not modify\n" );
	if ( binds ) {
		Key_WriteBindings( config );
	}
	Cvar_WriteVariables( config, hideFlags, scopes );
	Com_ConfigAppend( config, "seta com_configVersion \"" COM_CONFIG_VERSION "\"\n" );
}

/*
===============
Com_WriteConfigText

A config's text to a file opened to write it; returns whether it was
written
===============
*/
static qboolean Com_WriteConfigText( fileHandle_t f, const char *path, const configText_t *config ) {
	int	written;

	if ( !f ) {
		Com_Printf( "Couldn't write %s.\n", path );
		return qfalse;
	}
	written = FS_Write( config->text, config->length, f );
	FS_FCloseFile( f );
	// a short write (a full disk) isn't what the file holds, so the next
	// write tries again
	if ( written < config->length ) {
		Com_Printf( "Couldn't write all of %s.\n", path );
		return qfalse;
	}
	return qtrue;
}

/*
===============
Com_WriteConfigFile

A config in the home's game directory, with the binds; returns whether it
was written
===============
*/
static qboolean Com_WriteConfigFile( const char *filename, int hideFlags, int scopes ) {
	configText_t	config = { NULL, 0, 0 };
	qboolean	written;

	// --check saves nothing, a config's writeconfig included
	if ( com_check ) {
		Com_Printf( "--check writes no file: not %s.\n", filename );
		return qfalse;
	}
	Com_WriteConfigLines( &config, qtrue, hideFlags, scopes );
	written = Com_WriteConfigText( FS_FOpenFileWrite_HomeConfig( filename ), filename, &config );
	Z_Free( config.text );
	if ( written ) {
		Com_ConfigWritten();
	}
	return written;
}

/*
===============
Com_WriteSettings

The files whose text changed: what each would hold now, compared with
what it held as read or last written, for the game directory whose
settings were read
===============
*/
static void Com_WriteSettings( void ) {
	qboolean	written = qfalse;
	int		count, i;
	settingsFile_t	*files = Com_SettingsFiles( &count );

	for ( i = 0; i < count; i++ ) {
		settingsFile_t	*file = &files[i];
		configText_t	config = { NULL, 0, 0 };
		char		path[MAX_QPATH];

		Com_SettingsPath( file, path, sizeof( path ) );
		Com_WriteConfigLines( &config, file->binds, 0, file->scopes );
		if ( ( file->text && !strcmp( file->text, config.text ) ) ||
			!Com_WriteConfigText( FS_BaseDir_FOpenFileWrite_HomeConfig( path ), path, &config ) ) {
			Z_Free( config.text );
			continue;
		}
		if ( file->text ) {
			Z_Free( file->text );
		}
		file->text = config.text;
		written = qtrue;
	}
	if ( written ) {
		Com_ConfigWritten();
	}
}

/*
===============
Com_WriteConfiguration

Writes key bindings and archived cvars to the files whose text changed, if
any was modified
===============
*/
void Com_WriteConfiguration( void ) {
	// if we are quiting without fully initializing, make sure
	// we don't write out anything; nor does --check, which only reports
	if ( !com_fullyInitialized || com_check ) {
		return;
	}

	if ( !(cvar_modifiedFlags & CVAR_ARCHIVE ) ) {
		return;
	}
	cvar_modifiedFlags &= ~CVAR_ARCHIVE;
	Com_WriteSettings();

	// not needed for dedicated or standalone
#if !defined(DEDICATED) && !defined(STANDALONE)
	if(!com_standalone->integer)
	{
		const char *gamedir;
		gamedir = Cvar_VariableString( "fs_game" );
		if (UI_usesUniqueCDKey() && gamedir[0] != 0) {
			Com_WriteCDKey( gamedir, &cl_cdkey[16] );
		} else {
			Com_WriteCDKey( BASEGAME, cl_cdkey );
		}
	}
#endif
}


/*
===============
Com_ConfigFileName

The config a command names as its argument, with ".cfg" if it has no
extension, or NULL with why: a config only, and where protect says, not
one the engine keeps or runs at startup, or a startup script ran
===============
*/
static const char *Com_ConfigFileName( qboolean protect ) {
	static char	filename[MAX_QPATH];

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: %s <filename>\n", Cmd_Argv( 0 ) );
		return NULL;
	}
	Q_strncpyz( filename, Cmd_Argv( 1 ), sizeof( filename ) );
	COM_DefaultExtension( filename, sizeof( filename ), ".cfg" );
	if ( !COM_CompareExtension( filename, ".cfg" ) ) {
		Com_Printf( "%s: only a \".cfg\" file\n", Cmd_Argv( 0 ) );
		return NULL;
	}
	if ( protect && ( FS_IsEngineFile( filename ) ||
		!Q_stricmp( FS_SkipPathPrefix( filename ), "default.cfg" ) || Cmd_IsStartupScript( filename ) ) ) {
		if ( Cmd_IsRestricted() ) {
			Com_Printf( "%s can't be written by game code or game content.\n", filename );
		} else {
			Com_Printf( "%s is a file the engine keeps or runs at startup, which %s doesn't replace.\n",
				filename, Cmd_Argv( 0 ) );
		}
		return NULL;
	}
	return filename;
}

/*
===============
Com_SettingsExport_f

settings_export <name>: the player's settings and binds, the ones that
travel, in one config to share in the home's game directory: never the
device's, nor a secret, nor a mod's own, which would land in another mod
===============
*/
static void Com_SettingsExport_f( void ) {
	const char	*filename;

	if ( Cmd_IsRestricted() ) {
		Com_Printf( "settings_export can't be run by game code or game content.\n" );
		return;
	}
	filename = Com_ConfigFileName( qtrue );
	if ( filename && Com_WriteConfigFile( filename, CVAR_PRIVATE, CVAR_SCOPE_BIT( CVAR_SCOPE_PLAYER ) ) ) {
		Com_Printf( "Exported the player's settings to %s.\n", filename );
	}
}

/*
===============
Com_SettingsImport_f

settings_import <name>: a config imported as the first start imports one,
its writer's defaults left out (Com_ConfigWriter), and saved: a shared one,
or one from another install
===============
*/
static void Com_SettingsImport_f( void ) {
	const char	*filename;
	char		*text;

	if ( Cmd_IsRestricted() ) {
		Com_Printf( "settings_import can't be run by game code or game content.\n" );
		return;
	}
	filename = Com_ConfigFileName( qfalse );
	if ( !filename ) {
		return;
	}
	if ( FS_ReadFile( filename, (void **)&text ) < 0 ) {
		Com_Printf( "Couldn't read %s.\n", filename );
		return;
	}
	if ( FS_LastFileIsGameContent() ) {
		Com_Printf( "%s is game content, which isn't imported.\n", filename );
	} else {
		Com_Printf( "Imported %s, written by %s.\n", filename, Com_ImportConfig( text, filename, qfalse, qfalse ) );
	}
	FS_FreeFile( text );
}

/*
===============
Com_WriteConfig_f

Write the config file to a specific name
===============
*/
void Com_WriteConfig_f( void ) {
	// nor can anyone else, rcon included: a config run at startup runs
	// with full rights, and the engine's own files are read as data
	const char	*filename = Com_ConfigFileName( qtrue );

	if ( !filename ) {
		return;
	}

	// private cvars, such as passwords, go only in the engine's own config,
	// which writeconfig doesn't replace
	Com_Printf( "Writing %s.\n", filename );
	Com_WriteConfigFile( filename, CVAR_PRIVATE, CVAR_SCOPES_SAVED );
}

/*
================
Com_ModifyMsec
================
*/
int Com_ModifyMsec( int msec ) {
	int		clampTime;

	//
	// modify time for debugging values
	//
	if ( com_fixedtime->integer ) {
		msec = com_fixedtime->integer;
	} else if ( com_timescale->value ) {
		msec *= com_timescale->value;
	} else if (com_cameraMode->integer) {
		msec *= com_timescale->value;
	}
	
	// don't let it scale below 1 msec
	if ( msec < 1 && com_timescale->value) {
		msec = 1;
	}

	if ( com_dedicated->integer ) {
		// dedicated servers don't want to clamp for a much longer
		// period, because it would mess up all the client's views
		// of time.
		if (com_sv_running->integer && msec > 500)
			Com_Printf( "Hitch warning: %i msec frame time\n", msec );

		clampTime = 5000;
	} else 
	if ( !com_sv_running->integer ) {
		// clients of remote servers do not want to clamp time, because
		// it would skew their view of the server's time temporarily
		clampTime = 5000;
	} else {
		// for local single player gaming
		// we may want to clamp the time to prevent players from
		// flying off edges when something hitches.
		clampTime = 200;
	}

	if ( msec > clampTime ) {
		msec = clampTime;
	}

	return msec;
}

/*
=================
Com_TimeVal
=================
*/

int Com_TimeVal(int minMsec)
{
	int timeVal;

	timeVal = Sys_Milliseconds() - com_frameTime;

	if(timeVal >= minMsec)
		timeVal = 0;
	else
		timeVal = minMsec - timeVal;

	return timeVal;
}

static int	com_lastFrameTime = 0;
static int64_t	com_frameStart;			// when the last frame started, in Sys_Nanoseconds
/*
=================
Com_FrameMinMsec

How long the next frame of a server, or of a timedemo, takes at least
=================
*/
static int Com_FrameMinMsec( void ) {
	if(com_timedemo->integer)
		return 1;

	return SV_FrameMsec();
}

#ifndef DEDICATED
static int64_t	com_frameLastDue;	// when the last client frame was due, in Sys_Nanoseconds
static qboolean	com_frameWaited;	// whether the wait for the next slept (com_speeds' late)

/*
=================
Com_PacedFrames

Whether the client paces its frames to com_maxfps, to the nanosecond; a
server's and a timedemo's go by the millisecond
=================
*/
static qboolean Com_PacedFrames( void ) {
	return !com_dedicated->integer && !com_timedemo->integer;
}

/*
=================
Com_FrameInterval

How long a client frame takes at least, in nanoseconds: at most 1000
frames a second, since game time is in whole milliseconds
=================
*/
static int64_t Com_FrameInterval( void ) {
	int64_t	interval = 1000000;
	int64_t	cap, refresh, display;

	// the display's refresh interval, read only where a cap follows it or
	// vsync waits for it
	display = com_maxfps->integer < 0 || com_swapIntervalActive->integer ? Sys_RefreshInterval() : 0;
	cap = Com_MaxFpsInterval(com_maxfps->integer, display, CL_MovesByFrame());
	if(cap > 0)
	{
#ifdef __EMSCRIPTEN__
		refresh = 0;	// the cap is rounded to whole refreshes below
#else
		// a swap interval of n shows a frame every n refreshes (-1 is
		// adaptive vsync, every refresh)
		refresh = display * abs(com_swapIntervalActive->integer);
#endif

		// With vsync, the swap waits for the display: a cap at or above its
		// refresh rate would only drop frames it could show, so the display
		// paces them. One below it paces them as without vsync, for a
		// display of variable refresh rate, say, or to save power. Where
		// the refresh rate isn't known, the cap is kept.
		if(!refresh || cap > refresh + refresh / 1000)
			interval = MAX(interval, cap);
		// A driver's settings can turn vsync off unseen: then frames are
		// kept to a tenth faster than the display, which a swap that waits
		// never is.
		else
			interval = MAX(interval, refresh - refresh / 10);
	}

	// in the background, slower, but never faster than com_maxfps
	if(com_minimized->integer && com_maxfpsMinimized->integer > 0)
		interval = MAX(interval, Com_CapInterval(com_maxfpsMinimized->integer));
	else if(com_unfocused->integer && com_maxfpsUnfocused->integer > 0)
		interval = MAX(interval, Com_CapInterval(com_maxfpsUnfocused->integer));

	// a listen server's frames are the client's: no cap, the player's or the
	// background's, may keep them further apart than the server's, or its
	// players miss snapshots while its host looks away
	if(com_sv_running->integer)
		interval = MIN(interval, SV_FrameLength() * (int64_t)1000000);

#ifdef __EMSCRIPTEN__
	// A browser runs frames on the display's refreshes, and always waits
	// for them: a cap is the nearest whole number of refreshes, since one
	// between two would take one frame on the first and the next on the
	// second, unevenly
	refresh = Sys_RefreshInterval();
	if(refresh > 0)
	{
		int64_t	server = SV_FrameLength() * (int64_t)1000000;

		interval = MAX(1, (interval + refresh / 2) / refresh) * refresh;
		// rounded up past a listen server's frame, it's rounded down
		if(com_sv_running->integer && interval > server)
			interval = MAX(1, server / refresh) * refresh;
	}
#endif

	return interval;
}

/*
=================
Com_NextFrameDue

When the next client frame is due: an interval after the last one was due,
the interval as it is now, so that a changed cap, or the game coming back
from the background, takes effect at once, not after a frame at the old
rate. Counted from when the last frame was due, not when it started, a rate
that isn't a whole number of milliseconds a frame keeps its average, and a
frame that started late is made up by the next.
=================
*/
static int64_t Com_NextFrameDue( void ) {
	return com_frameLastDue + Com_FrameInterval();
}

/*
=================
Com_ClientFrameStarted

As a client frame starts, it's the last one due, and com_speeds' late is how
long after it was due it started, if the wait for it slept. A frame that
started more than an interval late starts the count again, rather than run
frames back to back: on a whole millisecond of the clock game time counts,
so frames of whole milliseconds each move game time by exactly that much.
=================
*/
static void Com_ClientFrameStarted( int64_t frameStart ) {
	int64_t	interval = Com_FrameInterval();
	int64_t	due = com_frameLastDue + interval;

	// one that started before it was due, as a wait cut short by an error
	// does, is on time: -1 is com_speeds' none
	if(com_frameWaited)
	{
		time_late = (int)(MAX(frameStart - due, 0) / 1000);
		com_frameWaited = qfalse;
	}

#ifdef __EMSCRIPTEN__
	// A browser runs frames on the display's refreshes: counted from this
	// frame's start, on one, the next is due on the refresh nearest its
	// time, and an interval measured a little short can't creep earlier.
	com_frameLastDue = frameStart - Sys_RefreshInterval() / 2;
#else
	if(frameStart - due > interval)
		due = frameStart - frameStart % 1000000;

	com_frameLastDue = due;
#endif
}

/*
=================
Com_WaitClientFrame

Com_WaitFrame for the client. The network is read here, and a listen
server's queued packets go out, on every call, due or not. While the
frame is more than 3 milliseconds away, it sleeps once and returns, for
the main loop to take input in between; then it waits precisely until
the frame is due. A queued packet's time is kept to the millisecond.
=================
*/
static qboolean Com_WaitClientFrame( void ) {
#ifdef __EMSCRIPTEN__
	// the page can't sleep: the main loop skips a refresh that comes early
	NET_Sleep(0);
	if(com_sv_running->integer)
		SV_SendQueuedPackets();

	return Sys_Nanoseconds() >= Com_NextFrameDue();
#else
	int64_t	left, wait;

	for(;;)
	{
		left = Com_NextFrameDue() - Sys_Nanoseconds();
		wait = left;
		if(com_sv_running->integer)
			wait = MIN(wait, SV_SendQueuedPackets() * (int64_t)1000000);

		if(left <= 0)
		{
			NET_Sleep(0);
			return qtrue;
		}

		// a millisecond sleep can overshoot, so one ends 2 ms before the
		// frame at the latest, whenever a queued packet is due
		com_frameWaited = qtrue;
		if(com_busyWait->integer || wait < 1000000)
			NET_Sleep(0);
		else if(left > 3000000)
			NET_Sleep(MIN(wait, left - 2000000) / 1000000);
		else if(wait < left)
			NET_Sleep(wait / 1000000);
		else
			Sys_SleepPrecise(left);

		if(left > 3000000)
			return Sys_Nanoseconds() >= Com_NextFrameDue();
	}
#endif
}
#endif

/*
=================
Com_FrameDue

Whether the next frame is due, without waiting for it
=================
*/
qboolean Com_FrameDue( void ) {
#ifndef DEDICATED
	if(Com_PacedFrames())
		return Sys_Nanoseconds() >= Com_NextFrameDue();
#endif

	return !Com_TimeVal(Com_FrameMinMsec());
}

/*
=================
Com_WaitFrame

Sleeps toward the next frame, and returns whether it's due. The main loop
calls it until it is, then Com_Frame, and can handle input in between. The
last millisecond or so passes here.

Packets that arrive meanwhile are handled here (NET_Sleep), outside
Com_Frame, so an ERR_DROP they raise needs abortframe set here too: the last
Com_Frame's has returned, and a longjmp to it unwinds into a frame that's
gone (on Windows, STATUS_BAD_STACK).
=================
*/
qboolean Com_WaitFrame( void ) {
	int		minMsec, timeVal, timeValSV;

	if ( setjmp (abortframe) ) {
		Cmd_EndRestricted();
		return qtrue;	// an ERR_DROP was thrown; the next frame shows it
	}

#ifndef DEDICATED
	if(Com_PacedFrames())
		return Com_WaitClientFrame();
#endif

	minMsec = Com_FrameMinMsec();

	do
	{
		if(com_sv_running->integer)
		{
			timeValSV = SV_SendQueuedPackets();

			timeVal = Com_TimeVal(minMsec);

			if(timeValSV < timeVal)
				timeVal = timeValSV;
		}
		else
			timeVal = Com_TimeVal(minMsec);

		if(com_busyWait->integer || timeVal < 1)
			NET_Sleep(0);
		else
			NET_Sleep(timeVal - 1);
	} while(timeVal < 2 && Com_TimeVal(minMsec));

	return !Com_TimeVal(minMsec);
}

/*
=================
Com_Frame
=================
*/
void Com_Frame( void ) {

	int		msec;
 
	int		timeBeforeFirstEvents;
	int		timeBeforeServer;
	int		timeBeforeEvents;
	int		timeBeforeClient;
	int		timeAfter;
	int64_t	frameStart = Sys_Nanoseconds( );
	int64_t	sinceLastFrame = frameStart - com_frameStart;

	com_frameStart = frameStart;
  

	if ( setjmp (abortframe) ) {
		Cmd_EndRestricted();
		return;			// an ERR_DROP was thrown
	}

	timeBeforeFirstEvents =0;
	timeBeforeServer =0;
	timeBeforeEvents =0;
	timeBeforeClient = 0;
	timeAfter = 0;

	// how long after it was due this frame started, if the wait for it
	// (Com_WaitClientFrame) slept: a frame that was due already, behind a
	// swap that waited for the display, say, has none, and nor has a
	// browser's, which runs frames on its refreshes. Taken from the frame's
	// own start, so a wait cut short by an error still gives its frame one
	time_late = -1;
#ifndef DEDICATED
	Com_ClientFrameStarted( frameStart );
#endif
	time_inputAge = -1;
	time_swap = -1;

	// write config file if anything changed
	Com_WriteConfiguration(); 

	//
	// main event loop
	//
	if ( com_speeds->integer ) {
		timeBeforeFirstEvents = Sys_Milliseconds ();
	}

	
	IN_Frame();

	com_lastFrameTime = com_frameTime;
	com_frameTime = Com_EventLoop();
	
	msec = com_frameTime - com_lastFrameTime;

	Cbuf_Execute ();

	if (com_altivec->modified)
	{
		Com_DetectAltivec();
		com_altivec->modified = qfalse;
	}

	// mess with msec if needed
	msec = Com_ModifyMsec(msec);

	//
	// server side
	//
	if ( com_speeds->integer ) {
		timeBeforeServer = Sys_Milliseconds ();
	}

	SV_Frame( msec );

	// if "dedicated" has been modified, start up
	// or shut down the client system.
	// Do this after the server may have started,
	// but before the client tries to auto-connect
	if ( com_dedicated->modified ) {
		// get the latched value
		Cvar_Get( "dedicated", "0", 0 );
		com_dedicated->modified = qfalse;
		if ( !com_dedicated->integer ) {
			SV_Shutdown( "dedicated set to 0" );
			CL_FlushMemory();
		}
	}

#ifndef DEDICATED
	//
	// client system
	//
	//
	// run event loop a second time to get server to client packets
	// without a frame of latency
	//
	if ( com_speeds->integer ) {
		timeBeforeEvents = Sys_Milliseconds ();
	}
	Com_EventLoop();
	Cbuf_Execute ();


	//
	// client side
	//
	if ( com_speeds->integer ) {
		timeBeforeClient = Sys_Milliseconds ();
	}

	CL_Frame( msec );

	if ( com_speeds->integer ) {
		timeAfter = Sys_Milliseconds ();
	}
#else
	if ( com_speeds->integer ) {
		timeAfter = Sys_Milliseconds ();
		timeBeforeEvents = timeAfter;
		timeBeforeClient = timeAfter;
	}
#endif


	NET_FlushPacketQueue();

	//
	// report timing information
	//
	if ( com_speeds->integer ) {
		int			all, sv, ev, cl;

		all = timeAfter - timeBeforeServer;
		sv = timeBeforeEvents - timeBeforeServer;
		ev = timeBeforeServer - timeBeforeFirstEvents + timeBeforeClient - timeBeforeEvents;
		cl = timeAfter - timeBeforeClient;
		sv -= time_game;
		cl -= time_frontend + time_backend;

		// and its pacing, in microseconds (-1 for none): how late it
		// started, the newest input's age as the usercmd was built, the
		// time since the last frame's swap; then its game time, and the
		// time since the last frame started, the cap to cap
		Com_Printf ("frame:%i all:%3i sv:%3i ev:%3i cl:%3i gm:%3i rf:%3i bk:%3i late:%i in:%i sw:%i ms:%i dt:%i\n",
					 com_frameNumber, all, sv, ev, cl, time_game, time_frontend, time_backend,
					 time_late, time_inputAge, time_swap, msec, (int)( sinceLastFrame / 1000 ) );
	}	

	//
	// trace optimization tracking
	//
	if ( com_showtrace->integer ) {
	
		extern	int c_traces, c_brush_traces, c_patch_traces;
		extern	int	c_pointcontents;

		Com_Printf ("%4i traces  (%ib %ip) %4i points\n", c_traces,
			c_brush_traces, c_patch_traces, c_pointcontents);
		c_traces = 0;
		c_brush_traces = 0;
		c_patch_traces = 0;
		c_pointcontents = 0;
	}

	Com_ReadFromPipe( );

	com_frameNumber++;
}

/*
=================
Com_Shutdown
=================
*/
void Com_Shutdown (void) {
	if (logfile) {
		FS_FCloseFile (logfile);
		logfile = 0;
	}

	if ( com_journalFile ) {
		FS_FCloseFile( com_journalFile );
		com_journalFile = 0;
	}

	if( pipefile ) {
		FS_FCloseFile( pipefile );
		FS_Remove_HomeData( com_pipefile->string );
	}

}

/*
===========================================
command line completion
===========================================
*/

/*
==================
Field_Clear
==================
*/
void Field_Clear( field_t *edit ) {
  memset(edit->buffer, 0, MAX_EDIT_LINE);
	edit->cursor = 0;
	edit->scroll = 0;
}

static const char *completionString;
static char shortestMatch[MAX_TOKEN_CHARS];
static int	matchCount;
// field we are working on, passed to Field_AutoComplete(&g_consoleCommand for instance)
static field_t *completionField;

/*
===============
FindMatches

===============
*/
static void FindMatches( const char *s ) {
	int		i;

	if ( Q_stricmpn( s, completionString, strlen( completionString ) ) ) {
		return;
	}
	matchCount++;
	if ( matchCount == 1 ) {
		Q_strncpyz( shortestMatch, s, sizeof( shortestMatch ) );
		return;
	}

	// cut shortestMatch to the amount common with s
	for ( i = 0 ; shortestMatch[i] ; i++ ) {
		if ( i >= strlen( s ) ) {
			shortestMatch[i] = 0;
			break;
		}

		if ( tolower(shortestMatch[i]) != tolower(s[i]) ) {
			shortestMatch[i] = 0;
		}
	}
}

/*
===============
PrintMatches

===============
*/
static void PrintMatches( const char *s ) {
	if ( !Q_stricmpn( s, shortestMatch, strlen( shortestMatch ) ) ) {
		Com_Printf( "    %s\n", s );
	}
}

/*
===============
PrintCvarMatches

===============
*/
static void PrintCvarMatches( const char *s ) {
	char value[ TRUNCATE_LENGTH ];

	if ( !Q_stricmpn( s, shortestMatch, strlen( shortestMatch ) ) ) {
		Com_TruncateLongString( value, Cvar_VariableString( s ) );
		Com_Printf( "    %s = \"%s\"\n", s, value );
	}
}

/*
===============
Field_FindFirstSeparator
===============
*/
static char *Field_FindFirstSeparator( char *s )
{
	int i;

	for( i = 0; i < strlen( s ); i++ )
	{
		if( s[ i ] == ';' )
			return &s[ i ];
	}

	return NULL;
}

/*
===============
Field_Complete
===============
*/
static qboolean Field_Complete( void )
{
	int completionOffset;

	if( matchCount == 0 )
		return qtrue;

	completionOffset = strlen( completionField->buffer ) - strlen( completionString );

	Q_strncpyz( &completionField->buffer[ completionOffset ], shortestMatch,
		sizeof( completionField->buffer ) - completionOffset );

	completionField->cursor = strlen( completionField->buffer );

	if( matchCount == 1 )
	{
		Q_strcat( completionField->buffer, sizeof( completionField->buffer ), " " );
		completionField->cursor++;
		return qtrue;
	}

	Com_Printf( "]%s\n", completionField->buffer );

	return qfalse;
}

#ifndef DEDICATED
/*
===============
Field_CompleteKeyname
===============
*/
void Field_CompleteKeyname( void )
{
	matchCount = 0;
	shortestMatch[ 0 ] = 0;

	Key_KeynameCompletion( FindMatches );

	if( !Field_Complete( ) )
		Key_KeynameCompletion( PrintMatches );
}
#endif

/*
===============
Field_CompleteFilename
===============
*/
void Field_CompleteFilename( const char *dir, const char *ext,
		char *filter, qboolean stripExt,
		qboolean allowNonPureFilesOnDisk )
{
	matchCount = 0;
	shortestMatch[ 0 ] = 0;

	FS_FilenameCompletion( dir, ext, filter, stripExt, FindMatches, allowNonPureFilesOnDisk );

	if( !Field_Complete( ) )
		FS_FilenameCompletion( dir, ext, filter, stripExt, PrintMatches, allowNonPureFilesOnDisk );
}

/*
===============
Field_CompleteCommand
===============
*/
void Field_CompleteCommand( char *cmd,
		qboolean doCommands, qboolean doCvars )
{
	int		completionArgument = 0;

	// Skip leading whitespace and quotes
	cmd = Com_SkipCharset( cmd, " \"" );

	Cmd_TokenizeStringIgnoreQuotes( cmd );
	completionArgument = Cmd_Argc( );

	// If there is trailing whitespace on the cmd
	if( *( cmd + strlen( cmd ) - 1 ) == ' ' )
	{
		completionString = "";
		completionArgument++;
	}
	else
		completionString = Cmd_Argv( completionArgument - 1 );

#ifndef DEDICATED
	// add a '\' to the start of the buffer if it might be sent as chat otherwise
	if( con_autochat->integer && completionField->buffer[ 0 ] &&
			completionField->buffer[ 0 ] != '\\' )
	{
		if( completionField->buffer[ 0 ] != '/' )
		{
			// Buffer is full, refuse to complete
			if( strlen( completionField->buffer ) + 1 >=
				sizeof( completionField->buffer ) )
				return;

			memmove( &completionField->buffer[ 1 ],
				&completionField->buffer[ 0 ],
				strlen( completionField->buffer ) + 1 );
			completionField->cursor++;
		}

		completionField->buffer[ 0 ] = '\\';
	}
#endif

	if( completionArgument > 1 )
	{
		const char *baseCmd = Cmd_Argv( 0 );
		char *p;

#ifndef DEDICATED
		// This should always be true
		if( baseCmd[ 0 ] == '\\' || baseCmd[ 0 ] == '/' )
			baseCmd++;
#endif

		if( ( p = Field_FindFirstSeparator( cmd ) ) )
			Field_CompleteCommand( p + 1, qtrue, qtrue ); // Compound command
		else
			Cmd_CompleteArgument( baseCmd, cmd, completionArgument ); 
	}
	else
	{
		if( completionString[0] == '\\' || completionString[0] == '/' )
			completionString++;

		matchCount = 0;
		shortestMatch[ 0 ] = 0;

		if( strlen( completionString ) == 0 )
			return;

		if( doCommands )
			Cmd_CommandCompletion( FindMatches );

		if( doCvars )
			Cvar_CommandCompletion( FindMatches );

		if( !Field_Complete( ) )
		{
			// run through again, printing matches
			if( doCommands )
				Cmd_CommandCompletion( PrintMatches );

			if( doCvars )
				Cvar_CommandCompletion( PrintCvarMatches );
		}
	}
}

/*
===============
Field_AutoComplete

Perform Tab expansion
===============
*/
void Field_AutoComplete( field_t *field )
{
	completionField = field;

	Field_CompleteCommand( completionField->buffer, qtrue, qtrue );
}

/*
==================
Com_RandomBytes

fills string array with len random bytes, preferably from the OS randomizer
==================
*/
void Com_RandomBytes( byte *string, int len )
{
	int i;

	if( Sys_RandomBytes( string, len ) )
		return;

	Com_Printf( "Com_RandomBytes: using weak randomization\n" );
	for( i = 0; i < len; i++ )
		string[i] = (unsigned char)( rand() % 256 );
}


/*
==================
Com_IsVoipTarget

Returns non-zero if given clientNum is enabled in voipTargets, zero otherwise.
If clientNum is negative return if any bit is set.
==================
*/
qboolean Com_IsVoipTarget(uint8_t *voipTargets, int voipTargetsSize, int clientNum)
{
	int index;
	if(clientNum < 0)
	{
		for(index = 0; index < voipTargetsSize; index++)
		{
			if(voipTargets[index])
				return qtrue;
		}
		
		return qfalse;
	}

	index = clientNum >> 3;
	
	if(index < voipTargetsSize)
		return (voipTargets[index] & (1 << (clientNum & 0x07)));

	return qfalse;
}

/*
===============
Field_CompletePlayerName
===============
*/
static qboolean Field_CompletePlayerNameFinal( qboolean whitespace )
{
	int completionOffset;

	if( matchCount == 0 )
		return qtrue;

	completionOffset = strlen( completionField->buffer ) - strlen( completionString );

	Q_strncpyz( &completionField->buffer[ completionOffset ], shortestMatch,
		sizeof( completionField->buffer ) - completionOffset );

	completionField->cursor = strlen( completionField->buffer );

	if( matchCount == 1 && whitespace )
	{
		Q_strcat( completionField->buffer, sizeof( completionField->buffer ), " " );
		completionField->cursor++;
		return qtrue;
	}

	return qfalse;
}

static void Name_PlayerNameCompletion( const char **names, int nameCount, void(*callback)(const char *s) ) 
{
	int i;

	for( i = 0; i < nameCount; i++ ) {
		callback( names[ i ] );
	}
}

qboolean Com_FieldStringToPlayerName( char *name, int length, const char *rawname )
{
	char		hex[5];
	int			i;
	int			ch;

	if( name == NULL || rawname == NULL )
		return qfalse;

	if( length <= 0 )
		return qtrue;

	for( i = 0; *rawname && i + 1 <= length; rawname++, i++ ) {
		if( *rawname == '\\' ) {
			Q_strncpyz( hex, rawname + 1, sizeof(hex) );
			ch = Com_HexStrToInt( hex );
			if( ch > -1 ) {
				name[i] = ch;
				rawname += 4; //hex string length, 0xXX
			} else {
				name[i] = *rawname;
			}
		} else {
			name[i] = *rawname;
		}
	}
	name[i] = '\0';

	return qtrue;
}

qboolean Com_PlayerNameToFieldString( char *str, int length, const char *name )
{
	const char *p;
	int i;
	int x1, x2;

	if( str == NULL || name == NULL )
		return qfalse;

	if( length <= 0 )
		return qtrue;

	*str = '\0';
	p = name;

	for( i = 0; *p != '\0'; i++, p++ )
	{
		if( i + 1 >= length )
			break;

		if( *p <= ' ' )
		{
			if( i + 5 + 1 >= length )
				break;

			x1 = *p >> 4;
			x2 = *p & 15;

			str[i+0] = '\\';
			str[i+1] = '0';
			str[i+2] = 'x';
			str[i+3] = x1 > 9 ? x1 - 10 + 'a' : x1 + '0';
			str[i+4] = x2 > 9 ? x2 - 10 + 'a' : x2 + '0';

			i += 4;
		} else {
			str[i] = *p;
		}		
	}
	str[i] = '\0';

	return qtrue;
}

void Field_CompletePlayerName( const char **names, int nameCount )
{
	qboolean whitespace;

	matchCount = 0;
	shortestMatch[ 0 ] = 0;

	if( nameCount <= 0 )
		return;

	Name_PlayerNameCompletion( names, nameCount, FindMatches );

	if( completionString[0] == '\0' )
	{
		Com_PlayerNameToFieldString( shortestMatch, sizeof( shortestMatch ), names[ 0 ] );
	}

	//allow to tab player names
	//if full player name switch to next player name
	if( completionString[0] != '\0'
		&& Q_stricmp( shortestMatch, completionString ) == 0 
		&& nameCount > 1 ) 
	{
		int i;

		for( i = 0; i < nameCount; i++ ) {
			if( Q_stricmp( names[ i ], completionString ) == 0 ) 
			{
				i++;
				if( i >= nameCount )
				{
					i = 0;
				}

				Com_PlayerNameToFieldString( shortestMatch, sizeof( shortestMatch ), names[ i ] );
				break;
			}
		}
	}

	if( matchCount > 1 )
	{
		Com_Printf( "]%s\n", completionField->buffer );
		
		Name_PlayerNameCompletion( names, nameCount, PrintMatches );
	}

	whitespace = nameCount == 1? qtrue: qfalse;
	if( !Field_CompletePlayerNameFinal( whitespace ) )
	{

	}
}

int QDECL Com_strCompare( const void *a, const void *b )
{
    const char **pa = (const char **)a;
    const char **pb = (const char **)b;
    return strcmp( *pa, *pb );
}
