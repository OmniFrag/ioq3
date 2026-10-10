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

// Use EnumProcesses() with Windows XP compatibility
#define PSAPI_VERSION 1

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

#include <windows.h>
#include <lmerr.h>
#include <lmcons.h>
#include <lmwksta.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <direct.h>
#include <io.h>
#include <conio.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <psapi.h>
// netfw.h's GUIDs, which no import library has, are defined here
#include <netfw.h>
#include <float.h>

#ifndef KEY_WOW64_32KEY
#define KEY_WOW64_32KEY 0x0200
#endif

static UINT timerResolution = 0;

#ifndef DEDICATED
// A laptop with a second, discrete GPU (NVIDIA Optimus, AMD PowerXpress)
// runs a program's OpenGL on its integrated GPU unless the program's
// executable exports these, set to 1
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

/*
================
Sys_SetFPUCW
Set FPU control word to default value
================
*/

#ifndef _RC_CHOP
// mingw doesn't seem to have these defined :(

  #define _MCW_EM	0x0008001fU
  #define _MCW_RC	0x00000300U
  #define _MCW_PC	0x00030000U
  #define _RC_NEAR      0x00000000U
  #define _PC_53	0x00010000U
  
  unsigned int _controlfp(unsigned int new, unsigned int mask);
#endif

#define FPUCWMASK1 (_MCW_RC | _MCW_EM)
#define FPUCW (_RC_NEAR | _MCW_EM | _PC_53)

#if idx64
#define FPUCWMASK	(FPUCWMASK1)
#else
#define FPUCWMASK	(FPUCWMASK1 | _MCW_PC)
#endif

void Sys_SetFloatEnv(void)
{
	_controlfp(FPUCW, FPUCWMASK);
}

/*
================
Sys_AppDataPath

The directory name in the user's application data, or qfalse if Windows
can't say where that is
================
*/
static qboolean Sys_AppDataPath( char *path, int size, const char *name )
{
	TCHAR szPath[MAX_PATH];

	if( !SUCCEEDED( SHGetFolderPathA( NULL, CSIDL_APPDATA,
					NULL, 0, szPath ) ) )
	{
		Com_Printf("Unable to detect CSIDL_APPDATA\n");
		return qfalse;
	}

	Com_sprintf(path, size, "%s%c%s", szPath, PATH_SEP, name);
	return qtrue;
}

/*
================
Sys_DefaultHomePath
================
*/
static char *Sys_DefaultHomePath( void )
{
	static char homePath[ MAX_OSPATH ] = { 0 };

	if(!*homePath && com_homepath)
	{
		if( !Sys_AppDataPath( homePath, sizeof( homePath ),
				com_homepath->string[0] ? com_homepath->string : HOMEPATH_NAME ) )
			return NULL;
	}

	return homePath;
}

char *Sys_DefaultHomeConfigPath(void) { return Sys_DefaultHomePath(); }
char *Sys_DefaultHomeDataPath(void)   { return Sys_DefaultHomePath(); }
char *Sys_DefaultHomeStatePath(void)  { return Sys_DefaultHomePath(); }

/*
================
Sys_PredecessorHomePaths

The home directory of the product this build succeeds, for both its config
and its data, named by HOMEPATH_NAME_PREDECESSOR, in the same place as
ours; "" where the build names none, or it doesn't exist
================
*/
void Sys_PredecessorHomePaths( const char **configPath, const char **dataPath )
{
	static char predPath[ MAX_OSPATH ];

#ifdef HOMEPATH_NAME_PREDECESSOR
	DWORD attributes;

	if( Sys_AppDataPath( predPath, sizeof( predPath ), HOMEPATH_NAME_PREDECESSOR ) )
	{
		attributes = GetFileAttributesA( predPath );
		if( attributes == INVALID_FILE_ATTRIBUTES || !( attributes & FILE_ATTRIBUTE_DIRECTORY ) )
			predPath[0] = '\0';
	}
#endif

	*configPath = *dataPath = predPath;
}

/*
================
Sys_SteamPath
================
*/
char *Sys_SteamPath( void )
{
#ifndef STANDALONE

#define STEAMPATH_NAME "Quake 3 Arena"
#define STEAMPATH_APPID "2200"

	static char steamPath[ MAX_OSPATH ] = { 0 };

	HKEY steamRegKey;
	DWORD pathLen = MAX_OSPATH;
	qboolean finishPath = qfalse;

	// Assuming Steam is a 32-bit app
	if (!steamPath[0] && !RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App "
		STEAMPATH_APPID, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &steamRegKey))
	{
		pathLen = MAX_OSPATH;
		if (RegQueryValueEx(steamRegKey, "InstallLocation", NULL, NULL, (LPBYTE)steamPath, &pathLen))
			steamPath[0] = '\0';

		RegCloseKey(steamRegKey);
	}

	if (!steamPath[0] && !RegOpenKeyEx(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &steamRegKey))
	{
		pathLen = MAX_OSPATH;
		if (RegQueryValueEx(steamRegKey, "SteamPath", NULL, NULL, (LPBYTE)steamPath, &pathLen))
			if (RegQueryValueEx(steamRegKey, "InstallPath", NULL, NULL, (LPBYTE)steamPath, &pathLen))
				steamPath[0] = '\0';

		if (steamPath[0])
			finishPath = qtrue;

		RegCloseKey(steamRegKey);
	}

	if (steamPath[0])
	{
		if (pathLen == MAX_OSPATH)
			pathLen--;

		steamPath[pathLen] = '\0';

		if (finishPath)
			Q_strcat(steamPath, MAX_OSPATH, "\\SteamApps\\common\\" STEAMPATH_NAME );
	}

	return steamPath;
