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
// sv_game.c -- interface to the game dll

#include "server.h"

#include "../botlib/botlib.h"
#include "../botlib/be_aas.h"
#include "../botlib/be_ai_chat.h"
#include "../botlib/be_ai_goal.h"
#include "../botlib/be_ai_move.h"
#include "../botlib/be_ai_weap.h"

botlib_export_t	*botlib_export;

// the number of game clients G_LOCATE_GAME_DATA's array was checked for
static int	sv_numGameClients;

// the game set botlib up and hasn't shut it down; a game freed after an
// error never does, and the next game's setup would start from its state
static qboolean	sv_gameBotLibSetUp;

// these functions must be used instead of pointer arithmetic, because
// the game allocates gentities with private information after the server shared part
int	SV_NumForGentity( sharedEntity_t *ent ) {
	int		num;

	num = ( (byte *)ent - (byte *)sv.gentities ) / sv.gentitySize;

	return num;
}

sharedEntity_t *SV_GentityNum( int num ) {
	sharedEntity_t *ent;

	// the game module said how many entities its array holds
	if ( num < 0 || num >= sv.num_entities || !sv.gentities ) {
		Com_Error( ERR_DROP, "SV_GentityNum: bad number %i (%i entities)", num, sv.num_entities );
	}

	ent = (sharedEntity_t *)((byte *)sv.gentities + sv.gentitySize*(num));

	return ent;
}

playerState_t *SV_GameClientNum( int num ) {
	playerState_t	*ps;

	if ( num < 0 || num >= sv_numGameClients || !sv.gameClients ) {
		Com_Error( ERR_DROP, "SV_GameClientNum: bad number %i (%i clients)", num, sv_numGameClients );
	}

	ps = (playerState_t *)((byte *)sv.gameClients + sv.gameClientSize*(num));

	return ps;
}

svEntity_t	*SV_SvEntityForGentity( sharedEntity_t *gEnt ) {
	if ( !gEnt || gEnt->s.number < 0 || gEnt->s.number >= MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "SV_SvEntityForGentity: bad gEnt" );
	}
	return &sv.svEntities[ gEnt->s.number ];
}

sharedEntity_t *SV_GEntityForSvEntity( svEntity_t *svEnt ) {
	int		num;

	num = svEnt - sv.svEntities;
	return SV_GentityNum( num );
}

/*
===============
SV_GameSendServerCommand

Sends a command string to a client
===============
*/
void SV_GameSendServerCommand( int clientNum, const char *text ) {
	if ( clientNum == -1 ) {
		SV_SendServerCommand( NULL, "%s", text );
	} else {
		if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
			return;
		}
		SV_SendServerCommand( svs.clients + clientNum, "%s", text );	
	}
}


/*
===============
SV_GameDropClient

Disconnects the client with a message
===============
*/
void SV_GameDropClient( int clientNum, const char *reason ) {
	if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
		return;
	}
	SV_DropClient( svs.clients + clientNum, reason );	
}


/*
=================
SV_SetBrushModel

sets mins and maxs for inline bmodels
=================
*/
void SV_SetBrushModel( sharedEntity_t *ent, const char *name ) {
	clipHandle_t	h;
	vec3_t			mins, maxs;

	if (!name) {
		Com_Error( ERR_DROP, "SV_SetBrushModel: NULL" );
	}

	if (name[0] != '*') {
		Com_Error( ERR_DROP, "SV_SetBrushModel: %s isn't a brush model", name );
	}


	ent->s.modelindex = atoi( name + 1 );

	h = CM_InlineModel( ent->s.modelindex );
	CM_ModelBounds( h, mins, maxs );
	VectorCopy (mins, ent->r.mins);
	VectorCopy (maxs, ent->r.maxs);
	ent->r.bmodel = qtrue;

	ent->r.contents = -1;		// we don't know exactly what is in the brushes

	SV_LinkEntity( ent );		// FIXME: remove
}



/*
=================
SV_inPVS

Also checks portalareas so that doors block sight
=================
*/
qboolean SV_inPVS (const vec3_t p1, const vec3_t p2)
{
	int		leafnum;
	int		cluster;
	int		area1, area2;
	byte	*mask;

	// a point in solid, in no cluster, sees nothing and isn't seen
	leafnum = CM_PointLeafnum (p1);
	cluster = CM_LeafCluster (leafnum);
	if ( cluster < 0 )
		return qfalse;
	area1 = CM_LeafArea (leafnum);
	mask = CM_ClusterPVS (cluster);

	leafnum = CM_PointLeafnum (p2);
	cluster = CM_LeafCluster (leafnum);
	if ( cluster < 0 )
		return qfalse;
	area2 = CM_LeafArea (leafnum);
	if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
		return qfalse;
	if (!CM_AreasConnected (area1, area2))
		return qfalse;		// a door blocks sight
	return qtrue;
}


/*
=================
SV_inPVSIgnorePortals

Does NOT check portalareas
=================
*/
qboolean SV_inPVSIgnorePortals( const vec3_t p1, const vec3_t p2)
{
	int		leafnum;
	int		cluster;
	byte	*mask;

	// a point in solid, in no cluster, sees nothing and isn't seen
	leafnum = CM_PointLeafnum (p1);
	cluster = CM_LeafCluster (leafnum);
	if ( cluster < 0 )
		return qfalse;
	mask = CM_ClusterPVS (cluster);

	leafnum = CM_PointLeafnum (p2);
	cluster = CM_LeafCluster (leafnum);
	if ( cluster < 0 )
		return qfalse;

	if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
		return qfalse;

	return qtrue;
}


/*
========================
SV_AdjustAreaPortalState
========================
*/
void SV_AdjustAreaPortalState( sharedEntity_t *ent, qboolean open ) {
	svEntity_t	*svEnt;

	svEnt = SV_SvEntityForGentity( ent );
	if ( svEnt->areanum2 == -1 ) {
		return;
	}
	CM_AdjustAreaPortalState( svEnt->areanum, svEnt->areanum2, open );
}


/*
==================
SV_EntityContact
==================
*/
qboolean	SV_EntityContact( vec3_t mins, vec3_t maxs, const sharedEntity_t *gEnt, int capsule ) {
	const float	*origin, *angles;
	clipHandle_t	ch;
	trace_t			trace;

	// check for exact collision
	origin = gEnt->r.currentOrigin;
	angles = gEnt->r.currentAngles;

	ch = SV_ClipHandleForEntity( gEnt );
	CM_TransformedBoxTrace ( &trace, vec3_origin, vec3_origin, mins, maxs,
		ch, -1, origin, angles, capsule );

	return trace.startsolid;
}


/*
===============
SV_GetServerinfo

===============
*/
void SV_GetServerinfo( char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetServerinfo: bufferSize == %i", bufferSize );
	}
	Q_strncpyz( buffer, Cvar_InfoString( CVAR_SERVERINFO ), bufferSize );
}

/*
===============
SV_LocateGameData

Keeps the game module's entity and client arrays, whose counts and strides
G_LOCATE_GAME_DATA has checked; SV_GentityNum and SV_GameClientNum index
them only within those counts
===============
*/
void SV_LocateGameData( sharedEntity_t *gEnts, int numGEntities, int sizeofGEntity_t,
					   playerState_t *clients, int numGameClients, int sizeofGameClient ) {
	sv.gentities = gEnts;
	sv.gentitySize = sizeofGEntity_t;
	sv.num_entities = numGEntities;

	sv.gameClients = clients;
	sv.gameClientSize = sizeofGameClient;
	sv_numGameClients = numGameClients;
}


/*
===============
SV_GetUsercmd

===============
*/
void SV_GetUsercmd( int clientNum, usercmd_t *cmd ) {
	if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
		Com_Error( ERR_DROP, "SV_GetUsercmd: bad clientNum:%i", clientNum );
	}
	*cmd = svs.clients[clientNum].lastUsercmd;
}

//==============================================

static int	FloatAsInt( float f ) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}

// the engine extensions the game module can look up with trap_GetValue;
// each has its case in SV_GameSystemCalls
static const vmExtension_t sv_gameExtensions[] = {
	{ "trap_Cvar_SetDescription_Q3E", COM_TRAP_CVAR_SETDESCRIPTION },
	{ NULL, 0 }
};

