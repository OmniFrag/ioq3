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
// tr_2d.c -- what both renderers' DrawTriangles2D share: the checks a batch
// passes before it's copied into a render command, and the copy, which
// moves the vertexes so that the back end only appends them

#include "../qcommon/q_shared.h"
#include "tr_2d.h"

/*
=============
R_CheckTris2D

Whether tris can be drawn, with no flags but those in flags and at most
maxBytes of vertexes and indexes. If not, error says why, or is empty when
there's just nothing to draw.
=============
*/
qboolean R_CheckTris2D( const tris2D_t *tris, int flags, int maxBytes, char *error, int errorSize ) {
	const draw2DState_t	*state = &tris->state;
	const float			*m = state->transform;
	qboolean			quads = ( state->flags & TRIS2D_QUADS ) != 0;
	int64_t				bytes;
	int					i;

	error[0] = '\0';

	if ( state->flags & ~flags ) {
		Com_sprintf( error, errorSize, "flags 0x%x has some this renderer doesn't draw yet", state->flags );
		return qfalse;
	}
	if ( tris->numVerts < 0 || tris->numIndexes < 0 || ( quads && tris->numIndexes ) ) {
		Com_sprintf( error, errorSize, "%i vertexes and %i indexes%s", tris->numVerts, tris->numIndexes,
			quads ? ", as quads, which take none" : "" );
		return qfalse;
	}
	if ( quads ? tris->numVerts % 4 : tris->numIndexes % 3 ) {
		Com_sprintf( error, errorSize, quads ? "%i vertexes aren't whole quads" : "%i indexes aren't whole triangles",
			quads ? tris->numVerts : tris->numIndexes );
		return qfalse;
	}
	if ( quads ? !tris->numVerts : !tris->numIndexes ) {
		return qfalse;
	}
	if ( ( state->flags & TRIS2D_SCISSOR ) && ( state->scissor[2] <= 0 || state->scissor[3] <= 0 ) ) {
		return qfalse;
	}
	if ( !tris->verts || ( tris->numIndexes && !tris->indexes ) ) {
		Com_sprintf( error, errorSize, "its %s are NULL", tris->verts ? "indexes" : "vertexes" );
		return qfalse;
	}

	bytes = (int64_t)tris->numVerts * sizeof( polyVert2D_t ) + (int64_t)tris->numIndexes * sizeof( int );
	if ( bytes > maxBytes ) {
		Com_sprintf( error, errorSize, "%i vertexes and %i indexes are too many", tris->numVerts, tris->numIndexes );
		return qfalse;
	}

	for ( i = 0; i < tris->numIndexes; i++ ) {
		if ( (unsigned)tris->indexes[i] >= (unsigned)tris->numVerts ) {
			Com_sprintf( error, errorSize, "index %i is %i, of %i vertexes", i, tris->indexes[i], tris->numVerts );
			return qfalse;
		}
	}

	// w must stay above 0, NaN failing too: once when it's the same
	// everywhere, as it is for every transform but a perspective
	if ( state->flags & TRIS2D_TRANSFORM ) {
		for ( i = 0; i < tris->numVerts; i++ ) {
			float x = tris->verts[i].xy[0] + state->translate[0];
			float y = tris->verts[i].xy[1] + state->translate[1];

			if ( !( m[6] * x + m[7] * y + m[8] > 0 ) ) {
				Com_sprintf( error, errorSize, "the transform takes vertex %i behind the screen", i );
				return qfalse;
			}
			if ( !m[6] && !m[7] ) {
				break;
			}
		}
	}

	return qtrue;
}

/*
=============
R_ClipScissor2D

The part of scissor (x, y, width and height) inside a width by height
window, in out: qfalse when none of it is, as for a scissor of no area.
Any ints a caller gives stay in range, for the back end's sums.
=============
*/
qboolean R_ClipScissor2D( const int *scissor, int width, int height, int *out ) {
	int64_t		x0 = MAX( scissor[0], 0 ), y0 = MAX( scissor[1], 0 );
	int64_t		x1 = MIN( (int64_t)scissor[0] + scissor[2], width );
	int64_t		y1 = MIN( (int64_t)scissor[1] + scissor[3], height );

	if ( x1 <= x0 || y1 <= y0 ) {
		return qfalse;
	}

	out[0] = (int)x0;
	out[1] = (int)y0;
	out[2] = (int)( x1 - x0 );
	out[3] = (int)( y1 - y0 );
	return qtrue;
}

/*
=============
R_CopyTris2D

Copies a batch R_CheckTris2D passed into verts and indexes: the vertexes
moved by the translation, then by the transform
=============
*/
void R_CopyTris2D( const tris2D_t *tris, polyVert2D_t *verts, int *indexes ) {
	const draw2DState_t	*state = &tris->state;
	const float			*m = state->transform;
	int					i;

	Com_Memcpy( verts, tris->verts, tris->numVerts * sizeof( *verts ) );
	if ( tris->numIndexes ) {
		Com_Memcpy( indexes, tris->indexes, tris->numIndexes * sizeof( *indexes ) );
	}

	if ( state->flags & TRIS2D_TRANSFORM ) {
		// an affine transform's w is the same everywhere
		qboolean	affine = !m[6] && !m[7];
		float		scale = 1.0f / m[8];

		for ( i = 0; i < tris->numVerts; i++ ) {
			float x = verts[i].xy[0] + state->translate[0];
			float y = verts[i].xy[1] + state->translate[1];

			if ( !affine ) {
				scale = 1.0f / ( m[6] * x + m[7] * y + m[8] );
			}
			verts[i].xy[0] = ( m[0] * x + m[1] * y + m[2] ) * scale;
			verts[i].xy[1] = ( m[3] * x + m[4] * y + m[5] ) * scale;
		}
	} else if ( state->translate[0] || state->translate[1] ) {
		for ( i = 0; i < tris->numVerts; i++ ) {
			verts[i].xy[0] += state->translate[0];
			verts[i].xy[1] += state->translate[1];
		}
	}
}
