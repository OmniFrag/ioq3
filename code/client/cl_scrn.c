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
// cl_scrn.c -- master for refresh, status bar, console, chat, notify, etc

#include "client.h"

qboolean	scr_initialized;		// ready to draw

cvar_t		*cl_timegraph;
cvar_t		*cl_debuggraph;
cvar_t		*cl_graphheight;
cvar_t		*cl_graphscale;
cvar_t		*cl_graphshift;

/*
================
SCR_DrawNamedPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawNamedPic( float x, float y, float width, float height, const char *picname ) {
	qhandle_t	hShader;

	assert( width != 0 );

	hShader = re.RegisterShader( picname );
	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 1, 1, hShader );
}


// where SCR_AdjustFrom640 puts what's drawn next; when not stretching, the
// window's safe area it places against and the scale 640x480 fits it at
static screenPlacement_t	scr_placeX = PLACE_STRETCH, scr_placeY = PLACE_STRETCH;
static int					scr_area[4];
static float				scr_fitScale;

/*
================
SCR_SetPlacement

Sets how SCR_AdjustFrom640 maps 640x480 to the window on each axis:
stretched over it, the default, as the console and cinematics want; or at
one scale on both axes, so text and pictures keep their shape, against an
edge of the window's safe area or centered in it
================
*/
void SCR_SetPlacement( screenPlacement_t horizontal, screenPlacement_t vertical ) {
	scr_placeX = horizontal;
	scr_placeY = vertical;
	if ( horizontal != PLACE_STRETCH || vertical != PLACE_STRETCH ) {
		CL_GetSafeArea( scr_area );
		scr_fitScale = MIN( scr_area[2] / (float)SCREEN_WIDTH, scr_area[3] / (float)SCREEN_HEIGHT );
	}
}

/*
================
SCR_PlaceAxis

One axis of SCR_AdjustFrom640: the scale and offset from 640x480 units to a
window side of windowSize pixels, whose safe part starts at start and is
size long
================
*/
static void SCR_PlaceAxis( screenPlacement_t place, int windowSize, int start, int size, int virtualSize,
	float *scale, float *bias ) {
	if ( place == PLACE_STRETCH ) {
		*scale = windowSize / (float)virtualSize;
		*bias = 0.0f;
		return;
	}

	*scale = scr_fitScale;
	*bias = start;
	if ( place == PLACE_CENTER ) {
		*bias += 0.5f * ( size - virtualSize * scr_fitScale );
	} else if ( place == PLACE_RIGHT ) {
		*bias += size - virtualSize * scr_fitScale;
	}
}

/*
================
SCR_AdjustFrom640

From the 640x480 virtual screen to the window's pixels, as SCR_SetPlacement
says
================
*/
void SCR_AdjustFrom640( float *x, float *y, float *w, float *h ) {
	float	xscale, yscale, xbias, ybias;

	SCR_PlaceAxis( scr_placeX, cls.glconfig.vidWidth, scr_area[0], scr_area[2], SCREEN_WIDTH, &xscale, &xbias );
	SCR_PlaceAxis( scr_placeY, cls.glconfig.vidHeight, scr_area[1], scr_area[3], SCREEN_HEIGHT, &yscale, &ybias );

	if ( x ) {
		*x = *x * xscale + xbias;
	}
	if ( y ) {
		*y = *y * yscale + ybias;
	}
	if ( w ) {
		*w *= xscale;
	}
	if ( h ) {
		*h *= yscale;
	}
}

/*
================
SCR_FillRect

Coordinates are 640*480 virtual values
=================
*/
void SCR_FillRect( float x, float y, float width, float height, const float *color ) {
	re.SetColor( color );

	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 0, 0, cls.whiteShader );

	re.SetColor( NULL );
}


/*
================
SCR_DrawPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawPic( float x, float y, float width, float height, qhandle_t hShader ) {
	SCR_AdjustFrom640( &x, &y, &width, &height );
	re.DrawStretchPic( x, y, width, height, 0, 0, 1, 1, hShader );
}


/*
=================
SCR_ResetModule2D

A cgame's or ui's 2D state as it starts: no clip region or images, white
=================
*/
void SCR_ResetModule2D( module2D_t *m ) {
	Com_Memset( m, 0, sizeof( *m ) );
	Com_Memset( m->color, 255, sizeof( m->color ) );
}


