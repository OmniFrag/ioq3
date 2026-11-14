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
// fs_picture.c -- whether a map's picture looks like one, by its size and
// its first bytes alone (files.c's FS_MapPicture and FS_WriteMapPicture), in
// a file of its own, which tests/fuzz's fuzz_picture builds too

#include "q_shared.h"
#include "qcommon.h"

/*
================
FS_PictureSizeAllowed, FS_PictureHead

Whether a picture of a size may be served: 1 byte to FS_MAP_PICTURE_MAX.
And how many of its first bytes FS_PictureLooksRight reads, FS_PICTURE_HEAD
at most: all of a smaller one
================
*/
qboolean FS_PictureSizeAllowed( int64_t size ) {
	return size >= 1 && size <= FS_MAP_PICTURE_MAX;
}

int FS_PictureHead( int size ) {
	return size < FS_PICTURE_HEAD ? size : FS_PICTURE_HEAD;
}

/*
================
FS_PictureLooksRight

Whether a picture's first bytes are its format's, never decoding it: a
JPEG's or PNG's signature, a BMP's "BM", or a TGA's header of an
uncompressed or run-length true color image of 24 or 32 bits a pixel, or
gray of 8, without a color map, 1 to 4096 pixels a side, as the page that
draws it takes
================
*/
qboolean FS_PictureLooksRight( const byte *data, int length, const char *type ) {
	static const byte	png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	int					width, height;

	if ( !strcmp( type, "image/jpeg" ) ) {
		return length >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
	}
	if ( !strcmp( type, "image/png" ) ) {
		return length >= (int)sizeof( png ) && !memcmp( data, png, sizeof( png ) );
	}
	if ( !strcmp( type, "image/bmp" ) ) {
		return length >= 2 && data[0] == 'B' && data[1] == 'M';
	}
	if ( length < FS_PICTURE_HEAD || data[1] != 0 ) {
		return qfalse;
	}
	if ( data[2] == 2 || data[2] == 10 ) {
		if ( data[16] != 24 && data[16] != 32 ) {
			return qfalse;
		}
	} else if ( ( data[2] != 3 && data[2] != 11 ) || data[16] != 8 ) {
		return qfalse;
	}
	width = data[12] | data[13] << 8;
	height = data[14] | data[15] << 8;
	return width >= 1 && width <= 4096 && height >= 1 && height <= 4096;
}
