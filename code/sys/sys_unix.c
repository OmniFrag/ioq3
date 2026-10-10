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

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <pwd.h>
#include <libgen.h>
#include <fcntl.h>
#include <fenv.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/resource.h>
#include <spawn.h>
#if defined( __linux__ ) && !defined( __EMSCRIPTEN__ )
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

qboolean stdinIsATTY;

static char execBuffer[ 1024 ];
static char *execBufferPointer;
static char *execArgv[ 16 ];
static int execArgc;

/*
==============
Sys_ClearExecBuffer
==============
*/
static void Sys_ClearExecBuffer( void )
{
	execBufferPointer = execBuffer;
	Com_Memset( execArgv, 0, sizeof( execArgv ) );
	execArgc = 0;
}

/*
==============
Sys_AppendToExecBuffer
==============
*/
static void Sys_AppendToExecBuffer( const char *text )
{
	size_t size = sizeof( execBuffer ) - ( execBufferPointer - execBuffer );
	int length = strlen( text ) + 1;

	if( length > size || execArgc >= ARRAY_LEN( execArgv ) )
		return;

	Q_strncpyz( execBufferPointer, text, size );
	execArgv[ execArgc++ ] = execBufferPointer;

	execBufferPointer += length;
}

/*
==============
Sys_Exec
==============
*/
static int Sys_Exec( void )
{
	pid_t pid = fork( );

	if( pid < 0 )
		return -1;

	if( pid )
	{
		// Parent
		int exitCode;

		wait( &exitCode );

		return WEXITSTATUS( exitCode );
	}
	else
	{
		// Child
		execvp( execArgv[ 0 ], execArgv );

		// Failed to execute
		exit( -1 );

		return -1;
	}
}

/*
==================
Sys_HomeName

The name of our directory in the home
==================
*/
static const char *Sys_HomeName(void)
{
	if( com_homepath && com_homepath->string[0] )
		return com_homepath->string;

	return HOMEPATH_NAME;
}

#ifdef __APPLE__

/*
==================
Sys_AppSupportPath

The directory name in ~/Library/Application Support, or "" without a HOME
==================
*/
static void Sys_AppSupportPath( char *path, int size, const char *name )
{
	char *p;

	path[0] = '\0';
	if( ( p = getenv( "HOME" ) ) != NULL )
		Com_sprintf( path, size, "%s%cLibrary/Application Support/%s", p, PATH_SEP, name );
}

/*
==================
Sys_DefaultHomePath
==================
*/
static char *Sys_DefaultHomePath(void)
{
	static char homePath[ MAX_OSPATH ] = { 0 };

	if( !*homePath )
		Sys_AppSupportPath( homePath, sizeof( homePath ), Sys_HomeName( ) );

	return homePath;
}

char *Sys_DefaultHomeConfigPath(void) { return Sys_DefaultHomePath(); }
char *Sys_DefaultHomeDataPath(void)   { return Sys_DefaultHomePath(); }
char *Sys_DefaultHomeStatePath(void)  { return Sys_DefaultHomePath(); }

#else // __APPLE__

/*
==================
Sys_XDGPath

The directory name in the XDG base directory xdgVar names, or without it
in fallback under the home; "" with neither
==================
*/
static void Sys_XDGPath( char *path, int size, const char *xdgVar, const char *fallback, const char *name )
{
	char *p;

	path[0] = '\0';
	if( ( p = getenv( xdgVar ) ) != NULL && *p != '\0' )
		Com_sprintf( path, size, "%s%c%s", p, PATH_SEP, name );
	else if( ( p = getenv( "HOME" ) ) != NULL && *p != '\0' )
		Com_sprintf( path, size, "%s%c%s%c%s", p, PATH_SEP, fallback, PATH_SEP, name );
}

/*
==================
Sys_HomeConfigPath
==================
*/
char *Sys_HomeConfigPath(void)
{
	static char homeConfigPath[ MAX_OSPATH ] = { 0 };

	if( !*homeConfigPath )
		Sys_XDGPath( homeConfigPath, sizeof( homeConfigPath ), "XDG_CONFIG_HOME", ".config", Sys_HomeName( ) );

	return homeConfigPath;
}

/*
==================
Sys_HomeDataPath
==================
*/
char *Sys_HomeDataPath(void)
{
	static char homeDataPath[ MAX_OSPATH ] = { 0 };

	if( !*homeDataPath )
		Sys_XDGPath( homeDataPath, sizeof( homeDataPath ), "XDG_DATA_HOME", ".local/share", Sys_HomeName( ) );

	return homeDataPath;
}

/*
==================
Sys_HomeStatePath
==================
*/
char *Sys_HomeStatePath(void)
{
	static char homeStatePath[ MAX_OSPATH ] = { 0 };

	if( !*homeStatePath )
		Sys_XDGPath( homeStatePath, sizeof( homeStatePath ), "XDG_STATE_HOME", ".local/state", Sys_HomeName( ) );

	return homeStatePath;
}

#if defined( HOMEPATH_NAME_UNIX_LEGACY ) || \
	( defined( HOMEPATH_NAME_PREDECESSOR ) && defined( HOMEPATH_NAME_PREDECESSOR_UNIX ) )
