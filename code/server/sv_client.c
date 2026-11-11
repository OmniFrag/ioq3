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
// sv_client.c -- server code for dealing with clients

#include "server.h"
#include "../qcommon/identity.h"
#include "../thirdparty/monocypher-4.0.2/monocypher.h"
#include "../thirdparty/monocypher-4.0.2/monocypher-ed25519.h"

static void SV_CloseDownload( client_t *cl );

/*
=================
SV_AskedExtensions

The extension bits a client's tokens, the command's arguments from
firstArg on, ask for that this server has too; 0 when they ask for none,
or are malformed
=================
*/
uint32_t SV_AskedExtensions( int firstArg ) {
	const char	*error;
	uint32_t	bits;

	error = Com_ParseExtensionBitsAndKey( Cmd_ArgsFrom( firstArg ), &bits, NULL );
	if ( error ) {
		Com_DPrintf( "Ignored a client's extension tokens: %s\n", error );
		return 0;
	}
	return bits & OFX_BUILD;
}

/*
=================
SV_AskedFragmentSize

The fragment size a client's tokens, the command's arguments from firstArg
on, ask for (off): a transport of its own that can't carry the classic
fragment whole. 0 when they ask for none, or are malformed
=================
*/
static int SV_AskedFragmentSize( int firstArg ) {
	extTokens_t	tokens;

	if ( Com_ParseExtensionTokens( Cmd_ArgsFrom( firstArg ), &tokens ) ) {
		return 0;
	}
	return Com_ParseFragmentSize( Com_ExtensionValue( &tokens, "off" ) );
}

/*
=================
Stateless challenges (ENG-067)

A challenge is a MAC of the client's address and port, and the fragment
size it asked for, under a secret the server keeps for one slot of
SV_CHALLENGE_SLOT ms and the slot after it, so a challenge holds for 16 to
32 seconds and the server keeps nothing for it: a flood of getchallenges
from forged addresses fills no table, and the challenge still shows that
a connect comes from the address it was sent to.

What else the challenge agreed, the extensions (ofx) and with OFX_IDENTITY
the client's nonce (ofc), the client repeats on its connect, with the off
it asked for and ofv, a MAC of them on the challenge that challengeResponse
sent (docs/contracts.md). The server's nonce (ofn) and the connection's
content key are MACs of the same terms, so every connect on one challenge
is sent the same key. A connect without ofv, a stock client's or an older
OmniFrag client's, or one whose terms don't match it, connects plainly.
=================
*/

#define SV_CHALLENGE_SLOT	16384	// ms
#define SV_CHALLENGE_SECRET	32
#define SV_CHALLENGE_TAG	16		// bytes of ofv, as 32 hex digits

// slots in Sys_Milliseconds' range, which wraps as an unsigned int
#define SV_CHALLENGE_SLOTS	( 1u << ( 32 - 14 ) )

// the secrets challenges are made with, by their slot's parity: this
// slot's and the last one's
static struct {
	qboolean		ready;
	unsigned int	slot;
	byte			secrets[2][SV_CHALLENGE_SECRET];
} challengeSecrets;

// one challenge and what it agreed
typedef struct {
	netadr_t	adr;
	char		address[NET_ADDRSTRMAXLEN + 8];	// adr as the MACs hash it, with its port
	int			challenge;
	int			secret;				// the one it was made with: its slot's parity
	uint32_t	ofx;				// the extensions agreed (exttokens.h), 0 for none
	int			fragmentSize;		// the client's off, which challengeResponse repeats, 0 for none
	// with OFX_IDENTITY, the nonces the proofs sign (identity.h): the
	// client's ofc, and this server's ofn
	byte		clientNonce[IDENT_KEY_SIZE];
	byte		serverNonce[IDENT_KEY_SIZE];
	// from the terms (SV_ChallengeTerms): ofv, which binds them, and the
	// content key, so each connect on the challenge is sent the same
	byte		tag[SV_CHALLENGE_TAG];
	uint64_t	contentKey;
} challenge_t;

/*
=================
SV_ChallengeSecret
=================
*/
static void SV_ChallengeSecret( byte secret[SV_CHALLENGE_SECRET] ) {
	static qboolean	warned;

	if ( !Com_RandomSecret( secret, SV_CHALLENGE_SECRET ) ) {
		if ( !warned ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: no secure randomness for the challenges' secret; "
				"forged addresses may be able to connect\n" );
			warned = qtrue;
		}
		Com_RandomBytes( secret, SV_CHALLENGE_SECRET );
	}
}

/*
=================
SV_ChallengeRotate

Makes a new secret each slot, keeping the last slot's, so a challenge made
just before the change still holds; after a longer gap both are new. The
slot's parity says which is which, and the challenge carries it. Each
challenge calls it, which is all it needs; each frame does too, so the
change is logged on time while nobody asks for one (tests/server/challenge.sh
waits for that line)
=================
*/
void SV_ChallengeRotate( void ) {
	unsigned int	slot = (unsigned int)Sys_Milliseconds() / SV_CHALLENGE_SLOT;

	if ( challengeSecrets.ready && slot == challengeSecrets.slot ) {
		return;
	}
	// the next slot, also as the clock wraps, overwrites the one before last
	if ( !challengeSecrets.ready || ( ( slot - challengeSecrets.slot ) & ( SV_CHALLENGE_SLOTS - 1 ) ) != 1 ) {
		SV_ChallengeSecret( challengeSecrets.secrets[!( slot & 1 )] );
	}
	SV_ChallengeSecret( challengeSecrets.secrets[slot & 1] );
	challengeSecrets.slot = slot;
	challengeSecrets.ready = qtrue;
	Com_DPrintf( "Challenge secret rotated\n" );
}

/*
=================
SV_ChallengeMAC

HMAC-SHA-512 under one of the secrets of what, the address and port (as
SV_ChallengeAddress gives them) and the fragment size; with terms, the
challenge, its ofx and with OFX_IDENTITY its ofc too
=================
*/
static void SV_ChallengeMAC( const char *what, int secret, const challenge_t *challenge,
	qboolean terms, byte mac[64] ) {
	crypto_sha512_hmac_ctx	ctx;
	byte					numbers[10], type = (byte)challenge->adr.type;
	uint32_t				number = (uint32_t)challenge->challenge;

	crypto_sha512_hmac_init( &ctx, challengeSecrets.secrets[secret], SV_CHALLENGE_SECRET );
	crypto_sha512_hmac_update( &ctx, (const byte *)what, strlen( what ) + 1 );
	crypto_sha512_hmac_update( &ctx, &type, 1 );
	crypto_sha512_hmac_update( &ctx, (const byte *)challenge->address, strlen( challenge->address ) + 1 );
	numbers[0] = (byte)( challenge->fragmentSize >> 8 );
	numbers[1] = (byte)challenge->fragmentSize;
	if ( !terms ) {
		crypto_sha512_hmac_update( &ctx, numbers, 2 );
	} else {
		numbers[2] = (byte)( number >> 24 );
		numbers[3] = (byte)( number >> 16 );
		numbers[4] = (byte)( number >> 8 );
		numbers[5] = (byte)number;
		numbers[6] = (byte)( challenge->ofx >> 24 );
		numbers[7] = (byte)( challenge->ofx >> 16 );
		numbers[8] = (byte)( challenge->ofx >> 8 );
		numbers[9] = (byte)challenge->ofx;
		crypto_sha512_hmac_update( &ctx, numbers, 10 );
		if ( challenge->ofx & OFX_IDENTITY ) {
			crypto_sha512_hmac_update( &ctx, challenge->clientNonce, IDENT_KEY_SIZE );
		}
	}
	crypto_sha512_hmac_final( &ctx, mac );
}

/*
=================
SV_ChallengeAddress

A challenge for adr, its fields cleared and its address as the MACs hash it
=================
*/
static void SV_ChallengeAddress( challenge_t *challenge, netadr_t adr ) {
	Com_Memset( challenge, 0, sizeof( *challenge ) );
	challenge->adr = adr;
	Q_strncpyz( challenge->address, NET_AdrToStringwPort( adr ), sizeof( challenge->address ) );
}

/*
=================
SV_MakeChallenge

The challenge for the address and fragment size under one of the secrets:
30 bits of MAC, never 0, and above them the secret's parity, so the
number stays positive
=================
*/
static int SV_MakeChallenge( int secret, const challenge_t *challenge ) {
	byte			mac[64];
	unsigned int	number;

	SV_ChallengeMAC( "challenge", secret, challenge, qfalse, mac );
	number = ( (unsigned int)mac[0] << 24 | mac[1] << 16 | mac[2] << 8 | mac[3] ) & 0x3fffffff;
	return (int)( ( number ? number : 1 ) | (unsigned int)secret << 30 );
}

/*
=================
SV_ChallengeTerms

ofv, ofn and the content key, from one MAC of the challenge's terms under
its secret: the same on every connect on it, and the key never 0
=================
*/
static void SV_ChallengeTerms( challenge_t *challenge ) {
	byte	mac[64];
	int		i;

	SV_ChallengeMAC( "terms", challenge->secret, challenge, qtrue, mac );
	Com_Memcpy( challenge->tag, mac, SV_CHALLENGE_TAG );
	Com_Memcpy( challenge->serverNonce, mac + SV_CHALLENGE_TAG, IDENT_KEY_SIZE );
	challenge->contentKey = 0;
	for ( i = 0; i < 8; i++ ) {
		challenge->contentKey = challenge->contentKey << 8 | mac[SV_CHALLENGE_TAG + IDENT_KEY_SIZE + i];
	}
	if ( !challenge->contentKey ) {
		challenge->contentKey = 1;
	}
}

/*
=================
SV_SendChallengeResponse

The challenge, echoing the client's, with the extensions agreed after it
as tokens, and ofv, which binds them. A client that asked for none, as
every classic client, is sent the classic reply
=================
*/
static void SV_SendChallengeResponse( const challenge_t *challenge, int clientChallenge ) {
	char	tokens[EXT_MAX_STRING];
	char	tag[SV_CHALLENGE_TAG * 2 + 1];

	tokens[0] = '\0';
	if ( challenge->ofx ) {
		Com_AppendExtensionToken( tokens, sizeof( tokens ), "ofx", va( "%x", challenge->ofx ) );
	}
	// the fragment size it asked for, which this server splits at too
	if ( challenge->fragmentSize ) {
		Com_AppendExtensionToken( tokens, sizeof( tokens ), "off", va( "%d", challenge->fragmentSize ) );
	}
	// this server's key, proved over both nonces (identity.h)
	if ( challenge->ofx & OFX_IDENTITY ) {
		const identKey_t	*key = Com_ServerKey();
		byte				sig[IDENT_SIG_SIZE];

		Ident_SignServer( key, challenge->clientNonce, challenge->serverNonce, sig );
		Ident_AppendHex( tokens, sizeof( tokens ), "ofn", challenge->serverNonce );
		Ident_AppendHex( tokens, sizeof( tokens ), "ofp", key->pub );
		Ident_AppendSig( tokens, sizeof( tokens ), sig );
	}
	if ( challenge->ofx || challenge->fragmentSize ) {
		Com_BytesToHex( challenge->tag, SV_CHALLENGE_TAG, tag );
		Com_AppendExtensionToken( tokens, sizeof( tokens ), "ofv", tag );
	}
	NET_OutOfBandPrint( NS_SERVER, challenge->adr, "challengeResponse %d %d %d%s",
		challenge->challenge, clientChallenge, com_protocol->integer, tokens );
}

