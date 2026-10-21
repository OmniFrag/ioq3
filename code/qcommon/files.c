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
/*****************************************************************************
 * name:		files.c
 *
 * desc:		handle based filesystem for Quake III Arena 
 *
 * $Archive: /MissionPack/code/qcommon/files.c $
 *
 *****************************************************************************/


#include "q_shared.h"
#include "qcommon.h"
#include "unzip.h"
#include "content.h"
#include <errno.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

/*
=============================================================================

QUAKE3 FILESYSTEM

All of Quake's data access is through a hierarchical file system, but the contents of 
the file system can be transparently merged from several sources.

A "qpath" is a reference to game file data.  MAX_ZPATH is 256 characters, which must include
a terminating zero. "..", "\\", and ":" are explicitly illegal in qpaths to prevent any
references outside the quake directory system.

The "base path" is the path to the directory holding all the game directories and usually
the executable.  It defaults to ".", but can be overridden with a "+set fs_basepath c:\quake3"
command line to allow code debugging in a different directory.  Basepath cannot
be modified at all after startup.  Any files that are created (demos, screenshots,
etc) will be created relative to the base path, so base path should usually be writable.

The "home path" is the path used for all write access. On win32 systems we have "base path"
== "home path", but on *nix systems the base installation is usually readonly, and
"home path" points to ~/.q3a or similar

The user can also install custom mods and content in "home path", so it should be searched
along with "home path" and "cd path" for game content.


The "base game" is the directory under the paths where data comes from by default, and
can be either "baseq3" or "demoq3".

The "current game" may be the same as the base game, or it may be the name of another
directory under the paths that should be searched for files before looking in the base game.
This is the basis for addons.

Clients automatically set the game directory after receiving a gamestate from a server,
so only servers need to worry about +set fs_game.

No other directories outside of the base game and current game will ever be referenced by
filesystem functions.

To save disk space and speed loading, directory trees can be collapsed into zip files.
The files use a ".pk3" extension to prevent users from unzipping them accidentally, but
otherwise the are simply normal uncompressed zip files.  A game directory can have multiple
zip files of the form "pak0.pk3", "pak1.pk3", etc.  Zip files are searched in decending order
from the highest number to the lowest, and will always take precedence over the filesystem.
This allows a pk3 distributed as a patch to override all existing data.

Because we will have updated executables freely available online, there is no point to
trying to restrict demo / oem versions of the game with code changes.  Demo / oem versions
should be exactly the same executables as release versions, but with different data that
automatically restricts where game media can come from to prevent add-ons from working.

File search order: when FS_FOpenFileRead gets called it will go through the fs_searchpaths
structure and stop on the first successful hit. fs_searchpaths is built with successive
calls to FS_AddGameDirectory

Additionally, we search in several subdirectories:
current game is the current mode
base game is a variable to allow mods based on other mods
(such as baseq3 + missionpack content combination in a mod for instance)
BASEGAME is the hardcoded base game ("baseq3")

e.g. the qpath "sound/newstuff/test.wav" would be searched for in the following places:

home path + current game's zip files
home path + current game's directory
base path + current game's zip files
base path + current game's directory
cd path + current game's zip files
cd path + current game's directory

home path + base game's zip file
home path + base game's directory
base path + base game's zip file
base path + base game's directory
cd path + base game's zip file
cd path + base game's directory

home path + BASEGAME's zip file
home path + BASEGAME's directory
base path + BASEGAME's zip file
base path + BASEGAME's directory
cd path + BASEGAME's zip file
cd path + BASEGAME's directory

server download, to be written to home path + current game's directory


The filesystem can be safely shutdown and reinitialized with different
basedir / cddir / game combinations, but all other subsystems that rely on it
(sound, video) must also be forced to restart.

Because the same files are loaded by both the clip model (CM_) and renderer (TR_)
subsystems, a simple single-file caching scheme is used.  The CM_ subsystems will
load the file with a request to cache.  Only one file will be kept cached at a time,
so any models that are going to be referenced by both subsystems should alternate
between the CM_ load function and the ref load function.

TODO: A qpath that starts with a leading slash will always refer to the base game, even if another
game is currently active.  This allows character models, skins, and sounds to be downloaded
to a common directory no matter which game is active.

How to prevent downloading zip files?
Pass pk3 file names in systeminfo, and download before FS_Restart()?

Aborting a download disconnects the client from the server.

How to mark files as downloadable?  Commercial add-ons won't be downloadable.

Non-commercial downloads will want to download the entire zip file.
the game would have to be reset to actually read the zip in

Auto-update information

Path separators

Casing

  separate server gamedir and client gamedir, so if the user starts
  a local game after having connected to a network game, it won't stick
  with the network game.

  allow menu options for game selection?

Read / write config to floppy option.

Different version coexistance?

When building a pak file, make sure a q3config.cfg isn't present in it,
or configs will never get loaded from disk!

  todo:

  downloading (outside fs?)
  game directory passing and restarting

=============================================================================

*/

// every time a new demo pk3 file is built, this checksum must be updated.
// the easiest way to get it is to just run the game and see what it spits out
#ifndef STANDALONE
#define	DEMO_PAK0_CHECKSUM	2985612116u
static const unsigned int pak_checksums[] = {
	1566731103u,
	298122907u,
	412165236u,
	2991495316u,
	1197932710u,
	4087071573u,
	3709064859u,
	908855077u,
	977125798u
};

static const unsigned int missionpak_checksums[] =
{
	2430342401u,
	511014160u,
	2662638993u,
	1438664554u
};
#endif

// a checksum the tests have the server take for one of the game's own
// paks (FS_RetailPak), from OMNIFRAG_TEST_REFUSED_PAK_CHECKSUM at startup
static qboolean		fs_refusedTestPak;
static unsigned int	fs_refusedTestChecksum;

/*
================
FS_RetailPakGame

Which of the game's own paks, Quake III Arena 1.32's or Team Arena's, a
checksum is: its game directory, and its number in index; NULL for none
================
*/
static const char *FS_RetailPakGame( unsigned int checksum, int *index ) {
#ifndef STANDALONE
	for( *index = 0; *index < ARRAY_LEN( pak_checksums ); ( *index )++ ) {
		if( checksum == pak_checksums[*index] ) {
			return BASEGAME;
		}
	}
	for( *index = 0; *index < ARRAY_LEN( missionpak_checksums ); ( *index )++ ) {
		if( checksum == missionpak_checksums[*index] ) {
			return BASETA;
		}
	}
#endif
	return NULL;
}

// if this is defined, the executable positively won't work with any paks other
// than the demo pak, even if productid is present.  This is only used for our
// last demo release to prevent the mac and linux users from using the demo
// executable with the production windows pak before the mac/linux products
// hit the shelves a little later
// NOW defined in build files
//#define PRE_RELEASE_TADEMO

#define MAX_ZPATH			256
#define	MAX_SEARCH_PATHS	4096
#define MAX_FILEHASH_SIZE	1024

typedef struct fileInPack_s {
	char					*name;		// name of the file
	unsigned long			pos;		// file info position in zip
	unsigned long			len;		// uncompress file size
	struct	fileInPack_s*	next;		// next file in the hash
} fileInPack_t;

typedef struct {
	char			pakPathname[MAX_OSPATH];	// c:\quake3\baseq3
	char			pakFilename[MAX_OSPATH];	// c:\quake3\baseq3\pak0.pk3
	char			pakBasename[MAX_OSPATH];	// pak0
	char			pakGamename[MAX_OSPATH];	// baseq3
	unzFile			handle;						// handle to zip file
	unsigned int	checksum;					// regular checksum
	int				contentRoot;				// the root it's under, for the pak index
	int				pure_checksum;				// checksum for pure
	int				numfiles;					// number of files in pk3
	int				referenced;					// referenced file flags
	int				hashSize;					// hash table size (power of 2)
	fileInPack_t*	*hashTable;					// hash table
	fileInPack_t*	buildBuffer;				// buffer with the filenames etc.
	qboolean		cached;						// a download, from the content cache
	qboolean		defaultCfgNoted;			// its default.cfg read, and said so
} pack_t;

typedef struct {
	char		path[MAX_OSPATH];		// c:\quake3
	char		fullpath[MAX_OSPATH];		// c:\quake3\baseq3
	char		gamedir[MAX_OSPATH];	// baseq3
} directory_t;

typedef struct searchpath_s {
	struct searchpath_s *next;

	pack_t		*pack;		// only one of pack / dir will be non NULL
	directory_t	*dir;
} searchpath_t;

static	char		fs_gamedir[MAX_OSPATH];	// this will be a single file name with no separators
static	cvar_t		*fs_debug;
static	cvar_t		*fs_homeconfigpath;
static	cvar_t		*fs_homedatapath;
static	cvar_t		*fs_homestatepath;

static	cvar_t		*fs_apppath;
static	cvar_t		*fs_steampath;
static	cvar_t		*fs_gogpath;
static	cvar_t		*fs_predecessorpath;
static	char		fs_predecessorConfigPath[MAX_OSPATH];	// its config home, the data one's on XDG
// under --check, its config a start would copy as ours, and where to
// (FS_ImportPredecessorConfig)
static	char		fs_uncopiedConfig[MAX_OSPATH], fs_uncopiedConfigTo[MAX_OSPATH];
static	cvar_t		*fs_microsoftstorepath;

static	cvar_t		*fs_basepath;
static	cvar_t		*fs_basegame;
static	cvar_t		*fs_gamedirvar;
static	searchpath_t	*fs_searchpaths;
static	int			fs_readCount;			// total bytes read
static	int			fs_loadCount;			// total files read
static	int			fs_loadStack;			// total files in memory
static	int			fs_packFiles = 0;		// total number of files in packs

static cvar_t *fs_pathVars[16];

// Only the paks are read from the predecessor's home (a server offers them
// for download), never its configs or other files
static qboolean FS_PathVarPaksOnly( const cvar_t *pathVar ) {
	return pathVar == fs_predecessorpath;
}

// the roots' numbers, which the pak index keeps: index 0 is none, and a
// new root goes at the end. Each is one of FS_InitPathVars' path cvars
static cvar_t **const fs_contentRoots[] = {
	NULL, &fs_basepath, &fs_homedatapath, &fs_homeconfigpath, &fs_homestatepath, &fs_apppath,
	&fs_predecessorpath, &fs_steampath, &fs_gogpath, &fs_microsoftstorepath
};

/*
================
FS_ContentRootPath
================
*/
const char *FS_ContentRootPath( int root ) {
	const cvar_t	*var;

	if ( root <= 0 || root >= (int)ARRAY_LEN( fs_contentRoots ) ) {
		return NULL;
	}
	var = *fs_contentRoots[root];
	return var && var->string[0] ? var->string : NULL;
}

/*
================
FS_SetContentRoot

The paks just added to the search path from a path cvar's directory, from
up to the entries before (to), take its root's number: the lowest of the
roots naming that directory, so where the homes are one, as fs_homepath
makes them, their paks are fs_homedatapath's whichever cvar added them
================
*/
static void FS_SetContentRoot( searchpath_t *from, const searchpath_t *to, const cvar_t *pathVar ) {
	const char	*rootPath;
	int			root;

	for ( root = 1; root < (int)ARRAY_LEN( fs_contentRoots ); root++ ) {
		if ( ( rootPath = FS_ContentRootPath( root ) ) != NULL && !Q_stricmp( rootPath, pathVar->string ) ) {
			break;
		}
	}
	if ( root == (int)ARRAY_LEN( fs_contentRoots ) ) {
		root = 0;	// a path cvar the table lacks: not indexed
	}
	for ( ; from != to; from = from->next ) {
		if ( from->pack ) {
			from->pack->contentRoot = root;
		}
	}
}

static int fs_checksumFeed;

typedef union qfile_gus {
	FILE*		o;
	unzFile		z;
} qfile_gut;

typedef struct qfile_us {
	qfile_gut	file;
	qboolean	unique;
} qfile_ut;

typedef struct {
	qfile_ut	handleFiles;
	qboolean	handleSync;
	int			fileSize;
	int			zipFilePos;
	int			zipFileLen;
	qboolean	zipFile;
	char		name[MAX_ZPATH];
	const vm_t	*owner;			// the module that opened it (FS_VM_FOpenFile)
	int			writeError;		// FS_Write drops its writes until it's closed
	qboolean	isLog;			// and drops them while Log_Paused
} fileHandleData_t;

static fileHandleData_t	fsh[MAX_FILE_HANDLES];

// TTimo - https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=540
// wether we did a reorder on the current search path when joining the server
static qboolean fs_reordered;

// never load anything from pk3 files that are not present at the server when pure
static int			fs_numServerPaks = 0;
static unsigned int	fs_serverPaks[MAX_SEARCH_PATHS];				// checksums
static char			*fs_serverPakNames[MAX_SEARCH_PATHS];			// pk3 names

// only used for autodownload, to make sure the client has at least
// all the pk3 files that are referenced at the server side
static int			fs_numServerReferencedPaks;
static unsigned int	fs_serverReferencedPaks[MAX_SEARCH_PATHS];			// checksums
static char			*fs_serverReferencedPakNames[MAX_SEARCH_PATHS];		// pk3 names
// each one's presence when the search path was built (FS_NoteServerPaks)
static qboolean		fs_serverReferencedPakHad[MAX_SEARCH_PATHS];

// the search path's paks from the content cache (FS_AddCachePaks)
static int			fs_numCachePaks;
// the installed paks left out of it for their hashes (FS_RefuseHashedPaks)
static int			fs_numRefusedPaks;

// the hashes the content manifest of the server connected to gives paks,
// by their game directories, names and checksums (FS_SetServerPakHashes):
// a pak with one of those is mounted only with its hash. Two paks of the
// same files under other names share a checksum, so the name tells them
// apart (FS_ServerPakHash)
static qboolean		fs_serverPakHashesSet;
static byte			fs_serverPakHashesFrom[SHA256_DIGEST_SIZE];	// that content manifest's hash
static int			fs_numServerPakHashes;
static ofcmEntry_t	*fs_serverPakHashes;		// its entries, each one
static qboolean		fs_serverPakHashesChanged;	// since the search path was built (FS_ConditionalRestart)

static qboolean FS_ContentRelativePath( const pack_t *pak, char *relative, int size );

// last valid game folder used
char lastValidBase[MAX_OSPATH];
char lastValidComBaseGame[MAX_OSPATH];
char lastValidFsBaseGame[MAX_OSPATH];
char lastValidGame[MAX_OSPATH];

#ifdef FS_MISSING
FILE*		missingFiles = NULL;
#endif

/* C99 defines __func__ */
#if __STDC_VERSION__ < 199901L
#  if __GNUC__ >= 2 || _MSC_VER >= 1300
#    define __func__ __FUNCTION__
#  else
#    define __func__ "(unknown)"
#  endif
#endif

/*
==============
FS_Initialized
==============
*/

qboolean FS_Initialized( void ) {
	return (fs_searchpaths != NULL);
}

/*
=================
FS_PakIsPure
=================
*/
qboolean FS_PakIsPure( pack_t *pack ) {
	int i;

	if ( fs_numServerPaks ) {
		for ( i = 0 ; i < fs_numServerPaks ; i++ ) {
			// FIXME: also use hashed file names
			// NOTE TTimo: a pk3 with same checksum but different name would be validated too
			//   I don't see this as allowing for any exploit, it would only happen if the client does manips of its file names 'not a bug'
			if ( pack->checksum == fs_serverPaks[i] ) {
				return qtrue;		// on the aproved list
			}
		}
		return qfalse;	// not on the pure server pak list
	}
	return qtrue;
}


/*
=================
FS_LoadStack
return load stack
=================
*/
int FS_LoadStack( void )
{
	return fs_loadStack;
}

/*
================
return a hash value for the filename
================
*/
static long FS_HashFileName( const char *fname, int hashSize ) {
	int		i;
	long	hash;
	char	letter;

	hash = 0;
	i = 0;
	while (fname[i] != '\0') {
		letter = tolower(fname[i]);
		if (letter =='.') break;				// don't include extension
		if (letter =='\\') letter = '/';		// damn path names
		if (letter == PATH_SEP) letter = '/';		// damn path names
		hash+=(long)(letter)*(i+119);
		i++;
	}
	hash = (hash ^ (hash >> 10) ^ (hash >> 20));
	hash &= (hashSize-1);
	return hash;
}

static fileHandle_t	FS_HandleForFile(void) {
	int		i;

	for ( i = 1 ; i < MAX_FILE_HANDLES ; i++ ) {
		if ( fsh[i].handleFiles.file.o == NULL ) {
			return i;
		}
	}
	Com_Error( ERR_DROP, "FS_HandleForFile: none free" );
	return 0;
}

static FILE	*FS_FileForHandle( fileHandle_t f ) {
	if ( f < 1 || f >= MAX_FILE_HANDLES ) {
		Com_Error( ERR_DROP, "FS_FileForHandle: out of range" );
	}
	if (fsh[f].zipFile == qtrue) {
		Com_Error( ERR_DROP, "FS_FileForHandle: can't get FILE on zip file" );
	}
	if ( ! fsh[f].handleFiles.file.o ) {
		Com_Error( ERR_DROP, "FS_FileForHandle: NULL" );
	}
	
	return fsh[f].handleFiles.file.o;
}

void	FS_ForceFlush( fileHandle_t f ) {
	FILE *file;

	file = FS_FileForHandle(f);
	setvbuf( file, NULL, _IONBF, 0 );
}

/*
================
FS_fplength
================
*/

long FS_fplength(FILE *h)
{
	long		pos;
	long		end;

	pos = ftell(h);
	fseek(h, 0, SEEK_END);
	end = ftell(h);
	fseek(h, pos, SEEK_SET);

	return end;
}

/*
================
FS_filelength

If this is called on a non-unique FILE (from a pak file),
it will return the size of the pak file, not the expected
size of the file.
================
*/
long FS_filelength(fileHandle_t f)
{
	FILE	*h;

	h = FS_FileForHandle(f);
	
	if(h == NULL)
		return -1;
	else
		return FS_fplength(h);
}

/*
====================
FS_ReplaceSeparators

Fix things up differently for win/unix/mac
====================
*/
static void FS_ReplaceSeparators( char *path ) {
	char	*s;
	qboolean lastCharWasSep = qfalse;

	for ( s = path ; *s ; s++ ) {
		if ( *s == '/' || *s == '\\' ) {
			if ( !lastCharWasSep ) {
				*s = PATH_SEP;
				lastCharWasSep = qtrue;
			} else {
				memmove (s, s + 1, strlen (s));
			}
		} else {
			lastCharWasSep = qfalse;
		}
	}
}

/*
===================
FS_BuildOSPath

Qpath may have either forward or backwards slashes
===================
*/
char *FS_BuildOSPath( const char *base, const char *game, const char *qpath ) {
	char	temp[MAX_OSPATH];
	static char ospath[2][MAX_OSPATH];
	static int toggle;
	
	toggle ^= 1;		// flip-flop to allow two returns without clash

	if( !game || !game[0] ) {
		game = fs_gamedir;
	}

	Com_sprintf( temp, sizeof(temp), "/%s/%s", game, qpath );
	FS_ReplaceSeparators( temp );
	Com_sprintf( ospath[toggle], sizeof( ospath[0] ), "%s%s", base, temp );
	
	return ospath[toggle];
}

/*
===================
FS_BaseDir_BuildOSPath

Qpath may have either forward or backwards slashes
===================
*/
char *FS_BaseDir_BuildOSPath( const char *base, const char *qpath ) {
	char *ospath = FS_BuildOSPath(base, qpath, "");
	ospath[strlen(ospath) - 1] = '\0';

	return ospath;
}


/*
============
FS_CheckRefuses

--check writes nothing. Its callers skip what they'd write, saying so
where that's clearer; this refuses anything else that would make, write,
rename or remove a file or folder, saying which
============
*/
static qboolean FS_CheckRefuses( const char *function, const char *ospath ) {
	if ( !com_check ) {
		return qfalse;
	}
	Com_Printf( "--check writes no file: %s refused %s\n", function, ospath );
	return qtrue;
}

/*
============
FS_CreatePath

Creates any directories needed to store the given filename
============
*/
qboolean FS_CreatePath (const char *OSPath) {
	char	*ofs;
	char	path[MAX_OSPATH];
	
	if (!OSPath || !*OSPath) {
		return qfalse;
	}

	if ( FS_CheckRefuses( __func__, OSPath ) ) {
		return qtrue;
	}

	// make absolutely sure that it can't back up the path
	// FIXME: is c: allowed???
	if ( strstr( OSPath, ".." ) || strstr( OSPath, "::" ) ) {
		Com_Printf( "WARNING: refusing to create relative path \"%s\"\n", OSPath );
		return qtrue;
	}

	Q_strncpyz( path, OSPath, sizeof( path ) );
	FS_ReplaceSeparators( path );

	// Skip creation of the root directory as it will always be there
	ofs = strchr( path, PATH_SEP );
	if ( ofs != NULL ) {
		ofs++;
	}

	for (; ofs != NULL && *ofs ; ofs++) {
		if (*ofs == PATH_SEP) {
			// create the directory
			*ofs = 0;
			if (!Sys_Mkdir (path)) {
				Com_Error( ERR_FATAL, "FS_CreatePath: failed to create path \"%s\"",
					path );
			}
			*ofs = PATH_SEP;
		}
	}

	return qfalse;
}

