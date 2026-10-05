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
// cl_present.c -- fullscreen: what the player wants against what the
// platform reports
//
// r_fullscreen is what the player wants, and only the player writes it,
// saved. com_fullscreen is what the platform reports, read only. Each
// frame the platform (sdl_input.c) gives its facts, and this compares them
// with the want: it asks the platform once, waits for the answer, and when
// the platform refuses or doesn't answer keeps the want and says why,
// asking again only at the platform's next chance. The renderer makes the
// window, and asks here whether to make it fullscreen.
//
// A change the platform makes by its own means while nothing is asked (the
// window manager's, the browser's Esc or F11) is the player's too, for this
// run: it becomes r_fullscreen's value from the command line's layer, which
// isn't saved. What the game's own controls set (the menu, Alt+Enter, the
// console) is saved.
//
// The pointer is the game's to want: held in a match and the menus, once
// the player has entered the window, and read but for the console and
// loading in a window. in_captured is whether the platform holds it.

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "cl_present.h"

// how long the system has to act on a request before it's taken as
// refused: macOS animates into fullscreen in under a second
#define PRESENT_ANSWER_MSEC		3000

// how long a window has the focus before the last resort, a new window, is
// tried
#define PRESENT_FOCUS_MSEC		1000

static cvar_t	*r_fullscreen;
static cvar_t	*in_nograb;
static cvar_t	*com_fullscreen;
static cvar_t	*com_fullscreenAvailable;
static cvar_t	*in_captured;

static struct {
	int				time;			// the last frame's
	qboolean		asking;			// a request is out, its answer not in
	qboolean		askedFor;
	int				askTime;
	qboolean		newWindow;		// asked by making a window, timed from its first frame
	presentReason_t	reason;			// why the window isn't what's wanted
	qboolean		reasonFor;		// the want it's about
	qboolean		recreated;		// the last resort was taken for this want
	int				focusTime;		// since when the window has had the focus, or 0
} present;

static const char *presentReasons[] = {
	"",
	"this device can't go fullscreen",
	"in_nograb is on",
	"the system didn't go fullscreen",
	"it starts with a click",
	"the browser refused it",
	"it's the browser's own fullscreen, which only the browser leaves (F11, or Control+Command+F on a Mac)"
};

/*
===============
Present_Init
===============
*/
void Present_Init( void )
{
	Com_Memset( &present, 0, sizeof( present ) );

	r_fullscreen = Cvar_Get( "r_fullscreen", "1", CVAR_ARCHIVE );
	in_nograb = Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );

	com_fullscreen = Cvar_Get( "com_fullscreen", "0", CVAR_ROM );
	Cvar_SetDescription( com_fullscreen, "Whether the game is fullscreen now; r_fullscreen is whether the player wants it" );
	com_fullscreenAvailable = Cvar_Get( "com_fullscreenAvailable", "1", CVAR_ROM );
	Cvar_SetDescription( com_fullscreenAvailable, "Whether this device can go fullscreen" );
	in_captured = Cvar_Get( "in_captured", "0", CVAR_ROM );
	Cvar_SetDescription( in_captured, "Whether the game has the pointer now" );
}

/*
===============
Present_CreateFullscreen

The renderer's question as it makes a window: fullscreen if that's wanted
and in_nograb allows it. The new window is a request of its own, whose
answer is the platform's fact. Never on the web, where the page's
fullscreen, which a browser grants only on the player's input, outlives a
new canvas window, so nothing is asked.
===============
*/
qboolean Present_CreateFullscreen( void )
{
#ifdef __EMSCRIPTEN__
	return qfalse;
#else
	present.asking = qtrue;
	present.askedFor = r_fullscreen->integer && !in_nograb->integer;
	present.newWindow = qtrue;
	return present.askedFor;
#endif
}

/*
===============
Present_SetReason
===============
*/
static void Present_SetReason( presentReason_t reason, qboolean want )
{
	if( reason == present.reason )
	{
		return;
	}

	present.reason = reason;
	present.reasonFor = want;

	// a browser waiting for the player's click is how it starts, not news
	if( reason != PRESENT_OK && reason != PRESENT_NEEDS_CLICK )
	{
		Com_Printf( "Not %s: %s\n", want ? "fullscreen" : "in a window", presentReasons[reason] );
	}
}

/*
===============
Present_Ask

A window that can't change in place is made again, as the player wants it
===============
*/
static void Present_Ask( qboolean want, int time )
{
	present.asking = qtrue;
	present.askedFor = want;
	present.askTime = time;
	if( !IN_RequestFullscreen( want ) )
	{
		IN_RecreateWindow( );
	}
}

