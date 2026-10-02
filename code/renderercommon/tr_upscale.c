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
// tr_upscale.c -- sharper 2D art at high resolutions (r_upscale2D)
//
// The filter is a C port of the EASU pass of AMD FidelityFX Super
// Resolution 1.0 (ffx_fsr1.h, https://github.com/GPUOpen-Effects/FidelityFX-FSR),
// under its license:
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "tr_common.h"

cvar_t *r_upscale2D;

// Extra texture memory the upscaled images may take, in bytes, until the
// renderer restarts. WebGL and GLES devices have less to spare. The cache
// of upscaled images, in system memory, holds at most as much
#define UPSCALE_BUDGET		( 64 * 1024 * 1024 )
#define UPSCALE_BUDGET_GLES	( 16 * 1024 * 1024 )

// The largest upscaled image, in bytes: opengl1's upload copies it to the
// hunk's temporary memory, which a big image would exhaust
#define UPSCALE_MAX_IMAGE	( 16 * 1024 * 1024 )

// The largest source, in pixels: filtering takes 32 bytes a source pixel
// of the hunk's temporary memory
#define UPSCALE_MAX_SOURCE	( 512 * 512 )

static int upscaleFactor;
static int upscaleLimit;
static int upscaleBudget;

// The folders Quake III's art is drawn from in 2D: the HUD, menus, icons
// and level shots. World textures and effects live elsewhere
static const char *upscalePaths[] = {
	"gfx/2d/", "menu/", "icons/", "ui/", "levelshots/"
};

// An upscaled image, kept so that the next map load, which recreates
// every image, needn't filter it again. The cache is in system memory
// (malloc) because it outlives the hunk, and would crowd the zone
typedef struct upscaled_s {
	struct upscaled_s	*next;
	char				name[MAX_QPATH];
	int					width, height;
	byte				*source;		// to tell it from another pak's
	int					outWidth, outHeight;
	byte				*pic;
	int					bytes;			// of source and pic
	int					load;			// the last load that used it
} upscaled_t;

static upscaled_t	*upscaleCache;		// the most recently used first
static int			upscaleLoad;

/*
===============
R_Is2DArt

Whether the image is art drawn in 2D: in one of upscalePaths, or a
player's head icon on the scoreboard (models/players/<model>/icon_<skin>)
===============
*/
static qboolean R_Is2DArt( const char *name ) {
	const char *file;
	int i;

	for ( i = 0; i < ARRAY_LEN( upscalePaths ); i++ ) {
		if ( !Q_stricmpn( name, upscalePaths[i], strlen( upscalePaths[i] ) ) ) {
			return qtrue;
		}
	}

	file = strrchr( name, '/' );
	return !Q_stricmpn( name, "models/players/", 15 ) && file && !Q_stricmpn( file + 1, "icon_", 5 );
}

/*
===============
R_UpscaleTrim

Frees the cached images past limit bytes, the least recently used first,
and any not used since load
===============
*/
static void R_UpscaleTrim( int limit, int load ) {
	upscaled_t **link = &upscaleCache;
	int bytes = 0;

	while ( *link ) {
		upscaled_t *entry = *link;

		if ( entry->load >= load && bytes + entry->bytes <= limit ) {
			bytes += entry->bytes;
			link = &entry->next;
			continue;
		}
		*link = entry->next;
		free( entry->source );
		free( entry->pic );
		free( entry );
	}
}

