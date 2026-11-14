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
// cvar.c -- dynamic variable tracking

#include "q_shared.h"
#include "qcommon.h"
#include "hooks.h"

#include <float.h>

cvar_t		*cvar_vars = NULL;
cvar_t		*cvar_cheats;
int			cvar_modifiedFlags;

#define	MAX_CVARS	2048
cvar_t		cvar_indexes[MAX_CVARS];
int			cvar_numIndexes;

// where an int's range ends (Cvar_IntegralBounds), which help and cvar_dump
// read as no bound: INT_MAX as a float is 2^31, one past it, which a value
// held at the bound would overflow as it's written back as an int, so the
// largest float under 2^31; and at the other end its mirror, since a value
// that rounds to -2^31 would pass as it is and overflow atoi, and -2^31
// itself is written back as -2.14748365e+09
#define CVAR_INT_MIN	-2147483520.0f
#define CVAR_INT_MAX	2147483520.0f

#define FILE_HASH_SIZE		256
static	cvar_t	*hashTable[FILE_HASH_SIZE];

static const cvarDefault_t	*cvar_profile;	// this platform's defaults (Cvar_SetProfile)

static void Cvar_MarkSaved( const cvar_t *var );
cvar_t *Cvar_Unset( cvar_t *cv );
static const cvarDeclaration_t	*cvar_declared;	// the game's declarations (Cvar_SetDeclarations)
static int			cvar_numDeclared;
const cvarDeclaration_t *Cvar_Declaration( const char *var_name );
static void Cvar_ForgetModules( cvar_t *var );

/*
================
return a hash value for the filename
================
*/
static long generateHashValue( const char *fname ) {
	int		i;
	long	hash;
	char	letter;

	hash = 0;
	i = 0;
	while (fname[i] != '\0') {
		letter = tolower(fname[i]);
		hash+=(long)(letter)*(i+119);
		i++;
	}
	hash &= (FILE_HASH_SIZE-1);
	return hash;
}

/*
============
Cvar_ValidateString
============
*/
static qboolean Cvar_ValidateString( const char *s ) {
	if ( !s ) {
		return qfalse;
	}
	if ( strchr( s, '\\' ) ) {
		return qfalse;
	}
	if ( strchr( s, '\"' ) ) {
		return qfalse;
	}
	if ( strchr( s, ';' ) ) {
		return qfalse;
	}
	// a space or a control character would split the line the name is
	// written on in a config
	for ( ; *s; s++ ) {
		if ( (unsigned char)*s <= ' ' || *s == 0x7f ) {
			return qfalse;
		}
	}
	return qtrue;
}

/*
============
Cvar_FindVar

The cvar of that name, or NULL; never creates one
============
*/
cvar_t *Cvar_FindVar( const char *var_name ) {
	cvar_t	*var;
	long hash;

	hash = generateHashValue(var_name);
	
	for (var=hashTable[hash] ; var ; var=var->hashNext) {
		if (!Q_stricmp(var_name, var->name)) {
			return var;
		}
	}

	return NULL;
}

/*
============
Cvar_VariableValue
============
*/
float Cvar_VariableValue( const char *var_name ) {
	cvar_t	*var;
	
	var = Cvar_FindVar (var_name);
	if (!var)
		return 0;
	return var->value;
}


/*
============
Cvar_VariableIntegerValue
============
*/
int Cvar_VariableIntegerValue( const char *var_name ) {
	cvar_t	*var;
	
	var = Cvar_FindVar (var_name);
	if (!var)
		return 0;
	return var->integer;
}


/*
============
Cvar_VariableString
============
*/
char *Cvar_VariableString( const char *var_name ) {
	cvar_t *var;
	
	var = Cvar_FindVar (var_name);
	if (!var)
		return "";
	return var->string;
}


/*
============
Cvar_VariableStringBuffer
============
*/
void Cvar_VariableStringBuffer( const char *var_name, char *buffer, int bufsize ) {
	cvar_t *var;
	
	var = Cvar_FindVar (var_name);
	if (!var) {
		*buffer = 0;
	}
	else {
		Q_strncpyz( buffer, var->string, bufsize );
	}
}

/*
============
Cvar_VariableValueSafe, Cvar_VariableIntegerValueSafe,
Cvar_VariableStringBufferSafe

Reads for game code: a cvar it may not read (VM_PrivateCvarFlag) reads
as one that doesn't exist
============
*/
float Cvar_VariableValueSafe( const char *var_name ) {
	if ( Cvar_Flags( var_name ) & VM_PrivateCvarFlag() ) {
		return 0;
	}
	return Cvar_VariableValue( var_name );
}

int Cvar_VariableIntegerValueSafe( const char *var_name ) {
	if ( Cvar_Flags( var_name ) & VM_PrivateCvarFlag() ) {
		return 0;
	}
	return Cvar_VariableIntegerValue( var_name );
}

void Cvar_VariableStringBufferSafe( const char *var_name, char *buffer, int bufsize ) {
	if ( Cvar_Flags( var_name ) & VM_PrivateCvarFlag() ) {
		Q_strncpyz( buffer, "", bufsize );
		return;
	}
	Cvar_VariableStringBuffer( var_name, buffer, bufsize );
}

/*
============
Cvar_Flags
============
*/
int Cvar_Flags(const char *var_name)
{
	cvar_t *var;
	
	if(!(var = Cvar_FindVar(var_name)))
		return CVAR_NONEXISTENT;
	else
	{
		if(var->modified)
			return var->flags | CVAR_MODIFIED;
		else
			return var->flags;
	}
}

/*
============
Cvar_CommandCompletion
============
*/
void Cvar_CommandCompletion(void (*callback)(const char *s))
{
	cvar_t		*cvar;
	
	for(cvar = cvar_vars; cvar; cvar = cvar->next)
	{
		if(cvar->name)
			callback(cvar->name);
	}
}

/*
============
Cvar_FormatValue

A number as a cvar's value: a whole one as an integer, and else with the
fewest decimals that read back as the same float: 0.8 is "0.8", as a
default of 0.8 is written, not "0.800000". A float reads back from 9
significant digits, so one of 1 or more needs at most 9 decimals, and a
smaller one its leading zeros too, as many as fit ("-0." and the end)
============
*/
static void Cvar_FormatValue( char *buf, int size, float value ) {
	int	decimals;

	// past an int (an open end of a float's range, FLT_MAX), where the
	// cast Q_isintegral makes is undefined: 9 significant digits
	if ( !( value > -2147483648.0f && value < 2147483648.0f ) ) {
		Com_sprintf( buf, size, "%.9g", value );
		return;
	}
	if ( Q_isintegral( value ) ) {
		Com_sprintf( buf, size, "%i", (int)value );
		return;
	}
	for ( decimals = 1; decimals <= 48 && decimals + 4 <= size; decimals++ ) {
		Com_sprintf( buf, size, "%.*f", decimals, value );
		if ( (float)atof( buf ) == value ) {
			return;
		}
	}
}

static qboolean Cvar_NamedValue( const cvar_t *var, const char *value, char *words, int size );

/*
============
Cvar_Validate

A value as the cvar takes it: a number held in its range, or an enum's
name it doesn't have (Cvar_NamedValue) taken as its default
============
*/
static const char *Cvar_Validate( cvar_t *var,
    const char *value, qboolean warn )
{
	static char s[ MAX_CVAR_VALUE_STRING ];
	char words[ MAX_STRING_CHARS ];
	float valuef;
	qboolean changed = qfalse;

	if( !value )
		return value;

	if( !var->validate )
	{
		if( Cvar_NamedValue( var, value, words, sizeof( words ) ) )
			return value;

		if( warn )
			Com_Printf( "WARNING: cvar '%s' takes %s, setting to %s\n", var->name, words, var->resetString );

		return var->resetString;
	}

	if( Q_isanumber( value ) )
	{
		valuef = atof( value );

		// whole numbers only, within the range: one outside it is held at
		// the bound below instead, since an int can't hold every float
		if( var->integral && valuef >= var->min && valuef <= var->max )
		{
			if( !Q_isintegral( valuef ) )
			{
				if( warn )
					Com_Printf( "WARNING: cvar '%s' must be integral", var->name );

				valuef = (int)valuef;
				changed = qtrue;
			}
		}
	}
	else
	{
		if( warn )
			Com_Printf( "WARNING: cvar '%s' must be numeric", var->name );

		valuef = atof( var->resetString );
		changed = qtrue;
	}

	if( valuef < var->min )
	{
		if( warn )
		{
			if( changed )
				Com_Printf( " and is" );
			else
				Com_Printf( "WARNING: cvar '%s'", var->name );

			if( Q_isintegral( var->min ) )
				Com_Printf( " out of range (min %d)", (int)var->min );
			else
				Com_Printf( " out of range (min %f)", var->min );
		}

		valuef = var->min;
		changed = qtrue;
	}
	else if( valuef > var->max )
	{
		if( warn )
		{
			if( changed )
				Com_Printf( " and is" );
			else
				Com_Printf( "WARNING: cvar '%s'", var->name );

			if( Q_isintegral( var->max ) )
				Com_Printf( " out of range (max %d)", (int)var->max );
			else
				Com_Printf( " out of range (max %f)", var->max );
		}

		valuef = var->max;
		changed = qtrue;
	}

	if( changed )
	{
		Cvar_FormatValue( s, sizeof( s ), valuef );

		if( warn )
			Com_Printf( ", setting to %s\n", s );

		return s;
	}
	else
		return value;
}

/*
============
Cvar_SetLayer

Replaces one of a cvar's layers (cvar_t's serverString and the rest), or
another string of its; NULL clears it
============
*/
static void Cvar_SetLayer( char **layer, const char *value ) {
	char *copy = value ? CopyString( value ) : NULL;

	if ( *layer ) {
		Z_Free( *layer );
	}
	*layer = copy;
}

/*
============
Cvar_ClearUser

Drops the player's value, and where it came from, a pak's config included
============
*/
static void Cvar_ClearUser( cvar_t *var ) {
	Cvar_SetLayer( &var->userString, NULL );
	Cvar_SetLayer( &var->userOrigin, NULL );
	var->userConfigLine = qfalse;
	var->pakSet = qfalse;
}

/*
============
Cvar_ForgetUser

Drops the player's layer and its saved copy, to apply after (Cvar_Apply)
============
*/
static void Cvar_ForgetUser( cvar_t *var ) {
	Cvar_ClearUser( var );
	var->userSource = CVAR_SOURCE_DEFAULT;
	if ( var->savedString ) {
		Cvar_SetLayer( &var->savedString, NULL );
		Cvar_MarkSaved( var );
	}
}

/*
============
Cvar_SameString

Whether two of a cvar's strings, either of which may be NULL, are the same
============
*/
static qboolean Cvar_SameString( const char *a, const char *b ) {
	return a ? b && !strcmp( a, b ) : !b;
}

// a dedicated server's layers (cvarServerLayer_t): whose each is, what
// cvar_why calls one with no origin, and cvar_dump's names for it
static const struct {
	cvarSource_t	source;
	const char		*unnamed;
	const char		*dump, *dumpOrigin;
} cvar_serverLayers[CVAR_SERVER_LAYERS] = {
	[CVAR_LAYER_ENTRY] = { CVAR_SOURCE_ENTRY, "the pool's entry", "entry", "entryOrigin" },
	[CVAR_LAYER_FILE] = { CVAR_SOURCE_FILE, "the server's file", "file", "fileOrigin" },
	[CVAR_LAYER_MODE] = { CVAR_SOURCE_MODE, "the mode", "mode", "modeOrigin" }
};

// a source's layer among the server's, or -1
static int Cvar_ServerLayer( cvarSource_t source ) {
	int	i;

	for ( i = 0; i < CVAR_SERVER_LAYERS; i++ ) {
		if ( cvar_serverLayers[i].source == source ) {
			return i;
		}
	}
	return -1;
}

/*
============
Cvar_UnderUser

What a cvar's value is without the player's or a server's: the map pool's
entry, the server's file, the entry's mode or the default, the first that
has one; and whose that is
============
*/
const char *Cvar_UnderUser( const cvar_t *var, cvarSource_t *source ) {
	int	i;

	for ( i = 0; i < CVAR_SERVER_LAYERS && !var->layerStrings[i]; i++ ) {
	}
	if ( source ) {
		*source = i < CVAR_SERVER_LAYERS ? cvar_serverLayers[i].source : CVAR_SOURCE_DEFAULT;
	}
	return i < CVAR_SERVER_LAYERS ? var->layerStrings[i] : var->resetString;
}

/*
============
Cvar_Resolve

A cvar's value, from its layers (cvar_t)
============
*/
static const char *Cvar_Resolve( const cvar_t *var ) {
	if ( var->serverString ) {
		return var->serverString;
	}
	if ( var->userString ) {
		return var->userString;
	}
	return Cvar_UnderUser( var, NULL );
}

/*
============
Cvar_Apply, Cvar_ApplyQuietly

Gives a cvar the value its layers resolve to: at once if forced, or else,
for a latched cvar, after a restart, which Cvar_Apply says. A map pool's
mode or entry sets a latched value as the map loads (Hooks_MapLoading),
and the load applies it, so Cvar_ApplyQuietly says nothing of a restart
============
*/
static cvar_t *Cvar_ApplyQuietly( cvar_t *var, qboolean force, qboolean quiet ) {
	const char *value = Cvar_Resolve( var );

	if ( !strcmp( value, var->string ) ) {
		Cvar_SetLayer( &var->latchedString, NULL );
		return var;
	}

	// note what types of cvars have been modified (userinfo, serverinfo,
	// systeminfo); the archive is the saved value's (Cvar_SetVar)
	cvar_modifiedFlags |= var->flags & ~CVAR_ARCHIVE;

	if ( !force && ( var->flags & CVAR_LATCH ) ) {
		if ( !Cvar_SameString( value, var->latchedString ) ) {
			if ( !quiet ) {
				Com_Printf( "%s will be changed upon restarting.\n", var->name );
			}
			Cvar_SetLayer( &var->latchedString, value );
			var->modified = qtrue;
			var->modificationCount++;
		}
		return var;
	}

	Cvar_SetLayer( &var->latchedString, NULL );
	var->modified = qtrue;
	var->modificationCount++;
	Cvar_SetLayer( &var->string, value );
	var->value = atof( var->string );
	var->integer = atoi( var->string );

	return var;
}

static cvar_t *Cvar_Apply( cvar_t *var, qboolean force ) {
	return Cvar_ApplyQuietly( var, force, qfalse );
}

/*
============
Cvar_SameValue

Whether two values are the same, as numbers if both are: "0.800000" is
"0.8"
============
*/
qboolean Cvar_SameValue( const char *a, const char *b ) {
	return !strcmp( a, b ) || ( Q_isanumber( a ) && Q_isanumber( b ) && atof( a ) == atof( b ) );
}

/*
============
Cvar_SavedValue

What the config gets for a cvar, if it's archived: the player's choice,
where it isn't the default, as a number too (Cvar_SameValue), so a
value written as "0.800000" by an older engine gives way to a later
default. Below a server file's value (CVAR_LAYER_FILE), that value is the
default: a choice of the built-in default over it is kept. Over a mode's
or a pool entry's, which change from map to map, any choice is kept: a
ctf map's timelimit 0 over its mode's 15 is a choice, though 0 is the
default. One no code has registered has no default yet, and its value is
kept as it is
============
*/
const char *Cvar_SavedValue( const cvar_t *var ) {
	if ( !var->savedString ) {
		return NULL;
	}
	// the entry's is above the file's, so the file's value is a choice there
	if ( var->layerStrings[CVAR_LAYER_ENTRY] ) {
		return var->savedString;
	}
	if ( var->layerStrings[CVAR_LAYER_FILE] ) {
		return Cvar_SameValue( var->savedString, var->layerStrings[CVAR_LAYER_FILE] ) ? NULL : var->savedString;
	}
	if ( var->layerStrings[CVAR_LAYER_MODE] ) {
		return var->savedString;
	}
	return !( var->flags & CVAR_USER_CREATED ) && Cvar_SameValue( var->savedString, var->resetString ) ?
		NULL : var->savedString;
}

/*
============
Cvar_ProfileDefault

A cvar's default on this platform: its profile's, if it has one
============
*/
static const char *Cvar_ProfileDefault( const char *var_name, const char *var_value ) {
	const cvarDefault_t *d;

	for ( d = cvar_profile; d && d->name; d++ ) {
		if ( !Q_stricmp( d->name, var_name ) ) {
			return d->value;
		}
	}
	return var_value;
}