/*
=================
SV_GetChallenge

A "getchallenge" OOB command has been received
Returns a challenge number that can be used
in a subsequent connectResponse command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.

ioquake3: we added a possibility for clients to add a challenge
to their packets, to make it more difficult for malicious servers
to hi-jack client connections.

OmniFrag: the challenge is stateless, and id's authorize server, gone for
years, is no longer asked: while it couldn't be reached, which was always
since, everyone was let in (ENG-067)
=================
*/
void SV_GetChallenge( netadr_t from ) {
	challenge_t	challenge;
	char		*gameName;
	qboolean	gameMismatch;

	// ignore if we are in single player
	if ( SV_ExtGametype() == GT_SINGLE_PLAYER || Cvar_VariableValue("ui_singlePlayerActive")) {
		return;
	}

	// Prevent using getchallenge as an amplifier. The reply is about
	// the request's size, so there's no limit for all addresses
	// together, which getinfo and getstatus requests would use up
	if ( SVC_RateLimitAddress( from, 10, 1000 ) ) {
		Com_DPrintf( "SV_GetChallenge: rate limit from %s exceeded, dropping request\n",
			NET_AdrToString( from ) );
		return;
	}

	gameName = Cmd_Argv(2);

#ifdef LEGACY_PROTOCOL
	// gamename is optional for legacy protocol
	if (com_legacyprotocol->integer && !*gameName)
		gameMismatch = qfalse;
	else
#endif
		gameMismatch = !*gameName || strcmp(gameName, com_gamename->string) != 0;

	// reject client if the gamename string sent by the client doesn't match ours
	if (gameMismatch)
	{
		NET_OutOfBandPrint(NS_SERVER, from, "print\nGame mismatch: This is a %s server\n",
			com_gamename->string);
		return;
	}

	SV_ChallengeRotate();
	SV_ChallengeAddress( &challenge, from );
	// the extensions its tokens ask for, after the game name
	challenge.ofx = SV_AskedExtensions( 3 );
	challenge.fragmentSize = SV_AskedFragmentSize( 3 );
	challenge.secret = (int)( challengeSecrets.slot & 1 );
	challenge.challenge = SV_MakeChallenge( challenge.secret, &challenge );
	// a key's proof needs the client's nonce, this server's key and a
	// nonce of its own: without one of them, the connection proves nothing.
	// Each is a signature, so all addresses together get 50 a second,
	// which forged ones can't spend past: the rest are answered unproven
	if ( challenge.ofx & OFX_IDENTITY ) {
		static rateLimit_t	signedReplies;
		extTokens_t			tokens;

		if ( SVC_RateLimit( &signedReplies, 50, 1000 ) || Com_ParseExtensionTokens( Cmd_ArgsFrom( 3 ), &tokens ) ||
			!Ident_ReadNonce( &tokens, "ofc", challenge.clientNonce ) || !Com_ServerKey() ) {
			challenge.ofx &= ~OFX_IDENTITY;
		}
	}
	SV_ChallengeTerms( &challenge );

	SV_SendChallengeResponse( &challenge, atoi( Cmd_Argv( 1 ) ) );
}

/*
=================
SV_CheckChallenge

Whether a connect's challenge is one this server sent to its address, in
this slot or the last (its parity says which), filling in what it agreed. The connect's own off
is tried, then none and the web client's, so a connect that doesn't repeat
it, an older OmniFrag web client's, still splits at the size it asked for.
Its other terms count only with an ofv that holds for them; without, it
connects plainly
=================
*/
static qboolean SV_CheckChallenge( netadr_t from, int number, qboolean extensions, challenge_t *challenge ) {
	extTokens_t	tokens;
	qboolean	repeated = qfalse;
	byte		tag[SV_CHALLENGE_TAG];
	int			asked = -1, sizes[3], s;

	SV_ChallengeRotate();
	SV_ChallengeAddress( challenge, from );
	challenge->challenge = number;
	challenge->secret = ( number >> 30 ) & 1;

	// the terms the connect repeats after its userinfo, with their ofv
	if ( extensions && !Com_ParseExtensionTokens( Cmd_ArgsFrom( 2 ), &tokens ) ) {
		const char	*ofv = Com_ExtensionValue( &tokens, "ofv" );
		const char	*ofx = Com_ExtensionValue( &tokens, "ofx" );

		// no ofx is none agreed
		repeated = ofv && Com_HexToBytes( ofv, tag, SV_CHALLENGE_TAG ) &&
			( !ofx || Com_ParseExtensionBits( ofx, &challenge->ofx ) );
		challenge->ofx &= OFX_BUILD;
		asked = Com_ParseFragmentSize( Com_ExtensionValue( &tokens, "off" ) );
		if ( ( challenge->ofx & OFX_IDENTITY ) && !Ident_ReadNonce( &tokens, "ofc", challenge->clientNonce ) ) {
			repeated = qfalse;
		}
	}
	sizes[0] = asked;
	sizes[1] = 0;
	sizes[2] = NET_WEB_FRAGMENT_SIZE;

	for ( s = 0; s < 3; s++ ) {
		if ( sizes[s] < 0 || ( s && sizes[s] == asked ) ) {
			continue;
		}
		challenge->fragmentSize = sizes[s];
		if ( SV_MakeChallenge( challenge->secret, challenge ) != number ) {
			continue;
		}
		// the size it repeated, with a tag that holds for its terms
		if ( repeated && s == 0 ) {
			SV_ChallengeTerms( challenge );
			if ( !crypto_verify16( tag, challenge->tag ) ) {
				return qtrue;
			}
			Com_Printf( "%s: extension tokens that don't match the challenge's; connecting without them\n",
				NET_AdrToString( from ) );
		}
		challenge->ofx = 0;
		return qtrue;
	}
	return qfalse;
}

/*
==================
SV_IsBanned

Check whether a certain address is banned
==================
*/

static qboolean SV_IsBanned(netadr_t *from, qboolean isexception)
{
	int index;
	serverBan_t *curban;
	
	if(!isexception)
	{
		// If this is a query for a ban, first check whether the client is excepted
		if(SV_IsBanned(from, qtrue))
			return qfalse;
	}
	
	for(index = 0; index < serverBansCount; index++)
	{
		curban = &serverBans[index];
		
		if(curban->isexception == isexception)
		{
			if(NET_CompareBaseAdrMask(curban->ip, *from, curban->subnet))
				return qtrue;
		}
	}
	
	return qfalse;
}

/*
==================
SV_ExtDropBanned

Drop every client the bans would now refuse, for extensions
(qcommon/hooks.h)
==================
*/
int SV_ExtDropBanned( const char *reason ) {
	client_t	*cl;
	int		i, count = 0;

	if ( !svs.clients ) {
		return 0;
	}
	for ( i = 0, cl = svs.clients; i < svs.maxclients; i++, cl++ ) {
		if ( cl->state >= CS_CONNECTED && ( cl->netchan.remoteAddress.type == NA_IP ||
			cl->netchan.remoteAddress.type == NA_IP6 ) && SV_IsBanned( &cl->netchan.remoteAddress, qfalse ) ) {
			SV_DropClient( cl, reason );
			count++;
		}
	}
	return count;
}

/*
==================
SV_RefuseConnect

Tell a client why its connect was refused; OmniFrag's client asks again
for the refusals that waiting can change, by their words (CL_RefusalEnds),
so keep those as they are. A connect's source address
can be forged, so the replies to all connects together are limited, and
can't be used to flood another address.
==================
*/
static Q_PRINTF_FUNC(2, 3) void QDECL SV_RefuseConnect( netadr_t to, const char *fmt, ... ) {
	static rateLimit_t bucket;
	va_list		argptr;
	char		message[MAX_STRING_CHARS];

	if ( SVC_RateLimit( &bucket, 10, 200 ) ) {
		Com_DPrintf( "SV_RefuseConnect: rate limit exceeded, not replying to %s\n",
			NET_AdrToString( to ) );
		return;
	}

	va_start( argptr, fmt );
	Q_vsnprintf( message, sizeof( message ), fmt, argptr );
	va_end( argptr );

	NET_OutOfBandPrint( NS_SERVER, to, "print\n%s", message );
}

/*
==================
SV_OldestFreeClient

The free slot in [startIndex, endIndex) let go longest ago, a never used
one first, or NULL: a client number isn't handed on at once, to a player or a
bot, whose scoreboard line and state would be taken for the last one's
(as Quake3e, ec-'s 64430ef9, does for players)
==================
*/
client_t *SV_OldestFreeClient( int startIndex, int endIndex ) {
	client_t	*cl, *oldest = NULL;
	int			i;

	if ( startIndex < 0 ) {
		startIndex = 0;
	}
	if ( endIndex > svs.maxclients ) {
		endIndex = svs.maxclients;
	}
	for ( i = startIndex, cl = svs.clients + startIndex; i < endIndex; i++, cl++ ) {
		if ( cl->state == CS_FREE && ( !oldest || cl->lastDisconnectTime < oldest->lastDisconnectTime ) ) {
			oldest = cl;
		}
	}
	return oldest;
}

/*
==================
SV_DirectConnect

A "connect" OOB command has been received
==================
*/

/*
==================
SV_ClientProof

The device key a connect's tokens prove, on the challenge it answers
(identity.h): qfalse for none, or a proof that doesn't hold, which leaves
the client unproven but connecting
==================
*/
static qboolean SV_ClientProof( const challenge_t *challenge, byte identity[IDENT_KEY_SIZE] ) {
	extTokens_t			tokens;
	byte				sig[IDENT_SIG_SIZE];
	const identKey_t	*key = Com_ServerKey();

	if ( !key || Com_ParseExtensionTokens( Cmd_ArgsFrom( 2 ), &tokens ) ||
		!Ident_ReadClientProof( &tokens, identity, sig ) ) {
		return qfalse;
	}
	if ( !Ident_CheckClient( identity, key->pub, challenge->serverNonce, challenge->clientNonce,
		challenge->challenge, sig ) ) {
		Com_Printf( "%s: a device key's proof that doesn't hold; connecting without it\n",
			NET_AdrToString( challenge->adr ) );
		return qfalse;
	}
	return qtrue;
}

