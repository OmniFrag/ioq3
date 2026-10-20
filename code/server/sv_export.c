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
// sv_export.c -- the content command. content export writes the paks the
// server would send, its content manifest's, to a directory for a static
// host or CDN that sv_dlURL then names, by hash and by name
// (downloads.md section 6). The files are copied, or hard-linked where
// they can be, on a thread of their own, so a big set never holds up a
// frame; it reads the disk beside the pak index's hash job (content.h).

#include "server.h"
#include "../qcommon/content.h"

#define EXPORT_BUFFER_SIZE		( 256 * 1024 )
#define EXPORT_REPORT_MSEC		10000		// between reports of how far it's got
#define EXPORT_MAX_LISTED		8			// paks named in a line of the summary

typedef enum {
	EXPORT_THERE,		// already there, with the pak's hash
	EXPORT_LINKED,
	EXPORT_COPIED,
	EXPORT_FAILED
} exportResult_t;

// where a pak goes: by hash, then by name
#define EXPORT_BY_HASH		0
#define EXPORT_BY_NAME		1

typedef struct {
	char			ospath[MAX_OSPATH];
	exportResult_t	result;
	const char		*why;		// a constant string, when it failed
} exportTarget_t;

typedef struct {
	char			source[MAX_OSPATH];
	int64_t			size, mtime;	// the source's, as hashed
	byte			sha256[SHA256_DIGEST_SIZE];
	char			name[OFCM_GAMEDIR_SIZE + OFCM_NAME_SIZE + 8];	// "<gamedir>/<name>.pk3", for what's said
	int				sameHash;		// an earlier item of the same hash, whose file by hash is this one's, or -1
	exportTarget_t	targets[2];
} exportItem_t;

// what the thread touches: its own copy of the items, its buffer, and the
// counts the frame reads for its reports, which may lag
typedef struct {
	exportItem_t	*items;
	int				count;
	byte			*buffer;
	qboolean		copies;			// never hard links
	volatile int	done;			// items finished
	volatile int64_t	bytes;		// bytes copied

	// the frame's
	char			dir[MAX_OSPATH];
	int64_t			total;			// the paks' bytes
	int				started, nextReport;
} exportJob_t;

static exportJob_t	exportJob;
static sysThread_t	*exportThread;
static char			exportMadeDir[MAX_OSPATH];	// the last directory SV_ExportMakeDirs made

/*
==============================================================

THE THREAD

Touches nothing but the job: reads and writes files by their OS paths,
with stdio and Sys_ calls only, and says why it failed in constant
strings

==============================================================
*/

/*
=================
SV_ExportHolds

Whether the file at ospath is the item's pak already. A file of the
pak's size and time is the pak, as the pak index takes it to be, which a
hard link to it always is; another of its size is hashed
=================
*/
static qboolean SV_ExportHolds( const exportItem_t *item, const char *ospath, int64_t mtime ) {
	byte		digest[SHA256_DIGEST_SIZE];
	int64_t		size, time;

	if ( !Content_FileStat( ospath, &size, &time ) || size != item->size ) {
		return qfalse;
	}
	if ( time == mtime ) {
		return qtrue;
	}
	return Content_HashFile( ospath, digest, &size ) && size == item->size
		&& !memcmp( digest, item->sha256, SHA256_DIGEST_SIZE );
}

/*
=================
SV_ExportUnchanged

Whether the item's pak still has the size and time it was hashed with, so
its bytes are still the hash's
=================
*/
static qboolean SV_ExportUnchanged( const exportItem_t *item ) {
	int64_t		size, mtime;

	return Content_FileStat( item->source, &size, &mtime ) && size == item->size && mtime == item->mtime;
}

