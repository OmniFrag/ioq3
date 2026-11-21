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
// RFC 1918's private ranges, a LAN's
static qboolean NET_IPv4Private( const byte *ip ) {
	return ip[0] == 10 || ( ip[0] == 172 && ( ip[1] & 0xf0 ) == 16 ) || ( ip[0] == 192 && ip[1] == 168 );
}

// IPv6's unique local fc00::/7 and link-local fe80::/10
static qboolean NET_IPv6Private( const byte *ip6 ) {
	return ( ip6[0] & 0xfe ) == 0xfc || ( ip6[0] == 0xfe && ( ip6[1] & 0xc0 ) == 0x80 );
}

// RFC 6598's shared space, 100.64.0.0/10, carriers' and VPNs' NAT
static qboolean NET_IPv4Shared( const byte *ip ) {
	return ip[0] == 100 && ( ip[1] & 0xc0 ) == 64;
}

static netScope_t NET_IPv4Scope( const byte *ip ) {
	// unspecified ("this network"), multicast, reserved and broadcast
	if ( ip[0] == 0 || ip[0] >= 224 ) {
		return NET_SCOPE_NONE;
	}
	if ( ip[0] == 127 ||								// loopback
		NET_IPv4Private( ip ) ||
		( ip[0] == 169 && ip[1] == 254 ) ||				// link-local
		NET_IPv4Shared( ip ) ) {
		return NET_SCOPE_PRIVATE;
	}
	return NET_SCOPE_PUBLIC;
}

/*
==================
NET_IsLANv4, NET_IsSharedv4

Whether an IPv4 address is in RFC 1918's private ranges, a LAN's, or in
the shared space carriers' and VPNs' NAT use (100.64.0.0/10)
==================
*/
qboolean NET_IsLANv4( const netadr_t *adr ) {
	return adr->type == NA_IP && NET_IPv4Private( adr->ip );
}

qboolean NET_IsSharedv4( const netadr_t *adr ) {
	return adr->type == NA_IP && NET_IPv4Shared( adr->ip );
}

/*
==================
NET_IsPrivate

Whether an address is in a private range many networks reuse: RFC 1918's,
IPv6's unique local fc00::/7 or its link-local fe80::/10, an IPv4-mapped
one as its IPv4. Not loopback, and not every address on one of this
computer's subnets (Sys_IsLANAddress)
==================
*/
qboolean NET_IsPrivate( const netadr_t *adr ) {
	netadr_t	a = NET_Unmapped( adr );

	return ( a.type == NA_IP && NET_IPv4Private( a.ip ) ) || ( a.type == NA_IP6 && NET_IPv6Private( a.ip6 ) );
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
// an IPv4-mapped IPv6 address's first 12 bytes (::ffff:a.b.c.d)
static const byte net_mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

netScope_t NET_AdrScope( const netadr_t *adr ) {
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

	if ( !memcmp( ip6, net_mapped, sizeof( net_mapped ) ) || !memcmp( ip6, nat64, sizeof( nat64 ) ) ) {
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

/*
==================
NET_ScopeIsLocal

Whether an address is on the network this computer is on, as a server
open to its local network alone takes a player (NET_IsLocalNetwork), by
its bytes and this computer's interfaces' subnets alone: loopback;
RFC 1918's ranges, IPv6's unique local and link-local addresses, but not
IPv4's link-local 169.254/16; a subnet of one of the interfaces, nets;
and the shared space 100.64/10 only when its traffic comes to this
computer's own address there, to (NULL where that isn't known), as a
tailnet's does and a carrier's NAT's doesn't, an interface's subnet
there notwithstanding. An IPv4-mapped IPv6 address is its IPv4; nothing
else in IPv6, NAT64, 6to4 or Teredo, is read as one
==================
*/
// an IPv4-mapped IPv6 address as its IPv4, any other as it is
netadr_t NET_Unmapped( const netadr_t *adr ) {
	netadr_t	a = *adr;

	if ( a.type == NA_IP6 && !memcmp( a.ip6, net_mapped, sizeof( net_mapped ) ) ) {
		a.type = NA_IP;
		Com_Memcpy( a.ip, adr->ip6 + 12, 4 );
	}
	return a;
}

// a netmask's prefix length: its leading 1 bits, size bytes of it; -1 for
// a mask with a 1 after a 0, which no prefix length says
int NET_MaskBits( const byte *mask, int size ) {
	int		bits = 0, i;

	while ( bits < size * 8 && ( mask[bits / 8] & ( 0x80 >> ( bits % 8 ) ) ) ) {
		bits++;
	}
	for ( i = bits; i < size * 8; i++ ) {
		if ( mask[i / 8] & ( 0x80 >> ( i % 8 ) ) ) {
			return -1;
		}
	}
	return bits;
}

// an interface's subnet, base already its address, from its mask: none
// for a mask that isn't contiguous or has no bits, either of which would
// make every address the interface's
qboolean NET_MaskSubnet( const byte *mask, int size, netSubnet_t *net ) {
	net->bits = NET_MaskBits( mask, size );
	return net->bits > 0;
}

// whether a's first bits bits are base's
static qboolean NET_InSubnet( const netadr_t *a, const netSubnet_t *net ) {
	const byte	*x = a->type == NA_IP ? a->ip : a->ip6, *y = net->base.type == NA_IP ? net->base.ip : net->base.ip6;
	int			size = a->type == NA_IP ? 4 : 16, bits = net->bits, i;

	if ( net->base.type != a->type || bits < 0 || bits > size * 8 ) {
		return qfalse;
	}
	for ( i = 0; bits >= 8; i++, bits -= 8 ) {
		if ( x[i] != y[i] ) {
			return qfalse;
		}
	}
	return !bits || ( ( x[i] ^ y[i] ) & ( 0xff << ( 8 - bits ) ) & 0xff ) == 0;
}

qboolean NET_ScopeIsLocal( const netadr_t *adr, const netadr_t *to, const netSubnet_t *nets, int count ) {
	static const byte	loopback6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	netadr_t			a = NET_Unmapped( adr ), arrived;
	int					i;

	switch ( a.type ) {
	case NA_LOOPBACK:
		return qtrue;
	case NA_IP:
		if ( NET_IPv4Shared( a.ip ) ) {
			if ( !to ) {
				return qfalse;
			}
			arrived = NET_Unmapped( to );
			return arrived.type == NA_IP && NET_IPv4Shared( arrived.ip );
		}
		if ( a.ip[0] == 127 || NET_IPv4Private( a.ip ) ) {
			return qtrue;
		}
		break;
	case NA_IP6:
		if ( !memcmp( a.ip6, loopback6, sizeof( loopback6 ) ) || NET_IPv6Private( a.ip6 ) ) {
			return qtrue;
		}
		break;
	default:
		return qfalse;
	}
	for ( i = 0; i < count; i++ ) {
		if ( NET_InSubnet( &a, &nets[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}
