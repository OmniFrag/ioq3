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
// tr_ui2d.c -- the UI's retained 2D in opengl2, apart from what
// renderercommon/tr_ui2d_common.c does for both renderers: UI textures,
// geometry in buffers of its own drawn with the ui2d program
// (glsl/ui2d_*.glsl), gradients, and clip masks in the render target's
// stencil

#include "tr_local.h"
#include "tr_dsa.h"

// the ui2d program's uniforms that GLSL_InitUniforms doesn't know, and the
// values last given them, which are set only when they change
static struct {
	GLint		vertexPremultiplied, colorScale, textureMode;
	GLint		paintKind, paintRepeat, paintP0, paintP1, numStops, stopPos, stopColor;
	GLint		paintBox, paintRadii, paintFeather, msdfRange, msdfSourceTexel, textureSize;

	int			lastPremultiplied, lastTextureMode, lastTextureWidth, lastTextureHeight;
	float		lastColorScale;
	qboolean	painting;
	paint2D_t	lastPaint;
} ui2d;

// ui2d's u_TextureMode
enum {
	TEXMODE_NONE,
	TEXMODE_STRAIGHT,
	TEXMODE_PREMULTIPLIED,
	TEXMODE_RED
};

/*
===============
R_TextureR8

Whether A8 textures can be R8; else they're RGBA, premultiplied white
===============
*/
static qboolean R_TextureR8( void ) {
	return QGL_VERSION_ATLEAST( 3, 0 ) || QGLES_VERSION_ATLEAST( 3, 0 );
}

/*
===============
R_Paints2DAvailable

Gradients, where GLSL indexes uniform arrays freely, which OpenGL ES 2.0's
needn't; GLSL_InitGPUShaders leaves them out of ui2d there
===============
*/
qboolean R_Paints2DAvailable( void ) {
	return !qglesMajorVersion || qglesMajorVersion >= 3;
}

/*
===============
R_UpdateTexture2DGL

Replaces a rectangle of a texture. GL's rows start on 4 bytes, so an R8
rectangle of another width is copied into rows that do; without R8, A8
becomes RGBA
===============
*/
static void R_UpdateTexture2DGL( texture2D_t *t, int x, int y, int width, int height, const byte *pixels ) {
	image_t	*image = &t->image;
	byte	*copy = NULL;
	int		i, row, stride = PAD( width, 4 );

	if ( t->format == TEX2D_A8 && image->dataFormat == GL_RGBA ) {
		copy = ri.Hunk_AllocateTempMemory( width * height * 4 );
		for ( i = 0; i < width * height; i++ ) {
			copy[i * 4 + 0] = copy[i * 4 + 1] = copy[i * 4 + 2] = copy[i * 4 + 3] = pixels[i];
		}
		pixels = copy;
	} else if ( t->format == TEX2D_A8 && stride != width ) {
		copy = ri.Hunk_AllocateTempMemory( stride * height );
		for ( row = 0; row < height; row++ ) {
			Com_Memcpy( copy + row * stride, pixels + row * width, width );
		}
		pixels = copy;
	}

	qglTextureSubImage2DEXT( image->texnum, GL_TEXTURE_2D, 0, x, y, width, height, image->dataFormat, GL_UNSIGNED_BYTE, pixels );

	if ( copy ) {
		ri.Hunk_FreeTempMemory( copy );
	}
}

