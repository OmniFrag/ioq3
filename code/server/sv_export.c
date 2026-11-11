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
// (downloads.md section 6). The files are cloned (reflinks), or
// hard-linked, where they can be, else copied, on a thread of their own, so a big set never holds up a
// frame; it reads the disk beside the pak index's hash job (content.h).
// content add checks a pak an operator installs, and clones it into a
// game directory where the disk can, else copies it, on the same thread,
// for the next map (section 11); with a verdict an extension can read
// (hooks.h's SV_ExtContentAdd), as the admin page's uploads do.
// What either is told to read or write is in sv_contentDir, which only
// the command line sets, so rcon may run them; content add then installs
// the pak in the home's game directory, as its name says.

#include "server.h"
#include "../qcommon/content.h"

#include <errno.h>

#define EXPORT_BUFFER_SIZE		( 256 * 1024 )
#define EXPORT_REPORT_MSEC		10000		// between reports of how far it's got
#define EXPORT_MAX_LISTED		8			// paks named in a line of the summary

// sv_contentDir's two folders, apart so serving one never serves the
// other: paks waiting for content add, and content export's, unless it's
// given another
#define CONTENT_INCOMING		"incoming"
#define CONTENT_WWW				"www"
// in an export's folder: the pak versions its last two exports listed
#define EXPORT_RECORD			"content-export.txt"

typedef enum {
	EXPORT_THERE,		// already there, with the pak's hash
	EXPORT_LINKED,
	EXPORT_CLONED,		// a reflink of the pak: its own file, sharing its blocks
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
	int64_t			size, mtime;	// the file's as the export left it, for the record
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
	qboolean		copies;			// never clones or hard links
	qboolean		noClones;		// the folder's disk can't clone, found by the first try
	qboolean		add;			// content add's: its one pak by name alone
	volatile int	done;			// items finished
	volatile int64_t	bytes;		// bytes copied
	// its folder, links resolved, which all its paths must stay in: the
	// export's, or content add's incoming
	char			dir[MAX_OSPATH];
	// what SV_ExportPrune did, for SV_ExportFinish
	int				pruned, prunedKept;		// old versions removed, and kept as a file couldn't be
	int64_t			prunedBytes;
	char			prunedNames[MAX_STRING_CHARS];
	const char		*pruneWhy;				// a constant string, when no version could be removed
	// old versions whose files by hash were written over in place, as a
	// hard link to a pak shares its bytes, removed
	int				dropped;
	char			droppedNames[MAX_STRING_CHARS];
	const char		*keepWhy;				// the frame's: why none may be, as a pak listed is gone

	// the frame's
	int64_t			total;			// the paks' bytes
	int				started, nextReport;
} exportJob_t;

static exportJob_t	exportJob;
static sysThread_t	*exportThread;
static char			exportMadeDir[MAX_OSPATH];	// the last directory SV_ExportMakeDirs made
// sv_contentDir and its incoming folder, links resolved (SV_ContentRoot)
static char			contentRoot[MAX_OSPATH], contentIncoming[MAX_OSPATH];
// why a copy failed for want of room, which content add's verdict names
static const char	exportDiskFull[] = "the disk is nearly full";

static void SV_AddFinish( void );
static void SV_AddEnd( void );

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
		return exportDiskFull;
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