/*
============
Cvar_Get

If the variable already exists, the value will not be set unless CVAR_ROM
The flags will be or'ed in if the variable exists.
============
*/
cvar_t *Cvar_Get( const char *var_name, const char *var_value, int flags ) {
	cvar_t	*var;
	long	hash;
	int	index;

	if ( !var_name || ! var_value ) {
		Com_Error( ERR_FATAL, "Cvar_Get: NULL parameter" );
	}

	if ( !Cvar_ValidateString( var_name ) ) {
		Com_Printf("invalid cvar name string: %s\n", var_name );
		var_name = "BADNAME";
	}

#if 0		// FIXME: values with backslash happen
	if ( !Cvar_ValidateString( var_value ) ) {
		Com_Printf("invalid cvar value string: %s\n", var_value );
		var_value = "BADVALUE";
	}
#endif

	// this platform's default, for a registration by code
	if ( !( flags & ( CVAR_USER_CREATED | CVAR_SERVER_CREATED ) ) ) {
		var_value = Cvar_ProfileDefault( var_name, var_value );
	}

	var = Cvar_FindVar (var_name);
	
	if(var)
	{
		// whether it had a saved value, which registering can drop
		qboolean	saved = var->savedString != NULL;

		// engine code taking over a cvar game code created drops the range
		// the module gave it (Cvar_SetRangeByName), before that range could
		// bend the engine's default, and keeps its description as its own
		if ( ( var->flags & CVAR_VM_CREATED ) && !( flags & CVAR_VM_CREATED ) ) {
			var->validate = qfalse;
			Cvar_ForgetModules( var );
		}

		var_value = Cvar_Validate(var, var_value, qfalse);

		// a protected or private cvar that game code, a server or restricted
		// text created before the engine registered it takes the engine's
		// value, as a read only one does
		if ( ( flags & ( CVAR_PROTECTED | CVAR_PRIVATE ) ) && !( flags & CVAR_VM_CREATED ) &&
			var->untrusted ) {
			if ( strcmp( var->string, var_value ) ) {
				Com_Printf( "%s can't be set by game code or game content.\n", var_name );
			}
			Cvar_SetLayer( &var->resetString, var_value );
			Cvar_SetLayer( &var->serverString, NULL );
			Cvar_ClearUser( var );
			Cvar_SetLayer( &var->savedString, NULL );
			var->untrusted = qfalse;
		}

		// Make sure the game code cannot mark engine-added variables as gamecode vars
		if(var->flags & CVAR_VM_CREATED)
		{
			if(!(flags & CVAR_VM_CREATED))
				var->flags &= ~CVAR_VM_CREATED;
		}
		else if (!(var->flags & CVAR_USER_CREATED))
		{
			if(flags & CVAR_VM_CREATED)
				flags &= ~CVAR_VM_CREATED;
		}

		// if the C code is now specifying a variable that the user already
		// set a value for, take the new value as the reset value
		if(var->flags & CVAR_USER_CREATED)
		{
			var->flags &= ~CVAR_USER_CREATED;
			Cvar_SetLayer( &var->resetString, var_value );

			if(flags & CVAR_ROM)
			{
				// this variable was set by the user,
				// so force it to value given by the engine.
				Cvar_ClearUser( var );
				Cvar_SetLayer( &var->savedString, NULL );
			}
		}
		
		// Make sure servers cannot mark engine-added variables as SERVER_CREATED
		if(var->flags & CVAR_SERVER_CREATED)
		{
			if(!(flags & CVAR_SERVER_CREATED))
			{
				// code registers it: the server's value is the server's,
				// until it goes, and the code's default the default
				var->flags &= ~CVAR_SERVER_CREATED;
				if ( !var->serverString )
					Cvar_SetLayer( &var->serverString, var->resetString );
				Cvar_SetLayer( &var->resetString, var_value );
			}
		}
		else
		{
			if(flags & CVAR_SERVER_CREATED)
				flags &= ~CVAR_SERVER_CREATED;
		}
		
		var->flags |= flags;
		// a read only cvar's value is state, never the player's to save
		if ( flags & CVAR_ROM )
			Cvar_SetLayer( &var->savedString, NULL );

		// only allow one non-empty reset string without a warning
		if ( !var->resetString[0] ) {
			// we don't have a reset string yet
			Cvar_SetLayer( &var->resetString, var_value );
		} else if ( var_value[0] && strcmp( var->resetString, var_value ) ) {
			Com_DPrintf( "Warning: cvar \"%s\" given initial values: \"%s\" and \"%s\"\n",
				var_name, var->resetString, var_value );
		}
		// its value from its layers, now that its default is known, and a
		// latched value takes effect now
		Cvar_Apply( var, qtrue );

		// ZOID--needs to be set so that cvars the game sets as
		// SERVERINFO get sent to clients; a saved value may now be saved
		// otherwise (archived, in another scope's file, the default, or
		// not at all, read only), which the next write compares
		cvar_modifiedFlags |= flags & ~CVAR_ARCHIVE;
		if ( saved || var->savedString ) {
			Cvar_MarkSaved( var );
		}

		return var;
	}

	//
	// allocate a new cvar
	//

	// find a free cvar
	for(index = 0; index < MAX_CVARS; index++)
	{
		if(!cvar_indexes[index].name)
			break;
	}

	if(index >= MAX_CVARS)
	{
		if(!com_errorEntered)
			Com_Error(ERR_DROP, "Error: Too many cvars, cannot create a new one!");

		return NULL;
	}
	
	var = &cvar_indexes[index];
	
	if(index >= cvar_numIndexes)
		cvar_numIndexes = index + 1;
		
	var->name = CopyString (var_name);
	var->string = CopyString (var_value);
	var->modified = qtrue;
	var->modificationCount = 1;
	var->value = atof (var->string);
	var->integer = atoi(var->string);
	var->resetString = CopyString( var_value );
	var->serverString = var->userString = var->userOrigin = var->savedString = NULL;
	var->lockOrigin = var->lockString = NULL;
	Com_Memset( var->layerStrings, 0, sizeof( var->layerStrings ) );
	Com_Memset( var->layerOrigins, 0, sizeof( var->layerOrigins ) );
	var->userSource = CVAR_SOURCE_DEFAULT;
	var->declaredScope = Cvar_DeclaredScope( var_name );
	var->serverStale = qfalse;
	var->validate = qfalse;
	var->description = NULL;
	// its value is game code's or a server's: see cvar_t's untrusted
	var->untrusted = ( flags & ( CVAR_VM_CREATED | CVAR_SERVER_CREATED ) ) != 0;

	// link the variable in
	var->next = cvar_vars;
	if(cvar_vars)
		cvar_vars->prev = var;

	var->prev = NULL;
	cvar_vars = var;

	var->flags = flags;
	// note what types of cvars have been modified (userinfo, serverinfo,
	// systeminfo); nothing of it is saved yet
	cvar_modifiedFlags |= var->flags & ~CVAR_ARCHIVE;

	hash = generateHashValue(var_name);
	var->hashIndex = hash;

	var->hashNext = hashTable[hash];
	if(hashTable[hash])
		hashTable[hash]->hashPrev = var;

	var->hashPrev = NULL;
	hashTable[hash] = var;

	return var;
}

/*
============
Cvar_Source
============
*/
cvarSource_t Cvar_Source( const cvar_t *var ) {
	cvarSource_t	source;

	if ( var->serverString ) {
		return CVAR_SOURCE_SERVER;
	}
	if ( var->userString ) {
		return var->userSource;
	}
	Cvar_UnderUser( var, &source );
	return source;
}

// whose value it is, as printing a cvar says it, and as cvar_dump does
static const struct {
	const char	*whose;
	const char	*name;
} cvar_sources[] = {
	[CVAR_SOURCE_DEFAULT] = { NULL, "default" },
	[CVAR_SOURCE_ENGINE] = { "the engine's", "engine" },
	[CVAR_SOURCE_GAME] = { "game code's", "game" },
	[CVAR_SOURCE_PLAYER] = { "yours", "player" },
	[CVAR_SOURCE_MENU] = { "the menus'", "menu" },
	[CVAR_SOURCE_SCRIPT] = { "a script's", "script" },
	[CVAR_SOURCE_SESSION] = { "the command line's", "command line" },
	[CVAR_SOURCE_SERVER] = { "the server's", "server" },
	[CVAR_SOURCE_SYSTEM] = { "the system's", "system" },
	[CVAR_SOURCE_TOOL] = { "a tool's", "tool" },
	[CVAR_SOURCE_FILE] = { "the server file's", "file" },
	[CVAR_SOURCE_ENTRY] = { "the pool entry's", "pool entry" },
	[CVAR_SOURCE_MODE] = { "the mode's", "mode" }
};

/*
============
Cvar_StripColors

Drops the color codes from a text in place, keeping its line breaks,
which Q_CleanStr would drop too
============
*/
static void Cvar_StripColors( char *text ) {
	char	*from, *to;

	for ( from = to = text; *from; from++ ) {
		if ( Q_IsColorString( from ) ) {
			from++;
		} else {
			*to++ = *from;
		}
	}
	*to = '\0';
}

/*
============
Cvar_ValueLine

Whether a line of a cvar's description, from its third on, names a value:
"<value> = <label>", with ": <meaning>" after it or not, the way CNQ3's
help writes them, spaces before it allowed. Splits the line into its
parts, meaning NULL if it has none
============
*/
static qboolean Cvar_ValueLine( char *line, char **value, char **label, char **meaning ) {
	char	*s = line, *valueEnd;

	while ( *s == ' ' ) {
		s++;
	}
	*value = s;
	while ( *s && *s != ' ' && *s != '=' ) {
		s++;
	}
	valueEnd = s;
	while ( *s == ' ' ) {
		s++;
	}
	if ( valueEnd == *value || *s != '=' ) {
		return qfalse;
	}
	for ( s++; *s == ' '; s++ ) {
	}
	if ( !*s ) {
		return qfalse;
	}
	*valueEnd = '\0';
	*label = s;
	*meaning = strstr( s, ": " );
	if ( *meaning ) {
		**meaning = '\0';
		*meaning += 2;
	}
	return qtrue;
}

// a cvar's description, read a line at a time (Cvar_NextLine)
typedef struct {
	const char	*text;		// what's left
	int			number;		// the line's, from 1
	char		line[MAX_STRING_CHARS];
	char		*value, *label, *meaning;	// a value line's parts; value NULL on any other
} cvarTextReader_t;

static void Cvar_StartReading( cvarTextReader_t *reader, const char *description ) {
	reader->text = description ? description : "";
	reader->number = 0;
}

/*
============
Cvar_NextLine

A cvar's description's next line, without its line break; qfalse at the
end. The first line is the summary, the second the explanation, and each
after it a value (Cvar_ValueLine) or more explanation
============
*/
static qboolean Cvar_NextLine( cvarTextReader_t *reader ) {
	const char	*end;
	int			length;

	if ( !*reader->text ) {
		return qfalse;
	}
	end = strchr( reader->text, '\n' );
	length = end ? end - reader->text : strlen( reader->text );
	Q_strncpyz( reader->line, reader->text, MIN( length + 1, sizeof( reader->line ) ) );
	reader->text += end ? length + 1 : length;
	reader->number++;
	if ( reader->number < 3 || !Cvar_ValueLine( reader->line, &reader->value, &reader->label, &reader->meaning ) ) {
		reader->value = NULL;
	}
	return qtrue;
}

/*
============
Cvar_NamedValue

Whether a cvar takes a value as one of its names: an enum of named
values, which its declaration calls an enum and which has no range and a
default that isn't a number, takes only the values its description names
(Cvar_ValueLine), as Omniwire's settings.set does, and its default. Its
names in words
("auto or always") into words; any other cvar takes any value, words
empty
============
*/
static qboolean Cvar_NamedValue( const cvar_t *var, const char *value, char *words, int size ) {
	const cvarDeclaration_t	*declaration;
	cvarTextReader_t		reader;
	qboolean				found = qfalse;
	int						count = 0, last = 0;
	char					name[MAX_STRING_CHARS];

	words[0] = '\0';
	// the cheap checks first: every set of a cvar with no range comes here
	if ( var->validate || Q_isanumber( var->resetString ) ) {
		return qtrue;
	}
	declaration = Cvar_Declaration( var->name );
	if ( !declaration || !declaration->type || strcmp( declaration->type, "enum" ) ) {
		return qtrue;
	}
	// no va(): a caller's value may be in its buffers
	for ( Cvar_StartReading( &reader, var->description ); Cvar_NextLine( &reader ); ) {
		if ( !reader.value ) {
			continue;
		}
		found |= !strcmp( value, reader.value );
		last = strlen( words );
		if ( count++ ) {
			Q_strcat( words, size, ", " );
		}
		Q_strcat( words, size, reader.value );
	}
	// "a, b or c"
	if ( count > 1 && !strncmp( words + last, ", ", 2 ) ) {
		Q_strncpyz( name, words + last + 2, sizeof( name ) );
		words[last] = '\0';
		Q_strcat( words, size, " or " );
		Q_strcat( words, size, name );
	}
	// its default too, whatever its names say
	return found || !count || !strcmp( value, var->resetString );
}

/*
============
Cvar_PrintSource

Whose value a cvar has, after it, unless it's the default
============
*/
static void Cvar_PrintSource( const cvar_t *var, void (QDECL *print)( const char *fmt, ... ) ) {
	const char *name = cvar_sources[ Cvar_Source( var ) ].whose;

	if ( name ) {
		print( " (%s)", name );
	}
}

/*
============
Cvar_Print

Prints the value, whose it is, the default, and latched string of the given variable
============
*/
static void Cvar_PrintWith( cvar_t *v, void (QDECL *print)( const char *fmt, ... ) ) {
	print ("\"%s\" is:\"%s" S_COLOR_WHITE "\"",
			v->name, v->string );
	Cvar_PrintSource( v, print );

	if ( !( v->flags & CVAR_ROM ) ) {
		if ( !Q_stricmp( v->string, v->resetString ) ) {
			print (", the default" );
		} else {
			print (" default:\"%s" S_COLOR_WHITE "\"",
					v->resetString );
		}
	}

	print ("\n");

	if ( v->latchedString ) {
		print( "latched: \"%s\"\n", v->latchedString );
	}

	// the summary, its description's first line (Cvar_NextLine); help
	// prints the rest
	if ( v->description ) {
		const char	*rest = strchr( v->description, '\n' );

		print( "%.*s\n", (int)strcspn( v->description, "\n" ), v->description );
		if ( rest && rest[1] ) {
			print( "help %s for more\n", v->name );
		}
	}
}

void Cvar_Print( cvar_t *v ) {
	Cvar_PrintWith( v, Com_Printf );
}

/*
============
Cvar_Refusal

Why a set that isn't forced leaves a cvar alone: it is read only, can't
be set after startup, or is cheat protected with cheats off. NULL if none
============
*/
static const char *Cvar_Refusal( const cvar_t *var ) {
	if ( var->flags & CVAR_ROM ) {
		return "read only";
	}
	if ( var->flags & CVAR_INIT ) {
		return "write protected";
	}
	if ( ( var->flags & CVAR_CHEAT ) && !cvar_cheats->integer ) {
		return "cheat protected";
	}
	return NULL;
}

/*
============
Cvar_Set
============
*/
void Cvar_Set( const char *var_name, const char *value) {
	Cvar_Set2 (var_name, value, qtrue);
}

/*
============
Cvar_SetSafe
============
*/
static void Cvar_CheckSafeSet( const char *var_name, const char *value )
{
	int flags = Cvar_Flags( var_name );

	if ( flags == CVAR_NONEXISTENT )
		return;

	// a private cvar can't be set either, but for a userinfo one the
	// player types in, password, which a server browser's field writes
	if( ( flags & CVAR_PROTECTED ) ||
		( ( flags & CVAR_PRIVATE ) && ( flags & ( CVAR_USERINFO | CVAR_ROM ) ) != CVAR_USERINFO ) )
	{
		if( value )
			Com_Error( ERR_DROP, "Restricted source tried to set "
				"\"%s\" to \"%s\"", var_name, value );
		else
			Com_Error( ERR_DROP, "Restricted source tried to "
				"modify \"%s\"", var_name );
	}
}

/*
============
Cvar_SetUntrusted

A set by game code or a server, which marks the cvar untrusted
============
*/
static void Cvar_SetUntrusted( const char *var_name, const char *value, qboolean force, cvarSource_t source )
{
	cvar_t	*var = Cvar_SetFrom( var_name, value, source, force );

	if ( var ) {
		var->untrusted = qtrue;
	}
}

void Cvar_SetSafe( const char *var_name, const char *value )
{
	Cvar_CheckSafeSet( var_name, value );
	Cvar_SetUntrusted( var_name, value, qtrue, CVAR_SOURCE_GAME );
}


// told of each set that changes a cvar's player or file value (Cvar_SetChangeHook)
static void ( *cvar_changeHook )( cvar_t *var, const char *old, cvarSource_t source );