/*
=================
SCR_ModuleSetColor

A cgame's or ui's color for its pictures, NULL for white, as the renderer
keeps it
=================
*/
void SCR_ModuleSetColor( module2D_t *m, const float *rgba ) {
	int		i;

	for ( i = 0; i < 4; i++ ) {
		// a module's floats may be anything: clamped, NaN as 0, before
		// they become bytes
		float	c = rgba ? rgba[i] * 255 : 255;

		m->color[i] = c >= 255 ? 255 : c > 0 ? c : 0;
	}
	re.SetColor( rgba );
}


/*
=================
SCR_ModuleStretchPic

A cgame's or ui's picture; inside its clip region, a quad of triangles in
the region's scissor, in its color, so that it merges with the module's
triangles there. Shader 0 draws white there, as triangles take it
=================
*/
void SCR_ModuleStretchPic( const module2D_t *m, float x, float y, float w, float h,
		float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	polyVert2D_t	v[4];
	tris2D_t		tris;
	int				i;

	if ( !m->clipped ) {
		re.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
		return;
	}

	// the corners as RB_StretchPic gives them
	v[0].xy[0] = v[3].xy[0] = x;
	v[1].xy[0] = v[2].xy[0] = x + w;
	v[0].xy[1] = v[1].xy[1] = y;
	v[2].xy[1] = v[3].xy[1] = y + h;
	v[0].st[0] = v[3].st[0] = s1;
	v[1].st[0] = v[2].st[0] = s2;
	v[0].st[1] = v[1].st[1] = t1;
	v[2].st[1] = v[3].st[1] = t2;
	for ( i = 0; i < 4; i++ ) {
		Byte4Copy( m->color, v[i].rgba );
	}

	Com_Memset( &tris, 0, sizeof( tris ) );
	tris.state.texture = hShader;
	tris.state.flags = TRIS2D_QUADS | TRIS2D_SCISSOR;
	Com_Memcpy( tris.state.scissor, m->clip, sizeof( m->clip ) );
	tris.verts = v;
	tris.numVerts = 4;
	re.DrawTriangles2D( &tris );
}


// whether a cgame or ui made image
static qboolean SCR_ModuleMadeImage( const module2D_t *m, qhandle_t image ) {
	int		i;

	for ( i = 0; i < m->numImages; i++ ) {
		if ( m->images[i] == image ) {
			return qtrue;
		}
	}
	return qfalse;
}


/*
=================
SCR_ModuleCreateImage2D

An image a cgame or ui makes, named for the module so that no file's name,
or the other module's, can be taken
=================
*/
static qhandle_t SCR_ModuleCreateImage2D( extModule_t ext, const intptr_t *args ) {
	module2D_t	*m = CL_Module2D( ext );
	const char	*module = ext == EXT_MODULE_CGAME ? "cgame" : "ui";
	const char	*name = VMA_STR( 1 );
	int			width = args[2], height = args[3];
	char		fullName[MAX_QPATH];
	qhandle_t	image;

	if ( m->numImages == MAX_MODULE_IMAGES ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: %s: trap_R_CreateImage2D: %s: it has made %i images, all it may\n",
			module, name, MAX_MODULE_IMAGES );
		return 0;
	}
	if ( width < 1 || height < 1 || width > MAX_MODULE_IMAGE_SIZE || height > MAX_MODULE_IMAGE_SIZE ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: %s: trap_R_CreateImage2D: %s: %i by %i isn't 1 to %i a side\n",
			module, name, width, height, MAX_MODULE_IMAGE_SIZE );
		return 0;
	}
	if ( strlen( module ) + strlen( name ) + 2 >= sizeof( fullName ) ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: %s: trap_R_CreateImage2D: %s: the name is too long\n", module, name );
		return 0;
	}
	Com_sprintf( fullName, sizeof( fullName ), "*%s/%s", module, name );

	image = re.CreateImage2D( fullName, width, height, VMA_BUF_OPT( 4, width * height * 4 ) );
	if ( image ) {
		// one it made before (CreateImage2D gives the same image again) counts once
		if ( !SCR_ModuleMadeImage( m, image ) ) {
			m->images[m->numImages++] = image;
		}
		Hooks_ModuleAsset( ext, EXT_ASSET_IMAGE2D, fullName, image, NULL );
	}
	return image;
}


