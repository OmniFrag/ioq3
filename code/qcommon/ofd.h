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
// ofd.h -- the content protocol's wire formats: its out-of-band datagrams,
// the content manifest, and the content keys of an infoResponse.
//
// Every function is pure: no globals, no allocation, nothing from the rest
// of the engine, so the server, the client, the tests and the fuzz
// harnesses all call the same code. Each reader checks every length,
// count and field before it fills anything its caller acts on, and refuses
// the whole input when one is out of range. Each writer refuses what the
// reader would, so whatever one side writes, the other reads, and a
// datagram read and written again comes out the same bytes.
//
// Integers are little-endian on the wire.

#ifndef OFD_H
#define OFD_H

// the common header of every content datagram
#define OFD_HEADER_SIZE			20
#define OFD_VERSION				1

#define OFD_TYPE_OPEN			0x01	// server to client
#define OFD_TYPE_CLOSE			0x02	// server to client
#define OFD_TYPE_NOTICE			0x03	// server to client; content manifest changed (step 6)
#define OFD_TYPE_DATA			0x10	// server to client
#define OFD_TYPE_CONTROL		0x30	// client to server

#define OFD_FLAG_LAST			1		// DATA: the transfer's last chunk

#define OFD_OPEN_BODY			56
#define OFD_CLOSE_BODY			8
#define OFD_NOTICE_BODY			48
#define OFD_DATA_HEADER			12		// DATA's body before its payload

// chunks follow from the largest datagram a request states
#define OFD_MIN_LARGEST			1000
#define OFD_MAX_LARGEST			1232
#define OFD_MIN_CHUNK			960
#define OFD_MAX_CHUNK			1152

// the largest datagram either side sends, a DATA with the largest chunk
#define OFD_MAX_DATAGRAM		( OFD_HEADER_SIZE + OFD_DATA_HEADER + OFD_MAX_CHUNK )

// OPEN's status
#define OFD_STATUS_OK			0
#define OFD_STATUS_NOT_FOUND	1
#define OFD_STATUS_RETAIL		2
#define OFD_STATUS_BUSY			3
#define OFD_STATUS_DISABLED		4
#define OFD_STATUS_LIMIT		5
#define OFD_STATUS_BAD_REQUEST	6
#define OFD_STATUS_READ_ERROR	7

// OPEN's kind
#define OFD_KIND_PAK			0
#define OFD_KIND_MANIFEST		1

// CLOSE's reason
#define OFD_CLOSE_CANCELED		1
#define OFD_CLOSE_GONE			2
#define OFD_CLOSE_PREEMPTED		3
#define OFD_CLOSE_STALLED		4
#define OFD_CLOSE_READ_ERROR	5
#define OFD_CLOSE_COMPLETE		6

// CONTROL's blocks, each present when its flag is set, in this order
#define OFD_BLOCK_REQUESTS		1
#define OFD_BLOCK_ACKS			2
#define OFD_BLOCK_CANCELS		4
#define OFD_BLOCK_NOTICE_ACK	8

#define OFD_MAX_REQUESTS		2
#define OFD_MAX_ACK_RANGES		15		// below the first
#define OFD_MAX_ACK_TRANSFERS	2
#define OFD_MAX_CANCELS			2
#define OFD_MAX_ACK_DELAY		2500	// 10 µs units: 25 ms

// a request's class
#define OFD_CLASS_LOADING		0
#define OFD_CLASS_PREFETCH		1

// CONTROL's sizes: its fixed part, a request, the acks before the ranges,
// a range, a transfer's cumulative chunk
#define OFD_CONTROL_FIXED		8
#define OFD_REQUEST_SIZE		44
#define OFD_ACKS_FIXED			9
#define OFD_RANGE_SIZE			4
#define OFD_ACK_TRANSFER_SIZE	6
#define OFD_MAX_CONTROL_BODY	( OFD_CONTROL_FIXED + 1 + OFD_MAX_REQUESTS * OFD_REQUEST_SIZE \
	+ OFD_ACKS_FIXED + OFD_MAX_ACK_RANGES * OFD_RANGE_SIZE + 1 \
	+ OFD_MAX_ACK_TRANSFERS * OFD_ACK_TRANSFER_SIZE + 1 + OFD_MAX_CANCELS * 2 + 4 )

