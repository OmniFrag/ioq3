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
// sv_session.c -- the session's handshake, Noise XX from connect (the
// handshake design, section 14): a connect carrying the client's ephemeral
// gets a pending handshake and message 2, and the third message, which
// proves the client's device, makes its slot (SV_AdmitClient). Nothing
// about the player is acted on before then.

#include "server.h"
#include "../qcommon/session.h"

#define SV_PENDING_MAX		1024	// the global bucket's ten seconds, and its burst
#define SV_PENDING_LIFE		10000	// ms, fixed from the connect

typedef struct {
	qboolean			used;
	int					created;			// Sys_Milliseconds
	svAdmit_t			terms;				// what the connect agreed, the address and qport among them
	noiseHandshake_t	hs;					// after message 2; its re is the client's ephemeral
	byte				reply[MAX_PACKETLEN];	// message 2's datagram, sent again unchanged
	int					replyLength;
} svPending_t;

static svPending_t		svPending[SV_PENDING_MAX];
static noiseStatic_t	svStatic;
static qboolean			svStaticReady;
static int				svSessionDrops;

/*
=================
SV_SessionReady

Whether this server can take a session: it needs its key, converted once
to its Noise static
=================
*/
qboolean SV_SessionReady( void ) {
	const identKey_t	*key = Com_ServerKey();

	if ( !key ) {
		return qfalse;
	}
	if ( !svStaticReady ) {
		Session_Static( key, &svStatic );
		svStaticReady = qtrue;
	}
	return qtrue;
}

/*
=================
SV_SessionDropped

A handshake's message dropped without a word: every failure of the
cryptography, a rate limit and a full table look alike from outside, and
add to one count
=================
*/
void SV_SessionDropped( netadr_t from, const char *why ) {
	svSessionDrops++;
	Com_DPrintf( "%s: session dropped, %s (%d so far)\n", NET_AdrToString( from ), why, svSessionDrops );
}

static qboolean SV_PendingExpired( const svPending_t *p ) {
	return Sys_Milliseconds() - p->created >= SV_PENDING_LIFE;
}

static void SV_PendingFree( svPending_t *p ) {
	Noise_Wipe( &p->hs );
	Com_Memset( p, 0, sizeof( *p ) );
}

/*
=================
SV_SessionShutdown

The server stopping: every pending handshake's secrets wiped, and the
converted static, made again at the next start
=================
*/
void SV_SessionShutdown( void ) {
	int		i;

	for ( i = 0; i < SV_PENDING_MAX; i++ ) {
		SV_PendingFree( &svPending[i] );
	}
	Ident_Wipe( &svStatic, sizeof( svStatic ) );
	svStaticReady = qfalse;
}

// the live pending handshake for an address, port and qport, or NULL; one
// expired is freed on the way
static svPending_t *SV_PendingFind( netadr_t from, int qport ) {
	svPending_t	*p;
	int			i;

	for ( i = 0, p = svPending; i < SV_PENDING_MAX; i++, p++ ) {
		if ( !p->used ) {
			continue;
		}
		if ( SV_PendingExpired( p ) ) {
			SV_PendingFree( p );
			continue;
		}
		if ( p->terms.qport == qport && NET_CompareAdr( from, p->terms.from ) ) {
			return p;
		}
	}
	return NULL;
}

static svPending_t *SV_PendingSpace( void ) {
	svPending_t	*p;
	int			i;

	for ( i = 0, p = svPending; i < SV_PENDING_MAX; i++, p++ ) {
		if ( p->used && SV_PendingExpired( p ) ) {
			SV_PendingFree( p );
		}
		if ( !p->used ) {
			return p;
		}
	}
	return NULL;
}

/*
=================
SV_HandshakeAllowed

The buckets a handshake's DH spends from: one per IPv4 address or IPv6
/64, 5 a second with a burst of 10, so one network can't spend the rest;
and one for every address, 100 a second with a burst of 10
=================
*/
static qboolean SV_HandshakeAllowed( netadr_t from ) {
	static rateLimit_t	everyone;

	return !SVC_RateLimitHandshake( from ) && !SVC_RateLimit( &everyone, 10, 10 );
}

/*
=================
SV_SessionTestContent

A test build's content key and manifest for a session from one port, read
from the file OMNIFRAG_TEST_SESSION_CONTENT names: "<port> <ofk> <sha256>".
The fast download's tests put their stand-in, which serves its own
manifest, between client and server (tests/server/content_standin.py),
and it can't read what the session seals. qfalse in any other build
=================
*/
static qboolean SV_SessionTestContent( netadr_t from, uint64_t *ofk, byte manifest[32] ) {
	const char	*path = Com_TestEnv( "OMNIFRAG_TEST_SESSION_CONTENT" );
	char		key[OFK_DIGITS + 1], hash[SHA256_HEX_SIZE];
	int			port;
	qboolean	ok;
	FILE		*f;

	if ( !path || !( f = fopen( path, "r" ) ) ) {
		return qfalse;
	}
	ok = fscanf( f, "%d %16s %64s", &port, key, hash ) == 3 && port == (unsigned short)BigShort( from.port ) &&
		Com_ParseExtensionKey( key, ofk ) && Com_SHA256FromHex( hash, manifest );
	fclose( f );
	return ok;
}

