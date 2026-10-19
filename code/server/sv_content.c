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
// sv_content.c -- the server's content manifest: the paks a client may
// fetch from it, by SHA-256 (content.h), built at each map load and again
// as the pak index learns more hashes. The current one and the one before
// are kept, so a transfer that spans a change still finds its hash, each
// with a table of where its paks are, which never goes on the wire

#include "server.h"
#include "../qcommon/content.h"

typedef struct {
	byte		sha256[SHA256_DIGEST_SIZE];
	char		*ospath;
	int64_t		size;
	int64_t		mtime;
} svManifestPak_t;

typedef struct {
	byte			*data;
	int				length;
	byte			sha256[SHA256_DIGEST_SIZE];
	uint64_t		requiredBytes;
	svManifestPak_t	*paks;			// its entries with a hash
	int				numPaks;
} svManifest_t;

static svManifest_t	svManifests[2];		// the current one, then the one before
static byte			svBuildBuffer[OFCM_MAX_SIZE];

// the paks left out of a content manifest already said, by checksum and
// why: an open addressing set, which stops taking more once nearly full
#define LEFT_OUT_SLOTS	8192
static struct {
	unsigned int		checksum;
	manifestListing_t	why;		// MANIFEST_LISTED for an empty slot
} svLeftOut[LEFT_OUT_SLOTS];
static int	svNumLeftOut;

/*
=================
SV_ContentLeftOutReason
=================
*/
static const char *SV_ContentLeftOutReason( manifestListing_t why ) {
	switch ( why ) {
	case MANIFEST_UNHASHED:
		return "isn't hashed yet";
	case MANIFEST_UNREADABLE:
		return "can't be read";
	case MANIFEST_BAD_NAME:
		return "has a name no download may have";
	case MANIFEST_SHADOWED:
		return "has the name of a pak found before it";
	case MANIFEST_TOO_MANY:
		return "is past the 4,096 paks a content manifest holds";
	default:
		return NULL;
	}
}

/*
=================
SV_ContentNoteLeftOut

Says once why a pak isn't offered by its hash; a client that needs it
gets it the classic way
=================
*/
static void SV_ContentNoteLeftOut( const contentPak_t *pak, manifestListing_t why ) {
	const char		*reason = SV_ContentLeftOutReason( why );
	unsigned int	slot;

	if ( !reason ) {
		return;
	}
	for ( slot = ( pak->checksum * 7 + why ) % LEFT_OUT_SLOTS; svLeftOut[slot].why != MANIFEST_LISTED;
		slot = ( slot + 1 ) % LEFT_OUT_SLOTS ) {
		if ( svLeftOut[slot].checksum == pak->checksum && svLeftOut[slot].why == why ) {
			return;
		}
	}
	if ( svNumLeftOut < LEFT_OUT_SLOTS * 7 / 8 ) {
		svLeftOut[slot].checksum = pak->checksum;
		svLeftOut[slot].why = why;
		svNumLeftOut++;
	}
	Com_Printf( "Content manifest: %s/%s.pk3 %s, so it's sent the classic way\n", pak->gameDir, pak->name, reason );
}

static void SV_ContentFree( svManifest_t *manifest ) {
	int		i;

	if ( manifest->data ) {
		Z_Free( manifest->data );
	}
	for ( i = 0; i < manifest->numPaks; i++ ) {
		Z_Free( manifest->paks[i].ospath );
	}
	if ( manifest->paks ) {
		Z_Free( manifest->paks );
	}
	Com_Memset( manifest, 0, sizeof( *manifest ) );
}

static int SV_ContentCompareChecksums( const void *a, const void *b ) {
	unsigned int	ca = *(const unsigned int *)a, cb = *(const unsigned int *)b;

	return ca < cb ? -1 : ca > cb;
}

/*
=================
SV_ContentReferenced

Marks as required the paks whose checksums sv_referencedPaks lists, as
the map load set it. The paks' own references grow after that as files
are read (a listen server's client loading the map, a bot joining), and
a content manifest built after a hashing job must still require what the
clients were told to have, no more
=================
*/
static void SV_ContentReferenced( contentPak_t *paks, int count ) {
	const char		*s = Cvar_VariableString( "sv_referencedPaks" );
	unsigned int	*listed;
	char			*end;
	long			value;
	int				i, numListed = 0;

	listed = Z_Malloc( ( strlen( s ) / 2 + 1 ) * sizeof( *listed ) );	// "%i " each
	for ( ;; s = end ) {
		value = strtol( s, &end, 10 );
		if ( end == s ) {
			break;
		}
		listed[numListed++] = (unsigned int)(int)value;
	}
	qsort( listed, numListed, sizeof( *listed ), SV_ContentCompareChecksums );
	for ( i = 0; i < count; i++ ) {
		paks[i].referenced = bsearch( &paks[i].checksum, listed, numListed, sizeof( *listed ),
			SV_ContentCompareChecksums ) != NULL;
	}
	Z_Free( listed );
}

