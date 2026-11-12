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
#include "pak_dir.h"
#include "content.h"
#include <errno.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <io.h>
#endif

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
from the highest number to the lowest.  A game directory's loose files are searched before
its zip files, and a zip file that sorts later overrides the files of earlier ones, so a pk3
distributed as a patch overrides all existing data.

Because we will have updated executables freely available online, there is no point to
trying to restrict demo / oem versions of the game with code changes.  Demo / oem versions
should be exactly the same executables as release versions, but with different data that
automatically restricts where game media can come from to prevent add-ons from working.

File search order: when FS_FOpenFileRead gets called it will go through the fs_searchpaths
structure and stop on the first successful hit. fs_searchpaths is built with successive
calls to FS_AddGameDirectory. Each entry has a tier, and the lookups walk the entries of
tier 0 in that order, then those of tier 1 (FS_NextSearch); the path's own order, which
the pure lists walk, stays as built

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
// paks (FS_RetailPak), any of pak0 to pak8 (FS_CheckPak0, FS_ChooseDemo),
// from OMNIFRAG_TEST_REFUSED_PAK_CHECKSUM at startup
static qboolean		fs_refusedTestPak;
static unsigned int	fs_refusedTestChecksum;

static qboolean FS_TestPakChecksum( unsigned int checksum ) {
	return fs_refusedTestPak && checksum == fs_refusedTestChecksum;
}

// the demo the engine chose (FS_ChooseDemo): from demoq3, where a dedicated
// server searches baseq3 as a directory alone (FS_Startup); or from
// baseq3's pak0, where the full game's point release there is left out
// (FS_AddGameDirectory), each one's bit in fs_leftOutPaks (FS_LeftOutPaks)
typedef enum {
	FS_DEMO_NONE,
	FS_DEMO_SERVER_DIR,
	FS_DEMO_IN_BASE
} fsDemo_t;
static fsDemo_t		fs_demo;
static unsigned int	fs_leftOutPaks, fs_leftOutSaid;

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

#define	MAX_SEARCH_PATHS	4096

typedef struct {
	// in the pack's own block, after its hash table, each as long as it
	// is: MAX_OSPATH is PATH_MAX, 4,096 on Linux, and four of those a pak
	// filled the main zone at about 2,900 paks (P349)
	char			*pakPathname;				// c:\quake3\baseq3
	char			*pakFilename;				// c:\quake3\baseq3\pak0.pk3
	char			*pakBasename;				// pak0
	char			*pakGamename;				// baseq3
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
	qboolean		holdsMap, holdsCode;		// a maps/*.bsp; a vm/*.qvm (FS_MapPack)
	qboolean		excluded;					// the server's own: fs_excludeReference names it (FS_ExcludePaks)
} pack_t;

typedef struct {
	char		path[MAX_OSPATH];		// c:\quake3
	char		fullpath[MAX_OSPATH];		// c:\quake3\baseq3
	char		gamedir[MAX_OSPATH];	// baseq3
	qboolean	holdsMap, holdsCode;	// a pk3dir's, as a pak's
} directory_t;

typedef struct searchpath_s {
	struct searchpath_s *next;

	pack_t		*pack;		// only one of pack / dir will be non NULL
	directory_t	*dir;
	int			tier;		// the lookups' rank, below FS_TIERS (FS_NextSearch)
	int			order;		// its place in the lookups' order (FS_IndexOrder)
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
static	int			fs_lastTier;			// the highest tier an entry has
// the map the server runs or the client plays, "" for none, and the entry
// its BSP loads from (FS_SetCurrentMap)
static	char		fs_currentMap[MAX_QPATH];
static	const searchpath_t	*fs_mapSearch;
static	int			fs_rankings;			// how many times a read's copy may have moved (FS_Ranking)
static	unsigned	fs_pathSignature;		// of the search path as last ranked (FS_PathSignature)
static	cvar_t		*fs_mapPakScope;
static	cvar_t		*fs_globalPaks;
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

// the referenced paks a non-pure server couldn't send that the player has
// another version of, played with that (FS_PlayOwnVersions), by name,
// "<game>/<name>", and checksum
static int			fs_numOwnVersions;
static unsigned int	fs_ownVersions[MAX_OWN_VERSIONS];
static char			fs_ownVersionNames[MAX_OWN_VERSIONS][MAX_QPATH];

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
FS_OpenOSFile

A file outside the search path opened to write, its folders made: a
private one readable by its user alone (Sys_FOpenPrivate), as tokens and
a server's own state are
===========
*/
static FILE *FS_OpenOSFile( const char *ospath, const char *mode, qboolean private ) {
	FILE	*f;
#ifndef _WIN32
	mode_t	saved;
#endif

	if ( FS_CreatePath( ospath ) ) {
		return NULL;
	}
	if ( private ) {
		return Sys_FOpenPrivate( ospath, mode );
	}
#ifndef _WIN32
	saved = umask( 0022 );
#endif
	f = Sys_FOpen( ospath, mode );
#ifndef _WIN32
	umask( saved );
#endif
	return f;
}

/*
===========
FS_ReplaceOSFile

Replaces a file outside the search path whole, through a file beside it
of this process's own, flushed to the disk and renamed over it, so a crash
leaves the old one or the new: a private one readable by its user alone,
another keeping the mode it had. Another, an admin's, may be a link, or
a file a container mounts alone, which a rename can't replace: the link's
target is replaced, and the mounted file written in place. --check writes
nothing
===========
*/
qboolean FS_ReplaceOSFile( const char *ospath, const void *data, int length, qboolean private ) {
	char		target[MAX_OSPATH], tmp[MAX_OSPATH];
	FILE		*f;
	qboolean	ok;
#ifndef _WIN32
	struct stat	st;
	qboolean	kept;
#endif

	if ( com_check ) {
		return qfalse;
	}
	// ospath may be a va() buffer, which the calls below reuse
	Q_strncpyz( target, ospath, sizeof( target ) );
#ifndef _WIN32
	if ( !private ) {
		// malloc's, as PATH_MAX may be past MAX_OSPATH
		char	*resolved = realpath( ospath, NULL );

		if ( resolved ) {
			Q_strncpyz( target, resolved, sizeof( target ) );
			free( resolved );
		}
	}
#endif
	if ( Com_sprintf( tmp, sizeof( tmp ), "%s.%d.tmp", target, Sys_PID() ) >= (int)sizeof( tmp ) ) {
		return qfalse;
	}
#ifndef _WIN32
	kept = !private && stat( target, &st ) == 0;
#endif
	f = FS_OpenOSFile( tmp, "wb", private );
	if ( !f ) {
		return qfalse;
	}
	ok = (int)fwrite( data, 1, length, f ) == length && fflush( f ) == 0;
#ifdef _WIN32
	ok = ok && _commit( _fileno( f ) ) == 0;
#else
	ok = ok && fsync( fileno( f ) ) == 0;
	if ( kept ) {
		ok = ok && fchmod( fileno( f ), st.st_mode & 07777 ) == 0;
	}
#endif
	ok = fclose( f ) == 0 && ok;
	if ( ok && Sys_Rename( tmp, target ) ) {
		return qtrue;
	}
	remove( tmp );
	if ( !ok || private ) {
		return qfalse;
	}
	// a mounted file: written in place, as an editor would
	f = Sys_FOpen( target, "r+b" );
	if ( !f ) {
		return qfalse;
	}
	ok = (int)fwrite( data, 1, length, f ) == length && fflush( f ) == 0;
#ifdef _WIN32
	ok = ok && _chsize( _fileno( f ), length ) == 0;
#else
	ok = ok && ftruncate( fileno( f ), length ) == 0;
#endif
	return fclose( f ) == 0 && ok;
}

/*
===========
FS_AppendOSFile

Adds to the end of a file outside the search path, made if it isn't
there, private as FS_OpenOSFile makes it. --check writes nothing
===========
*/
qboolean FS_AppendOSFile( const char *ospath, const void *data, int length, qboolean private ) {
	FILE		*f;
	qboolean	ok;

	if ( com_check ) {
		return qfalse;
	}
	f = FS_OpenOSFile( ospath, "ab", private );
	if ( !f ) {
		return qfalse;
	}
	ok = (int)fwrite( data, 1, length, f ) == length;
	return fclose( f ) == 0 && ok;
}

/*
===========
FS_ReadOSFile

A file outside the search path, into memory from the main zone (free it
with Z_Free)
===========
*/
long FS_ReadOSFile( const char *ospath, void **buffer )
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
FS_PakEntry

A pak's entry for a file, or NULL
================
*/
static const fileInPack_t *FS_PakEntry( const pack_t *pak, const char *name ) {
	const fileInPack_t	*file;

	for ( file = pak->hashTable[FS_HashFileName( name, pak->hashSize )]; file; file = file->next ) {
		if ( !FS_FilenameCompare( file->name, name ) ) {
			return file;
		}
	}
	return NULL;
}

static qboolean FS_PakHasFile( const pack_t *pak, const char *name ) {
	return FS_PakEntry( pak, name ) != NULL;
}

// whether exec runs no config of this name from a download: all but default.cfg
static qboolean FS_CachePakRefusesName( const char *filename ) {
	return Q_stricmp( FS_SkipPathPrefix( filename ), "default.cfg" ) != 0;
}

/*
===========
FS_CachePakRefuses

Whether exec passes over a file in a pak from the content cache, a
download: any but default.cfg, which a mod needs, and whose reading from
this pak is said once a mount, since a server could fill a config with
commands. The game reads the rest as any pak's, a player model's
animation.cfg among them
===========
*/
static qboolean FS_CachePakRefuses( pack_t *pak, const char *filename ) {

	if ( FS_CachePakRefusesName( filename ) ) {
		return qtrue;
	}
	// a search passes over every pak above the one that has it
	if ( !pak->defaultCfgNoted && FS_PakHasFile( pak, filename ) ) {
		pak->defaultCfgNoted = qtrue;
		Com_Printf( "Reading default.cfg from %s/%s.pk3, downloaded to the content cache\n",
			pak->pakGamename, pak->pakBasename );
	}
	return qfalse;
}

/*
===========
FS_NextSearch

The search path's entry after search in the lookups' order, or the first
for NULL: tier 0's entries in the path's order, then tier 1's; NULL after
the last
===========
*/
static searchpath_t *FS_NextSearch( const searchpath_t *search ) {
	int				tier = search ? search->tier : 0;
	searchpath_t	*next = search ? search->next : fs_searchpaths;

	for ( ; tier <= fs_lastTier; tier++, next = fs_searchpaths ) {
		for ( ; next; next = next->next ) {
			if ( next->tier == tier ) {
				return next;
			}
		}
	}
	return NULL;
}

/*
=============================================================================

THE NAME INDEX

Every pak's files by name, so a lookup asks only the paks that have the
name, and the directories, in the lookups' order, rather than every entry:
built with the search path (FS_Startup), each entry numbered in the
lookups' order whenever that changes (FS_RankPaks, FS_ReorderPurePaks).
It holds no rule, only which paks have a name: a lookup still asks each
entry it visits as it always did, each pak by its own table, so purity,
tiers and the other rules are as without it. The directories aren't
indexed, as files come and go in them. fs_index 0 walks the whole path
instead; fs_indexCheck 1 walks it beside each lookup and says where the two
differ (2 stops at the first)
=============================================================================
*/

// a pak's file in the index, kept apart from the pak's own table, outside the zone
typedef struct indexEntry_s {
	const fileInPack_t		*file;
	searchpath_t			*search;	// its pak's entry in the search path
	struct indexEntry_s		*next;		// next of any pak's in its chain
} indexEntry_t;

static indexEntry_t	**fs_index;
static indexEntry_t	*fs_indexEntries;
static unsigned int	fs_indexMask;
static searchpath_t	**fs_indexDirs;		// the directories, in the lookups' order
static int			fs_numIndexDirs;
static cvar_t		*fs_indexUse, *fs_indexCheck;
static int			fs_indexMismatches, fs_indexWalks;
static int			fs_lookupTimed;			// fs_lookup's timing: 1 of the index, 2 of the walk

// the most paks a lookup takes from the index; a name in more walks the path
#define INDEX_MAX_PAKS	256

static const char *FS_SearchSource( const searchpath_t *search );

/*
===========
FS_IndexHash

A name's hash for the index, over all of it, folding exactly what
FS_FilenameCompare folds: ASCII case, whatever the locale, and the
separators it takes as one
===========
*/
static unsigned int FS_IndexHash( const char *name ) {
	unsigned int	hash = 2166136261u;		// FNV-1a
	int				c;

	for ( ; *name; name++ ) {
		c = (unsigned char)*name;
		if ( c >= 'A' && c <= 'Z' ) {
			c += 'a' - 'A';
		} else if ( c == '\\' || c == ':' ) {
			c = '/';
		}
		hash = ( hash ^ (unsigned int)c ) * 16777619u;
	}
	return hash;
}

/*
===========
FS_IndexOrder

Numbers the entries in the lookups' order and lists the directories in it
===========
*/
static void FS_IndexOrder( void ) {
	searchpath_t	*search;
	int				order = 0;

	fs_numIndexDirs = 0;
	for ( search = FS_NextSearch( NULL ); search; search = FS_NextSearch( search ) ) {
		search->order = order++;
		if ( search->dir && fs_indexDirs ) {
			fs_indexDirs[fs_numIndexDirs++] = search;
		}
	}
}

/*
===========
FS_IndexClear
===========
*/
static void FS_IndexClear( void ) {
	free( fs_index );
	free( fs_indexEntries );
	if ( fs_indexDirs ) {
		Z_Free( fs_indexDirs );
	}
	fs_index = NULL;
	fs_indexEntries = NULL;
	fs_indexDirs = NULL;
	fs_indexMask = 0;
	fs_numIndexDirs = 0;
}

/*
===========
FS_IndexBuild

The index of every pak's files, as many slots as files, each chain in the
path's order (the paks added last first, at each chain's head), so a
name's paks come mostly in order; and the entries numbered, for the
lookups before FS_RankPaks numbers them again
===========
*/
static void FS_IndexBuild( void ) {
	searchpath_t	*search, **paks;
	indexEntry_t	*entry, **slot;
	unsigned int	size;
	int				i, p, numPaks = 0, files = 0, dirs = 0, start = Sys_Milliseconds();

	FS_IndexClear();
	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( search->pack ) {
			files += search->pack->numfiles;
			numPaks++;
		} else if ( search->dir ) {
			dirs++;
		}
	}
	for ( size = 1024; size < (unsigned int)files; size *= 2 ) {
	}
	fs_indexDirs = Z_Malloc( ( dirs + 1 ) * sizeof( *fs_indexDirs ) );
	paks = Z_Malloc( ( numPaks + 1 ) * sizeof( *paks ) );
	fs_index = calloc( size, sizeof( *fs_index ) );
	fs_indexEntries = malloc( ( files + 1 ) * sizeof( *fs_indexEntries ) );
	if ( !fs_index || !fs_indexEntries ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: no memory for the file name index, so each lookup asks every pak\n" );
		free( fs_index );
		free( fs_indexEntries );
		fs_index = NULL;
		fs_indexEntries = NULL;
		Z_Free( paks );
		return;
	}
	fs_indexMask = size - 1;
	for ( search = fs_searchpaths, p = 0; search; search = search->next ) {
		if ( search->pack ) {
			paks[p++] = search;
		}
	}
	for ( p = numPaks - 1, entry = fs_indexEntries; p >= 0; p-- ) {
		for ( i = 0; i < paks[p]->pack->numfiles; i++, entry++ ) {
			entry->file = &paks[p]->pack->buildBuffer[i];
			entry->search = paks[p];
			slot = &fs_index[FS_IndexHash( entry->file->name ) & fs_indexMask];
			entry->next = *slot;
			*slot = entry;
		}
	}
	Z_Free( paks );
	FS_IndexOrder();
	Com_DPrintf( "Indexed %d files of %d paks in %d msec, %d KB\n", files, numPaks, Sys_Milliseconds() - start,
		(int)( ( size * sizeof( *fs_index ) + files * sizeof( *fs_indexEntries ) +
		( dirs + 1 ) * sizeof( *fs_indexDirs ) ) / 1024 ) );
}