// the syscalls SV_GameSystemCalls handles, by number, for its errors
static const char * const sv_gameSyscallNames[] = {
	SYSCALL( G_PRINT ),
	SYSCALL( G_ERROR ),
	SYSCALL( G_MILLISECONDS ),
	SYSCALL( G_CVAR_REGISTER ),
	SYSCALL( G_CVAR_UPDATE ),
	SYSCALL( G_CVAR_SET ),
	SYSCALL( G_CVAR_VARIABLE_INTEGER_VALUE ),
	SYSCALL( G_CVAR_VARIABLE_STRING_BUFFER ),
	SYSCALL( G_ARGC ),
	SYSCALL( G_ARGV ),
	SYSCALL( G_FS_FOPEN_FILE ),
	SYSCALL( G_FS_READ ),
	SYSCALL( G_FS_WRITE ),
	SYSCALL( G_FS_FCLOSE_FILE ),
	SYSCALL( G_SEND_CONSOLE_COMMAND ),
	SYSCALL( G_LOCATE_GAME_DATA ),
	SYSCALL( G_DROP_CLIENT ),
	SYSCALL( G_SEND_SERVER_COMMAND ),
	SYSCALL( G_SET_CONFIGSTRING ),
	SYSCALL( G_GET_CONFIGSTRING ),
	SYSCALL( G_GET_USERINFO ),
	SYSCALL( G_SET_USERINFO ),
	SYSCALL( G_GET_SERVERINFO ),
	SYSCALL( G_SET_BRUSH_MODEL ),
	SYSCALL( G_TRACE ),
	SYSCALL( G_POINT_CONTENTS ),
	SYSCALL( G_IN_PVS ),
	SYSCALL( G_IN_PVS_IGNORE_PORTALS ),
	SYSCALL( G_ADJUST_AREA_PORTAL_STATE ),
	SYSCALL( G_AREAS_CONNECTED ),
	SYSCALL( G_LINKENTITY ),
	SYSCALL( G_UNLINKENTITY ),
	SYSCALL( G_ENTITIES_IN_BOX ),
	SYSCALL( G_ENTITY_CONTACT ),
	SYSCALL( G_BOT_ALLOCATE_CLIENT ),
	SYSCALL( G_BOT_FREE_CLIENT ),
	SYSCALL( G_GET_USERCMD ),
	SYSCALL( G_GET_ENTITY_TOKEN ),
	SYSCALL( G_FS_GETFILELIST ),
	SYSCALL( G_DEBUG_POLYGON_CREATE ),
	SYSCALL( G_DEBUG_POLYGON_DELETE ),
	SYSCALL( G_REAL_TIME ),
	SYSCALL( G_SNAPVECTOR ),
	SYSCALL( G_TRACECAPSULE ),
	SYSCALL( G_ENTITY_CONTACTCAPSULE ),
	SYSCALL( G_FS_SEEK ),
	SYSCALL( BOTLIB_SETUP ),
	SYSCALL( BOTLIB_SHUTDOWN ),
	SYSCALL( BOTLIB_LIBVAR_SET ),
	SYSCALL( BOTLIB_LIBVAR_GET ),
	SYSCALL( BOTLIB_PC_ADD_GLOBAL_DEFINE ),
	SYSCALL( BOTLIB_START_FRAME ),
	SYSCALL( BOTLIB_LOAD_MAP ),
	SYSCALL( BOTLIB_UPDATENTITY ),
	SYSCALL( BOTLIB_TEST ),
	SYSCALL( BOTLIB_GET_SNAPSHOT_ENTITY ),
	SYSCALL( BOTLIB_GET_CONSOLE_MESSAGE ),
	SYSCALL( BOTLIB_USER_COMMAND ),
	SYSCALL( BOTLIB_AAS_ENABLE_ROUTING_AREA ),
	SYSCALL( BOTLIB_AAS_BBOX_AREAS ),
	SYSCALL( BOTLIB_AAS_AREA_INFO ),
	SYSCALL( BOTLIB_AAS_ENTITY_INFO ),
	SYSCALL( BOTLIB_AAS_INITIALIZED ),
	SYSCALL( BOTLIB_AAS_PRESENCE_TYPE_BOUNDING_BOX ),
	SYSCALL( BOTLIB_AAS_TIME ),
	SYSCALL( BOTLIB_AAS_POINT_AREA_NUM ),
	SYSCALL( BOTLIB_AAS_TRACE_AREAS ),
	SYSCALL( BOTLIB_AAS_POINT_CONTENTS ),
	SYSCALL( BOTLIB_AAS_NEXT_BSP_ENTITY ),
	SYSCALL( BOTLIB_AAS_VALUE_FOR_BSP_EPAIR_KEY ),
	SYSCALL( BOTLIB_AAS_VECTOR_FOR_BSP_EPAIR_KEY ),
	SYSCALL( BOTLIB_AAS_FLOAT_FOR_BSP_EPAIR_KEY ),
	SYSCALL( BOTLIB_AAS_INT_FOR_BSP_EPAIR_KEY ),
	SYSCALL( BOTLIB_AAS_AREA_REACHABILITY ),
	SYSCALL( BOTLIB_AAS_AREA_TRAVEL_TIME_TO_GOAL_AREA ),
	SYSCALL( BOTLIB_AAS_SWIMMING ),
	SYSCALL( BOTLIB_AAS_PREDICT_CLIENT_MOVEMENT ),
	SYSCALL( BOTLIB_EA_SAY ),
	SYSCALL( BOTLIB_EA_SAY_TEAM ),
	SYSCALL( BOTLIB_EA_COMMAND ),
	SYSCALL( BOTLIB_EA_ACTION ),
	SYSCALL( BOTLIB_EA_GESTURE ),
	SYSCALL( BOTLIB_EA_TALK ),
	SYSCALL( BOTLIB_EA_ATTACK ),
	SYSCALL( BOTLIB_EA_USE ),
	SYSCALL( BOTLIB_EA_RESPAWN ),
	SYSCALL( BOTLIB_EA_CROUCH ),
	SYSCALL( BOTLIB_EA_MOVE_UP ),
	SYSCALL( BOTLIB_EA_MOVE_DOWN ),
	SYSCALL( BOTLIB_EA_MOVE_FORWARD ),
	SYSCALL( BOTLIB_EA_MOVE_BACK ),
	SYSCALL( BOTLIB_EA_MOVE_LEFT ),
	SYSCALL( BOTLIB_EA_MOVE_RIGHT ),
	SYSCALL( BOTLIB_EA_SELECT_WEAPON ),
	SYSCALL( BOTLIB_EA_JUMP ),
	SYSCALL( BOTLIB_EA_DELAYED_JUMP ),
	SYSCALL( BOTLIB_EA_MOVE ),
	SYSCALL( BOTLIB_EA_VIEW ),
	SYSCALL( BOTLIB_EA_END_REGULAR ),
	SYSCALL( BOTLIB_EA_GET_INPUT ),
	SYSCALL( BOTLIB_EA_RESET_INPUT ),
	SYSCALL( BOTLIB_AI_LOAD_CHARACTER ),
	SYSCALL( BOTLIB_AI_FREE_CHARACTER ),
	SYSCALL( BOTLIB_AI_CHARACTERISTIC_FLOAT ),
	SYSCALL( BOTLIB_AI_CHARACTERISTIC_BFLOAT ),
	SYSCALL( BOTLIB_AI_CHARACTERISTIC_INTEGER ),
	SYSCALL( BOTLIB_AI_CHARACTERISTIC_BINTEGER ),
	SYSCALL( BOTLIB_AI_CHARACTERISTIC_STRING ),
	SYSCALL( BOTLIB_AI_ALLOC_CHAT_STATE ),
	SYSCALL( BOTLIB_AI_FREE_CHAT_STATE ),
	SYSCALL( BOTLIB_AI_QUEUE_CONSOLE_MESSAGE ),
	SYSCALL( BOTLIB_AI_REMOVE_CONSOLE_MESSAGE ),
	SYSCALL( BOTLIB_AI_NEXT_CONSOLE_MESSAGE ),
	SYSCALL( BOTLIB_AI_NUM_CONSOLE_MESSAGE ),
	SYSCALL( BOTLIB_AI_INITIAL_CHAT ),
	SYSCALL( BOTLIB_AI_REPLY_CHAT ),
	SYSCALL( BOTLIB_AI_CHAT_LENGTH ),
	SYSCALL( BOTLIB_AI_ENTER_CHAT ),
	SYSCALL( BOTLIB_AI_STRING_CONTAINS ),
	SYSCALL( BOTLIB_AI_FIND_MATCH ),
	SYSCALL( BOTLIB_AI_MATCH_VARIABLE ),
	SYSCALL( BOTLIB_AI_UNIFY_WHITE_SPACES ),
	SYSCALL( BOTLIB_AI_REPLACE_SYNONYMS ),
	SYSCALL( BOTLIB_AI_LOAD_CHAT_FILE ),
	SYSCALL( BOTLIB_AI_SET_CHAT_GENDER ),
	SYSCALL( BOTLIB_AI_SET_CHAT_NAME ),
	SYSCALL( BOTLIB_AI_RESET_GOAL_STATE ),
	SYSCALL( BOTLIB_AI_RESET_AVOID_GOALS ),
	SYSCALL( BOTLIB_AI_PUSH_GOAL ),
	SYSCALL( BOTLIB_AI_POP_GOAL ),
	SYSCALL( BOTLIB_AI_EMPTY_GOAL_STACK ),
	SYSCALL( BOTLIB_AI_DUMP_AVOID_GOALS ),
	SYSCALL( BOTLIB_AI_DUMP_GOAL_STACK ),
	SYSCALL( BOTLIB_AI_GOAL_NAME ),
	SYSCALL( BOTLIB_AI_GET_TOP_GOAL ),
	SYSCALL( BOTLIB_AI_GET_SECOND_GOAL ),
	SYSCALL( BOTLIB_AI_CHOOSE_LTG_ITEM ),
	SYSCALL( BOTLIB_AI_CHOOSE_NBG_ITEM ),
	SYSCALL( BOTLIB_AI_TOUCHING_GOAL ),
	SYSCALL( BOTLIB_AI_ITEM_GOAL_IN_VIS_BUT_NOT_VISIBLE ),
	SYSCALL( BOTLIB_AI_GET_LEVEL_ITEM_GOAL ),
	SYSCALL( BOTLIB_AI_AVOID_GOAL_TIME ),
	SYSCALL( BOTLIB_AI_INIT_LEVEL_ITEMS ),
	SYSCALL( BOTLIB_AI_UPDATE_ENTITY_ITEMS ),
	SYSCALL( BOTLIB_AI_LOAD_ITEM_WEIGHTS ),
	SYSCALL( BOTLIB_AI_FREE_ITEM_WEIGHTS ),
	SYSCALL( BOTLIB_AI_SAVE_GOAL_FUZZY_LOGIC ),
	SYSCALL( BOTLIB_AI_ALLOC_GOAL_STATE ),
	SYSCALL( BOTLIB_AI_FREE_GOAL_STATE ),
	SYSCALL( BOTLIB_AI_RESET_MOVE_STATE ),
	SYSCALL( BOTLIB_AI_MOVE_TO_GOAL ),
	SYSCALL( BOTLIB_AI_MOVE_IN_DIRECTION ),
	SYSCALL( BOTLIB_AI_RESET_AVOID_REACH ),
	SYSCALL( BOTLIB_AI_RESET_LAST_AVOID_REACH ),
	SYSCALL( BOTLIB_AI_REACHABILITY_AREA ),
	SYSCALL( BOTLIB_AI_MOVEMENT_VIEW_TARGET ),
	SYSCALL( BOTLIB_AI_ALLOC_MOVE_STATE ),
	SYSCALL( BOTLIB_AI_FREE_MOVE_STATE ),
	SYSCALL( BOTLIB_AI_INIT_MOVE_STATE ),
	SYSCALL( BOTLIB_AI_CHOOSE_BEST_FIGHT_WEAPON ),
	SYSCALL( BOTLIB_AI_GET_WEAPON_INFO ),
	SYSCALL( BOTLIB_AI_LOAD_WEAPON_WEIGHTS ),
	SYSCALL( BOTLIB_AI_ALLOC_WEAPON_STATE ),
	SYSCALL( BOTLIB_AI_FREE_WEAPON_STATE ),
	SYSCALL( BOTLIB_AI_RESET_WEAPON_STATE ),
	SYSCALL( BOTLIB_AI_GENETIC_PARENTS_AND_CHILD_SELECTION ),
	SYSCALL( BOTLIB_AI_INTERBREED_GOAL_FUZZY_LOGIC ),
	SYSCALL( BOTLIB_AI_MUTATE_GOAL_FUZZY_LOGIC ),
	SYSCALL( BOTLIB_AI_GET_NEXT_CAMP_SPOT_GOAL ),
	SYSCALL( BOTLIB_AI_GET_MAP_LOCATION_GOAL ),
	SYSCALL( BOTLIB_AI_NUM_INITIAL_CHATS ),
	SYSCALL( BOTLIB_AI_GET_CHAT_MESSAGE ),
	SYSCALL( BOTLIB_AI_REMOVE_FROM_AVOID_GOALS ),
	SYSCALL( BOTLIB_AI_PREDICT_VISIBLE_POSITION ),
	SYSCALL( BOTLIB_AI_SET_AVOID_GOAL_TIME ),
	SYSCALL( BOTLIB_AI_ADD_AVOID_SPOT ),
	SYSCALL( BOTLIB_AAS_ALTERNATIVE_ROUTE_GOAL ),
	SYSCALL( BOTLIB_AAS_PREDICT_ROUTE ),
	SYSCALL( BOTLIB_AAS_POINT_REACHABILITY_AREA_INDEX ),
	SYSCALL( BOTLIB_PC_LOAD_SOURCE ),
	SYSCALL( BOTLIB_PC_FREE_SOURCE ),
	SYSCALL( BOTLIB_PC_READ_TOKEN ),
	SYSCALL( BOTLIB_PC_SOURCE_FILE_AND_LINE ),
	SYSCALL( TRAP_MEMSET ),
	SYSCALL( TRAP_MEMCPY ),
	SYSCALL( TRAP_STRNCPY ),
	SYSCALL( TRAP_SIN ),
	SYSCALL( TRAP_COS ),
	SYSCALL( TRAP_ATAN2 ),
	SYSCALL( TRAP_SQRT ),
	SYSCALL( TRAP_MATRIXMULTIPLY ),
	SYSCALL( TRAP_ANGLEVECTORS ),
	SYSCALL( TRAP_PERPENDICULARVECTOR ),
	SYSCALL( TRAP_FLOOR ),
	SYSCALL( TRAP_CEIL ),
	SYSCALL( COM_TRAP_GETVALUE ),
	SYSCALL( COM_TRAP_CVAR_SETDESCRIPTION ),
};

