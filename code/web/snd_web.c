/*
===========================================================================
Copyright (C) 2026 OmniFrag contributors

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

/*
The base sound backend's output through Web Audio (USE_WEB_AUDIO).

snd_dma.c keeps deciding what plays and how loud: the channels a sound may
take or steal, the loops merged per sound and clamped, each channel's left
and right volume from its place. Instead of painting those into a DMA
buffer (snd_mix.c), each frame this sends the page what changed: a sound's
samples are an AudioBuffer the page keeps, outside the heap, each playing
channel is a source node into its two gains, left and right, and the
browser mixes them on its audio thread. A sound starts at the next render
quantum, without a buffer of ours before it, and keeps playing through a
frame that stalls. The frame's new sounds go when the listener is placed,
before the frame is drawn, and again with the frame's sound update.

The page (Module.sound in client.html.in) makes the AudioContext in the
player's first input, as browsers allow. Until it runs, and while the
browser holds it, nothing starts; when it does, each sound still playing in
the game's time starts where it has reached, and each loop at its place in
the loops' shared cycle, so the player hears the present, not what was held.

Time is the game's, from Com_Milliseconds through S_GetSoundtime, so it
moves on while the browser holds the sound.
*/

#include <emscripten.h>

#include "../client/client.h"
#include "../client/snd_local.h"
#include "../client/snd_codec.h"

extern sfx_t s_knownSfx[];

// the strips the page keeps for channels: s_channels' first, then the loops'
#define LOOP_STRIP( i ) ( MAX_CHANNELS + ( i ) )
#define NUM_STRIPS ( MAX_CHANNELS * 2 )

// what changed this frame, as the page reads them: an operation, its strip,
// and up to four values
typedef enum {
	WEB_START,		// sfx, offset in seconds, loop, rate
	WEB_STOP,
	WEB_GAINS,		// left, right: 0 to 255, as the base mixer's volumes
	WEB_RATE,		// a loop's, for Doppler
	WEB_MASTER		// gain
} webOp_t;

typedef struct {
	float	op, strip, a, b, c, d;
} webCommand_t;

typedef struct {
	sfx_t	*sfx;		// what it plays, or NULL
	int		key;		// a channel's start, or which loop of its sound
	int		left, right;
	float	rate;
} webStrip_t;

static soundInterface_t	base;
static webCommand_t		commands[NUM_STRIPS * 3 + 1];
static int				numCommands;
static webStrip_t		strips[NUM_STRIPS];
static qboolean			running;
static float			masterGain;		// as last sent, or -1
static qboolean			clockStarted;
static int				startTime;
static int				rawRemainder[MAX_RAW_STREAMS];	// s_rawend's fractions, times the stream's rate

static void S_WebCommand( webOp_t op, int strip, float a, float b, float c, float d )
{
	webCommand_t *command;

	if ( numCommands == ARRAY_LEN( commands ) ) {
		return;
	}
	command = &commands[numCommands++];
	command->op = op;
	command->strip = strip;
	command->a = a;
	command->b = b;
	command->c = c;
	command->d = d;
}

/*
==============
S_WebStopAll

Everything playing stops at once, and nothing is left to stop
==============
*/
static void S_WebStopAll( void )
{
	Com_Memset( strips, 0, sizeof( strips ) );
	numCommands = 0;
	masterGain = -1;
	MAIN_THREAD_EM_ASM( { Module.sound.stopAll(); } );
}

static void S_WebClear( int strip )
{
	if ( strips[strip].sfx ) {
		S_WebCommand( WEB_STOP, strip, 0, 0, 0, 0 );
		strips[strip].sfx = NULL;
	}
}

/*
==============
S_WebSet

A strip to play sfx from offset, at its gains and rate: started again if
it plays something else, or nothing, and its gains or rate sent when they
changed
==============
*/
static void S_WebSet( int strip, sfx_t *sfx, int key, float offset, qboolean loop, int left, int right, float rate )
{
	webStrip_t *s = &strips[strip];

	if ( s->sfx != sfx || s->key != key ) {
		S_WebClear( strip );
		s->sfx = sfx;
		s->key = key;
		s->left = s->right = -1;
		s->rate = rate;
		S_WebCommand( WEB_START, strip, sfx - s_knownSfx, offset, loop, rate );
	} else if ( s->rate != rate ) {
		s->rate = rate;
		S_WebCommand( WEB_RATE, strip, rate, 0, 0, 0 );
	}

	if ( s->left != left || s->right != right ) {
		s->left = left;
		s->right = right;
		S_WebCommand( WEB_GAINS, strip, left, right, 0, 0 );
	}
}

