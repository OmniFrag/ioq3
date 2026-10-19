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
// sv_transfer.c -- one client's fast downloads (sv_transfer.h)

#include "sv_transfer.h"

#define SVT_SECOND		1000000LL
#define SVT_DATA_EXTRA	( OFD_HEADER_SIZE + OFD_DATA_HEADER )	// a DATA's bytes besides its payload

static const char	svtNeverSent[] = "acknowledged data it was never sent";

static void SVT_SendMessage( svTransfers_t *t, ofdMessage_t *msg, const svTransferEnv_t *env ) {
	byte	out[OFD_MAX_DATAGRAM];
	int		length;

	msg->key = t->key;
	if ( ( length = OFD_Write( msg, out, sizeof( out ) ) ) > 0 ) {
		env->send( env->arg, out, length );
	}
}

static void SVT_SendOpen( svTransfers_t *t, const ofdOpen_t *open, const svTransferEnv_t *env ) {
	ofdMessage_t	msg;

	Com_Memset( &msg, 0, sizeof( msg ) );
	msg.type = OFD_TYPE_OPEN;
	msg.u.open = *open;
	SVT_SendMessage( t, &msg, env );
}

static void SVT_SendClose( svTransfers_t *t, svtClosed_t *closed, int64_t now, const svTransferEnv_t *env ) {
	ofdMessage_t	msg;

	Com_Memset( &msg, 0, sizeof( msg ) );
	msg.type = OFD_TYPE_CLOSE;
	msg.u.close.serial = closed->serial;
	msg.u.close.reason = closed->reason;
	msg.u.close.requestId = closed->requestId;
	SVT_SendMessage( t, &msg, env );
	closed->lastSent = now;
}

// the packet numbers between skipped ones (download-protocol.md section 7.7)
static uint32_t SVT_SkipSpacing( const svTransferEnv_t *env ) {
	return SVT_SKIP_MIN + env->rand32( env->arg ) % SVT_SKIP_RANGE;
}

/*
=================
SVT_Init

Packet numbers start at 1, so a largest acknowledged of 0 is none
=================
*/
void SVT_Init( svTransfers_t *t, uint64_t key, const svTransferEnv_t *env ) {
	Com_Memset( t, 0, sizeof( *t ) );
	t->key = key;
	t->nextSerial = 1;
	t->nextPacket = 1;
	t->nextSkip = t->nextPacket + SVT_SkipSpacing( env );
}

static svtTransfer_t *SVT_Find( svTransfers_t *t, uint16_t serial ) {
	int		i;

	for ( i = 0; i < SVT_TRANSFERS; i++ ) {
		if ( serial && t->transfers[i].serial == serial ) {
			return &t->transfers[i];
		}
	}
	return NULL;
}

// a closed transfer acked or canceled again: its CLOSE again, at most
// every 100 ms
static void SVT_RepeatClose( svTransfers_t *t, uint16_t serial, int64_t now, const svTransferEnv_t *env ) {
	int		i;

	for ( i = 0; i < SVT_CLOSED; i++ ) {
		if ( serial && t->closed[i].serial == serial && now - t->closed[i].lastSent >= SVT_REPEAT_CLOSE ) {
			SVT_SendClose( t, &t->closed[i], now, env );
		}
	}
}

int SVT_OpenTransfers( const svTransfers_t *t ) {
	int		i, count = 0;

	for ( i = 0; i < SVT_TRANSFERS; i++ ) {
		count += t->transfers[i].serial != 0;
	}
	return count;
}

// a closed transfer's slot is all zero, whose kind reads as a pak
int SVT_OpenPaks( const svTransfers_t *t ) {
	int		i, count = 0;

	for ( i = 0; i < SVT_TRANSFERS; i++ ) {
		count += t->transfers[i].serial && t->transfers[i].kind == OFD_KIND_PAK;
	}
	return count;
}

// a closed transfer's slot is all zero, so never loading
qboolean SVT_Loading( const svTransfers_t *t ) {
	int		i;

	for ( i = 0; i < SVT_TRANSFERS; i++ ) {
		if ( t->transfers[i].kind == OFD_KIND_PAK && t->transfers[i].loading ) {
			return qtrue;
		}
	}
	return qfalse;
}

