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
// tr_ui2d_common.c -- the UI's retained 2D, the same in both renderers
// (tr_public.h's CreateTexture2D to DeletePaint2D): the textures, geometry
// and paints by handle, checked as they're made; the commands that draw and
// delete them in order among the frame's; and the clip masks' stencil. Each
// renderer's tr_ui2d.c does the rest of the GL work (r_renderer2D) and
// draws the commands.

#include "tr_common.h"
#include "tr_2d.h"

#include <stdlib.h>

// the renderer's GL 1.1 functions, for the stencil
#define GLE( ret, name, ... ) extern name##proc * qgl##name;
QGL_1_1_PROCS;
#undef GLE

// Handles are a slot's number from 1. A slot that's deleted is kept until
// its delete command runs, so the commands queued before it still find it;
// every slot starts with its deleted flag
typedef struct {
	qboolean	deleted;
} slot2D_t;

typedef struct {
	qboolean	deleted;
	paint2D_t	paint;
} paintSlot_t;

typedef struct {
	qboolean	deleted;
	filter2D_t	filter;
} filterSlot_t;

static void	*textures[MAX_TEXTURES2D], *geometries[MAX_GEOMETRIES2D], *paints[MAX_PAINTS2D], *filterSlots[MAX_FILTERS2D];

static struct {
	void		**slots;
	int			max;
	int			next;		// where the search for a free slot starts
	const char	*name;
} tables[HANDLE2D_KINDS] = {
	{ textures, MAX_TEXTURES2D, 0, "Texture2D" },
	{ geometries, MAX_GEOMETRIES2D, 0, "Geometry2D" },
	{ paints, MAX_PAINTS2D, 0, "Paint2D" },
	{ filterSlots, MAX_FILTERS2D, 0, "Filter2D" }
};

static qboolean	store2DOpen;	// between the renderer's start and its stop
static int		layerDepth;		// the layers the front end has pushed this frame
static int		geometryBytes;	// the vertexes and indexes kept, of MAX_GEOMETRY2D_BYTES

// The clip mask in the stencil. Each new mask writes a value above every
// one before it, so what older masks left never matches, and the stencil
// is cleared only when the values run low (or for an inverse mask); an
// intersection counts up from the mask it narrows. The draws with
// TRIS2D_CLIPMASK test for clipValue while clipOn.
static qboolean	clipOn;
static int		clipValue;

// the intersections a new mask leaves room for, below the stencil's 0xff
#define CLIPMASK_NESTING	32

/*
===============
R_GetSlot2D

A slot by its handle, NULL when there's none; one that's deleted only with
deleted, for the commands queued before its delete
===============
*/
static void *R_GetSlot2D( handle2DKind_t kind, int handle, qboolean deleted ) {
	slot2D_t	*slot;

	if ( handle < 1 || handle > tables[kind].max ) {
		return NULL;
	}
	slot = tables[kind].slots[handle - 1];
	return slot && ( !slot->deleted || deleted ) ? slot : NULL;
}

texture2D_t *R_GetTexture2D( int texture, qboolean deleted ) {
	return R_GetSlot2D( HANDLE2D_TEXTURE, texture, deleted );
}

geometry2D_t *R_GetGeometry2D( int geometry, qboolean deleted ) {
	return R_GetSlot2D( HANDLE2D_GEOMETRY, geometry, deleted );
}

const paint2D_t *R_GetPaint2D( int paint, qboolean deleted ) {
	paintSlot_t	*slot = R_GetSlot2D( HANDLE2D_PAINT, paint, deleted );

	return slot ? &slot->paint : NULL;
}

/*
===============
R_AddSlot2D

Gives object a free slot, searched for from after the last one given, and
returns its handle; 0, and object freed, when the table's full
===============
*/
static int R_AddSlot2D( handle2DKind_t kind, void *object ) {
	int		i, slot;

	for ( i = 0; i < tables[kind].max; i++ ) {
		slot = ( tables[kind].next + i ) % tables[kind].max;
		if ( !tables[kind].slots[slot] ) {
			tables[kind].slots[slot] = object;
			tables[kind].next = slot + 1;
			return slot + 1;
		}
	}
	ri.Printf( PRINT_DEVELOPER, "Create%s: all %i handles are taken\n", tables[kind].name, tables[kind].max );
	free( object );
	return 0;
}

/*
===============
R_FreeSlot2D

Frees a slot's object, and with freeGpu, what GL has of it
===============
*/
static void R_FreeSlot2D( handle2DKind_t kind, int handle, qboolean freeGpu ) {
	void	*object = tables[kind].slots[handle - 1];

	if ( freeGpu && ( kind == HANDLE2D_TEXTURE || kind == HANDLE2D_GEOMETRY ) ) {
		r_renderer2D.freeGpu( kind, object );
	}
	if ( kind == HANDLE2D_GEOMETRY ) {
		geometry2D_t	*g = object;

		geometryBytes -= g->numVerts * sizeof( *g->verts ) + g->numIndexes * sizeof( *g->indexes );
		free( g->straightVerts );
		// the renderer's record of its buffers, left when they went with
		// the window (freeGpu clears it)
		free( g->gpu );
	}
	free( object );
	tables[kind].slots[handle - 1] = NULL;
}

