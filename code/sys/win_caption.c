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
// win_caption.c -- a "Go full screen" button in the window's title bar,
// just left of minimize
//
// The title bar stays the system's own: its text and icon, the caption
// buttons, Snap layouts, dragging and the window menu. The button is a
// small window of its own, owned by the game's, over the title bar where
// a fourth caption button would be. An owned window stays above its owner
// and hides while it's minimized; the game's window is subclassed to move
// it along, in the same call that moves the game's, so the two move
// together. It's layered, drawn with per-pixel alpha: the title bar's
// color, which on Windows 11 follows the wallpaper, shows through it.
//
// It looks and acts like the caption buttons beside it: their size and
// place (WM_GETTITLEBARINFOEX), their hover and pressed fills, their
// glyphs' weight at each scale, and their tooltips. No API gives the
// fills, or the glyph's dimming while the window is inactive, so they're
// as Windows draws them beside the button. A press acts on release over
// the button, and screen readers see a button of that name.
//
// The glyph is the "Desktop" icon of Microsoft's Fluent UI System Icons
// (ic_fluent_desktop_16_regular, MIT License, Copyright (c) 2020
// Microsoft Corporation, github.com/microsoft/fluentui-system-icons),
// redrawn as strokes, so its lines can be as thick as the caption glyphs'
// at each scale, rather than scaled with the icon.

#include "../qcommon/q_shared.h"
#include "sys_local.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <uxtheme.h>

// DWMWINDOWATTRIBUTE values that older SDKs lack
#define CAPTION_DWMWA_DARK_MODE_OLD		19	// Windows 10 1809 to 1909
#define CAPTION_DWMWA_DARK_MODE			20
#define CAPTION_DWMWA_BORDER_THICKNESS	37	// Windows 11

#define CAPTION_SUBCLASS_ID				1
#define CAPTION_CLASS					L"FullscreenCaptionButton"
#define CAPTION_TOOLTIP_CLASS			L"FullscreenCaptionTooltip"
#define CAPTION_TEXT					L"Go full screen"
// while a change of scale settles, the button is placed again this often,
// for up to a second
#define CAPTION_SETTLE_TIMER			1
#define CAPTION_SETTLE_MSEC				30
#define CAPTION_SETTLE_LIMIT			1000
// the border of DWM's tooltips for the caption buttons
#define CAPTION_TOOLTIP_BORDER			RGB( 100, 100, 100 )

// The fills over the title bar's color, as alphas of the glyph's color
// (white on a dark title bar, black on a light one), and the glyph's alpha
// while the window is inactive. Windows 11's are measured beside its
// minimize button at 100% to 150%; Windows 10 fills its buttons with
// flat grays, 10% and 20%.
typedef struct
{
	BYTE	hover[2];		// light, dark
	BYTE	pressed[2];
	BYTE	inactive;
	int		gap;			// pixels left of each button's fill
} captionLook_t;

static const captionLook_t windows11 = { { 0x0a, 0x0f }, { 0x06, 0x0b }, 0x66, 1 };
static const captionLook_t windows10 = { { 0x1a, 0x1a }, { 0x33, 0x33 }, 0x66, 0 };

// Windows 10 1607 and later
static UINT ( WINAPI *getDpiForWindow )( HWND );
static BOOL ( WINAPI *parametersForDpi )( UINT, UINT, void *, UINT, UINT );
static const captionLook_t *look;

static struct
{
	HWND		owner;
	HWND		button;
	HWND		tooltip;
	void		( *pressed )( void );

	RECT		place;		// the button's, on the screen
	int			dpi;
	ULONGLONG	settleUntil;	// GetTickCount64 till when a change of scale may be settling
	qboolean	dark;
	qboolean	active;
	qboolean	hot;		// the pointer is over it
	qboolean	down;		// pressed, and the pointer still over it
	qboolean	tracking;	// TrackMouseEvent's leave is asked for

	HFONT		font;		// the tooltip's, at this dpi

	// the glyph's coverage, 0 to 255, at this dpi
	BYTE		*glyph;
	int			glyphWidth, glyphHeight;
} caption;

