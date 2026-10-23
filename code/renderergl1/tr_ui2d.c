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
// tr_ui2d.c -- the UI's retained 2D in GL 1, apart from what
// renderercommon/tr_ui2d_common.c does for both renderers: UI textures,
// geometry drawn from renderer memory with vertex arrays, and clip masks
// in the window's stencil. Gradients and shapes aren't drawn here
// (caps2D_t's paints): the UI tessellates them.

#include "tr_local.h"

/*
===============
R_UpdateTexture2DGL

Replaces a rectangle of a texture. GL's rows start on 4 bytes, so an A8
rectangle of another width is copied into rows that do
===============
*/
static void R_UpdateTexture2DGL( texture2D_t *t, int x, int y, int width, int height, const byte *pixels ) {
	byte	*rows = NULL;
	int		row, stride = PAD( width, 4 );

	if ( t->format == TEX2D_A8 && stride != width ) {
		rows = ri.Hunk_AllocateTempMemory( stride * height );
		for ( row = 0; row < height; row++ ) {
			Com_Memcpy( rows + row * stride, pixels + row * width, width );
		}
		pixels = rows;
	}

	GL_SelectTexture( 0 );
	GL_Bind( &t->image );
	qglTexSubImage2D( GL_TEXTURE_2D, 0, x, y, width, height,
		t->format == TEX2D_A8 ? GL_LUMINANCE : GL_RGBA, GL_UNSIGNED_BYTE, pixels );

	if ( rows ) {
		ri.Hunk_FreeTempMemory( rows );
	}
}

/*
===============
R_CreateTexture2DGL

A texture at exactly its size, clamped and filtered linearly. A8 is an
intensity texture, so GL_MODULATE gives the vertex color times its
coverage in every channel: premultiplied white
===============
*/
static void R_CreateTexture2DGL( texture2D_t *t, const byte *pixels ) {
	image_t	*image = &t->image;
	int		bytes = image->width * image->height * R_TexelBytes2D( t->format );
	byte	*clear = NULL;

	if ( !pixels ) {
		clear = ri.Hunk_AllocateTempMemory( bytes );
		Com_Memset( clear, 0, bytes );
	}

	qglGenTextures( 1, &image->texnum );
	GL_SelectTexture( 0 );
	GL_Bind( image );
	image->internalFormat = t->format == TEX2D_A8 ? GL_INTENSITY8 : GL_RGBA8;
	qglTexImage2D( GL_TEXTURE_2D, 0, image->internalFormat, image->width, image->height, 0,
		t->format == TEX2D_A8 ? GL_LUMINANCE : GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	R_UpdateTexture2DGL( t, 0, 0, image->width, image->height, pixels ? pixels : clear );

	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, haveClampToEdge ? GL_CLAMP_TO_EDGE : GL_CLAMP );

	if ( clear ) {
		ri.Hunk_FreeTempMemory( clear );
	}
}

/*
===============
R_FreeGpu2D
===============
*/
static void R_FreeGpu2D( handle2DKind_t kind, void *object ) {
	int		i;

	if ( kind == HANDLE2D_TEXTURE ) {
		image_t	*image = &( (texture2D_t *)object )->image;

		qglDeleteTextures( 1, &image->texnum );
		// GL unbinds it, and a new texture may get its number
		for ( i = 0; i < (int)ARRAY_LEN( glState.currenttextures ); i++ ) {
			if ( glState.currenttextures[i] == image->texnum ) {
				glState.currenttextures[i] = 0;
			}
		}
	}
}

/*
===============
R_GetCaps2DRetained

What GetCaps2D says of the retained 2D: no paints, as the UI tessellates
gradients and shapes itself
===============
*/
void R_GetCaps2DRetained( caps2D_t *caps ) {
	caps->textureA8 = qtrue;
	caps->clipMask = glConfig.stencilBits >= 8;
	caps->maxGeometryVerts = MAX_GEOMETRY2D_VERTS;
}


/*
============================================================================

BACK END

============================================================================
*/

// the colors a draw gives GL, and where the tessellator took each vertex
static color4ub_t	colors2D[MAX_GEOMETRY2D_VERTS];
static int			remap2D[MAX_GEOMETRY2D_VERTS], remapPass2D[MAX_GEOMETRY2D_VERTS], pass2D;

