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
// content_sources.c -- the content cache's side file, sources.dat: for each
// cached pak, by its SHA-256, the servers that sent or used it, so a server
// that names a pak by its checksum alone gets only one it sent itself
//
// Little-endian. A 16-byte header: "OFCS", the format (2 bytes), 2 reserved,
// the record count (4), 4 reserved. Each record carries its own length, and
// each server entry the size the record gives, so a later format adds fields
// only at the end of either, which this engine skips as it reads and keeps,
// byte for byte, as it writes the file again. A record, at its start:
//
//   0  4  its length, these bytes included
//   4 32  the pak's SHA-256
//  36  8  stored, Unix seconds
//  44  8  last used
//  52  1  how it came (contentSource_t)
//  53  1  reserved
//  54  2  the size of each server entry, at least SOURCES_SERVER_SIZE
//  56  2  how many server entries follow
//  58  2  reserved
//  60     the server entries, then the rest of the record
//
// A server entry:
//
//   0  1  kind (contentServerKind_t): an unknown one is kept and never matches
//   1  1  flags: SOURCES_SENT, SOURCES_USED
//   2  2  the port, for an address
//   4  4  reserved
//   8  8  when it last sent or used the pak, Unix seconds
//  16 32  the server's address (4 or 16 bytes, zero-padded), or a SHA-256:
//         of a key and the address it was proved at (Content_ServerAt), or,
//         kind NAMED, of a name: kept, though no engine makes one since
//         a web connection became none (Content_ServerOf)
//
// Reserved bytes, the header's and a record's, are kept as read, as are a
// later engine's fields and entries of a kind this engine doesn't know,
// which it never gives up for its own. A record is dropped once its pak
// leaves the cache.
//
// The format changes only where an older engine couldn't read the newer
// file's start this way, and an engine that meets a higher one leaves the
// file alone

#include "content.h"

/*
=================
Content_ServerAt
=================
*/
void Content_ServerAt( contentServer_t *out, const byte key[IDENT_KEY_SIZE], const contentServer_t *address ) {
	byte	both[IDENT_KEY_SIZE + 3 + IDENT_KEY_SIZE];

	memcpy( both, key, IDENT_KEY_SIZE );
	both[IDENT_KEY_SIZE] = address->kind;
	Content_Put16( both + IDENT_KEY_SIZE + 1, address->port );
	memcpy( both + IDENT_KEY_SIZE + 3, address->id, IDENT_KEY_SIZE );
	memset( out, 0, sizeof( *out ) );
	out->kind = CONTENT_SERVER_KEY_AT;
	Com_SHA256Buffer( both, sizeof( both ), out->id );
}

/*
=================
Content_ServerOf

The server a connection goes to, as the cache knows it: the resolved
address and port it was reached at, never a host name, and with key, the
key it proved there. Any other connection is none, a web one too: its
address is an id each page starts again, and its name ("web:N", or a
join's code) names no server, so even a key proved there could be any
relay's, and only this connection's note holds (Content_Cache_SentHere)
=================
*/
void Content_ServerOf( contentServer_t *out, const netadr_t *to, const byte *key ) {
	memset( out, 0, sizeof( *out ) );
	if ( to->type != NA_IP && to->type != NA_IP6 ) {
		return;
	}
	out->kind = to->type == NA_IP ? CONTENT_SERVER_IPV4 : CONTENT_SERVER_IPV6;
	out->port = (unsigned short)BigShort( to->port );
	if ( to->type == NA_IP ) {
		memcpy( out->id, to->ip, sizeof( to->ip ) );
	} else {
		memcpy( out->id, to->ip6, sizeof( to->ip6 ) );
	}
	if ( key ) {
		Content_ServerAt( out, key, out );
	}
}

// whether this engine knows an entry's kind, which it may give up for another
static qboolean Sources_KnownKind( int kind ) {
	return kind >= CONTENT_SERVER_KEY_AT && kind <= CONTENT_SERVER_NAMED;
}

// a server entry's identity, as the file keeps it
static void Sources_PutServer( byte *p, const contentServer_t *server ) {
	p[0] = server->kind;
	Content_Put16( p + 2, server->port );
	memcpy( p + 16, server->id, sizeof( server->id ) );
}

static qboolean Sources_IsServer( const byte *p, const contentServer_t *server ) {
	return server->kind != CONTENT_SERVER_NONE && p[0] == server->kind &&
		Content_Get16( p + 2 ) == server->port && !memcmp( p + 16, server->id, sizeof( server->id ) );
}

