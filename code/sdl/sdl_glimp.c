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

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL3/SDL.h"
#else
#	include <SDL3/SDL.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../renderercommon/tr_common.h"
#include "../sys/sys_local.h"
#include "sdl_icon.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

typedef enum
{
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
} rserr_t;

SDL_Window *SDL_window = NULL;
static SDL_GLContext SDL_glContext = NULL;

cvar_t *r_allowSoftwareGL; // Don't abort out if a hardware visual can't be obtained
cvar_t *r_centerWindow;
cvar_t *r_sdlDriver;
cvar_t *r_preferOpenGLES;
cvar_t *r_gpuSync;
cvar_t *r_modeFullscreen;

// a new r_mode for the window, which waits while it's fullscreen
static qboolean windowSizePending;

// the last frame's fence, which GLimp_EndFrame waits on after the next swap
static GLsync frameFence;
#ifdef __EMSCRIPTEN__
// the fence of the frame before, which GLimp_FrameReady looks at
static GLsync previousFrameFence;
#endif

int qglMajorVersion, qglMinorVersion;
int qglesMajorVersion, qglesMinorVersion;

typedef void (APIENTRYP qglActiveTextureARB_t) (GLenum texture);
typedef void (APIENTRYP qglClientActiveTextureARB_t) (GLenum texture);
typedef void (APIENTRYP qglMultiTexCoord2fARB_t) (GLenum target, GLfloat s, GLfloat t);

typedef void (APIENTRYP qglLockArraysEXT_t) (GLint first, GLsizei count);
typedef void (APIENTRYP qglUnlockArraysEXT_t) (void);

qglActiveTextureARB_t qglActiveTextureARB;
qglClientActiveTextureARB_t qglClientActiveTextureARB;
qglMultiTexCoord2fARB_t qglMultiTexCoord2fARB;

qglLockArraysEXT_t qglLockArraysEXT;
qglUnlockArraysEXT_t qglUnlockArraysEXT;

#define GLE(ret, name, ...) name##proc * qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_sync_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_ARB_map_buffer_range_PROCS;
QGL_EXT_direct_state_access_PROCS;
#undef GLE

/*
===============
GLimp_Shutdown
===============
*/
void GLimp_Shutdown( void )
{
	ri.IN_Shutdown();

	if( frameFence )
	{
		qglDeleteSync( frameFence );
		frameFence = NULL;
	}
#ifdef __EMSCRIPTEN__
	if( previousFrameFence )
	{
		qglDeleteSync( previousFrameFence );
		previousFrameFence = NULL;
	}
#endif

	SDL_QuitSubSystem( SDL_INIT_VIDEO );

	// no swap waits for the display now
	ri.Cvar_Set( "r_swapIntervalActive", "0" );
}

/*
===============
GLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void GLimp_Minimize( void )
{
	SDL_MinimizeWindow( SDL_window );
}


/*
===============
GLimp_LogComment
===============
*/
void GLimp_LogComment( char *comment )
{
}

/*
===============
GLimp_CompareModes
===============
*/
static int GLimp_CompareModes( const void *a, const void *b )
{
	const float ASPECT_EPSILON = 0.001f;
	SDL_Rect *modeA = (SDL_Rect *)a;
	SDL_Rect *modeB = (SDL_Rect *)b;
	float aspectA = (float)modeA->w / (float)modeA->h;
	float aspectB = (float)modeB->w / (float)modeB->h;
	int areaA = modeA->w * modeA->h;
	int areaB = modeB->w * modeB->h;
	float aspectDiffA = fabs( aspectA - displayAspect );
	float aspectDiffB = fabs( aspectB - displayAspect );
	float aspectDiffsDiff = aspectDiffA - aspectDiffB;

	if( aspectDiffsDiff > ASPECT_EPSILON )
		return 1;
	else if( aspectDiffsDiff < -ASPECT_EPSILON )
		return -1;
	else
		return areaA - areaB;
}


/*
===============
GLimp_ModePixels

A display mode's size in pixels, as r_mode's sizes are (GLimp_SetMode)
===============
*/
static void GLimp_ModePixels( const SDL_DisplayMode *mode, int *width, int *height )
{
	*width = SDL_lroundf( mode->w * mode->pixel_density );
	*height = SDL_lroundf( mode->h * mode->pixel_density );
}

/*
===============
GLimp_DetectAvailableModes
===============
*/
static void GLimp_DetectAvailableModes(void)
{
	int i, j;
	char buf[ MAX_STRING_CHARS ] = { 0 };
	int numSDLModes;
	SDL_Rect *modes;
	int numModes = 0;

	SDL_DisplayID display = SDL_GetDisplayForWindow( SDL_window );
	if( display <= 0 )
	{
		ri.Printf( PRINT_WARNING, "Couldn't get display for window, no resolutions detected: %s\n", SDL_GetError() );
		return;
	}

	SDL_DisplayMode **displayModes = SDL_GetFullscreenDisplayModes( display, &numSDLModes );
	const SDL_DisplayMode *desktopMode = SDL_GetDesktopDisplayMode( display );

	if( !desktopMode || !displayModes || numSDLModes <= 0 )
	{
		SDL_free( displayModes );
		ri.Printf( PRINT_WARNING, "Couldn't get display modes, no resolutions detected: %s\n", SDL_GetError() );
		return;
	}
	SDL_DisplayMode windowMode = *desktopMode;

	modes = SDL_calloc( (size_t)numSDLModes, sizeof( SDL_Rect ) );
	if ( !modes )
	{
		SDL_free( displayModes );
		ri.Error( ERR_FATAL, "Out of memory" );
	}

	// Filter the available modes down to ones that we care about
	for( i = 0; i < numSDLModes; i++ )
	{
		SDL_DisplayMode *mode = displayModes[i];
		int w, h;

		if( !mode || !mode->w || !mode->h )
		{
			ri.Printf( PRINT_ALL, "Display supports any resolution\n" );
			SDL_free( modes );
			SDL_free( displayModes );
			return;
		}

		if( windowMode.format != mode->format )
			continue;

		GLimp_ModePixels( mode, &w, &h );

		// SDL can give the same resolution with different refresh rates,
		// or densities. Only list resolution once.
		for( j = 0; j < numModes; j++ )
		{
			if( w == modes[ j ].w && h == modes[ j ].h )
				break;
		}

		if( j != numModes )
			continue;

		modes[ numModes ].w = w;
		modes[ numModes ].h = h;
		numModes++;
	}

	if( numModes > 1 )
		qsort( modes, numModes, sizeof( SDL_Rect ), GLimp_CompareModes );

	for( i = 0; i < numModes; i++ )
	{
		const char *newModeString = va( "%ux%u ", modes[ i ].w, modes[ i ].h );

		if( strlen( newModeString ) < (int)sizeof( buf ) - strlen( buf ) )
			Q_strcat( buf, sizeof( buf ), newModeString );
		else
			ri.Printf( PRINT_WARNING, "Skipping mode %ux%u, buffer too small\n", modes[ i ].w, modes[ i ].h );
	}

	if( *buf )
	{
		buf[ strlen( buf ) - 1 ] = 0;
		ri.Printf( PRINT_ALL, "Available modes: '%s'\n", buf );
		ri.Cvar_Set( "r_availableModes", buf );
	}

	SDL_free( modes );
	SDL_free( displayModes );
}

