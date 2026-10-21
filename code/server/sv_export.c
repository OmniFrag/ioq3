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
// content add checks a pak an operator installs, and copies it into a
// game directory on the same thread, for the next map (section 11).
// What either is told to read or write is in sv_contentDir, which only
// the command line sets, so rcon may run them; content add then installs
// the pak in the home's game directory, as its name says.

#include "server.h"
#include "../qcommon/content.h"

#define EXPORT_BUFFER_SIZE		( 256 * 1024 )
#define EXPORT_REPORT_MSEC		10000		// between reports of how far it's got
#define EXPORT_MAX_LISTED		8			// paks named in a line of the summary

typedef enum {
	EXPORT_THERE,		// already there, with the pak's hash
	EXPORT_LINKED,
	EXPORT_COPIED,
	EXPORT_FAILED,
	EXPORT_DIFFERENT	// content add's: a pak of its name with other contents is there
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
	qboolean		add;			// content add's: its one pak by name alone
	volatile int	done;			// items finished
	volatile int64_t	bytes;		// bytes copied
	char			root[MAX_OSPATH];	// sv_contentDir, its links resolved (SV_ContentRoot)

	// the frame's
	char			dir[MAX_OSPATH];
	int64_t			total;			// the paks' bytes
	int				started, nextReport;
} exportJob_t;

static exportJob_t	exportJob;
static sysThread_t	*exportThread;
static char			exportMadeDir[MAX_OSPATH];	// the last directory SV_ExportMakeDirs made

static void SV_AddFinish( void );

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
SV_ContentUnder