/*
=================
FS_IsNativeFile

Whether a file name has the extension of a library or program on any
platform, versioned Unix libraries included
=================
*/
static qboolean FS_IsNativeFile( const char *filename )
{
	static const char *extensions[] = { ".dll", ".exe", ".so", ".dylib" };
	const char *so;
	int i;

	for ( i = 0; i < ARRAY_LEN( extensions ); i++ ) {
		if ( COM_CompareExtension( filename, extensions[i] ) ) {
			return qtrue;
		}
	}

	// versioned Unix libraries: .so.1, .so.1.2.3
	for ( so = Q_stristr( filename, ".so." ); so; so = Q_stristr( so + 1, ".so." ) ) {
		const char *p = so + 4;

		if ( *p < '0' || *p > '9' ) {
			continue;
		}
		while ( ( *p >= '0' && *p <= '9' ) || *p == '.' ) {
			p++;
		}
		if ( !*p ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
FS_IsCodeFile

Whether a file name has the extension of a library, program or game
module on any platform, or of a pk3, which can hold game modules. Files
written on one platform can be copied to, or shared with, another.
=================
*/
static qboolean FS_IsCodeFile( const char *filename )
{
	return FS_IsNativeFile( filename ) || COM_CompareExtension( filename, ".qvm" )
		|| COM_CompareExtension( filename, ".wasm" ) || COM_CompareExtension( filename, ".pk3" );
}

/*
=================
FS_CheckFilenameIsMutable

ERR_DROP if trying to maniuplate a file with a library, program, QVM, or pk3 extension
=================
 */
static void FS_CheckFilenameIsMutable( const char *filename,
		const char *function )
{
	// Check if the filename ends with a library, program, QVM, or pk3 extension
	if( FS_IsCodeFile( filename ) )
	{
		Com_Error( ERR_DROP, "%s: Not allowed to manipulate '%s' due "
			"to %s extension", function, filename, COM_GetExtension( filename ) );
	}
}

/*
===========
FS_Remove

===========
*/
void FS_Remove( const char *osPath ) {
	FS_CheckFilenameIsMutable( osPath, __func__ );

	if ( FS_CheckRefuses( __func__, osPath ) ) {
		return;
	}
	remove( osPath );
}

/*
===========
FS_Remove_HomeData

===========
*/
void FS_Remove_HomeData( const char *homePath ) {
	FS_Remove( FS_BuildOSPath( fs_homedatapath->string, fs_gamedir, homePath ) );
}

/*
================
FS_FileInPathExists

Tests if path and file exists
================
*/
qboolean FS_FileInPathExists(const char *testpath)
{
	FILE *filep;

	filep = Sys_FOpen(testpath, "rb");
	
	if(filep)
	{
		fclose(filep);
		return qtrue;
	}
	
	return qfalse;
}

/*
================
FS_FileExists_HomeData

Tests if the file exists in the current gamedir, this DOES NOT
search the paths.  This is to determine if opening a file to write
(which always goes into the current gamedir) will cause any overwrites.
NOTE TTimo: this goes with FS_FOpenFileWrite for opening the file afterwards
================
*/
qboolean FS_FileExists_HomeData(const char *file)
{
	return FS_FileInPathExists(FS_BuildOSPath(fs_homedatapath->string, fs_gamedir, file));
}

/*
================
FS_BaseDir_FileExists_HomeData

Tests if the file exists 
================
*/
static qboolean FS_BaseDir_FileExists_HomeData( const char *file )
{
	return FS_FileInPathExists(FS_BaseDir_BuildOSPath(fs_homedatapath->string, file));
}


/*
===========
FS_OSPath_FOpenFileWrite
===========
*/
static fileHandle_t FS_OSPath_FOpenFileWrite( const char *ospath, const char *filename ) {
	fileHandle_t	f;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	f = FS_HandleForFile();
	fsh[f].zipFile = qfalse;

	if ( fs_debug->integer ) {
		Com_Printf( "FS_OSPath_FOpenFileWrite: %s\n", ospath );
	}

	FS_CheckFilenameIsMutable( ospath, __func__ );

	if( FS_CheckRefuses( __func__, ospath ) || FS_CreatePath( ospath ) ) {
		return 0;
	}

	fsh[f].handleFiles.file.o = Sys_FOpen( ospath, "wb" );

	Q_strncpyz( fsh[f].name, filename, sizeof( fsh[f].name ) );

	fsh[f].handleSync = qfalse;
	if (!fsh[f].handleFiles.file.o) {
		f = 0;
	}
	return f;
}

/*
===========
FS_BaseDir_FOpenFileWrite_HomeConfig
===========
*/
fileHandle_t FS_BaseDir_FOpenFileWrite_HomeConfig( const char *filename ) {
	return FS_OSPath_FOpenFileWrite(
		FS_BaseDir_BuildOSPath(fs_homeconfigpath->string, filename), filename);
}

/*
===========
FS_BaseDir_FOpenFileWrite_HomeData
===========
*/
fileHandle_t FS_BaseDir_FOpenFileWrite_HomeData( const char *filename ) {
	return FS_OSPath_FOpenFileWrite(
		FS_BaseDir_BuildOSPath(fs_homedatapath->string, filename), filename);
}

/*
===========
FS_BaseDir_FOpenFileWrite_HomeState
===========
*/
fileHandle_t FS_BaseDir_FOpenFileWrite_HomeState( const char *filename ) {
	return FS_OSPath_FOpenFileWrite(
		FS_BaseDir_BuildOSPath(fs_homestatepath->string, filename), filename);
}

/*
===========
FS_BaseDir_FOpenFileRead

Search for a file somewhere below the home path then base path
in that order
===========
*/
long FS_BaseDir_FOpenFileRead(const char *filename, fileHandle_t *fp)
{
	char *ospath;
	fileHandle_t f = 0;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	f = FS_HandleForFile();
	fsh[f].zipFile = qfalse;

	Q_strncpyz( fsh[f].name, filename, sizeof( fsh[f].name ) );

	// don't let sound stutter
	S_ClearSoundBuffer();

	for(int i = 0; i < ARRAY_LEN( fs_pathVars ) && !fsh[f].handleFiles.file.o; i++) {
		const cvar_t *pathVar = fs_pathVars[i];

		if (!pathVar || !pathVar->string[0] ||
			( FS_PathVarPaksOnly( pathVar ) && !COM_CompareExtension( filename, ".pk3" ) )) {
			continue;
		}

		ospath = FS_BaseDir_BuildOSPath( pathVar->string, filename );

		if ( fs_debug->integer )
		{
			Com_Printf( "FS_BaseDir_FOpenFileRead (%s): %s\n", pathVar->name, ospath );
		}

		fsh[f].handleFiles.file.o = Sys_FOpen( ospath, "rb" );
		fsh[f].handleSync = qfalse;
	}

	if (!fsh[f].handleFiles.file.o)
	{
		f = 0;
	}

	*fp = f;
	if (f) {
		return FS_filelength(f);
	}

	return -1;
}


/*
===========
FS_ReadOSFile

A file outside the search path, into memory from the main zone (free it
with Z_Free)
===========
*/
static long FS_ReadOSFile( const char *ospath, void **buffer )
{
	FILE	*f = Sys_FOpen( ospath, "rb" );
	long	length;
	char	*text;

	*buffer = NULL;
	if ( !f ) {
		return -1;
	}
	fseek( f, 0, SEEK_END );
	length = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( length < 0 ) {
		fclose( f );
		return -1;
	}
	text = Z_Malloc( length + 1 );
	length = fread( text, 1, length, f );
	fclose( f );
	text[length] = 0;
	*buffer = text;
	return length;
}

/*
===========
FS_BaseDir_ReadFile_HomeConfig
===========
*/
long FS_BaseDir_ReadFile_HomeConfig( const char *filename, void **buffer )
{
	return FS_ReadOSFile( FS_BaseDir_BuildOSPath( fs_homeconfigpath->string, filename ), buffer );
}

/*
===========
FS_PredecessorReadsConfig

Whether FS_ReadPredecessorConfig looks for this file: a config, never the
engine's own, and none with fs_predecessorpath emptied, as for the paks
===========
*/
static qboolean FS_PredecessorReadsConfig( const char *filename )
{
	return fs_predecessorpath && fs_predecessorpath->string[0] && COM_CompareExtension( filename, ".cfg" ) &&
		!FS_CheckDirTraversal( filename ) && !FS_IsEngineFile( filename );
}

/*
===========
FS_ReadPredecessorConfig

A config in the game directories of the predecessor's homes, its config
home's then its data home's, where a dedicated server that ran before this
one kept its server.cfg: read only, and never the engine's own files.
ospath gets where it was. Free the buffer with Z_Free
===========
*/
long FS_ReadPredecessorConfig( const char *filename, void **buffer, char *ospath, int size )
{
	const char	*homes[2], *games[3];
	long		length = -1;
	int			i, j;

	*buffer = NULL;
	if ( !FS_PredecessorReadsConfig( filename ) ) {
		return -1;
	}
	homes[0] = fs_predecessorConfigPath;
	homes[1] = fs_predecessorpath->string;
	// the game directories the search reads, the mod's first
	games[0] = fs_gamedir;
	games[1] = fs_basegame->string;
	games[2] = com_basegame->string;
	for ( j = 0; j < ARRAY_LEN( games ) && length < 0; j++ ) {
		if ( !games[j][0] || ( j > 0 && !Q_stricmp( games[j], games[0] ) ) ||
			( j == 2 && !Q_stricmp( games[j], games[1] ) ) ) {
			continue;
		}
		for ( i = 0; i < ARRAY_LEN( homes ) && length < 0; i++ ) {
			// on ~/.q3a both are one directory
			if ( homes[i][0] && ( i == 0 || Q_stricmp( homes[i], homes[0] ) ) ) {
				Q_strncpyz( ospath, FS_BuildOSPath( homes[i], games[j], filename ), size );
				length = FS_ReadOSFile( ospath, buffer );
			}
		}
	}
	return length;
}

/*
===========
FS_ReadUncopiedConfig

Under --check, which copies nothing, the predecessor's config a start
would copy as filename, in the home's config directory
(FS_ImportPredecessorConfig), read where it is. ospath gets where. Free
the buffer with Z_Free
===========
*/
long FS_ReadUncopiedConfig( const char *filename, void **buffer, char *ospath, int size )
{
	*buffer = NULL;
	if ( !fs_uncopiedConfig[0] || strcmp( filename, fs_uncopiedConfigTo ) ) {
		return -1;
	}
	Q_strncpyz( ospath, fs_uncopiedConfig, size );
	return FS_ReadOSFile( fs_uncopiedConfig, buffer );
}

/*
===========
FS_LoadedGameDir
===========
*/
const char *FS_LoadedGameDir( void )
{
	return fs_gamedir;
}

/*
===========
FS_BaseDir_Rename_HomeData

Returns whether the file has its new name
===========
*/
qboolean FS_BaseDir_Rename_HomeData( const char *from, const char *to, qboolean safe ) {
	char			*from_ospath, *to_ospath;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	// don't let sound stutter
	S_ClearSoundBuffer();

	from_ospath = FS_BaseDir_BuildOSPath( fs_homedatapath->string, from );
	to_ospath = FS_BaseDir_BuildOSPath( fs_homedatapath->string, to );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_BaseDir_Rename_HomeData: %s --> %s\n", from_ospath, to_ospath );
	}

	if ( safe ) {
		FS_CheckFilenameIsMutable( to_ospath, __func__ );
	}

	if ( FS_CheckRefuses( __func__, from_ospath ) ) {
		return qfalse;
	}
	return rename( from_ospath, to_ospath ) == 0;
}

/*
===========
FS_BaseDir_Remove_HomeData

===========
*/
void FS_BaseDir_Remove_HomeData( const char *file ) {
	FS_Remove( FS_BaseDir_BuildOSPath( fs_homedatapath->string, file ) );
}

/*
=================
FS_FailWrite

Warns that a write to h failed, once, on the terminal alone (not in a log,
which may be h, nor to an rcon redirect), and drops its later writes until
it's opened again, so a full disk loses lines rather than stalling on them
=================
*/
static void FS_FailWrite( fileHandle_t h ) {
	if ( fsh[h].writeError ) {
		return;
	}
	fsh[h].writeError = errno ? errno : -1;
	Sys_Print( va( S_COLOR_YELLOW "WARNING: couldn't write %s (%s); its lines are lost until it's "
		"opened again\n", fsh[h].name, errno ? strerror( errno ) : "no reason given" ) );
}

/*
==============
FS_FCloseFile

If the FILE pointer is an open pak file, leave it open.

For some reason, other dll's can't just cal fclose()
on files returned by FS_FOpenFile...
==============
*/
void FS_FCloseFile( fileHandle_t f ) {
	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if (fsh[f].zipFile == qtrue) {
		unzCloseCurrentFile( fsh[f].handleFiles.file.z );
		if ( fsh[f].handleFiles.unique ) {
			unzClose( fsh[f].handleFiles.file.z );
		}
		Com_Memset( &fsh[f], 0, sizeof( fsh[f] ) );
		return;
	}

	// we didn't find it as a pak, so close it as a unique file; a write
	// still buffered can fail here
	if (fsh[f].handleFiles.file.o) {
		errno = 0;
		if ( fclose( fsh[f].handleFiles.file.o ) ) {
			FS_FailWrite( f );
		}
	}
	Com_Memset( &fsh[f], 0, sizeof( fsh[f] ) );
}

/*
===========
FS_FOpenFileWrite_HomeConfig
===========
*/
fileHandle_t FS_FOpenFileWrite_HomeConfig( const char *filename ) {
	return FS_OSPath_FOpenFileWrite(
		FS_BuildOSPath(fs_homeconfigpath->string, fs_gamedir, filename), filename);
}

/*
===========
FS_FOpenFileWrite_HomeData
===========
*/
fileHandle_t FS_FOpenFileWrite_HomeData( const char *filename ) {
	return FS_OSPath_FOpenFileWrite(
		FS_BuildOSPath(fs_homedatapath->string, fs_gamedir, filename), filename);
}

/*
===========
FS_FOpenFileWrite_HomeState
===========
*/
fileHandle_t FS_FOpenFileWrite_HomeState( const char *filename ) {
	return FS_OSPath_FOpenFileWrite(
		FS_BuildOSPath(fs_homestatepath->string, fs_gamedir, filename), filename);
}

/*
===========
FS_IsLog

Whether a file appended to is a log the logs' budget keeps: qconsole.log
or the game's g_log, in the server's own directory on a dedicated server
===========
*/
static qboolean FS_IsLog( const char *filename ) {
	const char	*name = FS_SkipPathPrefix( filename );
	const char	*dir = FS_ServerDir();
	size_t		length = strlen( dir );

	if ( !COM_CompareExtension( name, ".log" ) ) {
		return qfalse;
	}
	if ( length ) {
		if ( Q_stricmpn( name, dir, length ) || name[length] != '/' ) {
			return qfalse;
		}
		name += length + 1;
	}
	return !Q_stricmp( name, "qconsole.log" ) ||
		!Q_stricmp( name, FS_SkipPathPrefix( Cvar_VariableString( "g_log" ) ) );
}

/*
===========
FS_FOpenFileAppend_HomeData

===========
*/
fileHandle_t FS_FOpenFileAppend_HomeData( const char *filename ) {
	char			*ospath;
	fileHandle_t	f;
	qboolean		isLog;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	f = FS_HandleForFile();
	fsh[f].zipFile = qfalse;

	Q_strncpyz( fsh[f].name, filename, sizeof( fsh[f].name ) );

	// don't let sound stutter
	S_ClearSoundBuffer();

	ospath = FS_BuildOSPath( fs_homedatapath->string, fs_gamedir, filename );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_FOpenFileAppend_HomeData: %s\n", ospath );
	}

	FS_CheckFilenameIsMutable( ospath, __func__ );

	if( FS_CheckRefuses( __func__, ospath ) || FS_CreatePath( ospath ) ) {
		return 0;
	}

	// a log kept within the logs' budget, qconsole.log or the game's: it
	// may first start afresh; and isn't written while the disk is short of
	// com_logFreeSpace
	isLog = FS_IsLog( filename );
	if ( isLog ) {
		Log_Prepare( ospath );
	}

	fsh[f].handleFiles.file.o = Sys_FOpen( ospath, "ab" );
	fsh[f].handleSync = qfalse;
	if (!fsh[f].handleFiles.file.o) {
		f = 0;
	} else {
		fsh[f].isLog = isLog;
	}
	return f;
}

/*
===========
FS_FCreateOpenPipeFile

===========
*/
fileHandle_t FS_FCreateOpenPipeFile( const char *filename ) {
	char	    		*ospath;
	FILE					*fifo;
	fileHandle_t	f;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	f = FS_HandleForFile();
	fsh[f].zipFile = qfalse;

	Q_strncpyz( fsh[f].name, filename, sizeof( fsh[f].name ) );

	// don't let sound stutter
	S_ClearSoundBuffer();

	ospath = FS_BuildOSPath( fs_homedatapath->string, fs_gamedir, filename );

	if ( fs_debug->integer ) {
		Com_Printf( "FS_FCreateOpenPipeFile: %s\n", ospath );
	}

	FS_CheckFilenameIsMutable( ospath, __func__ );

	if ( FS_CheckRefuses( __func__, ospath ) ) {
		return 0;
	}
	// a dedicated server's is in its own directory, which may be new
	FS_CreatePath( ospath );
	fifo = Sys_Mkfifo( ospath );
	if( fifo ) {
		fsh[f].handleFiles.file.o = fifo;
		fsh[f].handleSync = qfalse;
	}
	else
	{
		Com_Printf( S_COLOR_YELLOW "WARNING: Could not create new com_pipefile at %s. "
			"com_pipefile will not be used.\n", ospath );
		f = 0;
	}

	return f;
}

/*
===========
FS_FilenameCompare

Ignore case and seprator char distinctions
===========
*/
qboolean FS_FilenameCompare( const char *s1, const char *s2 ) {
	int		c1, c2;

	do {
		c1 = *s1++;
		c2 = *s2++;

		if (c1 >= 'a' && c1 <= 'z') {
			c1 -= ('a' - 'A');
		}
		if (c2 >= 'a' && c2 <= 'z') {
			c2 -= ('a' - 'A');
		}

		if ( c1 == '\\' || c1 == ':' ) {
			c1 = '/';
		}
		if ( c2 == '\\' || c2 == ':' ) {
			c2 = '/';
		}

		if (c1 != c2) {
			return qtrue;		// strings not equal
		}
	} while (c1);

	return qfalse;		// strings are equal
}

/*
===========
FS_IsExt

Return qtrue if ext matches file extension filename
===========
*/

qboolean FS_IsExt(const char *filename, const char *ext, int namelen)
{
	int extlen;

	extlen = strlen(ext);

	if(extlen > namelen)
		return qfalse;

	filename += namelen - extlen;

	return !Q_stricmp(filename, ext);
}

/*
===========
FS_IsDemoExt

Return qtrue if filename has a demo extension
===========
*/

qboolean FS_IsDemoExt(const char *filename, int namelen)
{
	char *ext_test;
	int index, protocol;

	ext_test = strrchr(filename, '.');
	if(ext_test && !Q_stricmpn(ext_test + 1, DEMOEXT, ARRAY_LEN(DEMOEXT) - 1))
	{
		protocol = atoi(ext_test + ARRAY_LEN(DEMOEXT));

		if(protocol == com_protocol->integer)
			return qtrue;

#ifdef LEGACY_PROTOCOL
		if(protocol == com_legacyprotocol->integer)
			return qtrue;
#endif

		for(index = 0; demo_protocols[index]; index++)
		{
			if(demo_protocols[index] == protocol)
			return qtrue;
		}
	}

	return qfalse;
}

/*
================
FS_PakHasFile
================
*/
static qboolean FS_PakHasFile( const pack_t *pak, const char *name ) {
	const fileInPack_t	*file;

	for ( file = pak->hashTable[FS_HashFileName( name, pak->hashSize )]; file; file = file->next ) {
		if ( !FS_FilenameCompare( file->name, name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
===========
FS_CachePakRefuses

Whether a file is kept from being read in a pak from the content cache, a
download: its configs, which a server could fill with commands, but for
default.cfg, which a mod needs, and whose reading from this pak is said
once a mount
===========
*/
static qboolean FS_CachePakRefuses( pack_t *pak, const char *filename, qboolean opening ) {

	if ( !COM_CompareExtension( filename, ".cfg" ) ) {
		return qfalse;
	}
	if ( Q_stricmp( FS_SkipPathPrefix( filename ), "default.cfg" ) ) {
		return qtrue;
	}
	if ( !opening || pak->defaultCfgNoted ) {
		return qfalse;
	}
	// a search passes over every pak above the one that has it
	if ( FS_PakHasFile( pak, filename ) ) {
		pak->defaultCfgNoted = qtrue;
		Com_Printf( "Reading default.cfg from %s/%s.pk3, downloaded to the content cache\n",
			pak->pakGamename, pak->pakBasename );
	}
	return qfalse;
}

/*
===========
FS_FOpenFileReadDir

Tries opening file "filename" in searchpath "search"
Returns filesize and an open FILE pointer.
===========
*/
extern qboolean		com_fullyInitialized;

long FS_FOpenFileReadDir(const char *filename, searchpath_t *search, fileHandle_t *file, qboolean uniqueFILE, qboolean unpure)
{
	long			hash;
	pack_t		*pak;
	fileInPack_t	*pakFile;
	directory_t	*dir;
	char		*netpath;
	FILE		*filep;
	int			len;

	if(filename == NULL)
		Com_Error(ERR_FATAL, "FS_FOpenFileRead: NULL 'filename' parameter passed");

	// qpaths are not supposed to have a leading slash
	if(filename[0] == '/' || filename[0] == '\\')
		filename++;

	// make absolutely sure that it can't back up the path.
	// The searchpaths do guarantee that something will always
	// be prepended, so we don't need to worry about "c:" or "//limbo" 
	if(strstr(filename, ".." ) || strstr(filename, "::"))
	{
		if(file == NULL)
			return qfalse;

		*file = 0;
		return -1;
	}

	// make sure the q3key file is only readable by the quake3.exe at initialization
	// any other time the key should only be accessed in memory using the provided functions
	if(com_fullyInitialized && strstr(filename, "q3key"))
	{
		if(file == NULL)
			return qfalse;

		*file = 0;
		return -1;
	}

	if(search->pack && search->pack->cached && FS_CachePakRefuses(search->pack, filename, file != NULL))
	{
		if(file == NULL)
			return 0;

		*file = 0;
		return -1;
	}

	if(file == NULL)
	{
		// just wants to see if file is there

		// is the element a pak file?
		if(search->pack)
		{
			hash = FS_HashFileName(filename, search->pack->hashSize);

			if(search->pack->hashTable[hash])
			{
				// look through all the pak file elements
				pak = search->pack;
				pakFile = pak->hashTable[hash];

				do
				{
					// case and separator insensitive comparisons
					if(!FS_FilenameCompare(pakFile->name, filename))
					{
						// found it!
						if(pakFile->len)
							return pakFile->len;
						else
						{
							// It's not nice, but legacy code depends
							// on positive value if file exists no matter
							// what size
							return 1;
						}
					}

					pakFile = pakFile->next;
				} while(pakFile != NULL);
			}
		}
		else if(search->dir)
		{
			dir = search->dir;

			netpath = FS_BuildOSPath(dir->path, dir->gamedir, filename);
			filep = Sys_FOpen(netpath, "rb");

			if(filep)
			{
				len = FS_fplength(filep);
				fclose(filep);

				if(len)
					return len;
				else
					return 1;
			}
		}

		return 0;
	}

	*file = FS_HandleForFile();
	fsh[*file].handleFiles.unique = uniqueFILE;

	// is the element a pak file?
	if(search->pack)
	{
		hash = FS_HashFileName(filename, search->pack->hashSize);

		if(search->pack->hashTable[hash])
		{
			// disregard if it doesn't match one of the allowed pure pak files
			if(!unpure && !FS_PakIsPure(search->pack))
			{
				*file = 0;
				return -1;
			}

			// look through all the pak file elements
			pak = search->pack;
			pakFile = pak->hashTable[hash];

			do
			{
				// case and separator insensitive comparisons
				if(!FS_FilenameCompare(pakFile->name, filename))
				{
					// found it!

					// mark the pak as having been referenced and mark specifics on cgame and ui
					// shaders, txt, arena files  by themselves do not count as a reference as 
					// these are loaded from all pk3s 
					// from every pk3 file.. 
					len = strlen(filename);

					if (!(pak->referenced & FS_GENERAL_REF))
					{
						if(!FS_IsExt(filename, ".shader", len) &&
						   !FS_IsExt(filename, ".txt", len) &&
						   !FS_IsExt(filename, ".cfg", len) &&
						   !FS_IsExt(filename, ".config", len) &&
						   !FS_IsExt(filename, ".bot", len) &&
						   !FS_IsExt(filename, ".arena", len) &&
						   !FS_IsExt(filename, ".menu", len) &&
						   Q_stricmp(filename, "vm/qagame.qvm") != 0 &&
						   !strstr(filename, "levelshots"))
						{
							pak->referenced |= FS_GENERAL_REF;
						}
					}

					if(strstr(filename, "cgame.qvm"))
						pak->referenced |= FS_CGAME_REF;
					if(strstr(filename, "ui.qvm"))
						pak->referenced |= FS_UI_REF;

					if(uniqueFILE)
					{
						// open a new file on the pakfile
						fsh[*file].handleFiles.file.z = unzOpen(pak->pakFilename);

						if(fsh[*file].handleFiles.file.z == NULL)
							Com_Error(ERR_FATAL, "Couldn't open %s", pak->pakFilename);
					}
					else
						fsh[*file].handleFiles.file.z = pak->handle;

					Q_strncpyz(fsh[*file].name, filename, sizeof(fsh[*file].name));
					fsh[*file].zipFile = qtrue;

					// set the file position in the zip file (also sets the current file info)
					unzSetOffset(fsh[*file].handleFiles.file.z, pakFile->pos);

					// open the file in the zip
					unzOpenCurrentFile(fsh[*file].handleFiles.file.z);
					fsh[*file].zipFilePos = pakFile->pos;
					fsh[*file].zipFileLen = pakFile->len;

					if(fs_debug->integer)
					{
						Com_Printf("FS_FOpenFileRead: %s (found in '%s')\n", 
								filename, pak->pakFilename);
					}

					return pakFile->len;
				}

				pakFile = pakFile->next;
			} while(pakFile != NULL);
		}
	}
	else if(search->dir)
	{
		// check a file in the directory tree

		// if we are running restricted, the only files we
		// will allow to come from the directory are .cfg files
		len = strlen(filename);
		// FIXME TTimo I'm not sure about the fs_numServerPaks test
		// if you are using FS_ReadFile to find out if a file exists,
		//   this test can make the search fail although the file is in the directory
		// I had the problem on https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=8
		// turned out I used FS_FileExists_HomeData instead
		if(!unpure && fs_numServerPaks)
		{
			if(!FS_IsExt(filename, ".cfg", len) &&		// for config files
			   !FS_IsExt(filename, ".menu", len) &&		// menu files
			   !FS_IsExt(filename, ".game", len) &&		// menu files
			   !FS_IsExt(filename, ".dat", len) &&		// for journal files
			   !FS_IsDemoExt(filename, len))			// demos
			{
				*file = 0;
				return -1;
			}
		}

		dir = search->dir;

		netpath = FS_BuildOSPath(dir->path, dir->gamedir, filename);
		filep = Sys_FOpen(netpath, "rb");

		if (filep == NULL)
		{
			*file = 0;
			return -1;
		}

		Q_strncpyz(fsh[*file].name, filename, sizeof(fsh[*file].name));
		fsh[*file].zipFile = qfalse;

		if(fs_debug->integer)
		{
			Com_Printf("FS_FOpenFileRead: %s (found in '%s%c%s')\n", filename,
					dir->path, PATH_SEP, dir->gamedir);
		}

		fsh[*file].handleFiles.file.o = filep;
		return FS_fplength(filep);
	}

	*file = 0;
	return -1;
}

/*
===========
FS_SkipPathPrefix

A game path without its leading slashes and "./", which the search
ignores
===========
*/
const char *FS_SkipPathPrefix( const char *qpath ) {
	for ( ;; ) {
		if ( qpath[0] == '/' || qpath[0] == '\\' ) {
			qpath++;
		} else if ( qpath[0] == '.' && ( qpath[1] == '/' || qpath[1] == '\\' ) ) {
			qpath += 2;
		} else {
			return qpath;
		}
	}
}

/*
===========
FS_IsLocalConfig

Whether a file is autoexec.cfg or q3config.cfg, which only the player
writes, named as the search would find it: ignoring case, leading
slashes and "./"
===========
*/
static qboolean FS_IsLocalConfig( const char *filename ) {
	filename = FS_SkipPathPrefix( filename );
	return !Q_stricmp( filename, "autoexec.cfg" ) || !Q_stricmp( filename, Q3CONFIG_CFG );
}

/*
===========
FS_IsGameContent

Whether a search path is a pk3 or a pk3dir, which come with the game, a
mod or a download, rather than a game directory, where the player keeps
their own files
===========
*/
static qboolean FS_IsGameContent( const searchpath_t *search ) {
	return search->pack || FS_IsExt( search->dir->gamedir, ".pk3dir", strlen( search->dir->gamedir ) );
}

// where the last file FS_FOpenFileRead found was: whether in a pk3 or
// pk3dir, and which (FS_LastFilePath)
static const searchpath_t	*fs_lastFileSearch;

/*
===========
FS_LastFileIsGameContent

Whether the last file FS_FOpenFileRead, and so FS_ReadFile, found is in
a pk3 or a pk3dir
===========
*/
qboolean FS_LastFileIsGameContent( void ) {
	return fs_lastFileSearch && FS_IsGameContent( fs_lastFileSearch );
}

/*
===========
FS_LastFilePath

Where the last file FS_FOpenFileRead found, qpath, is: its path, or a
pak's and the file's name in it; "" if it found none
===========
*/
const char *FS_LastFilePath( const char *qpath ) {
	if ( !fs_lastFileSearch ) {
		return "";
	}
	if ( fs_lastFileSearch->pack ) {
		return va( "%s/%s", fs_lastFileSearch->pack->pakFilename, qpath );
	}
	return FS_BuildOSPath( fs_lastFileSearch->dir->path, fs_lastFileSearch->dir->gamedir, qpath );
}

/*
===========
FS_LastFileContent

The pk3 or pk3dir the last file FS_FOpenFileRead found is in, such as
"pak0.pk3", or "" when it's in a game directory
===========
*/
const char *FS_LastFileContent( void ) {
	static char	content[MAX_QPATH];

	content[0] = '\0';
	// the pack's basename, not its OS path, whose separator varies
	if ( !FS_LastFileIsGameContent() ) {
		return content;
	}
	if ( fs_lastFileSearch->pack ) {
		Q_strncpyz( content, fs_lastFileSearch->pack->pakBasename, sizeof( content ) - 4 );
		Q_strcat( content, sizeof( content ), ".pk3" );
	} else {
		Q_strncpyz( content, COM_SkipPath( fs_lastFileSearch->dir->gamedir ), sizeof( content ) );
	}
	return content;
}

/*
===========
FS_FOpenFileRead

Finds the file in the search path.
Returns filesize and an open FILE pointer.
Used for streaming data out of either a
separate file or a ZIP file.
===========
*/
long FS_FOpenFileRead(const char *filename, fileHandle_t *file, qboolean uniqueFILE)
{
	searchpath_t *search;
	long len;
	qboolean isLocalConfig;

	if(!fs_searchpaths)
		Com_Error(ERR_FATAL, "Filesystem call made without initialization");

	fs_lastFileSearch = NULL;
	isLocalConfig = FS_IsLocalConfig( filename );
	for(search = fs_searchpaths; search; search = search->next)
	{
		// autoexec.cfg and q3config.cfg can only be loaded outside of pk3
		// files and pk3dirs, which come with the game, a mod or a download
		if (isLocalConfig && FS_IsGameContent(search))
			continue;

		len = FS_FOpenFileReadDir(filename, search, file, uniqueFILE, qfalse);

		if(file == NULL ? len > 0 : len >= 0 && *file)
		{
			fs_lastFileSearch = search;
			return len;
		}

	}
	
#ifdef FS_MISSING
	if(missingFiles)
		fprintf(missingFiles, "%s\n", filename);
#endif

	if(file)
	{
		*file = 0;
		return -1;
	}
	else
	{
		// When file is NULL, we're querying the existence of the file
		// If we've got here, it doesn't exist
		return 0;
	}
}

/*
=================
FS_FindVM

Find a suitable VM file in search path order.

In each searchpath try:
 - open DLL file if DLL loading enabled
 - open QVM file

Enable search for DLL by setting enableDll to FSVM_ENABLEDLL

write found DLL or QVM to "found" and return VMI_NATIVE if DLL, VMI_COMPILED if QVM
Return the searchpath in "startSearch".
=================
*/

int FS_FindVM(void **startSearch, char *found, int foundlen, const char *name, int enableDll)
{
	searchpath_t *search, *lastSearch;
	directory_t *dir;
	pack_t *pack;
	char qvmName[MAX_OSPATH];
	char *netpath;

	if(!fs_searchpaths)
		Com_Error(ERR_FATAL, "Filesystem call made without initialization");

	Com_sprintf(qvmName, sizeof(qvmName), "vm/%s.qvm", name);

	lastSearch = *startSearch;
	if(*startSearch == NULL)
		search = fs_searchpaths;
	else
		search = lastSearch->next;

	while(search)
	{
		if(search->dir && !fs_numServerPaks)
		{
			dir = search->dir;

			if(enableDll)
			{
				// The original Q3 put the architecture in the library name; in case
				// we're loading an old binary only mod, fallback on this format if
				// the architecture-less library doesn't exist
				const char *dllNameFormats[] =
				{
					"%s" DLL_EXT,
					"%s" ARCH_STRING DLL_EXT
				};

				for(int i = 0; i < ARRAY_LEN(dllNameFormats); i++)
				{
					char dllName[MAX_OSPATH];
					Com_sprintf(dllName, sizeof(dllName), dllNameFormats[i], name);
					netpath = FS_BuildOSPath(dir->path, dir->gamedir, dllName);

					if(FS_FileInPathExists(netpath))
					{
						Q_strncpyz(found, netpath, foundlen);
						*startSearch = search;

						return VMI_NATIVE;
					}
				}
			}

			if(FS_FOpenFileReadDir(qvmName, search, NULL, qfalse, qfalse) > 0)
			{
				*startSearch = search;
				return VMI_COMPILED;
			}
		}
		else if(search->pack)
		{
			pack = search->pack;

			if(lastSearch && lastSearch->pack)
			{
				// make sure we only try loading one VM file per game dir
				// i.e. if VM from pak7.pk3 fails we won't try one from pak6.pk3

				if(!FS_FilenameCompare(lastSearch->pack->pakPathname, pack->pakPathname))
				{
					search = search->next;
					continue;
				}
			}

			if(FS_FOpenFileReadDir(qvmName, search, NULL, qfalse, qfalse) > 0)
			{
				*startSearch = search;

				return VMI_COMPILED;
			}
		}

		search = search->next;
	}

	return -1;
}

/*
=================
FS_Read

Properly handles partial reads
=================
*/
int FS_Read( void *buffer, int len, fileHandle_t f ) {
	int		block, remaining;
	int		read;
	byte	*buf;
	int		tries;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !f ) {
		return 0;
	}

	// fread and unzReadCurrentFile would take it as a huge count, and
	// read the rest of the file over the buffer
	if ( len < 0 ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: FS_Read: negative length %i\n", len );
		return 0;
	}

	buf = (byte *)buffer;
	fs_readCount += len;

	if (fsh[f].zipFile == qfalse) {
		remaining = len;
		tries = 0;
		while (remaining) {
			block = remaining;
			read = fread (buf, 1, block, fsh[f].handleFiles.file.o);
			if (read == 0) {
				// we might have been trying to read from a CD, which
				// sometimes returns a 0 read on windows
				if (!tries) {
					tries = 1;
				} else {
					return len-remaining;	//Com_Error (ERR_FATAL, "FS_Read: 0 bytes read");
				}
			}

			if (read == -1) {
				Com_Error (ERR_FATAL, "FS_Read: -1 bytes read");
			}

			remaining -= read;
			buf += read;
		}
		return len;
	} else {
		return unzReadCurrentFile(fsh[f].handleFiles.file.z, buffer, len);
	}
}

/*
=================
FS_Write

Properly handles partial writes
=================
*/
int FS_Write( const void *buffer, int len, fileHandle_t h ) {
	int		block, remaining;
	int		written;
	byte	*buf;
	int		tries;
	FILE	*f;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !h ) {
		return 0;
	}

	f = FS_FileForHandle(h);
	if ( fsh[h].writeError || ( fsh[h].isLog && Log_Paused() ) ) {
		return 0;
	}
	buf = (byte *)buffer;

	remaining = len;
	tries = 0;
	errno = 0;
	while (remaining) {
		block = remaining;
		written = fwrite (buf, 1, block, f);
		if (written == 0) {
			if (!tries) {
				tries = 1;
			} else {
				FS_FailWrite( h );
				return 0;
			}
		}

		remaining -= written;
		buf += written;
	}
	// a buffered write that failed to reach the disk can return in full,
	// leaving only the stream's error
	if ( ferror( f ) || ( fsh[h].handleSync && fflush( f ) ) ) {
		FS_FailWrite( h );
		return 0;
	}
	return len;
}

/*
=================
FS_WriteFailed

The errno of a write to h that failed, so that FS_Write drops its writes,
or -1 without one; 0 if none did
=================
*/
int FS_WriteFailed( fileHandle_t h ) {
	FS_FileForHandle( h );
	return fsh[h].writeError;
}

void QDECL FS_Printf( fileHandle_t h, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	FS_Write(msg, strlen(msg), h);
}

#define PK3_SEEK_BUFFER_SIZE 65536

/*
=================
FS_Seek

=================
*/
int FS_Seek( fileHandle_t f, long offset, int origin ) {
	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
		return -1;
	}

	if (fsh[f].zipFile == qtrue) {
		//FIXME: this is really, really crappy
		//(but better than what was here before)
		byte	buffer[PK3_SEEK_BUFFER_SIZE];
		int		remainder;
		int		currentPosition = FS_FTell( f );

		// change negative offsets into FS_SEEK_SET
		if ( offset < 0 ) {
			switch( origin ) {
				case FS_SEEK_END:
					remainder = fsh[f].zipFileLen + offset;
					break;

				case FS_SEEK_CUR:
					remainder = currentPosition + offset;
					break;

				case FS_SEEK_SET:
				default:
					remainder = 0;
					break;
			}

			if ( remainder < 0 ) {
				remainder = 0;
			}

			origin = FS_SEEK_SET;
		} else {
			if ( origin == FS_SEEK_END ) {
				remainder = fsh[f].zipFileLen - currentPosition + offset;
			} else {
				remainder = offset;
			}
		}

		switch( origin ) {
			case FS_SEEK_SET:
				if ( remainder == currentPosition ) {
					return offset;
				}
				unzSetOffset(fsh[f].handleFiles.file.z, fsh[f].zipFilePos);
				unzOpenCurrentFile(fsh[f].handleFiles.file.z);
				//fallthrough

			case FS_SEEK_END:
			case FS_SEEK_CUR:
				while( remainder > PK3_SEEK_BUFFER_SIZE ) {
					FS_Read( buffer, PK3_SEEK_BUFFER_SIZE, f );
					remainder -= PK3_SEEK_BUFFER_SIZE;
				}
				FS_Read( buffer, remainder, f );
				return offset;

			default:
				Com_Error( ERR_DROP, "Bad origin in FS_Seek" );
				return -1;
		}
	} else {
		FILE *file;
		file = FS_FileForHandle(f);
		int _origin = SEEK_SET;
		switch( origin ) {
		case FS_SEEK_CUR:
			_origin = SEEK_CUR;
			break;
		case FS_SEEK_END:
			_origin = SEEK_END;
			break;
		case FS_SEEK_SET:
			_origin = SEEK_SET;
			break;
		default:
			Com_Error( ERR_DROP, "Bad origin in FS_Seek" );
			break;
		}

		return fseek( file, offset, _origin );
	}
}


/*
======================================================================================

CONVENIENCE FUNCTIONS FOR ENTIRE FILES

======================================================================================
*/

int	FS_FileIsInPAK(const char *filename, int *pChecksum ) {
	searchpath_t	*search;
	pack_t			*pak;
	fileInPack_t	*pakFile;
	long			hash = 0;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !filename ) {
		Com_Error( ERR_FATAL, "FS_FOpenFileRead: NULL 'filename' parameter passed" );
	}

	// qpaths are not supposed to have a leading slash
	if ( filename[0] == '/' || filename[0] == '\\' ) {
		filename++;
	}

	// make absolutely sure that it can't back up the path.
	// The searchpaths do guarantee that something will always
	// be prepended, so we don't need to worry about "c:" or "//limbo" 
	if ( strstr( filename, ".." ) || strstr( filename, "::" ) ) {
		return -1;
	}

	//
	// search through the path, one element at a time
	//

	for ( search = fs_searchpaths ; search ; search = search->next ) {
		//
		if (search->pack) {
			hash = FS_HashFileName(filename, search->pack->hashSize);
		}
		// is the element a pak file?
		if ( search->pack && search->pack->hashTable[hash] ) {
			// disregard if it doesn't match one of the allowed pure pak files
			if ( !FS_PakIsPure(search->pack) ) {
				continue;
			}

			// look through all the pak file elements
			pak = search->pack;
			pakFile = pak->hashTable[hash];
			do {
				// case and separator insensitive comparisons
				if ( !FS_FilenameCompare( pakFile->name, filename ) ) {
					if (pChecksum) {
						*pChecksum = pak->pure_checksum;
					}
					return 1;
				}
				pakFile = pakFile->next;
			} while(pakFile != NULL);
		}
	}
	return -1;
}

/*
============
FS_ReadFileDir

Filename are relative to the quake search path
a null buffer will just return the file length without loading
If searchPath is non-NULL search only in that specific search path
============
*/
long FS_ReadFileDir(const char *qpath, void *searchPath, qboolean unpure, void **buffer)
{
	fileHandle_t	h;
	searchpath_t	*search;
	byte*			buf;
	qboolean		isConfig;
	long				len;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !qpath || !qpath[0] ) {
		Com_Error( ERR_FATAL, "FS_ReadFile with empty name" );
	}

	// until FS_FOpenFileRead finds it: a journal's file wasn't searched for
	fs_lastFileSearch = NULL;

	buf = NULL;	// quiet compiler warning

	// if this is a .cfg file and we are playing back a journal, read
	// it from the journal file
	if ( strstr( qpath, ".cfg" ) ) {
		isConfig = qtrue;
		if ( com_journal && com_journal->integer == 2 ) {
			int		r;

			Com_DPrintf( "Loading %s from journal file.\n", qpath );
			r = FS_Read( &len, sizeof( len ), com_journalDataFile );
			if ( r != sizeof( len ) ) {
				if (buffer != NULL) *buffer = NULL;
				return -1;
			}
			// if the file didn't exist when the journal was created
			if (!len) {
				if (buffer == NULL) {
					return 1;			// hack for old journal files
				}
				*buffer = NULL;
				return -1;
			}
			if (buffer == NULL) {
				return len;
			}

			buf = Hunk_AllocateTempMemory(len+1);
			*buffer = buf;

			r = FS_Read( buf, len, com_journalDataFile );
			if ( r != len ) {
				Com_Error( ERR_FATAL, "Read from journalDataFile failed" );
			}

			fs_loadCount++;
			fs_loadStack++;

			// guarantee that it will have a trailing 0 for string operations
			buf[len] = 0;

			return len;
		}
	} else {
		isConfig = qfalse;
	}

	search = searchPath;

	if(search == NULL)
	{
		// look for it in the filesystem or pack files
		len = FS_FOpenFileRead(qpath, &h, qfalse);
	}
	else
	{
		// look for it in a specific search path only
		len = FS_FOpenFileReadDir(qpath, search, &h, qfalse, unpure);
	}

	if ( h == 0 ) {
		if ( buffer ) {
			*buffer = NULL;
		}
		// if we are journalling and it is a config file, write a zero to the journal file
		if ( isConfig && com_journal && com_journal->integer == 1 ) {
			Com_DPrintf( "Writing zero for %s to journal file.\n", qpath );
			len = 0;
			FS_Write( &len, sizeof( len ), com_journalDataFile );
			FS_Flush( com_journalDataFile );
		}
		return -1;
	}

	if ( !buffer ) {
		if ( isConfig && com_journal && com_journal->integer == 1 ) {
			Com_DPrintf( "Writing len for %s to journal file.\n", qpath );
			FS_Write( &len, sizeof( len ), com_journalDataFile );
			FS_Flush( com_journalDataFile );
		}
		FS_FCloseFile( h);
		return len;
	}

	buf = Hunk_AllocateTempMemory(len+1);

	// a damaged or truncated pak can hold less than its directory says,
	// and the rest of the buffer would be whatever the hunk held before
	if ( FS_Read( buf, len, h ) != len ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: couldn't read all of %s\n", qpath );
		Hunk_FreeTempMemory( buf );
		FS_FCloseFile( h );
		*buffer = NULL;
		// a journal records it as missing, as playback will find it
		if ( isConfig && com_journal && com_journal->integer == 1 ) {
			len = 0;
			FS_Write( &len, sizeof( len ), com_journalDataFile );
			FS_Flush( com_journalDataFile );
		}
		return -1;
	}

	fs_loadCount++;
	fs_loadStack++;
	*buffer = buf;

	// guarantee that it will have a trailing 0 for string operations
	buf[len] = 0;
	FS_FCloseFile( h );

	// if we are journalling and it is a config file, write it to the journal file
	if ( isConfig && com_journal && com_journal->integer == 1 ) {
		Com_DPrintf( "Writing %s to journal file.\n", qpath );
		FS_Write( &len, sizeof( len ), com_journalDataFile );
		FS_Write( buf, len, com_journalDataFile );
		FS_Flush( com_journalDataFile );
	}
	return len;
}