/*
=================
SV_ContentBuildManifest

At a map load, after the referenced paks are known, and whenever the pak
index learns hashes
=================
*/
void SV_ContentBuildManifest( void ) {
	contentPak_t		*paks;
	manifestListing_t	*listing;
	ofcmHeader_t		header;
	svManifest_t		built;
	const char			*error = NULL;
	char				hex[SHA256_HEX_SIZE];
	int					i, count, unhashed = 0;

	count = FS_ContentPaks( NULL, 0 );
	paks = Z_Malloc( ( count + 1 ) * sizeof( *paks ) );
	listing = Z_Malloc( ( count + 1 ) * sizeof( *listing ) );
	count = FS_ContentPaks( paks, count );
	SV_ContentReferenced( paks, count );

	Com_Memset( &built, 0, sizeof( built ) );
	built.length = Content_BuildManifest( paks, count, FS_ValidDownloadPak, listing, &header,
		svBuildBuffer, sizeof( svBuildBuffer ), &error );
	if ( built.length && ( svManifests[0].length != built.length
		|| memcmp( svManifests[0].data, svBuildBuffer, built.length ) ) ) {
		built.data = Z_Malloc( built.length );
		Com_Memcpy( built.data, svBuildBuffer, built.length );
		Com_SHA256Buffer( built.data, built.length, built.sha256 );
		built.requiredBytes = header.requiredBytes;
		built.paks = Z_Malloc( ( header.numEntries + 1 ) * sizeof( *built.paks ) );
	}
	for ( i = 0; i < count; i++ ) {
		SV_ContentNoteLeftOut( &paks[i], listing[i] );
		unhashed += listing[i] == MANIFEST_UNHASHED;
		if ( built.data && listing[i] == MANIFEST_LISTED && paks[i].sha256 && !paks[i].refused ) {
			svManifestPak_t	*p = &built.paks[built.numPaks++];

			Com_Memcpy( p->sha256, paks[i].sha256, SHA256_DIGEST_SIZE );
			p->ospath = CopyString( paks[i].ospath );
			p->size = paks[i].size;
			p->mtime = paks[i].mtime;
		}
	}
	Z_Free( listing );
	Z_Free( paks );

	if ( !built.length ) {
		// the one before stays for transfers that span the change, but
		// getinfo no longer offers it as this map's
		if ( svManifests[0].data ) {
			SV_ContentFree( &svManifests[1] );
			svManifests[1] = svManifests[0];
			Com_Memset( &svManifests[0], 0, sizeof( svManifests[0] ) );
		}
		Com_Printf( S_COLOR_YELLOW "WARNING: no content manifest: %s, so every pak is sent the classic way\n", error );
		return;
	}
	if ( !built.data ) {
		return;		// the same as the current one
	}

	SV_ContentFree( &svManifests[1] );
	svManifests[1] = svManifests[0];
	svManifests[0] = built;

	Com_SHA256Hex( built.sha256, hex );
	Com_Printf( "Content manifest %.12s: %d paks required (%llu bytes), %d optional; %d not hashed yet\n",
		hex, header.numRequired, (unsigned long long)header.requiredBytes, header.numOptional, unhashed );
}

/*
=================
SV_ContentManifest

The content manifest with that hash, the current one or the one before,
or the current one for NULL; NULL if there's none
=================
*/
const byte *SV_ContentManifest( const byte *sha256, int *length ) {
	int		i;

	for ( i = 0; i < (int)ARRAY_LEN( svManifests ); i++ ) {
		if ( svManifests[i].data && ( !sha256 || !memcmp( svManifests[i].sha256, sha256, SHA256_DIGEST_SIZE ) ) ) {
			*length = svManifests[i].length;
			return svManifests[i].data;
		}
	}
	return NULL;
}

/*
=================
SV_ContentFindPak

Where the pak with that hash is, as the current content manifest or the
one before lists it: its OS path, and the size and modification time it
had then, which the file must still have for its bytes to be the hash's;
NULL if neither lists it
=================
*/
const char *SV_ContentFindPak( const byte sha256[SHA256_DIGEST_SIZE], int64_t *size, int64_t *mtime ) {
	int		i, j;

	for ( i = 0; i < (int)ARRAY_LEN( svManifests ); i++ ) {
		for ( j = 0; j < svManifests[i].numPaks; j++ ) {
			if ( !memcmp( svManifests[i].paks[j].sha256, sha256, SHA256_DIGEST_SIZE ) ) {
				*size = svManifests[i].paks[j].size;
				*mtime = svManifests[i].paks[j].mtime;
				return svManifests[i].paks[j].ospath;
			}
		}
	}
	return NULL;
}

/*
=================
SV_ContentInfo

getinfo's content keys, for a client whose tokens ask for the content
protocol: the current content manifest's hash, and the bytes of its
required paks
=================
*/
void SV_ContentInfo( char *infostring ) {
	char	hex[SHA256_HEX_SIZE];

	if ( !svManifests[0].data ) {
		return;
	}
	Com_SHA256Hex( svManifests[0].sha256, hex );
	Info_SetValueForKey( infostring, "ofm", hex );
	Info_SetValueForKey( infostring, "ofr", va( "%llu", (unsigned long long)svManifests[0].requiredBytes ) );
}

/*
=================
SV_ContentFrame

Once a server frame: what the hash job finished goes into the pak index,
paused or not, and after the server has stopped, so the job it started is
finished; the content manifest is built again when the index learned
hashes
=================
*/
void SV_ContentFrame( void ) {
	if ( Content_PakIndex_Frame() && com_sv_running->integer ) {
		SV_ContentBuildManifest();
	}
}

/*
=================
SV_ContentShutdown
=================
*/
void SV_ContentShutdown( void ) {
	int		i;

	for ( i = 0; i < (int)ARRAY_LEN( svManifests ); i++ ) {
		SV_ContentFree( &svManifests[i] );
	}
	Com_Memset( svLeftOut, 0, sizeof( svLeftOut ) );
	svNumLeftOut = 0;
}