// where the set under way came from (Cvar_SetFromOrigin), noted as it takes
static const char *cvar_setOrigin;

/*
============
Cvar_NextValue
============
*/
const char *Cvar_NextValue( const cvar_t *var ) {
	return var->latchedString ? var->latchedString : var->string;
}

// what the change hook hears was the value before a set: the latched value
// waiting, if one was, as that's the one a latched set replaces
static char *Cvar_HookOld( const cvar_t *var ) {
	return CopyString( Cvar_NextValue( var ) );
}

/*
============
Cvar_SetVar

Sets a cvar in its source's layer (NULL clears it) and gives it the value
its layers resolve to. The player's own value is the latest of their
changes, the command line's or theirs, and only theirs is saved; a
dedicated server's file's (CVAR_SOURCE_FILE) is below it, never saved. A
set that isn't forced is refused (Cvar_Refusal), leaving the layers as
they were. A cvar the command line or the environment locked (Cvar_Lock)
refuses a tool's set, and the console's and a script's last for this run,
unsaved. The set's origin is noted before the change hook hears of it
============
*/
static cvar_t *Cvar_SetVar( cvar_t *var, const char *value, cvarSource_t source, qboolean force ) {
	int	serverLayer = Cvar_ServerLayer( source );
	char **layer = source == CVAR_SOURCE_SERVER ? &var->serverString :
		serverLayer >= 0 ? &var->layerStrings[serverLayer] : &var->userString;
	qboolean saves, locked = var->lockOrigin &&
		( source == CVAR_SOURCE_PLAYER || source == CVAR_SOURCE_TOOL || source == CVAR_SOURCE_SCRIPT );
	char *old, words[MAX_STRING_CHARS];

	// an enum's name it doesn't have is refused, as a read only cvar's set
	// is, where an engine's forced set takes its default (Cvar_Validate)
	if ( value && !force && !Cvar_NamedValue( var, value, words, sizeof( words ) ) ) {
		Com_Printf( "%s takes %s, not \"%s\".\n", var->name, words, value );
		return var;
	}
	if ( value ) {
		value = Cvar_Validate( var, value, qtrue );
	}
	// a read only cvar's value is state, never saved, and can be large (a
	// server's pak lists)
	saves = source != CVAR_SOURCE_SCRIPT && source != CVAR_SOURCE_SESSION && source != CVAR_SOURCE_SERVER &&
		source != CVAR_SOURCE_SYSTEM && serverLayer < 0 && !locked &&
		!( var->flags & CVAR_ROM ) && !Cvar_SameString( var->savedString, value );
	if ( source == CVAR_SOURCE_SERVER ) {
		var->serverStale = qfalse;
	}

	if ( Cvar_SameString( *layer, value ) && !saves ) {
		return var;		// not changed
	}

	if ( !force ) {
		const char *refusal = Cvar_Refusal( var );

		if ( refusal ) {
			// quietly, as stock, when it already has that value
			if ( strcmp( value ? value : var->resetString, var->string ) ) {
				Com_Printf( "%s is %s.\n", var->name, refusal );
			}
			return var;
		}
		if ( locked && source == CVAR_SOURCE_TOOL ) {
			Com_Printf( "%s is set by %s, which tools can't change.\n", var->name, var->lockOrigin );
			return var;
		}
		if ( locked && value && strcmp( value, var->string ) ) {
			if ( cvar_setOrigin ) {
				Com_Printf( "%s sets %s over %s's value: this lasts until the next start sets it again.\n",
					cvar_setOrigin, var->name, var->lockOrigin );
			} else {
				Com_Printf( "%s is set by %s: this change lasts until the next start sets it again.\n",
					var->name, var->lockOrigin );
			}
		}
	}

	old = cvar_changeHook && source != CVAR_SOURCE_SERVER && source != CVAR_SOURCE_SYSTEM ? Cvar_HookOld( var ) : NULL;
	Cvar_SetLayer( layer, value );
	if ( serverLayer >= 0 ) {
		Cvar_SetLayer( &var->layerOrigins[serverLayer], value ? cvar_setOrigin : NULL );
		// an admin's value now, so the loaded map's own gives way to it at
		// once, as the next load's would (Cvar_SetGameValue)
		if ( value && var->userString && var->userSource == CVAR_SOURCE_GAME ) {
			Cvar_ForgetUser( var );
		}
	} else if ( layer == &var->userString ) {
		var->userSource = source;
		Cvar_SetLayer( &var->userOrigin, NULL );
		var->userConfigLine = qfalse;
		// a set's, not a reset's, which leaves no value to have come from it
		if ( cvar_setOrigin && value ) {
			Cvar_NoteOrigin( var, cvar_setOrigin );
		}
	}
	if ( saves ) {
		Cvar_SetLayer( &var->savedString, value );
		Cvar_MarkSaved( var );
	}
	Cvar_ApplyQuietly( var, force, source == CVAR_SOURCE_ENTRY || source == CVAR_SOURCE_MODE );
	if ( old ) {
		cvar_changeHook( var, old, source );
		Z_Free( old );
	}
	return var;
}

/*
============
Cvar_SetFrom
============
*/
cvar_t *Cvar_SetFrom( const char *var_name, const char *value, cvarSource_t source, qboolean force ) {
	cvar_t	*var;
	int	flags = 0;

//	Com_DPrintf( "Cvar_SetFrom: %s %s\n", var_name, value );

	if ( !Cvar_ValidateString( var_name ) ) {
		Com_Printf("invalid cvar name string: %s\n", var_name );
		var_name = "BADNAME";
	}

#if 0	// FIXME
	if ( value && !Cvar_ValidateString( value ) ) {
		Com_Printf("invalid cvar value string: %s\n", value );
		var_value = "BADVALUE";
	}
#endif

	if ( source == CVAR_SOURCE_SERVER ) {
		Cvar_CheckSafeSet( var_name, value );
		flags = CVAR_SERVER_CREATED | CVAR_ROM;
	} else if ( !force ) {
		flags = CVAR_USER_CREATED;
	}

	var = Cvar_FindVar (var_name);
	if ( !var ) {
		if ( !value ) {
			return NULL;
		}
		// create it, the player's with their value as its default until code
		// registers it; with no default for engine code's forced set, so a
		// registration gives it one and the value is saved
		var = Cvar_Get( var_name, flags ? value : "", flags );
		if ( !var ) {
			return NULL;
		}
	}

	var = Cvar_SetVar( var, value, source, force );
	// a tool may hold less than the console's rights, so what it set runs
	// restricted, as a server's does. The command line's, set again after
	// the configs, is trusted once it holds, as an unrestricted config
	// line's is (Cvar_SetFromText), and no longer a pak's, so a protected
	// cvar keeps it when the engine registers the cvar after a pak's
	// config set it
	if ( source == CVAR_SOURCE_SERVER || source == CVAR_SOURCE_TOOL ) {
		var->untrusted = qtrue;
	} else if ( source == CVAR_SOURCE_SESSION && value && !strcmp( var->string, value ) ) {
		var->untrusted = qfalse;
		var->pakSet = qfalse;
	}
	return var;
}

/*
============
Cvar_SetFromOrigin

Cvar_SetFrom with where the set came from ("server.cfg:12", "the tool
\"panel\"", "rcon from 203.0.113.5:27960"), noted as it takes, so cvar_why
and the change hook have it
============
*/
cvar_t *Cvar_SetFromOrigin( const char *var_name, const char *value, cvarSource_t source, const char *origin ) {
	const char	*outer = cvar_setOrigin;
	cvar_t		*var;

	cvar_setOrigin = origin;
	var = Cvar_SetFrom( var_name, value, source, qfalse );
	cvar_setOrigin = outer;
	return var;
}

/*
============
Cvar_Set2

A set by the engine, if forced, or else the player's (the console's and
configs' go through Cvar_SetFromText); saved if archived. NULL resets the
cvar to its default
============
*/
cvar_t *Cvar_Set2( const char *var_name, const char *value, qboolean force ) {
	return Cvar_SetFrom( var_name, value, force ? CVAR_SOURCE_ENGINE : CVAR_SOURCE_PLAYER, force );
}

/*
============
Cvar_ServerCreated

How many cvars a server created that no code has registered since, and
the bytes their names and values take
============
*/
int Cvar_ServerCreated( int *bytes ) {
	cvar_t	*var;
	int	count = 0;

	*bytes = 0;
	for ( var = cvar_vars; var; var = var->next ) {
		if ( var->flags & CVAR_SERVER_CREATED ) {
			count++;
			*bytes += strlen( var->name ) + strlen( var->string );
		}
	}
	return count;
}

/*
============
Cvar_BeginServerValues, Cvar_EndServerValues

Around a server's systeminfo, and its cheat rule (Cvar_SetCheatState): a
cvar the server no longer sets in between takes the player's own value
again, and one the server created, which has no other, and which no code
has registered (Joe, 2026-10-06, CNQ-007), goes. With nothing in
between, on leaving the server, that's every cvar
============
*/
void Cvar_BeginServerValues( void ) {
	cvar_t	*var;

	for ( var = cvar_vars; var; var = var->next ) {
		var->serverStale = var->serverString != NULL;
	}
}

void Cvar_EndServerValues( void ) {
	cvar_t	*var = cvar_vars;

	while ( var ) {
		if ( var->serverStale && ( var->flags & CVAR_SERVER_CREATED ) ) {
			// read only, so never the player's: only a module reads it, by
			// name, while the server sends it
			var = Cvar_Unset( var );
			continue;
		}
		if ( var->serverStale ) {
			Cvar_SetLayer( &var->serverString, NULL );
			// a latched one, such as a renderer's, after a restart
			Cvar_Apply( var, qfalse );
		}
		var->serverStale = qfalse;
		var = var->next;
	}
}

/*
============
Cvar_SetDeclarations
============
*/
void Cvar_SetDeclarations( const cvarDeclaration_t *declarations ) {
	cvar_declared = declarations;
	for ( cvar_numDeclared = 0; declarations && declarations[cvar_numDeclared].name; cvar_numDeclared++ ) {
	}
}

/*
============
Cvar_CompareName

bsearch's comparison of a name with a table's entry, whose first member
is its name
============
*/
static int Cvar_CompareName( const void *name, const void *entry ) {
	return Q_stricmp( name, *(const char * const *)entry );
}

/*
============
Cvar_Declaration

A name's declaration, or NULL
============
*/
const cvarDeclaration_t *Cvar_Declaration( const char *var_name ) {
	return bsearch( var_name, cvar_declared, cvar_numDeclared, sizeof( cvar_declared[0] ), Cvar_CompareName );
}

/*
============
Cvar_DeclaredScope
============
*/
int Cvar_DeclaredScope( const char *var_name ) {
	const cvarDeclaration_t	*declaration = Cvar_Declaration( var_name );

	return declaration ? (int)declaration->scope : -1;
}

/*
============
Cvar_DeclaredClientOnly

Whether a name is declared as meaning nothing to a dedicated server
============
*/
static qboolean Cvar_DeclaredClientOnly( const char *var_name ) {
	const cvarDeclaration_t	*declaration = Cvar_Declaration( var_name );

	return declaration && declaration->clientOnly;
}

/*
============
Cvar_ForeignPrograms
============
*/
const char *Cvar_ForeignPrograms( const char *var_name ) {
	static int				count = -1;
	const cvarForeign_t		*foreign;

	if ( count < 0 ) {
		for ( count = 0; cvar_foreignNames[count].name; count++ ) {
		}
	}
	foreign = bsearch( var_name, cvar_foreignNames, count, sizeof( cvar_foreignNames[0] ), Cvar_CompareName );
	return foreign ? foreign->programs : NULL;
}

/*
============
Cvar_Scope

Where a cvar is saved: by its declaration, or by who made it. A dedicated
server has no player or device of its own, so on one, a cvar nobody
declared, a mod's or one the admin made, is the server's
============
*/
cvarScope_t Cvar_Scope( const cvar_t *var ) {
	if ( var->declaredScope >= 0 ) {
		return var->declaredScope;
	}
	if ( !Com_IsClient() ) {
		return CVAR_SCOPE_SERVER;
	}
	return ( var->flags & ( CVAR_VM_CREATED | CVAR_USER_CREATED ) ) ? CVAR_SCOPE_PLAYER_MOD : CVAR_SCOPE_DEVICE;
}

/*
============
Cvar_MarkSaved

What a cvar saves changed: the config is to be written, the files whose
text changed (Com_WriteConfiguration)
============
*/
static void Cvar_MarkSaved( const cvar_t *var ) {
	if ( var->flags & CVAR_ARCHIVE ) {
		cvar_modifiedFlags |= CVAR_ARCHIVE;
	}
}

/*
============
Cvar_SetProfile

Called before any cvar is registered
============
*/
void Cvar_SetProfile( const cvarDefault_t *defaults ) {
	cvar_profile = defaults;
}

/*
============
Cvar_SetLatched

The engine's set that a latched cvar waits for a restart to take
============
*/
void Cvar_SetLatched( const char *var_name, const char *value) {
	Cvar_SetFrom( var_name, value, CVAR_SOURCE_ENGINE, qfalse );
}

/*
============
Cvar_SetValue
============
*/
void Cvar_SetValue( const char *var_name, float value) {
	char	val[32];

	Cvar_FormatValue( val, sizeof( val ), value );
	Cvar_Set (var_name, val);
}

/*
============
Cvar_Reset
============
*/
void Cvar_Reset( const char *var_name ) {
	Cvar_Set2( var_name, NULL, qfalse );
}

/*
============
Cvar_SetFromVM

A game module's set. As Cvar_SetSafe, and an engine cvar (one no module,
user or server created) that is read only, can't be set after startup,
or is cheat protected with cheats off, keeps its value, as it would from
the console, unless its name is in allowed, which lists the ones stock
game code sets. CVAR_LATCH cvars are still set at once.
============
*/
void Cvar_SetFromVM( const char *var_name, const char *value, const char * const *allowed, cvarSource_t source )
{
	cvar_t *var;

	Cvar_CheckSafeSet( var_name, value );

	// the value it has already needs no refusal
	var = Cvar_FindVar( var_name );
	if ( var && !( var->flags & ( CVAR_VM_CREATED | CVAR_USER_CREATED | CVAR_SERVER_CREATED ) ) &&
		( !value || strcmp( value, var->string ) ) ) {
		const char *refusal = Cvar_Refusal( var );

		for ( ; refusal && allowed && *allowed; allowed++ ) {
			if ( !Q_stricmp( var_name, *allowed ) && !( var->flags & CVAR_CHEAT ) ) {
				refusal = NULL;
			}
		}
		if ( refusal ) {
			Com_Printf( "%s is %s.\n", var_name, refusal );
			return;
		}
	}
	Cvar_SetUntrusted( var_name, value, qtrue, source );

	// a module's set of a cvar of its own wins over the server's layer, as
	// it won over the server's value before: the single player orbit camera
	// at a match's end sets the cgame's cheat protected cvars
	var = Cvar_FindVar( var_name );
	if ( var && ( var->flags & CVAR_VM_CREATED ) && var->serverString ) {
		Cvar_SetVar( var, value, CVAR_SOURCE_SERVER, qtrue );
	}
}

void Cvar_SetValueFromVM( const char *var_name, float value, const char * const *allowed, cvarSource_t source )
{
	char val[32];

	Cvar_FormatValue( val, sizeof( val ), value );
	Cvar_SetFromVM( var_name, val, allowed, source );
}

/*
============
Cvar_ResetSafe

Cvar_Reset for a restricted source, which may not reset a protected or
private cvar
============
*/
void Cvar_ResetSafe( const char *var_name ) {
	Cvar_CheckSafeSet( var_name, NULL );
	Cvar_SetUntrusted( var_name, NULL, qfalse, CVAR_SOURCE_GAME );
}

/*
============
Cvar_RunsRestricted

Whether a cvar's value, run as commands (vstr, nextdemo, activeAction),
runs restricted (Cmd_IsRestricted), with the rights of whoever set it.

From restricted text, a cvar keeps full rights only if it isn't
untrusted and isn't archived, since an
archived value comes back after a restart as though the player's config
had set it; so an admin's map rotation (set m1 "map q3dm1; set nextmap
vstr m2") works when qagame runs vstr nextmap. From anywhere else, it is
restricted if it's untrusted.
============
*/
qboolean Cvar_RunsRestricted( const char *var_name ) {
	cvar_t	*var = Cvar_FindVar( var_name );

	if ( !var ) {
		return Cmd_IsRestricted();
	}
	if ( Cmd_IsRestricted() && ( var->flags & CVAR_ARCHIVE ) ) {
		return qtrue;
	}
	return var->untrusted;
}