/*
==============
S_WebUpdateChannels

Each channel's sound from where the game's time has it, as snd_mix.c
paints it. S_ScanChannelStarts has freed those that ended.
==============
*/
static void S_WebUpdateChannels( void )
{
	int			i;
	channel_t	*ch;

	for ( i = 0, ch = s_channels; i < MAX_CHANNELS; i++, ch++ ) {
		if ( !ch->thesfx || !ch->thesfx->soundLength ) {
			S_WebClear( i );
			continue;
		}
		S_WebSet( i, ch->thesfx, ch->startSample, (float)( s_soundtime - ch->startSample ) / dma.speed,
			qfalse, ch->leftvol, ch->rightvol, 1.0f );
	}
}

/*
==============
S_WebUpdateLoops

The loops S_AddLoopSounds merged this frame. A loop keeps its strip while
it plays: merged loops are one per sound, and those with Doppler, which
aren't merged, are told apart by their entity. The loops that stopped free
their strips before new ones take them, so there's always one. Each starts
at its place in the loops' shared cycle, as snd_mix.c paints them.
==============
*/
static void S_WebUpdateLoops( void )
{
	int			i, j, free;
	int			loopKeys[MAX_CHANNELS], kept[MAX_CHANNELS];
	channel_t	*ch;
	sfx_t		*sfx;
	qboolean	used[MAX_CHANNELS] = { qfalse };

	// the loops still playing keep their strips
	for ( i = 0, ch = loop_channels; i < numLoopChannels; i++, ch++ ) {
		kept[i] = -1;
		loopKeys[i] = ch->doppler ? ch->entnum + 1 : 0;
		sfx = ch->thesfx;
		if ( !sfx || !sfx->soundLength ) {
			continue;
		}
		for ( j = 0; j < MAX_CHANNELS; j++ ) {
			const webStrip_t *s = &strips[LOOP_STRIP( j )];

			if ( !used[j] && s->sfx == sfx && s->key == loopKeys[i] ) {
				kept[i] = j;
				used[j] = qtrue;
				break;
			}
		}
	}

	// the rest stop
	for ( j = 0; j < MAX_CHANNELS; j++ ) {
		if ( !used[j] ) {
			S_WebClear( LOOP_STRIP( j ) );
		}
	}

	// and the new loops take their strips
	free = 0;
	for ( i = 0, ch = loop_channels; i < numLoopChannels; i++, ch++ ) {
		sfx = ch->thesfx;
		if ( !sfx || !sfx->soundLength ) {
			continue;
		}
		j = kept[i];
		if ( j < 0 ) {
			while ( used[free] ) {
				free++;
			}
			j = free;
			used[j] = qtrue;
		}

		S_WebSet( LOOP_STRIP( j ), sfx, loopKeys[i], (float)( s_soundtime % sfx->soundLength ) / dma.speed, qtrue,
			ch->leftvol, ch->rightvol, ch->doppler ? ch->dopplerScale : 1.0f );
	}
}

/*
==============
S_WebSync

What plays to the page, in one call, which says whether the sound runs:
started, or held by the browser, what plays starts again from where the
game's time has it
==============
*/
static void S_WebSync( void )
{
	qboolean	nowRunning;
	float		gain;

	// channels started since the last sync start now, and those that ended
	// are freed
	S_ScanChannelStarts( );

	if ( running ) {
		gain = s_muted->integer ? 0 : s_volume->value * 255 / 256;
		if ( gain != masterGain ) {
			masterGain = gain;
			S_WebCommand( WEB_MASTER, 0, gain, 0, 0, 0 );
		}
		S_WebUpdateChannels( );
		S_WebUpdateLoops( );
	}

	nowRunning = MAIN_THREAD_EM_ASM_INT( {
		return Module.sound.update( HEAPF32.subarray( $0 >> 2, ( $0 >> 2 ) + $1 * 6 ) ) ? 1 : 0;
	}, commands, numCommands );
	numCommands = 0;

	if ( nowRunning != running ) {
		running = nowRunning;
		S_WebStopAll( );
	}
}

