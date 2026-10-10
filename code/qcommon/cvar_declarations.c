/*
===========================================================================
Copyright (C) 2026 the ioquake3 contributors

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
// cvar_declarations.c -- the game's cvar declarations, where it has none:
// an undeclared cvar is saved by who made it (Cvar_Scope). A game gives
// its own tables through CVAR_DECLARATIONS_SOURCE

#include "q_shared.h"
#include "qcommon.h"

const cvarDeclaration_t cvar_declarations[] = {
	{ NULL, CVAR_SCOPE_NONE, qfalse }
};

// and no other program's defaults, so an imported config's values are all
// taken as chosen
const cvarDefault_t cvar_q3Defaults[] = { { NULL, NULL } };
const cvarDefault_t cvar_ioq3Defaults[] = { { NULL, NULL } };
const cvarDefault_t cvar_cnq3Defaults[] = { { NULL, NULL } };
const cvarDefault_t cvar_q3eDefaults[] = { { NULL, NULL } };
const char * const cvar_ioq3Marks[] = { NULL };
const char * const cvar_cnq3Marks[] = { NULL };
const char * const cvar_q3eMarks[] = { NULL };
// and no other engine's cvars
const cvarForeign_t cvar_foreignNames[] = { { NULL, NULL } };