/*
===============
R_InitStore2D and R_ShutdownStore2D

The store opens as the renderer starts, and is emptied as it stops, from
GL too with freeGpu, when its context stays
===============
*/
void R_InitStore2D( void ) {
	store2DOpen = qtrue;
	layerDepth = 0;
	// the new stencil holds anything: the first mask clears it
	clipOn = qfalse;
	clipValue = 0xff;
}

void R_ShutdownStore2D( qboolean freeGpu ) {
	int		kind, i;

	for ( kind = 0; kind < HANDLE2D_KINDS; kind++ ) {
		for ( i = 0; i < tables[kind].max; i++ ) {
			if ( tables[kind].slots[i] ) {
				R_FreeSlot2D( kind, i + 1, freeGpu );
			}
		}
		tables[kind].next = 0;
	}
	store2DOpen = qfalse;
}

/*
===============
R_TexelBytes2D
===============
*/
int R_TexelBytes2D( tex2DFormat_t format ) {
	return format == TEX2D_A8 ? 1 : 4;
}

/*
===============
R_MakeTexture2D

A texture, from pixels, or transparent black; or with drawnInto and no
pixels, as GL makes it, for the back end to draw all of
===============
*/
static int R_MakeTexture2D( int width, int height, tex2DFormat_t format, const byte *pixels, qboolean drawnInto ) {
	texture2D_t	*t;
	int			handle;

	if ( !store2DOpen ) {
		return 0;
	}
	if ( (unsigned)format > TEX2D_A8 ) {
		ri.Printf( PRINT_WARNING, "CreateTexture2D: format %i\n", format );
		return 0;
	}
	if ( width < 1 || height < 1 || width > glConfig.maxTextureSize || height > glConfig.maxTextureSize ) {
		ri.Printf( PRINT_WARNING, "CreateTexture2D: %i by %i isn't a size this GL takes\n", width, height );
		return 0;
	}
	t = calloc( 1, sizeof( *t ) );
	if ( !t || !( handle = R_AddSlot2D( HANDLE2D_TEXTURE, t ) ) ) {
		return 0;
	}
	Com_sprintf( t->image.imgName, sizeof( t->image.imgName ), "*texture2D%i", handle );
	t->image.width = t->image.uploadWidth = width;
	t->image.height = t->image.uploadHeight = height;
	t->format = format;
	t->drawnInto = drawnInto;
	r_renderer2D.createTexture( t, pixels );
	return handle;
}

/*
===============
R_CreateTexture2D
===============
*/
int R_CreateTexture2D( int width, int height, tex2DFormat_t format, const byte *pixels ) {
	return R_MakeTexture2D( width, height, format, pixels, qfalse );
}

/*
===============
R_UpdateTexture2D

A small rectangle waits in the command buffer, in its place among the
draws. A large one draws what waits, and is uploaded at once.
===============
*/
void R_UpdateTexture2D( int texture, int x, int y, int width, int height, const byte *pixels ) {
	updateTexture2DCommand_t	*cmd;
	texture2D_t	*t = R_GetTexture2D( texture, qfalse );
	int			bytes;

	if ( !t || !pixels ) {
		ri.Printf( PRINT_WARNING, "UpdateTexture2D: no texture %i, or no pixels\n", texture );
		return;
	}
	if ( x < 0 || y < 0 || width < 1 || height < 1 || width > t->image.width - x || height > t->image.height - y ) {
		ri.Printf( PRINT_WARNING, "UpdateTexture2D: %i by %i at %i, %i is outside texture %i\n", width, height, x, y, texture );
		return;
	}

	bytes = width * height * R_TexelBytes2D( t->format );
	if ( bytes <= r_renderer2D.maxCommandBytes / 8 ) {
		cmd = R_GetCommandBuffer( sizeof( *cmd ) + bytes );
		if ( cmd ) {
			cmd->commandId = r_renderer2D.updateCommand;
			cmd->texture = texture;
			cmd->x = x;
			cmd->y = y;
			cmd->width = width;
			cmd->height = height;
			cmd->bytes = bytes;
			Com_Memcpy( cmd + 1, pixels, bytes );
			return;
		}
	}

	R_IssuePendingRenderCommands();
	r_renderer2D.updateTexture( t, x, y, width, height, pixels );
}

/*
===============
R_CheckTriangles2D

Whether indexes make triangles of numVerts vertexes
===============
*/
static qboolean R_CheckTriangles2D( const int *indexes, int numIndexes, int numVerts, const char *caller ) {
	int		i;

	if ( !indexes || numVerts < 1 || numIndexes < 3 || numIndexes % 3 ) {
		ri.Printf( PRINT_WARNING, "%s: %i vertexes and %i indexes aren't triangles\n", caller, numVerts, numIndexes );
		return qfalse;
	}
	for ( i = 0; i < numIndexes; i++ ) {
		if ( (unsigned)indexes[i] >= (unsigned)numVerts ) {
			ri.Printf( PRINT_WARNING, "%s: index %i is %i, of %i vertexes\n", caller, i, indexes[i], numVerts );
			return qfalse;
		}
	}
	return qtrue;
}

