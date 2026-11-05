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
A web client's network: its connections to servers through the browser
(NA_WEB, P7).

A browser has no UDP. Each connection is a WebRTC peer connection, which
net_web.js holds, to the server's gateway, with two data channels agreed
by id, so no message opens them: "q3" (id 0), unordered and never resent,
which carries Quake III's packets as UDP would, and "oob" (id 1), reliable,
for connectionless packets longer than one message carries well. The
gateway gives each packet to the server with a header naming the player.

An NA_WEB address is a connection's id, which is never reused in a page's
life, so a packet from a connection closed since can't pass for one from
its successor. Packets arrive in the page's event handlers and wait there
until NET_Sleep hands them to the client, at its next frame.
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

// net_web.js
extern int	NetWeb_Open( const char *signal, int oobAbove );
extern int	NetWeb_Send( int id, const void *data, int length );
extern int	NetWeb_Receive( void *buffer, int size, int *id );
extern int	NetWeb_Failure( int id, char *buffer, int size );
extern void	NetWeb_Close( int id );

/*
==================
NET_WebOpen
==================
*/
qboolean NET_WebOpen( const char *signal, netadr_t *adr ) {
	// a connectionless packet longer than a fragment goes on oob
	int		id = NetWeb_Open( signal, NET_WEB_FRAGMENT_SIZE );

	if ( !id ) {
		return qfalse;
	}
	Com_Memset( adr, 0, sizeof( *adr ) );
	adr->type = NA_WEB;
	adr->port = id;
	return qtrue;
}

/*
==================
NET_WebFailure
==================
*/
const char *NET_WebFailure( netadr_t adr ) {
	static char	failure[256];

	return NetWeb_Failure( adr.port, failure, sizeof( failure ) ) ? failure : NULL;
}

/*
==================
NET_WebClose
==================
*/
void NET_WebClose( netadr_t adr ) {
	NetWeb_Close( adr.port );
}

/*
==================
NET_WebSend
==================
*/
qboolean NET_WebSend( netadr_t to, const void *data, int length ) {
	return NetWeb_Send( to.port, data, length ) ? qtrue : qfalse;
}

/*
==================
NET_WebEvent
==================
*/
void NET_WebEvent( void ) {
	static byte	data[MAX_MSGLEN + 1];
	netadr_t	from;
	msg_t		msg;
	int			id, length;

	Com_Memset( &from, 0, sizeof( from ) );
	from.type = NA_WEB;
	// a byte past the longest, as NET_Event leaves
	while ( ( length = NetWeb_Receive( data, MAX_MSGLEN, &id ) ) >= 0 ) {
		MSG_Init( &msg, data, sizeof( data ) );
		msg.cursize = length;
		from.port = id;
		// always the client's: a listen server in the page is reached
		// over loopback, never through the browser
		CL_PacketEvent( from, &msg );
	}
}