/*
=================
SCR_Module2DCall

The 2D extension calls of a cgame or ui, which the engine extensions see;
one may take an image's update or a batch (hooks.h's moduleRender)
=================
*/
intptr_t SCR_Module2DCall( extModule_t ext, intptr_t *args ) {
	module2D_t	*m = CL_Module2D( ext );
	tris2D_t	tris;
	const int	*rect;
	const byte	*pixels;
	int			width, height;

	switch ( args[0] ) {
	case COM_TRAP_R_SETCLIPREGION:
		rect = VMA_BUF_OPT( 1, sizeof( m->clip ) );
		if ( Hooks_WantModuleRender( ) ) {
			extRenderCall_t	call = { EXT_RENDER_CLIP_REGION };

			call.clip = rect;
			Hooks_ModuleRenderSeen( ext, &call );
		}
		m->clipped = rect != NULL;
		if ( rect ) {
			Com_Memcpy( m->clip, rect, sizeof( m->clip ) );
		}
		return 0;

	case COM_TRAP_R_CREATEIMAGE2D:
		return SCR_ModuleCreateImage2D( ext, args );

	case COM_TRAP_R_UPDATEIMAGE2D:
		if ( !SCR_ModuleMadeImage( m, args[1] ) ) {
			VM_ArgError( args, 1, "%i isn't an image the module made", (int)args[1] );
		}
		width = VMV_RANGE( 4, 1, MAX_MODULE_IMAGE_SIZE );
		height = VMV_RANGE( 5, 1, MAX_MODULE_IMAGE_SIZE );
		pixels = VMA_BUF( 6, width * height * 4 );
		if ( Hooks_WantModuleRender( ) ) {
			extRenderCall_t	call = { EXT_RENDER_UPDATE_IMAGE };

			call.image = args[1];
			call.area[0] = args[2];
			call.area[1] = args[3];
			call.area[2] = width;
			call.area[3] = height;
			call.pixels = pixels;
			if ( Hooks_ModuleRender( ext, &call ) ) {
				return 0;
			}
		}
		re.UpdateImage2D( args[1], args[2], args[3], width, height, pixels );
		return 0;

	case COM_TRAP_R_DRAWTRIANGLES2D:
		Com_Memset( &tris, 0, sizeof( tris ) );
		tris.state.texture = args[1];
		tris.state.flags = args[6];
		if ( tris.state.flags & ~TRIS2D_QUADS ) {
			VM_ArgError( args, 6, "has flags besides TRIS2D_QUADS (0x%x)", tris.state.flags );
		}
		tris.numVerts = args[3];
		if ( !( tris.state.flags & TRIS2D_QUADS ) ) {
			tris.numIndexes = args[5];
		}
		if ( !tris.numVerts || ( !( tris.state.flags & TRIS2D_QUADS ) && !tris.numIndexes ) ) {
			return 0;
		}
		tris.verts = VMA_ARRAY( 2, polyVert2D_t, tris.numVerts );
		if ( tris.numIndexes ) {
			tris.indexes = VMA_ARRAY( 4, int, tris.numIndexes );
		}
		if ( m->clipped ) {
			tris.state.flags |= TRIS2D_SCISSOR;
			Com_Memcpy( tris.state.scissor, m->clip, sizeof( m->clip ) );
		}
		if ( Hooks_WantModuleRender( ) ) {
			extRenderCall_t	call = { EXT_RENDER_TRIANGLES };

			call.tris = &tris;
			call.clip = m->clipped ? m->clip : NULL;
			if ( Hooks_ModuleRender( ext, &call ) ) {
				return 0;
			}
		}
		re.DrawTriangles2D( &tris );
		return 0;
	}

	Com_Error( ERR_DROP, "SCR_Module2DCall: bad call %i", (int)args[0] );
	return 0;
}



/*
** SCR_DrawChar
** chars are drawn at 640*480 virtual screen size
*/
static void SCR_DrawChar( int x, int y, float size, int ch ) {
	int row, col;
	float frow, fcol;
	float	ax, ay, aw, ah;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -size ) {
		return;
	}

	ax = x;
	ay = y;
	aw = size;
	ah = size;
	SCR_AdjustFrom640( &ax, &ay, &aw, &ah );

	row = ch>>4;
	col = ch&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;

	re.DrawStretchPic( ax, ay, aw, ah,
					   fcol, frow, 
					   fcol + size, frow + size, 
					   cls.charSetShader );
}

