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
// tr_2d.h -- what both renderers' DrawTriangles2D share, apart from GL so
// that it can be tested alone
#ifndef TR_2D_H
#define TR_2D_H

#include "tr_types.h"

qboolean R_CheckTris2D( const tris2D_t *tris, int maxBytes, char *error, int errorSize );
qboolean R_ClipScissor2D( const int *scissor, int width, int height, int *out );
void R_CopyTris2D( const tris2D_t *tris, polyVert2D_t *verts, int *indexes );

#endif
