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
//
#ifndef __TR_TYPES_H
#define __TR_TYPES_H


#define	MAX_DLIGHTS		32		// can't be increased, because bit flags are used on surfaces

#define	REFENTITYNUM_BITS	10		// can't be increased without changing drawsurf bit packing
#define	REFENTITYNUM_MASK	((1<<REFENTITYNUM_BITS) - 1)
// the last N-bit number (2^REFENTITYNUM_BITS - 1) is reserved for the special world refentity,
//  and this is reflected by the value of MAX_REFENTITIES (which therefore is not a power-of-2)
#define	MAX_REFENTITIES		((1<<REFENTITYNUM_BITS) - 1)
#define	REFENTITYNUM_WORLD	((1<<REFENTITYNUM_BITS) - 1)

// renderfx flags
#define	RF_MINLIGHT		0x0001		// allways have some light (viewmodel, some items)
#define	RF_THIRD_PERSON		0x0002		// don't draw through eyes, only mirrors (player bodies, chat sprites)
#define	RF_FIRST_PERSON		0x0004		// only draw through eyes (view weapon, damage blood blob)
#define	RF_DEPTHHACK		0x0008		// for view weapon Z crunching

#define RF_CROSSHAIR		0x0010		// This item is a cross hair and will draw over everything similar to
						// DEPTHHACK in stereo rendering mode, with the difference that the
						// projection matrix won't be hacked to reduce the stereo separation as
						// is done for the gun.

#define	RF_NOSHADOW		0x0040		// don't add stencil shadows

#define RF_LIGHTING_ORIGIN	0x0080		// use refEntity->lightingOrigin instead of refEntity->origin
						// for lighting.  This allows entities to sink into the floor
						// with their origin going solid, and allows all parts of a
						// player to get the same lighting

#define	RF_SHADOW_PLANE		0x0100		// use refEntity->shadowPlane
#define	RF_WRAP_FRAMES		0x0200		// mod the model frames by the maxframes to allow continuous
										// animation without needing to know the frame count

// refdef flags
#define RDF_NOWORLDMODEL	0x0001		// used for player configuration screen
#define RDF_HYPERSPACE		0x0004		// teleportation effect

typedef struct {
	vec3_t		xyz;
	float		st[2];
	byte		modulate[4];
} polyVert_t;

typedef struct poly_s {
	qhandle_t			hShader;
	int					numVerts;
	polyVert_t			*verts;
} poly_t;

// a vertex of 2D triangles (DrawTriangles2D): where it is in window pixels,
// x right and y down from the top left, its color and its texture
// coordinates; 20 bytes, in the order RmlUi's vertexes have
typedef struct {
	float		xy[2];
	byte		rgba[4];
	float		st[2];
} polyVert2D_t;

#define TRIS2D_SCISSOR		0x0001	// draw only inside scissor
#define TRIS2D_TRANSFORM	0x0002	// transform the vertexes first
#define TRIS2D_QUADS		0x0004	// no indexes: each four vertexes a quad, clockwise
#define TRIS2D_PREMULTIPLIED	0x0008	// colors and texture premultiplied: ONE, ONE_MINUS_SRC_ALPHA
#define TRIS2D_TEXTURE2D	0x0010	// texture names a UI texture, not a shader
#define TRIS2D_CLIPMASK		0x0020	// draw only where the clip mask is set
#define TRIS2D_PAINT		0x0040	// color from paint (a gradient or a shape), times the vertex color

// how 2D triangles are drawn; a renderer that doesn't draw a flag yet
// leaves the batch out, with a warning
typedef struct {
	int			texture;		// a shader handle, or a UI texture with TRIS2D_TEXTURE2D; 0 for white
	int			flags;			// TRIS2D_*
	int			scissor[4];		// x, y, width and height in window pixels
	// row-major 3x3 and homogeneous: x' = ( m0 x + m1 y + m2 ) / w and
	// y' = ( m3 x + m4 y + m5 ) / w, with w = m6 x + m7 y + m8, which must
	// stay above 0
	float		transform[9];
	float		translate[2];	// added first, RmlUi's translation, with or without the transform
	int			paint;			// with TRIS2D_PAINT
} draw2DState_t;

// indexed triangles drawn in 2D, as a shader draws a picture: a shader made
// from an image (RegisterShaderNoMip, CreateImage2D) multiplies it by the
// vertex colors and blends it by its alpha
typedef struct {
	draw2DState_t		state;
	const polyVert2D_t	*verts;
	int					numVerts;
	const int			*indexes;		// three a triangle, each below numVerts;
	int					numIndexes;		// none with TRIS2D_QUADS
} tris2D_t;

