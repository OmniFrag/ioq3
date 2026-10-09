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

#include <signal.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifndef DEDICATED
// SDL runs the main loop (SDL_AppIterate), so the game keeps running while
// Windows moves or resizes its window, or shows the window menu
#define SDL_MAIN_USE_CALLBACKS
#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL3/SDL.h"
#	include "SDL3/SDL_main.h"
#	include "SDL3/SDL_cpuinfo.h"
#else
#	include <SDL3/SDL.h>
#	include <SDL3/SDL_main.h>
#	include <SDL3/SDL_cpuinfo.h>
#endif
#endif

#include "sys_local.h"
#include "sys_loadlib.h"

#if !defined(DEDICATED) && defined(SDL_PLATFORM_WINDOWS)
#include <windows.h>
#endif

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#ifndef APP_ID // the build sets it (cmake/identity.cmake)
#define APP_ID "org.ioquake3.ioquake3"
#endif

static char binaryPath[ MAX_OSPATH ] = { 0 };
static char installPath[ MAX_OSPATH ] = { 0 };

/*
=================
Sys_TouchDevice

Whether the main pointer is a finger, as on a phone or tablet: only a
browser tells it apart
=================
*/
qboolean Sys_TouchDevice( void )
{
#ifdef __EMSCRIPTEN__
	return MAIN_THREAD_EM_ASM_INT( { return matchMedia( '(pointer: coarse)' ).matches; } ) != 0;
#else
	return qfalse;
#endif
}

/*
=================
Sys_SetBinaryPath
=================
*/
void Sys_SetBinaryPath(const char *path)
{
	Q_strncpyz(binaryPath, path, sizeof(binaryPath));
}

/*
=================
Sys_BinaryPath
=================
*/
char *Sys_BinaryPath(void)
{
	return binaryPath;
}

/*
=================
Sys_SetDefaultInstallPath
=================
*/
void Sys_SetDefaultInstallPath(const char *path)
{
	Q_strncpyz(installPath, path, sizeof(installPath));
}

/*
=================
Sys_DefaultInstallPath
=================
*/
char *Sys_DefaultInstallPath(void)
{
	if (*installPath)
		return installPath;
	else
		return Sys_Cwd();
}

/*
=================
Sys_DefaultAppPath
=================
*/
char *Sys_DefaultAppPath(void)
{
	return Sys_BinaryPath();
}

/*
=================
Sys_In_Restart_f

Restart the input subsystem
=================
*/
void Sys_In_Restart_f( void )
{
#ifndef DEDICATED
	if( !SDL_WasInit( SDL_INIT_VIDEO ) )
	{
		Com_Printf( "in_restart: Cannot restart input while video is shutdown\n" );
		return;
	}
#endif

	IN_Restart( );
}

/*
=================
Sys_ConsoleInput

Handle new console input
=================
*/
char *Sys_ConsoleInput(void)
{
	return CON_Input( );
}

/*
==================
Sys_GetClipboardData
==================
*/
char *Sys_GetClipboardData(void)
{
#ifdef DEDICATED
	return NULL;
#else
	char *data = NULL;
	char *cliptext;

	if ( ( cliptext = SDL_GetClipboardText() ) != NULL ) {
		if ( cliptext[0] != '\0' ) {
			size_t bufsize = strlen( cliptext ) + 1;

			data = Z_Malloc( bufsize );
			Q_strncpyz( data, cliptext, bufsize );

			// find first listed char and set to '\0'
			strtok( data, "\n\r\b" );
		}
		SDL_free( cliptext );
	}
	return data;
#endif
}

#ifdef DEDICATED
#	define PID_FILENAME PRODUCT_NAME "_server.pid"
#else
#	define PID_FILENAME PRODUCT_NAME ".pid"
#endif

/*
=================
Sys_PIDFileName
=================
*/
static char *Sys_PIDFileName( const char *gamedir )
{
	const char *homeStatePath = Cvar_VariableString( "fs_homestatepath" );

	if( *homeStatePath != '\0' )
		return va( "%s/%s/%s", homeStatePath, gamedir, PID_FILENAME );

	return NULL;
}

// the PID file this process wrote, "" for none
static char pidFileWritten[ MAX_OSPATH ];

/*
=================
Sys_RemovePIDFile

Removes the PID file this process wrote, if it wrote one
=================
*/
void Sys_RemovePIDFile( void )
{
	if( pidFileWritten[ 0 ] )
	{
		remove( pidFileWritten );
		pidFileWritten[ 0 ] = '\0';
	}
}