/*
===============
R_CreateGeometry2D

Copies the vertexes and indexes, once they're checked, into a new
geometry; 0, with a warning, when they're malformed or there's no room
===============
*/
int R_CreateGeometry2D( const polyVert2D_t *verts, int numVerts, const int *indexes, int numIndexes ) {
	geometry2D_t	*geometry;
	int				bytes, handle;

	if ( !store2DOpen ) {
		return 0;
	}
	if ( numVerts > MAX_GEOMETRY2D_VERTS || numIndexes > MAX_GEOMETRY2D_VERTS * 6 ) {
		ri.Printf( PRINT_WARNING, "CreateGeometry2D: %i vertexes and %i indexes are too many\n", numVerts, numIndexes );
		return 0;
	}
	if ( !verts || !R_CheckTriangles2D( indexes, numIndexes, numVerts, "CreateGeometry2D" ) ) {
		return 0;
	}

	bytes = numVerts * sizeof( *verts ) + numIndexes * sizeof( *indexes );
	if ( geometryBytes + bytes > MAX_GEOMETRY2D_BYTES ) {
		ri.Printf( PRINT_DEVELOPER, "CreateGeometry2D: no room for %i more bytes\n", bytes );
		return 0;
	}

	// apart from the zone, which the rest of the game shares
	geometry = calloc( 1, sizeof( *geometry ) + bytes );
	if ( !geometry ) {
		return 0;
	}
	geometry->verts = (polyVert2D_t *)( geometry + 1 );
	geometry->indexes = (int *)( geometry->verts + numVerts );
	geometry->numVerts = numVerts;
	geometry->numIndexes = numIndexes;
	Com_Memcpy( geometry->verts, verts, numVerts * sizeof( *verts ) );
	Com_Memcpy( geometry->indexes, indexes, numIndexes * sizeof( *indexes ) );

	handle = R_AddSlot2D( HANDLE2D_GEOMETRY, geometry );
	if ( handle ) {
		geometryBytes += bytes;
	}
	return handle;
}

/*
===============
R_StraightColors2D

Premultiplied colors as straight ones, for the shaders, which blend by
alpha
===============
*/
void R_StraightColors2D( polyVert2D_t *verts, int numVerts ) {
	int		i, j;

	for ( i = 0; i < numVerts; i++ ) {
		for ( j = 0; j < 3; j++ ) {
			verts[i].rgba[j] = R_StraightChannel2D( verts[i].rgba[j], verts[i].rgba[3] );
		}
	}
}

/*
===============
R_StraightVerts2D

A geometry's vertexes with straight colors, made the first time they're
asked for
===============
*/
const polyVert2D_t *R_StraightVerts2D( geometry2D_t *g ) {
	if ( !g->straightVerts ) {
		g->straightVerts = malloc( g->numVerts * sizeof( *g->verts ) );
		if ( !g->straightVerts ) {
			return g->verts;
		}
		Com_Memcpy( g->straightVerts, g->verts, g->numVerts * sizeof( *g->verts ) );
		R_StraightColors2D( g->straightVerts, g->numVerts );
	}
	return g->straightVerts;
}

/*
===============
R_CheckGradient2D and R_CheckShape2D

Whether a gradient's stops and points, or a shape's numbers, make sense
===============
*/
static qboolean R_CheckGradient2D( const paint2D_t *paint ) {
	int		i;

	if ( paint->numStops < 2 || paint->numStops > MAX_PAINT2D_STOPS ) {
		ri.Printf( PRINT_WARNING, "CreatePaint2D: %i stops, not 2 to %i\n", paint->numStops, MAX_PAINT2D_STOPS );
		return qfalse;
	}
	for ( i = 0; i < paint->numStops; i++ ) {
		if ( !isfinite( paint->stopPos[i] ) || ( i && paint->stopPos[i] < paint->stopPos[i - 1] ) ) {
			ri.Printf( PRINT_WARNING, "CreatePaint2D: stop %i is out of order\n", i );
			return qfalse;
		}
	}
	for ( i = 0; i < 2; i++ ) {
		if ( !isfinite( paint->p0[i] ) || !isfinite( paint->p1[i] ) ) {
			ri.Printf( PRINT_WARNING, "CreatePaint2D: a point isn't a number\n" );
			return qfalse;
		}
	}
	if ( paint->kind == PAINT2D_LINEAR && paint->p0[0] == paint->p1[0] && paint->p0[1] == paint->p1[1] ) {
		ri.Printf( PRINT_WARNING, "CreatePaint2D: a linear gradient from %g, %g to itself\n", paint->p0[0], paint->p0[1] );
		return qfalse;
	}
	if ( paint->kind == PAINT2D_RADIAL && ( paint->p1[0] <= 0 || paint->p1[1] <= 0 ) ) {
		ri.Printf( PRINT_WARNING, "CreatePaint2D: radii %g, %g\n", paint->p1[0], paint->p1[1] );
		return qfalse;
	}
	return qtrue;
}

