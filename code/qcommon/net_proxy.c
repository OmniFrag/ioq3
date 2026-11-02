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
// net_proxy.c -- the PROXY protocol's version 2 header on datagrams
// (net_proxy.h)

#include "q_shared.h"
#include "net_proxy.h"

static const byte proxySignature[PROXY_SIGNATURE_SIZE] = {
	0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a
};

int NET_ProxyRead( const byte *data, int length, proxyHeader_t *header ) {
	int	addresses, total, size;

	Com_Memset( header, 0, sizeof( *header ) );
	if ( length < PROXY_SIGNATURE_SIZE || memcmp( data, proxySignature, PROXY_SIGNATURE_SIZE ) ) {
		return 0;
	}
	if ( length < PROXY_HEADER_SIZE || ( data[12] >> 4 ) != 2 ) {
		return -1;
	}
	addresses = ( data[14] << 8 ) | data[15];
	total = PROXY_HEADER_SIZE + addresses;
	if ( total > length || total > PROXY_MAX_HEADER ) {
		return -1;
	}
	switch ( data[12] & 0x0f ) {
	case 0:
		// LOCAL: the proxy's own, whatever its family says
		header->command = PROXY_LOCAL;
		return total;
	case 1:
		header->command = PROXY_FORWARDED;
		break;
	default:
		return -1;
	}
	// DGRAM over INET or INET6; the addresses, then TLVs, which are skipped
	switch ( data[13] ) {
	case 0x12:
		header->family = 4;
		size = 4;
		break;
	case 0x22:
		header->family = 6;
		size = 16;
		break;
	default:
		return -1;
	}
	if ( addresses < 2 * size + 4 ) {
		return -1;
	}
	data += PROXY_HEADER_SIZE;
	Com_Memcpy( header->src, data, size );
	Com_Memcpy( header->dst, data + size, size );
	header->srcPort = (unsigned short)( ( data[2 * size] << 8 ) | data[2 * size + 1] );
	header->dstPort = (unsigned short)( ( data[2 * size + 2] << 8 ) | data[2 * size + 3] );
	return total;
}

int NET_ProxyWrite( byte *out, int size, const proxyHeader_t *header ) {
	int	address = header->family == 6 ? 16 : 4;
	int	total = PROXY_HEADER_SIZE + 2 * address + 4;

	if ( size < total || ( header->family != 4 && header->family != 6 ) ) {
		return 0;
	}
	Com_Memcpy( out, proxySignature, PROXY_SIGNATURE_SIZE );
	out[12] = 0x21;									// version 2, PROXY
	out[13] = header->family == 6 ? 0x22 : 0x12;	// DGRAM over INET6 or INET
	out[14] = (byte)( ( total - PROXY_HEADER_SIZE ) >> 8 );
	out[15] = (byte)( total - PROXY_HEADER_SIZE );
	out += PROXY_HEADER_SIZE;
	Com_Memcpy( out, header->src, address );
	Com_Memcpy( out + address, header->dst, address );
	out[2 * address] = (byte)( header->srcPort >> 8 );
	out[2 * address + 1] = (byte)header->srcPort;
	out[2 * address + 2] = (byte)( header->dstPort >> 8 );
	out[2 * address + 3] = (byte)header->dstPort;
	return total;
}
