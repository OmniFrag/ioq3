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

#include "tr_common.h"

typedef struct
{
	char id[2];
	unsigned fileSize;
	unsigned reserved0;
	unsigned bitmapDataOffset;
	unsigned bitmapHeaderSize;
	unsigned width;
	unsigned height;
	unsigned short planes;
	unsigned short bitsPerPixel;
	unsigned compression;
	unsigned bitmapDataSize;
	unsigned hRes;
	unsigned vRes;
	unsigned colors;
	unsigned importantColors;
	unsigned char palette[256][4];
} BMPHeader_t;

// the header's fields, little-endian, at offsets that aren't multiples of
// their size
static int R_BMPLong( const byte *p )
{
	return (int)( p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (unsigned)p[3] << 24 ) );
}

static short R_BMPShort( const byte *p )
{
	return (short)( p[0] | ( p[1] << 8 ) );
}

void R_LoadBMP( const char *name, byte **pic, int *width, int *height )
{
	int		columns, rows;
	unsigned	numPixels;
	byte	*pixbuf;
	int		row, column;
	byte	*buf_p;
	byte	*end;
	union {
		byte *b;
		void *v;
	} buffer;
	unsigned	length;
	BMPHeader_t bmpHeader;
	byte		*bmpRGBA;
	const char	*error;

	*pic = NULL;

	if(width)
		*width = 0;

	if(height)
		*height = 0;

	//
	// load the file
	//
	length = ri.FS_ReadFile( ( char * ) name, &buffer.v);
	if (!buffer.b || length < 0) {
		return;
	}

	if (length < 54)
	{
		error = "header too short";
		goto fail;
	}

	buf_p = buffer.b;
	end = buffer.b + length;

	bmpHeader.id[0] = *buf_p++;
	bmpHeader.id[1] = *buf_p++;
	bmpHeader.fileSize = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.reserved0 = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.bitmapDataOffset = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.bitmapHeaderSize = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.width = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.height = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.planes = R_BMPShort( buf_p );
	buf_p += 2;
	bmpHeader.bitsPerPixel = R_BMPShort( buf_p );
	buf_p += 2;
	bmpHeader.compression = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.bitmapDataSize = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.hRes = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.vRes = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.colors = R_BMPLong( buf_p );
	buf_p += 4;
	bmpHeader.importantColors = R_BMPLong( buf_p );
	buf_p += 4;

	if ( bmpHeader.bitsPerPixel == 8 )
	{
		if (buf_p + sizeof(bmpHeader.palette) > end)
		{
			error = "header too short";
			goto fail;
		}

		Com_Memcpy( bmpHeader.palette, buf_p, sizeof( bmpHeader.palette ) );
	}

	if (bmpHeader.bitmapDataOffset > length)
	{
		error = "invalid offset value in header";
		goto fail;
	}

	buf_p = buffer.b + bmpHeader.bitmapDataOffset;

	if ( bmpHeader.id[0] != 'B' || bmpHeader.id[1] != 'M' ) 
	{
		error = "only Windows-style BMP files supported";
		goto fail;
	}
	if ( bmpHeader.fileSize != length )
	{
		error = "header size does not match file size";
		goto fail;
	}
	if ( bmpHeader.compression != 0 )
	{
		error = "only uncompressed BMP files supported";
		goto fail;
	}
	if ( bmpHeader.bitsPerPixel < 8 )
	{
		error = "monochrome and 4-bit BMP files not supported";
		goto fail;
	}

	switch ( bmpHeader.bitsPerPixel )
	{
		case 8:
		case 16:
		case 24:
		case 32:
			break;
		default:
			error = "illegal pixel size";
			goto fail;
	}

	columns = bmpHeader.width;
	rows = bmpHeader.height;
	if ( rows < 0 && rows != INT_MIN )
		rows = -rows;

	// 4*1FFFFFFF == 0x7FFFFFFC < 0x7FFFFFFF
	if(columns <= 0 || rows <= 0 || columns > 0x1FFFFFFF / rows)
	{
	  error = "invalid image size";
	  goto fail;
	}
	if(!R_CheckImageSize(name, columns, rows, (int64_t)columns * rows * 4))
	{
	  error = NULL;	// it warned
	  goto fail;
	}
	numPixels = columns * rows;
	if((uint64_t)numPixels * bmpHeader.bitsPerPixel / 8 > (uint64_t)(end - buf_p))
	{
	  error = "file truncated";
	  goto fail;
	}

	if ( width ) 
		*width = columns;
	if ( height )
		*height = rows;

	bmpRGBA = ri.Malloc( numPixels * 4 );
	*pic = bmpRGBA;


	for ( row = rows-1; row >= 0; row-- )
	{
		pixbuf = bmpRGBA + row*columns*4;

		for ( column = 0; column < columns; column++ )
		{
			unsigned char red, green, blue, alpha;
			int palIndex;
			unsigned short shortPixel;

			switch ( bmpHeader.bitsPerPixel )
			{
			case 8:
				palIndex = *buf_p++;
				*pixbuf++ = bmpHeader.palette[palIndex][2];
				*pixbuf++ = bmpHeader.palette[palIndex][1];
				*pixbuf++ = bmpHeader.palette[palIndex][0];
				*pixbuf++ = 0xff;
				break;
			case 16:
				shortPixel = buf_p[0] | ( buf_p[1] << 8 );
				buf_p += 2;
				*pixbuf++ = ( shortPixel & ( 31 << 10 ) ) >> 7;
				*pixbuf++ = ( shortPixel & ( 31 << 5 ) ) >> 2;
				*pixbuf++ = ( shortPixel & ( 31 ) ) << 3;
				*pixbuf++ = 0xff;
				break;

			case 24:
				blue = *buf_p++;
				green = *buf_p++;
				red = *buf_p++;
				*pixbuf++ = red;
				*pixbuf++ = green;
				*pixbuf++ = blue;
				*pixbuf++ = 255;
				break;
			case 32:
				blue = *buf_p++;
				green = *buf_p++;
				red = *buf_p++;
				alpha = *buf_p++;
				*pixbuf++ = red;
				*pixbuf++ = green;
				*pixbuf++ = blue;
				*pixbuf++ = alpha;
				break;
			}
		}
	}

	ri.FS_FreeFile( buffer.v );
	return;

fail:
	// the default image stands in for it
	if (error)
		ri.Printf( PRINT_WARNING, "WARNING: LoadBMP: %s (%s)\n", error, name );
	ri.FS_FreeFile( buffer.v );
}
