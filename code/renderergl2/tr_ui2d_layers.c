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
// tr_ui2d_layers.c -- the UI's layers and their filters (tr_public.h's
// PushLayer2D to DeleteFilter2D): framebuffers like the 2D target, its
// size and samples, sharing its depth and stencil, so a clip mask holds
// across them; resolved into a texture to be composited, through the
// ui2dfilter program's passes (glsl/ui2dfilter_*) over the composite's
// box alone; and saved into UI textures (SaveLayer2D)

#include "tr_local.h"
#include "tr_fbo.h"
#include "tr_dsa.h"

// a framebuffer and the texture it draws or resolves into
typedef struct {
	FBO_t	fbo;			// drawn into: the texture's, or with samples a renderbuffer's
	FBO_t	resolveFbo;		// with samples, the texture's
	image_t	image;
} target2D_t;

// blur draws at a smaller size for each halving of a large sigma
#define BLUR_LEVELS		5
#define BLUR_MAX_SIGMA	4.0f
#define BLUR_MAX_RADIUS	12		// ui2dfilter_fp.glsl's u_BlurWeights

// the layers are freed after this many frames without a layer command
#define LAYERS2D_IDLE_FRAMES	300

static struct {
	int			width, height, samples;		// the 2D target's, which the layers were made for
	FBO_t		*target;					// whose depth and stencil they share
	int			depth;						// the layers pushed, in the back end
	int			idleFrames;
	target2D_t	layers[MAX_LAYERS2D];
	// what of each pushed layer has been cleared since its push, in the 2D
	// target's pixels, y down: nothing, a box, or all of it (RB_ClearLayer2D)
	ivec4_t		cleared[MAX_LAYERS2D];
	// single-sampled, each level half the size of the one before; three
	// at each, for a drop shadow's
	target2D_t	scratch[BLUR_LEVELS][3];
	FBO_t		saveFbo;					// a saved layer's texture is drawn through
	// the glass filters' states, two texels each: this frame's and the
	// frame before's take turns (RB_Glass2D), by the frames counted from 1,
	// and each glass's last frame drawn, and whether it drew the one before
	target2D_t	glassStates[2];
	int			glassFrame, glassTime;
	float		glassDt;					// seconds the frame before took
	struct {
		int			serial;		// the glass's (filter2D_t's glassSerial)
		int			frame;		// the last it drew in, counted from 1; 0 for none
		qboolean	start;		// this frame, as it didn't draw the one before
	} glass[MAX_FILTERS2D];
} ui2dLayers;

// the ui2dfilter program's uniforms that GLSL_InitUniforms doesn't know
static struct {
	GLint	filterMode, colorMatrix, colorOffset, colorScale, blurDirection, blurRadius, blurWeights, maskMap, maskTransform, maskChannel;
	GLint	clipShape, clipBox, clipRadii;
} filterUniforms;

// and ui2dglass's
static struct {
	GLint	pass, colorScale, box, radii, rim, light, darkColor, lightColor, tint, mode, glow, deform, invTargetRes,
			stateMap, stateCoord, stateStep, smoothing, fade, start;
} glassUniforms;

// ui2dfilter's u_FilterMode
enum {
	FILTERMODE_COPY,
	FILTERMODE_COLOR_MATRIX,
	FILTERMODE_BLUR,
	FILTERMODE_SHADOW,
	FILTERMODE_MASK,
	FILTERMODE_BLUR_MATRIX
};

static const vec4_t	transparent = { 0, 0, 0, 0 };

/*
===============
R_Layers2DAvailable

Layers need framebuffers, and the ui2dfilter program, which
GLSL_InitGPUShaders makes where GLSL allows it
===============
*/
qboolean R_Layers2DAvailable( void ) {
	return glRefConfig.framebufferObject && tr.renderFbo && tr.ui2dFilterShader.program;
}