/*
=================
Sys_WritePIDFile

Return qtrue if there is an existing stale PID file
=================
*/
static qboolean Sys_WritePIDFile( const char *gamedir )
{
	char      *pidFile = Sys_PIDFileName( gamedir );
	FILE      *f;
	qboolean  stale = qfalse;

	if( pidFile == NULL )
		return qfalse;

	// First, check if the pid file is already there
	if( ( f = fopen( pidFile, "r" ) ) != NULL )
	{
		char  pidBuffer[ 64 ] = { 0 };
		int   pid;

		pid = fread( pidBuffer, sizeof( char ), sizeof( pidBuffer ) - 1, f );
		fclose( f );

		if(pid > 0)
		{
			pid = atoi( pidBuffer );
			if( !Sys_PIDIsRunning( pid ) )
				stale = qtrue;
		}
		else
			stale = qtrue;
	}

	if( FS_CreatePath( pidFile ) ) {
		return 0;
	}

	if( ( f = fopen( pidFile, "w" ) ) != NULL )
	{
		fprintf( f, "%d", Sys_PID( ) );
		fclose( f );
		Q_strncpyz( pidFileWritten, pidFile, sizeof( pidFileWritten ) );
	}
	else
		Com_Printf( S_COLOR_YELLOW "Couldn't write %s.\n", pidFile );

	return stale;
}

/*
=================
Sys_InitPIDFile
=================
*/
void Sys_InitPIDFile( const char *gamedir ) {
	if( Sys_WritePIDFile( gamedir ) ) {
#ifndef DEDICATED
		char message[1024];
		char modName[MAX_OSPATH];

		FS_GetModDescription( gamedir, modName, sizeof ( modName ) );
		Q_CleanStr( modName );

		Com_sprintf( message, sizeof (message), "The last time %s ran, "
			"it didn't exit properly. This may be due to inappropriate video "
			"settings. Would you like to start with \"safe\" video settings?", modName );

		if( Sys_Dialog( DT_YES_NO, message, "Abnormal Exit" ) == DR_YES ) {
			Cvar_Set( "com_abnormalExit", "1" );
		}
#endif
	}
}

/*
=================
Sys_Dialog

A dedicated server shows no dialogs: it may run under a service manager,
with nobody to answer them. It gets the answer of a dialog nobody saw: no,
or cancel
=================
*/
dialogResult_t Sys_Dialog( dialogType_t type, const char *message, const char *title )
{
	if( !Com_IsClient( ) )
	{
		if( type == DT_YES_NO )
			return DR_NO;
		if( type == DT_OK_CANCEL )
			return DR_CANCEL;
		return DR_OK;
	}

	return Sys_PlatformDialog( type, message, title );
}

/*
=================
Sys_OpenFolderInFileManager

Not on a dedicated server, which nobody may be watching
=================
*/
qboolean Sys_OpenFolderInFileManager( const char *path, qboolean create )
{
	if( !Com_IsClient( ) )
		return qfalse;

	if( create )
	{
		if( FS_CreatePath( path ) )
			return qfalse;
	}

	return Sys_OpenFolderInPlatformFileManager( path );
}

/*
=================
Sys_Exit

Single exit point (regular exit or in case of error)
=================
*/
static Q_NO_RETURN void Sys_Exit( int exitCode )
{
	CON_Shutdown( );

#ifndef DEDICATED
	SDL_Quit( );
#endif

	// A client's PID file left by a crash offers safe settings at its next
	// start (Sys_InitPIDFile); nothing reads a dedicated server's
	if( exitCode < 2 || !Com_IsClient( ) )
		Sys_RemovePIDFile( );

	NET_Shutdown( );

	Sys_PlatformExit( );

	exit( exitCode );
}

/*
=================
Sys_Quit
=================
*/
void Sys_Quit( void )
{
	Sys_Exit( 0 );
}

/*
=================
Sys_GetProcessorFeatures
=================
*/
cpuFeatures_t Sys_GetProcessorFeatures( void )
{
	cpuFeatures_t features = 0;

#ifndef DEDICATED
	if( SDL_HasMMX( ) )        features |= CF_MMX;
	if( SDL_HasSSE( ) )        features |= CF_SSE;
	if( SDL_HasSSE2( ) )       features |= CF_SSE2;
	if( SDL_HasAltiVec( ) )    features |= CF_ALTIVEC;
#endif

	return features;
}

/*
=================
Sys_Init
=================
*/
void Sys_Init(void)
{
	Cmd_AddCommand( "in_restart", Sys_In_Restart_f );
	Cvar_Get( "arch", OS_STRING " " ARCH_STRING, CVAR_ROM );
	Cvar_Get( "username", Sys_GetCurrentUser( ), CVAR_ROM | CVAR_PRIVATE );
}

