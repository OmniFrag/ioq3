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
#ifndef TR_COMMON_H
#define TR_COMMON_H

#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"
#include "tr_2d.h"
#include "qgl.h"

typedef enum
{
	IMGTYPE_COLORALPHA, // for color, lightmap, diffuse, and specular
	IMGTYPE_NORMAL,
	IMGTYPE_NORMALHEIGHT,
	IMGTYPE_DELUXE, // normals are swizzled, deluxe are not
} imgType_t;

typedef enum
{
	IMGFLAG_NONE           = 0x0000,
	IMGFLAG_MIPMAP         = 0x0001,
	IMGFLAG_PICMIP         = 0x0002,
	IMGFLAG_CUBEMAP        = 0x0004,
	IMGFLAG_NO_COMPRESSION = 0x0010,
	IMGFLAG_NOLIGHTSCALE   = 0x0020,
	IMGFLAG_CLAMPTOEDGE    = 0x0040,
	IMGFLAG_GENNORMALMAP   = 0x0080,
	IMGFLAG_DYNAMIC        = 0x0100,	// its rectangles may be replaced (RE_UpdateImage2D)
} imgFlags_t;

typedef struct image_s {
	char		imgName[MAX_QPATH];		// game path, including extension
	char		fileName[MAX_QPATH];	// the file it was read from, "" for one made in code
	int			width, height;				// source image
	int			fileWidth, fileHeight;		// the file's, before R_Upscale2D; width and height for an image made in code
	int			uploadWidth, uploadHeight;	// after power of two and picmip but not including clamp to MAX_TEXTURE_SIZE
	GLuint		texnum;					// gl texture binding

	int			frameUsed;			// for texture usage in frame statistics

	int			internalFormat;
	GLenum		dataFormat, dataType;	// what renderergl2 allocated its storage with
	int			TMU;				// only needed for voodoo2

	imgType_t   type;
	imgFlags_t  flags;

	struct image_s*	next;
} image_t;

// any change in the LIGHTMAP_* defines here MUST be reflected in
// R_FindShader() in tr_bsp.c
#define LIGHTMAP_2D         -4	// shader is for 2D rendering
#define LIGHTMAP_BY_VERTEX  -3	// pre-lit triangle models
#define LIGHTMAP_WHITEIMAGE -2
#define LIGHTMAP_NONE       -1

extern	refimport_t		ri;
extern glconfig_t	glConfig;		// outside of TR since it shouldn't be cleared during ref re-init

// These variables should live inside glConfig but can't because of
// compatibility issues to the original ID vms.  If you release a stand-alone
// game and your mod uses tr_types.h from this build you can safely move them
// to the glconfig_t struct.
extern qboolean  textureFilterAnisotropic;
extern int       maxAnisotropy;
extern float     displayAspect;
extern qboolean  haveClampToEdge;

//
// cvars
//
extern cvar_t *r_stencilbits;			// number of desired stencil bits
extern cvar_t *r_depthbits;			// number of desired depth bits
extern cvar_t *r_colorbits;			// number of desired color bits, only relevant for fullscreen
extern cvar_t *r_texturebits;			// number of desired texture bits
extern cvar_t *r_ext_multisample;
										// 0 = use framebuffer depth
										// 16 = use 16-bit textures
										// 32 = use 32-bit textures
										// all else = error

extern cvar_t *r_mode;				// video mode
extern cvar_t *r_customwidth;
extern cvar_t *r_customheight;
extern cvar_t *r_noborder;
extern cvar_t *r_fullscreen;
extern cvar_t *r_ignorehwgamma;		// overrides hardware gamma capabilities
extern cvar_t *r_drawBuffer;
extern cvar_t *r_swapInterval;

extern cvar_t *r_allowExtensions;				// global enable/disable of OpenGL extensions
extern cvar_t *r_ext_compressed_textures;		// these control use of specific extensions
extern cvar_t *r_ext_multitexture;
extern cvar_t *r_ext_compiled_vertex_array;
extern cvar_t *r_ext_texture_env_add;

extern cvar_t *r_ext_texture_filter_anisotropic;
extern cvar_t *r_ext_max_anisotropy;

extern cvar_t *r_stereoEnabled;