void SV_DirectConnect( netadr_t from ) {
	char		userinfo[MAX_INFO_STRING];
	int			i;
	client_t	*cl, *newcl;
	client_t	temp;
	sharedEntity_t *ent;
	int			clientNum;
	int			version;
	int			qport;
	int			challenge;
	char		*password;
	int			startIndex;
	intptr_t		denied;
	int			count;
	char		*ip;
	uint32_t	ofx = 0;
	uint64_t	contentKey = 0;
	int			fragmentSize = 0;
	qboolean	identityProven = qfalse;
	byte		identity[IDENT_KEY_SIZE];
	char		tokens[EXT_MAX_STRING];
	char		key[OFK_DIGITS + 1];
	char		hex[SHA256_HEX_SIZE];
	const byte	*sha256;
#ifdef LEGACY_PROTOCOL
	qboolean	compat = qfalse;
#endif

	Com_DPrintf ("SVC_DirectConnect ()\n");
	
	// Check whether this client is banned.
	if(SV_IsBanned(&from, qfalse))
	{
		SV_RefuseConnect(from, "You are banned from this server.\n");
		return;
	}

	Q_strncpyz( userinfo, Cmd_Argv(1), sizeof(userinfo) );

	version = atoi(Info_ValueForKey(userinfo, "protocol"));
	
#ifdef LEGACY_PROTOCOL
	if(version > 0 && com_legacyprotocol->integer == version)
		compat = qtrue;
	else
#endif
	{
		if(version != com_protocol->integer)
		{
			SV_RefuseConnect(from, "Server uses protocol version %i "
					   "(yours is %i).\n", com_protocol->integer, version);
			Com_DPrintf("    rejected connect from version %i\n", version);
			return;
		}
	}

	challenge = atoi( Info_ValueForKey( userinfo, "challenge" ) );
	qport = atoi( Info_ValueForKey( userinfo, "qport" ) );

	// quick reject
	for (i=0,cl=svs.clients ; i < svs.maxclients ; i++,cl++) {
		if ( cl->state == CS_FREE ) {
			continue;
		}
		if ( NET_CompareAdr( from, cl->netchan.remoteAddress ) ) {
			if (( svs.time - cl->lastConnectTime) 
				< (sv_reconnectlimit->integer * 1000)) {
				Com_DPrintf ("%s:reconnect rejected : too soon\n", NET_AdrToString (from));
				return;
			}
			break;
		}
	}
	
	// don't let "ip" overflow userinfo string
	if ( NET_IsLocalAddress (from) )
		ip = "localhost";
	else
		ip = (char *)NET_AdrToString( from );
	if( ( strlen( ip ) + strlen( userinfo ) + 4 ) >= MAX_INFO_STRING ) {
		SV_RefuseConnect( from,
			"Userinfo string length exceeded.  "
			"Try removing setu cvars from your config.\n" );
		return;
	}
	Info_SetValueForKey( userinfo, "ip", ip );

	// see if the challenge is valid (LAN clients don't need to challenge)
	if (!NET_IsLocalAddress(from))
	{
		challenge_t	agreed;
		qboolean	extensions = qtrue;

#ifdef LEGACY_PROTOCOL
		// the extensions agreed on its challenge; never with the legacy protocol
		extensions = !compat;
#endif
		if ( !SV_CheckChallenge( from, challenge, extensions, &agreed ) ) {
			SV_RefuseConnect( from, "No or bad challenge for your address.\n" );
			return;
		}
		Com_Printf( "%s connecting\n", NET_AdrToString( from ) );

		if ( extensions ) {
			ofx = agreed.ofx;
			// made from the challenge's terms, so a connect sent twice on
			// the challenge is sent the same
			if ( ofx ) {
				contentKey = agreed.contentKey;
			}
			fragmentSize = agreed.fragmentSize;
			identityProven = ( ofx & OFX_IDENTITY ) && SV_ClientProof( &agreed, identity );
		}

		// the challenge shows the address is real, so this connect
		// doesn't count against it
		SVC_RateRestoreBurstAddress( from, 10, 1000 );
	}

	newcl = &temp;
	Com_Memset (newcl, 0, sizeof(client_t));

	// if there is already a slot for this address, port and qport, reuse it
	for (i=0,cl=svs.clients ; i < svs.maxclients ; i++,cl++) {
		if ( cl->state == CS_FREE ) {
			continue;
		}
		if ( NET_CompareAdr( from, cl->netchan.remoteAddress )
			&& cl->netchan.qport == qport ) {
			Com_Printf ("%s:reconnect\n", NET_AdrToString (from));
			newcl = cl;

			if ( newcl->state >= CS_CONNECTED ) {
				// the old connection leaves, though nobody is told
				Hooks_ClientDropped( newcl - svs.clients, "reconnected" );

				// disconnect the client from the game first so any flags the
				// player might have are dropped, and the game doesn't keep
				// the player if it refuses the connect
				VM_Call( gvm, GAME_CLIENT_DISCONNECT, newcl - svs.clients );

				// free what the old connection holds, such as a download's
				// file handle, before the slot is cleared
				SV_FreeClient( newcl );
			}
			goto gotnewcl;
		}
	}

	// a draining server takes no new player: the ones on it reconnect above
	if ( sv_drainReason[0] ) {
		SV_RefuseConnect( from, "%s\n", sv_drainReason );
		return;
	}

	// a new player from an address with sv_maxclientsPerIP players already
	// is refused (bots aside; a reconnect took its slot above)
	if ( sv_maxclientsPerIP->integer ) {
		count = 0;
		for ( i = 0, cl = svs.clients; i < svs.maxclients; i++, cl++ ) {
			if ( cl->state >= CS_CONNECTED && cl->netchan.remoteAddress.type != NA_BOT &&
				NET_CompareBaseAdr( from, cl->netchan.remoteAddress ) ) {
				count++;
			}
		}
		if ( count >= sv_maxclientsPerIP->integer ) {
			Com_Printf( "%s refused: %d players connect from its address already (sv_maxclientsPerIP)\n",
				NET_AdrToString( from ), count );
			SV_RefuseConnect( from, "Too many connections.\n" );
			return;
		}
	}

	// find a client slot
	// if "sv_privateClients" is set > 0, then that number
	// of client slots will be reserved for connections that
	// have "password" set to the value of "sv_privatePassword"
	// Info requests will report the maxclients as if the private
	// slots didn't exist, to prevent people from trying to connect
	// to a full server.
	// This is to allow us to reserve a couple slots here on our
	// servers so we can play without having to kick people.

	// check for privateClient password
	password = Info_ValueForKey( userinfo, "password" );
	if ( ( *password && !strcmp( password, sv_privatePassword->string ) ) ||
		// an admin's device, as the keeper of roles says (hooks.h)
		( identityProven && Hooks_ClientRole( -1, identity ) >= ROLE_ADMIN ) ) {
		startIndex = 0;
	} else {
		// skip past the reserved slots
		startIndex = sv_privateClients->integer;
	}

	// one with the password takes a reserved slot while there is one,
	// leaving the public ones to everyone else
	newcl = startIndex == 0 ? SV_OldestFreeClient( 0, sv_privateClients->integer ) : NULL;
	if ( !newcl ) {
		newcl = SV_OldestFreeClient( sv_privateClients->integer, svs.maxclients );
	}

	if ( !newcl ) {
		if ( NET_IsLocalAddress( from ) ) {
			count = 0;
			for ( i = startIndex; i < svs.maxclients ; i++ ) {
				cl = &svs.clients[i];
				if (cl->netchan.remoteAddress.type == NA_BOT) {
					count++;
				}
			}
			// if they're all bots
			if (count >= svs.maxclients - startIndex) {
				SV_DropClient(&svs.clients[svs.maxclients - 1], "only bots on server");
				newcl = &svs.clients[svs.maxclients - 1];
			}
			else {
				Com_Error( ERR_FATAL, "server is full on local connect" );
				return;
			}
		}
		else {
			SV_RefuseConnect( from, "Server is full\n" );
			Com_DPrintf ("Rejected a connection.\n");
			return;
		}
	}

gotnewcl:	
	// build a new connection
	// accept the new client
	// this is the only place a client_t is ever initialized
	*newcl = temp;
	clientNum = newcl - svs.clients;
	ent = SV_GentityNum( clientNum );
	newcl->gentity = ent;

	// save the challenge
	newcl->challenge = challenge;

	// save the address
#ifdef LEGACY_PROTOCOL
	newcl->compat = compat;
	Netchan_Setup(NS_SERVER, &newcl->netchan, from, qport, challenge, compat);
#else
	Netchan_Setup(NS_SERVER, &newcl->netchan, from, qport, challenge, qfalse);
#endif
	newcl->proxied = NET_PacketProxied();
	if ( !newcl->proxied ) {
		NET_ProxyForget( &from );
	}
	if ( fragmentSize ) {
		Netchan_SetFragmentSize( &newcl->netchan, fragmentSize );
		// a web client's, which an admin may want to tell apart
		Com_Printf( "Client %i: fragments of %i bytes\n", clientNum, fragmentSize );
	}
	// init the netchan queue
	newcl->netchan_end_queue = &newcl->netchan_start_queue;

	// save the userinfo
	Q_strncpyz( newcl->userinfo, userinfo, sizeof(newcl->userinfo) );

	// the device it proved, before the game asks its role (trap_ClientRole)
	newcl->identityProven = identityProven;
	if ( identityProven ) {
		char	fingerprint[IDENT_FINGERPRINT_SIZE];

		Com_Memcpy( newcl->identity, identity, sizeof( newcl->identity ) );
		Ident_Fingerprint( identity, fingerprint );
		Com_Printf( "Client %i proved device %s\n", clientNum, fingerprint );
	}

	// get the game a chance to reject this connection or modify the userinfo
	denied = VM_Call( gvm, GAME_CLIENT_CONNECT, clientNum, qtrue, qfalse ); // firstTime = qtrue
	if ( denied ) {
		const char *str = VM_ExplicitArgStr( gvm, denied, "GAME_CLIENT_CONNECT's result" );

		SV_RefuseConnect( from, "%s\n", str );
		Com_DPrintf ("Game rejected a connection: %s.\n", str);
		// the free slot keeps no device for trap_ClientRole to find
		newcl->identityProven = qfalse;
		return;
	}

	SV_UserinfoChanged( newcl );

	// the extensions agreed, confirmed with the connection's content key
	tokens[0] = '\0';
	newcl->ofx = ofx;
	newcl->contentKey = contentKey;
	if ( ofx ) {
		Com_ExtensionKeyString( newcl->contentKey, key );
		Com_AppendExtensionToken( tokens, sizeof( tokens ), "ofx", va( "%x", ofx ) );
		Com_AppendExtensionToken( tokens, sizeof( tokens ), "ofk", key );
		// the content manifest the client checks its paks against
		if ( ( ofx & OFX_CONTENT ) && ( sha256 = SV_ContentManifestHash() ) != NULL ) {
			Com_SHA256Hex( sha256, hex );
			Com_AppendExtensionToken( tokens, sizeof( tokens ), "ofm", hex );
		}
	}
	Com_DPrintf( "Client %i: extensions %x\n", clientNum, ofx );

	// send the connect packet to the client
	NET_OutOfBandPrint(NS_SERVER, from, "connectResponse %d%s", challenge, tokens);

	Com_DPrintf( "Going from CS_FREE to CS_CONNECTED for %s\n", newcl->name );

	newcl->state = CS_CONNECTED;
	newcl->lastSnapshotTime = 0;
	newcl->lastPacketTime = svs.time;
	newcl->lastConnectTime = svs.time;
	
	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	newcl->gamestateMessageNum = -1;
	newcl->lastServerId = -1;

	Hooks_ClientConnected( clientNum );

	// if this was the first client on the server, or the last client
	// the server can hold, send a heartbeat to the master.
	count = 0;
	for (i=0,cl=svs.clients ; i < svs.maxclients ; i++,cl++) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			count++;
		}
	}
	if ( count == 1 || count == svs.maxclients ) {
		SV_Heartbeat_f();
	}
}

/*
=====================
SV_FreeClient

Destructor for data allocated in a client structure
=====================
*/
void SV_FreeClient(client_t *client)
{
#ifdef USE_VOIP
	int index;
	
	for(index = client->queuedVoipIndex; index < client->queuedVoipPackets; index++)
	{
		index %= ARRAY_LEN(client->voipPacket);
		
		Z_Free(client->voipPacket[index]);
	}
	
	client->queuedVoipPackets = 0;
#endif

	SV_Netchan_FreeQueue(client);
	SV_CloseDownload(client);
	SV_ContentFreeClient(client);
}

