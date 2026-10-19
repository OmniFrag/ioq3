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
// cl_content.c -- the client's side of the fast download
// (docs/design/download-protocol.md): a server that agreed on the content
// protocol (exttokens.h) sends the paks a client lacks as raw out-of-band
// datagrams (ofd.h), named by their SHA-256 in its content manifest.
//
// After the gamestate, the client asks for the content manifest, or uses
// its own copy of it, works out which of the paks the server references it
// lacks, and asks for each distinct hash, two at a time. Each arrives into
// <sha256>.part in the content cache, hashed as its start grows, and is
// stored by its hash once whole and checked. Acks of the packet numbers go
// from the packet handler and the wait between frames, not on the frame.
// What the fast path can't get goes to the classic download, and both end
// in CL_DownloadsComplete.
//
// The packet handler only records what arrives and acks it; the frame
// (CL_ContentFrame) makes the decisions, which can raise an error or load
// the game.

#include "client.h"
#include "../qcommon/content.h"
#include "../qcommon/exttokens.h"

#define CLC_SLOTS			2				// transfers open or asked for at once (section 6.1)
#define CLC_MAX_PAKS		64				// missing paks a gamestate lists, at most
#define CLC_MAX_DONE		64				// hashes a connection is done with
#define CLC_LARGEST			OFD_MAX_LARGEST	// native UDP's datagrams
#define CLC_CHUNK			OFD_MAX_CHUNK	// what CLC_LARGEST implies (OFD_ChunkSize)
#define CLC_RECEIVED		4096			// packet numbers remembered for the acks
#define CLC_ARRIVALS		32				// arrival times kept, for the ack spacing

#define CLC_MSEC			1000000LL		// in Sys_Nanoseconds
#define CLC_REPEAT			( 50 * CLC_MSEC )	// a request without its OPEN, a cancel
#define CLC_OPEN_TIMEOUT	( 10000 * CLC_MSEC )	// a request without its OPEN, at most
#define CLC_STALL			( 15000 * CLC_MSEC )	// an open transfer without data, at most
#define CLC_ACK_DELAY		( 4 * CLC_MSEC )	// the first arrival not acked, at most
#define CLC_ACK_SPACING		( 1 * CLC_MSEC )	// between acks, but for a gap's
#define CLC_CANCEL_TIME		( 1000 * CLC_MSEC )	// a cancel, until its CLOSE
#define CLC_FINISHED_TIME	( 1000 * CLC_MSEC )	// a finished transfer's ack, until its CLOSE
#define CLC_RETRY_MIN		1000			// ms, a busy or limit wait
#define CLC_RETRY_MAX		30000
#define CLC_LIMIT_RETRIES	5
#define CLC_FLUSH_BYTES		( 4 << 20 )		// the valid start recorded this often
#define CLC_HASH_SLICE		( 2 * CLC_MSEC )	// reading back to hash, a frame
#define CLC_RECEIVE_UNKNOWN	( 64 << 10 )	// the receive window, where the socket's isn't known

typedef enum {
	CLC_IDLE,
	CLC_MANIFEST,		// asking for the content manifest, or receiving it
	CLC_FETCHING
} clcPhase_t;

typedef enum {
	FETCH_QUEUED,
	FETCH_ACTIVE,		// in a slot
	FETCH_WAITING,		// busy or limit, until retryAt
	FETCH_DONE,
	FETCH_CLASSIC,		// for the classic path
	FETCH_FAILED		// noted as failed, for CL_DownloadsComplete
} clcFetchState_t;

// one distinct hash to fetch, for every missing pak that lists it
typedef struct {
	byte			sha256[SHA256_DIGEST_SIZE];
	uint32_t		size;
	unsigned int	checksum;
	char			gameDir[OFCM_GAMEDIR_SIZE];		// the first entry's, which the cache keeps
	char			name[OFCM_NAME_SIZE];
	clcFetchState_t	state;
	int64_t			retryAt;
	int				mismatches;		// of its hash
	int				limits;			// "limit" answers
	int64_t			valid;			// bytes of it at hand, for progress
} clcFetch_t;

typedef enum {
	PAK_FAST,
	PAK_CLASSIC,
	PAK_DONE
} clcPakState_t;

// a pak the server references and the client lacks
typedef struct {
	char			name[MAX_QPATH];	// the server's: <game directory>/<name>.pk3
	unsigned int	checksum;
	int				fetch;				// in fetches, or -1
	clcPakState_t	state;
} clcPak_t;

// what ended a slot early, for the frame
#define ANSWER_STATUS	0x100		// an OPEN with this status, not a transfer
#define ANSWER_CLOSE	0x200		// a CLOSE with this reason, before every chunk

// A transfer: a request until its OPEN, then open until every chunk is in.
// A local one is a .part already whole, hashed without asking
typedef struct {
	qboolean		used;
	int				fetch;			// -1 for the content manifest
	uint32_t		requestId;
	uint32_t		offset;			// asked from
	int64_t			notBefore;		// a request waits until then
	int64_t			firstSent;		// 0 before the request first goes
	int64_t			lastSent;
	int				answer;			// ANSWER_*, or 0
	int				retryMsec;		// with a busy or limit answer

	qboolean		open;
	qboolean		local;
	qboolean		cacheChecked;	// the content manifest's: whether the client has it
	uint16_t		serial;
	uint32_t		fileSize;
	uint32_t		numChunks;		// from offset
	uint32_t		chunks;			// received
	uint32_t		cumulative;		// the first chunk not received
	byte			*have;			// a bit a chunk
	int64_t			hashed;			// bytes hashed, from the file's start
	sha256_t		sha;
	FILE			*file;			// the .part
	int64_t			filePos;		// where its stream is, -1 if unknown
	char			path[MAX_OSPATH];
	byte			*memory;		// the content manifest's bytes
	byte			sha256[SHA256_DIGEST_SIZE];	// the content manifest's, from its OPEN
	int64_t			flushed;		// the valid start last recorded
	int64_t			lastData;
} clcSlot_t;

// a serial that stays in the CONTROLs a while: a finished transfer's last
// cumulative chunk, or a cancel
typedef struct {
	uint16_t		serial;
	uint32_t		chunks;
	int64_t			until;
} clcSerial_t;

enum {
	DROP_INVALID,		// the codec refused it
	DROP_ADDRESS,		// not from the server
	DROP_KEY,			// not this connection's key, or no fast download agreed
	DROP_STATE,			// nothing asked for, or a kind this client doesn't take
	DROP_UNASKED,		// an OPEN for no request of ours, or not as asked
	DROP_STALE,			// data or a CLOSE for no open transfer, or an OPEN repeated late
	DROP_RANGE,			// a chunk out of its transfer's range
	DROP_COUNT
};

static const char *dropNames[DROP_COUNT] = {
	"invalid", "address", "key", "state", "unasked", "stale", "range"
};

static struct {
	clcPhase_t		phase;
	qboolean		classicOnly;	// the fast path gave up for this connection
	qboolean		finishing;		// the set is done: the frame hands it on
	uint32_t		nextRequestId;

	clcPak_t		paks[CLC_MAX_PAKS];
	int				numPaks;
	clcFetch_t		fetches[CLC_MAX_PAKS];
	int				numFetches;
	clcSlot_t		slots[CLC_SLOTS];
	clcSerial_t		finished[CLC_SLOTS];
	clcSerial_t		cancels[OFD_MAX_CANCELS * 2];

	byte			*manifest;
	ofcmHeader_t	header;

