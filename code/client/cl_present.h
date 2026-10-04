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
// cl_present.h -- how the game presents itself: fullscreen and the
// pointer, what the player and the game want against what the platform
// reports

#ifndef CL_PRESENT_H
#define CL_PRESENT_H

#include "../qcommon/q_shared.h"

// why the window isn't what the player wants
typedef enum {
	PRESENT_OK,
	PRESENT_UNAVAILABLE,	// this device has no fullscreen (iPhone, iPad)
	PRESENT_NOGRAB,			// in_nograb is on
	PRESENT_NO_ANSWER,		// the system didn't act on the request
	PRESENT_NEEDS_CLICK,	// a browser acts only on the player's input
	PRESENT_REFUSED			// the browser refused
} presentReason_t;

// what the platform reports, each frame
typedef struct {
	qboolean		fullscreen;		// the window, or the page, is fullscreen
	qboolean		available;		// the device can go fullscreen
	qboolean		focused;
	qboolean		loading;		// a map or demo loads
	// a request stays with the platform until it can act on it, which it
	// says here meanwhile, and is done once met (the web, which waits for
	// the player's input)
	qboolean		standing;
	presentReason_t	answer;
	// a window that didn't go fullscreen stays out, however it's asked
	// again, and only a new window goes (macOS, for a window that lost the
	// focus on its way in)
	qboolean		recreateWhenStuck;

	// the pointer's
	qboolean		mouse;			// in_mouse is on
	qboolean		holdable;		// there's a pointer to hold: none on a touch screen
	qboolean		console;		// the console is down
	// natively, the player clicked into the window since it got the focus,
	// or was fullscreen since
	qboolean		clickedIn;
	// the platform holds the pointer for the game, which on the web the
	// page takes at the player's click, and that click is the entry
	qboolean		captured;
	qboolean		capturesAtClick;
} presentFacts_t;

// what the game does with the pointer
typedef struct {
	qboolean		grab;			// hold it: relative mode, or the page's lock
	qboolean		read;			// take its motion and buttons
} presentPointer_t;

void		Present_Init( void );
qboolean	Present_CreateFullscreen( void );
void		Present_Frame( const presentFacts_t *facts, int time );
void		Present_ToggleFullscreen( void );
const char	*Present_Reason( void );
presentPointer_t	Present_Pointer( const presentFacts_t *facts );
void		Present_Captured( qboolean captured );

// the platform's (sdl_input.c), and a test's stubs: ask for fullscreen or
// to leave it, qfalse if the window can't change in place; and a new
// window. And the client's keys' (cl_keys.c): release every one held
qboolean	IN_RequestFullscreen( qboolean fullscreen );
void		IN_RecreateWindow( void );
void		Key_ClearStates( void );

#endif