/*
============
Cvar_SetByPak

Whether a pak's config on a dedicated server set the cvar last, so that
what it holds runs with that config's limits
============
*/
qboolean Cvar_SetByPak( const char *var_name ) {
	cvar_t	*var = Cvar_FindVar( var_name );

	return var && var->pakSet;
}

/*
============
Cvar_ConfigLine

The config file's line that set a cvar's value ("server.cfg:12"), or NULL
when something else did (Cvar_NoteOrigin)
============
*/
const char *Cvar_ConfigLine( const char *var_name ) {
	cvar_t	*var = Cvar_FindVar( var_name );

	return var && var->userString && var->userConfigLine ? var->userOrigin : NULL;
}

/*
============
Cvar_KeptFromText, Cvar_AllowedFromText

Restricted text (Cmd_IsRestricted) can't use a private or protected
cvar, whether to set, reset, toggle, unset, print or vstr it; the first
says so quietly, help's list, the second aloud
============
*/
static qboolean Cvar_KeptFromText( int flags ) {
	return Cmd_IsRestricted() && ( flags & ( CVAR_PRIVATE | CVAR_PROTECTED ) );
}

qboolean Cvar_AllowedFromText( const char *var_name ) {
	// CVAR_NONEXISTENT has neither flag
	if ( Cvar_KeptFromText( Cvar_Flags( var_name ) ) ) {
		Com_Printf( "%s can't be used by game code or game content.\n", var_name );
		return qfalse;
	}
	return qtrue;
}

// the sets a config's lines made, in order, which --check reports
// (Cvar_PrintSetLog)
typedef struct {
	char	*origin, *name, *value;	// value NULL for one not shown
} cvarSet_t;

#define MAX_LOGGED_SETS	1024
static cvarSet_t	cvar_setLog[MAX_LOGGED_SETS];
static int			cvar_numSets, cvar_setsDropped;

// the engine's defaults, which the report leaves out
#define DEFAULT_CFG_ORIGIN	"default.cfg:"

/*
============
Cvar_LogCopy

A copy for the set log, which --check keeps until it exits: from the
heap, as a config that loops could fill the small zone
============
*/
static char *Cvar_LogCopy( const char *text ) {
	size_t	size = strlen( text ) + 1;
	char	*copy = malloc( size );

	if ( !copy ) {
		Com_Error( ERR_FATAL, "Cvar_LogCopy: out of memory" );
	}
	return memcpy( copy, text, size );
}

/*
============
Cvar_LogSet

A set by a config's line, for --check. The startup sets the command
line's settings more than once: a set the log has is moved to the end,
where it's now the latest. A secret's value (Cvar_Secret), or one an
OMNIFRAG_<CVAR>_FILE variable read from its file, isn't kept, so it isn't
shown
============
*/
static void Cvar_LogSet( const cvar_t *var, const char *origin ) {
	const char	*value = var->userString ? var->userString : "";
	cvarSet_t	set;
	int			i;

	if ( !com_check || !origin || !Q_stricmpn( origin, DEFAULT_CFG_ORIGIN, strlen( DEFAULT_CFG_ORIGIN ) ) ) {
		return;
	}
	if ( Cvar_Secret( var ) || Com_IsEnvironmentFile( origin ) ) {
		value = NULL;
	}
	for ( i = 0; i < cvar_numSets; i++ ) {
		set = cvar_setLog[i];
		if ( !Q_stricmp( set.name, var->name ) && !strcmp( set.origin, origin ) &&
				( set.value && value ? !strcmp( set.value, value ) : set.value == value ) ) {
			memmove( &cvar_setLog[i], &cvar_setLog[i + 1], ( cvar_numSets - i - 1 ) * sizeof( set ) );
			cvar_setLog[cvar_numSets - 1] = set;
			return;
		}
	}
	if ( cvar_numSets == MAX_LOGGED_SETS ) {
		cvar_setsDropped++;
		return;
	}
	set.origin = Cvar_LogCopy( origin );
	set.name = Cvar_LogCopy( var->name );	// an unset cvar is freed
	set.value = value ? Cvar_LogCopy( value ) : NULL;
	cvar_setLog[cvar_numSets++] = set;
}

// a logged set's value, quoted, or that it isn't shown
static const char *Cvar_LoggedValue( const cvarSet_t *set ) {
	return set->value ? va( "\"%s\"", set->value ) : "(its value not shown)";
}

/*
============
Cvar_PrintSetLog

Each set a config's line made, in order, and the last set, which the
cvar keeps, when another's
============
*/
void Cvar_PrintSetLog( void ) {
	int	i, j;

	for ( i = 0; i < cvar_numSets; i++ ) {
		const cvarSet_t	*set = &cvar_setLog[i], *later = NULL;

		for ( j = i + 1; j < cvar_numSets; j++ ) {
			if ( !Q_stricmp( cvar_setLog[j].name, set->name ) ) {
				later = &cvar_setLog[j];
			}
		}
		if ( later ) {
			Com_Printf( "  %-24s %s %s, then %s set %s\n", set->origin, set->name, Cvar_LoggedValue( set ),
				later->origin, Cvar_LoggedValue( later ) );
		} else {
			Com_Printf( "  %-24s %s %s\n", set->origin, set->name, Cvar_LoggedValue( set ) );
		}
	}
	if ( cvar_setsDropped ) {
		Com_Printf( "  and %i more sets\n", cvar_setsDropped );
	}
}

/*
============
Cvar_NoteOrigin

Where a set that gave the cvar its player's value came from: a text
set's line (Cmd_Origin), or a tool, named by the extension that set it.
cvar_why says it, and a config's line that does nothing is warned of
(Cvar_WarnConfigs)
============
*/
void Cvar_NoteOrigin( cvar_t *var, const char *origin ) {
	static int	order;

	Cvar_SetLayer( &var->userOrigin, origin );
	var->userConfigLine = Cmd_OriginIsConfigLine( origin );
	var->originOrder = ++order;
	Cvar_LogSet( var, origin );
}

/*
============
Cvar_SetFile

A dedicated server's own file's value for a cvar (server.toml), below the
player's, with the line that set it; NULL drops it. One no code has
registered yet is made, as a set makes it, its value its default until
code registers it. Returns why it was refused, as a set would be (read
only, write protected, cheat protected, "auto or always only"), or NULL
============
*/
const char *Cvar_SetFile( const char *var_name, const char *value, const char *origin ) {
	return Cvar_SetServerLayer( var_name, value, CVAR_SOURCE_FILE, origin );
}

/*
============
Cvar_SetServerLayer

Cvar_SetFile for any of a dedicated server's own layers: its file's
(CVAR_SOURCE_FILE), the map pool's entry's (CVAR_SOURCE_ENTRY) or its
mode's (CVAR_SOURCE_MODE)
============
*/
const char *Cvar_SetServerLayer( const char *var_name, const char *value, cvarSource_t source, const char *origin ) {
	static char	only[MAX_STRING_CHARS];
	cvar_t		*var = Cvar_FindVar( var_name );
	const char	*refusal = var ? Cvar_Refusal( var ) : NULL;
	int			layer = Cvar_ServerLayer( source );

	if ( layer < 0 ) {
		return "not one of a server's layers";
	}
	if ( refusal && !Cvar_SameString( var->layerStrings[layer], value ) ) {
		return refusal;
	}
	if ( var && value && !Cvar_NamedValue( var, value, only, sizeof( only ) ) ) {
		Q_strcat( only, sizeof( only ), " only" );
		return only;
	}
	if ( !var && !Cvar_ValidateString( var_name ) ) {
		return "not a setting's name";
	}
	Cvar_SetFromOrigin( var_name, value, source, origin );
	return NULL;
}

/*
============
Cvar_AdminValue, Cvar_SetGameValue

Whether an admin holds a value for a cvar, and the game's own set that
yields to it (qcommon.h). A config file's line (userConfigLine, noted as
it set the value) isn't an admin's: a classic server's config sets the
default a map's own value overrides
============
*/
qboolean Cvar_AdminValue( const cvar_t *var ) {
	int	i;

	for ( i = 0; i < CVAR_SERVER_LAYERS; i++ ) {
		if ( var->layerStrings[i] ) {
			return qtrue;
		}
	}
	if ( !var->userString ) {
		return qfalse;
	}
	switch ( var->userSource ) {
	case CVAR_SOURCE_SESSION:
	case CVAR_SOURCE_TOOL:
		return qtrue;
	case CVAR_SOURCE_PLAYER:
		return !var->userConfigLine;
	default:
		return qfalse;
	}
}

qboolean Cvar_SetGameValue( const char *name, const char *value ) {
	cvar_t	*var = Cvar_FindVar( name );

	if ( var && Cvar_AdminValue( var ) ) {
		// an earlier map's own value would outrank the server's layers
		if ( var->userString && var->userSource == CVAR_SOURCE_GAME ) {
			Cvar_DropUser( var );
		}
		return qfalse;
	}
	Cvar_SetFromVM( name, value, NULL, CVAR_SOURCE_GAME );
	return qtrue;
}

/*
============
Cvar_DropUser
============
*/
void Cvar_DropUser( cvar_t *var ) {
	char	*old = cvar_changeHook ? Cvar_HookOld( var ) : NULL;
	cvarSource_t	source = var->userSource;

	Cvar_ForgetUser( var );
	Cvar_Apply( var, qfalse );
	if ( old ) {
		cvar_changeHook( var, old, source );
		Z_Free( old );
	}
}

/*
============
Cvar_Lock
============
*/
void Cvar_Lock( cvar_t *var, const char *origin ) {
	Cvar_SetLayer( &var->lockOrigin, origin );
	Cvar_SetLayer( &var->lockString, var->userString );
}

/*
============
Cvar_LockedBy
============
*/
const char *Cvar_LockedBy( const char *var_name ) {
	cvar_t	*var = Cvar_FindVar( var_name );

	return var ? var->lockOrigin : NULL;
}

/*
============
Cvar_ForEach

Calls fn for every cvar, which it mustn't unset
============
*/
void Cvar_ForEach( void ( *fn )( cvar_t *var, void *context ), void *context ) {
	cvar_t	*var;

	for ( var = cvar_vars; var; var = var->next ) {
		if ( var->name ) {
			fn( var, context );
		}
	}
}

/*
============
Cvar_SourceName

A source's name, as cvar_dump gives it: "file", "player", "tool"
============
*/
const char *Cvar_SourceName( cvarSource_t source ) {
	return source >= 0 && source < ARRAY_LEN( cvar_sources ) ? cvar_sources[source].name : "default";
}

/*
============
Cvar_SavedChoice

What a config of these scopes (cvarScope_t bits) saves of a cvar, as
Cvar_WriteVariables writes it: an archived cvar's choice where it isn't
the default, or NULL
============
*/
const char *Cvar_SavedChoice( const cvar_t *var, int scopes ) {
	if ( !( var->flags & CVAR_ARCHIVE ) || !( scopes & CVAR_SCOPE_BIT( Cvar_Scope( var ) ) ) ||
		!Q_stricmp( var->name, "cl_cdkey" ) ) {
		return NULL;
	}
	return Cvar_SavedValue( var );
}

/*
============
Cvar_SetChangeHook
============
*/
void Cvar_SetChangeHook( void ( *hook )( cvar_t *var, const char *old, cvarSource_t source ) ) {
	cvar_changeHook = hook;
}

/*
============
Cvar_CommandSource

Whose a set by the running command is: a startup script's sets aren't
saved, its game content's too (default.cfg from a pak), which runs
restricted, nor a pak's config's on a dedicated server, a script of the
pak's; a tool's are the tool's; game code's restricted text is game
code's, but a passed vote's, which runs restricted, is the players'
============
*/
cvarSource_t Cvar_CommandSource( void ) {
	if ( Cmd_IsScript() || Cmd_IsPakConfig() ) {
		return CVAR_SOURCE_SCRIPT;
	}
	if ( Cmd_IsTool() ) {
		return CVAR_SOURCE_TOOL;
	}
	return Cmd_IsRestricted() && !Cmd_IsVote() ? CVAR_SOURCE_GAME : CVAR_SOURCE_PLAYER;
}

/*
============
Cvar_SetFromText

A set by a command, the player's, or game code's if the command is
restricted, which also marks the cvar untrusted; an unrestricted one
clears the mark if the cvar now holds the value it gave: a refused or
latched set leaves the old value, and its mark, in place
============
*/
static cvar_t *Cvar_SetFromText( const char *var_name, const char *value )
{
	qboolean	pak = Cmd_IsPakConfig();
	cvar_t	*var;

	// a pak's config sets what it likes but a protected cvar, which game
	// code can't set either: where files go, what the server runs, what
	// local programs may reach. One it set before the engine registered
	// it takes the engine's value then (Cvar_Get), being untrusted
	if ( pak && ( Cvar_Flags( var_name ) & CVAR_PROTECTED ) ) {
		Com_Printf( "%s can't be set by a config in a pak (%s).\n", var_name, Cmd_Origin() );
		return NULL;
	}

	// a set that takes notes its origin; a refused one leaves the earlier
	var = Cvar_SetFromOrigin( var_name, value, Cvar_CommandSource(), Cmd_Origin() );

	if ( var ) {
		if ( value && var->userString && !strcmp( var->userString, value ) ) {
			var->pakSet = pak;
		}
		// what a pak's config set runs restricted when it's run (vstr)
		if ( Cmd_IsRestricted() || pak ) {
			var->untrusted = qtrue;
		} else if ( !strcmp( var->string, value ? value : var->resetString ) ) {
			var->untrusted = qfalse;
		}
	}
	return var;
}

/*
============
Cvar_ForceReset
============
*/
void Cvar_ForceReset(const char *var_name)
{
	Cvar_Set2(var_name, NULL, qtrue);
}

/*
============
Cvar_SetCheatState

Any testing variables will be reset to the safe values
============
*/
void Cvar_SetCheatState(void)
{
	cvar_t	*var;

	// set all default vars to the safe value
	for(var = cvar_vars; var ; var = var->next)
	{
		if(var->flags & CVAR_CHEAT)
		{
			// the server's rule, until it allows cheats or we leave it
			// (Cvar_EndServerValues)
			Cvar_SetVar( var, var->resetString, CVAR_SOURCE_SERVER, qtrue );
		}
	}
}

/*
============
Cvar_Command

Handles variable inspection and changing from the console
============
*/
qboolean Cvar_Command( void ) {
	cvar_t	*v;

	// check variables
	v = Cvar_FindVar (Cmd_Argv(0));
	if (!v) {
		return qfalse;
	}
	if ( !Cvar_AllowedFromText( v->name ) ) {
		return qtrue;
	}

	// perform a variable print or set
	if ( Cmd_Argc() == 1 ) {
		Cvar_Print( v );
		return qtrue;
	}

	// set the value if forcing isn't required
	Cvar_SetFromText (v->name, Cmd_Args());
	return qtrue;
}


/*
============
Cvar_FromArgs

The cvar a command names as its argument, or NULL with why: restricted
text can't use a private or protected one
============
*/
static cvar_t *Cvar_FromArgs( void ) {
	cvar_t	*var;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: %s <variable>\n", Cmd_Argv( 0 ) );
		return NULL;
	}
	if ( !Cvar_AllowedFromText( Cmd_Argv( 1 ) ) ) {
		return NULL;
	}
	var = Cvar_FindVar( Cmd_Argv( 1 ) );
	if ( !var ) {
		Com_Printf( "Cvar %s does not exist.\n", Cmd_Argv( 1 ) );
	}
	return var;
}

/*
============
Cvar_Print_f

Prints the contents of a cvar
(preferred over Cvar_Command where cvar names and commands conflict)
============
*/
void Cvar_Print_f(void)
{
	cvar_t	*var = Cvar_FromArgs();

	if ( var ) {
		Cvar_Print( var );
	}
}

/*
============
Cvar_PlainPrintf

cvar_why's and help's text, with no color codes when it goes back to
rcon's sender, whose tools show them as text
============
*/
static void QDECL Cvar_PlainPrintf( const char *fmt, ... ) {
	char	text[MAX_STRING_CHARS];
	va_list	argptr;

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );
	if ( Com_IsRedirecting() ) {
		Cvar_StripColors( text );
	}
	Com_Printf( "%s", text );
}