/*
==============
Caption_Dpi
==============
*/
static int Caption_Dpi( void )
{
	HDC dc;
	int dpi;

	if( getDpiForWindow )
	{
		return getDpiForWindow( caption.owner );
	}

	dc = GetDC( caption.owner );
	dpi = GetDeviceCaps( dc, LOGPIXELSY );
	ReleaseDC( caption.owner, dc );
	return dpi;
}

/*
==============
Caption_IsWindows11
==============
*/
static qboolean Caption_IsWindows11( void )
{
	LONG ( WINAPI *rtlGetVersion )( OSVERSIONINFOW * );
	OSVERSIONINFOW version;

	memset( &version, 0, sizeof( version ) );
	version.dwOSVersionInfoSize = sizeof( version );
	rtlGetVersion = (LONG ( WINAPI * )( OSVERSIONINFOW * ))(void *)GetProcAddress(
		GetModuleHandleW( L"ntdll.dll" ), "RtlGetVersion" );
	return rtlGetVersion && rtlGetVersion( &version ) == 0 &&
		version.dwMajorVersion >= 10 && version.dwBuildNumber >= 22000;
}

/*
==============
Caption_IsDark

Whether DWM draws the title bar dark, as the game's window asked for
(SDL follows the system's theme). A Windows 10 without dark title bars
fails both, and draws them light.
==============
*/
static qboolean Caption_IsDark( void )
{
	BOOL dark = FALSE;

	if( FAILED( DwmGetWindowAttribute( caption.owner, CAPTION_DWMWA_DARK_MODE, &dark, sizeof( dark ) ) ) &&
		FAILED( DwmGetWindowAttribute( caption.owner, CAPTION_DWMWA_DARK_MODE_OLD, &dark, sizeof( dark ) ) ) )
	{
		return qfalse;
	}
	return dark != FALSE;
}

/*
==============
Caption_InRoundedRect

Whether a point is in a rectangle with rounded corners
==============
*/
static qboolean Caption_InRoundedRect( float x, float y, float left, float top, float right, float bottom, float radius )
{
	float cx, cy;

	if( x < left || x >= right || y < top || y >= bottom )
	{
		return qfalse;
	}
	cx = Com_Clamp( left + radius, right - radius, x );
	cy = Com_Clamp( top + radius, bottom - radius, y );
	return ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy ) <= radius * radius;
}

/*
==============
Caption_MakeGlyph

The glyph's coverage at the window's scale, sampled 16 times each way in
each pixel. Its sizes are whole pixels, so its straight lines are crisp,
and they're as wide as the minimize glyph's: 1 pixel from 100% to 200%.
In Fluent's 16 units: a screen 12 by 10 with corners of 2, a stand of
two posts 4 apart under it, 1 high, and a base 8 wide with round ends.
==============
*/
#define GLYPH_SAMPLES 16

static void Caption_MakeGlyph( void )
{
	float scale = caption.dpi / 96.0f;
	int stroke = MAX( 1, (int)( scale / 2 + 0.25f ) );
	int width = (int)( 12 * scale + 0.5f );
	int screen = (int)( 10 * scale + 0.5f );
	int neck = MAX( 1, (int)( scale + 0.5f ) );
	int posts = (int)( width / 3.0f + 0.5f );
	int base = (int)( width * 2 / 3.0f + 0.5f );
	float radius = 2 * scale;
	int x, y, sx, sy;

	// centered in whole pixels
	posts += ( width - posts ) & 1;
	base += ( width - base ) & 1;

	free( caption.glyph );
	caption.glyphWidth = width;
	caption.glyphHeight = screen + neck + stroke;
	caption.glyph = calloc( caption.glyphWidth * caption.glyphHeight, 1 );
	if( !caption.glyph )
	{
		return;
	}

	for( y = 0; y < caption.glyphHeight; y++ )
	{
		for( x = 0; x < width; x++ )
		{
			int inside = 0;

			for( sy = 0; sy < GLYPH_SAMPLES; sy++ )
			{
				for( sx = 0; sx < GLYPH_SAMPLES; sx++ )
				{
					float px = x + ( sx + 0.5f ) / GLYPH_SAMPLES;
					float py = y + ( sy + 0.5f ) / GLYPH_SAMPLES;
					qboolean frame = Caption_InRoundedRect( px, py, 0, 0, width, screen, radius ) &&
						!Caption_InRoundedRect( px, py, stroke, stroke, width - stroke, screen - stroke, MAX( radius - stroke, 0 ) );
					qboolean post = py >= screen && py < screen + neck &&
						( ( px >= ( width - posts ) / 2 && px < ( width - posts ) / 2 + stroke ) ||
						( px >= ( width + posts ) / 2 - stroke && px < ( width + posts ) / 2 ) );
					qboolean foot = Caption_InRoundedRect( px, py, ( width - base ) / 2, screen + neck,
						( width + base ) / 2, screen + neck + stroke, stroke / 2.0f );

					if( frame || post || foot )
					{
						inside++;
					}
				}
			}
			caption.glyph[y * width + x] = (BYTE)( inside * 255 / ( GLYPH_SAMPLES * GLYPH_SAMPLES ) );
		}
	}
}

