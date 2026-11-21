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
// noise.h -- the Noise Protocol Framework's XX handshake, as one protocol
// name alone, Noise_XX_25519_ChaChaPoly_SHA512 (the Noise specification,
// revision 34), on Monocypher: its SymmetricState and HandshakeState, the
// three messages of XX, and Split, with ChaChaPoly under an explicit nonce
// for the transport. Nothing calls it yet; the handshake that will is
// OFX_SESSION (the handshake design, section 14).
//
//   XX:
//     -> e
//     <- e, ee, s, es
//     -> s, se
//
// Each side's static is its Ed25519 key converted (Ident_X25519Pair). The
// caller gives the ephemeral secret, which a test fixes and the handshake
// will draw from the system's randomness; nothing here draws randomness.
// Like identity.c, it uses no globals and calls only q_shared.c's and
// Monocypher's, so a unit test or a fuzz harness links it alone.

#ifndef NOISE_H
#define NOISE_H

#define NOISE_PROTOCOL_NAME		"Noise_XX_25519_ChaChaPoly_SHA512"
#define NOISE_DHLEN				32		// X25519's keys and shared secrets
#define NOISE_HASHLEN			64		// SHA-512's
#define NOISE_KEYLEN			32		// ChaChaPoly's
#define NOISE_TAGLEN			16		// Poly1305's
#define NOISE_MAX_MESSAGE		65535	// the specification's bound on a message
#define NOISE_DONE				3		// the message after the last: Split may run
#define NOISE_FAILED			4		// a step failed: nothing further runs
#define NOISE_SPLIT				5		// Split ran

// A static key pair, made once when its key loads, which handshakes point at
typedef struct {
	byte	secret[NOISE_DHLEN];
	byte	pub[NOISE_DHLEN];
} noiseStatic_t;

// A CipherState: a key, or none, and the next nonce. Only the handshake
// counts with it; the transport numbers its own nonces
typedef struct {
	byte		k[NOISE_KEYLEN];
	uint64_t	n;
	qboolean	hasKey;
} noiseCipher_t;

// A SymmetricState: the CipherState, the chaining key and the hash
typedef struct {
	noiseCipher_t	cipher;
	byte			ck[NOISE_HASHLEN];
	byte			h[NOISE_HASHLEN];
} noiseSymmetric_t;

// A HandshakeState for XX, as either side
typedef struct {
	noiseSymmetric_t	sym;
	qboolean			initiator;
	const noiseStatic_t	*s;						// ours, which outlives the handshake
	byte				e[NOISE_DHLEN];			// our ephemeral's secret
	byte				rs[NOISE_DHLEN], re[NOISE_DHLEN];	// theirs
	int					message;	// the next message, 0 to 2, then NOISE_DONE, NOISE_SPLIT or NOISE_FAILED
} noiseHandshake_t;

// ChaChaPoly, RFC 8439's AEAD, as Monocypher's IETF context gives it with
// one write on a fresh context: the nonce 32 zero bits, then n, least
// significant first; the tag after the text. qfalse for the reserved nonce
// 2^64 - 1, and for a tag that doesn't hold, which writes nothing
qboolean	Noise_Seal( const byte k[NOISE_KEYLEN], uint64_t n, const void *ad, int adLength,
				const void *text, int length, byte *out );
qboolean	Noise_Open( const byte k[NOISE_KEYLEN], uint64_t n, const void *ad, int adLength,
				const byte *sealed, int sealedLength, byte *out );
// A netchan datagram's n (the handshake design, 14.6): the sequence word as
// sent, its fragment flag in bit 31 included, above the fragment start, 0
// unfragmented. The word is ioq3's signed int, taken as its 32 bits, never
// sign-extended
uint64_t	Noise_DatagramNonce( int sequenceWord, int fragmentStart );

// The HandshakeState for either side, with our static, which it points at
// and which must outlive it, and our ephemeral's secret, which it copies;
// failed at once for a negative prologue length. The caller wipes its own
// copy of the ephemeral, and the static when its key goes
void		Noise_Init( noiseHandshake_t *hs, qboolean initiator, const void *prologue, int prologueLength,
				const noiseStatic_t *s, const byte e[NOISE_DHLEN] );
// The next message, written into out (at most size) with payload: its
// length, or -1 for one that doesn't fit, isn't this side's to write, or
// meets an all-zero shared secret, after which the handshake has failed
int			Noise_WriteMessage( noiseHandshake_t *hs, const void *payload, int payloadLength, byte *out, int size );
// The next message read from in into payload (at most size, never
// overlapping in): its length, or -1 for one that isn't this side's to
// read, doesn't fit or doesn't open, or meets an all-zero shared secret,
// after which the handshake is as it was before, so a forged message
// changes nothing
int			Noise_ReadMessage( noiseHandshake_t *hs, const byte *in, int length, byte *payload, int size );
// A handshake given up or expired: everything in it wiped, and failed
void		Noise_Wipe( noiseHandshake_t *hs );
// The three messages done: the transport's two keys, the initiator's to
// send first (client to server), and the handshake's secrets wiped; qfalse
// before then, or after a first Split. The hash h stays in hs->sym
qboolean	Noise_Split( noiseHandshake_t *hs, byte c2s[NOISE_KEYLEN], byte s2c[NOISE_KEYLEN] );
// the other side's static, once the message carrying it is read
const byte	*Noise_RemoteStatic( const noiseHandshake_t *hs );

#endif
