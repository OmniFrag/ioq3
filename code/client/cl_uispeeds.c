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
// whole frame where it renders none; the ui's drawing; the engine
// extensions' at each draw point, while one has a draw hook; the renderer's
// front end, through wrappers that stand in for its 2D draws; and the back
// end's 2D commands (RE_TimeBackEnd2D). While ui_speeds is 0, the wrappers
// are out of the renderer's table and the back end doesn't time, so each
// span costs one check of cl_uiSpeeds.

#include "client.h"
#include "cl_uispeeds.h"

#define UISPEEDS_CAPACITY		8192	// samples held a part; more are thinned
#define UISPEEDS_FRONT_END_EVERY	16		// one front-end call timed in so many

enum {
	UISPEEDS_FRAME,
	UISPEEDS_CGAME,
	UISPEEDS_UI,
	UISPEEDS_EXTENSIONS,
	UISPEEDS_FRONT_END,
	UISPEEDS_COMMANDS,
	UISPEEDS_BACK_END,
	UISPEEDS_GPU,
	UISPEEDS_PARTS
};

// CL_UISpeedsEndFrame adds each span to the part at UISPEEDS_CGAME plus its
// number
#define UISPEEDS_SPAN_CGAME	0	// before client.h's spans
typedef char uisSpansInPartsOrder[ UISPEEDS_UI - UISPEEDS_CGAME == UISPEEDS_SPAN_UI &&
	UISPEEDS_EXTENSIONS - UISPEEDS_CGAME == UISPEEDS_SPAN_EXTENSIONS &&
	UISPEEDS_FRONT_END - UISPEEDS_CGAME == UISPEEDS_SPANS ? 1 : -1 ];