/*
=================
SV_ExportCopy

Copies the item's pak to part, hashing what it reads, which must be the
pak's hash; NULL, or why it failed
=================
*/
static const char *SV_ExportCopy( exportJob_t *job, const exportItem_t *item, const char *part ) {
	FILE		*in, *out;
	sha256_t	sha;
	byte		digest[SHA256_DIGEST_SIZE];
	int64_t		read = 0;
	size_t		got;
	const char	*why = NULL;

	in = Sys_FOpen( item->source, "rb" );
	if ( !in ) {
		return "the pak can't be read";
	}
	out = Sys_FCreateReadable( part );
	if ( !out ) {
		fclose( in );
		return "the file can't be written";
	}
	Com_SHA256Init( &sha );
	while ( ( got = fread( job->buffer, 1, EXPORT_BUFFER_SIZE, in ) ) > 0 ) {
		Com_SHA256Update( &sha, job->buffer, got );
		read += got;
		job->bytes += got;
		if ( fwrite( job->buffer, 1, got, out ) != got ) {
			why = "the file can't be written";
			break;
		}
	}
	if ( !why && ferror( in ) ) {
		why = "the pak can't be read";
	}
	if ( fclose( out ) != 0 && !why ) {
		why = "the file can't be written";
	}
	fclose( in );
	Com_SHA256Final( &sha, digest );
	if ( !why && ( read != item->size || memcmp( digest, item->sha256, SHA256_DIGEST_SIZE ) ) ) {
		why = "the pak changed since it was hashed";
	}
	return why;
}

/*
=================
SV_ExportTarget

Puts the item's pak at one of its targets, as a .part first: a hard link
to the pak, or to the copy by hash, costs no space, and a copy is checked
against the hash as it's made
=================
*/
static void SV_ExportTarget( exportJob_t *job, exportItem_t *item, int t ) {
	exportTarget_t			*target = &item->targets[t];
	const exportTarget_t	*byHash = &item->targets[EXPORT_BY_HASH];
	char					part[MAX_OSPATH + 8];
	const char				*why;
	int64_t					size, mtime = item->mtime;
	qboolean				linked;

	// the file by hash an earlier item put there, or the one by name a
	// link to it, as on another disk than the pak's
	if ( t == EXPORT_BY_HASH && item->sameHash >= 0 ) {
		*target = job->items[item->sameHash].targets[EXPORT_BY_HASH];
		if ( target->result != EXPORT_FAILED ) {
			target->result = EXPORT_THERE;
		}
		return;
	}
	if ( t == EXPORT_BY_NAME && byHash->result != EXPORT_FAILED ) {
		Content_FileStat( byHash->ospath, &size, &mtime );
	}
	if ( SV_ExportHolds( item, target->ospath, item->mtime ) || ( mtime != item->mtime
		&& SV_ExportHolds( item, target->ospath, mtime ) ) ) {
		target->result = EXPORT_THERE;
		return;
	}

	Com_sprintf( part, sizeof( part ), "%s.part", target->ospath );
	remove( part );
	linked = !job->copies && ( Sys_HardLink( item->source, part ) || ( t == EXPORT_BY_NAME
		&& byHash->result != EXPORT_FAILED && Sys_HardLink( byHash->ospath, part ) ) );
	if ( linked ) {
		why = SV_ExportUnchanged( item ) ? NULL : "the pak changed since it was hashed";
	} else {
		why = SV_ExportCopy( job, item, part );
	}
	if ( !why && !Sys_Rename( part, target->ospath ) ) {
		why = "the file can't be renamed into place";
	}
	if ( why ) {
		remove( part );
		target->result = EXPORT_FAILED;
		target->why = why;
	} else {
		target->result = linked ? EXPORT_LINKED : EXPORT_COPIED;
	}
}

static void SV_ExportThread( void *arg ) {
	exportJob_t	*job = arg;
	int			i;

	for ( i = 0; i < job->count; i++ ) {
		SV_ExportTarget( job, &job->items[i], EXPORT_BY_HASH );
		SV_ExportTarget( job, &job->items[i], EXPORT_BY_NAME );
		job->done = i + 1;
	}
}

/*
==============================================================

THE FRAME

==============================================================
*/

/*
=================
SV_ExportList

Adds a pak's name to a line of names, the first few, then a count of the
rest at the end (SV_ExportListEnd)
=================
*/
static void SV_ExportList( char *line, int size, int *count, const char *name ) {
	if ( (*count)++ < EXPORT_MAX_LISTED ) {
		Q_strcat( line, size, va( "%s%s", line[0] ? ", " : "", name ) );
	}
}