/*
** SCR_DrawSmallChar
** small chars are drawn at native screen resolution
*/
void SCR_DrawSmallChar( int x, int y, int ch ) {
	int row, col;
	float frow, fcol;
	float size;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -g_smallchar_height ) {
		return;
	}

	row = ch>>4;
	col = ch&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;

	re.DrawStretchPic( x, y, g_smallchar_width, g_smallchar_height,
					   fcol, frow, 
					   fcol + size, frow + size, 
					   cls.charSetShader );
}


/*
==================
SCR_DrawBigString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.

Coordinates are at 640 by 480 virtual resolution
==================
*/
void SCR_DrawStringExt( int x, int y, float size, const char *string, float *setColor, qboolean forceColor,
		qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the drop shadow
	color[0] = color[1] = color[2] = 0;
	color[3] = setColor[3];
	re.SetColor( color );
	s = string;
	xx = x;
	while ( *s ) {
		if ( !noColorEscape && Q_IsColorString( s ) ) {
			s += 2;
			continue;
		}
		SCR_DrawChar( xx+2, y+2, size, *s );
		xx += size;
		s++;
	}


	// draw the colored text
	s = string;
	xx = x;
	re.SetColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				Com_Memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				re.SetColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawChar( xx, y, size, *s );
		xx += size;
		s++;
	}
	re.SetColor( NULL );
}


void SCR_DrawBigString( int x, int y, const char *s, float alpha, qboolean noColorEscape ) {
	float	color[4];

	color[0] = color[1] = color[2] = 1.0;
	color[3] = alpha;
	SCR_DrawStringExt( x, y, BIGCHAR_WIDTH, s, color, qfalse, noColorEscape );
}

void SCR_DrawBigStringColor( int x, int y, const char *s, vec4_t color, qboolean noColorEscape ) {
	SCR_DrawStringExt( x, y, BIGCHAR_WIDTH, s, color, qtrue, noColorEscape );
}


/*
==================
SCR_DrawSmallString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.
==================
*/
void SCR_DrawSmallStringExt( int x, int y, const char *string, float *setColor, qboolean forceColor,
		qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the colored text
	s = string;
	xx = x;
	re.SetColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				Com_Memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				re.SetColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawSmallChar( xx, y, *s );
		xx += g_smallchar_width;
		s++;
	}
	re.SetColor( NULL );
}



/*
** SCR_Strlen -- skips color escape codes
*/
static int SCR_Strlen( const char *str ) {
	const char *s = str;
	int count = 0;

	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			s += 2;
		} else {
			count++;
			s++;
		}
	}

	return count;
}

/*
** SCR_GetBigStringWidth
*/ 
int	SCR_GetBigStringWidth( const char *str ) {
	return SCR_Strlen( str ) * BIGCHAR_WIDTH;
}


//===============================================================================

/*
=================
SCR_DrawDemoRecording
=================
*/
void SCR_DrawDemoRecording( void ) {
	char	string[1024];
	int		pos;

	if ( !clc.demorecording ) {
		return;
	}
	if ( clc.spDemoRecording ) {
		return;
	}

	pos = FS_FTell( clc.demofile );
	sprintf( string, "RECORDING %s: %ik", clc.demoName, pos / 1024 );

	SCR_SetPlacement( PLACE_CENTER, PLACE_TOP );
	SCR_DrawStringExt( 320 - strlen( string ) * 4, 20, 8, string, g_color_table[7], qtrue, qfalse );
	SCR_SetPlacement( PLACE_STRETCH, PLACE_STRETCH );
}


#ifdef USE_VOIP
/*
=================
SCR_DrawVoipMeter
=================
*/
void SCR_DrawVoipMeter( void ) {
	char	buffer[16];
	char	string[256];
	int limit, i;

	if (!cl_voipShowMeter->integer)
		return;  // player doesn't want to show meter at all.
	else if (!cl_voipSend->integer)
		return;  // not recording at the moment.
	else if (clc.state != CA_ACTIVE)
		return;  // not connected to a server.
	else if (!clc.voipEnabled)
		return;  // server doesn't support VoIP.
	else if (clc.demoplaying)
		return;  // playing back a demo.
	else if (!CL_VoipOn())
		return;  // client has VoIP support disabled.

	limit = (int) (clc.voipPower * 10.0f);
	if (limit > 10)
		limit = 10;

	for (i = 0; i < limit; i++)
		buffer[i] = '*';
	while (i < 10)
		buffer[i++] = ' ';
	buffer[i] = '\0';

	sprintf( string, "VoIP: [%s]", buffer );
	SCR_SetPlacement( PLACE_CENTER, PLACE_TOP );
	SCR_DrawStringExt( 320 - strlen( string ) * 4, 10, 8, string, g_color_table[7], qtrue, qfalse );
	SCR_SetPlacement( PLACE_STRETCH, PLACE_STRETCH );
}
#endif




