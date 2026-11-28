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
// ofd_seal.h -- sealed OFD datagrams (version 2) over an OFX_SESSION
// connection (docs/design/download-protocol.md): per-direction keys derived
// from the session's Noise transport keys, a 24-byte authenticated outer
// header (magic + version 2 + 64-bit connection id + 64-bit monotonic nonce),
// ChaCha20-Poly1305 over the inner message type/flags/length/body, and an
// RFC 6479 sliding replay window. Pure: no globals, no allocation, so unit
// tests and fuzzers link it with ofd.c, noise.c and Monocypher alone.

#ifndef OFD_SEAL_H
#define OFD_SEAL_H

#include "ofd.h"
#include "noise.h"

#define OFD_VERSION_SEALED		2
// 0xFFFFFFFF (4) + "OFD" (3) + version (1) + cid (8) + nonce (8)
#define OFD_SEALED_HEADER		24
// inner type (1) + flags (1) + body_length (2) encrypted before the body
#define OFD_SEALED_INNER		4
// bytes added over a v1 OFD datagram: (24 - 20) + 4 + 16 = 24
#define OFD_SEAL_OVERHEAD		( OFD_SEALED_HEADER - OFD_HEADER_SIZE + OFD_SEALED_INNER + NOISE_TAGLEN )
// smallest well-formed sealed envelope: outer header + inner header + tag
#define OFD_SEALED_MIN			( OFD_SEALED_HEADER + OFD_SEALED_INNER + NOISE_TAGLEN )
// largest sealed datagram (1184 + 24 = 1208 <= OFD_MAX_LARGEST 1232;
// web 960-byte chunk is 992 + 24 = 1016 <= NET_WEB_FRAGMENT_SIZE 1024)
#define OFD_MAX_SEALED_DATAGRAM	( OFD_MAX_DATAGRAM + OFD_SEAL_OVERHEAD )
// send nonce cap, well below Noise's reserved UINT64_MAX
#define OFD_SEALED_CAP			( (uint64_t)1 << 60 )

// RFC 6479 sliding replay window: 64 * 64-bit words = 4,032 usable window,
// matching SVT_RING / CLC_RECEIVED (4,096) for high-BDP reordering
#define OFD_REPLAY_WORDS		64
#define OFD_REPLAY_WINDOW		( ( OFD_REPLAY_WORDS - 1 ) * 64 )

typedef struct {
	uint64_t	maxNonce;
	uint64_t	bits[OFD_REPLAY_WORDS];
} ofdReplay_t;

// Per-connection sealed OFD state: keys, send nonce counter, and replay window.
typedef struct {
	qboolean	sealed;
	byte		sendKey[NOISE_KEYLEN];
	byte		receiveKey[NOISE_KEYLEN];
	uint64_t	sendNonce;
	ofdReplay_t	replay;
} ofdSession_t;

// Derives per-direction OFD transport keys from Noise_Split's c2s and s2c
// keys using distinct HMAC-SHA-512 domain labels so OFD nonces never share
// a key with netchan.
void		OFD_DeriveKeys( const byte netC2s[NOISE_KEYLEN], const byte netS2c[NOISE_KEYLEN],
				byte ofdC2s[NOISE_KEYLEN], byte ofdS2c[NOISE_KEYLEN] );

// Initializes, seals/opens through, and wipes an ofdSession_t.
// isServer: qtrue assigns s2c as sendKey and c2s as receiveKey; qfalse the reverse.
void		OFD_SessionInit( ofdSession_t *s, const byte netC2s[NOISE_KEYLEN],
				const byte netS2c[NOISE_KEYLEN], qboolean isServer );
void		OFD_SessionWipe( ofdSession_t *s );
int			OFD_SessionSeal( ofdSession_t *s, const byte *plain, int plainLen, byte *out, int size );
int			OFD_SessionOpen( ofdSession_t *s, const byte *data, int length, byte *plainOut, int plainSize );

// Whether a packet starts with the sealed OFD magic/version and meets the
// minimum and maximum sealed datagram sizes.
qboolean	OFD_IsSealed( const byte *data, int length );

// The 64-bit connection id (ofk) and 64-bit nonce in a header OFD_IsSealed
// accepted.
uint64_t	OFD_ReadSealedCid( const byte *data );
uint64_t	OFD_ReadSealedNonce( const byte *data );

// Two-phase sliding replay window: OFD_ReplayCheck runs before AEAD open
// without mutating state; OFD_ReplayAccept runs only after authentication
// succeeds so forged packets cannot advance the window.
qboolean	OFD_ReplayCheck( const ofdReplay_t *r, uint64_t nonce );
void		OFD_ReplayAccept( ofdReplay_t *r, uint64_t nonce );

// Seals a v1 OFD datagram (plain, plainLen) into a v2 sealed datagram in
// out (at most size; OFD_MAX_SEALED_DATAGRAM always suffices). Returns the
// sealed length (plainLen + OFD_SEAL_OVERHEAD), or 0 on bad input or a
// nonce of 0 or >= OFD_SEALED_CAP.
int			OFD_SealDatagram( const byte *plain, int plainLen, const byte key[NOISE_KEYLEN],
				uint64_t nonce, byte *out, int size );

// Opens a v2 sealed datagram (data, length) into its v1 OFD form in
// plainOut (at most plainSize; OFD_MAX_DATAGRAM always suffices). Checks
// replay before decrypting and updates replay only when the tag holds.
// Returns plainLen (length - OFD_SEAL_OVERHEAD), or 0 on any failure.
int			OFD_OpenDatagram( const byte *data, int length, const byte key[NOISE_KEYLEN],
				ofdReplay_t *replay, byte *plainOut, int plainSize );

// Wipes a 32-byte OFD key in memory.
void		OFD_WipeKey( byte key[NOISE_KEYLEN] );

#endif