/*
=====================
SV_ClientAt

Whether a client in the game is at this address. With directOnly, only one
that came directly: a trusted proxy that names it is never believed, as
its route would send that client's game to the proxy
=====================
*/
qboolean SV_ClientAt( const netadr_t *adr, qboolean directOnly ) {
	client_t	*cl;
	int			i;

	if ( !com_sv_running || !com_sv_running->integer || !svs.clients ) {
		return qfalse;
	}
	for ( i = 0, cl = svs.clients; i < svs.maxclients; i++, cl++ ) {
		if ( cl->state >= CS_CONNECTED && !( directOnly && cl->proxied )
			&& NET_CompareAdr( *adr, cl->netchan.remoteAddress ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
With a NULL reason, no one is told
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	int		i;
	const qboolean isBot = drop->netchan.remoteAddress.type == NA_BOT;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	Hooks_ClientDropped( drop - svs.clients, reason );

	// Free all allocated data on the client structure
	SV_FreeClient(drop);

	// Reset the reliable sequence to the currently acknowledged command
	// This prevents SV_AddServerCommand() from making another recursive call to SV_DropClient()
	// if the client lacks sufficient space for another reliable command
	// it also guarantees that the client receives both the print and disconnect commands
	drop->reliableSequence = drop->reliableAcknowledge;
	drop->snapshotAcknowledge = drop->reliableAcknowledge;
	// Setting the gamestate message number to -1 ensures that SV_AddServerCommand()
	// will not call SV_DropClient() again, even though it is unlikely the client
	// will receive many server commands during the drop
	drop->gamestateMessageNum = -1;

	// tell everyone why they got dropped
	if ( reason ) {
		SV_SendServerCommand( NULL, "print \"%s" S_COLOR_WHITE " %s\n\"", drop->name, reason );
	}

	// call the prog function for removing a client
	// this will remove the body, among other things
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, drop - svs.clients );

	// add the disconnect command
	if ( reason ) {
		SV_SendServerCommand( drop, "disconnect \"%s\"", reason);
	}

	drop->lastDisconnectTime = svs.time;

	if ( isBot ) {
		SV_BotFreeClient( drop - svs.clients );

		// bots shouldn't go zombie, as there's no real net connection.
		drop->state = CS_FREE;
	} else {
		Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
		drop->state = CS_ZOMBIE;		// become free in a few seconds
	}

	// nuke user info, and the device it proved
	SV_SetUserinfo( drop - svs.clients, "" );
	drop->identityProven = qfalse;

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it
	for (i=0 ; i < svs.maxclients ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}
	if ( i == svs.maxclients ) {
		SV_Heartbeat_f();
	}
}

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.

It will be resent if the client acknowledges a later message but has
the wrong gamestate. Returns its size in bytes, 0 when it didn't fit.
================
*/
static int SV_SendClientGameState( client_t *client ) {
	int			start;
	entityState_t	*base, nullstate;
	const byte	*manifest;
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

 	Com_DPrintf ("SV_SendClientGameState() for %s\n", client->name);
	Com_DPrintf( "Going from CS_CONNECTED to CS_PRIMED for %s\n", client->name );
	client->state = CS_PRIMED;
	client->contentGamestateDue = qfalse;
	client->pureAuthentic = 0;
	client->gotCP = qfalse;
	// each load says if it skips what's said, or more (SV_AddServerCommand)
	client->loadingSaySkipped = qfalse;
	client->resyncSkipped = qfalse;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit.
	// It goes out after any message still being sent (SV_Netchan_Transmit)
	client->gamestateMessageNum = SV_Netchan_NextSequence( client );
	// one sent after donedl, again, or as a resync, keeps the serverId
	client->keptServerId = client->lastServerId == sv.serverId ? sv.serverId : -1;
	// packets from before it are counted from this one on
	// (SV_ExecuteClientMessage)
	client->gamestateLateTime = 0;

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );
	client->snapshotAcknowledge = client->reliableSequence;

	// send the gamestate
	MSG_WriteByte( &msg, svc_gamestate );
	MSG_WriteLong( &msg, client->reliableSequence );

	// write the configstrings
	for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
		if (sv.configstrings[start][0]) {
			MSG_WriteByte( &msg, svc_configstring );
			MSG_WriteShort( &msg, start );
			MSG_WriteBigString( &msg, sv.configstrings[start] );
		}
	}
	// they're all as they are now, so none needs sending again when the
	// client enters (SV_UpdateConfigstrings)
	Com_Memset( client->csUpdated, 0, sizeof( client->csUpdated ) );

	// write the baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( start = 0 ; start < MAX_GENTITIES; start++ ) {
		base = &sv.svEntities[start].baseline;
		if ( !base->number ) {
			continue;
		}
		MSG_WriteByte( &msg, svc_baseline );
		MSG_WriteDeltaEntity( &msg, &nullstate, base, qtrue );
	}

	// the content manifest the client checks its paks against, before the
	// file system restart this gamestate makes, so a map change needs no
	// request for it (download-protocol.md section 9.1, step 3); a stock
	// client would take it for a bad command byte
	if ( ( client->ofx & OFX_CONTENT ) && ( manifest = SV_ContentManifestHash() ) != NULL ) {
		MSG_WriteByte( &msg, svc_ofManifest );
		MSG_WriteData( &msg, manifest, SHA256_DIGEST_SIZE );
	}

	MSG_WriteByte( &msg, svc_EOF );

	MSG_WriteLong( &msg, client - svs.clients);

	// write the checksum feed
	MSG_WriteLong( &msg, sv.checksumFeed);

	// a gamestate cut short fails on the client with a bad command
	// byte, and leaves the server waiting for it
	if ( msg.overflowed ) {
		int		configstringChars = 0, baselines = 0;

		for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
			configstringChars += strlen( sv.configstrings[start] );
		}
		for ( start = 0 ; start < MAX_GENTITIES ; start++ ) {
			if ( sv.svEntities[start].baseline.number ) {
				baselines++;
			}
		}
		Com_Printf( "WARNING: gamestate for %s is over %i bytes: %i server commands, "
			"%i characters of configstrings, %i baselines\n", client->name, MAX_MSGLEN,
			client->reliableSequence - client->reliableAcknowledge, configstringChars, baselines );

		if ( client->netchan.remoteAddress.type == NA_LOOPBACK ) {
			Com_Error( ERR_DROP, "gamestate overflow" );
		}
		NET_OutOfBandPrint( NS_SERVER, client->netchan.remoteAddress,
			"print\n" S_COLOR_RED "SERVER ERROR: gamestate overflow\n" );
		SV_DropClient( client, "gamestate overflow" );
		return 0;
	}

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
	return msg.cursize;
}

/*
================
SV_ResendGameState

Send a client the gamestate again, when it seems to have missed it.
Not while one is still being sent, and, but for the local client, at
most once a second, so a client can't make the server queue them
================
*/
static void SV_ResendGameState( client_t *client ) {
	if ( client->netchan.unsentFragments || client->netchan_start_queue ) {
		return;
	}
	if ( client->netchan.remoteAddress.type != NA_LOOPBACK &&
		SVC_RateLimit( &client->gamestateRate, 1, 1000 ) ) {
		return;
	}
	SV_SendClientGameState( client );
}

/*
================
SV_ResyncClient

Sends a player that fell behind on server commands (SV_AddServerCommand)
the gamestate again, as a map change does. Its game loads the map again
and the player plays on: the game itself isn't told, and the player
enters the world again without the game's ClientBegin, which would
respawn them. Also sends a gamestate that kept the serverId again when the
client didn't get it (SV_ExecuteClientMessage)
================
*/
void SV_ResyncClient( client_t *client ) {
	int		size;

	client->resyncServerId = sv.serverId;
	size = SV_SendClientGameState( client );
	if ( size ) {
		Com_Printf( "Sent %s the gamestate again, %i bytes\n", client->name, size );
	}
	// so a lost packet doesn't make SV_ResendGameState send it again at once
	SVC_RateLimit( &client->gamestateRate, 1, 1000 );
}

/*
================
SV_GamestateAwaitingAck

Whether a client sent a gamestate that kept the serverId its packets
carry, as one after donedl, a resync's (SV_ResyncClient) or another sent
again does, has yet to acknowledge it. Only its message number tells a
packet from before it, and it's the last message the client is sent until
then (SV_SendClientMessages): a later one would carry the same commands,
and the client would acknowledge them without it
================
*/
qboolean SV_GamestateAwaitingAck( const client_t *client ) {
	return client->state == CS_PRIMED && client->messageAcknowledge < client->gamestateMessageNum &&
		client->keptServerId == sv.serverId;
}


/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {
	int		clientNum;
	sharedEntity_t *ent;
	qboolean	inGame = qfalse;

	// a player resynced after falling behind (SV_ResyncClient) has been in
	// the game all along, unless the map changed or restarted since its
	// gamestate went out. map_restart also enters a player whose gamestate
	// is still to go: the game begins them then, and the resync goes on;
	// and one still loading a gamestate, whose commands may fill its queue,
	// so the restart's would overflow it: it's sent one after the restart
	if ( client->resync && client->state == CS_PRIMED ) {
		if ( client->resyncServerId != sv.serverId &&
			client->messageAcknowledge < client->gamestateMessageNum ) {
			client->resyncServerId = -1;
		} else {
			client->resync = qfalse;
			inGame = ( client->resyncServerId == sv.serverId );
		}
	}

	Com_DPrintf( "Going from CS_PRIMED to CS_ACTIVE for %s\n", client->name );
	client->state = CS_ACTIVE;
	// its ping is checked a few seconds into its time in the game, until
	// it has been: a map change before then starts the wait again, so the
	// frames sent across the load don't count
	if ( !client->pingChecked ) {
		client->pingCheckTime = svs.time + SV_PING_CHECK_TIME;
	}
	SV_ContentEnterWorld( client );

	// resend all configstrings using the cs commands since these are
	// no longer sent when the client is CS_PRIMED
	SV_UpdateConfigstrings( client );

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	ent->s.number = clientNum;
	client->gentity = ent;

	client->deltaMessage = -1;
	client->lastSnapshotTime = 0;	// generate a snapshot immediately

	if(cmd)
		memcpy(&client->lastUsercmd, cmd, sizeof(client->lastUsercmd));
	else
		memset(&client->lastUsercmd, '\0', sizeof(client->lastUsercmd));

	if ( inGame ) {
		return;
	}

	// in the game, its address is a real player's: one step less toxic
	// than its silent connects made it (ENG-069)
	if ( client->netchan.remoteAddress.type != NA_BOT ) {
		SVC_RateRestoreToxicAddress( client->netchan.remoteAddress, 10, 1000 );
	}

	// call the game begin function
	VM_Call( gvm, GAME_CLIENT_BEGIN, client - svs.clients );
	Hooks_ClientBegin( clientNum );
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/

/*
==================
SV_CloseDownload

clear/free any download vars
==================
*/
static void SV_CloseDownload( client_t *cl ) {
	int i;

	// EOF
	if (cl->download) {
		FS_FCloseFile( cl->download );
	}
	cl->download = 0;
	*cl->downloadName = 0;

	// Free the temporary buffer space
	for (i = 0; i < MAX_DOWNLOAD_WINDOW; i++) {
		if (cl->downloadBlocks[i]) {
			Z_Free(cl->downloadBlocks[i]);
			cl->downloadBlocks[i] = NULL;
		}
	}

}

/*
==================
SV_StopDownload_f

Abort a download if in progress
==================
*/
static void SV_StopDownload_f( client_t *cl ) {
	if (*cl->downloadName)
		Com_DPrintf( "clientDownload: %d : file \"%s\" aborted\n", (int) (cl - svs.clients), cl->downloadName );

	SV_CloseDownload( cl );
}

/*
==================
SV_DoneDownload_f

Downloads are finished. The gamestate keeps the serverId the client has,
so it waits for the client to acknowledge it, and goes again if it's lost
(SV_GamestateAwaitingAck)
==================
*/
static void SV_DoneDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;

	Com_DPrintf( "clientDownload: %s Done\n", cl->name);
	// resend the game state to update any clients that entered during the download
	SV_SendClientGameState(cl);

	// so a lost packet doesn't make SV_ResendGameState send it again at once
	SVC_RateLimit( &cl->gamestateRate, 1, 1000 );
}