static const char *SV_ExportListEnd( const char *line, int count ) {
	if ( count > EXPORT_MAX_LISTED ) {
		return va( "%s and %d more", line, count - EXPORT_MAX_LISTED );
	}
	return line;
}

/*
=================
SV_ExportFinish

What the job did
=================
*/
static void SV_ExportFinish( void ) {
	exportJob_t		*job = &exportJob;
	const exportItem_t	*item;
	char			failed[MAX_STRING_CHARS] = "";
	int				i, t, there = 0, linked = 0, copied = 0, numFailed = 0;
	int64_t			copiedBytes = 0;

	for ( i = 0; i < job->count; i++ ) {
		item = &job->items[i];
		for ( t = 0; t < 2; t++ ) {
			switch ( item->targets[t].result ) {
			case EXPORT_THERE:
				there++;
				break;
			case EXPORT_LINKED:
				linked++;
				break;
			case EXPORT_COPIED:
				copied++;
				copiedBytes += item->size;
				break;
			default:
				Com_Printf( S_COLOR_YELLOW "WARNING: content export: %s, %s: %s\n", item->name,
					t == EXPORT_BY_HASH ? "by hash" : "by name", item->targets[t].why );
				// each pak once, if both its files failed
				if ( t == EXPORT_BY_HASH || item->targets[EXPORT_BY_HASH].result != EXPORT_FAILED ) {
					SV_ExportList( failed, sizeof( failed ), &numFailed, item->name );
				}
				break;
			}
		}
	}

	Com_Printf( "Exported %d paks to %s in %d s: %d files copied (%lld MB), %d hard-linked, %d already there%s\n",
		job->count, job->dir, ( Sys_Milliseconds() - job->started ) / 1000, copied,
		(long long)( copiedBytes >> 20 ), linked, there,
		numFailed ? va( "; %d paks failed: %s", numFailed, SV_ExportListEnd( failed, numFailed ) ) : "" );
}

/*
=================
SV_ExportFrame

Once a frame, from SV_ContentFrame: how far the job has got, every so
often, and what it did once it's done
=================
*/
void SV_ExportFrame( void ) {
	exportJob_t	*job = &exportJob;

	if ( !exportThread ) {
		return;
	}
	if ( !Sys_ThreadFinished( exportThread ) ) {
		if ( Sys_Milliseconds() - job->nextReport >= 0 ) {
			job->nextReport = Sys_Milliseconds() + EXPORT_REPORT_MSEC;
			Com_Printf( "content export: %d of %d paks, %lld of %lld MB copied at most\n", job->done,
				job->count, (long long)( job->bytes >> 20 ), (long long)( job->total >> 20 ) );
		}
		return;
	}
	exportThread = NULL;
	SV_ExportFinish();
}

/*
=================
SV_ExportMakeDirs

Makes the directory of a file to be exported, and those above it, for
the web server to read; qfalse if one can't be made. The one it made
last is passed over, as the paks of a game come together
=================
*/
static qboolean SV_ExportMakeDirs( const char *ospath ) {
	char		path[MAX_OSPATH];
	char		*s;

	Q_strncpyz( path, ospath, sizeof( path ) );
	*strrchr( path, PATH_SEP ) = '\0';
	if ( !strcmp( path, exportMadeDir ) ) {
		return qtrue;
	}
	// from past the root, "/" or a drive's "C:\", or on Windows a share's
	// "\\server\share\", which can't be made
	s = path + ( path[1] == ':' ? 3 : 1 );
#ifdef _WIN32
	if ( ( path[0] == '/' || path[0] == '\\' ) && ( path[1] == '/' || path[1] == '\\' ) ) {
		int		seps = 0;

		for ( s = path + 2; *s && seps < 2; s++ ) {
			if ( *s == '/' || *s == '\\' ) {
				seps++;
			}
		}
		if ( seps < 2 ) {
			Q_strncpyz( exportMadeDir, path, sizeof( exportMadeDir ) );
			return qtrue;	// the share itself
		}
	}
#endif
	for ( ; ; s++ ) {
		if ( *s == '/' || *s == '\\' || !*s ) {
			char	c = *s;

			*s = '\0';
			if ( !Sys_MkdirReadable( path ) ) {
				Com_Printf( "content export: can't make %s\n", path );
				exportMadeDir[0] = '\0';
				return qfalse;
			}
			if ( !c ) {
				break;
			}
			*s = PATH_SEP;
		}
	}
	Q_strncpyz( exportMadeDir, path, sizeof( exportMadeDir ) );
	return qtrue;
}