/*
==================
Sys_OldHomePath

The directory name in the home, where files went before XDG, or "" in a
Flatpak, which always uses XDG
==================
*/
static void Sys_OldHomePath( char *path, int size, const char *name )
{
	char *p;

	path[0] = '\0';
	if( ( p = getenv( "FLATPAK_ID" ) ) != NULL && *p != '\0' )
		return;
	if( ( p = getenv( "HOME" ) ) != NULL && *p != '\0' )
		Com_sprintf( path, size, "%s%c%s", p, PATH_SEP, name );
}
#endif

/*
==================
Sys_LegacyHomePath
==================
*/
static char *Sys_LegacyHomePath(void)
{
	static char homePath[ MAX_OSPATH ] = { 0 };

#ifdef HOMEPATH_NAME_UNIX_LEGACY
	if( !*homePath )
		Sys_OldHomePath( homePath, sizeof( homePath ), HOMEPATH_NAME_UNIX_LEGACY );
#endif

	return homePath;
}

/*
==================
Sys_MigrateToXDG
==================
*/
qboolean Sys_MigrateToXDG(void)
{
	const char *scriptTemplate =
		"#!/bin/sh\n"

		"set -eu\n"

		"legacy_home=\"%s\"\n"
		"xdg_config_home=\"%s\"\n"
		"xdg_data_home=\"%s\"\n"
		"xdg_state_home=\"%s\"\n"

		"xdg_config_pattern=\"*.cfg\"\n"
		"xdg_data_pattern=\"demos/*.dm_* *.log *.pk3 *.txt \\\n"
		"    screenshots/*.jpg screenshots/*.tga videos/*.avi\"\n"
		"xdg_state_pattern=\"*.dat q3history q3key\"\n"

		"glob_copy() {\n"
		"    game_dir=${1:+$1/}\n"
		"    dst=\"$2\"\n"
		"    shift 2\n"
		"    for pattern in \"$@\"; do\n"
		"        subdir=$(dirname \"$pattern\")\n"
		"        [ \"$subdir\" = \".\" ] && subdir=\"\"\n"
		"        find \"$legacy_home/$game_dir\" \\\n"
		"            -path \"$legacy_home/$game_dir$pattern\" -type f \\\n"
		"            -exec mkdir -p \"$dst/$game_dir$subdir\" \\; \\\n"
		"            -exec cp -av {} \"$dst/$game_dir$subdir\" \\;\n"
		"    done\n"
		"}\n"

		"unmatched_copy() {\n"
		"    game_dir=${1:+$1/}\n"
		"    shift\n"
		"    find_args=\"\"\n"
		"    for pattern in \"$@\"; do\n"
		"        find_args=\"$find_args \\\n"
		"            -not -path \\\"$legacy_home/$game_dir$pattern\\\"\"\n"
		"    done\n"
		"    eval \"find '$legacy_home/$game_dir' -type f $find_args\" | \\\n"
		"    while IFS= read -r file; do\n"
		"        dst=\"$xdg_data_home${file#$legacy_home}\"\n"
		"        dst_dir=$(dirname \"$dst\")\n"
		"        mkdir -p \"$dst_dir\"\n"
		"        cp -av \"$file\" \"$dst\"\n"
		"    done\n"
		"}\n"

		"echo \"Starting XDG migration...\"\n"

		"glob_copy \"\" \"$xdg_state_home\" \"qkey\"\n"

		"for game_dir in \"$legacy_home\"/*; do\n"
		"    [ -d \"$game_dir\" ] || continue\n"
		"    game=$(basename \"$game_dir\")\n"
		"    glob_copy \"$game\" \"$xdg_config_home\" $xdg_config_pattern\n"
		"    glob_copy \"$game\" \"$xdg_data_home\" $xdg_data_pattern\n"
		"    glob_copy \"$game\" \"$xdg_state_home\" $xdg_state_pattern\n"
		"    unmatched_copy \"$game\" \\\n"
		"        $xdg_config_pattern \\\n"
		"        $xdg_data_pattern \\\n"
		"        $xdg_state_pattern\n"
		"done\n"

		"echo \"XDG migration complete!\"\n";

	char scriptBuffer[2048];
	int len = Com_sprintf( scriptBuffer, sizeof( scriptBuffer ), scriptTemplate,
		Sys_LegacyHomePath( ), Sys_HomeConfigPath( ),
		Sys_HomeDataPath( ), Sys_HomeStatePath( ) );

	if( len < 0 || len >= (int)sizeof( scriptBuffer ) )
	{
		Com_Printf( "XDG migration error: substitution failed.\n" );
		return qfalse;
	}

	char scriptPath[] = "/tmp/xdgmigrationXXXXXX";
	int fd = mkstemp( scriptPath );
	if( fd == -1 )
	{
		Com_Printf( "XDG migration error: script creation failed.\n" );
		return qfalse;
	}

	if( write( fd, scriptBuffer, len ) != len )
	{
		close( fd );
		unlink( scriptPath );
		Com_Printf( "XDG migration error: script write failed.\n" );
		return qfalse;
	}
	close( fd );

	if( chmod( scriptPath, 0700 ) == -1 )
	{
		unlink( scriptPath );
		Com_Printf( "XDG migration error: script chmod failed.\n" );
		return qfalse;
	}

	Sys_ClearExecBuffer( );
	Sys_AppendToExecBuffer( scriptPath );
	int result = Sys_Exec( );
	unlink( scriptPath );

	return result == 0;
}

