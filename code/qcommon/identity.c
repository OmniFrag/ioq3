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
// identity.c -- device and server keys, and the proofs of the handshake
// (identity.h)

#include "q_shared.h"
#include "qcommon.h"
#include "identity.h"
#include "../thirdparty/monocypher-4.0.2/monocypher.h"
#include "../thirdparty/monocypher-4.0.2/monocypher-ed25519.h"

void Ident_Wipe( void *secret, size_t size ) {
	crypto_wipe( secret, size );
}

void Ident_KeyFromSeed( identKey_t *key, const byte seed[IDENT_KEY_SIZE] ) {
	byte	copy[IDENT_KEY_SIZE];

	// Monocypher wipes the seed it's given
	Com_Memcpy( copy, seed, IDENT_KEY_SIZE );
	crypto_ed25519_key_pair( key->secret, key->pub, copy );
}

qboolean Ident_Equal32( const byte a[IDENT_KEY_SIZE], const byte b[IDENT_KEY_SIZE] ) {
	return crypto_verify32( a, b ) == 0;
}

/*
=================
Ident_Message

What a proof signs: the domain with its terminator, then the parts, each
IDENT_KEY_SIZE bytes, then the challenge's 4 bytes, least significant
first, when there is one
=================
*/
#define IDENT_MESSAGE_SIZE	( 32 + IDENT_KEY_SIZE * 3 + 4 )

static int Ident_Message( byte message[IDENT_MESSAGE_SIZE], const char *domain, const byte *a, const byte *b,
	const byte *c, const int *challenge ) {
	int		length = (int)strlen( domain ) + 1;
	const byte	*parts[3] = { a, b, c };
	int		i;

	Com_Memcpy( message, domain, length );
	for ( i = 0; i < 3; i++ ) {
		if ( parts[i] ) {
			Com_Memcpy( message + length, parts[i], IDENT_KEY_SIZE );
			length += IDENT_KEY_SIZE;
		}
	}
	if ( challenge ) {
		unsigned int	value = (unsigned int)*challenge;

		for ( i = 0; i < 4; i++ ) {
			message[length++] = (byte)( value >> ( i * 8 ) );
		}
	}
	return length;
}

void Ident_SignServer( const identKey_t *server, const byte clientNonce[IDENT_KEY_SIZE],
	const byte serverNonce[IDENT_KEY_SIZE], byte sig[IDENT_SIG_SIZE] ) {
	byte	message[IDENT_MESSAGE_SIZE];
	int		length = Ident_Message( message, IDENT_SERVER_DOMAIN, clientNonce, serverNonce, NULL, NULL );

	crypto_ed25519_sign( sig, server->secret, message, length );
}

qboolean Ident_CheckServer( const byte serverKey[IDENT_KEY_SIZE], const byte clientNonce[IDENT_KEY_SIZE],
	const byte serverNonce[IDENT_KEY_SIZE], const byte sig[IDENT_SIG_SIZE] ) {
	byte	message[IDENT_MESSAGE_SIZE];
	int		length = Ident_Message( message, IDENT_SERVER_DOMAIN, clientNonce, serverNonce, NULL, NULL );

	return crypto_ed25519_check( sig, serverKey, message, length ) == 0;
}

void Ident_SignClient( const identKey_t *client, const byte serverKey[IDENT_KEY_SIZE],
	const byte serverNonce[IDENT_KEY_SIZE], const byte clientNonce[IDENT_KEY_SIZE], int challenge,
	byte sig[IDENT_SIG_SIZE] ) {
	byte	message[IDENT_MESSAGE_SIZE];
	int		length = Ident_Message( message, IDENT_CLIENT_DOMAIN, serverKey, serverNonce, clientNonce, &challenge );

	crypto_ed25519_sign( sig, client->secret, message, length );
}

qboolean Ident_CheckClient( const byte clientKey[IDENT_KEY_SIZE], const byte serverKey[IDENT_KEY_SIZE],
	const byte serverNonce[IDENT_KEY_SIZE], const byte clientNonce[IDENT_KEY_SIZE], int challenge,
	const byte sig[IDENT_SIG_SIZE] ) {
	byte	message[IDENT_MESSAGE_SIZE];
	int		length = Ident_Message( message, IDENT_CLIENT_DOMAIN, serverKey, serverNonce, clientNonce, &challenge );

	return crypto_ed25519_check( sig, clientKey, message, length ) == 0;
}

/*
=================
Ident_PurposeMessage

What a purpose's signature signs, in buf: IDENT_PURPOSE_DOMAIN and the
purpose, each with its terminator, then the message. No proof's domain is
IDENT_PURPOSE_DOMAIN, so none signs the same bytes. 0 when
Ident_SignPurpose refuses it
=================
*/
#define IDENT_PURPOSE_SIZE	( sizeof( IDENT_PURPOSE_DOMAIN ) + IDENT_PURPOSE_NAME_MAX + 1 + IDENT_PURPOSE_MAX )