// the controller's class: loading while a loading transfer is open
static void SVT_FitClass( svTransfers_t *t ) {
	dlccClass_t		dlClass = DLCC_PREFETCH;
	int				i;

	for ( i = 0; i < SVT_TRANSFERS; i++ ) {
		if ( t->transfers[i].loading ) {
			dlClass = DLCC_LOADING;
		}
	}
	if ( t->ccStarted && t->cc.dlClass != dlClass ) {
		DLCC_SetClass( &t->cc, dlClass );
	}
}

// the lowest packet number the ring still holds
static uint32_t SVT_RingLow( const svTransfers_t *t ) {
	return t->nextPacket > SVT_RING ? t->nextPacket - SVT_RING : 0;
}

static qboolean SVT_InFlight( const svtFlight_t *f, uint32_t packet ) {
	return f->packet == packet && f->bytes;
}

// how long ago a record was sent; the stall closes everything long before
// its 32 bits of microseconds wrap
static int64_t SVT_Age( const svtFlight_t *f, int64_t now ) {
	return (int32_t)( (uint32_t)now - f->sent );
}

// past the records no longer in flight
static void SVT_AdvanceOldest( svTransfers_t *t ) {
	t->oldestInFlight = MAX( t->oldestInFlight, SVT_RingLow( t ) );
	while ( t->oldestInFlight < t->nextPacket
		&& !SVT_InFlight( &t->ring[t->oldestInFlight % SVT_RING], t->oldestInFlight ) ) {
		t->oldestInFlight++;
	}
}

// a closing transfer's records in flight leave the flight now (section
// 6.6): none of them goes again or is probed, so an ack never comes for
// the ones the client drops, and their loss would cut the window
static void SVT_DropFlight( svTransfers_t *t, uint16_t serial ) {
	svtFlight_t	*f;
	uint32_t	p;
	int64_t		bytes = 0;

	for ( p = MAX( t->oldestInFlight, SVT_RingLow( t ) ); p < t->nextPacket; p++ ) {
		f = &t->ring[p % SVT_RING];
		if ( SVT_InFlight( f, p ) && f->serial == serial ) {
			bytes += f->bytes;
			f->bytes = 0;
		}
	}
	t->cc.bytesInFlight = MAX( t->cc.bytesInFlight - bytes, 0 );
	SVT_AdvanceOldest( t );
}

/*
=================
SVT_Close
=================
*/
void SVT_Close( svTransfers_t *t, svtTransfer_t *transfer, byte reason, int64_t now, const svTransferEnv_t *env ) {
	svtClosed_t	*closed = &t->closed[t->nextClosed];

	t->nextClosed = ( t->nextClosed + 1 ) % SVT_CLOSED;
	closed->serial = transfer->serial;
	closed->reason = reason;
	closed->requestId = transfer->requestId;
	SVT_SendClose( t, closed, now, env );
	if ( transfer->file ) {
		env->close( env->arg, transfer->file );
	}
	SVT_DropFlight( t, transfer->serial );
	Com_Memset( transfer, 0, sizeof( *transfer ) );
	SVT_FitClass( t );
}

/*
=================
SVT_EnterGame

The client is in the game: its transfers are prefetch from now on
(section 7.1), as a request from it would be
=================
*/
void SVT_EnterGame( svTransfers_t *t ) {
	int		i;

	for ( i = 0; i < SVT_TRANSFERS; i++ ) {
		t->transfers[i].loading = qfalse;
	}
	SVT_FitClass( t );
}

/*
=================
SVT_Free

The client gone: its files closed, with no word to it
=================
*/
void SVT_Free( svTransfers_t *t, const svTransferEnv_t *env ) {
	int		i;

	for ( i = 0; i < SVT_TRANSFERS; i++ ) {
		if ( t->transfers[i].file ) {
			env->close( env->arg, t->transfers[i].file );
		}
	}
	Com_Memset( t->transfers, 0, sizeof( t->transfers ) );
}

