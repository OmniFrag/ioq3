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
// cl_ext.c -- the client's side of the engine extensions (qcommon/hooks.h):
// what they read and set, and the game modules' registrations and renderer
// calls, which they see and may take

#include "client.h"

// the renderer's handles are valid from rendererStarted until
// rendererStopping returns, which CL_ShutdownAll calls before it clears
// rendererStarted
const refexport_t *CL_ExtRenderer( void ) {
	return cls.rendererStarted ? &re : NULL;
}

const glconfig_t *CL_ExtGlconfig( void ) {
	return &cls.glconfig;
}

void CL_ExtSafeArea( int rect[4] ) {
	CL_GetSafeArea( rect );
}

int CL_ExtKeyCatcher( void ) {
	return Key_GetCatcher( );
}

// Com_Init started the renderer before the extensions, which get its
// rendererStarted now
void CL_ExtensionsStarted( void ) {
	if ( cls.rendererStarted ) {
		Hooks_RendererStarted( );
	}
}

// the platform reads it each frame (IN_Frame)
void CL_ExtSetPointerMode( qboolean cursor, qboolean drawn ) {
	cls.extCursor = cursor;
	cls.extCursorDrawn = drawn;
}

// no area is an empty one, which the platform doesn't pass on
void CL_ExtSetTextFocus( qboolean focused, const int rect[4] ) {
	cls.extTextFocus = focused;
	if ( focused && rect ) {
		Com_Memcpy( cls.extTextArea, rect, sizeof( cls.extTextArea ) );
	} else {
		Com_Memset( cls.extTextArea, 0, sizeof( cls.extTextArea ) );
	}
}

// CL_ClearState empties the gamestate as the client disconnects, so every
// offset is 0, an empty string
const char *CL_ExtConfigString( int index ) {
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		return "";
	}
	return cl.gameState.stringData + cl.gameState.stringOffsets[index];
}

int CL_ExtSnapshotEntities( entityState_t *out, int max ) {
	int	i, count;

	if ( clc.state != CA_ACTIVE || !cl.snap.valid || max <= 0 ) {
		return 0;
	}
	count = MIN( cl.snap.numEntities, max );
	for ( i = 0; i < count; i++ ) {
		out[i] = cl.parseEntities[( cl.snap.parseEntitiesNum + i ) & ( MAX_PARSE_ENTITIES - 1 )];
	}
	return count;
}

int CL_ExtModuleChecksum( extModule_t module ) {
	return VM_Checksum( module == EXT_MODULE_CGAME ? cgvm : uivm );
}

// the cvars, which both the server's download and the fast one keep
// (cl_parse.c, cl_content.c)
const char *CL_ExtDownloadState( int *count, int *size ) {
	const char	*name = Cvar_VariableString( "cl_downloadName" );

	*count = name[0] ? Cvar_VariableIntegerValue( "cl_downloadCount" ) : 0;
	*size = name[0] ? Cvar_VariableIntegerValue( "cl_downloadSize" ) : 0;
	return name;
}

const char *CL_ExtServerName( void ) {
	return clc.state >= CA_CONNECTING && clc.state != CA_CINEMATIC ? clc.servername : "";
}

// only until the server answers, as the ui's connect screen shows it: a
// print from the server later, an rcon reply among them, lands there too
const char *CL_ExtServerMessage( int *connectPackets ) {
	qboolean	connecting = clc.state >= CA_CONNECTING && clc.state < CA_CONNECTED;

	*connectPackets = connecting ? clc.connectPacketCount : 0;
	return connecting ? clc.serverMessage : "";
}

// a demo sends no packets, so CL_ParseSnapshot's ping is cls.realtime there
int CL_ExtPing( void ) {
	return clc.state == CA_ACTIVE && cl.snap.valid && !clc.demoplaying ? cl.snap.ping : 999;
}

int CL_ExtSnapshotNumber( int *serverTime ) {
	qboolean	valid = clc.state == CA_ACTIVE && cl.snap.valid;

	*serverTime = valid ? cl.snap.serverTime : 0;
	return valid ? cl.snap.messageNum : 0;
}

int CL_ExtFrameTime( void ) {
	return cls.realFrametime;
}

extChatMode_t CL_ExtChatLine( const char **text ) {
	if ( !( Key_GetCatcher( ) & KEYCATCH_MESSAGE ) ) {
		*text = "";
		return EXT_CHAT_NONE;
	}
	*text = chatField.buffer;
	return chat_playerNum != -1 ? EXT_CHAT_TELL : chat_team ? EXT_CHAT_TEAM : EXT_CHAT_ALL;
}

float CL_ExtVoipPower( void ) {
#ifdef USE_VOIP
	return clc.voipPower;
#else
	return 0;
#endif
}

const char *CL_ExtDemoRecording( int *bytes ) {
	*bytes = clc.demorecording && clc.demofile ? FS_FTell( clc.demofile ) : 0;
	return clc.demorecording ? clc.demoName : "";
}

const char *CL_ExtDownloadFailures( int *count ) {
	*count = clc.downloadFailed;
	return clc.downloadFailures;
}

