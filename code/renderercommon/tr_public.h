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
#ifndef __TR_PUBLIC_H
#define __TR_PUBLIC_H

#include "tr_types.h"

#define	REF_API_VERSION		18

// ui_speeds: the back end's 2D commands (each renderer's RC_IS_2D) since
// they were last asked for, timed in runs, each from its first
// command to the draw of what it left
#define MAX_GPU_TIMES2D		4

typedef struct {
	int64_t		cpu;		// the runs' time on the CPU, in ri.Nanoseconds
	int			runs;
	qboolean	gpuTimer;	// whether the GPU times them too
	int			gpuUntimed;	// runs it didn't, for want of a free query
	// earlier frames whose runs' GPU time came back since, oldest first,
	// in nanoseconds; the GPU answers a frame or a few late
	int			gpuFrames;
	int64_t		gpu[MAX_GPU_TIMES2D];
} times2D_t;

// the window's size after a crash (r_mode), whatever the player chose
#define R_MODE_FALLBACK		3	// 640 * 480

//
// these are the functions exported by the refresh module
//
typedef struct {
	// called before the library is unloaded
	// if the system is just reconfiguring, pass destroyWindow = qfalse,
	// which will keep the screen from flashing to the desktop.
	void	(*Shutdown)( qboolean destroyWindow );

	// All data that will be used in a level should be
	// registered before rendering any frames to prevent disk hits,
	// but they can still be registered at a later time
	// if necessary.
	//
	// BeginRegistration makes any existing media pointers invalid
	// and returns the current gl configuration, including screen width
	// and height, which can be used by the client to intelligently
	// size display elements
	void	(*BeginRegistration)( glconfig_t *config );
	qhandle_t (*RegisterModel)( const char *name );
	qhandle_t (*RegisterSkin)( const char *name );
	qhandle_t (*RegisterShader)( const char *name );
	qhandle_t (*RegisterShaderNoMip)( const char *name );
	void	(*LoadWorld)( const char *name );

	// the vis data is a large enough block of data that we go to the trouble
	// of sharing it with the clipmodel subsystem
	void	(*SetWorldVisData)( const byte *vis );

	// EndRegistration will draw a tiny polygon with each texture, forcing
	// them to be loaded into card memory
	void	(*EndRegistration)( void );

	// a scene is built up by calls to R_ClearScene and the various R_Add functions.
	// Nothing is drawn until R_RenderScene is called.
	void	(*ClearScene)( void );
	void	(*AddRefEntityToScene)( const refEntity_t *re );
	void	(*AddPolyToScene)( qhandle_t hShader , int numVerts, const polyVert_t *verts, int num );
	int		(*LightForPoint)( vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir );
	void	(*AddLightToScene)( const vec3_t org, float intensity, float r, float g, float b );
	void	(*AddAdditiveLightToScene)( const vec3_t org, float intensity, float r, float g, float b );
	void	(*RenderScene)( const refdef_t *fd );

	void	(*SetColor)( const float *rgba );	// NULL = 1,1,1,1
	void	(*DrawStretchPic) ( float x, float y, float w, float h, 
		float s1, float t1, float s2, float t2, qhandle_t hShader );	// 0 = white

	// Draw images for cinematic rendering, pass as 32 bit rgba
	void	(*DrawStretchRaw) (int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty);
	void	(*UploadCinematic) (int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty);

	void	(*BeginFrame)( stereoFrame_t stereoFrame );

	// if the pointers are not NULL, timing info will be returned
	void	(*EndFrame)( int *frontEndMsec, int *backEndMsec );


	int		(*MarkFragments)( int numPoints, const vec3_t *points, const vec3_t projection,
				   int maxPoints, vec3_t pointBuffer, int maxFragments, markFragment_t *fragmentBuffer );

	int		(*LerpTag)( orientation_t *tag,  qhandle_t model, int startFrame, int endFrame, 
					 float frac, const char *tagName );
	void	(*ModelBounds)( qhandle_t model, vec3_t mins, vec3_t maxs );

#ifdef __USEA3D
	void    (*A3D_RenderGeometry) (void *pVoidA3D, void *pVoidGeom, void *pVoidMat, void *pVoidGeomStatus);
#endif
	void	(*RegisterFont)(const char *fontName, int pointSize, fontInfo_t *font);
	void	(*RemapShader)(const char *oldShader, const char *newShader, const char *offsetTime);
	qboolean (*GetEntityToken)( char *buffer, int size );
	qboolean (*inPVS)( const vec3_t p1, const vec3_t p2 );

	void (*TakeVideoFrame)( int h, int w, byte* captureBuffer, byte *encodeBuffer, qboolean motionJpeg );

	// Follows a change in the window's size without a restart and returns
	// the new configuration. Returns qfalse if the renderer can't, and a
	// vid_restart is needed.
	qboolean (*ResizeWindow)( glconfig_t *config );

	// Asks for the window to go fullscreen, or to leave it; what happens
	// shows in its state (SDL_GetWindowFlags). Returns qfalse if the window
	// can't change in place, and must be made again. Not on the web.
	qboolean (*SetFullscreen)( qboolean fullscreen );

	// Whether the window has the size the renderer last gave it (r_mode's,
	// or fitted to the desktop), so that the client saves only the
	// player's resizes
	qboolean (*WindowSizeIsOwn)( void );

	// Whether the GPU is ready for a new frame, without waiting: where the
	// renderer can't wait for it after the swap (WebGL), the client skips
	// a frame instead while the last is still being drawn. May be NULL.
	qboolean (*FrameReady)( void );

	// Draws indexed triangles in 2D (tris2D_t), merged with the batches and
	// pictures around them that have the same shader and scissor. The
	// arrays are copied, so they can be reused at once. A batch that's
	// malformed, or larger than half the frame's command buffer, is left
	// out with a warning.
	void	(*DrawTriangles2D)( const tris2D_t *tris );

	// Makes an image of RGBA pixels with straight alpha, width by height
	// (NULL for transparent black), and a shader drawing it in 2D, whose
	// handle it returns, 0 when it can't. Its name must start with '*',
	// which no file's does, and be new, or one this made before at the same
	// size, which is given again with these pixels (for a module that has
	// restarted since). Its rectangles can be replaced; it lasts until the
	// renderer restarts, as registered shaders do.
	qhandle_t (*CreateImage2D)( const char *name, int width, int height, const byte *rgba );

	// Replaces a rectangle of an image CreateImage2D made with width by
	// height RGBA pixels: what was drawn before the call shows the old
	// pixels, and what is drawn after it the new
	void	(*UpdateImage2D)( qhandle_t hShader, int x, int y, int width, int height, const byte *rgba );

	// What the 2D path can do
	void	(*GetCaps2D)( caps2D_t *caps );

	// The size of the image a shader's first stage draws, as it was made or
	// loaded; qfalse when it has none
	qboolean (*GetShaderImageSize)( qhandle_t hShader, int *width, int *height );

	// ui_speeds: copies the back end's 2D times out, unless times is NULL,
	// starts them over, and says whether to time the next frames'. The
	// client times the front end's 2D through wrappers (cl_uispeeds.c),
	// where a new 2D call belongs too if it only queues a draw: sampled
	// when it costs the same each time, timed each time when its cost
	// grows with its size. A call that may draw what waits in the back end
	// inside it, or makes a resource, stays out.
	void	(*TimeBackEnd2D)( times2D_t *times, qboolean on );

	// Retained 2D, for a UI that keeps what it draws. Handles are numbered
	// from 1, 0 is none, and all of them are gone when the renderer
	// restarts. Deleting one takes effect in its place among the frame's
	// draws, so what was drawn with it before still draws.

	// A UI texture: width by height pixels in format (NULL for transparent
	// black), at exactly that size, without mipmaps, light scaling or
	// compression, clamped at its edges; 0 when there's no room for it.
	// Drawn with TRIS2D_TEXTURE2D
	int		(*CreateTexture2D)( int width, int height, tex2DFormat_t format, const byte *pixels );
	// replaces a rectangle of it, with pixels in its format, as
	// UpdateImage2D does an image's
	void	(*UpdateTexture2D)( int texture, int x, int y, int width, int height, const byte *pixels );
	void	(*DeleteTexture2D)( int texture );

	// Geometry kept by the renderer: the vertexes and indexes are copied,
	// and drawn again with any state, as often as asked, for the cost of a
	// small command; 0 when it's malformed or there's no room for it
	// (caps2D_t's maxGeometryVerts)
	int		(*CreateGeometry2D)( const polyVert2D_t *verts, int numVerts, const int *indexes, int numIndexes );
	void	(*DrawGeometry2D)( int geometry, const draw2DState_t *state );
	void	(*DeleteGeometry2D)( int geometry );

	// Changes the clip mask by op (clipMaskOp_t), with the geometry drawn
	// as state places it, but not seen. Without caps2D_t's clipMask, does
	// nothing, and TRIS2D_CLIPMASK clips nothing
	void	(*SetClipMask2D)( clipMaskOp_t op, int geometry, const draw2DState_t *state );

	// A paint (paint2D_t) for TRIS2D_PAINT; 0 when it's malformed, there's
	// no room for it, or the renderer doesn't draw its kind (caps2D_t's
	// paints)
	int		(*CreatePaint2D)( const paint2D_t *paint );
	void	(*DeletePaint2D)( int paint );

	// Layers, where caps2D_t has layers: a new one like the 2D target,
	// transparent, which the 2D draws go into until it's popped. Its
	// handle is its depth; the 2D target is 0. Layers share the 2D
	// target's stencil, so a clip mask holds across them.
	// CompositeLayers2D puts source onto destination (blend2DMode_t)
	// through the filters, in order, within state's scissor, grown by
	// the filters' reach, and with TRIS2D_CLIPMASK, where the mask is; a
	// NULL state is the whole target. What's pushed in a frame is popped
	// at its end
	int		(*PushLayer2D)( void );
	void	(*CompositeLayers2D)( int source, int destination, int blendMode, const int *filters, int numFilters,
				const draw2DState_t *state );
	void	(*PopLayer2D)( void );
	// A filter, by name, with its numbers:
	//   opacity		alpha
	//   blur			sigma, in pixels
	//   drop-shadow	red, green, blue, alpha (premultiplied, 0 to 1), x and y offset, sigma
	//   color-matrix	20 numbers, a 4x5 matrix by rows, on straight colors from 0 to 1
	int		(*CreateFilter2D)( const char *name, const float *params, int numParams );
	void	(*DeleteFilter2D)( int filter );
	// A UI texture of box (x, y, width and height in the 2D target's
	// pixels) as the top layer has it when the call's turn comes among the
	// frame's draws: premultiplied, transparent where the box is off the
	// target; 0 where caps2D_t has no layers, or there's no room for it.
	// Drawn and deleted as any other (CreateTexture2D)
	int		(*SaveLayer2D)( const int box[4] );
	// A filter that keeps a layer composited through it where the texture
	// is opaque: the source times the texture's alpha (an A8 texture's
	// coverage), the texture's top left at x, y in the target's pixels,
	// and nothing outside it. Once the texture's deleted, the composites
	// queued after show nothing through the filter
	int		(*CreateMaskFilter2D)( int texture, int x, int y );
} refexport_t;

