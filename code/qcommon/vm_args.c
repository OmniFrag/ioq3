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
// vm_args.c -- checked syscall arguments
//
// A syscall handler decodes each argument through a macro that says what it
// is (VMA_* for pointers, VMV_* for numbers, in qcommon.h), and these check
// it against the module that made the call (currentVM). A bad argument drops
// the module with an error that names the syscall and the argument.
//
// A QVM's pointers must lie inside its memory, start and end. Linked and dll
// modules (entryPoint set) have no bounds to check pointers against, so only
// the rest applies to them: NULL, lengths, counts and ranges.

#include "vm_local.h"

/*
============
VM_SetSyscallNames

Names the module's syscalls, by number, for VM_ArgError's messages
============
*/
void VM_SetSyscallNames( vm_t *vm, const char * const *names, int numNames ) {
	vm->syscallNames = names;
	vm->numSyscallNames = numNames;
}

/*
============
VM_ArgError

Drops the current module for a bad argument n of the syscall in args[0]
============
*/
void QDECL VM_ArgError( const intptr_t *args, int n, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[256];
	char		number[16];
	const char	*name;
	intptr_t	call = args[0];

	va_start( argptr, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );

	if ( call >= 0 && call < currentVM->numSyscallNames && currentVM->syscallNames[call] ) {
		name = currentVM->syscallNames[call];
	} else {
		Com_sprintf( number, sizeof( number ), "%i", (int)call );
		name = number;
	}
	Com_Error( ERR_DROP, "%s: syscall %s argument %i: %s", currentVM->name, name, n, msg );
}

/*
============
VM_MemoryLeft

The bytes of a QVM's memory from offset ptr to its end, or -1 when ptr lies
outside it
============
*/
static int64_t VM_MemoryLeft( const vm_t *vm, intptr_t ptr ) {
	if ( ptr < 0 || ptr > vm->dataMask ) {
		return -1;
	}
	return (int64_t)vm->dataMask + 1 - ptr;
}

/*
============
VM_StringInside

Whether a QVM's string at ptr is terminated inside its memory
============
*/
static qboolean VM_StringInside( const vm_t *vm, intptr_t ptr ) {
	int64_t	left = VM_MemoryLeft( vm, ptr );

	return left > 0 && memchr( vm->dataBase + ptr, 0, left ) != NULL;
}

/*
============
VM_ArgBuf

A pointer to size bytes of module memory, which may be NULL if optional
============
*/
void *VM_ArgBuf( const intptr_t *args, int n, int64_t size, qboolean optional ) {
	intptr_t	ptr = args[n];

	if ( !ptr ) {
		if ( optional ) {
			return NULL;
		}
		VM_ArgError( args, n, "is NULL" );
	}
	if ( size < 0 ) {
		VM_ArgError( args, n, "has a negative length (%lld)", (long long)size );
	}
	if ( currentVM->entryPoint ) {
		return (void *)( currentVM->dataBase + ptr );
	}
	if ( size > VM_MemoryLeft( currentVM, ptr ) ) {
		VM_ArgError( args, n, "%lld bytes at %lld run past the module's memory (%i bytes)",
			(long long)size, (long long)ptr, currentVM->dataMask + 1 );
	}
	return currentVM->dataBase + ptr;
}

/*
============
VM_ArgStrBuf

A buffer of size bytes that receives a terminated string, so it needs room
for the terminator
============
*/
char *VM_ArgStrBuf( const intptr_t *args, int n, int64_t size ) {
	if ( size < 1 ) {
		VM_ArgError( args, n, "has no room for a string (length %lld)", (long long)size );
	}
	return VM_ArgBuf( args, n, size, qfalse );
}

/*
============
VM_ArgArray

count1 * count2 elements of elemSize bytes each. The product is computed in
64 bits and capped, so it can't wrap.
============
*/
void *VM_ArgArray( const intptr_t *args, int n, size_t elemSize, int count1, int count2 ) {
	uint64_t	count;

	if ( count1 < 0 || count2 < 0 ) {
		VM_ArgError( args, n, "has a negative count (%i, %i)", count1, count2 );
	}
	count = (uint64_t)count1 * (uint64_t)count2;
	if ( count > INT_MAX ) {
		VM_ArgError( args, n, "has too many elements (%i times %i)", count1, count2 );
	}
	return VM_ArgBuf( args, n, (int64_t)( count * elemSize ), qfalse );
}