/*
==================
Sys_ShouldUseLegacyHomePath
==================
*/
static qboolean Sys_ShouldUseLegacyHomePath(void)
{
	if( access( Sys_HomeConfigPath( ), F_OK ) == 0 )
	{
		// If the XDG config directory exists, prefer XDG layout, regardless
		return qfalse;
	}

	if( ( com_homepath && com_homepath->string[0] ) ||
		Cvar_VariableString( "fs_homepath" )[0] )
	{
		// If a custom homepath has been explicity set then
		// that strongly implies that migration isn't desired
		return qfalse;
	}

	const char *legacyHomePath = Sys_LegacyHomePath();

	if( !*legacyHomePath || access( legacyHomePath, F_OK ) != 0 )
	{
		// The legacy home path doesn't exist
		return qfalse;
	}

	char migrationRefusedPath[ MAX_OSPATH ];
	Com_sprintf( migrationRefusedPath, sizeof( migrationRefusedPath ),
		"%s/.xdgMigrationRefused", legacyHomePath );

	// If the user hasn't already refused, ask if they want to migrate; a
	// dedicated server can't ask, so it keeps the legacy home and leaves
	// the question to the player
	if( Com_IsClient( ) && access( migrationRefusedPath, F_OK ) != 0 )
	{
		dialogResult_t result = Sys_Dialog( DT_YES_NO, va(
			"Modern games and applications store files in "
			"directories according to the Free Desktop standard. "
			"Here's what that would look like for %s:\n\n"
			"Configuration files:\n  %s\n\n"
			"Data files; pk3s, screenshots, logs, demos, etc.:\n  %s\n\n"
			"Internal runtime files:\n  %s\n\n"
			"At the moment all of these files are found here:\n  %s\n\n"
			"Do you want to copy your files to these new directories?",
			PRODUCT_NAME,
			Sys_HomeConfigPath( ), Sys_HomeDataPath( ), Sys_HomeStatePath( ),
			legacyHomePath ),
			"Home Directory Files Upgrade" );

		if( result == DR_YES )
			return !Sys_MigrateToXDG( );

		// Guard against asking again in future
		fclose( fopen( migrationRefusedPath, "w" ) );
	}

	return qtrue;
}

/*
==================
Sys_DefaultHomeConfigPath
==================
*/
char *Sys_DefaultHomeConfigPath(void)
{
	if( Sys_ShouldUseLegacyHomePath( ) )
		return Sys_LegacyHomePath( );

	return Sys_HomeConfigPath( );
}

/*
==================
Sys_DefaultHomeDataPath
==================
*/
char *Sys_DefaultHomeDataPath(void)
{
	if( Sys_ShouldUseLegacyHomePath( ) )
		return Sys_LegacyHomePath( );

	return Sys_HomeDataPath( );
}

/*
==================
Sys_DefaultHomeStatePath
==================
*/
char *Sys_DefaultHomeStatePath(void)
{
	if( Sys_ShouldUseLegacyHomePath( ) )
		return Sys_LegacyHomePath( );

	return Sys_HomeStatePath( );
}

#endif

#ifdef HOMEPATH_NAME_PREDECESSOR
/*
==================
Sys_IsDirectory
==================
*/
static qboolean Sys_IsDirectory( const char *path )
{
	struct stat st;

	return !stat( path, &st ) && S_ISDIR( st.st_mode );
}
#endif

/*
==================
Sys_PredecessorHomePaths

The config and data homes of the product this build succeeds, named by
HOMEPATH_NAME_PREDECESSOR, found where that product keeps them; "" where
the build names none, or they don't exist. On Linux that's the XDG layout
if its config directory exists, as ioquake3 decides, or else the old
directory HOMEPATH_NAME_PREDECESSOR_UNIX names.
==================
*/
void Sys_PredecessorHomePaths( const char **configPath, const char **dataPath )
{
	static char predConfigPath[ MAX_OSPATH ];
	static char predDataPath[ MAX_OSPATH ];

#ifdef HOMEPATH_NAME_PREDECESSOR
#ifdef __APPLE__
	Sys_AppSupportPath( predConfigPath, sizeof( predConfigPath ), HOMEPATH_NAME_PREDECESSOR );
	Q_strncpyz( predDataPath, predConfigPath, sizeof( predDataPath ) );
#else
	Sys_XDGPath( predConfigPath, sizeof( predConfigPath ), "XDG_CONFIG_HOME", ".config", HOMEPATH_NAME_PREDECESSOR );
	Sys_XDGPath( predDataPath, sizeof( predDataPath ), "XDG_DATA_HOME", ".local/share", HOMEPATH_NAME_PREDECESSOR );
#ifdef HOMEPATH_NAME_PREDECESSOR_UNIX
	if( !Sys_IsDirectory( predConfigPath ) )
	{
		char oldPath[ MAX_OSPATH ];

		Sys_OldHomePath( oldPath, sizeof( oldPath ), HOMEPATH_NAME_PREDECESSOR_UNIX );
		if( Sys_IsDirectory( oldPath ) )
		{
			Q_strncpyz( predConfigPath, oldPath, sizeof( predConfigPath ) );
			Q_strncpyz( predDataPath, oldPath, sizeof( predDataPath ) );
		}
	}
#endif
#endif
	if( !Sys_IsDirectory( predConfigPath ) )
		predConfigPath[0] = '\0';
	if( !Sys_IsDirectory( predDataPath ) )
		predDataPath[0] = '\0';
#endif

	*configPath = predConfigPath;
	*dataPath = predDataPath;
}

