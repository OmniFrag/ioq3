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
// content.h -- paks known by their content: each one's SHA-256

#ifndef CONTENT_H
#define CONTENT_H

#include "q_shared.h"
#include "qcommon.h"
#include "ofd.h"

// the files' common header: magic, version, record size, record count
#define CONTENT_HEADER_SIZE		16
#define CONTENT_VERSION			1
#define CONTENT_MAX_RECORDS		4096
// a path in a record, relative to its root, NUL-padded: a pak whose path
// is longer isn't indexed
#define CONTENT_PATH_SIZE		256
#define CONTENT_MAX_PAK_SIZE	0x7fffffff	// the engine's file sizes are ints
// the folder under fs_homedatapath for both indexes and the cache's paks
#define CONTENT_DIR				"cache/content"
// an index's file written first under the process's own name: <file>.<pid>.tmp
#define CONTENT_TEMP_FORMAT		"%s.%d.tmp"

/*
==============================================================

THE PAK INDEX

<fs_homedatapath>/cache/content/installed.dat: each installed pak's
SHA-256, known by its path, its size and its modification time, so a pak
is hashed once and again only when it changes. The path is relative to
one of the search path's roots, known by a number (FS_ContentRootPath),
with '/' between its parts, so it fits and survives the install moving.
A server and a client sharing one home both write it, so each reads it
again before it merges what it hashed, and replaces it whole through a
file of its own and a rename

==============================================================
*/

#define PAK_INDEX_MAGIC			"OFPI"
#define PAK_INDEX_RECORD_SIZE	320
#define PAK_INDEX_FILE			CONTENT_DIR "/installed.dat"

#define PAK_RECORD_RETAIL		1	// one of the game's own paks, not hashed; the
									// format's, which this engine doesn't write

typedef struct {
	byte			sha256[SHA256_DIGEST_SIZE];
	int64_t			size;
	int64_t			mtime;			// Sys_FileTime, in seconds
	unsigned int	pakChecksum;	// pack_t.checksum, as sv_referencedPaks lists
									// it, not the pure checksum, which varies by map
	unsigned int	flags;			// PAK_RECORD_*, a byte
	int				root;			// FS_ContentRootPath's number, 1 to 255
	int64_t			lastUsed;		// Unix seconds
	char			path[CONTENT_PATH_SIZE];	// relative to the root
} pakRecord_t;

// the records of an installed.dat read whole: their count, or -1 when the
// file is damaged in any way (short or long, wrong magic, version or record
// size, too many records, a path without its NUL, no root), when it's
// ignored
int		Content_ParsePakIndex( const byte *data, int length, pakRecord_t *records, int maxRecords );
// the file's bytes for these records; the length written, or -1 if out is
// too small
int		Content_WritePakIndex( const pakRecord_t *records, int count, byte *out, int size );

// a file's size and modification time, as the pak index keys on them
qboolean	Content_FileStat( const char *ospath, int64_t *size, int64_t *mtime );

/*
==============================================================

THE HASH JOB

Hashes a list of files, one job at a time, on a thread of its own: the
thread reads (Sys_FOpen, stat) and hashes into the job's own copy of the
list and its own buffer, and touches nothing else of the engine's. Where there are no
threads (the web), or none starts, the same job runs a slice at a time
from Content_HashJob_Poll

==============================================================
*/

typedef enum {
	HASH_WAITING,
	HASH_DONE,
	HASH_CHANGED,		// its size or time changed meanwhile: hash it again
	HASH_FAILED			// it couldn't be read
} hashResult_t;

typedef struct {
	pakRecord_t		pak;					// its size and time; its hash once done
	char			ospath[MAX_OSPATH];		// what's read
	hashResult_t	result;
} hashItem_t;

#define HASH_SLICE_MSEC		1		// without a thread, at most this a poll
#define HASH_JOB_MAX		64		// items a job

// starts hashing a copy of the items, at most HASH_JOB_MAX; qfalse if a job
// runs, there are too many, or there's no memory
qboolean	Content_HashJob_Start( const hashItem_t *items, int count );
qboolean	Content_HashJob_Running( void );
// once a frame: the items, with their results, once the job's finished
// (valid until the next start), or NULL while it runs or when none does
const hashItem_t	*Content_HashJob_Poll( int *count );

/*
==============================================================

THE SERVER'S PAK INDEX

At each map load the server queues every pak of its search path the index
lacks, the referenced ones first; a frame later merges what the job
hashed. A pak not hashed yet isn't offered by its hash

==============================================================
*/