/*
===============
R_CreateTexture2DGL

A texture at exactly its size, clamped and filtered linearly
===============
*/
static void R_CreateTexture2DGL( texture2D_t *t, const byte *pixels ) {
	image_t	*image = &t->image;
	byte	*clear = NULL;
	int		bytes = image->width * image->height * R_TexelBytes2D( t->format );

	if ( t->format == TEX2D_A8 && R_TextureR8() ) {
		image->internalFormat = GL_R8;
		image->dataFormat = GL_RED;
	} else {
		// OpenGL ES 2.0 takes only unsized formats
		image->internalFormat = qglesMajorVersion == 2 ? GL_RGBA : GL_RGBA8;
		image->dataFormat = GL_RGBA;
	}
	image->dataType = GL_UNSIGNED_BYTE;

	qglGenTextures( 1, &image->texnum );
	qglTextureImage2DEXT( image->texnum, GL_TEXTURE_2D, 0, image->internalFormat, image->width, image->height, 0,
		image->dataFormat, GL_UNSIGNED_BYTE, NULL );
	qglTextureParameteriEXT( image->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTextureParameteriEXT( image->texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTextureParameteriEXT( image->texnum, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTextureParameteriEXT( image->texnum, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	if ( t->drawnInto && !pixels ) {
		return;
	}
	if ( !pixels ) {
		clear = ri.Hunk_AllocateTempMemory( bytes );
		Com_Memset( clear, 0, bytes );
	}
	R_UpdateTexture2DGL( t, 0, 0, image->width, image->height, pixels ? pixels : clear );
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
	if ( kind == HANDLE2D_TEXTURE ) {
		image_t	*image = &( (texture2D_t *)object )->image;

		qglDeleteTextures( 1, &image->texnum );
		GL_ForgetTexture( image->texnum );
	} else if ( kind == HANDLE2D_GEOMETRY ) {
		geometry2D_t	*g = object;
		vao_t			*vao = g->gpu;

		if ( !vao ) {
			return;
		}
		if ( glState.currentVao == vao ) {
			R_BindNullVao();
		}
		if ( vao->vao ) {
			qglDeleteVertexArrays( 1, &vao->vao );
		}
		qglDeleteBuffers( 1, &vao->vertexesVBO );
		qglDeleteBuffers( 1, &vao->indexesIBO );
		free( vao );
		g->gpu = NULL;
	}
}

/*
===============
R_ClipMask2DAvailable

The 2D target has stencil: the render target's (FBO_CreateRenderTarget),
or without framebuffers, the window's
===============
*/
static qboolean R_ClipMask2DAvailable( void ) {
	if ( glRefConfig.framebufferObject && tr.renderFbo ) {
		return tr.renderDepthImage && tr.renderDepthImage->internalFormat == GL_DEPTH24_STENCIL8;
	}
	return glConfig.stencilBits >= 8;
}

/*
===============
R_GetCaps2DRetained

What GetCaps2D says of the retained 2D
===============
*/
void R_GetCaps2DRetained( caps2D_t *caps ) {
	caps->textureA8 = R_TextureR8();
	caps->clipMask = R_ClipMask2DAvailable();
	caps->paints = caps->msdf = R_Paints2DAvailable();
	caps->maxGeometryVerts = MAX_GEOMETRY2D_VERTS;
	// the render target's samples, unless the views draw apart from it
	// (FBO_UseRenderTarget)
	caps->layers = R_Layers2DAvailable();
	caps->multisample = tr.renderFbo && tr.renderFbo == tr.renderTargetFbo && tr.renderTargetResolveFbo ?
		tr.multisample : 0;
}

/*
===============
R_InitUI2D

Finds the ui2d program's own uniforms, once GLSL_InitGPUShaders has made
it, and opens the store
===============
*/
void R_InitUI2D( void ) {
	GLuint	program = tr.ui2dShader.program;

	Com_Memset( &ui2d, 0, sizeof( ui2d ) );
	ui2d.vertexPremultiplied = qglGetUniformLocation( program, "u_VertexPremultiplied" );
	ui2d.colorScale = qglGetUniformLocation( program, "u_ColorScale" );
	ui2d.textureMode = qglGetUniformLocation( program, "u_TextureMode" );
	ui2d.paintKind = qglGetUniformLocation( program, "u_PaintKind" );
	ui2d.paintRepeat = qglGetUniformLocation( program, "u_PaintRepeat" );
	ui2d.paintP0 = qglGetUniformLocation( program, "u_PaintP0" );
	ui2d.paintP1 = qglGetUniformLocation( program, "u_PaintP1" );
	ui2d.numStops = qglGetUniformLocation( program, "u_NumStops" );
	ui2d.stopPos = qglGetUniformLocation( program, "u_StopPos" );
	ui2d.stopColor = qglGetUniformLocation( program, "u_StopColor" );
	ui2d.paintBox = qglGetUniformLocation( program, "u_PaintBox" );
	ui2d.paintRadii = qglGetUniformLocation( program, "u_PaintRadii" );
	ui2d.paintFeather = qglGetUniformLocation( program, "u_PaintFeather" );
	ui2d.msdfRange = qglGetUniformLocation( program, "u_MsdfRange" );
	ui2d.msdfSourceTexel = qglGetUniformLocation( program, "u_MsdfSourceTexel" );
	ui2d.textureSize = qglGetUniformLocation( program, "u_TextureSize" );

	// GL starts them at 0: what's set first is set
	ui2d.lastPremultiplied = ui2d.lastTextureMode = -1;
	ui2d.lastColorScale = -1;
	ui2d.painting = qtrue;

	R_InitStore2D();
}


/*
============================================================================

BACK END

============================================================================
*/

// where the tessellator took each vertex of a draw
static int	remap2D[MAX_GEOMETRY2D_VERTS], remapPass2D[MAX_GEOMETRY2D_VERTS], pass2D;

/*
===============
RB_Tess2DVertex

Takes a vertex into the tessellator once a pass, its index there returned;
moved by matrix on the CPU, when there's one
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

	if ( matrix ) {
		R_Transform2DPoint( matrix, in->xy, tess.xyz[n] );
	} else {
		tess.xyz[n][0] = in->xy[0];
		tess.xyz[n][1] = in->xy[1];
	}
	tess.xyz[n][2] = 0;
	tess.xyz[n][3] = 1;
	tess.texCoords[n][0] = in->st[0];
	tess.texCoords[n][1] = in->st[1];
	tess.color[n][0] = in->rgba[0] * 257;
	tess.color[n][1] = in->rgba[1] * 257;
	tess.color[n][2] = in->rgba[2] * 257;
	tess.color[n][3] = in->rgba[3] * 257;
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
	RB_BeginSurface( shader, 0, 0 );
	tess.twoSided = qtrue;
	pass2D++;
	for ( i = 0; i < numIndexes; i += 3 ) {
		if ( tess.numVertexes + 3 > SHADER_MAX_VERTEXES || tess.numIndexes + 3 > SHADER_MAX_INDEXES ) {
			RB_EndSurface();
			RB_BeginSurface( shader, 0, 0 );
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
RB_Paint2D

A paint's uniforms, when it isn't the one they hold; a distance field's
texture size, with it
===============
*/
static void RB_Paint2D( const paint2D_t *paint, const texture2D_t *texture ) {
	float	colors[MAX_PAINT2D_STOPS * 4];
	int		i;

	if ( !paint ) {
		if ( ui2d.painting ) {
			qglUniform1i( ui2d.paintKind, -1 );
			ui2d.painting = qfalse;
		}
		return;
	}
	if ( paint->kind == PAINT2D_MSDF && texture &&
		( texture->image.width != ui2d.lastTextureWidth || texture->image.height != ui2d.lastTextureHeight ) ) {
		qglUniform2f( ui2d.textureSize, texture->image.width, texture->image.height );
		ui2d.lastTextureWidth = texture->image.width;
		ui2d.lastTextureHeight = texture->image.height;
	}
	if ( ui2d.painting && !memcmp( paint, &ui2d.lastPaint, sizeof( *paint ) ) ) {
		return;
	}

	qglUniform1i( ui2d.paintKind, paint->kind );
	if ( paint->kind == PAINT2D_ROUNDED_BOX ) {
		qglUniform4f( ui2d.paintBox, paint->box[0], paint->box[1], paint->box[2], paint->box[3] );
		qglUniform4f( ui2d.paintRadii, paint->radii[0], paint->radii[1], paint->radii[2], paint->radii[3] );
		qglUniform1f( ui2d.paintFeather, paint->feather );
	} else if ( paint->kind == PAINT2D_MSDF ) {
		qglUniform1f( ui2d.msdfRange, paint->msdfRange );
		qglUniform2f( ui2d.msdfSourceTexel, paint->msdfSourceTexel[0], paint->msdfSourceTexel[1] );
	} else {
		for ( i = 0; i < paint->numStops * 4; i++ ) {
			colors[i] = paint->stopColor[i / 4][i % 4] / 255.0f;
		}
		qglUniform1i( ui2d.paintRepeat, paint->repeating );
		qglUniform2f( ui2d.paintP0, paint->p0[0], paint->p0[1] );
		qglUniform2f( ui2d.paintP1, paint->p1[0], paint->p1[1] );
		qglUniform1i( ui2d.numStops, paint->numStops );
		qglUniform1fv( ui2d.stopPos, paint->numStops, paint->stopPos );
		qglUniform1fv( ui2d.stopColor, paint->numStops * 4, colors );
	}
	ui2d.lastPaint = *paint;
	ui2d.painting = qtrue;
}

/*
===============
RB_Bind2DProgram

The ui2d program set for a state: its matrix, its colors' and texture's
alpha, its paint, and premultiplied blending. Clip masks are drawn
without color
===============
*/
static void RB_Bind2DProgram( const draw2DState_t *state, qboolean writeColor ) {
	texture2D_t		*texture = ( state->flags & TRIS2D_TEXTURE2D ) ? R_GetTexture2D( state->texture, qtrue ) : NULL;
	int				premultiplied = ( state->flags & TRIS2D_PREMULTIPLIED ) != 0;
	mat4_t			transform, mvp;
	int				mode = TEXMODE_NONE;

	R_Transform2DMatrix( state, transform );
	Mat4Multiply( glState.projection, transform, mvp );

	GLSL_BindProgram( &tr.ui2dShader );
	GLSL_SetUniformMat4( &tr.ui2dShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, mvp );

	if ( texture ) {
		if ( texture->format == TEX2D_RGBA8 ) {
			mode = TEXMODE_STRAIGHT;
		} else if ( texture->image.dataFormat == GL_RED ) {
			mode = TEXMODE_RED;
		} else {
			mode = TEXMODE_PREMULTIPLIED;
		}
		GL_BindToTMU( &texture->image, TB_COLORMAP );
	}
	if ( mode != ui2d.lastTextureMode ) {
		qglUniform1i( ui2d.textureMode, mode );
		ui2d.lastTextureMode = mode;
	}
	if ( premultiplied != ui2d.lastPremultiplied ) {
		qglUniform1i( ui2d.vertexPremultiplied, premultiplied );
		ui2d.lastPremultiplied = premultiplied;
	}
	if ( tr.frameLight != ui2d.lastColorScale ) {
		qglUniform1f( ui2d.colorScale, tr.frameLight );
		ui2d.lastColorScale = tr.frameLight;
	}
	RB_Paint2D( writeColor && ( state->flags & TRIS2D_PAINT ) ? R_GetPaint2D( state->paint, qtrue ) : NULL, texture );

	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	GL_Cull( CT_TWO_SIDED );
}

/*
===============
RB_Flush2DTess

Draws what the tessellator holds with the bound program
===============
*/
static void RB_Flush2DTess( void ) {
	if ( tess.numIndexes ) {
		RB_UpdateTessVao( ATTR_POSITION | ATTR_TEXCOORD | ATTR_COLOR );
		R_DrawElements( tess.numIndexes, tess.firstIndex );
		backEnd.pc.c_totalIndexes += tess.numIndexes;
	}
	tess.numVertexes = tess.numIndexes = 0;
	pass2D++;
}

/*
===============
RB_Draw2DTess

Draws an immediate batch with the ui2d program, streamed through the
tessellator's buffers in pieces it holds
===============
*/
static void RB_Draw2DTess( const polyVert2D_t *verts, const int *indexes, int numIndexes ) {
	int		i, j;

	tess.numVertexes = tess.numIndexes = 0;
	tess.firstIndex = 0;
	pass2D++;
	for ( i = 0; i < numIndexes; i += 3 ) {
		if ( tess.numVertexes + 3 > SHADER_MAX_VERTEXES || tess.numIndexes + 3 > SHADER_MAX_INDEXES ) {
			RB_Flush2DTess();
		}
		for ( j = 0; j < 3; j++ ) {
			tess.indexes[tess.numIndexes++] = RB_Tess2DVertex( verts, indexes[i + j], NULL );
		}
	}
	RB_Flush2DTess();
}

/*
===============
RB_Geometry2DVao

A geometry's buffers, made the first time it's drawn: its vertexes as
they are, 20 bytes each, and its indexes as GL's
===============
*/
static vao_t *RB_Geometry2DVao( geometry2D_t *g ) {
	vao_t		*vao = g->gpu;
	glIndex_t	*indexes;
	int			i;

	if ( vao ) {
		return vao;
	}
	vao = calloc( 1, sizeof( *vao ) );
	if ( !vao ) {
		return NULL;
	}
	Q_strncpyz( vao->name, "*geometry2D", sizeof( vao->name ) );

	if ( glRefConfig.vertexArrayObject ) {
		qglGenVertexArrays( 1, &vao->vao );
		qglBindVertexArray( vao->vao );
	}

	vao->vertexesSize = g->numVerts * sizeof( *g->verts );
	qglGenBuffers( 1, &vao->vertexesVBO );
	qglBindBuffer( GL_ARRAY_BUFFER, vao->vertexesVBO );
	qglBufferData( GL_ARRAY_BUFFER, vao->vertexesSize, g->verts, GL_STATIC_DRAW );

	vao->indexesSize = g->numIndexes * sizeof( *indexes );
	indexes = ri.Hunk_AllocateTempMemory( vao->indexesSize );
	for ( i = 0; i < g->numIndexes; i++ ) {
		indexes[i] = g->indexes[i];
	}
	qglGenBuffers( 1, &vao->indexesIBO );
	qglBindBuffer( GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO );
	qglBufferData( GL_ELEMENT_ARRAY_BUFFER, vao->indexesSize, indexes, GL_STATIC_DRAW );
	ri.Hunk_FreeTempMemory( indexes );

	vao->attribs[ATTR_INDEX_POSITION].enabled = 1;
	vao->attribs[ATTR_INDEX_POSITION].count = 2;
	vao->attribs[ATTR_INDEX_POSITION].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_POSITION].offset = offsetof( polyVert2D_t, xy );
	vao->attribs[ATTR_INDEX_COLOR].enabled = 1;
	vao->attribs[ATTR_INDEX_COLOR].count = 4;
	vao->attribs[ATTR_INDEX_COLOR].type = GL_UNSIGNED_BYTE;
	vao->attribs[ATTR_INDEX_COLOR].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_COLOR].offset = offsetof( polyVert2D_t, rgba );
	vao->attribs[ATTR_INDEX_TEXCOORD].enabled = 1;
	vao->attribs[ATTR_INDEX_TEXCOORD].count = 2;
	vao->attribs[ATTR_INDEX_TEXCOORD].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_TEXCOORD].offset = offsetof( polyVert2D_t, st );
	vao->attribs[ATTR_INDEX_POSITION].stride = vao->attribs[ATTR_INDEX_COLOR].stride =
		vao->attribs[ATTR_INDEX_TEXCOORD].stride = sizeof( polyVert2D_t );

	Vao_SetVertexPointers( vao );
	glState.currentVao = vao;

	g->gpu = vao;
	return vao;
}

/*
===============
RB_Draw2DProgram

Draws triangles with the ui2d program: a retained geometry from its
buffers, or an immediate batch through the tessellator's
===============
*/
static void RB_Draw2DProgram( const draw2DState_t *state, geometry2D_t *g, const polyVert2D_t *verts,
	const int *indexes, int numIndexes, qboolean writeColor ) {
	vao_t	*vao = g ? RB_Geometry2DVao( g ) : NULL;

	if ( g && !vao ) {
		return;
	}
	RB_Bind2DProgram( state, writeColor );
	if ( vao ) {
		R_BindVao( vao );
		qglDrawElements( GL_TRIANGLES, g->numIndexes, GL_INDEX_TYPE, NULL );
		backEnd.pc.c_totalIndexes += g->numIndexes;
	} else {
		RB_Draw2DTess( verts, indexes, numIndexes );
	}
}

/*
===============
RB_Begin2D

The 2D target, projection and the state's scissor, with what waits in the
tessellator drawn first, since these draw at once
===============
*/
static void RB_Begin2D( const draw2DState_t *state ) {
	if ( glRefConfig.framebufferObject ) {
		FBO_Bind( RB_Target2D() );
	}
	RB_SetGL2D();
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
	// backEnd.colorMask is kept only with framebuffers
	GLboolean	colorMask[4];

	qglGetBooleanv( GL_COLOR_WRITEMASK, colorMask );
	qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	RB_Draw2DProgram( state, g, g->verts, g->indexes, g->numIndexes, qfalse );
	qglColorMask( colorMask[0], colorMask[1], colorMask[2], colorMask[3] );
}

const renderer2D_t r_renderer2D = {
	RC_DRAW_2D, RC_CLIPMASK_2D, RC_DELETE_2D, RC_UPDATE_TEXTURE_2D, RC_LAYER_2D,
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
	int						numIndexes = cmd->numIndexes;
	geometry2D_t			*g = NULL;
	qboolean				clip;

	if ( cmd->geometry ) {
		g = R_GetGeometry2D( cmd->geometry, qtrue );
		if ( !g ) {
			return next;
		}
		verts = ( ( cmd->state.flags & TRIS2D_PREMULTIPLIED ) && R_ShaderState2D( &cmd->state ) ) ?
			R_StraightVerts2D( g ) : g->verts;
		indexes = g->indexes;
		numIndexes = g->numIndexes;
	}

	RB_Begin2D( &cmd->state );
	clip = R_BeginClipTest2D( &cmd->state );
	if ( R_ShaderState2D( &cmd->state ) ) {
		RB_Draw2DShader( &cmd->state, verts, indexes, numIndexes );
	} else {
		RB_Draw2DProgram( &cmd->state, g, verts, indexes, numIndexes, qtrue );
	}
	if ( clip ) {
		R_EndClipTest2D();
	}
	return next;
}