extern	cvar_t	*r_saveFontData;

extern cvar_t *r_viewScale;		// the 3D view's resolution, a fraction of its size in pixels
extern cvar_t *r_upscale2D;		// sharpen 2D art at high resolutions

qboolean	R_GetModeInfo( int *width, int *height, float *windowAspect, int mode );

/*
================
R_ScaleViewSide

A side of the 3D view, in pixels, at r_viewScale's scale: rounded, at
least 64 pixels unless the side is smaller, and at most maxSide
================
*/
static ID_INLINE int R_ScaleViewSide( int side, float scale, int maxSide ) {
	int scaled = (int)( side * scale + 0.5f );

	scaled = MAX( scaled, MIN( side, 64 ) );
	return MIN( scaled, maxSide );
}

float R_NoiseGet4f( float x, float y, float z, double t );
void  R_NoiseInit( void );

image_t     *R_FindImageFile( const char *name, imgType_t type, imgFlags_t flags );
image_t *R_CreateImage( const char *name, byte *pic, int width, int height, imgType_t type, imgFlags_t flags, int internalFormat );

void R_InitUpscale2D( void );
void R_ShutdownUpscale2D( void );
void R_Upscale2D( const char *name, byte **pic, int *width, int *height, qboolean powerOfTwo, qboolean uploadShrinks );
qboolean R_Is2DArt( const char *name );

void R_IssuePendingRenderCommands( void );
qhandle_t		 RE_RegisterShaderLightMap( const char *name, int lightmapIndex );
qhandle_t		 RE_RegisterShader( const char *name );
qhandle_t		 RE_RegisterShaderNoMip( const char *name );
qhandle_t RE_RegisterShaderFromImage(const char *name, int lightmapIndex, image_t *image, qboolean mipRawImage);

// font stuff
void R_InitFreeType( void );
void R_DoneFreeType( void );
void RE_RegisterFont(const char *fontName, int pointSize, fontInfo_t *font);

/*
=============================================================

IMAGE LOADERS

=============================================================
*/

// the largest image the loaders decode: a side every OpenGL ES 3 and WebGL 2
// device can draw, and the memory a 2048-square 8-bit RGBA PNG takes to
// decode (its picture, and its data with a filter byte a row, under two a
// row interlaced), a third of the default zone (com_zoneMegs 48) left over
#define MAX_IMAGE_SIDE	2048
#define MAX_IMAGE_BYTES	( 2 * MAX_IMAGE_SIDE * MAX_IMAGE_SIDE * 4 + 2 * MAX_IMAGE_SIDE )

qboolean R_CheckImageSize( const char *name, int width, int height, int64_t bytes );

void R_LoadBMP( const char *name, byte **pic, int *width, int *height );
void R_LoadJPG( const char *name, byte **pic, int *width, int *height );
void R_LoadPCX( const char *name, byte **pic, int *width, int *height );
void R_LoadPNG( const char *name, byte **pic, int *width, int *height );
void R_LoadPVR( const char *name, byte **pic, int *width, int *height );
void R_LoadTGA( const char *name, byte **pic, int *width, int *height );

/*
=============================================================

MODEL CHECKS

=============================================================
*/

qboolean R_ModelBlockInside( int64_t offset, int64_t count, int64_t size, int64_t end );
qboolean R_ValidateMD3( const void *buffer, int fileSize, const char *name );
qboolean R_ValidateMDR( const void *buffer, int fileSize, const char *name );

/*
====================================================================

The UI's retained 2D (tr_ui2d_common.c): the front end of tr_public.h's
CreateTexture2D to DeletePaint2D, which both renderers share, and the
back end's commands that need no more of a renderer than its tr_ui2d.c
gives (r_renderer2D)

====================================================================
*/

#define MAX_TEXTURES2D			4096
#define MAX_GEOMETRIES2D		16384
#define MAX_GEOMETRY2D_VERTS	65536
#define MAX_GEOMETRY2D_BYTES	( 16 * 1024 * 1024 )	// the vertexes and indexes of all of them
#define MAX_PAINTS2D			1024
#define MAX_FILTERS2D			1024
#define MAX_LAYERS2D			8		// layers pushed at once, past the 2D target