/*
===============
R_InitUpscale2D

Registers r_upscale2D and picks the factor 2D art loads at until the
renderer restarts: the scale of the 640x480 screen in whole steps, at most
4 (2 on GLES), or 1 without r_upscale2D. Called as the renderer's images
are created, at each map load: the cache keeps what the last load used,
at the same factor
===============
*/
void R_InitUpscale2D( void ) {
	int factor = MIN( glConfig.vidWidth / 640, glConfig.vidHeight / 480 );
	int limit = qglesMajorVersion ? UPSCALE_BUDGET_GLES : UPSCALE_BUDGET;

	r_upscale2D = ri.Cvar_Get( "r_upscale2D", "1", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_CheckRange( r_upscale2D, 0, 1, qtrue );
	ri.Cvar_SetDescription( r_upscale2D, "Sharpen the HUD and menus' art at high resolutions by upscaling it "
		"as it loads, by the 640x480 screen's scale in whole steps (up to 4, or 2 on GLES)" );

	factor = r_upscale2D->integer ? MIN( factor, qglesMajorVersion ? 2 : 4 ) : 1;
	if ( factor != upscaleFactor || limit != upscaleLimit ) {
		R_ShutdownUpscale2D();
	}
	R_UpscaleTrim( INT_MAX, upscaleLoad++ );

	upscaleFactor = factor;
	upscaleLimit = limit;
	upscaleBudget = limit;
}

/*
===============
R_ShutdownUpscale2D

Frees the cache of upscaled images, as the renderer shuts down for good
or before its library is unloaded
===============
*/
void R_ShutdownUpscale2D( void ) {
	R_UpscaleTrim( 0, INT_MAX );
}

/*
===============
R_UpscaleSide

A side of the upscaled image. An upload that rounds to powers of two gets
the power of two nearest the side times factor, on a logarithmic scale:
any other size would be resampled again, with a box filter
===============
*/
static int R_UpscaleSide( int side, int factor, qboolean powerOfTwo ) {
	int target = side * factor;
	int lower;

	if ( !powerOfTwo ) {
		return target;
	}
	lower = 1 << Q_log2( target );
	return (int64_t)target * target >= 2 * (int64_t)lower * lower ? lower * 2 : lower;
}

/*
===============
R_Upscale2DSize

The size to upscale the image to, taking from the budget; qfalse to leave
it as it is
===============
*/
static qboolean R_Upscale2DSize( const char *name, int width, int height, qboolean powerOfTwo,
		qboolean uploadShrinks, int *outWidth, int *outHeight ) {
	int factor;

	// sources are at most UPSCALE_MAX_SOURCE pixels, so nothing below overflows
	if ( upscaleFactor < 2 || uploadShrinks || width * height > UPSCALE_MAX_SOURCE || !R_Is2DArt( name ) ) {
		return qfalse;
	}

	for ( factor = upscaleFactor; factor > 1; factor-- ) {
		int w = R_UpscaleSide( width, factor, powerOfTwo );
		int h = R_UpscaleSide( height, factor, powerOfTwo );
		int extra = ( w * h - width * height ) * 4;

		if ( w > width && h > height
			&& w <= glConfig.maxTextureSize && h <= glConfig.maxTextureSize
			&& w * h * 4 <= UPSCALE_MAX_IMAGE && extra <= upscaleBudget ) {
			upscaleBudget -= extra;
			*outWidth = w;
			*outHeight = h;
			return qtrue;
		}
	}
	return qfalse;
}

/*
===============
EASU, edge-adaptive spatial upsampling

Filters with a Lanczos-like kernel stretched along the local edge, then
clamps to the four nearest pixels so edges don't ring. Colors are filtered
premultiplied by their alpha, so cut-out art's transparent pixels don't
bleed into its edges, and also straight, for pixels that stay transparent:
additive shaders draw their color whatever their alpha.

The source is copied with a border of 2 pixels, repeating its edges, so
every tap around an output pixel is a fixed offset from it, and each
pixel's edge direction and length is worked out once
===============
*/

#define EASU_BORDER		2

typedef struct {
	const byte		*rgba;		// the source's pixels
	const float		*pixels;	// premultiplied, 0 to 1
	const float		*edges;		// direction across and down, and length
	int				stride;		// pixels in a row, with the border
	int				tapOffsets[12];
} easuSource_t;

// the 12 taps around an output pixel, as FSR names them:
//     b c
//   e f g h
//   i j k l
//     n o
enum { B, C, E, F, G, H, I, J, K, L, N, O };

static const int easuTaps[12][2] = {
	{ 0, -1 }, { 1, -1 },
	{ -1, 0 }, { 0, 0 }, { 1, 0 }, { 2, 0 },
	{ -1, 1 }, { 0, 1 }, { 1, 1 }, { 2, 1 },
	{ 0, 2 }, { 1, 2 }
};

static ID_INLINE float EASU_Saturate( float x ) {
	return MAX( 0.0f, MIN( x, 1.0f ) );
}

static ID_INLINE float EASU_Luma( const float *p ) {
	return 0.5f * p[2] + 0.5f * p[0] + p[1];
}

// A pixel's edge direction and squared length, from the luma of its
// neighbors above (a), left (b), right (d) and below (e) and its own (c)
// (FsrEasuSetF without the bilinear weight, which each output pixel applies)
static void EASU_Edge( float edge[3], float lA, float lB, float lC, float lD, float lE ) {
	float lenX = MAX( fabsf( lD - lC ), fabsf( lC - lB ) );
	float lenY = MAX( fabsf( lE - lC ), fabsf( lC - lA ) );
	float dirX = lD - lB, dirY = lE - lA;

	lenX = lenX > 0.0f ? EASU_Saturate( fabsf( dirX ) / lenX ) : 0.0f;
	lenY = lenY > 0.0f ? EASU_Saturate( fabsf( dirY ) / lenY ) : 0.0f;
	edge[0] = dirX;
	edge[1] = dirY;
	edge[2] = lenX * lenX + lenY * lenY;
}

// The weight of a tap at offset (ox, oy) from the output pixel, given the
// kernel's rotation and stretch, rot (FsrEasuTapF)
static ID_INLINE float EASU_Weight( float ox, float oy, const float rot[4], float lob, float clp ) {
	float vx = ox * rot[0] + oy * rot[1];
	float vy = ox * rot[2] + oy * rot[3];
	float d2 = MIN( vx * vx + vy * vy, clp );
	float wB = 2.0f / 5.0f * d2 - 1.0f;
	float wA = lob * d2 - 1.0f;

	wB *= wB;
	wA *= wA;
	return ( 25.0f / 16.0f * wB - ( 25.0f / 16.0f - 1.0f ) ) * wA;
}

static ID_INLINE byte EASU_Byte( float x ) {
	return (byte)MIN( x * 255.0f + 0.5f, 255.0f );
}

// Clamps a filtered value to the four nearest pixels' range: f, g, j, k
static ID_INLINE float EASU_Dering( float value, float f, float g, float j, float k ) {
	float lo = MIN( MIN( f, g ), MIN( j, k ) );
	float hi = MAX( MAX( f, g ), MAX( j, k ) );

	return MAX( lo, MIN( value, hi ) );
}

// Filters the output pixel whose nearest source pixel up and left, f, is at
// index f in the bordered source, x and y across from it, into out's RGBA
static void EASU_Pixel( const easuSource_t *src, int f, float x, float y, byte out[4] ) {
	const unsigned *rgba = (const unsigned *)src->rgba;
	const float *tap[12];
	const float *edge[4];
	float quad[4], w[12];
	float dir[2] = { 0, 0 }, len = 0, len2[2], rot[4];
	float dir2, stretch, lob, clp;
	float sum[4] = { 0 }, weight = 0, scale, p[4];
	int i, c;

	// where the four nearest pixels are the same, so is the output, which
	// the dering below would clamp to them: flat and transparent areas
	if ( rgba[f] == rgba[f + 1] && rgba[f] == rgba[f + src->stride] && rgba[f] == rgba[f + src->stride + 1] ) {
		memcpy( out, &rgba[f], 4 );
		return;
	}

	for ( i = 0; i < 12; i++ ) {
		tap[i] = src->pixels + ( f + src->tapOffsets[i] ) * 4;
	}

	// the edge's direction and length, from the four nearest pixels'
	// weighted bilinearly
	edge[0] = src->edges + f * 3;
	edge[1] = src->edges + ( f + 1 ) * 3;
	edge[2] = src->edges + ( f + src->stride ) * 3;
	edge[3] = src->edges + ( f + src->stride + 1 ) * 3;
	quad[0] = ( 1.0f - x ) * ( 1.0f - y );
	quad[1] = x * ( 1.0f - y );
	quad[2] = ( 1.0f - x ) * y;
	quad[3] = x * y;
	for ( i = 0; i < 4; i++ ) {
		dir[0] += edge[i][0] * quad[i];
		dir[1] += edge[i][1] * quad[i];
		len += edge[i][2] * quad[i];
	}

	dir2 = dir[0] * dir[0] + dir[1] * dir[1];
	if ( dir2 < 1.0f / 32768.0f ) {
		dir[0] = 1.0f;
		dir[1] = 0.0f;
	} else {
		float norm = 1.0f / sqrtf( dir2 );

		dir[0] *= norm;
		dir[1] *= norm;
	}

	// stretch the kernel along the edge, more as the edge is stronger, and
	// sharpen it from Lanczos-2-like towards a negative lobe of 1/4
	len *= 0.5f;
	len *= len;
	stretch = 1.0f / MAX( fabsf( dir[0] ), fabsf( dir[1] ) );
	len2[0] = 1.0f + ( stretch - 1.0f ) * len;
	len2[1] = 1.0f - 0.5f * len;
	lob = 0.5f + ( ( 1.0f / 4.0f - 0.04f ) - 0.5f ) * len;
	clp = 1.0f / lob;
	rot[0] = dir[0] * len2[0];
	rot[1] = dir[1] * len2[0];
	rot[2] = -dir[1] * len2[1];
	rot[3] = dir[0] * len2[1];

	for ( i = 0; i < 12; i++ ) {
		w[i] = EASU_Weight( easuTaps[i][0] - x, easuTaps[i][1] - y, rot, lob, clp );
		for ( c = 0; c < 4; c++ ) {
			sum[c] += tap[i][c] * w[i];
		}
		weight += w[i];
	}
	scale = 1.0f / weight;

	// dering: stay within the four nearest pixels
	for ( c = 0; c < 4; c++ ) {
		p[c] = EASU_Dering( sum[c] * scale, tap[F][c], tap[G][c], tap[J][c], tap[K][c] );
	}
	out[3] = EASU_Byte( p[3] );
	if ( out[3] ) {
		float unpremultiply = 1.0f / p[3];

		out[0] = EASU_Byte( p[0] * unpremultiply );
		out[1] = EASU_Byte( p[1] * unpremultiply );
		out[2] = EASU_Byte( p[2] * unpremultiply );
		return;
	}

	// a pixel that stays transparent keeps its straight color
	for ( c = 0; c < 3; c++ ) {
		const byte *s = src->rgba + c;
		float straight = 0;

		for ( i = 0; i < 12; i++ ) {
			straight += s[( f + src->tapOffsets[i] ) * 4] * w[i];
		}
		out[c] = EASU_Byte( EASU_Dering( straight * scale / 255.0f,
			s[f * 4] / 255.0f, s[( f + 1 ) * 4] / 255.0f,
			s[( f + src->stride ) * 4] / 255.0f, s[( f + src->stride + 1 ) * 4] / 255.0f ) );
	}
}

// Where output pixel o's center falls in a source side of size in, scaled
// to out: the nearest source pixel before it, and how far past it
static ID_INLINE int EASU_Position( int o, int in, int out, float *frac ) {
	float p = ( o + 0.5f ) * in / out - 0.5f;
	int n = (int)floorf( p );

	*frac = p - n;
	return n;
}

/*
===============
R_UpscaleImage

Upscales the RGBA image to outWidth by outHeight in out with EASU
===============
*/
static void R_UpscaleImage( const byte *pic, int width, int height, byte *out, int outWidth, int outHeight ) {
	int stride = width + 2 * EASU_BORDER, rows = height + 2 * EASU_BORDER;
	int count = stride * rows;
	float *pixels = ri.Hunk_AllocateTempMemory( count * 4 * sizeof( float ) );
	float *edges = ri.Hunk_AllocateTempMemory( count * 3 * sizeof( float ) );
	byte *rgba = ri.Hunk_AllocateTempMemory( count * 4 );
	int *nearestX = ri.Hunk_AllocateTempMemory( outWidth * ( sizeof( int ) + sizeof( float ) ) );
	float *fracX = (float *)( nearestX + outWidth );
	easuSource_t src;
	int i, x, y;

	// the source with its border, edges repeated
	for ( y = 0; y < rows; y++ ) {
		for ( x = 0; x < stride; x++ ) {
			int sx = MAX( 0, MIN( x - EASU_BORDER, width - 1 ) );
			int sy = MAX( 0, MIN( y - EASU_BORDER, height - 1 ) );
			const byte *in = pic + ( sy * width + sx ) * 4;
			float *p = pixels + ( y * stride + x ) * 4;
			float a = in[3] / 255.0f;

			memcpy( rgba + ( y * stride + x ) * 4, in, 4 );
			p[0] = in[0] / 255.0f * a;
			p[1] = in[1] / 255.0f * a;
			p[2] = in[2] / 255.0f * a;
			p[3] = a;
		}
	}

	// each pixel's edge, from its neighbors above, left, right and below;
	// the outermost ring of the border is never a nearest pixel
	for ( y = 1; y < rows - 1; y++ ) {
		for ( x = 1; x < stride - 1; x++ ) {
			const float *p = pixels + ( y * stride + x ) * 4;

			EASU_Edge( edges + ( y * stride + x ) * 3, EASU_Luma( p - stride * 4 ), EASU_Luma( p - 4 ),
				EASU_Luma( p ), EASU_Luma( p + 4 ), EASU_Luma( p + stride * 4 ) );
		}
	}

	src.rgba = rgba;
	src.pixels = pixels;
	src.edges = edges;
	src.stride = stride;
	for ( i = 0; i < 12; i++ ) {
		src.tapOffsets[i] = easuTaps[i][1] * stride + easuTaps[i][0];
	}

	for ( x = 0; x < outWidth; x++ ) {
		nearestX[x] = EASU_Position( x, width, outWidth, &fracX[x] );
	}
	for ( y = 0; y < outHeight; y++ ) {
		float fracY;
		int row = ( EASU_Position( y, height, outHeight, &fracY ) + EASU_BORDER ) * stride + EASU_BORDER;

		for ( x = 0; x < outWidth; x++ ) {
			EASU_Pixel( &src, row + nearestX[x], fracX[x], fracY, out + ( y * outWidth + x ) * 4 );
		}
	}

	// the temporary memory is freed in the reverse order
	ri.Hunk_FreeTempMemory( nearestX );
	ri.Hunk_FreeTempMemory( rgba );
	ri.Hunk_FreeTempMemory( edges );
	ri.Hunk_FreeTempMemory( pixels );
}

/*
===============
R_UpscaleCached

The image upscaled from this source to this size, moved to the front of
the cache, or NULL
===============
*/
static upscaled_t *R_UpscaleCached( const char *name, const byte *source, int width, int height,
		int outWidth, int outHeight ) {
	upscaled_t **link;

	for ( link = &upscaleCache; *link; link = &( *link )->next ) {
		upscaled_t *entry = *link;

		if ( entry->width == width && entry->height == height
			&& entry->outWidth == outWidth && entry->outHeight == outHeight
			&& !strcmp( entry->name, name ) && !memcmp( entry->source, source, width * height * 4 ) ) {
			*link = entry->next;
			entry->next = upscaleCache;
			upscaleCache = entry;
			entry->load = upscaleLoad;
			return entry;
		}
	}
	return NULL;
}

/*
===============
R_UpscaleCache

Keeps a copy of an upscaled image and its source, as many as fit in the
budget's size, past which the least recently used go. Usually one load's
images fit with room to spare; when they don't, the rest are filtered
again at the next load. Nothing is kept when memory is short
===============
*/
static void R_UpscaleCache( const char *name, const byte *source, int width, int height,
		const byte *pic, int outWidth, int outHeight ) {
	int limit = upscaleLimit;
	upscaled_t *entry;

	entry = malloc( sizeof( *entry ) );
	if ( !entry ) {
		return;
	}
	entry->source = malloc( width * height * 4 );
	entry->pic = malloc( outWidth * outHeight * 4 );
	if ( !entry->source || !entry->pic ) {
		free( entry->source );
		free( entry->pic );
		free( entry );
		return;
	}
	Q_strncpyz( entry->name, name, sizeof( entry->name ) );
	entry->width = width;
	entry->height = height;
	entry->outWidth = outWidth;
	entry->outHeight = outHeight;
	entry->bytes = ( width * height + outWidth * outHeight ) * 4;
	entry->load = upscaleLoad;
	memcpy( entry->source, source, width * height * 4 );
	memcpy( entry->pic, pic, outWidth * outHeight * 4 );
	entry->next = upscaleCache;
	upscaleCache = entry;

	R_UpscaleTrim( limit, 0 );
}

/*
===============
R_Upscale2D

Upscales an RGBA image just loaded from disk, which the caller frees with
ri.Free, if it's 2D art and the screen is big enough. powerOfTwo is for
images whose upload rounds them to powers of two, and uploadShrinks for
those it halves (r_picmip), which would undo the work
===============
*/
void R_Upscale2D( const char *name, byte **pic, int *width, int *height, qboolean powerOfTwo,
		qboolean uploadShrinks ) {
	int outWidth, outHeight, start;
	upscaled_t *cached;
	byte *upscaled;

	if ( !R_Upscale2DSize( name, *width, *height, powerOfTwo, uploadShrinks, &outWidth, &outHeight ) ) {
		return;
	}

	start = ri.Milliseconds();
	upscaled = ri.Malloc( outWidth * outHeight * 4 );
	cached = R_UpscaleCached( name, *pic, *width, *height, outWidth, outHeight );
	if ( cached ) {
		memcpy( upscaled, cached->pic, outWidth * outHeight * 4 );
	} else {
		R_UpscaleImage( *pic, *width, *height, upscaled, outWidth, outHeight );
		R_UpscaleCache( name, *pic, *width, *height, upscaled, outWidth, outHeight );
	}
	ri.Printf( PRINT_DEVELOPER, "upscaled %s, %dx%d, to %dx%d in %d ms%s\n", name, *width, *height,
		outWidth, outHeight, ri.Milliseconds() - start, cached ? ", cached" : "" );

	ri.Free( *pic );
	*pic = upscaled;
	*width = outWidth;
	*height = outHeight;
}