/*
============
Cvar_Why_f

Prints where a cvar's value comes from: each of its layers (cvar_t) that
has one, after what Cvar_Print says, and its owner's account
============
*/
void Cvar_Why_f( void ) {
	cvar_t		*var = Cvar_FromArgs();
	qboolean	above;
	int			i;

	if ( !var ) {
		return;
	}
	Cvar_PrintWith( var, Cvar_PlainPrintf );
	if ( var->serverString ) {
		Cvar_PlainPrintf( "  the server requires \"%s" S_COLOR_WHITE "\" while you're on it\n", var->serverString );
	}
	if ( var->userString && var->userSource == CVAR_SOURCE_SESSION ) {
		Cvar_PlainPrintf( "  the command line set \"%s" S_COLOR_WHITE "\" for this run\n", var->userString );
	} else if ( var->userString && var->userOrigin ) {
		Cvar_PlainPrintf( "  %s set \"%s" S_COLOR_WHITE "\"\n", var->userOrigin, var->userString );
	} else if ( var->userString && var->userSource == CVAR_SOURCE_TOOL ) {
		Cvar_PlainPrintf( "  a tool set \"%s" S_COLOR_WHITE "\"\n", var->userString );
	}
	if ( var->userString && var->userSource == CVAR_SOURCE_SYSTEM ) {
		Cvar_PlainPrintf( "  the system set \"%s" S_COLOR_WHITE "\" for this run, by its own means\n", var->userString );
	}
	for ( i = 0, above = var->userString != NULL; i < CVAR_SERVER_LAYERS; i++ ) {
		if ( var->layerStrings[i] ) {
			Cvar_PlainPrintf( "  %s sets \"%s" S_COLOR_WHITE "\"%s\n", var->layerOrigins[i] ? var->layerOrigins[i] :
				cvar_serverLayers[i].unnamed, var->layerStrings[i], above ? ", under the value above" : "" );
			above = qtrue;
		}
	}
	if ( var->lockOrigin ) {
		Cvar_PlainPrintf( "  locked: %s sets it%s, so tools can't change it, and the console's change lasts "
			"until the next start\n", var->lockOrigin, var->lockString && !Cvar_SameString( var->lockString,
			var->userString ) ? va( " to \"%s" S_COLOR_WHITE "\", under the value above for this run",
			var->lockString ) : "" );
	}
	if ( var->reason ) {
		Cvar_PlainPrintf( "  %s\n", var->reason );
	}
	if ( var->savedString ) {
		Cvar_PlainPrintf( "  the choice saved is \"%s" S_COLOR_WHITE "\"%s\n", var->savedString,
			!( var->flags & CVAR_ARCHIVE ) ? ", which isn't archived, so no config saves it" :
			Cvar_SavedValue( var ) ? "" : ", the default, so the config leaves it out" );
	}
	if ( var->flags & CVAR_USER_CREATED ) {
		if ( var->userOrigin ) {
			Cvar_PlainPrintf( "  no code has registered it: a config created it (%s), and only what reads it "
				"(a vstr, or a mod that isn't loaded) gives it a use\n", var->userOrigin );
		} else {
			Cvar_PlainPrintf( "  no code has registered it, so it has no default\n" );
		}
	} else {
		// a server created one has the server's value as its default
		const char *profile = Cvar_ProfileDefault( var->name, NULL );

		if ( profile && !strcmp( profile, var->resetString ) ) {
			Cvar_PlainPrintf( "  its default is this device's\n" );
		}
	}
}

/*
============
Cvar_Toggle_f

Toggles a cvar for easy single key binding, optionally through a list of
given values
============
*/
void Cvar_Toggle_f( void ) {
	int		i, c = Cmd_Argc();
	char		*curval;

	if(c < 2) {
		Com_Printf("usage: toggle <variable> [value1, value2, ...]\n");
		return;
	}
	if ( !Cvar_AllowedFromText( Cmd_Argv( 1 ) ) ) {
		return;
	}

	if(c == 2) {
		Cvar_SetFromText(Cmd_Argv(1), va("%d",
			!Cvar_VariableValue(Cmd_Argv(1))));
		return;
	}

	if(c == 3) {
		Com_Printf("toggle: nothing to toggle to\n");
		return;
	}

	curval = Cvar_VariableString(Cmd_Argv(1));

	// don't bother checking the last arg for a match since the desired
	// behaviour is the same as no match (set to the first argument)
	for(i = 2; i + 1 < c; i++) {
		if(strcmp(curval, Cmd_Argv(i)) == 0) {
			Cvar_SetFromText(Cmd_Argv(1), Cmd_Argv(i + 1));
			return;
		}
	}

	// fallback
	Cvar_SetFromText(Cmd_Argv(1), Cmd_Argv(2));
}

/*
============
Cvar_Set_f

Allows setting and defining of arbitrary cvars from console, even if they
weren't declared in C code.
============
*/
void Cvar_Set_f( void ) {
	int		c;
	char	*cmd;
	cvar_t	*v;

	c = Cmd_Argc();
	cmd = Cmd_Argv(0);

	if ( c < 2 ) {
		Com_Printf ("usage: %s <variable> <value>\n", cmd);
		return;
	}
	if ( !Cvar_AllowedFromText( Cmd_Argv( 1 ) ) ) {
		return;
	}
	if ( c == 2 ) {
		Cvar_Print_f();
		return;
	}

	v = Cvar_SetFromText (Cmd_Argv(1), Cmd_ArgsFrom(2));
	if( !v ) {
		return;
	}
	switch( cmd[3] ) {
		case 'a':
			if( !( v->flags & CVAR_ARCHIVE ) ) {
				v->flags |= CVAR_ARCHIVE;
				Cvar_MarkSaved( v );
			}
			break;
		case 'u':
			if( !( v->flags & CVAR_USERINFO ) ) {
				v->flags |= CVAR_USERINFO;
				cvar_modifiedFlags |= CVAR_USERINFO;
			}
			break;
		case 's':
			if( !( v->flags & CVAR_SERVERINFO ) ) {
				v->flags |= CVAR_SERVERINFO;
				cvar_modifiedFlags |= CVAR_SERVERINFO;
			}
			break;
	}
}

/*
============
Cvar_Reset_f
============
*/
void Cvar_Reset_f( void ) {
	if ( Cmd_Argc() != 2 ) {
		Com_Printf ("usage: reset <variable>\n");
		return;
	}
	if ( !Cvar_AllowedFromText( Cmd_Argv( 1 ) ) ) {
		return;
	}
	Cvar_SetFromText( Cmd_Argv( 1 ), NULL );
}

static int QDECL Cvar_CompareNames( const void *a, const void *b ) {
	return Q_stricmp( ( *(const cvar_t * const *)a )->name, ( *(const cvar_t * const *)b )->name );
}

/*
============
Cvar_Sorted

The cvars whose names match a pattern (Com_Filter), or all of them for
NULL, sorted by name into cvar_sorted; how many
============
*/
static cvar_t	*cvar_sorted[MAX_CVARS];

static int Cvar_Sorted( const char *match ) {
	cvar_t	*var;
	int		count = 0;

	for ( var = cvar_vars; var; var = var->next ) {
		if ( var->name && ( !match || Com_Filter( (char *)match, var->name, qfalse ) ) ) {
			cvar_sorted[count++] = var;
		}
	}
	qsort( cvar_sorted, count, sizeof( cvar_sorted[0] ), Cvar_CompareNames );
	return count;
}

/*
============
Cvar_WriteVariables

Appends a "seta variable value" line for each archived variable whose
saved value (cvar_t) isn't its default, sorted by name, so the text doesn't
depend on the order cvars were registered in. One no code has registered
has no default yet, and is written as it is
============
*/
void Cvar_WriteVariables( configText_t *config, int hideFlags, int scopes )
{
	static cvar_t	*sorted[MAX_CVARS];
	cvar_t	*var;
	char	buffer[1024];
	int	count = 0, i;

	// the ones written, then sorted
	for ( var = cvar_vars; var; var = var->next ) {
		if ( var->name && Q_stricmp( var->name, "cl_cdkey" ) && !( var->flags & hideFlags ) &&
			( var->flags & CVAR_ARCHIVE ) && ( scopes & CVAR_SCOPE_BIT( Cvar_Scope( var ) ) ) &&
			Cvar_SavedValue( var ) ) {
			sorted[count++] = var;
		}
	}
	qsort( sorted, count, sizeof( sorted[0] ), Cvar_CompareNames );

	for ( i = 0; i < count; i++ ) {
		// the player's choice, even if it hasn't taken effect yet
		// (latched, or under a server's)
		const char	*value = Cvar_SavedValue( sorted[i] );

		var = sorted[i];
		// a quote or a line break would end the value early, and the
		// rest of it would run as commands when the config is executed
		if ( strpbrk( value, "\"\r\n" ) ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: value of variable "
					"\"%s\" has a quote or a line break, not written to file\n", var->name );
			continue;
		}
		// and a comment in the name would hide the rest of the line, or
		// with /* the lines after it (engine cvars such as
		// //trap_GetValue aren't archived)
		if ( strstr( var->name, "//" ) || strstr( var->name, "/*" ) || strstr( var->name, "*/" ) ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: name of variable "
					"\"%s\" has a comment, not written to file\n", var->name );
			continue;
		}
		if( strlen( var->name ) + strlen( value ) + 10 > sizeof( buffer ) ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: value of variable "
					"\"%s\" too long to write to file\n", var->name );
			continue;
		}
		Com_sprintf (buffer, sizeof(buffer), "seta %s \"%s\"\n", var->name, value);
		Com_ConfigAppend( config, buffer );
	}
}

/*
============
Cvar_List_f
============
*/
void Cvar_List_f( void ) {
	cvar_t	*var;
	int		i;
	char	*match;

	if ( Cmd_Argc() > 1 ) {
		match = Cmd_Argv( 1 );
	} else {
		match = NULL;
	}

	i = 0;
	for (var = cvar_vars ; var ; var = var->next, i++)
	{
		if(!var->name || (match && !Com_Filter(match, var->name, qfalse)))
			continue;

		if (var->flags & CVAR_SERVERINFO) {
			Com_Printf("S");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_SYSTEMINFO) {
			Com_Printf("s");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_USERINFO) {
			Com_Printf("U");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_ROM) {
			Com_Printf("R");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_INIT) {
			Com_Printf("I");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_ARCHIVE) {
			Com_Printf("A");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_LATCH) {
			Com_Printf("L");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_CHEAT) {
			Com_Printf("C");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_USER_CREATED) {
			Com_Printf("?");
		} else {
			Com_Printf(" ");
		}

		Com_Printf (" %s \"%s\"", var->name, var->string);
		Cvar_PrintSource( var, Com_Printf );
		Com_Printf( "\n" );
	}

	Com_Printf ("\n%i total cvars\n", i);
	Com_Printf ("%i cvar indexes\n", cvar_numIndexes);
}

/*
============
Cvar_ListModified_f
============
*/
void Cvar_ListModified_f( void ) {
	cvar_t	*var;
	int		totalModified;
	char	*value;
	char	*match;

	if ( Cmd_Argc() > 1 ) {
		match = Cmd_Argv( 1 );
	} else {
		match = NULL;
	}

	totalModified = 0;
	for (var = cvar_vars ; var ; var = var->next)
	{
		if ( !var->name || !var->modificationCount )
			continue;

		value = Cvar_NextValue( var );
		if ( !strcmp( value, var->resetString ) )
			continue;

		totalModified++;

		if (match && !Com_Filter(match, var->name, qfalse))
			continue;

		if (var->flags & CVAR_SERVERINFO) {
			Com_Printf("S");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_SYSTEMINFO) {
			Com_Printf("s");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_USERINFO) {
			Com_Printf("U");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_ROM) {
			Com_Printf("R");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_INIT) {
			Com_Printf("I");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_ARCHIVE) {
			Com_Printf("A");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_LATCH) {
			Com_Printf("L");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_CHEAT) {
			Com_Printf("C");
		} else {
			Com_Printf(" ");
		}
		if (var->flags & CVAR_USER_CREATED) {
			Com_Printf("?");
		} else {
			Com_Printf(" ");
		}

		Com_Printf (" %s \"%s\"", var->name, value);
		Cvar_PrintSource( var, Com_Printf );
		Com_Printf (", default \"%s\"\n", var->resetString);
	}

	Com_Printf ("\n%i total modified cvars\n", totalModified);
}

/*
============
Cvar_HasMin, Cvar_HasMax

Whether a cvar's range has that bound: one at the end of an int
(CVAR_INT_MIN, CVAR_INT_MAX) or a float, as an open end of CNQ3's ranges
is, is none
============
*/
qboolean Cvar_HasMin( const cvar_t *var ) {
	return var->validate && var->min > ( var->integral ? CVAR_INT_MIN : -FLT_MAX );
}

qboolean Cvar_HasMax( const cvar_t *var ) {
	return var->validate && var->max < ( var->integral ? CVAR_INT_MAX : FLT_MAX );
}

/*
============
Cvar_Applies

When a change to a cvar takes effect, in the declarations' words, by its
declaration or else its flags ("at startup" for CVAR_INIT); NULL for a
read only one
============
*/
static const char *Cvar_Applies( const cvar_t *var, const cvarDeclaration_t *declaration ) {
	if ( var->flags & CVAR_ROM ) {
		return NULL;
	}
	if ( declaration && declaration->applies ) {
		return declaration->applies;
	}
	return ( var->flags & CVAR_INIT ) ? "at startup" : ( var->flags & CVAR_LATCH ) ? "next map" : "at once";
}

// when a change takes effect (Cvar_Applies), what help says of it, and
// what extensions get (Cvar_ExtApplies)
static const struct {
	const char			*applies;
	const char			*sentence;
	extCvarApplies_t	kind;
} cvar_appliesSentences[] = {
	{ "at once", "Takes effect at once.", EXT_APPLIES_NOW },
	{ "next map", "Takes effect at the next map.", EXT_APPLIES_MAP },
	{ "vid_restart", "Takes effect at the next vid_restart.", EXT_APPLIES_VID_RESTART },
	{ "snd_restart", "Takes effect at the next snd_restart.", EXT_APPLIES_SND_RESTART },
	{ "restart", "Takes effect at the next restart.", EXT_APPLIES_RESTART },
	{ "at startup", "Takes effect only at startup: set it on the command line.", EXT_APPLIES_RESTART }
};

// the table's row for an applies, or its length for none
static int Cvar_AppliesRow( const char *applies ) {
	int		i;

	for ( i = 0; applies && i < ARRAY_LEN( cvar_appliesSentences ) && strcmp( cvar_appliesSentences[i].applies, applies ); i++ ) {
	}
	return applies ? i : ARRAY_LEN( cvar_appliesSentences );
}

/*
============
Cvar_ExtApplies

Cvar_Applies for extensions (hooks.h); a read only cvar's is now, as it
never changes
============
*/
extCvarApplies_t Cvar_ExtApplies( const cvar_t *var ) {
	int		i = Cvar_AppliesRow( Cvar_Applies( var, Cvar_Declaration( var->name ) ) );

	return i < ARRAY_LEN( cvar_appliesSentences ) ? cvar_appliesSentences[i].kind : EXT_APPLIES_NOW;
}

// the flags, by the names cvar_dump lists, and what help says of the ones
// that change what a player or admin sees
static const struct {
	int			flag;
	const char	*name;
	const char	*note;
} cvar_flags[] = {
	{ CVAR_ARCHIVE, "archive", NULL },
	{ CVAR_USERINFO, "userinfo", "Sent to the server." },
	{ CVAR_SERVERINFO, "serverinfo", "Shown in server lists." },
	{ CVAR_SYSTEMINFO, "systeminfo", "Sent to every player." },
	{ CVAR_INIT, "init", NULL },
	{ CVAR_LATCH, "latch", NULL },
	{ CVAR_ROM, "rom", NULL },
	{ CVAR_USER_CREATED, "user created", "No code has registered it." },
	{ CVAR_TEMP, "temp", NULL },
	{ CVAR_CHEAT, "cheat", "Changes only while sv_cheats is 1." },
	{ CVAR_NORESTART, "norestart", NULL },
	{ CVAR_SERVER_CREATED, "server created", NULL },
	{ CVAR_VM_CREATED, "vm created", NULL },
	{ CVAR_PROTECTED, "protected", "Game code and servers can't change it." },
	{ CVAR_NODEFAULT, "nodefault", NULL },
	{ CVAR_PRIVATE, "private", "Game code can't read it." }
};

/*
============
Cvar_RangeWords

The values a cvar takes, in words, from its range: "whole numbers 10 to
125", "0 or 1", "numbers 0 or more"; an enum's names, "auto or always"
(Cvar_NamedValue); empty with neither
============
*/
void Cvar_RangeWords( const cvar_t *var, char *words, int size ) {
	const char	*numbers = var->integral ? "whole numbers" : "numbers";
	qboolean	hasMin = Cvar_HasMin( var ), hasMax = Cvar_HasMax( var );
	char		min[32], max[32];

	words[0] = '\0';
	if ( !var->validate ) {
		// an enum's names, or nothing
		Cvar_NamedValue( var, "", words, size );
		return;
	}
	Cvar_FormatValue( min, sizeof( min ), var->min );
	Cvar_FormatValue( max, sizeof( max ), var->max );
	if ( var->integral && var->max - var->min == 1 ) {
		Com_sprintf( words, size, "%s or %s", min, max );
	} else if ( hasMin && hasMax ) {
		Com_sprintf( words, size, "%s %s to %s", numbers, min, max );
	} else if ( hasMin ) {
		Com_sprintf( words, size, "%s %s or more", numbers, min );
	} else if ( hasMax ) {
		Com_sprintf( words, size, "%s up to %s", numbers, max );
	} else {
		Q_strncpyz( words, numbers, size );
	}
}