/*
=================
SV_SessionConnect

A connect whose challenge agreed the session, with the client's
ephemeral (ofe), message 1: the checks a new player gets before any DH,
then message 2, or the one sent already when the connect is a resend
(14.3, 14.4)
=================
*/
void SV_SessionConnect( const svAdmit_t *terms, const byte clientEphemeral[NOISE_DHLEN] ) {
	netadr_t		from = terms->from;
	svPending_t		*p;
	const char		*refusal;
	svRefusal_t		refused;
	byte			e[NOISE_DHLEN], prologue[SESSION_PROLOGUE_MAX], payload[SESSION_SERVER_MAX];
	byte			testManifest[32];
	byte			empty[1];
	int				prologueLength, payloadLength, written;
	uint64_t		ofk;
	const byte		*manifest = NULL;
	const char		*line;
	msg_t			reply;

	if ( !SV_SessionReady() ) {
		SV_SessionDropped( from, "no key for the session" );
		return;
	}
	refusal = SV_NewPlayerRefusal( from, terms->qport, &refused );
	if ( refusal ) {
		SV_RefuseConnect( from, "%s\n", refusal );
		return;
	}

	// a resend with the same ephemeral gets the same answer; another
	// ephemeral, a client that started again, replaces it
	p = SV_PendingFind( from, terms->qport );
	if ( p ) {
		if ( p->terms.challenge == terms->challenge && !memcmp( p->hs.re, clientEphemeral, NOISE_DHLEN ) ) {
			NET_SendPacket( NS_SERVER, p->replyLength, p->reply, from );
			return;
		}
		SV_PendingFree( p );
	}
	if ( !SV_HandshakeAllowed( from ) ) {
		SV_SessionDropped( from, "past the handshakes' rate" );
		return;
	}
	p = SV_PendingSpace();
	if ( !p ) {
		SV_SessionDropped( from, "the pending handshakes are full" );
		return;
	}
	if ( !Com_RandomSecret( e, sizeof( e ) ) || !Com_RandomSecret( (byte *)&ofk, sizeof( ofk ) ) ) {
		SV_SessionDropped( from, "no randomness" );
		return;
	}

	prologueLength = Session_Prologue( prologue, com_protocol->integer, com_gamename->string, terms->challenge,
		terms->ofx, terms->fragmentSize, terms->qport );
	Noise_Init( &p->hs, qfalse, prologue, prologueLength, &svStatic, e );
	Ident_Wipe( e, sizeof( e ) );
	// message 1: the client's ephemeral, its payload empty
	if ( !prologueLength || Noise_ReadMessage( &p->hs, clientEphemeral, NOISE_DHLEN, empty, 0 ) != 0 ) {
		SV_PendingFree( p );
		SV_SessionDropped( from, "a first message that isn't one" );
		return;
	}

	// message 2: this server's key, the connection's content key, and the
	// content manifest it checks its paks against
	p->terms = *terms;
	p->terms.contentKey = ofk ? ofk : 1;
	if ( terms->ofx & OFX_CONTENT ) {
		manifest = SV_ContentManifestHash();
		if ( SV_SessionTestContent( from, &p->terms.contentKey, testManifest ) ) {
			manifest = testManifest;
		}
	}
	payloadLength = Session_WriteServerPayload( payload, Com_ServerKey()->pub, p->terms.contentKey, manifest );
	MSG_InitOOB( &reply, p->reply, sizeof( p->reply ) );
	MSG_WriteLong( &reply, -1 );
	line = va( "connectResponse %d\n", terms->challenge );
	MSG_WriteData( &reply, line, (int)strlen( line ) );
	MSG_WriteByte( &reply, SESSION_VERSION );
	written = Noise_WriteMessage( &p->hs, payload, payloadLength, reply.data + reply.cursize,
		reply.maxsize - reply.cursize );
	if ( written < 0 ) {
		SV_PendingFree( p );
		SV_SessionDropped( from, "message 2 couldn't be written" );
		return;
	}
	p->replyLength = reply.cursize + written;
	p->used = qtrue;
	p->created = Sys_Milliseconds();
	NET_SendPacket( NS_SERVER, p->replyLength, p->reply, from );
}

/*
=================
SV_SessionUserinfoClear

A sealed userinfo held to what a classic connect's can be: no line break,
which would end that line, and in games.log, which Omniwire reads, start
one of its own, nor a quote, which would end its argument; qfalse for
one; and '%' and bytes past 127 made '.', as the classic readers make them
(MSG_ReadStringLine), lest a name reach a format
=================
*/
static qboolean SV_SessionUserinfoClear( char *userinfo ) {
	char	*c;

	for ( c = userinfo; *c; c++ ) {
		if ( *c == '\n' || *c == '\r' || *c == '"' ) {
			return qfalse;
		}
		if ( *c == '%' || (unsigned char)*c > 127 ) {
			*c = '.';
		}
	}
	return qtrue;
}

