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
// cmd.c -- Quake script command processing module

#include "q_shared.h"
#include "qcommon.h"

#define	MAX_CMD_BUFFER  128*1024
#define	MAX_CMD_LINE	1024

// Where a buffer's text comes from, for cvar_why and the warnings about
// configs: an exec frames the config's text with marker lines, which begin
// with CBUF_MARKER, a byte no text queued can hold (Cbuf_Copy turns it into
// a space, as the tokenizer takes it), and no command sees. "+name" opens a
// frame whose lines are counted from 1; "=line name" one whose lines all
// have the origin of the line that queued them (vstr's text, say, or the
// command line's, line 0); "*name" and "#line name" the same for a pak's
// config on a dedicated server and what it queues (Cmd_IsPakConfig); and
// "-" closes the latest.
#define	CBUF_MARKER			'\x01'
#define	MAX_ORIGIN_DEPTH	16

typedef struct {
	char		name[MAX_QPATH];	// "" for text from no config
	int			line;				// the line's number, 0 for none
	qboolean	counted;			// a frame's: line is the next line's, and counts on
	qboolean	pak;				// a pak's config's, on a dedicated server
} cmdOrigin_t;

typedef struct {
	byte	*data;
	int		maxsize;
	int		cursize;
	int		wait;				// frames to wait before running more of it
	qboolean	inStarComment;	// within one Cbuf_Execute: see Cbuf_ExecuteLine
	qboolean	inSlashComment;
	cmdOrigin_t	origins[MAX_ORIGIN_DEPTH];	// the frames open where its text starts
	int			depth;			// may pass MAX_ORIGIN_DEPTH, when the deepest aren't kept
} cmd_t;

// the running command's origin (Cmd_Origin), and those of the text run at
// once that Cmd_PushOrigin set, the latest last
#define	MAX_PUSHED_ORIGINS	4
static cmdOrigin_t	cmd_origin;
static cmdOrigin_t	cmd_pushedOrigins[MAX_PUSHED_ORIGINS];
static int			cmd_numPushedOrigins;

// The command buffers, by whose text they hold: the player's (and the
// configs' and the engine's), or game code's, which runs restricted; each
// either a script's that runs at every start (default.cfg, autoexec.cfg),
// whose sets aren't saved, or not. A buffer's index is its two bits
#define CBUF_SCRIPT		1
#define CBUF_RESTRICTED	2
#define CBUF_COUNT		4

static cmd_t	cmd_buffers[CBUF_COUNT];
static byte		cmd_buffersData[CBUF_COUNT][MAX_CMD_BUFFER];

// the running command is restricted: it came from game code, or from text
// that game code queued or ran
static qboolean	cmd_restricted;

// the running command came from a startup script, or from text one ran
static qboolean	cmd_script;

// the command line's configs are running again (Cmd_HoldMapCommands): the
// startup scripts' commands that would change the map are held back until
// their buffers are empty
static qboolean	cmd_holdMapCommands;

// the configs startup scripts ran, as exec named them; past the most it
// holds, every config counts as one
#define MAX_STARTUP_SCRIPTS	64
static char		cmd_startupScripts[MAX_STARTUP_SCRIPTS][MAX_QPATH];
static int		cmd_numStartupScripts;
static qboolean	cmd_startupScriptsFull;

// the configs the command line execs, still to run, which run as startup
// scripts when it does
static char		cmd_commandLineExecs[MAX_STARTUP_SCRIPTS][MAX_QPATH];
static int		cmd_numCommandLineExecs;

static cmd_t *Cbuf_For( qboolean restricted, qboolean script ) {
	return &cmd_buffers[ ( restricted ? CBUF_RESTRICTED : 0 ) | ( script ? CBUF_SCRIPT : 0 ) ];
}

// the buffer text a command adds goes to: text a restricted command adds
// stays restricted, and a script's stays the script's
static cmd_t *Cbuf_Current( void ) {
	return Cbuf_For( cmd_restricted, cmd_script );
}


//=============================================================================

/*
============
Cmd_Wait_f

Causes execution of the remainder of the command buffer to be delayed until
next frame.  This allows commands like:
bind g "cmd use rocket ; +attack ; wait ; -attack ; cmd use blaster"
============
*/
void Cmd_Wait_f( void ) {
	cmd_t	*buf = Cbuf_Current();

	if ( Cmd_Argc() == 2 ) {
		buf->wait = atoi( Cmd_Argv( 1 ) );
		if ( buf->wait < 0 )
			buf->wait = 1; // ignore the argument
	} else {
		buf->wait = 1;
	}
}


/*
=============================================================================

						COMMAND BUFFER

=============================================================================
*/

/*
============
Cbuf_Init
============
*/
void Cbuf_Init (void)
{
	int	i;

	for ( i = 0; i < CBUF_COUNT; i++ ) {
		cmd_buffers[i].data = cmd_buffersData[i];
		cmd_buffers[i].maxsize = MAX_CMD_BUFFER;
		cmd_buffers[i].cursize = 0;
	}
}

// copies text into a buffer, with no marker byte in it
static void Cbuf_Copy( byte *to, const char *text, int length ) {
	int	i;

	for ( i = 0; i < length; i++ ) {
		to[i] = text[i] == CBUF_MARKER ? ' ' : text[i];
	}
}

static void Cbuf_Add( cmd_t *buf, const char *text ) {
	int		l;

	l = strlen (text);

	if (buf->cursize + l >= buf->maxsize)
	{
		Com_Printf ("Cbuf_AddText: overflow\n");
		return;
	}
	Cbuf_Copy(&buf->data[buf->cursize], text, l);
	buf->cursize += l;
}