/*
============
Cvar_PrintWrapped

A paragraph, indented, its lines broken between words to fit 78 columns
============
*/
#define HELP_COLUMNS	78

static void Cvar_PrintWrapped( const char *indent, const char *text ) {
	int	width = HELP_COLUMNS - strlen( indent );

	while ( *text ) {
		int	cut = strlen( text );

		if ( cut > width ) {
			for ( cut = width; cut > 0 && text[cut] != ' '; cut-- ) {
			}
			// a word longer than a line
			if ( !cut ) {
				cut = width;
			}
		}
		Cvar_PlainPrintf( "%s%.*s\n", indent, cut, text );
		for ( text += cut; *text == ' '; text++ ) {
		}
	}
}

/*
============
Cvar_HelpNotes

What a cvar's flags and declaration say, in words, so its description
needn't: when a change takes effect, whether it's saved, and where else
its value goes
============
*/
static void Cvar_HelpNotes( const cvar_t *var, const cvarDeclaration_t *declaration, char *notes, int size ) {
	const char	*applies = Cvar_Applies( var, declaration );
	int			i;

	notes[0] = '\0';
	if ( !applies ) {
		Q_strcat( notes, size, "Read only." );
	} else {
		i = Cvar_AppliesRow( applies );
		Q_strcat( notes, size, i < ARRAY_LEN( cvar_appliesSentences ) ?
			cvar_appliesSentences[i].sentence : va( "Takes effect %s.", applies ) );
		if ( ( var->flags & CVAR_ARCHIVE ) && Cvar_Scope( var ) != CVAR_SCOPE_NONE ) {
			Q_strcat( notes, size, " Saved." );
		} else if ( !( var->flags & CVAR_INIT ) ) {
			Q_strcat( notes, size, va( " Not saved: set it in a config %s, or on the command line.",
				Com_IsClient() ? "such as autoexec.cfg" : "the server runs" ) );
		}
	}
	for ( i = 0; i < ARRAY_LEN( cvar_flags ); i++ ) {
		if ( ( var->flags & cvar_flags[i].flag ) && cvar_flags[i].note ) {
			Q_strcat( notes, size, va( " %s", cvar_flags[i].note ) );
		}
	}
}

/*
============
Cvar_PrintHelp

All a cvar says about itself: its value and the values it takes, its
whole description, its values one a line, and what its flags say
============
*/
static void Cvar_PrintHelp( const cvar_t *var ) {
	const cvarDeclaration_t	*declaration = Cvar_Declaration( var->name );
	cvarTextReader_t		reader;
	char					line[MAX_STRING_CHARS], words[64];

	Com_sprintf( line, sizeof( line ), "%s \"%s\"", var->name, var->string );
	if ( !( var->flags & CVAR_ROM ) ) {
		Q_strcat( line, sizeof( line ), !Q_stricmp( var->string, var->resetString ) ?
			", the default" : va( ", default \"%s\"", var->resetString ) );
	}
	Cvar_RangeWords( var, words, sizeof( words ) );
	if ( words[0] ) {
		Q_strcat( line, sizeof( line ), va( "; %s", words ) );
	}
	if ( declaration && declaration->unit ) {
		Q_strcat( line, sizeof( line ), va( "; %s", declaration->unit ) );
	}
	Cvar_PlainPrintf( "%s\n", line );
	if ( var->latchedString ) {
		Cvar_PlainPrintf( "\"%s\" from the next map\n", var->latchedString );
	}

	// the summary, then the explanation and the values
	for ( Cvar_StartReading( &reader, var->description ); Cvar_NextLine( &reader ); ) {
		if ( reader.number == 1 ) {
			Cvar_PrintWrapped( "", reader.line );
		} else if ( reader.value ) {
			Cvar_PlainPrintf( "  %s = %s%s%s\n", reader.value, reader.label,
				reader.meaning ? ": " : "", reader.meaning ? reader.meaning : "" );
		} else if ( reader.line[0] ) {
			Cvar_PrintWrapped( "  ", reader.line );
		}
	}
	if ( var->description && declaration && declaration->type && !strcmp( declaration->type, "bits" ) ) {
		Cvar_PlainPrintf( "  Add the numbers to combine them.\n" );
	}

	Cvar_HelpNotes( var, declaration, line, sizeof( line ) );
	Cvar_PrintWrapped( "", line );
}

/*
============
Cvar_Help_f

help <name>: all a cvar says about itself. help <pattern> (sv_*): one
line for each that matches, its name, value and summary
============
*/
static void Cvar_Help_f( void ) {
	const char	*match = Cmd_Argv( 1 );
	int			count, i;

	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: help <setting>, or help <pattern> for a list, such as help sv_*\n" );
		return;
	}
	if ( !strpbrk( match, "*?" ) ) {
		const cvar_t	*var = Cvar_FromArgs();

		if ( var ) {
			Cvar_PrintHelp( var );
		}
		return;
	}

	count = Cvar_Sorted( match );
	for ( i = 0; i < count; i++ ) {
		const cvar_t	*var = cvar_sorted[i];
		const char		*summary = var->description ? var->description : "";

		if ( !Cvar_KeptFromText( var->flags ) ) {
			Cvar_PlainPrintf( "%s \"%s\"%s%.*s\n", var->name, var->string, summary[0] ? "  " : "",
				(int)strcspn( summary, "\n" ), summary );
		}
	}
	if ( !count ) {
		Com_Printf( "No setting matches %s.\n", match );
	}
}

// cvar_dump's line for a cvar, built and then written in one go, and where
// it goes: a file, or with none the terminal (Sys_Print)
static char			cvar_dumpLine[MAX_STRING_CHARS * 16];
static int			cvar_dumpLength;
static fileHandle_t	cvar_dumpFile;

static void Cvar_DumpFlush( void ) {
	if ( cvar_dumpFile ) {
		FS_Write( cvar_dumpLine, cvar_dumpLength, cvar_dumpFile );
	} else {
		Sys_Print( cvar_dumpLine );
	}
	cvar_dumpLength = 0;
	cvar_dumpLine[0] = '\0';
}

// to the line, which is written early if it's full
static void Cvar_DumpAppend( const char *text ) {
	int	length = strlen( text );

	if ( cvar_dumpLength + length >= (int)sizeof( cvar_dumpLine ) ) {
		Cvar_DumpFlush();
	}
	Q_strncpyz( cvar_dumpLine + cvar_dumpLength, text, sizeof( cvar_dumpLine ) - cvar_dumpLength );
	cvar_dumpLength += strlen( cvar_dumpLine + cvar_dumpLength );
}

/*
============
Cvar_UTF8Length

How many bytes the UTF-8 character at text takes, or 0 if it isn't one
============
*/
int Cvar_UTF8Length( const char *text ) {
	const unsigned char	*s = (const unsigned char *)text;
	int					length, i;

	length = s[0] < 0x80 ? 1 : ( s[0] & 0xe0 ) == 0xc0 && s[0] >= 0xc2 ? 2 :
		( s[0] & 0xf0 ) == 0xe0 ? 3 : ( s[0] & 0xf8 ) == 0xf0 && s[0] <= 0xf4 ? 4 : 0;
	for ( i = 1; i < length; i++ ) {
		if ( ( s[i] & 0xc0 ) != 0x80 ) {
			return 0;
		}
	}
	return length;
}

/*
============
Cvar_DumpEscaped

A text as it goes inside a JSON string: quotes, backslashes and control
characters escaped; UTF-8 as it is, and any other byte past ASCII, as a
config or a mod's text may hold, escaped as the character of its value
============
*/
static void Cvar_DumpEscaped( const char *text ) {
	char	chunk[256];
	int		length = 0;

	for ( ; *text; text++ ) {
		unsigned char	c = *text;
		int				utf8 = c > '~' ? Cvar_UTF8Length( text ) : 0;

		if ( length > (int)sizeof( chunk ) - 8 ) {
			chunk[length] = '\0';
			Cvar_DumpAppend( chunk );
			length = 0;
		}
		if ( c == '"' || c == '\\' ) {
			chunk[length++] = '\\';
			chunk[length++] = c;
		} else if ( c == '\n' ) {
			chunk[length++] = '\\';
			chunk[length++] = 'n';
		} else if ( utf8 > 1 ) {
			memcpy( chunk + length, text, utf8 );
			length += utf8;
			text += utf8 - 1;
		} else if ( c < ' ' || c > '~' ) {
			length += Com_sprintf( chunk + length, sizeof( chunk ) - length, "\\u%04x", c );
		} else {
			chunk[length++] = c;
		}
	}
	chunk[length] = '\0';
	Cvar_DumpAppend( chunk );
}

// a JSON string, or null
static void Cvar_DumpString( const char *text ) {
	if ( !text ) {
		Cvar_DumpAppend( "null" );
		return;
	}
	Cvar_DumpAppend( "\"" );
	Cvar_DumpEscaped( text );
	Cvar_DumpAppend( "\"" );
}

// ,"key": and a string or null
static void Cvar_DumpField( const char *key, const char *text ) {
	Cvar_DumpAppend( va( ",\"%s\":", key ) );
	Cvar_DumpString( text );
}

// ,"key": and a range's bound as a JSON number, or null for none
static void Cvar_DumpBound( const char *key, float bound, qboolean set ) {
	char	number[32] = "null";

	if ( set ) {
		Cvar_FormatValue( number, sizeof( number ), bound );
	}
	Cvar_DumpAppend( va( ",\"%s\":%s", key, number ) );
}

static const char * const cvar_scopeNames[] = {
	[CVAR_SCOPE_NONE] = "none",
	[CVAR_SCOPE_PLAYER] = "player",
	[CVAR_SCOPE_PLAYER_MOD] = "player per mod",
	[CVAR_SCOPE_DEVICE] = "device",
	[CVAR_SCOPE_DEVICE_MOD] = "device per mod",
	[CVAR_SCOPE_SERVER] = "server"
};

#define MAX_DUMP_MENTIONS	32

/*
============
Cvar_DumpMentions

The cvars a cvar's description names: its words that name one that
exists or is declared, each once
============
*/
static void Cvar_DumpMentions( const cvar_t *var ) {
	const char	*mentions[MAX_DUMP_MENTIONS];
	const char	*text = var->description ? var->description : "";
	int			count = 0, i;

	Cvar_DumpAppend( ",\"mentions\":[" );
	while ( *text && count < MAX_DUMP_MENTIONS ) {
		const cvar_t			*named;
		const cvarDeclaration_t	*declared;
		const char				*name;
		char					word[MAX_CVAR_VALUE_STRING];
		int						length = 0;

		if ( !isalpha( (unsigned char)*text ) && *text != '_' ) {
			text++;
			continue;
		}
		for ( ; isalnum( (unsigned char)*text ) || *text == '_'; text++ ) {
			if ( length < (int)sizeof( word ) - 1 ) {
				word[length++] = *text;
			}
		}
		word[length] = '\0';
		named = Cvar_FindVar( word );
		declared = named ? NULL : Cvar_Declaration( word );
		name = named ? named->name : declared ? declared->name : NULL;
		if ( !name || !Q_stricmp( name, var->name ) ) {
			continue;
		}
		for ( i = 0; i < count && Q_stricmp( mentions[i], name ); i++ ) {
		}
		if ( i == count ) {
			Cvar_DumpAppend( count ? "," : "" );
			Cvar_DumpString( name );
			mentions[count++] = name;
		}
	}
	Cvar_DumpAppend( "]" );
}

/*
============
Cvar_DumpText

A cvar's description as help reads it (Cvar_NextLine): its values, its
summary, its explanation's lines joined by line breaks, and the cvars it
names
============
*/
static void Cvar_DumpText( const cvar_t *var ) {
	cvarTextReader_t	reader;
	char				summary[MAX_STRING_CHARS] = "", explanation[MAX_STRING_CHARS] = "";
	qboolean			first = qtrue;

	Cvar_DumpAppend( ",\"values\":[" );
	for ( Cvar_StartReading( &reader, var->description ); Cvar_NextLine( &reader ); ) {
		if ( reader.number == 1 ) {
			Q_strncpyz( summary, reader.line, sizeof( summary ) );
		} else if ( reader.value ) {
			Cvar_DumpAppend( first ? "{\"value\":" : ",{\"value\":" );
			Cvar_DumpString( reader.value );
			Cvar_DumpField( "label", reader.label );
			Cvar_DumpField( "meaning", reader.meaning );
			Cvar_DumpAppend( "}" );
			first = qfalse;
		} else if ( reader.line[0] ) {
			if ( explanation[0] ) {
				Q_strcat( explanation, sizeof( explanation ), "\n" );
			}
			Q_strcat( explanation, sizeof( explanation ), reader.line );
		}
	}
	Cvar_DumpAppend( "]" );
	Cvar_DumpField( "summary", var->description ? summary : NULL );
	Cvar_DumpField( "explanation", explanation[0] ? explanation : NULL );
	Cvar_DumpMentions( var );
}

/*
============
Cvar_IsSecretName

Whether a name, a cvar's or an info string's key, is one whose value is
kept out of the console and its log: any with "password" in it, so a
mod's referee password counts too
============
*/
qboolean Cvar_IsSecretName( const char *name ) {
	return Q_stristr( name, "password" ) != NULL;
}

/*
============
Cvar_IsSecret

Whether a cvar's value is one cvar_dump leaves out: a private one's (a
path, a password), a declared password, or one named as one, such as a
mod's referee password
============
*/
static qboolean Cvar_IsSecret( const cvar_t *var, const cvarDeclaration_t *declaration ) {
	return ( var->flags & CVAR_PRIVATE ) || Cvar_IsSecretName( var->name ) ||
		( declaration && declaration->type && !strcmp( declaration->type, "password" ) );
}

/*
============
Cvar_Secret

Whether a cvar's value is kept out of what's shown of it: a password's,
a private one's (Cvar_IsSecret)
============
*/
qboolean Cvar_Secret( const cvar_t *var ) {
	return Cvar_IsSecret( var, Cvar_Declaration( var->name ) );
}

/*
============
Cvar_ValidValue

What a cvar would take a value as, in its range (Cvar_Validate), quietly
============
*/
const char *Cvar_ValidValue( cvar_t *var, const char *value ) {
	return Cvar_Validate( var, value, qfalse );
}

/*
============
Cvar_ExtRefused

For extensions (hooks.h): whether a settings screen may not show a cvar,
by its name whether or not it exists: a secret, a protected one, or a CD
key
============
*/
qboolean Cvar_ExtRefused( const char *var_name ) {
	cvar_t	*var = Cvar_FindVar( var_name );
	cvar_t	unmade;

	// Team Arena's cdkey and cdkeychecksum, and cl_cdkey
	if ( Q_stristr( var_name, "cdkey" ) ) {
		return qtrue;
	}
	if ( !var ) {
		Com_Memset( &unmade, 0, sizeof( unmade ) );
		unmade.name = (char *)var_name;
		var = &unmade;
	}
	return ( var->flags & CVAR_PROTECTED ) || Cvar_IsSecret( var, Cvar_Declaration( var_name ) );
}

/*
============
Cvar_DumpVar

One cvar as a line of JSON: its value, where it comes from, its flags,
its declaration, its range, and its text. A secret's value and default
are left out (null): a path's default is the path
============
*/
static void Cvar_DumpVar( const cvar_t *var ) {
	const cvarDeclaration_t	*declaration = Cvar_Declaration( var->name );
	qboolean	secret = Cvar_IsSecret( var, declaration );
	qboolean	first = qtrue;
	int			i;

	Cvar_DumpAppend( "{\"name\":" );
	Cvar_DumpString( var->name );
	Cvar_DumpField( "value", secret ? NULL : var->string );
	Cvar_DumpField( "latched", secret ? NULL : var->latchedString );
	Cvar_DumpField( "default", secret ? NULL : var->resetString );
	Cvar_DumpField( "source", cvar_sources[ Cvar_Source( var ) ].name );
	for ( i = 0; i < CVAR_SERVER_LAYERS; i++ ) {
		Cvar_DumpField( cvar_serverLayers[i].dump, secret ? NULL : var->layerStrings[i] );
		Cvar_DumpField( cvar_serverLayers[i].dumpOrigin, var->layerOrigins[i] );
	}
	Cvar_DumpField( "locked", var->lockOrigin );
	Cvar_DumpAppend( ",\"flags\":[" );
	for ( i = 0; i < ARRAY_LEN( cvar_flags ); i++ ) {
		if ( var->flags & cvar_flags[i].flag ) {
			Cvar_DumpAppend( va( "%s\"%s\"", first ? "" : ",", cvar_flags[i].name ) );
			first = qfalse;
		}
	}
	Cvar_DumpAppend( "]" );
	Cvar_DumpField( "scope", cvar_scopeNames[ Cvar_Scope( var ) ] );
	Cvar_DumpField( "role", declaration ? declaration->role : NULL );
	Cvar_DumpField( "owner", declaration ? declaration->owner : NULL );
	Cvar_DumpField( "tier", declaration ? declaration->tier : NULL );
	Cvar_DumpField( "type", declaration ? declaration->type : NULL );
	Cvar_DumpField( "unit", declaration ? declaration->unit : NULL );
	Cvar_DumpBound( "min", var->min, Cvar_HasMin( var ) );
	Cvar_DumpBound( "max", var->max, Cvar_HasMax( var ) );
	Cvar_DumpAppend( va( ",\"integral\":%s", var->validate && var->integral ? "true" : "false" ) );
	Cvar_DumpField( "applies", Cvar_Applies( var, declaration ) );
	Cvar_DumpText( var );
	Cvar_DumpAppend( "}\n" );
	Cvar_DumpFlush();
}