/*
=================
SV_SessionUserinfo

A session's userinfo: the sealed one's pairs, but for the keys the
connect's clear ones set, then those; qfalse if it doesn't fit
=================
*/
static qboolean SV_SessionUserinfo( const char *sealed, const char *clear, char *out, int size ) {
	static const char	*const cleared[] = { "protocol", "qport", "challenge", "ip" };
	char				key[BIG_INFO_KEY], value[BIG_INFO_VALUE];
	const char			*s = sealed;
	int					i, length = 0;

	out[0] = '\0';
	while ( *s ) {
		Info_NextPair( &s, key, value );
		if ( !key[0] ) {
			break;
		}
		for ( i = 0; i < ARRAY_LEN( cleared ) && Q_stricmp( key, cleared[i] ); i++ ) {
		}
		if ( i < ARRAY_LEN( cleared ) ) {
			continue;
		}
		if ( length + 2 + (int)( strlen( key ) + strlen( value ) ) >= size ) {
			Ident_Wipe( value, sizeof( value ) );
			return qfalse;
		}
		length += Com_sprintf( out + length, size - length, "\\%s\\%s", key, value );
	}
	Ident_Wipe( value, sizeof( value ) );
	if ( length + (int)strlen( clear ) >= size ) {
		return qfalse;
	}
	Q_strcat( out, size, clear );
	return qtrue;
}

/*
=================
SV_SessionFinish

The third message, "session <challenge> <qport>", then the client's
static and its payload: its device's key and the userinfo. Without a live
pending handshake at its address and qport for that challenge it's
dropped, so a replay after the slot is made does nothing; one that doesn't
open leaves the handshake as it was (14.3). One that opens makes the slot
=================
*/
void SV_SessionFinish( netadr_t from, msg_t *msg ) {
	static svAdmit_t	admit;
	svPending_t			*p;
	const byte			*blob = msg->data + msg->readcount;
	int					length = msg->cursize - msg->readcount, read, qport, challenge;
	byte				payload[SESSION_CLIENT_MAX], key[IDENT_KEY_SIZE];
	char				userinfo[SESSION_USERINFO_MAX + 1];
	const char			*clear;

	if ( SVC_RateLimitAddress( from, 10, 1000 ) ) {
		SV_SessionDropped( from, "past the session messages' rate" );
		return;
	}
	challenge = atoi( Cmd_Argv( 1 ) );
	qport = atoi( Cmd_Argv( 2 ) );
	p = SV_PendingFind( from, qport );
	if ( !p || p->terms.challenge != challenge || length < 1 || blob[0] != SESSION_VERSION ) {
		SV_SessionDropped( from, "a third message with no handshake waiting for it" );
		return;
	}
	read = Noise_ReadMessage( &p->hs, blob + 1, length - 1, payload, sizeof( payload ) );
	if ( read < 0 ) {
		SV_SessionDropped( from, "a third message that doesn't open" );
		return;
	}
	// it opened, so it's the client's: a payload that isn't one, or a key
	// that isn't its static, ends the handshake
	if ( !Session_ReadClientPayload( payload, read, key, userinfo ) ||
		!Session_KeyIsStatic( key, Noise_RemoteStatic( &p->hs ) ) ) {
		SV_PendingFree( p );
		SV_SessionDropped( from, "a device key that isn't the one the handshake proved" );
		return;
	}

	admit = p->terms;
	Noise_Split( &p->hs, admit.receiveKey, admit.sendKey );
	SV_PendingFree( p );
	admit.identityProven = qtrue;
	Com_Memcpy( admit.identity, key, IDENT_KEY_SIZE );
	NET_PacketDestination( &admit.to );

	// the clear keys are the connect's, whatever the sealed userinfo says
	clear = va( "\\protocol\\%i\\qport\\%i\\challenge\\%i\\ip\\%s", com_protocol->integer, admit.qport,
		admit.challenge, NET_AdrToString( from ) );
	// pair by pair, so a key given twice is left out each time, and an odd
	// one out can't shift how the clear ones read
	if ( !SV_SessionUserinfoClear( userinfo ) ) {
		SV_RefuseConnect( from, "Your userinfo holds a character a connect can't carry.\n" );
	} else if ( !SV_SessionUserinfo( userinfo, clear, admit.userinfo, sizeof( admit.userinfo ) ) ) {
		SV_RefuseConnect( from, "Userinfo string length exceeded.  Try removing setu cvars from your config.\n" );
	} else {
		SV_AdmitClient( &admit );
	}
	// the keys, and the userinfo with its password, once the slot has them
	Ident_Wipe( &admit, sizeof( admit ) );
	Ident_Wipe( userinfo, sizeof( userinfo ) );
}