/*
=================
Content_ParseSources

The records of sources.dat's bytes, each one's servers and rest allocated,
and its header: how many; -1 if it's damaged (a record past the file's
end or shorter than its servers, a hash twice), -2 if this engine can't
hold it, a later format or more records than maxRecords, which its caller
leaves alone
=================
*/
int Content_ParseSources( const byte *data, int length, sourceRecord_t *records, int maxRecords,
	byte header[CONTENT_HEADER_SIZE] ) {
	sourceRecord_t	*r;
	const byte		*p;
	unsigned int	count, recordLength;
	int				i, j, at, serversLength;

	if ( length < CONTENT_HEADER_SIZE || memcmp( data, CACHE_SOURCES_MAGIC, 4 ) ) {
		return -1;
	}
	if ( Content_Get16( data + 4 ) != CACHE_SOURCES_FORMAT ) {
		return Content_Get16( data + 4 ) > CACHE_SOURCES_FORMAT ? -2 : -1;
	}
	count = Content_Get32( data + 8 );
	if ( count > (unsigned int)maxRecords ) {
		return -2;
	}
	memcpy( header, data, CONTENT_HEADER_SIZE );
	at = CONTENT_HEADER_SIZE;
	for ( i = 0; i < (int)count; i++ ) {
		r = &records[i];
		memset( r, 0, sizeof( *r ) );
		p = data + at;
		if ( length - at < SOURCES_RECORD_HEAD ) {
			break;
		}
		recordLength = Content_Get32( p );
		if ( recordLength < SOURCES_RECORD_HEAD || recordLength > (unsigned int)( length - at ) ) {
			break;
		}
		memcpy( r->sha256, p + 4, SHA256_DIGEST_SIZE );
		r->stored = Content_Get64( p + 36 );
		r->lastUsed = Content_Get64( p + 44 );
		r->source = p[52];
		r->reserved[0] = p[53];
		r->reserved[1] = p[58];
		r->reserved[2] = p[59];
		r->serverSize = Content_Get16( p + 54 );
		r->serverCount = Content_Get16( p + 56 );
		// in 64 bits: two 16-bit fields' product passes an int. Within the
		// record, which is within the file, every later product fits one
		if ( r->serverSize < SOURCES_SERVER_SIZE ||
			(int64_t)r->serverSize * r->serverCount > (int64_t)recordLength - SOURCES_RECORD_HEAD ) {
			break;
		}
		serversLength = r->serverSize * r->serverCount;
		for ( j = 0; j < i; j++ ) {
			if ( !memcmp( records[j].sha256, r->sha256, SHA256_DIGEST_SIZE ) ) {
				break;
			}
		}
		if ( j < i ) {
			break;
		}
		r->tailLength = recordLength - SOURCES_RECORD_HEAD - serversLength;
		r->servers = malloc( serversLength + r->tailLength + 1 );
		if ( !r->servers ) {
			break;
		}
		memcpy( r->servers, p + SOURCES_RECORD_HEAD, serversLength + r->tailLength );
		at += recordLength;
	}
	if ( i < (int)count || at != length ) {
		Content_FreeSources( records, i );
		return -1;
	}
	return count;
}

/*
=================
Content_SourcesSize

The bytes Content_WriteSources needs for these records
=================
*/
int Content_SourcesSize( const sourceRecord_t *records, int count ) {
	int64_t	size = CONTENT_HEADER_SIZE;
	int		i;

	for ( i = 0; i < count; i++ ) {
		size += SOURCES_RECORD_HEAD + (int64_t)records[i].serverSize * records[i].serverCount + records[i].tailLength;
	}
	return size > INT_MAX ? -1 : (int)size;
}

