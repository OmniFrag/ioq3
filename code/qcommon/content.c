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
// content.c -- paks known by their content: the pak index, which keeps
// each installed pak's SHA-256, and the job that hashes them

#include "content.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

/*
==============================================================

FILE FORMATS

Little-endian, a 16-byte header and fixed-size records

==============================================================
*/

static void Content_Put16( byte *p, unsigned int v ) {
	p[0] = v & 0xff;
	p[1] = ( v >> 8 ) & 0xff;
}

static void Content_Put32( byte *p, unsigned int v ) {
	p[0] = v & 0xff;
	p[1] = ( v >> 8 ) & 0xff;
	p[2] = ( v >> 16 ) & 0xff;
	p[3] = ( v >> 24 ) & 0xff;
}

static void Content_Put64( byte *p, int64_t v ) {
	Content_Put32( p, (unsigned int)( (uint64_t)v & 0xffffffffu ) );
	Content_Put32( p + 4, (unsigned int)( (uint64_t)v >> 32 ) );
}

static unsigned int Content_Get16( const byte *p ) {
	return p[0] | ( p[1] << 8 );
}

static unsigned int Content_Get32( const byte *p ) {
	return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (unsigned int)p[3] << 24 );
}

static int64_t Content_Get64( const byte *p ) {
	return (int64_t)( Content_Get32( p ) | ( (uint64_t)Content_Get32( p + 4 ) << 32 ) );
}

/*
=================
Content_ParseHeader

The record count of a file with this magic and record size, or -1 if the
header is wrong or the file isn't exactly the header and its records
=================
*/
static int Content_ParseHeader( const byte *data, int length, const char *magic, int recordSize, int maxRecords ) {
	unsigned int	count;

	if ( length < CONTENT_HEADER_SIZE || memcmp( data, magic, 4 ) ||
		Content_Get16( data + 4 ) != CONTENT_VERSION || Content_Get16( data + 6 ) != (unsigned int)recordSize ) {
		return -1;
	}
	count = Content_Get32( data + 8 );
	if ( count > CONTENT_MAX_RECORDS || count > (unsigned int)maxRecords ||
		(int64_t)length != CONTENT_HEADER_SIZE + (int64_t)count * recordSize ) {
		return -1;
	}
	return count;
}

static void Content_WriteHeader( byte *out, const char *magic, int recordSize, int count ) {
	memcpy( out, magic, 4 );
	Content_Put16( out + 4, CONTENT_VERSION );
	Content_Put16( out + 6, recordSize );
	Content_Put32( out + 8, count );
	Content_Put32( out + 12, 0 );
}

/*
=================
Content_ParsePakIndex
=================
*/
int Content_ParsePakIndex( const byte *data, int length, pakRecord_t *records, int maxRecords ) {
	const byte	*p;
	pakRecord_t	*r;
	int			count, i;

	count = Content_ParseHeader( data, length, PAK_INDEX_MAGIC, PAK_INDEX_RECORD_SIZE, maxRecords );
	for ( i = 0; i < count; i++ ) {
		p = data + CONTENT_HEADER_SIZE + i * PAK_INDEX_RECORD_SIZE;
		r = &records[i];
		memcpy( r->sha256, p, SHA256_DIGEST_SIZE );
		r->size = Content_Get64( p + 32 );
		r->mtime = Content_Get64( p + 40 );
		r->pakChecksum = Content_Get32( p + 48 );
		r->flags = p[52];
		r->root = p[53];
		r->lastUsed = Content_Get64( p + 56 );
		if ( !memchr( p + 64, '\0', CONTENT_PATH_SIZE ) || !p[64] || r->size < 0 || !r->root ) {
			return -1;
		}
		Q_strncpyz( r->path, (const char *)p + 64, sizeof( r->path ) );
	}
	return count;
}