/*
=================
Sys_AnsiColorPrint

Transform Q3 colour codes to ANSI escape sequences
=================
*/
void Sys_AnsiColorPrint( const char *msg )
{
	static char buffer[ MAXPRINTMSG ];
	int         length = 0;
	static int  q3ToAnsi[ 8 ] =
	{
		7, // COLOR_BLACK
		31, // COLOR_RED
		32, // COLOR_GREEN
		33, // COLOR_YELLOW
		34, // COLOR_BLUE
		36, // COLOR_CYAN
		35, // COLOR_MAGENTA
		0   // COLOR_WHITE
	};

	while( *msg )
	{
		if( Q_IsColorString( msg ) || *msg == '\n' )
		{
			// First empty the buffer
			if( length > 0 )
			{
				buffer[ length ] = '\0';
				fputs( buffer, stderr );
				length = 0;
			}

			if( *msg == '\n' )
			{
				// Issue a reset and then the newline
				fputs( "\033[0m\n", stderr );
				msg++;
			}
			else
			{
				// Print the color code (reset first to clear potential inverse (black))
				Com_sprintf( buffer, sizeof( buffer ), "\033[0m\033[%dm",
						q3ToAnsi[ ColorIndex( *( msg + 1 ) ) ] );
				fputs( buffer, stderr );
				msg += 2;
			}
		}
		else
		{
			if( length >= MAXPRINTMSG - 1 )
				break;

			buffer[ length ] = *msg;
			length++;
			msg++;
		}
	}

	// Empty anything still left in the buffer
	if( length > 0 )
	{
		buffer[ length ] = '\0';
		fputs( buffer, stderr );
	}
}

/*
=================
Sys_PrintText
=================
*/
static void Sys_PrintText( const char *text )
{
	CON_LogWrite( text );
	CON_Print( text );
}

/*
=================
Sys_Print

Each line starts with the date and time where com_timestamps asks for them
on the console (2)
=================
*/
void Sys_Print( const char *msg )
{
	static qboolean	lineStart = qtrue;

	Com_PrintLines( msg, &lineStart, 2, Sys_PrintText );
}

/*
=================
Sys_Error
=================
*/
void Sys_Error( const char *error, ... )
{
	va_list argptr;
	char    string[1024];

	va_start (argptr,error);
	Q_vsnprintf (string, sizeof(string), error, argptr);
	va_end (argptr);

	Sys_ErrorDialog( string );

	Sys_Exit( 3 );
}

/*
=================
Sys_ErrorExit

An error that isn't a crash, which Com_ErrorExit has printed: a client
shows it in a dialog, and there's no crash log, nor the PID file that
would offer safe settings at the next start
=================
*/
void Sys_ErrorExit( int status, const char *message )
{
	Sys_Dialog( DT_ERROR, message, "Error" );

	Sys_RemovePIDFile( );

	Sys_Exit( status );
}

#if 0
/*
=================
Sys_Warn
=================
*/
static Q_PRINTF_FUNC(1, 2) void Sys_Warn( char *warning, ... )
{
	va_list argptr;
	char    string[1024];

	va_start (argptr,warning);
	Q_vsnprintf (string, sizeof(string), warning, argptr);
	va_end (argptr);

	CON_Print( va( "Warning: %s", string ) );
}
#endif

/*
============
Sys_FileTime

returns -1 if not present
============
*/
int Sys_FileTime( char *path )
{
	struct stat buf;

	if (stat (path,&buf) == -1)
		return -1;

	return buf.st_mtime;
}

/*
=================
Sys_UnloadDll
=================
*/
void Sys_UnloadDll( void *dllHandle )
{
	if( !dllHandle )
	{
		Com_Printf("Sys_UnloadDll(NULL)\n");
		return;
	}

	Sys_UnloadLibrary(dllHandle);
}

/*
=================
Sys_LoadDll

First try to load library name from system library path,
from executable path, then fs_basepath.
=================
*/

void *Sys_LoadDll(const char *name, qboolean useSystemLib)
{
	void *dllhandle = NULL;

	if(!Sys_DllExtension(name))
	{
		Com_Printf("Refusing to attempt to load library \"%s\": Extension not allowed.\n", name);
		return NULL;
	}

	if(useSystemLib)
	{
		Com_Printf("Trying to load \"%s\"...\n", name);
		dllhandle = Sys_LoadLibrary(name);
	}
	
	if(!dllhandle)
	{
		const char *topDir;
		char libPath[MAX_OSPATH];
		int len;

		topDir = Sys_BinaryPath();

		if(!*topDir)
			topDir = ".";

		len = Com_sprintf(libPath, sizeof(libPath), "%s%c%s", topDir, PATH_SEP, name);
		if(len < sizeof(libPath))
		{
			Com_Printf("Trying to load \"%s\" from \"%s\"...\n", name, topDir);
			dllhandle = Sys_LoadLibrary(libPath);
		}
		else
		{
			Com_Printf("Skipping trying to load \"%s\" from \"%s\", file name is too long.\n", name, topDir);
		}

		if(!dllhandle)
		{
			const char *basePath = Cvar_VariableString("fs_basepath");
			
			if(!basePath || !*basePath)
				basePath = ".";
			
			if(FS_FilenameCompare(topDir, basePath))
			{
				len = Com_sprintf(libPath, sizeof(libPath), "%s%c%s", basePath, PATH_SEP, name);
				if(len < sizeof(libPath))
				{
					Com_Printf("Trying to load \"%s\" from \"%s\"...\n", name, basePath);
					dllhandle = Sys_LoadLibrary(libPath);
				}
				else
				{
					Com_Printf("Skipping trying to load \"%s\" from \"%s\", file name is too long.\n", name, basePath);
				}
			}
			
			if(!dllhandle)
				Com_Printf("Loading \"%s\" failed\n", name);
		}
	}
	
	return dllhandle;
}