static qboolean R_CheckShape2D( const paint2D_t *paint ) {
	int		i;

	if ( paint->kind == PAINT2D_MSDF ) {
		if ( !isfinite( paint->msdfRange ) || paint->msdfRange <= 0 ) {
			ri.Printf( PRINT_WARNING, "CreatePaint2D: a distance field's range of %g\n", paint->msdfRange );
			return qfalse;
		}
		for ( i = 0; i < 2; i++ ) {
			if ( !isfinite( paint->msdfSourceTexel[i] ) || paint->msdfSourceTexel[i] < 0 ||
				paint->msdfSourceTexel[i] > paint->msdfRange / 2 ||
				( paint->msdfSourceTexel[i] == 0 ) != ( paint->msdfSourceTexel[!i] == 0 ) ) {
				ri.Printf( PRINT_WARNING, "CreatePaint2D: a source texel of %g by %g, not both 0 or both up to half "
					"the range\n", paint->msdfSourceTexel[0], paint->msdfSourceTexel[1] );
				return qfalse;
			}
		}
		return qtrue;
	}
	for ( i = 0; i < 4; i++ ) {
		if ( !isfinite( paint->box[i] ) || !isfinite( paint->radii[i] ) || paint->radii[i] < 0 ) {
			ri.Printf( PRINT_WARNING, "CreatePaint2D: a rounded box's numbers\n" );
			return qfalse;
		}
	}
	if ( paint->box[2] <= 0 || paint->box[3] <= 0 || !isfinite( paint->feather ) || paint->feather < 0 ) {
		ri.Printf( PRINT_WARNING, "CreatePaint2D: a rounded box %g by %g, feather %g\n", paint->box[2], paint->box[3],
			paint->feather );
		return qfalse;
	}
	return qtrue;
}

/*
===============
R_CreatePaint2D

Keeps a paint once it's checked; 0 when it's malformed, there's no room,
or the renderer doesn't draw its kind (caps2D_t's paints and msdf)
===============
*/
int R_CreatePaint2D( const paint2D_t *paint ) {
	paintSlot_t	*slot;
	caps2D_t	caps;

	if ( !store2DOpen ) {
		return 0;
	}
	r_renderer2D.caps( &caps );
	if ( !paint || !caps.paints || (unsigned)paint->kind > PAINT2D_MSDF || ( paint->kind == PAINT2D_MSDF && !caps.msdf ) ) {
		ri.Printf( PRINT_DEVELOPER, "CreatePaint2D: kind %i isn't drawn\n", paint ? (int)paint->kind : -1 );
		return 0;
	}
	if ( paint->kind <= PAINT2D_CONIC ? !R_CheckGradient2D( paint ) : !R_CheckShape2D( paint ) ) {
		return 0;
	}

	slot = calloc( 1, sizeof( *slot ) );
	if ( !slot ) {
		return 0;
	}
	slot->paint = *paint;
	return R_AddSlot2D( HANDLE2D_PAINT, slot );
}

/*
===============
R_CheckState2D

Whether a 2D state can be drawn: its flags known, its scissor not empty,
and its texture and paint there. out, when given, gets it with the scissor
cut to the window, as RE_DrawTriangles2D cuts it; a composite's isn't cut,
as its filters reach past it (RB_CompositeBox2D clamps what they reach).
===============
*/
static qboolean R_CheckState2D( const draw2DState_t *state, draw2DState_t *out, const char *caller ) {
	if ( state->flags & ~TRIS2D_ALL_FLAGS ) {
		ri.Printf( PRINT_WARNING, "%s: flags 0x%x\n", caller, state->flags );
		return qfalse;
	}
	if ( out ) {
		*out = *state;
	}
	if ( ( state->flags & TRIS2D_SCISSOR ) && ( out ?
		!R_ClipScissor2D( state->scissor, glConfig.vidWidth, glConfig.vidHeight, out->scissor ) :
		state->scissor[2] <= 0 || state->scissor[3] <= 0 ) ) {
		return qfalse;
	}
	if ( ( state->flags & TRIS2D_TEXTURE2D ) && !R_GetTexture2D( state->texture, qfalse ) ) {
		ri.Printf( PRINT_WARNING, "%s: no texture %i\n", caller, state->texture );
		return qfalse;
	}
	if ( state->flags & TRIS2D_PAINT ) {
		const paint2D_t	*paint = R_GetPaint2D( state->paint, qfalse );

		if ( !paint ) {
			ri.Printf( PRINT_WARNING, "%s: no paint %i\n", caller, state->paint );
			return qfalse;
		}
		// a distance field is the texture's
		if ( paint->kind == PAINT2D_MSDF && !( state->flags & TRIS2D_TEXTURE2D ) ) {
			ri.Printf( PRINT_WARNING, "%s: a distance field without a texture\n", caller );
			return qfalse;
		}
	}
	return qtrue;
}