/*
===========
FS_IndexPaks

The paks that have a name, in the lookups' order, at most INDEX_MAX_PAKS;
qfalse when the index can't say (none built, fs_index 0, or the name in
more paks), so the lookup walks the whole path
===========
*/
static qboolean FS_IndexPaks( const char *filename, searchpath_t **paks, int *numPaks ) {
	const indexEntry_t	*entry;
	searchpath_t		*swap;
	int					i, j;

	if ( !fs_index || !fs_indexUse || !fs_indexUse->integer || fs_lookupTimed == 2 ) {
		return qfalse;
	}
	// as FS_FOpenFileReadDir takes it
	if ( filename[0] == '/' || filename[0] == '\\' ) {
		filename++;
	}
	*numPaks = 0;
	for ( entry = fs_index[FS_IndexHash( filename ) & fs_indexMask]; entry; entry = entry->next ) {
		if ( FS_FilenameCompare( entry->file->name, filename ) ) {
			continue;
		}
		// a pak with the name twice is asked once
		if ( *numPaks && paks[*numPaks - 1] == entry->search ) {
			continue;
		}
		if ( *numPaks == INDEX_MAX_PAKS ) {
			fs_indexWalks++;
			return qfalse;
		}
		paks[( *numPaks )++] = entry->search;
	}
	// in the path's order already; the tiers may move some
	for ( i = 1; i < *numPaks; i++ ) {
		for ( j = i; j > 0 && paks[j]->order < paks[j - 1]->order; j-- ) {
			swap = paks[j];
			paks[j] = paks[j - 1];
			paks[j - 1] = swap;
		}
	}
	return qtrue;
}

/*
===========
FS_IndexNext

The next entry a lookup asks: of the name's paks and the directories,
the earlier in the lookups' order; NULL after the last. *p and *d start at 0
===========
*/
static searchpath_t *FS_IndexNext( searchpath_t **paks, int numPaks, int *p, int *d ) {
	if ( *d < fs_numIndexDirs && ( *p == numPaks || fs_indexDirs[*d]->order < paks[*p]->order ) ) {
		return fs_indexDirs[( *d )++];
	}
	return *p < numPaks ? paks[( *p )++] : NULL;
}

