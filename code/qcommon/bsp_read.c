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
// bsp_read.c -- what a map's BSP says ahead of loading it: its header, as
// the collision map checks it, and a key of its first entity, the
// worldspawn, as game code reads it

#include "q_shared.h"
#include "qcommon.h"

/*
=================
BSP_CheckHeader

A BSP's header from its first length bytes, in the file's byte order
turned to ours, and whether a map loads from the file of fileLength bytes:
its ident and version, and each lump inside the file at an offset the
lumps are read in place from. CM_LoadMap says each fault its way; lump is
the lump at fault
=================
*/
bspFault_t BSP_CheckHeader( const void *data, int length, int fileLength, dheader_t *header, int *lump ) {
	int	i;

	*lump = 0;
	if ( length < (int)sizeof( dheader_t ) ) {
		return BSP_TRUNCATED;
	}
	Com_Memcpy( header, data, sizeof( *header ) );
	for ( i = 0; i < (int)( sizeof( dheader_t ) / 4 ); i++ ) {
		( (int *)header )[i] = LittleLong( ( (int *)header )[i] );
	}
	if ( header->ident != BSP_IDENT ) {
		return BSP_NOT_BSP;
	}
	if ( header->version != BSP_VERSION ) {
		return BSP_WRONG_VERSION;
	}
	for ( i = 0; i < HEADER_LUMPS; i++ ) {
		*lump = i;
		if ( !Com_RangeInTable( header->lumps[i].fileofs, header->lumps[i].filelen, fileLength ) ) {
			return BSP_LUMP_OUTSIDE;
		}
		if ( header->lumps[i].fileofs & 3 ) {
			return BSP_LUMP_UNALIGNED;
		}
	}
	return BSP_OK;
}

/*
=================
BSP_Token

The next token of an entity string, as a game module's
trap_GetEntityToken has it (sv_game.c's G_GET_ENTITY_TOKEN): COM_Parse's,
copied into a buffer of MAX_TOKEN_CHARS, qfalse once nothing's left. In
text that isn't the whole lump, a token that reaches its end may be cut
short, a word longer or a lone / that begins a comment, so it's none yet
=================
*/
static qboolean BSP_Token( char **parse, char *token, qboolean whole ) {
	const char	*s = COM_Parse( parse );

	Q_strncpyz( token, s, MAX_TOKEN_CHARS );
	if ( !whole && ( !*parse || !**parse ) ) {
		return qfalse;
	}
	return *parse || s[0];
}

/*
=================
BSP_WorldspawnKey

A key of a map's first entity, its worldspawn, as game code reads it:
the first entity parsed as its key and value pairs are (G_ParseSpawnVars,
with its limits of BSP_SPAWN_VARS pairs and BSP_SPAWN_VAR_CHARS of their
text), its classname worldspawn (SP_worldspawn), and the key's value the
first pair's with that key, in any case (G_SpawnString). text is the
entity lump's start, nul-terminated; whole says it's the lump's whole
text, else the text that ends before the entity does says BSP_KEY_MORE,
for more of it. BSP_KEY_BAD for an entity the game would stop the map's
load over
=================
*/
bspKey_t BSP_WorldspawnKey( const char *text, qboolean whole, const char *key, char *value, int size ) {
	char		token[MAX_TOKEN_CHARS], keyname[MAX_TOKEN_CHARS], *parse = (char *)text;
	char		classname[MAX_TOKEN_CHARS] = "";
	qboolean	found = qfalse, named = qfalse;
	int			pairs = 0, chars = 0;

	value[0] = '\0';
	if ( !BSP_Token( &parse, token, whole ) ) {
		return whole ? BSP_KEY_BAD : BSP_KEY_MORE;
	}
	if ( token[0] != '{' ) {
		return BSP_KEY_BAD;
	}
	for ( ;; ) {
		if ( !BSP_Token( &parse, keyname, whole ) ) {
			return whole ? BSP_KEY_BAD : BSP_KEY_MORE;
		}
		if ( keyname[0] == '}' ) {
			break;
		}
		if ( !BSP_Token( &parse, token, whole ) ) {
			return whole ? BSP_KEY_BAD : BSP_KEY_MORE;
		}
		if ( token[0] == '}' ) {
			return BSP_KEY_BAD;
		}
		if ( pairs == BSP_SPAWN_VARS ) {
			return BSP_KEY_BAD;
		}
		chars += strlen( keyname ) + 1 + strlen( token ) + 1;
		if ( chars > BSP_SPAWN_VAR_CHARS ) {
			return BSP_KEY_BAD;
		}
		pairs++;
		if ( !named && !Q_stricmp( keyname, "classname" ) ) {
			Q_strncpyz( classname, token, sizeof( classname ) );
			named = qtrue;
		}
		if ( !found && !Q_stricmp( keyname, key ) ) {
			Q_strncpyz( value, token, size );
			found = qtrue;
		}
	}
	if ( Q_stricmp( classname, "worldspawn" ) ) {
		return BSP_KEY_BAD;
	}
	return found ? BSP_KEY_FOUND : BSP_KEY_ABSENT;
}
