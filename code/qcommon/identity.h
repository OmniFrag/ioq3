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
// identity.h -- a client's device key and a server's own, Ed25519
// (Monocypher, RFC 8032, with SHA-512). Each is proved in the session's
// handshake (session.h), Noise XX with each key converted to X25519, which
// classic peers never agree on, so they see none of it. Roles, pins and
// fingerprints go by the Ed25519 key.
//
// The functions here use no globals, and call only q_shared.c's,
// com_sha256.c's, exttokens.c's and Monocypher's, so a fuzz harness or a
// unit test links them alone.

#ifndef IDENTITY_H
#define IDENTITY_H

#include "exttokens.h"

#define IDENT_KEY_SIZE		32		// a public key, a seed, a nonce, half a signature
#define IDENT_SIG_SIZE		64
#define IDENT_HEX_SIZE		( IDENT_KEY_SIZE * 2 + 1 )
// the fingerprint people see: the first 20 hex digits of the key's
// SHA-256, in groups of 4, "3f2a-9c01-77de-b0a4-5e21"
#define IDENT_FINGERPRINT_SIZE	25

#define IDENT_DEVICE_FILE	"identity.key"	// at the home config base's root
#define IDENT_SERVER_FILE	"server.key"	// in a dedicated server's folder
#define IDENT_KNOWN_FILE	"known_servers.txt"	// a client's, at the home config base's root

#define IDENT_PURPOSE_DOMAIN	"OmniFrag purpose v1"	// Ident_SignPurpose's

// a key pair: the secret never leaves its file but as this
typedef struct identKey_s {
	byte	pub[IDENT_KEY_SIZE];
	byte	secret[64];		// Monocypher's form: the seed, then the public key
} identKey_t;

void		Ident_KeyFromSeed( identKey_t *key, const byte seed[IDENT_KEY_SIZE] );
// Whether a key a peer gives is a real Ed25519 one, a point on the curve
// with its y canonical, as libsodium refuses others: one that isn't can
// never sign, so it's never stored or shown
qboolean	Ident_ValidKey( const byte key[IDENT_KEY_SIZE] );
// The key as an X25519 static, for the session's Noise handshake (noise.h):
// the public key by crypto_eddsa_to_x25519, the secret the scalar Ed25519
// signs with. A key and its negation give one X25519 key
void		Ident_X25519Public( const byte key[IDENT_KEY_SIZE], byte x25519[IDENT_KEY_SIZE] );
void		Ident_X25519Pair( const identKey_t *key, byte secret[IDENT_KEY_SIZE], byte pub[IDENT_KEY_SIZE] );
void		Ident_Wipe( void *secret, size_t size );
// whether two 32-byte values are the same, in constant time
qboolean	Ident_Equal32( const byte a[IDENT_KEY_SIZE], const byte b[IDENT_KEY_SIZE] );

// A signature for another purpose than the handshake's, a service's message
// say: over IDENT_PURPOSE_DOMAIN and the purpose, each with its terminator,
// then the message, so it holds only for that purpose. qfalse, and no signature, for an empty purpose or one past
// IDENT_PURPOSE_NAME_MAX characters, a NULL message, or one past
// IDENT_PURPOSE_MAX bytes; an empty message is signed
#define IDENT_PURPOSE_NAME_MAX	63
#define IDENT_PURPOSE_MAX		2048
qboolean	Ident_SignPurpose( const identKey_t *key, const char *purpose, const void *message, int length,
				byte sig[IDENT_SIG_SIZE] );
qboolean	Ident_CheckPurpose( const byte key[IDENT_KEY_SIZE], const char *purpose, const void *message, int length,
				const byte sig[IDENT_SIG_SIZE] );

void		Ident_Fingerprint( const byte key[IDENT_KEY_SIZE], char fingerprint[IDENT_FINGERPRINT_SIZE] );

// The keys a client knows servers by (known_servers.txt), a line each:
// "<address> <key in hex>". Ident_KnownServer finds an address's key in
// the text, qfalse where it has none; Ident_KnownServerLine writes the
// line for one, qfalse if it doesn't fit or the address holds a space or a
// control character, which would make another line; Ident_KnownServerText writes the
// text with the address's line left out, and with key's for it at the end
// unless key is NULL, returning its length, or -1 if it doesn't fit
qboolean	Ident_KnownServer( const char *text, const char *address, byte key[IDENT_KEY_SIZE] );
qboolean	Ident_KnownServerLine( const char *address, const byte key[IDENT_KEY_SIZE], char *line, int size );
int			Ident_KnownServerText( const char *text, const char *address, const byte *key, char *out, int size );

// The key a server at an address is remembered by (com_identity.c): found,
// with qfalse for none; remembered from now on, in place of any before,
// with qfalse if it couldn't be written; or forgotten. --check writes
// nothing
qboolean	Com_RememberedServerKey( const char *address, byte key[IDENT_KEY_SIZE] );
qboolean	Com_RememberServerKey( const char *address, const byte key[IDENT_KEY_SIZE] );
void		Com_ForgetServerKey( const char *address );

// A key file's text: "OmniFrag key v1\n<seed in hex>\n"; reading it gives
// qfalse for anything else
#define IDENT_FILE_HEADER	"OmniFrag key v1\n"
#define IDENT_FILE_SIZE		( sizeof( IDENT_FILE_HEADER ) - 1 + IDENT_HEX_SIZE )
void		Ident_KeyFileText( const byte seed[IDENT_KEY_SIZE], char text[IDENT_FILE_SIZE + 1] );
qboolean	Ident_ReadKeyFile( const char *text, int length, byte seed[IDENT_KEY_SIZE] );

// A password's hash for a login, Argon2id (RFC 9106) with a 16-byte salt:
// "$argon2id$v=19$m=19456,t=2,p=1$<salt hex>$<hash hex>". work is
// IDENT_ARGON2_WORK bytes the caller lends (it's large). Ident_CheckPassword
// compares in constant time, and is qfalse for a hash of another form
#define IDENT_ARGON2_BLOCKS	19456	// KiB: 19 MiB
#define IDENT_ARGON2_PASSES	2
#define IDENT_ARGON2_WORK	( IDENT_ARGON2_BLOCKS * 1024 )
#define IDENT_PASSWORD_HASH_SIZE	160	// the 128 characters above, and room
void		Ident_HashPassword( const char *password, const byte salt[16], void *work,
				char hash[IDENT_PASSWORD_HASH_SIZE] );
qboolean	Ident_CheckPassword( const char *password, const char *hash, void *work );

#endif