/*
=================
SVT_Request

A request, new or repeated. A repeat, until the client sees its OPEN, gets
the same OPEN, at most every 20 ms
=================
*/
static void SVT_Request( svTransfers_t *t, const ofdRequest_t *request, qboolean active, int64_t now,
	const svTransferEnv_t *env, svtResult_t *result ) {
	svtAdmission_t	admission;
	svtTransfer_t	*transfer;
	svtMemo_t		*memo;
	ofdOpen_t		open;
	int				i, slot = -1;

	for ( i = 0; i < SVT_MEMOS; i++ ) {
		memo = &t->memos[i];
		if ( memo->used && memo->requestId == request->requestId ) {
			if ( now - memo->lastSent >= SVT_REPEAT_OPEN ) {
				SVT_SendOpen( t, &memo->open, env );
				memo->lastSent = now;
			}
			return;
		}
	}

	// the free slot, unless this is more than 10 new request IDs in a
	// second, or a third transfer
	if ( now - t->requestWindow >= SVT_SECOND ) {
		t->requestWindow = now;
		t->newRequests = 0;
	}
	if ( ++t->newRequests <= SVT_NEW_REQUESTS ) {
		for ( i = 0; i < SVT_TRANSFERS && slot < 0; i++ ) {
			if ( !t->transfers[i].serial ) {
				slot = i;
			}
		}
	}

	Com_Memset( &admission, 0, sizeof( admission ) );
	env->admit( env->arg, request, slot, &admission );

	Com_Memset( &open, 0, sizeof( open ) );
	open.requestId = request->requestId;
	open.status = admission.status;
	open.kind = admission.kind;
	open.retryAfter = admission.retryAfter;
	Com_Memcpy( open.sha256, admission.status == OFD_STATUS_OK ? admission.sha256 : request->sha256,
		sizeof( open.sha256 ) );

	if ( admission.status == OFD_STATUS_OK ) {
		transfer = &t->transfers[slot];
		transfer->serial = t->nextSerial++;
		if ( !t->nextSerial ) {
			t->nextSerial = 1;
		}
		transfer->requestId = request->requestId;
		transfer->kind = admission.kind;
		transfer->loading = !active && request->requestClass == OFD_CLASS_LOADING;
		Com_Memcpy( transfer->sha256, admission.sha256, sizeof( transfer->sha256 ) );
		transfer->chunkSize = OFD_ChunkSize( request->largest );
		transfer->fileSize = admission.fileSize;
		transfer->startOffset = request->offset;
		transfer->numChunks = ( admission.fileSize - request->offset + transfer->chunkSize - 1 ) / transfer->chunkSize;
		transfer->file = admission.file;

		open.serial = transfer->serial;
		open.chunkSize = transfer->chunkSize;
		open.fileSize = transfer->fileSize;
		open.startOffset = transfer->startOffset;

		if ( !t->ccStarted ) {
			DLCC_Init( &t->cc, transfer->loading ? DLCC_LOADING : DLCC_PREFETCH,
				transfer->chunkSize + SVT_DATA_EXTRA, now );
			t->ccStarted = qtrue;
		}
		SVT_FitClass( t );
		if ( SVT_OpenTransfers( t ) == 1 ) {
			t->lastAck = t->lastProgress = now;
		}
		if ( transfer->kind == OFD_KIND_PAK && transfer->loading ) {
			result->loadingPakOpened = qtrue;
		}
	}

	memo = &t->memos[t->nextMemo];
	t->nextMemo = ( t->nextMemo + 1 ) % SVT_MEMOS;
	memo->used = qtrue;
	memo->requestId = request->requestId;
	memo->open = open;
	memo->lastSent = now;
	SVT_SendOpen( t, &open, env );
}

// a lost chunk queued to go again; a probe goes to the front, past the
// window
static void SVT_Retransmit( svTransfers_t *t, uint16_t serial, uint32_t chunk, qboolean probe ) {
	svtRetransmit_t	*r;

	if ( t->retransmitCount >= SVT_RETRANSMITS ) {
		return;
	}
	if ( probe ) {
		t->retransmitHead = ( t->retransmitHead + SVT_RETRANSMITS - 1 ) % SVT_RETRANSMITS;
		r = &t->retransmits[t->retransmitHead];
	} else {
		r = &t->retransmits[( t->retransmitHead + t->retransmitCount ) % SVT_RETRANSMITS];
	}
	r->serial = serial;
	r->chunk = chunk;
	r->probe = probe;
	t->retransmitCount++;
}