//
// these are the functions imported by the refresh module
//
typedef struct {
	// print message on the local console
	void	(QDECL *Printf)( int printLevel, const char *fmt, ...) Q_PRINTF_FUNC(2, 3);

	// abort the game
	void	(QDECL *Error)( int errorLevel, const char *fmt, ...) Q_NO_RETURN Q_PRINTF_FUNC(2, 3);

	// milliseconds should only be used for profiling, never
	// for anything game related.  Get time from the refdef
	int		(*Milliseconds)( void );

	// stack based memory allocation for per-level things that
	// won't be freed
#ifdef HUNK_DEBUG
	void	*(*Hunk_AllocDebug)( int size, ha_pref pref, char *label, char *file, int line );
#else
	void	*(*Hunk_Alloc)( int size, ha_pref pref );
#endif
	void	*(*Hunk_AllocateTempMemory)( int size );
	void	(*Hunk_FreeTempMemory)( void *block );

	// dynamic memory allocator for things that need to be freed
	void	*(*Malloc)( int bytes );
	void	(*Free)( void *buf );

	cvar_t	*(*Cvar_Get)( const char *name, const char *value, int flags );
	void	(*Cvar_Set)( const char *name, const char *value );
	void	(*Cvar_SetValue) (const char *name, float value);
	void	(*Cvar_CheckRange)( cvar_t *cv, float minVal, float maxVal, qboolean shouldBeIntegral );
	void	(*Cvar_SetDescription)( cvar_t *cv, const char *description );

	int		(*Cvar_VariableIntegerValue) (const char *var_name);

	void	(*Cmd_AddCommand)( const char *name, void(*cmd)(void) );
	void	(*Cmd_RemoveCommand)( const char *name );

	int		(*Cmd_Argc) (void);
	char	*(*Cmd_Argv) (int i);

	void	(*Cmd_ExecuteText) (int exec_when, const char *text);

	byte	*(*CM_ClusterPVS)(int cluster);

	// visualization for debugging collision detection
	void	(*CM_DrawDebugSurface)( void (*drawPoly)(int color, int numPoints, float *points) );

	// a -1 return means the file does not exist
	// NULL can be passed for buf to just determine existence
	int		(*FS_FileIsInPAK)( const char *name, int *pCheckSum );
	long		(*FS_ReadFile)( const char *name, void **buf );
	void	(*FS_FreeFile)( void *buf );
	char **	(*FS_ListFiles)( const char *name, const char *extension, int *numfilesfound );
	void	(*FS_FreeFileList)( char **filelist );
	void	(*FS_WriteFile)( const char *qpath, const void *buffer, int size );
	qboolean (*FS_FileExists)( const char *file );
	// the tier of the search path entry a read of the file takes, or -1
	// for none (FS_FileTier): a renderer tries a name's every format in
	// tier 0 before any in tier 1 (COM_NextByTier), and joins tier 0's
	// shader text before tier 1's
	int		(*FS_FileTier)( const char *name );
	// where a read of the file would come from, and where the last one
	// came from: its pak's or directory's path, or "" (whichshader)
	const char	*(*FS_FileSource)( const char *name );
	const char	*(*FS_LastFileSource)( void );

	// cinematic stuff
	void	(*CIN_UploadCinematic)(int handle);
	int		(*CIN_PlayCinematic)( const char *arg0, int xpos, int ypos, int width, int height, int bits);
	e_status (*CIN_RunCinematic) (int handle);

	void	(*CL_WriteAVIVideoFrame)( const byte *buffer, int size );

	// input event handling
	void	(*IN_Init)( void *windowData );
	void	(*IN_Shutdown)( void );
	void	(*IN_Restart)( void );

	// whether a new window is made fullscreen: the client decides
	qboolean (*CreateFullscreen)( void );

	// math
	long    (*ftol)(float f);

	// system stuff
	void	(*Sys_SetEnv)( const char *name, const char *value );
	void	(*Sys_GLimpSafeInit)( void );
	void	(*Sys_GLimpInit)( void );
	qboolean (*Sys_LowPhysicalMemory)( void );

	// a monotonic clock, for ui_speeds' times
	int64_t	(*Nanoseconds)( void );
} refimport_t;


// this is the only function actually exported at the linker level
// If the module can't init to a valid rendering state, NULL will be
// returned.
#ifdef USE_RENDERER_DLOPEN
typedef	refexport_t* (QDECL *GetRefAPI_t) (int apiVersion, refimport_t * rimp);
#else
refexport_t*GetRefAPI( int apiVersion, refimport_t *rimp );
#endif

#endif	// __TR_PUBLIC_H
