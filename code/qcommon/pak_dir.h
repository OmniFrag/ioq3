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
// pak_dir.h -- a pak's zip directory read into its table of files, with
// nothing of the file system's around it, so the fuzz harness reads it as
// FS_LoadZipFile does

#ifndef PAK_DIR_H
#define PAK_DIR_H

#include "q_shared.h"
#include "unzip.h"

#define MAX_ZPATH			256

typedef struct fileInPack_s {
	char					*name;		// name of the file
	unsigned long			pos;		// file info position in zip
	unsigned long			len;		// uncompress file size
	unsigned long			crc;		// its contents' CRC-32 (pakconflicts)
	struct	fileInPack_s*	next;		// next file in the hash
} fileInPack_t;

long	FS_HashFileName( const char *fname, int hashSize );

int		PakDir_Count( unzFile uf, uLong claimed, int *nameBytes );
int		PakDir_HashSize( int numfiles );
int		PakDir_Read( unzFile uf, int numfiles, int nameBytes, int checksumFeed, fileInPack_t **hashTable,
			int hashSize, fileInPack_t **buildBuffer, unsigned int *checksum, int *pureChecksum );

#endif