#else
	return "";
#endif
}

/*
================
Sys_GogPath
================
*/
char *Sys_GogPath( void )
{
#ifndef STANDALONE

#define GOGPATH_ID "1441704920"

	static char gogPath[ MAX_OSPATH ] = { 0 };

	HKEY gogRegKey;
	DWORD pathLen = MAX_OSPATH;

	if (!gogPath[0] && !RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\GOG.com\\Games\\" GOGPATH_ID, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &gogRegKey))
	{
		pathLen = MAX_OSPATH;
		if (RegQueryValueEx(gogRegKey, "PATH", NULL, NULL, (LPBYTE)gogPath, &pathLen))
			gogPath[0] = '\0';

		RegCloseKey(gogRegKey);
	}

	if (gogPath[0])
	{
		if (pathLen == MAX_OSPATH)
			pathLen--;

		gogPath[pathLen] = '\0';
	}

	return gogPath;
#else
	return "";
#endif
}

/*
================
Sys_MicrosoftStorePath
================
*/
char* Sys_MicrosoftStorePath(void)
{
#ifndef STANDALONE

#define MSSTORE_PATH "Quake 3"

	static char microsoftStorePath[MAX_OSPATH] = { 0 };

	if (!microsoftStorePath[0]) 
	{
		TCHAR szPath[MAX_PATH];

		if( !SUCCEEDED( SHGetFolderPathA( NULL, CSIDL_PROGRAM_FILES,
						NULL, 0, szPath ) ) )
		{
			Com_Printf("Unable to detect CSIDL_PROGRAM_FILES\n");
			return microsoftStorePath;
		}

		// default: C:\Program Files\ModifiableWindowsApps\Quake 3\EN
		Com_sprintf(microsoftStorePath, sizeof(microsoftStorePath), "%s%cModifiableWindowsApps%c%s%cEN", szPath, PATH_SEP, PATH_SEP, MSSTORE_PATH, PATH_SEP);
	}

	return microsoftStorePath;
#else
	return "";
#endif
}

/*
================
Sys_Nanoseconds

The time since the first call, by the performance counter
================
*/
int64_t Sys_Nanoseconds( void )
{
	static LARGE_INTEGER	frequency, base;
	LARGE_INTEGER			now;
	int64_t					ticks;

	QueryPerformanceCounter( &now );

	if( !frequency.QuadPart )
	{
		QueryPerformanceFrequency( &frequency );
		base = now;
	}

	// in two parts, so the multiplication can't overflow
	ticks = now.QuadPart - base.QuadPart;
	return ticks / frequency.QuadPart * 1000000000 +
		ticks % frequency.QuadPart * 1000000000 / frequency.QuadPart;
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
================
Sys_RandomBytes
================
*/
qboolean Sys_RandomBytes( byte *string, int len )
{
	HCRYPTPROV  prov;

	if( !CryptAcquireContext( &prov, NULL, NULL,
		PROV_RSA_FULL, CRYPT_VERIFYCONTEXT ) )  {

		return qfalse;
	}

	if( !CryptGenRandom( prov, len, (BYTE *)string ) )  {
		CryptReleaseContext( prov, 0 );
		return qfalse;
	}
	CryptReleaseContext( prov, 0 );
	return qtrue;
}

/*
================
Sys_GetCurrentUser
================
*/
char *Sys_GetCurrentUser( void )
{
	static char s_userName[1024];
	unsigned long size = sizeof( s_userName );

	if( !GetUserName( s_userName, &size ) )
		strcpy( s_userName, "player" );

	if( !s_userName[0] )
	{
		strcpy( s_userName, "player" );
	}

	return s_userName;
}

#define MEM_THRESHOLD 96*1024*1024

/*
==================
Sys_LowPhysicalMemory
==================
*/
qboolean Sys_LowPhysicalMemory( void )
{
	MEMORYSTATUS stat;
	GlobalMemoryStatus (&stat);
	return (stat.dwTotalPhys <= MEM_THRESHOLD) ? qtrue : qfalse;
}

/*
==================
Sys_KeepAwake
==================
*/
qboolean Sys_KeepAwake( qboolean awake )
{
	return qfalse;
}

/*
==============
Sys_Basename
==============
*/
const char *Sys_Basename( char *path )
{
	static char base[ MAX_OSPATH ] = { 0 };
	int length;

	length = strlen( path ) - 1;

	// Skip trailing slashes
	while( length > 0 && path[ length ] == '\\' )
		length--;

	while( length > 0 && path[ length - 1 ] != '\\' )
		length--;

	Q_strncpyz( base, &path[ length ], sizeof( base ) );

	length = strlen( base ) - 1;

	// Strip trailing slashes
	while( length > 0 && base[ length ] == '\\' )
    base[ length-- ] = '\0';

	return base;
}

/*
==============
Sys_Dirname
==============
*/
const char *Sys_Dirname( char *path )
{
	static char dir[ MAX_OSPATH ] = { 0 };
	int length;

	Q_strncpyz( dir, path, sizeof( dir ) );
	length = strlen( dir ) - 1;

	while( length > 0 && dir[ length ] != '\\' )
		length--;

	dir[ length ] = '\0';

	return dir;
}

/*
==============
Sys_FOpen
==============
*/
FILE *Sys_FOpen( const char *ospath, const char *mode ) {
	size_t length;

	// Windows API ignores all trailing spaces and periods which can get around Quake 3 file system restrictions.
	length = strlen( ospath );
	if ( length == 0 || ospath[length-1] == ' ' || ospath[length-1] == '.' ) {
		return NULL;
	}

	return fopen( ospath, mode );
}

/*
==============
Sys_Mkdir
==============
*/
qboolean Sys_Mkdir( const char *path )
{
	if( !CreateDirectory( path, NULL ) )
	{
		if( GetLastError( ) != ERROR_ALREADY_EXISTS )
			return qfalse;
	}

	return qtrue;
}

/*
==================
Sys_Mkfifo
Noop on windows because named pipes do not function the same way
==================
*/
FILE *Sys_Mkfifo( const char *ospath )
{
	return NULL;
}

/*
==============
Sys_Cwd
==============
*/
char *Sys_Cwd( void ) {
	static char cwd[MAX_OSPATH];

	_getcwd( cwd, sizeof( cwd ) - 1 );
	cwd[MAX_OSPATH-1] = 0;

	return cwd;
}

/*
==============
Sys_BinaryPathRelative
==============
*/
char *Sys_BinaryPathRelative(const char *relative)
{
	static char resolved[MAX_OSPATH];
	char combined[MAX_OSPATH];

	snprintf(combined, sizeof(combined), "%s\\%s", Sys_BinaryPath(), relative);

	DWORD len = GetFullPathNameA(combined, MAX_OSPATH, resolved, NULL);
	if (len == 0 || len >= MAX_OSPATH)
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
==============
Sys_ListFilteredFiles
==============
*/
void Sys_ListFilteredFiles( const char *basedir, char *subdirs, char *filter, char **list, int *numfiles )
{
	char		search[MAX_OSPATH], newsubdirs[MAX_OSPATH];
	char		filename[MAX_OSPATH];
	intptr_t	findhandle;
	struct _finddata_t findinfo;

	if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
		return;
	}

	if ( basedir[0] == '\0' ) {
		return;
	}

	if (strlen(subdirs)) {
		Com_sprintf( search, sizeof(search), "%s\\%s\\*", basedir, subdirs );
	}
	else {
		Com_sprintf( search, sizeof(search), "%s\\*", basedir );
	}

	findhandle = _findfirst (search, &findinfo);
	if (findhandle == -1) {
		return;
	}

	do {
		if (findinfo.attrib & _A_SUBDIR) {
			if (Q_stricmp(findinfo.name, ".") && Q_stricmp(findinfo.name, "..")) {
				if (strlen(subdirs)) {
					Com_sprintf( newsubdirs, sizeof(newsubdirs), "%s\\%s", subdirs, findinfo.name);
				}
				else {
					Com_sprintf( newsubdirs, sizeof(newsubdirs), "%s", findinfo.name);
				}
				Sys_ListFilteredFiles( basedir, newsubdirs, filter, list, numfiles );
			}
		}
		if ( *numfiles >= MAX_FOUND_FILES - 1 ) {
			break;
		}
		Com_sprintf( filename, sizeof(filename), "%s\\%s", subdirs, findinfo.name );
		if (!Com_FilterPath( filter, filename, qfalse ))
			continue;
		list[ *numfiles ] = CopyString( filename );
		(*numfiles)++;
	} while ( _findnext (findhandle, &findinfo) != -1 );

	_findclose (findhandle);
}

/*
==============
strgtr
==============
*/
static qboolean strgtr(const char *s0, const char *s1)
{
	int l0, l1, i;

	l0 = strlen(s0);
	l1 = strlen(s1);

	if (l1<l0) {
		l0 = l1;
	}

	for(i=0;i<l0;i++) {
		if (s1[i] > s0[i]) {
			return qtrue;
		}
		if (s1[i] < s0[i]) {
			return qfalse;
		}
	}
	return qfalse;
}

/*
==============
Sys_ListFiles
==============
*/
char **Sys_ListFiles( const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs )
{
	char		search[MAX_OSPATH];
	int			nfiles;
	char		**listCopy;
	char		*list[MAX_FOUND_FILES];
	struct _finddata_t findinfo;
	intptr_t		findhandle;
	int			flag;
	int			i;
	int			extLen;

	if (filter) {

		nfiles = 0;
		Sys_ListFilteredFiles( directory, "", filter, list, &nfiles );

		list[ nfiles ] = 0;
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

	if ( !extension) {
		extension = "";
	}

	// passing a slash as extension will find directories
	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		flag = 0;
	} else {
		flag = _A_SUBDIR;
	}

	extLen = strlen( extension );

	Com_sprintf( search, sizeof(search), "%s\\*%s", directory, extension );

	// search
	nfiles = 0;

	findhandle = _findfirst (search, &findinfo);
	if (findhandle == -1) {
		*numfiles = 0;
		return NULL;
	}

	do {
		if ( (!wantsubs && flag ^ ( findinfo.attrib & _A_SUBDIR )) || (wantsubs && findinfo.attrib & _A_SUBDIR) ) {
			if (*extension) {
				if ( strlen( findinfo.name ) < extLen ||
					Q_stricmp(
						findinfo.name + strlen( findinfo.name ) - extLen,
						extension ) ) {
					continue; // didn't match
				}
			}
			if ( nfiles == MAX_FOUND_FILES - 1 ) {
				break;
			}
			list[ nfiles ] = CopyString( findinfo.name );
			nfiles++;
		}
	} while ( _findnext (findhandle, &findinfo) != -1 );

	list[ nfiles ] = 0;

	_findclose (findhandle);

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

	do {
		flag = 0;
		for(i=1; i<nfiles; i++) {
			if (strgtr(listCopy[i-1], listCopy[i])) {
				char *temp = listCopy[i];
				listCopy[i] = listCopy[i-1];
				listCopy[i-1] = temp;
				flag = 1;
			}
		}
	} while(flag);

	return listCopy;
}

/*
==============
Sys_FreeFileList
==============
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
==============
Sys_Sleep

Block execution for msec or until input is received.
==============
*/
void Sys_Sleep( int msec )
{
	if( msec == 0 )
		return;

#ifdef DEDICATED
	if( msec < 0 )
		WaitForSingleObject( GetStdHandle( STD_INPUT_HANDLE ), INFINITE );
	else
		WaitForSingleObject( GetStdHandle( STD_INPUT_HANDLE ), msec );
#else
	// Client Sys_Sleep doesn't support waiting on stdin
	if( msec < 0 )
		return;

	Sleep( msec );
#endif
}

/*
==============
Sys_ErrorDialog

Display an error message
==============
*/
void Sys_ErrorDialog( const char *error )
{
	Sys_Print( va( "%s\n", error ) );

	if( Sys_Dialog( DT_YES_NO, va( "%s. Copy console log to clipboard?", error ),
			"Error" ) == DR_YES )
	{
		HGLOBAL memoryHandle;
		char *clipMemory;

		memoryHandle = GlobalAlloc( GMEM_MOVEABLE|GMEM_DDESHARE, CON_LogSize( ) + 1 );
		clipMemory = (char *)GlobalLock( memoryHandle );

		if( clipMemory )
		{
			char *p = clipMemory;
			char buffer[ 1024 ];
			unsigned int size;

			while( ( size = CON_LogRead( buffer, sizeof( buffer ) ) ) > 0 )
			{
				Com_Memcpy( p, buffer, size );
				p += size;
			}

			*p = '\0';

			if( OpenClipboard( NULL ) && EmptyClipboard( ) )
				SetClipboardData( CF_TEXT, memoryHandle );

			GlobalUnlock( clipMemory );
			CloseClipboard( );
		}
	}
}

/*
==============
Sys_PlatformDialog

Display a win32 dialog box
==============
*/
dialogResult_t Sys_PlatformDialog( dialogType_t type, const char *message, const char *title )
{
	UINT uType;

	switch( type )
	{
		default:
		case DT_INFO:      uType = MB_ICONINFORMATION|MB_OK; break;
		case DT_WARNING:   uType = MB_ICONWARNING|MB_OK; break;
		case DT_ERROR:     uType = MB_ICONERROR|MB_OK; break;
		case DT_YES_NO:    uType = MB_ICONQUESTION|MB_YESNO; break;
		case DT_OK_CANCEL: uType = MB_ICONWARNING|MB_OKCANCEL; break;
	}

	switch( MessageBox( NULL, message, title, uType ) )
	{
		default:
		case IDOK:      return DR_OK;
		case IDCANCEL:  return DR_CANCEL;
		case IDYES:     return DR_YES;
		case IDNO:      return DR_NO;
	}
}

/*
==============
Sys_GLimpSafeInit

Windows specific "safe" GL implementation initialisation
==============
*/
void Sys_GLimpSafeInit( void )
{
}

/*
==============
Sys_GLimpInit

Windows specific GL implementation initialisation
==============
*/
void Sys_GLimpInit( void )
{
}

#ifdef USE_PROTOCOL_REGISTRATION
#include <shellapi.h>

/*
==============
Sys_FormatW

_snwprintf, always terminated
==============
*/
static void Sys_FormatW( wchar_t *buffer, size_t size, const wchar_t *format, ... )
{
	va_list args;

	va_start( args, format );
	_vsnwprintf( buffer, size, format, args );
	va_end( args );
	buffer[ size - 1 ] = L'\0';
}

/*
==============
Sys_CommandProgramExists

Whether the program a registered command runs is still installed. Only a
path on a local drive is checked: a program named without one (found on
the PATH, such as rundll32.exe), or one only over the network, which can
take as long as a timeout, counts as installed.
==============
*/
static qboolean Sys_CommandProgramExists( const wchar_t *command )
{
	int argc;
	wchar_t **argv;
	qboolean exists = qtrue;

	// CommandLineToArgvW gives an empty command this executable
	if( !*command )
		return qfalse;

	argv = CommandLineToArgvW( command, &argc );
	if( !argv )
		return qtrue;

	if( argc > 0 && argv[ 0 ][ 0 ] && argv[ 0 ][ 1 ] == L':' )
	{
		wchar_t root[] = { argv[ 0 ][ 0 ], L':', L'\\', L'\0' };

		if( GetDriveTypeW( root ) != DRIVE_REMOTE )
			exists = GetFileAttributesW( argv[ 0 ] ) != INVALID_FILE_ATTRIBUTES;
	}

	LocalFree( argv );
	return exists;
}

/*
==============
Sys_SetRegistryString
==============
*/
static qboolean Sys_SetRegistryString( HKEY parent, const wchar_t *key, const wchar_t *name, const wchar_t *value )
{
	return RegSetKeyValueW( parent, key, name, REG_SZ, value,
		( wcslen( value ) + 1 ) * sizeof( wchar_t ) ) == ERROR_SUCCESS;
}

/*
==============
Sys_RegisterProtocolHandler

Registers the link schemes for the user, in HKEY_CURRENT_USER, which needs
no administrator, to open with this executable: a copy of the game has
them after it runs, but a scheme stays with another program that is still
installed.
==============
*/
void Sys_RegisterProtocolHandler( void )
{
	wchar_t exe[ MAX_PATH ], command[ MAX_PATH + 32 ], icon[ MAX_PATH + 8 ], name[ 64 ];
	const char *scheme;
	int schemeLength;
	HKEY classes;
	DWORD length = GetModuleFileNameW( NULL, exe, ARRAY_LEN( exe ) );

	if( length == 0 || length >= ARRAY_LEN( exe ) )
		return;

	// the user's classes: HKEY_CLASSES_ROOT reads them over the machine's
	if( RegCreateKeyExW( HKEY_CURRENT_USER, L"Software\\Classes", 0, NULL, 0,
			KEY_WRITE, NULL, &classes, NULL ) != ERROR_SUCCESS )
		return;

	Sys_FormatW( command, ARRAY_LEN( command ), L"\"%ls\" --uri \"%%1\"", exe );
	Sys_FormatW( icon, ARRAY_LEN( icon ), L"\"%ls\",0", exe );
	Sys_FormatW( name, ARRAY_LEN( name ), L"URL:%hs", PRODUCT_NAME );

	for( scheme = Sys_NextProtocolScheme( PROTOCOL_HANDLER, &schemeLength ); scheme;
		scheme = Sys_NextProtocolScheme( scheme + schemeLength, &schemeLength ) )
	{
		wchar_t key[ 64 ], openKey[ 96 ], iconKey[ 96 ], current[ MAX_PATH + 32 ], owner[ 64 ];
		DWORD size = sizeof( current );
		LONG status;

		Sys_FormatW( key, ARRAY_LEN( key ), L"%.*hs", schemeLength, scheme );
		Sys_FormatW( openKey, ARRAY_LEN( openKey ), L"%ls\\shell\\open\\command", key );
		Sys_FormatW( iconKey, ARRAY_LEN( iconKey ), L"%ls\\DefaultIcon", key );

		// the scheme's program now, the user's or the machine's; a
		// REG_EXPAND_SZ comes back expanded, as a REG_SZ (asking for
		// RRF_RT_REG_EXPAND_SZ without RRF_NOEXPAND is refused)
		status = RegGetValueW( HKEY_CLASSES_ROOT, openKey, NULL,
			RRF_RT_REG_SZ, NULL, current, &size );
		if( status == ERROR_SUCCESS )
		{
			if( !wcscmp( current, command ) )
				continue;

			// a copy of the game's, by its name, or one that's gone
			size = sizeof( owner );
			if( ( RegGetValueW( HKEY_CLASSES_ROOT, key, NULL, RRF_RT_REG_SZ,
					NULL, owner, &size ) != ERROR_SUCCESS || wcscmp( owner, name ) )
				&& Sys_CommandProgramExists( current ) )
			{
				Com_DPrintf( "Not registering %ls: another program has it\n", key );
				continue;
			}
		}
		else if( status != ERROR_FILE_NOT_FOUND )
			continue;

		if( Sys_SetRegistryString( classes, key, NULL, name )
			&& Sys_SetRegistryString( classes, key, L"URL Protocol", L"" )
			&& Sys_SetRegistryString( classes, iconKey, NULL, icon )
			&& Sys_SetRegistryString( classes, openKey, NULL, command ) )
			Com_Printf( "Registered the links this client opens: %ls\n", key );
	}

	RegCloseKey( classes );
}
#endif

#ifdef DEDICATED
/*
==============
Sys_KeepTimerResolution

Windows 11 ignores a process's timer resolution while it has no window
anyone sees, and a dedicated server's frames then come on the default
15.6 ms ticks whatever it asked for. Ask it not to, where it can be asked
(SetProcessInformation, Windows 8 and later)
==============
*/
static void Sys_KeepTimerResolution( void )
{
	typedef BOOL (WINAPI *setProcessInformation_t)( HANDLE, int, LPVOID, DWORD );
	// PROCESS_POWER_THROTTLING_STATE, ProcessPowerThrottling and
	// PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, which older SDKs lack
	struct { ULONG version, controlMask, stateMask; } state = { 1, 0x4, 0 };
	setProcessInformation_t setProcessInformation = (setProcessInformation_t)(void *)
		GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "SetProcessInformation" );

	if( setProcessInformation )
		setProcessInformation( GetCurrentProcess( ), 4, &state, sizeof( state ) );
}
#endif

/*
==============
Sys_PlatformInit

Windows specific initialisation
==============
*/
void Sys_PlatformInit( void )
{
	TIMECAPS ptc;

	Sys_SetFloatEnv();

	// The dedicated server needs it as much as the client: it sleeps in
	// NET_Sleep between its frames, which at the default resolution of
	// about 15.6 ms wake late enough to put an sv_fps 40 server's frames
	// and snapshots off by several ms
	if(timeGetDevCaps(&ptc, sizeof(ptc)) == MMSYSERR_NOERROR)
	{
		timerResolution = ptc.wPeriodMin;

		if(timerResolution > 1)
		{
			Com_Printf("Warning: Minimum supported timer resolution is %ums "
				"on this system, recommended resolution 1ms\n", timerResolution);
		}
		
		timeBeginPeriod(timerResolution);				
	}
	else
		timerResolution = 0;

#ifdef DEDICATED
	Sys_KeepTimerResolution();
#endif
}

/*
==============
Sys_PlatformExit

Windows specific initialisation
==============
*/
void Sys_PlatformExit( void )
{
	if(timerResolution)
		timeEndPeriod(timerResolution);
}

/*
==============
Sys_SetEnv

set/unset environment variables (empty value removes it)
==============
*/
void Sys_SetEnv(const char *name, const char *value)
{
	if(value)
		_putenv(va("%s=%s", name, value));
	else
		_putenv(va("%s=", name));
}

/*
==============
Sys_PID
==============
*/
int Sys_PID( void )
{
	return GetCurrentProcessId( );
}

/*
==============
Sys_PIDIsRunning
==============
*/
qboolean Sys_PIDIsRunning( int pid )
{
	DWORD processes[ 1024 ];
	DWORD numBytes, numProcesses;
	int i;

	if( !EnumProcesses( processes, sizeof( processes ), &numBytes ) )
		return qfalse; // Assume it's not running

	numProcesses = numBytes / sizeof( DWORD );

	// Search for the pid
	for( i = 0; i < numProcesses; i++ )
	{
		if( (int)processes[ i ] == pid )
			return qtrue;
	}

	return qfalse;
}

/*
=================
Sys_DllExtension

Check if filename should be allowed to be loaded as a DLL.
=================
*/
qboolean Sys_DllExtension( const char *name ) {
	return COM_CompareExtension( name, DLL_EXT );
}

/*
==============
Sys_OpenFolderInPlatformFileManager
==============
*/
qboolean Sys_OpenFolderInPlatformFileManager( const char *path )
{
	return ShellExecute( NULL, "explore", path, NULL, NULL, SW_SHOWDEFAULT ) > (HINSTANCE)32;
}

/*
=================
Sys_SetMaxFileLimit
=================
*/
qboolean Sys_SetMaxFileLimit( void )
{
	return qtrue;
}

/*
=================
Sys_InModalLoop

Windows is moving or resizing the window, or showing a menu, in a loop of
its own
=================
*/
qboolean Sys_InModalLoop( void )
{
	GUITHREADINFO info = { sizeof( info ) };

	return GetGUIThreadInfo( GetCurrentThreadId( ), &info ) &&
		( info.flags & ( GUI_INMOVESIZE | GUI_INMENUMODE ) );
}

/*
=================
Sys_Notify

No service manager on Windows reads a program's state this way
=================
*/
void Sys_Notify( const char *state )
{
}

typedef enum {
	FIREWALL_UNKNOWN,	// it couldn't be read
	FIREWALL_OFF,
	FIREWALL_ALLOWS,
	FIREWALL_BLOCKS,
	FIREWALL_BLOCKS_ALL,	// "Block all incoming connections"
	FIREWALL_NO_RULE	// on, with no rule for the program or its port
} firewall_t;

// NET_FW_IP_PROTOCOL_ANY, which MinGW's netfw.h lacks
#define FIREWALL_PROTOCOL_ANY 256

// Windows Firewall's class and interfaces, which netfw.h declares but may
// not define (MinGW's only after initguid.h)
static const CLSID firewallPolicyClass = { 0xe2b3c97f, 0x6ae1, 0x41ac,
	{ 0x81, 0x7a, 0xf6, 0xf9, 0x21, 0x66, 0xd7, 0xdd } };
static const IID firewallPolicyInterface = { 0x98325047, 0xc671, 0x4174,
	{ 0x8d, 0x81, 0xde, 0xfc, 0xd3, 0xf0, 0x31, 0x86 } };
static const IID firewallRuleInterface = { 0xaf230d27, 0xbaba, 0x4e42,
	{ 0xac, 0xed, 0xf5, 0x24, 0xf2, 0x2c, 0xfc, 0xe2 } };
static const IID firewallRule3Interface = { 0xb21563ff, 0xd696, 0x4222,
	{ 0xab, 0x46, 0x4e, 0x89, 0xb7, 0x3a, 0xb3, 0x4a } };

/*
=================
Sys_FirewallPortsInclude

Whether a rule's local ports, "*" or a list of ports and ranges, take in
port
=================
*/
static qboolean Sys_FirewallPortsInclude( BSTR ports, int port )
{
	char	list[ 1024 ];
	char	*p;
	int		low, high;

	if( !ports || !ports[ 0 ] )
		return qtrue;
	if( !WideCharToMultiByte( CP_UTF8, 0, ports, -1, list, sizeof( list ), NULL, NULL ) )
		return qfalse;
	for( p = strtok( list, "," ); p; p = strtok( NULL, "," ) )
	{
		if( !strcmp( p, "*" ) )
			return qtrue;
		if( sscanf( p, "%d-%d", &low, &high ) == 2 ? port >= low && port <= high : atoi( p ) == port )
			return qtrue;
	}
	return qfalse;
}

/*
=================
Sys_FirewallAny

Frees a rule's text property, and says whether it was empty or "*", or
match when given
=================
*/
static qboolean Sys_FirewallAny( BSTR text, const WCHAR *match )
{
	qboolean any = !text || !text[ 0 ] || !wcscmp( text, L"*" ) || ( match && !_wcsicmp( text, match ) );

	SysFreeString( text );
	return any;
}

/*
=================
Sys_FirewallRuleApplies

Whether an inbound rule decides whether players reach this program on
port: enabled in a current profile, for UDP, for this program or for any
(not a service's, an app package's or a user's), and for the port, from
any address to any of this computer's, on any kind of network interface
=================
*/
static qboolean Sys_FirewallRuleApplies( INetFwRule *rule, LONG profiles, const WCHAR *exe, int port )
{
	VARIANT_BOOL			enabled;
	NET_FW_RULE_DIRECTION	direction;
	LONG					ruleProfiles, protocol;
	BSTR					text = NULL;
	INetFwRule3				*rule3 = NULL;
	qboolean				applies;

	// the most telling first: most rules are for another program
	if( FAILED( rule->lpVtbl->get_Enabled( rule, &enabled ) ) || !enabled ||
		FAILED( rule->lpVtbl->get_Direction( rule, &direction ) ) || direction != NET_FW_RULE_DIR_IN ||
		FAILED( rule->lpVtbl->get_ApplicationName( rule, &text ) ) || !Sys_FirewallAny( text, exe ) ||
		FAILED( rule->lpVtbl->get_Profiles( rule, &ruleProfiles ) ) || !( ruleProfiles & profiles ) ||
		FAILED( rule->lpVtbl->get_Protocol( rule, &protocol ) ) ||
		( protocol != NET_FW_IP_PROTOCOL_UDP && protocol != FIREWALL_PROTOCOL_ANY ) ||
		FAILED( rule->lpVtbl->get_ServiceName( rule, &text ) ) || !Sys_FirewallAny( text, NULL ) ||
		FAILED( rule->lpVtbl->get_LocalAddresses( rule, &text ) ) || !Sys_FirewallAny( text, NULL ) ||
		FAILED( rule->lpVtbl->get_RemoteAddresses( rule, &text ) ) || !Sys_FirewallAny( text, NULL ) ||
		FAILED( rule->lpVtbl->get_InterfaceTypes( rule, &text ) ) || !Sys_FirewallAny( text, L"All" ) )
		return qfalse;

	// a Store app's rules, and a user's, name no program
	if( SUCCEEDED( rule->lpVtbl->QueryInterface( rule, &firewallRule3Interface, (void **)&rule3 ) ) )
	{
		applies =
			SUCCEEDED( rule3->lpVtbl->get_LocalAppPackageId( rule3, &text ) ) && Sys_FirewallAny( text, NULL ) &&
			SUCCEEDED( rule3->lpVtbl->get_LocalUserOwner( rule3, &text ) ) && Sys_FirewallAny( text, NULL );
		rule3->lpVtbl->Release( rule3 );
		if( !applies )
			return qfalse;
	}

	if( FAILED( rule->lpVtbl->get_LocalPorts( rule, &text ) ) )
		return qfalse;
	applies = Sys_FirewallPortsInclude( text, port );
	SysFreeString( text );
	return applies;
}

/*
=================
Sys_Firewall

Whether Windows Firewall lets players on other computers reach this
program on port, as far as a user without administrator rights can read
its policy: a block rule wins over an allow rule, and its name goes in
blocker
=================
*/
static firewall_t Sys_Firewall( const WCHAR *exe, int port, char *blocker, int size )
{
	static const NET_FW_PROFILE_TYPE2 types[] = {
		NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC
	};
	INetFwPolicy2	*policy = NULL;
	INetFwRules		*rules = NULL;
	IUnknown		*enumerator = NULL;
	IEnumVARIANT	*ruleList = NULL;
	VARIANT			item;
	VARIANT_BOOL	on;
	LONG			profiles;
	HRESULT			init;
	qboolean		firewallOn = qfalse, allowed = qfalse, blocked = qfalse, blockedAll = qfalse;
	firewall_t		result = FIREWALL_UNKNOWN;
	int				i;

	init = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
	if( FAILED( CoCreateInstance( &firewallPolicyClass, NULL, CLSCTX_INPROC_SERVER,
			&firewallPolicyInterface, (void **)&policy ) ) ||
		FAILED( policy->lpVtbl->get_CurrentProfileTypes( policy, &profiles ) ) )
		goto done;

	for( i = 0; i < ARRAY_LEN( types ); i++ )
	{
		if( ( profiles & types[ i ] ) &&
			SUCCEEDED( policy->lpVtbl->get_FirewallEnabled( policy, types[ i ], &on ) ) && on )
		{
			firewallOn = qtrue;
			if( SUCCEEDED( policy->lpVtbl->get_BlockAllInboundTraffic( policy, types[ i ], &on ) ) && on )
				blockedAll = qtrue;
		}
	}
	if( !firewallOn || blockedAll )
	{
		result = blockedAll ? FIREWALL_BLOCKS_ALL : FIREWALL_OFF;
		goto done;
	}

	if( FAILED( policy->lpVtbl->get_Rules( policy, &rules ) ) ||
		FAILED( rules->lpVtbl->get__NewEnum( rules, &enumerator ) ) ||
		FAILED( enumerator->lpVtbl->QueryInterface( enumerator, &IID_IEnumVARIANT, (void **)&ruleList ) ) )
		goto done;

	VariantInit( &item );
	while( !blocked && ruleList->lpVtbl->Next( ruleList, 1, &item, NULL ) == S_OK )
	{
		INetFwRule		*rule = NULL;
		NET_FW_ACTION	action;

		if( V_VT( &item ) == VT_DISPATCH && V_DISPATCH( &item ) &&
			SUCCEEDED( V_DISPATCH( &item )->lpVtbl->QueryInterface( V_DISPATCH( &item ),
				&firewallRuleInterface, (void **)&rule ) ) )
		{
			if( Sys_FirewallRuleApplies( rule, profiles, exe, port ) &&
				SUCCEEDED( rule->lpVtbl->get_Action( rule, &action ) ) )
			{
				if( action == NET_FW_ACTION_BLOCK )
				{
					BSTR name = NULL;

					blocked = qtrue;
					if( SUCCEEDED( rule->lpVtbl->get_Name( rule, &name ) ) && name )
						WideCharToMultiByte( CP_UTF8, 0, name, -1, blocker, size, NULL, NULL );
					SysFreeString( name );
				}
				else
					allowed = qtrue;
			}
			rule->lpVtbl->Release( rule );
		}
		VariantClear( &item );
	}
	result = blocked ? FIREWALL_BLOCKS : allowed ? FIREWALL_ALLOWS : FIREWALL_NO_RULE;

done:
	if( ruleList )
		ruleList->lpVtbl->Release( ruleList );
	if( enumerator )
		enumerator->lpVtbl->Release( enumerator );
	if( rules )
		rules->lpVtbl->Release( rules );
	if( policy )
		policy->lpVtbl->Release( policy );
	if( SUCCEEDED( init ) )
		CoUninitialize( );
	return result;
}

/*
=================
Sys_PrintFirewall

For a dedicated server's start summary: whether Windows Firewall lets
players on other computers in, and if not, the netsh lines that would,
for an administrator to run. Nothing when it can't be read
=================
*/
void Sys_PrintFirewall( int port )
{
	WCHAR	exe[ MAX_PATH ];
	char	path[ MAX_OSPATH ];
	char	blocker[ 256 ] = "";

	if( !GetModuleFileNameW( NULL, exe, ARRAY_LEN( exe ) ) ||
		!WideCharToMultiByte( CP_UTF8, 0, exe, -1, path, sizeof( path ), NULL, NULL ) )
		return;

	switch( Sys_Firewall( exe, port, blocker, sizeof( blocker ) ) )
	{
		case FIREWALL_OFF:
			Com_Printf( "  firewall: Windows Firewall is off\n" );
			return;
		case FIREWALL_ALLOWS:
			Com_Printf( "  firewall: Windows Firewall lets players in\n" );
			return;
		case FIREWALL_BLOCKS_ALL:
			Com_Printf( "  firewall: Windows Firewall blocks all incoming connections, so players on other\n"
				"    computers can't join: its \"Block all incoming connections\" setting is on\n" );
			return;
		case FIREWALL_BLOCKS:
			Com_Printf( "  firewall: Windows Firewall's rule \"%s\" blocks players on other computers.\n"
				"    To let them in, run as administrator:\n"
				"    netsh advfirewall firewall delete rule name=\"%s\" dir=in\n", blocker, blocker );
			break;
		case FIREWALL_NO_RULE:
			Com_Printf( "  firewall: no Windows Firewall rule lets players on other computers in yet,\n"
				"    and Windows may block them. To let them in, run as administrator:\n" );
			break;
		default:
			return;
	}
	Com_Printf( "    netsh advfirewall firewall add rule name=\"" PRODUCT_NAME " dedicated server\" "
		"dir=in action=allow program=\"%s\" protocol=UDP localport=%d\n", path, port );
}