/*
================
Sys_SteamPath
================
*/
char *Sys_SteamPath( void )
{
	// Steam doesn't let you install Quake 3 on Mac/Linux
	return "";
}

/*
================
Sys_GogPath
================
*/
char *Sys_GogPath( void )
{
	// GOG doesn't let you install Quake 3 on Mac/Linux
	return "";
}

/*
================
Sys_MicrosoftStorePath
================
*/
char* Sys_MicrosoftStorePath(void)
{
	// Microsoft Store doesn't exist on Mac/Linux
	return "";
}


/*
================
Sys_Nanoseconds

The time since the first call, by a monotonic clock: the time of day can
jump when it's set, and game time mustn't
================
*/
int64_t Sys_Nanoseconds( void )
{
	static int64_t	base = -1;
	struct timespec	tp;
	int64_t			now;

	clock_gettime( CLOCK_MONOTONIC, &tp );
	now = (int64_t)tp.tv_sec * 1000000000 + tp.tv_nsec;

	if( base < 0 )
		base = now;

	return now - base;
}

/*
================
Sys_Milliseconds

Sys_Nanoseconds, in milliseconds; 0x7fffffff ms is ~24 days
================
*/
int Sys_Milliseconds (void)
{
	return (int)( Sys_Nanoseconds() / 1000000 );
}

/*
==================
Sys_RandomBytes
==================
*/
qboolean Sys_RandomBytes( byte *string, int len )
{
	FILE *fp;

	fp = fopen( "/dev/urandom", "r" );
	if( !fp )
		return qfalse;

	setvbuf( fp, NULL, _IONBF, 0 ); // don't buffer reads from /dev/urandom

	if( fread( string, sizeof( byte ), len, fp ) != len )
	{
		fclose( fp );
		return qfalse;
	}

	fclose( fp );
	return qtrue;
}

/*
==================
Sys_GetCurrentUser
==================
*/
char *Sys_GetCurrentUser( void )
{
	struct passwd *p;

	if ( (p = getpwuid( getuid() )) == NULL ) {
		return "player";
	}
	return p->pw_name;
}

#define MEM_THRESHOLD 96*1024*1024

/*
==================
Sys_LowPhysicalMemory

TODO
==================
*/
qboolean Sys_LowPhysicalMemory( void )
{
	return qfalse;
}

#ifndef __APPLE__
/*
==================
Sys_KeepAwake
==================
*/
qboolean Sys_KeepAwake( qboolean awake )
{
	return qfalse;
}
#endif

/*
==================
Sys_Basename
==================
*/
const char *Sys_Basename( char *path )
{
	return basename( path );
}

/*
==================
Sys_Dirname
==================
*/
const char *Sys_Dirname( char *path )
{
	return dirname( path );
}

/*
==============
Sys_FOpen
==============
*/
FILE *Sys_FOpen( const char *ospath, const char *mode ) {
	struct stat buf;

	// check if path exists and is a directory
	if ( !stat( ospath, &buf ) && S_ISDIR( buf.st_mode ) )
		return NULL;

	return fopen( ospath, mode );
}

/*
==================
Sys_Mkdir
==================
*/
qboolean Sys_Mkdir( const char *path )
{
	int result = mkdir( path, 0750 );

	if( result != 0 )
		return errno == EEXIST;

	return qtrue;
}

/*
==================
Sys_Mkfifo
==================
*/
FILE *Sys_Mkfifo( const char *ospath )
{
	FILE	*fifo;
	int	result;
	int	fn;
	struct	stat buf;

	// if file already exists AND is a pipefile, remove it
	if( !stat( ospath, &buf ) && S_ISFIFO( buf.st_mode ) )
		FS_Remove( ospath );

	result = mkfifo( ospath, 0600 );
	if( result != 0 )
		return NULL;

	fifo = fopen( ospath, "w+" );
	if( fifo )
	{
		fn = fileno( fifo );
		fcntl( fn, F_SETFL, O_NONBLOCK );
	}

	return fifo;
}

/*
==================
Sys_Cwd
==================
*/
char *Sys_Cwd( void )
{
	static char cwd[MAX_OSPATH];

	char *result = getcwd( cwd, sizeof( cwd ) - 1 );
	if( result != cwd )
		return NULL;

	cwd[MAX_OSPATH-1] = 0;

	return cwd;
}

/*
==================
Sys_BinaryPathRelative
==================
*/
char *Sys_BinaryPathRelative(const char *relative)
{
	static char resolved[MAX_OSPATH];
	char combined[MAX_OSPATH];

	snprintf(combined, sizeof(combined), "%s/%s", Sys_BinaryPath(), relative);

	if (!realpath(combined, resolved))
		return NULL;

	return resolved;
}

/*
==============================================================

DIRECTORY SCANNING

==============================================================
*/

#define MAX_FOUND_FILES 0x1000