/*
==================
SV_NextDownload_f

The argument will be the last acknowledged block from the client, it should be
the same as cl->downloadClientBlock
==================
*/
static void SV_NextDownload_f( client_t *cl )
{
	int block = atoi( Cmd_Argv(1) );

	if (block == cl->downloadClientBlock) {
		Com_DPrintf( "clientDownload: %d : client acknowledge of block %d\n", (int) (cl - svs.clients), block );

		// Find out if we are done.  A zero-length block indicates EOF
		if (cl->downloadBlockSize[cl->downloadClientBlock % MAX_DOWNLOAD_WINDOW] == 0) {
			Com_Printf( "clientDownload: %d : file \"%s\" completed\n", (int) (cl - svs.clients), cl->downloadName );
			SV_CloseDownload( cl );
			return;
		}

		cl->downloadSendTime = svs.time;
		cl->downloadClientBlock++;
		return;
	}
	// We aren't getting an acknowledge for the correct block, drop the client
	// FIXME: this is bad... the client will never parse the disconnect message
	//			because the cgame isn't loaded yet
	SV_DropClient( cl, "broken download" );
}

/*
==================
SV_BeginDownload_f
==================
*/
static void SV_BeginDownload_f( client_t *cl ) {
	// a player in the game would stop getting snapshots, and leave
	// the game without the game knowing
	if ( cl->state == CS_ACTIVE ) {
		return;
	}

	// Kill any existing download
	SV_CloseDownload( cl );

	// cl->downloadName is non-zero now, SV_WriteDownloadToClient will see this and open
	// the file itself
	Q_strncpyz( cl->downloadName, Cmd_Argv(1), sizeof(cl->downloadName) );
}

/*
==================
SV_DownloadsFull

Whether sv_maxDownloads clients besides this one are downloading already,
by either path. Each classic download holds one of the file handles
everything else shares.
==================
*/
qboolean SV_DownloadsFull( const client_t *cl ) {
	int		i, count = 0;

	if ( sv_maxDownloads->integer <= 0 ) {
		return qfalse;
	}
	for ( i = 0 ; i < svs.maxclients ; i++ ) {
		if ( &svs.clients[i] != cl && ( svs.clients[i].download || SV_ContentPaks( &svs.clients[i] ) ) ) {
			count++;
		}
	}
	return count >= sv_maxDownloads->integer;
}

/*
==================
SV_WriteDownloadToClient

Check to see if the client wants a file, open it if needed and start pumping the client
Fill up msg with data, return number of download blocks added
==================
*/
int SV_WriteDownloadToClient(client_t *cl, msg_t *msg)
{
	int curindex;
	int unreferenced = 1;
	char errorMessage[1024];
	char pakbuf[MAX_QPATH], *pakptr;
	char retail[MAX_QPATH];
	int numRefPaks;
	qboolean full = qfalse, refused = qfalse;

	if (!*cl->downloadName)
		return 0;	// Nothing being downloaded

	if(!cl->download)
	{
		// a download asked for just before entering the game, as in
		// the same packet as the first usercmd, is ignored too
		if ( cl->state == CS_ACTIVE ) {
			*cl->downloadName = 0;
			return 0;
		}

 		// Chop off filename extension.
		Com_sprintf(pakbuf, sizeof(pakbuf), "%s", cl->downloadName);
		pakptr = strrchr(pakbuf, '.');
		
		if(pakptr)
		{
			*pakptr = '\0';

			// Check for pk3 filename extension
			if(!Q_stricmp(pakptr + 1, "pk3"))
			{
				const char *referencedPaks = FS_ReferencedPakNames();

				// Check whether the file appears in the list of referenced
				// paks to prevent downloading of arbitrary files.
				Cmd_TokenizeStringIgnoreQuotes(referencedPaks);
				numRefPaks = Cmd_Argc();

				for(curindex = 0; curindex < numRefPaks; curindex++)
				{
					if(!FS_FilenameCompare(Cmd_Argv(curindex), pakbuf))
					{
						unreferenced = 0;
						break;
					}
				}
			}
		}

		cl->download = 0;

		// We open the file here
		if ( !(sv_allowDownload->integer & DLF_ENABLE) ||
			(sv_allowDownload->integer & DLF_NO_UDP) ||
			unreferenced ||
			( refused = sv_refuseRetailPaks->integer && FS_RetailPak( pakbuf, retail, sizeof( retail ) ) ) ||
			( full = SV_DownloadsFull( cl ) ) ||
			( cl->downloadSize = FS_BaseDir_FOpenFileRead( cl->downloadName, &cl->download ) ) < 0 ) {
			// cannot auto-download file
			if(unreferenced)
			{
				Com_Printf("clientDownload: %d : \"%s\" is not referenced and cannot be downloaded.\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" is not referenced and cannot be downloaded.", cl->downloadName);
			}
			else if ( !(sv_allowDownload->integer & DLF_ENABLE) ||
				(sv_allowDownload->integer & DLF_NO_UDP) ) {

				Com_Printf("clientDownload: %d : \"%s\" download disabled\n", (int) (cl - svs.clients), cl->downloadName);
				if (SV_PureActive()) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
										"You will need to get this file elsewhere before you "
										"can connect to this pure server.\n", cl->downloadName);
				} else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
                    "The server you are connecting to is not a pure server, "
                    "set autodownload to No in your settings and you might be "
                    "able to join the game anyway.\n", cl->downloadName);
				}
			} else if ( refused ) {
				Com_Printf( "clientDownload: %d : \"%s\" is the game's own %s, which sv_refuseRetailPaks keeps from downloads\n",
					(int) (cl - svs.clients), cl->downloadName, retail );
				if ( !FS_FilenameCompare( cl->downloadName, retail ) ) {
					Com_sprintf( errorMessage, sizeof( errorMessage ), "This server doesn't send the game's own paks. "
						"Add %s from your own copy of the game, then connect again.\n", retail );
				} else {
					Com_sprintf( errorMessage, sizeof( errorMessage ), "This server doesn't send the game's own paks: "
						"\"%s\" is %s. Add %s from your own copy of the game, then connect again.\n",
						cl->downloadName, retail, retail );
				}
			} else if ( full ) {
				Com_Printf("clientDownload: %d : \"%s\" refused, %d downloads already\n", (int) (cl - svs.clients), cl->downloadName, sv_maxDownloads->integer);
				Com_sprintf(errorMessage, sizeof(errorMessage), "The server is sending too many downloads to download \"%s\" now. "
					"Try again in a moment.\n", cl->downloadName);
			} else {
        // NOTE TTimo this is NOT supposed to happen unless bug in our filesystem scheme?
        //   if the pk3 is referenced, it must have been found somewhere in the filesystem
				Com_Printf("clientDownload: %d : \"%s\" file not found on server\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" not found on server for autodownloading.\n", cl->downloadName);
			}
			MSG_WriteByte( msg, svc_download );
			MSG_WriteShort( msg, 0 ); // client is expecting block zero
			MSG_WriteLong( msg, -1 ); // illegal file size
			MSG_WriteString( msg, errorMessage );

			*cl->downloadName = 0;
			
			if(cl->download)
				FS_FCloseFile(cl->download);
			
			return 1;
		}
 
		Com_Printf( "clientDownload: %d : beginning \"%s\"\n", (int) (cl - svs.clients), cl->downloadName );
		
		// server commands wouldn't reach the client until it's done,
		// so don't queue them; donedl sends a new gamestate
		Com_DPrintf( "Going to CS_CONNECTED for %s\n", cl->name );
		cl->state = CS_CONNECTED;

		// Init
		cl->downloadCurrentBlock = cl->downloadClientBlock = cl->downloadXmitBlock = 0;
		cl->downloadCount = 0;
		cl->downloadEOF = qfalse;
	}

	// Perform any reads that we need to
	while (cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW &&
		cl->downloadSize != cl->downloadCount) {

		curindex = (cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW);

		if (!cl->downloadBlocks[curindex])
			cl->downloadBlocks[curindex] = Z_Malloc(MAX_DOWNLOAD_BLKSIZE);

		cl->downloadBlockSize[curindex] = FS_Read( cl->downloadBlocks[curindex], MAX_DOWNLOAD_BLKSIZE, cl->download );

		if (cl->downloadBlockSize[curindex] < 0) {
			// EOF right now
			cl->downloadCount = cl->downloadSize;
			break;
		}

		cl->downloadCount += cl->downloadBlockSize[curindex];

		// Load in next block
		cl->downloadCurrentBlock++;
	}

	// Check to see if we have eof condition and add the EOF block
	if (cl->downloadCount == cl->downloadSize &&
		!cl->downloadEOF &&
		cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW) {

		cl->downloadBlockSize[cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW] = 0;
		cl->downloadCurrentBlock++;

		cl->downloadEOF = qtrue;  // We have added the EOF block
	}

	if (cl->downloadClientBlock == cl->downloadCurrentBlock)
		return 0; // Nothing to transmit

	// Write out the next section of the file, if we have already reached our window,
	// automatically start retransmitting
	if (cl->downloadXmitBlock == cl->downloadCurrentBlock)
	{
		// We have transmitted the complete window, should we start resending?
		if (svs.time - cl->downloadSendTime > 1000)
			cl->downloadXmitBlock = cl->downloadClientBlock;
		else
			return 0;
	}

	// Send current block
	curindex = (cl->downloadXmitBlock % MAX_DOWNLOAD_WINDOW);

	MSG_WriteByte( msg, svc_download );
	MSG_WriteShort( msg, cl->downloadXmitBlock );

	// block zero is special, contains file size
	if ( cl->downloadXmitBlock == 0 )
		MSG_WriteLong( msg, cl->downloadSize );

	MSG_WriteShort( msg, cl->downloadBlockSize[curindex] );

	// Write the block
	if(cl->downloadBlockSize[curindex])
		MSG_WriteData(msg, cl->downloadBlocks[curindex], cl->downloadBlockSize[curindex]);

	Com_DPrintf( "clientDownload: %d : writing block %d\n", (int) (cl - svs.clients), cl->downloadXmitBlock );

	// Move on to the next block
	// It will get sent with next snap shot.  The rate will keep us in line.
	cl->downloadXmitBlock++;
	cl->downloadSendTime = svs.time;

	return 1;
}

/*
==================
SV_SendQueuedMessages

Send one round of fragments, or queued messages to all clients that have data pending.
Return the shortest time interval for sending next packet to client
==================
*/

int SV_SendQueuedMessages(void)
{
	int i, retval = -1, nextFragT;
	client_t *cl;
	
	for(i=0; i < svs.maxclients; i++)
	{
		cl = &svs.clients[i];
		
		if(cl->state)
		{
			nextFragT = SV_RateMsec(cl);

			if(!nextFragT)
				nextFragT = SV_Netchan_TransmitNextFragment(cl);

			if(nextFragT >= 0 && (retval == -1 || retval > nextFragT))
				retval = nextFragT;
		}
	}

	return retval;
}


/*
==================
SV_SendDownloadMessages

Send one round of download messages to all clients
==================
*/

int SV_SendDownloadMessages(void)
{
	int i, numDLs = 0, retval;
	client_t *cl;
	msg_t msg;
	byte msgBuffer[MAX_MSGLEN];
	
	for(i=0; i < svs.maxclients; i++)
	{
		cl = &svs.clients[i];
		
		if(cl->state && *cl->downloadName)
		{
			MSG_Init(&msg, msgBuffer, sizeof(msgBuffer));
			MSG_WriteLong(&msg, cl->lastClientCommand);
			
			retval = SV_WriteDownloadToClient(cl, &msg);
				
			if(retval)
			{
				MSG_WriteByte(&msg, svc_EOF);
				SV_Netchan_Transmit(cl, &msg);
				numDLs += retval;
			}
		}
	}

	return numDLs;
}

