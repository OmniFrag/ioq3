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
// sv_installed.c -- the paks a server has, and taking one away: content
// list, content remove and content restore, and the same for extensions
// (hooks.h's SV_ExtContentList, SV_ExtContentRemove, SV_ExtContentRestore).
//
// A pak the map reads is removed as the next map loads, between the file
// system letting go of every pak and reading them again (SV_InstalledMapLoad),
// so players still download it until then, and Windows, which won't move
// an open file, lets it go; one content add installed since the map loaded
// goes at once. It's moved aside, into sv_contentDir's removed folder, and
// kept for INSTALLED_KEEP_DAYS, so content restore brings it back.
//
// What the server knows of its paks beyond the file system is in the state
// home's content-record.txt, a line a pak, each tab-separated: its kind, its
// "<game>/<name>.pk3", then key=value fields, those it doesn't know kept as
// they are. file: one content add installed or content restore brought back,
// with its size and time then, so one replaced by hand since reads as put
// there by hand. removing: one content remove took, which goes as the next
// map loads. kept: a removed one's copy, until it's pruned. Every server of
// a home reads and writes it, each reading it again before it changes it.

#include "server.h"
#include "../qcommon/content.h"

#include <errno.h>
#include <time.h>

#define INSTALLED_RECORD		"content-record.txt"
#define INSTALLED_REMOVED		"removed"		// sv_contentDir's folder of kept copies
#define INSTALLED_KEEP_DAYS		30
#define INSTALLED_KEEP_SECONDS	( INSTALLED_KEEP_DAYS * 24 * 60 * 60 )
#define INSTALLED_NAME			( MAX_QPATH * 2 )	// a pak's file name, with .pk3
#define INSTALLED_EXTRA			256				// the fields a record keeps without knowing them
#define INSTALLED_MAX_NAMED		5				// maps a refusal names of each kind
#define INSTALLED_RECORD_MAX	( 16 * 1024 * 1024 )	// the record's most bytes

typedef enum {
	REC_FILE,
	REC_REMOVING,
	REC_KEPT
} recKind_t;

static const char *const recKinds[] = { "file", "removing", "kept" };

static qboolean SV_InstalledName( const char *name, const char *gameDir );

typedef struct {
	recKind_t	kind;
	char		game[OFCM_GAMEDIR_SIZE];
	char		name[INSTALLED_NAME];
	char		origin[16];			// as the list says it: added or by_hand
	int64_t		size, mtime;		// the file's as recorded
	int64_t		at, until;			// when it happened, and a kept copy's end; Unix seconds
	char		extra[INSTALLED_EXTRA];	// "\tkey=value" fields it doesn't know
} record_t;

static struct {
	record_t	*records;
	int			count, room;
	qboolean	read;
	int			changes;		// SV_ExtContentRevision's part: the record changed
	int			mapLoads;		// and maps loaded
	// the maps only removing paks hold, sorted, for SV_ExtMapRemoving,
	// as of changes + mapLoads
	char		(*removingMaps)[MAX_QPATH];
	int			numRemovingMaps, removingMapsAt;
} inst = { .removingMapsAt = -1 };

/*
==============================================================

THE RECORD

==============================================================
*/

static const char *SV_RecordPath( void ) {
	return va( "%s%c" INSTALLED_RECORD, Cvar_VariableString( "fs_homestatepath" ), PATH_SEP );
}

static void SV_RecordClear( void ) {
	free( inst.records );
	inst.records = NULL;
	inst.count = inst.room = 0;
}

static record_t *SV_RecordGrow( void ) {
	record_t	*grown;

	if ( inst.count == inst.room ) {
		if ( !( grown = realloc( inst.records, ( inst.room + 64 ) * sizeof( *grown ) ) ) ) {
			return NULL;
		}
		inst.records = grown;
		inst.room += 64;
	}
	Com_Memset( &inst.records[inst.count], 0, sizeof( inst.records[0] ) );
	return &inst.records[inst.count++];
}

// a line's record, qfalse for one that doesn't read
static qboolean SV_RecordParse( char *line, record_t *rec ) {
	char	*field, *next, *slash, *value;
	int		i;

	Com_Memset( rec, 0, sizeof( *rec ) );
	if ( !( next = strchr( line, '\t' ) ) ) {
		return qfalse;
	}
	*next++ = '\0';
	for ( i = 0; i < (int)ARRAY_LEN( recKinds ) && strcmp( line, recKinds[i] ); i++ ) {
	}
	if ( i == ARRAY_LEN( recKinds ) ) {
		return qfalse;
	}
	rec->kind = i;
	field = next;
	next = strchr( field, '\t' );
	if ( next ) {
		*next++ = '\0';
	}
	if ( !( slash = strchr( field, '/' ) ) || slash - field >= (int)sizeof( rec->game ) ||
		strlen( slash + 1 ) >= sizeof( rec->name ) ) {
		return qfalse;
	}
	Q_strncpyz( rec->game, field, slash - field + 1 );
	Q_strncpyz( rec->name, slash + 1, sizeof( rec->name ) );
	// a name content remove could have written, as it reaches remove()
	// and rename(): no path, nor a game folder that climbs out
	if ( !SV_InstalledName( rec->name, rec->game ) ) {
		return qfalse;
	}
	for ( field = next; field; field = next ) {
		if ( ( next = strchr( field, '\t' ) ) != NULL ) {
			*next++ = '\0';
		}
		if ( !( value = strchr( field, '=' ) ) ) {
			continue;
		}
		*value++ = '\0';
		if ( !strcmp( field, "origin" ) ) {
			// added, or anything else, put there by hand
			Q_strncpyz( rec->origin, !strcmp( value, "added" ) ? "added" : "by_hand", sizeof( rec->origin ) );
		} else if ( !strcmp( field, "size" ) ) {
			rec->size = strtoll( value, NULL, 10 );
		} else if ( !strcmp( field, "mtime" ) ) {
			rec->mtime = strtoll( value, NULL, 10 );
		} else if ( !strcmp( field, "at" ) ) {
			rec->at = strtoll( value, NULL, 10 );
		} else if ( !strcmp( field, "until" ) ) {
			rec->until = strtoll( value, NULL, 10 );
		} else {
			// not through va, whose buffers a caller's name may be in
			// (SV_InstalledAdded's)
			Q_strcat( rec->extra, sizeof( rec->extra ), "\t" );
			Q_strcat( rec->extra, sizeof( rec->extra ), field );
			Q_strcat( rec->extra, sizeof( rec->extra ), "=" );
			Q_strcat( rec->extra, sizeof( rec->extra ), value );
		}
	}
	return qtrue;
}