	// the hashes this connection is done with, stored or given up on, so
	// none is asked for again on the next gamestate
	byte			done[CLC_MAX_DONE][SHA256_DIGEST_SIZE];
	int				numDone;

	// the acks: the packet numbers received, by their place in a ring
	qboolean		anyPacket;
	uint32_t		largest;
	int64_t			largestAt;
	byte			received[CLC_RECEIVED / 8];
	int				unacked;
	int64_t			firstUnacked;
	int64_t			lastAck;
	int64_t			lastControl;
	qboolean		gap;
	int64_t			arrivals[CLC_ARRIVALS];
	int				nextArrival;

	uint32_t		receiveWindow;	// what the CONTROLs report, read at the set's start

	int				datagrams;
	int				drops[DROP_COUNT];
	int64_t			lastShown;
	int64_t			lastProgress;
	uint32_t		shownRequest;	// the transfer whose file the ui's cvars show
} clContent;

static cvar_t	*cl_showContent;

static qboolean CL_ContentHas( const byte *bits, uint32_t i ) {
	return ( bits[i >> 3] >> ( i & 7 ) ) & 1;
}

static void CL_ContentSetBit( byte *bits, uint32_t i, qboolean on ) {
	if ( on ) {
		bits[i >> 3] |= 1 << ( i & 7 );
	} else {
		bits[i >> 3] &= ~( 1 << ( i & 7 ) );
	}
}

/*
==============================================================

PROGRESS (section 9.4)

==============================================================
*/

/*
=================
CL_ContentSetProgress

The ui's view of the set, its files and bytes, and of the oldest open
transfer's file in the cvars the ui has always read
=================
*/
static void CL_ContentSetProgress( const char *state ) {
	const clcSlot_t		*s, *current = NULL;
	const clcFetch_t	*f;
	int64_t				total = 0, count = 0;
	int					i, done = 0;

	for ( i = 0; i < clContent.numFetches; i++ ) {
		f = &clContent.fetches[i];
		total += f->size;
		count += f->state == FETCH_DONE ? f->size : f->valid;
		done += f->state == FETCH_DONE;
	}
	Cvar_Set( "cl_downloadState", state );
	Cvar_Set( "cl_downloadSource", "server" );
	Cvar_Set( "cl_downloadFiles", va( "%d/%d", done, clContent.numFetches ) );
	Cvar_Set( "cl_downloadTotal", va( "%lld", (long long)total ) );
	Cvar_Set( "cl_downloadTotalCount", va( "%lld", (long long)count ) );

	for ( i = 0; i < CLC_SLOTS; i++ ) {
		s = &clContent.slots[i];
		if ( s->used && s->fetch >= 0 && s->open && ( !current || s->requestId < current->requestId ) ) {
			current = s;
		}
	}
	if ( current ) {
		f = &clContent.fetches[current->fetch];
		// the time it's counted from, as its own transfer's start
		if ( current->requestId != clContent.shownRequest ) {
			clContent.shownRequest = current->requestId;
			Cvar_Set( "cl_downloadTime", va( "%d", cls.realtime ) );
		}
		Cvar_Set( "cl_downloadName", va( "%s/%s.pk3", f->gameDir, f->name ) );
		Cvar_Set( "cl_downloadSize", va( "%u", f->size ) );
		Cvar_Set( "cl_downloadCount", va( "%lld", (long long)f->valid ) );
	}
}

/*
=================
CL_ContentClearProgress
=================
*/
void CL_ContentClearProgress( void ) {
	Cvar_Set( "cl_downloadState", "" );
	Cvar_Set( "cl_downloadSource", "" );
	Cvar_Set( "cl_downloadFiles", "" );
	Cvar_Set( "cl_downloadTotal", "0" );
	Cvar_Set( "cl_downloadTotalCount", "0" );
}

/*
==============================================================

TRANSFERS

==============================================================
*/