/*
===============
RB_Tess2DVertex

Takes a vertex into the tessellator once a pass, its index there returned
===============
*/
static int RB_Tess2DVertex( const polyVert2D_t *verts, int v, const float *matrix ) {
	const polyVert2D_t	*in = &verts[v];
	int					n;

	if ( remapPass2D[v] == pass2D ) {
		return remap2D[v];
	}
	n = tess.numVertexes++;
	remapPass2D[v] = pass2D;
	remap2D[v] = n;

	R_Transform2DPoint( matrix, in->xy, tess.xyz[n] );
	tess.xyz[n][2] = 0;
	tess.texCoords[n][0][0] = in->st[0];
	tess.texCoords[n][0][1] = in->st[1];
	Byte4Copy( in->rgba, tess.vertexColors[n] );
	return n;
}

/*
===============
RB_Draw2DShader

Draws triangles with a shader's stages, through the tessellator, as an
immediate batch is: moved by the state on the CPU, and in pieces when
there are more than it holds
===============
*/
static void RB_Draw2DShader( const draw2DState_t *state, const polyVert2D_t *verts, const int *indexes, int numIndexes ) {
	shader_t	*shader = R_GetShaderByHandle( state->texture );
	float		matrix[16];
	int			i, j;

	R_Transform2DMatrix( state, matrix );

	backEnd.currentEntity = &backEnd.entity2D;
	RB_BeginSurface( shader, 0 );
	tess.twoSided = qtrue;
	pass2D++;
	for ( i = 0; i < numIndexes; i += 3 ) {
		if ( tess.numVertexes + 3 > SHADER_MAX_VERTEXES || tess.numIndexes + 3 > SHADER_MAX_INDEXES ) {
			RB_EndSurface();
			RB_BeginSurface( shader, 0 );
			tess.twoSided = qtrue;
			pass2D++;
		}
		for ( j = 0; j < 3; j++ ) {
			tess.indexes[tess.numIndexes++] = RB_Tess2DVertex( verts, indexes[i + j], matrix );
		}
	}
	RB_EndSurface();
}

/*
===============
RB_Colors2D

The colors to draw verts with: premultiplied when the texture or the
state is (a straight texture's vertex colors go straight), times the
identity light, as pictures are, for the overbright pass to undo; the
vertexes' own when nothing changes
===============
*/
static const byte *RB_Colors2D( const polyVert2D_t *verts, int numVerts, qboolean vertexPremul, qboolean premul ) {
	int		i, j;

	if ( premul == vertexPremul && tr.identityLight == 1.0f ) {
		return verts->rgba;
	}
	for ( i = 0; i < numVerts; i++ ) {
		const byte	*in = verts[i].rgba;
		int			a = in[3];

		for ( j = 0; j < 3; j++ ) {
			int		c = in[j];

			if ( premul && !vertexPremul ) {
				c = ( c * a + 127 ) / 255;
			} else if ( !premul && vertexPremul ) {
				c = R_StraightChannel2D( c, a );
			}
			colors2D[i][j] = c * tr.identityLight;
		}
		colors2D[i][3] = a;
	}
	return colors2D[0];
}