/*
============
FS_ReadFile

Filename are relative to the quake search path
a null buffer will just return the file length without loading
============
*/
long FS_ReadFile(const char *qpath, void **buffer)
{
	return FS_ReadFileDir(qpath, NULL, qfalse, buffer);
}

/*
=============
FS_FreeFile
=============
*/
void FS_FreeFile( void *buffer ) {
	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}
	if ( !buffer ) {
		Com_Error( ERR_FATAL, "FS_FreeFile( NULL )" );
	}
	fs_loadStack--;

	Hunk_FreeTempMemory( buffer );

	// if all of our temp files are free, clear all of our space
	if ( fs_loadStack == 0 ) {
		Hunk_ClearTempMemory();
	}
}

/*
============
FS_WriteFile

Filename are relative to the quake search path
============
*/
void FS_WriteFile( const char *qpath, const void *buffer, int size ) {
	fileHandle_t f;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !qpath || !buffer ) {
		Com_Error( ERR_FATAL, "FS_WriteFile: NULL parameter" );
	}

#ifdef __EMSCRIPTEN__
	// a page's files are out of the player's reach: give them their
	// screenshots as downloads, and write no copy to the page's memory or
	// the browser's storage, where the home is kept between visits
	if ( !Q_stricmpn( qpath, "screenshots/", 12 ) ) {
		MAIN_THREAD_EM_ASM( { Module.saveFile?.( UTF8ToString( $0 ), HEAPU8.slice( $1, $1 + $2 ) ); },
			qpath, buffer, size );
		return;
	}
#endif

	f = FS_FOpenFileWrite_HomeData( qpath );
	if ( !f ) {
		Com_Printf( "Failed to open %s\n", qpath );
		return;
	}

	FS_Write( buffer, size, f );

	FS_FCloseFile( f );
}



/*
==========================================================================

ZIP FILE LOADING

==========================================================================
*/