/*
==============
Caption_Draw

Draws the button in its state, premultiplied, for UpdateLayeredWindow.
At rest its fill is all but clear, one 255th of the color the title
bar's is furthest from, less than a shade of difference: wholly clear
pixels would let the pointer through to the title bar beneath.
==============
*/
static void Caption_Draw( void )
{
	int width = caption.place.right - caption.place.left;
	int height = caption.place.bottom - caption.place.top;
	BYTE ink = caption.dark ? 255 : 0;
	BYTE fillColor, fillAlpha, glyphAlpha;
	BITMAPINFO info;
	BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
	POINT origin = { 0, 0 }, place = { caption.place.left, caption.place.top };
	SIZE size = { width, height };
	HDC screen, dc;
	HBITMAP bitmap, old;
	BYTE *bits;
	int x, y, glyphX, glyphY;

	if( width <= 0 || height <= 0 )
	{
		return;
	}

	if( caption.down )
	{
		fillColor = ink;
		fillAlpha = look->pressed[caption.dark];
	}
	else if( caption.hot && !GetCapture( ) )
	{
		fillColor = ink;
		fillAlpha = look->hover[caption.dark];
	}
	else
	{
		fillColor = 255 - ink;
		fillAlpha = 1;
	}
	glyphAlpha = caption.active || caption.hot ? 255 : look->inactive;
	glyphX = ( width - caption.glyphWidth ) / 2;
	glyphY = ( height - caption.glyphHeight ) / 2;

	memset( &info, 0, sizeof( info ) );
	info.bmiHeader.biSize = sizeof( info.bmiHeader );
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	screen = GetDC( NULL );
	dc = CreateCompatibleDC( screen );
	bitmap = CreateDIBSection( screen, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );
	if( !bitmap )
	{
		DeleteDC( dc );
		ReleaseDC( NULL, screen );
		return;
	}

	for( y = 0; y < height; y++ )
	{
		for( x = 0; x < width; x++ )
		{
			BYTE *pixel = bits + ( y * width + x ) * 4;
			int gx = x - glyphX, gy = y - glyphY;
			int cover = 0, alpha, color;

			if( caption.glyph && gx >= 0 && gx < caption.glyphWidth && gy >= 0 && gy < caption.glyphHeight )
			{
				cover = caption.glyph[gy * caption.glyphWidth + gx] * glyphAlpha / 255;
			}

			// the glyph over the fill
			alpha = cover * 255 + fillAlpha * ( 255 - cover );
			color = ink * cover * 255 + fillColor * fillAlpha * ( 255 - cover );
			pixel[0] = pixel[1] = pixel[2] = (BYTE)( ( color + 255 * 255 / 2 ) / ( 255 * 255 ) );
			pixel[3] = (BYTE)( ( alpha + 127 ) / 255 );
		}
	}

	old = SelectObject( dc, bitmap );
	UpdateLayeredWindow( caption.button, screen, &place, &size, dc, &origin, 0, &blend, ULW_ALPHA );
	SelectObject( dc, old );
	DeleteObject( bitmap );
	DeleteDC( dc );
	ReleaseDC( NULL, screen );
}

