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
// net_scope.c -- which network an address is on, by its own bytes alone

#include "q_shared.h"
#include "qcommon.h"

/*
==================
NET_IPv4Scope
==================
*/
static netScope_t NET_IPv4Scope( const byte *ip ) {
	// unspecified ("this network"), multicast, reserved and broadcast
	if ( ip[0] == 0 || ip[0] >= 224 ) {
		return NET_SCOPE_NONE;
	}
	if ( ip[0] == 127 ||								// loopback
		ip[0] == 10 ||									// private
		( ip[0] == 172 && ( ip[1] & 0xf0 ) == 16 ) ||
		( ip[0] == 192 && ip[1] == 168 ) ||
		( ip[0] == 169 && ip[1] == 254 ) ||				// link-local
		( ip[0] == 100 && ( ip[1] & 0xc0 ) == 64 ) ) {	// shared, as carriers' and VPNs' NAT
		return NET_SCOPE_PRIVATE;
	}
	return NET_SCOPE_PUBLIC;
}

/*
==================
NET_AdrScope

Whether an address is on the public internet, a private network (a LAN,
loopback, link-local, or a carrier's or VPN's shared space), or none to
connect to (unspecified, multicast or broadcast). An IPv6 address that
holds an IPv4 one, mapped or through NAT64, is that one's
==================
*/
netScope_t NET_AdrScope( const netadr_t *adr ) {
	static const byte mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
	static const byte nat64[12] = { 0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0 };
	static const byte zero[15] = { 0 };
	const byte	*ip6 = adr->ip6;

	switch ( adr->type ) {
	case NA_LOOPBACK:
		return NET_SCOPE_PRIVATE;
	case NA_IP:
		return NET_IPv4Scope( adr->ip );
	case NA_IP6:
		break;
	default:
		return NET_SCOPE_NONE;
	}

	if ( !memcmp( ip6, mapped, sizeof( mapped ) ) || !memcmp( ip6, nat64, sizeof( nat64 ) ) ) {
		return NET_IPv4Scope( ip6 + 12 );
	}
	if ( !memcmp( ip6, zero, sizeof( zero ) ) ) {
		// :: is unspecified, ::1 loopback
		return ip6[15] == 1 ? NET_SCOPE_PRIVATE : NET_SCOPE_NONE;
	}
	if ( ip6[0] == 0xff ) {								// multicast
		return NET_SCOPE_NONE;
	}
	if ( ( ip6[0] & 0xfe ) == 0xfc ||					// unique local
		( ip6[0] == 0xfe && ( ip6[1] & 0x80 ) == 0x80 ) ) {	// link-local, and the old site-local
		return NET_SCOPE_PRIVATE;
	}
	return NET_SCOPE_PUBLIC;
}

/*
==================
NET_AdrReachableFrom

Whether a host on the network from, such as a game server, may send a
client to adr, such as for a download: an address on the public internet,
or one on a private network when from is one too, as on a LAN. So a server
on the internet can't send its players to their own networks
==================
*/
qboolean NET_AdrReachableFrom( const netadr_t *adr, netScope_t from ) {
	switch ( NET_AdrScope( adr ) ) {
	case NET_SCOPE_PUBLIC:
		return qtrue;
	case NET_SCOPE_PRIVATE:
		return from != NET_SCOPE_PUBLIC;
	default:
		return qfalse;
	}
}