/*
=================
SVT_Acks

Section 6.2: the RTT sample, the ranges, the cumulative chunks, then the
records lost below the largest acknowledged. A range is walked only from
the oldest record in flight, as nothing below it is; the skipped packet
numbers are checked against it on their own
=================
*/
static const char *SVT_Acks( svTransfers_t *t, const ofdControl_t *control, int64_t now, const svTransferEnv_t *env ) {
	svtFlight_t		*f;
	svtTransfer_t	*transfer;
	uint32_t		low, high, p;
	int				i, j, acked = 0, lost = 0;
	int64_t			newestLostSent = 0, lossDelay;

	if ( !t->ccStarted || control->largest >= t->nextPacket ) {
		return svtNeverSent;
	}

	if ( control->largest > t->largestAcked ) {
		f = &t->ring[control->largest % SVT_RING];
		if ( SVT_InFlight( f, control->largest ) ) {
			int64_t	rtt = SVT_Age( f, now ) - (int64_t)control->ackDelay * 10;

			if ( rtt > 0 ) {
				DLCC_OnRttSample( &t->cc, now, rtt );
			}
		}
		t->largestAcked = control->largest;
	}

	for ( i = 0; i <= control->numRanges; i++ ) {
		OFD_AckRange( control, i, &low, &high );
		// packet 0 is never sent
		if ( !low ) {
			return svtNeverSent;
		}
		for ( j = 0; j < SVT_SKIPS; j++ ) {
			if ( t->skips[j] && t->skips[j] >= low && t->skips[j] <= high ) {
				return svtNeverSent;
			}
		}
		for ( p = MAX( MAX( low, t->oldestInFlight ), SVT_RingLow( t ) ); p <= high; p++ ) {
			f = &t->ring[p % SVT_RING];
			if ( SVT_InFlight( f, p ) ) {
				acked += f->bytes;
				f->bytes = 0;
			}
		}
	}

	for ( i = 0; i < control->numTransfers; i++ ) {
		const ofdAckTransfer_t	*ack = &control->transfers[i];

		if ( !( transfer = SVT_Find( t, ack->serial ) ) ) {
			SVT_RepeatClose( t, ack->serial, now, env );
			continue;
		}
		if ( ack->cumulativeChunk > transfer->nextChunk ) {
			return svtNeverSent;
		}
		transfer->cumulative = MAX( transfer->cumulative, ack->cumulativeChunk );
		if ( transfer->cumulative >= transfer->numChunks ) {
			SVT_Close( t, transfer, OFD_CLOSE_COMPLETE, now, env );
		}
	}

	// lost: 3 packets below the largest acknowledged, or sent longer ago
	// than the time threshold
	lossDelay = DLCC_LossDelay( &t->cc );
	for ( p = MAX( t->oldestInFlight, SVT_RingLow( t ) ); p < t->largestAcked; p++ ) {
		f = &t->ring[p % SVT_RING];
		if ( !SVT_InFlight( f, p ) || ( p + 3 > t->largestAcked && SVT_Age( f, now ) <= lossDelay ) ) {
			continue;
		}
		lost += f->bytes;
		newestLostSent = MAX( newestLostSent, now - SVT_Age( f, now ) );
		f->bytes = 0;
		if ( ( transfer = SVT_Find( t, f->serial ) ) != NULL && f->chunk >= transfer->cumulative ) {
			SVT_Retransmit( t, f->serial, f->chunk, qfalse );
		}
	}
	SVT_AdvanceOldest( t );

	DLCC_OnAck( &t->cc, now, acked, lost, newestLostSent );
	if ( acked ) {
		t->lastAck = t->lastProgress = now;
	}
	return NULL;
}

