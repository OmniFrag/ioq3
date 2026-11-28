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

You should have received a copy of the GNU General Public License along
with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// ofd_seal.c -- sealed OFD datagrams (ofd_seal.h)

#include "q_shared.h"
#include "ofd_seal.h"
#include "../thirdparty/monocypher-4.0.2/monocypher.h"
#include "../thirdparty/monocypher-4.0.2/monocypher-ed25519.h"

static const byte	ofdSealedMagic[8] = { 0xff, 0xff, 0xff, 0xff, 'O', 'F', 'D', OFD_VERSION_SEALED };
static const byte	ofdPlainMagic[8] = { 0xff, 0xff, 0xff, 0xff, 'O', 'F', 'D', OFD_VERSION };

static uint64_t Seal_Get64( const byte *p ) {
	uint32_t	lo = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	uint32_t	hi = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;

	return (uint64_t)lo | ( (uint64_t)hi << 32 );
}

static void Seal_Put64( byte *p, uint64_t v ) {
	int		i;

	for ( i = 0; i < 8; i++ ) {
		p[i] = (byte)( v >> ( i * 8 ) );
	}
}

void OFD_DeriveKeys( const byte netC2s[NOISE_KEYLEN], const byte netS2c[NOISE_KEYLEN],
	byte ofdC2s[NOISE_KEYLEN], byte ofdS2c[NOISE_KEYLEN] ) {
	static const char	c2sLabel[] = "OmniFrag ofd c2s v1";
	static const char	s2cLabel[] = "OmniFrag ofd s2c v1";
	byte				digest[NOISE_HASHLEN];

	crypto_sha512_hmac( digest, netC2s, NOISE_KEYLEN, (const byte *)c2sLabel, sizeof( c2sLabel ) - 1 );
	Com_Memcpy( ofdC2s, digest, NOISE_KEYLEN );
	crypto_sha512_hmac( digest, netS2c, NOISE_KEYLEN, (const byte *)s2cLabel, sizeof( s2cLabel ) - 1 );
	Com_Memcpy( ofdS2c, digest, NOISE_KEYLEN );
	crypto_wipe( digest, sizeof( digest ) );
}

void OFD_SessionInit( ofdSession_t *s, const byte netC2s[NOISE_KEYLEN],
	const byte netS2c[NOISE_KEYLEN], qboolean isServer ) {
	if ( isServer ) {
		OFD_DeriveKeys( netC2s, netS2c, s->receiveKey, s->sendKey );
	} else {
		OFD_DeriveKeys( netC2s, netS2c, s->sendKey, s->receiveKey );
	}
	s->sealed = qtrue;
	s->sendNonce = 0;
	Com_Memset( &s->replay, 0, sizeof( s->replay ) );
}

void OFD_SessionWipe( ofdSession_t *s ) {
	OFD_WipeKey( s->sendKey );
	OFD_WipeKey( s->receiveKey );
	s->sealed = qfalse;
	s->sendNonce = 0;
	Com_Memset( &s->replay, 0, sizeof( s->replay ) );
}

int OFD_SessionSeal( ofdSession_t *s, const byte *plain, int plainLen, byte *out, int size ) {
	if ( !s || !s->sealed || s->sendNonce + 1 >= OFD_SEALED_CAP ) {
		return 0;
	}
	s->sendNonce++;
	return OFD_SealDatagram( plain, plainLen, s->sendKey, s->sendNonce, out, size );
}

int OFD_SessionOpen( ofdSession_t *s, const byte *data, int length, byte *plainOut, int plainSize ) {
	if ( !s || !s->sealed ) {
		return 0;
	}
	return OFD_OpenDatagram( data, length, s->receiveKey, &s->replay, plainOut, plainSize );
}

qboolean OFD_IsSealed( const byte *data, int length ) {
	return length >= OFD_SEALED_MIN && length <= OFD_MAX_SEALED_DATAGRAM
		&& !memcmp( data, ofdSealedMagic, sizeof( ofdSealedMagic ) );
}

uint64_t OFD_ReadSealedCid( const byte *data ) {
	return Seal_Get64( data + 8 );
}

uint64_t OFD_ReadSealedNonce( const byte *data ) {
	return Seal_Get64( data + 16 );
}

qboolean OFD_ReplayCheck( const ofdReplay_t *r, uint64_t nonce ) {
	uint64_t	diff;

	if ( nonce == 0 || nonce >= OFD_SEALED_CAP ) {
		return qfalse;
	}
	if ( nonce > r->maxNonce ) {
		return qtrue;
	}
	diff = r->maxNonce - nonce;
	if ( diff >= OFD_REPLAY_WINDOW ) {
		return qfalse;
	}
	return ( r->bits[( nonce >> 6 ) & ( OFD_REPLAY_WORDS - 1 )] & ( (uint64_t)1 << ( nonce & 63 ) ) ) == 0;
}