/*
==============
S_WebTime

The game's time now, which is what's painted: the page plays from it
==============
*/
static void S_WebTime( void )
{
	S_GetSoundtime( );
	s_paintedtime = s_soundtime;
}

/*
==============
S_WebUpdate

Called by S_Base_Update each frame, in place of mixing
==============
*/
void S_WebUpdate( void )
{
	S_WebTime( );

	// add raw data from streamed samples
	S_UpdateBackgroundTrack( );

	S_WebSync( );
}

/*
==============
S_WebSample

A sample at i as a float, from 16-bit little-endian or unsigned 8-bit
data, which needn't be aligned
==============
*/
static float S_WebSample( const byte *data, int width, int i )
{
	if ( width == 2 ) {
		return (short)( data[i * 2] | ( data[i * 2 + 1] << 8 ) ) / 32768.0f;
	}
	return ( data[i] - 128 ) / 128.0f;
}

/*
==============
S_WebRawSamples

A stream's samples (music, a cinematic's sound, voice), after the ones
before, at its left and right gains: the base mixer's raw buffer, ahead of
the game's time. The page drops them while the sound isn't running, so a
stream doesn't pile up to play at once.
==============
*/
void S_WebRawSamples( int stream, int samples, int rate, int width, int channels, const byte *data, float left, float right )
{
	float		*out;
	int			i;
	long long	scaled;

	// in the game's samples, keeping the fraction, so the end doesn't fall
	// behind what the page has queued at a rate not a divisor of dma.speed
	scaled = (long long)samples * dma.speed + rawRemainder[stream];
	s_rawend[stream] += scaled / rate;
	rawRemainder[stream] = scaled % rate;

	out = Hunk_AllocateTempMemory( samples * 2 * sizeof( float ) );
	for ( i = 0; i < samples; i++ ) {
		out[i] = S_WebSample( data, width, i * channels ) * left;
		out[samples + i] = S_WebSample( data, width, i * channels + channels - 1 ) * right;
	}
	MAIN_THREAD_EM_ASM( {
		Module.sound.stream($0, HEAPF32.subarray($1 >> 2, ($1 >> 2) + $2 * 2), $2, $3);
	}, stream, out, samples, rate );
	Hunk_FreeTempMemory( out );
}

/*
==============
S_WebLoadSound

A sound's samples to the page, as an AudioBuffer at their own rate, which
the browser converts as it plays. Its length is in the game's samples, as
the base mixer's resampled sounds' are.
==============
*/
void S_WebLoadSound( sfx_t *sfx, const snd_info_t *info, const byte *data )
{
	float	*planar;
	int		c, i;

	planar = Hunk_AllocateTempMemory( info->samples * info->channels * sizeof( float ) );
	for ( c = 0; c < info->channels; c++ ) {
		for ( i = 0; i < info->samples; i++ ) {
			planar[c * info->samples + i] = S_WebSample( data, info->width, i * info->channels + c );
		}
	}
	MAIN_THREAD_EM_ASM( {
		Module.sound.load($0, HEAPF32.subarray($1 >> 2, ($1 >> 2) + $2 * $3), $2, $3, $4);
	}, sfx - s_knownSfx, planar, info->samples, info->channels, info->rate );
	Hunk_FreeTempMemory( planar );

	sfx->soundCompressionMethod = 0;
	sfx->soundChannels = info->channels;
	sfx->soundLength = (long long)info->samples * dma.speed / info->rate;
}

/*
==============
The base backend's own, and what the page then needs
==============
*/
static void S_Web_StopAllSounds( void )
{
	base.StopAllSounds( );
	S_WebStopAll( );
}

static void S_Web_ClearSoundBuffer( void )
{
	base.ClearSoundBuffer( );
	S_WebStopAll( );
}

static void S_Web_DisableSounds( void )
{
	base.DisableSounds( );
	S_WebStopAll( );
}

