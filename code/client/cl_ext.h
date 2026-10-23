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
// cl_ext.h -- the client for engine extensions (qcommon/hooks.h), where it
// takes the renderer's types, which hooks.h stays free of

#ifndef CL_EXT_H
#define CL_EXT_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../renderercommon/tr_public.h"

// the renderer's table, NULL while it's stopped: between rendererStopping
// and rendererStarted, and on a dedicated server
const refexport_t	*CL_ExtRenderer( void );
// the window's size and the renderer's limits, as the game modules get them
const glconfig_t	*CL_ExtGlconfig( void );

// A game module's renderer call, for the moduleRender hook: kind says which,
// and which fields hold its arguments. Pointers are the module's, valid
// during the call; a QVM's were checked as every syscall's are. Values are
// as the module gave them, which the renderer checks: a batch's indexes, an
// update's position
typedef enum {
	EXT_RENDER_SETCOLOR,	// color: red, green, blue and alpha, or NULL for white
	EXT_RENDER_STRETCHPIC,	// x, y, w, h, s1, t1, s2, t2, shader, and clip, the module's clip region or NULL
	EXT_RENDER_CLEARSCENE,	// none
	EXT_RENDER_REFENTITY,	// entity
	EXT_RENDER_POLYS,	// shader, verts, numVerts each of numPolys
	EXT_RENDER_LIGHT,	// origin, intensity, r, g, b, additive
	EXT_RENDER_SCENE,	// refdef
	EXT_RENDER_TRIANGLES,	// tris, a batch (trap_R_DrawTriangles2D_OF), already scissored by clip, the module's clip region or NULL
	EXT_RENDER_CLIP_REGION,	// clip, the module's new clip region or NULL (trap_R_SetClipRegion_OF), seen and never taken
	EXT_RENDER_UPDATE_IMAGE	// image, area and pixels (trap_R_UpdateImage2D_OF)
} extRenderKind_t;

typedef struct extRenderCall_s {
	extRenderKind_t		kind;
	const float		*color;
	float			x, y, w, h, s1, t1, s2, t2;
	qhandle_t		shader;
	const refEntity_t	*entity;
	const polyVert_t	*verts;
	int			numVerts, numPolys;
	const float		*origin;
	float			intensity, r, g, b;
	qboolean		additive;
	const refdef_t		*refdef;
	const tris2D_t		*tris;
	const int		*clip;		// x, y, width and height
	qhandle_t		image;
	int			area[4];	// x, y, width and height
	const byte		*pixels;
} extRenderCall_t;

#endif