void OFD_ReplayAccept( ofdReplay_t *r, uint64_t nonce ) {
	uint64_t	index = nonce >> 6;
	uint64_t	top = r->maxNonce >> 6;

	if ( nonce == 0 || nonce >= OFD_SEALED_CAP ) {
		return;
	}
	if ( nonce > r->maxNonce ) {
		uint64_t	delta = index - top;

		if ( delta >= OFD_REPLAY_WORDS ) {
			Com_Memset( r->bits, 0, sizeof( r->bits ) );
		} else {
			uint64_t	i;

			for ( i = 1; i <= delta; i++ ) {
				r->bits[( top + i ) & ( OFD_REPLAY_WORDS - 1 )] = 0;
			}
		}
		r->maxNonce = nonce;
	} else if ( r->maxNonce - nonce >= OFD_REPLAY_WINDOW ) {
		return;
	}
	r->bits[index & ( OFD_REPLAY_WORDS - 1 )] |= (uint64_t)1 << ( nonce & 63 );
}

int OFD_SealDatagram( const byte *plain, int plainLen, const byte key[NOISE_KEYLEN],
	uint64_t nonce, byte *out, int size ) {
	int		bodyLen, innerLen, totalLen;

	if ( !plain || !key || !out || plainLen < OFD_HEADER_SIZE || plainLen > OFD_MAX_DATAGRAM ||
		memcmp( plain, ofdPlainMagic, sizeof( ofdPlainMagic ) ) != 0 ||
		nonce == 0 || nonce >= OFD_SEALED_CAP ) {
		return 0;
	}
	bodyLen = plainLen - OFD_HEADER_SIZE;
	innerLen = OFD_SEALED_INNER + bodyLen;
	totalLen = OFD_SEALED_HEADER + innerLen + NOISE_TAGLEN;
	if ( size < totalLen ) {
		return 0;
	}

	// outer AD: magic + version 2 + cid (from v1 key at offset 12) + nonce
	Com_Memcpy( out, ofdSealedMagic, sizeof( ofdSealedMagic ) );
	Com_Memcpy( out + 8, plain + 12, 8 );
	Seal_Put64( out + 16, nonce );

	// inner plaintext: v1 type/flags/body_length (offset 8..11) + body (offset 20..)
	Com_Memcpy( out + OFD_SEALED_HEADER, plain + 8, OFD_SEALED_INNER );
	if ( bodyLen > 0 ) {
		Com_Memcpy( out + OFD_SEALED_HEADER + OFD_SEALED_INNER, plain + OFD_HEADER_SIZE, bodyLen );
	}
	if ( !Noise_Seal( key, nonce, out, OFD_SEALED_HEADER,
		out + OFD_SEALED_HEADER, innerLen, out + OFD_SEALED_HEADER ) ) {
		return 0;
	}
	return totalLen;
}

int OFD_OpenDatagram( const byte *data, int length, const byte key[NOISE_KEYLEN],
	ofdReplay_t *replay, byte *plainOut, int plainSize ) {
	byte		inner[OFD_SEALED_INNER + OFD_MAX_DATAGRAM - OFD_HEADER_SIZE];
	uint64_t	nonce;
	int			sealedLen, innerLen, bodyLen, plainLen;

	if ( !data || !key || !replay || !plainOut || !OFD_IsSealed( data, length ) ) {
		return 0;
	}
	sealedLen = length - OFD_SEALED_HEADER;
	innerLen = sealedLen - NOISE_TAGLEN;
	bodyLen = innerLen - OFD_SEALED_INNER;
	plainLen = OFD_HEADER_SIZE + bodyLen;
	if ( bodyLen < 0 || plainLen > OFD_MAX_DATAGRAM || plainSize < plainLen ) {
		return 0;
	}
	nonce = OFD_ReadSealedNonce( data );
	if ( !OFD_ReplayCheck( replay, nonce ) ) {
		return 0;
	}
	if ( !Noise_Open( key, nonce, data, OFD_SEALED_HEADER, data + OFD_SEALED_HEADER, sealedLen, inner ) ) {
		return 0;
	}
	OFD_ReplayAccept( replay, nonce );

	// reconstruct v1 OFD datagram for OFD_Read
	Com_Memcpy( plainOut, ofdPlainMagic, sizeof( ofdPlainMagic ) );
	Com_Memcpy( plainOut + 8, inner, OFD_SEALED_INNER );
	Com_Memcpy( plainOut + 12, data + 8, 8 );
	if ( bodyLen > 0 ) {
		Com_Memcpy( plainOut + OFD_HEADER_SIZE, inner + OFD_SEALED_INNER, bodyLen );
	}
	return plainLen;
}

void OFD_WipeKey( byte key[NOISE_KEYLEN] ) {
	crypto_wipe( key, NOISE_KEYLEN );
}