/*
=================
SV_RecordRead

The record as it is on disk now, as another server of the home may have
changed it
=================
*/
static void SV_RecordRead( void ) {
	char		*text, *line, *end;
	record_t	rec, *slot;
	byte		*data;
	int			length;

	SV_RecordClear();
	inst.read = qtrue;
	if ( !( data = Content_ReadFile( SV_RecordPath(), INSTALLED_RECORD_MAX, &length ) ) ) {
		return;
	}
	if ( !( text = realloc( data, length + 1 ) ) ) {
		free( data );
		return;
	}
	text[length] = '\0';
	for ( line = text; *line; line = end ) {
		end = line + strcspn( line, "\r\n" );
		if ( *end ) {
			*end++ = '\0';
		}
		if ( line[0] && line[0] != '#' && SV_RecordParse( line, &rec ) && ( slot = SV_RecordGrow() ) != NULL ) {
			*slot = rec;
		}
	}
	free( text );
}

static void SV_RecordReady( void ) {
	if ( !inst.read ) {
		SV_RecordRead();
	}
}

/*
=================
SV_RecordWrite

The record replaced whole, through a file of its own and a rename
=================
*/
static void SV_RecordWrite( void ) {
	configText_t	text = { 0 };
	const record_t	*rec;
	char			path[MAX_OSPATH];
	int				i;

	// a copy: the lines' va calls reuse its buffer
	Q_strncpyz( path, SV_RecordPath(), sizeof( path ) );
	inst.changes++;
	Com_ConfigAppend( &text, "# what this server knows of its paks: content list, remove and restore\n" );
	for ( i = 0; i < inst.count; i++ ) {
		rec = &inst.records[i];
		Com_ConfigAppend( &text, va( "%s\t%s/%s\torigin=%s\tsize=%lld\tmtime=%lld\tat=%lld", recKinds[rec->kind],
			rec->game, rec->name, rec->origin, (long long)rec->size, (long long)rec->mtime, (long long)rec->at ) );
		if ( rec->until ) {
			Com_ConfigAppend( &text, va( "\tuntil=%lld", (long long)rec->until ) );
		}
		Com_ConfigAppend( &text, va( "%s\n", rec->extra ) );
	}
	FS_CreatePath( path );
	if ( !Content_WriteFile( path, (const byte *)text.text, text.length ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: can't write %s\n", path );
	}
	Z_Free( text.text );
}

static record_t *SV_RecordFind( recKind_t kind, const char *game, const char *name ) {
	int		i;

	for ( i = 0; i < inst.count; i++ ) {
		if ( inst.records[i].kind == kind && !FS_FilenameCompare( inst.records[i].game, game ) &&
			!FS_FilenameCompare( inst.records[i].name, name ) ) {
			return &inst.records[i];
		}
	}
	return NULL;
}

static void SV_RecordDrop( record_t *rec ) {
	int		i = rec - inst.records;

	inst.count--;
	memmove( rec, rec + 1, ( inst.count - i ) * sizeof( *rec ) );
}

// a record of kind for game/name, replacing one there; NULL out of memory
static record_t *SV_RecordPut( recKind_t kind, const char *game, const char *name ) {
	record_t	*rec = SV_RecordFind( kind, game, name );

	if ( rec ) {
		Com_Memset( rec, 0, sizeof( *rec ) );
	} else if ( !( rec = SV_RecordGrow() ) ) {
		return NULL;
	}
	rec->kind = kind;
	Q_strncpyz( rec->game, game, sizeof( rec->game ) );
	Q_strncpyz( rec->name, name, sizeof( rec->name ) );
	return rec;
}

/*
==============================================================

PLACES AND NAMES

==============================================================
*/

// where content add installs gameDir's paks: the home's game directory
static const char *SV_HomePak( const char *gameDir, const char *name ) {
	return FS_BuildOSPath( Cvar_VariableString( "fs_homedatapath" ), gameDir, name );
}

// a kept copy's file, in sv_contentDir's removed folder
static const char *SV_KeptPak( const char *root, const char *gameDir, const char *name ) {
	return va( "%s%c" INSTALLED_REMOVED "%c%s%c%s", root, PATH_SEP, PATH_SEP, gameDir, PATH_SEP, name );
}

// a file's size, for size NULL whether it's there
static qboolean SV_Stat( const char *ospath, int64_t *size ) {
	int64_t	bytes, mtime;

	if ( !Content_FileStat( ospath, &bytes, &mtime ) ) {
		return qfalse;
	}
	if ( size ) {
		*size = bytes;
	}
	return qtrue;
}

static qboolean SV_SamePath( const char *a, const char *b ) {
#ifdef _WIN32
	return !Q_stricmp( a, b );
#else
	return !strcmp( a, b );
#endif
}

// whether a pak's name is one content remove and restore take: a name,
// not a path, of a .pk3, which downloads may have
static qboolean SV_InstalledName( const char *name, const char *gameDir ) {
	char	base[INSTALLED_NAME];

	if ( strlen( name ) >= sizeof( base ) || strpbrk( name, "/\\:" ) || !COM_CompareExtension( name, ".pk3" ) ) {
		return qfalse;
	}
	Q_strncpyz( base, name, sizeof( base ) );
	base[strlen( base ) - 4] = '\0';
	return base[0] && SV_ContentPakName( gameDir, base );
}

// whether anyone but bots is connected
static qboolean SV_InstalledPlayers( void ) {
	int		i;

	for ( i = 0; i < sv_maxclients->integer; i++ ) {
		if ( svs.clients && svs.clients[i].state >= CS_CONNECTED &&
			svs.clients[i].netchan.remoteAddress.type != NA_BOT ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
==============================================================

IN USE

The maps a server is using: the one it plays, the next one it loads where
nextmap names one, and the rotation's (sv_pool's: the no-config pool,
every map here, isn't one an admin chose)

==============================================================
*/

typedef struct {
	char	next[MAX_QPATH];
	char	*pool;			// sv_pool's words, maps alone, each ended by '\0', then an empty one
} inUse_t;

// the map a nextmap names, through vstr, as the configs' rotations write
// it ("vstr d2", d2 "map q3dm2; set nextmap vstr d3"); "" for none or the
// pool's (pool_next)
static void SV_NextMap( char *next, int size ) {
	char		text[MAX_CVAR_VALUE_STRING], *p, *command, *end;
	int			hops;

	next[0] = '\0';
	Q_strncpyz( text, Cvar_VariableString( "nextmap" ), sizeof( text ) );
	for ( hops = 0; hops < 4; hops++ ) {
		p = text;
		while ( *p == ' ' || *p == '\t' ) {
			p++;
		}
		if ( !Q_stricmpn( p, "vstr ", 5 ) ) {
			p += 5;
			while ( *p == ' ' ) {
				p++;
			}
			p[strcspn( p, " ;" )] = '\0';
			Q_strncpyz( text, Cvar_VariableString( p ), sizeof( text ) );
			continue;
		}
		// the first command, map or devmap and their kin
		end = p + strcspn( p, ";" );
		*end = '\0';
		command = p;
		p += strcspn( p, " \t" );
		if ( !*p ) {
			return;
		}
		*p++ = '\0';
		if ( Q_stricmp( command, "map" ) && Q_stricmp( command, "devmap" ) && Q_stricmp( command, "spmap" ) &&
			Q_stricmp( command, "spdevmap" ) ) {
			return;
		}
		while ( *p == ' ' || *p == '\t' ) {
			p++;
		}
		p[strcspn( p, " \t" )] = '\0';
		Q_strncpyz( next, p, size );
		return;
	}
}

static void SV_InUseRead( inUse_t *use ) {
	const char	*pool = Cvar_VariableString( "sv_pool" );
	char		*out;

	SV_NextMap( use->next, sizeof( use->next ) );
	use->pool = out = malloc( strlen( pool ) + 2 );
	if ( !out ) {
		return;
	}
	// each word's map, before its /mode
	while ( *pool ) {
		while ( *pool == ' ' ) {
			pool++;
		}
		if ( !*pool ) {
			break;
		}
		while ( *pool && *pool != ' ' && *pool != '/' ) {
			*out++ = *pool++;
		}
		*out++ = '\0';
		while ( *pool && *pool != ' ' ) {
			pool++;
		}
	}
	*out = '\0';
}

static qboolean SV_InPool( const inUse_t *use, const char *map ) {
	const char	*word;

	for ( word = use->pool; word && *word; word += strlen( word ) + 1 ) {
		if ( !Q_stricmp( word, map ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
==============================================================

THE LIST

==============================================================
*/

// the list's strings, in blocks that never move
#define LIST_BLOCK	( 64 * 1024 )

typedef struct listBlock_s {
	struct listBlock_s	*next;
	int					used;
	char				text[LIST_BLOCK];
} listBlock_t;

typedef struct {
	extContentList_t	*list;
	int					room;
	const char			*game;		// only this game directory's, or NULL
	const char			*name;		// only paks of this name, or NULL
	qboolean			details;	// maps and uses
	inUse_t				use;
	// the file being described
	extContentFile_t	*file;
	int					mapRoom, useRoom;
} listing_t;

static void *SV_ListAlloc( extContentList_t *list, int size ) {
	listBlock_t	*block = list->blocks;
	void		*p;

	size = ( size + 7 ) & ~7;
	if ( size > LIST_BLOCK ) {
		return NULL;
	}
	if ( !block || block->used + size > LIST_BLOCK ) {
		if ( !( block = malloc( sizeof( *block ) ) ) ) {
			return NULL;
		}
		block->used = 0;
		block->next = list->blocks;
		list->blocks = block;
	}
	p = block->text + block->used;
	block->used += size;
	return p;
}

static const char *SV_ListString( extContentList_t *list, const char *s ) {
	char	*copy = SV_ListAlloc( list, (int)strlen( s ) + 1 );

	if ( copy ) {
		strcpy( copy, s );
	}
	return copy;
}

// a map's use, if it has one, into the file being described
static void SV_ListUse( listing_t *l, const char *map, const char *why ) {
	extContentFile_t	*file = l->file;
	extContentUse_t		*grown;

	if ( file->numUses == l->useRoom ) {
		if ( !( grown = realloc( file->uses, ( l->useRoom + 8 ) * sizeof( *grown ) ) ) ) {
			return;
		}
		file->uses = grown;
		l->useRoom += 8;
	}
	file->uses[file->numUses].map = map;
	file->uses[file->numUses].why = why;
	file->numUses++;
}

static void SV_ListMap( void *user, const char *map ) {
	listing_t			*l = user;
	extContentFile_t	*file = l->file;
	const char			**grown, *copy;

	if ( file->numMaps == l->mapRoom ) {
		if ( !( grown = realloc( (void *)file->maps, ( l->mapRoom + 16 ) * sizeof( *grown ) ) ) ) {
			return;
		}
		file->maps = grown;
		l->mapRoom += 16;
	}
	if ( !( copy = SV_ListString( l->list, map ) ) ) {
		return;
	}
	file->maps[file->numMaps++] = copy;
	// a kept copy's maps aren't in use: it's gone
	if ( !Q_stricmp( file->state, "removed" ) ) {
		return;
	}
	if ( sv_mapname && !Q_stricmp( map, sv_mapname->string ) && com_sv_running->integer ) {
		SV_ListUse( l, copy, "playing" );
	}
	if ( l->use.next[0] && !Q_stricmp( map, l->use.next ) ) {
		SV_ListUse( l, copy, "next" );
	}
	if ( SV_InPool( &l->use, map ) ) {
		SV_ListUse( l, copy, "rotation" );
	}
}

// a new entry, its game directory and name from ospath's, or NULL
static extContentFile_t *SV_ListEntry( listing_t *l, const char *gameDir, const char *baseName ) {
	extContentList_t	*list = l->list;
	extContentFile_t	*grown, *file;
	char				name[sizeof( file->name )];

	if ( ( l->game && FS_FilenameCompare( l->game, gameDir ) ) || strlen( gameDir ) >= sizeof( file->game ) ||
		Com_sprintf( name, sizeof( name ), "%s.pk3", baseName ) >= (int)sizeof( name ) - 1 ||
		( l->name && FS_FilenameCompare( l->name, name ) ) ) {
		return NULL;
	}
	if ( list->count == l->room ) {
		if ( !( grown = realloc( list->files, ( l->room + 64 ) * sizeof( *grown ) ) ) ) {
			return NULL;
		}
		list->files = grown;
		l->room += 64;
	}
	file = &list->files[list->count++];
	Com_Memset( file, 0, sizeof( *file ) );
	Q_strncpyz( file->game, gameDir, sizeof( file->game ) );
	Q_strncpyz( file->name, name, sizeof( file->name ) );
	l->file = file;
	l->mapRoom = l->useRoom = 0;
	return file;
}

// a file's size and origin, by the record: added, or brought back, as it
// was, if it's the file the record has
static void SV_ListOrigin( extContentFile_t *file, const char *ospath ) {
	const record_t	*rec = SV_RecordFind( REC_FILE, file->game, file->name );
	int64_t			mtime;

	file->origin = "by_hand";
	if ( !Content_FileStat( ospath, &file->size, &mtime ) ) {
		file->size = 0;
	} else if ( rec && file->size == rec->size && mtime == rec->mtime ) {
		file->origin = !strcmp( rec->origin, "added" ) ? "added" : "by_hand";
		file->addedAt = rec->at;
	}
}

// a file's facts, as the file system says them
static void SV_ListFacts( void *user, const fsPak_t *pak ) {
	extContentFile_t	*file = ( (listing_t *)user )->file;

	file->gameOwn = pak->gameOwn;
	file->mapPack = pak->mapPack;
	file->gameCode = pak->gameCode;
	file->codeLeftOut = pak->codeLeftOut;
	if ( pak->gameOwn ) {
		file->origin = "game";
		file->removable = qfalse;
	}
}

static void SV_ListLoaded( void *user, const fsPak_t *pak ) {
	listing_t			*l = user;
	extContentFile_t	*file;

	if ( !( file = SV_ListEntry( l, pak->gameDir, pak->baseName ) ) ) {
		return;
	}
	file->home = SV_SamePath( pak->ospath, SV_HomePak( pak->gameDir, file->name ) );
	file->state = file->home && SV_RecordFind( REC_REMOVING, file->game, file->name ) ? "removing" : "loaded";
	SV_ListOrigin( file, pak->ospath );
	file->removable = file->home && SV_InstalledName( file->name, file->game );
	SV_ListFacts( l, pak );
	if ( l->details && !pak->gameOwn ) {
		FS_EachPakMap( pak, SV_ListMap, l );
	}
}

static int QDECL SV_ListOrder( const void *a, const void *b ) {
	const extContentFile_t	*x = a, *y = b;
	int						order;

	if ( ( order = Q_stricmp( x->game, y->game ) ) != 0 || ( order = Q_stricmp( x->name, y->name ) ) != 0 ) {
		return order;
	}
	return x->rank - y->rank;
}

/*
=================
SV_ContentList, SV_ExtContentList

The paks the server has: the search path's, those content add installed
since the map loaded, and with removed, the kept copies; sorted by game
directory, then name, then rank: the home's, its kept copy, another
folder's. Only game's for a game, only name's for a name; with details,
each one's maps and those in use
=================
*/
static void SV_ContentList( const char *game, const char *name, qboolean removed, qboolean details,
		extContentList_t *list ) {
	listing_t			l;
	extContentFile_t	*file;
	const char			*ospath, *gameDir, *root;
	char				base[INSTALLED_NAME];
	int					i;

	Com_Memset( list, 0, sizeof( *list ) );
	Com_Memset( &l, 0, sizeof( l ) );
	l.list = list;
	l.game = game && game[0] ? game : NULL;
	l.name = name;
	l.details = details;
	SV_RecordReady();
	if ( details ) {
		SV_InUseRead( &l.use );
	}
	FS_EachPak( SV_ListLoaded, &l );
	// added since the map loaded
	for ( i = 0; ( ospath = SV_PendingPak( i, &gameDir ) ) != NULL; i++ ) {
		COM_StripExtension( FS_OSPathName( ospath ), base, sizeof( base ) );
		if ( !( file = SV_ListEntry( &l, gameDir, base ) ) ) {
			continue;
		}
		file->state = "pending";
		file->home = qtrue;
		file->removable = SV_InstalledName( file->name, gameDir );
		SV_ListOrigin( file, ospath );
		if ( !FS_DescribePak( ospath, gameDir, SV_ListFacts, details ? SV_ListMap : NULL, &l ) ) {
			list->count--;
		}
	}
	root = removed ? SV_ContentDir() : NULL;
	for ( i = 0; root && i < inst.count; i++ ) {
		const record_t	*rec = &inst.records[i];

		if ( rec->kind != REC_KEPT || !SV_Stat( SV_KeptPak( root, rec->game, rec->name ), NULL ) ) {
			continue;
		}
		COM_StripExtension( rec->name, base, sizeof( base ) );
		if ( !( file = SV_ListEntry( &l, rec->game, base ) ) ) {
			continue;
		}
		file->state = "removed";
		file->home = qtrue;
		file->origin = !strcmp( rec->origin, "added" ) ? "added" : "by_hand";
		file->size = rec->size;
		file->removedAt = rec->at;
		file->keptUntil = rec->until;
		if ( !FS_DescribePak( SV_KeptPak( root, rec->game, rec->name ), rec->game, SV_ListFacts,
				details ? SV_ListMap : NULL, &l ) ) {
			list->count--;
		}
	}
	free( l.use.pool );
	for ( i = 0; i < list->count; i++ ) {
		file = &list->files[i];
		file->rank = !file->home ? 2 : !strcmp( file->state, "removed" ) ? 1 : 0;
		if ( file->rank != 1 ) {
			list->bytes += file->size;
			list->installed++;
		}
	}
	// an empty list's files are NULL, which qsort may not be given
	if ( list->count > 1 ) {
		qsort( list->files, list->count, sizeof( *list->files ), SV_ListOrder );
	}
}

qboolean SV_ExtContentList( const char *game, qboolean removed, qboolean details, extContentList_t *list ) {
	SV_ContentList( game, NULL, removed, details, list );
	return qtrue;
}

void SV_ExtContentListFree( extContentList_t *list ) {
	listBlock_t	*block, *next;
	int			i;

	for ( i = 0; i < list->count; i++ ) {
		free( (void *)list->files[i].maps );
		free( list->files[i].uses );
	}
	free( list->files );
	for ( block = list->blocks; block; block = next ) {
		next = block->next;
		free( block );
	}
	Com_Memset( list, 0, sizeof( *list ) );
}

int SV_ExtContentRevision( void ) {
	// found once there, as hosting makes sv_pool after the engine starts
	static const cvar_t	*pool, *next;

	if ( !pool ) {
		pool = Cvar_FindVar( "sv_pool" );
	}
	if ( !next ) {
		next = Cvar_FindVar( "nextmap" );
	}
	// each part only grows, so the sum moves as any does
	return inst.changes + inst.mapLoads + SV_ExtPendingMapChanges() + ( pool ? pool->modificationCount : 0 ) +
		( next ? next->modificationCount : 0 );
}

/*
==============================================================

REMOVING

==============================================================
*/

// "a, b and c", the first INSTALLED_MAX_NAMED, then how many more
static const char *SV_ListWords( const char **words, int count ) {
	static char	text[MAX_STRING_CHARS];
	int			i, shown = MIN( count, INSTALLED_MAX_NAMED );

	text[0] = '\0';
	for ( i = 0; i < shown; i++ ) {
		if ( i ) {
			Q_strcat( text, sizeof( text ), i == shown - 1 && shown == count ? " and " : ", " );
		}
		Q_strcat( text, sizeof( text ), words[i] );
	}
	if ( shown < count ) {
		Q_strcat( text, sizeof( text ), va( " and %d more", count - shown ) );
	}
	return text;
}

/*
=================
SV_InUseWords

content remove's refusal for a pak with maps in use: "x.pk3 holds maps the
server is using: q3dm17 is being played, and ztn2 and ztn4 are in the
rotation. Change the map or take them out of the rotation first."
=================
*/
static void SV_InUseWords( const extContentFile_t *file, char *out, int size ) {
	const char	*playing = NULL, *next = NULL, *rotation[MAX_QPATH];
	char		parts[3][MAX_STRING_CHARS], advice[256];
	int			i, numRotation = 0, numParts = 0;

	for ( i = 0; i < file->numUses; i++ ) {
		if ( !strcmp( file->uses[i].why, "playing" ) ) {
			playing = file->uses[i].map;
		} else if ( !strcmp( file->uses[i].why, "next" ) ) {
			next = file->uses[i].map;
		} else if ( numRotation < (int)ARRAY_LEN( rotation ) ) {
			rotation[numRotation++] = file->uses[i].map;
		}
	}
	if ( playing ) {
		Com_sprintf( parts[numParts++], sizeof( parts[0] ), "%s is being played", playing );
	}
	if ( next ) {
		Com_sprintf( parts[numParts++], sizeof( parts[0] ), "%s is the next map", next );
	}
	if ( numRotation ) {
		Com_sprintf( parts[numParts++], sizeof( parts[0] ), "%s %s in the rotation",
			SV_ListWords( rotation, numRotation ), numRotation == 1 ? "is" : "are" );
	}
	advice[0] = '\0';
	if ( playing ) {
		Q_strcat( advice, sizeof( advice ), "change the map" );
	}
	if ( next ) {
		Q_strcat( advice, sizeof( advice ), va( "%schange the next map", advice[0] ? ( numRotation ? ", " : " or " ) : "" ) );
	}
	if ( numRotation ) {
		Q_strcat( advice, sizeof( advice ), va( "%s%s out of the rotation", advice[0] ? " or " : "",
			numRotation == 1 ? "take it" : "take them" ) );
	}
	advice[0] = toupper( (byte)advice[0] );
	Com_sprintf( out, size, "%s holds maps the server is using: %s%s%s%s%s. %s first.", file->name, parts[0],
		numParts > 1 ? ( numParts > 2 ? ", " : ", and " ) : "", numParts > 1 ? parts[1] : "",
		numParts > 2 ? ", and " : "", numParts > 2 ? parts[2] : "", advice );
}

static Q_PRINTF_FUNC( 3, 4 ) void QDECL SV_ChangeSay( extContentChange_t *change, const char *code, const char *fmt,
		... ) {
	va_list	argptr;

	va_start( argptr, fmt );
	Q_vsnprintf( change->message, sizeof( change->message ), fmt, argptr );
	va_end( argptr );
	change->code = code;
}

// the names content remove and restore take, and the game directory, or
// qfalse with why
static qboolean SV_ChangeNames( extContentChange_t *change, const char *name, const char **gameDir ) {
	// NULL alone for this server's: "" names no folder
	if ( !*gameDir ) {
		*gameDir = FS_GetCurrentGameDir();
	}
	if ( strpbrk( name, "/\\:" ) ) {
		SV_ChangeSay( change, "bad_name", "Give a file's name, not a path." );
		return qfalse;
	}
	if ( !SV_InstalledName( name, "x" ) ) {
		SV_ChangeSay( change, "bad_name", "%s isn't a name the server can use for a file.", name );
		return qfalse;
	}
	if ( !SV_PendingSearchedGame( *gameDir ) || !SV_InstalledName( name, *gameDir ) ) {
		SV_ChangeSay( change, "bad_game", "%s isn't a game folder this server reads.", *gameDir );
		return qfalse;
	}
	if ( !SV_ContentDir() ) {
		SV_ChangeSay( change, "failed", "The server can't use its content folder, sv_contentDir; its console says why." );
		return qfalse;
	}
	return qtrue;
}

// the entry of gameDir/name, listed into change->list: a kept copy with
// removed, else the home's, then another folder's (the list's order); set
// as change->entry, NULL for none
static extContentFile_t *SV_ChangeFind( extContentChange_t *change, const char *name, const char *gameDir,
		qboolean removed ) {
	extContentList_t	*list = &change->list;
	int					i;

	SV_ExtContentChangeFree( change );
	SV_ContentList( gameDir, name, removed, qtrue, list );
	for ( i = 0; i < list->count && ( list->files[i].rank == 1 ) != removed; i++ ) {
	}
	change->entry = i < list->count ? &list->files[i] : NULL;
	return (extContentFile_t *)change->entry;
}

/*
=================
SV_KeepAside

A pak moved into sv_contentDir's removed folder, its kept copy recorded,
and its record as a file dropped; when it can't be moved, as its folder is
on another disk, it's deleted, without a copy. qfalse when neither can be
done, as Windows won't while a file's open. keptCopy, when given, says
whether a copy was kept
=================
*/
static qboolean SV_KeepAside( const char *gameDir, const char *name, const char *origin, qboolean *keptCopy ) {
	char		from[MAX_OSPATH], to[MAX_OSPATH];
	const char	*root = SV_ContentDir();
	record_t	*rec;
	int64_t		size, mtime;
	qboolean	kept;

	if ( keptCopy ) {
		*keptCopy = qfalse;
	}
	Q_strncpyz( from, SV_HomePak( gameDir, name ), sizeof( from ) );
	if ( !Content_FileStat( from, &size, &mtime ) ) {
		return qtrue;
	}
	kept = root && Com_sprintf( to, sizeof( to ), "%s", SV_KeptPak( root, gameDir, name ) ) < (int)sizeof( to ) - 1;
	if ( kept ) {
		FS_CreatePath( to );
		kept = Sys_Rename( from, to );
	}
	if ( !kept ) {
		if ( remove( from ) ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: content remove: %s/%s can't be moved or deleted now: %s\n", gameDir,
				name, strerror( errno ) );
			return qfalse;
		}
		Com_Printf( S_COLOR_YELLOW "WARNING: content remove: %s/%s couldn't be moved into %s, so it's deleted, "
			"with no copy kept\n", gameDir, name, root ? root : "sv_contentDir" );
	} else {
		Com_Printf( "content remove: %s/%s removed; its copy is kept for %d days in %s\n", gameDir, name,
			INSTALLED_KEEP_DAYS, to );
	}
	if ( ( rec = SV_RecordFind( REC_FILE, gameDir, name ) ) != NULL ) {
		SV_RecordDrop( rec );
	}
	if ( keptCopy ) {
		*keptCopy = kept;
	}
	if ( kept && ( rec = SV_RecordPut( REC_KEPT, gameDir, name ) ) != NULL ) {
		rec->size = size;
		rec->mtime = mtime;
		rec->at = time( NULL );
		rec->until = rec->at + INSTALLED_KEEP_SECONDS;
		Q_strncpyz( rec->origin, origin, sizeof( rec->origin ) );
	}
	return qtrue;
}

/*
=================
SV_ExtContentRemove

content remove of name, in gameDir (this server's for NULL), with its
verdict in change: removed, or why not. qfalse, doing nothing, while an
add or an export runs
=================
*/
qboolean SV_ExtContentRemove( const char *name, const char *gameDir, extContentChange_t *change ) {
	extContentFile_t	*file;
	record_t			*rec;
	qboolean			keptCopy;

	Com_Memset( change, 0, sizeof( *change ) );
	if ( SV_ContentWorking() ) {
		return qfalse;
	}
	if ( !SV_ChangeNames( change, name, &gameDir ) ) {
		return qtrue;
	}
	SV_RecordRead();
	if ( !( file = SV_ChangeFind( change, name, gameDir, qfalse ) ) ) {
		SV_ChangeSay( change, "not_found", "There's no file named %s in the %s folder.", name, gameDir );
		return qtrue;
	}
	if ( file->gameOwn ) {
		SV_ChangeSay( change, "game_own", "%s is one of the game's own files, so it can't be removed.", file->name );
		return qtrue;
	}
	if ( !file->home ) {
		SV_ChangeSay( change, "not_server_folder", "%s is in the game's install folder, not the server's own, so it "
			"can't be removed from here. Remove it there by hand.", file->name );
		return qtrue;
	}
	if ( file->numUses ) {
		SV_InUseWords( file, change->message, sizeof( change->message ) );
		change->code = "in_use";
		return qtrue;
	}
	if ( !strcmp( file->state, "pending" ) ) {
		if ( !SV_KeepAside( file->game, file->name, file->origin, &keptCopy ) ) {
			SV_ChangeSay( change, "failed", "The server couldn't remove %s: the file can't be moved.", file->name );
			return qtrue;
		}
		SV_RecordWrite();
		// its maps go from the pending ones now, not at the next frame's look
		SV_PendingChanged();
		file->state = "removed";
		change->applies = "now";
		if ( keptCopy ) {
			SV_ChangeSay( change, "removed", "%s is removed. A copy is kept for %d days.", file->name,
				INSTALLED_KEEP_DAYS );
		} else {
			SV_ChangeSay( change, "removed", "%s is removed. It couldn't be moved into the content folder, so "
				"no copy is kept.", file->name );
		}
		return qtrue;
	}
	if ( strcmp( file->state, "removing" ) ) {
		if ( !( rec = SV_RecordPut( REC_REMOVING, file->game, file->name ) ) ) {
			SV_ChangeSay( change, "failed", "The server couldn't remove %s: it's out of memory.", file->name );
			return qtrue;
		}
		Content_FileStat( SV_HomePak( file->game, file->name ), &rec->size, &rec->mtime );
		rec->at = time( NULL );
		Q_strncpyz( rec->origin, file->origin, sizeof( rec->origin ) );
		SV_RecordWrite();
		SV_PendingChanged();
		file->state = "removing";
		Com_Printf( "content remove: %s/%s goes as the next map loads\n", file->game, file->name );
	}
	change->applies = "next_session";
	SV_ChangeSay( change, "removed", SV_InstalledPlayers() ? "%s will be removed when the map changes. Players joining "
		"before then can still download it. A copy is kept for %d days." : "%s will be removed when the map changes. "
		"Nobody is playing, so you can change the map now. A copy is kept for %d days.", file->name,
		INSTALLED_KEEP_DAYS );
	return qtrue;
}

/*
=================
SV_ExtContentRestore

content restore: a removal called off, or a kept copy brought back, after
content add's checks, for the next map. qfalse while an add or an export
runs
=================
*/
qboolean SV_ExtContentRestore( const char *name, const char *gameDir, extContentChange_t *change ) {
	extContentFile_t	*file;
	extContentVerdict_t	verdict;
	record_t			*rec, kept;
	const char			*refused;
	char				from[MAX_OSPATH], to[MAX_OSPATH], base[INSTALLED_NAME];
	unsigned int		checksum;
	int64_t				size, mtime;

	Com_Memset( change, 0, sizeof( *change ) );
	if ( SV_ContentWorking() ) {
		return qfalse;
	}
	if ( !SV_ChangeNames( change, name, &gameDir ) ) {
		return qtrue;
	}
	SV_RecordRead();
	if ( ( rec = SV_RecordFind( REC_REMOVING, gameDir, name ) ) != NULL ) {
		SV_RecordDrop( rec );
		SV_RecordWrite();
		SV_PendingChanged();
		SV_ChangeFind( change, name, gameDir, qfalse );
		change->applies = "now";
		SV_ChangeSay( change, "restored", "%s stays: its removal is called off.", name );
		Com_Printf( "content restore: %s/%s's removal is called off\n", gameDir, name );
		return qtrue;
	}
	if ( !( file = SV_ChangeFind( change, name, gameDir, qtrue ) ) ||
		!( rec = SV_RecordFind( REC_KEPT, file->game, file->name ) ) ) {
		SV_ChangeSay( change, "not_found", "There's no removed copy of %s to bring back.", name );
		return qtrue;
	}
	kept = *rec;
	Q_strncpyz( from, SV_KeptPak( SV_ContentDir(), kept.game, kept.name ), sizeof( from ) );
	Q_strncpyz( to, SV_HomePak( kept.game, kept.name ), sizeof( to ) );
	COM_StripExtension( kept.name, base, sizeof( base ) );
	Com_Memset( &verdict, 0, sizeof( verdict ) );
	refused = FS_CheckAddPak( from, kept.game, base, &checksum, NULL, 0, &verdict );
	if ( refused ) {
		SV_ChangeSay( change, verdict.code ? verdict.code : "failed", "%s can't come back: %s.", kept.name, refused );
		return qtrue;
	}
	if ( Content_FileStat( to, &size, &mtime ) ) {
		if ( size != kept.size || !FS_CompareZipChecksum( to, checksum ) ) {
			SV_ChangeSay( change, "other_contents", "A different file named %s is installed now, so the removed one "
				"can't come back under that name.", kept.name );
			return qtrue;
		}
		// the same file: its copy isn't needed
		remove( from );
		SV_RecordDrop( rec );
		SV_RecordWrite();
		SV_ChangeSay( change, "installed_already", "%s is installed already, the same file.", kept.name );
		return qtrue;
	}
	FS_CreatePath( to );
	if ( !Sys_Rename( from, to ) ) {
		SV_ChangeSay( change, "failed", "The server couldn't bring %s back: the file can't be moved.", kept.name );
		return qtrue;
	}
	SV_RecordDrop( rec );
	if ( ( rec = SV_RecordPut( REC_FILE, kept.game, kept.name ) ) != NULL &&
		Content_FileStat( to, &rec->size, &rec->mtime ) ) {
		rec->at = time( NULL );
		Q_strncpyz( rec->origin, kept.origin, sizeof( rec->origin ) );
	}
	SV_RecordWrite();
	SV_AddPending( to, kept.game );
	// its entry again, as it's pending now
	SV_ChangeFind( change, kept.name, kept.game, qfalse );
	change->applies = "next_session";
	SV_ChangeSay( change, "restored", "%s is restored. The server reads it again when the map changes.", kept.name );
	Com_Printf( "content restore: %s/%s is back, for the next map\n", kept.game, kept.name );
	return qtrue;
}

void SV_ExtContentChangeFree( extContentChange_t *change ) {
	SV_ExtContentListFree( &change->list );
	change->entry = NULL;
}

/*
=================
SV_InstalledAdded

content add installed a pak at ospath: recorded, with its size and time,
so the list says it was added; and any removal of its name called off, as
adding it again says to keep it (SV_InstalledKeep)
=================
*/
void SV_InstalledAdded( const char *gameDir, const char *given, const char *ospath ) {
	record_t	*rec;
	char		name[INSTALLED_NAME];

	// a copy: the caller's may be va's, which reading the record reuses
	Q_strncpyz( name, given, sizeof( name ) );
	SV_RecordRead();
	if ( ( rec = SV_RecordFind( REC_REMOVING, gameDir, name ) ) != NULL ) {
		SV_RecordDrop( rec );
		SV_PendingChanged();
	}
	if ( ospath && ( rec = SV_RecordPut( REC_FILE, gameDir, name ) ) != NULL ) {
		Content_FileStat( ospath, &rec->size, &rec->mtime );
		rec->at = time( NULL );
		Q_strncpyz( rec->origin, "added", sizeof( rec->origin ) );
	}
	SV_RecordWrite();
}

/*
=================
SV_ExtMapRemoving

Whether a map is only in paks waiting to be removed, so it's gone from
the next map on: neither SV_ExtHasMap nor Omniwire's map list offers it
=================
*/
static int QDECL SV_CompareMapNames( const void *a, const void *b ) {
	return Q_stricmp( a, b );
}

// whether a pak the search path reads is one waiting to be removed: the
// home's, recorded so
static qboolean SV_PakRemoving( const fsPak_t *pak ) {
	char	name[INSTALLED_NAME];

	Com_sprintf( name, sizeof( name ), "%s.pk3", pak->baseName );
	return SV_RecordFind( REC_REMOVING, pak->gameDir, name ) && SV_SamePath( pak->ospath, SV_HomePak( pak->gameDir, name ) );
}

static void SV_RemovingMapsAdd( void *user, const char *map ) {
	char	(*grown)[MAX_QPATH];
	int		*room = user;

	if ( inst.numRemovingMaps == *room ) {
		if ( !( grown = realloc( inst.removingMaps, ( *room + 64 ) * sizeof( *grown ) ) ) ) {
			return;
		}
		inst.removingMaps = grown;
		*room += 64;
	}
	Q_strncpyz( inst.removingMaps[inst.numRemovingMaps++], map, MAX_QPATH );
}

static void SV_RemovingMapsOf( void *user, const fsPak_t *pak ) {
	if ( SV_PakRemoving( pak ) ) {
		FS_EachPakMap( pak, SV_RemovingMapsAdd, user );
	}
}

// a map another pak has: marked in its last byte, which a map's name,
// shorter than MAX_QPATH - 1 (FS_EachPakMap), leaves '\0', so the order
// holds for the lookups
static void SV_RemovingMapsDrop( void *user, const char *map ) {
	char	*found = bsearch( map, inst.removingMaps, inst.numRemovingMaps, MAX_QPATH, SV_CompareMapNames );

	(void)user;
	if ( found ) {
		found[MAX_QPATH - 1] = 1;
	}
}

static void SV_RemovingMapsOthers( void *user, const fsPak_t *pak ) {
	if ( !SV_PakRemoving( pak ) ) {
		FS_EachPakMap( pak, SV_RemovingMapsDrop, user );
	}
}

static void SV_RemovingMapsRead( void ) {
	int		room = 0, i, j;

	inst.removingMapsAt = inst.changes + inst.mapLoads;
	inst.numRemovingMaps = 0;
	free( inst.removingMaps );
	inst.removingMaps = NULL;
	SV_RecordReady();
	FS_EachPak( SV_RemovingMapsOf, &room );
	if ( !inst.numRemovingMaps ) {
		return;
	}
	qsort( inst.removingMaps, inst.numRemovingMaps, MAX_QPATH, SV_CompareMapNames );
	for ( i = j = 0; i < inst.numRemovingMaps; i++ ) {
		if ( !j || Q_stricmp( inst.removingMaps[i], inst.removingMaps[j - 1] ) ) {
			memmove( inst.removingMaps[j++], inst.removingMaps[i], MAX_QPATH );
		}
	}
	inst.numRemovingMaps = j;
	FS_EachPak( SV_RemovingMapsOthers, NULL );
	for ( i = j = 0; i < inst.numRemovingMaps; i++ ) {
		if ( !inst.removingMaps[i][MAX_QPATH - 1] ) {
			memmove( inst.removingMaps[j++], inst.removingMaps[i], MAX_QPATH );
		}
	}
	inst.numRemovingMaps = j;
}

qboolean SV_ExtMapRemoving( const char *map ) {
	// they change only with the record and the paks a map load reads
	if ( inst.removingMapsAt != inst.changes + inst.mapLoads ) {
		SV_RemovingMapsRead();
	}
	return inst.numRemovingMaps && bsearch( map, inst.removingMaps, inst.numRemovingMaps, MAX_QPATH,
		SV_CompareMapNames ) != NULL;
}

// why a removing pak stays at a map load (SV_InstalledMapLoad)
#define LOAD_HOLDS_MAP	1		// it holds the map loading
#define LOAD_ROTATION	2		// a map of its is in the rotation

typedef struct {
	const char	*map;
	inUse_t		use;
	byte		*why;				// by record
	qboolean	elsewhere;			// a pak not removing holds the map loading
	int			index;				// the record of the pak being walked
} loadHolds_t;

static void SV_LoadRotation( void *user, const char *map ) {
	loadHolds_t	*holds = user;

	if ( SV_InPool( &holds->use, map ) ) {
		holds->why[holds->index] |= LOAD_ROTATION;
	}
}

static void SV_LoadHolds( void *user, const fsPak_t *pak ) {
	loadHolds_t	*holds = user;
	char		name[INSTALLED_NAME];
	qboolean	holdsMap = FS_PakHolds( pak, va( "maps/%s.bsp", holds->map ) );

	if ( !SV_PakRemoving( pak ) ) {
		holds->elsewhere |= holdsMap;
		return;
	}
	Com_sprintf( name, sizeof( name ), "%s.pk3", pak->baseName );
	holds->index = SV_RecordFind( REC_REMOVING, pak->gameDir, name ) - inst.records;
	if ( holdsMap ) {
		holds->why[holds->index] |= LOAD_HOLDS_MAP;
	}
	FS_EachPakMap( pak, SV_LoadRotation, holds );
}

/*
=================
SV_LoadRemove

The paks waiting to be removed moved aside, once the file system has let
go of every one, but those the map loading or the rotation keeps; whether
the record changed
=================
*/
static qboolean SV_LoadRemove( const char *server ) {
	loadHolds_t	holds;
	record_t	*due, *rec;
	byte		*dueWhy;
	int64_t		size, mtime;
	qboolean	changed = qfalse;
	int			i, numDue = 0;

	Com_Memset( &holds, 0, sizeof( holds ) );
	holds.map = server;
	holds.why = calloc( inst.count, 1 );
	due = malloc( inst.count * sizeof( *due ) );
	dueWhy = malloc( inst.count );
	if ( !holds.why || !due || !dueWhy ) {
		free( holds.why );
		free( due );
		free( dueWhy );
		return qfalse;
	}
	SV_InUseRead( &holds.use );
	FS_EachPak( SV_LoadHolds, &holds );
	free( holds.use.pool );
	// copies, as moving one aside drops and adds records
	for ( i = 0; i < inst.count; i++ ) {
		if ( inst.records[i].kind == REC_REMOVING ) {
			due[numDue] = inst.records[i];
			dueWhy[numDue++] = holds.why[i];
		}
	}
	free( holds.why );
	// let go of every pak: FS_Restart starts it again
	FS_Shutdown( qfalse );
	for ( i = 0; i < numDue; i++ ) {
		if ( ( dueWhy[i] & LOAD_HOLDS_MAP ) && !holds.elsewhere ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: content remove: %s/%s holds %s, the map loading, so it goes at the next "
				"map instead\n", due[i].game, due[i].name, server );
			continue;
		}
		if ( dueWhy[i] & LOAD_ROTATION ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: content remove: %s/%s holds a map in the rotation now, so it stays until "
				"that's taken out\n", due[i].game, due[i].name );
			continue;
		}
		if ( Content_FileStat( SV_HomePak( due[i].game, due[i].name ), &size, &mtime ) &&
			( size != due[i].size || mtime != due[i].mtime ) ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: content remove: %s/%s changed since it was removed, so it stays\n",
				due[i].game, due[i].name );
		} else if ( !SV_KeepAside( due[i].game, due[i].name, due[i].origin, NULL ) ) {
			continue;
		}
		if ( ( rec = SV_RecordFind( REC_REMOVING, due[i].game, due[i].name ) ) != NULL ) {
			SV_RecordDrop( rec );
		}
		changed = qtrue;
	}
	free( due );
	free( dueWhy );
	return changed;
}

/*
=================
SV_InstalledMapLoad

A map, server, is about to load, and the file system to read its paks
again (SV_SpawnServer's FS_Restart): the paks waiting to be removed go,
once the file system has let go of every one, as Windows won't move an
open file. One that holds the map loading, and no other pak does, stays
until the next, so a load never fails for a removal, and so does one with
a map put back in the rotation since it was taken; one changed since
it was taken stays, as it's another file now; one that can't be moved now
waits for the next map. And the kept copies past their time are pruned
=================
*/
void SV_InstalledMapLoad( const char *server ) {
	record_t	*rec;
	int64_t		now = time( NULL );
	const char	*root;
	qboolean	changed = qfalse;
	int			i;

	inst.mapLoads++;
	SV_RecordRead();
	for ( i = 0; i < inst.count && inst.records[i].kind != REC_REMOVING; i++ ) {
	}
	if ( i < inst.count ) {
		changed = SV_LoadRemove( server );
	}
	// the kept copies past their time, or gone; sv_contentDir only looked
	// at (SV_ContentRoot makes it) when there are some, and none dropped
	// while it can't be used, so none is left on disk unrecorded
	for ( i = 0; i < inst.count && inst.records[i].kind != REC_KEPT; i++ ) {
	}
	root = i < inst.count ? SV_ContentDir() : NULL;
	for ( i = inst.count - 1; root && i >= 0; i-- ) {
		rec = &inst.records[i];
		if ( rec->kind == REC_KEPT && ( rec->until <= now ||
				!SV_Stat( SV_KeptPak( root, rec->game, rec->name ), NULL ) ) ) {
			remove( SV_KeptPak( root, rec->game, rec->name ) );
			SV_RecordDrop( rec );
			changed = qtrue;
		}
	}
	if ( changed ) {
		SV_RecordWrite();
		SV_PendingChanged();
	}
}

/*
==============================================================

THE CONSOLE

==============================================================
*/

void SV_InstalledList_f( const char *game ) {
	extContentList_t		list;
	const extContentFile_t	*file;
	char					when[64];
	int						i, j;

	SV_RecordRead();
	SV_ExtContentList( game, qtrue, qtrue, &list );
	for ( i = 0; i < list.count; i++ ) {
		file = &list.files[i];
		when[0] = '\0';
		if ( file->keptUntil ) {
			Com_sprintf( when, sizeof( when ), ", kept %lld more days", (long long)( ( file->keptUntil - time( NULL ) +
				86399 ) / 86400 ) );
		}
		Com_Printf( "%s/%s: %lld KB, %s, %s%s, %s%s%s%s\n", file->game, file->name, (long long)( ( file->size + 1023 ) >> 10 ),
			file->origin, file->state, when, file->home ? "the server's folder" : "the install's folder",
			!file->gameOwn && file->mapPack ? ", a map pack" : "", file->gameOwn || !file->gameCode ? "" :
			file->codeLeftOut ? ", game code left out" : ", game code", file->removable ? "" : ", can't be removed" );
		for ( j = 0; j < file->numMaps; j++ ) {
			Com_Printf( "  %s\n", file->maps[j] );
		}
		for ( j = 0; j < file->numUses; j++ ) {
			Com_Printf( "  in use: %s, %s\n", file->uses[j].map, file->uses[j].why );
		}
	}
	Com_Printf( "%d paks, %lld MB\n", list.installed, (long long)( ( list.bytes + ( 1 << 19 ) ) >> 20 ) );
	SV_ExtContentListFree( &list );
}

void SV_InstalledChange_f( qboolean restore, const char *name, const char *game ) {
	extContentChange_t	change;

	if ( !( restore ? SV_ExtContentRestore : SV_ExtContentRemove )( name, game, &change ) ) {
		Com_Printf( "content %s: an add or an export is running; try again once it's done\n", restore ? "restore" :
			"remove" );
		return;
	}
	Com_Printf( "%s\n", change.message );
	SV_ExtContentChangeFree( &change );
}
