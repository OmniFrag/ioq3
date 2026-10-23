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
// tr_times2d.c -- ui_speeds' times for the back end's 2D commands
//
// The back end calls R_Times2DCommand before each command while r_times2D
// is set. The 2D commands come in runs, between the 3D views and the swap;
// each run is timed on the CPU from its first command to the draw of what
// it left in the tessellator, and on the GPU with a GL_TIME_ELAPSED query
// around the same commands, where the driver has timer queries. The
// queries' answers are read frames later, without waiting for them.

#include "tr_common.h"

#define GLE(ret, name, ...) extern name##proc * qgl##name;
QGL_1_1_PROCS;
QGL_ARB_occlusion_query_PROCS;
#undef GLE

#define TIMES2D_FRAMES	( MAX_GPU_TIMES2D + 1 )	// frames waiting for the GPU, and this one
#define TIMES2D_RUNS	16						// runs a frame timed on the GPU

int		r_times2D;

static times2D_t	times;
static int64_t		runStart;		// 0 outside a run
static qboolean		gpuRun;			// a query is open around this run

// each frame's queries, a ring: this frame's, then those waiting
static struct {
	GLuint		ids[TIMES2D_RUNS];
	int			used;
	qboolean	waiting;
} frames[TIMES2D_FRAMES];
static int			current;
static qboolean		queriesMade;

/*
==================
R_Times2DCommand

Whether the run of 2D commands being timed ends here, at a command that
isn't one; the back end then draws what the run left, and ends it
==================
*/
qboolean R_Times2DCommand( qboolean is2D ) {
	if ( !is2D ) {
		return runStart != 0;
	}
	if ( runStart ) {
		return qfalse;
	}
	// the query's own calls stay out of the CPU's time
	if ( queriesMade ) {
		if ( !frames[current].waiting && frames[current].used < TIMES2D_RUNS ) {
			qglBeginQuery( GL_TIME_ELAPSED, frames[current].ids[frames[current].used] );
			gpuRun = qtrue;
		} else {
			times.gpuUntimed++;
		}
	}
	runStart = ri.Nanoseconds();
	times.runs++;
	return qfalse;
}

void R_Times2DEndRun( void ) {
	times.cpu += ri.Nanoseconds() - runStart;
	runStart = 0;
	if ( gpuRun ) {
		qglEndQuery( GL_TIME_ELAPSED );
		frames[current].used++;
		gpuRun = qfalse;
	}
}

static void ClearFrames( void ) {
	int i;

	for ( i = 0; i < TIMES2D_FRAMES; i++ ) {
		frames[i].used = 0;
		frames[i].waiting = qfalse;
	}
}

// the frames whose queries have answered, oldest first; the GPU answers in
// order, so the first still busy holds back those after it. The oldest is
// the one after this frame's slot, or the slot itself while every slot waits
static void CollectFrames( void ) {
	int oldest = frames[current].waiting ? current : current + 1;
	int i, j;

	for ( i = 0; i < TIMES2D_FRAMES && times.gpuFrames < MAX_GPU_TIMES2D; i++ ) {
		int			f = ( oldest + i ) % TIMES2D_FRAMES;
		GLuint		available = 0;
		int64_t		sum = 0;

		if ( !frames[f].waiting ) {
			continue;
		}
		qglGetQueryObjectuiv( frames[f].ids[frames[f].used - 1], GL_QUERY_RESULT_AVAILABLE, &available );
		if ( !available ) {
			break;
		}
		if ( qglesMajorVersion ) {
			GLint disjoint = 0;

			// a GPU on a phone or in a browser may have changed its clock
			// meanwhile, which spoils every answer come since
			qglGetIntegerv( GL_GPU_DISJOINT_EXT, &disjoint );
			if ( disjoint ) {
				ClearFrames();
				return;
			}
		}
		for ( j = 0; j < frames[f].used; j++ ) {
			GLuint ns = 0;

			qglGetQueryObjectuiv( frames[f].ids[j], GL_QUERY_RESULT, &ns );
			sum += ns;
		}
		times.gpu[times.gpuFrames++] = sum;
		frames[f].used = 0;
		frames[f].waiting = qfalse;
	}
}

/*
==================
RE_TimeBackEnd2D

Called after each frame. This frame's queries wait for their answers
while the next frame's are made in the next slot; while every slot waits,
the GPU times no runs.
==================
*/
void RE_TimeBackEnd2D( times2D_t *out, qboolean on ) {
	if ( queriesMade ) {
		// a slot still waiting made no queries this frame, and stays put
		if ( frames[current].used && !frames[current].waiting ) {
			frames[current].waiting = qtrue;
			current = ( current + 1 ) % TIMES2D_FRAMES;
		}
		CollectFrames();
	}
	times.gpuTimer = queriesMade;
	if ( out ) {
		*out = times;
	}
	Com_Memset( &times, 0, sizeof( times ) );

	if ( on && !queriesMade && qglTimerQuery ) {
		int i;

		for ( i = 0; i < TIMES2D_FRAMES; i++ ) {
			qglGenQueries( TIMES2D_RUNS, frames[i].ids );
		}
		queriesMade = qtrue;
	}
	r_times2D = on;
}

/*
==================
R_Times2DShutdown

The queries go with the GL context
==================
*/
void R_Times2DShutdown( void ) {
	int i;

	if ( queriesMade ) {
		for ( i = 0; i < TIMES2D_FRAMES; i++ ) {
			qglDeleteQueries( TIMES2D_RUNS, frames[i].ids );
		}
		queriesMade = qfalse;
	}
	ClearFrames();
	Com_Memset( &times, 0, sizeof( times ) );
	runStart = 0;
	gpuRun = qfalse;
	r_times2D = 0;
}