static const char *partNames[UISPEEDS_PARTS] = {
	"frame", "cgame 2D", "ui 2D", "extensions 2D", "front end 2D", "2D commands", "back end 2D", "GPU 2D"
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
static uisSamples_t	parts[UISPEEDS_PARTS];
static int			*storage;	// the parts' samples, then scratch for sorting

// a kind of 2D call that costs the same each time: one in every is timed,
// at a phase drawn each frame, so a frame's calls, the same frame after
// frame, are timed in turn. every is UISPEEDS_FRONT_END_EVERY, or the calls the
// last frame made when fewer, so a frame like it times one of them and
// isn't left out.
typedef struct {
	int			calls, timed;
	int			every, phase;
	int64_t		time;		// the timed ones'
} uisSampled_t;

// this frame's
static struct {
	int64_t		lastEnd;	// the last frame's end, 0 before the first
	qboolean	inCgame, sceneSeen;
	uisSpan_t	spans[UISPEEDS_SPANS];	// in the parts' order, from UISPEEDS_CGAME
	uisSampled_t	pictures;	// pictures and colors
	uisSampled_t	retained;	// retained geometry, clip masks and layers
	int64_t		batches;	// every batch of triangles
	int			batchesTimed;
} f;

// a call's start when it's the one of its kind timed; 0 for the others
static int64_t CallStart( uisSampled_t *kind ) {
	return ++kind->calls % kind->every == kind->phase ? Sys_Nanoseconds() : 0;
}

static void CallEnd( uisSampled_t *kind, int64_t start ) {
	if ( start ) {
		kind->time += Sys_Nanoseconds() - start;
		kind->timed++;
	}
}

// the calls timed so far this frame, whose clock reads the spans hold
static int TimedCalls( void ) {
	return f.pictures.timed + f.retained.timed + f.batchesTimed;
}

// a kind's calls as long as its timed ones took, which leaves no bias
// when they cost the same, the phase being random; qfalse when it had
// some, none of them timed
static qboolean AddSampled( const uisSampled_t *kind, int64_t *frontEnd ) {
	if ( !kind->timed ) {
		return !kind->calls;
	}
	*frontEnd += UISpeeds_Corrected( kind->time, kind->timed, clockCost ) * kind->calls / kind->timed;
	return qtrue;
}

// the next frame's sampling of a kind that made lastCalls this frame
static void NextSample( uisSampled_t *kind, int lastCalls ) {
	static unsigned int seed = 1;

	kind->every = lastCalls > 0 && lastCalls < UISPEEDS_FRONT_END_EVERY ? lastCalls : UISPEEDS_FRONT_END_EVERY;
	seed = seed * 1103515245u + 12345u;
	kind->phase = ( seed >> 16 ) % kind->every;
}

static void Speeds_SetColor( const float *rgba ) {
	int64_t start = CallStart( &f.pictures );

	real.SetColor( rgba );
	CallEnd( &f.pictures, start );
}

static void Speeds_DrawStretchPic( float x, float y, float w, float h,
		float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	int64_t start = CallStart( &f.pictures );

	real.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
	CallEnd( &f.pictures, start );
}

// a batch costs as its vertexes do, and a frame has few: each is timed
static void Speeds_DrawTriangles2D( const tris2D_t *tris ) {
	int64_t start = Sys_Nanoseconds();

	real.DrawTriangles2D( tris );
	f.batches += Sys_Nanoseconds() - start;
	f.batchesTimed++;
}

// a retained geometry drawn, or drawn into the clip mask: a small command
// each, which a retained UI makes many of
static void Speeds_DrawGeometry2D( int geometry, const draw2DState_t *state ) {
	int64_t start = CallStart( &f.retained );

	real.DrawGeometry2D( geometry, state );
	CallEnd( &f.retained, start );
}

static void Speeds_SetClipMask2D( clipMaskOp_t op, int geometry, const draw2DState_t *state ) {
	int64_t start = CallStart( &f.retained );

	real.SetClipMask2D( op, geometry, state );
	CallEnd( &f.retained, start );
}

// a layer pushed, composited or popped: a small command each too
static int Speeds_PushLayer2D( void ) {
	int64_t	start = CallStart( &f.retained );
	int		layer = real.PushLayer2D();

	CallEnd( &f.retained, start );
	return layer;
}

static void Speeds_CompositeLayers2D( int source, int destination, int blendMode, const int *filters, int numFilters,
		const draw2DState_t *state ) {
	int64_t start = CallStart( &f.retained );

	real.CompositeLayers2D( source, destination, blendMode, filters, numFilters, state );
	CallEnd( &f.retained, start );
}

static void Speeds_PopLayer2D( void ) {
	int64_t start = CallStart( &f.retained );

	real.PopLayer2D();
	CallEnd( &f.retained, start );
}

static void SpanBegin( uisSpan_t *span ) {
	span->start = Sys_Nanoseconds();
	span->timedAtStart = TimedCalls();
}

// its own clock read and the two of each call timed inside it come off
static void SpanEnd( uisSpan_t *span ) {
	int reads = 1 + 2 * ( TimedCalls() - span->timedAtStart );

	span->total += UISpeeds_Corrected( Sys_Nanoseconds() - span->start, reads, clockCost );
	span->drawn = qtrue;
}

// the cgame's 2D begins as its first scene, its 3D view, returns
static void Speeds_RenderScene( const refdef_t *fd ) {
	real.RenderScene( fd );
	if ( f.inCgame && !f.sceneSeen ) {
		f.sceneSeen = qtrue;
		SpanBegin( &f.spans[UISPEEDS_SPAN_CGAME] );
	}
}

// as tr_public.h's TimeBackEnd2D says: the calls that only queue a 2D
// draw. UpdateImage2D, UpdateTexture2D and the deletes stay out, since each
// may draw what waits in the back end inside the call, and so do the
// create calls, which make a resource rather than draw
static void Wrap( void ) {
	real = re;
	re.SetColor = Speeds_SetColor;
	re.DrawStretchPic = Speeds_DrawStretchPic;
	re.DrawTriangles2D = Speeds_DrawTriangles2D;
	re.DrawGeometry2D = Speeds_DrawGeometry2D;
	re.SetClipMask2D = Speeds_SetClipMask2D;
	re.PushLayer2D = Speeds_PushLayer2D;
	re.CompositeLayers2D = Speeds_CompositeLayers2D;
	re.PopLayer2D = Speeds_PopLayer2D;
	re.RenderScene = Speeds_RenderScene;
	Com_Memset( &f, 0, sizeof( f ) );
	NextSample( &f.pictures, 0 );
	NextSample( &f.retained, 0 );
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

	for ( i = 0; i < UISPEEDS_PARTS; i++ ) {
		UISpeeds_Clear( &parts[i] );
	}
	gpuUntimed = 0;
	f.lastEnd = 0;
}

static void Start( void ) {
	int i;

	storage = Z_Malloc( ( UISPEEDS_PARTS + 1 ) * UISPEEDS_CAPACITY * sizeof( *storage ) );
	for ( i = 0; i < UISPEEDS_PARTS; i++ ) {
		UISpeeds_Init( &parts[i], storage + i * UISPEEDS_CAPACITY, UISPEEDS_CAPACITY );
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

	if ( !print || !parts[UISPEEDS_FRAME].seen ) {
		ClearParts();
		return;
	}
	Com_Printf( "ui_speeds: %d frames, per frame: median, p95, max, mean; in usec, less %.0f ns a clock read\n",
		parts[UISPEEDS_FRAME].seen, clockCost );
	for ( i = 0; i < UISPEEDS_PARTS; i++ ) {
		// counts as they are; times in microseconds
		double scale = i == UISPEEDS_COMMANDS ? 1 : 0.001;

		if ( !UISpeeds_Summarize( &parts[i], storage + UISPEEDS_PARTS * UISPEEDS_CAPACITY, &sum ) ) {
			Com_Printf( "  %-14s %s\n", partNames[i], i == UISPEEDS_GPU && !gpuTimer ? "no timer queries" : "none" );
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
	int			pictures = f.pictures.calls, retained = f.retained.calls;
	qboolean	picturesSampled, retainedSampled;
	int			i;

	re.TimeBackEnd2D( &be, qtrue );
	gpuTimer = be.gpuTimer;
	if ( f.lastEnd ) {
		UISpeeds_Add( &parts[UISPEEDS_FRAME], now - f.lastEnd );
		for ( i = 0; i < UISPEEDS_SPANS; i++ ) {
			if ( f.spans[i].drawn ) {
				UISpeeds_Add( &parts[UISPEEDS_CGAME + i], f.spans[i].total );
			}
		}
		// a frame where a kind went untimed is left out
		frontEnd = UISpeeds_Corrected( f.batches, f.batchesTimed, clockCost );
		picturesSampled = AddSampled( &f.pictures, &frontEnd );
		retainedSampled = AddSampled( &f.retained, &frontEnd );
		if ( picturesSampled && retainedSampled ) {
			UISpeeds_Add( &parts[UISPEEDS_FRONT_END], frontEnd );
		}
		UISpeeds_Add( &parts[UISPEEDS_COMMANDS], pictures + retained + f.batchesTimed );
		UISpeeds_Add( &parts[UISPEEDS_BACK_END], UISpeeds_Corrected( be.cpu, be.runs, clockCost ) );
		for ( i = 0; i < be.gpuFrames; i++ ) {
			UISpeeds_Add( &parts[UISPEEDS_GPU], be.gpu[i] );
		}
		gpuUntimed += be.gpuUntimed;
	}
	Com_Memset( &f, 0, sizeof( f ) );
	f.lastEnd = now;
	NextSample( &f.pictures, pictures );
	NextSample( &f.retained, retained );
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
		SpanBegin( &f.spans[UISPEEDS_SPAN_CGAME] );
		return;
	}
	SpanEnd( &f.spans[UISPEEDS_SPAN_CGAME] );
	f.inCgame = qfalse;
}

/*
====================
CL_UISpeedsSpan

Brackets the ui's drawing (UISPEEDS_SPAN_UI) or the engine extensions' at
a draw point (UISPEEDS_SPAN_EXTENSIONS): their own work, such as a UI's
layout and scripts, and the 2D they draw
====================
*/
void CL_UISpeedsSpan( uiSpeedsSpan_t span, qboolean begin ) {
	if ( begin ) {
		SpanBegin( &f.spans[span] );
	} else {
		SpanEnd( &f.spans[span] );
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
		"the ui's, the engine extensions' at each draw point while one has a draw hook, "
		"and the renderer's front end and back end, on the GPU too where it has timer queries. "
		"Setting it back to 0, and the end of each match or demo, print their median, p95, max and mean per frame." );
}