/*
==================
Sys_ListFilteredFiles
==================
*/
void Sys_ListFilteredFiles( const char *basedir, char *subdirs, char *filter, char **list, int *numfiles )
{
	char          search[MAX_OSPATH], newsubdirs[MAX_OSPATH];
	char          filename[MAX_OSPATH];
	DIR           *fdir;
	struct dirent *d;
	struct stat   st;

	if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
		return;
	}

	if ( basedir[0] == '\0' ) {
		return;
	}

	if (strlen(subdirs)) {
		Com_sprintf( search, sizeof(search), "%s/%s", basedir, subdirs );
	}
	else {
		Com_sprintf( search, sizeof(search), "%s", basedir );
	}

	if ((fdir = opendir(search)) == NULL) {
		return;
	}

	while ((d = readdir(fdir)) != NULL) {
		Com_sprintf(filename, sizeof(filename), "%s/%s", search, d->d_name);
		if (stat(filename, &st) == -1)
			continue;

		if (st.st_mode & S_IFDIR) {
			if (Q_stricmp(d->d_name, ".") && Q_stricmp(d->d_name, "..")) {
				if (strlen(subdirs)) {
					Com_sprintf( newsubdirs, sizeof(newsubdirs), "%s/%s", subdirs, d->d_name);
				}
				else {
					Com_sprintf( newsubdirs, sizeof(newsubdirs), "%s", d->d_name);
				}
				Sys_ListFilteredFiles( basedir, newsubdirs, filter, list, numfiles );
			}
		}
		if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
			break;
		}
		Com_sprintf( filename, sizeof(filename), "%s/%s", subdirs, d->d_name );
		if (!Com_FilterPath( filter, filename, qfalse ))
			continue;
		list[ *numfiles ] = CopyString( filename );
		(*numfiles)++;
	}

	closedir(fdir);
}

/*
==================
Sys_ListFiles
==================
*/
char **Sys_ListFiles( const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs )
{
	struct dirent *d;
	DIR           *fdir;
	qboolean      dironly = wantsubs;
	char          search[MAX_OSPATH];
	int           nfiles;
	char          **listCopy;
	char          *list[MAX_FOUND_FILES];
	int           i;
	struct stat   st;

	int           extLen;

	if (filter) {

		nfiles = 0;
		Sys_ListFilteredFiles( directory, "", filter, list, &nfiles );

		list[ nfiles ] = NULL;
		*numfiles = nfiles;

		if (!nfiles)
			return NULL;

		listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( *listCopy ) );
		for ( i = 0 ; i < nfiles ; i++ ) {
			listCopy[i] = list[i];
		}
		listCopy[i] = NULL;

		return listCopy;
	}

	if ( directory[0] == '\0' ) {
		*numfiles = 0;
		return NULL;
	}

	if ( !extension)
		extension = "";

	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		dironly = qtrue;
	}

	extLen = strlen( extension );

	// search
	nfiles = 0;

	if ((fdir = opendir(directory)) == NULL) {
		*numfiles = 0;
		return NULL;
	}

	while ((d = readdir(fdir)) != NULL) {
		Com_sprintf(search, sizeof(search), "%s/%s", directory, d->d_name);
		if (stat(search, &st) == -1)
			continue;
		if ((dironly && !(st.st_mode & S_IFDIR)) ||
			(!dironly && (st.st_mode & S_IFDIR)))
			continue;

		if (*extension) {
			if ( strlen( d->d_name ) < extLen ||
				Q_stricmp(
					d->d_name + strlen( d->d_name ) - extLen,
					extension ) ) {
				continue; // didn't match
			}
		}

		if ( nfiles == MAX_FOUND_FILES - 1 )
			break;
		list[ nfiles ] = CopyString( d->d_name );
		nfiles++;
	}

	list[ nfiles ] = NULL;

	closedir(fdir);

	// return a copy of the list
	*numfiles = nfiles;

	if ( !nfiles ) {
		return NULL;
	}

	listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( *listCopy ) );
	for ( i = 0 ; i < nfiles ; i++ ) {
		listCopy[i] = list[i];
	}
	listCopy[i] = NULL;

	return listCopy;
}

/*
==================
Sys_FreeFileList
==================
*/
void Sys_FreeFileList( char **list )
{
	int i;

	if ( !list ) {
		return;
	}

	for ( i = 0 ; list[i] ; i++ ) {
		Z_Free( list[i] );
	}

	Z_Free( list );
}

/*
==================
Sys_Sleep

Block execution for msec or until input is received.
==================
*/
void Sys_Sleep( int msec )
{
	if( msec == 0 )
		return;

	if( stdinIsATTY )
	{
		fd_set fdset;

		FD_ZERO(&fdset);
		FD_SET(STDIN_FILENO, &fdset);
		if( msec < 0 )
		{
			select(STDIN_FILENO + 1, &fdset, NULL, NULL, NULL);
		}
		else
		{
			struct timeval timeout;

			timeout.tv_sec = msec/1000;
			timeout.tv_usec = (msec%1000)*1000;
			select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout);
		}
	}
	else
	{
		struct timespec req;

		// With nothing to select() on, we can't wait indefinitely
		if( msec < 0 )
			msec = 10;

		req.tv_sec = msec/1000;
		req.tv_nsec = (msec%1000)*1000000;
		nanosleep(&req, NULL);
	}
}

