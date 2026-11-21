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
// no_sockets.h -- the BSD socket calls, each failing as a system without
// sockets does, for the web build. A browser has no UDP, and Emscripten's
// emulation of sockets (SOCKFS, over WebSockets) would connect to any host
// with nothing checking its key, so the web client links none of it: a
// file that calls them includes this after <sys/socket.h>, on the web alone.
// Each argument is still evaluated, so no variable goes unused

#ifndef NO_SOCKETS_H
#define NO_SOCKETS_H

#include <errno.h>
#include <sys/socket.h>

static inline int NoSockets_Fail( void ) {
	errno = ENOSYS;
	return -1;
}

#define socket( d, t, p )				( (void)( d ), (void)( t ), (void)( p ), NoSockets_Fail() )
#define bind( s, a, l )					( (void)( s ), (void)( a ), (void)( l ), NoSockets_Fail() )
#define connect( s, a, l )				( (void)( s ), (void)( a ), (void)( l ), NoSockets_Fail() )
#define sendto( s, b, n, f, a, l )		( (void)( s ), (void)( b ), (void)( n ), (void)( f ), (void)( a ), (void)( l ), NoSockets_Fail() )
#define recvfrom( s, b, n, f, a, l )	( (void)( s ), (void)( b ), (void)( n ), (void)( f ), (void)( a ), (void)( l ), NoSockets_Fail() )
#define sendmsg( s, m, f )				( (void)( s ), (void)( m ), (void)( f ), NoSockets_Fail() )
#define recvmsg( s, m, f )				( (void)( s ), (void)( m ), (void)( f ), NoSockets_Fail() )
#define setsockopt( s, v, o, p, l )		( (void)( s ), (void)( v ), (void)( o ), (void)( p ), (void)( l ), NoSockets_Fail() )
#define getsockopt( s, v, o, p, l )		( (void)( s ), (void)( v ), (void)( o ), (void)( p ), (void)( l ), NoSockets_Fail() )
#define getsockname( s, a, l )			( (void)( s ), (void)( a ), (void)( l ), NoSockets_Fail() )
#define getpeername( s, a, l )			( (void)( s ), (void)( a ), (void)( l ), NoSockets_Fail() )
#define listen( s, n )					( (void)( s ), (void)( n ), NoSockets_Fail() )
#define accept( s, a, l )				( (void)( s ), (void)( a ), (void)( l ), NoSockets_Fail() )
#define shutdown( s, h )				( (void)( s ), (void)( h ), NoSockets_Fail() )
// and the C library's calls that open a socket themselves: musl's
// if_nametoindex asks the interface's index of one. 0 is no such interface
#define if_nametoindex( n )				( (void)( n ), 0u )

#endif