typedef struct {
	qboolean		deleted;		// its delete waits among the frame's commands
	polyVert2D_t	*verts;
	int				*indexes;
	int				numVerts, numIndexes;
	polyVert2D_t	*straightVerts;	// with straight colors, made when first asked for (R_StraightVerts2D)
	void			*gpu;			// the renderer's copy, opengl2's buffers (malloc'd)
} geometry2D_t;

// a UI texture, which the renderer draws with image
typedef struct {
	qboolean		deleted;
	image_t			image;			// not in the renderer's table of images
	tex2DFormat_t	format;
} texture2D_t;

typedef enum {
	HANDLE2D_TEXTURE,
	HANDLE2D_GEOMETRY,
	HANDLE2D_PAINT,
	HANDLE2D_FILTER,
	HANDLE2D_KINDS
} handle2DKind_t;

// a filter (CreateFilter2D), with its numbers
typedef enum {
	FILTER2D_OPACITY,		// alpha
	FILTER2D_BLUR,			// sigma
	FILTER2D_DROP_SHADOW,	// red, green, blue, alpha, x, y, sigma
	FILTER2D_COLOR_MATRIX	// a 4x5 matrix by rows
} filter2DKind_t;

#define MAX_FILTER2D_PARAMS	20

typedef struct {
	filter2DKind_t	kind;
	float			params[MAX_FILTER2D_PARAMS];
} filter2D_t;

// the 2D state's flags that tr_ui2d_common.c's draws take, past the
// immediate batch's
#define TRIS2D_RETAINED_FLAGS	( TRIS2D_PREMULTIPLIED | TRIS2D_TEXTURE2D | TRIS2D_CLIPMASK | TRIS2D_PAINT )
#define TRIS2D_ALL_FLAGS		( TRIS2D_IMMEDIATE_FLAGS | TRIS2D_RETAINED_FLAGS )

// a 2D state drawn with a shader's stages, through the tessellator, rather
// than the renderer's own UI path
#define R_ShaderState2D( state ) ( ( state )->texture && !( ( state )->flags & ( TRIS2D_TEXTURE2D | TRIS2D_PAINT ) ) )

// a premultiplied color channel straight, rounded
#define R_StraightChannel2D( c, a ) ( ( a ) ? MIN( 255, ( ( c ) * 255 + ( a ) / 2 ) / ( a ) ) : 0 )

// the render commands, after their renderCommand_t
typedef struct {
	int				commandId;
	int				geometry;			// a retained geometry; 0 for the vertexes and indexes after this
	int				numVerts, numIndexes;
	draw2DState_t	state;
} draw2DCommand_t;

typedef struct {
	int				commandId;
	clipMaskOp_t	op;
	int				geometry;
	draw2DState_t	state;
} clipMask2DCommand_t;

typedef struct {
	int				commandId;
	handle2DKind_t	kind;
	int				handle;
} delete2DCommand_t;

typedef enum {
	LAYER2D_PUSH,
	LAYER2D_POP,
	LAYER2D_COMPOSITE
} layer2DOp_t;

// filters, once they're checked, are copied in
typedef struct {
	int				commandId;
	layer2DOp_t		op;
	int				source, destination;
	blend2DMode_t	blendMode;
	draw2DState_t	state;
	int				numFilters;
	filter2D_t		filters[MAX_FILTERS2D_PER_COMPOSITE];
} layer2DCommand_t;

// followed by width * height pixels in the texture's format
typedef struct {
	int				commandId;
	int				texture;
	int				x, y, width, height;
	int				bytes;
} updateTexture2DCommand_t;

// what each renderer gives tr_ui2d_common.c (tr_ui2d.c)
typedef struct {
	int		drawCommand, clipMaskCommand, deleteCommand, updateCommand, layerCommand;	// its renderCommand_t
	int		maxCommandBytes;							// MAX_RENDER_COMMANDS
	// what it draws (RE_GetCaps2D's retained part)
	void	(*caps)( caps2D_t *caps );
	// makes a texture's GL texture, from pixels or transparent black
	void	(*createTexture)( texture2D_t *t, const byte *pixels );
	// replaces a rectangle of it now, among the back end's commands
	void	(*updateTexture)( texture2D_t *t, int x, int y, int width, int height, const byte *pixels );
	// frees what a texture or a geometry has in GL, in the back end
	void	(*freeGpu)( handle2DKind_t kind, void *object );
	// the back end: the 2D target, projection and a state's scissor, with
	// what waits drawn; and a geometry drawn unseen, into the stencil
	void	(*begin)( const draw2DState_t *state );
	void	(*drawUnseen)( const draw2DState_t *state, geometry2D_t *g );
} renderer2D_t;