// what a renderer's 2D path can do (GetCaps2D)
typedef struct {
	qboolean	textureA8;			// A8 textures without a conversion to RGBA8
	qboolean	clipMask;			// stencil clip masks
	qboolean	paints;				// gradients and rounded boxes on the GPU
	qboolean	msdf;				// multi-channel distance field text
	qboolean	layers;				// layers and their filters (PushLayer2D)
	int			multisample;		// samples of the 2D target, and so of the layers; 0 for none
	int			maxTextureSize;		// an image's widest side
	int			maxGeometryVerts;	// a retained geometry's vertexes, 0 for none
} caps2D_t;

// a UI texture's pixels (CreateTexture2D)
typedef enum {
	TEX2D_RGBA8,		// straight alpha
	TEX2D_RGBA8_PREMUL,	// premultiplied alpha
	TEX2D_A8			// coverage, a byte a pixel: white with this alpha (glyphs)
} tex2DFormat_t;

// what SetClipMask2D does with the clip mask, which draws with
// TRIS2D_CLIPMASK test
typedef enum {
	CLIPMASK_OFF,			// the draws stop testing, until a new mask
	CLIPMASK_SET,			// the mask becomes the geometry
	CLIPMASK_SET_INVERSE,	// the mask becomes everything but the geometry
	CLIPMASK_INTERSECT		// the mask keeps only what the geometry covers too
} clipMaskOp_t;

// A paint colors 2D triangles drawn with TRIS2D_PAINT, by their texture
// coordinates, which say where each vertex is in the paint's space (the
// element's box, as RmlUi gives them); the vertex colors multiply it. A
// shape anti-aliases its own edges
typedef enum {
	PAINT2D_LINEAR,			// p0 to p1
	PAINT2D_RADIAL,			// centered on p0, p1 the radii of where it ends
	PAINT2D_CONIC,			// around p0, starting at p1[0] radians clockwise from up
	PAINT2D_ROUNDED_BOX,	// box with radii corners, its edge softened over feather pixels (a soft shadow)
	PAINT2D_MSDF			// the coverage of a multi-channel distance field in the UI texture (TRIS2D_TEXTURE2D)
} paint2DKind_t;

#define MAX_PAINT2D_STOPS	16

typedef struct {
	paint2DKind_t	kind;
	qboolean		repeating;		// the stops again, past the first and the last
	float			p0[2], p1[2];
	int				numStops;		// a gradient's, 2 or more, positions in order: 0 at the start, 1 at the end
	float			stopPos[MAX_PAINT2D_STOPS];
	byte			stopColor[MAX_PAINT2D_STOPS][4];	// premultiplied
	float			box[4], radii[4], feather;		// rounded box: rectangle, corner radii, edge softness
	float			msdfRange;						// MSDF: the field's range in the texture's pixels
	// MSDF: a texel, across and down in the texture's pixels, of a raster the
	// glyph stands in for, with premultiplied colors drawn as straight alpha:
	// its color falls toward the outline as that raster's coverage does, to
	// half on it, a band one such texel wide (0, 0 for none). Measured with
	// msdfRange, which must then be the field's own range
	float			msdfSourceTexel[2];
} paint2D_t;

// how CompositeLayers2D puts a layer onto another
typedef enum {
	BLEND2D_BLEND,		// over it, premultiplied
	BLEND2D_REPLACE		// instead of it
} blend2DMode_t;

// the most filters one CompositeLayers2D applies
#define MAX_FILTERS2D_PER_COMPOSITE	8

typedef enum {
	RT_MODEL,
	RT_POLY,
	RT_SPRITE,
	RT_BEAM,
	RT_RAIL_CORE,
	RT_RAIL_RINGS,
	RT_LIGHTNING,
	RT_PORTALSURFACE,		// doesn't draw anything, just info for portals

	RT_MAX_REF_ENTITY_TYPE
} refEntityType_t;