/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
static void SV_Disconnect_f( client_t *cl ) {
	SV_DropClient( cl, "disconnected" );
}

/*
=================
SV_VerifyPaks_f

If we are pure, disconnect the client if they do no meet the following conditions:

1. the first two checksums match our view of cgame and ui
2. there are no any additional checksums that we do not have

This routine would be a bit simpler with a goto but i abstained

=================
*/
static void SV_VerifyPaks_f( client_t *cl ) {
	int nChkSum1, nChkSum2, nClientPaks, nServerPaks, i, j, nCurArg;
	int nClientChkSum[1024];
	int nServerChkSum[1024];
	const char *pPaks, *pArg;
	qboolean bGood = qtrue;

	// if we are pure, we "expect" the client to load certain things from 
	// certain pk3 files, namely we want the client to have loaded the
	// ui and cgame that we think should be loaded based on the pure setting
	//
	if ( SV_PureActive() ) {

		nChkSum1 = nChkSum2 = 0;
		// we run the game, so determine which cgame and ui the client "should" be running
		bGood = (FS_FileIsInPAK("vm/cgame.qvm", &nChkSum1) == 1);
		if (bGood)
			bGood = (FS_FileIsInPAK("vm/ui.qvm", &nChkSum2) == 1);

		nClientPaks = Cmd_Argc();

		// start at arg 2 ( skip serverId cl_paks )
		nCurArg = 1;

		pArg = Cmd_Argv(nCurArg++);
		if(!pArg) {
			bGood = qfalse;
		}
		else
		{
			// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=475
			// we may get incoming cp sequences from a previous checksumFeed, which we need to ignore
			// since serverId is a frame count, it always goes up
			if (atoi(pArg) < sv.checksumFeedServerId)
			{
				Com_DPrintf("ignoring outdated cp command from client %s\n", cl->name);
				return;
			}
		}
	
		// we basically use this while loop to avoid using 'goto' :)
		while (bGood) {

			// must be at least 6: "cl_paks cgame ui @ firstref ... numChecksums"
			// numChecksums is encoded
			if (nClientPaks < 6) {
				bGood = qfalse;
				break;
			}
			// verify first to be the cgame checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum1 ) {
				bGood = qfalse;
				break;
			}
			// verify the second to be the ui checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum2 ) {
				bGood = qfalse;
				break;
			}
			// should be sitting at the delimeter now
			pArg = Cmd_Argv(nCurArg++);
			if (*pArg != '@') {
				bGood = qfalse;
				break;
			}
			// store checksums since tokenization is not re-entrant
			for (i = 0; nCurArg < nClientPaks; i++) {
				nClientChkSum[i] = atoi(Cmd_Argv(nCurArg++));
			}

			// store number to compare against (minus one cause the last is the number of checksums)
			nClientPaks = i - 1;

			// make sure none of the client check sums are the same
			// so the client can't send 5 the same checksums
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nClientPaks; j++) {
					if (i == j)
						continue;
					if (nClientChkSum[i] == nClientChkSum[j]) {
						bGood = qfalse;
						break;
					}
				}
				if (bGood == qfalse)
					break;
			}
			if (bGood == qfalse)
				break;

			// get the pure checksums of the pk3 files loaded by the server
			pPaks = FS_LoadedPakPureChecksums();
			Cmd_TokenizeString( pPaks );
			nServerPaks = Cmd_Argc();
			if (nServerPaks > 1024)
				nServerPaks = 1024;

			for (i = 0; i < nServerPaks; i++) {
				nServerChkSum[i] = atoi(Cmd_Argv(i));
			}

			// check if the client has provided any pure checksums of pk3 files not loaded by the server
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nServerPaks; j++) {
					if (nClientChkSum[i] == nServerChkSum[j]) {
						break;
					}
				}
				if (j >= nServerPaks) {
					bGood = qfalse;
					break;
				}
			}
			if ( bGood == qfalse ) {
				break;
			}

			// check if the number of checksums was correct
			nChkSum1 = sv.checksumFeed;
			for (i = 0; i < nClientPaks; i++) {
				nChkSum1 ^= nClientChkSum[i];
			}
			nChkSum1 ^= nClientPaks;
			if (nChkSum1 != nClientChkSum[nClientPaks]) {
				bGood = qfalse;
				break;
			}

			// break out
			break;
		}

		cl->gotCP = qtrue;

		if (bGood) {
			cl->pureAuthentic = 1;
		} 
		else {
			cl->pureAuthentic = 0;
			cl->lastSnapshotTime = 0;
			cl->state = CS_ACTIVE;
			SV_SendClientSnapshot( cl );
			SV_DropClient( cl, "Unpure Client. "
				"You may need to enable in-game downloads "
				"to connect to this server (set "
				"cl_allowDownload 1)" );
		}
	}
}

/*
=================
SV_ResetPureClient_f
=================
*/
static void SV_ResetPureClient_f( client_t *cl ) {
	cl->pureAuthentic = 0;
	cl->gotCP = qfalse;
}

/*
=================
SV_IsUnlimitedRate

The local client, and a client on the same subnet as the server unless
it's an internet public server (dedicated 2), don't need a rate choke
=================
*/
qboolean SV_IsUnlimitedRate( client_t *cl ) {
	return cl->netchan.remoteAddress.type == NA_LOOPBACK ||
		( sv_lanForceRate->integer && com_dedicated->integer != 2 &&
		Sys_IsLANAddress( cl->netchan.remoteAddress ) );
}

/*
=================
SV_UpdateSnapshotRate

The client's snapshots a second, from its snaps userinfo, capped by
sv_fps: on a userinfo change, and when sv_fps changes
=================
*/
void SV_UpdateSnapshotRate( client_t *cl ) {
	char	*val;
	int		i;

	// none given: every frame
	val = Info_ValueForKey (cl->userinfo, "snaps");
	i = *val ? atoi(val) : sv_fps->integer;

	if(i < 1)
		i = 1;
	else if(i > sv_fps->integer)
		i = sv_fps->integer;

	i = 1000 / i;

	if(i != cl->snapshotMsec)
	{
		// Reset last sent snapshot so we avoid desync between server frame time and snapshot send time
		cl->lastSnapshotTime = 0;
		cl->snapshotMsec = i;
	}
}

/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	char	*val;
	char	*ip;
	int		i;
	int	len;

	// name for C code
	Q_strncpyz( cl->name, Info_ValueForKey (cl->userinfo, "name"), sizeof(cl->name) );

	// rate command

	// if the client is on the same subnet as the server and we aren't running an
	// internet public server, assume they don't need a rate choke
	if ( SV_IsUnlimitedRate( cl ) ) {
		cl->rate = 99999;	// lans should not rate limit
	} else {
		val = Info_ValueForKey (cl->userinfo, "rate");
		if (strlen(val)) {
			i = atoi(val);
			cl->rate = i;
			if (cl->rate < 1000) {
				cl->rate = 1000;
			} else if (cl->rate > 90000) {
				cl->rate = 90000;
			}
		} else {
			cl->rate = 3000;
		}
	}
	val = Info_ValueForKey (cl->userinfo, "handicap");
	if (strlen(val)) {
		i = atoi(val);
		if (i<=0 || i>100 || strlen(val) > 4) {
			Info_SetValueForKey( cl->userinfo, "handicap", "100" );
		}
	}

	SV_UpdateSnapshotRate( cl );

#ifdef USE_VOIP
#ifdef LEGACY_PROTOCOL
	if(cl->compat)
		cl->hasVoip = qfalse;
	else
#endif
	{
		val = Info_ValueForKey(cl->userinfo, "cl_voipProtocol");
		cl->hasVoip = !Q_stricmp( val, "opus" );
	}
#endif

	// TTimo
	// maintain the IP information
	// the banning code relies on this being consistently present
	if( NET_IsLocalAddress(cl->netchan.remoteAddress) )
		ip = "localhost";
	else
		ip = (char*)NET_AdrToString( cl->netchan.remoteAddress );

	val = Info_ValueForKey( cl->userinfo, "ip" );
	if( val[0] )
		len = strlen( ip ) - strlen( val ) + strlen( cl->userinfo );
	else
		len = strlen( ip ) + 4 + strlen( cl->userinfo );

	if( len >= MAX_INFO_STRING )
		SV_DropClient( cl, "userinfo string length exceeded" );
	else
		Info_SetValueForKey( cl->userinfo, "ip", ip );

}


/*
==================
SV_UpdateUserinfo_f
==================
*/
static void SV_UpdateUserinfo_f( client_t *cl ) {
	// an empty or missing userinfo would wipe the player's
	if ( Cmd_Argc() != 2 || !*Cmd_Argv(1) ) {
		return;
	}

	Q_strncpyz( cl->userinfo, Cmd_Argv(1), sizeof(cl->userinfo) );

	SV_UserinfoChanged( cl );
	// call prog code to allow overrides
	VM_Call( gvm, GAME_CLIENT_USERINFO_CHANGED, cl - svs.clients );
	// as the game has it now
	Hooks_ClientUserinfoChanged( cl - svs.clients );
}


#ifdef USE_VOIP
static
void SV_UpdateVoipIgnore(client_t *cl, const char *idstr, qboolean ignore)
{
	if ((*idstr >= '0') && (*idstr <= '9')) {
		const int id = atoi(idstr);
		if ((id >= 0) && (id < MAX_CLIENTS)) {
			cl->ignoreVoipFromClient[id] = ignore;
		}
	}
}

/*
==================
SV_Voip_f
==================
*/
static void SV_Voip_f( client_t *cl ) {
	const char *cmd = Cmd_Argv(1);
	if (strcmp(cmd, "ignore") == 0) {
		SV_UpdateVoipIgnore(cl, Cmd_Argv(2), qtrue);
	} else if (strcmp(cmd, "unignore") == 0) {
		SV_UpdateVoipIgnore(cl, Cmd_Argv(2), qfalse);
	} else if (strcmp(cmd, "muteall") == 0) {
		cl->muteAllVoip = qtrue;
	} else if (strcmp(cmd, "unmuteall") == 0) {
		cl->muteAllVoip = qfalse;
	}
}
#endif


typedef struct {
	char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},
	{"cp", SV_VerifyPaks_f},
	{"vdr", SV_ResetPureClient_f},
	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	{"stopdl", SV_StopDownload_f},
	{"donedl", SV_DoneDownload_f},

#ifdef USE_VOIP
	{"voip", SV_Voip_f},
#endif

	{NULL, NULL}
};

/*
==================
SV_FindUcmd

The server's own client command with this name, or NULL
==================
*/
static ucmd_t *SV_FindUcmd( const char *name ) {
	ucmd_t	*u;

	for ( u = ucmds ; u->name ; u++ ) {
		if ( !strcmp( name, u->name ) ) {
			return u;
		}
	}
	return NULL;
}

