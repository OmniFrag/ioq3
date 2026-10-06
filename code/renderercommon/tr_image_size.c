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
// tr_image_size.c -- the largest image the image loaders decode

#include "tr_common.h"

/*
=================
R_CheckImageSize

Whether a loader may decode an image of width by height pixels into bytes
of memory, its picture and any buffers it decodes through, before it
allocates them. Warns when it may not: the image is then refused, and the
default image stands in for it, rather than the allocation dropping the
client. A side is at most MAX_IMAGE_SIDE (as in CNQ3, 0ee2a50), and the
memory at most MAX_IMAGE_BYTES.
=================
*/
qboolean R_CheckImageSize( const char *name, int width, int height, int64_t bytes ) {
	if ( width < 1 || height < 1 || width > MAX_IMAGE_SIDE || height > MAX_IMAGE_SIDE ) {
		ri.Printf( PRINT_WARNING, "WARNING: %s is %dx%d, larger than %d a side or empty\n",
			name, width, height, MAX_IMAGE_SIDE );
		return qfalse;
	}
	if ( bytes > MAX_IMAGE_BYTES ) {
		ri.Printf( PRINT_WARNING, "WARNING: %s takes %lld bytes to load, more than %d\n",
			name, (long long)bytes, MAX_IMAGE_BYTES );
		return qfalse;
	}
	return qtrue;
}