/*
============
Cbuf_WriteFrame

Writes the frame a marker opens around text (Cbuf_ExecuteMarker), each
followed by a \n, then the frame's end; with no marker, the text and a \n.
Returns the length written, or with to NULL, to be written
============
*/
static int Cbuf_WriteFrame( byte *to, const char *marker, const char *text ) {
	static const char	close[] = { CBUF_MARKER, '-', '\n' };
	int		markerLength = marker ? strlen( marker ) + 1 : 0, textLength = strlen( text ) + 1;

	if ( to ) {
		if ( marker ) {
			Com_Memcpy( to, marker, markerLength - 1 );
			to[ markerLength - 1 ] = '\n';
		}
		Cbuf_Copy( to + markerLength, text, textLength - 1 );
		to[ markerLength + textLength - 1 ] = '\n';
		if ( marker ) {
			Com_Memcpy( to + markerLength + textLength, close, sizeof( close ) );
		}
	}
	return markerLength + textLength + ( marker ? sizeof( close ) : 0 );
}

/*
============
Cbuf_InsertFrame

Inserts text at the buffer's start in the marker's frame (Cbuf_WriteFrame):
all of it, or nothing if it doesn't fit
============
*/
static void Cbuf_InsertFrame( cmd_t *buf, const char *marker, const char *text ) {
	int	len = Cbuf_WriteFrame( NULL, marker, text );

	if ( len + buf->cursize > buf->maxsize ) {
		Com_Printf( "Cbuf_InsertText overflowed\n" );
		return;
	}
	// a frame past the deepest kept has no origin, so a pak's text there
	// would run as no one's: an exec loop in a pak's config stops here
	if ( marker && ( marker[1] == '*' || marker[1] == '#' ) && buf->depth >= MAX_ORIGIN_DEPTH ) {
		Com_Printf( "%s: configs nested too deep\n", Cmd_Origin() ? Cmd_Origin() : "a pak's config" );
		return;
	}

	// move the existing command text
	memmove( buf->data + len, buf->data, buf->cursize );
	Cbuf_WriteFrame( buf->data, marker, text );
	buf->cursize += len;
}

/*
============
Cbuf_OriginMarker

The marker of a frame whose lines all have an origin, line 0 for none
============
*/
static const char *Cbuf_OriginMarker( const char *name, int line, qboolean pak ) {
	return va( "%c%c%i %s", CBUF_MARKER, pak ? '#' : '=', line, name );
}

// inserts text a command queues, in a frame keeping the command's origin;
// with none, and no frame to keep it out of, as it is
static void Cbuf_Insert( cmd_t *buf, const char *text ) {
	Cbuf_InsertFrame( buf, cmd_origin.name[0] || cmd_origin.pak || buf->depth ?
		Cbuf_OriginMarker( cmd_origin.name, cmd_origin.line, cmd_origin.pak ) : NULL, text );
}

/*
============
Cbuf_ExecuteMarker

Opens or closes the frame of the marker line at the position given, and
takes the line out
============
*/
static void Cbuf_ExecuteMarker( cmd_t *buf, int at ) {
	char		*text = (char *)buf->data + at;
	char		*end = memchr( text, '\n', buf->cursize - at );
	int			length = end ? end - text + 1 : buf->cursize - at;
	cmdOrigin_t	*origin;

	if ( text[1] == '-' ) {
		if ( buf->depth > 0 ) {
			buf->depth--;
		}
	} else if ( buf->depth++ < MAX_ORIGIN_DEPTH ) {
		const char	*name = text + 2;

		origin = &buf->origins[ buf->depth - 1 ];
		origin->counted = text[1] == '+' || text[1] == '*';
		origin->pak = text[1] == '*' || text[1] == '#';
		origin->line = 1;
		if ( !origin->counted ) {
			origin->line = atoi( name );
			name = strchr( name, ' ' ) ? strchr( name, ' ' ) + 1 : name;
		}
		Q_strncpyz( origin->name, name, MIN( (int)sizeof( origin->name ), (int)( text + length - name ) ) );
	}

	buf->cursize -= length;
	memmove( text, text + length, buf->cursize - at );
}

/*
============
Cbuf_AddText

Adds command text at the end of the buffer, does NOT add a final \n
============
*/
void Cbuf_AddText( const char *text ) {
	Cbuf_Add( Cbuf_Current(), text );
}


/*
============
Cbuf_InsertText

Adds command text immediately after the current command
Adds a \n to the text
============
*/
void Cbuf_InsertText( const char *text ) {
	Cbuf_Insert( Cbuf_Current(), text );
}

/*
============
Cbuf_AddTextRestricted, Cbuf_InsertTextRestricted

The same, into game code's buffer if restricted, or else the player's
============
*/
void Cbuf_AddTextRestricted( const char *text, qboolean restricted ) {
	Cbuf_Add( Cbuf_For( restricted, cmd_script ), text );
}

void Cbuf_InsertTextRestricted( const char *text, qboolean restricted ) {
	Cbuf_Insert( Cbuf_For( restricted, cmd_script ), text );
}

/*
============
Cbuf_AddScriptText

Text that runs at every start (default.cfg, autoexec.cfg), with full
rights: what it and the configs it execs set is a script's, not saved,
whenever it runs, after waits too
============
*/
void Cbuf_AddScriptText( const char *text ) {
	Cbuf_Add( Cbuf_For( qfalse, qtrue ), text );
}

/*
============
Cmd_IsScript
============
*/
qboolean Cmd_IsScript( void ) {
	return cmd_script;
}

/*
============
Cmd_HoldMapCommands

Holds back the map commands of the startup scripts' text queued from now
until it has all run, so that configs run again change settings, not the
map
============
*/
void Cmd_HoldMapCommands( void ) {
	cmd_holdMapCommands = qtrue;
}