// the paks to hash next, after the job running, HASH_JOB_MAX a job, by
// their records' keys (root, path, size and time) and checksums; replaces
// those queued
void		Content_PakIndex_Queue( const pakRecord_t *paks, int count );
// once a server frame; qtrue when the index changed, so what's built from
// it should be built again
qboolean	Content_PakIndex_Frame( void );
// the record of the pak at path under root, if its size and time are still
// these
const pakRecord_t	*Content_PakIndex_Find( int root, const char *path, int64_t size, int64_t mtime );
const pakRecord_t	*Content_PakIndex_FindSHA256( const byte sha256[SHA256_DIGEST_SIZE] );

/*
==============================================================

THE CLIENT'S CONTENT CACHE

<fs_homedatapath>/cache/content/: downloaded paks named by their SHA-256
(<sha256>.pk3, <sha256>.part while partial), and index.dat, one record a
hash, with the game directory, name and checksum it was first seen as.
The paks a connected server references are mounted in their game
directories, below that directory's installed paks; the least recently
used go when the cache is over cl_contentCacheMegs. Nothing a server
sends is written under a name it chose

==============================================================
*/

#define CACHE_INDEX_FILE		CONTENT_DIR "/index.dat"
#define CACHE_INDEX_MAGIC		"OFCI"
#define CACHE_INDEX_RECORD_SIZE	144
#define CACHE_GAMEDIR_SIZE		OFCM_GAMEDIR_SIZE	// the content manifest's fields
#define CACHE_NAME_SIZE			OFCM_NAME_SIZE
#define CACHE_MIN_PAK_SIZE		OFCM_MIN_PAK
#define CACHE_DISK_RESERVE		( 256 << 20 )	// left free on the disk

typedef enum {
	CACHE_COMPLETE = 1,
	CACHE_PARTIAL = 2
} cacheState_t;

typedef struct {
	byte			sha256[SHA256_DIGEST_SIZE];
	int64_t			size;
	int64_t			valid;			// the bytes a partial one has
	int64_t			lastUsed;		// Unix seconds: when mounted or downloaded
	unsigned int	pakChecksum;	// pack_t.checksum, as sv_referencedPaks lists it
	cacheState_t	state;
	// as it was first seen, without .pk3: one record a hash, which a
	// content manifest may list under other names too
	char			gamedir[CACHE_GAMEDIR_SIZE];
	char			name[CACHE_NAME_SIZE];
	qboolean		mounted;		// not in the file: in the search path now
} cacheEntry_t;

// as Content_ParsePakIndex, for index.dat; a record that breaks a rule
// (its state, size, names by the content manifest's OFCM_ValidName, or a
// hash listed twice) is damage too
int		Content_ParseCacheIndex( const byte *data, int length, cacheEntry_t *entries, int maxEntries );
int		Content_WriteCacheIndex( const cacheEntry_t *entries, int count, byte *out, int size );

// which entries to evict, least recently used first, until what's kept is
// within maxBytes (0 for no limit) and maxEntries; never a mounted one.
// Sets evict[i] for each, and returns how many
int		Content_ChooseEvictions( const cacheEntry_t *entries, int count, int64_t maxBytes,
			int maxEntries, qboolean *evict );

// the client: at startup, reads index.dat, removes what it doesn't list,
// and evicts down to the bound
void	Content_Cache_Init( void );
void	Content_Cache_Shutdown( void );
// the complete cached pak a classic server references by this checksum,
// game directory and name, the only identity it gives, not mounted yet,
// for the search path: its entry's number, and its OS path; -1 if there's
// none
int		Content_Cache_Find( unsigned int pakChecksum, const char *gamedir, const char *name,
			char *ospath, int ospathSize );
// the same by hash alone, as a content manifest names a pak, mounted or
// not, to be mounted under each name the manifest gives it
int		Content_Cache_FindSHA256( const byte sha256[SHA256_DIGEST_SIZE], char *ospath, int ospathSize );
// whether bytes more fit in the cache, within cl_contentCacheMegs less what
// it has mounted, and on the disk, leaving CACHE_DISK_RESERVE free; if not,
// why, saying what's needed and what there is
qboolean	Content_Cache_HasRoom( int64_t bytes, char *why, int whySize );
// a pak the search path took (it's used now), or one damaged, which goes
void	Content_Cache_Mounted( int entry );
void	Content_Cache_Remove( int entry );
// as the search path is built again: nothing's mounted, then what was
// changed is written
void	Content_Cache_Unmounted( void );
void	Content_Cache_Flush( void );

#endif