/*
===============
R_DrawGeometry2D
===============
*/
void R_DrawGeometry2D( int geometry, const draw2DState_t *state ) {
	draw2DCommand_t	*cmd;
	draw2DState_t	checked;

	if ( !R_GetGeometry2D( geometry, qfalse ) ) {
		ri.Printf( PRINT_WARNING, "DrawGeometry2D: no geometry %i\n", geometry );
		return;
	}
	if ( !R_CheckState2D( state, &checked, "DrawGeometry2D" ) ) {
		return;
	}
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = r_renderer2D.drawCommand;
	cmd->geometry = geometry;
	cmd->numVerts = cmd->numIndexes = 0;
	cmd->state = checked;
}

/*
===============
R_DrawTriangles2DState

An immediate batch with the retained path's flags (TRIS2D_RETAINED_FLAGS),
copied into the command buffer as a draw of its own: quads as indexed
triangles, and premultiplied colors for a shader straight
===============
*/
void R_DrawTriangles2DState( const tris2D_t *tris ) {
	static const int	quad[6] = { 3, 0, 2, 2, 0, 1 };	// the corners in the order RB_StretchPic gives them
	draw2DCommand_t		*cmd;
	draw2DState_t		checked;
	polyVert2D_t		*verts;
	int					*indexes;
	int					i, numIndexes;
	int64_t				bytes;
	char				error[128];

	// the immediate path's checks, then the handles'
	if ( !R_CheckTris2D( tris, TRIS2D_ALL_FLAGS, r_renderer2D.maxCommandBytes / 2, error, sizeof( error ) ) ) {
		if ( error[0] ) {
			ri.Printf( PRINT_WARNING, "DrawTriangles2D: %s\n", error );
		}
		return;
	}
	if ( !R_CheckState2D( &tris->state, &checked, "DrawTriangles2D" ) ) {
		return;
	}
	numIndexes = ( tris->state.flags & TRIS2D_QUADS ) ? tris->numVerts / 4 * 6 : tris->numIndexes;

	// no more than half the command buffer, which a frame's other
	// commands share, with a quad's six indexes
	bytes = sizeof( *cmd ) + (int64_t)tris->numVerts * sizeof( *verts ) + (int64_t)numIndexes * sizeof( *indexes );
	if ( bytes > r_renderer2D.maxCommandBytes / 2 ) {
		ri.Printf( PRINT_WARNING, "DrawTriangles2D: %i vertexes are too many\n", tris->numVerts );
		return;
	}

	cmd = R_GetCommandBuffer( bytes );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = r_renderer2D.drawCommand;
	cmd->geometry = 0;
	cmd->numVerts = tris->numVerts;
	cmd->numIndexes = numIndexes;
	cmd->state = checked;
	cmd->state.flags &= ~TRIS2D_QUADS;

	verts = (polyVert2D_t *)( cmd + 1 );
	indexes = (int *)( verts + tris->numVerts );
	Com_Memcpy( verts, tris->verts, tris->numVerts * sizeof( *verts ) );
	if ( ( tris->state.flags & TRIS2D_PREMULTIPLIED ) && R_ShaderState2D( &tris->state ) ) {
		R_StraightColors2D( verts, tris->numVerts );
	}

	if ( tris->state.flags & TRIS2D_QUADS ) {
		for ( i = 0; i < numIndexes; i++ ) {
			indexes[i] = i / 6 * 4 + quad[i % 6];
		}
	} else {
		Com_Memcpy( indexes, tris->indexes, numIndexes * sizeof( *indexes ) );
	}
}

/*
===============
R_SetClipMask2D

Without stencil (caps2D_t's clipMask), does nothing
===============
*/
void R_SetClipMask2D( clipMaskOp_t op, int geometry, const draw2DState_t *state ) {
	clipMask2DCommand_t	*cmd;
	draw2DState_t		checked = { 0 };
	caps2D_t			caps;

	if ( !store2DOpen ) {
		return;
	}
	r_renderer2D.caps( &caps );
	if ( !caps.clipMask ) {
		return;
	}
	if ( (unsigned)op > CLIPMASK_INTERSECT ) {
		ri.Printf( PRINT_WARNING, "SetClipMask2D: op %i\n", op );
		return;
	}
	if ( op != CLIPMASK_OFF ) {
		if ( !R_GetGeometry2D( geometry, qfalse ) ) {
			ri.Printf( PRINT_WARNING, "SetClipMask2D: no geometry %i\n", geometry );
			return;
		}
		if ( !R_CheckState2D( state, &checked, "SetClipMask2D" ) ) {
			return;
		}
	}
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = r_renderer2D.clipMaskCommand;
	cmd->op = op;
	cmd->geometry = op != CLIPMASK_OFF ? geometry : 0;
	cmd->state = checked;
}