/*
============
Cmd_AddCommandLineExec

A config a dedicated server's command line execs: when that exec runs,
the config is a startup script, as autoexec.cfg is
============
*/
void Cmd_AddCommandLineExec( const char *filename ) {
	if ( cmd_numCommandLineExecs < MAX_STARTUP_SCRIPTS ) {
		Q_strncpyz( cmd_commandLineExecs[cmd_numCommandLineExecs], FS_SkipPathPrefix( filename ), MAX_QPATH );
		COM_DefaultExtension( cmd_commandLineExecs[cmd_numCommandLineExecs], MAX_QPATH, ".cfg" );
		cmd_numCommandLineExecs++;
	}
}

/*
============
Cmd_TakeCommandLineExec

Whether the command line's exec of the config is the one running, which
it is only once
============
*/
static qboolean Cmd_TakeCommandLineExec( const char *filename ) {
	int		i;

	filename = FS_SkipPathPrefix( filename );
	for ( i = 0; i < cmd_numCommandLineExecs; i++ ) {
		if ( !Q_stricmp( filename, cmd_commandLineExecs[i] ) ) {
			// the last one in its place, unless it's the last
			if ( i != --cmd_numCommandLineExecs ) {
				Q_strncpyz( cmd_commandLineExecs[i], cmd_commandLineExecs[cmd_numCommandLineExecs], MAX_QPATH );
			}
			return qtrue;
		}
	}
	return qfalse;
}

