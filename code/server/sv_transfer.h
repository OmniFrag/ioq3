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
// sv_transfer.h -- one client's fast downloads (download-protocol.md
// section 6): its transfers, the datagrams in flight, the acks that come
// back, loss, probes and retransmissions, over the congestion controller
// (sv_dlcc.h). Pure, as sv_dlcc.c is: what it needs of the server (which
// requests to admit, the files' bytes, the network, random numbers) comes
// through svTransferEnv_t, and times are microseconds, given

#ifndef SV_TRANSFER_H
#define SV_TRANSFER_H

#include "../qcommon/q_shared.h"
#include "../qcommon/ofd.h"
#include "sv_dlcc.h"

#define SVT_RING			4096	// datagrams in flight, by packet number
#define SVT_RETRANSMITS		4096	// chunks lost, waiting to go again
#define SVT_TRANSFERS		2		// open at once
#define SVT_CLOSED			4		// closed ones remembered, to say so again
#define SVT_MEMOS			4		// request IDs remembered with their OPEN
#define SVT_NEW_REQUESTS	10		// new request IDs a second, past which: limit
#define SVT_STALL			( 15 * 1000000LL )	// no ack this long closes the transfers
#define SVT_REPEAT_OPEN		( 20 * 1000 )		// a repeated request's OPEN, at most this often
#define SVT_REPEAT_CLOSE	( 100 * 1000 )		// a closed transfer's CLOSE again, at most this often
#define SVT_SKIP_MIN		128		// packet numbers between skipped ones, at least,
#define SVT_SKIP_RANGE		256		// plus up to this
#define SVT_SKIPS			( SVT_RING / SVT_SKIP_MIN )	// skipped ones remembered: the ring's span

// what the server makes of a request that may open a transfer
typedef struct {
	byte		status;			// OFD_STATUS_*
	byte		kind;			// OFD_KIND_*
	uint32_t	fileSize;
	uint32_t	retryAfter;		// ms, for busy and limit
	byte		sha256[32];		// the file's: for the content manifest's request, its hash
	void		*file;			// for OK, what read and close take
} svtAdmission_t;

typedef struct {
	void		*arg;
	// the answer to a request in the spec's order (section 6.1): slot is the
	// transfer slot its file would hold, or -1 when this client asked too
	// much, which is limit, after disabled
	void		( *admit )( void *arg, const ofdRequest_t *request, int slot, svtAdmission_t *out );
	// length bytes of a file at offset; qfalse when they can't be read
	qboolean	( *read )( void *arg, void *file, uint32_t offset, byte *out, int length );
	void		( *close )( void *arg, void *file );
	// a datagram to the client; qfalse when the socket can't take it now
	qboolean	( *send )( void *arg, const byte *data, int length );
	uint32_t	( *rand32 )( void *arg );
} svTransferEnv_t;

typedef struct {
	uint16_t	serial;			// 0 for a free slot
	uint32_t	requestId;
	byte		kind;
	qboolean	loading;
	byte		sha256[32];
	int			chunkSize;
	uint32_t	fileSize;
	uint32_t	startOffset;
	uint32_t	numChunks;
	uint32_t	nextChunk;		// the next never sent
	uint32_t	cumulative;		// every chunk below it is the client's
	void		*file;
} svtTransfer_t;

typedef struct {
	uint16_t	serial;			// 0 for none
	byte		reason;
	uint32_t	requestId;
	int64_t		lastSent;
} svtClosed_t;

typedef struct {
	uint32_t	requestId;
	qboolean	used;
	ofdOpen_t	open;
	int64_t		lastSent;
} svtMemo_t;

// a DATA sent, in flight until acked or lost
typedef struct {
	uint32_t	packet;
	uint32_t	chunk;
	uint32_t	sent;			// the low 32 bits of the time
	uint16_t	serial;
	uint16_t	bytes;			// the datagram's; 0 once it's no longer in flight
} svtFlight_t;

typedef struct {
	uint32_t	chunk;
	uint16_t	serial;
	byte		probe;			// sent past the window
} svtRetransmit_t;

typedef struct {
	uint64_t		key;
	dlcc_t			cc;
	qboolean		ccStarted;
	svtTransfer_t	transfers[SVT_TRANSFERS];
	int				turn;			// the transfer whose new chunk goes next
	svtClosed_t		closed[SVT_CLOSED];
	int				nextClosed;
	svtMemo_t		memos[SVT_MEMOS];
	int				nextMemo;
	uint16_t		nextSerial;
	int64_t			requestWindow;	// the second new request IDs are counted in
	int				newRequests;

	svtFlight_t		ring[SVT_RING];
	uint32_t		nextPacket;		// from 1
	uint32_t		nextSkip;
	uint32_t		skips[SVT_SKIPS];	// the skipped packet numbers, 0 for none
	int				nextSkipSlot;
	uint32_t		oldestInFlight;	// no record in flight below it
	uint32_t		largestAcked;	// 0 for none
	svtRetransmit_t	retransmits[SVT_RETRANSMITS];
	int				retransmitHead, retransmitCount;

	int64_t			lastAck;		// or the first send's time, for the stall
	int64_t			lastProgress;	// for the probe timeout: the last ack or probe
	uint32_t		datagramsSent, datagramsResent;
} svTransfers_t;

typedef struct {
	const char	*drop;			// why the client is dropped, or NULL
	qboolean	loadingPakOpened;	// a pak transfer opened, class loading
} svtResult_t;

void		SVT_Init( svTransfers_t *t, uint64_t key, const svTransferEnv_t *env );
// a CONTROL from the client; active when it's in the game, which makes
// every request prefetch
svtResult_t	SVT_Control( svTransfers_t *t, const ofdControl_t *control, qboolean active, int64_t now,
				const svTransferEnv_t *env );
// whether there's something to send, and when: 0 now, the microseconds to
// wait, or -1 for nothing to send or the window full
int64_t		SVT_SendWait( svTransfers_t *t, int64_t now );
// sends one DATA, a probe or a lost chunk first; capFree when the server's
// shared cap would let another go after it. The datagram's bytes, or 0 if
// none went
int			SVT_Send( svTransfers_t *t, int64_t now, qboolean capFree, const svTransferEnv_t *env );
// the probe timeout and the stall; the microseconds until it's due again,
// or -1 for nothing in flight
int64_t		SVT_Timer( svTransfers_t *t, int64_t now, const svTransferEnv_t *env );
// ends a transfer with a CLOSE of this reason; one at a map change whose
// file left both content manifests is gone
void		SVT_Close( svTransfers_t *t, svtTransfer_t *transfer, byte reason, int64_t now, const svTransferEnv_t *env );
void		SVT_Free( svTransfers_t *t, const svTransferEnv_t *env );
// the client entered the game: every transfer prefetch
void		SVT_EnterGame( svTransfers_t *t );

int			SVT_OpenTransfers( const svTransfers_t *t );
int			SVT_OpenPaks( const svTransfers_t *t );		// the ones that count as downloads
qboolean	SVT_Loading( const svTransfers_t *t );	// a loading pak transfer open

#endif
