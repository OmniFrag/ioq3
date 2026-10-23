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
// cl_uispeeds.c -- ui_speeds: what the 2D costs each frame
//
// The parts timed: the cgame's 2D, from the return of the first scene it
// renders in CG_DRAW_ACTIVE_FRAME (its 3D view) to the frame's end, or the
// whole frame where it renders none; the ui's drawing; the renderer's
// front end, through wrappers that stand in for its 2D draws; and the back
// end's 2D commands (RE_TimeBackEnd2D). While ui_speeds is 0, the wrappers
// are out of the renderer's table and the back end doesn't time, so each
// span costs one check of cl_uiSpeeds.

#include "client.h"
#include "cl_uispeeds.h"

#define UIS_CAPACITY		8192	// samples held a part; more are thinned
#define UIS_FRONT_END_EVERY	16		// one front-end call timed in so many

enum {
	UIS_FRAME,
	UIS_CGAME,
	UIS_UI,
	UIS_FRONT_END,
	UIS_COMMANDS,
	UIS_BACK_END,
	UIS_GPU,
	UIS_PARTS
};

static const char *partNames[UIS_PARTS] = {
	"frame", "cgame 2D", "ui 2D", "front end 2D", "2D commands", "back end 2D", "GPU 2D"
};

// a module's drawing this frame, less the clock reads of the calls timed
// inside it
typedef struct {
	int64_t		start, total;
	int			timedAtStart;
	qboolean	drawn;
} uisSpan_t;

qboolean			cl_uiSpeeds;

static cvar_t		*ui_speeds;
static refexport_t	real;		// the renderer's own entry points ours stand in for
static double		clockCost;	// a clock read's, in nanoseconds
static qboolean		gpuTimer;
static int			gpuUntimed;	// runs the GPU didn't time, for want of queries
static uisSamples_t	parts[UIS_PARTS];
static int			*storage;	// the parts' samples, then scratch for sorting

// this frame's
static struct {
	int64_t		lastEnd;	// the last frame's end, 0 before the first
	qboolean	inCgame, sceneSeen;
	uisSpan_t	cgame, ui;
	int			calls;		// the front end's 2D calls
	int			pictures;	// pictures and colors
	int64_t		sampled;	// the timed ones of them
	int			sampledTimed;
	int64_t		batches;	// every batch of triangles
	int			batchesTimed;
	int			phase;		// which call of each UIS_FRONT_END_EVERY is timed
} f;

// a cheap 2D call's start, when it's the one in UIS_FRONT_END_EVERY timed,
// at a phase drawn each frame, so a frame's calls, the same frame after
// frame, are timed in turn; 0 for the others, which are counted
static int64_t CallStart( void ) {
	f.calls++;
	return ++f.pictures % UIS_FRONT_END_EVERY == f.phase ? Sys_Nanoseconds() : 0;
}

static void CallEnd( int64_t start ) {
	if ( start ) {
		f.sampled += Sys_Nanoseconds() - start;
		f.sampledTimed++;
	}
}

static int NextPhase( void ) {
	static unsigned int seed = 1;

	seed = seed * 1103515245u + 12345u;
	return ( seed >> 16 ) % UIS_FRONT_END_EVERY;
}

static void Speeds_SetColor( const float *rgba ) {
	int64_t start = CallStart();

	real.SetColor( rgba );
	CallEnd( start );
}

static void Speeds_DrawStretchPic( float x, float y, float w, float h,
		float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	int64_t start = CallStart();

	real.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
	CallEnd( start );
}

// a batch costs as its vertexes do, and a frame has few: each is timed
static void Speeds_DrawTriangles2D( const tris2D_t *tris ) {
	int64_t start = Sys_Nanoseconds();

	real.DrawTriangles2D( tris );
	f.batches += Sys_Nanoseconds() - start;
	f.batchesTimed++;
	f.calls++;
}

static void SpanBegin( uisSpan_t *span ) {
	span->start = Sys_Nanoseconds();
	span->timedAtStart = f.sampledTimed + f.batchesTimed;
}