/*
=================
Sys_LoadGameDll

Used to load a development dll instead of a virtual machine
=================
*/
void *Sys_LoadGameDll(const char *name,
	vmMainProc *entryPoint,
	intptr_t (*systemcalls)(intptr_t, ...))
{
	typedef void (*dllEntry_t)(intptr_t (*syscallptr)(intptr_t, ...));
	dllEntry_t dllEntry;
	void *libHandle;

	assert(name);

	if(!Sys_DllExtension(name))
	{
		Com_Printf("Refusing to attempt to load library \"%s\": Extension not allowed.\n", name);
		return NULL;
	}

	Com_Printf( "Loading DLL file: %s\n", name);
	libHandle = Sys_LoadLibrary(name);

	if(!libHandle)
	{
		Com_Printf("Sys_LoadGameDll(%s) failed:\n\"%s\"\n", name, Sys_LibraryError());
		return NULL;
	}

	dllEntry = (dllEntry_t)Sys_LoadFunction( libHandle, "dllEntry" );
	*entryPoint = (vmMainProc)Sys_LoadFunction( libHandle, "vmMain" );

	if ( !*entryPoint || !dllEntry )
	{
		Com_Printf ( "Sys_LoadGameDll(%s) failed to find vmMain function:\n\"%s\" !\n", name, Sys_LibraryError( ) );
		Sys_UnloadLibrary(libHandle);

		return NULL;
	}

	Com_Printf ( "Sys_LoadGameDll(%s) found vmMain function at %p\n", name, *entryPoint );
	dllEntry( systemcalls );

	return libHandle;
}

/*
=================
Sys_ParseArgs

Answers --version and --help anywhere on the command line, and -v and -h
alone, before the engine starts
=================
*/
void Sys_ParseArgs( int argc, char **argv )
{
	const char *name = argv[0];
	const char *p;
	int i;

	// the program's name, after either separator: on Windows argv[0] can
	// have both, and Sys_Basename splits on one and may change its argument
	for( p = argv[0]; *p; p++ )
	{
		if( *p == '/' || *p == '\\' )
			name = p + 1;
	}

	for( i = 1; i < argc; i++ )
	{
		// what follows is a link, not an option
		if( !strcmp( argv[i], "--uri" ) )
			break;

		if( !strcmp( argv[i], "--version" ) ||
				( argc == 2 && !strcmp( argv[i], "-v" ) ) )
		{
			const char* date = PRODUCT_DATE;
#ifdef DEDICATED
			fprintf( stdout, Q3_VERSION " dedicated server (%s)\n", date );
#else
			fprintf( stdout, Q3_VERSION " client (%s)\n", date );
#endif
			Sys_Exit( 0 );
		}

		if( !strcmp( argv[i], "--help" ) ||
				( argc == 2 && !strcmp( argv[i], "-h" ) ) )
		{
			fprintf( stdout, "Usage: %s [--help] [--version] [+command [arguments]]...\n"
				"\n"
				"Each +command runs as if typed into the console, after the configs.\n"
				"\n"
				"  +set <cvar> <value>   set a cvar for this run\n"
				"  +exec <file>          run a config, such as server.cfg\n"
#ifdef DEDICATED
				"  +map <map>            start the server on a map\n"
#else
				"  +connect <server>     join a server\n"
				"  +map <map>            start a server on a map, and play on it\n"
#endif
				"\n"
				"Cvars set at startup with +set include fs_homepath (where configs,\n"
				"downloads and logs are kept), fs_basepath (where the game's data is),\n"
				"fs_game (a mod), net_port (the UDP port, 27960)"
#ifdef DEDICATED
				" and dedicated (1 for\n"
				"the LAN, 2 to be listed on the Internet's master servers)"
#endif
				".\n", name );
			Sys_Exit( 0 );
		}
	}
}