/*
=================
SV_ExportAbsolute

Whether the directory the operator gave is a whole path, not one from
wherever the server happened to start
=================
*/
static qboolean SV_ExportAbsolute( const char *dir ) {
	const char	*s;

	// with no ".." in it, though "a..b" is a name
	for ( s = dir; ( s = strstr( s, ".." ) ) != NULL; s += 2 ) {
		if ( ( s == dir || s[-1] == '/' || s[-1] == '\\' ) && ( !s[2] || s[2] == '/' || s[2] == '\\' ) ) {
			return qfalse;
		}
	}
#ifdef _WIN32
	return dir[0] == '/' || dir[0] == '\\' || ( Q_isalpha( dir[0] ) && dir[1] == ':' && ( dir[2] == '/' || dir[2] == '\\' ) );
#else
	// "\dir" or "C:/dir" is a name in the current directory here
	return dir[0] == '/';
#endif
}

/*
=================
SV_ExportStart

content export <dir> [copy]: every pak the current content manifest
lists, required and optional, but the game's own, by hash and by name
=================
*/
static void SV_ExportStart( const char *dir, qboolean copies ) {
	exportJob_t		*job = &exportJob;
	const byte		*manifest;
	ofcmHeader_t	header;
	ofcmEntry_t		entry;
	exportItem_t	*item;
	char			hex[SHA256_HEX_SIZE], retail[MAX_QPATH], refused[MAX_STRING_CHARS] = "";
	const char		*source;
	int				i, j, length, numRefused = 0, numGone = 0;
	size_t			dirLength;

	if ( exportThread ) {
		Com_Printf( "An export is running: %d of %d paks done\n", job->done, job->count );
		return;
	}
	if ( !SV_ExportAbsolute( dir ) ) {
		Com_Printf( "content export: give a whole path to a directory, without \"..\"\n" );
		return;
	}
	manifest = SV_ContentManifest( NULL, &length );
	if ( !manifest || OFCM_Read( manifest, length, &header, NULL ) != NULL ) {
		Com_Printf( "content export: there's no content manifest yet; load a map first\n" );
		return;
	}

	Q_strncpyz( job->dir, dir, sizeof( job->dir ) );
	dirLength = strlen( job->dir );
	while ( dirLength > 1 && ( job->dir[dirLength - 1] == '/' || job->dir[dirLength - 1] == '\\' ) ) {
		job->dir[--dirLength] = '\0';
	}

	free( job->items );
	job->items = calloc( header.numEntries + 1, sizeof( *job->items ) );
	if ( !job->buffer ) {
		job->buffer = malloc( EXPORT_BUFFER_SIZE );
	}
	if ( !job->items || !job->buffer ) {
		Com_Printf( "content export: out of memory\n" );
		return;
	}
	job->count = 0;
	job->total = 0;
	exportMadeDir[0] = '\0';
	if ( !SV_ExportMakeDirs( va( "%s%csha256%cx", job->dir, PATH_SEP, PATH_SEP ) ) ) {
		return;
	}
	exportMadeDir[0] = '\0';
	for ( i = 0; i < header.numEntries; i++ ) {
		OFCM_ReadEntry( manifest, i, &entry );
		item = &job->items[job->count];
		Com_sprintf( item->name, sizeof( item->name ), "%s/%s.pk3", entry.gameDir, entry.name );
		// the game's own, never, whatever sv_refuseRetailPaks says: they go
		// to a host the world can read
		if ( ( entry.flags & OFCM_FLAG_OWN ) || FS_RetailPakChecksum( item->name, entry.checksum, retail, sizeof( retail ) ) ) {
			SV_ExportList( refused, sizeof( refused ), &numRefused, item->name );
			continue;
		}
		source = SV_ContentFindPak( entry.sha256, &item->size, &item->mtime );
		if ( !source || !FS_ValidDownloadPak( entry.gameDir, entry.name ) ) {
			numGone++;
			continue;
		}
		Q_strncpyz( item->source, source, sizeof( item->source ) );
		Com_Memcpy( item->sha256, entry.sha256, SHA256_DIGEST_SIZE );
		item->sameHash = -1;
		for ( j = 0; j < job->count; j++ ) {
			if ( !memcmp( job->items[j].sha256, item->sha256, SHA256_DIGEST_SIZE ) ) {
				item->sameHash = j;
				break;
			}
		}
		Com_SHA256Hex( entry.sha256, hex );
		if ( Com_sprintf( item->targets[EXPORT_BY_HASH].ospath, MAX_OSPATH, "%s%csha256%c%s.pk3",
				job->dir, PATH_SEP, PATH_SEP, hex ) >= MAX_OSPATH - 8
			|| Com_sprintf( item->targets[EXPORT_BY_NAME].ospath, MAX_OSPATH, "%s%c%s%c%s.pk3",
				job->dir, PATH_SEP, entry.gameDir, PATH_SEP, entry.name ) >= MAX_OSPATH - 8 ) {
			Com_Printf( "content export: %s is too long a path for the files\n", job->dir );
			return;
		}
		if ( !SV_ExportMakeDirs( item->targets[EXPORT_BY_NAME].ospath ) ) {
			return;
		}
		job->total += item->size;
		job->count++;
	}

	if ( numRefused ) {
		Com_Printf( "content export: left out the game's own paks, which players add from their own copies: %s\n",
			SV_ExportListEnd( refused, numRefused ) );
	}
	if ( numGone ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: content export: %d paks the content manifest lists are gone from the server\n", numGone );
	}
	// what isn't in the content manifest isn't exported: a pak still
	// waiting to be hashed, or one added since the map loaded
	if ( Content_PakIndex_Busy() ) {
		Com_Printf( "content export: some paks are still being hashed and aren't in the content manifest yet; "
			"export again once \"Content manifest\" says none are left\n" );
	}
	if ( !job->count ) {
		Com_Printf( "content export: no paks to export\n" );
		return;
	}

	job->copies = copies;
	job->done = 0;
	job->bytes = 0;
	exportThread = Sys_StartThread( SV_ExportThread, job );
	if ( !exportThread ) {
		Com_Printf( "content export: this build has no threads to export with\n" );
		return;
	}
	job->started = Sys_Milliseconds();
	job->nextReport = job->started + EXPORT_REPORT_MSEC;
	Com_Printf( "Exporting %d paks, %lld MB, to %s: by hash in sha256/ and by name in each game's directory\n",
		job->count, (long long)( job->total >> 20 ), job->dir );
}

/*
=================
SV_Content_f

The content command. It writes files where the operator says, so it runs
only from the server's own console, not through rcon, whose password may
be all a stranger needs; nor from game code or a pak's config, which
Cmd_ExecuteString turns away
=================
*/
void SV_Content_f( void ) {
	if ( Com_IsRedirecting() ) {
		Com_Printf( "content runs only from the server's console, not through rcon\n" );
		return;
	}
	if ( ( Cmd_Argc() == 3 || ( Cmd_Argc() == 4 && !Q_stricmp( Cmd_Argv( 3 ), "copy" ) ) )
		&& !Q_stricmp( Cmd_Argv( 1 ), "export" ) ) {
		SV_ExportStart( Cmd_Argv( 2 ), Cmd_Argc() == 4 );
		return;
	}
	Com_Printf( "usage: content export <directory> [copy]: puts the paks the server sends, but the game's own, "
		"in the directory by hash (sha256/<hash>.pk3) and by name (<game>/<name>.pk3), for a web server "
		"that sv_dlURL names; hard-linked where it can be, unless copy says to copy\n" );
}