typedef struct {
	uint32_t	requestId;
	uint16_t	serial;			// not 0 when status is OK
	byte		status;
	byte		kind;
	uint16_t	chunkSize;		// OFD_ChunkSize of the request's largest datagram
	uint32_t	fileSize;
	uint32_t	startOffset;
	uint32_t	retryAfter;		// ms, for busy and limit
	byte		sha256[32];		// not all zero when status is OK
} ofdOpen_t;

typedef struct {
	uint16_t	serial;
	byte		reason;
	uint32_t	requestId;
} ofdClose_t;

typedef struct {
	uint32_t	noticeId;
	byte		sha256[32];
	uint64_t	nextBytes;		// at most OFCM_MAX_ROLE_BYTES
} ofdNotice_t;

typedef struct {
	uint32_t	packetNumber;
	uint32_t	chunkIndex;
	uint16_t	serial;
	uint16_t	length;			// 1 to OFD_MAX_CHUNK
	const byte	*payload;		// in the datagram read, or the bytes to write
} ofdData_t;

typedef struct {
	uint32_t	requestId;
	byte		sha256[32];		// all zero: the current content manifest
	uint32_t	offset;			// below OFCM_MAX_PAK; 0 for the content manifest
	byte		requestClass;
	uint16_t	largest;		// OFD_MIN_LARGEST to OFD_MAX_LARGEST
} ofdRequest_t;

typedef struct {
	uint16_t	gap;			// packets missing below the range before, at least 1
	uint16_t	length;			// packets received below the gap, at least 1
} ofdAckRange_t;

typedef struct {
	uint16_t	serial;
	uint32_t	cumulativeChunk;	// every chunk below it is received and written
} ofdAckTransfer_t;

// A CONTROL's requests have different IDs and hashes, and no serial
// appears twice among its acked transfers and its cancels.
typedef struct {
	byte				slot;		// the client's number on the server
	byte				blocks;		// OFD_BLOCK_*
	uint32_t			receiveWindow;

	int					numRequests;
	ofdRequest_t		requests[OFD_MAX_REQUESTS];

	uint32_t			largest;	// the highest packet number received
	uint16_t			ackDelay;	// 10 µs units
	uint16_t			firstRange;	// packets received from the largest down, at least 1
	int					numRanges;
	ofdAckRange_t		ranges[OFD_MAX_ACK_RANGES];
	int					numTransfers;
	ofdAckTransfer_t	transfers[OFD_MAX_ACK_TRANSFERS];

	int					numCancels;
	uint16_t			cancels[OFD_MAX_CANCELS];

	uint32_t			noticeId;
} ofdControl_t;

typedef struct {
	byte		type;		// OFD_TYPE_*
	byte		flags;
	uint64_t	key;		// the connection's ofk; the caller compares it
	union {
		ofdOpen_t		open;
		ofdClose_t		close;
		ofdNotice_t		notice;
		ofdData_t		data;
		ofdControl_t	control;
	} u;
} ofdMessage_t;

// a connectionless packet whose first bytes are a content datagram's, so
// worth reading with OFD_Read rather than as a command
qboolean	OFD_IsContent( const byte *data, int length );

// a whole datagram, from its 0xFFFFFFFF on; qfalse if any length, count or
// field is out of range, and then msg is left unspecified
qboolean	OFD_Read( const byte *data, int length, ofdMessage_t *msg );

// the datagram's bytes, or 0 if msg has a field OFD_Read would refuse or
// the buffer is too small; OFD_MAX_DATAGRAM bytes always do
int			OFD_Write( const ofdMessage_t *msg, byte *out, int size );

// the chunk size a request's largest datagram implies, or 0 when out of range
int			OFD_ChunkSize( int largest );

// The packet numbers an ack covers, from the largest down: range 0 is the
// first range, then one for each of control->ranges. Every range lies at or
// above packet 0, which OFD_Read checks.
void		OFD_AckRange( const ofdControl_t *control, int range, uint32_t *low, uint32_t *high );

// The content manifest. Its header's fields, and an entry's or source's,
// as OFCM_Read has checked them; strings are NUL terminated.
#define OFCM_HEADER_SIZE		64
#define OFCM_ENTRY_SIZE			128
#define OFCM_SOURCE_SIZE		256
#define OFCM_MAX_ENTRIES		4096
#define OFCM_MAX_SOURCES		16
#define OFCM_MAX_SIZE			( OFCM_HEADER_SIZE + OFCM_MAX_ENTRIES * OFCM_ENTRY_SIZE \
	+ OFCM_MAX_SOURCES * OFCM_SOURCE_SIZE )
