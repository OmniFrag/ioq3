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
// tr_model_mdr.c -- checks an MDR file before either renderer loads it

#include "tr_common.h"
#include "../qcommon/qfiles.h"

/*
=================
R_ValidateMDR

Checks everything the loaders read from an MDR file lies inside it,
before they read it, that its bones fit MDR_MAX_BONES, and that its
tags, weights and triangles index what it has. Warns and returns
qfalse if anything doesn't.
=================
*/
qboolean R_ValidateMDR( const void *buffer, int fileSize, const char *name ) {
	const byte		*file = buffer;
	mdrHeader_t		header;
	int64_t			frameSize, framesOffset, lodOffset, surfOffset, offset, loaded;
	int				end, i, j, k, l;

	if ( fileSize < (int)sizeof( header ) ) {
		ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s is truncated\n", name );
		return qfalse;
	}
	Com_Memcpy( &header, file, sizeof( header ) );

	header.numFrames = LittleLong( header.numFrames );
	header.numBones = LittleLong( header.numBones );
	header.ofsFrames = LittleLong( header.ofsFrames );
	header.numLODs = LittleLong( header.numLODs );
	header.ofsLODs = LittleLong( header.ofsLODs );
	header.numTags = LittleLong( header.numTags );
	header.ofsTags = LittleLong( header.ofsTags );
	header.ofsEnd = LittleLong( header.ofsEnd );

	// everything is read up to ofsEnd, which sizes the loaded model
	end = header.ofsEnd;
	if ( end < (int)sizeof( header ) || end > fileSize ) {
		ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has a bad end %i\n", name, end );
		return qfalse;
	}

	// draws lerp the bones into an array of MDR_MAX_BONES
	if ( header.numFrames < 1 || header.numBones < 0 || header.numBones > MDR_MAX_BONES || header.numLODs < 0 ) {
		ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has bad counts of frames, bones or levels of detail\n", name );
		return qfalse;
	}

	// a negative offset is to compressed frames, which the loader
	// byte-swaps in place as it uncompresses them
	if ( header.ofsFrames < 0 ) {
		frameSize = offsetof( mdrCompFrame_t, bones ) + header.numBones * sizeof( mdrCompBone_t );
		framesOffset = -(int64_t)header.ofsFrames;
	} else {
		frameSize = MDR_FRAME_SIZE( header.numBones );
		framesOffset = header.ofsFrames;
	}

	if ( !R_ModelBlockInside( framesOffset, header.numFrames, frameSize, end )
		|| !R_ModelBlockInside( header.ofsTags, header.numTags, sizeof( mdrTag_t ), end ) ) {
		ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has frames or tags past its end\n", name );
		return qfalse;
	}

	for ( i = 0 ; i < header.numTags ; i++ ) {
		mdrTag_t	tag;

		Com_Memcpy( &tag, (const mdrTag_t *)( file + header.ofsTags ) + i, sizeof( tag ) );
		if ( (unsigned)LittleLong( tag.boneIndex ) >= (unsigned)header.numBones ) {
			ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has a tag on a bad bone\n", name );
			return qfalse;
		}
	}

	// each level of detail's surfaces are from its start, each surface's
	// vertexes and triangles are from the surface's, and each ends where
	// the next starts. Levels of detail and surfaces can share data, but
	// the loader copies each into a model no bigger than the file, so
	// loaded counts what it would copy, which bounds the time spent here too
	lodOffset = header.ofsLODs;
	loaded = 0;
	for ( l = 0 ; l < header.numLODs ; l++ ) {
		mdrLOD_t	lod;

		loaded += sizeof( lod );
		if ( loaded > end ) {
			ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s is bigger loaded than its end\n", name );
			return qfalse;
		}
		if ( !R_ModelBlockInside( lodOffset, 1, sizeof( lod ), end ) ) {
			ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has level of detail %i past its end\n", name, l );
			return qfalse;
		}
		Com_Memcpy( &lod, file + lodOffset, sizeof( lod ) );

		lod.numSurfaces = LittleLong( lod.numSurfaces );
		lod.ofsSurfaces = LittleLong( lod.ofsSurfaces );
		lod.ofsEnd = LittleLong( lod.ofsEnd );

		if ( lod.numSurfaces < 0 || lod.ofsEnd < (int)sizeof( lod ) || lod.ofsEnd > end - lodOffset ) {
			ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has a bad level of detail %i\n", name, l );
			return qfalse;
		}

		surfOffset = lodOffset + lod.ofsSurfaces;
		for ( i = 0 ; i < lod.numSurfaces ; i++ ) {
			mdrSurface_t	surf;
			const byte		*surfBase;
			int64_t			bytesToEnd;

			if ( !R_ModelBlockInside( surfOffset, 1, sizeof( surf ), end ) ) {
				ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has surface %i past its end\n", name, i );
				return qfalse;
			}
			surfBase = file + surfOffset;
			bytesToEnd = end - surfOffset;
			Com_Memcpy( &surf, surfBase, sizeof( surf ) );

			surf.numVerts = LittleLong( surf.numVerts );
			surf.ofsVerts = LittleLong( surf.ofsVerts );
			surf.numTriangles = LittleLong( surf.numTriangles );
			surf.ofsTriangles = LittleLong( surf.ofsTriangles );
			surf.ofsEnd = LittleLong( surf.ofsEnd );

			if ( surf.numVerts < 0 || surf.numVerts >= SHADER_MAX_VERTEXES
				|| surf.numTriangles < 0 || surf.numTriangles >= SHADER_MAX_INDEXES / 3
				|| !R_ModelBlockInside( surf.ofsTriangles, surf.numTriangles, sizeof( mdrTriangle_t ), bytesToEnd )
				|| surf.ofsEnd < (int)sizeof( surf ) || surf.ofsEnd > bytesToEnd ) {
				ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has a bad surface %i\n", name, i );
				return qfalse;
			}

			// each vertex is its weights long
			offset = surf.ofsVerts;
			for ( j = 0 ; j < surf.numVerts ; j++ ) {
				mdrVertex_t	v;
				int			numWeights;

				if ( !R_ModelBlockInside( offset, 1, offsetof( mdrVertex_t, weights ), bytesToEnd ) ) {
					ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has a bad vertex on surface %i\n", name, i );
					return qfalse;
				}
				Com_Memcpy( &v, surfBase + offset, offsetof( mdrVertex_t, weights ) );
				numWeights = LittleLong( v.numWeights );
				offset += offsetof( mdrVertex_t, weights );

				if ( numWeights < 0 || numWeights > MDR_MAX_BONES
					|| !R_ModelBlockInside( offset, numWeights, sizeof( mdrWeight_t ), bytesToEnd ) ) {
					ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has a bad vertex on surface %i\n", name, i );
					return qfalse;
				}

				for ( k = 0 ; k < numWeights ; k++ ) {
					mdrWeight_t	w;

					Com_Memcpy( &w, (const mdrWeight_t *)( surfBase + offset ) + k, sizeof( w ) );
					if ( (unsigned)LittleLong( w.boneIndex ) >= (unsigned)header.numBones ) {
						ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has a weight on a bad bone on surface %i\n",
							name, i );
						return qfalse;
					}
				}
				offset += numWeights * sizeof( mdrWeight_t );
			}

			loaded += sizeof( surf ) + ( offset - surf.ofsVerts ) + surf.numTriangles * sizeof( mdrTriangle_t );
			if ( loaded > end ) {
				ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s is bigger loaded than its end\n", name );
				return qfalse;
			}

			for ( j = 0 ; j < surf.numTriangles ; j++ ) {
				mdrTriangle_t	tri;

				Com_Memcpy( &tri, (const mdrTriangle_t *)( surfBase + surf.ofsTriangles ) + j, sizeof( tri ) );
				for ( k = 0 ; k < 3 ; k++ ) {
					if ( (unsigned)LittleLong( tri.indexes[k] ) >= surf.numVerts ) {
						ri.Printf( PRINT_WARNING, "R_ValidateMDR: %s has a bad index on surface %i\n", name, i );
						return qfalse;
					}
				}
			}

			surfOffset += surf.ofsEnd;
		}

		lodOffset += lod.ofsEnd;
	}

	return qtrue;
}
