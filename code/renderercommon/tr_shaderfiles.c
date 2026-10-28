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
// tr_shaderfiles.c -- the shader files as the renderer read them, and
// whichshader, which says where each definition of a shader comes from

#include "tr_common.h"

// a shader file, as ScanAndLoadShaderFiles read it
typedef struct {
	char		name[MAX_QPATH];
	const char	*source;		// its pak or directory (FS_FileSource)
	int			tier;			// the file system's (FS_FileTier)
	qboolean	loaded;			// its text was joined
} shaderFile_t;

static shaderFile_t	*shaderFiles;
static int			numShaderFiles;

/*
====================
R_ShaderFilesBegin

Room for the shader files the renderer is about to read, on the hunk
with their text
====================
*/
void R_ShaderFilesBegin( int count ) {
	shaderFiles = count ? ri.Hunk_Alloc( count * sizeof( *shaderFiles ), h_low ) : NULL;
	numShaderFiles = count;
}

/*
====================
R_ShaderFileNote

The shader file just read, its place in the list, and whether its text
was joined
====================
*/
void R_ShaderFileNote( int index, const char *name, int tier, qboolean loaded ) {
	shaderFile_t	*file = &shaderFiles[index];
	const char		*source = ri.FS_LastFileSource( );
	char			*copy;

	Q_strncpyz( file->name, name, sizeof( file->name ) );
	copy = ri.Hunk_Alloc( strlen( source ) + 1, h_low );
	strcpy( copy, source );
	file->source = copy;
	file->tier = tier;
	file->loaded = loaded;
}

/*
====================
R_WhichShader_f

whichshader <name>: each definition of a shader, in the order the
renderer joined the shader files' text (tier by tier, each tier's files
in reverse list order), the first the one in use; and when no file
defines it, the images it would be drawn from, by format, in the order
they're tried
====================
*/
void R_WhichShader_f( void ) {
	const char	*name = ri.Cmd_Argv( 1 );
	char		names[MAX_IMAGE_NAMES][MAX_QPATH], stripped[MAX_QPATH], *text, *p, *token;
	int			formats[MAX_IMAGE_NAMES], tiers[MAX_IMAGE_NAMES];
	int			tier, i, found = 0, numNames, moved;

	if ( !name[0] ) {
		ri.Printf( PRINT_ALL, "Usage: whichshader <name>\n" );
		return;
	}
	// a definition's name has no extension (R_FindShader)
	COM_StripExtension( name, stripped, sizeof( stripped ) );

	for ( tier = 0; tier < FS_TIERS; tier++ ) {
		for ( i = numShaderFiles - 1; i >= 0; i-- ) {
			if ( !shaderFiles[i].loaded || shaderFiles[i].tier != tier || ri.FS_ReadFile( shaderFiles[i].name, (void **)&text ) <= 0 ) {
				continue;
			}
			// the paks ranked since the renderer started can read another copy
			// now, whose text the renderer didn't join
			moved = Q_stricmp( ri.FS_LastFileSource( ), shaderFiles[i].source );
			for ( p = text; ( token = COM_NextShaderName( &p ) )[0]; ) {
				if ( !Q_stricmp( token, stripped ) ) {
					ri.Printf( PRINT_ALL, "%s %s in %s (%s)%s\n", found ? "  and in" : "In use: from", shaderFiles[i].name,
						shaderFiles[i].source, shaderFiles[i].tier ? "set aside" : "leading",
						moved ? ", as read when the renderer started: the paks have been ranked otherwise since" : "" );
					found++;
				}
			}
			ri.FS_FreeFile( text );
		}
	}
	if ( found ) {
		return;
	}

	ri.Printf( PRINT_ALL, "No shader file defines %s; its image, by format, in the order tried:\n", name );
	numNames = R_ImageNames( name, names, formats );
	for ( i = COM_NextByTier( ri.FS_FileTier, names, numNames, tiers, -1 ); i >= 0;
			i = COM_NextByTier( ri.FS_FileTier, names, numNames, tiers, i ) ) {
		const char	*source = ri.FS_FileSource( names[i] );

		if ( *source ) {
			ri.Printf( PRINT_ALL, "  %s in %s (%s)\n", names[i], source, tiers[i] > 0 ? "set aside" : "leading" );
			found++;
		}
	}
	if ( !found ) {
		ri.Printf( PRINT_ALL, "  none: it draws as the default\n" );
	}
}