/*
=================
FS_LoadZipFile

Creates a new pak_t in the search chain for the contents
of a zip file.
=================
*/
static pack_t *FS_LoadZipFile(const char *zipfile, const char *basename)
{
	fileInPack_t	*buildBuffer;
	pack_t			*pack;
	unzFile			uf;
	int				err;
	unz_global_info gi;
	char			filename_inzip[MAX_ZPATH];
	unz_file_info	file_info;
	int				i, len;
	long			hash;
	int				fs_numHeaderLongs;
	int				*fs_headerLongs;
	char			*namePtr;

	fs_numHeaderLongs = 0;

	uf = unzOpen(zipfile);
	err = unzGetGlobalInfo (uf,&gi);

	if (err != UNZ_OK)
		return NULL;

	len = 0;
	unzGoToFirstFile(uf);
	for (i = 0; i < gi.number_entry; i++)
	{
		err = unzGetCurrentFileInfo(uf, &file_info, filename_inzip, sizeof(filename_inzip), NULL, 0, NULL, 0);
		if (err != UNZ_OK) {
			break;
		}
		// unzip leaves a name that fills the buffer unterminated
		filename_inzip[sizeof(filename_inzip) - 1] = '\0';
		len += strlen(filename_inzip) + 1;
		unzGoToNextFile(uf);
	}

	buildBuffer = Z_Malloc( (gi.number_entry * sizeof( fileInPack_t )) + len );
	namePtr = ((char *) buildBuffer) + gi.number_entry * sizeof( fileInPack_t );
	fs_headerLongs = Z_Malloc( ( gi.number_entry + 1 ) * sizeof(int) );
	fs_headerLongs[ fs_numHeaderLongs++ ] = LittleLong( fs_checksumFeed );

	// get the hash table size from the number of files in the zip
	// because lots of custom pk3 files have less than 32 or 64 files
	for (i = 1; i <= MAX_FILEHASH_SIZE; i <<= 1) {
		if (i > gi.number_entry) {
			break;
		}
	}

	pack = Z_Malloc( sizeof( pack_t ) + i * sizeof(fileInPack_t *) );
	pack->hashSize = i;
	pack->hashTable = (fileInPack_t **) (((char *) pack) + sizeof( pack_t ));
	for(i = 0; i < pack->hashSize; i++) {
		pack->hashTable[i] = NULL;
	}

	Q_strncpyz( pack->pakFilename, zipfile, sizeof( pack->pakFilename ) );
	Q_strncpyz( pack->pakBasename, basename, sizeof( pack->pakBasename ) );

	// strip .pk3 if needed
	if ( strlen( pack->pakBasename ) > 4 && !Q_stricmp( pack->pakBasename + strlen( pack->pakBasename ) - 4, ".pk3" ) ) {
		pack->pakBasename[strlen( pack->pakBasename ) - 4] = 0;
	}

	pack->handle = uf;
	pack->numfiles = gi.number_entry;
	unzGoToFirstFile(uf);

	for (i = 0; i < gi.number_entry; i++)
	{
		err = unzGetCurrentFileInfo(uf, &file_info, filename_inzip, sizeof(filename_inzip), NULL, 0, NULL, 0);
		if (err != UNZ_OK) {
			break;
		}
		// unzip leaves a name that fills the buffer unterminated
		filename_inzip[sizeof(filename_inzip) - 1] = '\0';
		if (file_info.uncompressed_size > 0) {
			fs_headerLongs[fs_numHeaderLongs++] = LittleLong(file_info.crc);
		}
		Q_strlwr( filename_inzip );
		hash = FS_HashFileName(filename_inzip, pack->hashSize);
		buildBuffer[i].name = namePtr;
		strcpy( buildBuffer[i].name, filename_inzip );
		namePtr += strlen(filename_inzip) + 1;
		// store the file position in the zip
		buildBuffer[i].pos = unzGetOffset(uf);
		buildBuffer[i].len = file_info.uncompressed_size;
		buildBuffer[i].next = pack->hashTable[hash];
		pack->hashTable[hash] = &buildBuffer[i];
		unzGoToNextFile(uf);
	}

	pack->checksum = Com_BlockChecksum( &fs_headerLongs[ 1 ], sizeof(*fs_headerLongs) * ( fs_numHeaderLongs - 1 ) );
	pack->pure_checksum = Com_BlockChecksum( fs_headerLongs, sizeof(*fs_headerLongs) * fs_numHeaderLongs );
	pack->checksum = LittleLong( pack->checksum );
	pack->pure_checksum = LittleLong( pack->pure_checksum );

	Z_Free(fs_headerLongs);

	pack->buildBuffer = buildBuffer;
	return pack;
}

/*
=================
FS_FreePak

Frees a pak structure and releases all associated resources
=================
*/

static void FS_FreePak(pack_t *thepak)
{
	unzClose(thepak->handle);
	Z_Free(thepak->buildBuffer);
	Z_Free(thepak);
}

#define PAK_ADD_OVERRIDES		20		// files of another pak one overrides before it's said

/*
=================
FS_AddPakEntryRefused

Why content add refuses a pak's entry by its name, or NULL: a name that
could reach outside the game's directories, or native code
=================
*/
static const char *FS_AddPakEntryRefused( const char *name ) {
	size_t	length = strlen( name );

	if ( name[0] == '/' || strchr( name, '\\' ) || strchr( name, ':' ) || !strcmp( name, ".." )
		|| !Q_strncmp( name, "../", 3 ) || strstr( name, "/../" ) || ( length >= 3 && !strcmp( name + length - 3, "/.." ) ) ) {
		return "a name that leaves the game's directories";
	}
	if ( FS_IsNativeFile( name ) ) {
		return "native code";
	}
	return NULL;
}

/*
=================
FS_NativeMagic

Whether a file's first bytes are an executable's: Windows' (MZ), ELF, or
Mach-O, thin or fat
=================
*/
static qboolean FS_NativeMagic( const byte *b, int length ) {
	unsigned int	magic;

	if ( length >= 2 && b[0] == 'M' && b[1] == 'Z' ) {
		return qtrue;
	}
	if ( length < 4 ) {
		return qfalse;
	}
	magic = ( (unsigned int)b[0] << 24 ) | ( b[1] << 16 ) | ( b[2] << 8 ) | b[3];
	return magic == 0x7f454c46 || magic == 0xfeedface || magic == 0xfeedfacf || magic == 0xcefaedfe
		|| magic == 0xcffaedfe || magic == 0xcafebabe || magic == 0xbebafeca || magic == 0xcafebabf
		|| magic == 0xbfbafeca;
}

/*
=================
FS_CheckAddPak

What content add checks of a pak an operator installs as gameDir/name.pk3:
a zip that loads as a pak, none of whose entries could reach outside the
game's directories, is encrypted or is native code, by its name or its
first bytes, within CONTENT_MAX_PAK_SIZE unpacked; and not one of the
game's own. NULL, with the pak's checksum, or why it's refused. In
overrides, the paks of the search path it would take code (a QVM in vm/) or
more than PAK_ADD_OVERRIDES files from, said as a warning, or ""
=================
*/
const char *FS_CheckAddPak( const char *ospath, const char *gameDir, const char *name,
	unsigned int *checksum, char *overrides, int overridesSize ) {
	pack_t			*pak;
	const searchpath_t	*search;
	unz_file_info	info;
	char			entry[MAX_ZPATH], retail[MAX_QPATH];
	const char		*why = NULL, *code;
	byte			first[4];
	uint64_t		unpacked = 0;
	int				i, got, count;

	overrides[0] = '\0';
	pak = FS_LoadZipFile( ospath, name );
	if ( !pak ) {
		return "it isn't a zip that loads as a pak";
	}
	unzGoToFirstFile( pak->handle );
	for ( i = 0; i < pak->numfiles; i++, unzGoToNextFile( pak->handle ) ) {
		if ( unzGetCurrentFileInfo( pak->handle, &info, entry, sizeof( entry ), NULL, 0, NULL, 0 ) != UNZ_OK ) {
			why = "its directory can't be read";
			break;
		}
		// a name the buffer cuts short, whose end could be anything
		if ( info.size_filename >= sizeof( entry ) ) {
			entry[sizeof( entry ) - 1] = '\0';
			why = va( "%s... has a name longer than %d characters", entry, MAX_ZPATH - 1 );
			break;
		}
		if ( ( why = FS_AddPakEntryRefused( entry ) ) != NULL ) {
			why = va( "%s holds %s", entry, why );
			break;
		}
		if ( info.flag & 1 ) {
			why = va( "%s is encrypted", entry );
			break;
		}
		unpacked += info.uncompressed_size;
		if ( unpacked > CONTENT_MAX_PAK_SIZE ) {
			why = va( "it unpacks to more than %d MB", CONTENT_MAX_PAK_SIZE >> 20 );
			break;
		}
		if ( !info.uncompressed_size ) {
			continue;
		}
		// what the engine can't read, another unzip may: compressed some
		// other way, or damaged
		if ( unzOpenCurrentFile( pak->handle ) != UNZ_OK ) {
			why = va( "%s can't be read", entry );
			break;
		}
		got = unzReadCurrentFile( pak->handle, first, sizeof( first ) );
		unzCloseCurrentFile( pak->handle );
		if ( got < (int)MIN( info.uncompressed_size, sizeof( first ) ) ) {
			why = va( "%s can't be read", entry );
			break;
		}
		if ( FS_NativeMagic( first, got ) ) {
			why = va( "%s is native code", entry );
			break;
		}
	}
	*checksum = pak->checksum;
	if ( !why && FS_RetailPakChecksum( va( "%s/%s.pk3", gameDir, name ), pak->checksum, retail, sizeof( retail ) ) ) {
		why = va( "it's one of the game's own paks, %s, which players add from their own copies", retail );
	}
	// a pak of its name in another of the game's directories, as the base
	// path's, with other contents: players know a pak by its name
	for ( search = fs_searchpaths; search && !why; search = search->next ) {
		const pack_t	*other = search->pack;

		if ( other && !Q_stricmp( other->pakGamename, gameDir ) && !Q_stricmp( other->pakBasename, name )
			&& other->checksum != pak->checksum ) {
			why = va( "a pak named %s/%s.pk3 is already installed with different contents (%s); give the new "
				"version its own name", gameDir, name, other->pakFilename );
		}
	}

	// the paks it would override: the game's paks before it by name, and
	// those of the base game under a mod
	for ( search = fs_searchpaths; search && !why; search = search->next ) {
		const pack_t	*other = search->pack;

		if ( !other || !( ( !Q_stricmp( other->pakGamename, gameDir ) && Q_stricmp( other->pakBasename, name ) < 0 )
			|| ( Q_stricmp( gameDir, com_basegame->string ) && !Q_stricmp( other->pakGamename, com_basegame->string ) ) ) ) {
			continue;
		}
		// the files they share, looked up from the one with fewer
		count = 0;
		code = NULL;
		for ( i = 0; i < MIN( pak->numfiles, other->numfiles ); i++ ) {
			const char	*file = pak->numfiles <= other->numfiles ? pak->buildBuffer[i].name : other->buildBuffer[i].name;

			if ( FS_PakHasFile( pak->numfiles <= other->numfiles ? other : pak, file ) ) {
				count++;
				if ( !code && !Q_strncmp( file, "vm/", 3 ) && COM_CompareExtension( file, ".qvm" ) ) {
					code = file;
				}
			}
		}
		if ( code || count > PAK_ADD_OVERRIDES ) {
			Q_strcat( overrides, overridesSize, va( "%s%s.pk3 overrides %d files of %s/%s.pk3%s%s", overrides[0] ? "; " : "",
				name, count, other->pakGamename, other->pakBasename, code ? ", including " : "", code ? code : "" ) );
		}
	}
	FS_FreePak( pak );
	return why;
}

/*
=================
FS_CompareZipChecksum

Whether the given file is a pak with the checksum given, as the server
lists those of its referenced paks
=================
*/
qboolean FS_CompareZipChecksum(const char *zipfile, unsigned int checksum)
{
	pack_t *thepak;
	qboolean matches;

	thepak = FS_LoadZipFile(zipfile, "");

	if(!thepak)
		return qfalse;

	matches = thepak->checksum == checksum;
	FS_FreePak(thepak);

	return matches;
}

/*
=================================================================================

DIRECTORY SCANNING FUNCTIONS

=================================================================================
*/

#define	MAX_FOUND_FILES	0x1000

static int FS_ReturnPath( const char *zname, char *zpath, int *depth ) {
	int len, at, newdep;

	newdep = 0;
	zpath[0] = 0;
	len = 0;
	at = 0;

	while(zname[at] != 0)
	{
		if (zname[at]=='/' || zname[at]=='\\') {
			len = at;
			newdep++;
		}
		at++;
	}
	strcpy(zpath, zname);
	zpath[len] = 0;
	*depth = newdep;

	return len;
}

/*
==================
FS_AddFileToList
==================
*/
static int FS_AddFileToList( char *name, char *list[MAX_FOUND_FILES], int nfiles ) {
	int		i;

	if ( nfiles == MAX_FOUND_FILES - 1 ) {
		return nfiles;
	}
	for ( i = 0 ; i < nfiles ; i++ ) {
		if ( !Q_stricmp( name, list[i] ) ) {
			return nfiles;		// already in list
		}
	}
	list[nfiles] = CopyString( name );
	nfiles++;

	return nfiles;
}

/*
===============
FS_ListFilteredFiles

Returns a uniqued list of files that match the given criteria
from all search paths
===============
*/
char **FS_ListFilteredFiles( const char *path, const char *extension,
	char *filter, int *numfiles, qboolean stripPath,
	qboolean allowNonPureFilesOnDisk ) {
	int				nfiles;
	char			**listCopy;
	char			*list[MAX_FOUND_FILES];
	searchpath_t	*search;
	int				i;
	int				pathLength;
	int				extensionLength;
	int				length, pathDepth, pathSkip;
	pack_t			*pak;
	fileInPack_t	*buildBuffer;
	char			zpath[MAX_ZPATH];

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !path ) {
		*numfiles = 0;
		return NULL;
	}
	if ( !extension ) {
		extension = "";
	}

	pathLength = strlen( path );
	if ( pathLength && ( path[pathLength-1] == '\\' || path[pathLength-1] == '/' ) ) {
		pathLength--;
	}
	extensionLength = strlen( extension );
	nfiles = 0;
	FS_ReturnPath(path, zpath, &pathDepth);

	//
	// search through the path, one element at a time, adding to list
	//
	for (search = fs_searchpaths ; search ; search = search->next) {
		// is the element a pak file?
		if (search->pack) {

			//ZOID:  If we are pure, don't search for files on paks that
			// aren't on the pure list
			if ( !FS_PakIsPure(search->pack) ) {
				continue;
			}

			// look through all the pak file elements
			pak = search->pack;
			buildBuffer = pak->buildBuffer;
			for (i = 0; i < pak->numfiles; i++) {
				char	*name;
				int		zpathLen, depth;

				// check for directory match
				name = buildBuffer[i].name;

				if (stripPath) {
					pathSkip = pathLength;
					if (pathLength) {
						pathSkip++; // include the '/'
					}
				} else {
					pathSkip = 0;
				}

				if (filter) {
					// case insensitive
					if (!Com_FilterPath( filter, name, qfalse ))
						continue;

					// unique the match
					nfiles = FS_AddFileToList( name + pathSkip, list, nfiles );
				}
				else {

					zpathLen = FS_ReturnPath(name, zpath, &depth);

					if ( (depth-pathDepth)>2 || pathLength > zpathLen || Q_stricmpn( name, path, pathLength ) ) {
						continue;
					}

					// check for extension match
					length = strlen( name );
					if ( length < extensionLength ) {
						continue;
					}

					if ( Q_stricmp( name + length - extensionLength, extension ) ) {
						continue;
					}

					// unique the match
					nfiles = FS_AddFileToList( name + pathSkip, list, nfiles );
				}
			}
		} else if (search->dir) { // scan for files in the filesystem
			char	*netpath;
			int		numSysFiles;
			char	**sysFiles;
			char	*name;

			// don't scan directories for files if we are pure or restricted
			if ( fs_numServerPaks && !allowNonPureFilesOnDisk ) {
				continue;
			} else {
				netpath = FS_BuildOSPath( search->dir->path, search->dir->gamedir, path );
				sysFiles = Sys_ListFiles( netpath, extension, filter, &numSysFiles, qfalse );
				for ( i = 0 ; i < numSysFiles ; i++ ) {
					// unique the match
					name = sysFiles[i];
					nfiles = FS_AddFileToList( name, list, nfiles );
				}
				Sys_FreeFileList( sysFiles );
			}
		}
	}

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
=================
FS_ListFiles
=================
*/
char **FS_ListFiles( const char *path, const char *extension, int *numfiles ) {
	return FS_ListFilteredFiles( path, extension, NULL, numfiles, qtrue, qfalse );
}

/*
=================
FS_FreeFileList
=================
*/
void FS_FreeFileList( char **list ) {
	int		i;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}

	if ( !list ) {
		return;
	}

	for ( i = 0 ; list[i] ; i++ ) {
		Z_Free( list[i] );
	}

	Z_Free( list );
}


/*
================
FS_GetFileList
================
*/
int	FS_GetFileList(  const char *path, const char *extension, char *listbuf, int bufsize ) {
	int		nFiles, i, nTotal, nLen;
	char **pFiles = NULL;

	*listbuf = 0;
	nFiles = 0;
	nTotal = 0;

	if (Q_stricmp(path, "$modlist") == 0) {
		return FS_GetModList(listbuf, bufsize);
	}

	pFiles = FS_ListFiles(path, extension, &nFiles);

	for (i =0; i < nFiles; i++) {
		nLen = strlen(pFiles[i]) + 1;
		if (nTotal + nLen + 1 < bufsize) {
			strcpy(listbuf, pFiles[i]);
			listbuf += nLen;
			nTotal += nLen;
		}
		else {
			nFiles = i;
			break;
		}
	}

	FS_FreeFileList(pFiles);

	return nFiles;
}

/*
=======================
Sys_ConcatenateFileLists

mkv: Naive implementation. Concatenates three lists into a
     new list, and frees the old lists from the heap.
bk001129 - from cvs1.17 (mkv)

FIXME TTimo those two should move to common.c next to Sys_ListFiles
=======================
 */
static unsigned int Sys_CountFileList(char **list)
{
	int i = 0;

	if (list)
	{
		while (*list)
		{
			list++;
			i++;
		}
	}
	return i;
}

static char** Sys_ConcatenateFileLists( char **list0, char **list1 )
{
	int totalLength = 0;
	char** cat = NULL, **dst, **src;

	totalLength += Sys_CountFileList(list0);
	totalLength += Sys_CountFileList(list1);

	/* Create new list. */
	dst = cat = Z_Malloc( ( totalLength + 1 ) * sizeof( char* ) );

	/* Copy over lists. */
	if (list0)
	{
		for (src = list0; *src; src++, dst++)
			*dst = *src;
	}
	if (list1)
	{
		for (src = list1; *src; src++, dst++)
			*dst = *src;
	}

	// Terminate the list
	*dst = NULL;

	// Free our old lists.
	// NOTE: not freeing their content, it's been merged in dst and still being used
	if (list0) Z_Free( list0 );
	if (list1) Z_Free( list1 );

	return cat;
}

/*
================
FS_GetModDescription
================
*/
void FS_GetModDescription( const char *modDir, char *description, int descriptionLen ) {
	fileHandle_t	descHandle;
	char			descPath[MAX_QPATH];
	int				nDescLen;
	FILE			*file;

	Com_sprintf( descPath, sizeof ( descPath ), "%s%cdescription.txt", modDir, PATH_SEP );
	nDescLen = FS_BaseDir_FOpenFileRead( descPath, &descHandle );

	if ( nDescLen > 0 ) {
		file = FS_FileForHandle(descHandle);
		Com_Memset( description, 0, descriptionLen );
		nDescLen = fread(description, 1, descriptionLen, file);
		if (nDescLen >= 0) {
			description[nDescLen] = '\0';
		}
	} else {
		Q_strncpyz( description, modDir, descriptionLen );
	}

	if ( descHandle ) {
		FS_FCloseFile( descHandle );
	}
}

/*
================
FS_GetModList

Returns a list of mod directory names
A mod directory is a peer to baseq3 with a pk3 or pk3dir in it
================
*/
int	FS_GetModList( char *listbuf, int bufsize ) {
	int nMods, i, j, k, nTotal, nLen, nPaks = 0, nDirs = 0, nPakDirs = 0, nPotential, nDescLen;
	char **pFiles = NULL;
	char **pPaks = NULL;
	char **pDirs = NULL;
	char *name, *path;
	char description[MAX_OSPATH];

	int dummy;
	char **pFiles0 = NULL;
	qboolean bDrop = qfalse;

	*listbuf = 0;
	nMods = nTotal = 0;

	// iterate through paths and get list of potential mods
	for (i = 0; i < ARRAY_LEN(fs_pathVars); i++) {
		const cvar_t *pathVar = fs_pathVars[i];

		if (!pathVar || !pathVar->string[0]) {
			continue;
		}

		pFiles0 = Sys_ListFiles(pathVar->string, NULL, NULL, &dummy, qtrue);
		// Sys_ConcatenateFileLists frees the lists so Sys_FreeFileList isn't required
		pFiles = Sys_ConcatenateFileLists(pFiles, pFiles0);
	}

	nPotential = Sys_CountFileList(pFiles);

	for (i = 0; i < nPotential; i++) {
		name = pFiles[i];
		// NOTE: cleaner would involve more changes
		// ignore duplicate mod directories
		if (i != 0) {
			bDrop = qfalse;
			for (j = 0; j < i; j++) {
				if (Q_stricmp(pFiles[j], name) == 0) {
					// this one can be dropped
					bDrop = qtrue;
					break;
				}
			}
		}
		// we also drop "baseq3" "." and ".."
		if (bDrop || Q_stricmp(name, com_basegame->string) == 0 || Q_stricmpn(name, ".", 1) == 0) {
			continue;
		}

		// in order to be a valid mod the directory must contain at least one .pk3 or .pk3dir
		// we didn't keep the information when we merged the directory names, as to what OS Path it was found under
		// so we will try each of them here
		for (j = 0; j < ARRAY_LEN(fs_pathVars); j++) {
			const cvar_t *pathVar = fs_pathVars[j];

			if (!pathVar || !pathVar->string[0]) {
				continue;
			}

			path = FS_BaseDir_BuildOSPath(pathVar->string, name);
			nPaks = nDirs = nPakDirs = 0;
			pPaks = Sys_ListFiles(path, ".pk3", NULL, &nPaks, qfalse);
			pDirs = Sys_ListFiles(path, "/", NULL, &nDirs, qfalse);
			for (k = 0; k < nDirs; k++) {
				// we only want to count directories ending with ".pk3dir"
				if (FS_IsExt(pDirs[k], ".pk3dir", strlen(pDirs[k]))) {
					nPakDirs++;
				}
			}
			// we only use Sys_ListFiles to check whether files are present
			Sys_FreeFileList(pPaks);
			Sys_FreeFileList(pDirs);

			if (nPaks > 0 || nPakDirs > 0) {
				break;
			}
		}

		if (nPaks > 0 || nPakDirs > 0) {
			nLen = strlen(name) + 1;
			// nLen is the length of the mod path
			// we need to see if there is a description available
			FS_GetModDescription(name, description, sizeof(description));
			nDescLen = strlen(description) + 1;

			if (nTotal + nLen + 1 + nDescLen + 1 < bufsize) {
				strcpy(listbuf, name);
				listbuf += nLen;
				strcpy(listbuf, description);
				listbuf += nDescLen;
				nTotal += nLen + nDescLen;
				nMods++;
			} else {
				break;
			}
		}
	}
	Sys_FreeFileList( pFiles );

	return nMods;
}




//============================================================================