/*
==============
Caption_SetTooltipFont

The tooltip's font at the window's scale, as the caption's tooltips have:
on its own, a tooltip has the font of the system's scale
==============
*/
static void Caption_SetTooltipFont( void )
{
	NONCLIENTMETRICSW metrics;
	HFONT font;

	if( !caption.tooltip || !parametersForDpi )
	{
		return;
	}

	memset( &metrics, 0, sizeof( metrics ) );
	metrics.cbSize = sizeof( metrics );
	if( !parametersForDpi( SPI_GETNONCLIENTMETRICS, sizeof( metrics ), &metrics, 0, caption.dpi ) ||
		!( font = CreateFontIndirectW( &metrics.lfStatusFont ) ) )
	{
		return;
	}
	SendMessageW( caption.tooltip, WM_SETFONT, (WPARAM)font, FALSE );
	if( caption.font )
	{
		DeleteObject( caption.font );
	}
	caption.font = font;
}

/*
==============
Caption_Place

Where the button goes on the screen: where a caption button left of
minimize would be, by the maximize button's place and size. FALSE
when the window has no title bar or caption buttons, as in fullscreen.
==============
*/
static qboolean Caption_Place( RECT *place )
{
	TITLEBARINFOEX info;
	RECT window, bounds;
	UINT border;
	LONG style = GetWindowLongW( caption.owner, GWL_STYLE );
	int width;

	if( !IsWindowVisible( caption.owner ) || IsIconic( caption.owner ) ||
		( style & WS_CAPTION ) != WS_CAPTION || !( style & ( WS_MINIMIZEBOX | WS_MAXIMIZEBOX ) ) )
	{
		return qfalse;
	}

	memset( &info, 0, sizeof( info ) );
	info.cbSize = sizeof( info );
	SendMessageW( caption.owner, WM_GETTITLEBARINFOEX, 0, (LPARAM)&info );
	if( ( info.rgstate[3] & ( STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN ) ) ||
		IsRectEmpty( &info.rgrect[3] ) || !GetWindowRect( caption.owner, &window ) )
	{
		return qfalse;
	}

	// the maximize button's place, two buttons left
	width = info.rgrect[3].right - info.rgrect[3].left;
	*place = info.rgrect[3];
	OffsetRect( place, -2 * width, 0 );
	place->left += look->gap;

	// Windows 11 fills the buttons below the window's top border, and
	// to the bottom of their bounds, a pixel short of the rectangles
	if( SUCCEEDED( DwmGetWindowAttribute( caption.owner, CAPTION_DWMWA_BORDER_THICKNESS, &border, sizeof( border ) ) ) )
	{
		place->top = MAX( place->top, window.top + (LONG)border );
	}
	if( SUCCEEDED( DwmGetWindowAttribute( caption.owner, DWMWA_CAPTION_BUTTON_BOUNDS, &bounds, sizeof( bounds ) ) ) &&
		!IsRectEmpty( &bounds ) )
	{
		place->bottom = MIN( place->bottom, window.top + bounds.bottom );

		// For a moment after the window changes scale, the rectangles are
		// those of the old scale while DWM's bounds are already the new
		// one's. Until they agree on the close button's right edge, the
		// button waits, hidden.
		if( info.rgrect[5].right != window.left + bounds.right &&
			GetTickCount64( ) < caption.settleUntil )
		{
			SetTimer( caption.button, CAPTION_SETTLE_TIMER, CAPTION_SETTLE_MSEC, NULL );
			return qfalse;
		}
	}

	return place->right > place->left && place->bottom > place->top;
}