/*
=================
Content_WritePakIndex
=================
*/
int Content_WritePakIndex( const pakRecord_t *records, int count, byte *out, int size ) {
	const pakRecord_t	*r;
	byte				*p;
	int					i, length;

	if ( count < 0 || count > CONTENT_MAX_RECORDS ) {
		return -1;
	}
	length = CONTENT_HEADER_SIZE + count * PAK_INDEX_RECORD_SIZE;
	if ( length > size ) {
		return -1;
	}
	Content_WriteHeader( out, PAK_INDEX_MAGIC, PAK_INDEX_RECORD_SIZE, count );
	for ( i = 0; i < count; i++ ) {
		p = out + CONTENT_HEADER_SIZE + i * PAK_INDEX_RECORD_SIZE;
		r = &records[i];
		memset( p, 0, PAK_INDEX_RECORD_SIZE );
		memcpy( p, r->sha256, SHA256_DIGEST_SIZE );
		Content_Put64( p + 32, r->size );
		Content_Put64( p + 40, r->mtime );
		Content_Put32( p + 48, r->pakChecksum );
		p[52] = r->flags;
		p[53] = r->root;
		Content_Put64( p + 56, r->lastUsed );
		Q_strncpyz( (char *)p + 64, r->path, CONTENT_PATH_SIZE );
	}
	return length;
}

/*
=================
Content_FileStat

A file's size and modification time in seconds, as Sys_FileTime gives it,
the key of its record; qfalse if it isn't there or is 2 GB or larger,
which no pak may be. One stat, and no engine state, so the hash job's
thread can call it
=================
*/
qboolean Content_FileStat( const char *ospath, int64_t *size, int64_t *mtime ) {
	struct stat	st;

	if ( stat( ospath, &st ) || st.st_size < 0 || st.st_size > CONTENT_MAX_PAK_SIZE ) {
		return qfalse;
	}
	*size = st.st_size;
	*mtime = st.st_mtime;
	return qtrue;
}

/*
=================
Content_HomeFile

A file under fs_homedatapath, in a static buffer
=================
*/
static const char *Content_HomeFile( const char *file ) {
	return FS_BaseDir_BuildOSPath( Cvar_VariableString( "fs_homedatapath" ), file );
}

/*
=================
Content_RemoveStaleTemps

Deletes the temporary files of index writes that a process stopping
midway left (CONTENT_TEMP_FORMAT's), an hour or more old, so they can't
pile up; once a run
=================
*/
static void Content_RemoveStaleTemps( void ) {
	static qboolean	done;
	char			dir[MAX_OSPATH], **files;
	const char		*name, *c, *ospath;
	int64_t			size, mtime, now = time( NULL );
	int				i, count;

	if ( done || com_check ) {
		return;
	}
	done = qtrue;
	Q_strncpyz( dir, Content_HomeFile( CONTENT_DIR ), sizeof( dir ) );
	files = Sys_ListFiles( dir, ".tmp", NULL, &count, qfalse );
	for ( i = 0; i < count; i++ ) {
		// <name>.dat.<pid>.tmp only
		name = strstr( files[i], ".dat." );
		if ( !name ) {
			continue;
		}
		for ( c = name + 5; *c >= '0' && *c <= '9'; c++ ) {
		}
		if ( c == name + 5 || strcmp( c, ".tmp" ) ) {
			continue;
		}
		ospath = va( "%s%c%s", dir, PATH_SEP, files[i] );
		if ( Content_FileStat( ospath, &size, &mtime ) && mtime < now - 3600 ) {
			remove( ospath );
		}
	}
	Sys_FreeFileList( files );
}

/*
=================
Content_ReadFile

A whole file into a malloc'd buffer, at most maxLength bytes; NULL if
it's missing, unreadable or longer
=================
*/
static byte *Content_ReadFile( const char *ospath, int maxLength, int *length ) {
	FILE	*f = Sys_FOpen( ospath, "rb" );
	byte	*data;
	long	size;

	if ( !f ) {
		return NULL;
	}
	size = fseek( f, 0, SEEK_END ) ? -1 : ftell( f );
	if ( size < 0 || size > maxLength || fseek( f, 0, SEEK_SET ) ) {
		fclose( f );
		return NULL;
	}
	data = malloc( size ? size : 1 );
	if ( data && (long)fread( data, 1, size, f ) != size ) {
		free( data );
		data = NULL;
	}
	fclose( f );
	*length = size;
	return data;
}