/*
================
FS_Dir_f
================
*/
void FS_Dir_f( void ) {
	char	*path;
	char	*extension;
	char	**dirnames;
	int		ndirs;
	int		i;

	if ( Cmd_Argc() < 2 || Cmd_Argc() > 3 ) {
		Com_Printf( "usage: dir <directory> [extension]\n" );
		return;
	}

	if ( Cmd_Argc() == 2 ) {
		path = Cmd_Argv( 1 );
		extension = "";
	} else {
		path = Cmd_Argv( 1 );
		extension = Cmd_Argv( 2 );
	}

	Com_Printf( "Directory of %s %s\n", path, extension );
	Com_Printf( "---------------\n" );

	dirnames = FS_ListFiles( path, extension, &ndirs );

	for ( i = 0; i < ndirs; i++ ) {
		Com_Printf( "%s\n", dirnames[i] );
	}
	FS_FreeFileList( dirnames );
}

/*
===========
FS_ConvertPath
===========
*/
void FS_ConvertPath( char *s ) {
	while (*s) {
		if ( *s == '\\' || *s == ':' ) {
			*s = '/';
		}
		s++;
	}
}

/*
===========
FS_PathCmp

Ignore case and seprator char distinctions
===========
*/
int FS_PathCmp( const char *s1, const char *s2 ) {
	int		c1, c2;

	do {
		c1 = *s1++;
		c2 = *s2++;

		if (c1 >= 'a' && c1 <= 'z') {
			c1 -= ('a' - 'A');
		}
		if (c2 >= 'a' && c2 <= 'z') {
			c2 -= ('a' - 'A');
		}

		if ( c1 == '\\' || c1 == ':' ) {
			c1 = '/';
		}
		if ( c2 == '\\' || c2 == ':' ) {
			c2 = '/';
		}
		
		if (c1 < c2) {
			return -1;		// strings not equal
		}
		if (c1 > c2) {
			return 1;
		}
	} while (c1);
	
	return 0;		// strings are equal
}

/*
================
FS_SortFileList
================
*/
void FS_SortFileList(char **filelist, int numfiles) {
	int i, j, k, numsortedfiles;
	char **sortedlist;

	sortedlist = Z_Malloc( ( numfiles + 1 ) * sizeof( *sortedlist ) );
	sortedlist[0] = NULL;
	numsortedfiles = 0;
	for (i = 0; i < numfiles; i++) {
		for (j = 0; j < numsortedfiles; j++) {
			if (FS_PathCmp(filelist[i], sortedlist[j]) < 0) {
				break;
			}
		}
		for (k = numsortedfiles; k > j; k--) {
			sortedlist[k] = sortedlist[k-1];
		}
		sortedlist[j] = filelist[i];
		numsortedfiles++;
	}
	Com_Memcpy(filelist, sortedlist, numfiles * sizeof( *filelist ) );
	Z_Free(sortedlist);
}

/*
================
FS_NewDir_f
================
*/
void FS_NewDir_f( void ) {
	char	*filter;
	char	**dirnames;
	int		ndirs;
	int		i;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: fdir <filter>\n" );
		Com_Printf( "example: fdir *q3dm*.bsp\n");
		return;
	}

	filter = Cmd_Argv( 1 );

	Com_Printf( "---------------\n" );

	dirnames = FS_ListFilteredFiles( "", "", filter, &ndirs, qfalse, qfalse );

	FS_SortFileList(dirnames, ndirs);

	for ( i = 0; i < ndirs; i++ ) {
		FS_ConvertPath(dirnames[i]);
		Com_Printf( "%s\n", dirnames[i] );
	}
	Com_Printf( "%d files listed\n", ndirs );
	FS_FreeFileList( dirnames );
}

/*
============
FS_AppendSearchDirs

Appends the search path's directories, game's alone where it isn't NULL,
each between before and after
============
*/
static void FS_AppendSearchDirs( char *buf, int size, const char *game, const char *before, const char *after ) {
	searchpath_t	*s;

	for ( s = fs_searchpaths; s; s = s->next ) {
		if ( s->dir && ( !game || !Q_stricmp( s->dir->gamedir, game ) ) ) {
			Q_strcat( buf, size, before );
			Q_strcat( buf, size, s->dir->fullpath );
			Q_strcat( buf, size, after );
		}
	}
}

/*
============
FS_SearchDirs

The search path's directories, quoted, for saying where a file wasn't
found: "a", "b" or their paks; and the predecessor's homes where
FS_ReadPredecessorConfig looked for predecessorFile too (NULL when it
wasn't asked to)
============
*/
const char *FS_SearchDirs( const char *predecessorFile ) {
	static char	dirs[MAXPRINTMSG];
	int			length;

	dirs[0] = '\0';
	FS_AppendSearchDirs( dirs, sizeof( dirs ), NULL, "\"", "\", " );
	length = strlen( dirs );
	if ( length >= 2 && !strcmp( dirs + length - 2, ", " ) ) {
		dirs[length - 2] = '\0';
	}
	Q_strcat( dirs, sizeof( dirs ), dirs[0] ? " or their paks" : "the search path" );

	if ( predecessorFile && FS_PredecessorReadsConfig( predecessorFile ) ) {
		Q_strcat( dirs, sizeof( dirs ), va( ", nor in the predecessor's home \"%s\"",
			fs_predecessorpath->string ) );
		if ( fs_predecessorConfigPath[0] && Q_stricmp( fs_predecessorConfigPath, fs_predecessorpath->string ) ) {
			Q_strcat( dirs, sizeof( dirs ), va( " or \"%s\"", fs_predecessorConfigPath ) );
		}
	}
	return dirs;
}

/*
============
FS_Path_f

============
*/
void FS_Path_f( void ) {
	searchpath_t	*s;
	int				i;

	Com_Printf ("We are looking in the current search path:\n");
	for (s = fs_searchpaths; s; s = s->next) {
		if (s->pack) {
			Com_Printf ("%s (%i files)\n", s->pack->pakFilename, s->pack->numfiles);
			if ( s->pack->cached ) {
				Com_Printf( "    from the content cache, as %s/%s.pk3\n", s->pack->pakGamename, s->pack->pakBasename );
			}
			if ( fs_numServerPaks ) {
				if ( !FS_PakIsPure(s->pack) ) {
					Com_Printf( "    not on the pure list\n" );
				} else {
					Com_Printf( "    on the pure list\n" );
				}
			}
		} else {
			Com_Printf ("%s%c%s\n", s->dir->path, PATH_SEP, s->dir->gamedir );
		}
	}


	Com_Printf( "\n" );
	for ( i = 1 ; i < MAX_FILE_HANDLES ; i++ ) {
		if ( fsh[i].handleFiles.file.o ) {
			Com_Printf( "handle %i: %s\n", i, fsh[i].name );
		}
	}
}

/*
============
FS_TouchFile_f
============
*/
void FS_TouchFile_f( void ) {
	fileHandle_t	f;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "Usage: touchFile <file>\n" );
		return;
	}

	FS_FOpenFileRead( Cmd_Argv( 1 ), &f, qfalse );
	if ( f ) {
		FS_FCloseFile( f );
	}
}

/*
============
FS_Which
============
*/

qboolean FS_Which(const char *filename, void *searchPath)
{
	searchpath_t *search = searchPath;

	if(FS_FOpenFileReadDir(filename, search, NULL, qfalse, qfalse) > 0)
	{
		if(search->pack)
		{
			Com_Printf("File \"%s\" found in \"%s\"\n", filename, search->pack->pakFilename);
			return qtrue;
		}
		else if(search->dir)
		{
			Com_Printf( "File \"%s\" found at \"%s\"\n", filename, search->dir->fullpath);
			return qtrue;
		}
	}

	return qfalse;
}

/*
============
FS_Which_f
============
*/
void FS_Which_f( void ) {
	searchpath_t	*search;
	char		*filename;

	filename = Cmd_Argv(1);

	if ( !filename[0] ) {
		Com_Printf( "Usage: which <file>\n" );
		return;
	}

	// qpaths are not supposed to have a leading slash
	if ( filename[0] == '/' || filename[0] == '\\' ) {
		filename++;
	}

	// just wants to see if file is there
	for(search = fs_searchpaths; search; search = search->next)
	{
		if(FS_Which(filename, search))
			return;
	}

	Com_Printf("File not found: \"%s\"\n", filename);
}


//===========================================================================


static int QDECL paksort( const void *a, const void *b ) {
	char	*aa, *bb;

	aa = *(char **)a;
	bb = *(char **)b;

	return FS_PathCmp( aa, bb );
}

/*
================
FS_AddGameDirectory

Sets fs_gamedir, adds the directory to the head of the path,
then loads the zip headers
================
*/
static void FS_AddGameDirectory( const char *path, const char *dir, qboolean paksOnly ) {
	searchpath_t	*sp;
	searchpath_t	*search;
	pack_t			*pak;
	char			curpath[MAX_OSPATH + 1], *pakfile;
	int				numfiles;
	char			**pakfiles;
	int				pakfilesi;
	char			**pakfilestmp;
	int				numdirs;
	char			**pakdirs;
	int				pakdirsi;
	char			**pakdirstmp;

	int				pakwhich;
	int				len;

	// find all pak files in this directory
	Q_strncpyz(curpath, FS_BaseDir_BuildOSPath(path, dir), sizeof(curpath));

	// Unique; a directory read only for its paks has no entry of its own,
	// so it's known by them
	for ( sp = fs_searchpaths ; sp ; sp = sp->next ) {
		if ( sp->dir && !Q_stricmp(sp->dir->path, path) && !Q_stricmp(sp->dir->gamedir, dir)) {
			return;			// we've already got this one
		}
		if ( paksOnly && sp->pack && !Q_stricmp( sp->pack->pakPathname, curpath ) ) {
			return;
		}
	}

	if ( !paksOnly ) {
		Q_strncpyz( fs_gamedir, dir, sizeof( fs_gamedir ) );
	}

	// Get .pk3 files
	pakfiles = Sys_ListFiles(curpath, ".pk3", NULL, &numfiles, qfalse);

	if ( pakfiles ) {
		qsort( pakfiles, numfiles, sizeof(char*), paksort );
	}

	if ( fs_numServerPaks || paksOnly ) {
		numdirs = 0;
		pakdirs = NULL;
	} else {
		// Get top level directories (we'll filter them later since the Sys_ListFiles filtering is terrible)
		pakdirs = Sys_ListFiles(curpath, "/", NULL, &numdirs, qfalse);

		if ( pakdirs ) {
			qsort( pakdirs, numdirs, sizeof(char *), paksort );
		}
	}

	pakfilesi = 0;
	pakdirsi = 0;

	while((pakfilesi < numfiles) || (pakdirsi < numdirs))
	{
		// Check if a pakfile or pakdir comes next
		if (pakfilesi >= numfiles) {
			// We've used all the pakfiles, it must be a pakdir.
			pakwhich = 0;
		}
		else if (pakdirsi >= numdirs) {
			// We've used all the pakdirs, it must be a pakfile.
			pakwhich = 1;
		}
		else {
			// Could be either, compare to see which name comes first
			// Need tmp variables for appropriate indirection for paksort()
			pakfilestmp = &pakfiles[pakfilesi];
			pakdirstmp = &pakdirs[pakdirsi];
			pakwhich = (paksort(pakfilestmp, pakdirstmp) < 0);
		}

		if (pakwhich) {
			// The next .pk3 file is before the next .pk3dir
			pakfile = FS_BuildOSPath(path, dir, pakfiles[pakfilesi]);
			if ((pak = FS_LoadZipFile(pakfile, pakfiles[pakfilesi])) == 0) {
				// This isn't a .pk3, or it couldn't be opened! Next!
				Com_Printf( S_COLOR_YELLOW "WARNING: couldn't open %s as a pak, so it's left out\n", pakfile );
				pakfilesi++;
				continue;
			}

			Q_strncpyz(pak->pakPathname, curpath, sizeof(pak->pakPathname));
			// store the game name for downloading
			Q_strncpyz(pak->pakGamename, dir, sizeof(pak->pakGamename));

			fs_packFiles += pak->numfiles;

			search = Z_Malloc(sizeof(searchpath_t));
			search->pack = pak;
			search->next = fs_searchpaths;
			fs_searchpaths = search;

			pakfilesi++;
		}
		else {
			// The next .pk3dir is before the next .pk3 file
			// But wait, this could be any directory, we're filtering to only ending with ".pk3dir" here.
			len = strlen(pakdirs[pakdirsi]);
			if (!FS_IsExt(pakdirs[pakdirsi], ".pk3dir", len)) {
				// This isn't a .pk3dir! Next!
				pakdirsi++;
				continue;
			}

			pakfile = FS_BuildOSPath(path, dir, pakdirs[pakdirsi]);

			// add the directory to the search path
			search = Z_Malloc(sizeof(searchpath_t));
			search->dir = Z_Malloc(sizeof(*search->dir));

			Q_strncpyz(search->dir->path, curpath, sizeof(search->dir->path));	// c:\quake3\baseq3
			Q_strncpyz(search->dir->fullpath, pakfile, sizeof(search->dir->fullpath));	// c:\quake3\baseq3\mypak.pk3dir
			Q_strncpyz(search->dir->gamedir, pakdirs[pakdirsi], sizeof(search->dir->gamedir)); // mypak.pk3dir

			search->next = fs_searchpaths;
			fs_searchpaths = search;

			pakdirsi++;
		}
	}

	// done
	Sys_FreeFileList( pakfiles );
	Sys_FreeFileList( pakdirs );

	if ( paksOnly ) {
		return;
	}

	//
	// add the directory to the search path
	//
	search = Z_Malloc (sizeof(searchpath_t));
	search->dir = Z_Malloc( sizeof( *search->dir ) );

	Q_strncpyz(search->dir->path, path, sizeof(search->dir->path));
	Q_strncpyz(search->dir->fullpath, curpath, sizeof(search->dir->fullpath));
	Q_strncpyz(search->dir->gamedir, dir, sizeof(search->dir->gamedir));

	search->next = fs_searchpaths;
	fs_searchpaths = search;
}