Whether a resolved path is root or in it
=================
*/
static qboolean SV_ContentUnder( const char *root, const char *real ) {
	size_t	length = strlen( root );

#ifdef _WIN32
	if ( Q_stricmpn( real, root, length ) ) {
#else
	if ( strncmp( real, root, length ) ) {
#endif
		return qfalse;
	}
	// a root of "/" or "C:\" ends in its separator
	return !real[length] || real[length] == PATH_SEP || root[length - 1] == PATH_SEP;
}

/*
=================
SV_ContentInside

Whether what's opened at ospath stays in root, with every link resolved:
the directory it's in, and the file or directory itself if it's there.
Checked before each is opened or made
=================
*/
static qboolean SV_ContentInside( const char *root, const char *ospath ) {
	char	path[MAX_OSPATH], real[MAX_OSPATH];
	char	*sep;

	if ( Sys_RealPath( ospath, real, sizeof( real ) ) && !SV_ContentUnder( root, real ) ) {
		return qfalse;
	}
	Q_strncpyz( path, ospath, sizeof( path ) );
	sep = strrchr( path, PATH_SEP );
	if ( !sep ) {
		return qfalse;
	}
	*sep = '\0';
	return Sys_RealPath( path, real, sizeof( real ) ) && SV_ContentUnder( root, real );
}

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
pak's hash; content add's item takes it as its hash. NULL, or why it
failed
=================
*/
static const char *SV_ExportCopy( exportJob_t *job, exportItem_t *item, const char *part ) {
	FILE		*in, *out;
	sha256_t	sha;
	byte		digest[SHA256_DIGEST_SIZE];
	int64_t		read = 0;
	size_t		got;
	const char	*why = NULL;
	char		dir[MAX_OSPATH];
	unsigned long long	available, total;

	// leaving the disk's last CACHE_DISK_RESERVE free, as downloads do,
	// however often rcon exports
	Q_strncpyz( dir, part, sizeof( dir ) );
	*strrchr( dir, PATH_SEP ) = '\0';
	if ( Sys_DiskSpace( dir, &available, &total ) && (unsigned long long)item->size + CACHE_DISK_RESERVE > available ) {
		return "the disk is nearly full";
	}
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
	if ( why ) {
		return why;
	}
	if ( read != item->size ) {
		return "the pak changed since it was hashed";
	}
	if ( job->add ) {
		memcpy( item->sha256, digest, SHA256_DIGEST_SIZE );
	} else if ( memcmp( digest, item->sha256, SHA256_DIGEST_SIZE ) ) {
		return "the pak changed since it was hashed";
	}
	return NULL;
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
	if ( !SV_ContentInside( job->root, target->ospath ) ) {
		target->result = EXPORT_FAILED;
		target->why = "a link there leads out of sv_contentDir";
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

/*
=================
SV_AddTarget

content add's pak, beside its place in its game directory as a .part, a
copy hashed as it's made, which the frame checks again and renames into
place (SV_AddFinish); unless a pak of its name is there already, which is
the same file (its size, then its hash) or refused
=================
*/
static void SV_AddTarget( exportJob_t *job, exportItem_t *item ) {
	exportTarget_t	*target = &item->targets[EXPORT_BY_NAME];
	char			part[MAX_OSPATH + 8];
	byte			digest[SHA256_DIGEST_SIZE];
	const char		*why;
	int64_t			size, mtime;

	if ( !SV_ContentInside( job->root, item->source ) ) {
		target->result = EXPORT_FAILED;
		target->why = "a link there leads out of sv_contentDir";
		return;
	}
	if ( Content_FileStat( target->ospath, &size, &mtime ) ) {
		if ( size != item->size ) {
			target->result = EXPORT_DIFFERENT;
		} else if ( !Content_HashFile( item->source, item->sha256, &size ) || size != item->size ) {
			target->result = EXPORT_FAILED;
			target->why = "the pak can't be read";
		} else {
			target->result = Content_HashFile( target->ospath, digest, &size ) && size == item->size
				&& !memcmp( digest, item->sha256, SHA256_DIGEST_SIZE ) ? EXPORT_THERE : EXPORT_DIFFERENT;
		}
		return;
	}
	Com_sprintf( part, sizeof( part ), "%s.part", target->ospath );
	remove( part );
	why = SV_ExportCopy( job, item, part );
	if ( why ) {
		remove( part );
		target->result = EXPORT_FAILED;
		target->why = why;
	} else {
		target->result = EXPORT_COPIED;
	}
}

static void SV_ExportThread( void *arg ) {
	exportJob_t	*job = arg;
	int			i;

	if ( job->add ) {
		SV_AddTarget( job, &job->items[0] );
		job->done = 1;
		return;
	}
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
			if ( job->add ) {
				Com_Printf( "content add: %lld of %lld MB copied\n", (long long)( job->bytes >> 20 ),
					(long long)( job->total >> 20 ) );
			} else {
				Com_Printf( "content export: %d of %d paks, %lld of %lld MB copied at most\n", job->done,
					job->count, (long long)( job->bytes >> 20 ), (long long)( job->total >> 20 ) );
			}
		}
		return;
	}
	exportThread = NULL;
	if ( job->add ) {
		SV_AddFinish();
	} else {
		SV_ExportFinish();
	}
}

/*
=================
SV_ExportMakeDirs

Makes the directory of a file to be exported, and those above it in
root, for the web server to read: each, made or found there, must be in
root with its links resolved, so none on the way leads the next one
elsewhere. qfalse if one can't be made. The one it made last is passed
over, as the paks of a game come together
=================
*/
static qboolean SV_ExportMakeDirs( const char *root, const char *ospath ) {
	char		path[MAX_OSPATH], real[MAX_OSPATH];
	char		*s;

	Q_strncpyz( path, ospath, sizeof( path ) );
	*strrchr( path, PATH_SEP ) = '\0';
	if ( !strcmp( path, exportMadeDir ) || strlen( path ) <= strlen( root ) ) {
		return qtrue;	// or root itself, made already
	}
	for ( s = path + strlen( root ) + 1; ; s++ ) {
		if ( *s == '/' || *s == '\\' || !*s ) {
			char	c = *s;

			*s = '\0';
			if ( !Sys_MkdirReadable( path ) ) {
				Com_Printf( "content: can't make %s\n", path );
				exportMadeDir[0] = '\0';
				return qfalse;
			}
			if ( !Sys_RealPath( path, real, sizeof( real ) ) || !SV_ContentUnder( root, real ) ) {
				Com_Printf( "content: %s leads out of sv_contentDir, through a link\n", path );
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
SV_ContentRoot

sv_contentDir, made the first time it's used, in a directory that's
there, with its links resolved into the job's root, which all the
content command opens must be in
=================
*/
static qboolean SV_ContentRoot( void ) {
	const char	*dir = sv_contentDir->string;

	// a whole path, not one from wherever the server happened to start
#ifdef _WIN32
	if ( dir[0] != '/' && dir[0] != '\\' && !( Q_isalpha( dir[0] ) && dir[1] == ':' && ( dir[2] == '/' || dir[2] == '\\' ) ) ) {
#else
	// "\dir" or "C:/dir" is a name in the current directory here
	if ( dir[0] != '/' ) {
#endif
		Com_Printf( "content: sv_contentDir, %s, must be a whole path\n", dir );
		return qfalse;
	}
	if ( !Sys_MkdirReadable( dir ) || !Sys_RealPath( dir, exportJob.root, sizeof( exportJob.root ) ) ) {
		Com_Printf( "content: can't make sv_contentDir, %s\n", dir );
		return qfalse;
	}
	return qtrue;
}

// content add's pak
static struct {
	char			name[OFCM_GAMEDIR_SIZE + OFCM_NAME_SIZE + 8];	// "<gamedir>/<name>.pk3"
	char			gameDir[OFCM_GAMEDIR_SIZE], base[OFCM_NAME_SIZE];	// its parts, for the check again
	unsigned int	checksum;
	qboolean		added;			// one was added since the map loaded (SV_ExportMapLoaded)
} addPak;

/*
=================
SV_AddRunning

Whether an export or an add runs, saying so: one at a time
=================
*/
static qboolean SV_AddRunning( void ) {
	if ( !exportThread ) {
		return qfalse;
	}
	if ( exportJob.add ) {
		Com_Printf( "content add is adding %s\n", addPak.name );
	} else {
		Com_Printf( "An export is running: %d of %d paks done\n", exportJob.done, exportJob.count );
	}
	return qtrue;
}

/*
=================
SV_ExportMapLoaded

A map loaded, and its content manifest lists the paks added before
=================
*/
void SV_ExportMapLoaded( void ) {
	addPak.added = qfalse;
}

/*
=================
SV_ExportStart

content export [name] [copy]: every pak the current content manifest
lists, required and optional, but the game's own, by hash and by name,
in sv_contentDir or the folder name in it
=================
*/
static void SV_ExportStart( const char *name, qboolean copies ) {
	exportJob_t		*job = &exportJob;
	const byte		*manifest;
	ofcmHeader_t	header;
	ofcmEntry_t		entry;
	exportItem_t	*item;
	char			hex[SHA256_HEX_SIZE], retail[MAX_QPATH], refused[MAX_STRING_CHARS] = "";
	const char		*source;
	int				i, j, length, numRefused = 0, numGone = 0;

	if ( SV_AddRunning() || !SV_ContentRoot() ) {
		return;
	}
	// one folder's name, by the rule for a pak's names
	if ( name && !OFCM_ValidName( name, name ) ) {
		Com_Printf( "content export: %s isn't a name for a folder in sv_contentDir; give one name, with no "
			"path, of the characters a pak's name can have\n", name );
		return;
	}
	Q_strncpyz( job->dir, name ? va( "%s%c%s", job->root, PATH_SEP, name ) : job->root, sizeof( job->dir ) );
	manifest = SV_ContentManifest( NULL, &length );
	if ( !manifest || OFCM_Read( manifest, length, &header, NULL ) != NULL ) {
		Com_Printf( "content export: there's no content manifest yet; load a map first\n" );
		return;
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
	if ( !SV_ExportMakeDirs( job->root, va( "%s%csha256%cx", job->dir, PATH_SEP, PATH_SEP ) ) ) {
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
		if ( !SV_ExportMakeDirs( job->root, item->targets[EXPORT_BY_NAME].ospath ) ) {
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
	if ( addPak.added ) {
		Com_Printf( "content export: paks added since the map loaded aren't in the content manifest "
			"until the next map, and aren't exported\n" );
	}
	if ( Content_PakIndex_Busy() ) {
		Com_Printf( "content export: some paks are still being hashed and aren't in the content manifest yet; "
			"export again once \"Content manifest\" says none are left\n" );
	}
	if ( !job->count ) {
		Com_Printf( "content export: no paks to export\n" );
		return;
	}

	job->copies = copies;
	job->add = qfalse;
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
==============================================================

CONTENT ADD

==============================================================
*/

/*
=================
SV_AddStart

content add <file> [gamedir]: checks the pak, a file in sv_contentDir
(FS_CheckAddPak), then copies it on the thread, hashing it as it goes
=================
*/
static void SV_AddStart( const char *file, const char *gameDir ) {
	exportJob_t		*job = &exportJob;
	exportItem_t	*item;
	char			name[MAX_OSPATH], source[MAX_OSPATH], overrides[MAX_STRING_CHARS];
	const char		*refused;
	int64_t			size, mtime;

	if ( SV_AddRunning() || !SV_ContentRoot() ) {
		return;
	}
	if ( !gameDir ) {
		gameDir = FS_GetCurrentGameDir();
	}
	// to say so: OFCM_ValidName below refuses these too
	if ( strpbrk( file, "/\\:" ) ) {
		Com_Printf( "content add: give the name of a pak in sv_contentDir, %s, not a path\n", job->root );
		return;
	}
	Q_strncpyz( name, file, sizeof( name ) );
	if ( !COM_CompareExtension( name, ".pk3" ) ) {
		Com_Printf( "content add: %s isn't a pak (.pk3)\n", file );
		return;
	}
	name[strlen( name ) - 4] = '\0';
	if ( !FS_ValidDownloadPak( gameDir, name ) || !OFCM_ValidName( gameDir, name ) ) {
		Com_Printf( "content add: %s/%s.pk3 isn't a name a pak can have for downloads\n", gameDir, name );
		return;
	}
	if ( !Sys_RealPath( va( "%s%c%s", job->root, PATH_SEP, file ), source, sizeof( source ) ) ) {
		Com_Printf( "content add: there's no %s in sv_contentDir, %s\n", file, job->root );
		return;
	}
	if ( !SV_ContentUnder( job->root, source ) ) {
		Com_Printf( "content add: %s is a link out of sv_contentDir, which content add doesn't follow\n", file );
		return;
	}
	// Content_FileStat refuses what's too large for a pak too
	if ( !Content_FileStat( source, &size, &mtime ) ) {
		Com_Printf( "content add: %s can't be read, or is %d MB or larger, which no pak may be\n", file,
			( CONTENT_MAX_PAK_SIZE >> 20 ) + 1 );
		return;
	}
	Com_sprintf( addPak.name, sizeof( addPak.name ), "%s/%s.pk3", gameDir, name );
	Q_strncpyz( addPak.gameDir, gameDir, sizeof( addPak.gameDir ) );
	Q_strncpyz( addPak.base, name, sizeof( addPak.base ) );
	refused = FS_CheckAddPak( source, gameDir, name, &addPak.checksum, overrides, sizeof( overrides ) );
	if ( refused ) {
		Com_Printf( "content add: %s is refused: %s\n", addPak.name, refused );
		return;
	}
	if ( overrides[0] ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: %s\n", overrides );
	}

	free( job->items );
	job->items = calloc( 1, sizeof( *job->items ) );
	if ( !job->buffer ) {
		job->buffer = malloc( EXPORT_BUFFER_SIZE );
	}
	if ( !job->items || !job->buffer ) {
		Com_Printf( "content add: out of memory\n" );
		return;
	}
	item = &job->items[0];
	Q_strncpyz( item->source, source, sizeof( item->source ) );
	Q_strncpyz( item->name, addPak.name, sizeof( item->name ) );
	item->size = size;
	item->mtime = mtime;
	Q_strncpyz( item->targets[EXPORT_BY_NAME].ospath,
		FS_BuildOSPath( Cvar_VariableString( "fs_homedatapath" ), gameDir, va( "%s.pk3", name ) ), MAX_OSPATH );
	// nor into sv_contentDir, by a game directory of its name
	if ( SV_ContentInside( job->root, item->targets[EXPORT_BY_NAME].ospath ) ) {
		Com_Printf( "content add: %s is sv_contentDir, %s, not a game directory\n", gameDir, job->root );
		return;
	}
	FS_CreatePath( item->targets[EXPORT_BY_NAME].ospath );
	job->count = 1;
	job->add = qtrue;
	job->copies = qtrue;
	job->total = size;
	job->done = 0;
	job->bytes = 0;
	exportThread = Sys_StartThread( SV_ExportThread, job );
	if ( !exportThread ) {
		Com_Printf( "content add: this build has no threads to copy with\n" );
		return;
	}
	job->started = Sys_Milliseconds();
	job->nextReport = job->started + EXPORT_REPORT_MSEC;
}

/*
=================
SV_AddFinish

The copy's done: checked again, as the source could have changed since
its check, it's renamed into place, and goes into the pak index with the
hash it was copied with, for the next map's content manifest
=================
*/
static void SV_AddFinish( void ) {
	const exportItem_t		*item = &exportJob.items[0];
	const exportTarget_t	*target = &item->targets[EXPORT_BY_NAME];
	const char				*home = Cvar_VariableString( "fs_homedatapath" ), *root, *refused;
	char					part[MAX_OSPATH + 8], overrides[MAX_STRING_CHARS];
	unsigned int			checksum;
	pakRecord_t				record;

	switch ( target->result ) {
	case EXPORT_THERE:
		Com_Printf( "%s is installed already, the same file\n", addPak.name );
		return;
	case EXPORT_DIFFERENT:
		Com_Printf( "content add: a pak named %s is already installed with different contents; give the new "
			"version its own name so players who have the old one aren't confused\n", addPak.name );
		return;
	case EXPORT_COPIED:
		break;
	default:
		Com_Printf( "content add: %s: %s\n", addPak.name, target->why );
		return;
	}

	Com_sprintf( part, sizeof( part ), "%s.part", target->ospath );
	refused = FS_CheckAddPak( part, addPak.gameDir, addPak.base, &checksum, overrides, sizeof( overrides ) );
	if ( !refused && checksum != addPak.checksum ) {
		refused = "it changed while it was copied";
	}
	if ( !refused && !Sys_Rename( part, target->ospath ) ) {
		refused = "the file can't be renamed into place";
	}
	if ( refused ) {
		remove( part );
		Com_Printf( "content add: %s is refused: %s\n", addPak.name, refused );
		return;
	}

	// the home's root number, as the search path gives it its paks
	// (download-protocol.md section 4.2)
	Com_Memset( &record, 0, sizeof( record ) );
	for ( record.root = 1; record.root <= 255; record.root++ ) {
		root = FS_ContentRootPath( record.root );
		if ( root && !Q_stricmp( root, home ) ) {
			break;
		}
	}
	if ( record.root <= 255 && Content_FileStat( target->ospath, &record.size, &record.mtime ) ) {
		Com_Memcpy( record.sha256, item->sha256, SHA256_DIGEST_SIZE );
		record.pakChecksum = addPak.checksum;
		Q_strncpyz( record.path, addPak.name, sizeof( record.path ) );
		Content_PakIndex_Put( &record );
	}
	addPak.added = qtrue;
	Com_Printf( "%s added; it's served from the next map change\n", addPak.name );
}

/*
=================
SV_Content_f

The content command. What it's told to read or write is in sv_contentDir,
which only the command line sets, so it runs through rcon too; though not
from game code or a pak's config, which Cmd_ExecuteString turns away
=================
*/
void SV_Content_f( void ) {
	const char	*sub = Cmd_Argv( 1 );
	int			argc = Cmd_Argc();

	if ( !Q_stricmp( sub, "export" ) ) {
		qboolean	copies = argc > 2 && !Q_stricmp( Cmd_Argv( argc - 1 ), "copy" );

		if ( argc - copies <= 3 ) {
			SV_ExportStart( argc - copies == 3 ? Cmd_Argv( 2 ) : NULL, copies );
			return;
		}
	} else if ( !Q_stricmp( sub, "add" ) && ( argc == 3 || argc == 4 ) ) {
		SV_AddStart( Cmd_Argv( 2 ), argc == 4 ? Cmd_Argv( 3 ) : NULL );
		return;
	}
	Com_Printf( "usage:\n"
		"content export [folder] [copy]: puts the paks the server sends, but the game's own, in sv_contentDir, "
		"or a folder of that name in it, by hash (sha256/<hash>.pk3) and by name (<game>/<name>.pk3), for a "
		"web server that sv_dlURL names; hard-linked where it can be, unless copy says to copy\n"
		"content add <file> [game]: checks a pak, a file in sv_contentDir, and installs it in the game's "
		"directory (this one's, or game) for the next map\n" );
}