#ifdef PROTOCOL_HANDLER
/*
=================
Sys_NextProtocolScheme

The first scheme in a list of them separated by spaces, such as
PROTOCOL_HANDLER, and its length; NULL at the list's end. The next one
comes after scheme + length.
=================
*/
const char *Sys_NextProtocolScheme( const char *list, int *length )
{
	list += strspn( list, " " );
	*length = strcspn( list, " " );
	return *length ? list : NULL;
}

/*
=================
Sys_ProtocolUriScheme

The length of the URI's scheme and the colon after it, if the scheme is
one of PROTOCOL_HANDLER's; 0 otherwise. Schemes don't depend on case.
=================
*/
int Sys_ProtocolUriScheme( const char *uri )
{
	const char *scheme;
	int len;

	for ( scheme = Sys_NextProtocolScheme( PROTOCOL_HANDLER, &len ); scheme;
		scheme = Sys_NextProtocolScheme( scheme + len, &len ) )
	{
		if ( !Q_stricmpn( uri, scheme, len ) && uri[len] == ':' )
			return len + 1;
	}

	return 0;
}

/*
=================
Sys_ParseProtocolUri

This parses a protocol URI, e.g. "quake3://connect/example.com:27950"
to a string that can be run in the console, or a null pointer if the
operation is invalid or unsupported.
At the moment only the "connect" command is supported.
=================
*/
char *Sys_ParseProtocolUri( const char *uri )
{
	int schemeLength = Sys_ProtocolUriScheme( uri );

	// Both "quake3://" and "quake3:" can be used
	if ( !schemeLength )
	{
		Com_Printf( "Sys_ParseProtocolUri: unsupported protocol.\n" );
		return NULL;
	}
	uri += schemeLength;
	if ( !Q_strncmp( uri, "//", strlen( "//" ) ) )
	{
		uri += strlen( "//" );
	}
	Com_Printf( "Sys_ParseProtocolUri: %s\n", uri );

	// At the moment, only "connect/hostname:port" is supported
	if ( !Q_strncmp( uri, "connect/", strlen( "connect/" ) ) )
	{
		int i, bufsize;
		char *out;

		uri += strlen( "connect/" );
		if ( *uri == '\0' || *uri == '?' || *uri == '/' )
		{
			Com_Printf( "Sys_ParseProtocolUri: missing argument.\n" );
			return NULL;
		}

		// Check for any unsupported characters
		// For safety reasons, the "hostname:port" part can only
		// contain characters from: a-zA-Z0-9.:-[]
		for ( i=0; uri[i] != '\0'; i++ )
		{
			if ( uri[i] == '?' || uri[i] == '/' )
			{
				// For forwards compatibility, any query string parameters are ignored (e.g. "?password=abcd")
				// However, these are not passed on macOS, so it may be a bad idea to add them.
				// So is a path after the address, such as the slash a browser may add
				break;
			}

			if ( isalpha( uri[i] ) == 0 && isdigit( uri[i] ) == 0
				&& uri[i] != '.' && uri[i] != ':' && uri[i] != '-'
				&& uri[i] != '[' && uri[i] != ']' )
			{
				Com_Printf( "Sys_ParseProtocolUri: hostname contains unsupported character.\n" );
				return NULL;
			}
		}

		bufsize = strlen( "connect " ) + i + 1;
		out = malloc( bufsize );
		strcpy( out, "connect " );
		strncat( out, uri, i );
		return out;
	}
	else
	{
		Com_Printf( "Sys_ParseProtocolUri: unsupported command.\n" );
		return NULL;
	}
}
#endif

#ifndef DEFAULT_BASEDIR
#	if defined(DEFAULT_RELATIVE_BASEDIR)
#		define DEFAULT_BASEDIR Sys_BinaryPathRelative(DEFAULT_RELATIVE_BASEDIR)
#	elif defined(__APPLE__)
#		define DEFAULT_BASEDIR Sys_StripAppBundle(Sys_BinaryPath())
#	else
#		define DEFAULT_BASEDIR Sys_BinaryPath()
#	endif
#endif

/*
=================
Sys_SigHandler
=================
*/
void Sys_SigHandler( int signal )
{
	static qboolean signalcaught = qfalse;
	static qboolean crashed = qfalse;

	// a stop asked for while a crash shuts down is still a crash
	if( signal != SIGTERM && signal != SIGINT )
		crashed = qtrue;

	if( signalcaught )
	{
		fprintf( stderr, "DOUBLE SIGNAL FAULT: Received signal %d, exiting...\n",
			signal );
	}
	else
	{
		signalcaught = qtrue;
		VM_Forced_Unload_Start();
#ifndef DEDICATED
		CL_Shutdown(va("Received signal %d", signal), qtrue, qtrue);
#endif
		SV_Shutdown(va("Received signal %d", signal) );
		VM_Forced_Unload_Done();
	}

	// a stop that was asked for succeeds, so that service managers can tell
	// it from a crash
	Sys_Exit( crashed ? 2 : 0 );
}