typedef struct {
	refEntityType_t	reType;
	int			renderfx;

	qhandle_t	hModel;				// opaque type outside refresh

	// most recent data
	vec3_t		lightingOrigin;		// so multi-part models can be lit identically (RF_LIGHTING_ORIGIN)
	float		shadowPlane;		// projection shadows go here, stencils go slightly lower

	vec3_t		axis[3];			// rotation vectors
	qboolean	nonNormalizedAxes;	// axis are not normalized, i.e. they have scale
	float		origin[3];			// also used as MODEL_BEAM's "from"
	int			frame;				// also used as MODEL_BEAM's diameter

	// previous data for frame interpolation
	float		oldorigin[3];		// also used as MODEL_BEAM's "to"
	int			oldframe;
	float		backlerp;			// 0.0 = current, 1.0 = old

	// texturing
	int			skinNum;			// inline skin index
	qhandle_t	customSkin;			// NULL for default skin
	qhandle_t	customShader;		// use one image for the entire thing

	// misc
	byte		shaderRGBA[4];		// colors used by rgbgen entity shaders
	float		shaderTexCoord[2];	// texture coordinates used by tcMod entity modifiers
	float		shaderTime;			// subtracted from refdef time to control effect start times

	// extra sprite information
	float		radius;
	float		rotation;
} refEntity_t;


#define	MAX_RENDER_STRINGS			8
#define	MAX_RENDER_STRING_LENGTH	32

typedef struct {
	int			x, y, width, height;
	float		fov_x, fov_y;
	vec3_t		vieworg;
	vec3_t		viewaxis[3];		// transformation matrix

	// time in milliseconds for shader effects and other time dependent rendering issues
	int			time;

	int			rdflags;			// RDF_NOWORLDMODEL, etc

	// 1 bits will prevent the associated area from rendering at all
	byte		areamask[MAX_MAP_AREA_BYTES];

	// text messages for deform text shaders
	char		text[MAX_RENDER_STRINGS][MAX_RENDER_STRING_LENGTH];
} refdef_t;


typedef enum {
	STEREO_CENTER,
	STEREO_LEFT,
	STEREO_RIGHT
} stereoFrame_t;


/*
** glconfig_t
**
** Contains variables specific to the OpenGL configuration
** being run right now.  These are constant once the OpenGL
** subsystem is initialized.
*/
typedef enum {
	TC_NONE,
	TC_S3TC,  // this is for the GL_S3_s3tc extension.
	TC_S3TC_ARB  // this is for the GL_EXT_texture_compression_s3tc extension.
} textureCompression_t;

typedef enum {
	GLDRV_ICD,					// driver is integrated with window system
								// WARNING: there are tests that check for
								// > GLDRV_ICD for minidriverness, so this
								// should always be the lowest value in this
								// enum set
	GLDRV_STANDALONE,			// driver is a non-3Dfx standalone driver
	GLDRV_VOODOO				// driver is a 3Dfx standalone driver
} glDriverType_t;

typedef enum {
	GLHW_GENERIC,			// where everything works the way it should
	GLHW_3DFX_2D3D,			// Voodoo Banshee or Voodoo3, relevant since if this is
							// the hardware type then there can NOT exist a secondary
							// display adapter
	GLHW_RIVA128,			// where you can't interpolate alpha
	GLHW_RAGEPRO,			// where you can't modulate alpha on alpha textures
	GLHW_PERMEDIA2			// where you don't have src*dst
} glHardwareType_t;

typedef struct {
	char					renderer_string[MAX_STRING_CHARS];
	char					vendor_string[MAX_STRING_CHARS];
	char					version_string[MAX_STRING_CHARS];
	char					extensions_string[BIG_INFO_STRING];

	int						maxTextureSize;			// queried from GL
	int						numTextureUnits;		// multitexture ability

	int						colorBits, depthBits, stencilBits;

	glDriverType_t			driverType;
	glHardwareType_t		hardwareType;

	qboolean				deviceSupportsGamma;
	textureCompression_t	textureCompression;
	qboolean				textureEnvAddAvailable;

	int						vidWidth, vidHeight;
	// aspect is the screen's physical width / height, which may be different
	// than scrWidth / scrHeight if the pixels are non-square
	// normal screens should be 4/3, but wide aspect monitors may be 16/9
	float					windowAspect;

	int						displayFrequency;

	// synonymous with "does rendering consume the entire screen?", therefore
	// a Voodoo or Voodoo2 will have this set to TRUE, as will a Win32 ICD that
	// used CDS.
	qboolean				isFullscreen;
	qboolean				stereoEnabled;
	qboolean				smpActive;		// UNUSED, present for compatibility
} glconfig_t;

#endif	// __TR_TYPES_H