/*
===============
OpenGL ES compatibility
===============
*/
static void APIENTRY GLimp_GLES_ClearDepth( GLclampd depth ) {
	qglClearDepthf( depth );
}

static void APIENTRY GLimp_GLES_DepthRange( GLclampd near_val, GLclampd far_val ) {
	qglDepthRangef( near_val, far_val );
}

static void APIENTRY GLimp_GLES_DrawBuffer( GLenum mode ) {
	// unsupported
}

static void APIENTRY GLimp_GLES_PolygonMode( GLenum face, GLenum mode ) {
	// unsupported
}

/*
===============
GLimp_GetProcAddresses

Get addresses for OpenGL functions.
===============
*/
static qboolean GLimp_GetProcAddresses( qboolean fixedFunction ) {
	qboolean success = qtrue;
	const char *version;

#ifdef __SDL_NOGETPROCADDR__
#define GLE( ret, name, ... ) qgl##name = gl#name;
#else
#define GLE( ret, name, ... ) qgl##name = (name##proc *) SDL_GL_GetProcAddress("gl" #name); \
	if ( qgl##name == NULL ) { \
		ri.Printf( PRINT_ALL, "ERROR: Missing OpenGL function %s\n", "gl" #name ); \
		success = qfalse; \
	}
#endif

	// OpenGL 1.0 and OpenGL ES 1.0
	GLE(const GLubyte *, GetString, GLenum name)

	if ( !qglGetString ) {
		Com_Error( ERR_FATAL, "glGetString is NULL" );
	}

	version = (const char *)qglGetString( GL_VERSION );

	if ( !version ) {
		Com_Error( ERR_FATAL, "GL_VERSION is NULL" );
	}

	if ( Q_stricmpn( "OpenGL ES", version, 9 ) == 0 ) {
		char profile[6]; // ES, ES-CM, or ES-CL
		sscanf( version, "OpenGL %5s %d.%d", profile, &qglesMajorVersion, &qglesMinorVersion );
		// common lite profile (no floating point) is not supported
		if ( Q_stricmp( profile, "ES-CL" ) == 0 ) {
			qglesMajorVersion = 0;
			qglesMinorVersion = 0;
		}
	} else {
		sscanf( version, "%d.%d", &qglMajorVersion, &qglMinorVersion );
	}

	if ( fixedFunction ) {
		if ( QGL_VERSION_ATLEAST( 1, 1 ) ) {
			QGL_1_1_PROCS;
			QGL_1_1_FIXED_FUNCTION_PROCS;
			QGL_DESKTOP_1_1_PROCS;
			QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
		} else if ( qglesMajorVersion == 1 && qglesMinorVersion >= 1 ) {
			// OpenGL ES 1.1 (2.0 is not backward compatible)
			QGL_1_1_PROCS;
			QGL_1_1_FIXED_FUNCTION_PROCS;
			QGL_ES_1_1_PROCS;
			QGL_ES_1_1_FIXED_FUNCTION_PROCS;
			// error so this doesn't segfault due to NULL desktop GL functions being used
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version: %s", version );
		} else {
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version (%s), OpenGL 1.1 is required", version );
		}
	} else {
		if ( QGL_VERSION_ATLEAST( 2, 0 ) ) {
			QGL_1_1_PROCS;
			QGL_DESKTOP_1_1_PROCS;
			QGL_1_3_PROCS;
			QGL_1_5_PROCS;
			QGL_2_0_PROCS;
		} else if ( QGLES_VERSION_ATLEAST( 2, 0 ) ) {
			QGL_1_1_PROCS;
			QGL_ES_1_1_PROCS;
			QGL_1_3_PROCS;
			QGL_1_5_PROCS;
			QGL_2_0_PROCS;

			qglClearDepth = GLimp_GLES_ClearDepth;
			qglDepthRange = GLimp_GLES_DepthRange;
			qglDrawBuffer = GLimp_GLES_DrawBuffer;
			qglPolygonMode = GLimp_GLES_PolygonMode;
		} else {
			Com_Error( ERR_FATAL, "Unsupported OpenGL Version (%s), OpenGL 2.0 is required", version );
		}
	}

	if ( QGL_VERSION_ATLEAST( 3, 0 ) || QGLES_VERSION_ATLEAST( 3, 0 ) ) {
		QGL_3_0_PROCS;
	}

#undef GLE

	// optional: without them, GLimp_EndFrame doesn't limit the frames the
	// driver queues
	if ( QGL_VERSION_ATLEAST( 3, 2 ) || QGLES_VERSION_ATLEAST( 3, 0 ) || SDL_GL_ExtensionSupported( "GL_ARB_sync" ) ) {
#ifdef __SDL_NOGETPROCADDR__
#define GLE( ret, name, ... ) qgl##name = gl#name;
#else
#define GLE( ret, name, ... ) qgl##name = (name##proc *) SDL_GL_GetProcAddress( "gl" #name );
#endif
		QGL_ARB_sync_PROCS;
#undef GLE

		if ( !qglFenceSync || !qglDeleteSync || !qglClientWaitSync ) {
			qglFenceSync = NULL;
		}
	}

	return success;
}

/*
===============
GLimp_ClearProcAddresses

Clear addresses for OpenGL functions.
===============
*/
static void GLimp_ClearProcAddresses( void ) {
#define GLE( ret, name, ... ) qgl##name = NULL;

	qglMajorVersion = 0;
	qglMinorVersion = 0;
	qglesMajorVersion = 0;
	qglesMinorVersion = 0;

	QGL_1_1_PROCS;
	QGL_1_1_FIXED_FUNCTION_PROCS;
	QGL_DESKTOP_1_1_PROCS;
	QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
	QGL_ES_1_1_PROCS;
	QGL_ES_1_1_FIXED_FUNCTION_PROCS;
	QGL_1_3_PROCS;
	QGL_1_5_PROCS;
	QGL_2_0_PROCS;
	QGL_3_0_PROCS;
	QGL_ARB_sync_PROCS;
	QGL_ARB_occlusion_query_PROCS;
	QGL_ARB_framebuffer_object_PROCS;
	QGL_ARB_vertex_array_object_PROCS;
	QGL_ARB_map_buffer_range_PROCS;
	QGL_EXT_direct_state_access_PROCS;

	qglActiveTextureARB = NULL;
	qglClientActiveTextureARB = NULL;
	qglMultiTexCoord2fARB = NULL;

	qglLockArraysEXT = NULL;
	qglUnlockArraysEXT = NULL;

#undef GLE
}

/*
===============
GLimp_WindowedSize

A window as big as the desktop looks fullscreen, so one that nearly covers
it is shrunk, keeping its shape, to fit three quarters of the display's
usable area
===============
*/
static void GLimp_WindowedSize( SDL_DisplayID display, int *width, int *height )
{
	SDL_Rect usable;
	float scale;

	if( !display || !SDL_GetDisplayUsableBounds( display, &usable ) || *width <= 0 || *height <= 0 )
	{
		return;
	}

	scale = SDL_min( usable.w * 0.75f / *width, usable.h * 0.75f / *height );
	*width = SDL_lroundf( *width * scale );
	*height = SDL_lroundf( *height * scale );
}

/*
===============
GLimp_FitWindow

Leaving fullscreen gives the window back its size from before, which may
nearly cover the desktop (r_mode -2): shrink and center it
===============
*/
static void GLimp_FitWindow( void )
{
	SDL_DisplayID display = SDL_GetDisplayForWindow( SDL_window );
	SDL_Rect usable;
	int width, height;

	if( !display || !SDL_GetDisplayUsableBounds( display, &usable ) ||
		!SDL_GetWindowSize( SDL_window, &width, &height ) )
	{
		return;
	}

	// a maximized window is meant to fill the desktop
	if( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_MAXIMIZED )
	{
		return;
	}

	// window borders keep one a little smaller than the usable area
	if( width < usable.w * 9 / 10 && height < usable.h * 9 / 10 )
	{
		return;
	}

	GLimp_WindowedSize( display, &width, &height );
	SDL_SetWindowSize( SDL_window, width, height );
	SDL_SetWindowPosition( SDL_window, SDL_WINDOWPOS_CENTERED_DISPLAY( display ),
		SDL_WINDOWPOS_CENTERED_DISPLAY( display ) );
}

/*
===============
GLimp_ClosestFullscreenMode

The display's fullscreen mode nearest width by height pixels. Prefers, in
order: at least that many pixels each way; the fewest pixels then (else
the most); pixel density 1; the refresh rate nearest refreshRate (the
desktop's for 0).
===============
*/
static qboolean GLimp_ClosestFullscreenMode( SDL_DisplayID display, int width, int height,
	float refreshRate, SDL_DisplayMode *closest )
{
	SDL_DisplayMode **modes;
	const SDL_DisplayMode *best = NULL, *desktopMode;
	int i, count, bestPixels = 0;
	qboolean bestFits = qfalse;

	if( refreshRate <= 0.0f && ( desktopMode = SDL_GetDesktopDisplayMode( display ) ) != NULL )
	{
		refreshRate = desktopMode->refresh_rate;
	}

	modes = SDL_GetFullscreenDisplayModes( display, &count );
	if( !modes )
	{
		return qfalse;
	}

	for( i = 0; i < count; i++ )
	{
		const SDL_DisplayMode *mode = modes[i];
		int w, h, pixels;
		qboolean fits, better;

		if( !mode->w || !mode->h )
			continue;

		GLimp_ModePixels( mode, &w, &h );
		pixels = w * h;
		fits = w >= width && h >= height;

		if( !best || fits != bestFits )
			better = !best || fits;
		else if( pixels != bestPixels )
			better = fits ? pixels < bestPixels : pixels > bestPixels;
		else if( mode->pixel_density != best->pixel_density )
			better = mode->pixel_density < best->pixel_density;
		else if( refreshRate > 0.0f )
			better = SDL_fabsf( mode->refresh_rate - refreshRate ) < SDL_fabsf( best->refresh_rate - refreshRate );
		else
			better = mode->refresh_rate > best->refresh_rate;

		if( better )
		{
			best = mode;
			bestPixels = pixels;
			bestFits = fits;
		}
	}

	if( best )
	{
		*closest = *best;
	}
	SDL_free( modes );
	return best != NULL;
}

/*
===============
GLimp_FullscreenMode

The display mode fullscreen asks for, or NULL for none: the window fills
the display in the mode it has, at its resolution and refresh rate, and
no screen goes blank for a mode change (r_modeFullscreen -2). A mode, or
-1 for r_customwidth by r_customheight, changes the display to the
closest it has; an empty r_modeFullscreen uses r_mode's.
===============
*/
static const SDL_DisplayMode *GLimp_FullscreenMode( SDL_DisplayID display, SDL_DisplayMode *closest )
{
	// what isn't a number keeps the display's mode, rather than reading as
	// mode 0
	int mode = !*r_modeFullscreen->string ? r_mode->integer :
		Q_isanumber( r_modeFullscreen->string ) ? r_modeFullscreen->integer : -2;
	int width, height;
	float aspect;

	if( mode == -2 )
	{
		return NULL;
	}

	if( !R_GetModeInfo( &width, &height, &aspect, mode ) )
	{
		ri.Printf( PRINT_ALL, "Invalid fullscreen mode %d, using the display's own\n", mode );
		return NULL;
	}

	if( !GLimp_ClosestFullscreenMode( display, width, height,
			(float)ri.Cvar_VariableIntegerValue( "r_displayRefresh" ), closest ) )
	{
		ri.Printf( PRINT_DEVELOPER, "No fullscreen modes for %dx%d, using the display's own: %s\n",
			width, height, SDL_GetError( ) );
		return NULL;
	}

	return closest;
}

/*
===============
GLimp_ApplyFullscreenMode

Gives the window GLimp_FullscreenMode's mode, for whenever it's fullscreen
===============
*/
static qboolean GLimp_ApplyFullscreenMode( SDL_DisplayID display )
{
	SDL_DisplayMode closestMode;
	const SDL_DisplayMode *mode = GLimp_FullscreenMode( display, &closestMode );

	if( !SDL_SetWindowFullscreenMode( SDL_window, mode ) )
	{
		ri.Printf( PRINT_DEVELOPER, "SDL_SetWindowFullscreenMode failed: %s\n", SDL_GetError( ) );
		return qfalse;
	}

	if( !mode )
	{
		mode = SDL_GetDesktopDisplayMode( display );
	}
	glConfig.displayFrequency = mode ? SDL_lroundf( mode->refresh_rate ) : 0;
	return qtrue;
}

/*
===============
GLimp_ModeSize

A window's size for an r_mode, in the window's coordinates, which on macOS
and Wayland are points, and in pixels. -2 is the desktop's size, of which
a native window gets three quarters
===============
*/
static qboolean GLimp_ModeSize( SDL_DisplayID display, int mode, float density,
	int *windowWidth, int *windowHeight, int *pixelWidth, int *pixelHeight )
{
	const SDL_DisplayMode *desktopMode = SDL_GetDesktopDisplayMode( display );
	float aspect;

	if( mode == -2 && desktopMode )
	{
		if( desktopMode->h > 0 )
		{
			*windowWidth = desktopMode->w;
			*windowHeight = desktopMode->h;
#ifndef __EMSCRIPTEN__
			GLimp_WindowedSize( display, windowWidth, windowHeight );
#endif
		}
		else
		{
			*windowWidth = 640;
			*windowHeight = 480;
			ri.Printf( PRINT_ALL, "Cannot determine display resolution, assuming 640x480\n" );
		}

		*pixelWidth = SDL_lroundf( *windowWidth * density );
		*pixelHeight = SDL_lroundf( *windowHeight * density );
		return qtrue;
	}

	if( !R_GetModeInfo( pixelWidth, pixelHeight, &aspect, mode ) )
	{
		return qfalse;
	}

	*windowWidth = SDL_lroundf( *pixelWidth / density );
	*windowHeight = SDL_lroundf( *pixelHeight / density );
	return qtrue;
}

/*
===============
GLimp_WindowSizeChanged

Whether r_mode, r_customwidth or r_customheight changed since last asked
===============
*/
static qboolean GLimp_WindowSizeChanged( void )
{
	qboolean changed = r_mode->modified || r_customwidth->modified || r_customheight->modified;

	r_mode->modified = r_customwidth->modified = r_customheight->modified = qfalse;
	return changed;
}

#ifndef __EMSCRIPTEN__
/*
===============
GLimp_ResizeWindow

Gives a window r_mode's size, in place
===============
*/
static void GLimp_ResizeWindow( void )
{
	float density = SDL_GetWindowPixelDensity( SDL_window );
	int width, height, pixelWidth, pixelHeight, currentWidth, currentHeight;

	if( !GLimp_ModeSize( SDL_GetDisplayForWindow( SDL_window ), r_mode->integer,
			density > 0.0f ? density : 1.0f, &width, &height, &pixelWidth, &pixelHeight ) )
	{
		ri.Printf( PRINT_ALL, "Invalid mode %d\n", r_mode->integer );
		return;
	}

	// already that size, as when IN_SaveWindowSize keeps a resize. Ask the
	// window: glConfig follows it only once its resize event arrives, so
	// just after leaving fullscreen it still has the fullscreen size.
	if( SDL_GetWindowSizeInPixels( SDL_window, &currentWidth, &currentHeight ) &&
		pixelWidth == currentWidth && pixelHeight == currentHeight )
	{
		return;
	}

	// restoring is asynchronous on X11 and Wayland, and a size asked for
	// while still maximized is lost to it
	if( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_MAXIMIZED )
	{
		SDL_RestoreWindow( SDL_window );
		SDL_SyncWindow( SDL_window );
	}
	SDL_SetWindowSize( SDL_window, width, height );
}
#endif

/*
===============
GLimp_SetMode

r_mode's sizes are the pixels to render, on every platform. The window
is created at that size in its own coordinates, which on macOS and
Wayland are points: fewer on a high density display.
===============
*/
static int GLimp_SetMode(int mode, qboolean fullscreen, qboolean noborder, qboolean fixedFunction)
{
	struct GLimp_ContextType {
		int profileMask;
		int majorVersion;
		int minorVersion;
	} contexts[4];
	int numContexts, type;
	const char *glstring;
	int perChannelColorBits;
	int colorBits, depthBits, stencilBits;
	int samples;
	int i = 0;
	SDL_Surface *icon = NULL;
	Uint64 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL;
	const SDL_DisplayMode *desktopMode = NULL;
	int display = 0;
	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;
	int windowWidth, windowHeight, pixelWidth, pixelHeight;
	float density = 1.0f;
	int swapInterval;

	ri.Printf( PRINT_ALL, "Initializing OpenGL display\n");

	// Windows resize in place, without a vid_restart. On the web, the page
	// sizes the canvas, and SDL3 follows its size changes only for a
	// resizable window.
	flags |= SDL_WINDOW_RESIZABLE;

	// full resolution where windows are sized in points (macOS, Wayland,
	// the web)
	flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

#ifdef USE_ICON
	icon = SDL_CreateSurfaceFrom(
			CLIENT_WINDOW_ICON.width,
			CLIENT_WINDOW_ICON.height,
			SDL_PIXELFORMAT_RGBA32,
			(void *)CLIENT_WINDOW_ICON.pixel_data,
			CLIENT_WINDOW_ICON.bytes_per_pixel * CLIENT_WINDOW_ICON.width);
#endif

	// Use the window's display if a window exists, else the primary one.
	// SDL3 display IDs aren't indexes: 0 means none.
	if( SDL_window != NULL )
	{
		display = SDL_GetDisplayForWindow( SDL_window );
		if( !display )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GetDisplayForWindow() failed: %s\n", SDL_GetError() );
		}
	}
	if( !display )
	{
		display = SDL_GetPrimaryDisplay( );
	}

	desktopMode = SDL_GetDesktopDisplayMode( display );
	if( desktopMode && desktopMode->pixel_density > 0.0f )
	{
		density = desktopMode->pixel_density;
	}
	if( desktopMode && desktopMode->h > 0 )
	{
		displayAspect = (float)desktopMode->w / (float)desktopMode->h;

		ri.Printf( PRINT_ALL, "Display aspect: %.3f\n", displayAspect );
	}
	else
	{
		ri.Printf( PRINT_ALL, "Cannot determine display aspect, assuming 1.333\n" );
	}

	ri.Printf (PRINT_ALL, "...setting mode %d:", mode );

	// the window's size, which it also keeps for when it leaves fullscreen;
	// GLimp_UpdateWindowSize takes the size it gets
	if( !GLimp_ModeSize( display, mode, density, &windowWidth, &windowHeight, &pixelWidth, &pixelHeight ) )
	{
		ri.Printf( PRINT_ALL, " invalid mode\n" );
		return RSERR_INVALID_MODE;
	}
	ri.Printf( PRINT_ALL, " %d %d\n", pixelWidth, pixelHeight );

	// until the window says otherwise, should it report no size
	glConfig.vidWidth = pixelWidth;
	glConfig.vidHeight = pixelHeight;
	glConfig.windowAspect = (float)pixelWidth / (float)pixelHeight;

	// Center window
	if( r_centerWindow->integer && !fullscreen && desktopMode )
	{
		x = ( desktopMode->w / 2 ) - ( windowWidth / 2 );
		y = ( desktopMode->h / 2 ) - ( windowHeight / 2 );
	}

	// Destroy existing state if it exists
	if( SDL_glContext != NULL )
	{
		GLimp_ClearProcAddresses();
		SDL_GL_DestroyContext( SDL_glContext );
		SDL_glContext = NULL;
	}

	if( SDL_window != NULL )
	{
		SDL_GetWindowPosition( SDL_window, &x, &y );
		ri.Printf( PRINT_DEVELOPER, "Existing window at %dx%d before being destroyed\n", x, y );
		SDL_DestroyWindow( SDL_window );
		SDL_window = NULL;
	}

	if( fullscreen )
	{
		flags |= SDL_WINDOW_FULLSCREEN;
		glConfig.isFullscreen = qtrue;
	}
	else
	{
		if( noborder )
			flags |= SDL_WINDOW_BORDERLESS;

		glConfig.isFullscreen = qfalse;
	}

	colorBits = r_colorbits->value;
	if ((!colorBits) || (colorBits >= 32))
		colorBits = 24;

	if (!r_depthbits->value)
		depthBits = 24;
	else
		depthBits = r_depthbits->value;

	stencilBits = r_stencilbits->value;
	samples = r_ext_multisample->value;

	numContexts = 0;

	if ( !fixedFunction ) {
		int profileMask;
		qboolean preferOpenGLES;

		SDL_GL_ResetAttributes();
		SDL_GL_GetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, &profileMask );

		preferOpenGLES = ( r_preferOpenGLES->integer == 1 ||
		                 ( r_preferOpenGLES->integer == -1 && profileMask == SDL_GL_CONTEXT_PROFILE_ES ) );

		if ( preferOpenGLES ) {
#ifdef __EMSCRIPTEN__
			// WebGL 2.0 isn't fully backward compatible so you have to ask for it specifically
			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 3;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
#endif

			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 2;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
		}

		contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_CORE;
		contexts[numContexts].majorVersion = 3;
		contexts[numContexts].minorVersion = 2;
		numContexts++;

		contexts[numContexts].profileMask = 0;
		contexts[numContexts].majorVersion = 2;
		contexts[numContexts].minorVersion = 0;
		numContexts++;

		if ( !preferOpenGLES ) {
#ifdef __EMSCRIPTEN__
			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 3;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
#endif

			contexts[numContexts].profileMask = SDL_GL_CONTEXT_PROFILE_ES;
			contexts[numContexts].majorVersion = 2;
			contexts[numContexts].minorVersion = 0;
			numContexts++;
		}
	} else {
		contexts[numContexts].profileMask = 0;
		contexts[numContexts].majorVersion = 1;
		contexts[numContexts].minorVersion = 1;
		numContexts++;
	}

	for (i = 0; i < 16; i++)
	{
		int testColorBits, testDepthBits, testStencilBits;
		int realColorBits[3];

		// 0 - default
		// 1 - minus colorBits
		// 2 - minus depthBits
		// 3 - minus stencil
		if ((i % 4) == 0 && i)
		{
			// one pass, reduce
			switch (i / 4)
			{
				case 2 :
					if (colorBits == 24)
						colorBits = 16;
					break;
				case 1 :
					if (depthBits == 24)
						depthBits = 16;
					else if (depthBits == 16)
						depthBits = 8;
				case 3 :
					if (stencilBits == 24)
						stencilBits = 16;
					else if (stencilBits == 16)
						stencilBits = 8;
			}
		}

		testColorBits = colorBits;
		testDepthBits = depthBits;
		testStencilBits = stencilBits;

		if ((i % 4) == 3)
		{ // reduce colorBits
			if (testColorBits == 24)
				testColorBits = 16;
		}

		if ((i % 4) == 2)
		{ // reduce depthBits
			if (testDepthBits == 24)
				testDepthBits = 16;
			else if (testDepthBits == 16)
				testDepthBits = 8;
		}

		if ((i % 4) == 1)
		{ // reduce stencilBits
			if (testStencilBits == 24)
				testStencilBits = 16;
			else if (testStencilBits == 16)
				testStencilBits = 8;
			else
				testStencilBits = 0;
		}

		if (testColorBits == 24)
			perChannelColorBits = 8;
		else
			perChannelColorBits = 4;

#ifdef __sgi /* Fix for SGIs grabbing too many bits of color */
		if (perChannelColorBits == 4)
			perChannelColorBits = 0; /* Use minimum size for 16-bit color */

		/* Need alpha or else SGIs choose 36+ bit RGB mode */
		SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE, 1);