// its own clock read and the two of each call timed inside it come off
static void SpanEnd( uisSpan_t *span ) {
	int reads = 1 + 2 * ( f.sampledTimed + f.batchesTimed - span->timedAtStart );

	span->total += UIS_Corrected( Sys_Nanoseconds() - span->start, reads, clockCost );
	span->drawn = qtrue;
}

// the cgame's 2D begins as its first scene, its 3D view, returns
static void Speeds_RenderScene( const refdef_t *fd ) {
	real.RenderScene( fd );
	if ( f.inCgame && !f.sceneSeen ) {
		f.sceneSeen = qtrue;
		SpanBegin( &f.cgame );
	}
}

// UpdateImage2D stays out: a large update draws what waits in the back end
// inside the call, which the back end's runs already time
static void Wrap( void ) {
	real = re;
	re.SetColor = Speeds_SetColor;
	re.DrawStretchPic = Speeds_DrawStretchPic;
	re.DrawTriangles2D = Speeds_DrawTriangles2D;
	re.RenderScene = Speeds_RenderScene;
	Com_Memset( &f, 0, sizeof( f ) );
}

// real is the whole table, taken again whenever the renderer loads
static void Unwrap( void ) {
	re = real;
}

static double ClockCost( void ) {
	int64_t	start = Sys_Nanoseconds(), end = start;
	int		i;

	for ( i = 0; i < 10000; i++ ) {
		end = Sys_Nanoseconds();
	}
	return ( end - start ) / 10000.0;
}

static void ClearParts( void ) {
	int i;

	for ( i = 0; i < UIS_PARTS; i++ ) {
		UIS_Clear( &parts[i] );
	}
	gpuUntimed = 0;
	f.lastEnd = 0;
}

static void Start( void ) {
	int i;

	storage = Z_Malloc( ( UIS_PARTS + 1 ) * UIS_CAPACITY * sizeof( *storage ) );
	for ( i = 0; i < UIS_PARTS; i++ ) {
		UIS_Init( &parts[i], storage + i * UIS_CAPACITY, UIS_CAPACITY );
	}
	clockCost = ClockCost();
	gpuTimer = qfalse;
	Wrap();
	ClearParts();
	cl_uiSpeeds = qtrue;
}

static void Stop( void ) {
	CL_UISpeedsReport( qtrue );
	Unwrap();
	re.TimeBackEnd2D( NULL, qfalse );
	Z_Free( storage );
	storage = NULL;
	cl_uiSpeeds = qfalse;
}

/*
====================
CL_UISpeedsReport

Prints each part's summary over the frames sampled, if asked to and there
were any, and starts over
====================
*/
void CL_UISpeedsReport( qboolean print ) {
	uisSummary_t	sum;
	int				i;

	if ( !print || !parts[UIS_FRAME].seen ) {
		ClearParts();
		return;
	}
	Com_Printf( "ui_speeds: %d frames, per frame: median, p95, max, mean; in usec, less %.0f ns a clock read\n",
		parts[UIS_FRAME].seen, clockCost );
	for ( i = 0; i < UIS_PARTS; i++ ) {
		// counts as they are; times in microseconds
		double scale = i == UIS_COMMANDS ? 1 : 0.001;

		if ( !UIS_Summarize( &parts[i], storage + UIS_PARTS * UIS_CAPACITY, &sum ) ) {
			Com_Printf( "  %-14s %s\n", partNames[i], i == UIS_GPU && !gpuTimer ? "no timer queries" : "none" );
			continue;
		}
		Com_Printf( "  %-14s %9.1f %9.1f %9.1f %9.1f  (%d frames)\n", partNames[i],
			sum.median * scale, sum.p95 * scale, sum.max * scale, sum.mean * scale, sum.count );
	}
	if ( gpuUntimed ) {
		Com_Printf( "  the GPU didn't time %d runs of 2D commands, for want of queries\n", gpuUntimed );
	}
	ClearParts();
}

