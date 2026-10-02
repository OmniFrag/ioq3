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
// renderer restarts. WebGL and GLES devices have less to spare
#define UPSCALE_BUDGET		( 64 * 1024 * 1024 )
#define UPSCALE_BUDGET_GLES	( 16 * 1024 * 1024 )

// The largest upscaled image, in bytes: opengl1's upload copies it to the
// hunk's temporary memory, which a big image would exhaust
#define UPSCALE_MAX_IMAGE	( 16 * 1024 * 1024 )

static int upscaleFactor;
static int upscaleBudget;

// The folders Quake III's art is drawn from in 2D: the HUD, menus, icons
// and level shots. World textures and effects live elsewhere
static const char *upscalePaths[] = {
	"gfx/2d/", "menu/", "icons/", "ui/", "levelshots/"
};

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
R_InitUpscale2D

Registers r_upscale2D and picks the factor 2D art loads at until the
renderer restarts: the scale of the 640x480 screen in whole steps, at most
4 (2 on GLES), or 1 without r_upscale2D. Called as the renderer's images
are created
===============
*/
void R_InitUpscale2D( void ) {
	int factor = MIN( glConfig.vidWidth / 640, glConfig.vidHeight / 480 );

	r_upscale2D = ri.Cvar_Get( "r_upscale2D", "1", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_CheckRange( r_upscale2D, 0, 1, qtrue );
	ri.Cvar_SetDescription( r_upscale2D, "Sharpen the HUD and menus' art at high resolutions by upscaling it "
		"as it loads, by the 640x480 screen's scale in whole steps (up to 4, or 2 on GLES)" );

	factor = MIN( factor, qglesMajorVersion ? 2 : 4 );
	upscaleFactor = r_upscale2D->integer ? factor : 1;
	upscaleBudget = qglesMajorVersion ? UPSCALE_BUDGET_GLES : UPSCALE_BUDGET;
}

/*
===============
R_Upscale2DFactor

The factor to upscale the image by, or 1 to leave it as it is. powerOfTwo
is for images whose upload rounds them to powers of two, which would round
an image of another size, or a factor of 3, back down
===============
*/
static int R_Upscale2DFactor( const char *name, int width, int height, qboolean powerOfTwo ) {
	int factor = upscaleFactor;
	int64_t extra = 0;

	if ( factor < 2 || !R_Is2DArt( name ) ) {
		return 1;
	}
	if ( powerOfTwo && ( ( width & ( width - 1 ) ) || ( height & ( height - 1 ) ) ) ) {
		return 1;
	}

	for ( ; factor > 1; factor-- ) {
		extra = (int64_t)width * height * 4 * ( factor * factor - 1 );
		if ( ( !powerOfTwo || factor != 3 )
			&& (int64_t)width * factor <= glConfig.maxTextureSize
			&& (int64_t)height * factor <= glConfig.maxTextureSize
			&& extra <= upscaleBudget
			&& (int64_t)width * height * 4 * factor * factor <= UPSCALE_MAX_IMAGE ) {
			break;
		}
	}

	if ( factor > 1 ) {
		upscaleBudget -= extra;
	}
	return factor;
}

/*
===============
EASU, edge-adaptive spatial upsampling

Filters with a Lanczos-like kernel stretched along the local edge, then
clamps to the four nearest pixels so edges don't ring. Colors are filtered
premultiplied by their alpha, so cut-out art's transparent pixels don't
bleed into its edges, and also straight, for pixels that stay transparent:
additive shaders draw their color whatever their alpha
===============
*/

// premultiplied red, green, blue and alpha, then straight red, green and blue
#define EASU_CHANNELS	7

typedef struct {
	const byte	*pic;		// RGBA
	const float	*pixels;	// EASU_CHANNELS each, 0 to 1
	const float	*luma;
	int			width, height;
} easuSource_t;

static ID_INLINE int EASU_Index( const easuSource_t *src, int x, int y ) {
	x = MAX( 0, MIN( x, src->width - 1 ) );
	y = MAX( 0, MIN( y, src->height - 1 ) );
	return y * src->width + x;
}

static ID_INLINE float EASU_Saturate( float x ) {
	return MAX( 0.0f, MIN( x, 1.0f ) );
}

// Accumulates one quadrant's edge direction and length (FsrEasuSetF)
static void EASU_Set( float dir[2], float *len, float w,
		float lA, float lB, float lC, float lD, float lE ) {
	float lenX = MAX( fabsf( lD - lC ), fabsf( lC - lB ) );
	float lenY = MAX( fabsf( lE - lC ), fabsf( lC - lA ) );
	float dirX = lD - lB, dirY = lE - lA;

	lenX = lenX > 0.0f ? EASU_Saturate( fabsf( dirX ) / lenX ) : 0.0f;
	lenY = lenY > 0.0f ? EASU_Saturate( fabsf( dirY ) / lenY ) : 0.0f;
	dir[0] += dirX * w;
	dir[1] += dirY * w;
	*len += ( lenX * lenX + lenY * lenY ) * w;
}

// The weight of a tap at offset (ox, oy) from the output pixel (FsrEasuTapF)
static ID_INLINE float EASU_Weight( float ox, float oy, const float dir[2],
		const float len2[2], float lob, float clp ) {
	float vx = ( ox * dir[0] + oy * dir[1] ) * len2[0];
	float vy = ( ox * -dir[1] + oy * dir[0] ) * len2[1];
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

// the 12 taps around an output pixel, as FSR names them:
//     b c
//   e f g h
//   i j k l
//     n o
enum { B, C, E, F, G, H, I, J, K, L, N, O };

// Clamps a filtered channel to the four nearest pixels' range
static ID_INLINE float EASU_Dering( const float *tap[12], int c, float value ) {
	float lo = MIN( MIN( tap[F][c], tap[G][c] ), MIN( tap[J][c], tap[K][c] ) );
	float hi = MAX( MAX( tap[F][c], tap[G][c] ), MAX( tap[J][c], tap[K][c] ) );

	return MAX( lo, MIN( value, hi ) );
}

// Filters the output pixel at (px, py) in the source into out's RGBA
static void EASU_Pixel( const easuSource_t *src, float px, float py, byte out[4] ) {
	static const int tapOffsets[12][2] = {
		{ 0, -1 }, { 1, -1 },
		{ -1, 0 }, { 0, 0 }, { 1, 0 }, { 2, 0 },
		{ -1, 1 }, { 0, 1 }, { 1, 1 }, { 2, 1 },
		{ 0, 2 }, { 1, 2 }
	};
	int fx = (int)floorf( px ), fy = (int)floorf( py );
	float x = px - fx, y = py - fy;
	const float *tap[12];
	int index[12];
	float l[12], w[12];
	float dir[2] = { 0, 0 }, len = 0, len2[2];
	float dir2, stretch, lob, clp;
	float sum[4] = { 0 }, weight = 0, p[4];
	int i, c;

	for ( i = 0; i < 12; i++ ) {
		index[i] = EASU_Index( src, fx + tapOffsets[i][0], fy + tapOffsets[i][1] );
		tap[i] = src->pixels + index[i] * EASU_CHANNELS;
		l[i] = src->luma[index[i]];
	}

	// where the four nearest pixels are the same, so is the output, which
	// the dering below would clamp to them: flat and transparent areas
	if ( !memcmp( src->pic + index[F] * 4, src->pic + index[G] * 4, 4 )
		&& !memcmp( src->pic + index[F] * 4, src->pic + index[J] * 4, 4 )
		&& !memcmp( src->pic + index[F] * 4, src->pic + index[K] * 4, 4 ) ) {
		memcpy( out, src->pic + index[F] * 4, 4 );
		return;
	}

	// the edge's direction and length, from the four quadrants around the
	// pixel weighted bilinearly
	EASU_Set( dir, &len, ( 1.0f - x ) * ( 1.0f - y ), l[B], l[E], l[F], l[G], l[J] );
	EASU_Set( dir, &len, x * ( 1.0f - y ), l[C], l[F], l[G], l[H], l[K] );
	EASU_Set( dir, &len, ( 1.0f - x ) * y, l[F], l[I], l[J], l[K], l[N] );
	EASU_Set( dir, &len, x * y, l[G], l[J], l[K], l[L], l[O] );

	dir2 = dir[0] * dir[0] + dir[1] * dir[1];
	if ( dir2 < 1.0f / 32768.0f ) {
		dir[0] = 1.0f;
		dir[1] = 0.0f;
	} else {
		float scale = 1.0f / sqrtf( dir2 );

		dir[0] *= scale;
		dir[1] *= scale;
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

	for ( i = 0; i < 12; i++ ) {
		w[i] = EASU_Weight( tapOffsets[i][0] - x, tapOffsets[i][1] - y, dir, len2, lob, clp );
		for ( c = 0; c < 4; c++ ) {
			sum[c] += tap[i][c] * w[i];
		}
		weight += w[i];
	}

	// dering: stay within the four nearest pixels
	for ( c = 0; c < 4; c++ ) {
		p[c] = EASU_Dering( tap, c, sum[c] / weight );
	}
	out[3] = EASU_Byte( p[3] );
	if ( out[3] ) {
		out[0] = EASU_Byte( p[0] / p[3] );
		out[1] = EASU_Byte( p[1] / p[3] );
		out[2] = EASU_Byte( p[2] / p[3] );
		return;
	}

	// a pixel that stays transparent keeps its straight color
	for ( c = 0; c < 3; c++ ) {
		float straight = 0;

		for ( i = 0; i < 12; i++ ) {
			straight += tap[i][4 + c] * w[i];
		}
		out[c] = EASU_Byte( EASU_Dering( tap, 4 + c, straight / weight ) );
	}
}

/*
===============
R_UpscaleImage

Returns the RGBA image upscaled by factor with EASU, which the caller frees
with ri.Free
===============
*/
static byte *R_UpscaleImage( const byte *pic, int width, int height, int factor ) {
	int outWidth = width * factor, outHeight = height * factor;
	int count = width * height;
	float *pixels = ri.Malloc( count * EASU_CHANNELS * sizeof( *pixels ) );
	float *luma = ri.Malloc( count * sizeof( *luma ) );
	byte *out = ri.Malloc( outWidth * outHeight * 4 );
	easuSource_t src;
	int i, x, y;

	for ( i = 0; i < count; i++ ) {
		const byte *in = pic + i * 4;
		float *p = pixels + i * EASU_CHANNELS;
		float a = in[3] / 255.0f;

		p[4] = in[0] / 255.0f;
		p[5] = in[1] / 255.0f;
		p[6] = in[2] / 255.0f;
		p[0] = p[4] * a;
		p[1] = p[5] * a;
		p[2] = p[6] * a;
		p[3] = a;
		luma[i] = 0.5f * p[2] + 0.5f * p[0] + p[1];
	}

	src.pic = pic;
	src.pixels = pixels;
	src.luma = luma;
	src.width = width;
	src.height = height;

	for ( y = 0; y < outHeight; y++ ) {
		float py = ( y + 0.5f ) / factor - 0.5f;

		for ( x = 0; x < outWidth; x++ ) {
			float px = ( x + 0.5f ) / factor - 0.5f;

			EASU_Pixel( &src, px, py, out + ( y * outWidth + x ) * 4 );
		}
	}

	ri.Free( pixels );
	ri.Free( luma );
	return out;
}

/*
===============
R_Upscale2D

Upscales an RGBA image just loaded from disk, which the caller frees with
ri.Free, if it's 2D art and the screen is big enough. powerOfTwo is for
images whose upload rounds them to powers of two
===============
*/
void R_Upscale2D( const char *name, byte **pic, int *width, int *height, qboolean powerOfTwo ) {
	int factor = R_Upscale2DFactor( name, *width, *height, powerOfTwo );
	byte *upscaled;
	int start;

	if ( factor < 2 ) {
		return;
	}

	start = ri.Milliseconds();
	upscaled = R_UpscaleImage( *pic, *width, *height, factor );
	ri.Printf( PRINT_DEVELOPER, "upscaled %s, %dx%d, by %d in %d ms\n",
		name, *width, *height, factor, ri.Milliseconds() - start );
	ri.Free( *pic );
	*pic = upscaled;
	*width *= factor;
	*height *= factor;
}