/*
=================
Sys_Start
=================
*/
static void Sys_Start( int argc, char **argv )
{
	int   i;
	// Com_StartupVariable reads it after Com_Init
	static char  commandLine[ MAX_STRING_CHARS ] = { 0 };
#ifdef PROTOCOL_HANDLER
	char *protocolCommand = NULL;
#endif

	// a service manager's log, a pipe or a file gets each line as it's
	// printed, as a terminal does, and nothing is lost if the process is
	// killed; Windows' C library has no line buffering, so none there
#ifdef _WIN32
	setvbuf( stdout, NULL, _IONBF, 0 );
#else
	setvbuf( stdout, NULL, _IOLBF, BUFSIZ );
#endif
	setvbuf( stderr, NULL, _IONBF, 0 );

#ifdef USE_AUTOUPDATER
	Sys_LaunchAutoupdater(argc, argv);
#endif

#ifndef DEDICATED
	// SDL version check

	// Compile time
#	if !SDL_VERSION_ATLEAST(MINSDL_MAJOR,MINSDL_MINOR,MINSDL_MICRO)
#		error A more recent version of SDL is required
#	endif

	// Run time
	int version = SDL_GetVersion();
	int major = SDL_VERSIONNUM_MAJOR(version);
	int minor = SDL_VERSIONNUM_MINOR(version);
	int micro = SDL_VERSIONNUM_MICRO(version);

#define MINSDL_VERSION \
	XSTRING(MINSDL_MAJOR) "." \
	XSTRING(MINSDL_MINOR) "." \
	XSTRING(MINSDL_MICRO)

	if( SDL_VERSIONNUM( major, minor, micro ) <
			SDL_VERSIONNUM( MINSDL_MAJOR, MINSDL_MINOR, MINSDL_MICRO ) )
	{
		Sys_Dialog( DT_ERROR, va( "SDL version " MINSDL_VERSION " or greater is required, "
			"but only version %d.%d.%d was found. You may be able to obtain a more recent copy "
			"from https://www.libsdl.org/.", major, minor, micro ), "SDL Library Too Old" );

		Sys_Exit( 1 );
	}
#endif

	Sys_PlatformInit( );
	Sys_SetMaxFileLimit( );

	// Set the initial time base
	Sys_Milliseconds( );

#ifdef __APPLE__
	// This is passed if we are launched by double-clicking
	if ( argc >= 2 && Q_strncmp ( argv[1], "-psn", 4 ) == 0 )
		argc = 1;
#endif

	Sys_ParseArgs( argc, argv );
	Sys_SetBinaryPath( Sys_Dirname( argv[ 0 ] ) );
	Sys_SetDefaultInstallPath( DEFAULT_BASEDIR );

	// Concatenate the command line for passing to Com_Init
	for( i = 1; i < argc; i++ )
	{
		qboolean containsSpaces;

		// For security reasons we always detect --uri, even when PROTOCOL_HANDLER is undefined
		// Any arguments after "--uri quake3://..." is ignored
		if ( !strcmp( argv[i], "--uri" ) )
		{
#ifdef PROTOCOL_HANDLER
			if ( argc > i+1 )
			{
				protocolCommand = Sys_ParseProtocolUri( argv[i+1] );
			}
#endif
			break;
		}

		containsSpaces = strchr(argv[i], ' ') != NULL;
		if (containsSpaces)
			Q_strcat( commandLine, sizeof( commandLine ), "\"" );

		Q_strcat( commandLine, sizeof( commandLine ), argv[ i ] );

		if (containsSpaces)
			Q_strcat( commandLine, sizeof( commandLine ), "\"" );

		Q_strcat( commandLine, sizeof( commandLine ), " " );
	}

#ifdef PROTOCOL_HANDLER
	if ( protocolCommand != NULL )
	{
		Q_strcat( commandLine, sizeof( commandLine ), "+" );
		Q_strcat( commandLine, sizeof( commandLine ), protocolCommand );
		free( protocolCommand );
	}
#endif

	CON_Init( );
	Com_Init( commandLine );
	NET_Init( );

#ifdef USE_PROTOCOL_REGISTRATION
	{
		// after Com_Init, so the player's config and command line can
		// turn it off
		cvar_t *protocolHandler = Cvar_Get( "cl_protocolHandler", "1", CVAR_ARCHIVE );

		Cvar_SetDescription( protocolHandler, "Register the links that open this game, <scheme>://connect/<server> for the schemes " PROTOCOL_HANDLER ", "
			"for this user at startup: the copy of the game that ran last has them. On Windows, a scheme stays with another program that is still installed." );
		if ( protocolHandler->integer )
			Sys_RegisterProtocolHandler( );
	}
#endif

	signal( SIGILL, Sys_SigHandler );
	signal( SIGFPE, Sys_SigHandler );
	signal( SIGSEGV, Sys_SigHandler );
	signal( SIGTERM, Sys_SigHandler );
	signal( SIGINT, Sys_SigHandler );
}

