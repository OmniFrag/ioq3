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
===========================================================================
*/
// net_proxy.h -- the PROXY protocol's version 2 header (HAProxy's published
// format) on UDP datagrams, as a relay in front of a server sends it with
// each player's packet and the server sends it back with each reply (P8,
// net_ip.c). No globals: tests/unit and tests/fuzz link this file alone.

#ifndef NET_PROXY_H
#define NET_PROXY_H

#define PROXY_SIGNATURE_SIZE	12
#define PROXY_HEADER_SIZE		16		// signature, version and command, family, length
#define PROXY_MAX_HEADER		( PROXY_HEADER_SIZE + 36 + 512 )	// IPv6's addresses, and TLVs past them
#define PROXY_MAX_WRITTEN		( PROXY_HEADER_SIZE + 36 )		// what NET_ProxyWrite writes, at most
#define PROXY_ADDRESSES( family )	( ( family ) == 6 ? 36 : 12 )		// a datagram's addresses and ports

typedef enum {
	PROXY_LOCAL,		// the proxy's own: a health check, carrying no address
	PROXY_FORWARDED		// a datagram from src to dst, which the proxy passed on
} proxyCommand_t;

typedef struct proxyHeader_s {
	proxyCommand_t	command;
	int				family;			// 4 or 6, for PROXY_FORWARDED
	byte			src[16];		// the sender's address: its first 4 bytes for IPv4
	byte			dst[16];		// the address it sent to, the proxy's own
	unsigned short	srcPort;		// in host order
	unsigned short	dstPort;
	int				tlvs;			// where the TLVs past the addresses start, from the header's start
	int				tlvLength;		// and their bytes, 0 for none
} proxyHeader_t;

// data's header: its length, which the datagram's payload follows; 0 for
// no header at all (no signature); -1 for a malformed one, or one that isn't
// a datagram's over IPv4 or IPv6
int		NET_ProxyRead( const byte *data, int length, proxyHeader_t *header );
// a PROXY_FORWARDED header into out: its length, or 0 if size is too small
int		NET_ProxyWrite( byte *out, int size, const proxyHeader_t *header );
// the same with tlvLength bytes of TLVs past the addresses, the header's
// length counting them
int		NET_ProxyWriteTLVs( byte *out, int size, const proxyHeader_t *header, const byte *tlvs, int tlvLength );

#endif