/*
=================
Content_WriteFile

Replaces a file whole: written beside it under a name of this process's
own, <file>.<pid>.tmp, then renamed over it, so a reader, in this process
or another sharing the home, sees the old file or the new one, never half
of one, and two writing at once don't mix
=================
*/
static qboolean Content_WriteFile( const char *ospath, const byte *data, int length ) {
	char		target[MAX_OSPATH], tmp[MAX_OSPATH];
	FILE		*f;
	qboolean	ok;

	if ( com_check ) {
		return qfalse;	// --check writes nothing
	}
	// ospath may be Content_HomeFile's buffer, which the folders' reuse
	Q_strncpyz( target, ospath, sizeof( target ) );
	// cut short, the name wouldn't be this process's, and could be the file's
	if ( Com_sprintf( tmp, sizeof( tmp ), CONTENT_TEMP_FORMAT, target, Sys_PID() ) >= (int)sizeof( tmp ) ) {
		return qfalse;
	}
	// the cache's folders, without FS_CreatePath's fatal error where the
	// home can't take them: then the file isn't written, and says so
	Sys_Mkdir( Content_HomeFile( "cache" ) );
	Sys_Mkdir( Content_HomeFile( CONTENT_DIR ) );
	f = Sys_FOpen( tmp, "wb" );
	if ( !f ) {
		return qfalse;
	}
	ok = (int)fwrite( data, 1, length, f ) == length;
	ok = fclose( f ) == 0 && ok;
	if ( !ok || !Sys_Rename( tmp, target ) ) {
		remove( tmp );
		return qfalse;
	}
	return qtrue;
}

/*
==============================================================

THE HASH JOB

==============================================================
*/

#define HASH_BUFFER_SIZE	( 256 * 1024 )

typedef struct {
	hashItem_t		*items;
	int				count;
	int				next;			// the item being hashed
	FILE			*file;			// its file, while it's being read
	int64_t			read;			// what's been read of it
	sha256_t		sha;
	byte			*buffer;
	sysThread_t		*thread;		// NULL when sliced
	qboolean		running;
} hashJob_t;

static hashJob_t	hashJob;

/*
=================
Content_HashJob_Step

Reads and hashes a buffer of the job's current item, opening it, or
finishing it and moving to the next. Touches nothing but the job, so the
thread can run it. Returns qfalse once every item is done
=================
*/
static qboolean Content_HashJob_Step( hashJob_t *job ) {
	hashItem_t	*item;
	size_t		got;
	int64_t		size, mtime;

	if ( job->next >= job->count ) {
		return qfalse;
	}
	item = &job->items[job->next];
	if ( !job->file ) {
		job->file = Sys_FOpen( item->ospath, "rb" );
		if ( !job->file ) {
			item->result = HASH_FAILED;
			job->next++;
			return job->next < job->count;
		}
		job->read = 0;
		Com_SHA256Init( &job->sha );
	}

	got = fread( job->buffer, 1, HASH_BUFFER_SIZE, job->file );
	if ( got > 0 ) {
		Com_SHA256Update( &job->sha, job->buffer, got );
		job->read += got;
		if ( job->read <= item->pak.size ) {
			return qtrue;
		}
	}

	// at its end, or past the size it had: is it as it was when queued?
	if ( ferror( job->file ) ) {
		item->result = HASH_FAILED;
	} else if ( job->read != item->pak.size || !Content_FileStat( item->ospath, &size, &mtime ) ||
		size != item->pak.size || mtime != item->pak.mtime ) {
		item->result = HASH_CHANGED;
	} else {
		Com_SHA256Final( &job->sha, item->pak.sha256 );
		item->result = HASH_DONE;
	}
	fclose( job->file );
	job->file = NULL;
	job->next++;
	return job->next < job->count;
}