/*
==================
SV_MuteClient

Mutes a client's chat and voice for seconds, or with 0 until it leaves, or
lifts the mute. A mute longer than EXT_MUTE_MAX_SECONDS lasts until the
client leaves, which every mute does at the latest. The client is told
when the mute begins and ends; everyone else is told nothing. Returns
whether it began or ended
==================
*/
qboolean SV_MuteClient( client_t *cl, qboolean mute, int seconds ) {
	if ( mute ) {
		cl->muteEndTime = 0;
		if ( seconds > 0 && seconds <= EXT_MUTE_MAX_SECONDS ) {
			// as SV_ClientMuted counts, past the clock's wrap; 0 is for
			// the connection
			cl->muteEndTime = (int)( (unsigned)Sys_Milliseconds() + seconds * 1000u );
			if ( !cl->muteEndTime ) {
				cl->muteEndTime = 1;
			}
		}
	}
	if ( mute == cl->muted ) {
		return qfalse;
	}
	cl->muted = mute;
	Com_Printf( "%s" S_COLOR_WHITE " %s\n", cl->name, mute ? "is muted" : "is no longer muted" );
	SV_SendServerCommand( cl, "print \"%s\n\"", mute ?
		"You have been muted: other players won't see your chat or hear your voice." : "You are no longer muted." );
	return qtrue;
}

/*
==================
SV_ClientMuted

Whether a client is muted: one whose mute's time has run out isn't, though
SV_CheckTimeouts tells it so only in the next frame
==================
*/
qboolean SV_ClientMuted( const client_t *cl ) {
	return cl->muted && ( !cl->muteEndTime || (int)( (unsigned)Sys_Milliseconds() - (unsigned)cl->muteEndTime ) < 0 );
}

/*
==================
SV_IsChatCommand

Whether a client command is one of the game's chat commands, which a mute
holds back: what baseq3 and Team Arena call them, as their mods do
==================
*/
static qboolean SV_IsChatCommand( const char *cmd ) {
	static const char *const chat[] = {
		"say", "say_team", "tell",
		// voice chat, and the orders a player gives another (a tell)
		"vsay", "vsay_team", "vtell", "vosay", "vosay_team", "votell", "vtaunt", "gc"
	};
	int		i;

	for ( i = 0; i < ARRAY_LEN( chat ); i++ ) {
		if ( !Q_stricmp( cmd, chat[i] ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

// whether a chat line starts with a command whose words are secrets
// (Com_IsSecretWord), typed into the chat by mistake:
// "/login alice hunter2", "claim KQ7M-3XRP"; a tell's, or an order's
// (gc), after the player it goes to
static qboolean SV_ChatHoldsSecret( const char *chat, const char *text ) {
	char	word[32];
	int		n;

	text += strspn( text, " \"" );
	if ( Q_stristr( chat, "tell" ) || !Q_stricmp( chat, "gc" ) ) {
		text += strcspn( text, " " );
		text += strspn( text, " \"" );
	}
	if ( *text == '/' || *text == '\\' ) {
		text++;
	}
	for ( n = 0; text[n] && text[n] != ' ' && n < (int)sizeof( word ) - 1; n++ ) {
		word[n] = text[n];
	}
	word[n] = '\0';
	return Com_IsSecretWord( word );
}

/*
==================
SV_ExtensionCommand

A client's command that an extension runs (hooks.h's clientCommands): a
limited one at 5 a minute for each client and 30 for every client, as
rcon is, which puts a claim code's or a password's guessing out of reach
==================
*/
static rateLimit_t	sv_extensionCommandRate;

static void SV_ExtensionCommand( client_t *cl, int extension, int flags ) {
	if ( ( flags & EXT_COMMAND_LIMITED ) && cl->netchan.remoteAddress.type != NA_LOOPBACK &&
		// a bucket gives one back each period: 5 a minute, and 30
		( SVC_RateLimit( &cl->roleRate, 5, 60000 / 5 ) || SVC_RateLimit( &sv_extensionCommandRate, 30, 60000 / 30 ) ) ) {
		SV_SendServerCommand( cl, "print \"Too many tries; wait a minute.\n\"" );
		return;
	}
	Hooks_RunClientCommand( extension, cl - svs.clients );
}

/*
==================
SV_ExecuteClientCommand

Also called by bot code
==================
*/
void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK ) {
	ucmd_t	*u;
	qboolean bProcessed = qfalse;
	int		extension, flags;
	
	Cmd_TokenizeString( s );

	// an extension's command, which the game never sees (a bot's goes on)
	if ( clientOK && cl->netchan.remoteAddress.type != NA_BOT &&
		( extension = Hooks_FindClientCommand( Cmd_Argv( 0 ), &flags ) ) >= 0 ) {
		SV_ExtensionCommand( cl, extension, flags );
		return;
	}

	// see if it is a server level command
	u = SV_FindUcmd( Cmd_Argv(0) );
	if ( u ) {
		u->func( cl );
		bProcessed = qtrue;
	}

	if (clientOK) {
		// pass unknown strings to the game
		if (!u && sv.state == SS_GAME && (cl->state == CS_ACTIVE || cl->state == CS_PRIMED)) {
			int clientNum = cl - svs.clients;

			// a muted client's chat reaches neither the game nor anyone
			if ( SV_ClientMuted( cl ) && SV_IsChatCommand( Cmd_Argv(0) ) ) {
				SV_SendServerCommand( cl, "print \"You are muted.\n\"" );
				return;
			}
			// a password or a claim code typed into the chat stays with its
			// player
			if ( SV_IsChatCommand( Cmd_Argv( 0 ) ) && SV_ChatHoldsSecret( Cmd_Argv( 0 ), Cmd_ArgsFrom( 1 ) ) ) {
				SV_SendServerCommand( cl, "print \"That goes in the console, not the chat: nothing was sent.\n\"" );
				return;
			}

			Cmd_Args_Sanitize();
			Hooks_ClientCommand( clientNum, s );
			VM_Call( gvm, GAME_CLIENT_COMMAND, clientNum );
			Hooks_ClientCommandDone( clientNum );
		}
	}
	else if (!bProcessed)
		Com_DPrintf( "client text ignored for %s: %s\n", cl->name, Cmd_Argv(0) );
}

/*
===============
SV_ClientCommand
===============
*/
static qboolean SV_ClientCommand( client_t *cl, msg_t *msg ) {
	int		seq;
	const char	*s;
	qboolean clientOk = qtrue;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed it
	if ( cl->lastClientCommand >= seq ) {
		return qtrue;
	}

	// a secret command's words aren't printed, even for developers, nor
	// one's whose words run one: "admin login alice hunter2"
	Cmd_TokenizeString( s );
	if ( !com_developer->integer ) {
		// nothing's printed
	} else if ( Com_IsSecretWord( Cmd_Argv( 0 ) ) || Com_IsSecretCommand( Cmd_ArgsFrom( 1 ), NULL, 0 ) ) {
		Com_DPrintf( "clientCommand: %s : %i : %s (hidden)\n", cl->name, seq, Cmd_Argv( 0 ) );
	} else if ( !Q_stricmp( Cmd_Argv( 0 ), "userinfo" ) ) {
		// and a userinfo's password (ENG-098)
		char	shown[MAX_INFO_STRING];

		Info_HideSecrets( Cmd_Argv( 1 ), shown, sizeof( shown ) );
		Com_DPrintf( "clientCommand: %s : %i : userinfo \"%s\"\n", cl->name, seq, shown );
	} else {
		Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, seq, s );
	}

	// drop the connection if we have somehow lost commands
	if ( seq > cl->lastClientCommand + 1 ) {
		Com_Printf( "Client %s lost %i clientCommands\n", cl->name, 
			seq - cl->lastClientCommand + 1 );
		SV_DropClient( cl, "Lost reliable commands" );
		return qfalse;
	}

	// tokenized above, for the log

	// each userinfo change makes the game send commands to every
	// other client, so a client changing it often could overflow
	// their command queues. Stall the rest of this client's packet
	// instead; it will send the command again
	if ( cl->netchan.remoteAddress.type != NA_LOOPBACK &&
		!strcmp( Cmd_Argv(0), "userinfo" ) &&
		SVC_RateLimit( &cl->userinfoRate, 5, 1000 ) ) {
		Com_DPrintf( "%s: userinfo flood, delaying it\n", cl->name );
		return qfalse;
	}

	// malicious users may try using too many string commands
	// to lag other players. Commands for the game come from a leaky
	// bucket; one over it is ignored but acknowledged. The server's own
	// commands don't count, and bots and the local client aren't limited
	if ( sv_floodProtect->integer &&
		cl->state >= CS_PRIMED &&
		cl->netchan.remoteAddress.type != NA_LOOPBACK &&
		!SV_FindUcmd( Cmd_Argv(0) ) &&
		SVC_RateLimit( &cl->commandRate, 8, 500 ) ) {
		// ignore any other text messages from this client but let them keep playing
		// TTimo - moved the ignored verbose to the actual processing in SV_ExecuteClientCommand, only printing if the core doesn't intercept
		clientOk = qfalse;

		// tell the player, now and then, so that a button that sends
		// a command doesn't just seem broken
		if ( !SVC_RateLimit( &cl->floodNoticeRate, 1, 2000 ) ) {
			SV_SendServerCommand( cl, "print \"Too many commands; some were ignored.\n\"" );
		}
	}

	SV_ExecuteClientCommand( cl, s, clientOk );

	cl->lastClientCommand = seq;
	Com_sprintf(cl->lastClientCommandString, sizeof(cl->lastClientCommandString), "%s", s);

	return qtrue;		// continue procesing
}


//==================================================================================


/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}

	VM_Call( gvm, GAME_CLIENT_THINK, cl - svs.clients );
}

/*
==================
SV_UserMove

The message usually contains all the movement commands 
that were in the last three packets, so that the information
in dropped packets can be recovered.

On very fast clients, there may be multiple usercmd packed into
each of the backup packets.
==================
*/
static void SV_UserMove( client_t *cl, msg_t *msg, qboolean delta ) {
	int			i, key;
	int			cmdCount;
	usercmd_t	nullcmd;
	usercmd_t	cmds[MAX_PACKET_USERCMDS];
	usercmd_t	*cmd, *oldcmd;

	if ( delta ) {
		cl->deltaMessage = cl->messageAcknowledge;
	} else {
		cl->deltaMessage = -1;
	}

	cmdCount = MSG_ReadByte( msg );

	if ( cmdCount < 1 ) {
		Com_Printf( "cmdCount < 1\n" );
		return;
	}

	if ( cmdCount > MAX_PACKET_USERCMDS ) {
		Com_Printf( "cmdCount > MAX_PACKET_USERCMDS\n" );
		return;
	}

	// use the checksum feed in the key
	key = sv.checksumFeed;
	// also use the message acknowledge
	key ^= cl->messageAcknowledge;
	// also use the last acknowledged server command in the key
	key ^= MSG_HashKey(cl->reliableCommands[ cl->reliableAcknowledge & (MAX_RELIABLE_COMMANDS-1) ], 32);

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;
	for ( i = 0 ; i < cmdCount ; i++ ) {
		cmd = &cmds[i];
		MSG_ReadDeltaUsercmdKey( msg, key, oldcmd, cmd );
		oldcmd = cmd;
	}

	// save time for ping calculation
	cl->frames[ cl->messageAcknowledge & PACKET_MASK ].messageAcked = svs.time;

	// the newest message the client has, if it had the snapshot, had every
	// command up to the newest it has, which its game will run; and one sent
	// after an overflow began ends it
	if ( cl->netchan.outgoingSequence - cl->messageAcknowledge > 0
		&& cl->netchan.outgoingSequence - cl->messageAcknowledge <= PACKET_BACKUP
		&& !cl->frames[ cl->messageAcknowledge & PACKET_MASK ].commandsOnly ) {
		cl->snapshotAcknowledge = cl->reliableAcknowledge;
		if ( cl->overflowSequence && cl->messageAcknowledge - cl->overflowSequence > 0 ) {
			cl->overflowSequence = 0;
		}
	}

	// TTimo
	// catch the no-cp-yet situation before SV_ClientEnterWorld
	// if CS_ACTIVE, then it's time to trigger a new gamestate emission
	// if not, then we are getting remaining parasite usermove commands, which we should ignore
	if (SV_PureActive() && cl->pureAuthentic == 0 && !cl->gotCP) {
		if (cl->state == CS_ACTIVE)
		{
			// we didn't get a cp yet, don't assume anything and just send the gamestate all over again
			Com_DPrintf( "%s: didn't get cp command, resending gamestate\n", cl->name);
			SV_ResendGameState( cl );
		}
		return;
	}			
	
	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( cl->state == CS_PRIMED ) {
		SV_ClientEnterWorld( cl, &cmds[0] );
		// the moves can be processed normaly
	}
	
	// a bad cp command was sent, drop the client
	if (SV_PureActive() && cl->pureAuthentic == 0) {
		SV_DropClient( cl, "Cannot validate pure client!");
		return;
	}

	if ( cl->state != CS_ACTIVE ) {
		cl->deltaMessage = -1;
		return;
	}

	// usually, the first couple commands will be duplicates
	// of ones we have previously received, but the servertimes
	// in the commands will cause them to be immediately discarded
	for ( i =  0 ; i < cmdCount ; i++ ) {
		// if this is a cmd from before a map_restart ignore it
		if ( cmds[i].serverTime > cmds[cmdCount-1].serverTime ) {
			continue;
		}
		// extremely lagged or cmd from before a map_restart
		//if ( cmds[i].serverTime > svs.time + 3000 ) {
		//	continue;
		//}
		// don't execute if this is an old cmd which is already executed
		// these old cmds are included when cl_packetdup > 0
		if ( cmds[i].serverTime <= cl->lastUsercmd.serverTime ) {
			continue;
		}
		SV_ClientThink (cl, &cmds[ i ]);
	}
}