/*
==============
Caption_Update

Shows the button where it goes, or hides it, and redraws it when its size
or look changed
==============
*/
static void Caption_Update( qboolean redraw )
{
	RECT place;
	int dpi;

	if( !caption.button )
	{
		return;
	}

	if( !Caption_Place( &place ) )
	{
		// a press doesn't outlast it, and it's drawn afresh when it's back:
		// the hover or press it was drawn with, the window's activation and
		// the theme may have changed meanwhile
		caption.hot = caption.down = qfalse;
		if( GetCapture( ) == caption.button )
		{
			ReleaseCapture( );
		}
		SetRectEmpty( &caption.place );
		ShowWindow( caption.button, SW_HIDE );
		return;
	}

	dpi = Caption_Dpi( );
	if( dpi != caption.dpi )
	{
		caption.dpi = dpi;
		Caption_SetTooltipFont( );
		Caption_MakeGlyph( );
		redraw = qtrue;
	}
	if( place.right - place.left != caption.place.right - caption.place.left ||
		place.bottom - place.top != caption.place.bottom - caption.place.top )
	{
		redraw = qtrue;
	}
	// the theme changes only with a message that redraws
	if( redraw )
	{
		caption.dark = Caption_IsDark( );
	}

	if( redraw || !EqualRect( &place, &caption.place ) )
	{
		caption.place = place;
		if( redraw )
		{
			Caption_Draw( );
		}
		else
		{
			SetWindowPos( caption.button, NULL, place.left, place.top, 0, 0,
				SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE );
		}
	}

	if( !IsWindowVisible( caption.button ) )
	{
		ShowWindow( caption.button, SW_SHOWNOACTIVATE );
	}
}

/*
==============
Caption_SetState
==============
*/
static void Caption_SetState( qboolean hot, qboolean down )
{
	if( hot != caption.hot || down != caption.down )
	{
		caption.hot = hot;
		caption.down = down;
		Caption_Draw( );
	}
}

/*
==============
Caption_Over

Whether the pointer is over the button
==============
*/
static qboolean Caption_Over( LPARAM lParam )
{
	POINT point = { (short)LOWORD( lParam ), (short)HIWORD( lParam ) };
	RECT client;

	GetClientRect( caption.button, &client );
	return PtInRect( &client, point );
}

/*
==============
Caption_ButtonProc
==============
*/
static LRESULT CALLBACK Caption_ButtonProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam )
{
	switch( message )
	{
		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE;

		case WM_MOUSEMOVE:
		{
			// pressed, it shows pressed only while the pointer is over it
			qboolean captured = GetCapture( ) == window;
			qboolean over = !captured || Caption_Over( lParam );

			if( !caption.tracking )
			{
				TRACKMOUSEEVENT track = { sizeof( track ), TME_LEAVE, window, 0 };

				caption.tracking = TrackMouseEvent( &track ) ? qtrue : qfalse;
			}
			Caption_SetState( over, captured && over );
			return 0;
		}

		// the tooltip doesn't see the pointer leave, as this tracks it
		case WM_MOUSELEAVE:
			caption.tracking = qfalse;
			SendMessageW( caption.tooltip, TTM_POP, 0, 0 );
			if( GetCapture( ) != window )
			{
				Caption_SetState( qfalse, qfalse );
			}
			return 0;

		// the class has no CS_DBLCLKS: a double click is two presses
		case WM_LBUTTONDOWN:
			// a press activates the window, as on its own title bar
			SetForegroundWindow( caption.owner );
			SendMessageW( caption.tooltip, TTM_POP, 0, 0 );
			SetCapture( window );
			Caption_SetState( qtrue, qtrue );
			return 0;

		case WM_LBUTTONUP:
			if( GetCapture( ) == window )
			{
				qboolean over = Caption_Over( lParam );

				ReleaseCapture( );
				Caption_SetState( over, qfalse );
				if( over )
				{
					caption.pressed( );
				}
			}
			return 0;

		case WM_CAPTURECHANGED:
			if( caption.down )
			{
				Caption_SetState( caption.hot, qfalse );
			}
			return 0;

		// the tooltip's border, as DWM draws the caption buttons' tooltips'
		case WM_NOTIFY:
		{
			NMTTCUSTOMDRAW *draw = (NMTTCUSTOMDRAW *)lParam;

			if( draw->nmcd.hdr.hwndFrom != caption.tooltip || draw->nmcd.hdr.code != NM_CUSTOMDRAW )
			{
				break;
			}
			if( draw->nmcd.dwDrawStage == CDDS_PREPAINT )
			{
				return CDRF_NOTIFYPOSTPAINT;
			}
			if( draw->nmcd.dwDrawStage == CDDS_POSTPAINT )
			{
				RECT client;

				GetClientRect( caption.tooltip, &client );
				SetDCBrushColor( draw->nmcd.hdc, CAPTION_TOOLTIP_BORDER );
				FrameRect( draw->nmcd.hdc, &client, (HBRUSH)GetStockObject( DC_BRUSH ) );
			}
			return CDRF_DODEFAULT;
		}

		// a screen reader's press
		case BM_CLICK:
			SetForegroundWindow( caption.owner );
			caption.pressed( );
			return 0;

		// to screen readers it's a button, named by its text
		case WM_GETOBJECT:
			if( (LONG)lParam == OBJID_CLIENT )
			{
				IAccessible *accessible;

				if( SUCCEEDED( CreateStdAccessibleProxyW( window, L"Button", OBJID_CLIENT,
					&IID_IAccessible, (void **)&accessible ) ) )
				{
					LRESULT result = LresultFromObject( &IID_IAccessible, wParam, (IUnknown *)accessible );

					accessible->lpVtbl->Release( accessible );
					return result;
				}
			}
			break;

		// it's placed by the owner's DPI, not its own
		case WM_DPICHANGED:
			return 0;

		case WM_TIMER:
			if( wParam != CAPTION_SETTLE_TIMER )
			{
				break;
			}
			KillTimer( window, CAPTION_SETTLE_TIMER );
			Caption_Update( qtrue );
			return 0;

		// destroyed with its owner
		case WM_NCDESTROY:
			caption.button = caption.tooltip = NULL;
			break;
	}

	return DefWindowProcW( window, message, wParam, lParam );
}