static void Content_HashJob_Thread( void *arg ) {
	while ( Content_HashJob_Step( arg ) ) {
	}
}

/*
=================
Content_HashJob_Start
=================
*/
qboolean Content_HashJob_Start( const hashItem_t *items, int count ) {
	hashJob_t	*job = &hashJob;
	int			i;

	if ( job->running || count <= 0 || count > HASH_JOB_MAX ) {
		return qfalse;
	}
	free( job->items );
	job->items = malloc( count * sizeof( *job->items ) );
	if ( !job->buffer ) {
		job->buffer = malloc( HASH_BUFFER_SIZE );
	}
	if ( !job->items || !job->buffer ) {
		free( job->items );
		job->items = NULL;
		return qfalse;
	}
	memcpy( job->items, items, count * sizeof( *job->items ) );
	for ( i = 0; i < count; i++ ) {
		job->items[i].result = HASH_WAITING;
	}
	job->count = count;
	job->next = 0;
	job->file = NULL;
	job->running = qtrue;
	job->thread = Sys_StartThread( Content_HashJob_Thread, job );
	return qtrue;
}

/*
=================
Content_HashJob_Running
=================
*/
qboolean Content_HashJob_Running( void ) {
	return hashJob.running;
}

/*
=================
Content_HashJob_Poll
=================
*/
const hashItem_t *Content_HashJob_Poll( int *count ) {
	hashJob_t	*job = &hashJob;
	int64_t		end;
	qboolean	more;

	if ( !job->running ) {
		return NULL;
	}
	if ( job->thread ) {
		if ( !Sys_ThreadFinished( job->thread ) ) {
			return NULL;
		}
		job->thread = NULL;
	} else {
		end = Sys_Nanoseconds() + HASH_SLICE_MSEC * 1000000;
		do {
			more = Content_HashJob_Step( job );
		} while ( more && Sys_Nanoseconds() < end );
		if ( more ) {
			return NULL;
		}
	}

	job->running = qfalse;
	*count = job->count;
	return job->items;
}

/*
==============================================================

THE SERVER'S PAK INDEX

==============================================================
*/

static pakRecord_t	pakIndex[CONTENT_MAX_RECORDS];
static unsigned int	pakIndexHash[CONTENT_MAX_RECORDS];	// of each record's path
static int			pakIndexCount;
static qboolean		pakIndexLoaded;

// what to hash once the job running is done
static pakRecord_t	*pakIndexQueued;
static int			pakIndexQueuedCount;
// whether the job running is the index's: a client's checks of installed
// paks share the job (P192)
static qboolean		pakIndexHashing;

/*
=================
Content_PakOSPath

Where a record's pak is now, in a static buffer; NULL if its root has no
directory
=================
*/
static const char *Content_PakOSPath( const pakRecord_t *pak ) {
	const char	*root = FS_ContentRootPath( pak->root );

	return root ? FS_BaseDir_BuildOSPath( root, pak->path ) : NULL;
}

// a record's key, for a quicker lookup
static unsigned int Content_PathHash( int root, const char *path ) {
	unsigned int	hash = 2166136261u ^ (unsigned int)root;	// FNV-1a

	for ( ; *path; path++ ) {
		hash = ( hash ^ (byte)*path ) * 16777619u;
	}
	return hash;
}

/*
=================
Content_PakIndex_Load

Reads installed.dat again, as another process sharing the home may have
written it; a damaged one counts as empty, and is replaced at the next
write. The first time, also removes what a write cut short left
=================
*/
static void Content_PakIndex_Load( void ) {
	byte	*data;
	int		i, length;

	Content_RemoveStaleTemps();
	pakIndexLoaded = qtrue;
	pakIndexCount = 0;
	data = Content_ReadFile( Content_HomeFile( PAK_INDEX_FILE ),
		CONTENT_HEADER_SIZE + CONTENT_MAX_RECORDS * PAK_INDEX_RECORD_SIZE, &length );
	if ( !data ) {
		return;
	}
	pakIndexCount = Content_ParsePakIndex( data, length, pakIndex, CONTENT_MAX_RECORDS );
	if ( pakIndexCount < 0 ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: %s is damaged, so its paks will be hashed again\n", PAK_INDEX_FILE );
		pakIndexCount = 0;
	}
	for ( i = 0; i < pakIndexCount; i++ ) {
		pakIndexHash[i] = Content_PathHash( pakIndex[i].root, pakIndex[i].path );
	}
	free( data );
}