/*
==================
CL_ModuleRegister, CL_ModuleRegisterFont

A game module's registration, which the engine extensions see (hooks.h's
moduleAsset)
==================
*/
qhandle_t CL_ModuleRegister( extModule_t module, extAssetKind_t kind,
	qhandle_t (*registerAsset)( const char *name ), const char *name ) {
	qhandle_t	handle = registerAsset( name );

	Hooks_ModuleAsset( module, kind, name, handle, NULL );
	return handle;
}

void CL_ModuleRegisterFont( extModule_t module, const char *name, int pointSize, fontInfo_t *font ) {
	re.RegisterFont( name, pointSize, font );
	Hooks_ModuleAsset( module, EXT_ASSET_FONT, name, 0, font );
}

// a module's own 2D state, its clip region's (cl_scrn.c)
module2D_t *CL_Module2D( extModule_t module ) {
	return module == EXT_MODULE_CGAME ? &cls.cgame2D : &cls.ui2D;
}

/*
==================
CL_ModuleSetColor, CL_ModuleDrawStretchPic, CL_ModuleClearScene,
CL_ModuleAddRefEntity, CL_ModuleAddPolys, CL_ModuleAddLight,
CL_ModuleRenderScene

A game module's renderer calls, each passed on unless an engine
extension takes it (hooks.h's moduleRender): colors and pictures through
the module's clip region (cl_scrn.c), the rest to the renderer. The call
is filled in only while one wants them
==================
*/
void CL_ModuleSetColor( extModule_t module, const float *color ) {
	if ( Hooks_WantModuleRender( ) ) {
		extRenderCall_t	call = { EXT_RENDER_SETCOLOR };

		call.color = color;
		if ( Hooks_ModuleRender( module, &call ) ) {
			return;
		}
	}
	SCR_ModuleSetColor( CL_Module2D( module ), color );
}

void CL_ModuleDrawStretchPic( extModule_t module, float x, float y, float w, float h, float s1, float t1,
	float s2, float t2, qhandle_t shader ) {
	module2D_t	*m = CL_Module2D( module );

	if ( Hooks_WantModuleRender( ) ) {
		extRenderCall_t	call = { EXT_RENDER_STRETCHPIC };

		call.x = x;
		call.y = y;
		call.w = w;
		call.h = h;
		call.s1 = s1;
		call.t1 = t1;
		call.s2 = s2;
		call.t2 = t2;
		call.shader = shader;
		call.clip = m->clipped ? m->clip : NULL;
		if ( Hooks_ModuleRender( module, &call ) ) {
			return;
		}
	}
	SCR_ModuleStretchPic( m, x, y, w, h, s1, t1, s2, t2, shader );
}

void CL_ModuleClearScene( extModule_t module ) {
	if ( Hooks_WantModuleRender( ) ) {
		extRenderCall_t	call = { EXT_RENDER_CLEARSCENE };

		if ( Hooks_ModuleRender( module, &call ) ) {
			return;
		}
	}
	re.ClearScene( );
}

void CL_ModuleAddRefEntity( extModule_t module, const refEntity_t *entity ) {
	if ( Hooks_WantModuleRender( ) ) {
		extRenderCall_t	call = { EXT_RENDER_REFENTITY };

		call.entity = entity;
		if ( Hooks_ModuleRender( module, &call ) ) {
			return;
		}
	}
	re.AddRefEntityToScene( entity );
}

void CL_ModuleAddPolys( extModule_t module, qhandle_t shader, int numVerts, const polyVert_t *verts, int numPolys ) {
	if ( Hooks_WantModuleRender( ) ) {
		extRenderCall_t	call = { EXT_RENDER_POLYS };

		call.shader = shader;
		call.verts = verts;
		call.numVerts = numVerts;
		call.numPolys = numPolys;
		if ( Hooks_ModuleRender( module, &call ) ) {
			return;
		}
	}
	re.AddPolyToScene( shader, numVerts, verts, numPolys );
}

void CL_ModuleAddLight( extModule_t module, const vec3_t origin, float intensity, float r, float g, float b,
	qboolean additive ) {
	if ( Hooks_WantModuleRender( ) ) {
		extRenderCall_t	call = { EXT_RENDER_LIGHT };

		call.origin = origin;
		call.intensity = intensity;
		call.r = r;
		call.g = g;
		call.b = b;
		call.additive = additive;
		if ( Hooks_ModuleRender( module, &call ) ) {
			return;
		}
	}
	if ( additive ) {
		re.AddAdditiveLightToScene( origin, intensity, r, g, b );
	} else {
		re.AddLightToScene( origin, intensity, r, g, b );
	}
}

void CL_ModuleRenderScene( extModule_t module, const refdef_t *refdef ) {
	if ( Hooks_WantModuleRender( ) ) {
		extRenderCall_t	call = { EXT_RENDER_SCENE };

		call.refdef = refdef;
		if ( Hooks_ModuleRender( module, &call ) ) {
			return;
		}
	}
	re.RenderScene( refdef );
}