/*
==============
Caption_OwnerProc

The game's window, subclassed, moves and redraws the button
==============
*/
static LRESULT CALLBACK Caption_OwnerProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR id, DWORD_PTR data )
{
	LRESULT result;

	if( message == WM_NCDESTROY )
	{
		Sys_DestroyFullscreenButton( );
		return DefSubclassProc( window, message, wParam, lParam );
	}

	// A change of scale: the button waits, hidden, until it's through.
	// SDL answers it by resizing the window, which would place the button
	// meanwhile.
	if( message == WM_GETDPISCALEDSIZE || message == WM_DPICHANGED )
	{
		caption.settleUntil = GetTickCount64( ) + CAPTION_SETTLE_LIMIT;
		ShowWindow( caption.button, SW_HIDE );
	}

	// The button moves with the window before SDL hears of the move: SDL
	// may run a frame then, and the two would be apart meanwhile. A change
	// of z-order alone takes it along by itself.
	if( message == WM_WINDOWPOSCHANGED )
	{
		const WINDOWPOS *pos = (const WINDOWPOS *)lParam;

		if( ( pos->flags & ( SWP_NOMOVE | SWP_NOSIZE ) ) != ( SWP_NOMOVE | SWP_NOSIZE ) ||
			( pos->flags & ( SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_FRAMECHANGED ) ) )
		{
			Caption_Update( qfalse );
		}
	}

	result = DefSubclassProc( window, message, wParam, lParam );

	switch( message )
	{
		case WM_STYLECHANGED:
			Caption_Update( qfalse );
			break;

		case WM_NCACTIVATE:
			caption.active = wParam ? qtrue : qfalse;
			Caption_Update( qtrue );
			break;

		// the title bar's theme or scale; SDL has answered the theme's change
		case WM_SETTINGCHANGE:
		case WM_THEMECHANGED:
		case WM_DWMCOMPOSITIONCHANGED:
		case WM_DPICHANGED:
			Caption_Update( qtrue );
			break;
	}

	return result;
}