/*
====================
SV_GameSystemCalls

The module is making a system call
====================
*/
intptr_t SV_GameSystemCalls( intptr_t *args ) {
	switch( args[0] ) {
	case G_PRINT:
		Com_Printf( "%s", VMA_STR( 1 ) );
		return 0;
	case G_ERROR:
		Com_Error( ERR_DROP, "%s", VMA_STR( 1 ) );
		return 0;
	case G_MILLISECONDS:
		return Sys_Milliseconds();
	case G_CVAR_REGISTER:
		Cvar_Register( VMA_OUT_OPT( 1, vmCvar_t ), VMA_STR( 2 ), VMA_STR( 3 ), args[4] );
		return 0;
	case G_CVAR_UPDATE:
		Cvar_Update( VMA_INOUT( 1, vmCvar_t ) );
		return 0;
	case G_CVAR_SET:
		Cvar_SetFromVM( VMA_STR( 1 ), VMA_STR_OPT( 2 ), NULL );
		return 0;
	case G_CVAR_VARIABLE_INTEGER_VALUE:
		return Cvar_VariableIntegerValueSafe( VMA_STR( 1 ) );
	case G_CVAR_VARIABLE_STRING_BUFFER:
		Cvar_VariableStringBufferSafe( VMA_STR( 1 ), VMA_STRBUF( 2, args[3] ), args[3] );
		return 0;
	case G_ARGC:
		return Cmd_Argc();
	case G_ARGV:
		Cmd_ArgvBuffer( args[1], VMA_STRBUF( 2, args[3] ), args[3] );
		return 0;
	case G_SEND_CONSOLE_COMMAND:
		// EXEC_NOW takes NULL for the current command line
		Cbuf_ExecuteTextRestricted( args[1], (int)args[1] == EXEC_NOW ? VMA_STR_OPT( 2 ) : VMA_STR( 2 ) );
		return 0;

	case G_FS_FOPEN_FILE:
		return VM_FOpenFile( args );
	case G_FS_READ:
		FS_Read( VMA_BUF( 1, args[2] ), args[2], VMV_FILE( 3 ) );
		return 0;
	case G_FS_WRITE:
		FS_Write( VMA_BUF( 1, args[2] ), args[2], VMV_FILE( 3 ) );
		return 0;
	case G_FS_FCLOSE_FILE:
		FS_FCloseFile( VMV_FILE( 1 ) );
		return 0;
	case G_FS_GETFILELIST:
		// the list is terminated, even an empty one
		return FS_GetFileList( VMA_STR( 1 ), VMA_STR_OPT( 2 ), VMA_STRBUF( 3, args[4] ), args[4] );
	case G_FS_SEEK:
		return FS_Seek( VMV_FILE( 1 ), args[2], args[3] );

	case G_LOCATE_GAME_DATA: {
		// the server indexes both arrays from now on, so their counts and
		// strides must cover what the module really has
		int		numEntities = VMV_COUNT( 2, MAX_GENTITIES );
		int		entitySize = VMV_RANGE( 3, (int)sizeof( sharedEntity_t ), INT_MAX );
		int		numClients = sv_maxclients->integer;
		int		clientSize = VMV_RANGE( 5, (int)sizeof( playerState_t ), INT_MAX );

		SV_LocateGameData( VM_ArgArray( args, 1, entitySize, numEntities, 1 ), numEntities, entitySize,
			VM_ArgArray( args, 4, clientSize, numClients, 1 ), numClients, clientSize );
		return 0;
	}
	case G_DROP_CLIENT:
		// a bad client is ignored, as it always was
		SV_GameDropClient( args[1], VMA_STR( 2 ) );
		return 0;
	case G_SEND_SERVER_COMMAND:
		// -1 sends to every client; another bad client is ignored, as it
		// always was
		SV_GameSendServerCommand( args[1], VMA_STR( 2 ) );
		return 0;
	case G_LINKENTITY:
		SV_LinkEntity( VMA_INOUT( 1, sharedEntity_t ) );
		return 0;
	case G_UNLINKENTITY:
		SV_UnlinkEntity( VMA_INOUT( 1, sharedEntity_t ) );
		return 0;
	case G_ENTITIES_IN_BOX: {
		// the list never holds more than every entity
		int		maxcount = VMV_COUNT( 4, INT_MAX );

		return SV_AreaEntities( VMA_VEC3( 1 ), VMA_VEC3( 2 ),
			VMA_ARRAY( 3, int, MIN( maxcount, MAX_GENTITIES ) ), maxcount );
	}
	case G_ENTITY_CONTACT:
		return SV_EntityContact( VMA_VEC3( 1 ), VMA_VEC3( 2 ), VMA_IN( 3, sharedEntity_t ), /*int capsule*/ qfalse );
	case G_ENTITY_CONTACTCAPSULE:
		return SV_EntityContact( VMA_VEC3( 1 ), VMA_VEC3( 2 ), VMA_IN( 3, sharedEntity_t ), /*int capsule*/ qtrue );
	case G_TRACE:
		SV_Trace( VMA_OUT( 1, trace_t ), VMA_VEC3( 2 ), VMA_VEC3_OPT( 3 ), VMA_VEC3_OPT( 4 ), VMA_VEC3( 5 ),
			VMV_PASSENT( 6 ), args[7], /*int capsule*/ qfalse );
		return 0;
	case G_TRACECAPSULE:
		SV_Trace( VMA_OUT( 1, trace_t ), VMA_VEC3( 2 ), VMA_VEC3_OPT( 3 ), VMA_VEC3_OPT( 4 ), VMA_VEC3( 5 ),
			VMV_PASSENT( 6 ), args[7], /*int capsule*/ qtrue );
		return 0;
	case G_POINT_CONTENTS:
		// passEntityNum is only compared, and game code passes -1 for none
		return SV_PointContents( VMA_VEC3( 1 ), args[2] );
	case G_SET_BRUSH_MODEL:
		SV_SetBrushModel( VMA_INOUT( 1, sharedEntity_t ), VMA_STR( 2 ) );
		return 0;
	case G_IN_PVS:
		return SV_inPVS( VMA_VEC3( 1 ), VMA_VEC3( 2 ) );
	case G_IN_PVS_IGNORE_PORTALS:
		return SV_inPVSIgnorePortals( VMA_VEC3( 1 ), VMA_VEC3( 2 ) );

	case G_SET_CONFIGSTRING:
		SV_SetConfigstring( VMV_RANGE( 1, 0, MAX_CONFIGSTRINGS - 1 ), VMA_STR_OPT( 2 ) );
		return 0;
	case G_GET_CONFIGSTRING:
		SV_GetConfigstring( VMV_RANGE( 1, 0, MAX_CONFIGSTRINGS - 1 ), VMA_STRBUF( 2, args[3] ), args[3] );
		return 0;
	case G_SET_USERINFO:
		SV_SetUserinfo( VMV_CLIENT( 1 ), VMA_STR_OPT( 2 ) );
		return 0;
	case G_GET_USERINFO:
		SV_GetUserinfo( VMV_CLIENT( 1 ), VMA_STRBUF( 2, args[3] ), args[3] );
		return 0;
	case G_GET_SERVERINFO:
		SV_GetServerinfo( VMA_STRBUF( 1, args[2] ), args[2] );
		return 0;
	case G_ADJUST_AREA_PORTAL_STATE:
		SV_AdjustAreaPortalState( VMA_INOUT( 1, sharedEntity_t ), args[2] );
		return 0;
	case G_AREAS_CONNECTED:
		return CM_AreasConnected( args[1], args[2] );

	case G_BOT_ALLOCATE_CLIENT:
		return SV_BotAllocateClient();
	case G_BOT_FREE_CLIENT:
		SV_BotFreeClient( VMV_CLIENT( 1 ) );
		return 0;

	case G_GET_USERCMD:
		SV_GetUsercmd( VMV_CLIENT( 1 ), VMA_OUT( 2, usercmd_t ) );
		return 0;
	case G_GET_ENTITY_TOKEN:
		{
			const char	*s;

			s = COM_Parse( &sv.entityParsePoint );
			Q_strncpyz( VMA_STRBUF( 1, args[2] ), s, args[2] );
			if ( !sv.entityParsePoint && !s[0] ) {
				return qfalse;
			} else {
				return qtrue;
			}
		}

	case G_DEBUG_POLYGON_CREATE: {
		int		numPoints = VMV_COUNT( 2, MAX_DEBUGPOLYGON_POINTS );

		return BotImport_DebugPolygonCreate( args[1], numPoints,
			VMA_ARRAY( 3, vec3_t, numPoints ) );
	}
	case G_DEBUG_POLYGON_DELETE:
		BotImport_DebugPolygonDelete( args[1] );
		return 0;
	case G_REAL_TIME:
		return Com_RealTime( VMA_OUT_OPT( 1, qtime_t ) );
	case G_SNAPVECTOR:
		// Q_SnapVector is a function of a vec3_t or a macro of a vec3_t *
		Q_SnapVector( VMA_BUF( 1, sizeof( vec3_t ) ) );
		return 0;

		//====================================

	case BOTLIB_SETUP:
		{
			int		result = SV_BotLibSetup();

			sv_gameBotLibSetUp = result == 0;
			return result;
		}
	case BOTLIB_SHUTDOWN:
		sv_gameBotLibSetUp = qfalse;
		return SV_BotLibShutdown();
	case BOTLIB_LIBVAR_SET:
		return botlib_export->BotLibVarSet( VMA_STR( 1 ), VMA_STR( 2 ) );
	case BOTLIB_LIBVAR_GET:
		return botlib_export->BotLibVarGet( VMA_STR( 1 ), VMA_STRBUF( 2, args[3] ), args[3] );

	case BOTLIB_PC_ADD_GLOBAL_DEFINE:
		return botlib_export->PC_AddGlobalDefine( (char *)VMA_STR( 1 ) );
	case BOTLIB_PC_LOAD_SOURCE:
		return botlib_export->PC_LoadSourceHandle( VMA_STR( 1 ) );
	case BOTLIB_PC_FREE_SOURCE:
		return botlib_export->PC_FreeSourceHandle( args[1] );
	case BOTLIB_PC_READ_TOKEN:
		return botlib_export->PC_ReadTokenHandle( args[1], VMA_OUT( 2, pc_token_t ) );
	case BOTLIB_PC_SOURCE_FILE_AND_LINE:
		return botlib_export->PC_SourceFileAndLine( args[1], VMA_BUF( 2, PC_MODULE_FILENAME_SIZE ),
			PC_MODULE_FILENAME_SIZE, VMA_OUT( 3, int ) );

	case BOTLIB_START_FRAME:
		return botlib_export->BotLibStartFrame( VMF(1) );
	case BOTLIB_LOAD_MAP:
		return botlib_export->BotLibLoadMap( VMA_STR_OPT( 1 ) );
	case BOTLIB_UPDATENTITY:
		// NULL unlinks the entity
		return botlib_export->BotLibUpdateEntity( VMV_ENTITY( 1 ), (bot_entitystate_t *)VMA_IN_OPT( 2, bot_entitystate_t ) );
	case BOTLIB_TEST:
		return botlib_export->Test( args[1], (char *)VMA_STR_OPT( 2 ), VMA_VEC3( 3 ), VMA_VEC3( 4 ) );

	case BOTLIB_GET_SNAPSHOT_ENTITY:
		return SV_BotGetSnapshotEntity( VMV_CLIENT( 1 ), args[2] );
	case BOTLIB_GET_CONSOLE_MESSAGE:
		return SV_BotGetConsoleMessage( VMV_CLIENT( 1 ), VMA_STRBUF( 2, args[3] ), args[3] );
	case BOTLIB_USER_COMMAND: {
		int		clientNum = VMV_CLIENT( 1 );

		SV_ClientThink( &svs.clients[clientNum], (usercmd_t *)VMA_IN( 2, usercmd_t ) );
		return 0;
	}

	case BOTLIB_AAS_BBOX_AREAS: {
		// it writes one area before it looks at the count
		int		maxareas = VMV_COUNT( 4, INT_MAX );

		return botlib_export->aas.AAS_BBoxAreas( VMA_VEC3( 1 ), VMA_VEC3( 2 ),
			VMA_ARRAY( 3, int, MAX( maxareas, 1 ) ), maxareas );
	}
	case BOTLIB_AAS_AREA_INFO:
		return botlib_export->aas.AAS_AreaInfo( args[1], VMA_OUT( 2, aas_areainfo_t ) );
	case BOTLIB_AAS_ALTERNATIVE_ROUTE_GOAL: {
		// it writes one goal before it looks at the count
		int		maxgoals = VMV_COUNT( 7, INT_MAX );

		return botlib_export->aas.AAS_AlternativeRouteGoals( VMA_VEC3( 1 ), args[2], VMA_VEC3( 3 ), args[4], args[5],
			VMA_ARRAY( 6, aas_altroutegoal_t, MAX( maxgoals, 1 ) ), maxgoals, args[8] );
	}
	case BOTLIB_AAS_ENTITY_INFO:
		// botlib range-checks the number and clears info for a bad one
		botlib_export->aas.AAS_EntityInfo( args[1], VMA_OUT( 2, aas_entityinfo_t ) );
		return 0;

	case BOTLIB_AAS_INITIALIZED:
		return botlib_export->aas.AAS_Initialized();
	case BOTLIB_AAS_PRESENCE_TYPE_BOUNDING_BOX:
		botlib_export->aas.AAS_PresenceTypeBoundingBox( args[1], VMA_VEC3( 2 ), VMA_VEC3( 3 ) );
		return 0;
	case BOTLIB_AAS_TIME:
		return FloatAsInt( botlib_export->aas.AAS_Time() );

	case BOTLIB_AAS_POINT_AREA_NUM:
		return botlib_export->aas.AAS_PointAreaNum( VMA_VEC3( 1 ) );
	case BOTLIB_AAS_POINT_REACHABILITY_AREA_INDEX:
		return botlib_export->aas.AAS_PointReachabilityAreaIndex( VMA_VEC3_OPT( 1 ) );
	case BOTLIB_AAS_TRACE_AREAS: {
		// it writes one area before it looks at the count; points may be NULL
		int		maxareas = VMV_COUNT( 5, INT_MAX );
		int		size = MAX( maxareas, 1 );

		return botlib_export->aas.AAS_TraceAreas( VMA_VEC3( 1 ), VMA_VEC3( 2 ), VMA_ARRAY( 3, int, size ),
			args[4] ? VMA_ARRAY( 4, vec3_t, size ) : NULL, maxareas );
	}

	case BOTLIB_AAS_POINT_CONTENTS:
		return botlib_export->aas.AAS_PointContents( VMA_VEC3( 1 ) );
	case BOTLIB_AAS_NEXT_BSP_ENTITY:
		return botlib_export->aas.AAS_NextBSPEntity( args[1] );
	case BOTLIB_AAS_VALUE_FOR_BSP_EPAIR_KEY:
		return botlib_export->aas.AAS_ValueForBSPEpairKey( args[1], (char *)VMA_STR( 2 ), VMA_STRBUF( 3, args[4] ), args[4] );
	case BOTLIB_AAS_VECTOR_FOR_BSP_EPAIR_KEY:
		return botlib_export->aas.AAS_VectorForBSPEpairKey( args[1], (char *)VMA_STR( 2 ), VMA_VEC3( 3 ) );
	case BOTLIB_AAS_FLOAT_FOR_BSP_EPAIR_KEY:
		return botlib_export->aas.AAS_FloatForBSPEpairKey( args[1], (char *)VMA_STR( 2 ), VMA_OUT( 3, float ) );
	case BOTLIB_AAS_INT_FOR_BSP_EPAIR_KEY:
		return botlib_export->aas.AAS_IntForBSPEpairKey( args[1], (char *)VMA_STR( 2 ), VMA_OUT( 3, int ) );

	case BOTLIB_AAS_AREA_REACHABILITY:
		return botlib_export->aas.AAS_AreaReachability( args[1] );

	case BOTLIB_AAS_AREA_TRAVEL_TIME_TO_GOAL_AREA:
		return botlib_export->aas.AAS_AreaTravelTimeToGoalArea( args[1], VMA_VEC3_OPT( 2 ), args[3], args[4] );
	case BOTLIB_AAS_ENABLE_ROUTING_AREA:
		return botlib_export->aas.AAS_EnableRoutingArea( args[1], args[2] );
	case BOTLIB_AAS_PREDICT_ROUTE:
		return botlib_export->aas.AAS_PredictRoute( VMA_OUT( 1, aas_predictroute_t ), args[2], VMA_VEC3( 3 ),
			args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11] );

	case BOTLIB_AAS_SWIMMING:
		return botlib_export->aas.AAS_Swimming( VMA_VEC3( 1 ) );
	case BOTLIB_AAS_PREDICT_CLIENT_MOVEMENT:
		// entnum is only compared
		return botlib_export->aas.AAS_PredictClientMovement( VMA_OUT( 1, aas_clientmove_t ), args[2], VMA_VEC3( 3 ),
			args[4], args[5], VMA_VEC3( 6 ), VMA_VEC3( 7 ), args[8], args[9], VMF(10), args[11], args[12], args[13] );

	case BOTLIB_EA_SAY:
		botlib_export->ea.EA_Say( VMV_CLIENT( 1 ), (char *)VMA_STR( 2 ) );
		return 0;
	case BOTLIB_EA_SAY_TEAM:
		botlib_export->ea.EA_SayTeam( VMV_CLIENT( 1 ), (char *)VMA_STR( 2 ) );
		return 0;
	case BOTLIB_EA_COMMAND:
		botlib_export->ea.EA_Command( VMV_CLIENT( 1 ), (char *)VMA_STR( 2 ) );
		return 0;

	case BOTLIB_EA_ACTION:
		botlib_export->ea.EA_Action( VMV_CLIENT( 1 ), args[2] );
		return 0;
	case BOTLIB_EA_GESTURE:
		botlib_export->ea.EA_Gesture( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_TALK:
		botlib_export->ea.EA_Talk( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_ATTACK:
		botlib_export->ea.EA_Attack( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_USE:
		botlib_export->ea.EA_Use( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_RESPAWN:
		botlib_export->ea.EA_Respawn( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_CROUCH:
		botlib_export->ea.EA_Crouch( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_MOVE_UP:
		botlib_export->ea.EA_MoveUp( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_MOVE_DOWN:
		botlib_export->ea.EA_MoveDown( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_MOVE_FORWARD:
		botlib_export->ea.EA_MoveForward( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_MOVE_BACK:
		botlib_export->ea.EA_MoveBack( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_MOVE_LEFT:
		botlib_export->ea.EA_MoveLeft( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_MOVE_RIGHT:
		botlib_export->ea.EA_MoveRight( VMV_CLIENT( 1 ) );
		return 0;

	case BOTLIB_EA_SELECT_WEAPON:
		botlib_export->ea.EA_SelectWeapon( VMV_CLIENT( 1 ), args[2] );
		return 0;
	case BOTLIB_EA_JUMP:
		botlib_export->ea.EA_Jump( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_DELAYED_JUMP:
		botlib_export->ea.EA_DelayedJump( VMV_CLIENT( 1 ) );
		return 0;
	case BOTLIB_EA_MOVE:
		botlib_export->ea.EA_Move( VMV_CLIENT( 1 ), VMA_VEC3( 2 ), VMF(3) );
		return 0;
	case BOTLIB_EA_VIEW:
		botlib_export->ea.EA_View( VMV_CLIENT( 1 ), VMA_VEC3( 2 ) );
		return 0;

	case BOTLIB_EA_END_REGULAR:
		botlib_export->ea.EA_EndRegular( VMV_CLIENT( 1 ), VMF(2) );
		return 0;
	case BOTLIB_EA_GET_INPUT:
		botlib_export->ea.EA_GetInput( VMV_CLIENT( 1 ), VMF(2), VMA_OUT( 3, bot_input_t ) );
		return 0;
	case BOTLIB_EA_RESET_INPUT:
		botlib_export->ea.EA_ResetInput( VMV_CLIENT( 1 ) );
		return 0;

	case BOTLIB_AI_LOAD_CHARACTER:
		return botlib_export->ai.BotLoadCharacter( (char *)VMA_STR( 1 ), VMF(2) );
	case BOTLIB_AI_FREE_CHARACTER:
		botlib_export->ai.BotFreeCharacter( args[1] );
		return 0;
	case BOTLIB_AI_CHARACTERISTIC_FLOAT:
		return FloatAsInt( botlib_export->ai.Characteristic_Float( args[1], args[2] ) );
	case BOTLIB_AI_CHARACTERISTIC_BFLOAT:
		return FloatAsInt( botlib_export->ai.Characteristic_BFloat( args[1], args[2], VMF(3), VMF(4) ) );
	case BOTLIB_AI_CHARACTERISTIC_INTEGER:
		return botlib_export->ai.Characteristic_Integer( args[1], args[2] );
	case BOTLIB_AI_CHARACTERISTIC_BINTEGER:
		return botlib_export->ai.Characteristic_BInteger( args[1], args[2], args[3], args[4] );
	case BOTLIB_AI_CHARACTERISTIC_STRING:
		botlib_export->ai.Characteristic_String( args[1], args[2], VMA_STRBUF( 3, args[4] ), args[4] );
		return 0;

	case BOTLIB_AI_ALLOC_CHAT_STATE:
		return botlib_export->ai.BotAllocChatState();
	case BOTLIB_AI_FREE_CHAT_STATE:
		botlib_export->ai.BotFreeChatState( args[1] );
		return 0;
	case BOTLIB_AI_QUEUE_CONSOLE_MESSAGE:
		botlib_export->ai.BotQueueConsoleMessage( args[1], args[2], (char *)VMA_STR( 3 ) );
		return 0;
	case BOTLIB_AI_REMOVE_CONSOLE_MESSAGE:
		botlib_export->ai.BotRemoveConsoleMessage( args[1], args[2] );
		return 0;
	case BOTLIB_AI_NEXT_CONSOLE_MESSAGE:
		// botlib writes the fields before the list pointers, whose size
		// differs between a QVM and the engine
		return botlib_export->ai.BotNextConsoleMessage( args[1],
			VMA_BUF( 2, offsetof( bot_consolemessage_t, prev ) ) );
	case BOTLIB_AI_NUM_CONSOLE_MESSAGE:
		return botlib_export->ai.BotNumConsoleMessages( args[1] );
	case BOTLIB_AI_INITIAL_CHAT:
		botlib_export->ai.BotInitialChat( args[1], (char *)VMA_STR( 2 ), args[3],
			(char *)VMA_STR_OPT( 4 ), (char *)VMA_STR_OPT( 5 ), (char *)VMA_STR_OPT( 6 ), (char *)VMA_STR_OPT( 7 ),
			(char *)VMA_STR_OPT( 8 ), (char *)VMA_STR_OPT( 9 ), (char *)VMA_STR_OPT( 10 ), (char *)VMA_STR_OPT( 11 ) );
		return 0;
	case BOTLIB_AI_NUM_INITIAL_CHATS:
		return botlib_export->ai.BotNumInitialChats( args[1], (char *)VMA_STR( 2 ) );
	case BOTLIB_AI_REPLY_CHAT:
		return botlib_export->ai.BotReplyChat( args[1], (char *)VMA_STR( 2 ), args[3], args[4],
			(char *)VMA_STR_OPT( 5 ), (char *)VMA_STR_OPT( 6 ), (char *)VMA_STR_OPT( 7 ), (char *)VMA_STR_OPT( 8 ),
			(char *)VMA_STR_OPT( 9 ), (char *)VMA_STR_OPT( 10 ), (char *)VMA_STR_OPT( 11 ), (char *)VMA_STR_OPT( 12 ) );
	case BOTLIB_AI_CHAT_LENGTH:
		return botlib_export->ai.BotChatLength( args[1] );
	case BOTLIB_AI_ENTER_CHAT:
		// clientto is only printed into a tell command
		botlib_export->ai.BotEnterChat( args[1], args[2], args[3] );
		return 0;
	case BOTLIB_AI_GET_CHAT_MESSAGE:
		botlib_export->ai.BotGetChatMessage( args[1], VMA_STRBUF( 2, args[3] ), args[3] );
		return 0;
	case BOTLIB_AI_STRING_CONTAINS:
		// NULL returns -1
		return botlib_export->ai.StringContains( (char *)VMA_STR_OPT( 1 ), (char *)VMA_STR_OPT( 2 ), args[3] );
	case BOTLIB_AI_FIND_MATCH:
		return botlib_export->ai.BotFindMatch( (char *)VMA_STR( 1 ), VMA_OUT( 2, bot_match_t ), args[3] );
	case BOTLIB_AI_MATCH_VARIABLE:
		// it writes a terminator whatever the size
		botlib_export->ai.BotMatchVariable( (bot_match_t *)VMA_IN( 1, bot_match_t ), args[2],
			VMA_STRBUF( 3, args[4] ), args[4] );
		return 0;
	case BOTLIB_AI_UNIFY_WHITE_SPACES:
		// it only ever shortens the string
		botlib_export->ai.UnifyWhiteSpaces( (char *)VMA_STR( 1 ) );
		return 0;
	case BOTLIB_AI_REPLACE_SYNONYMS:
		// the syscall has no size, so assume a whole console message's,
		// the buffer game code expands synonyms in. Older game code passes
		// a pointer into a console message, past the sender's name, which
		// leaves less room.
		botlib_export->ai.BotReplaceSynonyms( VMA_BUF( 1, MAX_MESSAGE_SIZE ), MAX_MESSAGE_SIZE, args[2] );
		return 0;
	case BOTLIB_AI_LOAD_CHAT_FILE:
		return botlib_export->ai.BotLoadChatFile( args[1], (char *)VMA_STR( 2 ), (char *)VMA_STR( 3 ) );
	case BOTLIB_AI_SET_CHAT_GENDER:
		botlib_export->ai.BotSetChatGender( args[1], args[2] );
		return 0;
	case BOTLIB_AI_SET_CHAT_NAME:
		// botlib keeps the client and later sends chat as it
		botlib_export->ai.BotSetChatName( args[1], (char *)VMA_STR( 2 ), VMV_CLIENT( 3 ) );
		return 0;

	case BOTLIB_AI_RESET_GOAL_STATE:
		botlib_export->ai.BotResetGoalState( args[1] );
		return 0;
	case BOTLIB_AI_RESET_AVOID_GOALS:
		botlib_export->ai.BotResetAvoidGoals( args[1] );
		return 0;
	case BOTLIB_AI_REMOVE_FROM_AVOID_GOALS:
		botlib_export->ai.BotRemoveFromAvoidGoals( args[1], args[2] );
		return 0;
	case BOTLIB_AI_PUSH_GOAL:
		botlib_export->ai.BotPushGoal( args[1], (bot_goal_t *)VMA_IN( 2, bot_goal_t ) );
		return 0;
	case BOTLIB_AI_POP_GOAL:
		botlib_export->ai.BotPopGoal( args[1] );
		return 0;
	case BOTLIB_AI_EMPTY_GOAL_STACK:
		botlib_export->ai.BotEmptyGoalStack( args[1] );
		return 0;
	case BOTLIB_AI_DUMP_AVOID_GOALS:
		botlib_export->ai.BotDumpAvoidGoals( args[1] );
		return 0;
	case BOTLIB_AI_DUMP_GOAL_STACK:
		botlib_export->ai.BotDumpGoalStack( args[1] );
		return 0;
	case BOTLIB_AI_GOAL_NAME:
		// it writes a terminator whatever the size
		botlib_export->ai.BotGoalName( args[1], VMA_STRBUF( 2, args[3] ), args[3] );
		return 0;
	case BOTLIB_AI_GET_TOP_GOAL:
		return botlib_export->ai.BotGetTopGoal( args[1], VMA_OUT( 2, bot_goal_t ) );
	case BOTLIB_AI_GET_SECOND_GOAL:
		return botlib_export->ai.BotGetSecondGoal( args[1], VMA_OUT( 2, bot_goal_t ) );
	case BOTLIB_AI_CHOOSE_LTG_ITEM:
		// weight files index the inventory, up to BOT_MAX_INVENTORY
		return botlib_export->ai.BotChooseLTGItem( args[1], VMA_VEC3( 2 ), VMA_ARRAY( 3, int, BOT_MAX_INVENTORY ), args[4] );
	case BOTLIB_AI_CHOOSE_NBG_ITEM:
		return botlib_export->ai.BotChooseNBGItem( args[1], VMA_VEC3( 2 ), VMA_ARRAY( 3, int, BOT_MAX_INVENTORY ), args[4],
			(bot_goal_t *)VMA_IN_OPT( 5, bot_goal_t ), VMF(6) );
	case BOTLIB_AI_TOUCHING_GOAL:
		return botlib_export->ai.BotTouchingGoal( VMA_VEC3( 1 ), (bot_goal_t *)VMA_IN( 2, bot_goal_t ) );
	case BOTLIB_AI_ITEM_GOAL_IN_VIS_BUT_NOT_VISIBLE:
		// the viewer is a trace's pass entity
		return botlib_export->ai.BotItemGoalInVisButNotVisible( VMV_ENTITY( 1 ), VMA_VEC3( 2 ), VMA_VEC3( 3 ),
			(bot_goal_t *)VMA_IN( 4, bot_goal_t ) );
	case BOTLIB_AI_GET_LEVEL_ITEM_GOAL:
		return botlib_export->ai.BotGetLevelItemGoal( args[1], (char *)VMA_STR( 2 ), VMA_OUT( 3, bot_goal_t ) );
	case BOTLIB_AI_GET_NEXT_CAMP_SPOT_GOAL:
		return botlib_export->ai.BotGetNextCampSpotGoal( args[1], VMA_OUT( 2, bot_goal_t ) );
	case BOTLIB_AI_GET_MAP_LOCATION_GOAL:
		return botlib_export->ai.BotGetMapLocationGoal( (char *)VMA_STR( 1 ), VMA_OUT( 2, bot_goal_t ) );
	case BOTLIB_AI_AVOID_GOAL_TIME:
		return FloatAsInt( botlib_export->ai.BotAvoidGoalTime( args[1], args[2] ) );
	case BOTLIB_AI_SET_AVOID_GOAL_TIME:
		botlib_export->ai.BotSetAvoidGoalTime( args[1], args[2], VMF(3));
		return 0;
	case BOTLIB_AI_INIT_LEVEL_ITEMS:
		botlib_export->ai.BotInitLevelItems();
		return 0;
	case BOTLIB_AI_UPDATE_ENTITY_ITEMS:
		botlib_export->ai.BotUpdateEntityItems();
		return 0;
	case BOTLIB_AI_LOAD_ITEM_WEIGHTS:
		return botlib_export->ai.BotLoadItemWeights( args[1], (char *)VMA_STR( 2 ) );
	case BOTLIB_AI_FREE_ITEM_WEIGHTS:
		botlib_export->ai.BotFreeItemWeights( args[1] );
		return 0;
	case BOTLIB_AI_INTERBREED_GOAL_FUZZY_LOGIC:
		botlib_export->ai.BotInterbreedGoalFuzzyLogic( args[1], args[2], args[3] );
		return 0;
	case BOTLIB_AI_SAVE_GOAL_FUZZY_LOGIC:
		botlib_export->ai.BotSaveGoalFuzzyLogic( args[1], (char *)VMA_STR( 2 ) );
		return 0;
	case BOTLIB_AI_MUTATE_GOAL_FUZZY_LOGIC:
		botlib_export->ai.BotMutateGoalFuzzyLogic( args[1], VMF(2) );
		return 0;
	case BOTLIB_AI_ALLOC_GOAL_STATE:
		// botlib keeps the client and later traces past it
		return botlib_export->ai.BotAllocGoalState( VMV_CLIENT( 1 ) );
	case BOTLIB_AI_FREE_GOAL_STATE:
		botlib_export->ai.BotFreeGoalState( args[1] );
		return 0;

	case BOTLIB_AI_RESET_MOVE_STATE:
		botlib_export->ai.BotResetMoveState( args[1] );
		return 0;
	case BOTLIB_AI_ADD_AVOID_SPOT:
		botlib_export->ai.BotAddAvoidSpot( args[1], VMA_VEC3( 2 ), VMF(3), args[4] );
		return 0;
	case BOTLIB_AI_MOVE_TO_GOAL:
		botlib_export->ai.BotMoveToGoal( VMA_OUT( 1, bot_moveresult_t ), args[2],
			(bot_goal_t *)VMA_IN_OPT( 3, bot_goal_t ), args[4] );
		return 0;
	case BOTLIB_AI_MOVE_IN_DIRECTION:
		return botlib_export->ai.BotMoveInDirection( args[1], VMA_VEC3( 2 ), VMF(3), args[4] );
	case BOTLIB_AI_RESET_AVOID_REACH:
		botlib_export->ai.BotResetAvoidReach( args[1] );
		return 0;
	case BOTLIB_AI_RESET_LAST_AVOID_REACH:
		botlib_export->ai.BotResetLastAvoidReach( args[1] );
		return 0;
	case BOTLIB_AI_REACHABILITY_AREA:
		// the client is a trace's pass entity
		return botlib_export->ai.BotReachabilityArea( VMA_VEC3( 1 ), VMV_ENTITY( 2 ) );
	case BOTLIB_AI_MOVEMENT_VIEW_TARGET:
		return botlib_export->ai.BotMovementViewTarget( args[1], (bot_goal_t *)VMA_IN_OPT( 2, bot_goal_t ), args[3],
			VMF(4), VMA_VEC3( 5 ) );
	case BOTLIB_AI_PREDICT_VISIBLE_POSITION:
		return botlib_export->ai.BotPredictVisiblePosition( VMA_VEC3( 1 ), args[2], (bot_goal_t *)VMA_IN( 3, bot_goal_t ),
			args[4], VMA_VEC3( 5 ) );
	case BOTLIB_AI_ALLOC_MOVE_STATE:
		return botlib_export->ai.BotAllocMoveState();
	case BOTLIB_AI_FREE_MOVE_STATE:
		botlib_export->ai.BotFreeMoveState( args[1] );
		return 0;
	case BOTLIB_AI_INIT_MOVE_STATE: {
		// botlib keeps the client and entity: the client indexes its inputs
		// and svs.clients, the entity is a trace's pass entity
		bot_initmove_t	*initmove = (bot_initmove_t *)VMA_IN( 2, bot_initmove_t );

		if ( initmove->client < 0 || initmove->client >= sv_maxclients->integer ) {
			VM_ArgError( args, 2, "client %i is outside 0 to %i", initmove->client, sv_maxclients->integer - 1 );
		}
		if ( initmove->entitynum < 0 || initmove->entitynum >= MAX_GENTITIES ) {
			VM_ArgError( args, 2, "entity %i is outside 0 to %i", initmove->entitynum, MAX_GENTITIES - 1 );
		}
		botlib_export->ai.BotInitMoveState( args[1], initmove );
		return 0;
	}

	case BOTLIB_AI_CHOOSE_BEST_FIGHT_WEAPON:
		// weight files index the inventory, up to BOT_MAX_INVENTORY
		return botlib_export->ai.BotChooseBestFightWeapon( args[1], VMA_ARRAY( 2, int, BOT_MAX_INVENTORY ) );
	case BOTLIB_AI_GET_WEAPON_INFO:
		botlib_export->ai.BotGetWeaponInfo( args[1], args[2], VMA_OUT( 3, weaponinfo_t ) );
		return 0;
	case BOTLIB_AI_LOAD_WEAPON_WEIGHTS:
		return botlib_export->ai.BotLoadWeaponWeights( args[1], (char *)VMA_STR( 2 ) );
	case BOTLIB_AI_ALLOC_WEAPON_STATE:
		return botlib_export->ai.BotAllocWeaponState();
	case BOTLIB_AI_FREE_WEAPON_STATE:
		botlib_export->ai.BotFreeWeaponState( args[1] );
		return 0;
	case BOTLIB_AI_RESET_WEAPON_STATE:
		botlib_export->ai.BotResetWeaponState( args[1] );
		return 0;

	case BOTLIB_AI_GENETIC_PARENTS_AND_CHILD_SELECTION: {
		// botlib copies the ranks into an array of 256
		int		numranks = VMV_COUNT( 1, 256 );

		return botlib_export->ai.GeneticParentsAndChildSelection( numranks, VMA_ARRAY( 2, float, numranks ),
			VMA_OUT( 3, int ), VMA_OUT( 4, int ), VMA_OUT( 5, int ) );
	}

	case TRAP_MEMSET:
		Com_Memset( VMA_BUF( 1, args[3] ), args[2], (int)args[3] );
		return 0;

	case TRAP_MEMCPY:
		Com_Memcpy( VMA_BUF( 1, args[3] ), VMA_BUF( 2, args[3] ), (int)args[3] );
		return 0;

	case TRAP_STRNCPY:
		return VM_Strncpy( args );

	case TRAP_SIN:
		return FloatAsInt( sin( VMF(1) ) );

	case TRAP_COS:
		return FloatAsInt( cos( VMF(1) ) );

	case TRAP_ATAN2:
		return FloatAsInt( atan2( VMF(1), VMF(2) ) );

	case TRAP_SQRT:
		return FloatAsInt( sqrt( VMF(1) ) );

	case TRAP_MATRIXMULTIPLY:
		MatrixMultiply( VMA_BUF( 1, sizeof( vec3_t[3] ) ), VMA_BUF( 2, sizeof( vec3_t[3] ) ),
			VMA_BUF( 3, sizeof( vec3_t[3] ) ) );
		return 0;

	case TRAP_ANGLEVECTORS:
		AngleVectors( VMA_VEC3( 1 ), VMA_VEC3_OPT( 2 ), VMA_VEC3_OPT( 3 ), VMA_VEC3_OPT( 4 ) );
		return 0;

	case TRAP_PERPENDICULARVECTOR:
		PerpendicularVector( VMA_VEC3( 1 ), VMA_VEC3( 2 ) );
		return 0;

	case TRAP_FLOOR:
		return FloatAsInt( floor( VMF(1) ) );

	case TRAP_CEIL:
		return FloatAsInt( ceil( VMF(1) ) );


	case COM_TRAP_GETVALUE:
		return VM_GetValue( args, sv_gameExtensions );

	case COM_TRAP_CVAR_SETDESCRIPTION:
		// NULL does nothing
		Cvar_SetDescriptionByName( VMA_STR_OPT( 1 ), VMA_STR_OPT( 2 ) );
		return 0;

	default:
		Com_Error( ERR_DROP, "Bad game system trap: %ld", (long int) args[0] );
	}
	return 0;
}

/*
===============
SV_ShutdownGameProgs

Called every time a map changes
===============
*/
void SV_ShutdownGameProgs( void ) {
	if ( !gvm ) {
		return;
	}
	VM_Call( gvm, GAME_SHUTDOWN, qfalse );
	if ( sv_gameBotLibSetUp ) {
		SV_BotLibShutdown();
		sv_gameBotLibSetUp = qfalse;
	}
	VM_Free( gvm );
	gvm = NULL;
	sv_numGameClients = 0;
}

/*
==================
SV_InitGameVM

Called for both a full init and a restart
==================
*/
static void SV_InitGameVM( qboolean restart ) {
	int		i;

	// VM_Restart may have made gvm afresh
	VM_SetSyscallNames( gvm, sv_gameSyscallNames, ARRAY_LEN( sv_gameSyscallNames ) );

	// start the entity parsing at the beginning
	sv.entityParsePoint = CM_EntityString();

	// clear all gentity pointers that might still be set from
	// a previous level
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=522
	//   now done before GAME_INIT call
	for ( i = 0 ; i < sv_maxclients->integer ; i++ ) {
		svs.clients[i].gentity = NULL;
	}
	
	// use the current msec count for a random seed
	// init for this gamestate
	VM_Call (gvm, GAME_INIT, sv.time, com_randomSeed->integer ? com_randomSeed->integer : Com_Milliseconds(), restart);
}



/*
===================
SV_RestartGameProgs

Called on a map_restart, but not on a normal map change
===================
*/
void SV_RestartGameProgs( void ) {
	if ( !gvm ) {
		return;
	}
	VM_Call( gvm, GAME_SHUTDOWN, qtrue );

	// do a restart instead of a free
	gvm = VM_Restart(gvm, qtrue);
	if ( !gvm ) {
		Com_Error( ERR_FATAL, "VM_Restart on game failed" );
	}

	// the restarted game links its entities afresh, and may have fewer:
	// one left linked from before would be found above its count
	SV_ClearWorld();

	SV_InitGameVM( qtrue );
}


/*
===============
SV_InitGameProgs

Called on a normal map change, not on a map_restart
===============
*/
void SV_InitGameProgs( void ) {
	cvar_t	*var;
	//FIXME these are temp while I make bots run in vm
	extern int	bot_enable;

	var = Cvar_Get( "bot_enable", "1", CVAR_LATCH );
	if ( var ) {
		bot_enable = var->integer;
	}
	else {
		bot_enable = 0;
	}

	// load the dll or bytecode
	gvm = VM_Create( "qagame", SV_GameSystemCalls, Cvar_VariableValue( "vm_game" ) );
	if ( !gvm ) {
		Com_Error( ERR_FATAL, "VM_Create on game failed" );
	}

	SV_InitGameVM( qfalse );
}


/*
====================
SV_GameCommand

See if the current console command is claimed by the game
====================
*/
qboolean SV_GameCommand( void ) {
	if ( sv.state != SS_GAME ) {
		return qfalse;
	}

	return VM_Call( gvm, GAME_CONSOLE_COMMAND );
}