/*
============
VM_ArgStr

A string terminated inside the module's memory, which may be NULL if
optional
============
*/
const char *VM_ArgStr( const intptr_t *args, int n, qboolean optional ) {
	intptr_t	ptr = args[n];

	if ( !ptr ) {
		if ( optional ) {
			return NULL;
		}
		VM_ArgError( args, n, "is NULL" );
	}
	if ( currentVM->entryPoint ) {
		return (const char *)( currentVM->dataBase + ptr );
	}
	if ( !VM_StringInside( currentVM, ptr ) ) {
		VM_ArgError( args, n, "string at %lld isn't terminated inside the module's memory (%i bytes)",
			(long long)ptr, currentVM->dataMask + 1 );
	}
	return (const char *)( currentVM->dataBase + ptr );
}

/*
============
VM_ArgFile

A file handle the module opened itself (FS_VM_FOpenFile), or 0, which some
modules close without having opened
============
*/
int VM_ArgFile( const intptr_t *args, int n ) {
	int		f = (int)args[n];

	if ( f && !FS_HandleOwnedBy( f, currentVM ) ) {
		VM_ArgError( args, n, "file handle %i isn't open, or isn't the module's", f );
	}
	return f;
}

/*
============
VM_ExplicitArgStr

A string vm returned from a VM_Call, such as GAME_CLIENT_CONNECT's reason
for refusing a client: NULL for 0, otherwise terminated inside the module's
memory, or the module is dropped
============
*/
const char *VM_ExplicitArgStr( vm_t *vm, intptr_t value, const char *what ) {
	if ( !value ) {
		return NULL;
	}
	if ( vm->entryPoint ) {
		return (const char *)( vm->dataBase + value );
	}
	if ( !VM_StringInside( vm, value ) ) {
		Com_Error( ERR_DROP, "%s: %s isn't a string inside the module's memory", vm->name, what );
	}
	return (const char *)( vm->dataBase + value );
}

/*
============
VM_PrivateCvarFlag

The cvars the current module may not read: CVAR_PRIVATE ones, unless it
is native code (a dll or a linked module), which shares the engine's
process and could read them anyway
============
*/
int VM_PrivateCvarFlag( void ) {
	return currentVM && currentVM->entryPoint ? 0 : CVAR_PRIVATE;
}

/*
============
VM_ArgInt

A number from lo to hi
============
*/
int VM_ArgInt( const intptr_t *args, int n, int lo, int hi ) {
	int		value = (int)args[n];

	if ( value < lo || value > hi ) {
		VM_ArgError( args, n, "%i is outside %i to %i", value, lo, hi );
	}
	return value;
}

/*
============
VM_Strncpy

strncpy( dest, src, count ) for a module (the TRAP_STRNCPY syscalls):
dest's count bytes and the source bytes it reads must lie in module memory,
the two may overlap, and dest is zero-filled past the copy, as strncpy does.
Returns dest, as the module gave it.
============
*/
intptr_t VM_Strncpy( const intptr_t *args ) {
	int64_t		count = (int)args[3];
	char		*dest = VM_ArgBuf( args, 1, count, qfalse );
	const char	*src;
	const char	*end;
	size_t		len;

	if ( currentVM->entryPoint ) {
		src = VM_ArgStr( args, 2, qfalse );
		for ( len = 0; len < (size_t)count && src[len]; len++ ) {
		}
	} else {
		// the source needn't be terminated within count bytes, but what's
		// read of it must be in memory
		int64_t	left = VM_MemoryLeft( currentVM, args[2] );

		if ( !args[2] ) {
			VM_ArgError( args, 2, "is NULL" );
		}
		if ( left < 0 ) {
			VM_ArgError( args, 2, "string at %lld lies outside the module's memory", (long long)args[2] );
		}
		src = (const char *)currentVM->dataBase + args[2];
		len = MIN( left, count );
		end = memchr( src, 0, len );
		if ( end ) {
			len = end - src;
		} else if ( (int64_t)len < count ) {
			VM_ArgError( args, 2, "string runs past the module's memory" );
		}
	}

	memmove( dest, src, len );
	memset( dest + len, 0, count - len );
	return args[1];
}

/*
============
VM_FOpenFile

trap_FS_FOpenFile( const char *qpath, fileHandle_t *f, fsMode_t mode ) for
any module: reading may pass no handle, to ask only for the length; writing
needs one
============
*/
intptr_t VM_FOpenFile( const intptr_t *args ) {
	int				mode = (int)args[3];
	const char		*qpath = VMA_STR( 1 );
	fileHandle_t	*f = mode == FS_READ ? VMA_OUT_OPT( 2, fileHandle_t ) : VMA_OUT( 2, fileHandle_t );

	return FS_VM_FOpenFile( currentVM, qpath, f, mode );
}