static void S_Web_StopBackgroundTrack( void )
{
	base.StopBackgroundTrack( );
	MAIN_THREAD_EM_ASM( { Module.sound.stopStream(0); } );
}

// the frame's new sounds before it's drawn: the cgame places the listener
// as it starts drawing the frame
static void S_Web_Respatialize( int entityNum, const vec3_t origin, vec3_t axis[3], int inwater )
{
	base.Respatialize( entityNum, origin, axis, inwater );
	// the sounds started now from the time now, not the last update's, or
	// they'd be freed before the page has played them out (a recording's
	// clock steps once a frame, in the update)
	if ( !CL_VideoRecording( ) ) {
		S_WebTime( );
	}
	S_WebSync( );
}

static void S_Web_SoundInfo( void )
{
	static const char *states[] = { "not started: it starts with the first click or key", "suspended",
		"running", "closed", "interrupted" };
	int state;

	base.SoundInfo( );
	state = MAIN_THREAD_EM_ASM_INT( { return Module.sound.state(); } );
	Com_Printf( "Web Audio: %s, %i Hz, latency %.0f ms\n", states[state < ARRAY_LEN( states ) ? state : 0],
		MAIN_THREAD_EM_ASM_INT( { return Module.sound.rate(); } ),
		MAIN_THREAD_EM_ASM_DOUBLE( { return Module.sound.latency(); } ) * 1000 );
}

/*
==============
S_Web_Init
==============
*/
qboolean S_Web_Init( soundInterface_t *si )
{
	if ( !S_Base_Init( si ) ) {
		return qfalse;
	}

	base = *si;
	si->StopAllSounds = S_Web_StopAllSounds;
	si->ClearSoundBuffer = S_Web_ClearSoundBuffer;
	si->DisableSounds = S_Web_DisableSounds;
	si->StopBackgroundTrack = S_Web_StopBackgroundTrack;
	si->Respatialize = S_Web_Respatialize;
	si->SoundInfo = S_Web_SoundInfo;
	return qtrue;
}

/*
==============
SNDDMA_Init

No device of our own: the page's, once it has one. dma.speed is the game's
rate for time and lengths, not the device's.
==============
*/
qboolean SNDDMA_Init( void )
{
	if ( !MAIN_THREAD_EM_ASM_INT( { return Module.sound ? 1 : 0; } ) ) {
		Com_Printf( "No Web Audio on this page\n" );
		return qfalse;
	}

	Com_Memset( &dma, 0, sizeof( dma ) );
	dma.speed = 44100;
	dma.channels = 2;
	dma.samplebits = 16;
	dma.submission_chunk = 1;
	// the clock's wrap for S_GetSoundtime: about 1.7 hours
	dma.fullsamples = 1 << 28;
	dma.samples = dma.fullsamples * dma.channels;
	dma.buffer = NULL;

	// the clock runs on through a restart: begun again, S_GetSoundtime would
	// count a wrap, a jump of fullsamples, and soon stop every sound to
	// chop its time
	if ( !clockStarted ) {
		startTime = Com_Milliseconds( );
		clockStarted = qtrue;
	}
	running = qfalse;
	masterGain = -1;
	Com_Memset( strips, 0, sizeof( strips ) );
	Com_Memset( rawRemainder, 0, sizeof( rawRemainder ) );

	Com_Printf( "Sound through Web Audio\n" );
	return qtrue;
}

// the game's time, wrapping as a DMA position does
int SNDDMA_GetDMAPos( void )
{
	long long frames = (long long)( Com_Milliseconds( ) - startTime ) * dma.speed / 1000;

	return (int)( frames % dma.fullsamples ) * dma.channels;
}

void SNDDMA_Shutdown( void )
{
	S_WebStopAll( );
}

void SNDDMA_BeginPainting( void )
{
}

void SNDDMA_Submit( void )
{
}

#ifdef USE_VOIP
// no capture on the web yet: the browser client can't join a server
void SNDDMA_StartCapture( void )
{
}

int SNDDMA_AvailableCaptureSamples( void )
{
	return 0;
}

void SNDDMA_Capture( int samples, byte *data )
{
}

void SNDDMA_StopCapture( void )
{
}

void SNDDMA_MasterGain( float val )
{
}
#endif