/*
===========
FS_PureRefusesDirFile

Whether, connected to a pure server, a file can't be read from a
directory: any but configs, menus and demos
===========
*/
static qboolean FS_PureRefusesDirFile( const char *filename ) {
	int		len;

	if ( !fs_numServerPaks ) {
		return qfalse;
	}
	len = strlen( filename );
	return !FS_IsExt( filename, ".cfg", len ) &&		// for config files
		!FS_IsExt( filename, ".menu", len ) &&		// menu files
		!FS_IsExt( filename, ".game", len ) &&		// menu files
		!FS_IsExt( filename, ".dat", len ) &&		// for journal files
		!FS_IsDemoExt( filename, len );			// demos
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
		// FIXME TTimo I'm not sure about the fs_numServerPaks test
		// if you are using FS_ReadFile to find out if a file exists,
		//   this test can make the search fail although the file is in the directory
		// I had the problem on https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=8
		// turned out I used FS_FileExists_HomeData instead
		if(!unpure && FS_PureRefusesDirFile(filename))
		{
			*file = 0;
			return -1;
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
FS_LastFileStat

The size and modification time of where the last file FS_FOpenFileRead
found, qpath, is: the file's own, or its pak's; qfalse if it found none
===========
*/
qboolean FS_LastFileStat( const char *qpath, int64_t *size, int64_t *mtime ) {
	if ( !fs_lastFileSearch ) {
		return qfalse;
	}
	if ( fs_lastFileSearch->pack ) {
		return Content_FileStat( fs_lastFileSearch->pack->pakFilename, size, mtime );
	}
	return Content_FileStat( FS_BuildOSPath( fs_lastFileSearch->dir->path, fs_lastFileSearch->dir->gamedir, qpath ),
		size, mtime );
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
FS_TryRead

A read of a file from one search path entry, by the rules every read
keeps: whether it's there (and so its length, and the entry in
fs_lastFileSearch)
===========
*/
static qboolean FS_TryRead( searchpath_t *search, const char *filename, fileHandle_t *file, qboolean uniqueFILE,
	qboolean isExec, qboolean isLocalConfig, long *len )
{
	// autoexec.cfg and q3config.cfg can only be loaded outside of pk3
	// files and pk3dirs, which come with the game, a mod or a download
	if ( isLocalConfig && FS_IsGameContent( search ) ) {
		return qfalse;
	}
	// and exec runs no config from a download but default.cfg
	if ( isExec && search->pack && search->pack->cached && FS_CachePakRefuses( search->pack, filename ) ) {
		return qfalse;
	}
	*len = FS_FOpenFileReadDir( filename, search, file, uniqueFILE, qfalse );
	if ( file == NULL ? *len > 0 : *len >= 0 && *file ) {
		fs_lastFileSearch = search;
		return qtrue;
	}
	return qfalse;
}

/*
===========
FS_WalkFinds

The entry a lookup would stop at walking the whole path, by FS_TryRead's
rules, but marking nothing and opening nothing: whether the file is there
(exists), as FS_FOpenFileReadDir without a handle says, or can be read, as
FS_SearchHasFile says. fs_indexCheck's other half
===========
*/
static qboolean FS_SearchHasFile( const searchpath_t *search, const char *filename );

static const searchpath_t *FS_WalkFinds( const char *filename, qboolean exists, qboolean isExec, qboolean isLocalConfig )
{
	const searchpath_t	*search;

	for ( search = FS_NextSearch( NULL ); search; search = FS_NextSearch( search ) ) {
		if ( ( isLocalConfig && FS_IsGameContent( search ) ) ||
			( isExec && search->pack && search->pack->cached && FS_CachePakRefusesName( filename ) ) ) {
			continue;
		}
		if ( exists ? FS_FOpenFileReadDir( filename, (searchpath_t *)search, NULL, qfalse, qfalse ) > 0
			: FS_SearchHasFile( search, filename ) ) {
			return search;
		}
	}
	return NULL;
}

/*
===========
FS_IndexChecked

Under fs_indexCheck, the entry an indexed lookup found against the walk's
===========
*/
static void FS_IndexChecked( const char *filename, const searchpath_t *found, qboolean exists, qboolean isExec,
	qboolean isLocalConfig )
{
	const searchpath_t	*walked;

	// fs_lookup times the lookups alone
	if ( !fs_indexCheck || !fs_indexCheck->integer || fs_lookupTimed ) {
		return;
	}
	walked = FS_WalkFinds( filename, exists, isExec, isLocalConfig );
	if ( walked == found ) {
		return;
	}
	fs_indexMismatches++;
	Com_Printf( S_COLOR_YELLOW "WARNING: fs index: %s from %s, the walk's from %s\n", filename,
		found ? FS_SearchSource( found ) : "nowhere", walked ? FS_SearchSource( walked ) : "nowhere" );
	if ( fs_indexCheck->integer > 1 ) {
		Com_Error( ERR_FATAL, "fs index: %s found apart from the walk (fs_indexCheck 2)", filename );
	}
}

/*
===========
FS_FOpenFileReadAs

FS_FOpenFileRead, which for exec passes over the configs in the content
cache's paks, but default.cfg
===========
*/
static long FS_FOpenFileReadAs(const char *filename, fileHandle_t *file, qboolean uniqueFILE, qboolean isExec)
{
	searchpath_t *search, *paks[INDEX_MAX_PAKS];
	long len;
	qboolean isLocalConfig;
	int numPaks, p = 0, d = 0;

	if(!fs_searchpaths)
		Com_Error(ERR_FATAL, "Filesystem call made without initialization");

	fs_lastFileSearch = NULL;
	isLocalConfig = FS_IsLocalConfig( filename );
	if ( FS_IndexPaks( filename, paks, &numPaks ) ) {
		while ( ( search = FS_IndexNext( paks, numPaks, &p, &d ) ) &&
			!FS_TryRead( search, filename, file, uniqueFILE, isExec, isLocalConfig, &len ) ) {
		}
		FS_IndexChecked( filename, search, file == NULL, isExec, isLocalConfig );
		if ( search ) {
			return len;
		}
	} else {
		for ( search = FS_NextSearch( NULL ); search; search = FS_NextSearch( search ) ) {
			if ( FS_TryRead( search, filename, file, uniqueFILE, isExec, isLocalConfig, &len ) ) {
				return len;
			}
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
	return FS_FOpenFileReadAs(filename, file, uniqueFILE, qfalse);
}

/*
===========
FS_SearchHasFile

Whether a read of a file would find it in a search path entry: one a
pure server allows, which has it. Marks no pak referenced
===========
*/
static qboolean FS_SearchHasFile( const searchpath_t *search, const char *filename )
{
	if ( search->dir && FS_PureRefusesDirFile( filename ) ) {
		return qfalse;
	}
	return FS_FOpenFileReadDir( filename, (searchpath_t *)search, NULL, qfalse, qfalse ) > 0 &&
		( !search->pack || FS_PakIsPure( search->pack ) );
}

/*
===========
FS_FileSearch

The entry a read of a file would come from, or NULL
===========
*/
static const searchpath_t *FS_FileSearch( const char *filename )
{
	searchpath_t		*paks[INDEX_MAX_PAKS], *search;
	int					numPaks, p = 0, d = 0;

	if ( !FS_IndexPaks( filename, paks, &numPaks ) ) {
		return FS_WalkFinds( filename, qfalse, qfalse, qfalse );
	}
	while ( ( search = FS_IndexNext( paks, numPaks, &p, &d ) ) && !FS_SearchHasFile( search, filename ) ) {
	}
	FS_IndexChecked( filename, search, qfalse, qfalse, qfalse );
	return search;
}

/*
===========
FS_FileTier

The tier of the entry FS_FOpenFileRead would read a file from, or -1 for
none, without marking a pak referenced: so a loop over a name's
extensions can try each in tier 0 before any in tier 1
(COM_NextByTier). While every entry is in tier 0, it's 0 without a
look, whether the file is there or not, so the loop costs what it did
===========
*/
int FS_FileTier( const char *filename )
{
	const searchpath_t	*search;

	if ( !fs_searchpaths ) {
		Com_Error( ERR_FATAL, "Filesystem call made without initialization" );
	}
	if ( !fs_lastTier ) {
		return 0;
	}
	search = FS_FileSearch( filename );
	return search ? search->tier : -1;
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
	search = FS_NextSearch(lastSearch);

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
					search = FS_NextSearch(search);
					continue;
				}
			}

			if(FS_FOpenFileReadDir(qvmName, search, NULL, qfalse, qfalse) > 0)
			{
				*startSearch = search;

				return VMI_COMPILED;
			}
		}

		search = FS_NextSearch(search);
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

	for ( search = FS_NextSearch( NULL ) ; search ; search = FS_NextSearch( search ) ) {
		//
		if (search->pack) {
			hash = FS_HashFileName(filename, search->pack->hashSize);
		}
		// is the element a pak file?
		if ( search->pack && search->pack->hashTable[hash] ) {
			// disregard if it doesn't match one of the allowed pure pak
			// files, or is the server's own, which clients don't have
			if ( !FS_PakIsPure(search->pack) || search->pack->excluded ) {
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
FS_ReadFileAs

Filename are relative to the quake search path
a null buffer will just return the file length without loading
If searchPath is non-NULL search only in that specific search path
With isExec, the search is exec's (FS_FOpenFileReadAs)
============
*/
static long FS_ReadFileAs(const char *qpath, void *searchPath, qboolean unpure, qboolean isExec, void **buffer)
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
		len = FS_FOpenFileReadAs(qpath, &h, qfalse, isExec);
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
FS_ReadFileDir

If searchPath is non-NULL search only in that specific search path
============
*/
long FS_ReadFileDir(const char *qpath, void *searchPath, qboolean unpure, void **buffer)
{
	return FS_ReadFileAs(qpath, searchPath, unpure, qfalse, buffer);
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
============
FS_ReadConfig

FS_ReadFile for exec: it passes over the configs in the content cache's
paks, but default.cfg, to the next place that has the file
============
*/
long FS_ReadConfig( const char *qpath, void **buffer ) {
	return FS_ReadFileAs( qpath, NULL, qfalse, qtrue, buffer );
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

#ifdef __EMSCRIPTEN__
/*
============
FS_OfferDownload

A file handed to the player as the page's download (its saveFile), named
for its path
============
*/
void FS_OfferDownload( const char *qpath, const void *buffer, int size ) {
	MAIN_THREAD_EM_ASM( { Module.saveFile?.( UTF8ToString( $0 ), HEAPU8.slice( $1, $1 + $2 ) ); },
		qpath, buffer, size );
}
#endif

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
		FS_OfferDownload( qpath, buffer, size );
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
FS_IsMapFile

Whether a pak's file is a map, maps/<name>.bsp, with either separator, as
the lookups take both (FS_FilenameCompare)
=================
*/
static qboolean FS_IsMapFile( const char *name )
{
	return !Q_stricmpn( name, "maps", 4 ) && ( name[4] == '/' || name[4] == '\\' ) && !strpbrk( name + 5, "/\\" ) &&
		COM_CompareExtension( name, ".bsp" );
}

/*
=================
FS_IsShaderFile

Whether a pak's file is a shader file, scripts/<name>.shader
=================
*/
static qboolean FS_IsShaderFile( const char *name )
{
	return !Q_stricmpn( name, "scripts/", 8 ) && COM_CompareExtension( name, ".shader" );
}

/*
=================
FS_LoadZipFile

Creates a new pak_t in the search chain for the contents
of a zip file.
=================
*/
static pack_t *FS_LoadZipFile(const char *zipfile, const char *basename, const char *pathname, const char *gamename)
{
	pack_t			*pack;
	unzFile			uf;
	unz_global_info gi;
	int				i, numfiles, len, hashSize, baseLength;

	uf = unzOpen(zipfile);
	if (unzGetGlobalInfo (uf,&gi) != UNZ_OK) {
		unzClose( uf );
		return NULL;
	}

	numfiles = PakDir_Count( uf, gi.number_entry, &len );
	if ( numfiles < gi.number_entry ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: %s: read %d of its %d entries, so the rest are left out\n",
			zipfile, numfiles, (int)gi.number_entry );
	}
	hashSize = PakDir_HashSize( numfiles );

	// the base name without .pk3
	baseLength = strlen( basename );
	if ( baseLength > 4 && !Q_stricmp( basename + baseLength - 4, ".pk3" ) ) {
		baseLength -= 4;
	}
	pack = Z_Malloc( sizeof( pack_t ) + hashSize * sizeof(fileInPack_t *) + strlen( zipfile ) + 1 + baseLength + 1 +
		strlen( pathname ) + 1 + strlen( gamename ) + 1 );
	// Z_Malloc clears the block, the hash table with it
	pack->hashSize = hashSize;
	pack->hashTable = (fileInPack_t **) (((char *) pack) + sizeof( pack_t ));

	// the names after the hash table
	pack->pakFilename = (char *)( pack->hashTable + pack->hashSize );
	strcpy( pack->pakFilename, zipfile );
	pack->pakBasename = pack->pakFilename + strlen( zipfile ) + 1;
	Com_Memcpy( pack->pakBasename, basename, baseLength );
	pack->pakBasename[baseLength] = '\0';
	pack->pakPathname = pack->pakBasename + baseLength + 1;
	strcpy( pack->pakPathname, pathname );
	pack->pakGamename = pack->pakPathname + strlen( pathname ) + 1;
	strcpy( pack->pakGamename, gamename );

	pack->handle = uf;
	pack->numfiles = PakDir_Read( uf, numfiles, len, fs_checksumFeed, pack->hashTable, pack->hashSize,
		&pack->buildBuffer, &pack->checksum, &pack->pure_checksum );

	// either separator, as the lookups take both (FS_FilenameCompare)
	for (i = 0; i < pack->numfiles; i++) {
		const char	*name = pack->buildBuffer[i].name;

		pack->holdsMap |= FS_IsMapFile( name );
		pack->holdsCode |= !strncmp( name, "vm", 2 ) && ( name[2] == '/' || name[2] == '\\' ) &&
			COM_CompareExtension( name, ".qvm" );
	}
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

static qboolean FS_HoldsMapOnly( const searchpath_t *search );
static const char *FS_PakConflictsWhere( const searchpath_t *search );
static int FS_DroppedShaders( const searchpath_t *search, const searchpath_t *other, const char *name,
	qboolean print );

/*
=================
FS_AddPakEntryRefused

Why content add refuses a pak's entry by its name, with the verdict's
code, or NULL: a name that could reach outside the game's directories, or
native code
=================
*/
static const char *FS_AddPakEntryRefused( const char *name, const char **code ) {
	size_t	length = strlen( name );

	if ( name[0] == '/' || strchr( name, '\\' ) || strchr( name, ':' ) || !strcmp( name, ".." )
		|| !Q_strncmp( name, "../", 3 ) || strstr( name, "/../" ) || ( length >= 3 && !strcmp( name + length - 3, "/.." ) ) ) {
		*code = "escapes";
		return "a name that leaves the game's directories";
	}
	if ( FS_IsNativeFile( name ) ) {
		*code = "native_code";
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
FS_Zip64Records

Whether a zip has zip64's records, which unzip doesn't read: an end record
that leaves its directory's size or place to them (0xffffffff), or zip64's
locator just before it, whose records unzip then takes for bytes before
the zip (byte_before_the_zipfile), so it finds no entry where the
directory says. The last end record, as unzip finds it
(unzlocal_SearchCentralDir) and the web's pak check reads it
(code/web/pak_check.js)
=================
*/
#define ZIP_END_RECORD			0x06054b50
#define ZIP_END_SIZE			22
#define ZIP64_LOCATOR			0x07064b50
#define ZIP64_LOCATOR_SIZE		20
#define ZIP64_VERSION			45		// the version an entry needs that has zip64's sizes

static unsigned int FS_ZipLong( const byte *b ) {
	return b[0] | ( b[1] << 8 ) | ( b[2] << 16 ) | ( (unsigned int)b[3] << 24 );
}

static qboolean FS_Zip64Records( const char *ospath ) {
	FILE		*f = Sys_FOpen( ospath, "rb" );
	byte		*tail;
	long		size, back, at;
	qboolean	zip64 = qfalse;

	if ( !f ) {
		return qfalse;
	}
	if ( fseek( f, 0, SEEK_END ) || ( size = ftell( f ) ) < ZIP_END_SIZE ) {
		fclose( f );
		return qfalse;
	}
	// the end record and the longest comment after it
	back = MIN( size, 0xffff + ZIP_END_SIZE );
	tail = Z_Malloc( back );
	if ( !fseek( f, size - back, SEEK_SET ) && fread( tail, 1, back, f ) == (size_t)back ) {
		for ( at = back - ZIP_END_SIZE; at >= 0; at-- ) {
			if ( FS_ZipLong( tail + at ) == ZIP_END_RECORD ) {
				zip64 = FS_ZipLong( tail + at + 12 ) == 0xffffffff || FS_ZipLong( tail + at + 16 ) == 0xffffffff
					|| ( at >= ZIP64_LOCATOR_SIZE && FS_ZipLong( tail + at - ZIP64_LOCATOR_SIZE ) == ZIP64_LOCATOR );
				break;
			}
		}
	}
	Z_Free( tail );
	fclose( f );
	return zip64;
}

#define ADD_PAK_ZIP64	"it's saved as zip64, which the game can't read; zip it again without zip64"

// a warning content add's check found, in its words, while there's room
static void FS_AddPakWarning( extContentVerdict_t *verdict, const char *code, const char *text ) {
	if ( verdict && verdict->numWarnings < EXT_CONTENT_MAX_WARNINGS ) {
		verdict->warnings[verdict->numWarnings].code = code;
		Q_strncpyz( verdict->warnings[verdict->numWarnings].message, text, sizeof( verdict->warnings[0].message ) );
		verdict->numWarnings++;
	}
}

/*
=================
FS_AddPakInstalled

The file of a pak of the search path named gameDir/name.pk3, in any of the
game's directories (the base path's too), one whose checksum isn't
checksum first, with its checksum in installed; NULL for none. Players
know a pak by its name, so content add takes no other contents under it
=================
*/
const char *FS_AddPakInstalled( const char *gameDir, const char *name, unsigned int checksum,
	unsigned int *installed ) {
	const searchpath_t	*search;
	const pack_t		*found = NULL;

	for ( search = fs_searchpaths; search; search = search->next ) {
		const pack_t	*other = search->pack;

		if ( other && !Q_stricmp( other->pakGamename, gameDir ) && !Q_stricmp( other->pakBasename, name )
			&& ( !found || found->checksum == checksum ) ) {
			found = other;
		}
	}
	if ( !found ) {
		return NULL;
	}
	*installed = found->checksum;
	return found->pakFilename;
}

/*
=================
FS_CheckAddPak

What content add checks of a pak an operator installs as gameDir/name.pk3:
a zip that loads as a pak, not zip64, none of whose entries could reach
outside the game's directories, is encrypted or is native code, by its
name or its first bytes, within CONTENT_MAX_PAK_SIZE unpacked; and not one
of the game's own. NULL, with the pak's checksum, or why it's refused. In
overrides, the paks of the search path it would take code (a QVM in vm/),
more than PAK_ADD_OVERRIDES files or a shader from, where its copy of a
shader file leaves some out, and of a map pack where its files count, said
as a warning, or ""; with overrides NULL, none of that is looked at. In
verdict, when it's given, the refusal's code, and with overrides each of
those warnings with its code
=================
*/
const char *FS_CheckAddPak( const char *ospath, const char *gameDir, const char *name,
	unsigned int *checksum, char *overrides, int overridesSize, extContentVerdict_t *verdict ) {
	pack_t			*pak;
	const searchpath_t	*search;
	searchpath_t	added;
	unz_global_info	gi;
	unz_file_info	info;
	char			entry[MAX_ZPATH], retail[MAX_QPATH], clause[MAX_STRING_CHARS];
	const char		*why = NULL, *refused = NULL, *code, *named;
	byte			first[4];
	uint64_t		unpacked = 0;
	unsigned int	installed;
	int				i, got, count, dropped;
	qboolean		zip64;

	if ( overrides ) {
		overrides[0] = '\0';
		if ( verdict ) {
			verdict->numWarnings = 0;
		}
	}
	// zip64's records, which unzip can't read, keep a pak from loading or
	// hide its directory
	pak = FS_LoadZipFile( ospath, name, "", gameDir );
	if ( !pak ) {
		zip64 = FS_Zip64Records( ospath );
		if ( verdict ) {
			verdict->code = zip64 ? "zip64" : "damaged";
		}
		return zip64 ? ADD_PAK_ZIP64 : "it isn't a zip that loads as a pak";
	}
	// FS_LoadZipFile keeps the entries before a record it can't read
	if ( unzGetGlobalInfo( pak->handle, &gi ) != UNZ_OK || pak->numfiles < gi.number_entry ) {
		zip64 = FS_Zip64Records( ospath );
		refused = zip64 ? "zip64" : "damaged";
		why = zip64 ? ADD_PAK_ZIP64 : "its directory can't be read";
	}
	unzGoToFirstFile( pak->handle );
	for ( i = 0; !why && i < pak->numfiles; i++, unzGoToNextFile( pak->handle ) ) {
		if ( unzGetCurrentFileInfo( pak->handle, &info, entry, sizeof( entry ), NULL, 0, NULL, 0 ) != UNZ_OK ) {
			refused = "damaged";
			why = "its directory can't be read";
			break;
		}
		// a name the buffer cuts short, whose end could be anything
		if ( info.size_filename >= sizeof( entry ) ) {
			entry[sizeof( entry ) - 1] = '\0';
			refused = "long_name";
			why = va( "%s... has a name longer than %d characters", entry, MAX_ZPATH - 1 );
			break;
		}
		if ( ( why = FS_AddPakEntryRefused( entry, &refused ) ) != NULL ) {
			why = va( "%s holds %s", entry, why );
			break;
		}
		if ( info.flag & 1 ) {
			refused = "encrypted";
			why = va( "%s is encrypted", entry );
			break;
		}
		unpacked += info.uncompressed_size;
		if ( unpacked > CONTENT_MAX_PAK_SIZE ) {
			refused = "unpacks_too_big";
			why = va( "it unpacks to more than %d MB", CONTENT_MAX_PAK_SIZE >> 20 );
			break;
		}
		if ( !info.uncompressed_size ) {
			continue;
		}
		// what the engine can't read, another unzip may: compressed some
		// other way, with zip64's sizes in its local header (which unzip
		// takes for other sizes than the directory's), or damaged
		if ( unzOpenCurrentFile( pak->handle ) == UNZ_OK ) {
			got = unzReadCurrentFile( pak->handle, first, sizeof( first ) );
			unzCloseCurrentFile( pak->handle );
		} else {
			got = -1;
		}
		if ( got < (int)MIN( info.uncompressed_size, sizeof( first ) ) ) {
			zip64 = info.version_needed == ZIP64_VERSION;
			if ( info.compression_method != 0 && info.compression_method != Z_DEFLATED ) {
				refused = "method";
				zip64 = qfalse;
			} else {
				refused = zip64 ? "zip64" : "damaged";
			}
			why = zip64 ? ADD_PAK_ZIP64 : va( "%s can't be read", entry );
			break;
		}
		if ( FS_NativeMagic( first, got ) ) {
			refused = "native_code";
			why = va( "%s is native code", entry );
			break;
		}
	}
	*checksum = pak->checksum;
	if ( !why && FS_RetailPakChecksum( va( "%s/%s.pk3", gameDir, name ), pak->checksum, retail, sizeof( retail ) ) ) {
		refused = "retail";
		why = va( ADD_PAK_RETAIL, retail );
	}
	// a pak of its name in another of the game's directories, as the base
	// path's, with other contents: players know a pak by its name
	if ( !why && ( named = FS_AddPakInstalled( gameDir, name, pak->checksum, &installed ) ) != NULL
		&& installed != pak->checksum ) {
		refused = "other_contents";
		why = va( ADD_PAK_OTHER_CONTENTS, gameDir, name, named );
	}
	if ( verdict && why ) {
		verdict->code = refused;
	}

	// the paks it would override: the game's paks before it by name, and
	// those of the base game under a mod; its shader files read as an
	// entry of the search path's
	Com_Memset( &added, 0, sizeof( added ) );
	added.pack = pak;
	for ( search = fs_searchpaths; search && overrides && !why; search = search->next ) {
		const pack_t	*other = search->pack;

		if ( !other || !( ( !Q_stricmp( other->pakGamename, gameDir ) && Q_stricmp( other->pakBasename, name ) < 0 )
			|| ( Q_stricmp( gameDir, com_basegame->string ) && !Q_stricmp( other->pakGamename, com_basegame->string ) ) ) ) {
			continue;
		}
		// the files they share, looked up from the one with fewer, by the
		// new pak's names, which have no backslashes; not folders
		count = dropped = 0;
		code = NULL;
		for ( i = 0; i < MIN( pak->numfiles, other->numfiles ); i++ ) {
			const fileInPack_t	*ours, *theirs;
			const char			*file;

			if ( pak->numfiles <= other->numfiles ) {
				ours = &pak->buildBuffer[i];
				theirs = FS_PakEntry( other, ours->name );
			} else {
				theirs = &other->buildBuffer[i];
				ours = FS_PakEntry( pak, theirs->name );
			}
			if ( !ours || !theirs || !ours->name[0] || ours->name[strlen( ours->name ) - 1] == '/' ) {
				continue;
			}
			file = ours->name;
			count++;
			if ( !code && !Q_strncmp( file, "vm/", 3 ) && COM_CompareExtension( file, ".qvm" ) ) {
				code = file;
			}
			// a copy the same as the other's, by its CRC, leaves nothing out
			if ( FS_IsShaderFile( file ) && ours->crc != theirs->crc ) {
				dropped += FS_DroppedShaders( &added, search, file, qfalse );
			}
		}
		if ( code || dropped || count > PAK_ADD_OVERRIDES ) {
			Com_sprintf( clause, sizeof( clause ), "%s.pk3 overrides %d file%s of %s/%s.pk3%s%s", name, count,
				count == 1 ? "" : "s", other->pakGamename, other->pakBasename, code ? ", including " : "", code ? code : "" );
			if ( dropped ) {
				Q_strcat( clause, sizeof( clause ), va( ", and its shader files leave out %d of that pak's shaders",
					dropped ) );
			}
			Q_strcat( overrides, overridesSize, va( "%s%s", overrides[0] ? "; " : "", clause ) );
			// each thing the clause warns of, in its words
			if ( code ) {
				FS_AddPakWarning( verdict, "overrides_game_code", clause );
			}
			if ( count > PAK_ADD_OVERRIDES ) {
				FS_AddPakWarning( verdict, "overrides_many", clause );
			}
			if ( dropped ) {
				FS_AddPakWarning( verdict, "drops_shaders", clause );
			}
		}
	}
	// a map pack's count only on its own maps, from the next map
	if ( overrides && overrides[0] && FS_HoldsMapOnly( &added ) ) {
		Com_sprintf( clause, sizeof( clause ), "as a map pack, its files count %s", FS_PakConflictsWhere( &added ) );
		Q_strcat( overrides, overridesSize, va( "; %s", clause ) );
		FS_AddPakWarning( verdict, "map_pack_scope", clause );
	}
	FS_FreePak( pak );
	return why;
}

#define PAK_ARENA_TEXT	8192	// the size of an .arena the game skips (bg_public.h's MAX_ARENAS_TEXT)

// an arena of a pak outside the search path: its map and longname
typedef struct {
	char	map[MAX_QPATH], longname[256];
} pakArena_t;

/*
=================
FS_PakMaps

Each map of a pak at ospath, outside the search path, to fn: maps/<name>.bsp
by its name, with the longname of the first arena of the pak's own
scripts/<name>.arena files that names it, or "". The game reads an .arena
that way (G_LoadArenas): one of PAK_ARENA_TEXT bytes or more not at all,
and none past a token outside a block. A file that doesn't load as a pak
has none
=================
*/
void FS_PakMaps( const char *ospath, void ( *fn )( void *context, const char *map, const char *title ),
	void *context ) {
	searchpath_t		search;
	const fileInPack_t	*entry;
	pakArena_t			*arenas = NULL, *arena, *grown;
	char				*text, *p, *token, key[MAX_TOKEN_CHARS], map[MAX_QPATH];
	const char			*title;
	int					i, a, numArenas = 0, room = 0;

	Com_Memset( &search, 0, sizeof( search ) );
	if ( !( search.pack = FS_LoadZipFile( ospath, "", "", "" ) ) ) {
		return;
	}
	for ( i = 0; i < search.pack->numfiles; i++ ) {
		entry = &search.pack->buildBuffer[i];
		if ( Q_stricmpn( entry->name, "scripts/", 8 ) || strchr( entry->name + 8, '/' )
			|| !COM_CompareExtension( entry->name, ".arena" ) || entry->len >= PAK_ARENA_TEXT
			|| FS_ReadFileDir( entry->name, &search, qtrue, (void **)&text ) < 0 ) {
			continue;
		}
		for ( p = text; ( token = COM_ParseExt( &p, qtrue ) )[0] && !strcmp( token, "{" ); ) {
			if ( numArenas == room ) {
				if ( !( grown = realloc( arenas, ( room + 64 ) * sizeof( *arenas ) ) ) ) {
					break;
				}
				arenas = grown;
				room += 64;
			}
			arena = &arenas[numArenas];
			arena->map[0] = arena->longname[0] = '\0';
			while ( ( token = COM_ParseExt( &p, qtrue ) )[0] && strcmp( token, "}" ) ) {
				Q_strncpyz( key, token, sizeof( key ) );
				token = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( key, "map" ) ) {
					Q_strncpyz( arena->map, token, sizeof( arena->map ) );
				} else if ( !Q_stricmp( key, "longname" ) ) {
					Q_strncpyz( arena->longname, token, sizeof( arena->longname ) );
				}
			}
			numArenas += arena->map[0] != '\0';
		}
		FS_FreeFile( text );
	}

	for ( i = 0; i < search.pack->numfiles; i++ ) {
		// a name the map command can't take whole isn't one it loads, nor
		// an empty one (maps/.bsp)
		if ( !FS_IsMapFile( search.pack->buildBuffer[i].name )
			|| strlen( search.pack->buildBuffer[i].name + 5 ) - 5 >= sizeof( map ) - 1 ) {
			continue;
		}
		COM_StripExtension( search.pack->buildBuffer[i].name + 5, map, sizeof( map ) );
		for ( a = 0, title = ""; a < numArenas; a++ ) {
			if ( !Q_stricmp( arenas[a].map, map ) ) {
				title = arenas[a].longname;
				break;
			}
		}
		fn( context, map, title );
	}
	free( arenas );
	FS_FreePak( search.pack );
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

	thepak = FS_LoadZipFile(zipfile, "", "", "");

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
===============
FS_ListFilteredFiles

Returns a uniqued list of files that match the given criteria
from all search paths
===============
*/
char **FS_ListFilteredFiles( const char *path, const char *extension,
	char *filter, int *numfiles, qboolean stripPath,
	qboolean allowNonPureFilesOnDisk ) {
	fileList_t		list = { 0 };
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
	FS_ReturnPath(path, zpath, &pathDepth);

	//
	// search through the path, one element at a time, adding to list
	//
	for (search = FS_NextSearch(NULL) ; search ; search = FS_NextSearch(search)) {
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
					FileList_Add( &list, name + pathSkip, qtrue );
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
					FileList_Add( &list, name + pathSkip, qtrue );
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
					FileList_Add( &list, name, qtrue );
				}
				Sys_FreeFileList( sysFiles );
			}
		}
	}

	return FileList_Done( &list, numfiles );
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
FS_WarnListCut

Warns, once for each list in each game directory, that a caller's buffer
held only the first names of a list: the game's lists of maps, bots and
models otherwise come up short without a word
================
*/
static void FS_WarnListCut( const char *path, const char *extension, int listed, int total, int bufsize ) {
	static char	warned[64][MAX_OSPATH];
	static int	numWarned;
	char		key[MAX_OSPATH];
	const char	*list;
	int			i;

	// a VM may pass no extension, which FS_ListFiles takes as ""
	if ( !extension ) {
		extension = "";
	}
	list = *path ? va( "%s/*%s", path, extension ) : va( "*%s", extension );
	// not by the buffer's size, which a list of demos leaves to what's left
	Q_strncpyz( key, va( "%s %s", FS_GetCurrentGameDir(), list ), sizeof( key ) );
	for ( i = 0; i < numWarned; i++ ) {
		if ( !Q_stricmp( warned[i], key ) ) {
			return;
		}
	}
	// past the table, no more: a warning on every call would be noise
	if ( numWarned == (int)ARRAY_LEN( warned ) ) {
		return;
	}
	Q_strncpyz( warned[numWarned++], key, sizeof( warned[0] ) );
	if ( total < 0 ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: the list of %s holds only the first %i, in %i bytes\n",
			path, listed, bufsize );
	} else {
		Com_Printf( S_COLOR_YELLOW "WARNING: the list of %s holds %i of %i names, in %i bytes\n",
			list, listed, total, bufsize );
	}
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
			FS_WarnListCut( path, extension, i, nFiles, bufsize );
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
	char			descPath[MAX_OSPATH];
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
		// and a folder named past MAX_QPATH, which fs_game can't be set to
		if (strlen(name) >= MAX_QPATH) {
			Com_Printf("Mods: %s's name is longer than %d characters, so it isn't listed\n", name, MAX_QPATH - 1);
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
				FS_WarnListCut( "mods", "", nMods, -1, bufsize );
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

static int QDECL paksort( const void *a, const void *b );

/*
================
FS_SortFileList

The lists sorted here are unique already, so the sort's stability doesn't
matter
================
*/
void FS_SortFileList(char **filelist, int numfiles) {
	// an empty list is NULL, which qsort mustn't be given
	if ( numfiles > 1 ) {
		qsort( filelist, numfiles, sizeof( *filelist ), paksort );
	}
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
FS_Lookup_f

fs_lookup <name> [count]: where a read of the name comes from, through the
index and by the walk, and how long a lookup takes each way, as a mean of
count (1000) lookups of whether it's there, timed without fs_indexCheck's
walk
============
*/
static void FS_Lookup_f( void ) {
	const searchpath_t	*search;
	const char			*name = Cmd_Argv( 1 );
	int					i, n, count, start, msec[2];

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: fs_lookup <name> [count]\n" );
		return;
	}
	count = Cmd_Argc() > 2 ? MAX( atoi( Cmd_Argv( 2 ) ), 1 ) : 1000;
	search = FS_WalkFinds( name, qtrue, qfalse, FS_IsLocalConfig( name ) );
	Com_Printf( "%s: %s\n", name, search ? FS_SearchSource( search ) : "not found" );
	for ( i = 0; i < 2; i++ ) {
		fs_lookupTimed = i + 1;
		start = Sys_Milliseconds();
		for ( n = 0; n < count; n++ ) {
			FS_FOpenFileRead( name, NULL, qfalse );
		}
		msec[i] = Sys_Milliseconds() - start;
	}
	fs_lookupTimed = 0;
	Com_Printf( "%d lookups: %.3f msec each through the index, %.3f by the walk; %d lookups so far of a name in "
		"more than %d paks, walked\n", count, msec[0] / (float)count, msec[1] / (float)count, fs_indexWalks, INDEX_MAX_PAKS );
}

/*
============
FS_Path_f

============
*/

static qboolean FS_IdPak( const pack_t *pak );
static qboolean FS_GlobalPak( const searchpath_t *search );

/*
================
FS_EntryGame

A search path entry's game directory: a pak's or pk3dir's, or the
directory itself
================
*/
static const char *FS_EntryGame( const searchpath_t *search ) {
	const char	*game, *c;

	if ( search->pack ) {
		return search->pack->pakGamename;
	}
	if ( !FS_IsExt( search->dir->gamedir, ".pk3dir", strlen( search->dir->gamedir ) ) ) {
		return search->dir->gamedir;
	}
	// a pk3dir's, the last in its path
	for ( game = c = search->dir->path; *c; c++ ) {
		if ( *c == '/' || *c == '\\' ) {
			game = c + 1;
		}
	}
	return game;
}

/*
================
FS_EntryFolder

The folder a search path entry is in, a game directory under a root
================
*/
static const char *FS_EntryFolder( const searchpath_t *search ) {
	if ( search->pack ) {
		return search->pack->pakPathname;
	}
	return FS_IsGameContent( search ) ? search->dir->path : search->dir->fullpath;
}

/*
================
FS_SearchSource

A search path entry, as path and which name it: a pak's file, or a
directory's path; "" for none
================
*/
static const char *FS_SearchSource( const searchpath_t *search ) {
	return !search ? "" : search->pack ? search->pack->pakFilename : search->dir->fullpath;
}

// where a pak or pk3dir with maps and no game code counts (FS_MapPackPlace)
typedef enum {
	MAPPACK_NONE,		// it isn't one: id's, or has game code or no map
	MAPPACK_OWN,		// the current map's own
	MAPPACK_SET_ASIDE,	// one the current map doesn't use
	MAPPACK_GLOBAL,		// named by fs_globalPaks, so on every map
	MAPPACK_UNSCOPED	// fs_mapPakScope 0, so on every map
} mapPackPlace_t;

static mapPackPlace_t FS_MapPackPlace( const searchpath_t *search ) {
	if ( !FS_HoldsMapOnly( search ) ) {
		return MAPPACK_NONE;
	}
	// set aside by this map's ranking, though the settings since say
	// otherwise from the next
	if ( search->tier ) {
		return MAPPACK_SET_ASIDE;
	}
	if ( FS_GlobalPak( search ) ) {
		return MAPPACK_GLOBAL;
	}
	return fs_mapPakScope->integer ? MAPPACK_OWN : MAPPACK_UNSCOPED;
}

/*
================
FS_EntryKind

What path says a pak or pk3dir is, and so where its files count; NULL for
a game directory
================
*/
static const char *FS_EntryKind( const searchpath_t *search ) {
	if ( search->pack && FS_IdPak( search->pack ) ) {
		return "id's";
	}
	switch ( FS_MapPackPlace( search ) ) {
	case MAPPACK_OWN: return "a map pack, this map's own";
	// with no map yet, as at the first start, every map pack waits, as
	// FS_LostReason says
	case MAPPACK_SET_ASIDE: return fs_currentMap[0] ? "set aside: a map pack this map doesn't use" :
		"set aside: a map pack, and there's no map";
	case MAPPACK_GLOBAL: return "a map pack fs_globalPaks counts on every map";
	case MAPPACK_UNSCOPED: return "a map pack, counted on every map with fs_mapPakScope 0";
	default: break;
	}
	if ( !FS_IsGameContent( search ) ) {
		return NULL;
	}
	if ( Q_stricmp( FS_EntryGame( search ), com_basegame->string ) ) {
		return "the mod's";
	}
	return ( search->pack ? search->pack->holdsMap : search->dir->holdsMap ) ?
		"with game code, counted on every map" : "without maps, counted on every map";
}

/*
================
FS_LostReason

Why a read takes one copy of a file before another (which)
================
*/
static const char *FS_LostReason( const searchpath_t *loser, const searchpath_t *winner, const char *filename ) {
	const char	*game = FS_EntryGame( winner );

	if ( loser->pack && !FS_PakIsPure( loser->pack ) ) {
		return "not on the pure server's list";
	}
	if ( loser->dir && FS_PureRefusesDirFile( filename ) ) {
		return "a pure server reads it only from its paks";
	}
	if ( loser->tier > winner->tier ) {
		if ( !fs_currentMap[0] ) {
			return "set aside: a map pack, and there's no map";
		}
		if ( !fs_mapSearch || !fs_mapSearch->pack ) {
			return va( "set aside: a map pack, and %s isn't in one", fs_currentMap );
		}
		return va( "set aside: a map pack, and %s is in %s.pk3", fs_currentMap, fs_mapSearch->pack->pakBasename );
	}
	// both on the pure server's list, which it reordered (FS_ReorderPurePaks)
	if ( fs_reordered && loser->pack && winner->pack ) {
		return "later in the pure server's list";
	}
	// a download takes the home folder's name (FS_AddCachePaks), but not
	// its place
	if ( loser->pack && loser->pack->cached && !( winner->pack && winner->pack->cached ) &&
		!Q_stricmp( FS_EntryGame( loser ), game ) ) {
		return "a download, below the folder's installed paks";
	}
	if ( !Q_stricmp( FS_EntryFolder( loser ), FS_EntryFolder( winner ) ) ) {
		return FS_IsGameContent( winner ) ? "a later name in the same folder" : "the folder's loose files come before its paks";
	}
	if ( Q_stricmp( FS_EntryGame( loser ), game ) ) {
		return va( "below the %s folder%s", game, Q_stricmp( game, com_basegame->string ) ? ", the mod's" : "" );
	}
	return va( "%s is searched first", FS_EntryFolder( winner ) );
}

void FS_Path_f( void ) {
	searchpath_t	*s;
	int				i;
	const char		*kind;

	Com_Printf ("We are looking in the current search path:\n");
	if ( fs_mapSearch ) {
		Com_Printf( "The map is %s, from %s\n", fs_currentMap,
			fs_mapSearch->pack ? fs_mapSearch->pack->pakFilename : fs_mapSearch->dir->fullpath );
	} else if ( fs_currentMap[0] ) {
		Com_Printf( "The map is %s\n", fs_currentMap );
	}
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
		if ( ( kind = FS_EntryKind( s ) ) != NULL ) {
			Com_Printf( "    %s\n", kind );
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
	const searchpath_t	*search, *winner;
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

	// the copy a read takes, then every other and why it loses
	winner = FS_FileSearch( filename );
	if ( !winner ) {
		Com_Printf( "File not found: \"%s\"\n", filename );
	} else {
		FS_Which( filename, (void *)winner );
	}
	for ( search = FS_NextSearch( NULL ); search; search = FS_NextSearch( search ) ) {
		if ( search != winner && FS_FOpenFileReadDir( filename, (searchpath_t *)search, NULL, qfalse, qfalse ) > 0 ) {
			Com_Printf( "    also in \"%s\": %s\n", FS_SearchSource( search ),
				winner ? FS_LostReason( search, winner, filename ) : "not on the pure server's list" );
		}
	}
}


//===========================================================================


/*
================
FS_DirHolds

Whether a directory's subdirectory holds a file of an extension
================
*/
static qboolean FS_DirHolds( const char *path, const char *subdir, const char *extension ) {
	char	**files;
	int		count;

	files = Sys_ListFiles( va( "%s%c%s", path, PATH_SEP, subdir ), extension, NULL, &count, qfalse );
	Sys_FreeFileList( files );
	return count > 0;
}

static int QDECL paksort( const void *a, const void *b ) {
	char	*aa, *bb;

	aa = *(char **)a;
	bb = *(char **)b;

	return FS_PathCmp( aa, bb );
}

#ifndef STANDALONE
/*
================
FS_IdPakIndex

One of id's paks' numbers by its name, pak0 to pak<count - 1>; -1 for
none
================
*/
static int FS_IdPakIndex( const char *basename, int count ) {
	if ( strlen( basename ) != 4 || Q_stricmpn( basename, "pak", 3 ) ||
		basename[3] < '0' || basename[3] > '0' + count - 1 ) {
		return -1;
	}
	return basename[3] - '0';
}
#endif

/*
================
FS_PointReleasePak

The number of the full game's point release a pak is, pak1 to pak8 by
checksum, whatever its name, as sv_refuseRetailPaks knows it; or the
tests' stand-in for one (FS_TestPakChecksum), by its name; 0 for none
================
*/
static int FS_PointReleasePak( const pack_t *pak ) {
#ifndef STANDALONE
	const char	*game;
	int			index;

	if ( FS_TestPakChecksum( pak->checksum ) ) {
		return MAX( FS_IdPakIndex( pak->pakBasename, NUM_ID_PAKS ), 0 );
	}
	game = FS_RetailPakGame( pak->checksum, &index );
	if ( game && !Q_stricmp( game, BASEGAME ) && index > 0 ) {
		return index;
	}
#endif
	return 0;
}

// what FS_AddGameDirectory adds of a directory
typedef enum {
	FS_ADD_ALL,		// the directory and its paks
	FS_ADD_PAKS,	// its paks alone (FS_PathVarPaksOnly)
	FS_ADD_DIR		// the directory alone: a dedicated server's folder on the demo
} fsAdd_t;

/*
================
FS_AddGameDirectory

Sets fs_gamedir, adds the directory to the head of the path,
then loads the zip headers
================
*/
static void FS_AddGameDirectory( const char *path, const char *dir, fsAdd_t what ) {
	const qboolean	paksOnly = what == FS_ADD_PAKS;
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
	int				index;

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
	if ( what == FS_ADD_DIR ) {
		numfiles = 0;
		pakfiles = NULL;
	} else {
		pakfiles = Sys_ListFiles(curpath, ".pk3", NULL, &numfiles, qfalse);
	}

	if ( pakfiles ) {
		qsort( pakfiles, numfiles, sizeof(char*), paksort );
	}

	if ( fs_numServerPaks || what != FS_ADD_ALL ) {
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
			if ((pak = FS_LoadZipFile(pakfile, pakfiles[pakfilesi], curpath, dir)) == 0) {
				// This isn't a .pk3, or it couldn't be opened! Next!
				Com_Printf( S_COLOR_YELLOW "WARNING: couldn't open %s as a pak, so it's left out\n", pakfile );
				pakfilesi++;
				continue;
			}
			// on the demo played from baseq3, the full game's point release
			// there, which the demo's players don't have and can't download
			if ( fs_demo == FS_DEMO_IN_BASE && !Q_stricmp( dir, BASEGAME ) && ( index = FS_PointReleasePak( pak ) ) ) {
				// once a run: the search path is built again at each map
				if ( !( fs_leftOutSaid & ( 1u << index ) ) ) {
					Com_Printf( "%s is the full game's point release, which the demo's players can't have, so "
						"it's left out while the demo plays\n", pakfile );
					fs_leftOutSaid |= 1u << index;
				}
				fs_leftOutPaks |= 1u << index;
				FS_FreePak( pak );
				pakfilesi++;
				continue;
			}

			// store the game name for downloading

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
			search->dir->holdsMap = FS_DirHolds( pakfile, "maps", ".bsp" );
			search->dir->holdsCode = FS_DirHolds( pakfile, "vm", ".qvm" );

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
FS_PlayOwnVersions

Plays a non-pure server's paks, by a download's name ("<game>/<name>.pk3")
and the checksum the server lists, with the player's own versions of them:
the server couldn't send them, and a non-pure server takes any. They then
aren't asked for (FS_ServerPakMissing), so a later gamestate doesn't fetch
them again, until FS_ForgetOwnVersions. All of them or none: none on a
server that lists its paks, which takes only those (FS_OwnVersionPlayed),
or past MAX_OWN_VERSIONS. Returns whether they were kept
================
*/
qboolean FS_PlayOwnVersions( const missingPak_t *paks, int count )
{
	int		i;

	if ( fs_numServerPaks || count > MAX_OWN_VERSIONS - fs_numOwnVersions ) {
		return qfalse;
	}
	for ( i = 0; i < count; i++ ) {
		COM_StripExtension( paks[i].name, fs_ownVersionNames[fs_numOwnVersions], sizeof( fs_ownVersionNames[0] ) );
		fs_ownVersions[fs_numOwnVersions++] = paks[i].checksum;
	}
	return qtrue;
}

/*
================
FS_ForgetOwnVersions

For another server: the paks played with the player's own versions are
asked for again
================
*/
void FS_ForgetOwnVersions( void )
{
	fs_numOwnVersions = 0;
}

/*
================
FS_OwnVersionPlayed

Whether a non-pure server's pak refName ("<game>/<name>") with the
checksum is played with the player's own version (FS_PlayOwnVersions);
never on a pure server, which takes only its own
================
*/
static qboolean FS_OwnVersionPlayed( const char *refName, unsigned int checksum )
{
	int		i;

	if ( fs_numServerPaks ) {
		return qfalse;
	}
	for ( i = 0; i < fs_numOwnVersions; i++ ) {
		if ( fs_ownVersions[i] == checksum && !FS_FilenameCompare( fs_ownVersionNames[i], refName ) ) {
			return qtrue;
		}
	}
	return qfalse;
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
	char				ospath[MAX_OSPATH], cachePath[MAX_OSPATH];
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
		// in the game directory's place, where a download used to go, so a
		// QVM is looked for in one pak a directory (FS_FindVM)
		Q_strncpyz( cachePath, FS_BaseDir_BuildOSPath( fs_homedatapath->string, dir ), sizeof( cachePath ) );
		while ( ( entry = hash ? Content_Cache_FindSHA256( hash, ospath, sizeof( ospath ) ) :
			Content_Cache_Find( fs_serverReferencedPaks[i], dir, ospath, sizeof( ospath ) ) ) >= 0 ) {
			pak = FS_LoadZipFile( ospath, slash + 1, cachePath, dir );
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
FS_IdPak

Whether a pak is one of id's, by its checksum: Quake III Arena 1.32's,
Team Arena's or the demo's
================
*/
static qboolean FS_IdPak( const pack_t *pak ) {
	int		index;

#ifndef STANDALONE
	if ( pak->checksum == DEMO_PAK0_CHECKSUM ) {
		return qtrue;
	}
#endif
	return FS_RetailPakGame( pak->checksum, &index ) != NULL;
}

/*
================
FS_NextListedName

The next name in a list of paks separated by spaces, fs_globalPaks' or
fs_excludeReference's, into token without its .pk3 or .pk3dir, moving
list past it; qfalse at the list's end
================
*/
static qboolean FS_NextListedName( const char **list, char *token, int size ) {
	char	*end;
	int		length;

	while ( **list == ' ' ) {
		( *list )++;
	}
	if ( !**list ) {
		return qfalse;
	}
	for ( length = 0; ( *list )[length] && ( *list )[length] != ' '; length++ ) {
	}
	Q_strncpyz( token, *list, MIN( length + 1, size ) );
	*list += length;
	if ( ( end = strrchr( token, '.' ) ) && ( !Q_stricmp( end, ".pk3" ) || !Q_stricmp( end, ".pk3dir" ) ) ) {
		*end = '\0';
	}
	return qtrue;
}

/*
================
FS_GlobalPak

Whether fs_globalPaks names a pak or pk3dir: a name, with or without its
.pk3, for one in any game directory, or "<game>/<name>" for one in that
directory, in any case
================
*/
static qboolean FS_GlobalPak( const searchpath_t *search ) {
	const char	*list = fs_globalPaks->string, *game;
	char		name[MAX_OSPATH], full[MAX_OSPATH], token[MAX_OSPATH];

	game = FS_EntryGame( search );
	if ( search->pack ) {
		Q_strncpyz( name, search->pack->pakBasename, sizeof( name ) );
	} else {
		COM_StripExtension( search->dir->gamedir, name, sizeof( name ) );
	}
	Com_sprintf( full, sizeof( full ), "%s/%s", game, name );

	while ( FS_NextListedName( &list, token, sizeof( token ) ) ) {
		if ( token[0] && !FS_FilenameCompare( strchr( token, '/' ) ? full : name, token ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
================
FS_MapPack

Whether a search path entry is a map pack: a pak or pk3dir that holds a
map, maps/<name>.bsp, but no game code, vm/<name>.qvm, and isn't one of
id's (FS_HoldsMapOnly), or one fs_globalPaks names
================
*/
static qboolean FS_HoldsMapOnly( const searchpath_t *search ) {
	if ( search->pack ) {
		return search->pack->holdsMap && !search->pack->holdsCode && !FS_IdPak( search->pack );
	}
	return search->dir->holdsMap && !search->dir->holdsCode;
}

static qboolean FS_MapPack( const searchpath_t *search ) {
	return FS_HoldsMapOnly( search ) && !FS_GlobalPak( search );
}

/*
================
FS_PathSignature

A hash of the search path's entries, in order: each pak's path,
checksum and tier, each directory's path and tier
================
*/
static unsigned FS_PathSignature( void ) {
	const searchpath_t	*search;
	const char			*c;
	unsigned			hash = 5381;

	for ( search = fs_searchpaths; search; search = search->next ) {
		for ( c = search->pack ? search->pack->pakFilename : search->dir->fullpath; *c; c++ ) {
			hash = hash * 33 + (unsigned char)*c;
		}
		hash = ( hash * 33 + ( search->pack ? search->pack->checksum : 0 ) ) * 33 + search->tier;
	}
	return hash;
}

/*
================
FS_RankPaks

Puts the map packs the current map doesn't use in tier 1, so they only
fill in files no other entry has, and every other entry in tier 0, as
they were: with fs_mapPakScope 0, every entry. The map's own pak is the
one its BSP loads from, the first in the path's order, as a read finds it
in tier 0 too, with any pak of the same content. A change to
fs_mapPakScope or fs_globalPaks applies here, at the next map or
restart of the file system
================
*/
static void FS_RankPaks( void ) {
	searchpath_t	*search;
	const pack_t	*own;
	char			bsp[MAX_QPATH];
	unsigned		signature;

	fs_mapSearch = NULL;
	if ( fs_currentMap[0] ) {
		Com_sprintf( bsp, sizeof( bsp ), "maps/%s.bsp", fs_currentMap );
		for ( search = fs_searchpaths; search && !fs_mapSearch; search = search->next ) {
			if ( FS_SearchHasFile( search, bsp ) ) {
				fs_mapSearch = search;
			}
		}
	}
	own = fs_mapSearch ? fs_mapSearch->pack : NULL;

	fs_lastTier = 0;
	for ( search = fs_searchpaths; search; search = search->next ) {
		search->tier = fs_mapPakScope->integer && search != fs_mapSearch &&
			!( own && search->pack && search->pack->checksum == own->checksum ) && FS_MapPack( search );
		fs_lastTier = MAX( fs_lastTier, search->tier );
	}

	// a path built or ranked otherwise may have moved what a read finds
	signature = FS_PathSignature();
	fs_rankings += signature != fs_pathSignature;
	fs_pathSignature = signature;
	FS_IndexOrder();
}

/*
================
FS_Ranking

How many times the copy a read takes may have moved, as the search path
was built otherwise or reordered, an entry's tier changed, or a pure
server's pak list changed: a cache of what it read (the sound caches)
checks where a file comes from again once it's changed, as a map's own
pak replaces a file only on its map
================
*/
int FS_Ranking( void ) {
	return fs_rankings;
}

/*
===========
FS_FileSource

Where a read of a file would come from now: its pak's or directory's
path, "" for none. Marks no pak referenced
===========
*/
const char *FS_FileSource( const char *filename ) {
	return FS_SearchSource( FS_FileSearch( filename ) );
}

/*
===========
FS_LastFileSource

FS_FileSource's answer for the last file FS_FOpenFileRead found, without
looking again
===========
*/
const char *FS_LastFileSource( void ) {
	return FS_SearchSource( fs_lastFileSearch );
}

/*
================
FS_SetCurrentMap

The map the server runs or the client plays, NULL or "" for none, whose
own pak leads while the other map packs are set aside (FS_RankPaks). It
mounts nothing, so the pure lists and checksums stay as they are
================
*/
void FS_SetCurrentMap( const char *map ) {
	FS_NameCurrentMap( map );
	if ( fs_searchpaths ) {
		FS_RankPaks();
	}
}

/*
================
FS_NameCurrentMap

The map, as FS_SetCurrentMap, before a restart of the file system, which
ranks the paks by it and so prints their list by it, without ranking the
path the restart then frees
================
*/
void FS_NameCurrentMap( const char *map ) {
	Q_strncpyz( fs_currentMap, map ? map : "", sizeof( fs_currentMap ) );
}


// id's paks in the path's order, at most this many (FS_IdPaks)
#define MAX_ID_PAKS	32

/*
================
FS_IdPaks

The search path's entries of id's paks, in its order
================
*/
static int FS_IdPaks( const searchpath_t **ids ) {
	const searchpath_t	*search;
	int					count = 0;

	for ( search = fs_searchpaths; search && count < MAX_ID_PAKS; search = search->next ) {
		if ( search->pack && FS_IdPak( search->pack ) ) {
			ids[count++] = search;
		}
	}
	return count;
}

/*
================
FS_IdCopy

The first of id's paks with a file, and its entry there; NULL for none
================
*/
static const searchpath_t *FS_IdCopy( const searchpath_t **ids, int numIds, const char *name,
	const fileInPack_t **entry ) {
	int		i;

	for ( i = 0; i < numIds; i++ ) {
		if ( ( *entry = FS_PakEntry( ids[i]->pack, name ) ) != NULL ) {
			return ids[i];
		}
	}
	return NULL;
}

/*
================
FS_ShaderNames

The shaders a shader file of a search path entry defines, at most max
================
*/
static int FS_ShaderNames( const searchpath_t *search, const char *name, char (*names)[MAX_QPATH], int max ) {
	char	*text, *p, *token;
	int		count = 0;

	if ( FS_ReadFileDir( name, (void *)search, qtrue, (void **)&text ) <= 0 ) {
		return 0;
	}
	for ( p = text; count < max && ( token = COM_NextShaderName( &p ) )[0]; ) {
		Q_strncpyz( names[count++], token, MAX_QPATH );
	}
	FS_FreeFile( text );
	return count;
}

/*
================
FS_DroppedShaders

Of the shaders another entry's copy of a shader file defines, how many a
pak's copy leaves out; with print, lists them
================
*/
static int FS_DroppedShaders( const searchpath_t *search, const searchpath_t *other, const char *name,
	qboolean print ) {
	enum { MAX_NAMES = 1024 };
	char	(*ours)[MAX_QPATH] = Z_Malloc( 2 * MAX_NAMES * MAX_QPATH ), (*theirs)[MAX_QPATH] = ours + MAX_NAMES;
	int		numOurs = FS_ShaderNames( search, name, ours, MAX_NAMES );
	int		numTheirs = FS_ShaderNames( other, name, theirs, MAX_NAMES ), dropped = 0, i, j;

	for ( i = 0; i < numTheirs; i++ ) {
		for ( j = 0; j < numOurs && Q_stricmp( theirs[i], ours[j] ); j++ ) {
		}
		if ( j == numOurs ) {
			if ( print ) {
				Com_Printf( "%s%s", dropped ? ", " : ", leaving out ", theirs[i] );
			}
			dropped++;
		}
	}
	Z_Free( ours );
	return dropped;
}

/*
================
FS_PakConflicts

How many of a pak's files one of id's paks also has, with other contents;
with print, lists them, and of a shader file the shaders it leaves out of
id's copy
================
*/
static int FS_PakConflicts( const searchpath_t *search, const searchpath_t **ids, int numIds, qboolean print ) {
	const pack_t		*pak = search->pack;
	const fileInPack_t	*id;
	const searchpath_t	*idSearch;
	int					count = 0, i;

	for ( i = 0; i < pak->numfiles; i++ ) {
		const fileInPack_t	*file = &pak->buildBuffer[i];

		if ( !file->len || !( idSearch = FS_IdCopy( ids, numIds, file->name, &id ) ) || id->crc == file->crc ) {
			continue;
		}
		count++;
		if ( print ) {
			Com_Printf( "    %s", file->name );
			if ( FS_IsShaderFile( file->name ) ) {
				FS_DroppedShaders( search, idSearch, file->name, qtrue );
			}
			Com_Printf( "\n" );
		}
	}
	return count;
}

/*
================
FS_PakConflictsWhere

Where an installed pak's copies of id's files count
================
*/
static const char *FS_PakConflictsWhere( const searchpath_t *search ) {
	static char	where[MAX_STRING_CHARS];
	const pack_t	*pak = search->pack;
	char		map[MAX_QPATH];
	int			i, maps = 0;

	switch ( FS_MapPackPlace( search ) ) {
	case MAPPACK_NONE: return "on every map";
	case MAPPACK_GLOBAL: return "on every map (fs_globalPaks)";
	case MAPPACK_UNSCOPED: return "on every map (fs_mapPakScope 0)";
	default: break;
	}
	Q_strncpyz( where, "only on its own maps:", sizeof( where ) );
	for ( i = 0; i < pak->numfiles; i++ ) {
		const char	*name = pak->buildBuffer[i].name;

		if ( !FS_IsMapFile( name ) ) {
			continue;
		}
		if ( maps++ < 8 ) {
			COM_StripExtension( name + 5, map, sizeof( map ) );
			Q_strcat( where, sizeof( where ), va( " %s", map ) );
		}
	}
	if ( maps > 8 ) {
		Q_strcat( where, sizeof( where ), va( " and %d more", maps - 8 ) );
	}
	return where;
}

/*
================
FS_PakConflicts_f

pakconflicts: each installed pak's copies of id's files that differ from
id's, and where they count
================
*/
static void FS_PakConflicts_f( void ) {
	const searchpath_t	*search, *ids[MAX_ID_PAKS];
	int					numIds = FS_IdPaks( ids ), count, paks = 0;

	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( !search->pack || search->pack->cached || FS_IdPak( search->pack ) ||
			!( count = FS_PakConflicts( search, ids, numIds, qfalse ) ) ) {
			continue;
		}
		Com_Printf( "%s: %d of id's files, %s\n", search->pack->pakFilename, count, FS_PakConflictsWhere( search ) );
		FS_PakConflicts( search, ids, numIds, qtrue );
		paks++;
	}
	if ( !paks ) {
		Com_Printf( "No installed pak has its own copy of one of id's files\n" );
	}
}

/*
================
FS_NoteMapPackConflicts

Says once, at the first start, how many installed map packs have their
own copies of id's files
================
*/
static void FS_NoteMapPackConflicts( void ) {
	static qboolean	noted;
	const searchpath_t	*search, *ids[MAX_ID_PAKS];
	int					numIds, count = 0;

	if ( noted ) {
		return;
	}
	noted = qtrue;
	numIds = FS_IdPaks( ids );
	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( search->pack && !search->pack->cached && FS_MapPack( search ) &&
			FS_PakConflicts( search, ids, numIds, qfalse ) ) {
			count++;
		}
	}
	if ( !count ) {
		return;
	}
	if ( fs_mapPakScope->integer ) {
		Com_Printf( "%d map pack%s copies of id's files; %s only on %s own maps (pakconflicts lists them)\n",
			count, count == 1 ? " carries" : "s carry", count == 1 ? "it applies" : "they apply", count == 1 ? "its" : "their" );
	} else {
		Com_Printf( "%d map pack%s copies of id's files, which with fs_mapPakScope 0 apply on every map (pakconflicts lists them)\n",
			count, count == 1 ? " carries" : "s carry" );
	}
}

/*
================
FS_AddGameDirectories
================
*/
static void FS_AddGameDirectories(const char *dir, qboolean dirOnly)
{
	const searchpath_t	*below = fs_searchpaths;

	// add search path elements in reverse priority order
	for(int i = ARRAY_LEN(fs_pathVars) - 1; i >= 0; i--) {
		const cvar_t *pathVar = fs_pathVars[i];
		const qboolean paksOnly = pathVar && FS_PathVarPaksOnly( pathVar );

		if (!pathVar || !pathVar->string[0] || ( dirOnly && paksOnly )) {
			continue;
		}

		searchpath_t *added = fs_searchpaths;
		FS_AddGameDirectory(pathVar->string, dir, dirOnly ? FS_ADD_DIR : paksOnly ? FS_ADD_PAKS : FS_ADD_ALL);
		if ( !dirOnly ) {
			FS_SetContentRoot( fs_searchpaths, added, pathVar );
		}
	}

	if ( !dirOnly ) {
		FS_RefuseHashedPaks( below );
		FS_AddCachePaks( dir, below );
	}
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
spaces; or a name past MAX_QPATH, which the game's paths can't hold
================
*/
qboolean FS_InvalidGameDir( const char *gamedir ) {
	char	name[MAX_QPATH];

	if ( strlen( gamedir ) >= MAX_QPATH ) {
		Com_Printf( "A game folder's name is at most %d characters: %s\n", MAX_QPATH - 1, gamedir );
		return qtrue;
	}
	Q_strncpyz( name, gamedir, sizeof( name ) );
	name[FS_NameLength( name, strlen( name ) )] = '\0';
	if ( gamedir[0] == '.' || strchr( gamedir, '/' ) || strchr( gamedir, '\\' ) || FS_EngineFolder( name ) ) {
		return qtrue;
	}

	return qfalse;
}

/*
================
FS_ServerPakMissing

Whether the server's referenced pak i is one to fetch: named, not in the
search path, and not one the player plays with their own version of
(FS_OwnVersionPlayed)
================
*/
static qboolean FS_ServerPakMissing( int i )
{
	const char		*name = fs_serverReferencedPakNames[i];
	unsigned int	checksum = fs_serverReferencedPaks[i];

	return name && *name && !FS_HasServerPak( name, checksum ) && !FS_OwnVersionPlayed( name, checksum );
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

		if ( FS_ServerPakMissing( i ) ) {
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
		if ( !fs_serverReferencedPakNames[i] || strlen( fs_serverReferencedPakNames[i] ) + 4 >= sizeof( paks->name ) ||
			!FS_ValidDownloadName( fs_serverReferencedPakNames[i] ) || !FS_ServerPakMissing( i ) ) {
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

	if ( fs_indexMismatches ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: fs index: %d lookups found apart from the walk\n", fs_indexMismatches );
		fs_indexMismatches = 0;
	}
	FS_IndexClear();

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
	fs_mapSearch = NULL;
	fs_lastTier = 0;

	Cmd_RemoveCommand( "path" );
	Cmd_RemoveCommand( "dir" );
	Cmd_RemoveCommand( "fdir" );
	Cmd_RemoveCommand( "touchFile" );
	Cmd_RemoveCommand( "which" );
	Cmd_RemoveCommand( "pakconflicts" );
	Cmd_RemoveCommand( "fs_lookup" );

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
	// the order a read takes may have changed (FS_Ranking)
	fs_rankings += fs_reordered;
	FS_IndexOrder();
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

	// what this start leaves out, said again by each
	fs_leftOutPaks = 0;

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
	fs_indexUse = Cvar_Get( "fs_index", "1", 0 );
	Cvar_SetDescription( fs_indexUse,
		"Whether file lookups use the name index of the search path's paks, rather than asking each pak in turn: 1 or 0, to compare" );
	fs_indexCheck = Cvar_Get( "fs_indexCheck", "0", 0 );
	Cvar_SetDescription( fs_indexCheck,
		"Whether each indexed file lookup is checked against asking every pak in turn: 0, 1 to say where they differ, 2 to stop at the first" );
	// the tests turn it on for a whole run
	if ( getenv( "OMNIFRAG_TEST_FS_INDEX_CHECK" ) ) {
		Cvar_Set( "fs_indexCheck", getenv( "OMNIFRAG_TEST_FS_INDEX_CHECK" ) );
	}
	fs_mapPakScope = Cvar_Get( "fs_mapPakScope", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( fs_mapPakScope, 0, 1, qtrue );
	Cvar_SetDescription( fs_mapPakScope,
		"Whether a map pack changes only its own maps\n"
		"A map pack, a pak file (.pk3) with maps and no game code, can carry its own copies of the game's "
		"files: shaders, textures, sounds, bot files. On, its copies apply only on its own maps, as their "
		"author tested them; on every other map it only fills in files nothing else has, so another map's "
		"look and sounds stay as they were made. Mods, id's paks and paks without maps are unchanged. "
		"Applies from the next map.\n"
		"0 = off: every pak counts on every map, as in Quake III\n"
		"1 = on" );
	fs_globalPaks = Cvar_Get( "fs_globalPaks", "", CVAR_ARCHIVE );
	Cvar_SetDescription( fs_globalPaks,
		"Map packs whose files count on every map, as they did before fs_mapPakScope\n"
		"Names of pak files, separated by spaces, with or without .pk3, in any case: a name alone for "
		"that pak in any game directory, \"<directory>/<name>\" (baseq3/ospmaps0) for it in that one. For a "
		"map pack that also carries files meant for every map. Applies from the next map." );
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

	// the content cache's paks are mounted again as the directories are added
	Content_Cache_Unmounted();
	fs_numCachePaks = fs_numRefusedPaks = 0;

	FS_AddGameDirectories(gameName, qfalse);

	// a dedicated server on the demo the engine chose: baseq3 searched
	// above it as a directory alone, its paks not, so the server's own
	// folder and server.cfg stay where they'll be with the full game's paks,
	// and the game directory written in (FS_ServerGameDir) is baseq3; not
	// where the admin's fs_basegame or fs_game names baseq3, paks and all
	if ( fs_demo == FS_DEMO_SERVER_DIR && Q_stricmp( BASEGAME, gameName ) &&
		Q_stricmp( BASEGAME, fs_basegame->string ) && Q_stricmp( BASEGAME, fs_gamedirvar->string ) ) {
		FS_AddGameDirectories(BASEGAME, qtrue);
	}

	// check for additional base game so mods can be based upon other mods
	if ( fs_basegame->string[0] && Q_stricmp( fs_basegame->string, gameName ) ) {
		FS_AddGameDirectories(fs_basegame->string, qfalse);
	}

	// check for additional game folder for mods
	if ( fs_gamedirvar->string[0] && Q_stricmp( fs_gamedirvar->string, gameName ) ) {
		FS_AddGameDirectories(fs_gamedirvar->string, qfalse);
	}

	// a dedicated server's into the directory it writes in, the last one
	// added (FS_ServerGameDir)
	FS_ImportPredecessorConfig(predConfigPath, Com_IsClient() ? gameName : fs_gamedir);

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
	Cmd_AddCommand ("pakconflicts", FS_PakConflicts_f );
	Cmd_AddCommand ("fs_lookup", FS_Lookup_f );

	// the name index of the paks found, in the order the lookups go
	FS_IndexBuild();

	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=506
	// reorder the pure pk3 files according to server order
	FS_ReorderPurePaks();
	FS_NoteServerPaks();
	FS_RankPaks();
	FS_NoteMapPackConflicts();

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

	// the demo is played without asking (FS_ChooseDemo), and com_basegame
	// set by hand would keep a server's folder in demoq3, so it isn't
	// suggested
	if(!Q_stricmp(game, BASEGAME))
	{
		char demoPath[MAX_OSPATH];

		Com_sprintf(demoPath, sizeof(demoPath), "%s%cdemoq3%c", installPath, PATH_SEP, PATH_SEP);
		Q_strcat(errorText, sizeof(errorText),
				va("\nTo run on the free demo's data instead, put its pak0.pk3 in "
				"\"%s\": it's played when the full game's paks aren't found, "
				"unless com_basegame is set. The pak is inside id's demo installer "
				"for Linux, linuxq3ademo-1.11-6.x86.gz.sh, which mirrors of id's "
				"FTP site keep.\n", demoPath));
		if(Com_IsClient())
		{
#ifdef DEMO_HELPER_NAME
			// the product's helper that fetches it, in the client's package
			Q_strcat(errorText, sizeof(errorText),
					"\"" DEMO_HELPER_NAME "\", in " PRODUCT_NAME "'s folder, downloads it, "
					"asks you to accept id's agreement and puts the pak there.\n");
#endif
			// both folders the text names, and the home that shows them
			// opened; a server writes nothing, as before
			FS_CreatePath(demoPath);
			FS_CreatePath(gamePath);
			Com_sprintf(gamePath, sizeof(gamePath), "%s%c", installPath, PATH_SEP);
		}
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
	int			index;
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
				&& ( index = FS_IdPakIndex( pakBasename, NUM_ID_PAKS ) ) >= 0)
		{
			// or the tests' stand-in for it (FS_TestPakChecksum)
			if( curpack->checksum != pak_checksums[index] && !FS_TestPakChecksum( curpack->checksum ) )
			{
				if(index == 0)
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
							index, curpack->checksum );
				}
			}

			foundPak |= 1<<index;
		}
		else if(!Q_stricmpn(curpack->pakGamename, BASETA, MAX_OSPATH)
				&& ( index = FS_IdPakIndex( pakBasename, NUM_TA_PAKS ) ) >= 0)

		{
			if(curpack->checksum != missionpak_checksums[index])
			{
				Com_Printf("\n\n"
						"**************************************************\n"
						"WARNING: " BASETA "/pak%d.pk3 is present but its checksum (%u)\n"
						"is not correct. Please re-install Team Arena\n"
						"**************************************************\n\n\n",
						index, curpack->checksum );				
			}

			foundTA |= 1 << index;
		}
		else
		{
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
		// is the element a pak file, and one clients have?
		if ( !search->pack || search->pack->excluded ) {
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
		// is the element a pak file, and one clients have?
		if ( !search->pack || search->pack->excluded ) {
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
		// is the element a pak file, and one clients have?
		if ( !search->pack || search->pack->excluded ) {
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
outside the base game, and the base game's referenced ones, but for the
server's own (fs_excludeReference)
=====================
*/
static qboolean FS_PakIsReferenced( const pack_t *pak ) {
	return !pak->excluded &&
		( pak->referenced || Q_stricmpn( pak->pakGamename, com_basegame->string, strlen( com_basegame->string ) ) );
}

/*
=====================
FS_ExcludePaks

Marks the paks a list names, "game/name" or "game/name.pk3" separated by
spaces in any case (fs_excludeReference, after Quake3e's), as the
server's own: the server reads them as ever, but no list it gives clients
names them, nor the content manifest, so clients neither need nor fetch
them. Every pak is matched by name, those outside the base game too,
which the referenced list names all of. A name no pak has is said, once
for each list. Returns how many paks are marked
=====================
*/
int FS_ExcludePaks( const char *list ) {
	static char		said[MAX_STRING_CHARS];
	const char		*p = list;
	searchpath_t	*search;
	char			word[MAX_OSPATH], unmatched[MAX_STRING_CHARS];
	qboolean		matched;
	int				count = 0;

	for ( search = fs_searchpaths; search; search = search->next ) {
		if ( search->pack ) {
			search->pack->excluded = qfalse;
		}
	}
	unmatched[0] = '\0';
	while ( FS_NextListedName( &p, word, sizeof( word ) ) ) {
		matched = qfalse;
		for ( search = fs_searchpaths; search; search = search->next ) {
			pack_t	*pak = search->pack;

			if ( pak && !FS_FilenameCompare( va( "%s/%s", pak->pakGamename, pak->pakBasename ), word ) ) {
				count += !pak->excluded;
				pak->excluded = qtrue;
				matched = qtrue;
			}
		}
		if ( !matched ) {
			Q_strcat( unmatched, sizeof( unmatched ), va( " %s", word ) );
		}
	}
	if ( unmatched[0] && strcmp( said, list ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: fs_excludeReference names paks the server doesn't have, which it "
			"leaves out:%s\n", unmatched );
	}
	Q_strncpyz( said, unmatched[0] ? list : "", sizeof( said ) );
	return count;
}

/*
=====================
FS_ExcludedPakWith

The pak, as "game/name", a file is read from when that's one of the
server's own (FS_ExcludePaks), which clients don't have; NULL otherwise
=====================
*/
const char *FS_ExcludedPakWith( const char *filename ) {
	static char			name[MAX_OSPATH];
	const searchpath_t	*search = FS_FileSearch( filename );

	if ( !search || !search->pack || !search->pack->excluded ) {
		return NULL;
	}
	Com_sprintf( name, sizeof( name ), "%s/%s", search->pack->pakGamename, search->pack->pakBasename );
	return name;
}

/*
=====================
FS_TouchFileInPak

Marks as referenced, with the flags, the first pak holding a file that
clients have, passing over the server's own (FS_ExcludePaks): for the
pure check's cgame and ui, when one of those holds them
=====================
*/
void FS_TouchFileInPak( const char *filename, int flags ) {
	searchpath_t	*search;

	for ( search = FS_NextSearch( NULL ); search; search = FS_NextSearch( search ) ) {
		if ( search->pack && !search->pack->excluded && FS_PakHasFile( search->pack, filename ) ) {
			search->pack->referenced |= flags;
			return;
		}
	}
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
	int		i, c, d, sum, moved;

	Cmd_TokenizeString( pakSums );

	c = Cmd_Argc();
	if ( c > MAX_SEARCH_PATHS ) {
		c = MAX_SEARCH_PATHS;
	}

	moved = c != fs_numServerPaks;
	fs_numServerPaks = c;

	for ( i = 0 ; i < c ; i++ ) {
		sum = atoi( Cmd_Argv( i ) );
		moved |= sum != fs_serverPaks[i];
		fs_serverPaks[i] = sum;
	}
	// the paks a read may take changed (FS_Ranking)
	fs_rankings += moved;

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

#ifndef STANDALONE
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
#endif

/*
================
FS_FindInstalledPak

The search path's first installed pak, not one of the content cache's,
named name ("<game>/<name>", as the referenced paks are named), or NULL
================
*/
static const pack_t *FS_FindInstalledPak( const char *name ) {
	const searchpath_t	*path;
	const pack_t		*pak;

	for( path = fs_searchpaths; path; path = path->next ) {
		pak = path->pack;
		if( pak && !pak->cached && !FS_FilenameCompare( va( "%s/%s", pak->pakGamename, pak->pakBasename ), name ) ) {
			return pak;
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
	const pack_t	*pak = FS_FindInstalledPak( download );

	return pak && FS_RetailPakChecksum( va( "%s/%s.pk3", pak->pakGamename, pak->pakBasename ),
		pak->checksum, retail, size );
}

/*
================
FS_InstalledPakNamed

Whether an installed pak has the name a download gives
("<game>/<name>.pk3"), whatever its contents: a download of that name is
then another version of it, which goes into the content cache beside it
and never replaces it
================
*/
qboolean FS_InstalledPakNamed( const char *download ) {
	char	name[MAX_QPATH];

	COM_StripExtension( download, name, sizeof( name ) );
	return FS_FindInstalledPak( name ) != NULL;
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
	if( FS_TestPakChecksum( checksum ) ) {
		Q_strncpyz( retail, download, size );
		return qtrue;
	}
	return qfalse;
}

/*
================
FS_LeftOutPaks

The full game's point release left out of baseq3 while the demo plays
from there (FS_AddGameDirectory), "pak1.pk3, pak2.pk3"; empty for none
================
*/
const char *FS_LeftOutPaks( void ) {
	static char	names[MAX_QPATH * 2];
	int			i;

	names[0] = '\0';
	for ( i = 1; i < NUM_ID_PAKS; i++ ) {
		if ( fs_leftOutPaks & ( 1u << i ) ) {
			Q_strcat( names, sizeof( names ), va( "%spak%d.pk3", names[0] ? ", " : "", i ) );
		}
	}
	return names;
}

/*
================
FS_DemoPak0Checksum

Whether a pak's checksum is the demo's pak0's, whatever its name
================
*/
qboolean FS_DemoPak0Checksum( unsigned int checksum ) {
#ifndef STANDALONE
	return checksum == DEMO_PAK0_CHECKSUM;
#else
	return qfalse;
#endif
}

#ifndef STANDALONE
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
	qboolean		inBase, asked;

	if( Cvar_Source( com_basegame ) != CVAR_SOURCE_DEFAULT || Q_stricmp( com_basegame->string, BASEGAME ) ||
		FS_FindPak0( BASEGAME, pak_checksums[0] ) ||
		( fs_refusedTestPak && FS_FindPak0( BASEGAME, fs_refusedTestChecksum ) ) ) {
		return qfalse;
	}
	demo = FS_FindPak0( BASEGAME, DEMO_PAK0_CHECKSUM );
	inBase = demo != NULL;
	// playing it from baseq3 takes fs_basegame, which an admin's may hold
	if( inBase && fs_basegame->string[0] && Q_stricmp( fs_basegame->string, BASEGAME ) ) {
		return qfalse;
	}

	asked = fs_basegame->string[0] || !Q_stricmp( Cvar_VariableString( "fs_game" ), BASEGAME );
	FS_Shutdown( qfalse );
	Cvar_Set2( "com_basegame", "demoq3", qtrue );
	if( inBase ) {
		Cvar_Set2( "fs_basegame", BASEGAME, qtrue );
	}
	// a dedicated server keeps baseq3 searched as a directory alone
	// (FS_Startup), where its own folder and server.cfg stay; with the
	// demo's pak0 there, as fs_basegame does already, it leaves out the
	// point release there, unless an admin asked for baseq3 (fs_basegame,
	// fs_game) and its paks
	fs_demo = Com_IsClient() || ( inBase && asked ) ? FS_DEMO_NONE : inBase ? FS_DEMO_IN_BASE : FS_DEMO_SERVER_DIR;
	FS_Startup( com_basegame->string );
	demo = FS_FindPak0( inBase ? BASEGAME : "demoq3", DEMO_PAK0_CHECKSUM );
	if( !demo ) {
		// no demo either: the full game's missing data, as before
		FS_Shutdown( qfalse );
		Cvar_Set2( "com_basegame", BASEGAME, qtrue );
		fs_demo = FS_DEMO_NONE;
		FS_Startup( com_basegame->string );
		return qfalse;
	}

	Com_Printf( "Playing the demo's data (%s): the full game's paks weren't found. "
		"Put pak0.pk3 to pak8.pk3 in %s%c" BASEGAME " to play the full game.\n",
		demo->pakFilename, fs_homedatapath->string, PATH_SEP );
	return qtrue;
}
#endif

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
		Sys_InitPIDFile( FS_ServerGameDir() );

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
"servers/<sv_instance>", or without a name "servers/<port>", by the port
it answers on (NET_ServerPort), fixed for the run before the config is
read. A name is letters, digits, - and _, up to 32, as it's a folder's
name on every system; another stops the server. "" on a client
================
*/
const char *FS_ServerDir( void )
{
	static char	dir[MAX_QPATH];
	cvar_t		*instance;
	const char	*c;

	if ( Com_IsClient() ) {
		return "";
	}
	if ( !dir[0] ) {
		instance = Cvar_Get( "sv_instance", "", CVAR_INIT | CVAR_PROTECTED );
		Cvar_SetDescription( instance, "Name of this server among those sharing a home\n"
			"Its folder is servers/<name> in the game directory, and its Omniwire instance server-<name>; "
			"empty uses the port, servers/27960, as before. Letters, digits, - and _, up to 32, set on the "
			"command line or in the environment (OMNIFRAG_SV_INSTANCE)." );
		for ( c = instance->string; *c; c++ ) {
			if ( !isalnum( (unsigned char)*c ) && *c != '-' && *c != '_' ) {
				break;
			}
		}
		// and not one Windows keeps for a device, con or com1
		if ( *c || strlen( instance->string ) > 32 ||
			( instance->string[0] && !FS_ValidFolderName( instance->string ) ) ) {
			Com_ErrorExit( EXIT_CONFIG_ERROR, "sv_instance \"%s\" isn't a server's name: letters, digits, - and _, "
				"up to 32, and not a name Windows keeps for a device", instance->string );
		}
		if ( instance->string[0] ) {
			Com_sprintf( dir, sizeof( dir ), "servers/%s", instance->string );
		} else {
			Com_sprintf( dir, sizeof( dir ), "servers/%d", NET_ServerPort() );
		}
	}
	return dir;
}

/*
================
FS_ServerGameDir

The game directory a dedicated server's own folder (FS_ServerDir) is in:
the one it writes in, the last one searched (fs_gamedir): a mod's with
fs_game, or baseq3, which the demo the engine chose for want of the full
game's paks searches above demoq3 (FS_ChooseDemo), so the server finds its
settings, bans, keys and logs as it left them once retail paks are added.
The current game directory on a client
================
*/
const char *FS_ServerGameDir( void )
{
	return Com_IsClient() ? FS_GetCurrentGameDir() : FS_LoadedGameDir();
}

/*
================
FS_ServerQPath

A file of the server's own folder, relative to the home: in its game
directory (FS_ServerGameDir)
================
*/
const char *FS_ServerQPath( const char *filename )
{
	return va( "%s/%s", FS_ServerGameDir(), FS_ServerFile( filename ) );
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
			// the game's own paks only where the server sends them, and
			// none of the server's own, which nothing sends
			if ( FS_PakIsReferenced( pak ) == pass || FS_PakRefused( pak ) || pak->excluded ||
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
		// the server's own paks are no client's content (fs_excludeReference)
		if ( !( pak = search->pack ) || pak->excluded ) {
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