#endif

		SDL_GL_SetAttribute( SDL_GL_RED_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, perChannelColorBits );
		SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, testDepthBits );
		SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, testStencilBits );

		SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, samples ? 1 : 0 );
		SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, samples );

		if(r_stereoEnabled->integer)
		{
			glConfig.stereoEnabled = qtrue;
			SDL_GL_SetAttribute(SDL_GL_STEREO, 1);
		}
		else
		{
			glConfig.stereoEnabled = qfalse;
			SDL_GL_SetAttribute(SDL_GL_STEREO, 0);
		}
		
		SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

		for ( type = 0; type < numContexts; type++ ) {
			char contextName[32];

			switch ( contexts[type].profileMask ) {
				default:
				case 0:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL %d.%d",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
				case SDL_GL_CONTEXT_PROFILE_CORE:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL %d.%d Core",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
				case SDL_GL_CONTEXT_PROFILE_ES:
					Com_sprintf( contextName, sizeof( contextName ), "OpenGL ES %d.%d",
					             contexts[type].majorVersion, contexts[type].minorVersion );
					break;
			}

			SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, contexts[type].profileMask );
			SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, contexts[type].majorVersion );
			SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, contexts[type].minorVersion );

			if( ( SDL_window = SDL_CreateWindow( CLIENT_WINDOW_TITLE,
					windowWidth, windowHeight, flags ) ) == NULL )
			{
				ri.Printf( PRINT_DEVELOPER, "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
				break;
			}

			SDL_glContext = SDL_GL_CreateContext( SDL_window );
			if ( !SDL_glContext )
			{
				SDL_DestroyWindow( SDL_window );
				SDL_window = NULL;
				ri.Printf( PRINT_ALL, "SDL_GL_CreateContext() for %s context failed: %s\n", contextName, SDL_GetError() );
				continue;
			}

			if ( !GLimp_GetProcAddresses( fixedFunction ) )
			{
				ri.Printf( PRINT_ALL, "GLimp_GetProcAddresses() for %s context failed\n", contextName );
				GLimp_ClearProcAddresses();
				SDL_GL_DestroyContext( SDL_glContext );
				SDL_glContext = NULL;
				SDL_DestroyWindow( SDL_window );
				SDL_window = NULL;
				continue;
			}

			if ( contexts[type].profileMask == SDL_GL_CONTEXT_PROFILE_CORE ) {
				const char *renderer;

				renderer = (const char *)qglGetString( GL_RENDERER );

				if ( !renderer || strstr( renderer, "Software Renderer" ) || strstr( renderer, "Software Rasterizer" ) )
				{
					ri.Printf( PRINT_ALL, "GL_RENDERER is %s, rejecting %s context\n", renderer, contextName );

					GLimp_ClearProcAddresses();
					SDL_GL_DestroyContext( SDL_glContext );
					SDL_glContext = NULL;
					SDL_DestroyWindow( SDL_window );
					SDL_window = NULL;
					continue;
				}
			}

			break;
		}

		if ( !SDL_window ) {
			continue;
		}

		if ( !SDL_glContext ) {
			SDL_DestroyWindow( SDL_window );
			SDL_window = NULL;
			continue;
		}

		// Set window position if specified
		if( x != SDL_WINDOWPOS_UNDEFINED && y != SDL_WINDOWPOS_UNDEFINED )
		{
			SDL_SetWindowPosition( SDL_window, x, y );
		}

		if( fullscreen )
		{
			if( !GLimp_ApplyFullscreenMode( display ) )
			{
				continue;
			}
		}

		SDL_SetWindowIcon( SDL_window, icon );

		// smaller, and the console and menus have no room to draw in
		SDL_SetWindowMinimumSize( SDL_window, 320, 240 );

		qglClearColor( 0, 0, 0, 1 );
		qglClear( GL_COLOR_BUFFER_BIT );
		SDL_GL_SwapWindow( SDL_window );

