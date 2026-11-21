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
// com_identity.c -- the keys this program proves itself with (identity.h):
// the device's, identity.key at the home config base's root, under no
// game directory, so every mod shares it; and a dedicated server's own,
// server.key in its folder (FS_ServerDir). A listen server is its
// player's device. Each is made at its first use, readable by its user
// alone, from the system's randomness; with none, or a file that isn't a
// key, there's no key, and the handshake proves nothing.

#include "q_shared.h"
#include "qcommon.h"
#include "identity.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

typedef enum {
	KEY_UNREAD,
	KEY_READY,
	KEY_NONE
} keyState_t;

static identKey_t	com_deviceKey, com_serverKey;
static keyState_t	com_deviceKeyState, com_serverKeyState;

/*
=================
Com_RandomSecret

Bytes from the system's randomness alone: qfalse, saying so once, where
there is none, rather than the weak fallback Com_RandomBytes takes. In a
test build, OMNIFRAG_TEST_NO_RANDOMNESS stands in for a system without it,
as it does for hosting's HS_RandomSecret
=================
*/
qboolean Com_RandomSecret( byte *bytes, int length ) {
	static qboolean	warned;

	if ( !Com_TestEnv( "OMNIFRAG_TEST_NO_RANDOMNESS" ) && Sys_RandomBytes( bytes, length ) ) {
		return qtrue;
	}
	if ( !warned ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: the system gives no randomness, so no key is made or proved, and "
			"connections aren't protected\n" );
		warned = qtrue;
	}
	return qfalse;
}

/*
=================
Com_LoadKey

A key from its file, or a new one written there when there's none. A
file that isn't a key is left as it is, and there's no key
=================
*/
static keyState_t Com_LoadKey( const char *ospath, identKey_t *key, const char *what ) {
	char		path[MAX_OSPATH], text[IDENT_FILE_SIZE + 1];
	void		*buffer;
	long		length;
	byte		seed[IDENT_KEY_SIZE];
	qboolean	read;

	Q_strncpyz( path, ospath, sizeof( path ) );
	length = FS_ReadOSFile( path, &buffer );
	if ( length >= 0 ) {
#ifndef _WIN32
		struct stat	st;

		// written for its user alone (Sys_FOpenPrivate); a copy restored
		// from a backup often isn't
		if ( !stat( path, &st ) && ( st.st_mode & ( S_IRWXG | S_IRWXO ) ) ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: %s can be read by other users of this computer; make it yours "
				"alone (chmod 600 %s)\n", path, path );
		}
#endif
		read = Ident_ReadKeyFile( buffer, (int)length, seed );
		Ident_Wipe( buffer, length );
		Z_Free( buffer );
		if ( !read ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: %s isn't an OmniFrag key, so %s proves nothing; move it away to "
				"get a new one\n", path, what );
			return KEY_NONE;
		}
	} else {
		// --check makes nothing
		if ( com_check || !Com_RandomSecret( seed, sizeof( seed ) ) ) {
			return KEY_NONE;
		}
		Ident_KeyFileText( seed, text );
		read = FS_ReplaceOSFile( path, text, (int)strlen( text ), qtrue );
		Ident_Wipe( text, sizeof( text ) );
		if ( !read ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: couldn't write %s, so %s proves nothing\n", path, what );
			Ident_Wipe( seed, sizeof( seed ) );
			return KEY_NONE;
		}
	}
	Ident_KeyFromSeed( key, seed );
	Ident_Wipe( seed, sizeof( seed ) );
	return KEY_READY;
}

/*
=================
Com_DeviceKey

This device's key, or NULL
=================
*/
const identKey_t *Com_DeviceKey( void ) {
	if ( com_deviceKeyState == KEY_UNREAD ) {
		com_deviceKeyState = Com_LoadKey( FS_BaseDir_BuildOSPath( Cvar_VariableString( "fs_homeconfigpath" ),
			IDENT_DEVICE_FILE ), &com_deviceKey, "this device" );
	}
	return com_deviceKeyState == KEY_READY ? &com_deviceKey : NULL;
}

/*
=================
Com_ServerKey

The key this program serves with, or NULL: a dedicated server's own, in
its folder; a listen server's player's device's
=================
*/
const identKey_t *Com_ServerKey( void ) {
	if ( Com_IsClient() ) {
		return Com_DeviceKey();
	}
	if ( com_serverKeyState == KEY_UNREAD ) {
		com_serverKeyState = Com_LoadKey( FS_BaseDir_BuildOSPath( Cvar_VariableString( "fs_homeconfigpath" ),
			FS_ServerQPath( IDENT_SERVER_FILE ) ), &com_serverKey, "this server" );
	}
	return com_serverKeyState == KEY_READY ? &com_serverKey : NULL;
}

/*
=================
Com_KeyFingerprint

A key's fingerprint, for messages and the start summary, or "none"
=================
*/
const char *Com_KeyFingerprint( const identKey_t *key ) {
	static char	fingerprint[IDENT_FINGERPRINT_SIZE];

	if ( !key ) {
		return "none";
	}
	Ident_Fingerprint( key->pub, fingerprint );
	return fingerprint;
}

/*
=================
Com_RememberedServerKey, Com_RememberServerKey, Com_ForgetServerKey

The keys servers are remembered by, in known_servers.txt at the home
config base's root, by address: the client's trust rule (the handshake
design, 14.5) checks a session's key against the remembered one, and the
engine alone writes them, once a session's first sealed datagram opens
=================
*/
static const char *Com_KnownServersPath( void ) {
	return FS_BaseDir_BuildOSPath( Cvar_VariableString( "fs_homeconfigpath" ), IDENT_KNOWN_FILE );
}

qboolean Com_RememberedServerKey( const char *address, byte key[IDENT_KEY_SIZE] ) {
	void		*text = NULL;
	qboolean	found;

	FS_ReadOSFile( Com_KnownServersPath(), &text );
	found = Ident_KnownServer( text, address, key );
	if ( text ) {
		Z_Free( text );
	}
	return found;
}

// the file with the address's line replaced, or left out for a NULL key
static qboolean Com_WriteServerKey( const char *address, const byte *key ) {
	char		path[MAX_OSPATH], *out;
	void		*text = NULL;
	long		length;
	int			size, written;
	qboolean	ok;

	if ( com_check ) {
		return qfalse;
	}
	Q_strncpyz( path, Com_KnownServersPath(), sizeof( path ) );
	length = FS_ReadOSFile( path, &text );
	size = (int)MAX( length, 0 ) + MAX_OSPATH + IDENT_HEX_SIZE + 4;
	out = Z_Malloc( size );
	written = Ident_KnownServerText( text, address, key, out, size );
	ok = written >= 0 && FS_ReplaceOSFile( path, out, written, qfalse );
	Z_Free( out );
	if ( text ) {
		Z_Free( text );
	}
	return ok;
}

qboolean Com_RememberServerKey( const char *address, const byte key[IDENT_KEY_SIZE] ) {
	return Com_WriteServerKey( address, key );
}

void Com_ForgetServerKey( const char *address ) {
	Com_WriteServerKey( address, NULL );
}
