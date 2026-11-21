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
// session.c -- OFX_SESSION's prologue, payloads and key checks (session.h)

#include "q_shared.h"
#include "session.h"
#include "../thirdparty/monocypher-4.0.2/monocypher.h"

static const char	sessionLabel[] = "OmniFrag session v1";

static byte *Session_Put32( byte *p, uint32_t value ) {
	p[0] = (byte)value;
	p[1] = (byte)( value >> 8 );
	p[2] = (byte)( value >> 16 );
	p[3] = (byte)( value >> 24 );
	return p + 4;
}

static byte *Session_Put16( byte *p, unsigned int value ) {
	p[0] = (byte)value;
	p[1] = (byte)( value >> 8 );
	return p + 2;
}

int Session_Prologue( byte out[SESSION_PROLOGUE_MAX], int protocol, const char *game, int challenge,
	uint32_t ofx, int fragmentSize, int qport ) {
	byte	*p = out;
	size_t	gameLength = strlen( game );

	if ( gameLength > SESSION_GAME_MAX ) {
		return 0;
	}
	// the label with its terminator
	Com_Memcpy( p, sessionLabel, sizeof( sessionLabel ) );
	p += sizeof( sessionLabel );
	p = Session_Put32( p, (uint32_t)protocol );
	*p++ = (byte)gameLength;
	Com_Memcpy( p, game, gameLength );
	p += gameLength;
	p = Session_Put32( p, (uint32_t)challenge );
	p = Session_Put32( p, ofx );
	p = Session_Put16( p, (unsigned int)fragmentSize & 0xffff );
	p = Session_Put16( p, (unsigned int)qport & 0xffff );
	return (int)( p - out );
}

int Session_WriteServerPayload( byte out[SESSION_SERVER_MAX], const byte key[IDENT_KEY_SIZE], uint64_t ofk,
	const byte *manifest ) {
	byte	*p = out;
	int		i;

	Com_Memcpy( p, key, IDENT_KEY_SIZE );
	p += IDENT_KEY_SIZE;
	for ( i = 0; i < 8; i++ ) {
		*p++ = (byte)( ofk >> ( i * 8 ) );
	}
	*p++ = manifest ? SESSION_MANIFEST : 0;
	if ( manifest ) {
		Com_Memcpy( p, manifest, 32 );
		p += 32;
	}
	return (int)( p - out );
}

qboolean Session_ReadServerPayload( const byte *in, int length, byte key[IDENT_KEY_SIZE], uint64_t *ofk,
	byte manifest[32], qboolean *hasManifest ) {
	int		i;
	byte	flags;

	if ( length < IDENT_KEY_SIZE + 8 + 1 ) {
		return qfalse;
	}
	flags = in[IDENT_KEY_SIZE + 8];
	if ( ( flags & ~SESSION_MANIFEST ) || length != IDENT_KEY_SIZE + 8 + 1 + ( flags ? 32 : 0 ) ) {
		return qfalse;
	}
	Com_Memcpy( key, in, IDENT_KEY_SIZE );
	*ofk = 0;
	for ( i = 7; i >= 0; i-- ) {
		*ofk = *ofk << 8 | in[IDENT_KEY_SIZE + i];
	}
	*hasManifest = flags != 0;
	if ( *hasManifest ) {
		Com_Memcpy( manifest, in + IDENT_KEY_SIZE + 9, 32 );
	}
	return qtrue;
}

int Session_WriteClientPayload( byte out[SESSION_CLIENT_MAX], const byte key[IDENT_KEY_SIZE], const char *userinfo ) {
	size_t	length = strlen( userinfo );

	if ( length > SESSION_USERINFO_MAX ) {
		length = SESSION_USERINFO_MAX;
	}
	Com_Memcpy( out, key, IDENT_KEY_SIZE );
	Session_Put16( out + IDENT_KEY_SIZE, (unsigned int)length );
	Com_Memcpy( out + IDENT_KEY_SIZE + 2, userinfo, length );
	return IDENT_KEY_SIZE + 2 + (int)length;
}

qboolean Session_ReadClientPayload( const byte *in, int length, byte key[IDENT_KEY_SIZE],
	char userinfo[SESSION_USERINFO_MAX + 1] ) {
	int		u;

	if ( length < IDENT_KEY_SIZE + 2 ) {
		return qfalse;
	}
	u = in[IDENT_KEY_SIZE] | in[IDENT_KEY_SIZE + 1] << 8;
	// the rest of the payload exactly, no NUL inside
	if ( u > SESSION_USERINFO_MAX || length != IDENT_KEY_SIZE + 2 + u ||
		memchr( in + IDENT_KEY_SIZE + 2, '\0', u ) ) {
		return qfalse;
	}
	Com_Memcpy( key, in, IDENT_KEY_SIZE );
	Com_Memcpy( userinfo, in + IDENT_KEY_SIZE + 2, u );
	userinfo[u] = '\0';
	return qtrue;
}

void Session_Static( const identKey_t *key, noiseStatic_t *s ) {
	Ident_X25519Pair( key, s->secret, s->pub );
}

qboolean Session_KeyIsStatic( const byte key[IDENT_KEY_SIZE], const byte remoteStatic[NOISE_DHLEN] ) {
	byte	converted[NOISE_DHLEN];

	if ( !Ident_ValidKey( key ) ) {
		return qfalse;
	}
	Ident_X25519Public( key, converted );
	return crypto_verify32( converted, remoteStatic ) == 0;
}