/*
===============================================================================

DEBUG GRAPH

===============================================================================
*/

static	int			current;
static	float		values[1024];

/*
==============
SCR_DebugGraph
==============
*/
void SCR_DebugGraph (float value)
{
	values[current] = value;
	current = (current + 1) % ARRAY_LEN(values);
}

/*
==============
SCR_DrawDebugGraph
==============
*/
void SCR_DrawDebugGraph (void)
{
	int		a, x, y, w, i, h;
	float	v;

	//
	// draw the graph
	//
	w = cls.glconfig.vidWidth;
	x = 0;
	y = cls.glconfig.vidHeight;
	re.SetColor( g_color_table[0] );
	re.DrawStretchPic(x, y - cl_graphheight->integer, 
		w, cl_graphheight->integer, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );

	for (a=0 ; a<w ; a++)
	{
		i = (ARRAY_LEN(values)+current-1-(a % ARRAY_LEN(values))) % ARRAY_LEN(values);
		v = values[i];
		v = v * cl_graphscale->integer + cl_graphshift->integer;
		
		if (v < 0)
			v += cl_graphheight->integer * (1+(int)(-v / cl_graphheight->integer));
		h = (int)v % cl_graphheight->integer;
		re.DrawStretchPic( x+w-1-a, y - h, 1, h, 0, 0, 0, 0, cls.whiteShader );
	}
}

//=============================================================================

/*
==================
SCR_Init
==================
*/
void SCR_Init( void ) {
	cl_timegraph = Cvar_Get ("timegraph", "0", CVAR_CHEAT);
	cl_debuggraph = Cvar_Get ("debuggraph", "0", CVAR_CHEAT);
	cl_graphheight = Cvar_Get ("graphheight", "32", CVAR_CHEAT);
	cl_graphscale = Cvar_Get ("graphscale", "1", CVAR_CHEAT);
	cl_graphshift = Cvar_Get ("graphshift", "0", CVAR_CHEAT);

	scr_initialized = qtrue;
}


//=======================================================

/*
==================
SCR_DrawUI

The ui's drawing, which ui_speeds times: its refresh, then the connect
screen when connect is set, over the loading screen when loading is
==================
*/
static void SCR_DrawUI( qboolean connect, qboolean loading ) {
	if ( cl_uiSpeeds ) {
		CL_UISpeedsSpan( UISPEEDS_SPAN_UI, qtrue );
	}
	VM_Call( uivm, UI_REFRESH, cls.realtime );
	if ( connect ) {
		VM_Call( uivm, UI_DRAW_CONNECT_SCREEN, loading );
	}
	if ( cl_uiSpeeds ) {
		CL_UISpeedsSpan( UISPEEDS_SPAN_UI, qfalse );
	}
}

/*
==================
SCR_DrawExtensions

The engine extensions' drawing at a draw point, which ui_speeds times while
an active extension has a draw hook
==================
*/
static void SCR_DrawExtensions( extDrawPoint_t point, int stereo ) {
	if ( !cl_uiSpeeds || !Hooks_WantDraw() ) {
		Hooks_Draw( point, stereo );
		return;
	}
	CL_UISpeedsSpan( UISPEEDS_SPAN_EXTENSIONS, qtrue );
	Hooks_Draw( point, stereo );
	CL_UISpeedsSpan( UISPEEDS_SPAN_EXTENSIONS, qfalse );
}