static int Ident_PurposeMessage( byte buf[IDENT_PURPOSE_SIZE], const char *purpose, const void *message, int length ) {
	int		name;

	if ( !purpose || !message || length < 0 || length > IDENT_PURPOSE_MAX ) {
		return 0;
	}
	name = (int)strlen( purpose );
	if ( !name || name > IDENT_PURPOSE_NAME_MAX ) {
		return 0;
	}
	Com_Memcpy( buf, IDENT_PURPOSE_DOMAIN, sizeof( IDENT_PURPOSE_DOMAIN ) );
	Com_Memcpy( buf + sizeof( IDENT_PURPOSE_DOMAIN ), purpose, name + 1 );
	Com_Memcpy( buf + sizeof( IDENT_PURPOSE_DOMAIN ) + name + 1, message, length );
	return (int)sizeof( IDENT_PURPOSE_DOMAIN ) + name + 1 + length;
}

qboolean Ident_SignPurpose( const identKey_t *key, const char *purpose, const void *message, int length,
	byte sig[IDENT_SIG_SIZE] ) {
	byte	buf[IDENT_PURPOSE_SIZE];
	int		total = Ident_PurposeMessage( buf, purpose, message, length );

	if ( !total ) {
		return qfalse;
	}
	crypto_ed25519_sign( sig, key->secret, buf, total );
	return qtrue;
}

qboolean Ident_CheckPurpose( const byte key[IDENT_KEY_SIZE], const char *purpose, const void *message, int length,
	const byte sig[IDENT_SIG_SIZE] ) {
	byte	buf[IDENT_PURPOSE_SIZE];
	int		total = Ident_PurposeMessage( buf, purpose, message, length );

	return total && crypto_ed25519_check( sig, key, buf, total ) == 0;
}

qboolean Ident_ReadNonce( const extTokens_t *tokens, const char *key, byte nonce[IDENT_KEY_SIZE] ) {
	const char	*value = Com_ExtensionValue( tokens, key );

	return value && Com_SHA256FromHex( value, nonce );
}

// a signature's two halves, ofs then oft
static qboolean Ident_ReadSig( const extTokens_t *tokens, byte sig[IDENT_SIG_SIZE] ) {
	return Ident_ReadNonce( tokens, "ofs", sig ) && Ident_ReadNonce( tokens, "oft", sig + IDENT_KEY_SIZE );
}

qboolean Ident_ReadServerProof( const extTokens_t *tokens, byte serverNonce[IDENT_KEY_SIZE],
	byte serverKey[IDENT_KEY_SIZE], byte sig[IDENT_SIG_SIZE] ) {
	return Ident_ReadNonce( tokens, "ofn", serverNonce ) && Ident_ReadNonce( tokens, "ofp", serverKey ) &&
		Ident_ReadSig( tokens, sig );
}

qboolean Ident_ReadClientProof( const extTokens_t *tokens, byte clientKey[IDENT_KEY_SIZE], byte sig[IDENT_SIG_SIZE] ) {
	return Ident_ReadNonce( tokens, "ofi", clientKey ) && Ident_ReadSig( tokens, sig );
}

qboolean Ident_AppendHex( char *text, int size, const char *key, const byte bytes[IDENT_KEY_SIZE] ) {
	char	hex[IDENT_HEX_SIZE];

	Com_SHA256Hex( bytes, hex );
	return Com_AppendExtensionToken( text, size, key, hex );
}

qboolean Ident_AppendSig( char *text, int size, const byte sig[IDENT_SIG_SIZE] ) {
	char	saved[EXT_MAX_STRING];

	// both halves or neither
	Q_strncpyz( saved, text, sizeof( saved ) );
	if ( Ident_AppendHex( text, size, "ofs", sig ) && Ident_AppendHex( text, size, "oft", sig + IDENT_KEY_SIZE ) ) {
		return qtrue;
	}
	Q_strncpyz( text, saved, size );
	return qfalse;
}

void Ident_Fingerprint( const byte key[IDENT_KEY_SIZE], char fingerprint[IDENT_FINGERPRINT_SIZE] ) {
	byte	digest[SHA256_DIGEST_SIZE];
	char	hex[SHA256_HEX_SIZE];
	int		i, used = 0;

	Com_SHA256Buffer( key, IDENT_KEY_SIZE, digest );
	Com_SHA256Hex( digest, hex );
	for ( i = 0; i < 20; i++ ) {
		if ( i && !( i % 4 ) ) {
			fingerprint[used++] = '-';
		}
		fingerprint[used++] = hex[i];
	}
	fingerprint[used] = '\0';
}