/*
===============
R_MakeTarget2D

A target width by height, RGBA8; drawn into a renderbuffer with samples;
with the 2D target's depth and stencil when shared
===============
*/
static void R_MakeTarget2D( target2D_t *t, int width, int height, int samples, FBO_t *shared ) {
	FBO_t	*texFbo = samples ? &t->resolveFbo : &t->fbo;

	Com_Memset( t, 0, sizeof( *t ) );
	t->image.width = t->image.uploadWidth = width;
	t->image.height = t->image.uploadHeight = height;
	t->image.internalFormat = GL_RGBA8;
	t->image.dataFormat = GL_RGBA;
	t->image.dataType = GL_UNSIGNED_BYTE;
	qglGenTextures( 1, &t->image.texnum );
	qglTextureImage2DEXT( t->image.texnum, GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	qglTextureParameteriEXT( t->image.texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTextureParameteriEXT( t->image.texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTextureParameteriEXT( t->image.texnum, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTextureParameteriEXT( t->image.texnum, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	Q_strncpyz( texFbo->name, "*layer2D", sizeof( texFbo->name ) );
	texFbo->width = width;
	texFbo->height = height;
	qglGenFramebuffers( 1, &texFbo->frameBuffer );
	FBO_AttachImage( texFbo, &t->image, GL_COLOR_ATTACHMENT0, 0 );

	if ( samples ) {
		Q_strncpyz( t->fbo.name, "*layer2DSamples", sizeof( t->fbo.name ) );
		t->fbo.width = width;
		t->fbo.height = height;
		qglGenFramebuffers( 1, &t->fbo.frameBuffer );
		FBO_CreateBuffer( &t->fbo, GL_RGBA8, 0, samples );
		R_CheckFBO( &t->resolveFbo );
	}
	if ( shared ) {
		if ( shared->packedDepthStencilBuffer ) {
			qglNamedFramebufferRenderbufferEXT( t->fbo.frameBuffer, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, shared->packedDepthStencilBuffer );
			qglNamedFramebufferRenderbufferEXT( t->fbo.frameBuffer, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, shared->packedDepthStencilBuffer );
		} else {
			FBO_AttachImage( &t->fbo, tr.renderDepthImage, GL_DEPTH_STENCIL_ATTACHMENT, 0 );
		}
	}
	R_CheckFBO( &t->fbo );
}

static void R_FreeFbo2D( FBO_t *fbo ) {
	if ( fbo->frameBuffer ) {
		if ( glState.currentFBO == fbo ) {
			FBO_Bind( NULL );
		}
		qglDeleteFramebuffers( 1, &fbo->frameBuffer );
	}
	if ( fbo->colorBuffers[0] ) {
		qglDeleteRenderbuffers( 1, &fbo->colorBuffers[0] );
	}
}

static void R_FreeTarget2D( target2D_t *t ) {
	if ( !t->image.texnum ) {
		return;
	}
	R_FreeFbo2D( &t->fbo );
	R_FreeFbo2D( &t->resolveFbo );
	qglDeleteTextures( 1, &t->image.texnum );
	GL_ForgetTexture( t->image.texnum );
	Com_Memset( t, 0, sizeof( *t ) );
}

/*
===============
R_ShutdownLayers2D

Frees the layers and their scratch targets: as the renderer stops, as the
2D target changes, or once they've gone unused for a while
===============
*/
void R_ShutdownLayers2D( void ) {
	int		i, j;

	for ( i = 0; i < MAX_LAYERS2D; i++ ) {
		R_FreeTarget2D( &ui2dLayers.layers[i] );
	}
	for ( i = 0; i < BLUR_LEVELS; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			R_FreeTarget2D( &ui2dLayers.scratch[i][j] );
		}
	}
	for ( i = 0; i < 2; i++ ) {
		R_FreeTarget2D( &ui2dLayers.glassStates[i] );
	}
	R_FreeFbo2D( &ui2dLayers.saveFbo );
	Com_Memset( &ui2dLayers, 0, sizeof( ui2dLayers ) );
}

/*
===============
R_InitLayers2D

Finds the ui2dfilter program's own uniforms, once GLSL_InitGPUShaders has
made it. The layers are made when first pushed
===============
*/
void R_InitLayers2D( void ) {
	GLuint	program = tr.ui2dFilterShader.program;

	if ( !program ) {
		return;
	}
	filterUniforms.filterMode = qglGetUniformLocation( program, "u_FilterMode" );
	filterUniforms.colorMatrix = qglGetUniformLocation( program, "u_ColorMatrix" );
	filterUniforms.colorOffset = qglGetUniformLocation( program, "u_ColorOffset" );
	filterUniforms.colorScale = qglGetUniformLocation( program, "u_ColorScale" );
	filterUniforms.blurDirection = qglGetUniformLocation( program, "u_BlurDirection" );
	filterUniforms.blurRadius = qglGetUniformLocation( program, "u_BlurRadius" );
	filterUniforms.blurWeights = qglGetUniformLocation( program, "u_BlurWeights" );
	filterUniforms.maskMap = qglGetUniformLocation( program, "u_MaskMap" );
	filterUniforms.maskTransform = qglGetUniformLocation( program, "u_MaskTransform" );
	filterUniforms.maskChannel = qglGetUniformLocation( program, "u_MaskChannel" );
	filterUniforms.clipShape = qglGetUniformLocation( program, "u_ClipShape" );
	filterUniforms.clipBox = qglGetUniformLocation( program, "u_ClipBox" );
	filterUniforms.clipRadii = qglGetUniformLocation( program, "u_ClipRadii" );
	GLSL_BindProgram( &tr.ui2dFilterShader );
	qglUniform1i( filterUniforms.maskMap, TB_LIGHTMAP );

	program = tr.ui2dGlassShader.program;
	if ( !program ) {
		return;
	}
	glassUniforms.pass = qglGetUniformLocation( program, "u_GlassPass" );
	glassUniforms.colorScale = qglGetUniformLocation( program, "u_ColorScale" );
	glassUniforms.box = qglGetUniformLocation( program, "u_GlassBox" );
	glassUniforms.radii = qglGetUniformLocation( program, "u_GlassRadii" );
	glassUniforms.rim = qglGetUniformLocation( program, "u_GlassRim" );
	glassUniforms.light = qglGetUniformLocation( program, "u_GlassLight" );
	glassUniforms.darkColor = qglGetUniformLocation( program, "u_GlassDarkColor" );
	glassUniforms.lightColor = qglGetUniformLocation( program, "u_GlassLightColor" );
	glassUniforms.tint = qglGetUniformLocation( program, "u_GlassTint" );
	glassUniforms.mode = qglGetUniformLocation( program, "u_GlassMode" );
	glassUniforms.glow = qglGetUniformLocation( program, "u_GlassGlow" );
	glassUniforms.deform = qglGetUniformLocation( program, "u_GlassDeform" );
	glassUniforms.invTargetRes = qglGetUniformLocation( program, "u_InvTargetRes" );
	glassUniforms.stateMap = qglGetUniformLocation( program, "u_GlassStateMap" );
	glassUniforms.stateCoord = qglGetUniformLocation( program, "u_GlassStateCoord" );
	glassUniforms.stateStep = qglGetUniformLocation( program, "u_GlassStateStep" );
	glassUniforms.smoothing = qglGetUniformLocation( program, "u_GlassSmoothing" );
	glassUniforms.fade = qglGetUniformLocation( program, "u_GlassFade" );
	glassUniforms.start = qglGetUniformLocation( program, "u_GlassStart" );
	GLSL_BindProgram( &tr.ui2dGlassShader );
	qglUniform1i( glassUniforms.stateMap, TB_LIGHTMAP );
}


/*
============================================================================

BACK END

============================================================================
*/

/*
===============
RB_ForgetScissor2D

The 2D's scissor state is no longer what GL has, after passes that set
their own; the next 2D draw sets it again as it binds its target
===============
*/
static void RB_ForgetScissor2D( void ) {
	backEnd.scissor2D = qtrue;
	VectorSet4( backEnd.scissorRect2D, -1, -1, -1, -1 );
}

/*
===============
RB_ClearBoxes2D

Clears boxes (x, y, width and height, y down) of the bound framebuffer to
transparent, or their alpha alone to 1 (opaque), through the 2D's scissor,
which a layer command's own passes and blits may have left other than its
state says
===============
*/
static void RB_ClearBoxes2D( ivec4_t *boxes, int numBoxes, qboolean opaque ) {
	int		i;

	RB_ForgetScissor2D();
	qglColorMask( !opaque, !opaque, !opaque, GL_TRUE );
	qglClearColor( 0, 0, 0, opaque );
	for ( i = 0; i < numBoxes; i++ ) {
		if ( boxes[i][2] > 0 && boxes[i][3] > 0 ) {
			RB_Scissor2D( boxes[i] );
			qglClear( GL_COLOR_BUFFER_BIT );
		}
	}
	qglColorMask( !backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3] );
}

/*
===============
RB_OpaqueWindow2D

The window's alpha set to 1 within box, before a composite or a save reads
it: the window is opaque, but the 3D view leaves its alpha as its shaders
wrote it (masks a later stage reads with GL_DST_ALPHA, and dynamic lights
adding to them, past 1 in a float target), which the filters and the glass
would take for how much of each texel is there. Nothing after the view
reads the window's alpha
===============
*/
static void RB_OpaqueWindow2D( const int box[4] ) {
	ivec4_t	b;

	VectorCopy4( box, b );
	FBO_Bind( tr.renderFbo );
	RB_ClearBoxes2D( &b, 1, qtrue );
}

static qboolean RB_BoxWithin2D( const int in[4], const int out[4] ) {
	return in[0] >= out[0] && in[1] >= out[1] && in[0] + in[2] <= out[0] + out[2] && in[1] + in[3] <= out[1] + out[3];
}

/*
===============
RB_ClearLayer2D

Clears what of a pushed layer box covers, or all of it without one, that
hasn't been cleared since its push: a layer isn't cleared as it's pushed,
as a backdrop's is only composited into within a box. The first box is
cleared alone; past it, the rest is cleared in bands around it. (Dropping
the old contents first with glInvalidateFramebuffer was slower, on an RTX
5070 Ti without MSAA and in Chrome on an M4)
===============
*/
static void RB_ClearLayer2D( int layer, const int box[4] ) {
	int		w = ui2dLayers.width, h = ui2dLayers.height;
	int		*c;
	ivec4_t	b;

	if ( !layer ) {
		return;
	}
	c = ui2dLayers.cleared[layer - 1];
	if ( c[2] == w && c[3] == h ) {
		return;
	}
	if ( box ) {
		b[0] = MAX( 0, box[0] );
		b[1] = MAX( 0, box[1] );
		b[2] = MIN( w, (int64_t)box[0] + box[2] ) - b[0];
		b[3] = MIN( h, (int64_t)box[1] + box[3] ) - b[1];
	} else {
		VectorSet4( b, 0, 0, w, h );
	}
	if ( b[2] <= 0 || b[3] <= 0 || RB_BoxWithin2D( b, c ) ) {
		return;
	}

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}
	FBO_Bind( &ui2dLayers.layers[layer - 1].fbo );
	if ( !c[2] && ( b[2] < w || b[3] < h ) ) {
		RB_ClearBoxes2D( &b, 1, qfalse );
		VectorCopy4( b, c );
	} else if ( !c[2] ) {
		FBO_ClearWhole( GL_COLOR_BUFFER_BIT, transparent );
		VectorSet4( c, 0, 0, w, h );
	} else {
		// the bands above and below the cleared box, then beside it
		ivec4_t	bands[4] = {
			{ 0, 0, w, c[1] },
			{ 0, c[1] + c[3], w, h - c[1] - c[3] },
			{ 0, c[1], c[0], c[3] },
			{ c[0] + c[2], c[1], w - c[0] - c[2], c[3] }
		};

		RB_ClearBoxes2D( bands, 4, qfalse );
		VectorSet4( c, 0, 0, w, h );
	}
}

/*
===============
RB_BindTarget2D

Binds what the 2D draws and clip masks go into: the top layer, or the 2D
target. A draw in color clears the rest of the top layer first; a clip
mask writes only the stencil, so it clears nothing
===============
*/
void RB_BindTarget2D( qboolean color ) {
	if ( color ) {
		RB_ClearLayer2D( ui2dLayers.depth, NULL );
	}
	FBO_Bind( ui2dLayers.depth ? &ui2dLayers.layers[ui2dLayers.depth - 1].fbo : tr.renderFbo );
}

/*
===============
RB_FitLayers2D

Makes the layers again for the 2D target, as a layer command comes with
none pushed, when it's another, or its size or samples changed
===============
*/
static void RB_FitLayers2D( void ) {
	FBO_t	*target = tr.renderFbo;
	int		samples = target->packedDepthStencilBuffer ? tr.multisample : 0;

	if ( ui2dLayers.target != target || ui2dLayers.width != target->width || ui2dLayers.height != target->height ||
		ui2dLayers.samples != samples ) {
		R_ShutdownLayers2D();
		ui2dLayers.target = target;
		ui2dLayers.width = target->width;
		ui2dLayers.height = target->height;
		ui2dLayers.samples = samples;
	}
}

/*
===============
RB_Scratch2D

A scratch target at a level, made when first needed, other than the ones
in use; there are always enough. It's cleared, so what a pass reads past
the box it drew is transparent, not an earlier composite's, but only
within read (x, y, width and height in the 2D target's pixels, y down):
a whole clear costs a whole target's pass on a tile-based GPU. read is
what the composite's passes can read, its blurs' 3 sigma among it, so
three texels more cover the rest at this level: a blur's radius rounded
up, a pass's box rounded to its texels, and a linear filter's texel
===============
*/
static target2D_t *RB_Scratch2D( int level, const image_t *inUse1, const image_t *inUse2, const int read[4] ) {
	const int	margin = 3;
	target2D_t	*t = ui2dLayers.scratch[level];
	int			x0, y0, x1, y1;

	while ( &t->image == inUse1 || &t->image == inUse2 ) {
		t++;
	}
	if ( !t->image.texnum ) {
		R_MakeTarget2D( t, MAX( 1, ui2dLayers.width >> level ), MAX( 1, ui2dLayers.height >> level ), 0, NULL );
	}
	FBO_Bind( &t->fbo );
	x0 = MAX( 0, ( read[0] >> level ) - margin );
	y0 = MAX( 0, ( read[1] >> level ) - margin );
	x1 = MIN( t->fbo.width, ( ( read[0] + read[2] ) >> level ) + margin + 1 );
	y1 = MIN( t->fbo.height, ( ( read[1] + read[3] ) >> level ) + margin + 1 );
	if ( x0 > 0 || y0 > 0 || x1 < t->fbo.width || y1 < t->fbo.height ) {
		ivec4_t	box = { x0, y0, x1 - x0, y1 - y0 };

		RB_ClearBoxes2D( &box, 1, qfalse );
	} else {
		FBO_ClearWhole( GL_COLOR_BUFFER_BIT, transparent );
	}
	return t;
}

/*
===============
RB_Quad2D

Draws src through a program, its mode or pass set already, as a quad into
the bound framebuffer, in its 2D projection
===============
*/
static void RB_Quad2D( shaderProgram_t *program, image_t *src, const vec4_t color, vec4_t verts[4], vec2_t texCoords[4],
	int blend ) {
	vec2_t	invTexRes;

	VectorSet2( invTexRes, 1.0f / src->width, 1.0f / src->height );
	GLSL_BindProgram( program );
	GLSL_SetUniformMat4( program, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.projection );
	GLSL_SetUniformVec4( program, UNIFORM_COLOR, color );
	GLSL_SetUniformVec2( program, UNIFORM_INVTEXRES, invTexRes );
	GL_BindToTMU( src, TB_COLORMAP );
	GL_State( GLS_DEPTHTEST_DISABLE | blend );
	GL_Cull( CT_TWO_SIDED );
	RB_InstantQuad2( verts, texCoords );
}

/*
===============
RB_ProgramPass2D

Draws src over dst with a program whose uniforms the caller has set:
within box (x, y, width and height in the 2D target's pixels, y down),
moved down and right by offset, the same in dst's pixels at a smaller
level
===============
*/
static void RB_ProgramPass2D( shaderProgram_t *program, image_t *src, FBO_t *dst, const vec4_t color, const int box[4],
	const vec2_t offset, int blend ) {
	float	scale = dst->width / (float)ui2dLayers.width;
	float	ox = offset ? offset[0] * scale : 0, oy = offset ? offset[1] * scale : 0;
	vec4_t	verts[4], c;
	vec2_t	texCoords[4] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };	// rows bottom up
	int		x = floorf( box[0] * scale ), y = floorf( box[1] * scale );

	FBO_Bind( dst );
	RB_SetGL2D();
	// GL's rows go up
	qglScissor( x, dst->height - y - ceilf( box[3] * scale ) - 1, ceilf( box[2] * scale ) + 1, ceilf( box[3] * scale ) + 1 );

	VectorSet4( verts[0], ox, oy, 0, 1 );
	VectorSet4( verts[1], ox + dst->width, oy, 0, 1 );
	VectorSet4( verts[2], ox + dst->width, oy + dst->height, 0, 1 );
	VectorSet4( verts[3], ox, oy + dst->height, 0, 1 );
	if ( color ) {
		VectorCopy4( color, c );
	} else {
		VectorSet4( c, 1, 1, 1, 1 );
	}
	RB_Quad2D( program, src, c, verts, texCoords, blend );
}

// the ui2dfilter program's mode for the passes after
static void RB_FilterMode2D( int mode ) {
	GLSL_BindProgram( &tr.ui2dFilterShader );
	qglUniform1i( filterUniforms.filterMode, mode );
}

// a pass of the ui2dfilter program in a mode
static void RB_Pass2D( image_t *src, FBO_t *dst, int mode, const vec4_t color, const int box[4], const vec2_t offset,
	int blend ) {
	RB_FilterMode2D( mode );
	RB_ProgramPass2D( &tr.ui2dFilterShader, src, dst, color, box, offset, blend );
}

#define BLEND_OVER		( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
#define BLEND_REPLACE	( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO )

/*
===============
RB_SetColorMatrix2D

A color matrix filter's uniforms
===============
*/
static void RB_SetColorMatrix2D( const filter2D_t *filter ) {
	const float	*p = filter->params;
	// by columns, as GL takes it
	mat4_t		m = { p[0], p[5], p[10], p[15], p[1], p[6], p[11], p[16], p[2], p[7], p[12], p[17], p[3], p[8], p[13], p[18] };

	GLSL_BindProgram( &tr.ui2dFilterShader );
	qglUniformMatrix4fv( filterUniforms.colorMatrix, 1, GL_FALSE, m );
	qglUniform4f( filterUniforms.colorOffset, p[4], p[9], p[14], p[19] );
	qglUniform1f( filterUniforms.colorScale, tr.frameLight );
}

/*
===============
RB_FilterClipShape2D

The passes after drawn by how much of each pixel a clip shape
(draw2DState_t's) covers, or wholly for NULL
===============
*/
static void RB_FilterClipShape2D( const float *shape ) {
	float	clip[8];

	GLSL_BindProgram( &tr.ui2dFilterShader );
	qglUniform1i( filterUniforms.clipShape, shape != NULL );
	if ( shape ) {
		RB_ShapeRowsUp2D( shape, ui2dLayers.height, clip );
		qglUniform4fv( filterUniforms.clipBox, 1, clip );
		qglUniform4fv( filterUniforms.clipRadii, 1, clip + 4 );
	}
}

/*
===============
RB_Blur2D

A Gaussian blur of src at sigma pixels of the 2D target within box, keep
untouched: halved in size, level by level from src's own (an earlier
blur's is smaller), until a large sigma is down to BLUR_MAX_SIGMA texels,
then blurred across and down; read as RB_Scratch2D takes it. With
matrix, a color matrix after the blur, done in its last pass, which saves
a pass of its own: at the blur's level, before it's scaled up, so it
differs from the matrix's own pass by a few steps of 255 in some pixels
===============
*/
static image_t *RB_Filter2D( image_t *src, const filter2D_t *filter, const int box[4], const int read[4] );

static image_t *RB_Blur2D( image_t *src, float sigma, const image_t *keep, const int box[4], const int read[4],
	const filter2D_t *matrix ) {
	float		weights[BLUR_MAX_RADIUS + 1], sum;
	image_t		*image = src;
	target2D_t	*t;
	int			start = 0, level, radius, i;

	// sigma in src's texels
	while ( start < BLUR_LEVELS - 1 && MAX( 1, ui2dLayers.width >> start ) > src->width ) {
		start++;
	}
	sigma /= 1 << start;
	if ( sigma < 0.5f ) {
		return matrix ? RB_Filter2D( src, matrix, box, read ) : src;
	}
	for ( level = start; sigma > BLUR_MAX_SIGMA && level < BLUR_LEVELS - 1; level++ ) {
		sigma *= 0.5f;
	}
	radius = MIN( BLUR_MAX_RADIUS, (int)ceilf( sigma * 3.0f ) );
	for ( i = 0, sum = 0; i <= radius; i++ ) {
		weights[i] = expf( -( i * i ) / ( 2.0f * sigma * sigma ) );
		sum += i ? 2 * weights[i] : weights[i];
	}
	for ( i = 0; i <= radius; i++ ) {
		weights[i] /= sum;
	}

	// each halving filtered linearly from the one before
	for ( i = start + 1; i <= level; i++ ) {
		t = RB_Scratch2D( i, image, keep, read );
		RB_Pass2D( image, &t->fbo, FILTERMODE_COPY, NULL, box, NULL, BLEND_REPLACE );
		image = &t->image;
	}

	if ( matrix ) {
		RB_SetColorMatrix2D( matrix );
	}
	GLSL_BindProgram( &tr.ui2dFilterShader );
	qglUniform1i( filterUniforms.blurRadius, radius );
	qglUniform1fv( filterUniforms.blurWeights, radius + 1, weights );
	for ( i = 0; i < 2; i++ ) {
		t = RB_Scratch2D( level, image, keep, read );
		qglUniform2f( filterUniforms.blurDirection, !i, i );
		RB_Pass2D( image, &t->fbo, i && matrix ? FILTERMODE_BLUR_MATRIX : FILTERMODE_BLUR, NULL, box, NULL, BLEND_REPLACE );
		image = &t->image;
	}
	return image;
}

/*
===============
RB_Filter2D

Applies a filter to src within box, into a scratch target, whose image it
returns; read as RB_Scratch2D takes it
===============
*/
static image_t *RB_Filter2D( image_t *src, const filter2D_t *filter, const int box[4], const int read[4] ) {
	const float	*p = filter->params;
	image_t		*shadow = NULL;
	target2D_t	*dst;
	vec4_t		color;

	if ( filter->kind == FILTER2D_BLUR ) {
		return RB_Blur2D( src, p[0], NULL, box, read, NULL );
	}
	if ( filter->kind == FILTER2D_DROP_SHADOW ) {
		shadow = RB_Blur2D( src, p[6], src, box, read, NULL );
	}
	dst = RB_Scratch2D( 0, src, shadow, read );

	switch ( filter->kind ) {
	case FILTER2D_DROP_SHADOW:
		// the shadow, moved, then what casts it over it, its color at the
		// 2D's light as a vertex color would be
		VectorSet4( color, p[0] * tr.frameLight, p[1] * tr.frameLight, p[2] * tr.frameLight, p[3] );
		RB_Pass2D( shadow, &dst->fbo, FILTERMODE_SHADOW, color, box, &p[4], BLEND_OVER );
		RB_Pass2D( src, &dst->fbo, FILTERMODE_COPY, NULL, box, NULL, BLEND_OVER );
		break;

	case FILTER2D_COLOR_MATRIX:
		RB_SetColorMatrix2D( filter );
		RB_Pass2D( src, &dst->fbo, FILTERMODE_COLOR_MATRIX, NULL, box, NULL, BLEND_REPLACE );
		break;

	case FILTER2D_MASK: {
		texture2D_t	*mask = R_GetTexture2D( filter->texture, qtrue );
		float		w = ui2dLayers.width, h = ui2dLayers.height;

		if ( !mask ) {
			// its texture's gone: nothing shows through it
			RB_Pass2D( src, &dst->fbo, FILTERMODE_COPY, transparent, box, NULL, BLEND_REPLACE );
			break;
		}
		// the mask's texture coordinates from the pass's, whose rows go up
		GLSL_BindProgram( &tr.ui2dFilterShader );
		qglUniform4f( filterUniforms.maskTransform, w / mask->image.width, -h / mask->image.height,
			-p[0] / mask->image.width, ( h - p[1] ) / mask->image.height );
		// an A8 texture's coverage is in red where it's R8
		if ( mask->image.dataFormat == GL_RED ) {
			qglUniform4f( filterUniforms.maskChannel, 1, 0, 0, 0 );
		} else {
			qglUniform4f( filterUniforms.maskChannel, 0, 0, 0, 1 );
		}
		GL_BindToTMU( &mask->image, TB_LIGHTMAP );
		RB_Pass2D( src, &dst->fbo, FILTERMODE_MASK, NULL, box, NULL, BLEND_REPLACE );
		break;
	}

	case FILTER2D_OPACITY:
	default:
		VectorSet4( color, p[0], p[0], p[0], p[0] );
		RB_Pass2D( src, &dst->fbo, FILTERMODE_COPY, color, box, NULL, BLEND_REPLACE );
		break;
	}
	return &dst->image;
}

/*
===============
RB_CompositeReach2D

How far a composite's filters read past what they cover, no more than the
target's size
===============
*/
static int RB_CompositeReach2D( const layer2DCommand_t *cmd ) {
	float	spread = 0;
	int		i;

	for ( i = 0; i < cmd->numFilters; i++ ) {
		const filter2D_t	*f = &cmd->filters[i];

		if ( f->kind == FILTER2D_BLUR ) {
			spread += f->params[0] * 3;
		} else if ( f->kind == FILTER2D_DROP_SHADOW ) {
			spread += f->params[6] * 3 + MAX( fabsf( f->params[4] ), fabsf( f->params[5] ) );
		} else if ( f->kind == FILTER2D_GLASS && ( cmd->state.flags & TRIS2D_CLIPSHAPE ) ) {
			float	rim, mirror;

			// its mirror, from past the shape's box across or down
			R_GlassLimits2D( f, cmd->state.clipShape, &rim, &mirror );
			spread += mirror / GLASS2D_MIRROR * GLASS2D_REACH;
		}
	}
	return (int)ceilf( MIN( spread, (float)( ui2dLayers.width + ui2dLayers.height ) ) );
}

// rect (x, y, width and height, y down) grown by reach, within the target;
// the whole target without a rect. Empty where it's off the target
static void RB_GrowBox2D( const int *rect, int reach, int box[4] ) {
	int64_t	x0, y0, x1, y1;

	if ( !rect ) {
		VectorSet4( box, 0, 0, ui2dLayers.width, ui2dLayers.height );
		return;
	}
	x0 = MAX( 0, (int64_t)rect[0] - reach );
	y0 = MAX( 0, (int64_t)rect[1] - reach );
	x1 = MIN( ui2dLayers.width, (int64_t)rect[0] + rect[2] + reach );
	y1 = MIN( ui2dLayers.height, (int64_t)rect[1] + rect[3] + reach );
	box[0] = (int)MIN( x0, ui2dLayers.width );
	box[1] = (int)MIN( y0, ui2dLayers.height );
	box[2] = (int)MAX( 0, x1 - x0 );
	box[3] = (int)MAX( 0, y1 - y0 );
}

// box (x, y, width and height, y down) of src blitted to the same place in
// dst, resolving its samples
static void RB_CopyBox2D( FBO_t *src, FBO_t *dst, const int box[4] ) {
	ivec4_t	glBox;

	// GL's rows go up
	VectorSet4( glBox, box[0], ui2dLayers.height - box[1] - box[3], box[2], box[3] );
	FBO_FastBlit( src, glBox, dst, glBox, GL_COLOR_BUFFER_BIT, GL_NEAREST );
}

/*
===============
RB_LayerFbo2D

A framebuffer with a layer's picture within box in its texture: resolved
from its samples, and for the 2D target, its own render image, its alpha
made opaque first (RB_OpaqueWindow2D). The filters read past the box by
about their reach, so as much more is resolved, as a layer without
samples has it
===============
*/
static FBO_t *RB_LayerFbo2D( int layer, const int box[4], int reach ) {
	FBO_t	*fbo = layer ? &ui2dLayers.layers[layer - 1].fbo : tr.renderFbo;
	FBO_t	*resolve = layer ? &ui2dLayers.layers[layer - 1].resolveFbo : tr.msaaResolveFbo;
	// a blur's levels round to their texels, and filter linearly a texel
	// further
	int		margin = reach ? reach + ( 1 << ( BLUR_LEVELS - 1 ) ) + 1 : 0;
	// what the filters read: the box and its margin, and the pixel more to
	// the right and below that RB_Pass2D writes, so a read of the box a
	// composite wrote is within what it cleared
	int		x0 = MAX( 0, box[0] - margin ), y0 = MAX( 0, box[1] - margin );
	int		x1 = MIN( ui2dLayers.width, box[0] + box[2] + margin + 1 );
	int		y1 = MIN( ui2dLayers.height, box[1] + box[3] + margin + 1 );
	int		read[4] = { x0, y0, x1 - x0, y1 - y0 };

	if ( !layer ) {
		RB_OpaqueWindow2D( read );
	}
	RB_ClearLayer2D( layer, read );
	if ( ui2dLayers.samples && resolve ) {
		RB_CopyBox2D( fbo, resolve, read );
		return resolve;
	}
	return fbo;
}

/*
===============
RB_SaveLayer2D

A layer's box into a UI texture, as colors in display units: the 2D draws
at tr.frameLight, which the texture gets again as it's drawn. Its rows go
down where GL's go up
===============
*/
static void RB_SaveLayer2D( const layer2DCommand_t *cmd ) {
	texture2D_t	*t = R_GetTexture2D( cmd->texture, qtrue );
	const int	*b = cmd->box;
	FBO_t		*src = NULL, *dst = &ui2dLayers.saveFbo;
	vec4_t		verts[4], color;
	vec2_t		texCoords[4];
	float		w, h, s0, s1, t0, t1;
	int			x0, y0, x1, y1, area[4];

	if ( !t ) {
		return;
	}
	x0 = (int)MAX( 0, b[0] );
	y0 = (int)MAX( 0, b[1] );
	x1 = (int)MIN( ui2dLayers.width, (int64_t)b[0] + b[2] );
	y1 = (int)MIN( ui2dLayers.height, (int64_t)b[1] + b[3] );
	if ( x1 > x0 && y1 > y0 ) {
		VectorSet4( area, x0, y0, x1 - x0, y1 - y0 );
		src = RB_LayerFbo2D( MIN( cmd->source, ui2dLayers.depth ), area, 0 );
	}

	if ( !dst->frameBuffer ) {
		Q_strncpyz( dst->name, "*layer2DSave", sizeof( dst->name ) );
		qglGenFramebuffers( 1, &dst->frameBuffer );
	}
	dst->width = t->image.width;
	dst->height = t->image.height;
	FBO_AttachImage( dst, &t->image, GL_COLOR_ATTACHMENT0, 0 );
	FBO_Bind( dst );
	RB_SetGL2D();
	// the texture was made uncleared: what's off the target is transparent,
	// all of it when the whole box is
	if ( !src || x0 != b[0] || y0 != b[1] || x1 - x0 != b[2] || y1 - y0 != b[3] ) {
		FBO_ClearWhole( GL_COLOR_BUFFER_BIT, transparent );
	}

	if ( src ) {
		// the part of the box on the target, in the texture's rows
		qglScissor( x0 - b[0], y0 - b[1], x1 - x0, y1 - y0 );

		// the box's top row to the texture's first, GL's last of the quad's
		w = dst->width;
		h = dst->height;
		s0 = b[0] / (float)ui2dLayers.width;
		s1 = ( b[0] + w ) / ui2dLayers.width;
		t0 = 1 - b[1] / (float)ui2dLayers.height;
		t1 = 1 - ( b[1] + h ) / ui2dLayers.height;
		VectorSet4( verts[0], 0, 0, 0, 1 );
		VectorSet4( verts[1], w, 0, 0, 1 );
		VectorSet4( verts[2], w, h, 0, 1 );
		VectorSet4( verts[3], 0, h, 0, 1 );
		VectorSet2( texCoords[0], s0, t1 );
		VectorSet2( texCoords[1], s1, t1 );
		VectorSet2( texCoords[2], s1, t0 );
		VectorSet2( texCoords[3], s0, t0 );
		VectorSet4( color, 1 / tr.frameLight, 1 / tr.frameLight, 1 / tr.frameLight, 1 );
		RB_FilterMode2D( FILTERMODE_COPY );
		RB_Quad2D( &tr.ui2dFilterShader, src->colorImage[0], color, verts, texCoords, BLEND_REPLACE );
	}

	qglNamedFramebufferTexture2DEXT( dst->frameBuffer, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );
	dst->colorImage[0] = NULL;
}

/*
===============
RB_GlassStates2D

A state texture of the glass filters', made when first needed, two
texels a glass: this frame's (1) or the frame before's (0)
===============
*/
static target2D_t *RB_GlassStates2D( int now ) {
	target2D_t	*t = &ui2dLayers.glassStates[( ui2dLayers.glassFrame + now ) & 1];

	if ( !t->image.texnum ) {
		R_MakeTarget2D( t, MAX_FILTERS2D * 2, 1, 0, NULL );
		qglTextureParameteriEXT( t->image.texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		qglTextureParameteriEXT( t->image.texnum, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		FBO_Bind( &t->fbo );
		FBO_ClearWhole( GL_COLOR_BUFFER_BIT, transparent );
	}
	return t;
}

/*
===============
RB_GlassState2D

A composite's glass filter (ui2dglass_fp.glsl) set for its shape, and its
state for this frame drawn from frost, the filters' result before it, and
the frame before's state; it starts again when the glass didn't draw the
frame before. RB_Glass2D then draws the glass, which reads it
===============
*/
static void RB_GlassState2D( const layer2DCommand_t *cmd, const filter2D_t *glass, image_t *frost ) {
	const float	*p = glass->params;
	const float	*glow = cmd->glassGlow, *m = cmd->glassDeform;
	int			s = glass->glassState, width = MAX_FILTERS2D * 2, frame = ui2dLayers.glassFrame + 1;
	target2D_t	*before = RB_GlassStates2D( 0 ), *now = RB_GlassStates2D( 1 );
	float		shape[8], rim, mirror, dt = ui2dLayers.glassDt, det = m[0] * m[3] - m[1] * m[2];
	vec4_t		verts[4], white = { 1, 1, 1, 1 };
	vec2_t		texCoords[4] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };

	// a glass composited again this frame keeps its first's start; a new one
	// in a slot the frame before's had starts afresh
	if ( ui2dLayers.glass[s].serial != glass->glassSerial ) {
		ui2dLayers.glass[s].serial = glass->glassSerial;
		ui2dLayers.glass[s].frame = 0;
	}
	if ( ui2dLayers.glass[s].frame != frame ) {
		ui2dLayers.glass[s].start = ui2dLayers.glass[s].frame != frame - 1;
		ui2dLayers.glass[s].frame = frame;
	}
	RB_ShapeRowsUp2D( cmd->state.clipShape, ui2dLayers.height, shape );
	R_GlassLimits2D( glass, cmd->state.clipShape, &rim, &mirror );

	GLSL_BindProgram( &tr.ui2dGlassShader );
	qglUniform1f( glassUniforms.colorScale, tr.frameLight );
	qglUniform4fv( glassUniforms.box, 1, shape );
	qglUniform4fv( glassUniforms.radii, 1, shape + 4 );
	qglUniform4f( glassUniforms.rim, rim, mirror, p[GLASS2D_HIGHLIGHT], p[GLASS2D_EDGE] );
	qglUniform2f( glassUniforms.light, p[GLASS2D_LIGHT_X], p[GLASS2D_LIGHT_Y] );
	qglUniform3f( glassUniforms.darkColor, p[GLASS2D_DARK_COLOR], p[GLASS2D_DARK_COLOR + 1], p[GLASS2D_DARK_COLOR + 2] );
	qglUniform3f( glassUniforms.lightColor, p[GLASS2D_LIGHT_COLOR], p[GLASS2D_LIGHT_COLOR + 1], p[GLASS2D_LIGHT_COLOR + 2] );
	qglUniform4f( glassUniforms.tint, p[GLASS2D_TINT_MIN], p[GLASS2D_TINT_MAX], p[GLASS2D_DARK_GOAL], p[GLASS2D_LIGHT_GOAL] );
	qglUniform1i( glassUniforms.mode, (int)p[GLASS2D_TINT] );
	// rows up, as the shape's
	qglUniform4f( glassUniforms.glow, glow[0], ui2dLayers.height - glow[1], glow[2], glow[3] );
	// the deformation's inverse, rows up, which takes a pixel back to where
	// it lies on the shape
	qglUniform4f( glassUniforms.deform, m[3] / det, m[1] / det, m[2] / det, m[0] / det );
	qglUniform2f( glassUniforms.invTargetRes, 1.0f / ui2dLayers.width, 1.0f / ui2dLayers.height );
	qglUniform2f( glassUniforms.stateCoord, ( s * 2 + 0.5f ) / width, 0.5f );
	qglUniform1f( glassUniforms.stateStep, 1.0f / width );
	// light past 0.62, dark again below 0.45
	qglUniform4f( glassUniforms.smoothing, 1 - expf( -dt / p[GLASS2D_SMOOTHING] ), 1 - expf( -dt / p[GLASS2D_RELEASE] ),
		0.62f, 0.45f );
	// how far the look turns toward dark or light this frame
	qglUniform1f( glassUniforms.fade, p[GLASS2D_FADE] > 0 ? MIN( dt / p[GLASS2D_FADE], 1 ) : 1 );
	qglUniform1i( glassUniforms.start, ui2dLayers.glass[s].start );

	// the state's two texels, every channel whatever the color mask
	FBO_Bind( &now->fbo );
	RB_SetGL2D();
	qglScissor( s * 2, 0, 2, 1 );
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	VectorSet4( verts[0], 0, 0, 0, 1 );
	VectorSet4( verts[1], width, 0, 0, 1 );
	VectorSet4( verts[2], width, 1, 0, 1 );
	VectorSet4( verts[3], 0, 1, 0, 1 );
	GL_BindToTMU( &before->image, TB_LIGHTMAP );
	qglUniform1i( glassUniforms.pass, 1 );
	RB_Quad2D( &tr.ui2dGlassShader, frost, white, verts, texCoords, BLEND_REPLACE );
	qglColorMask( !backEnd.colorMask[0], !backEnd.colorMask[1], !backEnd.colorMask[2], !backEnd.colorMask[3] );
}

/*
===============
RB_Glass2D

A composite's last pass through its glass filter, set and its state drawn
by RB_GlassState2D: from frost onto dst within out
===============
*/
static void RB_Glass2D( image_t *frost, FBO_t *dst, const int out[4], const vec4_t color, int blend ) {
	GL_BindToTMU( &RB_GlassStates2D( 1 )->image, TB_LIGHTMAP );
	GLSL_BindProgram( &tr.ui2dGlassShader );
	qglUniform1i( glassUniforms.pass, 0 );
	RB_ProgramPass2D( &tr.ui2dGlassShader, frost, dst, color, out, NULL, blend );
}

/*
===============
RB_Layer2D

Each 2D draw binds its target and sets its scissor itself, so a layer
command leaves the framebuffer it last drew into bound, and the 2D's
scissor state forgotten: a backdrop's commands in a row don't go back to
the 2D target between them
===============
*/
const void *RB_Layer2D( const void *data ) {
	const layer2DCommand_t	*cmd = (const layer2DCommand_t *)data;

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}
	// with none pushed, the layers and scratch targets are fitted to the
	// 2D target, which a composite or save may read alone (a pop comes
	// only after a push)
	if ( !ui2dLayers.depth ) {
		RB_FitLayers2D();
	}
	ui2dLayers.idleFrames = 0;

	switch ( cmd->op ) {
	case LAYER2D_PUSH: {
		target2D_t	*t;

		// the front end counts them too (R_PushLayer2D)
		if ( ui2dLayers.depth == MAX_LAYERS2D ) {
			break;
		}
		t = &ui2dLayers.layers[ui2dLayers.depth];
		if ( !t->image.texnum ) {
			R_MakeTarget2D( t, ui2dLayers.width, ui2dLayers.height, ui2dLayers.samples, ui2dLayers.target );
		}
		VectorSet4( ui2dLayers.cleared[ui2dLayers.depth], 0, 0, 0, 0 );
		ui2dLayers.depth++;
		break;
	}

	case LAYER2D_POP:
		if ( ui2dLayers.depth ) {
			ui2dLayers.depth--;
		}
		break;

	case LAYER2D_COMPOSITE: {
		FBO_t		*dst = cmd->destination ? &ui2dLayers.layers[cmd->destination - 1].fbo : tr.renderFbo;
		image_t		*image;
		vec4_t		color = { 1, 1, 1, 1 };
		const int	*scissor = ( cmd->state.flags & TRIS2D_SCISSOR ) ? cmd->state.scissor : NULL;
		const int	*input = cmd->input[2] ? cmd->input : NULL;
		const float	*shape = ( cmd->state.flags & TRIS2D_CLIPSHAPE ) ? cmd->state.clipShape : NULL;
		const filter2D_t	*glass = NULL;
		FBO_t		*read;
		int			box[4], reads[4], out[4], written[4], reach = RB_CompositeReach2D( cmd ), numFilters = cmd->numFilters, blend, i;
		qboolean	clip;

		// what the filters cover, the input box or the scissor, grown by
		// their reach; and what's written, the scissor alone with an input
		RB_GrowBox2D( input ? input : scissor, reach, box );
		RB_GrowBox2D( scissor, input ? 0 : reach, out );
		if ( !box[2] || !box[3] || !out[2] || !out[3] ) {
			break;
		}
		// what RB_Pass2D's scissor lets it write: the box, and a pixel more
		// to the right and below
		VectorSet4( written, out[0], out[1], MIN( out[2] + 1, ui2dLayers.width - out[0] ),
			MIN( out[3] + 1, ui2dLayers.height - out[1] ) );
		// what the passes can read of a scratch target (RB_Scratch2D): over
		// box, a drop shadow's, moved by its offset, and a blur's, by its
		// radius; and the last pass's, what it writes, which with an input
		// can go past box
		RB_GrowBox2D( box, reach, reads );
		{
			int		x0 = MIN( reads[0], written[0] ), y0 = MIN( reads[1], written[1] );
			int		x1 = MAX( reads[0] + reads[2], written[0] + written[2] );
			int		y1 = MAX( reads[1] + reads[3], written[1] + written[3] );

			VectorSet4( reads, x0, y0, x1 - x0, y1 - y0 );
		}
		read = RB_LayerFbo2D( cmd->source, box, reach );
		image = read->colorImage[0];
		// a last opacity goes into the composite; with an input, it's a
		// pass of its own, as into the layer of its own it stands for, so
		// the pixels are rounded as that layer would round them
		if ( !input && numFilters && cmd->filters[numFilters - 1].kind == FILTER2D_OPACITY ) {
			float	alpha = cmd->filters[--numFilters].params[0];

			VectorSet4( color, alpha, alpha, alpha, alpha );
		}
		// a glass is the last pass, drawn within the shape (the front end
		// leaves one without a shape out)
		if ( numFilters && cmd->filters[numFilters - 1].kind == FILTER2D_GLASS ) {
			glass = &cmd->filters[--numFilters];
		}
		for ( i = 0; i < numFilters; i++ ) {
			// a blur's last pass takes a color matrix after it, as in a
			// backdrop's blur() saturate() or a filter's blur() grayscale(),
			// but one adding alpha, which the blur's wider scissor at a
			// smaller size would spread past the box
			if ( cmd->filters[i].kind == FILTER2D_BLUR && i + 1 < numFilters &&
				cmd->filters[i + 1].kind == FILTER2D_COLOR_MATRIX && cmd->filters[i + 1].params[19] <= 0 ) {
				image = RB_Blur2D( image, cmd->filters[i].params[0], NULL, box, reads, &cmd->filters[i + 1] );
				i++;
			} else {
				image = RB_Filter2D( image, &cmd->filters[i], box, reads );
			}
		}
		// unfiltered, the source's own picture: copied within what a pass
		// over box writes, transparent past it as a filter's target is,
		// when that's the destination itself (without samples), or the
		// scissor goes past an input's box
		if ( image == read->colorImage[0] ) {
			int		within[4] = { box[0], box[1], MIN( box[2] + 1, ui2dLayers.width - box[0] ),
						MIN( box[3] + 1, ui2dLayers.height - box[1] ) };

			if ( image == dst->colorImage[0] || !RB_BoxWithin2D( written, within ) ) {
				target2D_t	*copy = RB_Scratch2D( 0, NULL, NULL, reads );
				int			x0 = MAX( written[0], within[0] ), y0 = MAX( written[1], within[1] );
				int			x1 = MIN( written[0] + written[2], within[0] + within[2] );
				int			y1 = MIN( written[1] + written[3], within[1] + within[3] );

				if ( x1 > x0 && y1 > y0 ) {
					VectorSet4( within, x0, y0, x1 - x0, y1 - y0 );
					RB_CopyBox2D( read, &copy->fbo, within );
				}
				image = &copy->image;
			}
		}
		if ( glass ) {
			RB_GlassState2D( cmd, glass, image );
		}
		RB_ClearLayer2D( cmd->destination, written );
		FBO_Bind( dst );
		clip = R_BeginClipTest2D( &cmd->state );
		blend = cmd->blendMode == BLEND2D_REPLACE ? BLEND_REPLACE : BLEND_OVER;
		if ( glass ) {
			RB_Glass2D( image, dst, out, color, blend );
		} else {
			RB_FilterClipShape2D( shape );
			RB_Pass2D( image, dst, FILTERMODE_COPY, color, out, NULL, blend );
			if ( shape ) {
				RB_FilterClipShape2D( NULL );
			}
		}
		if ( clip ) {
			R_EndClipTest2D();
		}
		break;
	}

	case LAYER2D_SAVE:
		RB_SaveLayer2D( cmd );
		break;
	}

	RB_ForgetScissor2D();
	return cmd + 1;
}

/*
===============
RB_EndFrame2D

The layers a frame left pushed are popped as it ends, and freed when no
layer command has come for a while; the 2D target is bound again for
what follows the 2D, as a layer command leaves the framebuffer it last
drew into bound
===============
*/
void RB_EndFrame2D( void ) {
	int		now = ri.Milliseconds();

	ui2dLayers.depth = 0;
	// the glass filters' states take turns, and smooth over the time the
	// frame took, as long as 0.1 s at most
	ui2dLayers.glassFrame++;
	ui2dLayers.glassDt = Com_Clamp( 0, 0.1f, ( now - ui2dLayers.glassTime ) * 0.001f );
	ui2dLayers.glassTime = now;
	if ( ui2dLayers.target && ++ui2dLayers.idleFrames > LAYERS2D_IDLE_FRAMES ) {
		R_ShutdownLayers2D();
	}
	if ( tr.renderFbo ) {
		FBO_Bind( tr.renderFbo );
	}
}
