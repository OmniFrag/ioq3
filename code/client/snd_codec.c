/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2005 Stuart Dalton (badcdev@gmail.com)

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

#include "client.h"
#include "snd_codec.h"

static snd_codec_t *codecs;
// where S_CodecLoad first found the last sound it loaded (S_CodecNoteSource)
static int loadedSource;

// the codecs, at most this many
#define MAX_CODECS	8

/*
=================
S_SourceHash

A file's name and where it's read from, as a cached sound's source: 0 for
none
=================
*/
static int S_SourceHash(const char *where, const char *name)
{
	unsigned	hash = 5381;
	const char	*c;

	if( !*where )
		return 0;
	for( c = where; *c; c++ )
		hash = hash * 33 + (unsigned char)*c;
	hash = hash * 33 + '|';
	for( c = name; *c; c++ )
		hash = hash * 33 + (unsigned char)*c;
	return (int)( hash | 1 );
}

/*
=================
S_CodecNames

The names a sound is tried by (COM_FormatNames) and each one's codec:
the name, then the name with each codec's extension
=================
*/
static int S_CodecNames(const char *filename, char (*names)[MAX_QPATH], snd_codec_t **byName)
{
	snd_codec_t	*codec, *byFormat[MAX_CODECS];
	const char	*exts[MAX_CODECS];
	int			formats[MAX_CODECS];
	int			numCodecs = 0, numNames, i;

	for( codec = codecs; codec && numCodecs < MAX_CODECS; codec = codec->next )
	{
		byFormat[numCodecs] = codec;
		exts[numCodecs++] = codec->ext;
	}
	numNames = COM_FormatNames( filename, exts, numCodecs, names, formats );
	for( i = 0; i < numNames; i++ )
		byName[i] = byFormat[formats[i]];
	return numNames;
}

/*
=================
S_CodecGetSound

Opens/loads a sound, tries codec based on the sound's file extension
then tries all supported codecs.
=================
*/
static void *S_CodecGetSound(const char *filename, snd_info_t *info)
{
	snd_codec_t	*codec, *byName[MAX_CODECS];
	char		names[MAX_CODECS][MAX_QPATH];
	int			tiers[MAX_CODECS];
	int			numNames, i;
	void		*rtn;

	numNames = S_CodecNames( filename, names, byName );

	// the first copy found, as S_CodecSource finds it, even when its codec
	// refuses it and another format loads
	loadedSource = 0;

	// the name, then its other formats: every format in the file system's
	// tier 0 before any in tier 1
	for( i = COM_NextByTier( FS_FileTier, names, numNames, tiers, -1 ); i >= 0;
			i = COM_NextByTier( FS_FileTier, names, numNames, tiers, i ) )
	{
		// Load
		codec = byName[i];
		if( info )
			rtn = codec->load(names[i], info);
		else
			rtn = codec->open(names[i]);

		if( !loadedSource )
			loadedSource = S_SourceHash( FS_LastFileSource( ), names[i] );

		if( rtn )
		{
			if( i > 0 && !Q_stricmp( names[0], filename ) )
			{
				Com_DPrintf(S_COLOR_YELLOW "WARNING: %s not present, using %s instead\n",
						filename, names[i] );
			}

			return rtn;
		}
	}

	Com_Printf(S_COLOR_YELLOW "WARNING: Failed to %s sound %s!\n", info ? "load" : "open", filename);

	return NULL;
}

/*
=================
S_CodecSource

Where S_CodecLoad would read a sound from now (S_SourceHash), 0 for none
=================
*/
static int S_CodecSource(const char *filename)
{
	snd_codec_t	*byName[MAX_CODECS];
	char		names[MAX_CODECS][MAX_QPATH];
	int			tiers[MAX_CODECS];
	int			numNames, i, hash;

	numNames = S_CodecNames( filename, names, byName );
	for( i = COM_NextByTier( FS_FileTier, names, numNames, tiers, -1 ); i >= 0;
			i = COM_NextByTier( FS_FileTier, names, numNames, tiers, i ) )
	{
		if( ( hash = S_SourceHash( FS_FileSource( names[i] ), names[i] ) ) != 0 )
			return hash;
	}
	return 0;
}

/*
=================
S_CodecNoteSource

Keeps where the last sound S_CodecLoad loaded came from, for its cache
=================
*/
void S_CodecNoteSource(snd_source_t *source)
{
	source->hash = loadedSource;
	source->ranking = FS_Ranking( );
}

/*
=================
S_CodecSourceMoved

Whether a cached sound must be loaded again: the paks' ranking for this
map moved its file since it was found, as a map's own pak replaces a
sound only on its map. Looks once a ranking
=================
*/
qboolean S_CodecSourceMoved(const char *filename, snd_source_t *source)
{
	if( source->ranking == FS_Ranking( ) )
		return qfalse;
	source->ranking = FS_Ranking( );
	if( S_CodecSource( filename ) == source->hash )
		return qfalse;
	Com_DPrintf( "Sound %s comes from another pak on this map: loading it again\n", filename );
	return qtrue;
}

/*
=================
S_CodecInit
=================
*/
void S_CodecInit(void)
{
	codecs = NULL;

#ifdef USE_CODEC_OPUS
	S_CodecRegister(&opus_codec);
#endif

#ifdef USE_CODEC_VORBIS
	S_CodecRegister(&ogg_codec);
#endif

// Register wav codec last so that it is always tried first when a file extension was not found
	S_CodecRegister(&wav_codec);
}

/*
=================
S_CodecShutdown
=================
*/
void S_CodecShutdown(void)
{
	codecs = NULL;
}

/*
=================
S_CodecRegister
=================
*/
void S_CodecRegister(snd_codec_t *codec)
{
	codec->next = codecs;
	codecs = codec;
}

/*
=================
S_CodecLoad
=================
*/
void *S_CodecLoad(const char *filename, snd_info_t *info)
{
	return S_CodecGetSound(filename, info);
}

/*
=================
S_CodecOpenStream
=================
*/
snd_stream_t *S_CodecOpenStream(const char *filename)
{
	return S_CodecGetSound(filename, NULL);
}

void S_CodecCloseStream(snd_stream_t *stream)
{
	stream->codec->close(stream);
}

int S_CodecReadStream(snd_stream_t *stream, int bytes, void *buffer)
{
	return stream->codec->read(stream, bytes, buffer);
}

//=======================================================================
// Util functions (used by codecs)

/*
=================
S_CodecUtilOpen
=================
*/
snd_stream_t *S_CodecUtilOpen(const char *filename, snd_codec_t *codec)
{
	snd_stream_t *stream;
	fileHandle_t hnd;
	int length;

	// Try to open the file
	length = FS_FOpenFileRead(filename, &hnd, qtrue);
	if(!hnd)
	{
		Com_DPrintf("Can't read sound file %s\n", filename);
		return NULL;
	}

	// Allocate a stream
	stream = Z_Malloc(sizeof(snd_stream_t));
	if(!stream)
	{
		FS_FCloseFile(hnd);
		return NULL;
	}

	// Copy over, return
	stream->codec = codec;
	stream->file = hnd;
	stream->length = length;
	return stream;
}

/*
=================
S_CodecUtilClose
=================
*/
void S_CodecUtilClose(snd_stream_t **stream)
{
	FS_FCloseFile((*stream)->file);
	Z_Free(*stream);
	*stream = NULL;
}