#ifdef __EMSCRIPTEN__
		// A browser shows frames on the display's refreshes: 0 would run
		// them on timers instead, some drawn and never shown.
		swapInterval = MAX( 1, r_swapInterval->integer );
#else
		swapInterval = r_swapInterval->integer;
#endif
		if( !SDL_GL_SetSwapInterval( swapInterval ) )
		{
			ri.Printf( PRINT_DEVELOPER, "SDL_GL_SetSwapInterval failed: %s\n", SDL_GetError( ) );
		}

		// the interval that took, which isn't the one asked for when the
		// driver refused it (a driver's settings can also override it
		// unseen, which the frame cap allows for)
		if( !SDL_GL_GetSwapInterval( &swapInterval ) )
		{
			swapInterval = 0;
		}
		ri.Cvar_Set( "r_swapIntervalActive", va( "%d", swapInterval ) );

		SDL_GL_GetAttribute( SDL_GL_RED_SIZE, &realColorBits[0] );
		SDL_GL_GetAttribute( SDL_GL_GREEN_SIZE, &realColorBits[1] );
		SDL_GL_GetAttribute( SDL_GL_BLUE_SIZE, &realColorBits[2] );
		SDL_GL_GetAttribute( SDL_GL_DEPTH_SIZE, &glConfig.depthBits );
		SDL_GL_GetAttribute( SDL_GL_STENCIL_SIZE, &glConfig.stencilBits );

		glConfig.colorBits = realColorBits[0] + realColorBits[1] + realColorBits[2];

		ri.Printf( PRINT_ALL, "Using %d color bits, %d depth, %d stencil display.\n",
				glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits );
		break;
	}

	SDL_DestroySurface( icon );

	if( !SDL_window )
	{
		ri.Printf( PRINT_ALL, "Couldn't get a visual\n" );
		return RSERR_INVALID_MODE;
	}

	SDL_ShowWindow( SDL_window );

	// fullscreen takes effect once the window is shown
	SDL_SyncWindow( SDL_window );
	GLimp_UpdateWindowSize( );
	ri.Printf( PRINT_ALL, "Window: %dx%d pixels\n", glConfig.vidWidth, glConfig.vidHeight );

	GLimp_DetectAvailableModes();

	glstring = (char *) qglGetString (GL_RENDERER);
	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", glstring );

	return RSERR_OK;
}