static int Content_PakIndex_Lookup( int root, const char *path ) {
	unsigned int	hash = Content_PathHash( root, path );
	int				i;

	for ( i = 0; i < pakIndexCount; i++ ) {
		if ( pakIndexHash[i] == hash && pakIndex[i].root == root && !strcmp( pakIndex[i].path, path ) ) {
			return i;
		}
	}
	return -1;
}

/*
=================
Content_PakIndex_Find
=================
*/
const pakRecord_t *Content_PakIndex_Find( int root, const char *path, int64_t size, int64_t mtime ) {
	int		i;

	if ( !pakIndexLoaded ) {
		Content_PakIndex_Load();
	}
	i = Content_PakIndex_Lookup( root, path );
	if ( i < 0 || pakIndex[i].size != size || pakIndex[i].mtime != mtime ) {
		return NULL;
	}
	return &pakIndex[i];
}

/*
=================
Content_PakIndex_FindSHA256
=================
*/
const pakRecord_t *Content_PakIndex_FindSHA256( const byte sha256[SHA256_DIGEST_SIZE] ) {
	int		i;

	if ( !pakIndexLoaded ) {
		Content_PakIndex_Load();
	}
	for ( i = 0; i < pakIndexCount; i++ ) {
		if ( !memcmp( pakIndex[i].sha256, sha256, SHA256_DIGEST_SIZE ) ) {
			return &pakIndex[i];
		}
	}
	return NULL;
}

/*
=================
Content_PakIndex_Add

A record for a pak just hashed, replacing its path's; when the index is
full, in place of the one used least recently
=================
*/
static void Content_PakIndex_Add( const hashItem_t *item, int64_t now ) {
	pakRecord_t	*r;
	int			i, j;

	i = Content_PakIndex_Lookup( item->pak.root, item->pak.path );
	if ( i < 0 && pakIndexCount < CONTENT_MAX_RECORDS ) {
		i = pakIndexCount++;
	} else if ( i < 0 ) {
		for ( i = 0, j = 1; j < pakIndexCount; j++ ) {
			if ( pakIndex[j].lastUsed < pakIndex[i].lastUsed ) {
				i = j;
			}
		}
	}
	r = &pakIndex[i];
	*r = item->pak;
	r->flags = 0;
	r->lastUsed = now;
	pakIndexHash[i] = Content_PathHash( r->root, r->path );
}

/*
=================
Content_PakIndex_Write

installed.dat; after the last job queued, without the paks that are gone.
A record under a root this process has no directory for is kept, as
another sharing the home may
=================
*/
static void Content_PakIndex_Write( qboolean prune ) {
	int64_t		size, mtime;
	const char	*ospath;
	byte		*data;
	int			i, kept, length;

	for ( i = kept = 0; i < pakIndexCount; i++ ) {
		if ( !prune || !( ospath = Content_PakOSPath( &pakIndex[i] ) ) || Content_FileStat( ospath, &size, &mtime ) ) {
			pakIndexHash[kept] = pakIndexHash[i];
			pakIndex[kept++] = pakIndex[i];
		}
	}
	pakIndexCount = kept;

	data = malloc( CONTENT_HEADER_SIZE + pakIndexCount * PAK_INDEX_RECORD_SIZE );
	if ( !data ) {
		return;
	}
	length = Content_WritePakIndex( pakIndex, pakIndexCount, data,
		CONTENT_HEADER_SIZE + pakIndexCount * PAK_INDEX_RECORD_SIZE );
	if ( !Content_WriteFile( Content_HomeFile( PAK_INDEX_FILE ), data, length ) && !com_check ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: couldn't write %s\n", Content_HomeFile( PAK_INDEX_FILE ) );
	}
	free( data );
}