/*
===============
R_Delete2D

Deletes a texture, geometry or paint after the commands queued before;
when the command buffer is full, after drawing them. A texture's mask
filters show nothing from then on, not whatever later takes its handle
===============
*/
static void R_Delete2D( handle2DKind_t kind, int handle ) {
	slot2D_t			*slot = R_GetSlot2D( kind, handle, qfalse );
	delete2DCommand_t	*cmd;
	int					i;

	if ( !slot ) {
		return;
	}
	slot->deleted = qtrue;
	for ( i = 0; kind == HANDLE2D_TEXTURE && i < MAX_FILTERS2D; i++ ) {
		filterSlot_t	*f = filterSlots[i];

		if ( f && f->filter.kind == FILTER2D_MASK && f->filter.texture == handle ) {
			f->filter.texture = 0;
		}
	}

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( cmd ) {
		cmd->commandId = r_renderer2D.deleteCommand;
		cmd->kind = kind;
		cmd->handle = handle;
		return;
	}
	R_IssuePendingRenderCommands();
	R_FreeSlot2D( kind, handle, qtrue );
}

void R_DeleteTexture2D( int texture ) {
	R_Delete2D( HANDLE2D_TEXTURE, texture );
}

void R_DeleteGeometry2D( int geometry ) {
	R_Delete2D( HANDLE2D_GEOMETRY, geometry );
}

void R_DeletePaint2D( int paint ) {
	R_Delete2D( HANDLE2D_PAINT, paint );
}

/*
===============
R_Layers2DReady

Whether the store's open and the renderer has layers, which the layer and
filter calls need
===============
*/
static qboolean R_Layers2DReady( void ) {
	caps2D_t	caps;

	if ( !store2DOpen ) {
		return qfalse;
	}
	r_renderer2D.caps( &caps );
	return caps.layers;
}

/*
===============
R_CreateFilter2D

A filter by its name, with as many numbers as it takes (tr_public.h)
===============
*/
int R_CreateFilter2D( const char *name, const float *params, int numParams ) {
	static const struct {
		const char		*name;
		filter2DKind_t	kind;
		int				numParams;
	} kinds[] = {
		{ "opacity", FILTER2D_OPACITY, 1 },
		{ "blur", FILTER2D_BLUR, 1 },
		{ "drop-shadow", FILTER2D_DROP_SHADOW, 7 },
		{ "color-matrix", FILTER2D_COLOR_MATRIX, 20 }
	};
	filterSlot_t	*slot;
	int				i, k;

	if ( !name || !R_Layers2DReady() ) {
		return 0;
	}
	for ( k = 0; k < (int)ARRAY_LEN( kinds ) && Q_stricmp( name, kinds[k].name ); k++ ) {
	}
	if ( k == (int)ARRAY_LEN( kinds ) ) {
		ri.Printf( PRINT_WARNING, "CreateFilter2D: no filter %s\n", name );
		return 0;
	}
	if ( numParams != kinds[k].numParams || !params ) {
		ri.Printf( PRINT_WARNING, "CreateFilter2D: %s takes %i numbers, not %i\n", name, kinds[k].numParams, numParams );
		return 0;
	}
	for ( i = 0; i < numParams; i++ ) {
		if ( !isfinite( params[i] ) ) {
			ri.Printf( PRINT_WARNING, "CreateFilter2D: %s's number %i isn't one\n", name, i );
			return 0;
		}
	}
	if ( ( kinds[k].kind == FILTER2D_BLUR && params[0] < 0 ) || ( kinds[k].kind == FILTER2D_DROP_SHADOW && params[6] < 0 ) ) {
		ri.Printf( PRINT_WARNING, "CreateFilter2D: %s's sigma is below 0\n", name );
		return 0;
	}

	slot = calloc( 1, sizeof( *slot ) );
	if ( !slot ) {
		return 0;
	}
	slot->filter.kind = kinds[k].kind;
	Com_Memcpy( slot->filter.params, params, numParams * sizeof( *params ) );
	// an alpha, or a shadow's premultiplied color, from 0 to 1, as CSS
	// clamps it, so the colors composited stay premultiplied
	if ( slot->filter.kind == FILTER2D_OPACITY || slot->filter.kind == FILTER2D_DROP_SHADOW ) {
		for ( i = 0; i < ( slot->filter.kind == FILTER2D_OPACITY ? 1 : 4 ); i++ ) {
			slot->filter.params[i] = Com_Clamp( 0, 1, slot->filter.params[i] );
		}
	}
	return R_AddSlot2D( HANDLE2D_FILTER, slot );
}

void R_DeleteFilter2D( int filter ) {
	R_Delete2D( HANDLE2D_FILTER, filter );
}

/*
===============
R_QueueLayer2D
===============
*/
static layer2DCommand_t *R_QueueLayer2D( layer2DOp_t op ) {
	layer2DCommand_t	*cmd = R_GetCommandBuffer( sizeof( *cmd ) );

	if ( cmd ) {
		cmd->commandId = r_renderer2D.layerCommand;
		cmd->op = op;
		cmd->numFilters = 0;
	}
	return cmd;
}