qboolean Ident_KnownServer( const char *text, const char *address, byte key[IDENT_KEY_SIZE] ) {
	int		length = (int)strlen( address );
	char	hex[IDENT_HEX_SIZE];

	for ( ; text && *text; text = strchr( text, '\n' ) ? strchr( text, '\n' ) + 1 : NULL ) {
		// the address, a space, the key and the line's end
		if ( !strncmp( text, address, length ) && text[length] == ' ' ) {
			Q_strncpyz( hex, text + length + 1, sizeof( hex ) );
			return Com_SHA256FromHex( hex, key ) &&
				( text[length + IDENT_HEX_SIZE] == '\n' || !text[length + IDENT_HEX_SIZE] );
		}
	}
	return qfalse;
}

qboolean Ident_KnownServerLine( const char *address, const byte key[IDENT_KEY_SIZE], char *line, int size ) {
	char	hex[IDENT_HEX_SIZE];

	Com_SHA256Hex( key, hex );
	return Com_sprintf( line, size, "%s %s\n", address, hex ) < size;
}

void Ident_KeyFileText( const byte seed[IDENT_KEY_SIZE], char text[IDENT_FILE_SIZE + 1] ) {
	char	hex[IDENT_HEX_SIZE];

	Com_SHA256Hex( seed, hex );
	Com_sprintf( text, IDENT_FILE_SIZE + 1, "%s%s\n", IDENT_FILE_HEADER, hex );
	Ident_Wipe( hex, sizeof( hex ) );
}

qboolean Ident_ReadKeyFile( const char *text, int length, byte seed[IDENT_KEY_SIZE] ) {
	int		header = (int)strlen( IDENT_FILE_HEADER );
	char	hex[IDENT_HEX_SIZE];
	qboolean	read;

	if ( length < header + IDENT_HEX_SIZE - 1 || strncmp( text, IDENT_FILE_HEADER, header ) ) {
		return qfalse;
	}
	// the digits, then a line break or the end, and nothing after
	if ( length > header + IDENT_HEX_SIZE || ( length == header + IDENT_HEX_SIZE &&
		text[header + IDENT_HEX_SIZE - 1] != '\n' ) ) {
		return qfalse;
	}
	Com_Memcpy( hex, text + header, IDENT_HEX_SIZE - 1 );
	hex[IDENT_HEX_SIZE - 1] = '\0';
	read = Com_SHA256FromHex( hex, seed );
	Ident_Wipe( hex, sizeof( hex ) );
	return read;
}

// a password's Argon2id hash, raw
static void Ident_Argon2( const char *password, const byte salt[16], void *work, byte hash[32] ) {
	crypto_argon2_config	config = { CRYPTO_ARGON2_ID, IDENT_ARGON2_BLOCKS, IDENT_ARGON2_PASSES, 1 };
	crypto_argon2_inputs	inputs = { (const uint8_t *)password, salt, (uint32_t)strlen( password ), 16 };

	crypto_argon2( hash, 32, work, config, inputs, crypto_argon2_no_extras );
}

#define IDENT_HASH_PREFIX	"$argon2id$v=19$m=19456,t=2,p=1$"

void Ident_HashPassword( const char *password, const byte salt[16], void *work,
	char hash[IDENT_PASSWORD_HASH_SIZE] ) {
	byte	raw[32];
	char	saltHex[33], hashHex[SHA256_HEX_SIZE];

	Ident_Argon2( password, salt, work, raw );
	Com_BytesToHex( salt, 16, saltHex );
	Com_SHA256Hex( raw, hashHex );
	Com_sprintf( hash, IDENT_PASSWORD_HASH_SIZE, "%s%s$%s", IDENT_HASH_PREFIX, saltHex, hashHex );
	Ident_Wipe( raw, sizeof( raw ) );
}

qboolean Ident_CheckPassword( const char *password, const char *hash, void *work ) {
	int		prefix = (int)strlen( IDENT_HASH_PREFIX );
	char	saltHex[33];
	byte	salt[16], want[32], got[32];
	qboolean	same;

	// the prefix, 32 hex digits of salt, "$", 64 of hash, and the end
	if ( strncmp( hash, IDENT_HASH_PREFIX, prefix ) || strlen( hash ) != (size_t)( prefix + 32 + 1 + 64 ) ||
		hash[prefix + 32] != '$' ) {
		return qfalse;
	}
	Com_Memcpy( saltHex, hash + prefix, 32 );
	saltHex[32] = '\0';
	if ( !Com_HexToBytes( saltHex, salt, 16 ) || !Com_SHA256FromHex( hash + prefix + 33, want ) ) {
		return qfalse;
	}
	Ident_Argon2( password, salt, work, got );
	same = Ident_Equal32( got, want );
	Ident_Wipe( got, sizeof( got ) );
	return same;
}