/*
===============
Present_Frame
===============
*/
void Present_Frame( const presentFacts_t *facts, int time )
{
	qboolean	want;

	present.time = time;

	// the player's own request, which the platform made at their key: the
	// want, saved, and a request out, unless the device has no fullscreen,
	// whose reason follows
	if( facts->playerAsked )
	{
		Cvar_SetFrom( "r_fullscreen", facts->playerWants ? "1" : "0", CVAR_SOURCE_PLAYER, qtrue );
		present.recreated = qfalse;
		if( !facts->playerWants || facts->available )
		{
			present.asking = qtrue;
			present.askedFor = facts->playerWants;
			present.askTime = time;
		}
	}
	want = r_fullscreen->integer != 0;

	// making a window, and loading after it, isn't the system's time to
	// answer: that starts with the window's first frame
	if( present.newWindow )
	{
		present.newWindow = qfalse;
		present.askTime = time;
	}

	if( com_fullscreenAvailable->integer != facts->available )
	{
		Cvar_SetValue( "com_fullscreenAvailable", facts->available );
	}

	if( present.asking && facts->fullscreen == present.askedFor )
	{
		// the answer
		present.asking = qfalse;
	}
	else if( !present.asking && facts->fullscreen != com_fullscreen->integer && facts->fullscreen != want )
	{
		// the platform's own: the player's choice, for this run
		Cvar_SetFrom( "r_fullscreen", facts->fullscreen ? "1" : "0", CVAR_SOURCE_SESSION, qtrue );
		want = facts->fullscreen;
	}

	if( com_fullscreen->integer != facts->fullscreen )
	{
		Cvar_SetValue( "com_fullscreen", facts->fullscreen );
	}

	if( !facts->focused )
	{
		present.focusTime = 0;
	}
	else if( !present.focusTime )
	{
		present.focusTime = time;
	}

	// a new want, or what was in the way of the last gone
	if( facts->fullscreen == want || want != present.reasonFor ||
		( present.reason == PRESENT_NOGRAB && !in_nograb->integer ) ||
		( present.reason == PRESENT_UNAVAILABLE && facts->available ) )
	{
		Present_SetReason( PRESENT_OK, want );
		present.recreated = qfalse;
	}

	if( present.asking )
	{
		if( want != present.askedFor && ( !want || ( facts->available && !in_nograb->integer ) ) )
		{
			// wanted otherwise since: ask for that instead, unless something
			// is in the way, which shows once this answer is in
			Present_Ask( want, time );
		}
		else if( facts->standing )
		{
			// the platform says why it's waiting
			Present_SetReason( facts->answer, want );
		}
		else if( time - present.askTime > PRESENT_ANSWER_MSEC )
		{
			present.asking = qfalse;
			Present_SetReason( PRESENT_NO_ANSWER, want );
		}
		return;
	}

	if( facts->fullscreen == want )
	{
		return;
	}

	if( present.reason == PRESENT_NO_ANSWER )
	{
		// where asking again does nothing, a new window, once it has had the
		// focus a while
		if( facts->recreateWhenStuck && !present.recreated && present.focusTime &&
			time - present.focusTime >= PRESENT_FOCUS_MSEC && !facts->loading )
		{
			present.recreated = qtrue;
			IN_RecreateWindow( );
		}
		return;
	}

	if( present.reason != PRESENT_OK )
	{
		return;
	}

	if( want && !facts->available )
	{
		Present_SetReason( PRESENT_UNAVAILABLE, want );
	}
	else if( want && in_nograb->integer )
	{
		Present_SetReason( PRESENT_NOGRAB, want );
	}
	else
	{
		Present_Ask( want, time );
	}
}

/*
===============
Present_Pointer

What the game does with the pointer this frame. It holds it with the
mouse on, unless in_nograb in a window, where it makes sense, or a touch
screen, which has none to hold; and reads it
once the player has entered the window, which on the web is the page's
lock (or the game not wanting it), but for the console and loading in a
window, which leave it free natively. The web's lock stays through them:
Safari shows its bar while the pointer is free, resizing the game.
===============
*/
presentPointer_t Present_Pointer( const presentFacts_t *facts )
{
	presentPointer_t	pointer;
	qboolean			entered;

	pointer.grab = facts->mouse && facts->holdable && ( facts->fullscreen || !in_nograb->integer );
	entered = facts->capturesAtClick ? facts->captured || !pointer.grab : facts->clickedIn;
	pointer.read = facts->mouse && facts->focused && entered &&
		( facts->fullscreen || !( facts->console || facts->loading ) );
	return pointer;
}

/*
===============
Present_Captured

Whether the platform holds the pointer, as it reports after this frame's
change. A release it kept while it held it, a button's, or Esc's when a
browser frees the lock, never comes: losing it lets go of every held key.
===============
*/
void Present_Captured( qboolean captured )
{
	if( in_captured->integer == captured )
	{
		return;
	}

	if( !captured )
	{
		Key_ClearStates( );
	}
	Cvar_SetValue( "in_captured", captured );
}

/*
===============
Present_ToggleFullscreen

Alt+Enter: the other of what's on screen, saved, and asked for again even
if it's already the want, as a refusal's next chance
===============
*/
void Present_ToggleFullscreen( void )
{
	Cvar_SetFrom( "r_fullscreen", com_fullscreen->integer ? "0" : "1", CVAR_SOURCE_PLAYER, qtrue );

	present.asking = qfalse;
	present.reason = PRESENT_OK;
	present.recreated = qfalse;
}

/*
===============
Present_Reason

Why the window isn't what the player wants, or "" when it is
===============
*/
const char *Present_Reason( void )
{
	return presentReasons[present.reason];
}