/*
===============
R_PushLayer2D and R_PopLayer2D
===============
*/
int R_PushLayer2D( void ) {
	if ( !R_Layers2DReady() ) {
		return 0;
	}
	if ( layerDepth == MAX_LAYERS2D ) {
		ri.Printf( PRINT_WARNING, "PushLayer2D: %i layers are pushed already\n", MAX_LAYERS2D );
		return 0;
	}
	if ( !R_QueueLayer2D( LAYER2D_PUSH ) ) {
		return 0;
	}
	return ++layerDepth;
}

void R_PopLayer2D( void ) {
	if ( layerDepth && R_QueueLayer2D( LAYER2D_POP ) ) {
		layerDepth--;
	}
}

void R_EndFrame2D( void ) {
	layerDepth = 0;
}

/*
===============
R_CompositeLayers2D
===============
*/
void R_CompositeLayers2D( int source, int destination, int blendMode, const int *filters, int numFilters,
	const draw2DState_t *state ) {
	const filterSlot_t	*slots[MAX_FILTERS2D_PER_COMPOSITE];
	layer2DCommand_t	*cmd;
	int					i;

	if ( !layerDepth ) {
		return;
	}
	if ( source < 0 || source > layerDepth || destination < 0 || destination > layerDepth || source == destination ) {
		ri.Printf( PRINT_WARNING, "CompositeLayers2D: layers %i and %i, of %i\n", source, destination, layerDepth );
		return;
	}
	if ( (unsigned)blendMode > BLEND2D_REPLACE ) {
		ri.Printf( PRINT_WARNING, "CompositeLayers2D: blend mode %i\n", blendMode );
		return;
	}
	if ( numFilters < 0 || numFilters > MAX_FILTERS2D_PER_COMPOSITE || ( numFilters && !filters ) ) {
		ri.Printf( PRINT_WARNING, "CompositeLayers2D: %i filters\n", numFilters );
		return;
	}
	for ( i = 0; i < numFilters; i++ ) {
		slots[i] = R_GetSlot2D( HANDLE2D_FILTER, filters[i], qfalse );
		if ( !slots[i] ) {
			ri.Printf( PRINT_WARNING, "CompositeLayers2D: no filter %i\n", filters[i] );
			return;
		}
	}
	if ( state && !R_CheckState2D( state, NULL, "CompositeLayers2D" ) ) {
		return;
	}

	cmd = R_QueueLayer2D( LAYER2D_COMPOSITE );
	if ( !cmd ) {
		return;
	}
	cmd->source = source;
	cmd->destination = destination;
	cmd->blendMode = blendMode;
	cmd->numFilters = numFilters;
	for ( i = 0; i < numFilters; i++ ) {
		cmd->filters[i] = slots[i]->filter;
	}
	if ( state ) {
		cmd->state = *state;
	} else {
		Com_Memset( &cmd->state, 0, sizeof( cmd->state ) );
	}
}

/*
===============
R_SaveLayer2D

A new texture of box, filled from the top layer when the command's turn
comes in the back end (tr_public.h)
===============
*/
int R_SaveLayer2D( const int box[4] ) {
	layer2DCommand_t	*cmd;
	int					texture;

	if ( !box || !R_Layers2DReady() ) {
		return 0;
	}
	texture = R_MakeTexture2D( box[2], box[3], TEX2D_RGBA8_PREMUL, NULL, qtrue );
	if ( !texture ) {
		return 0;
	}
	cmd = R_QueueLayer2D( LAYER2D_SAVE );
	if ( !cmd ) {
		// no command has it yet
		R_FreeSlot2D( HANDLE2D_TEXTURE, texture, qtrue );
		return 0;
	}
	cmd->source = layerDepth;
	cmd->texture = texture;
	Com_Memcpy( cmd->box, box, sizeof( cmd->box ) );
	return texture;
}

/*
===============
R_CreateMaskFilter2D
===============
*/
int R_CreateMaskFilter2D( int texture, int x, int y ) {
	filterSlot_t	*slot;

	if ( !R_Layers2DReady() ) {
		return 0;
	}
	if ( !R_GetTexture2D( texture, qfalse ) ) {
		ri.Printf( PRINT_WARNING, "CreateMaskFilter2D: no texture %i\n", texture );
		return 0;
	}
	slot = calloc( 1, sizeof( *slot ) );
	if ( !slot ) {
		return 0;
	}
	slot->filter.kind = FILTER2D_MASK;
	slot->filter.texture = texture;
	slot->filter.params[0] = x;
	slot->filter.params[1] = y;
	return R_AddSlot2D( HANDLE2D_FILTER, slot );
}

/*
===============
R_Transform2DMatrix

A 2D state's translation and, with TRIS2D_TRANSFORM, its 3x3 transform, as
a column-major 4x4 matrix for GL, with w from the transform's last row,
so GL divides by it and interpolates the texture coordinates as the
transform's perspective asks
===============
*/
void R_Transform2DMatrix( const draw2DState_t *state, float out[16] ) {
	static const float	identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	const float	*m = ( state->flags & TRIS2D_TRANSFORM ) ? state->transform : identity;
	float		tx = state->translate[0], ty = state->translate[1];
	int			row;

	Com_Memset( out, 0, sizeof( float ) * 16 );
	for ( row = 0; row < 3; row++ ) {
		const float	*r = &m[row * 3];
		// x's and y's rows go to GL's, w's to its fourth; z stays 0
		int			glRow = row == 2 ? 3 : row;

		out[0 * 4 + glRow] = r[0];
		out[1 * 4 + glRow] = r[1];
		out[3 * 4 + glRow] = r[0] * tx + r[1] * ty + r[2];
	}
	out[2 * 4 + 2] = 1.0f;
}