/*
==============
Sys_ErrorDialog

Display an error message
==============
*/
void Sys_ErrorDialog( const char *error )
{
	char buffer[ 1024 ];
	unsigned int size;
	int f = -1;
	const char *homedatapath = Cvar_VariableString( "fs_homedatapath" );
	const char *gamedir = Cvar_VariableString( "fs_game" );
	const char *fileName = "crashlog.txt";
	char *ospath = FS_BuildOSPath( homedatapath, gamedir, fileName );

	Sys_Print( va( "%s\n", error ) );

	Sys_Dialog( DT_ERROR, va( "%s. See \"%s\" for details.", error, ospath ), "Error" );

	// Make sure the write path for the crashlog exists...
	if( FS_CreatePath( homedatapath ) )
	{
		Com_Printf("ERROR: couldn't create path '%s' for crash log.\n", ospath);
		return;
	}

	// We might be crashing because we maxed out the Quake MAX_FILE_HANDLES,
	// which will come through here, so we don't want to recurse forever by
	// calling FS_FOpenFileWrite()...use the Unix system APIs instead.
	f = open( ospath, O_CREAT | O_TRUNC | O_WRONLY, 0640 );
	if( f == -1 )
	{
		Com_Printf( "ERROR: couldn't open %s\n", fileName );
		return;
	}

	// We're crashing, so we don't care much if write() or close() fails.
	while( ( size = CON_LogRead( buffer, sizeof( buffer ) ) ) > 0 ) {
		if( write( f, buffer, size ) != size ) {
			Com_Printf( "ERROR: couldn't fully write to %s\n", fileName );
			break;
		}
	}

	close( f );
}

#ifndef __APPLE__
/*
==============
Sys_ZenityCommand
==============
*/
static void Sys_ZenityCommand( dialogType_t type, const char *message, const char *title )
{
	Sys_ClearExecBuffer( );
	Sys_AppendToExecBuffer( "zenity" );

	switch( type )
	{
		default:
		case DT_INFO:      Sys_AppendToExecBuffer( "--info" ); break;
		case DT_WARNING:   Sys_AppendToExecBuffer( "--warning" ); break;
		case DT_ERROR:     Sys_AppendToExecBuffer( "--error" ); break;
		case DT_YES_NO:
			Sys_AppendToExecBuffer( "--question" );
			Sys_AppendToExecBuffer( "--ok-label=Yes" );
			Sys_AppendToExecBuffer( "--cancel-label=No" );
			break;

		case DT_OK_CANCEL:
			Sys_AppendToExecBuffer( "--question" );
			Sys_AppendToExecBuffer( "--ok-label=OK" );
			Sys_AppendToExecBuffer( "--cancel-label=Cancel" );
			break;
	}

	Sys_AppendToExecBuffer( va( "--text=%s", message ) );
	Sys_AppendToExecBuffer( va( "--title=%s", title ) );
}

/*
==============
Sys_KdialogCommand
==============
*/
static void Sys_KdialogCommand( dialogType_t type, const char *message, const char *title )
{
	Sys_ClearExecBuffer( );
	Sys_AppendToExecBuffer( "kdialog" );

	switch( type )
	{
		default:
		case DT_INFO:      Sys_AppendToExecBuffer( "--msgbox" ); break;
		case DT_WARNING:   Sys_AppendToExecBuffer( "--sorry" ); break;
		case DT_ERROR:     Sys_AppendToExecBuffer( "--error" ); break;
		case DT_YES_NO:    Sys_AppendToExecBuffer( "--warningyesno" ); break;
		case DT_OK_CANCEL: Sys_AppendToExecBuffer( "--warningcontinuecancel" ); break;
	}

	Sys_AppendToExecBuffer( message );
	Sys_AppendToExecBuffer( va( "--title=%s", title ) );
}

/*
==============
Sys_XmessageCommand
==============
*/
static void Sys_XmessageCommand( dialogType_t type, const char *message, const char *title )
{
	Sys_ClearExecBuffer( );
	Sys_AppendToExecBuffer( "xmessage" );
	Sys_AppendToExecBuffer( "-buttons" );

	switch( type )
	{
		default:           Sys_AppendToExecBuffer( "OK:0" ); break;
		case DT_YES_NO:    Sys_AppendToExecBuffer( "Yes:0,No:1" ); break;
		case DT_OK_CANCEL: Sys_AppendToExecBuffer( "OK:0,Cancel:1" ); break;
	}

	Sys_AppendToExecBuffer( "-center" );
	Sys_AppendToExecBuffer( message );
}

/*
==============
Sys_PlatformDialog

Display a *nix dialog box
==============
*/
dialogResult_t Sys_PlatformDialog( dialogType_t type, const char *message, const char *title )
{
	typedef enum
	{
		NONE = 0,
		ZENITY,
		KDIALOG,
		XMESSAGE,
		NUM_DIALOG_PROGRAMS
	} dialogCommandType_t;
	typedef void (*dialogCommandBuilder_t)( dialogType_t, const char *, const char * );

	const char              *session = getenv( "DESKTOP_SESSION" );
	qboolean                tried[ NUM_DIALOG_PROGRAMS ] = { qfalse };
	dialogCommandBuilder_t  commands[ NUM_DIALOG_PROGRAMS ] = { NULL };
	dialogCommandType_t     preferredCommandType = NONE;
	int                     i;

	commands[ ZENITY ] = &Sys_ZenityCommand;
	commands[ KDIALOG ] = &Sys_KdialogCommand;
	commands[ XMESSAGE ] = &Sys_XmessageCommand;

	// This may not be the best way
	if( !Q_stricmp( session, "gnome" ) )
		preferredCommandType = ZENITY;
	else if( !Q_stricmp( session, "kde" ) )
		preferredCommandType = KDIALOG;

	for( i = NONE + 1; i < NUM_DIALOG_PROGRAMS; i++ )
	{
		if( preferredCommandType != NONE && preferredCommandType != i )
			continue;

		if( !tried[ i ] )
		{
			int exitCode;

			commands[ i ]( type, message, title );
			exitCode = Sys_Exec( );

			if( exitCode >= 0 )
			{
				switch( type )
				{
					case DT_YES_NO:    return exitCode ? DR_NO : DR_YES;
					case DT_OK_CANCEL: return exitCode ? DR_CANCEL : DR_OK;
					default:           return DR_OK;
				}
			}

			tried[ i ] = qtrue;

			// The preference failed, so start again in order
			if( preferredCommandType != NONE )
			{
				preferredCommandType = NONE;
				i = NONE + 1;
			}
		}
	}

	Com_DPrintf( S_COLOR_YELLOW "WARNING: failed to show a dialog\n" );
	return DR_OK;
}