/*
============
Cvar_Dump_f

cvar_dump [<file>.json]: every cvar, a JSON object a line (Cvar_DumpVar),
for a front end, to the file in the home's game directory, or with none
to the terminal, which rcon's sender doesn't see. Game code and a pak's
config can't run it (Cmd_ExecuteString)
============
*/
static void Cvar_Dump_f( void ) {
	char	filename[MAX_QPATH];
	int		count, i;

	if ( Cmd_Argc() > 2 ) {
		Com_Printf( "usage: cvar_dump [<file>.json]\n" );
		return;
	}
	if ( Cmd_Argc() == 1 && Com_IsRedirecting() ) {
		Com_Printf( "cvar_dump with no file writes to the server's terminal, which rcon doesn't see: name a .json file.\n" );
		return;
	}
	if ( Cmd_Argc() == 2 ) {
		Q_strncpyz( filename, Cmd_Argv( 1 ), sizeof( filename ) );
		COM_DefaultExtension( filename, sizeof( filename ), ".json" );
		if ( !COM_CompareExtension( filename, ".json" ) ) {
			Com_Printf( "cvar_dump writes only a .json file.\n" );
			return;
		}
		// under --check the file system refuses it, saying so
		cvar_dumpFile = FS_FOpenFileWrite_HomeData( filename );
		if ( !cvar_dumpFile ) {
			Com_Printf( "Couldn't write %s.\n", filename );
			return;
		}
	}

	count = Cvar_Sorted( NULL );
	for ( i = 0; i < count; i++ ) {
		Cvar_DumpVar( cvar_sorted[i] );
	}

	if ( cvar_dumpFile ) {
		FS_FCloseFile( cvar_dumpFile );
		cvar_dumpFile = 0;
		Com_Printf( "Wrote %d settings to %s.\n", count, filename );
	}
}

/*
============
Cvar_Unset

Unsets a cvar
============
*/

cvar_t *Cvar_Unset(cvar_t *cv)
{
	cvar_t *next = cv->next;
	int i;

	// note what types of cvars have been modified (userinfo, archive, serverinfo, systeminfo)
	cvar_modifiedFlags |= cv->flags;

	if(cv->name)
		Z_Free(cv->name);
	if(cv->string)
		Z_Free(cv->string);
	if(cv->latchedString)
		Z_Free(cv->latchedString);
	if(cv->resetString)
		Z_Free(cv->resetString);
	if(cv->description)
		Z_Free(cv->description);
	Cvar_ForgetModules( cv );
	Cvar_SetLayer( &cv->serverString, NULL );
	Cvar_ClearUser( cv );
	Cvar_SetLayer( &cv->savedString, NULL );
	for ( i = 0; i < CVAR_SERVER_LAYERS; i++ ) {
		Cvar_SetLayer( &cv->layerStrings[i], NULL );
		Cvar_SetLayer( &cv->layerOrigins[i], NULL );
	}
	Cvar_SetLayer( &cv->lockOrigin, NULL );
	Cvar_SetLayer( &cv->lockString, NULL );
	Cvar_SetLayer( &cv->reason, NULL );

	if(cv->prev)
		cv->prev->next = cv->next;
	else
		cvar_vars = cv->next;
	if(cv->next)
		cv->next->prev = cv->prev;

	if(cv->hashPrev)
		cv->hashPrev->hashNext = cv->hashNext;
	else
		hashTable[cv->hashIndex] = cv->hashNext;
	if(cv->hashNext)
		cv->hashNext->hashPrev = cv->hashPrev;

	Com_Memset(cv, '\0', sizeof(*cv));
	
	return next;
}

/*
============
Cvar_Unset_f

Unsets a userdefined cvar
============
*/

void Cvar_Unset_f(void)
{
	cvar_t *cv;
	
	if(Cmd_Argc() != 2)
	{
		Com_Printf("Usage: %s <varname>\n", Cmd_Argv(0));
		return;
	}
	if ( !Cvar_AllowedFromText( Cmd_Argv( 1 ) ) ) {
		return;
	}

	cv = Cvar_FindVar(Cmd_Argv(1));

	if(!cv)
		return;
	
	if(cv->flags & CVAR_USER_CREATED)
		Cvar_Unset(cv);
	else
		Com_Printf("Error: %s: Variable %s is not user created.\n", Cmd_Argv(0), cv->name);
}



/*
============
Cvar_RestartKeeping

Resets all cvars to their hardcoded values and removes userdefined variables
and variables added via the VMs if requested, but those with any of
keepFlags
============
*/

static void Cvar_RestartKeeping(qboolean unsetVM, int keepFlags)
{
	cvar_t	*curvar;

	curvar = cvar_vars;

	while(curvar)
	{
		if ( curvar->flags & keepFlags ) {
			curvar = curvar->next;
			continue;
		}

		if((curvar->flags & CVAR_USER_CREATED) ||
			(unsetVM && (curvar->flags & CVAR_VM_CREATED)))
		{
			// throw out any variables the user/vm created
			curvar = Cvar_Unset(curvar);
			continue;
		}
		
		if(!(curvar->flags & (CVAR_ROM | CVAR_INIT | CVAR_NORESTART)))
		{
			// Just reset the rest to their default values: the player's
			// and the command line's go, even under a server's rule that
			// refuses a set (cheats), which still holds
			Cvar_ClearUser( curvar );
			if ( curvar->savedString ) {
				Cvar_SetLayer( &curvar->savedString, NULL );
				Cvar_MarkSaved( curvar );
			}
			Cvar_Apply( curvar, qfalse );
		}
		
		curvar = curvar->next;
	}
}


/*
============
Cvar_Restart
============
*/
void Cvar_Restart(qboolean unsetVM)
{
	Cvar_RestartKeeping( unsetVM, 0 );
}

/*
============
Cvar_Restart_f

Resets all cvars to their hardcoded values; restricted text leaves the
private and protected ones be, and a pak's config the protected ones
============
*/
void Cvar_Restart_f(void)
{
	Cvar_RestartKeeping( qfalse, Cmd_IsRestricted() ? CVAR_PRIVATE | CVAR_PROTECTED :
		Cmd_IsPakConfig() ? CVAR_PROTECTED : 0 );
}

/*
=====================
Cvar_InfoString
=====================
*/
static char *Cvar_InfoStringHiding( int bit, int hideFlags )
{
	static char	info[MAX_INFO_STRING];
	cvar_t	*var;

	info[0] = 0;

	// a private cvar goes only in the userinfo, to the server the player
	// joins, never in what a server tells everyone
	if ( bit != CVAR_USERINFO ) {
		hideFlags |= CVAR_PRIVATE;
	}

	for(var = cvar_vars; var; var = var->next)
	{
		if(var->name && (var->flags & bit) && !(var->flags & hideFlags))
			Info_SetValueForKey (info, var->name, var->string);
	}

	return info;
}

char *Cvar_InfoString(int bit)
{
	return Cvar_InfoStringHiding( bit, 0 );
}

/*
=====================
Cvar_InfoString_Big

  handles large info strings ( CS_SYSTEMINFO )
=====================
*/
char *Cvar_InfoString_Big(int bit)
{
	static char	info[BIG_INFO_STRING];
	cvar_t	*var;

	info[0] = 0;

	for (var = cvar_vars; var; var = var->next)
	{
		if(var->name && (var->flags & bit) && !(var->flags & CVAR_PRIVATE))
			Info_SetValueForKey_Big (info, var->name, var->string);
	}
	return info;
}



/*
=====================
Cvar_InfoStringBuffer
=====================
*/
void Cvar_InfoStringBuffer( int bit, char* buff, int buffsize ) {
	Q_strncpyz(buff,Cvar_InfoString(bit),buffsize);
}

/*
=====================
Cvar_InfoStringBufferSafe

For game code, without the cvars it may not read (VM_PrivateCvarFlag)
=====================
*/
void Cvar_InfoStringBufferSafe( int bit, char *buff, int buffsize ) {
	Q_strncpyz( buff, Cvar_InfoStringHiding( bit, VM_PrivateCvarFlag() ), buffsize );
}

// a cvar's own description and range, while game modules' are in place
// (cvar_t's own)
typedef struct cvarOwn_s {
	char		*description;
	qboolean	validate;
	qboolean	integral;
	float		min;
	float		max;
} cvarOwn_t;

/*
=====================
Cvar_SetRange

Cvar_CheckRange's, and a game module's (Cvar_SetRangeByName), which holds
the value but not the choice saved: a mod's narrower range mustn't
rewrite an admin's saved fraglimit for good
=====================
*/
static void Cvar_ValidateLayers( cvar_t *var, qboolean keepSaved );

static void Cvar_SetRange( cvar_t *var, float min, float max, qboolean integral, qboolean keepSaved )
{
	var->validate = qtrue;
	var->min = min;
	var->max = max;
	var->integral = integral;
	Cvar_ValidateLayers( var, keepSaved );
}

/*
=====================
Cvar_ValidateLayers

What a cvar takes, once it's known (a range, an enum's names), checked
in its default and each layer set before
=====================
*/
static void Cvar_ValidateLayers( cvar_t *var, qboolean keepSaved )
{
	int		i;

	Cvar_SetLayer( &var->resetString, Cvar_Validate( var, var->resetString, qfalse ) );
	// a server's own layers (server.toml's, a mode's, an entry's), set
	// before code registered the cvar, kept as the saved value is
	for ( i = 0; i < CVAR_SERVER_LAYERS && !keepSaved; i++ )
		if ( var->layerStrings[i] )
			Cvar_SetLayer( &var->layerStrings[i], Cvar_Validate( var, var->layerStrings[i], qtrue ) );
	if ( var->serverString )
		Cvar_SetLayer( &var->serverString, Cvar_Validate( var, var->serverString, qtrue ) );
	if ( var->userString )
		Cvar_SetLayer( &var->userString, Cvar_Validate( var, var->userString, qtrue ) );
	if ( var->savedString && !keepSaved )
		Cvar_SetLayer( &var->savedString, Cvar_Validate( var, var->savedString, qfalse ) );
	Cvar_Apply( var, qtrue );
}

/*
=====================
Cvar_IntegralBounds

An integer's bounds as whole numbers, inside the ones given and inside an
int's range as a float holds it (CVAR_INT_MIN, CVAR_INT_MAX)
=====================
*/
static void Cvar_IntegralBounds( float *min, float *max )
{
	*min = MAX( ceilf( *min ), CVAR_INT_MIN );
	*max = MIN( floorf( *max ), CVAR_INT_MAX );
}

/*
=====================
Cvar_CheckRange

The cvar's own range: kept for later while a game module's is in place
=====================
*/
void Cvar_CheckRange( cvar_t *var, float min, float max, qboolean integral )
{
	if( integral )
		Cvar_IntegralBounds( &min, &max );
	if( var->own )
	{
		var->own->validate = qtrue;
		var->own->min = min;
		var->own->max = max;
		var->own->integral = integral;
		return;
	}
	Cvar_SetRange( var, min, max, integral, qfalse );
}

/*
=====================
Cvar_CopyDescription

A description into its place, without color codes, which a mod's text may
have, so what prints it and reads its lines (Cvar_NextLine) see none
=====================
*/
static void Cvar_CopyDescription( char **description, const char *text )
{
	if( text && text[0] != '\0' )
	{
		if( *description != NULL )
		{
			Z_Free( *description );
		}
		*description = CopyString( text );
		Cvar_StripColors( *description );
	}
}

/*
=====================
Cvar_SetDescription

The cvar's own description: kept for later while a game module's is in
place
=====================
*/
// whether the default and each layer are among an enum's names
// (Cvar_NamedValue), as they are for any other cvar
static qboolean Cvar_LayersNamed( const cvar_t *var )
{
	const char	*layers[4 + CVAR_SERVER_LAYERS] = { var->resetString, var->serverString, var->userString,
		var->savedString };
	char		words[MAX_STRING_CHARS];
	int			i;

	for ( i = 0; i < CVAR_SERVER_LAYERS; i++ )
		layers[4 + i] = var->layerStrings[i];
	for ( i = 0; i < ARRAY_LEN( layers ); i++ )
		if ( layers[i] && !Cvar_NamedValue( var, layers[i], words, sizeof( words ) ) )
			return qfalse;
	return qtrue;
}

void Cvar_SetDescription( cvar_t *var, const char *var_description )
{
	Cvar_CopyDescription( var->own ? &var->own->description : &var->description, var_description );
	// an enum's names, checked in what a config or the command line set
	// before them; only when one isn't a name, as checking applies a
	// latched value at once (Cvar_ValidateLayers' forced Cvar_Apply)
	if ( !var->own && !Cvar_LayersNamed( var ) )
		Cvar_ValidateLayers( var, qfalse );
}

/*
=====================
Cvar_SetReason
=====================
*/
void Cvar_SetReason( cvar_t *var, const char *reason )
{
	Cvar_SetLayer( &var->reason, reason && reason[0] ? reason : NULL );
}

/*
=====================
Cvar_KeepOwn

Before a game module first gives a cvar a description or a range, keeps
the cvar's own, which come back as the module unloads
(Cvar_ModuleUnloaded), so a mod loaded after ours never shows our text
=====================
*/
static void Cvar_KeepOwn( cvar_t *var, cvarModule_t module )
{
	if( !var->own )
	{
		var->own = Z_Malloc( sizeof( *var->own ) );
		var->own->description = var->description ? CopyString( var->description ) : NULL;
		var->own->validate = var->validate;
		var->own->integral = var->integral;
		var->own->min = var->min;
		var->own->max = var->max;
	}
	var->moduleGave |= module;
}

/*
=====================
Cvar_ForgetModules

What game modules gave a cvar is its own now, as engine code takes over
one they created, or it's unset
=====================
*/
static void Cvar_ForgetModules( cvar_t *var )
{
	var->moduleGave = 0;
	if( var->own )
	{
		if( var->own->description )
			Z_Free( var->own->description );
		Z_Free( var->own );
		var->own = NULL;
	}
}

/*
=====================
Cvar_ModuleUnloaded
=====================
*/
void Cvar_ModuleUnloaded( int module )
{
	cvar_t		*var;
	cvarOwn_t	*own;

	for( var = cvar_vars; var; var = var->next )
	{
		if( !( var->moduleGave & module ) )
			continue;
		// another module gave it too, and it keeps what they gave
		var->moduleGave &= ~module;
		if( var->moduleGave )
			continue;

		own = var->own;
		var->own = NULL;
		if( var->description )
			Z_Free( var->description );
		var->description = own->description;
		// the range only if a module changed it: setting it again would
		// apply a latched value now
		if( own->validate && ( !var->validate || var->min != own->min ||
			var->max != own->max || var->integral != own->integral ) )
			Cvar_SetRange( var, own->min, own->max, own->integral, qfalse );
		else if( !own->validate )
			var->validate = qfalse;
		Z_Free( own );
	}
}

/*
=====================
Cvar_FindVMVar

The cvar a game module names, if it may describe or bound it: one game
code created itself, unlike the engine's. OmniFrag: and for the server's
game module, one the game's declarations give to the game or the bots,
such as fraglimit or g_gametype, which the engine creates for the server
info before any module loads (docs/design/settings-descriptions.md,
decision 2)
=====================
*/
static cvar_t *Cvar_FindVMVar( cvarModule_t module, const char *var_name )
{
	cvar_t					*var = var_name ? Cvar_FindVar( var_name ) : NULL;
	const cvarDeclaration_t	*declaration;

	if( !var )
		return NULL;
	if( var->flags & CVAR_VM_CREATED )
		return var;
	if( module != CVAR_MODULE_GAME || ( var->flags & ( CVAR_PRIVATE | CVAR_PROTECTED ) ) )
		return NULL;
	declaration = Cvar_Declaration( var->name );
	if( declaration && declaration->owner &&
		( !strcmp( declaration->owner, "game" ) || !strcmp( declaration->owner, "bots" ) ) )
		return var;
	return NULL;
}

