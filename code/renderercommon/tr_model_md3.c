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
// tr_model_md3.c -- checks an MD3 file before either renderer loads it

#include "tr_common.h"
#include "../qcommon/qfiles.h"

/*
=================
R_MD3BlockInside

Whether count elements of size bytes from offset lie inside end bytes
=================
*/
static qboolean R_MD3BlockInside( int offset, int64_t count, int size, int end ) {
	return offset >= 0 && count >= 0 && offset <= end && count * size <= end - offset;
}

/*
=================
R_MD3NameTerminated
=================
*/
static qboolean R_MD3NameTerminated( const char *name ) {
	return memchr( name, '\0', MAX_QPATH ) != NULL;
}

/*
=================
R_ValidateMD3

Checks everything the loaders read from an MD3 file lies inside it,
before they read it: the header, frames and tags, then each surface,
its shaders, triangles and vertexes, that each triangle's indexes are
the surface's vertexes, and that every name is terminated. Warns and
returns qfalse if anything doesn't.
=================
*/
qboolean R_ValidateMD3( const void *buffer, int fileSize, const char *name ) {
	const byte		*file = buffer;
	md3Header_t		header;
	int				i, j, k;
	int				end, offset;

	if ( fileSize < (int)sizeof( header ) ) {
		ri.Printf( PRINT_WARNING, "R_ValidateMD3: %s is truncated\n", name );
		return qfalse;
	}
	Com_Memcpy( &header, file, sizeof( header ) );

	header.numFrames = LittleLong( header.numFrames );
	header.numTags = LittleLong( header.numTags );
	header.numSurfaces = LittleLong( header.numSurfaces );
	header.ofsFrames = LittleLong( header.ofsFrames );
	header.ofsTags = LittleLong( header.ofsTags );
	header.ofsSurfaces = LittleLong( header.ofsSurfaces );
	header.ofsEnd = LittleLong( header.ofsEnd );

	// everything is read up to ofsEnd, which opengl1 copies
	end = header.ofsEnd;
	if ( end < (int)sizeof( header ) || end > fileSize ) {
		ri.Printf( PRINT_WARNING, "R_ValidateMD3: %s has a bad end %i\n", name, end );
		return qfalse;
	}

	if ( header.numFrames < 1
		|| !R_MD3BlockInside( header.ofsFrames, header.numFrames, sizeof( md3Frame_t ), end )
		|| !R_MD3BlockInside( header.ofsTags, (int64_t)header.numTags * header.numFrames, sizeof( md3Tag_t ), end )
		|| !R_MD3BlockInside( header.ofsSurfaces, header.numSurfaces, sizeof( md3Surface_t ), end ) ) {
		ri.Printf( PRINT_WARNING, "R_ValidateMD3: %s has bad frames, tags or surfaces\n", name );
		return qfalse;
	}

	for ( i = 0 ; i < header.numTags * header.numFrames ; i++ ) {
		const md3Tag_t	*tag = (const md3Tag_t *)( file + header.ofsTags ) + i;

		if ( !R_MD3NameTerminated( tag->name ) ) {
			ri.Printf( PRINT_WARNING, "R_ValidateMD3: %s has an unterminated tag name\n", name );
			return qfalse;
		}
	}

	// each surface's offsets are from its start, and it ends where the
	// next starts
	offset = header.ofsSurfaces;
	for ( i = 0 ; i < header.numSurfaces ; i++ ) {
		md3Surface_t	surf;
		const byte		*surfBase;
		int				bytesToEnd;

		if ( !R_MD3BlockInside( offset, 1, sizeof( surf ), end ) ) {
			ri.Printf( PRINT_WARNING, "R_ValidateMD3: %s has surface %i past its end\n", name, i );
			return qfalse;
		}
		surfBase = file + offset;
		bytesToEnd = end - offset;
		Com_Memcpy( &surf, surfBase, sizeof( surf ) );

		surf.numFrames = LittleLong( surf.numFrames );
		surf.numShaders = LittleLong( surf.numShaders );
		surf.numVerts = LittleLong( surf.numVerts );
		surf.numTriangles = LittleLong( surf.numTriangles );
		surf.ofsTriangles = LittleLong( surf.ofsTriangles );
		surf.ofsShaders = LittleLong( surf.ofsShaders );
		surf.ofsSt = LittleLong( surf.ofsSt );
		surf.ofsXyzNormals = LittleLong( surf.ofsXyzNormals );
		surf.ofsEnd = LittleLong( surf.ofsEnd );

		// draws index a surface's vertexes by the model's frame
		if ( !R_MD3NameTerminated( surf.name ) || surf.numFrames < header.numFrames
			|| !R_MD3BlockInside( surf.ofsShaders, surf.numShaders, sizeof( md3Shader_t ), bytesToEnd )
			|| !R_MD3BlockInside( surf.ofsTriangles, surf.numTriangles, sizeof( md3Triangle_t ), bytesToEnd )
			|| !R_MD3BlockInside( surf.ofsSt, surf.numVerts, sizeof( md3St_t ), bytesToEnd )
			|| !R_MD3BlockInside( surf.ofsXyzNormals, (int64_t)surf.numVerts * surf.numFrames,
				sizeof( md3XyzNormal_t ), bytesToEnd )
			|| surf.ofsEnd < (int)sizeof( surf ) || surf.ofsEnd > bytesToEnd ) {
			ri.Printf( PRINT_WARNING, "R_ValidateMD3: %s has a bad surface %i\n", name, i );
			return qfalse;
		}

		for ( j = 0 ; j < surf.numShaders ; j++ ) {
			const md3Shader_t	*shader = (const md3Shader_t *)( surfBase + surf.ofsShaders ) + j;

			if ( !R_MD3NameTerminated( shader->name ) ) {
				ri.Printf( PRINT_WARNING, "R_ValidateMD3: %s has an unterminated shader name\n", name );
				return qfalse;
			}
		}

		for ( j = 0 ; j < surf.numTriangles ; j++ ) {
			md3Triangle_t	tri;

			Com_Memcpy( &tri, (const md3Triangle_t *)( surfBase + surf.ofsTriangles ) + j, sizeof( tri ) );
			for ( k = 0 ; k < 3 ; k++ ) {
				if ( (unsigned)LittleLong( tri.indexes[k] ) >= surf.numVerts ) {
					ri.Printf( PRINT_WARNING, "R_ValidateMD3: %s has a bad index on surface %i\n", name, i );
					return qfalse;
				}
			}
		}

		offset += surf.ofsEnd;
	}

	return qtrue;
}
