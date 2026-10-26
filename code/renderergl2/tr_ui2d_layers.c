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

// the layers are freed after this many frames without one pushed
#define LAYERS2D_IDLE_FRAMES	300

static struct {
	int			width, height, samples;		// the 2D target's, which the layers were made for
	FBO_t		*target;					// whose depth and stencil they share
	int			depth;						// the layers pushed, in the back end
	int			idleFrames;
	target2D_t	layers[MAX_LAYERS2D];
	// single-sampled, each level half the size of the one before; three
	// at each, for a drop shadow's
	target2D_t	scratch[BLUR_LEVELS][3];
	FBO_t		saveFbo;					// a saved layer's texture is drawn through
} ui2dLayers;

// the ui2dfilter program's uniforms that GLSL_InitUniforms doesn't know
static struct {
	GLint	filterMode, colorMatrix, colorOffset, colorScale, blurDirection, blurRadius, blurWeights, maskMap, maskTransform, maskChannel;
} filterUniforms;

// ui2dfilter's u_FilterMode
enum {
	FILTERMODE_COPY,
	FILTERMODE_COLOR_MATRIX,
	FILTERMODE_BLUR,
	FILTERMODE_SHADOW,
	FILTERMODE_MASK
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
	GLSL_BindProgram( &tr.ui2dFilterShader );
	qglUniform1i( filterUniforms.maskMap, TB_LIGHTMAP );
}


/*
============================================================================

BACK END

============================================================================
*/

/*
===============
RB_Target2D

What the 2D draws go into: the top layer, or the 2D target
===============
*/
FBO_t *RB_Target2D( void ) {
	return ui2dLayers.depth ? &ui2dLayers.layers[ui2dLayers.depth - 1].fbo : tr.renderFbo;
}

/*
===============
RB_FitLayers2D

Makes the layers again for the 2D target, as the first is pushed, when
it's another, or its size or samples changed
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
the box it drew is transparent, not an earlier composite's
===============
*/
static target2D_t *RB_Scratch2D( int level, const image_t *inUse1, const image_t *inUse2 ) {
	target2D_t	*t = ui2dLayers.scratch[level];

	while ( &t->image == inUse1 || &t->image == inUse2 ) {
		t++;
	}
	if ( !t->image.texnum ) {
		R_MakeTarget2D( t, MAX( 1, ui2dLayers.width >> level ), MAX( 1, ui2dLayers.height >> level ), 0, NULL );
	}
	FBO_Bind( &t->fbo );
	FBO_ClearWhole( GL_COLOR_BUFFER_BIT, transparent );
	return t;
}

/*
===============
RB_Quad2D

Draws src through the ui2dfilter program in a mode, as a quad into the
bound framebuffer, in its 2D projection
===============
*/
static void RB_Quad2D( image_t *src, int mode, const vec4_t color, vec4_t verts[4], vec2_t texCoords[4], int blend ) {
	vec2_t	invTexRes;

	VectorSet2( invTexRes, 1.0f / src->width, 1.0f / src->height );
	GLSL_BindProgram( &tr.ui2dFilterShader );
	GLSL_SetUniformMat4( &tr.ui2dFilterShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.projection );
	GLSL_SetUniformVec4( &tr.ui2dFilterShader, UNIFORM_COLOR, color );
	GLSL_SetUniformVec2( &tr.ui2dFilterShader, UNIFORM_INVTEXRES, invTexRes );
	qglUniform1i( filterUniforms.filterMode, mode );
	GL_BindToTMU( src, TB_COLORMAP );
	GL_State( GLS_DEPTHTEST_DISABLE | blend );
	GL_Cull( CT_TWO_SIDED );
	RB_InstantQuad2( verts, texCoords );
}

/*
===============
RB_Pass2D

Draws src over dst with the ui2dfilter program in a mode, whose other
uniforms the caller has set: within box (x, y, width and height in the 2D
target's pixels, y down), moved down and right by offset, the same in
dst's pixels at a smaller level
===============
*/
static void RB_Pass2D( image_t *src, FBO_t *dst, int mode, const vec4_t color, const int box[4], const vec2_t offset,
	int blend ) {
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
	RB_Quad2D( src, mode, c, verts, texCoords, blend );
}

#define BLEND_OVER		( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
#define BLEND_REPLACE	( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO )

/*
===============
RB_Blur2D

A Gaussian blur of src at sigma pixels within box, keep untouched: halved
in size, level by level, until a large sigma is down to BLUR_MAX_SIGMA,
then blurred across and down
===============
*/
static image_t *RB_Blur2D( image_t *src, float sigma, const image_t *keep, const int box[4] ) {
	float		weights[BLUR_MAX_RADIUS + 1], sum;
	image_t		*image = src;
	target2D_t	*t;
	int			level = 0, radius, i;

	if ( sigma < 0.5f ) {
		return src;
	}
	while ( sigma > BLUR_MAX_SIGMA && level < BLUR_LEVELS - 1 ) {
		sigma *= 0.5f;
		level++;
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
	for ( i = 1; i <= level; i++ ) {
		t = RB_Scratch2D( i, image, keep );
		RB_Pass2D( image, &t->fbo, FILTERMODE_COPY, NULL, box, NULL, BLEND_REPLACE );
		image = &t->image;
	}

	GLSL_BindProgram( &tr.ui2dFilterShader );
	qglUniform1i( filterUniforms.blurRadius, radius );
	qglUniform1fv( filterUniforms.blurWeights, radius + 1, weights );
	for ( i = 0; i < 2; i++ ) {
		t = RB_Scratch2D( level, image, keep );
		qglUniform2f( filterUniforms.blurDirection, !i, i );
		RB_Pass2D( image, &t->fbo, FILTERMODE_BLUR, NULL, box, NULL, BLEND_REPLACE );
		image = &t->image;
	}
	return image;
}

/*
===============
RB_Filter2D

Applies a filter to src within box, into a scratch target, whose image it
returns
===============
*/
static image_t *RB_Filter2D( image_t *src, const filter2D_t *filter, const int box[4] ) {
	const float	*p = filter->params;
	image_t		*shadow = NULL;
	target2D_t	*dst;
	vec4_t		color;

	if ( filter->kind == FILTER2D_BLUR ) {
		return RB_Blur2D( src, p[0], NULL, box );
	}
	if ( filter->kind == FILTER2D_DROP_SHADOW ) {
		shadow = RB_Blur2D( src, p[6], src, box );
	}
	dst = RB_Scratch2D( 0, src, shadow );

	switch ( filter->kind ) {
	case FILTER2D_DROP_SHADOW:
		// the shadow, moved, then what casts it over it, its color at the
		// 2D's light as a vertex color would be
		VectorSet4( color, p[0] * tr.frameLight, p[1] * tr.frameLight, p[2] * tr.frameLight, p[3] );
		RB_Pass2D( shadow, &dst->fbo, FILTERMODE_SHADOW, color, box, &p[4], BLEND_OVER );
		RB_Pass2D( src, &dst->fbo, FILTERMODE_COPY, NULL, box, NULL, BLEND_OVER );
		break;

	case FILTER2D_COLOR_MATRIX: {
		// by columns, as GL takes it
		mat4_t	m = { p[0], p[5], p[10], p[15], p[1], p[6], p[11], p[16], p[2], p[7], p[12], p[17], p[3], p[8], p[13], p[18] };

		GLSL_BindProgram( &tr.ui2dFilterShader );
		qglUniformMatrix4fv( filterUniforms.colorMatrix, 1, GL_FALSE, m );
		qglUniform4f( filterUniforms.colorOffset, p[4], p[9], p[14], p[19] );
		qglUniform1f( filterUniforms.colorScale, tr.frameLight );
		RB_Pass2D( src, &dst->fbo, FILTERMODE_COLOR_MATRIX, NULL, box, NULL, BLEND_REPLACE );
		break;
	}

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
RB_CompositeBox2D

What a composite covers: its scissor, grown by its filters' reach, or the
whole target; and the reach, no more than the target's size. Empty when
the scissor is off the target
===============
*/
static int RB_CompositeBox2D( const layer2DCommand_t *cmd, int box[4] ) {
	const int	*s = cmd->state.scissor;
	float		spread = 0;
	int64_t		x0, y0, x1, y1;
	int			i, reach;

	for ( i = 0; i < cmd->numFilters; i++ ) {
		const filter2D_t	*f = &cmd->filters[i];

		if ( f->kind == FILTER2D_BLUR ) {
			spread += f->params[0] * 3;
		} else if ( f->kind == FILTER2D_DROP_SHADOW ) {
			spread += f->params[6] * 3 + MAX( fabsf( f->params[4] ), fabsf( f->params[5] ) );
		}
	}
	reach = (int)ceilf( MIN( spread, (float)( ui2dLayers.width + ui2dLayers.height ) ) );

	if ( !( cmd->state.flags & TRIS2D_SCISSOR ) ) {
		box[0] = box[1] = 0;
		box[2] = ui2dLayers.width;
		box[3] = ui2dLayers.height;
		return reach;
	}
	x0 = MAX( 0, (int64_t)s[0] - reach );
	y0 = MAX( 0, (int64_t)s[1] - reach );
	x1 = MIN( ui2dLayers.width, (int64_t)s[0] + s[2] + reach );
	y1 = MIN( ui2dLayers.height, (int64_t)s[1] + s[3] + reach );
	box[0] = (int)MIN( x0, ui2dLayers.width );
	box[1] = (int)MIN( y0, ui2dLayers.height );
	box[2] = (int)MAX( 0, x1 - x0 );
	box[3] = (int)MAX( 0, y1 - y0 );
	return reach;
}

/*
===============
RB_LayerFbo2D

A framebuffer with a layer's picture within box in its texture: resolved
from its samples, and for the 2D target, its own render image. The
filters read past the box by about their reach, so as much more is
resolved, as a layer without samples has it
===============
*/
static FBO_t *RB_LayerFbo2D( int layer, const int box[4], int reach ) {
	FBO_t	*fbo = layer ? &ui2dLayers.layers[layer - 1].fbo : tr.renderFbo;
	FBO_t	*resolve = layer ? &ui2dLayers.layers[layer - 1].resolveFbo : tr.msaaResolveFbo;
	ivec4_t	glBox;

	if ( ui2dLayers.samples && resolve ) {
		// a blur's levels round to their texels
		int		margin = reach ? reach + ( 1 << ( BLUR_LEVELS - 1 ) ) : 0;
		int		x0 = MAX( 0, box[0] - margin ), y0 = MAX( 0, box[1] - margin );
		int		x1 = MIN( ui2dLayers.width, box[0] + box[2] + margin );
		int		y1 = MIN( ui2dLayers.height, box[1] + box[3] + margin );

		// GL's rows go up
		VectorSet4( glBox, x0, ui2dLayers.height - y1, x1 - x0, y1 - y0 );
		FBO_FastBlit( fbo, glBox, resolve, glBox, GL_COLOR_BUFFER_BIT, GL_NEAREST );
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
	if ( !ui2dLayers.depth ) {
		RB_FitLayers2D();
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
		RB_Quad2D( src->colorImage[0], FILTERMODE_COPY, color, verts, texCoords, BLEND_REPLACE );
	}

	qglNamedFramebufferTexture2DEXT( dst->frameBuffer, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );
	dst->colorImage[0] = NULL;
}

/*
===============
RB_ResetScissor2D

The whole target to draw in again, as the 2D's scissor state has it
===============
*/
static void RB_ResetScissor2D( void ) {
	FBO_t	*fbo = RB_Target2D();

	FBO_Bind( fbo );
	RB_SetGL2D();
	qglScissor( 0, 0, fbo->width, fbo->height );
	backEnd.scissor2D = qfalse;
}

/*
===============
RB_Layer2D
===============
*/
const void *RB_Layer2D( const void *data ) {
	const layer2DCommand_t	*cmd = (const layer2DCommand_t *)data;

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	switch ( cmd->op ) {
	case LAYER2D_PUSH: {
		target2D_t	*t;

		if ( !ui2dLayers.depth ) {
			RB_FitLayers2D();
		}
		// the front end counts them too (R_PushLayer2D)
		if ( ui2dLayers.depth == MAX_LAYERS2D ) {
			break;
		}
		t = &ui2dLayers.layers[ui2dLayers.depth];
		if ( !t->image.texnum ) {
			R_MakeTarget2D( t, ui2dLayers.width, ui2dLayers.height, ui2dLayers.samples, ui2dLayers.target );
		}
		FBO_Bind( &t->fbo );
		FBO_ClearWhole( GL_COLOR_BUFFER_BIT, transparent );
		ui2dLayers.depth++;
		ui2dLayers.idleFrames = 0;
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
		int			box[4], reach, numFilters = cmd->numFilters, i;
		qboolean	clip;

		reach = RB_CompositeBox2D( cmd, box );
		if ( !box[2] || !box[3] ) {
			break;
		}
		image = RB_LayerFbo2D( cmd->source, box, reach )->colorImage[0];
		// a last opacity goes into the composite
		if ( numFilters && cmd->filters[numFilters - 1].kind == FILTER2D_OPACITY ) {
			float	alpha = cmd->filters[--numFilters].params[0];

			VectorSet4( color, alpha, alpha, alpha, alpha );
		}
		for ( i = 0; i < numFilters; i++ ) {
			image = RB_Filter2D( image, &cmd->filters[i], box );
		}

		FBO_Bind( dst );
		clip = R_BeginClipTest2D( &cmd->state );
		RB_Pass2D( image, dst, FILTERMODE_COPY, color, box, NULL,
			cmd->blendMode == BLEND2D_REPLACE ? BLEND_REPLACE : BLEND_OVER );
		if ( clip ) {
			R_EndClipTest2D();
		}
		break;
	}

	case LAYER2D_SAVE:
		RB_SaveLayer2D( cmd );
		break;
	}

	RB_ResetScissor2D();
	return cmd + 1;
}

/*
===============
RB_EndFrame2D

The layers a frame left pushed are popped as it ends, and freed when none
has been pushed for a while
===============
*/
void RB_EndFrame2D( void ) {
	ui2dLayers.depth = 0;
	if ( ui2dLayers.target && ++ui2dLayers.idleFrames > LAYERS2D_IDLE_FRAMES ) {
		R_ShutdownLayers2D();
	}
}