/*
=====================
Cvar_SetDescriptionByName

Describes an existing cvar, for game modules, which name cvars rather
than hold them
=====================
*/
void Cvar_SetDescriptionByName( cvarModule_t module, const char *var_name, const char *var_description )
{
	cvar_t *var;

	if( !var_description || strlen( var_description ) >= MAX_STRING_CHARS )
		return;

	var = Cvar_FindVMVar( module, var_name );
	if( var )
	{
		Cvar_KeepOwn( var, module );
		Cvar_CopyDescription( &var->description, var_description );
	}
}

/*
=====================
Cvar_SetRangeByName

CNQ3's trap_Cvar_SetRange, for game modules: gives a cvar the game code
created the range of a type (CVAR_RANGE_*, CNQ3's), which Cvar_CheckRange
then keeps it in. A NULL min or max is no bound.
=====================
*/
void Cvar_SetRangeByName( cvarModule_t module, const char *var_name, int type, const char *minString, const char *maxString )
{
	cvar_t	*var;
	float	min, max;

	var = Cvar_FindVMVar( module, var_name );
	if( !var )
		return;
	Cvar_KeepOwn( var, module );

	switch( type )
	{
	case CVAR_RANGE_STRING:
	default:	// CPMA's colors, unchecked
		var->validate = qfalse;
		return;
	case CVAR_RANGE_FLOAT:
		min = -FLT_MAX;
		max = FLT_MAX;
		break;
	case CVAR_RANGE_INTEGER:
	case CVAR_RANGE_BITMASK:
		min = CVAR_INT_MIN;
		max = CVAR_INT_MAX;
		break;
	case CVAR_RANGE_BOOL:
		Cvar_SetRange( var, 0, 1, qtrue, qtrue );
		return;
	}

	if( ( minString && !Q_isanumber( minString ) ) || ( maxString && !Q_isanumber( maxString ) ) )
	{
		Com_Printf( "WARNING: cvar '%s' given a range that isn't numbers\n", var->name );
		return;
	}
	if( minString )
		min = atof( minString );
	if( maxString )
		max = atof( maxString );
	if( type != CVAR_RANGE_FLOAT )
		Cvar_IntegralBounds( &min, &max );
	// also refuses NaN
	if( !( min <= max ) )
	{
		Com_Printf( "WARNING: cvar '%s' given a range whose min is over its max\n", var->name );
		return;
	}
	Cvar_SetRange( var, min, max, type != CVAR_RANGE_FLOAT, qtrue );
}

/*
=====================
Cvar_Register

basically a slightly modified Cvar_Get for the interpreted modules
=====================
*/
void Cvar_Register(vmCvar_t *vmCvar, const char *varName, const char *defaultValue, int flags)
{
	cvar_t	*cv;

	// There is code in Cvar_Get to prevent CVAR_ROM cvars being changed by the
	// user. In other words CVAR_ARCHIVE and CVAR_ROM are mutually exclusive
	// flags. Unfortunately some historical game code (including single player
	// baseq3) sets both flags. We unset CVAR_ROM for such cvars.
	if ((flags & (CVAR_ARCHIVE | CVAR_ROM)) == (CVAR_ARCHIVE | CVAR_ROM)) {
		Com_DPrintf( S_COLOR_YELLOW "WARNING: Unsetting CVAR_ROM from cvar '%s', "
			"since it is also CVAR_ARCHIVE\n", varName );
		flags &= ~CVAR_ROM;
	}

	// Don't allow VM to specific a different creator or other internal flags.
	if ( flags & CVAR_USER_CREATED ) {
		Com_DPrintf( S_COLOR_YELLOW "WARNING: VM tried to set CVAR_USER_CREATED on cvar '%s'\n", varName );
		flags &= ~CVAR_USER_CREATED;
	}
	if ( flags & CVAR_SERVER_CREATED ) {
		Com_DPrintf( S_COLOR_YELLOW "WARNING: VM tried to set CVAR_SERVER_CREATED on cvar '%s'\n", varName );
		flags &= ~CVAR_SERVER_CREATED;
	}
	if ( flags & CVAR_PROTECTED ) {
		Com_DPrintf( S_COLOR_YELLOW "WARNING: VM tried to set CVAR_PROTECTED on cvar '%s'\n", varName );
		flags &= ~CVAR_PROTECTED;
	}
	if ( flags & CVAR_MODIFIED ) {
		Com_DPrintf( S_COLOR_YELLOW "WARNING: VM tried to set CVAR_MODIFIED on cvar '%s'\n", varName );
		flags &= ~CVAR_MODIFIED;
	}
	if ( flags & CVAR_NONEXISTENT ) {
		Com_DPrintf( S_COLOR_YELLOW "WARNING: VM tried to set CVAR_NONEXISTENT on cvar '%s'\n", varName );
		flags &= ~CVAR_NONEXISTENT;
	}

	cv = Cvar_FindVar(varName);

	// Don't modify cvar if it's protected or private.
	if ( cv && ( cv->flags & ( CVAR_PROTECTED | CVAR_PRIVATE ) ) ) {
		Com_DPrintf( S_COLOR_YELLOW "WARNING: VM tried to register %s cvar '%s' with value '%s'%s\n",
			( cv->flags & CVAR_PROTECTED ) ? "protected" : "private",
			varName, defaultValue, ( flags & ~cv->flags ) != 0 ? " and new flags" : "" );
	} else {
		cv = Cvar_Get(varName, defaultValue, flags | CVAR_VM_CREATED);
	}

	if (!vmCvar)
		return;

	vmCvar->handle = cv - cvar_indexes;
	vmCvar->modificationCount = -1;
	Cvar_Update( vmCvar );
}


/*
=====================
Cvar_Update

updates an interpreted modules' version of a cvar
=====================
*/
void	Cvar_Update( vmCvar_t *vmCvar ) {
	cvar_t	*cv = NULL;
	assert(vmCvar);

	if ( (unsigned)vmCvar->handle >= cvar_numIndexes ) {
		Com_Error( ERR_DROP, "Cvar_Update: handle out of range" );
	}

	cv = cvar_indexes + vmCvar->handle;

	if ( cv->modificationCount == vmCvar->modificationCount ) {
		return;
	}
	if ( !cv->string ) {
		return;		// variable might have been cleared by a cvar_restart
	}
	vmCvar->modificationCount = cv->modificationCount;
	// a cvar the module may not read reads as empty
	if ( cv->flags & VM_PrivateCvarFlag() ) {
		vmCvar->string[0] = '\0';
		vmCvar->value = 0;
		vmCvar->integer = 0;
		return;
	}
	if ( strlen(cv->string)+1 > MAX_CVAR_VALUE_STRING ) 
	  Com_Error( ERR_DROP, "Cvar_Update: src %s length %u exceeds MAX_CVAR_VALUE_STRING",
		     cv->string, 
		     (unsigned int) strlen(cv->string));
	Q_strncpyz( vmCvar->string, cv->string,  MAX_CVAR_VALUE_STRING ); 

	vmCvar->value = cv->value;
	vmCvar->integer = cv->integer;
}

/*
==================
Cvar_CompleteCvarName
==================
*/
void Cvar_CompleteCvarName( char *args, int argNum )
{
	if( argNum == 2 )
	{
		// Skip "<cmd> "
		char *p = Com_SkipTokens( args, 1, " " );

		if( p > args )
			Field_CompleteCommand( p, qfalse, qtrue );
	}
}

/*
============
Cvar_Init

Reads in all archived cvars
============
*/
void Cvar_Init (void)
{
	Com_Memset(cvar_indexes, '\0', sizeof(cvar_indexes));
	Com_Memset(hashTable, '\0', sizeof(hashTable));

	cvar_cheats = Cvar_Get("sv_cheats", "1", CVAR_ROM | CVAR_SYSTEMINFO );

	Cmd_AddCommand ("print", Cvar_Print_f);
	Cmd_AddCommand ("toggle", Cvar_Toggle_f);
	Cmd_SetCommandCompletionFunc( "toggle", Cvar_CompleteCvarName );
	Cmd_AddCommand ("set", Cvar_Set_f);
	Cmd_SetCommandCompletionFunc( "set", Cvar_CompleteCvarName );
	Cmd_AddCommand ("sets", Cvar_Set_f);
	Cmd_SetCommandCompletionFunc( "sets", Cvar_CompleteCvarName );
	Cmd_AddCommand ("setu", Cvar_Set_f);
	Cmd_SetCommandCompletionFunc( "setu", Cvar_CompleteCvarName );
	Cmd_AddCommand ("seta", Cvar_Set_f);
	Cmd_SetCommandCompletionFunc( "seta", Cvar_CompleteCvarName );
	Cmd_AddCommand ("reset", Cvar_Reset_f);
	Cmd_SetCommandCompletionFunc( "reset", Cvar_CompleteCvarName );
	Cmd_AddCommand ("unset", Cvar_Unset_f);
	Cmd_SetCommandCompletionFunc("unset", Cvar_CompleteCvarName);

	Cmd_AddCommand ("cvar_why", Cvar_Why_f);
	Cmd_SetCommandCompletionFunc( "cvar_why", Cvar_CompleteCvarName );
	Cmd_AddCommand ("help", Cvar_Help_f);
	Cmd_SetCommandCompletionFunc( "help", Cvar_CompleteCvarName );
	Cmd_AddCommand ("cvar_dump", Cvar_Dump_f);
	Cmd_AddCommand ("cvarlist", Cvar_List_f);
	Cmd_AddCommand ("cvar_modified", Cvar_ListModified_f);
	Cmd_AddCommand ("cvar_restart", Cvar_Restart_f);
}

/*
============
Cvar_EditDistance

The edits (a letter added, dropped or changed) that make one name the
other, case aside, or limit + 1 if it takes more
============
*/
static int Cvar_EditDistance( const char *a, const char *b, int limit ) {
	int	lengthA = strlen( a ), lengthB = strlen( b );
	int	row[MAX_CVAR_VALUE_STRING + 1];
	int	i, j;

	if ( abs( lengthA - lengthB ) > limit || lengthB > MAX_CVAR_VALUE_STRING ) {
		return limit + 1;
	}
	for ( j = 0; j <= lengthB; j++ ) {
		row[j] = j;
	}
	for ( i = 1; i <= lengthA; i++ ) {
		int	diagonal = row[0], best;

		row[0] = best = i;
		for ( j = 1; j <= lengthB; j++ ) {
			int	above = row[j];

			row[j] = MIN( MIN( row[j] + 1, row[j - 1] + 1 ),
				diagonal + ( tolower( a[i - 1] ) != tolower( b[j - 1] ) ) );
			diagonal = above;
			best = MIN( best, row[j] );
		}
		if ( best > limit ) {
			return limit + 1;
		}
	}
	return row[lengthB];
}

// the best match Cvar_KnownNameLike has found
typedef struct {
	const char	*name;
	int			distance, count;
} cvarNameMatch_t;

/*
============
Cvar_ConsiderName
============
*/
static void Cvar_ConsiderName( const char *var_name, const char *known, cvarNameMatch_t *match ) {
	int	distance = Cvar_EditDistance( var_name, known, 2 );

	if ( distance < match->distance ) {
		match->name = known, match->distance = distance, match->count = 1;
	} else if ( distance == match->distance && Q_stricmp( match->name, known ) ) {
		match->count++;
	}
}

/*
============
Cvar_KnownNameLike

The name of a cvar the code registered or the build declares that is a
typo or two from a name nothing knows, or NULL: none, or more than one
============
*/
static const char *Cvar_KnownNameLike( const char *var_name ) {
	cvarNameMatch_t	match = { NULL, 3, 0 };
	cvar_t			*var;
	int				i;

	for ( var = cvar_vars; var; var = var->next ) {
		if ( !( var->flags & CVAR_USER_CREATED ) ) {
			Cvar_ConsiderName( var_name, var->name, &match );
		}
	}
	for ( i = 0; i < cvar_numDeclared; i++ ) {
		Cvar_ConsiderName( var_name, cvar_declared[i].name, &match );
	}
	return match.count == 1 ? match.name : NULL;
}

// master servers that are gone, by the end of their host names
static const char * const cvar_deadMasters[] = { ".idsoftware.com", ".gamespy.com", NULL };

/*
============
Cvar_IsDeadMaster
============
*/
static qboolean Cvar_IsDeadMaster( const char *address ) {
	char		host[MAX_CVAR_VALUE_STRING];
	char		*port;
	const char	* const *dead;
	int			length;

	// the host, without a port
	Q_strncpyz( host, address, sizeof( host ) );
	if ( ( port = strchr( host, ':' ) ) != NULL ) {
		*port = '\0';
	}
	length = strlen( host );
	for ( dead = cvar_deadMasters; *dead; dead++ ) {
		int	tail = strlen( *dead );

		if ( length >= tail && !Q_stricmp( host + length - tail, *dead ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
============
Cvar_ConfigWarning

What's wrong with a config's line that set the cvar, or NULL: it sets a
client's cvar on a dedicated server, another engine's, a typo of a known
name, or a master server that's gone
============
*/
static const char *Cvar_ConfigWarning( const cvar_t *var ) {
	const char	*programs, *like;

	// a client's that this program doesn't register either
	if ( !Com_IsClient() && ( var->flags & CVAR_USER_CREATED ) && Cvar_DeclaredClientOnly( var->name ) ) {
		return va( "%s is a client's setting; a dedicated server doesn't use it", var->name );
	}
	if ( !( var->flags & CVAR_USER_CREATED ) || var->declaredScope >= 0 ) {
		if ( !Q_stricmpn( var->name, "sv_master", 9 ) && Cvar_IsDeadMaster( var->string ) ) {
			return va( "%s's %s is gone; remove the line", var->name, var->string );
		}
		return NULL;
	}
	if ( ( programs = Cvar_ForeignPrograms( var->name ) ) != NULL ) {
		return va( "%s is known from %s, and isn't supported here, so the line does nothing",
			var->name, programs );
	}
	if ( strlen( var->name ) >= 5 && ( like = Cvar_KnownNameLike( var->name ) ) != NULL ) {
		return va( "there's no %s, so the line does nothing; %s?", var->name, like );
	}
	return NULL;
}

/*
============
Cvar_CompareOrigins

Orders cvars by when a config's line last set them (Cvar_NoteOrigin)
============
*/
static int Cvar_CompareOrigins( const void *a, const void *b ) {
	return ( *(const cvar_t * const *)a )->originOrder - ( *(const cvar_t * const *)b )->originOrder;
}

/*
============
Cvar_WarnFile

Cvar_WarnConfigs for a dedicated server's own file's lines (server.toml),
its pool entry's and mode's included, which a cvar's player's value
doesn't hide
============
*/
static void Cvar_WarnFile( void ) {
	cvar_t		*var;
	const char	*warning, *origin;
	int			i;

	for ( var = cvar_vars; var; var = var->next ) {
		for ( i = 0, origin = NULL; i < CVAR_SERVER_LAYERS && !origin; i++ ) {
			origin = var->layerOrigins[i];
		}
		if ( origin && !var->userOrigin && ( warning = Cvar_ConfigWarning( var ) ) != NULL ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: %s: %s\n", origin, warning );
		}
	}
}

/*
============
Cvar_WarnConfigs

Warns, once each and in the order they ran, of a config's lines that set
a cvar and do nothing (Cvar_ConfigWarning). Run once the game and a mod
have registered theirs, so a cvar of either isn't taken for unknown; a
cvar no code knows that isn't like a known name, a vstr's text or an
absent mod's, says nothing
============
*/
void Cvar_WarnConfigs( void ) {
	cvar_t		**set, *var;
	const char	*warning;
	int			count = 0, i;

	for ( var = cvar_vars; var; var = var->next ) {
		count += var->userOrigin != NULL;
	}
	if ( !count ) {
		Cvar_WarnFile();
		return;
	}
	set = Z_Malloc( count * sizeof( *set ) );
	for ( count = 0, var = cvar_vars; var; var = var->next ) {
		if ( var->userOrigin ) {
			set[count++] = var;
		}
	}
	qsort( set, count, sizeof( *set ), Cvar_CompareOrigins );
	for ( i = 0; i < count; i++ ) {
		if ( ( warning = Cvar_ConfigWarning( set[i] ) ) != NULL ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: %s: %s\n", set[i]->userOrigin, warning );
		}
	}
	Z_Free( set );
	Cvar_WarnFile();
}
