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
// com_sha256.c -- SHA-256 for the engine, over Brad Conte's sha256.c, whose
// types stay out of qcommon.h

#include "q_shared.h"
#include "qcommon.h"
#include "sha256.h"

// sha256_t holds a SHA256_CTX: a compile error here means it's too small
typedef char sha256ContextFits[ sizeof( SHA256_CTX ) <= sizeof( ( (sha256_t *)0 )->u.opaque ) ? 1 : -1 ];

/*
=================
Com_SHA256Init
=================
*/
void Com_SHA256Init( sha256_t *ctx ) {
	sha256_init( (SHA256_CTX *)ctx->u.opaque );
}

/*
=================
Com_SHA256Update

Hashes len more bytes. sha256_update counts them in a WORD, so they go in
pieces of at most 1 GB
=================
*/
void Com_SHA256Update( sha256_t *ctx, const void *data, size_t len ) {
	const byte	*bytes = data;
	size_t		piece;

	while ( len > 0 ) {
		piece = len < 0x40000000 ? len : 0x40000000;
		sha256_update( (SHA256_CTX *)ctx->u.opaque, bytes, piece );
		bytes += piece;
		len -= piece;
	}
}

/*
=================
Com_SHA256Final
=================
*/
void Com_SHA256Final( sha256_t *ctx, byte digest[SHA256_DIGEST_SIZE] ) {
	sha256_final( (SHA256_CTX *)ctx->u.opaque, digest );
}

/*
=================
Com_SHA256Buffer

The digest of one buffer, whole
=================
*/
void Com_SHA256Buffer( const void *data, size_t len, byte digest[SHA256_DIGEST_SIZE] ) {
	sha256_t	ctx;

	Com_SHA256Init( &ctx );
	Com_SHA256Update( &ctx, data, len );
	Com_SHA256Final( &ctx, digest );
}

/*
=================
Com_SHA256Hex

A digest as 64 lowercase hexadecimal digits
=================
*/
void Com_SHA256Hex( const byte digest[SHA256_DIGEST_SIZE], char hex[SHA256_HEX_SIZE] ) {
	static const char	digits[] = "0123456789abcdef";
	int					i;

	for ( i = 0; i < SHA256_DIGEST_SIZE; i++ ) {
		hex[i * 2] = digits[digest[i] >> 4];
		hex[i * 2 + 1] = digits[digest[i] & 15];
	}
	hex[SHA256_DIGEST_SIZE * 2] = '\0';
}

/*
=================
Com_SHA256FromHex

A digest from exactly 64 lowercase hexadecimal digits, as Com_SHA256Hex
writes them; qfalse for anything else, leaving digest as it was
=================
*/
qboolean Com_SHA256FromHex( const char *hex, byte digest[SHA256_DIGEST_SIZE] ) {
	byte	read[SHA256_DIGEST_SIZE];
	int		i, nibble;
	char	c;

	for ( i = 0; i < SHA256_DIGEST_SIZE * 2; i++ ) {
		c = hex[i];
		if ( c >= '0' && c <= '9' ) {
			nibble = c - '0';
		} else if ( c >= 'a' && c <= 'f' ) {
			nibble = c - 'a' + 10;
		} else {
			return qfalse;	// the terminator too, in a short string
		}
		if ( i & 1 ) {
			read[i / 2] |= nibble;
		} else {
			read[i / 2] = nibble << 4;
		}
	}
	if ( hex[i] != '\0' ) {
		return qfalse;
	}
	Com_Memcpy( digest, read, sizeof( read ) );
	return qtrue;
}