/*
====================
CL_UISpeedsFrame

Follows ui_speeds, before a frame draws
====================
*/
void CL_UISpeedsFrame( void ) {
	if ( !ui_speeds->modified ) {
		return;
	}
	ui_speeds->modified = qfalse;
	if ( ui_speeds->integer && !cl_uiSpeeds ) {
		Start();
	} else if ( !ui_speeds->integer && cl_uiSpeeds ) {
		Stop();
	}
}

/*
====================
CL_UISpeedsEndFrame

Takes the frame's samples, after the renderer has drawn it. The first
frame after sampling starts, or the renderer restarts, began untimed.
====================
*/
void CL_UISpeedsEndFrame( void ) {
	times2D_t	be;
	int64_t		now = Sys_Nanoseconds(), frontEnd;
	int			i;

	re.TimeBackEnd2D( &be, qtrue );
	gpuTimer = be.gpuTimer;
	if ( f.lastEnd ) {
		UIS_Add( &parts[UIS_FRAME], now - f.lastEnd );
		if ( f.cgame.drawn ) {
			UIS_Add( &parts[UIS_CGAME], f.cgame.total );
		}
		if ( f.ui.drawn ) {
			UIS_Add( &parts[UIS_UI], f.ui.total );
		}
		// the pictures and colors untimed as long as those timed took; a
		// frame with some, none of them timed, is left out, which leaves no
		// bias, its phase being random
		if ( f.sampledTimed || !f.pictures ) {
			frontEnd = UIS_Corrected( f.batches, f.batchesTimed, clockCost );
			if ( f.sampledTimed ) {
				frontEnd += UIS_Corrected( f.sampled, f.sampledTimed, clockCost ) * f.pictures / f.sampledTimed;
			}
			UIS_Add( &parts[UIS_FRONT_END], frontEnd );
		}
		UIS_Add( &parts[UIS_COMMANDS], f.calls );
		UIS_Add( &parts[UIS_BACK_END], UIS_Corrected( be.cpu, be.runs, clockCost ) );
		for ( i = 0; i < be.gpuFrames; i++ ) {
			UIS_Add( &parts[UIS_GPU], be.gpu[i] );
		}
		gpuUntimed += be.gpuUntimed;
	}
	Com_Memset( &f, 0, sizeof( f ) );
	f.lastEnd = now;
	f.phase = NextPhase();
}

/*
====================
CL_UISpeedsCgame

Brackets the cgame's CG_DRAW_ACTIVE_FRAME
====================
*/
void CL_UISpeedsCgame( qboolean begin ) {
	if ( begin ) {
		f.inCgame = qtrue;
		f.sceneSeen = qfalse;
		SpanBegin( &f.cgame );
		return;
	}
	SpanEnd( &f.cgame );
	f.inCgame = qfalse;
}

/*
====================
CL_UISpeedsUI

Brackets the ui's drawing
====================
*/
void CL_UISpeedsUI( qboolean begin ) {
	if ( begin ) {
		SpanBegin( &f.ui );
	} else {
		SpanEnd( &f.ui );
	}
}

/*
====================
CL_UISpeedsRendererLoaded

A new renderer's table, which the wrappers stand in for again
====================
*/
void CL_UISpeedsRendererLoaded( void ) {
	if ( cl_uiSpeeds ) {
		Wrap();
	}
}

void CL_UISpeedsInit( void ) {
	ui_speeds = Cvar_Get( "ui_speeds", "0", CVAR_TEMP );
	Cvar_SetDescription( ui_speeds, "Timers for the HUD's and menus' 2D, on the CPU and the GPU\n"
		"1 times each frame's 2D in its parts: the cgame's, from its 3D view's return to the end of its frame, "
		"the ui's, and the renderer's front end and back end, on the GPU too where it has timer queries. "
		"Setting it back to 0, and the end of each match or demo, print their median, p95, max and mean per frame." );
}