Puts the item's pak at one of its targets, as a .part first: a clone of
the pak (a reflink), its own file sharing the pak's blocks, which writing
over the pak leaves as it was; where the disk can't, a hard link to the
pak, or to the copy by hash; both cost no space; and a copy, checked
against the hash as it's made
=================
*/
static void SV_ExportTarget( exportJob_t *job, exportItem_t *item, int t ) {
	exportTarget_t			*target = &item->targets[t];
	const exportTarget_t	*byHash = &item->targets[EXPORT_BY_HASH];
	char					part[MAX_OSPATH + 8];
	const char				*why;
	int64_t					size, mtime = item->mtime;
	qboolean				cloned, linked;

	// the file by hash an earlier item put there, or the one by name a
	// link to it, as on another disk than the pak's
	if ( t == EXPORT_BY_HASH && item->sameHash >= 0 ) {
		*target = job->items[item->sameHash].targets[EXPORT_BY_HASH];
		if ( target->result != EXPORT_FAILED ) {
			target->result = EXPORT_THERE;
		}
		return;
	}
	if ( !SV_ContentInside( job->dir, target->ospath ) ) {
		target->result = EXPORT_FAILED;
		target->why = "a link there leads out of the export's folder";
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
	cloned = qfalse;
	if ( !job->copies && !job->noClones ) {
		cloned = Sys_CloneFile( item->source, part ) || ( t == EXPORT_BY_NAME
			&& byHash->result != EXPORT_FAILED && Sys_CloneFile( byHash->ospath, part ) );
		// a folder whose disk can't clone: the rest of the job hard-links
		// without trying; not for a pak on another disk (EXDEV) or one
		// that failed on its own (unreadable, the disk full)
		if ( !cloned && ( errno == EOPNOTSUPP || errno == ENOTSUP || errno == ENOTTY
			|| errno == EINVAL || errno == ENOSYS ) ) {
			job->noClones = qtrue;
		}
	}
	linked = !job->copies && !cloned && ( Sys_HardLink( item->source, part ) || ( t == EXPORT_BY_NAME
		&& byHash->result != EXPORT_FAILED && Sys_HardLink( byHash->ospath, part ) ) );
	if ( cloned || linked ) {
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
		target->result = cloned ? EXPORT_CLONED : linked ? EXPORT_LINKED : EXPORT_COPIED;
	}
}

// and the file's size and time as it's left, which the record keeps
static void SV_ExportTargetStat( exportJob_t *job, exportItem_t *item, int t ) {
	exportTarget_t	*target = &item->targets[t];

	SV_ExportTarget( job, item, t );
	if ( target->result == EXPORT_FAILED || !Content_FileStat( target->ospath, &target->size, &target->mtime ) ) {
		target->size = target->mtime = -1;
	}
}

/*
=================
SV_AddTarget

content add's pak, beside its place in its game directory as a .part: a
clone of it (a reflink) where the disk can, which takes no space until
either is written, hashed by reading it, else a copy hashed as it's made;
which the frame checks again and renames into place (SV_AddFinish).
Unless a pak of its name is there already, which is the same file (its
size, then its hash) or refused
=================
*/
static void SV_AddTarget( exportJob_t *job, exportItem_t *item ) {
	exportTarget_t	*target = &item->targets[EXPORT_BY_NAME];
	char			part[MAX_OSPATH + 8];
	byte			digest[SHA256_DIGEST_SIZE];
	const char		*why;
	int64_t			size, mtime;
	qboolean		cloned;

	if ( !SV_ContentInside( job->dir, item->source ) ) {
		target->result = EXPORT_FAILED;
		target->why = "a link there leads out of " CONTENT_INCOMING;
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
	cloned = Sys_CloneFile( item->source, part );
	if ( cloned ) {
		why = !Content_HashFile( part, item->sha256, &size ) ? "the pak can't be read"
			: size != item->size ? "the pak changed since it was hashed" : NULL;
	} else {
		why = SV_ExportCopy( job, item, part );
	}
	if ( why ) {
		remove( part );
		target->result = EXPORT_FAILED;
		target->why = why;
	} else {
		target->result = cloned ? EXPORT_CLONED : EXPORT_COPIED;
	}
}

/*
=================
SV_ExportList

Adds a pak's name to a line of names, the first few, then a count of the
rest at the end (SV_ExportListEnd); the thread's too
=================
*/
static void SV_ExportList( char *line, int size, int *count, const char *name ) {
	if ( (*count)++ < EXPORT_MAX_LISTED ) {
		if ( line[0] ) {
			Q_strcat( line, size, ", " );
		}
		Q_strcat( line, size, name );
	}
}

/*
=================
SV_ExportPaths

A pak version's two files in the export's folder, by hash and by name:
false when one would be too long, with its .part beside it
=================
*/
static qboolean SV_ExportPaths( const char *dir, const byte sha256[SHA256_DIGEST_SIZE], const char *gameDir,
	const char *base, char *byHash, char *byName ) {
	char	hex[SHA256_HEX_SIZE];

	// measured first: Com_sprintf says when it cuts, which the thread mustn't
	if ( strlen( dir ) + 8 + SHA256_HEX_SIZE + 4 >= MAX_OSPATH - 8
		|| strlen( dir ) + strlen( gameDir ) + strlen( base ) + 6 >= MAX_OSPATH - 8 ) {
		return qfalse;
	}
	Com_SHA256Hex( sha256, hex );
	Com_sprintf( byHash, MAX_OSPATH, "%s%csha256%c%s.pk3", dir, PATH_SEP, PATH_SEP, hex );
	Com_sprintf( byName, MAX_OSPATH, "%s%c%s%c%s.pk3", dir, PATH_SEP, gameDir, PATH_SEP, base );
	return qtrue;
}

/*
==============================================================

THE RECORD

An export's folder keeps the pak versions its last export listed and the
ones the export before it listed that this one doesn't (EXPORT_RECORD), a
line each: "current" or "previous", the hash, and the pak's name. A
client that read the content manifest before the paks changed may still
be fetching an old version, by hash or by name, so both sets' files stay.
An export that lists something else makes its versions current, the last
current ones previous, and removes the files of the previous ones before
those: the file by hash, unless a version kept has its hash, and the file
by name, unless a version kept has its name, whose file that is. One that
lists the same versions changes nothing. Only files an export listed go;
what an operator put in the folder never does. On the thread, once the
export's files are all there.

Each line has the file by hash's size and time too. A hard link shares a
pak's bytes (a clone has its own), so a pak written over in place changes
the old version's exported file with it, which would then serve bytes that aren't its
hash's. A version this export doesn't list whose file by hash has
another size or time is removed and dropped from the record, said in a
warning. One whose hash this export lists has its file by hash checked
and made again from that pak, as every export's are

==============================================================
*/

typedef struct {
	byte		sha256[SHA256_DIGEST_SIZE];
	char		gameDir[OFCM_GAMEDIR_SIZE], base[OFCM_NAME_SIZE];
	int64_t		size, mtime;	// its file by hash's as the export left it, -1 unknown
	qboolean	previous;
} exportVersion_t;

static int QDECL SV_ExportHashOrder( const void *a, const void *b ) {
	return memcmp( ( (const exportVersion_t *)a )->sha256, ( (const exportVersion_t *)b )->sha256, SHA256_DIGEST_SIZE );
}

static int QDECL SV_ExportNameOrder( const void *a, const void *b ) {
	const exportVersion_t	*x = *(exportVersion_t * const *)a, *y = *(exportVersion_t * const *)b;
	int						order = strcmp( x->gameDir, y->gameDir );

	return order ? order : strcmp( x->base, y->base );
}

// a name's file, which on Windows' and the Mac's disks is any case's
static int QDECL SV_ExportFileOrder( const void *a, const void *b ) {
	const exportVersion_t	*x = *(exportVersion_t * const *)a, *y = *(exportVersion_t * const *)b;
	int						order = Q_stricmp( x->gameDir, y->gameDir );

	return order ? order : Q_stricmp( x->base, y->base );
}

// by hash, then name: a version, kept current or previous
static int QDECL SV_ExportPairOrder( const void *a, const void *b ) {
	const exportVersion_t	*x = a, *y = b;
	int						order = SV_ExportHashOrder( a, b );

	return order ? order : SV_ExportNameOrder( &x, &y );
}

// and current before previous, which the pair's searches can use too
static int QDECL SV_ExportVersionOrder( const void *a, const void *b ) {
	int		order = SV_ExportPairOrder( a, b );

	return order ? order : ( (const exportVersion_t *)a )->previous - ( (const exportVersion_t *)b )->previous;
}

/*
=================
SV_ExportSplit

A pak's name as the export and the record write it, "<gamedir>/<name>.pk3",
into its parts, as content add takes a name: false if it isn't one
=================
*/
static qboolean SV_ExportSplit( const char *name, char *gameDir, char *base ) {
	char	copy[OFCM_GAMEDIR_SIZE + OFCM_NAME_SIZE + 8], *sep;

	if ( strlen( name ) >= sizeof( copy ) || !COM_CompareExtension( name, ".pk3" ) ) {
		return qfalse;
	}
	Q_strncpyz( copy, name, sizeof( copy ) );
	copy[strlen( copy ) - 4] = '\0';
	sep = strchr( copy, '/' );
	if ( !sep || sep - copy >= OFCM_GAMEDIR_SIZE || strlen( sep + 1 ) >= OFCM_NAME_SIZE ) {
		return qfalse;
	}
	*sep = '\0';
	Q_strncpyz( gameDir, copy, OFCM_GAMEDIR_SIZE );
	Q_strncpyz( base, sep + 1, OFCM_NAME_SIZE );
	return OFCM_ValidName( gameDir, base );
}

/*
=================
SV_ExportReadRecord

The record's versions, sorted, each once; NULL with none, or the record
unreadable, which leaves none to remove
=================
*/
static exportVersion_t *SV_ExportReadRecord( const char *path, int *count ) {
	exportVersion_t	*versions = NULL, *grown, *v;
	// the rest of a line: the size and time, then the name
	char			line[512], kind[16], hex[SHA256_HEX_SIZE + 1], name[2 * 21 + OFCM_GAMEDIR_SIZE + OFCM_NAME_SIZE + 8];
	char			format[32], *rest;
	FILE			*f = Sys_FOpen( path, "r" );
	int				size = 0, i, n = 0, skip;
	long long		fileSize, fileTime;

	*count = 0;
	if ( !f ) {
		return NULL;
	}
	// the name, which may have spaces, is the rest of the line
	Com_sprintf( format, sizeof( format ), "%%15s %%65s %%%d[^\r\n]", (int)sizeof( name ) - 1 );
	while ( fgets( line, sizeof( line ), f ) ) {
		if ( n == size ) {
			grown = realloc( versions, ( size * 2 + 64 ) * sizeof( *versions ) );
			if ( !grown ) {
				break;
			}
			versions = grown;
			size = size * 2 + 64;
		}
		v = &versions[n];
		if ( line[0] == '#' ) {
			continue;
		}
		if ( sscanf( line, format, kind, hex, name ) != 3 ) {
			continue;
		}
		// the size and time, which a record before them lacks
		rest = name;
		if ( sscanf( name, "%lld %lld %n", &fileSize, &fileTime, &skip ) == 2 && name[skip] ) {
			v->size = fileSize;
			v->mtime = fileTime;
			rest = name + skip;
		} else {
			v->size = v->mtime = -1;
		}
		if ( !Com_SHA256FromHex( hex, v->sha256 ) || !SV_ExportSplit( rest, v->gameDir, v->base ) ) {
			continue;
		}
		v->previous = !strcmp( kind, "previous" );
		if ( v->previous || !strcmp( kind, "current" ) ) {
			n++;
		}
	}
	fclose( f );
	if ( !n ) {
		free( versions );
		return NULL;
	}
	qsort( versions, n, sizeof( *versions ), SV_ExportVersionOrder );
	*count = 1;
	for ( i = 1; i < n; i++ ) {
		// one current and previous both: current, which sorts first
		if ( SV_ExportPairOrder( &versions[i], &versions[*count - 1] ) ) {
			versions[( *count )++] = versions[i];
		}
	}
	return versions;
}

/*
=================
SV_ExportRemove

Removes an old version's file, if it's there and in the export's folder:
true when it's gone, adding its size to the job's
=================
*/
static qboolean SV_ExportRemove( exportJob_t *job, const char *path ) {
	int64_t		size, mtime;

	// one that can't be looked at stays listed
	errno = 0;
	if ( !Content_FileStat( path, &size, &mtime ) ) {
		return errno == ENOENT;
	}
	if ( SV_ContentInside( job->dir, path ) && !remove( path ) ) {
		job->prunedBytes += size;
		return qtrue;
	}
	return qfalse;
}

/*
=================
SV_ExportFindName

The version kept of a name, in now's versions sorted by name, in any
case, as the disk may take it
=================
*/
static qboolean SV_ExportFindName( exportVersion_t **byName, int count, const exportVersion_t *v ) {
	return bsearch( &v, byName, count, sizeof( *byName ), SV_ExportFileOrder ) != NULL;
}

/*
=================
SV_ExportWrittenOver

Whether an old version this export doesn't list had its file by hash
written over in place, its size or time not the record's: then its files
are removed, and it's to be dropped. One whose size isn't known yet takes
its file's. One whose file by hash can't be removed stays listed, and the
next export tries again. dropped is the last version dropped, with its
file's size and time, or a size of -1: another name of its hash is
dropped too, its file by hash gone with it. changed says a version did
=================
*/
static qboolean SV_ExportWrittenOver( exportJob_t *job, exportVersion_t *v, const exportVersion_t *cur, int numCurrent,
	exportVersion_t **curByName, exportVersion_t *dropped, qboolean *changed ) {
	char		byHash[MAX_OSPATH], byNameFile[MAX_OSPATH], name[OFCM_GAMEDIR_SIZE + OFCM_NAME_SIZE + 8];
	int64_t		size, mtime, nameSize, nameTime;

	// a hash this export lists has its file by hash from this export
	if ( bsearch( v, cur, numCurrent, sizeof( *cur ), SV_ExportHashOrder )
		|| !SV_ExportPaths( job->dir, v->sha256, v->gameDir, v->base, byHash, byNameFile ) ) {
		return qfalse;
	}
	if ( dropped->size >= 0 && !SV_ExportHashOrder( v, dropped ) ) {
		size = dropped->size;
		mtime = dropped->mtime;
	} else {
		if ( !Content_FileStat( byHash, &size, &mtime ) ) {
			return qfalse;
		}
		if ( v->size < 0 ) {
			v->size = size;
			v->mtime = mtime;
			*changed = qtrue;
			return qfalse;
		}
		if ( size == v->size && mtime == v->mtime ) {
			return qfalse;
		}
		if ( !SV_ContentInside( job->dir, byHash ) || remove( byHash ) ) {
			job->prunedKept++;
			return qfalse;
		}
		*dropped = *v;
		dropped->size = size;
		dropped->mtime = mtime;
	}
	*changed = qtrue;
	// its file by name too, if no version this export lists has the name
	// and it's the same bytes written over
	if ( !SV_ExportFindName( curByName, numCurrent, v ) && Content_FileStat( byNameFile, &nameSize, &nameTime )
		&& nameSize == size && nameTime == mtime && SV_ContentInside( job->dir, byNameFile ) ) {
		remove( byNameFile );
	}
	Com_sprintf( name, sizeof( name ), "%s/%s.pk3", v->gameDir, v->base );
	SV_ExportList( job->droppedNames, sizeof( job->droppedNames ), &job->dropped, name );
	return qtrue;
}

/*
=================
SV_ExportCheckOld

The old versions this export doesn't list whose files by hash were
written over in place: their files removed, and they dropped from old
(SV_ExportWrittenOver). True if old changed
=================
*/
static qboolean SV_ExportCheckOld( exportJob_t *job, exportVersion_t *old, int *numOld, const exportVersion_t *cur,
	int numCurrent, exportVersion_t **curByName ) {
	exportVersion_t	dropped;
	int				i, kept = 0;
	qboolean		changed = qfalse;

	// old is sorted by hash, so a hash's names are side by side
	dropped.size = -1;
	for ( i = 0; i < *numOld; i++ ) {
		if ( !SV_ExportWrittenOver( job, &old[i], cur, numCurrent, curByName, &dropped, &changed ) ) {
			old[kept++] = old[i];
		}
	}
	*numOld = kept;
	return changed;
}

/*
=================
SV_ExportPrune

After an export: the record anew, and the files of the versions only
older exports listed removed (see THE RECORD). One where a pak failed, or
whose content manifest lists one that's gone, merges instead: what it
listed is current beside what was, and nothing goes, so its files are
removed later and the gone pak's stay while clients may fetch them
=================
*/
static void SV_ExportPrune( exportJob_t *job, qboolean merge ) {
	exportVersion_t	*old, *now = NULL, **byName = NULL, *v;
	char			record[MAX_OSPATH], part[MAX_OSPATH + 8], byHash[MAX_OSPATH], byNameFile[MAX_OSPATH];
	char			hex[SHA256_HEX_SIZE], *text = NULL, name[OFCM_GAMEDIR_SIZE + OFCM_NAME_SIZE + 8];
	int				numOld, numNow = 0, numCurrent, numKept, i, j, length = 0;
	qboolean		gone, same, fresh, written, changed;
	FILE			*f;

	job->pruneWhy = NULL;
	// measured first: Com_sprintf says when it cuts, which the thread mustn't
	if ( strlen( job->dir ) + sizeof( EXPORT_RECORD ) + 1 > sizeof( record ) ) {
		job->pruneWhy = "its record, " EXPORT_RECORD ", is too long a path";
		return;
	}
	Com_sprintf( record, sizeof( record ), "%s%c" EXPORT_RECORD, job->dir, PATH_SEP );
	if ( !SV_ContentInside( job->dir, record ) ) {
		job->pruneWhy = "its record, " EXPORT_RECORD ", is a link out of the folder";
		return;
	}
	old = SV_ExportReadRecord( record, &numOld );
	// this export's versions, then the last's this one doesn't list, and
	// those whose files can't be removed
	now = malloc( ( job->count + numOld + 1 ) * sizeof( *now ) );
	byName = malloc( ( job->count + numOld + 1 ) * sizeof( *byName ) );
	if ( !now || !byName ) {
		job->pruneWhy = "out of memory";
		goto done;
	}
	for ( i = 0; i < job->count; i++ ) {
		v = &now[numNow];
		Com_Memcpy( v->sha256, job->items[i].sha256, SHA256_DIGEST_SIZE );
		v->previous = qfalse;
		// its file by hash as this export left it
		v->size = job->items[i].targets[EXPORT_BY_HASH].size;
		v->mtime = job->items[i].targets[EXPORT_BY_HASH].mtime;
		if ( SV_ExportSplit( job->items[i].name, v->gameDir, v->base ) ) {
			numNow++;
		}
	}
	numCurrent = numNow;
	qsort( now, numCurrent, sizeof( *now ), SV_ExportVersionOrder );
	for ( i = 0; i < numCurrent; i++ ) {
		byName[i] = &now[i];
	}
	qsort( byName, numCurrent, sizeof( *byName ), SV_ExportFileOrder );
	changed = SV_ExportCheckOld( job, old, &numOld, now, numCurrent, byName );

	// the same versions as the record's current ones: nothing to do, or,
	// with a file's size or time new or a version dropped, the record
	// anew, merged so the previous versions stay and nothing more goes
	same = !merge;
	fresh = !changed;
	for ( i = j = 0; same && i < numOld; i++ ) {
		if ( !old[i].previous ) {
			same = j < numCurrent && !SV_ExportVersionOrder( &old[i], &now[j] );
			fresh = fresh && same && old[i].size == now[j].size && old[i].mtime == now[j].mtime;
			j++;
		}
	}
	if ( same && j == numCurrent ) {
		if ( fresh ) {
			goto done;
		}
		merge = qtrue;
	}

	for ( i = 0; i < numOld; i++ ) {
		if ( ( merge || !old[i].previous ) && !bsearch( &old[i], now, numCurrent, sizeof( *now ), SV_ExportPairOrder ) ) {
			now[numNow] = old[i];
			now[numNow++].previous = merge ? old[i].previous : qtrue;
		}
	}
	qsort( now, numNow, sizeof( *now ), SV_ExportVersionOrder );
	for ( i = 0; i < numNow; i++ ) {
		byName[i] = &now[i];
	}
	qsort( byName, numNow, sizeof( *byName ), SV_ExportFileOrder );

	numKept = numNow;
	for ( i = 0; !merge && i < numOld; i++ ) {
		v = &old[i];
		if ( !v->previous || bsearch( v, now, numNow, sizeof( *now ), SV_ExportPairOrder ) ) {
			continue;
		}
		if ( !SV_ExportPaths( job->dir, v->sha256, v->gameDir, v->base, byHash, byNameFile ) ) {
			continue;
		}
		gone = qtrue;
		if ( !bsearch( v, now, numNow, sizeof( *now ), SV_ExportHashOrder ) ) {
			gone = SV_ExportRemove( job, byHash );
		}
		if ( !SV_ExportFindName( byName, numNow, v ) ) {
			gone = SV_ExportRemove( job, byNameFile ) && gone;
		}
		if ( gone ) {
			Com_sprintf( name, sizeof( name ), "%s/%s.pk3", v->gameDir, v->base );
			SV_ExportList( job->prunedNames, sizeof( job->prunedNames ), &job->pruned, name );
		} else {
			// listed again, so the next export tries again
			now[numKept++] = *v;
			job->prunedKept++;
		}
	}

	// the record anew, written whole
	// a line: its kind, the hash, two numbers of at most 20 digits, the name
	text = malloc( 256 + numKept * ( 9 + SHA256_HEX_SIZE + 2 * 21 + sizeof( name ) ) );
	if ( !text ) {
		job->pruneWhy = "out of memory";
		goto done;
	}
	length = Com_sprintf( text, 256, "# content export's record of the pak versions it listed: the last "
		"export's (current) and those the one before listed (previous), whose files it keeps\n" );
	for ( i = 0; i < numKept; i++ ) {
		Com_SHA256Hex( now[i].sha256, hex );
		length += sprintf( text + length, "%s %s %lld %lld %s/%s.pk3\n", now[i].previous ? "previous" : "current", hex,
			(long long)now[i].size, (long long)now[i].mtime, now[i].gameDir, now[i].base );
	}
	// as the paks' files are, with stdio and Sys_ calls alone: a .part of
	// its own, made new so no link there is followed, renamed into place
	Com_sprintf( part, sizeof( part ), "%s.part", record );
	remove( part );
	f = Sys_FCreateReadable( part );
	written = f && (int)fwrite( text, 1, length, f ) == length;
	if ( f && fclose( f ) != 0 ) {
		written = qfalse;
	}
	if ( !written || !Sys_Rename( part, record ) ) {
		remove( part );
		job->pruneWhy = "its record, " EXPORT_RECORD ", can't be written";
	}

done:
	free( text );
	free( byName );
	free( now );
	free( old );
}

static void SV_ExportThread( void *arg ) {
	exportJob_t	*job = arg;
	int			i;

	if ( job->add ) {
		SV_AddTarget( job, &job->items[0] );
		job->done = 1;
		return;
	}
	job->pruned = job->prunedKept = job->dropped = 0;
	job->prunedBytes = 0;
	job->prunedNames[0] = job->droppedNames[0] = '\0';
	for ( i = 0; i < job->count; i++ ) {
		SV_ExportTargetStat( job, &job->items[i], EXPORT_BY_HASH );
		SV_ExportTarget( job, &job->items[i], EXPORT_BY_NAME );
		job->done = i + 1;
	}
	// old versions go only once every pak's files are there, which a
	// client might otherwise fetch the last version of instead, and every
	// pak the content manifest lists is exported
	for ( i = 0; i < job->count; i++ ) {
		if ( job->items[i].targets[EXPORT_BY_HASH].result == EXPORT_FAILED
			|| job->items[i].targets[EXPORT_BY_NAME].result == EXPORT_FAILED ) {
			break;
		}
	}
	SV_ExportPrune( job, i < job->count || job->keepWhy );
	if ( !job->pruneWhy ) {
		job->pruneWhy = i < job->count ? "some paks failed" : job->keepWhy;
	}
}

/*
==============================================================

THE FRAME

==============================================================
*/

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
	char			failed[MAX_STRING_CHARS] = "", removed[MAX_STRING_CHARS + 64];
	int				i, t, there = 0, cloned = 0, linked = 0, copied = 0, numFailed = 0;
	int64_t			copiedBytes = 0;

	for ( i = 0; i < job->count; i++ ) {
		item = &job->items[i];
		for ( t = 0; t < 2; t++ ) {
			switch ( item->targets[t].result ) {
			case EXPORT_THERE:
				there++;
				break;
			case EXPORT_CLONED:
				cloned++;
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

	// what SV_ExportPrune did, on the thread
	removed[0] = '\0';
	if ( numFailed ) {
		Q_strncpyz( removed, "; old versions' files are kept until an export where none fail", sizeof( removed ) );
	} else if ( job->pruneWhy ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: content export: no old version's files are removed: %s\n", job->pruneWhy );
	}
	if ( job->dropped ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: content export: %d old version%s file by hash had changed, as when a pak "
			"is written over in place, which a hard link shares, so %s removed and no longer served: %s. Move a new "
			"version into place instead of writing over the pak (downloads.md section 6)\n", job->dropped,
			job->dropped == 1 ? "'s" : "s'", job->dropped == 1 ? "it's" : "they're",
			SV_ExportListEnd( job->droppedNames, job->dropped ) );
	}
	if ( job->prunedKept ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: content export: %d old versions' files can't be removed; the next "
			"export that lists other versions tries again\n", job->prunedKept );
	}
	if ( job->pruned ) {
		Com_sprintf( removed, sizeof( removed ), "; removed %d old version%s (%lld MB of files): %s", job->pruned,
			job->pruned == 1 ? "" : "s", (long long)( job->prunedBytes >> 20 ),
			SV_ExportListEnd( job->prunedNames, job->pruned ) );
	}
	Com_Printf( "Exported %d paks to %s in %d s: %d files copied (%lld MB), %d cloned, %d hard-linked, %d already there%s%s\n",
		job->count, job->dir, ( Sys_Milliseconds() - job->started ) / 1000, copied,
		(long long)( copiedBytes >> 20 ), cloned, linked, there,
		numFailed ? va( "; %d paks failed: %s", numFailed, SV_ExportListEnd( failed, numFailed ) ) : "", removed );
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
		SV_AddEnd();
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
	if ( !strcmp( path, exportMadeDir ) ) {
		return qtrue;
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
				Com_Printf( "content: %s leads out of %s, through a link\n", path, root );
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
SV_ContentFolder

A folder in sv_contentDir, made if it isn't there, and its path with its
links resolved, which must be in sv_contentDir; qfalse, saying why, if
it isn't
=================
*/
static qboolean SV_ContentFolder( const char *name, char *real, int size ) {
	char	path[MAX_OSPATH];

	// cut short, it would name another folder
	if ( Com_sprintf( path, sizeof( path ), "%s%c%s", contentRoot, PATH_SEP, name ) >= (int)sizeof( path )
		|| !Sys_MkdirReadable( path ) || !Sys_RealPath( path, real, size ) ) {
		Com_Printf( "content: can't make %s\n", path );
		return qfalse;
	}
	if ( !SV_ContentUnder( contentRoot, real ) ) {
		Com_Printf( "content: %s leads out of sv_contentDir, through a link\n", path );
		return qfalse;
	}
	return qtrue;
}

/*
=================
SV_ContentRoot

sv_contentDir, made the first time it's used, in a directory that's
there, with its links resolved, which all the content command opens must
be in; and its incoming and www folders. Incoming is itself, not a link
to another folder; www, which only an export uses, may be one, which the
export checks
=================
*/
static qboolean SV_ContentRoot( void ) {
	const char	*dir = sv_contentDir->string;
	size_t		length;

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
	if ( !Sys_MkdirReadable( dir ) || !Sys_RealPath( dir, contentRoot, sizeof( contentRoot ) ) ) {
		Com_Printf( "content: can't make sv_contentDir, %s\n", dir );
		return qfalse;
	}
	Sys_MkdirReadable( va( "%s%c" CONTENT_WWW, contentRoot, PATH_SEP ) );
	if ( !SV_ContentFolder( CONTENT_INCOMING, contentIncoming, sizeof( contentIncoming ) ) ) {
		return qfalse;
	}
	// in any case, as the disk may take it; a root of "/" or "C:\" ends
	// in its separator
	length = strlen( contentRoot );
	if ( Q_stricmp( contentIncoming, va( contentRoot[length - 1] == PATH_SEP ? "%s" CONTENT_INCOMING
		: "%s%c" CONTENT_INCOMING, contentRoot, PATH_SEP ) ) ) {
		Com_Printf( "content: sv_contentDir's " CONTENT_INCOMING " is a link, %s; make it a folder of its own\n",
			contentIncoming );
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
	// its verdict, for the extension that started it (SV_ExtContentAdd),
	// and the file in incoming removed as it ends, or ""
	extContentVerdict_t	verdict;
	extContentDone_t	done;
	void			*user;
	char			removing[MAX_OSPATH];
} addPak;

#define ADD_NO_CONTENT_DIR	"content add: sv_contentDir can't be used; the console says why"

/*
=================
SV_AddSay

A line of a verdict, as its code: into it, and at the console for content
add's own, not content.check's (SV_ExtContentCheck)
=================
*/
static Q_PRINTF_FUNC( 3, 4 ) void QDECL SV_AddSay( extContentVerdict_t *verdict, const char *code, const char *fmt,
		... ) {
	va_list	argptr;
	char	line[MAXPRINTMSG];

	va_start( argptr, fmt );
	Q_vsnprintf( line, sizeof( line ), fmt, argptr );
	va_end( argptr );
	verdict->code = code;
	Q_strncpyz( verdict->message, line, sizeof( verdict->message ) );
	if ( verdict == &addPak.verdict ) {
		Com_Printf( "%s\n", line );
	}
}

/*
=================
SV_AddEnd

content add has its verdict: the file it was given goes from incoming,
when it was asked to, and the extension that started it is told
=================
*/
static void SV_AddEnd( void ) {
	extContentDone_t	done = addPak.done;

	if ( addPak.removing[0] && remove( addPak.removing ) && errno != ENOENT ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: content add: %s can't be removed from " CONTENT_INCOMING ": %s\n",
			addPak.removing, strerror( errno ) );
	}
	addPak.removing[0] = '\0';
	addPak.done = NULL;
	if ( done ) {
		done( addPak.user, &addPak.verdict );
	}
}

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
SV_ExportSameHashes

Each item's earlier one of the same hash, whose file by hash is its own,
or -1: by the items sorted by hash, then by their place
=================
*/
static int QDECL SV_ExportItemOrder( const void *a, const void *b ) {
	int		x = *(const int *)a, y = *(const int *)b;
	int		order = memcmp( exportJob.items[x].sha256, exportJob.items[y].sha256, SHA256_DIGEST_SIZE );

	return order ? order : x - y;
}

static qboolean SV_ExportSameHashes( exportJob_t *job ) {
	int		*order = malloc( ( job->count + 1 ) * sizeof( *order ) );
	int		i, first = 0;

	if ( !order ) {
		return qfalse;
	}
	for ( i = 0; i < job->count; i++ ) {
		order[i] = i;
		job->items[i].sameHash = -1;
	}
	qsort( order, job->count, sizeof( *order ), SV_ExportItemOrder );
	for ( i = 1; i < job->count; i++ ) {
		if ( memcmp( job->items[order[i]].sha256, job->items[order[first]].sha256, SHA256_DIGEST_SIZE ) ) {
			first = i;
		} else {
			job->items[order[i]].sameHash = order[first];
		}
	}
	free( order );
	return qtrue;
}

/*
=================
SV_ExportStart

content export [name] [copy]: every pak the current content manifest
lists, required and optional, but the game's own, by hash and by name,
in the folder name in sv_contentDir, www if it's not given
=================
*/
static void SV_ExportStart( const char *name, qboolean copies ) {
	exportJob_t		*job = &exportJob;
	const byte		*manifest;
	ofcmHeader_t	header;
	ofcmEntry_t		entry;
	exportItem_t	*item;
	char			retail[MAX_QPATH], refused[MAX_STRING_CHARS] = "";
	const char		*source;
	int				i, length, numRefused = 0, numGone = 0;

	if ( SV_AddRunning() || !SV_ContentRoot() ) {
		return;
	}
	// one folder's name, by the rule for a pak's names, but not incoming's
	if ( !name ) {
		name = CONTENT_WWW;
	}
	if ( !OFCM_ValidName( name, name ) || !Q_stricmp( name, CONTENT_INCOMING ) ) {
		Com_Printf( "content export: %s isn't a name for a folder in sv_contentDir; give one name, with no "
			"path, of the characters a pak's name can have; not " CONTENT_INCOMING "\n", name );
		return;
	}
	// which, through a link, mustn't be incoming, in it, or around it
	if ( !SV_ContentFolder( name, job->dir, sizeof( job->dir ) ) ) {
		return;
	}
	if ( SV_ContentUnder( contentIncoming, job->dir ) || SV_ContentUnder( job->dir, contentIncoming ) ) {
		Com_Printf( "content export: %s leads to " CONTENT_INCOMING ", %s, through a link\n", name, job->dir );
		return;
	}
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
	if ( !SV_ExportMakeDirs( job->dir, va( "%s%csha256%cx", job->dir, PATH_SEP, PATH_SEP ) ) ) {
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
		if ( !SV_ExportPaths( job->dir, entry.sha256, entry.gameDir, entry.name, item->targets[EXPORT_BY_HASH].ospath,
				item->targets[EXPORT_BY_NAME].ospath ) ) {
			Com_Printf( "content export: %s is too long a path for the files\n", job->dir );
			return;
		}
		if ( !SV_ExportMakeDirs( job->dir, item->targets[EXPORT_BY_NAME].ospath ) ) {
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
	if ( !SV_ExportSameHashes( job ) ) {
		Com_Printf( "content export: out of memory\n" );
		return;
	}

	// one gone is still the content manifest's, which clients may fetch
	// from the export's last files
	job->keepWhy = numGone ? "some paks the content manifest lists are gone from the server" : NULL;
	job->copies = copies;
	job->noClones = qfalse;
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
SV_AddName

content add's pak by its name in incoming, without .pk3 in base, and its
game directory: a name, not a path, a pak's that downloads may have;
qfalse, saying why in the verdict
=================
*/
static qboolean SV_AddName( extContentVerdict_t *verdict, const char *file, const char *gameDir, char *base, int size ) {
	// to say so: OFCM_ValidName below refuses these too
	if ( strpbrk( file, "/\\:" ) ) {
		SV_AddSay( verdict, "bad_name", "content add: give the name of a pak in %s, not a path", contentIncoming );
		return qfalse;
	}
	Q_strncpyz( base, file, size );
	if ( !COM_CompareExtension( base, ".pk3" ) ) {
		SV_AddSay( verdict, "bad_name", "content add: %s isn't a pak (.pk3)", file );
		return qfalse;
	}
	base[strlen( base ) - 4] = '\0';
	if ( !FS_ValidDownloadPak( gameDir, base ) || !OFCM_ValidName( gameDir, base ) ) {
		// the game directory's fault, if a plain pak's name passes with it:
		// both rules check each part alone
		SV_AddSay( verdict, FS_ValidDownloadPak( gameDir, "x" ) && OFCM_ValidName( gameDir, "x" ) ? "bad_name" : "bad_game",
			"content add: %s/%s.pk3 isn't a name a pak can have for downloads", gameDir, base );
		return qfalse;
	}
	return qtrue;
}

/*
=================
SV_AddPlace

Where content add puts a pak, in the home's game directory, into ospath;
qfalse, saying so, for a game directory that's sv_contentDir
=================
*/
static qboolean SV_AddPlace( extContentVerdict_t *verdict, const char *gameDir, const char *base, char *ospath,
		int size ) {
	Q_strncpyz( ospath, FS_BuildOSPath( Cvar_VariableString( "fs_homedatapath" ), gameDir, va( "%s.pk3", base ) ), size );
	// nor into sv_contentDir, by a game directory of its name
	if ( SV_ContentInside( contentRoot, ospath ) ) {
		SV_AddSay( verdict, "bad_game", "content add: %s is sv_contentDir, %s, not a game directory", gameDir, contentRoot );
		return qfalse;
	}
	return qtrue;
}

/*
=================
SV_AddBegin

content add <file> [gamedir]: checks the pak, a file in sv_contentDir's
incoming folder (FS_CheckAddPak), then clones or copies it on the thread
(SV_AddTarget). qtrue when the thread has started, whose end ends the add
(SV_ExportFrame); else its verdict is said. With removeSource, the file
goes from incoming as the add ends
=================
*/
static qboolean SV_AddBegin( const char *file, const char *gameDir, qboolean removeSource ) {
	exportJob_t			*job = &exportJob;
	exportItem_t		*item;
	extContentVerdict_t	*verdict = &addPak.verdict;
	char				name[MAX_OSPATH], source[MAX_OSPATH], overrides[MAX_STRING_CHARS];
	const char			*refused;
	int64_t				size, mtime;
	FILE				*f;

	Com_Memset( verdict, 0, sizeof( *verdict ) );
	verdict->size = -1;
	addPak.removing[0] = '\0';
	if ( !SV_ContentRoot() ) {
		// its lines said why
		verdict->code = "failed";
		Q_strncpyz( verdict->message, ADD_NO_CONTENT_DIR, sizeof( verdict->message ) );
		return qfalse;
	}
	if ( !gameDir ) {
		gameDir = FS_GetCurrentGameDir();
	}
	if ( !SV_AddName( verdict, file, gameDir, name, sizeof( name ) ) ) {
		return qfalse;
	}
	if ( removeSource ) {
		Com_sprintf( addPak.removing, sizeof( addPak.removing ), "%s%c%s", contentIncoming, PATH_SEP, file );
	}
	if ( !Sys_RealPath( va( "%s%c%s", contentIncoming, PATH_SEP, file ), source, sizeof( source ) ) ) {
		SV_AddSay( verdict, "failed", "content add: there's no %s in %s", file, contentIncoming );
		return qfalse;
	}
	if ( !SV_ContentUnder( contentIncoming, source ) ) {
		SV_AddSay( verdict, "failed", "content add: %s is a link out of %s, which content add doesn't follow", file,
			contentIncoming );
		return qfalse;
	}
	// Content_FileStat refuses what's too large for a pak too, which opens
	if ( !Content_FileStat( source, &size, &mtime ) ) {
		f = Sys_FOpen( source, "rb" );
		if ( f ) {
			fclose( f );
		}
		SV_AddSay( verdict, f ? "too_big" : "failed", "content add: %s can't be read, or is %d MB or larger, which no pak "
			"may be", file, ( CONTENT_MAX_PAK_SIZE >> 20 ) + 1 );
		return qfalse;
	}
	verdict->size = size;
	Com_sprintf( addPak.name, sizeof( addPak.name ), "%s/%s.pk3", gameDir, name );
	Q_strncpyz( addPak.gameDir, gameDir, sizeof( addPak.gameDir ) );
	Q_strncpyz( addPak.base, name, sizeof( addPak.base ) );
	refused = FS_CheckAddPak( source, gameDir, name, &addPak.checksum, overrides, sizeof( overrides ), verdict );
	if ( refused ) {
		SV_AddSay( verdict, verdict->code, "content add: %s is refused: %s", addPak.name, refused );
		return qfalse;
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
		SV_AddSay( verdict, "failed", "content add: out of memory" );
		return qfalse;
	}
	item = &job->items[0];
	Q_strncpyz( item->source, source, sizeof( item->source ) );
	Q_strncpyz( item->name, addPak.name, sizeof( item->name ) );
	item->size = size;
	item->mtime = mtime;
	if ( !SV_AddPlace( verdict, gameDir, name, item->targets[EXPORT_BY_NAME].ospath, MAX_OSPATH ) ) {
		return qfalse;
	}
	FS_CreatePath( item->targets[EXPORT_BY_NAME].ospath );
	// all the thread reads, which it checks again
	Q_strncpyz( job->dir, contentIncoming, sizeof( job->dir ) );
	job->count = 1;
	job->add = qtrue;
	job->copies = qtrue;
	job->total = size;
	job->done = 0;
	job->bytes = 0;
	exportThread = Sys_StartThread( SV_ExportThread, job );
	if ( !exportThread ) {
		SV_AddSay( verdict, "failed", "content add: this build has no threads to copy with" );
		return qfalse;
	}
	job->started = Sys_Milliseconds();
	job->nextReport = job->started + EXPORT_REPORT_MSEC;
	return qtrue;
}

// content add, ended at once with its verdict unless its thread started
static void SV_AddStart( const char *file, const char *gameDir, qboolean removeSource ) {
	if ( !SV_AddBegin( file, gameDir, removeSource ) ) {
		SV_AddEnd();
	}
}

// the home's root number, as the search path gives it its paks
// (download-protocol.md section 4.2), where content add installs; 0 for none
static int SV_AddHomeRoot( void ) {
	const char	*home = Cvar_VariableString( "fs_homedatapath" ), *root;
	int			i;

	for ( i = 1; i <= 255; i++ ) {
		root = FS_ContentRootPath( i );
		if ( root && !Q_stricmp( root, home ) ) {
			return i;
		}
	}
	return 0;
}

/*
=================
SV_AddFinish

The clone or copy is done: checked again, as the source could have
changed since its check, it's renamed into place, and goes into the pak
index with the hash it was read with, for the next map's content manifest
=================
*/
static void SV_AddFinish( void ) {
	const exportItem_t		*item = &exportJob.items[0];
	const exportTarget_t	*target = &item->targets[EXPORT_BY_NAME];
	extContentVerdict_t		*verdict = &addPak.verdict;
	const char				*refused;
	char					part[MAX_OSPATH + 8];
	unsigned int			checksum;
	pakRecord_t				record;

	switch ( target->result ) {
	case EXPORT_THERE:
		Com_SHA256Hex( item->sha256, verdict->sha256 );
		SV_AddSay( verdict, "installed_already", "%s is installed already, the same file", addPak.name );
		return;
	case EXPORT_DIFFERENT:
		SV_AddSay( verdict, "other_contents", "content add: a pak named %s is already installed with different contents; give "
			"the new version its own name so players who have the old one aren't confused", addPak.name );
		return;
	case EXPORT_COPIED:
	case EXPORT_CLONED:
		break;
	default:
		// SV_ExportCopy's, as the disk keeps CACHE_DISK_RESERVE free
		SV_AddSay( verdict, target->why == exportDiskFull ? "no_room" : "failed", "content add: %s: %s", addPak.name,
			target->why );
		return;
	}

	Com_sprintf( part, sizeof( part ), "%s.part", target->ospath );
	verdict->code = NULL;
	refused = FS_CheckAddPak( part, addPak.gameDir, addPak.base, &checksum, NULL, 0, verdict );
	if ( !refused && checksum != addPak.checksum ) {
		refused = "it changed while it was copied";
	}
	if ( !refused && !Sys_Rename( part, target->ospath ) ) {
		refused = "the file can't be renamed into place";
	}
	if ( refused ) {
		remove( part );
		SV_AddSay( verdict, verdict->code ? verdict->code : "failed", "content add: %s is refused: %s", addPak.name,
			refused );
		return;
	}

	Com_Memset( &record, 0, sizeof( record ) );
	record.root = SV_AddHomeRoot();
	if ( record.root && Content_FileStat( target->ospath, &record.size, &record.mtime ) ) {
		Com_Memcpy( record.sha256, item->sha256, SHA256_DIGEST_SIZE );
		record.pakChecksum = addPak.checksum;
		Q_strncpyz( record.path, addPak.name, sizeof( record.path ) );
		Content_PakIndex_Put( &record );
	}
	addPak.added = qtrue;
	Com_SHA256Hex( item->sha256, verdict->sha256 );
	// the extension's file goes as the add ends (SV_AddEnd); else a copy
	// or a clone, so the operator's own stays, which may be on purpose. A
	// clone takes no more space until either file changes
	if ( addPak.removing[0] ) {
		SV_AddSay( verdict, "added", "%s added; it's served from the next map change. Its file in " CONTENT_INCOMING
			" is removed", addPak.name );
	} else if ( target->result == EXPORT_CLONED ) {
		SV_AddSay( verdict, "added", "%s added; it's served from the next map change. The original is still in " CONTENT_INCOMING
			", a clone taking no more space until either file changes; delete it there if you don't keep it",
			addPak.name );
	} else {
		SV_AddSay( verdict, "added", "%s added; it's served from the next map change. The original is still in " CONTENT_INCOMING
			", taking %s more; delete it there if you don't keep it", addPak.name, item->size < ( 1 << 20 )
			? va( "%lld KB", (long long)( ( item->size + 1023 ) >> 10 ) )
			: va( "%lld MB", (long long)( ( item->size + ( 1 << 19 ) ) >> 20 ) ) );
	}
}

/*
=================
SV_ExtContentAdd, SV_ExtContentCheck

content add for an extension (hooks.h): the same add, its verdict to the
extension as it ends; and content.check's, before the pak is in incoming,
which says nothing at the console
=================
*/
qboolean SV_ExtContentAdd( const char *file, const char *gameDir, qboolean removeSource, extContentDone_t done,
		void *user ) {
	if ( exportThread ) {
		return qfalse;
	}
	addPak.done = done;
	addPak.user = user;
	SV_AddStart( file, gameDir, removeSource );
	return qtrue;
}

void SV_ExtContentCheck( const char *file, const char *gameDir, long long size, qboolean hasChecksum,
		unsigned int checksum, extContentVerdict_t *verdict ) {
	char			name[MAX_OSPATH], place[MAX_OSPATH], retail[MAX_QPATH];
	const char		*other;
	const pakRecord_t	*record;
	unsigned int	installed = 0;
	int64_t			installedSize = -1, mtime;

	Com_Memset( verdict, 0, sizeof( *verdict ) );
	verdict->code = "ok";
	verdict->size = size;
	if ( !SV_ContentRoot() ) {
		verdict->code = "failed";
		Q_strncpyz( verdict->message, ADD_NO_CONTENT_DIR, sizeof( verdict->message ) );
		return;
	}
	if ( !gameDir ) {
		gameDir = FS_GetCurrentGameDir();
	}
	if ( !SV_AddName( verdict, file, gameDir, name, sizeof( name ) ) || !SV_AddPlace( verdict, gameDir, name, place,
			sizeof( place ) ) ) {
		return;
	}
	if ( size < 0 || size > CONTENT_MAX_PAK_SIZE ) {
		SV_AddSay( verdict, "too_big", "content add: %s is %d MB or larger, which no pak may be", file,
			( CONTENT_MAX_PAK_SIZE >> 20 ) + 1 );
		return;
	}
	if ( hasChecksum && FS_RetailPakChecksum( va( "%s/%s.pk3", gameDir, name ), checksum, retail, sizeof( retail ) ) ) {
		SV_AddSay( verdict, "retail", "content add: %s/%s.pk3 is refused: " ADD_PAK_RETAIL, gameDir, name, retail );
		return;
	}
	other = FS_AddPakInstalled( gameDir, name, checksum, &installed );
	if ( !other ) {
		// the search path has none, but the add's place may: a pak added
		// since the map loaded, or one of a game directory not loaded. Its
		// size, then the checksum the pak index has for it, as the add put it
		// there; else the rest is left to the add, which hashes it
		if ( !Content_FileStat( place, &installedSize, &mtime ) ) {
			return;
		}
		record = hasChecksum && installedSize == size ? Content_PakIndex_Find( SV_AddHomeRoot(),
			va( "%s/%s.pk3", gameDir, name ), installedSize, mtime ) : NULL;
		if ( installedSize != size || ( record && record->pakChecksum != checksum ) ) {
			SV_AddSay( verdict, "other_contents", "content add: a pak named %s/%s.pk3 is already installed with different "
				"contents; give the new version its own name so players who have the old one aren't confused", gameDir, name );
		} else if ( record ) {
			SV_AddSay( verdict, "installed_already", "%s/%s.pk3 is installed already, the same file", gameDir, name );
		}
		return;
	}
	// the same file, as far as its size and checksum tell, which the add
	// tells by its hash
	Content_FileStat( other, &installedSize, &mtime );
	if ( installedSize != size || ( hasChecksum && installed != checksum ) ) {
		SV_AddSay( verdict, "other_contents", "content add: %s/%s.pk3 is refused: " ADD_PAK_OTHER_CONTENTS, gameDir, name,
			gameDir, name, other );
	} else if ( hasChecksum ) {
		SV_AddSay( verdict, "installed_already", "%s/%s.pk3 is installed already, the same file", gameDir, name );
	}
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
		if ( !SV_AddRunning() ) {
			SV_AddStart( Cmd_Argv( 2 ), argc == 4 ? Cmd_Argv( 3 ) : NULL, qfalse );
		}
		return;
	}
	Com_Printf( "usage:\n"
		"content export [folder] [copy]: puts the paks the server sends, but the game's own, in the folder "
		"in sv_contentDir, " CONTENT_WWW " if it's not given, by hash (sha256/<hash>.pk3) and by name "
		"(<game>/<name>.pk3), for a web server that sv_dlURL names; cloned or hard-linked where it can be, "
		"unless copy says to copy. It keeps the files of the versions it lists and of those the export before it listed, "
		"and removes older ones it made (" EXPORT_RECORD ")\n"
		"content add <file> [game]: checks a pak, a file in sv_contentDir's " CONTENT_INCOMING " folder, and "
		"installs it in the game's directory (this one's, or game) for the next map\n" );
}