#ifdef USE_PROTOCOL_REGISTRATION
#include "desktop_entry.h"

/*
==============
Sys_ReadSmallFile

The file's text, in a buffer of size, which must hold all of it; qfalse
if it can't be read or is too big
==============
*/
static qboolean Sys_ReadSmallFile( const char *path, char *buffer, int size )
{
	FILE *f = fopen( path, "rb" );
	size_t length;

	if( !f )
		return qfalse;

	length = fread( buffer, 1, size, f );
	fclose( f );
	if( length >= (size_t)size )
		return qfalse;

	buffer[ length ] = '\0';
	return qtrue;
}

/*
==============
Sys_RegisterProtocolHandler

Puts the client's desktop entry, which names the link schemes it opens
(misc/linux/client.desktop.in, DESKTOP_ENTRY_FORMAT), in the user's
applications, with this executable's path, and the icon beside it if it's
there, so desktops open the links with it. The entry is the game's by its
name, so the copy that ran last has the links; a desktop offers them to
the other programs that have them too.
==============
*/
void Sys_RegisterProtocolHandler( void )
{
	char exe[ MAX_OSPATH ], icon[ MAX_OSPATH ], apps[ MAX_OSPATH ], path[ MAX_OSPATH ], temp[ MAX_OSPATH ];
	char entry[ 4096 ], current[ 4096 ];
	char *slash;
	ssize_t length = readlink( "/proc/self/exe", exe, sizeof( exe ) );
	FILE *f;
	qboolean ok;
	pid_t pid;
	char *argv[] = { "sh", "-c", "update-desktop-database \"$1\" > /dev/null 2>&1 &", "sh", apps, NULL };
	extern char **environ;

	if( length <= 0 || (size_t)length >= sizeof( exe ) )
		return;
	exe[ length ] = '\0';

	// Exec quotes the path, in which these would need escapes
	if( strpbrk( exe, "\"`$\\%\n" ) )
	{
		Com_DPrintf( "Not registering links: the path has characters a desktop entry would escape\n" );
		return;
	}

	// dirname() changes what it's given
	Q_strncpyz( temp, exe, sizeof( temp ) );
	Com_sprintf( icon, sizeof( icon ), "%s/" APP_ID ".png", Sys_Dirname( temp ) );
	if( access( icon, R_OK ) )
		Q_strncpyz( icon, APP_ID, sizeof( icon ) );
	Com_sprintf( entry, sizeof( entry ), DESKTOP_ENTRY_FORMAT, exe, icon );

	Sys_XDGPath( apps, sizeof( apps ), "XDG_DATA_HOME", ".local/share", "applications" );
	if( !*apps )
		return;
	Com_sprintf( path, sizeof( path ), "%s/" APP_ID ".desktop", apps );
	if( Sys_ReadSmallFile( path, current, sizeof( current ) ) && !strcmp( current, entry ) )
		return;

	// as FS_CreatePath does, but its failure is fatal, and this one mustn't be
	for( slash = strchr( apps + 1, '/' ); slash; slash = strchr( slash + 1, '/' ) )
	{
		*slash = '\0';
		ok = Sys_Mkdir( apps );
		*slash = '/';
		if( !ok )
			return;
	}
	if( !Sys_Mkdir( apps ) )
		return;

	// a whole entry or none, for the desktop reading it, and for another
	// copy starting at the same time, which writes its own
	Com_sprintf( temp, sizeof( temp ), "%s.%d.tmp", path, (int)getpid( ) );
	f = fopen( temp, "wb" );
	if( !f )
		return;
	ok = fputs( entry, f ) >= 0;
	if( fclose( f ) || !ok || rename( temp, path ) )
	{
		unlink( temp );
		Com_DPrintf( "Not registering links: can't write %s\n", path );
		return;
	}
	Com_Printf( "Registered the links this client opens (" PROTOCOL_HANDLER ") in %s\n", path );

	// the desktops that read the entries' MIME types from its cache, where
	// it's installed, in the background: it reads every entry there. Through
	// posix_spawnp, since a fork would run our atexit handlers
	if( !posix_spawnp( &pid, argv[ 0 ], NULL, NULL, argv, environ ) )
		waitpid( pid, NULL, 0 );
}
#endif
#endif

/*
==============
Sys_GLimpSafeInit

Unix specific "safe" GL implementation initialisation
==============
*/
void Sys_GLimpSafeInit( void )
{
	// NOP
}

/*
==============
Sys_GLimpInit

Unix specific GL implementation initialisation
==============
*/
void Sys_GLimpInit( void )
{
	// NOP
}

void Sys_SetFloatEnv(void)
{
	// rounding toward nearest
	fesetround(FE_TONEAREST);
}