/*
==============
Sys_CreateFullscreenButton

Puts the button in a window's title bar; pressed() runs on the window's
thread, from its message loop
==============
*/
void Sys_CreateFullscreenButton( void *window, void ( *pressed )( void ) )
{
	HINSTANCE instance = GetModuleHandleW( NULL );
	TTTOOLINFOW tool;
	RECT margin = { 4, 1, 4, 1 };

	if( caption.owner == window )
	{
		caption.pressed = pressed;
		return;
	}
	Sys_DestroyFullscreenButton( );
	if( !window || !pressed )
	{
		return;
	}

	if( !look )
	{
		HMODULE user32 = GetModuleHandleW( L"user32.dll" );
		WNDCLASSW windowClass;
		WNDCLASSEXW tooltipClass;
		INITCOMMONCONTROLSEX controls = { sizeof( controls ), ICC_BAR_CLASSES };

		InitCommonControlsEx( &controls );

		memset( &windowClass, 0, sizeof( windowClass ) );
		windowClass.lpfnWndProc = Caption_ButtonProc;
		windowClass.hInstance = instance;
		windowClass.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
		windowClass.lpszClassName = CAPTION_CLASS;

		// registered by a try that failed after it, the class is still there
		if( !RegisterClassW( &windowClass ) && GetLastError( ) != ERROR_CLASS_ALREADY_EXISTS )
		{
			return;
		}

		// the system's tooltip without its shadow; without it, no tooltip
		memset( &tooltipClass, 0, sizeof( tooltipClass ) );
		tooltipClass.cbSize = sizeof( tooltipClass );
		if( GetClassInfoExW( NULL, TOOLTIPS_CLASSW, &tooltipClass ) )
		{
			tooltipClass.style &= ~CS_DROPSHADOW;
			tooltipClass.hInstance = instance;
			tooltipClass.lpszClassName = CAPTION_TOOLTIP_CLASS;
			RegisterClassExW( &tooltipClass );
		}

		getDpiForWindow = (UINT ( WINAPI * )( HWND ))(void *)GetProcAddress( user32, "GetDpiForWindow" );
		parametersForDpi = (BOOL ( WINAPI * )( UINT, UINT, void *, UINT, UINT ))(void *)GetProcAddress(
			user32, "SystemParametersInfoForDpi" );
		look = Caption_IsWindows11( ) ? &windows11 : &windows10;
	}

	caption.owner = window;
	caption.pressed = pressed;
	caption.active = GetForegroundWindow( ) == caption.owner;

	caption.button = CreateWindowExW( WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		CAPTION_CLASS, CAPTION_TEXT, WS_POPUP, 0, 0, 0, 0, caption.owner, NULL, instance, NULL );
	if( !caption.button || !SetWindowSubclass( caption.owner, Caption_OwnerProc, CAPTION_SUBCLASS_ID, 0 ) )
	{
		Sys_DestroyFullscreenButton( );
		return;
	}

	// the system's tooltip, after the system's delay, drawn as DWM draws
	// the caption buttons' (WM_NOTIFY in Caption_ButtonProc)
	caption.tooltip = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW, CAPTION_TOOLTIP_CLASS, NULL,
		WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		caption.button, NULL, instance, NULL );
	if( caption.tooltip )
	{
		SetWindowTheme( caption.tooltip, L"", L"" );
		SetWindowLongW( caption.tooltip, GWL_STYLE, GetWindowLongW( caption.tooltip, GWL_STYLE ) & ~WS_BORDER );
		SendMessageW( caption.tooltip, TTM_SETMARGIN, 0, (LPARAM)&margin );

		memset( &tool, 0, sizeof( tool ) );
		tool.cbSize = sizeof( tool );
		tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		tool.hwnd = caption.button;
		tool.uId = (UINT_PTR)caption.button;
		tool.lpszText = CAPTION_TEXT;
		SendMessageW( caption.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool );
	}

	Caption_Update( qtrue );
}

/*
==============
Sys_DestroyFullscreenButton
==============
*/
void Sys_DestroyFullscreenButton( void )
{
	if( caption.owner )
	{
		RemoveWindowSubclass( caption.owner, Caption_OwnerProc, CAPTION_SUBCLASS_ID );
	}
	// the tooltip, which it owns, with it
	if( caption.button )
	{
		DestroyWindow( caption.button );
	}
	if( caption.font )
	{
		DeleteObject( caption.font );
	}
	free( caption.glyph );
	memset( &caption, 0, sizeof( caption ) );
}