/*
============
Cmd_IsStartupScript
============
*/
qboolean Cmd_IsStartupScript( const char *filename ) {
	int		i;

	if ( cmd_startupScriptsFull ) {
		return qtrue;
	}
	filename = FS_SkipPathPrefix( filename );
	for ( i = 0; i < cmd_numStartupScripts; i++ ) {
		if ( !Q_stricmp( filename, cmd_startupScripts[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
============
Cmd_IsRestricted

Whether the running command is restricted: game code queued or ran it,
or it came from text that did. Restricted commands can't run the ones
that reveal secrets or stop the process, or touch private and protected
cvars
============
*/
qboolean Cmd_IsRestricted( void ) {
	return cmd_restricted;
}

/*
============
Cmd_EndRestricted

An error ended the running command
============
*/
void Cmd_EndRestricted( void ) {
	cmd_restricted = qfalse;
	cmd_script = qfalse;
	// nor does its origin outlive it (Cbuf_ExecuteLine, Cmd_PushOrigin)
	cmd_origin.name[0] = '\0';
	cmd_origin.line = 0;
	cmd_origin.pak = qfalse;
	cmd_numPushedOrigins = 0;
}


/*
============
Cbuf_ExecuteTextWithRights

Cbuf_ExecuteText, restricted or not, whatever the running command is
============
*/
static void Cbuf_ExecuteTextWithRights( int exec_when, const char *text, qboolean restricted )
{
	qboolean	running = cmd_restricted, script = cmd_script, pak = cmd_origin.pak;

	// the engine's text, or game code's, isn't a script's; nor is game
	// code's a pak config's when a pak's map command started the game
	cmd_restricted = restricted;
	cmd_script = qfalse;
	if ( restricted ) {
		cmd_origin.pak = qfalse;
	}
	switch (exec_when)
	{
	case EXEC_NOW:
		if (text && strlen(text) > 0) {
			Com_DPrintf(S_COLOR_YELLOW "EXEC_NOW %s\n", text);
			Cmd_ExecuteString (text);
		} else {
			Cbuf_Execute();
			Com_DPrintf(S_COLOR_YELLOW "EXEC_NOW %s\n", Cbuf_For( qfalse, qfalse )->data);
		}
		break;
	case EXEC_INSERT:
		Cbuf_InsertText (text);
		break;
	case EXEC_APPEND:
		Cbuf_AddText (text);
		break;
	default:
		Com_Error (ERR_DROP, "Cbuf_ExecuteText: bad exec_when");
	}
	cmd_restricted = running;
	cmd_script = script;
	cmd_origin.pak = pak;
}

/*
============
Cbuf_ExecuteText

The engine's text, which runs with full rights even when a restricted
command made the engine run it (a vid_restart that restarts the game
runs the player's configs)
============
*/
void Cbuf_ExecuteText( int exec_when, const char *text )
{
	Cbuf_ExecuteTextWithRights( exec_when, text, qfalse );
}

/*
============
Cbuf_ExecuteTextRestricted

Game code's command text, which runs restricted: see Cmd_IsRestricted
============
*/
void Cbuf_ExecuteTextRestricted( int exec_when, const char *text )
{
	Cbuf_ExecuteTextWithRights( exec_when, text, qtrue );
}

// no origin
static const cmdOrigin_t	cmd_noOrigin;

/*
============
Cbuf_Frame

The frame open where the buffer's text now starts, or NULL
============
*/
static cmdOrigin_t *Cbuf_Frame( cmd_t *buf ) {
	return buf->depth > 0 && buf->depth <= MAX_ORIGIN_DEPTH ? &buf->origins[ buf->depth - 1 ] : NULL;
}

/*
============
Cbuf_ExecuteLine

Runs the next command in a buffer, with the buffer's rights
============
*/
static void Cbuf_ExecuteLine( cmd_t *buf )
{
	int		i;
	char	*text;
	char	line[MAX_CMD_LINE];
	cmdOrigin_t	origin, lineOrigin;
	int		quotes, consumed;
	qboolean	restricted, script, started;
	cmdOrigin_t	*frame;

	// find a \n or ; line break or comment: // or /* */
	// This will keep // style comments all on one line by not breaking on
	// a semicolon.  It will keep /* ... */ style comments all on one line by not
	// breaking it for semicolon or newline.
	text = (char *)buf->data;

	quotes = 0;
	started = qfalse;
	for (i=0 ; i< buf->cursize ; i++)
	{
		// a frame's marker: its frame opens or closes there, and the line
		// goes on as if it weren't there, a comment too
		if ( text[i] == CBUF_MARKER ) {
			Cbuf_ExecuteMarker( buf, i-- );
			continue;
		}
		// the line's origin is that of its frame where it starts
		if ( !started ) {
			started = qtrue;
			frame = Cbuf_Frame( buf );
			lineOrigin = frame ? *frame : cmd_noOrigin;
		}

		if (text[i] == '"')
			quotes++;

		if ( !(quotes&1)) {
			if (i < buf->cursize - 1) {
				if (! buf->inStarComment && text[i] == '/' && text[i+1] == '/')
					buf->inSlashComment = qtrue;
				else if (! buf->inSlashComment && text[i] == '/' && text[i+1] == '*')
					buf->inStarComment = qtrue;
				else if (buf->inStarComment && text[i] == '*' && text[i+1] == '/') {
					buf->inStarComment = qfalse;
					// If we are in a star comment, then the part after it is valid
					// Note: This will cause it to NUL out the terminating '/'
					// but ExecuteString doesn't require it anyway.
					i++;
					break;
				}
			}
			if (! buf->inSlashComment && ! buf->inStarComment && text[i] == ';')
				break;
		}
		if (! buf->inStarComment && (text[i] == '\n' || text[i] == '\r')) {
			buf->inSlashComment = qfalse;
			break;
		}
	}
	if ( !started ) {
		frame = Cbuf_Frame( buf );
		lineOrigin = frame ? *frame : cmd_noOrigin;
	}

	if( i >= (MAX_CMD_LINE - 1)) {
		i = MAX_CMD_LINE - 1;
	}

	Com_Memcpy (line, text, i);
	line[i] = 0;

// delete the text from the command buffer and move remaining commands down
// this is necessary because commands (exec) can insert data at the
// beginning of the text buffer

	consumed = i == buf->cursize ? i : i + 1;

	// the frame's next line, after the lines consumed
	frame = Cbuf_Frame( buf );
	if ( frame && frame->counted ) {
		for ( i = 0; i < consumed; i++ ) {
			frame->line += text[i] == '\n';
		}
	}

	buf->cursize -= consumed;
	memmove (text, text+consumed, buf->cursize);

// execute the command line, restricted if it's game code's, and a
// script's if it's a startup script's

	restricted = cmd_restricted;
	script = cmd_script;
	origin = cmd_origin;
	cmd_restricted = ( ( buf - cmd_buffers ) & CBUF_RESTRICTED ) != 0;
	cmd_script = ( ( buf - cmd_buffers ) & CBUF_SCRIPT ) != 0;
	cmd_origin = lineOrigin;
	Cmd_ExecuteString (line);
	cmd_restricted = restricted;
	cmd_script = script;
	cmd_origin = origin;

	// the frames the line ends close at once, so that text a wait ends
	// leaves the buffer empty, as it did before frames: the wait then
	// holds for the next text queued (Cbuf_ExecuteFirst), and Cbuf_Empty
	// says so
	while ( buf->cursize > 1 && buf->data[0] == CBUF_MARKER && buf->data[1] == '-' ) {
		Cbuf_ExecuteMarker( buf, 0 );
	}
}

/*
============
Cmd_Origin
============
*/
const char *Cmd_Origin( void ) {
	static char	text[MAX_QPATH + 16];

	if ( !cmd_origin.name[0] ) {
		return NULL;
	}
	if ( !cmd_origin.line ) {
		return cmd_origin.name;
	}
	Com_sprintf( text, sizeof( text ), "%s:%i", cmd_origin.name, cmd_origin.line );
	return text;
}

/*
============
Cmd_OriginIsCommandLine
============
*/
qboolean Cmd_OriginIsCommandLine( void ) {
	return !strcmp( cmd_origin.name, CMD_ORIGIN_COMMAND_LINE );
}

/*
============
Cmd_IsPakConfig

Whether the running command came from a config in a pak on a dedicated
server, or from text one queued (Cmd_Exec_f's frame)
============
*/
qboolean Cmd_IsPakConfig( void ) {
	return cmd_origin.pak;
}

/*
============
Cmd_PushOrigin

The origin of text run at once rather than queued, a settings file's
lines, say, until Cmd_PopOrigin
============
*/
void Cmd_PushOrigin( const char *name, int line ) {
	if ( cmd_numPushedOrigins < MAX_PUSHED_ORIGINS ) {
		cmd_pushedOrigins[cmd_numPushedOrigins] = cmd_origin;
	}
	cmd_numPushedOrigins++;
	Q_strncpyz( cmd_origin.name, name, sizeof( cmd_origin.name ) );
	cmd_origin.line = line;
	cmd_origin.counted = qfalse;
	cmd_origin.pak = qfalse;
}

/*
============
Cmd_PopOrigin
============
*/
void Cmd_PopOrigin( void ) {
	if ( cmd_numPushedOrigins > 0 && --cmd_numPushedOrigins < MAX_PUSHED_ORIGINS ) {
		cmd_origin = cmd_pushedOrigins[cmd_numPushedOrigins];
	}
}

/*
============
Cbuf_AddTextFrom

Text added to the player's buffer whose lines all have one origin
============
*/
void Cbuf_AddTextFrom( const char *origin, const char *text ) {
	cmd_t		*buf = Cbuf_Current();
	const char	*marker = Cbuf_OriginMarker( origin, 0, qfalse );
	int			len = Cbuf_WriteFrame( NULL, marker, text );

	if ( buf->cursize + len >= buf->maxsize ) {
		Com_Printf( "Cbuf_AddText: overflow\n" );
		return;
	}
	Cbuf_WriteFrame( buf->data + buf->cursize, marker, text );
	buf->cursize += len;
}

// the order the buffers run in: the startup scripts' first, as one buffer
// ran them before the command line's commands even after a wait, then the
// player's, then game code's
static const int cbuf_order[CBUF_COUNT] = {
	CBUF_SCRIPT, CBUF_RESTRICTED | CBUF_SCRIPT, 0, CBUF_RESTRICTED
};

/*
============
Cbuf_ExecuteFirst

Runs the first count buffers in order, a line from the first that has one
and doesn't wait, until none has; then counts down their waits
============
*/
static void Cbuf_ExecuteFirst( int count )
{
	int	i;

	// a comment runs to the end of what one call executes, as it always has
	for ( i = 0; i < count; i++ ) {
		cmd_buffers[ cbuf_order[i] ].inStarComment = cmd_buffers[ cbuf_order[i] ].inSlashComment = qfalse;
	}

	for ( ;; ) {
		for ( i = 0; i < count; i++ ) {
			cmd_t *buf = &cmd_buffers[ cbuf_order[i] ];

			if ( buf->cursize && buf->wait <= 0 ) {
				Cbuf_ExecuteLine( buf );
				break;
			}
		}
		if ( i == count ) {
			break;
		}
	}

	// a buffer still holding text after a wait runs it a frame later
	for ( i = 0; i < count; i++ ) {
		cmd_t *buf = &cmd_buffers[ cbuf_order[i] ];

		if ( buf->cursize && buf->wait > 0 ) {
			buf->wait--;
		}
	}

	// a reload ends when the scripts' text it queued has all run
	if ( Cbuf_ScriptsEmpty() ) {
		cmd_holdMapCommands = qfalse;
	}
}

/*
============
Cbuf_ExecuteScripts

Runs the startup scripts' buffers only, leaving the player's and game
code's, and their waits, for the frame: a startup script run in the middle
of a command, such as game_restart's, doesn't run the commands after it
first
============
*/
void Cbuf_ExecuteScripts( void )
{
	Cbuf_ExecuteFirst( 2 );	// cbuf_order's two script buffers
}

/*
============
Cbuf_ScriptsEmpty

Whether the startup scripts' buffers are empty: no script waits to run
============
*/
qboolean Cbuf_ScriptsEmpty( void )
{
	return !cmd_buffers[CBUF_SCRIPT].cursize && !cmd_buffers[CBUF_SCRIPT | CBUF_RESTRICTED].cursize;
}

/*
============
Cbuf_Empty

Whether no buffer holds text, waiting or not
============
*/
qboolean Cbuf_Empty( void )
{
	int	i;

	for ( i = 0; i < CBUF_COUNT; i++ ) {
		if ( cmd_buffers[i].cursize ) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
============
Cbuf_Clear

Drops every buffer's text and waits: --check's, once its startup's
reported, before it follows the rotation
============
*/
void Cbuf_Clear( void )
{
	int	i;

	for ( i = 0; i < CBUF_COUNT; i++ ) {
		cmd_buffers[i].cursize = 0;
		cmd_buffers[i].wait = 0;
		cmd_buffers[i].depth = 0;
	}
}

/*
============
Cbuf_Execute
============
*/
void Cbuf_Execute (void)
{
	Cbuf_ExecuteFirst( CBUF_COUNT );
}


/*
==============================================================================

						SCRIPT COMMANDS

==============================================================================
*/


/*
===============
Cmd_Exec_f
===============
*/
void Cmd_Exec_f( void ) {
	qboolean quiet, script, predecessor, restricted;
	int		i;
	union {
		char	*c;
		void	*v;
	} f;
	char	filename[MAX_QPATH];
	char	ospath[MAX_OSPATH];
	char	content[MAX_QPATH];

	quiet = !Q_stricmp(Cmd_Argv(0), "execq");

	if (Cmd_Argc () != 2) {
		Com_Printf ("exec%s <filename> : execute a script file%s\n",
		            quiet ? "q" : "", quiet ? " without notification" : "");
		return;
	}

	Q_strncpyz( filename, Cmd_Argv(1), sizeof( filename ) );
	COM_DefaultExtension( filename, sizeof( filename ), ".cfg" );

	FS_ReadFile( filename, &f.v);
	Q_strncpyz( content, FS_LastFileContent(), sizeof( content ) );
	// a pak's config execs only what's in a pak: refused before anything
	// takes note of the exec (startup scripts, the predecessor's home)
	if ( Cmd_IsPakConfig() && f.c && !content[0] ) {
		FS_FreeFile( f.v );
		Com_Printf( "%s can't exec %s, which isn't in a pak.\n", Cmd_Origin(), filename );
		return;
	}
	// the pak's name goes in a marker line, which a line break would end
	for ( i = 0; content[i]; i++ ) {
		if ( (unsigned char)content[i] < ' ' ) {
			content[i] = '?';
		}
	}

	// the command line's exec runs as a startup script, where the command
	// line puts it: before the rest of its buffer, as an exec's text runs.
	// Taken even when the config is missing, so that a later exec of it,
	// the admin's, isn't one
	script = cmd_script || ( !cmd_restricted && Cmd_TakeCommandLineExec( filename ) );

	// a dedicated server moved from the predecessor's home finds the configs
	// it kept there, as it finds the paks, and says which it ran
	predecessor = !f.c && !Com_IsClient() && !cmd_restricted && !Cmd_IsPakConfig() &&
		FS_ReadPredecessorConfig( filename, &f.v, ospath, sizeof( ospath ) ) >= 0;
	if (!f.c) {
		Com_Printf ("couldn't exec %s: it isn't in %s\n", filename,
			FS_SearchDirs( !Com_IsClient() && !cmd_restricted ? filename : NULL ));
		return;
	}
	if ( predecessor )
		Com_Printf( "execing %s from %s\n", filename, ospath );
	else if (!quiet)
		Com_Printf ("execing %s\n", filename);
	if ( com_check ) {
		Com_NoteConfigRun( filename, predecessor ? ospath : FS_LastFilePath( filename ) );
	}

	if ( script && !Cmd_IsStartupScript( filename ) ) {
		if ( cmd_numStartupScripts < MAX_STARTUP_SCRIPTS ) {
			Q_strncpyz( cmd_startupScripts[cmd_numStartupScripts++],
				FS_SkipPathPrefix( filename ), MAX_QPATH );
		} else {
			Com_Printf( S_COLOR_YELLOW "WARNING: more than %i configs ran at startup; "
				"writeconfig now replaces none\n", MAX_STARTUP_SCRIPTS );
			cmd_startupScriptsFull = qtrue;
		}
	}

	// a config from a pk3 or pk3dir, which a download can bring, runs
	// restricted in the client. On a dedicated server, where a mod's or a
	// server pack's paks bring them, it sets what it likes, passwords
	// among them, but can't reach past the paks: its frame is a pak's
	// (Cmd_IsPakConfig), named for the pak, which its lines' origin shows
	restricted = cmd_restricted || ( Com_IsClient() && content[0] );
	if ( !Com_IsClient() && content[0] ) {
		// but default.cfg, which every pak0 has
		if ( !Cmd_IsPakConfig() && Q_stricmp( filename, "default.cfg" ) ) {
			Com_Printf( "%s is in %s, so it can set settings, but not write files, exec configs "
				"outside paks or reach the system, and what it sets isn't saved.\n", filename, content );
		}
		Cbuf_InsertFrame( Cbuf_For( restricted, script ), va( "%c*%s/%s", CBUF_MARKER, content, filename ), f.c );
	} else {
		Cbuf_InsertFrame( Cbuf_For( restricted, script ), va( "%c+%s", CBUF_MARKER, filename ), f.c );
	}

	if ( predecessor )
		Z_Free( f.v );
	else
		FS_FreeFile (f.v);
}


/*
===============
Cmd_Vstr_f

Inserts the current value of a variable as command text
===============
*/
void Cmd_Vstr_f( void ) {
	char	*v;

	if (Cmd_Argc () != 2) {
		Com_Printf ("vstr <variablename> : execute a variable command\n");
		return;
	}

	// restricted text can't run, and so read, a private or protected cvar
	if ( !Cvar_AllowedFromText( Cmd_Argv( 1 ) ) ) {
		return;
	}

	// with the rights of whoever set it: see Cvar_RunsRestricted; and a
	// pak's config's, run later by game code (vstr nextmap), keeps its limits
	v = Cvar_VariableString( Cmd_Argv( 1 ) );
	if ( Cvar_SetByPak( Cmd_Argv( 1 ) ) ) {
		Cbuf_InsertFrame( Cbuf_For( Cvar_RunsRestricted( Cmd_Argv( 1 ) ), cmd_script ),
			Cbuf_OriginMarker( cmd_origin.name, cmd_origin.line, qtrue ), v );
	} else {
		Cbuf_InsertTextRestricted( va("%s\n", v ), Cvar_RunsRestricted( Cmd_Argv( 1 ) ) );
	}
}


/*
===============
Cmd_Echo_f

Just prints the rest of the line to the console
===============
*/
void Cmd_Echo_f (void)
{
	Com_Printf ("%s\n", Cmd_Args());
}


/*
=============================================================================

					COMMAND EXECUTION

=============================================================================
*/

typedef struct cmd_function_s
{
	struct cmd_function_s	*next;
	char					*name;
	xcommand_t				function;
	completionFunc_t	complete;
} cmd_function_t;


static	int			cmd_argc;
static	char		*cmd_argv[MAX_STRING_TOKENS];		// points into cmd_tokenized
static	char		cmd_tokenized[BIG_INFO_STRING+MAX_STRING_TOKENS];	// will have 0 bytes inserted
static	char		cmd_cmd[BIG_INFO_STRING]; // the original command we received (no token processing)

static	cmd_function_t	*cmd_functions;		// possible commands to execute

/*
============
Cmd_Argc
============
*/
int		Cmd_Argc( void ) {
	return cmd_argc;
}

/*
============
Cmd_Argv
============
*/
char	*Cmd_Argv( int arg ) {
	if ( (unsigned)arg >= cmd_argc ) {
		return "";
	}
	return cmd_argv[arg];	
}

/*
============
Cmd_ArgvBuffer

The interpreted versions use this because
they can't have pointers returned to them
============
*/
void	Cmd_ArgvBuffer( int arg, char *buffer, int bufferLength ) {
	Q_strncpyz( buffer, Cmd_Argv( arg ), bufferLength );
}


/*
============
Cmd_Args

Returns a single string containing argv(1) to argv(argc()-1)
============
*/
char	*Cmd_Args( void ) {
	static	char		cmd_args[MAX_STRING_CHARS];
	int		i;

	cmd_args[0] = 0;
	for ( i = 1 ; i < cmd_argc ; i++ ) {
		strcat( cmd_args, cmd_argv[i] );
		if ( i != cmd_argc-1 ) {
			strcat( cmd_args, " " );
		}
	}

	return cmd_args;
}

/*
============
Cmd_Args

Returns a single string containing argv(arg) to argv(argc()-1)
============
*/
char *Cmd_ArgsFrom( int arg ) {
	static	char		cmd_args[BIG_INFO_STRING];
	int		i;

	cmd_args[0] = 0;
	if (arg < 0)
		arg = 0;
	for ( i = arg ; i < cmd_argc ; i++ ) {
		strcat( cmd_args, cmd_argv[i] );
		if ( i != cmd_argc-1 ) {
			strcat( cmd_args, " " );
		}
	}

	return cmd_args;
}

/*
============
Cmd_ArgsBuffer

The interpreted versions use this because
they can't have pointers returned to them
============
*/
void	Cmd_ArgsBuffer( char *buffer, int bufferLength ) {
	Q_strncpyz( buffer, Cmd_Args(), bufferLength );
}

/*
============
Cmd_Cmd

Retrieve the unmodified command string
For rcon use when you want to transmit without altering quoting
https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=543
============
*/
char *Cmd_Cmd(void)
{
	return cmd_cmd;
}

/*
   Replace command separators with space to prevent interpretation
   This is a hack to protect buggy qvms
   https://bugzilla.icculus.org/show_bug.cgi?id=3593
   https://bugzilla.icculus.org/show_bug.cgi?id=4769
*/

void Cmd_Args_Sanitize(void)
{
	int i;

	for(i = 1; i < cmd_argc; i++)
	{
		char *c = cmd_argv[i];
		
		if(strlen(c) > MAX_CVAR_VALUE_STRING - 1)
			c[MAX_CVAR_VALUE_STRING - 1] = '\0';
		
		while ((c = strpbrk(c, "\n\r;"))) {
			*c = ' ';
			++c;
		}
	}
}

/*
============
Cmd_TokenizeString

Parses the given string into command line tokens.
The text is copied to a separate buffer and 0 characters
are inserted in the appropriate place, The argv array
will point into this temporary buffer.
============
*/
// NOTE TTimo define that to track tokenization issues
//#define TKN_DBG
static void Cmd_TokenizeString2( const char *text_in, qboolean ignoreQuotes ) {
	const char	*text;
	char	*textOut;

#ifdef TKN_DBG
  // FIXME TTimo blunt hook to try to find the tokenization of userinfo
  Com_DPrintf("Cmd_TokenizeString: %s\n", text_in);
#endif

	// clear previous args
	cmd_argc = 0;

	if ( !text_in ) {
		return;
	}
	
	Q_strncpyz( cmd_cmd, text_in, sizeof(cmd_cmd) );

	text = text_in;
	textOut = cmd_tokenized;

	while ( 1 ) {
		if ( cmd_argc == MAX_STRING_TOKENS ) {
			return;			// this is usually something malicious
		}

		while ( 1 ) {
			// skip whitespace
			while ( *text && *text <= ' ' ) {
				text++;
			}
			if ( !*text ) {
				return;			// all tokens parsed
			}

			// skip // comments
			if ( text[0] == '/' && text[1] == '/' ) {
				return;			// all tokens parsed
			}

			// skip /* */ comments
			if ( text[0] == '/' && text[1] =='*' ) {
				while ( *text && ( text[0] != '*' || text[1] != '/' ) ) {
					text++;
				}
				if ( !*text ) {
					return;		// all tokens parsed
				}
				text += 2;
			} else {
				break;			// we are ready to parse a token
			}
		}

		// handle quoted strings
    // NOTE TTimo this doesn't handle \" escaping
		if ( !ignoreQuotes && *text == '"' ) {
			cmd_argv[cmd_argc] = textOut;
			cmd_argc++;
			text++;
			while ( *text && *text != '"' ) {
				*textOut++ = *text++;
			}
			*textOut++ = 0;
			if ( !*text ) {
				return;		// all tokens parsed
			}
			text++;
			continue;
		}

		// regular token
		cmd_argv[cmd_argc] = textOut;
		cmd_argc++;

		// skip until whitespace, quote, or command
		while ( *text > ' ' ) {
			if ( !ignoreQuotes && text[0] == '"' ) {
				break;
			}

			if ( text[0] == '/' && text[1] == '/' ) {
				break;
			}

			// skip /* */ comments
			if ( text[0] == '/' && text[1] =='*' ) {
				break;
			}

			*textOut++ = *text++;
		}

		*textOut++ = 0;

		if ( !*text ) {
			return;		// all tokens parsed
		}
	}
	
}

/*
============
Cmd_TokenizeString
============
*/
void Cmd_TokenizeString( const char *text_in ) {
	Cmd_TokenizeString2( text_in, qfalse );
}

/*
============
Cmd_TokenizeStringIgnoreQuotes
============
*/
void Cmd_TokenizeStringIgnoreQuotes( const char *text_in ) {
	Cmd_TokenizeString2( text_in, qtrue );
}

/*
============
Cmd_FindCommand
============
*/
cmd_function_t *Cmd_FindCommand( const char *cmd_name )
{
	cmd_function_t *cmd;
	for( cmd = cmd_functions; cmd; cmd = cmd->next )
		if( !Q_stricmp( cmd_name, cmd->name ) )
			return cmd;
	return NULL;
}

/*
============
Cmd_AddCommand
============
*/
void	Cmd_AddCommand( const char *cmd_name, xcommand_t function ) {
	cmd_function_t	*cmd;
	
	// fail if the command already exists
	if( Cmd_FindCommand( cmd_name ) )
	{
		// allow completion-only commands to be silently doubled
		if( function != NULL )
			Com_Printf( "Cmd_AddCommand: %s already defined\n", cmd_name );
		return;
	}

	// use a small malloc to avoid zone fragmentation
	cmd = S_Malloc (sizeof(cmd_function_t));
	cmd->name = CopyString( cmd_name );
	cmd->function = function;
	cmd->complete = NULL;
	cmd->next = cmd_functions;
	cmd_functions = cmd;
}

/*
============
Cmd_SetCommandCompletionFunc
============
*/
void Cmd_SetCommandCompletionFunc( const char *command, completionFunc_t complete ) {
	cmd_function_t	*cmd;

	for( cmd = cmd_functions; cmd; cmd = cmd->next ) {
		if( !Q_stricmp( command, cmd->name ) ) {
			cmd->complete = complete;
			return;
		}
	}
}

/*
============
Cmd_RemoveCommand
============
*/
void	Cmd_RemoveCommand( const char *cmd_name ) {
	cmd_function_t	*cmd, **back;

	back = &cmd_functions;
	while( 1 ) {
		cmd = *back;
		if ( !cmd ) {
			// command wasn't active
			return;
		}
		if ( !strcmp( cmd_name, cmd->name ) ) {
			*back = cmd->next;
			Z_Free (cmd->name);
			Z_Free (cmd);
			return;
		}
		back = &cmd->next;
	}
}

/*
============
Cmd_RemoveCommandSafe

Only remove commands with no associated function
============
*/
void Cmd_RemoveCommandSafe( const char *cmd_name )
{
	cmd_function_t *cmd = Cmd_FindCommand( cmd_name );

	if( !cmd )
		return;
	if( cmd->function )
	{
		Com_Error( ERR_DROP, "Restricted source tried to remove "
			"system command \"%s\"", cmd_name );
		return;
	}

	Cmd_RemoveCommand( cmd_name );
}

/*
============
Cmd_CommandCompletion
============
*/
void	Cmd_CommandCompletion( void(*callback)(const char *s) ) {
	cmd_function_t	*cmd;
	
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next) {
		callback( cmd->name );
	}
}

/*
============
Cmd_CompleteArgument
============
*/
void Cmd_CompleteArgument( const char *command, char *args, int argNum ) {
	cmd_function_t	*cmd;

	for( cmd = cmd_functions; cmd; cmd = cmd->next ) {
		if( !Q_stricmp( command, cmd->name ) ) {
			if ( cmd->complete ) {
				cmd->complete( args, argNum );
			}
			return;
		}
	}
}


/*
============
Cmd_InList

Whether a command's name is one of a list's
============
*/
static qboolean Cmd_InList( const char *name, const char **list, int count ) {
	int		i;

	for ( i = 0; i < count; i++ ) {
		if ( !Q_stricmp( name, list[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
============
Cmd_ExecuteString

A complete command line has been parsed, so try to execute it
============
*/
void	Cmd_ExecuteString( const char *text ) {	
	cmd_function_t	*cmd, **prev;

	// execute the command line
	Cmd_TokenizeString( text );
	if ( !Cmd_Argc() ) {
		return;		// no tokens
	}

	// game code's text can't run the commands that send or print secrets,
	// or stop the process; nor can a pak's config on a dedicated server,
	// which sets what it likes but writes no file either
	{
		static const char *denied[] = { "rcon", "condump", "setenv", "error", "crash", "freeze" };
		// and the ban list's, which writes the file sv_banFile names
		static const char *writers[] = { "writeconfig", "settings_export", "settings_import",
			"banaddr", "bandel", "exceptaddr", "exceptdel", "flushbans" };

		if ( cmd_restricted && Cmd_InList( cmd_argv[0], denied, ARRAY_LEN( denied ) ) ) {
			Com_Printf( "%s can't be run by game code or game content.\n", cmd_argv[0] );
			return;
		}
		if ( Cmd_IsPakConfig() && ( Cmd_InList( cmd_argv[0], denied, ARRAY_LEN( denied ) ) ||
				Cmd_InList( cmd_argv[0], writers, ARRAY_LEN( writers ) ) ) ) {
			Com_Printf( "%s can't be run by a config in a pak (%s).\n", cmd_argv[0], Cmd_Origin() );
			return;
		}
	}

	// configs run again keep the map: their map, map_restart and vstr
	// (most rotations start with one) wait for the admin
	if ( cmd_holdMapCommands && cmd_script ) {
		static const char *held[] = { "map", "devmap", "spmap", "spdevmap", "map_restart", "vstr",
			"game_restart", "reload" };

		if ( Cmd_InList( cmd_argv[0], held, ARRAY_LEN( held ) ) ) {
			Com_Printf( "Held back, as configs run again keep the map: %s\n", Cmd_Cmd() );
			return;
		}
	}

	// check registered command functions	
	for ( prev = &cmd_functions ; *prev ; prev = &cmd->next ) {
		cmd = *prev;
		if ( !Q_stricmp( cmd_argv[0],cmd->name ) ) {
			// rearrange the links so that the command will be
			// near the head of the list next time it is used
			*prev = cmd->next;
			cmd->next = cmd_functions;
			cmd_functions = cmd;

			// perform the action
			if ( !cmd->function ) {
				// let the cgame or game handle it
				break;
			} else {
				cmd->function ();
			}
			return;
		}
	}
	
	// check cvars
	if ( Cvar_Command() ) {
		return;
	}

	// check client game commands
	if ( com_cl_running && com_cl_running->integer && CL_GameCommand() ) {
		return;
	}

	// check server game commands
	if ( com_sv_running && com_sv_running->integer && SV_GameCommand() ) {
		return;
	}

	// check ui commands
	if ( com_cl_running && com_cl_running->integer && UI_GameCommand() ) {
		return;
	}

	// send it as a server command if we are connected
	// this will usually result in a chat message
	CL_ForwardCommandToServer ( text );
}

/*
============
Cmd_List_f
============
*/
void Cmd_List_f (void)
{
	cmd_function_t	*cmd;
	int				i;
	char			*match;

	if ( Cmd_Argc() > 1 ) {
		match = Cmd_Argv( 1 );
	} else {
		match = NULL;
	}

	i = 0;
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next) {
		if (match && !Com_Filter(match, cmd->name, qfalse)) continue;

		Com_Printf ("%s\n", cmd->name);
		i++;
	}
	Com_Printf ("%i commands\n", i);
}

/*
==================
Cmd_CompleteCfgName
==================
*/
void Cmd_CompleteCfgName( char *args, int argNum ) {
	if( argNum == 2 ) {
		Field_CompleteFilename( "", "cfg", NULL, qfalse, qtrue );
	}
}

/*
============
Cmd_Init
============
*/
void Cmd_Init (void) {
	Cmd_AddCommand ("cmdlist",Cmd_List_f);
	Cmd_AddCommand ("exec",Cmd_Exec_f);
	Cmd_AddCommand ("execq",Cmd_Exec_f);
	Cmd_SetCommandCompletionFunc( "exec", Cmd_CompleteCfgName );
	Cmd_SetCommandCompletionFunc( "execq", Cmd_CompleteCfgName );
	Cmd_AddCommand ("vstr",Cmd_Vstr_f);
	Cmd_SetCommandCompletionFunc( "vstr", Cvar_CompleteCvarName );
	Cmd_AddCommand ("echo",Cmd_Echo_f);
	Cmd_AddCommand ("wait", Cmd_Wait_f);
}

