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
// noise.c -- Noise_XX_25519_ChaChaPoly_SHA512 (noise.h), each step named as
// the Noise specification, revision 34, names it

#include "q_shared.h"
#include "noise.h"
#include "../thirdparty/monocypher-4.0.2/monocypher.h"
#include "../thirdparty/monocypher-4.0.2/monocypher-ed25519.h"

/*
=================
ChaChaPoly

RFC 8439's AEAD_CHACHA20_POLY1305 with Noise's nonce: 32 zero bits, then n,
64 bits least significant first. Monocypher's IETF context sets its djb
counter to the nonce's first 4 bytes shifted up 32, with the other 8 as the
djb nonce, so its first write is RFC 8439's; each later write rekeys, so
each message gets a fresh context. crypto_aead_read checks the tag before
it writes anything
=================
*/
static void Noise_Context( crypto_aead_ctx *ctx, const byte k[NOISE_KEYLEN], uint64_t n ) {
	byte	nonce[12];
	int		i;

	Com_Memset( nonce, 0, 4 );
	for ( i = 0; i < 8; i++ ) {
		nonce[4 + i] = (byte)( n >> ( i * 8 ) );
	}
	crypto_aead_init_ietf( ctx, k, nonce );
}

qboolean Noise_Seal( const byte k[NOISE_KEYLEN], uint64_t n, const void *ad, int adLength,
	const void *text, int length, byte *out ) {
	crypto_aead_ctx	ctx;

	// 2^64 - 1 is reserved (the specification's section 5.1)
	if ( n == UINT64_MAX || length < 0 || adLength < 0 ) {
		return qfalse;
	}
	Noise_Context( &ctx, k, n );
	crypto_aead_write( &ctx, out, out + length, ad, (size_t)adLength, text, (size_t)length );
	crypto_wipe( &ctx, sizeof( ctx ) );
	return qtrue;
}

qboolean Noise_Open( const byte k[NOISE_KEYLEN], uint64_t n, const void *ad, int adLength,
	const byte *sealed, int sealedLength, byte *out ) {
	crypto_aead_ctx	ctx;
	int				length = sealedLength - NOISE_TAGLEN, mismatch;

	if ( n == UINT64_MAX || length < 0 || adLength < 0 ) {
		return qfalse;
	}
	Noise_Context( &ctx, k, n );
	mismatch = crypto_aead_read( &ctx, out, sealed + length, ad, (size_t)adLength, sealed, (size_t)length );
	crypto_wipe( &ctx, sizeof( ctx ) );
	return !mismatch;
}

uint64_t Noise_DatagramNonce( int sequenceWord, int fragmentStart ) {
	return (uint64_t)(uint32_t)sequenceWord << 16 | (uint64_t)(uint16_t)fragmentStart;
}

/*
=================
CipherState: InitializeKey, EncryptWithAd, DecryptWithAd

Each is the length written, or -1, after which the cipher stays as it was;
the text as it is while there's no key
=================
*/
static void Noise_InitializeKey( noiseCipher_t *c, const byte *k ) {
	if ( k ) {
		Com_Memcpy( c->k, k, NOISE_KEYLEN );
		c->hasKey = qtrue;
	} else {
		crypto_wipe( c->k, NOISE_KEYLEN );
		c->hasKey = qfalse;
	}
	c->n = 0;
}

static int Noise_Encrypt( noiseCipher_t *c, const void *ad, int adLength, const void *text, int length, byte *out ) {
	if ( !c->hasKey ) {
		if ( length > 0 ) {
			memmove( out, text, (size_t)length );
		}
		return length;
	}
	if ( !Noise_Seal( c->k, c->n, ad, adLength, text, length, out ) ) {
		return -1;
	}
	c->n++;
	return length + NOISE_TAGLEN;
}

static int Noise_Decrypt( noiseCipher_t *c, const void *ad, int adLength, const byte *sealed, int length, byte *out ) {
	if ( !c->hasKey ) {
		if ( length > 0 ) {
			memmove( out, sealed, (size_t)length );
		}
		return length;
	}
	if ( !Noise_Open( c->k, c->n, ad, adLength, sealed, length, out ) ) {
		return -1;
	}
	c->n++;
	return length - NOISE_TAGLEN;
}

/*
=================
HKDF

The specification's HKDF( chaining_key, input_key_material, 2 ): HMAC-SHA-512
under the chaining key, then two outputs from it. It's RFC 5869's, as
Monocypher's crypto_sha512_hkdf is, but that one leaves its pseudorandom key
and last block, the next chaining key's and cipher key's material, on the
stack; this wipes them
=================
*/
static void Noise_HKDF( const byte ck[NOISE_HASHLEN], const byte *ikm, int ikmLength,
	byte out1[NOISE_HASHLEN], byte out2[NOISE_HASHLEN] ) {
	byte	tempKey[NOISE_HASHLEN], input[NOISE_HASHLEN + 1];

	crypto_sha512_hmac( tempKey, ck, NOISE_HASHLEN, ikm, (size_t)ikmLength );
	input[0] = 0x01;
	crypto_sha512_hmac( out1, tempKey, NOISE_HASHLEN, input, 1 );
	Com_Memcpy( input, out1, NOISE_HASHLEN );
	input[NOISE_HASHLEN] = 0x02;
	crypto_sha512_hmac( out2, tempKey, NOISE_HASHLEN, input, sizeof( input ) );
	crypto_wipe( tempKey, sizeof( tempKey ) );
	crypto_wipe( input, sizeof( input ) );
}