/*
=================
Content_WriteSources

sources.dat's bytes for these records, each one's servers and rest as they
are, with header's reserved bytes (NULL: none): the length written, -1 if
out is too small
=================
*/
int Content_WriteSources( const sourceRecord_t *records, int count, const byte *header, byte *out, int size ) {
	const sourceRecord_t	*r;
	byte					*p;
	int						i, length = Content_SourcesSize( records, count ), serversLength;

	if ( length < 0 || length > size ) {
		return -1;
	}
	if ( header ) {
		memcpy( out, header, CONTENT_HEADER_SIZE );
	} else {
		memset( out, 0, CONTENT_HEADER_SIZE );
	}
	memcpy( out, CACHE_SOURCES_MAGIC, 4 );
	Content_Put16( out + 4, CACHE_SOURCES_FORMAT );
	Content_Put32( out + 8, count );
	p = out + CONTENT_HEADER_SIZE;
	for ( i = 0; i < count; i++ ) {
		r = &records[i];
		serversLength = r->serverSize * r->serverCount;
		memset( p, 0, SOURCES_RECORD_HEAD );
		Content_Put32( p, SOURCES_RECORD_HEAD + serversLength + r->tailLength );
		memcpy( p + 4, r->sha256, SHA256_DIGEST_SIZE );
		Content_Put64( p + 36, r->stored );
		Content_Put64( p + 44, r->lastUsed );
		p[52] = r->source;
		p[53] = r->reserved[0];
		p[58] = r->reserved[1];
		p[59] = r->reserved[2];
		// a record made with no server yet has this format's entry size
		Content_Put16( p + 54, r->serverSize ? r->serverSize : SOURCES_SERVER_SIZE );
		Content_Put16( p + 56, r->serverCount );
		if ( serversLength + r->tailLength ) {
			memcpy( p + SOURCES_RECORD_HEAD, r->servers, serversLength + r->tailLength );
		}
		p += SOURCES_RECORD_HEAD + serversLength + r->tailLength;
	}
	return length;
}

/*
=================
Content_FreeSources
=================
*/
void Content_FreeSources( sourceRecord_t *records, int count ) {
	int		i;

	for ( i = 0; i < count; i++ ) {
		free( records[i].servers );
		records[i].servers = NULL;
	}
}

/*
=================
Content_Sources_Has

Whether a record names the server with all of these flags
=================
*/
qboolean Content_Sources_Has( const sourceRecord_t *r, const contentServer_t *server, int flags ) {
	const byte	*p;
	int			i;

	for ( i = 0; i < r->serverCount; i++ ) {
		p = r->servers + i * r->serverSize;
		if ( Sources_IsServer( p, server ) && ( p[1] & flags ) == flags ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
Content_Sources_Note

Adds these flags to the record's entry for the server, made if it has
none, at now. A record full at SOURCES_MAX_SERVERS gives up the entry
least recently noted that didn't send the pak, then the least recently
noted, so servers that only used it don't push out one that sent it,
and never one of a kind a later engine knows and this one doesn't. A
record whose entries are longer than SOURCES_MAX_ENTRY, a later engine's,
isn't grown. qfalse if no entry was noted
=================
*/
qboolean Content_Sources_Note( sourceRecord_t *r, const contentServer_t *server, int flags, int64_t now ) {
	byte	*p, *grown;
	int		i, oldest = -1, oldestUsed = -1, serversLength;

	if ( server->kind == CONTENT_SERVER_NONE ) {
		return qfalse;
	}
	if ( !r->serverSize ) {
		r->serverSize = SOURCES_SERVER_SIZE;
	}
	for ( i = 0; i < r->serverCount; i++ ) {
		p = r->servers + i * r->serverSize;
		if ( Sources_IsServer( p, server ) ) {
			p[1] |= flags;
			Content_Put64( p + 8, now );
			return qtrue;
		}
		if ( !Sources_KnownKind( p[0] ) ) {
			continue;
		}
		if ( oldest < 0 || Content_Get64( p + 8 ) < Content_Get64( r->servers + oldest * r->serverSize + 8 ) ) {
			oldest = i;
		}
		if ( !( p[1] & SOURCES_SENT ) && ( oldestUsed < 0 ||
			Content_Get64( p + 8 ) < Content_Get64( r->servers + oldestUsed * r->serverSize + 8 ) ) ) {
			oldestUsed = i;
		}
	}
	serversLength = r->serverSize * r->serverCount;
	if ( r->serverSize > SOURCES_MAX_ENTRY || ( r->serverCount >= SOURCES_MAX_SERVERS && oldest < 0 ) ) {
		return qfalse;
	}
	if ( r->serverCount >= SOURCES_MAX_SERVERS ) {
		p = r->servers + ( oldestUsed >= 0 ? oldestUsed : oldest ) * r->serverSize;
	} else {
		// a new entry before the rest of the record, which moves along
		grown = realloc( r->servers, serversLength + r->serverSize + r->tailLength + 1 );
		if ( !grown ) {
			return qfalse;
		}
		memmove( grown + serversLength + r->serverSize, grown + serversLength, r->tailLength );
		r->servers = grown;
		p = grown + serversLength;
		r->serverCount++;
	}
	memset( p, 0, r->serverSize );
	Sources_PutServer( p, server );
	p[1] = flags;
	Content_Put64( p + 8, now );
	return qtrue;
}