/*
=================
Content_PakIndex_StartQueued
=================
*/
static void Content_PakIndex_StartQueued( void ) {
	hashItem_t	*items;
	const char	*ospath;
	int			i, n, count = MIN( pakIndexQueuedCount, HASH_JOB_MAX );

	if ( !count || Content_HashJob_Running() || !( items = malloc( count * sizeof( *items ) ) ) ) {
		return;
	}
	for ( i = n = 0; i < count; i++ ) {
		if ( ( ospath = Content_PakOSPath( &pakIndexQueued[i] ) ) != NULL ) {
			items[n].pak = pakIndexQueued[i];
			Q_strncpyz( items[n].ospath, ospath, sizeof( items[n].ospath ) );
			n++;
		}
	}
	if ( n && !Content_HashJob_Start( items, n ) ) {
		free( items );
		return;
	}
	free( items );
	pakIndexHashing = n > 0;
	pakIndexQueuedCount -= count;
	memmove( pakIndexQueued, pakIndexQueued + count, pakIndexQueuedCount * sizeof( *pakIndexQueued ) );
	if ( !pakIndexQueuedCount ) {
		free( pakIndexQueued );
		pakIndexQueued = NULL;
	}
}

/*
=================
Content_PakIndex_Append

Adds items to the end of the queue
=================
*/
static void Content_PakIndex_Append( const pakRecord_t *paks, int count ) {
	pakRecord_t	*queue;

	if ( count <= 0 ) {
		return;
	}
	queue = realloc( pakIndexQueued, ( pakIndexQueuedCount + count ) * sizeof( *paks ) );
	if ( !queue ) {
		return;
	}
	memcpy( queue + pakIndexQueuedCount, paks, count * sizeof( *paks ) );
	pakIndexQueued = queue;
	pakIndexQueuedCount += count;
}

/*
=================
Content_PakIndex_Queue
=================
*/
void Content_PakIndex_Queue( const pakRecord_t *paks, int count ) {
	free( pakIndexQueued );
	pakIndexQueued = NULL;
	pakIndexQueuedCount = 0;
	Content_PakIndex_Append( paks, count );
	Content_PakIndex_StartQueued();
}

/*
=================
Content_PakIndex_Frame

Merges a finished job into installed.dat, as it now is on disk; a pak
that changed while it was read goes to the end of the queue
=================
*/
qboolean Content_PakIndex_Frame( void ) {
	const hashItem_t	*items;
	pakRecord_t			again;
	int64_t				now, bytes;
	int					i, count, hashed;

	items = pakIndexHashing ? Content_HashJob_Poll( &count ) : NULL;
	if ( !items ) {
		Content_PakIndex_StartQueued();
		return qfalse;
	}
	pakIndexHashing = qfalse;

	Content_PakIndex_Load();
	now = time( NULL );
	hashed = 0;
	bytes = 0;
	for ( i = 0; i < count; i++ ) {
		if ( items[i].result == HASH_DONE ) {
			Content_PakIndex_Add( &items[i], now );
			hashed++;
			bytes += items[i].pak.size;
		} else if ( items[i].result == HASH_CHANGED ) {
			again = items[i].pak;
			if ( Content_FileStat( items[i].ospath, &again.size, &again.mtime ) ) {
				Content_PakIndex_Append( &again, 1 );
			}
		}
	}
	if ( hashed ) {
		Content_PakIndex_Write( !pakIndexQueuedCount );
		Com_Printf( "Hashed %d pak%s, %.1f MB, for the pak index\n", hashed, hashed == 1 ? "" : "s",
			bytes / ( 1024.0 * 1024.0 ) );
	}
	Content_PakIndex_StartQueued();
	return hashed > 0;
}