#ifdef DEDICATED
/*
=================
main
=================
*/
int main( int argc, char **argv )
{
	Sys_Start( argc, argv );

	while( 1 )
	{
		while( !Com_WaitFrame( ) )
		{
		}

		Com_Frame( );
	}

	return 0;
}
#else
static qboolean inFrame = qfalse;

#ifdef __EMSCRIPTEN__
#define REFRESH_SAMPLES 15
static int64_t refreshSamples[ REFRESH_SAMPLES ];
static int numRefreshSamples = 0, nextRefreshSample = 0;
static int64_t refreshInterval = 0;

/*
=================
Sys_CompareRefreshSamples
=================
*/
static int Sys_CompareRefreshSamples( const void *a, const void *b )
{
	int64_t d = *(const int64_t *)a - *(const int64_t *)b;

	return ( d > 0 ) - ( d < 0 );
}

/*
=================
Sys_SampleRefresh

The browser calls SDL_AppIterate on each refresh of the display: the
median of the last few intervals between calls is the refresh interval,
whatever frames took longer
=================
*/
static void Sys_SampleRefresh( void )
{
	static int64_t last = 0;
	int64_t now = Sys_Nanoseconds( );
	int64_t sorted[ REFRESH_SAMPLES ];

	if( last && now - last < 100000000 )
	{
		refreshSamples[ nextRefreshSample ] = now - last;
		nextRefreshSample = ( nextRefreshSample + 1 ) % REFRESH_SAMPLES;
		numRefreshSamples = MIN( numRefreshSamples + 1, REFRESH_SAMPLES );
	}
	last = now;

	if( numRefreshSamples == REFRESH_SAMPLES )
	{
		Com_Memcpy( sorted, refreshSamples, sizeof( sorted ) );
		qsort( sorted, REFRESH_SAMPLES, sizeof( sorted[ 0 ] ), Sys_CompareRefreshSamples );
		refreshInterval = sorted[ REFRESH_SAMPLES / 2 ];
	}
}

/*
=================
Sys_RefreshInterval

As measured, since a page isn't told the display's refresh rate
=================
*/
int64_t Sys_RefreshInterval( void )
{
	return refreshInterval;
}
#else
/*
=================
Sys_RefreshInterval

From the display mode of the display the game's window is on: the one
with the keyboard or the mouse, or failing both, the first
=================
*/
int64_t Sys_RefreshInterval( void )
{
	SDL_Window *window = SDL_GetKeyboardFocus( );
	const SDL_DisplayMode *mode;

	if( !window )
	{
		window = SDL_GetMouseFocus( );
	}
	if( !window )
	{
		SDL_Window **windows = SDL_GetWindows( NULL );

		window = windows ? windows[ 0 ] : NULL;
		SDL_free( windows );
	}

	mode = window ? SDL_GetCurrentDisplayMode( SDL_GetDisplayForWindow( window ) ) : NULL;

	// the rate as a fraction, exact (59.94 is 60000/1001), which SDL works
	// out where a backend gives only the float
	return mode ? Com_RefreshNanoseconds( mode->refresh_rate_numerator, mode->refresh_rate_denominator ) : 0;
}
#endif

/*
=================
Sys_SleepPrecise
=================
*/
void Sys_SleepPrecise( int64_t ns )
{
	if( ns > 0 )
	{
		SDL_DelayPrecise( (Uint64)ns );
	}
}

/*
=================
Sys_TakeQueuedEvents

Hands the client the events SDL has queued since the last frame took its
input, without pumping for more: pumping can start a Windows modal loop,
which runs frames from inside it (Sys_WindowsMessageHook). Windows' raw
input comes in on a thread of SDL's own and is queued as it arrives, so
this takes what came during the wait for the frame; elsewhere events
reach the queue only as it's pumped, before SDL_AppIterate, and there
are none.
=================
*/
static void Sys_TakeQueuedEvents( void )
{
	SDL_Event e;

	while( SDL_PeepEvents( &e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST ) > 0 )
	{
		SDL_AppEvent( NULL, &e );
	}
}

/*
=================
Sys_Frame

Runs a frame that's due, with the freshest input there is
=================
*/
static void Sys_Frame( void )
{
	Sys_TakeQueuedEvents( );
	inFrame = qtrue;
	Com_Frame( );
	inFrame = qfalse;
}

#ifdef SDL_PLATFORM_WINDOWS
// runs a frame from Sys_WindowsMessageHook
#define SYS_FRAME_MESSAGE ( WM_APP + 0x51 )
static qboolean framePosted = qfalse;
static HWND framePostedTo = NULL;