static qboolean CL_ContentIsDone( const byte sha256[SHA256_DIGEST_SIZE] ) {
	int		i;

	for ( i = 0; i < clContent.numDone; i++ ) {
		if ( !memcmp( clContent.done[i], sha256, SHA256_DIGEST_SIZE ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static void CL_ContentSetDone( const byte sha256[SHA256_DIGEST_SIZE] ) {
	if ( !CL_ContentIsDone( sha256 ) && clContent.numDone < CLC_MAX_DONE ) {
		Com_Memcpy( clContent.done[clContent.numDone++], sha256, SHA256_DIGEST_SIZE );
	}
}

/*
=================
CL_ContentKeepSerial

Keeps a serial in the CONTROLs for a while, in place of the oldest
=================
*/
static void CL_ContentKeepSerial( clcSerial_t *list, int count, uint16_t serial, uint32_t chunks, int64_t time ) {
	clcSerial_t	*oldest = &list[0];
	int			i;

	for ( i = 0; i < count; i++ ) {
		if ( list[i].serial == serial || !list[i].serial ) {
			oldest = &list[i];
			break;
		}
		if ( list[i].until < oldest->until ) {
			oldest = &list[i];
		}
	}
	oldest->serial = serial;
	oldest->chunks = chunks;
	oldest->until = Sys_Nanoseconds() + time;
}

// a serial its CLOSE ended, from a list; whether it was there
static qboolean CL_ContentDropSerial( clcSerial_t *list, int count, uint16_t serial ) {
	int		i;

	for ( i = 0; i < count; i++ ) {
		if ( list[i].serial == serial ) {
			list[i].serial = 0;
			return qtrue;
		}
	}
	return qfalse;
}

// a transfer with every chunk, still acked whole until its CLOSE
static void CL_ContentKeepFinished( const clcSlot_t *s ) {
	if ( s->serial ) {
		CL_ContentKeepSerial( clContent.finished, ARRAY_LEN( clContent.finished ), s->serial, s->numChunks,
			CLC_FINISHED_TIME );
	}
}

// a request waiting for its OPEN
static qboolean CL_ContentAsking( const clcSlot_t *s ) {
	return s->used && !s->open && !s->answer;
}

/*
=================
CL_ContentWhole

The bytes at the start of a slot's file that are written; all of a local
one's, whose offset is its size
=================
*/
static int64_t CL_ContentWhole( const clcSlot_t *s ) {
	if ( !s->open ) {
		return s->offset;
	}
	return MIN( (int64_t)s->offset + (int64_t)s->cumulative * CLC_CHUNK, (int64_t)s->fileSize );
}

/*
=================
CL_ContentRecordValid

The .part's valid start, once flushed, into the cache index, so a later
download resumes from it
=================
*/
static void CL_ContentRecordValid( clcSlot_t *s ) {
	int64_t	valid = CL_ContentWhole( s );

	if ( s->file && s->fetch >= 0 && valid != s->flushed && !fflush( s->file ) ) {
		Content_Cache_UpdatePartial( clContent.fetches[s->fetch].sha256, valid );
		s->flushed = valid;
	}
}

/*
=================
CL_ContentFreeSlot

Ends a slot: an open transfer canceled with cancel, and its .part's valid
start recorded and the file closed, kept for a resume, or deleted with
remove
=================
*/
static void CL_ContentFreeSlot( clcSlot_t *s, qboolean cancel, qboolean remove ) {
	if ( !s->used ) {
		return;
	}
	if ( cancel && s->open && s->serial ) {
		CL_ContentKeepSerial( clContent.cancels, ARRAY_LEN( clContent.cancels ), s->serial, 0, CLC_CANCEL_TIME );
	}
	if ( s->file ) {
		if ( !remove ) {
			CL_ContentRecordValid( s );
		}
		fclose( s->file );
		if ( remove ) {
			Content_Cache_DropPartial( clContent.fetches[s->fetch].sha256 );
		} else {
			Content_Cache_ReleasePartial( clContent.fetches[s->fetch].sha256 );
		}
	}
	free( s->have );
	free( s->memory );
	Com_Memset( s, 0, sizeof( *s ) );
}

/*
=================
CL_ContentGiveUp

The fast path stops for this connection, and what's left goes the classic
way: no content manifest, or one that breaks a rule, no OPEN in time, a
stall, or the server refusing
=================
*/
static void CL_ContentGiveUp( const char *why ) {
	int		i;

	Com_Printf( S_COLOR_YELLOW "WARNING: the fast download stopped (%s), so the server's paks come the classic way\n",
		why );
	clContent.classicOnly = qtrue;
	for ( i = 0; i < CLC_SLOTS; i++ ) {
		CL_ContentFreeSlot( &clContent.slots[i], qtrue, qfalse );
	}
	for ( i = 0; i < clContent.numPaks; i++ ) {
		if ( clContent.paks[i].state == PAK_FAST ) {
			clContent.paks[i].state = PAK_CLASSIC;
		}
	}
	clContent.finishing = qtrue;
}

/*
=================
CL_ContentFetchEnded

A fetch is over, why being why it went classic or failed: each pak listing
it follows it, and the connection doesn't ask for its hash again
=================
*/
static void CL_ContentFetchEnded( int fetch, clcFetchState_t state, const char *why ) {
	clcFetch_t	*f = &clContent.fetches[fetch];
	clcPak_t	*p;
	int			i;

	f->state = state;
	CL_ContentSetDone( f->sha256 );
	if ( state == FETCH_CLASSIC ) {
		Com_Printf( "%s/%s.pk3 comes the classic way: %s\n", f->gameDir, f->name, why );
	}
	for ( i = 0; i < clContent.numPaks; i++ ) {
		p = &clContent.paks[i];
		if ( p->fetch != fetch || p->state != PAK_FAST ) {
			continue;
		}
		if ( state == FETCH_CLASSIC ) {
			p->state = PAK_CLASSIC;
		} else {
			p->state = PAK_DONE;
			if ( state == FETCH_FAILED ) {
				CL_AddDownloadFailure( p->name, p->checksum, why );
			}
		}
	}
}

/*
=================
CL_ContentAsk

A slot asks for a fetch, or for the content manifest (fetch -1), from
where its .part stopped; a .part already whole is hashed without asking
=================
*/
static void CL_ContentAsk( clcSlot_t *s, int fetch ) {
	clcFetch_t	*f;
	int64_t		valid;

	Com_Memset( s, 0, sizeof( *s ) );
	s->used = qtrue;
	s->fetch = fetch;
	s->requestId = ++clContent.nextRequestId;
	if ( fetch < 0 ) {
		return;
	}

	f = &clContent.fetches[fetch];
	f->state = FETCH_ACTIVE;
	valid = Content_Cache_BeginPartial( f->sha256, f->size, f->checksum, f->gameDir, f->name,
		s->path, sizeof( s->path ) );
	if ( valid > 0 && !( s->file = Sys_FOpen( s->path, "r+b" ) ) ) {
		valid = 0;
	}
	if ( valid == 0 ) {
		s->file = Sys_FOpen( s->path, "w+b" );
	}
	if ( !s->file ) {
		if ( valid >= 0 ) {
			Content_Cache_ReleasePartial( f->sha256 );		// evictable again
		}
		Com_Memset( s, 0, sizeof( *s ) );
		CL_ContentFetchEnded( fetch, FETCH_FAILED, "it couldn't be written into the content cache" );
		return;
	}
	Com_SHA256Init( &s->sha );
	s->filePos = -1;
	s->offset = valid;
	s->flushed = valid;
	s->fileSize = f->size;
	f->valid = valid;
	if ( valid > 0 ) {
		Com_Printf( "Resuming %s/%s.pk3 from byte %lld of %u\n", f->gameDir, f->name, (long long)valid, f->size );
	}
	// every byte at hand: finished here, once hashed
	if ( valid == f->size ) {
		s->local = qtrue;
		s->open = qtrue;
	}
}

/*
=================
CL_ContentSeek

The .part's stream at offset at, seeking only when it isn't there: the C
library drops its buffer on every seek
=================
*/
static qboolean CL_ContentSeek( clcSlot_t *s, int64_t at ) {
	if ( s->filePos != at && fseek( s->file, (long)at, SEEK_SET ) ) {
		s->filePos = -1;
		return qfalse;
	}
	s->filePos = at;
	return qtrue;
}

/*
=================
CL_ContentHashAhead

Hashes the file from where its hash stopped while its start is written,
reading it back from the .part until the deadline (Sys_Nanoseconds)
=================
*/
static void CL_ContentHashAhead( clcSlot_t *s, int64_t deadline ) {
	static byte	buffer[64 * 1024];
	int64_t		whole = CL_ContentWhole( s );
	size_t		n;

	if ( !s->file || s->hashed >= whole ) {
		return;
	}
	// C wants a seek between a write and a read on the stream, either way
	// round, even where it stands already
	s->filePos = -1;
	while ( s->hashed < whole && Sys_Nanoseconds() < deadline ) {
		n = (size_t)MIN( (int64_t)sizeof( buffer ), whole - s->hashed );
		if ( !CL_ContentSeek( s, s->hashed ) || fread( buffer, 1, n, s->file ) != n ) {
			break;
		}
		s->filePos += n;
		Com_SHA256Update( &s->sha, buffer, n );
		s->hashed += n;
	}
	s->filePos = -1;
}

/*
=================
CL_ContentData

A chunk of an open transfer: written, and hashed when it's next in line.
qfalse when it couldn't be written, so its packet isn't acked: the server
sends again only what it hasn't seen acked (section 6.2)
=================
*/
static qboolean CL_ContentData( clcSlot_t *s, const ofdData_t *data, byte flags, int64_t now ) {
	int64_t		at = (int64_t)s->offset + (int64_t)data->chunkIndex * CLC_CHUNK;
	qboolean	last = data->chunkIndex == s->numChunks - 1;

	if ( data->chunkIndex >= s->numChunks || data->length != ( last ? s->fileSize - at : CLC_CHUNK ) ||
		!( flags & OFD_FLAG_LAST ) != !last ) {
		clContent.drops[DROP_RANGE]++;
		return qtrue;
	}
	s->lastData = now;
	if ( CL_ContentHas( s->have, data->chunkIndex ) ) {
		return qtrue;		// a copy
	}
	if ( s->memory ) {
		Com_Memcpy( s->memory + at, data->payload, data->length );
	} else if ( !CL_ContentSeek( s, at ) || fwrite( data->payload, 1, data->length, s->file ) != data->length ) {
		s->filePos = -1;
		return qfalse;		// not written, so not had: it comes again
	} else {
		s->filePos += data->length;
	}
	CL_ContentSetBit( s->have, data->chunkIndex, qtrue );
	s->chunks++;
	if ( s->fetch >= 0 ) {
		clContent.fetches[s->fetch].valid += data->length;
	}

	// next in line for the hash: from the payload, not read back
	if ( s->file && at == s->hashed && data->chunkIndex == s->cumulative ) {
		Com_SHA256Update( &s->sha, data->payload, data->length );
		s->hashed += data->length;
	}
	while ( s->cumulative < s->numChunks && CL_ContentHas( s->have, s->cumulative ) ) {
		s->cumulative++;
	}
	return qtrue;
}

/*
==============================================================

THE CONTROL DATAGRAM (section 5.4)

==============================================================
*/

static qboolean CL_ContentReceived( int64_t pn ) {
	return CL_ContentHas( clContent.received, (uint32_t)( pn % CLC_RECEIVED ) );
}

// the packets received from n down, n left at the first that wasn't: a
// byte of the ring at a time where all eight were, as without loss
static int CL_ContentReceivedRun( int64_t *n, int64_t oldest ) {
	int		run = 0;

	while ( *n >= oldest && CL_ContentReceived( *n ) ) {
		if ( ( *n & 7 ) == 7 && *n - 7 >= oldest && clContent.received[( *n % CLC_RECEIVED ) >> 3] == 0xff ) {
			*n -= 8;
			run += 8;
		} else {
			( *n )--;
			run++;
		}
	}
	return run;
}

/*
=================
CL_ContentWriteAcks

The packet numbers received, in ranges from the largest down, within those
the ring remembers and at or above packet 0; then each transfer's
cumulative chunk, the open ones' first
=================
*/
static void CL_ContentWriteAcks( ofdControl_t *c, int64_t now ) {
	int64_t	n = clContent.largest, oldest = MAX( 0, n - ( CLC_RECEIVED - 1 ) );
	int		i, j;

	c->blocks |= OFD_BLOCK_ACKS;
	c->largest = clContent.largest;
	// clamped: a longer delay would be out of range, and drop the whole CONTROL
	c->ackDelay = (uint16_t)MIN( ( now - clContent.largestAt ) / 10000, OFD_MAX_ACK_DELAY );

	c->firstRange = CL_ContentReceivedRun( &n, oldest );
	for ( c->numRanges = 0; n >= oldest && c->numRanges < OFD_MAX_ACK_RANGES; c->numRanges++ ) {
		ofdAckRange_t	*r = &c->ranges[c->numRanges];

		for ( r->gap = 0; n >= oldest && !CL_ContentReceived( n ); n-- ) {
			r->gap++;
		}
		if ( n < oldest ) {
			break;		// a gap down past what's remembered: no range
		}
		r->length = CL_ContentReceivedRun( &n, oldest );
	}

	c->numTransfers = 0;
	for ( i = 0; i < CLC_SLOTS && c->numTransfers < OFD_MAX_ACK_TRANSFERS; i++ ) {
		const clcSlot_t	*s = &clContent.slots[i];

		if ( s->used && s->open && s->serial ) {
			c->transfers[c->numTransfers].serial = s->serial;
			c->transfers[c->numTransfers].cumulativeChunk = s->cumulative;
			c->numTransfers++;
		}
	}
	// a serial the server gave an open transfer since is acked as that one
	for ( i = 0; i < (int)ARRAY_LEN( clContent.finished ) && c->numTransfers < OFD_MAX_ACK_TRANSFERS; i++ ) {
		const clcSerial_t	*f = &clContent.finished[i];

		for ( j = 0; j < c->numTransfers && c->transfers[j].serial != f->serial; j++ ) {
		}
		if ( f->serial && f->until > now && j == c->numTransfers ) {
			c->transfers[c->numTransfers].serial = f->serial;
			c->transfers[c->numTransfers].cumulativeChunk = f->chunks;
			c->numTransfers++;
		}
	}
}

/*
=================
CL_ContentReceiveWindow

The bytes the socket can hold (section 7.6), as NET_ReceiveBufferSize gives
the buffer set, without Linux's bookkeeping. Never under the two datagrams
the server's window keeps; where it isn't known, 64 KB, the smallest
default buffer of the systems the client runs on
=================
*/
static uint32_t CL_ContentReceiveWindow( void ) {
	int		size = NET_ReceiveBufferSize( clc.netchan.remoteAddress.type );

	if ( size <= 0 ) {
		return CLC_RECEIVE_UNKNOWN;
	}
	return MAX( size, 2 * OFD_MAX_DATAGRAM );
}

/*
=================
CL_ContentSend

One CONTROL with all there is to say: the requests without their OPEN, the
acks, and the cancels
=================
*/
static void CL_ContentSend( int64_t now ) {
	ofdMessage_t	msg;
	ofdControl_t	*c = &msg.u.control;
	byte			out[OFD_HEADER_SIZE + OFD_MAX_CONTROL_BODY];
	int				i, j, length;

	Com_Memset( &msg, 0, sizeof( msg ) );
	msg.type = OFD_TYPE_CONTROL;
	msg.key = clc.contentKey;
	c->slot = clc.clientNum;
	c->receiveWindow = clContent.receiveWindow;

	for ( i = 0; i < CLC_SLOTS; i++ ) {
		clcSlot_t		*s = &clContent.slots[i];
		ofdRequest_t	*r;

		if ( !CL_ContentAsking( s ) || now < s->notBefore ) {
			continue;
		}
		r = &c->requests[c->numRequests++];
		r->requestId = s->requestId;
		if ( s->fetch >= 0 ) {
			Com_Memcpy( r->sha256, clContent.fetches[s->fetch].sha256, SHA256_DIGEST_SIZE );
		}
		r->offset = s->offset;
		r->requestClass = OFD_CLASS_LOADING;
		r->largest = CLC_LARGEST;
		s->lastSent = now;
		if ( !s->firstSent ) {
			s->firstSent = now;
		}
	}
	if ( c->numRequests ) {
		c->blocks |= OFD_BLOCK_REQUESTS;
	}

	if ( clContent.anyPacket ) {
		CL_ContentWriteAcks( c, now );
		clContent.unacked = 0;
		clContent.gap = qfalse;
		clContent.lastAck = now;
	}

	// a transfer canceled is no longer acked; no serial goes twice
	for ( i = 0; i < (int)ARRAY_LEN( clContent.cancels ) && c->numCancels < OFD_MAX_CANCELS; i++ ) {
		clcSerial_t	*x = &clContent.cancels[i];

		if ( x->serial && x->until <= now ) {
			x->serial = 0;
		}
		if ( !x->serial ) {
			continue;
		}
		for ( j = 0; j < c->numTransfers && c->transfers[j].serial != x->serial; j++ ) {
		}
		if ( j == c->numTransfers ) {
			c->cancels[c->numCancels++] = x->serial;
		}
	}
	if ( c->numCancels ) {
		c->blocks |= OFD_BLOCK_CANCELS;
	}

	clContent.lastControl = now;
	length = OFD_Write( &msg, out, sizeof( out ) );
	if ( length > 0 ) {
		NET_SendPacket( NS_CLIENT, length, out, clc.netchan.remoteAddress );
	} else {
		Com_DPrintf( "Couldn't write a content CONTROL\n" );
	}
}

/*
=================
CL_ContentAckDue

When the acks are due (section 6.3): at once after a gap; every N packets,
half those that arrived in the last 4 ms, 2 to 16, once a millisecond has
passed since the last ack, which an arrival rechecks, so no timer finer
than a millisecond spins the wait; and 4 ms after the first arrival not
acked
=================
*/
static int64_t CL_ContentAckDue( int64_t now ) {
	int		i, recent = 0;

	if ( !clContent.unacked ) {
		return INT64_MAX;
	}
	if ( clContent.gap ) {
		return 0;		// at once, whenever asked
	}
	if ( clContent.unacked >= 2 && now - clContent.lastAck >= CLC_ACK_SPACING ) {
		for ( i = 0; i < CLC_ARRIVALS; i++ ) {
			recent += clContent.arrivals[i] && now - clContent.arrivals[i] <= CLC_ACK_DELAY;
		}
		if ( clContent.unacked >= Com_Clamp( 2, 16, recent / 2 ) ) {
			return 0;
		}
	}
	return clContent.firstUnacked + CLC_ACK_DELAY;
}

/*
=================
CL_ContentDue

When the next CONTROL is due, in Sys_Nanoseconds; INT64_MAX for never
=================
*/
static int64_t CL_ContentDue( int64_t now ) {
	int64_t	due;
	int		i;

	// nothing goes without a connection that agreed on the protocol
	if ( clc.state < CA_CONNECTED || !( clc.ofx & OFX_CONTENT ) || clc.demoplaying ) {
		return INT64_MAX;
	}
	due = CL_ContentAckDue( now );
	for ( i = 0; i < CLC_SLOTS; i++ ) {
		const clcSlot_t	*s = &clContent.slots[i];

		if ( CL_ContentAsking( s ) ) {
			due = MIN( due, MAX( s->notBefore, s->lastSent ? s->lastSent + CLC_REPEAT : 0 ) );
		}
	}
	for ( i = 0; i < (int)ARRAY_LEN( clContent.cancels ); i++ ) {
		if ( clContent.cancels[i].serial ) {
			due = MIN( due, clContent.lastControl + CLC_REPEAT );
		}
	}
	return due;
}

/*
=================
CL_ContentService
=================
*/
int64_t CL_ContentService( void ) {
	int64_t	now = Sys_Nanoseconds(), due = CL_ContentDue( now );

	if ( due <= now ) {
		CL_ContentSend( now );
		due = CL_ContentDue( now );
	}
	return due;
}

/*
==============================================================

THE SERVER'S DATAGRAMS

==============================================================
*/

/*
=================
CL_ContentFindSlot

The slot whose open transfer has this serial, or with no serial, the one
that asked with this request ID
=================
*/
static clcSlot_t *CL_ContentFindSlot( uint16_t serial, uint32_t requestId ) {
	int		i;

	for ( i = 0; i < CLC_SLOTS; i++ ) {
		clcSlot_t	*s = &clContent.slots[i];

		if ( s->used && ( serial ? s->open && s->serial == serial : s->requestId == requestId ) ) {
			return s;
		}
	}
	return NULL;
}

/*
=================
CL_ContentOpen

The server's answer to a request. One that answers no request of ours, or
not as it asked, is dropped
=================
*/
static void CL_ContentOpen( const ofdOpen_t *open ) {
	clcSlot_t	*s = CL_ContentFindSlot( 0, open->requestId );
	clcFetch_t	*f;
	int			chunk = open->chunkSize;

	if ( !s ) {
		// a repeat of an answer to a request done with, or no request of ours
		clContent.drops[open->requestId && open->requestId <= clContent.nextRequestId ? DROP_STALE : DROP_UNASKED]++;
		return;
	}
	if ( s->open || s->answer ) {
		return;		// a repeat
	}
	f = s->fetch >= 0 ? &clContent.fetches[s->fetch] : NULL;

	if ( open->status != OFD_STATUS_OK ) {
		// its hash echoes the request's, all zero for the content manifest
		if ( f ? memcmp( open->sha256, f->sha256, SHA256_DIGEST_SIZE ) :
			!OFD_AllZero( open->sha256, SHA256_DIGEST_SIZE ) ) {
			clContent.drops[DROP_UNASKED]++;
			return;
		}
		s->answer = ANSWER_STATUS | open->status;
		s->retryMsec = Com_Clamp( CLC_RETRY_MIN, CLC_RETRY_MAX, open->retryAfter );
		return;
	}

	// what the request implies, and the content manifest says
	// and a serial no other open transfer has (never 0, which the codec checks)
	if ( open->kind != ( f ? OFD_KIND_PAK : OFD_KIND_MANIFEST ) || chunk != CLC_CHUNK ||
		open->startOffset != s->offset ||
		( f && ( open->fileSize != f->size || memcmp( open->sha256, f->sha256, SHA256_DIGEST_SIZE ) ) ) ||
		CL_ContentFindSlot( open->serial, 0 ) ) {
		clContent.drops[DROP_UNASKED]++;
		return;
	}

	s->numChunks = ( open->fileSize - open->startOffset + chunk - 1 ) / chunk;
	s->have = calloc( ( s->numChunks + 7 ) / 8, 1 );
	if ( !f ) {
		s->memory = malloc( open->fileSize );
		Com_Memcpy( s->sha256, open->sha256, SHA256_DIGEST_SIZE );
	}
	if ( !s->have || ( !f && !s->memory ) ) {
		CL_ContentGiveUp( "out of memory" );
		return;
	}
	s->open = qtrue;
	s->serial = open->serial;
	s->fileSize = open->fileSize;
	s->lastData = Sys_Nanoseconds();
	if ( f ) {
		// the server holds this client for its download, until donedl
		// (section 6.5)
		clc.downloadHeld = qtrue;
	}
}

/*
=================
CL_ContentClose

The server ended a transfer
=================
*/
static void CL_ContentClose( const ofdClose_t *close ) {
	clcSlot_t	*s;

	if ( CL_ContentDropSerial( clContent.cancels, ARRAY_LEN( clContent.cancels ), close->serial ) ||
		CL_ContentDropSerial( clContent.finished, ARRAY_LEN( clContent.finished ), close->serial ) ) {
		return;
	}
	if ( !( s = CL_ContentFindSlot( close->serial, 0 ) ) ) {
		clContent.drops[DROP_STALE]++;
		return;
	}
	// either way it's acked no more; before every chunk came, the frame
	// decides what next
	if ( s->chunks < s->numChunks ) {
		s->answer = ANSWER_CLOSE | close->reason;
	}
	s->serial = 0;
}

/*
=================
CL_ContentPacketNumber

A DATA's packet number, for the acks, whichever transfer it's for, so the
server sends none of them again; but not one whose chunk couldn't be written
=================
*/
static void CL_ContentPacketNumber( uint32_t pn, int64_t now ) {
	uint32_t	n;

	if ( !clContent.anyPacket || pn > clContent.largest ) {
		if ( !clContent.anyPacket || pn - clContent.largest >= CLC_RECEIVED ) {
			Com_Memset( clContent.received, 0, sizeof( clContent.received ) );
		} else {
			for ( n = clContent.largest + 1; n != pn; n++ ) {
				CL_ContentSetBit( clContent.received, n % CLC_RECEIVED, qfalse );
			}
		}
		clContent.gap |= clContent.anyPacket && pn != clContent.largest + 1;
		clContent.anyPacket = qtrue;
		clContent.largest = pn;
		clContent.largestAt = now;
	} else if ( clContent.largest - pn >= CLC_RECEIVED ) {
		return;		// older than the acks reach
	}
	CL_ContentSetBit( clContent.received, pn % CLC_RECEIVED, qtrue );
	if ( !clContent.unacked++ ) {
		clContent.firstUnacked = now;
	}
	clContent.arrivals[clContent.nextArrival++ % CLC_ARRIVALS] = now;
}

/*
=================
CL_ContentPacket

A content datagram: from the server, with the connection's key, on a
connection that agreed on the content protocol, or dropped and counted.
The acks go from here
=================
*/
void CL_ContentPacket( netadr_t from, msg_t *msg ) {
	ofdMessage_t	m;
	clcSlot_t		*s;
	int64_t			now;

	clContent.datagrams++;
	if ( clc.state < CA_CONNECTED || clc.demoplaying || !NET_CompareAdr( from, clc.netchan.remoteAddress ) ) {
		clContent.drops[DROP_ADDRESS]++;
		return;
	}
	if ( !OFD_Read( msg->data, msg->cursize, &m ) ) {
		clContent.drops[DROP_INVALID]++;
		return;
	}
	if ( !( clc.ofx & OFX_CONTENT ) || m.key != clc.contentKey ) {
		clContent.drops[DROP_KEY]++;
		return;
	}

	switch ( m.type ) {
	case OFD_TYPE_OPEN:
		if ( clContent.phase == CLC_IDLE ) {
			clContent.drops[DROP_STATE]++;
		} else {
			CL_ContentOpen( &m.u.open );
		}
		break;
	case OFD_TYPE_CLOSE:
		CL_ContentClose( &m.u.close );
		break;
	case OFD_TYPE_DATA:
		now = Sys_Nanoseconds();
		if ( !( s = CL_ContentFindSlot( m.u.data.serial, 0 ) ) ) {
			clContent.drops[DROP_STALE]++;
		} else if ( !CL_ContentData( s, &m.u.data, m.flags, now ) ) {
			break;		// unacked, so sent again
		}
		CL_ContentPacketNumber( m.u.data.packetNumber, now );
		break;
	default:	// a NOTICE, step 6's, which this client doesn't take
		clContent.drops[DROP_STATE]++;
		break;
	}
	CL_ContentService();
}

/*
==============================================================

THE SET

==============================================================
*/

/*
=================
CL_ContentFinishFetch

A pak whose every byte is here and hashed: its SHA-256 checked, then its
pak checksum, then its .part made the cache's complete pak, which the
filesystem restart after the set mounts
=================
*/
static void CL_ContentFinishFetch( clcSlot_t *s ) {
	int			fetch = s->fetch;
	clcFetch_t	*f = &clContent.fetches[fetch];
	byte		digest[SHA256_DIGEST_SIZE];
	char		why[MAX_STRING_CHARS], path[MAX_OSPATH];

	Com_SHA256Final( &s->sha, digest );
	CL_ContentKeepFinished( s );

	if ( memcmp( digest, f->sha256, SHA256_DIGEST_SIZE ) ) {
		CL_ContentFreeSlot( s, qfalse, qtrue );
		f->valid = 0;
		if ( ++f->mismatches < 2 ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: %s/%s.pk3 arrived with the wrong SHA-256, so it's asked for "
				"again\n", f->gameDir, f->name );
			f->state = FETCH_QUEUED;
		} else {
			CL_ContentFetchEnded( fetch, FETCH_CLASSIC, "it arrived with the wrong SHA-256 twice" );
		}
		return;
	}

	Q_strncpyz( path, s->path, sizeof( path ) );
	CL_ContentFreeSlot( s, qfalse, qfalse );
	if ( !FS_CompareZipChecksum( path, f->checksum ) ) {
		Content_Cache_DropPartial( f->sha256 );
		CL_ContentFetchEnded( fetch, FETCH_FAILED,
			"the server's own copy doesn't have the checksum the server gives for it" );
		return;
	}
	if ( !Content_Cache_CompletePartial( f->sha256, why, sizeof( why ) ) ) {
		CL_ContentFetchEnded( fetch, FETCH_FAILED, why );
		return;
	}
	Com_Printf( "Downloaded %s/%s.pk3, %u bytes, from the server by the fast path\n", f->gameDir, f->name, f->size );
	clc.downloadRestart = qtrue;
	CL_ContentFetchEnded( fetch, FETCH_DONE, NULL );
}

/*
=================
CL_ContentFindEntry

The required entry for a missing pak: one with its checksum, by its name
if more than one has it
=================
*/
static qboolean CL_ContentFindEntry( const clcPak_t *p, ofcmEntry_t *best ) {
	ofcmEntry_t	entry;
	qboolean	found = qfalse;
	int			i;

	for ( i = 0; i < clContent.header.numRequired; i++ ) {
		OFCM_ReadEntry( clContent.manifest, i, &entry );
		if ( (unsigned int)entry.checksum != p->checksum ) {
			continue;
		}
		if ( !found || !Q_stricmp( va( "%s/%s.pk3", entry.gameDir, entry.name ), p->name ) ) {
			*best = entry;
		}
		found = qtrue;
	}
	return found;
}

/*
=================
CL_ContentUseManifest

The content manifest, checked: which of the missing paks it lists, which
the client has installed elsewhere, and which to fetch, each distinct hash
once (section 9.1)
=================
*/
static void CL_ContentUseManifest( void ) {
	char		own[MAX_STRING_CHARS], why[MAX_STRING_CHARS];
	ofcmEntry_t	entry;
	clcPak_t	*p;
	clcFetch_t	*f;
	int64_t		total;
	int			i, j;

	*own = '\0';
	for ( i = 0; i < clContent.numPaks; i++ ) {
		p = &clContent.paks[i];
		if ( !CL_ContentFindEntry( p, &entry ) ) {
			Com_Printf( "%s comes the classic way: the server's content manifest doesn't list it\n", p->name );
			p->state = PAK_CLASSIC;
			continue;
		}
		if ( entry.flags & OFCM_FLAG_OWN ) {
			Q_strcat( own, sizeof( own ), va( "%s%s/%s.pk3", *own ? ", " : "", entry.gameDir, entry.name ) );
			continue;
		}
		// asked for already on this connection, and still missing
		if ( CL_ContentIsDone( entry.sha256 ) ) {
			p->state = PAK_CLASSIC;
			continue;
		}
		// installed outside the search path, under its hash
		if ( Content_PakIndex_FindSHA256( entry.sha256 ) ) {
			if ( Content_Cache_StoreInstalled( entry.sha256, entry.gameDir, entry.name, entry.checksum, why, sizeof( why ) ) ) {
				Com_Printf( "%s is installed under another name: copied into the content cache\n", p->name );
				CL_ContentSetDone( entry.sha256 );
				clc.downloadRestart = qtrue;
				p->state = PAK_DONE;
				continue;
			}
			Com_DPrintf( "Not using the installed copy of %s: %s\n", p->name, why );
		}
		for ( j = 0; j < clContent.numFetches && memcmp( clContent.fetches[j].sha256, entry.sha256, SHA256_DIGEST_SIZE ); j++ ) {
		}
		if ( j == clContent.numFetches ) {
			f = &clContent.fetches[clContent.numFetches++];
			Com_Memset( f, 0, sizeof( *f ) );
			Com_Memcpy( f->sha256, entry.sha256, SHA256_DIGEST_SIZE );
			f->size = entry.size;
			f->checksum = entry.checksum;
			Q_strncpyz( f->gameDir, entry.gameDir, sizeof( f->gameDir ) );
			Q_strncpyz( f->name, entry.name, sizeof( f->name ) );
		}
		p->fetch = j;
	}

	// none of the game's own paks comes from a server, so nothing does
	if ( *own ) {
		Com_Error( ERR_DROP, "Add %s from your own copy of the game, then connect again", own );
	}

	// a pak larger than the cache keeps isn't fetched; the rest must fit
	total = 0;
	for ( i = 0; i < clContent.numFetches; i++ ) {
		f = &clContent.fetches[i];
		if ( !Content_Cache_PakFits( f->size, why, sizeof( why ) ) ) {
			CL_ContentFetchEnded( i, FETCH_FAILED, why );
		} else {
			total += f->size;
		}
	}
	if ( !Content_Cache_HasRoom( total, why, sizeof( why ) ) ) {
		Com_Error( ERR_DROP, "Can't download the server's paks: %s", why );
	}
	clContent.phase = CLC_FETCHING;
}

/*
=================
CL_ContentManifestArrived

The content manifest's bytes, received or the client's own copy, which
takes data: checked, kept if fresh, and used
=================
*/
static void CL_ContentManifestArrived( byte *data, int length, const byte sha256[SHA256_DIGEST_SIZE], qboolean fresh ) {
	byte		digest[SHA256_DIGEST_SIZE];
	const char	*error;

	Com_SHA256Buffer( data, length, digest );
	if ( memcmp( digest, sha256, SHA256_DIGEST_SIZE ) ) {
		free( data );
		CL_ContentGiveUp( "the content manifest arrived with the wrong SHA-256" );
		return;
	}
	if ( ( error = OFCM_Read( data, length, &clContent.header, FS_ValidDownloadPak ) ) != NULL ) {
		free( data );
		CL_ContentGiveUp( va( "the server's content manifest breaks a rule: %s", error ) );
		return;
	}
	if ( fresh && !Content_Manifest_Store( data, length, FS_ValidDownloadPak, digest ) ) {
		Com_DPrintf( "Couldn't keep the content manifest\n" );
	}
	free( clContent.manifest );
	clContent.manifest = data;
	CL_ContentUseManifest();
}

/*
=================
CL_ContentSlotAnswered

A request answered with a status other than 0, or a transfer the server
ended early (section 6.6)
=================
*/
static void CL_ContentSlotAnswered( clcSlot_t *s ) {
	int			answer = s->answer, retry = s->retryMsec, fetch = s->fetch;
	uint32_t	offset = s->offset;
	clcFetch_t	*f = fetch >= 0 ? &clContent.fetches[fetch] : NULL;

	CL_ContentFreeSlot( s, qfalse, qfalse );
	if ( answer == ( ANSWER_STATUS | OFD_STATUS_DISABLED ) ) {
		CL_ContentGiveUp( "the server's fast downloads are off" );
		return;
	}
	if ( !f ) {
		if ( answer == ( ANSWER_STATUS | OFD_STATUS_BUSY ) || answer == ( ANSWER_STATUS | OFD_STATUS_LIMIT ) ) {
			CL_ContentAsk( s, -1 );
			s->notBefore = Sys_Nanoseconds() + retry * CLC_MSEC;
		} else {
			CL_ContentGiveUp( va( "the server answered the content manifest's request with %s %d",
				answer & ANSWER_CLOSE ? "a CLOSE," : "status", answer & 0xff ) );
		}
		return;
	}

	switch ( answer ) {
	case ANSWER_STATUS | OFD_STATUS_LIMIT:
		if ( ++f->limits >= CLC_LIMIT_RETRIES ) {
			CL_ContentFetchEnded( fetch, FETCH_CLASSIC, "the server's limit, five times" );
			break;
		}
		// fall through: it waits, as for busy
	case ANSWER_STATUS | OFD_STATUS_BUSY:
		f->state = FETCH_WAITING;
		f->retryAt = Sys_Nanoseconds() + retry * CLC_MSEC;
		break;
	case ANSWER_STATUS | OFD_STATUS_RETAIL:
		CL_ContentFetchEnded( fetch, FETCH_FAILED, "the server doesn't send it: it's one of the game's own paks" );
		break;
	case ANSWER_STATUS | OFD_STATUS_BAD_REQUEST:
		Com_Printf( "The server called the request for %s/%s.pk3 from byte %u bad\n", f->gameDir, f->name, offset );
		CL_ContentFetchEnded( fetch, FETCH_CLASSIC, "the server called the request bad" );
		break;
	case ANSWER_STATUS | OFD_STATUS_NOT_FOUND:
		CL_ContentFetchEnded( fetch, FETCH_CLASSIC, "the server doesn't have it by its hash" );
		break;
	case ANSWER_CLOSE | OFD_CLOSE_STALLED:
	case ANSWER_CLOSE | OFD_CLOSE_PREEMPTED:
		f->state = FETCH_QUEUED;	// asked for again, from where it stopped
		break;
	case ANSWER_STATUS | OFD_STATUS_READ_ERROR:
	case ANSWER_CLOSE | OFD_CLOSE_READ_ERROR:
		CL_ContentFetchEnded( fetch, FETCH_CLASSIC, "the server couldn't read it" );
		break;
	default:	// gone from the content manifest, or canceled
		CL_ContentFetchEnded( fetch, FETCH_CLASSIC, "the server stopped sending it" );
		break;
	}
}

/*
=================
CL_ContentFinish

The set is done: what's left goes the classic way, then
CL_DownloadsComplete, as CL_StartClassicDownloads ends
=================
*/
static void CL_ContentFinish( void ) {
	char	list[sizeof( clc.downloadList )] = "";
	int		i, classic = 0;

	clContent.finishing = qfalse;
	clContent.phase = CLC_IDLE;
	for ( i = 0; i < CLC_SLOTS; i++ ) {
		CL_ContentFreeSlot( &clContent.slots[i], qtrue, qfalse );
	}
	// before any the fast path didn't take, past CLC_MAX_PAKS
	for ( i = 0; i < clContent.numPaks; i++ ) {
		const clcPak_t	*p = &clContent.paks[i];

		if ( p->state != PAK_DONE ) {
			Q_strcat( list, sizeof( list ), va( "@%s@%s@%08x", p->name, p->name, p->checksum ) );
			classic++;
		}
	}
	Q_strcat( list, sizeof( list ), clc.downloadList );
	Q_strncpyz( clc.downloadList, list, sizeof( clc.downloadList ) );
	CL_ContentClearProgress();
	CL_ClearDownloadName();
	if ( classic ) {
		Com_Printf( "%d of the server's paks come the classic way\n", classic );
	}
	CL_StartClassicDownloads();
}

/*
=================
CL_ContentManifestSlot

The content manifest's transfer, once open: the client's own copy if it has
one, or the bytes once they're all here
=================
*/
static void CL_ContentManifestSlot( clcSlot_t *s ) {
	byte	*data, sha256[SHA256_DIGEST_SIZE];
	int		length;

	Com_Memcpy( sha256, s->sha256, sizeof( sha256 ) );
	if ( !s->cacheChecked ) {
		ofcmHeader_t	header;

		s->cacheChecked = qtrue;
		if ( ( data = Content_Manifest_Load( sha256, FS_ValidDownloadPak, qtrue, &length, &header ) ) != NULL ) {
			Com_DPrintf( "Using the content manifest kept from an earlier visit\n" );
			CL_ContentFreeSlot( s, qtrue, qfalse );
			CL_ContentManifestArrived( data, length, sha256, qfalse );
			return;
		}
	}
	if ( s->chunks == s->numChunks ) {
		data = s->memory;
		length = s->fileSize;
		s->memory = NULL;
		CL_ContentKeepFinished( s );
		CL_ContentFreeSlot( s, qfalse, qfalse );
		CL_ContentManifestArrived( data, length, sha256, qtrue );
	}
}

/*
=================
CL_ContentShowDrops
=================
*/
static void CL_ContentShowDrops( int64_t now ) {
	char	drops[MAX_STRING_CHARS] = "";
	int		i;

	if ( !cl_showContent->integer || !clContent.datagrams || now - clContent.lastShown < 1000 * CLC_MSEC ) {
		return;
	}
	clContent.lastShown = now;
	for ( i = 0; i < DROP_COUNT; i++ ) {
		if ( clContent.drops[i] ) {
			Q_strcat( drops, sizeof( drops ), va( " %s %d", dropNames[i], clContent.drops[i] ) );
		}
	}
	Com_Printf( "content: %d datagrams, dropped:%s\n", clContent.datagrams, *drops ? drops : " none" );
}

/*
=================
CL_ContentFrame

Once a frame: the decisions the packet handler leaves, the hash read back
from the .parts, their valid starts recorded, the timeouts, the next
requests, and the progress. The pak index's hashing job is polled in
SV_Frame, which runs with or without a server
=================
*/
void CL_ContentFrame( void ) {
	int64_t		now = Sys_Nanoseconds();
	clcSlot_t	*s;
	clcFetch_t	*f;
	int			i, j, pending;
	qboolean	active, verifying;

	CL_ContentShowDrops( now );
	if ( clContent.phase == CLC_IDLE || clc.state != CA_CONNECTED ) {
		return;
	}

	for ( i = 0; i < CLC_SLOTS && !clContent.finishing && clContent.phase != CLC_IDLE; i++ ) {
		s = &clContent.slots[i];
		if ( !s->used ) {
			continue;
		}
		if ( s->answer ) {
			CL_ContentSlotAnswered( s );
		} else if ( !s->open ) {
			if ( s->firstSent && now - s->firstSent > CLC_OPEN_TIMEOUT ) {
				CL_ContentGiveUp( "the server didn't answer a request in 10 seconds" );
			}
		} else if ( !s->local && s->chunks < s->numChunks && now - s->lastData > CLC_STALL ) {
			// (with every chunk in, it may still be hashing the start it read back)
			CL_ContentGiveUp( "no data for 15 seconds" );
		} else if ( s->fetch < 0 ) {
			CL_ContentManifestSlot( s );
		} else {
			CL_ContentHashAhead( s, now + CLC_HASH_SLICE );
			if ( s->chunks == s->numChunks && s->hashed == s->fileSize ) {
				CL_ContentFinishFetch( s );
			} else if ( CL_ContentWhole( s ) - s->flushed >= CLC_FLUSH_BYTES ) {
				CL_ContentRecordValid( s );
			}
		}
	}
	if ( clContent.finishing ) {
		CL_ContentFinish();
		return;
	}
	if ( clContent.phase != CLC_FETCHING ) {
		CL_ContentService();
		return;
	}

	// the waits over, and the next fetches into the free slots
	pending = 0;
	active = verifying = qfalse;
	for ( i = 0; i < clContent.numFetches; i++ ) {
		f = &clContent.fetches[i];
		if ( f->state == FETCH_WAITING && now >= f->retryAt ) {
			f->state = FETCH_QUEUED;
		}
		if ( f->state == FETCH_QUEUED ) {
			for ( j = 0; j < CLC_SLOTS && clContent.slots[j].used; j++ ) {
			}
			if ( j < CLC_SLOTS ) {
				CL_ContentAsk( &clContent.slots[j], i );
			}
		}
		pending += f->state == FETCH_QUEUED || f->state == FETCH_ACTIVE || f->state == FETCH_WAITING;
		active |= f->state == FETCH_ACTIVE;
	}
	if ( !pending ) {
		CL_ContentFinish();
		return;
	}
	// the progress ten times a second, which a bar or a rate needs no more
	if ( now - clContent.lastProgress >= 100 * CLC_MSEC ) {
		clContent.lastProgress = now;
		for ( i = 0; i < CLC_SLOTS; i++ ) {
			s = &clContent.slots[i];
			verifying |= s->used && s->fetch >= 0 && s->hashed < s->offset;
		}
		CL_ContentSetProgress( verifying ? "verifying" : active ? "downloading" : "waiting" );
	}
	CL_ContentService();
}

/*
=================
CL_ContentStart

From CL_InitDownloads, with the paks the gamestate references that the
client lacks in clc.downloadList: whether the fast path takes them. It
doesn't on a classic server, one whose downloads or this client's leave
out the server itself, when the paks come from sv_dlURL, or once it has
given up on the connection
=================
*/
qboolean CL_ContentStart( void ) {
	clcPak_t	*p;
	char		local[MAX_OSPATH];

	if ( !( clc.ofx & OFX_CONTENT ) || clContent.classicOnly ||
		!( cl_allowDownload->integer & DLF_ENABLE ) || ( cl_allowDownload->integer & DLF_NO_UDP ) ||
		!( clc.sv_allowDownload & DLF_ENABLE ) || ( clc.sv_allowDownload & DLF_NO_UDP ) || CL_DownloadsByHTTP() ) {
		return qfalse;
	}

	while ( clContent.numPaks < CLC_MAX_PAKS ) {
		p = &clContent.paks[clContent.numPaks];
		Com_Memset( p, 0, sizeof( *p ) );
		if ( !CL_NextListedPak( p->name, sizeof( p->name ), local, sizeof( local ), &p->checksum ) ) {
			break;
		}
		p->fetch = -1;
		p->state = PAK_FAST;
		clContent.numPaks++;
	}
	if ( !clContent.numPaks ) {
		return qfalse;
	}

	clContent.receiveWindow = CL_ContentReceiveWindow();

	Com_Printf( "Asking the server for %d pak%s by the fast path\n", clContent.numPaks,
		clContent.numPaks == 1 ? "" : "s" );
	clContent.phase = CLC_MANIFEST;
	CL_ContentAsk( &clContent.slots[0], -1 );
	Cvar_Set( "cl_downloadState", "manifest" );
	Cvar_Set( "cl_downloadSource", "server" );
	CL_ContentService();
	return qtrue;
}

/*
=================
CL_ContentReset

Ends the set under way, as a new gamestate or a disconnect does: the open
transfers canceled, unless the connection is gone, their .parts kept for
a resume. With disconnect, what the connection remembered goes too
=================
*/
void CL_ContentReset( qboolean disconnect ) {
	int		i;

	for ( i = 0; i < CLC_SLOTS; i++ ) {
		CL_ContentFreeSlot( &clContent.slots[i], !disconnect, qfalse );
	}
	free( clContent.manifest );
	clContent.manifest = NULL;
	clContent.phase = CLC_IDLE;
	clContent.finishing = qfalse;
	clContent.numPaks = clContent.numFetches = 0;
	if ( disconnect ) {
		Com_Memset( &clContent, 0, sizeof( clContent ) );
		CL_ContentClearProgress();
	}
}

/*
=================
CL_ContentInit
=================
*/
void CL_ContentInit( void ) {
	cl_showContent = Cvar_Get( "cl_showContent", "0", CVAR_TEMP );
	Cvar_SetDescription( cl_showContent, "Prints the fast download's datagrams once a second, with those "
		"dropped and why" );
	// what the client reports of a set of downloads, which the menus read
	// and only it writes
	Cvar_Get( "cl_downloadTotal", "0", CVAR_ROM );
	Cvar_Get( "cl_downloadTotalCount", "0", CVAR_ROM );
	Cvar_Get( "cl_downloadFiles", "", CVAR_ROM );
	Cvar_Get( "cl_downloadState", "", CVAR_ROM );
	Cvar_Get( "cl_downloadSource", "", CVAR_ROM );
}