/*
=================
SVT_Control
=================
*/
svtResult_t SVT_Control( svTransfers_t *t, const ofdControl_t *control, qboolean active, int64_t now,
	const svTransferEnv_t *env ) {
	svtResult_t		result;
	svtTransfer_t	*transfer;
	int				i;

	Com_Memset( &result, 0, sizeof( result ) );
	if ( control->blocks & OFD_BLOCK_ACKS ) {
		if ( ( result.drop = SVT_Acks( t, control, now, env ) ) != NULL ) {
			return result;
		}
	}

	// a cancel frees its transfer now; one already closed is told so again
	for ( i = 0; i < control->numCancels; i++ ) {
		if ( ( transfer = SVT_Find( t, control->cancels[i] ) ) != NULL ) {
			SVT_Close( t, transfer, OFD_CLOSE_CANCELED, now, env );
		} else {
			SVT_RepeatClose( t, control->cancels[i], now, env );
		}
	}

	for ( i = 0; i < control->numRequests; i++ ) {
		SVT_Request( t, &control->requests[i], active, now, env, &result );
	}
	// after the requests, so a controller one of them started has it too
	if ( control->receiveWindow && t->ccStarted ) {
		DLCC_SetReceiveWindow( &t->cc, control->receiveWindow );
	}
	return result;
}

/*
=================
SVT_Next

What goes next: a lost chunk first, oldest first, then the transfers' new
chunks in turn. Lost chunks of a closed transfer, or below what the
client has, are passed over
=================
*/
static svtTransfer_t *SVT_Next( svTransfers_t *t, uint32_t *chunk, qboolean *retransmit ) {
	svtTransfer_t	*transfer;
	svtRetransmit_t	*r;
	int				i;

	while ( t->retransmitCount ) {
		r = &t->retransmits[t->retransmitHead];
		transfer = SVT_Find( t, r->serial );
		if ( transfer && r->chunk >= transfer->cumulative ) {
			*chunk = r->chunk;
			*retransmit = qtrue;
			return transfer;
		}
		t->retransmitHead = ( t->retransmitHead + 1 ) % SVT_RETRANSMITS;
		t->retransmitCount--;
	}
	for ( i = 0; i < SVT_TRANSFERS; i++ ) {
		transfer = &t->transfers[( t->turn + i ) % SVT_TRANSFERS];
		if ( transfer->serial && transfer->nextChunk < transfer->numChunks ) {
			*chunk = transfer->nextChunk;
			*retransmit = qfalse;
			return transfer;
		}
	}
	return NULL;
}

static int SVT_ChunkLength( const svtTransfer_t *transfer, uint32_t chunk ) {
	uint32_t	offset = transfer->startOffset + chunk * (uint32_t)transfer->chunkSize;

	return (int)MIN( (uint32_t)transfer->chunkSize, transfer->fileSize - offset );
}

/*
=================
SVT_Ready

What goes next, and SVT_SendWait's answer for it. The packet number's
ring slot must not hold a record still in flight; a probe goes past the
window
=================
*/
static int64_t SVT_Ready( svTransfers_t *t, int64_t now, svtTransfer_t **transfer, uint32_t *chunk,
	qboolean *retransmit ) {
	if ( !t->ccStarted || !( *transfer = SVT_Next( t, chunk, retransmit ) ) ) {
		return -1;
	}
	if ( t->ring[t->nextPacket % SVT_RING].bytes ) {
		return -1;
	}
	if ( *retransmit && t->retransmits[t->retransmitHead].probe ) {
		return 0;
	}
	return DLCC_SendWait( &t->cc, now, SVT_ChunkLength( *transfer, *chunk ) + SVT_DATA_EXTRA );
}

/*
=================
SVT_SendWait
=================
*/
int64_t SVT_SendWait( svTransfers_t *t, int64_t now ) {
	svtTransfer_t	*transfer;
	uint32_t		chunk;
	qboolean		retransmit;

	return SVT_Ready( t, now, &transfer, &chunk, &retransmit );
}