/*
=================
Sys_WindowsMessageHook

While Windows moves or resizes the window, SDL runs frames from a timer,
but Windows fires timers only when no input is waiting, so a moving mouse
holds them off. When a frame comes due, post the window a message to run
it: posted messages come before input, and after the step of the move or
resize at hand. The frame first takes the events SDL has queued, as SDL
does before SDL_AppIterate (Sys_Frame).
=================
*/
static bool SDLCALL Sys_WindowsMessageHook( void *userdata, MSG *msg )
{
	if( msg->message == SYS_FRAME_MESSAGE )
	{
		framePosted = qfalse;

		if( !inFrame && Sys_InModalLoop( ) )
		{
			// it's due; this sends the server's queued packets
			Com_WaitFrame( );
			Sys_Frame( );
		}

		return false;
	}

	// a window destroyed with the message waiting (vid_restart) took it
	// along, and it will never come
	if( framePosted && framePostedTo && !IsWindow( framePostedTo ) )
	{
		framePosted = qfalse;
	}

	if( !framePosted && !inFrame && Com_FrameDue( ) && Sys_InModalLoop( ) )
	{
		framePosted = PostMessage( msg->hwnd, SYS_FRAME_MESSAGE, 0, 0 ) != 0;
		framePostedTo = msg->hwnd;
	}

	return true;
}
#endif

/*
=================
SDL_AppInit
=================
*/
SDL_AppResult SDL_AppInit( void **appstate, int argc, char *argv[] )
{
	// Before SDL starts: desktops find the app's icon by its ID, and name
	// its audio streams and windows after it
	SDL_SetAppMetadata( CLIENT_WINDOW_TITLE, PRODUCT_VERSION, APP_ID );
#ifdef __EMSCRIPTEN__
	// keys go to the game only while its canvas has focus, not from the
	// whole page, which may have fields of its own
	SDL_SetHint( SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas" );
#endif

	// The click that focuses a window also reaches the game, so it can be
	// the click that captures the mouse rather than a click before it
	SDL_SetHint( SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1" );

	Sys_Start( argc, argv );

#ifdef SDL_PLATFORM_WINDOWS
	SDL_SetWindowsMessageHook( Sys_WindowsMessageHook, NULL );
#endif

	return SDL_APP_CONTINUE;
}

/*
=================
SDL_AppIterate

SDL handles the pending events (SDL_AppEvent) before each call
=================
*/
SDL_AppResult SDL_AppIterate( void *appstate )
{
	qboolean due;

	// While Windows moves or resizes the window, or shows the window menu,
	// SDL calls this from its modal loop. That loop can also start inside
	// a frame, when something in it pumps events; let that frame finish.
	if( inFrame )
	{
		return SDL_APP_CONTINUE;
	}

#ifdef __EMSCRIPTEN__
	Sys_SampleRefresh( );
#endif

	due = Com_WaitFrame( );

	// Return between sleeps, so SDL handles the events that come in
	// meanwhile and the frame starts with them; a browser, which calls this
	// once per display refresh, skips a refresh that comes early for a
	// capped frame. A Windows modal loop calls this from a timer instead,
	// and handles no events in between; wait there, or frames come only on
	// the timer's ticks. Skip one, too, while the GPU is still drawing the
	// last frame where the renderer can't wait for it (the web), rather
	// than queue another behind it.
	if( ( !due && !Sys_InModalLoop( ) ) || !CL_FrameReady( ) )
	{
		return SDL_APP_CONTINUE;
	}

	while( !due )
	{
		due = Com_WaitFrame( );
	}

	Sys_Frame( );

	return SDL_APP_CONTINUE;
}

/*
=================
SDL_AppEvent
=================
*/
SDL_AppResult SDL_AppEvent( void *appstate, SDL_Event *event )
{
	switch( event->type )
	{
		// SDL sends these as they happen, on phones from the system's own
		// thread, even while a frame runs. Nothing handles them yet.
		case SDL_EVENT_TERMINATING:
		case SDL_EVENT_LOW_MEMORY:
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
		case SDL_EVENT_DID_ENTER_BACKGROUND:
		case SDL_EVENT_WILL_ENTER_FOREGROUND:
		case SDL_EVENT_DID_ENTER_FOREGROUND:
			return SDL_APP_CONTINUE;

		default:
			break;
	}

	IN_ProcessEvent( event );
	return SDL_APP_CONTINUE;
}

/*
=================
SDL_AppQuit

The engine quits through Sys_Quit, which exits before SDL gets here. SDL
calls this when the system ends the app, as phones do: quit as the quit
command does, shutting the game down.
=================
*/
void SDL_AppQuit( void *appstate, SDL_AppResult result )
{
	Cbuf_ExecuteText( EXEC_NOW, "quit\n" );
}
#endif
