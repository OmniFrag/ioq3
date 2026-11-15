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
// sv_banfile.c -- what the ban file keeps of a ban after its subnet, which a
// classic server reads past, and a ban's length as an admin types it

#include "server.h"

#include <ctype.h>

// the latest time a ban file's line may name, past any clock's
#define BAN_TIME_MOST	( (int64_t)1 << 50 )

/*
==================
SV_BanQuote

A ban's text for the file, quoted: a backslash and a quote escaped, and
every control character, a line end among them, written \xHH, so no
name or reason can end the line or the field it's in, or forge another
==================
*/
void SV_BanQuote(const char *text, char *out, int size)
{
	int len = 0;

	if(size < 3)
	{
		*out = '\0';
		return;
	}
	out[len++] = '"';
	for(; *text && len < size - 6; text++)
	{
		unsigned char c = (unsigned char)*text;

		if(c == '\\' || c == '"')
		{
			out[len++] = '\\';
			out[len++] = c;
		}
		else if(c < 0x20 || c == 0x7f)
			len += Com_sprintf(out + len, size - len, "\\x%02x", c);
		else
			out[len++] = c;
	}
	out[len++] = '"';
	out[len] = '\0';
}

/*
==================
SV_BanUnquote

A quoted field as SV_BanQuote wrote it, from *text, which must start at
its quote, into out of size, cut short to fit; *text past its closing
quote, or at the line's end for one with none. \xHH is the byte it
names, but for 00, which is taken as an x, as any other escape is taken
as the character after the backslash
==================
*/
void SV_BanUnquote(const char **text, char *out, int size)
{
	const char *p = *text + 1;
	int len = 0, c;

	while(*p && *p != '"')
	{
		c = (unsigned char)*p++;
		if(c == '\\' && *p)
		{
			// p[2] only past a p[1] that isn't the line's end
			char hex[3] = { p[0] == 'x' ? p[1] : '\0', '\0', '\0' };
			int value;

			hex[1] = hex[0] ? p[2] : '\0';
			if(isxdigit((unsigned char)hex[0]) && isxdigit((unsigned char)hex[1]) && (value = (int)strtol(hex, NULL, 16)))
			{
				c = value;
				p += 3;
			}
			else
				c = (unsigned char)*p++;
		}
		if(len < size - 1)
			out[len++] = c;
	}
	out[len] = '\0';
	*text = *p ? p + 1 : p;
}

/*
==================
SV_ParseBanDetails

What a ban file's line keeps after the subnet, which a classic server
reads past: t=<made> e=<runs out> b="<by>" n="<name>" r="<reason>", each
optional and in any order, a field it doesn't know skipped, quotes and
all. A field starts only a word, so nothing inside another field's value
reads as one. A ban with a time it was made or a maker is recorded
==================
*/
void SV_ParseBanDetails(const char *text, extBanDetails_t *ban)
{
	while(*text)
	{
		char key = text[0], skipped[2];

		if(key == ' ')
		{
			text++;
			continue;
		}
		if(text[1] != '=')
		{
			// not a field: the word skipped
			for(; *text && *text != ' '; text++)
				;
			continue;
		}
		text += 2;
		if(key == 't' || key == 'e')
		{
			// a time, held to BAN_TIME_MOST, the digits past it skipped
			int64_t value = 0;
			qboolean full = qfalse;

			for(; *text >= '0' && *text <= '9'; text++)
			{
				full |= value > (BAN_TIME_MOST - (*text - '0')) / 10;
				if(!full)
					value = value * 10 + (*text - '0');
			}
			if(key == 't')
			{
				ban->at = value;
				ban->recorded |= value > 0;
			}
			else
				ban->expires = value;
			// the rest of the word
			for(; *text && *text != ' '; text++)
				;
		}
		else if(*text == '"')
		{
			if(key == 'b')
			{
				SV_BanUnquote(&text, ban->by, sizeof(ban->by));
				ban->recorded |= ban->by[0] != '\0';
			}
			else if(key == 'n')
				SV_BanUnquote(&text, ban->name, sizeof(ban->name));
			else if(key == 'r')
				SV_BanUnquote(&text, ban->reason, sizeof(ban->reason));
			// a field it doesn't know: its quotes, escapes and all
			else
				SV_BanUnquote(&text, skipped, sizeof(skipped));
		}
		// a field it doesn't know, or one unquoted: up to the next space
		else
		{
			for(; *text && *text != ' '; text++)
				;
		}
	}
}

/*
==================
SV_BanDuration

A ban's length, typed: a whole number of seconds, minutes, hours, days or
weeks, "90m", "1h", "7d", "2w", up to SERVER_BAN_LONGEST; 0 for anything
else, which isn't one
==================
*/
int SV_BanDuration(const char *text)
{
	static const struct { char unit; int seconds; } units[] = {
		{ 's', 1 }, { 'm', 60 }, { 'h', 60 * 60 }, { 'd', 24 * 60 * 60 }, { 'w', 7 * 24 * 60 * 60 }
	};
	int len = strlen(text), digits = strspn(text, "0123456789"), i;
	int64_t count;

	if(digits < 1 || digits > 9 || len != digits + 1)
		return 0;
	count = atoi(text);
	for(i = 0; i < ARRAY_LEN(units); i++)
	{
		if(text[digits] == units[i].unit)
			return count * units[i].seconds > SERVER_BAN_LONGEST ? 0 : (int)(count * units[i].seconds);
	}
	return 0;
}