/*
=================
SymmetricState: InitializeSymmetric, MixKey, MixHash, EncryptAndHash,
DecryptAndHash
=================
*/
static void Noise_InitializeSymmetric( noiseSymmetric_t *sym ) {
	size_t	length = strlen( NOISE_PROTOCOL_NAME );

	// a name of HASHLEN bytes or fewer, padded with zeros, as this one is
	Com_Memset( sym->h, 0, NOISE_HASHLEN );
	Com_Memcpy( sym->h, NOISE_PROTOCOL_NAME, length );
	Com_Memcpy( sym->ck, sym->h, NOISE_HASHLEN );
	Noise_InitializeKey( &sym->cipher, NULL );
}

static void Noise_MixHash( noiseSymmetric_t *sym, const void *data, int length ) {
	crypto_sha512_ctx	ctx;

	crypto_sha512_init( &ctx );
	crypto_sha512_update( &ctx, sym->h, NOISE_HASHLEN );
	crypto_sha512_update( &ctx, data, (size_t)length );
	crypto_sha512_final( &ctx, sym->h );
}

static void Noise_MixKey( noiseSymmetric_t *sym, const byte *ikm, int length ) {
	byte	out2[NOISE_HASHLEN];

	Noise_HKDF( sym->ck, ikm, length, sym->ck, out2 );
	// HASHLEN 64: the key is the output's first 32 bytes
	Noise_InitializeKey( &sym->cipher, out2 );
	crypto_wipe( out2, sizeof( out2 ) );
}

static int Noise_EncryptAndHash( noiseSymmetric_t *sym, const void *text, int length, byte *out ) {
	int		written = Noise_Encrypt( &sym->cipher, sym->h, NOISE_HASHLEN, text, length, out );

	if ( written >= 0 ) {
		Noise_MixHash( sym, out, written );
	}
	return written;
}

// out must not overlap sealed, which is hashed after it's written
static int Noise_DecryptAndHash( noiseSymmetric_t *sym, const byte *sealed, int length, byte *out ) {
	int		read = Noise_Decrypt( &sym->cipher, sym->h, NOISE_HASHLEN, sealed, length, out );

	if ( read >= 0 ) {
		Noise_MixHash( sym, sealed, length );
	}
	return read;
}

/*
=================
DH

X25519, its shared secret mixed into the key, with an all-zero one, which a
low-order key gives, refused
=================
*/
static qboolean Noise_DH( noiseSymmetric_t *sym, const byte secret[NOISE_DHLEN], const byte theirs[NOISE_DHLEN] ) {
	static const byte	zero[NOISE_DHLEN];
	byte				shared[NOISE_DHLEN];
	qboolean			ok;

	crypto_x25519( shared, secret, theirs );
	ok = crypto_verify32( shared, zero ) != 0;
	if ( ok ) {
		Noise_MixKey( sym, shared, NOISE_DHLEN );
	}
	crypto_wipe( shared, sizeof( shared ) );
	return ok;
}

/*
=================
HandshakeState: Initialize
=================
*/
void Noise_Init( noiseHandshake_t *hs, qboolean initiator, const void *prologue, int prologueLength,
	const noiseStatic_t *s, const byte e[NOISE_DHLEN] ) {
	Com_Memset( hs, 0, sizeof( *hs ) );
	hs->initiator = initiator ? qtrue : qfalse;
	if ( prologueLength < 0 ) {
		hs->message = NOISE_FAILED;
		return;
	}
	Noise_InitializeSymmetric( &hs->sym );
	Noise_MixHash( &hs->sym, prologue, prologueLength );
	hs->s = s;
	Com_Memcpy( hs->e, e, NOISE_DHLEN );
}

// the secrets go, as Split's do: nothing further runs
static int Noise_Fail( noiseHandshake_t *hs ) {
	crypto_wipe( hs->e, sizeof( hs->e ) );
	crypto_wipe( hs->sym.ck, sizeof( hs->sym.ck ) );
	crypto_wipe( &hs->sym.cipher, sizeof( hs->sym.cipher ) );
	hs->message = NOISE_FAILED;
	return -1;
}

// what message m adds to its payload: an ephemeral in the first two, a
// sealed static in the last two, and the payload's tag once there's a key
static int Noise_Overhead( int message ) {
	return ( message < 2 ? NOISE_DHLEN : 0 ) + ( message > 0 ? NOISE_DHLEN + NOISE_TAGLEN * 2 : 0 );
}