/*
================
FS_HasPakChecksum

Whether the search path has a pak with the checksum
================
*/
static qboolean FS_HasPakChecksum( unsigned int checksum ) {
	searchpath_t	*sp;

	for ( sp = fs_searchpaths ; sp ; sp = sp->next ) {
		if ( sp->pack && sp->pack->checksum == checksum ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
================
FS_SetServerPakHashes
================
*/
void FS_SetServerPakHashes( const byte *manifest, const ofcmHeader_t *header, const byte sha256[SHA256_DIGEST_SIZE] )
{
	int		i;

	if ( manifest ? fs_serverPakHashesSet && !memcmp( sha256, fs_serverPakHashesFrom, SHA256_DIGEST_SIZE ) :
		!fs_serverPakHashesSet ) {
		return;
	}
	fs_serverPakHashesSet = manifest != NULL;
	fs_numServerPakHashes = 0;
	if ( fs_serverPakHashes ) {
		Z_Free( fs_serverPakHashes );
		fs_serverPakHashes = NULL;
	}
	// every entry, an optional one's name too, so a pak is told from
	// another with its checksum (FS_ServerPakHash)
	if ( manifest && header->numEntries > 0 ) {
		fs_numServerPakHashes = header->numEntries;
		fs_serverPakHashes = Z_Malloc( fs_numServerPakHashes * sizeof( *fs_serverPakHashes ) );
		for ( i = 0; i < fs_numServerPakHashes; i++ ) {
			OFCM_ReadEntry( manifest, i, &fs_serverPakHashes[i] );
		}
	}
	if ( manifest ) {
		Com_Memcpy( fs_serverPakHashesFrom, sha256, SHA256_DIGEST_SIZE );
	}
	fs_serverPakHashesChanged = qtrue;
}

// whether the server's pak hashes apply: on a client connected to another's
// server, as the content cache's paks are (FS_AddCachePaks)
static qboolean FS_ServerPakHashesApply( void )
{
	return fs_numServerPakHashes && fs_numServerReferencedPaks && !( com_sv_running && com_sv_running->integer );
}

/*
================
FS_ServerPakEntry

The server's content manifest's entry with the pak's game directory, name
and checksum, if it has one and applies
================
*/
static const ofcmEntry_t *FS_ServerPakEntry( const char *gameDir, const char *name, unsigned int checksum )
{
	const ofcmEntry_t	*e;
	int					i;

	if ( !FS_ServerPakHashesApply() ) {
		return NULL;
	}
	for ( i = 0; i < fs_numServerPakHashes; i++ ) {
		e = &fs_serverPakHashes[i];
		if ( (unsigned int)e->checksum == checksum && !Q_stricmp( e->gameDir, gameDir ) &&
			!Q_stricmp( e->name, name ) ) {
			return e;
		}
	}
	return NULL;
}

/*
================
FS_ServerPakHash

The hash of the entry with the pak's game directory, name and checksum.
None for a pak no entry has under its name, as an installed copy under
another name, since another entry with its checksum may be a twin's: such
a pak stands in for the server's only by that one's hash (FS_HasServerPak),
and isn't refused (P243). A "bring your own" entry has none either
================
*/
const byte *FS_ServerPakHash( const char *gameDir, const char *name, unsigned int checksum )
{
	const ofcmEntry_t	*e = FS_ServerPakEntry( gameDir, name, checksum );

	return e && !( e->flags & OFCM_FLAG_OWN ) ? e->sha256 : NULL;
}

/*
================
FS_ServerHashesReferencedPak

Whether the server connected to references a pak with an installed pak's
checksum, and its content manifest gives a hash the pak is checked
against: the referenced pak's, which a pak under any name with the
checksum counts by (FS_HasServerPak), or the pak's own entry's, which it's
refused by (FS_PakHashRefused)
================
*/
static qboolean FS_ServerHashesReferencedPak( const pack_t *pak )
{
	const char	*refName, *slash;
	char		dir[MAX_QPATH];
	int			i;

	for ( i = 0; i < fs_numServerReferencedPaks; i++ ) {
		if ( fs_serverReferencedPaks[i] != pak->checksum ) {
			continue;
		}
		if ( FS_ServerPakHash( pak->pakGamename, pak->pakBasename, pak->checksum ) ) {
			return qtrue;
		}
		refName = fs_serverReferencedPakNames[i];
		slash = refName ? strchr( refName, '/' ) : NULL;
		if ( !slash || slash - refName >= (int)sizeof( dir ) ) {
			continue;
		}
		Q_strncpyz( dir, refName, slash - refName + 1 );
		if ( FS_ServerPakHash( dir, slash + 1, pak->checksum ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
================
FS_ServerPaksListed

Whether a content manifest lists every pak the server references among its
required entries, by checksum and name, as the server's current one does;
one kept from before a map change may not (open question 14), or may list
another pak of the same files under its name
================
*/
qboolean FS_ServerPaksListed( const byte *manifest, const ofcmHeader_t *header )
{
	ofcmEntry_t	entry;
	int			i, j;

	for ( i = 0; i < fs_numServerReferencedPaks; i++ ) {
		for ( j = 0; j < header->numRequired; j++ ) {
			OFCM_ReadEntry( manifest, j, &entry );
			if ( (unsigned int)entry.checksum == fs_serverReferencedPaks[i] &&
				!Q_stricmp( va( "%s/%s", entry.gameDir, entry.name ), fs_serverReferencedPakNames[i] ) ) {
				break;
			}
		}
		if ( j == header->numRequired ) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
================
FS_PakIndexKey

An installed pak's key in the pak index, as a record without its hash:
qfalse when it has none, or its path is too long for one
================
*/
static qboolean FS_PakIndexKey( const pack_t *pak, pakRecord_t *key )
{
	char	relative[MAX_OSPATH];

	Com_Memset( key, 0, sizeof( *key ) );
	if ( !FS_ContentRelativePath( pak, relative, sizeof( relative ) ) || strlen( relative ) >= sizeof( key->path ) ||
		!Content_FileStat( pak->pakFilename, &key->size, &key->mtime ) ) {
		return qfalse;
	}
	key->root = pak->contentRoot;
	key->pakChecksum = pak->checksum;
	Q_strncpyz( key->path, relative, sizeof( key->path ) );
	return qtrue;
}

/*
================
FS_PakSHA256

A pak's SHA-256, as far as it's known: a cached pak's from its name, an
installed one's from the pak index. qfalse when it isn't, as for one the
index hasn't hashed (FS_UnhashedServerPaks) or a retail pak
================
*/
static qboolean FS_PakSHA256( const pack_t *pak, byte sha256[SHA256_DIGEST_SIZE] )
{
	const pakRecord_t	*record;
	pakRecord_t			key;
	char				name[SHA256_HEX_SIZE];

	if ( pak->cached ) {
		// a cached pak's name is its hash
		Q_strncpyz( name, COM_SkipPath( (char *)pak->pakFilename ), sizeof( name ) );
		return Com_SHA256FromHex( name, sha256 );
	}
	if ( !FS_PakIndexKey( pak, &key ) ||
		!( record = Content_PakIndex_Find( key.root, key.path, key.size, key.mtime ) ) ||
		( record->flags & PAK_RECORD_RETAIL ) ) {
		return qfalse;
	}
	Com_Memcpy( sha256, record->sha256, SHA256_DIGEST_SIZE );
	return qtrue;
}

/*
================
FS_PakHashRefused

Whether a pak has the game directory, name and checksum of one the server
gives a hash for, but not that hash. It isn't mounted for the server, and
the right one is fetched by its hash. One whose hash isn't known passes,
as an installed one the index hasn't hashed does until it has
(FS_UnhashedServerPaks). By its own entry alone (FS_ServerPakHash)
================
*/
static qboolean FS_PakHashRefused( const pack_t *pak )
{
	const byte	*hash = FS_ServerPakHash( pak->pakGamename, pak->pakBasename, pak->checksum );
	byte		have[SHA256_DIGEST_SIZE];

	if ( !hash || !FS_PakSHA256( pak, have ) || !memcmp( have, hash, SHA256_DIGEST_SIZE ) ) {
		return qfalse;
	}
	Com_Printf( S_COLOR_YELLOW "WARNING: %s has the checksum of the server's %s/%s.pk3, but not the hash its content "
		"manifest gives, so it isn't used on this server\n", pak->pakFilename, pak->pakGamename, pak->pakBasename );
	return qtrue;
}

/*
================
FS_HasServerPak

Whether the search path has the server's pak refName ("<game>/<name>")
with the checksum. Under the server's content manifest, a pak with the
checksum counts under that name, or with the hash the manifest gives that
pak, since another pak of the same files under another name shares the
checksum (P229); a pak whose hash isn't known counts too. One the manifest
doesn't list counts only under its name: another entry's hash may be a
twin's. A "bring your own" pak, and any on a classic server, by the
checksum alone
================
*/
static qboolean FS_HasServerPak( const char *refName, unsigned int checksum )
{
	const searchpath_t	*sp;
	const ofcmEntry_t	*e;
	const char			*slash = strchr( refName, '/' );
	char				dir[MAX_QPATH];
	byte				have[SHA256_DIGEST_SIZE];

	if ( !slash || slash - refName >= (int)sizeof( dir ) || !FS_ServerPakHashesApply() ) {
		return FS_HasPakChecksum( checksum );
	}
	Q_strncpyz( dir, refName, slash - refName + 1 );
	e = FS_ServerPakEntry( dir, slash + 1, checksum );
	if ( e && ( e->flags & OFCM_FLAG_OWN ) ) {
		return FS_HasPakChecksum( checksum );
	}
	for ( sp = fs_searchpaths ; sp ; sp = sp->next ) {
		if ( !sp->pack || sp->pack->checksum != checksum ) {
			continue;
		}
		if ( !Q_stricmp( sp->pack->pakGamename, dir ) && !Q_stricmp( sp->pack->pakBasename, slash + 1 ) ) {
			return qtrue;
		}
		if ( e && ( !FS_PakSHA256( sp->pack, have ) || !memcmp( have, e->sha256, SHA256_DIGEST_SIZE ) ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
================
FS_NoteServerPaks

Notes which of the server's paks the search path has, as it's built or the
paks change, for FS_ServerPaksMatch
================
*/
static void FS_NoteServerPaks( void )
{
	int		i;

	for ( i = 0; i < fs_numServerReferencedPaks; i++ ) {
		fs_serverReferencedPakHad[i] = FS_HasServerPak( fs_serverReferencedPakNames[i], fs_serverReferencedPaks[i] );
	}
}

/*
================
FS_RefuseHashedPaks

Takes out of the search path, down to below, the installed paks whose hash
the server's content manifest refuses (FS_PakHashRefused)
================
*/
static void FS_RefuseHashedPaks( const searchpath_t *below )
{
	searchpath_t	**link, *search;

	if ( !FS_ServerPakHashesApply() ) {
		return;
	}
	for ( link = &fs_searchpaths; *link != below; ) {
		search = *link;
		if ( search->pack && FS_PakHashRefused( search->pack ) ) {
			*link = search->next;
			fs_packFiles -= search->pack->numfiles;
			FS_FreePak( search->pack );
			Z_Free( search );
			fs_numRefusedPaks++;
		} else {
			link = &search->next;
		}
	}
}

/*
================
FS_ServerPaksMatch

Whether every pak in the search path matches the hash the server's
content manifest gives (FS_PakHashRefused), and every referenced pak the
search path had when built is still present (FS_HasServerPak), which a
hash learned since may change, so a restart would mount the same: then
the hashes needn't restart it (FS_ConditionalRestart)
================
*/
qboolean FS_ServerPaksMatch( void )
{
	const searchpath_t	*search;
	int					i;

	if ( FS_ServerPakHashesApply() ) {
		for ( search = fs_searchpaths; search; search = search->next ) {
			if ( search->pack && FS_PakHashRefused( search->pack ) ) {
				return qfalse;
			}
		}
		for ( i = 0; i < fs_numServerReferencedPaks; i++ ) {
			if ( fs_serverReferencedPakHad[i] &&
				!FS_HasServerPak( fs_serverReferencedPakNames[i], fs_serverReferencedPaks[i] ) ) {
				return qfalse;
			}
		}
	}
	fs_serverPakHashesChanged = qfalse;
	return qtrue;
}

/*
================
FS_UnhashedServerPaks
================
*/
int FS_UnhashedServerPaks( pakRecord_t *paks, int max )
{
	const searchpath_t	*search;
	pakRecord_t			key;
	int					count = 0;

	for ( search = fs_searchpaths; search && FS_ServerPakHashesApply(); search = search->next ) {
		if ( !search->pack || search->pack->cached || !FS_ServerHashesReferencedPak( search->pack ) ||
			!FS_PakIndexKey( search->pack, &key ) || Content_PakIndex_Find( key.root, key.path, key.size, key.mtime ) ) {
			continue;
		}
		if ( paks && count < max ) {
			paks[count] = key;
		}
		count++;
	}
	return count;
}

/*
================
FS_AddCachePaks

The content cache's paks of a game directory that the server connected to
references, found by checksum whatever game directory and name they were
cached under and mounted as the server lists them, and that no pak in the
search path has already, put after that directory's own entries, below,
so each ranks under its installed paks and over any lower game directory. A
download then never replaces an installed pak of its own directory, and a
mod's still overrides the base game's. Only on a client connected to
another's server (a demo sets no referenced paks)
================
*/
static void FS_AddCachePaks( const char *dir, const searchpath_t *below )
{
	searchpath_t		**link, *search;
	pack_t				*pak;
	const char			*refName, *slash;
	const byte			*hash;
	char				ospath[MAX_OSPATH];
	int					i, entry;

	// (com_dedicated and com_sv_running are registered after the first startup)
	if ( !fs_numServerReferencedPaks || ( com_dedicated && com_dedicated->integer ) ||
		( com_sv_running && com_sv_running->integer ) ) {
		return;
	}

	// each goes just over below, after those before it
	for ( link = &fs_searchpaths; *link != below; link = &(*link)->next ) {
	}

	for ( i = 0; i < fs_numServerReferencedPaks; i++ ) {
		refName = fs_serverReferencedPakNames[i];
		slash = refName ? strchr( refName, '/' ) : NULL;
		if ( !slash || slash - refName != (int)strlen( dir ) || Q_stricmpn( refName, dir, slash - refName ) ) {
			continue;
		}
		if ( FS_HasServerPak( refName, fs_serverReferencedPaks[i] ) ) {
			continue;
		}
		// in this directory under the server's name for it, wherever and
		// however it was cached; if it's damaged, the next with that checksum.
		// One the server gives a hash for, by that hash alone (FS_ServerPakHash)
		hash = FS_ServerPakHash( dir, slash + 1, fs_serverReferencedPaks[i] );
		while ( ( entry = hash ? Content_Cache_FindSHA256( hash, ospath, sizeof( ospath ) ) :
			Content_Cache_Find( fs_serverReferencedPaks[i], dir, ospath, sizeof( ospath ) ) ) >= 0 ) {
			pak = FS_LoadZipFile( ospath, slash + 1 );
			if ( pak && pak->checksum == fs_serverReferencedPaks[i] ) {
				break;
			}
			Com_Printf( S_COLOR_YELLOW "WARNING: %s, cached for %s, is damaged, so it's removed\n",
				ospath, refName );
			if ( pak ) {
				FS_FreePak( pak );
			}
			Content_Cache_Remove( entry );
		}
		if ( entry < 0 ) {
			continue;
		}
		// in the game directory's place, where a download used to go, so a
		// QVM is looked for in one pak a directory (FS_FindVM)
		Q_strncpyz( pak->pakPathname, FS_BaseDir_BuildOSPath( fs_homedatapath->string, dir ),
			sizeof( pak->pakPathname ) );
		Q_strncpyz( pak->pakGamename, dir, sizeof( pak->pakGamename ) );
		pak->cached = qtrue;
		fs_packFiles += pak->numfiles;

		search = Z_Malloc( sizeof( searchpath_t ) );
		search->pack = pak;
		search->next = *link;
		*link = search;
		link = &search->next;
		fs_numCachePaks++;
		Content_Cache_Mounted( entry );
	}
}

/*
================
FS_UnmountCachePaks

Once the client has left the server that referenced them, restarts the
filesystem without the content cache's paks, and with the installed ones
the server's hashes left out, so the menus and a local game run on
installed content only; the next server's gamestate mounts what it
references. Until paks can be taken out of the search path in place
================
*/
void FS_UnmountCachePaks( void )
{
	if ( ( fs_numCachePaks || fs_numRefusedPaks ) && !fs_numServerReferencedPaks ) {
		FS_Restart( fs_checksumFeed );
	}
}

/*
================
FS_AddGameDirectories
================
*/
static void FS_AddGameDirectories(const char *dir)
{
	const searchpath_t	*below = fs_searchpaths;

	// add search path elements in reverse priority order
	for(int i = ARRAY_LEN(fs_pathVars) - 1; i >= 0; i--) {
		const cvar_t *pathVar = fs_pathVars[i];

		if (!pathVar || !pathVar->string[0]) {
			continue;
		}

		searchpath_t *added = fs_searchpaths;
		FS_AddGameDirectory(pathVar->string, dir, FS_PathVarPaksOnly( pathVar ));
		FS_SetContentRoot( fs_searchpaths, added, pathVar );
	}

	FS_RefuseHashedPaks( below );
	FS_AddCachePaks( dir, below );
}

/*
================
FS_CheckDirTraversal

Check whether the string contains stuff like "../" to prevent directory traversal bugs
and return qtrue if it does.
================
*/

qboolean FS_CheckDirTraversal(const char *checkdir)
{
	if(strstr(checkdir, "../") || strstr(checkdir, "..\\"))
		return qtrue;
	
	return qfalse;
}

static size_t FS_NameLength( const char *name, size_t length );

/*
================
FS_InvalidGameDir

return true if path is a reference to current directory or directory traversal
or a sub-directory, a hidden directory, or one of the engine's own folders
(FS_EngineFolder), named as Windows would find it, without final dots and
spaces
================
*/
qboolean FS_InvalidGameDir( const char *gamedir ) {
	char	name[MAX_QPATH];

	Q_strncpyz( name, gamedir, sizeof( name ) );
	name[FS_NameLength( name, strlen( name ) )] = '\0';
	if ( gamedir[0] == '.' || strchr( gamedir, '/' ) || strchr( gamedir, '\\' ) || FS_EngineFolder( name ) ) {
		return qtrue;
	}

	return qfalse;
}

/*
================
FS_ComparePaks

----------------
dlstring == qtrue

Returns a list of pak files that we should download from the server. They all get stored
in the content cache and an FS_Restart will be fired up after we download them all.

The string is the format:

@remotename@localname@checksum [repeat]

with the checksum the server listed for the pak, in hexadecimal.

static int		fs_numServerReferencedPaks;
static int		fs_serverReferencedPaks[MAX_SEARCH_PATHS];
static char		*fs_serverReferencedPakNames[MAX_SEARCH_PATHS];

----------------
dlstring == qfalse

we are not interested in a download string format, we want something human-readable
(this is used for diagnostics while connecting to a pure server)

================
*/
qboolean FS_ComparePaks( char *neededpaks, int len, qboolean dlstring ) {
	qboolean havepak;
	char *origpos = neededpaks;
	int i;

	if (!fs_numServerReferencedPaks)
		return qfalse; // Server didn't send any pack information along

	*neededpaks = 0;

	for ( i = 0 ; i < fs_numServerReferencedPaks ; i++ )
	{
		// Make sure the server cannot make us write anywhere but a game
		// directory
		if(!FS_ValidDownloadName(fs_serverReferencedPakNames[i]))
		{
			Com_Printf(S_COLOR_YELLOW "WARNING: Invalid download name %s: a pak's name must be "
				"<game directory>/<name>\n", fs_serverReferencedPakNames[i]);
			continue;
		}

		// Ok, see if we have this pak file
		havepak = FS_HasServerPak( fs_serverReferencedPakNames[i], fs_serverReferencedPaks[i] );

		if ( !havepak && fs_serverReferencedPakNames[i] && *fs_serverReferencedPakNames[i] ) { 
			// Don't got it

			if (dlstring)
			{
				// We need this to make sure we won't hit the end of the buffer or the server could
				// overwrite non-pk3 files on clients by writing so much crap into neededpaks that
				// Q_strcat cuts off the .pk3 extension.

				origpos += strlen(origpos);

				// Remote name
				Q_strcat( neededpaks, len, "@");
				Q_strcat( neededpaks, len, fs_serverReferencedPakNames[i] );
				Q_strcat( neededpaks, len, ".pk3" );

				// Local name: only its .tmp is written there, and the pak then
				// goes into the content cache by its hash (CL_EndDownload), so
				// one of the same name already there doesn't matter
				Q_strcat( neededpaks, len, "@");
				Q_strcat( neededpaks, len, fs_serverReferencedPakNames[i] );
				Q_strcat( neededpaks, len, ".pk3" );

				// The checksum the download must have
				Q_strcat( neededpaks, len, va( "@%08x", fs_serverReferencedPaks[i] ) );

				// Find out whether it might have overflowed the buffer and don't add this file to the
				// list if that is the case.
				if(strlen(origpos) + (origpos - neededpaks) >= len - 1)
				{
					*origpos = '\0';
					break;
				}
			}
			else
			{
				Q_strcat( neededpaks, len, fs_serverReferencedPakNames[i] );
				Q_strcat( neededpaks, len, ".pk3" );
				// Do we have one with the same name?
				if ( FS_BaseDir_FileExists_HomeData( va( "%s.pk3", fs_serverReferencedPakNames[i] ) ) )
				{
					Q_strcat( neededpaks, len, " (local file exists with wrong checksum)");
				}
				Q_strcat( neededpaks, len, "\n");
			}
		}
	}

	if ( *neededpaks ) {
		return qtrue;
	}

	return qfalse; // We have them all
}

/*
================
FS_MissingPaks

The paks the server references that the client lacks, as FS_ComparePaks
finds them but with no string to fill: up to max of them into paks, their
names <game directory>/<name>.pk3. How many there are, which may be more
than max. A name too long for a missingPak_t's is left out: cut short, it
would lose its .pk3 and name another file
================
*/
int FS_MissingPaks( missingPak_t *paks, int max ) {
	int		i, count = 0;

	for ( i = 0; i < fs_numServerReferencedPaks; i++ ) {
		if ( !fs_serverReferencedPakNames[i] || !*fs_serverReferencedPakNames[i] ||
			strlen( fs_serverReferencedPakNames[i] ) + 4 >= sizeof( paks->name ) ||
			!FS_ValidDownloadName( fs_serverReferencedPakNames[i] ) ||
			FS_HasServerPak( fs_serverReferencedPakNames[i], fs_serverReferencedPaks[i] ) ) {
			continue;
		}
		if ( count < max ) {
			Com_sprintf( paks[count].name, sizeof( paks[count].name ), "%s.pk3", fs_serverReferencedPakNames[i] );
			paks[count].checksum = fs_serverReferencedPaks[i];
		}
		count++;
	}
	return count;
}

/*
================
FS_Shutdown

Frees all resources.
================
*/
void FS_Shutdown( qboolean closemfp ) {
	searchpath_t	*p, *next;
	int	i;

	for(i = 0; i < MAX_FILE_HANDLES; i++) {
		if (fsh[i].fileSize) {
			FS_FCloseFile(i);
		}
	}

	// free everything
	for(p = fs_searchpaths; p; p = next)
	{
		next = p->next;

		if(p->pack)
			FS_FreePak(p->pack);
		if (p->dir)
			Z_Free(p->dir);

		Z_Free(p);
	}

	// any FS_ calls will now be an error until reinitialized
	fs_searchpaths = NULL;
	fs_lastFileSearch = NULL;

	Cmd_RemoveCommand( "path" );
	Cmd_RemoveCommand( "dir" );
	Cmd_RemoveCommand( "fdir" );
	Cmd_RemoveCommand( "touchFile" );
	Cmd_RemoveCommand( "which" );

#ifdef FS_MISSING
	if (closemfp) {
		fclose(missingFiles);
	}
#endif
}

#ifndef STANDALONE
void Com_AppendCDKey( const char *filename );
void Com_ReadCDKey( const char *filename );
#endif

/*
================
FS_ReorderPurePaks
NOTE TTimo: the reordering that happens here is not reflected in the cvars (\cvarlist *pak*)
  this can lead to misleading situations, see https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=540
================
*/
static void FS_ReorderPurePaks( void )
{
	searchpath_t *s;
	int i;
	searchpath_t **p_insert_index, // for linked list reordering
		**p_previous; // when doing the scan

	fs_reordered = qfalse;

	// only relevant when connected to pure server
	if ( !fs_numServerPaks )
		return;

	p_insert_index = &fs_searchpaths; // we insert in order at the beginning of the list
	for ( i = 0 ; i < fs_numServerPaks ; i++ ) {
		p_previous = p_insert_index; // track the pointer-to-current-item
		for (s = *p_insert_index; s; s = s->next) {
			// the part of the list before p_insert_index has been sorted already
			if (s->pack && fs_serverPaks[i] == s->pack->checksum) {
				fs_reordered = qtrue;
				// move this element to the insert list
				*p_previous = s->next;
				s->next = *p_insert_index;
				*p_insert_index = s;
				// increment insert list
				p_insert_index = &s->next;
				break; // iterate to next server pack
			}
			p_previous = &s->next;
		}
	}
}

/*
================
FS_AddPathVar
================
*/
static void FS_AddPathVar( cvar_t *pathVar ) {
	if( !pathVar || !*pathVar->string ) {
		return;
	}

	// Check for duplicates
	for( int i = 0; i < ARRAY_LEN( fs_pathVars ); i++ ) {
		if ( fs_pathVars[i] && !Q_stricmp( fs_pathVars[i]->string, pathVar->string ) ) {
			return;
		}
	}

	// Add to first empty slot
	for( int i = 0; i < ARRAY_LEN( fs_pathVars ); i++ ) {
		if ( !fs_pathVars[i] ) {
			fs_pathVars[i] = pathVar;
			return;
		}
	}

	Com_Error( ERR_FATAL, "FS_AddPathVar: exhausted fs_pathVars array" );
}

/*
================
FS_InitPathVars
================
*/
static void FS_InitPathVars( void ) {
	memset( fs_pathVars, 0, sizeof( fs_pathVars ) );

	FS_AddPathVar( fs_homeconfigpath );
	FS_AddPathVar( fs_homedatapath );
	FS_AddPathVar( fs_homestatepath );
	FS_AddPathVar( fs_basepath );
	FS_AddPathVar( fs_apppath );
	// after the install, so that when it's the same directory, the install's
	// files are all read
	FS_AddPathVar( fs_predecessorpath );
	FS_AddPathVar( fs_steampath );
	FS_AddPathVar( fs_gogpath );
	FS_AddPathVar( fs_microsoftstorepath );
}

/*
================
FS_MarkPredecessorConfig

Leaves the mark beside our config that stops the predecessor's being
copied, naming where it came from ("" when it wasn't)
================
*/
static void FS_MarkPredecessorConfig( const char *mark, const char *from ) {
	FILE	*out;

	if ( FS_CheckRefuses( __func__, mark ) ) {
		return;
	}
	if ( ( out = Sys_FOpen( mark, "wb" ) ) ) {
		fprintf( out, "%s\n", from );
		fclose( out );
	}
}

/*
================
FS_ImportPredecessorConfig

While our home has no config, copies the one in predHome, the config
home of the product this build succeeds, so its players keep their
settings. That home is only ever read. A copy, or finding ours already
there, leaves a mark beside ours, so a player who deletes our config to
reset it doesn't get the old one back. --check copies and marks nothing:
it notes the config it would copy, which it reads where it is
(FS_ReadUncopiedConfig).
================
*/
static void FS_ImportPredecessorConfig( const char *predHome, const char *gameName ) {
	char	from[MAX_OSPATH], to[MAX_OSPATH], old[MAX_OSPATH], tmp[MAX_OSPATH + 4], mark[MAX_OSPATH + 9];
	byte	buffer[4096];
	size_t	len;
	FILE	*in, *out;
	qboolean	failed;

	fs_uncopiedConfig[0] = '\0';
	if ( !predHome[0] ) {
		return;
	}

	// a dedicated server's goes in its own directory (FS_ServerFile), and
	// one in the game directory, where servers kept it and its mark before,
	// is ours too
	Q_strncpyz( to, FS_BuildOSPath( fs_homeconfigpath->string, gameName, FS_ServerFile( Q3CONFIG_CFG ) ), sizeof( to ) );
	Q_strncpyz( old, FS_BuildOSPath( fs_homeconfigpath->string, gameName, Q3CONFIG_CFG ), sizeof( old ) );
	Com_sprintf( mark, sizeof( mark ), "%s.imported", to );
	if ( FS_FileInPathExists( mark ) || FS_FileInPathExists( va( "%s.imported", old ) ) ) {
		return;
	}
	if ( FS_FileInPathExists( to ) || FS_FileInPathExists( old ) ) {
		// ours was made without a copy, or by a build before the mark
		if ( !com_check ) {
			FS_MarkPredecessorConfig( mark, "" );
		}
		return;
	}
	Q_strncpyz( from, FS_BuildOSPath( predHome, gameName, Q3CONFIG_CFG ), sizeof( from ) );
	if ( com_check ) {
		Q_strncpyz( fs_uncopiedConfig, from, sizeof( fs_uncopiedConfig ) );
		Com_sprintf( fs_uncopiedConfigTo, sizeof( fs_uncopiedConfigTo ), "%s/%s", gameName, FS_ServerFile( Q3CONFIG_CFG ) );
		return;
	}
	if ( !( in = Sys_FOpen( from, "rb" ) ) ) {
		return;
	}
	// copied beside it, then renamed: a partial copy, even one cut short by
	// a crash, would stop the next run from trying again
	Com_sprintf( tmp, sizeof( tmp ), "%s.tmp", to );
	if ( FS_CreatePath( tmp ) || !( out = Sys_FOpen( tmp, "wb" ) ) ) {
		fclose( in );
		return;
	}
	while ( ( len = fread( buffer, 1, sizeof( buffer ), in ) ) > 0 && fwrite( buffer, 1, len, out ) == len ) {
	}
	failed = ferror( in ) || len > 0;
	fclose( in );
	if ( fclose( out ) || failed || rename( tmp, to ) ) {
		FS_Remove( tmp );
		Com_Printf( S_COLOR_YELLOW "WARNING: couldn't copy your settings from %s\n", from );
		return;
	}
	Com_Printf( "Copied your settings from %s\n", from );
	FS_MarkPredecessorConfig( mark, from );
}

/*
================
FS_Startup
================
*/
static void FS_Startup( const char *gameName )
{
	cvar_t *fs_homepath = Cvar_Get("fs_homepath", "", CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE);
	const char *configPath = Sys_DefaultHomeConfigPath();
	const char *dataPath = Sys_DefaultHomeDataPath();
	const char *statePath = Sys_DefaultHomeStatePath();
	const char *predConfigPath = "", *predDataPath = "";

	if(*(fs_homepath)->string) {
		// Setting fs_homepath manually overrides everything else
		configPath = dataPath = statePath = fs_homepath->string;
	} else if(!*configPath || !*dataPath || !*statePath) {
		// #shouldneverhappen; just a sensible fallback
		configPath = dataPath = statePath = Sys_DefaultInstallPath();
	}

	Com_Printf( "----- FS_Startup -----\n" );

	fs_packFiles = 0;

	fs_debug = Cvar_Get( "fs_debug", "0", 0 );
	fs_basepath = Cvar_Get ("fs_basepath", Sys_DefaultInstallPath(), CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
	fs_basegame = Cvar_Get ("fs_basegame", "", CVAR_INIT );
	fs_homeconfigpath = Cvar_Get ("fs_homeconfigpath", configPath, CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
	fs_homedatapath = Cvar_Get ("fs_homedatapath", dataPath, CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
	fs_homestatepath = Cvar_Get ("fs_homestatepath", statePath, CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
	fs_gamedirvar = Cvar_Get ("fs_game", "", CVAR_INIT|CVAR_SYSTEMINFO );
	fs_steampath = Cvar_Get ("fs_steampath", Sys_SteamPath(), CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
	fs_gogpath = Cvar_Get ("fs_gogpath", Sys_GogPath(), CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
	fs_microsoftstorepath = Cvar_Get ("fs_microsoftstorepath", Sys_MicrosoftStorePath(), CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
	// the home of the product this build succeeds, whose paks are read and
	// whose config is copied; not when the home is set by hand, as the tests do
	if (!fs_homepath->string[0]) {
		Sys_PredecessorHomePaths( &predConfigPath, &predDataPath );
	}
	fs_predecessorpath = Cvar_Get ("fs_predecessorpath", predDataPath, CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
	Q_strncpyz( fs_predecessorConfigPath, predConfigPath, sizeof( fs_predecessorConfigPath ) );

#ifdef __APPLE__
	fs_apppath = Cvar_Get ("fs_apppath", Sys_DefaultAppPath(), CVAR_INIT|CVAR_PROTECTED|CVAR_PRIVATE );
#endif

	FS_InitPathVars( );

	if (!gameName[0]) {
		Cvar_ForceReset( "com_basegame" );
	}

	if (!FS_FilenameCompare(fs_gamedirvar->string, gameName)) {
		// This is the standard base game. Servers and clients should
		// use "" and not the standard basegame name because this messes
		// up pak file negotiation and lots of other stuff
		Cvar_ForceReset( "fs_game" );
	}

	if (FS_InvalidGameDir(gameName)) {
		Com_Error( ERR_DROP, "Invalid com_basegame '%s'", gameName );
	}
	if (FS_InvalidGameDir(fs_basegame->string)) {
		Com_Error( ERR_DROP, "Invalid fs_basegame '%s'", fs_basegame->string );
	}
	if (FS_InvalidGameDir(fs_gamedirvar->string)) {
		Com_Error( ERR_DROP, "Invalid fs_game '%s'", fs_gamedirvar->string );
	}

	// --check writes nothing: no home made
	if ( !com_check ) {
		FS_CreatePath(fs_homeconfigpath->string);
		FS_CreatePath(fs_homedatapath->string);
		FS_CreatePath(fs_homestatepath->string);
	}

	FS_ImportPredecessorConfig(predConfigPath, gameName);

	// the content cache's paks are mounted again as the directories are added
	Content_Cache_Unmounted();
	fs_numCachePaks = fs_numRefusedPaks = 0;

	FS_AddGameDirectories(gameName);

	// check for additional base game so mods can be based upon other mods
	if ( fs_basegame->string[0] && Q_stricmp( fs_basegame->string, gameName ) ) {
		FS_AddGameDirectories(fs_basegame->string);
	}

	// check for additional game folder for mods
	if ( fs_gamedirvar->string[0] && Q_stricmp( fs_gamedirvar->string, gameName ) ) {
		FS_AddGameDirectories(fs_gamedirvar->string);
	}

#ifndef STANDALONE
	if (!com_standalone->integer) {
		Com_ReadCDKey(BASEGAME);
		if (fs_gamedirvar->string[0]) {
			Com_AppendCDKey(fs_gamedirvar->string);
		}
	}
#endif

	// add our commands
	Cmd_AddCommand ("path", FS_Path_f);
	Cmd_AddCommand ("dir", FS_Dir_f );
	Cmd_AddCommand ("fdir", FS_NewDir_f );
	Cmd_AddCommand ("touchFile", FS_TouchFile_f );
	Cmd_AddCommand ("which", FS_Which_f );

	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=506
	// reorder the pure pk3 files according to server order
	FS_ReorderPurePaks();
	FS_NoteServerPaks();

	// when the cached paks mounted were last used
	Content_Cache_Flush();

	// print the current search paths
	FS_Path_f();

	fs_gamedirvar->modified = qfalse; // We just loaded, it's not modified

	Com_Printf( "----------------------\n" );

#ifdef FS_MISSING
	if (missingFiles == NULL) {
		missingFiles = Sys_FOpen( "\\missing.txt", "ab" );
	}
#endif
	Com_Printf( "%d files in pk3 files\n", fs_packFiles );
}

#ifndef STANDALONE
/*
===================
FS_AppendUserFriendlyPakList
===================
*/
static void FS_AppendUserFriendlyPakList( char *buf, size_t bufsize, int foundPakFiles, int numPakFiles )
{
	int missingCount = 0;
	int missingIndex = 0;
	int missingMin = numPakFiles;
	int missingMax = 0;
	qboolean missingConsecutive = qtrue;
	int i;

	for ( i = 0; i < numPakFiles; i++ ) {
		if ( foundPakFiles & ( 1 << i ) ) {
			continue;
		}

		missingCount++;

		if ( missingMin > i ) {
			missingMin = i;
		}
		if ( missingMax < i ) {
			missingMax = i;
		}

		if ( i > missingMin && ( foundPakFiles & ( 1<< ( i - 1 ) ) ) ) {
			missingConsecutive = qfalse;
		}
	}

	if ( missingConsecutive && missingMax >= missingMin + 2 ) {
		Q_strcat( buf, bufsize,
				va( " \"pak%d.pk3\" through \"pak%d.pk3\"",
					missingMin, missingMax ) );
	} else {
		for ( i = 0; i < numPakFiles; i++ ) {
			if ( foundPakFiles & (1<<i) ) {
				continue;
			}

			if ( missingIndex > 0 && missingCount > 2 ) {
				Q_strcat( buf, bufsize, "," );
			}

			if ( missingIndex == missingCount - 1 && missingCount > 1 ) {
				Q_strcat( buf, bufsize, " and" );
			}

			Q_strcat( buf, bufsize, va( " \"pak%d.pk3\"", i ) );

			missingIndex++;
		}
	}
}

/*
===================
FS_MissingDataError

Ends the program, as an error that isn't a crash, saying which of game's
paks weren't found, where they were looked for, where to put them and
how to get them; a client also opens the folder for them
===================
*/
static Q_NO_RETURN void FS_MissingDataError( const char *game, const char *gameName,
	int foundPaks, int numPaks, const char *installPath )
{
	char errorText[MAXPRINTMSG] = "";
	char gamePath[MAX_OSPATH];

	Com_sprintf(gamePath, sizeof(gamePath), "%s%c%s%c",
		installPath, PATH_SEP, game, PATH_SEP);

	Q_strcat(errorText, sizeof(errorText), va("%s's data is missing; not found:", gameName));
	FS_AppendUserFriendlyPakList(errorText, sizeof(errorText), foundPaks, numPaks);
	Q_strcat(errorText, sizeof(errorText), ".\n\nLooked in:\n");
	FS_AppendSearchDirs(errorText, sizeof(errorText), game, "  ", "\n");

#ifdef HOMEPATH_NAME_PREDECESSOR
	// the predecessor's home where it was found; with none found, where
	// it would be; none with the home set by hand, which leaves it out
	if (fs_predecessorpath->string[0]) {
		Q_strcat(errorText, sizeof(errorText),
				va("Paks in \"%s%c%s\" are read too.\n",
				fs_predecessorpath->string, PATH_SEP, game));
	} else if (!Cvar_VariableString("fs_homepath")[0]) {
		Q_strcat(errorText, sizeof(errorText),
				"Paks in the \"" HOMEPATH_NAME_PREDECESSOR "\" folder next to the home are read too.\n");
	}
#endif

	Q_strcat(errorText, sizeof(errorText),
			va("\nCopy them to \"%s\". pak0.pk3 comes with %s: its CD, or the "
			"\"%s\" folder of a Steam or GOG install. The others are its free "
			"point release, from https://ioquake3.org/extras/patch-data/\n",
			gamePath, gameName, game));

	if(!Q_stricmp(game, BASEGAME))
	{
		Q_strcat(errorText, sizeof(errorText),
				va("\nTo run on the free demo's data instead, put its pak0.pk3 in "
				"\"%s%cdemoq3%c\" and add +set com_basegame demoq3 to the command line.\n",
				installPath, PATH_SEP, PATH_SEP));
	}

	Sys_OpenFolderInFileManager(gamePath, qtrue);
	Com_ErrorExit(EXIT_MISSING_DATA, "%s", errorText);
}

/*
===================
FS_CheckPak0

Check whether any of the original id pak files is present,
and start up in standalone mode, if there are none and a
different com_basegame was set.
Note: If you're building a game that doesn't depend on the
Q3 media pak0.pk3, you'll want to remove this by defining
STANDALONE in q_shared.h
===================
*/
static void FS_CheckPak0( void )
{
	searchpath_t	*path;
	pack_t		*curpack;
	const char	*pakBasename;
	qboolean founddemo = qfalse;
	unsigned int foundPak = 0, foundTA = 0;
	const char *installPath;

	for( path = fs_searchpaths; path; path = path->next )
	{
		// a download named pak0 isn't the game's
		if(!path->pack || path->pack->cached)
			continue;

		curpack = path->pack;
		pakBasename = curpack->pakBasename;

		if(!Q_stricmpn( curpack->pakGamename, "demoq3", MAX_OSPATH )
				&& !Q_stricmpn( pakBasename, "pak0", MAX_OSPATH ))
		{
			if(curpack->checksum == DEMO_PAK0_CHECKSUM)
				founddemo = qtrue;
		}

		// the demo's pak0, put in baseq3, which FS_ChooseDemo plays
		else if(!Q_stricmpn( curpack->pakGamename, BASEGAME, MAX_OSPATH )
				&& !Q_stricmpn( pakBasename, "pak0", MAX_OSPATH )
				&& curpack->checksum == DEMO_PAK0_CHECKSUM
				&& !Q_stricmp( com_basegame->string, "demoq3" ))
		{
			founddemo = qtrue;
		}

		else if(!Q_stricmpn( curpack->pakGamename, BASEGAME, MAX_OSPATH )
				&& strlen(pakBasename) == 4 && !Q_stricmpn( pakBasename, "pak", 3 )
				&& pakBasename[3] >= '0' && pakBasename[3] <= '0' + NUM_ID_PAKS - 1)
		{
			if( curpack->checksum != pak_checksums[pakBasename[3]-'0'] )
			{
				if(pakBasename[3] == '0')
				{
					Com_Printf("\n\n"
							"**************************************************\n"
							"WARNING: " BASEGAME "/pak0.pk3 is present but its checksum (%u)\n"
							"is not correct. Please re-copy pak0.pk3 from your\n"
							"legitimate Q3 CDROM.\n"
							"**************************************************\n\n\n",
							curpack->checksum );
				}
				else
				{
					Com_Printf("\n\n"
							"**************************************************\n"
							"WARNING: " BASEGAME "/pak%d.pk3 is present but its checksum (%u)\n"
							"is not correct. Please re-install the point release\n"
							"**************************************************\n\n\n",
							pakBasename[3]-'0', curpack->checksum );
				}
			}

			foundPak |= 1<<(pakBasename[3]-'0');
		}
		else if(!Q_stricmpn(curpack->pakGamename, BASETA, MAX_OSPATH)
				&& strlen(pakBasename) == 4 && !Q_stricmpn(pakBasename, "pak", 3)
				&& pakBasename[3] >= '0' && pakBasename[3] <= '0' + NUM_TA_PAKS - 1)

		{
			if(curpack->checksum != missionpak_checksums[pakBasename[3]-'0'])
			{
				Com_Printf("\n\n"
						"**************************************************\n"
						"WARNING: " BASETA "/pak%d.pk3 is present but its checksum (%u)\n"
						"is not correct. Please re-install Team Arena\n"
						"**************************************************\n\n\n",
						pakBasename[3]-'0', curpack->checksum );				
			}

			foundTA |= 1 << (pakBasename[3]-'0');
		}
		else
		{
			int index;
			const char *game;

			// Finally check whether this pak's checksum is listed because the user tried
			// to trick us by renaming the file, and set foundPak's highest bit to indicate this case.
			if((game = FS_RetailPakGame(curpack->checksum, &index)) != NULL)
			{
				Com_Printf("\n\n"
						"**************************************************\n"
						"WARNING: %s is renamed pak file %s%cpak%d.pk3\n"
						"Running in standalone mode won't work\n"
						"Please rename, or remove this file\n"
						"**************************************************\n\n\n",
						curpack->pakFilename, game, PATH_SEP, index);

				if(!strcmp(game, BASEGAME))
					foundPak |= 0x80000000;
				else
					foundTA |= 0x80000000;
			}
		}
	}

	if(!foundPak && !foundTA && Q_stricmp(com_basegame->string, BASEGAME))
	{
		Cvar_Set("com_standalone", "1");
	}
	else
		Cvar_Set("com_standalone", "0");

	if(!com_standalone->integer)
	{
		if(!(foundPak & 0x01))
		{
			if(founddemo)
			{
				Com_Printf( "\n\n"
						"**************************************************\n"
						"WARNING: It looks like you're using pak0.pk3\n"
						"from the demo. This may work fine, but it is not\n"
						"guaranteed or supported.\n"
						"**************************************************\n\n\n" );

				foundPak |= 0x01;
			}
		}
	}

	installPath = fs_basepath->string;
#if defined(__linux__)
	{
		const char *p;

		// Users can't write to the default Flatpak fs_basepath
		if( ( p = getenv( "FLATPAK_ID" ) ) != NULL && *p != '\0' )
			installPath = fs_homedatapath->string;
	}
#elif defined(__APPLE__)
	// If we're running from an .app, it makes more sense to recommend
	// using fs_homepath as fs_basepath is likely not suitable
	if( strstr( fs_apppath->string, "Contents/MacOS" ) )
		installPath = fs_homedatapath->string;
#endif
#ifdef HOMEPATH_NAME_PREDECESSOR
	// a product that succeeds another keeps the data in the home, as its
	// predecessor's players do
	installPath = fs_homedatapath->string;
#endif

	if(!com_standalone->integer && (foundPak & ((1<<NUM_ID_PAKS)-1)) != ((1<<NUM_ID_PAKS)-1))
		FS_MissingDataError(BASEGAME, "Quake III Arena", foundPak, NUM_ID_PAKS, installPath);

	if(!com_standalone->integer && (foundTA & ((1<<NUM_TA_PAKS)-1)) != ((1<<NUM_TA_PAKS)-1)
			&& (!Q_stricmp(fs_gamedirvar->string, BASETA) || !Q_stricmp(fs_basegame->string, BASETA)))
		FS_MissingDataError(BASETA, "Team Arena", foundTA, NUM_TA_PAKS, installPath);
}
#endif

/*
=====================
FS_LoadedPakChecksums

Returns a space separated string containing the checksums of all loaded pk3 files.
Servers with sv_pure set will get this string and pass it to clients.
=====================
*/
const char *FS_LoadedPakChecksums( void ) {
	static char	info[BIG_INFO_STRING];
	searchpath_t	*search;

	info[0] = 0;

	for ( search = fs_searchpaths ; search ; search = search->next ) {
		// is the element a pak file? 
		if ( !search->pack ) {
			continue;
		}

		Q_strcat( info, sizeof( info ), va("%i ", search->pack->checksum ) );
	}

	return info;
}

/*
=====================
FS_LoadedPakNames

Returns a space separated string containing the names of all loaded pk3 files.
Servers with sv_pure set will get this string and pass it to clients.
=====================
*/
const char *FS_LoadedPakNames( void ) {
	static char	info[BIG_INFO_STRING];
	searchpath_t	*search;

	info[0] = 0;

	for ( search = fs_searchpaths ; search ; search = search->next ) {
		// is the element a pak file?
		if ( !search->pack ) {
			continue;
		}

		if (*info) {
			Q_strcat(info, sizeof( info ), " " );
		}
		Q_strcat( info, sizeof( info ), search->pack->pakBasename );
	}

	return info;
}

/*
=====================
FS_LoadedPakPureChecksums

Returns a space separated string containing the pure checksums of all loaded pk3 files.
Servers with sv_pure use these checksums to compare with the checksums the clients send
back to the server.
=====================
*/
const char *FS_LoadedPakPureChecksums( void ) {
	static char	info[BIG_INFO_STRING];
	searchpath_t	*search;

	info[0] = 0;

	for ( search = fs_searchpaths ; search ; search = search->next ) {
		// is the element a pak file? 
		if ( !search->pack ) {
			continue;
		}

		Q_strcat( info, sizeof( info ), va("%i ", search->pack->pure_checksum ) );
	}

	return info;
}

/*
=====================
FS_PakIsReferenced

Whether the server lists a pak among those clients must have: every pak
outside the base game, and the base game's referenced ones
=====================
*/
static qboolean FS_PakIsReferenced( const pack_t *pak ) {
	return pak->referenced || Q_stricmpn( pak->pakGamename, com_basegame->string, strlen( com_basegame->string ) );
}

/*
=====================
FS_ReferencedPakChecksums

Returns a space separated string containing the checksums of all referenced pk3 files.
The server will send this to the clients so they can check which files should be auto-downloaded. 
=====================
*/
const char *FS_ReferencedPakChecksums( void ) {
	static char	info[BIG_INFO_STRING];
	searchpath_t *search;

	info[0] = 0;


	for ( search = fs_searchpaths ; search ; search = search->next ) {
		// is the element a pak file?
		if ( search->pack ) {
			if ( FS_PakIsReferenced( search->pack ) ) {
				Q_strcat( info, sizeof( info ), va("%i ", search->pack->checksum ) );
			}
		}
	}

	return info;
}

/*
=====================
FS_ReferencedPakPureChecksums

Returns a space separated string containing the pure checksums of all referenced pk3 files.
Servers with sv_pure set will get this string back from clients for pure validation 

The string has a specific order, "cgame ui @ ref1 ref2 ref3 ..."
=====================
*/
const char *FS_ReferencedPakPureChecksums( void ) {
	static char	info[BIG_INFO_STRING];
	searchpath_t	*search;
	int nFlags, numPaks, checksum;

	info[0] = 0;

	checksum = fs_checksumFeed;
	numPaks = 0;
	for (nFlags = FS_CGAME_REF; nFlags; nFlags = nFlags >> 1) {
		if (nFlags & FS_GENERAL_REF) {
			// add a delimter between must haves and general refs
			//Q_strcat(info, sizeof(info), "@ ");
			info[strlen(info)+1] = '\0';
			info[strlen(info)+2] = '\0';
			info[strlen(info)] = '@';
			info[strlen(info)] = ' ';
		}
		for ( search = fs_searchpaths ; search ; search = search->next ) {
			// is the element a pak file and has it been referenced based on flag?
			if ( search->pack && (search->pack->referenced & nFlags)) {
				Q_strcat( info, sizeof( info ), va("%i ", search->pack->pure_checksum ) );
				if (nFlags & (FS_CGAME_REF | FS_UI_REF)) {
					break;
				}
				checksum ^= search->pack->pure_checksum;
				numPaks++;
			}
		}
	}
	// last checksum is the encoded number of referenced pk3s
	checksum ^= numPaks;
	Q_strcat( info, sizeof( info ), va("%i ", checksum ) );

	return info;
}

/*
=====================
FS_ReferencedPakNames

Returns a space separated string containing the names of all referenced pk3 files.
The server will send this to the clients so they can check which files should be auto-downloaded. 
=====================
*/
const char *FS_ReferencedPakNames( void ) {
	static char	info[BIG_INFO_STRING];
	searchpath_t	*search;

	info[0] = 0;

	// we want to return ALL pk3's from the fs_game path
	// and referenced one's from baseq3
	for ( search = fs_searchpaths ; search ; search = search->next ) {
		// is the element a pak file?
		if ( search->pack ) {
			if ( FS_PakIsReferenced( search->pack ) ) {
				if (*info) {
					Q_strcat(info, sizeof( info ), " " );
				}
				Q_strcat( info, sizeof( info ), search->pack->pakGamename );
				Q_strcat( info, sizeof( info ), "/" );
				Q_strcat( info, sizeof( info ), search->pack->pakBasename );
			}
		}
	}

	return info;
}

/*
=====================
FS_ClearPakReferences
=====================
*/
void FS_ClearPakReferences( int flags ) {
	searchpath_t *search;

	if ( !flags ) {
		flags = -1;
	}
	for ( search = fs_searchpaths; search; search = search->next ) {
		// is the element a pak file and has it been referenced?
		if ( search->pack ) {
			search->pack->referenced &= ~flags;
		}
	}
}


/*
=====================
FS_PureServerSetLoadedPaks

If the string is empty, all data sources will be allowed.
If not empty, only pk3 files that match one of the space
separated checksums will be checked for files, with the
exception of .cfg and .dat files.
=====================
*/
void FS_PureServerSetLoadedPaks( const char *pakSums, const char *pakNames ) {
	int		i, c, d;

	Cmd_TokenizeString( pakSums );

	c = Cmd_Argc();
	if ( c > MAX_SEARCH_PATHS ) {
		c = MAX_SEARCH_PATHS;
	}

	fs_numServerPaks = c;

	for ( i = 0 ; i < c ; i++ ) {
		fs_serverPaks[i] = atoi( Cmd_Argv( i ) );
	}

	if (fs_numServerPaks) {
		Com_DPrintf( "Connected to a pure server.\n" );
	}
	else
	{
		if (fs_reordered)
		{
			// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=540
			// force a restart to make sure the search order will be correct
			Com_DPrintf( "FS search reorder is required\n" );
			FS_Restart(fs_checksumFeed);
			return;
		}
	}

	for ( i = 0 ; i < c ; i++ ) {
		if (fs_serverPakNames[i]) {
			Z_Free(fs_serverPakNames[i]);
		}
		fs_serverPakNames[i] = NULL;
	}
	if ( pakNames && *pakNames ) {
		Cmd_TokenizeString( pakNames );

		d = Cmd_Argc();
		if ( d > MAX_SEARCH_PATHS ) {
			d = MAX_SEARCH_PATHS;
		}

		for ( i = 0 ; i < d ; i++ ) {
			fs_serverPakNames[i] = CopyString( Cmd_Argv( i ) );
		}
	}
}

/*
=====================
FS_PureServerSetReferencedPaks

The checksums and names of the pk3 files referenced at the server
are sent to the client and stored here. The client will use these
checksums to see if any pk3 files need to be auto-downloaded. 
=====================
*/
void FS_PureServerSetReferencedPaks( const char *pakSums, const char *pakNames ) {
	int		i, c, d = 0;

	Cmd_TokenizeString( pakSums );

	c = Cmd_Argc();
	if ( c > MAX_SEARCH_PATHS ) {
		c = MAX_SEARCH_PATHS;
	}

	for ( i = 0 ; i < c ; i++ ) {
		fs_serverReferencedPaks[i] = atoi( Cmd_Argv( i ) );
	}

	for (i = 0 ; i < ARRAY_LEN(fs_serverReferencedPakNames); i++)
	{
		if(fs_serverReferencedPakNames[i])
			Z_Free(fs_serverReferencedPakNames[i]);

		fs_serverReferencedPakNames[i] = NULL;
	}

	if ( pakNames && *pakNames ) {
		Cmd_TokenizeString( pakNames );

		d = Cmd_Argc();

		if(d > c)
			d = c;

		for ( i = 0 ; i < d ; i++ ) {
			fs_serverReferencedPakNames[i] = CopyString( Cmd_Argv( i ) );
		}
	}

	// ensure that there are as many checksums as there are pak names.
	if(d < c)
		c = d;

	fs_numServerReferencedPaks = c;
	// against the search path as it is, if no restart follows
	FS_NoteServerPaks();
}

/*
================
FS_FindPak0

The base game's pak0 with the checksum given, among the paks found, or NULL
================
*/
static const pack_t *FS_FindPak0( const char *gameName, unsigned int checksum ) {
	searchpath_t	*path;

	for( path = fs_searchpaths; path; path = path->next ) {
		if( path->pack && !Q_stricmp( path->pack->pakGamename, gameName ) &&
			!Q_stricmp( path->pack->pakBasename, "pak0" ) && path->pack->checksum == checksum ) {
			return path->pack;
		}
	}
	return NULL;
}

/*
================
FS_RetailPak

Whether the pak a download names ("<game>/<name>", as the referenced paks
are named) is one of the game's own, Quake III Arena 1.32's or Team
Arena's (the demo's pak0 isn't), known by its checksum whatever its name;
if so, its own name, for the player to add ("baseq3/pak3.pk3"). The
tests' OMNIFRAG_TEST_REFUSED_PAK_CHECKSUM adds a checksum
================
*/
qboolean FS_RetailPak( const char *download, char *retail, int size ) {
	searchpath_t	*path;
	const pack_t	*pak;

	for( path = fs_searchpaths; path; path = path->next ) {
		pak = path->pack;
		if( pak && !FS_FilenameCompare( va( "%s/%s", pak->pakGamename, pak->pakBasename ), download ) ) {
			return FS_RetailPakChecksum( va( "%s/%s.pk3", pak->pakGamename, pak->pakBasename ),
				pak->checksum, retail, size );
		}
	}
	return qfalse;
}

/*
================
FS_RetailPakChecksum

Whether a pak the server lists for download ("<game>/<name>.pk3"), by the
checksum it lists, is one of the game's own; if so, its own name, for the
player to add ("baseq3/pak3.pk3"). The tests' checksum is the download's
================
*/
qboolean FS_RetailPakChecksum( const char *download, unsigned int checksum, char *retail, int size ) {
	const char	*game;
	int			index;

	if( ( game = FS_RetailPakGame( checksum, &index ) ) != NULL ) {
		Com_sprintf( retail, size, "%s/pak%d.pk3", game, index );
		return qtrue;
	}
	if( fs_refusedTestPak && checksum == fs_refusedTestChecksum ) {
		Q_strncpyz( retail, download, size );
		return qtrue;
	}
	return qfalse;
}

/*
================
FS_ChooseDemo

With the full game's pak0 nowhere, and no base game asked for, plays the
demo's pak0, known by its checksum, where there is one: in demoq3, or in
baseq3, where a newcomer copies it, which is then read as the demo's. So
no one needs com_basegame demoq3. Returns whether it did
================
*/
static qboolean FS_ChooseDemo( void ) {
	const pack_t	*demo;
	qboolean		inBase;

	if( Cvar_Source( com_basegame ) != CVAR_SOURCE_DEFAULT || Q_stricmp( com_basegame->string, BASEGAME ) ||
		FS_FindPak0( BASEGAME, pak_checksums[0] ) ) {
		return qfalse;
	}
	demo = FS_FindPak0( BASEGAME, DEMO_PAK0_CHECKSUM );
	inBase = demo != NULL;
	// playing it from baseq3 takes fs_basegame, which an admin's may hold
	if( inBase && fs_basegame->string[0] && Q_stricmp( fs_basegame->string, BASEGAME ) ) {
		return qfalse;
	}

	FS_Shutdown( qfalse );
	Cvar_Set2( "com_basegame", "demoq3", qtrue );
	if( inBase ) {
		Cvar_Set2( "fs_basegame", BASEGAME, qtrue );
	}
	FS_Startup( com_basegame->string );
	demo = FS_FindPak0( inBase ? BASEGAME : "demoq3", DEMO_PAK0_CHECKSUM );
	if( !demo ) {
		// no demo either: the full game's missing data, as before
		FS_Shutdown( qfalse );
		Cvar_Set2( "com_basegame", BASEGAME, qtrue );
		FS_Startup( com_basegame->string );
		return qfalse;
	}

	Com_Printf( "Playing the demo's data (%s): the full game's paks weren't found. "
		"Put pak0.pk3 to pak8.pk3 in %s%c" BASEGAME " to play the full game.\n",
		demo->pakFilename, fs_homedatapath->string, PATH_SEP );
	return qtrue;
}

/*
================
FS_InitFilesystem

Called only at initial startup, not when the filesystem
is resetting due to a game change
================
*/
void FS_InitFilesystem( void ) {
	const char	*refusedTest = getenv( "OMNIFRAG_TEST_REFUSED_PAK_CHECKSUM" );

	// read once, so setenv can't change it later; signed, as
	// sv_referencedPaks lists it
	if ( refusedTest && *refusedTest ) {
		fs_refusedTestPak = qtrue;
		fs_refusedTestChecksum = (unsigned int)strtoll( refusedTest, NULL, 10 );
	}

	// allow command line parms to override our defaults
	// we have to specially handle this, because normal command
	// line variable sets don't happen until after the filesystem
	// has already been initialized
	Com_StartupVariable("fs_basepath");
	Com_StartupVariable("fs_homepath");
	Com_StartupVariable("fs_homeconfigpath");
	Com_StartupVariable("fs_homedatapath");
	Com_StartupVariable("fs_homestatepath");
	Com_StartupVariable("fs_game");

	if(!FS_FilenameCompare(Cvar_VariableString("fs_game"), com_basegame->string))
		Cvar_Set("fs_game", "");

	// try to start up normally
	FS_Startup(com_basegame->string);

#ifndef STANDALONE
	FS_ChooseDemo( );
	FS_CheckPak0( );
#endif

	// if we can't find default.cfg, assume that the paths are
	// busted and error out now, rather than getting an unreadable
	// graphics screen when the font fails to load
	if ( FS_ReadFile( "default.cfg", NULL ) <= 0 ) {
		Com_Error( ERR_FATAL, "Couldn't load default.cfg" );
	}

	Q_strncpyz(lastValidBase, fs_basepath->string, sizeof(lastValidBase));
	Q_strncpyz(lastValidComBaseGame, com_basegame->string, sizeof(lastValidComBaseGame));
	Q_strncpyz(lastValidFsBaseGame, fs_basegame->string, sizeof(lastValidFsBaseGame));
	Q_strncpyz(lastValidGame, fs_gamedirvar->string, sizeof(lastValidGame));
}


/*
================
FS_Restart
================
*/
void FS_Restart( int checksumFeed ) {
	const char *lastGameDir;

	// free anything we currently have loaded
	FS_Shutdown(qfalse);

	// set the checksum feed
	fs_checksumFeed = checksumFeed;
	fs_serverPakHashesChanged = qfalse;

	// clear pak references
	FS_ClearPakReferences(0);

	// try to start up normally
	FS_Startup(com_basegame->string);

#ifndef STANDALONE
	FS_CheckPak0( );
#endif

	// if we can't find default.cfg, assume that the paths are
	// busted and error out now, rather than getting an unreadable
	// graphics screen when the font fails to load
	if ( FS_ReadFile( "default.cfg", NULL ) <= 0 ) {
		// this might happen when connecting to a pure server not using BASEGAME/pak0.pk3
		// (for instance a TA demo server)
		if (lastValidBase[0]) {
			FS_PureServerSetLoadedPaks("", "");
			Cvar_Set("fs_basepath", lastValidBase);
			Cvar_Set("com_basegame", lastValidComBaseGame);
			Cvar_Set("fs_basegame", lastValidFsBaseGame);
			Cvar_Set("fs_game", lastValidGame);
			lastValidBase[0] = '\0';
			lastValidComBaseGame[0] = '\0';
			lastValidFsBaseGame[0] = '\0';
			lastValidGame[0] = '\0';
			FS_Restart(checksumFeed);
			Com_Error( ERR_DROP, "Invalid game folder" );
			return;
		}
		Com_Error( ERR_FATAL, "Couldn't load default.cfg" );
	}

	lastGameDir = ( lastValidGame[0] ) ? lastValidGame : lastValidComBaseGame;

	if ( Q_stricmp( FS_GetCurrentGameDir(), lastGameDir ) ) {
		Sys_RemovePIDFile( );
		Sys_InitPIDFile( FS_GetCurrentGameDir() );

		// the settings are read again as the game directory changes
		// (Com_GameRestart's Com_ExecuteCfg)
	}

	Q_strncpyz(lastValidBase, fs_basepath->string, sizeof(lastValidBase));
	Q_strncpyz(lastValidComBaseGame, com_basegame->string, sizeof(lastValidComBaseGame));
	Q_strncpyz(lastValidFsBaseGame, fs_basegame->string, sizeof(lastValidFsBaseGame));
	Q_strncpyz(lastValidGame, fs_gamedirvar->string, sizeof(lastValidGame));

}

/*
=================
FS_ConditionalRestart

Restart if necessary
Return qtrue if restarting due to game directory changed, qfalse otherwise
=================
*/
qboolean FS_ConditionalRestart(int checksumFeed, qboolean disconnect)
{
	if(fs_gamedirvar->modified)
	{
		if(FS_FilenameCompare(lastValidGame, fs_gamedirvar->string) &&
				(*lastValidGame || FS_FilenameCompare(fs_gamedirvar->string, com_basegame->string)) &&
				(*fs_gamedirvar->string || FS_FilenameCompare(lastValidGame, com_basegame->string)))
		{
			Com_GameRestart(checksumFeed, disconnect);
			return qtrue;
		}
		else
			fs_gamedirvar->modified = qfalse;
	}

	if(checksumFeed != fs_checksumFeed || fs_serverPakHashesChanged)
		FS_Restart(checksumFeed);
	else if(fs_numServerPaks && !fs_reordered)
		FS_ReorderPurePaks();

	return qfalse;
}

/*
========================================================================================

Handle based file calls for virtual machines

========================================================================================
*/

int		FS_FOpenFileByMode( const char *qpath, fileHandle_t *f, fsMode_t mode ) {
	int		r;
	qboolean	sync;

	sync = qfalse;

	switch( mode ) {
		case FS_READ:
			r = FS_FOpenFileRead( qpath, f, qtrue );
			break;
		case FS_WRITE:
			*f = FS_FOpenFileWrite_HomeData( qpath );
			r = 0;
			if (*f == 0) {
				r = -1;
			}
			break;
		case FS_APPEND_SYNC:
			sync = qtrue;
		case FS_APPEND:
			*f = FS_FOpenFileAppend_HomeData( qpath );
			r = 0;
			if (*f == 0) {
				r = -1;
			}
			break;
		default:
			Com_Error( ERR_DROP, "FS_FOpenFileByMode: bad mode" );
			return -1;
	}

	if (!f) {
		return r;
	}

	if ( *f ) {
		fsh[*f].fileSize = r;
	}
	fsh[*f].handleSync = sync;

	return r;
}

/*
===========
FS_NameLength

How much of a name of length characters Windows goes by: it drops the
final dots and spaces, so "q3key." and "q3key " open q3key
===========
*/
static size_t FS_NameLength( const char *name, size_t length ) {
	while ( length && ( name[length - 1] == '.' || name[length - 1] == ' ' ) ) {
		length--;
	}
	return length;
}

/*
===========
FS_NameIs

Whether the length characters at qpath are name, ignoring case
===========
*/
static qboolean FS_NameIs( const char *qpath, size_t length, const char *name ) {
	return strlen( name ) == length && !Q_stricmpn( qpath, name, length );
}

/*
===========
FS_IsEngineFile

Whether a game directory's file is one the engine keeps for itself: the
player's settings and CD key, the console log, the crash log and the
command pipe, and anything in the dedicated servers' own directories
(servers/, FS_ServerDir). Named as the search would find it: ignoring
case, leading slashes and "./", and as Windows would, without final dots
and spaces
===========
*/
qboolean FS_IsEngineFile( const char *qpath ) {
	// the client's and the dedicated server's configs, whichever this is
	static const char *names[] = { CONFIG_PREFIX ".cfg", CONFIG_PREFIX "_server.cfg",
		"autoexec.cfg", "qconsole.log", "crashlog.txt", "q3key" };
	const char *pipe = Cvar_VariableString( "com_pipefile" );
	size_t		length;
	int		i;

	qpath = FS_SkipPathPrefix( qpath );

	// servers/ as a directory
	length = strcspn( qpath, "/\\" );
	if ( qpath[length] && FS_NameIs( qpath, FS_NameLength( qpath, length ), "servers" ) ) {
		return qtrue;
	}

	length = FS_NameLength( qpath, strlen( qpath ) );
	for ( i = 0; i < ARRAY_LEN( names ); i++ ) {
		if ( FS_NameIs( qpath, length, names[i] ) ) {
			return qtrue;
		}
	}

	// Windows also finds a file by its 8.3 short name (Q3CONF~1.CFG), so
	// a name in the directory itself with a "~" and a digit may be one of
	// the above
	if ( !strpbrk( qpath, "/\\" ) ) {
		const char *tilde;

		for ( tilde = strchr( qpath, '~' ); tilde; tilde = strchr( tilde + 1, '~' ) ) {
			if ( tilde[1] >= '0' && tilde[1] <= '9' ) {
				return qtrue;
			}
		}
	}

	return pipe[0] && FS_NameIs( qpath, length, FS_SkipPathPrefix( pipe ) );
}

/*
===========
FS_VM_FOpenFile

A module's trap_FS_FOpenFile: the handle it gets is its own, and only it may
use it (FS_HandleOwnedBy). A dedicated server's game log (g_log, which
every Quake III game module writes) goes in the server's own directory
(FS_ServerFile), so servers sharing a home keep their logs apart
===========
*/
int FS_VM_FOpenFile( const vm_t *vm, const char *qpath, fileHandle_t *f, fsMode_t mode ) {
	// as Windows goes by it (FS_NameLength)
	size_t	length = FS_NameLength( qpath, strlen( qpath ) );
	int		r;

	// the engine's own files hold the player's settings and secrets, or
	// run with full rights; reading one fails as for a missing file. A
	// ':' names another drive or an NTFS stream. Nor may it write any
	// other config, which someone may exec later with full rights
	if ( FS_IsEngineFile( qpath ) || strchr( qpath, ':' ) ||
		( mode != FS_READ && length >= 4 && !Q_stricmpn( qpath + length - 4, ".cfg", 4 ) ) ) {
		if ( mode != FS_READ ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: game code may not write %s\n", qpath );
		}
		if ( f ) {
			*f = 0;
		}
		return -1;
	}

	// only a .log: the module sets g_log, so it could otherwise name one of
	// the engine's files there (the bans, the PID file)
	if ( mode != FS_READ && *FS_ServerDir() && COM_CompareExtension( qpath, ".log" ) &&
		!Q_stricmp( FS_SkipPathPrefix( qpath ), FS_SkipPathPrefix( Cvar_VariableString( "g_log" ) ) ) ) {
		qpath = FS_ServerFile( FS_SkipPathPrefix( qpath ) );
	}
	r = FS_FOpenFileByMode( qpath, f, mode );
	if ( f && *f ) {
		fsh[*f].owner = vm;
	}
	return r;
}

/*
===========
FS_HandleOwnedBy

Whether f is open and vm opened it
===========
*/
qboolean FS_HandleOwnedBy( fileHandle_t f, const vm_t *vm ) {
	return f > 0 && f < MAX_FILE_HANDLES && fsh[f].handleFiles.file.o && fsh[f].owner == vm;
}

/*
===========
FS_VM_CloseFiles

Closes the files a module left open, when it's freed
===========
*/
void FS_VM_CloseFiles( const vm_t *vm ) {
	int		i;

	for ( i = 1; i < MAX_FILE_HANDLES; i++ ) {
		if ( FS_HandleOwnedBy( i, vm ) ) {
			FS_FCloseFile( i );
		}
	}
}

int		FS_FTell( fileHandle_t f ) {
	int pos;
	if (fsh[f].zipFile == qtrue) {
		pos = unztell(fsh[f].handleFiles.file.z);
	} else {
		pos = ftell(fsh[f].handleFiles.file.o);
	}
	return pos;
}

void	FS_Flush( fileHandle_t f ) {
	fflush(fsh[f].handleFiles.file.o);
}

void	FS_FilenameCompletion( const char *dir, const char *ext, char *filter,
		qboolean stripExt, void(*callback)(const char *s), qboolean allowNonPureFilesOnDisk ) {
	char	**filenames;
	int		nfiles;
	int		i;
	char	filename[ MAX_STRING_CHARS ];

	filenames = FS_ListFilteredFiles( dir, ext, filter,
		&nfiles, qtrue, allowNonPureFilesOnDisk );

	FS_SortFileList( filenames, nfiles );

	for( i = 0; i < nfiles; i++ ) {
		FS_ConvertPath( filenames[ i ] );
		Q_strncpyz( filename, filenames[ i ], MAX_STRING_CHARS );

		if( stripExt ) {
			COM_StripExtension(filename, filename, sizeof(filename));
		}

		callback( filename );
	}
	FS_FreeFileList( filenames );
}

const char *FS_GetCurrentGameDir(void)
{
	if(fs_gamedirvar->string[0])
		return fs_gamedirvar->string;

	return com_basegame->string;
}

/*
================
FS_ServerDir

A dedicated server's own directory in each home's game directory, for
what's its alone: its saved settings, qconsole.log, games.log, PID file
and bans, while servers sharing a home share paks and scripts.
"servers/<port>", by the port it answers on (NET_ServerPort), fixed for
the run before the config is read. "" on a client
================
*/
const char *FS_ServerDir( void )
{
	static char	dir[MAX_QPATH];

	if ( Com_IsClient() ) {
		return "";
	}
	if ( !dir[0] ) {
		Com_sprintf( dir, sizeof( dir ), "servers/%d", NET_ServerPort() );
	}
	return dir;
}

/*
================
FS_ServerFile

A file in the server's own directory (FS_ServerDir), relative to the
game directory: the name alone on a client
================
*/
const char *FS_ServerFile( const char *filename )
{
	return Com_IsClient() ? filename : va( "%s/%s", FS_ServerDir(), filename );
}

/*
================
FS_PakRefused

Whether a pak is one of the game's own (FS_RetailPakChecksum, as the
classic download refuses it) that sv_refuseRetailPaks keeps the server
from sending: the pak index doesn't hash it, and the content manifest
lists it "bring your own"
================
*/
static qboolean FS_PakRefused( const pack_t *pak ) {
	char	retail[MAX_QPATH];

	return Cvar_VariableIntegerValue( "sv_refuseRetailPaks" ) &&
		FS_RetailPakChecksum( va( "%s/%s.pk3", pak->pakGamename, pak->pakBasename ), pak->checksum, retail, sizeof( retail ) );
}

/*
================
FS_ContentRelativePath

A pak's path under its root (past the separator, two where the root ends
in one), with the separators every platform reads: only PATH_SEP, as a
':' is a name's own on Unix. qfalse if it has no root
================
*/
static qboolean FS_ContentRelativePath( const pack_t *pak, char *relative, int size ) {
	const char	*rootPath, *under;
	char		*c;

	if ( !( rootPath = FS_ContentRootPath( pak->contentRoot ) ) || strlen( pak->pakFilename ) <= strlen( rootPath ) ) {
		return qfalse;
	}
	for ( under = pak->pakFilename + strlen( rootPath ); *under == PATH_SEP; under++ ) {
	}
	Q_strncpyz( relative, under, size );
	for ( c = relative; *c; c++ ) {
		if ( *c == PATH_SEP ) {
			*c = '/';
		}
	}
	return qtrue;
}

/*
================
FS_HashPaks

At a map load, queues every pak of the search path the pak index lacks,
the referenced ones first, for the hash job; the game's own paks, known
by their checksums, only when sv_refuseRetailPaks 0 lets the server send
them
================
*/
void FS_HashPaks( void ) {
	static qboolean	warned;
	searchpath_t	*search;
	const pack_t	*pak;
	pakRecord_t		*paks, *p;
	char			relative[MAX_OSPATH];
	int				count, pass;
	for ( count = 0, search = fs_searchpaths; search; search = search->next ) {
		count += search->pack != NULL;
	}
	paks = count ? calloc( count, sizeof( *paks ) ) : NULL;
	if ( !paks ) {
		return;
	}

	count = 0;
	for ( pass = 0; pass < 2; pass++ ) {
		for ( search = fs_searchpaths; search; search = search->next ) {
			if ( !( pak = search->pack ) ) {
				continue;
			}
			// the referenced ones in the first pass, the rest in the second;
			// the game's own paks only where the server sends them
			if ( FS_PakIsReferenced( pak ) == pass || FS_PakRefused( pak ) ||
				!FS_ContentRelativePath( pak, relative, sizeof( relative ) ) ) {
				continue;
			}
			if ( strlen( relative ) >= sizeof( p->path ) ) {
				if ( !warned ) {
					warned = qtrue;
					Com_Printf( S_COLOR_YELLOW "WARNING: %s's path is too long for the pak index, so it's offered "
						"by its name alone, as are any others like it\n", pak->pakFilename );
				}
				continue;
			}
			p = &paks[count];
			if ( !Content_FileStat( pak->pakFilename, &p->size, &p->mtime ) ||
				Content_PakIndex_Find( pak->contentRoot, relative, p->size, p->mtime ) ) {
				continue;
			}
			p->root = pak->contentRoot;
			Q_strncpyz( p->path, relative, sizeof( p->path ) );
			p->pakChecksum = pak->checksum;
			count++;
		}
	}

	Content_PakIndex_Queue( paks, count );
	free( paks );
}

/*
================
FS_ContentPaks
================
*/
int FS_ContentPaks( contentPak_t *paks, int max ) {
	const searchpath_t	*search;
	const pack_t		*pak;
	const pakRecord_t	*record;
	contentPak_t		*p;
	char				relative[MAX_OSPATH];
	int					count = 0;

	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( !( pak = search->pack ) ) {
			continue;
		}
		if ( paks && count < max ) {
			p = &paks[count];
			Com_Memset( p, 0, sizeof( *p ) );
			p->gameDir = pak->pakGamename;
			p->name = pak->pakBasename;
			p->ospath = pak->pakFilename;
			p->checksum = pak->checksum;
			p->referenced = FS_PakIsReferenced( pak );
			p->refused = FS_PakRefused( pak );
			p->code = FS_PakHasFile( pak, "vm/cgame.qvm" ) || FS_PakHasFile( pak, "vm/ui.qvm" );
			// a refused pak is known by its checksum alone, even where an
			// earlier run or another process sharing the home hashed it, so
			// nothing finds it by that hash to send; and a record another
			// writer marked retail has no hash
			if ( !Content_FileStat( pak->pakFilename, &p->size, &p->mtime ) ) {
				p->size = -1;
			} else if ( !p->refused && FS_ContentRelativePath( pak, relative, sizeof( relative ) ) &&
				( record = Content_PakIndex_Find( pak->contentRoot, relative, p->size, p->mtime ) ) != NULL &&
				!( record->flags & PAK_RECORD_RETAIL ) ) {
				p->sha256 = record->sha256;
			}
		}
		count++;
	}
	return count;
}

/*
================
FS_HasInstalledPak

Whether the search path has an installed pak for a content manifest entry:
a "bring your own" one's with its checksum, any other's with its checksum,
size, game directory and name, and the entry's hash unless the pak index
hasn't hashed it yet. Another pak of the same files under another name
shares the checksum, and often the size, and the index hashes an installed
pak only for a server whose content manifest lists its checksum, so a twin
never used on one has no hash to tell it apart (P242). A copy under
another name that the index has hashed is found by its hash instead
(CL_ContentReady)
================
*/
qboolean FS_HasInstalledPak( const ofcmEntry_t *e ) {
	const searchpath_t	*search;
	const pack_t		*pak;
	int64_t				pakSize, mtime;
	byte				have[SHA256_DIGEST_SIZE];

	for ( search = fs_searchpaths; search; search = search->next ) {
		pak = search->pack;
		if ( !pak || pak->cached || pak->checksum != (unsigned int)e->checksum ) {
			continue;
		}
		if ( e->flags & OFCM_FLAG_OWN ) {
			return qtrue;
		}
		if ( !Q_stricmp( pak->pakGamename, e->gameDir ) && !Q_stricmp( pak->pakBasename, e->name ) &&
			Content_FileStat( pak->pakFilename, &pakSize, &mtime ) && pakSize == e->size &&
			( !FS_PakSHA256( pak, have ) || !memcmp( have, e->sha256, SHA256_DIGEST_SIZE ) ) ) {
			return qtrue;
		}
	}
	return qfalse;
}