/*
=================
SVT_Send

The chunk is read straight into the datagram, where OFD_Write leaves it
=================
*/
int SVT_Send( svTransfers_t *t, int64_t now, qboolean capFree, const svTransferEnv_t *env ) {
	byte			out[OFD_MAX_DATAGRAM];
	byte			*payload = out + OFD_HEADER_SIZE + OFD_DATA_HEADER;
	svtTransfer_t	*transfer;
	svtFlight_t		*f;
	ofdMessage_t	msg;
	uint32_t		chunk;
	qboolean		retransmit;
	int				length, bytes;

	if ( SVT_Ready( t, now, &transfer, &chunk, &retransmit ) != 0 ) {
		return 0;
	}

	// a skipped packet number: one the client never gets, so acking it
	// gives a liar away
	if ( t->nextPacket == t->nextSkip ) {
		t->skips[t->nextSkipSlot] = t->nextPacket;
		t->nextSkipSlot = ( t->nextSkipSlot + 1 ) % SVT_SKIPS;
		t->nextPacket++;
		t->nextSkip = t->nextPacket + SVT_SkipSpacing( env );
		if ( t->ring[t->nextPacket % SVT_RING].bytes ) {
			return 0;
		}
	}

	length = SVT_ChunkLength( transfer, chunk );
	if ( !env->read( env->arg, transfer->file, transfer->startOffset + chunk * (uint32_t)transfer->chunkSize,
		payload, length ) ) {
		SVT_Close( t, transfer, OFD_CLOSE_READ_ERROR, now, env );
		return 0;
	}
	Com_Memset( &msg, 0, sizeof( msg ) );
	msg.type = OFD_TYPE_DATA;
	msg.key = t->key;
	msg.flags = chunk == transfer->numChunks - 1 ? OFD_FLAG_LAST : 0;
	msg.u.data.packetNumber = t->nextPacket;
	msg.u.data.chunkIndex = chunk;
	msg.u.data.serial = transfer->serial;
	msg.u.data.length = length;
	msg.u.data.payload = payload;
	bytes = OFD_Write( &msg, out, sizeof( out ) );
	if ( !bytes || !env->send( env->arg, out, bytes ) ) {
		return 0;
	}

	if ( retransmit ) {
		t->retransmitHead = ( t->retransmitHead + 1 ) % SVT_RETRANSMITS;
		t->retransmitCount--;
		t->datagramsResent++;
	} else {
		transfer->nextChunk++;
		t->turn = ( transfer - t->transfers + 1 ) % SVT_TRANSFERS;
	}
	f = &t->ring[t->nextPacket % SVT_RING];
	f->packet = t->nextPacket;
	f->chunk = chunk;
	f->serial = transfer->serial;
	f->bytes = bytes;
	f->sent = (uint32_t)now;
	t->nextPacket++;
	t->datagramsSent++;

	DLCC_OnSend( &t->cc, bytes, capFree && SVT_Next( t, &chunk, &retransmit ) != NULL );
	return bytes;
}

/*
=================
SVT_Timer

With nothing acked for a probe timeout, the oldest 2 chunks in flight go
again, past the window; with nothing acked for 15 s, the transfers close
=================
*/
int64_t SVT_Timer( svTransfers_t *t, int64_t now, const svTransferEnv_t *env ) {
	const svtFlight_t	*oldest[2];
	int64_t				due;
	uint32_t			p;
	int					i, probes = 0;

	if ( !t->ccStarted || !SVT_OpenTransfers( t ) ) {
		return -1;
	}
	if ( now - t->lastAck >= SVT_STALL ) {
		for ( i = 0; i < SVT_TRANSFERS; i++ ) {
			if ( t->transfers[i].serial ) {
				SVT_Close( t, &t->transfers[i], OFD_CLOSE_STALLED, now, env );
			}
		}
		return -1;
	}
	if ( t->cc.bytesInFlight <= 0 ) {
		return t->lastAck + SVT_STALL - now;
	}

	due = t->lastProgress + DLCC_ProbeTimeout( &t->cc );
	if ( now < due ) {
		return MIN( due, t->lastAck + SVT_STALL ) - now;
	}
	DLCC_OnPto( &t->cc );
	t->lastProgress = now;
	for ( p = MAX( t->oldestInFlight, SVT_RingLow( t ) ); p < t->nextPacket && probes < 2; p++ ) {
		const svtFlight_t	*f = &t->ring[p % SVT_RING];

		if ( SVT_InFlight( f, p ) && SVT_Find( t, f->serial ) ) {
			oldest[probes++] = f;
		}
	}
	// to the front of the queue, the newer first, so the oldest goes first
	for ( i = probes - 1; i >= 0; i-- ) {
		SVT_Retransmit( t, oldest[i]->serial, oldest[i]->chunk, qtrue );
	}
	return DLCC_ProbeTimeout( &t->cc );
}