#ifdef USE_VOIP
/*
==================
SV_ShouldIgnoreVoipSender

Blocking of voip packets based on source client
==================
*/

static qboolean SV_ShouldIgnoreVoipSender(const client_t *cl)
{
	if (!sv_voip->integer)
		return qtrue;  // VoIP disabled on this server.
	else if (!cl->hasVoip)  // client doesn't have VoIP support?!
		return qtrue;
	else if (SV_ClientMuted(cl))  // an admin muted it
		return qtrue;

	return qfalse;  // don't ignore.
}

static
void SV_UserVoip(client_t *cl, msg_t *msg, qboolean ignoreData)
{
	int sender, generation, sequence, frames, packetsize;
	uint8_t recips[(MAX_CLIENTS + 7) / 8];
	int flags;
	byte encoded[sizeof(cl->voipPacket[0]->data)];
	client_t *client = NULL;
	voipServerPacket_t *packet = NULL;
	int i;

	sender = cl - svs.clients;
	generation = MSG_ReadByte(msg);
	sequence = MSG_ReadLong(msg);
	frames = MSG_ReadByte(msg);
	MSG_ReadData(msg, recips, sizeof(recips));
	flags = MSG_ReadByte(msg);
	packetsize = MSG_ReadShort(msg);

	if (msg->readcount > msg->cursize)
		return;   // short/invalid packet, bail.
	else if (packetsize < 0)
		return;   // invalid packet, bail.

	if (packetsize > sizeof (encoded)) {  // overlarge packet?
		int bytesleft = packetsize;
		while (bytesleft) {
			int br = bytesleft;
			if (br > sizeof (encoded))
				br = sizeof (encoded);
			MSG_ReadData(msg, encoded, br);
			bytesleft -= br;
		}
		return;   // overlarge packet, bail.
	}

	MSG_ReadData(msg, encoded, packetsize);

	if (ignoreData || SV_ShouldIgnoreVoipSender(cl))
		return;   // Blacklisted, disabled, etc.

	// !!! FIXME: see if we read past end of msg...

	// !!! FIXME: reject if not opus data.
	// !!! FIXME: decide if this is bogus data?

	// decide who needs this VoIP packet sent to them...
	for (i = 0, client = svs.clients; i < svs.maxclients ; i++, client++) {
		if (client->state != CS_ACTIVE)
			continue;  // not in the game yet, don't send to this guy.
		else if (i == sender)
			continue;  // don't send voice packet back to original author.
		else if (!client->hasVoip)
			continue;  // no VoIP support, or unsupported protocol
		else if (client->muteAllVoip)
			continue;  // client is ignoring everyone.
		else if (client->ignoreVoipFromClient[sender])
			continue;  // client is ignoring this talker.
		else if (SV_ClientDownloading(client))   // !!! FIXME: possible to DoS?
			continue;  // no VoIP allowed if downloading, to save bandwidth.

		if(Com_IsVoipTarget(recips, sizeof(recips), i))
			flags |= VOIP_DIRECT;
		else
			flags &= ~VOIP_DIRECT;

		if (!(flags & (VOIP_SPATIAL | VOIP_DIRECT)))
			continue;  // not addressed to this player.

		// Transmit this packet to the client.
		if (client->queuedVoipPackets >= ARRAY_LEN(client->voipPacket)) {
			Com_Printf("Too many VoIP packets queued for client #%d\n", i);
			continue;  // no room for another packet right now.
		}

		packet = Z_Malloc(sizeof(*packet));
		packet->sender = sender;
		packet->frames = frames;
		packet->len = packetsize;
		packet->generation = generation;
		packet->sequence = sequence;
		packet->flags = flags;
		memcpy(packet->data, encoded, packetsize);

		client->voipPacket[(client->queuedVoipIndex + client->queuedVoipPackets) % ARRAY_LEN(client->voipPacket)] = packet;
		client->queuedVoipPackets++;
	}
}
#endif



/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
===================
SV_ExecuteClientMessage

Parse a client packet
===================
*/
void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {
	int			c;
	int			serverId;
	int			reliableAcknowledge;

	MSG_Bitstream(msg);

	serverId = MSG_ReadLong( msg );
	cl->lastServerId = serverId;
	cl->messageAcknowledge = MSG_ReadLong( msg );

	if (cl->messageAcknowledge < 0) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
#ifndef NDEBUG
		SV_DropClient( cl, "DEBUG: illegible client message" );
#endif
		return;
	}

	reliableAcknowledge = MSG_ReadLong( msg );

	// a packet from before a resync's gamestate, still to go
	// (SV_SendClientMessages), or from before a gamestate that kept the
	// serverId, a resync's or donedl's, not yet acknowledged
	// (SV_GamestateAwaitingAck), is ignored, as an old map's are, so the
	// client doesn't enter the world
	// before it has loaded, nor acknowledge commands. Once such packets have
	// kept coming for SV_RESYNC_LOST after the gamestate had all gone, the
	// client didn't get it, and it's sent again. (Not SV_ResendGameState's
	// rate, which counts from when it began going: a slow one would go again
	// before a client could acknowledge it, and with nothing sent after it,
	// no later message's acknowledgment shows it lost)
	if ( cl->resync && cl->state == CS_ACTIVE ) {
		return;
	}
	if ( SV_GamestateAwaitingAck( cl ) ) {
		if ( cl->netchan.unsentFragments || cl->netchan_start_queue ) {
			cl->gamestateLateTime = 0;
		} else if ( !cl->gamestateLateTime ) {
			cl->gamestateLateTime = svs.time;
		} else if ( svs.time - cl->gamestateLateTime > SV_RESYNC_LOST ) {
			Com_Printf( "%s didn't get the gamestate\n", cl->name );
			SV_ResyncClient( cl );
		}
		return;
	}
	// a client exactly MAX_RELIABLE_COMMANDS behind is still in step:
	// SV_AddServerCommand keeps that many waiting and overflows at one more.
	// But the slot of the command it last acknowledged now holds the newest,
	// and that command is part of the key for this packet's usercmds (and
	// for all of it from a protocol 68 client), so they can't be read.
	// Ignore the packet and keep the commands: taking the client to have
	// them all, as below, would lose them
	if ( reliableAcknowledge == cl->reliableSequence - MAX_RELIABLE_COMMANDS ) {
		return;
	}
	cl->reliableAcknowledge = reliableAcknowledge;

	// NOTE: when the client message is fux0red the acknowledgement numbers
	// can be out of range, this could cause the server to send thousands of server
	// commands which the server thinks are not yet acknowledged in SV_UpdateServerCommandsToClient
	// (compared without subtracting the client's number, which can overflow)
	if (cl->reliableAcknowledge <= cl->reliableSequence - MAX_RELIABLE_COMMANDS || cl->reliableAcknowledge > cl->reliableSequence) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
#ifndef NDEBUG
		SV_DropClient( cl, "DEBUG: illegible client message" );
#endif
		cl->reliableAcknowledge = cl->reliableSequence;
		return;
	}
	// if this is a usercmd from a previous gamestate,
	// ignore it or retransmit the current gamestate
	// 
	// if the client was downloading, let it stay at whatever serverId and
	// gamestate it was at.  This allows it to keep downloading even when
	// the gamestate changes.  After the download is finished, we'll
	// notice and send it a new game state
	//
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=536
	// don't drop as long as previous command was a nextdl, after a dl is done, downloadName is set back to ""
	// but we still need to read the next message to move to next download or send gamestate
	// I don't like this hack though, it must have been working fine at some point, suspecting the fix is somewhere else
	if ( serverId != sv.serverId && !*cl->downloadName && !strstr(cl->lastClientCommandString, "nextdl") ) {
		if ( serverId >= sv.restartedServerId && serverId < sv.serverId ) { // TTimo - use a comparison here to catch multiple map_restart
			// they just haven't caught the map_restart yet
			Com_DPrintf("%s : ignoring pre map_restart / outdated client message\n", cl->name);
			return;
		}
		// if we can tell that the client has dropped the last
		// gamestate we sent them, resend it
		if ( cl->state != CS_ACTIVE && ( cl->messageAcknowledge > cl->gamestateMessageNum || cl->contentGamestateDue ) ) {
			Com_DPrintf( "%s : dropped gamestate, resending\n", cl->name );
			SV_ResendGameState( cl );
		}
		return;
	}

	// this client has acknowledged the new gamestate so it's
	// safe to start sending it the real time again
	if( cl->oldServerTime && serverId == sv.serverId ){
		Com_DPrintf( "%s acknowledged gamestate\n", cl->name );
		cl->oldServerTime = 0;
	}

	// read optional clientCommand strings
	do {
		c = MSG_ReadByte( msg );

		if ( c == clc_EOF ) {
			break;
		}

		if ( c != clc_clientCommand ) {
			break;
		}
		if ( !SV_ClientCommand( cl, msg ) ) {
			return;	// we couldn't execute it because of the flood protection
		}
		if (cl->state == CS_ZOMBIE) {
			return;	// disconnect command
		}
	} while ( 1 );

	// skip legacy speex voip data
	if ( c == clc_voipSpeex ) {
#ifdef USE_VOIP
		SV_UserVoip( cl, msg, qtrue );
		c = MSG_ReadByte( msg );
#endif
	}

	// read optional voip data
	if ( c == clc_voipOpus ) {
#ifdef USE_VOIP
		SV_UserVoip( cl, msg, qfalse );
		c = MSG_ReadByte( msg );
#endif
	}

	// read the usercmd_t
	if ( c == clc_move ) {
		SV_UserMove( cl, msg, qtrue );
	} else if ( c == clc_moveNoDelta ) {
		SV_UserMove( cl, msg, qfalse );
	} else if ( c != clc_EOF ) {
		Com_Printf( "WARNING: bad command byte for client %i\n", (int) (cl - svs.clients) );
	}
//	if ( msg->readcount != msg->cursize ) {
//		Com_Printf( "WARNING: Junk at end of packet for client %i\n", cl - svs.clients );
//	}
}
