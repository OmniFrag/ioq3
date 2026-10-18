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
// fs_name.c -- which names a server may give the files a client writes, by
// the names alone

#include "q_shared.h"
#include "qcommon.h"

/*
================
FS_DeviceName

Whether the length characters at name are a device name Windows
reserves whatever extension follows: CON, PRN, AUX, NUL, and COM and LPT
with a digit, 0 to 9, in any case. The name is taken up to its first
dot, then trimmed of final spaces, as Windows takes it, so "con .x" and
"con..x" are CON and "con x.y" isn't. CONIN$, CONOUT$ and CLOCK$ have a
'$', which FS_ValidName refuses, and the superscript digits Windows also
takes after COM and LPT aren't ASCII
================
*/
static qboolean FS_DeviceName( const char *name, size_t length ) {
	static const char	*devices[] = { "con", "prn", "aux", "nul" };
	size_t				stem = 0;
	int					i;

	while ( stem < length && name[stem] != '.' ) {
		stem++;
	}
	while ( stem > 0 && name[stem - 1] == ' ' ) {
		stem--;
	}

	for ( i = 0; i < ARRAY_LEN( devices ); i++ ) {
		if ( stem == 3 && !Q_stricmpn( name, devices[i], 3 ) ) {
			return qtrue;
		}
	}
	return stem == 4 && ( !Q_stricmpn( name, "com", 3 ) || !Q_stricmpn( name, "lpt", 3 ) ) &&
		name[3] >= '0' && name[3] <= '9';
}

/*
================
FS_ValidName

Whether the length characters at name are a file or folder name that
means the same on every platform: printable ASCII, as real pak names
are, without any of / \ : * ? " < > | $; not empty, not hidden (a
leading dot), not ending in a dot or a space, which Windows drops, so
"settings." is settings/ there; with no '~' and a digit, as in a short
name Windows makes up (ZZ-MAP~1), which may be another file's; and not a
device name Windows reserves (FS_DeviceName)
================
*/
static qboolean FS_ValidName( const char *name, size_t length ) {
	size_t	i;

	if ( !length || name[0] == '.' || name[length - 1] == '.' || name[length - 1] == ' ' ) {
		return qfalse;
	}
	for ( i = 0; i < length; i++ ) {
		if ( (unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7e ||
			strchr( "/\\:*?\"<>|$", name[i] ) || ( name[i] == '~' && i + 1 < length && name[i + 1] >= '0' && name[i + 1] <= '9' ) ) {
			return qfalse;
		}
	}
	return !FS_DeviceName( name, length );
}

/*
================
FS_EngineFolder

Whether a valid name (FS_ValidName) is one of the folders the engine keeps
in the home for itself, settings/ and cache/, ignoring case
================
*/
qboolean FS_EngineFolder( const char *name ) {
	return !Q_stricmp( name, "settings" ) || !Q_stricmp( name, "cache" );
}

/*
================
FS_ValidDownloadName

Whether a name a server lists for a pak ("<game>/<name>", without its
.pk3) names a pak directly in a game directory, where the client may
write it: both parts valid names (FS_ValidName), and the game directory
not one of the engine's own folders (FS_EngineFolder). So not in a folder
within a game directory, at the top of the home, or anywhere outside it
================
*/
qboolean FS_ValidDownloadName( const char *name ) {
	const char	*slash = strchr( name, '/' );
	char		game[MAX_QPATH];

	if ( !slash || slash - name >= sizeof( game ) || !FS_ValidName( name, slash - name ) ||
		!FS_ValidName( slash + 1, strlen( slash + 1 ) ) ) {
		return qfalse;
	}
	Q_strncpyz( game, name, slash - name + 1 );
	return !FS_EngineFolder( game );
}
