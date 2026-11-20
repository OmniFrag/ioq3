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
// bsp_align.c -- a map whose lumps don't all start at a multiple of 4, laid
// out again so they do. The collision map and the renderers read lumps in
// place, as ints and floats, and q3map pads every lump, but other compilers
// and tools have written maps that don't.

#include "q_shared.h"
#include "qcommon.h"

/*
=================
BSP_AlignLumps

When a lump of header, swapped and checked to lie inside file, doesn't
start at a multiple of 4, copies every lump into memory from alloc, each
at the next multiple of 4, points header at the copies and returns the
copy, for the caller to free and read from in place of file. NULL when
every lump is aligned already, and the file is read where it is. name is
the map's, for the error when the copy would be larger than an int
=================
*/
void *BSP_AlignLumps( const char *name, dheader_t *header, const void *file, void *( *alloc )( int size ) ) {
	dheader_t	in = *header;
	int			i, at;
	size_t		size = 0;
	byte		*aligned;

	for ( i = 0; i < HEADER_LUMPS; i++ ) {
		if ( header->lumps[i].fileofs & 3 ) {
			break;
		}
	}
	if ( i == HEADER_LUMPS ) {
		return NULL;
	}

	// lumps may overlap, so their lengths can add up past the file's
	for ( i = 0; i < HEADER_LUMPS; i++ ) {
		size += PAD( (size_t)header->lumps[i].filelen, 4 );
		if ( size > INT_MAX ) {
			Com_Error( ERR_DROP, "BSP_AlignLumps: %s has lumps too large to align", name );
		}
	}

	// header may be the start of file, which a lump may overlap, so it's
	// pointed at the copies only once every lump is copied as it was
	aligned = alloc( (int)size );
	for ( i = 0, at = 0; i < HEADER_LUMPS; i++ ) {
		Com_Memcpy( aligned + at, (const byte *)file + in.lumps[i].fileofs, in.lumps[i].filelen );
		in.lumps[i].fileofs = at;
		at += PAD( in.lumps[i].filelen, 4 );
	}
	*header = in;
	return aligned;
}
