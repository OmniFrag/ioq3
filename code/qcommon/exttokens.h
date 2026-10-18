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
// exttokens.h -- extension tokens: key=value words after the classic
// arguments of getchallenge, challengeResponse, connectResponse and
// getinfo, by which two peers of this engine agree on what both can do.
// Classic clients and servers read no further than their own arguments,
// so they see nothing new, and are sent nothing new.
//
// A token is key=value: the key "of" and up to two more lowercase letters,
// the value 1 to EXT_MAX_VALUE characters from [0-9a-z.-]. A message has at
// most EXT_MAX_TOKENS. More, a malformed token or a key given twice, and
// the whole set is ignored, so the connection is classic. Keys a peer
// doesn't know are ignored.
//
// The functions here use no globals and call only q_shared.c's helpers
// (Q_islower, Com_sprintf), so a fuzz harness or a unit test links this
// file with q_shared.c and the q_math.c it needs.
#ifndef EXTTOKENS_H
#define EXTTOKENS_H

#define EXT_MAX_TOKENS	8	// in one message
#define EXT_MAX_KEY		4	// characters in a key
#define EXT_MAX_VALUE	64	// characters in a value

// the longest set of tokens Com_AppendExtensionToken writes, each with
// the space before it, and the terminator
#define EXT_MAX_STRING	( EXT_MAX_TOKENS * ( 1 + EXT_MAX_KEY + 1 + EXT_MAX_VALUE ) + 1 )

// the capability bits, sent as ofx in lowercase hex. Each is assigned in
// the commit that first uses it and never reused
#define OFX_CONTENT		0x00000001u	// content protocol version 1
#define OFX_BUILD		OFX_CONTENT	// the bits this build has

// the digits of ofk: a connection's 64-bit content key in lowercase hex,
// the most significant first, as the content datagrams' key field holds
// it (ofd.h)
#define OFK_DIGITS		16

typedef struct {
	char	key[EXT_MAX_KEY + 1];
	char	value[EXT_MAX_VALUE + 1];
} extToken_t;

typedef struct {
	int			count;
	extToken_t	tokens[EXT_MAX_TOKENS];
} extTokens_t;

// Parses the tokens in text, separated by spaces, into tokens. NULL when
// they're well formed, an empty text among them; otherwise why not, and
// tokens is left empty. text must be terminated
const char	*Com_ParseExtensionTokens( const char *text, extTokens_t *tokens );

// the value of key in tokens, or NULL
const char	*Com_ExtensionValue( const extTokens_t *tokens, const char *key );

// ofx's value: 1 to 8 lowercase hex digits
qboolean	Com_ParseExtensionBits( const char *value, uint32_t *bits );

// ofk's value: exactly OFK_DIGITS lowercase hex digits
qboolean	Com_ParseExtensionKey( const char *value, uint64_t *key );
void		Com_ExtensionKeyString( uint64_t key, char string[OFK_DIGITS + 1] );

// The handshake's tokens in text: their ofx bits, 0 without one, and with
// key, their ofk, which must then be there. NULL when they're well formed;
// otherwise why not, with bits 0
const char	*Com_ParseExtensionBitsAndKey( const char *text, uint32_t *bits, uint64_t *key );

// Appends " key=value" to text, a set of tokens of size bytes, which the
// caller starts empty and puts after a message's classic arguments.
// qfalse, leaving text as it was, for a malformed key or value, a key
// already there, a set of EXT_MAX_TOKENS already, or no room. What it
// writes, Com_ParseExtensionTokens reads back
qboolean	Com_AppendExtensionToken( char *text, int size, const char *key, const char *value );

#endif
