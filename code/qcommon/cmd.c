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

typedef struct {
	byte	*data;
	int		maxsize;
	int		cursize;
	int		wait;				// frames to wait before running more of it
	qboolean	inStarComment;	// within one Cbuf_Execute: see Cbuf_ExecuteLine
	qboolean	inSlashComment;
} cmd_t;

static cmd_t	cmd_text;		// the player's, the configs' and the engine's
static cmd_t	cmd_gameText;	// what game code queues, which runs restricted
static byte		cmd_text_buf[MAX_CMD_BUFFER];
static byte		cmd_gameText_buf[MAX_CMD_BUFFER];

// the running command is restricted: it came from game code, or from text
// that game code queued or ran
static qboolean	cmd_restricted;

// the buffer text a command adds goes to: text a restricted command adds
// stays restricted
static cmd_t *Cbuf_Current( void ) {
	return cmd_restricted ? &cmd_gameText : &cmd_text;
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
	cmd_text.data = cmd_text_buf;
	cmd_text.maxsize = MAX_CMD_BUFFER;
	cmd_text.cursize = 0;
	cmd_gameText.data = cmd_gameText_buf;
	cmd_gameText.maxsize = MAX_CMD_BUFFER;
	cmd_gameText.cursize = 0;
}

static void Cbuf_Add( cmd_t *buf, const char *text ) {
	int		l;

	l = strlen (text);

	if (buf->cursize + l >= buf->maxsize)
	{
		Com_Printf ("Cbuf_AddText: overflow\n");
		return;
	}
	Com_Memcpy(&buf->data[buf->cursize], text, l);
	buf->cursize += l;
}

static void Cbuf_Insert( cmd_t *buf, const char *text ) {
	int		len;
	int		i;

	len = strlen( text ) + 1;
	if ( len + buf->cursize > buf->maxsize ) {
		Com_Printf( "Cbuf_InsertText overflowed\n" );
		return;
	}

	// move the existing command text
	for ( i = buf->cursize - 1 ; i >= 0 ; i-- ) {
		buf->data[ i + len ] = buf->data[ i ];
	}

	// copy the new text in
	Com_Memcpy( buf->data, text, len - 1 );

	// add a \n
	buf->data[ len - 1 ] = '\n';

	buf->cursize += len;
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
	Cbuf_Add( restricted ? &cmd_gameText : &cmd_text, text );
}

void Cbuf_InsertTextRestricted( const char *text, qboolean restricted ) {
	Cbuf_Insert( restricted ? &cmd_gameText : &cmd_text, text );
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
}


/*
============
Cbuf_ExecuteTextWithRights

Cbuf_ExecuteText, restricted or not, whatever the running command is
============
*/
static void Cbuf_ExecuteTextWithRights( int exec_when, const char *text, qboolean restricted )
{
	qboolean	running = cmd_restricted;

	cmd_restricted = restricted;
	switch (exec_when)
	{
	case EXEC_NOW:
		if (text && strlen(text) > 0) {
			Com_DPrintf(S_COLOR_YELLOW "EXEC_NOW %s\n", text);
			Cmd_ExecuteString (text);
		} else {
			Cbuf_Execute();
			Com_DPrintf(S_COLOR_YELLOW "EXEC_NOW %s\n", cmd_text.data);
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
	int		quotes;
	qboolean	restricted;

	// find a \n or ; line break or comment: // or /* */
	// This will keep // style comments all on one line by not breaking on
	// a semicolon.  It will keep /* ... */ style comments all on one line by not
	// breaking it for semicolon or newline.
	text = (char *)buf->data;

	quotes = 0;
	for (i=0 ; i< buf->cursize ; i++)
	{
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

	if( i >= (MAX_CMD_LINE - 1)) {
		i = MAX_CMD_LINE - 1;
	}

	Com_Memcpy (line, text, i);
	line[i] = 0;

// delete the text from the command buffer and move remaining commands down
// this is necessary because commands (exec) can insert data at the
// beginning of the text buffer

	if (i == buf->cursize)
		buf->cursize = 0;
	else
	{
		i++;
		buf->cursize -= i;
		memmove (text, text+i, buf->cursize);
	}

// execute the command line, restricted if it's game code's

	restricted = cmd_restricted;
	cmd_restricted = buf == &cmd_gameText;
	Cmd_ExecuteString (line);
	cmd_restricted = restricted;
}

/*
============
Cbuf_Execute
============
*/
void Cbuf_Execute (void)
{
	// a comment runs to the end of what one call executes, as it always has
	cmd_text.inStarComment = cmd_text.inSlashComment = qfalse;
	cmd_gameText.inStarComment = cmd_gameText.inSlashComment = qfalse;

	// the player's commands first, then game code's, while neither waits
	for ( ;; ) {
		if ( cmd_text.cursize && cmd_text.wait <= 0 ) {
			Cbuf_ExecuteLine( &cmd_text );
		} else if ( cmd_gameText.cursize && cmd_gameText.wait <= 0 ) {
			Cbuf_ExecuteLine( &cmd_gameText );
		} else {
			break;
		}
	}

	// a buffer still holding text after a wait runs it a frame later
	if ( cmd_text.cursize && cmd_text.wait > 0 ) {
		cmd_text.wait--;
	}
	if ( cmd_gameText.cursize && cmd_gameText.wait > 0 ) {
		cmd_gameText.wait--;
	}
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
	qboolean quiet;
	union {
		char	*c;
		void	*v;
	} f;
	char	filename[MAX_QPATH];

	quiet = !Q_stricmp(Cmd_Argv(0), "execq");

	if (Cmd_Argc () != 2) {
		Com_Printf ("exec%s <filename> : execute a script file%s\n",
		            quiet ? "q" : "", quiet ? " without notification" : "");
		return;
	}

	Q_strncpyz( filename, Cmd_Argv(1), sizeof( filename ) );
	COM_DefaultExtension( filename, sizeof( filename ), ".cfg" );
	FS_ReadFile( filename, &f.v);
	if (!f.c) {
		Com_Printf ("couldn't exec %s\n", filename);
		return;
	}
	if (!quiet)
		Com_Printf ("execing %s\n", filename);

	// a config from a pk3 or pk3dir, which a download can bring, runs
	// restricted in the client; a dedicated server doesn't download.
	// The first configs run before com_dedicated exists
#ifdef DEDICATED
	Cbuf_InsertTextRestricted( f.c, Cmd_IsRestricted() );
#else
	Cbuf_InsertTextRestricted( f.c, Cmd_IsRestricted() ||
		( !Cvar_VariableIntegerValue( "dedicated" ) && FS_LastFileIsGameContent() ) );
#endif

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

	// with the rights of whoever set it: see Cvar_RunsRestricted
	v = Cvar_VariableString( Cmd_Argv( 1 ) );
	Cbuf_InsertTextRestricted( va("%s\n", v ), Cvar_RunsRestricted( Cmd_Argv( 1 ) ) );
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
	// or stop the process
	if ( cmd_restricted ) {
		static const char *denied[] = { "rcon", "condump", "setenv", "error", "crash", "freeze" };
		int		i;

		for ( i = 0; i < ARRAY_LEN( denied ); i++ ) {
			if ( !Q_stricmp( cmd_argv[0], denied[i] ) ) {
				Com_Printf( "%s can't be run by game code or game content.\n", cmd_argv[0] );
				return;
			}
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