/*
=================
WriteMessage, ReadMessage

The initiator writes messages 0 (-> e) and 2 (-> s, se) and reads message 1
(<- e, ee, s, es); the responder the other way about. es is the initiator's
ephemeral with the responder's static, se the initiator's static with the
responder's ephemeral
=================
*/
int Noise_WriteMessage( noiseHandshake_t *hs, const void *payload, int payloadLength, byte *out, int size ) {
	int		length = 0, sealed, m = hs->message;

	if ( m > 2 || hs->initiator != ( m != 1 ) || payloadLength < 0 ||
		payloadLength > NOISE_MAX_MESSAGE - Noise_Overhead( m ) || payloadLength > size - Noise_Overhead( m ) ) {
		return Noise_Fail( hs );
	}
	if ( m < 2 ) {
		// e
		crypto_x25519_public_key( out, hs->e );
		Noise_MixHash( &hs->sym, out, NOISE_DHLEN );
		length = NOISE_DHLEN;
	}
	if ( m == 1 && !Noise_DH( &hs->sym, hs->e, hs->re ) ) {
		// ee
		return Noise_Fail( hs );
	}
	if ( m > 0 ) {
		// s, then es as the responder or se as the initiator: our static,
		// their ephemeral
		sealed = Noise_EncryptAndHash( &hs->sym, hs->s->pub, NOISE_DHLEN, out + length );
		if ( sealed < 0 || !Noise_DH( &hs->sym, hs->s->secret, hs->re ) ) {
			return Noise_Fail( hs );
		}
		length += sealed;
	}
	sealed = Noise_EncryptAndHash( &hs->sym, payload, payloadLength, out + length );
	if ( sealed < 0 ) {
		return Noise_Fail( hs );
	}
	hs->message++;
	return length + sealed;
}

// a message this side may read, of a length that fits, read into hs, which
// is a copy: a failure needs no wipe here
static int Noise_Read( noiseHandshake_t *hs, const byte *in, int length, byte *payload ) {
	int		at = 0, read, m = hs->message;
	if ( m < 2 ) {
		// e
		Com_Memcpy( hs->re, in, NOISE_DHLEN );
		Noise_MixHash( &hs->sym, hs->re, NOISE_DHLEN );
		at = NOISE_DHLEN;
	}
	if ( m == 1 && !Noise_DH( &hs->sym, hs->e, hs->re ) ) {
		// ee
		return -1;
	}
	if ( m > 0 ) {
		// s, then es as the initiator or se as the responder: our
		// ephemeral, their static
		if ( Noise_DecryptAndHash( &hs->sym, in + at, NOISE_DHLEN + NOISE_TAGLEN, hs->rs ) != NOISE_DHLEN ||
			!Noise_DH( &hs->sym, hs->e, hs->rs ) ) {
			return -1;
		}
		at += NOISE_DHLEN + NOISE_TAGLEN;
	}
	read = Noise_DecryptAndHash( &hs->sym, in + at, length - at, payload );
	if ( read < 0 ) {
		return -1;
	}
	hs->message++;
	return read;
}

// read on a copy, kept only when it opens, so a forged message leaves the
// handshake as it was; one out of turn or of the wrong length isn't copied
int Noise_ReadMessage( noiseHandshake_t *hs, const byte *in, int length, byte *payload, int size ) {
	noiseHandshake_t	copy;
	int					read, m = hs->message, overhead = Noise_Overhead( m );

	if ( m > 2 || hs->initiator == ( m != 1 ) || length < overhead || length > NOISE_MAX_MESSAGE ||
		length - overhead > size ) {
		return -1;
	}
	copy = *hs;
	read = Noise_Read( &copy, in, length, payload );
	if ( read >= 0 ) {
		*hs = copy;
	}
	crypto_wipe( &copy, sizeof( copy ) );
	return read;
}

void Noise_Wipe( noiseHandshake_t *hs ) {
	crypto_wipe( hs, sizeof( *hs ) );
	hs->message = NOISE_FAILED;
}

/*
=================
Split
=================
*/
qboolean Noise_Split( noiseHandshake_t *hs, byte c2s[NOISE_KEYLEN], byte s2c[NOISE_KEYLEN] ) {
	static const byte	empty[1];
	byte				k1[NOISE_HASHLEN], k2[NOISE_HASHLEN];

	if ( hs->message != NOISE_DONE ) {
		return qfalse;
	}
	Noise_HKDF( hs->sym.ck, empty, 0, k1, k2 );
	// HASHLEN 64: each key is its output's first 32 bytes, the first the
	// initiator's to send with
	Com_Memcpy( c2s, k1, NOISE_KEYLEN );
	Com_Memcpy( s2c, k2, NOISE_KEYLEN );
	crypto_wipe( k1, sizeof( k1 ) );
	crypto_wipe( k2, sizeof( k2 ) );
	// what's left that's secret: the ephemeral, the chaining key, the cipher
	crypto_wipe( hs->e, sizeof( hs->e ) );
	crypto_wipe( hs->sym.ck, sizeof( hs->sym.ck ) );
	crypto_wipe( &hs->sym.cipher, sizeof( hs->sym.cipher ) );
	hs->message = NOISE_SPLIT;
	return qtrue;
}

const byte *Noise_RemoteStatic( const noiseHandshake_t *hs ) {
	return hs->rs;
}
