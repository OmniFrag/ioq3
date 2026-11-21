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
// session.h -- what OFX_SESSION's handshake carries around Noise XX
// (noise.h): the prologue both sides build, messages 2's and 3's version byte,
// the server's and the client's payloads, and the checks of a peer's key
// (the handshake design, sections 14.1 and 14.2). It uses no globals, so a
// unit test or a fuzz harness links it alone.

#ifndef SESSION_H
#define SESSION_H

#include "identity.h"
#include "noise.h"

#define SESSION_VERSION			1		// the first byte of each message's binary part
#define SESSION_GAME_MAX		255		// the longest game name the prologue holds
#define SESSION_PROLOGUE_MAX	( 20 + 4 + 1 + SESSION_GAME_MAX + 4 + 4 + 2 + 2 )
#define SESSION_USERINFO_MAX	( MAX_INFO_STRING - 1 )

// the server's payload (in message 2): its Ed25519 key, the connection's
// content key (ofk), flags, and with SESSION_MANIFEST the content
// manifest's SHA-256
#define SESSION_MANIFEST		0x01
#define SESSION_SERVER_MAX		( IDENT_KEY_SIZE + 8 + 1 + 32 )
// the client's (in message 3): its device's Ed25519 key, then the userinfo
// with its 2-byte length
#define SESSION_CLIENT_MAX		( IDENT_KEY_SIZE + 2 + SESSION_USERINFO_MAX )

// the binary parts of messages 2 and 3, after their lines: the version
// byte with the Noise message, the most each can be. Message 1, the
// client's ephemeral, is the connect's ofe token, as the connect's line is
// Huffman-coded
#define SESSION_REPLY_MAX		( 1 + NOISE_DHLEN + NOISE_DHLEN + NOISE_TAGLEN + SESSION_SERVER_MAX + NOISE_TAGLEN )
#define SESSION_FINISH_MAX		( 1 + NOISE_DHLEN + NOISE_TAGLEN + SESSION_CLIENT_MAX + NOISE_TAGLEN )

// The prologue, 14.1's: the label, the protocol, the game, the challenge,
// the ofx agreed, the fragment size agreed (0 for none) and the qport,
// little-endian. Its length, or 0 for a game name longer than
// SESSION_GAME_MAX
int			Session_Prologue( byte out[SESSION_PROLOGUE_MAX], int protocol, const char *game, int challenge,
				uint32_t ofx, int fragmentSize, int qport );

// The payloads: each writer's length; each reader qfalse for one that
// isn't exactly its layout (a flag it doesn't know, a length that doesn't
// match, a userinfo with a NUL in it or too long)
int			Session_WriteServerPayload( byte out[SESSION_SERVER_MAX], const byte key[IDENT_KEY_SIZE], uint64_t ofk,
				const byte *manifest );
qboolean	Session_ReadServerPayload( const byte *in, int length, byte key[IDENT_KEY_SIZE], uint64_t *ofk,
				byte manifest[32], qboolean *hasManifest );
int			Session_WriteClientPayload( byte out[SESSION_CLIENT_MAX], const byte key[IDENT_KEY_SIZE], const char *userinfo );
qboolean	Session_ReadClientPayload( const byte *in, int length, byte key[IDENT_KEY_SIZE],
				char userinfo[SESSION_USERINFO_MAX + 1] );

// A key's Noise static (Ident_X25519Pair)
void		Session_Static( const identKey_t *key, noiseStatic_t *s );
// Whether a peer's Ed25519 key, as its payload gives it, is a real one
// (Ident_ValidKey) whose conversion is the static the handshake proved
qboolean	Session_KeyIsStatic( const byte key[IDENT_KEY_SIZE], const byte remoteStatic[NOISE_DHLEN] );

#endif
