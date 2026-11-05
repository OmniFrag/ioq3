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
// pak_dir.c -- a pak's zip directory read into its table of files: the
// names, their positions, lengths and CRCs, a hash table by name, and the
// pak's checksums. FS_LoadZipFile builds a pak around it; it uses nothing
// of the file system, so the fuzz harness fuzz_pk3 reads a pak the same way

#include "q_shared.h"
#include "qcommon.h"
#include "pak_dir.h"

#define MAX_FILEHASH_SIZE	1024

/*
================
FS_HashFileName

return a hash value for the filename
================
*/
long FS_HashFileName( const char *fname, int hashSize ) {
	int		i;
	long	hash;
	char	letter;

	hash = 0;
	i = 0;
	while (fname[i] != '\0') {
		letter = tolower(fname[i]);
		if (letter =='.') break;				// don't include extension
		if (letter =='\\') letter = '/';		// damn path names
		if (letter == PATH_SEP) letter = '/';		// damn path names
		hash+=(long)(letter)*(i+119);
		i++;
	}
	hash = (hash ^ (hash >> 10) ^ (hash >> 20));
	hash &= (hashSize-1);
	return hash;
}

/*
=================
PakDir_Count

How many of the directory's claimed entries can be read, and how many bytes
their names take, each terminated. A record that can't be read ends the
list: the entries after it have no name
=================
*/
int PakDir_Count( unzFile uf, uLong claimed, int *nameBytes )
{
	char			filename_inzip[MAX_ZPATH];
	unz_file_info	file_info;
	int				i, len;

	len = 0;
	unzGoToFirstFile(uf);
	for (i = 0; i < claimed; i++)
	{
		if (unzGetCurrentFileInfo(uf, &file_info, filename_inzip, sizeof(filename_inzip), NULL, 0, NULL, 0) != UNZ_OK) {
			break;
		}
		// unzip leaves a name that fills the buffer unterminated
		filename_inzip[sizeof(filename_inzip) - 1] = '\0';
		len += strlen(filename_inzip) + 1;
		unzGoToNextFile(uf);
	}
	*nameBytes = len;
	return i;
}

/*
=================
PakDir_HashSize

The hash table's size for a pak's files: lots of custom pk3 files have
less than 32 or 64 files
=================
*/
int PakDir_HashSize( int numfiles )
{
	int		i;

	for (i = 1; i <= MAX_FILEHASH_SIZE; i <<= 1) {
		if (i > numfiles) {
			break;
		}
	}
	return i;
}

/*
=================
PakDir_Read

The first numfiles entries (PakDir_Count's, and nameBytes its names'
length) into *buildBuffer, one Z_Malloc block of the entries and then their
names, lowercased, and into the hash table of hashSize slots
(PakDir_HashSize( numfiles )), cleared; and the pak's checksums, of its
files' CRCs, and with the checksum feed first for pure servers. Returns the
entries read, numfiles unless the file changed since PakDir_Count
=================
*/
int PakDir_Read( unzFile uf, int numfiles, int nameBytes, int checksumFeed, fileInPack_t **hashTable,
	int hashSize, fileInPack_t **buildBuffer, unsigned int *checksum, int *pureChecksum )
{
	fileInPack_t	*files;
	char			filename_inzip[MAX_ZPATH];
	unz_file_info	file_info;
	int				i, len;
	long			hash;
	int				fs_numHeaderLongs;
	int				*fs_headerLongs;
	char			*namePtr, *namesEnd;

	files = Z_Malloc( (numfiles * sizeof( fileInPack_t )) + nameBytes );
	namePtr = (char *)( files + numfiles );
	namesEnd = namePtr + nameBytes;
	fs_numHeaderLongs = 0;
	fs_headerLongs = Z_Malloc( ( numfiles + 1 ) * sizeof(int) );
	fs_headerLongs[ fs_numHeaderLongs++ ] = LittleLong( checksumFeed );

	unzGoToFirstFile(uf);
	for (i = 0; i < numfiles; i++)
	{
		if (unzGetCurrentFileInfo(uf, &file_info, filename_inzip, sizeof(filename_inzip), NULL, 0, NULL, 0) != UNZ_OK) {
			break;
		}
		// unzip leaves a name that fills the buffer unterminated
		filename_inzip[sizeof(filename_inzip) - 1] = '\0';
		len = strlen(filename_inzip) + 1;
		// a name longer than PakDir_Count's, if the file changed between
		if (len > namesEnd - namePtr) {
			break;
		}
		if (file_info.uncompressed_size > 0) {
			fs_headerLongs[fs_numHeaderLongs++] = LittleLong(file_info.crc);
		}
		Q_strlwr( filename_inzip );
		hash = FS_HashFileName(filename_inzip, hashSize);
		files[i].name = namePtr;
		Com_Memcpy( namePtr, filename_inzip, len );
		namePtr += len;
		// store the file position in the zip
		files[i].pos = unzGetOffset(uf);
		files[i].len = file_info.uncompressed_size;
		files[i].crc = file_info.crc;
		files[i].next = hashTable[hash];
		hashTable[hash] = &files[i];
		unzGoToNextFile(uf);
	}

	*checksum = LittleLong( Com_BlockChecksum( &fs_headerLongs[ 1 ], sizeof(*fs_headerLongs) * ( fs_numHeaderLongs - 1 ) ) );
	*pureChecksum = LittleLong( Com_BlockChecksum( fs_headerLongs, sizeof(*fs_headerLongs) * fs_numHeaderLongs ) );
	Z_Free(fs_headerLongs);

	*buildBuffer = files;
	return i;
}