/*
==============
Sys_PlatformInit

Unix specific initialisation
==============
*/
void Sys_PlatformInit( void )
{
	const char* term = getenv( "TERM" );

	signal( SIGHUP, Sys_SigHandler );
	signal( SIGQUIT, Sys_SigHandler );
	signal( SIGTRAP, Sys_SigHandler );
	signal( SIGABRT, Sys_SigHandler );
	signal( SIGBUS, Sys_SigHandler );

	Sys_SetFloatEnv();

	stdinIsATTY = isatty( STDIN_FILENO ) &&
		!( term && ( !strcmp( term, "raw" ) || !strcmp( term, "dumb" ) ) );
}

/*
==============
Sys_PlatformExit

Unix specific deinitialisation
==============
*/
void Sys_PlatformExit( void )
{
}

/*
==============
Sys_SetEnv

set/unset environment variables (empty value removes it)
==============
*/

void Sys_SetEnv(const char *name, const char *value)
{
	if(value && *value)
		setenv(name, value, 1);
	else
		unsetenv(name);
}

/*
==============
Sys_PID
==============
*/
int Sys_PID( void )
{
	return getpid( );
}

/*
==============
Sys_PIDIsRunning
==============
*/
qboolean Sys_PIDIsRunning( int pid )
{
	return kill( pid, 0 ) == 0;
}

/*
=================
Sys_DllExtension

Check if filename should be allowed to be loaded as a DLL.
=================
*/
qboolean Sys_DllExtension( const char *name ) {
	const char *p;
	char c = 0;

	if ( COM_CompareExtension( name, DLL_EXT ) ) {
		return qtrue;
	}

#ifdef __APPLE__
	// Allow system frameworks without dylib extensions
	// i.e., /System/Library/Frameworks/OpenAL.framework/OpenAL
	if ( strncmp( name, "/System/Library/Frameworks/", 27 ) == 0 ) {
		return qtrue;
	}
#endif

	// Check for format of filename.so.1.2.3
	p = strstr( name, DLL_EXT "." );

	if ( p ) {
		p += strlen( DLL_EXT );

		// Check if .so is only followed for periods and numbers.
		while ( *p ) {
			c = *p;

			if ( !isdigit( c ) && c != '.' ) {
				return qfalse;
			}

			p++;
		}

		// Don't allow filename to end in a period. file.so., file.so.0., etc
		if ( c != '.' ) {
			return qtrue;
		}
	}

	return qfalse;
}

/*
==============
Sys_InModalLoop
==============
*/
qboolean Sys_InModalLoop( void )
{
	return qfalse;
}

/*
==============
Sys_OpenFolderInPlatformFileManager
==============
*/
qboolean Sys_OpenFolderInPlatformFileManager( const char *path )
{
	Sys_ClearExecBuffer( );

#ifdef __APPLE__
	Sys_AppendToExecBuffer( "open" );
#else
	Sys_AppendToExecBuffer( "xdg-open" );
#endif

	Sys_AppendToExecBuffer( path );

	return Sys_Exec( ) == 0;
}

/*
=================
Sys_SetMaxFileLimit
=================
*/
qboolean Sys_SetMaxFileLimit( void )
{
#ifdef RLIMIT_NOFILE
	struct rlimit limit;

	// Get the current open file limit
	if( getrlimit( RLIMIT_NOFILE, &limit ) == 0 )
	{
		// Set the file limit to the maximum
		limit.rlim_cur = limit.rlim_max;
		if( setrlimit( RLIMIT_NOFILE, &limit ) == 0 )
			return qtrue;
		else
			Com_DPrintf( S_COLOR_YELLOW "WARNING: setrlimit (rlim_max) failed\n" );

#ifdef OPEN_MAX
		// On older macOS versions an error can happen trying to set a file limit above
		// OPEN_MAX. If we see an error, then try again with OPEN_MAX as the limit.
		limit.rlim_cur = OPEN_MAX;
		if( setrlimit( RLIMIT_NOFILE, &limit ) == 0 )
			return qtrue;
		else
			Com_DPrintf( S_COLOR_YELLOW "WARNING: setrlimit (OPEN_MAX) failed\n" );
#endif
	}
	else
		Com_DPrintf( S_COLOR_YELLOW "WARNING: getrlimit failed\n" );

#endif // RLIMIT_NOFILE

	return qfalse;
}

/*
=================
Sys_Notify

Tells the service manager that started the program its state, as systemd's
sd_notify does ("READY=1", "STOPPING=1"), where NOTIFY_SOCKET names its
socket; nothing otherwise. A datagram to that socket, without libsystemd
=================
*/
void Sys_Notify( const char *state )
{
#if defined( __linux__ ) && !defined( __EMSCRIPTEN__ )
	const char			*path = getenv( "NOTIFY_SOCKET" );
	struct sockaddr_un	address;
	size_t				length;
	int					fd;

	if( !path || ( path[0] != '/' && path[0] != '@' ) )
		return;
	length = strlen( path );
	if( length >= sizeof( address.sun_path ) )
		return;

	memset( &address, 0, sizeof( address ) );
	address.sun_family = AF_UNIX;
	memcpy( address.sun_path, path, length );
	// an abstract socket's name starts with a 0 byte
	if( address.sun_path[0] == '@' )
		address.sun_path[0] = '\0';

	fd = socket( AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0 );
	if( fd < 0 )
		return;
	sendto( fd, state, strlen( state ), MSG_NOSIGNAL, (struct sockaddr *)&address,
		offsetof( struct sockaddr_un, sun_path ) + length );
	close( fd );
#endif
}
