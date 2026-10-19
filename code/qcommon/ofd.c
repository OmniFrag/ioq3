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
// ofd.c -- the content protocol's wire formats (ofd.h)

#include "q_shared.h"
#include "ofd.h"

// every content datagram's first bytes, then the version
static const byte	ofdMagic[7] = { 0xff, 0xff, 0xff, 0xff, 'O', 'F', 'D' };

static uint16_t Get16( const byte *p ) {
	return (uint16_t)( p[0] | p[1] << 8 );
}

static uint32_t Get32( const byte *p ) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t Get64( const byte *p ) {
	return Get32( p ) | (uint64_t)Get32( p + 4 ) << 32;
}

static void Put16( byte *p, uint16_t v ) {
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static void Put32( byte *p, uint32_t v ) {
	p[0] = v & 0xff;
	p[1] = ( v >> 8 ) & 0xff;
	p[2] = ( v >> 16 ) & 0xff;
	p[3] = v >> 24;
}

static void Put64( byte *p, uint64_t v ) {
	Put32( p, (uint32_t)v );
	Put32( p + 4, (uint32_t)( v >> 32 ) );
}

qboolean OFD_AllZero( const byte *p, int length ) {
	int		i;

	for ( i = 0; i < length; i++ ) {
		if ( p[i] ) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
================
OFD_IsContent
================
*/
qboolean OFD_IsContent( const byte *data, int length ) {
	return length >= OFD_HEADER_SIZE && !memcmp( data, ofdMagic, sizeof( ofdMagic ) );
}

/*
================
OFD_ReadKey
================
*/
uint64_t OFD_ReadKey( const byte *data ) {
	return Get64( data + 12 );
}

/*
================
OFD_ChunkSize
================
*/
int OFD_ChunkSize( int largest ) {
	if ( largest < OFD_MIN_LARGEST || largest > OFD_MAX_LARGEST ) {
		return 0;
	}
	return MIN( OFD_MAX_CHUNK, ( largest - 32 ) / 64 * 64 );
}

static qboolean OFD_ValidChunk( int chunk ) {
	return chunk >= OFD_MIN_CHUNK && chunk <= OFD_MAX_CHUNK && chunk % 64 == 0;
}

/*
================
OFCM_SizeAllowed
================
*/
qboolean OFCM_SizeAllowed( uint32_t size ) {
	return size >= OFCM_HEADER_SIZE && size <= OFCM_MAX_SIZE
		&& ( size - OFCM_HEADER_SIZE ) % OFCM_ENTRY_SIZE == 0;
}

/*
================
OFD_CheckOpen

The fields the reader and the writer both check. What only makes sense
against the request, such as the chunk size it implies, is the client's.
================
*/
static qboolean OFD_CheckOpen( const ofdOpen_t *open ) {
	if ( open->status > OFD_STATUS_READ_ERROR || open->kind > OFD_KIND_MANIFEST ) {
		return qfalse;
	}
	if ( open->status != OFD_STATUS_OK ) {
		return qtrue;
	}
	// the hash echoes the request's, or names the current content manifest:
	// never all zero, which no content has
	if ( !open->serial || !OFD_ValidChunk( open->chunkSize ) || OFD_AllZero( open->sha256, 32 ) ) {
		return qfalse;
	}
	if ( open->kind == OFD_KIND_PAK ) {
		if ( open->fileSize < OFCM_MIN_PAK || open->fileSize > OFCM_MAX_PAK ) {
			return qfalse;
		}
	} else if ( !OFCM_SizeAllowed( open->fileSize ) ) {
		return qfalse;
	}
	return open->startOffset < open->fileSize;
}

static qboolean OFD_CheckClose( const ofdClose_t *close ) {
	return close->serial && close->reason >= OFD_CLOSE_CANCELED && close->reason <= OFD_CLOSE_COMPLETE;
}

static qboolean OFD_CheckData( const ofdData_t *data ) {
	return data->serial && data->length >= 1 && data->length <= OFD_MAX_CHUNK;
}

/*
================
OFD_CheckAcks

Every range must lie at or above packet 0, so a reader can walk them
without wrapping around.
================
*/
static qboolean OFD_CheckAcks( const ofdControl_t *control ) {
	int64_t	low;
	int		i;

	if ( control->ackDelay > OFD_MAX_ACK_DELAY || !control->firstRange
		|| control->numRanges < 0 || control->numRanges > OFD_MAX_ACK_RANGES
		|| control->numTransfers < 0 || control->numTransfers > OFD_MAX_ACK_TRANSFERS ) {
		return qfalse;
	}
	low = (int64_t)control->largest - ( control->firstRange - 1 );
	if ( low < 0 ) {
		return qfalse;
	}
	for ( i = 0; i < control->numRanges; i++ ) {
		if ( !control->ranges[i].gap || !control->ranges[i].length ) {
			return qfalse;
		}
		low -= control->ranges[i].gap + control->ranges[i].length;
		if ( low < 0 ) {
			return qfalse;
		}
	}
	for ( i = 0; i < control->numTransfers; i++ ) {
		if ( !control->transfers[i].serial ) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
================
OFD_SerialsDiffer

No transfer is named twice in one CONTROL, among its acked transfers and
its cancels together
================
*/
static qboolean OFD_SerialsDiffer( const ofdControl_t *control ) {
	uint16_t	serials[OFD_MAX_ACK_TRANSFERS + OFD_MAX_CANCELS];
	int			count = 0, i, j;

	for ( i = 0; i < control->numTransfers; i++ ) {
		serials[count++] = control->transfers[i].serial;
	}
	for ( i = 0; i < control->numCancels; i++ ) {
		serials[count++] = control->cancels[i];
	}
	for ( i = 0; i < count; i++ ) {
		for ( j = i + 1; j < count; j++ ) {
			if ( serials[i] == serials[j] ) {
				return qfalse;
			}
		}
	}
	return qtrue;
}

static qboolean OFD_CheckControl( const ofdControl_t *control ) {
	int		i, j;

	if ( control->slot >= MAX_CLIENTS || control->blocks & ~( OFD_BLOCK_REQUESTS | OFD_BLOCK_ACKS
		| OFD_BLOCK_CANCELS | OFD_BLOCK_NOTICE_ACK ) ) {
		return qfalse;
	}

	if ( control->blocks & OFD_BLOCK_REQUESTS ) {
		if ( control->numRequests < 1 || control->numRequests > OFD_MAX_REQUESTS ) {
			return qfalse;
		}
		for ( i = 0; i < control->numRequests; i++ ) {
			const ofdRequest_t	*request = &control->requests[i];

			// an offset below the largest pak's size, the most a file can
			// have, and 0 for the content manifest
			if ( request->requestClass > OFD_CLASS_PREFETCH || request->offset >= OFCM_MAX_PAK
				|| ( request->offset && OFD_AllZero( request->sha256, 32 ) ) || !OFD_ChunkSize( request->largest ) ) {
				return qfalse;
			}
			// and neither its ID nor its file another's
			for ( j = 0; j < i; j++ ) {
				if ( request->requestId == control->requests[j].requestId
					|| !memcmp( request->sha256, control->requests[j].sha256, 32 ) ) {
					return qfalse;
				}
			}
		}
	} else if ( control->numRequests ) {
		return qfalse;
	}

	if ( control->blocks & OFD_BLOCK_ACKS ) {
		if ( !OFD_CheckAcks( control ) ) {
			return qfalse;
		}
	} else if ( control->numRanges || control->numTransfers ) {
		return qfalse;
	}

	if ( control->blocks & OFD_BLOCK_CANCELS ) {
		if ( control->numCancels < 1 || control->numCancels > OFD_MAX_CANCELS ) {
			return qfalse;
		}
		for ( i = 0; i < control->numCancels; i++ ) {
			if ( !control->cancels[i] ) {
				return qfalse;
			}
		}
	} else if ( control->numCancels ) {
		return qfalse;
	}
	return OFD_SerialsDiffer( control );
}

/*
================
OFD_ControlSize

The body's size the blocks take, once OFD_CheckControl has passed them
================
*/
static int OFD_ControlSize( const ofdControl_t *control ) {
	int		size = OFD_CONTROL_FIXED;

	if ( control->blocks & OFD_BLOCK_REQUESTS ) {
		size += 1 + control->numRequests * OFD_REQUEST_SIZE;
	}
	if ( control->blocks & OFD_BLOCK_ACKS ) {
		size += OFD_ACKS_FIXED + control->numRanges * OFD_RANGE_SIZE + 1
			+ control->numTransfers * OFD_ACK_TRANSFER_SIZE;
	}
	if ( control->blocks & OFD_BLOCK_CANCELS ) {
		size += 1 + control->numCancels * 2;
	}
	if ( control->blocks & OFD_BLOCK_NOTICE_ACK ) {
		size += 4;
	}
	return size;
}

/*
================
OFD_Check

Whether a message's flags and fields are in range: OFD_Read's last word,
and OFD_Write's first
================
*/
static qboolean OFD_Check( const ofdMessage_t *msg ) {
	// only DATA has a flag
	if ( msg->flags & ~( msg->type == OFD_TYPE_DATA ? OFD_FLAG_LAST : 0 ) ) {
		return qfalse;
	}
	switch ( msg->type ) {
	case OFD_TYPE_OPEN:
		return OFD_CheckOpen( &msg->u.open );
	case OFD_TYPE_CLOSE:
		return OFD_CheckClose( &msg->u.close );
	case OFD_TYPE_NOTICE:
		return msg->u.notice.nextBytes <= OFCM_MAX_ROLE_BYTES;
	case OFD_TYPE_DATA:
		return OFD_CheckData( &msg->u.data );
	case OFD_TYPE_CONTROL:
		return OFD_CheckControl( &msg->u.control );
	}
	return qfalse;
}

/*
================
OFD_ReadControl

Each block's count is checked against the bytes left before its records
are read; OFD_Check checks the rest.
================
*/
static qboolean OFD_ReadControl( const byte *body, int length, ofdControl_t *control ) {
	int		pos, i;

	if ( length < OFD_CONTROL_FIXED || Get16( body + 6 ) ) {
		return qfalse;
	}
	control->slot = body[0];
	control->blocks = body[1];
	control->receiveWindow = Get32( body + 2 );
	pos = OFD_CONTROL_FIXED;

	if ( control->blocks & OFD_BLOCK_REQUESTS ) {
		if ( pos + 1 > length ) {
			return qfalse;
		}
		control->numRequests = body[pos++];
		if ( control->numRequests > OFD_MAX_REQUESTS || pos + control->numRequests * OFD_REQUEST_SIZE > length ) {
			return qfalse;
		}
		for ( i = 0; i < control->numRequests; i++, pos += OFD_REQUEST_SIZE ) {
			ofdRequest_t	*request = &control->requests[i];

			request->requestId = Get32( body + pos );
			Com_Memcpy( request->sha256, body + pos + 4, 32 );
			request->offset = Get32( body + pos + 36 );
			request->requestClass = body[pos + 40];
			request->largest = Get16( body + pos + 41 );
			if ( body[pos + 43] ) {
				return qfalse;
			}
		}
	}

	if ( control->blocks & OFD_BLOCK_ACKS ) {
		if ( pos + OFD_ACKS_FIXED > length ) {
			return qfalse;
		}
		control->largest = Get32( body + pos );
		control->ackDelay = Get16( body + pos + 4 );
		control->firstRange = Get16( body + pos + 6 );
		control->numRanges = body[pos + 8];
		pos += OFD_ACKS_FIXED;
		if ( control->numRanges > OFD_MAX_ACK_RANGES
			|| pos + control->numRanges * OFD_RANGE_SIZE + 1 > length ) {
			return qfalse;
		}
		for ( i = 0; i < control->numRanges; i++, pos += OFD_RANGE_SIZE ) {
			control->ranges[i].gap = Get16( body + pos );
			control->ranges[i].length = Get16( body + pos + 2 );
		}
		control->numTransfers = body[pos++];
		if ( control->numTransfers > OFD_MAX_ACK_TRANSFERS
			|| pos + control->numTransfers * OFD_ACK_TRANSFER_SIZE > length ) {
			return qfalse;
		}
		for ( i = 0; i < control->numTransfers; i++, pos += OFD_ACK_TRANSFER_SIZE ) {
			control->transfers[i].serial = Get16( body + pos );
			control->transfers[i].cumulativeChunk = Get32( body + pos + 2 );
		}
	}

	if ( control->blocks & OFD_BLOCK_CANCELS ) {
		if ( pos + 1 > length ) {
			return qfalse;
		}
		control->numCancels = body[pos++];
		if ( control->numCancels > OFD_MAX_CANCELS || pos + control->numCancels * 2 > length ) {
			return qfalse;
		}
		for ( i = 0; i < control->numCancels; i++, pos += 2 ) {
			control->cancels[i] = Get16( body + pos );
		}
	}

	if ( control->blocks & OFD_BLOCK_NOTICE_ACK ) {
		if ( pos + 4 > length ) {
			return qfalse;
		}
		control->noticeId = Get32( body + pos );
		pos += 4;
	}

	return pos == length;
}

/*
================
OFD_ReadBody

The body's fields, from bytes whose length fits the type
================
*/
static qboolean OFD_ReadBody( const byte *body, int length, ofdMessage_t *msg ) {
	switch ( msg->type ) {
	case OFD_TYPE_OPEN: {
		ofdOpen_t	*open = &msg->u.open;

		if ( length != OFD_OPEN_BODY || Get16( body + 10 ) ) {
			return qfalse;
		}
		open->requestId = Get32( body );
		open->serial = Get16( body + 4 );
		open->status = body[6];
		open->kind = body[7];
		open->chunkSize = Get16( body + 8 );
		open->fileSize = Get32( body + 12 );
		open->startOffset = Get32( body + 16 );
		open->retryAfter = Get32( body + 20 );
		Com_Memcpy( open->sha256, body + 24, 32 );
		return qtrue;
	}

	case OFD_TYPE_CLOSE:
		if ( length != OFD_CLOSE_BODY || body[3] ) {
			return qfalse;
		}
		msg->u.close.serial = Get16( body );
		msg->u.close.reason = body[2];
		msg->u.close.requestId = Get32( body + 4 );
		return qtrue;

	case OFD_TYPE_NOTICE:
		if ( length != OFD_NOTICE_BODY || !OFD_AllZero( body + 44, 4 ) ) {
			return qfalse;
		}
		msg->u.notice.noticeId = Get32( body );
		Com_Memcpy( msg->u.notice.sha256, body + 4, 32 );
		msg->u.notice.nextBytes = Get64( body + 36 );
		return qtrue;

	case OFD_TYPE_DATA:
		if ( length < OFD_DATA_HEADER ) {
			return qfalse;
		}
		msg->u.data.packetNumber = Get32( body );
		msg->u.data.chunkIndex = Get32( body + 4 );
		msg->u.data.serial = Get16( body + 8 );
		msg->u.data.length = Get16( body + 10 );
		msg->u.data.payload = body + OFD_DATA_HEADER;
		return length == OFD_DATA_HEADER + msg->u.data.length;

	case OFD_TYPE_CONTROL:
		return OFD_ReadControl( body, length, &msg->u.control );
	}
	return qfalse;
}

/*
================
OFD_Read
================
*/
qboolean OFD_Read( const byte *data, int length, ofdMessage_t *msg ) {
	int		bodyLength;

	if ( length > OFD_MAX_DATAGRAM || !OFD_IsContent( data, length ) || data[7] != OFD_VERSION ) {
		return qfalse;
	}
	bodyLength = Get16( data + 10 );
	if ( OFD_HEADER_SIZE + bodyLength != length ) {
		return qfalse;
	}

	Com_Memset( msg, 0, sizeof( *msg ) );
	msg->type = data[8];
	msg->flags = data[9];
	msg->key = OFD_ReadKey( data );
	return OFD_ReadBody( data + OFD_HEADER_SIZE, bodyLength, msg ) && OFD_Check( msg );
}

/*
================
OFD_WriteControl
================
*/
static void OFD_WriteControl( const ofdControl_t *control, byte *body ) {
	int		pos, i;

	body[0] = control->slot;
	body[1] = control->blocks;
	Put32( body + 2, control->receiveWindow );
	Put16( body + 6, 0 );
	pos = OFD_CONTROL_FIXED;

	if ( control->blocks & OFD_BLOCK_REQUESTS ) {
		body[pos++] = control->numRequests;
		for ( i = 0; i < control->numRequests; i++, pos += OFD_REQUEST_SIZE ) {
			const ofdRequest_t	*request = &control->requests[i];

			Put32( body + pos, request->requestId );
			Com_Memcpy( body + pos + 4, request->sha256, 32 );
			Put32( body + pos + 36, request->offset );
			body[pos + 40] = request->requestClass;
			Put16( body + pos + 41, request->largest );
			body[pos + 43] = 0;
		}
	}

	if ( control->blocks & OFD_BLOCK_ACKS ) {
		Put32( body + pos, control->largest );
		Put16( body + pos + 4, control->ackDelay );
		Put16( body + pos + 6, control->firstRange );
		body[pos + 8] = control->numRanges;
		pos += OFD_ACKS_FIXED;
		for ( i = 0; i < control->numRanges; i++, pos += OFD_RANGE_SIZE ) {
			Put16( body + pos, control->ranges[i].gap );
			Put16( body + pos + 2, control->ranges[i].length );
		}
		body[pos++] = control->numTransfers;
		for ( i = 0; i < control->numTransfers; i++, pos += OFD_ACK_TRANSFER_SIZE ) {
			Put16( body + pos, control->transfers[i].serial );
			Put32( body + pos + 2, control->transfers[i].cumulativeChunk );
		}
	}

	if ( control->blocks & OFD_BLOCK_CANCELS ) {
		body[pos++] = control->numCancels;
		for ( i = 0; i < control->numCancels; i++, pos += 2 ) {
			Put16( body + pos, control->cancels[i] );
		}
	}

	if ( control->blocks & OFD_BLOCK_NOTICE_ACK ) {
		Put32( body + pos, control->noticeId );
	}
}

/*
================
OFD_Write
================
*/
int OFD_Write( const ofdMessage_t *msg, byte *out, int size ) {
	byte	*body = out + OFD_HEADER_SIZE;
	int		bodyLength;

	if ( !OFD_Check( msg ) ) {
		return 0;
	}
	switch ( msg->type ) {
	case OFD_TYPE_OPEN:
		bodyLength = OFD_OPEN_BODY;
		break;
	case OFD_TYPE_CLOSE:
		bodyLength = OFD_CLOSE_BODY;
		break;
	case OFD_TYPE_NOTICE:
		bodyLength = OFD_NOTICE_BODY;
		break;
	case OFD_TYPE_DATA:
		if ( !msg->u.data.payload ) {
			return 0;
		}
		bodyLength = OFD_DATA_HEADER + msg->u.data.length;
		break;
	default:
		bodyLength = OFD_ControlSize( &msg->u.control );
		break;
	}
	if ( OFD_HEADER_SIZE + bodyLength > size ) {
		return 0;
	}

	Com_Memcpy( out, ofdMagic, sizeof( ofdMagic ) );
	out[7] = OFD_VERSION;
	out[8] = msg->type;
	out[9] = msg->flags;
	Put16( out + 10, bodyLength );
	Put64( out + 12, msg->key );

	switch ( msg->type ) {
	case OFD_TYPE_OPEN: {
		const ofdOpen_t	*open = &msg->u.open;

		Put32( body, open->requestId );
		Put16( body + 4, open->serial );
		body[6] = open->status;
		body[7] = open->kind;
		Put16( body + 8, open->chunkSize );
		Put16( body + 10, 0 );
		Put32( body + 12, open->fileSize );
		Put32( body + 16, open->startOffset );
		Put32( body + 20, open->retryAfter );
		Com_Memcpy( body + 24, open->sha256, 32 );
		break;
	}
	case OFD_TYPE_CLOSE:
		Put16( body, msg->u.close.serial );
		body[2] = msg->u.close.reason;
		body[3] = 0;
		Put32( body + 4, msg->u.close.requestId );
		break;
	case OFD_TYPE_NOTICE:
		Put32( body, msg->u.notice.noticeId );
		Com_Memcpy( body + 4, msg->u.notice.sha256, 32 );
		Put64( body + 36, msg->u.notice.nextBytes );
		Put32( body + 44, 0 );
		break;
	case OFD_TYPE_DATA:
		Put32( body, msg->u.data.packetNumber );
		Put32( body + 4, msg->u.data.chunkIndex );
		Put16( body + 8, msg->u.data.serial );
		Put16( body + 10, msg->u.data.length );
		memmove( body + OFD_DATA_HEADER, msg->u.data.payload, msg->u.data.length );
		break;
	case OFD_TYPE_CONTROL:
		OFD_WriteControl( &msg->u.control, body );
		break;
	}
	return OFD_HEADER_SIZE + bodyLength;
}

/*
================
OFD_AckRange
================
*/
void OFD_AckRange( const ofdControl_t *control, int range, uint32_t *low, uint32_t *high ) {
	int		i;

	*high = control->largest;
	*low = control->largest - ( control->firstRange - 1 );
	for ( i = 0; i < range && i < control->numRanges; i++ ) {
		*high = *low - control->ranges[i].gap - 1;
		*low = *high - ( control->ranges[i].length - 1 );
	}
}

/*
=============================================================================

THE CONTENT MANIFEST

=============================================================================
*/

// an entry's fields
#define ENTRY_HASH		0
#define ENTRY_SIZE		32
#define ENTRY_CHECKSUM	40
#define ENTRY_ROLE		44
#define ENTRY_FLAGS		45
#define ENTRY_RESERVED	46
#define ENTRY_GAMEDIR	48
#define ENTRY_NAME		80

#define SOURCE_URL		8

#define ENTRY( data, i )	( (data) + OFCM_HEADER_SIZE + (i) * OFCM_ENTRY_SIZE )

static int LowerByte( int c ) {
	return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

// bytes after lowercasing, as entries are sorted
static int CompareLower( const char *a, const char *b ) {
	while ( *a && LowerByte( (byte)*a ) == LowerByte( (byte)*b ) ) {
		a++;
		b++;
	}
	return LowerByte( (byte)*a ) - LowerByte( (byte)*b );
}

static int ComparePair( const char *gameDirA, const char *nameA, const char *gameDirB, const char *nameB ) {
	int		order = CompareLower( gameDirA, gameDirB );

	return order ? order : CompareLower( nameA, nameB );
}

// a checked entry's names, NUL terminated in their fields
#define GAMEDIR( entry )	( (const char *)(entry) + ENTRY_GAMEDIR )
#define NAME( entry )		( (const char *)(entry) + ENTRY_NAME )

/*
================
PaddedLength

The length of a NUL padded string in its field, or -1 if it fills the
field or something follows its NUL
================
*/
static int PaddedLength( const byte *field, int size ) {
	const byte	*nul = memchr( field, 0, size );

	if ( !nul || !OFD_AllZero( nul, size - ( nul - field ) ) ) {
		return -1;
	}
	return nul - field;
}

// whether the name starts with the word, in any case
static qboolean StemIs( const char *name, const char *word ) {
	int		i;

	for ( i = 0; word[i]; i++ ) {
		if ( LowerByte( (byte)name[i] ) != word[i] ) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
================
OFCM_DeviceName

Whether Windows takes a name for a device: con, prn, aux, nul, com0 to
com9 or lpt0 to lpt9, in any case, alone or before an extension. The name
stops at its first dot, and Windows drops spaces before it, so "con .x" is
con too.
================
*/
static qboolean OFCM_DeviceName( const char *name ) {
	const char	*dot = strchr( name, '.' );
	int			length = dot ? (int)( dot - name ) : (int)strlen( name );

	while ( length > 0 && name[length - 1] == ' ' ) {
		length--;
	}
	if ( length == 3 ) {
		return StemIs( name, "con" ) || StemIs( name, "prn" ) || StemIs( name, "aux" ) || StemIs( name, "nul" );
	}
	return length == 4 && name[3] >= '0' && name[3] <= '9' && ( StemIs( name, "com" ) || StemIs( name, "lpt" ) );
}

/*
================
OFCM_ValidName
================
*/
qboolean OFCM_ValidName( const char *gameDir, const char *name ) {
	const char	*parts[2] = { gameDir, name };
	int			i;
	const char	*c;

	for ( i = 0; i < 2; i++ ) {
		if ( !parts[i][0] || parts[i][0] == '.' ) {
			return qfalse;
		}
		for ( c = parts[i]; *c; c++ ) {
			// printable ASCII only, so no code page makes a byte something
			// else; no '$', as in the CONIN$, CONOUT$ and CLOCK$ devices; and
			// no '~' before a digit, as in a Windows short name like
			// ZZ-MAP~1, which may be another file's
			if ( (byte)*c < 0x20 || (byte)*c >= 0x7f || strchr( "/\\:*?\"<>|$", *c )
				|| ( *c == '~' && c[1] >= '0' && c[1] <= '9' ) ) {
				return qfalse;
			}
		}
		// Windows drops a folder's final dots and spaces, so "settings."
		// would be settings and "baseq3 " baseq3 (FS_NameLength); names keep
		// the same rule, so both parts have one. And some names are devices
		c--;
		if ( *c == '.' || *c == ' ' || OFCM_DeviceName( parts[i] ) ) {
			return qfalse;
		}
	}
	return CompareLower( gameDir, "settings" ) && CompareLower( gameDir, "cache" );
}

/*
================
OFCM_Find

Whether the pair is among the count entries from first, which are sorted
================
*/
static qboolean OFCM_Find( const byte *data, int first, int count, const char *gameDir, const char *name ) {
	int		low = first, high = first + count - 1;

	while ( low <= high ) {
		int			mid = low + ( high - low ) / 2;
		const byte	*entry = ENTRY( data, mid );
		int			order = ComparePair( gameDir, name, GAMEDIR( entry ), NAME( entry ) );

		if ( !order ) {
			return qtrue;
		}
		if ( order < 0 ) {
			high = mid - 1;
		} else {
			low = mid + 1;
		}
	}
	return qfalse;
}

/*
================
OFCM_CheckEntry

An entry's fields, and its names by OFCM_ValidName and the caller's rule
================
*/
static const char *OFCM_CheckEntry( const byte *entry, int role, ofcmValidName_t validName ) {
	uint64_t	size = Get64( entry + ENTRY_SIZE );
	byte		flags = entry[ENTRY_FLAGS];
	qboolean	zeroHash = OFD_AllZero( entry + ENTRY_HASH, 32 );

	if ( size < OFCM_MIN_PAK || size > OFCM_MAX_PAK ) {
		return "a pak size out of range";
	}
	if ( entry[ENTRY_ROLE] != role ) {
		return "an entry outside its role's section";
	}
	if ( flags & ~( OFCM_FLAG_OWN | OFCM_FLAG_CODE ) || entry[ENTRY_RESERVED] || entry[ENTRY_RESERVED + 1] ) {
		return "an entry with unknown flags or reserved bytes set";
	}
	if ( flags & OFCM_FLAG_OWN ) {
		if ( !zeroHash || role != OFCM_ROLE_REQUIRED ) {
			return "a \"bring your own\" entry that isn't required or has a hash";
		}
	} else if ( zeroHash ) {
		return "an entry without a hash";
	}
	if ( PaddedLength( entry + ENTRY_GAMEDIR, OFCM_GAMEDIR_SIZE ) < 0
		|| PaddedLength( entry + ENTRY_NAME, OFCM_NAME_SIZE ) < 0 ) {
		return "a game directory or name not NUL padded in its field";
	}
	if ( !OFCM_ValidName( GAMEDIR( entry ), NAME( entry ) ) ) {
		return "a game directory or name no download may have";
	}
	if ( validName && !validName( GAMEDIR( entry ), NAME( entry ) ) ) {
		return "a name the download name rule refuses";
	}
	return NULL;
}

/*
================
OFCM_CheckSource
================
*/
static const char *OFCM_CheckSource( const byte *source ) {
	const char	*url = (const char *)source + SOURCE_URL;
	int			length, i;

	if ( source[0] < OFCM_SOURCE_HASH || source[0] > OFCM_SOURCE_BOTH || !OFD_AllZero( source + 1, SOURCE_URL - 1 ) ) {
		return "a source of an unknown kind";
	}
	length = PaddedLength( source + SOURCE_URL, OFCM_URL_SIZE );
	if ( length < 0 ) {
		return "a source URL not NUL terminated and padded";
	}
	if ( !( ( length > 7 && !strncmp( url, "http://", 7 ) ) || ( length > 8 && !strncmp( url, "https://", 8 ) ) ) ) {
		return "a source URL not http:// or https://";
	}
	for ( i = 0; i < length; i++ ) {
		if ( (byte)url[i] <= ' ' || (byte)url[i] >= 0x7f ) {
			return "a source URL with a space, control or non-ASCII character";
		}
	}
	return NULL;
}

/*
================
OFCM_Read
================
*/
const char *OFCM_Read( const byte *data, int length, ofcmHeader_t *header, ofcmValidName_t validName ) {
	uint64_t	sums[3] = { 0, 0, 0 };
	int			counts[3], starts[3], numEntries, numSources;
	int			role, i, other;
	const char	*error;

	// the header, and every count against the bytes there are, before an
	// entry is read
	if ( length < OFCM_HEADER_SIZE ) {
		return "shorter than its header";
	}
	if ( length > OFCM_MAX_SIZE ) {
		return "larger than 528,448 bytes";
	}
	if ( memcmp( data, "OFCM", 4 ) ) {
		return "not a content manifest";
	}
	if ( Get16( data + 4 ) != 1 || Get16( data + 6 ) != OFCM_HEADER_SIZE
		|| Get16( data + 14 ) != OFCM_ENTRY_SIZE || Get16( data + 24 ) != OFCM_SOURCE_SIZE ) {
		return "a version or record size other than version 1's";
	}
	if ( Get16( data + 26 ) || Get32( data + 52 ) || !OFD_AllZero( data + 56, 8 ) ) {
		return "flags or reserved bytes set";
	}
	numEntries = Get16( data + 12 );
	counts[0] = Get16( data + 16 );
	counts[1] = Get16( data + 18 );
	counts[2] = Get16( data + 20 );
	numSources = Get16( data + 22 );
	if ( numEntries > OFCM_MAX_ENTRIES ) {
		return "more than 4,096 entries";
	}
	if ( numSources > OFCM_MAX_SOURCES ) {
		return "more than 16 sources";
	}
	if ( counts[0] + counts[1] + counts[2] != numEntries ) {
		return "role counts that don't add up to the entries";
	}
	if ( Get32( data + 8 ) != (uint32_t)length
		|| length != OFCM_HEADER_SIZE + numEntries * OFCM_ENTRY_SIZE + numSources * OFCM_SOURCE_SIZE ) {
		return "a total size unlike its counts' or the bytes received";
	}
	starts[0] = 0;
	starts[1] = counts[0];
	starts[2] = counts[0] + counts[1];

	// each entry in its role's section, its fields in range, after the one
	// before it, and in no section before its own
	for ( role = 0; role < 3; role++ ) {
		for ( i = starts[role]; i < starts[role] + counts[role]; i++ ) {
			const byte	*entry = ENTRY( data, i );

			if ( ( error = OFCM_CheckEntry( entry, OFCM_ROLE_REQUIRED + role, validName ) ) != NULL ) {
				return error;
			}
			if ( i > starts[role] && ComparePair( GAMEDIR( entry - OFCM_ENTRY_SIZE ), NAME( entry - OFCM_ENTRY_SIZE ),
				GAMEDIR( entry ), NAME( entry ) ) >= 0 ) {
				return "entries out of order, or a game directory and name twice";
			}
			for ( other = 0; other < role; other++ ) {
				if ( OFCM_Find( data, starts[other], counts[other], GAMEDIR( entry ), NAME( entry ) ) ) {
					return "entries out of order, or a game directory and name twice";
				}
			}
			if ( !( entry[ENTRY_FLAGS] & OFCM_FLAG_OWN ) ) {
				sums[role] += Get64( entry + ENTRY_SIZE );
			}
		}
	}

	if ( Get64( data + 28 ) != sums[0] || Get64( data + 36 ) != sums[1] || Get64( data + 44 ) != sums[2] ) {
		return "a role's bytes unlike the sum of its sizes";
	}

	for ( i = 0; i < numSources; i++ ) {
		if ( ( error = OFCM_CheckSource( ENTRY( data, numEntries ) + i * OFCM_SOURCE_SIZE ) ) != NULL ) {
			return error;
		}
	}

	header->numEntries = numEntries;
	header->numRequired = counts[0];
	header->numNext = counts[1];
	header->numOptional = counts[2];
	header->numSources = numSources;
	header->requiredBytes = sums[0];
	header->nextBytes = sums[1];
	header->optionalBytes = sums[2];
	return NULL;
}

/*
================
OFCM_ReadEntry
================
*/
void OFCM_ReadEntry( const byte *data, int index, ofcmEntry_t *entry ) {
	const byte	*p = ENTRY( data, index );

	Com_Memcpy( entry->sha256, p + ENTRY_HASH, 32 );
	entry->size = (uint32_t)Get64( p + ENTRY_SIZE );
	entry->checksum = (int)Get32( p + ENTRY_CHECKSUM );
	entry->role = p[ENTRY_ROLE];
	entry->flags = p[ENTRY_FLAGS];
	Com_Memcpy( entry->gameDir, p + ENTRY_GAMEDIR, OFCM_GAMEDIR_SIZE );
	Com_Memcpy( entry->name, p + ENTRY_NAME, OFCM_NAME_SIZE );
}

/*
================
OFCM_ReadSource
================
*/
void OFCM_ReadSource( const byte *data, const ofcmHeader_t *header, int index, ofcmSource_t *source ) {
	const byte	*p = ENTRY( data, header->numEntries ) + index * OFCM_SOURCE_SIZE;

	source->kind = p[0];
	Com_Memcpy( source->url, p + SOURCE_URL, OFCM_URL_SIZE );
	source->url[OFCM_URL_SIZE] = '\0';
}

// the order entries go on the wire: by role, then game directory and name
static int OFCM_CompareEntries( const void *a, const void *b ) {
	const ofcmEntry_t	*entryA = a, *entryB = b;

	if ( entryA->role != entryB->role ) {
		return entryA->role - entryB->role;
	}
	return ComparePair( entryA->gameDir, entryA->name, entryB->gameDir, entryB->name );
}

/*
================
OFCM_Write

The bytes go through OFCM_Read before they're given out, so nothing is
written that a client would refuse.
================
*/
int OFCM_Write( ofcmEntry_t *entries, int numEntries, const ofcmSource_t *sources, int numSources,
	ofcmValidName_t validName, byte *out, int size, const char **error ) {
	uint64_t		sums[3] = { 0, 0, 0 };
	int				counts[3] = { 0, 0, 0 };
	int				total, i;
	ofcmHeader_t	header;

	if ( numEntries < 0 || numEntries > OFCM_MAX_ENTRIES ) {
		*error = "more than 4,096 entries";
		return 0;
	}
	if ( numSources < 0 || numSources > OFCM_MAX_SOURCES ) {
		*error = "more than 16 sources";
		return 0;
	}
	// what sorting needs: a role to index the counts, and terminated names
	for ( i = 0; i < numEntries; i++ ) {
		if ( entries[i].role < OFCM_ROLE_REQUIRED || entries[i].role > OFCM_ROLE_OPTIONAL ) {
			*error = "an entry outside its role's section";
			return 0;
		}
		if ( !memchr( entries[i].gameDir, 0, OFCM_GAMEDIR_SIZE ) || !memchr( entries[i].name, 0, OFCM_NAME_SIZE ) ) {
			*error = "a game directory or name not NUL padded in its field";
			return 0;
		}
	}
	total = OFCM_HEADER_SIZE + numEntries * OFCM_ENTRY_SIZE + numSources * OFCM_SOURCE_SIZE;
	if ( total > size ) {
		*error = "a buffer too small for it";
		return 0;
	}

	if ( numEntries ) {
		qsort( entries, numEntries, sizeof( *entries ), OFCM_CompareEntries );
	}
	Com_Memset( out, 0, total );
	for ( i = 0; i < numEntries; i++ ) {
		byte	*p = ENTRY( out, i );

		counts[entries[i].role - 1]++;
		if ( !( entries[i].flags & OFCM_FLAG_OWN ) ) {
			sums[entries[i].role - 1] += entries[i].size;
		}
		Com_Memcpy( p + ENTRY_HASH, entries[i].sha256, 32 );
		Put64( p + ENTRY_SIZE, entries[i].size );
		Put32( p + ENTRY_CHECKSUM, (uint32_t)entries[i].checksum );
		p[ENTRY_ROLE] = entries[i].role;
		p[ENTRY_FLAGS] = entries[i].flags;
		// strncpy pads with NULs, and both end within their fields
		strncpy( (char *)p + ENTRY_GAMEDIR, entries[i].gameDir, OFCM_GAMEDIR_SIZE );
		strncpy( (char *)p + ENTRY_NAME, entries[i].name, OFCM_NAME_SIZE );
	}
	for ( i = 0; i < numSources; i++ ) {
		byte	*p = ENTRY( out, numEntries ) + i * OFCM_SOURCE_SIZE;

		// a URL that fills its field is left unterminated, for OFCM_Read to refuse
		p[0] = sources[i].kind;
		strncpy( (char *)p + SOURCE_URL, sources[i].url, OFCM_URL_SIZE );
	}

	Com_Memcpy( out, "OFCM", 4 );
	Put16( out + 4, 1 );
	Put16( out + 6, OFCM_HEADER_SIZE );
	Put32( out + 8, total );
	Put16( out + 12, numEntries );
	Put16( out + 14, OFCM_ENTRY_SIZE );
	Put16( out + 16, counts[0] );
	Put16( out + 18, counts[1] );
	Put16( out + 20, counts[2] );
	Put16( out + 22, numSources );
	Put16( out + 24, OFCM_SOURCE_SIZE );
	Put64( out + 28, sums[0] );
	Put64( out + 36, sums[1] );
	Put64( out + 44, sums[2] );

	if ( ( *error = OFCM_Read( out, total, &header, validName ) ) != NULL ) {
		return 0;
	}
	return total;
}

/*
================
OFD_ReadInfoContent
================
*/
qboolean OFD_ReadInfoContent( const char *ofm, const char *ofr, byte sha256[32], uint64_t *requiredBytes ) {
	byte		hash[32];
	uint64_t	bytes = 0;
	int			i, digit;

	if ( !ofm || !ofr || strlen( ofm ) != 64 ) {
		return qfalse;
	}
	for ( i = 0; i < 64; i++ ) {
		if ( ofm[i] >= '0' && ofm[i] <= '9' ) {
			digit = ofm[i] - '0';
		} else if ( ofm[i] >= 'a' && ofm[i] <= 'f' ) {
			digit = ofm[i] - 'a' + 10;
		} else {
			return qfalse;
		}
		hash[i / 2] = i & 1 ? hash[i / 2] | digit : digit << 4;
	}

	// 2^53 has 16 digits, and only "0" starts with a 0
	if ( !ofr[0] || strlen( ofr ) > 16 || ( ofr[0] == '0' && ofr[1] ) ) {
		return qfalse;
	}
	for ( i = 0; ofr[i]; i++ ) {
		if ( ofr[i] < '0' || ofr[i] > '9' ) {
			return qfalse;
		}
		bytes = bytes * 10 + ( ofr[i] - '0' );
	}
	if ( bytes >= (uint64_t)1 << 53 ) {
		return qfalse;
	}

	Com_Memcpy( sha256, hash, 32 );
	*requiredBytes = bytes;
	return qtrue;
}