#define OFCM_MIN_PAK			22			// a zip's end record
#define OFCM_MAX_PAK			0x7fffffff	// the engine's file sizes are int
#define OFCM_MAX_ROLE_BYTES		( (uint64_t)OFCM_MAX_ENTRIES * OFCM_MAX_PAK )	// a role's sizes, added up
#define OFCM_GAMEDIR_SIZE		32
#define OFCM_NAME_SIZE			48
#define OFCM_URL_SIZE			( OFCM_SOURCE_SIZE - 8 )

#define OFCM_ROLE_REQUIRED		1
#define OFCM_ROLE_NEXT			2
#define OFCM_ROLE_OPTIONAL		3

#define OFCM_FLAG_OWN			1		// "bring your own": a retail pak the server refuses
#define OFCM_FLAG_CODE			2		// holds vm/cgame.qvm or vm/ui.qvm

#define OFCM_SOURCE_HASH		1		// <url>/sha256/<hex>.pk3
#define OFCM_SOURCE_NAME		2		// <url>/<gamedir>/<name>.pk3
#define OFCM_SOURCE_BOTH		3

typedef struct {
	int			numEntries;
	int			numRequired, numNext, numOptional;
	int			numSources;
	uint64_t	requiredBytes;		// required and not "bring your own"
	uint64_t	nextBytes;
	uint64_t	optionalBytes;
} ofcmHeader_t;

typedef struct {
	byte		sha256[32];			// all zero for "bring your own"
	uint32_t	size;
	int			checksum;			// pack_t.checksum
	byte		role;
	byte		flags;
	char		gameDir[OFCM_GAMEDIR_SIZE];
	char		name[OFCM_NAME_SIZE];	// without .pk3
} ofcmEntry_t;

typedef struct {
	byte		kind;
	char		url[OFCM_URL_SIZE + 1];
} ofcmSource_t;

// What every download name keeps to, of "<gameDir>/<name>.pk3"'s parts:
// neither empty, starting with a dot, nor ending in a dot or space, which
// Windows drops; printable ASCII only, none of /\:*?"<>|$, and no '~'
// before a digit, as in a Windows short name; not a name Windows takes
// for a device (con, prn, aux, nul, com0 to com9, lpt0 to lpt9, in any
// case, alone or before an extension, with any spaces or dots before the
// extension); and a game directory other than settings and cache, which
// hold the player's own files. OFCM_Read applies it to every entry, and
// FS_ValidDownloadName (step 2) is meant to build on it.
qboolean	OFCM_ValidName( const char *gameDir, const char *name );

// the caller's own rule besides, such as FS_ValidDownloadName's game
// directory checks; NULL for none
typedef qboolean ( *ofcmValidName_t )( const char *gameDir, const char *name );

// what OPEN may give as the content manifest's file size: checked before
// anything is allocated for it
qboolean	OFCM_SizeAllowed( uint32_t size );

// NULL if the whole content manifest keeps every rule, then header is
// filled; otherwise the rule it breaks, and header is unspecified
const char	*OFCM_Read( const byte *data, int length, ofcmHeader_t *header, ofcmValidName_t validName );

// the entry or source at index, of a content manifest OFCM_Read accepted
void		OFCM_ReadEntry( const byte *data, int index, ofcmEntry_t *entry );
void		OFCM_ReadSource( const byte *data, const ofcmHeader_t *header, int index, ofcmSource_t *source );

// Writes a content manifest of the entries and sources, sorting the entries
// in place into their order on the wire, and returns its size; 0 when it
// breaks a rule (then *error names it) or doesn't fit
int			OFCM_Write( ofcmEntry_t *entries, int numEntries, const ofcmSource_t *sources, int numSources,
				ofcmValidName_t validName, byte *out, int size, const char **error );

// An infoResponse's ofm and ofr: the content manifest's SHA-256, 64
// lowercase hex digits, and the required bytes, a decimal under 2^53
// without leading zeros. qfalse, and nothing filled, unless both hold.
qboolean	OFD_ReadInfoContent( const char *ofm, const char *ofr, byte sha256[32], uint64_t *requiredBytes );

#endif
