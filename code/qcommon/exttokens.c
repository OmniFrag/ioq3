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
// exttokens.c -- extension tokens, read and written (exttokens.h)

#include <string.h>

#include "q_shared.h"
#include "exttokens.h"

static qboolean IsHexDigit( char c ) {
	return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' );
}

static int HexValue( char c ) {
	return c <= '9' ? c - '0' : c - 'a' + 10;
}

static qboolean IsValueChar( char c ) {
	return ( c >= '0' && c <= '9' ) || Q_islower( c ) || c == '.' || c == '-';
}

/*
=================
ExtensionKeyLength

The length of the key at the start of s, up to the first character that
isn't a lowercase letter: 2 to EXT_MAX_KEY of them, starting with "of".
0 if it isn't one
=================
*/
static int ExtensionKeyLength( const char *s ) {
	int		i;

	if ( s[0] != 'o' || s[1] != 'f' ) {
		return 0;
	}
	for ( i = 2; Q_islower( s[i] ); i++ ) {
		if ( i == EXT_MAX_KEY ) {
			return 0;
		}
	}
	return i;
}

/*
=================
ExtensionValueLength

The length of the value at the start of s, up to a space or the
terminator: 1 to EXT_MAX_VALUE characters from [0-9a-z.-]. 0 if it
isn't one
=================
*/
static int ExtensionValueLength( const char *s ) {
	int		i;

	for ( i = 0; IsValueChar( s[i] ); i++ ) {
		if ( i == EXT_MAX_VALUE ) {
			return 0;
		}
	}
	if ( s[i] && s[i] != ' ' ) {
		return 0;
	}
	return i;
}

/*
=================
Com_ParseExtensionTokens
=================
*/
const char *Com_ParseExtensionTokens( const char *text, extTokens_t *tokens ) {
	const char	*error;
	extToken_t	*token;
	int			keyLength, valueLength;

	tokens->count = 0;
	for ( ;; ) {
		while ( *text == ' ' ) {
			text++;
		}
		if ( !*text ) {
			return NULL;
		}
		if ( tokens->count == EXT_MAX_TOKENS ) {
			error = "more than " XSTRING( EXT_MAX_TOKENS ) " tokens";
			break;
		}
		keyLength = ExtensionKeyLength( text );
		if ( !keyLength || text[keyLength] != '=' ) {
			error = "a malformed key";
			break;
		}
		valueLength = ExtensionValueLength( text + keyLength + 1 );
		if ( !valueLength ) {
			error = "a malformed value";
			break;
		}

		token = &tokens->tokens[tokens->count];
		memcpy( token->key, text, keyLength );
		token->key[keyLength] = '\0';
		memcpy( token->value, text + keyLength + 1, valueLength );
		token->value[valueLength] = '\0';
		if ( Com_ExtensionValue( tokens, token->key ) ) {
			error = "a key twice";
			break;
		}
		tokens->count++;
		text += keyLength + 1 + valueLength;
	}

	tokens->count = 0;
	return error;
}

/*
=================
Com_ExtensionValue
=================
*/
const char *Com_ExtensionValue( const extTokens_t *tokens, const char *key ) {
	int		i;

	for ( i = 0; i < tokens->count; i++ ) {
		if ( !strcmp( tokens->tokens[i].key, key ) ) {
			return tokens->tokens[i].value;
		}
	}
	return NULL;
}

/*
=================
Com_ParseExtensionBits
=================
*/
qboolean Com_ParseExtensionBits( const char *value, uint32_t *bits ) {
	uint32_t	result = 0;
	int			i;

	for ( i = 0; IsHexDigit( value[i] ); i++ ) {
		if ( i == 8 ) {
			return qfalse;
		}
		result = result << 4 | HexValue( value[i] );
	}
	if ( !i || value[i] ) {
		return qfalse;
	}
	*bits = result;
	return qtrue;
}

/*
=================
Com_ParseExtensionKey
=================
*/
qboolean Com_ParseExtensionKey( const char *value, uint64_t *key ) {
	uint64_t	result = 0;
	int			i;

	for ( i = 0; i < OFK_DIGITS; i++ ) {
		if ( !IsHexDigit( value[i] ) ) {
			return qfalse;
		}
		result = result << 4 | HexValue( value[i] );
	}
	if ( value[i] ) {
		return qfalse;
	}
	*key = result;
	return qtrue;
}

/*
=================
Com_ExtensionKeyString
=================
*/
void Com_ExtensionKeyString( uint64_t key, char string[OFK_DIGITS + 1] ) {
	static const char	digits[] = "0123456789abcdef";
	int					i;

	for ( i = OFK_DIGITS - 1; i >= 0; i-- ) {
		string[i] = digits[key & 15];
		key >>= 4;
	}
	string[OFK_DIGITS] = '\0';
}

/*
=================
Com_AppendExtensionToken
=================
*/
qboolean Com_AppendExtensionToken( char *text, int size, const char *key, const char *value ) {
	extTokens_t	tokens;
	int			length, keyLength, valueLength;

	keyLength = ExtensionKeyLength( key );
	valueLength = ExtensionValueLength( value );
	if ( !keyLength || key[keyLength] || !valueLength || value[valueLength] ) {
		return qfalse;
	}
	// what's there must be a set this token can join
	if ( Com_ParseExtensionTokens( text, &tokens ) || tokens.count == EXT_MAX_TOKENS ||
		Com_ExtensionValue( &tokens, key ) ) {
		return qfalse;
	}
	length = strlen( text );
	if ( length + 1 + keyLength + 1 + valueLength >= size ) {
		return qfalse;
	}
	Com_sprintf( text + length, size - length, " %s=%s", key, value );
	return qtrue;
}

/*
=================
Com_ParseExtensionBitsAndKey
=================
*/
const char *Com_ParseExtensionBitsAndKey( const char *text, uint32_t *bits, uint64_t *key ) {
	extTokens_t	tokens;
	const char	*error, *value;

	*bits = 0;
	error = Com_ParseExtensionTokens( text, &tokens );
	if ( error ) {
		return error;
	}
	value = Com_ExtensionValue( &tokens, "ofx" );
	if ( !value ) {
		return NULL;
	}
	if ( !Com_ParseExtensionBits( value, bits ) ) {
		*bits = 0;
		return "a malformed ofx";
	}
	if ( key ) {
		value = Com_ExtensionValue( &tokens, "ofk" );
		if ( !value || !Com_ParseExtensionKey( value, key ) ) {
			*bits = 0;
			return value ? "a malformed ofk" : "no ofk";
		}
	}
	return NULL;
}