/*
==================
SCR_DrawScreenField

This will be called twice if rendering in stereo mode
==================
*/
void SCR_DrawScreenField( stereoFrame_t stereoFrame ) {
	qboolean uiFullscreen;

	re.BeginFrame( stereoFrame );

	uiFullscreen = (uivm && VM_Call( uivm, UI_IS_FULLSCREEN ));

	// wide aspect ratio screens need to have the sides cleared
	// unless they are displaying game renderings
	if ( uiFullscreen || clc.state < CA_LOADING ) {
		if ( cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640 ) {
			re.SetColor( g_color_table[0] );
			re.DrawStretchPic( 0, 0, cls.glconfig.vidWidth, cls.glconfig.vidHeight, 0, 0, 0, 0, cls.whiteShader );
			re.SetColor( NULL );
		}
	}

	// if the menu is going to cover the entire screen, we
	// don't need to render anything under it
	if ( uivm && !uiFullscreen ) {
		switch( clc.state ) {
		default:
			Com_Error( ERR_FATAL, "SCR_DrawScreenField: bad clc.state" );
			break;
		case CA_CINEMATIC:
			SCR_DrawCinematic();
			break;
		case CA_DISCONNECTED:
			// force menu up
			S_StopAllSounds();
			VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
			break;
		case CA_CONNECTING:
		case CA_CHALLENGING:
		case CA_CONNECTED:
			// connecting clients will only show the connection dialog
			// refresh to update the time
			SCR_DrawUI( qtrue, qfalse );
			SCR_DrawExtensions( EXT_DRAW_GAME, stereoFrame );
			break;
		case CA_LOADING:
		case CA_PRIMED:
			// draw the game information screen and loading progress
			CL_CGameRendering(stereoFrame);

			// also draw the connection information, so it doesn't
			// flash away too briefly on local or lan games
			// refresh to update the time
			SCR_DrawUI( qtrue, qtrue );
			SCR_DrawExtensions( EXT_DRAW_GAME, stereoFrame );
			break;
		case CA_ACTIVE:
			// always supply STEREO_CENTER as vieworg offset is now done by the engine.
			CL_CGameRendering(stereoFrame);
			// under the recording line and the voice meter
			SCR_DrawExtensions( EXT_DRAW_GAME, stereoFrame );
			SCR_DrawDemoRecording();
#ifdef USE_VOIP
			SCR_DrawVoipMeter();
#endif
			break;
		}
	}

	// the menu draws next
	if ( Key_GetCatcher( ) & KEYCATCH_UI && uivm ) {
		SCR_DrawUI( qfalse, qfalse );
	}
	SCR_DrawExtensions( EXT_DRAW_MENU, stereoFrame );

	// console draws next
	Con_DrawConsole ();
	SCR_DrawExtensions( EXT_DRAW_TOP, stereoFrame );

	// debug graph can be drawn on top of anything
	if ( cl_debuggraph->integer || cl_timegraph->integer || cl_debugMove->integer ) {
		SCR_DrawDebugGraph ();
	}
}

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.
==================
*/
void SCR_UpdateScreen( void ) {
	static int	recursive;
	static int64_t	lastSwap;

	if ( !scr_initialized ) {
		return;				// not initialized yet
	}

	if ( ++recursive > 2 ) {
		Com_Error( ERR_FATAL, "SCR_UpdateScreen: recursively called" );
	}
	recursive = 1;

	// If there is no VM, there are also no rendering commands issued. Stop the renderer in
	// that case.
	if( uivm || com_dedicated->integer )
	{
		CL_UISpeedsFrame();

		// XXX
		int in_anaglyphMode = Cvar_VariableIntegerValue("r_anaglyphMode");
		// if running in stereo, we need to draw the frame twice
		if ( cls.glconfig.stereoEnabled || in_anaglyphMode) {
			SCR_DrawScreenField( STEREO_LEFT );
			SCR_DrawScreenField( STEREO_RIGHT );
		} else {
			SCR_DrawScreenField( STEREO_CENTER );
		}

		if ( com_speeds->integer ) {
			re.EndFrame( &time_frontend, &time_backend );
		} else {
			re.EndFrame( NULL, NULL );
		}
		if ( cl_uiSpeeds ) {
			CL_UISpeedsEndFrame();
		}

		// for com_speeds: the frame has been swapped (natively; a browser
		// shows it once the frame returns)
		{
			int64_t now = Sys_Nanoseconds();

			if ( lastSwap ) {
				time_swap = (int)( ( now - lastSwap ) / 1000 );
			}
			lastSwap = now;
		}
	}
	
	recursive = 0;
}