/*
===============
RB_Draw2DArrays

Draws triangles with the UI's own path: vertex arrays straight from verts,
moved by the state's matrix in GL, so a perspective's texture coordinates
interpolate as they should. Clip masks are drawn without color
===============
*/
static void RB_Draw2DArrays( const draw2DState_t *state, const polyVert2D_t *verts, int numVerts, const int *indexes,
	int numIndexes, qboolean writeColor ) {
	texture2D_t	*texture = ( state->flags & TRIS2D_TEXTURE2D ) ? R_GetTexture2D( state->texture, qtrue ) : NULL;
	qboolean	vertexPremul = ( state->flags & TRIS2D_PREMULTIPLIED ) != 0;
	qboolean	premul = texture ? texture->format != TEX2D_RGBA8 : vertexPremul;
	const byte	*colors = writeColor ? RB_Colors2D( verts, numVerts, vertexPremul, premul ) : NULL;
	float		matrix[16];

	GL_SelectTexture( 0 );
	GL_Bind( texture ? &texture->image : tr.whiteImage );
	GL_TexEnv( GL_MODULATE );
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA |
		( premul ? GLS_SRCBLEND_ONE : GLS_SRCBLEND_SRC_ALPHA ) );

	R_Transform2DMatrix( state, matrix );
	qglMatrixMode( GL_MODELVIEW );
	qglLoadMatrixf( matrix );

	// the tessellator's arrays that stay enabled, which hold fewer
	// vertexes than these indexes reach: the lightmap's texture
	// coordinates (DrawMultitextured), and the colors
	if ( qglActiveTextureARB ) {
		GL_SelectTexture( 1 );
		qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
		GL_SelectTexture( 0 );
	}
	qglVertexPointer( 2, GL_FLOAT, sizeof( *verts ), verts->xy );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglTexCoordPointer( 2, GL_FLOAT, sizeof( *verts ), verts->st );
	if ( colors ) {
		qglEnableClientState( GL_COLOR_ARRAY );
		qglColorPointer( 4, GL_UNSIGNED_BYTE, colors == verts->rgba ? sizeof( *verts ) : 0, colors );
	} else {
		qglDisableClientState( GL_COLOR_ARRAY );
		qglColor4f( 1, 1, 1, 1 );
	}

	qglDrawElements( GL_TRIANGLES, numIndexes, GL_UNSIGNED_INT, indexes );

	// nothing else draws from these arrays, which a delete frees
	qglDisableClientState( GL_COLOR_ARRAY );
	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	qglVertexPointer( 3, GL_FLOAT, 16, tess.xyz );
	qglLoadIdentity();
	backEnd.pc.c_totalIndexes += numIndexes;
}

/*
===============
RB_Begin2D

The 2D projection and the state's scissor, with what waits in the
tessellator drawn first, since these draw at once
===============
*/
static void RB_Begin2D( const draw2DState_t *state ) {
	if ( !backEnd.projection2D ) {
		RB_SetGL2D();
	}
	RB_Scissor2D( ( state->flags & TRIS2D_SCISSOR ) ? state->scissor : NULL );
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}
}

/*
===============
RB_Draw2DUnseen

A geometry drawn into the stencil alone, for a clip mask
===============
*/
static void RB_Draw2DUnseen( const draw2DState_t *state, geometry2D_t *g ) {
	GLboolean	colorMask[4];

	qglGetBooleanv( GL_COLOR_WRITEMASK, colorMask );
	qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	RB_Draw2DArrays( state, g->verts, g->numVerts, g->indexes, g->numIndexes, qfalse );
	qglColorMask( colorMask[0], colorMask[1], colorMask[2], colorMask[3] );
}

const renderer2D_t r_renderer2D = {
	RC_DRAW_2D, RC_CLIPMASK_2D, RC_DELETE_2D, RC_UPDATE_TEXTURE_2D, -1,
	MAX_RENDER_COMMANDS,
	R_GetCaps2DRetained, R_CreateTexture2DGL, R_UpdateTexture2DGL, R_FreeGpu2D,
	RB_Begin2D, RB_Draw2DUnseen
};

/*
===============
RB_Draw2D
===============
*/
const void *RB_Draw2D( const void *data ) {
	const draw2DCommand_t	*cmd = (const draw2DCommand_t *)data;
	const polyVert2D_t		*verts = (const polyVert2D_t *)( cmd + 1 );
	const int				*indexes = (const int *)( verts + cmd->numVerts );
	const void				*next = indexes + cmd->numIndexes;
	int						numVerts = cmd->numVerts, numIndexes = cmd->numIndexes;
	qboolean				clip;

	if ( cmd->geometry ) {
		geometry2D_t	*g = R_GetGeometry2D( cmd->geometry, qtrue );

		if ( !g ) {
			return next;
		}
		verts = ( ( cmd->state.flags & TRIS2D_PREMULTIPLIED ) && R_ShaderState2D( &cmd->state ) ) ?
			R_StraightVerts2D( g ) : g->verts;
		indexes = g->indexes;
		numVerts = g->numVerts;
		numIndexes = g->numIndexes;
	}

	RB_Begin2D( &cmd->state );
	clip = R_BeginClipTest2D( &cmd->state );
	if ( R_ShaderState2D( &cmd->state ) ) {
		RB_Draw2DShader( &cmd->state, verts, indexes, numIndexes );
	} else {
		RB_Draw2DArrays( &cmd->state, verts, numVerts, indexes, numIndexes, qtrue );
	}
	if ( clip ) {
		R_EndClipTest2D();
	}
	return next;
}