extern const renderer2D_t	r_renderer2D;

void				*R_GetCommandBuffer( int bytes );
void				R_IssuePendingRenderCommands( void );

// the front end: refexport_t's entries
void				R_InitStore2D( void );
void				R_ShutdownStore2D( qboolean freeGpu );
int					R_CreateTexture2D( int width, int height, tex2DFormat_t format, const byte *pixels );
void				R_UpdateTexture2D( int texture, int x, int y, int width, int height, const byte *pixels );
void				R_DeleteTexture2D( int texture );
int					R_CreateGeometry2D( const polyVert2D_t *verts, int numVerts, const int *indexes, int numIndexes );
void				R_DrawGeometry2D( int geometry, const draw2DState_t *state );
void				R_DeleteGeometry2D( int geometry );
void				R_SetClipMask2D( clipMaskOp_t op, int geometry, const draw2DState_t *state );
int					R_CreatePaint2D( const paint2D_t *paint );
void				R_DeletePaint2D( int paint );
int					R_PushLayer2D( void );
void				R_CompositeLayers2D( int source, int destination, int blendMode, const int *filters, int numFilters,
						const draw2DState_t *state );
void				R_PopLayer2D( void );
int					R_CreateFilter2D( const char *name, const float *params, int numParams );
void				R_DeleteFilter2D( int filter );
// the frame's layers are popped as it ends (RE_EndFrame)
void				R_EndFrame2D( void );
// an immediate batch with TRIS2D_RETAINED_FLAGS
void				R_DrawTriangles2DState( const tris2D_t *tris );

// for the back end
texture2D_t			*R_GetTexture2D( int texture, qboolean deleted );
geometry2D_t		*R_GetGeometry2D( int geometry, qboolean deleted );
const paint2D_t		*R_GetPaint2D( int paint, qboolean deleted );
int					R_TexelBytes2D( tex2DFormat_t format );
void				R_StraightColors2D( polyVert2D_t *verts, int numVerts );
const polyVert2D_t	*R_StraightVerts2D( geometry2D_t *g );
void				R_Transform2DMatrix( const draw2DState_t *state, float out[16] );
void				R_Transform2DPoint( const float matrix[16], const float in[2], float out[2] );
qboolean			R_BeginClipTest2D( const draw2DState_t *state );
void				R_EndClipTest2D( void );
const void			*RB_ClipMask2D( const void *data );
const void			*RB_Delete2D( const void *data );
const void			*RB_UpdateTexture2D( const void *data );

/*
====================================================================

IMPLEMENTATION SPECIFIC FUNCTIONS

====================================================================
*/

void		GLimp_Init( qboolean fixedFunction );
void		GLimp_Shutdown( void );
void		GLimp_EndFrame( void );
qboolean	GLimp_SetFullscreen( qboolean fullscreen );
qboolean	GLimp_WindowSizeIsOwn( void );
qboolean	GLimp_FrameReady( void );
qboolean	GLimp_UpdateWindowSize( void );

void		GLimp_LogComment( char *comment );
void		GLimp_Minimize(void);

void		GLimp_SetGamma( unsigned char red[256],
		unsigned char green[256],
		unsigned char blue[256] );

// tr_times2d.c: ui_speeds' times for the back end's 2D commands. While
// r_times2D is set, the back end calls R_Times2DCommand before each
// command, and where it says a run ends, draws what the tessellator holds
// and calls R_Times2DEndRun
extern int	r_times2D;

qboolean	R_Times2DCommand( qboolean is2D );
void		R_Times2DEndRun( void );
void		RE_TimeBackEnd2D( times2D_t *times, qboolean on );
void		R_Times2DShutdown( void );


#endif