/*
===============
R_Transform2DPoint

Moves a point as R_Transform2DMatrix's matrix does, for the tessellator,
which takes it divided by w
===============
*/
void R_Transform2DPoint( const float matrix[16], const float in[2], float out[2] ) {
	float	w = matrix[3] * in[0] + matrix[7] * in[1] + matrix[15];

	out[0] = ( matrix[0] * in[0] + matrix[4] * in[1] + matrix[12] ) / w;
	out[1] = ( matrix[1] * in[0] + matrix[5] * in[1] + matrix[13] ) / w;
}


/*
============================================================================

BACK END

============================================================================
*/

/*
===============
R_BeginClipTest2D and R_EndClipTest2D

Around a draw: whether it tests the clip mask, which it then does
===============
*/
qboolean R_BeginClipTest2D( const draw2DState_t *state ) {
	if ( !clipOn || !( state->flags & TRIS2D_CLIPMASK ) ) {
		return qfalse;
	}
	qglEnable( GL_STENCIL_TEST );
	qglStencilFunc( GL_EQUAL, clipValue, 0xff );
	qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	return qtrue;
}

void R_EndClipTest2D( void ) {
	qglDisable( GL_STENCIL_TEST );
}

// the whole stencil, whatever the scissor
static void R_ClearStencil2D( int value ) {
	qglDisable( GL_SCISSOR_TEST );
	qglClearStencil( value );
	qglClear( GL_STENCIL_BUFFER_BIT );
	// the renderer's other stencil clears (r_shadows 2) expect 0
	qglClearStencil( 0 );
	qglEnable( GL_SCISSOR_TEST );
}

/*
===============
RB_ClipMask2D

Draws a geometry into the stencil, unseen, as the op says
===============
*/
const void *RB_ClipMask2D( const void *data ) {
	const clipMask2DCommand_t	*cmd = (const clipMask2DCommand_t *)data;
	geometry2D_t	*g = R_GetGeometry2D( cmd->geometry, qtrue );

	if ( cmd->op == CLIPMASK_OFF || !g ) {
		clipOn = qfalse;
		return cmd + 1;
	}

	if ( cmd->op == CLIPMASK_INTERSECT && clipValue == 0xff ) {
		// as deep as the stencil counts: no further
		return cmd + 1;
	}

	r_renderer2D.begin( &cmd->state );
	qglEnable( GL_STENCIL_TEST );
	qglStencilMask( 0xff );
	if ( cmd->op == CLIPMASK_SET_INVERSE ) {
		// everything 1 but the geometry
		R_ClearStencil2D( 1 );
		qglStencilFunc( GL_ALWAYS, 0, 0xff );
		qglStencilOp( GL_KEEP, GL_KEEP, GL_REPLACE );
		clipValue = 1;
	} else {
		// a new mask leaves room to be narrowed
		if ( cmd->op == CLIPMASK_SET && clipValue + 1 + CLIPMASK_NESTING > 0xff ) {
			R_ClearStencil2D( 0 );
			clipValue = 0;
		}
		if ( cmd->op == CLIPMASK_INTERSECT ) {
			qglStencilFunc( GL_EQUAL, clipValue, 0xff );
			qglStencilOp( GL_KEEP, GL_KEEP, GL_INCR );
		} else {
			qglStencilFunc( GL_ALWAYS, clipValue + 1, 0xff );
			qglStencilOp( GL_KEEP, GL_KEEP, GL_REPLACE );
		}
		clipValue++;
	}
	r_renderer2D.drawUnseen( &cmd->state, g );
	qglDisable( GL_STENCIL_TEST );

	clipOn = qtrue;
	return cmd + 1;
}

/*
===============
RB_Delete2D
===============
*/
const void *RB_Delete2D( const void *data ) {
	const delete2DCommand_t	*cmd = (const delete2DCommand_t *)data;

	if ( R_GetSlot2D( cmd->kind, cmd->handle, qtrue ) ) {
		R_FreeSlot2D( cmd->kind, cmd->handle, qtrue );
	}
	return cmd + 1;
}

/*
===============
RB_UpdateTexture2D
===============
*/
const void *RB_UpdateTexture2D( const void *data ) {
	const updateTexture2DCommand_t	*cmd = (const updateTexture2DCommand_t *)data;
	texture2D_t	*t = R_GetTexture2D( cmd->texture, qtrue );
	const byte	*pixels = (const byte *)( cmd + 1 );

	if ( t ) {
		r_renderer2D.updateTexture( t, cmd->x, cmd->y, cmd->width, cmd->height, pixels );
	}
	return pixels + cmd->bytes;
}