/*
===============
GLimp_StartDriverAndSetMode
===============
*/
static qboolean GLimp_StartDriverAndSetMode(int mode, qboolean fullscreen, qboolean noborder, qboolean gl3Core)
{
	rserr_t err;

	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		const char *driverName;

		if (!SDL_Init(SDL_INIT_VIDEO))
		{
			ri.Printf( PRINT_ALL, "SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n", SDL_GetError());
			return qfalse;
		}

		driverName = SDL_GetCurrentVideoDriver( );
		ri.Printf( PRINT_ALL, "SDL using driver \"%s\"\n", driverName );
		ri.Cvar_Set( "r_sdlDriver", driverName );
	}

	if (fullscreen && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
	{
		ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
		ri.Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = qfalse;
	}
	
	err = GLimp_SetMode(mode, fullscreen, noborder, gl3Core);

	switch ( err )
	{
		case RSERR_INVALID_FULLSCREEN:
			ri.Printf( PRINT_ALL, "...WARNING: fullscreen unavailable in this mode\n" );
			return qfalse;
		case RSERR_INVALID_MODE:
			ri.Printf( PRINT_ALL, "...WARNING: could not set the given mode (%d)\n", mode );
			return qfalse;
		default:
			break;
	}

	return qtrue;
}


/*
===============
GLimp_InitExtensions
===============
*/
static void GLimp_InitExtensions( qboolean fixedFunction )
{
	if ( !r_allowExtensions->integer )
	{
		ri.Printf( PRINT_ALL, "* IGNORING OPENGL EXTENSIONS *\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "Initializing OpenGL extensions\n" );

	glConfig.textureCompression = TC_NONE;

	// GL_EXT_texture_compression_s3tc
	if ( ( QGLES_VERSION_ATLEAST( 2, 0 ) || SDL_GL_ExtensionSupported( "GL_ARB_texture_compression" ) ) &&
	     SDL_GL_ExtensionSupported( "GL_EXT_texture_compression_s3tc" ) )
	{
		if ( r_ext_compressed_textures->value )
		{
			glConfig.textureCompression = TC_S3TC_ARB;
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_compression_s3tc\n" );
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_compression_s3tc\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_compression_s3tc not found\n" );
	}

	// GL_S3_s3tc ... legacy extension before GL_EXT_texture_compression_s3tc.
	if (glConfig.textureCompression == TC_NONE)
	{
		if ( SDL_GL_ExtensionSupported( "GL_S3_s3tc" ) )
		{
			if ( r_ext_compressed_textures->value )
			{
				glConfig.textureCompression = TC_S3TC;
				ri.Printf( PRINT_ALL, "...using GL_S3_s3tc\n" );
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_S3_s3tc\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_S3_s3tc not found\n" );
		}
	}

	// OpenGL 1 fixed function pipeline
	if ( fixedFunction )
	{
		// GL_EXT_texture_env_add
		glConfig.textureEnvAddAvailable = qfalse;
		if ( SDL_GL_ExtensionSupported( "GL_EXT_texture_env_add" ) )
		{
			if ( r_ext_texture_env_add->integer )
			{
				glConfig.textureEnvAddAvailable = qtrue;
				ri.Printf( PRINT_ALL, "...using GL_EXT_texture_env_add\n" );
			}
			else
			{
				glConfig.textureEnvAddAvailable = qfalse;
				ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_env_add\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_EXT_texture_env_add not found\n" );
		}

		// GL_ARB_multitexture
		qglMultiTexCoord2fARB = NULL;
		qglActiveTextureARB = NULL;
		qglClientActiveTextureARB = NULL;
		if ( SDL_GL_ExtensionSupported( "GL_ARB_multitexture" ) )
		{
			if ( r_ext_multitexture->value )
			{
				qglMultiTexCoord2fARB = (qglMultiTexCoord2fARB_t)SDL_GL_GetProcAddress( "glMultiTexCoord2fARB" );
				qglActiveTextureARB = (qglActiveTextureARB_t)SDL_GL_GetProcAddress( "glActiveTextureARB" );
				qglClientActiveTextureARB = (qglActiveTextureARB_t)SDL_GL_GetProcAddress( "glClientActiveTextureARB" );

				if ( qglActiveTextureARB )
				{
					GLint glint = 0;
					qglGetIntegerv( GL_MAX_TEXTURE_UNITS_ARB, &glint );
					glConfig.numTextureUnits = (int) glint;
					if ( glConfig.numTextureUnits > 1 )
					{
						ri.Printf( PRINT_ALL, "...using GL_ARB_multitexture\n" );
					}
					else
					{
						qglMultiTexCoord2fARB = NULL;
						qglActiveTextureARB = NULL;
						qglClientActiveTextureARB = NULL;
						ri.Printf( PRINT_ALL, "...not using GL_ARB_multitexture, < 2 texture units\n" );
					}
				}
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_ARB_multitexture\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_ARB_multitexture not found\n" );
		}

		// GL_EXT_compiled_vertex_array
		if ( SDL_GL_ExtensionSupported( "GL_EXT_compiled_vertex_array" ) )
		{
			if ( r_ext_compiled_vertex_array->value )
			{
				ri.Printf( PRINT_ALL, "...using GL_EXT_compiled_vertex_array\n" );
				qglLockArraysEXT = (qglLockArraysEXT_t)SDL_GL_GetProcAddress( "glLockArraysEXT" );
				qglUnlockArraysEXT = (qglUnlockArraysEXT_t)SDL_GL_GetProcAddress( "glUnlockArraysEXT" );
				if (!qglLockArraysEXT || !qglUnlockArraysEXT)
				{
					ri.Error (ERR_FATAL, "bad getprocaddress");
				}
			}
			else
			{
				ri.Printf( PRINT_ALL, "...ignoring GL_EXT_compiled_vertex_array\n" );
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...GL_EXT_compiled_vertex_array not found\n" );
		}
	}

	textureFilterAnisotropic = qfalse;
	if ( SDL_GL_ExtensionSupported( "GL_EXT_texture_filter_anisotropic" ) )
	{
		if ( r_ext_texture_filter_anisotropic->integer ) {
			qglGetIntegerv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, (GLint *)&maxAnisotropy );
			if ( maxAnisotropy <= 0 ) {
				ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not properly supported!\n" );
				maxAnisotropy = 0;
			}
			else
			{
				ri.Printf( PRINT_ALL, "...using GL_EXT_texture_filter_anisotropic (max: %i)\n", maxAnisotropy );
				textureFilterAnisotropic = qtrue;
			}
		}
		else
		{
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_filter_anisotropic\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_filter_anisotropic not found\n" );
	}

	haveClampToEdge = qfalse;
	if ( QGL_VERSION_ATLEAST( 1, 2 ) || QGLES_VERSION_ATLEAST( 1, 0 ) || SDL_GL_ExtensionSupported( "GL_SGIS_texture_edge_clamp" ) )
	{
		ri.Printf( PRINT_ALL, "...using GL_SGIS_texture_edge_clamp\n" );
		haveClampToEdge = qtrue;
	}
	else
	{
		ri.Printf( PRINT_ALL, "...GL_SGIS_texture_edge_clamp not found\n" );
	}
}

#define R_MODE_FALLBACK 3 // 640 * 480

/*
===============
GLimp_Init

This routine is responsible for initializing the OS specific portions
of OpenGL
===============
*/
void GLimp_Init( qboolean fixedFunction )
{
	ri.Printf( PRINT_DEVELOPER, "Glimp_Init( )\n" );

	r_allowSoftwareGL = ri.Cvar_Get( "r_allowSoftwareGL", "0", CVAR_LATCH );
	r_sdlDriver = ri.Cvar_Get( "r_sdlDriver", "", CVAR_ROM );
	r_centerWindow = ri.Cvar_Get( "r_centerWindow", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_preferOpenGLES = ri.Cvar_Get( "r_preferOpenGLES", "-1", CVAR_ARCHIVE | CVAR_LATCH );
	r_gpuSync = ri.Cvar_Get( "r_gpuSync", "1", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_gpuSync, "Wait after each swap for the GPU to finish the frame before, so that no more than one frame is queued and frames "
		"show as soon after they're drawn as they can. Needs OpenGL 3.2, or GL_ARB_sync." );
	ri.Cvar_SetDescription( ri.Cvar_Get( "r_swapIntervalActive", "0", CVAR_ROM ),
		"The swap interval the driver gave for r_swapInterval: with one, a com_maxfps at or above the display's refresh rate leaves the pacing to the display." );
	r_modeFullscreen = ri.Cvar_Get( "r_modeFullscreen", "-2", CVAR_ARCHIVE );
	ri.Cvar_SetDescription( r_modeFullscreen, "Display mode for fullscreen: -2 keeps the display's own resolution and refresh rate; "
		"-1 (r_customwidth by r_customheight) or a mode number changes the display to the closest mode it has, at r_displayRefresh; "
		"empty uses r_mode" );

	if( ri.Cvar_VariableIntegerValue( "com_abnormalExit" ) )
	{
		ri.Cvar_Set( "r_mode", va( "%d", R_MODE_FALLBACK ) );
		ri.Cvar_Set( "r_fullscreen", "0" );
		ri.Cvar_Set( "r_centerWindow", "0" );
		ri.Cvar_Set( "com_abnormalExit", "0" );
	}

#ifdef __EMSCRIPTEN__
	// The page owns browser fullscreen (client.html), and r_fullscreen
	// follows it, starting off; the page enters it on a click. A window
	// made fullscreen by SDL would keep getting the display's size instead
	// of the page's.
	ri.Cvar_Set( "r_fullscreen", "0" );
	r_fullscreen->modified = qfalse;
#endif

	ri.Sys_GLimpInit( );

	// Create the window and set up the context
	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, r_noborder->integer, fixedFunction))
		goto success;

	// Try again, this time in a platform specific "safe mode"
	ri.Sys_GLimpSafeInit( );

	if(GLimp_StartDriverAndSetMode(r_mode->integer, r_fullscreen->integer, qfalse, fixedFunction))
		goto success;

	// Finally, try the default screen resolution
	if( r_mode->integer != R_MODE_FALLBACK )
	{
		ri.Printf( PRINT_ALL, "Setting r_mode %d failed, falling back on r_mode %d\n",
				r_mode->integer, R_MODE_FALLBACK );

		if(GLimp_StartDriverAndSetMode(R_MODE_FALLBACK, qfalse, qfalse, fixedFunction))
			goto success;
	}

	// Nothing worked, give up
	ri.Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem" );

success:
	// the new window has r_mode's size, and fullscreen r_modeFullscreen's
	GLimp_WindowSizeChanged( );
	r_modeFullscreen->modified = qfalse;
	windowSizePending = qfalse;

	// These values force the UI to disable driver selection
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;

	//FIXME SDL3 doesn't do gamma
	glConfig.deviceSupportsGamma = qfalse;

	// get our config strings
	Q_strncpyz( glConfig.vendor_string, (char *) qglGetString (GL_VENDOR), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (char *) qglGetString (GL_RENDERER), sizeof( glConfig.renderer_string ) );
	if (*glConfig.renderer_string && glConfig.renderer_string[strlen(glConfig.renderer_string) - 1] == '\n')
		glConfig.renderer_string[strlen(glConfig.renderer_string) - 1] = 0;
	Q_strncpyz( glConfig.version_string, (char *) qglGetString (GL_VERSION), sizeof( glConfig.version_string ) );

	// manually create extension list if using OpenGL 3
	if ( qglGetStringi )
	{
		int i, numExtensions, extensionLength, listLength;
		const char *extension;

		qglGetIntegerv( GL_NUM_EXTENSIONS, &numExtensions );
		listLength = 0;

		for ( i = 0; i < numExtensions; i++ )
		{
			extension = (char *) qglGetStringi( GL_EXTENSIONS, i );
			extensionLength = strlen( extension );

			if ( ( listLength + extensionLength + 1 ) >= sizeof( glConfig.extensions_string ) )
				break;

			if ( i > 0 ) {
				Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), " " );
				listLength++;
			}

			Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), extension );
			listLength += extensionLength;
		}
	}
	else
	{
		Q_strncpyz( glConfig.extensions_string, (char *) qglGetString (GL_EXTENSIONS), sizeof( glConfig.extensions_string ) );
	}

	// initialize extensions
	GLimp_InitExtensions( fixedFunction );

	ri.Cvar_Get( "r_availableModes", "", CVAR_ROM );

	// This depends on SDL_INIT_VIDEO, hence having it here
	ri.IN_Init( SDL_window );
}


/*
===============
GLimp_UpdateWindowSize

Takes the renderer's size from the window's size in pixels. SDL3 windows
are DPI aware, so on a scaled display the window has more pixels than the
screen coordinates it was created with.
===============
*/
qboolean GLimp_UpdateWindowSize( void )
{
	int width, height;

	if( !SDL_window || !SDL_GetWindowSizeInPixels( SDL_window, &width, &height ) ||
		width <= 0 || height <= 0 )
	{
		return qfalse;
	}

	glConfig.vidWidth = width;
	glConfig.vidHeight = height;
	glConfig.windowAspect = (float)width / (float)height;
	return qtrue;
}

/*
===============
GLimp_FrameReady

Whether the GPU has finished the frame before last, where GLimp_EndFrame
doesn't wait for it (WebGL)
===============
*/
qboolean GLimp_FrameReady( void )
{
#ifdef __EMSCRIPTEN__
	if( previousFrameFence )
	{
		if( qglClientWaitSync( previousFrameFence, 0, 0 ) == GL_TIMEOUT_EXPIRED )
		{
			return qfalse;
		}

		qglDeleteSync( previousFrameFence );
		previousFrameFence = NULL;
	}
#endif

	return qtrue;
}

/*
===============
GLimp_EndFrame

Responsible for doing a swapbuffers
===============
*/
void GLimp_EndFrame( void )
{
	// don't flip if drawing to front buffer
	if ( Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) != 0 )
	{
		SDL_GL_SwapWindow( SDL_window );
	}

	// At most one frame in flight, so that the driver can't queue frames
	// and show each later than it was drawn: a fence goes in after each
	// swap, and the next swap waits for it, so the CPU still works on the
	// next frame while the GPU draws this one. WebGL can't wait on a
	// fence, and a fence's state only changes between the browser's
	// tasks: GLimp_FrameReady looks at refreshes instead, and the client
	// skips one while the frame before last isn't done. The last may still
	// be drawing, as natively: waiting for it would skip a whole refresh
	// whenever the GPU takes longer than one, and halve the frame rate.
	// Without fences, there, reading a pixel waits for the frame.
#ifndef __EMSCRIPTEN__
	if( frameFence )
	{
		if( r_gpuSync->integer )
		{
			qglClientWaitSync( frameFence, GL_SYNC_FLUSH_COMMANDS_BIT, 100 * 1000000 );
		}
		qglDeleteSync( frameFence );
		frameFence = NULL;
	}
#else
	// one not looked at belongs to a frame drawn without asking
	if( previousFrameFence )
	{
		qglDeleteSync( previousFrameFence );
	}
	previousFrameFence = frameFence;
	frameFence = NULL;

	if( previousFrameFence && !r_gpuSync->integer )
	{
		qglDeleteSync( previousFrameFence );
		previousFrameFence = NULL;
	}
#endif

	if( r_gpuSync->integer && qglFenceSync )
	{
		frameFence = qglFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
	}
#ifdef __EMSCRIPTEN__
	else if( r_gpuSync->integer && !qglFenceSync )
	{
		byte pixel[4];

		qglReadPixels( 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel );
	}
#endif

#ifdef __EMSCRIPTEN__
	// the page owns browser fullscreen; changing r_fullscreen, e.g. with
	// Alt+Enter, toggles it, which the key press lets the page do
	if( r_fullscreen->modified )
	{
		MAIN_THREAD_EM_ASM({ Module.setFullscreen?.($0); }, r_fullscreen->integer);
		r_fullscreen->modified = qfalse;
	}
#else
	if( r_fullscreen->modified )
	{
		int         fullscreen;
		qboolean    needToToggle;
		qboolean    sdlToggled = qfalse;

		// Find out the current state
		fullscreen = !!( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_FULLSCREEN );

		if( r_fullscreen->integer && ri.Cvar_VariableIntegerValue( "in_nograb" ) )
		{
			ri.Printf( PRINT_ALL, "Fullscreen not allowed with in_nograb 1\n");
			ri.Cvar_Set( "r_fullscreen", "0" );
			r_fullscreen->modified = qfalse;
		}

		// Is the state we want different from the current state?
		needToToggle = !!r_fullscreen->integer != fullscreen;

		if( needToToggle )
		{
			// the same display mode as a window created fullscreen
			if( r_fullscreen->integer )
			{
				GLimp_ApplyFullscreenMode( SDL_GetDisplayForWindow( SDL_window ) );
			}

			sdlToggled = SDL_SetWindowFullscreen( SDL_window, r_fullscreen->integer );

			// SDL_WM_ToggleFullScreen didn't work, so do it the slow way;
			// otherwise the new size arrives as a window resize
			if( !sdlToggled )
				ri.Cmd_ExecuteText(EXEC_APPEND, "vid_restart\n");
			else
			{
				glConfig.isFullscreen = !!r_fullscreen->integer;

				// the window gets back its size from before fullscreen,
				// which may cover the desktop
				if( !r_fullscreen->integer )
				{
					SDL_SyncWindow( SDL_window );
					GLimp_FitWindow( );
				}
			}

			ri.IN_Restart( );
		}

		r_fullscreen->modified = qfalse;
	}

	// r_mode sizes the window, at once, or when it leaves fullscreen; a
	// fullscreen window takes a new display mode at once
	{
		qboolean sizeChanged = GLimp_WindowSizeChanged( );
		qboolean fullscreenModeChanged = r_modeFullscreen->modified;

		r_modeFullscreen->modified = qfalse;
		if( sizeChanged )
		{
			windowSizePending = qtrue;

			// an empty r_modeFullscreen follows r_mode, and -1 r_customwidth
			// by r_customheight
			if( !*r_modeFullscreen->string || r_modeFullscreen->integer == -1 )
			{
				fullscreenModeChanged = qtrue;
			}
		}

		if( SDL_GetWindowFlags( SDL_window ) & SDL_WINDOW_FULLSCREEN )
		{
			if( fullscreenModeChanged )
			{
				GLimp_ApplyFullscreenMode( SDL_GetDisplayForWindow( SDL_window ) );
			}
			else if( sizeChanged )
			{
				ri.Printf( PRINT_ALL, "r_mode sizes the window; fullscreen keeps its display mode "
					"(r_modeFullscreen changes that)\n" );
			}
		}
		else if( windowSizePending )
		{
			windowSizePending = qfalse;
			GLimp_ResizeWindow( );
		}
	}
#endif
}
